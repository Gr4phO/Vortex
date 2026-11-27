/*
 * VORTEX PROGRAMMING LANGUAGE - COMMON COMPILER & RUNTIME HEADER
 * Modular C99 Architecture (Zero-GC, 2MB Paged Arena, Linear-Scan IR, AVX2, Dual-ABI x86-64 JIT)
 */

#ifndef VORTEX_H
#define VORTEX_H

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#ifndef _WIN32
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
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
#ifdef _MSC_VER
#include <intrin.h>
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <mach/mach_time.h>
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif
#if !defined(VORTEX_ENABLE_X11) && !defined(VORTEX_NO_X11)
#define VORTEX_NO_X11
#endif
#endif
#if defined(VORTEX_HEADLESS) && !defined(VORTEX_NO_X11)
#define VORTEX_NO_X11
#endif
#ifndef VORTEX_NO_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#endif
#endif

#ifndef VORTEX_VERSION
#define VORTEX_VERSION "1.1.0"
#endif

/* ========================================================================= */
/* 1. MEMORY ARCHITECTURE CONSTANTS & TYPES                                  */
/* ========================================================================= */

#define VORTEX_PAGE_SIZE       (2 * 1024 * 1024) /* 2 MB Huge Pages */
#define VORTEX_ALIGNMENT       64                /* 64-byte alignment (Cache line & AVX-512) */
#define MAX_JIT_CODE_SIZE      (16 * 1024 * 1024)
#define VORTEX_MAX_SCOPE_DEPTH 256
#define VORTEX_SAFE_MAGIC      0x5654585341464531ULL /* "VTXSAFE1" */

typedef enum {
    VORTEX_MODE_SAFE = 0,
    VORTEX_MODE_RELEASE = 1
} VortexBuildMode;

typedef struct VortexPage {
    uint8_t* base;
    size_t cursor;
    size_t capacity;
    size_t high_water;
    struct VortexPage* next;
} VortexPage;

typedef struct {
    VortexPage* head;
    VortexPage* current;
    size_t current_page_index;
    size_t total_pages;
    uint32_t scope_depth;
    uint64_t next_epoch;
    uint64_t scope_stack[VORTEX_MAX_SCOPE_DEPTH];
} VortexRegionArena;

typedef struct {
    uint64_t magic;
    VortexRegionArena* owner_arena;
    uint64_t epoch;
    uint64_t byte_size;
    uint64_t elem_count;
    uint32_t scope_depth;
    uint32_t elem_size;
    uint32_t page_idx;
    uint32_t page_offset;
    uint64_t reserved;
} VortexSafeHeader;

typedef char vortex_assert_safe_hdr_size[(sizeof(VortexSafeHeader) == 64) ? 1 : -1];

typedef struct {
    int64_t* data;
    int64_t len;
    int64_t cap;
} VortexVec;

typedef struct {
    char* key;
    int64_t value;
    uint64_t hash;
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
    TOK_UNSAFE,
    TOK_AS,
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
    TOK_COLON,
    TOK_QUESTION,
    TOK_IMPORT,
    TOK_HASH,
    TOK_TRUE,
    TOK_FALSE,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_ENUM,
    TOK_MATCH,
    TOK_FAT_ARROW,
    TOK_AND,
    TOK_OR,
    TOK_BANG,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_TILDE,
    TOK_SHL,
    TOK_SHR,
    TOK_PLUS_EQ,
    TOK_MINUS_EQ,
    TOK_STAR_EQ,
    TOK_SLASH_EQ,
    TOK_PERCENT_EQ,
    TOK_AMP_EQ,
    TOK_PIPE_EQ,
    TOK_CARET_EQ,
    TOK_SHL_EQ,
    TOK_SHR_EQ,
    TOK_COLONCOLON
} VortexTokenType;

