/*
 * VORTEX PROGRAMMING LANGUAGE - LEXER, TYPE RESOLVER & IR FRONTEND PARSER
 */

#include "vortex.h"

static void unescape_string(char* dst, const char* src) {
    while (*src) {
        if (*src == '\\' && *(src + 1)) {
            src++;
            switch (*src) {
                case 'n': *dst++ = '\n'; break;
                case 't': *dst++ = '\t'; break;
                case 'r': *dst++ = '\r'; break;
                case '0': *dst++ = '\0'; break;
                case '\\': *dst++ = '\\'; break;
                case '\"': *dst++ = '\"'; break;
                default: *dst++ = *src; break;
            }
        } else {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

void next_token(Lexer* lex) {
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lex->pos++; lex->col++; continue;
        }
        if (c == '\n') {
            lex->pos++; lex->line++; lex->col = 1; continue;
        }
        if (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/') {
            lex->pos += 2;
            while (lex->pos < lex->len && lex->src[lex->pos] != '\n') lex->pos++;
            continue;
        }
        break;
    }

    if (lex->pos >= lex->len) {
        lex->cur.type = TOK_EOF;
        lex->cur.text[0] = '\0';
        return;
    }

    lex->cur.line = lex->line;
    lex->cur.col = lex->col;
    char c = lex->src[lex->pos];

    if (isdigit((unsigned char)c)) {
        size_t start = lex->pos;
        bool is_float = false;
        while (lex->pos < lex->len && (isdigit((unsigned char)lex->src[lex->pos]) || lex->src[lex->pos] == '.')) {
            if (lex->src[lex->pos] == '.') {
                if (lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '.') break;
                is_float = true;
            }
            lex->pos++; lex->col++;
        }
        size_t len = lex->pos - start;
        if (len >= sizeof(lex->cur.text)) len = sizeof(lex->cur.text) - 1;
        memcpy(lex->cur.text, &lex->src[start], len);
        lex->cur.text[len] = '\0';

        if (is_float) {
            lex->cur.type = TOK_FLOAT;
            lex->cur.float_val = (float)strtod(lex->cur.text, NULL);
        } else {
            lex->cur.type = TOK_INT;
            lex->cur.int_val = strtoll(lex->cur.text, NULL, 10);
        }
        return;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = lex->pos;
        while (lex->pos < lex->len && (isalnum((unsigned char)lex->src[lex->pos]) || lex->src[lex->pos] == '_')) {
            lex->pos++; lex->col++;
        }
        size_t len = lex->pos - start;
        if (len >= sizeof(lex->cur.text)) len = sizeof(lex->cur.text) - 1;
        memcpy(lex->cur.text, &lex->src[start], len);
        lex->cur.text[len] = '\0';

        if (strcmp(lex->cur.text, "let") == 0) lex->cur.type = TOK_LET;
        else if (strcmp(lex->cur.text, "mut") == 0) lex->cur.type = TOK_MUT;
        else if (strcmp(lex->cur.text, "for") == 0) lex->cur.type = TOK_FOR;
        else if (strcmp(lex->cur.text, "parallel_for") == 0) lex->cur.type = TOK_PARALLEL_FOR;
        else if (strcmp(lex->cur.text, "in") == 0) lex->cur.type = TOK_IN;
        else if (strcmp(lex->cur.text, "while") == 0) lex->cur.type = TOK_WHILE;
        else if (strcmp(lex->cur.text, "if") == 0) lex->cur.type = TOK_IF;
        else if (strcmp(lex->cur.text, "else") == 0) lex->cur.type = TOK_ELSE;
        else if (strcmp(lex->cur.text, "fn") == 0) lex->cur.type = TOK_FN;
        else if (strcmp(lex->cur.text, "return") == 0) lex->cur.type = TOK_RETURN;
        else if (strcmp(lex->cur.text, "struct") == 0) lex->cur.type = TOK_STRUCT;
        else if (strcmp(lex->cur.text, "extern") == 0) lex->cur.type = TOK_EXTERN;
        else if (strcmp(lex->cur.text, "i64") == 0) lex->cur.type = TOK_I64_KW;
        else if (strcmp(lex->cur.text, "f32") == 0) lex->cur.type = TOK_F32_KW;
        else lex->cur.type = TOK_IDENT;
        return;
    }

    if (c == '"') {
        lex->pos++; lex->col++;
        size_t start = lex->pos;
        while (lex->pos < lex->len && lex->src[lex->pos] != '"') {
            if (lex->src[lex->pos] == '\\' && lex->pos + 1 < lex->len) {
                lex->pos += 2; lex->col += 2;
            } else {
                lex->pos++; lex->col++;
            }
        }
        size_t raw_len = lex->pos - start;
        char temp[512];
        if (raw_len >= sizeof(temp)) raw_len = sizeof(temp) - 1;
        memcpy(temp, &lex->src[start], raw_len);
        temp[raw_len] = '\0';
        unescape_string(lex->cur.text, temp);

        if (lex->pos < lex->len && lex->src[lex->pos] == '"') {
            lex->pos++; lex->col++;
        }
        lex->cur.type = TOK_STRING;
        return;
    }

    if (lex->pos + 1 < lex->len) {
        char c2 = lex->src[lex->pos + 1];
        if (c == '-' && c2 == '>') {
            lex->pos += 2; lex->col += 2;
            lex->cur.type = TOK_ARROW; strcpy(lex->cur.text, "->"); return;
        }
        if (c == '.' && c2 == '.') {
            lex->pos += 2; lex->col += 2;
            lex->cur.type = TOK_DOTDOT; strcpy(lex->cur.text, ".."); return;
        }
        if (c == '=' && c2 == '=') {
            lex->pos += 2; lex->col += 2;
            lex->cur.type = TOK_EQEQ; strcpy(lex->cur.text, "=="); return;
        }
        if (c == '!' && c2 == '=') {
            lex->pos += 2; lex->col += 2;
            lex->cur.type = TOK_NEQ; strcpy(lex->cur.text, "!="); return;
        }
        if (c == '<' && c2 == '=') {
            lex->pos += 2; lex->col += 2;
            lex->cur.type = TOK_LE; strcpy(lex->cur.text, "<="); return;
        }
        if (c == '>' && c2 == '=') {
            lex->pos += 2; lex->col += 2;
            lex->cur.type = TOK_GE; strcpy(lex->cur.text, ">="); return;
        }
    }

    lex->pos++; lex->col++;
    lex->cur.text[0] = c; lex->cur.text[1] = '\0';

    switch (c) {
        case '+': lex->cur.type = TOK_PLUS; break;
        case '-': lex->cur.type = TOK_MINUS; break;
        case '*': lex->cur.type = TOK_STAR; break;
        case '/': lex->cur.type = TOK_SLASH; break;
        case '%': lex->cur.type = TOK_PERCENT; break;
        case '=': lex->cur.type = TOK_EQ; break;
        case '<': lex->cur.type = TOK_LT; break;
        case '>': lex->cur.type = TOK_GT; break;
        case '(': lex->cur.type = TOK_LPAREN; break;
        case ')': lex->cur.type = TOK_RPAREN; break;
        case '{': lex->cur.type = TOK_LBRACE; break;
        case '}': lex->cur.type = TOK_RBRACE; break;
        case '[': lex->cur.type = TOK_LBRACKET; break;
        case ']': lex->cur.type = TOK_RBRACKET; break;
        case ';': lex->cur.type = TOK_SEMI; break;
        case ',': lex->cur.type = TOK_COMMA; break;
        case ':': lex->cur.type = TOK_COLON; break;
        case '.': lex->cur.type = TOK_DOT; break;
        default:
            fprintf(stderr, "[VORTEX ERROR] Unknown character '%c' at %d:%d\n", c, lex->line, lex->col);
            vortex_error("");
    }
}


/* --- Structs, Types & Symbol Tables --- */
VortexStructType g_struct_types[MAX_STRUCT_TYPES];
int g_struct_type_count = 0;

VortexStructType* struct_type_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_struct_type_count; i++) {
        if (strcmp(g_struct_types[i].name, name) == 0) return &g_struct_types[i];
    }
    return NULL;
}

