/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright Ericsson AB 2026. All Rights Reserved.
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


#ifndef __ERL_CODE_PATCH_H__
#define __ERL_CODE_PATCH_H__

#ifndef __SYS_H__
#  ifdef HAVE_CONFIG_H
#    include "config.h"
#  endif
#  include "sys.h"
#endif

#include "code_ix.h"

typedef enum {
    /* Called once the module that owns the patch is on its way to be
     * committed.
     *
     * Note that this means that this will not run until after the `on_load`
     * callback if one is present. */
    ERTS_CODE_PATCH_ACTION_LOAD = 0,

    /* Called once the module that owns the patch is about to be purged. */
    ERTS_CODE_PATCH_ACTION_PURGE = 1,

    /* When a patch has been staged to be refreshed, these actions are called
     * in turn with a code barrier in between. This is only ever done as a
     * consequence of loading a module, and at this point we know that the
     * code index is about to be committed (between `APPLY` and `ENABLE`).
     *
     * - `DISABLE` renders the patch inert (e.g. by forcing control flow to a
     *   generic fallback)
     * - `APPLY` applies the patch *BEFORE* the code index is committed, so all
     *   of its lookups must use the staging code index to match the index used
     *   once enabled.
     * - `ENABLE` enables the patch *AFTER* the code index has been committed.
     *
     * The gadgets in the `DISABLE` and `ENABLE` steps must tolerate concurrent
     * modification of executing code. This is generally not a big problem on
     * x86 as you're guaranteed to either execute the old or the new code as
     * as long as the write wasn't sheared, but ARM restricts that guarantee to
     * certain instructions, of which only `B`, `BL`, and `NOP` are relevant to
     * us. Otherwise, the code patching logic takes care of cache invalidation
     * and instruction synchronization so you don't have to.
     *
     * Needless to say, the code must already be disabled when first generated
     * to avoid disaster when an `on_load` callback is present. */
    ERTS_CODE_PATCH_ACTION_DISABLE = 2,
    ERTS_CODE_PATCH_ACTION_APPLY = 3,
    ERTS_CODE_PATCH_ACTION_ENABLE = 4
} ErtsCodePatchAction;

typedef struct ErtsCodePatch_ {
    struct erl_module_instance *module_instance;

    /* NULL-terminated singly-linked list of all patches (regardless of kind)
     * in the module the patch concerns, for quick removal of all patches
     * when unloading a module; just sweep through and invoke the remove
     * callback.
     *
     * This is protected by the code modification lock after module load. */
    struct ErtsCodePatch_ *module_next;

    /* NULL-terminated singly-linked list of all patches to refresh in this
     * staging phase (invoking disable, apply, enable in turn). This is
     * garbage when the patch is not actively being staged.
     * 
     * This is protected by the code modification lock after module load. */
    struct ErtsCodePatch_ *staging_next;

    void (*action)(struct ErtsCodePatch_ *patch, ErtsCodePatchAction action);
} ErtsCodePatch;

/* Marks the given patch as due for a refresh.
 *
 * This requires code loading permission. */
void erts_code_patch_refresh(ErtsCodePatch *patch);

typedef struct {
    ErtsCodePatchAction action;
    ErtsCodePatch *next;
} ErtsCodePatchState;

/** \brief Returns whether we have any code patches pending, so that we may
 * skip a bunch of redundant barriers if there are none. */
bool erts_code_patches_pending(void);

/** \brief Sets up \c erts_code_patch_continue */
void erts_code_patch_begin(ErtsCodePatchAction action,
                           ErtsCodePatchState *state);

/** \brief Executes the given patch action on all patches that are due for
 * refresh.
 *
 * Returns true when the current step is finished, and false when there's more
 * work remaining.
 *
 * This requires code loading permission. */
bool erts_code_patch_continue(ErtsCodePatchState *state,
                              struct erl_module_instance **unsealed);

void erts_code_patch_start_staging(void);
void erts_code_patch_end_staging(int commit);

typedef struct {
    bool yield;
} ErtsCodePatchYieldData;

#endif /* __ERL_CODE_PATCH_H__ */
