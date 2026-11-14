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
static bool g_is_packaged_app = false;

bool vortex_is_cross_target(void) {
    if (g_target_arch != VORTEX_HOST_ARCH) return true;
    if (g_target_arch == VORTEX_ARCH_X86_64 && g_target_abi != VORTEX_HOST_ABI) return true;
    if (g_target_arch == VORTEX_ARCH_ARM64 && g_target_abi != VORTEX_HOST_ABI) return true;
    return false;
}

#define MAX_INTERN_CSTRS 1024
static const char* g_intern_cstrs[MAX_INTERN_CSTRS];
static int g_intern_cstr_count = 0;

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
        if (argv0) {
            strncpy(out, argv0, cap - 1);
            out[cap - 1] = '\0';
        }
    }
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) != 0) {
        if (argv0) {
            strncpy(out, argv0, cap - 1);
            out[cap - 1] = '\0';
        }
    }
#else
    ssize_t rlen = readlink("/proc/self/exe", out, cap - 1);
    if (rlen > 0) {
        out[rlen] = '\0';
    } else if (argv0) {
        strncpy(out, argv0, cap - 1);
        out[cap - 1] = '\0';
    }
#endif
}

static void vortex_wait_on_standalone_error(const char* msg) {
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

/* --- Native GUI & Software Framebuffer Subsystem --- */
/* ========================================================================= */

#ifdef _WIN32
typedef struct {
    HWND hwnd;
    HDC hdc;
    HDC mem_dc;
    HBITMAP bitmap;
    uint32_t* pixels;
    int width;
    int height;
    int mouse_x;
    int mouse_y;
    bool mouse_down;
    bool mouse_clicked;
    bool is_open;
    HFONT font;
    char title[128];
} VortexWindow;

static LRESULT CALLBACK VortexWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    VortexWindow* w = (VortexWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            if (w) w->is_open = false;
            return 0;
        case WM_MOUSEMOVE:
            if (w) {
                w->mouse_x = GET_X_LPARAM(lParam);
                w->mouse_y = GET_Y_LPARAM(lParam);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (w) {
                w->mouse_down = true;
                w->mouse_clicked = true;
            }
            return 0;
        case WM_LBUTTONUP:
            if (w) w->mouse_down = false;
            return 0;
        case WM_PAINT:
            if (w) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                BitBlt(hdc, 0, 0, w->width, w->height, w->mem_dc, 0, 0, SRCCOPY);
                EndPaint(hwnd, &ps);
            }
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

static bool g_win_class_registered = false;

int64_t vortex_gui_window(int64_t title_ptr, int64_t width, int64_t height) {
    const char* title = (const char*)title_ptr;
    if (g_headless_mode || getenv("VORTEX_HEADLESS") != NULL) {
        VortexWindow* w = (VortexWindow*)calloc(1, sizeof(VortexWindow));
        if (!w) return 0;
        w->width = (int)width;
        w->height = (int)height;
        w->is_open = false;
        if (title) strncpy(w->title, title, sizeof(w->title) - 1);
        return (int64_t)(uintptr_t)w;
    }
    HINSTANCE hInst = GetModuleHandle(NULL);
    if (!g_win_class_registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = VortexWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = "VortexWindowClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        if (!RegisterClassA(&wc)) {
            fprintf(stderr, "[VORTEX GUI ERROR] RegisterClassA failed (Win32 error %lu)\n", (unsigned long)GetLastError());
            vortex_error("");
        }
        g_win_class_registered = true;
    }

    VortexWindow* w = (VortexWindow*)calloc(1, sizeof(VortexWindow));
    if (!w) {
        fprintf(stderr, "[VORTEX GUI ERROR] Out of memory allocating VortexWindow\n");
        vortex_error("");
    }
    w->width = (int)width;
    w->height = (int)height;
    w->is_open = true;
    if (title) strncpy(w->title, title, sizeof(w->title) - 1);

    RECT rc = {0, 0, w->width, w->height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    w->hwnd = CreateWindowExA(
        0, "VortexWindowClass", title ? title : "Vortex",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL
    );
    if (!w->hwnd) {
        fprintf(stderr, "[VORTEX GUI ERROR] CreateWindowExA failed (Win32 error %lu)\n", (unsigned long)GetLastError());
        free(w);
        vortex_error("");
    }

    SetWindowLongPtr(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);

    w->hdc = GetDC(w->hwnd);
    w->mem_dc = CreateCompatibleDC(w->hdc);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w->width;
    bmi.bmiHeader.biHeight = -w->height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    w->bitmap = CreateDIBSection(w->mem_dc, &bmi, DIB_RGB_COLORS, (void**)&w->pixels, NULL, 0);
    if (!w->hdc || !w->mem_dc || !w->bitmap || !w->pixels) {
        fprintf(stderr, "[VORTEX GUI ERROR] Failed to create GDI framebuffer (Win32 error %lu)\n", (unsigned long)GetLastError());
        if (w->bitmap) DeleteObject(w->bitmap);
        if (w->mem_dc) DeleteDC(w->mem_dc);
        if (w->hdc) ReleaseDC(w->hwnd, w->hdc);
        DestroyWindow(w->hwnd);
        free(w);
        vortex_error("");
    }
    SelectObject(w->mem_dc, w->bitmap);

    w->font = CreateFontA(17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SelectObject(w->mem_dc, w->font);
    SetBkMode(w->mem_dc, TRANSPARENT);

    ShowWindow(w->hwnd, SW_SHOW);
    UpdateWindow(w->hwnd);

    return (int64_t)w;
}

int64_t vortex_gui_set_pos(int64_t win_ptr, int64_t x, int64_t y) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (w && w->hwnd) {
        SetWindowPos(w->hwnd, NULL, (int)x, (int)y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    return 0;
}

int64_t vortex_gui_is_open(int64_t win_ptr) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (!w || !w->is_open) return 0;

    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            w->is_open = false;
            return 0;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return w->is_open ? 1 : 0;
}

int64_t vortex_gui_draw_text(int64_t win_ptr, int64_t text_ptr, int64_t x, int64_t y, int64_t color_rgb) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    const char* text = (const char*)text_ptr;
    if (!w || !text) return 0;

    COLORREF cr = RGB((color_rgb >> 16) & 0xFF, (color_rgb >> 8) & 0xFF, color_rgb & 0xFF);
    SetTextColor(w->mem_dc, cr);
    TextOutA(w->mem_dc, (int)x, (int)y, text, (int)strlen(text));
    return 0;
}

int64_t vortex_gui_present(int64_t win_ptr) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (!w) return 0;
    BitBlt(w->hdc, 0, 0, w->width, w->height, w->mem_dc, 0, 0, SRCCOPY);
    w->mouse_clicked = false;
    return 0;
}
#else
/* --- LINUX X11 NATIVE GUI IMPLEMENTATION --- */
#ifndef VORTEX_NO_X11
typedef struct {
    Display* dpy;
    Window win;
    GC gc;
    XImage* ximage;
    Atom wm_delete_window;
    uint32_t* pixels;
    int width;
    int height;
    int mouse_x;
    int mouse_y;
    bool mouse_down;
    bool mouse_clicked;
    bool is_open;
    char title[128];
} VortexWindow;

int64_t vortex_gui_window(int64_t title_ptr, int64_t width, int64_t height) {
    const char* title = (const char*)title_ptr;
    if (g_headless_mode || getenv("VORTEX_HEADLESS") != NULL) {
        VortexWindow* w = (VortexWindow*)calloc(1, sizeof(VortexWindow));
        if (!w) return 0;
        w->width = (int)width;
        w->height = (int)height;
        w->is_open = false;
        if (title) strncpy(w->title, title, sizeof(w->title) - 1);
        return (int64_t)(uintptr_t)w;
    }
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[VORTEX GUI ERROR] Cannot open X11 display (check DISPLAY environment variable)\n");
        return 0;
    }
    int screen = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(
        dpy, RootWindow(dpy, screen),
        100, 100, (unsigned int)width, (unsigned int)height,
        1, BlackPixel(dpy, screen), WhitePixel(dpy, screen)
    );

    XStoreName(dpy, win, title ? title : "Vortex");

    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    XSelectInput(dpy, win,
        ExposureMask | KeyPressMask | KeyReleaseMask |
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);

    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    VortexWindow* w = (VortexWindow*)calloc(1, sizeof(VortexWindow));
    if (!w) {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 0;
    }
    w->dpy = dpy;
    w->win = win;
    w->gc = gc;
    w->wm_delete_window = wm_del;
    w->width = (int)width;
    w->height = (int)height;
    w->is_open = true;
    if (title) strncpy(w->title, title, sizeof(w->title) - 1);

    w->pixels = (uint32_t*)malloc((size_t)width * (size_t)height * sizeof(uint32_t));
    if (!w->pixels) {
        free(w);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 0;
    }
    memset(w->pixels, 0, (size_t)width * (size_t)height * sizeof(uint32_t));

    w->ximage = XCreateImage(
        dpy, DefaultVisual(dpy, screen), 24, ZPixmap, 0,
        (char*)w->pixels, (unsigned int)width, (unsigned int)height, 32, 0
    );

    XFlush(dpy);
    return (int64_t)w;
}

