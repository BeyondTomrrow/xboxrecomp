/* ── Host-drawn 3D scenes inside a guest frame (xbox_hostscene.h) ─────────────
 * Included by nv2a_gpu_d3d11.inc.c after the presenter, so it runs on everything that renderer
 * runs on: Direct3D 11 directly, Direct3D 12 and Vulkan through gx (SDL_GPU), Android through
 * gx with glslang. Only the D3D11 subset the renderer already uses: D3DCompile, default-usage
 * buffers filled with UpdateSubresource, one depth texture of our own.
 *
 * Every piece of pipeline state it binds is the renderer's to rebind: gpu_state_invalidate()
 * afterwards makes the next guest draw send all of it again. Constant buffer slot 3, which the
 * guest pipeline does not use. */
#include "xbox_hostscene.h"

static SRWLOCK s_hs_lock = SRWLOCK_INIT;
static XboxHostScene s_hs_scene;            /* the latest from the game thread */
static XboxHostScene s_hs_draw;             /* the copy being drawn */
static int s_hs_have;
static volatile LONG s_hs_frames;

void xbox_HostSceneSet(const XboxHostScene *scene)
{
    AcquireSRWLockExclusive(&s_hs_lock);
    memcpy(&s_hs_scene, scene, sizeof s_hs_scene);
    s_hs_have = 1;
    ReleaseSRWLockExclusive(&s_hs_lock);
}

int xbox_HostSceneDrawn(void) { return (int)s_hs_frames; }

static uint32_t s_hs_trig[16];
static volatile LONG s_hs_ntrig;

void xbox_HostSceneTrigger(const uint32_t *offsets, int n)
{
    int i;
    if (n > 16) n = 16;
    for (i = 0; i < n; i++) s_hs_trig[i] = offsets[i] & 0x03FFFFFFu;
    InterlockedExchange(&s_hs_ntrig, n);
}

/* The trigger is latched: a draw with the trigger texture (a tiny 2D rect the title queues
 * every frame it wants the scene) is dropped and marks the frame; the NEXT frame gets the
 * host scene at its start -- right after its colour clear, or before its first draw if it
 * has none -- so everything the title draws, 3D and 2D, lands on top of the scene. (The
 * title's 2D is queued and drawn last, so a trigger in its 2D cannot mark the start.) */
static int s_hs_seen, s_hs_active, s_hs_done;
static int gpu_host_scene(void);              /* the locked wrapper, nv2a_gpu_d3d11.inc.c */

/* Is this draw the trigger? (the stage-0 texture offset, low 26 bits) */
static int gpu_host_scene_trigger(uint32_t tex_offset)
{
    int i, n = (int)s_hs_ntrig;
    tex_offset &= 0x03FFFFFFu;
    for (i = 0; i < n; i++)
        if (s_hs_trig[i] == tex_offset && tex_offset) { s_hs_seen = 1; return 1; }
    return 0;
}

/* The start of the frame's drawing: the host scene goes in first. */
static void gpu_host_scene_frame_start(void)
{
    if (s_hs_active && !s_hs_done) { s_hs_done = 1; gpu_host_scene(); }
}

/* FLIP_STALL: the frame that just ended decides whether the next one has the scene. */
static void gpu_host_scene_flip(void)
{
    s_hs_active = s_hs_seen;
    s_hs_seen = 0;
    s_hs_done = 0;
}

static const char k_hs_hlsl[] =
/* One buffer, declared once per stage: SDL_GPU (GX_SDL: Direct3D 12, Vulkan) wants vertex
 * uniforms in space 1 and pixel uniforms in space 3; Direct3D 11 takes slots 3 and 4. */
