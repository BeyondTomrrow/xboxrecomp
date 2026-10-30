/* ── Post-processing (2026-10-30) ─────────────────────────────────────────
 *
 * Included into nv2a_gpu_present.inc.c. With a "Post-processing" preset or
 * "Sharpening" on, the game picture is drawn into an off-screen image the
 * size it is shown at instead of straight to the back buffer, and from there:
 *
 *   bloom     the parts brighter than a threshold, halved five times and
 *             added back up (a soft glow around stadium lights and white
 *             jerseys);
 *   ambient   (Direct3D 12 / Vulkan, which keep a readable copy of the
 *   occlusion depth) distance from depth, blurred twice; a pixel farther
 *             away than its surroundings -- grass at a player's feet, a
 *             corner -- is darkened by how much (unsharp-masked depth);
 *   composite picture + bloom, a highlight shoulder (bright parts roll off
 *             instead of clipping), contrast, saturation, warmth, vignette
 *             and contrast-adaptive sharpening, into the back buffer.
 *
 * The HUD and the menus are drawn afterwards, untouched. Works on both the
 * native Direct3D 11 path and gx_sdl.c (Direct3D 12 / Vulkan); each texture
 * has its own sampler slot, so gx's default texture/sampler pairing fits. */

static const char k_post_hlsl[] =
    "#ifdef GX_SDL\n"
    "[[vk::binding(0, 3)]] cbuffer P : register(b0, space3) { float4 k0; float4 k1; float4 k2; float4 sz; float4 k3; };\n"
    "[[vk::combinedImageSampler]][[vk::binding(0, 2)]] Texture2D T0 : register(t0, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(0, 2)]] SamplerState S0 : register(s0, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(1, 2)]] Texture2D T1 : register(t1, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(1, 2)]] SamplerState S1 : register(s1, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(2, 2)]] Texture2D T2 : register(t2, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(2, 2)]] SamplerState S2 : register(s2, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(3, 2)]] Texture2D T3 : register(t3, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(3, 2)]] SamplerState S3 : register(s3, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(4, 2)]] Texture2D T4 : register(t4, space2);\n"
    "[[vk::combinedImageSampler]][[vk::binding(4, 2)]] SamplerState S4 : register(s4, space2);\n"
    "#else\n"
    "cbuffer P : register(b0) { float4 k0; float4 k1; float4 k2; float4 sz; float4 k3; };\n"
    "Texture2D T0 : register(t0); Texture2D T1 : register(t1); Texture2D T2 : register(t2);\n"
    "Texture2D T3 : register(t3); Texture2D T4 : register(t4);\n"
    "SamplerState S0 : register(s0); SamplerState S1 : register(s1); SamplerState S2 : register(s2);\n"
    "SamplerState S3 : register(s3); SamplerState S4 : register(s4);\n"
    "#endif\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    /* sz.xy: one texel of the source, sz.zw: one texel of the target */
    "float4 box4(float2 uv) {\n"
    "  float2 d = sz.xy;\n"
    "  return 0.25 * (T0.SampleLevel(S0, uv + float2(-d.x, -d.y), 0) + T0.SampleLevel(S0, uv + float2(d.x, -d.y), 0) +\n"
    "                 T0.SampleLevel(S0, uv + float2(-d.x, d.y), 0) + T0.SampleLevel(S0, uv + float2(d.x, d.y), 0)); }\n"
    /* k0.x threshold, k0.y knee */
    "float4 ps_bright(V i) : SV_Target {\n"
    "  float3 c = box4(i.uv).rgb;\n"
    "  float l = max(c.r, max(c.g, c.b));\n"
    "  float s = clamp(l - k0.x + k0.y, 0.0, 2.0 * k0.y); s = s * s / (4.0 * k0.y + 1e-4);\n"
    "  float w = max(s, l - k0.x) / max(l, 1e-4);\n"
    "  return float4(c * w, 1.0); }\n"
    "float4 ps_down(V i) : SV_Target { return float4(box4(i.uv).rgb, 1.0); }\n"
    /* distance from the depth copy (perspective z: distance ~ 1 / (1 - d));
     * k3.xy/zw: the shown part of the surface (offset, size) */
    "float lin(float d) { return d >= 0.99999 ? 100000.0 : 1.0 / max(1.0 - d, 1e-5); }\n"
    "float4 ps_aolin(V i) : SV_Target {\n"
    "  float2 uv = k3.xy + i.uv * k3.zw, d = sz.xy;\n"
    "  float a = lin(T0.SampleLevel(S0, uv + float2(-d.x, -d.y), 0).r), b = lin(T0.SampleLevel(S0, uv + float2(d.x, -d.y), 0).r);\n"
    "  float c = lin(T0.SampleLevel(S0, uv + float2(-d.x, d.y), 0).r), e = lin(T0.SampleLevel(S0, uv + float2(d.x, d.y), 0).r);\n"
    "  return float4(min(min(a, b), min(c, e)), 0, 0, 1); }\n"
    /* 9-tap tent from the smaller level, added onto the larger one */
    "float4 ps_up(V i) : SV_Target {\n"
    "  float2 d = sz.xy; float3 c =\n"
    "    T0.SampleLevel(S0, i.uv + float2(-d.x, -d.y), 0).rgb + 2.0 * T0.SampleLevel(S0, i.uv + float2(0, -d.y), 0).rgb +\n"
    "    T0.SampleLevel(S0, i.uv + float2(d.x, -d.y), 0).rgb + 2.0 * T0.SampleLevel(S0, i.uv + float2(-d.x, 0), 0).rgb +\n"
    "    4.0 * T0.SampleLevel(S0, i.uv, 0).rgb + 2.0 * T0.SampleLevel(S0, i.uv + float2(d.x, 0), 0).rgb +\n"
    "    T0.SampleLevel(S0, i.uv + float2(-d.x, d.y), 0).rgb + 2.0 * T0.SampleLevel(S0, i.uv + float2(0, d.y), 0).rgb +\n"
    "    T0.SampleLevel(S0, i.uv + float2(d.x, d.y), 0).rgb;\n"
    "  return float4(c / 16.0, 1.0); }\n"
    /* k0.w bloom strength; k1.x sharpening, k1.y shoulder start, k1.z exposure;
     * k2 = contrast, saturation, warmth, vignette */
    "float4 ps_comp(V i) : SV_Target {\n"
    "  float2 d = sz.zw;\n"
    "  float3 c = T0.SampleLevel(S0, i.uv, 0).rgb;\n"
    "  if (k1.x > 0.0) {\n"
    "    float3 n = T0.SampleLevel(S0, i.uv + float2(0, -d.y), 0).rgb, s = T0.SampleLevel(S0, i.uv + float2(0, d.y), 0).rgb;\n"
    "    float3 e = T0.SampleLevel(S0, i.uv + float2(d.x, 0), 0).rgb, w = T0.SampleLevel(S0, i.uv + float2(-d.x, 0), 0).rgb;\n"
    "    float3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));\n"
    /* contrast-adaptive: less where the local contrast is already high */
    "    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));\n"
    "    float3 wt = -amp * 0.2 * saturate(k1.x);\n"
    "    c = saturate((c + (n + s + e + w) * wt) / (1.0 + 4.0 * wt));\n"
    "  }\n"
    "  if (k1.w != 0.0) {\n"
    "    float l0 = T2.SampleLevel(S2, i.uv, 0).r;\n"
    "    float lb = 0.5 * (T3.SampleLevel(S3, i.uv, 0).r + T4.SampleLevel(S4, i.uv, 0).r);\n"
    "    float rel = (l0 - lb) / max(l0, 1e-3);\n"
    /* farther than around it darkens; a big jump (a player against the far
     * stands) fades out instead of leaving a halo; the sky is left alone */
    "    float occ = saturate(rel * 16.0) * saturate(1.6 - rel * 8.0) * step(l0, 50000.0);\n"
    "    if (k1.w < 0.0) return float4(1.0 - occ, 1.0 - occ, 1.0 - occ, 1.0);   /* RECOMP_AO_SHOW */\n"
    "    c *= 1.0 - k1.w * occ;\n"
    "  }\n"
    "  c = c * k1.z + T1.SampleLevel(S1, i.uv, 0).rgb * k0.w;\n"
    /* highlight shoulder: below k1.y unchanged, above it rolls off toward 1 */
    "  float t = k1.y, r = 1.0 - t;\n"
    "  c = lerp(c, t + r * (1.0 - exp(-max(c - t, 0.0) / r)), step(t, c));\n"
    "  float l = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "  c = lerp(float3(l, l, l), c, k2.y);\n"
    "  c = (c - 0.5) * k2.x + 0.5;\n"
    "  c += float3(0.03, 0.006, -0.03) * k2.z;\n"
    "  float2 v = (i.uv - 0.5) * float2(sz.w / sz.z, 1.0) * 1.1;\n"
    "  c *= 1.0 - k2.w * saturate(dot(v, v) * 0.9);\n"
    "  return float4(saturate(c), 1.0); }\n";

