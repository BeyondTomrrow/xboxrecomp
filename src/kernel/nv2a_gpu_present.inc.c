/* ── Presentation and video settings (2026-09-26) ────────────────────────
 *
 * Included into nv2a_gpu_d3d11.inc.c.
 *
 * Until now every flip read each dirty render target back into guest RAM
 * (a GPU->CPU stall per frame) so the framebuffer window's thread could
 * convert it and StretchDIBits it with GDI. Here the renderer presents
 * directly: a flip-model swap chain on that window, and one draw that puts
 * the displayed surface into the back buffer -- scaled, filtered and placed
 * by the video settings. Guest RAM still gets the pixels when something
 * reads them (render-to-texture, the software paths, screenshots).
 *
 * The same pass draws the settings page (F1, or Back+Start on a pad) as an
 * overlay: a GDI-drawn bitmap composited over the picture. Settings live in
 * nfl2k5_video.ini next to the executable's working directory.
 */

#include <dxgi1_2.h>
#include <dxgi1_5.h>     /* IDXGIFactory5: tearing / variable refresh */
#include "../input/xinput_xbox.h"



/* src/video/fb_present.c: the window this presents into. */
extern void *xbox_FramebufferWindowHwnd(void);
extern void xbox_FramebufferSetGpuPresent(int on);
extern void xbox_FramebufferSetKeyHook(void (*hook)(unsigned vk));
extern void xbox_FramebufferApplyMode(int mode, int w, int h);

#include <SDL3/SDL.h>

static HWND xbox_GetNativeHwnd(void)
{
    SDL_Window *win = (SDL_Window *)xbox_FramebufferWindowHwnd();
    if (!win) return NULL;
    return (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(win),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        NULL
    );
}

int  xbox_ggl_release_all(void);
void xbox_ggl_reacquire(int depth);

/* ---- settings ---------------------------------------------------------- */

enum { VM_WINDOWED, VM_BORDERLESS, VM_FULLSCREEN };
enum { AR_ORIGINAL, AR_STRETCH, AR_16_9, AR_21_9, AR_32_9, AR_ZOOM };
enum { SF_BILINEAR, SF_SHARP, SF_NEAREST, SF_ENHANCED };

typedef struct {
    int mode;               /* VM_* */
    int res_w, res_h;       /* windowed client size / fullscreen mode */
    int aspect;             /* AR_* */
    int scale;              /* internal resolution multiplier, 1..16 (device may step down safely) */
    int filter;             /* SF_* */
    int vsync;
    int brightness;         /* percent */
    int gamma;              /* x100 */
    int crop;               /* guest pixels cropped from the left and right edges (0 = none) */
    int show_fps;
    int aniso;              /* 1 (off), 2, 4, 8, 16 */
    int fps_limit;          /* frame-rate cap in Hz: 30, 60, 120; 0 unlimited; -1 original (game's own 30/60) */
    int widescreen;         /* built-in Hor+ widescreen: the picture fills the window */
    int lighting;           /* 0 classic, 1 enhanced, 2 maximum */
} VideoSettings;

/* NFL 2K5 draws several full-screen backdrops only inside the 640-pixel
 * title-safe area of its 720-pixel scanout.  A CRT hid the remaining edge;
 * showing the full scanout exposes hard black strips beside the sky. */
static VideoSettings s_vs = { VM_BORDERLESS, 1920, 1080, AR_ORIGINAL, 3, SF_ENHANCED, 1, 100, 100, 0, 0, 8, 0, 1 };
static volatile LONG s_vs_dirty = 1;
void xbox_VideoSettingsLoad(void);           /* window/swap chain need (re)applying */

static const char *k_vs_file = "nfl2k5_video.ini";
void xbox_PresentToast(const char *header, const char *line1, const char *line2, unsigned ms);

static void vs_load(void)
{
    char vpath[MAX_PATH];
    FILE *f = fopen(xbox_user_path(k_vs_file, vpath, sizeof vpath), "r");
    char line[256];
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char key[64];
        int v, v2;
        if (sscanf(line, " resolution = %dx%d", &v, &v2) == 2) { s_vs.res_w = v; s_vs.res_h = v2; continue; }
        if (sscanf(line, " %63[a-z_] = %d", key, &v) != 2)
            continue;
        if (!strcmp(key, "mode")) s_vs.mode = v;
        else if (!strcmp(key, "aspect")) s_vs.aspect = v;
        else if (!strcmp(key, "scale")) s_vs.scale = v;
        else if (!strcmp(key, "filter")) s_vs.filter = v;
        else if (!strcmp(key, "vsync")) s_vs.vsync = v;
        else if (!strcmp(key, "brightness")) s_vs.brightness = v;
        else if (!strcmp(key, "gamma")) s_vs.gamma = v;
        else if (!strcmp(key, "crop")) s_vs.crop = v;
        else if (!strcmp(key, "show_fps")) s_vs.show_fps = v;
        else if (!strcmp(key, "aniso")) s_vs.aniso = v;
        else if (!strcmp(key, "fps_limit")) s_vs.fps_limit = v;
        else if (!strcmp(key, "widescreen")) s_vs.widescreen = v != 0;
        else if (!strcmp(key, "lighting")) s_vs.lighting = v;
    }
    fclose(f);
    if (s_vs.scale < 1) s_vs.scale = 1;
    if (s_vs.scale > 16) s_vs.scale = 16;   /* stable maximum for the supported display modes */
    if (s_vs.lighting < 0 || s_vs.lighting > 2) s_vs.lighting = 0;
    if (s_vs.filter < SF_BILINEAR || s_vs.filter > SF_ENHANCED) s_vs.filter = SF_ENHANCED;
    if (s_vs.crop < 0) s_vs.crop = 0;
    if (s_vs.crop > 40) s_vs.crop = 40;
    if (s_vs.res_w < 320 || s_vs.res_h < 240) { s_vs.res_w = 1440; s_vs.res_h = 1080; }
    if (s_vs.fps_limit != 30 && s_vs.fps_limit != 60 && s_vs.fps_limit != 120 && s_vs.fps_limit != -1) s_vs.fps_limit = 0;
}

static void vs_save(void)
{
    char vpath[MAX_PATH];
    FILE *f = fopen(xbox_user_path(k_vs_file, vpath, sizeof vpath), "w");
    if (!f)
        return;
    fprintf(f, "; NFL 2K5 video settings (F1 in game edits these)\n"
               "; mode: 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen\n"
               "mode = %d\nresolution = %dx%d\n"
               "; aspect: 0 fill the screen (a wider view when it is wider than 16:9), 1 stretch, 2 16:9 with bars, 3 21:9, 4 32:9, 5 zoom to fill\n"
               "aspect = %d\n"
               "; scale: internal resolution = 720x480 times this (1..16; hardware/display limits step it down safely)\n"
               "scale = %d\n"
               "; filter: 0 bilinear, 1 sharp bilinear, 2 nearest, 3 enhanced\n"
               "filter = %d\nvsync = %d\nbrightness = %d\ngamma = %d\ncrop = %d\nshow_fps = %d\naniso = %d\n"
               "; fps_limit: 30 locks 30; 60, 120 and 0 (unlimited) lock 60 in every camera (the game\n"
               "; simulates at most 60 frames a second); -1 original: the game's own 30/60 per camera\n"
               "fps_limit = %d\n"
               "; widescreen: 1 fills the screen with a wider view (built-in widescreen), 0 the original 4:3 picture\n"
               "widescreen = %d\n"
               "; lighting: 0 classic Xbox, 1 enhanced material response, 2 maximum material response\n"
               "lighting = %d\n",
            s_vs.mode, s_vs.res_w, s_vs.res_h, s_vs.aspect, s_vs.scale, s_vs.filter, s_vs.vsync,
            s_vs.brightness, s_vs.gamma, s_vs.crop, s_vs.show_fps, s_vs.aniso, s_vs.fps_limit, s_vs.widescreen, s_vs.lighting);
    fclose(f);
}

/* ---- resolution list --------------------------------------------------- */

static int s_res[64][2], s_nres;

static void res_add(int w, int h)
{
    int i;
    for (i = 0; i < s_nres; i++)
        if (s_res[i][0] == w && s_res[i][1] == h) return;
    if (s_nres < 64) { s_res[s_nres][0] = w; s_res[s_nres][1] = h; s_nres++; }
}

static void res_build(void)
{
    static const int presets[][2] = {
        { 960, 720 }, { 1280, 720 }, { 1440, 1080 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1080 },
        { 2560, 1440 }, { 3440, 1440 }, { 3840, 1600 }, { 3840, 2160 }, { 5120, 2160 }, { 5120, 2880 },
        { 7680, 4320 }
    };
    DEVMODEA dm;
    DWORD i;
    int k, j;
    if (s_nres)                     /* built once: the in-game menu holds indices into it */
        return;
    for (k = 0; k < (int)(sizeof presets / sizeof presets[0]); k++)
        res_add(presets[k][0], presets[k][1]);
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++)
        if (dm.dmPelsWidth >= 800 && dm.dmBitsPerPel == 32)
            res_add((int)dm.dmPelsWidth, (int)dm.dmPelsHeight);
    for (k = 1; k < s_nres; k++)                  /* by pixel count */
        for (j = k; j > 0 && s_res[j][0] * s_res[j][1] < s_res[j - 1][0] * s_res[j - 1][1]; j--) {
            int t0 = s_res[j][0], t1 = s_res[j][1];
            s_res[j][0] = s_res[j - 1][0]; s_res[j][1] = s_res[j - 1][1];
            s_res[j - 1][0] = t0; s_res[j - 1][1] = t1;
        }
}

/* ---- presenter state --------------------------------------------------- */

static struct {
    int tried, ok;
    HWND hwnd;
    IDXGISwapChain1 *sc;
    ID3D11RenderTargetView *bb_rtv;
    UINT bb_w, bb_h;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps, *ps_ovl, *ps_fill;
    ID3D11BlendState *mask_blend[16];
    ID3D11Buffer *cb;
    ID3D11SamplerState *lin, *pt;
    ID3D11BlendState *blend;
    ID3D11RasterizerState *rs;
    ID3D11DepthStencilState *nodepth;
    ID3D11Texture2D *ovl;
    ID3D11ShaderResourceView *ovl_srv;
    int ovl_w, ovl_h, ovl_valid;
    ID3D11Texture2D *toast;
    ID3D11ShaderResourceView *toast_srv;
    int toast_w, toast_h, toast_built;
    int fullscreen_state;
    int tearing;                /* DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING in use */
    /* fps */
    LARGE_INTEGER t0, freq;
    int frames, fps;
} s_pr;

#include "nv2a_gpu_perf.inc.c"

static volatile LONG s_menu_open;
volatile LONG g_xbox_input_blocked;      /* nfl2k5_input_hle.c: game input off while the menu is up */
static int s_menu_sel;

