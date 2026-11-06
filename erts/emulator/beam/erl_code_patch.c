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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "erl_code_patch.h"
#include "module.h"

#ifdef BEAMASM
#  include "beam_asm.h"
#endif

/* Circular list of staged patches.
 *
 * Patches are part of this list when their `staging_next` is non-NULL. */
static ErtsCodePatch staging_patches;

void erts_init_code_patches(void) {
    erts_code_patch_start_staging();
}

void erts_code_patch_start_staging(void) {
    staging_patches.staging_next = &staging_patches;
}

void erts_code_patch_end_staging(int commit) {
    if (commit == 0) {
        ErtsCodePatch *next;

        /* We're aborting; clear out all patches that are scheduled to be
         * refreshed. This doesn't yield but we currently lack a general
         * mechanism for yielding during the code staging steps, so this will
         * just have to do. */
        for (ErtsCodePatch *patch = staging_patches.staging_next;
             patch != &staging_patches;
             patch = next) {

            next = patch->staging_next;
            ASSERT(next != NULL);
            patch->staging_next = NULL;
        }
    }
}

void erts_code_patch_refresh(ErtsCodePatch *patch) {
#ifdef ERTS_ENABLE_LOCK_CHECK
    ASSERT(erts_has_code_load_permission());
#endif

    /* A patch may be refreshed twice if two modules are loaded in the same
     * batch, e.g. if a native record is used in one module and defined in
     * another. Nop if we're already going to be refreshed. */
    if (patch->staging_next == NULL) {
        patch->staging_next = staging_patches.staging_next;
        staging_patches.staging_next = patch;
    }
}

bool erts_code_patches_pending(void) {
    return staging_patches.staging_next != &staging_patches;
}

void erts_code_patch_begin(ErtsCodePatchAction action,
                           ErtsCodePatchState *state) {
    ASSERT((action != ERTS_CODE_PATCH_ACTION_LOAD) &&
           (action != ERTS_CODE_PATCH_ACTION_PURGE));

    state->next = staging_patches.staging_next;
    state->action = action;
}

bool erts_code_patch_continue(ErtsCodePatchState *state,
                              struct erl_module_instance **unsealed) {
    int limit = 4000;

#ifdef DEBUG
    limit /= 1000;
#endif

#ifdef ERTS_ENABLE_LOCK_CHECK
    ASSERT(erts_has_code_load_permission());
#endif

    for (ErtsCodePatch *patch = state->next, *next;
         patch != &staging_patches;
         patch = next) {
        /* All staged patches have non-NULL next pointers. */
        next = patch->staging_next;
        ASSERT(next != NULL);

        erts_unseal_module(patch->module_instance, unsealed);
        patch->action(patch, state->action);

        if (state->action == ERTS_CODE_PATCH_ACTION_ENABLE) {
            /* Make sure we can be refreshed again. */
            patch->staging_next = NULL;
        }

        if (limit-- < 0 && next) {
            state->next = next;
            return false;
        }
    }

    return true;
}
