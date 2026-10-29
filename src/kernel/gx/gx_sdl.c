/* ── The renderer's Direct3D 11 subset on SDL3's GPU API (2026-10-29) ──────
 *
 * gx_d3d11.h routes every D3D11/DXGI call of the renderer here when the
 * player picks Vulkan or Direct3D 12. Objects are small structs standing in
 * for the COM objects (the renderer only passes them back to these calls);
 * everything else is the renderer's own code, unchanged.
 *
 * Direct3D 11 has an immediate context: a draw can follow a texture upload
 * which follows a draw, in any order. SDL_GPU (like Vulkan and D3D12) records
 * copies and draws in separate passes. So the context records a command list
 * and turns it into passes when it is flushed: at each Present, before a
 * read-back, and when a vertex ring wraps. Vertex data written through Map is
 * uploaded once per flush (the renderer's ring never overwrites what a pending
 * draw reads before it wraps, and a wrap flushes first); texture uploads,
 * copies, clears and draws keep their order.
 *
 * Differences from Direct3D 11, all unused or harmless here:
 *  - no geometry shaders: the triangle rebuild the D3D11 geometry shader did
 *    runs in a vertex shader that pulls its triangle (vs_pull, gxs_set_pull);
 *  - one scissor rectangle: a draw with several is drawn once per rectangle;
 *  - no BORDER address mode: clamp to edge;
 *  - no shared textures with other devices (the HTML HUD then uploads from
 *    the CPU, its existing fallback).
 */
#define GX_IMPL
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "gx_d3d11.h"
#include "gx_dxc.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

int g_gx_sg;
static int s_backend;                     /* 0 D3D11, 1 Vulkan, 2 D3D12 */

char *xbox_user_path(const char *rel, char *out, size_t n);
extern void xbox_FramebufferSetGpuPresenter(int (*fn)(void *window, int letterbox));
extern void *xbox_FramebufferWindowHwnd(void);

/* ---- objects ------------------------------------------------------------ */

#define GX_MAGIC 0x47584F42u
enum { K_DEVICE = 1, K_CONTEXT, K_TEX, K_VIEW, K_BUF, K_SHADER, K_BLEND, K_DSS, K_RS, K_SMP, K_BLOB,
       K_LAYOUT, K_GS, K_SWAP };

typedef struct GxObj {
    uint32_t magic, kind;
    volatile LONG refs;
    void (*destroy)(struct GxObj *);
} GxObj;

typedef struct GxFence {
    SDL_GPUFence *f;
    volatile LONG refs;
} GxFence;

typedef struct {
    GxObj o;
    D3D11_TEXTURE2D_DESC d;
    SDL_GPUTexture *t;                    /* NULL for a staging texture */
    SDL_GPUTextureFormat fmt;
    uint32_t block;                       /* 4 for BC formats, else 1 */
    uint32_t unit;                        /* bytes per texel, or per 4x4 block */
    SDL_GPUTransferBuffer *dl;            /* staging: where downloads land */
    uint32_t dl_size;
    uint32_t dl_flush;                    /* flush serial holding its pending download (0 none) */
    GxFence *dl_fence;                    /* the submission that carried it */
    int dl_mapped;
    uint8_t *dyn;                         /* dynamic: the bytes Map hands out */
} GxTex;

typedef struct { GxObj o; GxTex *tex; void *buf; } GxView;

typedef struct {
    GxObj o;
    D3D11_BUFFER_DESC d;
    uint8_t *cpu;                         /* contents as last written */
    SDL_GPUBuffer *b;                     /* vertex/index buffers */
    uint32_t version;
    uint32_t lo, hi;                      /* bytes pending draws read (lo >= hi: none) */
} GxBuf;

typedef struct {
    GxObj o;
    SDL_GPUShaderStage stage;
    SDL_GPUShader *sh;
    uint32_t id;
    uint8_t pairs[16][2];                 /* sampler slot -> (texture slot, sampler slot) */
    GxReflect r;
} GxShader;

typedef struct { GxObj o; D3D11_BLEND_DESC d; } GxBlend;
typedef struct { GxObj o; D3D11_DEPTH_STENCIL_DESC d; } GxDss;
typedef struct { GxObj o; D3D11_RASTERIZER_DESC d; } GxRs;
typedef struct { GxObj o; SDL_GPUSampler *s; } GxSmp;
typedef struct { GxObj o; void *p; size_t n; } GxBlob;
typedef struct { GxObj o; GxTex *bb; } GxSwap;

/* The compiled-shader blob gx_D3DCompile hands out (and the pixel-shader
 * cache stores): this header, then the SPIR-V or DXIL. */
typedef struct {
    uint32_t magic;                       /* 'GXSH' */
    uint32_t format;                      /* SDL_GPU_SHADERFORMAT_* */
    uint32_t stage;                       /* 0 vertex, 1 fragment */
    GxReflect r;
    uint8_t pairs[16][2];
    char entry[32];
} GxShaderHdr;
#define GXSH_MAGIC 0x48535847u

static GxObj s_dev_obj = { GX_MAGIC, K_DEVICE, 1, NULL };
static GxObj s_ctx_obj = { GX_MAGIC, K_CONTEXT, 1, NULL };

/* Per-frame counters, logged every 300 presents (where the time goes). */
static struct {
    unsigned draws, rpasses, cpasses, copies, uploads, downloads, submits, waits, flushes;
    double wait_ms, flush_ms, pipe_ms;
    unsigned frames;
} s_st;

static double qpc_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}


static void *gx_new(size_t sz, uint32_t kind, void (*destroy)(GxObj *))
{
    GxObj *o = (GxObj *)calloc(1, sz);
    if (!o) return NULL;
    o->magic = GX_MAGIC; o->kind = kind; o->refs = 1; o->destroy = destroy;
    return o;
}

ULONG gxs_obj_AddRef(void *p)
{
    GxObj *o = (GxObj *)p;
    if (!o || o->magic != GX_MAGIC) return 0;
    return (ULONG)InterlockedIncrement(&o->refs);
}

ULONG gxs_obj_Release(void *p)
{
    GxObj *o = (GxObj *)p;
    LONG n;
    if (!o || o->magic != GX_MAGIC) return 0;
    if (o->kind == K_DEVICE || o->kind == K_CONTEXT) return 1;
    n = InterlockedDecrement(&o->refs);
    if (n == 0) {
        if (o->destroy) o->destroy(o);
        o->magic = 0;
        free(o);
    }
    return (ULONG)n;
}

HRESULT gxs_obj_QueryInterface(void *o, REFIID riid, void **out)
{
    (void)o; (void)riid;
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

static void ref_set(void **slot, void *obj)
{
    if (*slot == obj) return;
    if (obj) gxs_obj_AddRef(obj);
    if (*slot) gxs_obj_Release(*slot);
    *slot = obj;
}

/* ---- device ---------------------------------------------------------------- */

static struct {
    SDL_GPUDevice *dev;
    SDL_Window *win;
    volatile LONG claimed;                /* 1 claimed, -1 failed */
    SDL_GPUTextureFormat depth_fmt;
    CRITICAL_SECTION create_lock;         /* object creation from worker threads */
    volatile LONG shader_ids;
    SDL_GPUTexture *white;                /* bound where a slot has no texture */
    SDL_GPUTexture *white_cube;
    SDL_GPUSampler *point;
    int present_mode;                     /* -1 unset */
} G = { .present_mode = -1 };

const char *gx_backend_name(void)
{
    return s_backend == 1 ? "Vulkan" : s_backend == 2 ? "Direct3D 12" : "Direct3D 11";
}

const char *gx_cache_suffix(void)
{
    return s_backend == 1 ? "-vk" : s_backend == 2 ? "-dx12" : "";
}

/* RECOMP_RENDERER=vulkan|d3d12|d3d11, else `renderer = N` in
 * nfl2k5_video.ini (0 Direct3D 11, 1 Vulkan, 2 Direct3D 12). */
static int gx_choice(void)
{
    const char *e = getenv("RECOMP_RENDERER");
    if (e && *e) {
        if (!_stricmp(e, "vulkan") || !_stricmp(e, "vk") || !_stricmp(e, "sdl") || !_stricmp(e, "sdl3") ||
            !_stricmp(e, "sdl_gpu"))
            return 1;
        if (!_stricmp(e, "d3d12") || !_stricmp(e, "dx12"))
            return 2;
        return 0;
    }
    {
        char path[MAX_PATH], line[256];
        FILE *f = fopen(xbox_user_path("nfl2k5_video.ini", path, sizeof path), "r");
        int v = 0;
        if (!f) return 0;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, " renderer = %d", &v) == 1) break;
        fclose(f);
        return v == 1 || v == 2 ? v : 0;
    }
}

/* SDL's GPU warnings (why a driver was refused, validation messages with
 * RECOMP_GPU_DEBUG) into the game log, then on to whatever logged before. */
static SDL_LogOutputFunction s_prev_log;
static void *s_prev_log_ud;
static void gx_log(void *ud, int cat, SDL_LogPriority pri, const char *msg)
{
    (void)ud;
    if (cat == SDL_LOG_CATEGORY_GPU || pri >= SDL_LOG_PRIORITY_WARN)
        fprintf(stderr, "  [SDL] %s\n", msg);
    if (s_prev_log) s_prev_log(s_prev_log_ud, cat, pri, msg);
}

static int gx_sdl_init(int backend)
{
    SDL_PropertiesID p;
    if (!gx_dxc_available())
        return 0;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "  [GX] SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }
    {
        /* Vulkan needs the video subsystem finished (it creates a surface for
         * the window). The window thread starts it, and SDL_WasInit reports
         * it before start-up is over; the window existing is the real signal
         * (as in sdlgpu/sdlgpu.inc.c). Direct3D 12 does not care. */
        int i;
        for (i = 0; i < 500 && !xbox_FramebufferWindowHwnd(); i++)
            SDL_Delay(10);
    }
    SDL_GetLogOutputFunction(&s_prev_log, &s_prev_log_ud);
    SDL_SetLogOutputFunction(gx_log, NULL);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, getenv("RECOMP_GPU_DEBUG") ? SDL_LOG_PRIORITY_VERBOSE : SDL_LOG_PRIORITY_INFO);
    p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, backend == 1 ? "vulkan" : "direct3d12");
    SDL_SetBooleanProperty(p, backend == 1 ? SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN
                                           : SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, getenv("RECOMP_GPU_DEBUG") != NULL);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, false);
    G.dev = SDL_CreateGPUDeviceWithProperties(p);
    SDL_DestroyProperties(p);
    if (!G.dev) {
        fprintf(stderr, "  [GX] %s device failed: %s\n", backend == 1 ? "Vulkan" : "Direct3D 12", SDL_GetError());
        return 0;
    }
    InitializeCriticalSection(&G.create_lock);
    G.depth_fmt = SDL_GPUTextureSupportsFormat(G.dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                               SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                      ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT : SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    fprintf(stderr, "  [GX] renderer on %s through SDL_GPU (driver %s, depth %s)\n",
            backend == 1 ? "Vulkan" : "Direct3D 12", SDL_GetGPUDeviceDriver(G.dev),
            G.depth_fmt == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT ? "D32S8" : "D24S8");
    return 1;
}

static void gx_sdl_defaults(void);

