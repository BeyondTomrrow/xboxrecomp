/* SDL3 GPU renderer against a real Vulkan device (Mesa lavapipe is enough).
 *
 * Milestone 0: GPU clears and downloads match guest RAM byte for byte.
 * Milestone 1: flat 2D draws match a reference that follows the software
 * rasterizer's rules (nv2a_pb_exec.c raster_triangle/shade_pixel) exactly,
 * with two inherent exceptions:
 *  - blending: software truncates src*f/255 and dst*g/255 separately, the
 *    GPU blends in floating point; they differ by up to 2 steps in 255
 *  - pixel centres within 1/64 px of a triangle edge: software tests edges
 *    in float (and draws a quad's shared diagonal twice), a GPU snaps
 *    vertices to a 1/256 px grid; those pixels are not compared
 *
 * Supplies the executor symbols the renderer reads, then includes it. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- executor stand-ins ------------------------------------------------ */
typedef struct { int unused; } VertexAttr;
typedef struct { int valid, enabled; uint32_t width, height, addr_u, addr_v; } TexState;
static struct {
    uint32_t color_offset, pitch, clear_color;
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t idx_count;
    VertexAttr attr[1];
    TexState tex;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t blend, blend_sf, blend_df, blend_eq, blend_color;
} s_gpu;
static uint32_t g_clear_rx = 0x0FFF0000u, g_clear_ry = 0x0FFF0000u;
static uint32_t g_color_mask = 0x01010101u;
static uint8_t *g_ram;
static void *xbox_GetMemoryOffset(void) { return g_ram; }
static void *xbox_FramebufferWindowHwnd(void) { return (void *)1; }
static uint32_t dma_resolve(uint32_t offset) { return offset; }
static uint32_t surface_bpp(void) { return s_gpu.clip_w ? s_gpu.pitch / s_gpu.clip_w : 0; }
static void comb_unpack(uint32_t c, float v[4])
{
    v[0] = (float)((c >> 16) & 0xFF) / 255.0f; v[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    v[2] = (float)(c & 0xFF) / 255.0f;         v[3] = (float)(c >> 24) / 255.0f;
}

/* A scene: triangles with their flat colour, positions and texel coords. */
typedef struct { float p[3][2], uv[3][2]; uint32_t argb; int textured; } Tri;
static Tri g_tris[8];
static int g_ntris;
static uint32_t *g_texels;                       /* ARGB, g_tex_w x g_tex_h */

static int batch_is_screen_space(void) { return 1; }
typedef void (*BatchTriFn)(void *ctx, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb);
static void batch_triangles(BatchTriFn emit, void *ctx)
{
    for (int t = 0; t < g_ntris; t++)
        emit(ctx, (uint32_t)t * 3, (uint32_t)t * 3 + 1, (uint32_t)t * 3 + 2, g_tris[t].argb);
}
static int fetch_attr(const VertexAttr *a, uint32_t i, float out[4])
{
    (void)a; out[0] = g_tris[i / 3].p[i % 3][0]; out[1] = g_tris[i / 3].p[i % 3][1]; out[2] = 0; out[3] = 1;
    return 1;
}
static int fetch_texcoord(uint32_t i, float uv[2])
{
    if (!g_tris[i / 3].textured) return 0;
    uv[0] = g_tris[i / 3].uv[i % 3][0]; uv[1] = g_tris[i / 3].uv[i % 3][1];
    return 1;
}
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    if (!size) return 0;
    if (mode == 1) return c % size;
    return c >= size ? size - 1 : c;
}
static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    if (!s_gpu.tex.valid) return 0;
    u = wrap_coord(u, s_gpu.tex.width, s_gpu.tex.addr_u);
    v = wrap_coord(v, s_gpu.tex.height, s_gpu.tex.addr_v);
    *argb = g_texels[v * s_gpu.tex.width + u];
    return 1;
}

#include "kernel/sdlgpu/sdlgpu.inc.c"

