/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright Ericsson AB 2025-2026. All Rights Reserved.
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

#include "beam_asm.hpp"

extern "C"
{
#include "erl_record.h"
}

struct RecordPatchBase : ErtsRecordCodePatch {
    static_assert(std::is_standard_layout<ErtsRecordCodePatch>::value);

    /* This is a magical value that we use to force the fallback path when a
     * patch is enabled. An alternative would be to have a special return value
     * for the `APPLY` action to suppress `ENABLE`, but I'd rather not have to
     * keep that state around.
     *
     * The bit pattern here will never match a valid definition or
     * [Definition|Defaults] pair, even when the value is partially read as
     * both the high and low bytes cannot possibly match a valid value. The
     * latter because of the tag scheme, and the former because user-mode
     * pointers on x64 always have the highest bit cleared. */
    static constexpr Eterm ForceFallbackSentinel = make_small(-1);

    Eterm module, name;

#ifdef __GNUC__
#    pragma GCC diagnostic push
/* Suppress alignment warnings for `Eterm` in templates. */
#    pragma GCC diagnostic ignored "-Wignored-attributes"
#endif

    std::vector<std::pair<Eterm, size_t>> fields;
    size_t jump_offset, sentinel_offset;

#ifdef __GNUC__
#    pragma GCC diagnostic pop
#endif

    RecordPatchBase(ErtsCodePatch *&patch_list, Eterm id) {
        auto tuple = tuple_val(id);

        module = tuple[1];
        name = tuple[2];

        /* This is nonsense but simplifies `purge` in the case we abort
         * loading. */
        link_previous = this;
        link_next = this;

        base.module_next = patch_list;
        patch_list = &base;
    }

    template<typename T>
    void poke(size_t offset, T value) {
        char *writable_region =
                reinterpret_cast<char *>(base.module_instance->writable_region);
        *reinterpret_cast<T *>(&writable_region[offset]) = value;
    }

    template<typename T>
    T peek(size_t offset) {
        char *writable_region =
                reinterpret_cast<char *>(base.module_instance->writable_region);
        return *reinterpret_cast<T *>(&writable_region[offset]);
    }

    void load() {
#ifdef ERTS_ENABLE_LOCK_CHECK
        ASSERT(erts_has_code_load_permission());
#endif

        entry = erts_record_put(module, name);

        link_previous = &entry->patches;
        link_next = entry->patches.link_next;

        link_next->link_previous = this;
    }

    void purge() {
        link_next->link_previous = link_previous;
        link_previous->link_next = link_next;
    }

    void disable() {
        /* Rewrite `JNE fallback` to `NOP, JMP fallback`
         *
         * Note that this is only intended to guard the region we modify in
         * `apply`, it does not guard the sentinel value.
         *
         * Overwrites of the sentinel value are benign because unlike AArch64,
         * x64 tolerates partial write of instructions as long as both the old
         * and new instructions make some semblance of sense, so even
         * an unaligned overwrite of an immediate argument is quite okay as we
         * will present either the new or the old value; we'll be fine as long
         * as we don't try to follow the pointer and just use the sentinel
         * value for comparison.
         *
         * However, a partial write could lead to a false positive if we're
         * not careful, so we cannot write it willy-nilly. If we were to
         * write a new sentinel value (other than the rather magical
         * `ForceFallbackSentinel`) we could possibly end up with a shredded
         * value that's erroneously equal to the definition the fast path is
         * compiled for, so it's important that we only write valid values
         * while the patch is disabled. */
        auto jump = peek<UWord>(jump_offset);
        ASSERT((jump & 0xFFFF) == 0x850F);
        poke<UWord>(jump_offset, (jump & ~0xFFFF) | 0xE990);
    }

    void enable() {
        /* Rewrite `NOP, JMP fallback` to `JNE fallback` */
        auto jump = peek<UWord>(jump_offset);
        ASSERT(((jump & 0xFFFF) == 0xE990));
        poke<UWord>(jump_offset, (jump & ~0xFFFF) | 0x850F);
    }

    template<typename T>
    static void action(struct ErtsCodePatch_ *patch, ErtsCodePatchAction what) {
        auto me = static_cast<T *>(
                reinterpret_cast<ErtsRecordCodePatch *>(patch));

#ifdef ERTS_ENABLE_LOCK_CHECK
        ASSERT(erts_has_code_mod_permission());
#endif

        switch (what) {
        case ERTS_CODE_PATCH_ACTION_LOAD:
            me->load();
            break;
        case ERTS_CODE_PATCH_ACTION_PURGE:
            me->purge();
            me->~T();

            erts_free(ERTS_ALC_T_CODE_PATCH, me);
            break;
        case ERTS_CODE_PATCH_ACTION_DISABLE:
            me->disable();
            break;
        case ERTS_CODE_PATCH_ACTION_APPLY:
            me->apply();
            break;
        case ERTS_CODE_PATCH_ACTION_ENABLE:
            me->enable();
            break;
        }
    }
};

