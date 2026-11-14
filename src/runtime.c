/*
 * VORTEX PROGRAMMING LANGUAGE - RUNTIME, GUI, THREAD POOL, BUILTINS & PACKAGER
 */

#include "vortex.h"
#include <stdarg.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/wait.h>
#if defined(__aarch64__) || defined(__arm64__)
extern void pthread_jit_write_protect_np(int enabled);
#endif
#endif

#ifndef _WIN32
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define VORTEX_HOST_ARCH VORTEX_ARCH_ARM64
#if defined(__APPLE__)
#define VORTEX_HOST_ABI VORTEX_ABI_DARWIN64
#else
#define VORTEX_HOST_ABI VORTEX_ABI_AAPCS64
#endif
#else
#define VORTEX_HOST_ARCH VORTEX_ARCH_X86_64
#if defined(_WIN32)
#define VORTEX_HOST_ABI VORTEX_ABI_WIN64
#else
#define VORTEX_HOST_ABI VORTEX_ABI_SYSV64
#endif
#endif

SymbolTable g_last_symtab;
bool g_has_last_symtab = false;

jmp_buf g_error_jmp;
bool g_in_repl = false;
VortexBuildMode g_compile_mode = VORTEX_MODE_SAFE;
VortexTargetArch g_target_arch = VORTEX_HOST_ARCH;
VortexTargetABI  g_target_abi  = VORTEX_HOST_ABI;
const char* g_current_filename = "<input>";
const char* g_dump_jit_path = NULL;
bool g_headless_mode = false;
bool g_is_packaged_app = false;

bool vortex_is_cross_target(void) {
    if (g_target_arch != VORTEX_HOST_ARCH) return true;
    if (g_target_arch == VORTEX_ARCH_X86_64 && g_target_abi != VORTEX_HOST_ABI) return true;
    if (g_target_arch == VORTEX_ARCH_ARM64 && g_target_abi != VORTEX_HOST_ABI) return true;
    return false;
}

#define MAX_INTERN_CSTRS 1024
static const char* g_intern_cstrs[MAX_INTERN_CSTRS];
static int g_intern_cstr_count = 0;

void vx_str_copy(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t slen = strlen(src);
    if (slen >= cap) slen = cap - 1;
    if (slen > 0) memcpy(dst, src, slen);
    dst[slen] = '\0';
}

const char* vortex_intern_cstr(const char* s) {
    if (!s) return "";
    for (int i = 0; i < g_intern_cstr_count; i++) {
        if (strcmp(g_intern_cstrs[i], s) == 0) return g_intern_cstrs[i];
    }
    size_t len = strlen(s);
    char* copy = (char*)malloc(len + 1);
    if (!copy) return s;
    memcpy(copy, s, len + 1);
    if (g_intern_cstr_count < MAX_INTERN_CSTRS) {
        g_intern_cstrs[g_intern_cstr_count++] = copy;
    }
    return copy;
}

void jit_write_protect(void* ptr, size_t size, bool writable) {
    if (!ptr || size == 0 || g_target_arch != VORTEX_HOST_ARCH) {
        (void)writable;
        return;
    }
#ifdef _WIN32
    if (!writable) {
        FlushInstructionCache(GetCurrentProcess(), ptr, size);
    }
#else
    if (!writable) {
#ifdef __APPLE__
        sys_icache_invalidate(ptr, size);
#else
        __builtin___clear_cache((char*)ptr, (char*)ptr + size);
#endif
    }
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    pthread_jit_write_protect_np(writable ? 0 : 1);
#else
    long page_sz = sysconf(_SC_PAGESIZE);
    size_t pg = (page_sz > 0) ? (size_t)page_sz : 4096;
    uintptr_t start = (uintptr_t)ptr & ~(uintptr_t)(pg - 1);
    uintptr_t end = ((uintptr_t)ptr + size + pg - 1) & ~(uintptr_t)(pg - 1);
    int prot = writable ? (PROT_READ | PROT_WRITE) : (PROT_READ | PROT_EXEC);
    mprotect((void*)start, (size_t)(end - start), prot);
#endif
#endif
}

void* alloc_exec_page(size_t size) {
    if (g_target_arch != VORTEX_HOST_ARCH) {
        return calloc(1, size);
    }
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#elif defined(__APPLE__)
    void* mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (mem == MAP_FAILED) {
        mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mem == MAP_FAILED) return NULL;
    }
#if defined(__aarch64__) || defined(__arm64__)
    pthread_jit_write_protect_np(0);
#endif
    return mem;
#else
    void* mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (mem == MAP_FAILED) ? NULL : mem;
#endif
}

