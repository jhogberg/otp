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

static ErtsCodePatch *staging_patches = NULL;

void erts_code_patch_start_staging(void) {
    staging_patches = NULL;
}

void erts_code_patch_end_staging(int commit) {
    (void)commit;
}

void erts_code_patch_refresh(ErtsCodePatch *patch) {
#ifdef ERTS_ENABLE_LOCK_CHECK
    ASSERT(erts_has_code_load_permission());
#endif

    patch->staging_next = staging_patches;
    staging_patches = patch;
}

bool erts_code_patches_pending(void) {
    return staging_patches != NULL;
}

void erts_code_patch_begin(ErtsCodePatchAction action,
                           ErtsCodePatchState *state) {
    ASSERT((action != ERTS_CODE_PATCH_ACTION_LOAD) &&
           (action != ERTS_CODE_PATCH_ACTION_PURGE));

    state->next = staging_patches;
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

    for (ErtsCodePatch *patch = state->next;
         patch != NULL;
         patch = patch->staging_next) {
        erts_unseal_module(patch->module_instance, unsealed);
        patch->action(patch, state->action);

        if (limit-- < 0 && patch->staging_next) {
            state->next = patch->staging_next;
            return false;
        }
    }

    return true;
}
