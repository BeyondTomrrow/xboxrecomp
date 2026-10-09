/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <d3d11.h>
#include <imm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted BGRA8 upload buffer */
/* The surface's real size, when the executor knows it. NFL 2K5 renders to a
 * 720x480 surface with the picture centred on it; a fixed 640x480 window cut
 * off the right 80 columns. Applied by the window thread, which owns s_rgb. */
static volatile LONG s_want_w, s_want_h;

/* Keep the title's 4:3 render targets intact on a modern ultrawide display.
 * This changes only host presentation: the guest still owns its surface size,
 * viewport and projection matrices. A real wider field of view needs separate
 * title-side projection work; stretching the existing surface would distort
 * every menu and HUD element. */
static int fb_ultrawide_on(void)
{
    const char *v = getenv("RECOMP_ULTRAWIDE");
    return v && *v && *v != '0';
}

void xbox_FramebufferWindowSize(uint32_t w, uint32_t h)
{
    if (w >= 64 && w <= 2048 && h >= 64 && h <= 2048) {
        InterlockedExchange(&s_want_w, (LONG)w);
        InterlockedExchange(&s_want_h, (LONG)h);
    }
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* ── GPU presentation hooks (2026-09-26) ─────────────────────────────────
 *
 * The D3D11 renderer can present straight from its render target through a
 * swap chain on this window (nv2a_gpu_present.inc.c). It then owns what the
 * window shows: the GDI blit below stops, and so does the per-frame
 * readback that fed it. This file keeps owning the window itself -- its
 * thread, message pump and style -- and offers the presenter the handle,
 * the displayed guest address, key presses (the settings page) and a way to
 * change the window mode on this thread. */
static volatile HWND s_fb_hwnd;
static volatile LONG s_fb_gpu_present;
static void (*volatile s_fb_key_hook)(unsigned vk);

#define FB_WM_APPLY_MODE (WM_APP + 1)

HWND xbox_FramebufferWindowHwnd(void) { return s_fb_hwnd; }
uint32_t xbox_FramebufferWindowVa(void) { return s_fb_va; }
void xbox_FramebufferSetGpuPresent(int on) { InterlockedExchange(&s_fb_gpu_present, on ? 1 : 0); }
void xbox_FramebufferSetKeyHook(void (*hook)(unsigned vk)) { s_fb_key_hook = hook; }

/* mode: 0 windowed (client w x h, centred), 1 borderless on the window's
 * monitor, 2 exclusive fullscreen (the swap chain switches; the window is
 * made borderless here). Runs on the window thread. */
static void fb_apply_mode(HWND h, int mode, int w, int hh)
{
    MONITORINFO mi;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(mon, &mi);
    if (mode == 0) {
        RECT r;
        int mw = mi.rcWork.right - mi.rcWork.left, mh = mi.rcWork.bottom - mi.rcWork.top, ww, wh;
        /* A client area that does not fit the work area is scaled down,
         * keeping its shape (the default is sized for a 1440p screen). */
        if (w > mw - 16 || hh > mh - 48) {
            double k = (double)(mw - 16) / w, k2 = (double)(mh - 48) / hh;
            if (k2 < k) k = k2;
            w = (int)(w * k); hh = (int)(hh * k);
        }
        SetWindowLongPtrA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        r.left = 0; r.top = 0; r.right = w; r.bottom = hh;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        ww = r.right - r.left; wh = r.bottom - r.top;
        SetWindowPos(h, HWND_NOTOPMOST, mi.rcWork.left + (mw > ww ? (mw - ww) / 2 : 0),
                     mi.rcWork.top + (mh > wh ? (mh - wh) / 2 : 0), ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

void xbox_FramebufferApplyMode(int mode, int w, int h)
{
    HWND hw = s_fb_hwnd;
    if (hw)
        PostMessageA(hw, FB_WM_APPLY_MODE, (WPARAM)mode, MAKELPARAM(w, h));
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || m == WM_DESTROY) {
        /* The game ends quietly here; say so, so a closed window is never
         * mistaken for a crash (or the other way round). */
        fprintf(stderr, "[WINDOW] %s received (foreground=%d) -- the game is closing\n",
                m == WM_CLOSE ? "WM_CLOSE" : "WM_DESTROY", GetForegroundWindow() == h);
        fflush(stderr);
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE) {
        fprintf(stderr, "[WINDOW] close from the title bar / Alt+F4\n");
        fflush(stderr);
    }
    if (m == FB_WM_APPLY_MODE) {
        fb_apply_mode(h, (int)w, LOWORD(l), HIWORD(l));
        return 0;
    }
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && s_fb_key_hook) {
        s_fb_key_hook((unsigned)w);
        if (w == VK_F10 || m == WM_SYSKEYDOWN)
            return 0;            /* F10/Alt would otherwise open the system menu */
    }
    if (m == WM_ERASEBKGND && s_fb_gpu_present)
        return 1;                /* the swap chain paints every pixel */
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    RECT r;
    HDC hdc;
    BITMAPINFO bmi;
    int ultrawide = fb_ultrawide_on();
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    int win_x = CW_USEDEFAULT, win_y = CW_USEDEFAULT;
    int win_w, win_h;

    (void)unused;

    /* Real pixels: without this Windows scales the window by the display
     * scaling (150% made a 1440x1080 window taller than a 1440p screen) and
     * the swap chain renders at the lower logical size. */
    {
        typedef BOOL (WINAPI *SetDpiCtx)(HANDLE);
        SetDpiCtx f = (SetDpiCtx)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (f) f((HANDLE)-4);          /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        else SetProcessDPIAware();
    }

    /* 2026-09-21: switched from D3D11 (device/swapchain creation was
     * observed hanging for 60+ seconds under this machine's GPU driver
     * stack, likely fighting an injected overlay/hook DLL -- confirmed via
     * a live thread stack showing it stuck inside nvldumdx's own
     * OpenAdapter12/LoadLibraryExW) to plain GDI StretchDIBits. No device,
     * no swapchain, no driver JIT -- just a blit into the window's own DC.
     * This is also what the file's own original design comment above
     * already called for ("Deliberately plain GDI rather than the D3D8
     * layer"); the D3D11 swapchain path never actually matched that. */
    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        /* The game's icon (resource 1 of the executable, if it has one). */
        wc.hIcon         = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
        wc.lpszClassName = "ESPN NFL 2K5";
        RegisterClassA(&wc);
    }
    r.left = 0; r.top = 0; r.right = (LONG)s_fb_width; r.bottom = (LONG)s_fb_height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    win_w = r.right - r.left;
    win_h = r.bottom - r.top;
    if (ultrawide) {
        POINT cursor = { 0, 0 };
        MONITORINFO mi;
        HMONITOR monitor;
        GetCursorPos(&cursor);
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(monitor, &mi)) {
            win_x = mi.rcMonitor.left;
            win_y = mi.rcMonitor.top;
            win_w = mi.rcMonitor.right - mi.rcMonitor.left;
            win_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
        style = WS_POPUP | WS_VISIBLE;
    }
    hwnd = CreateWindowExA(0, "ESPN NFL 2K5", "ESPN NFL 2K5",
                           style, win_x, win_y, win_w, win_h,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    /* 2026-09-21: this window has no text input at all -- disassociate it
     * from IME entirely. Confirmed live (cdb, non-invasive attach) that
     * without this, DispatchMessage can wedge indefinitely inside
     * MSCTF/IMM32 (ImmLockIMC -> RtlLockHeap -> a contended critical
     * section held by something else on this machine, likely a hooked/
     * injected input tool) -- a real, reproducible hang, not a fix for
     * anything in this project's own code. */
    ImmAssociateContext(hwnd, NULL);
    s_fb_hwnd = hwnd;
    hdc = GetDC(hwnd);
    if (!hdc) {
        DestroyWindow(hwnd);
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)s_fb_width;
    bmi.bmiHeader.biHeight = -(LONG)s_fb_height; /* negative: top-down, matches s_rgb's row order */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] GDI framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            /* No TranslateMessage: this window takes no text input, and
             * translating can itself feed the same IME/MSCTF path that
             * ImmAssociateContext(hwnd, NULL) above is trying to avoid. */
            DispatchMessageA(&msg);
        }
        {
            uint32_t ww = (uint32_t)s_want_w, wh = (uint32_t)s_want_h;
            if (ww && wh && (ww != s_fb_width || wh != s_fb_height)
                    && ww * 4u <= (s_fb_pitch ? s_fb_pitch : ww * 4u)) {
                uint32_t *nb = (uint32_t *)calloc((size_t)ww * wh, 4);
                if (nb) {
                    RECT wr;
                    free(s_rgb);
                    s_rgb = nb;
                    s_fb_width = ww;
                    s_fb_height = wh;
                    bmi.bmiHeader.biWidth = (LONG)ww;
                    bmi.bmiHeader.biHeight = -(LONG)wh;
                    if (!ultrawide && !s_fb_gpu_present) {
                        wr.left = 0; wr.top = 0; wr.right = (LONG)ww; wr.bottom = (LONG)wh;
                        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
                        SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                }
            }
        }
        /* Only copy from a surface that lies inside guest RAM (the 64 MB
         * contiguous window at 0x80000000, or its low-memory alias). Around
         * NFL 2K5's Start Game the title renders into off-screen targets, the
         * window followed one, and the copy read 0xFFB0AFAF and took the
         * whole process down. A bad frame is skipped, never fatal. */
        if (s_fb_gpu_present) {
            /* The GPU presenter owns the window's contents. */
        } else if (s_fb_va && s_fb_pitch && s_rgb && s_fb_width && s_fb_pitch >= s_fb_width) {
            uint32_t base = s_fb_va & 0x7FFFFFFFu;
            uint64_t end = (uint64_t)base + (uint64_t)s_fb_pitch * s_fb_height;
            if ((s_fb_va >= 0x80000000u || s_fb_va < 0x04000000u) && end <= 0x04000000u) {
                const uint8_t *src =
                    (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
                __try {
                    fb_convert(src, s_fb_pitch / s_fb_width);
                    int dst_x = 0, dst_y = 0;
                    int dst_w = (int)s_fb_width, dst_h = (int)s_fb_height;
                    if (ultrawide) {
                        RECT client;
                        GetClientRect(hwnd, &client);
                        dst_w = client.right - client.left;
                        dst_h = client.bottom - client.top;
                        if (dst_w > 0 && dst_h > 0) {
                            double scale_x = (double)dst_w / s_fb_width;
                            double scale_y = (double)dst_h / s_fb_height;
                            double scale = scale_x < scale_y ? scale_x : scale_y;
                            dst_w = (int)(s_fb_width * scale + 0.5);
                            dst_h = (int)(s_fb_height * scale + 0.5);
                            dst_x = (client.right - dst_w) / 2;
                            dst_y = (client.bottom - dst_h) / 2;
                            PatBlt(hdc, 0, 0, client.right, client.bottom, BLACKNESS);
                        }
                    }
                    SetStretchBltMode(hdc, HALFTONE);
                    StretchDIBits(hdc, dst_x, dst_y, dst_w, dst_h,
                                  0, 0, (int)s_fb_width, (int)s_fb_height,
                                  s_rgb, &bmi, DIB_RGB_COLORS, SRCCOPY);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    /* keep the last good frame on screen */
                }
            }
        }
        {
            /* One dump, a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            static int frames;
            /* Bring-up captures need to happen before the short watchdog run
             * ends; a longer production run still gets the same first frame. */
            if (dump && ++frames == 60)
                xbox_FramebufferDumpBmp(dump);
        }
        if (s_fb_gpu_present)
            MsgWaitForMultipleObjects(0, NULL, FALSE, 8, QS_ALLINPUT);   /* just pump */
        else
            Sleep(16);
    }

    s_fb_hwnd = NULL;
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    /* Forced-display path: the main thread may be parked in an infinite
     * sleep after a guest crash (see src/main.c) specifically so this
     * window survives independently of guest logic. Closing the window is
     * then the only way to end the run -- make it actually do that. */
    ExitProcess(0);
    return 0;
}

static void fb_window_start_unconditional(void)
{
    HANDLE th;

    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

void xbox_FramebufferWindowStart(void)
{
    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    fb_window_start_unconditional();
}

/* 2026-09-21: forced-display path (user directive: "force it, I don't
 * care") -- starts the window unconditionally, no env var, no CRT/module
 * boundary to cross. See nfl2k5_forced_display_thread in src/main.c. */
void xbox_FramebufferWindowForceStart(void)
{
    fb_window_start_unconditional();
}

#else
void *xbox_FramebufferWindowHwnd(void) { return 0; }
uint32_t xbox_FramebufferWindowVa(void) { return 0; }
void xbox_FramebufferSetGpuPresent(int on) { (void)on; }
void xbox_FramebufferSetKeyHook(void (*hook)(unsigned vk)) { (void)hook; }
void xbox_FramebufferApplyMode(int mode, int w, int h) { (void)mode; (void)w; (void)h; }
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowSize(uint32_t w, uint32_t h) { (void)w; (void)h; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowForceStart(void) {}
#endif
