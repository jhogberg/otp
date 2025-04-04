/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2025. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */

#ifndef ERTS_BIF_PERSISTENT_H__
#define ERTS_BIF_PERSISTENT_H__

#include "sys.h"
#include "global.h"

#include "erl_term_hashing.h"

#define ERTS_CTRIE_PREFIX pt_ctrie
#define ERTS_CTRIE_KEY_TYPE Eterm
#define ERTS_CTRIE_HASH_TYPE erts_ihash_t
#define ERTS_CTRIE_BRANCH_ALLOC_TYPE ERTS_ALC_T_PERSISTENT_TERM
#define ERTS_CTRIE_ITERATOR_ALLOC_TYPE ERTS_ALC_T_PERSISTENT_TERM
#define ERTS_CTRIE_NODE_ALLOC_TYPE ERTS_ALC_T_PERSISTENT_TERM

#ifdef ARCH_64
#    define ERTS_CTRIE_BRANCH_FACTOR 6
#else
#    define ERTS_CTRIE_BRANCH_FACTOR 5
#endif

#define ERTS_CTRIE_WANT_CLEAR
#define ERTS_CTRIE_WANT_CRASH_DUMP
#define ERTS_CTRIE_WANT_INSERT
#define ERTS_CTRIE_WANT_ITERATORS
#define ERTS_CTRIE_WANT_KEEP
#define ERTS_CTRIE_WANT_LOOKUP
#define ERTS_CTRIE_WANT_ERASE
#define ERTS_CTRIE_WANT_REPLACE
#define ERTS_CTRIE_INCLUDE_TYPES

#define ERTS_CTRIE_UNDEF
#include "erl_ctrie.h"

typedef struct {
    pt_ctrie_SingletonNode base;

    erts_ihash_t hash;
    Eterm key, value;

    ErtsLiteralArea *area;

    /* Used for inline caching; `pt_sequence` is bumped for every mutating
     * operation, producing a globally unique number, and we store that number
     * here on creation and *before* publishing updated nodes (or removing them
     * in case of erase).
     *
     * When performing a cached lookup, we read this with a ddrb and compare
     * it to the separately stored sequence number. If they do not match, we
     * look it up again and update it. Either can be updated or read separately
     * as no two nodes share the same sequence number. */
    erts_atomic64_t sequence;
} PersistentTermNode;

/** @brief Updates the given inline cache. Note that it takes ownership of the
 * \c out node which must be freed with \c erts_persistent_term_release_cache
 *
 * @return \c CTRIE_OK on success, \c CTRIE_RESTART to yield, and
 * \c CTRIE_NOT_FOUND when we need to clear the cache (and optionally
 * throw). */
enum erts_ctrie_result erts_persistent_term_update_cache(
        Process *c_p,
        Eterm key,
        erts_aint_t *cookie,
        PersistentTermNode **out);
void erts_persistent_term_release_cache(PersistentTermNode *node);

