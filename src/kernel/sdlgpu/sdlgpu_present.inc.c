
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

    for (i = 0; i < SG_MAX_SURFACES; i++)
        if (s_sg.surf[i].color && (s_sg.surf[i].guest & 0x03FFFFFFu) == va)
            s = &s_sg.surf[i];
    if (!s)
        return;
    sg_queue_flush();                             /* its last draws land first */

    /* The CPU drew it last (software fallback, a movie): bring guest RAM up. */
    upload = s->owner != SG_OWNER_GPU;
    if (upload) {
        uint8_t *stage;
        if (!sg_upload_reserve(s->w * s->h * 4u) ||
            !(stage = (uint8_t *)SDL_MapGPUTransferBuffer(s_sg.dev, s_sg.upload, true)))
            return;
        sg_surface_pack(s, stage);
        SDL_UnmapGPUTransferBuffer(s_sg.dev, s_sg.upload);
    }

    if (!s_sgp.tex || s_sgp.w != s->w || s_sgp.h != s->h) {
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = s->w, .height = s->h, .layer_count_or_depth = 1, .num_levels = 1,
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
        s_sgp.w = s->w;
        s_sgp.h = s->h;
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
        SDL_GPUTextureRegion dst = { .texture = s->color, .w = s->w, .h = s->h, .d = 1 };
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        s->owner = SG_OWNER_GPU;                  /* both copies agree now */
    }
    {
        SDL_GPUTextureLocation from = { .texture = s->color }, to = { .texture = s_sgp.tex };
        SDL_CopyGPUTextureToTexture(copy, &from, &to, s->w, s->h, 1, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
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