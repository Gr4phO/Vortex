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
#include <dirent.h>
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
int g_ultrasafe_level = 0;
bool g_json_errors = false;
bool g_check_only = false;
bool g_recover_active = false;
jmp_buf g_recover_jmp;
VortexDiagnostic g_diags[MAX_DIAGNOSTICS];
int g_diag_count = 0;
int g_diag_error_count = 0;
int g_diag_warn_count = 0;
VortexEnumType g_enum_types[MAX_ENUM_TYPES];
int g_enum_type_count = 0;
static int64_t g_ultrasafe_call_depth = 0;
VortexTargetArch g_target_arch = VORTEX_HOST_ARCH;
VortexTargetABI  g_target_abi  = VORTEX_HOST_ABI;
const char* g_current_filename = "<input>";
const char* g_dump_jit_path = NULL;
bool g_headless_mode = false;
bool g_is_packaged_app = false;
bool g_host_mode = false;
static char g_host_last_err[2048] = {0};
static void* s_host_exec_mem = NULL;
void* g_vortex_dll_handle = NULL;

typedef struct {
    int merged_start_line;
    int merged_end_line;
    int orig_start_line;
    int file_id;
    bool is_stdlib;
    bool is_imported;
} VortexSourceSpan;

typedef struct {
    const char* source_ptr;
    char** files;
    int file_count;
    int file_cap;
    VortexSourceSpan* spans;
    int span_count;
    int span_cap;
} VortexSourceMap;

static VortexSourceMap g_source_map = {0};
static bool g_source_map_active = false;

#define MAX_CALL_SITES 16384
typedef struct {
    char name[64];
    bool called_in_unsafe;
    const char* file;
    int line;
    int col;
} VortexCallSite;

static VortexCallSite g_call_sites[MAX_CALL_SITES];
static int g_call_site_count = 0;

void vortex_record_user_call_site(const char* name, bool called_in_unsafe, const char* file, int line, int col) {
    if (!name || !name[0] || g_call_site_count >= MAX_CALL_SITES) return;
    VortexCallSite* cs = &g_call_sites[g_call_site_count++];
    vx_str_copy(cs->name, sizeof(cs->name), name);
    cs->called_in_unsafe = called_in_unsafe;
    cs->file = file;
    cs->line = line;
    cs->col = (col > 0) ? col : 1;
}

void vortex_source_map_reset(void) {
    for (int i = 0; i < g_source_map.file_count; i++) {
        free(g_source_map.files[i]);
    }
    free(g_source_map.files);
    free(g_source_map.spans);
    memset(&g_source_map, 0, sizeof(g_source_map));
    g_source_map_active = false;
}

static int source_map_intern_file(const char* path) {
    const char* p = (path && path[0]) ? path : "<input>";
    for (int i = 0; i < g_source_map.file_count; i++) {
        if (strcmp(g_source_map.files[i], p) == 0) return i;
    }
    if (g_source_map.file_count >= g_source_map.file_cap) {
        int ncap = (g_source_map.file_cap == 0) ? 16 : (g_source_map.file_cap * 2);
        char** nf = (char**)realloc(g_source_map.files, (size_t)ncap * sizeof(char*));
        if (!nf) return -1;
        g_source_map.files = nf;
        g_source_map.file_cap = ncap;
    }
    size_t len = strlen(p);
    char* copy = (char*)malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, p, len + 1);
    g_source_map.files[g_source_map.file_count] = copy;
    return g_source_map.file_count++;
}

static void source_map_record_line(int merged_line, int orig_line, int file_id, bool is_stdlib, bool is_imported) {
    if (file_id < 0 || merged_line <= 0 || orig_line <= 0) return;
    if (g_source_map.span_count > 0) {
        VortexSourceSpan* last = &g_source_map.spans[g_source_map.span_count - 1];
        if (last->file_id == file_id &&
            last->is_stdlib == is_stdlib &&
            last->is_imported == is_imported &&
            last->merged_end_line + 1 == merged_line &&
            last->orig_start_line + (merged_line - last->merged_start_line) == orig_line) {
            last->merged_end_line = merged_line;
            return;
        }
    }
    if (g_source_map.span_count >= g_source_map.span_cap) {
        int ncap = (g_source_map.span_cap == 0) ? 32 : (g_source_map.span_cap * 2);
        VortexSourceSpan* ns = (VortexSourceSpan*)realloc(g_source_map.spans, (size_t)ncap * sizeof(VortexSourceSpan));
        if (!ns) return;
        g_source_map.spans = ns;
        g_source_map.span_cap = ncap;
    }
    VortexSourceSpan* sp = &g_source_map.spans[g_source_map.span_count++];
    sp->merged_start_line = merged_line;
    sp->merged_end_line = merged_line;
    sp->orig_start_line = orig_line;
    sp->file_id = file_id;
    sp->is_stdlib = is_stdlib;
    sp->is_imported = is_imported;
}

bool vortex_source_map_lookup(int merged_line, const char** out_file, int* out_line, bool* out_is_stdlib, bool* out_is_imported) {
    if (!g_source_map_active || merged_line <= 0 || g_source_map.span_count == 0) return false;
    int lo = 0;
    int hi = g_source_map.span_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const VortexSourceSpan* sp = &g_source_map.spans[mid];
        if (merged_line < sp->merged_start_line) {
            hi = mid - 1;
        } else if (merged_line > sp->merged_end_line) {
            lo = mid + 1;
        } else {
            if (out_file && sp->file_id >= 0 && sp->file_id < g_source_map.file_count) {
                *out_file = g_source_map.files[sp->file_id];
            }
            if (out_line) {
                *out_line = sp->orig_start_line + (merged_line - sp->merged_start_line);
            }
            if (out_is_stdlib) *out_is_stdlib = sp->is_stdlib;
            if (out_is_imported) *out_is_imported = sp->is_imported;
            return true;
        }
    }
    return false;
}

static void map_diag_location(const char** file_io, int* line_io) {
    if (!file_io || !line_io) return;
    const char* f = *file_io;
    int ln = *line_io;
    if (g_source_map_active && ln > 0 &&
        (!f || !f[0] || (g_current_filename && strcmp(f, g_current_filename) == 0))) {
        const char* mapped_file = NULL;
        int mapped_line = 0;
        if (vortex_source_map_lookup(ln, &mapped_file, &mapped_line, NULL, NULL)) {
            if (mapped_file && mapped_file[0]) *file_io = mapped_file;
            if (mapped_line > 0) *line_io = mapped_line;
        }
    }
}

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

void vortex_diag_reset(void) {
    g_diag_count = 0;
    g_diag_error_count = 0;
    g_diag_warn_count = 0;
    g_ultrasafe_call_depth = 0;
}

static int extract_line_from_msg(const char* msg) {
    if (!msg) return 0;
    const char* p = strstr(msg, "line ");
    if (p) {
        int ln = atoi(p + 5);
        if (ln > 0) return ln;
    }
    const char* c = msg;
    while ((c = strchr(c, ':')) != NULL) {
        if (isdigit((unsigned char)c[1])) {
            int ln = atoi(c + 1);
            if (ln > 0) return ln;
        }
        c++;
    }
    return 0;
}

static const char* strip_vortex_diag_prefix(const char* msg) {
    if (!msg) return "";
    if (strncmp(msg, "[VORTEX ", 8) == 0) {
        const char* close = strchr(msg + 8, ']');
        if (close && (close - msg) < 48) {
            const char* p = close + 1;
            while (*p == ' ') p++;
            if (*p) return p;
        }
    }
    return msg;
}

void vortex_diag_add(VortexDiagSeverity sev, const char* code, const char* file, int line, int col, const char* msg, const char* suggestion) {
    const char* clean_msg = strip_vortex_diag_prefix(msg);
    if (line <= 0 && clean_msg[0]) line = extract_line_from_msg(clean_msg);
    if (col <= 0) col = 1;
    const char* f = (file && file[0]) ? file : (g_current_filename ? g_current_filename : "<input>");

    if (g_source_map_active && line > 0 &&
        (!file || !file[0] || (g_current_filename && strcmp(f, g_current_filename) == 0))) {
        const char* mapped_file = NULL;
        int mapped_line = 0;
        bool is_stdlib = false;
        bool is_imported = false;
        if (vortex_source_map_lookup(line, &mapped_file, &mapped_line, &is_stdlib, &is_imported)) {
            if (sev == VDIAG_WARNING) {
                if (is_stdlib) return;
                if (is_imported && code && strcmp(code, "W105") == 0) return;
            }
            if (mapped_file && mapped_file[0]) f = mapped_file;
            if (mapped_line > 0) line = mapped_line;
        }
    }

    /* Deduplicate identical code+file+line diagnostics (and collapse duplicate E301 on same line) */
    for (int i = 0; i < g_diag_count; i++) {
        if (g_diags[i].line == line && g_diags[i].severity == sev &&
            strcmp(g_diags[i].file, f) == 0 &&
            ((code && strcmp(code, "E301") == 0 && strcmp(g_diags[i].code, "E301") == 0) ||
             (strcmp(g_diags[i].code, code ? code : "") == 0 &&
              strcmp(g_diags[i].message, clean_msg) == 0))) {
            return;
        }
    }

    if (sev == VDIAG_ERROR) g_diag_error_count++;
    else if (sev == VDIAG_WARNING) g_diag_warn_count++;

    if (g_diag_count >= MAX_DIAGNOSTICS) return;
    VortexDiagnostic* d = &g_diags[g_diag_count++];
    d->severity = sev;
    vx_str_copy(d->code, sizeof(d->code), code ? code : "E000");
    vx_str_copy(d->file, sizeof(d->file), f);
    d->line = line;
    d->col = col;
    vx_str_copy(d->message, sizeof(d->message), clean_msg);
    vx_str_copy(d->suggestion, sizeof(d->suggestion), suggestion ? suggestion : "");
}

