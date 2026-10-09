/* ── NV2A vertex programs on the GPU ─────────────────────────────────────
 *
 * Included into nv2a_gpu_d3d11.inc.c. Opt-in with RECOMP_GPU_VP=1 while it is
 * checked against the interpreter.
 *
 * vp_run interprets every program for every vertex, and in the stadium that
 * was the largest single cost of a frame (~88,000 triangles a frame, ~23 of
 * 60 vblanks a second, 2026-09-25). Here each program is translated once
 * into an HLSL vertex shader -- instruction for instruction with vp_run's
 * decode, as xemu's glsl/vsh-prog.c does for GLSL -- compiled, and cached by
 * a hash of its words. The CPU still assembles the inputs (fetch_attr per
 * unique vertex, which knows every attribute format and the inline values);
 * the GPU runs the program and the screen-to-clip conversion gpu_vertex did.
 *
 * Not translated (the batch falls back to the interpreter): programs that
 * write constants (o-register writes with the constant bit clear).
 */

#include <stdarg.h>

/* state: 0 empty, 1 compiling (the batch uses the interpreter meanwhile),
 * 2 ready, 3 cannot be translated or compiled. Written by the compile
 * worker, read by the draw path. */
typedef struct {
    uint32_t key, start, len;
    uint32_t inmask;            /* inputs (v0-v15) the program reads */
    ID3D11VertexShader *vs;
    volatile LONG state;
} GpuVpProg;

/* Inputs the program in use reads: gpu_vp_vertex fetches only these.
 * Fetching all 16 for every vertex was ~20% of the game thread
 * (2026-10-02); most programs read 3-6. */
static uint32_t s_vp_inmask = 0xFFFFu;

/* RECOMP_GPU_RAWVB: programs decode raw guest vertex bytes (see
 * k_vp_prelude_raw). Needs shader model 5 (ByteAddressBuffer). */
static int gpu_vp_raw_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_GPU_RAWVB");
        on = v && *v && strcmp(v, "0") != 0;
        if (on && ID3D11Device_GetFeatureLevel(s_gpu11.dev) < D3D_FEATURE_LEVEL_11_0)
            on = 0;
    }
    return on;
}
static uint32_t gpu_vp_input_mask(uint32_t start);

#define GPU_VP_CACHE 2048
static GpuVpProg s_vpc[GPU_VP_CACHE];
static ID3D11InputLayout *s_vp_layout;
static ID3D11Buffer *s_vp_vb, *s_vp_ib, *s_vp_cb;
static float s_vp_in[GPU_MAX_VERTS][16][4];
static uint32_t s_vp_ix[GPU_MAX_VERTS];
static uint32_t s_vp_map[65536], s_vp_map_gen[65536], s_vp_gen;

typedef struct {
    float c[VP_CONSTS][4];
    float info[4];                       /* surface width (with the widescreen columns), height, zmax, widescreen offset */
    float clip[4];                       /* clip min, max, 1 when set */
    float fog[4];                        /* enable, mode, param0, param1 */
    float light[4];                      /* specular enable, material specular alpha */
} GpuVpConsts;

static uint32_t gpu_vp_vertex(uint32_t index, uint32_t *nv);

static int gpu_vp_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = env_cached("RECOMP_GPU_VP");
        on = v && *v && *v != '0';
    }
    return on;
}

