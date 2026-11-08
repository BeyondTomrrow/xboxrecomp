/* gx_portable.h's counterpart: the Windows-only entry points the renderer
 * names, for hosts where only SDL_GPU exists (XBOX_GX_PORTABLE). */
#if defined(XBOX_GX_PORTABLE) && !defined(_WIN32)
#include "platform/xbox_winnt.h"
#define GX_IMPL 1                 /* the real D3D11CreateDevice / D3DCompile names */
#include "gx_d3d11.h"
#include "gx_portable.h"
#include "gx_ffx.h"
#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

__attribute__((weak)) BOOL CreateDirectoryA(LPCSTR path, void *sa)   /* the game's own compat may have one */
{
    char p[1024];
    size_t i;
    (void)sa;
    for (i = 0; path && path[i] && i < sizeof p - 1; i++) p[i] = path[i] == 92 ? '/' : path[i];
    p[i] = 0;
    return mkdir(p, 0755) == 0;
}

void GetSystemInfo(SYSTEM_INFO *si) { long n = sysconf(_SC_NPROCESSORS_ONLN); si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1; }
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid) { (void)access; (void)inherit; (void)tid; return NULL; }
BOOL QueryThreadCycleTime(HANDLE h, ULONG64 *cycles) { (void)h; if (cycles) *cycles = 0; return FALSE; }
SHORT GetKeyState(int vk) { (void)vk; return 0; }
BOOL GetClientRect(HWND w, RECT *r) { (void)w; if (r) memset(r, 0, sizeof *r); return FALSE; }
BOOL EnumDisplaySettingsA(LPCSTR dev, DWORD mode, DEVMODEA *dm) { (void)dev; (void)mode; (void)dm; return FALSE; }

/* gx_sdl.c falls through to the native Direct3D 11 calls when SDL_GPU is
 * not chosen; here it always is, so these only report failure. */
HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
                                 const D3D_FEATURE_LEVEL *fls, UINT nfl, UINT sdk, ID3D11Device **dev,
                                 D3D_FEATURE_LEVEL *fl, ID3D11DeviceContext **ctx)
{
    (void)ad; (void)dt; (void)sw; (void)flags; (void)fls; (void)nfl; (void)sdk; (void)fl;
    if (dev) *dev = NULL;
    if (ctx) *ctx = NULL;
    return E_FAIL;
}

HRESULT WINAPI D3DCompile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defs, ID3DInclude *inc,
                          LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **err)
{
    (void)src; (void)len; (void)name; (void)defs; (void)inc; (void)entry; (void)target; (void)flags1; (void)flags2;
    if (code) *code = NULL;
    if (err) *err = NULL;
    return E_FAIL;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **f) { (void)riid; if (f) *f = NULL; return E_FAIL; }

/* The patched SDL's Direct3D 12 command-list access (FSR 3): no Direct3D 12 here. */
void *SDL_GetGPUCommandBufferNativeD3D12(SDL_GPUCommandBuffer *cb) { (void)cb; return NULL; }
void SDL_RestoreGPUCommandBufferD3D12(SDL_GPUCommandBuffer *cb) { (void)cb; }

/* gx_ffx.cpp (FSR 3, Direct3D 12 only) */
int gxf_init(SDL_GPUDevice *device) { (void)device; return 0; }
void gxf_jitter(uint32_t rw, uint32_t ow, float *jx, float *jy) { (void)rw; (void)ow; *jx = *jy = 0.0f; }
int gxf_upscale(SDL_GPUTexture *color, SDL_GPUTexture *depth, SDL_GPUTexture *mv, SDL_GPUTexture *reactive,
                SDL_GPUTexture *out, SDL_GPUTexture *interp, void *native_list,
                uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, float jx, float jy,
                float dt_ms, int reset, float sharpness)
{
    (void)color; (void)depth; (void)mv; (void)reactive; (void)out; (void)interp; (void)native_list;
    (void)rw; (void)rh; (void)ow; (void)oh; (void)jx; (void)jy; (void)dt_ms; (void)reset; (void)sharpness;
    return 0;
}
#endif
