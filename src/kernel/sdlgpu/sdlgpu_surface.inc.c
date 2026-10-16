/* ── SDL3 GPU renderer: render targets ──────────────────────────────────
 *
 * A render target lives in guest RAM and, once the renderer uses it, in a
 * GPU texture. SgSurface.owner says which copy is current
 * (docs/sdl-gpu-renderer.md, "Ownership of a surface").
 *
 * Milestone 0 handles 32-bit colour targets only (A8R8G8B8 / X8R8G8B8,
 * which are B,G,R,A bytes in guest RAM: SDL's B8G8R8A8_UNORM).
 */

static void sg_download(SgSurface *s);

static uint8_t *sg_guest_bytes(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static void sg_surface_release(SgSurface *s)
{
    if (s->color)
        SDL_ReleaseGPUTexture(s_sg.dev, s->color);
    memset(s, 0, sizeof *s);
}

/* The surface the executor is drawing into, created on first use.
 * NULL when the current target is something the renderer does not handle. */
static SgSurface *sg_surface_current(void)
{
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w;
    uint32_t h = s_gpu.clip_y + s_gpu.clip_h;
    uint32_t va = dma_resolve(s_gpu.color_offset);
    SgSurface *free_slot = NULL, *oldest = &s_sg.surf[0];
    SgSurface *s;
    int i;

    if (surface_bpp() != 4 || !w || !h || s_gpu.pitch < w * 4)
        return NULL;   /* 16-bit targets: later milestone */

    s_sg.tick++;
    for (i = 0; i < SG_MAX_SURFACES; i++) {
        s = &s_sg.surf[i];
        if (s->color && s->guest == va && s->pitch == s_gpu.pitch && s->w == w && s->h == h) {
            s->last_use = s_sg.tick;
            return s;
        }
        if (!s->color && !free_slot)
            free_slot = s;
        if (s->last_use < oldest->last_use)
            oldest = s;
    }

    s = free_slot;
    if (!s) {                                  /* evict the least recently used */
        s = oldest;
        if (s->owner == SG_OWNER_GPU)
            sg_download(s);
        sg_surface_release(s);
    }

    {
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = w, .height = h,
            .layer_count_or_depth = 1, .num_levels = 1,
        };
        s->color = SDL_CreateGPUTexture(s_sg.dev, &info);
    }
    if (!s->color) {
        fprintf(stderr, "  [SDLGPU] cannot create a %ux%u target: %s\n", w, h, SDL_GetError());
        return NULL;
    }
    s->guest = va;
    s->pitch = s_gpu.pitch;
    s->w = w;
    s->h = h;
    s->owner = SG_OWNER_GUEST;
    s->last_use = s_sg.tick;
    return s;
}

/* Copy a GPU-owned surface back into guest RAM and hand it to the guest. */
static void sg_download(SgSurface *s)
{
    uint32_t row = s->w * 4, bytes = row * s->h, y;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *copy;
    SDL_GPUFence *done;
    const uint8_t *src;
    uint8_t *dst;

    if (s->owner != SG_OWNER_GPU)
        return;

    if (bytes > s_sg.download_size) {
        SDL_GPUTransferBufferCreateInfo info = {
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = bytes,
        };
        if (s_sg.download)
            SDL_ReleaseGPUTransferBuffer(s_sg.dev, s_sg.download);
        s_sg.download = SDL_CreateGPUTransferBuffer(s_sg.dev, &info);
        s_sg.download_size = s_sg.download ? bytes : 0;
        if (!s_sg.download)
            return;   /* keeps GPU ownership; the next attempt retries */
    }

    cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
    copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureRegion from = { .texture = s->color, .w = s->w, .h = s->h, .d = 1 };
        SDL_GPUTextureTransferInfo to = { .transfer_buffer = s_sg.download };
        SDL_DownloadFromGPUTexture(copy, &from, &to);
    }
    SDL_EndGPUCopyPass(copy);
    done = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(s_sg.dev, true, &done, 1);
    SDL_ReleaseGPUFence(s_sg.dev, done);

    src = (const uint8_t *)SDL_MapGPUTransferBuffer(s_sg.dev, s_sg.download, false);
    dst = sg_guest_bytes(s->guest);
    for (y = 0; y < s->h; y++)
        memcpy(dst + (size_t)y * s->pitch, src + (size_t)y * row, row);
    SDL_UnmapGPUTransferBuffer(s_sg.dev, s_sg.download);
    s->owner = SG_OWNER_GUEST;
}

/* The guest copy of a surface, rows packed tight, for uploading when the
 * GPU copy is stale (a software draw or a title write happened since). */
static void sg_surface_pack(const SgSurface *s, uint8_t *dst)
{
    const uint8_t *src = sg_guest_bytes(s->guest);
    uint32_t row = s->w * 4, y;
    for (y = 0; y < s->h; y++)
        memcpy(dst + (size_t)y * row, src + (size_t)y * s->pitch, row);
}

static void sg_download_all(void)
{
    int i;
    for (i = 0; i < SG_MAX_SURFACES; i++)
        if (s_sg.surf[i].color)
            sg_download(&s_sg.surf[i]);
}

/* NV097_CLEAR_SURFACE on the GPU. Milestone 0 takes the common case: all
 * four colour channels over the whole surface, no depth or stencil. */
static int sg_clear_color(uint32_t param)
{
    uint32_t x0 = g_clear_rx & 0xFFF, x1 = ((g_clear_rx >> 16) & 0xFFF) + 1;
    uint32_t y0 = g_clear_ry & 0xFFF, y1 = ((g_clear_ry >> 16) & 0xFFF) + 1;
    SDL_GPUColorTargetInfo target = { 0 };
    SDL_GPUCommandBuffer *cmd;
    SgSurface *s;
    float c[4];

    if ((param & 0xF0u) != 0xF0u || (param & 3u))
        return 0;   /* channel subset or depth/stencil: software for now */
    s = sg_surface_current();
    if (!s)
        return 0;
    if (x0 != 0 || y0 != 0 || x1 < s->w || y1 < s->h)
        return 0;   /* partial rectangle: needs a quad, milestone 1 */

    comb_unpack(s_gpu.clear_color, c);   /* clear colour as R,G,B,A floats */
    target.texture = s->color;
    target.clear_color = (SDL_FColor){ c[0], c[1], c[2], c[3] };
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &target, 1, NULL));
    SDL_SubmitGPUCommandBuffer(cmd);
    s->owner = SG_OWNER_GPU;
    return 1;
}