void BeamModuleAssembler::emit_is_any_native_record(const ArgLabel &Fail,
                                                    const ArgRegister &Src) {
    mov_arg(ARG3, Src);

    emit_is_boxed(resolve_beam_label(Fail), Src, ARG3);

    preserve_cache(
            [&]() {
                x86::Gp boxed_ptr = emit_ptr_val(RET, ARG3);
                a.mov(RETd, emit_boxed_val(boxed_ptr, 0, sizeof(Uint32)));
                a.and_(RETb, imm(_TAG_HEADER_MASK));
                a.cmp(RETb, imm(_TAG_HEADER_RECORD));
                a.jne(resolve_beam_label(Fail));
            },
            RET);
}

void BeamModuleAssembler::emit_is_native_record(const ArgLabel &Fail,
                                                const ArgRegister &Src,
                                                const ArgAtom &Module,
                                                const ArgAtom &Name) {
    x86::Gp boxed_ptr;

    mov_arg(ARG3, Src);
    boxed_ptr = emit_ptr_val(RET, ARG3);

    mov_preserve_cache(
            RET,
            emit_boxed_val(boxed_ptr,
                           offsetof(ErtsRecordInstance, record_definition),
                           sizeof(Uint64)));
    boxed_ptr = emit_ptr_val(RET, RET);

    /* We're never going to see a Module or Name that doesn't fit into 32
     * bits. Shave off a byte by making it a 32 bit operation. */
    cmp_arg(emit_boxed_val(boxed_ptr,
                           offsetof(ErtsRecordDefinition, module),
                           sizeof(Uint32)),
            Module);
    preserve_cache([&]() {
        a.jne(resolve_beam_label(Fail));
    });
    cmp_arg(emit_boxed_val(boxed_ptr,
                           offsetof(ErtsRecordDefinition, name),
                           sizeof(Uint32)),
            Name);
    preserve_cache([&]() {
        a.jne(resolve_beam_label(Fail));
    });
}

void BeamModuleAssembler::emit_is_record_accessible(const ArgLabel &Fail,
                                                    const ArgRegister &Src,
                                                    const ArgAtom &Scope) {
    x86::Gp boxed_ptr;
    mov_arg(ARG3, Src);

    boxed_ptr = emit_ptr_val(RET, ARG3);

    mov_preserve_cache(
            RET,
            emit_boxed_val(boxed_ptr,
                           offsetof(ErtsRecordInstance, record_definition)));
    boxed_ptr = emit_ptr_val(RET, RET);
    cmp_arg(emit_boxed_val(boxed_ptr,
                           offsetof(ErtsRecordDefinition, is_exported)),
            ArgAtom(am_true));
    if (Scope.get() == am_external) {
        comment("external operation");
        preserve_cache([&]() {
            a.jne(resolve_beam_label(Fail));
        });
    } else {
        Label next = a.new_label();

        comment("auto_local operation");
        preserve_cache([&]() {
            a.short_().je(next);
        });

        cmp_arg(emit_boxed_val(boxed_ptr,
                               offsetof(ErtsRecordDefinition, module)),
                ArgAtom(mod));
        preserve_cache([&]() {
            a.jne(resolve_beam_label(Fail));
        });

        a.bind(next);
    }
}

void BeamModuleAssembler::emit_i_get_local_record_elements(
        const ArgLiteral &Def,
        const ArgRegister &Src,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    Eterm def;
    ErtsRecordDefinition *defp;
    int field_count;
    Eterm cons = beamfile_get_literal(beam, Def.get());
    Uint argp;
    const Uint header_offset =
            offsetof(ErtsRecordInstance, values) - TAG_PRIMARY_BOXED;

    def = CAR(list_val(cons));
    defp = (ErtsRecordDefinition *)tuple_val(def);

    field_count = RECORD_DEF_FIELD_COUNT(defp);

    comment("name: %T", defp->name);
    mov_arg(ARG1, Src);
    emit_ptr_val(ARG1, ARG1);
    argp = 0;
    for (int i = 0; i < field_count; i++) {
        if (argp + 1 < args.size() &&
            args[argp].as<ArgAtom>().get() == defp->keys[i]) {
            if (argp + 3 < args.size() &&
                args[argp + 2].as<ArgAtom>().get() == defp->keys[i + 1]) {
                emit_get_pair(ARG1,
                              header_offset + i * sizeof(Eterm),
                              args[argp + 1],
                              args[argp + 3]);
                argp += 4;
            } else {
                a.mov(ARG2,
                      x86::qword_ptr(ARG1, header_offset + i * sizeof(Eterm)));
                mov_arg(args[argp + 1], ARG2);
                argp += 2;
            }
        }
    }
}