/* FIXME: Next idea for caching; keep a separate ctrie with cache entries,
 * which are in turn referenced by all persistent term nodes, as well as the
 * static caches in code.
 *
 * How does this help?
 *
 * It lets us have a (separate) global hash table of all cache entries, which
 * does not churn with value updates, only additions and erasures, which may be
 * lazily updated as we will hit the main trie anyway on a cache miss.
 *
 * Cache entries will be added to the cache-trie either by code, or by presence
 * in the main trie. If we assume that erasures are uncommon, this should not
 * be too problematic.
 *
 * This global hash table will not own the cache entries, and any
 * additions/erasures will not be immediately applied, but will be handled
 * lazily to amortize the cost of rehashing.
 *
 * Open issues:
 *
 * 1. What can we do about key storage? The cache needs to have keys (duh). To
 *    avoid storing keys twice, can we say that the key storage for persistent
 *    terms is owned by the cache?
 * 2. The cache necessarily implies a read barrier with an extra indirection,
 *    which is not the best for performance. However, the old implementation
 *    sort of had that already with the key-value pairs being tuples.
 * 3. At the end of the day we need an equality check, and we cannot be too
 *    clever with it. It would be nice if we could somehow replace the given
 *    key with the canonical cached literal on a match, but as complex keys
 *    are usually created like:
 *
 *        persistent_term:get({?MODULE, SomethingDynamic})
 *
 *    ... there's extremely little we can do.
 *
 * Observations:
 *
 * 1. The `maint-27` implementation already does direct overwrites in the
 *    global table, so barriers are probably redundant (the implied ddrb on
 *    non-Alpha systems is enough).
 *
 *    This means that the hash table could be an open table of
 *    PersistentTermStaticCache entries, rather than pointing at them,
 *    effectively bringing the characteristics back to where we were.
 *
 * In summary:
 *
 * Let us have two ctries: that for nodes (supporting snapshots etc), that for
 * cache entries (insert/lookup/erase). The nodes own the cache entries, and in
 * turn the key data for the nodes are (canonically) owned by the cache
 * entries to reduce redundant storage.
 *
 * Let us have a open hash table of PersistentTermStaticCache entries. These
 * reference but do not own the cache entries.
 *
 * When the reference count of a PersistentTermStaticCache hits zero, it is
 * immediately removed from the cache trie, and an asynchronous rebuild of the
 * hash table is scheduled (amortized with others). Once it no longer exists in
 * the table, it is finally freed.
 *
 * ... or we redesign the API. Namespacing is quite likely to help once the
 * number of (global) elements exceed a certain figure, and further improves
 * update speed.
 *
 * Let's do some quick tests:
 *
 * CTrie, no dynamic cache:
 *     1> bench:bench(1000000, 2).
 *     {32539,2642,376327}
 *     2> bench:bench(1000000, 1000000).
 *     {49228,2619,463663}
 *     3> bench:bench(1000000, 10000000).
 *     Command is taking a long time, type Ctrl+G, then enter 'i' to interrupt
 *     {51809,2647,1096153}
 *
 *     Things become slightly slower as the number of elements grows, but
 *     perhaps not too horrible.
 *
 * CTrie, dynamic cache:
 *
 *     1> bench:bench(1000000, 2).
 *     {21850,2621,371745}
 *     2> bench:bench(1000000, 1000000).
 *     {21659,2650,541118}
 *     3> bench:bench(1000000, 10000000).
 *     Command is taking a long time, type Ctrl+G, then enter 'i' to interrupt
 *     {22562,2646,819057}
 *
 * ... Number of entries doesn't seem to matter at all for lookup speed when
 *     the dynamic cache is in place. This makes sense since we've effectively
 *     only got two entries in the relevant cache.
 *
 *     Much of the speed difference relative to the non-caching version appears
 *     to be due to snapshot support. If we alter the benchmark to have dynamic
 *     lookup of 1000000 elements rather than "modulo 2"
 *
 * CTrie, no dynamic cache, no yielding:
 *
 *     1> bench:bench(1000000, 2).
 *     {25166,2639,374222}
 *     2> bench:bench(1000000, 2).
 *     {26786,2647,375931}
 *     3> bench:bench(1000000, 2).
 *     {25740,2629,372958}
 *
 * ... This is also "better."
 *
 * Open hash table:
 *
 *     1> bench:bench(1000000, 2).
 *     Command is taking a long time, type Ctrl+G, then enter 'i' to interrupt
 *     {15340,34379,5207637}
 *     2> bench:bench(1000000, 1000000).
 *     Command is taking a long time, type Ctrl+G, then enter 'i' to interrupt
 *     {12527,11594,5187501}
 *     3> bench:bench(1000000, 10000000).
 *     Command is taking a long time, type Ctrl+G, then enter 'i' to interrupt
 *     {12665,11383,5153098}
 *
 * ... Nor does it here.
 *
 * Hence, namespacing is fairly unlikely to help with a cache in place, but
 * would probably help a lot if we add an (open hash) cache-table in front as
 * described above, as it would make rebuilds cheaper by virtue of only
 * affecting nodes related to the added/removed node. */

typedef struct {
    erts_atomic64_t cookie;
    erts_atomic_t node;
} PersistentTermStaticCache;

enum erts_ctrie_result erts_persistent_term_update_static_cache(
        PersistentTermStaticCache *cache,
        Eterm key,
        Eterm *value);
void erts_persistent_term_init_static_cache(PersistentTermStaticCache *cache);

typedef struct {
    erts_atomic_t table;
} PersistentTermDynamicCache;

typedef struct {
    ErtsThrPrgrLaterOp later_op;

    /* MUST be a power of two. */
    size_t size;
    PersistentTermStaticCache entries[];
} PersistentTermDynamicCacheTable;

enum erts_ctrie_result erts_persistent_term_lookup_dynamic_cache(
        PersistentTermDynamicCache *cache,
        Eterm key,
        Eterm *value);
void erts_persistent_term_init_dynamic_cache(PersistentTermDynamicCache *cache);

Eterm erts_persistent_term_lookup_fast(Eterm key);

#endif