typedef struct {
    VortexTokenType type;
    char text[4096];
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
    TYPE_PTR_I32,
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
    /* Jalon 1: Static Region & Bounds Metadata */
    bool is_ref;
    bool holds_refs;
    bool is_region_mark;
    bool is_slice_ptr;
    int region_id;
    int region_depth;
    int elem_region_id;
    int elem_region_depth;
    int len_vreg;
    int64_t known_len;
    bool used;
    bool reassigned;
    int decl_line;
    int decl_col;
} Symbol;

#define MAX_FIELDS 64
#define MAX_STRUCT_TYPES 1024
#define MAX_ENUM_TYPES 256
#define MAX_ENUM_VARIANTS 128
#define MAX_SYMBOLS 1024
#define MAX_USER_FUNCS 8192
#define MAX_USER_FUNC_RELOCS 32768
#define MAX_DYN_LIBS 256
#define MAX_FFI_FUNCS 2048
#define MAX_LITERAL_STRINGS 16384
#define MAX_CALL_ARGS 16
#define MAX_COMPILE_REGIONS 64
#define MAX_LOOP_DEPTH 64
#define MAX_DIAGNOSTICS 256

typedef struct {
    char name[64];
    int64_t value;
} VortexEnumVariant;

typedef struct {
    char name[64];
    int variant_count;
    VortexEnumVariant variants[MAX_ENUM_VARIANTS];
} VortexEnumType;

typedef enum {
    VDIAG_INFO = 0,
    VDIAG_WARNING = 1,
    VDIAG_ERROR = 2
} VortexDiagSeverity;

typedef struct {
    VortexDiagSeverity severity;
    char code[16];
    char file[256];
    int line;
    int col;
    char message[512];
    char suggestion[256];
} VortexDiagnostic;

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
    char param_names[MAX_CALL_ARGS][64];
    VortexType param_types[MAX_CALL_ARGS];
    int param_struct_ids[MAX_CALL_ARGS];
    VortexType ret_type;
    int ret_struct_id;
    bool is_unsafe;
    bool returns_ref;
    bool can_inline;
    bool called;
    int decl_line;
    int inline_ir_count;
    int inline_vreg_count;
    void* inline_instrs;
    void* inline_vregs;
} UserFunc;