typedef struct { float k0[4], k1[4], k2[4], sz[4], k3[4]; } PostConsts;

#define POST_LEVELS 5
#define POST_AO_LEVELS 4   /* distance at 1/2, 1/4, 1/8, 1/16 of the picture */
static int s_post_ao_on;
static float s_post_ao_strength;

static struct {
    int tried, ok;
    ID3D11PixelShader *bright, *down, *up, *comp, *aolin;
    ID3D11Buffer *cb;
    ID3D11BlendState *add;
    UINT w, h;
    ID3D11Texture2D *scene;
    ID3D11RenderTargetView *scene_rtv;
    ID3D11ShaderResourceView *scene_srv;
    ID3D11Texture2D *lv[POST_LEVELS];
    ID3D11RenderTargetView *lv_rtv[POST_LEVELS];
    ID3D11ShaderResourceView *lv_srv[POST_LEVELS];
    UINT lw[POST_LEVELS], lh[POST_LEVELS];
    /* ambient occlusion: distance at 1/2 ... 1/16 of the picture; the composite blurs with 1/8 and 1/16 */
    ID3D11Texture2D *ao[POST_AO_LEVELS];
    ID3D11RenderTargetView *ao_rtv[POST_AO_LEVELS];
    ID3D11ShaderResourceView *ao_srv[POST_AO_LEVELS];
    UINT aw[POST_AO_LEVELS], ah[POST_AO_LEVELS];
} s_post;

