/* ── SDL3 GPU renderer: textures ─────────────────────────────────────────
 *
 * Milestone 1 decodes texture stage 0 through the executor's own
 * sample_texture(), texel by texel, into RGBA8. That is exact for every
 * format the software path reads (swizzled, linear, palettised, DXT), at the
 * cost of a CPU decode per batch. Consecutive batches with the same texture
 * (runs of text, HUD pieces) skip the upload. A real cache keyed by guest
 * writes is milestone 3.
 */

typedef struct {
    SDL_GPUTexture *tex;
    uint32_t        w, h;
    uint32_t       *rgba;      /* last decode, kept to detect unchanged textures */
    int             uploaded;  /* rgba is what the GPU texture holds */
} SgStage0;

static SgStage0 s_sg_stage0;

/* Decode stage 0 into s_sg_stage0.rgba. Returns 0 when the software path
 * could not read it either (then the batch stays in software), -1 when the
 * decode equals what the GPU already holds, 1 when it needs uploading. */
static int sg_texture_decode(void)
{
    SgStage0 *t = &s_sg_stage0;
    uint32_t w = s_gpu.tex.width, h = s_gpu.tex.height, x, y, argb;
    size_t n = (size_t)w * h;
    uint32_t *fresh;
    int same;

    if (!w || !h || w > 4096 || h > 4096)
        return 0;
    fresh = (uint32_t *)malloc(n * 4);
    if (!fresh)
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            if (!sample_texture(x, y, &argb)) {
                free(fresh);
                return 0;
            }
            /* ARGB -> R,G,B,A bytes in memory */
            fresh[(size_t)y * w + x] = ((argb >> 16) & 0xFFu) | (argb & 0xFF00u)
                                     | ((argb & 0xFFu) << 16) | (argb & 0xFF000000u);
        }

    same = t->uploaded && t->w == w && t->h == h && memcmp(t->rgba, fresh, n * 4) == 0;
    free(t->rgba);
    t->rgba = fresh;
    if (same)
        return -1;

    if (!t->tex || t->w != w || t->h != h) {
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1,
        };
        if (t->tex)
            SDL_ReleaseGPUTexture(s_sg.dev, t->tex);
        t->tex = SDL_CreateGPUTexture(s_sg.dev, &info);
        if (!t->tex) {
            t->uploaded = 0;
            return 0;
        }
        t->w = w;
        t->h = h;
    }
    t->uploaded = 0;
    return 1;
}