static void json_print_escaped(FILE* out, const char* s) {
    fputc('"', out);
    if (s) {
        for (const char* p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') fputs("\\\"", out);
            else if (c == '\\') fputs("\\\\", out);
            else if (c == '\n') fputs("\\n", out);
            else if (c == '\r') fputs("\\r", out);
            else if (c == '\t') fputs("\\t", out);
            else if (c < 32) fprintf(out, "\\u%04x", c);
            else fputc(c, out);
        }
    }
    fputc('"', out);
}

static int diag_cmp(const VortexDiagnostic* a, const VortexDiagnostic* b) {
    int fc = strcmp(a->file, b->file);
    if (fc != 0) return fc;
    if (a->line != b->line) return a->line - b->line;
    return a->col - b->col;
}

void vortex_diag_sort(void) {
    for (int i = 1; i < g_diag_count; i++) {
        VortexDiagnostic key = g_diags[i];
        int j = i - 1;
        while (j >= 0 && diag_cmp(&g_diags[j], &key) > 0) {
            g_diags[j + 1] = g_diags[j];
            j--;
        }
        g_diags[j + 1] = key;
    }
}

void vortex_diag_print_all(void) {
    vortex_diag_sort();

    if (g_json_errors) {
        FILE* out = (g_diag_error_count > 0 || g_diag_warn_count > 0) ? stderr : stdout;
        fprintf(out, "{\n  \"version\": \"%s\",\n  \"ultrasafe\": %d,\n  \"ok\": %s,\n  \"errors\": %d,\n  \"warnings\": %d,\n  \"diagnostics\": [\n",
                VORTEX_VERSION, g_ultrasafe_level, (g_diag_error_count == 0) ? "true" : "false",
                g_diag_error_count, g_diag_warn_count);
        for (int i = 0; i < g_diag_count; i++) {
            const VortexDiagnostic* d = &g_diags[i];
            const char* sev = (d->severity == VDIAG_ERROR) ? "error" : (d->severity == VDIAG_WARNING ? "warning" : "info");
            fprintf(out, "    {\"severity\": \"%s\", \"code\": ", sev);
            json_print_escaped(out, d->code);
            fprintf(out, ", \"file\": ");
            json_print_escaped(out, d->file);
            fprintf(out, ", \"line\": %d, \"col\": %d, \"message\": ", d->line, d->col);
            json_print_escaped(out, d->message);
            fprintf(out, ", \"suggestion\": ");
            json_print_escaped(out, d->suggestion);
            fprintf(out, "}%s\n", (i + 1 < g_diag_count) ? "," : "");
        }
        fprintf(out, "  ]\n}\n");
        fflush(out);
        return;
    }

    for (int i = 0; i < g_diag_count; i++) {
        const VortexDiagnostic* d = &g_diags[i];
        const char* tag = (d->severity == VDIAG_ERROR) ? "ERROR" : (d->severity == VDIAG_WARNING ? "WARNING" : "INFO");
        vortex_diag_emit(stderr, "[VORTEX %s %s] %s:%d:%d: %s\n",
                         tag, d->code, d->file, d->line, d->col, d->message);
        if (d->suggestion[0]) {
            vortex_diag_emit(stderr, "  help: %s\n", d->suggestion);
        }
    }
    if (g_diag_count > 0) {
        vortex_diag_emit(stderr, "[VORTEX ULTRASAFE %d] Summary: %d error(s), %d warning(s).\n",
                         g_ultrasafe_level, g_diag_error_count, g_diag_warn_count);
    }
    fflush(stderr);
}

void vortex_ultrasafe_div_zero(const char* file, int line) {
    const char* out_file = file;
    int out_line = line;
    map_diag_location(&out_file, &out_line);
    char msg[256];
    snprintf(msg, sizeof(msg), "[VORTEX SAFETY PANIC] Division or modulo by zero at %s:%d",
             out_file ? out_file : "<unknown>", out_line);
    vortex_error(msg);
}

void vortex_ultrasafe_check_recursion(int64_t delta, const char* fn_name, const char* file, int line) {
    if (delta > 0) {
        g_ultrasafe_call_depth++;
        if (g_ultrasafe_call_depth > 2048) {
            g_ultrasafe_call_depth = 0;
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "[VORTEX SAFETY PANIC] Call/recursion depth exceeded 2048 in fn '%s' at %s:%d",
                     fn_name ? fn_name : "<fn>", file ? file : "<unknown>", line);
            vortex_error(msg);
        }
    } else {
        if (g_ultrasafe_call_depth > 0) g_ultrasafe_call_depth--;
    }
}

void vortex_ultrasafe_audit_ir(const IRBuffer* ir, const SymbolTable* st, const char* fn_name, VortexType ret_type, int fn_line) {
    if (!ir || g_ultrasafe_level < 1) return;

    int n_vregs = ir->vreg_count > 0 ? ir->vreg_count : 1;
    bool* has_const = (bool*)calloc((size_t)n_vregs, sizeof(bool));
    int64_t* const_val = (int64_t*)calloc((size_t)n_vregs, sizeof(int64_t));
    bool* known_null = (bool*)calloc((size_t)n_vregs, sizeof(bool));
    int64_t* slice_len = (int64_t*)malloc((size_t)n_vregs * sizeof(int64_t));
    if (slice_len) {
        for (int i = 0; i < n_vregs; i++) slice_len[i] = -1;
    }

    bool after_term = false;
    bool warn_unreach_emitted = false;
    bool has_return = false;

    for (int i = 0; i < ir->count; i++) {
        const IRInstruction* ins = &ir->instrs[i];
        if (ins->op == IR_LABEL) {
            after_term = false;
            warn_unreach_emitted = false;
            if (has_const) memset(has_const, 0, (size_t)n_vregs * sizeof(bool));
            if (known_null) memset(known_null, 0, (size_t)n_vregs * sizeof(bool));
            continue;
        }
        if (ins->op == IR_NOP) continue;

        if (after_term && !warn_unreach_emitted && ins->line > 0) {
            vortex_diag_add(VDIAG_WARNING, "W101", ins->file ? ins->file : g_current_filename,
                            ins->line, ins->col > 0 ? ins->col : 1, "Unreachable code after return/break/continue",
                            "Remove dead statements or move them before the control-flow jump.");
            warn_unreach_emitted = true;
        }

        if (ins->op == IR_RET) {
            has_return = true;
            after_term = true;
        } else if (ins->op == IR_JMP) {
            after_term = true;
        }

        if (ins->op == IR_CONST_I64 && ins->dest >= 0 && ins->dest < n_vregs) {
            if (has_const) { has_const[ins->dest] = true; const_val[ins->dest] = ins->imm; }
            if (known_null) known_null[ins->dest] = (ins->imm == 0);
        } else if (ins->op == IR_MOV && ins->dest >= 0 && ins->dest < n_vregs) {
            if (ins->src1 >= 0 && ins->src1 < n_vregs) {
                if (has_const && has_const[ins->src1]) {
                    has_const[ins->dest] = true;
                    const_val[ins->dest] = const_val[ins->src1];
                } else if (has_const) {
                    has_const[ins->dest] = false;
                }
                if (known_null) known_null[ins->dest] = known_null[ins->src1];
                if (slice_len) slice_len[ins->dest] = slice_len[ins->src1];
            } else {
                if (has_const) has_const[ins->dest] = false;
                if (known_null) known_null[ins->dest] = false;
                if (slice_len) slice_len[ins->dest] = -1;
            }
        } else if (ins->op == IR_CALL &&
                   (ins->func_ptr == (void*)vortex_alloc_slice_i64 ||
                    ins->func_ptr == (void*)vortex_alloc_slice_f32 ||
                    ins->func_ptr == (void*)vortex_alloc_slice_i32) &&
                   ins->dest >= 0 && ins->dest < n_vregs && ins->arg_count >= 1) {
            VReg lv = ins->args[0];
            if (lv >= 0 && lv < n_vregs && has_const && has_const[lv]) {
                if (g_ultrasafe_level >= 2 && const_val[lv] < 0) {
                    char msg[160];
                    snprintf(msg, sizeof(msg), "Negative slice allocation length (%lld) in '%s'",
                             (long long)const_val[lv], fn_name ? fn_name : "__main__");
                    vortex_diag_add(VDIAG_ERROR, "E202", ins->file ? ins->file : g_current_filename,
                                    ins->line, ins->col > 0 ? ins->col : 1, msg, "Use a non-negative integer length >= 0.");
                }
                if (slice_len) slice_len[ins->dest] = const_val[lv];
            }
        } else if (ins->op != IR_NOP && ins->dest >= 0 && ins->dest < n_vregs) {
            if (has_const) has_const[ins->dest] = false;
            if (known_null) known_null[ins->dest] = false;
            if (slice_len) slice_len[ins->dest] = -1;
        }

        if (g_ultrasafe_level >= 2) {
            /* Static literal division/modulo by zero */
            if ((ins->op == IR_DIV || ins->op == IR_MOD) &&
                ins->src2 >= 0 && ins->src2 < n_vregs && has_const && has_const[ins->src2] && const_val[ins->src2] == 0) {
                char msg[160];
                snprintf(msg, sizeof(msg), "Static %s by zero detected in '%s'",
                         (ins->op == IR_MOD) ? "modulo" : "division", fn_name ? fn_name : "__main__");
                vortex_diag_add(VDIAG_ERROR, "E201", ins->file ? ins->file : g_current_filename,
                                ins->line, ins->col > 0 ? ins->col : 1, msg, "Ensure the divisor is non-zero before dividing.");
            }
            /* Static constant out-of-bounds array access */
            if ((ins->op == IR_LOAD_ARRAY || ins->op == IR_STORE_ARRAY) &&
                ins->src1 >= 0 && ins->src1 < n_vregs && ins->src2 >= 0 && ins->src2 < n_vregs &&
                has_const && has_const[ins->src2]) {
                int64_t idx = const_val[ins->src2];
                int64_t klen = slice_len ? slice_len[ins->src1] : -1;
                if (klen < 0) klen = ir->vregs[ins->src1].known_len;
                if (idx < 0 || (klen > 0 && idx >= klen)) {
                    char msg[192];
                    snprintf(msg, sizeof(msg), "Static out-of-bounds index %lld (length %lld) in '%s'",
                             (long long)idx, (long long)klen, fn_name ? fn_name : "__main__");
                    vortex_diag_add(VDIAG_ERROR, "E202", ins->file ? ins->file : g_current_filename,
                                    ins->line, ins->col > 0 ? ins->col : 1, msg, "Check the array index bounds: 0 <= idx < len.");
                }
            }
            /* Static null dereference / null field access */
            if ((ins->op == IR_LOAD_DEREF || ins->op == IR_STORE_DEREF ||
                 ins->op == IR_LOAD_OFFSET || ins->op == IR_STORE_OFFSET) &&
                ins->src1 >= 0 && ins->src1 < n_vregs && known_null && known_null[ins->src1]) {
                vortex_diag_add(VDIAG_ERROR, "E203", ins->file ? ins->file : g_current_filename,
                                ins->line, ins->col > 0 ? ins->col : 1, "Static null pointer dereference (pointer is 0)",
                                "Validate the pointer against 0 before dereferencing or accessing fields.");
            }
        }
    }

    if (g_ultrasafe_level >= 2 && ret_type != TYPE_VOID && ret_type != TYPE_UNKNOWN && !has_return &&
        fn_name && strcmp(fn_name, "__main__") != 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Function '%s' declares a non-void return type but has no return statement", fn_name);
        vortex_diag_add(VDIAG_ERROR, "E204", g_current_filename, fn_line, 1, msg,
                        "Add a 'return <value>' statement on all control-flow paths.");
    }

    if (st) {
        vortex_ultrasafe_audit_symbols(st, 0, st->count, fn_name);
    }

    free(has_const);
    free(const_val);
    free(known_null);
    free(slice_len);
}

