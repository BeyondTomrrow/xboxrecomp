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
#include <stdarg.h>

#define SG_MAX_SURFACES 8

typedef enum {
    SG_OWNER_GUEST, /* guest RAM is current */
    SG_OWNER_GPU /* the texture is newer; download before the CPU reads */
} SgOwner;

typedef struct {
    uint32_t guest; /* guest VA of the colour buffer */
    uint32_t pitch; /* bytes per row in guest RAM */
    uint32_t w, h; /* pixels */
    SDL_GPUTexture *color;
    SgOwner owner; /* applies to colour only, see below */
    /* Depth and stencil live only on the GPU. In software they are host
     * arrays (s_zbuf, s_sbuf), never guest RAM, so nothing ever downloads
     * them. While this renderer is on, the GPU copy is authoritative: a
     * depth-tested batch that falls back to software tests against its own
     * (stale) buffer. Fallbacks are rare; see docs/sdl-gpu-renderer.md. */
    SDL_GPUTexture *depth;
    int depth_ready; /* depth/stencil hold data (else clear before use) */
    uint32_t last_use; /* s_sg.tick when last bound, for eviction */
} SgSurface;

#define SG_MAX_PIPELINES 1024

/* XfVert z and the depth clear value are in Z24 units: s_zbuf holds
 * 0..2^24-1 (zbuf_clear_bits). The GPU stores z / SG_DEPTH_MAX. */
#define SG_DEPTH_MAX 16777215.0f

/* One vertex of a flat 2D triangle (shaders/flat2d.vert). */
typedef struct {
    float x, y; /* surface pixels */
    float color[4]; /* the triangle's flat colour, RGBA 0..1 */
    float uv[3]; /* texels; uv[2] = 1 when the triangle is textured */
} SgVertex;

/* One vertex of a 3D triangle (shaders/xf.vert): an XfVert that already went
 * through the vertex program on the CPU. */
typedef struct {
    float pos[4]; /* screen x, y (pixels), z (Z24 depth units), w */
    float col[4]; /* oD0, RGBA 0..1 */
    float spec[4]; /* oD1, RGBA 0..1 */
    float tc[4][4];
    float fog;/* oT0..oT3, in texels of their stage */
} SgXfVertex;

/* Everything a pipeline bakes in. Compared with memcmp, so it has no padding
 * (only uint32_t members). Field layout: sdlgpu_pipeline.inc.c. */
typedef struct {
    uint32_t color; /* blend, write mask, kind */
    uint32_t depth; /* test, write, compare */
    uint32_t stencil; /* test, compare, fail/zfail/pass ops */
    uint32_t masks; /* stencil compare mask, write mask */
    uint32_t vs;
} SgPipeKey;

typedef struct {
    SgPipeKey key;
    SDL_GPUGraphicsPipeline *pipe;
} SgPipeline;


static struct {
    int tried; /* device creation attempted */
    SDL_GPUDevice *dev;
    SDL_GPUTextureFormat depth_format;
    SgSurface surf[SG_MAX_SURFACES];
    uint32_t tick;

    /* Transfers between guest RAM and the GPU, reused and grown on demand. */
    SDL_GPUTransferBuffer *download;
    uint32_t download_size;
    SDL_GPUTransferBuffer *upload;
    uint32_t upload_size;

    /* Flat 2D drawing (sdlgpu_draw.inc.c). */
    int draw_ready; /* shaders loaded: draws can be taken */
    SDL_GPUShader *flat2d_vs, *flat2d_fs;
    SDL_GPUShader *xf_vs, *xf_fs;
    SgPipeline pipes[SG_MAX_PIPELINES];
    int pipe_count;
    SDL_GPUBuffer *vertices;
    uint32_t vertices_size;
    SDL_GPUBuffer         *indices;      /* index data of the flush being drawn */
    uint32_t               indices_size;
    SDL_GPUSampler *point; /* texelFetch ignores it; SDL needs a pair */
    SDL_GPUTexture *white; /* bound when a batch has no texture */
} s_sg;


/* ──
 * Batches are queued, not submitted one by one: a flush uploads all their
 * vertices and new textures in one copy pass and draws them in one render
 * pass. A queue targets one surface. Flushed before anything needs the
 * GPU's results (CPU access, flip), before a clear, when the target
 * changes, and when full. */
#define SG_QUEUE_MAX_DRAWS 8192

typedef struct {
    SDL_GPUGraphicsPipeline *pipe;
    uint32_t vertex_offset;
    uint32_t vertex_count;
    SDL_GPUTexture *tex[4];
    SDL_GPUSampler *sampler[4];
    int tex_count;
    SDL_Rect scissor;
    SDL_FColor blend_constant;
    Uint8 stencil_ref;
    uint32_t        index_offset;    /* bytes into the queue's index data */
    uint32_t        index_count;     /* 32-bit indices; 0 = not indexed */
    uint32_t        vs_offset;       /* bytes into the queue's vertex-uniform data */
    uint32_t        vs_size, fs_size;
    uint8_t fs[384]; /* fits SgXfShade (304 bytes) */
} SgQueuedDraw;

typedef struct {
    SDL_GPUTexture *tex;
    uint32_t w, h;
    uint32_t offset; /* into the queue's texture data */
} SgQueuedUpload;