void free_exec_page(void* ptr, size_t size) {
    if (!ptr) return;
    if (g_target_arch != VORTEX_HOST_ARCH) {
        free(ptr);
        return;
    }
#ifdef _WIN32
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

void vortex_get_self_path(char* out, size_t cap, const char* argv0) {
    if (!out || cap == 0) return;
    out[0] = '\0';
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (len == 0 || len >= cap) {
        if (argv0) vx_str_copy(out, cap, argv0);
    }
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) != 0) {
        if (argv0) vx_str_copy(out, cap, argv0);
    }
#else
    ssize_t rlen = readlink("/proc/self/exe", out, cap - 1);
    if (rlen > 0 && (size_t)rlen < cap) {
        out[rlen] = '\0';
    } else if (argv0) {
        vx_str_copy(out, cap, argv0);
    }
#endif
}

void vortex_wait_on_standalone_error(const char* msg) {
#ifdef _WIN32
    fflush(stderr);
    HWND console_wnd = GetConsoleWindow();
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD err_type = (hErr != NULL && hErr != INVALID_HANDLE_VALUE) ? GetFileType(hErr) : FILE_TYPE_UNKNOWN;
    DWORD pids[2];
    DWORD proc_count = (console_wnd != NULL) ? GetConsoleProcessList(pids, 2) : 0;

    if (g_is_packaged_app && console_wnd == NULL && err_type == FILE_TYPE_UNKNOWN) {
        const char* text = (msg && msg[0]) ? msg : "Vortex initialization or runtime execution failed.";
        MessageBoxA(NULL, text, "Vortex Runtime Error", MB_OK | MB_ICONERROR);
    } else if (g_is_packaged_app && console_wnd != NULL && proc_count == 1 && err_type == FILE_TYPE_CHAR) {
        fprintf(stderr, "\n[VORTEX] Initialization or runtime error encountered. Press Enter to exit...\n");
        fflush(stderr);
        int ch = getchar();
        (void)ch;
    }
#else
    (void)msg;
    fflush(stderr);
#endif
}

static char g_lsp_diag_buf[4096];
static size_t g_lsp_diag_len = 0;

void vortex_diag_emit(FILE* stream, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (stream == stderr && g_lsp_capture_errors) {
        if (g_lsp_diag_len + 1 < sizeof(g_lsp_diag_buf)) {
            int n = vsnprintf(g_lsp_diag_buf + g_lsp_diag_len,
                              sizeof(g_lsp_diag_buf) - g_lsp_diag_len, fmt, ap);
            if (n > 0) {
                size_t add = (size_t)n;
                if (g_lsp_diag_len + add >= sizeof(g_lsp_diag_buf)) {
                    g_lsp_diag_len = sizeof(g_lsp_diag_buf) - 1;
                } else {
                    g_lsp_diag_len += add;
                }
            }
        }
        va_end(ap);
        return;
    }
    vfprintf(stream, fmt, ap);
    va_end(ap);
}

void vortex_error(const char* msg) {
    if (msg && msg[0]) {
        if (g_lsp_capture_errors) {
            vortex_diag_emit(stderr, "%s\n", msg);
        } else {
            fprintf(stderr, "%s\n", msg);
        }
    }
    if (!g_in_repl && !g_lsp_capture_errors) {
        vortex_print_stacktrace(NULL);
    }
    fflush(stderr);
    if (g_in_repl || g_lsp_capture_errors) {
        longjmp(g_error_jmp, 1);
    } else {
        vortex_wait_on_standalone_error(msg);
        exit(1);
    }
}


/* ========================================================================= */
/* 5. RUNTIME CONTEXT & UTILITIES                                            */
/* ========================================================================= */

void helper_print_i64(int64_t val) { printf("%lld\n", (long long)val); fflush(stdout); }
void helper_print_f32(float val) { printf("%.6f\n", (double)val); fflush(stdout); }
void helper_print_str(const char* s) { printf("%s", s); fflush(stdout); }

int64_t vortex_math_sin(int64_t deg_x10) {
    double rad = (double)deg_x10 * (3.14159265358979323846 / 1800.0);
    return (int64_t)(sin(rad) * 4096.0);
}

int64_t vortex_math_cos(int64_t deg_x10) {
    double rad = (double)deg_x10 * (3.14159265358979323846 / 1800.0);
    return (int64_t)(cos(rad) * 4096.0);
}

int64_t vortex_math_sqrt(int64_t x) {
    if (x <= 0) return 0;
    return (int64_t)sqrt((double)x);
}

int64_t helper_exit(int64_t code) {
    exit((int)code);
    return 0;
}

int64_t helper_time_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000000LL) / freq.QuadPart);
#else
#if defined(__APPLE__) && !defined(CLOCK_MONOTONIC)
    static mach_timebase_info_data_t tb_info;
    static int tb_init = 0;
    if (!tb_init) {
        mach_timebase_info(&tb_info);
        tb_init = 1;
    }
    uint64_t ns = (mach_absolute_time() * tb_info.numer) / tb_info.denom;
    return (int64_t)(ns / 1000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000LL);
#endif
#endif
}


