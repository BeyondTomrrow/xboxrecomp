#version 450
// Fragment shader for vertex-program (3D) batches: the NV2A register
// combiners, ported from nv2a_pb_exec.c so the GPU computes what the
// software rasterizer computes.
//
//   comb_input  -> comb_input()   one mapped input (register, channel, mapping)
//   comb_map    -> comb_map()     output scale/bias, clamped to -1..1
//   comb_stage  -> comb_stage()   one general-combiner stage (RGB or alpha)
//   comb_eval   -> comb_eval()    all stages, then the final combiner
//
// Without active combiners the pixel is the software path's fallback:
// texel MODULATE diffuse when stage 0 is textured, else diffuse alone.
// The alpha test runs last, on the 8-bit result, as shade_pixel does.
// Blending, depth and stencil are pipeline state, not shader code.

layout(location = 0) in vec4 v_col;
layout(location = 1) in vec4 v_spec;
layout(location = 2) in vec4 v_t0;
layout(location = 3) in vec4 v_t1;
layout(location = 4) in vec4 v_t2;
layout(location = 5) in vec4 v_t3;
layout(location = 6) in float v_fog;
layout(location = 7) in float v_w;

layout(location = 0) out vec4 out_color;

// Texture stages 0..3, decoded to RGBA8 on the CPU through sample_texture /
// sample_stage (sdlgpu_texture.inc.c). Unused stages get a 1x1 white texture.
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 2, binding = 3) uniform sampler2D tex3;
// Cube-map stages (SET_TEXTURE_FORMAT bit 2): the same stage's texture as a
// real cube. Unused slots get a 1x1 white cube.
layout(set = 2, binding = 4) uniform samplerCube cube0;
layout(set = 2, binding = 5) uniform samplerCube cube1;
layout(set = 2, binding = 6) uniform samplerCube cube2;
layout(set = 2, binding = 7) uniform samplerCube cube3;

// The combiner program and per-batch state. Layout must match SgXfShade in
// sdlgpu_xf.inc.c (std140: every member here is a uvec4 or array of them).
// Arrays of 8 values are packed four to a uvec4: value s is [s >> 2][s & 3].
layout(set = 3, binding = 0, std140) uniform Shade {
    uvec4 prog;           // x control (0 = combiners off), y stage_prog, z final0, w final1
    uvec4 misc;           // x specfog[0], y specfog[1], z stage flags (bit n: stage n sampled)
    uvec4 alpha;          // x alpha test on, y alpha func, z alpha ref (0..255)
    uvec4 tex[4];         // per stage: x width, y height, z wrap u, w wrap v
    uvec4 factor0[2];     // combiner constants C0, per stage (UNIQUE_C0) or [0]
    uvec4 factor1[2];     // combiner constants C1
    uvec4 color_icw[2];   // per stage: RGB input words
    uvec4 color_ocw[2];   //            RGB output words
    uvec4 alpha_icw[2];   //            alpha input words
    uvec4 alpha_ocw[2];   //            alpha output words
    uvec4 fog;            // x: fog color, low byte red
    uvec4 mode;           // per stage: texture-shader mode (stage program, 5 bits each)
    uvec4 rect;           // per stage: 1 = coordinates in texels (linear texture), 0 = normalised
    uvec4 depth;
    uvec4 dotmap;         // per stage: SET_DOT_RGBMAPPING (NV_PGRAPH_SHADERCTL)
    uvec4 sinput;         // per stage: the earlier stage a dot-product mode reads
    uvec4 cube;           // per stage: the texture is a cube map (cube0..3)
} sh;

