/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2018-2025. All Rights Reserved.
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

/*
 * Purpose: Implement persistent term storage.
 */

#ifdef HAVE_CONFIG_H
#    include "config.h"
#endif

#include "sys.h"
#include "erl_vm.h"
#include "global.h"
#include "erl_process.h"
#include "error.h"
#include "erl_driver.h"
#include "bif.h"
#include "erl_map.h"
#include "erl_binary.h"

#include "erl_bif_persistent.h"

static void persistent_term_destroy_node(void *node);

#define ERTS_CTRIE_PREFIX pt_ctrie
#define ERTS_CTRIE_KEY_TYPE Eterm
#define ERTS_CTRIE_HASH_TYPE erts_ihash_t
#define ERTS_CTRIE_BRANCH_ALLOC_TYPE ERTS_ALC_T_PERSISTENT_TERM
#define ERTS_CTRIE_ITERATOR_ALLOC_TYPE ERTS_ALC_T_PERSISTENT_TERM
#define ERTS_CTRIE_NODE_ALLOC_TYPE ERTS_ALC_T_PERSISTENT_TERM

#define ERTS_CTRIE_KEY_GET(Singleton)                                          \
    (((const PersistentTermNode *)(Singleton))->key)
#define ERTS_CTRIE_HASH_GET(Singleton)                                         \
    (((const PersistentTermNode *)(Singleton))->hash)
#define ERTS_CTRIE_SINGLETON_DESTRUCTOR(Singleton)                             \
    persistent_term_destroy_node((void *)(Singleton))
#define ERTS_CTRIE_KEY_EQ(LHS, RHS) eq((LHS), (RHS))
#define ERTS_CTRIE_HASH_EQ(LHS, RHS) ((LHS) == (RHS))

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

#define ERTS_CTRIE_INCLUDE_IMPLEMENTATION
#define ERTS_CTRIE_UNDEF
#include "erl_ctrie.h"

static pt_ctrie_Trie persistent_terms;
erts_atomic64_t pt_sequence;

typedef struct {
    pt_ctrie_Trie trie;
    erts_atomic64_t sequence;
} PersistentTermNamespace;

PersistentTermNamespace pt_global;

static BIF_RETTYPE persistent_term_get_all_trap(BIF_ALIST_1);
static BIF_RETTYPE persistent_term_get_default_trap(BIF_ALIST_2);
static BIF_RETTYPE persistent_term_get_trap(BIF_ALIST_1);
static BIF_RETTYPE persistent_term_info_trap(BIF_ALIST_1);
static BIF_RETTYPE persistent_term_put_trap(BIF_ALIST_1);
static BIF_RETTYPE persistent_term_clear_trap(BIF_ALIST_1);

static Export persistent_term_erase_export;
static Export persistent_term_get_all_export;
static Export persistent_term_get_default_export;
static Export persistent_term_get_export;
static Export persistent_term_info_export;
static Export persistent_term_put_export;
static Export persistent_term_clear_export;

/* Helper routine for use when the number of persistent terms change,
 * maintaining a memory area used for crash dumping as we cannot allocate
 * memory at that point. */
static void persistent_term_update_count(erts_aint_t diff);

/* Used for figuring out which literal area a term belongs to during crash
 * dumping.
 *
 * For performance reasons, this is a flat array that's allocated ahead of
 * time, irreversibly growing as the peak number of persistent terms
 * increases, regardless of whether the terms have literal areas or not.
 *
 * It is assumed that the number of terms will be reasonably small, so that
 * this overallocation doesn't cost too much. */
ErtsLiteralArea **erts_persistent_areas;
Uint erts_num_persistent_areas;
static erts_atomic_t pt_cd_current_count;
static erts_atomic_t pt_cd_watermark;
static erts_mtx_t pt_cd_lock;
static Uint pt_cd_allocated;

void erts_init_bif_persistent_term(void) {
    static const Uint INITIAL_WATERMARK = 16;

    pt_ctrie_init(&persistent_terms);
    erts_atomic_init_nob(&pt_sequence, 1);

    erts_persistent_areas =
            erts_alloc(ERTS_ALC_T_CRASH_DUMP,
                       sizeof(ErtsLiteralArea *) * INITIAL_WATERMARK);
    erts_atomic_init_nob(&pt_cd_watermark, INITIAL_WATERMARK);
    erts_atomic_init_nob(&pt_cd_current_count, 0);

    erts_mtx_init(&pt_cd_lock,
                  "persistent_term_areas_lock",
                  NIL,
                  ERTS_LOCK_FLAGS_PROPERTY_STATIC |
                          ERTS_LOCK_FLAGS_CATEGORY_GENERIC);

    erts_init_trap_export(&persistent_term_get_all_export,
                          am_persistent_term,
                          am_get,
                          1,
                          &persistent_term_get_all_trap);
    erts_init_trap_export(&persistent_term_get_default_export,
                          am_persistent_term,
                          am_get,
                          2,
                          &persistent_term_get_default_trap);
    erts_init_trap_export(&persistent_term_get_export,
                          am_persistent_term,
                          am_get,
                          1,
                          &persistent_term_get_trap);
    erts_init_trap_export(&persistent_term_erase_export,
                          am_persistent_term,
                          am_erase,
                          1,
                          &persistent_term_erase_1);
    erts_init_trap_export(&persistent_term_info_export,
                          am_persistent_term,
                          am_info,
                          1,
                          &persistent_term_info_trap);
    erts_init_trap_export(&persistent_term_put_export,
                          am_persistent_term,
                          am_put,
                          1,
                          &persistent_term_put_trap);
    erts_init_trap_export(&persistent_term_clear_export,
                          am_erts_internal,
                          am_erase_persistent_terms,
                          1,
                          &persistent_term_clear_trap);
}