/* --- Native OS Worker Thread Pool (parallel_for) --- */
static VortexThreadPool g_thread_pool;

#ifdef _WIN32
static DWORD WINAPI vortex_worker_proc(LPVOID lpParam) {
    VortexWorker* w = (VortexWorker*)lpParam;
    arena_set_thread_local(&w->arena);
    while (1) {
        WaitForSingleObject(w->start_event, INFINITE);
        if (w->should_exit) break;
        if (w->job_fn) {
            w->job_fn(w->job_ctx, w->slice_start, w->slice_end);
        }
        SetEvent(w->done_event);
    }
    return 0;
}
#else
static void* vortex_worker_proc(void* lpParam) {
    VortexWorker* w = (VortexWorker*)lpParam;
    arena_set_thread_local(&w->arena);
    while (1) {
        pthread_mutex_lock(&w->mutex);
        while (!w->job_ready && !w->should_exit) {
            pthread_cond_wait(&w->start_cond, &w->mutex);
        }
        if (w->should_exit) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        w->job_ready = false;
        pthread_mutex_unlock(&w->mutex);

        if (w->job_fn) {
            w->job_fn(w->job_ctx, w->slice_start, w->slice_end);
        }

        pthread_mutex_lock(&w->mutex);
        w->job_done = true;
        pthread_cond_signal(&w->done_cond);
        pthread_mutex_unlock(&w->mutex);
    }
    return NULL;
}
#endif

void vortex_thread_pool_init(void) {
    if (g_thread_pool.initialized) return;
    int count = 1;
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    count = (int)sysinfo.dwNumberOfProcessors;
#else
    count = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (count <= 0) count = 1;
    if (count > VORTEX_MAX_THREADS) count = VORTEX_MAX_THREADS;
    g_thread_pool.worker_count = count;

    for (int i = 0; i < count; i++) {
        VortexWorker* w = &g_thread_pool.workers[i];
        memset(w, 0, sizeof(*w));
        w->id = i;
        w->should_exit = false;
        w->arena.head = arena_page_create(VORTEX_PAGE_SIZE);
        w->arena.current = w->arena.head;
        w->arena.current_page_index = 0;
        w->arena.total_pages = 1;
        w->arena.scope_depth = 0;
        w->arena.scope_stack[0] = 1;
        w->arena.next_epoch = 2;
        arena_register(&w->arena);
#ifdef _WIN32
        w->start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        w->done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_thread_pool.done_handles[i] = w->done_event;
        w->thread = CreateThread(NULL, 0, vortex_worker_proc, w, 0, NULL);
#else
        pthread_mutex_init(&w->mutex, NULL);
        pthread_cond_init(&w->start_cond, NULL);
        pthread_cond_init(&w->done_cond, NULL);
        w->job_ready = false;
        w->job_done = false;
        pthread_create(&w->thread, NULL, vortex_worker_proc, w);
#endif
    }
    g_thread_pool.initialized = true;
}

void vortex_parallel_for_dispatch(void* fn_ptr, void* ctx, int64_t start, int64_t end) {
    ParallelJobFn fn = (ParallelJobFn)fn_ptr;
    if (!g_thread_pool.initialized) {
        vortex_thread_pool_init();
    }
    int count = g_thread_pool.worker_count;
    int64_t total = end - start;
    if (total <= 0) return;

    if (total < count || count <= 1) {
        fn(ctx, start, end);
        return;
    }

    int64_t chunk = total / count;
    int64_t rem = total % count;
    int worker_dispatch = count - 1;

    int64_t cur = start;
    for (int i = 0; i < worker_dispatch; i++) {
        int64_t this_chunk = chunk + (i < rem ? 1 : 0);
        VortexWorker* w = &g_thread_pool.workers[i];
        w->job_fn = fn;
        w->job_ctx = ctx;
        w->slice_start = cur;
        w->slice_end = cur + this_chunk;
        cur += this_chunk;
#ifdef _WIN32
        SetEvent(w->start_event);
#else
        pthread_mutex_lock(&w->mutex);
        w->job_ready = true;
        w->job_done = false;
        pthread_cond_signal(&w->start_cond);
        pthread_mutex_unlock(&w->mutex);
#endif
    }

    /* Run final slice directly on caller thread to eliminate 1 wakeup + 1 wait */
    fn(ctx, cur, end);

#ifdef _WIN32
    WaitForMultipleObjects((DWORD)worker_dispatch, g_thread_pool.done_handles, TRUE, INFINITE);
#else
    for (int i = 0; i < worker_dispatch; i++) {
        VortexWorker* w = &g_thread_pool.workers[i];
        pthread_mutex_lock(&w->mutex);
        while (!w->job_done) {
            pthread_cond_wait(&w->done_cond, &w->mutex);
        }
        pthread_mutex_unlock(&w->mutex);
    }
#endif
}

