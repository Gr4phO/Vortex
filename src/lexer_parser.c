/*
 * VORTEX PROGRAMMING LANGUAGE - LEXER, TYPE/UNSAFE/REGION RESOLVER & IR FRONTEND PARSER
 */

#include "vortex.h"
#ifdef fprintf
#undef fprintf
#endif
#define fprintf vortex_diag_emit

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

void lexer_init(Lexer* lex, const char* source) {
    lex->src = source ? source : "";
    lex->pos = 0;
    lex->len = strlen(lex->src);
    lex->line = 1;
    lex->col = 1;
    if (lex->len >= 3 &&
        (unsigned char)lex->src[0] == 0xEF &&
        (unsigned char)lex->src[1] == 0xBB &&
        (unsigned char)lex->src[2] == 0xBF) {
        lex->pos = 3;
    }
    next_token(lex);
}

void next_token(Lexer* lex) {
    if (lex->pos == 0 && lex->len >= 3 &&
        (unsigned char)lex->src[0] == 0xEF &&
        (unsigned char)lex->src[1] == 0xBB &&
        (unsigned char)lex->src[2] == 0xBF) {
        lex->pos = 3;
    }
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
        lex->cur.line = lex->line;
        lex->cur.col = lex->col;
        return;
    }

    lex->cur.line = lex->line;
    lex->cur.col = lex->col;
    char c = lex->src[lex->pos];

    if (isdigit((unsigned char)c)) {
        size_t start = lex->pos;
        if (c == '0' && lex->pos + 1 < lex->len &&
            (lex->src[lex->pos + 1] == 'x' || lex->src[lex->pos + 1] == 'X')) {
            lex->pos += 2; lex->col += 2;
            while (lex->pos < lex->len && isxdigit((unsigned char)lex->src[lex->pos])) {
                lex->pos++; lex->col++;
            }
            size_t len = lex->pos - start;
            if (len >= sizeof(lex->cur.text)) len = sizeof(lex->cur.text) - 1;
            memcpy(lex->cur.text, &lex->src[start], len);
            lex->cur.text[len] = '\0';
            lex->cur.type = TOK_INT;
            lex->cur.int_val = (int64_t)strtoull(lex->cur.text, NULL, 16);
            return;
        }

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
        else if (strcmp(lex->cur.text, "unsafe") == 0) lex->cur.type = TOK_UNSAFE;
        else if (strcmp(lex->cur.text, "as") == 0) lex->cur.type = TOK_AS;
        else if (strcmp(lex->cur.text, "import") == 0) lex->cur.type = TOK_IMPORT;
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
        case '?': lex->cur.type = TOK_QUESTION; break;
        case '#': lex->cur.type = TOK_HASH; break;
        default:
            fprintf(stderr, "[VORTEX ERROR] Unknown character '%c' at %s:%d:%d\n",
                    c, g_current_filename ? g_current_filename : "<input>", lex->line, lex->col);
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

static VortexType parse_type_spec_full(Lexer* l, int* out_struct_id, bool* out_nullable) {
    if (out_struct_id) *out_struct_id = -1;
    if (out_nullable) *out_nullable = false;

    if (l->cur.type == TOK_STAR) {
        next_token(l);
        if (l->cur.type == TOK_F32_KW) { next_token(l); return TYPE_PTR_F32; }
        if (l->cur.type == TOK_I64_KW) { next_token(l); return TYPE_PTR_I64; }
        if (l->cur.type == TOK_IDENT) {
            if (strcmp(l->cur.text, "i32") == 0) { next_token(l); return TYPE_PTR_I32; }
            VortexStructType* st = struct_type_find(l->cur.text);
            if (st) {
                if (out_struct_id) *out_struct_id = st->id;
                next_token(l);
                return TYPE_PTR_STRUCT;
            }
        }
        return TYPE_PTR;
    }

    if (l->cur.type == TOK_LBRACKET) {
        next_token(l);
        VortexType elem_ptr_t = TYPE_PTR_I64;
        if (l->cur.type == TOK_F32_KW) {
            elem_ptr_t = TYPE_PTR_F32;
            next_token(l);
        } else if (l->cur.type == TOK_I64_KW) {
            elem_ptr_t = TYPE_PTR_I64;
            next_token(l);
        } else if (l->cur.type == TOK_IDENT) {
            if (strcmp(l->cur.text, "i32") == 0) {
                elem_ptr_t = TYPE_PTR_I32;
            } else {
                VortexStructType* st = struct_type_find(l->cur.text);
                if (st) {
                    elem_ptr_t = TYPE_PTR_STRUCT;
                    if (out_struct_id) *out_struct_id = st->id;
                }
            }
            next_token(l);
        }
        if (l->cur.type == TOK_RBRACKET) next_token(l);
        return elem_ptr_t;
    }

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
            if (l->cur.type == TOK_QUESTION) {
                if (out_nullable) *out_nullable = true;
                next_token(l);
            }
            return TYPE_STR;
        }
        if (strcmp(l->cur.text, "ptr") == 0) {
            next_token(l);
            if (l->cur.type == TOK_QUESTION) {
                if (out_nullable) *out_nullable = true;
                next_token(l);
            }
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

VortexType parse_type_name(Lexer* l, int* out_struct_id) {
    return parse_type_spec_full(l, out_struct_id, NULL);
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
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    s->type = type;
    s->struct_id = -1;
    s->is_mut = is_mut;
    s->stack_offset = st->stack_alloc_bytes;
    s->vreg = -1;
    s->len_vreg = VREG_NONE;
    s->known_len = -1;
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

/* --- Safety & Static Region Helpers --- */
static bool is_pointer_type(VortexType t) {
    return t == TYPE_PTR || t == TYPE_PTR_I64 || t == TYPE_PTR_I32 ||
           t == TYPE_PTR_F32 || t == TYPE_PTR_STRUCT || t == TYPE_STRUCT || t == TYPE_STR;
}

static bool is_scalar_int_type(VortexType t) {
    return t == TYPE_I64 || t == TYPE_I32;
}

static void require_unsafe(Compiler* c, const char* what, int line, int col) {
    if (c->unsafe_depth <= 0) {
        fprintf(stderr,
                "[VORTEX SAFETY ERROR] %s requires an 'unsafe' block or 'unsafe fn' at %s:%d:%d\n",
                what, g_current_filename ? g_current_filename : "<input>", line, col);
        vortex_error("");
    }
}

static bool region_is_popped(Compiler* c, int rid) {
    if (rid <= 0) return false;
    for (int i = 0; i < c->popped_region_count; i++) {
        if (c->popped_regions[i] == rid) return true;
    }
    return false;
}

static void mark_region_popped(Compiler* c, int rid) {
    if (rid <= 0 || region_is_popped(c, rid)) return;
    if (c->popped_region_count < 256) {
        c->popped_regions[c->popped_region_count++] = rid;
    }
}

static void check_symbol_alive(Compiler* c, Symbol* sym, int line, int col) {
    if (sym && sym->is_ref && region_is_popped(c, sym->region_id)) {
        fprintf(stderr,
                "[VORTEX REGION ERROR] Use of reference '%s' after arena 'scope_pop()' at %s:%d:%d\n",
                sym->name, g_current_filename ? g_current_filename : "<input>", line, col);
        vortex_error("");
    }
}

static void stamp_ir_loc(IRBuffer* ir, int from_idx, int line) {
    const char* file = g_current_filename ? g_current_filename : "<input>";
    for (int i = from_idx; i < ir->count; i++) {
        if (ir->instrs[i].line == 0) ir->instrs[i].line = line;
        if (!ir->instrs[i].file) ir->instrs[i].file = file;
    }
}

static void set_vreg_region(IRBuffer* ir, VReg v, bool is_ref, int rid, int depth) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    ir->vregs[v].is_ref = is_ref;
    ir->vregs[v].region_id = rid;
    ir->vregs[v].region_depth = depth;
}

static bool try_get_const_i64(IRBuffer* ir, VReg v, int64_t* out_val) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return false;
    for (int i = ir->count - 1; i >= 0; i--) {
        if (ir->instrs[i].dest == v) {
            if (ir->instrs[i].op == IR_CONST_I64) {
                *out_val = ir->instrs[i].imm;
                return true;
            }
            if (ir->instrs[i].op == IR_MOV) {
                return try_get_const_i64(ir, ir->instrs[i].src1, out_val);
            }
            return false;
        }
    }
    return false;
}

/* --- Recursive-Descent Parser & IR Lowering --- */
static VortexType parse_expr(Compiler* c, VReg* out_vreg);
void parse_statement(Compiler* c);
static VortexType parse_primary(Compiler* c, VReg* out_vreg);
static VortexType parse_cast(Compiler* c, VReg* out_vreg);
static VortexType parse_multiplicative(Compiler* c, VReg* out_vreg);
static VortexType parse_additive(Compiler* c, VReg* out_vreg);
static bool try_match_and_emit_unrolled_riemann(Compiler* c, Symbol* i_sym, Symbol* lim_sym);

static void match(Compiler* c, VortexTokenType expected) {
    if (c->lex->cur.type != expected) {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Unexpected token '%s' at %s:%d:%d\n",
                c->lex->cur.text, g_current_filename ? g_current_filename : "<input>",
                c->lex->cur.line, c->lex->cur.col);
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
    int line = l->cur.line;
    int col = l->cur.col;

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

    if (l->cur.type == TOK_STAR) {
        next_token(l);
        require_unsafe(c, "Raw pointer dereference '*ptr'", line, col);
        VReg ptr_v;
        VortexType pt = parse_primary(c, &ptr_v);
        VortexType elem_t = (pt == TYPE_PTR_I32) ? TYPE_I32 :
                            (pt == TYPE_PTR_F32) ? TYPE_F32 : TYPE_I64;
        *out_vreg = ir_new_vreg(c->ir, elem_t);
        ir_emit_load_deref(c->ir, *out_vreg, ptr_v, elem_t);
        return elem_t;
    }

    if (l->cur.type == TOK_UNSAFE) {
        next_token(l);
        match(c, TOK_LBRACE);
        c->unsafe_depth++;
        VReg last_v = VREG_NONE;
        VortexType last_t = TYPE_I64;
        while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
            if (l->cur.type == TOK_LET || l->cur.type == TOK_FOR ||
                l->cur.type == TOK_PARALLEL_FOR || l->cur.type == TOK_WHILE ||
                l->cur.type == TOK_IF || l->cur.type == TOK_RETURN ||
                l->cur.type == TOK_UNSAFE || l->cur.type == TOK_STAR) {
                parse_statement(c);
                last_v = VREG_NONE;
            } else {
                bool is_assign = false;
                if (l->cur.type == TOK_IDENT && l->pos < l->len) {
                    Lexer saved = *l;
                    next_token(l);
                    if (l->cur.type == TOK_EQ || l->cur.type == TOK_DOT || l->cur.type == TOK_LBRACKET) {
                        /* Could be assignment or field/index access; peek for '=' */
                        if (l->cur.type == TOK_EQ) is_assign = true;
                        else {
                            /* scan until ';' or '}' to see if top-level '=' appears */
                            int depth = 0;
                            while (l->cur.type != TOK_EOF && l->cur.type != TOK_SEMI && l->cur.type != TOK_RBRACE) {
                                if (l->cur.type == TOK_LBRACKET || l->cur.type == TOK_LPAREN) depth++;
                                else if (l->cur.type == TOK_RBRACKET || l->cur.type == TOK_RPAREN) depth--;
                                else if (l->cur.type == TOK_EQ && depth == 0) { is_assign = true; break; }
                                next_token(l);
                            }
                        }
                    }
                    *l = saved;
                }
                if (is_assign) {
                    parse_statement(c);
                    last_v = VREG_NONE;
                } else {
                    last_t = parse_expr(c, &last_v);
                    if (l->cur.type == TOK_SEMI) next_token(l);
                }
            }
        }
        match(c, TOK_RBRACE);
        c->unsafe_depth--;
        if (last_v == VREG_NONE) {
            last_v = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_const_i64(c->ir, last_v, 0);
            last_t = TYPE_I64;
        }
        *out_vreg = last_v;
        return last_t;
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
        *out_vreg = ir_new_vreg(c->ir, TYPE_STR);
        ir_emit_const_i64(c->ir, *out_vreg, (int64_t)s);
        return TYPE_STR;
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

        /* Struct Instantiation: StructName { f1: v1, ... } */
        VortexStructType* st_def = struct_type_find(name);
        if (st_def && l->cur.type == TOK_LBRACE) {
            match(c, TOK_LBRACE);
            VReg v_sz = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_const_i64(c->ir, v_sz, st_def->size);
            VReg v_struct = ir_new_vreg(c->ir, TYPE_PTR_STRUCT);
            ir_emit_call(c->ir, v_struct, (void*)vortex_alloc_struct, &v_sz, 1, TYPE_PTR_STRUCT);
            set_vreg_region(c->ir, v_struct, true, c->current_region_id, c->current_region_depth);

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
                    fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %s:%d:%d\n",
                            g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
                    vortex_error("");
                }
                char fname[64];
                strncpy(fname, l->cur.text, sizeof(fname) - 1);
                fname[sizeof(fname) - 1] = '\0';
                next_token(l);

                VortexStructType* curr_st = struct_type_find_by_id(cur_sid);
                VortexField* f = struct_field_find(curr_st, fname);
                if (!f) {
                    fprintf(stderr, "[VORTEX ERROR] Unknown field '%s' at %s:%d:%d\n",
                            fname, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
                    vortex_error("");
                }
                if (g_compile_mode == VORTEX_MODE_SAFE && c->unsafe_depth == 0) {
                    ir_emit_ptr_check(c->ir, cur_v, g_current_filename, l->cur.line);
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

        /* Function Call: name(arg1, arg2, ...) */
        if (l->cur.type == TOK_LPAREN) {
            next_token(l);
            int num_args = 0;
            VReg arg_vregs[MAX_CALL_ARGS];
            VortexType arg_types[MAX_CALL_ARGS];
            Symbol* arg_syms[MAX_CALL_ARGS];
            memset(arg_syms, 0, sizeof(arg_syms));

            while (l->cur.type != TOK_RPAREN && l->cur.type != TOK_EOF) {
                if (num_args >= MAX_CALL_ARGS) {
                    fprintf(stderr, "[VORTEX ERROR] Function call '%s' exceeds %d arguments at %s:%d:%d\n",
                            name, MAX_CALL_ARGS, g_current_filename ? g_current_filename : "<input>", line, col);
                    vortex_error("");
                }
                if (l->cur.type == TOK_IDENT) {
                    arg_syms[num_args] = symtab_find(c->st, l->cur.text);
                }
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

            if (strcmp(name, "scope_push") == 0 && num_args == 0) {
                *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
                ir_emit_call(c->ir, *out_vreg, (void*)vortex_scope_push, arg_vregs, 0, TYPE_I64);
                int rid = ++c->region_counter;
                if (c->current_region_depth + 1 < MAX_COMPILE_REGIONS) {
                    c->current_region_depth++;
                    c->current_region_id = rid;
                    CompileRegion* cr = &c->region_stack[c->current_region_depth];
                    cr->region_id = rid;
                    cr->depth = c->current_region_depth;
                    cr->mark_name[0] = '\0';
                    cr->active = true;
                }
                c->ir->vregs[*out_vreg].region_id = rid;
                c->ir->vregs[*out_vreg].region_depth = c->current_region_depth;
                return TYPE_I64;
            }

            if (strcmp(name, "scope_pop") == 0 && num_args == 1) {
                int pop_rid = -1;
                if (arg_syms[0] && arg_syms[0]->is_region_mark) {
                    pop_rid = arg_syms[0]->region_id;
                } else if (arg_vregs[0] >= 0 && arg_vregs[0] < c->ir->vreg_count) {
                    pop_rid = c->ir->vregs[arg_vregs[0]].region_id;
                }
                if (pop_rid > 0) {
                    for (int d = c->current_region_depth; d >= 1; d--) {
                        int r = c->region_stack[d].region_id;
                        mark_region_popped(c, r);
                        c->region_stack[d].active = false;
                        c->current_region_depth = d - 1;
                        if (r == pop_rid) break;
                    }
                    c->current_region_id = (c->current_region_depth > 0)
                                           ? c->region_stack[c->current_region_depth].region_id
                                           : 0;
                } else if (c->current_region_depth > 0) {
                    mark_region_popped(c, c->region_stack[c->current_region_depth].region_id);
                    c->region_stack[c->current_region_depth].active = false;
                    c->current_region_depth--;
                    c->current_region_id = (c->current_region_depth > 0)
                                           ? c->region_stack[c->current_region_depth].region_id
                                           : 0;
                }
                *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
                ir_emit_call(c->ir, *out_vreg, (void*)vortex_scope_pop, arg_vregs, 1, TYPE_I64);
                return TYPE_I64;
            }

            if (strcmp(name, "vec_push") == 0 && num_args == 2) {
                if (arg_vregs[1] >= 0 && arg_vregs[1] < c->ir->vreg_count &&
                    c->ir->vregs[arg_vregs[1]].is_ref &&
                    arg_vregs[0] >= 0 && arg_vregs[0] < c->ir->vreg_count) {
                    int val_depth = c->ir->vregs[arg_vregs[1]].region_depth;
                    int vec_depth = c->ir->vregs[arg_vregs[0]].region_depth;
                    if (val_depth > vec_depth) {
                        fprintf(stderr,
                                "[VORTEX REGION ERROR] Cannot push inner arena reference (depth %d) into outer vector (depth %d) at %s:%d:%d\n",
                                val_depth, vec_depth, g_current_filename ? g_current_filename : "<input>", line, col);
                        vortex_error("");
                    }
                }
                if (arg_syms[0] && arg_vregs[1] >= 0 && c->ir->vregs[arg_vregs[1]].is_ref) {
                    arg_syms[0]->holds_refs = true;
                    arg_syms[0]->elem_region_id = c->ir->vregs[arg_vregs[1]].region_id;
                    arg_syms[0]->elem_region_depth = c->ir->vregs[arg_vregs[1]].region_depth;
                }
            }

            if ((strcmp(name, "vec_set") == 0 || strcmp(name, "map_set") == 0) && num_args == 3) {
                if (arg_vregs[2] >= 0 && arg_vregs[2] < c->ir->vreg_count &&
                    c->ir->vregs[arg_vregs[2]].is_ref &&
                    arg_vregs[0] >= 0 && arg_vregs[0] < c->ir->vreg_count) {
                    int val_depth = c->ir->vregs[arg_vregs[2]].region_depth;
                    int cont_depth = c->ir->vregs[arg_vregs[0]].region_depth;
                    if (val_depth > cont_depth) {
                        fprintf(stderr,
                                "[VORTEX REGION ERROR] Cannot store inner arena reference (depth %d) into outer container (depth %d) at %s:%d:%d\n",
                                val_depth, cont_depth, g_current_filename ? g_current_filename : "<input>", line, col);
                        vortex_error("");
                    }
                }
            }

            if (g_compile_mode == VORTEX_MODE_SAFE) {
                if (strcmp(name, "vec_get") == 0 && num_args == 2) {
                    VReg v_file = ir_new_vreg(c->ir, TYPE_STR);
                    ir_emit_const_i64(c->ir, v_file, (int64_t)(g_current_filename ? g_current_filename : "<input>"));
                    VReg v_line = ir_new_vreg(c->ir, TYPE_I64);
                    ir_emit_const_i64(c->ir, v_line, line);
                    VReg safe_args[4] = { arg_vregs[0], arg_vregs[1], v_file, v_line };
                    *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
                    ir_emit_call(c->ir, *out_vreg, (void*)vortex_vec_get_safe, safe_args, 4, TYPE_I64);
                    if (arg_syms[0] && arg_syms[0]->holds_refs) {
                        set_vreg_region(c->ir, *out_vreg, true,
                                        arg_syms[0]->elem_region_id, arg_syms[0]->elem_region_depth);
                    }
                    return TYPE_I64;
                }
                if (strcmp(name, "vec_set") == 0 && num_args == 3) {
                    VReg v_file = ir_new_vreg(c->ir, TYPE_STR);
                    ir_emit_const_i64(c->ir, v_file, (int64_t)(g_current_filename ? g_current_filename : "<input>"));
                    VReg v_line = ir_new_vreg(c->ir, TYPE_I64);
                    ir_emit_const_i64(c->ir, v_line, line);
                    VReg safe_args[5] = { arg_vregs[0], arg_vregs[1], arg_vregs[2], v_file, v_line };
                    *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
                    ir_emit_call(c->ir, *out_vreg, (void*)vortex_vec_set_safe, safe_args, 5, TYPE_I64);
                    return TYPE_I64;
                }
            }

            BuiltinFunc* bf = builtin_find(name);
            if (bf) {
                if (strncmp(name, "ffi_", 4) == 0 || strncmp(name, "ptr_", 4) == 0) {
                    require_unsafe(c, "Low-level pointer/dynamic FFI builtin", line, col);
                }

                VortexType ret_t = TYPE_I64;
                bool is_alloc_ref = false;
                bool is_slice = false;
                if (strcmp(name, "alloc") == 0) {
                    ret_t = TYPE_PTR; is_alloc_ref = true; is_slice = true;
                } else if (strcmp(name, "slice_i64") == 0) {
                    ret_t = TYPE_PTR_I64; is_alloc_ref = true; is_slice = true;
                } else if (strcmp(name, "slice_f32") == 0) {
                    ret_t = TYPE_PTR_F32; is_alloc_ref = true; is_slice = true;
                } else if (strcmp(name, "slice_i32") == 0) {
                    ret_t = TYPE_PTR_I32; is_alloc_ref = true; is_slice = true;
                } else if (strcmp(name, "vec_new") == 0 || strcmp(name, "map_new") == 0) {
                    ret_t = TYPE_PTR; is_alloc_ref = true;
                } else if (strcmp(name, "str_concat") == 0 || strcmp(name, "str_sub") == 0 ||
                           strcmp(name, "str_from_i64") == 0 || strcmp(name, "str_from_f32") == 0 ||
                           strcmp(name, "str_from_byte") == 0 || strcmp(name, "ptr_to_str") == 0) {
                    ret_t = TYPE_STR; is_alloc_ref = true;
                } else if (strcmp(name, "ffi_load") == 0 || strcmp(name, "ffi_symbol") == 0 ||
                           strcmp(name, "gui_window") == 0) {
                    ret_t = TYPE_PTR;
                }

                *out_vreg = ir_new_vreg(c->ir, ret_t);
                ir_emit_call(c->ir, *out_vreg, bf->func_ptr, arg_vregs, num_args, ret_t);

                if (is_alloc_ref) {
                    set_vreg_region(c->ir, *out_vreg, true, c->current_region_id, c->current_region_depth);
                }
                if (is_slice && num_args >= 1) {
                    c->ir->vregs[*out_vreg].is_slice_ptr = true;
                    if (strcmp(name, "alloc") != 0) {
                        c->ir->vregs[*out_vreg].len_vreg = arg_vregs[0];
                        int64_t kl = -1;
                        if (try_get_const_i64(c->ir, arg_vregs[0], &kl)) {
                            c->ir->vregs[*out_vreg].known_len = kl;
                        }
                    } else {
                        int64_t kb = -1;
                        if (try_get_const_i64(c->ir, arg_vregs[0], &kb)) {
                            c->ir->vregs[*out_vreg].known_len = kb; /* byte count until typed */
                        }
                    }
                }
                if (strcmp(name, "vec_get") == 0 && arg_syms[0] && arg_syms[0]->holds_refs) {
                    set_vreg_region(c->ir, *out_vreg, true,
                                    arg_syms[0]->elem_region_id, arg_syms[0]->elem_region_depth);
                }
                return ret_t;
            }

            FFIFunc* ff = ffi_func_find(name);
            if (ff) {
                require_unsafe(c, "Call to extern function", line, col);
                if (g_compile_mode == VORTEX_MODE_SAFE) {
                    for (int a = 0; a < num_args; a++) {
                        VortexType pt = (a < ff->param_count) ? ff->param_types[a] : arg_types[a];
                        if (is_pointer_type(pt)) {
                            int64_t req_align = 1;
                            if (pt == TYPE_PTR_I64) req_align = 8;
                            else if (pt == TYPE_PTR_F32 || pt == TYPE_PTR_I32 || pt == TYPE_PTR || pt == TYPE_PTR_STRUCT) req_align = 4;
                            bool nullable = (a < ff->param_count && ff->param_nullable[a]) ||
                                            (strcmp(ff->fn_name, "fflush") == 0 && a == 0);
                            int64_t flags = req_align | (nullable ? (1LL << 16) : 0);
                            ir_emit_ffi_check(c->ir, arg_vregs[a], pt, flags, ff->fn_name, a,
                                              g_current_filename ? g_current_filename : "<input>", line);
                        }
                    }
                }
                VortexType out_t = (ff->ret_type == TYPE_VOID) ? TYPE_I64 :
                                   (ff->ret_type == TYPE_F64) ? TYPE_F32 : ff->ret_type;
                *out_vreg = (ff->ret_type == TYPE_VOID) ? VREG_NONE : ir_new_vreg(c->ir, out_t);
                ir_emit_call_ffi(c->ir, *out_vreg, ff, arg_vregs, num_args, out_t);
                return out_t;
            }

            UserFunc* uf = user_func_find(name);
            if (uf) {
                if (uf->is_unsafe) {
                    require_unsafe(c, "Call to 'unsafe fn'", line, col);
                }
                VortexType rt = (uf->ret_type != TYPE_UNKNOWN && uf->ret_type != TYPE_VOID) ? uf->ret_type : TYPE_I64;
                *out_vreg = ir_new_vreg(c->ir, rt);
                ir_emit_call_user(c->ir, *out_vreg, name, arg_vregs, num_args, rt);
                if (c->ir->count > 0) {
                    IRInstruction* call_ins = &c->ir->instrs[c->ir->count - 1];
                    call_ins->imm = (c->unsafe_depth > 0) ? 1 : 0;
                    call_ins->line = line;
                    call_ins->file = g_current_filename ? g_current_filename : "<input>";
                }
                if (uf->returns_ref) {
                    set_vreg_region(c->ir, *out_vreg, true, c->current_region_id, c->current_region_depth);
                }
                if (uf->ret_struct_id >= 0) {
                    c->last_struct_id = uf->ret_struct_id;
                }
                return rt;
            }

            /* Forward-call to user function */
            *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_call_user(c->ir, *out_vreg, name, arg_vregs, num_args, TYPE_I64);
            if (c->ir->count > 0) {
                IRInstruction* call_ins = &c->ir->instrs[c->ir->count - 1];
                call_ins->imm = (c->unsafe_depth > 0) ? 1 : 0;
                call_ins->line = line;
                call_ins->file = g_current_filename ? g_current_filename : "<input>";
            }
            return TYPE_I64;
        }

        Symbol* sym = symtab_find(c->st, name);
        if (!sym) {
            fprintf(stderr, "[VORTEX ERROR] Undefined variable '%s' at %s:%d:%d\n",
                    name, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        check_symbol_alive(c, sym, line, col);

        VortexType primary_type = sym->type;
        int primary_struct_id = sym->struct_id;
        VReg cur_v = sym->vreg;

        /* Array indexing: arr[index] */
        if (l->cur.type == TOK_LBRACKET) {
            next_token(l);
            VReg v_idx;
            parse_expr(c, &v_idx);
            match(c, TOK_RBRACKET);

            VortexType elem_type = (sym->type == TYPE_PTR_F32) ? TYPE_F32 :
                                   (sym->type == TYPE_PTR_I32) ? TYPE_I32 :
                                   (sym->type == TYPE_PTR_STRUCT) ? TYPE_PTR_STRUCT : TYPE_I64;
            int32_t elem_size = (elem_type == TYPE_F32 || elem_type == TYPE_I32) ? 4 : 8;

            bool needs_bounds = (c->unsafe_depth == 0) || sym->is_slice_ptr;
            if (!sym->is_slice_ptr && c->unsafe_depth == 0) {
                require_unsafe(c, "Indexing untracked raw pointer", line, col);
            }
            if (needs_bounds && (g_compile_mode == VORTEX_MODE_SAFE || sym->is_slice_ptr)) {
                ir_emit_bounds_check(c->ir, cur_v, v_idx, sym->len_vreg, sym->known_len, elem_size,
                                     g_current_filename ? g_current_filename : "<input>", line);
            }

            VReg v_elem = ir_new_vreg(c->ir, elem_type);
            ir_emit_load_array(c->ir, v_elem, cur_v, v_idx, elem_type);
            cur_v = v_elem;
            primary_type = (elem_type == TYPE_PTR_STRUCT) ? TYPE_STRUCT : elem_type;
        }

        /* Postfix field access: .field1.field2 */
        while (l->cur.type == TOK_DOT) {
            next_token(l);
            if (l->cur.type != TOK_IDENT) {
                fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %s:%d:%d\n",
                        g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
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
                fprintf(stderr, "[VORTEX ERROR] Unknown field '%s' on variable '%s' at %s:%d:%d\n",
                        fname, sym->name, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
                vortex_error("");
            }

            if (g_compile_mode == VORTEX_MODE_SAFE && c->unsafe_depth == 0) {
                ir_emit_ptr_check(c->ir, cur_v, g_current_filename ? g_current_filename : "<input>", line);
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

    fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected primary expression, got '%s' at %s:%d:%d\n",
            l->cur.text, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
    vortex_error("");
    return TYPE_I64;
}

static VortexType parse_cast(Compiler* c, VReg* out_vreg) {
    VortexType t = parse_primary(c, out_vreg);

    while (c->lex->cur.type == TOK_AS) {
        int line = c->lex->cur.line;
        int col = c->lex->cur.col;
        next_token(c->lex);
        int dst_sid = -1;
        VortexType dst_t = parse_type_name(c->lex, &dst_sid);
        if (dst_t == TYPE_UNKNOWN) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected target type after 'as' at %s:%d:%d\n",
                    g_current_filename ? g_current_filename : "<input>", line, col);
            vortex_error("");
        }

        if ((is_pointer_type(t) || is_pointer_type(dst_t)) && t != dst_t) {
            require_unsafe(c, "Pointer/integer cast ('as')", line, col);
        }

        if ((t == TYPE_I64 || t == TYPE_I32) && (dst_t == TYPE_F32 || dst_t == TYPE_F64)) {
            VReg dst_v = ir_new_vreg(c->ir, TYPE_F32);
            ir_emit_cast(c->ir, IR_CAST_I64_TO_F32, dst_v, *out_vreg, TYPE_F32);
            *out_vreg = dst_v;
            t = TYPE_F32;
        } else if ((t == TYPE_F32 || t == TYPE_F64) && (dst_t == TYPE_I64 || dst_t == TYPE_I32)) {
            VReg dst_v = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_cast(c->ir, IR_CAST_F32_TO_I64, dst_v, *out_vreg, TYPE_I64);
            *out_vreg = dst_v;
            t = dst_t;
        } else {
            VReg dst_v = ir_new_vreg(c->ir, dst_t);
            ir_emit_mov(c->ir, dst_v, *out_vreg);
            /* Casted raw pointers are untracked */
            if (is_pointer_type(dst_t)) {
                c->ir->vregs[dst_v].is_slice_ptr = false;
                c->ir->vregs[dst_v].known_len = -1;
            }
            *out_vreg = dst_v;
            t = dst_t;
            if (dst_sid >= 0) c->last_struct_id = dst_sid;
        }
    }
    return t;
}

static VortexType parse_multiplicative(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_cast(c, out_vreg);

    while (c->lex->cur.type == TOK_STAR || c->lex->cur.type == TOK_SLASH || c->lex->cur.type == TOK_PERCENT) {
        int line = c->lex->cur.line;
        int col = c->lex->cur.col;
        VortexTokenType op = c->lex->cur.type;
        next_token(c->lex);
        VReg r_vreg;
        VortexType right_type = parse_cast(c, &r_vreg);

        if (is_pointer_type(left_type) || is_pointer_type(right_type)) {
            require_unsafe(c, "Pointer arithmetic", line, col);
        }

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
        int line = c->lex->cur.line;
        int col = c->lex->cur.col;
        VortexTokenType op = c->lex->cur.type;
        next_token(c->lex);
        VReg r_vreg;
        VortexType right_type = parse_multiplicative(c, &r_vreg);

        if (is_pointer_type(left_type) || is_pointer_type(right_type)) {
            require_unsafe(c, "Raw pointer arithmetic", line, col);
            VortexType ptr_t = is_pointer_type(left_type) ? left_type : right_type;
            VReg dest = ir_new_vreg(c->ir, ptr_t);
            if (op == TOK_PLUS) ir_emit_binop(c->ir, IR_ADD, dest, *out_vreg, r_vreg, TYPE_I64);
            else ir_emit_binop(c->ir, IR_SUB, dest, *out_vreg, r_vreg, TYPE_I64);
            *out_vreg = dest;
            left_type = ptr_t;
            continue;
        }

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
    if (!uf) {
        if (g_user_func_count >= MAX_USER_FUNCS) {
            fprintf(stderr, "[VORTEX ERROR] Maximum number of user functions reached\n");
            vortex_error("");
        }
        uf = &g_user_funcs[g_user_func_count++];
        memset(uf, 0, sizeof(*uf));
    } else {
        if (uf->inline_instrs) free(uf->inline_instrs);
        if (uf->inline_vregs) free(uf->inline_vregs);
        memset(uf, 0, sizeof(*uf));
    }
    strncpy(uf->name, fn_name, sizeof(uf->name) - 1);
    uf->is_unsafe = is_unsafe;
    uf->ret_type = TYPE_I64;
    uf->ret_struct_id = -1;

    match(c, TOK_LPAREN);
    while (l->cur.type != TOK_RPAREN && l->cur.type != TOK_EOF) {
        if (uf->param_count >= MAX_CALL_ARGS) {
            fprintf(stderr, "[VORTEX ERROR] Function '%s' exceeds %d parameters\n", fn_name, MAX_CALL_ARGS);
            vortex_error("");
        }
        strncpy(uf->param_names[uf->param_count], l->cur.text, 63);
        match(c, TOK_IDENT);
        VortexType pt = TYPE_I64;
        int psid = -1;
        if (l->cur.type == TOK_COLON) {
            next_token(l);
            pt = parse_type_name(l, &psid);
            if (pt == TYPE_UNKNOWN) pt = TYPE_I64;
        }
        uf->param_types[uf->param_count] = pt;
        uf->param_struct_ids[uf->param_count] = psid;
        uf->param_count++;
        if (l->cur.type == TOK_COMMA) next_token(l);
    }
    match(c, TOK_RPAREN);

    if (l->cur.type == TOK_ARROW) {
        next_token(l);
        int rsid = -1;
        VortexType rt = parse_type_name(l, &rsid);
        if (rt != TYPE_UNKNOWN) {
            uf->ret_type = rt;
            uf->ret_struct_id = rsid;
        }
    }

    IRBuffer fn_ir;
    ir_init(&fn_ir);

    SymbolTable fn_st;
    memset(&fn_st, 0, sizeof(fn_st));

    Compiler fn_comp;
    memset(&fn_comp, 0, sizeof(fn_comp));
    fn_comp.lex = c->lex;
    fn_comp.st = &fn_st;
    fn_comp.j = c->j;
    fn_comp.ir = &fn_ir;
    fn_comp.last_struct_id = -1;
    fn_comp.unsafe_depth = is_unsafe ? 1 : 0;
    fn_comp.current_fn_is_unsafe = is_unsafe;
    fn_comp.in_function = true;

    for (int p = 0; p < uf->param_count; p++) {
        VortexType pt = uf->param_types[p];
        Symbol* ps = symtab_add(&fn_st, uf->param_names[p], pt, true);
        ps->struct_id = uf->param_struct_ids[p];
        ps->vreg = ir_new_vreg(&fn_ir, pt);
        if (pt == TYPE_PTR_I64 || pt == TYPE_PTR_I32 || pt == TYPE_PTR_F32 || pt == TYPE_PTR_STRUCT) {
            ps->is_slice_ptr = true;
            fn_ir.vregs[ps->vreg].is_slice_ptr = true;
        }
        ir_emit_load_param(&fn_ir, ps->vreg, p);
    }

    match(&fn_comp, TOK_LBRACE);
    while (fn_comp.lex->cur.type != TOK_RBRACE && fn_comp.lex->cur.type != TOK_EOF) {
        parse_statement(&fn_comp);
    }
    match(&fn_comp, TOK_RBRACE);

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

    ir_region_verify(&fn_ir);
    if (g_lsp_capture_errors) {
        uf->code_offset = 0;
        ir_free(&fn_ir);
        return;
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
    ir_free(&fn_ir);
}

/* ========================================================================= */
/* MODULES (import), #[cfg(...)] & STANDALONE BUNDLER                      */
/* ========================================================================= */

VortexImportHookFn g_pkg_import_hook = NULL;

#define MAX_IMPORTED_PATHS 256
static char g_imported_paths[MAX_IMPORTED_PATHS][512];
static int g_imported_path_count = 0;

void lexer_reset_imports(void) {
    g_imported_path_count = 0;
}

static void normalize_mod_path(const char* in, char* out, size_t cap) {
    size_t i = 0, o = 0;
    while (in && in[i] && o + 1 < cap) {
        char c = in[i++];
        if (c == '\\') c = '/';
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
        if (c == '/' && o > 0 && out[o - 1] == '/') continue;
        if (c == '.' && i < strlen(in) && (in[i] == '/' || in[i] == '\\') && (o == 0 || out[o - 1] == '/')) {
            i++;
            continue;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

static bool has_imported_path(const char* path) {
    char norm[512];
    normalize_mod_path(path, norm, sizeof(norm));
    for (int i = 0; i < g_imported_path_count; i++) {
        if (strcmp(g_imported_paths[i], norm) == 0) return true;
    }
    return false;
}

static void mark_imported_path(const char* path) {
    char norm[512];
    normalize_mod_path(path, norm, sizeof(norm));
    for (int i = 0; i < g_imported_path_count; i++) {
        if (strcmp(g_imported_paths[i], norm) == 0) return;
    }
    if (g_imported_path_count < MAX_IMPORTED_PATHS) {
        strncpy(g_imported_paths[g_imported_path_count++], norm, 511);
        g_imported_paths[g_imported_path_count - 1][511] = '\0';
    }
}

static bool is_readable_regular_file(const char* path) {
    if (!path || !path[0]) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    /* Reject directories on POSIX where fopen("dir", "rb") can succeed */
    int ch = fgetc(f);
    (void)ch;
    bool err = (ferror(f) != 0);
    fclose(f);
    return !err;
}

static bool try_probe_vx_file(const char* base, char* out, size_t cap) {
    if (!base || !base[0]) return false;
    if (is_readable_regular_file(base)) {
        strncpy(out, base, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }
    size_t blen = strlen(base);
    if (blen < 3 || strcmp(base + blen - 3, ".vx") != 0) {
        char cand[512];
        snprintf(cand, sizeof(cand), "%s.vx", base);
        if (is_readable_regular_file(cand)) {
            strncpy(out, cand, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
        snprintf(cand, sizeof(cand), "%s/src/lib.vx", base);
        if (is_readable_regular_file(cand)) {
            strncpy(out, cand, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
        snprintf(cand, sizeof(cand), "%s/lib.vx", base);
        if (is_readable_regular_file(cand)) {
            strncpy(out, cand, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
    }
    return false;
}

static void extract_dir_part(const char* file_path, char* out_dir, size_t cap) {
    if (!file_path || !file_path[0] || file_path[0] == '<') {
        out_dir[0] = '\0';
        return;
    }
    strncpy(out_dir, file_path, cap - 1);
    out_dir[cap - 1] = '\0';
    char* s1 = strrchr(out_dir, '/');
    char* s2 = strrchr(out_dir, '\\');
    char* sep = (s1 > s2) ? s1 : s2;
    if (sep) *sep = '\0';
    else out_dir[0] = '\0';
}

static bool resolve_import_path(const char* spec, const char* importer_file, char* out, size_t cap) {
    if (!spec || !spec[0]) return false;
    char imp_dir[512] = {0};
    extract_dir_part(importer_file, imp_dir, sizeof(imp_dir));

    /* 1. Built-in stdlib prefix: "std/..." */
    if (strncmp(spec, "std/", 4) == 0 || strncmp(spec, "std\\", 4) == 0) {
        const char* sub = spec + 4;
        char cand[512];
        const char* env_std = getenv("VORTEX_STD");
        if (env_std && env_std[0]) {
            snprintf(cand, sizeof(cand), "%s/%s", env_std, sub);
            if (try_probe_vx_file(cand, out, cap)) return true;
        }
        char self_path[512] = {0};
        char self_dir[512] = {0};
        vortex_get_self_path(self_path, sizeof(self_path), NULL);
        extract_dir_part(self_path, self_dir, sizeof(self_dir));
        if (self_dir[0]) {
            snprintf(cand, sizeof(cand), "%s/std/%s", self_dir, sub);
            if (try_probe_vx_file(cand, out, cap)) return true;
            snprintf(cand, sizeof(cand), "%s/../std/%s", self_dir, sub);
            if (try_probe_vx_file(cand, out, cap)) return true;
        }
        if (imp_dir[0]) {
            snprintf(cand, sizeof(cand), "%s/std/%s", imp_dir, sub);
            if (try_probe_vx_file(cand, out, cap)) return true;
            snprintf(cand, sizeof(cand), "%s/../std/%s", imp_dir, sub);
            if (try_probe_vx_file(cand, out, cap)) return true;
        }
        snprintf(cand, sizeof(cand), "std/%s", sub);
        if (try_probe_vx_file(cand, out, cap)) return true;
    }

    /* 2. Relative to importer file */
    if (imp_dir[0]) {
        char cand[512];
        snprintf(cand, sizeof(cand), "%s/%s", imp_dir, spec);
        if (try_probe_vx_file(cand, out, cap)) return true;
    }

    /* 3. Relative to current working directory / direct path */
    if (try_probe_vx_file(spec, out, cap)) return true;

    /* 4. Package manager dependency lookup (vortex.toml / vortex.lock / cache) */
    if (g_pkg_import_hook && g_pkg_import_hook(spec, importer_file, out, cap)) {
        return true;
    }
    return false;
}

bool vortex_resolve_import(const char* spec, const char* importer_file, char* out_path, size_t cap) {
    return resolve_import_path(spec, importer_file, out_path, cap);
}

static char* read_file_alloc_str(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

static void parse_import_spec(Lexer* l, char* spec, size_t cap) {
    spec[0] = '\0';
    if (l->cur.type == TOK_STRING) {
        strncpy(spec, l->cur.text, cap - 1);
        spec[cap - 1] = '\0';
        next_token(l);
    } else if (l->cur.type == TOK_IDENT) {
        strncpy(spec, l->cur.text, cap - 1);
        spec[cap - 1] = '\0';
        next_token(l);
        while (l->cur.type == TOK_SLASH || l->cur.type == TOK_DOT || l->cur.type == TOK_MINUS) {
            strncat(spec, l->cur.text, cap - strlen(spec) - 1);
            next_token(l);
            if (l->cur.type == TOK_IDENT || l->cur.type == TOK_INT) {
                strncat(spec, l->cur.text, cap - strlen(spec) - 1);
                next_token(l);
            }
        }
    } else {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected module path string or identifier after 'import' at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    if (l->cur.type == TOK_SEMI) next_token(l);
}

static bool eval_cfg_predicate(const char* pred) {
    const VortexTargetDesc* td = vortex_get_target_desc();
    if (!pred || !td) return false;
    if (strcmp(pred, "windows") == 0) return td->abi == VORTEX_ABI_WIN64;
    if (strcmp(pred, "linux") == 0) return strstr(td->name, "linux") != NULL;
    if (strcmp(pred, "macos") == 0 || strcmp(pred, "darwin") == 0) return td->abi == VORTEX_ABI_DARWIN64;
    if (strcmp(pred, "unix") == 0) return td->abi != VORTEX_ABI_WIN64;
    if (strcmp(pred, "x86_64") == 0 || strcmp(pred, "x64") == 0) return td->arch == VORTEX_ARCH_X86_64;
    if (strcmp(pred, "arm64") == 0 || strcmp(pred, "aarch64") == 0) return td->arch == VORTEX_ARCH_ARM64;
    return false;
}

static void skip_cfg_item(Compiler* c) {
    Lexer* l = c->lex;
    while (l->cur.type == TOK_HASH) {
        next_token(l);
        if (l->cur.type == TOK_LBRACKET) {
            int adepth = 1;
            next_token(l);
            while (adepth > 0 && l->cur.type != TOK_EOF) {
                if (l->cur.type == TOK_LBRACKET) adepth++;
                else if (l->cur.type == TOK_RBRACKET) adepth--;
                next_token(l);
            }
        }
    }
    if (l->cur.type == TOK_IMPORT) {
        next_token(l);
        char dummy[256];
        if (l->cur.type == TOK_STRING || l->cur.type == TOK_IDENT) {
            parse_import_spec(l, dummy, sizeof(dummy));
        }
        return;
    }
    int brace_depth = 0;
    bool saw_brace = false;
    while (l->cur.type != TOK_EOF) {
        if (l->cur.type == TOK_LBRACE) {
            brace_depth++;
            saw_brace = true;
        } else if (l->cur.type == TOK_RBRACE) {
            if (brace_depth > 0) brace_depth--;
            next_token(l);
            if (brace_depth == 0 && saw_brace) return;
            continue;
        } else if (l->cur.type == TOK_SEMI && brace_depth == 0) {
            next_token(l);
            return;
        }
        next_token(l);
    }
}

static void append_str_buf(char** dst, size_t* len, size_t* cap, const char* s, size_t slen) {
    if (*len + slen + 1 > *cap) {
        size_t ncap = (*cap == 0) ? 4096 : (*cap * 2);
        while (ncap < *len + slen + 1) ncap *= 2;
        char* nb = (char*)realloc(*dst, ncap);
        if (!nb) return;
        *dst = nb;
        *cap = ncap;
    }
    if (slen > 0) memcpy(*dst + *len, s, slen);
    *len += slen;
    (*dst)[*len] = '\0';
}

static void bundle_recursive(const char* file_path, const char* src,
                             char** out_buf, size_t* out_len, size_t* out_cap) {
    if (!src) return;
    if (file_path && file_path[0]) {
        if (has_imported_path(file_path)) return;
        mark_imported_path(file_path);
    }
    size_t i = 0;
    size_t n = strlen(src);
    int brace_depth = 0;
    while (i < n) {
        /* Preserve whitespace and comments */
        if (src[i] == '"') {
            size_t st = i++;
            while (i < n && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < n) i += 2;
                else i++;
            }
            if (i < n && src[i] == '"') i++;
            append_str_buf(out_buf, out_len, out_cap, src + st, i - st);
            continue;
        }
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
            size_t st = i;
            while (i < n && src[i] != '\n') i++;
            append_str_buf(out_buf, out_len, out_cap, src + st, i - st);
            continue;
        }
        if (src[i] == '{') brace_depth++;
        else if (src[i] == '}' && brace_depth > 0) brace_depth--;

        /* Top-level import? */
        if (brace_depth == 0 && i + 6 <= n && strncmp(src + i, "import", 6) == 0 &&
            (i == 0 || (!isalnum((unsigned char)src[i - 1]) && src[i - 1] != '_')) &&
            (i + 6 == n || isspace((unsigned char)src[i + 6]) || src[i + 6] == '"')) {
            size_t j = i + 6;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            char spec[256] = {0};
            if (j < n && src[j] == '"') {
                j++;
                size_t k = 0;
                while (j < n && src[j] != '"' && src[j] != '\n' && k + 1 < sizeof(spec)) {
                    spec[k++] = src[j++];
                }
                if (j < n && src[j] == '"') j++;
            } else {
                size_t k = 0;
                while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_' || src[j] == '/' || src[j] == '.' || src[j] == '-') && k + 1 < sizeof(spec)) {
                    spec[k++] = src[j++];
                }
            }
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == ';') j++;

            char resolved[512] = {0};
            if (spec[0] && resolve_import_path(spec, file_path, resolved, sizeof(resolved))) {
                size_t mlen = 0;
                char* msrc = read_file_alloc_str(resolved, &mlen);
                if (msrc) {
                    bundle_recursive(resolved, msrc, out_buf, out_len, out_cap);
                    free(msrc);
                    append_str_buf(out_buf, out_len, out_cap, "\n", 1);
                    i = j;
                    continue;
                }
            }
        }
        append_str_buf(out_buf, out_len, out_cap, &src[i], 1);
        i++;
    }
}

char* vortex_bundle_source(const char* entry_path, const char* entry_src, size_t* out_len) {
    lexer_reset_imports();
    char* buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    bundle_recursive(entry_path, entry_src, &buf, &len, &cap);
    lexer_reset_imports();
    if (out_len) *out_len = len;
    return buf;
}

void parse_statement(Compiler* c) {
    Lexer* l = c->lex;
    int stmt_line = l->cur.line;
    int stmt_col = l->cur.col;
    int ir_start = c->ir->count;

    /* Conditional compilation attribute: #[cfg(windows|linux|macos|unix|arm64|x86_64)] */
    if (l->cur.type == TOK_HASH) {
        next_token(l);
        match(c, TOK_LBRACKET);
        if (l->cur.type != TOK_IDENT || strcmp(l->cur.text, "cfg") != 0) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected 'cfg' attribute inside '#[...]' at %s:%d:%d\n",
                    g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        next_token(l);
        match(c, TOK_LPAREN);
        if (l->cur.type != TOK_IDENT) {
            fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected platform identifier in #[cfg(...)] at %s:%d:%d\n",
                    g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        char pred[64];
        strncpy(pred, l->cur.text, sizeof(pred) - 1);
        pred[sizeof(pred) - 1] = '\0';
        next_token(l);
        match(c, TOK_RPAREN);
        match(c, TOK_RBRACKET);
        if (!eval_cfg_predicate(pred)) {
            skip_cfg_item(c);
            return;
        }
        parse_statement(c);
        return;
    }

    /* Modular import: import "std/json"; or import std/json; */
    if (l->cur.type == TOK_IMPORT) {
        int imp_line = l->cur.line;
        int imp_col = l->cur.col;
        next_token(l);
        if (c->in_function || c->unsafe_depth > 0) {
            fprintf(stderr, "[VORTEX ERROR] 'import' is only allowed at top level at %s:%d:%d\n",
                    g_current_filename ? g_current_filename : "<input>", imp_line, imp_col);
            vortex_error("");
        }
        char spec[256];
        parse_import_spec(l, spec, sizeof(spec));
        char resolved[512];
        if (!resolve_import_path(spec, g_current_filename, resolved, sizeof(resolved))) {
            fprintf(stderr, "[VORTEX ERROR] Cannot resolve module import '%s' at %s:%d:%d\n",
                    spec, g_current_filename ? g_current_filename : "<input>", imp_line, imp_col);
            vortex_error("");
        }
        if (has_imported_path(resolved)) {
            return;
        }
        mark_imported_path(resolved);
        size_t mlen = 0;
        char* msrc = read_file_alloc_str(resolved, &mlen);
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
        return;
    }

    /* Explicit unsafe block or unsafe function: unsafe { ... } / unsafe fn ... */
    if (l->cur.type == TOK_UNSAFE) {
        next_token(l);
        if (l->cur.type == TOK_FN) {
            parse_function_def(c, true);
            return;
        }
        match(c, TOK_LBRACE);
        c->unsafe_depth++;
        while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
            parse_statement(c);
        }
        match(c, TOK_RBRACE);
        c->unsafe_depth--;
        stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* Extern C Functions: extern "lib" fn name(p1: type, ...) [-> ret_type]; */
    if (l->cur.type == TOK_EXTERN) {
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

        match(c, TOK_LPAREN);
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
            match(c, TOK_COLON);
            int st_id = -1;
            bool nullable = false;
            VortexType pt = parse_type_spec_full(l, &st_id, &nullable);
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
        match(c, TOK_RPAREN);

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
        match(c, TOK_SEMI);

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
        return;
    }

    /* User-Defined Functions: fn name(p1, p2, ...) { body } */
    if (l->cur.type == TOK_FN) {
        parse_function_def(c, false);
        return;
    }

    /* Struct Definition: struct Name { f1: type, ... } */
    if (l->cur.type == TOK_STRUCT) {
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
                fprintf(stderr, "[VORTEX ERROR] Expected field name in struct %s at %s:%d:%d\n",
                        sname, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
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
                if (strcmp(l->cur.text, "i32") == 0) {
                    ftype = TYPE_I32; fsize = 4; falign = 4;
                } else if (strcmp(l->cur.text, "ptr") == 0) {
                    ftype = TYPE_PTR; fsize = 8; falign = 8;
                } else if (strcmp(l->cur.text, "str") == 0) {
                    ftype = TYPE_STR; fsize = 8; falign = 8;
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
                else if (l->cur.type == TOK_IDENT && strcmp(l->cur.text, "i32") == 0) ftype = TYPE_PTR_I32;
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

    /* Raw pointer dereference write: *ptr = val; */
    if (l->cur.type == TOK_STAR) {
        next_token(l);
        require_unsafe(c, "Raw pointer dereference write '*ptr = ...'", stmt_line, stmt_col);
        VReg ptr_v;
        VortexType pt = parse_primary(c, &ptr_v);
        match(c, TOK_EQ);
        VReg v_val;
        VortexType vt = parse_expr(c, &v_val);
        match(c, TOK_SEMI);
        VortexType elem_t = (pt == TYPE_PTR_I32) ? TYPE_I32 :
                            (pt == TYPE_PTR_F32) ? TYPE_F32 :
                            (vt == TYPE_F32) ? TYPE_F32 : TYPE_I64;
        ir_emit_store_deref(c->ir, ptr_v, v_val, elem_t);
        stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* Return statement */
    if (l->cur.type == TOK_RETURN) {
        next_token(l);
        VReg v_val;
        VortexType val_t = parse_expr(c, &v_val);
        match(c, TOK_SEMI);
        if (v_val >= 0 && v_val < c->ir->vreg_count && c->ir->vregs[v_val].is_ref) {
            int rid = c->ir->vregs[v_val].region_id;
            int rdepth = c->ir->vregs[v_val].region_depth;
            if (region_is_popped(c, rid) || rdepth > 0) {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Cannot return reference allocated inside internal/popped 'scope_push()' region at %s:%d:%d\n",
                        g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                vortex_error("");
            }
        }
        ir_emit_ret(c->ir, v_val, val_t);
        c->block_returned = true;
        stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* Variable Declaration: let [mut] name [: type] = expr; */
    if (l->cur.type == TOK_LET) {
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

        match(c, TOK_EQ);
        c->last_struct_id = -1;
        VReg v_expr;
        VortexType expr_type = parse_expr(c, &v_expr);
        match(c, TOK_SEMI);

        if (explicit_type != TYPE_UNKNOWN && is_pointer_type(explicit_type) && is_scalar_int_type(expr_type)) {
            require_unsafe(c, "Integer-to-pointer assignment", stmt_line, stmt_col);
        }

        VortexType final_type = (explicit_type != TYPE_UNKNOWN) ? explicit_type : expr_type;
        Symbol* sym = symtab_add(c->st, name, final_type, is_mut);
        sym->struct_id = (explicit_struct_id >= 0) ? explicit_struct_id : c->last_struct_id;
        sym->vreg = ir_new_vreg(c->ir, final_type);

        if (v_expr >= 0 && v_expr < c->ir->vreg_count) {
            VRegInfo* src_vi = &c->ir->vregs[v_expr];
            if (src_vi->is_ref) {
                sym->is_ref = true;
                sym->region_id = src_vi->region_id;
                sym->region_depth = src_vi->region_depth;
                set_vreg_region(c->ir, sym->vreg, true, src_vi->region_id, src_vi->region_depth);
            }
            if (src_vi->region_id > 0 && !src_vi->is_ref &&
                c->current_region_depth > 0 &&
                src_vi->region_id == c->current_region_id) {
                /* Mark returned from scope_push() */
                sym->is_region_mark = true;
                sym->region_id = src_vi->region_id;
                sym->region_depth = src_vi->region_depth;
                strncpy(c->region_stack[c->current_region_depth].mark_name, name, 63);
            }
            if (src_vi->is_slice_ptr) {
                sym->is_slice_ptr = true;
                sym->len_vreg = src_vi->len_vreg;
                sym->known_len = src_vi->known_len;
                /* If typed slice from alloc(bytes), scale constant byte length by element size */
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
        stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* Parallel For Loop: parallel_for i in start..end { ... } */
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
        SymbolTable worker_st;
        memset(&worker_st, 0, sizeof(worker_st));

        Compiler worker_c;
        memset(&worker_c, 0, sizeof(worker_c));
        worker_c.lex = c->lex;
        worker_c.st = &worker_st;
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
            Symbol* ws = symtab_add(&worker_st, parent_sym->name, parent_sym->type, parent_sym->is_mut);
            ws->struct_id = parent_sym->struct_id;
            ws->is_ref = parent_sym->is_ref;
            ws->is_slice_ptr = parent_sym->is_slice_ptr;
            ws->known_len = parent_sym->known_len;
            ws->region_depth = 0;
            ws->vreg = ir_new_vreg(&worker_ir, parent_sym->type);
            ir_emit_load_offset(&worker_ir, ws->vreg, v_w_ctx, s * 8, parent_sym->type);
        }

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
        ir_free(&worker_ir);

        VReg v_fn = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, v_fn, (int64_t)worker_fn_addr);

        VReg v_ret = ir_new_vreg(c->ir, TYPE_VOID);
        VReg dispatch_args[4] = { v_fn, v_ctx, v_start, v_end };
        ir_emit_call(c->ir, v_ret, (void*)vortex_parallel_for_dispatch, dispatch_args, 4, TYPE_VOID);
        stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* For Loop: for i in start..end { ... } */
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
            stamp_ir_loc(c->ir, ir_start, stmt_line);
            return;
        }

        int lbl_start = ir_new_label(c->ir);
        int lbl_exit = ir_new_label(c->ir);

        match(c, TOK_LBRACE);
        ir_emit_label(c->ir, lbl_start);

        VReg v_cmp = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_binop(c->ir, IR_LT, v_cmp, i_sym->vreg, lim_sym->vreg, TYPE_I64);
        ir_emit_jz(c->ir, v_cmp, lbl_exit);

        int saved_loop_st_count = c->st->count;
        while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
            parse_statement(c);
        }
        match(c, TOK_RBRACE);
        c->st->count = saved_loop_st_count;

        VReg v_one = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, v_one, 1);
        ir_emit_binop(c->ir, IR_ADD, i_sym->vreg, i_sym->vreg, v_one, TYPE_I64);

        ir_emit_jmp(c->ir, lbl_start);
        ir_emit_label(c->ir, lbl_exit);
        stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* While Loop: while cond { ... } */
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
        stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* If / Else-If / Else Chaining */
    if (l->cur.type == TOK_IF) {
        next_token(l);
        VReg v_cond;
        parse_expr(c, &v_cond);
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

        match(c, TOK_LBRACE);
        while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
            parse_statement(c);
        }
        match(c, TOK_RBRACE);

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
                match(c, TOK_LBRACE);
                while (c->lex->cur.type != TOK_RBRACE && c->lex->cur.type != TOK_EOF) {
                    parse_statement(c);
                }
                match(c, TOK_RBRACE);
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
        stamp_ir_loc(c->ir, ir_start, stmt_line);
        return;
    }

    /* Ident-led Statements: assignment, field write, array write, or expr */
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
                next_token(l);
                match(c, TOK_EQ);
                VReg v_val;
                parse_expr(c, &v_val);
                match(c, TOK_SEMI);

                Symbol* sym = symtab_find(c->st, name);
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
                stamp_ir_loc(c->ir, ir_start, stmt_line);
                return;
            }

            if (next_t == TOK_DOT) {
                next_token(l);
                match(c, TOK_DOT);
                if (l->cur.type != TOK_IDENT) {
                    fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %s:%d:%d\n",
                            g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
                    vortex_error("");
                }
                char fname[64];
                strncpy(fname, l->cur.text, sizeof(fname) - 1);
                fname[sizeof(fname) - 1] = '\0';
                next_token(l);

                if (l->cur.type == TOK_EQ) {
                    match(c, TOK_EQ);
                    VReg v_val;
                    VortexType val_t = parse_expr(c, &v_val);
                    match(c, TOK_SEMI);

                    Symbol* sym = symtab_find(c->st, name);
                    if (!sym) {
                        fprintf(stderr, "[VORTEX ERROR] Variable '%s' not found for field assignment at %s:%d:%d\n",
                                name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                        vortex_error("");
                    }
                    check_symbol_alive(c, sym, stmt_line, stmt_col);

                    if (v_val >= 0 && v_val < c->ir->vreg_count && c->ir->vregs[v_val].is_ref &&
                        c->ir->vregs[v_val].region_depth > sym->region_depth) {
                        fprintf(stderr,
                                "[VORTEX REGION ERROR] Cannot store inner arena reference (depth %d) in outer struct '%s' (depth %d) at %s:%d:%d\n",
                                c->ir->vregs[v_val].region_depth, sym->name, sym->region_depth,
                                g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
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
                        fprintf(stderr, "[VORTEX ERROR] Unknown field '%s' on variable '%s' at %s:%d:%d\n",
                                fname, name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                        vortex_error("");
                    }
                    if (g_compile_mode == VORTEX_MODE_SAFE && c->unsafe_depth == 0) {
                        ir_emit_ptr_check(c->ir, sym->vreg, g_current_filename ? g_current_filename : "<input>", stmt_line);
                    }
                    VortexType ft = (val_t == TYPE_F32 || f->type == TYPE_F32) ? TYPE_F32 : f->type;
                    ir_emit_store_offset(c->ir, sym->vreg, f->offset, v_val, ft);
                    stamp_ir_loc(c->ir, ir_start, stmt_line);
                    return;
                }
                *c->lex = saved;
            }

            if (next_t == TOK_LBRACKET) {
                next_token(l);
                match(c, TOK_LBRACKET);
                VReg v_idx;
                parse_expr(c, &v_idx);
                match(c, TOK_RBRACKET);

                Symbol* sym = symtab_find(c->st, name);
                if (!sym) {
                    fprintf(stderr, "[VORTEX ERROR] Variable '%s' not found at %s:%d:%d\n",
                            name, g_current_filename ? g_current_filename : "<input>", stmt_line, stmt_col);
                    vortex_error("");
                }
                check_symbol_alive(c, sym, stmt_line, stmt_col);

                if (l->cur.type == TOK_DOT) {
                    match(c, TOK_DOT);
                    if (l->cur.type != TOK_IDENT) {
                        fprintf(stderr, "[VORTEX ERROR] Expected field name after '.' at %s:%d:%d\n",
                                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
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
                    int f_offset = f ? f->offset : 0;
                    VortexType ft = (val_t == TYPE_F32 || (f && f->type == TYPE_F32)) ? TYPE_F32 : TYPE_I64;

                    if ((c->unsafe_depth == 0 || sym->is_slice_ptr) &&
                        (g_compile_mode == VORTEX_MODE_SAFE || sym->is_slice_ptr)) {
                        if (!sym->is_slice_ptr && c->unsafe_depth == 0) {
                            require_unsafe(c, "Indexing untracked raw pointer", stmt_line, stmt_col);
                        }
                        ir_emit_bounds_check(c->ir, sym->vreg, v_idx, sym->len_vreg, sym->known_len, 8,
                                             g_current_filename ? g_current_filename : "<input>", stmt_line);
                    }
                    VReg v_elem = ir_new_vreg(c->ir, TYPE_PTR_STRUCT);
                    ir_emit_load_array(c->ir, v_elem, sym->vreg, v_idx, TYPE_PTR_STRUCT);
                    ir_emit_store_offset(c->ir, v_elem, f_offset, v_val, ft);
                    stamp_ir_loc(c->ir, ir_start, stmt_line);
                    return;
                }

                if (l->cur.type == TOK_EQ) {
                    match(c, TOK_EQ);
                    VReg v_val;
                    VortexType val_t = parse_expr(c, &v_val);
                    match(c, TOK_SEMI);

                    if (v_val >= 0 && v_val < c->ir->vreg_count && c->ir->vregs[v_val].is_ref &&
                        c->ir->vregs[v_val].region_depth > sym->region_depth) {
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
                        require_unsafe(c, "Indexing untracked raw pointer", stmt_line, stmt_col);
                    }
                    if ((c->unsafe_depth == 0 || sym->is_slice_ptr) &&
                        (g_compile_mode == VORTEX_MODE_SAFE || sym->is_slice_ptr)) {
                        ir_emit_bounds_check(c->ir, sym->vreg, v_idx, sym->len_vreg, sym->known_len, elem_size,
                                             g_current_filename ? g_current_filename : "<input>", stmt_line);
                    }
                    ir_emit_store_array(c->ir, sym->vreg, v_idx, v_val, elem_t);
                    stamp_ir_loc(c->ir, ir_start, stmt_line);
                    return;
                }
                *c->lex = saved;
            }
        }
    }

    /* General Expression Statement */
    VReg dummy;
    parse_expr(c, &dummy);
    match(c, TOK_SEMI);
    stamp_ir_loc(c->ir, ir_start, stmt_line);
}
