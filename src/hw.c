/*
 * VORTEX HARDWARE SILICON PROFILER, ROUTE PLANNER & AVX2/FMA3 + OPENCL GPU KERNELS
 * Probes host CPU topology, L1/L2/L3 caches, SIMD ISA, RAM, and OpenCL GPU dynamically,
 * pre-computes optimal memory/compute execution routes, and accelerates GEMM/DOT/AXPY.
 */

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#endif

#include "vortex.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#define VORTEX_HW_X86 1
#else
#define VORTEX_HW_X86 0
#endif

/* Execution Route Constants (matches std/hw.vx) */
#define HW_ROUTE_L1          1
#define HW_ROUTE_L2          2
#define HW_ROUTE_L3          3
#define HW_ROUTE_RAM_STREAM  4
#define HW_ROUTE_GPU         5

/* --- Minimal Self-Contained OpenCL 1.2 C ABI Bindings (No External SDK Needed) --- */
typedef int32_t  vx_cl_int;
typedef uint32_t vx_cl_uint;
typedef uint64_t vx_cl_ulong;
typedef uint64_t vx_cl_bitfield;
typedef void*    vx_cl_platform_id;
typedef void*    vx_cl_device_id;
typedef void*    vx_cl_context;
typedef void*    vx_cl_command_queue;
typedef void*    vx_cl_program;
typedef void*    vx_cl_kernel;
typedef void*    vx_cl_mem;

#define VX_CL_SUCCESS                    0
#define VX_CL_DEVICE_TYPE_GPU            (1ULL << 2)
#define VX_CL_DEVICE_MAX_COMPUTE_UNITS   0x1002
#define VX_CL_DEVICE_GLOBAL_MEM_SIZE     0x101F
#define VX_CL_DEVICE_LOCAL_MEM_SIZE      0x1023
#define VX_CL_DEVICE_NAME                0x102B
#define VX_CL_DEVICE_DOUBLE_FP_CONFIG    0x1032
#define VX_CL_MEM_READ_WRITE             (1ULL << 0)
#define VX_CL_MEM_WRITE_ONLY             (1ULL << 1)
#define VX_CL_MEM_READ_ONLY              (1ULL << 2)
#define VX_CL_MEM_COPY_HOST_PTR          (1ULL << 5)
#define VX_CL_TRUE                       1

#ifdef _WIN32
#define VX_CL_CALL __stdcall
#else
#define VX_CL_CALL
#endif

typedef vx_cl_int (VX_CL_CALL *pfn_clGetPlatformIDs)(vx_cl_uint, vx_cl_platform_id*, vx_cl_uint*);
typedef vx_cl_int (VX_CL_CALL *pfn_clGetDeviceIDs)(vx_cl_platform_id, vx_cl_bitfield, vx_cl_uint, vx_cl_device_id*, vx_cl_uint*);
typedef vx_cl_int (VX_CL_CALL *pfn_clGetDeviceInfo)(vx_cl_device_id, vx_cl_uint, size_t, void*, size_t*);
typedef vx_cl_context (VX_CL_CALL *pfn_clCreateContext)(const intptr_t*, vx_cl_uint, const vx_cl_device_id*, void (VX_CL_CALL *)(const char*, const void*, size_t, void*), void*, vx_cl_int*);
typedef vx_cl_command_queue (VX_CL_CALL *pfn_clCreateCommandQueue)(vx_cl_context, vx_cl_device_id, vx_cl_bitfield, vx_cl_int*);
typedef vx_cl_program (VX_CL_CALL *pfn_clCreateProgramWithSource)(vx_cl_context, vx_cl_uint, const char**, const size_t*, vx_cl_int*);
typedef vx_cl_int (VX_CL_CALL *pfn_clBuildProgram)(vx_cl_program, vx_cl_uint, const vx_cl_device_id*, const char*, void (VX_CL_CALL *)(vx_cl_program, void*), void*);
typedef vx_cl_kernel (VX_CL_CALL *pfn_clCreateKernel)(vx_cl_program, const char*, vx_cl_int*);
typedef vx_cl_mem (VX_CL_CALL *pfn_clCreateBuffer)(vx_cl_context, vx_cl_bitfield, size_t, void*, vx_cl_int*);
typedef vx_cl_int (VX_CL_CALL *pfn_clEnqueueWriteBuffer)(vx_cl_command_queue, vx_cl_mem, vx_cl_uint, size_t, size_t, const void*, vx_cl_uint, const void*, void*);
typedef vx_cl_int (VX_CL_CALL *pfn_clEnqueueReadBuffer)(vx_cl_command_queue, vx_cl_mem, vx_cl_uint, size_t, size_t, void*, vx_cl_uint, const void*, void*);
typedef vx_cl_int (VX_CL_CALL *pfn_clSetKernelArg)(vx_cl_kernel, vx_cl_uint, size_t, const void*);
typedef vx_cl_int (VX_CL_CALL *pfn_clEnqueueNDRangeKernel)(vx_cl_command_queue, vx_cl_kernel, vx_cl_uint, const size_t*, const size_t*, const size_t*, vx_cl_uint, const void*, void*);
typedef vx_cl_int (VX_CL_CALL *pfn_clFinish)(vx_cl_command_queue);
typedef vx_cl_int (VX_CL_CALL *pfn_clReleaseMemObject)(vx_cl_mem);

typedef struct {
    bool initialized;
    char cpu_name[128];
    char gpu_name[128];
    int64_t phys_cores;
    int64_t logical_cores;
    int64_t l1d_kb;
    int64_t l1i_kb;
    int64_t l2_kb;
    int64_t l3_kb;
    int64_t ram_mb;
    int64_t simd_width;
    bool has_avx;
    bool has_avx2;
    bool has_fma3;
    bool has_avx512;
    uint64_t core_masks[VORTEX_MAX_THREADS];

    /* GPU / OpenCL state */
    bool has_gpu;
    bool gpu_fp64;
    int64_t gpu_cus;
    int64_t gpu_vram_mb;
    int64_t gpu_lds_kb;
    bool cl_compiled;

    void* cl_lib;
    vx_cl_device_id cl_dev;
    vx_cl_context cl_ctx;
    vx_cl_command_queue cl_queue;
    vx_cl_program cl_prog_f32;
    vx_cl_program cl_prog_f64;
    vx_cl_kernel k_matmul_f32;
    vx_cl_kernel k_matmul_f64;

    pfn_clGetPlatformIDs p_get_platforms;
    pfn_clGetDeviceIDs p_get_devices;
    pfn_clGetDeviceInfo p_get_dev_info;
    pfn_clCreateContext p_create_ctx;
    pfn_clCreateCommandQueue p_create_queue;
    pfn_clCreateProgramWithSource p_create_prog;
    pfn_clBuildProgram p_build_prog;
    pfn_clCreateKernel p_create_kernel;
    pfn_clCreateBuffer p_create_buf;
    pfn_clEnqueueWriteBuffer p_write_buf;
    pfn_clEnqueueReadBuffer p_read_buf;
    pfn_clSetKernelArg p_set_arg;
    pfn_clEnqueueNDRangeKernel p_ndrange;
    pfn_clFinish p_finish;
    pfn_clReleaseMemObject p_rel_mem;
} VortexHwProfile;

static VortexHwProfile g_hw;

