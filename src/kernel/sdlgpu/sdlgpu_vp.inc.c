/* ── SDL3 GPU renderer: NV2A vertex programs as GLSL ────────────────────
 *
 * Each vertex program is translated once into a GLSL vertex shader --
 * instruction for instruction with vp_run's decode, as the D3D11 path does
 * in HLSL (nv2a_gpu_vp.inc.c) and xemu in glsl/vsh-prog.c -- compiled to
 * SPIR-V at run time (sdlgpu_compile.inc.c) and cached by a hash of its
 * words (step 3).
 *
 * The generated shader outputs exactly what shaders/xf.vert outputs (same
 * locations, same meaning), so shaders/xf.frag -- combiners, stage modes,
 * fog, w-buffer -- is shared by both 3D paths.
 *
 * Unlike the CPU path, the position goes to the rasteriser as homogeneous
 * coordinates and the GPU clips: a vertex far off screen or behind the eye
 * no longer distorts or drops the triangles touching it (the D3D11 fix for
 * NFL 2K5's missing field and stands, 2026-10-02).
 *
 * Not translated (the batch keeps the CPU path): programs that write
 * constants, as in the D3D11 path.
 */

/* Inputs v0..v15 (assembled on the CPU, step 3), the program's uniforms,
 * the NV2A math helpers (vp_run's edge cases: RCP of 1, RCC clamping, RSQ
 * of |x|, EXP/LOG/LIT forms), and the registers. */