static void persistent_term_destroy_node(void *node_) {
    PersistentTermNode *node = (PersistentTermNode *)node_;

    if (is_not_immed(node->value)) {
        /* Value may have been observed, schedule a literal GC. */
        erts_queue_release_literals(NULL, node->area);
    } else if (is_not_immed(node->key)) {
        /* Keys are not observable (always copied in get/0), and the value is
         * an immediate, so we do not have to schedule a literal GC. */
        erts_free(ERTS_ALC_T_LITERAL, node->area);
    }

    erts_free(ERTS_ALC_T_PERSISTENT_TERM, node);
}

static PersistentTermNode *create_node(Eterm key,
                                       Eterm value,
                                       erts_ihash_t hash,
                                       erts_aint_t sequence) {
    PersistentTermNode *node =
            erts_alloc(ERTS_ALC_T_PERSISTENT_TERM, sizeof(PersistentTermNode));

    pt_ctrie_singleton_init(&node->base);

    erts_atomic_init_nob(&node->sequence, sequence);
    node->hash = hash;

    if (is_both_immed(key, value)) {
        node->key = key;
        node->value = value;

#ifdef DEBUG
        node->area = NULL;
#endif
    } else {
        /* Preserve internal sharing in the terms by using the sharing-
         * preserving functions. Literals must be copied in case the module
         * holding them are unloaded. */
        erts_shcopy_t key_info, value_info;
        Uint key_size, value_size, term_size;
        ErtsLiteralArea *area;

        INITIALIZE_SHCOPY(key_info);
        INITIALIZE_SHCOPY(value_info);

        key_info.copy_literals = 1;
        value_info.copy_literals = 1;

        key_size = copy_shared_calculate(key, &key_info);
        value_size = copy_shared_calculate(value, &value_info);

        term_size = key_size + value_size;
        area = erts_alloc(ERTS_ALC_T_LITERAL,
                          ERTS_LITERAL_AREA_ALLOC_SIZE(term_size));
        node->area = area;

        {
            ErlOffHeap off_heap;
            Eterm *ptr;

            ptr = &area->start[0];
            area->end = &ptr[term_size];

            ERTS_INIT_OFF_HEAP(&off_heap);

            node->key = copy_shared_perform(key,
                                            key_size,
                                            &key_info,
                                            &ptr,
                                            &off_heap);
            node->value = copy_shared_perform(value,
                                              value_size,
                                              &value_info,
                                              &ptr,
                                              &off_heap);
            area->off_heap = off_heap.first;
        }

        DESTROY_SHCOPY(value_info);
        DESTROY_SHCOPY(key_info);

        erts_set_literal_tag(&node->key, area->start, term_size);
        erts_set_literal_tag(&node->value, area->start, term_size);
    }

    return node;
}

/* */

Eterm erts_persistent_term_get(Eterm key) {
    enum erts_ctrie_result result;
    PersistentTermNode *node;
    erts_ihash_t hash;

    hash = erts_internal_hash(key);

    do {
        result = pt_ctrie_lookup(&persistent_terms,
                                 key,
                                 hash,
                                 (pt_ctrie_SingletonNode **)&node);
    } while (result == CTRIE_RESTART);

    if (result == CTRIE_OK) {
        return node->value;
    }

    return THE_NON_VALUE;
}

static enum erts_ctrie_result persistent_term_get(
        Process *c_p,
        Eterm key,
        pt_ctrie_SingletonNode **out) {
    int budget = ERTS_BIF_REDS_LEFT(c_p), spent = 0;
    enum erts_ctrie_result result = CTRIE_RESTART;
    erts_ihash_t hash = erts_internal_hash(key);

    while (spent < budget && result == CTRIE_RESTART) {
        result = pt_ctrie_lookup(&persistent_terms, key, hash, out);
        spent++;
    }

    BUMP_REDS(c_p, spent);

    return result;
}

Eterm erts_persistent_term_lookup_fast(Eterm key) {
    erts_ihash_t hash = erts_internal_hash(key);
    enum erts_ctrie_result result;
    PersistentTermNode *node;

    do {
        result = pt_ctrie_lookup(&persistent_terms,
                                 key,
                                 hash,
                                 (pt_ctrie_SingletonNode **)&node);
    } while (result == CTRIE_RESTART);

    if (result == CTRIE_OK) {
        return node->value;
    }

    return THE_NON_VALUE;
}