void vortex_ultrasafe_audit_symbols(const SymbolTable* st, int start_idx, int end_idx, const char* fn_name) {
    if (!st || g_ultrasafe_level < 1) return;
    if (start_idx < 0) start_idx = 0;
    if (end_idx > st->count) end_idx = st->count;
    for (int i = start_idx; i < end_idx; i++) {
        const Symbol* s = &st->symbols[i];
        if (s->name[0] == '_' || s->decl_line <= 0) continue;
        if (!s->used) {
            char msg[160];
            char help[160];
            snprintf(msg, sizeof(msg), "Unused local variable '%s' in '%s'", s->name, fn_name && fn_name[0] ? fn_name : "__main__");
            snprintf(help, sizeof(help), "Prefix with '_' (e.g. '_%s') or remove the unused variable.", s->name);
            vortex_diag_add(VDIAG_WARNING, "W104", g_current_filename, s->decl_line, s->decl_col, msg, help);
        } else if (s->is_mut && !s->reassigned) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Variable '%s' declared 'mut' is never reassigned", s->name);
            vortex_diag_add(VDIAG_WARNING, "W102", g_current_filename, s->decl_line, s->decl_col, msg,
                            "Remove 'mut' so the binding remains immutable.");
        }
    }
}

void vortex_ultrasafe_audit_program(void) {
    if (g_ultrasafe_level < 2) return;
    for (int i = 0; i < g_user_func_count; i++) {
        const UserFunc* uf = &g_user_funcs[i];
        if (!uf->called && uf->decl_line > 0 &&
            strcmp(uf->name, "main") != 0 &&
            strncmp(uf->name, "test_", 5) != 0) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Function '%s' is defined but never called", uf->name);
            vortex_diag_add(VDIAG_WARNING, "W105", g_current_filename, uf->decl_line, 1, msg,
                            "Remove unused functions or invoke them from main/tests.");
        }
    }
}

int64_t vortex_assert(int64_t cond) {
    if (!cond) {
        vortex_error("[VORTEX ASSERT FAILED] assert(cond) evaluated to false (0)");
    }
    return 1;
}

int64_t vortex_assert_eq(int64_t a, int64_t b) {
    if (a != b) {
        char msg[200];
        snprintf(msg, sizeof(msg), "[VORTEX ASSERT FAILED] assert_eq failed: expected %lld == %lld",
                 (long long)a, (long long)b);
        vortex_error(msg);
    }
    return 1;
}

void vortex_compile_error(const char* code, int line, int col, const char* msg, const char* suggestion) {
    const char* c = (code && code[0]) ? code : "E001";
    const char* m = (msg && msg[0]) ? msg : "Syntax or compilation error";
    if (g_recover_active && g_ultrasafe_level >= 3) {
        vortex_diag_add(VDIAG_ERROR, c, g_current_filename, line, col > 0 ? col : 1, m, suggestion);
        longjmp(g_recover_jmp, 1);
    }
    const char* out_file = g_current_filename ? g_current_filename : "<input>";
    int out_line = line;
    map_diag_location(&out_file, &out_line);
    char full[512];
    if (out_line > 0) {
        snprintf(full, sizeof(full), "%s at %s:%d:%d", m, out_file, out_line, col > 0 ? col : 1);
    } else {
        vx_str_copy(full, sizeof(full), m);
    }
    vortex_error(full);
}