"#ifdef GX_SDL\n"
"[[vk::binding(0, 1)]] cbuffer VPULL : register(b0, space1) { uint4 pull; float4 jit; };\n"
"[[vk::binding(1, 1)]] cbuffer CV : register(b1, space1) { row_major float4x4 vp; };\n"
"[[vk::binding(0, 3)]] cbuffer CP : register(b0, space3) {\n"
"#else\n"
"cbuffer CV : register(b3) { row_major float4x4 vp; };\n"
"cbuffer CP : register(b4) {\n"
"#endif\n"
"  row_major float4x4 vp_unused; float4 eye; float4 sky; float4 ground; float4 lpos[4]; float4 lcol[4]; float4 misc;\n"
"};\n"
"struct VI { float3 p : POSITION; float3 n : NORMAL; float4 c : COLOR; float2 m : TEXCOORD; };\n"
"struct VO { float4 pos : SV_Position; float3 w : TEXCOORD0; float3 n : TEXCOORD1; float4 c : COLOR; float2 m : TEXCOORD2; };\n"
"#ifdef GX_SDL\n"
"struct HV { float3 p; float3 n; float4 c; float2 m; };\n"
"[[vk::binding(0, 0)]] StructuredBuffer<HV> VB : register(t0, space0);\n"
"VO vs_main(uint id : SV_VertexID) { HV i = VB[pull.x + id];\n"
"#else\n"
"VO vs_main(VI i) {\n"
"#endif\n"
"  VO o; o.pos = mul(float4(i.p, 1), vp); o.w = i.p; o.n = i.n; o.c = i.c; o.m = i.m; return o; }\n"
"float h21(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
"float4 ps_main(VO i) : SV_Target {\n"
"  float3 n = normalize(i.n), v = normalize(eye.xyz - i.w), base = i.c.rgb;\n"
"  int mat = (int)(i.m.x + 0.5); float glow = i.m.y; float spec_k = 0.15, gloss = 24.0;\n"
"  if (mat == 4) return float4(pow(base, 1.0/2.2), 1);\n"
"  if (mat == 7) { float s = 0.5 + 0.5 * sin(i.w.y * 40.0 + misc.y * 2.0); base = lerp(base, base * 1.6, s * 0.15); return float4(pow(base, 1.0/2.2), 1); }\n"
"  if (mat == 1) {\n"                                         /* wood: planks along x */
"    float2 q = abs(n.y) > 0.5 ? i.w.xz : (abs(n.x) > 0.5 ? i.w.zy : i.w.xy);\n"
"    float row = floor(q.y * 5.0), off = h21(float2(row, 3.0));\n"
"    float plank = floor(q.x * 0.8 + off * 4.0);\n"
"    float tone = 0.82 + 0.25 * h21(float2(row, plank));\n"
"    float grain = 0.92 + 0.08 * sin(q.x * 60.0 + sin(q.y * 23.0) * 2.0);\n"
"    float seam = step(0.03, frac(q.y * 5.0)) * step(0.01, frac(q.x * 0.8 + off * 4.0));\n"
"    base *= tone * grain * (0.65 + 0.35 * seam); spec_k = 0.25; gloss = 40.0;\n"
"  } else if (mat == 2) {\n"                                  /* tile */
"    float2 q = abs(n.y) > 0.5 ? i.w.xz : (abs(n.x) > 0.5 ? i.w.zy : i.w.xy);\n"
"    float2 f = frac(q / 0.6); float g = step(0.02, f.x) * step(0.02, f.y);\n"
"    base *= (0.75 + 0.25 * g) * (0.95 + 0.05 * h21(floor(q / 0.6))); spec_k = 0.35; gloss = 64.0;\n"
"  } else if (mat == 5) {\n"                                  /* fabric */
"    base *= 0.9 + 0.1 * h21(floor(i.w.xz * 90.0) + floor(i.w.y * 90.0)); spec_k = 0.03; gloss = 8.0;\n"
"  } else if (mat == 3) {\n"                                  /* mirror: the room's light, reflected */
"    float3 r = reflect(-v, n);\n"
"    float3 env = lerp(ground.rgb * 1.4, sky.rgb * 1.6, saturate(r.y * 0.5 + 0.5));\n"
"    env *= 0.8 + 0.2 * sin(r.x * 9.0) * sin(r.z * 7.0);\n"
"    return float4(pow(saturate(env * base + 0.04), 1.0/2.2), 1);\n"
"  } else if (mat == 6) { spec_k = 0.9; gloss = 120.0; }\n"
"  float3 amb = lerp(ground.rgb, sky.rgb, n.y * 0.5 + 0.5);\n"
"  float ao = 0.55 + 0.45 * saturate(i.w.y / 0.6);\n"
"  float3 dif = 0, spc = 0;\n"
"  for (int k = 0; k < (int)misc.x; k++) {\n"
"    float3 d = lpos[k].xyz - i.w; float dist = length(d); float3 l = d / dist;\n"
"    float att = saturate(1.0 - dist / lpos[k].w); att *= att;\n"
"    dif += lcol[k].rgb * saturate(dot(n, l)) * att;\n"
"    spc += lcol[k].rgb * pow(saturate(dot(n, normalize(l + v))), gloss) * att;\n"
"  }\n"
"  float3 col = base * (amb * ao + dif) + spc * spec_k + base * glow * 0.35;\n"
"  col = col / (1.0 + col * 0.35);\n"
"  return float4(pow(saturate(col), 1.0/2.2), 1);\n"
"}\n";