static const char k_present_hlsl[] =
    "cbuffer P : register(b0) { float4 src; float4 prm; float4 sz; };\n"
    "Texture2D Src : register(t0);\n"
    "SamplerState Lin : register(s0); SamplerState Pt : register(s1);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs_main(uint id : SV_VertexID) {\n"
    "  V o; float2 p = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o; }\n"
    "float4 ps_main(V i) : SV_Target {\n"
    "  float2 uv = src.xy + i.uv * src.zw; float4 c;\n"
    "  if (prm.z == 1.0) c = Src.Sample(Pt, uv);\n"
    "  else if (prm.z == 2.0) {\n"
    "    float2 tex = uv * sz.xy, fl = floor(tex), s = frac(tex);\n"
    "    float2 sc = max(floor(sz.zw / (sz.xy * src.zw)), 1.0);\n"
    "    float2 rr = 0.5 - 0.5 / sc, cd = s - 0.5;\n"
    "    float2 f = (cd - clamp(cd, -rr, rr)) * sc + 0.5;\n"
    "    c = Src.Sample(Lin, (fl + f) / sz.xy);\n"
    "  } else if (prm.z == 3.0) {\n"
    "    float2 d = 1.0 / sz.xy;\n"
    "    float4 cc = Src.Sample(Lin, uv);\n"
    "    float4 n = Src.Sample(Lin, uv + float2(0,-d.y));\n"
    "    float4 s = Src.Sample(Lin, uv + float2(0, d.y));\n"
    "    float4 e = Src.Sample(Lin, uv + float2( d.x,0));\n"
    "    float4 w = Src.Sample(Lin, uv + float2(-d.x,0));\n"
    "    float3 lo = min(cc.rgb, min(min(n.rgb,s.rgb),min(e.rgb,w.rgb)));\n"
    "    float3 hi = max(cc.rgb, max(max(n.rgb,s.rgb),max(e.rgb,w.rgb)));\n"
    "    float3 detail = cc.rgb * 4.0 - n.rgb - s.rgb - e.rgb - w.rgb;\n"
    "    c = float4(clamp(cc.rgb + detail * 0.16, lo, hi), cc.a);\n"
    "  } else c = Src.Sample(Lin, uv);\n"
    "  c.rgb = pow(saturate(c.rgb * prm.x), prm.y); c.a = 1.0; return c; }\n"
    "float4 ps_ovl(V i) : SV_Target { return Src.Sample(Lin, i.uv); }\n"
    "float4 ps_fill(V i) : SV_Target { return src; }\n";

typedef struct { float src[4], prm[4], sz[4]; } PresentConsts;

static int pr_init(void)
{
    ID3DBlob *b = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_BLEND_DESC bld;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dd;
    if (s_pr.tried)
        return s_pr.ok;
    s_pr.tried = 1;
    xbox_VideoSettingsLoad();
#define PR_COMPILE(entry, target, out, create) \
    if (FAILED(D3DCompile(k_present_hlsl, sizeof k_present_hlsl - 1, "present", NULL, NULL, entry, target, 0, 0, &b, &err))) { \
        fprintf(stderr, "  [PRESENT] %s: %s\n", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?"); \
        return 0; } \
    create(s_gpu11.dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, out); \
    ID3D10Blob_Release(b);
    PR_COMPILE("vs_main", "vs_4_0", &s_pr.vs, ID3D11Device_CreateVertexShader)
    PR_COMPILE("ps_main", "ps_4_0", &s_pr.ps, ID3D11Device_CreatePixelShader)
    PR_COMPILE("ps_ovl", "ps_4_0", &s_pr.ps_ovl, ID3D11Device_CreatePixelShader)
    PR_COMPILE("ps_fill", "ps_4_0", &s_pr.ps_fill, ID3D11Device_CreatePixelShader)
#undef PR_COMPILE
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(PresentConsts); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_pr.cb);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(s_gpu11.dev, &sd, &s_pr.lin);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(s_gpu11.dev, &sd, &s_pr.pt);
    memset(&bld, 0, sizeof bld);
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(s_gpu11.dev, &bld, &s_pr.blend);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_gpu11.dev, &rd, &s_pr.rs);
    memset(&dd, 0, sizeof dd);
    ID3D11Device_CreateDepthStencilState(s_gpu11.dev, &dd, &s_pr.nodepth);
    QueryPerformanceFrequency(&s_pr.freq);
    QueryPerformanceCounter(&s_pr.t0);
    s_pr.ok = s_pr.vs && s_pr.ps && s_pr.ps_ovl && s_pr.ps_fill && s_pr.cb && s_pr.lin && s_pr.pt;
    {   /* NFL2K5_RENDER_SCALE overrides the saved scale for this run only
         * (performance captures); the ini is left alone. */
        const char *e = getenv("NFL2K5_RENDER_SCALE");
        if (e && atoi(e) >= 1 && atoi(e) <= 16) s_vs.scale = atoi(e);
    }
    g_render_scale = (uint32_t)s_vs.scale;
    g_aniso = (uint32_t)s_vs.aniso;
    g_lighting_profile = (uint32_t)s_vs.lighting;
    return s_pr.ok;
}

/* Common state for the helper draws below. */
static void pr_helper_state(ID3D11RenderTargetView *dst, UINT x, UINT y, UINT w, UINT h, const PresentConsts *pc)
{
    gpu_state_invalidate();     /* binds its own state: the next draw resends all */
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
    D3D11_RECT all = { 0, 0, 16384, 16384 };
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_pr.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, pc, sizeof *pc);
        ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_pr.cb, 0);
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_gpu11.ctx, 1, &dst, NULL);
    ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_gpu11.ctx, 1, &all);
    ID3D11DeviceContext_RSSetState(s_gpu11.ctx, s_pr.rs);
    ID3D11DeviceContext_OMSetDepthStencilState(s_gpu11.ctx, s_pr.nodepth, 0);
    ID3D11DeviceContext_IASetInputLayout(s_gpu11.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_gpu11.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_gpu11.ctx, s_pr.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(s_gpu11.ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_gpu11.ctx, 0, 1, &s_pr.cb);
}

/* src -> dst (dw x dh), filtered, no colour adjustment. */
static void gpu_blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, UINT dw, UINT dh, int point)
{
    PresentConsts pc;
    ID3D11SamplerState *smp[2];
    ID3D11ShaderResourceView *none = NULL;
    if (!pr_init())
        return;
    memset(&pc, 0, sizeof pc);
    pc.src[2] = pc.src[3] = 1.0f;
    pc.prm[0] = 1.0f; pc.prm[1] = 1.0f; pc.prm[2] = point ? 1.0f : 0.0f;
    pc.sz[0] = pc.sz[2] = (float)dw; pc.sz[1] = pc.sz[3] = (float)dh;
    pr_helper_state(dst, 0, 0, dw, dh, &pc);
    ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, s_pr.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &src);
    smp[0] = s_pr.lin; smp[1] = s_pr.pt;
    ID3D11DeviceContext_PSSetSamplers(s_gpu11.ctx, 0, 2, smp);
    ID3D11DeviceContext_Draw(s_gpu11.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &none);
}

/* A solid rectangle, only the channels in `mask` written. */
static void gpu_fill(ID3D11RenderTargetView *dst, UINT x, UINT y, UINT w, UINT h, const float col[4], UINT mask)
{
    PresentConsts pc;
    if (!pr_init() || !mask)
        return;
    mask &= 15u;
    if (!s_pr.mask_blend[mask]) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
        ID3D11Device_CreateBlendState(s_gpu11.dev, &bd, &s_pr.mask_blend[mask]);
    }
    memset(&pc, 0, sizeof pc);
    memcpy(pc.src, col, sizeof pc.src);
    pr_helper_state(dst, x, y, w, h, &pc);
    ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, s_pr.mask_blend[mask], NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, s_pr.ps_fill, NULL, 0);
    ID3D11DeviceContext_Draw(s_gpu11.ctx, 3, 0);
}

static void pr_release_bb(void)
{
    if (s_pr.bb_rtv) { ID3D11RenderTargetView_Release(s_pr.bb_rtv); s_pr.bb_rtv = NULL; }
}

