/*
 * VORTEX PROGRAMMING LANGUAGE - COMMON COMPILER & RUNTIME HEADER
 * Modular C99 Architecture (Zero-GC, 2MB Paged Arena, Linear-Scan IR, AVX2, Dual-ABI x86-64 JIT)
 */

#ifndef VORTEX_H
#define VORTEX_H

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#ifndef VORTEX_NO_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#endif
#endif

#define VORTEX_VERSION "1.0.0"

/* ========================================================================= */
/* 1. MEMORY ARCHITECTURE CONSTANTS & TYPES                                  */
/* ========================================================================= */

#define VORTEX_PAGE_SIZE   (2 * 1024 * 1024) /* 2 MB Huge Pages */
#define VORTEX_ALIGNMENT   64                /* 64-byte alignment (Cache line & AVX-512) */
#define MAX_JIT_CODE_SIZE  (256 * 1024)

typedef struct VortexPage {
    uint8_t* base;
    size_t cursor;
    size_t capacity;
    struct VortexPage* next;
} VortexPage;

typedef struct {
    VortexPage* head;
    VortexPage* current;
    size_t current_page_index;
    size_t total_pages;
} VortexRegionArena;

typedef struct {
    int64_t* data;
    int64_t len;
    int64_t cap;
} VortexVec;

typedef struct {
    char* key;
    int64_t value;
    bool occupied;
} MapEntry;

typedef struct {
    MapEntry* entries;
    int64_t count;
    int64_t capacity;
} VortexMap;

/* ========================================================================= */
/* 2. THREAD POOL TYPES                                                      */
/* ========================================================================= */

#define VORTEX_MAX_THREADS 64

typedef void (*ParallelJobFn)(void* ctx, int64_t start, int64_t end);

typedef struct {
#ifdef _WIN32
    HANDLE thread;
    HANDLE start_event;
    HANDLE done_event;
#else
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t start_cond;
    pthread_cond_t done_cond;
    bool job_ready;
    bool job_done;
#endif
    int id;
    ParallelJobFn job_fn;
    void* job_ctx;
    int64_t slice_start;
    int64_t slice_end;
    bool should_exit;
    VortexRegionArena arena;
} VortexWorker;

typedef struct {
    int worker_count;
    VortexWorker workers[VORTEX_MAX_THREADS];
#ifdef _WIN32
    HANDLE done_handles[VORTEX_MAX_THREADS];
#endif
    bool initialized;
} VortexThreadPool;

/* ========================================================================= */
/* 3. LEXER, SYMBOL TABLE, USER FUNCTIONS & FFI TYPES                        */
/* ========================================================================= */

typedef enum {
    TOK_EOF = 0,
    TOK_LET,
    TOK_MUT,
    TOK_FOR,
    TOK_PARALLEL_FOR,
    TOK_IN,
    TOK_WHILE,
    TOK_IF,
    TOK_ELSE,
    TOK_FN,
    TOK_RETURN,
    TOK_STRUCT,
    TOK_EXTERN,
    TOK_ARROW,
    TOK_I64_KW,
    TOK_F32_KW,
    TOK_IDENT,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_DOT,
    TOK_DOTDOT,
    TOK_EQ,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_SEMI,
    TOK_COMMA,
    TOK_COLON
} VortexTokenType;

typedef struct {
    VortexTokenType type;
    char text[128];
    int64_t int_val;
    float float_val;
    int line;
    int col;
} Token;

typedef struct {
    const char* src;
    size_t pos;
    size_t len;
    int line;
    int col;
    Token cur;
} Lexer;

typedef enum {
    TYPE_UNKNOWN = 0,
    TYPE_VOID,
    TYPE_I64,
    TYPE_I32,
    TYPE_F32,
    TYPE_F64,
    TYPE_STR,
    TYPE_PTR,
    TYPE_PTR_I64,
    TYPE_PTR_F32,
    TYPE_STRUCT,
    TYPE_PTR_STRUCT
} VortexType;

typedef struct {
    char name[64];
    VortexType type;
    int struct_id; /* -1 if not struct, or index in g_struct_types */
    int stack_offset;
    bool is_mut;
    int vreg;
} Symbol;

#define MAX_FIELDS 16
#define MAX_STRUCT_TYPES 32
#define MAX_SYMBOLS 256
#define MAX_USER_FUNCS 64
#define MAX_USER_FUNC_RELOCS 256
#define MAX_DYN_LIBS 64
#define MAX_FFI_FUNCS 128
#define MAX_LITERAL_STRINGS 256

typedef struct {
    char name[64];
    VortexType type;
    int offset;
    int size;
    int align;
    int struct_id;
} VortexField;