void vortex_error(const char* msg) {
    if (g_recover_active && g_ultrasafe_level >= 3) {
        const char* m = (msg && msg[0]) ? msg : "Syntax or compilation error";
        vortex_diag_add(VDIAG_ERROR, "E001", g_current_filename, 0, 1, m, NULL);
        longjmp(g_recover_jmp, 1);
    }
    if (msg && msg[0]) {
        vx_str_copy(g_host_last_err, sizeof(g_host_last_err), msg);
        if (g_json_errors) {
            vortex_diag_add(VDIAG_ERROR, "E001", g_current_filename, 0, 1, msg, NULL);
            vortex_diag_print_all();
        } else if (g_lsp_capture_errors) {
            vortex_diag_emit(stderr, "%s\n", msg);
        } else {
            fprintf(stderr, "%s\n", msg);
        }
    }
    if (!g_in_repl && !g_lsp_capture_errors && !g_host_mode && !g_json_errors) {
        vortex_print_stacktrace(NULL);
    }
    fflush(stderr);
    if (g_in_repl || g_lsp_capture_errors || g_host_mode) {
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
void helper_print_f64(double val) { printf("%.6f\n", val); fflush(stdout); }
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

float vortex_math_sinf(float rad) {
    return sinf(rad);
}

float vortex_math_cosf(float rad) {
    return cosf(rad);
}

float vortex_math_sqrtf(float x) {
    return x > 0.0f ? sqrtf(x) : 0.0f;
}

float vortex_math_tanf(float rad) {
    return tanf(rad);
}

float vortex_math_floorf(float x) {
    return floorf(x);
}

float vortex_math_ceilf(float x) {
    return ceilf(x);
}

float vortex_math_fabsf(float x) {
    return fabsf(x);
}

float vortex_math_expf(float x) {
    return expf(x);
}

float vortex_math_logf(float x) {
    return logf(x);
}

float vortex_math_powf(float base, float exp) {
    return powf(base, exp);
}

float vortex_math_atan2f(float y, float x) {
    return atan2f(y, x);
}

double vortex_math_sin_f64(double rad) { return sin(rad); }
double vortex_math_cos_f64(double rad) { return cos(rad); }
double vortex_math_sqrt_f64(double x) { return x > 0.0 ? sqrt(x) : 0.0; }
double vortex_math_tan_f64(double rad) { return tan(rad); }
double vortex_math_floor_f64(double x) { return floor(x); }
double vortex_math_ceil_f64(double x) { return ceil(x); }
double vortex_math_fabs_f64(double x) { return fabs(x); }
double vortex_math_exp_f64(double x) { return exp(x); }
double vortex_math_log_f64(double x) { return log(x); }
double vortex_math_pow_f64(double base, double exp) { return pow(base, exp); }
double vortex_math_atan2_f64(double y, double x) { return atan2(y, x); }

static int g_program_argc = 0;
static char** g_program_argv = NULL;

void vortex_set_program_args(int argc, char** argv) {
    g_program_argc = (argc > 0) ? argc : 0;
    g_program_argv = argv;
}

static int64_t vortex_arena_dup_cstr(const char* s) {
    if (!s || !s[0]) return (int64_t)"";
    size_t len = strlen(s);
    char* res = (char*)arena_alloc_compact(len + 1);
    if (!res) return (int64_t)"";
    memcpy(res, s, len + 1);
    return (int64_t)res;
}

int64_t vortex_args_count(void) {
    return (int64_t)g_program_argc;
}

int64_t vortex_args_get(int64_t idx) {
    if (!g_program_argv || idx < 0 || idx >= (int64_t)g_program_argc) {
        return (int64_t)"";
    }
    return vortex_arena_dup_cstr(g_program_argv[(int)idx]);
}

int64_t vortex_env_get(int64_t name_ptr) {
    const char* name = (const char*)name_ptr;
    if (!name || !name[0]) return (int64_t)"";
    const char* val = getenv(name);
    return vortex_arena_dup_cstr(val);
}

int64_t vortex_env_has(int64_t name_ptr) {
    const char* name = (const char*)name_ptr;
    if (!name || !name[0]) return 0;
    return getenv(name) != NULL ? 1 : 0;
}

int64_t vortex_fs_read(int64_t path_ptr) {
    const char* path = (const char*)path_ptr;
    if (!path || !path[0]) return (int64_t)"";
    FILE* f = fopen(path, "rb");
    if (!f) return (int64_t)"";
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return (int64_t)"";
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return (int64_t)"";
    }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)arena_alloc_compact((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return (int64_t)"";
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return (int64_t)buf;
}

int64_t vortex_fs_write(int64_t path_ptr, int64_t content_ptr) {
    const char* path = (const char*)path_ptr;
    const char* content = (const char*)content_ptr;
    if (!path || !path[0]) return 0;
    if (!content) content = "";
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t len = strlen(content);
    size_t written = (len > 0) ? fwrite(content, 1, len, f) : 0;
    int cerr = ferror(f);
    fclose(f);
    return (written == len && cerr == 0) ? 1 : 0;
}

int64_t vortex_fs_append(int64_t path_ptr, int64_t content_ptr) {
    const char* path = (const char*)path_ptr;
    const char* content = (const char*)content_ptr;
    if (!path || !path[0]) return 0;
    if (!content) content = "";
    FILE* f = fopen(path, "ab");
    if (!f) return 0;
    size_t len = strlen(content);
    size_t written = (len > 0) ? fwrite(content, 1, len, f) : 0;
    int cerr = ferror(f);
    fclose(f);
    return (written == len && cerr == 0) ? 1 : 0;
}

int64_t vortex_fs_exists(int64_t path_ptr) {
    const char* path = (const char*)path_ptr;
    if (!path || !path[0]) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int64_t vortex_fs_remove(int64_t path_ptr) {
    const char* path = (const char*)path_ptr;
    if (!path || !path[0]) return 0;
    return (remove(path) == 0) ? 1 : 0;
}

int64_t vortex_sleep_ms(int64_t ms) {
    if (ms <= 0) return 0;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000LL);
    nanosleep(&ts, NULL);
#endif
    return 0;
}

int64_t vortex_unix_now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    return (int64_t)((ticks - 116444736000000000ULL) / 10000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000LL);
#endif
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
    if (w->affinity_mask != 0) {
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)w->affinity_mask);
    }
    for (;;) {
        int spins = 0;
        while (InterlockedCompareExchange(&w->has_work, 0, 0) == 0) {
            if (w->should_exit) return 0;
            if (spins < 4000) {
                YieldProcessor();
                spins++;
            } else {
                InterlockedExchange(&w->sleeping, 1);
                if (InterlockedCompareExchange(&w->has_work, 0, 0) == 0 && !w->should_exit) {
                    WaitForSingleObject(w->start_event, INFINITE);
                }
                InterlockedExchange(&w->sleeping, 0);
            }
        }
        if (w->should_exit) break;
        if (w->job_fn) {
            w->job_fn(w->job_ctx, w->slice_start, w->slice_end);
        }
        InterlockedExchange(&w->has_work, 0);
        MemoryBarrier();
        InterlockedDecrement(&g_thread_pool.remaining);
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
    g_thread_pool.active_worker_limit = 0;
#ifdef _WIN32
    g_thread_pool.remaining = 0;
#endif

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
        w->has_work = 0;
        w->sleeping = 0;
        w->affinity_mask = 0;
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

    /* Prioritize physical cores for primary workers using detected hw topology (zero OpenCL load). */
    vortex_hw_init_cpu_only();
#ifdef _WIN32
    int64_t phys = vortex_hw_get_phys_cores();
    if (phys > 0 && count > 1 && vortex_hw_get_core_mask(0) != 0) {
        int worker_slots = count - 1;
        int widx = 0;
        for (int c = 0; c < phys && widx < worker_slots; c++, widx++) {
            uint64_t mask = vortex_hw_get_core_mask(c);
            uint64_t primary_bit = mask & (~mask + 1ULL);
            if (primary_bit != 0) vortex_thread_pool_set_worker_mask(widx, primary_bit);
        }
        for (int c = 0; c < phys && widx < worker_slots; c++) {
            uint64_t mask = vortex_hw_get_core_mask(c);
            uint64_t primary_bit = mask & (~mask + 1ULL);
            uint64_t smt_bits = mask & ~primary_bit;
            if (smt_bits != 0) {
                vortex_thread_pool_set_worker_mask(widx, smt_bits);
                widx++;
            }
        }
    }
#endif
}

void vortex_parallel_for_dispatch(void* fn_ptr, void* ctx, int64_t start, int64_t end) {
    ParallelJobFn fn = (ParallelJobFn)fn_ptr;
    if (!g_thread_pool.initialized) {
        vortex_thread_pool_init();
    }
    int count = g_thread_pool.worker_count;
    if (g_thread_pool.active_worker_limit > 0 && g_thread_pool.active_worker_limit < count) {
        count = g_thread_pool.active_worker_limit;
    }
    int64_t total = end - start;
    if (total <= 0) return;

    if (total < count || count <= 1) {
        fn(ctx, start, end);
        return;
    }

    /* Align worker chunk boundaries to 64-byte cache-line multiples to eliminate false sharing. */
    int64_t align_step = 1;
    if (total >= (int64_t)count * 64LL) align_step = 16;
    else if (total >= (int64_t)count * 16LL) align_step = 8;

    int64_t blocks = total / align_step;
    int64_t chunk = blocks / count;
    int64_t rem = blocks % count;
    int worker_dispatch = count - 1;

#ifdef _WIN32
    InterlockedExchange(&g_thread_pool.remaining, (LONG)worker_dispatch);
#endif

    int64_t cur = start;
    for (int i = 0; i < worker_dispatch; i++) {
        int64_t this_chunk = (chunk + (i < rem ? 1 : 0)) * align_step;
        VortexWorker* w = &g_thread_pool.workers[i];
        w->job_fn = fn;
        w->job_ctx = ctx;
        w->slice_start = cur;
        w->slice_end = cur + this_chunk;
        cur += this_chunk;
#ifdef _WIN32
        MemoryBarrier();
        InterlockedExchange(&w->has_work, 1);
        if (InterlockedCompareExchange(&w->sleeping, 0, 0) != 0) {
            SetEvent(w->start_event);
        }
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
    while (InterlockedCompareExchange(&g_thread_pool.remaining, 0, 0) > 0) {
        YieldProcessor();
    }
    MemoryBarrier();
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

void vortex_thread_pool_set_worker_mask(int worker_id, uint64_t mask) {
    if (!g_thread_pool.initialized) vortex_thread_pool_init();
    if (worker_id < 0 || worker_id >= g_thread_pool.worker_count) return;
#ifdef _WIN32
    g_thread_pool.workers[worker_id].affinity_mask = (uintptr_t)mask;
    if (mask != 0 && g_thread_pool.workers[worker_id].thread) {
        SetThreadAffinityMask(g_thread_pool.workers[worker_id].thread, (DWORD_PTR)mask);
    }
#else
    (void)mask;
#endif
}

int64_t vortex_hw_set_worker_pinning(int64_t mode) {
    if (!g_thread_pool.initialized) vortex_thread_pool_init();
    int count = g_thread_pool.worker_count;
#ifdef _WIN32
    if (mode <= 0) {
        g_thread_pool.active_worker_limit = 0;
        DWORD_PTR proc_mask = 0, sys_mask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask);
        if (proc_mask == 0) proc_mask = (count >= 64) ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << count) - 1);
        for (int i = 0; i < count; i++) {
            vortex_thread_pool_set_worker_mask(i, (uint64_t)proc_mask);
        }
        SetThreadAffinityMask(GetCurrentThread(), proc_mask);
        return (int64_t)count;
    }
    if (mode == 2 && count >= 2) {
        /* Physical cores only: assumes SMT pairs at (0,1), (2,3)... if caller didn't override */
        int phys = count / 2;
        if (phys < 1) phys = 1;
        g_thread_pool.active_worker_limit = phys;
        for (int i = 0; i < phys - 1; i++) {
            vortex_thread_pool_set_worker_mask(i, 1ULL << (i * 2));
        }
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)(1ULL << ((phys - 1) * 2)));
        return (int64_t)phys;
    }
    /* mode == 1: 1-to-1 pin all logical workers + caller */
    g_thread_pool.active_worker_limit = 0;
    for (int i = 0; i < count - 1; i++) {
        vortex_thread_pool_set_worker_mask(i, 1ULL << i);
    }
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)(1ULL << (count - 1)));
    return (int64_t)count;
#else
    (void)mode;
    return (int64_t)count;
#endif
}