static void hw_trim_spaces(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

static void* hw_dl_sym(void* lib, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

#define HW_LOAD_CL(field, fn_name) do { \
    void* _sym = hw_dl_sym(lib, fn_name); \
    if (!_sym) return; \
    memcpy(&g_hw.field, &_sym, sizeof(_sym)); \
} while (0)

static void hw_probe_opencl(void) {
    void* lib = NULL;
#ifdef _WIN32
    lib = (void*)LoadLibraryA("OpenCL.dll");
#elif defined(__APPLE__)
    lib = dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_LAZY);
#else
    lib = dlopen("libOpenCL.so.1", RTLD_LAZY);
    if (!lib) lib = dlopen("libOpenCL.so", RTLD_LAZY);
#endif
    if (!lib) return;
    g_hw.cl_lib = lib;

    HW_LOAD_CL(p_get_platforms, "clGetPlatformIDs");
    HW_LOAD_CL(p_get_devices,   "clGetDeviceIDs");
    HW_LOAD_CL(p_get_dev_info,  "clGetDeviceInfo");
    HW_LOAD_CL(p_create_ctx,    "clCreateContext");
    HW_LOAD_CL(p_create_queue,  "clCreateCommandQueue");
    HW_LOAD_CL(p_create_prog,   "clCreateProgramWithSource");
    HW_LOAD_CL(p_build_prog,    "clBuildProgram");
    HW_LOAD_CL(p_create_kernel, "clCreateKernel");
    HW_LOAD_CL(p_create_buf,    "clCreateBuffer");
    HW_LOAD_CL(p_write_buf,     "clEnqueueWriteBuffer");
    HW_LOAD_CL(p_read_buf,      "clEnqueueReadBuffer");
    HW_LOAD_CL(p_set_arg,       "clSetKernelArg");
    HW_LOAD_CL(p_ndrange,       "clEnqueueNDRangeKernel");
    HW_LOAD_CL(p_finish,        "clFinish");
    HW_LOAD_CL(p_rel_mem,       "clReleaseMemObject");

    vx_cl_platform_id plats[8];
    vx_cl_uint num_plats = 0;
    if (g_hw.p_get_platforms(8, plats, &num_plats) != VX_CL_SUCCESS || num_plats == 0) return;

    vx_cl_device_id best_dev = NULL;
    vx_cl_uint best_cu = 0;
    for (vx_cl_uint p = 0; p < num_plats && p < 8; p++) {
        vx_cl_device_id devs[8];
        vx_cl_uint num_devs = 0;
        if (g_hw.p_get_devices(plats[p], VX_CL_DEVICE_TYPE_GPU, 8, devs, &num_devs) != VX_CL_SUCCESS) continue;
        for (vx_cl_uint d = 0; d < num_devs && d < 8; d++) {
            vx_cl_uint cu = 0;
            g_hw.p_get_dev_info(devs[d], VX_CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cu), &cu, NULL);
            if (!best_dev || cu > best_cu) {
                best_dev = devs[d];
                best_cu = cu;
            }
        }
    }
    if (!best_dev) return;

    g_hw.has_gpu = true;
    g_hw.cl_dev = best_dev;
    g_hw.gpu_cus = (int64_t)best_cu;
    g_hw.p_get_dev_info(best_dev, VX_CL_DEVICE_NAME, sizeof(g_hw.gpu_name) - 1, g_hw.gpu_name, NULL);
    hw_trim_spaces(g_hw.gpu_name);

    vx_cl_ulong vram = 0, lds = 0, fp64_cfg = 0;
    g_hw.p_get_dev_info(best_dev, VX_CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(vram), &vram, NULL);
    g_hw.p_get_dev_info(best_dev, VX_CL_DEVICE_LOCAL_MEM_SIZE, sizeof(lds), &lds, NULL);
    g_hw.p_get_dev_info(best_dev, VX_CL_DEVICE_DOUBLE_FP_CONFIG, sizeof(fp64_cfg), &fp64_cfg, NULL);
    g_hw.gpu_vram_mb = (int64_t)(vram / (1024ULL * 1024ULL));
    g_hw.gpu_lds_kb = (int64_t)(lds / 1024ULL);
    g_hw.gpu_fp64 = (fp64_cfg != 0);
}

static bool hw_ensure_opencl_kernels(void) {
    if (!g_hw.has_gpu) return false;
    if (g_hw.cl_compiled) return g_hw.k_matmul_f32 != NULL;
    g_hw.cl_compiled = true;

    vx_cl_int err = 0;
    g_hw.cl_ctx = g_hw.p_create_ctx(NULL, 1, &g_hw.cl_dev, NULL, NULL, &err);
    if (err != VX_CL_SUCCESS || !g_hw.cl_ctx) return false;

    g_hw.cl_queue = g_hw.p_create_queue(g_hw.cl_ctx, g_hw.cl_dev, 0, &err);
    if (err != VX_CL_SUCCESS || !g_hw.cl_queue) return false;

    static const char* cl_src_f32 =
        "__kernel void matmul_f32(__global const float* A, __global const float* B, __global float* C, int M, int N, int K) {\n"
        "    int row = get_global_id(1);\n"
        "    int col = get_global_id(0);\n"
        "    int lr = get_local_id(1);\n"
        "    int lc = get_local_id(0);\n"
        "    __local float As[16][16];\n"
        "    __local float Bs[16][16];\n"
        "    float acc = 0.0f;\n"
        "    for (int t = 0; t < K; t += 16) {\n"
        "        int ac = t + lc;\n"
        "        int br = t + lr;\n"
        "        As[lr][lc] = (row < M && ac < K) ? A[row * K + ac] : 0.0f;\n"
        "        Bs[lr][lc] = (br < K && col < N) ? B[br * N + col] : 0.0f;\n"
        "        barrier(CLK_LOCAL_MEM_FENCE);\n"
        "        #pragma unroll\n"
        "        for (int i = 0; i < 16; i++) acc = fma(As[lr][i], Bs[i][lc], acc);\n"
        "        barrier(CLK_LOCAL_MEM_FENCE);\n"
        "    }\n"
        "    if (row < M && col < N) C[row * N + col] = acc;\n"
        "}\n";

    size_t len_f32 = strlen(cl_src_f32);
    g_hw.cl_prog_f32 = g_hw.p_create_prog(g_hw.cl_ctx, 1, &cl_src_f32, &len_f32, &err);
    if (err == VX_CL_SUCCESS && g_hw.cl_prog_f32) {
        if (g_hw.p_build_prog(g_hw.cl_prog_f32, 1, &g_hw.cl_dev, "-cl-fast-relaxed-math", NULL, NULL) == VX_CL_SUCCESS) {
            g_hw.k_matmul_f32 = g_hw.p_create_kernel(g_hw.cl_prog_f32, "matmul_f32", &err);
            if (err != VX_CL_SUCCESS) g_hw.k_matmul_f32 = NULL;
        }
    }

    if (g_hw.gpu_fp64) {
        static const char* cl_src_f64 =
            "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n"
            "__kernel void matmul_f64(__global const double* A, __global const double* B, __global double* C, int M, int N, int K) {\n"
            "    int row = get_global_id(1);\n"
            "    int col = get_global_id(0);\n"
            "    int lr = get_local_id(1);\n"
            "    int lc = get_local_id(0);\n"
            "    __local double As[16][16];\n"
            "    __local double Bs[16][16];\n"
            "    double acc = 0.0;\n"
            "    for (int t = 0; t < K; t += 16) {\n"
            "        int ac = t + lc;\n"
            "        int br = t + lr;\n"
            "        As[lr][lc] = (row < M && ac < K) ? A[row * K + ac] : 0.0;\n"
            "        Bs[lr][lc] = (br < K && col < N) ? B[br * N + col] : 0.0;\n"
            "        barrier(CLK_LOCAL_MEM_FENCE);\n"
            "        #pragma unroll\n"
            "        for (int i = 0; i < 16; i++) acc = fma(As[lr][i], Bs[i][lc], acc);\n"
            "        barrier(CLK_LOCAL_MEM_FENCE);\n"
            "    }\n"
            "    if (row < M && col < N) C[row * N + col] = acc;\n"
            "}\n";
        size_t len_f64 = strlen(cl_src_f64);
        g_hw.cl_prog_f64 = g_hw.p_create_prog(g_hw.cl_ctx, 1, &cl_src_f64, &len_f64, &err);
        if (err == VX_CL_SUCCESS && g_hw.cl_prog_f64) {
            if (g_hw.p_build_prog(g_hw.cl_prog_f64, 1, &g_hw.cl_dev, "", NULL, NULL) == VX_CL_SUCCESS) {
                g_hw.k_matmul_f64 = g_hw.p_create_kernel(g_hw.cl_prog_f64, "matmul_f64", &err);
                if (err != VX_CL_SUCCESS) g_hw.k_matmul_f64 = NULL;
            }
        }
    }
    return g_hw.k_matmul_f32 != NULL;
}

