
void xbox_FramebufferSetGpuPresenter(int (*fn)(void *window, int letterbox));
uint32_t xbox_FramebufferWindowVa(void);

static struct {
    SDL_Mutex      *lock;             /* tex, w, h: replaced on the flip thread */
    SDL_GPUTexture *tex;              /* the last flipped frame */
    uint32_t        w, h;
    int             registered;
    SDL_AtomicInt   failed;           /* the window could not be claimed */
    int             claimed;          /* fb_thread only */
} s_sgp;

static int sg_present_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_SDLGPU_PRESENT");
        on = !(v && strcmp(v, "0") == 0);
    }
    return on;
}

static int sg_present_active(void)
{
    return s_sgp.registered && !SDL_GetAtomicInt(&s_sgp.failed);
}

/* fb_thread: show the last flipped frame (see FbPresenter in fb_present.c). */
static int SDLCALL sg_present_window(void *window, int letterbox)
{
    SDL_Window *win = (SDL_Window *)window;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;

    if (letterbox < 0) {                          /* shutdown */
        if (s_sgp.claimed)
            SDL_ReleaseWindowFromGPUDevice(s_sg.dev, win);
        s_sgp.claimed = 0;
        return 1;
    }
    if (!s_sgp.claimed) {
        if (!SDL_ClaimWindowForGPUDevice(s_sg.dev, win)) {
            fprintf(stderr, "  [SDLGPU] cannot present on the window (%s); frames go through guest RAM\n",
                    SDL_GetError());
            SDL_SetAtomicInt(&s_sgp.failed, 1);
            return 0;
        }
        s_sgp.claimed = 1;
        fprintf(stderr, "  [SDLGPU] presenting from the GPU\n");
    }

    cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
    if (!cmd) {
        sg_gpu_error("present: command buffer");
        return 1;
    }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swap, &sw, &sh)) {
        sg_gpu_error("present: swapchain");
        SDL_CancelGPUCommandBuffer(cmd);
        return 1;
    }
    if (swap) {                                   /* NULL while minimised */
        SDL_LockMutex(s_sgp.lock);
        if (s_sgp.tex) {
            SDL_GPUBlitInfo blit;
            float dw = (float)sw, dh = (float)sh;
            SDL_zero(blit);
            if (letterbox) {                      /* fit, keeping the frame's shape */
                float sx = (float)sw / (float)s_sgp.w, sy = (float)sh / (float)s_sgp.h;
                float s = sx < sy ? sx : sy;
                dw = (float)s_sgp.w * s;
                dh = (float)s_sgp.h * s;
            }
            blit.source.texture = s_sgp.tex;
            blit.source.w = s_sgp.w;
            blit.source.h = s_sgp.h;
            blit.destination.texture = swap;
            blit.destination.x = (Uint32)(((float)sw - dw) * 0.5f);
            blit.destination.y = (Uint32)(((float)sh - dh) * 0.5f);
            blit.destination.w = (Uint32)dw;
            blit.destination.h = (Uint32)dh;
            blit.load_op = SDL_GPU_LOADOP_CLEAR;  /* black outside the frame */
            blit.clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f };
            blit.filter = (s_vs.filter == SF_NEAREST) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(cmd, &blit);
        } else {                                  /* nothing flipped yet: black */
            SDL_GPUColorTargetInfo t = {
                .texture = swap, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE,
                .clear_color = { 0.0f, 0.0f, 0.0f, 1.0f },
            };
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &t, 1, NULL));
        }
        SDL_UnlockMutex(s_sgp.lock);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    return 1;
}

/* RECOMP_SDLGPU_DUMP=<file.bmp>: write the GPU's copy of the displayed
 * surface at flip RECOMP_SDLGPU_DUMP_AT (default 600), read straight back
 * from the GPU texture. Diagnostic: shows what the renderer drew, without
 * guest RAM or a screen capture in between. */