int64_t vortex_num_workers(void) {
    if (!g_thread_pool.initialized) vortex_thread_pool_init();
    if (g_thread_pool.active_worker_limit > 0 && g_thread_pool.active_worker_limit < g_thread_pool.worker_count) {
        return (int64_t)g_thread_pool.active_worker_limit;
    }
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
    {"str_from_f64",   (void*)vortex_str_from_f64,   1},
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
    {"slice_f64",      (void*)vortex_alloc_slice_f64,1},
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
    {"sinf_rad",       (void*)vortex_math_sinf,      1},
    {"cosf_rad",       (void*)vortex_math_cosf,      1},
    {"sqrtf_val",      (void*)vortex_math_sqrtf,     1},
    {"tanf_rad",       (void*)vortex_math_tanf,      1},
    {"floorf_val",     (void*)vortex_math_floorf,    1},
    {"ceilf_val",      (void*)vortex_math_ceilf,     1},
    {"fabsf_val",      (void*)vortex_math_fabsf,     1},
    {"expf_val",       (void*)vortex_math_expf,      1},
    {"logf_val",       (void*)vortex_math_logf,      1},
    {"powf_val",       (void*)vortex_math_powf,      2},
    {"atan2f_rad",     (void*)vortex_math_atan2f,    2},
    {"sin_f64",        (void*)vortex_math_sin_f64,   1},
    {"cos_f64",        (void*)vortex_math_cos_f64,   1},
    {"sqrt_f64",       (void*)vortex_math_sqrt_f64,  1},
    {"tan_f64",        (void*)vortex_math_tan_f64,   1},
    {"floor_f64",      (void*)vortex_math_floor_f64, 1},
    {"ceil_f64",       (void*)vortex_math_ceil_f64,  1},
    {"fabs_f64",       (void*)vortex_math_fabs_f64,  1},
    {"exp_f64",        (void*)vortex_math_exp_f64,   1},
    {"log_f64",        (void*)vortex_math_log_f64,   1},
    {"pow_f64",        (void*)vortex_math_pow_f64,   2},
    {"atan2_f64",      (void*)vortex_math_atan2_f64, 2},
    {"args_count",     (void*)vortex_args_count,     0},
    {"args_get",       (void*)vortex_args_get,       1},
    {"env_get",        (void*)vortex_env_get,        1},
    {"env_has",        (void*)vortex_env_has,        1},
    {"fs_read",        (void*)vortex_fs_read,        1},
    {"fs_write",       (void*)vortex_fs_write,       2},
    {"fs_append",      (void*)vortex_fs_append,      2},
    {"fs_exists",      (void*)vortex_fs_exists,      1},
    {"fs_remove",      (void*)vortex_fs_remove,      1},
    {"sleep_ms",       (void*)vortex_sleep_ms,       1},
    {"unix_now_ms",    (void*)vortex_unix_now_ms,    0},
    {"struct_size",    (void*)vortex_builtin_struct_size, 1},
    {"struct_align",   (void*)vortex_builtin_struct_align, 1},
    {"assert",         (void*)vortex_assert,              1},
    {"assert_eq",      (void*)vortex_assert_eq,           2},
    {"time_us",        (void*)helper_time_us,             0},
    {"num_workers",    (void*)vortex_num_workers,    0},
    {"hw_init",        (void*)vortex_hw_init,        0},
    {"hw_print_map",   (void*)vortex_hw_print_map,   0},
    {"hw_cpu_name",    (void*)vortex_hw_cpu_name,    0},
    {"hw_gpu_name",    (void*)vortex_hw_gpu_name,    0},
    {"hw_l1_kb",       (void*)vortex_hw_l1_kb,       0},
    {"hw_l2_kb",       (void*)vortex_hw_l2_kb,       0},
    {"hw_l3_kb",       (void*)vortex_hw_l3_kb,       0},
    {"hw_phys_cores",  (void*)vortex_hw_phys_cores,  0},
    {"hw_logical_cores", (void*)vortex_hw_logical_cores, 0},
    {"hw_simd_width",  (void*)vortex_hw_simd_width,  0},
    {"hw_has_gpu",     (void*)vortex_hw_has_gpu,     0},
    {"hw_plan_route",  (void*)vortex_hw_plan_route,  2},
    {"hw_plan_tile_f64", (void*)vortex_hw_plan_tile_f64, 0},
    {"hw_pin_workers", (void*)vortex_hw_pin_workers, 1},
    {"hw_prefetch",    (void*)vortex_hw_prefetch,    2},
    {"hw_matmul_f64",  (void*)vortex_hw_matmul_f64,  6},
    {"hw_matmul_f32",  (void*)vortex_hw_matmul_f32,  6},
    {"hw_matmul_f32_cpu", (void*)vortex_hw_matmul_f32_cpu, 6},
    {"hw_dot_f64",     (void*)vortex_hw_dot_f64,     3},
    {"hw_axpy_f64",    (void*)vortex_hw_axpy_f64,    4},
    {"hw_axpy_f32",    (void*)vortex_hw_axpy_f32,    4},
    {"hw_buf_from_f32",(void*)vortex_hw_buf_from_f32,2},
    {"hw_buf_from_f64",(void*)vortex_hw_buf_from_f64,2},
    {"hw_buf_sync_host",(void*)vortex_hw_buf_sync_host,1},
    {"hw_buf_mark_host_dirty",(void*)vortex_hw_buf_mark_host_dirty,1},
    {"hw_buf_state",   (void*)vortex_hw_buf_state,   1},
    {"hw_buf_free",    (void*)vortex_hw_buf_free,    1},
    {"hw_matmul_f32_buf",(void*)vortex_hw_matmul_f32_buf,6},
    {"hw_matmul_f64_buf",(void*)vortex_hw_matmul_f64_buf,6},
    {"hw_axpy_f32_buf",(void*)vortex_hw_axpy_f32_buf,4},
    {"hw_relu_f32_buf",(void*)vortex_hw_relu_f32_buf,2},
    {"hw_router_stats",(void*)vortex_hw_router_stats,0},
    {"hw_gpu_cache_hits",(void*)vortex_hw_gpu_cache_hits,0},
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
    g_source_map_active = (g_source_map.source_ptr != NULL && source == g_source_map.source_ptr);
    if (!g_in_repl && !g_lsp_capture_errors) {
        g_struct_type_count = 0;
        g_enum_type_count = 0;
        vortex_diag_reset();
    } else if (g_lsp_capture_errors) {
        g_struct_type_count = 0;
        g_enum_type_count = 0;
    }
    vortex_parser_reset_tables(!g_in_repl);
    for (int i = 0; i < g_user_func_count; i++) {
        free(g_user_funcs[i].inline_instrs);
        free(g_user_funcs[i].inline_vregs);
        g_user_funcs[i].inline_instrs = NULL;
        g_user_funcs[i].inline_vregs = NULL;
        g_user_funcs[i].can_inline = false;
    }
    g_user_func_count = 0;
    g_user_func_reloc_count = 0;
    g_call_site_count = 0;
    g_ffi_func_count = 0;
    g_literal_string_count = 0;

    Lexer lexer;
    lexer_init(&lexer, source);

    SymbolTable* symtab = (SymbolTable*)calloc(1, sizeof(SymbolTable));
    if (!symtab) {
        vortex_error("[VORTEX PANIC] Out of memory allocating SymbolTable");
    }

    JITBuffer jit;
    jit.code = (uint8_t*)exec_mem;
    jit.size = 0;
    jit.capacity = MAX_JIT_CODE_SIZE;

    if (!g_check_only && exec_mem) {
        jit_write_protect(exec_mem, MAX_JIT_CODE_SIZE, true);
    }

    IRBuffer main_ir;
    ir_init(&main_ir);
    vx_str_copy(main_ir.func_name, sizeof(main_ir.func_name), "__main__");

    Compiler comp;
    memset(&comp, 0, sizeof(comp));
    comp.lex = &lexer;
    comp.st = symtab;
    comp.j = &jit;
    comp.ir = &main_ir;
    comp.last_struct_id = -1;
    comp.last_f64_lit_vreg = VREG_NONE;

    if (g_ultrasafe_level >= 3 && !g_in_repl) {
        bool prev_rec = g_recover_active;
        jmp_buf prev_rec_jmp;
        memcpy(&prev_rec_jmp, &g_recover_jmp, sizeof(prev_rec_jmp));
        g_recover_active = true;
        int recovery_steps = 0;
        while (comp.lex->cur.type != TOK_EOF && recovery_steps < 512) {
            size_t start_pos = comp.lex->pos;
            if (setjmp(g_recover_jmp) == 0) {
                parse_statement(&comp);
            } else {
                recovery_steps++;
                comp.unsafe_depth = 0;
                comp.loop_depth = 0;
                if (comp.lex->pos == start_pos &&
                    comp.lex->cur.type != TOK_EOF &&
                    comp.lex->cur.type != TOK_SEMI) {
                    next_token(comp.lex);
                }
                vortex_lexer_sync(comp.lex, false);
            }
        }
        g_recover_active = prev_rec;
        memcpy(&g_recover_jmp, &prev_rec_jmp, sizeof(prev_rec_jmp));
    } else {
        while (comp.lex->cur.type != TOK_EOF) {
            parse_statement(&comp);
        }
    }

    g_last_symtab.count = symtab->count;
    g_last_symtab.stack_alloc_bytes = symtab->stack_alloc_bytes;
    if (symtab->count > 0) {
        memcpy(g_last_symtab.symbols, symtab->symbols, (size_t)symtab->count * sizeof(Symbol));
    }
    g_has_last_symtab = true;

    const VortexJITBackend* bk = vortex_get_backend();

    if (!vortex_is_cross_target()) {
        vortex_thread_pool_init();
    }

    bool saved_rec = g_recover_active;
    g_recover_active = (g_ultrasafe_level >= 3);
    ir_region_verify(&main_ir);
    g_recover_active = saved_rec;

    /* Audit all IR_CALL_USER sites recorded inside user function bodies */
    for (int i = 0; i < g_call_site_count; i++) {
        const VortexCallSite* cs = &g_call_sites[i];
        UserFunc* uf = user_func_find(cs->name);
        if (!uf) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[VORTEX ERROR] Unresolved user function '%s'", cs->name);
            if (g_ultrasafe_level >= 3) {
                vortex_diag_add(VDIAG_ERROR, "E101",
                                cs->file ? cs->file : g_current_filename,
                                cs->line, cs->col > 0 ? cs->col : 1, msg, "Define the function or import its module.");
                continue;
            }
            const char* err_file = cs->file ? cs->file : g_current_filename;
            int err_line = cs->line;
            map_diag_location(&err_file, &err_line);
            fprintf(stderr, "%s at %s:%d\n", msg, err_file, err_line);
            vortex_error("");
        }
        uf->called = true;
        if (uf->is_unsafe && !cs->called_in_unsafe) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[VORTEX SAFETY ERROR] Calling unsafe fn '%s' requires an unsafe block or unsafe fn", uf->name);
            if (g_ultrasafe_level >= 3) {
                vortex_diag_add(VDIAG_ERROR, "E302",
                                cs->file ? cs->file : g_current_filename,
                                cs->line, cs->col > 0 ? cs->col : 1, msg, "Wrap the call in 'unsafe { ... }' or mark the caller 'unsafe fn'.");
                continue;
            }
            const char* err_file = cs->file ? cs->file : g_current_filename;
            int err_line = cs->line;
            map_diag_location(&err_file, &err_line);
            fprintf(stderr, "%s at %s:%d\n", msg, err_file, err_line);
            vortex_error("");
        }
    }

    /* Pre-scan main_ir calls so `--check` and `vortex_ultrasafe_audit_program()` see direct/forward calls */
    for (int i = 0; i < main_ir.count; i++) {
        IRInstruction* ins = &main_ir.instrs[i];
        if (ins->op != IR_CALL_USER) continue;
        UserFunc* uf = user_func_find(ins->func_name);
        if (!uf) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[VORTEX ERROR] Unresolved user function '%s'", ins->func_name);
            if (g_ultrasafe_level >= 3) {
                vortex_diag_add(VDIAG_ERROR, "E101",
                                ins->file ? ins->file : g_current_filename,
                                ins->line, ins->col > 0 ? ins->col : 1, msg, "Define the function or import its module.");
                continue;
            }
            const char* err_file = ins->file ? ins->file : g_current_filename;
            int err_line = ins->line;
            map_diag_location(&err_file, &err_line);
            fprintf(stderr, "%s at %s:%d\n", msg, err_file, err_line);
            vortex_error("");
        }
        uf->called = true;
        if (uf->is_unsafe && !ins->imm) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[VORTEX SAFETY ERROR] Calling unsafe fn '%s' requires an unsafe block or unsafe fn", uf->name);
            if (g_ultrasafe_level >= 3) {
                vortex_diag_add(VDIAG_ERROR, "E302",
                                ins->file ? ins->file : g_current_filename,
                                ins->line, ins->col > 0 ? ins->col : 1, msg, "Wrap the call in 'unsafe { ... }' or mark the caller 'unsafe fn'.");
                continue;
            }
            const char* err_file = ins->file ? ins->file : g_current_filename;
            int err_line = ins->line;
            map_diag_location(&err_file, &err_line);
            fprintf(stderr, "%s at %s:%d\n", msg, err_file, err_line);
            vortex_error("");
        }
    }

    if (g_ultrasafe_level >= 1) {
        vortex_ultrasafe_audit_ir(&main_ir, symtab, "__main__", TYPE_VOID, 1);
        vortex_ultrasafe_audit_program();
    }

    if (!g_lsp_capture_errors) {
        if (g_diag_count > 0 && (g_ultrasafe_level > 0 || g_json_errors)) {
            vortex_diag_print_all();
        } else if (g_check_only && g_json_errors) {
            vortex_diag_print_all();
        }
    }

    if (g_diag_error_count > 0) {
        free(symtab);
        ir_free(&main_ir);
        *out_jit = jit;
        return false;
    }

    if (g_check_only) {
        free(symtab);
        ir_free(&main_ir);
        *out_jit = jit;
        return true;
    }

    free(symtab);
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

    /* Validate & patch forward user function calls across the JIT code */
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
    volatile bool prev_capture = g_lsp_capture_errors;
    const char* volatile prev_file = g_current_filename;
    volatile int prev_ultrasafe = g_ultrasafe_level;
    volatile bool prev_check = g_check_only;
    volatile bool prev_json = g_json_errors;
    volatile VortexBuildMode prev_mode = g_compile_mode;
    jmp_buf saved_jmp;
    memcpy(&saved_jmp, &g_error_jmp, sizeof(saved_jmp));

    g_lsp_capture_errors = true;
    g_ultrasafe_level = 3;
    g_check_only = true;
    g_json_errors = false;
    g_compile_mode = VORTEX_MODE_SAFE;
    g_lsp_diag_len = 0;
    g_lsp_diag_buf[0] = '\0';
    g_current_filename = (file_path && file_path[0]) ? file_path : "<lsp>";
    vortex_diag_reset();

    char* volatile merged = vortex_resolve_imports_from_memory(source ? source : "", g_current_filename);
    const char* input_src = merged ? merged : (source ? source : "");

    volatile bool ok = false;
    if (setjmp(g_error_jmp) == 0) {
        uint8_t dummy_code[64];
        JITBuffer dummy_jit;
        memset(&dummy_jit, 0, sizeof(dummy_jit));
        dummy_jit.code = dummy_code;
        dummy_jit.capacity = sizeof(dummy_code);
        ok = compile_vortex_source_internal(input_src, dummy_code, &dummy_jit);
    } else {
        ir_free_all_active();
        if (diag_out && diag_cap > 0 && g_lsp_diag_buf[0]) {
            vx_str_copy(diag_out, diag_cap, g_lsp_diag_buf);
        }
        ok = false;
    }

    vortex_diag_sort();
    if (diag_out && diag_cap > 0 && !diag_out[0] && g_diag_count > 0) {
        vx_str_copy(diag_out, diag_cap, g_diags[0].message);
    }

    free(merged);
    vortex_source_map_reset();

    memcpy(&g_error_jmp, &saved_jmp, sizeof(saved_jmp));
    g_current_filename = prev_file;
    g_lsp_capture_errors = prev_capture;
    g_ultrasafe_level = prev_ultrasafe;
    g_check_only = prev_check;
    g_json_errors = prev_json;
    g_compile_mode = prev_mode;
    return ok && (g_diag_error_count == 0);
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

    if (g_check_only) {
        free_exec_page(exec_mem, MAX_JIT_CODE_SIZE);
        return 0;
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
    int cur_merged_line;
    bool tolerant_missing;
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
    if (len > 0) {
        memcpy(r->out_buf + r->out_len, s, len);
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '\n') r->cur_merged_line++;
        }
    }
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