int64_t vortex_gui_set_pos(int64_t win_ptr, int64_t x, int64_t y) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (w && w->dpy && w->win) {
        XMoveWindow(w->dpy, w->win, (int)x, (int)y);
        XFlush(w->dpy);
    }
    return 0;
}

int64_t vortex_gui_is_open(int64_t win_ptr) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (!w || !w->is_open || !w->dpy) return 0;

    while (XPending(w->dpy)) {
        XEvent ev;
        XNextEvent(w->dpy, &ev);
        if (ev.type == ClientMessage) {
            if ((Atom)ev.xclient.data.l[0] == w->wm_delete_window) {
                w->is_open = false;
                return 0;
            }
        } else if (ev.type == DestroyNotify) {
            w->is_open = false;
            return 0;
        } else if (ev.type == MotionNotify) {
            w->mouse_x = ev.xmotion.x;
            w->mouse_y = ev.xmotion.y;
        } else if (ev.type == ButtonPress) {
            if (ev.xbutton.button == 1) { // Left mouse button
                w->mouse_down = true;
                w->mouse_clicked = true;
            }
        } else if (ev.type == ButtonRelease) {
            if (ev.xbutton.button == 1) {
                w->mouse_down = false;
            }
        }
    }
    return w->is_open ? 1 : 0;
}

int64_t vortex_gui_draw_text(int64_t win_ptr, int64_t text_ptr, int64_t x, int64_t y, int64_t color_rgb) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    const char* text = (const char*)text_ptr;
    if (!w || !text || !w->dpy || !w->win) return 0;
    XSetForeground(w->dpy, w->gc, (unsigned long)(color_rgb & 0xFFFFFF));
    XDrawString(w->dpy, w->win, w->gc, (int)x, (int)y + 12, text, (int)strlen(text));
    return 0;
}