int64_t vortex_num_workers(void) {
    if (!g_thread_pool.initialized) vortex_thread_pool_init();
    return (int64_t)g_thread_pool.worker_count;
}


/* --- Built-in Runtime Dispatch Table --- */
static BuiltinFunc g_builtins[] = {
    {"vec_new",        (void*)vortex_vec_new,        0},
    {"vec_push",       (void*)vortex_vec_push,       2},
    {"vec_get",        (void*)vortex_vec_get,        2},
    {"vec_set",        (void*)vortex_vec_set,        3},
    {"vec_len",        (void*)vortex_vec_len,        1},
    {"map_new",        (void*)vortex_map_new,        0},
    {"map_set",        (void*)vortex_map_set,        3},
    {"map_get",        (void*)vortex_map_get,        2},
    {"map_has",        (void*)vortex_map_has,        2},
    {"map_len",        (void*)vortex_map_len,        1},
    {"str_concat",     (void*)vortex_str_concat,     2},
    {"str_join",       (void*)vortex_str_join,       2},
    {"str_len",        (void*)vortex_str_len,        1},
    {"str_sub",        (void*)vortex_str_sub,        3},
    {"str_from_i64",   (void*)vortex_str_from_i64,   1},
    {"str_from_f32",   (void*)vortex_str_from_f32,   1},
    {"str_byte",       (void*)vortex_str_byte,       2},
    {"str_from_byte",  (void*)vortex_str_from_byte,  1},
    {"str_eq",         (void*)vortex_str_eq,         2},
    {"str_find",       (void*)vortex_str_find,       3},
    {"str_find_any2",  (void*)vortex_str_find_any2,  4},
    {"str_to_i64",     (void*)vortex_str_to_i64,     1},
    {"ptr_to_str",     (void*)vortex_ptr_to_str,     1},
    {"scope_push",     (void*)vortex_scope_push,     0},
    {"scope_pop",      (void*)vortex_scope_pop,      1},
    {"slice_i64",      (void*)vortex_alloc_slice_i64,1},
    {"slice_f32",      (void*)vortex_alloc_slice_f32,1},
    {"slice_i32",      (void*)vortex_alloc_slice_i32,1},
    {"slice_len",      (void*)vortex_slice_len,      1},
    {"ptr_read8",      (void*)vortex_ptr_read8,      1},
    {"ptr_write8",     (void*)vortex_ptr_write8,     2},
    {"ptr_read32",     (void*)vortex_ptr_read32,     1},
    {"ptr_write32",    (void*)vortex_ptr_write32,    2},
    {"ptr_read64",     (void*)vortex_ptr_read64,     1},
    {"ptr_write64",    (void*)vortex_ptr_write64,    2},
    {"ffi_load",       (void*)vortex_ffi_load,       1},
    {"ffi_symbol",     (void*)vortex_ffi_symbol,     2},
    {"ffi_call0",      (void*)vortex_ffi_call0,      1},
    {"ffi_call1",      (void*)vortex_ffi_call1,      2},
    {"ffi_call2",      (void*)vortex_ffi_call2,      3},
    {"ffi_call3",      (void*)vortex_ffi_call3,      4},
    {"ffi_call4",      (void*)vortex_ffi_call4,      5},
    {"ffi_call5",      (void*)vortex_ffi_call5,      6},
    {"ffi_call6",      (void*)vortex_ffi_call6,      7},
    {"gui_window",     (void*)vortex_gui_window,     3},
    {"gui_is_open",    (void*)vortex_gui_is_open,    1},
    {"gui_rgb",        (void*)vortex_gui_rgb,        3},
    {"gui_clear",      (void*)vortex_gui_clear,      2},
    {"gui_draw_rect",  (void*)vortex_gui_draw_rect,  6},
    {"gui_draw_text",  (void*)vortex_gui_draw_text,  5},
    {"gui_draw_number",(void*)vortex_gui_draw_number,5},
    {"gui_button",     (void*)vortex_gui_button,     7},
    {"gui_present",    (void*)vortex_gui_present,    1},
    {"gui_set_pos",    (void*)vortex_gui_set_pos,    3},
    {"gui_draw_line",  (void*)vortex_gui_draw_line,  6},
    {"gui_draw_pixel", (void*)vortex_gui_draw_pixel, 4},
    {"math_sin",       (void*)vortex_math_sin,       1},
    {"math_cos",       (void*)vortex_math_cos,       1},
    {"math_sqrt",      (void*)vortex_math_sqrt,      1},
    {"struct_size",    (void*)vortex_builtin_struct_size, 1},
    {"struct_align",   (void*)vortex_builtin_struct_align, 1},
    {"time_us",        (void*)helper_time_us,        0},
    {"num_workers",    (void*)vortex_num_workers,    0},
    {"alloc",          (void*)arena_alloc,           1},
    {"print",          (void*)helper_print_i64,      1},
    {"print_str",      (void*)helper_print_str,      1},
    {"exit",           (void*)helper_exit,           1},
    {NULL, NULL, 0}
};

