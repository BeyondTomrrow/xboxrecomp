/**
 * d3d8_sdl_gpu.c - Cross-Platform SDL3 GPU Backend & Runtime Dispatcher
 * =============================================================================
 * Implements IDirect3D8 / IDirect3DDevice8 over SDL3's SDL_GPU API
 * (Vulkan on Linux/Windows, Metal on macOS, D3D12 on Windows).
 *
 * Runtime selection via environment variable:
 *   RECOMP_RENDERER=d3d11    (Default on Windows: uses d3d8_device.c)
 *   RECOMP_RENDERER=sdl_gpu  (Default on Linux/macOS, opt-in on Windows)
 */

#include "d3d8_xbox.h"
#include "d3d8_fvf.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef D3D_OK
#define D3D_OK ((HRESULT)0)
#endif
#ifndef D3DERR_INVALIDCALL
#define D3DERR_INVALIDCALL ((HRESULT)0x8876086CL)
#endif

extern void *xbox_FramebufferWindowHwnd(void);
extern void  xbox_FramebufferSetGpuPresent(int on);

#define MAX_RS  256
#define MAX_TSS 33
#define MAX_TSU 4

typedef struct {
    SDL_Window    *window;
    bool           owns_window;
    SDL_GPUDevice *gpu;
    int            backbuf_w;
    int            backbuf_h;

    /* Clear state tracked between Clear() and Present() */
    SDL_FColor     clear_color;
    bool           has_clear;

    /* D3D8 state cache */
    DWORD          rs[MAX_RS];
    DWORD          tss[MAX_TSU][MAX_TSS];
    D3DMATRIX      transforms[512];
    D3DVIEWPORT8   viewport;
    D3DMATERIAL8   material;
    D3DLIGHT8      lights[8];
    BOOL           light_enabled[8];

    IDirect3DBaseTexture8  *textures[MAX_TSU];
    IDirect3DVertexBuffer8 *vb;
    UINT                    vb_stride;
    IDirect3DIndexBuffer8  *ib;
    UINT                    ib_base;
    DWORD                   current_fvf;
    DWORD                   current_vsh;
    DWORD                   current_psh;
} D3DSdlGpuState;

static D3DSdlGpuState g_sdl;
static IDirect3DDevice8 g_sdl_device;
static BOOL g_sdl_initialized = FALSE;

static const char *g_window_title = "ESPN NFL 2K5";
void xbox_D3D8SetWindowTitle(const char *title)
{
    g_window_title = (title && title[0]) ? title : "ESPN NFL 2K5";
}

/* ========================================================================
 * Resource Wrappers (Vertex Buffer, Index Buffer, Texture, Surface)
 * ======================================================================== */

typedef struct {
    IDirect3DVertexBuffer8 iface;
    SDL_AtomicInt          ref;
    SDL_GPUBuffer         *gpu_buf;
    UINT                   size;
    DWORD                  fvf;
    DWORD                  usage;
    BYTE                  *sys_mem;
    BOOL                   dirty;
} SdlGpuVertexBuffer;

typedef struct {
    IDirect3DIndexBuffer8 iface;
    SDL_AtomicInt         ref;
    SDL_GPUBuffer        *gpu_buf;
    UINT                  size;
    D3DFORMAT             format;
    BYTE                 *sys_mem;
    BOOL                  dirty;
} SdlGpuIndexBuffer;

typedef struct {
    IDirect3DTexture8 iface;
    SDL_AtomicInt     ref;
    SDL_GPUTexture   *gpu_tex;
    UINT              width, height, levels;
    D3DFORMAT         format;
    BYTE             *sys_mem;
    UINT              pitch;
    BOOL              dirty;
} SdlGpuTexture;

typedef struct {
    IDirect3DSurface8 iface;
    SDL_AtomicInt     ref;
    UINT              width, height;
    D3DFORMAT         format;
    BYTE             *sys_mem;
    UINT              pitch;
} SdlGpuSurface;

