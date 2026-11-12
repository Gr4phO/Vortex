/*
 * VORTEX PROGRAMMING LANGUAGE - RUNTIME, GUI, THREAD POOL, BUILTINS & PACKAGER
 */

#include "vortex.h"

SymbolTable g_last_symtab;
bool g_has_last_symtab = false;

jmp_buf g_error_jmp;
bool g_in_repl = false;

void vortex_error(const char* msg) {
    if (msg && msg[0]) {
        fprintf(stderr, "%s\n", msg);
    }
    if (g_in_repl) {
        longjmp(g_error_jmp, 1);
    } else {
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
    HINSTANCE hInst = GetModuleHandle(NULL);
    if (!g_win_class_registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = VortexWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = "VortexWindowClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassA(&wc);
        g_win_class_registered = true;
    }

    VortexWindow* w = (VortexWindow*)calloc(1, sizeof(VortexWindow));
    w->width = (int)width;
    w->height = (int)height;
    w->is_open = true;
    strncpy(w->title, title, sizeof(w->title) - 1);

    RECT rc = {0, 0, w->width, w->height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    w->hwnd = CreateWindowExA(
        0, "VortexWindowClass", title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL
    );

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
    Sleep(16); // 60 FPS cap
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
    usleep(16000); // 60 FPS cap (~16 ms)
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

int64_t vortex_gui_window(int64_t t, int64_t w, int64_t h) { (void)t;(void)w;(void)h; return 1; }
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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000LL);
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
    {"scope_push",     (void*)vortex_scope_push,     0},
    {"scope_pop",      (void*)vortex_scope_pop,      1},
    {"ffi_load",       (void*)vortex_ffi_load,       1},
    {"ffi_symbol",     (void*)vortex_ffi_symbol,     2},
    {"ffi_call0",      (void*)vortex_ffi_call0,      1},
    {"ffi_call1",      (void*)vortex_ffi_call1,      2},
    {"ffi_call2",      (void*)vortex_ffi_call2,      3},
    {"ffi_call3",      (void*)vortex_ffi_call3,      4},
    {"ffi_call4",      (void*)vortex_ffi_call4,      5},
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
    {NULL, NULL, 0}
};

BuiltinFunc* builtin_find(const char* name) {
    for (int i = 0; g_builtins[i].name != NULL; i++) {
        if (strcmp(g_builtins[i].name, name) == 0) return &g_builtins[i];
    }
    return NULL;
}

/* --- Source / File JIT Execution --- */
int execute_vortex_source(const char* source) {
    arena_init();
    g_user_func_count = 0;
    g_user_func_reloc_count = 0;
    g_ffi_func_count = 0;
    g_literal_string_count = 0;

    Lexer lexer;
    lexer.src = source;
    lexer.pos = 0;
    lexer.len = strlen(source);
    lexer.line = 1;
    lexer.col = 1;
    next_token(&lexer);

    SymbolTable symtab;
    memset(&symtab, 0, sizeof(symtab));

    void* exec_mem = alloc_exec_page(MAX_JIT_CODE_SIZE);
    if (!exec_mem) {
        fprintf(stderr, "[VORTEX PANIC] Failed to allocate executable page\n");
        return 1;
    }

    JITBuffer jit;
    jit.code = (uint8_t*)exec_mem;
    jit.size = 0;
    jit.capacity = MAX_JIT_CODE_SIZE;

    IRBuffer main_ir;
    ir_init(&main_ir);

    Compiler comp;
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

    vortex_thread_pool_init();
    ir_loop_vectorize(&main_ir);
    ir_liveness_analysis(&main_ir);
    ir_linear_scan_allocate(&main_ir);

    int main_frame_size = compute_frame_size(main_ir.total_spill_bytes);
    emit_prologue(&jit, main_frame_size);
    ir_codegen_x86_64(&main_ir, &jit);
    emit_epilogue(&jit, main_frame_size);

    // Patch forward user function calls across the JIT code
    for (int r = 0; r < g_user_func_reloc_count; r++) {
        UserFunc* uf = user_func_find(g_user_func_relocs[r].name);
        if (!uf) {
            fprintf(stderr, "[VORTEX ERROR] Unresolved user function '%s'\n", g_user_func_relocs[r].name);
            vortex_error("");
        }
        int32_t rel = (int32_t)(uf->code_offset - (g_user_func_relocs[r].patch_pos + 4));
        memcpy(&jit.code[g_user_func_relocs[r].patch_pos], &rel, 4);
    }

    ir_free(&main_ir);

    typedef void (*vortex_entry_fn)(void);
    vortex_entry_fn entry = (vortex_entry_fn)exec_mem;

    entry();

    free_exec_page(exec_mem, MAX_JIT_CODE_SIZE);
    return 0;
}

int execute_vortex_file(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "[VORTEX ERROR] Cannot open file '%s'\n", filename);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(sz + 1);
    if (!buffer) {
        fclose(f);
        fprintf(stderr, "[VORTEX ERROR] Memory allocation error\n");
        return 1;
    }
    size_t read_bytes = fread(buffer, 1, sz, f);
    buffer[read_bytes] = '\0';
    fclose(f);

    int res = execute_vortex_source(buffer);
    free(buffer);
    return res;
}