static const char *k_vp_prelude =
    "cbuffer VC : register(b1) { float4 c[192]; float4 info; float4 clipr; float4 fogp; float4 lightp; };\n"
    "struct VSIn {\n"
    "  float4 a0:V0; float4 a1:V1; float4 a2:V2; float4 a3:V3; float4 a4:V4; float4 a5:V5; float4 a6:V6; float4 a7:V7;\n"
    "  float4 a8:V8; float4 a9:V9; float4 a10:V10; float4 a11:V11; float4 a12:V12; float4 a13:V13; float4 a14:V14; float4 a15:V15; };\n"
    "struct PIn { float4 pos:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1;\n"
    "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; float4 wz:TEXCOORD4; float4 rawscreen:TEXCOORD5;\n"
    "  nointerpolation float4 tri0:TEXCOORD6; nointerpolation float4 tri1:TEXCOORD7; nointerpolation float4 tri2:TEXCOORD8;\n"
    "  nointerpolation float2 depthmeta:TEXCOORD9; };\n"
    "float vrcp(float x) { return x == 1.0 ? 1.0 : 1.0 / x; }\n"
    "float vrcc(float x) { float r = x == 0.0 ? 1.8446743e19 : 1.0 / x;\n"
    "  if (r > 0) r = clamp(r, 5.42101e-20, 1.8446743e19); else r = clamp(r, -1.8446743e19, -5.42101e-20); return r; }\n"
    "float vrsq(float x) { x = abs(x); return x == 1.0 ? 1.0 : rsqrt(x); }\n"
    "float vclampw(float w) {\n"
    "  if (w > 0.0 || asuint(w) == 0u) return clamp(w, asfloat(0x1F800000u), asfloat(0x5F800000u));\n"
    "  return clamp(w, asfloat(0xDF800000u), asfloat(0x9F800000u));\n"
    "}\n"
    "float4 vexp(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "float4 vlog(float x) { x = abs(x); if (x == 0.0) return float4(-asfloat(0x7F800000u), 1.0, -asfloat(0x7F800000u), 1.0);\n"
    "  float e = floor(log2(x)); return float4(e, x / exp2(e), log2(x), 1.0); }\n"
    "float4 vlit(float4 s) { float sx = max(s.x, 0.0), sy = max(s.y, 0.0), sw = clamp(s.w, -127.996, 127.996);\n"
    "  return float4(1.0, sx, sx > 0 ? exp2(sw * log2(sy)) : 0.0, 1.0); }\n"
    "PIn vs_main(VSIn i) {\n"
    "  float4 v[16] = { i.a0, i.a1, i.a2, i.a3, i.a4, i.a5, i.a6, i.a7, i.a8, i.a9, i.a10, i.a11, i.a12, i.a13, i.a14, i.a15 };\n"
    "  float4 R[13]; float4 O[16]; int a0 = 0; int k;\n"
    "  [unroll] for (k = 0; k < 13; k++) R[k] = float4(0, 0, 0, 0);\n"
    "  [unroll] for (k = 0; k < 16; k++) O[k] = float4(0, 0, 0, 1);\n";

/* RECOMP_GPU_RAWVB=1: the vertex inputs are not converted on the CPU. The
 * draw copies the guest's raw vertex bytes into a ByteAddressBuffer and the
 * program decodes each input itself (vfetch), from a per-draw table in b2:
 * va[k] = { byte offset of vertex 0, stride, type | size << 8 | mode << 16 }
 * with mode 0 = no data (0,0,0,1), 1 = constant (vk[k]), 2 = array. The
 * decoding is fetch_attr's, format for format. Converting every vertex on
 * the CPU and sending 256 bytes of it was ~70% of the draw path
 * (2026-10-02). */
static const char *k_vp_prelude_raw =
    "cbuffer VC : register(b1) { float4 c[192]; float4 info; float4 clipr; float4 fogp; float4 lightp; };\n"
    "cbuffer VA : register(b2) { int4 va[16]; float4 vk[16]; };\n"
    "ByteAddressBuffer RB : register(t0);\n"
    "struct PIn { float4 pos:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1;\n"
    "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; float4 wz:TEXCOORD4; float4 rawscreen:TEXCOORD5;\n"
    "  nointerpolation float4 tri0:TEXCOORD6; nointerpolation float4 tri1:TEXCOORD7; nointerpolation float4 tri2:TEXCOORD8;\n"
    "  nointerpolation float2 depthmeta:TEXCOORD9; };\n"
    "uint rb8(int a) { return (RB.Load(a & ~3) >> ((a & 3) * 8)) & 255u; }\n"
    "uint rb16(int a) { return rb8(a) | (rb8(a + 1) << 8); }\n"
    "uint rb32(int a) { return (a & 3) == 0 ? RB.Load(a) : (rb16(a) | (rb16(a + 2) << 16)); }\n"
    "float sx16(uint v) { return (float)((int)(v << 16) >> 16); }\n"
    "float4 vfetch(int k, uint vid) {\n"
    "  int4 d = va[k]; uint ty = (uint)d.z & 255u, sz = ((uint)d.z >> 8) & 255u, md = ((uint)d.z >> 16) & 255u;\n"
    "  float4 r = float4(0, 0, 0, 1);\n"
    "  if (md == 1u) return vk[k];\n"
    "  if (md != 2u) return r;\n"
    "  int a = d.x + (int)vid * d.y;\n"
    "  if (ty == 0u) { uint w = rb32(a); float4 q = float4(w & 255u, (w >> 8) & 255u, (w >> 16) & 255u, w >> 24) / 255.0;\n"
    "    return d.y != 0 ? q.zyxw : q; }\n"
    "  if (ty == 2u) { if (sz > 0u) r.x = asfloat(rb32(a)); if (sz > 1u) r.y = asfloat(rb32(a + 4));\n"
    "    if (sz > 2u) r.z = asfloat(rb32(a + 8)); if (sz > 3u) r.w = asfloat(rb32(a + 12)); return r; }\n"
    "  if (ty == 4u) { if (sz > 0u) r.x = rb8(a) / 255.0; if (sz > 1u) r.y = rb8(a + 1) / 255.0;\n"
    "    if (sz > 2u) r.z = rb8(a + 2) / 255.0; if (sz > 3u) r.w = rb8(a + 3) / 255.0; return r; }\n"
    "  if (ty == 1u) { if (sz > 0u) r.x = max(sx16(rb16(a)) / 32767.0, -1.0); if (sz > 1u) r.y = max(sx16(rb16(a + 2)) / 32767.0, -1.0);\n"
    "    if (sz > 2u) r.z = max(sx16(rb16(a + 4)) / 32767.0, -1.0); if (sz > 3u) r.w = max(sx16(rb16(a + 6)) / 32767.0, -1.0); return r; }\n"
    "  if (ty == 5u) { if (sz > 0u) r.x = sx16(rb16(a)); if (sz > 1u) r.y = sx16(rb16(a + 2));\n"
    "    if (sz > 2u) r.z = sx16(rb16(a + 4)); if (sz > 3u) r.w = sx16(rb16(a + 6)); return r; }\n"
    "  if (ty == 6u) { uint w = rb32(a);\n"
    "    r.x = (float)((int)(w << 21) >> 21) / 1023.0; r.y = (float)((int)((w >> 11) << 21) >> 21) / 1023.0;\n"
    "    r.z = (float)((int)w >> 22) / 511.0;\n"
    "    if (d.y == 0) r.xyz = max(r.xyz, -1.0); return r; }\n"
    "  return r;\n"
    "}\n";

static const char *k_vp_epilogue =
    "  PIn o;\n"
    "  float4 P = O[0];\n"
    "  P.xy = trunc(P.xy * 16.0) / 16.0;\n"
    "  P.w = vclampw(P.w);\n"
    "  float w = P.w;\n"
    "  o.rawscreen = P; o.rawscreen.x += info.w;\n"
    "  o.tri0 = o.tri1 = o.tri2 = 0; o.depthmeta = 0;\n"
    "  float nx = (P.x + info.w) / info.x * 2.0 - 1.0, ny = 1.0 - P.y / info.y * 2.0;\n"
    "  float zc = P.z;\n"
    /* A vertex far off screen or behind the eye (huge projected x/y, or w
     * <= 0) used to be discarded, which dropped every triangle touching it:
     * the near field and stands vanished in patches (Candlestick, 2026-10-02).
     * Like xemu, hand the rasteriser homogeneous coordinates and let it clip;
     * depth stays linear (unclamped) so the clipped part interpolates
     * correctly, and the geometry shader then takes depth from the
     * rasteriser instead of the projected corners. */
    "  float nzr = zc / info.z;\n"
    "  bool farv = abs(P.x) >= 1e6 || abs(P.y) >= 1e6 || P.w <= 0.0;\n"
    "  float nz = farv ? nzr : saturate(nzr);\n"
    "  o.pos = float4(nx * w, ny * w, nz * w, w);\n"
    "  if (isnan(P.x) || isnan(P.y) || isnan(P.w)) o.pos = asfloat(0x7FC00000u).xxxx;\n"
    "  float4 d0 = O[3], d1 = O[4];\n"
    "  [unroll] for (k = 0; k < 4; k++) { if (isnan(d0[k])) d0[k] = 1.0; if (isnan(d1[k])) d1[k] = 1.0; }\n"
    "  o.d0 = saturate(d0); o.d1 = saturate(d1);\n"
    "  if (lightp.x == 0.0) o.d1 = float4(0,0,0,1);\n"
    "  else if (lightp.y == 0.0) o.d1.w = 1.0;\n"
    "  o.t0 = O[9]; o.t1 = O[10]; o.t2 = O[11]; o.t3 = O[12];\n"
    "  float fd = O[5].x, ff = 1.0, exceptional = 0.0; uint fm = asuint(fogp.y);\n"
    "  if (fogp.x != 0.0) {\n"
    "    if (fm == 0x2601u || fm == 0x804u) ff = fogp.z + fd * fogp.w - 1.0;\n"
    "    else if (fm == 0x800u || fm == 0x802u) ff = fogp.z + exp2(fd * fogp.w * 16.0) - 1.5;\n"
    "    else if (fm == 0x801u || fm == 0x803u) ff = fogp.z + exp2(-fd * fd * fogp.w * fogp.w * 32.0) - 1.5;\n"
    "    if (fm == 0x2601u || fm == 0x804u || fm == 0x800u) exceptional = 1.0;\n"
    "    if (fm == 0x804u || fm == 0x802u || fm == 0x803u) ff = abs(ff);\n"
    "    if (isinf(fd) || isnan(ff)) ff = exceptional;\n"
    "    else ff = clamp(ff, -asfloat(0x7F7FFFFFu), asfloat(0x7F7FFFFFu));\n"
    "  }\n"
    "  o.wz = float4(w, ff, 0, 1);\n"
    "  return o;\n"
    "}\n";

typedef struct { char *p; size_t n, cap; } VpBuf;

static void vpb(VpBuf *b, const char *fmt, ...)
{
    va_list ap;
    int len;
    if (b->cap - b->n < 512) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        char *np = (char *)realloc(b->p, nc);
        if (!np) return;
        b->p = np; b->cap = nc;
    }
    va_start(ap, fmt);
    len = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (len > 0) b->n += (size_t)len;
}

