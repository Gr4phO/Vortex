/*
 * VORTEX PROGRAMMING LANGUAGE - TYPE/REGION HELPERS & EXPRESSION PARSER (src/parser_expr.c)
 */

#include "parser_internal.h"

/* --- Structs, Types & Symbol Tables --- */
VortexStructType g_struct_types[MAX_STRUCT_TYPES];
int g_struct_type_count = 0;

#define ST_HASH_CAP 4096
#define UF_HASH_CAP 32768
static uint32_t g_st_hash_epoch_arr[ST_HASH_CAP];
static int      g_st_hash_idx[ST_HASH_CAP];
static uint32_t g_st_epoch = 1;

static uint32_t g_uf_hash_epoch_arr[UF_HASH_CAP];
static int      g_uf_hash_idx[UF_HASH_CAP];
static uint32_t g_uf_epoch = 1;

static uint32_t vortex_fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

void vortex_parser_reset_tables(bool reset_structs) {
    g_uf_epoch++;
    if (g_uf_epoch == 0) {
        memset(g_uf_hash_epoch_arr, 0, sizeof(g_uf_hash_epoch_arr));
        g_uf_epoch = 1;
    }
    if (reset_structs) {
        g_st_epoch++;
        if (g_st_epoch == 0) {
            memset(g_st_hash_epoch_arr, 0, sizeof(g_st_hash_epoch_arr));
            g_st_epoch = 1;
        }
    }
}

void struct_type_hash_insert(const char* name, int idx) {
    if (!name) return;
    uint32_t mask = ST_HASH_CAP - 1;
    uint32_t slot = vortex_fnv1a(name) & mask;
    for (uint32_t probe = 0; probe < ST_HASH_CAP; probe++) {
        uint32_t pos = (slot + probe) & mask;
        if (g_st_hash_epoch_arr[pos] != g_st_epoch || g_st_hash_idx[pos] == idx) {
            g_st_hash_epoch_arr[pos] = g_st_epoch;
            g_st_hash_idx[pos] = idx;
            return;
        }
    }
}

VortexStructType* struct_type_find(const char* name) {
    if (!name || g_struct_type_count == 0) return NULL;
    uint32_t mask = ST_HASH_CAP - 1;
    uint32_t slot = vortex_fnv1a(name) & mask;
    for (uint32_t probe = 0; probe < ST_HASH_CAP; probe++) {
        uint32_t pos = (slot + probe) & mask;
        if (g_st_hash_epoch_arr[pos] != g_st_epoch) return NULL;
        int idx = g_st_hash_idx[pos];
        if (idx >= 0 && idx < g_struct_type_count && strcmp(g_struct_types[idx].name, name) == 0) {
            return &g_struct_types[idx];
        }
    }
    return NULL;
}

VortexStructType* struct_type_find_by_id(int id) {
    if (id >= 0 && id < g_struct_type_count) return &g_struct_types[id];
    return NULL;
}

VortexEnumType* enum_type_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_enum_type_count; i++) {
        if (strcmp(g_enum_types[i].name, name) == 0) {
            return &g_enum_types[i];
        }
    }
    return NULL;
}

VortexField* struct_field_find(VortexStructType* st, const char* field_name) {
    if (!st || !field_name) return NULL;
    for (int i = 0; i < st->field_count; i++) {
        if (strcmp(st->fields[i].name, field_name) == 0) return &st->fields[i];
    }
    return NULL;
}

VortexField* vortex_resolve_struct_field(int struct_id, const char* fname) {
    VortexStructType* st_lookup = (struct_id >= 0) ? struct_type_find_by_id(struct_id) : NULL;
    if (!st_lookup) {
        for (int s = 0; s < g_struct_type_count; s++) {
            if (struct_field_find(&g_struct_types[s], fname)) {
                st_lookup = &g_struct_types[s];
                break;
            }
        }
    }
    return struct_field_find(st_lookup, fname);
}

