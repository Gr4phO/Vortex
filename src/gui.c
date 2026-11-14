/*
 * VORTEX NATIVE GUI & SOFTWARE FRAMEBUFFER SUBSYSTEM (Win32 / X11 / Headless)
 */

#include "vortex.h"

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

