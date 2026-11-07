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

static int s_hs_last_si = -1;
static void hs_zprobe(void);

/* FLIP_STALL: the frame that just ended decides whether the next one has the scene. */
static void gpu_host_scene_flip(void)
{
    if (s_hs_active) hs_zprobe();
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
"  float4 fwd; float4 zmap; float4 zclip; float4 room; float4 roomh; float4 shad[16];\n"
"};\n"
"#ifdef GX_SDL\n"
"[[vk::combinedImageSampler]][[vk::binding(0, 2)]] Texture2D Tex : register(t0, space2);\n"
"[[vk::combinedImageSampler]][[vk::binding(0, 2)]] SamplerState Smp : register(s0, space2);\n"
"#else\n"
"Texture2D Tex : register(t0); SamplerState Smp : register(s0);\n"
"#endif\n"
"struct VI { float3 p : POSITION; float3 n : NORMAL; float4 c : COLOR; float2 m : TEXCOORD; float2 uv : TEXCOORD1; };\n"
"struct VO { float4 pos : SV_Position; float3 w : TEXCOORD0; float3 n : TEXCOORD1; float4 c : COLOR; float2 m : TEXCOORD2; float2 uv : TEXCOORD3; };\n"
"#ifdef GX_SDL\n"
"struct HV { float3 p; float3 n; float4 c; float2 m; float2 uv; };\n"
"[[vk::binding(0, 0)]] StructuredBuffer<HV> VB : register(t0, space0);\n"
"VO vs_main(uint id : SV_VertexID) { HV i = VB[pull.x + id];\n"
"#else\n"
"VO vs_main(VI i) {\n"
"#endif\n"
"  VO o; o.pos = mul(float4(i.p, 1), vp); o.w = i.p; o.n = i.n; o.c = i.c; o.m = i.m; o.uv = i.uv; return o; }\n"
"float h21(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
"float4 shade_hs(VO i) {\n"
"  float3 n = normalize(i.n), v = normalize(eye.xyz - i.w), base = i.c.rgb;\n"
"  int mat = (int)(i.m.x + 0.5); float glow = i.m.y; float spec_k = 0.15, gloss = 24.0;\n"
"  if (mat == 4) return float4(pow(base, 1.0/2.2), 1);\n"
"  if (mat >= 8) {\n"
"    float4 t;\n"
"    if (mat >= 24) {\n"                       /* triplanar: the texture by world position */
"      float3 bw = abs(n); bw /= (bw.x + bw.y + bw.z); float3 p = i.w * 1.7;\n"
"      t = Tex.Sample(Smp, p.zy) * bw.x + Tex.Sample(Smp, p.xz) * bw.y + Tex.Sample(Smp, p.xy) * bw.z; mat -= 16;\n"
"      if (mat == 13) { spec_k = 0.03; gloss = 8.0; } else if (mat == 9) { spec_k = 0.22; gloss = 36.0; }\n"
"      base *= pow(t.rgb, 2.2); mat = mat == 14 ? 6 : 0;\n"
"    } else { t = Tex.Sample(Smp, i.uv); clip(t.a - 0.4); base *= pow(t.rgb, 2.2); mat -= 8; }\n"
"  }\n"
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
"  if (roomh.x > 0.0) {\n"                     /* the room's seams and corners, and contact shadows */
"    float dx = max(min(i.w.x - room.x, room.z - i.w.x), 0.0), dz = max(min(i.w.z - room.y, room.w - i.w.z), 0.0);\n"
"    float dyf = max(i.w.y, 0.0), dyc = max(roomh.x - i.w.y, 0.0), dw = min(dx, dz);\n"
"    ao *= 1.0 - 0.32 * exp(-dw * 4.0) * exp(-min(dyf, dyc) * 4.0);\n"
"    ao *= 1.0 - 0.22 * exp(-dx * 3.0) * exp(-dz * 3.0);\n"
"    if (n.y > 0.5 && i.w.y < 0.06) {\n"
"      float sh = 0.0;\n"
"      for (int q = 0; q < (int)roomh.y; q++) {\n"
"        float2 d = max(abs(i.w.xz - shad[q].xy) - shad[q].zw, 0.0);\n"
"        sh += 0.55 * exp(-length(d) * 7.0);\n"
"      }\n"
"      ao *= 1.0 - min(sh, 0.6);\n"
"    }\n"
"  }\n"
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
"}\n"
/* Depth: our own (zmap.w 0), or the game's encoding -- z = A + B / distance (zmap.w 1) or the
 * distance itself (w-buffer, 2), in depth-buffer units clamped to the clip range, over zmax. */
"struct PO { float4 c : SV_Target; float d : SV_Depth; };\n"
"PO ps_main(VO i) {\n"
"  PO o; o.c = shade_hs(i); o.d = i.pos.z;\n"
"  if (zmap.w != 0.0) {\n"
"    float dist = max(dot(i.w - eye.xyz, fwd.xyz), 0.001) * fwd.w;\n"
"    float zd = zmap.w > 1.5 ? dist : zmap.x + zmap.y / dist;\n"
"    o.d = saturate(clamp(zd, zclip.x, zclip.y) / zmap.z);\n"
"  }\n"
"  return o;\n"
"}\n";

typedef struct { float p[3], n[3], c[4], m[2], uv[2]; } HsVert;
#define HS_MAX_VERTS (XBOX_HOSTSCENE_BOXES * 36)

/* Registered meshes: the game side's vertices (8 floats each) and texture, until the render
 * thread makes its texture; placed copies are transformed into one buffer when the placements
 * change. */
#define HS_MAX_MESHES 128
typedef struct {
    float *pnu; int nv, mat;
    uint32_t *rgba; int tw, th;
    ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv;
} HsMesh;
static HsMesh s_hs_mesh[HS_MAX_MESHES];
static volatile LONG s_hs_nmesh;

int xbox_HostMeshAdd(const float *pnu, int nverts, const uint32_t *rgba, int tw, int th, int mat)
{
    HsMesh m;
    LONG id;
    if (!pnu || nverts <= 0 || !rgba || tw <= 0 || th <= 0) return -1;
    memset(&m, 0, sizeof m);
    m.pnu = (float *)malloc(sizeof(float) * 8 * (size_t)nverts);
    m.rgba = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)tw * (size_t)th);
    if (!m.pnu || !m.rgba) { free(m.pnu); free(m.rgba); return -1; }
    memcpy(m.pnu, pnu, sizeof(float) * 8 * (size_t)nverts);
    memcpy(m.rgba, rgba, sizeof(uint32_t) * (size_t)tw * (size_t)th);
    m.nv = nverts; m.tw = tw; m.th = th; m.mat = (mat & 7) | (mat & HS_MESH_TRIPLANAR);
    AcquireSRWLockExclusive(&s_hs_lock);
    id = s_hs_nmesh;
    if (id < HS_MAX_MESHES) { s_hs_mesh[id] = m; s_hs_nmesh = id + 1; }
    ReleaseSRWLockExclusive(&s_hs_lock);
    if (id >= HS_MAX_MESHES) { free(m.pnu); free(m.rgba); return -1; }
    return (int)id;
}