static void vp_src_expr(char *out, size_t n, const uint32_t *t, int mux, int reg, int neg,
                        const int swz[4], int scalar)
{
    char base[64];
    static const char comp[] = "xyzw";
    switch (mux) {
    case 1:  snprintf(base, sizeof base, reg == 12 ? "O[0]" : "R[%d]", reg < 13 ? reg : 0); break;
    case 2:  snprintf(base, sizeof base, "v[%u]", vp_field(t, 1, 9, 4)); break;
    case 3: {
        int ci = (int)vp_field(t, 1, 13, 8);
        if (vp_field(t, 3, 1, 1))
            snprintf(base, sizeof base, "c[clamp(%d + a0, 0, %d)]", ci, VP_CONSTS - 1);
        else
            snprintf(base, sizeof base, "c[%d]", ci < VP_CONSTS ? ci : VP_CONSTS - 1);
        break;
    }
    default: snprintf(base, sizeof base, "R[0]"); break;
    }
    snprintf(out, n, "%s%s.%c%c%c%c%s", neg ? "(-" : "", base,
             comp[scalar ? swz[0] : swz[0]], comp[scalar ? swz[0] : swz[1]],
             comp[scalar ? swz[0] : swz[2]], comp[scalar ? swz[0] : swz[3]], neg ? ")" : "");
}

static void vp_mask(char *out, uint32_t mask)
{
    int k = 0;
    out[k++] = '.';
    if (mask & 8) out[k++] = 'x';
    if (mask & 4) out[k++] = 'y';
    if (mask & 2) out[k++] = 'z';
    if (mask & 1) out[k++] = 'w';
    out[k] = 0;
}

/* HLSL for the program at start, or NULL when it cannot be translated.
 * *len_out: slots up to and including the final instruction. */