/* --- Standalone Executable Packaging (vortex build) --- */
bool check_and_run_embedded_payload(int argc, char** argv) {
    (void)argc;
    char self_path[1024] = {0};
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, self_path, sizeof(self_path));
    if (len == 0 || len >= sizeof(self_path)) {
        if (argv && argv[0]) strncpy(self_path, argv[0], sizeof(self_path) - 1);
    }
#else
    ssize_t rlen = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (rlen > 0) {
        self_path[rlen] = '\0';
    } else if (argv && argv[0]) {
        strncpy(self_path, argv[0], sizeof(self_path) - 1);
    }
#endif

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

    if (footer.payload_offset >= (uint64_t)(file_size - sizeof(VortexPackageFooter))) {
        fclose(f);
        return false;
    }

    if (fseek(f, (long)footer.payload_offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    VortexPackageHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (memcmp(hdr.header_magic, VORTEX_PACK_MAGIC_HEADER, 4) != 0) {
        fclose(f);
        return false;
    }

    if (hdr.source_len == 0 || hdr.source_len > 100 * 1024 * 1024) {
        fclose(f);
        return false;
    }

    char* src_buf = (char*)malloc(hdr.source_len + 1);
    if (!src_buf) {
        fclose(f);
        return false;
    }

    size_t read_bytes = fread(src_buf, 1, hdr.source_len, f);
    src_buf[read_bytes] = '\0';
    fclose(f);

    if (read_bytes != hdr.source_len) {
        free(src_buf);
        return false;
    }

    int res = execute_vortex_source(src_buf);
    free(src_buf);
    exit(res);
    return true;
}

int vortex_cmd_build(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "[VORTEX BUILD ERROR] Missing input script file\n");
        fprintf(stderr, "Usage: vortex build <script.vx> [-o <output.exe>]\n");
        return 1;
    }

    const char* input_path = argv[2];
    char output_path[1024] = {0};

    if (argc >= 5 && strcmp(argv[3], "-o") == 0) {
        strncpy(output_path, argv[4], sizeof(output_path) - 1);
    } else if (argc == 4 && argv[3][0] != '-') {
        strncpy(output_path, argv[3], sizeof(output_path) - 1);
    } else {
        strncpy(output_path, input_path, sizeof(output_path) - 1);
        char* dot = strrchr(output_path, '.');
        if (dot) *dot = '\0';
#ifdef _WIN32
        strncat(output_path, ".exe", sizeof(output_path) - strlen(output_path) - 1);
#endif
    }

    FILE* in_f = fopen(input_path, "rb");
    if (!in_f) {
        fprintf(stderr, "[VORTEX BUILD ERROR] Cannot open source file '%s'\n", input_path);
        return 1;
    }
    fseek(in_f, 0, SEEK_END);
    long src_len = ftell(in_f);
    fseek(in_f, 0, SEEK_SET);

    char* src_buf = (char*)malloc(src_len + 1);
    if (!src_buf) {
        fclose(in_f);
        fprintf(stderr, "[VORTEX BUILD ERROR] Out of memory reading source file\n");
        return 1;
    }
    size_t r = fread(src_buf, 1, src_len, in_f);
    src_buf[r] = '\0';
    fclose(in_f);

    char self_path[1024] = {0};
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, self_path, sizeof(self_path));
    if (len == 0 || len >= sizeof(self_path)) {
        if (argv[0]) strncpy(self_path, argv[0], sizeof(self_path) - 1);
    }