/* ---- Vertex Buffer Vtable ---- */
static HRESULT __stdcall svb_QueryInterface(IDirect3DVertexBuffer8 *s, const IID *i, void **p) { (void)s;(void)i;(void)p; return E_NOINTERFACE; }
static ULONG   __stdcall svb_AddRef(IDirect3DVertexBuffer8 *s) { return (ULONG)(SDL_AddAtomicInt(&((SdlGpuVertexBuffer *)s)->ref, 1) + 1); }
static ULONG   __stdcall svb_Release(IDirect3DVertexBuffer8 *s)
{
    SdlGpuVertexBuffer *v = (SdlGpuVertexBuffer *)s;
    int r = SDL_AddAtomicInt(&v->ref, -1) - 1;
    if (r <= 0) {
        if (v->gpu_buf && g_sdl.gpu) SDL_ReleaseGPUBuffer(g_sdl.gpu, v->gpu_buf);
        free(v->sys_mem);
        free(v);
    }
    return (ULONG)(r < 0 ? 0 : r);
}
static HRESULT __stdcall svb_GetDevice(IDirect3DVertexBuffer8 *s, IDirect3DDevice8 **pp) { (void)s; if (pp) *pp = &g_sdl_device; return D3D_OK; }
static DWORD   __stdcall svb_SetPriority(IDirect3DVertexBuffer8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD   __stdcall svb_GetPriority(IDirect3DVertexBuffer8 *s) { (void)s; return 0; }
static void    __stdcall svb_PreLoad(IDirect3DVertexBuffer8 *s) { (void)s; }
static DWORD   __stdcall svb_GetType(IDirect3DVertexBuffer8 *s) { (void)s; return D3DRTYPE_VERTEXBUFFER; }
static HRESULT __stdcall svb_Lock(IDirect3DVertexBuffer8 *s, UINT off, UINT sz, BYTE **pp, DWORD f)
{
    SdlGpuVertexBuffer *v = (SdlGpuVertexBuffer *)s;
    (void)sz; (void)f;
    if (!pp || !v->sys_mem) return D3DERR_INVALIDCALL;
    *pp = v->sys_mem + off;
    v->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall svb_Unlock(IDirect3DVertexBuffer8 *s) { (void)s; return D3D_OK; }
static HRESULT __stdcall svb_GetDesc(IDirect3DVertexBuffer8 *s, void *p) { (void)s;(void)p; return D3D_OK; }

static const IDirect3DVertexBuffer8Vtbl g_svb_vtbl = {
    svb_QueryInterface, svb_AddRef, svb_Release,
    svb_GetDevice, svb_SetPriority, svb_GetPriority, svb_PreLoad, svb_GetType,
    svb_Lock, svb_Unlock, svb_GetDesc
};

/* ---- Index Buffer Vtable ---- */
static HRESULT __stdcall sib_QueryInterface(IDirect3DIndexBuffer8 *s, const IID *i, void **p) { (void)s;(void)i;(void)p; return E_NOINTERFACE; }
static ULONG   __stdcall sib_AddRef(IDirect3DIndexBuffer8 *s) { return (ULONG)(SDL_AddAtomicInt(&((SdlGpuIndexBuffer *)s)->ref, 1) + 1); }
static ULONG   __stdcall sib_Release(IDirect3DIndexBuffer8 *s)
{
    SdlGpuIndexBuffer *v = (SdlGpuIndexBuffer *)s;
    int r = SDL_AddAtomicInt(&v->ref, -1) - 1;
    if (r <= 0) {
        if (v->gpu_buf && g_sdl.gpu) SDL_ReleaseGPUBuffer(g_sdl.gpu, v->gpu_buf);
        free(v->sys_mem);
        free(v);
    }
    return (ULONG)(r < 0 ? 0 : r);
}
static HRESULT __stdcall sib_GetDevice(IDirect3DIndexBuffer8 *s, IDirect3DDevice8 **pp) { (void)s; if (pp) *pp = &g_sdl_device; return D3D_OK; }
static DWORD   __stdcall sib_SetPriority(IDirect3DIndexBuffer8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD   __stdcall sib_GetPriority(IDirect3DIndexBuffer8 *s) { (void)s; return 0; }
static void    __stdcall sib_PreLoad(IDirect3DIndexBuffer8 *s) { (void)s; }
static DWORD   __stdcall sib_GetType(IDirect3DIndexBuffer8 *s) { (void)s; return D3DRTYPE_INDEXBUFFER; }
static HRESULT __stdcall sib_Lock(IDirect3DIndexBuffer8 *s, UINT off, UINT sz, BYTE **pp, DWORD f)
{
    SdlGpuIndexBuffer *v = (SdlGpuIndexBuffer *)s;
    (void)sz; (void)f;
    if (!pp || !v->sys_mem) return D3DERR_INVALIDCALL;
    *pp = v->sys_mem + off;
    v->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall sib_Unlock(IDirect3DIndexBuffer8 *s) { (void)s; return D3D_OK; }
static HRESULT __stdcall sib_GetDesc(IDirect3DIndexBuffer8 *s, void *p) { (void)s;(void)p; return D3D_OK; }

static const IDirect3DIndexBuffer8Vtbl g_sib_vtbl = {
    sib_QueryInterface, sib_AddRef, sib_Release,
    sib_GetDevice, sib_SetPriority, sib_GetPriority, sib_PreLoad, sib_GetType,
    sib_Lock, sib_Unlock, sib_GetDesc
};

/* ---- Texture Vtable ---- */
static HRESULT __stdcall stex_QueryInterface(IDirect3DTexture8 *s, const IID *i, void **p) { (void)s;(void)i;(void)p; return E_NOINTERFACE; }
static ULONG   __stdcall stex_AddRef(IDirect3DTexture8 *s) { return (ULONG)(SDL_AddAtomicInt(&((SdlGpuTexture *)s)->ref, 1) + 1); }
static ULONG   __stdcall stex_Release(IDirect3DTexture8 *s)
{
    SdlGpuTexture *t = (SdlGpuTexture *)s;
    int r = SDL_AddAtomicInt(&t->ref, -1) - 1;
    if (r <= 0) {
        if (t->gpu_tex && g_sdl.gpu) SDL_ReleaseGPUTexture(g_sdl.gpu, t->gpu_tex);
        free(t->sys_mem);
        free(t);
    }
    return (ULONG)(r < 0 ? 0 : r);
}
static HRESULT __stdcall stex_GetDevice(IDirect3DTexture8 *s, IDirect3DDevice8 **pp) { (void)s; if (pp) *pp = &g_sdl_device; return D3D_OK; }
static DWORD   __stdcall stex_SetPriority(IDirect3DTexture8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD   __stdcall stex_GetPriority(IDirect3DTexture8 *s) { (void)s; return 0; }
static void    __stdcall stex_PreLoad(IDirect3DTexture8 *s) { (void)s; }
static DWORD   __stdcall stex_GetType(IDirect3DTexture8 *s) { (void)s; return D3DRTYPE_TEXTURE; }
static DWORD   __stdcall stex_GetLevelCount(IDirect3DTexture8 *s) { return ((SdlGpuTexture *)s)->levels; }
static HRESULT __stdcall stex_GetLevelDesc(IDirect3DTexture8 *s, UINT lvl, D3DSURFACE_DESC *pD)
{
    SdlGpuTexture *t = (SdlGpuTexture *)s;
    if (!pD || lvl >= t->levels) return D3DERR_INVALIDCALL;
    memset(pD, 0, sizeof(*pD));
    pD->Format = t->format;
    pD->Width  = (t->width >> lvl) ? (t->width >> lvl) : 1;
    pD->Height = (t->height >> lvl) ? (t->height >> lvl) : 1;
    return D3D_OK;
}
static HRESULT __stdcall stex_GetSurfaceLevel(IDirect3DTexture8 *s, UINT lvl, IDirect3DSurface8 **pp) { (void)s;(void)lvl; if (pp) *pp = NULL; return D3D_OK; }
static HRESULT __stdcall stex_LockRect(IDirect3DTexture8 *s, UINT lvl, D3DLOCKED_RECT *pLR, const RECT *r, DWORD f)
{
    SdlGpuTexture *t = (SdlGpuTexture *)s;
    (void)lvl; (void)r; (void)f;
    if (!pLR || !t->sys_mem) return D3DERR_INVALIDCALL;
    pLR->Pitch = (INT)t->pitch;
    pLR->pBits = t->sys_mem;
    t->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall stex_UnlockRect(IDirect3DTexture8 *s, UINT lvl) { (void)s;(void)lvl; return D3D_OK; }

static const IDirect3DTexture8Vtbl g_stex_vtbl = {
    stex_QueryInterface, stex_AddRef, stex_Release,
    stex_GetDevice, stex_SetPriority, stex_GetPriority, stex_PreLoad, stex_GetType,
    stex_GetLevelCount, stex_GetLevelDesc, stex_GetSurfaceLevel, stex_LockRect, stex_UnlockRect
};

/* ---- Surface Vtable ---- */
static HRESULT __stdcall ssf_QueryInterface(IDirect3DSurface8 *s, const IID *i, void **p) { (void)s;(void)i;(void)p; return E_NOINTERFACE; }
static ULONG   __stdcall ssf_AddRef(IDirect3DSurface8 *s) { return (ULONG)(SDL_AddAtomicInt(&((SdlGpuSurface *)s)->ref, 1) + 1); }
static ULONG   __stdcall ssf_Release(IDirect3DSurface8 *s)
{
    SdlGpuSurface *sf = (SdlGpuSurface *)s;
    int r = SDL_AddAtomicInt(&sf->ref, -1) - 1;
    if (r <= 0) { free(sf->sys_mem); free(sf); }
    return (ULONG)(r < 0 ? 0 : r);
}
static HRESULT __stdcall ssf_GetDevice(IDirect3DSurface8 *s, IDirect3DDevice8 **pp) { (void)s; if (pp) *pp = &g_sdl_device; return D3D_OK; }
static HRESULT __stdcall ssf_GetDesc(IDirect3DSurface8 *s, D3DSURFACE_DESC *pD)
{
    SdlGpuSurface *sf = (SdlGpuSurface *)s;
    if (!pD) return D3DERR_INVALIDCALL;
    memset(pD, 0, sizeof(*pD));
    pD->Format = sf->format; pD->Width = sf->width; pD->Height = sf->height;
    return D3D_OK;
}
static HRESULT __stdcall ssf_LockRect(IDirect3DSurface8 *s, D3DLOCKED_RECT *pL, const RECT *r, DWORD f)
{
    SdlGpuSurface *sf = (SdlGpuSurface *)s;
    (void)r; (void)f;
    if (!pL || !sf->sys_mem) return D3DERR_INVALIDCALL;
    pL->Pitch = (INT)sf->pitch;
    pL->pBits = sf->sys_mem;
    return D3D_OK;
}
static HRESULT __stdcall ssf_UnlockRect(IDirect3DSurface8 *s) { (void)s; return D3D_OK; }

static const IDirect3DSurface8Vtbl g_ssf_vtbl = {
    ssf_QueryInterface, ssf_AddRef, ssf_Release,
    ssf_GetDevice, ssf_GetDesc, ssf_LockRect, ssf_UnlockRect
};

/* ========================================================================
 * IDirect3DDevice8 (SDL_GPU implementation)
 * ======================================================================== */

static HRESULT __stdcall sdev_QueryInterface(IDirect3DDevice8 *s, const IID *i, void **p) { (void)s;(void)i;(void)p; return E_NOINTERFACE; }
static ULONG   __stdcall sdev_AddRef(IDirect3DDevice8 *s) { (void)s; return 1; }
static ULONG   __stdcall sdev_Release(IDirect3DDevice8 *s)
{
    (void)s;
    if (g_sdl.gpu) {
        if (g_sdl.window) SDL_ReleaseWindowFromGPUDevice(g_sdl.gpu, g_sdl.window);
        SDL_DestroyGPUDevice(g_sdl.gpu);
        g_sdl.gpu = NULL;
    }
    if (g_sdl.owns_window && g_sdl.window) {
        SDL_DestroyWindow(g_sdl.window);
    }
    g_sdl.window = NULL;
    g_sdl_initialized = FALSE;
    return 0;
}
static HRESULT __stdcall sdev_GetDirect3D(IDirect3DDevice8 *s, IDirect3D8 **pp) { (void)s;(void)pp; return D3D_OK; }
static HRESULT __stdcall sdev_GetDeviceCaps(IDirect3DDevice8 *s, void *p) { (void)s;(void)p; return D3D_OK; }
static HRESULT __stdcall sdev_GetDisplayMode(IDirect3DDevice8 *s, void *p) { (void)s;(void)p; return D3D_OK; }
static HRESULT __stdcall sdev_GetCreationParameters(IDirect3DDevice8 *s, void *p) { (void)s;(void)p; return D3D_OK; }
static HRESULT __stdcall sdev_Reset(IDirect3DDevice8 *s, D3DPRESENT_PARAMETERS *p) { (void)s;(void)p; return D3D_OK; }

static HRESULT __stdcall sdev_Present(IDirect3DDevice8 *s, const RECT *src, const RECT *dst, HWND h, void *dirty)
{
    (void)s; (void)src; (void)dst; (void)h; (void)dirty;
    if (!g_sdl.gpu || !g_sdl.window) return D3D_OK;

    if (g_sdl.owns_window) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) exit(0);
        }
    }

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_sdl.gpu);
    if (!cmd) return D3D_OK;

    SDL_GPUTexture *swapchain_tex = NULL;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_sdl.window, &swapchain_tex, NULL, NULL) && swapchain_tex) {
        SDL_GPUColorTargetInfo color_target;
        SDL_zero(color_target);
        color_target.texture     = swapchain_tex;
        color_target.clear_color = g_sdl.clear_color;
        color_target.load_op     = g_sdl.has_clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        color_target.store_op    = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
        if (pass) {
            SDL_EndGPURenderPass(pass);
        }
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    g_sdl.has_clear = false;
    return D3D_OK;
}

static HRESULT __stdcall sdev_GetBackBuffer(IDirect3DDevice8 *s, INT i, DWORD t, IDirect3DSurface8 **pp)
{
    (void)s; (void)i; (void)t;
    if (!pp) return D3DERR_INVALIDCALL;
    SdlGpuSurface *sf = (SdlGpuSurface *)calloc(1, sizeof(*sf));
    if (!sf) return E_OUTOFMEMORY;
    sf->iface.lpVtbl = &g_ssf_vtbl;
    SDL_SetAtomicInt(&sf->ref, 1);
    sf->width  = (UINT)g_sdl.backbuf_w;
    sf->height = (UINT)g_sdl.backbuf_h;
    sf->format = D3DFMT_A8R8G8B8;
    sf->pitch  = sf->width * 4;
    sf->sys_mem = (BYTE *)calloc(1, (size_t)sf->pitch * sf->height);
    *pp = &sf->iface;
    return D3D_OK;
}

static HRESULT __stdcall sdev_BeginScene(IDirect3DDevice8 *s) { (void)s; return D3D_OK; }
static HRESULT __stdcall sdev_EndScene(IDirect3DDevice8 *s)   { (void)s; return D3D_OK; }

static HRESULT __stdcall sdev_Clear(IDirect3DDevice8 *s, DWORD c, const D3DRECT *r, DWORD flags, D3DCOLOR col, float z, DWORD st)
{
    (void)s; (void)c; (void)r; (void)z; (void)st;
    if (flags & D3DCLEAR_TARGET) {
        g_sdl.clear_color.r = ((col >> 16) & 0xFF) / 255.0f;
        g_sdl.clear_color.g = ((col >>  8) & 0xFF) / 255.0f;
        g_sdl.clear_color.b = ((col >>  0) & 0xFF) / 255.0f;
        g_sdl.clear_color.a = ((col >> 24) & 0xFF) / 255.0f;
        g_sdl.has_clear = true;
    }
    return D3D_OK;
}

static HRESULT __stdcall sdev_SetTransform(IDirect3DDevice8 *s, D3DTRANSFORMSTATETYPE st, const D3DMATRIX *m)
{
    (void)s;
    if ((DWORD)st < 512 && m) g_sdl.transforms[(DWORD)st] = *m;
    return D3D_OK;
}
static HRESULT __stdcall sdev_GetTransform(IDirect3DDevice8 *s, D3DTRANSFORMSTATETYPE st, D3DMATRIX *m)
{
    (void)s;
    if ((DWORD)st < 512 && m) *m = g_sdl.transforms[(DWORD)st];
    return D3D_OK;
}
static HRESULT __stdcall sdev_SetRenderState(IDirect3DDevice8 *s, D3DRENDERSTATETYPE st, DWORD v)
{
    (void)s;
    if ((DWORD)st < MAX_RS) g_sdl.rs[(DWORD)st] = v;
    return D3D_OK;
}
static HRESULT __stdcall sdev_GetRenderState(IDirect3DDevice8 *s, D3DRENDERSTATETYPE st, DWORD *pv)
{
    (void)s;
    if ((DWORD)st < MAX_RS && pv) *pv = g_sdl.rs[(DWORD)st];
    return D3D_OK;
}
static HRESULT __stdcall sdev_SetTextureStageState(IDirect3DDevice8 *s, DWORD st, D3DTEXTURESTAGESTATETYPE ty, DWORD v)
{
    (void)s;
    if (st < MAX_TSU && (DWORD)ty < MAX_TSS) g_sdl.tss[st][(DWORD)ty] = v;
    return D3D_OK;
}
static HRESULT __stdcall sdev_GetTextureStageState(IDirect3DDevice8 *s, DWORD st, D3DTEXTURESTAGESTATETYPE ty, DWORD *pv)
{
    (void)s;
    if (st < MAX_TSU && (DWORD)ty < MAX_TSS && pv) *pv = g_sdl.tss[st][(DWORD)ty];
    return D3D_OK;
}
static HRESULT __stdcall sdev_SetTexture(IDirect3DDevice8 *s, DWORD st, IDirect3DBaseTexture8 *t)
{
    (void)s;
    if (st < MAX_TSU) g_sdl.textures[st] = t;
    return D3D_OK;
}
static HRESULT __stdcall sdev_GetTexture(IDirect3DDevice8 *s, DWORD st, IDirect3DBaseTexture8 **pp)
{
    (void)s;
    if (!pp || st >= MAX_TSU) return D3DERR_INVALIDCALL;
    *pp = g_sdl.textures[st];
    return D3D_OK;
}
static HRESULT __stdcall sdev_SetStreamSource(IDirect3DDevice8 *s, UINT n, IDirect3DVertexBuffer8 *vb, UINT st)
{
    (void)s;
    if (n == 0) { g_sdl.vb = vb; g_sdl.vb_stride = st; }
    return D3D_OK;
}
static HRESULT __stdcall sdev_GetStreamSource(IDirect3DDevice8 *s, UINT n, IDirect3DVertexBuffer8 **pp, UINT *st)
{
    (void)s;
    if (n != 0 || !pp) return D3DERR_INVALIDCALL;
    *pp = g_sdl.vb; if (st) *st = g_sdl.vb_stride;
    return D3D_OK;
}
static HRESULT __stdcall sdev_SetIndices(IDirect3DDevice8 *s, IDirect3DIndexBuffer8 *ib, UINT base)
{
    (void)s; g_sdl.ib = ib; g_sdl.ib_base = base; return D3D_OK;
}
static HRESULT __stdcall sdev_GetIndices(IDirect3DDevice8 *s, IDirect3DIndexBuffer8 **pp, UINT *base)
{
    (void)s; if (pp) *pp = g_sdl.ib; if (base) *base = g_sdl.ib_base; return D3D_OK;
}
static HRESULT __stdcall sdev_DrawPrimitive(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt, UINT start, UINT cnt) { (void)s;(void)pt;(void)start;(void)cnt; return D3D_OK; }
static HRESULT __stdcall sdev_DrawIndexedPrimitive(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt, UINT mv, UINT nv, UINT si, UINT cnt) { (void)s;(void)pt;(void)mv;(void)nv;(void)si;(void)cnt; return D3D_OK; }
static HRESULT __stdcall sdev_DrawPrimitiveUP(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt, UINT cnt, const void *v, UINT st) { (void)s;(void)pt;(void)cnt;(void)v;(void)st; return D3D_OK; }
static HRESULT __stdcall sdev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt, UINT mv, UINT nv, UINT cnt, const void *idx, D3DFORMAT fmt, const void *v, UINT st) { (void)s;(void)pt;(void)mv;(void)nv;(void)cnt;(void)idx;(void)fmt;(void)v;(void)st; return D3D_OK; }