VortexStructType* struct_type_find_by_id(int id) {
    if (id >= 0 && id < g_struct_type_count) return &g_struct_types[id];
    return NULL;
}

VortexField* struct_field_find(VortexStructType* st, const char* field_name) {
    if (!st || !field_name) return NULL;
    for (int i = 0; i < st->field_count; i++) {
        if (strcmp(st->fields[i].name, field_name) == 0) return &st->fields[i];
    }
    return NULL;
}

VortexType parse_type_name(Lexer* l, int* out_struct_id) {
    if (out_struct_id) *out_struct_id = -1;
    if (l->cur.type == TOK_I64_KW || (l->cur.type == TOK_IDENT && strcmp(l->cur.text, "i64") == 0)) {
        next_token(l);
        return TYPE_I64;
    }
    if (l->cur.type == TOK_F32_KW || (l->cur.type == TOK_IDENT && strcmp(l->cur.text, "f32") == 0)) {
        next_token(l);
        return TYPE_F32;
    }
    if (l->cur.type == TOK_IDENT) {
        if (strcmp(l->cur.text, "i32") == 0) {
            next_token(l);
            return TYPE_I32;
        }
        if (strcmp(l->cur.text, "f64") == 0) {
            next_token(l);
            return TYPE_F64;
        }
        if (strcmp(l->cur.text, "str") == 0) {
            next_token(l);
            return TYPE_STR;
        }
        if (strcmp(l->cur.text, "ptr") == 0) {
            next_token(l);
            return TYPE_PTR;
        }
        if (strcmp(l->cur.text, "void") == 0) {
            next_token(l);
            return TYPE_VOID;
        }
        VortexStructType* st = struct_type_find(l->cur.text);
        if (st) {
            if (out_struct_id) *out_struct_id = st->id;
            next_token(l);
            return TYPE_PTR_STRUCT;
        }
    }
    return TYPE_UNKNOWN;
}

int64_t vortex_builtin_struct_size(int64_t name_ptr) {
    const char* name = (const char*)name_ptr;
    if (!name) return 0;
    VortexStructType* st = struct_type_find(name);
    return st ? st->size : 0;
}

int64_t vortex_builtin_struct_align(int64_t name_ptr) {
    const char* name = (const char*)name_ptr;
    if (!name) return 0;
    VortexStructType* st = struct_type_find(name);
    return st ? st->align : 0;
}

static Symbol* symtab_find(SymbolTable* st, const char* name) {
    for (int i = st->count - 1; i >= 0; i--) {
        if (strcmp(st->symbols[i].name, name) == 0) return &st->symbols[i];
    }
    return NULL;
}

static Symbol* symtab_add(SymbolTable* st, const char* name, VortexType type, bool is_mut) {
    if (st->count >= MAX_SYMBOLS) {
        fprintf(stderr, "[VORTEX ERROR] Symbol table full\n");
        vortex_error("");
    }
    st->stack_alloc_bytes += 8;
    Symbol* s = &st->symbols[st->count++];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    s->type = type;
    s->struct_id = -1;
    s->is_mut = is_mut;
    s->stack_offset = st->stack_alloc_bytes;
    s->vreg = -1;
    return s;
}

/* --- User Functions & Relocations --- */
UserFuncReloc g_user_func_relocs[MAX_USER_FUNC_RELOCS];
int g_user_func_reloc_count = 0;

UserFunc g_user_funcs[MAX_USER_FUNCS];
int g_user_func_count = 0;

UserFunc* user_func_find(const char* name) {
    for (int i = 0; i < g_user_func_count; i++) {
        if (strcmp(g_user_funcs[i].name, name) == 0) return &g_user_funcs[i];
    }
    return NULL;
}

/* --- Literal Strings Pool --- */
char* g_literal_strings[MAX_LITERAL_STRINGS];
int g_literal_string_count = 0;

static const char* register_string_literal(const char* s) {
    if (g_literal_string_count >= MAX_LITERAL_STRINGS) return "";
    char* copy = strdup(s);
    g_literal_strings[g_literal_string_count++] = copy;
    return copy;
}

/* --- Recursive-Descent Parser & IR Lowering --- */
static VortexType parse_expr(Compiler* c, VReg* out_vreg);
void parse_statement(Compiler* c);
static VortexType parse_primary(Compiler* c, VReg* out_vreg);
static VortexType parse_multiplicative(Compiler* c, VReg* out_vreg);
static VortexType parse_additive(Compiler* c, VReg* out_vreg);
static bool try_match_and_emit_unrolled_riemann(Compiler* c, Symbol* i_sym, Symbol* lim_sym);

static void match(Compiler* c, VortexTokenType expected) {
    if (c->lex->cur.type != expected) {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Unexpected token '%s' at %d:%d\n",
                c->lex->cur.text, c->lex->cur.line, c->lex->cur.col);
        vortex_error("");
    }
    next_token(c->lex);
}

static bool is_field_named_init(Lexer* lex) {
    if (lex->cur.type != TOK_IDENT) return false;
    size_t p = lex->pos;
    while (p < lex->len) {
        char ch = lex->src[p];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { p++; continue; }
        if (ch == '/' && p + 1 < lex->len && lex->src[p + 1] == '/') {
            p += 2;
            while (p < lex->len && lex->src[p] != '\n') p++;
            continue;
        }
        break;
    }
    if (p < lex->len && lex->src[p] == ':' && (p + 1 >= lex->len || lex->src[p + 1] != ':')) {
        return true;
    }
    return false;
}