// ---- control words: uniforms, or constants in a specialised variant ----
// The renderer compiles a variant per distinct combiner setup
// (sdlgpu_fscache.inc.c), prepending SG_SPEC and these as constants. The
// compiler then unrolls the stage loop, folds every branch below and
// resolves the register-file indices, which the generic shader -- an
// interpreter -- pays for on every pixel. P_NO_FRAGDEPTH (variant only):
// no w-buffer and no clip range, so the rasterised depth is already right
// and not writing gl_FragDepth keeps early depth testing / hidden-surface
// removal on (tile-based GPUs turn it off for shaders that write depth).
#ifndef SG_SPEC
#define P_PROG       sh.prog
#define P_SAMPLED    sh.misc.z
#define P_ALPHA_ON   sh.alpha.x
#define P_ALPHA_FUNC sh.alpha.y
#define P_CICW       sh.color_icw
#define P_COCW       sh.color_ocw
#define P_AICW       sh.alpha_icw
#define P_AOCW       sh.alpha_ocw
#define P_MODE       sh.mode
#define P_RECT       sh.rect
#define P_DEPTHX     sh.depth.x
#define P_DOTMAP     sh.dotmap
#define P_INPUT      sh.sinput
#define P_CUBE       sh.cube
#endif

// Same nudge as flat2d.frag: exact texel boundaries land on the same texel
// as the software path despite interpolation rounding.
const float TEXEL_BIAS = 1.0 / 512.0;

uint value8(uvec4 packed[2], int s) { return packed[s >> 2][s & 3]; }

// ARGB8888 word -> RGBA 0..1, as comb_unpack.
vec4 unpack_argb(uint c)
{
    return vec4(float((c >> 16) & 0xFFu), float((c >> 8) & 0xFFu),
                float(c & 0xFFu), float(c >> 24)) / 255.0;
}

// ---- texture fetch, as raster_triangle_xf + sample_texture -------------

uint wrap_coord(uint c, uint size, uint mode)
{
    if (size == 0u) return 0u;
    if (mode == 1u) return c % size;
    return c >= size ? size - 1u : c;
}

// The 3D path wraps negative coordinates into range before the unsigned
// conversion (unlike the 2D path, which clamps them to 0).
float shift_negative(float c, float size)
{
    return c < 0.0 ? c + size * (floor(-c / size) + 1.0) : c;
}

// ---- texture stages, as the D3D11 shader samples them (nv2a_gpu_d3d11.hlsl
// shade(): tex2dc, texdir, the stage-mode branches) ------------------------

vec4 texel(int s, ivec2 at)
{
    if (s == 0) return texelFetch(tex0, at, 0);
    if (s == 1) return texelFetch(tex1, at, 0);
    if (s == 2) return texelFetch(tex2, at, 0);
    return texelFetch(tex3, at, 0);
}

// Normalised coordinates -> the point-sampled texel, wrapped or clamped as
// the software path does (raster_triangle_xf + wrap_coord).
vec4 tex2d(int s, vec2 nuv)
{
    if (s == 0) return texture(tex0, nuv);
    if (s == 1) return texture(tex1, nuv);
    if (s == 2) return texture(tex2, nuv);
    return texture(tex3, nuv);
}

// Coordinates in the stage's convention: texels for a linear ("rect")
// texture, normalised otherwise (D3D11 tex2dc).
vec4 tex2dc(int s, vec2 uv)
{
    if (P_RECT[s] != 0u)
    uv /= vec2(sh.tex[s].xy);
    return tex2d(s, uv);
}

vec4 texcube(int s, vec3 d)
{
    if (s == 0) return texture(cube0, d);
    if (s == 1) return texture(cube1, d);
    if (s == 2) return texture(cube2, d);
    return texture(cube3, d);
}

// A direction: the cube map itself when the stage has one, else remapped
// onto the face of a 2D texture it points at (xemu remapCubeTo2D, D3D11
// texdir).
vec4 texdir(int s, vec3 d)
{
    if (P_CUBE[s] != 0u) return texcube(s, d);
    vec3 a = max(abs(d), vec3(1e-8));
    vec2 uv;
    if (a.x > a.y && a.x > a.z)      uv = (d.x > 0.0 ? vec2(-d.z, d.y) : vec2(d.z, d.y)) / a.x;
    else if (a.y > a.x && a.y > a.z) uv = (d.y > 0.0 ? vec2(d.x, -d.z) : vec2(d.x, d.z)) / a.y;
    else                             uv = (d.z > 0.0 ? vec2(d.x, d.y) : vec2(-d.x, d.y)) / a.z;
    return tex2d(s, uv * 0.5 + 0.5);
}

