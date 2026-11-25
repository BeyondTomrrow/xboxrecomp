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

/* RECOMP_SDLGPU_FPS: per-second flips, downloads (each a full GPU sync)
 * and surface uploads, printed by sg_present_flip. */
static SDL_AtomicInt s_sg_stat_flips, s_sg_stat_downloads, s_sg_stat_uploads, s_sg_stat_passes, s_sg_stat_draws, s_sg_stat_rtt;
static SDL_AtomicInt s_sg_stat_drain_us, s_sg_stat_dl_us;   /* download: prior GPU work, then the copy itself */

static uint8_t *sg_guest_bytes(uint32_t va) {
    return (uint8_t *) xbox_GetMemoryOffset() + va;
}

static void sg_surface_release(SgSurface *s) {
    free(s->shadow);
    if (s->rtt)
        SDL_ReleaseGPUTexture(s_sg.dev, s->rtt);
    if (s->low)
        SDL_ReleaseGPUTexture(s_sg.dev, s->low);
    if (s->color)
        SDL_ReleaseGPUTexture(s_sg.dev, s->color);
    if (s->depth)
        SDL_ReleaseGPUTexture(s_sg.dev, s->depth);
    memset(s, 0, sizeof *s);
}

/* Internal-resolution multiplier for a new w x h surface: the video
 * settings' scale (F1 > Video, "Internal resolution"), or
 * RECOMP_SDLGPU_SCALE, kept under 8192 px a side. */
static uint32_t sg_scale_for(uint32_t w, uint32_t h) {
    static int env = -1;
    int k;
    if (env < 0) {
        const char *e = getenv("RECOMP_SDLGPU_SCALE");
        env = e ? atoi(e) : 0;
    }
    k = env > 0 ? env : s_vs.scale;
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    while (k > 1 && ((uint32_t) k * w > 8192u || (uint32_t) k * h > 8192u))
        k--;
    return (uint32_t) k;
}

/* The surface's 1x staging texture, created on first need. */
static SDL_GPUTexture *sg_surface_low(SgSurface *s) {
    if (!s->low) {
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = s->w, .height = s->h, .layer_count_or_depth = 1, .num_levels = 1,
        };
        s->low = SDL_CreateGPUTexture(s_sg.dev, &info);
        if (!s->low)
            sg_gpu_error("surface staging texture");
    }
    return s->low;
}

static void sg_blit(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t sw, uint32_t sh,
                    SDL_GPUTexture *dst, uint32_t dw, uint32_t dh) {
    SDL_GPUBlitInfo b;
    SDL_zero(b);
    b.source.texture = src;
    b.source.w = sw;
    b.source.h = sh;
    b.destination.texture = dst;
    b.destination.w = dw;
    b.destination.h = dh;
    b.load_op = SDL_GPU_LOADOP_DONT_CARE;
    b.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &b);
}

/* Where a guest-RAM upload of s lands: color itself at 1x, else `low`,
 * which sg_surface_upscale() then stretches into color. */
static SDL_GPUTexture *sg_surface_upload_target(SgSurface *s) {
    SDL_AddAtomicInt(&s_sg_stat_uploads, 1);
    s->shadow_valid = 0;   /* the GPU copy is about to become the guest's */
    s->version++;          /* the GPU copy's content changes: rtt is stale */
    return s->k > 1 ? sg_surface_low(s) : s->color;
}

static void sg_surface_upscale(SDL_GPUCommandBuffer *cmd, SgSurface *s) {
    if (s->k > 1 && s->low)
        sg_blit(cmd, s->low, s->w, s->h, s->color, s->w * s->k, s->h * s->k);
}

/* GPU render-to-texture: on by default; RECOMP_SDLGPU_NO_GPURTT=1 falls back
 * to the download-through-guest-RAM path. */
static int sg_gpurt_enabled(void) {
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_SDLGPU_NO_GPURTT") == NULL;
    return on;
}

/* A 1x R8G8B8A8 texture holding s's rendered colour, so a draw that samples
 * this surface can bind it directly instead of downloading to guest RAM and
 * re-uploading (a full GPU sync per frame). The blit also downscales k>1 and
 * converts B8G8R8A8 -> R8G8B8A8 (SDL_BlitGPUTexture converts formats). The
 * surface stays GPU-owned, so guest RAM is untouched; sg_cpu_access() and
 * the software-fallback path still download it when they need the bytes.
 * Returns NULL (caller falls back to the download path) on any failure. */
static SDL_GPUTexture *sg_surface_rtt(SgSurface *s) {
    SDL_GPUCommandBuffer *cmd;

    if (!s->rtt) {
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            .width = s->w, .height = s->h, .layer_count_or_depth = 1, .num_levels = 1,
        };
        s->rtt = SDL_CreateGPUTexture(s_sg.dev, &info);
        if (!s->rtt) {
            sg_gpu_error("surface rtt texture");
            return NULL;
        }
        s->rtt_version = 0xFFFFFFFFu;
    }
    if (s->rtt_version != s->version) {
        cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
        if (!cmd) {
            sg_gpu_error("rtt: command buffer");
            return NULL;
        }
        sg_blit(cmd, s->color, s->w * s->k, s->h * s->k, s->rtt, s->w, s->h);
        SDL_SubmitGPUCommandBuffer(cmd);
        s->rtt_version = s->version;
        SDL_AddAtomicInt(&s_sg_stat_rtt, 1);
    }
    return s->rtt;
}