static bool hw_gpu_matmul_f32(const float* A, const float* B, float* C, int M, int N, int K) {
    if (!hw_ensure_opencl_kernels() || !g_hw.k_matmul_f32) return false;
    vx_cl_int err = 0;
    size_t sz_a = (size_t)M * (size_t)K * sizeof(float);
    size_t sz_b = (size_t)K * (size_t)N * sizeof(float);
    size_t sz_c = (size_t)M * (size_t)N * sizeof(float);

    vx_cl_mem dA = g_hw.p_create_buf(g_hw.cl_ctx, VX_CL_MEM_READ_ONLY | VX_CL_MEM_COPY_HOST_PTR, sz_a, (void*)A, &err);
    if (err != VX_CL_SUCCESS || !dA) return false;
    vx_cl_mem dB = g_hw.p_create_buf(g_hw.cl_ctx, VX_CL_MEM_READ_ONLY | VX_CL_MEM_COPY_HOST_PTR, sz_b, (void*)B, &err);
    if (err != VX_CL_SUCCESS || !dB) { g_hw.p_rel_mem(dA); return false; }
    vx_cl_mem dC = g_hw.p_create_buf(g_hw.cl_ctx, VX_CL_MEM_WRITE_ONLY, sz_c, NULL, &err);
    if (err != VX_CL_SUCCESS || !dC) { g_hw.p_rel_mem(dA); g_hw.p_rel_mem(dB); return false; }

    err |= g_hw.p_set_arg(g_hw.k_matmul_f32, 0, sizeof(vx_cl_mem), &dA);
    err |= g_hw.p_set_arg(g_hw.k_matmul_f32, 1, sizeof(vx_cl_mem), &dB);
    err |= g_hw.p_set_arg(g_hw.k_matmul_f32, 2, sizeof(vx_cl_mem), &dC);
    err |= g_hw.p_set_arg(g_hw.k_matmul_f32, 3, sizeof(int), &M);
    err |= g_hw.p_set_arg(g_hw.k_matmul_f32, 4, sizeof(int), &N);
    err |= g_hw.p_set_arg(g_hw.k_matmul_f32, 5, sizeof(int), &K);

    size_t local[2] = { 16, 16 };
    size_t global[2] = {
        (size_t)(((N + 15) / 16) * 16),
        (size_t)(((M + 15) / 16) * 16)
    };
    if (err == VX_CL_SUCCESS) {
        err = g_hw.p_ndrange(g_hw.cl_queue, g_hw.k_matmul_f32, 2, NULL, global, local, 0, NULL, NULL);
    }
    if (err == VX_CL_SUCCESS) {
        err = g_hw.p_read_buf(g_hw.cl_queue, dC, VX_CL_TRUE, 0, sz_c, C, 0, NULL, NULL);
    }
    g_hw.p_rel_mem(dA);
    g_hw.p_rel_mem(dB);
    g_hw.p_rel_mem(dC);
    return err == VX_CL_SUCCESS;
}

static void hw_probe_cpu(void) {
    vx_str_copy(g_hw.cpu_name, sizeof(g_hw.cpu_name), "Generic CPU");
    vx_str_copy(g_hw.gpu_name, sizeof(g_hw.gpu_name), "None");
    g_hw.l1d_kb = 32;
    g_hw.l1i_kb = 32;
    g_hw.l2_kb = 512;
    g_hw.l3_kb = 8192;
    g_hw.simd_width = 128;

#if VORTEX_HW_X86
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) && eax >= 0x80000004u) {
        uint32_t brand[12];
        __cpuid(0x80000002u, brand[0], brand[1], brand[2], brand[3]);
        __cpuid(0x80000003u, brand[4], brand[5], brand[6], brand[7]);
        __cpuid(0x80000004u, brand[8], brand[9], brand[10], brand[11]);
        char tmp[49];
        memcpy(tmp, brand, 48);
        tmp[48] = '\0';
        hw_trim_spaces(tmp);
        if (tmp[0]) vx_str_copy(g_hw.cpu_name, sizeof(g_hw.cpu_name), tmp);
    }
    if (__get_cpuid(1u, &eax, &ebx, &ecx, &edx)) {
        bool osxsave = (ecx & (1u << 27)) != 0;
        bool avx = (ecx & (1u << 28)) != 0;
        bool fma = (ecx & (1u << 12)) != 0;
        if (osxsave) {
            uint32_t xcr0_lo = 0, xcr0_hi = 0;
            __asm__ __volatile__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
            (void)xcr0_hi;
            if ((xcr0_lo & 0x6u) == 0x6u) {
                g_hw.has_avx = avx;
                g_hw.has_fma3 = fma;
                uint32_t max_leaf = 0;
                __cpuid(0u, max_leaf, ebx, ecx, edx);
                if (max_leaf >= 7u) {
                    __cpuid_count(7u, 0u, eax, ebx, ecx, edx);
                    if (ebx & (1u << 5)) {
                        g_hw.has_avx2 = true;
                        g_hw.simd_width = 256;
                    }
                    if ((xcr0_lo & 0xE6u) == 0xE6u && (ebx & (1u << 16))) {
                        g_hw.has_avx512 = true;
                        g_hw.simd_width = 512;
                    }
                }
            }
        }
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    vx_str_copy(g_hw.cpu_name, sizeof(g_hw.cpu_name), "ARM64 NEON CPU");
    g_hw.simd_width = 128;
#endif

#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_hw.logical_cores = (int64_t)si.dwNumberOfProcessors;
    g_hw.phys_cores = g_hw.logical_cores;

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    if (len > 0) {
        uint8_t* buf = (uint8_t*)malloc(len);
        if (buf && GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
            DWORD off = 0;
            int cores = 0;
            int logical = 0;
            int64_t l1d = 0, l1i = 0, l2 = 0, l3 = 0;
            while (off < len) {
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info =
                    (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
                if (info->Relationship == RelationProcessorCore) {
                    uint64_t mask = (uint64_t)info->Processor.GroupMask[0].Mask;
                    if (cores < VORTEX_MAX_THREADS) g_hw.core_masks[cores] = mask;
                    cores++;
                    for (int b = 0; b < 64; b++) {
                        if (mask & (1ULL << b)) logical++;
                    }
                } else if (info->Relationship == RelationCache) {
                    CACHE_RELATIONSHIP* c = &info->Cache;
                    int64_t kb = (int64_t)(c->CacheSize / 1024);
                    if (c->Level == 1 && c->Type == CacheData && kb > l1d) l1d = kb;
                    else if (c->Level == 1 && c->Type == CacheInstruction && kb > l1i) l1i = kb;
                    else if (c->Level == 2 && (c->Type == CacheUnified || c->Type == CacheData) && kb > l2) l2 = kb;
                    else if (c->Level == 3 && kb > l3) l3 = kb;
                }
                off += info->Size;
            }
            if (cores > 0) g_hw.phys_cores = cores;
            if (logical > 0) g_hw.logical_cores = logical;
            if (l1d > 0) g_hw.l1d_kb = l1d;
            if (l1i > 0) g_hw.l1i_kb = l1i;
            if (l2 > 0) g_hw.l2_kb = l2;
            if (l3 > 0) g_hw.l3_kb = l3;
        }
        free(buf);
    }

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        g_hw.ram_mb = (int64_t)(ms.ullTotalPhys / (1024ULL * 1024ULL));
    }
#else
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    g_hw.logical_cores = (ncpu > 0) ? (int64_t)ncpu : 1;
    g_hw.phys_cores = g_hw.logical_cores;
#ifdef __APPLE__
    int phys = 0;
    size_t sz = sizeof(phys);
    if (sysctlbyname("hw.physicalcpu", &phys, &sz, NULL, 0) == 0 && phys > 0) g_hw.phys_cores = phys;
    uint64_t memsize = 0;
    sz = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &sz, NULL, 0) == 0) g_hw.ram_mb = (int64_t)(memsize / (1024ULL * 1024ULL));
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0) g_hw.ram_mb = ((int64_t)pages * (int64_t)psize) / (1024LL * 1024LL);
#endif
#endif
}