static BIF_RETTYPE persistent_term_get_trap(BIF_ALIST_1) {
    return persistent_term_get_1(BIF_P, BIF__ARGS, BIF_I);
}

BIF_RETTYPE persistent_term_get_1(BIF_ALIST_1) {
    enum erts_ctrie_result result;
    PersistentTermNode *node;

    result = persistent_term_get(BIF_P,
                                 BIF_ARG_1,
                                 (pt_ctrie_SingletonNode **)&node);

    if (result == CTRIE_OK) {
        BIF_RET(node->value);
    } else if (result == CTRIE_RESTART) {
        BIF_TRAP1(&persistent_term_get_export, BIF_P, BIF_ARG_1);
    }

    BIF_ERROR(BIF_P, BADARG);
}

static BIF_RETTYPE persistent_term_get_default_trap(BIF_ALIST_1) {
    return persistent_term_get_2(BIF_P, BIF__ARGS, BIF_I);
}

BIF_RETTYPE persistent_term_get_2(BIF_ALIST_2) {
    enum erts_ctrie_result result;
    PersistentTermNode *node;

    result = persistent_term_get(BIF_P,
                                 BIF_ARG_1,
                                 (pt_ctrie_SingletonNode **)&node);

    if (result == CTRIE_OK) {
        BIF_RET(node->value);
    } else if (result == CTRIE_RESTART) {
        BIF_TRAP2(&persistent_term_get_default_export,
                  BIF_P,
                  BIF_ARG_1,
                  BIF_ARG_2);
    }

    BIF_RET(BIF_ARG_2);
}

/* */

typedef struct {
    pt_ctrie_Iterator iterator;

    Eterm pairs;
    Eterm *pairs_tail;
} GetAllContext;

static int persistent_term_get_all_context_dtor(Binary *context_bin) {
    GetAllContext *ctx = ERTS_MAGIC_BIN_DATA(context_bin);
    pt_ctrie_iterate_finish(&ctx->iterator);
    return 1;
}

static BIF_RETTYPE persistent_term_get_all_trap(BIF_ALIST_1) {
    Binary *magic_binary = erts_magic_ref2bin(BIF_ARG_1);
    GetAllContext *ctx = ERTS_MAGIC_BIN_DATA(magic_binary);
    int budget = ERTS_BIF_REDS_LEFT(BIF_P), spent = 0;
    PersistentTermNode *node;

    while (spent < budget) {
        spent++;

        if (pt_ctrie_iterate_next(&ctx->iterator,
                                  (pt_ctrie_SingletonNode **)&node)) {
            Eterm *cell, *hp;
            Uint key_size;
            Eterm key;

            /* Unlike values (literals) which can be used as-is, keys live in
             * their nodes and must be copied over to the process heap. */
            key_size = size_object(node->key);
            hp = HAlloc(BIF_P, 5 + key_size);
            key = copy_struct(node->key, key_size, &hp, &MSO(BIF_P));

            cell = hp;
            hp += 2;

            CAR(cell) = TUPLE2(hp, key, node->value);
            CDR(cell) = NIL;
            *ctx->pairs_tail = make_list(cell);
            ctx->pairs_tail = &CDR(cell);

            continue;
        }

        /* Iteration context will be finished in the destructor. */
        erts_set_gc_state(BIF_P, 1);
        BIF_RET(ctx->pairs);
    }

    BUMP_REDS(BIF_P, spent);
    BIF_TRAP1(&persistent_term_get_all_export, BIF_P, BIF_ARG_1);
}

BIF_RETTYPE persistent_term_get_0(BIF_ALIST_0) {
    GetAllContext *state;
    Eterm state_mref;
    Binary *state_bin;
    Eterm *hp;

    state_bin = erts_create_magic_binary(sizeof(GetAllContext),
                                         persistent_term_get_all_context_dtor);
    hp = HAlloc(BIF_P, ERTS_MAGIC_REF_THING_SIZE);

    state_mref = erts_mk_magic_ref(&hp, &MSO(BIF_P), state_bin);
    state = ERTS_MAGIC_BIN_DATA(state_bin);

    state->pairs_tail = &state->pairs;
    state->pairs = NIL;

    pt_ctrie_iterate(&persistent_terms, &state->iterator);
    erts_set_gc_state(BIF_P, 0);

    BIF__ARGS[0] = state_mref;
    BIF_RET(persistent_term_get_all_trap(BIF_P, BIF__ARGS, BIF_I));
}

/* */

static enum erts_ctrie_result persistent_term_erase(Process *c_p, Eterm key) {
    int budget = ERTS_BIF_REDS_LEFT(c_p), spent = 0;
    enum erts_ctrie_result result = CTRIE_RESTART;
    erts_ihash_t hash = erts_internal_hash(key);
    PersistentTermNode *node;

    while (spent < budget && result == CTRIE_RESTART) {
        result = pt_ctrie_lookup(&persistent_terms,
                                 key,
                                 hash,
                                 (pt_ctrie_SingletonNode **)&node);
    }