/* The swap chain on the framebuffer window, (re)sized to its client area. */
static int pr_swapchain(void)
{
    gpu_state_invalidate();     /* binds its own state: the next draw resends all */
    HWND hw = xbox_GetNativeHwnd();
    RECT cr;
    UINT w, h;
    if (!hw)
        return 0;
    if (!s_pr.sc || s_pr.hwnd != hw) {
        IDXGIDevice *dxd = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory2 *fac = NULL;
        DXGI_SWAP_CHAIN_DESC1 d;
        if (s_pr.sc) { pr_release_bb(); IDXGISwapChain1_Release(s_pr.sc); s_pr.sc = NULL; }
        if (FAILED(ID3D11Device_QueryInterface(s_gpu11.dev, &IID_IDXGIDevice, (void **)&dxd)))
            return 0;
        IDXGIDevice_GetAdapter(dxd, &ad);
        if (ad) IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
        memset(&d, 0, sizeof d);
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 3;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        if (fac) {
            /* Variable refresh (G-Sync / FreeSync, 2026-10-09): with tearing
             * allowed and VSync off, a frame reaches the screen the moment it
             * is ready, so the game's 60 frames a second stay even on a 120 or
             * 144 Hz display instead of juddering against its refresh. Only when
             * the system supports it (Windows 10 1709+, a VRR-capable driver). */
            IDXGIFactory5 *f5 = NULL;
            BOOL tearing = FALSE;
            if (SUCCEEDED(IDXGIFactory2_QueryInterface(fac, &IID_IDXGIFactory5, (void **)&f5))) {
                if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)))
                    tearing = FALSE;
                IDXGIFactory5_Release(f5);
            }
            s_pr.tearing = tearing && !getenv("NFL2K5_NO_TEARING");
            if (s_pr.tearing) d.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            fprintf(stderr, "  [PRESENT] variable refresh / tearing %s\n", s_pr.tearing ? "available" : "not available");
        }
        if (fac) {
            HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)s_gpu11.dev, hw, &d, NULL, NULL, &s_pr.sc);
            if (FAILED(hr))
                fprintf(stderr, "  [PRESENT] swap chain failed 0x%08lX\n", (unsigned long)hr);
            else
                IDXGIFactory2_MakeWindowAssociation(fac, hw, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory2_Release(fac);
        }
        if (ad) IDXGIAdapter_Release(ad);
        IDXGIDevice_Release(dxd);
        if (!s_pr.sc)
            return 0;
        s_pr.hwnd = hw;
        s_pr.bb_w = s_pr.bb_h = 0;
        xbox_FramebufferSetGpuPresent(1);
        fprintf(stderr, "  [PRESENT] presenting through a D3D11 swap chain\n");
    }
    GetClientRect(hw, &cr);
    w = (UINT)(cr.right - cr.left); h = (UINT)(cr.bottom - cr.top);
    if (!w || !h)
        return 0;
    if (w != s_pr.bb_w || h != s_pr.bb_h || !s_pr.bb_rtv) {
        ID3D11Texture2D *bb = NULL;
        pr_release_bb();
        ID3D11DeviceContext_OMSetRenderTargets(s_gpu11.ctx, 0, NULL, NULL);
        ID3D11DeviceContext_Flush(s_gpu11.ctx);
        if (FAILED(IDXGISwapChain1_ResizeBuffers(s_pr.sc, 0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (s_pr.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0))))
            return 0;
        if (FAILED(IDXGISwapChain1_GetBuffer(s_pr.sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
            return 0;
        ID3D11Device_CreateRenderTargetView(s_gpu11.dev, (ID3D11Resource *)bb, NULL, &s_pr.bb_rtv);
        ID3D11Texture2D_Release(bb);
        s_pr.bb_w = w; s_pr.bb_h = h;
        s_pr.ovl_valid = 0;
    }
    return s_pr.bb_rtv != NULL;
}

/* Window mode and swap-chain fullscreen state from the settings. */
static void pr_apply_mode(void)
{
    int want_fs = s_vs.mode == VM_FULLSCREEN;
    if (s_pr.sc && s_pr.fullscreen_state && !want_fs) {
        IDXGISwapChain1_SetFullscreenState(s_pr.sc, FALSE, NULL);
        s_pr.fullscreen_state = 0;
    }
    xbox_FramebufferApplyMode(s_vs.mode, s_vs.res_w, s_vs.res_h);
    if (s_pr.sc && want_fs) {
        DXGI_MODE_DESC md;
        memset(&md, 0, sizeof md);
        md.Width = (UINT)s_vs.res_w; md.Height = (UINT)s_vs.res_h;
        md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        IDXGISwapChain1_ResizeTarget(s_pr.sc, &md);
        if (SUCCEEDED(IDXGISwapChain1_SetFullscreenState(s_pr.sc, TRUE, NULL)))
            s_pr.fullscreen_state = 1;
    }
}

/* ---- HUD layer ----------------------------------------------------------
 * A premultiplied BGRA image drawn over the game picture at display
 * resolution -- the broadcast scorebug (src/nfl2k5_presentation.cpp). The
 * callback runs on this thread once per presented frame, is told where the
 * game picture sits, and returns its image (uploaded only when changed). */
typedef struct { int bb_w, bb_h; float game_x, game_y, game_w, game_h; } XboxHudFrame;
typedef struct { const void *pixels; int w, h, stride, x, y, changed, dw, dh; void *shared; } XboxHudImage;   /* dw/dh: drawn size (0 = w/h); shared: NT handle of a keyed-mutex texture, or NULL */
int xbox_hud_shared_failed;
typedef int (*XboxHudCallback)(const XboxHudFrame *f, XboxHudImage *img);
static void pr_present_shot(void);
static void pr_frame_limit(void);
static void pr_record(void);
static XboxHudCallback s_hud_cb;
static struct {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11BlendState *blend;
    int w, h;
    /* Shared textures from the HTML host, opened once per handle. */
    struct { HANDLE handle; ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; IDXGIKeyedMutex *mutex; } sh[4];
    int sh_next;
} s_hud;

/* The view and lock of a shared HUD texture; NULL (and sharing off for
 * good) when this device cannot open it, e.g. another adapter. */
static ID3D11ShaderResourceView *hud_shared_view(HANDLE h, IDXGIKeyedMutex **mutex)
{
    int i;
    ID3D11Device1 *d1 = NULL;
    for (i = 0; i < 4; i++)
        if (s_hud.sh[i].handle == h && s_hud.sh[i].srv) { *mutex = s_hud.sh[i].mutex; return s_hud.sh[i].srv; }
    i = s_hud.sh_next++ & 3;
    if (s_hud.sh[i].mutex) IDXGIKeyedMutex_Release(s_hud.sh[i].mutex);
    if (s_hud.sh[i].srv) ID3D11ShaderResourceView_Release(s_hud.sh[i].srv);
    if (s_hud.sh[i].tex) ID3D11Texture2D_Release(s_hud.sh[i].tex);
    memset(&s_hud.sh[i], 0, sizeof s_hud.sh[i]);
    if (FAILED(ID3D11Device_QueryInterface(s_gpu11.dev, &IID_ID3D11Device1, (void **)&d1)) ||
        FAILED(ID3D11Device1_OpenSharedResource1(d1, h, &IID_ID3D11Texture2D, (void **)&s_hud.sh[i].tex)) ||
        FAILED(ID3D11Texture2D_QueryInterface(s_hud.sh[i].tex, &IID_IDXGIKeyedMutex, (void **)&s_hud.sh[i].mutex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s_hud.sh[i].tex, NULL,
                                                     &s_hud.sh[i].srv))) {
        fprintf(stderr, "  [HUD] shared texture unavailable; CPU upload\n");
        if (s_hud.sh[i].mutex) IDXGIKeyedMutex_Release(s_hud.sh[i].mutex);
        if (s_hud.sh[i].tex) ID3D11Texture2D_Release(s_hud.sh[i].tex);
        memset(&s_hud.sh[i], 0, sizeof s_hud.sh[i]);
        if (d1) ID3D11Device1_Release(d1);
        xbox_hud_shared_failed = 1;
        return NULL;
    }
    ID3D11Device1_Release(d1);
    s_hud.sh[i].handle = h;
    *mutex = s_hud.sh[i].mutex;
    return s_hud.sh[i].srv;
}

void xbox_PresentSetHudCallback(XboxHudCallback cb) { s_hud_cb = cb; }

static void pr_draw_hud(float gx, float gy, float gw, float gh)
{
    gpu_state_invalidate();     /* binds its own state: the next draw resends all */
    XboxHudFrame f;
    XboxHudImage img;
    D3D11_VIEWPORT vp;
    if (!s_hud_cb)
        return;
    {   /* NFL2K5_HUD_CPU=1: no shared texture (diagnosis) */
        static int checked;
        if (!checked) { checked = 1; if (getenv("NFL2K5_HUD_CPU")) xbox_hud_shared_failed = 1; }
    }
    f.bb_w = (int)s_pr.bb_w; f.bb_h = (int)s_pr.bb_h;
    f.game_x = gx; f.game_y = gy; f.game_w = gw; f.game_h = gh;
    memset(&img, 0, sizeof img);
    ID3D11ShaderResourceView *view;
    IDXGIKeyedMutex *lock = NULL;
    if (!s_hud_cb(&f, &img) || !img.pixels || img.w <= 0 || img.h <= 0)
        return;
    if (!s_hud.blend) {
        D3D11_BLEND_DESC bld;
        memset(&bld, 0, sizeof bld);
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(s_gpu11.dev, &bld, &s_hud.blend);
    }
    if (img.shared) {
        view = hud_shared_view((HANDLE)img.shared, &lock);
        if (!view || IDXGIKeyedMutex_AcquireSync(lock, 0, 4) != S_OK)
            return;                     /* this frame without it; next uses pixels or retries */
    } else {
    if (!s_hud.tex || s_hud.w != img.w || s_hud.h != img.h) {
        D3D11_TEXTURE2D_DESC td;
        if (s_hud.srv) { ID3D11ShaderResourceView_Release(s_hud.srv); s_hud.srv = NULL; }
        if (s_hud.tex) { ID3D11Texture2D_Release(s_hud.tex); s_hud.tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = (UINT)img.w; td.Height = (UINT)img.h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &s_hud.tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s_hud.tex, NULL, &s_hud.srv)))
            return;
        s_hud.w = img.w; s_hud.h = img.h;
        img.changed = 1;
    }
    if (img.changed) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_hud.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            int row;
            for (row = 0; row < img.h; row++)
                memcpy((uint8_t *)m.pData + (size_t)row * m.RowPitch,
                       (const uint8_t *)img.pixels + (size_t)row * (size_t)img.stride, (size_t)img.w * 4u);
            ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_hud.tex, 0);
        }
    }
    view = s_hud.srv;
    }
    vp.TopLeftX = (float)img.x; vp.TopLeftY = (float)img.y;
    vp.Width = (float)(img.dw > 0 ? img.dw : img.w); vp.Height = (float)(img.dh > 0 ? img.dh : img.h);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, s_hud.blend, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, s_pr.ps_ovl, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &view);
    ID3D11DeviceContext_Draw(s_gpu11.ctx, 3, 0);
    ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, NULL, NULL, 0xFFFFFFFFu);
    if (lock) {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &none);
        IDXGIKeyedMutex_ReleaseSync(lock, 0);
    }
}

/* ---- the settings page -------------------------------------------------- */

enum { MI_MODE, MI_RES, MI_WIDE, MI_ASPECT, MI_SCALE, MI_LIGHTING, MI_FILTER, MI_VSYNC, MI_BRIGHT, MI_GAMMA, MI_CROP,
       MI_ANISO, MI_FPS, MI_LIMIT, MI_RESET, MI_CLOSE, MI_COUNT };
static const int k_fps_limits[] = { 30, 60, 120, 0, -1 };
#define FPS_LIMITS 5
/* The title's frame-interval hook (src/main.c) re-reads the cap when it
 * changes, so a new setting applies at once rather than at the game's next
 * camera change. */
void (*g_xbox_fps_limit_changed)(void);
int xbox_VideoFpsLimit(void) { return s_vs.fps_limit; }
static void fps_limit_changed(void) { if (g_xbox_fps_limit_changed) g_xbox_fps_limit_changed(); }