static HRESULT __stdcall sdev_CreateTexture(IDirect3DDevice8 *s, UINT w, UINT h, UINT lvls, DWORD u, D3DFORMAT fmt, D3DPOOL p, IDirect3DTexture8 **pp)
{
    (void)s; (void)u; (void)p;
    if (!pp || !w || !h) return D3DERR_INVALIDCALL;
    SdlGpuTexture *t = (SdlGpuTexture *)calloc(1, sizeof(*t));
    if (!t) return E_OUTOFMEMORY;
    t->iface.lpVtbl = &g_stex_vtbl;
    SDL_SetAtomicInt(&t->ref, 1);
    t->width = w; t->height = h; t->levels = lvls ? lvls : 1; t->format = fmt;
    t->pitch = w * 4;
    t->sys_mem = (BYTE *)calloc(1, (size_t)t->pitch * h);
    *pp = &t->iface;
    return D3D_OK;
}
static HRESULT __stdcall sdev_CreateVertexBuffer(IDirect3DDevice8 *s, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer8 **pp)
{
    (void)s; (void)p;
    if (!pp) return D3DERR_INVALIDCALL;
    SdlGpuVertexBuffer *v = (SdlGpuVertexBuffer *)calloc(1, sizeof(*v));
    if (!v) return E_OUTOFMEMORY;
    v->iface.lpVtbl = &g_svb_vtbl;
    SDL_SetAtomicInt(&v->ref, 1);
    v->size = len; v->fvf = fvf; v->usage = u;
    v->sys_mem = (BYTE *)calloc(1, len ? len : 1);
    *pp = &v->iface;
    return D3D_OK;
}
static HRESULT __stdcall sdev_CreateIndexBuffer(IDirect3DDevice8 *s, UINT len, DWORD u, D3DFORMAT fmt, D3DPOOL p, IDirect3DIndexBuffer8 **pp)
{
    (void)s; (void)u; (void)p;
    if (!pp) return D3DERR_INVALIDCALL;
    SdlGpuIndexBuffer *v = (SdlGpuIndexBuffer *)calloc(1, sizeof(*v));
    if (!v) return E_OUTOFMEMORY;
    v->iface.lpVtbl = &g_sib_vtbl;
    SDL_SetAtomicInt(&v->ref, 1);
    v->size = len; v->format = fmt;
    v->sys_mem = (BYTE *)calloc(1, len ? len : 1);
    *pp = &v->iface;
    return D3D_OK;
}
static HRESULT __stdcall sdev_CreateRenderTarget(IDirect3DDevice8 *s, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, BOOL lockable, IDirect3DSurface8 **pp)
{
    (void)s; (void)ms; (void)lockable;
    if (!pp) return D3DERR_INVALIDCALL;
    SdlGpuSurface *sf = (SdlGpuSurface *)calloc(1, sizeof(*sf));
    if (!sf) return E_OUTOFMEMORY;
    sf->iface.lpVtbl = &g_ssf_vtbl;
    SDL_SetAtomicInt(&sf->ref, 1);
    sf->width = w; sf->height = h; sf->format = fmt; sf->pitch = w * 4;
    sf->sys_mem = (BYTE *)calloc(1, (size_t)sf->pitch * h);
    *pp = &sf->iface;
    return D3D_OK;
}
static HRESULT __stdcall sdev_CreateDepthStencilSurface(IDirect3DDevice8 *s, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8 **pp)
{
    return sdev_CreateRenderTarget(s, w, h, fmt, ms, FALSE, pp);
}
static HRESULT __stdcall sdev_SetRenderTarget(IDirect3DDevice8 *s, IDirect3DSurface8 *rt, IDirect3DSurface8 *zs) { (void)s;(void)rt;(void)zs; return D3D_OK; }
static HRESULT __stdcall sdev_GetRenderTarget(IDirect3DDevice8 *s, IDirect3DSurface8 **pp) { (void)s; if (pp) *pp = NULL; return D3D_OK; }
static HRESULT __stdcall sdev_GetDepthStencilSurface(IDirect3DDevice8 *s, IDirect3DSurface8 **pp) { (void)s; if (pp) *pp = NULL; return D3D_OK; }
static HRESULT __stdcall sdev_SetViewport(IDirect3DDevice8 *s, const D3DVIEWPORT8 *vp) { (void)s; if (vp) g_sdl.viewport = *vp; return D3D_OK; }
static HRESULT __stdcall sdev_GetViewport(IDirect3DDevice8 *s, D3DVIEWPORT8 *vp) { (void)s; if (vp) *vp = g_sdl.viewport; return D3D_OK; }
static HRESULT __stdcall sdev_SetMaterial(IDirect3DDevice8 *s, const D3DMATERIAL8 *m) { (void)s; if (m) g_sdl.material = *m; return D3D_OK; }
static HRESULT __stdcall sdev_GetMaterial(IDirect3DDevice8 *s, D3DMATERIAL8 *m) { (void)s; if (m) *m = g_sdl.material; return D3D_OK; }
static HRESULT __stdcall sdev_SetLight(IDirect3DDevice8 *s, DWORD i, const D3DLIGHT8 *l) { (void)s; if (i < 8 && l) g_sdl.lights[i] = *l; return D3D_OK; }
static HRESULT __stdcall sdev_GetLight(IDirect3DDevice8 *s, DWORD i, D3DLIGHT8 *l) { (void)s; if (i < 8 && l) *l = g_sdl.lights[i]; return D3D_OK; }
static HRESULT __stdcall sdev_LightEnable(IDirect3DDevice8 *s, DWORD i, BOOL en) { (void)s; if (i < 8) g_sdl.light_enabled[i] = en; return D3D_OK; }
static HRESULT __stdcall sdev_SetVertexShader(IDirect3DDevice8 *s, DWORD h) { (void)s; g_sdl.current_vsh = h; if (!(h & 0x80000000u)) g_sdl.current_fvf = h; return D3D_OK; }
static HRESULT __stdcall sdev_GetVertexShader(IDirect3DDevice8 *s, DWORD *p) { (void)s; if (p) *p = g_sdl.current_vsh; return D3D_OK; }
static HRESULT __stdcall sdev_SetVertexShaderConstant(IDirect3DDevice8 *s, INT r, const void *p, DWORD c) { (void)s;(void)r;(void)p;(void)c; return D3D_OK; }
static HRESULT __stdcall sdev_SetPixelShader(IDirect3DDevice8 *s, DWORD h) { (void)s; g_sdl.current_psh = h; return D3D_OK; }
static HRESULT __stdcall sdev_GetPixelShader(IDirect3DDevice8 *s, DWORD *p) { (void)s; if (p) *p = g_sdl.current_psh; return D3D_OK; }
static HRESULT __stdcall sdev_SetPixelShaderConstant(IDirect3DDevice8 *s, INT r, const void *p, DWORD c) { (void)s;(void)r;(void)p;(void)c; return D3D_OK; }
static void    __stdcall sdev_SetGammaRamp(IDirect3DDevice8 *s, DWORD f, const D3DGAMMARAMP *r) { (void)s;(void)f;(void)r; }
static void    __stdcall sdev_GetGammaRamp(IDirect3DDevice8 *s, D3DGAMMARAMP *r) { (void)s;(void)r; }
static HRESULT __stdcall sdev_SetPalette(IDirect3DDevice8 *s, DWORD n, const void *e) { (void)s;(void)n;(void)e; return D3D_OK; }
static HRESULT __stdcall sdev_BeginPush(IDirect3DDevice8 *s, DWORD c, DWORD **pp) { (void)s;(void)c; if (pp) *pp = NULL; return D3D_OK; }
static HRESULT __stdcall sdev_EndPush(IDirect3DDevice8 *s, DWORD *p) { (void)s;(void)p; return D3D_OK; }
static HRESULT __stdcall sdev_Swap(IDirect3DDevice8 *s, DWORD f) { (void)f; return sdev_Present(s, NULL, NULL, NULL, NULL); }
static HRESULT __stdcall sdev_CreateImageSurface(IDirect3DDevice8 *s, UINT w, UINT h, D3DFORMAT fmt, IDirect3DSurface8 **pp) { return sdev_CreateRenderTarget(s, w, h, fmt, D3DMULTISAMPLE_NONE, TRUE, pp); }
static HRESULT __stdcall sdev_CreateCubeTexture(IDirect3DDevice8 *s, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture8 **pp) { (void)s;(void)e;(void)l;(void)u;(void)f;(void)p; if (pp) *pp = NULL; return D3D_OK; }
static HRESULT __stdcall sdev_CreateVolumeTexture(IDirect3DDevice8 *s, UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture8 **pp) { (void)s;(void)w;(void)h;(void)d;(void)l;(void)u;(void)f;(void)p; if (pp) *pp = NULL; return D3D_OK; }