int64_t vortex_gui_present(int64_t win_ptr) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (!w || !w->dpy || !w->win || !w->ximage) return 0;
    XPutImage(w->dpy, w->win, w->gc, w->ximage, 0, 0, 0, 0, (unsigned int)w->width, (unsigned int)w->height);
    XFlush(w->dpy);
    w->mouse_clicked = false;
    return 0;
}
#else
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    int mouse_x;
    int mouse_y;
    bool mouse_down;
    bool mouse_clicked;
    bool is_open;
    char title[128];
} VortexWindow;

int64_t vortex_gui_window(int64_t t, int64_t w_px, int64_t h_px) {
    const char* title = (const char*)t;
    VortexWindow* w = (VortexWindow*)calloc(1, sizeof(VortexWindow));
    if (!w) return 0;
    w->width = (int)w_px;
    w->height = (int)h_px;
    w->is_open = false;
    if (title) strncpy(w->title, title, sizeof(w->title) - 1);
    return (int64_t)(uintptr_t)w;
}
int64_t vortex_gui_set_pos(int64_t win, int64_t x, int64_t y) { (void)win;(void)x;(void)y; return 0; }
int64_t vortex_gui_is_open(int64_t win) { (void)win; return 0; }
int64_t vortex_gui_draw_text(int64_t win, int64_t t, int64_t x, int64_t y, int64_t c) { (void)win;(void)t;(void)x;(void)y;(void)c; return 0; }
int64_t vortex_gui_present(int64_t win) { (void)win; return 0; }
#endif
#endif

/* --- PLATFORM-INDEPENDENT SOFTWARE FRAMEBUFFER DRAWING PRIMITIVES --- */

int64_t vortex_gui_rgb(int64_t r, int64_t g, int64_t b) {
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

int64_t vortex_gui_clear(int64_t win_ptr, int64_t color_rgb) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (!w || !w->pixels) return 0;
    uint32_t color = (uint32_t)color_rgb;
    int total = w->width * w->height;
    for (int i = 0; i < total; i++) {
        w->pixels[i] = color;
    }
    return 0;
}

int64_t vortex_gui_draw_rect(int64_t win_ptr, int64_t x, int64_t y, int64_t width, int64_t height, int64_t color_rgb) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (!w || !w->pixels) return 0;

    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + (int)width;
    int y1 = y0 + (int)height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w->width) x1 = w->width;
    if (y1 > w->height) y1 = w->height;

    uint32_t color = (uint32_t)color_rgb;
    for (int cy = y0; cy < y1; cy++) {
        uint32_t* row = &w->pixels[cy * w->width];
        for (int cx = x0; cx < x1; cx++) {
            row[cx] = color;
        }
    }
    return 0;
}

int64_t vortex_gui_draw_pixel(int64_t win_ptr, int64_t x, int64_t y, int64_t color_rgb) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (!w || !w->pixels) return 0;
    int cx = (int)x;
    int cy = (int)y;
    if (cx >= 0 && cx < w->width && cy >= 0 && cy < w->height) {
        w->pixels[cy * w->width + cx] = (uint32_t)color_rgb;
    }
    return 0;
}

int64_t vortex_gui_draw_line(int64_t win_ptr, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t color_rgb) {
    VortexWindow* w = (VortexWindow*)win_ptr;
    if (!w || !w->pixels) return 0;

    int cx0 = (int)x0, cy0 = (int)y0;
    int cx1 = (int)x1, cy1 = (int)y1;

    // Quick boundary rejection if completely outside
    if ((cx0 < 0 && cx1 < 0) || (cx0 >= w->width && cx1 >= w->width) ||
        (cy0 < 0 && cy1 < 0) || (cy0 >= w->height && cy1 >= w->height)) {
        return 0;
    }

    int dx = abs(cx1 - cx0);
    int sx = (cx0 < cx1) ? 1 : -1;
    int dy = -abs(cy1 - cy0);
    int sy = (cy0 < cy1) ? 1 : -1;
    int err = dx + dy;
    uint32_t col = (uint32_t)color_rgb;
    int W = w->width;
    int H = w->height;

    while (1) {
        if (cx0 >= 0 && cx0 < W && cy0 >= 0 && cy0 < H) {
            w->pixels[cy0 * W + cx0] = col;
        }
        if (cx0 == cx1 && cy0 == cy1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; cx0 += sx; }
        if (e2 <= dx) { err += dx; cy0 += sy; }
    }
    return 0;
}

int64_t vortex_gui_draw_number(int64_t win_ptr, int64_t num, int64_t x, int64_t y, int64_t color_rgb) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)num);
    return vortex_gui_draw_text(win_ptr, (int64_t)buf, x, y, color_rgb);
}

