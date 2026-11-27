/*
 * VORTEX PROGRAMMING LANGUAGE - STATEMENT & DECLARATION PARSER (src/parser_stmt.c)
 */

#include "parser_internal.h"

static void parse_function_def(Compiler* c, bool is_unsafe) {
    Lexer* l = c->lex;
    int fn_decl_line = l->cur.line;
    next_token(l); /* consume fn */
    if (l->cur.type != TOK_IDENT) {
        fprintf(stderr, "[VORTEX ERROR] Expected function name at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    char fn_name[64];
    strncpy(fn_name, l->cur.text, sizeof(fn_name) - 1);
    fn_name[sizeof(fn_name) - 1] = '\0';
    next_token(l);

    UserFunc* uf = user_func_find(fn_name);
    bool uf_is_new = false;
    if (!uf) {
        if (g_user_func_count >= MAX_USER_FUNCS) {
            fprintf(stderr, "[VORTEX ERROR] Maximum number of user functions reached\n");
            vortex_error("");
        }
        uf = &g_user_funcs[g_user_func_count++];
        memset(uf, 0, sizeof(*uf));
        uf_is_new = true;
    } else {
        if (uf->inline_instrs) free(uf->inline_instrs);
        if (uf->inline_vregs) free(uf->inline_vregs);
        memset(uf, 0, sizeof(*uf));
    }
    strncpy(uf->name, fn_name, sizeof(uf->name) - 1);
    if (uf_is_new) {
        user_func_hash_insert(uf->name, (int)(uf - g_user_funcs));
    }
    uf->is_unsafe = is_unsafe;
    uf->ret_type = TYPE_I64;
    uf->ret_struct_id = -1;
    uf->decl_line = fn_decl_line;
    uf->called = (strcmp(fn_name, "main") == 0);

    vortex_parser_match(c, TOK_LPAREN);
    bool param_is_slice[MAX_CALL_ARGS];
    memset(param_is_slice, 0, sizeof(param_is_slice));
    while (l->cur.type != TOK_RPAREN && l->cur.type != TOK_EOF) {
        if (uf->param_count >= MAX_CALL_ARGS) {
            fprintf(stderr, "[VORTEX ERROR] Function '%s' exceeds %d parameters\n", fn_name, MAX_CALL_ARGS);
            vortex_error("");
        }
        strncpy(uf->param_names[uf->param_count], l->cur.text, 63);
        vortex_parser_match(c, TOK_IDENT);
        VortexType pt = TYPE_I64;
        int psid = -1;
        bool p_is_slice = false;
        if (l->cur.type == TOK_COLON) {
            next_token(l);
            p_is_slice = (l->cur.type == TOK_LBRACKET);
            pt = parse_type_name(l, &psid);
            if (pt == TYPE_UNKNOWN) pt = TYPE_I64;
        }
        uf->param_types[uf->param_count] = pt;
        uf->param_struct_ids[uf->param_count] = psid;
        param_is_slice[uf->param_count] = p_is_slice;
        uf->param_count++;
        if (l->cur.type == TOK_COMMA) next_token(l);
    }
    vortex_parser_match(c, TOK_RPAREN);

    bool explicit_ret_type = false;
    if (l->cur.type == TOK_ARROW) {
        next_token(l);
        int rsid = -1;
        VortexType rt = parse_type_name(l, &rsid);
        if (rt != TYPE_UNKNOWN) {
            uf->ret_type = rt;
            uf->ret_struct_id = rsid;
            explicit_ret_type = true;
        }
    }

    IRBuffer fn_ir;
    ir_init(&fn_ir);

    SymbolTable* fn_st = (SymbolTable*)calloc(1, sizeof(SymbolTable));
    if (!fn_st) {
        ir_free(&fn_ir);
        vortex_error("[VORTEX PANIC] Out of memory allocating function SymbolTable");
    }

    Compiler fn_comp;
    memset(&fn_comp, 0, sizeof(fn_comp));
    fn_comp.lex = c->lex;
    fn_comp.st = fn_st;
    fn_comp.j = c->j;
    fn_comp.ir = &fn_ir;
    fn_comp.last_struct_id = -1;
    fn_comp.unsafe_depth = is_unsafe ? 1 : 0;
    fn_comp.current_fn_is_unsafe = is_unsafe;
    fn_comp.in_function = true;

    for (int p = 0; p < uf->param_count; p++) {
        VortexType pt = uf->param_types[p];
        Symbol* ps = vortex_symtab_add(fn_st, uf->param_names[p], pt, true);
        ps->struct_id = uf->param_struct_ids[p];
        ps->vreg = ir_new_vreg(&fn_ir, pt);
        if (param_is_slice[p] &&
            (pt == TYPE_PTR_I64 || pt == TYPE_PTR_I32 || pt == TYPE_PTR_F32 || pt == TYPE_PTR_STRUCT)) {
            ps->is_slice_ptr = true;
            fn_ir.vregs[ps->vreg].is_slice_ptr = true;
        }
        ir_emit_load_param(&fn_ir, ps->vreg, p);
    }

    vortex_parser_match(&fn_comp, TOK_LBRACE);
    bool fn_had_error = false;
    int err_before = g_diag_error_count;
    if (g_ultrasafe_level >= 3) {
        bool prev_rec = g_recover_active;
        jmp_buf prev_rec_jmp;
        memcpy(&prev_rec_jmp, &g_recover_jmp, sizeof(prev_rec_jmp));
        g_recover_active = true;
        int recovery_steps = 0;
        while (fn_comp.lex->cur.type != TOK_RBRACE && fn_comp.lex->cur.type != TOK_EOF && recovery_steps < 512) {
            if (setjmp(g_recover_jmp) == 0) {
                parse_statement(&fn_comp);
            } else {
                fn_had_error = true;
                recovery_steps++;
                fn_comp.unsafe_depth = is_unsafe ? 1 : 0;
                fn_comp.loop_depth = 0;
                vortex_lexer_sync(fn_comp.lex, true);
            }
        }
        g_recover_active = prev_rec;
        memcpy(&g_recover_jmp, &prev_rec_jmp, sizeof(prev_rec_jmp));
    } else {
        while (fn_comp.lex->cur.type != TOK_RBRACE && fn_comp.lex->cur.type != TOK_EOF) {
            parse_statement(&fn_comp);
        }
    }
    vortex_parser_match(&fn_comp, TOK_RBRACE);

    /* Record if function returns a reference */
    for (int i = 0; i < fn_ir.count; i++) {
        if (fn_ir.instrs[i].op == IR_RET && fn_ir.instrs[i].src1 != VREG_NONE) {
            VReg rv = fn_ir.instrs[i].src1;
            if (rv >= 0 && rv < fn_ir.vreg_count && fn_ir.vregs[rv].is_ref) {
                uf->returns_ref = true;
            }
        }
    }

    if (fn_ir.count == 0 || fn_ir.instrs[fn_ir.count - 1].op != IR_RET) {
        ir_emit_ret(&fn_ir, VREG_NONE, TYPE_VOID);
    }
    fn_ir.param_count = uf->param_count;
    strncpy(fn_ir.func_name, uf->name, sizeof(fn_ir.func_name) - 1);
    fn_ir.func_name[sizeof(fn_ir.func_name) - 1] = '\0';

    {
        bool saved_rec = g_recover_active;
        g_recover_active = (g_ultrasafe_level >= 3);
        ir_region_verify(&fn_ir);
        g_recover_active = saved_rec;
    }

    if (g_ultrasafe_level >= 1) {
        vortex_ultrasafe_audit_ir(&fn_ir, fn_st, uf->name, explicit_ret_type ? uf->ret_type : TYPE_VOID, fn_decl_line);
    }

    if (g_diag_error_count > err_before) {
        fn_had_error = true;
    }

    if (g_lsp_capture_errors || fn_had_error || g_check_only || g_diag_error_count > 0) {
        uf->code_offset = 0;
        free(fn_st);
        ir_free(&fn_ir);
        return;
    }

    /* Level 2+ runtime recursion depth guard for functions making user calls */
    if (g_ultrasafe_level >= 2 && !is_unsafe) {
        bool has_user_call = false;
        int ret_count = 0;
        for (int i = 0; i < fn_ir.count; i++) {
            if (fn_ir.instrs[i].op == IR_CALL_USER) has_user_call = true;
            if (fn_ir.instrs[i].op == IR_RET) ret_count++;
        }
        int need = fn_ir.count + 5 + ret_count * 2;
        if (has_user_call && need > fn_ir.capacity) {
            int ncap = fn_ir.capacity > 0 ? fn_ir.capacity : 4096;
            while (ncap < need) ncap *= 2;
            IRInstruction* nb = (IRInstruction*)realloc(fn_ir.instrs, (size_t)ncap * sizeof(IRInstruction));
            if (nb) {
                fn_ir.instrs = nb;
                fn_ir.capacity = ncap;
            }
        }
        if (has_user_call && need <= fn_ir.capacity) {
            VReg v_p1 = ir_new_vreg(&fn_ir, TYPE_I64);
            VReg v_fn = ir_new_vreg(&fn_ir, TYPE_STR);
            VReg v_fl = ir_new_vreg(&fn_ir, TYPE_STR);
            VReg v_ln = ir_new_vreg(&fn_ir, TYPE_I64);
            int entry_start = 0;
            while (entry_start < fn_ir.count && fn_ir.instrs[entry_start].op == IR_LOAD_PARAM) {
                entry_start++;
            }
            const char* fn_intern = vortex_intern_cstr(uf->name);
            const char* fl_intern = vortex_intern_cstr(g_current_filename ? g_current_filename : "<input>");
            IRInstruction enter_ins[5];
            for (int k = 0; k < 5; k++) {
                memset(&enter_ins[k], 0, sizeof(IRInstruction));
                enter_ins[k].dest = VREG_NONE;
                enter_ins[k].src1 = VREG_NONE;
                enter_ins[k].src2 = VREG_NONE;
                enter_ins[k].src3 = VREG_NONE;
                enter_ins[k].src4 = VREG_NONE;
                enter_ins[k].line = fn_decl_line;
                enter_ins[k].file = fl_intern;
            }
            enter_ins[0].op = IR_CONST_I64; enter_ins[0].dest = v_p1; enter_ins[0].imm = 1; enter_ins[0].type = TYPE_I64;
            enter_ins[1].op = IR_CONST_I64; enter_ins[1].dest = v_fn; enter_ins[1].imm = (int64_t)fn_intern; enter_ins[1].type = TYPE_STR;
            enter_ins[2].op = IR_CONST_I64; enter_ins[2].dest = v_fl; enter_ins[2].imm = (int64_t)fl_intern; enter_ins[2].type = TYPE_STR;
            enter_ins[3].op = IR_CONST_I64; enter_ins[3].dest = v_ln; enter_ins[3].imm = fn_decl_line; enter_ins[3].type = TYPE_I64;
            enter_ins[4].op = IR_CALL; enter_ins[4].dest = VREG_NONE; enter_ins[4].func_ptr = (void*)vortex_ultrasafe_check_recursion;
            enter_ins[4].arg_count = 4;
            enter_ins[4].args[0] = v_p1; enter_ins[4].args[1] = v_fn; enter_ins[4].args[2] = v_fl; enter_ins[4].args[3] = v_ln;
            enter_ins[4].type = TYPE_VOID;
            memmove(&fn_ir.instrs[entry_start + 5], &fn_ir.instrs[entry_start],
                    (size_t)(fn_ir.count - entry_start) * sizeof(IRInstruction));
            memcpy(&fn_ir.instrs[entry_start], enter_ins, sizeof(enter_ins));
            fn_ir.count += 5;

            for (int i = entry_start + 5; i < fn_ir.count; i++) {
                if (fn_ir.instrs[i].op == IR_RET && fn_ir.count + 2 <= fn_ir.capacity) {
                    VReg v_m1 = ir_new_vreg(&fn_ir, TYPE_I64);
                    IRInstruction exit_ins[2];
                    for (int k = 0; k < 2; k++) {
                        memset(&exit_ins[k], 0, sizeof(IRInstruction));
                        exit_ins[k].dest = VREG_NONE;
                        exit_ins[k].src1 = VREG_NONE;
                        exit_ins[k].src2 = VREG_NONE;
                        exit_ins[k].src3 = VREG_NONE;
                        exit_ins[k].src4 = VREG_NONE;
                        exit_ins[k].line = fn_ir.instrs[i].line > 0 ? fn_ir.instrs[i].line : fn_decl_line;
                        exit_ins[k].file = fl_intern;
                    }
                    exit_ins[0].op = IR_CONST_I64; exit_ins[0].dest = v_m1; exit_ins[0].imm = -1; exit_ins[0].type = TYPE_I64;
                    exit_ins[1].op = IR_CALL; exit_ins[1].dest = VREG_NONE; exit_ins[1].func_ptr = (void*)vortex_ultrasafe_check_recursion;
                    exit_ins[1].arg_count = 4;
                    exit_ins[1].args[0] = v_m1; exit_ins[1].args[1] = v_fn; exit_ins[1].args[2] = v_fl; exit_ins[1].args[3] = v_ln;
                    exit_ins[1].type = TYPE_VOID;
                    memmove(&fn_ir.instrs[i + 2], &fn_ir.instrs[i],
                            (size_t)(fn_ir.count - i) * sizeof(IRInstruction));
                    memcpy(&fn_ir.instrs[i], exit_ins, sizeof(exit_ins));
                    fn_ir.count += 2;
                    i += 2;
                }
            }
        }
    }

    /* Pillar #1: Capture unoptimized small non-recursive function IR for caller inlining */
    if (!is_unsafe && !uf->returns_ref && fn_ir.count > 0 && fn_ir.count <= 32 && fn_ir.vreg_count <= 256) {
        bool ok_inline = true;
        for (int i = 0; i < fn_ir.count; i++) {
            IROpcode op = fn_ir.instrs[i].op;
            if (op == IR_RAW_RIEMANN || op == IR_VEC_LOOP || op == IR_VEC_ARR_OP) {
                ok_inline = false;
                break;
            }
            if (op == IR_CALL_USER && strcmp(fn_ir.instrs[i].func_name, uf->name) == 0) {
                ok_inline = false;
                break;
            }
        }
        if (ok_inline) {
            uf->inline_instrs = malloc((size_t)fn_ir.count * sizeof(IRInstruction));
            uf->inline_vregs = malloc((size_t)fn_ir.vreg_count * sizeof(VRegInfo));
            if (uf->inline_instrs && uf->inline_vregs) {
                memcpy(uf->inline_instrs, fn_ir.instrs, (size_t)fn_ir.count * sizeof(IRInstruction));
                memcpy(uf->inline_vregs, fn_ir.vregs, (size_t)fn_ir.vreg_count * sizeof(VRegInfo));
                uf->can_inline = true;
                uf->inline_ir_count = fn_ir.count;
                uf->inline_vreg_count = fn_ir.vreg_count;
            } else {
                free(uf->inline_instrs);
                free(uf->inline_vregs);
                uf->inline_instrs = NULL;
                uf->inline_vregs = NULL;
            }
        }
    }

    ir_optimize_bounds_and_licm(&fn_ir);
    ir_loop_vectorize(&fn_ir);
    ir_liveness_analysis(&fn_ir);
    ir_linear_scan_allocate(&fn_ir);

    const VortexJITBackend* bk = vortex_get_backend();
    size_t skip_func_jmp = bk->emit_jmp_placeholder(c->j);
    uf->code_offset = c->j->size;

    jit_set_active_ir_frame(&fn_ir);
    int fn_frame_size = bk->compute_frame_size(fn_ir.total_spill_bytes);
    debug_info_set_current_func(uf->name);
    bk->emit_func_prologue(c->j, fn_frame_size);
    bk->ir_codegen(&fn_ir, c->j);
    bk->emit_func_epilogue(c->j, fn_frame_size);
    jit_set_active_ir_frame(NULL);
    debug_info_record_func(uf->name, uf->code_offset, c->j->size, fn_frame_size, false, g_current_filename, fn_decl_line);
    debug_info_set_current_func("__main__");

    bk->patch_jump(c->j, skip_func_jmp);
    free(fn_st);
    ir_free(&fn_ir);
}

static void parse_cfg_stmt(Compiler* c) {
    Lexer* l = c->lex;
    next_token(l);
    vortex_parser_match(c, TOK_LBRACKET);
    if (l->cur.type != TOK_IDENT || strcmp(l->cur.text, "cfg") != 0) {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected 'cfg' attribute inside '#[...]' at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    next_token(l);
    vortex_parser_match(c, TOK_LPAREN);
    if (l->cur.type != TOK_IDENT) {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected platform identifier in #[cfg(...)] at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    char pred[64];
    strncpy(pred, l->cur.text, sizeof(pred) - 1);
    pred[sizeof(pred) - 1] = '\0';
    next_token(l);
    vortex_parser_match(c, TOK_RPAREN);
    vortex_parser_match(c, TOK_RBRACKET);
    if (!vortex_eval_cfg_predicate(pred)) {
        vortex_skip_cfg_item(c);
        return;
    }
    parse_statement(c);
}

static void parse_import_stmt(Compiler* c) {
    Lexer* l = c->lex;
    int imp_line = l->cur.line;
    int imp_col = l->cur.col;
    next_token(l);
    if (c->in_function || c->unsafe_depth > 0) {
        fprintf(stderr, "[VORTEX ERROR] 'import' is only allowed at top level at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", imp_line, imp_col);
        vortex_error("");
    }
    char spec[256];
    vortex_parse_import_spec(l, spec, sizeof(spec));
    char resolved[512];
    if (!vortex_resolve_import(spec, g_current_filename, resolved, sizeof(resolved))) {
        fprintf(stderr, "[VORTEX ERROR] Cannot resolve module import '%s' at %s:%d:%d\n",
                spec, g_current_filename ? g_current_filename : "<input>", imp_line, imp_col);
        vortex_error("");
    }
    if (vortex_has_imported_path(resolved)) {
        return;
    }
    vortex_mark_imported_path(resolved);
    size_t mlen = 0;
    char* msrc = vortex_read_file_alloc(resolved, &mlen);
    if (!msrc) {
        fprintf(stderr, "[VORTEX ERROR] Failed to read imported module '%s' at %s:%d:%d\n",
                resolved, g_current_filename ? g_current_filename : "<input>", imp_line, imp_col);
        vortex_error("");
    }
    Lexer sub_lex;
    lexer_init(&sub_lex, msrc);
    Lexer* saved_lex = c->lex;
    const char* saved_file = g_current_filename;
    c->lex = &sub_lex;
    g_current_filename = vortex_intern_cstr(resolved);
    while (c->lex->cur.type != TOK_EOF) {
        parse_statement(c);
    }
    g_current_filename = saved_file;
    c->lex = saved_lex;
    free(msrc);
}

static void parse_extern_stmt(Compiler* c) {
    Lexer* l = c->lex;
    next_token(l);
    if (l->cur.type != TOK_STRING) {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected library string after 'extern' at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    char lib_name[128];
    strncpy(lib_name, l->cur.text, sizeof(lib_name) - 1);
    lib_name[sizeof(lib_name) - 1] = '\0';
    next_token(l);

    if (l->cur.type != TOK_FN) {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected 'fn' after extern library name at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    next_token(l);

    if (l->cur.type != TOK_IDENT) {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected function name at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    char fn_name[64];
    strncpy(fn_name, l->cur.text, sizeof(fn_name) - 1);
    fn_name[sizeof(fn_name) - 1] = '\0';
    next_token(l);

    vortex_parser_match(c, TOK_LPAREN);
    char param_names[MAX_CALL_ARGS][64];
    VortexType param_types[MAX_CALL_ARGS];
    bool param_nullable[MAX_CALL_ARGS];
    int param_count = 0;
    bool is_variadic = false;

    while (l->cur.type != TOK_RPAREN && l->cur.type != TOK_EOF) {
        if (l->cur.type == TOK_DOTDOT) {
            next_token(l);
            if (l->cur.type == TOK_DOT) next_token(l);
            is_variadic = true;
            if (l->cur.type == TOK_COMMA) next_token(l);
            continue;
        }
        if (param_count >= MAX_CALL_ARGS) {
            fprintf(stderr, "[VORTEX ERROR] Extern function '%s' exceeds %d parameters at %s:%d:%d\n",
                    fn_name, MAX_CALL_ARGS, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        if (l->cur.type != TOK_IDENT) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected parameter name at %s:%d:%d\n",
                    g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        strncpy(param_names[param_count], l->cur.text, sizeof(param_names[0]) - 1);
        param_names[param_count][sizeof(param_names[0]) - 1] = '\0';
        next_token(l);
        vortex_parser_match(c, TOK_COLON);
        int st_id = -1;
        bool nullable = false;
        VortexType pt = vortex_parse_type_spec_full(l, &st_id, &nullable);
        if (pt == TYPE_UNKNOWN) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Unknown parameter type in extern function '%s' at %s:%d:%d\n",
                    fn_name, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        param_types[param_count] = pt;
        param_nullable[param_count] = nullable;
        param_count++;
        if (l->cur.type == TOK_COMMA) next_token(l);
    }
    vortex_parser_match(c, TOK_RPAREN);

    VortexType ret_type = TYPE_VOID;
    int ret_st_id = -1;
    if (l->cur.type == TOK_ARROW) {
        next_token(l);
        ret_type = parse_type_name(l, &ret_st_id);
        if (ret_type == TYPE_UNKNOWN) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Unknown return type in extern function '%s' at %s:%d:%d\n",
                    fn_name, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
    }
    vortex_parser_match(c, TOK_SEMI);

    void* fn_ptr = NULL;
    if (vortex_is_cross_target()) {
        static uintptr_t cross_ffi_stub = 0x100000ULL;
        cross_ffi_stub += 0x100ULL;
        fn_ptr = (void*)cross_ffi_stub;
    } else {
        void* lib_handle = vortex_load_dynlib(lib_name);
        if (!lib_handle) {
#ifdef _WIN32
            fprintf(stderr, "[VORTEX FFI ERROR] Cannot load dynamic library '%s' for extern function '%s' at %s:%d (Win32 error %lu)\n",
                    lib_name, fn_name, g_current_filename ? g_current_filename : "<input>", l->cur.line, (unsigned long)GetLastError());
#else
            const char* dl_err = dlerror();
            fprintf(stderr, "[VORTEX FFI ERROR] Cannot load dynamic library '%s' for extern function '%s' at %s:%d (%s)\n",
                    lib_name, fn_name, g_current_filename ? g_current_filename : "<input>", l->cur.line, dl_err ? dl_err : "unknown dlopen error");
#endif
            vortex_error("");
        }
        fn_ptr = vortex_resolve_symbol(lib_handle, fn_name);
        if (!fn_ptr) {
#ifdef _WIN32
            fprintf(stderr, "[VORTEX FFI ERROR] Symbol '%s' not found in dynamic library '%s' at %s:%d (Win32 error %lu)\n",
                    fn_name, lib_name, g_current_filename ? g_current_filename : "<input>", l->cur.line, (unsigned long)GetLastError());
#else
            const char* dl_err = dlerror();
            fprintf(stderr, "[VORTEX FFI ERROR] Symbol '%s' not found in dynamic library '%s' at %s:%d (%s)\n",
                    fn_name, lib_name, g_current_filename ? g_current_filename : "<input>", l->cur.line, dl_err ? dl_err : "unknown dlsym error");
#endif
            vortex_error("");
        }
    }

    FFIFunc* ff = ffi_func_find(fn_name);
    if (!ff) {
        if (g_ffi_func_count >= MAX_FFI_FUNCS) {
            fprintf(stderr, "[VORTEX ERROR] Maximum extern functions (%d) exceeded\n", MAX_FFI_FUNCS);
            vortex_error("");
        }
        ff = &g_ffi_funcs[g_ffi_func_count++];
    }
    memset(ff, 0, sizeof(*ff));
    strncpy(ff->lib_name, lib_name, sizeof(ff->lib_name) - 1);
    strncpy(ff->fn_name, fn_name, sizeof(ff->fn_name) - 1);
    ff->func_ptr = fn_ptr;
    ff->ret_type = ret_type;
    ff->param_count = param_count;
    ff->is_variadic = is_variadic;
    for (int p = 0; p < param_count; p++) {
        ff->param_types[p] = param_types[p];
        ff->param_nullable[p] = param_nullable[p];
        strncpy(ff->param_names[p], param_names[p], sizeof(param_names[0]) - 1);
    }
}

static void parse_struct_stmt(Compiler* c) {
    Lexer* l = c->lex;
    next_token(l);
    if (l->cur.type != TOK_IDENT) {
        fprintf(stderr, "[VORTEX ERROR] Expected struct name after 'struct' at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    char sname[64];
    strncpy(sname, l->cur.text, sizeof(sname) - 1);
    sname[sizeof(sname) - 1] = '\0';
    next_token(l);

    VortexStructType* st = struct_type_find(sname);
    bool st_is_new = false;
    if (!st) {
        if (g_struct_type_count >= MAX_STRUCT_TYPES) {
            fprintf(stderr, "[VORTEX ERROR] Maximum number of struct types reached\n");
            vortex_error("");
        }
        st = &g_struct_types[g_struct_type_count++];
        st_is_new = true;
    }
    memset(st, 0, sizeof(*st));
    strncpy(st->name, sname, sizeof(sname) - 1);
    st->id = (int)(st - g_struct_types);
    if (st_is_new) {
        struct_type_hash_insert(st->name, st->id);
    }

    vortex_parser_match(c, TOK_LBRACE);
    int current_offset = 0;
    int max_align = 4;

    while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
        if (l->cur.type != TOK_IDENT) {
            fprintf(stderr, "[VORTEX ERROR] Expected field name in struct %s at %s:%d:%d\n",
                    sname, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        char fname[64];
        strncpy(fname, l->cur.text, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = '\0';
        next_token(l);

        vortex_parser_match(c, TOK_COLON);

        VortexType ftype = TYPE_I64;
        int fsize = 8;
        int falign = 8;
        int sub_sid = -1;

        if (l->cur.type == TOK_I64_KW) {
            ftype = TYPE_I64; fsize = 8; falign = 8;
            next_token(l);
        } else if (l->cur.type == TOK_F32_KW) {
            ftype = TYPE_F32; fsize = 4; falign = 4;
            next_token(l);
        } else if (l->cur.type == TOK_IDENT) {
            if (strcmp(l->cur.text, "i32") == 0 || strcmp(l->cur.text, "u32") == 0) {
                ftype = TYPE_I32; fsize = 4; falign = 4;
            } else if (strcmp(l->cur.text, "ptr") == 0) {
                ftype = TYPE_PTR; fsize = 8; falign = 8;
            } else if (strcmp(l->cur.text, "str") == 0) {
                ftype = TYPE_STR; fsize = 8; falign = 8;
            } else if (strcmp(l->cur.text, "bool") == 0 || strcmp(l->cur.text, "u64") == 0 ||
                       strcmp(l->cur.text, "usize") == 0 || strcmp(l->cur.text, "isize") == 0 ||
                       strcmp(l->cur.text, "i8") == 0 || strcmp(l->cur.text, "u8") == 0 ||
                       strcmp(l->cur.text, "i16") == 0 || strcmp(l->cur.text, "u16") == 0 ||
                       strcmp(l->cur.text, "char") == 0) {
                ftype = TYPE_I64; fsize = 8; falign = 8;
            } else {
                VortexStructType* sub_st = struct_type_find(l->cur.text);
                if (sub_st) {
                    ftype = TYPE_STRUCT; fsize = sub_st->size; falign = sub_st->align; sub_sid = sub_st->id;
                } else {
                    ftype = TYPE_I64; fsize = 8; falign = 8;
                }
            }
            next_token(l);
        } else if (l->cur.type == TOK_LBRACKET) {
            next_token(l);
            if (l->cur.type == TOK_F32_KW) ftype = TYPE_PTR_F32;
            else if (l->cur.type == TOK_IDENT && (strcmp(l->cur.text, "i32") == 0 || strcmp(l->cur.text, "u32") == 0)) ftype = TYPE_PTR_I32;
            else if (l->cur.type == TOK_IDENT && struct_type_find(l->cur.text) != NULL) {
                VortexStructType* elem_st = struct_type_find(l->cur.text);
                ftype = TYPE_PTR_STRUCT;
                sub_sid = elem_st->id;
            } else {
                ftype = TYPE_PTR_I64;
            }
            fsize = 8; falign = 8;
            next_token(l);
            vortex_parser_match(c, TOK_RBRACKET);
        }

        if (falign > max_align) max_align = falign;
        current_offset = (current_offset + (falign - 1)) & ~(falign - 1);

        if (st->field_count >= MAX_FIELDS) {
            fprintf(stderr, "[VORTEX ERROR] Struct '%s' exceeds maximum fields (%d)\n", sname, MAX_FIELDS);
            vortex_error("");
        }
        VortexField* f = &st->fields[st->field_count++];
        strncpy(f->name, fname, sizeof(f->name) - 1);
        f->type = ftype;
        f->offset = current_offset;
        f->size = fsize;
        f->align = falign;
        f->struct_id = sub_sid;

        current_offset += fsize;

        if (l->cur.type == TOK_COMMA || l->cur.type == TOK_SEMI) {
            next_token(l);
        }
    }
    vortex_parser_match(c, TOK_RBRACE);
    if (l->cur.type == TOK_SEMI) next_token(l);

    st->align = max_align;
    st->size = (current_offset + (max_align - 1)) & ~(max_align - 1);
    if (st->size == 0) st->size = max_align;
}

static void parse_enum_stmt(Compiler* c) {
    Lexer* l = c->lex;
    next_token(l);
    if (l->cur.type != TOK_IDENT) {
        fprintf(stderr, "[VORTEX ERROR] Expected enum name at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    if (g_enum_type_count >= MAX_ENUM_TYPES) {
        vortex_error("[VORTEX ERROR] Maximum enum types exceeded");
    }
    VortexEnumType* en = enum_type_find(l->cur.text);
    if (!en) {
        en = &g_enum_types[g_enum_type_count++];
        memset(en, 0, sizeof(*en));
        strncpy(en->name, l->cur.text, sizeof(en->name) - 1);
    } else {
        en->variant_count = 0;
    }
    next_token(l);
    vortex_parser_match(c, TOK_LBRACE);

    int64_t next_val = 0;
    while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
        if (l->cur.type != TOK_IDENT) {
            fprintf(stderr, "[VORTEX ERROR] Expected enum variant name in '%s' at %s:%d:%d\n",
                    en->name, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        if (en->variant_count >= MAX_ENUM_VARIANTS) {
            vortex_error("[VORTEX ERROR] Maximum enum variants exceeded");
        }
        VortexEnumVariant* v = &en->variants[en->variant_count++];
        strncpy(v->name, l->cur.text, sizeof(v->name) - 1);
        v->name[sizeof(v->name) - 1] = '\0';
        next_token(l);
        if (l->cur.type == TOK_EQ) {
            next_token(l);
            bool neg = false;
            if (l->cur.type == TOK_MINUS) {
                neg = true;
                next_token(l);
            }
            if (l->cur.type != TOK_INT) {
                vortex_error("[VORTEX ERROR] Enum variant value must be an integer constant");
            }
            next_val = neg ? -l->cur.int_val : l->cur.int_val;
            next_token(l);
        }
        v->value = next_val++;
        if (l->cur.type == TOK_COMMA || l->cur.type == TOK_SEMI) next_token(l);
    }
    vortex_parser_match(c, TOK_RBRACE);
    if (l->cur.type == TOK_SEMI) next_token(l);
}

static void consume_stmt_end(Compiler* c) {
    if (c->in_match_arm && (c->lex->cur.type == TOK_COMMA || c->lex->cur.type == TOK_RBRACE)) {
        return;
    }
    vortex_parser_match(c, TOK_SEMI);
}

static void parse_let_stmt(Compiler* c, int stmt_line, int stmt_col) {
    Lexer* l = c->lex;
    next_token(l);
    bool is_mut = false;
    if (l->cur.type == TOK_MUT) {
        is_mut = true;
        next_token(l);
    }
    if (l->cur.type != TOK_IDENT) {
        fprintf(stderr, "[VORTEX ERROR] Expected identifier after 'let' at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    char name[64];
    strncpy(name, l->cur.text, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    next_token(l);

    VortexType explicit_type = TYPE_UNKNOWN;
    int explicit_struct_id = -1;
    if (l->cur.type == TOK_COLON) {
        next_token(l);
        explicit_type = parse_type_name(l, &explicit_struct_id);
    }

    vortex_parser_match(c, TOK_EQ);
    c->last_struct_id = -1;
    VReg v_expr;
    VortexType expr_type = vortex_parse_expr(c, &v_expr);
    consume_stmt_end(c);

    if (explicit_type != TYPE_UNKNOWN && vortex_is_pointer_type(explicit_type) && vortex_is_scalar_int_type(expr_type)) {
        vortex_require_unsafe(c, "Integer-to-pointer assignment", stmt_line, stmt_col);
    }

    if (g_ultrasafe_level >= 1 && name[0] != '_' && vortex_symtab_find(c->st, name) != NULL) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Variable '%s' shadows an existing binding in scope", name);
        vortex_diag_add(VDIAG_WARNING, "W103", g_current_filename, stmt_line, stmt_col, msg,
                        "Rename the inner variable to avoid accidental shadowing.");
    }

    VortexType final_type = (explicit_type != TYPE_UNKNOWN) ? explicit_type : expr_type;
    Symbol* sym = vortex_symtab_add(c->st, name, final_type, is_mut);
    sym->decl_line = stmt_line;
    sym->decl_col = stmt_col;
    sym->used = false;
    sym->reassigned = false;
    sym->struct_id = (explicit_struct_id >= 0) ? explicit_struct_id : c->last_struct_id;
    sym->vreg = ir_new_vreg(c->ir, final_type);

    if (v_expr >= 0 && v_expr < c->ir->vreg_count) {
        VRegInfo* src_vi = &c->ir->vregs[v_expr];
        if (src_vi->is_ref) {
            sym->is_ref = true;
            sym->region_id = src_vi->region_id;
            sym->region_depth = src_vi->region_depth;
            vortex_set_vreg_region(c->ir, sym->vreg, true, src_vi->region_id, src_vi->region_depth);
        }
        if (src_vi->region_id > 0 && !src_vi->is_ref &&
            c->current_region_depth > 0 &&
            src_vi->region_id == c->current_region_id) {
            sym->is_region_mark = true;
            sym->region_id = src_vi->region_id;
            sym->region_depth = src_vi->region_depth;
            strncpy(c->region_stack[c->current_region_depth].mark_name, name, 63);
        }
        if (src_vi->is_slice_ptr) {
            sym->is_slice_ptr = true;
            sym->len_vreg = src_vi->len_vreg;
            sym->known_len = src_vi->known_len;
            if (sym->len_vreg == VREG_NONE && sym->known_len >= 0 &&
                (final_type == TYPE_PTR_I64 || final_type == TYPE_PTR_F32 || final_type == TYPE_PTR_I32)) {
                int64_t esz = (final_type == TYPE_PTR_I64) ? 8 : 4;
                sym->known_len /= esz;
            }
            c->ir->vregs[sym->vreg].is_slice_ptr = true;
            c->ir->vregs[sym->vreg].len_vreg = sym->len_vreg;
            c->ir->vregs[sym->vreg].known_len = sym->known_len;
        }
    }
    ir_emit_mov(c->ir, sym->vreg, v_expr);
}

static void parse_parallel_for_stmt(Compiler* c) {
    Lexer* l = c->lex;
    next_token(l);
    char var_name[64];
    strncpy(var_name, l->cur.text, sizeof(var_name) - 1);
    var_name[sizeof(var_name) - 1] = '\0';
    vortex_parser_match(c, TOK_IDENT);
    vortex_parser_match(c, TOK_IN);

    VReg v_start;
    vortex_parse_expr(c, &v_start);
    vortex_parser_match(c, TOK_DOTDOT);

    VReg v_end;
    vortex_parse_expr(c, &v_end);

    int num_captures = c->st->count;
    VReg v_size = ir_new_vreg(c->ir, TYPE_I64);
    ir_emit_const_i64(c->ir, v_size, (num_captures + 1) * 8);

    VReg v_ctx = ir_new_vreg(c->ir, TYPE_I64);
    VReg call_args1[1] = { v_size };
    ir_emit_call(c->ir, v_ctx, (void*)arena_alloc_fast, call_args1, 1, TYPE_I64);

    int store_ip_base = c->ir->count;
    for (int s = 0; s < num_captures; s++) {
        Symbol* parent_sym = &c->st->symbols[s];
        ir_emit_store_offset(c->ir, v_ctx, s * 8, parent_sym->vreg, parent_sym->type);
    }

    IRBuffer worker_ir;
    ir_init(&worker_ir);
    SymbolTable* worker_st = (SymbolTable*)calloc(1, sizeof(SymbolTable));
    if (!worker_st) {
        ir_free(&worker_ir);
        vortex_error("[VORTEX PANIC] Out of memory allocating worker SymbolTable");
    }

    Compiler worker_c;
    memset(&worker_c, 0, sizeof(worker_c));
    worker_c.lex = c->lex;
    worker_c.st = worker_st;
    worker_c.j = c->j;
    worker_c.ir = &worker_ir;
    worker_c.last_struct_id = -1;
    worker_c.unsafe_depth = c->unsafe_depth;
    worker_c.in_function = true;

    VReg v_w_ctx = ir_new_vreg(&worker_ir, TYPE_I64);
    ir_emit_load_param(&worker_ir, v_w_ctx, 0);

    VReg v_w_start = ir_new_vreg(&worker_ir, TYPE_I64);
    ir_emit_load_param(&worker_ir, v_w_start, 1);

    VReg v_w_end = ir_new_vreg(&worker_ir, TYPE_I64);
    ir_emit_load_param(&worker_ir, v_w_end, 2);

    int load_ip_base = worker_ir.count;
    for (int s = 0; s < num_captures; s++) {
        Symbol* parent_sym = &c->st->symbols[s];
        Symbol* ws = vortex_symtab_add(worker_st, parent_sym->name, parent_sym->type, parent_sym->is_mut);
        ws->struct_id = parent_sym->struct_id;
        ws->is_ref = parent_sym->is_ref;
        ws->is_slice_ptr = parent_sym->is_slice_ptr;
        ws->known_len = parent_sym->known_len;
        ws->region_depth = 0;
        ws->vreg = ir_new_vreg(&worker_ir, parent_sym->type);
        ir_emit_load_offset(&worker_ir, ws->vreg, v_w_ctx, s * 8, parent_sym->type);
    }

    Symbol* wi_sym = vortex_symtab_add(worker_st, var_name, TYPE_I64, true);
    wi_sym->vreg = ir_new_vreg(&worker_ir, TYPE_I64);
    ir_emit_mov(&worker_ir, wi_sym->vreg, v_w_start);

    int lbl_loop = ir_new_label(&worker_ir);
    int lbl_exit = ir_new_label(&worker_ir);

    ir_emit_label(&worker_ir, lbl_loop);
    VReg v_cmp = ir_new_vreg(&worker_ir, TYPE_I64);
    ir_emit_binop(&worker_ir, IR_LT, v_cmp, wi_sym->vreg, v_w_end, TYPE_I64);
    ir_emit_jz(&worker_ir, v_cmp, lbl_exit);

    vortex_parser_match(&worker_c, TOK_LBRACE);
    while (worker_c.lex->cur.type != TOK_RBRACE && worker_c.lex->cur.type != TOK_EOF) {
        parse_statement(&worker_c);
    }
    vortex_parser_match(&worker_c, TOK_RBRACE);

    VReg v_one = ir_new_vreg(&worker_ir, TYPE_I64);
    ir_emit_const_i64(&worker_ir, v_one, 1);
    ir_emit_binop(&worker_ir, IR_ADD, wi_sym->vreg, wi_sym->vreg, v_one, TYPE_I64);

    ir_emit_jmp(&worker_ir, lbl_loop);
    ir_emit_label(&worker_ir, lbl_exit);
    ir_emit_ret(&worker_ir, VREG_NONE, TYPE_VOID);

    for (int s = 0; s < num_captures; s++) {
        VReg wv = worker_ir.instrs[load_ip_base + s].dest;
        bool used = false;
        for (int k = load_ip_base + num_captures; k < worker_ir.count; k++) {
            IRInstruction* wi = &worker_ir.instrs[k];
            if (wi->dest == wv || wi->src1 == wv || wi->src2 == wv ||
                wi->src3 == wv || wi->src4 == wv) {
                used = true;
                break;
            }
            for (int a = 0; a < wi->arg_count; a++) {
                if (wi->args[a] == wv) { used = true; break; }
            }
            if (used) break;
            if (wi->op == IR_RAW_RIEMANN || wi->op == IR_VEC_LOOP || wi->op == IR_VEC_ARR_OP) {
                if (wi->i_vreg == wv || wi->lim_vreg == wv || wi->acc_vreg == wv ||
                    wi->x_vreg == wv || wi->step_vreg == wv) {
                    used = true;
                    break;
                }
            }
        }
        if (!used) {
            c->ir->instrs[store_ip_base + s].op = IR_NOP;
            worker_ir.instrs[load_ip_base + s].op = IR_NOP;
        }
    }
    worker_ir.param_count = 3;

    ir_region_verify(&worker_ir);
    ir_optimize_bounds_and_licm(&worker_ir);
    ir_loop_vectorize(&worker_ir);
    ir_liveness_analysis(&worker_ir);
    ir_linear_scan_allocate(&worker_ir);

    const VortexJITBackend* bk = vortex_get_backend();
    size_t skip_worker = bk->emit_jmp_placeholder(c->j);
    void* worker_fn_addr = (void*)(c->j->code + c->j->size);

    jit_set_active_ir_frame(&worker_ir);
    int worker_frame_size = bk->compute_frame_size(worker_ir.total_spill_bytes);
    bk->emit_func_prologue(c->j, worker_frame_size);
    bk->ir_codegen(&worker_ir, c->j);
    bk->emit_func_epilogue(c->j, worker_frame_size);
    jit_set_active_ir_frame(NULL);

    bk->patch_jump(c->j, skip_worker);
    free(worker_st);
    ir_free(&worker_ir);

    VReg v_fn = ir_new_vreg(c->ir, TYPE_I64);
    ir_emit_const_i64(c->ir, v_fn, (int64_t)worker_fn_addr);

    VReg v_ret = ir_new_vreg(c->ir, TYPE_VOID);
    VReg dispatch_args[4] = { v_fn, v_ctx, v_start, v_end };
    ir_emit_call(c->ir, v_ret, (void*)vortex_parallel_for_dispatch, dispatch_args, 4, TYPE_VOID);
}

static void parse_for_stmt(Compiler* c) {
    Lexer* l = c->lex;
    next_token(l);
    char var_name[64];
    strncpy(var_name, l->cur.text, sizeof(var_name) - 1);
    var_name[sizeof(var_name) - 1] = '\0';
    vortex_parser_match(c, TOK_IDENT);
    vortex_parser_match(c, TOK_IN);

    VReg v_start;
    vortex_parse_expr(c, &v_start);
    Symbol* i_sym = vortex_symtab_add(c->st, var_name, TYPE_I64, true);
    i_sym->vreg = ir_new_vreg(c->ir, TYPE_I64);
    ir_emit_mov(c->ir, i_sym->vreg, v_start);

    vortex_parser_match(c, TOK_DOTDOT);

    VReg v_end;
    vortex_parse_expr(c, &v_end);
    char limit_name[64];
    snprintf(limit_name, sizeof(limit_name), "_lim_%d", c->st->count);
    Symbol* lim_sym = vortex_symtab_add(c->st, limit_name, TYPE_I64, false);
    lim_sym->vreg = ir_new_vreg(c->ir, TYPE_I64);
    ir_emit_mov(c->ir, lim_sym->vreg, v_end);

    if (vortex_try_match_and_emit_unrolled_riemann(c, i_sym, lim_sym)) {
        return;
    }

    int lbl_start = ir_new_label(c->ir);
    int lbl_step = ir_new_label(c->ir);
    int lbl_exit = ir_new_label(c->ir);

    vortex_parser_match(c, TOK_LBRACE);
    ir_emit_label(c->ir, lbl_start);

    VReg v_cmp = ir_new_vreg(c->ir, TYPE_I64);
    ir_emit_binop(c->ir, IR_LT, v_cmp, i_sym->vreg, lim_sym->vreg, TYPE_I64);
    ir_emit_jz(c->ir, v_cmp, lbl_exit);

    if (c->loop_depth < MAX_LOOP_DEPTH) {
        c->loop_break_lbl[c->loop_depth] = lbl_exit;
        c->loop_cont_lbl[c->loop_depth] = lbl_step;
        c->loop_depth++;
    }
    int saved_loop_st_count = c->st->count;
    while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
        parse_statement(c);
    }
    if (c->loop_depth > 0) c->loop_depth--;
    vortex_parser_match(c, TOK_RBRACE);
    c->st->count = saved_loop_st_count;

    ir_emit_label(c->ir, lbl_step);
    VReg v_one = ir_new_vreg(c->ir, TYPE_I64);
    ir_emit_const_i64(c->ir, v_one, 1);
    ir_emit_binop(c->ir, IR_ADD, i_sym->vreg, i_sym->vreg, v_one, TYPE_I64);

    ir_emit_jmp(c->ir, lbl_start);
    ir_emit_label(c->ir, lbl_exit);
}

static void parse_while_stmt(Compiler* c) {
    Lexer* l = c->lex;
    next_token(l);
    int lbl_start = ir_new_label(c->ir);
    int lbl_exit = ir_new_label(c->ir);

    ir_emit_label(c->ir, lbl_start);

    VReg v_cond;
    vortex_parse_expr(c, &v_cond);
    ir_emit_jz(c->ir, v_cond, lbl_exit);

    vortex_parser_match(c, TOK_LBRACE);
    if (c->loop_depth < MAX_LOOP_DEPTH) {
        c->loop_break_lbl[c->loop_depth] = lbl_exit;
        c->loop_cont_lbl[c->loop_depth] = lbl_start;
        c->loop_depth++;
    }
    int saved_loop_st_count = c->st->count;
    while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
        parse_statement(c);
    }
    if (c->loop_depth > 0) c->loop_depth--;
    c->st->count = saved_loop_st_count;
    vortex_parser_match(c, TOK_RBRACE);

    ir_emit_jmp(c->ir, lbl_start);
    ir_emit_label(c->ir, lbl_exit);
}

static void parse_if_stmt(Compiler* c) {
    next_token(c->lex);
    VReg v_cond;
    vortex_parse_expr(c, &v_cond);
    int lbl_else = ir_new_label(c->ir);
    int lbl_end = ir_new_label(c->ir);

    ir_emit_jz(c->ir, v_cond, lbl_else);

    int saved_popped = c->popped_region_count;
    int saved_depth = c->current_region_depth;
    int saved_rid = c->current_region_id;
    CompileRegion saved_stack[MAX_COMPILE_REGIONS];
    memcpy(saved_stack, c->region_stack, sizeof(saved_stack));
    bool saved_ret = c->block_returned;
    c->block_returned = false;

    vortex_parser_match(c, TOK_LBRACE);
    while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
        parse_statement(c);
    }
    vortex_parser_match(c, TOK_RBRACE);

    bool if_returned = c->block_returned;
    if (if_returned) {
        c->popped_region_count = saved_popped;
        c->current_region_depth = saved_depth;
        c->current_region_id = saved_rid;
        memcpy(c->region_stack, saved_stack, sizeof(saved_stack));
    }

    if (c->lex->cur.type == TOK_ELSE) {
        next_token(c->lex);
        ir_emit_jmp(c->ir, lbl_end);
        ir_emit_label(c->ir, lbl_else);

        int if_popped = c->popped_region_count;
        int if_depth = c->current_region_depth;
        int if_rid = c->current_region_id;
        CompileRegion if_stack[MAX_COMPILE_REGIONS];
        memcpy(if_stack, c->region_stack, sizeof(if_stack));

        if (!if_returned) {
            c->popped_region_count = saved_popped;
            c->current_region_depth = saved_depth;
            c->current_region_id = saved_rid;
            memcpy(c->region_stack, saved_stack, sizeof(saved_stack));
        }
        c->block_returned = false;

        if (c->lex->cur.type == TOK_IF) {
            parse_statement(c);
        } else {
            vortex_parser_match(c, TOK_LBRACE);
            while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
                parse_statement(c);
            }
            vortex_parser_match(c, TOK_RBRACE);
        }
        bool else_returned = c->block_returned;
        if (!if_returned && else_returned) {
            c->popped_region_count = if_popped;
            c->current_region_depth = if_depth;
            c->current_region_id = if_rid;
            memcpy(c->region_stack, if_stack, sizeof(if_stack));
        }
        c->block_returned = saved_ret || (if_returned && else_returned);
        ir_emit_label(c->ir, lbl_end);
    } else {
        c->block_returned = saved_ret;
        ir_emit_label(c->ir, lbl_else);
    }
}

static bool is_compound_assign_tok(VortexTokenType t) {
    return t >= TOK_PLUS_EQ && t <= TOK_SHR_EQ;
}

static IROpcode compound_assign_ir_op(VortexTokenType t, bool is_f32) {
    switch (t) {
        case TOK_PLUS_EQ:    return is_f32 ? IR_FADD : IR_ADD;
        case TOK_MINUS_EQ:   return is_f32 ? IR_FSUB : IR_SUB;
        case TOK_STAR_EQ:    return is_f32 ? IR_FMUL : IR_MUL;
        case TOK_SLASH_EQ:   return is_f32 ? IR_FDIV : IR_DIV;
        case TOK_PERCENT_EQ: return IR_MOD;
        case TOK_AMP_EQ:     return IR_BAND;
        case TOK_PIPE_EQ:    return IR_BOR;
        case TOK_CARET_EQ:   return IR_BXOR;
        case TOK_SHL_EQ:     return IR_SHL;
        case TOK_SHR_EQ:     return IR_SHR;
        default:             return IR_ADD;
    }
}

static bool try_parse_ident_stmt(Compiler* c, int stmt_line, int stmt_col) {
    Lexer* l = c->lex;
    if (c->lex->pos >= c->lex->len) return false;

    char name[64];
    strncpy(name, l->cur.text, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    Lexer saved = *c->lex;
    next_token(c->lex);
    VortexTokenType next_t = c->lex->cur.type;
    *c->lex = saved;

    if (next_t == TOK_EQ || is_compound_assign_tok(next_t)) {
        next_token(l);
        VortexTokenType assign_op = l->cur.type;
        next_token(l);
        VReg v_val;
        vortex_parse_expr(c, &v_val);
        consume_stmt_end(c);

        Symbol* sym = vortex_symtab_find(c->st, name);
        if (!sym) {
            fprintf(stderr, "[VORTEX ERROR] Variable '%s' not found for assignment at %s:%d:%d\n",
                    name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
            vortex_error("");
        }
        if (!sym->is_mut) {
            fprintf(stderr, "[VORTEX SAFETY ERROR] Cannot reassign immutable binding '%s' at %s:%d:%d\n",
                    name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
            vortex_error("");
        }
        sym->reassigned = true;
        if (assign_op == TOK_EQ) {
            if (v_val >= 0 && v_val < c->ir->vreg_count && c->ir->vregs[v_val].is_ref) {
                int val_depth = c->ir->vregs[v_val].region_depth;
                if (val_depth > sym->region_depth) {
                    fprintf(stderr,
                            "[VORTEX REGION ERROR] Reference from inner arena scope (depth %d) cannot escape to outer variable '%s' (depth %d) at %s:%d:%d\n",
                            val_depth, sym->name, sym->region_depth,
                            g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                    vortex_error("");
                }
                sym->is_ref = true;
                sym->region_id = c->ir->vregs[v_val].region_id;
            }
            ir_emit_mov(c->ir, sym->vreg, v_val);
        } else {
            sym->used = true;
            bool is_f32 = (sym->type == TYPE_F32);
            if (!is_f32 && (assign_op == TOK_SLASH_EQ || assign_op == TOK_PERCENT_EQ) &&
                g_ultrasafe_level >= 1 && c->unsafe_depth == 0) {
                ir_emit_div_check(c->ir, v_val, g_current_filename, stmt_line);
            }
            IROpcode op = compound_assign_ir_op(assign_op, is_f32);
            ir_emit_binop(c->ir, op, sym->vreg, sym->vreg, v_val, is_f32 ? TYPE_F32 : TYPE_I64);
        }
        return true;
    }

    if (next_t == TOK_DOT) {
        next_token(l);
        char chain_fields[16][64];
        int chain_len = 0;
        while (l->cur.type == TOK_DOT && chain_len < 16) {
            vortex_parser_match(c, TOK_DOT);
            if (l->cur.type != TOK_IDENT) {
                fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %s:%d:%d\n",
                        g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
                vortex_error("");
            }
            strncpy(chain_fields[chain_len], l->cur.text, 63);
            chain_fields[chain_len][63] = '\0';
            chain_len++;
            next_token(l);
        }

        if (chain_len > 0 && (l->cur.type == TOK_EQ || is_compound_assign_tok(l->cur.type))) {
            VortexTokenType assign_op = l->cur.type;
            next_token(l);
            VReg v_val;
            VortexType val_t = vortex_parse_expr(c, &v_val);
            consume_stmt_end(c);

            Symbol* sym = vortex_symtab_find(c->st, name);
            if (!sym) {
                fprintf(stderr, "[VORTEX ERROR] Variable '%s' not found for field assignment at %s:%d:%d\n",
                        name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                vortex_error("");
            }
            vortex_check_symbol_alive(c, sym, stmt_line, stmt_col);
            sym->used = true;

            if (assign_op == TOK_EQ && v_val >= 0 && v_val < c->ir->vreg_count &&
                c->ir->vregs[v_val].is_ref && c->ir->vregs[v_val].region_depth > sym->region_depth) {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Cannot store inner arena reference (depth %d) in outer struct '%s' (depth %d) at %s:%d:%d\n",
                        c->ir->vregs[v_val].region_depth, sym->name, sym->region_depth,
                        g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                vortex_error("");
            }

            VReg base_v = sym->vreg;
            int cur_sid = sym->struct_id;
            for (int ci = 0; ci < chain_len - 1; ci++) {
                VortexField* f_mid = vortex_resolve_struct_field(cur_sid, chain_fields[ci]);
                if (!f_mid) {
                    fprintf(stderr, "[VORTEX ERROR] Unknown field '%s' on variable '%s' at %s:%d:%d\n",
                            chain_fields[ci], name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                    vortex_error("");
                }
                if (g_compile_mode == VORTEX_MODE_SAFE && c->unsafe_depth == 0) {
                    ir_emit_ptr_check(c->ir, base_v, g_current_filename ? g_current_filename : "<input>", stmt_line);
                }
                VReg v_next_ptr = ir_new_vreg(c->ir, f_mid->type);
                ir_emit_load_offset(c->ir, v_next_ptr, base_v, f_mid->offset, f_mid->type);
                base_v = v_next_ptr;
                cur_sid = f_mid->struct_id;
            }

            const char* target_fname = chain_fields[chain_len - 1];
            VortexField* f = vortex_resolve_struct_field(cur_sid, target_fname);
            if (!f) {
                fprintf(stderr, "[VORTEX ERROR] Unknown field '%s' on variable '%s' at %s:%d:%d\n",
                        target_fname, name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                vortex_error("");
            }
            if (g_compile_mode == VORTEX_MODE_SAFE && c->unsafe_depth == 0) {
                ir_emit_ptr_check(c->ir, base_v, g_current_filename ? g_current_filename : "<input>", stmt_line);
            }
            VortexType ft = (val_t == TYPE_F32 || f->type == TYPE_F32) ? TYPE_F32 : f->type;
            if (assign_op == TOK_EQ) {
                ir_emit_store_offset(c->ir, base_v, f->offset, v_val, ft);
            } else {
                bool is_f32 = (ft == TYPE_F32);
                if (!is_f32 && (assign_op == TOK_SLASH_EQ || assign_op == TOK_PERCENT_EQ) &&
                    g_ultrasafe_level >= 1 && c->unsafe_depth == 0) {
                    ir_emit_div_check(c->ir, v_val, g_current_filename, stmt_line);
                }
                VReg v_cur = ir_new_vreg(c->ir, ft);
                VReg v_next = ir_new_vreg(c->ir, ft);
                ir_emit_load_offset(c->ir, v_cur, base_v, f->offset, ft);
                ir_emit_binop(c->ir, compound_assign_ir_op(assign_op, is_f32), v_next, v_cur, v_val, is_f32 ? TYPE_F32 : TYPE_I64);
                ir_emit_store_offset(c->ir, base_v, f->offset, v_next, ft);
            }
            return true;
        }
        *c->lex = saved;
        return false;
    }

    if (next_t == TOK_LBRACKET) {
        next_token(l);
        vortex_parser_match(c, TOK_LBRACKET);
        VReg v_idx;
        vortex_parse_expr(c, &v_idx);
        vortex_parser_match(c, TOK_RBRACKET);

        Symbol* sym = vortex_symtab_find(c->st, name);
        if (!sym) {
            fprintf(stderr, "[VORTEX ERROR] Variable '%s' not found at %s:%d:%d\n",
                    name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
            vortex_error("");
        }
        vortex_check_symbol_alive(c, sym, stmt_line, stmt_col);
        sym->used = true;

        if (l->cur.type == TOK_DOT) {
            vortex_parser_match(c, TOK_DOT);
            if (l->cur.type != TOK_IDENT) {
                fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %s:%d:%d\n",
                        g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
                vortex_error("");
            }
            char fname[64];
            strncpy(fname, l->cur.text, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
            next_token(l);
            if (l->cur.type != TOK_EQ && !is_compound_assign_tok(l->cur.type)) {
                *c->lex = saved;
                return false;
            }
            VortexTokenType assign_op = l->cur.type;
            next_token(l);
            VReg v_val;
            VortexType val_t = vortex_parse_expr(c, &v_val);
            consume_stmt_end(c);

            VortexField* f = vortex_resolve_struct_field(sym->struct_id, fname);
            int f_offset = f ? f->offset : 0;
            VortexType ft = (val_t == TYPE_F32 || (f && f->type == TYPE_F32)) ? TYPE_F32 : TYPE_I64;

            if ((c->unsafe_depth == 0 || sym->is_slice_ptr) &&
                (g_compile_mode == VORTEX_MODE_SAFE || sym->is_slice_ptr)) {
                if (!sym->is_slice_ptr && c->unsafe_depth == 0) {
                    vortex_require_unsafe(c, "Indexing untracked raw pointer", stmt_line, stmt_col);
                }
                ir_emit_bounds_check(c->ir, sym->vreg, v_idx, sym->len_vreg, sym->known_len, 8,
                                     g_current_filename ? g_current_filename : "<input>", stmt_line);
            }
            VReg v_elem = ir_new_vreg(c->ir, TYPE_PTR_STRUCT);
            ir_emit_load_array(c->ir, v_elem, sym->vreg, v_idx, TYPE_PTR_STRUCT);
            if (assign_op == TOK_EQ) {
                ir_emit_store_offset(c->ir, v_elem, f_offset, v_val, ft);
            } else {
                bool is_f32 = (ft == TYPE_F32);
                if (!is_f32 && (assign_op == TOK_SLASH_EQ || assign_op == TOK_PERCENT_EQ) &&
                    g_ultrasafe_level >= 1 && c->unsafe_depth == 0) {
                    ir_emit_div_check(c->ir, v_val, g_current_filename, stmt_line);
                }
                VReg v_cur = ir_new_vreg(c->ir, ft);
                VReg v_next = ir_new_vreg(c->ir, ft);
                ir_emit_load_offset(c->ir, v_cur, v_elem, f_offset, ft);
                ir_emit_binop(c->ir, compound_assign_ir_op(assign_op, is_f32), v_next, v_cur, v_val, is_f32 ? TYPE_F32 : TYPE_I64);
                ir_emit_store_offset(c->ir, v_elem, f_offset, v_next, ft);
            }
            return true;
        }

        if (l->cur.type == TOK_EQ || is_compound_assign_tok(l->cur.type)) {
            VortexTokenType assign_op = l->cur.type;
            next_token(l);
            VReg v_val;
            VortexType val_t = vortex_parse_expr(c, &v_val);
            consume_stmt_end(c);

            if (assign_op == TOK_EQ && v_val >= 0 && v_val < c->ir->vreg_count &&
                c->ir->vregs[v_val].is_ref && c->ir->vregs[v_val].region_depth > sym->region_depth) {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Cannot store inner arena reference (depth %d) in outer array '%s' (depth %d) at %s:%d:%d\n",
                        c->ir->vregs[v_val].region_depth, sym->name, sym->region_depth,
                        g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                vortex_error("");
            }

            VortexType elem_t = (sym->type == TYPE_PTR_F32 || val_t == TYPE_F32) ? TYPE_F32 :
                                (sym->type == TYPE_PTR_I32) ? TYPE_I32 : TYPE_I64;
            int32_t elem_size = (elem_t == TYPE_F32 || elem_t == TYPE_I32) ? 4 : 8;
            if (!sym->is_slice_ptr && c->unsafe_depth == 0) {
                vortex_require_unsafe(c, "Indexing untracked raw pointer", stmt_line, stmt_col);
            }
            if ((c->unsafe_depth == 0 || sym->is_slice_ptr) &&
                (g_compile_mode == VORTEX_MODE_SAFE || sym->is_slice_ptr)) {
                ir_emit_bounds_check(c->ir, sym->vreg, v_idx, sym->len_vreg, sym->known_len, elem_size,
                                     g_current_filename ? g_current_filename : "<input>", stmt_line);
            }
            if (assign_op == TOK_EQ) {
                ir_emit_store_array(c->ir, sym->vreg, v_idx, v_val, elem_t);
            } else {
                bool is_f32 = (elem_t == TYPE_F32);
                if (!is_f32 && (assign_op == TOK_SLASH_EQ || assign_op == TOK_PERCENT_EQ) &&
                    g_ultrasafe_level >= 1 && c->unsafe_depth == 0) {
                    ir_emit_div_check(c->ir, v_val, g_current_filename, stmt_line);
                }
                VReg v_cur = ir_new_vreg(c->ir, elem_t);
                VReg v_next = ir_new_vreg(c->ir, elem_t);
                ir_emit_load_array(c->ir, v_cur, sym->vreg, v_idx, elem_t);
                ir_emit_binop(c->ir, compound_assign_ir_op(assign_op, is_f32), v_next, v_cur, v_val, is_f32 ? TYPE_F32 : TYPE_I64);
                ir_emit_store_array(c->ir, sym->vreg, v_idx, v_next, elem_t);
            }
            return true;
        }
        *c->lex = saved;
    }

    return false;
}

static void parse_match_stmt(Compiler* c) {
    Lexer* l = c->lex;
    next_token(l); /* consume 'match' */
    VReg v_scrut;
    VortexType scrut_t = vortex_parse_expr(c, &v_scrut);
    vortex_parser_match(c, TOK_LBRACE);

    int lbl_end = ir_new_label(c->ir);
    bool saved_ret = c->block_returned;
    bool all_arms_returned = true;
    bool has_wildcard = false;

    while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
        bool is_wild = (l->cur.type == TOK_IDENT && strcmp(l->cur.text, "_") == 0);
        int lbl_body = ir_new_label(c->ir);
        int lbl_next = ir_new_label(c->ir);

        if (is_wild) {
            next_token(l);
            has_wildcard = true;
            ir_emit_jmp(c->ir, lbl_body);
        } else {
            while (l->cur.type != TOK_FAT_ARROW && l->cur.type != TOK_EOF) {
                VReg v_pat;
                VortexType pat_t = vortex_parse_match_pattern(c, &v_pat);
                VReg v_cmp = ir_new_vreg(c->ir, TYPE_I64);
                if (scrut_t == TYPE_F32 || pat_t == TYPE_F32) {
                    ir_emit_binop(c->ir, IR_FEQ, v_cmp, v_scrut, v_pat, TYPE_F32);
                } else {
                    ir_emit_binop(c->ir, IR_EQ, v_cmp, v_scrut, v_pat, TYPE_I64);
                }
                int lbl_try_next = ir_new_label(c->ir);
                ir_emit_jz(c->ir, v_cmp, lbl_try_next);
                ir_emit_jmp(c->ir, lbl_body);
                ir_emit_label(c->ir, lbl_try_next);
                if (l->cur.type == TOK_PIPE) {
                    next_token(l);
                } else {
                    break;
                }
            }
            ir_emit_jmp(c->ir, lbl_next);
        }

        vortex_parser_match(c, TOK_FAT_ARROW);
        ir_emit_label(c->ir, lbl_body);
        c->block_returned = false;
        if (l->cur.type == TOK_LBRACE) {
            next_token(l);
            bool saved_arm = c->in_match_arm;
            c->in_match_arm = false;
            while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
                parse_statement(c);
            }
            c->in_match_arm = saved_arm;
            vortex_parser_match(c, TOK_RBRACE);
        } else {
            bool saved_arm = c->in_match_arm;
            c->in_match_arm = true;
            parse_statement(c);
            c->in_match_arm = saved_arm;
        }
        if (!c->block_returned) all_arms_returned = false;
        ir_emit_jmp(c->ir, lbl_end);
        ir_emit_label(c->ir, lbl_next);

        if (l->cur.type == TOK_COMMA || l->cur.type == TOK_SEMI) next_token(l);
    }
    vortex_parser_match(c, TOK_RBRACE);
    ir_emit_label(c->ir, lbl_end);
    c->block_returned = saved_ret || (has_wildcard && all_arms_returned);
}

void parse_statement(Compiler* c) {
    Lexer* l = c->lex;
    int stmt_line = l->cur.line;
    int stmt_col = l->cur.col;
    int ir_start = c->ir->count;

    if (l->cur.type == TOK_HASH) { parse_cfg_stmt(c); return; }
    if (l->cur.type == TOK_IMPORT) { parse_import_stmt(c); return; }
    if (l->cur.type == TOK_EXTERN) { parse_extern_stmt(c); return; }
    if (l->cur.type == TOK_FN) { parse_function_def(c, false); return; }
    if (l->cur.type == TOK_STRUCT) { parse_struct_stmt(c); return; }
    if (l->cur.type == TOK_ENUM) { parse_enum_stmt(c); return; }
    if (l->cur.type == TOK_MATCH) { parse_match_stmt(c); vortex_stamp_ir_loc(c->ir, ir_start, stmt_line); return; }

    if (l->cur.type == TOK_BREAK) {
        next_token(l);
        if (l->cur.type == TOK_SEMI) next_token(l);
        if (c->loop_depth <= 0) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] 'break' used outside of loop at %s:%d:%d\n",
                    g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
            vortex_error("");
        }
        ir_emit_jmp(c->ir, c->loop_break_lbl[c->loop_depth - 1]);
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    if (l->cur.type == TOK_CONTINUE) {
        next_token(l);
        if (l->cur.type == TOK_SEMI) next_token(l);
        if (c->loop_depth <= 0) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] 'continue' used outside of loop at %s:%d:%d\n",
                    g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
            vortex_error("");
        }
        ir_emit_jmp(c->ir, c->loop_cont_lbl[c->loop_depth - 1]);
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    if (l->cur.type == TOK_UNSAFE) {
        next_token(l);
        if (l->cur.type == TOK_FN) {
            parse_function_def(c, true);
            return;
        }
        vortex_parser_match(c, TOK_LBRACE);
        c->unsafe_depth++;
        while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
            parse_statement(c);
        }
        vortex_parser_match(c, TOK_RBRACE);
        c->unsafe_depth--;
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    if (l->cur.type == TOK_STAR) {
        next_token(l);
        vortex_require_unsafe(c, "Raw pointer dereference write '*ptr = ...'", stmt_line, stmt_col);
        VReg ptr_v;
        VortexType pt = vortex_parse_primary(c, &ptr_v);
        vortex_parser_match(c, TOK_EQ);
        VReg v_val;
        VortexType vt = vortex_parse_expr(c, &v_val);
        consume_stmt_end(c);
        VortexType elem_t = (pt == TYPE_PTR_I32) ? TYPE_I32 :
                            (pt == TYPE_PTR_F32) ? TYPE_F32 :
                            (vt == TYPE_F32) ? TYPE_F32 : TYPE_I64;
        ir_emit_store_deref(c->ir, ptr_v, v_val, elem_t);
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    if (l->cur.type == TOK_RETURN) {
        next_token(l);
        VReg v_val;
        VortexType val_t = vortex_parse_expr(c, &v_val);
        consume_stmt_end(c);
        if (v_val >= 0 && v_val < c->ir->vreg_count && c->ir->vregs[v_val].is_ref) {
            int rid = c->ir->vregs[v_val].region_id;
            int rdepth = c->ir->vregs[v_val].region_depth;
            if (vortex_region_is_popped(c, rid) || rdepth > 0) {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Cannot return reference allocated inside internal/popped 'scope_push()' region at %s:%d:%d\n",
                        g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                vortex_error("");
            }
        }
        ir_emit_ret(c->ir, v_val, val_t);
        c->block_returned = true;
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    if (l->cur.type == TOK_LET) {
        parse_let_stmt(c, stmt_line, stmt_col);
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }
    if (l->cur.type == TOK_PARALLEL_FOR) {
        parse_parallel_for_stmt(c);
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }
    if (l->cur.type == TOK_FOR) {
        parse_for_stmt(c);
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }
    if (l->cur.type == TOK_WHILE) {
        parse_while_stmt(c);
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }
    if (l->cur.type == TOK_IF) {
        parse_if_stmt(c);
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }
    if (l->cur.type == TOK_IDENT && try_parse_ident_stmt(c, stmt_line, stmt_col)) {
        vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* General Expression Statement */
    VReg dummy;
    vortex_parse_expr(c, &dummy);
    consume_stmt_end(c);
    vortex_stamp_ir_loc(c->ir, ir_start, stmt_line);
}