int64_t vortex_hw_init(void) {
    if (g_hw.initialized) return 1;
    memset(&g_hw, 0, sizeof(g_hw));
    hw_probe_cpu();
    hw_probe_opencl();
    g_hw.initialized = true;
    return 1;
}

static inline void hw_ensure_init(void) {
    if (!g_hw.initialized) vortex_hw_init();
}

int64_t vortex_hw_cpu_name(void) {
    hw_ensure_init();
    return (int64_t)(uintptr_t)vortex_intern_cstr(g_hw.cpu_name);
}

int64_t vortex_hw_gpu_name(void) {
    hw_ensure_init();
    return (int64_t)(uintptr_t)vortex_intern_cstr(g_hw.gpu_name);
}

int64_t vortex_hw_l1_kb(void) { hw_ensure_init(); return g_hw.l1d_kb; }
int64_t vortex_hw_l2_kb(void) { hw_ensure_init(); return g_hw.l2_kb; }
int64_t vortex_hw_l3_kb(void) { hw_ensure_init(); return g_hw.l3_kb; }
int64_t vortex_hw_phys_cores(void) { hw_ensure_init(); return g_hw.phys_cores; }
int64_t vortex_hw_logical_cores(void) { hw_ensure_init(); return g_hw.logical_cores; }
int64_t vortex_hw_simd_width(void) { hw_ensure_init(); return g_hw.simd_width; }
int64_t vortex_hw_has_gpu(void) { hw_ensure_init(); return g_hw.has_gpu ? 1 : 0; }

int64_t vortex_hw_plan_route(int64_t working_bytes, int64_t flops) {
    hw_ensure_init();
    if (g_hw.has_gpu && flops >= 33554432LL) return HW_ROUTE_GPU;
    if (working_bytes <= g_hw.l1d_kb * 1024LL * 3 / 4) return HW_ROUTE_L1;
    if (working_bytes <= g_hw.l2_kb * 1024LL * 3 / 4) return HW_ROUTE_L2;
    if (working_bytes <= g_hw.l3_kb * 1024LL * 3 / 4) return HW_ROUTE_L3;
    return HW_ROUTE_RAM_STREAM;
}

int64_t vortex_hw_plan_tile_f64(void) {
    hw_ensure_init();
    int64_t l2_bytes = g_hw.l2_kb * 1024LL / 2;
    int64_t max_elems = l2_bytes / (3 * 8);
    int64_t t = 32;
    while ((t + 8) * (t + 8) <= max_elems && (t + 8) <= 128) t += 8;
    return t;
}

int64_t vortex_hw_pin_workers(int64_t mode) {
    hw_ensure_init();
    if (mode == 2 && g_hw.phys_cores > 0 && g_hw.core_masks[0] != 0) {
        int phys = (int)g_hw.phys_cores;
        if (phys > VORTEX_MAX_THREADS) phys = VORTEX_MAX_THREADS;
        vortex_hw_set_worker_pinning(2);
        for (int i = 0; i < phys - 1; i++) {
            uint64_t m = g_hw.core_masks[i];
            uint64_t single_bit = m & (~m + 1ULL);
            if (single_bit != 0) vortex_thread_pool_set_worker_mask(i, single_bit);
        }
#ifdef _WIN32
        uint64_t cm = g_hw.core_masks[phys - 1];
        uint64_t single_bit = cm & (~cm + 1ULL);
        if (single_bit != 0) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)single_bit);
#endif
        return (int64_t)phys;
    }
    return vortex_hw_set_worker_pinning(mode);
}

int64_t vortex_hw_prefetch(int64_t ptr, int64_t bytes) {
    if (!ptr || bytes <= 0) return 0;
    if (bytes > 16LL * 1024LL * 1024LL) bytes = 16LL * 1024LL * 1024LL;
    const char* p = (const char*)(uintptr_t)ptr;
    int64_t lines = 0;
    for (int64_t off = 0; off < bytes; off += 64) {
#if VORTEX_HW_X86
        _mm_prefetch(p + off, _MM_HINT_T0);
#else
        __builtin_prefetch(p + off, 0, 3);
#endif
        lines++;
    }
    return lines;
}