int64_t vortex_gui_button(int64_t win_ptr, int64_t id, int64_t label_ptr,
                                 int64_t x, int64_t y, int64_t w_btn, int64_t h_btn) {
    (void)id;
    VortexWindow* w = (VortexWindow*)win_ptr;
    const char* label = (const char*)label_ptr;
    if (!w) return 0;

    bool hovered = (w->mouse_x >= x && w->mouse_x < x + w_btn &&
                    w->mouse_y >= y && w->mouse_y < y + h_btn);
    bool clicked = hovered && w->mouse_clicked;

    // Button colors (Dark Modern Theme)
    int64_t bg_color = vortex_gui_rgb(45, 95, 190);
    if (hovered && w->mouse_down) {
        bg_color = vortex_gui_rgb(25, 70, 150);
    } else if (hovered) {
        bg_color = vortex_gui_rgb(60, 120, 230);
    }

    vortex_gui_draw_rect(win_ptr, x, y, w_btn, h_btn, bg_color);
    vortex_gui_draw_rect(win_ptr, x, y, w_btn, 1, vortex_gui_rgb(100, 160, 255));
    vortex_gui_draw_rect(win_ptr, x, y + h_btn - 1, w_btn, 1, vortex_gui_rgb(20, 50, 110));

    if (label) {
        int text_len = (int)strlen(label);
        int tx = 0, ty = 0;
#ifdef _WIN32
        SIZE sz;
        GetTextExtentPoint32A(w->mem_dc, label, text_len, &sz);
        tx = (int)x + (int)(w_btn - sz.cx) / 2;
        ty = (int)y + (int)(h_btn - sz.cy) / 2;
#else
        int text_px = text_len * 7;
        tx = (int)x + (int)(w_btn - text_px) / 2;
        ty = (int)y + (int)(h_btn - 12) / 2;
#endif
        vortex_gui_draw_text(win_ptr, label_ptr, tx, ty, vortex_gui_rgb(255, 255, 255));
    }

    return clicked ? 1 : 0;
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

    int64_t cur = start;
    for (int i = 0; i < count; i++) {
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

#ifdef _WIN32
    WaitForMultipleObjects(count, g_thread_pool.done_handles, TRUE, INFINITE);
#else
    for (int i = 0; i < count; i++) {
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
    {"str_len",        (void*)vortex_str_len,        1},
    {"str_sub",        (void*)vortex_str_sub,        3},
    {"str_from_i64",   (void*)vortex_str_from_i64,   1},
    {"str_from_f32",   (void*)vortex_str_from_f32,   1},
    {"str_byte",       (void*)vortex_str_byte,       2},
    {"str_from_byte",  (void*)vortex_str_from_byte,  1},
    {"str_eq",         (void*)vortex_str_eq,         2},
    {"str_find",       (void*)vortex_str_find,       3},
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
static bool compile_vortex_source_internal(const char* source, void* exec_mem, JITBuffer* out_jit) {
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
    char raw[1024];
    strncpy(raw, path ? path : "", sizeof(raw) - 1);
    raw[sizeof(raw) - 1] = '\0';
#ifdef _WIN32
    char full[1024];
    if (_fullpath(full, raw, sizeof(full)) != NULL) {
        strncpy(out, full, cap - 1);
        out[cap - 1] = '\0';
    } else {
        strncpy(out, raw, cap - 1);
        out[cap - 1] = '\0';
    }
    import_normalize_slashes(out);
    for (size_t i = 0; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    }
#else
    char full[1024];
    if (realpath(raw, full) != NULL) {
        strncpy(out, full, cap - 1);
        out[cap - 1] = '\0';
    } else {
        strncpy(out, raw, cap - 1);
        out[cap - 1] = '\0';
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
    if (!file_path || !file_path[0]) {
        out_dir[0] = '\0';
        return;
    }
    strncpy(out_dir, file_path, cap - 1);
    out_dir[cap - 1] = '\0';
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
    if (!spec || !spec[0]) return false;
    char norm_spec[512];
    strncpy(norm_spec, spec, sizeof(norm_spec) - 1);
    norm_spec[sizeof(norm_spec) - 1] = '\0';
    import_normalize_slashes(norm_spec);

    char parent_dir[512];
    import_extract_dir(parent_file, parent_dir, sizeof(parent_dir));

    /* 1. Resolve relative to parent file directory */
    if (parent_dir[0]) {
        char cand[1024];
        snprintf(cand, sizeof(cand), "%s/%s", parent_dir, norm_spec);
        if (import_is_regular_file(cand)) {
            strncpy(out, cand, cap - 1);
            out[cap - 1] = '\0';
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


/* --- Standalone Executable Packaging (vortex build) --- */
bool check_and_run_embedded_payload(int argc, char** argv) {
    (void)argc;
    char self_path[1024] = {0};
    vortex_get_self_path(self_path, sizeof(self_path), (argc > 0 && argv) ? argv[0] : NULL);

    if (self_path[0] == '\0') return false;

    FILE* f = fopen(self_path, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long file_size = ftell(f);
    if (file_size < (long)sizeof(VortexPackageFooter)) {
        fclose(f);
        return false;
    }

    if (fseek(f, file_size - (long)sizeof(VortexPackageFooter), SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    VortexPackageFooter footer;
    if (fread(&footer, sizeof(footer), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (memcmp(footer.footer_magic, VORTEX_PACK_MAGIC_FOOTER, 8) != 0) {
        fclose(f);
        return false;
    }

    g_is_packaged_app = true;

    if (footer.payload_offset >= (uint64_t)(file_size - sizeof(VortexPackageFooter))) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Invalid embedded payload offset (%llu)\n",
                (unsigned long long)footer.payload_offset);
        vortex_error("");
    }

    if (fseek(f, (long)footer.payload_offset, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Failed to seek to embedded payload\n");
        vortex_error("");
    }

    VortexPackageHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Failed to read embedded package header\n");
        vortex_error("");
    }

    if (memcmp(hdr.header_magic, VORTEX_PACK_MAGIC_HEADER, 4) != 0) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Corrupted package header magic\n");
        vortex_error("");
    }

    if (hdr.version != VORTEX_PACK_VERSION) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Unsupported package version (%u)\n", hdr.version);
        vortex_error("");
    }

    if (hdr.source_len == 0 || hdr.source_len > 100 * 1024 * 1024) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Invalid embedded source size (%u bytes)\n", hdr.source_len);
        vortex_error("");
    }

    char* src_buf = (char*)malloc(hdr.source_len + 1);
    if (!src_buf) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Out of memory loading embedded payload (%u bytes)\n", hdr.source_len);
        vortex_error("");
    }

    size_t read_bytes = fread(src_buf, 1, hdr.source_len, f);
    src_buf[read_bytes] = '\0';
    fclose(f);

    if (read_bytes != hdr.source_len) {
        free(src_buf);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Truncated embedded payload (%zu/%u bytes read)\n",
                read_bytes, hdr.source_len);
        vortex_error("");
    }

    g_compile_mode = (hdr.flags & VORTEX_PACK_FLAG_RELEASE) ? VORTEX_MODE_RELEASE : VORTEX_MODE_SAFE;
    g_current_filename = "<packaged>";
    int res = execute_vortex_source(src_buf);
    free(src_buf);
    if (res != 0) {
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Packaged application exited with status %d\n", res);
        vortex_wait_on_standalone_error("Packaged application initialization or execution failed.");
    }
    exit(res);
    return true;
}

#pragma pack(push, 1)
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} VxElf64Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} VxElf64Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} VxElf64Shdr;

typedef struct {
    uint32_t magic;
    int32_t  cputype;
    int32_t  cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} VxMachHeader64;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t  maxprot;
    int32_t  initprot;
    uint32_t nsects;
    uint32_t flags;
} VxMachSegCmd64;

typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} VxMachSect64;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
} VxMachMainCmd;
#pragma pack(pop)