static const IDirect3DDevice8Vtbl g_sdl_device_vtbl = {
    sdev_QueryInterface, sdev_AddRef, sdev_Release,
    sdev_GetDirect3D, sdev_GetDeviceCaps, sdev_GetDisplayMode, sdev_GetCreationParameters,
    sdev_Reset, sdev_Present, sdev_GetBackBuffer,
    sdev_BeginScene, sdev_EndScene, sdev_Clear,
    sdev_SetTransform, sdev_GetTransform,
    sdev_SetRenderState, sdev_GetRenderState,
    sdev_SetTextureStageState, sdev_GetTextureStageState,
    sdev_SetTexture, sdev_GetTexture,
    sdev_SetStreamSource, sdev_GetStreamSource,
    sdev_SetIndices, sdev_GetIndices,
    sdev_DrawPrimitive, sdev_DrawIndexedPrimitive,
    sdev_DrawPrimitiveUP, sdev_DrawIndexedPrimitiveUP,
    sdev_CreateTexture, sdev_CreateVertexBuffer, sdev_CreateIndexBuffer,
    sdev_CreateRenderTarget, sdev_CreateDepthStencilSurface,
    sdev_SetRenderTarget, sdev_GetRenderTarget, sdev_GetDepthStencilSurface,
    sdev_SetViewport, sdev_GetViewport,
    sdev_SetMaterial, sdev_GetMaterial,
    sdev_SetLight, sdev_GetLight, sdev_LightEnable,
    sdev_SetVertexShader, sdev_GetVertexShader, sdev_SetVertexShaderConstant,
    sdev_SetPixelShader, sdev_GetPixelShader, sdev_SetPixelShaderConstant,
    sdev_SetGammaRamp, sdev_GetGammaRamp,
    sdev_SetPalette,
    sdev_BeginPush, sdev_EndPush,
    sdev_Swap,
    sdev_CreateImageSurface,
    sdev_CreateCubeTexture,
    sdev_CreateVolumeTexture
};

