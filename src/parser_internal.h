/*
 * VORTEX INTERNAL FRONTEND HEADER
 * Shared declarations across lexer.c, modules.c, parser_expr.c, parser_stmt.c.
 */

#ifndef VORTEX_PARSER_INTERNAL_H
#define VORTEX_PARSER_INTERNAL_H

#include "vortex.h"
#ifdef fprintf
#undef fprintf
#endif
#define fprintf vortex_diag_emit

/* --- Type, Symbol, Safety & Region Helpers (src/parser_expr.c) --- */
VortexType vortex_parse_type_spec_full(Lexer* l, int* out_struct_id, bool* out_nullable);
VortexField* vortex_resolve_struct_field(int struct_id, const char* fname);
Symbol* vortex_symtab_find(SymbolTable* st, const char* name);
Symbol* vortex_symtab_add(SymbolTable* st, const char* name, VortexType type, bool is_mut);

typedef struct {
    char name[64];
    VortexType type;
    int64_t int_val;
    float float_val;
    double f64_val;
    const char* str_val;
} VortexConst;

VortexConst* vortex_const_find(const char* name);
VortexConst* vortex_const_add(const char* name, VortexType type, int64_t iv, float fv, double f64v, const char* sv);
void vortex_const_reset(void);
bool vortex_is_pointer_type(VortexType t);
bool vortex_is_scalar_int_type(VortexType t);
VReg vortex_promote_to_f64(Compiler* c, VReg v, VortexType t);
void vortex_require_unsafe(Compiler* c, const char* what, int line, int col);
bool vortex_region_is_popped(Compiler* c, int rid);
void vortex_mark_region_popped(Compiler* c, int rid);
void vortex_check_symbol_alive(Compiler* c, Symbol* sym, int line, int col);
void vortex_stamp_ir_loc(IRBuffer* ir, int from_idx, int line);
void vortex_set_vreg_region(IRBuffer* ir, VReg v, bool is_ref, int rid, int depth);

/* --- Parser Primitives & Expressions (src/parser_expr.c) --- */
void vortex_parser_match(Compiler* c, VortexTokenType expected);
VortexType vortex_parse_primary(Compiler* c, VReg* out_vreg);
VortexType vortex_parse_match_pattern(Compiler* c, VReg* out_vreg);
VortexType vortex_parse_expr(Compiler* c, VReg* out_vreg);
bool vortex_try_match_and_emit_unrolled_riemann(Compiler* c, Symbol* i_sym, Symbol* lim_sym);

/* --- Module / CFG Helpers (src/modules.c) --- */
bool vortex_has_imported_path(const char* path);
void vortex_mark_imported_path(const char* path);
char* vortex_read_file_alloc(const char* path, size_t* out_len);
void vortex_parse_import_spec(Lexer* l, char* spec, size_t cap);
bool vortex_eval_cfg_predicate(const char* pred);
void vortex_skip_cfg_item(Compiler* c);

#endif /* VORTEX_PARSER_INTERNAL_H */