static bool write_zero_bytes(FILE* f, size_t count) {
    static const uint8_t zeros[256] = {0};
    while (count > 0) {
        size_t chunk = (count < sizeof(zeros)) ? count : sizeof(zeros);
        if (fwrite(zeros, 1, chunk, f) != chunk) return false;
        count -= chunk;
    }
    return true;
}

static long probe_runtime_clean_size(FILE* f, long file_size) {
    long clean_size = file_size;
    if (file_size >= (long)sizeof(VortexPackageFooter)) {
        if (fseek(f, file_size - (long)sizeof(VortexPackageFooter), SEEK_SET) == 0) {
            VortexPackageFooter prev_footer;
            if (fread(&prev_footer, sizeof(prev_footer), 1, f) == 1 &&
                memcmp(prev_footer.footer_magic, VORTEX_PACK_MAGIC_FOOTER, 8) == 0 &&
                prev_footer.payload_offset < (uint64_t)(file_size - (long)sizeof(VortexPackageFooter))) {
                clean_size = (long)prev_footer.payload_offset;
            }
        }
    }
    fseek(f, 0, SEEK_SET);
    return clean_size;
}

static bool copy_runtime_prefix(FILE* src, FILE* dst, long remaining) {
    char copy_buf[65536];
    while (remaining > 0) {
        size_t chunk = (remaining < (long)sizeof(copy_buf)) ? (size_t)remaining : sizeof(copy_buf);
        size_t n = fread(copy_buf, 1, chunk, src);
        if (n == 0) break;
        if (fwrite(copy_buf, 1, n, dst) != n) return false;
        remaining -= (long)n;
    }
    return remaining == 0;
}

static bool write_package_payload_and_footer(FILE* out_f, const char* src_buf, size_t src_len, uint64_t payload_offset) {
    VortexPackageHeader hdr;
    memcpy(hdr.header_magic, VORTEX_PACK_MAGIC_HEADER, 4);
    hdr.version = VORTEX_PACK_VERSION;
    hdr.flags = (g_compile_mode == VORTEX_MODE_RELEASE) ? VORTEX_PACK_FLAG_RELEASE : 0;
    hdr.num_structs = (uint32_t)g_struct_type_count;
    hdr.num_constants = 0;
    hdr.source_len = (uint32_t)src_len;

    if (fwrite(&hdr, sizeof(hdr), 1, out_f) != 1 ||
        fwrite(src_buf, 1, src_len, out_f) != src_len) {
        return false;
    }

    VortexPackageFooter footer;
    footer.payload_offset = payload_offset;
    memcpy(footer.footer_magic, VORTEX_PACK_MAGIC_FOOTER, 8);
    return fwrite(&footer, sizeof(footer), 1, out_f) == 1;
}