    if (result == CTRIE_OK) {
        erts_atomic_set_nob(&node->sequence,
                            erts_atomic_inc_read_nob(&pt_sequence));

        ERTS_THR_WRITE_MEMORY_BARRIER;

        while (spent < budget && result == CTRIE_RESTART) {
            result = pt_ctrie_erase(&persistent_terms, &node->base);
            spent++;
        }

        if (result == CTRIE_OK) {
            persistent_term_update_count(-1);
        }
    }

    return result;
}

BIF_RETTYPE persistent_term_erase_1(BIF_ALIST_1) {
    enum erts_ctrie_result result = persistent_term_erase(BIF_P, BIF_ARG_1);

    if (result == CTRIE_OK) {
        BIF_RET(am_true);
    } else if (result == CTRIE_RESTART) {
        BIF_TRAP1(&persistent_term_erase_export, BIF_P, BIF_ARG_1);
    }

    ASSERT(result == CTRIE_NOT_FOUND);
    BIF_RET(am_false);
}

/* */

typedef struct {
    pt_ctrie_Iterator iterator;

    Uint count;
    Uint memory;
} InfoContext;

static int persistent_term_info_context_dtor(Binary *context_bin) {
    InfoContext *ctx = ERTS_MAGIC_BIN_DATA(context_bin);

    pt_ctrie_iterate_finish(&ctx->iterator);

    return 1;
}

static BIF_RETTYPE persistent_term_info_trap(BIF_ALIST_1) {
    Binary *magic_binary = erts_magic_ref2bin(BIF_ARG_1);
    InfoContext *ctx = ERTS_MAGIC_BIN_DATA(magic_binary);
    int budget = ERTS_BIF_REDS_LEFT(BIF_P), spent = 0;
    PersistentTermNode *node;

    while (spent < budget) {
        spent++;

        if (pt_ctrie_iterate_next(&ctx->iterator,
                                  (pt_ctrie_SingletonNode **)&node)) {
            Uint term_size = size_object(node->key) + size_object(node->value);

            ctx->memory += sizeof(*node) + term_size * sizeof(Eterm);
            ctx->count++;

            spent++;
            continue;
        }

        /* Iteration context will be finished in the destructor. */
        /* FIXME: Build the result map. */
        BIF_RET(NIL);
    }

    BUMP_REDS(BIF_P, spent);
    BIF_TRAP1(&persistent_term_info_export, BIF_P, BIF_ARG_1);
}

BIF_RETTYPE persistent_term_info_0(BIF_ALIST_0) {
    InfoContext *state;
    Eterm state_mref;
    Binary *state_bin;
    Eterm *hp;

    state_bin = erts_create_magic_binary(sizeof(InfoContext),
                                         persistent_term_info_context_dtor);
    hp = HAlloc(BIF_P, ERTS_MAGIC_REF_THING_SIZE);

    state_mref = erts_mk_magic_ref(&hp, &MSO(BIF_P), state_bin);
    state = ERTS_MAGIC_BIN_DATA(state_bin);

    pt_ctrie_iterate(&persistent_terms, &state->iterator);
    state->count = 0;
    state->memory = 0;

    BIF__ARGS[0] = state_mref;
    BIF_RET(persistent_term_info_trap(BIF_P, BIF__ARGS, BIF_I));
}

/* */

typedef struct {
    PersistentTermNode *node;
} PutContext;

static int persistent_term_put_context_dtor(Binary *context_bin) {
    PutContext *ctx = ERTS_MAGIC_BIN_DATA(context_bin);
    PersistentTermNode *node = ctx->node;

    if (node != NULL) {
        /* We've failed to insert ourselves into the trie, so nobody could have
         * observed us: free the literal area straight away even if the stored
         * term is complex so that the ordinary destructor doesn't trigger a
         * literal GC on aborts. */
        if (is_not_both_immed(node->key, node->value)) {
            erts_free(ERTS_ALC_T_LITERAL, node->area);

            /* Replace the pair with immediates to avoid double-freeing them in
             * the destructor. */
            node->key = NIL;
            node->value = NIL;
        }

        pt_ctrie_singleton_release(&(ctx->node)->base);
    }

    return 1;
}