typedef struct {
    char name[64];
    int id;
    int size;
    int align;
    int field_count;
    VortexField fields[MAX_FIELDS];
} VortexStructType;

typedef struct {
    Symbol symbols[MAX_SYMBOLS];
    int count;
    int stack_alloc_bytes;
} SymbolTable;

typedef struct {
    char name[64];
    size_t code_offset;
    int param_count;
    char param_names[8][64];
} UserFunc;

typedef struct {
    size_t patch_pos;
    char name[64];
} UserFuncReloc;

typedef struct {
    const char* name;
    void* func_ptr;
    int arg_count;
} BuiltinFunc;

typedef struct {
    char name[128];
    void* handle;
} VortexDynLib;

typedef struct {
    char lib_name[128];
    char fn_name[64];
    void* func_ptr;
    VortexType ret_type;
    int param_count;
    char param_names[8][64];
    VortexType param_types[8];
} FFIFunc;

/* ========================================================================= */
/* 4. JIT CODE BUFFER, PHYSICAL REGISTERS & 3-ADDRESS LINEAR IR              */
/* ========================================================================= */

typedef struct {
    uint8_t* code;
    size_t size;
    size_t capacity;
} JITBuffer;

typedef enum {
    PREG_RAX = 0,
    PREG_RCX = 1,
    PREG_RDX = 2,
    PREG_RBX = 3,
    PREG_RSP = 4,
    PREG_RBP = 5,
    PREG_RSI = 6,
    PREG_RDI = 7,
    PREG_R8  = 8,
    PREG_R9  = 9,
    PREG_R10 = 10,
    PREG_R11 = 11,
    PREG_R12 = 12,
    PREG_R13 = 13,
    PREG_R14 = 14,
    PREG_R15 = 15,
    PREG_NONE = -1
} PReg;

#define MAX_IR_INSTRS 65536
#define MAX_VREGS 16384
#define MAX_LABELS 4096

typedef enum {
    IR_NOP = 0,
    IR_CONST_I64,
    IR_CONST_F32,
    IR_MOV,
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_DIV,
    IR_MOD,
    IR_FADD,
    IR_FSUB,
    IR_FMUL,
    IR_FDIV,
    IR_EQ,
    IR_NE,
    IR_LT,
    IR_LE,
    IR_GT,
    IR_GE,
    IR_FEQ,
    IR_FNE,
    IR_FLT,
    IR_FLE,
    IR_FGT,
    IR_FGE,
    IR_LOAD_OFFSET,
    IR_STORE_OFFSET,
    IR_LOAD_ARRAY,
    IR_STORE_ARRAY,
    IR_LABEL,
    IR_JMP,
    IR_JZ,
    IR_JNZ,
    IR_CALL,
    IR_CALL_USER,
    IR_CALL_FFI,
    IR_LOAD_PARAM,
    IR_RET,
    IR_RAW_RIEMANN,
    IR_VEC_LOOP
} IROpcode;

typedef int VReg;
#define VREG_NONE -1

typedef struct IRInstruction {
    IROpcode op;
    VortexType type;
    VReg dest;
    VReg src1;
    VReg src2;
    VReg src3;
    int64_t imm;
    float fimm;
    int32_t offset;
    void* func_ptr;
    char func_name[64];
    int arg_count;
    VReg args[8];
    VortexType ret_type;
    VortexType arg_types[8];
    /* Vector / Riemann loop fields */
    VReg i_vreg;
    VReg lim_vreg;
    VReg acc_vreg;
    VReg x_vreg;
    VReg step_vreg;
    VReg term_vreg;
    float num_val;
    float den_base;
    int body_op_count;
    struct IRInstruction* body_ops;
} IRInstruction;

typedef struct {
    int id;
    VortexType type;
    bool is_float;
    int start_ip;
    int end_ip;
    bool crosses_call;
    int preg;
    int spill_offset;
    bool is_spilled;
} VRegInfo;

typedef struct {
    IRInstruction* instrs;
    int count;
    int capacity;
    VRegInfo* vregs;
    int vreg_count;
    int vreg_capacity;
    int next_label;
    int total_spill_bytes;
} IRBuffer;

typedef struct {
    int vreg_id;
    int start_ip;
    int end_ip;
} LiveInterval;

typedef struct {
    Lexer* lex;
    SymbolTable* st;
    JITBuffer* j;
    IRBuffer* ir;
    int last_struct_id;
} Compiler;

/* ========================================================================= */
/* 5. STANDALONE PACKAGING STRUCTURES                                        */
/* ========================================================================= */

#define VORTEX_PACK_MAGIC_HEADER "VRTX"
#define VORTEX_PACK_MAGIC_FOOTER "VTXPACK1"
#define VORTEX_PACK_VERSION 1