static void menu_label(int item, char *name, char *val, size_t n)
{
    static const char *modes[] = { "Windowed", "Borderless fullscreen", "Exclusive fullscreen" };
    static const char *aspects[] = { "Fill screen (widens to fit)", "Stretch to screen", "16:9 (black bars)", "21:9 widescreen",
                                     "32:9 widescreen", "Zoom to fill (crop)" };
    static const char *filters[] = { "Bilinear", "Sharp bilinear", "Nearest (pixelated)", "Enhanced sharp" };
    static const char *lighting[] = { "Classic Xbox", "Enhanced", "Maximum" };
    val[0] = 0;
    switch (item) {
    case MI_MODE:   strcpy(name, "Display mode"); snprintf(val, n, "%s", modes[s_vs.mode % 3]); break;
    case MI_RES:    strcpy(name, s_vs.mode == VM_BORDERLESS ? "Resolution (desktop)" : "Resolution");
                    snprintf(val, n, "%d x %d", s_vs.res_w, s_vs.res_h); break;
    case MI_ASPECT: strcpy(name, "Aspect ratio"); snprintf(val, n, "%s", aspects[s_vs.aspect % 6]); break;
    case MI_SCALE:  strcpy(name, "Internal resolution");
                    snprintf(val, n, "%dx  (%d x %d)", s_vs.scale, 720 * s_vs.scale, 480 * s_vs.scale); break;
    case MI_LIGHTING: strcpy(name, "Lighting and materials"); snprintf(val, n, "%s", lighting[s_vs.lighting % 3]); break;
    case MI_FILTER: strcpy(name, "Upscaling filter"); snprintf(val, n, "%s", filters[s_vs.filter % 4]); break;
    case MI_VSYNC:  strcpy(name, "VSync"); snprintf(val, n, "%s", s_vs.vsync ? "On" : "Off"); break;
    case MI_BRIGHT: strcpy(name, "Brightness"); snprintf(val, n, "%d%%", s_vs.brightness); break;
    case MI_GAMMA:  strcpy(name, "Gamma"); snprintf(val, n, "%.2f", s_vs.gamma / 100.0); break;
    case MI_CROP:   strcpy(name, "Crop sides"); snprintf(val, n, s_vs.crop ? "%d px" : "Off", s_vs.crop); break;
    case MI_ANISO:  strcpy(name, "Anisotropic filtering");
                    snprintf(val, n, s_vs.aniso > 1 ? "%dx" : "Off", s_vs.aniso); break;
    case MI_FPS:    strcpy(name, "Show FPS"); snprintf(val, n, "%s", s_vs.show_fps ? "On" : "Off"); break;
    case MI_WIDE:   strcpy(name, "Widescreen"); snprintf(val, n, "%s", s_vs.widescreen ? "On" : "Off"); break;
    case MI_LIMIT:  strcpy(name, "Frame rate cap");
                    if (s_vs.fps_limit < 0) snprintf(val, n, "Original (30/60)");
                    else snprintf(val, n, s_vs.fps_limit ? "%d" : "Unlimited", s_vs.fps_limit); break;
    case MI_RESET:  strcpy(name, "Reset to defaults"); break;
    case MI_CLOSE:  strcpy(name, "Close  (F1 / Esc)"); break;
    default:        name[0] = 0; break;
    }
}

static void menu_change(int item, int dir)
{
    int i;
    switch (item) {
    case MI_MODE:   s_vs.mode = (s_vs.mode + 3 + dir) % 3; InterlockedExchange(&s_vs_dirty, 1); break;
    case MI_RES:
        for (i = 0; i < s_nres; i++)
            if (s_res[i][0] == s_vs.res_w && s_res[i][1] == s_vs.res_h) break;
        i = (i >= s_nres) ? 0 : (i + s_nres + dir) % s_nres;
        s_vs.res_w = s_res[i][0]; s_vs.res_h = s_res[i][1];
        InterlockedExchange(&s_vs_dirty, 1);
        break;
    case MI_ASPECT: s_vs.aspect = (s_vs.aspect + 6 + dir) % 6; break;
    case MI_SCALE:  s_vs.scale += dir; if (s_vs.scale < 1) s_vs.scale = 16; if (s_vs.scale > 16) s_vs.scale = 1; break;
    case MI_LIGHTING: s_vs.lighting = (s_vs.lighting + 3 + dir) % 3; break;
    case MI_FILTER: s_vs.filter = (s_vs.filter + 4 + dir) % 4; break;
    case MI_VSYNC:  s_vs.vsync = !s_vs.vsync; break;
    case MI_BRIGHT: s_vs.brightness += 5 * dir; if (s_vs.brightness < 50) s_vs.brightness = 50;
                    if (s_vs.brightness > 150) s_vs.brightness = 150; break;
    case MI_GAMMA:  s_vs.gamma += 5 * dir; if (s_vs.gamma < 60) s_vs.gamma = 60; if (s_vs.gamma > 160) s_vs.gamma = 160; break;
    case MI_CROP:   s_vs.crop = dir > 0 ? (s_vs.crop >= 40 ? 0 : s_vs.crop + 8) : (s_vs.crop <= 0 ? 40 : s_vs.crop - 8); break;
    case MI_ANISO:  { static const int a[] = { 1, 2, 4, 8, 16 }; int k;
                      for (k = 0; k < 5 && a[k] != s_vs.aniso; k++) {}
                      k = (k >= 5 ? 0 : (k + 5 + dir) % 5); s_vs.aniso = a[k]; } break;
    case MI_FPS:    s_vs.show_fps = !s_vs.show_fps; break;
    case MI_WIDE:   s_vs.widescreen = !s_vs.widescreen; break;
    case MI_LIMIT:  { int k;
                      for (k = 0; k < FPS_LIMITS && k_fps_limits[k] != s_vs.fps_limit; k++) {}
                      k = (k >= FPS_LIMITS ? 3 : (k + FPS_LIMITS + dir) % FPS_LIMITS); s_vs.fps_limit = k_fps_limits[k];
                      fps_limit_changed(); } break;
    default: return;
    }
    s_pr.ovl_valid = 0;
    vs_save();
}

static void menu_activate(int item)
{
    if (item == MI_RESET) {
        VideoSettings d = { VM_BORDERLESS, 1920, 1080, AR_ORIGINAL, 3, SF_ENHANCED, 1, 100, 100, 0, 0, 8, 0, 1 };
        s_vs = d;
        InterlockedExchange(&s_vs_dirty, 1);
        vs_save();
    } else if (item == MI_CLOSE) {
        InterlockedExchange(&s_menu_open, 0);
        InterlockedExchange(&g_xbox_input_blocked, 0);
    } else {
        menu_change(item, 1);
    }
    s_pr.ovl_valid = 0;
}

/* ---- settings pages (2026-10-09) -----------------------------------------
 * The F1 page shows one page at a time; LB/RB (Page Up/Page Down) switch.
 * Page 0 is Video; the title adds its own (src/nfl2k5_controls.c adds
 * Controls) with xbox_MenuAddPage. A page that is waiting for input of its
 * own -- binding a button -- returns nonzero from busy(), and the menu then
 * leaves the controller to it. */
typedef struct XboxMenuPage {
    const char *title;
    int (*count)(void);
    void (*label)(int row, char *name, char *val, size_t n);
    void (*change)(int row, int dir);
    void (*activate)(int row);
    int (*busy)(void);
} XboxMenuPage;
static int video_page_count(void) { return MI_COUNT; }
static const XboxMenuPage k_video_page = { "VIDEO SETTINGS", video_page_count, menu_label, menu_change, menu_activate, NULL };
#define MENU_MAX_PAGES 6
static const XboxMenuPage *s_pages[MENU_MAX_PAGES] = { &k_video_page };
static int s_npages = 1, s_menu_page;
void xbox_MenuAddPage(const XboxMenuPage *p) { if (p && s_npages < MENU_MAX_PAGES) s_pages[s_npages++] = p; }
int (*g_xbox_menu_pad)(XBOX_GAMEPAD *g);    /* set by the title's controller layer */
void xbox_MenuInvalidate(void) { s_pr.ovl_valid = 0; }
int xbox_MenuIsOpen(void) { return s_menu_open != 0; }
static const XboxMenuPage *menu_page(void) { return s_pages[s_menu_page % s_npages]; }
static int menu_rows(void) { int n = menu_page()->count(); return n > 0 ? n : 1; }

/* ---- the in-game Video Settings screen ----------------------------------
 * src/nfl2k5_video_menu.c adds "Video Settings" to the game's own Options
 * menu; its rows are these settings as indexed value lists. Called from the
 * game thread: changes mark the swap chain for re-applying on the next flip
 * and are saved at once. */
static const int k_vm_items[] = { MI_MODE, MI_RES, MI_WIDE, MI_ASPECT, MI_SCALE, MI_LIGHTING, MI_FILTER, MI_VSYNC,
                                  MI_BRIGHT, MI_GAMMA, MI_CROP, MI_ANISO, MI_FPS, MI_LIMIT };
#define VM_COUNT ((int)(sizeof k_vm_items / sizeof k_vm_items[0]))
static const int k_vm_aniso[] = { 1, 2, 4, 8, 16 };

int xbox_VideoSettingCount(void) { return VM_COUNT; }

const char *xbox_VideoSettingName(int i)
{
    static const char *names[] = { "Display Mode", "Resolution", "Widescreen", "Aspect Ratio", "Render Scale", "Lighting & Materials",
                                   "Upscaling Filter", "VSync", "Brightness", "Gamma",
                                   "Overscan Crop", "Texture Filtering", "Show FPS", "Frame Rate Cap" };
    return (i >= 0 && i < VM_COUNT) ? names[i] : "";
}

int xbox_VideoSettingIsToggle(int i)
{
    return i >= 0 && i < VM_COUNT && (k_vm_items[i] == MI_VSYNC || k_vm_items[i] == MI_FPS || k_vm_items[i] == MI_WIDE);
}

int xbox_VideoSettingValues(int i)
{
    if (i < 0 || i >= VM_COUNT) return 1;
    switch (k_vm_items[i]) {
    case MI_MODE: return 3;
    case MI_FILTER: return 4;
    case MI_RES:    res_build(); return s_nres ? s_nres : 1;
    case MI_ASPECT: return 6;
    case MI_SCALE:  return 16;
    case MI_LIGHTING: return 3;
    case MI_VSYNC: case MI_FPS: case MI_WIDE: return 2;
    case MI_BRIGHT: case MI_GAMMA: return 21;
    case MI_CROP: case MI_ANISO: return 5;
    case MI_LIMIT:  return FPS_LIMITS;
    }
    return 1;
}

static int vm_get(const VideoSettings *vs, int item)
{
    int k;
    switch (item) {
    case MI_MODE:   return vs->mode;
    case MI_RES:
        res_build();
        for (k = 0; k < s_nres; k++)
            if (s_res[k][0] == vs->res_w && s_res[k][1] == vs->res_h) return k;
        return 0;
    case MI_ASPECT: return vs->aspect;
    case MI_SCALE:  return vs->scale - 1;
    case MI_LIGHTING: return vs->lighting;
    case MI_FILTER: return vs->filter;
    case MI_VSYNC:  return vs->vsync ? 1 : 0;
    case MI_BRIGHT: return (vs->brightness - 50) / 5;
    case MI_GAMMA:  return (vs->gamma - 60) / 5;
    case MI_CROP:   return vs->crop / 8;
    case MI_ANISO:
        for (k = 0; k < 5; k++)
            if (k_vm_aniso[k] == vs->aniso) return k;
        return 0;
    case MI_FPS:    return vs->show_fps ? 1 : 0;
    case MI_WIDE:   return vs->widescreen ? 1 : 0;
    case MI_LIMIT:
        for (k = 0; k < FPS_LIMITS; k++)
            if (k_fps_limits[k] == vs->fps_limit) return k;
        return 3;
    }
    return 0;
}