static VortexType parse_primary(Compiler* c, VReg* out_vreg) {
    Lexer* l = c->lex;

    if (l->cur.type == TOK_MINUS) {
        next_token(l);
        VReg v_inner;
        VortexType t = parse_primary(c, &v_inner);
        if (t == TYPE_F32) {
            *out_vreg = ir_new_vreg(c->ir, TYPE_F32);
            VReg v_zero = ir_new_vreg(c->ir, TYPE_F32);
            ir_emit_const_f32(c->ir, v_zero, 0.0f);
            ir_emit_binop(c->ir, IR_FSUB, *out_vreg, v_zero, v_inner, TYPE_F32);
            return TYPE_F32;
        } else {
            *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
            VReg v_zero = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_const_i64(c->ir, v_zero, 0);
            ir_emit_binop(c->ir, IR_SUB, *out_vreg, v_zero, v_inner, TYPE_I64);
            return TYPE_I64;
        }
    }

    if (l->cur.type == TOK_INT) {
        int64_t val = l->cur.int_val;
        next_token(l);
        *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, *out_vreg, val);
        return TYPE_I64;
    }

    if (l->cur.type == TOK_FLOAT) {
        float val = l->cur.float_val;
        next_token(l);
        *out_vreg = ir_new_vreg(c->ir, TYPE_F32);
        ir_emit_const_f32(c->ir, *out_vreg, val);
        return TYPE_F32;
    }

    if (l->cur.type == TOK_STRING) {
        const char* s = register_string_literal(l->cur.text);
        next_token(l);
        *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, *out_vreg, (int64_t)s);
        return TYPE_I64;
    }

    if (l->cur.type == TOK_LPAREN) {
        next_token(l);
        VortexType t = parse_expr(c, out_vreg);
        match(c, TOK_RPAREN);
        return t;
    }

    if (l->cur.type == TOK_IDENT) {
        char name[64];
        strncpy(name, l->cur.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        next_token(l);

        // Struct Instantiation: StructName { f1: v1, ... }
        VortexStructType* st_def = struct_type_find(name);
        if (st_def && l->cur.type == TOK_LBRACE) {
            match(c, TOK_LBRACE);
            VReg v_sz = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_const_i64(c->ir, v_sz, st_def->size);
            VReg v_struct = ir_new_vreg(c->ir, TYPE_PTR_STRUCT);
            ir_emit_call(c->ir, v_struct, (void*)vortex_alloc_struct, &v_sz, 1, TYPE_PTR_STRUCT);

            int field_idx = 0;
            while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
                VortexField* f = NULL;
                if (is_field_named_init(l)) {
                    char fname[64];
                    strncpy(fname, l->cur.text, sizeof(fname) - 1);
                    fname[sizeof(fname) - 1] = '\0';
                    next_token(l);
                    match(c, TOK_COLON);
                    f = struct_field_find(st_def, fname);
                } else {
                    if (field_idx < st_def->field_count) {
                        f = &st_def->fields[field_idx];
                    }
                }
                field_idx++;

                VReg v_val;
                VortexType val_t = parse_expr(c, &v_val);
                if (f) {
                    VortexType ft = (f->type == TYPE_F32 || val_t == TYPE_F32) ? TYPE_F32 : f->type;
                    ir_emit_store_offset(c->ir, v_struct, f->offset, v_val, ft);
                }
                if (l->cur.type == TOK_COMMA || l->cur.type == TOK_SEMI) next_token(l);
            }
            match(c, TOK_RBRACE);
            c->last_struct_id = st_def->id;

            VortexType cur_type = TYPE_STRUCT;
            int cur_sid = st_def->id;
            VReg cur_v = v_struct;

            while (l->cur.type == TOK_DOT) {
                next_token(l);
                if (l->cur.type != TOK_IDENT) {
                    fprintf(stderr, "[VORTEX ERROR] Expected field name after '.'\n");
                    vortex_error("");
                }
                char fname[64];
                strncpy(fname, l->cur.text, sizeof(fname) - 1);
                fname[sizeof(fname) - 1] = '\0';
                next_token(l);

                VortexStructType* curr_st = struct_type_find_by_id(cur_sid);
                VortexField* f = struct_field_find(curr_st, fname);
                if (!f) {
                    fprintf(stderr, "[VORTEX ERROR] Unknown field '%s'\n", fname);
                    vortex_error("");
                }
                VReg v_f = ir_new_vreg(c->ir, f->type);
                ir_emit_load_offset(c->ir, v_f, cur_v, f->offset, f->type);
                cur_v = v_f;
                cur_type = f->type;
                cur_sid = f->struct_id;
            }
            *out_vreg = cur_v;
            return cur_type;
        }

        // Function Call: name(arg1, arg2, ...)
        if (l->cur.type == TOK_LPAREN) {
            next_token(l);
            int num_args = 0;
            VReg arg_vregs[8];
            VortexType arg_types[8];
            while (l->cur.type != TOK_RPAREN && l->cur.type != TOK_EOF) {
                arg_types[num_args] = parse_expr(c, &arg_vregs[num_args]);
                num_args++;
                if (l->cur.type == TOK_COMMA) next_token(l);
            }
            match(c, TOK_RPAREN);

            if (strcmp(name, "print") == 0 && num_args == 1) {
                *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
                if (arg_types[0] == TYPE_F32) {
                    ir_emit_call(c->ir, *out_vreg, (void*)helper_print_f32, arg_vregs, 1, TYPE_I64);
                } else {
                    ir_emit_call(c->ir, *out_vreg, (void*)helper_print_i64, arg_vregs, 1, TYPE_I64);
                }
                return TYPE_I64;
            }

            BuiltinFunc* bf = builtin_find(name);
            if (bf) {
                *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
                ir_emit_call(c->ir, *out_vreg, bf->func_ptr, arg_vregs, num_args, TYPE_I64);
                return TYPE_I64;
            }

            FFIFunc* ff = ffi_func_find(name);
            if (ff) {
                VortexType out_t = (ff->ret_type == TYPE_VOID) ? TYPE_I64 :
                                   (ff->ret_type == TYPE_F64) ? TYPE_F32 : ff->ret_type;
                *out_vreg = (ff->ret_type == TYPE_VOID) ? VREG_NONE : ir_new_vreg(c->ir, out_t);
                ir_emit_call_ffi(c->ir, *out_vreg, ff, arg_vregs, num_args, out_t);
                return out_t;
            }

            UserFunc* uf = user_func_find(name);
            if (uf) {
                *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
                ir_emit_call_user(c->ir, *out_vreg, name, arg_vregs, num_args, TYPE_I64);
                return TYPE_I64;
            }

            // Forward-call to user function:
            *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_call_user(c->ir, *out_vreg, name, arg_vregs, num_args, TYPE_I64);
            return TYPE_I64;
        }

        Symbol* sym = symtab_find(c->st, name);
        if (!sym) {
            fprintf(stderr, "[VORTEX ERROR] Undefined variable '%s' at %d:%d\n", name, l->cur.line, l->cur.col);
            vortex_error("");
        }

        VortexType primary_type = sym->type;
        int primary_struct_id = sym->struct_id;
        VReg cur_v = sym->vreg;

        // Array indexing: arr[index]
        if (l->cur.type == TOK_LBRACKET) {
            next_token(l);
            VReg v_idx;
            parse_expr(c, &v_idx);
            match(c, TOK_RBRACKET);

            VortexType elem_type = (sym->type == TYPE_PTR_F32) ? TYPE_F32 :
                                   (sym->type == TYPE_PTR_STRUCT) ? TYPE_PTR_STRUCT : TYPE_I64;
            VReg v_elem = ir_new_vreg(c->ir, elem_type);
            ir_emit_load_array(c->ir, v_elem, cur_v, v_idx, elem_type);
            cur_v = v_elem;
            primary_type = (elem_type == TYPE_PTR_STRUCT) ? TYPE_STRUCT : elem_type;
        }

        // Postfix field access: .field1.field2
        while (l->cur.type == TOK_DOT) {
            next_token(l);
            if (l->cur.type != TOK_IDENT) {
                fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %d:%d\n", l->cur.line, l->cur.col);
                vortex_error("");
            }
            char fname[64];
            strncpy(fname, l->cur.text, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
            next_token(l);

            VortexStructType* st_lookup = NULL;
            if (primary_struct_id >= 0) {
                st_lookup = struct_type_find_by_id(primary_struct_id);
            }
            if (!st_lookup) {
                for (int s = 0; s < g_struct_type_count; s++) {
                    if (struct_field_find(&g_struct_types[s], fname)) {
                        st_lookup = &g_struct_types[s];
                        break;
                    }
                }
            }
            VortexField* f = struct_field_find(st_lookup, fname);
            if (!f) {
                fprintf(stderr, "[VORTEX ERROR] Unknown field '%s' on variable '%s' at %d:%d\n", fname, sym->name, l->cur.line, l->cur.col);
                vortex_error("");
            }

            VReg v_field = ir_new_vreg(c->ir, f->type);
            ir_emit_load_offset(c->ir, v_field, cur_v, f->offset, f->type);
            cur_v = v_field;
            primary_type = f->type;
            primary_struct_id = f->struct_id;
        }

        *out_vreg = cur_v;
        return primary_type;
    }

    fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected primary expression, got '%s' at %d:%d\n",
            l->cur.text, l->cur.line, l->cur.col);
    vortex_error("");
    return TYPE_I64;
}

