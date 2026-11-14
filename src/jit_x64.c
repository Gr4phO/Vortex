/*
 * VORTEX X86-64 DUAL-ABI JIT CODE GENERATOR & DYNAMIC FFI RESOLVER
 * - Windows x64 & Linux System V AMD64 ABI compliant machine code emitter
 * - 16-Way Unrolled AVX2 SIMD Loop Kernel Emitter
 * - Dynamic C Library Loader (LoadLibraryA / dlopen) & FFI Registry
 */

#include "vortex.h"

/* ========================================================================= */
/* DYNAMIC FFI SUBSYSTEM (LoadLibraryA / dlopen)                             */
/* ========================================================================= */

static VortexDynLib g_dyn_libs[MAX_DYN_LIBS];
static int g_dyn_lib_count = 0;

FFIFunc g_ffi_funcs[MAX_FFI_FUNCS];
int g_ffi_func_count = 0;

void* vortex_load_dynlib(const char* lib_name) {
    if (!lib_name || lib_name[0] == '\0') return NULL;

    for (int i = 0; i < g_dyn_lib_count; i++) {
        if (strcmp(g_dyn_libs[i].name, lib_name) == 0) {
            return g_dyn_libs[i].handle;
        }
    }

    void* handle = NULL;
#ifdef _WIN32
    handle = (void*)LoadLibraryA(lib_name);
    if (!handle && !strstr(lib_name, ".dll") && !strstr(lib_name, ".DLL")) {
        char alt_name[140];
        snprintf(alt_name, sizeof(alt_name), "%s.dll", lib_name);
        handle = (void*)LoadLibraryA(alt_name);
    }
#else
    handle = dlopen(lib_name, RTLD_NOW | RTLD_GLOBAL);
#ifdef __APPLE__
    if (!handle && (strcmp(lib_name, "msvcrt") == 0 || strcmp(lib_name, "c") == 0 || strcmp(lib_name, "m") == 0)) {
        handle = dlopen("libSystem.B.dylib", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!handle && !strstr(lib_name, ".dylib")) {
        char alt_name[140];
        if (strncmp(lib_name, "lib", 3) != 0) {
            snprintf(alt_name, sizeof(alt_name), "lib%s.dylib", lib_name);
        } else {
            snprintf(alt_name, sizeof(alt_name), "%s.dylib", lib_name);
        }
        handle = dlopen(alt_name, RTLD_NOW | RTLD_GLOBAL);
    }
#else
    if (!handle && (strcmp(lib_name, "msvcrt") == 0 || strcmp(lib_name, "c") == 0)) {
        handle = dlopen("libc.so.6", RTLD_NOW | RTLD_GLOBAL);
    } else if (!handle && strcmp(lib_name, "m") == 0) {
        handle = dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!handle && !strstr(lib_name, ".so")) {
        char alt_name[140];
        if (strncmp(lib_name, "lib", 3) != 0) {
            snprintf(alt_name, sizeof(alt_name), "lib%s.so", lib_name);
        } else {
            snprintf(alt_name, sizeof(alt_name), "%s.so", lib_name);
        }
        handle = dlopen(alt_name, RTLD_NOW | RTLD_GLOBAL);
    }
#endif
#endif

    if (!handle) return NULL;

    if (g_dyn_lib_count < MAX_DYN_LIBS) {
        strncpy(g_dyn_libs[g_dyn_lib_count].name, lib_name, sizeof(g_dyn_libs[0].name) - 1);
        g_dyn_libs[g_dyn_lib_count].name[sizeof(g_dyn_libs[0].name) - 1] = '\0';
        g_dyn_libs[g_dyn_lib_count].handle = handle;
        g_dyn_lib_count++;
    }
    return handle;
}

void* vortex_resolve_symbol(void* handle, const char* sym_name) {
    if (!handle || !sym_name) return NULL;
#ifdef _WIN32
    void* ptr = (void*)GetProcAddress((HMODULE)handle, sym_name);
    if (!ptr && strcmp(sym_name, "snprintf") == 0) {
        ptr = (void*)GetProcAddress((HMODULE)handle, "_snprintf");
    }
    return ptr;
#else
    return dlsym(handle, sym_name);
#endif
}

FFIFunc* ffi_func_find(const char* name) {
    for (int i = 0; i < g_ffi_func_count; i++) {
        if (strcmp(g_ffi_funcs[i].fn_name, name) == 0) {
            return &g_ffi_funcs[i];
        }
    }
    return NULL;
}

int64_t vortex_ffi_load(int64_t lib_name_ptr) {
    const char* name = (const char*)lib_name_ptr;
#ifdef _WIN32
    HMODULE h = LoadLibraryA(name);
    return (int64_t)h;
#else
    void* h = dlopen(name, RTLD_NOW);
    return (int64_t)h;
#endif
}

int64_t vortex_ffi_symbol(int64_t lib_handle, int64_t sym_name_ptr) {
    const char* sym = (const char*)sym_name_ptr;
#ifdef _WIN32
    FARPROC p = GetProcAddress((HMODULE)lib_handle, sym);
    return (int64_t)p;
#else
    void* p = dlsym((void*)lib_handle, sym);
    return (int64_t)p;
#endif
}

int64_t vortex_ffi_call0(int64_t fn_ptr) {
    typedef int64_t (*fn0_t)(void);
    return ((fn0_t)fn_ptr)();
}

int64_t vortex_ffi_call1(int64_t fn_ptr, int64_t a1) {
    typedef int64_t (*fn1_t)(int64_t);
    return ((fn1_t)fn_ptr)(a1);
}

int64_t vortex_ffi_call2(int64_t fn_ptr, int64_t a1, int64_t a2) {
    typedef int64_t (*fn2_t)(int64_t, int64_t);
    return ((fn2_t)fn_ptr)(a1, a2);
}

int64_t vortex_ffi_call3(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3) {
    typedef int64_t (*fn3_t)(int64_t, int64_t, int64_t);
    return ((fn3_t)fn_ptr)(a1, a2, a3);
}

int64_t vortex_ffi_call4(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    typedef int64_t (*fn4_t)(int64_t, int64_t, int64_t, int64_t);
    return ((fn4_t)fn_ptr)(a1, a2, a3, a4);
}

int64_t vortex_ffi_call5(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5) {
    typedef int64_t (*fn5_t)(int64_t, int64_t, int64_t, int64_t, int64_t);
    return ((fn5_t)fn_ptr)(a1, a2, a3, a4, a5);
}

int64_t vortex_ffi_call6(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6) {
    typedef int64_t (*fn6_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
    return ((fn6_t)fn_ptr)(a1, a2, a3, a4, a5, a6);
}

/* ========================================================================= */
/* LOW-LEVEL X86-64 / AVX2 EMITTER                                           */
/* ========================================================================= */

static void emit_u8(JITBuffer* j, uint8_t b) {
    if (j->size >= j->capacity) {
        fprintf(stderr, "[VORTEX ERROR] JIT Buffer overflow\n");
        vortex_error("");
    }
    j->code[j->size++] = b;
}

static void emit_u32(JITBuffer* j, uint32_t val) {
    emit_u8(j, (uint8_t)(val & 0xFF));
    emit_u8(j, (uint8_t)((val >> 8) & 0xFF));
    emit_u8(j, (uint8_t)((val >> 16) & 0xFF));
    emit_u8(j, (uint8_t)((val >> 24) & 0xFF));
}

static void emit_u64(JITBuffer* j, uint64_t val) {
    emit_u32(j, (uint32_t)(val & 0xFFFFFFFF));
    emit_u32(j, (uint32_t)((val >> 32) & 0xFFFFFFFF));
}

size_t emit_jmp_placeholder(JITBuffer* j) {
    emit_u8(j, 0xE9);
    size_t patch = j->size;
    emit_u32(j, 0);
    return patch;
}

static size_t emit_jcc_placeholder(JITBuffer* j, uint8_t cond_byte) {
    emit_u8(j, 0x0F);
    emit_u8(j, cond_byte);
    size_t patch = j->size;
    emit_u32(j, 0);
    return patch;
}

void patch_jump(JITBuffer* j, size_t patch_pos) {
    int32_t rel = (int32_t)(j->size - (patch_pos + 4));
    memcpy(&j->code[patch_pos], &rel, 4);
}

int compute_frame_size(int total_spill_bytes) {
    int size = 128 + total_spill_bytes + 64;
    if ((size % 16) != 8) {
        size = ((size + 15) & ~15) + 8;
    }
    return size;
}

void emit_prologue(JITBuffer* j, int frame_size) {
    emit_u8(j, 0x55);                                     /* push rbp */
    emit_u8(j, 0x48); emit_u8(j, 0x89); emit_u8(j, 0xE5); /* mov rbp, rsp */
    emit_u8(j, 0x53); emit_u8(j, 0x56); emit_u8(j, 0x57); /* push rbx, rsi, rdi */
    emit_u8(j, 0x41); emit_u8(j, 0x54); emit_u8(j, 0x41); emit_u8(j, 0x55); /* push r12, r13 */
    emit_u8(j, 0x41); emit_u8(j, 0x56); emit_u8(j, 0x41); emit_u8(j, 0x57); /* push r14, r15 */
    emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xEC); /* sub rsp, frame_size */
    emit_u32(j, (uint32_t)frame_size);
}

void emit_epilogue(JITBuffer* j, int frame_size) {
    emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); /* add rsp, frame_size */
    emit_u32(j, (uint32_t)frame_size);
    emit_u8(j, 0x41); emit_u8(j, 0x5F); emit_u8(j, 0x41); emit_u8(j, 0x5E); /* pop r15, r14 */
    emit_u8(j, 0x41); emit_u8(j, 0x5D); emit_u8(j, 0x41); emit_u8(j, 0x5C); /* pop r13, r12 */
    emit_u8(j, 0x5F); emit_u8(j, 0x5E); emit_u8(j, 0x5B); emit_u8(j, 0x5D); /* pop rdi, rsi, rbx, rbp */
    emit_u8(j, 0xC3);                                     /* ret */
}