static BIF_RETTYPE persistent_term_put(Process *c_p,
                                       Eterm state_mref,
                                       PutContext *ctx,
                                       PersistentTermNode *previous) {
    int budget = ERTS_BIF_REDS_LEFT(c_p), spent = 0;
    enum erts_ctrie_result result = CTRIE_RESTART;

    while (spent < budget && result != CTRIE_OK) {
        if (previous == NULL) {
            result = pt_ctrie_insert(&persistent_terms, &(ctx->node)->base);

            if (result == CTRIE_OK) {
                persistent_term_update_count(1);
            }

            /* Returns CTRIE_ALREADY_EXISTS on races, in which case we'll retry
             * with a replace operation once it's found on the second try. */
        } else {
            result = pt_ctrie_replace(&persistent_terms,
                                      &(ctx->node)->base,
                                      (pt_ctrie_SingletonNode *)previous);

            if (result == CTRIE_OK) {
                /* Kill all caches pointing to the previous node. */
                erts_atomic_set_wb(&previous->sequence,
                                   erts_atomic_inc_read_nob(&pt_sequence));
            }

            /* FIXME: it would be nice with an CTRIE_XYZ code for when the CAS
             * alone failed, as we could pretend that the previous write was
             * preceded by this write before anyone had a chance to observe
             * it. This is also true for the erase operation. */
        }

        if (result != CTRIE_OK) {
            previous = NULL;

            result = pt_ctrie_lookup(&persistent_terms,
                                     (ctx->node)->key,
                                     (ctx->node)->hash,
                                     (pt_ctrie_SingletonNode **)&previous);
        }

        spent++;
    }

    BUMP_REDS(c_p, spent);

    if (result == CTRIE_OK) {
        /* Ownership has been transferred to the trie, don't try to free the
         * node in the context destructor. */
        ctx->node = NULL;
        return am_ok;
    }

    if (is_non_value(state_mref)) {
        PutContext *state;
        Binary *state_bin;
        Eterm *hp;

        state_bin = erts_create_magic_binary(sizeof(PutContext),
                                             persistent_term_put_context_dtor);
        hp = HAlloc(c_p, ERTS_MAGIC_REF_THING_SIZE);

        state_mref = erts_mk_magic_ref(&hp, &MSO(c_p), state_bin);
        state = ERTS_MAGIC_BIN_DATA(state_bin);

        state->node = ctx->node;
    }

    BIF_TRAP1(&persistent_term_put_export, c_p, state_mref);
}

static BIF_RETTYPE persistent_term_put_trap(BIF_ALIST_1) {
    PutContext *ctx = ERTS_MAGIC_BIN_DATA(erts_magic_ref2bin(BIF_ARG_1));
    return persistent_term_put(BIF_P, BIF_ARG_1, ctx, NULL);
}

BIF_RETTYPE persistent_term_put_2(BIF_ALIST_2) {
    int budget = ERTS_BIF_REDS_LEFT(BIF_P), spent = 0;
    enum erts_ctrie_result result = CTRIE_RESTART;
    PersistentTermNode *previous;
    erts_ihash_t hash;
    Eterm key, value;
    PutContext local;

    key = BIF_ARG_1;
    value = BIF_ARG_2;
    hash = erts_internal_hash(key);

    /* Attempt to apply the no-modification fast path mentioned in the
     * documentation. */
    previous = NULL;
    while (spent < budget && result == CTRIE_RESTART) {
        result = pt_ctrie_lookup(&persistent_terms,
                                 key,
                                 hash,
                                 (pt_ctrie_SingletonNode **)&previous);
        spent++;
    }

    BUMP_REDS(BIF_P, spent);

    if (result == CTRIE_RESTART) {
        BIF_TRAP2(BIF_TRAP_EXPORT(BIF_persistent_term_put_2),
                  BIF_P,
                  key,
                  value);
    } else if (result == CTRIE_OK) {
        if (eq(value, previous->value)) {
            ASSERT(eq(key, previous->key));
            return am_ok;
        }
    }

    local.node = create_node(key,
                             value,
                             hash,
                             erts_atomic_inc_read_nob(&pt_sequence));
    return persistent_term_put(BIF_P, THE_NON_VALUE, &local, previous);
}

/* */

static PersistentTermNode pt_sentinel = {};

static enum erts_ctrie_result persistent_term_update_static_cache(
        PersistentTermStaticCache *cache,
        Eterm key,
        erts_ihash_t hash,
        Eterm *value) {
    enum erts_ctrie_result result;
    PersistentTermNode *node;

    do {
        result = pt_ctrie_lookup(&persistent_terms,
                                 key,
                                 hash,
                                 (pt_ctrie_SingletonNode **)&node);
        if (result == CTRIE_OK) {
            erts_aint_t sequence = erts_atomic_read_nob(&node->sequence);

            result = CTRIE_RESTART;
            do {
                result = pt_ctrie_lookup(&persistent_terms,
                                         key,
                                         hash,
                                         (pt_ctrie_SingletonNode **)&node);
            } while (result == CTRIE_RESTART);

            if (result == CTRIE_OK &&
                sequence == erts_atomic_read_acqb(&node->sequence)) {
                erts_aint_t old = erts_atomic_read_nob(&cache->node);

                if (erts_atomic_cmpxchg_nob(&cache->node,
                                            (erts_aint_t)node,
                                            old) != old) {
                    /* FIXME: Count reductions later on, but just start over
                     * for now. */
                    continue;
                }

                pt_ctrie_singleton_keep((pt_ctrie_SingletonNode *)node);

                if (old != (erts_aint_t)&pt_sentinel) {
                    pt_ctrie_singleton_release((pt_ctrie_SingletonNode *)old);
                }

                /* Since cookies are unique within their namespace, coupled to
                 * specific versions of a key, and the old node is in-place
                 * updated before a new node version is published, all races
                 * here will result in a mismatch between the stored cookie
                 * version and that of the node, resulting in another update.
                 *
                 * Hence, we do not need a double-word swap here. */
                erts_atomic_set_relb(&cache->cookie, (erts_aint_t)sequence);
                *value = node->value;
                result = CTRIE_OK;
            }
        }
    } while (result == CTRIE_RESTART);

    return result;
}