static VortexType parse_multiplicative(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_primary(c, out_vreg);

    while (c->lex->cur.type == TOK_STAR || c->lex->cur.type == TOK_SLASH || c->lex->cur.type == TOK_PERCENT) {
        VortexTokenType op = c->lex->cur.type;
        next_token(c->lex);
        VReg r_vreg;
        VortexType right_type = parse_primary(c, &r_vreg);

        if (left_type == TYPE_F32 || right_type == TYPE_F32) {
            VReg dest = ir_new_vreg(c->ir, TYPE_F32);
            if (op == TOK_STAR) ir_emit_binop(c->ir, IR_FMUL, dest, *out_vreg, r_vreg, TYPE_F32);
            else ir_emit_binop(c->ir, IR_FDIV, dest, *out_vreg, r_vreg, TYPE_F32);
            *out_vreg = dest;
            left_type = TYPE_F32;
        } else {
            VReg dest = ir_new_vreg(c->ir, TYPE_I64);
            if (op == TOK_STAR) ir_emit_binop(c->ir, IR_MUL, dest, *out_vreg, r_vreg, TYPE_I64);
            else if (op == TOK_SLASH) ir_emit_binop(c->ir, IR_DIV, dest, *out_vreg, r_vreg, TYPE_I64);
            else ir_emit_binop(c->ir, IR_MOD, dest, *out_vreg, r_vreg, TYPE_I64);
            *out_vreg = dest;
            left_type = TYPE_I64;
        }
    }
    return left_type;
}

static VortexType parse_additive(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_multiplicative(c, out_vreg);

    while (c->lex->cur.type == TOK_PLUS || c->lex->cur.type == TOK_MINUS) {
        VortexTokenType op = c->lex->cur.type;
        next_token(c->lex);
        VReg r_vreg;
        VortexType right_type = parse_multiplicative(c, &r_vreg);

        if (left_type == TYPE_F32 || right_type == TYPE_F32) {
            VReg dest = ir_new_vreg(c->ir, TYPE_F32);
            if (op == TOK_PLUS) ir_emit_binop(c->ir, IR_FADD, dest, *out_vreg, r_vreg, TYPE_F32);
            else ir_emit_binop(c->ir, IR_FSUB, dest, *out_vreg, r_vreg, TYPE_F32);
            *out_vreg = dest;
            left_type = TYPE_F32;
        } else {
            VReg dest = ir_new_vreg(c->ir, TYPE_I64);
            if (op == TOK_PLUS) ir_emit_binop(c->ir, IR_ADD, dest, *out_vreg, r_vreg, TYPE_I64);
            else ir_emit_binop(c->ir, IR_SUB, dest, *out_vreg, r_vreg, TYPE_I64);
            *out_vreg = dest;
            left_type = TYPE_I64;
        }
    }
    return left_type;
}

static VortexType parse_expr(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_additive(c, out_vreg);
    VortexTokenType t = c->lex->cur.type;

    if (t == TOK_LT || t == TOK_LE || t == TOK_GT || t == TOK_GE || t == TOK_EQEQ || t == TOK_NEQ) {
        VortexTokenType op = t;
        next_token(c->lex);
        VReg r_vreg;
        VortexType right_type = parse_additive(c, &r_vreg);
        VReg dest = ir_new_vreg(c->ir, TYPE_I64);

        if (left_type == TYPE_F32 || right_type == TYPE_F32) {
            IROpcode ir_op;
            switch (op) {
                case TOK_EQEQ: ir_op = IR_FEQ; break;
                case TOK_NEQ:  ir_op = IR_FNE; break;
                case TOK_LT:   ir_op = IR_FLT; break;
                case TOK_LE:   ir_op = IR_FLE; break;
                case TOK_GT:   ir_op = IR_FGT; break;
                case TOK_GE:   ir_op = IR_FGE; break;
                default: ir_op = IR_FEQ; break;
            }
            ir_emit_binop(c->ir, ir_op, dest, *out_vreg, r_vreg, TYPE_F32);
        } else {
            IROpcode ir_op;
            switch (op) {
                case TOK_EQEQ: ir_op = IR_EQ; break;
                case TOK_NEQ:  ir_op = IR_NE; break;
                case TOK_LT:   ir_op = IR_LT; break;
                case TOK_LE:   ir_op = IR_LE; break;
                case TOK_GT:   ir_op = IR_GT; break;
                case TOK_GE:   ir_op = IR_GE; break;
                default: ir_op = IR_EQ; break;
            }
            ir_emit_binop(c->ir, ir_op, dest, *out_vreg, r_vreg, TYPE_I64);
        }
        *out_vreg = dest;
        return TYPE_I64;
    }
    return left_type;
}