#pragma pack(push, 1)
typedef struct {
    char header_magic[4];     /* "VRTX" */
    uint32_t version;         /* 1 */
    uint32_t flags;           /* 0 */
    uint32_t num_structs;
    uint32_t num_constants;
    uint32_t source_len;
} VortexPackageHeader;

typedef struct {
    uint64_t payload_offset;
    char footer_magic[8];     /* "VTXPACK1" */
} VortexPackageFooter;
#pragma pack(pop)

/* ========================================================================= */
/* 6. SHARED GLOBAL STATE                                                    */
/* ========================================================================= */

extern jmp_buf g_error_jmp;
extern bool g_in_repl;

extern VortexStructType g_struct_types[MAX_STRUCT_TYPES];
extern int g_struct_type_count;

extern UserFunc g_user_funcs[MAX_USER_FUNCS];
extern int g_user_func_count;

extern UserFuncReloc g_user_func_relocs[MAX_USER_FUNC_RELOCS];
extern int g_user_func_reloc_count;

extern FFIFunc g_ffi_funcs[MAX_FFI_FUNCS];
extern int g_ffi_func_count;

extern char* g_literal_strings[MAX_LITERAL_STRINGS];
extern int g_literal_string_count;

extern SymbolTable g_last_symtab;
extern bool g_has_last_symtab;

/* ========================================================================= */
/* 7. MODULE EXPORTS                                                         */
/* ========================================================================= */

/* --- src/arena.c --- */
VortexPage* arena_page_create(size_t min_capacity);
void arena_init(void);
void arena_set_thread_local(VortexRegionArena* ar);
void* arena_alloc_fast(size_t bytes);
void* arena_alloc(size_t bytes);
int64_t vortex_alloc_struct(int64_t size);
int64_t vortex_scope_push(void);
int64_t vortex_scope_pop(int64_t mark);

int64_t vortex_vec_new(void);
int64_t vortex_vec_push(int64_t vec_ptr, int64_t val);
int64_t vortex_vec_get(int64_t vec_ptr, int64_t idx);
int64_t vortex_vec_set(int64_t vec_ptr, int64_t idx, int64_t val);
int64_t vortex_vec_len(int64_t vec_ptr);

int64_t vortex_map_new(void);
int64_t vortex_map_set(int64_t map_ptr, int64_t key_ptr, int64_t val);
int64_t vortex_map_get(int64_t map_ptr, int64_t key_ptr);
int64_t vortex_map_has(int64_t map_ptr, int64_t key_ptr);
int64_t vortex_map_len(int64_t map_ptr);

int64_t vortex_str_concat(int64_t s1_ptr, int64_t s2_ptr);
int64_t vortex_str_len(int64_t s_ptr);
int64_t vortex_str_sub(int64_t s_ptr, int64_t start, int64_t len);
int64_t vortex_str_from_i64(int64_t n);
int64_t vortex_str_from_f32(int64_t f_bits);

/* --- src/lexer_parser.c --- */
void lexer_init(Lexer* lex, const char* source);
void next_token(Lexer* lex);
VortexStructType* struct_type_find(const char* name);
VortexStructType* struct_type_find_by_id(int id);
VortexField* struct_field_find(VortexStructType* st, const char* field_name);
VortexType parse_type_name(Lexer* l, int* out_struct_id);
int64_t vortex_builtin_struct_size(int64_t name_ptr);
int64_t vortex_builtin_struct_align(int64_t name_ptr);
UserFunc* user_func_find(const char* name);
void parse_statement(Compiler* c);

