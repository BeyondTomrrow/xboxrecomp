/**
 * fb_present.c -- the game window and the guest framebuffer view
 * =============================================================================
 *
 * WHAT THIS FILE DOES
 *
 *   An Xbox title draws into a framebuffer in guest RAM and tells the kernel
 *   where it is (AvSetDisplayMode). On hardware the CRTC scans that memory out
 *   to the TV. Nothing in the recompiled runtime does that, so this file owns
 *   the one Windows window the player sees, and shows the picture in it.
 *
 *   There are two ways a frame reaches the window:
 *
 *     1. GDI blit (fallback / bring-up). This thread reads the guest
 *        framebuffer, converts it to BGRA8 and StretchDIBits it into the
 *        window. Few moving parts, so it is the tool for answering "is the
 *        read path broken or did the title really draw black?".
 *
 *     2. GPU present (normal play). The D3D11 renderer
 *        (src/kernel/nv2a_gpu_present.inc.c) presents its render target
 *        through a swap chain on this window and calls
 *        xbox_FramebufferSetGpuPresent(1). From then on the GDI path is idle;
 *        this file only keeps the window alive (thread, message pump, style,
 *        DPI, window mode, key hook).
 *
 * THREADING
 *
 *   Everything that touches the HWND runs on the window thread (fb_thread).
 *   Other threads talk to it only through:
 *     - plain stores to the volatile/Interlocked fields in g_fb, and
 *     - PostMessage (FB_WM_APPLY_MODE) for window-mode changes.
 *   The BGRA copy buffer (g_fb.rgb) is swapped on resize and read by
 *   xbox_FramebufferDumpBmp from another thread, so it is guarded by
 *   g_fb.rgb_lock.
 *
 * DPI (why 4K screens used to crop)
 *
 *   The window must work in physical pixels. If the process is DPI-unaware,
 *   Windows reports a 3840x2160 monitor at 150% scaling as 2560x1440, while
 *   DXGI still sees 3840x2160 -- the swap chain and the window disagree and
 *   the picture is cropped or blurred.
 *
 *   The reliable fix is an application manifest in the GAME executable
 *   declaring PerMonitorV2 (see the game repo's src/nfl2k5.manifest). This
 *   library is shared with other titles that may not ship one, so it still
 *   tries SetProcessDpiAwarenessContext as a fallback -- but that call fails
 *   if anything in the process already fixed the awareness or created a
 *   window first, so the result is logged rather than assumed.
 *
 *   All window-rect math goes through fb_adjust_rect(), which uses the
 *   window's actual DPI, and WM_DPICHANGED re-applies the current mode when
 *   the window moves to a monitor with different scaling.
 *
 * ENVIRONMENT VARIABLES
 *
 *   RECOMP_FB_WINDOW   start the window from xbox_FramebufferWindowStart()
 *   RECOMP_FB_VA       pin the GDI view to one guest address (debugging)
 *   RECOMP_FB_DUMP     write the 60th frame to this .bmp path (debugging)
 *   RECOMP_ULTRAWIDE   open borderless on the cursor's monitor and
 *                      letterbox the 4:3 picture (presentation only)
 *
 * PUBLIC API (declared in video_player.h and used by the renderer/game)
 *
 *   xbox_FramebufferWindowStart / ForceStart   create the window thread
 *   xbox_FramebufferWindowSet / Size           where/what the GDI path reads
 *   xbox_FramebufferApplyMode                  windowed/borderless/fullscreen
 *   xbox_FramebufferWindowHwnd / Va            for the GPU presenter
 *   xbox_FramebufferSetGpuPresent / KeyHook    GPU presenter handoff
 *   xbox_FramebufferDumpBmp                    debugging capture
 */
#include <stdint.h>

#if defined(_WIN32)
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <imm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef USER_DEFAULT_SCREEN_DPI
#define USER_DEFAULT_SCREEN_DPI 96
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* Private message: change window mode on the window thread.
 * wParam = FB_MODE_*, lParam = MAKELPARAM(client width, client height). */
#define FB_WM_APPLY_MODE (WM_APP + 1)

#define FB_CLASS_NAME "ESPN NFL 2K5"
#define FB_TITLE      "ESPN NFL 2K5"