static char *gpu_vp_translate(uint32_t start, uint32_t *len_out)
{
    VpBuf b = { 0 };
    uint32_t slot;
    if (gpu_vp_raw_on()) {
        /* Raw inputs: same helpers as the classic prelude, then fetch only
         * the inputs the program reads. */
        const char *h0 = strstr(k_vp_prelude, "float vrcp");
        const char *h1 = strstr(k_vp_prelude, "PIn vs_main(VSIn i)");
        uint32_t mask = gpu_vp_input_mask(start), k;
        vpb(&b, "%s", k_vp_prelude_raw);
        if (h0 && h1 && h1 > h0)
            vpb(&b, "%.*s", (int)(h1 - h0), h0);
        vpb(&b, "PIn vs_main(uint vid : SV_VertexID) {\n"
                "  float4 v[16]; float4 R[13]; float4 O[16]; int a0 = 0; int k;\n"
                "  [unroll] for (k = 0; k < 16; k++) v[k] = float4(0, 0, 0, 1);\n"
                "  [unroll] for (k = 0; k < 13; k++) R[k] = float4(0, 0, 0, 0);\n"
                "  [unroll] for (k = 0; k < 16; k++) O[k] = float4(0, 0, 0, 1);\n");
        for (k = 0; k < 16; k++)
            if (mask >> k & 1)
                vpb(&b, "  v[%u] = vfetch(%u, vid);\n", k, k);
    } else
    vpb(&b, "%s", k_vp_prelude);
    for (slot = start; slot < VP_PROG_LEN; slot++) {
        const uint32_t *t = s_vp.prog[slot];
        uint32_t ilu = vp_field(t, 1, 25, 3), mac = vp_field(t, 1, 21, 4);
        int mux_a = (int)vp_field(t, 2, 26, 2), mux_b = (int)vp_field(t, 2, 11, 2);
        int mux_c = (int)vp_field(t, 3, 28, 2);
        int reg_a = (int)vp_field(t, 2, 28, 4), reg_b = (int)vp_field(t, 2, 13, 4);
        int reg_c = (int)((vp_field(t, 2, 0, 2) << 2) | vp_field(t, 3, 30, 2));
        uint32_t mac_mask = vp_field(t, 3, 24, 4), ilu_mask = vp_field(t, 3, 16, 4);
        uint32_t o_mask = vp_field(t, 3, 12, 4), out_r = vp_field(t, 3, 20, 4);
        uint32_t orb = vp_field(t, 3, 11, 1), out_addr = vp_field(t, 3, 3, 8);
        uint32_t out_mux = vp_field(t, 3, 2, 1);
        int sa[4], sb[4], sc[4];
        char A[96], B[96], C[96], mk[8];
        char dst[16];

        sa[0] = (int)vp_field(t, 1, 6, 2); sa[1] = (int)vp_field(t, 1, 4, 2);
        sa[2] = (int)vp_field(t, 1, 2, 2); sa[3] = (int)vp_field(t, 1, 0, 2);
        sb[0] = (int)vp_field(t, 2, 23, 2); sb[1] = (int)vp_field(t, 2, 21, 2);
        sb[2] = (int)vp_field(t, 2, 19, 2); sb[3] = (int)vp_field(t, 2, 17, 2);
        sc[0] = (int)vp_field(t, 2, 8, 2); sc[1] = (int)vp_field(t, 2, 6, 2);
        sc[2] = (int)vp_field(t, 2, 4, 2); sc[3] = (int)vp_field(t, 2, 2, 2);
        vp_src_expr(A, sizeof A, t, mux_a, reg_a, (int)vp_field(t, 1, 8, 1), sa, 0);
        vp_src_expr(B, sizeof B, t, mux_b, reg_b, (int)vp_field(t, 2, 25, 1), sb, 0);
        vp_src_expr(C, sizeof C, t, mux_c, reg_c, (int)vp_field(t, 2, 10, 1), sc, ilu >= 2 && ilu <= 6);

        /* A constant write needs the program to change its own constants
         * between vertices: leave those to the interpreter. */
        if (o_mask && !orb && ((out_mux == 0 && mac && mac != 13) || (out_mux == 1 && ilu))) {
            free(b.p);
            return NULL;
        }

        vpb(&b, "  { float4 A = %s, B = %s, C = %s; float4 m = float4(0,0,0,0), il = float4(0,0,0,0); int na0 = a0;\n",
            A, B, C);
        switch (mac) {
        case 1:  vpb(&b, "    m = A;\n"); break;
        case 2:  vpb(&b, "    m = A * B;\n"); break;
        case 3:  vpb(&b, "    m = A + C;\n"); break;
        case 4:  vpb(&b, "    m = A * B + C;\n"); break;
        case 5:  vpb(&b, "    m = dot(A.xyz, B.xyz).xxxx;\n"); break;
        case 6:  vpb(&b, "    m = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
        case 7:  vpb(&b, "    m = dot(A, B).xxxx;\n"); break;
        case 8:  vpb(&b, "    m = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  vpb(&b, "    m = min(A, B);\n"); break;
        case 10: vpb(&b, "    m = max(A, B);\n"); break;
        case 11: vpb(&b, "    m = (float4)(A < B);\n"); break;
        case 12: vpb(&b, "    m = (float4)(A >= B);\n"); break;
        case 13: vpb(&b, "    na0 = (int)floor(A.x + 0.001);\n"); break;
        default: break;
        }
        switch (ilu) {
        case 1: vpb(&b, "    il = C;\n"); break;
        case 2: vpb(&b, "    il = vrcp(C.x).xxxx;\n"); break;
        case 3: vpb(&b, "    il = vrcc(C.x).xxxx;\n"); break;
        case 4: vpb(&b, "    il = vrsq(C.x).xxxx;\n"); break;
        case 5: vpb(&b, "    il = vexp(C.x);\n"); break;
        case 6: vpb(&b, "    il = vlog(C.x);\n"); break;
        case 7: vpb(&b, "    il = vlit(C);\n"); break;
        default: break;
        }
        if (out_r == 12) snprintf(dst, sizeof dst, "O[0]");
        else snprintf(dst, sizeof dst, "R[%u]", out_r < 13 ? out_r : 0);
        if (mac && mac != 13 && mac_mask && !(ilu && out_r == 1)) {
            vp_mask(mk, mac_mask);
            vpb(&b, "    %s%s = m%s;\n", dst, mk, mk);
        }
        if (ilu && ilu_mask) {
            vp_mask(mk, ilu_mask);
            vpb(&b, "    %s%s = il%s;\n", mac ? "R[1]" : dst, mk, mk);
        }
        if (o_mask && orb && ((out_mux == 0 && mac && mac != 13) || (out_mux == 1 && ilu))) {
            vp_mask(mk, (out_addr & 15u) == 5 ? vp_fog_write_mask(o_mask) : o_mask);
            vpb(&b, "    O[%u]%s = %s%s;\n", out_addr & 15u, mk, out_mux ? "il" : "m", mk);
        }
        if (mac == 13)
            vpb(&b, "    a0 = na0;\n");
        vpb(&b, "  }\n");
        if (vp_field(t, 3, 0, 1))
            break;
    }
    vpb(&b, "%s", k_vp_epilogue);
    if (len_out)
        *len_out = (slot < VP_PROG_LEN ? slot + 1 : VP_PROG_LEN) - start;
    return b.p;
}

static uint32_t gpu_vp_input_mask(uint32_t start)
{
    uint32_t slot, mask = 0;
    for (slot = start; slot < VP_PROG_LEN; slot++) {
        const uint32_t *t = s_vp.prog[slot];
        if (vp_field(t, 2, 26, 2) == 2 || vp_field(t, 2, 11, 2) == 2 || vp_field(t, 3, 28, 2) == 2)
            mask |= 1u << vp_field(t, 1, 9, 4);
        if (vp_field(t, 3, 0, 1))
            break;
    }
    return mask;
}

static uint32_t gpu_vp_hash(uint32_t start)
{
    uint32_t h = 2166136261u ^ start, slot, k;
    for (slot = start; slot < VP_PROG_LEN; slot++) {
        for (k = 0; k < 4; k++)
            h = (h ^ s_vp.prog[slot][k]) * 16777619u;
        if (vp_field(s_vp.prog[slot], 3, 0, 1))
            break;
    }
    return h;
}

/* Compiling a program took tens of milliseconds on the draw path, and a
 * match brings ~300 of them: nearly a third of the frame went to D3DCompile
 * (profile, 2026-09-25). Programs now compile on a worker thread -- the
 * batch draws through the interpreter until its shader is ready -- and the
 * bytecode is kept in cache\vp\ so later runs skip compiling. */
typedef struct { GpuVpProg *e; char *src; uint32_t key; } GpuVpJob;
#define GPU_VP_JOBS 1024
static GpuVpJob s_vpjob[GPU_VP_JOBS];
static volatile LONG s_vpjob_head, s_vpjob_tail;
static HANDLE s_vpjob_event;
static SRWLOCK s_vp_layout_lock = SRWLOCK_INIT;

static uint64_t gpu_vp_srchash(const char *src)
{
    uint64_t h = 1469598103934665603ull;
    for (; *src; src++)
        h = (h ^ (uint8_t)*src) * 1099511628211ull;
    return h;
}

static void gpu_vp_make_layout(const void *code, SIZE_T size)
{
    AcquireSRWLockExclusive(&s_vp_layout_lock);
    if (!s_vp_layout) {
        D3D11_INPUT_ELEMENT_DESC el[16];
        int k;
        for (k = 0; k < 16; k++) {
            el[k].SemanticName = "V"; el[k].SemanticIndex = (UINT)k;
            el[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[k].InputSlot = 0;
            el[k].AlignedByteOffset = (UINT)(k * 16);
            el[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA; el[k].InstanceDataStepRate = 0;
        }
        ID3D11Device_CreateInputLayout(s_gpu11.dev, el, 16, code, size, &s_vp_layout);
    }
    ReleaseSRWLockExclusive(&s_vp_layout_lock);
}

static void gpu_vp_compile_job(GpuVpJob *j)
{
    static volatile LONG compiled, loaded, failed;
    char path[MAX_PATH];
    uint64_t sh = gpu_vp_srchash(j->src);
    ID3D11VertexShader *vs = NULL;
    void *code = NULL;
    SIZE_T size = 0;
    ID3DBlob *blob = NULL, *err = NULL;
    FILE *f;

    {   /* in the per-user cache folder (kernel_path.c xbox_user_path) */
        char rel[64];
        snprintf(rel, sizeof rel, "cache\\vp\\%016llX.cso", (unsigned long long)sh);
        xbox_user_path(rel, path, sizeof path);
    }
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size = (SIZE_T)ftell(f);
        fseek(f, 0, SEEK_SET);
        code = malloc(size ? size : 1);
        if (!code || fread(code, 1, size, f) != size) { free(code); code = NULL; size = 0; }
        fclose(f);
        if (code) InterlockedIncrement(&loaded);
    }
    if (!code) {
        HRESULT hr = D3DCompile(j->src, strlen(j->src), "nv2a_vp", NULL, NULL, "vs_main",
                                gpu_vp_raw_on() ? "vs_5_0" : "vs_4_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &err);
        if (FAILED(hr)) {
            if (InterlockedIncrement(&failed) <= 5)
                fprintf(stderr, "  [GPUVP] program at %u failed to compile: %s\n", j->e->start,
                        err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
            if (err) ID3D10Blob_Release(err);
            InterlockedExchange(&j->e->state, 3);
            return;
        }
        code = ID3D10Blob_GetBufferPointer(blob);
        size = ID3D10Blob_GetBufferSize(blob);
        CreateDirectoryA("cache", NULL);
        CreateDirectoryA("cache\\vp", NULL);
        f = fopen(path, "wb");
        if (f) { fwrite(code, 1, size, f); fclose(f); }
        {
            LONG n = InterlockedIncrement(&compiled);
            if (n <= 5 || (n % 50) == 0)
                fprintf(stderr, "  [GPUVP] compiled program at %u, %ld compiled, %ld from cache\\vp\n",
                        j->e->start, n, loaded);
        }
    }
    if (FAILED(ID3D11Device_CreateVertexShader(s_gpu11.dev, code, size, NULL, &vs))) {
        InterlockedExchange(&j->e->state, 3);
    } else {
        if (!gpu_vp_raw_on())
            gpu_vp_make_layout(code, size);
        j->e->vs = vs;
        MemoryBarrier();
        InterlockedExchange(&j->e->state, 2);
    }
    if (blob) ID3D10Blob_Release(blob); else free(code);
}

static DWORD WINAPI gpu_vp_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        LONG t = s_vpjob_tail;
        if (t == s_vpjob_head) {
            WaitForSingleObject(s_vpjob_event, 100);
            continue;
        }
        {
            GpuVpJob job = s_vpjob[t % GPU_VP_JOBS];
            if (InterlockedCompareExchange(&s_vpjob_tail, t + 1, t) != t)
                continue;                              /* another worker took it */
            gpu_vp_compile_job(&job);
            free(job.src);
        }
    }
}

/* The vertex shader for the current program, or NULL for the interpreter
 * (not translatable, or still compiling). */
static ID3D11VertexShader *gpu_vp_shader(void)
{
    uint32_t key = gpu_vp_hash(s_vp.start), i, base = key & (GPU_VP_CACHE - 1);
    GpuVpProg *e = NULL;
    static int workers;
    {
        const char *dump = env_cached("RECOMP_GPU_VP_DUMP_START");
        static uint8_t dumped[256];
        static DWORD dump_t0;
        const char *dump_after = env_cached("RECOMP_GPU_VP_DUMP_AFTER");   /* ms after first use */
        if (dump && !dump_t0) dump_t0 = GetTickCount();
        if (dump && s_vp.start < sizeof dumped && !dumped[s_vp.start]
                && (!dump_after || GetTickCount() - dump_t0 >= (DWORD)atoi(dump_after))
                && (uint32_t)strtoul(dump, NULL, 0) == s_vp.start) {
            uint32_t len = 0, slot;
            char *src = gpu_vp_translate(s_vp.start, &len);
            char path[96];
            FILE *f;
            dumped[s_vp.start] = 1;
            CreateDirectoryA("cache", NULL);
            snprintf(path, sizeof path, "cache\\vp-start-%u.hlsl", s_vp.start);
            f = fopen(path, "wb");
            if (f && src) { fwrite(src, 1, strlen(src), f); fclose(f); }
            else if (f) fclose(f);
            snprintf(path, sizeof path, "cache\\vp-start-%u.words.txt", s_vp.start);
            f = fopen(path, "w");
            if (f) {
                for (slot = s_vp.start; slot < s_vp.start + len; slot++)
                    fprintf(f, "%3u: %08X %08X %08X %08X\n", slot,
                            s_vp.prog[slot][0], s_vp.prog[slot][1],
                            s_vp.prog[slot][2], s_vp.prog[slot][3]);
                fclose(f);
            }
            free(src);
        }
    }
    if (env_cached("RECOMP_GPU_VP_LOG_STARTS")) {
        static uint8_t seen[256];
        if (s_vp.start < sizeof seen && !seen[s_vp.start]) {
            seen[s_vp.start] = 1;
            fprintf(stderr, "  [GPUVP] first use start=%u frame=%u prim=%u tex0=%08X/%02X/%ux%u\n",
                    s_vp.start, s_gpu11.frame, s_gpu.prim, s_gpu.tex.offset,
                    s_gpu.tex.color, s_gpu.tex.width, s_gpu.tex.height);
        }
    }
    {
        const char *skip = env_cached("RECOMP_GPU_VP_SKIP_START");
        const char *p = skip;
        while (p && *p) {
            char *end;
            unsigned long v = strtoul(p, &end, 0);
            if (end == p)
                break;
            if ((uint32_t)v == s_vp.start)
                return NULL;
            p = *end ? end + 1 : end;
        }
    }
    for (i = 0; i < 16; i++) {
        GpuVpProg *c = &s_vpc[(base + i) & (GPU_VP_CACHE - 1)];
        LONG st = c->state;
        if (st && c->key == key && c->start == s_vp.start) {
            if (st == 2) { MemoryBarrier(); s_vp_inmask = c->inmask; return c->vs; }
            return NULL;
        }
        if (!st) { e = c; break; }
    }
    if (!e) {
        /* Full: replace a finished entry, never one still compiling. */
        for (i = 0; i < 16 && !e; i++) {
            GpuVpProg *c = &s_vpc[(base + i) & (GPU_VP_CACHE - 1)];
            if (c->state >= 2) e = c;
        }
        if (!e) return NULL;
        if (e->vs) { ID3D11VertexShader_Release(e->vs); e->vs = NULL; }
    }
    e->key = key; e->start = s_vp.start;
    e->inmask = gpu_vp_input_mask(s_vp.start);
    if (s_vpjob_head - s_vpjob_tail >= GPU_VP_JOBS) {
        e->state = 0;                                  /* queue full: retry later */
        return NULL;
    }
    {
        uint32_t len = 0;
        char *src = gpu_vp_translate(s_vp.start, &len);
        if (!src) { e->state = 3; return NULL; }
        e->len = len;
        e->state = 1;
        if (!workers) {
            /* Three compilers: the game itself only ever uses one core. */
            int k;
            s_vpjob_event = CreateEventA(NULL, FALSE, FALSE, NULL);
            for (k = 0; k < 3; k++) {
                HANDLE h = CreateThread(NULL, 0, gpu_vp_worker, NULL, 0, NULL);
                if (h) CloseHandle(h);
            }
            workers = 1;
        }
        s_vpjob[s_vpjob_head % GPU_VP_JOBS].e = e;
        s_vpjob[s_vpjob_head % GPU_VP_JOBS].src = src;
        s_vpjob[s_vpjob_head % GPU_VP_JOBS].key = key;
        MemoryBarrier();
        InterlockedIncrement(&s_vpjob_head);
        SetEvent(s_vpjob_event);
    }
    return NULL;
}

/* Inputs for the batch's unique vertices and a triangle-list index buffer.
 * Returns the index count, 0 when there is nothing to draw. */
static uint32_t gpu_vp_build(uint32_t *nverts_out)
{
    const uint16_t *ix = s_gpu.idx;
    uint32_t n = s_gpu.idx_count, i, nv = 0, ni = 0;
    if (++s_vp_gen == 0) {
        memset(s_vp_map_gen, 0, sizeof s_vp_map_gen);
        s_vp_gen = 1;
    }
#define VPU(k_) gpu_vp_vertex(ix[k_], &nv)
#define VPT(a_, b_, c_) do { if (ni + 3 <= GPU_MAX_VERTS) { \
        uint32_t A_ = VPU(a_), B_ = VPU(b_), C_ = VPU(c_); \
        if (A_ != 0xFFFFFFFFu && B_ != 0xFFFFFFFFu && C_ != 0xFFFFFFFFu) { \
            s_vp_ix[ni++] = A_; s_vp_ix[ni++] = B_; s_vp_ix[ni++] = C_; } } } while (0)
#define VPL(a_, b_) do { if (ni + 2 <= GPU_MAX_VERTS) {         uint32_t A_ = VPU(a_), B_ = VPU(b_);         if (A_ != 0xFFFFFFFFu && B_ != 0xFFFFFFFFu) { s_vp_ix[ni++] = A_; s_vp_ix[ni++] = B_; } } } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) VPT(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) { if (i & 1) VPT(i + 1, i, i + 2); else VPT(i, i + 1, i + 2); }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) VPT(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) { VPT(i, i + 1, i + 2); VPT(i, i + 2, i + 3); }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) { VPT(i, i + 1, i + 3); VPT(i, i + 3, i + 2); }
        break;
    case NV_PRIM_LINES:
        for (i = 0; i + 1 < n; i += 2) VPL(i, i + 1);
        break;
    case NV_PRIM_LINE_LOOP:
    case NV_PRIM_LINE_STRIP:
        for (i = 0; i + 1 < n; i++) VPL(i, i + 1);
        if (s_gpu.prim == NV_PRIM_LINE_LOOP && n > 2) VPL(n - 1, 0);
        break;
    default:
        break;
    }
#undef VPT
#undef VPL
#undef VPU
    *nverts_out = nv;
    return ni;
}