struct GetExternalRecordElementsPatch : RecordPatchBase {
    GetExternalRecordElementsPatch(ErtsCodePatch *&patch_list, Eterm id)
            : RecordPatchBase(patch_list, id) {
        base.action = action<GetExternalRecordElementsPatch>;
    }

    void apply() {
        Eterm cons = entry->definitions[erts_staging_code_ix()];

        if (is_value(cons)) {
            auto definition = CAR(list_val(cons));
            auto definition_p = reinterpret_cast<ErtsRecordDefinition *>(
                    tuple_val(definition));
            auto count = RECORD_DEF_FIELD_COUNT(definition_p);

            size_t found = 0;

            if (fields.size() <= count) {
                size_t i = 0;

                for (const auto &[key, where] : fields) {
                    if (key == definition_p->keys[i]) {
                        poke<Uint32>(where,
                                     offsetof(ErtsRecordInstance, values) +
                                             (i * sizeof(Eterm) -
                                              TAG_PRIMARY_BOXED));
                        found++;
                    }

                    i++;
                }
            }

            if (found == fields.size()) {
                /* All keys were found. */
                poke<Eterm>(sentinel_offset, definition);
                return;
            }
        }

        poke<Eterm>(sentinel_offset, RecordPatchBase::ForceFallbackSentinel);
    }
};

void BeamModuleAssembler::emit_i_get_external_record_elements(
        const ArgLabel &Fail,
        const ArgLiteral &Name,
        const ArgRegister &Src,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    auto name = beamfile_get_literal(beam, Name.get());

    auto patch = new (erts_alloc(ERTS_ALC_T_CODE_PATCH,
                                 sizeof(GetExternalRecordElementsPatch)))
            GetExternalRecordElementsPatch(patches, name);

    comment("name: %T:%T", patch->module, patch->name);
    mov_arg(ARG3, Src);

    auto source = emit_ptr_val(ARG1, ARG3);

    Label next = a.new_label();

    /* Compare the instance definition against the definition we're compiled
     * for. This assumes that the patch is kept up to date with the current
     * canonical definition at all times. Note that this is not guarded by the
     * jump below, and that a shredded read of the pointer may very well occur
     * because we cannot guarantee proper alignment of the imm64. This is
     * benign as it can only shred when the fallback path is unconditional. */
    a.long_().movabs(RET, imm(ERTS_SINT64_MAX));
    patch->sentinel_offset = a.offset() - 8;
    a.cmp(RET, x86::qword_ptr(source, sizeof(Eterm) - TAG_PRIMARY_BOXED));

    /* This is word-aligned as the required jump instructions differ in length
     * (5 unconditional, 6 conditional), and must therefore be written
     * atomically. */
    a.align(AlignMode::kCode, 8);
    {
        patch->jump_offset = a.offset();
        a.long_().jne(cold_fragment([&]() {
            Label generic = a.new_label();
            a.bind(generic);

            a.mov(ARG1, c_p);
            load_x_reg_array(ARG2);
            /* ARG3 is already set. */
            mov_imm(ARG4, args.size());
            embed_vararg_rodata(args, ARG5, 0);

            emit_enter_runtime<Update::eStack>();
            runtime_call<
                    bool (*)(Process *, Eterm *, Eterm, Uint, const Eterm *),
                    erl_get_record_elements>();
            emit_leave_runtime<Update::eStack>();

            a.test(RETb, RETb);
            a.je(resolve_beam_label(Fail));
            a.jmp(next);

            return generic;
        }));
    }

    for (UWord i = 0; i < Size.get(); i += 2) {
        a.mov(RET, x86::qword_ptr(source, ERTS_SINT32_MAX));
        patch->fields.emplace_back(args[i].as<ArgAtom>().get(), a.offset() - 4);

        mov_arg(args[i + 1], RET);
    }

    a.bind(next);
    invalidate_cache();
}

void BeamModuleAssembler::emit_i_get_record_elements(
        const ArgLabel &Fail,
        const ArgRegister &Src,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    mov_arg(ARG3, Src);
    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    mov_imm(ARG4, args.size());
    embed_vararg_rodata(args, ARG5, 0);

    emit_enter_runtime<Update::eStack>();
    runtime_call<bool (*)(Process *, Eterm *, Eterm, Uint, const Eterm *),
                 erl_get_record_elements>();
    emit_leave_runtime<Update::eStack>();

    a.test(RETb, RETb);
    a.je(resolve_beam_label(Fail));
}