typedef struct { float p[3], n[3], c[4], m[2]; } HsVert;
#define HS_MAX_VERTS (XBOX_HOSTSCENE_BOXES * 36)

static struct {
    int tried, ok;
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *il;
    ID3D11Buffer *vb, *cb;
    ID3D11RasterizerState *rs; ID3D11DepthStencilState *dss; ID3D11BlendState *bs;
    ID3D11Texture2D *ds; ID3D11DepthStencilView *dsv; UINT ds_w, ds_h;
    HsVert verts[HS_MAX_VERTS];
} s_hs;

static int hs_init(void)
{
    ID3D10Blob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    if (s_hs.tried) return s_hs.ok;
    s_hs.tried = 1;
    if (FAILED(D3DCompile(k_hs_hlsl, sizeof k_hs_hlsl - 1, "hostscene", NULL, NULL, "vs_main", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(k_hs_hlsl, sizeof k_hs_hlsl - 1, "hostscene", NULL, NULL, "ps_main", "ps_4_0", 0, 0, &psb, &err))) {
        fprintf(stderr, "  [HOSTSCENE] shader: %s\n", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    {
        static const D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(ID3D11Device_CreateVertexShader(s_gpu11.dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_hs.vs)) ||
            FAILED(ID3D11Device_CreatePixelShader(s_gpu11.dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_hs.ps)) ||
            FAILED(ID3D11Device_CreateInputLayout(s_gpu11.dev, el, 4, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &s_hs.il))) {
            fprintf(stderr, "  [HOSTSCENE] shader objects failed\n");
            return 0;
        }
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof s_hs.verts; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_hs.vb))) return 0;
    bd.ByteWidth = 256; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_hs.cb))) return 0;
    {
        D3D11_RASTERIZER_DESC rd; D3D11_DEPTH_STENCIL_DESC dd; D3D11_BLEND_DESC bl;
        memset(&rd, 0, sizeof rd); rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        memset(&dd, 0, sizeof dd); dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
        memset(&bl, 0, sizeof bl); bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (FAILED(ID3D11Device_CreateRasterizerState(s_gpu11.dev, &rd, &s_hs.rs)) ||
            FAILED(ID3D11Device_CreateDepthStencilState(s_gpu11.dev, &dd, &s_hs.dss)) ||
            FAILED(ID3D11Device_CreateBlendState(s_gpu11.dev, &bl, &s_hs.bs))) return 0;
    }
    s_hs.ok = 1;
    fprintf(stderr, "  [HOSTSCENE] ready\n");
    return 1;
}

static int hs_depth(UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td;
    if (s_hs.dsv && s_hs.ds_w == w && s_hs.ds_h == h) return 1;
    if (s_hs.dsv) { ID3D11DepthStencilView_Release(s_hs.dsv); s_hs.dsv = NULL; }
    if (s_hs.ds) { ID3D11Texture2D_Release(s_hs.ds); s_hs.ds = NULL; }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &s_hs.ds)) ||
        FAILED(ID3D11Device_CreateDepthStencilView(s_gpu11.dev, (ID3D11Resource *)s_hs.ds, NULL, &s_hs.dsv)))
        return 0;
    s_hs.ds_w = w; s_hs.ds_h = h;
    return 1;
}