// Dot-product mappings of an earlier stage's texel (SET_DOT_RGBMAPPING),
// as nv2a_gpu_d3d11.hlsl dotmapf; the HILO modes are unimplemented, as in xemu.
float sign1(float x) { x *= 255.0; return (x - 128.0) / 127.0; }
float sign2(float x) { x *= 255.0; return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5; }
float sign3(float x) { x *= 255.0; return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }
vec3 dotmapf(uint m, vec4 c)
{
    if (m == 1u) return vec3(sign1(c.r), sign1(c.g), sign1(c.b));
    if (m == 2u) return vec3(sign2(c.r), sign2(c.g), sign2(c.b));
    if (m == 3u) return vec3(sign3(c.r), sign3(c.g), sign3(c.b));
    return c.rgb;
}

// All four texture stages, as nv2a_gpu_d3d11.hlsl shade(): plain, cube and
// passthrough modes, and the dot-product modes the field and player models
// light with (DOT_RFLCT_DIFF / DOT_RFLCT_SPEC read cube maps through normals
// built from earlier stages). Treated as plain 2D -- or as black, as this
// shader did before -- those passes lit nothing, and players looked flat.
void stage_all(inout vec4 t[4])
{
    vec4 T[4] = vec4[4](v_t0, v_t1, v_t2, v_t3);
    float dt[4] = float[4](0.0, 0.0, 0.0, 0.0);
    for (int s = 0; s < 4; s++) {
        uint m = P_MODE[s];
        vec4 pt = T[s];
        bool on = ((P_SAMPLED >> uint(s)) & 1u) != 0u;
        vec4 prev = t[min(int(P_INPUT[s]), 3)];
        int sp = max(s - 1, 0);
        if (m == 0u)       t[s] = vec4(0.0, 0.0, 0.0, 1.0);                     // NONE
        else if (m == 4u)  t[s] = pt;                                            // PASSTHRU
        else if (m == 1u || m == 2u || m == 6u || m == 7u) {                     // PROJECT2D/3D, BUMPENV (plain)
            float w = ((m == 1u || m == 2u) && pt.w != 0.0) ? pt.w : 1.0;
            t[s] = !on ? vec4(1.0) : (P_CUBE[s] != 0u ? texcube(s, pt.xyz) : tex2dc(s, pt.xy / w));
        }
        else if (m == 3u)  t[s] = on ? texdir(s, pt.xyz) : vec4(1.0);            // CUBEMAP
        else if (m == 0x11u) {                                                   // DOTPRODUCT
            dt[s] = dot(pt.xyz, dotmapf(P_DOTMAP[s], prev));
            t[s] = vec4(0.0);
        }
        else if (m == 0x09u) {                                                   // DOT_ST
            dt[s] = dot(pt.xyz, dotmapf(P_DOTMAP[s], prev));
            t[s] = tex2dc(s, vec2(dt[sp], dt[s]));
        }
        else if (m == 0x0Bu) {                                                   // DOT_RFLCT_DIFF
            dt[s] = dot(pt.xyz, dotmapf(P_DOTMAP[s], prev));
            float dn = dot(T[3].xyz, dotmapf(P_DOTMAP[3], t[min(int(P_INPUT[3]), 3)]));
            t[s] = texdir(s, vec3(dt[sp], dt[s], dn));
        }
        else if (m == 0x0Cu) {                                                   // DOT_RFLCT_SPEC
            dt[s] = dot(pt.xyz, dotmapf(P_DOTMAP[s], prev));
            vec3 n = vec3(dt[1], dt[2], dt[3]);
            vec3 e = vec3(T[1].w, T[2].w, T[3].w);
            vec3 r = 2.0 * n * dot(n, e) / max(dot(n, n), 1e-8) - e;
            t[s] = texdir(s, r);
        }
        else if (m == 0x0Du || m == 0x0Eu) {                                     // DOT_STR_3D / CUBE
            dt[s] = dot(pt.xyz, dotmapf(P_DOTMAP[s], prev));
            t[s] = texdir(s, vec3(dt[1], dt[2], dt[3]));
        }
        else if (m == 0x0Fu) t[s] = tex2d(s, prev.ar);                          // DEPENDENT_AR
        else if (m == 0x10u) t[s] = tex2d(s, prev.gb);                          // DEPENDENT_GB
        else                 t[s] = vec4(0.0, 0.0, 0.0, 1.0);                   // unimplemented
    }
}