typedef struct {
    size_t patch_pos;
    char name[64];
    bool called_in_unsafe;
    int call_line;
    const char* call_file;
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
    char param_names[MAX_CALL_ARGS][64];
    VortexType param_types[MAX_CALL_ARGS];
    bool param_nullable[MAX_CALL_ARGS];
    bool is_variadic;
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

typedef enum {
    A64_X0  = 0,  A64_X1  = 1,  A64_X2  = 2,  A64_X3  = 3,
    A64_X4  = 4,  A64_X5  = 5,  A64_X6  = 6,  A64_X7  = 7,
    A64_X8  = 8,  A64_X9  = 9,  A64_X10 = 10, A64_X11 = 11,
    A64_X12 = 12, A64_X13 = 13, A64_X14 = 14, A64_X15 = 15,
    A64_X16 = 16, A64_X17 = 17, A64_X18 = 18, A64_X19 = 19,
    A64_X20 = 20, A64_X21 = 21, A64_X22 = 22, A64_X23 = 23,
    A64_X24 = 24, A64_X25 = 25, A64_X26 = 26, A64_X27 = 27,
    A64_X28 = 28, A64_FP  = 29, A64_LR  = 30, A64_XZR = 31
} A64Reg;

typedef enum {
    VORTEX_ARCH_X86_64 = 0,
    VORTEX_ARCH_ARM64  = 1
} VortexTargetArch;

typedef enum {
    VORTEX_ABI_WIN64    = 0,
    VORTEX_ABI_SYSV64   = 1,
    VORTEX_ABI_AAPCS64  = 2,
    VORTEX_ABI_DARWIN64 = 3
} VortexTargetABI;

typedef enum {
    VORTEX_SIMD_NONE = 0,
    VORTEX_SIMD_AVX2 = 1, /* 256-bit AVX2 (8 x f32) */
    VORTEX_SIMD_NEON = 2, /* 128-bit ASIMD/NEON (4 x f32) */
    VORTEX_SIMD_SVE  = 4  /* Scalable Vector Extension (optional) */
} VortexSIMDCap;

typedef struct {
    VortexTargetArch arch;
    VortexTargetABI abi;
    const char* name;
    uint32_t simd_caps;
    int simd_width_bits;
    int simd_lanes_f32;
    int spill_base_offset;
    int int_callee_regs[16];
    int int_callee_count;
    int int_scratch_regs[16];
    int int_scratch_count;
    int float_callee_regs[16];
    int float_callee_count;
    int float_scratch_regs[16];
    int float_scratch_count;
} VortexTargetDesc;

#define MAX_IR_INSTRS 262144
#define MAX_VREGS 65536
#define MAX_LABELS 16384

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
    IR_CAST_I64_TO_F32,
    IR_CAST_F32_TO_I64,
    IR_LOAD_OFFSET,
    IR_STORE_OFFSET,
    IR_LOAD_ARRAY,
    IR_STORE_ARRAY,
    IR_LOAD_DEREF,
    IR_STORE_DEREF,
    IR_BOUNDS_CHECK,
    IR_RANGE_CHECK,
    IR_PTR_CHECK,
    IR_FFI_CHECK,
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
    IR_VEC_LOOP,
    IR_SUB_MOD_IMM,
    IR_VEC_ARR_OP,
    IR_VEC_GRID_MOD,
    IR_VEC_MOD_FILL,
    IR_BAND,
    IR_BOR,
    IR_BXOR,
    IR_BNOT,
    IR_SHL,
    IR_SHR,
    IR_LNOT,
    IR_DIV_CHECK
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
    VReg src4;
    int64_t imm;
    float fimm;
    int32_t offset;
    void* func_ptr;
    char func_name[64];
    int arg_count;
    VReg args[MAX_CALL_ARGS];
    VortexType ret_type;
    VortexType arg_types[MAX_CALL_ARGS];
    bool is_variadic;
    int fixed_param_count;
    /* Source location & region safety metadata */
    int line;
    int col;
    const char* file;
    int region_id;
    int region_depth;
    bool is_ref_result;
    int64_t known_len;
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
    /* Static region & bounds metadata */
    bool is_ref;
    int region_id;
    int region_depth;
    int len_vreg;
    int64_t known_len;
    bool is_slice_ptr;
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
    bool uses_avx;
    bool has_calls;
    bool optimize_frame;
    bool omit_fp;
    int param_count;
    int max_call_args;
    uint32_t callee_save_mask;
    uint32_t float_callee_save_mask;
    char func_name[64];
} IRBuffer;

typedef struct {
    int vreg_id;
    int start_ip;
    int end_ip;
} LiveInterval;

typedef struct {
    int region_id;
    int depth;
    char mark_name[64];
    bool active;
} CompileRegion;

typedef struct {
    Lexer* lex;
    SymbolTable* st;
    JITBuffer* j;
    IRBuffer* ir;
    int last_struct_id;
    /* Jalon 1: Unsafe & Static Arena Region tracking */
    int unsafe_depth;
    int region_counter;
    int current_region_depth;
    int current_region_id;
    CompileRegion region_stack[MAX_COMPILE_REGIONS];
    int popped_regions[256];
    int popped_region_count;
    bool current_fn_is_unsafe;
    bool in_function;
    bool block_returned;
    bool in_match_arm;
    int loop_break_lbl[MAX_LOOP_DEPTH];
    int loop_cont_lbl[MAX_LOOP_DEPTH];
    int loop_depth;
} Compiler;

/* ========================================================================= */
/* 5. STANDALONE PACKAGING STRUCTURES                                        */
/* ========================================================================= */

#define VORTEX_PACK_MAGIC_HEADER "VRTX"
#define VORTEX_PACK_MAGIC_FOOTER "VTXPACK1"
#define VORTEX_PACK_VERSION 1
#define VORTEX_PACK_FLAG_RELEASE 1