static void emit_store_mem_offset(JITBuffer* j, int base, int32_t offset, int src);

void emit_func_prologue(JITBuffer* j, int frame_size) {
    emit_u8(j, 0x55);                                     /* push rbp */
    emit_u8(j, 0x48); emit_u8(j, 0x89); emit_u8(j, 0xE5); /* mov rbp, rsp */
    if (g_target_abi != VORTEX_ABI_SYSV64) {
        /* Home Win64 argument registers into caller shadow space */
        emit_u8(j, 0x48); emit_u8(j, 0x89); emit_u8(j, 0x4D); emit_u8(j, 0x10); /* mov [rbp+16], rcx */
        emit_u8(j, 0x48); emit_u8(j, 0x89); emit_u8(j, 0x55); emit_u8(j, 0x18); /* mov [rbp+24], rdx */
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x45); emit_u8(j, 0x20); /* mov [rbp+32], r8 */
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x4D); emit_u8(j, 0x28); /* mov [rbp+40], r9 */
    }
    emit_u8(j, 0x53); emit_u8(j, 0x56); emit_u8(j, 0x57); /* push rbx, rsi, rdi */
    emit_u8(j, 0x41); emit_u8(j, 0x54); emit_u8(j, 0x41); emit_u8(j, 0x55); /* push r12, r13 */
    emit_u8(j, 0x41); emit_u8(j, 0x56); emit_u8(j, 0x41); emit_u8(j, 0x57); /* push r14, r15 */
    emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xEC); /* sub rsp, frame_size */
    emit_u32(j, (uint32_t)frame_size);
    if (g_target_abi == VORTEX_ABI_SYSV64) {
        /* Home SysV64 argument registers (rdi..r9) into private frame home area [rbp-64..-104] */
        emit_store_mem_offset(j, PREG_RBP, -64,  PREG_RDI);
        emit_store_mem_offset(j, PREG_RBP, -72,  PREG_RSI);
        emit_store_mem_offset(j, PREG_RBP, -80,  PREG_RDX);
        emit_store_mem_offset(j, PREG_RBP, -88,  PREG_RCX);
        emit_store_mem_offset(j, PREG_RBP, -96,  PREG_R8);
        emit_store_mem_offset(j, PREG_RBP, -104, PREG_R9);
    }
}

void emit_func_epilogue(JITBuffer* j, int frame_size) {
    emit_epilogue(j, frame_size);
}

/* --- AVX2 SIMD TABLES & EMITTERS --- */
static const float g_simd_lane_offsets_A[8] __attribute__((aligned(32))) = {
    0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
};
static const float g_simd_lane_offsets_B[8] __attribute__((aligned(32))) = {
    8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f
};
static const float g_simd_sixteen[8] __attribute__((aligned(32))) = {
    16.0f, 16.0f, 16.0f, 16.0f, 16.0f, 16.0f, 16.0f, 16.0f
};

static inline void emit_vzeroupper(JITBuffer* j) {
    emit_u8(j, 0xC5); emit_u8(j, 0xF8); emit_u8(j, 0x77);
}

static void emit_vex_ymm(JITBuffer* j, uint8_t opcode, int dest, int src1, int src2) {
    int r_bit = (dest < 8) ? 1 : 0;
    int b_bit = (src2 < 8) ? 1 : 0;
    int v_bits = (~src1) & 0x0F;
    if (src2 < 8) {
        uint8_t b1 = (uint8_t)((r_bit << 7) | (v_bits << 3) | 0x04);
        uint8_t modrm = (uint8_t)(0xC0 | ((dest & 7) << 3) | (src2 & 7));
        emit_u8(j, 0xC5); emit_u8(j, b1); emit_u8(j, opcode); emit_u8(j, modrm);
    } else {
        uint8_t b1 = (uint8_t)((r_bit << 7) | 0x40 | (b_bit << 5) | 0x01);
        uint8_t b2 = (uint8_t)((v_bits << 3) | 0x04);
        uint8_t modrm = (uint8_t)(0xC0 | ((dest & 7) << 3) | (src2 & 7));
        emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, b2); emit_u8(j, opcode); emit_u8(j, modrm);
    }
}

static void emit_vbroadcastss_yx(JITBuffer* j, int dest, int src) {
    int r_bit = (dest < 8) ? 1 : 0;
    int b_bit = (src < 8) ? 1 : 0;
    uint8_t b1 = (uint8_t)((r_bit << 7) | 0x40 | (b_bit << 5) | 0x02);
    uint8_t modrm = (uint8_t)(0xC0 | ((dest & 7) << 3) | (src & 7));
    emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, 0x7D); emit_u8(j, 0x18); emit_u8(j, modrm);
}

static void emit_vmovups_ymm_mem(JITBuffer* j, int dest, int base_reg) {
    int r_bit = (dest < 8) ? 1 : 0;
    int b_bit = (base_reg < 8) ? 1 : 0;
    uint8_t b1 = (uint8_t)((r_bit << 7) | 0x40 | (b_bit << 5) | 0x01);
    uint8_t modrm = (uint8_t)(0x00 | ((dest & 7) << 3) | (base_reg & 7));
    emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, 0x7C); emit_u8(j, 0x10); emit_u8(j, modrm);
}

static void emit_avx2_reduce_ymm0_ymm1(JITBuffer* j) {
    emit_u8(j, 0xC5); emit_u8(j, 0xFC); emit_u8(j, 0x58); emit_u8(j, 0xC1);
    emit_u8(j, 0xC4); emit_u8(j, 0xE3); emit_u8(j, 0x7D); emit_u8(j, 0x19); emit_u8(j, 0xC1); emit_u8(j, 0x01);
    emit_u8(j, 0xC5); emit_u8(j, 0xF8); emit_u8(j, 0x58); emit_u8(j, 0xC1);
    emit_u8(j, 0xC5); emit_u8(j, 0xFB); emit_u8(j, 0x7C); emit_u8(j, 0xC0);
    emit_u8(j, 0xC5); emit_u8(j, 0xFB); emit_u8(j, 0x7C); emit_u8(j, 0xC0);
    emit_vzeroupper(j);
}