static struct {
    int tried, ok;
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *il;
    ID3D11Buffer *vb, *cb;
    ID3D11RasterizerState *rs; ID3D11DepthStencilState *dss; ID3D11BlendState *bs;
    ID3D11Texture2D *ds; ID3D11DepthStencilView *dsv; UINT ds_w, ds_h;
    ID3D11SamplerState *smp; ID3D11Texture2D *white; ID3D11ShaderResourceView *white_srv;
    ID3D11Buffer *mvb; int mvb_cap, mvb_n;            /* placed meshes, world space */
    int mrange[XBOX_HOSTSCENE_MESHES][3];              /* mesh id, first vertex, count */
    int nrange;
    XboxHostMeshInst placed[XBOX_HOSTSCENE_MESHES]; int nplaced;
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
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(ID3D11Device_CreateVertexShader(s_gpu11.dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_hs.vs)) ||
            FAILED(ID3D11Device_CreatePixelShader(s_gpu11.dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_hs.ps)) ||
            FAILED(ID3D11Device_CreateInputLayout(s_gpu11.dev, el, 5, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &s_hs.il))) {
            fprintf(stderr, "  [HOSTSCENE] shader objects failed\n");
            return 0;
        }
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof s_hs.verts; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_hs.vb))) return 0;
    bd.ByteWidth = 640; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
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
    {
        D3D11_SAMPLER_DESC sd; D3D11_TEXTURE2D_DESC td; D3D11_SUBRESOURCE_DATA sr;
        static const uint32_t one = 0xFFFFFFFFu;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sr.pSysMem = &one; sr.SysMemPitch = 4; sr.SysMemSlicePitch = 4;
        if (FAILED(ID3D11Device_CreateSamplerState(s_gpu11.dev, &sd, &s_hs.smp)) ||
            FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, &sr, &s_hs.white)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s_hs.white, NULL, &s_hs.white_srv))) return 0;
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
            v->uv[0] = v->uv[1] = 0.0f;
        }
    }
    return n;
}