#pragma pack(push, 1)
typedef struct {
    char header_magic[4];     /* "VRTX" */
    uint32_t version;         /* 1 */
    uint32_t flags;           /* bit 0: release mode */
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
extern VortexBuildMode g_compile_mode;
extern int g_ultrasafe_level;
extern bool g_json_errors;
extern bool g_check_only;
extern bool g_recover_active;
extern jmp_buf g_recover_jmp;
extern VortexDiagnostic g_diags[MAX_DIAGNOSTICS];
extern int g_diag_count;
extern int g_diag_error_count;
extern int g_diag_warn_count;
extern VortexEnumType g_enum_types[MAX_ENUM_TYPES];
extern int g_enum_type_count;
extern VortexTargetArch g_target_arch;
extern VortexTargetABI g_target_abi;
extern const char* g_current_filename;
extern const char* g_dump_jit_path;
extern const char* g_dump_debug_path;
extern bool g_lsp_capture_errors;

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
void arena_register(VortexRegionArena* ar);
void arena_init(void);
void arena_set_thread_local(VortexRegionArena* ar);
void* arena_alloc_fast(size_t bytes);
void* arena_alloc(size_t bytes);
int64_t vortex_alloc_struct(int64_t size);
int64_t vortex_alloc_slice_i64(int64_t len);
int64_t vortex_alloc_slice_f32(int64_t len);
int64_t vortex_alloc_slice_i32(int64_t len);
int64_t vortex_slice_len(int64_t ptr_val);
int64_t vortex_scope_push(void);
int64_t vortex_scope_pop(int64_t mark);

void vortex_safe_check_ptr(int64_t ptr_val, int64_t file_ptr, int64_t line);
void vortex_safe_check_bounds(int64_t ptr_val, int64_t idx, int64_t len_override, int64_t elem_size, int64_t file_ptr, int64_t line);
void vortex_safe_check_range(int64_t ptr_val, int64_t start, int64_t lim, int64_t len_override, int64_t elem_size, int64_t file_ptr, int64_t line);
void vortex_safe_check_ffi_arg(int64_t val, int64_t type_code, int64_t align_and_flags, int64_t fn_name_ptr, int64_t arg_idx, int64_t file_ptr, int64_t line);

int64_t vortex_ptr_read8(int64_t ptr_val);
int64_t vortex_ptr_write8(int64_t ptr_val, int64_t val);
int64_t vortex_ptr_read32(int64_t ptr_val);
int64_t vortex_ptr_write32(int64_t ptr_val, int64_t val);
int64_t vortex_ptr_read64(int64_t ptr_val);
int64_t vortex_ptr_write64(int64_t ptr_val, int64_t val);

int64_t vortex_vec_new(void);
int64_t vortex_vec_push(int64_t vec_ptr, int64_t val);
int64_t vortex_vec_get(int64_t vec_ptr, int64_t idx);
int64_t vortex_vec_set(int64_t vec_ptr, int64_t idx, int64_t val);
int64_t vortex_vec_get_safe(int64_t vec_ptr, int64_t idx, int64_t file_ptr, int64_t line);
int64_t vortex_vec_set_safe(int64_t vec_ptr, int64_t idx, int64_t val, int64_t file_ptr, int64_t line);
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
int64_t vortex_str_byte(int64_t s_ptr, int64_t idx);
int64_t vortex_str_from_byte(int64_t ch);
int64_t vortex_str_eq(int64_t s1_ptr, int64_t s2_ptr);
int64_t vortex_str_find(int64_t haystack_ptr, int64_t needle_ptr, int64_t start_idx);
int64_t vortex_str_to_i64(int64_t s_ptr);
int64_t vortex_ptr_to_str(int64_t ptr_val);
int64_t vortex_str_join(int64_t vec_ptr, int64_t sep_ptr);
int64_t vortex_str_find_any2(int64_t s_ptr, int64_t c1, int64_t c2, int64_t start_idx);
void* arena_alloc_compact(size_t bytes);

/* --- src/lexer.c, src/modules.c, src/parser_expr.c, src/parser_stmt.c --- */
void lexer_init(Lexer* lex, const char* source);
void next_token(Lexer* lex);
VortexStructType* struct_type_find(const char* name);
VortexStructType* struct_type_find_by_id(int id);
VortexField* struct_field_find(VortexStructType* st, const char* field_name);
void struct_type_hash_insert(const char* name, int idx);
VortexType parse_type_name(Lexer* l, int* out_struct_id);
VortexEnumType* enum_type_find(const char* name);
void vortex_lexer_sync(Lexer* l, bool in_function);
int64_t vortex_builtin_struct_size(int64_t name_ptr);
int64_t vortex_builtin_struct_align(int64_t name_ptr);
UserFunc* user_func_find(const char* name);
void user_func_hash_insert(const char* name, int idx);
void vortex_parser_reset_tables(bool reset_structs);
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
void ir_emit_cast(IRBuffer* ir, IROpcode op, VReg dest, VReg src, VortexType t);
void ir_emit_load_offset(IRBuffer* ir, VReg dest, VReg base, int32_t offset, VortexType t);
void ir_emit_store_offset(IRBuffer* ir, VReg base, int32_t offset, VReg val, VortexType t);
void ir_emit_load_array(IRBuffer* ir, VReg dest, VReg base, VReg idx, VortexType t);
void ir_emit_store_array(IRBuffer* ir, VReg base, VReg idx, VReg val, VortexType t);
void ir_emit_load_deref(IRBuffer* ir, VReg dest, VReg ptr_vreg, VortexType t);
void ir_emit_store_deref(IRBuffer* ir, VReg ptr_vreg, VReg val, VortexType t);
void ir_emit_bounds_check(IRBuffer* ir, VReg base, VReg idx, VReg len_vreg, int64_t known_len, int32_t elem_size, const char* file, int line);
void ir_emit_ptr_check(IRBuffer* ir, VReg ptr_vreg, const char* file, int line);
void ir_emit_div_check(IRBuffer* ir, VReg divisor, const char* file, int line);
void ir_emit_ffi_check(IRBuffer* ir, VReg val, VortexType expected_t, int64_t align_and_flags, const char* fn_name, int arg_idx, const char* file, int line);
void ir_emit_label(IRBuffer* ir, int lbl);
void ir_emit_jmp(IRBuffer* ir, int lbl);
void ir_emit_jz(IRBuffer* ir, VReg cond, int lbl);
void ir_emit_call(IRBuffer* ir, VReg dest, void* fn_ptr, VReg* args, int num_args, VortexType ret_t);
void ir_emit_call_user(IRBuffer* ir, VReg dest, const char* name, VReg* args, int num_args, VortexType ret_t);
void ir_emit_call_ffi(IRBuffer* ir, VReg dest, FFIFunc* ff, VReg* args, int num_args, VortexType ret_t);
void ir_emit_load_param(IRBuffer* ir, VReg dest, int param_idx);
void ir_emit_ret(IRBuffer* ir, VReg val, VortexType t);
void ir_region_verify(IRBuffer* ir);
void ir_optimize_bounds_and_licm(IRBuffer* ir);
void ir_liveness_analysis(IRBuffer* ir);
void ir_loop_vectorize(IRBuffer* ir);
void ir_linear_scan_allocate(IRBuffer* ir);
void jit_set_active_ir_frame(const IRBuffer* ir);

/* --- Target Abstraction & JIT Backends (src/jit_x64.c, src/jit_arm64.c) --- */
typedef struct {
    VortexTargetArch arch;
    const char* backend_name;
    int (*compute_frame_size)(int total_spill_bytes);
    void (*emit_prologue)(JITBuffer* j, int frame_size);
    void (*emit_epilogue)(JITBuffer* j, int frame_size);
    void (*emit_func_prologue)(JITBuffer* j, int frame_size);
    void (*emit_func_epilogue)(JITBuffer* j, int frame_size);
    size_t (*emit_jmp_placeholder)(JITBuffer* j);
    void (*patch_jump)(JITBuffer* j, size_t patch_pos);
    void (*patch_call_rel)(JITBuffer* j, size_t patch_pos, size_t target_offset);
    void (*ir_codegen)(IRBuffer* ir, JITBuffer* j);
} VortexJITBackend;

const VortexTargetDesc* vortex_get_target_desc(void);
const VortexTargetDesc* vortex_get_target_desc_for(VortexTargetArch arch, VortexTargetABI abi);
const VortexJITBackend* vortex_get_backend(void);
bool vortex_set_target_name(const char* name);
bool vortex_is_cross_target(void);
const char* vortex_intern_cstr(const char* s);
void vortex_get_self_path(char* out, size_t cap, const char* argv0);

/* Executable page & Apple Silicon / Linux W^X helpers (src/runtime.c) */
void* alloc_exec_page(size_t size);
void free_exec_page(void* ptr, size_t size);
void jit_write_protect(void* ptr, size_t size, bool writable);

extern const VortexJITBackend g_backend_x64;
extern const VortexJITBackend g_backend_arm64;

/* Compatibility & Backend entry points */
size_t emit_jmp_placeholder(JITBuffer* j);
void patch_jump(JITBuffer* j, size_t patch_pos);
int compute_frame_size(int total_spill_bytes);
void emit_prologue(JITBuffer* j, int frame_size);
void emit_epilogue(JITBuffer* j, int frame_size);
void emit_func_prologue(JITBuffer* j, int frame_size);
void emit_func_epilogue(JITBuffer* j, int frame_size);
void ir_codegen_x86_64(IRBuffer* ir, JITBuffer* j);
void ir_codegen_arm64(IRBuffer* ir, JITBuffer* j);

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
int64_t vortex_ffi_call5(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5);
int64_t vortex_ffi_call6(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6);

/* --- src/runtime.c --- */
void vortex_error(const char* msg);
void vortex_compile_error(const char* code, int line, int col, const char* msg, const char* suggestion);
void vortex_record_user_call_site(const char* name, bool called_in_unsafe, const char* file, int line, int col);
void vortex_diag_reset(void);
void vortex_diag_add(VortexDiagSeverity sev, const char* code, const char* file, int line, int col, const char* msg, const char* suggestion);
void vortex_diag_print_all(void);
void vortex_ultrasafe_div_zero(const char* file, int line);
void vortex_ultrasafe_check_recursion(int64_t delta, const char* fn_name, const char* file, int line);
void vortex_ultrasafe_audit_ir(const IRBuffer* ir, const SymbolTable* st, const char* fn_name, VortexType ret_type, int fn_line);
void vortex_ultrasafe_audit_symbols(const SymbolTable* st, int start_idx, int end_idx, const char* fn_name);
void vortex_ultrasafe_audit_program(void);
bool vortex_source_map_lookup(int merged_line, const char** out_file, int* out_line, bool* out_is_stdlib, bool* out_is_imported);
void vortex_source_map_reset(void);
int64_t vortex_assert(int64_t cond);
int64_t vortex_assert_eq(int64_t a, int64_t b);
int vortex_cmd_test(int argc, char** argv);
int vortex_cmd_spec(int argc, char** argv);
void helper_print_i64(int64_t val);
void helper_print_f32(float val);
void helper_print_str(const char* s);
int64_t vortex_math_sin(int64_t deg_x10);
int64_t vortex_math_cos(int64_t deg_x10);
int64_t vortex_math_sqrt(int64_t x);
float vortex_math_sinf(float rad);
float vortex_math_cosf(float rad);
float vortex_math_sqrtf(float x);
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

extern bool g_is_packaged_app;
void vx_str_copy(char* dst, size_t cap, const char* src);
void vortex_wait_on_standalone_error(const char* msg);
bool compile_vortex_source_internal(const char* source, void* exec_mem, JITBuffer* out_jit);
char* vortex_resolve_imports(const char* entry_path);
int execute_vortex_source(const char* source);
int execute_vortex_file(const char* filename);
bool check_and_run_embedded_payload(int argc, char** argv);
int vortex_cmd_build(int argc, char** argv);

/* --- src/debug_info.c (Stacktraces, DWARF / GDB-LLDB JIT, Win64 Unwind) --- */
typedef struct {
    size_t start_offset;
    size_t end_offset;
    int line;
    const char* file;
    const char* func_name;
} VortexDebugLineEntry;

typedef struct {
    char name[64];
    size_t start_offset;
    size_t end_offset;
    int frame_size;
    bool is_main;
    int line;
    const char* file;
} VortexDebugFuncInfo;

void debug_info_reset(void);
void debug_info_set_current_func(const char* name);
void debug_info_record_func(const char* name, size_t start_off, size_t end_off, int frame_size, bool is_main, const char* file, int line);
void debug_info_record_line(size_t start_off, size_t end_off, int line, const char* file);
void debug_info_set_jit_image(void* base, size_t size, JITBuffer* jit_opt);
void debug_info_unregister_jit_image(void);
bool debug_write_dwarf_elf(const char* path);
const VortexDebugLineEntry* debug_info_lookup_addr(uintptr_t addr, const char** out_func_name);
void vortex_print_stacktrace(void* native_ctx);
void vortex_crash_guard_enter(void);
void vortex_crash_guard_leave(void);

/* --- Module imports, bundling & Package / LSP Hooks --- */
typedef bool (*VortexImportHookFn)(const char* spec, const char* importer_file, char* out_path, size_t cap);
extern VortexImportHookFn g_pkg_import_hook;
void lexer_reset_imports(void);
bool vortex_resolve_import(const char* spec, const char* importer_file, char* out_path, size_t cap);
char* vortex_bundle_source(const char* entry_path, const char* entry_src, size_t* out_len);

/* --- src/pkg.c (Package Manager, vortex.toml / vortex.lock, SHA-256, Cache) --- */
void vortex_sha256_hex(const uint8_t* data, size_t len, char out_hex[65]);
bool pkg_resolve_import(const char* spec, const char* importer_file, char* out_path, size_t cap);
int vortex_cmd_pkg(int argc, char** argv);

/* --- src/lsp.c & Diagnostics Capture (Language Server Protocol over stdio) --- */
void vortex_diag_emit(FILE* stream, const char* fmt, ...);
bool vortex_frontend_check(const char* source, const char* file_path, char* diag_out, size_t diag_cap);
void ir_free_all_active(void);
int vortex_cmd_lsp(int argc, char** argv);

extern const char* g_dump_jit_path;
extern bool g_headless_mode;
extern bool g_host_mode;

#ifdef _WIN32
#define VORTEX_API __declspec(dllexport)
#else
#define VORTEX_API __attribute__((visibility("default")))
#endif

/* --- Native Host / JVM FFM & JNI Embedding API --- */
VORTEX_API int vortex_host_init(int release_mode);
VORTEX_API int vortex_host_compile(const char* source);
VORTEX_API int vortex_host_compile_file(const char* filepath);
VORTEX_API void* vortex_host_get_fn(const char* fn_name);
VORTEX_API int64_t vortex_host_call0(void* fn_ptr);
VORTEX_API int64_t vortex_host_call1(void* fn_ptr, int64_t a1);
VORTEX_API int64_t vortex_host_call2(void* fn_ptr, int64_t a1, int64_t a2);
VORTEX_API int64_t vortex_host_call3(void* fn_ptr, int64_t a1, int64_t a2, int64_t a3);
VORTEX_API int64_t vortex_host_call4(void* fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4);
VORTEX_API int64_t vortex_host_scope_push(void);
VORTEX_API int64_t vortex_host_scope_pop(int64_t mark);
VORTEX_API void* vortex_host_arena_alloc(int64_t bytes);
VORTEX_API void vortex_host_arena_reset(void);
VORTEX_API const char* vortex_host_last_error(void);
VORTEX_API void vortex_host_shutdown(void);

bool vortex_load_embedded_module_payload(void* module_handle, char** out_src, uint32_t* out_flags);

#endif /* VORTEX_H */