VortexType vortex_parse_type_spec_full(Lexer* l, int* out_struct_id, bool* out_nullable) {
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
    return vortex_parse_type_spec_full(l, out_struct_id, NULL);
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

Symbol* vortex_symtab_find(SymbolTable* st, const char* name) {
    for (int i = st->count - 1; i >= 0; i--) {
        if (strcmp(st->symbols[i].name, name) == 0) return &st->symbols[i];
    }
    return NULL;
}

Symbol* vortex_symtab_add(SymbolTable* st, const char* name, VortexType type, bool is_mut) {
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

void user_func_hash_insert(const char* name, int idx) {
    if (!name) return;
    uint32_t mask = UF_HASH_CAP - 1;
    uint32_t slot = vortex_fnv1a(name) & mask;
    for (uint32_t probe = 0; probe < UF_HASH_CAP; probe++) {
        uint32_t pos = (slot + probe) & mask;
        if (g_uf_hash_epoch_arr[pos] != g_uf_epoch || g_uf_hash_idx[pos] == idx) {
            g_uf_hash_epoch_arr[pos] = g_uf_epoch;
            g_uf_hash_idx[pos] = idx;
            return;
        }
    }
}

UserFunc* user_func_find(const char* name) {
    if (!name || g_user_func_count == 0) return NULL;
    uint32_t mask = UF_HASH_CAP - 1;
    uint32_t slot = vortex_fnv1a(name) & mask;
    for (uint32_t probe = 0; probe < UF_HASH_CAP; probe++) {
        uint32_t pos = (slot + probe) & mask;
        if (g_uf_hash_epoch_arr[pos] != g_uf_epoch) return NULL;
        int idx = g_uf_hash_idx[pos];
        if (idx >= 0 && idx < g_user_func_count && strcmp(g_user_funcs[idx].name, name) == 0) {
            return &g_user_funcs[idx];
        }
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
bool vortex_is_pointer_type(VortexType t) {
    return t == TYPE_PTR || t == TYPE_PTR_I64 || t == TYPE_PTR_I32 ||
           t == TYPE_PTR_F32 || t == TYPE_PTR_STRUCT || t == TYPE_STRUCT || t == TYPE_STR;
}

bool vortex_is_scalar_int_type(VortexType t) {
    return t == TYPE_I64 || t == TYPE_I32;
}

void vortex_require_unsafe(Compiler* c, const char* what, int line, int col) {
    if (c->unsafe_depth <= 0) {
        fprintf(stderr,
                "[VORTEX SAFETY ERROR] %s requires an 'unsafe' block or 'unsafe fn' at %s:%d:%d\n",
                what, g_current_filename ? g_current_filename : "<input>", line, col);
        vortex_error("");
    }
}

bool vortex_region_is_popped(Compiler* c, int rid) {
    if (rid <= 0) return false;
    for (int i = 0; i < c->popped_region_count; i++) {
        if (c->popped_regions[i] == rid) return true;
    }
    return false;
}

void vortex_mark_region_popped(Compiler* c, int rid) {
    if (rid <= 0 || vortex_region_is_popped(c, rid)) return;
    if (c->popped_region_count < 256) {
        c->popped_regions[c->popped_region_count++] = rid;
    }
}

void vortex_check_symbol_alive(Compiler* c, Symbol* sym, int line, int col) {
    if (sym && sym->is_ref && vortex_region_is_popped(c, sym->region_id)) {
        fprintf(stderr,
                "[VORTEX REGION ERROR] Use of reference '%s' after arena 'scope_pop()' at %s:%d:%d\n",
                sym->name, g_current_filename ? g_current_filename : "<input>", line, col);
        vortex_error("");
    }
}

void vortex_stamp_ir_loc(IRBuffer* ir, int from_idx, int line) {
    const char* file = g_current_filename ? g_current_filename : "<input>";
    for (int i = from_idx; i < ir->count; i++) {
        if (ir->instrs[i].line == 0) ir->instrs[i].line = line;
        if (!ir->instrs[i].file) ir->instrs[i].file = file;
    }
}

void vortex_set_vreg_region(IRBuffer* ir, VReg v, bool is_ref, int rid, int depth) {
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

/* --- Recursive-Descent Expression Parser & IR Lowering --- */
static VortexType parse_cast(Compiler* c, VReg* out_vreg);
static VortexType parse_multiplicative(Compiler* c, VReg* out_vreg);
static VortexType parse_additive(Compiler* c, VReg* out_vreg);

void vortex_parser_match(Compiler* c, VortexTokenType expected) {
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

static void check_container_escape(Compiler* c, VReg val_v, VReg cont_v, bool is_vec_push, int line, int col) {
    if (val_v >= 0 && val_v < c->ir->vreg_count &&
        c->ir->vregs[val_v].is_ref &&
        cont_v >= 0 && cont_v < c->ir->vreg_count) {
        int val_depth = c->ir->vregs[val_v].region_depth;
        int cont_depth = c->ir->vregs[cont_v].region_depth;
        if (val_depth > cont_depth) {
            if (is_vec_push) {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Cannot push inner arena reference (depth %d) into outer vector (depth %d) at %s:%d:%d\n",
                        val_depth, cont_depth, g_current_filename ? g_current_filename : "<input>", line, col);
            } else {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Cannot store inner arena reference (depth %d) into outer container (depth %d) at %s:%d:%d\n",
                        val_depth, cont_depth, g_current_filename ? g_current_filename : "<input>", line, col);
            }
            vortex_error("");
        }
    }
}

VortexType vortex_parse_primary(Compiler* c, VReg* out_vreg) {
    Lexer* l = c->lex;
    int line = l->cur.line;
    int col = l->cur.col;

    if (l->cur.type == TOK_BANG) {
        next_token(l);
        VReg v_inner;
        vortex_parse_primary(c, &v_inner);
        *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_binop(c->ir, IR_LNOT, *out_vreg, v_inner, v_inner, TYPE_I64);
        return TYPE_I64;
    }

    if (l->cur.type == TOK_TILDE) {
        next_token(l);
        VReg v_inner;
        vortex_parse_primary(c, &v_inner);
        *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_binop(c->ir, IR_BNOT, *out_vreg, v_inner, v_inner, TYPE_I64);
        return TYPE_I64;
    }

    if (l->cur.type == TOK_MINUS) {
        next_token(l);
        VReg v_inner;
        VortexType t = vortex_parse_primary(c, &v_inner);
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
        vortex_require_unsafe(c, "Raw pointer dereference '*ptr'", line, col);
        VReg ptr_v;
        VortexType pt = vortex_parse_primary(c, &ptr_v);
        VortexType elem_t = (pt == TYPE_PTR_I32) ? TYPE_I32 :
                            (pt == TYPE_PTR_F32) ? TYPE_F32 : TYPE_I64;
        *out_vreg = ir_new_vreg(c->ir, elem_t);
        ir_emit_load_deref(c->ir, *out_vreg, ptr_v, elem_t);
        return elem_t;
    }

    if (l->cur.type == TOK_UNSAFE) {
        next_token(l);
        vortex_parser_match(c, TOK_LBRACE);
        c->unsafe_depth++;
        VReg last_v = VREG_NONE;
        VortexType last_t = TYPE_I64;
        while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
            if (l->cur.type == TOK_LET || l->cur.type == TOK_FOR ||
                l->cur.type == TOK_PARALLEL_FOR || l->cur.type == TOK_WHILE ||
                l->cur.type == TOK_IF || l->cur.type == TOK_RETURN ||
                l->cur.type == TOK_UNSAFE || l->cur.type == TOK_STAR ||
                l->cur.type == TOK_BREAK || l->cur.type == TOK_CONTINUE ||
                l->cur.type == TOK_ENUM) {
                parse_statement(c);
                last_v = VREG_NONE;
            } else {
                bool is_assign = false;
                if (l->cur.type == TOK_IDENT && l->pos < l->len) {
                    Lexer saved = *l;
                    next_token(l);
                    VortexTokenType nt = l->cur.type;
                    if (nt == TOK_EQ || (nt >= TOK_PLUS_EQ && nt <= TOK_SHR_EQ) ||
                        nt == TOK_DOT || nt == TOK_LBRACKET) {
                        if (nt == TOK_EQ || (nt >= TOK_PLUS_EQ && nt <= TOK_SHR_EQ)) is_assign = true;
                        else {
                            int depth = 0;
                            while (l->cur.type != TOK_EOF && l->cur.type != TOK_SEMI && l->cur.type != TOK_RBRACE) {
                                if (l->cur.type == TOK_LBRACKET || l->cur.type == TOK_LPAREN) depth++;
                                else if (l->cur.type == TOK_RBRACKET || l->cur.type == TOK_RPAREN) depth--;
                                else if ((l->cur.type == TOK_EQ ||
                                          (l->cur.type >= TOK_PLUS_EQ && l->cur.type <= TOK_SHR_EQ)) &&
                                         depth == 0) {
                                    is_assign = true;
                                    break;
                                }
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
                    last_t = vortex_parse_expr(c, &last_v);
                    if (l->cur.type == TOK_SEMI) next_token(l);
                }
            }
        }
        vortex_parser_match(c, TOK_RBRACE);
        c->unsafe_depth--;
        if (last_v == VREG_NONE) {
            last_v = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_const_i64(c->ir, last_v, 0);
            last_t = TYPE_I64;
        }
        *out_vreg = last_v;
        return last_t;
    }

    if (l->cur.type == TOK_TRUE) {
        next_token(l);
        *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, *out_vreg, 1);
        return TYPE_I64;
    }

    if (l->cur.type == TOK_FALSE) {
        next_token(l);
        *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, *out_vreg, 0);
        return TYPE_I64;
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
        VortexType t = vortex_parse_expr(c, out_vreg);
        vortex_parser_match(c, TOK_RPAREN);
        return t;
    }

    if (l->cur.type == TOK_IDENT) {
        char name[64];
        strncpy(name, l->cur.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        next_token(l);

        /* Enum Variant: EnumName::Variant or EnumName.Variant */
        if (l->cur.type == TOK_COLONCOLON || l->cur.type == TOK_DOT) {
            VortexEnumType* en = enum_type_find(name);
            if (en) {
                next_token(l);
                if (l->cur.type != TOK_IDENT) {
                    fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected variant name after '%s' at %s:%d:%d\n",
                            name, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
                    vortex_error("");
                }
                int64_t eval = 0;
                bool found_v = false;
                for (int vi = 0; vi < en->variant_count; vi++) {
                    if (strcmp(en->variants[vi].name, l->cur.text) == 0) {
                        eval = en->variants[vi].value;
                        found_v = true;
                        break;
                    }
                }
                if (!found_v) {
                    fprintf(stderr, "[VORTEX ERROR] Unknown enum variant '%s::%s' at %s:%d:%d\n",
                            name, l->cur.text, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
                    vortex_error("");
                }
                next_token(l);
                *out_vreg = ir_new_vreg(c->ir, TYPE_I64);
                ir_emit_const_i64(c->ir, *out_vreg, eval);
                return TYPE_I64;
            }
        }

        /* Struct Instantiation: StructName { f1: v1, ... } */
        VortexStructType* st_def = struct_type_find(name);
        if (st_def && l->cur.type == TOK_LBRACE) {
            vortex_parser_match(c, TOK_LBRACE);
            VReg v_sz = ir_new_vreg(c->ir, TYPE_I64);
            ir_emit_const_i64(c->ir, v_sz, st_def->size);
            VReg v_struct = ir_new_vreg(c->ir, TYPE_PTR_STRUCT);
            ir_emit_call(c->ir, v_struct, (void*)vortex_alloc_struct, &v_sz, 1, TYPE_PTR_STRUCT);
            vortex_set_vreg_region(c->ir, v_struct, true, c->current_region_id, c->current_region_depth);

            int field_idx = 0;
            while (l->cur.type != TOK_RBRACE && l->cur.type != TOK_EOF) {
                VortexField* f = NULL;
                if (is_field_named_init(l)) {
                    char fname[64];
                    strncpy(fname, l->cur.text, sizeof(fname) - 1);
                    fname[sizeof(fname) - 1] = '\0';
                    next_token(l);
                    vortex_parser_match(c, TOK_COLON);
                    f = struct_field_find(st_def, fname);
                } else {
                    if (field_idx < st_def->field_count) {
                        f = &st_def->fields[field_idx];
                    }
                }
                field_idx++;

                VReg v_val;
                VortexType val_t = vortex_parse_expr(c, &v_val);
                if (f) {
                    VortexType ft = (f->type == TYPE_F32 || val_t == TYPE_F32) ? TYPE_F32 : f->type;
                    ir_emit_store_offset(c->ir, v_struct, f->offset, v_val, ft);
                }
                if (l->cur.type == TOK_COMMA || l->cur.type == TOK_SEMI) next_token(l);
            }
            vortex_parser_match(c, TOK_RBRACE);
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
                    arg_syms[num_args] = vortex_symtab_find(c->st, l->cur.text);
                }
                arg_types[num_args] = vortex_parse_expr(c, &arg_vregs[num_args]);
                num_args++;
                if (l->cur.type == TOK_COMMA) next_token(l);
            }
            vortex_parser_match(c, TOK_RPAREN);

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
                        vortex_mark_region_popped(c, r);
                        c->region_stack[d].active = false;
                        c->current_region_depth = d - 1;
                        if (r == pop_rid) break;
                    }
                    c->current_region_id = (c->current_region_depth > 0)
                                           ? c->region_stack[c->current_region_depth].region_id
                                           : 0;
                } else if (c->current_region_depth > 0) {
                    vortex_mark_region_popped(c, c->region_stack[c->current_region_depth].region_id);
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
                check_container_escape(c, arg_vregs[1], arg_vregs[0], true, line, col);
                if (arg_syms[0] && arg_vregs[1] >= 0 && c->ir->vregs[arg_vregs[1]].is_ref) {
                    arg_syms[0]->holds_refs = true;
                    arg_syms[0]->elem_region_id = c->ir->vregs[arg_vregs[1]].region_id;
                    arg_syms[0]->elem_region_depth = c->ir->vregs[arg_vregs[1]].region_depth;
                }
            }

            if ((strcmp(name, "vec_set") == 0 || strcmp(name, "map_set") == 0) && num_args == 3) {
                check_container_escape(c, arg_vregs[2], arg_vregs[0], false, line, col);
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
                        vortex_set_vreg_region(c->ir, *out_vreg, true,
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
                    vortex_require_unsafe(c, "Low-level pointer/dynamic FFI builtin", line, col);
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
                           strcmp(name, "str_from_byte") == 0 || strcmp(name, "ptr_to_str") == 0 ||
                           strcmp(name, "str_join") == 0) {
                    ret_t = TYPE_STR; is_alloc_ref = true;
                } else if (strcmp(name, "ffi_load") == 0 || strcmp(name, "ffi_symbol") == 0 ||
                           strcmp(name, "gui_window") == 0) {
                    ret_t = TYPE_PTR;
                } else if (strcmp(name, "sinf_rad") == 0 || strcmp(name, "cosf_rad") == 0 ||
                           strcmp(name, "sqrtf_val") == 0) {
                    ret_t = TYPE_F32;
                }

                *out_vreg = ir_new_vreg(c->ir, ret_t);
                ir_emit_call(c->ir, *out_vreg, bf->func_ptr, arg_vregs, num_args, ret_t);

                if (is_alloc_ref) {
                    vortex_set_vreg_region(c->ir, *out_vreg, true, c->current_region_id, c->current_region_depth);
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
                            c->ir->vregs[*out_vreg].known_len = kb;
                        }
                    }
                }
                if (strcmp(name, "vec_get") == 0 && arg_syms[0] && arg_syms[0]->holds_refs) {
                    vortex_set_vreg_region(c->ir, *out_vreg, true,
                                           arg_syms[0]->elem_region_id, arg_syms[0]->elem_region_depth);
                }
                return ret_t;
            }

            FFIFunc* ff = ffi_func_find(name);
            if (ff) {
                vortex_require_unsafe(c, "Call to extern function", line, col);
                if (g_compile_mode == VORTEX_MODE_SAFE) {
                    for (int a = 0; a < num_args; a++) {
                        VortexType pt = (a < ff->param_count) ? ff->param_types[a] : arg_types[a];
                        if (vortex_is_pointer_type(pt)) {
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
                uf->called = true;
                if (uf->is_unsafe) {
                    vortex_require_unsafe(c, "Call to 'unsafe fn'", line, col);
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
                    vortex_set_vreg_region(c->ir, *out_vreg, true, c->current_region_id, c->current_region_depth);
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

        Symbol* sym = vortex_symtab_find(c->st, name);
        if (!sym) {
            fprintf(stderr, "[VORTEX ERROR] Undefined variable '%s' at %s:%d:%d\n",
                    name, g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
            vortex_error("");
        }
        vortex_check_symbol_alive(c, sym, line, col);
        sym->used = true;

        VortexType primary_type = sym->type;
        int primary_struct_id = sym->struct_id;
        VReg cur_v = sym->vreg;

        /* Array indexing: arr[index] */
        if (l->cur.type == TOK_LBRACKET) {
            next_token(l);
            VReg v_idx;
            vortex_parse_expr(c, &v_idx);
            vortex_parser_match(c, TOK_RBRACKET);

            VortexType elem_type = (sym->type == TYPE_PTR_F32) ? TYPE_F32 :
                                   (sym->type == TYPE_PTR_I32) ? TYPE_I32 :
                                   (sym->type == TYPE_PTR_STRUCT) ? TYPE_PTR_STRUCT : TYPE_I64;
            int32_t elem_size = (elem_type == TYPE_F32 || elem_type == TYPE_I32) ? 4 : 8;

            bool needs_bounds = (c->unsafe_depth == 0) || sym->is_slice_ptr;
            if (!sym->is_slice_ptr && c->unsafe_depth == 0) {
                vortex_require_unsafe(c, "Indexing untracked raw pointer", line, col);
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

            VortexField* f = vortex_resolve_struct_field(primary_struct_id, fname);
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
    VortexType t = vortex_parse_primary(c, out_vreg);

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

        if ((vortex_is_pointer_type(t) || vortex_is_pointer_type(dst_t)) && t != dst_t) {
            vortex_require_unsafe(c, "Pointer/integer cast ('as')", line, col);
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
            if (vortex_is_pointer_type(dst_t)) {
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

        if (vortex_is_pointer_type(left_type) || vortex_is_pointer_type(right_type)) {
            vortex_require_unsafe(c, "Pointer arithmetic", line, col);
        }

        if (left_type == TYPE_F32 || right_type == TYPE_F32) {
            VReg dest = ir_new_vreg(c->ir, TYPE_F32);
            if (op == TOK_STAR) ir_emit_binop(c->ir, IR_FMUL, dest, *out_vreg, r_vreg, TYPE_F32);
            else ir_emit_binop(c->ir, IR_FDIV, dest, *out_vreg, r_vreg, TYPE_F32);
            *out_vreg = dest;
            left_type = TYPE_F32;
        } else {
            if ((op == TOK_SLASH || op == TOK_PERCENT) && g_ultrasafe_level >= 1 && c->unsafe_depth == 0) {
                ir_emit_div_check(c->ir, r_vreg, g_current_filename, line);
            }
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

        if (vortex_is_pointer_type(left_type) || vortex_is_pointer_type(right_type)) {
            vortex_require_unsafe(c, "Raw pointer arithmetic", line, col);
            VortexType ptr_t = vortex_is_pointer_type(left_type) ? left_type : right_type;
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

static VortexType parse_shift(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_additive(c, out_vreg);
    while (c->lex->cur.type == TOK_SHL || c->lex->cur.type == TOK_SHR) {
        VortexTokenType op = c->lex->cur.type;
        next_token(c->lex);
        VReg r_vreg;
        parse_additive(c, &r_vreg);
        VReg dest = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_binop(c->ir, (op == TOK_SHL) ? IR_SHL : IR_SHR, dest, *out_vreg, r_vreg, TYPE_I64);
        *out_vreg = dest;
        left_type = TYPE_I64;
    }
    return left_type;
}

static VortexType parse_relational(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_shift(c, out_vreg);
    VortexTokenType t = c->lex->cur.type;

    while (t == TOK_LT || t == TOK_LE || t == TOK_GT || t == TOK_GE || t == TOK_EQEQ || t == TOK_NEQ) {
        VortexTokenType op = t;
        next_token(c->lex);
        VReg r_vreg;
        VortexType right_type = parse_shift(c, &r_vreg);
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
        left_type = TYPE_I64;
        t = c->lex->cur.type;
    }
    return left_type;
}

static VortexType parse_bitwise_and(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_relational(c, out_vreg);
    while (c->lex->cur.type == TOK_AMP) {
        next_token(c->lex);
        VReg r_vreg;
        parse_relational(c, &r_vreg);
        VReg dest = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_binop(c->ir, IR_BAND, dest, *out_vreg, r_vreg, TYPE_I64);
        *out_vreg = dest;
        left_type = TYPE_I64;
    }
    return left_type;
}

static VortexType parse_bitwise_xor(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_bitwise_and(c, out_vreg);
    while (c->lex->cur.type == TOK_CARET) {
        next_token(c->lex);
        VReg r_vreg;
        parse_bitwise_and(c, &r_vreg);
        VReg dest = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_binop(c->ir, IR_BXOR, dest, *out_vreg, r_vreg, TYPE_I64);
        *out_vreg = dest;
        left_type = TYPE_I64;
    }
    return left_type;
}

static VortexType parse_bitwise_or(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_bitwise_xor(c, out_vreg);
    while (c->lex->cur.type == TOK_PIPE) {
        next_token(c->lex);
        VReg r_vreg;
        parse_bitwise_xor(c, &r_vreg);
        VReg dest = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_binop(c->ir, IR_BOR, dest, *out_vreg, r_vreg, TYPE_I64);
        *out_vreg = dest;
        left_type = TYPE_I64;
    }
    return left_type;
}

static VortexType parse_logical_and(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_bitwise_or(c, out_vreg);
    while (c->lex->cur.type == TOK_AND) {
        next_token(c->lex);
        int lbl_false = ir_new_label(c->ir);
        int lbl_end = ir_new_label(c->ir);
        VReg res = ir_new_vreg(c->ir, TYPE_I64);
        VReg v_zero = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, v_zero, 0);

        ir_emit_jz(c->ir, *out_vreg, lbl_false);
        VReg r_vreg;
        parse_bitwise_or(c, &r_vreg);
        ir_emit_binop(c->ir, IR_NE, res, r_vreg, v_zero, TYPE_I64);
        ir_emit_jmp(c->ir, lbl_end);

        ir_emit_label(c->ir, lbl_false);
        ir_emit_const_i64(c->ir, res, 0);
        ir_emit_label(c->ir, lbl_end);

        *out_vreg = res;
        left_type = TYPE_I64;
    }
    return left_type;
}

VortexType vortex_parse_expr(Compiler* c, VReg* out_vreg) {
    VortexType left_type = parse_logical_and(c, out_vreg);
    while (c->lex->cur.type == TOK_OR) {
        next_token(c->lex);
        int lbl_false = ir_new_label(c->ir);
        int lbl_true = ir_new_label(c->ir);
        int lbl_end = ir_new_label(c->ir);
        VReg res = ir_new_vreg(c->ir, TYPE_I64);
        VReg v_zero = ir_new_vreg(c->ir, TYPE_I64);
        ir_emit_const_i64(c->ir, v_zero, 0);

        ir_emit_jz(c->ir, *out_vreg, lbl_false);
        ir_emit_jmp(c->ir, lbl_true);

        ir_emit_label(c->ir, lbl_false);
        VReg r_vreg;
        parse_logical_and(c, &r_vreg);
        ir_emit_binop(c->ir, IR_NE, res, r_vreg, v_zero, TYPE_I64);
        ir_emit_jmp(c->ir, lbl_end);

        ir_emit_label(c->ir, lbl_true);
        ir_emit_const_i64(c->ir, res, 1);
        ir_emit_label(c->ir, lbl_end);

        *out_vreg = res;
        left_type = TYPE_I64;
    }
    return left_type;
}

bool vortex_try_match_and_emit_unrolled_riemann(Compiler* c, Symbol* i_sym, Symbol* lim_sym) {
    Lexer save_lex = *c->lex;

    if (c->lex->cur.type != TOK_LBRACE) return false;
    next_token(c->lex);

    if (c->lex->cur.type != TOK_IDENT) { *c->lex = save_lex; return false; }
    Symbol* acc_sym = vortex_symtab_find(c->st, c->lex->cur.text);
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
    Symbol* x_sym = vortex_symtab_find(c->st, c->lex->cur.text);
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
    Symbol* step_sym = vortex_symtab_find(c->st, c->lex->cur.text);
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
