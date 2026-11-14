/*
 * VORTEX DYNAMIC FFI LOADER & TRAMPOLINES (LoadLibraryA / dlopen)
 */

#include "vortex.h"

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

static void vortex_ffi_require_fn(int64_t fn_ptr, const char* call_name) {
    if (!fn_ptr) {
        fprintf(stderr, "[VORTEX FFI ERROR] %s called with NULL function pointer\n", call_name);
        vortex_error("");
    }
}

int64_t vortex_ffi_call0(int64_t fn_ptr) {
    vortex_ffi_require_fn(fn_ptr, "ffi_call0");
    typedef int64_t (*fn0_t)(void);
    return ((fn0_t)fn_ptr)();
}

int64_t vortex_ffi_call1(int64_t fn_ptr, int64_t a1) {
    vortex_ffi_require_fn(fn_ptr, "ffi_call1");
    typedef int64_t (*fn1_t)(int64_t);
    return ((fn1_t)fn_ptr)(a1);
}

int64_t vortex_ffi_call2(int64_t fn_ptr, int64_t a1, int64_t a2) {
    vortex_ffi_require_fn(fn_ptr, "ffi_call2");
    typedef int64_t (*fn2_t)(int64_t, int64_t);
    return ((fn2_t)fn_ptr)(a1, a2);
}

int64_t vortex_ffi_call3(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3) {
    vortex_ffi_require_fn(fn_ptr, "ffi_call3");
    typedef int64_t (*fn3_t)(int64_t, int64_t, int64_t);
    return ((fn3_t)fn_ptr)(a1, a2, a3);
}

int64_t vortex_ffi_call4(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    vortex_ffi_require_fn(fn_ptr, "ffi_call4");
    typedef int64_t (*fn4_t)(int64_t, int64_t, int64_t, int64_t);
    return ((fn4_t)fn_ptr)(a1, a2, a3, a4);
}

int64_t vortex_ffi_call5(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5) {
    vortex_ffi_require_fn(fn_ptr, "ffi_call5");
    typedef int64_t (*fn5_t)(int64_t, int64_t, int64_t, int64_t, int64_t);
    return ((fn5_t)fn_ptr)(a1, a2, a3, a4, a5);
}

int64_t vortex_ffi_call6(int64_t fn_ptr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6) {
    vortex_ffi_require_fn(fn_ptr, "ffi_call6");
    typedef int64_t (*fn6_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
    return ((fn6_t)fn_ptr)(a1, a2, a3, a4, a5, a6);
}