/* Window styles for each mode, kept in one place so the rect math and
 * SetWindowLongPtr can never disagree. */
#define FB_STYLE_WINDOWED   (WS_OVERLAPPEDWINDOW | WS_VISIBLE)
#define FB_STYLE_BORDERLESS (WS_POPUP | WS_VISIBLE)

enum {
    FB_MODE_WINDOWED   = 0, /* client area w x h, centred on the work area   */
    FB_MODE_BORDERLESS = 1, /* covers the window's monitor                    */
    FB_MODE_FULLSCREEN = 2  /* exclusive: the swap chain switches; the window */
    /* itself is made borderless here                 */
};

/* Guest RAM the GDI path may read: the 64 MB contiguous window at
 * 0x80000000 or its low-memory alias. Anything else is an off-screen
 * target or garbage, and is skipped rather than crashing the process
 * (this happened around NFL 2K5's Start Game, reading 0xFFB0AFAF). */
#define FB_GUEST_RAM_SIZE 0x04000000u

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static struct {
    /* Lifecycle */
    volatile LONG running;          /* 1 while fb_thread is alive            */
    volatile HWND hwnd;             /* NULL until the window exists          */

    /* What the GDI path reads (written by the executor thread) */
    volatile uint32_t va;           /* guest address of the front buffer     */
    volatile uint32_t pitch;        /* bytes per row                          */
    volatile LONG want_w, want_h;   /* requested surface size (0 = none)     */

    /* GDI copy buffer (owned by the window thread, guarded for the dump) */
    SRWLOCK   rgb_lock;
    uint32_t *rgb;                  /* BGRA8, width * height                 */
    uint32_t  width, height;        /* size of rgb                           */

    /* GPU presenter handoff */
    volatile LONG gpu_present;      /* 1: the swap chain owns the pixels     */
    void (*volatile key_hook)(unsigned vk);

    /* Current window mode, so WM_DPICHANGED can re-apply it */
    int mode;
    int client_w, client_h;         /* requested windowed client size        */
    int ultrawide;
} g_fb = {
    .rgb_lock = SRWLOCK_INIT,
    .width = 640,
    .height = 480,
};

/* ---------------------------------------------------------------------------
 * DPI helpers
 *
 * The per-monitor DPI APIs are Windows 10 1607+. They are looked up at run
 * time so this file builds whatever WINVER the project uses, and falls back
 * to 96 DPI / AdjustWindowRect on anything older.
 * ------------------------------------------------------------------------- */