/* Local index of guest vertex `index`, fetching its 16 inputs the first time
 * it appears in the batch. */
static uint32_t gpu_vp_vertex(uint32_t index, uint32_t *nv)
{
    uint32_t a;
    index &= 0xFFFF;
    if (s_vp_map_gen[index] == s_vp_gen)
        return s_vp_map[index];
    if (*nv >= GPU_MAX_VERTS)
        return 0xFFFFFFFFu;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_vp_inmask >> a & 1)
            fetch_attr(&s_gpu.attr[a], index, s_vp_in[*nv][a]);
    s_vp_map_gen[index] = s_vp_gen;
    s_vp_map[index] = *nv;
    return (*nv)++;
}

/* Upload the batch and set the input assembler and vertex stage. */
/* Vertex and index data go into rings, appended with NO_OVERWRITE and
 * discarded only on wrap. A WRITE_DISCARD map per batch renamed the whole
 * 3 MB buffer thousands of times a frame, and the driver stalled on it
 * (~20% of the frame, 2026-09-25). The constant buffer is re-sent only when
 * its contents change. */
#define GPU_VP_RING_VB (32u << 20)
#define GPU_VP_RING_IB (4u << 20)
static UINT s_vp_vb_pos, s_vp_ib_pos;
static GpuVpConsts s_vp_last_consts;
static INT s_vp_base_vertex;          /* where gpu_vp_bind put this batch in the rings */
static UINT s_vp_start_index;
static int s_vp_consts_valid;