/* Presets: bloom threshold, bloom strength, contrast, saturation, warmth,
 * vignette (sharp: not used yet; the Sharpening setting is separate). */
static const struct { const char *name; float thr, bloom, con, sat, warm, vig, sharp; } k_post_presets[] = {
    { "Off",        1.00f, 0.00f, 1.00f, 1.00f, 0.0f, 0.00f, 0.00f },
    { "Broadcast",  0.78f, 0.22f, 1.06f, 1.08f, 0.0f, 0.00f, 0.25f },
    { "Cinematic",  0.68f, 0.40f, 1.12f, 0.94f, 0.6f, 0.28f, 0.15f },
    { "Vivid",      0.72f, 0.32f, 1.10f, 1.25f, 0.2f, 0.10f, 0.35f },
    { "Night Game", 0.60f, 0.55f, 1.14f, 1.02f, -0.3f, 0.20f, 0.20f },
};
#define POST_PRESETS ((int)(sizeof k_post_presets / sizeof k_post_presets[0]))
static const float k_sharpen_amount[] = { 0.0f, 0.2f, 0.4f, 0.7f };   /* Off, Low, Medium, High */

static void post_release_targets(void)
{
    int i;
    if (s_post.scene_srv) ID3D11ShaderResourceView_Release(s_post.scene_srv);
    if (s_post.scene_rtv) ID3D11RenderTargetView_Release(s_post.scene_rtv);
    if (s_post.scene) ID3D11Texture2D_Release(s_post.scene);
    s_post.scene_srv = NULL; s_post.scene_rtv = NULL; s_post.scene = NULL;
    for (i = 0; i < POST_LEVELS; i++) {
        if (s_post.lv_srv[i]) ID3D11ShaderResourceView_Release(s_post.lv_srv[i]);
        if (s_post.lv_rtv[i]) ID3D11RenderTargetView_Release(s_post.lv_rtv[i]);
        if (s_post.lv[i]) ID3D11Texture2D_Release(s_post.lv[i]);
        s_post.lv_srv[i] = NULL; s_post.lv_rtv[i] = NULL; s_post.lv[i] = NULL;
    }
    for (i = 0; i < POST_AO_LEVELS; i++) {
        if (s_post.ao_srv[i]) ID3D11ShaderResourceView_Release(s_post.ao_srv[i]);
        if (s_post.ao_rtv[i]) ID3D11RenderTargetView_Release(s_post.ao_rtv[i]);
        if (s_post.ao[i]) ID3D11Texture2D_Release(s_post.ao[i]);
        s_post.ao_srv[i] = NULL; s_post.ao_rtv[i] = NULL; s_post.ao[i] = NULL;
    }
    s_post.w = s_post.h = 0;
}