/* --- src/ir_opt.c --- */
void ir_init(IRBuffer* ir);
void ir_free(IRBuffer* ir);
VReg ir_new_vreg(IRBuffer* ir, VortexType type);
int ir_new_label(IRBuffer* ir);
IRInstruction* ir_emit(IRBuffer* ir, IROpcode op);
void ir_emit_const_i64(IRBuffer* ir, VReg dest, int64_t val);
void ir_emit_const_f32(IRBuffer* ir, VReg dest, float val);
void ir_emit_mov(IRBuffer* ir, VReg dest, VReg src);
void ir_emit_binop(IRBuffer* ir, IROpcode op, VReg dest, VReg s1, VReg s2, VortexType t);
void ir_emit_load_offset(IRBuffer* ir, VReg dest, VReg base, int32_t offset, VortexType t);
void ir_emit_store_offset(IRBuffer* ir, VReg base, int32_t offset, VReg val, VortexType t);
void ir_emit_load_array(IRBuffer* ir, VReg dest, VReg base, VReg idx, VortexType t);
void ir_emit_store_array(IRBuffer* ir, VReg base, VReg idx, VReg val, VortexType t);
void ir_emit_label(IRBuffer* ir, int lbl);
void ir_emit_jmp(IRBuffer* ir, int lbl);
void ir_emit_jz(IRBuffer* ir, VReg cond, int lbl);
void ir_emit_call(IRBuffer* ir, VReg dest, void* fn_ptr, VReg* args, int num_args, VortexType ret_t);
void ir_emit_call_user(IRBuffer* ir, VReg dest, const char* name, VReg* args, int num_args, VortexType ret_t);
void ir_emit_call_ffi(IRBuffer* ir, VReg dest, FFIFunc* ff, VReg* args, int num_args, VortexType ret_t);
void ir_emit_load_param(IRBuffer* ir, VReg dest, int param_idx);
void ir_emit_ret(IRBuffer* ir, VReg val, VortexType t);
void ir_liveness_analysis(IRBuffer* ir);
void ir_loop_vectorize(IRBuffer* ir);
void ir_linear_scan_allocate(IRBuffer* ir);

/* --- src/jit_x64.c --- */
void* alloc_exec_page(size_t size);
void free_exec_page(void* ptr, size_t size);
size_t emit_jmp_placeholder(JITBuffer* j);
void patch_jump(JITBuffer* j, size_t patch_pos);
int compute_frame_size(int total_spill_bytes);
void emit_prologue(JITBuffer* j, int frame_size);
void emit_epilogue(JITBuffer* j, int frame_size);
void emit_func_prologue(JITBuffer* j, int frame_size);
void emit_func_epilogue(JITBuffer* j, int frame_size);
void ir_codegen_x86_64(IRBuffer* ir, JITBuffer* j);

void* vortex_load_dynlib(const char* lib_name);
void* vortex_resolve_symbol(void* handle, const char* sym_name);
FFIFunc* ffi_func_find(const char* name);
int64_t vortex_ffi_load(int64_t lib_name_ptr);
int64_t vortex_ffi_symbol(int64_t lib_handle, int64_t sym_name_ptr);
int64_t vortex_ffi_call0(int64_t fn_ptr);
int64_t vortex_ffi_call1(int64_t fn_ptr, int64_t a1);
int64_t vortex_ffi_call2(int64_t fn_ptr, int64_t a1, int64_t a2);
int64_t vortex_ffi_call3(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3);
int64_t vortex_ffi_call4(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4);

/* --- src/runtime.c --- */
void vortex_error(const char* msg);
void helper_print_i64(int64_t val);
void helper_print_f32(float val);
void helper_print_str(const char* s);
int64_t vortex_math_sin(int64_t deg_x10);
int64_t vortex_math_cos(int64_t deg_x10);
int64_t vortex_math_sqrt(int64_t x);
int64_t helper_time_us(void);

int64_t vortex_gui_window(int64_t title_ptr, int64_t width, int64_t height);
int64_t vortex_gui_set_pos(int64_t win_ptr, int64_t x, int64_t y);
int64_t vortex_gui_is_open(int64_t win_ptr);
int64_t vortex_gui_draw_text(int64_t win_ptr, int64_t text_ptr, int64_t x, int64_t y, int64_t color_rgb);
int64_t vortex_gui_present(int64_t win_ptr);
int64_t vortex_gui_rgb(int64_t r, int64_t g, int64_t b);
int64_t vortex_gui_clear(int64_t win_ptr, int64_t color_rgb);
int64_t vortex_gui_draw_rect(int64_t win_ptr, int64_t x, int64_t y, int64_t width, int64_t height, int64_t color_rgb);
int64_t vortex_gui_draw_pixel(int64_t win_ptr, int64_t x, int64_t y, int64_t color_rgb);
int64_t vortex_gui_draw_line(int64_t win_ptr, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t color_rgb);
int64_t vortex_gui_draw_number(int64_t win_ptr, int64_t num, int64_t x, int64_t y, int64_t color_rgb);
int64_t vortex_gui_button(int64_t win_ptr, int64_t id, int64_t label_ptr, int64_t x, int64_t y, int64_t w_btn, int64_t h_btn);

void vortex_thread_pool_init(void);
void vortex_parallel_for_dispatch(void* fn_ptr, void* ctx, int64_t start, int64_t end);
int64_t vortex_num_workers(void);
BuiltinFunc* builtin_find(const char* name);

int execute_vortex_source(const char* source);
int execute_vortex_file(const char* filename);
bool check_and_run_embedded_payload(int argc, char** argv);
int vortex_cmd_build(int argc, char** argv);

#endif /* VORTEX_H */
