/* ── SDL3 GPU renderer: textures ─────────────────────────────────────────
 *
 * Stage 0 is decoded through the executor's own sample_texture(), texel by
 * texel, into RGBA8: exact for every format the software path reads
 * (swizzled, linear, palettised, DXT). Decoding a whole texture costs far
 * more than the few texels a small batch covers, so decoded textures are
 * cached on the GPU, keyed by a hash of the texture's raw bytes in guest RAM
 * (the executor's per-batch snapshot) plus its format, size and palette. A
 * batch reusing a texture -- every glyph of a line of text -- costs one hash.
 */

#define SG_MAX_TEXTURES 48

typedef struct {
    uint64_t        key;        /* sg_texture_key(); 0 = empty slot */
    uint32_t        w, h;
    SDL_GPUTexture *tex;
    uint32_t        last_use;
} SgCachedTexture;

typedef struct {
    SDL_GPUTexture  *tex;       /* bind this */
    uint32_t        *upload;    /* non-NULL: RGBA8 to upload into tex this batch */
    uint32_t         w, h;
    SgCachedTexture *pending;   /* cache slot to mark valid once the upload is issued */
    uint64_t         pending_key;
} SgTextureUse;

static SgCachedTexture s_sg_textures[SG_MAX_TEXTURES];
static uint32_t        s_sg_texture_tick;
static uint32_t       *s_sg_decode;          /* decode buffer, reused */
static size_t          s_sg_decode_texels;

/* Word-at-a-time multiply/xorshift hash: a few tens of microseconds for a
 * 256 KB font page, against milliseconds to decode it. */
static uint64_t sg_hash(const uint8_t *p, size_t n, uint64_t h)
{
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t w;
    for (; n >= 8; p += 8, n -= 8) {
        memcpy(&w, p, 8);
        h = (h ^ w) * k;
        h ^= h >> 29;
    }
    for (; n; p++, n--)
        h = (h ^ *p) * k;
    return h ^ (h >> 31);
}

/* Everything that decides the decoded texels: raw bytes, palette, format,
 * size, row pitch. Wrap and clamp are shader state, not part of the key. */
static uint64_t sg_texture_key(void)
{
    const TextureSnapshot *snap = &s_texture_snapshot[0];
    uint64_t h = 0x243F6A8885A308D3ull;
    uint32_t shape[4] = { s_gpu.tex.width, s_gpu.tex.height, s_gpu.tex.color, s_gpu.tex.pitch };

    h = sg_hash((const uint8_t *)shape, sizeof shape, h);
    h = sg_hash(snap->pixels, snap->bytes, h);
    if (s_gpu.tex.color == 0x0B)
        h = sg_hash((const uint8_t *)snap->palette, sizeof snap->palette, h);
    return h ? h : 1;
}

/* Decode stage 0 into s_sg_decode. 0 if the software path cannot read it. */
static int sg_texture_decode(uint32_t w, uint32_t h)
{
    uint32_t x, y, argb;
    size_t n = (size_t)w * h;

    if (n > s_sg_decode_texels) {
        uint32_t *grown = (uint32_t *)realloc(s_sg_decode, n * 4);
        if (!grown)
            return 0;
        s_sg_decode = grown;
        s_sg_decode_texels = n;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            if (!sample_texture(x, y, &argb))
                return 0;
            /* ARGB -> R,G,B,A bytes in memory */
            s_sg_decode[(size_t)y * w + x] = ((argb >> 16) & 0xFFu) | (argb & 0xFF00u)
                                           | ((argb & 0xFFu) << 16) | (argb & 0xFF000000u);
        }
    return 1;
}

/* The GPU texture for stage 0. Returns 0 to leave the batch to software. */
static int sg_texture_stage0(SgTextureUse *use)
{
    uint32_t w = s_gpu.tex.width, h = s_gpu.tex.height;
    SgCachedTexture *slot = &s_sg_textures[0];
    uint64_t key;
    int i;

    if (!w || !h || w > 4096 || h > 4096)
        return 0;
    if (!s_texture_snapshot[0].ready || !s_texture_snapshot[0].pixels)
        return 0;

    key = sg_texture_key();
    s_sg_texture_tick++;
    for (i = 0; i < SG_MAX_TEXTURES; i++) {
        SgCachedTexture *c = &s_sg_textures[i];
        if (c->key == key && c->w == w && c->h == h) {
            c->last_use = s_sg_texture_tick;
            use->tex = c->tex;
            use->upload = NULL;
            use->pending = NULL;
            use->w = w;
            use->h = h;
            return 1;
        }
        if (c->last_use < slot->last_use)
            slot = c;                                 /* least recently used */
    }

    if (!sg_texture_decode(w, h))
        return 0;
    if (!slot->tex || slot->w != w || slot->h != h) {
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1,
        };
        if (slot->tex)
            SDL_ReleaseGPUTexture(s_sg.dev, slot->tex);
        slot->tex = SDL_CreateGPUTexture(s_sg.dev, &info);
        slot->key = 0;
        if (!slot->tex)
            return 0;
        slot->w = w;
        slot->h = h;
    }
    /* The slot only becomes a cache hit in sg_texture_commit(), once its
     * upload has been recorded: a batch that fails in between must not leave
     * a key pointing at a texture that never received its texels. */
    slot->key = 0;
    slot->last_use = s_sg_texture_tick;
    use->tex = slot->tex;
    use->upload = s_sg_decode;
    use->w = w;
    use->h = h;
    use->pending = slot;
    use->pending_key = key;
    return 1;
}

/* The upload for use->pending is in the command buffer: later batches may hit it. */
static void sg_texture_commit(SgTextureUse *use)
{
    if (use->pending)
        use->pending->key = use->pending_key;
    use->pending = NULL;
}
