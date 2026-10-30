/* ── gx on non-Windows hosts (Android): what the renderer needs from Win32 ──
 *
 * XBOX_GX_PORTABLE builds the Direct3D 11 renderer (nv2a_gpu_d3d11.inc.c and
 * the files it includes) on SDL_GPU's Vulkan backend only. The NT types,
 * threads, events, timers and interlocked calls come from xboxrecomp's
 * platform layer (win32_compat); the Direct3D types from the headers in
 * gx/portable. This header covers the rest; gx_portable.c implements it. */
#ifndef GX_PORTABLE_H
#define GX_PORTABLE_H
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* __rdtsc: the draw-path profile's clock */
static inline uint64_t gx_portable_ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
#ifndef __rdtsc
#define __rdtsc() gx_portable_ticks()
#endif


/* Win32 calls the renderer makes that the platform layer does not have */
#define YieldProcessor() __asm__ __volatile__("yield")
BOOL CreateDirectoryA(LPCSTR path, void *sa);
typedef struct { DWORD dwNumberOfProcessors; } SYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO *si);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);   /* NULL: no per-thread cycle counts here */
BOOL QueryThreadCycleTime(HANDLE h, ULONG64 *cycles);
#ifndef THREAD_QUERY_LIMITED_INFORMATION
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#endif
typedef struct { DWORD dmSize, dmPelsWidth, dmPelsHeight, dmBitsPerPel, dmDisplayFrequency; } DEVMODEA;
BOOL EnumDisplaySettingsA(LPCSTR dev, DWORD mode, DEVMODEA *dm);      /* FALSE: SDL lists the modes */
#ifndef VK_F9
#define VK_F9 0x78
#endif
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
static inline int strncpy_s(char *d, size_t n, const char *s, size_t count)
{
    size_t i = 0;
    if (!d || !n) return 22;
    for (; s && s[i] && i + 1 < n && (count == _TRUNCATE || i < count); i++) d[i] = s[i];
    d[i] = 0;
    return 0;
}
#define _popen popen
#define _pclose pclose
SHORT GetKeyState(int vk);                     /* 0: no keyboard state here */
BOOL GetClientRect(HWND w, RECT *r);           /* the native (DXGI) path only, never taken */

#ifdef __cplusplus
}
#endif
#endif