void BeamModuleAssembler::emit_i_create_local_native_record(
        const ArgLiteral &Def,
        const ArgRegister &Dst,
        const ArgWord &Live,
        const ArgWord &size,
        const Span<const ArgVal> &args) {
    Eterm def;
    ErtsRecordDefinition *defp;
    int field_count;
    Uint num_words_needed;
    Eterm *loader_def_values;
    Eterm cons = beamfile_get_literal(beam, Def.get());
    Uint argp;
    bool otp_29 = beam->code.max_opcode <= genop_get_record_field_5;

    def = CAR(list_val(cons));
    defp = (ErtsRecordDefinition *)tuple_val(def);
    loader_def_values = tuple_val(CDR(list_val(cons))) + 1;

    field_count = RECORD_DEF_FIELD_COUNT(defp);
    num_words_needed = RECORD_INST_SIZE(field_count);

    comment("name: %T", defp->name);

    if (otp_29) {
        /* If compiled by OTP 29, we must do a GC test here.
         * If compiled by OTP 30 or later, this instruction is
         * preceded by a `test_heap` instruction that has already
         * ensured sufficient heap space. */
        emit_gc_test(ArgWord(0), ArgWord(num_words_needed), Live);
    }

    extract_from_literal(RET, Def, [](Eterm value) -> Eterm {
        return CAR(list_val(value));
    });

    a.mov(x86::qword_ptr(HTOP), MAKE_RECORD_HEADER(field_count));
    a.mov(x86::qword_ptr(HTOP, sizeof(Eterm)), RET);

    argp = 0;
    for (int i = 0; i < field_count; i++) {
        x86::Mem dst_ptr = x86::qword_ptr(HTOP, (i + 2) * sizeof(Eterm));
        if (argp < args.size() &&
            args[argp].as<ArgAtom>().get() == defp->keys[i]) {
            if (args[argp + 1].isImmed() &&
                Bitwise::fits_signed<32>(
                        (Sint)(args[argp + 1].as<ArgImmed>().get()))) {
                Eterm value = args[argp + 1].as<ArgImmed>().get();
                a.mov(dst_ptr, imm(value));
            } else {
                mov_arg(RET, args[argp + 1]);
                a.mov(dst_ptr, RET);
            }
            argp += 2;
        } else {
            Eterm value = loader_def_values[i];
            if (is_immed(value) && Bitwise::fits_signed<32>((Sint)(value))) {
                a.mov(dst_ptr, imm(value));
            } else {
                extract_from_literal(RET, Def, [i](Eterm value) -> Eterm {
                    auto defaults = CDR(list_val(value));
                    return tuple_val(defaults)[i + 1];
                });

                a.mov(dst_ptr, RET);
            }
        }
    }

    comment("Create boxed ptr");
    x86::Gp tmp_reg = alloc_temp_reg();
    preserve_cache(
            [&]() {
                a.lea(tmp_reg, x86::qword_ptr(HTOP, TAG_PRIMARY_BOXED));
                a.add(HTOP, imm(num_words_needed * sizeof(Eterm)));
            },
            HTOP,
            tmp_reg);

    mov_arg(Dst, tmp_reg);
}

/*
 * ARG4 = Id
 * ARG5 = (size << 10) | live
 * ARG6 = args
 */