void erts_persistent_term_init_static_cache(PersistentTermStaticCache *cache) {
    erts_atomic64_init_nob(&cache->cookie, 1);
    erts_atomic_init_nob(&cache->node, (erts_aint_t)&pt_sentinel);
}

enum erts_ctrie_result erts_persistent_term_update_static_cache(
        PersistentTermStaticCache *cache,
        Eterm key,
        Eterm *value) {
    return persistent_term_update_static_cache(cache,
                                               key,
                                               erts_internal_hash(key),
                                               value);
}

static void pt_dynamic_cache_free_table(void *table) {
    erts_free(ERTS_ALC_T_PERSISTENT_TERM, table);
}

static PersistentTermDynamicCacheTable *pt_dynamic_cache_create_table(
        size_t size) {
    PersistentTermDynamicCacheTable *table =
            erts_alloc(ERTS_ALC_T_PERSISTENT_TERM,
                       sizeof(PersistentTermDynamicCacheTable) +
                               sizeof(PersistentTermStaticCache) * size);

    table->size = size;
    for (size_t i = 0; i < table->size; i++) {
        erts_persistent_term_init_static_cache(&table->entries[i]);
    }

    return table;
}

void erts_persistent_term_init_dynamic_cache(
        PersistentTermDynamicCache *cache) {
    erts_atomic_init_wb(&cache->table,
                        (erts_aint_t)pt_dynamic_cache_create_table(16));
}

static void pt_dynamic_cache_grow_table(PersistentTermDynamicCache *cache) {
    PersistentTermDynamicCacheTable *old_table, *new_table;

    old_table = (PersistentTermDynamicCacheTable *)erts_atomic_read_ddrb(
            &cache->table);
    new_table = pt_dynamic_cache_create_table(old_table->size * 2);

    if (erts_atomic_cmpxchg_wb(&cache->table,
                               (erts_aint_t)new_table,
                               (erts_aint_t)old_table) ==
        (erts_aint_t)old_table) {
        erts_schedule_thr_prgr_later_op(pt_dynamic_cache_free_table,
                                        (void *)old_table,
                                        &old_table->later_op);
    } else {
        erts_free(ERTS_ALC_T_PERSISTENT_TERM, new_table);
    }
}

enum erts_ctrie_result erts_persistent_term_lookup_dynamic_cache(
        PersistentTermDynamicCache *cache,
        Eterm key,
        Eterm *value) {
    PersistentTermDynamicCacheTable *table;
    PersistentTermStaticCache *entry;
    PersistentTermNode *cached;
    erts_aint_t cookie;
    erts_ihash_t hash;
    size_t index;

    table = (PersistentTermDynamicCacheTable *)erts_atomic_read_ddrb(
        &cache->table);

    hash = erts_map_hash(key);

    /* Table size is always a power of 2. */
    for (index = hash & (table->size - 1); index < table->size; index++) {
        entry = &table->entries[index];

        cached = (PersistentTermNode *)erts_atomic_read_ddrb(&entry->node);
        cookie = erts_atomic_read_nob(&entry->cookie);

        if (cookie != erts_atomic64_read_nob(&cached->sequence)) {
            /* Cache miss; the entry has either been altered since the last
             * change, or been removed. It will need to be updated.
             *
             * (This can also be caused by collisions on certain races, but we
             * don't have to treat that differently from a cache miss) */
            break;
        }

        if (cached->hash == hash && eq(cached->key, key)) {
            *value = cached->value;
            return CTRIE_OK;
        }

        /* Slide over to the next entry. */
        index++;
    }

    /* FIXME: Configurable limits? */
    if (index < table->size && (table->size < (16u << 20))) {
        return persistent_term_update_static_cache(entry, key, hash, value);
    }

    pt_dynamic_cache_grow_table(cache);
    return erts_persistent_term_lookup_dynamic_cache(cache, key, value);
}