/* A registered mesh's texture, made on first use (render thread). */
static ID3D11ShaderResourceView *hs_mesh_srv(int id)
{
    HsMesh *m = &s_hs_mesh[id];
    if (!m->srv && m->rgba) {
        D3D11_TEXTURE2D_DESC td; D3D11_SUBRESOURCE_DATA sr;
        memset(&td, 0, sizeof td);
        td.Width = (UINT)m->tw; td.Height = (UINT)m->th; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sr.pSysMem = m->rgba; sr.SysMemPitch = (UINT)m->tw * 4u; sr.SysMemSlicePitch = sr.SysMemPitch * (UINT)m->th;
        if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, &sr, &m->tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)m->tex, NULL, &m->srv)))
            m->srv = NULL;
    }
    return m->srv ? m->srv : s_hs.white_srv;
}

/* The placed meshes, transformed into one world-space buffer whenever the placements change. */
static int hs_meshes_build(const XboxHostScene *sc)
{
    int i, total = 0, nm = (int)s_hs_nmesh, n = sc->nmesh < XBOX_HOSTSCENE_MESHES ? sc->nmesh : XBOX_HOSTSCENE_MESHES;
    HsVert *v;
    if (n == s_hs.nplaced && !memcmp(s_hs.placed, sc->mesh, sizeof sc->mesh[0] * (size_t)n)) return 1;
    for (i = 0; i < n; i++)
        if (sc->mesh[i].mesh >= 0 && sc->mesh[i].mesh < nm) total += s_hs_mesh[sc->mesh[i].mesh].nv;
    s_hs.nrange = 0; s_hs.mvb_n = 0;
    memcpy(s_hs.placed, sc->mesh, sizeof sc->mesh[0] * (size_t)n); s_hs.nplaced = n;
    if (!total) return 1;
    if (total > s_hs.mvb_cap) {
        D3D11_BUFFER_DESC bd;
        if (s_hs.mvb) { ID3D11Buffer_Release(s_hs.mvb); s_hs.mvb = NULL; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (UINT)(sizeof(HsVert) * (size_t)total); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_hs.mvb))) { s_hs.mvb_cap = 0; return 0; }
        s_hs.mvb_cap = total;
    }
    v = (HsVert *)calloc((size_t)s_hs.mvb_cap, sizeof *v);
    if (!v) return 0;
    for (i = 0; i < n; i++) {
        const XboxHostMeshInst *in = &sc->mesh[i];
        const HsMesh *m;
        const float *M = in->m;
        int k;
        if (in->mesh < 0 || in->mesh >= nm) continue;
        m = &s_hs_mesh[in->mesh];
        s_hs.mrange[s_hs.nrange][0] = in->mesh; s_hs.mrange[s_hs.nrange][1] = s_hs.mvb_n; s_hs.mrange[s_hs.nrange][2] = m->nv;
        s_hs.nrange++;
        for (k = 0; k < m->nv; k++) {
            const float *s = m->pnu + k * 8;
            HsVert *o = &v[s_hs.mvb_n++];
            float nx, ny, nz, l;
            o->p[0] = s[0] * M[0] + s[1] * M[4] + s[2] * M[8] + M[12];
            o->p[1] = s[0] * M[1] + s[1] * M[5] + s[2] * M[9] + M[13];
            o->p[2] = s[0] * M[2] + s[1] * M[6] + s[2] * M[10] + M[14];
            nx = s[3] * M[0] + s[4] * M[4] + s[5] * M[8];
            ny = s[3] * M[1] + s[4] * M[5] + s[5] * M[9];
            nz = s[3] * M[2] + s[4] * M[6] + s[5] * M[10];
            l = sqrtf(nx * nx + ny * ny + nz * nz); if (l <= 0.0f) l = 1.0f;
            o->n[0] = nx / l; o->n[1] = ny / l; o->n[2] = nz / l;
            o->c[0] = o->c[1] = o->c[2] = o->c[3] = 1.0f;
            o->m[0] = 8.0f + (float)(m->mat & 7) + ((m->mat & HS_MESH_TRIPLANAR) ? 16.0f : 0.0f); o->m[1] = 0.0f;
            o->uv[0] = s[6]; o->uv[1] = s[7];
        }
    }
    ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)s_hs.mvb, 0, NULL, v, 0, 0);
    free(v);
    return 1;
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
    float cb[160];
    int shared = 0;
    ID3D11DepthStencilView *dsv;
    D3D11_VIEWPORT vp;
    if (!gpu_enabled() || !gpu_init() || !hs_init()) return 0;
    AcquireSRWLockExclusive(&s_hs_lock);
    if (s_hs_have) memcpy(&s_hs_draw, &s_hs_scene, sizeof s_hs_draw);
    ReleaseSRWLockExclusive(&s_hs_lock);
    if (!s_hs_have) return 0;
    si = gpu_bind_surface();
    if (si < 0) return 0;
    s_hs_last_si = si;
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
    memcpy(cb + 76, s_hs_draw.room, 16);
    cb[80] = s_hs_draw.ceiling;
    cb[81] = (float)(s_hs_draw.nshadow < XBOX_HOSTSCENE_SHADOWS ? s_hs_draw.nshadow : XBOX_HOSTSCENE_SHADOWS);
    memcpy(cb + 84, s_hs_draw.shadow, sizeof(float) * 4 * XBOX_HOSTSCENE_SHADOWS);
    {   /* forward, for the distance the game's depth is a function of */
        float f[3], l;
        for (i = 0; i < 3; i++) f[i] = s_hs_draw.at[i] - s_hs_draw.eye[i];
        l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); if (l <= 0.0f) l = 1.0f;
        cb[64] = f[0] / l; cb[65] = f[1] / l; cb[66] = f[2] / l; cb[67] = s_hs_draw.zmap[2];
    }
    /* The game's depth buffer, written in the game's own encoding, once its 3D depth
     * parameters are known (s_hs_guest_depthp: the last depth-tested guest draw) and the game
     * side gave its camera's z mapping. RECOMP_HOSTSCENE_OWN_DEPTH keeps a depth of our own. */
    if (s_hs_draw.zmap[3] != 0.0f && s_hs_guest_depthp[1] > 0.0f && s->dsv && !env_cached("RECOMP_HOSTSCENE_OWN_DEPTH")) {
        shared = 1;
        cb[68] = s_hs_draw.zmap[0]; cb[69] = s_hs_draw.zmap[1]; cb[70] = s_hs_guest_depthp[1];
        cb[71] = s_hs_guest_depthp[0] != 0.0f ? 2.0f : 1.0f;
        cb[72] = s_hs_guest_depthp[2]; cb[73] = s_hs_guest_depthp[3] > s_hs_guest_depthp[2] ? s_hs_guest_depthp[3] : s_hs_guest_depthp[1];
    }
    {   /* RECOMP_HOSTSCENE_ZFILE (research): "mode a b scale" overrides the depth mapping live --
         * mode 0 = our own depth buffer; a/b replace the z = a + b / d terms when not 0; scale
         * multiplies the distance. */
        const char *zf = env_cached("RECOMP_HOSTSCENE_ZFILE");
        static float ov[4]; static int have; static DWORD next;
        if (zf && GetTickCount() >= next) {
            FILE *f;
            next = GetTickCount() + 300;
            if (!fopen_s(&f, zf, "r") && f) { have = fscanf(f, "%f %f %f %f", &ov[0], &ov[1], &ov[2], &ov[3]) >= 1; fclose(f); }
        }
        if (zf && have) {
            if (ov[0] == 0.0f) shared = 0;
            else if (s->dsv) {
                shared = 1;
                cb[71] = ov[0];
                if (ov[1] != 0.0f) cb[68] = ov[1];
                if (ov[2] != 0.0f) cb[69] = ov[2];
                if (ov[3] != 0.0f) cb[67] *= ov[3];
                if (cb[70] <= 0.0f) { cb[70] = 16777215.0f; cb[73] = 16777215.0f; }
            }
        }
    }
    ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)s_hs.vb, 0, NULL, s_hs.verts, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)s_hs.cb, 0, NULL, cb, 0, 0);
    dsv = shared ? s->dsv : s_hs.dsv;
    if (!shared) ID3D11DeviceContext_ClearDepthStencilView(s_gpu11.ctx, s_hs.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s_gpu11.ctx, 1, &s->rtv, dsv);
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
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &s_hs.white_srv);
    ID3D11DeviceContext_PSSetSamplers(s_gpu11.ctx, 0, 1, &s_hs.smp);
    ID3D11DeviceContext_Draw(s_gpu11.ctx, (UINT)n, 0);
    if (hs_meshes_build(&s_hs_draw) && s_hs.mvb)
        for (i = 0; i < s_hs.nrange; i++) {
            ID3D11ShaderResourceView *srv = hs_mesh_srv(s_hs.mrange[i][0]);
            UINT moff = (UINT)(sizeof(HsVert) * (size_t)s_hs.mrange[i][1]);
            ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 1, &srv);
            if (g_gx_sg) gxs_set_pull2(s_hs.mvb, NULL, (UINT)s_hs.mrange[i][1], 0, 0.0f, (UINT)sizeof(HsVert));
            else ID3D11DeviceContext_IASetVertexBuffers(s_gpu11.ctx, 0, 1, &s_hs.mvb, &stride, &moff);
            ID3D11DeviceContext_Draw(s_gpu11.ctx, (UINT)s_hs.mrange[i][2], 0);
        }
    gpu_state_invalidate();
    s->dirty = 1;
    s->gen++;
    if (env_cached("RECOMP_HOSTSCENE_LOG") && (s_hs_frames % 120) == 0)
        fprintf(stderr, "  [HOSTSCENE] frame %ld surface %08X verts %d eye %.2f %.2f %.2f\n", (long)s_hs_frames, s->guest, n,
                s_hs_draw.eye[0], s_hs_draw.eye[1], s_hs_draw.eye[2]);
    if (env_cached("RECOMP_HOSTSCENE_LOG") && (s_hs_frames % 120) == 0)
        fprintf(stderr, "  [HOSTSCENE] depth %s: z = %.0f + %.0f / d, zmax %.0f, mode %.0f, clip %.1f..%.1f\n", shared ? "shared" : "own",
                cb[68], cb[69], cb[70], cb[71], cb[72], cb[73]);
    if (InterlockedIncrement(&s_hs_frames) == 1)
        fprintf(stderr, "  [HOSTSCENE] first frame: %d boxes, surface %08X %ux%u (+%u cols) x%u\n",
                s_hs_draw.nbox, s->guest, s->w, s->h, s->ox, s->scale);
    return 1;
}