void BeamGlobalAssembler::emit_create_native_record_shared() {
    Label trap = a.new_label();
    Label error = a.new_label();
    Label finish_create = a.new_label();

    emit_enter_frame();

    load_x_reg_array(ARG2);
    a.mov(ARG1, c_p);
    a.mov(ARG3, active_code_ix);

    emit_enter_runtime<Update::eHeapAlloc | Update::eReductions>();

    runtime_call<Eterm (*)(Process *,
                           Eterm *,
                           ErtsCodeIndex,
                           Eterm,
                           Uint,
                           const Eterm *),
                 erl_create_native_record_jit>();

    emit_leave_runtime<Update::eHeapAlloc | Update::eReductions>();

    emit_test_the_non_value(RET);
    a.short_().je(trap);

    emit_leave_frame();
    a.ret();

    a.bind(error);
    {
        emit_leave_frame();
        emit_test_the_non_value(RET);
        a.ret();
    }

    a.bind(trap);
    {
        a.cmp(x86::qword_ptr(c_p, offsetof(Process, freason)), imm(TRAP));
        a.jne(error);

        /* Ensure that control is transferred to `finish_create` after
         * having loaded the code. */
        a.lea(RET, x86::qword_ptr(finish_create));
#if defined(NATIVE_ERLANG_STACK)
        a.mov(getYRef(0), RET);
#else
        /* It is essential that the continuation pointer is stored on
         * the stack for the Erlang process, not on the runtime stack,
         * because `finish_create` might execute on a different
         * scheduler thread (with a different runtime stack). */
        a.pop(getCPRef().clone_adjusted(sizeof(Eterm)));
        a.mov(getCPRef(), RET);
#endif
        emit_enter_frame();

        /* Dispatch directly to the trap code. There is no need to do
         * a context switch. */
        a.mov(RET, x86::qword_ptr(c_p, offsetof(Process, i)));
        a.jmp(RET);
    }

    align_erlang_cp();
    a.bind(finish_create);
    {
        /* Now, the module is probably loaded. Again try to create the
         * record. */
        load_x_reg_array(ARG2);
        a.mov(ARG1, c_p);
        a.mov(ARG3, active_code_ix);

        emit_enter_runtime<Update::eHeapAlloc | Update::eReductions>();
        runtime_call<Eterm (*)(Process *, Eterm *, ErtsCodeIndex),
                     erl_finish_create_record_jit>();
        emit_leave_runtime<Update::eHeapAlloc | Update::eReductions>();

#if !defined(NATIVE_ERLANG_STACK)
        /* Put back the continuation on the runtime stack, since we
         * will soon do a `ret`. */

        a.push(TMP_MEM1q);
#endif

        emit_leave_frame();
        emit_test_the_non_value(RET);
        a.ret();
    }
}

struct CreateExternalRecordPatch : RecordPatchBase {
    size_t copy_offset, entry_offset, gc_offset, header_offset;

    CreateExternalRecordPatch(ErtsCodePatch *&patch_list, Eterm id)
            : RecordPatchBase(patch_list, id) {
        base.action = action<CreateExternalRecordPatch>;
    }

    void load() {
        RecordPatchBase::load();

        poke<ErtsRecordEntry *>(entry_offset, entry);
    }

    void apply() {
        Eterm cons = entry->definitions[erts_staging_code_ix()];

        if (is_value(cons)) {
            auto definition = reinterpret_cast<ErtsRecordDefinition *>(
                    tuple_val(CAR(list_val(cons))));
            auto defaults = tuple_val(CDR(list_val(cons)));
            size_t count = RECORD_DEF_FIELD_COUNT(definition), found = 0, i = 0;

            if (fields.size() <= count) {
                for (const auto &[key, where] : fields) {
                    if (key == definition->keys[i]) {
                        /* N.B. The offset is backwards from the end. */
                        poke<Sint32>(where, -((count - i) * sizeof(Eterm)));
                        found++;
                    } else if (!is_value(defaults[i])) {
                        break;
                    }

                    i++;
                }
            }

            if (i == count && found == fields.size()) {
                /* All fields we've set out to fill have been set, and we
                 * didn't miss any mandatory fields.
                 *
                 * Keep in mind that it's perfectly valid to create a native
                 * record without setting any fields, as long as there are
                 * defaults for everything, and `count` may thus be 0 for an
                 * empty record. */
                poke<Uint32>(gc_offset,
                             (RECORD_INST_SIZE(count) + S_RESERVED) *
                                     sizeof(Eterm));
                poke<Uint32>(header_offset, MAKE_RECORD_HEADER(count));
                poke<Uint32>(copy_offset, count);

                poke<Eterm>(sentinel_offset, cons);
                return;
            }
        }

        poke<Eterm>(sentinel_offset, RecordPatchBase::ForceFallbackSentinel);
    }
};

