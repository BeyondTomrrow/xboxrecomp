/* ── SDL3 GPU renderer: device and entry points ─────────────────────────
 *
 * Hardware rendering for non-Windows hosts through SDL3's GPU API (Vulkan
 * on Linux). Design, ownership rules and milestones:
 * docs/sdl-gpu-renderer.md.
 *
 * Included into nv2a_pb_exec.c (through nv2a_gpu_sdl3.inc.c) because it
 * reads the executor's state (s_gpu, s_vp, dma_resolve, ...). Every function
 * here runs on the executor's thread, so the renderer keeps no lock.
 *
 * Off unless RECOMP_SDLGPU=1 while milestones land.
 */

#include <stddef.h>     /* offsetof */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#define SG_MAX_SURFACES 8

typedef enum {
    SG_OWNER_GUEST,   /* guest RAM is current */
    SG_OWNER_GPU      /* the texture is newer; download before the CPU reads */
} SgOwner;

typedef struct {
    uint32_t        guest;      /* guest VA of the colour buffer */
    uint32_t        pitch;      /* bytes per row in guest RAM */
    uint32_t        w, h;       /* pixels */
    SDL_GPUTexture *color;
    SgOwner         owner;      /* applies to colour only, see below */
    /* Depth and stencil live only on the GPU. In software they are host
     * arrays (s_zbuf, s_sbuf), never guest RAM, so nothing ever downloads
     * them. While this renderer is on, the GPU copy is authoritative: a
     * depth-tested batch that falls back to software tests against its own
     * (stale) buffer. Fallbacks are rare; see docs/sdl-gpu-renderer.md. */
    SDL_GPUTexture *depth;
    int             depth_ready; /* depth/stencil hold data (else clear before use) */
    uint32_t        last_use;   /* s_sg.tick when last bound, for eviction */
} SgSurface;

#define SG_MAX_PIPELINES 64

/* XfVert z and the depth clear value are in Z24 units: s_zbuf holds
 * 0..2^24-1 (zbuf_clear_bits). The GPU stores z / SG_DEPTH_MAX. */
#define SG_DEPTH_MAX 16777215.0f

/* One vertex of a flat 2D triangle (shaders/flat2d.vert). */
typedef struct {
    float x, y;          /* surface pixels */
    float color[4];      /* the triangle's flat colour, RGBA 0..1 */
    float uv[3];         /* texels; uv[2] = 1 when the triangle is textured */
} SgVertex;

/* One vertex of a 3D triangle (shaders/xf.vert): an XfVert that already went
 * through the vertex program on the CPU. */
typedef struct {
    float pos[4];        /* screen x, y (pixels), z (Z24 depth units), w */
    float col[4];        /* oD0, RGBA 0..1 */
    float spec[4];       /* oD1, RGBA 0..1 */
    float uv[4][2];      /* oT0..oT3, in texels of their stage */
} SgXfVertex;

/* Everything a pipeline bakes in. Compared with memcmp, so it has no padding
 * (only uint32_t members). Field layout: sdlgpu_pipeline.inc.c. */
typedef struct {
    uint32_t color;      /* blend, write mask, kind */
    uint32_t depth;      /* test, write, compare */
    uint32_t stencil;    /* test, compare, fail/zfail/pass ops */
    uint32_t masks;      /* stencil compare mask, write mask */
} SgPipeKey;

typedef struct {
    SgPipeKey                key;
    SDL_GPUGraphicsPipeline *pipe;
} SgPipeline;


static struct {
    int                    tried;      /* device creation attempted */
    SDL_GPUDevice         *dev;
    SDL_GPUTextureFormat depth_format;
    SgSurface              surf[SG_MAX_SURFACES];
    uint32_t               tick;

    /* Transfers between guest RAM and the GPU, reused and grown on demand. */
    SDL_GPUTransferBuffer *download;
    uint32_t               download_size;
    SDL_GPUTransferBuffer *upload;
    uint32_t               upload_size;

    /* Flat 2D drawing (sdlgpu_draw.inc.c). */
    int                    draw_ready;   /* shaders loaded: draws can be taken */
    SDL_GPUShader         *flat2d_vs, *flat2d_fs;
    SDL_GPUShader         *xf_vs, *xf_fs;
    SgPipeline             pipes[SG_MAX_PIPELINES];
    int                    pipe_count;
    SDL_GPUBuffer         *vertices;
    uint32_t               vertices_size;
    SDL_GPUSampler        *point;        /* texelFetch ignores it; SDL needs a pair */
    SDL_GPUTexture        *white;        /* bound when a batch has no texture */
} s_sg;

static int sg_wanted(void)
{
    static int wanted = -1;
    if (wanted < 0) {
        const char *v = getenv("RECOMP_SDLGPU");
        wanted = v && *v && strcmp(v, "0") != 0;
    }
    return wanted;
}

/* Lazily create the device once the game window exists. SDL's video
 * subsystem is started by the window thread, and SDL_WasInit() already
 * reports it while that start-up is still running: a device requested then
 * fails with "Video subsystem not initialized". The window is the reliable
 * signal. Until it exists every call falls back to software. */
static int sg_ready(void)
{
    if (s_sg.dev)
        return 1;
    if (!sg_wanted() || s_sg.tried || !xbox_FramebufferWindowHwnd())
        return 0;
    s_sg.tried = 1;
    s_sg.dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,
                                   getenv("RECOMP_SDLGPU_DEBUG") != NULL, NULL);
    if (!s_sg.dev) {
        fprintf(stderr, "  [SDLGPU] no GPU device (%s); software rendering\n", SDL_GetError());
        return 0;
    }
    /* 32-bit float depth keeps the software path's float comparisons
     */
    s_sg.depth_format = SDL_GPUTextureSupportsFormat(s_sg.dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
                                                 SDL_GPU_TEXTURETYPE_2D,
                                                 SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                  ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT
                  : SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    fprintf(stderr, "  [SDLGPU] hardware rendering via %s\n", SDL_GetGPUDeviceDriver(s_sg.dev));
    return 1;
}

#include "sdlgpu_surface.inc.c"
#include "sdlgpu_texture.inc.c"
#include "sdlgpu_pipeline.inc.c"
#include "sdlgpu_draw.inc.c"
#include "sdlgpu_xf.inc.c"

/* ── The five entry points (docs/sdl-gpu-renderer.md, "The contract") ── */

static void sg_cpu_access(void)
{
    if (s_sg.dev)
        sg_download_all();
}

static int sg_draw_batch(int flat2d)
{
    if (!flat2d)
        return sg_ready() && sg_draw_xf();
    return sg_ready() && sg_draw_flat2d();
}

static int sg_clear(uint32_t param)
{
    return sg_ready() && sg_clear_surface(param);
}

static void sg_flip(void)
{
    /* The presenter shows guest RAM (snapshot at flip), so the frame must be
     * there before nv2a_pb_exec takes that snapshot. */
    if (s_sg.dev)
        sg_download_all();
}