int64_t vortex_hw_print_map(void) {
    hw_ensure_init();
    int64_t tile64 = vortex_hw_plan_tile_f64();
    printf("+======================================================================+\n");
    printf("|                  VORTEX SILICON HARDWARE CIRCUIT MAP                 |\n");
    printf("+======================================================================+\n");
    printf("| CPU      : %-57s |\n", g_hw.cpu_name);
    printf("| Cores    : %2lld physical / %2lld logical   | SIMD: %3lld-bit %-12s |\n",
           (long long)g_hw.phys_cores, (long long)g_hw.logical_cores,
           (long long)g_hw.simd_width,
           g_hw.has_avx512 ? "(AVX-512)" : (g_hw.has_avx2 && g_hw.has_fma3 ? "(AVX2+FMA3)" : (g_hw.has_avx ? "(AVX)" : "(Scalar)")));
    printf("| L1 Cache : %3lld KB D + %3lld KB I per core (%4lld KB L1D total)          |\n",
           (long long)g_hw.l1d_kb, (long long)g_hw.l1i_kb, (long long)(g_hw.l1d_kb * g_hw.phys_cores));
    printf("| L2 Cache : %3lld KB per core (%5lld KB L2 total)                       |\n",
           (long long)g_hw.l2_kb, (long long)(g_hw.l2_kb * g_hw.phys_cores));
    printf("| L3 Cache : %5lld KB shared   | RAM: %6lld MB                      |\n",
           (long long)g_hw.l3_kb, (long long)g_hw.ram_mb);
    if (g_hw.has_gpu) {
        printf("| GPU      : %-40s (%2lld CU, %5lld MB) |\n",
               g_hw.gpu_name, (long long)g_hw.gpu_cus, (long long)g_hw.gpu_vram_mb);
        printf("| GPU ISA  : OpenCL LDS=%3lld KB/CU | FP64=%-3s (FP32 Auto >= 32M FLOPs)   |\n",
               (long long)g_hw.gpu_lds_kb, g_hw.gpu_fp64 ? "YES" : "NO");
    } else {
        printf("| GPU      : None detected (AVX2/NEON multi-core CPU route active)     |\n");
    }
    printf("| Routes   : L1<=%lldKB -> L2<=%lldKB -> L3<=%lldKB -> RAM -> GPU        |\n",
           (long long)(g_hw.l1d_kb * 3 / 4), (long long)(g_hw.l2_kb * 3 / 4), (long long)(g_hw.l3_kb * 3 / 4));
    printf("| Tile Opt : %lldx%lld f64 BLIS panel (MC=64 KC=256 NC=128 micro=4x8)    |\n",
           (long long)tile64, (long long)tile64);
    printf("+======================================================================+\n");
    fflush(stdout);
    return 1;
}

/* --- Parallel Native Kernels (AVX2+FMA3 4x8 / 4x16 BLIS + Scalar Fallback) --- */

typedef struct {
    const double* A;
    const double* B;
    double* C;
    int64_t M;
    int64_t N;
    int64_t K;
} HwGemmF64Ctx;

#if VORTEX_HW_X86
__attribute__((target("avx2,fma")))
static void hw_matmul_f64_worker_avx2(void* raw_ctx, int64_t mb0, int64_t mb1) {
    HwGemmF64Ctx* ctx = (HwGemmF64Ctx*)raw_ctx;
    const double* A = ctx->A;
    const double* B = ctx->B;
    double* C = ctx->C;
    int64_t M = ctx->M, N = ctx->N, K = ctx->K;

    int64_t row0 = mb0 * 4;
    int64_t row1 = mb1 * 4;
    if (row1 > M) row1 = M;
    if (row0 >= row1) return;

    memset(C + row0 * N, 0, (size_t)(row1 - row0) * (size_t)N * sizeof(double));

    enum { KC = 256, NC = 128, MC = 64 };
    double packB[KC * 8] __attribute__((aligned(32)));

    for (int64_t jc = 0; jc < N; jc += NC) {
        int64_t j_end = (jc + NC < N) ? (jc + NC) : N;
        for (int64_t pc = 0; pc < K; pc += KC) {
            int64_t kb = (pc + KC < K) ? KC : (K - pc);

            for (int64_t j = jc; j < j_end; j += 8) {
                if (j + 8 <= N) {
                    for (int64_t p = 0; p < kb; p++) {
                        const double* bp = &B[(pc + p) * N + j];
                        _mm256_store_pd(&packB[p * 8 + 0], _mm256_loadu_pd(bp + 0));
                        _mm256_store_pd(&packB[p * 8 + 4], _mm256_loadu_pd(bp + 4));
                    }

                    for (int64_t r0 = row0; r0 < row1; r0 += MC) {
                        int64_t r_end = (r0 + MC < row1) ? (r0 + MC) : row1;
                        int64_t i = r0;
                        for (; i + 4 <= r_end; i += 4) {
                            double* c0p = &C[(i + 0) * N + j];
                            double* c1p = &C[(i + 1) * N + j];
                            double* c2p = &C[(i + 2) * N + j];
                            double* c3p = &C[(i + 3) * N + j];

                            __m256d c00 = _mm256_loadu_pd(c0p + 0);
                            __m256d c01 = _mm256_loadu_pd(c0p + 4);
                            __m256d c10 = _mm256_loadu_pd(c1p + 0);
                            __m256d c11 = _mm256_loadu_pd(c1p + 4);
                            __m256d c20 = _mm256_loadu_pd(c2p + 0);
                            __m256d c21 = _mm256_loadu_pd(c2p + 4);
                            __m256d c30 = _mm256_loadu_pd(c3p + 0);
                            __m256d c31 = _mm256_loadu_pd(c3p + 4);

                            const double* a0p = &A[(i + 0) * K + pc];
                            const double* a1p = &A[(i + 1) * K + pc];
                            const double* a2p = &A[(i + 2) * K + pc];
                            const double* a3p = &A[(i + 3) * K + pc];
                            const double* pb = packB;

                            for (int64_t p = 0; p < kb; p++, pb += 8) {
                                __m256d b0 = _mm256_load_pd(pb + 0);
                                __m256d b1 = _mm256_load_pd(pb + 4);

                                __m256d a0 = _mm256_broadcast_sd(a0p + p);
                                c00 = _mm256_fmadd_pd(a0, b0, c00);
                                c01 = _mm256_fmadd_pd(a0, b1, c01);

                                __m256d a1 = _mm256_broadcast_sd(a1p + p);
                                c10 = _mm256_fmadd_pd(a1, b0, c10);
                                c11 = _mm256_fmadd_pd(a1, b1, c11);

                                __m256d a2 = _mm256_broadcast_sd(a2p + p);
                                c20 = _mm256_fmadd_pd(a2, b0, c20);
                                c21 = _mm256_fmadd_pd(a2, b1, c21);

                                __m256d a3 = _mm256_broadcast_sd(a3p + p);
                                c30 = _mm256_fmadd_pd(a3, b0, c30);
                                c31 = _mm256_fmadd_pd(a3, b1, c31);
                            }

                            _mm256_storeu_pd(c0p + 0, c00);
                            _mm256_storeu_pd(c0p + 4, c01);
                            _mm256_storeu_pd(c1p + 0, c10);
                            _mm256_storeu_pd(c1p + 4, c11);
                            _mm256_storeu_pd(c2p + 0, c20);
                            _mm256_storeu_pd(c2p + 4, c21);
                            _mm256_storeu_pd(c3p + 0, c30);
                            _mm256_storeu_pd(c3p + 4, c31);
                        }
                        for (; i < r_end; i++) {
                            double* cp = &C[i * N + j];
                            __m256d c0 = _mm256_loadu_pd(cp + 0);
                            __m256d c1 = _mm256_loadu_pd(cp + 4);
                            const double* ap = &A[i * K + pc];
                            const double* pb = packB;
                            for (int64_t p = 0; p < kb; p++, pb += 8) {
                                __m256d av = _mm256_broadcast_sd(ap + p);
                                c0 = _mm256_fmadd_pd(av, _mm256_load_pd(pb + 0), c0);
                                c1 = _mm256_fmadd_pd(av, _mm256_load_pd(pb + 4), c1);
                            }
                            _mm256_storeu_pd(cp + 0, c0);
                            _mm256_storeu_pd(cp + 4, c1);
                        }
                    }
                } else {
                    int64_t rem_cols = N - j;
                    for (int64_t i = row0; i < row1; i++) {
                        const double* ap = &A[i * K + pc];
                        double* cp = &C[i * N + j];
                        for (int64_t p = 0; p < kb; p++) {
                            double av = ap[p];
                            const double* bp = &B[(pc + p) * N + j];
                            for (int64_t jj = 0; jj < rem_cols; jj++) cp[jj] += av * bp[jj];
                        }
                    }
                }
            }
        }
    }
}
#endif