static int gpu_vp_ring_write(ID3D11Buffer *buf, UINT cap, UINT *pos, const void *src, UINT size, UINT *at)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (size > cap)
        return 0;
    if (*pos + size > cap) {
        how = D3D11_MAP_WRITE_DISCARD;
        *pos = 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)buf, 0, how, 0, &m)))
        return 0;
    memcpy((uint8_t *)m.pData + *pos, src, size);
    ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)buf, 0);
    *at = *pos;
    *pos += (size + 255u) & ~255u;
    return 1;
}

/* ---- raw vertex inputs (RECOMP_GPU_RAWVB) ---- */
#define GPU_VP_RING_RAW (32u << 20)
typedef struct { int32_t va[16][4]; float vk[16][4]; } GpuVaConsts;
static ID3D11Buffer *s_vp_raw, *s_vp_vacb;
static ID3D11ShaderResourceView *s_vp_raw_srv;
static UINT s_vp_raw_pos;
static uint32_t s_vp_idx_min, s_vp_idx_max;

/* Triangle- or line-list indices straight from the guest's vertex numbers
 * (the program fetches each vertex itself), with their range. */
static uint32_t gpu_vp_build_raw(void)
{
    const uint16_t *ix = s_gpu.idx;
    uint32_t n = s_gpu.idx_count, i, ni = 0, mn = 0xFFFFFFFFu, mx = 0;
#define RV(k_) do { uint32_t v_ = ix[k_]; s_vp_ix[ni++] = v_; if (v_ < mn) mn = v_; if (v_ > mx) mx = v_; } while (0)
#define RT(a_, b_, c_) do { if (ni + 3 <= GPU_MAX_VERTS) { RV(a_); RV(b_); RV(c_); } } while (0)
#define RL(a_, b_) do { if (ni + 2 <= GPU_MAX_VERTS) { RV(a_); RV(b_); } } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:      for (i = 0; i + 2 < n; i += 3) RT(i, i + 1, i + 2); break;
    case NV_PRIM_TRIANGLE_STRIP: for (i = 0; i + 2 < n; i++) { if (i & 1) RT(i + 1, i, i + 2); else RT(i, i + 1, i + 2); } break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:        for (i = 1; i + 1 < n; i++) RT(0, i, i + 1); break;
    case NV_PRIM_QUADS:          for (i = 0; i + 3 < n; i += 4) { RT(i, i + 1, i + 2); RT(i, i + 2, i + 3); } break;
    case NV_PRIM_QUAD_STRIP:     for (i = 0; i + 3 < n; i += 2) { RT(i, i + 1, i + 3); RT(i, i + 3, i + 2); } break;
    case NV_PRIM_LINES:          for (i = 0; i + 1 < n; i += 2) RL(i, i + 1); break;
    case NV_PRIM_LINE_LOOP:
    case NV_PRIM_LINE_STRIP:     for (i = 0; i + 1 < n; i++) RL(i, i + 1);
                                 if (s_gpu.prim == NV_PRIM_LINE_LOOP && n > 2) RL(n - 1, 0);
                                 break;
    default: break;
    }
