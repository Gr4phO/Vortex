/*
 * VORTEX HARDWARE SILICON PROFILER, ADAPTIVE WAZE ROUTER & AVX2/FMA3 + OPENCL GPU ENGINE
 * - Probes host CPU topology, L1/L2/L3 caches, SIMD ISA, RAM, and OpenCL GPU dynamically.
 * - Provides resident RAM/VRAM HwBuf buffers tied to Arena scopes (zero redundant PCIe).
 * - Disk-cached OpenCL binaries + 64x64 4x4 register-blocked float4 GEMM / AXPY / ReLU kernels.
 * - Adaptive EMA ("Waze") latency router with PCIe dirty-transfer cost & congestion fallback.
 */

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#else
#include <sys/stat.h>
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

/* HwBuf residency bitflags (matches std/hw.vx) */
#define HW_BUF_HOST_DIRTY    1
#define HW_BUF_DEV_DIRTY     2
#define HW_BUF_IN_VRAM       4

#define HW_BUF_MAGIC         0x56584857U
#define HW_MAX_BUFS          1024

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
#define VX_CL_DEVICE_DRIVER_VERSION      0x102D
#define VX_CL_DEVICE_DOUBLE_FP_CONFIG    0x1032
#define VX_CL_PROGRAM_BINARY_SIZES       0x1165
#define VX_CL_PROGRAM_BINARIES           0x1166
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
typedef vx_cl_program (VX_CL_CALL *pfn_clCreateProgramWithBinary)(vx_cl_context, vx_cl_uint, const vx_cl_device_id*, const size_t*, const unsigned char**, vx_cl_int*, vx_cl_int*);
typedef vx_cl_int (VX_CL_CALL *pfn_clGetProgramInfo)(vx_cl_program, vx_cl_uint, size_t, void*, size_t*);
typedef vx_cl_int (VX_CL_CALL *pfn_clReleaseProgram)(vx_cl_program);
typedef vx_cl_int (VX_CL_CALL *pfn_clBuildProgram)(vx_cl_program, vx_cl_uint, const vx_cl_device_id*, const char*, void (VX_CL_CALL *)(vx_cl_program, void*), void*);
typedef vx_cl_kernel (VX_CL_CALL *pfn_clCreateKernel)(vx_cl_program, const char*, vx_cl_int*);
typedef vx_cl_mem (VX_CL_CALL *pfn_clCreateBuffer)(vx_cl_context, vx_cl_bitfield, size_t, void*, vx_cl_int*);
typedef vx_cl_int (VX_CL_CALL *pfn_clEnqueueWriteBuffer)(vx_cl_command_queue, vx_cl_mem, vx_cl_uint, size_t, size_t, const void*, vx_cl_uint, const void*, void*);
typedef vx_cl_int (VX_CL_CALL *pfn_clEnqueueReadBuffer)(vx_cl_command_queue, vx_cl_mem, vx_cl_uint, size_t, size_t, void*, vx_cl_uint, const void*, void*);
typedef vx_cl_int (VX_CL_CALL *pfn_clSetKernelArg)(vx_cl_kernel, vx_cl_uint, size_t, const void*);
typedef vx_cl_int (VX_CL_CALL *pfn_clEnqueueNDRangeKernel)(vx_cl_command_queue, vx_cl_kernel, vx_cl_uint, const size_t*, const size_t*, const size_t*, vx_cl_uint, const void*, void*);
typedef vx_cl_int (VX_CL_CALL *pfn_clFinish)(vx_cl_command_queue);
typedef vx_cl_int (VX_CL_CALL *pfn_clReleaseMemObject)(vx_cl_mem);

/* --- Adaptive "Waze" Telemetry Router --- */
enum {
    HW_OP_GEMM_F32 = 0,
    HW_OP_GEMM_F64 = 1,
    HW_OP_AXPY_F32 = 2,
    HW_OP_RELU_F32 = 3,
    HW_OP_COUNT    = 4,
    HW_BUCKETS     = 6
};

typedef struct {
    double ema_us[HW_OP_COUNT][HW_BUCKETS][2];   /* [op][size_bucket][0=CPU, 1=GPU] */
    uint32_t samples[HW_OP_COUNT][HW_BUCKETS][2];
    double pcie_gbps;
    int64_t gpu_cooldown_until_us;
    int64_t gpu_congestion_trips;
    int64_t pcie_uploads;
    int64_t pcie_downloads;
    int64_t pcie_bytes_up;
    int64_t pcie_bytes_down;
    int64_t cl_cache_hits;
    int64_t cl_cache_misses;
} HwAdaptiveRouter;

/* --- StarPU-style Resident RAM/VRAM Buffer Handle --- */
typedef struct {
    uint32_t magic;
    uint8_t active;
    uint8_t host_dirty;
    uint8_t dev_dirty;
    uint8_t elem_size;
    uint32_t scope_depth;
    uint64_t scope_epoch;
    void* owner_arena;
    void* host_ptr;
    vx_cl_mem cl_mem;
    size_t bytes;
    int64_t elem_count;
} VortexHwBuf;

typedef struct {
    bool cpu_initialized;
    bool gpu_probed;
    bool initialized;
    char cpu_name[128];
    char gpu_name[128];
    char gpu_driver[64];
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
    vx_cl_kernel k_matmul_f32_reg64;
    vx_cl_kernel k_axpy_f32;
    vx_cl_kernel k_relu_f32;
    vx_cl_kernel k_matmul_f64;

    pfn_clGetPlatformIDs p_get_platforms;
    pfn_clGetDeviceIDs p_get_devices;
    pfn_clGetDeviceInfo p_get_dev_info;
    pfn_clCreateContext p_create_ctx;
    pfn_clCreateCommandQueue p_create_queue;
    pfn_clCreateProgramWithSource p_create_prog;
    pfn_clCreateProgramWithBinary p_create_prog_bin;
    pfn_clGetProgramInfo p_get_prog_info;
    pfn_clReleaseProgram p_rel_prog;
    pfn_clBuildProgram p_build_prog;
    pfn_clCreateKernel p_create_kernel;
    pfn_clCreateBuffer p_create_buf;
    pfn_clEnqueueWriteBuffer p_write_buf;
    pfn_clEnqueueReadBuffer p_read_buf;
    pfn_clSetKernelArg p_set_arg;
    pfn_clEnqueueNDRangeKernel p_ndrange;
    pfn_clFinish p_finish;
    pfn_clReleaseMemObject p_rel_mem;

    HwAdaptiveRouter router;
} VortexHwProfile;

static VortexHwProfile g_hw;
static VortexHwBuf g_hw_buf_pool[HW_MAX_BUFS];
static int g_hw_active_buf_count = 0;