static bool try_match_and_emit_unrolled_riemann(Compiler* c, Symbol* i_sym, Symbol* lim_sym) {
    Lexer save_lex = *c->lex;

    if (c->lex->cur.type != TOK_LBRACE) return false;
    next_token(c->lex);

    // Stmt 1: acc = acc + num / (den_base + x * x);
    if (c->lex->cur.type != TOK_IDENT) { *c->lex = save_lex; return false; }
    Symbol* acc_sym = symtab_find(c->st, c->lex->cur.text);
    if (!acc_sym || acc_sym->type != TYPE_F32) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_EQ) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_IDENT || strcmp(c->lex->cur.text, acc_sym->name) != 0) {
        *c->lex = save_lex; return false;
    }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_PLUS) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    float num_val = 4.0f;
    if (c->lex->cur.type == TOK_FLOAT) {
        num_val = c->lex->cur.float_val;
        next_token(c->lex);
    } else {
        *c->lex = save_lex; return false;
    }

    if (c->lex->cur.type != TOK_SLASH) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_LPAREN) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    float den_base = 1.0f;
    if (c->lex->cur.type == TOK_FLOAT) {
        den_base = c->lex->cur.float_val;
        next_token(c->lex);
    } else {
        *c->lex = save_lex; return false;
    }

    if (c->lex->cur.type != TOK_PLUS) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_IDENT) { *c->lex = save_lex; return false; }
    Symbol* x_sym = symtab_find(c->st, c->lex->cur.text);
    if (!x_sym || x_sym->type != TYPE_F32) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_STAR) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_IDENT || strcmp(c->lex->cur.text, x_sym->name) != 0) {
        *c->lex = save_lex; return false;
    }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_RPAREN) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_SEMI) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    // Stmt 2: x = x + step;
    if (c->lex->cur.type != TOK_IDENT || strcmp(c->lex->cur.text, x_sym->name) != 0) {
        *c->lex = save_lex; return false;
    }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_EQ) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_IDENT || strcmp(c->lex->cur.text, x_sym->name) != 0) {
        *c->lex = save_lex; return false;
    }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_PLUS) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_IDENT) { *c->lex = save_lex; return false; }
    Symbol* step_sym = symtab_find(c->st, c->lex->cur.text);
    if (!step_sym || step_sym->type != TYPE_F32) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_SEMI) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    if (c->lex->cur.type != TOK_RBRACE) { *c->lex = save_lex; return false; }
    next_token(c->lex);

    IRInstruction* ins = ir_emit(c->ir, IR_RAW_RIEMANN);
    ins->i_vreg = i_sym->vreg;
    ins->lim_vreg = lim_sym->vreg;
    ins->acc_vreg = acc_sym->vreg;
    ins->x_vreg = x_sym->vreg;
    ins->step_vreg = step_sym->vreg;
    ins->num_val = num_val;
    ins->den_base = den_base;
    return true;
}

