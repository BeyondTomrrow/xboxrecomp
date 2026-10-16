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
 * Off unless RECOMP_SDLGPU=1 while milestones land: until draws are on the
 * GPU, GPU clears only add a download before the next software draw.
 */

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
    SgOwner         owner;
    uint32_t        last_use;   /* s_sg.tick when last bound, for eviction */
} SgSurface;

static struct {
    int                    tried;      /* device creation attempted */
    SDL_GPUDevice         *dev;
    SgSurface              surf[SG_MAX_SURFACES];
    uint32_t               tick;
    SDL_GPUTransferBuffer *download;   /* reused; grows to the largest surface */
    uint32_t               download_size;
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
    fprintf(stderr, "  [SDLGPU] hardware rendering via %s\n", SDL_GetGPUDeviceDriver(s_sg.dev));
    return 1;
}

#include "sdlgpu_surface.inc.c"

/* ── The five entry points (docs/sdl-gpu-renderer.md, "The contract") ── */

static void sg_cpu_access(void)
{
    if (s_sg.dev)
        sg_download_all();
}

static int sg_draw_batch(int flat2d)
{
    (void)flat2d;
    return 0;   /* milestone 1: no draws on the GPU yet */
}

static int sg_clear(uint32_t param)
{
    return sg_ready() && sg_clear_color(param);
}

static void sg_flip(void)
{
    /* The presenter shows guest RAM (snapshot at flip), so the frame must be
     * there before nv2a_pb_exec takes that snapshot. */
    if (s_sg.dev)
        sg_download_all();
}