HRESULT gx_D3D11CreateDevice(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
                             const D3D_FEATURE_LEVEL *fls, UINT nfl, UINT sdk, ID3D11Device **dev,
                             D3D_FEATURE_LEVEL *fl, ID3D11DeviceContext **ctx)
{
    static int chosen = -1;
    if (chosen < 0) {
        chosen = gx_choice();
        if (chosen && gx_sdl_init(chosen)) {
            s_backend = chosen;
            g_gx_sg = 1;
            gx_sdl_defaults();
        } else {
            if (chosen) fprintf(stderr, "  [GX] falling back to Direct3D 11\n");
            s_backend = 0;
            g_gx_sg = 0;
        }
    }
    if (!g_gx_sg)
        return D3D11CreateDevice(ad, dt, sw, flags, fls, nfl, sdk, dev, fl, ctx);
    if (dev) *dev = (ID3D11Device *)&s_dev_obj;
    if (ctx) *ctx = (ID3D11DeviceContext *)&s_ctx_obj;
    if (fl) *fl = D3D_FEATURE_LEVEL_11_0;
    return S_OK;
}

D3D_FEATURE_LEVEL gxs_ID3D11Device_GetFeatureLevel(void *d)
{
    (void)d;
    return D3D_FEATURE_LEVEL_11_0;
}

/* ---- shaders ----------------------------------------------------------- */

/* "//@GX_PAIRS <entry> t:s t:s ..." in the source: which texture and sampler
 * each SDL sampler slot of that entry point combines. Default slot k = (k, k). */
static void parse_pairs(const char *src, size_t len, const char *entry, uint8_t pairs[16][2])
{
    const char *p = src, *end = src + len;
    size_t el = strlen(entry);
    int k;
    for (k = 0; k < 16; k++) { pairs[k][0] = (uint8_t)k; pairs[k][1] = (uint8_t)k; }
    while (p < end && (p = strstr(p, "//@GX_PAIRS ")) != NULL && p < end) {
        const char *q = p + 12;
        if (!strncmp(q, entry, el) && (q[el] == ' ' || q[el] == '\t')) {
            q += el;
            k = 0;
            while (k < 16 && *q && *q != '\n' && *q != '\r') {
                unsigned t, s;
                int used = 0;
                while (*q == ' ' || *q == '\t') q++;
                if (sscanf(q, "%u:%u%n", &t, &s, &used) != 2 || !used) break;
                pairs[k][0] = (uint8_t)t; pairs[k][1] = (uint8_t)s;
                k++;
                q += used;
            }
            return;
        }
        p += 12;
    }
}

static void blob_destroy(GxObj *o) { free(((GxBlob *)o)->p); }

static GxBlob *blob_new(const void *data, size_t n)
{
    GxBlob *b = (GxBlob *)gx_new(sizeof *b, K_BLOB, blob_destroy);
    if (!b) return NULL;
    b->p = malloc(n ? n : 1);
    if (!b->p) { free(b); return NULL; }
    memcpy(b->p, data, n);
    b->n = n;
    return b;
}

LPVOID gxs_ID3D10Blob_GetBufferPointer(void *b) { return ((GxBlob *)b)->p; }
SIZE_T gxs_ID3D10Blob_GetBufferSize(void *b) { return ((GxBlob *)b)->n; }

HRESULT gx_D3DCompile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defs, ID3DInclude *inc,
                      LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **err)
{
    char profile[16], msg[4096];
    void *out = NULL;
    size_t outlen = 0;
    GxShaderHdr h;
    int vs;
    if (!g_gx_sg)
        return D3DCompile(src, len, name, defs, inc, entry, target, flags1, flags2, code, err);
    if (code) *code = NULL;
    if (err) *err = NULL;
    if (!target || (target[0] != 'v' && target[0] != 'p')) {          /* geometry shaders: none */
        if (err) *err = (ID3DBlob *)blob_new("no geometry shaders on SDL_GPU", 31);
        return E_FAIL;
    }
    vs = target[0] == 'v';
    snprintf(profile, sizeof profile, "%s_6_0", vs ? "vs" : "ps");
    memset(&h, 0, sizeof h);
    if (!gx_dxc_compile((const char *)src, len, entry, profile, s_backend == 1,
                        (flags1 & D3DCOMPILE_IEEE_STRICTNESS) != 0, &out, &outlen, msg, sizeof msg, &h.r)) {
        if (err) *err = (ID3DBlob *)blob_new(msg, strlen(msg) + 1);
        return E_FAIL;
    }
    h.magic = GXSH_MAGIC;
    h.format = s_backend == 1 ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_DXIL;
    h.stage = vs ? 0 : 1;
    parse_pairs((const char *)src, len, entry, h.pairs);
    snprintf(h.entry, sizeof h.entry, "%s", entry);
    {
        GxBlob *b = (GxBlob *)gx_new(sizeof *b, K_BLOB, blob_destroy);
        if (!b || !(b->p = malloc(sizeof h + outlen))) { free(b); free(out); return E_OUTOFMEMORY; }
        memcpy(b->p, &h, sizeof h);
        memcpy((uint8_t *)b->p + sizeof h, out, outlen);
        b->n = sizeof h + outlen;
        free(out);
        if (code) *code = (ID3DBlob *)b; else gxs_obj_Release(b);
    }
    return S_OK;
}

static void shader_destroy(GxObj *o);

static HRESULT make_shader(const void *code, SIZE_T len, int want_stage, void **out)
{
    const GxShaderHdr *h = (const GxShaderHdr *)code;
    SDL_GPUShaderCreateInfo ci;
    GxShader *s;
    if (out) *out = NULL;
    if (!code || len < sizeof *h || h->magic != GXSH_MAGIC || (int)h->stage != want_stage ||
        h->format != (s_backend == 1 ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_DXIL))
        return E_INVALIDARG;
    s = (GxShader *)gx_new(sizeof *s, K_SHADER, shader_destroy);
    if (!s) return E_OUTOFMEMORY;
    memset(&ci, 0, sizeof ci);
    ci.code = (const Uint8 *)code + sizeof *h;
    ci.code_size = len - sizeof *h;
    ci.entrypoint = h->entry;
    ci.format = h->format;
    ci.stage = want_stage ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
    ci.num_samplers = h->r.nsamplers;
    ci.num_storage_buffers = h->r.nstorage;
    ci.num_uniform_buffers = h->r.nuniform;
    EnterCriticalSection(&G.create_lock);
    s->sh = SDL_CreateGPUShader(G.dev, &ci);
    LeaveCriticalSection(&G.create_lock);
    if (!s->sh) {
        fprintf(stderr, "  [GX] shader %s: %s\n", h->entry, SDL_GetError());
        free(s);
        return E_FAIL;
    }
    s->stage = ci.stage;
    s->id = (uint32_t)InterlockedIncrement(&G.shader_ids);
    memcpy(s->pairs, h->pairs, sizeof s->pairs);
    s->r = h->r;
    if (out) *out = s; else gxs_obj_Release(s);
    return S_OK;
}

HRESULT gxs_ID3D11Device_CreateVertexShader(void *d, const void *code, SIZE_T len, ID3D11ClassLinkage *l,
                                            ID3D11VertexShader **out)
{
    (void)d; (void)l;
    return make_shader(code, len, 0, (void **)out);
}

HRESULT gxs_ID3D11Device_CreatePixelShader(void *d, const void *code, SIZE_T len, ID3D11ClassLinkage *l,
                                           ID3D11PixelShader **out)
{
    (void)d; (void)l;
    return make_shader(code, len, 1, (void **)out);
}

HRESULT gxs_ID3D11Device_CreateGeometryShader(void *d, const void *code, SIZE_T len, ID3D11ClassLinkage *l,
                                              ID3D11GeometryShader **out)
{
    (void)d; (void)code; (void)len; (void)l;
    if (out) *out = NULL;
    return E_NOTIMPL;
}

HRESULT gxs_ID3D11Device_CreateInputLayout(void *d, const D3D11_INPUT_ELEMENT_DESC *el, UINT n, const void *code,
                                           SIZE_T len, ID3D11InputLayout **out)
{
    (void)d; (void)el; (void)n; (void)code; (void)len;
    *out = (ID3D11InputLayout *)gx_new(sizeof(GxObj), K_LAYOUT, NULL);   /* vertices are pulled, see vs_pull */
    return *out ? S_OK : E_OUTOFMEMORY;
}

/* ---- deferred releases ---------------------------------------------------- */

/* SDL objects that commands still waiting in the list refer to: released
 * once the list has been recorded (SDL then keeps them until the GPU is done). */
enum { DR_TEX, DR_BUF, DR_SHADER, DR_SMP, DR_TB };
static struct { int kind; void *p; } *s_dr;
static int s_ndr, s_capdr;

static void defer_release(int kind, void *p)
{
    if (!p) return;
    EnterCriticalSection(&G.create_lock);          /* worker threads release failed shaders */
    if (s_ndr == s_capdr) {
        s_capdr = s_capdr ? s_capdr * 2 : 256;
        s_dr = realloc(s_dr, sizeof *s_dr * (size_t)s_capdr);
    }
    s_dr[s_ndr].kind = kind; s_dr[s_ndr].p = p; s_ndr++;
    LeaveCriticalSection(&G.create_lock);
}

static void run_releases(void)
{
    int i;
    EnterCriticalSection(&G.create_lock);
    for (i = 0; i < s_ndr; i++) {
        switch (s_dr[i].kind) {
        case DR_TEX:    SDL_ReleaseGPUTexture(G.dev, (SDL_GPUTexture *)s_dr[i].p); break;
        case DR_BUF:    SDL_ReleaseGPUBuffer(G.dev, (SDL_GPUBuffer *)s_dr[i].p); break;
        case DR_SHADER: SDL_ReleaseGPUShader(G.dev, (SDL_GPUShader *)s_dr[i].p); break;
        case DR_SMP:    SDL_ReleaseGPUSampler(G.dev, (SDL_GPUSampler *)s_dr[i].p); break;
        case DR_TB:     SDL_ReleaseGPUTransferBuffer(G.dev, (SDL_GPUTransferBuffer *)s_dr[i].p); break;
        }
    }
    s_ndr = 0;
    LeaveCriticalSection(&G.create_lock);
}

static void fence_release(GxFence *f)
{
    if (f && InterlockedDecrement(&f->refs) == 0) {
        SDL_ReleaseGPUFence(G.dev, f->f);
        free(f);
    }
}

static void shader_destroy(GxObj *o)
{
    defer_release(DR_SHADER, ((GxShader *)o)->sh);
}

/* ---- textures and views -------------------------------------------------- */

static void tex_destroy(GxObj *o)
{
    GxTex *t = (GxTex *)o;
    defer_release(DR_TEX, t->t);
    if (t->dl) {
        if (t->dl_mapped) SDL_UnmapGPUTransferBuffer(G.dev, t->dl);
        defer_release(DR_TB, t->dl);
    }
    fence_release(t->dl_fence);
    free(t->dyn);
}