static void hw_matmul_f64_worker_scalar(void* raw_ctx, int64_t mb0, int64_t mb1) {
    HwGemmF64Ctx* ctx = (HwGemmF64Ctx*)raw_ctx;
    const double* A = ctx->A;
    const double* B = ctx->B;
    double* C = ctx->C;
    int64_t M = ctx->M, N = ctx->N, K = ctx->K;
    int64_t row0 = mb0 * 4;
    int64_t row1 = mb1 * 4;
    if (row1 > M) row1 = M;
    for (int64_t i = row0; i < row1; i++) {
        double* cp = &C[i * N];
        memset(cp, 0, (size_t)N * sizeof(double));
        const double* ap = &A[i * K];
        for (int64_t p = 0; p < K; p++) {
            double av = ap[p];
            const double* bp = &B[p * N];
            for (int64_t j = 0; j < N; j++) cp[j] += av * bp[j];
        }
    }
}

int64_t vortex_hw_matmul_f64(int64_t a_ptr, int64_t b_ptr, int64_t c_ptr, int64_t m, int64_t n, int64_t k) {
    hw_ensure_init();
    if (!a_ptr || !b_ptr || !c_ptr || m <= 0 || n <= 0 || k <= 0) return 0;

    int64_t working = (m * k + k * n + m * n) * 8LL;
    int64_t flops = 2LL * m * n * k;
    int64_t route = vortex_hw_plan_route(working, flops);
    if (route == HW_ROUTE_GPU) route = HW_ROUTE_L3; /* Ryzen 5500 AVX2+FMA3 beats consumer RDNA2 1/16 FP64 */

    HwGemmF64Ctx ctx = {
        (const double*)(uintptr_t)a_ptr,
        (const double*)(uintptr_t)b_ptr,
        (double*)(uintptr_t)c_ptr,
        m, n, k
    };
    int64_t m_blocks = (m + 3) / 4;
    void (*worker_fn)(void*, int64_t, int64_t) = hw_matmul_f64_worker_scalar;
#if VORTEX_HW_X86
    if (g_hw.has_avx2 && g_hw.has_fma3) worker_fn = hw_matmul_f64_worker_avx2;
#endif
    if (flops >= 65536LL && m_blocks >= 2) {
        vortex_parallel_for_dispatch((void*)worker_fn, &ctx, 0, m_blocks);
    } else {
        worker_fn(&ctx, 0, m_blocks);
    }
    return route;
}

typedef struct {
    const float* A;
    const float* B;
    float* C;
    int64_t M;
    int64_t N;
    int64_t K;
} HwGemmF32Ctx;

#if VORTEX_HW_X86
__attribute__((target("avx2,fma")))
static void hw_matmul_f32_worker_avx2(void* raw_ctx, int64_t mb0, int64_t mb1) {
    HwGemmF32Ctx* ctx = (HwGemmF32Ctx*)raw_ctx;
    const float* A = ctx->A;
    const float* B = ctx->B;
    float* C = ctx->C;
    int64_t M = ctx->M, N = ctx->N, K = ctx->K;

    int64_t row0 = mb0 * 4;
    int64_t row1 = mb1 * 4;
    if (row1 > M) row1 = M;
    if (row0 >= row1) return;

    memset(C + row0 * N, 0, (size_t)(row1 - row0) * (size_t)N * sizeof(float));

    enum { KC = 256, NC = 256, MC = 64 };
    float packB[KC * 16] __attribute__((aligned(32)));

    for (int64_t jc = 0; jc < N; jc += NC) {
        int64_t j_end = (jc + NC < N) ? (jc + NC) : N;
        for (int64_t pc = 0; pc < K; pc += KC) {
            int64_t kb = (pc + KC < K) ? KC : (K - pc);
            for (int64_t j = jc; j < j_end; j += 16) {
                if (j + 16 <= N) {
                    for (int64_t p = 0; p < kb; p++) {
                        const float* bp = &B[(pc + p) * N + j];
                        _mm256_store_ps(&packB[p * 16 + 0], _mm256_loadu_ps(bp + 0));
                        _mm256_store_ps(&packB[p * 16 + 8], _mm256_loadu_ps(bp + 8));
                    }
                    for (int64_t r0 = row0; r0 < row1; r0 += MC) {
                        int64_t r_end = (r0 + MC < row1) ? (r0 + MC) : row1;
                        int64_t i = r0;
                        for (; i + 4 <= r_end; i += 4) {
                            float* c0p = &C[(i + 0) * N + j];
                            float* c1p = &C[(i + 1) * N + j];
                            float* c2p = &C[(i + 2) * N + j];
                            float* c3p = &C[(i + 3) * N + j];

                            __m256 c00 = _mm256_loadu_ps(c0p + 0), c01 = _mm256_loadu_ps(c0p + 8);
                            __m256 c10 = _mm256_loadu_ps(c1p + 0), c11 = _mm256_loadu_ps(c1p + 8);
                            __m256 c20 = _mm256_loadu_ps(c2p + 0), c21 = _mm256_loadu_ps(c2p + 8);
                            __m256 c30 = _mm256_loadu_ps(c3p + 0), c31 = _mm256_loadu_ps(c3p + 8);

                            const float* a0p = &A[(i + 0) * K + pc];
                            const float* a1p = &A[(i + 1) * K + pc];
                            const float* a2p = &A[(i + 2) * K + pc];
                            const float* a3p = &A[(i + 3) * K + pc];
                            const float* pb = packB;

                            for (int64_t p = 0; p < kb; p++, pb += 16) {
                                __m256 b0 = _mm256_load_ps(pb + 0);
                                __m256 b1 = _mm256_load_ps(pb + 8);
                                __m256 a0 = _mm256_broadcast_ss(a0p + p);
                                c00 = _mm256_fmadd_ps(a0, b0, c00);
                                c01 = _mm256_fmadd_ps(a0, b1, c01);
                                __m256 a1 = _mm256_broadcast_ss(a1p + p);
                                c10 = _mm256_fmadd_ps(a1, b0, c10);
                                c11 = _mm256_fmadd_ps(a1, b1, c11);
                                __m256 a2 = _mm256_broadcast_ss(a2p + p);
                                c20 = _mm256_fmadd_ps(a2, b0, c20);
                                c21 = _mm256_fmadd_ps(a2, b1, c21);
                                __m256 a3 = _mm256_broadcast_ss(a3p + p);
                                c30 = _mm256_fmadd_ps(a3, b0, c30);
                                c31 = _mm256_fmadd_ps(a3, b1, c31);
                            }

                            _mm256_storeu_ps(c0p + 0, c00); _mm256_storeu_ps(c0p + 8, c01);
                            _mm256_storeu_ps(c1p + 0, c10); _mm256_storeu_ps(c1p + 8, c11);
                            _mm256_storeu_ps(c2p + 0, c20); _mm256_storeu_ps(c2p + 8, c21);
                            _mm256_storeu_ps(c3p + 0, c30); _mm256_storeu_ps(c3p + 8, c31);
                        }
                        for (; i < r_end; i++) {
                            float* cp = &C[i * N + j];
                            __m256 c0 = _mm256_loadu_ps(cp + 0), c1 = _mm256_loadu_ps(cp + 8);
                            const float* ap = &A[i * K + pc];
                            const float* pb = packB;
                            for (int64_t p = 0; p < kb; p++, pb += 16) {
                                __m256 av = _mm256_broadcast_ss(ap + p);
                                c0 = _mm256_fmadd_ps(av, _mm256_load_ps(pb + 0), c0);
                                c1 = _mm256_fmadd_ps(av, _mm256_load_ps(pb + 8), c1);
                            }
                            _mm256_storeu_ps(cp + 0, c0); _mm256_storeu_ps(cp + 8, c1);
                        }
                    }
                } else {
                    int64_t rem_cols = N - j;
                    for (int64_t i = row0; i < row1; i++) {
                        const float* ap = &A[i * K + pc];
                        float* cp = &C[i * N + j];
                        for (int64_t p = 0; p < kb; p++) {
                            float av = ap[p];
                            const float* bp = &B[(pc + p) * N + j];
                            for (int64_t jj = 0; jj < rem_cols; jj++) cp[jj] += av * bp[jj];
                        }
                    }
                }
            }
        }
    }
}
#endif