static int post_make_fmt(UINT w, UINT h, DXGI_FORMAT fmt, ID3D11Texture2D **t, ID3D11RenderTargetView **rtv,
                         ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, t)) &&
           SUCCEEDED(ID3D11Device_CreateRenderTargetView(s_gpu11.dev, (ID3D11Resource *)*t, NULL, rtv)) &&
           SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)*t, NULL, srv));
}
#define post_make(w, h, t, rtv, srv) post_make_fmt(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, t, rtv, srv)

static int post_init(void)
{
    ID3DBlob *b = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bld;
    if (s_post.tried) return s_post.ok;
    s_post.tried = 1;
#define POST_COMPILE(entry, out) \
    if (FAILED(D3DCompile(k_post_hlsl, sizeof k_post_hlsl - 1, "post", NULL, NULL, entry, "ps_4_0", 0, 0, &b, &err))) { \
        fprintf(stderr, "  [POST] %s: %s\n", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?"); \
        return 0; } \
    ID3D11Device_CreatePixelShader(s_gpu11.dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, out); \
    ID3D10Blob_Release(b);
    POST_COMPILE("ps_bright", &s_post.bright)
    POST_COMPILE("ps_down", &s_post.down)
    POST_COMPILE("ps_up", &s_post.up)
    POST_COMPILE("ps_comp", &s_post.comp)
    POST_COMPILE("ps_aolin", &s_post.aolin)
#undef POST_COMPILE
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(PostConsts); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_post.cb);
    memset(&bld, 0, sizeof bld);
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(s_gpu11.dev, &bld, &s_post.add);
    s_post.ok = s_post.bright && s_post.down && s_post.up && s_post.comp && s_post.aolin && s_post.cb && s_post.add;
    fprintf(stderr, "  [POST] %s\n", s_post.ok ? "ready" : "unavailable");
    return s_post.ok;
}

static int post_wanted(void)
{
    if (env_cached("RECOMP_NO_POST")) return 0;
    return (s_vs.post > 0 && s_vs.post < POST_PRESETS) || s_vs.sharpen > 0 || s_vs.ao > 0;
}

/* The off-screen picture for this frame (w x h, the size it is shown at), or
 * NULL when post-processing is off or not available. */
static ID3D11RenderTargetView *post_begin(UINT w, UINT h)
{
    int i;
    if (!post_wanted() || !w || !h || !post_init()) return NULL;
    if (s_post.w != w || s_post.h != h || !s_post.scene) {
        post_release_targets();
        if (!post_make(w, h, &s_post.scene, &s_post.scene_rtv, &s_post.scene_srv)) { post_release_targets(); return NULL; }
        for (i = 0; i < POST_LEVELS; i++) {
            UINT lw = w >> (i + 1), lh = h >> (i + 1);
            s_post.lw[i] = lw ? lw : 1; s_post.lh[i] = lh ? lh : 1;
            if (!post_make(s_post.lw[i], s_post.lh[i], &s_post.lv[i], &s_post.lv_rtv[i], &s_post.lv_srv[i])) {
                post_release_targets();
                return NULL;
            }
        }
        for (i = 0; i < POST_AO_LEVELS; i++) {
            UINT aw = w >> (i + 1), ah = h >> (i + 1);
            s_post.aw[i] = aw ? aw : 1; s_post.ah[i] = ah ? ah : 1;
            if (!post_make_fmt(s_post.aw[i], s_post.ah[i], DXGI_FORMAT_R32_FLOAT, &s_post.ao[i], &s_post.ao_rtv[i],
                               &s_post.ao_srv[i])) {
                post_release_targets();
                return NULL;
            }
        }
        s_post.w = w; s_post.h = h;
    }
    return s_post.scene_rtv;
}

static void post_pass5(ID3D11PixelShader *ps, ID3D11RenderTargetView *dst, float x, float y, UINT w, UINT h,
                       ID3D11ShaderResourceView *const t[5], const PostConsts *pc, ID3D11BlendState *blend)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp = { x, y, (float)w, (float)h, 0.0f, 1.0f };
    ID3D11ShaderResourceView *none[5] = { NULL, NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[5] = { s_pr.lin, s_pr.lin, s_pr.lin, s_pr.lin, s_pr.lin };
    UINT nt = 5;
    while (nt > 1 && !t[nt - 1]) nt--;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_post.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, pc, sizeof *pc);
        ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_post.cb, 0);
    }
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 5, none);
    ID3D11DeviceContext_OMSetRenderTargets(s_gpu11.ctx, 1, &dst, NULL);
    ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, blend, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_gpu11.ctx, 0, 1, &s_post.cb);
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, nt, t);
    ID3D11DeviceContext_PSSetSamplers(s_gpu11.ctx, 0, nt, smp);
    ID3D11DeviceContext_Draw(s_gpu11.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 5, none);
}