// ---- register combiners -----------------------------------------------
// Registers, as the C code's regs[16][4]: 1 C0, 2 C1, 4 v0 (diffuse),
// 5 v1 (specular), 8..11 t0..t3, 12 r0, 13 r1, 14 V1R0 sum, 15 EF product.
// src_r is what a stage reads, dst_r what it writes; they are globals so the
// functions below can mirror the C signatures without copying 16 vec4s.
vec4 src_r[16];
vec4 dst_r[16];

// One mapped input byte: register (bits 0-3), alpha channel (bit 4),
// mapping (bits 5-7). For alpha inputs every component holds the value.
vec3 comb_input(uint in_byte, bool is_alpha)
{
    vec4 r = src_r[in_byte & 0xFu];
    bool chan_alpha = (in_byte & 0x10u) != 0u;
    uint mapping = in_byte & 0xE0u;
    vec3 x = is_alpha ? vec3(chan_alpha ? r.a : r.b)
                      : (chan_alpha ? vec3(r.a) : r.rgb);
    vec3 p = max(x, 0.0);
    switch (mapping) {
    case 0x00u: x = p; break;                        // unsigned identity
    case 0x20u: x = 1.0 - min(p, 1.0); break;        // unsigned invert
    case 0x40u: x = 2.0 * p - 1.0; break;            // expand normal
    case 0x60u: x = -2.0 * p + 1.0; break;           // expand negate
    case 0x80u: x = p - 0.5; break;                  // half-bias normal
    case 0xA0u: x = -p + 0.5; break;                 // half-bias negate
    case 0xC0u: break;                               // signed identity
    default:    x = -x; break;                       // signed negate
    }
    return x;
}

// Output scale/bias of a stage, then clamp to -1..1.
float comb_map(float x, uint mapping)
{
    switch (mapping) {
    case 0x08u: x -= 0.5; break;
    case 0x10u: x *= 2.0; break;
    case 0x18u: x = (x - 0.5) * 2.0; break;
    case 0x20u: x *= 4.0; break;
    case 0x30u: x *= 0.5; break;
    default: break;
    }
    return clamp(x, -1.0, 1.0);
}

vec3 comb_map3(vec3 v, uint mapping)
{
    return vec3(comb_map(v.x, mapping), comb_map(v.y, mapping), comb_map(v.z, mapping));
}