BuiltinFunc* builtin_find(const char* name) {
    for (int i = 0; g_builtins[i].name != NULL; i++) {
        if (strcmp(g_builtins[i].name, name) == 0) return &g_builtins[i];
    }
    return NULL;
}

/* --- Source / File JIT Compilation & Execution --- */
bool compile_vortex_source_internal(const char* source, void* exec_mem, JITBuffer* out_jit) {
    if (!g_current_filename || !g_current_filename[0]) {
        g_current_filename = "<input>";
    }
    arena_init();
    debug_info_reset();
    lexer_reset_imports();
    if (!g_in_repl) {
        g_struct_type_count = 0;
    }
    for (int i = 0; i < g_user_func_count; i++) {
        free(g_user_funcs[i].inline_instrs);
        free(g_user_funcs[i].inline_vregs);
        g_user_funcs[i].inline_instrs = NULL;
        g_user_funcs[i].inline_vregs = NULL;
        g_user_funcs[i].can_inline = false;
    }
    g_user_func_count = 0;
    g_user_func_reloc_count = 0;
    g_ffi_func_count = 0;
    g_literal_string_count = 0;

    Lexer lexer;
    lexer_init(&lexer, source);

    SymbolTable symtab;
    memset(&symtab, 0, sizeof(symtab));

    JITBuffer jit;
    jit.code = (uint8_t*)exec_mem;
    jit.size = 0;
    jit.capacity = MAX_JIT_CODE_SIZE;

    jit_write_protect(exec_mem, MAX_JIT_CODE_SIZE, true);

    IRBuffer main_ir;
    ir_init(&main_ir);

    Compiler comp;
    memset(&comp, 0, sizeof(comp));
    comp.lex = &lexer;
    comp.st = &symtab;
    comp.j = &jit;
    comp.ir = &main_ir;
    comp.last_struct_id = -1;

    while (comp.lex->cur.type != TOK_EOF) {
        parse_statement(&comp);
    }

    g_last_symtab = symtab;
    g_has_last_symtab = true;

    const VortexJITBackend* bk = vortex_get_backend();

    if (!vortex_is_cross_target()) {
        vortex_thread_pool_init();
    }
    ir_region_verify(&main_ir);
    ir_optimize_bounds_and_licm(&main_ir);
    ir_loop_vectorize(&main_ir);
    ir_liveness_analysis(&main_ir);
    ir_linear_scan_allocate(&main_ir);

    int main_frame_size = bk->compute_frame_size(main_ir.total_spill_bytes);
    size_t main_start_off = jit.size;
    debug_info_set_current_func("__main__");
    bk->emit_prologue(&jit, main_frame_size);
    bk->ir_codegen(&main_ir, &jit);
    bk->emit_epilogue(&jit, main_frame_size);
    debug_info_record_func("__main__", main_start_off, jit.size, main_frame_size, true, g_current_filename, 1);

    /* Patch forward user function calls across the JIT code */
    for (int r = 0; r < g_user_func_reloc_count; r++) {
        UserFunc* uf = user_func_find(g_user_func_relocs[r].name);
        if (!uf) {
            fprintf(stderr, "[VORTEX ERROR] Unresolved user function '%s'\n", g_user_func_relocs[r].name);
            vortex_error("");
        }
        if (uf->is_unsafe && !g_user_func_relocs[r].called_in_unsafe) {
            fprintf(stderr,
                    "[VORTEX SAFETY ERROR] Calling unsafe fn '%s' requires an unsafe block or unsafe fn at %s:%d\n",
                    uf->name,
                    g_user_func_relocs[r].call_file ? g_user_func_relocs[r].call_file : g_current_filename,
                    g_user_func_relocs[r].call_line);
            vortex_error("");
        }
        bk->patch_call_rel(&jit, g_user_func_relocs[r].patch_pos, uf->code_offset);
    }

    ir_free(&main_ir);
    *out_jit = jit;
    return true;
}