/* Colour and depth-stencil textures for a new w x h surface. */
static int sg_surface_create(SgSurface *s, uint32_t w, uint32_t h) {
    uint32_t k = sg_scale_for(w, h);
    SDL_GPUTextureCreateInfo color = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = w * k, .height = h * k, .layer_count_or_depth = 1, .num_levels = 1,
    };
    SDL_GPUTextureCreateInfo depth = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = s_sg.depth_format,
        .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
        .width = w * k, .height = h * k, .layer_count_or_depth = 1, .num_levels = 1,
    };
    s->color = SDL_CreateGPUTexture(s_sg.dev, &color);
    s->depth = s->color ? SDL_CreateGPUTexture(s_sg.dev, &depth) : NULL;
    if (!s->color || !s->depth) {
        fprintf(stderr, "  [SDLGPU] cannot create a %ux%u target: %s\n", w, h, SDL_GetError());
        sg_surface_release(s);
        return 0;
    }
    s->k = k;
    s->low = NULL;
    s->shadow = NULL;
    s->shadow_valid = 0;
    s->rtt = NULL;
    s->version = 0;
    s->rtt_version = 0xFFFFFFFFu; /* force a blit on first sample */
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

    static int timing = -1;
    Uint64 t_drain = 0, t_dl = 0;
    if (timing < 0) timing = getenv("RECOMP_SDLGPU_FPS") != NULL;
    if (timing) {   /* drain earlier GPU work first, so the two costs separate */
        SDL_GPUCommandBuffer *c0 = SDL_AcquireGPUCommandBuffer(s_sg.dev);
        SDL_GPUFence *f0;
        t_drain = SDL_GetTicksNS();
        if (c0 && (f0 = SDL_SubmitGPUCommandBufferAndAcquireFence(c0))) {
            SDL_WaitForGPUFences(s_sg.dev, true, &f0, 1);
            SDL_ReleaseGPUFence(s_sg.dev, f0);
        }
        t_dl = SDL_GetTicksNS();
        SDL_AddAtomicInt(&s_sg_stat_drain_us, (int) ((t_dl - t_drain) / 1000));
    }
    cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
    if (!cmd) {
        sg_gpu_error("download: command buffer");
        return; /* keeps GPU ownership */
    }
    SDL_AddAtomicInt(&s_sg_stat_downloads, 1);
    /* Scaled: shrink to 1x on the GPU first; guest RAM is 1x. */
    if (s->k > 1) {
        if (!sg_surface_low(s)) {
            SDL_CancelGPUCommandBuffer(cmd);
            return;
        }
        sg_blit(cmd, s->color, s->w * s->k, s->h * s->k, s->low, s->w, s->h);
    }
    copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureRegion from = {.texture = s->k > 1 ? s->low : s->color, .w = s->w, .h = s->h, .d = 1};
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
    if (timing)
        SDL_AddAtomicInt(&s_sg_stat_dl_us, (int) ((SDL_GetTicksNS() - t_dl) / 1000));

    src = (const uint8_t *) SDL_MapGPUTransferBuffer(s_sg.dev, s_sg.download, false);
    if (!src) {
        sg_gpu_error("download: map");
        return;
    }
    dst = sg_guest_bytes(s->guest);
    for (y = 0; y < s->h; y++)
        memcpy(dst + (size_t) y * s->pitch, src + (size_t) y * row, row);
    if (!s->shadow)
        s->shadow = (uint8_t *) malloc(bytes);
    if (s->shadow) {
        memcpy(s->shadow, src, bytes);
        s->shadow_valid = 1;
    }
    SDL_UnmapGPUTransferBuffer(s_sg.dev, s_sg.download);
    s->owner = SG_OWNER_GUEST;
}

/* Guest-owned, but guest RAM is byte-for-byte what the last download left
 * there: the GPU copy is still current, so take it back without an upload. */
static int sg_surface_reclaim(SgSurface *s) {
    const uint8_t *g;
    uint32_t row = s->w * 4, y;
    if (s->owner != SG_OWNER_GUEST || !s->shadow_valid || !s->shadow)
        return 0;
    g = sg_guest_bytes(s->guest);
    for (y = 0; y < s->h; y++)
        if (memcmp(g + (size_t) y * s->pitch, s->shadow + (size_t) y * row, row) != 0) {
            s->shadow_valid = 0;   /* the title or a software draw wrote it */
            return 0;
        }
    s->owner = SG_OWNER_GPU;
    return 1;
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
    if (channels) {
        s->owner = SG_OWNER_GPU; /* the GPU copy is now the newer one */
        s->version++;
    }
    /* Clear the software depth/stencil buffers as well. A 3D batch that
     * falls back to software tests against s_zbuf; left uncleared it held
     * depth from long ago and most fallback pixels failed the test and
     * vanished. (Fallbacks still cannot see depth the GPU drew.) */
    if (clear_depth || clear_stencil)
        zbuf_clear_bits(param & 3u);
    return 1;
}