static void emit_push_r(JITBuffer* j, int r) {
    if (r < 8) emit_u8(j, (uint8_t)(0x50 + r));
    else { emit_u8(j, 0x41); emit_u8(j, (uint8_t)(0x50 + (r & 7))); }
}

static void emit_mov_rr(JITBuffer* j, int dst, int src) {
    if (dst == src) return;
    uint8_t rex = 0x48;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x89);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

static void emit_add_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x01);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

static void emit_sub_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x29);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

static void emit_imul_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48;
    if (dst >= 8) rex |= 0x04;
    if (src >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0xAF);
    emit_u8(j, (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

static void emit_idiv_r(JITBuffer* j, int divisor) {
    if (divisor >= 8) {
        emit_u8(j, 0x49); emit_u8(j, 0xF7); emit_u8(j, (uint8_t)(0xF8 | (divisor & 7)));
    } else {
        emit_u8(j, 0x48); emit_u8(j, 0xF7); emit_u8(j, (uint8_t)(0xF8 | (divisor & 7)));
    }
}

static void emit_cmp_rr(JITBuffer* j, int r1, int r2) {
    uint8_t rex = 0x48;
    if (r2 >= 8) rex |= 0x04;
    if (r1 >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x39);
    emit_u8(j, (uint8_t)(0xC0 | ((r2 & 7) << 3) | (r1 & 7)));
}

static void emit_test_rr(JITBuffer* j, int r1, int r2) {
    uint8_t rex = 0x48;
    if (r2 >= 8) rex |= 0x04;
    if (r1 >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x85);
    emit_u8(j, (uint8_t)(0xC0 | ((r2 & 7) << 3) | (r1 & 7)));
}

static void emit_setcc(JITBuffer* j, uint8_t cond_byte, int dst_reg8) {
    (void)dst_reg8;
    emit_u8(j, 0x0F);
    emit_u8(j, cond_byte);
    emit_u8(j, 0xC0); /* AL */
}

static void emit_movzx_r64_r8(JITBuffer* j, int dst, int src8) {
    (void)src8;
    uint8_t rex = 0x48 | (dst >= 8 ? 4 : 0);
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0xB6);
    emit_u8(j, (uint8_t)(0xC0 | ((dst & 7) << 3) | 0));
}

static void emit_mov_ri64(JITBuffer* j, int reg, int64_t val) {
    uint8_t rex = 0x48 | (reg >= 8 ? 1 : 0);
    emit_u8(j, rex);
    emit_u8(j, (uint8_t)(0xB8 | (reg & 7)));
    emit_u64(j, (uint64_t)val);
}

static void emit_load_spill(JITBuffer* j, int reg, int spill_offset) {
    uint8_t rex = 0x48 | (reg >= 8 ? 4 : 0);
    emit_u8(j, rex);
    emit_u8(j, 0x8B);
    emit_u8(j, (uint8_t)(0x80 | ((reg & 7) << 3) | 5));
    emit_u32(j, (uint32_t)(-spill_offset));
}

static void emit_store_spill(JITBuffer* j, int reg, int spill_offset) {
    uint8_t rex = 0x48 | (reg >= 8 ? 4 : 0);
    emit_u8(j, rex);
    emit_u8(j, 0x89);
    emit_u8(j, (uint8_t)(0x80 | ((reg & 7) << 3) | 5));
    emit_u32(j, (uint32_t)(-spill_offset));
}

static void emit_loadss_spill(JITBuffer* j, int xmm_reg, int spill_offset) {
    emit_u8(j, 0xF3);
    if (xmm_reg >= 8) emit_u8(j, 0x44);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    emit_u8(j, (uint8_t)(0x80 | ((xmm_reg & 7) << 3) | 5));
    emit_u32(j, (uint32_t)(-spill_offset));
}

static void emit_storess_spill(JITBuffer* j, int xmm_reg, int spill_offset) {
    emit_u8(j, 0xF3);
    if (xmm_reg >= 8) emit_u8(j, 0x44);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    emit_u8(j, (uint8_t)(0x80 | ((xmm_reg & 7) << 3) | 5));
    emit_u32(j, (uint32_t)(-spill_offset));
}

static void emit_load_mem_offset(JITBuffer* j, int dst, int base, int32_t offset) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x8B);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((dst & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((dst & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_store_mem_offset(JITBuffer* j, int base, int32_t offset, int src) {
    uint8_t rex = 0x48 | ((src >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x89);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((src & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((src & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_loadss_offset(JITBuffer* j, int dst_xmm, int base, int32_t offset) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x40 | ((dst_xmm >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((dst_xmm & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((dst_xmm & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_loadsd_offset(JITBuffer* j, int dst_xmm, int base, int32_t offset) {
    emit_u8(j, 0xF2);
    uint8_t rex = 0x40 | ((dst_xmm >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((dst_xmm & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((dst_xmm & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_storess_offset(JITBuffer* j, int base, int32_t offset, int src_xmm) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x40 | ((src_xmm >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((src_xmm & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((src_xmm & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_load_mem32s_offset(JITBuffer* j, int dst, int base, int32_t offset) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x63); /* movsxd r64, dword [base + offset] */
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((dst & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((dst & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_store_mem32_offset(JITBuffer* j, int base, int32_t offset, int src) {
    uint8_t rex = 0x40 | ((src >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x89);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((src & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((src & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_load_mem32s_array(JITBuffer* j, int dst, int base, int idx, int scale) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x63);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((dst & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_store_mem32_array(JITBuffer* j, int base, int idx, int src, int scale) {
    uint8_t rex = 0x40 | ((src >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x89);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((src & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_cvtsi2ss_xr(JITBuffer* j, int dst_xmm, int src_gpr) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x48 | ((dst_xmm >= 8 ? 4 : 0) | (src_gpr >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2A);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_gpr & 7)));
}

static void emit_cvttss2si_rx(JITBuffer* j, int dst_gpr, int src_xmm) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x48 | ((dst_gpr >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2C);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_gpr & 7) << 3) | (src_xmm & 7)));
}

static void emit_sub_rsp(JITBuffer* j, int32_t bytes) {
    if (bytes <= 0) return;
    if (bytes <= 127) {
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, (uint8_t)bytes);
    } else {
        emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xEC); emit_u32(j, (uint32_t)bytes);
    }
}

static void emit_load_mem_array(JITBuffer* j, int dst, int base, int idx, int scale) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x8B);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((dst & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_store_mem_array(JITBuffer* j, int base, int idx, int src, int scale) {
    uint8_t rex = 0x48 | ((src >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x89);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((src & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_loadss_array(JITBuffer* j, int dst_xmm, int base, int idx, int scale) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x40 | ((dst_xmm >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((dst_xmm & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_storess_array(JITBuffer* j, int base, int idx, int src_xmm, int scale) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x40 | ((src_xmm >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((src_xmm & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_movaps_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    if (dst_xmm == src_xmm) return;
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x28);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_addss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x58);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_subss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x5C);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_mulss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x59);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_divss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x5E);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_ucomiss_xx(JITBuffer* j, int x1, int x2) {
    if (x1 >= 8 || x2 >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (x1 >= 8 ? 4 : 0) | (x2 >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2E);
    emit_u8(j, (uint8_t)(0xC0 | ((x1 & 7) << 3) | (x2 & 7)));
}

static void emit_load_f32_imm_to(JITBuffer* j, float val, int xmm_reg) {
    uint32_t bits;
    memcpy(&bits, &val, 4);
    emit_u8(j, 0x41); emit_u8(j, 0xBA); /* mov r10d, bits */
    emit_u32(j, bits);
    emit_u8(j, 0x66);
    uint8_t rex = 0x40 | (xmm_reg >= 8 ? 4 : 0) | 1; /* 1 for R10 */
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x6E);
    uint8_t modrm = (uint8_t)(0xC0 | ((xmm_reg & 7) << 3) | 2); /* 2 = R10 & 7 */
    emit_u8(j, modrm);
}

#ifdef _WIN32
static void emit_call_with_args(JITBuffer* j, void* func_ptr, int num_args) {
    if (num_args == 0) {
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 1) {
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 2) {
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 3) {
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 4) {
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 5) {
        emit_u8(j, 0x41); emit_u8(j, 0x5A);
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x30);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x54); emit_u8(j, 0x24); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x30);
        return;
    }
    if (num_args == 6) {
        emit_u8(j, 0x41); emit_u8(j, 0x5B);
        emit_u8(j, 0x41); emit_u8(j, 0x5A);
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x30);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x54); emit_u8(j, 0x24); emit_u8(j, 0x20);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x5C); emit_u8(j, 0x24); emit_u8(j, 0x28);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x30);
        return;
    }
    if (num_args == 7) {
        emit_u8(j, 0x41); emit_u8(j, 0x5A);
        emit_u8(j, 0x41); emit_u8(j, 0x5B);
        emit_u8(j, 0x58);
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x40);
        emit_u8(j, 0x48); emit_u8(j, 0x89); emit_u8(j, 0x44); emit_u8(j, 0x24); emit_u8(j, 0x20);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x5C); emit_u8(j, 0x24); emit_u8(j, 0x28);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x54); emit_u8(j, 0x24); emit_u8(j, 0x30);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x40);
        return;
    }
}
#else
static void emit_call_with_args(JITBuffer* j, void* func_ptr, int num_args) {
    if (num_args == 0) {
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 1) {
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 2) {
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 3) {
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 4) {
        emit_u8(j, 0x59);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 5) {
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x59);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 6) {
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x59);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 7) {
        emit_u8(j, 0x41); emit_u8(j, 0x5A);
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x59);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x6A); emit_u8(j, 0x00);
        emit_u8(j, 0x41); emit_u8(j, 0x52);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x10);
        return;
    }
}
#endif

static int get_vreg_int_reg(IRBuffer* ir, VReg v, int scratch_reg, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return PREG_NONE;
    if (ir->vregs[v].is_spilled) {
        emit_load_spill(j, scratch_reg, ir->vregs[v].spill_offset);
        return scratch_reg;
    }
    return ir->vregs[v].preg;
}

static void commit_vreg_int_reg(IRBuffer* ir, VReg v, int source_reg, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    if (ir->vregs[v].is_spilled) {
        emit_store_spill(j, source_reg, ir->vregs[v].spill_offset);
    } else {
        int target_reg = ir->vregs[v].preg;
        if (target_reg != source_reg && target_reg != PREG_NONE) {
            emit_mov_rr(j, target_reg, source_reg);
        }
    }
}

static int get_vreg_float_reg(IRBuffer* ir, VReg v, int scratch_xmm, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return 0;
    if (ir->vregs[v].is_spilled) {
        emit_loadss_spill(j, scratch_xmm, ir->vregs[v].spill_offset);
        return scratch_xmm;
    }
    return ir->vregs[v].preg;
}

static void commit_vreg_float_reg(IRBuffer* ir, VReg v, int source_xmm, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    if (ir->vregs[v].is_spilled) {
        emit_storess_spill(j, source_xmm, ir->vregs[v].spill_offset);
    } else {
        int target_xmm = ir->vregs[v].preg;
        if (target_xmm != source_xmm && target_xmm != PREG_NONE) {
            emit_movaps_xx(j, target_xmm, source_xmm);
        }
    }
}

static void emit_avx2_vector_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins) {
    int i_reg = get_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
    if (i_reg != PREG_RAX) emit_mov_rr(j, PREG_RAX, i_reg);

    int lim_reg = get_vreg_int_reg(ir, ins->lim_vreg, PREG_RCX, j);
    if (lim_reg != PREG_RCX) emit_mov_rr(j, PREG_RCX, lim_reg);

    int acc_reg = get_vreg_float_reg(ir, ins->acc_vreg, 0, j);
    if (acc_reg != 0) emit_movaps_xx(j, 0, acc_reg);

    int x_reg = get_vreg_float_reg(ir, ins->x_vreg, 4, j);
    if (x_reg != 4) emit_movaps_xx(j, 4, x_reg);

    int step_reg = get_vreg_float_reg(ir, ins->step_vreg, 5, j);
    if (step_reg != 5) emit_movaps_xx(j, 5, step_reg);

    emit_vbroadcastss_yx(j, 4, 5);
    emit_vbroadcastss_yx(j, 6, 4);

    emit_u8(j, 0xC5); emit_u8(j, 0xFC); emit_u8(j, 0x57); emit_u8(j, 0xC0);
    emit_u8(j, 0xC5); emit_u8(j, 0xF4); emit_u8(j, 0x57); emit_u8(j, 0xC9);

    emit_mov_ri64(j, PREG_R10, (int64_t)g_simd_lane_offsets_A);
    emit_mov_ri64(j, PREG_R11, (int64_t)g_simd_lane_offsets_B);
    emit_vmovups_ymm_mem(j, 2, PREG_R10);
    emit_vmovups_ymm_mem(j, 3, PREG_R11);

    emit_vex_ymm(j, 0x59, 2, 4, 2);
    emit_vex_ymm(j, 0x59, 3, 4, 3);

    emit_vex_ymm(j, 0x58, 8, 6, 2);
    emit_vex_ymm(j, 0x58, 9, 6, 3);

    emit_mov_ri64(j, PREG_R10, (int64_t)g_simd_sixteen);
    emit_vmovups_ymm_mem(j, 7, PREG_R10);
    emit_vex_ymm(j, 0x59, 7, 4, 7);

    float const1_val = (ins->num_val != 0.0f) ? ins->num_val : 4.0f;
    float const2_val = (ins->den_base != 0.0f) ? ins->den_base : 1.0f;
    for (int b = 0; b < ins->body_op_count; b++) {
        if (ins->body_ops[b].op == IR_CONST_F32) {
            if (b == 0) const1_val = ins->body_ops[b].fimm;
            else const2_val = ins->body_ops[b].fimm;
        }
    }
    emit_load_f32_imm_to(j, const1_val, 6);
    emit_vbroadcastss_yx(j, 14, 6);

    emit_load_f32_imm_to(j, const2_val, 6);
    emit_vbroadcastss_yx(j, 15, 6);

    emit_mov_rr(j, PREG_R11, PREG_RAX);
    emit_mov_rr(j, PREG_R10, PREG_RCX);
    emit_u8(j, 0x49); emit_u8(j, 0x83); emit_u8(j, 0xE2); emit_u8(j, 0xF0);

    size_t loop_start = j->size;
    emit_u8(j, 0x4C); emit_u8(j, 0x39); emit_u8(j, 0xD0);
    size_t exit_unroll = emit_jcc_placeholder(j, 0x8D);

    if (ins->body_op_count > 0 && ins->num_val == 0.0f && ins->den_base == 0.0f) {
        for (int b = 0; b < ins->body_op_count; b++) {
            IRInstruction* bo = &ins->body_ops[b];
            if (bo->op == IR_CONST_F32) continue;
            int yd = 10 + (b % 4);
            int ys1 = (bo->src1 == ins->x_vreg) ? 8 : (10 + ((b - 1 + 4) % 4));
            int ys2 = (bo->src2 == ins->x_vreg) ? 8 : (10 + ((b - 2 + 4) % 4));
            uint8_t opc = 0x58;
            if (bo->op == IR_FMUL) opc = 0x59;
            else if (bo->op == IR_FSUB) opc = 0x5C;
            else if (bo->op == IR_FDIV) opc = 0x5E;
            emit_vex_ymm(j, opc, yd, ys1, ys2);
            if (bo->dest == ins->term_vreg || b == ins->body_op_count - 1) {
                emit_vex_ymm(j, 0x58, 0, 0, yd);
            }
        }
        for (int b = 0; b < ins->body_op_count; b++) {
            IRInstruction* bo = &ins->body_ops[b];
            if (bo->op == IR_CONST_F32) continue;
            int yd = 10 + (b % 4);
            int ys1 = (bo->src1 == ins->x_vreg) ? 9 : (10 + ((b - 1 + 4) % 4));
            int ys2 = (bo->src2 == ins->x_vreg) ? 9 : (10 + ((b - 2 + 4) % 4));
            uint8_t opc = 0x58;
            if (bo->op == IR_FMUL) opc = 0x59;
            else if (bo->op == IR_FSUB) opc = 0x5C;
            else if (bo->op == IR_FDIV) opc = 0x5E;
            emit_vex_ymm(j, opc, yd, ys1, ys2);
            if (bo->dest == ins->term_vreg || b == ins->body_op_count - 1) {
                emit_vex_ymm(j, 0x58, 1, 1, yd);
            }
        }
    } else {
        emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x3C); emit_u8(j, 0x59); emit_u8(j, 0xD0);
        emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x04); emit_u8(j, 0x58); emit_u8(j, 0xDA);
        emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x0C); emit_u8(j, 0x5E); emit_u8(j, 0xE3);
        emit_u8(j, 0xC5); emit_u8(j, 0x9C); emit_u8(j, 0x58); emit_u8(j, 0xC0);

        emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x34); emit_u8(j, 0x59); emit_u8(j, 0xD1);
        emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x04); emit_u8(j, 0x58); emit_u8(j, 0xDA);
        emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x0C); emit_u8(j, 0x5E); emit_u8(j, 0xE3);
        emit_u8(j, 0xC5); emit_u8(j, 0x9C); emit_u8(j, 0x58); emit_u8(j, 0xC9);
    }

    emit_u8(j, 0xC5); emit_u8(j, 0x3C); emit_u8(j, 0x58); emit_u8(j, 0xC7);
    emit_u8(j, 0xC5); emit_u8(j, 0x34); emit_u8(j, 0x58); emit_u8(j, 0xCF);
    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC0); emit_u8(j, 0x10);

    size_t back_jmp = emit_jmp_placeholder(j);
    int32_t back_rel = (int32_t)(loop_start - (back_jmp + 4));
    memcpy(&j->code[back_jmp], &back_rel, 4);

    patch_jump(j, exit_unroll);

    emit_avx2_reduce_ymm0_ymm1(j);

    emit_mov_rr(j, PREG_R10, PREG_RAX);
    emit_u8(j, 0x4D); emit_u8(j, 0x29); emit_u8(j, 0xDA);
    emit_u8(j, 0xF3); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x2A); emit_u8(j, 0xCA);
    emit_u8(j, 0xF3); emit_u8(j, 0x0F); emit_u8(j, 0x59); emit_u8(j, 0xCD);
    emit_u8(j, 0xF3); emit_u8(j, 0x0F); emit_u8(j, 0x58); emit_u8(j, 0xE1);

    emit_load_f32_imm_to(j, const1_val, 7);
    emit_load_f32_imm_to(j, const2_val, 6);

    size_t tail_start = j->size;
    emit_u8(j, 0x48); emit_u8(j, 0x39); emit_u8(j, 0xC8);
    size_t tail_exit = emit_jcc_placeholder(j, 0x8D);

    emit_u8(j, 0x44); emit_u8(j, 0x0F); emit_u8(j, 0x28); emit_u8(j, 0xC4);
    emit_u8(j, 0x45); emit_u8(j, 0x0F); emit_u8(j, 0x59); emit_u8(j, 0xC0);
    emit_u8(j, 0x44); emit_u8(j, 0x0F); emit_u8(j, 0x58); emit_u8(j, 0xC6);
    emit_u8(j, 0x44); emit_u8(j, 0x0F); emit_u8(j, 0x28); emit_u8(j, 0xCF);
    emit_u8(j, 0x45); emit_u8(j, 0x0F); emit_u8(j, 0x5E); emit_u8(j, 0xC8);
    emit_u8(j, 0x41); emit_u8(j, 0x0F); emit_u8(j, 0x58); emit_u8(j, 0xC1);
    emit_u8(j, 0x0F); emit_u8(j, 0x58); emit_u8(j, 0xE5);
    emit_u8(j, 0x48); emit_u8(j, 0xFF); emit_u8(j, 0xC0);

    size_t tail_back = emit_jmp_placeholder(j);
    int32_t tail_rel = (int32_t)(tail_start - (tail_back + 4));
    memcpy(&j->code[tail_back], &tail_rel, 4);

    patch_jump(j, tail_exit);

    commit_vreg_float_reg(ir, ins->acc_vreg, 0, j);
    commit_vreg_float_reg(ir, ins->x_vreg, 4, j);
    commit_vreg_float_reg(ir, ins->step_vreg, 5, j);
    commit_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
    emit_vzeroupper(j);
}

typedef struct {
    int label_id;
    size_t patch_pos;
} JumpPatch;

void ir_codegen_x86_64(IRBuffer* ir, JITBuffer* j) {
    int* label_offsets = (int*)malloc(MAX_LABELS * sizeof(int));
    JumpPatch* patches = (JumpPatch*)malloc(MAX_IR_INSTRS * sizeof(JumpPatch));
    if (!label_offsets || !patches) {
        if (label_offsets) free(label_offsets);
        if (patches) free(patches);
        return;
    }
    for (int i = 0; i < MAX_LABELS; i++) label_offsets[i] = -1;
    int patch_count = 0;

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        size_t ins_start_off = j->size;
        switch (ins->op) {
            case IR_NOP: break;

            case IR_CONST_I64: {
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_mov_ri64(j, rd, ins->imm);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_CONST_F32: {
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                emit_load_f32_imm_to(j, ins->fimm, xd);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_MOV: {
                if (ir->vregs[ins->dest].is_float) {
                    int xs = get_vreg_float_reg(ir, ins->src1, 4, j);
                    commit_vreg_float_reg(ir, ins->dest, xs, j);
                } else {
                    int rs = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                    commit_vreg_int_reg(ir, ins->dest, rs, j);
                }
                break;
            }

            case IR_ADD:
            case IR_SUB:
            case IR_MUL: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int r2 = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                if (ins->op == IR_ADD) {
                    if (rd == r1) {
                        emit_add_rr(j, rd, r2);
                    } else if (rd == r2) {
                        emit_add_rr(j, rd, r1);
                    } else {
                        emit_mov_rr(j, rd, r1);
                        emit_add_rr(j, rd, r2);
                    }
                } else if (ins->op == IR_SUB) {
                    if (rd == r1) {
                        emit_sub_rr(j, rd, r2);
                    } else {
                        emit_mov_rr(j, rd, r1);
                        emit_sub_rr(j, rd, r2);
                    }
                } else {
                    if (rd == r1) {
                        emit_imul_rr(j, rd, r2);
                    } else if (rd == r2) {
                        emit_imul_rr(j, rd, r1);
                    } else {
                        emit_mov_rr(j, rd, r1);
                        emit_imul_rr(j, rd, r2);
                    }
                }
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_DIV:
            case IR_MOD: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int r2 = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                if (r2 == PREG_RAX || r2 == PREG_RDX) {
                    emit_mov_rr(j, PREG_R11, r2);
                    r2 = PREG_R11;
                }
                emit_mov_rr(j, PREG_RAX, r1);
                emit_u8(j, 0x48); emit_u8(j, 0x99);
                emit_idiv_r(j, r2);
                if (ins->op == IR_DIV) {
                    commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                } else {
                    commit_vreg_int_reg(ir, ins->dest, PREG_RDX, j);
                }
                break;
            }

            case IR_FADD:
            case IR_FSUB:
            case IR_FMUL:
            case IR_FDIV: {
                int x1 = get_vreg_float_reg(ir, ins->src1, 4, j);
                int x2 = get_vreg_float_reg(ir, ins->src2, 5, j);
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                if (xd != x1) {
                    emit_movaps_xx(j, xd, x1);
                }
                if (ins->op == IR_FADD) emit_addss_xx(j, xd, x2);
                else if (ins->op == IR_FSUB) emit_subss_xx(j, xd, x2);
                else if (ins->op == IR_FMUL) emit_mulss_xx(j, xd, x2);
                else if (ins->op == IR_FDIV) emit_divss_xx(j, xd, x2);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_EQ:
            case IR_NE:
            case IR_LT:
            case IR_LE:
            case IR_GT:
            case IR_GE: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int r2 = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                emit_cmp_rr(j, r1, r2);
                uint8_t cc = 0x94;
                switch (ins->op) {
                    case IR_EQ: cc = 0x94; break;
                    case IR_NE: cc = 0x95; break;
                    case IR_LT: cc = 0x9C; break;
                    case IR_LE: cc = 0x9E; break;
                    case IR_GT: cc = 0x9F; break;
                    case IR_GE: cc = 0x9D; break;
                    default: break;
                }
                emit_setcc(j, cc, PREG_RAX);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_movzx_r64_r8(j, rd, PREG_RAX);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_FEQ:
            case IR_FNE:
            case IR_FLT:
            case IR_FLE:
            case IR_FGT:
            case IR_FGE: {
                int x1 = get_vreg_float_reg(ir, ins->src1, 4, j);
                int x2 = get_vreg_float_reg(ir, ins->src2, 5, j);
                emit_ucomiss_xx(j, x1, x2);
                uint8_t cc = 0x94;
                switch (ins->op) {
                    case IR_FEQ: cc = 0x94; break;
                    case IR_FNE: cc = 0x95; break;
                    case IR_FLT: cc = 0x92; break;
                    case IR_FLE: cc = 0x96; break;
                    case IR_FGT: cc = 0x97; break;
                    case IR_FGE: cc = 0x93; break;
                    default: break;
                }
                emit_setcc(j, cc, PREG_RAX);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_movzx_r64_r8(j, rd, PREG_RAX);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_CAST_I64_TO_F32: {
                int rs = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                emit_cvtsi2ss_xr(j, xd, rs);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_CAST_F32_TO_I64: {
                int xs = get_vreg_float_reg(ir, ins->src1, 4, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_cvttss2si_rx(j, rd, xs);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_LOAD_OFFSET: {
                int r_base = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->type == TYPE_F32) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadss_offset(j, xd, r_base, ins->offset);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem32s_offset(j, rd, r_base, ins->offset);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem_offset(j, rd, r_base, ins->offset);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_OFFSET: {
                int r_base = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->type == TYPE_F32) {
                    int xs = get_vreg_float_reg(ir, ins->src2, 4, j);
                    emit_storess_offset(j, r_base, ins->offset, xs);
                } else if (ins->type == TYPE_I32) {
                    int rs = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                    emit_store_mem32_offset(j, r_base, ins->offset, rs);
                } else {
                    int rs = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                    emit_store_mem_offset(j, r_base, ins->offset, rs);
                }
                break;
            }

            case IR_LOAD_ARRAY: {
                int r_base = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int r_idx = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                int scale = (ins->type == TYPE_F32 || ins->type == TYPE_I32) ? 4 : 8;
                if (ins->type == TYPE_F32) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadss_array(j, xd, r_base, r_idx, scale);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem32s_array(j, rd, r_base, r_idx, scale);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem_array(j, rd, r_base, r_idx, scale);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_ARRAY: {
                int r_base = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int r_idx = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                int scale = (ins->type == TYPE_F32 || ins->type == TYPE_I32) ? 4 : 8;
                if (ins->type == TYPE_F32) {
                    int xs = get_vreg_float_reg(ir, ins->src3, 4, j);
                    emit_storess_array(j, r_base, r_idx, xs, scale);
                } else if (ins->type == TYPE_I32) {
                    int rs = get_vreg_int_reg(ir, ins->src3, PREG_RAX, j);
                    emit_store_mem32_array(j, r_base, r_idx, rs, scale);
                } else {
                    int rs = get_vreg_int_reg(ir, ins->src3, PREG_RAX, j);
                    emit_store_mem_array(j, r_base, r_idx, rs, scale);
                }
                break;
            }

            case IR_LOAD_DEREF: {
                int r_ptr = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->type == TYPE_F32) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadss_offset(j, xd, r_ptr, 0);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem32s_offset(j, rd, r_ptr, 0);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem_offset(j, rd, r_ptr, 0);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_DEREF: {
                int r_ptr = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->type == TYPE_F32) {
                    int xs = get_vreg_float_reg(ir, ins->src2, 4, j);
                    emit_storess_offset(j, r_ptr, 0, xs);
                } else if (ins->type == TYPE_I32) {
                    int rs = get_vreg_int_reg(ir, ins->src2, PREG_RAX, j);
                    emit_store_mem32_offset(j, r_ptr, 0, rs);
                } else {
                    int rs = get_vreg_int_reg(ir, ins->src2, PREG_RAX, j);
                    emit_store_mem_offset(j, r_ptr, 0, rs);
                }
                break;
            }

            case IR_PTR_CHECK: {
                emit_vzeroupper(j);
                int rp = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                emit_push_r(j, rp);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<unknown>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
                emit_push_r(j, PREG_RAX);
                emit_call_with_args(j, (void*)vortex_safe_check_ptr, 3);
                break;
            }

            case IR_BOUNDS_CHECK: {
                emit_vzeroupper(j);
                int rb = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                emit_push_r(j, rb);
                int ri = get_vreg_int_reg(ir, ins->src2, PREG_RAX, j);
                emit_push_r(j, ri);
                if (ins->src3 != VREG_NONE) {
                    int rl = get_vreg_int_reg(ir, ins->src3, PREG_RAX, j);
                    emit_push_r(j, rl);
                } else {
                    emit_mov_ri64(j, PREG_RAX, ins->known_len);
                    emit_push_r(j, PREG_RAX);
                }
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->offset);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<unknown>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
                emit_push_r(j, PREG_RAX);
                emit_call_with_args(j, (void*)vortex_safe_check_bounds, 6);
                break;
            }

            case IR_RANGE_CHECK: {
                emit_vzeroupper(j);
                int rb = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                emit_push_r(j, rb);
                int rs = get_vreg_int_reg(ir, ins->src2, PREG_RAX, j);
                emit_push_r(j, rs);
                int re = get_vreg_int_reg(ir, ins->src3, PREG_RAX, j);
                emit_push_r(j, re);
                if (ins->src4 != VREG_NONE) {
                    int rl = get_vreg_int_reg(ir, ins->src4, PREG_RAX, j);
                    emit_push_r(j, rl);
                } else {
                    emit_mov_ri64(j, PREG_RAX, ins->known_len);
                    emit_push_r(j, PREG_RAX);
                }
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->offset);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<unknown>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
                emit_push_r(j, PREG_RAX);
                emit_call_with_args(j, (void*)vortex_safe_check_range, 7);
                break;
            }

            case IR_FFI_CHECK: {
                emit_vzeroupper(j);
                int rv = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                emit_push_r(j, rv);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->type);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, ins->imm);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->func_name[0] ? ins->func_name : "<ffi>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->offset);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<unknown>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
                emit_push_r(j, PREG_RAX);
                emit_call_with_args(j, (void*)vortex_safe_check_ffi_arg, 7);
                break;
            }

            case IR_LABEL: {
                int lbl = (int)ins->imm;
                if (lbl >= 0 && lbl < MAX_LABELS) {
                    label_offsets[lbl] = (int)j->size;
                    for (int p = 0; p < patch_count; p++) {
                        if (patches[p].label_id == lbl) {
                            int32_t rel = (int32_t)(j->size - (patches[p].patch_pos + 4));
                            memcpy(&j->code[patches[p].patch_pos], &rel, 4);
                        }
                    }
                }
                break;
            }

            case IR_JMP: {
                int lbl = (int)ins->imm;
                emit_u8(j, 0xE9);
                if (lbl >= 0 && lbl < MAX_LABELS && label_offsets[lbl] >= 0) {
                    int32_t rel = (int32_t)(label_offsets[lbl] - (j->size + 4));
                    emit_u32(j, (uint32_t)rel);
                } else {
                    patches[patch_count++] = (JumpPatch){lbl, j->size};
                    emit_u32(j, 0);
                }
                break;
            }

            case IR_JZ:
            case IR_JNZ: {
                int r = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                emit_test_rr(j, r, r);
                int lbl = (int)ins->imm;
                uint8_t cond = (ins->op == IR_JZ) ? 0x84 : 0x85;
                emit_u8(j, 0x0F); emit_u8(j, cond);
                if (lbl >= 0 && lbl < MAX_LABELS && label_offsets[lbl] >= 0) {
                    int32_t rel = (int32_t)(label_offsets[lbl] - (j->size + 4));
                    emit_u32(j, (uint32_t)rel);
                } else {
                    patches[patch_count++] = (JumpPatch){lbl, j->size};
                    emit_u32(j, 0);
                }
                break;
            }

            case IR_CALL: {
                emit_vzeroupper(j);
                if (ins->func_ptr == (void*)helper_print_f32) {
                    int x = get_vreg_float_reg(ir, ins->args[0], 0, j);
                    if (x != 0) emit_movaps_xx(j, 0, x);
                    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
                    emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)helper_print_f32);
                    emit_u8(j, 0xFF); emit_u8(j, 0xD0);
                    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
                } else {
                    for (int a = 0; a < ins->arg_count; a++) {
                        VReg av = ins->args[a];
                        if (ir->vregs[av].is_float) {
                            int x = get_vreg_float_reg(ir, av, 4, j);
                            emit_u8(j, 0x66); emit_u8(j, 0x48); emit_u8(j, 0x0F); emit_u8(j, 0x7E);
                            emit_u8(j, (uint8_t)(0xC0 | ((x & 7) << 3)));
                            emit_u8(j, 0x50);
                        } else {
                            int r = get_vreg_int_reg(ir, av, PREG_RAX, j);
                            emit_push_r(j, r);
                        }
                    }
                    emit_call_with_args(j, ins->func_ptr, ins->arg_count);
                }
                if (ins->dest != VREG_NONE) {
                    if (ins->type == TYPE_F32) {
                        commit_vreg_float_reg(ir, ins->dest, 0, j);
                    } else {
                        commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                    }
                }
                break;
            }

            case IR_CALL_USER: {
                int num_args = ins->arg_count;
                for (int a = 0; a < num_args; a++) {
                    if (ir->vregs[ins->args[a]].is_float) {
                        int xr = get_vreg_float_reg(ir, ins->args[a], 0, j);
                        /* MOVD/MOVQ EAX/RAX, XMM_xr */
                        emit_u8(j, 0x66);
                        if (ir->vregs[ins->args[a]].type == TYPE_F64) {
                            emit_u8(j, (xr >= 8) ? 0x4C : 0x48);
                        } else if (xr >= 8) {
                            emit_u8(j, 0x44);
                        }
                        emit_u8(j, 0x0F); emit_u8(j, 0x7E);
                        emit_u8(j, (uint8_t)(0xC0 | ((xr & 7) << 3)));
                        emit_push_r(j, PREG_RAX);
                    } else {
                        int r = get_vreg_int_reg(ir, ins->args[a], PREG_RAX, j);
                        emit_push_r(j, r);
                    }
                }

                int alloc_space = 0;
                if (g_target_abi == VORTEX_ABI_SYSV64) {
                    alloc_space = (num_args > 6) ? (num_args - 6) * 8 : 0;
                    if ((num_args * 8 + alloc_space) % 16 != 0) alloc_space += 8;
                    emit_sub_rsp(j, alloc_space);

                    for (int k = 6; k < num_args; k++) {
                        int src_off = alloc_space + (num_args - 1 - k) * 8;
                        int dst_off = (k - 6) * 8;
                        emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                        emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                    }

                    static const int sysv_uargs[6] = { PREG_RDI, PREG_RSI, PREG_RDX, PREG_RCX, PREG_R8, PREG_R9 };
                    for (int k = 0; k < num_args && k < 6; k++) {
                        int off = alloc_space + (num_args - 1 - k) * 8;
                        emit_load_mem_offset(j, sysv_uargs[k], PREG_RSP, off);
                    }
                } else {
                    int min_space = (num_args <= 4) ? 32 : (32 + (num_args - 4) * 8);
                    alloc_space = min_space;
                    if ((num_args * 8 + alloc_space) % 16 != 0) alloc_space += 8;
                    emit_sub_rsp(j, alloc_space);

                    for (int k = 4; k < num_args; k++) {
                        int src_off = alloc_space + (num_args - 1 - k) * 8;
                        int dst_off = 32 + (k - 4) * 8;
                        emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                        emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                    }

                    if (num_args > 0) {
                        int off0 = alloc_space + (num_args - 1) * 8;
                        emit_load_mem_offset(j, PREG_RCX, PREG_RSP, off0);
                    }
                    if (num_args > 1) {
                        int off1 = alloc_space + (num_args - 2) * 8;
                        emit_load_mem_offset(j, PREG_RDX, PREG_RSP, off1);
                    }
                    if (num_args > 2) {
                        int off2 = alloc_space + (num_args - 3) * 8;
                        emit_load_mem_offset(j, PREG_R8, PREG_RSP, off2);
                    }
                    if (num_args > 3) {
                        int off3 = alloc_space + (num_args - 4) * 8;
                        emit_load_mem_offset(j, PREG_R9, PREG_RSP, off3);
                    }
                }

                emit_u8(j, 0xE8);
                size_t call_pos = j->size;
                emit_u32(j, 0);

                int total_cleanup = alloc_space + num_args * 8;
                emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); emit_u32(j, (uint32_t)total_cleanup);

                if (g_user_func_reloc_count < MAX_USER_FUNC_RELOCS) {
                    UserFuncReloc* ufr = &g_user_func_relocs[g_user_func_reloc_count++];
                    ufr->patch_pos = call_pos;
                    strncpy(ufr->name, ins->func_name, sizeof(ufr->name) - 1);
                    ufr->name[sizeof(ufr->name) - 1] = '\0';
                    ufr->called_in_unsafe = (ins->imm != 0);
                    ufr->call_line = ins->line;
                    ufr->call_file = vortex_intern_cstr(ins->file ? ins->file : "<unknown>");
                }

                if (ins->dest != VREG_NONE) {
                    commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                }
                break;
            }

            case IR_CALL_FFI: {
                int num_args = ins->arg_count;

                for (int a = 0; a < num_args; a++) {
                    VReg av = ins->args[a];
                    if (ir->vregs[av].is_float) {
                        int x = get_vreg_float_reg(ir, av, 4, j);
                        if (ins->arg_types[a] == TYPE_F64) {
                            emit_u8(j, 0xF3);
                            if (x >= 8) emit_u8(j, 0x41);
                            emit_u8(j, 0x0F); emit_u8(j, 0x5A);
                            emit_u8(j, (uint8_t)(0xC0 | (4 << 3) | (x & 7)));
                            emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x08);
                            emit_u8(j, 0xF2); emit_u8(j, 0x0F); emit_u8(j, 0x11); emit_u8(j, 0x24); emit_u8(j, 0x24);
                        } else {
                            emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x08);
                            emit_u8(j, 0xF3);
                            if (x >= 8) emit_u8(j, 0x44);
                            emit_u8(j, 0x0F); emit_u8(j, 0x11);
                            emit_u8(j, (uint8_t)(0x04 | ((x & 7) << 3)));
                            emit_u8(j, 0x24);
                        }
                    } else {
                        int r = get_vreg_int_reg(ir, av, PREG_RAX, j);
                        emit_push_r(j, r);
                    }
                }

#ifdef _WIN32
                int min_space = (num_args <= 4) ? 32 : (32 + (num_args - 4) * 8);
                int alloc_space = min_space;
                if ((num_args * 8 + alloc_space) % 16 != 0) alloc_space += 8;
                emit_sub_rsp(j, alloc_space);

                for (int k = 4; k < num_args; k++) {
                    int src_off = alloc_space + (num_args - 1 - k) * 8;
                    int dst_off = 32 + (k - 4) * 8;
                    emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                    emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                }

                static const int win_arg_gprs[4] = { PREG_RCX, PREG_RDX, PREG_R8, PREG_R9 };
                for (int k = 0; k < 4 && k < num_args; k++) {
                    int offk = alloc_space + (num_args - 1 - k) * 8;
                    if (ins->arg_types[k] == TYPE_F64) {
                        emit_loadsd_offset(j, k, PREG_RSP, offk);
                    } else if (ins->arg_types[k] == TYPE_F32) {
                        emit_loadss_offset(j, k, PREG_RSP, offk);
                    } else {
                        emit_load_mem_offset(j, win_arg_gprs[k], PREG_RSP, offk);
                    }
                }

                emit_vzeroupper(j);
                emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)ins->func_ptr);
                emit_u8(j, 0xFF); emit_u8(j, 0xD0);

                int total_cleanup = alloc_space + num_args * 8;
                emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); emit_u32(j, (uint32_t)total_cleanup);
#else
                int int_count = 0;
                int flt_count = 0;
                int stack_arg_count = 0;
                for (int k = 0; k < num_args; k++) {
                    bool is_flt = (ins->arg_types[k] == TYPE_F32 || ins->arg_types[k] == TYPE_F64);
                    if (is_flt) {
                        if (flt_count < 8) flt_count++;
                        else stack_arg_count++;
                    } else {
                        if (int_count < 6) int_count++;
                        else stack_arg_count++;
                    }
                }

                int alloc_space = stack_arg_count * 8;
                if ((num_args * 8 + alloc_space) % 16 != 0) alloc_space += 8;
                emit_sub_rsp(j, alloc_space);

                int cur_int = 0;
                int cur_flt = 0;
                int cur_stack = 0;
                static const int sysv_gprs[6] = { PREG_RDI, PREG_RSI, PREG_RDX, PREG_RCX, PREG_R8, PREG_R9 };
                for (int k = 0; k < num_args; k++) {
                    int src_off = alloc_space + (num_args - 1 - k) * 8;
                    bool is_flt = (ins->arg_types[k] == TYPE_F32 || ins->arg_types[k] == TYPE_F64);
                    if (is_flt) {
                        if (cur_flt < 8) {
                            if (ins->arg_types[k] == TYPE_F64) emit_loadsd_offset(j, cur_flt, PREG_RSP, src_off);
                            else emit_loadss_offset(j, cur_flt, PREG_RSP, src_off);
                            cur_flt++;
                        } else {
                            int dst_off = cur_stack * 8;
                            cur_stack++;
                            emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                            emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                        }
                    } else {
                        if (cur_int < 6) {
                            emit_load_mem_offset(j, sysv_gprs[cur_int], PREG_RSP, src_off);
                            cur_int++;
                        } else {
                            int dst_off = cur_stack * 8;
                            cur_stack++;
                            emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                            emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                        }
                    }
                }

                emit_u8(j, 0xB0); emit_u8(j, (uint8_t)cur_flt);
                emit_vzeroupper(j);
                emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)ins->func_ptr);
                emit_u8(j, 0xFF); emit_u8(j, 0xD0);

                int total_cleanup = alloc_space + num_args * 8;
                emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); emit_u32(j, (uint32_t)total_cleanup);
#endif

                if (ins->dest != VREG_NONE) {
                    if (ins->ret_type == TYPE_F64) {
                        emit_u8(j, 0xF2); emit_u8(j, 0x0F); emit_u8(j, 0x5A); emit_u8(j, 0xC0);
                        commit_vreg_float_reg(ir, ins->dest, 0, j);
                    } else if (ins->ret_type == TYPE_F32) {
                        commit_vreg_float_reg(ir, ins->dest, 0, j);
                    } else if (ins->ret_type == TYPE_I32) {
                        emit_u8(j, 0x48); emit_u8(j, 0x63); emit_u8(j, 0xC0);
                        commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                    } else if (ins->ret_type != TYPE_VOID) {
                        commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                    }
                }
                break;
            }

            case IR_LOAD_PARAM: {
                int param_idx = (int)ins->imm;
                int disp = (g_target_abi == VORTEX_ABI_SYSV64)
                         ? ((param_idx < 6) ? -(64 + 8 * param_idx) : (16 + 8 * (param_idx - 6)))
                         : (16 + 8 * param_idx);
                if (ir->vregs[ins->dest].is_float) {
                    int xrd = (ir->vregs[ins->dest].is_spilled || ir->vregs[ins->dest].preg == PREG_NONE)
                              ? 0 : ir->vregs[ins->dest].preg;
                    emit_u8(j, (ir->vregs[ins->dest].type == TYPE_F64) ? 0xF2 : 0xF3);
                    if (xrd >= 8) emit_u8(j, 0x44);
                    emit_u8(j, 0x0F); emit_u8(j, 0x10);
                    if (disp >= -128 && disp <= 127) {
                        emit_u8(j, (uint8_t)(0x45 | ((xrd & 7) << 3)));
                        emit_u8(j, (uint8_t)disp);
                    } else {
                        emit_u8(j, (uint8_t)(0x85 | ((xrd & 7) << 3)));
                        emit_u32(j, (uint32_t)disp);
                    }
                    commit_vreg_float_reg(ir, ins->dest, xrd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem_offset(j, rd, PREG_RBP, disp);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_RET: {
                emit_vzeroupper(j);
                if (ins->src1 != VREG_NONE) {
                    if (ins->type == TYPE_F32) {
                        int x = get_vreg_float_reg(ir, ins->src1, 0, j);
                        if (x != 0) emit_movaps_xx(j, 0, x);
                    } else {
                        int r = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                        if (r != PREG_RAX) emit_mov_rr(j, PREG_RAX, r);
                    }
                }
                emit_func_epilogue(j, compute_frame_size(ir->total_spill_bytes));
                break;
            }

            case IR_RAW_RIEMANN:
            case IR_VEC_LOOP: {
                emit_avx2_vector_loop(j, ir, ins);
                break;
            }
        }
        if (ins->line > 0 && j->size > ins_start_off) {
            debug_info_record_line(ins_start_off, j->size, ins->line, ins->file ? ins->file : g_current_filename);
        }
    }

    for (int p = 0; p < patch_count; p++) {
        int lbl = patches[p].label_id;
        if (lbl >= 0 && lbl < MAX_LABELS && label_offsets[lbl] >= 0) {
            int32_t rel = (int32_t)(label_offsets[lbl] - (patches[p].patch_pos + 4));
            memcpy(&j->code[patches[p].patch_pos], &rel, 4);
        }
    }
    free(label_offsets);
    free(patches);
}

static void patch_call_rel_x64(JITBuffer* j, size_t patch_pos, size_t target_offset) {
    int32_t rel = (int32_t)(target_offset - (patch_pos + 4));
    memcpy(&j->code[patch_pos], &rel, 4);
}

const VortexJITBackend g_backend_x64 = {
    VORTEX_ARCH_X86_64,
    "x86_64-avx2",
    compute_frame_size,
    emit_prologue,
    emit_epilogue,
    emit_func_prologue,
    emit_func_epilogue,
    emit_jmp_placeholder,
    patch_jump,
    patch_call_rel_x64,
    ir_codegen_x86_64
};