static int hs_box(int n, const XboxHostBox *b, int glow)
{
    static const int face[6][4] = { {0,2,6,4}, {1,5,7,3}, {0,4,5,1}, {2,3,7,6}, {0,1,3,2}, {4,6,7,5} };
    static const float nrm[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };
    float c[8][3], col[4];
    int f, k, i;
    for (i = 0; i < 8; i++) {
        c[i][0] = (i & 4) ? b->hi[0] : b->lo[0];
        c[i][1] = (i & 2) ? b->hi[1] : b->lo[1];
        c[i][2] = (i & 1) ? b->hi[2] : b->lo[2];
    }
    col[0] = ((b->rgb >> 16) & 255) / 255.0f; col[1] = ((b->rgb >> 8) & 255) / 255.0f; col[2] = (b->rgb & 255) / 255.0f; col[3] = 1.0f;
    for (i = 0; i < 3; i++) col[i] = powf(col[i], 2.2f);    /* colours are given in sRGB */
    for (f = 0; f < 6; f++) {
        static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (k = 0; k < 6; k++) {
            HsVert *v = &s_hs.verts[n++];
            memcpy(v->p, c[face[f][tri[k]]], sizeof v->p);
            memcpy(v->n, nrm[f], sizeof v->n);
            memcpy(v->c, col, sizeof v->c);
            v->m[0] = (float)b->mat; v->m[1] = glow ? 1.0f : 0.0f;
        }
    }
    return n;
}

/* Row-vector view-projection (D3D clip depth 0..1). */
static void hs_matrix(const XboxHostScene *sc, float aspect, float out[16])
{
    float f[3], r[3], u[3], len, t, view[16], proj[16];
    int i, j, k;
    for (i = 0; i < 3; i++) f[i] = sc->at[i] - sc->eye[i];
    len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); if (len <= 0) len = 1;
    for (i = 0; i < 3; i++) f[i] /= len;
    r[0] = f[2]; r[1] = 0; r[2] = -f[0];                       /* up x f: left-handed, x to the right */
    len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); if (len <= 0) len = 1;
    for (i = 0; i < 3; i++) r[i] /= len;
    u[0] = f[1] * r[2] - f[2] * r[1]; u[1] = f[2] * r[0] - f[0] * r[2]; u[2] = f[0] * r[1] - f[1] * r[0];   /* f x r */
    memset(view, 0, sizeof view);
    for (i = 0; i < 3; i++) { view[i * 4 + 0] = r[i]; view[i * 4 + 1] = u[i]; view[i * 4 + 2] = f[i]; }
    view[12] = -(r[0] * sc->eye[0] + r[1] * sc->eye[1] + r[2] * sc->eye[2]);
    view[13] = -(u[0] * sc->eye[0] + u[1] * sc->eye[1] + u[2] * sc->eye[2]);
    view[14] = -(f[0] * sc->eye[0] + f[1] * sc->eye[1] + f[2] * sc->eye[2]);
    view[15] = 1;
    t = 1.0f / tanf(sc->fov_y * 0.5f * 3.14159265f / 180.0f);
    {
        float zn = 0.05f, zf = 100.0f;
        memset(proj, 0, sizeof proj);
        proj[0] = t / aspect; proj[5] = t; proj[10] = zf / (zf - zn); proj[11] = 1; proj[14] = -zn * zf / (zf - zn);
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0;
            for (k = 0; k < 4; k++) s += view[i * 4 + k] * proj[k * 4 + j];
            out[i * 4 + j] = s;
        }
}