static void ad_hoc_codesign_if_apple(const char* output_path) {
#ifdef __APPLE__
    if (g_target_abi == VORTEX_ABI_DARWIN64) {
        pid_t pid = fork();
        if (pid == 0) {
            const char* cs_argv[] = {"codesign", "--force", "--sign", "-", output_path, NULL};
            execvp("codesign", (char* const*)cs_argv);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }
#else
    (void)output_path;
#endif
}

static FILE* open_companion_runtime(const char* self_path, const VortexTargetDesc* td, char* out_found, size_t cap) {
    const char* ext = (td->abi == VORTEX_ABI_WIN64) ? ".exe" : "";
    char dir[1024] = {0};
    strncpy(dir, self_path, sizeof(dir) - 1);
    char* slash1 = strrchr(dir, '/');
    char* slash2 = strrchr(dir, '\\');
    char* sep = (slash1 && slash2) ? ((slash1 > slash2) ? slash1 : slash2) : (slash1 ? slash1 : slash2);
    if (sep) {
        *sep = '\0';
    } else {
        strcpy(dir, ".");
    }

    snprintf(out_found, cap, "%s/vortex-%s%s", dir, td->name, ext);
    FILE* f = fopen(out_found, "rb");
    if (f) return f;

    snprintf(out_found, cap, "%s/targets/vortex-%s%s", dir, td->name, ext);
    return fopen(out_found, "rb");
}

static bool emit_elf64_arm64_binary(const char* output_path, const uint8_t* jit_code, size_t jit_size,
                                    const char* src_buf, size_t src_len,
                                    long* out_code_bytes, long* out_total_bytes) {
    static const char shstrtab[] = "\0.text\0.shstrtab\0";
    const uint64_t vaddr_base = 0x400000ULL;
    const uint64_t text_off = 0x1000ULL;
    const uint32_t stub[4] = {
        0x94000004U, /* BL +16 (enter Vortex JIT main) */
        0xD2800000U, /* MOVZ X0, #0 */
        0xD2800BA8U, /* MOVZ X8, #93 (__NR_exit on Linux ARM64) */
        0xD4000001U  /* SVC #0 */
    };
    uint64_t text_size = sizeof(stub) + (uint64_t)jit_size;
    uint64_t shstr_off = text_off + text_size;
    uint64_t shoff = (shstr_off + sizeof(shstrtab) + 7ULL) & ~7ULL;

    VxElf64Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = 0x7F; ehdr.e_ident[1] = 'E'; ehdr.e_ident[2] = 'L'; ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 2; /* ELFCLASS64 */
    ehdr.e_ident[5] = 1; /* ELFDATA2LSB */
    ehdr.e_ident[6] = 1; /* EV_CURRENT */
    ehdr.e_type = 2;     /* ET_EXEC */
    ehdr.e_machine = 183;/* EM_AARCH64 */
    ehdr.e_version = 1;
    ehdr.e_entry = vaddr_base + text_off;
    ehdr.e_phoff = sizeof(VxElf64Ehdr);
    ehdr.e_shoff = shoff;
    ehdr.e_ehsize = sizeof(VxElf64Ehdr);
    ehdr.e_phentsize = sizeof(VxElf64Phdr);
    ehdr.e_phnum = 1;
    ehdr.e_shentsize = sizeof(VxElf64Shdr);
    ehdr.e_shnum = 3;
    ehdr.e_shstrndx = 2;

    VxElf64Phdr phdr;
    memset(&phdr, 0, sizeof(phdr));
    phdr.p_type = 1; /* PT_LOAD */
    phdr.p_flags = 5;/* PF_R | PF_X */
    phdr.p_offset = 0;
    phdr.p_vaddr = vaddr_base;
    phdr.p_paddr = vaddr_base;
    phdr.p_filesz = text_off + text_size;
    phdr.p_memsz = text_off + text_size;
    phdr.p_align = 0x10000ULL;

    VxElf64Shdr shdrs[3];
    memset(shdrs, 0, sizeof(shdrs));
    shdrs[1].sh_name = 1; /* ".text" */
    shdrs[1].sh_type = 1; /* SHT_PROGBITS */
    shdrs[1].sh_flags = 6;/* SHF_ALLOC | SHF_EXECINSTR */
    shdrs[1].sh_addr = vaddr_base + text_off;
    shdrs[1].sh_offset = text_off;
    shdrs[1].sh_size = text_size;
    shdrs[1].sh_addralign = 4;

    shdrs[2].sh_name = 7; /* ".shstrtab" */
    shdrs[2].sh_type = 3; /* SHT_STRTAB */
    shdrs[2].sh_offset = shstr_off;
    shdrs[2].sh_size = sizeof(shstrtab);
    shdrs[2].sh_addralign = 1;

    FILE* out_f = fopen(output_path, "wb");
    if (!out_f) return false;

    bool ok = (fwrite(&ehdr, sizeof(ehdr), 1, out_f) == 1) &&
              (fwrite(&phdr, sizeof(phdr), 1, out_f) == 1) &&
              write_zero_bytes(out_f, (size_t)(text_off - (sizeof(ehdr) + sizeof(phdr)))) &&
              (fwrite(stub, sizeof(stub), 1, out_f) == 1) &&
              (jit_size == 0 || fwrite(jit_code, 1, jit_size, out_f) == jit_size) &&
              (fwrite(shstrtab, sizeof(shstrtab), 1, out_f) == 1) &&
              write_zero_bytes(out_f, (size_t)(shoff - (shstr_off + sizeof(shstrtab)))) &&
              (fwrite(shdrs, sizeof(shdrs), 1, out_f) == 1);

    if (ok) {
        uint64_t payload_off = (uint64_t)ftell(out_f);
        ok = write_package_payload_and_footer(out_f, src_buf, src_len, payload_off);
    }

    *out_code_bytes = (long)text_size;
    *out_total_bytes = ftell(out_f);
    fclose(out_f);
    return ok;
}

static bool emit_macho64_arm64_binary(const char* output_path, const uint8_t* jit_code, size_t jit_size,
                                      const char* src_buf, size_t src_len,
                                      long* out_code_bytes, long* out_total_bytes) {
    const uint64_t text_vmaddr = 0x100000000ULL;
    const uint64_t text_off = 0x1000ULL;
    const uint32_t stub[4] = {
        0x94000004U, /* BL +16 (enter Vortex JIT main) */
        0xD2800000U, /* MOVZ X0, #0 */
        0xD2800030U, /* MOVZ X16, #1 (SYS_exit on Darwin ARM64) */
        0xD4001001U  /* SVC #0x80 */
    };
    uint64_t text_size = sizeof(stub) + (uint64_t)jit_size;
    uint64_t text_end = text_off + text_size;
    uint64_t seg_size = (text_end + 0x3FFFULL) & ~0x3FFFULL; /* 16 KiB Apple Silicon page */

    uint32_t cmds_size = (uint32_t)(sizeof(VxMachSegCmd64) + sizeof(VxMachSegCmd64) +
                                    sizeof(VxMachSect64) + sizeof(VxMachMainCmd));

    VxMachHeader64 mh;
    memset(&mh, 0, sizeof(mh));
    mh.magic = 0xFEEDFACFU;     /* MH_MAGIC_64 */
    mh.cputype = 0x0100000C;    /* CPU_TYPE_ARM64 */
    mh.cpusubtype = 0;          /* CPU_SUBTYPE_ARM64_ALL */
    mh.filetype = 2;            /* MH_EXECUTE */
    mh.ncmds = 3;
    mh.sizeofcmds = cmds_size;
    mh.flags = 0x85U;           /* MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL */

    VxMachSegCmd64 pagezero;
    memset(&pagezero, 0, sizeof(pagezero));
    pagezero.cmd = 0x19U;       /* LC_SEGMENT_64 */
    pagezero.cmdsize = sizeof(VxMachSegCmd64);
    strncpy(pagezero.segname, "__PAGEZERO", sizeof(pagezero.segname));
    pagezero.vmsize = 0x100000000ULL;

    VxMachSegCmd64 text_seg;
    memset(&text_seg, 0, sizeof(text_seg));
    text_seg.cmd = 0x19U;
    text_seg.cmdsize = sizeof(VxMachSegCmd64) + sizeof(VxMachSect64);
    strncpy(text_seg.segname, "__TEXT", sizeof(text_seg.segname));
    text_seg.vmaddr = text_vmaddr;
    text_seg.vmsize = seg_size;
    text_seg.fileoff = 0;
    text_seg.filesize = seg_size;
    text_seg.maxprot = 5;       /* VM_PROT_READ | VM_PROT_EXECUTE */
    text_seg.initprot = 5;
    text_seg.nsects = 1;

    VxMachSect64 text_sect;
    memset(&text_sect, 0, sizeof(text_sect));
    strncpy(text_sect.sectname, "__text", sizeof(text_sect.sectname));
    strncpy(text_sect.segname, "__TEXT", sizeof(text_sect.segname));
    text_sect.addr = text_vmaddr + text_off;
    text_sect.size = text_size;
    text_sect.offset = (uint32_t)text_off;
    text_sect.align = 2;        /* 2^2 = 4-byte alignment */
    text_sect.flags = 0x80000400U; /* S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS */

    VxMachMainCmd main_cmd;
    memset(&main_cmd, 0, sizeof(main_cmd));
    main_cmd.cmd = 0x80000028U; /* LC_MAIN */
    main_cmd.cmdsize = sizeof(VxMachMainCmd);
    main_cmd.entryoff = text_off;

    size_t hdr_bytes = sizeof(mh) + cmds_size;

    FILE* out_f = fopen(output_path, "wb");
    if (!out_f) return false;

    bool ok = (fwrite(&mh, sizeof(mh), 1, out_f) == 1) &&
              (fwrite(&pagezero, sizeof(pagezero), 1, out_f) == 1) &&
              (fwrite(&text_seg, sizeof(text_seg), 1, out_f) == 1) &&
              (fwrite(&text_sect, sizeof(text_sect), 1, out_f) == 1) &&
              (fwrite(&main_cmd, sizeof(main_cmd), 1, out_f) == 1) &&
              write_zero_bytes(out_f, (size_t)(text_off - hdr_bytes)) &&
              (fwrite(stub, sizeof(stub), 1, out_f) == 1) &&
              (jit_size == 0 || fwrite(jit_code, 1, jit_size, out_f) == jit_size) &&
              write_zero_bytes(out_f, (size_t)(seg_size - text_end));

    if (ok) {
        uint64_t payload_off = (uint64_t)ftell(out_f);
        ok = write_package_payload_and_footer(out_f, src_buf, src_len, payload_off);
    }

    *out_code_bytes = (long)text_size;
    *out_total_bytes = ftell(out_f);
    fclose(out_f);
    return ok;
}

int vortex_cmd_build(int argc, char** argv) {
    const char* input_path = NULL;
    char output_path[1024] = {0};

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) {
            g_compile_mode = VORTEX_MODE_RELEASE;
        } else if (strcmp(argv[i], "--safe") == 0) {
            g_compile_mode = VORTEX_MODE_SAFE;
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            const char* tname = argv[++i];
            if (!vortex_set_target_name(tname)) {
                fprintf(stderr, "[VORTEX BUILD ERROR] Unknown target '%s'\n", tname);
                return 1;
            }
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            if (!vortex_set_target_name(argv[i] + 9)) {
                fprintf(stderr, "[VORTEX BUILD ERROR] Unknown target '%s'\n", argv[i] + 9);
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            strncpy(output_path, argv[++i], sizeof(output_path) - 1);
        } else if (argv[i][0] != '-') {
            if (!input_path) input_path = argv[i];
            else if (output_path[0] == '\0') strncpy(output_path, argv[i], sizeof(output_path) - 1);
        }
    }

    if (!input_path) {
        fprintf(stderr, "[VORTEX BUILD ERROR] Missing input script file\n");
        fprintf(stderr, "Usage: vortex build <script.vx> [-o <output>] [--safe|--release] [--target <triple>]\n");
        return 1;
    }

    const VortexTargetDesc* td = vortex_get_target_desc();
    if (output_path[0] == '\0') {
        strncpy(output_path, input_path, sizeof(output_path) - 1);
        char* dot = strrchr(output_path, '.');
        if (dot) *dot = '\0';
        if (td->abi == VORTEX_ABI_WIN64) {
            strncat(output_path, ".exe", sizeof(output_path) - strlen(output_path) - 1);
        }
    }

    char* src_buf = vortex_resolve_imports(input_path);
    if (!src_buf) {
        return 1;
    }
    size_t r = strlen(src_buf);
    if (r == 0) {
        free(src_buf);
        fprintf(stderr, "[VORTEX BUILD ERROR] Source file '%s' is empty or unreadable\n", input_path);
        return 1;
    }

    char self_path[1024] = {0};
    vortex_get_self_path(self_path, sizeof(self_path), argv ? argv[0] : NULL);

    FILE* rt_f = NULL;
    char companion_path[1024] = {0};
    bool is_cross = vortex_is_cross_target();

    if (!is_cross) {
        rt_f = fopen(self_path, "rb");
    } else {
        rt_f = open_companion_runtime(self_path, td, companion_path, sizeof(companion_path));
    }

    if (rt_f) {
        fseek(rt_f, 0, SEEK_END);
        long rt_size = ftell(rt_f);
        long runtime_clean_size = probe_runtime_clean_size(rt_f, rt_size);
        if (runtime_clean_size <= 0) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Invalid base runtime executable size\n");
            fclose(rt_f);
            free(src_buf);
            return 1;
        }

        FILE* out_f = fopen(output_path, "wb");
        if (!out_f) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Cannot open output executable '%s' for writing\n", output_path);
            fclose(rt_f);
            free(src_buf);
            return 1;
        }

        if (!copy_runtime_prefix(rt_f, out_f, runtime_clean_size)) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Write error while copying base runtime\n");
            fclose(rt_f);
            fclose(out_f);
            free(src_buf);
            return 1;
        }
        fclose(rt_f);

        uint64_t payload_offset = (uint64_t)ftell(out_f);
        bool ok = write_package_payload_and_footer(out_f, src_buf, r, payload_offset);
        free(src_buf);
        if (!ok) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Failed to write embedded payload to '%s'\n", output_path);
            fclose(out_f);
            return 1;
        }

        long final_size = ftell(out_f);
        fclose(out_f);
        ad_hoc_codesign_if_apple(output_path);
