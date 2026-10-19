/* ── SDL3 GPU renderer: render targets ──────────────────────────────────
 *
 * A render target lives in guest RAM and, once the renderer uses it, in a
 * GPU texture. SgSurface.owner says which colour copy is current
 * (docs/sdl-gpu-renderer.md, "Ownership of a surface"). Each surface also
 * has a GPU-only depth-stencil texture of the same size (see SgSurface).
 *
 * Colour targets: 32-bit only (A8R8G8B8 / X8R8G8B8, which are B,G,R,A
 * bytes in guest RAM: SDL's B8G8R8A8_UNORM). 16-bit targets stay in
 * software.
 */

static void sg_download(SgSurface *s);

static uint8_t *sg_guest_bytes(uint32_t va) {
    return (uint8_t *) xbox_GetMemoryOffset() + va;
}

static void sg_surface_release(SgSurface *s) {
    if (s->color)
        SDL_ReleaseGPUTexture(s_sg.dev, s->color);
    if (s->depth)
        SDL_ReleaseGPUTexture(s_sg.dev, s->depth);
    memset(s, 0, sizeof *s);
}

/* Colour and depth-stencil textures for a new w x h surface. */
static int sg_surface_create(SgSurface *s, uint32_t w, uint32_t h) {
    SDL_GPUTextureCreateInfo color = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1,
    };
    SDL_GPUTextureCreateInfo depth = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = s_sg.depth_format,
        .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
        .width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1,
    };
    s->color = SDL_CreateGPUTexture(s_sg.dev, &color);
    s->depth = s->color ? SDL_CreateGPUTexture(s_sg.dev, &depth) : NULL;
    if (!s->color || !s->depth) {
        fprintf(stderr, "  [SDLGPU] cannot create a %ux%u target: %s\n", w, h, SDL_GetError());
        sg_surface_release(s);
        return 0;
    }
    return 1;
}

/* The surface the executor is drawing into, created on first use.
 * NULL when the current target is something the renderer does not handle. */
static SgSurface *sg_surface_current(void) {
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w;
    uint32_t h = s_gpu.clip_y + s_gpu.clip_h;
    uint32_t va = dma_resolve(s_gpu.color_offset);
    SgSurface *free_slot = NULL, *oldest = &s_sg.surf[0];
    SgSurface *s;
    int i;

    if (surface_bpp() != 4 || !w || !h || s_gpu.pitch < w * 4)
        return NULL; /* 16-bit targets stay in software */

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
    if (!s) {
        /* evict the least recently used */
        s = oldest;
        if (s == s_sg_queue.surface)
            sg_queue_flush(); /* its queued draws land first */
        if (s->owner == SG_OWNER_GPU)
            sg_download(s);
        sg_surface_release(s);
    }
    if (!sg_surface_create(s, w, h))
        return NULL;
    s->guest = va;
    s->pitch = s_gpu.pitch;
    s->w = w;
    s->h = h;
    s->owner = SG_OWNER_GUEST;
    s->depth_ready = 0;
    s->last_use = s_sg.tick;
    return s;
}

/* Copy a GPU-owned surface back into guest RAM and hand it to the guest. */
static void sg_download(SgSurface *s) {
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
            return; /* keeps GPU ownership; the next attempt retries */
    }

    cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
    if (!cmd) {
        sg_gpu_error("download: command buffer");
        return; /* keeps GPU ownership */
    }
    copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureRegion from = {.texture = s->color, .w = s->w, .h = s->h, .d = 1};
        SDL_GPUTextureTransferInfo to = {.transfer_buffer = s_sg.download};
        SDL_DownloadFromGPUTexture(copy, &from, &to);
    }
    SDL_EndGPUCopyPass(copy);
    done = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!done) {
        sg_gpu_error("download: submit");
        return;
    }
    SDL_WaitForGPUFences(s_sg.dev, true, &done, 1);
    SDL_ReleaseGPUFence(s_sg.dev, done);

    src = (const uint8_t *) SDL_MapGPUTransferBuffer(s_sg.dev, s_sg.download, false);
    if (!src) {
        sg_gpu_error("download: map");
        return;
    }
    dst = sg_guest_bytes(s->guest);
    for (y = 0; y < s->h; y++)
        memcpy(dst + (size_t) y * s->pitch, src + (size_t) y * row, row);
    SDL_UnmapGPUTransferBuffer(s_sg.dev, s_sg.download);
    s->owner = SG_OWNER_GUEST;
}