static void post_pass(ID3D11PixelShader *ps, ID3D11RenderTargetView *dst, float x, float y, UINT w, UINT h,
                      ID3D11ShaderResourceView *t0, ID3D11ShaderResourceView *t1, const PostConsts *pc,
                      ID3D11BlendState *blend)
{
    ID3D11ShaderResourceView *t[5] = { t0, t1, NULL, NULL, NULL };
    post_pass5(ps, dst, x, y, w, h, t, pc, blend);
}

/* Bloom and the composite into `target` at (x, y), then the state the HUD
 * and menus expect back: `target` bound, the presenter's shaders and samplers. */
static void post_finish(ID3D11RenderTargetView *target, float x, float y, ID3D11Texture2D *depth, const float src[4])
{
    PostConsts pc;
    int i, p = s_vs.post > 0 && s_vs.post < POST_PRESETS ? s_vs.post : 0;
    float sharp = s_vs.sharpen >= 1 && s_vs.sharpen <= 3 ? k_sharpen_amount[s_vs.sharpen] : 0.0f;
    static const float clear0[4] = { 0, 0, 0, 0 };
    gpu_state_invalidate();
    ID3D11DeviceContext_RSSetState(s_gpu11.ctx, s_pr.rs);
    ID3D11DeviceContext_OMSetDepthStencilState(s_gpu11.ctx, s_pr.nodepth, 0);
    ID3D11DeviceContext_IASetInputLayout(s_gpu11.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_gpu11.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_gpu11.ctx, s_pr.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(s_gpu11.ctx, NULL, NULL, 0);
    memset(&pc, 0, sizeof pc);
    if (k_post_presets[p].bloom > 0.0f) {
        pc.k0[0] = k_post_presets[p].thr; pc.k0[1] = 0.12f;
        /* down: picture -> level 0 (bright pass) -> ... -> level 4 */
        for (i = 0; i < POST_LEVELS; i++) {
            UINT sw = i ? s_post.lw[i - 1] : s_post.w, sh = i ? s_post.lh[i - 1] : s_post.h;
            pc.sz[0] = 1.0f / (float)sw; pc.sz[1] = 1.0f / (float)sh;
            post_pass(i ? s_post.down : s_post.bright, s_post.lv_rtv[i], 0, 0, s_post.lw[i], s_post.lh[i],
                      i ? s_post.lv_srv[i - 1] : s_post.scene_srv, NULL, &pc, NULL);
        }
        /* up: each level added onto the next larger one */
        for (i = POST_LEVELS - 1; i > 0; i--) {
            pc.sz[0] = 1.0f / (float)s_post.lw[i]; pc.sz[1] = 1.0f / (float)s_post.lh[i];
            post_pass(s_post.up, s_post.lv_rtv[i - 1], 0, 0, s_post.lw[i - 1], s_post.lh[i - 1],
                      s_post.lv_srv[i], NULL, &pc, s_post.add);
        }
    } else
        ID3D11DeviceContext_ClearRenderTargetView(s_gpu11.ctx, s_post.lv_rtv[0], clear0);
    {   /* ambient occlusion: distance at 1/2, then 1/4 and 1/8 */
        ID3D11ShaderResourceView *dsrv = NULL;
        float aos = s_vs.ao == 1 ? 0.35f : s_vs.ao == 2 ? 0.6f : 0.0f;
        s_post_ao_on = 0;
        if (aos > 0.0f && depth &&
            SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)depth, NULL, &dsrv))) {
            D3D11_TEXTURE2D_DESC dd;
            ID3D11Texture2D_GetDesc(depth, &dd);
            memset(&pc, 0, sizeof pc);
            memcpy(pc.k3, src, sizeof pc.k3);
            (void)dd;
            /* the four taps a quarter output pixel apart, in surface uv */
            pc.sz[0] = 0.5f * src[2] / (float)s_post.aw[0]; pc.sz[1] = 0.5f * src[3] / (float)s_post.ah[0];
            post_pass(s_post.aolin, s_post.ao_rtv[0], 0, 0, s_post.aw[0], s_post.ah[0], dsrv, NULL, &pc, NULL);
            for (i = 1; i < POST_AO_LEVELS; i++) {
                pc.sz[0] = 1.0f / (float)s_post.aw[i - 1]; pc.sz[1] = 1.0f / (float)s_post.ah[i - 1];
                post_pass(s_post.down, s_post.ao_rtv[i], 0, 0, s_post.aw[i], s_post.ah[i], s_post.ao_srv[i - 1], NULL, &pc, NULL);
            }
            ID3D11ShaderResourceView_Release(dsrv);
            s_post_ao_on = 1;
            s_post_ao_strength = aos;
        }
    }
    memset(&pc, 0, sizeof pc);
    pc.k1[3] = !s_post_ao_on ? 0.0f : env_cached("RECOMP_AO_SHOW") ? -1.0f : s_post_ao_strength;
    pc.k0[3] = k_post_presets[p].bloom;
    pc.k1[0] = sharp;
    pc.k1[1] = 0.80f; pc.k1[2] = 1.0f;
    pc.k2[0] = k_post_presets[p].con; pc.k2[1] = k_post_presets[p].sat;
    pc.k2[2] = k_post_presets[p].warm; pc.k2[3] = k_post_presets[p].vig;
    pc.sz[2] = 1.0f / (float)s_post.w; pc.sz[3] = 1.0f / (float)s_post.h;
    {
        ID3D11ShaderResourceView *t[5] = { s_post.scene_srv, s_post.lv_srv[0], s_post.ao_srv[0], s_post.ao_srv[2], s_post.ao_srv[3] };
        post_pass5(s_post.comp, target, x, y, s_post.w, s_post.h, t, &pc, NULL);
    }
    /* back to the presenter's state */
    {
        ID3D11SamplerState *smp[2] = { s_pr.lin, s_pr.pt };
        ID3D11DeviceContext_PSSetConstantBuffers(s_gpu11.ctx, 0, 1, &s_pr.cb);
        ID3D11DeviceContext_PSSetSamplers(s_gpu11.ctx, 0, 2, smp);
        ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, s_pr.ps, NULL, 0);
    }
}