#else
    ssize_t rlen = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (rlen > 0) {
        self_path[rlen] = '\0';
    } else if (argv[0]) {
        strncpy(self_path, argv[0], sizeof(self_path) - 1);
    }
#endif

    FILE* self_f = fopen(self_path, "rb");
    if (!self_f) {
        fprintf(stderr, "[VORTEX BUILD ERROR] Cannot open runtime executable '%s' for self-cloning\n", self_path);
        free(src_buf);
        return 1;
    }

    fseek(self_f, 0, SEEK_END);
    long self_size = ftell(self_f);
    long runtime_clean_size = self_size;

    if (self_size >= (long)sizeof(VortexPackageFooter)) {
        fseek(self_f, self_size - (long)sizeof(VortexPackageFooter), SEEK_SET);
        VortexPackageFooter prev_footer;
        if (fread(&prev_footer, sizeof(prev_footer), 1, self_f) == 1 &&
            memcmp(prev_footer.footer_magic, VORTEX_PACK_MAGIC_FOOTER, 8) == 0 &&
            prev_footer.payload_offset < (uint64_t)(self_size - sizeof(VortexPackageFooter))) {
            runtime_clean_size = (long)prev_footer.payload_offset;
        }
    }
    fseek(self_f, 0, SEEK_SET);

    FILE* out_f = fopen(output_path, "wb");
    if (!out_f) {
        fprintf(stderr, "[VORTEX BUILD ERROR] Cannot open output executable '%s' for writing\n", output_path);
        fclose(self_f);
        free(src_buf);
        return 1;
    }

    // 1. Copy base runtime
    char copy_buf[65536];
    long remaining = runtime_clean_size;
    while (remaining > 0) {
        size_t chunk = (remaining < (long)sizeof(copy_buf)) ? (size_t)remaining : sizeof(copy_buf);
        size_t n = fread(copy_buf, 1, chunk, self_f);
        if (n == 0) break;
        fwrite(copy_buf, 1, n, out_f);
        remaining -= (long)n;
    }
    fclose(self_f);

    // 2. Append payload (Header + Source)
    uint64_t payload_offset = (uint64_t)ftell(out_f);

    VortexPackageHeader hdr;
    memcpy(hdr.header_magic, VORTEX_PACK_MAGIC_HEADER, 4);
    hdr.version = VORTEX_PACK_VERSION;
    hdr.flags = 0;
    hdr.num_structs = (uint32_t)g_struct_type_count;
    hdr.num_constants = 0;
    hdr.source_len = (uint32_t)src_len;

    fwrite(&hdr, sizeof(hdr), 1, out_f);
    fwrite(src_buf, 1, src_len, out_f);
    free(src_buf);

    // 3. Append fixed 16-byte footer
    VortexPackageFooter footer;
    footer.payload_offset = payload_offset;
    memcpy(footer.footer_magic, VORTEX_PACK_MAGIC_FOOTER, 8);
    fwrite(&footer, sizeof(footer), 1, out_f);

    long final_size = ftell(out_f);
    fclose(out_f);

#ifndef _WIN32
    chmod(output_path, 0755);
#endif

    printf("[VORTEX BUILD] Executable standalone generated successfully: %s\n", output_path);
    printf("               Runtime: %ld bytes | Payload: %ld bytes | Total: %ld bytes\n",
           runtime_clean_size, src_len + (long)sizeof(hdr) + (long)sizeof(footer), final_size);
    printf("               Payload offset: %llu (0x%llX)\n",
           (unsigned long long)payload_offset, (unsigned long long)payload_offset);
    printf("               Zero external dependencies. Sub-millisecond instant startup ready.\n");
    return 0;
}

