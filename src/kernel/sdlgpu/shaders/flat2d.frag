#version 450
// Shading for pre-transformed NV2A triangles, bit-exact with the software
// path in nv2a_pb_exec.c: point-sampled texel MODULATE flat colour
// ((t * c + 127) / 255 per channel), then the alpha test. Blending and the
// channel write mask are pipeline state.

layout(location = 0) flat in vec4 v_color;
layout(location = 1) noperspective in vec3 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex;   // stage 0, decoded to RGBA

layout(set = 3, binding = 0) uniform Shade {
    uvec2 tex_size;
    uvec2 tex_wrap;          // per axis: 1 = wrap, otherwise clamp (wrap_coord)
    uint  alpha_test;        // nonzero: apply alpha_func against alpha_ref
    uint  alpha_func;        // GL compare enum, 0x0200..0x0207
    uint  alpha_ref;         // 0..255
    uint  pad;
} sh;

// As raster_triangle: negative coordinates clamp to 0, then a C cast to
// uint32_t (truncation), then wrap_coord.
//
// Software interpolation computes texel boundaries exactly (a 16-texel span
// over 190 rows gives exactly 4.0 at row 77); the GPU's interpolator lands a
// hair short (3.99998) and would read the texel before. A 1/512-texel bias
// fixes those, and only differs where the exact value is within 1/512 below
// a boundary, which is far rarer.
const float TEXEL_BIAS = 1.0 / 512.0;

uint texel_coord(float c, uint size, uint wrap)
{
    uint u = uint(max(c + TEXEL_BIAS, 0.0));
    if (size == 0u) return 0u;
    if (wrap == 1u) return u % size;
    return u >= size ? size - 1u : u;
}

bool alpha_pass(uint a)
{
    uint r = sh.alpha_ref;
    switch (sh.alpha_func) {
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
    uvec4 c = uvec4(round(clamp(v_color, 0.0, 1.0) * 255.0));
    if (v_uv.z != 0.0) {
        ivec2 at = ivec2(texel_coord(v_uv.x, sh.tex_size.x, sh.tex_wrap.x),
                         texel_coord(v_uv.y, sh.tex_size.y, sh.tex_wrap.y));
        uvec4 t = uvec4(round(texelFetch(tex, at, 0) * 255.0));
        c = (t * c + 127u) / 255u;
    }
    if (sh.alpha_test != 0u && !alpha_pass(c.a))
        discard;
    out_color = vec4(c) / 255.0;
}