static int64_t hw_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return (int64_t)((cnt.QuadPart * 1000000LL) / (freq.QuadPart ? freq.QuadPart : 1));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000LL);
#endif
}

static void hw_trim_spaces(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

static uint64_t hw_fnv1a(uint64_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
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

#define HW_LOAD_CL_OPT(field, fn_name) do { \
    void* _sym = hw_dl_sym(lib, fn_name); \
    if (_sym) memcpy(&g_hw.field, &_sym, sizeof(_sym)); \
} while (0)

static bool hw_get_ocl_cache_dir(char* out, size_t cap) {
    if (!out || cap < 64) return false;
#ifdef _WIN32
    const char* root = getenv("LOCALAPPDATA");
    if (!root || !root[0]) root = getenv("TEMP");
    if (!root || !root[0]) return false;
    snprintf(out, cap, "%s\\Vortex", root);
    CreateDirectoryA(out, NULL);
    snprintf(out, cap, "%s\\Vortex\\ocl_cache", root);
    CreateDirectoryA(out, NULL);
#else
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, cap, "%s/.cache", home);
    mkdir(out, 0755);
    snprintf(out, cap, "%s/.cache/vortex", home);
    mkdir(out, 0755);
    snprintf(out, cap, "%s/.cache/vortex/ocl_cache", home);
    mkdir(out, 0755);
#endif
    return true;
}

static vx_cl_program hw_build_or_load_program(const char* tag, const char* src, const char* build_opts) {
    size_t src_len = strlen(src);
    size_t opt_len = build_opts ? strlen(build_opts) : 0;
    uint64_t h = 14695981039346656037ULL;
    h = hw_fnv1a(h, tag, strlen(tag));
    h = hw_fnv1a(h, g_hw.gpu_name, strlen(g_hw.gpu_name));
    h = hw_fnv1a(h, g_hw.gpu_driver, strlen(g_hw.gpu_driver));
    if (opt_len > 0) h = hw_fnv1a(h, build_opts, opt_len);
    h = hw_fnv1a(h, src, src_len);

    char cache_dir[512];
    char bin_path[640];
    bool have_cache = hw_get_ocl_cache_dir(cache_dir, sizeof(cache_dir));
    if (have_cache) {
#ifdef _WIN32
        snprintf(bin_path, sizeof(bin_path), "%s\\%s_%016llx.bin", cache_dir, tag, (unsigned long long)h);
#else
        snprintf(bin_path, sizeof(bin_path), "%s/%s_%016llx.bin", cache_dir, tag, (unsigned long long)h);
#endif
    }

    /* Try cached OpenCL binary first for near-instant startup */
    if (have_cache && g_hw.p_create_prog_bin) {
        FILE* f = fopen(bin_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsz > 0 && fsz < (32L * 1024L * 1024L)) {
                size_t bsz = (size_t)fsz;
                unsigned char* buf = (unsigned char*)malloc(bsz);
                if (buf && fread(buf, 1, bsz, f) == bsz) {
                    const unsigned char* bptr = buf;
                    vx_cl_int bin_status = -1;
                    vx_cl_int err = 0;
                    vx_cl_program prog = g_hw.p_create_prog_bin(g_hw.cl_ctx, 1, &g_hw.cl_dev, &bsz, &bptr, &bin_status, &err);
                    if (err == VX_CL_SUCCESS && bin_status == VX_CL_SUCCESS && prog) {
                        if (g_hw.p_build_prog(prog, 1, &g_hw.cl_dev, build_opts ? build_opts : "", NULL, NULL) == VX_CL_SUCCESS) {
                            free(buf);
                            fclose(f);
                            g_hw.router.cl_cache_hits++;
                            return prog;
                        }
                        if (g_hw.p_rel_prog) g_hw.p_rel_prog(prog);
                    }
                }
                free(buf);
            }
            fclose(f);
        }
    }

    /* Cache miss: compile OpenCL C source and save binary to disk */
    vx_cl_int err = 0;
    vx_cl_program prog = g_hw.p_create_prog(g_hw.cl_ctx, 1, &src, &src_len, &err);
    if (err != VX_CL_SUCCESS || !prog) return NULL;
    if (g_hw.p_build_prog(prog, 1, &g_hw.cl_dev, build_opts ? build_opts : "", NULL, NULL) != VX_CL_SUCCESS) {
        if (g_hw.p_rel_prog) g_hw.p_rel_prog(prog);
        return NULL;
    }
    g_hw.router.cl_cache_misses++;

    if (have_cache && g_hw.p_get_prog_info) {
        size_t bsz = 0;
        if (g_hw.p_get_prog_info(prog, VX_CL_PROGRAM_BINARY_SIZES, sizeof(bsz), &bsz, NULL) == VX_CL_SUCCESS &&
            bsz > 0 && bsz < (32ULL * 1024ULL * 1024ULL)) {
            unsigned char* buf = (unsigned char*)malloc(bsz);
            if (buf) {
                unsigned char* ptrs[1] = { buf };
                if (g_hw.p_get_prog_info(prog, VX_CL_PROGRAM_BINARIES, sizeof(ptrs), ptrs, NULL) == VX_CL_SUCCESS) {
                    char tmp_path[656];
                    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", bin_path);
                    FILE* wf = fopen(tmp_path, "wb");
                    if (wf) {
                        size_t wr = fwrite(buf, 1, bsz, wf);
                        fclose(wf);
                        if (wr == bsz) {
                            remove(bin_path);
                            rename(tmp_path, bin_path);
                        } else {
                            remove(tmp_path);
                        }
                    }
                }
                free(buf);
            }
        }
    }
    return prog;
}

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
    HW_LOAD_CL_OPT(p_create_prog_bin, "clCreateProgramWithBinary");
    HW_LOAD_CL_OPT(p_get_prog_info,   "clGetProgramInfo");
    HW_LOAD_CL_OPT(p_rel_prog,        "clReleaseProgram");
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
    g_hw.p_get_dev_info(best_dev, VX_CL_DEVICE_DRIVER_VERSION, sizeof(g_hw.gpu_driver) - 1, g_hw.gpu_driver, NULL);
    hw_trim_spaces(g_hw.gpu_driver);

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
        "}\n"
        "\n"
        "__kernel void matmul_f32_reg64(__global const float* __restrict__ A,\n"
        "                               __global const float* __restrict__ B,\n"
        "                               __global float* __restrict__ C,\n"
        "                               int M, int N, int K) {\n"
        "    int lr = get_local_id(1);\n"
        "    int lc = get_local_id(0);\n"
        "    int tid = (lr << 4) | lc;\n"
        "    int row_base = get_group_id(1) * 64;\n"
        "    int col_base = get_group_id(0) * 64;\n"
        "    __local float As[16][68];\n"
        "    __local float Bs[16][68];\n"
        "    float4 c0 = (float4)(0.0f);\n"
        "    float4 c1 = (float4)(0.0f);\n"
        "    float4 c2 = (float4)(0.0f);\n"
        "    float4 c3 = (float4)(0.0f);\n"
        "    int a_row = tid >> 2;\n"
        "    int a_col4 = (tid & 3) * 4;\n"
        "    int b_row = tid >> 4;\n"
        "    int b_col4 = (tid & 15) * 4;\n"
        "    for (int t = 0; t < K; t += 16) {\n"
        "        float4 va = vload4(0, &A[(row_base + a_row) * K + (t + a_col4)]);\n"
        "        As[a_col4 + 0][a_row] = va.x;\n"
        "        As[a_col4 + 1][a_row] = va.y;\n"
        "        As[a_col4 + 2][a_row] = va.z;\n"
        "        As[a_col4 + 3][a_row] = va.w;\n"
        "        float4 vb = vload4(0, &B[(t + b_row) * N + (col_base + b_col4)]);\n"
        "        vstore4(vb, 0, &Bs[b_row][b_col4]);\n"
        "        barrier(CLK_LOCAL_MEM_FENCE);\n"
        "        #pragma unroll\n"
        "        for (int p = 0; p < 16; p++) {\n"
        "            float4 a4 = vload4(0, &As[p][lr * 4]);\n"
        "            float4 b4 = vload4(0, &Bs[p][lc * 4]);\n"
        "            c0 = fma((float4)(a4.x), b4, c0);\n"
        "            c1 = fma((float4)(a4.y), b4, c1);\n"
        "            c2 = fma((float4)(a4.z), b4, c2);\n"
        "            c3 = fma((float4)(a4.w), b4, c3);\n"
        "        }\n"
        "        barrier(CLK_LOCAL_MEM_FENCE);\n"
        "    }\n"
        "    int r0 = row_base + lr * 4;\n"
        "    int c_off = col_base + lc * 4;\n"
        "    vstore4(c0, 0, &C[(r0 + 0) * N + c_off]);\n"
        "    vstore4(c1, 0, &C[(r0 + 1) * N + c_off]);\n"
        "    vstore4(c2, 0, &C[(r0 + 2) * N + c_off]);\n"
        "    vstore4(c3, 0, &C[(r0 + 3) * N + c_off]);\n"
        "}\n"
        "\n"
        "__kernel void axpy_f32(float alpha, __global const float* X, __global float* Y, int N) {\n"
        "    int i = get_global_id(0);\n"
        "    if (i < N) Y[i] = fma(alpha, X[i], Y[i]);\n"
        "}\n"
        "\n"
        "__kernel void relu_f32(__global float* X, int N) {\n"
        "    int i = get_global_id(0);\n"
        "    if (i < N) X[i] = fmax(X[i], 0.0f);\n"
        "}\n";

    g_hw.cl_prog_f32 = hw_build_or_load_program("vx_f32_v2", cl_src_f32, "-cl-fast-relaxed-math");
    if (g_hw.cl_prog_f32) {
        g_hw.k_matmul_f32       = g_hw.p_create_kernel(g_hw.cl_prog_f32, "matmul_f32", &err);
        if (err != VX_CL_SUCCESS) g_hw.k_matmul_f32 = NULL;
        g_hw.k_matmul_f32_reg64 = g_hw.p_create_kernel(g_hw.cl_prog_f32, "matmul_f32_reg64", &err);
        if (err != VX_CL_SUCCESS) g_hw.k_matmul_f32_reg64 = NULL;
        g_hw.k_axpy_f32         = g_hw.p_create_kernel(g_hw.cl_prog_f32, "axpy_f32", &err);
        if (err != VX_CL_SUCCESS) g_hw.k_axpy_f32 = NULL;
        g_hw.k_relu_f32         = g_hw.p_create_kernel(g_hw.cl_prog_f32, "relu_f32", &err);
        if (err != VX_CL_SUCCESS) g_hw.k_relu_f32 = NULL;
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
        g_hw.cl_prog_f64 = hw_build_or_load_program("vx_f64_v1", cl_src_f64, "");
        if (g_hw.cl_prog_f64) {
            g_hw.k_matmul_f64 = g_hw.p_create_kernel(g_hw.cl_prog_f64, "matmul_f64", &err);
            if (err != VX_CL_SUCCESS) g_hw.k_matmul_f64 = NULL;
        }
    }
    return g_hw.k_matmul_f32 != NULL;
}