bool vortex_frontend_check(const char* source, const char* file_path, char* diag_out, size_t diag_cap) {
    if (diag_out && diag_cap > 0) diag_out[0] = '\0';
    bool prev_capture = g_lsp_capture_errors;
    const char* prev_file = g_current_filename;
    jmp_buf saved_jmp;
    memcpy(&saved_jmp, &g_error_jmp, sizeof(saved_jmp));

    g_lsp_capture_errors = true;
    g_lsp_diag_len = 0;
    g_lsp_diag_buf[0] = '\0';
    g_current_filename = (file_path && file_path[0]) ? file_path : "<lsp>";

    bool ok = false;
    if (setjmp(g_error_jmp) == 0) {
        arena_init();
        debug_info_reset();
        lexer_reset_imports();
        g_struct_type_count = 0;
        g_user_func_count = 0;
        g_user_func_reloc_count = 0;
        g_ffi_func_count = 0;
        g_literal_string_count = 0;

        Lexer lexer;
        lexer_init(&lexer, source ? source : "");

        SymbolTable symtab;
        memset(&symtab, 0, sizeof(symtab));

        uint8_t dummy_code[64];
        JITBuffer dummy_jit;
        dummy_jit.code = dummy_code;
        dummy_jit.size = 0;
        dummy_jit.capacity = sizeof(dummy_code);

        IRBuffer main_ir;
        ir_init(&main_ir);

        Compiler comp;
        memset(&comp, 0, sizeof(comp));
        comp.lex = &lexer;
        comp.st = &symtab;
        comp.j = &dummy_jit;
        comp.ir = &main_ir;
        comp.last_struct_id = -1;

        while (comp.lex->cur.type != TOK_EOF) {
            parse_statement(&comp);
        }

        ir_region_verify(&main_ir);

        for (int r = 0; r < g_user_func_reloc_count; r++) {
            UserFunc* uf = user_func_find(g_user_func_relocs[r].name);
            if (!uf) {
                vortex_diag_emit(stderr, "[VORTEX ERROR] Unresolved user function '%s' at %s:%d\n",
                                 g_user_func_relocs[r].name,
                                 g_user_func_relocs[r].call_file ? g_user_func_relocs[r].call_file : g_current_filename,
                                 g_user_func_relocs[r].call_line);
                vortex_error("");
            }
            if (uf->is_unsafe && !g_user_func_relocs[r].called_in_unsafe) {
                vortex_diag_emit(stderr,
                                 "[VORTEX SAFETY ERROR] Calling unsafe fn '%s' requires an unsafe block or unsafe fn at %s:%d\n",
                                 uf->name,
                                 g_user_func_relocs[r].call_file ? g_user_func_relocs[r].call_file : g_current_filename,
                                 g_user_func_relocs[r].call_line);
                vortex_error("");
            }
        }

        ir_free(&main_ir);
        ok = true;
    } else {
        ir_free_all_active();
        if (diag_out && diag_cap > 0) {
            strncpy(diag_out, g_lsp_diag_buf, diag_cap - 1);
            diag_out[diag_cap - 1] = '\0';
        }
        ok = false;
    }

    memcpy(&g_error_jmp, &saved_jmp, sizeof(saved_jmp));
    g_current_filename = prev_file;
    g_lsp_capture_errors = prev_capture;
    return ok;
}

int execute_vortex_source(const char* source) {
    void* exec_mem = alloc_exec_page(MAX_JIT_CODE_SIZE);
    if (!exec_mem) {
        fprintf(stderr, "[VORTEX PANIC] Failed to allocate executable page\n");
        return 1;
    }

    JITBuffer jit;
    if (!compile_vortex_source_internal(source, exec_mem, &jit)) {
        free_exec_page(exec_mem, MAX_JIT_CODE_SIZE);
        return 1;
    }

    const VortexTargetDesc* td = vortex_get_target_desc();
    const VortexJITBackend* bk = vortex_get_backend();

    if (g_dump_jit_path && g_dump_jit_path[0] != '\0') {
        FILE* df = fopen(g_dump_jit_path, "wb");
        if (!df) {
            free_exec_page(exec_mem, MAX_JIT_CODE_SIZE);
            fprintf(stderr, "[VORTEX ERROR] Failed to open --dump-jit output '%s'\n", g_dump_jit_path);
            return 1;
        }
        if (jit.size > 0) fwrite(jit.code, 1, jit.size, df);
        fclose(df);
        printf("[VORTEX JIT] Dumped %zu bytes (%s, %s, SIMD %u-bit) to %s\n",
               jit.size, td->name, bk->backend_name, td->simd_width_bits, g_dump_jit_path);
    }

    debug_info_set_jit_image(exec_mem, jit.size, &jit);

    if (g_dump_debug_path && g_dump_debug_path[0] != '\0') {
        if (debug_write_dwarf_elf(g_dump_debug_path)) {
            printf("[VORTEX DEBUG] Wrote DWARF v4 ELF debug info (%s) to %s\n", td->name, g_dump_debug_path);
        } else {
            fprintf(stderr, "[VORTEX DEBUG] Warning: failed to write DWARF output '%s'\n", g_dump_debug_path);
        }
    }

    if (vortex_is_cross_target()) {
        printf("[VORTEX CROSS] Compiled %zu bytes for target '%s' (%s, %u-bit SIMD) - skipping host execution.\n",
               jit.size, td->name, bk->backend_name, td->simd_width_bits);
        debug_info_unregister_jit_image();
        free_exec_page(exec_mem, MAX_JIT_CODE_SIZE);
        return 0;
    }

    jit_write_protect(exec_mem, jit.size, false);

    typedef void (*vortex_entry_fn)(void);
    vortex_entry_fn entry = (vortex_entry_fn)exec_mem;

    vortex_crash_guard_enter();
    entry();
    vortex_crash_guard_leave();

    debug_info_unregister_jit_image();
    free_exec_page(exec_mem, MAX_JIT_CODE_SIZE);
    return 0;
}