enum erts_ctrie_result erts_persistent_term_update_cache(
        Process *c_p,
        Eterm key,
        erts_aint_t *cookie,
        PersistentTermNode **out) {
    int budget = ERTS_BIF_REDS_LEFT(c_p), spent = 0;
    enum erts_ctrie_result result = CTRIE_RESTART;
    erts_ihash_t hash = erts_internal_hash(key);
    PersistentTermNode *node;

    while (spent < budget && result == CTRIE_RESTART) {
        result = pt_ctrie_lookup(&persistent_terms,
                                 key,
                                 hash,
                                 (pt_ctrie_SingletonNode **)&node);
        spent++;

        if (result == CTRIE_OK) {
            erts_aint_t sequence = erts_atomic_read_nob(&node->sequence);

            result = CTRIE_RESTART;
            while (spent < budget && result == CTRIE_RESTART) {
                result = pt_ctrie_lookup(&persistent_terms,
                                         key,
                                         hash,
                                         (pt_ctrie_SingletonNode **)&node);
                spent++;
            }

            /* Note that we already have a ddrb from the lookup. */
            if (result == CTRIE_OK &&
                sequence == erts_atomic_read_nob(&node->sequence)) {
                pt_ctrie_singleton_keep(&node->base);
                *cookie = sequence;
                *out = node;

                result = CTRIE_OK;
            }
        }
    }

    BUMP_REDS(c_p, spent);
    return result;
}

void erts_persistent_term_release_cache(PersistentTermNode *node) {
    pt_ctrie_singleton_release(&node->base);
}

/* */

typedef struct {
    pt_ctrie_Iterator iterator;
    pt_ctrie_Trie snapshot;
    bool clearing;

    ErtsThrPrgrLaterOp later_op;
    Process *process;
} ClearContext;

static int persistent_term_clear_context_dtor(Binary *context_bin) {
    ClearContext *ctx = ERTS_MAGIC_BIN_DATA(context_bin);

    if (ctx->clearing) {
        pt_ctrie_iterate_finish(&ctx->iterator);
        pt_ctrie_destroy(&ctx->snapshot);
    }

    return 1;
}

static void persistent_term_clear_snapshot(void *ctx_) {
    ClearContext *ctx = (ClearContext *)ctx_;
    Binary *bin = &ERTS_MAGIC_BIN_FROM_DATA(ctx)->binary;

    erts_proc_lock(ctx->process, ERTS_PROC_LOCK_STATUS);

    if (!ERTS_PROC_IS_EXITING(ctx->process)) {
        ctx->clearing = true;

        pt_ctrie_iterate(&ctx->snapshot, &ctx->iterator);
        erts_resume(ctx->process, ERTS_PROC_LOCK_STATUS);
    } else {
        /* This ought to never happen; `init` is a system process. */
        ASSERT(!ctx->clearing);
        pt_ctrie_destroy(&ctx->snapshot);
    }

    erts_proc_unlock(ctx->process, ERTS_PROC_LOCK_STATUS);

    erts_proc_dec_refc(ctx->process);
    erts_bin_release(bin);
}

static BIF_RETTYPE persistent_term_clear_trap(BIF_ALIST_1) {
    Binary *magic_binary = erts_magic_ref2bin(BIF_ARG_1);
    ClearContext *ctx = ERTS_MAGIC_BIN_DATA(magic_binary);
    int budget = ERTS_BIF_REDS_LEFT(BIF_P), spent = 0;
    enum erts_ctrie_result result = CTRIE_RESTART;
    PersistentTermNode *node;

    /* Note that we do not shrink the pre-allocated area for crash dumps, as
     * it's a bit of a hassle and we're quite likely to grow to the previous
     * size anyway.
     *
     * We also do not care about the atomicity of the operation with regards
     * to inline caching, as only a few system processes that do not use
     * persistent_term will be running at this point. */

    if (ctx->process == NULL) {
        while (spent < budget && result == CTRIE_RESTART) {
            result = pt_ctrie_clear(&persistent_terms, &ctx->snapshot);
            spent++;
        }

        BUMP_REDS(BIF_P, spent);

        if (result == CTRIE_OK) {
            erts_refc_inctest(&magic_binary->intern.refc, 2);
            erts_schedule_thr_prgr_later_op(persistent_term_clear_snapshot,
                                            (void *)ctx,
                                            &ctx->later_op);

            ctx->process = BIF_P;
            erts_proc_inc_refc(ctx->process);
            erts_suspend(ctx->process, ERTS_PROC_LOCK_MAIN, NULL);
        }

        BIF_TRAP1(&persistent_term_clear_export, BIF_P, BIF_ARG_1);
    }

    while (spent < budget) {
        spent++;

        if (!pt_ctrie_iterate_next(&ctx->iterator,
                                   (pt_ctrie_SingletonNode **)&node)) {
            ERTS_THR_WRITE_MEMORY_BARRIER;

            BIF_RET(am_ok);
        }

        /* Invalidate the corresponding cache entry. The node will be kept
         * alive until the next lookup. */
        erts_atomic_set_nob(&node->sequence,
                            erts_atomic_inc_read_nob(&pt_sequence));
    }

    BUMP_REDS(BIF_P, spent);
    BIF_TRAP1(&persistent_term_clear_export, BIF_P, BIF_ARG_1);
}