static bool import_resolve_file_recursive(VortexImportResolver* r, const char* file_path, bool is_entry, bool is_stdlib);

static bool import_resolve_text_lines(VortexImportResolver* r, const char* src, size_t len,
                                      const char* file_path, bool is_entry, bool is_stdlib) {
    int file_id = source_map_intern_file(file_path);

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
                if (r->tolerant_missing) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Cannot open imported file '%s'", spec);
                    vortex_diag_add(VDIAG_ERROR, "E001", file_path, line_num, 1, msg,
                                    "Check the import path or install the required package.");
                    line_num++;
                    continue;
                }
                fprintf(stderr, "[VORTEX IMPORT ERROR] Cannot open imported file '%s' (at %s:%d)\n",
                        spec, file_path, line_num);
                return false;
            }
            char target_canon[1024];
            import_canonicalize_path(resolved, target_canon, sizeof(target_canon));
            if (!import_has_seen(r, target_canon)) {
                bool child_is_stdlib = is_stdlib || (strncmp(spec, "std/", 4) == 0) || (strncmp(spec, "std\\", 4) == 0);
                char marker[1280];
                snprintf(marker, sizeof(marker), "// --- begin import \"%s\" (%s:%d) ---\n",
                         spec, file_path, line_num);
                if (!import_append_cstr(r, marker) ||
                    !import_resolve_file_recursive(r, resolved, false, child_is_stdlib)) {
                    return false;
                }
                snprintf(marker, sizeof(marker), "\n// --- end import \"%s\" ---\n", spec);
                if (!import_append_cstr(r, marker)) {
                    return false;
                }
            }
        } else {
            source_map_record_line(r->cur_merged_line, line_num, file_id, is_stdlib, !is_entry);
            if (!import_append_str(r, src + line_start, line_len) ||
                (has_nl && !import_append_str(r, "\n", 1))) {
                return false;
            }
        }
        line_num++;
    }

    return true;
}