static void vm_set(VideoSettings *vs, int item, int idx)
{
    switch (item) {
    case MI_MODE:   vs->mode = idx; break;
    case MI_RES:    res_build(); if (idx < s_nres) { vs->res_w = s_res[idx][0]; vs->res_h = s_res[idx][1]; } break;
    case MI_ASPECT: vs->aspect = idx; break;
    case MI_SCALE:  vs->scale = idx + 1; break;
    case MI_LIGHTING: vs->lighting = idx; break;
    case MI_FILTER: vs->filter = idx; break;
    case MI_VSYNC:  vs->vsync = idx; break;
    case MI_BRIGHT: vs->brightness = 50 + 5 * idx; break;
    case MI_GAMMA:  vs->gamma = 60 + 5 * idx; break;
    case MI_CROP:   vs->crop = 8 * idx; break;
    case MI_ANISO:  vs->aniso = k_vm_aniso[idx % 5]; break;
    case MI_FPS:    vs->show_fps = idx; break;
    case MI_WIDE:   vs->widescreen = idx; break;
    case MI_LIMIT:  vs->fps_limit = k_fps_limits[idx % FPS_LIMITS]; break;
    }
}

void xbox_VideoSettingValueName(int i, int idx, char *buf, size_t n)
{
    static const char *modes[] = { "Windowed", "Borderless", "Fullscreen" };
    static const char *aspects[] = { "Fill Screen", "Stretch", "16:9 Bars", "21:9 Wide",
                                     "32:9 Wide", "Zoom to Fill" };
    static const char *filters[] = { "Bilinear", "Sharp Bilinear", "Nearest", "Enhanced Sharp" };
    static const char *lighting[] = { "Classic Xbox", "Enhanced", "Maximum" };
    VideoSettings v = s_vs;
    int item;
    buf[0] = 0;
    if (i < 0 || i >= VM_COUNT) return;
    item = k_vm_items[i];
    vm_set(&v, item, idx);
    switch (item) {
    case MI_MODE:   snprintf(buf, n, "%s", modes[v.mode % 3]); break;
    case MI_RES:    snprintf(buf, n, "%d x %d", v.res_w, v.res_h); break;
    case MI_ASPECT: snprintf(buf, n, "%s", aspects[v.aspect % 6]); break;
    case MI_SCALE:  snprintf(buf, n, "%dx (%d x %d)", v.scale, 720 * v.scale, 480 * v.scale); break;
    case MI_LIGHTING: snprintf(buf, n, "%s", lighting[v.lighting % 3]); break;
    case MI_FILTER: snprintf(buf, n, "%s", filters[v.filter % 4]); break;
    case MI_VSYNC:  snprintf(buf, n, "%s", v.vsync ? "On" : "Off"); break;
    case MI_BRIGHT: snprintf(buf, n, "%d%%", v.brightness); break;
    case MI_GAMMA:  snprintf(buf, n, "%d.%02d", v.gamma / 100, v.gamma % 100); break;
    case MI_CROP:   if (v.crop) snprintf(buf, n, "%d px", v.crop); else snprintf(buf, n, "Off"); break;
    case MI_ANISO:  if (v.aniso > 1) snprintf(buf, n, "%dx Anisotropic", v.aniso); else snprintf(buf, n, "Standard"); break;
    case MI_FPS:    snprintf(buf, n, "%s", v.show_fps ? "On" : "Off"); break;
    case MI_WIDE:   snprintf(buf, n, "%s", v.widescreen ? "On" : "Off"); break;
    case MI_LIMIT:  if (v.fps_limit < 0) snprintf(buf, n, "Original (30/60)");
                    else if (v.fps_limit) snprintf(buf, n, "%d FPS", v.fps_limit); else snprintf(buf, n, "Unlimited"); break;
    }
}

int xbox_VideoSettingGet(int i)
{
    return (i >= 0 && i < VM_COUNT) ? vm_get(&s_vs, k_vm_items[i]) : 0;
}

void xbox_VideoSettingSet(int i, int idx)
{
    int item, n;
    if (i < 0 || i >= VM_COUNT) return;
    n = xbox_VideoSettingValues(i);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    item = k_vm_items[i];
    vm_set(&s_vs, item, idx);
    if (item == MI_LIMIT)
        fps_limit_changed();
    if (item == MI_MODE || item == MI_RES)
        InterlockedExchange(&s_vs_dirty, 1);
    s_pr.ovl_valid = 0;
    vs_save();
}

/* The settings file is read before the game builds its menus. */
void xbox_VideoSettingsLoad(void)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1)) {
        vs_load();
        res_build();
    }
}

static void menu_key(unsigned vk)
{
    if (vk == VK_F1) {
        LONG open = !s_menu_open;
        InterlockedExchange(&s_menu_open, open);
        InterlockedExchange(&g_xbox_input_blocked, open);
        s_pr.ovl_valid = 0;
        return;
    }
    if (vk == VK_F2) {                                              /* next broadcast presentation */
        extern int nfl2k5_pres_values(int what);
        extern int nfl2k5_pres_get(int what);
        extern void nfl2k5_pres_set(int what, int v);
        extern const char *nfl2k5_pres_package_name(int i);
        int n = nfl2k5_pres_values(0);
        if (n > 0) {
            int v = (nfl2k5_pres_get(0) + 1) % n;
            nfl2k5_pres_set(0, v);
            xbox_PresentToast("PRESENTATION", nfl2k5_pres_package_name(v), "F2 for the next one", 3500);
        }
        return;
    }
    if (vk == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)) {       /* Alt+Enter */
        s_vs.mode = s_vs.mode == VM_WINDOWED ? VM_BORDERLESS : VM_WINDOWED;
        InterlockedExchange(&s_vs_dirty, 1);
        vs_save();
        s_pr.ovl_valid = 0;
        return;
    }
    if (!s_menu_open)
        return;
    {
        const XboxMenuPage *pg = menu_page();
        int rows = menu_rows();
        if (pg->busy && pg->busy()) {                /* the page is reading input itself */
            if (vk == VK_ESCAPE && pg->activate) pg->activate(-1);   /* -1: cancel */
            s_pr.ovl_valid = 0;
            return;
        }
        switch (vk) {
        case VK_UP:     s_menu_sel = (s_menu_sel + rows - 1) % rows; break;
        case VK_DOWN:   s_menu_sel = (s_menu_sel + 1) % rows; break;
        case VK_LEFT:   if (pg->change) pg->change(s_menu_sel, -1); break;
        case VK_RIGHT:  if (pg->change) pg->change(s_menu_sel, 1); break;
        case VK_RETURN: case VK_SPACE: if (pg->activate) pg->activate(s_menu_sel); break;
        case VK_PRIOR:  s_menu_page = (s_menu_page + s_npages - 1) % s_npages; s_menu_sel = 0; break;
        case VK_NEXT:   s_menu_page = (s_menu_page + 1) % s_npages; s_menu_sel = 0; break;
        case VK_ESCAPE: InterlockedExchange(&s_menu_open, 0); InterlockedExchange(&g_xbox_input_blocked, 0); break;
        default: return;
        }
    }
    s_pr.ovl_valid = 0;
}

/* Pad navigation while the page is open, and Back+Start to open it. */
static void menu_pad(void)
{
    static WORD prev;
    static DWORD repeat_at;
    XBOX_INPUT_STATE st;
    WORD b, down;
    /* The title's controller layer (any pad, remapped) when it has one. */
    extern int (*g_xbox_menu_pad)(XBOX_GAMEPAD *g);
    if (g_xbox_menu_pad) {
        memset(&st, 0, sizeof st);
        if (!g_xbox_menu_pad(&st.Gamepad))
            return;
    } else if (xbox_InputGetState(0, &st) != 0)
        return;
    b = st.Gamepad.wButtons;
    if (st.Gamepad.bAnalogButtons[0] > 128) b |= 0x1000;   /* A */
    if (st.Gamepad.bAnalogButtons[1] > 128) b |= 0x2000;   /* B */
    if (st.Gamepad.bAnalogButtons[4] > 128) b |= 0x4000;   /* LB (Black) */
    if (st.Gamepad.bAnalogButtons[5] > 128) b |= 0x8000;   /* RB (White) */
    down = (WORD)(b & ~prev);
    if (s_menu_open && menu_page()->busy && menu_page()->busy()) {   /* the page reads the pad itself */
        prev = b;
        return;
    }
    if ((b & 0x30) == 0x30 && (down & 0x30)) {             /* Back + Start */
        menu_key(VK_F1);
        prev = b;
        return;
    }
    if (s_menu_open) {
        DWORD now = GetTickCount();
        if (b & 0x0F) {                                     /* held d-pad repeats */
            if (down & 0x0F) repeat_at = now + 350;
            else if (now >= repeat_at) { down |= (WORD)(b & 0x0F); repeat_at = now + 90; }
        }
        if (down & 0x01) menu_key(VK_UP);
        if (down & 0x02) menu_key(VK_DOWN);
        if (down & 0x04) menu_key(VK_LEFT);
        if (down & 0x08) menu_key(VK_RIGHT);
        if (down & 0x1000) menu_key(VK_RETURN);
        if (down & 0x2000) menu_key(VK_ESCAPE);
        if (down & 0x4000) menu_key(VK_PRIOR);
        if (down & 0x8000) menu_key(VK_NEXT);
    }
    prev = b;
}

/* Draw the page (or just the FPS counter) with GDI into a BGRA bitmap and
 * upload it; alpha is made from a colour key. */