#undef RV
#undef RT
#undef RL
    s_vp_idx_min = ni ? mn : 0;
    s_vp_idx_max = ni ? mx : 0;
    return ni;
}

static uint32_t gpu_vp_attr_bytes(const VertexAttr *a)
{
    switch (a->type) {
    case 0: case 6: return 4;
    case 2: return 4 * a->size;
    case 4: return a->size;
    case 1: case 5: return 2 * a->size;
    default: return 0;
    }
}

/* Copy the guest bytes this draw's inputs read into the raw ring (each
 * overlapping range once -- interleaved inputs share one) and fill the
 * per-draw input table. Returns 0 when nothing could be uploaded. */
static int gpu_vp_raw_upload(void)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    D3D11_MAPPED_SUBRESOURCE m;
    GpuVaConsts va;
    struct { uint32_t lo, hi; uint32_t ring; } sp[NV_VERTEX_ATTRS];
    int span_of[NV_VERTEX_ATTRS], nsp = 0, k, j;
    uint32_t total = 0, mn = s_vp_idx_min, mx = s_vp_idx_max;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (!s_vp_raw) {
        D3D11_BUFFER_DESC bd;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&bd, 0, sizeof bd);
        bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = GPU_VP_RING_RAW; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_vp_raw)))
            return 0;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.FirstElement = 0;
        sd.BufferEx.NumElements = GPU_VP_RING_RAW / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s_vp_raw, &sd, &s_vp_raw_srv);
        memset(&bd, 0, sizeof bd);
        bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = sizeof(GpuVaConsts); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_vp_vacb);
    }
    if (!s_vp_raw_srv || !s_vp_vacb)
        return 0;
    memset(&va, 0, sizeof va);
    for (k = 0; k < NV_VERTEX_ATTRS; k++) {
        const VertexAttr *a = &s_gpu.attr[k];
        uint32_t bytes, lo, hi;
        span_of[k] = -1;
        if (!(s_vp_inmask >> k & 1))
            continue;
        if (!a->size) {                                   /* constant (SET_VERTEX_DATA) */
            memcpy(va.vk[k], s_attr_inline[k], sizeof va.vk[k]);
            va.va[k][2] = 1 << 16;
            continue;
        }
        bytes = gpu_vp_attr_bytes(a);
        if (!bytes || (!s_gpu.inline_active && !a->offset))
            continue;                                     /* no data: (0,0,0,1) */
        if (s_gpu.inline_active) {                        /* INLINE_ARRAY: offsets into the payload */
            lo = 0;
            hi = s_gpu.inline_count * 4;
        } else {
            lo = a->offset + mn * a->stride;
            hi = a->offset + mx * a->stride + bytes;
            if (hi < lo || hi - lo > (16u << 20))     /* addresses are guest VAs (0x8xxxxxxx too); the copy is guarded */
                continue;
        }
        for (j = 0; j < nsp; j++)                         /* overlaps an earlier range? */
            if (lo <= sp[j].hi + 64 && sp[j].lo <= hi + 64) {
                if (lo < sp[j].lo) sp[j].lo = lo;
                if (hi > sp[j].hi) sp[j].hi = hi;
                break;
            }
        if (j == nsp) { sp[nsp].lo = lo; sp[nsp].hi = hi; nsp++; }
        span_of[k] = j;
        va.va[k][1] = (int32_t)a->stride;
        va.va[k][2] = (int32_t)(a->type | (a->size << 8) | (2u << 16));
    }
    for (j = 0; j < nsp; j++) {                           /* ring layout, 16-byte aligned */
        sp[j].lo &= ~3u;
        sp[j].ring = total;
        total += ((sp[j].hi - sp[j].lo) + 15u) & ~15u;
    }
    if (total > GPU_VP_RING_RAW / 2)
        return 0;
    if (total) {
        if (s_vp_raw_pos + total > GPU_VP_RING_RAW) { s_vp_raw_pos = 0; how = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_vp_raw, 0, how, 0, &m)))
            return 0;
        __try {
            for (j = 0; j < nsp; j++) {
                const uint8_t *src = s_gpu.inline_active ? (const uint8_t *)s_gpu.inline_buf + sp[j].lo
                                                         : mem + sp[j].lo;
                memcpy((uint8_t *)m.pData + s_vp_raw_pos + sp[j].ring, src, sp[j].hi - sp[j].lo);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_vp_raw, 0);
            return 0;
        }
        ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_vp_raw, 0);
    }
    for (k = 0; k < NV_VERTEX_ATTRS; k++) {
        if (span_of[k] < 0)
            continue;
        j = span_of[k];
        /* byte of element 0 = ring place of its range + (element 0 - range start) */
        va.va[k][0] = (int32_t)(s_vp_raw_pos + sp[j].ring) + (int32_t)(s_gpu.attr[k].offset - sp[j].lo);
    }
    s_vp_raw_pos += (total + 255u) & ~255u;
    if (FAILED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_vp_vacb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, &va, sizeof va);
    ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_vp_vacb, 0);
    return 1;
}