static void hw_matmul_f32_worker_scalar(void* raw_ctx, int64_t mb0, int64_t mb1) {
    HwGemmF32Ctx* ctx = (HwGemmF32Ctx*)raw_ctx;
    const float* A = ctx->A;
    const float* B = ctx->B;
    float* C = ctx->C;
    int64_t M = ctx->M, N = ctx->N, K = ctx->K;
    int64_t row0 = mb0 * 4;
    int64_t row1 = mb1 * 4;
    if (row1 > M) row1 = M;
    for (int64_t i = row0; i < row1; i++) {
        float* cp = &C[i * N];
        memset(cp, 0, (size_t)N * sizeof(float));
        const float* ap = &A[i * K];
        for (int64_t p = 0; p < K; p++) {
            float av = ap[p];
            const float* bp = &B[p * N];
            for (int64_t j = 0; j < N; j++) cp[j] += av * bp[j];
        }
    }
}

int64_t vortex_hw_matmul_f32_cpu(int64_t a_ptr, int64_t b_ptr, int64_t c_ptr, int64_t m, int64_t n, int64_t k) {
    hw_ensure_init();
    if (!a_ptr || !b_ptr || !c_ptr || m <= 0 || n <= 0 || k <= 0) return 0;

    int64_t working = (m * k + k * n + m * n) * 4LL;
    int64_t flops = 2LL * m * n * k;
    int64_t route = vortex_hw_plan_route(working, 0);

    HwGemmF32Ctx ctx = {
        (const float*)(uintptr_t)a_ptr,
        (const float*)(uintptr_t)b_ptr,
        (float*)(uintptr_t)c_ptr,
        m, n, k
    };
    int64_t m_blocks = (m + 3) / 4;
    void (*worker_fn)(void*, int64_t, int64_t) = hw_matmul_f32_worker_scalar;
#if VORTEX_HW_X86
    if (g_hw.has_avx2 && g_hw.has_fma3) worker_fn = hw_matmul_f32_worker_avx2;
#endif
    if (flops >= 65536LL && m_blocks >= 2) {
        vortex_parallel_for_dispatch((void*)worker_fn, &ctx, 0, m_blocks);
    } else {
        worker_fn(&ctx, 0, m_blocks);
    }
    return route;
}

int64_t vortex_hw_matmul_f32(int64_t a_ptr, int64_t b_ptr, int64_t c_ptr, int64_t m, int64_t n, int64_t k) {
    hw_ensure_init();
    if (!a_ptr || !b_ptr || !c_ptr || m <= 0 || n <= 0 || k <= 0) return 0;
    int64_t working = (m * k + k * n + m * n) * 4LL;
    int64_t flops = 2LL * m * n * k;
    int64_t route = vortex_hw_plan_route(working, flops);
    if (route == HW_ROUTE_GPU) {
        if (hw_gpu_matmul_f32((const float*)(uintptr_t)a_ptr,
                              (const float*)(uintptr_t)b_ptr,
                              (float*)(uintptr_t)c_ptr,
                              (int)m, (int)n, (int)k)) {
            return HW_ROUTE_GPU;
        }
    }
    return vortex_hw_matmul_f32_cpu(a_ptr, b_ptr, c_ptr, m, n, k);
}

/* --- Parallel Vector Dot Product & AXPY --- */

typedef struct {
    const double* a;
    const double* b;
    int64_t n;
    int64_t nw;
    double partials[VORTEX_MAX_THREADS];
} HwDotF64Ctx;

#if VORTEX_HW_X86
__attribute__((target("avx2,fma")))
static double hw_dot_f64_range_avx2(const double* a, const double* b, int64_t len) {
    __m256d s0 = _mm256_setzero_pd();
    __m256d s1 = _mm256_setzero_pd();
    __m256d s2 = _mm256_setzero_pd();
    __m256d s3 = _mm256_setzero_pd();
    int64_t i = 0;
    for (; i + 16 <= len; i += 16) {
        if (i + 64 < len) {
            _mm_prefetch((const char*)(a + i + 64), _MM_HINT_T0);
            _mm_prefetch((const char*)(b + i + 64), _MM_HINT_T0);
        }
        s0 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 0),  _mm256_loadu_pd(b + i + 0),  s0);
        s1 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 4),  _mm256_loadu_pd(b + i + 4),  s1);
        s2 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 8),  _mm256_loadu_pd(b + i + 8),  s2);
        s3 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 12), _mm256_loadu_pd(b + i + 12), s3);
    }
    __m256d sum = _mm256_add_pd(_mm256_add_pd(s0, s1), _mm256_add_pd(s2, s3));
    __m128d hi = _mm256_extractf128_pd(sum, 1);
    __m128d lo = _mm256_castpd256_pd128(sum);
    __m128d p2 = _mm_add_pd(lo, hi);
    __m128d sh = _mm_unpackhi_pd(p2, p2);
    double acc = _mm_cvtsd_f64(_mm_add_sd(p2, sh));
    for (; i < len; i++) acc += a[i] * b[i];
    return acc;
}

__attribute__((target("avx2,fma")))
static void hw_dot_f64_worker_avx2(void* raw_ctx, int64_t w0, int64_t w1) {
    HwDotF64Ctx* ctx = (HwDotF64Ctx*)raw_ctx;
    int64_t i0 = (ctx->n * w0) / ctx->nw;
    int64_t i1 = (ctx->n * w1) / ctx->nw;
    if (w0 >= 0 && w0 < VORTEX_MAX_THREADS) {
        ctx->partials[w0] = hw_dot_f64_range_avx2(ctx->a + i0, ctx->b + i0, i1 - i0);
    }
}
#endif

static void hw_dot_f64_worker_scalar(void* raw_ctx, int64_t w0, int64_t w1) {
    HwDotF64Ctx* ctx = (HwDotF64Ctx*)raw_ctx;
    int64_t i0 = (ctx->n * w0) / ctx->nw;
    int64_t i1 = (ctx->n * w1) / ctx->nw;
    double acc = 0.0;
    for (int64_t i = i0; i < i1; i++) acc += ctx->a[i] * ctx->b[i];
    if (w0 >= 0 && w0 < VORTEX_MAX_THREADS) ctx->partials[w0] = acc;
}

