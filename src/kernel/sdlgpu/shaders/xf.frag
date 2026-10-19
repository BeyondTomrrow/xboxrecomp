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
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) in vec2 v_uv2;
layout(location = 5) in vec2 v_uv3;
layout(location = 0) out vec4 out_color;

// Texture stages 0..3, decoded to RGBA8 on the CPU through sample_texture /
// sample_stage (sdlgpu_texture.inc.c). Unused stages get a 1x1 white texture.
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 2, binding = 3) uniform sampler2D tex3;

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
} sh;

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

vec4 fetch(sampler2D t, vec2 uv, uvec4 st)
{
    float u = shift_negative(uv.x + TEXEL_BIAS, float(st.x));
    float v = shift_negative(uv.y + TEXEL_BIAS, float(st.y));
    ivec2 at = ivec2(wrap_coord(uint(u), st.x, st.z), wrap_coord(uint(v), st.y, st.w));
    return texelFetch(t, at, 0);
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
    bool mux_cd = (sh.prog.x & 0x100u) != 0u ? src_r[12].a >= 0.5
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
    uint control = sh.prog.x, stage_prog = sh.prog.y;
    uint final0 = sh.prog.z, final1 = sh.prog.w;
    int stages = min(int(control & 0xFFu), 8);

    for (int i = 0; i < 16; i++)
        src_r[i] = vec4(0.0);
    src_r[4] = v0;
    src_r[5] = v1;
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
        comb_stage(value8(sh.color_icw, s), value8(sh.color_ocw, s), false);
        comb_stage(value8(sh.alpha_icw, s), value8(sh.alpha_ocw, s), true);
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
    switch (sh.alpha.y) {
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
    bool sampled0 = (sh.misc.z & 1u) != 0u;
    vec4 t0 = sampled0 ? fetch(tex0, v_uv0, sh.tex[0]) : vec4(1.0);
    uvec4 c8;

    if (sh.prog.x != 0u) {
        // Combiners active. Stages 1..3 read 1.0 unless they are sampled.
        vec4 tn[3];
        tn[0] = (sh.misc.z & 2u) != 0u ? fetch(tex1, v_uv1, sh.tex[1]) : vec4(1.0);
        tn[1] = (sh.misc.z & 4u) != 0u ? fetch(tex2, v_uv2, sh.tex[2]) : vec4(1.0);
        tn[2] = (sh.misc.z & 8u) != 0u ? fetch(tex3, v_uv3, sh.tex[3]) : vec4(1.0);
        c8 = to8(comb_eval(t0, v_col, v_spec, tn));
    } else if (sampled0) {
        // modulate(texel, argb): (t * c + 127) / 255 per channel, in integers.
        c8 = (uvec4(t0 * 255.0 + 0.5) * to8(v_col) + 127u) / 255u;
    } else {
        c8 = to8(v_col);
    }

    if (sh.alpha.x != 0u && !alpha_pass(c8.a))
        discard;
    out_color = vec4(c8) / 255.0;
}