// One general-combiner stage: AB, CD and their sum (or mux), mapped and
// written to the registers the output word names.
void comb_stage(uint icw, uint ocw, bool is_alpha)
{
    uint flags = ocw >> 12, mapping = flags & 0x38u;
    uint ab_dst = (ocw >> 4) & 0xFu, cd_dst = ocw & 0xFu, ms_dst = (ocw >> 8) & 0xFu;
    vec3 A = comb_input((icw >> 24) & 0xFFu, is_alpha);
    vec3 B = comb_input((icw >> 16) & 0xFFu, is_alpha);
    vec3 C = comb_input((icw >> 8) & 0xFFu, is_alpha);
    vec3 D = comb_input(icw & 0xFFu, is_alpha);

    // Dot products (RGB only) replace the per-channel products.
    vec3 ab = (!is_alpha && (flags & 2u) != 0u) ? vec3(A.x * B.x + A.y * B.y + A.z * B.z) : A * B;
    vec3 cd = (!is_alpha && (flags & 1u) != 0u) ? vec3(C.x * D.x + C.y * D.y + C.z * D.z) : C * D;

    // The mux picks AB or CD by r0.a: its top bit (MUX_MSB) or its low bit.
    bool mux_cd = (P_PROG.x & 0x100u) != 0u ? src_r[12].a >= 0.5
                                              : (uint(src_r[12].a * 255.0 + 0.5) & 1u) != 0u;
    vec3 ms = (flags & 4u) != 0u ? (mux_cd ? cd : ab) : ab + cd;

    ab = comb_map3(ab, mapping);
    cd = comb_map3(cd, mapping);
    ms = comb_map3(ms, mapping);

    if (is_alpha) {
        if (ab_dst != 0u) dst_r[ab_dst].a = ab.x;
        if (cd_dst != 0u) dst_r[cd_dst].a = cd.x;
        if (ms_dst != 0u) dst_r[ms_dst].a = ms.x;
    } else {
        // Flags 0x80 / 0x40: also write the blue result into alpha.
        if (ab_dst != 0u) { dst_r[ab_dst].rgb = ab; if ((flags & 0x80u) != 0u) dst_r[ab_dst].a = ab.z; }
        if (cd_dst != 0u) { dst_r[cd_dst].rgb = cd; if ((flags & 0x40u) != 0u) dst_r[cd_dst].a = cd.z; }
        if (ms_dst != 0u) dst_r[ms_dst].rgb = ms;
    }
}

// All general stages, then the final combiner. Returns RGBA clamped to 0..1.
vec4 comb_eval(vec4 t0, vec4 v0, vec4 v1, vec4 tn[3])
{
    uint control = P_PROG.x, stage_prog = P_PROG.y;
    uint final0 = P_PROG.z, final1 = P_PROG.w;
    int stages = min(int(control & 0xFFu), 8);

    for (int i = 0; i < 16; i++)
        src_r[i] = vec4(0.0);
    src_r[4] = v0;
    src_r[5] = v1;
    src_r[3] = vec4(float(sh.fog.x & 0xFFu), float((sh.fog.x >> 8) & 0xFFu),
    float((sh.fog.x >> 16) & 0xFFu), 0.0) / 255.0;
    src_r[3].a = clamp(v_fog, 0.0, 1.0);
    src_r[8] = t0;
    for (int s = 1; s < 4; s++)
        if (((stage_prog >> uint(s * 5)) & 0x1Fu) != 0u)
            src_r[8 + s] = tn[s - 1];
    src_r[12].a = (stage_prog & 0x1Fu) != 0u ? t0.a : 1.0;   // r0.a = t0.a

    for (int s = 0; s < stages; s++) {
        int c0 = (control & 0x1000u) != 0u ? s : 0;    // UNIQUE_C0
        int c1 = (control & 0x10000u) != 0u ? s : 0;   // UNIQUE_C1
        src_r[1] = unpack_argb(value8(sh.factor0, c0));
        src_r[2] = unpack_argb(value8(sh.factor1, c1));
        dst_r = src_r;
        comb_stage(value8(P_CICW, s), value8(P_COCW, s), false);
        comb_stage(value8(P_AICW, s), value8(P_AOCW, s), true);
        src_r = dst_r;
    }

    vec4 result;
    if (final0 != 0u || final1 != 0u) {
        uint fl = final1 & 0xFFu;
        src_r[1] = unpack_argb(sh.misc.x);             // specular/fog colours
        src_r[2] = unpack_argb(sh.misc.y);
        // E and F first: their product (register 15) can feed A..D.
        vec3 E = comb_input((final1 >> 24) & 0xFFu, false);
        vec3 F = comb_input((final1 >> 16) & 0xFFu, false);
        vec3 v1c = (fl & 0x40u) != 0u ? 1.0 - src_r[5].rgb : src_r[5].rgb;
        vec3 r0c = (fl & 0x20u) != 0u ? 1.0 - src_r[12].rgb : src_r[12].rgb;
        vec3 sum = v1c + r0c;
        if ((fl & 0x80u) != 0u) sum = clamp(sum, 0.0, 1.0);
        src_r[14] = vec4(sum, 0.0);
        src_r[15] = vec4(E * F, 0.0);
        vec3 A = comb_input((final0 >> 24) & 0xFFu, false);
        vec3 B = comb_input((final0 >> 16) & 0xFFu, false);
        vec3 C = comb_input((final0 >> 8) & 0xFFu, false);
        vec3 D = comb_input(final0 & 0xFFu, false);
        vec3 G = comb_input((final1 >> 8) & 0xFFu, true);
        result = vec4(D + C * (1.0 - A) + B * A, G.x);
    } else {
        result = src_r[12];
    }
    return clamp(result, 0.0, 1.0);
}