/* --- Adaptive "Waze" Cost Model & Telemetry --- */

static int hw_flops_bucket(int64_t flops) {
    if (flops < 262144LL) return 0;         /* < 256K */
    if (flops < 2097152LL) return 1;        /* < 2M */
    if (flops < 16777216LL) return 2;       /* < 16M */
    if (flops < 134217728LL) return 3;      /* < 128M */
    if (flops < 1073741824LL) return 4;     /* < 1G */
    return 5;                               /* >= 1G */
}

static double hw_prior_compute_us(int op, int64_t flops, bool is_gpu) {
    if (!is_gpu) {
        double cores = (double)(g_hw.phys_cores > 0 ? g_hw.phys_cores : 1);
        double cpu_gf = (op == HW_OP_GEMM_F64)
                      ? (g_hw.has_avx2 && g_hw.has_fma3 ? cores * 26.0 : cores * 3.0)
                      : ((g_hw.has_avx2 && g_hw.has_fma3) ? cores * 52.0 : cores * 5.0);
        if (op == HW_OP_AXPY_F32 || op == HW_OP_RELU_F32) cpu_gf = cores * 6.0; /* memory-bound */
        return ((double)flops / (cpu_gf * 1e9)) * 1e6 + 15.0;
    }
    double cus = (double)(g_hw.gpu_cus > 0 ? g_hw.gpu_cus : 4);
    double gpu_gf = (op == HW_OP_GEMM_F64)
                  ? (g_hw.gpu_fp64 ? cus * 12.0 : 1.0)
                  : ((op == HW_OP_GEMM_F32) ? cus * 180.0 : cus * 35.0);
    return ((double)flops / (gpu_gf * 1e9)) * 1e6 + 60.0;
}