void BeamModuleAssembler::emit_i_create_external_native_record(
        const ArgLabel &Fail,
        const ArgLiteral &Name,
        const ArgRegister &Dst,
        const ArgWord &Live,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    auto name = beamfile_get_literal(beam, Name.get());

    auto patch = new (erts_alloc(ERTS_ALC_T_CODE_PATCH,
                                 sizeof(CreateExternalRecordPatch)))
            CreateExternalRecordPatch(patches, name);

    Label entry = a.new_label(), next = a.new_label();

    align_erlang_cp();
    a.bind(entry);
    {
        a.long_().movabs(ARG1, imm(ERTS_SINT64_MAX));
        patch->entry_offset = a.offset() - 8;

        /* This is not guarded by the jump, see the same code in
         * `GetExternalRecordElementsPatch` for details. */
        a.long_().movabs(RET, imm(ERTS_SINT64_MAX));
        patch->sentinel_offset = a.offset() - 8;
        a.cmp(RET,
              x86::qword_ptr(ARG1,
                             active_code_ix,
                             3,
                             offsetof(ErtsRecordEntry, definitions)));

        /* This is word-aligned as the required jump instructions differ in
         * length (5 unconditional, 6 conditional), and must therefore be
         * written atomically. */
        a.align(AlignMode::kCode, 8);
        patch->jump_offset = a.offset();
        a.long_().jne(cold_fragment([&]() {
            Uint size_live = (args.size() << 10) | Live.get();
            Label fallback = a.new_label();

            a.bind(fallback);
            mov_arg(ARG4, Name);
            mov_imm(ARG5, size_live);
            embed_vararg_rodata(args, ARG6, 0);
            fragment_call(ga->get_create_native_record_shared());
            if (Fail.get() != 0) {
                a.je(resolve_beam_label(Fail));
                a.jmp(next);
            } else {
                a.jne(next);

                emit_raise_exception(entry, nullptr);
            }

            return fallback;
        }));

        a.long_().lea(ARG3, x86::qword_ptr(HTOP, ERTS_SINT32_MAX));
        patch->gc_offset = a.offset() - 4;
        a.cmp(ARG3, E);
        a.ja(cold_fragment([&]() {
            Label garbage_collect = a.new_label();

            a.bind(garbage_collect);

            mov_imm(ARG4, Live.get());
            fragment_call(ga->get_garbage_collect());

            /* The definition may have changed if the GC scheduled us out,
             * retry. Note that this means we cannot use the register cache
             * at all in this instruction. */
            a.jmp(entry);

            return garbage_collect;
        }));

        auto literal_tag = 0;
#ifdef TAG_LITERAL_PTR
        literal_tag += TAG_LITERAL_PTR;
#endif

        /* Load default value array and definition pointer. Both are known to
         * be literals. */
        a.mov(ARG1,
              x86::qword_ptr(RET,
                             sizeof(Eterm) - TAG_PRIMARY_LIST - literal_tag));
        a.mov(RET, x86::qword_ptr(RET, -TAG_PRIMARY_LIST - literal_tag));

        /* Store the header word. */
        ERTS_CT_ASSERT(MAX_ARITYVAL < (ERTS_UINT32_MAX >> _HEADER_ARITY_OFFS));
        a.mov(x86::qword_ptr(HTOP), imm(ERTS_SINT32_MAX));
        patch->header_offset = a.offset() - 4;

        /* Store the definition pointer */
        a.mov(x86::qword_ptr(HTOP, sizeof(Eterm)), RET);

        /* Create the result term before copying, as we pre-bump HTOP to
         * simplify iteration. */
        a.lea(RET, x86::qword_ptr(HTOP, TAG_PRIMARY_BOXED));

        /* Copy all the default values, THE_NON_VALUE and all. This is a bit of
         * redundant work but it ought to be faster than than the fallback. */
        a.mov(ARG2d, imm(ERTS_SINT32_MAX));
        patch->copy_offset = a.offset() - 4;

        a.lea(HTOP, x86::qword_ptr(HTOP, ARG2, 3, sizeof(Eterm[2])));
        a.lea(ARG1,
              x86::qword_ptr(ARG1,
                             ARG2,
                             3,
                             sizeof(Eterm[2]) - TAG_PRIMARY_BOXED -
                                     literal_tag));
        a.neg(ARG2);
        a.jz(next);

        Label repeat = a.new_label();
        a.bind(repeat);
        {
            a.mov(ARG3, x86::qword_ptr(ARG1, ARG2, 3));
            a.mov(x86::qword_ptr(HTOP, ARG2, 3), ARG3);
            a.inc(ARG2);
            a.short_().jnz(repeat);
        }

        for (UWord i = 0; i < Size.get(); i += 2) {
            mov_arg(ARG1, args[i + 1]);

            /* Note that HTOP is already bumped, so the field offset is
             * backwards from the end. */
            a.mov(x86::qword_ptr(HTOP, ERTS_SINT32_MAX), ARG1);
            patch->fields.emplace_back(args[i].as<ArgAtom>().get(),
                                       a.offset() - 4);
        }
    }

    a.bind(next);
    mov_arg(Dst, RET);
}

void BeamModuleAssembler::emit_i_update_local_native_record(
        const ArgAtom &Hint,
        const ArgLiteral &Def,
        const ArgSource &Src,
        const ArgRegister &Dst,
        const ArgWord &Live,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    Eterm cons = beamfile_get_literal(beam, Def.get());
    Eterm def = CAR(list_val(cons));
    ErtsRecordDefinition *defp = (ErtsRecordDefinition *)tuple_val(def);
    int field_count = RECORD_DEF_FIELD_COUNT(defp);

    comment("name: %T", defp->name);

    /* Create the `updates` vector, mapping from positions to
     * values. */
    size_t argp = 0;
    std::vector<ArgVal> updates;
    updates.reserve(args.size());
    const Uint header_offset = RECORD_INST_SIZE(0);
    for (int i = 0; i < field_count; i++) {
        if (argp < args.size() &&
            args[argp].as<ArgAtom>().get() == defp->keys[i]) {
            updates.emplace(updates.end(), ArgWord(header_offset + i));
            updates.emplace(updates.end(), args[argp + 1]);
            argp += 2;
        }
    }

    /* Reuse the code for updating a tuple record. */
    Span<const ArgVal> Updates(updates.data(), updates.size());
    emit_update_any_record(Hint,
                           RECORD_INST_SIZE(field_count),
                           Src,
                           Dst,
                           Size,
                           Updates);
}