typedef struct {
    SgSurface *surface; /* every queued draw targets it; NULL = empty */
    int upload_surface; /* upload its guest copy before drawing */
    SgQueuedDraw *draws;
    uint32_t count, cap;
    uint8_t *vdata;
    uint32_t vbytes, vcap;
    uint8_t *tdata;
    uint32_t tbytes, tcap;
    SgQueuedUpload *uploads;
    uint8_t    *idata;    uint32_t ibytes, icap;   /* 32-bit indices */
    uint8_t    *udata;    uint32_t ubytes, ucap;   /* vertex uniforms (a program's
                                                        * 3 KB of constants) */
    uint32_t        last_vs_offset, last_vs_size;       /* reuse identical uniforms */
    uint32_t upload_count, upload_cap;
    uint32_t epoch;                          /* bumped by every flush; cache slots used
                                      * in the current epoch are in flight */
} SgQueue;

static SgQueue s_sg_queue = {.epoch = 1};

static int sg_queue_flush(void);


/* ── Statistics: how many batches the GPU took, and why the rest fell back.
 * Printed every 5 s while the renderer is on; the first thing to read when
 * a scene is slow (each fallback costs a GPU->RAM download) or drawn wrong. */
typedef enum {
    SG_FB_TARGET, /* 16-bit or otherwise unsupported render target */
    SG_FB_BLEND, /* blend state SDL cannot express (CONSTANT_ALPHA) */
    SG_FB_TEXTURE, /* a texture stage the decoder could not read */
    SG_FB_RESOURCES, /* GPU object creation or transfer failed */
    SG_FB_SHADERS, /* shaders missing or failed to load */
    SG_FB_DISABLED, /* turned off for testing (RECOMP_SDLGPU_NO3D) */
    SG_FB_COUNT
} SgFallback;

static const char *const k_sg_fallback_names[SG_FB_COUNT] = {
    "target", "blend", "texture", "resources", "shaders", "disabled",
};

static struct {
    uint32_t gpu2d, gpu3d;          /* batches drawn on the GPU */
    uint32_t gpuvp;                 /* ...of which with the vertex program on the GPU */
    uint32_t sw2d, sw3d; /* batches left to software */
    uint32_t why[SG_FB_COUNT];
    uint64_t since_ms;
} s_sg_stats;

/* Record why a batch stays in software. Returns 0 so callers can
 * `return sg_fallback(...)`. */
static int sg_fallback(SgFallback why, int xf) {
    s_sg_stats.why[why]++;
    if (xf) s_sg_stats.sw3d++;
    else s_sg_stats.sw2d++;
    return 0;
}

static void sg_stats_tick(void) {
    uint64_t now = SDL_GetTicks();
    int i;

    if (!s_sg_stats.since_ms)
        s_sg_stats.since_ms = now;
    if (now - s_sg_stats.since_ms < 5000)
        return;
    fprintf(stderr, "  [SDLGPU] 5s: 2D gpu %u sw %u | 3D gpu %u (vp %u) sw %u | fallbacks:",
        s_sg_stats.gpu2d, s_sg_stats.sw2d, s_sg_stats.gpu3d, s_sg_stats.gpuvp, s_sg_stats.sw3d);
    for (i = 0; i < SG_FB_COUNT; i++)
        if (s_sg_stats.why[i])
            fprintf(stderr, " %s %u", k_sg_fallback_names[i], s_sg_stats.why[i]);
    fprintf(stderr, "\n");
    memset(&s_sg_stats, 0, sizeof s_sg_stats);
    s_sg_stats.since_ms = now;
}

/* A GPU call failed: report it once per call site with SDL's reason, so a
 * failure shows up in the log instead of as a crash further down (SDL's
 * calls crash when handed the NULL an earlier failed call returned). */
#define sg_gpu_error(what)                                                         \
    do {                                                                           \
        static int told_;                                                          \
        if (!told_++)                                                              \
            fprintf(stderr, "  [SDLGPU] %s failed: %s\n", (what), SDL_GetError()); \
    } while (0)

static int sg_wanted(void) {
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
static int sg_ready(void) {
    if (s_sg.dev)
        return 1;
    if (!sg_wanted() || s_sg.tried || !xbox_FramebufferWindowHwnd())
        return 0;
    s_sg.tried = 1;
    s_sg.dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,
                               getenv("RECOMP_SDLGPU_DEBUG") != NULL,   // <- remove for release
                               NULL);
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
#include "sdlgpu_compile.inc.c"
#include "sdlgpu_vp.inc.c"
#include "sdlgpu_vpcache.inc.c"
#include "sdlgpu_pipeline.inc.c"
#include "sdlgpu_draw.inc.c"
#include "sdlgpu_present.inc.c"
#include "sdlgpu_xf.inc.c"


static void sg_cpu_access(void) {
    if (s_sg.dev) {
        sg_queue_flush(); /* queued draws first, then read back */
        sg_download_all();
    }
}

static int sg_draw_batch(int flat2d) {
    if (!flat2d) {
        /* RECOMP_SDLGPU_NO3D=1 keeps 3D batches in software: tells apart
         * a 3D-path bug from a 2D or clear one. */
        static int no3d = -1;
        if (no3d < 0)
            no3d = getenv("RECOMP_SDLGPU_NO3D") != NULL;
        if (no3d)
            return sg_fallback(SG_FB_DISABLED, 1);
        return sg_ready() && sg_draw_xf();
    }
    return sg_ready() && sg_draw_flat2d();
}

static int sg_clear(uint32_t param) {
    return sg_ready() && sg_clear_surface(param);
}

static void sg_flip(void) {
    /* The presenter shows guest RAM (snapshot at flip), so the frame must be
     * there before nv2a_pb_exec takes that snapshot. */
    if (s_sg.dev) {
        sg_queue_flush();
        sg_present_register();
        if (sg_present_active())
            sg_present_flip();
        else
            sg_download_all();
        sg_stats_tick();
        s_sg_frame++;
        sg_probe_flip();
    }
}