#ifndef _WIN32
        chmod(output_path, 0755);
#endif
        printf("[VORTEX BUILD] Executable standalone generated (%s): %s\n", td->name, output_path);
        printf("               Runtime: %ld bytes | Payload: %zu bytes | Total: %ld bytes\n",
               runtime_clean_size, r + sizeof(VortexPackageHeader) + sizeof(VortexPackageFooter), final_size);
        printf("               Payload offset: %llu (0x%llX)\n",
               (unsigned long long)payload_offset, (unsigned long long)payload_offset);
        return 0;
    }

    if (td->arch == VORTEX_ARCH_ARM64) {
        g_current_filename = input_path;
        void* code_mem = alloc_exec_page(MAX_JIT_CODE_SIZE);
        if (!code_mem) {
            free(src_buf);
            fprintf(stderr, "[VORTEX BUILD ERROR] Out of memory compiling ARM64 target\n");
            return 1;
        }
        JITBuffer jit;
        compile_vortex_source_internal(src_buf, code_mem, &jit);

        long code_bytes = 0;
        long total_bytes = 0;
        const char* fmt_name = (td->abi == VORTEX_ABI_DARWIN64) ? "Mach-O 64 ARM64" : "ELF64 AArch64";
        bool ok = (td->abi == VORTEX_ABI_DARWIN64)
                  ? emit_macho64_arm64_binary(output_path, jit.code, jit.size, src_buf, r, &code_bytes, &total_bytes)
                  : emit_elf64_arm64_binary(output_path, jit.code, jit.size, src_buf, r, &code_bytes, &total_bytes);
        free_exec_page(code_mem, MAX_JIT_CODE_SIZE);
        free(src_buf);
        if (!ok) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Failed to write %s output '%s'\n", fmt_name, output_path);
            return 1;
        }
        ad_hoc_codesign_if_apple(output_path);
#ifndef _WIN32
        chmod(output_path, 0755);
#endif
        printf("[VORTEX BUILD] Cross-compiled %s standalone binary: %s\n", fmt_name, output_path);
        printf("               Target: %s | Code (.text): %ld bytes | Total: %ld bytes\n",
               td->name, code_bytes, total_bytes);
        printf("               Note: Place companion runtime 'vortex-%s' beside binary for full dynamic FFI runtime cloning.\n",
               td->name);
        return 0;
    }

    free(src_buf);
    fprintf(stderr, "[VORTEX BUILD ERROR] Cannot open runtime executable '%s' (companion '%s' not found)\n",
            self_path, companion_path[0] ? companion_path : td->name);
    return 1;
}