static void sg_present_dump(SgSurface *s)
{
    static int flips;
    static const char *path;
    static int at;
    SDL_GPUTransferBuffer *tb;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *copy;
    SDL_GPUFence *done;
    const uint8_t *px;
    FILE *f;
    static const char *base;
    static int seq = -1, stop;
    uint32_t W = s->w * s->k, H = s->h * s->k;   /* the scaled target, as drawn */
    uint32_t y, row = W * 4u, bytes = row * H;

    if (!flips) {
        const char *a = getenv("RECOMP_SDLGPU_DUMP_AT");
        path = getenv("RECOMP_SDLGPU_DUMP");
        at = a ? atoi(a) : 600;
    }
    if (!path)
        return;
    {
        /* RECOMP_SDLGPU_DUMP_SEQ=<n>: <n> consecutive flips from DUMP_AT, each
         * written as <path>.<flip>.bmp. Frame-by-frame evidence. */
        static char seq_name[1200];
        char req[1024];
        if (seq < 0) {
            const char *v = getenv("RECOMP_SDLGPU_DUMP_SEQ");
            seq = v ? atoi(v) : 0;
            if (seq)
                base = path;
        }
        if (seq) {
            flips++;
            if (!stop)
                stop = at + seq;
            if (flips < at || flips >= stop)
                return;
            snprintf(seq_name, sizeof seq_name, "%s.%d.bmp", base, flips);
            path = seq_name;
            at = flips;                     /* this flip is the one to write */
        } else if (++flips % 15 == 0) {
            /* On request: touch <path>.req and the next flip is written to
             * <path> (the request file is removed). */
            snprintf(req, sizeof req, "%s.req", path);
            if (remove(req) == 0)
                at = flips;
        }
    }
    if (flips != at)
        return;
    {
        SDL_GPUTransferBufferCreateInfo info = {
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = bytes,
        };
        tb = SDL_CreateGPUTransferBuffer(s_sg.dev, &info);
    }
    if (!tb || !(cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev)))
        return;
    copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureRegion from = { .texture = s->color, .w = W, .h = H, .d = 1 };
        SDL_GPUTextureTransferInfo to = { .transfer_buffer = tb };
        SDL_DownloadFromGPUTexture(copy, &from, &to);
    }
    SDL_EndGPUCopyPass(copy);
    done = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!done) { SDL_ReleaseGPUTransferBuffer(s_sg.dev, tb); return; }
    SDL_WaitForGPUFences(s_sg.dev, true, &done, 1);
    SDL_ReleaseGPUFence(s_sg.dev, done);
    px = (const uint8_t *)SDL_MapGPUTransferBuffer(s_sg.dev, tb, false);
    if (px && (f = fopen(path, "wb"))) {
        uint8_t hdr[54] = { 'B', 'M' };
        uint32_t v;
        #define SG_LE32(o, x) do { v = (x); memcpy(hdr + (o), &v, 4); } while (0)
        SG_LE32(2, 54 + bytes); SG_LE32(10, 54); SG_LE32(14, 40);
        SG_LE32(18, W); SG_LE32(22, (uint32_t)-(int32_t)H);   /* top-down */
        hdr[26] = 1; hdr[28] = 32;
        SG_LE32(34, bytes);
        #undef SG_LE32
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = 0; y < H; y++)                   /* B8G8R8A8 is BMP's own order */
            fwrite(px + (size_t)y * row, 1, row, f);
        fclose(f);
        fprintf(stderr, "  [SDLGPU] dumped flip %d (%ux%u) to %s\n", at, W, H, path);
    }
    if (px)
        SDL_UnmapGPUTransferBuffer(s_sg.dev, tb);
    SDL_ReleaseGPUTransferBuffer(s_sg.dev, tb);
}

/* Flip (command thread): make the displayed surface the present texture.
 * Nothing changes on screen when the displayed buffer is not a surface the
 * renderer holds (the previous frame stays). */