struct UpdateExternalRecordPatch : RecordPatchBase {
    Eterm module, name;

    size_t copy_offset, gc_offset, entry_offset;

    UpdateExternalRecordPatch(ErtsCodePatch *&patch_list, Eterm id)
            : RecordPatchBase(patch_list, id) {
        base.action = action<UpdateExternalRecordPatch>;
    }

    void load() {
        RecordPatchBase::load();

        poke<ErtsRecordEntry *>(entry_offset, entry);
    }

    void apply() {
        Eterm cons = entry->definitions[erts_staging_code_ix()];

        if (is_value(cons)) {
            auto definition = reinterpret_cast<ErtsRecordDefinition *>(
                    tuple_val(CAR(list_val(cons))));
            size_t count = RECORD_DEF_FIELD_COUNT(definition), found = 0;

            if (fields.size() <= count) {
                size_t i = 0;

                for (const auto &[key, where] : fields) {
                    if (key == definition->keys[i]) {
                        /* N.B. The offset is backwards from the end. */
                        poke<Sint32>(where, -((count - i) * sizeof(Eterm)));
                        found++;
                    }

                    i++;
                }
            }

            if (found == fields.size()) {
                /* All fields we've set out to update were present. */
                poke<Uint32>(gc_offset,
                             (RECORD_INST_SIZE(count) + S_RESERVED) *
                                     sizeof(Eterm));

                poke<Uint32>(
                        copy_offset,
                        (offsetof(ErtsRecordInstance, values) / sizeof(Eterm)) +
                                count);

                poke<Eterm>(sentinel_offset, cons);
                return;
            }
        }

        poke<Eterm>(sentinel_offset, RecordPatchBase::ForceFallbackSentinel);
    }
};