static SDL_GPUTextureFormat map_format(DXGI_FORMAT f, uint32_t *block, uint32_t *unit)
{
    *block = 1; *unit = 4;
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_BC1_UNORM: *block = 4; *unit = 8;  return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case DXGI_FORMAT_BC2_UNORM: *block = 4; *unit = 16; return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
    case DXGI_FORMAT_BC3_UNORM: *block = 4; *unit = 16; return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: *unit = 8; return G.depth_fmt;
    default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

static void tex_upload(GxTex *t, uint32_t layer, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const void *data, uint32_t pitch);

HRESULT gxs_ID3D11Device_CreateTexture2D(void *d, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *init,
                                         ID3D11Texture2D **out)
{
    GxTex *t;
    SDL_GPUTextureCreateInfo ci;
    uint32_t mips, k;
    (void)d;
    if (out) *out = NULL;
    t = (GxTex *)gx_new(sizeof *t, K_TEX, tex_destroy);
    if (!t) return E_OUTOFMEMORY;
    t->d = *desc;
    t->fmt = map_format(desc->Format, &t->block, &t->unit);
    if (t->fmt == SDL_GPU_TEXTUREFORMAT_INVALID) {
        fprintf(stderr, "  [GX] texture format %d unsupported\n", (int)desc->Format);
        free(t);
        return E_INVALIDARG;
    }
    mips = desc->MipLevels;
    if (!mips) {
        uint32_t m = desc->Width > desc->Height ? desc->Width : desc->Height;
        for (mips = 1; m > 1; m >>= 1) mips++;
        t->d.MipLevels = mips;
    }
    if (desc->Usage == D3D11_USAGE_STAGING) {
        SDL_GPUTransferBufferCreateInfo ti;
        memset(&ti, 0, sizeof ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        t->dl_size = SDL_CalculateGPUTextureFormatSize(t->fmt, desc->Width, desc->Height, 1);
        ti.size = t->dl_size;
        EnterCriticalSection(&G.create_lock);
        t->dl = SDL_CreateGPUTransferBuffer(G.dev, &ti);
        LeaveCriticalSection(&G.create_lock);
        if (!t->dl) { free(t); return E_OUTOFMEMORY; }
        *out = (ID3D11Texture2D *)t;
        return S_OK;
    }
    memset(&ci, 0, sizeof ci);
    ci.type = (desc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? SDL_GPU_TEXTURETYPE_CUBE
            : desc->ArraySize > 1 ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D;
    ci.format = t->fmt;
    if (desc->BindFlags & D3D11_BIND_SHADER_RESOURCE) ci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (desc->BindFlags & D3D11_BIND_RENDER_TARGET) ci.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if (desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) ci.usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (!ci.usage) ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = desc->Width; ci.height = desc->Height;
    ci.layer_count_or_depth = desc->ArraySize ? desc->ArraySize : 1;
    ci.num_levels = mips;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    EnterCriticalSection(&G.create_lock);
    t->t = SDL_CreateGPUTexture(G.dev, &ci);
    LeaveCriticalSection(&G.create_lock);
    if (!t->t) {
        fprintf(stderr, "  [GX] texture %ux%u format %d: %s\n", desc->Width, desc->Height, (int)desc->Format, SDL_GetError());
        free(t);
        return E_OUTOFMEMORY;
    }
    if (desc->Usage == D3D11_USAGE_DYNAMIC)
        t->dyn = (uint8_t *)malloc((size_t)SDL_CalculateGPUTextureFormatSize(t->fmt, desc->Width, desc->Height, 1));
    if (init) {
        uint32_t layers = ci.layer_count_or_depth;
        for (k = 0; k < layers * mips; k++) {
            uint32_t mip = k % mips, layer = k / mips;
            uint32_t w = desc->Width >> mip, h = desc->Height >> mip;
            if (!w) w = 1;
            if (!h) h = 1;
            if (init[k].pSysMem)
                tex_upload(t, layer, mip, 0, 0, w, h, init[k].pSysMem, init[k].SysMemPitch);
        }
    }
    *out = (ID3D11Texture2D *)t;
    return S_OK;
}

void gxs_ID3D11Texture2D_GetDesc(void *t, D3D11_TEXTURE2D_DESC *d)
{
    *d = ((GxTex *)t)->d;
}

static void view_destroy(GxObj *o)
{
    gxs_obj_Release(((GxView *)o)->tex);
    gxs_obj_Release(((GxView *)o)->buf);
}

static HRESULT make_view(ID3D11Resource *res, void **out)
{
    GxTex *t = (GxTex *)res;
    GxView *v;
    if (out) *out = NULL;
    if (!t || t->o.magic != GX_MAGIC || (t->o.kind != K_TEX && t->o.kind != K_BUF) ||
        (t->o.kind == K_TEX && !t->t)) return E_INVALIDARG;
    v = (GxView *)gx_new(sizeof *v, K_VIEW, view_destroy);
    if (!v) return E_OUTOFMEMORY;
    if (t->o.kind == K_BUF) v->buf = t; else v->tex = t;
    gxs_obj_AddRef(t);
    if (out) *out = v; else gxs_obj_Release(v);
    return S_OK;
}

HRESULT gxs_ID3D11Device_CreateShaderResourceView(void *d, ID3D11Resource *res, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
                                                  ID3D11ShaderResourceView **out)
{
    (void)d; (void)desc;
    return make_view(res, (void **)out);
}

HRESULT gxs_ID3D11Device_CreateRenderTargetView(void *d, ID3D11Resource *res, const D3D11_RENDER_TARGET_VIEW_DESC *desc,
                                                ID3D11RenderTargetView **out)
{
    (void)d; (void)desc;
    return make_view(res, (void **)out);
}

HRESULT gxs_ID3D11Device_CreateDepthStencilView(void *d, ID3D11Resource *res, const D3D11_DEPTH_STENCIL_VIEW_DESC *desc,
                                                ID3D11DepthStencilView **out)
{
    (void)d; (void)desc;
    return make_view(res, (void **)out);
}

/* ---- buffers ----------------------------------------------------------- */

static void buf_destroy(GxObj *o)
{
    GxBuf *b = (GxBuf *)o;
    defer_release(DR_BUF, b->b);
    free(b->cpu);
}

HRESULT gxs_ID3D11Device_CreateBuffer(void *d, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *init,
                                      ID3D11Buffer **out)
{
    GxBuf *b;
    (void)d;
    if (out) *out = NULL;
    b = (GxBuf *)gx_new(sizeof *b, K_BUF, buf_destroy);
    if (!b) return E_OUTOFMEMORY;
    b->d = *desc;
    b->cpu = (uint8_t *)calloc(1, desc->ByteWidth ? desc->ByteWidth : 1);
    if (!b->cpu) { free(b); return E_OUTOFMEMORY; }
    if (init && init->pSysMem) memcpy(b->cpu, init->pSysMem, desc->ByteWidth);
    b->lo = 0xFFFFFFFFu; b->hi = 0;
    if (desc->BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE)) {
        /* Every one of them is read by vertex shaders as a storage buffer
         * (vertices and indices are pulled, see vs_pull and gpu_vp_translate). */
        SDL_GPUBufferCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        if (desc->BindFlags & D3D11_BIND_VERTEX_BUFFER) ci.usage |= SDL_GPU_BUFFERUSAGE_VERTEX;
        if (desc->BindFlags & D3D11_BIND_INDEX_BUFFER) ci.usage |= SDL_GPU_BUFFERUSAGE_INDEX;
        ci.size = desc->ByteWidth;
        EnterCriticalSection(&G.create_lock);
        b->b = SDL_CreateGPUBuffer(G.dev, &ci);
        LeaveCriticalSection(&G.create_lock);
        if (!b->b) { free(b->cpu); free(b); return E_OUTOFMEMORY; }
    }
    *out = (ID3D11Buffer *)b;
    return S_OK;
}

/* ---- state objects --------------------------------------------------------- */

HRESULT gxs_ID3D11Device_CreateBlendState(void *d, const D3D11_BLEND_DESC *desc, ID3D11BlendState **out)
{
    GxBlend *b = (GxBlend *)gx_new(sizeof *b, K_BLEND, NULL);
    (void)d;
    if (!b) return E_OUTOFMEMORY;
    b->d = *desc;
    *out = (ID3D11BlendState *)b;
    return S_OK;
}

HRESULT gxs_ID3D11Device_CreateDepthStencilState(void *d, const D3D11_DEPTH_STENCIL_DESC *desc, ID3D11DepthStencilState **out)
{
    GxDss *s = (GxDss *)gx_new(sizeof *s, K_DSS, NULL);
    (void)d;
    if (!s) return E_OUTOFMEMORY;
    s->d = *desc;
    *out = (ID3D11DepthStencilState *)s;
    return S_OK;
}

HRESULT gxs_ID3D11Device_CreateRasterizerState(void *d, const D3D11_RASTERIZER_DESC *desc, ID3D11RasterizerState **out)
{
    GxRs *s = (GxRs *)gx_new(sizeof *s, K_RS, NULL);
    (void)d;
    if (!s) return E_OUTOFMEMORY;
    s->d = *desc;
    *out = (ID3D11RasterizerState *)s;
    return S_OK;
}

static void smp_destroy(GxObj *o) { defer_release(DR_SMP, ((GxSmp *)o)->s); }

static SDL_GPUSamplerAddressMode map_addr(D3D11_TEXTURE_ADDRESS_MODE m)
{
    switch (m) {
    case D3D11_TEXTURE_ADDRESS_WRAP:   return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case D3D11_TEXTURE_ADDRESS_MIRROR: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    default:                           return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;   /* CLAMP, BORDER */
    }
}

HRESULT gxs_ID3D11Device_CreateSamplerState(void *d, const D3D11_SAMPLER_DESC *desc, ID3D11SamplerState **out)
{
    SDL_GPUSamplerCreateInfo ci;
    GxSmp *s;
    D3D11_FILTER f = desc->Filter;
    (void)d;
    memset(&ci, 0, sizeof ci);
    if (f == D3D11_FILTER_ANISOTROPIC) {
        ci.min_filter = ci.mag_filter = SDL_GPU_FILTER_LINEAR;
        ci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        ci.enable_anisotropy = true;
        ci.max_anisotropy = (float)(desc->MaxAnisotropy ? desc->MaxAnisotropy : 1);
    } else {
        ci.min_filter = (f & 0x10) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;   /* D3D11 filter bits */
        ci.mag_filter = (f & 0x04) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        ci.mipmap_mode = (f & 0x01) ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    }
    ci.address_mode_u = map_addr(desc->AddressU);
    ci.address_mode_v = map_addr(desc->AddressV);
    ci.address_mode_w = map_addr(desc->AddressW);
    ci.mip_lod_bias = desc->MipLODBias;
    ci.min_lod = desc->MinLOD;
    ci.max_lod = desc->MaxLOD > 1000.0f ? 1000.0f : desc->MaxLOD;
    s = (GxSmp *)gx_new(sizeof *s, K_SMP, smp_destroy);
    if (!s) return E_OUTOFMEMORY;
    EnterCriticalSection(&G.create_lock);
    s->s = SDL_CreateGPUSampler(G.dev, &ci);
    LeaveCriticalSection(&G.create_lock);
    if (!s->s) { free(s); return E_FAIL; }
    *out = (ID3D11SamplerState *)s;
    return S_OK;
}

/* ---- the command list ------------------------------------------------------- */

enum { C_DRAW, C_CLEAR_RT, C_CLEAR_DS, C_COPY, C_UPLOAD, C_DOWNLOAD, C_MIPS };

typedef struct {
    uint8_t type;
    union {
        uint32_t draw;                                                  /* index into s_draws */
        struct { SDL_GPUTexture *t; float c[4]; } clr;
        struct { SDL_GPUTexture *t; uint32_t flags; float z; uint8_t s; } cds;
        struct { SDL_GPUTexture *src, *dst; uint32_t sl, sm, sx, sy, dl, dm, dx, dy, w, h; } cp;
        struct { SDL_GPUTexture *t; uint32_t layer, mip, x, y, w, h, off, ppr, rows; } up;
        struct { SDL_GPUTexture *t; uint32_t x, y, w, h; GxTex *stg; } dl;
        struct { SDL_GPUTexture *t; } mips;
    } u;
} GxCmd;

typedef struct {
    SDL_GPUGraphicsPipeline *pipe;
    SDL_GPUTexture *rt, *ds;
    SDL_GPUTextureSamplerBinding fs[8];
    uint8_t nfs, nsc, sref;
    SDL_GPUBuffer *vsbuf[2];
    uint8_t nvsbuf, nvsu;
    int32_t vsu[4], fsu;                  /* uniform arena offsets, -1 none */
    uint16_t vsu_len[4], fsu_len;
    SDL_GPUViewport vp;
    SDL_Rect sc[8];
    SDL_FColor bf;
    uint32_t n, first;
} GxDraw;

static GxCmd *s_cmds;    static uint32_t s_ncmd, s_capcmd;
static GxDraw *s_draws;  static uint32_t s_ndraw, s_capdraw;
static uint8_t *s_uni;   static uint32_t s_nuni, s_capuni;
static uint8_t *s_up;    static uint32_t s_nup, s_capup;          /* texture upload bytes */
static GxBuf *s_vbufs[16]; static int s_nvbufs;                   /* buffers pending draws read */
static GxTex **s_dl; static int s_ndl, s_capdl;                   /* stagings awaiting submission */
static uint32_t s_flush_serial = 1;
static SDL_GPUCommandBuffer *s_cb;
static SDL_GPUTransferBuffer *s_tb; static uint32_t s_tb_size;

static GxCmd *cmd_add(uint8_t type)
{
    if (s_ncmd == s_capcmd) {
        s_capcmd = s_capcmd ? s_capcmd * 2 : 4096;
        s_cmds = (GxCmd *)realloc(s_cmds, sizeof *s_cmds * s_capcmd);
    }
    memset(&s_cmds[s_ncmd], 0, sizeof s_cmds[0]);
    s_cmds[s_ncmd].type = type;
    return &s_cmds[s_ncmd++];
}

static uint32_t arena_add(uint8_t **a, uint32_t *n, uint32_t *cap, const void *data, uint32_t len, uint32_t align)
{
    uint32_t off = (*n + align - 1) & ~(align - 1);
    if (off + len > *cap) {
        uint32_t c = *cap ? *cap : 1u << 20;
        while (off + len > c) c *= 2;
        *a = (uint8_t *)realloc(*a, c);
        *cap = c;
    }
    if (data) memcpy(*a + off, data, len);
    *n = off + len;
    return off;
}

/* A texture upload: the rows go into the upload arena now (D3D12 wants
 * 256-byte row pitch and 512-byte offsets; one layout suits every backend). */
static void tex_upload(GxTex *t, uint32_t layer, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const void *data, uint32_t pitch)
{
    uint32_t rows = (h + t->block - 1) / t->block, cols = (w + t->block - 1) / t->block;
    uint32_t rowbytes = cols * t->unit, dpitch = (rowbytes + 255u) & ~255u, r, off;
    GxCmd *c;
    if (!t->t || !w || !h) return;
    off = arena_add(&s_up, &s_nup, &s_capup, NULL, dpitch * rows, 512);
    for (r = 0; r < rows; r++)
        memcpy(s_up + off + (size_t)r * dpitch, (const uint8_t *)data + (size_t)r * pitch, rowbytes);
    c = cmd_add(C_UPLOAD);
    c->u.up.t = t->t; c->u.up.layer = layer; c->u.up.mip = mip;
    c->u.up.x = x; c->u.up.y = y; c->u.up.w = w; c->u.up.h = h;
    c->u.up.off = off; c->u.up.ppr = dpitch / t->unit * t->block; c->u.up.rows = rows * t->block;
}

/* ---- pipelines ----------------------------------------------------------- */

typedef struct {
    uint32_t vs, ps;
    uint32_t topo;
    D3D11_RENDER_TARGET_BLEND_DESC b;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rs;
    uint32_t cfmt, dfmt, has_rt, has_ds;
} GxPipeKey;

#define PIPE_CACHE 4096
static struct { GxPipeKey k; uint32_t h; SDL_GPUGraphicsPipeline *p; } s_pipes[PIPE_CACHE];
static unsigned s_npipes;

static uint32_t key_hash(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h ? h : 1;
}

static SDL_GPUBlendFactor map_blend(D3D11_BLEND b)
{
    switch (b) {
    case D3D11_BLEND_ZERO:             return SDL_GPU_BLENDFACTOR_ZERO;
    case D3D11_BLEND_ONE:              return SDL_GPU_BLENDFACTOR_ONE;
    case D3D11_BLEND_SRC_COLOR:        return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case D3D11_BLEND_INV_SRC_COLOR:    return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case D3D11_BLEND_SRC_ALPHA:        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_ALPHA:    return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D11_BLEND_DEST_ALPHA:       return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case D3D11_BLEND_INV_DEST_ALPHA:   return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case D3D11_BLEND_DEST_COLOR:       return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case D3D11_BLEND_INV_DEST_COLOR:   return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case D3D11_BLEND_SRC_ALPHA_SAT:    return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case D3D11_BLEND_BLEND_FACTOR:     return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case D3D11_BLEND_INV_BLEND_FACTOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    default:                           return SDL_GPU_BLENDFACTOR_ONE;
    }
}

static SDL_GPUBlendOp map_blendop(D3D11_BLEND_OP o)
{
    switch (o) {
    case D3D11_BLEND_OP_SUBTRACT:     return SDL_GPU_BLENDOP_SUBTRACT;
    case D3D11_BLEND_OP_REV_SUBTRACT: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case D3D11_BLEND_OP_MIN:          return SDL_GPU_BLENDOP_MIN;
    case D3D11_BLEND_OP_MAX:          return SDL_GPU_BLENDOP_MAX;
    default:                          return SDL_GPU_BLENDOP_ADD;
    }
}

static SDL_GPUCompareOp map_cmp(D3D11_COMPARISON_FUNC f)
{
    switch (f) {
    case D3D11_COMPARISON_NEVER:         return SDL_GPU_COMPAREOP_NEVER;
    case D3D11_COMPARISON_LESS:          return SDL_GPU_COMPAREOP_LESS;
    case D3D11_COMPARISON_EQUAL:         return SDL_GPU_COMPAREOP_EQUAL;
    case D3D11_COMPARISON_LESS_EQUAL:    return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case D3D11_COMPARISON_GREATER:       return SDL_GPU_COMPAREOP_GREATER;
    case D3D11_COMPARISON_NOT_EQUAL:     return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case D3D11_COMPARISON_GREATER_EQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default:                             return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

static SDL_GPUStencilOp map_sop(D3D11_STENCIL_OP o)
{
    switch (o) {
    case D3D11_STENCIL_OP_ZERO:     return SDL_GPU_STENCILOP_ZERO;
    case D3D11_STENCIL_OP_REPLACE:  return SDL_GPU_STENCILOP_REPLACE;
    case D3D11_STENCIL_OP_INCR_SAT: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
    case D3D11_STENCIL_OP_DECR_SAT: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case D3D11_STENCIL_OP_INVERT:   return SDL_GPU_STENCILOP_INVERT;
    case D3D11_STENCIL_OP_INCR:     return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case D3D11_STENCIL_OP_DECR:     return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
    default:                        return SDL_GPU_STENCILOP_KEEP;
    }
}

static void stencil_face(const D3D11_DEPTH_STENCILOP_DESC *s, SDL_GPUStencilOpState *o)
{
    o->fail_op = map_sop(s->StencilFailOp);
    o->depth_fail_op = map_sop(s->StencilDepthFailOp);
    o->pass_op = map_sop(s->StencilPassOp);
    o->compare_op = map_cmp(s->StencilFunc);
}

static SDL_GPUGraphicsPipeline *pipe_get(GxShader *vs, GxShader *ps, D3D11_PRIMITIVE_TOPOLOGY topo,
                                         const GxBlend *bl, const GxDss *dss, const GxRs *rs,
                                         const GxTex *rt, const GxTex *ds)
{
    GxPipeKey k;
    uint32_t h, i;
    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_GPUColorTargetDescription ct;
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.ps = ps->id; k.topo = (uint32_t)topo;
    if (bl) k.b = bl->d.RenderTarget[0];
    else { k.b.SrcBlend = k.b.SrcBlendAlpha = D3D11_BLEND_ONE; k.b.DestBlend = k.b.DestBlendAlpha = D3D11_BLEND_ZERO;
           k.b.BlendOp = k.b.BlendOpAlpha = D3D11_BLEND_OP_ADD; k.b.RenderTargetWriteMask = 15; }
    if (ds) {
        if (dss) k.ds = dss->d;
        else { k.ds.DepthEnable = TRUE; k.ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; k.ds.DepthFunc = D3D11_COMPARISON_LESS; }
    }
    if (rs) k.rs = rs->d;
    else { k.rs.FillMode = D3D11_FILL_SOLID; k.rs.CullMode = D3D11_CULL_BACK; k.rs.DepthClipEnable = TRUE; }
    k.has_rt = rt != NULL; k.cfmt = rt ? (uint32_t)rt->fmt : 0;
    k.has_ds = ds != NULL; k.dfmt = ds ? (uint32_t)ds->fmt : 0;
    h = key_hash(&k, sizeof k);
    for (i = 0; i < PIPE_CACHE; i++) {
        uint32_t s = (h + i) & (PIPE_CACHE - 1);
        if (!s_pipes[s].p) break;
        if (s_pipes[s].h == h && !memcmp(&s_pipes[s].k, &k, sizeof k)) return s_pipes[s].p;
    }
    memset(&ci, 0, sizeof ci);
    memset(&ct, 0, sizeof ct);
    ci.vertex_shader = vs->sh;
    ci.fragment_shader = ps->sh;
    ci.primitive_type = topo == D3D11_PRIMITIVE_TOPOLOGY_LINELIST ? SDL_GPU_PRIMITIVETYPE_LINELIST
                      : topo == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP ? SDL_GPU_PRIMITIVETYPE_LINESTRIP
                      : topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ? SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP
                      : topo == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST ? SDL_GPU_PRIMITIVETYPE_POINTLIST
                      : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = k.rs.FillMode == D3D11_FILL_WIREFRAME ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = k.rs.CullMode == D3D11_CULL_FRONT ? SDL_GPU_CULLMODE_FRONT
                                  : k.rs.CullMode == D3D11_CULL_BACK ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = k.rs.FrontCounterClockwise ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE
                                                                 : SDL_GPU_FRONTFACE_CLOCKWISE;
    ci.rasterizer_state.enable_depth_clip = k.rs.DepthClipEnable ? true : false;
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (ds) {
        ci.depth_stencil_state.enable_depth_test = k.ds.DepthEnable ? true : false;
        ci.depth_stencil_state.enable_depth_write = k.ds.DepthEnable && k.ds.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        ci.depth_stencil_state.compare_op = k.ds.DepthEnable ? map_cmp(k.ds.DepthFunc) : SDL_GPU_COMPAREOP_ALWAYS;
        ci.depth_stencil_state.enable_stencil_test = k.ds.StencilEnable ? true : false;
        stencil_face(&k.ds.FrontFace, &ci.depth_stencil_state.front_stencil_state);
        stencil_face(&k.ds.BackFace, &ci.depth_stencil_state.back_stencil_state);
        ci.depth_stencil_state.compare_mask = k.ds.StencilReadMask;
        ci.depth_stencil_state.write_mask = k.ds.StencilWriteMask;
        ci.target_info.has_depth_stencil_target = true;
        ci.target_info.depth_stencil_format = ds->fmt;
    }
    if (rt) {
        ct.format = rt->fmt;
        ct.blend_state.enable_blend = k.b.BlendEnable ? true : false;
        ct.blend_state.src_color_blendfactor = map_blend(k.b.SrcBlend);
        ct.blend_state.dst_color_blendfactor = map_blend(k.b.DestBlend);
        ct.blend_state.color_blend_op = map_blendop(k.b.BlendOp);
        ct.blend_state.src_alpha_blendfactor = map_blend(k.b.SrcBlendAlpha);
        ct.blend_state.dst_alpha_blendfactor = map_blend(k.b.DestBlendAlpha);
        ct.blend_state.alpha_blend_op = map_blendop(k.b.BlendOpAlpha);
        ct.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)(k.b.RenderTargetWriteMask & 15);
        ct.blend_state.enable_color_write_mask = true;
        ci.target_info.color_target_descriptions = &ct;
        ci.target_info.num_color_targets = 1;
    }
    {
        static unsigned created;
        LARGE_INTEGER a, b, fq;
        SDL_GPUGraphicsPipeline *p;
        QueryPerformanceCounter(&a);
        p = SDL_CreateGPUGraphicsPipeline(G.dev, &ci);
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&fq);
        s_st.pipe_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart;
        if (!p) {
            static int told;
            if (told++ < 8) fprintf(stderr, "  [GX] pipeline: %s\n", SDL_GetError());
            return NULL;
        }
        created++;
        if (created <= 3 || created % 200 == 0 || (b.QuadPart - a.QuadPart) * 1000 / fq.QuadPart > 30)
            fprintf(stderr, "  [GX] %u pipelines (last took %.1f ms)\n", created,
                    (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart);
        if (s_npipes >= PIPE_CACHE * 3 / 4) {          /* full: start over (rare) */
            for (i = 0; i < PIPE_CACHE; i++)
                if (s_pipes[i].p) SDL_ReleaseGPUGraphicsPipeline(G.dev, s_pipes[i].p);
            memset(s_pipes, 0, sizeof s_pipes);
            s_npipes = 0;
        }
        for (i = 0; i < PIPE_CACHE; i++) {
            uint32_t s = (h + i) & (PIPE_CACHE - 1);
            if (!s_pipes[s].p) { s_pipes[s].k = k; s_pipes[s].h = h; s_pipes[s].p = p; s_npipes++; break; }
        }
        return p;
    }
}

/* ---- context state ------------------------------------------------------ */

static struct {
    GxView *rtv, *dsv;
    GxShader *vs, *ps;
    GxBlend *blend; float bf[4];
    GxDss *dss; UINT sref;
    GxRs *rs;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc[8]; UINT nsc;
    GxView *srv[16];
    GxSmp *smp[16];
    GxBuf *pscb[4], *vscb[4];
    GxBuf *pull_buf[2]; UINT pull_first, pull_tri, pull_stride; float pull_w;
    GxBuf *last_vs_buf[4]; uint32_t last_vs_ver[4]; int32_t last_vs_off[4];
    /* uniform dedupe: the last data recorded per stage */
    GxBuf *last_fs_buf; uint32_t last_fs_ver; int32_t last_fs_off;
} C;

void gxs_ID3D11DeviceContext_OMSetRenderTargets(void *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv)
{
    (void)c;
    ref_set((void **)&C.rtv, n && rtv ? (void *)rtv[0] : NULL);
    ref_set((void **)&C.dsv, (void *)dsv);
}

void gxs_ID3D11DeviceContext_OMSetBlendState(void *c, ID3D11BlendState *b, const FLOAT f[4], UINT mask)
{
    (void)c; (void)mask;
    ref_set((void **)&C.blend, b);
    if (f) memcpy(C.bf, f, sizeof C.bf);
    else C.bf[0] = C.bf[1] = C.bf[2] = C.bf[3] = 1.0f;
}

void gxs_ID3D11DeviceContext_OMSetDepthStencilState(void *c, ID3D11DepthStencilState *d, UINT ref)
{
    (void)c;
    ref_set((void **)&C.dss, d);
    C.sref = ref;
}

void gxs_ID3D11DeviceContext_RSSetState(void *c, ID3D11RasterizerState *s) { (void)c; ref_set((void **)&C.rs, s); }
void gxs_ID3D11DeviceContext_IASetPrimitiveTopology(void *c, D3D11_PRIMITIVE_TOPOLOGY t) { (void)c; C.topo = t; }
void gxs_ID3D11DeviceContext_IASetInputLayout(void *c, ID3D11InputLayout *l) { (void)c; (void)l; }
void gxs_ID3D11DeviceContext_IASetVertexBuffers(void *c, UINT start, UINT n, ID3D11Buffer *const *b, const UINT *strides, const UINT *offs)
{
    (void)c; (void)start; (void)n; (void)b; (void)strides; (void)offs;     /* vertices are pulled (gxs_set_pull) */
}
void gxs_ID3D11DeviceContext_IASetIndexBuffer(void *c, ID3D11Buffer *b, DXGI_FORMAT f, UINT off) { (void)c; (void)b; (void)f; (void)off; }
void gxs_ID3D11DeviceContext_GSSetShader(void *c, ID3D11GeometryShader *s, ID3D11ClassInstance *const *ci, UINT n) { (void)c; (void)s; (void)ci; (void)n; }
void gxs_ID3D11DeviceContext_GSSetConstantBuffers(void *c, UINT start, UINT n, ID3D11Buffer *const *b) { (void)c; (void)start; (void)n; (void)b; }
void gxs_ID3D11DeviceContext_VSSetShaderResources(void *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v) { (void)c; (void)start; (void)n; (void)v; }

void gxs_ID3D11DeviceContext_VSSetShader(void *c, ID3D11VertexShader *s, ID3D11ClassInstance *const *ci, UINT n)
{
    (void)c; (void)ci; (void)n;
    ref_set((void **)&C.vs, s);
}

void gxs_ID3D11DeviceContext_PSSetShader(void *c, ID3D11PixelShader *s, ID3D11ClassInstance *const *ci, UINT n)
{
    (void)c; (void)ci; (void)n;
    ref_set((void **)&C.ps, s);
}

void gxs_ID3D11DeviceContext_PSSetShaderResources(void *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v)
{
    UINT i;
    (void)c;
    for (i = 0; i < n && start + i < 16; i++)
        ref_set((void **)&C.srv[start + i], v ? (void *)v[i] : NULL);
}

void gxs_ID3D11DeviceContext_PSSetSamplers(void *c, UINT start, UINT n, ID3D11SamplerState *const *s)
{
    UINT i;
    (void)c;
    for (i = 0; i < n && start + i < 16; i++)
        ref_set((void **)&C.smp[start + i], s ? (void *)s[i] : NULL);
}

void gxs_ID3D11DeviceContext_PSSetConstantBuffers(void *c, UINT start, UINT n, ID3D11Buffer *const *b)
{
    UINT i;
    (void)c;
    for (i = 0; i < n && start + i < 4; i++)
        ref_set((void **)&C.pscb[start + i], b ? (void *)b[i] : NULL);
}

void gxs_ID3D11DeviceContext_VSSetConstantBuffers(void *c, UINT start, UINT n, ID3D11Buffer *const *b)
{
    UINT i;
    (void)c;
    for (i = 0; i < n && start + i < 4; i++)
        ref_set((void **)&C.vscb[start + i], b ? (void *)b[i] : NULL);
}

void gxs_ID3D11DeviceContext_RSSetViewports(void *c, UINT n, const D3D11_VIEWPORT *v)
{
    (void)c;
    if (n && v) C.vp = v[0];
}

void gxs_ID3D11DeviceContext_RSSetScissorRects(void *c, UINT n, const D3D11_RECT *r)
{
    (void)c;
    if (n > 8) n = 8;
    C.nsc = n;
    if (n) memcpy(C.sc, r, n * sizeof *r);
}

void gxs_set_pull2(ID3D11Buffer *b0, ID3D11Buffer *b1, UINT first, UINT tri_mode, float w_buffer, UINT stride)
{
    ref_set((void **)&C.pull_buf[0], b0);
    ref_set((void **)&C.pull_buf[1], b1);
    C.pull_first = first; C.pull_tri = tri_mode; C.pull_w = w_buffer; C.pull_stride = stride;
}

void gxs_set_pull(ID3D11Buffer *vb, UINT first, UINT tri_mode, float w_buffer)
{
    gxs_set_pull2(vb, NULL, first, tri_mode, w_buffer, 144);
}

static void note_vbuf(GxBuf *b, uint32_t lo, uint32_t hi);

void gxs_buffer_written(ID3D11Buffer *buf, UINT lo, UINT hi)
{
    GxBuf *b = (GxBuf *)buf;
    if (b && b->o.kind == K_BUF && b->b && hi > lo)
        note_vbuf(b, lo, hi);
}

/* ---- flushing ----------------------------------------------------------- */

static void flush(int submit);

static void note_vbuf(GxBuf *b, uint32_t lo, uint32_t hi)
{
    int i;
    if (lo < b->lo) b->lo = lo;
    if (hi > b->hi) b->hi = hi;
    for (i = 0; i < s_nvbufs; i++)
        if (s_vbufs[i] == b) return;
    if (s_nvbufs == 16) return;               /* the renderer uses one or two */
    gxs_obj_AddRef(b);
    s_vbufs[s_nvbufs++] = b;
}

void gxs_ID3D11DeviceContext_Draw(void *c, UINT n, UINT start)
{
    GxDraw *d;
    GxTex *rt = C.rtv ? C.rtv->tex : NULL, *ds = C.dsv ? C.dsv->tex : NULL;
    SDL_GPUGraphicsPipeline *pipe;
    uint32_t i, tw, th;
    int pull;
    (void)c;
    if (!n || !C.vs || !C.ps || (!rt && !ds))
        return;
    pull = C.vs->r.nstorage > 0;
    for (i = 0; pull && i < C.vs->r.nstorage && i < 2; i++)
        if (!C.pull_buf[i] || !C.pull_buf[i]->b)
            return;
    pipe = pipe_get(C.vs, C.ps, C.topo, C.blend, C.dss, C.rs, rt, ds);
    if (!pipe)
        return;
    if (s_ndraw == s_capdraw) {
        s_capdraw = s_capdraw ? s_capdraw * 2 : 4096;
        s_draws = (GxDraw *)realloc(s_draws, sizeof *s_draws * s_capdraw);
    }
    d = &s_draws[s_ndraw];
    memset(d, 0, sizeof *d);
    d->pipe = pipe;
    d->rt = rt ? rt->t : NULL;
    d->ds = ds ? ds->t : NULL;
    tw = rt ? rt->d.Width : ds->d.Width;
    th = rt ? rt->d.Height : ds->d.Height;
    d->nfs = (uint8_t)(C.ps->r.nsamplers > 8 ? 8 : C.ps->r.nsamplers);
    for (i = 0; i < d->nfs; i++) {
        GxView *v = C.srv[C.ps->pairs[i][0] & 15];
        GxSmp *s = C.smp[C.ps->pairs[i][1] & 15];
        d->fs[i].texture = v && v->tex ? v->tex->t : ((C.ps->r.cube_mask >> i) & 1) ? G.white_cube : G.white;
        d->fs[i].sampler = s ? s->s : G.point;
    }
    d->fsu = d->vsu[0] = d->vsu[1] = d->vsu[2] = d->vsu[3] = -1;
    if (pull) {
        uint32_t u[4];
        float w = C.pull_w;
        d->nvsbuf = (uint8_t)(C.vs->r.nstorage < 2 ? C.vs->r.nstorage : 2);
        for (i = 0; i < d->nvsbuf; i++)
            d->vsbuf[i] = C.pull_buf[i]->b;
        /* Vertex uniform slot 0: where this draw's vertices are, and how to
         * treat them; slots 1-3: the vertex constant buffers bound there. */
        u[0] = C.pull_first; u[1] = C.pull_tri; memcpy(&u[2], &w, 4); u[3] = 0;
        d->vsu[0] = (int32_t)arena_add(&s_uni, &s_nuni, &s_capuni, u, sizeof u, 16);
        d->vsu_len[0] = sizeof u;
        if (C.pull_stride)
            note_vbuf(C.pull_buf[0], C.pull_first * C.pull_stride, (C.pull_first + n) * C.pull_stride);
    }
    d->nvsu = (uint8_t)(C.vs->r.nuniform < 4 ? C.vs->r.nuniform : 4);
    for (i = 1; i < d->nvsu; i++) {
        GxBuf *b = C.vscb[i];
        d->vsu[i] = -1;
        if (!b) continue;
        if (C.last_vs_buf[i] == b && C.last_vs_ver[i] == b->version && C.last_vs_off[i] >= 0)
            d->vsu[i] = C.last_vs_off[i];
        else {
            d->vsu[i] = (int32_t)arena_add(&s_uni, &s_nuni, &s_capuni, b->cpu, b->d.ByteWidth, 16);
            C.last_vs_buf[i] = b; C.last_vs_ver[i] = b->version; C.last_vs_off[i] = d->vsu[i];
        }
        d->vsu_len[i] = (uint16_t)b->d.ByteWidth;
    }
    if (C.ps->r.nuniform && C.pscb[0]) {
        GxBuf *b = C.pscb[0];
        if (C.last_fs_buf == b && C.last_fs_ver == b->version && C.last_fs_off >= 0)
            d->fsu = C.last_fs_off;
        else {
            d->fsu = (int32_t)arena_add(&s_uni, &s_nuni, &s_capuni, b->cpu, b->d.ByteWidth, 16);
            C.last_fs_buf = b; C.last_fs_ver = b->version; C.last_fs_off = d->fsu;
        }
        d->fsu_len = (uint16_t)b->d.ByteWidth;
    }
    d->vp.x = C.vp.TopLeftX; d->vp.y = C.vp.TopLeftY; d->vp.w = C.vp.Width; d->vp.h = C.vp.Height;
    d->vp.min_depth = C.vp.MinDepth; d->vp.max_depth = C.vp.MaxDepth;
    if (d->vp.w <= 0 || d->vp.h <= 0) return;
    /* Direct3D 11 takes the union of up to 8 scissor rectangles; SDL has
     * one. Each pixel must still be drawn once (blending), so the union is
     * cut into disjoint rectangles and the draw repeated per piece. NFL 2K5
     * mostly sets one rectangle, or several that are the same. */
    {
        SDL_Rect in[8], out[8];
        int nin = 0, nout = 0, a, k2, q;
        for (i = 0; i < C.nsc; i++) {
            LONG l = C.sc[i].left < 0 ? 0 : C.sc[i].left, t = C.sc[i].top < 0 ? 0 : C.sc[i].top;
            LONG r = C.sc[i].right > (LONG)tw ? (LONG)tw : C.sc[i].right, b = C.sc[i].bottom > (LONG)th ? (LONG)th : C.sc[i].bottom;
            if (r <= l || b <= t) continue;
            in[nin].x = l; in[nin].y = t; in[nin].w = r - l; in[nin].h = b - t;
            nin++;
        }
        for (a = 0; a < nin; a++) {
            /* in[a] minus every piece already kept: at most 4 parts each */
            SDL_Rect work[32];
            int nw = 1;
            work[0] = in[a];
            for (k2 = 0; k2 < nout && nw; k2++) {
                SDL_Rect next[32];
                int nn = 0;
                const SDL_Rect *o = &out[k2];
                for (q = 0; q < nw; q++) {
                    SDL_Rect r = work[q];
                    int ox0 = o->x, oy0 = o->y, ox1 = o->x + o->w, oy1 = o->y + o->h;
                    int rx1 = r.x + r.w, ry1 = r.y + r.h;
                    if (ox1 <= r.x || ox0 >= rx1 || oy1 <= r.y || oy0 >= ry1) {
                        if (nn < 32) next[nn++] = r;
                        continue;
                    }
                    if (oy0 > r.y && nn < 32) { SDL_Rect p = { r.x, r.y, r.w, oy0 - r.y }; next[nn++] = p; }
                    if (oy1 < ry1 && nn < 32) { SDL_Rect p = { r.x, oy1, r.w, ry1 - oy1 }; next[nn++] = p; }
                    {
                        int my0 = oy0 > r.y ? oy0 : r.y, my1 = oy1 < ry1 ? oy1 : ry1;
                        if (ox0 > r.x && nn < 32) { SDL_Rect p = { r.x, my0, ox0 - r.x, my1 - my0 }; next[nn++] = p; }
                        if (ox1 < rx1 && nn < 32) { SDL_Rect p = { ox1, my0, rx1 - ox1, my1 - my0 }; next[nn++] = p; }
                    }
                }
                memcpy(work, next, sizeof next[0] * (size_t)nn);
                nw = nn;
            }
            for (q = 0; q < nw && nout < 8; q++)
                out[nout++] = work[q];
        }
        for (i = 0; i < (uint32_t)nout; i++)
            d->sc[d->nsc++] = out[i];
    }
    if (!C.nsc) { d->sc[0].x = 0; d->sc[0].y = 0; d->sc[0].w = (int)tw; d->sc[0].h = (int)th; d->nsc = 1; }
    if (!d->nsc) return;                      /* everything scissored away */
    d->bf.r = C.bf[0]; d->bf.g = C.bf[1]; d->bf.b = C.bf[2]; d->bf.a = C.bf[3];
    d->sref = (uint8_t)C.sref;
    d->n = n;
    d->first = pull ? 0 : start;
    cmd_add(C_DRAW)->u.draw = s_ndraw++;
}

void gxs_ID3D11DeviceContext_DrawIndexed(void *c, UINT n, UINT start, INT base)
{
    (void)base;                 /* indices are pulled too: where they start came with gxs_set_pull2 */
    gxs_ID3D11DeviceContext_Draw(c, n, start);
}

void gxs_ID3D11DeviceContext_ClearRenderTargetView(void *c, ID3D11RenderTargetView *v, const FLOAT col[4])
{
    GxView *vw = (GxView *)v;
    GxCmd *cm;
    (void)c;
    if (!vw || !vw->tex || !vw->tex->t) return;
    cm = cmd_add(C_CLEAR_RT);
    cm->u.clr.t = vw->tex->t;
    memcpy(cm->u.clr.c, col, sizeof cm->u.clr.c);
}

void gxs_ID3D11DeviceContext_ClearDepthStencilView(void *c, ID3D11DepthStencilView *v, UINT flags, FLOAT depth, UINT8 stencil)
{
    GxView *vw = (GxView *)v;
    GxCmd *cm;
    (void)c;
    if (!vw || !vw->tex || !vw->tex->t) return;
    cm = cmd_add(C_CLEAR_DS);
    cm->u.cds.t = vw->tex->t; cm->u.cds.flags = flags; cm->u.cds.z = depth; cm->u.cds.s = stencil;
}

static void copy_region(GxTex *dst, UINT dsub, UINT x, UINT y, GxTex *src, UINT ssub, const D3D11_BOX *box)
{
    uint32_t smips = src->d.MipLevels ? src->d.MipLevels : 1, dmips = dst->d.MipLevels ? dst->d.MipLevels : 1;
    uint32_t sm = ssub % smips, sl = ssub / smips;
    uint32_t sw = src->d.Width >> sm, sh = src->d.Height >> sm;
    uint32_t bx = box ? box->left : 0, by = box ? box->top : 0;
    uint32_t w = box ? box->right - box->left : sw, h = box ? box->bottom - box->top : sh;
    if (!src->t) return;
    if (!dst->t) {                                         /* into a staging texture: a download */
        GxCmd *cm;
        if (!dst->dl) return;
        cm = cmd_add(C_DOWNLOAD);
        cm->u.dl.t = src->t; cm->u.dl.x = bx; cm->u.dl.y = by; cm->u.dl.w = w; cm->u.dl.h = h;
        cm->u.dl.stg = dst;
        if (!dst->dl_flush) {
            if (s_ndl == s_capdl) {
                s_capdl = s_capdl ? s_capdl * 2 : 16;
                s_dl = (GxTex **)realloc(s_dl, sizeof *s_dl * (size_t)s_capdl);
            }
            gxs_obj_AddRef(dst);
            s_dl[s_ndl++] = dst;
            dst->dl_flush = 1;
        }
        return;
    }
    {
        GxCmd *cm = cmd_add(C_COPY);
        cm->u.cp.src = src->t; cm->u.cp.dst = dst->t;
        cm->u.cp.sl = sl; cm->u.cp.sm = sm; cm->u.cp.sx = bx; cm->u.cp.sy = by;
        cm->u.cp.dl = dsub / dmips; cm->u.cp.dm = dsub % dmips; cm->u.cp.dx = x; cm->u.cp.dy = y;
        cm->u.cp.w = w; cm->u.cp.h = h;
    }
}

void gxs_ID3D11DeviceContext_CopyResource(void *c, ID3D11Resource *dst, ID3D11Resource *src)
{
    GxTex *d = (GxTex *)dst, *s = (GxTex *)src;
    uint32_t k, n;
    (void)c;
    if (!d || !s || d->o.kind != K_TEX || s->o.kind != K_TEX) return;
    n = (s->d.ArraySize ? s->d.ArraySize : 1) * (s->d.MipLevels ? s->d.MipLevels : 1);
    if (!d->t) n = 1;                                      /* staging: the top level */
    for (k = 0; k < n; k++)
        copy_region(d, k, 0, 0, s, k, NULL);
}

void gxs_ID3D11DeviceContext_CopySubresourceRegion(void *c, ID3D11Resource *dst, UINT dsub, UINT x, UINT y, UINT z,
                                                   ID3D11Resource *src, UINT ssub, const D3D11_BOX *box)
{
    (void)c; (void)z;
    if (!dst || !src) return;
    copy_region((GxTex *)dst, dsub, x, y, (GxTex *)src, ssub, box);
}

void gxs_ID3D11DeviceContext_UpdateSubresource(void *c, ID3D11Resource *dst, UINT sub, const D3D11_BOX *box,
                                               const void *data, UINT row, UINT depth)
{
    GxObj *o = (GxObj *)dst;
    (void)c; (void)depth;
    if (!o || !data) return;
    if (o->kind == K_TEX) {
        GxTex *t = (GxTex *)o;
        uint32_t mips = t->d.MipLevels ? t->d.MipLevels : 1, mip = sub % mips, layer = sub / mips;
        uint32_t w = t->d.Width >> mip, h = t->d.Height >> mip, x = 0, y = 0;
        if (!w) w = 1;
        if (!h) h = 1;
        if (box) { x = box->left; y = box->top; w = box->right - box->left; h = box->bottom - box->top; }
        tex_upload(t, layer, mip, x, y, w, h, data, row);
    } else if (o->kind == K_BUF) {
        GxBuf *b = (GxBuf *)o;
        memcpy(b->cpu, data, b->d.ByteWidth);
        b->version++;
    }
}

void gxs_ID3D11DeviceContext_GenerateMips(void *c, ID3D11ShaderResourceView *v)
{
    GxView *vw = (GxView *)v;
    (void)c;
    if (vw && vw->tex && vw->tex->t)
        cmd_add(C_MIPS)->u.mips.t = vw->tex->t;
}

void gxs_ID3D11DeviceContext_Flush(void *c) { (void)c; }

HRESULT gxs_ID3D11DeviceContext_Map(void *c, ID3D11Resource *r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *m)
{
    GxObj *o = (GxObj *)r;
    (void)c; (void)sub; (void)flags;
    if (!o || !m) return E_INVALIDARG;
    if (o->kind == K_BUF) {
        GxBuf *b = (GxBuf *)o;
        /* A ring wrap: pending draws read the old contents, so they go first. */
        if (type == D3D11_MAP_WRITE_DISCARD && b->b && b->lo < b->hi)
            flush(0);
        m->pData = b->cpu; m->RowPitch = b->d.ByteWidth; m->DepthPitch = b->d.ByteWidth;
        return S_OK;
    }
    if (o->kind == K_TEX) {
        GxTex *t = (GxTex *)o;
        if (t->dl) {                                       /* staging read-back */
            void *p;
            if (t->dl_flush) flush(1);                      /* its download is still in the list */
            if (t->dl_fence) {
                double w0 = qpc_ms();
                SDL_WaitForGPUFences(G.dev, true, &t->dl_fence->f, 1);
                s_st.waits++;
                s_st.wait_ms += qpc_ms() - w0;
                fence_release(t->dl_fence);
                t->dl_fence = NULL;
            }
            p = SDL_MapGPUTransferBuffer(G.dev, t->dl, false);
            if (!p) return E_FAIL;
            t->dl_mapped = 1;
            m->pData = p;
            m->RowPitch = (t->d.Width + t->block - 1) / t->block * t->unit;
            m->DepthPitch = m->RowPitch * t->d.Height;
            return S_OK;
        }
        if (t->dyn) {
            m->pData = t->dyn;
            m->RowPitch = (t->d.Width + t->block - 1) / t->block * t->unit;
            m->DepthPitch = m->RowPitch * t->d.Height;
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

void gxs_ID3D11DeviceContext_Unmap(void *c, ID3D11Resource *r, UINT sub)
{
    GxObj *o = (GxObj *)r;
    (void)c; (void)sub;
    if (!o) return;
    if (o->kind == K_BUF) {
        ((GxBuf *)o)->version++;
    } else if (o->kind == K_TEX) {
        GxTex *t = (GxTex *)o;
        if (t->dl && t->dl_mapped) {
            SDL_UnmapGPUTransferBuffer(G.dev, t->dl);
            t->dl_mapped = 0;
        } else if (t->dyn) {
            tex_upload(t, 0, 0, 0, 0, t->d.Width, t->d.Height, t->dyn, (t->d.Width + t->block - 1) / t->block * t->unit);
        }
    }
}

/* ---- replay: the list into SDL passes -------------------------------------- */

typedef struct { SDL_GPUTexture *t; int ds; float c[4]; uint32_t flags; float z; uint8_t s; } GxClear;
static GxClear s_clears[16];
static int s_nclears;

static void replay_clear_alone(SDL_GPUCommandBuffer *cb, const GxClear *k)
{
    SDL_GPURenderPass *rp;
    if (!k->ds) {
        SDL_GPUColorTargetInfo ct;
        memset(&ct, 0, sizeof ct);
        ct.texture = k->t; ct.load_op = SDL_GPU_LOADOP_CLEAR; ct.store_op = SDL_GPU_STOREOP_STORE;
        ct.clear_color.r = k->c[0]; ct.clear_color.g = k->c[1]; ct.clear_color.b = k->c[2]; ct.clear_color.a = k->c[3];
        rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
    } else {
        SDL_GPUDepthStencilTargetInfo dt;
        memset(&dt, 0, sizeof dt);
        dt.texture = k->t;
        dt.load_op = (k->flags & D3D11_CLEAR_DEPTH) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.stencil_load_op = (k->flags & D3D11_CLEAR_STENCIL) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.store_op = dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
        dt.clear_depth = k->z; dt.clear_stencil = k->s;
        rp = SDL_BeginGPURenderPass(cb, NULL, 0, &dt);
    }
    if (rp) SDL_EndGPURenderPass(rp);
}

static void clears_flush(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *keep_rt, SDL_GPUTexture *keep_ds)
{
    int i, n = 0;
    for (i = 0; i < s_nclears; i++) {
        if ((!s_clears[i].ds && s_clears[i].t == keep_rt) || (s_clears[i].ds && s_clears[i].t == keep_ds))
            s_clears[n++] = s_clears[i];               /* merged into the pass about to begin */
        else
            replay_clear_alone(cb, &s_clears[i]);
    }
    s_nclears = n;
}

static void clear_add(const GxClear *k)
{
    int i;
    for (i = 0; i < s_nclears; i++)
        if (s_clears[i].t == k->t) {
            if (k->ds) {
                if (k->flags & D3D11_CLEAR_DEPTH) s_clears[i].z = k->z;
                if (k->flags & D3D11_CLEAR_STENCIL) s_clears[i].s = k->s;
                s_clears[i].flags |= k->flags;
            } else memcpy(s_clears[i].c, k->c, sizeof k->c);
            return;
        }
    if (s_nclears < 16) s_clears[s_nclears++] = *k;
}

static SDL_GPURenderPass *begin_pass(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *rt, SDL_GPUTexture *ds)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUDepthStencilTargetInfo dt;
    int i;
    clears_flush(cb, rt, ds);
    memset(&ct, 0, sizeof ct);
    memset(&dt, 0, sizeof dt);
    ct.texture = rt; ct.load_op = SDL_GPU_LOADOP_LOAD; ct.store_op = SDL_GPU_STOREOP_STORE;
    dt.texture = ds; dt.load_op = dt.stencil_load_op = SDL_GPU_LOADOP_LOAD;
    dt.store_op = dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
    for (i = 0; i < s_nclears; i++) {
        const GxClear *k = &s_clears[i];
        if (!k->ds && k->t == rt) {
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.clear_color.r = k->c[0]; ct.clear_color.g = k->c[1]; ct.clear_color.b = k->c[2]; ct.clear_color.a = k->c[3];
        } else if (k->ds && k->t == ds) {
            if (k->flags & D3D11_CLEAR_DEPTH) { dt.load_op = SDL_GPU_LOADOP_CLEAR; dt.clear_depth = k->z; }
            if (k->flags & D3D11_CLEAR_STENCIL) { dt.stencil_load_op = SDL_GPU_LOADOP_CLEAR; dt.clear_stencil = k->s; }
        }
    }
    s_nclears = 0;
    return SDL_BeginGPURenderPass(cb, rt ? &ct : NULL, rt ? 1 : 0, ds ? &dt : NULL);
}

static void replay(SDL_GPUCommandBuffer *cb, uint32_t up_base)
{
    SDL_GPURenderPass *rp = NULL;
    SDL_GPUCopyPass *cp = NULL;
    SDL_GPUTexture *cur_rt = NULL, *cur_ds = NULL;
    SDL_GPUGraphicsPipeline *bound = NULL;
    int32_t last_vsu[4] = { -2, -2, -2, -2 }, last_fsu = -2;
    uint32_t i, k;
#define END_RP() do { if (rp) { SDL_EndGPURenderPass(rp); rp = NULL; } } while (0)
#define END_CP() do { if (cp) { SDL_EndGPUCopyPass(cp); cp = NULL; } } while (0)
    for (i = 0; i < s_ncmd; i++) {
        const GxCmd *c = &s_cmds[i];
        switch (c->type) {
        case C_DRAW: {
            const GxDraw *d = &s_draws[c->u.draw];
            if (!rp || d->rt != cur_rt || d->ds != cur_ds) {
                END_RP(); END_CP();
                rp = begin_pass(cb, d->rt, d->ds);
                s_st.rpasses++;
                cur_rt = d->rt; cur_ds = d->ds;
                bound = NULL;
                if (!rp) break;
            }
            if (bound != d->pipe) { SDL_BindGPUGraphicsPipeline(rp, d->pipe); bound = d->pipe; }
            SDL_SetGPUViewport(rp, &d->vp);
            SDL_SetGPUBlendConstants(rp, d->bf);
            SDL_SetGPUStencilReference(rp, d->sref);
            if (d->nfs) SDL_BindGPUFragmentSamplers(rp, 0, d->fs, d->nfs);
            if (d->nvsbuf) SDL_BindGPUVertexStorageBuffers(rp, 0, d->vsbuf, d->nvsbuf);
            for (k = 0; k < d->nvsu; k++)
                if (d->vsu[k] >= 0 && d->vsu[k] != last_vsu[k]) {
                    SDL_PushGPUVertexUniformData(cb, k, s_uni + d->vsu[k], d->vsu_len[k]);
                    last_vsu[k] = d->vsu[k];
                }
            if (d->fsu >= 0 && d->fsu != last_fsu) {
                SDL_PushGPUFragmentUniformData(cb, 0, s_uni + d->fsu, d->fsu_len);
                last_fsu = d->fsu;
            }
            for (k = 0; k < d->nsc; k++) {
                SDL_SetGPUScissor(rp, &d->sc[k]);
                SDL_DrawGPUPrimitives(rp, d->n, 1, d->first, 0);
                s_st.draws++;
            }
            break;
        }
        case C_CLEAR_RT: {
            GxClear k2;
            memset(&k2, 0, sizeof k2);
            END_RP();
            k2.t = c->u.clr.t; memcpy(k2.c, c->u.clr.c, sizeof k2.c);
            clear_add(&k2);
            break;
        }
        case C_CLEAR_DS: {
            GxClear k2;
            memset(&k2, 0, sizeof k2);
            END_RP();
            k2.t = c->u.cds.t; k2.ds = 1; k2.flags = c->u.cds.flags; k2.z = c->u.cds.z; k2.s = c->u.cds.s;
            clear_add(&k2);
            break;
        }
        case C_MIPS:
            END_RP(); END_CP();
            clears_flush(cb, NULL, NULL);
            SDL_GenerateMipmapsForGPUTexture(cb, c->u.mips.t);
            break;
        default:
            END_RP();
            if (s_nclears) { END_CP(); clears_flush(cb, NULL, NULL); }
            if (!cp) { cp = SDL_BeginGPUCopyPass(cb); s_st.cpasses++; }
            if (!cp) break;
            if (c->type == C_COPY) {
                SDL_GPUTextureLocation s, d;
                memset(&s, 0, sizeof s); memset(&d, 0, sizeof d);
                s.texture = c->u.cp.src; s.layer = c->u.cp.sl; s.mip_level = c->u.cp.sm; s.x = c->u.cp.sx; s.y = c->u.cp.sy;
                d.texture = c->u.cp.dst; d.layer = c->u.cp.dl; d.mip_level = c->u.cp.dm; d.x = c->u.cp.dx; d.y = c->u.cp.dy;
                SDL_CopyGPUTextureToTexture(cp, &s, &d, c->u.cp.w, c->u.cp.h, 1, false);
                s_st.copies++;
            } else if (c->type == C_UPLOAD) {
                SDL_GPUTextureTransferInfo src;
                SDL_GPUTextureRegion dst;
                memset(&src, 0, sizeof src); memset(&dst, 0, sizeof dst);
                src.transfer_buffer = s_tb; src.offset = up_base + c->u.up.off;
                src.pixels_per_row = c->u.up.ppr; src.rows_per_layer = c->u.up.rows;
                dst.texture = c->u.up.t; dst.layer = c->u.up.layer; dst.mip_level = c->u.up.mip;
                dst.x = c->u.up.x; dst.y = c->u.up.y; dst.w = c->u.up.w; dst.h = c->u.up.h; dst.d = 1;
                SDL_UploadToGPUTexture(cp, &src, &dst, false);
                s_st.uploads++;
            } else if (c->type == C_DOWNLOAD) {
                SDL_GPUTextureRegion src;
                SDL_GPUTextureTransferInfo dst;
                memset(&src, 0, sizeof src); memset(&dst, 0, sizeof dst);
                src.texture = c->u.dl.t; src.x = c->u.dl.x; src.y = c->u.dl.y; src.w = c->u.dl.w; src.h = c->u.dl.h; src.d = 1;
                dst.transfer_buffer = c->u.dl.stg->dl;
                SDL_DownloadFromGPUTexture(cp, &src, &dst);
                s_st.downloads++;
            }
            break;
        }
    }
    END_RP(); END_CP();
    clears_flush(cb, NULL, NULL);
#undef END_RP
#undef END_CP
}

/* Record everything listed into the command buffer; with submit, also send it
 * (and keep its fence for the read-backs it carries). */
static void flush(int submit)
{
    uint32_t total, vb_off[16], i, up_base = 0;
    double t0 = qpc_ms();
    if (!G.dev) return;
    s_st.flushes++;
    if (!s_cb) s_cb = SDL_AcquireGPUCommandBuffer(G.dev);
    if (!s_cb) return;
    if (s_ncmd || s_nvbufs) {
        /* One staging area: texture uploads, then each vertex buffer's range. */
        total = (s_nup + 511u) & ~511u;
        for (i = 0; i < (uint32_t)s_nvbufs; i++) {
            GxBuf *b = s_vbufs[i];
            vb_off[i] = total;
            if (b->hi > b->d.ByteWidth) b->hi = b->d.ByteWidth;
            if (b->lo < b->hi) total += (b->hi - b->lo + 255u) & ~255u;
        }
        if (total) {
            uint8_t *p;
            if (total > s_tb_size) {
                SDL_GPUTransferBufferCreateInfo ti;
                if (s_tb) SDL_ReleaseGPUTransferBuffer(G.dev, s_tb);
                memset(&ti, 0, sizeof ti);
                ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                s_tb_size = total + total / 2 + (1u << 20);
                ti.size = s_tb_size;
                s_tb = SDL_CreateGPUTransferBuffer(G.dev, &ti);
                if (!s_tb) { s_tb_size = 0; fprintf(stderr, "  [GX] upload buffer: %s\n", SDL_GetError()); }
            }
            p = s_tb ? (uint8_t *)SDL_MapGPUTransferBuffer(G.dev, s_tb, true) : NULL;
            if (p) {
                if (s_nup) memcpy(p + up_base, s_up, s_nup);
                for (i = 0; i < (uint32_t)s_nvbufs; i++) {
                    GxBuf *b = s_vbufs[i];
                    if (b->lo < b->hi) memcpy(p + vb_off[i], b->cpu + b->lo, b->hi - b->lo);
                }
                SDL_UnmapGPUTransferBuffer(G.dev, s_tb);
                if (s_nvbufs) {
                    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(s_cb);
                    for (i = 0; i < (uint32_t)s_nvbufs && cp; i++) {
                        GxBuf *b = s_vbufs[i];
                        SDL_GPUTransferBufferLocation src;
                        SDL_GPUBufferRegion dst;
                        if (b->lo >= b->hi || !b->b) continue;
                        src.transfer_buffer = s_tb; src.offset = vb_off[i];
                        dst.buffer = b->b; dst.offset = b->lo; dst.size = b->hi - b->lo;
                        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
                    }
                    if (cp) SDL_EndGPUCopyPass(cp);
                }
            }
        }
        replay(s_cb, up_base);
    }
    for (i = 0; i < (uint32_t)s_nvbufs; i++) {
        s_vbufs[i]->lo = 0xFFFFFFFFu; s_vbufs[i]->hi = 0;
        gxs_obj_Release(s_vbufs[i]);
    }
    s_nvbufs = 0;
    /* Stagings downloaded into since the last submission: their data lands
     * with this one. Recorded but unsubmitted ones wait for the next. */
    if (submit) {
        GxFence *fence = NULL;
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(s_cb);
        s_st.submits++;
        s_cb = NULL;
        if (f) {
            fence = (GxFence *)calloc(1, sizeof *fence);
            if (fence) { fence->f = f; fence->refs = 1; }
            else SDL_ReleaseGPUFence(G.dev, f);
        }
        for (i = 0; i < (uint32_t)s_ndl; i++) {
            GxTex *t = s_dl[i];
            t->dl_flush = 0;
            if (fence) {
                fence_release(t->dl_fence);
                InterlockedIncrement(&fence->refs);
                t->dl_fence = fence;
            }
            gxs_obj_Release(t);
        }
        s_ndl = 0;
        if (fence) fence_release(fence);
    }
    s_ncmd = s_ndraw = s_nuni = s_nup = 0;
    C.last_fs_buf = NULL; C.last_fs_off = -1;
    memset(C.last_vs_buf, 0, sizeof C.last_vs_buf);
    run_releases();
    s_flush_serial++;
    s_st.flush_ms += qpc_ms() - t0;
}

/* ---- swap chain ------------------------------------------------------------ */

static GxSwap *s_swap;

static int gx_presenter(void *window, int letterbox)
{
    (void)letterbox;
    if (!G.claimed) {
        if (SDL_ClaimWindowForGPUDevice(G.dev, (SDL_Window *)window)) {
            G.win = (SDL_Window *)window;
            InterlockedExchange(&G.claimed, 1);
            fprintf(stderr, "  [GX] window claimed for %s\n", gx_backend_name());
        } else {
            fprintf(stderr, "  [GX] could not claim the window: %s\n", SDL_GetError());
            InterlockedExchange(&G.claimed, -1);
            return 0;
        }
    }
    SDL_Delay(4);
    return 1;
}

static void swap_destroy(GxObj *o) { gxs_obj_Release(((GxSwap *)o)->bb); s_swap = NULL; }

static GxTex *make_bb(int w, int h)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *t = NULL;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(gxs_ID3D11Device_CreateTexture2D(&s_dev_obj, &td, NULL, &t))) return NULL;
    return (GxTex *)t;
}

IDXGISwapChain1 *gxs_swapchain(void *sdl_window, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    if (!sdl_window) return NULL;
    if (!G.claimed) {
        static int asked;
        if (!asked) { asked = 1; xbox_FramebufferSetGpuPresenter(gx_presenter); }
        return NULL;                                   /* claimed on the window's thread */
    }
    if (G.claimed < 0) return NULL;
    SDL_GetWindowSizeInPixels(G.win, &w, &h);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (!s_swap) {
        s_swap = (GxSwap *)gx_new(sizeof *s_swap, K_SWAP, swap_destroy);
        if (!s_swap) return NULL;
    }
    return (IDXGISwapChain1 *)s_swap;
}

HRESULT gxs_IDXGISwapChain1_ResizeBuffers(void *s, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    GxSwap *sw = (GxSwap *)s;
    (void)n; (void)f; (void)flags;
    if (!w || !h) return E_INVALIDARG;
    if (sw->bb && sw->bb->d.Width == w && sw->bb->d.Height == h) return S_OK;
    if (sw->bb) { gxs_obj_Release(sw->bb); sw->bb = NULL; }
    sw->bb = make_bb((int)w, (int)h);
    return sw->bb ? S_OK : E_OUTOFMEMORY;
}

HRESULT gxs_IDXGISwapChain1_GetBuffer(void *s, UINT i, REFIID riid, void **out)
{
    GxSwap *sw = (GxSwap *)s;
    (void)i; (void)riid;
    if (!sw->bb) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(G.win, &w, &h);
        sw->bb = make_bb(w > 0 ? w : 1280, h > 0 ? h : 720);
        if (!sw->bb) return E_FAIL;
    }
    gxs_obj_AddRef(sw->bb);
    *out = sw->bb;
    return S_OK;
}

HRESULT gxs_IDXGISwapChain1_ResizeTarget(void *s, const DXGI_MODE_DESC *d) { (void)s; (void)d; return S_OK; }
HRESULT gxs_IDXGISwapChain1_SetFullscreenState(void *s, BOOL fs, IDXGIOutput *o) { (void)s; (void)fs; (void)o; return S_OK; }

HRESULT gxs_IDXGISwapChain1_Present(void *s, UINT sync, UINT flags)
{
    GxSwap *sw = (GxSwap *)s;
    SDL_GPUTexture *sct = NULL;
    Uint32 w = 0, h = 0;
    int mode = sync ? 0 : 1;
    (void)flags;
    if (!G.win || !sw->bb) { flush(1); return S_OK; }
    if (mode != G.present_mode) {
        SDL_GPUPresentMode pm = SDL_GPU_PRESENTMODE_VSYNC;
        if (mode == 1) {
            if (SDL_WindowSupportsGPUPresentMode(G.dev, G.win, SDL_GPU_PRESENTMODE_IMMEDIATE)) pm = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if (SDL_WindowSupportsGPUPresentMode(G.dev, G.win, SDL_GPU_PRESENTMODE_MAILBOX)) pm = SDL_GPU_PRESENTMODE_MAILBOX;
        }
        SDL_SetGPUSwapchainParameters(G.dev, G.win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
        G.present_mode = mode;
    }
    flush(0);
    if (s_cb && SDL_WaitAndAcquireGPUSwapchainTexture(s_cb, G.win, &sct, &w, &h) && sct) {
        SDL_GPUBlitInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.source.texture = sw->bb->t; bi.source.w = sw->bb->d.Width; bi.source.h = sw->bb->d.Height;
        bi.destination.texture = sct; bi.destination.w = w; bi.destination.h = h;
        bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
        bi.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(s_cb, &bi);
    }
    flush(1);
    if (++s_st.frames == 300) {
        fprintf(stderr, "  [GX] per frame: %.0f draws, %.1f render passes, %.1f copy passes, %.1f copies, %.1f uploads, "
                "%.2f downloads, %.2f submits, %.2f GPU waits (%.2f ms), flush %.2f ms, new pipelines %.2f ms\n",
                s_st.draws / 300.0, s_st.rpasses / 300.0, s_st.cpasses / 300.0, s_st.copies / 300.0, s_st.uploads / 300.0,
                s_st.downloads / 300.0, s_st.submits / 300.0, s_st.waits / 300.0, s_st.wait_ms / 300.0,
                s_st.flush_ms / 300.0, s_st.pipe_ms / 300.0);
        memset(&s_st, 0, sizeof s_st);
    }
    return S_OK;
}

/* Small objects every backend draw may need. */
static void gx_sdl_defaults(void)
{
    static int done;
    if (done || !G.dev) return;
    done = 1;
    {
        D3D11_TEXTURE2D_DESC td;
        uint32_t white[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
        D3D11_SUBRESOURCE_DATA sd[6];
        ID3D11Texture2D *t = NULL;
        int f;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (f = 0; f < 6; f++) { sd[f].pSysMem = &white[f]; sd[f].SysMemPitch = 4; sd[f].SysMemSlicePitch = 4; }
        if (SUCCEEDED(gxs_ID3D11Device_CreateTexture2D(&s_dev_obj, &td, sd, &t))) {
            G.white = ((GxTex *)t)->t;                 /* kept for the whole run */
            ((GxTex *)t)->t = NULL;
            gxs_obj_Release(t);
        }
        td.ArraySize = 6; td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        if (SUCCEEDED(gxs_ID3D11Device_CreateTexture2D(&s_dev_obj, &td, sd, &t))) {
            G.white_cube = ((GxTex *)t)->t;
            ((GxTex *)t)->t = NULL;
            gxs_obj_Release(t);
        }
    }
    {
        SDL_GPUSamplerCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.address_mode_u = ci.address_mode_v = ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        G.point = SDL_CreateGPUSampler(G.dev, &ci);
    }
}