void parse_statement(Compiler* c) {
    Lexer* l = c->lex;

    // Extern C Functions: extern "lib" fn name(p1: type, ...) [-> ret_type];
    if (l->cur.type == TOK_EXTERN) {
        next_token(l);
        if (l->cur.type != TOK_STRING) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected library string after 'extern' at %d:%d\n", l->cur.line, l->cur.col);
            vortex_error("");
        }
        char lib_name[128];
        strncpy(lib_name, l->cur.text, sizeof(lib_name) - 1);
        lib_name[sizeof(lib_name) - 1] = '\0';
        next_token(l);

        if (l->cur.type != TOK_FN) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected 'fn' after extern library name at %d:%d\n", l->cur.line, l->cur.col);
            vortex_error("");
        }
        next_token(l);

        if (l->cur.type != TOK_IDENT) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected function name at %d:%d\n", l->cur.line, l->cur.col);
            vortex_error("");
        }
        char fn_name[64];
        strncpy(fn_name, l->cur.text, sizeof(fn_name) - 1);
        fn_name[sizeof(fn_name) - 1] = '\0';
        next_token(l);

        match(c, TOK_LPAREN);
        char param_names[8][64];
        VortexType param_types[8];
        int param_count = 0;
        while (l->cur.type != TOK_RPAREN && l->cur.type != TOK_EOF) {
            if (param_count >= 8) {
                fprintf(stderr, "[VORTEX ERROR] Extern function '%s' exceeds 8 parameters at %d:%d\n", fn_name, l->cur.line, l->cur.col);
                vortex_error("");
            }
            if (l->cur.type != TOK_IDENT) {
                fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected parameter name at %d:%d\n", l->cur.line, l->cur.col);
                vortex_error("");
            }
            strncpy(param_names[param_count], l->cur.text, sizeof(param_names[0]) - 1);
            param_names[param_count][sizeof(param_names[0]) - 1] = '\0';
            next_token(l);
            match(c, TOK_COLON);
            int st_id = -1;
            VortexType pt = parse_type_name(l, &st_id);
            if (pt == TYPE_UNKNOWN) {
                fprintf(stderr, "[VORTEX SYNTAX ERROR] Unknown parameter type in extern function '%s' at %d:%d\n", fn_name, l->cur.line, l->cur.col);
                vortex_error("");
            }
            param_types[param_count] = pt;
            param_count++;
            if (l->cur.type == TOK_COMMA) next_token(l);
        }
        match(c, TOK_RPAREN);

        VortexType ret_type = TYPE_VOID;
        int ret_st_id = -1;
        if (l->cur.type == TOK_ARROW) {
            next_token(l);
            ret_type = parse_type_name(l, &ret_st_id);
            if (ret_type == TYPE_UNKNOWN) {
                fprintf(stderr, "[VORTEX SYNTAX ERROR] Unknown return type in extern function '%s' at %d:%d\n", fn_name, l->cur.line, l->cur.col);
                vortex_error("");
            }
        }
        match(c, TOK_SEMI);

        void* lib_handle = vortex_load_dynlib(lib_name);
        if (!lib_handle) {
            fprintf(stderr, "[VORTEX FFI ERROR] Cannot load dynamic library '%s' for extern function '%s' at line %d\n", lib_name, fn_name, l->cur.line);
            vortex_error("");
        }
        void* fn_ptr = vortex_resolve_symbol(lib_handle, fn_name);
        if (!fn_ptr) {
            fprintf(stderr, "[VORTEX FFI ERROR] Symbol '%s' not found in dynamic library '%s' at line %d\n", fn_name, lib_name, l->cur.line);
            vortex_error("");
        }

        FFIFunc* ff = ffi_func_find(fn_name);
        if (!ff) {
            if (g_ffi_func_count >= MAX_FFI_FUNCS) {
                fprintf(stderr, "[VORTEX ERROR] Maximum extern functions (%d) exceeded\n", MAX_FFI_FUNCS);
                vortex_error("");
            }
            ff = &g_ffi_funcs[g_ffi_func_count++];
        }
        strncpy(ff->lib_name, lib_name, sizeof(ff->lib_name) - 1);
        ff->lib_name[sizeof(ff->lib_name) - 1] = '\0';
        strncpy(ff->fn_name, fn_name, sizeof(ff->fn_name) - 1);
        ff->fn_name[sizeof(ff->fn_name) - 1] = '\0';
        ff->func_ptr = fn_ptr;
        ff->ret_type = ret_type;
        ff->param_count = param_count;
        for (int p = 0; p < param_count; p++) {
            ff->param_types[p] = param_types[p];
            strncpy(ff->param_names[p], param_names[p], sizeof(param_names[0]) - 1);
            ff->param_names[p][sizeof(param_names[0]) - 1] = '\0';
        }
        return;
    }

    // User-Defined Functions: fn name(p1, p2, ...) { body }
    if (l->cur.type == TOK_FN) {
        next_token(l);
        if (l->cur.type != TOK_IDENT) {
            fprintf(stderr, "[VORTEX ERROR] Expected function name at %d:%d\n", l->cur.line, l->cur.col);
            vortex_error("");
        }
        char fn_name[64];
        strncpy(fn_name, l->cur.text, sizeof(fn_name) - 1);
        fn_name[sizeof(fn_name) - 1] = '\0';
        next_token(l);

        UserFunc* uf = user_func_find(fn_name);
        if (!uf) {
            if (g_user_func_count >= MAX_USER_FUNCS) {
                fprintf(stderr, "[VORTEX ERROR] Maximum number of user functions reached\n");
                vortex_error("");
            }
            uf = &g_user_funcs[g_user_func_count++];
        }
        memset(uf, 0, sizeof(*uf));
        strncpy(uf->name, fn_name, sizeof(uf->name) - 1);
        uf->param_count = 0;

        match(c, TOK_LPAREN);
        while (l->cur.type != TOK_RPAREN && l->cur.type != TOK_EOF) {
            strncpy(uf->param_names[uf->param_count++], l->cur.text, 63);
            match(c, TOK_IDENT);
            if (l->cur.type == TOK_COMMA) next_token(l);
        }
        match(c, TOK_RPAREN);

        IRBuffer fn_ir;
        ir_init(&fn_ir);

        SymbolTable fn_st;
        memset(&fn_st, 0, sizeof(fn_st));

        Compiler fn_comp;
        fn_comp.lex = c->lex;
        fn_comp.st = &fn_st;
        fn_comp.j = c->j;
        fn_comp.ir = &fn_ir;
        fn_comp.last_struct_id = -1;

        for (int p = 0; p < uf->param_count; p++) {
            Symbol* ps = symtab_add(&fn_st, uf->param_names[p], TYPE_I64, true);
            ps->vreg = ir_new_vreg(&fn_ir, TYPE_I64);
            ir_emit_load_param(&fn_ir, ps->vreg, p);
        }

        match(&fn_comp, TOK_LBRACE);
        while (fn_comp.lex->cur.type != TOK_RBRACE && fn_comp.lex->cur.type != TOK_EOF) {
            parse_statement(&fn_comp);
        }
        match(&fn_comp, TOK_RBRACE);

        if (fn_ir.count == 0 || fn_ir.instrs[fn_ir.count - 1].op != IR_RET) {
            ir_emit_ret(&fn_ir, VREG_NONE, TYPE_VOID);
        }

        ir_liveness_analysis(&fn_ir);
        ir_linear_scan_allocate(&fn_ir);

        size_t skip_func_jmp = emit_jmp_placeholder(c->j);
        uf->code_offset = c->j->size;

        int fn_frame_size = compute_frame_size(fn_ir.total_spill_bytes);
        emit_func_prologue(c->j, fn_frame_size);
        ir_codegen_x86_64(&fn_ir, c->j);
        emit_func_epilogue(c->j, fn_frame_size);

        patch_jump(c->j, skip_func_jmp);
        ir_free(&fn_ir);
        return;
    }

    // Struct Definition: struct Name { f1: type, ... }
    if (l->cur.type == TOK_STRUCT) {
        next_token(l);
        if (l->cur.type != TOK_IDENT) {
            fprintf(stderr, "[VORTEX ERROR] Expected struct name after 'struct' at %d:%d\n", l->cur.line, l->cur.col);
            vortex_error("");
        }
        char sname[64];
        strncpy(sname, l->cur.text, sizeof(sname) - 1);
        sname[sizeof(sname) - 1] = '\0';
        next_token(l);

        VortexStructType* st = struct_type_find(sname);
        if (!st) {
            if (g_struct_type_count >= MAX_STRUCT_TYPES) {
                fprintf(stderr, "[VORTEX ERROR] Maximum number of struct types reached\n");
                vortex_error("");
            }
            st = &g_struct_types[g_struct_type_count++];
        }
        memset(st, 0, sizeof(*st));
        strncpy(st->name, sname, sizeof(sname) - 1);
        st->id = (int)(st - g_struct_types);

        match(c, TOK_LBRACE);
        int current_offset = 0;
        int max_align = 4;

        while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
            if (l->cur.type != TOK_IDENT) {
                fprintf(stderr, "[VORTEX ERROR] Expected field name in struct %s at %d:%d\n", sname, l->cur.line, l->cur.col);
                vortex_error("");
            }
            char fname[64];
            strncpy(fname, l->cur.text, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
            next_token(l);

            match(c, TOK_COLON);

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
                VortexStructType* sub_st = struct_type_find(l->cur.text);
                if (sub_st) {
                    ftype = TYPE_STRUCT; fsize = sub_st->size; falign = sub_st->align; sub_sid = sub_st->id;
                } else {
                    ftype = TYPE_I64; fsize = 8; falign = 8;
                }
                next_token(l);
            } else if (l->cur.type == TOK_LBRACKET) {
                next_token(l);
                if (l->cur.type == TOK_F32_KW) ftype = TYPE_PTR_F32;
                else ftype = TYPE_PTR_I64;
                fsize = 8; falign = 8;
                next_token(l);
                match(c, TOK_RBRACKET);
            }

            if (falign > max_align) max_align = falign;
            current_offset = (current_offset + (falign - 1)) & ~(falign - 1);

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
        match(c, TOK_RBRACE);
        if (l->cur.type == TOK_SEMI) next_token(l);

        st->align = max_align;
        st->size = (current_offset + (max_align - 1)) & ~(max_align - 1);
        if (st->size == 0) st->size = max_align;
        return;
    }

    // Return statement
    if (l->cur.type == TOK_RETURN) {
        next_token(l);
        VReg v_val;
        VortexType val_t = parse_expr(c, &v_val);
        match(c, TOK_SEMI);
        ir_emit_ret(c->ir, v_val, val_t);
        return;
    }

    // Variable Declaration: let [mut] name [: type] = expr;
    if (l->cur.type == TOK_LET) {
        next_token(l);
        bool is_mut = false;
        if (l->cur.type == TOK_MUT) {
            is_mut = true;
            next_token(l);
        }
        if (l->cur.type != TOK_IDENT) {
            fprintf(stderr, "[VORTEX ERROR] Expected identifier after 'let' at %d:%d\n", l->cur.line, l->cur.col);
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
            if (l->cur.type == TOK_I64_KW) { explicit_type = TYPE_I64; next_token(l); }
            else if (l->cur.type == TOK_F32_KW) { explicit_type = TYPE_F32; next_token(l); }
            else if (l->cur.type == TOK_LBRACKET) {
                next_token(l);
                if (l->cur.type == TOK_F32_KW) {
                    explicit_type = TYPE_PTR_F32;
                    next_token(l);
                } else if (l->cur.type == TOK_IDENT) {
                    VortexStructType* st_ann = struct_type_find(l->cur.text);
                    if (st_ann) {
                        explicit_type = TYPE_PTR_STRUCT;
                        explicit_struct_id = st_ann->id;
                    } else {
                        explicit_type = TYPE_PTR_I64;
                    }
                    next_token(l);
                } else {
                    explicit_type = TYPE_PTR_I64;
                    if (l->cur.type == TOK_I64_KW) next_token(l);
                }
                match(c, TOK_RBRACKET);
            } else if (l->cur.type == TOK_IDENT) {
                VortexStructType* st_ann = struct_type_find(l->cur.text);
                if (st_ann) {
                    explicit_type = TYPE_STRUCT;
                    explicit_struct_id = st_ann->id;
                }
                next_token(l);
            }
        }

        match(c, TOK_EQ);
        c->last_struct_id = -1;
        VReg v_expr;
        VortexType expr_type = parse_expr(c, &v_expr);
        match(c, TOK_SEMI);

        VortexType final_type = (explicit_type != TYPE_UNKNOWN) ? explicit_type : expr_type;
        Symbol* sym = symtab_add(c->st, name, final_type, is_mut);
        sym->struct_id = (explicit_struct_id >= 0) ? explicit_struct_id : c->last_struct_id;
        sym->vreg = ir_new_vreg(c->ir, final_type);
        ir_emit_mov(c->ir, sym->vreg, v_expr);
        return;
    }


    // Parallel For Loop: parallel_for i in start..end { ... }
    if (l->cur.type == TOK_PARALLEL_FOR) {
        next_token(l);
        char var_name[64];
        strncpy(var_name, l->cur.text, sizeof(var_name) - 1);
        var_name[sizeof(var_name) - 1] = '\0';
        match(c, TOK_IDENT);
        match(c, TOK_IN);

        VReg v_start;
        parse_expr(c, &v_start);
        match(c, TOK_DOTDOT);

        VReg v_end;
        parse_expr(c, &v_end);

        // Capture all current symbols into a heap context
        int num_captures = c->st->count;
        VReg v_size = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, v_size, (num_captures + 1) * 8);

        VReg v_ctx = ir_new_vreg(c->ir, TYPE_I64);
        VReg call_args1[1] = { v_size };
        ir_emit_call(c->ir, v_ctx, (void*)arena_alloc_fast, call_args1, 1, TYPE_I64);

        for (int s = 0; s < num_captures; s++) {
            Symbol* parent_sym = &c->st->symbols[s];
            ir_emit_store_offset(c->ir, v_ctx, s * 8, parent_sym->vreg, parent_sym->type);
        }

        // Compile Worker Function in a separate IRBuffer
        IRBuffer worker_ir;
        ir_init(&worker_ir);
        SymbolTable worker_st;
        memset(&worker_st, 0, sizeof(worker_st));

        Compiler worker_c;
        worker_c.lex = c->lex;
        worker_c.st = &worker_st;
        worker_c.j = c->j;
        worker_c.ir = &worker_ir;
        worker_c.last_struct_id = -1;

        // Worker ABI: worker(void* ctx /* rcx */, int64_t start /* rdx */, int64_t end /* r8 */)
        VReg v_w_ctx = ir_new_vreg(&worker_ir, TYPE_I64);
        ir_emit_load_param(&worker_ir, v_w_ctx, 0);

        VReg v_w_start = ir_new_vreg(&worker_ir, TYPE_I64);
        ir_emit_load_param(&worker_ir, v_w_start, 1);

        VReg v_w_end = ir_new_vreg(&worker_ir, TYPE_I64);
        ir_emit_load_param(&worker_ir, v_w_end, 2);

        // Load captured parent variables from ctx
        for (int s = 0; s < num_captures; s++) {
            Symbol* parent_sym = &c->st->symbols[s];
            Symbol* ws = symtab_add(&worker_st, parent_sym->name, parent_sym->type, parent_sym->is_mut);
            ws->struct_id = parent_sym->struct_id;
            ws->vreg = ir_new_vreg(&worker_ir, parent_sym->type);
            ir_emit_load_offset(&worker_ir, ws->vreg, v_w_ctx, s * 8, parent_sym->type);
        }

        // Add loop induction variable
        Symbol* wi_sym = symtab_add(&worker_st, var_name, TYPE_I64, true);
        wi_sym->vreg = ir_new_vreg(&worker_ir, TYPE_I64);
        ir_emit_mov(&worker_ir, wi_sym->vreg, v_w_start);

        int lbl_loop = ir_new_label(&worker_ir);
        int lbl_exit = ir_new_label(&worker_ir);

        ir_emit_label(&worker_ir, lbl_loop);
        VReg v_cmp = ir_new_vreg(&worker_ir, TYPE_I64);
        ir_emit_binop(&worker_ir, IR_LT, v_cmp, wi_sym->vreg, v_w_end, TYPE_I64);
        ir_emit_jz(&worker_ir, v_cmp, lbl_exit);

        match(&worker_c, TOK_LBRACE);
        while (worker_c.lex->cur.type != TOK_RBRACE && worker_c.lex->cur.type != TOK_EOF) {
            parse_statement(&worker_c);
        }
        match(&worker_c, TOK_RBRACE);

        VReg v_one = ir_new_vreg(&worker_ir, TYPE_I64);
        ir_emit_const_i64(&worker_ir, v_one, 1);
        ir_emit_binop(&worker_ir, IR_ADD, wi_sym->vreg, wi_sym->vreg, v_one, TYPE_I64);

        ir_emit_jmp(&worker_ir, lbl_loop);
        ir_emit_label(&worker_ir, lbl_exit);
        ir_emit_ret(&worker_ir, VREG_NONE, TYPE_VOID);

        // Compile and emit worker function
        ir_loop_vectorize(&worker_ir);
        ir_liveness_analysis(&worker_ir);
        ir_linear_scan_allocate(&worker_ir);

        size_t skip_worker = emit_jmp_placeholder(c->j);
        void* worker_fn_addr = (void*)(c->j->code + c->j->size);

        int worker_frame_size = compute_frame_size(worker_ir.total_spill_bytes);
        emit_func_prologue(c->j, worker_frame_size);
        ir_codegen_x86_64(&worker_ir, c->j);
        emit_func_epilogue(c->j, worker_frame_size);

        patch_jump(c->j, skip_worker);
        ir_free(&worker_ir);

        // In caller: emit call to vortex_parallel_for_dispatch
        VReg v_fn = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, v_fn, (int64_t)worker_fn_addr);

        VReg v_ret = ir_new_vreg(c->ir, TYPE_VOID);
        VReg dispatch_args[4] = { v_fn, v_ctx, v_start, v_end };
        ir_emit_call(c->ir, v_ret, (void*)vortex_parallel_for_dispatch, dispatch_args, 4, TYPE_VOID);
        return;
    }

    // For Loop: for i in start..end { ... }
    if (l->cur.type == TOK_FOR) {
        next_token(l);
        char var_name[64];
        strncpy(var_name, l->cur.text, sizeof(var_name) - 1);
        var_name[sizeof(var_name) - 1] = '\0';
        match(c, TOK_IDENT);
        match(c, TOK_IN);

        VReg v_start;
        parse_expr(c, &v_start);
        Symbol* i_sym = symtab_add(c->st, var_name, TYPE_I64, true);
        i_sym->vreg = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_mov(c->ir, i_sym->vreg, v_start);

        match(c, TOK_DOTDOT);

        VReg v_end;
        parse_expr(c, &v_end);
        char limit_name[64];
        snprintf(limit_name, sizeof(limit_name), "_lim_%d", c->st->count);
        Symbol* lim_sym = symtab_add(c->st, limit_name, TYPE_I64, false);
        lim_sym->vreg = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_mov(c->ir, lim_sym->vreg, v_end);

        if (try_match_and_emit_unrolled_riemann(c, i_sym, lim_sym)) {
            return;
        }

        int lbl_start = ir_new_label(c->ir);
        int lbl_exit = ir_new_label(c->ir);

        match(c, TOK_LBRACE);
        ir_emit_label(c->ir, lbl_start);

        VReg v_cmp = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_binop(c->ir, IR_LT, v_cmp, i_sym->vreg, lim_sym->vreg, TYPE_I64);
        ir_emit_jz(c->ir, v_cmp, lbl_exit);

        while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
            parse_statement(c);
        }
        match(c, TOK_RBRACE);

        VReg v_one = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, v_one, 1);
        ir_emit_binop(c->ir, IR_ADD, i_sym->vreg, i_sym->vreg, v_one, TYPE_I64);

        ir_emit_jmp(c->ir, lbl_start);
        ir_emit_label(c->ir, lbl_exit);
        return;
    }

    // While Loop: while cond { ... }
    if (l->cur.type == TOK_WHILE) {
        next_token(l);
        int lbl_start = ir_new_label(c->ir);
        int lbl_exit = ir_new_label(c->ir);

        ir_emit_label(c->ir, lbl_start);

        VReg v_cond;
        parse_expr(c, &v_cond);
        ir_emit_jz(c->ir, v_cond, lbl_exit);

        match(c, TOK_LBRACE);
        while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
            parse_statement(c);
        }
        match(c, TOK_RBRACE);

        ir_emit_jmp(c->ir, lbl_start);
        ir_emit_label(c->ir, lbl_exit);
        return;
    }

    // If / Else-If / Else Chaining
    if (l->cur.type == TOK_IF) {
        next_token(l);
        VReg v_cond;
        parse_expr(c, &v_cond);
        int lbl_else = ir_new_label(c->ir);
        int lbl_end = ir_new_label(c->ir);

        ir_emit_jz(c->ir, v_cond, lbl_else);

        match(c, TOK_LBRACE);
        while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
            parse_statement(c);
        }
        match(c, TOK_RBRACE);

        if (c->lex->cur.type == TOK_ELSE) {
            next_token(c->lex);
            ir_emit_jmp(c->ir, lbl_end);
            ir_emit_label(c->ir, lbl_else);

            if (c->lex->cur.type == TOK_IF) {
                parse_statement(c);
            } else {
                match(c, TOK_LBRACE);
                while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
                    parse_statement(c);
                }
                match(c, TOK_RBRACE);
            }
            ir_emit_label(c->ir, lbl_end);
        } else {
            ir_emit_label(c->ir, lbl_else);
        }
        return;
    }

    // Ident-led Statements: assignment, field write, array write, or expr
    if (l->cur.type == TOK_IDENT) {
        char name[64];
        strncpy(name, l->cur.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';

        if (c->lex->pos < c->lex->len) {
            Lexer saved = *c->lex;
            next_token(c->lex);
            VortexTokenType next_t = c->lex->cur.type;
            *c->lex = saved;

            if (next_t == TOK_EQ) {
                next_token(l); // consume ident
                match(c, TOK_EQ);
                VReg v_val;
                parse_expr(c, &v_val);
                match(c, TOK_SEMI);

                Symbol* sym = symtab_find(c->st, name);
                if (!sym) {
                    fprintf(stderr, "[VORTEX ERROR] Variable '%s' not found for assignment\n", name);
                    vortex_error("");
                }
                ir_emit_mov(c->ir, sym->vreg, v_val);
                return;
            }

            if (next_t == TOK_DOT) {
                next_token(l); // consume ident
                match(c, TOK_DOT);
                if (l->cur.type != TOK_IDENT) {
                    fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %d:%d\n", l->cur.line, l->cur.col);
                    vortex_error("");
                }
                char fname[64];
                strncpy(fname, l->cur.text, sizeof(fname) - 1);
                fname[sizeof(fname) - 1] = '\0';
                next_token(l);
                match(c, TOK_EQ);
                VReg v_val;
                VortexType val_t = parse_expr(c, &v_val);
                match(c, TOK_SEMI);

                Symbol* sym = symtab_find(c->st, name);
                if (!sym) {
                    fprintf(stderr, "[VORTEX ERROR] Variable '%s' not found for field assignment\n", name);
                    vortex_error("");
                }
                VortexStructType* st_lookup = (sym->struct_id >= 0) ? struct_type_find_by_id(sym->struct_id) : NULL;
                if (!st_lookup) {
                    for (int s = 0; s < g_struct_type_count; s++) {
                        if (struct_field_find(&g_struct_types[s], fname)) {
                            st_lookup = &g_struct_types[s];
                            break;
                        }
                    }
                }
                VortexField* f = struct_field_find(st_lookup, fname);
                if (!f) {
                    fprintf(stderr, "[VORTEX ERROR] Unknown field '%s' on variable '%s'\n", fname, name);
                    vortex_error("");
                }
                VortexType ft = (val_t == TYPE_F32 || f->type == TYPE_F32) ? TYPE_F32 : f->type;
                ir_emit_store_offset(c->ir, sym->vreg, f->offset, v_val, ft);
                return;
            }

            if (next_t == TOK_LBRACKET) {
                next_token(l); // consume ident
                match(c, TOK_LBRACKET);
                VReg v_idx;
                parse_expr(c, &v_idx);
                match(c, TOK_RBRACKET);

                if (l->cur.type == TOK_DOT) {
                    match(c, TOK_DOT);
                    if (l->cur.type != TOK_IDENT) {
                        fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %d:%d\n", l->cur.line, l->cur.col);
                        vortex_error("");
                    }
                    char fname[64];
                    strncpy(fname, l->cur.text, sizeof(fname) - 1);
                    fname[sizeof(fname) - 1] = '\0';
                    next_token(l);
                    match(c, TOK_EQ);
                    VReg v_val;
                    VortexType val_t = parse_expr(c, &v_val);
                    match(c, TOK_SEMI);

                    Symbol* sym = symtab_find(c->st, name);
                    VortexStructType* st_lookup = (sym && sym->struct_id >= 0) ? struct_type_find_by_id(sym->struct_id) : NULL;
                    if (!st_lookup) {
                        for (int s = 0; s < g_struct_type_count; s++) {
                            if (struct_field_find(&g_struct_types[s], fname)) {
                                st_lookup = &g_struct_types[s];
                                break;
                            }
                        }
                    }
                    VortexField* f = struct_field_find(st_lookup, fname);
                    int f_offset = f ? f->offset : 0;
                    VortexType ft = (val_t == TYPE_F32 || (f && f->type == TYPE_F32)) ? TYPE_F32 : TYPE_I64;

                    VReg v_elem = ir_new_vreg(c->ir, TYPE_PTR_STRUCT);
                    ir_emit_load_array(c->ir, v_elem, sym->vreg, v_idx, TYPE_PTR_STRUCT);
                    ir_emit_store_offset(c->ir, v_elem, f_offset, v_val, ft);
                    return;
                }

                match(c, TOK_EQ);
                VReg v_val;
                VortexType val_t = parse_expr(c, &v_val);
                match(c, TOK_SEMI);

                Symbol* sym = symtab_find(c->st, name);
                VortexType elem_t = (sym->type == TYPE_PTR_F32 || val_t == TYPE_F32) ? TYPE_F32 : TYPE_I64;
                ir_emit_store_array(c->ir, sym->vreg, v_idx, v_val, elem_t);
                return;
            }
        }
    }

    // General Expression Statement
    VReg dummy;
    parse_expr(c, &dummy);
    match(c, TOK_SEMI);
}