/* ---- reference: the software path's rules, line for line ------------- */
static uint8_t *g_ref;                           /* reference copy of guest RAM */
static uint8_t g_edge[1024 * 1024];              /* pixel centre lies on an edge */

static uint32_t modulate(uint32_t t, uint32_t c)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8)
        out |= ((((t >> sh) & 0xFF) * ((c >> sh) & 0xFF) + 127) / 255) << sh;
    return out;
}
static uint32_t ref_factor(uint32_t f, const uint32_t s[4], const uint32_t d[4], int ch)
{
    switch (f) {
    case 0x0000: return 0;            case 0x0001: return 255;
    case 0x0302: return s[3];         case 0x0303: return 255 - s[3];
    case 0x0304: return d[3];         case 0x0305: return 255 - d[3];
    default:     return 255;
    }
    (void)ch;
}
static void ref_shade(int x, int y, uint32_t src)
{
    uint32_t *px = (uint32_t *)(g_ref + s_gpu.color_offset + (size_t)y * s_gpu.pitch) + x;
    uint32_t a = src >> 24;
    if (s_gpu.alpha_test && s_gpu.alpha_func == 0x0204 && !(a > s_gpu.alpha_ref)) return;
    if (s_gpu.blend) {
        uint32_t sv[4] = { (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF, a };
        uint32_t dv[4] = { (*px >> 16) & 0xFF, (*px >> 8) & 0xFF, *px & 0xFF, *px >> 24 };
        uint32_t out = 0;
        for (int ch = 0; ch < 4; ch++) {
            int32_t v = (int32_t)(sv[ch] * ref_factor(s_gpu.blend_sf, sv, dv, ch) / 255)
                      + (int32_t)(dv[ch] * ref_factor(s_gpu.blend_df, sv, dv, ch) / 255);
            if (v > 255) v = 255;
            out |= (uint32_t)v << (ch == 3 ? 24 : 16 - 8 * ch);
        }
        src = out;
    }
    if (g_color_mask != 0x01010101u) {
        uint32_t keep = 0;
        if (!(g_color_mask & 0x00000001u)) keep |= 0x000000FFu;
        if (!(g_color_mask & 0x00000100u)) keep |= 0x0000FF00u;
        if (!(g_color_mask & 0x00010000u)) keep |= 0x00FF0000u;
        if (!(g_color_mask & 0x01000000u)) keep |= 0xFF000000u;
        src = (*px & keep) | (src & ~keep);
    }
    *px = src;
}
static void ref_draw(void)
{
    for (int t = 0; t < g_ntris; t++) {
        const float *a = g_tris[t].p[0], *b = g_tris[t].p[1], *c = g_tris[t].p[2];
        float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        int minx = (int)floorf(fminf(a[0], fminf(b[0], c[0]))), maxx = (int)ceilf(fmaxf(a[0], fmaxf(b[0], c[0])));
        int miny = (int)floorf(fminf(a[1], fminf(b[1], c[1]))), maxy = (int)ceilf(fmaxf(a[1], fmaxf(b[1], c[1])));
        float len[3] = { hypotf(b[0] - a[0], b[1] - a[1]), hypotf(c[0] - b[0], c[1] - b[1]),
                         hypotf(a[0] - c[0], a[1] - c[1]) };
        if (area == 0.0f) continue;
        if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
        if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
        if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
        if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
        for (int y = miny; y < maxy; y++)
            for (int x = minx; x < maxx; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
                float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
                float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
                if (fabsf(w0) < len[0] / 64 || fabsf(w1) < len[1] / 64 || fabsf(w2) < len[2] / 64)
                    g_edge[y * 1024 + x] = 1;          /* within 1/64 px of an edge */
                if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))) continue;
                if (g_tris[t].textured && s_gpu.tex.valid) {
                    const float (*uv)[2] = g_tris[t].uv;
                    float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                    float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                    uint32_t texel;
                    if (su < 0) su = 0;
                    if (sv < 0) sv = 0;
                    if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                        ref_shade(x, y, modulate(texel, g_tris[t].argb));
                        continue;
                    }
                }
                ref_shade(x, y, g_tris[t].argb);
            }
    }
}