static void sg_present_flip(void)
{
    uint32_t va = xbox_FramebufferWindowVa() & 0x03FFFFFFu;
    SgSurface *s = NULL;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *copy;
    int i, upload;

    {
        static int fps = -1;
        static Uint64 t0;
        Uint64 now = SDL_GetTicks();
        if (fps < 0) { fps = getenv("RECOMP_SDLGPU_FPS") != NULL; t0 = now; }
        SDL_AddAtomicInt(&s_sg_stat_flips, 1);
        {
            static const char *dir;
            static int looked;
            static Uint64 next;
            if (!looked) { dir = getenv("RECOMP_SDLGPU_TOGGLE_DIR"); looked = 1; }
            if (dir && now >= next) {
                char f[1024];
                int v;
                next = now + 500;
                snprintf(f, sizeof f, "%s/novp", dir);
                v = access(f, F_OK) == 0;
                if (v != s_sg_toggle_novp) fprintf(stderr, "  [SDLGPU] toggle novp %d\n", v);
                s_sg_toggle_novp = v;
                snprintf(f, sizeof f, "%s/nofloor", dir);
                v = access(f, F_OK) == 0;
                if (v != s_sg_toggle_nofloor) fprintf(stderr, "  [SDLGPU] toggle nofloor %d\n", v);
                s_sg_toggle_nofloor = v;
                snprintf(f, sizeof f, "%s/nomip", dir);
                v = access(f, F_OK) == 0;
                if (v != s_sg_toggle_nomip) fprintf(stderr, "  [SDLGPU] toggle nomip %d\n", v);
                s_sg_toggle_nomip = v;
                snprintf(f, sizeof f, "%s/no3d", dir);
                v = access(f, F_OK) == 0;
                if (v != s_sg_toggle_no3d) fprintf(stderr, "  [SDLGPU] toggle no3d %d\n", v);
                s_sg_toggle_no3d = v;
                snprintf(f, sizeof f, "%s/nospec", dir);
                v = access(f, F_OK) == 0;
                if (v != s_sg_toggle_nospec) fprintf(stderr, "  [SDLGPU] toggle nospec %d\n", v);
                s_sg_toggle_nospec = v;
            }
        }
        if (fps && now - t0 >= 1000) {
            fprintf(stderr, "  [SDLGPU] drain %d ms, download %d ms (per second)\n",
                    SDL_SetAtomicInt(&s_sg_stat_drain_us, 0) / 1000, SDL_SetAtomicInt(&s_sg_stat_dl_us, 0) / 1000);
            fprintf(stderr, "  [SDLGPU] %.1f flips/s, %d downloads, %d surface uploads, %d passes, %d draws, %d rtt\n",
                    SDL_SetAtomicInt(&s_sg_stat_flips, 0) * 1000.0 / (double)(now - t0),
                    SDL_SetAtomicInt(&s_sg_stat_downloads, 0),
                    SDL_SetAtomicInt(&s_sg_stat_uploads, 0),
                    SDL_SetAtomicInt(&s_sg_stat_passes, 0),
                    SDL_SetAtomicInt(&s_sg_stat_draws, 0),
                    SDL_SetAtomicInt(&s_sg_stat_rtt, 0));
            t0 = now;
        }
    }
    for (i = 0; i < SG_MAX_SURFACES; i++)
        if (s_sg.surf[i].color && (s_sg.surf[i].guest & 0x03FFFFFFu) == va)
            s = &s_sg.surf[i];
    if (!s)
        return;
    sg_queue_flush();                             /* its last draws land first */

    /* The CPU drew it last (software fallback, a movie): bring guest RAM up. */
    upload = s->owner != SG_OWNER_GPU && !sg_surface_reclaim(s);
    if (upload) {
        uint8_t *stage;
        if (!sg_upload_reserve(s->w * s->h * 4u) ||
            !(stage = (uint8_t *)SDL_MapGPUTransferBuffer(s_sg.dev, s_sg.upload, true)))
            return;
        sg_surface_pack(s, stage);
        SDL_UnmapGPUTransferBuffer(s_sg.dev, s_sg.upload);
    }

    if (!s_sgp.tex || s_sgp.w != s->w * s->k || s_sgp.h != s->h * s->k) {
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = s->w * s->k, .height = s->h * s->k, .layer_count_or_depth = 1, .num_levels = 1,
        };
        SDL_GPUTexture *t = SDL_CreateGPUTexture(s_sg.dev, &info);
        if (!t) {
            sg_gpu_error("present texture");
            return;
        }
        SDL_LockMutex(s_sgp.lock);
        if (s_sgp.tex)
            SDL_ReleaseGPUTexture(s_sg.dev, s_sgp.tex);   /* freed once the GPU is done */
        s_sgp.tex = t;
        s_sgp.w = s->w * s->k;
        s_sgp.h = s->h * s->k;
        SDL_UnlockMutex(s_sgp.lock);
    }

    cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
    if (!cmd) {
        sg_gpu_error("present flip: command buffer");
        return;
    }
    copy = SDL_BeginGPUCopyPass(cmd);
    if (upload) {
        SDL_GPUTextureTransferInfo src = { .transfer_buffer = s_sg.upload };
        SDL_GPUTextureRegion dst = { .texture = sg_surface_upload_target(s), .w = s->w, .h = s->h, .d = 1 };
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        s->owner = SG_OWNER_GPU;                  /* both copies agree now */
        if (s->k > 1) {                           /* stretch the 1x upload first */
            SDL_EndGPUCopyPass(copy);
            sg_surface_upscale(cmd, s);
            copy = SDL_BeginGPUCopyPass(cmd);
        }
    }
    {
        SDL_GPUTextureLocation from = { .texture = s->color }, to = { .texture = s_sgp.tex };
        SDL_CopyGPUTextureToTexture(copy, &from, &to, s->w * s->k, s->h * s->k, 1, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    sg_present_dump(s);
}

/* Once the renderer can draw: take over presenting (fb_thread claims the
 * window on its next pass). */
static void sg_present_register(void)
{
    if (s_sgp.registered || !s_sg.dev || !s_sg.draw_ready || !sg_present_wanted())
        return;
    s_sgp.lock = SDL_CreateMutex();
    if (!s_sgp.lock)
        return;
    s_sgp.registered = 1;
    xbox_FramebufferSetGpuPresenter(sg_present_window);
}