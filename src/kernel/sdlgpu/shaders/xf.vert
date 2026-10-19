#version 450
// Vertex shader for vertex-program (3D) batches.
//
// The vertex program itself already ran on the CPU (nv2a_pb_exec.c, vp_run):
// each vertex arrives as an XfVert in screen space -- pixels for x/y, the
// title's depth value for z, and w for perspective. This shader only turns
// that back into clip space so the GPU can rasterize it.
//
// Perspective: the software rasterizer weights attributes by 1/w
// (raster_triangle_xf: pa = la * iwa ...). Writing clip = screen * w makes
// the GPU's own perspective-correct interpolation do exactly that.
//
// Depth: software interpolates z linearly in screen space. clip.z = z * w
// gives clip.z / clip.w = z, which the GPU also interpolates linearly in
// screen space. depth_scale maps the title's depth range onto 0..1.

layout(location = 0) in vec4 in_pos;     // screen x, y (pixels), z (title depth units), w
layout(location = 1) in vec4 in_col;     // oD0, 0..1 RGBA (diffuse)
layout(location = 2) in vec4 in_spec;    // oD1, 0..1 RGBA (specular; where 3D lighting lands)
layout(location = 3) in vec2 in_uv0;     // oT0, in texels of stage 0
layout(location = 4) in vec2 in_uv1;     // oT1..oT3, in texels of stages 1..3
layout(location = 5) in vec2 in_uv2;
layout(location = 6) in vec2 in_uv3;

layout(location = 0) out vec4 v_col;
layout(location = 1) out vec4 v_spec;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out vec2 v_uv2;
layout(location = 5) out vec2 v_uv3;

layout(set = 1, binding = 0, std140) uniform Viewport {
    vec2  surface_size;   // render target size in pixels
    float depth_scale;    // 1 / the title's maximum depth value
    float pad;
} vp;

void main()
{
    float w = in_pos.w;
    vec2 ndc = in_pos.xy / vp.surface_size * 2.0 - 1.0;
    // y is negated: screen y grows downward, NDC y upward (as flat2d.vert).
    gl_Position = vec4(ndc.x * w, -ndc.y * w, in_pos.z * vp.depth_scale * w, w);
    v_col  = in_col;
    v_spec = in_spec;
    v_uv0  = in_uv0;
    v_uv1  = in_uv1;
    v_uv2  = in_uv2;
    v_uv3  = in_uv3;
}