static void pr_build_overlay(void)
{
    /* Tall enough for the open page's rows (32 px each), at least the old 560. */
    const int W = 760, H = s_menu_open && 90 + menu_rows() * 32 + 80 > 560 ? 90 + menu_rows() * 32 + 80 : 560;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc;
    HBITMAP bmp, old;
    HFONT fb, fs, of;
    int i;
    const COLORREF key = RGB(255, 0, 255);
    if (s_pr.ovl) { ID3D11Texture2D_Release(s_pr.ovl); s_pr.ovl = NULL; }
    if (s_pr.ovl_srv) { ID3D11ShaderResourceView_Release(s_pr.ovl_srv); s_pr.ovl_srv = NULL; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { DeleteDC(dc); return; }
    old = (HBITMAP)SelectObject(dc, bmp);
    {
        HBRUSH kb = CreateSolidBrush(key);
        RECT all = { 0, 0, W, H };
        FillRect(dc, &all, kb);
        DeleteObject(kb);
    }
    fb = CreateFontA(30, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    fs = CreateFontA(24, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    SetBkMode(dc, TRANSPARENT);
    if (s_menu_open) {
        HBRUSH panel = CreateSolidBrush(RGB(16, 22, 38)), bar = CreateSolidBrush(RGB(170, 20, 30));
        HBRUSH selb = CreateSolidBrush(RGB(40, 60, 110));
        RECT pr = { 20, 20, W - 20, H - 20 }, tr = { 20, 20, W - 20, 74 };
        FillRect(dc, &pr, panel);
        FillRect(dc, &tr, bar);
        of = (HFONT)SelectObject(dc, fb);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutA(dc, 40, 30, menu_page()->title, (int)strlen(menu_page()->title));
        if (s_npages > 1) {                          /* which page, and how to switch */
            char pg[48];
            SIZE ps;
            SelectObject(dc, fs);
            snprintf(pg, sizeof pg, "LB / RB  page %d of %d", s_menu_page % s_npages + 1, s_npages);
            GetTextExtentPoint32A(dc, pg, (int)strlen(pg), &ps);
            TextOutA(dc, W - 40 - ps.cx, 36, pg, (int)strlen(pg));
        }
        SelectObject(dc, fs);
        for (i = 0; i < menu_rows(); i++) {
            char name[64], val[96];
            int y = 90 + i * 32;
            name[0] = val[0] = 0;
            menu_page()->label(i, name, val, sizeof val);
            if (i == s_menu_sel) {
                RECT sr = { 30, y - 2, W - 30, y + 30 };
                FillRect(dc, &sr, selb);
            }
            SetTextColor(dc, i == s_menu_sel ? RGB(255, 210, 60) : RGB(225, 230, 240));
            TextOutA(dc, 50, y, name, (int)strlen(name));
            if (val[0]) {
                SIZE ts;
                char v2[80];
                snprintf(v2, sizeof v2, i == s_menu_sel ? "<  %s  >" : "%s", val);
                GetTextExtentPoint32A(dc, v2, (int)strlen(v2), &ts);
                TextOutA(dc, W - 50 - ts.cx, y, v2, (int)strlen(v2));
            }
        }
        SetTextColor(dc, RGB(140, 150, 170));
        TextOutA(dc, 50, H - 56, "Arrows / D-pad: choose   Left/Right: change   Enter / A: select", 64);
        SelectObject(dc, of);
        DeleteObject(panel); DeleteObject(bar); DeleteObject(selb);
    }
    if (s_vs.show_fps) {
        char t[64], t2[128];
        int len, len2;
        perf_overlay_text(t, sizeof t, t2, sizeof t2, s_pr.fps);
        len = (int)strlen(t); len2 = (int)strlen(t2);
        of = (HFONT)SelectObject(dc, fb);
        SetTextColor(dc, RGB(1, 1, 1));
        TextOutA(dc, 6, 6, t, len);
        SetTextColor(dc, RGB(120, 255, 120));
        TextOutA(dc, 4, 4, t, len);
        SelectObject(dc, fs);
        SetTextColor(dc, RGB(1, 1, 1));
        TextOutA(dc, 6, 40, t2, len2);
        SetTextColor(dc, RGB(200, 230, 200));
        TextOutA(dc, 4, 38, t2, len2);
        SelectObject(dc, of);
    }
    GdiFlush();
    {
        uint32_t *p = (uint32_t *)bits;
        int n = W * H;
        for (i = 0; i < n; i++) {
            uint32_t c = p[i] & 0x00FFFFFFu;
            p[i] = c == 0x00FF00FFu ? 0u : (c | (s_menu_open && c == 0x00101626u ? 0xE6000000u : 0xFF000000u));
        }
    }
    {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        memset(&td, 0, sizeof td);
        td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = bits; sd.SysMemPitch = W * 4; sd.SysMemSlicePitch = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, &sd, &s_pr.ovl)))
            ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s_pr.ovl, NULL, &s_pr.ovl_srv);
    }
    s_pr.ovl_w = W; s_pr.ovl_h = H;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteObject(fb); DeleteObject(fs);
    DeleteDC(dc);
    s_pr.ovl_valid = 1;
}

/* ── Toast: a short banner in the bottom-left corner ──────────────────
 * xbox_PresentToast(header, line1, line2, ms): "NOW PLAYING" for the local
 * music library, drawn in the style of the game's own panels. Any thread may
 * call it; the presenter rebuilds the texture when the text changes. */
static SRWLOCK s_toast_lock = SRWLOCK_INIT;
static volatile LONG s_toast_version;
static char s_toast_text[3][128];
static DWORD s_toast_until;

void xbox_PresentToast(const char *header, const char *line1, const char *line2, unsigned ms)
{
    AcquireSRWLockExclusive(&s_toast_lock);
    strncpy_s(s_toast_text[0], sizeof s_toast_text[0], header ? header : "", _TRUNCATE);
    strncpy_s(s_toast_text[1], sizeof s_toast_text[1], line1 ? line1 : "", _TRUNCATE);
    strncpy_s(s_toast_text[2], sizeof s_toast_text[2], line2 ? line2 : "", _TRUNCATE);
    s_toast_until = GetTickCount() + ms;
    InterlockedIncrement(&s_toast_version);
    ReleaseSRWLockExclusive(&s_toast_lock);
}

static void pr_build_toast(void)
{
    const int W = 640, H = 132;
    const COLORREF key = RGB(255, 0, 255);
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc;
    HBITMAP bmp, old;
    HFONT fh, ft, fs, of;
    char text[3][128];
    int i;
    AcquireSRWLockShared(&s_toast_lock);
    memcpy(text, s_toast_text, sizeof text);
    ReleaseSRWLockShared(&s_toast_lock);
    if (s_pr.toast) { ID3D11Texture2D_Release(s_pr.toast); s_pr.toast = NULL; }
    if (s_pr.toast_srv) { ID3D11ShaderResourceView_Release(s_pr.toast_srv); s_pr.toast_srv = NULL; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { DeleteDC(dc); return; }
    old = (HBITMAP)SelectObject(dc, bmp);
    {
        HBRUSH kb = CreateSolidBrush(key), panel = CreateSolidBrush(RGB(14, 20, 34));
        HBRUSH bar = CreateSolidBrush(RGB(196, 18, 30)), edge = CreateSolidBrush(RGB(200, 205, 215));
        RECT all = { 0, 0, W, H }, pr = { 0, 0, W, H }, br = { 0, 0, W, 34 }, er = { 0, H - 4, W, H };
        FillRect(dc, &all, kb);
        FillRect(dc, &pr, panel);
        FillRect(dc, &br, bar);
        FillRect(dc, &er, edge);
        DeleteObject(kb); DeleteObject(panel); DeleteObject(bar); DeleteObject(edge);
    }
    fh = CreateFontA(26, 0, 0, 0, FW_HEAVY, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial Black");
    ft = CreateFontA(36, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
    fs = CreateFontA(26, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
    SetBkMode(dc, TRANSPARENT);
    of = (HFONT)SelectObject(dc, fh);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, 16, 3, text[0], (int)strlen(text[0]));
    SelectObject(dc, ft);
    SetTextColor(dc, RGB(255, 255, 255));
    {
        RECT r = { 16, 40, W - 16, 82 };
        DrawTextA(dc, text[1], -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, fs);
    SetTextColor(dc, RGB(120, 220, 120));
    {
        RECT r = { 16, 86, W - 16, 122 };
        DrawTextA(dc, text[2], -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, of);
    GdiFlush();
    {
        uint32_t *p = (uint32_t *)bits;
        int n = W * H;
        for (i = 0; i < n; i++) {
            uint32_t c = p[i] & 0x00FFFFFFu;
            p[i] = c == 0x00FF00FFu ? 0u : (c | (c == 0x000E1422u ? 0xEB000000u : 0xFF000000u));
        }
    }
    {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        memset(&td, 0, sizeof td);
        td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = bits; sd.SysMemPitch = W * 4; sd.SysMemSlicePitch = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, &sd, &s_pr.toast)))
            ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s_pr.toast, NULL, &s_pr.toast_srv);
    }
    s_pr.toast_w = W; s_pr.toast_h = H;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteObject(fh); DeleteObject(ft); DeleteObject(fs);
    DeleteDC(dc);
}

/* Draw the toast while it is due, in the bottom-left corner. */
static void pr_draw_toast(void)
{
    gpu_state_invalidate();     /* binds its own state: the next draw resends all */
    D3D11_VIEWPORT vp;
    float scale;
    if (!s_toast_version || (LONG)(s_toast_until - GetTickCount()) <= 0)
        return;
    if (s_pr.toast_built != s_toast_version) {
        pr_build_toast();
        s_pr.toast_built = s_toast_version;
    }
    if (!s_pr.toast_srv) return;
    scale = (float)s_pr.bb_h / 1080.0f;
    if (scale < 0.5f) scale = 0.5f;
    memset(&vp, 0, sizeof vp);
    vp.Width = s_pr.toast_w * scale; vp.Height = s_pr.toast_h * scale;
    vp.TopLeftX = s_pr.bb_w * 0.04f;
    vp.TopLeftY = s_pr.bb_h - vp.Height - s_pr.bb_h * 0.07f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, s_pr.blend, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, s_pr.ps_ovl, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &s_pr.toast_srv);
    ID3D11DeviceContext_Draw(s_gpu11.ctx, 3, 0);
}

/* Where the picture goes in the back buffer, for the aspect setting. The
 * kernel reports a widescreen TV (XC_VIDEO_FLAGS_WIDESCREEN), so the game
 * draws anamorphic 16:9 into its 720x480 surface: "original" is 16:9.
 * `frac_w` is the part of the surface's width that is shown (a side crop
 * shows less than all of it, and the picture keeps its proportions). */
/* Hor+ widescreen built into the XBE (2K5 Mod Studio's widescreen patch,
 * which SOFTDRINK 2K28 ships): nine floats at VA 0x10254 -- stretch F, 1/F,
 * 360*(1-1/F), 40, 680, 720, 2, 360, 1. The game then widens its world camera
 * by F and keeps its HUD at 4:3 size; the 720x480 frame is meant to be shown
 * at 1.5*F. Rewriting F for the window's aspect makes it fill any display.
 * Returns that display aspect, or 0 when the XBE has no such block. */
static float s_ana_ar;
/* Set by the game layer: nonzero while a match is on screen. In a match the
 * world camera is widened; in menus F stays 1 so the 4:3 menus are drawn
 * across the whole picture instead of in a middle strip. */
