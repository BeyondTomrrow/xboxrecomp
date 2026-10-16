#version 450
// Pre-transformed (fixed-function) NV2A vertices: screen pixels in, clip
// space out. Matches nv2a_pb_exec.c's software 2D path (raster_triangle).

layout(location = 0) in vec2 in_pos;      // surface pixels
layout(location = 1) in vec4 in_color;    // the triangle's flat colour, 0..1 RGBA
layout(location = 2) in vec3 in_uv;       // texels; z = 1 when the triangle is textured

layout(location = 0) flat out vec4 v_color;
layout(location = 1) noperspective out vec3 v_uv;   // screen-linear, as the software path

layout(set = 1, binding = 0) uniform Viewport {
    vec2 surface_size;                    // pixels
} vp;

void main()
{
    vec2 ndc = in_pos / vp.surface_size * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    v_color = in_color;
    v_uv = in_uv;
}