/* --- Recursive Module Preprocessor / Import Resolver --- */

typedef struct {
    char** paths;
    int count;
    int cap;
    char* out_buf;
    size_t out_len;
    size_t out_cap;
} VortexImportResolver;

static bool import_append_str(VortexImportResolver* r, const char* s, size_t len) {
    if (r->out_len + len + 1 > r->out_cap) {
        size_t ncap = (r->out_cap == 0) ? 4096 : (r->out_cap * 2);
        while (ncap < r->out_len + len + 1) ncap *= 2;
        char* nb = (char*)realloc(r->out_buf, ncap);
        if (!nb) return false;
        r->out_buf = nb;
        r->out_cap = ncap;
    }
    if (len > 0) memcpy(r->out_buf + r->out_len, s, len);
    r->out_len += len;
    r->out_buf[r->out_len] = '\0';
    return true;
}

static bool import_append_cstr(VortexImportResolver* r, const char* s) {
    return import_append_str(r, s, strlen(s));
}

static void import_normalize_slashes(char* s) {
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\\') s[i] = '/';
    }
}

static void import_canonicalize_path(const char* path, char* out, size_t cap) {
    if (!out || cap == 0) return;
    char raw[1024];
    vx_str_copy(raw, sizeof(raw), path ? path : "");
#ifdef _WIN32
    char full[1024];
    if (_fullpath(full, raw, sizeof(full)) != NULL) {
        vx_str_copy(out, cap, full);
    } else {
        vx_str_copy(out, cap, raw);
    }
    import_normalize_slashes(out);
    for (size_t i = 0; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    }
#else
    /* Pass NULL so POSIX realpath allocates dynamically (avoids __realpath_chk < PATH_MAX abort under _FORTIFY_SOURCE) */
    char* full = realpath(raw, NULL);
    if (full != NULL) {
        vx_str_copy(out, cap, full);
        free(full);
    } else {
        vx_str_copy(out, cap, raw);
    }
    import_normalize_slashes(out);
#endif
}

static bool import_has_seen(const VortexImportResolver* r, const char* canon_path) {
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->paths[i], canon_path) == 0) return true;
    }
    return false;
}

static bool import_mark_seen(VortexImportResolver* r, const char* canon_path) {
    if (import_has_seen(r, canon_path)) return true;
    if (r->count >= r->cap) {
        int ncap = (r->cap == 0) ? 32 : (r->cap * 2);
        char** np = (char**)realloc(r->paths, (size_t)ncap * sizeof(char*));
        if (!np) return false;
        r->paths = np;
        r->cap = ncap;
    }
    size_t len = strlen(canon_path);
    char* copy = (char*)malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, canon_path, len + 1);
    r->paths[r->count++] = copy;
    return true;
}

static void import_extract_dir(const char* file_path, char* out_dir, size_t cap) {
    if (!out_dir || cap == 0) return;
    if (!file_path || !file_path[0]) {
        out_dir[0] = '\0';
        return;
    }
    vx_str_copy(out_dir, cap, file_path);
    import_normalize_slashes(out_dir);
    char* sep = strrchr(out_dir, '/');
    if (sep) *sep = '\0';
    else out_dir[0] = '\0';
}

static bool import_is_regular_file(const char* path) {
    if (!path || !path[0]) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    int ch = fgetc(f);
    (void)ch;
    bool err = (ferror(f) != 0);
    fclose(f);
    return !err;
}

static bool import_resolve_target(const char* spec, const char* parent_file, char* out, size_t cap) {
    if (!spec || !spec[0] || !out || cap == 0) return false;
    char norm_spec[512];
    vx_str_copy(norm_spec, sizeof(norm_spec), spec);
    import_normalize_slashes(norm_spec);

    char parent_dir[512];
    import_extract_dir(parent_file, parent_dir, sizeof(parent_dir));

    /* 1. Resolve relative to parent file directory */
    if (parent_dir[0]) {
        char cand[1280];
        int n = snprintf(cand, sizeof(cand), "%s/%s", parent_dir, norm_spec);
        if (n > 0 && (size_t)n < sizeof(cand) && import_is_regular_file(cand)) {
            vx_str_copy(out, cap, cand);
            return true;
        }
    }

    /* 2. Fall back to standard library / CWD / package resolver */
    return vortex_resolve_import(norm_spec, parent_file, out, cap);
}