int (*xbox_present_in_match_fn)(void);
static float pr_anamorphic_update(void)
{
    float *k = (float *)((uint8_t *)xbox_GetMemoryOffset() + 0x10254u);
    float bw = (float)s_pr.bb_w, bh = (float)s_pr.bb_h, ar, f;
    if (!(k[3] == 40.0f && k[4] == 680.0f && k[5] == 720.0f && k[7] == 360.0f && k[8] == 1.0f
          && k[0] >= 1.0f && k[0] < 3.0f) || bh <= 0.0f)
        return 0.0f;
    if (!s_vs.widescreen) {                 /* Video Settings > Widescreen Off: the original picture */
        k[0] = 1.0f; k[1] = 1.0f; k[2] = 0.0f;
        return 0.0f;
    }
    ar = bw / bh;                           /* always the window: the picture fills it */
    if (ar < 4.0f / 3.0f) ar = 4.0f / 3.0f;
    if (ar > 32.0f / 9.0f) ar = 32.0f / 9.0f;
    /* The picture shown is the 640x448 action area (the rest of the 720x480
     * frame is TV overscan border), stretched over the whole window: a
     * 640x448 area shown at `ar` is the full frame at ar * 1.05, so
     * F = ar * 1.05 / 1.5. */
    /* A run of frames with no 3D at all (SportsCenter's full-screen cards and
     * the like) is 2D art made for 4:3: the widescreen squeeze would show it
     * narrowed between bars, so it is stretched over the window instead. */
    {
        static int flat_frames;
        if (InterlockedExchange(&s_frame_vp_draws, 0) == 0) { if (flat_frames < 1000) flat_frames++; }
        else flat_frames = 0;
        f = (flat_frames < 6 && xbox_present_in_match_fn && xbox_present_in_match_fn()) ? ar / (640.0f / 448.0f) : 1.0f;
    }
    if (f < 1.0f) f = 1.0f;
    if (k[0] != f) {
        k[0] = f; k[1] = 1.0f / f; k[2] = 360.0f * (1.0f - 1.0f / f);
        fprintf(stderr, "  [PRESENT] built-in widescreen: display %.3f, stretch %.4f\n", ar, f);
    }
    return ar;
}

static void pr_dest_rect(float frac_w, float *x, float *y, float *w, float *h)
{
    float bw = (float)s_pr.bb_w, bh = (float)s_pr.bb_h, ar;
    if (s_ana_ar > 0.0f) {
        ar = s_ana_ar * frac_w;
        if (bw / bh > ar) { *h = bh; *w = bh * ar; } else { *w = bw; *h = bw / ar; }
        *x = (bw - *w) * 0.5f; *y = (bh - *h) * 0.5f;
        return;
    }
    switch (s_vs.aspect) {
    case AR_STRETCH: *x = 0; *y = 0; *w = bw; *h = bh; return;
    case AR_ZOOM:    ar = 16.0f / 9.0f * frac_w;
        if (bw / bh > ar) { *w = bw; *h = bw / ar; } else { *h = bh; *w = bh * ar; }
        *x = (bw - *w) * 0.5f; *y = (bh - *h) * 0.5f;
        return;
    default:         ar = 16.0f / 9.0f * frac_w; break;
    }
    if (bw / bh > ar) { *h = bh; *w = bh * ar; } else { *w = bw; *h = bw / ar; }
    *x = (bw - *w) * 0.5f; *y = (bh - *h) * 0.5f;
}

/* The display aspect the render targets should fill (see g_wide_ar): the
 * window's own when it is wider than 16:9, or a fixed 21:9 / 32:9 preset; 0
 * keeps the plain 16:9 surface. */
static float pr_wanted_wide_ar(void)
{
    float win = s_pr.bb_h > 0 ? (float)s_pr.bb_w / (float)s_pr.bb_h : 0.0f;
    switch (s_vs.aspect) {
    case AR_ORIGINAL: return win > 16.0f / 9.0f + 0.02f ? (win > 32.0f / 9.0f ? 32.0f / 9.0f : win) : 0.0f;
    case AR_21_9:     return 21.0f / 9.0f;
    case AR_32_9:     return 32.0f / 9.0f;
    default:          return 0.0f;
    }
}

/* Present the surface at guest address `guest`. Returns 1 when it did. */
static int pr_present(uint32_t guest)
{
    gpu_state_invalidate();     /* binds its own state: the next draw resends all */
    GpuSurface *s = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    PresentConsts pc;
    D3D11_VIEWPORT vp;
    float dx, dy, dw, dh, frac = 1.0f, clear[4] = { 0, 0, 0, 1 };
    int i;
    LARGE_INTEGER now;

    if (env_cached("RECOMP_GPU_NOPRESENT") || !pr_init())
        return 0;
    for (i = 0; i < GPU_MAX_SURFACES; i++)
        if (s_gpu11.surf[i].tex && s_gpu11.surf[i].guest == guest) { s = &s_gpu11.surf[i]; break; }
    if (!s)
        return 0;
    if (!pr_swapchain())
        return 0;
    if (!s_pr.ok)
        return 0;
    if (InterlockedExchange(&s_vs_dirty, 0))
        pr_apply_mode();
    g_render_scale = (uint32_t)s_vs.scale;     /* surfaces follow on their next use */
    g_aniso = (uint32_t)s_vs.aniso;
    g_lighting_profile = (uint32_t)s_vs.lighting;
    s_ana_ar = pr_anamorphic_update();
    g_wide_ar = s_ana_ar > 0.0f ? 0.0f : pr_wanted_wide_ar();
    menu_pad();

    if (FAILED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s->tex, NULL, &srv)))
        return 0;
    {
        /* A widescreen surface is wider than the guest's: it is shown whole,
         * and its proportions follow from its width. */
        float gw = (float)s->w, sw = gw + 2.0f * (float)s->ox, c = s->ox ? 0.0f : (float)s_vs.crop;
        if (c * 2.0f >= sw) c = 0.0f;
        frac = (sw - 2.0f * c) / gw;                 /* shown width, in guest widths */
        pc.src[0] = c / sw; pc.src[1] = 0.0f;
        pc.src[2] = (sw - 2.0f * c) / sw; pc.src[3] = 1.0f;
        pc.prm[0] = s_vs.brightness / 100.0f;
        pc.prm[1] = 100.0f / (float)s_vs.gamma;
        pc.prm[2] = (float)(s_vs.filter == SF_NEAREST ? 1 : s_vs.filter == SF_SHARP ? 2
                               : s_vs.filter == SF_ENHANCED ? 3 : 0);
        pc.prm[3] = 0.0f;
        pr_dest_rect(frac, &dx, &dy, &dw, &dh);
        if (s_ana_ar > 0.0f && !s->ox) {
            /* Built-in widescreen: the action area fills the window -- no bars. */
            pc.src[0] = 40.0f / 720.0f; pc.src[1] = 16.0f / 480.0f;
            pc.src[2] = 640.0f / 720.0f; pc.src[3] = 448.0f / 480.0f;
            dx = 0.0f; dy = 0.0f; dw = (float)s_pr.bb_w; dh = (float)s_pr.bb_h;
        }
        {
            static unsigned logged_rect;
            if (logged_rect++ % 600 == 0)
                fprintf(stderr, "  [PRESENT] surface %08X %ux%u +%u cols, frac %.3f, window %dx%d -> picture %.0f,%.0f %.0fx%.0f (aspect mode %d)\n",
                        guest, s->w, s->h, s->ox, frac, s_pr.bb_w, s_pr.bb_h, dx, dy, dw, dh, s_vs.aspect);
        }
        /* The surface texture is scale x its guest size once the internal
         * resolution is raised; the shader's texel maths wants its real size. */
        {
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc(s->tex, &td);
            pc.sz[0] = (float)td.Width; pc.sz[1] = (float)td.Height;
        }
        pc.sz[2] = dw; pc.sz[3] = dh;
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_pr.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &pc, sizeof pc);
        ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_pr.cb, 0);
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_gpu11.ctx, 1, &s_pr.bb_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_gpu11.ctx, s_pr.bb_rtv, clear);
    vp.TopLeftX = dx; vp.TopLeftY = dy; vp.Width = dw; vp.Height = dh; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx, 1, &vp);
    {
        D3D11_RECT all = { 0, 0, (LONG)s_pr.bb_w, (LONG)s_pr.bb_h };
        ID3D11DeviceContext_RSSetScissorRects(s_gpu11.ctx, 1, &all);
    }
    ID3D11DeviceContext_RSSetState(s_gpu11.ctx, s_pr.rs);
    ID3D11DeviceContext_OMSetDepthStencilState(s_gpu11.ctx, s_pr.nodepth, 0);
    ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_IASetInputLayout(s_gpu11.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_gpu11.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_gpu11.ctx, s_pr.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(s_gpu11.ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, s_pr.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_gpu11.ctx, 0, 1, &s_pr.cb);
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &srv);
    {
        ID3D11SamplerState *smp[2] = { s_pr.lin, s_pr.pt };
        ID3D11DeviceContext_PSSetSamplers(s_gpu11.ctx, 0, 2, smp);
    }
    ID3D11DeviceContext_Draw(s_gpu11.ctx, 3, 0);
    {
        /* The HUD lays out against the whole surface; a side crop pushes part
         * of it past the picture's edges. */
        float extra = frac < 1.0f ? dw * (1.0f - frac) / (2.0f * frac) : 0.0f;
        if (s->ox) {
            /* The overlay lays out against the original 16:9 picture, which is
             * the centre of the wide one. */
            float cw = dw * (float)s->w / ((float)s->w + 2.0f * (float)s->ox);
            pr_draw_hud(dx + (dw - cw) * 0.5f, dy, cw, dh);
        } else
        pr_draw_hud(dx - extra, dy, dw + 2.0f * extra, dh);
    }

    /* FPS, and the overlay (page and/or counter). */
    s_pr.frames++;
    QueryPerformanceCounter(&now);
    if (now.QuadPart - s_pr.t0.QuadPart >= s_pr.freq.QuadPart) {
        s_pr.fps = (int)((double)s_pr.frames * s_pr.freq.QuadPart / (double)(now.QuadPart - s_pr.t0.QuadPart) + 0.5);
        s_pr.frames = 0;
        s_pr.t0 = now;
        {
            static int secs;
            if (++secs % 10 == 0)
                fprintf(stderr, "  [FPS] %d (scale %d, vsync %d)\n", s_pr.fps, s_vs.scale, s_vs.vsync);
        }
        if (s_vs.show_fps) s_pr.ovl_valid = 0;
    }
    if (s_menu_open || s_vs.show_fps) {
        if (!s_pr.ovl_valid)
            pr_build_overlay();
        if (s_pr.ovl_srv) {
            float scale = (float)s_pr.bb_h * 0.85f / (float)s_pr.ovl_h;
            if (scale * s_pr.ovl_w > s_pr.bb_w * 0.95f) scale = s_pr.bb_w * 0.95f / (float)s_pr.ovl_w;
            if (!s_menu_open) scale = (float)s_pr.bb_h / 1080.0f < 0.6f ? 0.6f : (float)s_pr.bb_h / 1080.0f;
            vp.Width = s_pr.ovl_w * scale; vp.Height = s_pr.ovl_h * scale;
            vp.TopLeftX = s_menu_open ? (s_pr.bb_w - vp.Width) * 0.5f : 0.0f;
            vp.TopLeftY = s_menu_open ? (s_pr.bb_h - vp.Height) * 0.5f : 0.0f;
            ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx, 1, &vp);
            ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, s_pr.blend, NULL, 0xFFFFFFFFu);
            ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, s_pr.ps_ovl, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &s_pr.ovl_srv);
            ID3D11DeviceContext_Draw(s_gpu11.ctx, 3, 0);
        }
    }
    pr_draw_toast();
    {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &none);
    }
    ID3D11ShaderResourceView_Release(srv);
    pr_present_shot();
    pr_record();
    pr_frame_limit();

    {
        LARGE_INTEGER a, b, fq;
        QueryPerformanceCounter(&a);
        /* Tearing is only allowed with VSync off and not in exclusive
         * fullscreen (DXGI rejects the flag there). */
        IDXGISwapChain1_Present(s_pr.sc, s_vs.vsync ? 1u : 0u,
                                (!s_vs.vsync && s_pr.tearing && s_vs.mode != VM_FULLSCREEN) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        QueryPerformanceCounter(&b);
        perf_on_present(s_vs.scale, (float)((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)s_perf.freq.QuadPart));
        QueryPerformanceFrequency(&fq);
        if ((b.QuadPart - a.QuadPart) * 1000 / fq.QuadPart > 200)
            fprintf(stderr, "  [PRESENT] Present took %lld ms (thread %lu)\n",
                    (long long)((b.QuadPart - a.QuadPart) * 1000 / fq.QuadPart), GetCurrentThreadId());
    }
    return 1;
}