static const char k_sg_vp_prelude[] =
    "#version 450\n"
    "layout(location = 0) in vec4 in_v0;   layout(location = 1) in vec4 in_v1;\n"
    "layout(location = 2) in vec4 in_v2;   layout(location = 3) in vec4 in_v3;\n"
    "layout(location = 4) in vec4 in_v4;   layout(location = 5) in vec4 in_v5;\n"
    "layout(location = 6) in vec4 in_v6;   layout(location = 7) in vec4 in_v7;\n"
    "layout(location = 8) in vec4 in_v8;   layout(location = 9) in vec4 in_v9;\n"
    "layout(location = 10) in vec4 in_v10; layout(location = 11) in vec4 in_v11;\n"
    "layout(location = 12) in vec4 in_v12; layout(location = 13) in vec4 in_v13;\n"
    "layout(location = 14) in vec4 in_v14; layout(location = 15) in vec4 in_v15;\n"
    /* Same outputs as shaders/xf.vert. */
    "layout(location = 0) out vec4 v_col;\n"
    "layout(location = 1) out vec4 v_spec;\n"
    "layout(location = 2) out vec4 v_t0;\n"
    "layout(location = 3) out vec4 v_t1;\n"
    "layout(location = 4) out vec4 v_t2;\n"
    "layout(location = 5) out vec4 v_t3;\n"
    "layout(location = 6) out float v_fog;\n"
    "layout(location = 7) out float v_w;\n"
    /* c: the 192 program constants. info: surface width, height, depth
     * range (zmax), x offset. clipr: unused here (depth range in xf.frag).
     * fogp: enable, mode (uint bits), p0, p1. lightp: specular on, light
     * control bit 17. Layout must match SgVpUniforms. */
    "layout(set = 1, binding = 0, std140) uniform VC {\n"
    "    vec4 c[192]; vec4 info; vec4 clipr; vec4 fogp; vec4 lightp;\n"
    "};\n"
    "float vrcp(float x) { return x == 1.0 ? 1.0 : 1.0 / x; }\n"
    "float vrcc(float x) {\n"
    "    float r = x == 0.0 ? 1.8446743e19 : 1.0 / x;\n"
    "    return r > 0.0 ? clamp(r, 5.42101e-20, 1.8446743e19) : clamp(r, -1.8446743e19, -5.42101e-20);\n"
    "}\n"
    "float vrsq(float x) { x = abs(x); return x == 1.0 ? 1.0 : inversesqrt(x); }\n"
    "float vclampw(float w) {\n"
    "    if (w > 0.0 || floatBitsToUint(w) == 0u)\n"
    "        return clamp(w, uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u));\n"
    "    return clamp(w, uintBitsToFloat(0xDF800000u), uintBitsToFloat(0x9F800000u));\n"
    "}\n"
    "vec4 vexp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "vec4 vlog(float x) {\n"
    "    x = abs(x);\n"
    "    if (x == 0.0) return vec4(-uintBitsToFloat(0x7F800000u), 1.0, -uintBitsToFloat(0x7F800000u), 1.0);\n"
    "    float e = floor(log2(x));\n"
    "    return vec4(e, x / exp2(e), log2(x), 1.0);\n"
    "}\n"
    "vec4 vlit(vec4 s) {\n"
    "    float sx = max(s.x, 0.0), sy = max(s.y, 0.0), sw = clamp(s.w, -127.996, 127.996);\n"
    "    return vec4(1.0, sx, sx > 0.0 ? exp2(sw * log2(sy)) : 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    vec4 v[16] = vec4[16](in_v0, in_v1, in_v2, in_v3, in_v4, in_v5, in_v6, in_v7,\n"
    "                          in_v8, in_v9, in_v10, in_v11, in_v12, in_v13, in_v14, in_v15);\n"
    "    vec4 R[13]; vec4 O[16]; int a0 = 0; int k;\n"
    "    for (k = 0; k < 13; k++) R[k] = vec4(0.0);\n"
    "    for (k = 0; k < 16; k++) O[k] = vec4(0.0, 0.0, 0.0, 1.0);\n";

/* After the program: the D3D11 epilogue (nv2a_gpu_vp.inc.c k_vp_epilogue)
 * in GLSL, writing xf.vert's outputs. O[0] holds screen x/y (already
 * divided by the program), depth and w; the rasteriser gets homogeneous
 * coordinates and clips. */
static const char k_sg_vp_epilogue[] =
    "    vec4 P = O[0];\n"
    "    P.xy = trunc(P.xy * 16.0) / 16.0;\n"                 /* NV2A 1/16-pixel grid */
    "    P.w = vclampw(P.w);\n"
    "    float w = P.w;\n"
    "    float nx = (P.x + info.w) / info.x * 2.0 - 1.0, ny = 1.0 - P.y / info.y * 2.0;\n"
    "    float nzr = P.z / info.z;\n"
    /* Depth stays unclamped for vertices off screen or behind the eye, so
     * the part the rasteriser clips interpolates correctly. */
    "    bool farv = abs(P.x) >= 1e6 || abs(P.y) >= 1e6 || P.w <= 0.0;\n"
    "    float nz = farv ? nzr : clamp(nzr, 0.0, 1.0);\n"
    "    gl_Position = vec4(nx * w, ny * w, nz * w, w);\n"
    "    if (isnan(P.x) || isnan(P.y) || isnan(P.w)) gl_Position = vec4(uintBitsToFloat(0x7FC00000u));\n"
    "    vec4 d0 = O[3], d1 = O[4];\n"
    "    for (k = 0; k < 4; k++) { if (isnan(d0[k])) d0[k] = 1.0; if (isnan(d1[k])) d1[k] = 1.0; }\n"
    "    v_col = clamp(d0, 0.0, 1.0);\n"
    "    v_spec = clamp(d1, 0.0, 1.0);\n"
    "    if (lightp.x == 0.0) v_spec = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    else if (lightp.y == 0.0) v_spec.w = 1.0;\n"
    "    v_t0 = O[9]; v_t1 = O[10]; v_t2 = O[11]; v_t3 = O[12];\n"
    /* Fog factor from oFog.x, as the D3D11 epilogue. */
    "    float fd = O[5].x, ff = 1.0, exceptional = 0.0; uint fm = floatBitsToUint(fogp.y);\n"
    "    if (fogp.x != 0.0) {\n"
    "        if (fm == 0x2601u || fm == 0x804u) ff = fogp.z + fd * fogp.w - 1.0;\n"
    "        else if (fm == 0x800u || fm == 0x802u) ff = fogp.z + exp2(fd * fogp.w * 16.0) - 1.5;\n"
    "        else if (fm == 0x801u || fm == 0x803u) ff = fogp.z + exp2(-fd * fd * fogp.w * fogp.w * 32.0) - 1.5;\n"
    "        if (fm == 0x2601u || fm == 0x804u || fm == 0x800u) exceptional = 1.0;\n"
    "        if (fm == 0x804u || fm == 0x802u || fm == 0x803u) ff = abs(ff);\n"
    "        if (isinf(fd) || isnan(ff)) ff = exceptional;\n"
    "        else ff = clamp(ff, -uintBitsToFloat(0x7F7FFFFFu), uintBitsToFloat(0x7F7FFFFFu));\n"
    "    }\n"
    "    v_fog = ff;\n"
    "    v_w = w;\n"
    "}\n";

/* The program's uniform block (VC above), std140: only vec4s, so plain
 * float arrays match it. */
typedef struct {
    float c[VP_CONSTS][4];
    float info[4];
    float clipr[4];
    float fogp[4];
    float lightp[4];
} SgVpUniforms;

/* A growing text buffer for the generated source. */
typedef struct { char *p; size_t n, cap; int failed; } SgText;

static void sg_text(SgText *b, const char *fmt, ...)
{
    va_list ap;
    int len;
    if (b->failed)
        return;
    if (b->cap - b->n < 512) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        char *np = (char *)realloc(b->p, nc);
        if (!np) { b->failed = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    va_start(ap, fmt);
    len = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (len > 0)
        b->n += (size_t)len;
}

/* One source operand: register / input / constant (relative to a0 or not),
 * negated or not, swizzled; scalar operands replicate their first lane. */
static void sg_vp_src(char *out, size_t n, const uint32_t *t, int mux, int reg, int neg,
                      const int swz[4], int scalar)
{
    static const char comp[] = "xyzw";
    char base[64];
    switch (mux) {
    case 1: snprintf(base, sizeof base, reg == 12 ? "O[0]" : "R[%d]", reg < 13 ? reg : 0); break;
    case 2: snprintf(base, sizeof base, "v[%u]", vp_field(t, 1, 9, 4)); break;
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
             comp[swz[0]], comp[scalar ? swz[0] : swz[1]],
             comp[scalar ? swz[0] : swz[2]], comp[scalar ? swz[0] : swz[3]], neg ? ")" : "");
}

/* ".xyzw" for a write mask (bit 3 = x). */
static void sg_vp_mask(char *out, uint32_t mask)
{
    int k = 0;
    out[k++] = '.';
    if (mask & 8) out[k++] = 'x';
    if (mask & 4) out[k++] = 'y';
    if (mask & 2) out[k++] = 'z';
    if (mask & 1) out[k++] = 'w';
    out[k] = 0;
}

/* GLSL for the program at `start`, malloc'd, or NULL when it cannot be
 * translated. *len_out: slots up to and including the final instruction. */
static char *sg_vp_translate(uint32_t start, uint32_t *len_out)
{
    SgText b = { 0 };
    uint32_t slot;

    sg_text(&b, "%s", k_sg_vp_prelude);
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
        char A[96], B[96], C[96], mk[8], dst[16];

        sa[0] = (int)vp_field(t, 1, 6, 2);  sa[1] = (int)vp_field(t, 1, 4, 2);
        sa[2] = (int)vp_field(t, 1, 2, 2);  sa[3] = (int)vp_field(t, 1, 0, 2);
        sb[0] = (int)vp_field(t, 2, 23, 2); sb[1] = (int)vp_field(t, 2, 21, 2);
        sb[2] = (int)vp_field(t, 2, 19, 2); sb[3] = (int)vp_field(t, 2, 17, 2);
        sc[0] = (int)vp_field(t, 2, 8, 2);  sc[1] = (int)vp_field(t, 2, 6, 2);
        sc[2] = (int)vp_field(t, 2, 4, 2);  sc[3] = (int)vp_field(t, 2, 2, 2);
        sg_vp_src(A, sizeof A, t, mux_a, reg_a, (int)vp_field(t, 1, 8, 1), sa, 0);
        sg_vp_src(B, sizeof B, t, mux_b, reg_b, (int)vp_field(t, 2, 25, 1), sb, 0);
        sg_vp_src(C, sizeof C, t, mux_c, reg_c, (int)vp_field(t, 2, 10, 1), sc, ilu >= 2 && ilu <= 6);

        /* A constant write changes the program's constants between
         * vertices: leave those programs to the CPU path. */
        if (o_mask && !orb && ((out_mux == 0 && mac && mac != 13) || (out_mux == 1 && ilu))) {
            free(b.p);
            return NULL;
        }

        sg_text(&b, "    { vec4 A = %s, B = %s, C = %s; vec4 m = vec4(0.0), il = vec4(0.0); int na0 = a0;\n",
                A, B, C);
        switch (mac) {                                  /* the multiply-add unit */
        case 1:  sg_text(&b, "      m = A;\n"); break;                                   /* MOV */
        case 2:  sg_text(&b, "      m = A * B;\n"); break;                               /* MUL */
        case 3:  sg_text(&b, "      m = A + C;\n"); break;                               /* ADD */
        case 4:  sg_text(&b, "      m = A * B + C;\n"); break;                           /* MAD */
        case 5:  sg_text(&b, "      m = vec4(dot(A.xyz, B.xyz));\n"); break;             /* DP3 */
        case 6:  sg_text(&b, "      m = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;       /* DPH */
        case 7:  sg_text(&b, "      m = vec4(dot(A, B));\n"); break;                     /* DP4 */
        case 8:  sg_text(&b, "      m = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;      /* DST */
        case 9:  sg_text(&b, "      m = min(A, B);\n"); break;                           /* MIN */
        case 10: sg_text(&b, "      m = max(A, B);\n"); break;                           /* MAX */
        case 11: sg_text(&b, "      m = vec4(lessThan(A, B));\n"); break;                /* SLT */
        case 12: sg_text(&b, "      m = vec4(greaterThanEqual(A, B));\n"); break;        /* SGE */
        case 13: sg_text(&b, "      na0 = int(floor(A.x + 0.001));\n"); break;           /* ARL */
        default: break;
        }
        switch (ilu) {                                  /* the inverse/log unit */
        case 1: sg_text(&b, "      il = C;\n"); break;                                   /* MOV */
        case 2: sg_text(&b, "      il = vec4(vrcp(C.x));\n"); break;                     /* RCP */
        case 3: sg_text(&b, "      il = vec4(vrcc(C.x));\n"); break;                     /* RCC */
        case 4: sg_text(&b, "      il = vec4(vrsq(C.x));\n"); break;                     /* RSQ */
        case 5: sg_text(&b, "      il = vexp(C.x);\n"); break;                           /* EXP */
        case 6: sg_text(&b, "      il = vlog(C.x);\n"); break;                           /* LOG */
        case 7: sg_text(&b, "      il = vlit(C);\n"); break;                             /* LIT */
        default: break;
        }
        if (out_r == 12) snprintf(dst, sizeof dst, "O[0]");
        else snprintf(dst, sizeof dst, "R[%u]", out_r < 13 ? out_r : 0);
        if (mac && mac != 13 && mac_mask && !(ilu && out_r == 1)) {
            sg_vp_mask(mk, mac_mask);
            sg_text(&b, "      %s%s = m%s;\n", dst, mk, mk);
        }
        if (ilu && ilu_mask) {                          /* paired ILU results land in R1 */
            sg_vp_mask(mk, ilu_mask);
            sg_text(&b, "      %s%s = il%s;\n", mac ? "R[1]" : dst, mk, mk);
        }
        if (o_mask && orb && ((out_mux == 0 && mac && mac != 13) || (out_mux == 1 && ilu))) {
            sg_vp_mask(mk, (out_addr & 15u) == 5 ? vp_fog_write_mask(o_mask) : o_mask);
            sg_text(&b, "      O[%u]%s = %s%s;\n", out_addr & 15u, mk, out_mux ? "il" : "m", mk);
        }
        if (mac == 13)
            sg_text(&b, "      a0 = na0;\n");
        sg_text(&b, "    }\n");
        if (vp_field(t, 3, 0, 1))                       /* final instruction */
            break;
    }
    sg_text(&b, "%s", k_sg_vp_epilogue);
    if (b.failed) {
        free(b.p);
        return NULL;
    }
    if (len_out)
        *len_out = (slot < VP_PROG_LEN ? slot + 1 : VP_PROG_LEN) - start;
    return b.p;
}

/* RECOMP_SDLGPU_VPCHECK=1: translate and compile every distinct program the
 * title runs, once, and report -- the translator checked against real
 * programs before anything draws with them. Called from sg_draw_xf. */
static void sg_vp_selfcheck(void)
{
#ifdef SG_HAVE_GLSLANG
    static int on = -1;
    static uint8_t seen[VP_PROG_LEN];
    static unsigned ok, refused, failed;
    uint32_t len = 0;

    char *src;


    if (on < 0)
        on = getenv("RECOMP_SDLGPU_VPCHECK") != NULL;
    if (!on || s_vp.start >= VP_PROG_LEN || seen[s_vp.start])
        return;
    seen[s_vp.start] = 1;
    src = sg_vp_translate(s_vp.start, &len);
    if (!src) {
        refused++;
        fprintf(stderr, "  [VPCHECK] program @%u: stays on the CPU (writes constants)\n", s_vp.start);
    } else {
        ok++;
        fprintf(stderr, "  [VPCHECK] program @%u: %u instructions translated\n", s_vp.start, len);
        free(src);
    }
    fprintf(stderr, "  [VPCHECK] so far: %u ok, %u stay on CPU, %u failed\n", ok, refused, failed);
#endif
}