typedef BOOL   (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef HANDLE (WINAPI *PFN_GetThreadDpiAwarenessContext)(void);
typedef int    (WINAPI *PFN_GetAwarenessFromDpiAwarenessContext)(HANDLE);
typedef UINT   (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT   (WINAPI *PFN_GetDpiForSystem)(void);
typedef BOOL   (WINAPI *PFN_AdjustWindowRectExForDpi)(RECT *, DWORD, BOOL, DWORD, UINT);

static struct {
    PFN_GetDpiForWindow          get_dpi_for_window;
    PFN_GetDpiForSystem          get_dpi_for_system;
    PFN_AdjustWindowRectExForDpi adjust_rect_for_dpi;
} g_dpi;

/* Make the process per-monitor DPI aware if the manifest has not already,
 * and log what we actually ended up with. Must run before the window is
 * created. */
static void fb_dpi_init(void)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    PFN_SetProcessDpiAwarenessContext       set_ctx;
    PFN_GetThreadDpiAwarenessContext        get_ctx;
    PFN_GetAwarenessFromDpiAwarenessContext ctx_awareness;
    int awareness = -1;

    g_dpi.get_dpi_for_window  = (PFN_GetDpiForWindow)GetProcAddress(user32, "GetDpiForWindow");
    g_dpi.get_dpi_for_system  = (PFN_GetDpiForSystem)GetProcAddress(user32, "GetDpiForSystem");
    g_dpi.adjust_rect_for_dpi = (PFN_AdjustWindowRectExForDpi)GetProcAddress(user32, "AdjustWindowRectExForDpi");

    set_ctx       = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    get_ctx       = (PFN_GetThreadDpiAwarenessContext)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    ctx_awareness = (PFN_GetAwarenessFromDpiAwarenessContext)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");

    /* Fallback only. Returns FALSE (ERROR_ACCESS_DENIED) when the manifest
     * already set the awareness, which is the expected, good case. */
    if (set_ctx)
        set_ctx((HANDLE)-4);        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        else
            SetProcessDPIAware();

    if (get_ctx && ctx_awareness)
        awareness = ctx_awareness(get_ctx());

    /* 0 = unaware, 1 = system aware, 2 = per-monitor aware. Anything but 2
     * means Windows is scaling the window and 4K displays will misbehave. */
    fprintf(stderr, "  [FBWIN] DPI awareness %d (%s), system DPI %u\n",
            awareness,
            awareness == 2 ? "per-monitor, good" :
            awareness == 1 ? "system only, check the manifest" :
            awareness == 0 ? "UNAWARE, expect cropping on scaled displays" : "unknown",
            g_dpi.get_dpi_for_system ? g_dpi.get_dpi_for_system() : USER_DEFAULT_SCREEN_DPI);
}

static UINT fb_window_dpi(HWND h)
{
    if (h && g_dpi.get_dpi_for_window)
        return g_dpi.get_dpi_for_window(h);
    if (g_dpi.get_dpi_for_system)
        return g_dpi.get_dpi_for_system();
    return USER_DEFAULT_SCREEN_DPI;
}

/* Grow a client rect (0,0,w,h) to the full window rect for this style,
 * using the borders/caption size at the given DPI. */
static void fb_adjust_rect(RECT *r, DWORD style, UINT dpi)
{
    if (g_dpi.adjust_rect_for_dpi)
        g_dpi.adjust_rect_for_dpi(r, style, FALSE, 0, dpi);
    else
        AdjustWindowRect(r, style, FALSE);
}

static void fb_monitor_info(HMONITOR mon, MONITORINFO *mi)
{
    memset(mi, 0, sizeof *mi);
    mi->cbSize = sizeof *mi;
    GetMonitorInfoA(mon, mi);
}

/* ---------------------------------------------------------------------------
 * Window mode
 * ------------------------------------------------------------------------- */

/* Windowed: a client area of w x h, centred on the monitor's work area.
 * If it does not fit (the default is sized for 1440p) it is scaled down
 * keeping its shape, leaving room for the frame and taskbar. */
static void fb_apply_windowed(HWND h, int w, int hh)
{
    MONITORINFO mi;
    RECT r;
    UINT dpi = fb_window_dpi(h);
    int mw, mh, ww, wh;
    /* Room for borders and caption, in pixels at this DPI. */
    int margin_w = MulDiv(16, (int)dpi, USER_DEFAULT_SCREEN_DPI);
    int margin_h = MulDiv(48, (int)dpi, USER_DEFAULT_SCREEN_DPI);

    fb_monitor_info(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    mw = mi.rcWork.right - mi.rcWork.left;
    mh = mi.rcWork.bottom - mi.rcWork.top;

    if (w > mw - margin_w || hh > mh - margin_h) {
        double kx = (double)(mw - margin_w) / w;
        double ky = (double)(mh - margin_h) / hh;
        double k  = kx < ky ? kx : ky;
        w  = (int)(w * k);
        hh = (int)(hh * k);
    }

    SetWindowLongPtrA(h, GWL_STYLE, FB_STYLE_WINDOWED);
    r.left = 0; r.top = 0; r.right = w; r.bottom = hh;
    fb_adjust_rect(&r, FB_STYLE_WINDOWED, dpi);
    ww = r.right - r.left;
    wh = r.bottom - r.top;

    SetWindowPos(h, HWND_NOTOPMOST,
                 mi.rcWork.left + (mw > ww ? (mw - ww) / 2 : 0),
                 mi.rcWork.top  + (mh > wh ? (mh - wh) / 2 : 0),
                 ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* Borderless / exclusive: cover the whole monitor the window is on.
 * rcMonitor is in physical pixels only when the process is per-monitor
 * aware; see the DPI notes at the top. */
static void fb_apply_borderless(HWND h)
{
    MONITORINFO mi;

    fb_monitor_info(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowLongPtrA(h, GWL_STYLE, FB_STYLE_BORDERLESS);
    SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* Window thread only. Remembers the mode so a DPI change can re-apply it. */
static void fb_apply_mode(HWND h, int mode, int w, int hh)
{
    g_fb.mode = mode;
    if (mode == FB_MODE_WINDOWED) {
        g_fb.client_w = w;
        g_fb.client_h = hh;
        fb_apply_windowed(h, w, hh);
    } else {
        fb_apply_borderless(h);
    }
}

void xbox_FramebufferApplyMode(int mode, int w, int h)
{
    HWND hw = g_fb.hwnd;
    if (hw)
        PostMessageA(hw, FB_WM_APPLY_MODE, (WPARAM)mode, MAKELPARAM(w, h));
}

/* ---------------------------------------------------------------------------
 * Window procedure
 * ------------------------------------------------------------------------- */

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_CLOSE:
        case WM_DESTROY:
            /* The game ends quietly here; say so, so a closed window is never
             * mistaken for a crash (or the other way round). The loop in
             * fb_thread sees running == 0, tears down and ends the process. */
            fprintf(stderr, "[WINDOW] %s received (foreground=%d) -- the game is closing\n",
                    m == WM_CLOSE ? "WM_CLOSE" : "WM_DESTROY", GetForegroundWindow() == h);
            fflush(stderr);
            InterlockedExchange(&g_fb.running, 0);
            return 0;

        case WM_SYSCOMMAND:
            if ((w & 0xFFF0) == SC_CLOSE) {
                fprintf(stderr, "[WINDOW] close from the title bar / Alt+F4\n");
                fflush(stderr);
            }
            break;      /* DefWindowProc still handles it (sends WM_CLOSE) */

        case FB_WM_APPLY_MODE:
            fb_apply_mode(h, (int)w, LOWORD(l), HIWORD(l));
            return 0;

        case WM_DPICHANGED:
            /* Moved to a monitor with different scaling. Borderless/fullscreen
             * must cover the new monitor exactly; windowed takes the rect
             * Windows suggests (same physical size at the new DPI). */
            if (g_fb.mode == FB_MODE_WINDOWED) {
                const RECT *s = (const RECT *)l;
                SetWindowPos(h, NULL, s->left, s->top,
                             s->right - s->left, s->bottom - s->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                fb_apply_borderless(h);
            }
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (g_fb.key_hook) {
                g_fb.key_hook((unsigned)w);
                /* F10 and Alt would otherwise open the system menu and freeze
                 * the picture until it closes. */
                if (w == VK_F10 || m == WM_SYSKEYDOWN)
                    return 0;
            }
            break;

        case WM_ERASEBKGND:
            if (g_fb.gpu_present)
                return 1;   /* the swap chain paints every pixel; avoid flicker */
                break;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ---------------------------------------------------------------------------
 * GDI path: guest framebuffer -> BGRA8 -> window
 * ------------------------------------------------------------------------- */

/* Convert one frame from guest memory into g_fb.rgb.
 * bpp comes from pitch / width. 32-bit X8R8G8B8 is already BGRA in memory.
 * 16-bit is treated as R5G6B5; X1R5G5B5 surfaces will show wrong colours
 * (no title tested so far uses one for its front buffer). */
static void fb_convert(const uint8_t *src, uint32_t pitch, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < g_fb.height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = g_fb.rgb + (size_t)y * g_fb.width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)g_fb.width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < g_fb.width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)g_fb.width * 4);
        }
    }
}

static void fb_bitmap_info(BITMAPINFO *bmi)
{
    memset(bmi, 0, sizeof *bmi);
    bmi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi->bmiHeader.biWidth       = (LONG)g_fb.width;
    bmi->bmiHeader.biHeight      = -(LONG)g_fb.height; /* negative = top-down */
    bmi->bmiHeader.biPlanes      = 1;
    bmi->bmiHeader.biBitCount    = 32;
    bmi->bmiHeader.biCompression = BI_RGB;
}

/* Apply a pending xbox_FramebufferWindowSize request: reallocate the copy
 * buffer and, in plain windowed GDI mode, resize the window to match.
 * NFL 2K5 renders to 720x480 with the picture centred; a fixed 640x480
 * view cut off the right 80 columns. */
static void fb_apply_pending_size(HWND hwnd, BITMAPINFO *bmi)
{
    uint32_t ww = (uint32_t)g_fb.want_w, wh = (uint32_t)g_fb.want_h;
    uint32_t pitch = g_fb.pitch ? g_fb.pitch : ww * 4u;
    uint32_t *nb;

    if (!ww || !wh || (ww == g_fb.width && wh == g_fb.height) || ww * 4u > pitch)
        return;
    nb = (uint32_t *)calloc((size_t)ww * wh, 4);
    if (!nb)
        return;

    AcquireSRWLockExclusive(&g_fb.rgb_lock);
    free(g_fb.rgb);
    g_fb.rgb    = nb;
    g_fb.width  = ww;
    g_fb.height = wh;
    ReleaseSRWLockExclusive(&g_fb.rgb_lock);
    fb_bitmap_info(bmi);

    if (!g_fb.ultrawide && !g_fb.gpu_present && g_fb.mode == FB_MODE_WINDOWED) {
        RECT r = { 0, 0, (LONG)ww, (LONG)wh };
        fb_adjust_rect(&r, FB_STYLE_WINDOWED, fb_window_dpi(hwnd));
        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* Read the guest framebuffer and draw it. A bad frame is skipped and the
 * last good one stays on screen; it is never fatal. */
static void fb_draw_gdi(HWND hwnd, HDC hdc, const BITMAPINFO *bmi)
{
    uint32_t va = g_fb.va, pitch = g_fb.pitch;
    uint32_t base;
    uint64_t end;
    const uint8_t *src;

    if (!va || !pitch || !g_fb.rgb || !g_fb.width || pitch < g_fb.width)
        return;

    base = va & 0x7FFFFFFFu;
    end  = (uint64_t)base + (uint64_t)pitch * g_fb.height;
    if (!(va >= 0x80000000u || va < FB_GUEST_RAM_SIZE) || end > FB_GUEST_RAM_SIZE)
        return;

    src = (const uint8_t *)((uintptr_t)va + xbox_GetMemoryOffset());

    __try {
        int dst_x = 0, dst_y = 0;
        int dst_w = (int)g_fb.width, dst_h = (int)g_fb.height;

        fb_convert(src, pitch, pitch / g_fb.width);

        if (g_fb.ultrawide) {
            /* Letterbox: fit the 4:3 picture inside the client area and
             * clear the bars. The guest's own surface is untouched; a real
             * wider field of view needs title-side projection changes. */
            RECT client;
            GetClientRect(hwnd, &client);
            if (client.right > 0 && client.bottom > 0) {
                double sx = (double)client.right  / g_fb.width;
                double sy = (double)client.bottom / g_fb.height;
                double s  = sx < sy ? sx : sy;
                dst_w = (int)(g_fb.width  * s + 0.5);
                dst_h = (int)(g_fb.height * s + 0.5);
                dst_x = (client.right  - dst_w) / 2;
                dst_y = (client.bottom - dst_h) / 2;
                PatBlt(hdc, 0, 0, client.right, client.bottom, BLACKNESS);
            }
        }

        SetStretchBltMode(hdc, HALFTONE);
        StretchDIBits(hdc, dst_x, dst_y, dst_w, dst_h,
                      0, 0, (int)g_fb.width, (int)g_fb.height,
                      g_fb.rgb, bmi, DIB_RGB_COLORS, SRCCOPY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Guest memory vanished under us; keep the last good frame. */
    }
}

/* ---------------------------------------------------------------------------
 * Window thread
 * ------------------------------------------------------------------------- */

static HWND fb_create_window(void)
{
    WNDCLASSA wc;
    RECT r;
    DWORD style = FB_STYLE_WINDOWED;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w, h;
    HWND hwnd;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = fb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1)); /* game icon, if any */
    wc.lpszClassName = FB_CLASS_NAME;
    RegisterClassA(&wc);

    /* Initial windowed size. The window does not exist yet, so use the
     * system DPI; WM_DPICHANGED corrects it if it opens on another monitor. */
    r.left = 0; r.top = 0; r.right = (LONG)g_fb.width; r.bottom = (LONG)g_fb.height;
    fb_adjust_rect(&r, style, fb_window_dpi(NULL));
    w = r.right - r.left;
    h = r.bottom - r.top;
    g_fb.mode     = FB_MODE_WINDOWED;
    g_fb.client_w = (int)g_fb.width;
    g_fb.client_h = (int)g_fb.height;

    if (g_fb.ultrawide) {
        /* Open borderless on whichever monitor the cursor is on. */
        POINT cursor = { 0, 0 };
        MONITORINFO mi;
        GetCursorPos(&cursor);
        fb_monitor_info(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top;
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        style = FB_STYLE_BORDERLESS;
        g_fb.mode = FB_MODE_BORDERLESS;
    }

    hwnd = CreateWindowExA(0, FB_CLASS_NAME, FB_TITLE, style, x, y, w, h,
                           NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        return NULL;

    /* This window takes no text input. Without this, DispatchMessage was
     * seen (cdb, 2026-09-21) wedging forever inside MSCTF/IMM32 on a
     * contended lock held by an injected input tool. */
    ImmAssociateContext(hwnd, NULL);
    return hwnd;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bmi;
    int dump_frames = 0;
    const char *dump_path = getenv("RECOMP_FB_DUMP");

    (void)unused;

    g_fb.ultrawide = getenv("RECOMP_ULTRAWIDE") && *getenv("RECOMP_ULTRAWIDE") != '0';
    fb_dpi_init();      /* before any window: see the DPI notes at the top */

    hwnd = fb_create_window();
    if (!hwnd) {
        InterlockedExchange(&g_fb.running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    if (!hdc) {
        DestroyWindow(hwnd);
        InterlockedExchange(&g_fb.running, 0);
        return 0;
    }
    g_fb.hwnd = hwnd;   /* published last: the presenter may use it now */

    AcquireSRWLockExclusive(&g_fb.rgb_lock);
    g_fb.rgb = (uint32_t *)calloc((size_t)g_fb.width * g_fb.height, 4);
    ReleaseSRWLockExclusive(&g_fb.rgb_lock);
    fb_bitmap_info(&bmi);

    fprintf(stderr, "  [FBWIN] window open (%ux%u surface, DPI %u)\n",
            g_fb.width, g_fb.height, fb_window_dpi(hwnd));

    while (InterlockedCompareExchange(&g_fb.running, 1, 1)) {
        MSG msg;

        /* No TranslateMessage: it can feed the same IME/MSCTF path that
         * ImmAssociateContext above avoids, and nothing here needs WM_CHAR. */
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);

        fb_apply_pending_size(hwnd, &bmi);

        if (!g_fb.gpu_present)
            fb_draw_gdi(hwnd, hdc, &bmi);

        /* One capture a second or so in, after the title has drawn
         * something. Only meaningful on the GDI path: with GPU present the
         * copy buffer is not updated. */
        if (dump_path && ++dump_frames == 60)
            xbox_FramebufferDumpBmp(dump_path);

        if (g_fb.gpu_present)
            MsgWaitForMultipleObjects(0, NULL, FALSE, 8, QS_ALLINPUT); /* just pump */
            else
                Sleep(16);                                                 /* ~60 Hz   */
    }

    g_fb.hwnd = NULL;
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    AcquireSRWLockExclusive(&g_fb.rgb_lock);
    free(g_fb.rgb);
    g_fb.rgb = NULL;
    ReleaseSRWLockExclusive(&g_fb.rgb_lock);

    /* After a guest crash the main thread may be parked forever so this
     * window stays up (forced-display path, see the game's src/main.c).
     * Closing the window is then the only way out, so make it end the run. */
    ExitProcess(0);
    return 0;
}

static void fb_window_start(void)
{
    HANDLE th;

    if (InterlockedCompareExchange(&g_fb.running, 1, 0) != 0)
        return;     /* already running */
        th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&g_fb.running, 0);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Start the window only when RECOMP_FB_WINDOW is set (generic titles). */
void xbox_FramebufferWindowStart(void)
{
    if (getenv("RECOMP_FB_WINDOW"))
        fb_window_start();
}

/* Start the window unconditionally (NFL 2K5's forced-display path,
 * nfl2k5_forced_display_thread in the game's src/main.c). */
void xbox_FramebufferWindowForceStart(void)
{
    fb_window_start();
}

/* The front buffer the title just set. RECOMP_FB_VA pins the view to a
 * fixed address instead, to tell "read path broken" from "title drew black". */
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    const char *pin = getenv("RECOMP_FB_VA");

    g_fb.va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        g_fb.pitch = pitch;
}

/* The surface's real size, when the executor knows it. Applied on the
 * window thread (fb_apply_pending_size), which owns the copy buffer. */
void xbox_FramebufferWindowSize(uint32_t w, uint32_t h)
{
    if (w >= 64 && w <= 2048 && h >= 64 && h <= 2048) {
        InterlockedExchange(&g_fb.want_w, (LONG)w);
        InterlockedExchange(&g_fb.want_h, (LONG)h);
    }
}

HWND     xbox_FramebufferWindowHwnd(void) { return g_fb.hwnd; }
uint32_t xbox_FramebufferWindowVa(void)   { return g_fb.va; }
void     xbox_FramebufferSetGpuPresent(int on) { InterlockedExchange(&g_fb.gpu_present, on ? 1 : 0); }
void     xbox_FramebufferSetKeyHook(void (*hook)(unsigned vk)) { g_fb.key_hook = hook; }

/* Write the GDI copy buffer to a 24-bit BMP. Safe from any thread. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint8_t hdr[54], *line;
    uint32_t row, img, total, x, y;
    int rc = -1;

    AcquireSRWLockShared(&g_fb.rgb_lock);
    if (!g_fb.rgb || !g_fb.va)
        goto out;

    row   = ((g_fb.width * 3u) + 3u) & ~3u;     /* rows pad to 4 bytes */
    img   = row * g_fb.height;
    total = 54u + img;

    f = fopen(path, "wb");
    if (!f)
        goto out;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2,  &total, 4);
    hdr[10] = 54;                               /* pixel data offset */
    hdr[14] = 40;                               /* BITMAPINFOHEADER  */
    memcpy(hdr + 18, &g_fb.width, 4);
    memcpy(hdr + 22, &g_fb.height, 4);          /* positive = bottom-up */
    hdr[26] = 1;                                /* planes            */
    hdr[28] = 24;                               /* bits per pixel    */
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof hdr, f);

    line = (uint8_t *)calloc(1, row);
    if (line) {
        for (y = 0; y < g_fb.height; y++) {
            const uint32_t *src = g_fb.rgb + (size_t)(g_fb.height - 1 - y) * g_fb.width;
            for (x = 0; x < g_fb.width; x++) {
                line[x * 3 + 0] = (uint8_t)( src[x]        & 0xFF);
                line[x * 3 + 1] = (uint8_t)((src[x] >>  8) & 0xFF);
                line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
            }
            fwrite(line, 1, row, f);
        }
        free(line);
        rc = 0;
    }
    fclose(f);
    if (rc == 0)
        fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
                path, g_fb.width, g_fb.height, g_fb.va);
        out:
        ReleaseSRWLockShared(&g_fb.rgb_lock);
    return rc;
}

#else /* !_WIN32: no window, every entry point is a no-op */

void    *xbox_FramebufferWindowHwnd(void) { return 0; }
uint32_t xbox_FramebufferWindowVa(void) { return 0; }
void     xbox_FramebufferSetGpuPresent(int on) { (void)on; }
void     xbox_FramebufferSetKeyHook(void (*hook)(unsigned vk)) { (void)hook; }
void     xbox_FramebufferApplyMode(int mode, int w, int h) { (void)mode; (void)w; (void)h; }
void     xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void     xbox_FramebufferWindowSize(uint32_t w, uint32_t h) { (void)w; (void)h; }
void     xbox_FramebufferWindowStart(void) {}
void     xbox_FramebufferWindowForceStart(void) {}
int      xbox_FramebufferDumpBmp(const char *path) { (void)path; return -1; }

#endif