// ---- the pixel -----------------------------------------------------------

// 0..1 -> the 8-bit value the C code stores: (uint)(x * 255 + 0.5).
uvec4 to8(vec4 c) { return uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5); }

bool alpha_pass(uint a)
{
    uint r = sh.alpha.z;
    switch (P_ALPHA_FUNC) {
    case 0x0200u: return false;
    case 0x0201u: return a <  r;
    case 0x0202u: return a == r;
    case 0x0203u: return a <= r;
    case 0x0204u: return a >  r;
    case 0x0205u: return a != r;
    case 0x0206u: return a >= r;
    default:      return true;
    }
}

void main()
{
    bool sampled0 = (P_SAMPLED & 1u) != 0u;
    vec4 ts[4] = vec4[4](vec4(0.0, 0.0, 0.0, 1.0), vec4(0.0, 0.0, 0.0, 1.0),
                         vec4(0.0, 0.0, 0.0, 1.0), vec4(0.0, 0.0, 0.0, 1.0));
    stage_all(ts);
    vec4 t0 = ts[0];
    uvec4 c8;

    if (P_PROG.x != 0u) {
        // Combiners active. Stages 1..3 read 1.0 unless they are sampled.
        vec4 tn[3] = vec4[3](ts[1], ts[2], ts[3]);
        c8 = to8(comb_eval(t0, v_col, v_spec, tn));
    } else if (sampled0) {
        // modulate(texel, argb): (t * c + 127) / 255 per channel, in integers.
        c8 = (uvec4(t0 * 255.0 + 0.5) * to8(v_col) + 127u) / 255u;
    } else {
        c8 = to8(v_col);
    }

    if (P_ALPHA_ON != 0u && !alpha_pass(c8.a))
        discard;
    out_color = vec4(c8) / 255.0;
#ifndef P_NO_FRAGDEPTH
    float zmax = uintBitsToFloat(sh.depth.y);
    float zd = (P_DEPTHX & 1u) != 0u ? v_w : gl_FragCoord.z * zmax;
    float cmin = uintBitsToFloat(sh.depth.z), cmax = uintBitsToFloat(sh.depth.w);
    if ((P_DEPTHX & 2u) != 0u && (zd < cmin || zd > cmax))
    discard;
    float zc = clamp(zd, cmin, cmax);
    // Integer depth formats (depth.x bit 2): store whole depth units, as the
    // D3D11 path and the console do. Fractions differ by a rounding step
    // between passes drawn with different programs, and the equal-depth
    // tests of model overlays (player lighting/detail passes) then fail.
    if ((P_DEPTHX & 4u) != 0u)
        zc = floor(zc);
    gl_FragDepth = clamp(zc / zmax, 0.0, 1.0);
#endif
}