static bool import_parse_line_directive(const char* line, size_t line_len, char* out_spec, size_t cap) {
    size_t i = 0;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;

    /* Ignore line comments */
    if (i + 1 < line_len && line[i] == '/' && line[i + 1] == '/') return false;

    /* Must start with 'import' keyword */
    if (i + 6 > line_len || strncmp(line + i, "import", 6) != 0) return false;
    i += 6;
    if (i >= line_len || (line[i] != ' ' && line[i] != '\t' && line[i] != '"')) return false;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line_len) return false;

    size_t k = 0;
    if (line[i] == '"') {
        i++;
        while (i < line_len && line[i] != '"' && line[i] != '\r' && line[i] != '\n') {
            if (k + 1 < cap) out_spec[k++] = line[i];
            i++;
        }
        if (i >= line_len || line[i] != '"') return false;
        i++;
    } else {
        while (i < line_len && (isalnum((unsigned char)line[i]) || line[i] == '_' ||
               line[i] == '/' || line[i] == '\\' || line[i] == '.' || line[i] == '-')) {
            if (k + 1 < cap) out_spec[k++] = line[i];
            i++;
        }
    }
    out_spec[k] = '\0';
    if (k == 0) return false;

    while (i < line_len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    if (i < line_len && line[i] == ';') i++;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    if (i + 1 < line_len && line[i] == '/' && line[i + 1] == '/') return true;
    return i == line_len;
}

static char* import_read_file_text(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
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

static bool import_resolve_file_recursive(VortexImportResolver* r, const char* file_path, bool is_entry) {
    char canon[1024];
    import_canonicalize_path(file_path, canon, sizeof(canon));
    if (import_has_seen(r, canon)) {
        return true;
    }

    size_t len = 0;
    char* src = import_read_file_text(file_path, &len);
    if (!src) {
        if (is_entry) {
            fprintf(stderr, "[VORTEX ERROR] Cannot open file '%s'\n", file_path);
        } else {
            fprintf(stderr, "[VORTEX IMPORT ERROR] Cannot open imported file '%s'\n", file_path);
        }
        return false;
    }

    if (!import_mark_seen(r, canon)) {
        free(src);
        fprintf(stderr, "[VORTEX ERROR] Memory allocation error\n");
        return false;
    }

    size_t pos = 0;
    /* Strip UTF-8 BOM if present */
    if (len >= 3 && (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        pos = 3;
    }

    int line_num = 1;
    while (pos < len) {
        size_t line_start = pos;
        while (pos < len && src[pos] != '\n') pos++;
        size_t line_len = pos - line_start;
        bool has_nl = (pos < len && src[pos] == '\n');
        if (has_nl) pos++;

        char spec[512];
        if (import_parse_line_directive(src + line_start, line_len, spec, sizeof(spec))) {
            char resolved[1024];
            if (!import_resolve_target(spec, file_path, resolved, sizeof(resolved))) {
                fprintf(stderr, "[VORTEX IMPORT ERROR] Cannot open imported file '%s' (at %s:%d)\n",
                        spec, file_path, line_num);
                free(src);
                return false;
            }
            char target_canon[1024];
            import_canonicalize_path(resolved, target_canon, sizeof(target_canon));
            if (!import_has_seen(r, target_canon)) {
                char marker[1280];
                snprintf(marker, sizeof(marker), "// --- begin import \"%s\" (%s:%d) ---\n",
                         spec, file_path, line_num);
                if (!import_append_cstr(r, marker) ||
                    !import_resolve_file_recursive(r, resolved, false)) {
                    free(src);
                    return false;
                }
                snprintf(marker, sizeof(marker), "\n// --- end import \"%s\" ---\n", spec);
                if (!import_append_cstr(r, marker)) {
                    free(src);
                    return false;
                }
            }
        } else {
            if (!import_append_str(r, src + line_start, line_len) ||
                (has_nl && !import_append_str(r, "\n", 1))) {
                free(src);
                return false;
            }
        }
        line_num++;
    }

    free(src);
    return true;
}

char* vortex_resolve_imports(const char* entry_path) {
    if (!entry_path || !entry_path[0]) {
        fprintf(stderr, "[VORTEX ERROR] Cannot open file '%s'\n", entry_path ? entry_path : "");
        return NULL;
    }

    VortexImportResolver r;
    memset(&r, 0, sizeof(r));
    if (!import_append_cstr(&r, "")) {
        free(r.out_buf);
        return NULL;
    }

    bool ok = import_resolve_file_recursive(&r, entry_path, true);
    for (int i = 0; i < r.count; i++) {
        free(r.paths[i]);
    }
    free(r.paths);

    if (!ok) {
        free(r.out_buf);
        return NULL;
    }
    return r.out_buf;
}

int execute_vortex_file(const char* filename) {
    char* buffer = vortex_resolve_imports(filename);
    if (!buffer) {
        return 1;
    }

    const char* prev_file = g_current_filename;
    g_current_filename = filename ? filename : "<file>";
    int res = execute_vortex_source(buffer);
    g_current_filename = prev_file;
    free(buffer);
    return res;
}