/* RECOMP_HOSTSCENE_ZPROBE (research, Direct3D 11): every 2 s, the finished frame's depth buffer
 * read back; a 9 x 5 grid of depth values over the action area is logged, so the game's own
 * depth for a known object can be compared with what the host scene writes. */
static void hs_zprobe_unlocked(void)
{
    static int frames;
    static ID3D11Texture2D *st;
    static UINT sw, sh;
    GpuSurface *s;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    int gx, gy;
    if (g_gx_sg || s_hs_last_si < 0 || (++frames % 120) != 60) return;
    s = &s_gpu11.surf[s_hs_last_si];
    if (!s->ds) return;
    ID3D11Texture2D_GetDesc(s->ds, &td);
    if (!st || sw != td.Width || sh != td.Height) {
        if (st) ID3D11Texture2D_Release(st);
        st = NULL;
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &st))) return;
        sw = td.Width; sh = td.Height;
    }
    ID3D11DeviceContext_CopyResource(s_gpu11.ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->ds);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &map))) {
        fprintf(stderr, "  [ZPROBE] %ux%u", sw, sh);
        for (gy = 0; gy < 5; gy++)
            for (gx = 0; gx < 9; gx++) {
                UINT x = sw * (UINT)(gx + 1) / 10u, y = sh * (UINT)(gy + 1) / 6u;
                const float *p = (const float *)((const uint8_t *)map.pData + (size_t)y * map.RowPitch + (size_t)x * 8u);
                fprintf(stderr, "%s%.6f", gx ? " " : " | ", *p);
            }
        fprintf(stderr, "\n");
        ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)st, 0);
    }
}