/* Called by the pushbuffer executor at the marker, with the renderer locked. */
static int gpu_host_scene_unlocked(void)
{
    int si, n = 0, i;
    GpuSurface *s;
    UINT w, h, stride = sizeof(HsVert), off = 0;
    float cb[64];
    D3D11_VIEWPORT vp;
    if (!gpu_enabled() || !gpu_init() || !hs_init()) return 0;
    AcquireSRWLockExclusive(&s_hs_lock);
    if (s_hs_have) memcpy(&s_hs_draw, &s_hs_scene, sizeof s_hs_draw);
    ReleaseSRWLockExclusive(&s_hs_lock);
    if (!s_hs_have) return 0;
    si = gpu_bind_surface();
    if (si < 0) return 0;
    s = &s_gpu11.surf[si];
    w = (s->w + 2 * s->ox) * s->scale; h = s->h * s->scale;
    if (!hs_depth(w, h)) return 0;
    for (i = 0; i < s_hs_draw.nbox && i < XBOX_HOSTSCENE_BOXES; i++)
        n = hs_box(n, &s_hs_draw.box[i], i == s_hs_draw.highlight);
    if (!n) return 0;
    memset(cb, 0, sizeof cb);
    {
                /* The window shows the surface's 640 x 448 action area: the whole surface is that
         * much wider and taller on screen. */
        float ar = s_pr.bb_h ? (float)s_pr.bb_w / (float)s_pr.bb_h : 16.0f / 9.0f;
        ar *= (float)(s->w + 2 * s->ox) / 640.0f * 448.0f / (float)s->h;
        hs_matrix(&s_hs_draw, ar, cb);
    }
    memcpy(cb + 16, s_hs_draw.eye, 12);
    memcpy(cb + 20, s_hs_draw.sky, 12);
    memcpy(cb + 24, s_hs_draw.ground, 12);
    for (i = 0; i < s_hs_draw.nlight && i < 4; i++) {
        memcpy(cb + 28 + i * 4, s_hs_draw.light[i].pos, 12); cb[28 + i * 4 + 3] = s_hs_draw.light[i].range;
        memcpy(cb + 44 + i * 4, s_hs_draw.light[i].color, 12);
    }
    cb[60] = (float)(s_hs_draw.nlight < 4 ? s_hs_draw.nlight : 4); cb[61] = s_hs_draw.time;
    ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)s_hs.vb, 0, NULL, s_hs.verts, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)s_hs.cb, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_ClearDepthStencilView(s_gpu11.ctx, s_hs.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s_gpu11.ctx, 1, &s->rtv, s_hs.dsv);
    memset(&vp, 0, sizeof vp); vp.Width = (float)w; vp.Height = (float)h; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx, 1, &vp);
    {   /* the whole surface: gx (SDL_GPU) applies the last scissor even with scissoring off */
        D3D11_RECT sc = { 0, 0, (LONG)w, (LONG)h };
        ID3D11DeviceContext_RSSetScissorRects(s_gpu11.ctx, 1, &sc);
    }
    ID3D11DeviceContext_RSSetState(s_gpu11.ctx, s_hs.rs);
    ID3D11DeviceContext_OMSetDepthStencilState(s_gpu11.ctx, s_hs.dss, 0);
    ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, s_hs.bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_gpu11.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(s_gpu11.ctx, s_hs.il);
    ID3D11DeviceContext_IASetVertexBuffers(s_gpu11.ctx, 0, 1, &s_hs.vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(s_gpu11.ctx, s_hs.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(s_gpu11.ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, s_hs.ps, NULL, 0);
    if (g_gx_sg) gxs_set_pull2(s_hs.vb, NULL, 0, 0, 0.0f, (UINT)sizeof(HsVert));   /* gx pulls vertices */
    ID3D11DeviceContext_VSSetConstantBuffers(s_gpu11.ctx, g_gx_sg ? 1 : 3, 1, &s_hs.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(s_gpu11.ctx, g_gx_sg ? 0 : 4, 1, &s_hs.cb);
    if (env_cached("RECOMP_HOSTSCENE_DEBUG")) {          /* test switch: is the marker's surface the one shown? */
        static const float magenta[4] = { 1, 0, 1, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(s_gpu11.ctx, s->rtv, magenta);
    }
    ID3D11DeviceContext_Draw(s_gpu11.ctx, (UINT)n, 0);
    gpu_state_invalidate();
    s->dirty = 1;
    s->gen++;
    if (env_cached("RECOMP_HOSTSCENE_LOG") && (s_hs_frames % 120) == 0)
        fprintf(stderr, "  [HOSTSCENE] frame %ld surface %08X verts %d eye %.2f %.2f %.2f\n", (long)s_hs_frames, s->guest, n,
                s_hs_draw.eye[0], s_hs_draw.eye[1], s_hs_draw.eye[2]);
    if (InterlockedIncrement(&s_hs_frames) == 1)
        fprintf(stderr, "  [HOSTSCENE] first frame: %d boxes, surface %08X %ux%u (+%u cols) x%u\n",
                s_hs_draw.nbox, s->guest, s->w, s->h, s->ox, s->scale);
    return 1;
}