/* The guest copy of a surface, rows packed tight, for uploading when the
 * GPU copy is stale (a software draw or a title write happened since). */
static void sg_surface_pack(const SgSurface *s, uint8_t *dst) {
    const uint8_t *src = sg_guest_bytes(s->guest);
    uint32_t row = s->w * 4, y;
    for (y = 0; y < s->h; y++)
        memcpy(dst + (size_t) y * row, src + (size_t) y * s->pitch, row);
}

static void sg_download_all(void) {
    int i;
    for (i = 0; i < SG_MAX_SURFACES; i++)
        if (s_sg.surf[i].color)
            sg_download(&s_sg.surf[i]);
}

/* The depth-stencil target of a render pass. Unless this pass clears them,
 * depth and stencil are kept -- or, on a surface's first use, started at 0
 * (as the software buffers are). */
static SDL_GPUDepthStencilTargetInfo sg_depth_target(SgSurface *s, int clear_depth, int clear_stencil) {
    SDL_GPUDepthStencilTargetInfo t = {
        .texture = s->depth,
        .clear_depth = 0.0f,
        .clear_stencil = 0,
        .load_op = s->depth_ready ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
        .stencil_load_op = s->depth_ready ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
        .stencil_store_op = SDL_GPU_STOREOP_STORE,
    };
    if (clear_depth) {
        t.load_op = SDL_GPU_LOADOP_CLEAR;
        t.clear_depth = (float) (s_vp.zclear >> 8) / SG_DEPTH_MAX; /* as zbuf_clear_bits */
    }
    if (clear_stencil) {
        t.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
        t.clear_stencil = (Uint8) (s_vp.zclear & 0xFF);
    }
    s->depth_ready = 1;
    return t;
}

/* NV097_CLEAR_SURFACE on the GPU: colour (all four channels or none), depth
 * and stencil, over the whole surface. A channel subset or a partial
 * rectangle needs a quad: software for now. */
static int sg_clear_surface(uint32_t param) {
    uint32_t x0 = g_clear_rx & 0xFFF, x1 = ((g_clear_rx >> 16) & 0xFFF) + 1;
    uint32_t y0 = g_clear_ry & 0xFFF, y1 = ((g_clear_ry >> 16) & 0xFFF) + 1;
    uint32_t channels = param & 0xF0u;
    int clear_depth = (param & 1u) != 0, clear_stencil = (param & 2u) != 0;
    SDL_GPUColorTargetInfo color = {0};
    SDL_GPUDepthStencilTargetInfo depth;
    SDL_GPUCommandBuffer *cmd;
    SgSurface *s;
    float c[4];

    if (channels != 0xF0u && channels != 0)
        return 0; /* a channel subset */
    if (!channels && !clear_depth && !clear_stencil)
        return 0; /* nothing to clear: let software see it */
    s = sg_surface_current();
    if (!s)
        return 0;
    if (x0 != 0 || y0 != 0 || x1 < s->w || y1 < s->h)
        return 0; /* partial rectangle */

    color.texture = s->color;
    color.store_op = SDL_GPU_STOREOP_STORE;
    if (channels) {
        comb_unpack(s_gpu.clear_color, c); /* clear colour as R,G,B,A floats */
        color.clear_color = (SDL_FColor){c[0], c[1], c[2], c[3]};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
    } else {
        color.load_op = SDL_GPU_LOADOP_LOAD; /* depth/stencil only: colour untouched */
    }
    depth = sg_depth_target(s, clear_depth, clear_stencil);

    sg_queue_flush(); /* draws queued before the clear go first */
    cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
    if (!cmd) {
        sg_gpu_error("clear: command buffer");
        return 0; /* software clears instead */
    }
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &color, 1, &depth));
    SDL_SubmitGPUCommandBuffer(cmd);
    if (channels)
        s->owner = SG_OWNER_GPU; /* the GPU copy is now the newer one */
    /* Clear the software depth/stencil buffers as well. A 3D batch that
     * falls back to software tests against s_zbuf; left uncleared it held
     * depth from long ago and most fallback pixels failed the test and
     * vanished. (Fallbacks still cannot see depth the GPU drew.) */
    if (clear_depth || clear_stencil)
        zbuf_clear_bits(param & 3u);
    return 1;
}