/* Frame-rate cap (video settings): hold each presented frame until its
 * slot on an even 1/limit grid. The guest lock is dropped for the wait so
 * the audio thread keeps running. At 30 the game sees every other vblank
 * pass and settles on its 30 fps tier with even pacing; 60 and 120 only cut
 * bursts, since the game never produces more than 60 frames a second (its
 * simulation is tied to the 60 Hz vblank). */
static void pr_frame_limit(void)
{
    static LARGE_INTEGER fq;
    static LONGLONG next;
    static HANDLE timer;
    LARGE_INTEGER now;
    LONGLONG interval;
    if (s_vs.fps_limit <= 0) { next = 0; return; }
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    interval = fq.QuadPart / s_vs.fps_limit;
    QueryPerformanceCounter(&now);
    if (next && now.QuadPart < next && timer) {
        LARGE_INTEGER rel;
        int depth = xbox_ggl_release_all();
        rel.QuadPart = -((next - now.QuadPart) * 10000000 / fq.QuadPart);
        if (SetWaitableTimer(timer, &rel, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, 100);
        xbox_ggl_reacquire(depth);
        QueryPerformanceCounter(&now);
    }
    next = (next && now.QuadPart - next < interval) ? next + interval : now.QuadPart + interval;
}

/* NFL2K5_PRESENT_SHOT=<path prefix>: every NFL2K5_PRESENT_SHOT_SECONDS
 * (default 5) save the frame as presented -- game picture, HUD (HTML
 * presentations) and overlays -- as <prefix>-<n>.bmp at half size. Unlike
 * RECOMP_SHOT_PREFIX (the guest surface) it shows what the player sees, and
 * unlike a screen grab it works with another window in front. */
static void pr_present_shot(void)
{
    static DWORD next;
    static unsigned n;
    static ID3D11Texture2D *staging;
    static UINT sw, sh;
    const char *prefix = getenv("NFL2K5_PRESENT_SHOT");
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    DWORD now = GetTickCount();
    if (!prefix || !*prefix || (next && now < next))
        return;
    next = now + 1000u * (getenv("NFL2K5_PRESENT_SHOT_SECONDS") ? (unsigned)atoi(getenv("NFL2K5_PRESENT_SHOT_SECONDS")) : 5u);
    if (FAILED(IDXGISwapChain1_GetBuffer(s_pr.sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return;
    ID3D11Texture2D_GetDesc(bb, &td);
    if (!staging || sw != td.Width || sh != td.Height) {
        if (staging) ID3D11Texture2D_Release(staging);
        staging = NULL;
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &staging))) { ID3D11Texture2D_Release(bb); return; }
        sw = td.Width; sh = td.Height;
    }
    ID3D11DeviceContext_CopyResource(s_gpu11.ctx, (ID3D11Resource *)staging, (ID3D11Resource *)bb);
    ID3D11Texture2D_Release(bb);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        UINT w = sw / 2, h = sh / 2, row = (w * 3 + 3) & ~3u, x, y;
        uint32_t size = 54 + row * h;
        uint8_t hdr[54] = { 'B', 'M' }, *line = (uint8_t *)calloc(1, row);
        char path[512];
        FILE *f;
        snprintf(path, sizeof path, "%s-%03u.bmp", prefix, n++);
        f = fopen(path, "wb");
        if (f && line) {
            int32_t nh = -(int32_t)h;
            memcpy(hdr + 2, &size, 4); hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &nh, 4); hdr[26] = 1; hdr[28] = 24;
            fwrite(hdr, 1, 54, f);
            for (y = 0; y < h; y++) {
                const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * 2 * m.RowPitch;
                for (x = 0; x < w; x++) memcpy(line + x * 3, src + x * 8, 3);
                fwrite(line, 1, row, f);
            }
            fprintf(stderr, "  [PSHOT] %s t=%lu\n", path, (unsigned long)now);
        }
        if (f) fclose(f);
        free(line);
        ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)staging, 0);
    }
}

/* Video capture: xbox_PresentRecord(seconds) records the presented frames
 * (game, HUD and overlays, half size) for that long into
 * <NFL2K5_REC prefix>-<n>.mp4 through ffmpeg (on PATH, or NFL2K5_FFMPEG).
 * Frames are timestamped by wall clock, so the video keeps real speed at any
 * frame rate. Readback goes through a ring of three staging textures and is
 * read two frames late, so the GPU never waits for it. */
static volatile LONG s_rec_until;     /* GetTickCount deadline, 0 = off */
void xbox_AudioRecordTap(const char *path);   /* apu_xaudio2.c */

/* The clip's picture and sound, recorded side by side, become one MP4:
 * ffmpeg runs in the background so the game does not wait for it. */
static void pr_record_mux(const char *ff, const char *video, const char *raw, const char *out)
{
    char cmd[2048];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    snprintf(cmd, sizeof cmd,
             "cmd.exe /c \"\"%s\" -loglevel error -y -i \"%s\" -f s16le -ar 48000 -ac 2 -i \"%s\" "
             "-c:v copy -c:a aac -b:a 192k -shortest \"%s\" && del \"%s\" \"%s\"\"",
             ff, video, raw, out, video, raw);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void xbox_PresentRecord(int seconds)
{
    if (getenv("NFL2K5_REC") && seconds > 0)
        InterlockedExchange(&s_rec_until, (LONG)(GetTickCount() + (DWORD)seconds * 1000u));
}

static void pr_record(void)
{
    static FILE *pipe;
    static ID3D11Texture2D *ring[3];
    static int filled[3], slot;
    static UINT rw, rh;
    static uint8_t *half;
    static unsigned clip;
    static char video[600], raw[600], out[600];
    DWORD until = (DWORD)s_rec_until;
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC td;
    int i;
    if (!until && !pipe)
        return;
    if (until && GetTickCount() - until < 0x80000000u) {      /* time is up */
        InterlockedExchange(&s_rec_until, 0);
        until = 0;
    }
    if (!until) {
        if (pipe) {
            const char *ff = getenv("NFL2K5_FFMPEG");
            _pclose(pipe);
            pipe = NULL;
            xbox_AudioRecordTap(NULL);
            pr_record_mux(ff ? ff : "ffmpeg", video, raw, out);
            fprintf(stderr, "  [REC] clip %u saved: %s\n", clip - 1, out);
        }
        for (i = 0; i < 3; i++) filled[i] = 0;
        return;
    }
    if (FAILED(IDXGISwapChain1_GetBuffer(s_pr.sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return;
    ID3D11Texture2D_GetDesc(bb, &td);
    if (!ring[0] || rw != td.Width || rh != td.Height) {
        if (pipe) { _pclose(pipe); pipe = NULL; }
        for (i = 0; i < 3; i++) { if (ring[i]) ID3D11Texture2D_Release(ring[i]); ring[i] = NULL; filled[i] = 0; }
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        for (i = 0; i < 3; i++) ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &ring[i]);
        rw = td.Width; rh = td.Height;
        free(half);
        half = (uint8_t *)malloc((size_t)(rw / 2) * (rh / 2) * 4);
    }
    if (!pipe && half) {
        char cmd[1024];
        const char *ff = getenv("NFL2K5_FFMPEG");
        snprintf(out, sizeof out, "%s-%03u.mp4", getenv("NFL2K5_REC"), clip);
        snprintf(video, sizeof video, "%s-%03u.video.mp4", getenv("NFL2K5_REC"), clip);
        snprintf(raw, sizeof raw, "%s-%03u.audio.raw", getenv("NFL2K5_REC"), clip);
        clip++;
        snprintf(cmd, sizeof cmd,
                 "\"\"%s\" -loglevel error -y -use_wallclock_as_timestamps 1 -f rawvideo -pix_fmt bgra -s %ux%u -i - "
                 "-fps_mode cfr -r 30 -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \"%s\"\"",
                 ff ? ff : "ffmpeg", rw / 2, rh / 2, video);
        pipe = _popen(cmd, "wb");
        if (pipe) xbox_AudioRecordTap(raw);
        fprintf(stderr, "  [REC] %s\n", pipe ? "recording" : "ffmpeg did not start");
        if (!pipe) { InterlockedExchange(&s_rec_until, 0); ID3D11Texture2D_Release(bb); return; }
    }
    if (ring[slot]) {
        ID3D11DeviceContext_CopyResource(s_gpu11.ctx, (ID3D11Resource *)ring[slot], (ID3D11Resource *)bb);
        filled[slot] = 1;
    }
    ID3D11Texture2D_Release(bb);
    slot = (slot + 1) % 3;
    if (filled[slot] && ring[slot] && half) {            /* the copy from two frames ago */
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)ring[slot], 0, D3D11_MAP_READ, 0, &m))) {
            UINT w = rw / 2, h = rh / 2, x, y;
            for (y = 0; y < h; y++) {
                const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * 2 * m.RowPitch);
                uint32_t *dst = (uint32_t *)(half + (size_t)y * w * 4);
                for (x = 0; x < w; x++) dst[x] = src[x * 2];
            }
            ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)ring[slot], 0);
            fwrite(half, 4, (size_t)w * h, pipe);
        }
        filled[slot] = 0;
    }
}

/* Called once the window exists: key presses reach the settings page. */
static void pr_hook_keys(void)
{
    static int done;
    if (!done) {
        xbox_FramebufferSetKeyHook(menu_key);
        done = 1;
    }
}