static int gpu_vp_bind(uint32_t nverts, uint32_t nidx, uint32_t rw, uint32_t rh, ID3D11VertexShader *vs)
{
    D3D11_MAPPED_SUBRESOURCE m;
    GpuVpConsts k;
    UINT vb_at = 0, ib_at = 0;
    if (!s_vp_vb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = GPU_VP_RING_VB; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_vp_vb);
        bd.ByteWidth = GPU_VP_RING_IB; bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_vp_ib);
        bd.ByteWidth = (sizeof(GpuVpConsts) + 15) & ~15u; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_vp_cb);
    }
    if (gpu_vp_raw_on()) {
        if (!s_vp_ib || !s_vp_cb || !gpu_vp_raw_upload())
            return 0;
    } else {
        if (!s_vp_vb || !s_vp_ib || !s_vp_cb || !s_vp_layout)
            return 0;
        if (!gpu_vp_ring_write(s_vp_vb, GPU_VP_RING_VB, &s_vp_vb_pos, s_vp_in,
                               (UINT)(nverts * sizeof s_vp_in[0]), &vb_at))
            return 0;
    }
    if (!gpu_vp_ring_write(s_vp_ib, GPU_VP_RING_IB, &s_vp_ib_pos, s_vp_ix, (UINT)(nidx * 4), &ib_at))
        return 0;
    /* The 3 KB of program constants are re-sent only when they changed
     * (s_vp_cgen); the small tail is compared directly. Copying and
     * comparing all of it every draw cost ~15 MB of memory traffic a frame
     * at ~2,500 draws (2026-10-02). */
    k.info[0] = (float)(rw + 2u * g_draw_ox); k.info[1] = (float)rh; k.info[2] = gpu_zmax(); k.info[3] = (float)g_draw_ox;
    k.clip[0] = s_vp.clip_min; k.clip[1] = s_vp.clip_max;
    k.clip[2] = s_vp.clip_max > s_vp.clip_min ? 1.0f : 0.0f; k.clip[3] = 0.0f;
    k.fog[0] = s_fog.enable ? 1.0f : 0.0f;
    memcpy(&k.fog[1], &s_fog.mode, 4);
    k.fog[2] = s_fog.p[0]; k.fog[3] = s_fog.p[1];
    k.light[0] = s_vp.specular_enable ? 1.0f : 0.0f;
    k.light[1] = (s_vp.light_control & (1u << 17)) ? 1.0f : 0.0f;
    k.light[2] = k.light[3] = 0.0f;
    {
        static uint32_t last_cgen;
        if (!s_vp_consts_valid || last_cgen != s_vp_cgen
                || memcmp(k.info, s_vp_last_consts.info, sizeof k - sizeof k.c) != 0) {
            if (FAILED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_vp_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return 0;
            memcpy(m.pData, s_vp.c, sizeof k.c);
            memcpy((uint8_t *)m.pData + sizeof k.c, k.info, sizeof k - sizeof k.c);
            ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_vp_cb, 0);
            memcpy(s_vp_last_consts.info, k.info, sizeof k - sizeof k.c);
            last_cgen = s_vp_cgen;
            s_vp_consts_valid = 1;
        }
    }
    {
        /* The rings stay bound at offset 0; the draw passes this batch's
         * place as base vertex / start index (ring writes are 256-byte
         * aligned and a vertex is 256 bytes, so both divide exactly). */
        UINT stride = sizeof s_vp_in[0], off = 0;
        s_vp_base_vertex = (INT)(vb_at / stride);
        s_vp_start_index = ib_at / 4u;
        if (gpu_vp_raw_on()) {
            /* No input assembler data: the program reads the raw ring. */
            s_vp_base_vertex = 0;
            if (s_bound.il != (ID3D11InputLayout *)(uintptr_t)2) {
                ID3D11DeviceContext_IASetInputLayout(s_gpu11.ctx, NULL);
                s_bound.il = (ID3D11InputLayout *)(uintptr_t)2;
            }
            if (s_bound.vs_srv != s_vp_raw_srv) {
                ID3D11DeviceContext_VSSetShaderResources(s_gpu11.ctx, 0, 1, &s_vp_raw_srv);
                s_bound.vs_srv = s_vp_raw_srv;
            }
            if (s_bound.vs_cb2 != s_vp_vacb) {
                ID3D11DeviceContext_VSSetConstantBuffers(s_gpu11.ctx, 2, 1, &s_vp_vacb);
                s_bound.vs_cb2 = s_vp_vacb;
            }
        } else
        if (s_bound.il != s_vp_layout) {
            ID3D11DeviceContext_IASetInputLayout(s_gpu11.ctx, s_vp_layout);
            s_bound.il = s_vp_layout;
        }
        if (s_bound.vb != s_vp_vb || s_bound.vb_stride != stride) {
            ID3D11DeviceContext_IASetVertexBuffers(s_gpu11.ctx, 0, 1, &s_vp_vb, &stride, &off);
            s_bound.vb = s_vp_vb;
            s_bound.vb_stride = stride;
        }
        if (s_bound.ib != s_vp_ib) {
            ID3D11DeviceContext_IASetIndexBuffer(s_gpu11.ctx, s_vp_ib, DXGI_FORMAT_R32_UINT, 0);
            s_bound.ib = s_vp_ib;
        }
        if (s_bound.vs != vs) {
            ID3D11DeviceContext_VSSetShader(s_gpu11.ctx, vs, NULL, 0);
            s_bound.vs = vs;
        }
        if (s_bound.vs_cb1 != s_vp_cb) {
            ID3D11DeviceContext_VSSetConstantBuffers(s_gpu11.ctx, 1, 1, &s_vp_cb);
            s_bound.vs_cb1 = s_vp_cb;
        }
    }
    return 1;
}