static HRESULT __stdcall sd3d_QueryInterface(IDirect3D8 *s, const IID *i, void **p) { (void)s;(void)i;(void)p; return E_NOINTERFACE; }
static ULONG   __stdcall sd3d_AddRef(IDirect3D8 *s)  { (void)s; return 1; }
static ULONG   __stdcall sd3d_Release(IDirect3D8 *s) { (void)s; return 0; }

static HRESULT __stdcall sd3d_CreateDevice(IDirect3D8 *s, UINT adapter, DWORD devtype,
                                           HWND hwnd, DWORD flags,
                                           D3DPRESENT_PARAMETERS *pPP,
                                           IDirect3DDevice8 **pp)
{
    (void)s; (void)adapter; (void)devtype; (void)hwnd; (void)flags;
    if (!pp || !pPP) return D3DERR_INVALIDCALL;

    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            fprintf(stderr, "[D3D8-SDLGPU] SDL_InitSubSystem failed: %s\n", SDL_GetError());
            return D3DERR_INVALIDCALL;
        }
    }

    memset(&g_sdl, 0, sizeof(g_sdl));
    g_sdl.backbuf_w = pPP->BackBufferWidth  ? (int)pPP->BackBufferWidth  : 640;
    g_sdl.backbuf_h = pPP->BackBufferHeight ? (int)pPP->BackBufferHeight : 480;

    /* Reuse fb_present's SDL_Window if already running, otherwise create one */
    SDL_Window *existing_win = (SDL_Window *)xbox_FramebufferWindowHwnd();
    if (existing_win) {
        xbox_FramebufferSetGpuPresent(1);
        g_sdl.window = existing_win;
        g_sdl.owns_window = false;
    } else {
        g_sdl.window = SDL_CreateWindow(g_window_title,
                                        g_sdl.backbuf_w, g_sdl.backbuf_h,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!g_sdl.window) {
            fprintf(stderr, "[D3D8-SDLGPU] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return D3DERR_INVALIDCALL;
        }
        g_sdl.owns_window = true;
    }

    g_sdl.gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
        false, NULL);
    if (!g_sdl.gpu) {
        fprintf(stderr, "[D3D8-SDLGPU] SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        return D3DERR_INVALIDCALL;
    }

    if (!SDL_ClaimWindowForGPUDevice(g_sdl.gpu, g_sdl.window)) {
        fprintf(stderr, "[D3D8-SDLGPU] SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(g_sdl.gpu);
        g_sdl.gpu = NULL;
        return D3DERR_INVALIDCALL;
    }

    fprintf(stderr, "[D3D8-SDLGPU] Initialized SDL3 GPU device (%s backend, %dx%d)\n",
            SDL_GetGPUDeviceDriver(g_sdl.gpu), g_sdl.backbuf_w, g_sdl.backbuf_h);

    g_sdl_device.lpVtbl = &g_sdl_device_vtbl;
    g_sdl_initialized = TRUE;
    *pp = &g_sdl_device;
    return D3D_OK;
}

static const IDirect3D8Vtbl g_sd3d_vtbl = {
    sd3d_QueryInterface, sd3d_AddRef, sd3d_Release, sd3d_CreateDevice
};
static IDirect3D8 g_sd3d = { &g_sd3d_vtbl };

/* ========================================================================
 * Runtime Backend Dispatcher (D3D11 vs SDL3 GPU)
 * ======================================================================== */

#if defined(_WIN32) && defined(D3D8_RUNTIME_DISPATCH)
extern IDirect3D8       *d3d8_d3d11_Direct3DCreate8(UINT SDKVersion);
extern IDirect3DDevice8 *d3d8_d3d11_GetD3DDevice(void);
extern IDirect3DDevice8 *d3d8_d3d11_GetDevice(void);
extern void              d3d8_d3d11_PresentFrame(void);

static int use_sdl_gpu_backend(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *env = getenv("RECOMP_RENDERER");
        mode = (env && (! SDL_strcasecmp(env, "sdl") ||
                        ! SDL_strcasecmp(env, "sdl3") ||
                        ! SDL_strcasecmp(env, "sdl_gpu"))) ? 1 : 0;
        fprintf(stderr, "[D3D8] Runtime renderer selected: %s\n",
                mode ? "SDL3 GPU" : "Direct3D 11");
    }
    return mode;
}

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    if (use_sdl_gpu_backend())
        return &g_sd3d;
    return d3d8_d3d11_Direct3DCreate8(SDKVersion);
}

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    if (use_sdl_gpu_backend())
        return g_sdl_initialized ? &g_sdl_device : NULL;
    return d3d8_d3d11_GetD3DDevice();
}

IDirect3DDevice8 *d3d8_GetDevice(void)
{
    if (use_sdl_gpu_backend())
        return &g_sdl_device;
    return d3d8_d3d11_GetDevice();
}

void d3d8_PresentFrame(void)
{
    if (use_sdl_gpu_backend()) {
        sdev_Present(&g_sdl_device, NULL, NULL, NULL, NULL);
        return;
    }
    d3d8_d3d11_PresentFrame();
}

#else /* Non-Windows (Linux / macOS): always use SDL3 GPU */

volatile int g_suppress_present = 0;

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    return &g_sd3d;
}

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_sdl_initialized ? &g_sdl_device : NULL;
}

IDirect3DDevice8 *d3d8_GetDevice(void)
{
    return &g_sdl_device;
}

void d3d8_PresentFrame(void)
{
    if (!g_suppress_present)
        sdev_Present(&g_sdl_device, NULL, NULL, NULL, NULL);
}
#endif