static bool import_resolve_file_recursive(VortexImportResolver* r, const char* file_path, bool is_entry, bool is_stdlib) {
    char canon[1024];
    import_canonicalize_path(file_path, canon, sizeof(canon));
    if (import_has_seen(r, canon)) {
        return true;
    }

    size_t len = 0;
    char* src = import_read_file_text(file_path, &len);
    if (!src) {
        if (r->tolerant_missing && !is_entry) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Cannot open imported file '%s'", file_path);
            vortex_diag_add(VDIAG_ERROR, "E001", file_path, 1, 1, msg, NULL);
            return true;
        }
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

    bool ok = import_resolve_text_lines(r, src, len, file_path, is_entry, is_stdlib);
    free(src);
    return ok;
}

char* vortex_resolve_imports(const char* entry_path) {
    if (!entry_path || !entry_path[0]) {
        fprintf(stderr, "[VORTEX ERROR] Cannot open file '%s'\n", entry_path ? entry_path : "");
        return NULL;
    }

    vortex_source_map_reset();

    VortexImportResolver r;
    memset(&r, 0, sizeof(r));
    r.cur_merged_line = 1;
    if (!import_append_cstr(&r, "")) {
        free(r.out_buf);
        return NULL;
    }

    bool ok = import_resolve_file_recursive(&r, entry_path, true, false);
    for (int i = 0; i < r.count; i++) {
        free(r.paths[i]);
    }
    free(r.paths);

    if (!ok) {
        free(r.out_buf);
        vortex_source_map_reset();
        return NULL;
    }
    g_source_map.source_ptr = r.out_buf;
    return r.out_buf;
}

char* vortex_resolve_imports_from_memory(const char* source, const char* virtual_path) {
    const char* vpath = (virtual_path && virtual_path[0]) ? virtual_path : "<lsp>";
    const char* text = source ? source : "";

    vortex_source_map_reset();

    VortexImportResolver r;
    memset(&r, 0, sizeof(r));
    r.cur_merged_line = 1;
    r.tolerant_missing = true;
    if (!import_append_cstr(&r, "")) {
        free(r.out_buf);
        return NULL;
    }

    char canon[1024];
    import_canonicalize_path(vpath, canon, sizeof(canon));
    import_mark_seen(&r, canon);

    bool ok = import_resolve_text_lines(&r, text, strlen(text), vpath, true, false);
    for (int i = 0; i < r.count; i++) {
        free(r.paths[i]);
    }
    free(r.paths);

    if (!ok) {
        free(r.out_buf);
        vortex_source_map_reset();
        return NULL;
    }
    g_source_map.source_ptr = r.out_buf;
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

/* ========================================================================= */
/* 8. NATIVE HOST / JVM FFM & JNI EMBEDDING API                              */
/* ========================================================================= */

VORTEX_API void vortex_host_shutdown(void) {
    if (s_host_exec_mem) {
        debug_info_unregister_jit_image();
        free_exec_page(s_host_exec_mem, MAX_JIT_CODE_SIZE);
        s_host_exec_mem = NULL;
    }
    g_host_mode = false;
}

VORTEX_API int vortex_host_init(int release_mode) {
    g_pkg_import_hook = pkg_resolve_import;
    g_headless_mode = true;
    g_host_mode = true;
    g_compile_mode = release_mode ? VORTEX_MODE_RELEASE : VORTEX_MODE_SAFE;
    g_host_last_err[0] = '\0';
    arena_init();
    vortex_thread_pool_init();

    char* emb_src = NULL;
    uint32_t emb_flags = 0;
    if (vortex_load_embedded_module_payload(g_vortex_dll_handle, &emb_src, &emb_flags)) {
        g_compile_mode = (emb_flags & VORTEX_PACK_FLAG_RELEASE) ? VORTEX_MODE_RELEASE : g_compile_mode;
        int rc = vortex_host_compile(emb_src);
        free(emb_src);
        return rc;
    }
    return 0;
}

VORTEX_API int vortex_host_compile(const char* source) {
    if (!source) {
        vx_str_copy(g_host_last_err, sizeof(g_host_last_err), "Null source passed to vortex_host_compile");
        return 1;
    }
    g_host_mode = true;
    g_headless_mode = true;
    g_host_last_err[0] = '\0';

    if (s_host_exec_mem) {
        debug_info_unregister_jit_image();
        free_exec_page(s_host_exec_mem, MAX_JIT_CODE_SIZE);
        s_host_exec_mem = NULL;
    }

    void* exec_mem = alloc_exec_page(MAX_JIT_CODE_SIZE);
    if (!exec_mem) {
        vx_str_copy(g_host_last_err, sizeof(g_host_last_err), "Failed to allocate executable JIT page");
        return 1;
    }

    jmp_buf saved_jmp;
    memcpy(&saved_jmp, &g_error_jmp, sizeof(saved_jmp));
    JITBuffer jit;
    memset(&jit, 0, sizeof(jit));

    if (setjmp(g_error_jmp) != 0) {
        ir_free_all_active();
        if (g_host_last_err[0] == '\0') {
            vx_str_copy(g_host_last_err, sizeof(g_host_last_err), "Vortex compilation or initialization error");
        }
        free_exec_page(exec_mem, MAX_JIT_CODE_SIZE);
        memcpy(&g_error_jmp, &saved_jmp, sizeof(saved_jmp));
        return 1;
    }

    if (!compile_vortex_source_internal(source, exec_mem, &jit)) {
        free_exec_page(exec_mem, MAX_JIT_CODE_SIZE);
        memcpy(&g_error_jmp, &saved_jmp, sizeof(saved_jmp));
        return 1;
    }

    debug_info_set_jit_image(exec_mem, jit.size, &jit);
    jit_write_protect(exec_mem, jit.size, false);

    s_host_exec_mem = exec_mem;

    typedef void (*vortex_entry_fn)(void);
    vortex_entry_fn entry = (vortex_entry_fn)exec_mem;
    vortex_crash_guard_enter();
    entry();
    vortex_crash_guard_leave();

    memcpy(&g_error_jmp, &saved_jmp, sizeof(saved_jmp));
    return 0;
}

VORTEX_API int vortex_host_compile_file(const char* filepath) {
    if (!filepath || !filepath[0]) {
        vx_str_copy(g_host_last_err, sizeof(g_host_last_err), "Empty filepath passed to vortex_host_compile_file");
        return 1;
    }
    char* buffer = vortex_resolve_imports(filepath);
    if (!buffer) {
        vx_str_copy(g_host_last_err, sizeof(g_host_last_err), "Failed to read or resolve imports for file");
        return 1;
    }
    const char* prev_file = g_current_filename;
    g_current_filename = vortex_intern_cstr(filepath);
    int rc = vortex_host_compile(buffer);
    g_current_filename = prev_file;
    free(buffer);
    return rc;
}

VORTEX_API void* vortex_host_get_fn(const char* fn_name) {
    if (!s_host_exec_mem || !fn_name || !fn_name[0]) return NULL;
    UserFunc* uf = user_func_find(fn_name);
    if (!uf) return NULL;
    return (void*)((uint8_t*)s_host_exec_mem + uf->code_offset);
}

VORTEX_API int64_t vortex_host_call0(void* fn_ptr) {
    if (!fn_ptr) return 0;
    typedef int64_t (*fn0_t)(void);
    return ((fn0_t)fn_ptr)();
}

VORTEX_API int64_t vortex_host_call1(void* fn_ptr, int64_t a1) {
    if (!fn_ptr) return 0;
    typedef int64_t (*fn1_t)(int64_t);
    return ((fn1_t)fn_ptr)(a1);
}

VORTEX_API int64_t vortex_host_call2(void* fn_ptr, int64_t a1, int64_t a2) {
    if (!fn_ptr) return 0;
    typedef int64_t (*fn2_t)(int64_t, int64_t);
    return ((fn2_t)fn_ptr)(a1, a2);
}

VORTEX_API int64_t vortex_host_call3(void* fn_ptr, int64_t a1, int64_t a2, int64_t a3) {
    if (!fn_ptr) return 0;
    typedef int64_t (*fn3_t)(int64_t, int64_t, int64_t);
    return ((fn3_t)fn_ptr)(a1, a2, a3);
}

VORTEX_API int64_t vortex_host_call4(void* fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    if (!fn_ptr) return 0;
    typedef int64_t (*fn4_t)(int64_t, int64_t, int64_t, int64_t);
    return ((fn4_t)fn_ptr)(a1, a2, a3, a4);
}

VORTEX_API int64_t vortex_host_scope_push(void) {
    return vortex_scope_push();
}

VORTEX_API int64_t vortex_host_scope_pop(int64_t mark) {
    return vortex_scope_pop(mark);
}

VORTEX_API void* vortex_host_arena_alloc(int64_t bytes) {
    if (bytes <= 0) return NULL;
    return arena_alloc_compact((size_t)bytes);
}

VORTEX_API void vortex_host_arena_reset(void) {
    arena_init();
}

VORTEX_API const char* vortex_host_last_error(void) {
    return g_host_last_err;
}

/* ========================================================================= */
/* NATIVE TEST RUNNER (`vortex test`) & SELF-DESCRIBING SPEC (`vortex spec`) */
/* ========================================================================= */

static bool str_ends_with_vx(const char* name) {
    if (!name) return false;
    size_t n = strlen(name);
    return n > 3 && strcmp(name + n - 3, ".vx") == 0;
}

static bool test_name_matches(const char* name) {
    if (!name || !str_ends_with_vx(name)) return false;
    return (strncmp(name, "test_", 5) == 0) || (strstr(name, "_test.vx") != NULL);
}

static int run_single_test_file(const char* filepath, bool quiet, int* out_total_fn, int* out_pass_fn, int* out_fail_fn) {
    if (!quiet) {
        printf("[VORTEX TEST] Running %s\n", filepath);
    }
    int rc = vortex_host_compile_file(filepath);
    if (rc != 0) {
        (*out_total_fn)++;
        (*out_fail_fn)++;
        fprintf(stderr, "  [FAIL] %s: %s\n", filepath, g_host_last_err[0] ? g_host_last_err : "compile/runtime failure");
        vortex_host_shutdown();
        return 1;
    }

    int file_tests = 0;
    int file_fails = 0;
    for (int i = 0; i < g_user_func_count; i++) {
        if (strncmp(g_user_funcs[i].name, "test_", 5) == 0 && g_user_funcs[i].param_count == 0) {
            file_tests++;
            (*out_total_fn)++;
            void* fn_ptr = vortex_host_get_fn(g_user_funcs[i].name);
            jmp_buf saved_jmp;
            memcpy(&saved_jmp, &g_error_jmp, sizeof(saved_jmp));
            if (setjmp(g_error_jmp) == 0) {
                vortex_host_call0(fn_ptr);
                (*out_pass_fn)++;
                if (!quiet) printf("  [PASS] %s\n", g_user_funcs[i].name);
            } else {
                vortex_crash_guard_leave();
                file_fails++;
                (*out_fail_fn)++;
                fprintf(stderr, "  [FAIL] %s: %s\n", g_user_funcs[i].name, g_host_last_err[0] ? g_host_last_err : "assertion failed");
            }
            memcpy(&g_error_jmp, &saved_jmp, sizeof(saved_jmp));
        }
    }

    if (file_tests == 0) {
        (*out_total_fn)++;
        (*out_pass_fn)++;
        if (!quiet) printf("  [PASS] %s (top-level)\n", filepath);
    }

    vortex_host_shutdown();
    return file_fails > 0 ? 1 : 0;
}

int vortex_cmd_test(int argc, char** argv) {
    const char* target = "tests";
    bool quiet = g_json_errors;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) g_compile_mode = VORTEX_MODE_RELEASE;
        else if (strcmp(argv[i], "--safe") == 0) g_compile_mode = VORTEX_MODE_SAFE;
        else if (strcmp(argv[i], "--json") == 0) { g_json_errors = true; quiet = true; }
        else if (strncmp(argv[i], "--ultrasafe=", 12) == 0) { g_compile_mode = VORTEX_MODE_SAFE; g_ultrasafe_level = atoi(argv[i] + 12); }
        else if (strcmp(argv[i], "--ultrasafe") == 0 && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
            g_compile_mode = VORTEX_MODE_SAFE;
            g_ultrasafe_level = atoi(argv[++i]);
        }
        else if (argv[i][0] != '-') target = argv[i];
    }

    char files[256][512];
    int file_count = 0;

    if (str_ends_with_vx(target)) {
        vx_str_copy(files[file_count++], 512, target);
    } else {
#ifdef _WIN32
        char pattern[512];
        snprintf(pattern, sizeof(pattern), "%s\\*.vx", target);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && test_name_matches(fd.cFileName)) {
                    if (file_count < 256) {
                        snprintf(files[file_count++], 512, "%s/%s", target, fd.cFileName);
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
#else
        DIR* dir = opendir(target);
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != NULL) {
                if (test_name_matches(ent->d_name) && file_count < 256) {
                    snprintf(files[file_count++], 512, "%s/%s", target, ent->d_name);
                }
            }
            closedir(dir);
        }