BIF_RETTYPE erts_internal_erase_persistent_terms_0(BIF_ALIST_0) {
    ClearContext *state;
    Eterm state_mref;
    Binary *state_bin;
    Eterm *hp;

    state_bin = erts_create_magic_binary(sizeof(ClearContext),
                                         persistent_term_clear_context_dtor);
    hp = HAlloc(BIF_P, ERTS_MAGIC_REF_THING_SIZE);

    state_mref = erts_mk_magic_ref(&hp, &MSO(BIF_P), state_bin);
    state = ERTS_MAGIC_BIN_DATA(state_bin);

    state->process = NULL;
    state->clearing = false;

    BIF__ARGS[0] = state_mref;
    BIF_RET(persistent_term_clear_trap(BIF_P, BIF__ARGS, BIF_I));
}

/* persistent_term_SUITE:chk/0,1 */
Eterm erts_debug_persistent_term_xtra_info(Process *c_p) {
    Eterm count_term, res;
    erts_aint_t count;
    Uint hsz;
    Eterm *hp;

    count = erts_atomic_read_nob(&pt_cd_current_count);
    hsz = MAP_SZ(1);

    (void)erts_bld_uint(NULL, &hsz, count);
    hp = HAlloc(c_p, hsz);
    count_term = erts_bld_uint(&hp, NULL, count);

    res = MAP1(hp, am_table, count_term);
    BIF_RET(res);
}

static void persistent_term_update_count(erts_aint_t diff) {
    erts_aint_t count, watermark;

    count = erts_atomic_add_read_acqb(&pt_cd_current_count, diff);
    watermark = erts_atomic_read_nob(&pt_cd_watermark);

    if (count < watermark) {
        return;
    }

    /* We need to grow `erts_persistent_areas` in case we crash. We bump the
     * watermark relative to its current level to avoid any funny races with
     * `count` being reduced in the meantime: we don't want to land here
     * very often, so passing the watermark once should be cause to bump it.
     */
    do {
        erts_aint_t next = watermark;

        ASSERT((next / 4) > 0);
        while (next <= count) {
            next += next / 4;
        }

        watermark = erts_atomic_cmpxchg_relb(&pt_cd_watermark, next, watermark);
        count = erts_atomic_read_acqb(&pt_cd_current_count);
    } while (count >= watermark);

    erts_mtx_lock(&pt_cd_lock);

    watermark = erts_atomic_read_nob(&pt_cd_watermark);
    if (watermark > pt_cd_allocated) {
        erts_free(ERTS_ALC_T_CRASH_DUMP, erts_persistent_areas);

        erts_persistent_areas = erts_alloc(ERTS_ALC_T_CRASH_DUMP, watermark);
        pt_cd_allocated = watermark;
    }

    erts_mtx_unlock(&pt_cd_lock);
}

static void persistent_term_init_crash_dump_node(
        pt_ctrie_SingletonNode *singleton,
        void *arg) {
    PersistentTermNode *node = (PersistentTermNode *)singleton;

    (void)arg;

    if (is_not_both_immed(node->key, node->value)) {
        erts_persistent_areas[erts_num_persistent_areas++] = node->area;
    }
}

void erts_init_persistent_dumping(void) {
    pt_ctrie_crash_dump_init(&persistent_terms);

    pt_ctrie_crash_dump_foreach(&persistent_terms,
                                persistent_term_init_crash_dump_node,
                                NULL);
}

static Uint accessed_literal_areas_size;
static Uint accessed_no_literal_areas;
static ErtsLiteralArea **accessed_literal_areas;

int erts_debug_have_accessed_literal_area(ErtsLiteralArea *lap) {
    for (Uint i = 0; i < accessed_no_literal_areas; i++) {
        if (accessed_literal_areas[i] == lap) {
            return !0;
        }
    }

    return 0;
}

void erts_debug_save_accessed_literal_area(ErtsLiteralArea *lap) {
    if (accessed_no_literal_areas == accessed_literal_areas_size) {
        accessed_literal_areas_size += 10;
        accessed_literal_areas = erts_realloc(
                ERTS_ALC_T_TMP,
                accessed_literal_areas,
                (sizeof(ErtsLiteralArea *) * accessed_literal_areas_size));
    }

    accessed_literal_areas[accessed_no_literal_areas++] = lap;
}

typedef struct {
    void (*func)(ErlOffHeap *, void *);
    void *arg;
} CrashDumpContext;

static void persistent_term_dump_node(pt_ctrie_SingletonNode *singleton,
                                      void *arg) {
    PersistentTermNode *node = (PersistentTermNode *)singleton;
    CrashDumpContext *ctx = (CrashDumpContext *)arg;
    ErlOffHeap oh;

    if (is_not_both_immed(node->key, node->value) &&
        !erts_debug_have_accessed_literal_area(node->area)) {
        ERTS_INIT_OFF_HEAP(&oh);

        oh.first = (node->area)->off_heap;
        ctx->func(&oh, ctx->arg);

        erts_debug_save_accessed_literal_area(node->area);
    }
}

void erts_debug_foreach_persistent_term_off_heap(void (*func)(ErlOffHeap *,
                                                              void *),
                                                 void *arg) {
    CrashDumpContext ctx;

    ctx.func = func;
    ctx.arg = arg;

    pt_ctrie_crash_dump_foreach(&persistent_terms,
                                persistent_term_dump_node,
                                (void *)&ctx);
}