static bool hw_router_should_use_gpu(int op, int64_t flops, size_t dirty_up, size_t dirty_down, bool resident_hint) {
    if (!g_hw.has_gpu) return false;
    if (op == HW_OP_GEMM_F64 && !g_hw.gpu_fp64) return false;

    int64_t now = hw_now_us();
    if (now < g_hw.router.gpu_cooldown_until_us) {
        return false; /* Congestion cooldown active -> divert to CPU BLIS */
    }

    int b = hw_flops_bucket(flops);
    double cpu_us = g_hw.router.samples[op][b][0] > 0
                  ? g_hw.router.ema_us[op][b][0]
                  : hw_prior_compute_us(op, flops, false);
    double gpu_us = g_hw.router.samples[op][b][1] > 0
                  ? g_hw.router.ema_us[op][b][1]
                  : hw_prior_compute_us(op, flops, true);

    double bw_gbps = (g_hw.router.pcie_gbps > 0.5) ? g_hw.router.pcie_gbps : 6.0;
    double xfer_bytes = (double)(dirty_up + dirty_down);
    double pcie_us = (xfer_bytes > 0.0) ? ((xfer_bytes / (bw_gbps * 1e9)) * 1e6 + 65.0) : 0.0;

    if (resident_hint && (op == HW_OP_AXPY_F32 || op == HW_OP_RELU_F32)) {
        return (gpu_us + pcie_us) <= (cpu_us + 250.0);
    }
    return (gpu_us + pcie_us) < cpu_us;
}

static void hw_router_record(int op, int bucket, bool is_gpu, double elapsed_us) {
    int dev = is_gpu ? 1 : 0;
    uint32_t n = g_hw.router.samples[op][bucket][dev];
    double prev = g_hw.router.ema_us[op][bucket][dev];

    if (is_gpu && n >= 3 && elapsed_us > prev * 2.5 && elapsed_us > 8000.0) {
        g_hw.router.gpu_cooldown_until_us = hw_now_us() + 5000000LL; /* 5s congestion bypass */
        g_hw.router.gpu_congestion_trips++;
    }
    if (n == 0) {
        g_hw.router.ema_us[op][bucket][dev] = elapsed_us;
    } else {
        g_hw.router.ema_us[op][bucket][dev] = 0.75 * prev + 0.25 * elapsed_us;
    }
    g_hw.router.samples[op][bucket][dev] = n + 1;
}

static bool hw_dispatch_gpu_matmul_f32_mems(vx_cl_mem dA, vx_cl_mem dB, vx_cl_mem dC, int M, int N, int K) {
    vx_cl_kernel kern = g_hw.k_matmul_f32;
    size_t local[2] = { 16, 16 };
    size_t global[2];
    if (g_hw.k_matmul_f32_reg64 && M >= 64 && N >= 64 && K >= 16 &&
        (M % 64) == 0 && (N % 64) == 0 && (K % 16) == 0) {
        kern = g_hw.k_matmul_f32_reg64;
        global[0] = (size_t)((N / 64) * 16);
        global[1] = (size_t)((M / 64) * 16);
    } else {
        global[0] = (size_t)(((N + 15) / 16) * 16);
        global[1] = (size_t)(((M + 15) / 16) * 16);
    }

    vx_cl_int err = 0;
    err |= g_hw.p_set_arg(kern, 0, sizeof(vx_cl_mem), &dA);
    err |= g_hw.p_set_arg(kern, 1, sizeof(vx_cl_mem), &dB);
    err |= g_hw.p_set_arg(kern, 2, sizeof(vx_cl_mem), &dC);
    err |= g_hw.p_set_arg(kern, 3, sizeof(int), &M);
    err |= g_hw.p_set_arg(kern, 4, sizeof(int), &N);
    err |= g_hw.p_set_arg(kern, 5, sizeof(int), &K);
    if (err == VX_CL_SUCCESS) {
        err = g_hw.p_ndrange(g_hw.cl_queue, kern, 2, NULL, global, local, 0, NULL, NULL);
    }
    if (err == VX_CL_SUCCESS) {
        err = g_hw.p_finish(g_hw.cl_queue);
    }
    return err == VX_CL_SUCCESS;
}

static bool hw_gpu_matmul_f32(const float* A, const float* B, float* C, int M, int N, int K) {
    if (!hw_ensure_opencl_kernels() || !g_hw.k_matmul_f32) return false;
    vx_cl_int err = 0;
    size_t sz_a = (size_t)M * (size_t)K * sizeof(float);
    size_t sz_b = (size_t)K * (size_t)N * sizeof(float);
    size_t sz_c = (size_t)M * (size_t)N * sizeof(float);

    int64_t t0 = hw_now_us();
    vx_cl_mem dA = g_hw.p_create_buf(g_hw.cl_ctx, VX_CL_MEM_READ_ONLY | VX_CL_MEM_COPY_HOST_PTR, sz_a, (void*)A, &err);
    if (err != VX_CL_SUCCESS || !dA) return false;
    vx_cl_mem dB = g_hw.p_create_buf(g_hw.cl_ctx, VX_CL_MEM_READ_ONLY | VX_CL_MEM_COPY_HOST_PTR, sz_b, (void*)B, &err);
    if (err != VX_CL_SUCCESS || !dB) { g_hw.p_rel_mem(dA); return false; }
    vx_cl_mem dC = g_hw.p_create_buf(g_hw.cl_ctx, VX_CL_MEM_WRITE_ONLY, sz_c, NULL, &err);
    if (err != VX_CL_SUCCESS || !dC) { g_hw.p_rel_mem(dA); g_hw.p_rel_mem(dB); return false; }

    g_hw.router.pcie_uploads += 2;
    g_hw.router.pcie_bytes_up += (int64_t)(sz_a + sz_b);
    int64_t t_up = hw_now_us() - t0;
    if (t_up > 0) {
        double gbps = ((double)(sz_a + sz_b) / ((double)t_up * 1000.0));
        if (gbps > 0.2 && gbps < 64.0) g_hw.router.pcie_gbps = 0.7 * g_hw.router.pcie_gbps + 0.3 * gbps;
    }

    int64_t tk0 = hw_now_us();
    bool ok = hw_dispatch_gpu_matmul_f32_mems(dA, dB, dC, M, N, K);
    int64_t tk_dt = hw_now_us() - tk0;
    if (ok && tk_dt > 0) {
        int64_t flops = 2LL * (int64_t)M * (int64_t)N * (int64_t)K;
        hw_router_record(HW_OP_GEMM_F32, hw_flops_bucket(flops), true, (double)tk_dt);
    }
    if (ok) {
        err = g_hw.p_read_buf(g_hw.cl_queue, dC, VX_CL_TRUE, 0, sz_c, C, 0, NULL, NULL);
        ok = (err == VX_CL_SUCCESS);
        if (ok) {
            g_hw.router.pcie_downloads++;
            g_hw.router.pcie_bytes_down += (int64_t)sz_c;
        }
    }
    g_hw.p_rel_mem(dA);
    g_hw.p_rel_mem(dB);
    g_hw.p_rel_mem(dC);
    return ok;
}