double vortex_hw_dot_f64(int64_t a_ptr, int64_t b_ptr, int64_t n) {
    hw_ensure_init();
    if (!a_ptr || !b_ptr || n <= 0) return 0.0;

    int64_t nw = vortex_num_workers();
    if (nw < 1) nw = 1;
    if (nw > VORTEX_MAX_THREADS) nw = VORTEX_MAX_THREADS;
    if (n < 262144LL || nw == 1) {
#if VORTEX_HW_X86
        if (g_hw.has_avx2 && g_hw.has_fma3) {
            return hw_dot_f64_range_avx2((const double*)(uintptr_t)a_ptr, (const double*)(uintptr_t)b_ptr, n);
        }
#endif
        const double* a = (const double*)(uintptr_t)a_ptr;
        const double* b = (const double*)(uintptr_t)b_ptr;
        double acc = 0.0;
        for (int64_t i = 0; i < n; i++) acc += a[i] * b[i];
        return acc;
    }

    HwDotF64Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.a = (const double*)(uintptr_t)a_ptr;
    ctx.b = (const double*)(uintptr_t)b_ptr;
    ctx.n = n;
    ctx.nw = nw;

    void (*worker_fn)(void*, int64_t, int64_t) = hw_dot_f64_worker_scalar;
#if VORTEX_HW_X86
    if (g_hw.has_avx2 && g_hw.has_fma3) worker_fn = hw_dot_f64_worker_avx2;
#endif
    vortex_parallel_for_dispatch((void*)worker_fn, &ctx, 0, nw);
    double total = 0.0;
    for (int64_t k = 0; k < nw; k++) total += ctx.partials[k];
    return total;
}

typedef struct {
    double alpha;
    const double* x;
    double* y;
} HwAxpyF64Ctx;

#if VORTEX_HW_X86
__attribute__((target("avx2,fma")))
static void hw_axpy_f64_worker_avx2(void* raw_ctx, int64_t start, int64_t end) {
    HwAxpyF64Ctx* ctx = (HwAxpyF64Ctx*)raw_ctx;
    const double* x = ctx->x;
    double* y = ctx->y;
    __m256d va = _mm256_set1_pd(ctx->alpha);
    int64_t i = start;
    for (; i + 16 <= end; i += 16) {
        if (i + 64 < end) {
            _mm_prefetch((const char*)(x + i + 64), _MM_HINT_T0);
            _mm_prefetch((const char*)(y + i + 64), _MM_HINT_T0);
        }
        _mm256_storeu_pd(y + i + 0,  _mm256_fmadd_pd(va, _mm256_loadu_pd(x + i + 0),  _mm256_loadu_pd(y + i + 0)));
        _mm256_storeu_pd(y + i + 4,  _mm256_fmadd_pd(va, _mm256_loadu_pd(x + i + 4),  _mm256_loadu_pd(y + i + 4)));
        _mm256_storeu_pd(y + i + 8,  _mm256_fmadd_pd(va, _mm256_loadu_pd(x + i + 8),  _mm256_loadu_pd(y + i + 8)));
        _mm256_storeu_pd(y + i + 12, _mm256_fmadd_pd(va, _mm256_loadu_pd(x + i + 12), _mm256_loadu_pd(y + i + 12)));
    }
    double a = ctx->alpha;
    for (; i < end; i++) y[i] += a * x[i];
}
#endif

static void hw_axpy_f64_worker_scalar(void* raw_ctx, int64_t start, int64_t end) {
    HwAxpyF64Ctx* ctx = (HwAxpyF64Ctx*)raw_ctx;
    double a = ctx->alpha;
    for (int64_t i = start; i < end; i++) ctx->y[i] += a * ctx->x[i];
}

int64_t vortex_hw_axpy_f64(int64_t alpha_bits, int64_t x_ptr, int64_t y_ptr, int64_t n) {
    hw_ensure_init();
    if (!x_ptr || !y_ptr || n <= 0) return 0;
    double alpha = 0.0;
    memcpy(&alpha, &alpha_bits, sizeof(double));
    HwAxpyF64Ctx ctx = { alpha, (const double*)(uintptr_t)x_ptr, (double*)(uintptr_t)y_ptr };
    void (*worker_fn)(void*, int64_t, int64_t) = hw_axpy_f64_worker_scalar;
#if VORTEX_HW_X86
    if (g_hw.has_avx2 && g_hw.has_fma3) worker_fn = hw_axpy_f64_worker_avx2;
#endif
    if (n >= 131072LL) vortex_parallel_for_dispatch((void*)worker_fn, &ctx, 0, n);
    else worker_fn(&ctx, 0, n);
    return vortex_hw_plan_route(n * 16LL, 2LL * n);
}

typedef struct {
    float alpha;
    const float* x;
    float* y;
} HwAxpyF32Ctx;

#if VORTEX_HW_X86
__attribute__((target("avx2,fma")))
static void hw_axpy_f32_worker_avx2(void* raw_ctx, int64_t start, int64_t end) {
    HwAxpyF32Ctx* ctx = (HwAxpyF32Ctx*)raw_ctx;
    const float* x = ctx->x;
    float* y = ctx->y;
    __m256 va = _mm256_set1_ps(ctx->alpha);
    int64_t i = start;
    for (; i + 32 <= end; i += 32) {
        if (i + 128 < end) {
            _mm_prefetch((const char*)(x + i + 128), _MM_HINT_T0);
            _mm_prefetch((const char*)(y + i + 128), _MM_HINT_T0);
        }
        _mm256_storeu_ps(y + i + 0,  _mm256_fmadd_ps(va, _mm256_loadu_ps(x + i + 0),  _mm256_loadu_ps(y + i + 0)));
        _mm256_storeu_ps(y + i + 8,  _mm256_fmadd_ps(va, _mm256_loadu_ps(x + i + 8),  _mm256_loadu_ps(y + i + 8)));
        _mm256_storeu_ps(y + i + 16, _mm256_fmadd_ps(va, _mm256_loadu_ps(x + i + 16), _mm256_loadu_ps(y + i + 16)));
        _mm256_storeu_ps(y + i + 24, _mm256_fmadd_ps(va, _mm256_loadu_ps(x + i + 24), _mm256_loadu_ps(y + i + 24)));
    }
    float a = ctx->alpha;
    for (; i < end; i++) y[i] += a * x[i];
}
#endif

static void hw_axpy_f32_worker_scalar(void* raw_ctx, int64_t start, int64_t end) {
    HwAxpyF32Ctx* ctx = (HwAxpyF32Ctx*)raw_ctx;
    float a = ctx->alpha;
    for (int64_t i = start; i < end; i++) ctx->y[i] += a * ctx->x[i];
}

int64_t vortex_hw_axpy_f32(int64_t alpha_bits, int64_t x_ptr, int64_t y_ptr, int64_t n) {
    hw_ensure_init();
    if (!x_ptr || !y_ptr || n <= 0) return 0;
    uint32_t u32 = (uint32_t)(uint64_t)alpha_bits;
    float alpha = 0.0f;
    memcpy(&alpha, &u32, sizeof(float));
    HwAxpyF32Ctx ctx = { alpha, (const float*)(uintptr_t)x_ptr, (float*)(uintptr_t)y_ptr };
    void (*worker_fn)(void*, int64_t, int64_t) = hw_axpy_f32_worker_scalar;
#if VORTEX_HW_X86
    if (g_hw.has_avx2 && g_hw.has_fma3) worker_fn = hw_axpy_f32_worker_avx2;
#endif
    if (n >= 262144LL) vortex_parallel_for_dispatch((void*)worker_fn, &ctx, 0, n);
    else worker_fn(&ctx, 0, n);
    return vortex_hw_plan_route(n * 8LL, 2LL * n);
}