void BeamModuleAssembler::emit_i_update_external_native_record(
        const ArgLabel &Fail,
        const ArgLiteral &Name,
        const ArgSource &Src,
        const ArgRegister &Dst,
        const ArgWord &Live,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    auto name = beamfile_get_literal(beam, Name.get());

    auto patch = new (erts_alloc(ERTS_ALC_T_CODE_PATCH,
                                 sizeof(UpdateExternalRecordPatch)))
            UpdateExternalRecordPatch(patches, name);

    Label entry = a.new_label(), next = a.new_label();

    mov_arg(ARG1, Src);

    align_erlang_cp();
    a.bind(entry);
    {
        a.long_().movabs(ARG2, imm(ERTS_SINT64_MAX));
        patch->entry_offset = a.offset() - 8;

        /* This is not guarded by the jump, see the same code in
         * `GetExternalRecordElementsPatch` for details. */
        a.long_().movabs(RET, imm(ERTS_SINT64_MAX));
        patch->sentinel_offset = a.offset() - 8;
        a.cmp(RET,
              x86::qword_ptr(ARG2,
                             active_code_ix,
                             3,
                             offsetof(ErtsRecordEntry, definitions)));

        /* This is word-aligned as the required jump instructions differ in
         * length (5 unconditional, 6 conditional), and must therefore be
         * written atomically. */
        a.align(AlignMode::kCode, 8);
        patch->jump_offset = a.offset();
        a.long_().jne(cold_fragment([&]() {
            Label generic = a.new_label();
            a.bind(generic);

            a.mov(ARG3, ARG1);

            a.mov(ARG1, c_p);
            load_x_reg_array(ARG2);
            mov_arg(ARG4, Live);
            mov_imm(ARG5, args.size());
            embed_vararg_rodata(args, ARG6, 0);

            emit_enter_runtime<Update::eHeapAlloc | Update::eReductions>();

            runtime_call<Eterm (*)(Process *,
                                   Eterm *,
                                   Eterm,
                                   Uint,
                                   Uint,
                                   const Eterm *args),
                         erl_update_native_record>();

            emit_leave_runtime<Update::eHeapAlloc | Update::eReductions>();

            if (Fail.get() != 0) {
                emit_test_the_non_value(RET);
                a.je(resolve_beam_label(Fail));
                a.jmp(next);
            } else {
                emit_test_the_non_value(RET);
                a.jne(next);

                emit_raise_exception(entry, nullptr);
            }

            return generic;
        }));

        a.long_().lea(ARG3, x86::qword_ptr(HTOP, ERTS_SINT32_MAX));
        patch->gc_offset = a.offset() - 4;
        a.cmp(ARG3, E);
        a.ja(cold_fragment([&]() {
            Label garbage_collect = a.new_label();
            a.bind(garbage_collect);

            mov_arg(ArgXRegister(Live.get()), ARG1);
            mov_imm(ARG4, Live.get() + 1);
            fragment_call(ga->get_garbage_collect());
            mov_arg(ARG1, ArgXRegister(Live.get()));

            a.jmp(entry);

            return garbage_collect;
        }));

        a.and_(ARG1, imm(~TAG_PTR_MASK__));

        /* Create the result term before copying, as we pre-bump HTOP to
         * simplify iteration. */
        a.lea(RET, x86::qword_ptr(HTOP, TAG_PRIMARY_BOXED));

        /* Copy the entire thing including header word and definition, the
         * fields will be overwritten later.
         *
         * Note that this is always non-zero; aside from the header word and
         * definition we will never issue an update with zero fields. */
        a.mov(ARG2d, imm(ERTS_SINT32_MAX));
        patch->copy_offset = a.offset() - 4;

        a.lea(ARG1, x86::qword_ptr(ARG1, ARG2, 3));
        a.lea(HTOP, x86::qword_ptr(HTOP, ARG2, 3));
        a.neg(ARG2);

        Label repeat = a.new_label();
        a.bind(repeat);
        {
            a.mov(ARG3, x86::qword_ptr(ARG1, ARG2, 3));
            a.mov(x86::qword_ptr(HTOP, ARG2, 3), ARG3);
            a.inc(ARG2);
            a.short_().jnz(repeat);
        }

        for (UWord i = 0; i < Size.get(); i += 2) {
            mov_arg(ARG1, args[i + 1]);

            /* Note that HTOP is already bumped, so the field offset is
             * backwards from the end. */
            a.mov(x86::qword_ptr(HTOP, ERTS_SINT32_MAX), ARG1);
            patch->fields.emplace_back(args[i].as<ArgAtom>().get(),
                                       a.offset() - 4);
        }
    }

    a.bind(next);
    mov_arg(Dst, RET);
}

void BeamModuleAssembler::emit_i_update_native_record(
        const ArgLabel &Fail,
        const ArgSource &Src,
        const ArgRegister &Dst,
        const ArgWord &Live,
        const ArgWord &size,
        const Span<const ArgVal> &args) {
    mov_arg(ARG3, Src);
    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    mov_arg(ARG4, Live);
    mov_imm(ARG5, args.size());
    embed_vararg_rodata(args, ARG6, 0);

    emit_enter_runtime<Update::eHeapAlloc | Update::eReductions>();

    runtime_call<
            Eterm (*)(Process *, Eterm *, Eterm, Uint, Uint, const Eterm *args),
            erl_update_native_record>();

    emit_leave_runtime<Update::eHeapAlloc | Update::eReductions>();

    if (Fail.get() != 0) {
        emit_test_the_non_value(RET);
        a.je(resolve_beam_label(Fail));
    } else {
        Label next = a.new_label();

        emit_test_the_non_value(RET);
        a.short_().jne(next);

        emit_raise_exception();

        a.bind(next);
    }
    mov_arg(Dst, RET);
}

void BeamModuleAssembler::emit_get_record_field(const ArgLabel &Fail,
                                                const ArgRegister &Src,
                                                const ArgConstant &Id,
                                                const ArgAtom &Name,
                                                const ArgRegister &Dst) {
    a.mov(ARG1, c_p);
    mov_arg(ARG2, Src);
    mov_arg(ARG3, Id);
    mov_arg(ARG4, Name);

    emit_enter_runtime<Update::eHeapAlloc>();
    if (Id.isImmed()) {
        comment("local record");
        runtime_call<Eterm (*)(Process *, Eterm, Eterm, Eterm),
                     erl_get_local_record_field>();
    } else {
        comment("external record");
        runtime_call<Eterm (*)(Process *, Eterm, Eterm, Eterm),
                     erl_get_record_field>();
    }
    emit_leave_runtime<Update::eHeapAlloc>();

    if (Fail.get() != 0) {
        emit_test_the_non_value(RET);
        a.je(resolve_beam_label(Fail));
    } else {
        Label next = a.new_label();

        emit_test_the_non_value(RET);
        a.short_().jne(next);

        emit_raise_exception();

        a.bind(next);
    }

    mov_arg(Dst, RET);
}