/* --- CPU & Topology Probe --- */

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

void vortex_hw_init_cpu_only(void) {
    if (g_hw.cpu_initialized) return;
    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.router.pcie_gbps = 6.0;
    hw_probe_cpu();
    g_hw.cpu_initialized = true;
}

int64_t vortex_hw_get_phys_cores(void) {
    vortex_hw_init_cpu_only();
    return g_hw.phys_cores;
}

uint64_t vortex_hw_get_core_mask(int core_idx) {
    vortex_hw_init_cpu_only();
    if (core_idx < 0 || core_idx >= VORTEX_MAX_THREADS) return 0;
    return g_hw.core_masks[core_idx];
}

int64_t vortex_hw_init(void) {
    if (!g_hw.cpu_initialized) vortex_hw_init_cpu_only();
    if (!g_hw.gpu_probed) {
        hw_probe_opencl();
        g_hw.gpu_probed = true;
    }
    g_hw.initialized = true;
    return 1;
}

static inline void hw_ensure_init(void) {
    if (!g_hw.initialized) vortex_hw_init();
}

int64_t vortex_hw_cpu_name(void) {
    vortex_hw_init_cpu_only();
    return (int64_t)(uintptr_t)vortex_intern_cstr(g_hw.cpu_name);
}

int64_t vortex_hw_gpu_name(void) {
    hw_ensure_init();
    return (int64_t)(uintptr_t)vortex_intern_cstr(g_hw.gpu_name);
}

int64_t vortex_hw_l1_kb(void) { vortex_hw_init_cpu_only(); return g_hw.l1d_kb; }
int64_t vortex_hw_l2_kb(void) { vortex_hw_init_cpu_only(); return g_hw.l2_kb; }
int64_t vortex_hw_l3_kb(void) { vortex_hw_init_cpu_only(); return g_hw.l3_kb; }
int64_t vortex_hw_phys_cores(void) { vortex_hw_init_cpu_only(); return g_hw.phys_cores; }
int64_t vortex_hw_logical_cores(void) { vortex_hw_init_cpu_only(); return g_hw.logical_cores; }
int64_t vortex_hw_simd_width(void) { vortex_hw_init_cpu_only(); return g_hw.simd_width; }
int64_t vortex_hw_has_gpu(void) { hw_ensure_init(); return g_hw.has_gpu ? 1 : 0; }

int64_t vortex_hw_plan_route(int64_t working_bytes, int64_t flops) {
    hw_ensure_init();
    size_t est_bytes = (working_bytes > 0) ? (size_t)working_bytes : 0;
    if (flops > 0 && hw_router_should_use_gpu(HW_OP_GEMM_F32, flops, est_bytes, est_bytes / 3, false)) {
        return HW_ROUTE_GPU;
    }
    if (working_bytes <= g_hw.l1d_kb * 1024LL * 3 / 4) return HW_ROUTE_L1;
    if (working_bytes <= g_hw.l2_kb * 1024LL * 3 / 4) return HW_ROUTE_L2;
    if (working_bytes <= g_hw.l3_kb * 1024LL * 3 / 4) return HW_ROUTE_L3;
    return HW_ROUTE_RAM_STREAM;
}

int64_t vortex_hw_plan_tile_f64(void) {
    vortex_hw_init_cpu_only();
    int64_t l2_bytes = g_hw.l2_kb * 1024LL / 2;
    int64_t max_elems = l2_bytes / (3 * 8);
    int64_t t = 32;
    while ((t + 8) * (t + 8) <= max_elems && (t + 8) <= 128) t += 8;
    return t;
}