/* ---- harness ----------------------------------------------------------- */
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void target(uint32_t off, uint32_t w, uint32_t h, uint32_t pitch)
{
    s_gpu.color_offset = off; s_gpu.pitch = pitch;
    s_gpu.clip_x = 0; s_gpu.clip_w = w; s_gpu.clip_y = 0; s_gpu.clip_h = h;
}

static void quad(int t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 uint32_t argb, int textured)
{
    Tri a = { { {x0, y0}, {x1, y0}, {x1, y1} }, { {u0, v0}, {u1, v0}, {u1, v1} }, argb, textured };
    Tri b = { { {x0, y0}, {x1, y1}, {x0, y1} }, { {u0, v0}, {u1, v1}, {u0, v1} }, argb, textured };
    g_tris[t] = a; g_tris[t + 1] = b;
}

/* Fill the surface (guest and reference) with a gradient, draw the scene on
 * the GPU and in the reference, compare. */
static int run_scene(const char *name, int tolerance)
{
    const uint32_t off = s_gpu.color_offset, w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h;
    int bad = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = 0x80000000u | (x * 255 / w) << 16 | (y * 255 / h) << 8 | 0x40;
            memcpy(g_ram + off + (size_t)y * s_gpu.pitch + x * 4, &v, 4);
        }
    memcpy(g_ref + off, g_ram + off, (size_t)s_gpu.pitch * h);
    memset(g_edge, 0, sizeof g_edge);
    for (int i = 0; i < SG_MAX_SURFACES; i++)        /* the title wrote the surface */
        if (s_sg.surf[i].color) s_sg.surf[i].owner = SG_OWNER_GUEST;

    s_gpu.idx_count = (uint32_t)g_ntris * 3;
    int taken = sg_draw_batch(1);
    sg_flip();
    ref_draw();
    if (!taken) { printf("FAIL: %s: GPU did not take the batch\n", name); fails++; return 0; }

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *g = g_ram + off + (size_t)y * s_gpu.pitch + x * 4;
            const uint8_t *r = g_ref + off + (size_t)y * s_gpu.pitch + x * 4;
            if (g_edge[y * 1024 + x]) continue;
            for (int ch = 0; ch < 4; ch++)
                if (abs((int)g[ch] - (int)r[ch]) > tolerance) {
                    if (bad++ < 3)
                        printf("  %s: (%u,%u) gpu %02X%02X%02X%02X ref %02X%02X%02X%02X\n", name, x, y,
                               g[3], g[2], g[1], g[0], r[3], r[2], r[1], r[0]);
                    break;
                }
        }
    CHECK(bad == 0, "%s: %d pixels differ from the software rules", name, bad);
    return bad == 0;
}