#endif
    }

    if (file_count == 0) {
        fprintf(stderr, "[VORTEX TEST] No test_*.vx files found in '%s'\n", target);
        return 1;
    }

    int total = 0, passed = 0, failed = 0;
    for (int i = 0; i < file_count; i++) {
        run_single_test_file(files[i], quiet, &total, &passed, &failed);
    }
    g_host_mode = false;
    g_headless_mode = false;

    if (g_json_errors) {
        printf("{\"version\":\"%s\",\"files\":%d,\"total\":%d,\"passed\":%d,\"failed\":%d,\"ok\":%s}\n",
               VORTEX_VERSION, file_count, total, passed, failed, failed == 0 ? "true" : "false");
    } else {
        printf("\n[VORTEX TEST] Summary: %d/%d passed (%d failed) across %d file(s)\n",
               passed, total, failed, file_count);
    }
    return failed == 0 ? 0 : 1;
}

int vortex_cmd_spec(int argc, char** argv) {
    bool as_json = g_json_errors;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) as_json = true;
    }

    static const char* k_keywords[] = {
        "let", "mut", "fn", "unsafe", "return", "if", "else", "while", "for",
        "in", "parallel_for", "struct", "enum", "match", "import", "extern",
        "as", "break", "continue", "true", "false"
    };
    static const char* k_types[] = {
        "i64", "i32", "i16", "i8", "u64", "u32", "u16", "u8", "usize", "isize",
        "bool", "char", "f32", "f64", "str", "ptr", "void", "[T]", "<Struct>", "<Enum>"
    };
    static const char* k_ops[] = {
        "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=",
        "&&", "||", "!", "&", "|", "^", "~", "<<", ">>",
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "::", "?:", "=>"
    };

    if (as_json) {
        printf("{\n  \"language\": \"Vortex\",\n  \"version\": \"%s\",\n", VORTEX_VERSION);
        printf("  \"limits\": {\"max_struct_fields\": %d, \"max_struct_types\": %d, \"max_enum_types\": %d, \"max_enum_variants\": %d, \"max_user_funcs\": %d, \"max_string_bytes\": 4095},\n",
               MAX_FIELDS, MAX_STRUCT_TYPES, MAX_ENUM_TYPES, MAX_ENUM_VARIANTS, MAX_USER_FUNCS);
        printf("  \"safety_modes\": [\"--safe\", \"--release\", \"--ultrasafe 1\", \"--ultrasafe 2\", \"--ultrasafe 3\"],\n");
        printf("  \"keywords\": [");
        for (size_t i = 0; i < sizeof(k_keywords) / sizeof(k_keywords[0]); i++) {
            printf("\"%s\"%s", k_keywords[i], (i + 1 < sizeof(k_keywords) / sizeof(k_keywords[0])) ? ", " : "");
        }
        printf("],\n  \"types\": [");
        for (size_t i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
            printf("\"%s\"%s", k_types[i], (i + 1 < sizeof(k_types) / sizeof(k_types[0])) ? ", " : "");
        }
        printf("],\n  \"operators\": [");
        for (size_t i = 0; i < sizeof(k_ops) / sizeof(k_ops[0]); i++) {
            json_print_escaped(stdout, k_ops[i]);
            if (i + 1 < sizeof(k_ops) / sizeof(k_ops[0])) printf(", ");
        }
        printf("],\n  \"builtins\": [\n");
        for (int i = 0; g_builtins[i].name != NULL; i++) {
            printf("    {\"name\": \"%s\", \"arity\": %d}%s\n",
                   g_builtins[i].name, g_builtins[i].arg_count,
                   g_builtins[i + 1].name ? "," : "");
        }
        printf("  ]\n}\n");
        return 0;
    }

    printf("# Vortex %s Compiler Specification (Auto-Generated)\n\n", VORTEX_VERSION);
    printf("## Safety & Diagnostic Flags\n");
    printf("- `--safe` (default): bounds checks, null-ptr checks, FFI checks + constant folding, copy prop, LICM, DCE\n");
    printf("- `--release`: removes runtime bounds/null checks and enables aggressive vectorization & unrolling\n");
    printf("- `--ultrasafe 1`: runtime div/mod-by-zero checks + unreachable code & unused-mut warnings\n");
    printf("- `--ultrasafe 2`: level 1 + static div/zero, static OOB, static null deref, missing-return, unused locals/fns, recursion depth guard\n");
    printf("- `--ultrasafe 3`: level 2 + multi-error recovery reporting all parse/region/safety errors in one pass\n");
    printf("- `--json` / `--error-format=json`: structured JSON diagnostics\n");
    printf("- `--check`: validate without executing JIT code\n");
    printf("- `vortex test [path] [--json]`: run native `test_*.vx` assertions\n\n");
    printf("## Limits\n");
    printf("- Max struct fields: %d | Max user functions: %d | Max literal string: 4095 bytes\n\n",
           MAX_FIELDS, MAX_USER_FUNCS);
    printf("## Keywords\n");
    for (size_t i = 0; i < sizeof(k_keywords) / sizeof(k_keywords[0]); i++) {
        printf("`%s`%s", k_keywords[i], (i + 1 < sizeof(k_keywords) / sizeof(k_keywords[0])) ? " " : "\n\n");
    }
    printf("## Types & Operators\n");
    for (size_t i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
        printf("`%s`%s", k_types[i], (i + 1 < sizeof(k_types) / sizeof(k_types[0])) ? " " : "\n");
    }
    for (size_t i = 0; i < sizeof(k_ops) / sizeof(k_ops[0]); i++) {
        printf("`%s`%s", k_ops[i], (i + 1 < sizeof(k_ops) / sizeof(k_ops[0])) ? " " : "\n\n");
    }
    printf("## Builtin Functions (%s)\n", "live from g_builtins");
    for (int i = 0; g_builtins[i].name != NULL; i++) {
        printf("- `%s` (arity: %d)\n", g_builtins[i].name, g_builtins[i].arg_count);
    }
    return 0;
}