int64_t vortex_hw_pin_workers(int64_t mode) {
    vortex_hw_init_cpu_only();
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

int64_t vortex_hw_gpu_cache_hits(void) {
    return g_hw.router.cl_cache_hits;
}

int64_t vortex_hw_router_stats(void) {
    hw_ensure_init();
    printf("[HW ROUTER] GPU=%s | cache hits=%lld misses=%lld | PCIe up=%lld (%lld KB) down=%lld (%lld KB) | congestion_trips=%lld | active_vram_bufs=%d\n",
           g_hw.has_gpu ? g_hw.gpu_name : "None",
           (long long)g_hw.router.cl_cache_hits,
           (long long)g_hw.router.cl_cache_misses,
           (long long)g_hw.router.pcie_uploads,
           (long long)(g_hw.router.pcie_bytes_up / 1024LL),
           (long long)g_hw.router.pcie_downloads,
           (long long)(g_hw.router.pcie_bytes_down / 1024LL),
           (long long)g_hw.router.gpu_congestion_trips,
           g_hw_active_buf_count);
    fflush(stdout);
    return g_hw.router.pcie_uploads + g_hw.router.pcie_downloads;
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
        printf("| GPU ISA  : OpenCL LDS=%3lld KB/CU | FP64=%-3s | 64x64 float4 + DiskCache |\n",
               (long long)g_hw.gpu_lds_kb, g_hw.gpu_fp64 ? "YES" : "NO");
    } else {
        printf("| GPU      : None detected (AVX2/NEON multi-core CPU route active)     |\n");
    }
    printf("| Routes   : Adaptive Waze EMA (L1<=%lldK L2<=%lldK L3<=%lldK RAM GPU)    |\n",
           (long long)(g_hw.l1d_kb * 3 / 4), (long long)(g_hw.l2_kb * 3 / 4), (long long)(g_hw.l3_kb * 3 / 4));
    printf("| Tile Opt : %lldx%lld f64 BLIS panel | HwBuf VRAM Residency + ScopeGC   |\n",
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
    int64_t route = vortex_hw_plan_route(working, 0);

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
    int64_t t0 = hw_now_us();
    if (flops >= 65536LL && m_blocks >= 2) {
        vortex_parallel_for_dispatch((void*)worker_fn, &ctx, 0, m_blocks);
    } else {
        worker_fn(&ctx, 0, m_blocks);
    }
    hw_router_record(HW_OP_GEMM_F64, hw_flops_bucket(flops), false, (double)(hw_now_us() - t0));
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
    int64_t t0 = hw_now_us();
    if (flops >= 65536LL && m_blocks >= 2) {
        vortex_parallel_for_dispatch((void*)worker_fn, &ctx, 0, m_blocks);
    } else {
        worker_fn(&ctx, 0, m_blocks);
    }
    hw_router_record(HW_OP_GEMM_F32, hw_flops_bucket(flops), false, (double)(hw_now_us() - t0));
    return route;
}

int64_t vortex_hw_matmul_f32(int64_t a_ptr, int64_t b_ptr, int64_t c_ptr, int64_t m, int64_t n, int64_t k) {
    hw_ensure_init();
    if (!a_ptr || !b_ptr || !c_ptr || m <= 0 || n <= 0 || k <= 0) return 0;
    size_t sz_up = ((size_t)m * (size_t)k + (size_t)k * (size_t)n) * sizeof(float);
    size_t sz_dn = (size_t)m * (size_t)n * sizeof(float);
    int64_t flops = 2LL * m * n * k;
    if (hw_router_should_use_gpu(HW_OP_GEMM_F32, flops, sz_up, sz_dn, false)) {
        if (hw_gpu_matmul_f32((const float*)(uintptr_t)a_ptr,
                              (const float*)(uintptr_t)b_ptr,
                              (float*)(uintptr_t)c_ptr,
                              (int)m, (int)n, (int)k)) {
            return HW_ROUTE_GPU;
        }
    }
    return vortex_hw_matmul_f32_cpu(a_ptr, b_ptr, c_ptr, m, n, k);
}

/* --- Parallel Vector Dot Product, AXPY & ReLU --- */

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

static int64_t hw_axpy_f32_cpu_internal(float alpha, const float* x, float* y, int64_t n) {
    HwAxpyF32Ctx ctx = { alpha, x, y };
    void (*worker_fn)(void*, int64_t, int64_t) = hw_axpy_f32_worker_scalar;
#if VORTEX_HW_X86
    if (g_hw.has_avx2 && g_hw.has_fma3) worker_fn = hw_axpy_f32_worker_avx2;
#endif
    int64_t t0 = hw_now_us();
    if (n >= 262144LL) vortex_parallel_for_dispatch((void*)worker_fn, &ctx, 0, n);
    else worker_fn(&ctx, 0, n);
    hw_router_record(HW_OP_AXPY_F32, hw_flops_bucket(2LL * n), false, (double)(hw_now_us() - t0));
    return vortex_hw_plan_route(n * 8LL, 2LL * n);
}

int64_t vortex_hw_axpy_f32(int64_t alpha_bits, int64_t x_ptr, int64_t y_ptr, int64_t n) {
    hw_ensure_init();
    if (!x_ptr || !y_ptr || n <= 0) return 0;
    uint32_t u32 = (uint32_t)(uint64_t)alpha_bits;
    float alpha = 0.0f;
    memcpy(&alpha, &u32, sizeof(float));
    return hw_axpy_f32_cpu_internal(alpha, (const float*)(uintptr_t)x_ptr, (float*)(uintptr_t)y_ptr, n);
}

#if VORTEX_HW_X86
__attribute__((target("avx2")))
static void hw_relu_f32_worker_avx2(void* raw_ctx, int64_t start, int64_t end) {
    float* x = (float*)raw_ctx;
    __m256 z = _mm256_setzero_ps();
    int64_t i = start;
    for (; i + 32 <= end; i += 32) {
        _mm256_storeu_ps(x + i + 0,  _mm256_max_ps(_mm256_loadu_ps(x + i + 0),  z));
        _mm256_storeu_ps(x + i + 8,  _mm256_max_ps(_mm256_loadu_ps(x + i + 8),  z));
        _mm256_storeu_ps(x + i + 16, _mm256_max_ps(_mm256_loadu_ps(x + i + 16), z));
        _mm256_storeu_ps(x + i + 24, _mm256_max_ps(_mm256_loadu_ps(x + i + 24), z));
    }
    for (; i < end; i++) {
        if (x[i] < 0.0f) x[i] = 0.0f;
    }
}
#endif

static void hw_relu_f32_worker_scalar(void* raw_ctx, int64_t start, int64_t end) {
    float* x = (float*)raw_ctx;
    for (int64_t i = start; i < end; i++) {
        if (x[i] < 0.0f) x[i] = 0.0f;
    }
}

/* --- Resident RAM/VRAM HwBuf Implementation --- */

static VortexHwBuf* hw_buf_validate(int64_t handle) {
    if (!handle) return NULL;
    uintptr_t addr = (uintptr_t)handle;
    uintptr_t lo = (uintptr_t)&g_hw_buf_pool[0];
    uintptr_t hi = (uintptr_t)&g_hw_buf_pool[HW_MAX_BUFS];
    if (addr < lo || addr >= hi) return NULL;
    if ((addr - lo) % sizeof(VortexHwBuf) != 0) return NULL;
    VortexHwBuf* b = (VortexHwBuf*)addr;
    if (b->magic != HW_BUF_MAGIC || !b->active) return NULL;
    return b;
}

static void hw_buf_release_slot(VortexHwBuf* b) {
    if (!b || !b->active) return;
    if (b->cl_mem && g_hw.p_rel_mem) {
        g_hw.p_rel_mem(b->cl_mem);
    }
    memset(b, 0, sizeof(*b));
    if (g_hw_active_buf_count > 0) g_hw_active_buf_count--;
}

void vortex_hw_release_scope(void* arena_ptr, uint32_t target_depth) {
    if (g_hw_active_buf_count == 0) return;
    for (int i = 0; i < HW_MAX_BUFS; i++) {
        VortexHwBuf* b = &g_hw_buf_pool[i];
        if (b->active && b->owner_arena == arena_ptr && b->scope_depth > target_depth) {
            hw_buf_release_slot(b);
        }
    }
}

void vortex_hw_release_all_bufs(void) {
    if (g_hw_active_buf_count == 0) return;
    for (int i = 0; i < HW_MAX_BUFS; i++) {
        if (g_hw_buf_pool[i].active) {
            hw_buf_release_slot(&g_hw_buf_pool[i]);
        }
    }
}

static int64_t hw_buf_alloc_common(int64_t slice_ptr, int64_t n, uint8_t elem_size) {
    hw_ensure_init();
    if (!slice_ptr || n <= 0) return 0;
    for (int i = 0; i < HW_MAX_BUFS; i++) {
        if (!g_hw_buf_pool[i].active) {
            VortexHwBuf* b = &g_hw_buf_pool[i];
            memset(b, 0, sizeof(*b));
            b->magic = HW_BUF_MAGIC;
            b->active = 1;
            b->host_dirty = 1;
            b->dev_dirty = 0;
            b->elem_size = elem_size;
            b->host_ptr = (void*)(uintptr_t)slice_ptr;
            b->elem_count = n;
            b->bytes = (size_t)n * (size_t)elem_size;
            arena_get_scope_info(&b->owner_arena, &b->scope_depth, &b->scope_epoch);
            g_hw_active_buf_count++;
            return (int64_t)(uintptr_t)b;
        }
    }
    return 0;
}

int64_t vortex_hw_buf_from_f32(int64_t slice_ptr, int64_t n) {
    return hw_buf_alloc_common(slice_ptr, n, 4);
}

int64_t vortex_hw_buf_from_f64(int64_t slice_ptr, int64_t n) {
    return hw_buf_alloc_common(slice_ptr, n, 8);
}

static bool hw_buf_ensure_dev(VortexHwBuf* b, bool need_contents) {
    if (!b || !g_hw.has_gpu || !hw_ensure_opencl_kernels()) return false;
    vx_cl_int err = 0;
    if (!b->cl_mem) {
        if (need_contents && b->host_dirty) {
            int64_t t0 = hw_now_us();
            b->cl_mem = g_hw.p_create_buf(g_hw.cl_ctx, VX_CL_MEM_READ_WRITE | VX_CL_MEM_COPY_HOST_PTR,
                                          b->bytes, b->host_ptr, &err);
            if (err != VX_CL_SUCCESS || !b->cl_mem) { b->cl_mem = NULL; return false; }
            int64_t dt = hw_now_us() - t0;
            if (dt > 0 && b->bytes >= 65536) {
                double gbps = ((double)b->bytes / ((double)dt * 1000.0));
                if (gbps > 0.2 && gbps < 64.0) g_hw.router.pcie_gbps = 0.7 * g_hw.router.pcie_gbps + 0.3 * gbps;
            }
            g_hw.router.pcie_uploads++;
            g_hw.router.pcie_bytes_up += (int64_t)b->bytes;
            b->host_dirty = 0;
            b->dev_dirty = 0;
        } else {
            b->cl_mem = g_hw.p_create_buf(g_hw.cl_ctx, VX_CL_MEM_READ_WRITE, b->bytes, NULL, &err);
            if (err != VX_CL_SUCCESS || !b->cl_mem) { b->cl_mem = NULL; return false; }
            if (!need_contents) b->host_dirty = 0;
        }
        return true;
    }

    if (need_contents && b->host_dirty) {
        int64_t t0 = hw_now_us();
        err = g_hw.p_write_buf(g_hw.cl_queue, b->cl_mem, VX_CL_TRUE, 0, b->bytes, b->host_ptr, 0, NULL, NULL);
        if (err != VX_CL_SUCCESS) return false;
        int64_t dt = hw_now_us() - t0;
        if (dt > 0 && b->bytes >= 65536) {
            double gbps = ((double)b->bytes / ((double)dt * 1000.0));
            if (gbps > 0.2 && gbps < 64.0) g_hw.router.pcie_gbps = 0.7 * g_hw.router.pcie_gbps + 0.3 * gbps;
        }
        g_hw.router.pcie_uploads++;
        g_hw.router.pcie_bytes_up += (int64_t)b->bytes;
        b->host_dirty = 0;
    }
    return true;
}

static bool hw_buf_ensure_host(VortexHwBuf* b) {
    if (!b) return false;
    if (b->dev_dirty && b->cl_mem && g_hw.p_read_buf) {
        vx_cl_int err = g_hw.p_read_buf(g_hw.cl_queue, b->cl_mem, VX_CL_TRUE, 0, b->bytes, b->host_ptr, 0, NULL, NULL);
        if (err != VX_CL_SUCCESS) return false;
        g_hw.router.pcie_downloads++;
        g_hw.router.pcie_bytes_down += (int64_t)b->bytes;
        b->dev_dirty = 0;
        b->host_dirty = 0;
    }
    return true;
}

int64_t vortex_hw_buf_sync_host(int64_t buf_handle) {
    hw_ensure_init();
    VortexHwBuf* b = hw_buf_validate(buf_handle);
    if (!b) return 0;
    uint8_t was_dirty = b->dev_dirty;
    if (!hw_buf_ensure_host(b)) return 0;
    return was_dirty ? 1 : 0;
}

int64_t vortex_hw_buf_mark_host_dirty(int64_t buf_handle) {
    VortexHwBuf* b = hw_buf_validate(buf_handle);
    if (!b) return 0;
    b->host_dirty = 1;
    b->dev_dirty = 0;
    return 1;
}

int64_t vortex_hw_buf_state(int64_t buf_handle) {
    VortexHwBuf* b = hw_buf_validate(buf_handle);
    if (!b) return 0;
    int64_t s = 0;
    if (b->host_dirty) s |= HW_BUF_HOST_DIRTY;
    if (b->dev_dirty)  s |= HW_BUF_DEV_DIRTY;
    if (b->cl_mem)     s |= HW_BUF_IN_VRAM;
    return s;
}

int64_t vortex_hw_buf_free(int64_t buf_handle) {
    VortexHwBuf* b = hw_buf_validate(buf_handle);
    if (!b) return 0;
    hw_buf_release_slot(b);
    return 1;
}

int64_t vortex_hw_matmul_f32_buf(int64_t a_buf, int64_t b_buf, int64_t c_buf, int64_t m, int64_t n, int64_t k) {
    hw_ensure_init();
    VortexHwBuf* ba = hw_buf_validate(a_buf);
    VortexHwBuf* bb = hw_buf_validate(b_buf);
    VortexHwBuf* bc = hw_buf_validate(c_buf);
    if (!ba || !bb || !bc || m <= 0 || n <= 0 || k <= 0) return 0;

    size_t dirty_up = (ba->host_dirty ? ba->bytes : 0) + (bb->host_dirty ? bb->bytes : 0);
    int64_t flops = 2LL * m * n * k;
    bool resident_hint = (ba->cl_mem != NULL && bb->cl_mem != NULL);

    if (hw_router_should_use_gpu(HW_OP_GEMM_F32, flops, dirty_up, 0, resident_hint)) {
        if (hw_buf_ensure_dev(ba, true) && hw_buf_ensure_dev(bb, true) && hw_buf_ensure_dev(bc, false)) {
            int64_t t0 = hw_now_us();
            bool ok = hw_dispatch_gpu_matmul_f32_mems(ba->cl_mem, bb->cl_mem, bc->cl_mem, (int)m, (int)n, (int)k);
            int64_t dt = hw_now_us() - t0;
            if (ok) {
                bc->dev_dirty = 1;
                bc->host_dirty = 0;
                hw_router_record(HW_OP_GEMM_F32, hw_flops_bucket(flops), true, (double)dt);
                return HW_ROUTE_GPU;
            }
        }
    }

    hw_buf_ensure_host(ba);
    hw_buf_ensure_host(bb);
    int64_t route = vortex_hw_matmul_f32_cpu((int64_t)(uintptr_t)ba->host_ptr,
                                             (int64_t)(uintptr_t)bb->host_ptr,
                                             (int64_t)(uintptr_t)bc->host_ptr,
                                             m, n, k);
    bc->host_dirty = 1;
    bc->dev_dirty = 0;
    return route;
}

int64_t vortex_hw_matmul_f64_buf(int64_t a_buf, int64_t b_buf, int64_t c_buf, int64_t m, int64_t n, int64_t k) {
    hw_ensure_init();
    VortexHwBuf* ba = hw_buf_validate(a_buf);
    VortexHwBuf* bb = hw_buf_validate(b_buf);
    VortexHwBuf* bc = hw_buf_validate(c_buf);
    if (!ba || !bb || !bc || m <= 0 || n <= 0 || k <= 0) return 0;

    hw_buf_ensure_host(ba);
    hw_buf_ensure_host(bb);
    int64_t route = vortex_hw_matmul_f64((int64_t)(uintptr_t)ba->host_ptr,
                                         (int64_t)(uintptr_t)bb->host_ptr,
                                         (int64_t)(uintptr_t)bc->host_ptr,
                                         m, n, k);
    bc->host_dirty = 1;
    bc->dev_dirty = 0;
    return route;
}

int64_t vortex_hw_axpy_f32_buf(int64_t alpha_bits, int64_t x_buf, int64_t y_buf, int64_t n) {
    hw_ensure_init();
    VortexHwBuf* bx = hw_buf_validate(x_buf);
    VortexHwBuf* by = hw_buf_validate(y_buf);
    if (!bx || !by || n <= 0) return 0;
    uint32_t u32 = (uint32_t)(uint64_t)alpha_bits;
    float alpha = 0.0f;
    memcpy(&alpha, &u32, sizeof(float));

    size_t dirty_up = (bx->host_dirty ? bx->bytes : 0) + (by->host_dirty ? by->bytes : 0);
    bool resident_hint = (bx->cl_mem != NULL && by->cl_mem != NULL && !bx->host_dirty && !by->host_dirty);
    int64_t flops = 2LL * n;

    if ((resident_hint || hw_router_should_use_gpu(HW_OP_AXPY_F32, flops, dirty_up, 0, resident_hint)) &&
        hw_buf_ensure_dev(bx, true) && hw_buf_ensure_dev(by, true) && g_hw.k_axpy_f32) {
        int ni = (int)n;
        size_t local[1] = { 256 };
        size_t global[1] = { (size_t)(((n + 255) / 256) * 256) };
        int64_t t0 = hw_now_us();
        vx_cl_int err = 0;
        err |= g_hw.p_set_arg(g_hw.k_axpy_f32, 0, sizeof(float), &alpha);
        err |= g_hw.p_set_arg(g_hw.k_axpy_f32, 1, sizeof(vx_cl_mem), &bx->cl_mem);
        err |= g_hw.p_set_arg(g_hw.k_axpy_f32, 2, sizeof(vx_cl_mem), &by->cl_mem);
        err |= g_hw.p_set_arg(g_hw.k_axpy_f32, 3, sizeof(int), &ni);
        if (err == VX_CL_SUCCESS) err = g_hw.p_ndrange(g_hw.cl_queue, g_hw.k_axpy_f32, 1, NULL, global, local, 0, NULL, NULL);
        if (err == VX_CL_SUCCESS) err = g_hw.p_finish(g_hw.cl_queue);
        if (err == VX_CL_SUCCESS) {
            by->dev_dirty = 1;
            by->host_dirty = 0;
            hw_router_record(HW_OP_AXPY_F32, hw_flops_bucket(flops), true, (double)(hw_now_us() - t0));
            return HW_ROUTE_GPU;
        }
    }

    hw_buf_ensure_host(bx);
    hw_buf_ensure_host(by);
    int64_t route = hw_axpy_f32_cpu_internal(alpha, (const float*)bx->host_ptr, (float*)by->host_ptr, n);
    by->host_dirty = 1;
    by->dev_dirty = 0;
    return route;
}

int64_t vortex_hw_relu_f32_buf(int64_t x_buf, int64_t n) {
    hw_ensure_init();
    VortexHwBuf* bx = hw_buf_validate(x_buf);
    if (!bx || n <= 0) return 0;

    size_t dirty_up = bx->host_dirty ? bx->bytes : 0;
    bool resident_hint = (bx->cl_mem != NULL && !bx->host_dirty);
    int64_t flops = n;

    if ((resident_hint || hw_router_should_use_gpu(HW_OP_RELU_F32, flops, dirty_up, 0, resident_hint)) &&
        hw_buf_ensure_dev(bx, true) && g_hw.k_relu_f32) {
        int ni = (int)n;
        size_t local[1] = { 256 };
        size_t global[1] = { (size_t)(((n + 255) / 256) * 256) };
        int64_t t0 = hw_now_us();
        vx_cl_int err = 0;
        err |= g_hw.p_set_arg(g_hw.k_relu_f32, 0, sizeof(vx_cl_mem), &bx->cl_mem);
        err |= g_hw.p_set_arg(g_hw.k_relu_f32, 1, sizeof(int), &ni);
        if (err == VX_CL_SUCCESS) err = g_hw.p_ndrange(g_hw.cl_queue, g_hw.k_relu_f32, 1, NULL, global, local, 0, NULL, NULL);
        if (err == VX_CL_SUCCESS) err = g_hw.p_finish(g_hw.cl_queue);
        if (err == VX_CL_SUCCESS) {
            bx->dev_dirty = 1;
            bx->host_dirty = 0;
            hw_router_record(HW_OP_RELU_F32, hw_flops_bucket(flops), true, (double)(hw_now_us() - t0));
            return HW_ROUTE_GPU;
        }
    }

    hw_buf_ensure_host(bx);
    void (*worker_fn)(void*, int64_t, int64_t) = hw_relu_f32_worker_scalar;
#if VORTEX_HW_X86
    if (g_hw.has_avx2) worker_fn = hw_relu_f32_worker_avx2;
#endif
    int64_t t0 = hw_now_us();
    if (n >= 262144LL) vortex_parallel_for_dispatch((void*)worker_fn, bx->host_ptr, 0, n);
    else worker_fn(bx->host_ptr, 0, n);
    hw_router_record(HW_OP_RELU_F32, hw_flops_bucket(flops), false, (double)(hw_now_us() - t0));
    bx->host_dirty = 1;
    bx->dev_dirty = 0;
    return vortex_hw_plan_route(n * 4LL, flops);
}