int main(void)
{
    setenv("RECOMP_SDLGPU", "1", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("SDL_Init: %s\n", SDL_GetError()); return 2; }
    g_ram = calloc(1, 16u << 20);
    g_ref = calloc(1, 16u << 20);

    /* ---- milestone 0: clears ---- */
    target(0x100000, 640, 480, 640 * 4);
    memset(g_ram + 0x100000, 0x5A, 640 * 480 * 4);
    s_gpu.clear_color = 0xFF2040C0u;
    CHECK(sg_clear(0xF0) == 1, "full colour clear not taken by the GPU");
    sg_flip();
    {
        uint32_t px; memcpy(&px, g_ram + 0x100000 + 300 * 2560 + 200 * 4, 4);
        CHECK(px == 0xFF2040C0u, "clear colour wrong: %08X", px);
    }
    CHECK(sg_clear(0xF3) == 0, "colour+depth clear should fall back");
    g_clear_rx = (100u << 16) | 10u;
    CHECK(sg_clear(0xF0) == 0, "partial-rectangle clear should fall back");
    g_clear_rx = 0x0FFF0000u;

    /* ---- milestone 1: flat 2D draws on a 320x240 target ---- */
    target(0x400000, 320, 240, 1536);              /* padded rows, like the title */
    static uint32_t tex[8 * 8];
    for (int i = 0; i < 64; i++)                   /* checker with varying alpha */
        tex[i] = (((i % 8) + (i / 8)) & 1) ? 0xFFE03010u : (uint32_t)(0x20 + i * 3) << 24 | 0x1080F0u;
    g_texels = tex;

    g_ntris = 2; quad(0, 10, 20, 110, 70, 0, 0, 0, 0, 0xFF336699u, 0);
    run_scene("orientation + solid quad", 0);
    {
        uint32_t px; memcpy(&px, g_ram + 0x400000 + 25 * 1536 + 15 * 4, 4);
        CHECK(px == 0xFF336699u, "quad not at its top-left position (y flipped?): %08X", px);
    }

    s_gpu.tex = (TexState){ 1, 1, 8, 8, 1, 1 };    /* wrap both axes */
    g_ntris = 2; quad(0, 0, 0, 64, 48, -3.5f, 0, 28.5f, 24, 0xC0FFFFFFu, 1);
    run_scene("textured, wrapped, modulated, negative u clamps", 0);

    s_gpu.tex.addr_u = s_gpu.tex.addr_v = 3;        /* clamp */
    g_ntris = 2; quad(0, 100, 100, 180, 160, 0, 0, 16, 12, 0xFFFFFFFFu, 1);
    run_scene("textured, clamped", 0);

    s_gpu.tex.addr_u = s_gpu.tex.addr_v = 1;
    s_gpu.alpha_test = 1; s_gpu.alpha_func = 0x0204; s_gpu.alpha_ref = 0x80;
    g_ntris = 2; quad(0, 20, 120, 120, 200, 0, 0, 8, 8, 0xFFFFFFFFu, 1);
    run_scene("alpha test GREATER 0x80", 0);
    s_gpu.alpha_test = 0;

    s_gpu.blend = 1; s_gpu.blend_sf = 0x0302; s_gpu.blend_df = 0x0303; s_gpu.blend_eq = 0x8006;
    g_ntris = 2; quad(0, 40, 30, 300, 220, 0, 0, 24, 16, 0x80FFFFFFu, 1);
    run_scene("alpha blend over existing content", 2);
    s_gpu.blend = 0;

    s_gpu.tex.valid = 0;
    g_ntris = 2; quad(0, -50, -40, 400, 300, 0, 0, 0, 0, 0xFF00FF00u, 0);
    run_scene("geometry beyond the surface edges", 0);

    g_color_mask = 0x00010000u;                     /* red only */
    g_ntris = 2; quad(0, 0, 0, 320, 240, 0, 0, 0, 0, 0xFF123456u, 0);
    run_scene("colour write mask", 0);
    g_color_mask = 0x01010101u;

    g_ntris = 1;
    g_tris[0] = (Tri){ { {5.3f, 7.1f}, {290.7f, 30.2f}, {120.4f, 230.9f} }, { {0} }, 0xFF8040C0u, 0 };
    run_scene("diagonal triangle", 0);

    s_gpu.blend = 1; s_gpu.blend_sf = 0x8003; s_gpu.blend_df = 0x0303;
    g_ntris = 2; quad(0, 0, 0, 10, 10, 0, 0, 0, 0, 0xFFFFFFFFu, 0);
    s_gpu.idx_count = 6;
    CHECK(sg_draw_batch(1) == 0, "CONSTANT_ALPHA blend should fall back");
    s_gpu.blend = 0;

    printf("%s (driver %s)\n", fails ? "FAILED" : "ALL PASS", s_sg.dev ? SDL_GetGPUDeviceDriver(s_sg.dev) : "none");
    SDL_Quit();
    return fails != 0;
}
