/* ── SDL3 GPU renderer: textures ─────────────────────────────────────────
 *
 * Each texture stage (0..3) is decoded through the executor's own sampler
 * (sample_texture for stage 0, sample_stage for 1..3), texel by texel, into
 * RGBA8: exact for every format the software path reads (swizzled, linear,
 * palettised, DXT). Decoding a whole texture costs far more than the texels
 * a small batch covers, so decoded textures are cached on the GPU, keyed by
 * a hash of the raw bytes in guest RAM (the executor's per-batch snapshot)
 * plus format, size, pitch and palette. A batch reusing a texture costs one
 * hash. The cache is shared by all stages: the same texture decodes the same
 * whichever stage samples it.
 */

#define SG_MAX_TEXTURES 512

typedef struct {
    uint64_t        key;        /* sg_texture_key(); 0 = empty or not yet uploaded */
    uint32_t        w, h;
    SDL_GPUTexture *tex;
    uint32_t        last_use;
    uint32_t        epoch_used; /* s_sg_queue.epoch when last used: in flight
                                 * until that queue is flushed, never evicted */
} SgCachedTexture;

typedef struct {
    SDL_GPUTexture  *tex;       /* bind this; NULL = stage unused (bind white) */
    uint32_t        *upload;    /* non-NULL: RGBA8 to upload into tex this batch */
    uint32_t         w, h;
    SgCachedTexture *pending;   /* cache slot to mark valid once the upload is issued */
    uint64_t         pending_key;
    SDL_GPUSampler *sampler;
} SgTextureUse;

static SgCachedTexture s_sg_textures[SG_MAX_TEXTURES];
static uint32_t        s_sg_texture_tick;



/* Hashing a texture's raw bytes is the per-batch cost of the cache, so each
 * texture is hashed at most once per frame. Only textures read in place
 * from guest RAM are remembered (a malloc'd snapshot copy can reuse an
 * address). Limitation: a texture the title rewrites in place after using
 * it in the same frame reaches the GPU one frame late. */
#define SG_HASH_MEMO 1024

typedef struct {
    const uint8_t *pixels;
    size_t         bytes;
    uint32_t       shape[4];
    uint32_t       frame;
    uint64_t       hash;
} SgHashMemo;

static SgHashMemo s_sg_hash_memo[SG_HASH_MEMO];
static uint32_t   s_sg_frame = 1;            /* bumped at every flip */

/* One decode buffer per stage: one batch can upload several stages. */
static uint32_t       *s_sg_decode[4];
static size_t          s_sg_decode_texels[4];

/* The executor's description of a stage: stage 0 is s_gpu.tex, 1..3 s_stage[]. */
static const Texture *sg_stage_texture(int st)
{
    return st == 0 ? &s_gpu.tex : &s_stage[st];
}

/* ── Samplers, from the title's own per-stage settings (as the D3D11 path):
 * SET_TEXTURE_FILTER's magnification filter picks bilinear or point, and
 * SET_TEXTURE_ADDRESS picks wrap / mirror / clamp per axis. Point stages
 * stay on mip level 0 (crisp HUD art); bilinear ones filter between the
 * generated mip levels, so distant detail (the grass) does not shimmer. */

#define SG_SAMPLER_KEYS 128                 /* 1 bit filter, 3 bits u, 3 bits v */
static SDL_GPUSampler *s_sg_samplers[SG_SAMPLER_KEYS];

static SDL_GPUSamplerAddressMode sg_address_mode(uint32_t m)
{
    switch (m) {
    case 1:  return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;           /* wrap */
    case 2:  return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;  /* mirror */
    default: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;    /* clamp; border has no SDL mode */
    }
}

static SDL_GPUSampler *sg_sampler_for_stage(int st)
{
    const Texture *tx = sg_stage_texture(st);
    uint32_t filter = s_tex_reg[(0x1B14u + 0x40u * (uint32_t)st - NV_TEX_FIRST) / 4];
    int linear = ((filter >> 24) & 0xFu) != 1;                  /* MAG_FILTER 1 = point */
    uint32_t key = (uint32_t)linear | ((tx->addr_u & 7u) << 1) | ((tx->addr_v & 7u) << 4);

    if (!s_sg_samplers[key]) {
        SDL_GPUSamplerCreateInfo info = {
            .min_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
            .mag_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
            .mipmap_mode = linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
            .address_mode_u = sg_address_mode(tx->addr_u),
            .address_mode_v = sg_address_mode(tx->addr_v),
            .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .min_lod = 0.0f,
            .max_lod = linear ? 1000.0f : 0.0f,                 /* point: level 0 only */
        };
        s_sg_samplers[key] = SDL_CreateGPUSampler(s_sg.dev, &info);
        if (!s_sg_samplers[key])
            return s_sg.point;                                  /* still drawable */
    }
    return s_sg_samplers[key];
}

static int sg_stage_sample(int st, uint32_t x, uint32_t y, uint32_t *argb)
{
    return st == 0 ? sample_texture(x, y, argb) : sample_stage(st, x, y, argb);
}

/* Word-at-a-time multiply/xorshift hash: tens of microseconds for a 256 KB
 * font page, against milliseconds to decode it. */
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
static uint64_t sg_texture_key(int st)
{
    const TextureSnapshot *snap = &s_texture_snapshot[st];
    const Texture *tx = sg_stage_texture(st);
    uint32_t shape[4] = { tx->width, tx->height, tx->color, tx->pitch };
    uint64_t h;

    if (!snap->owned) {
        SgHashMemo *m = &s_sg_hash_memo[(((uintptr_t)snap->pixels >> 6) ^ tx->width ^ (tx->height << 7))
                                        % SG_HASH_MEMO];
        if (m->frame != s_sg_frame || m->pixels != snap->pixels || m->bytes != snap->bytes ||
            memcmp(m->shape, shape, sizeof shape) != 0) {
            m->pixels = snap->pixels;
            m->bytes = snap->bytes;
            memcpy(m->shape, shape, sizeof shape);
            m->frame = s_sg_frame;
            m->hash = sg_hash(snap->pixels, snap->bytes,
                              sg_hash((const uint8_t *)shape, sizeof shape, 0x243F6A8885A308D3ull));
            }
        h = m->hash;
    } else {
        h = sg_hash(snap->pixels, snap->bytes,
                    sg_hash((const uint8_t *)shape, sizeof shape, 0x243F6A8885A308D3ull));
    }
    if (tx->color == 0x0B)                    /* 1 KB palette: cheap, hashed every time */
        h = sg_hash((const uint8_t *)snap->palette, sizeof snap->palette, h);
    return h ? h : 1;
}

/* Decode a stage into s_sg_decode[st]. 0 if the software path cannot read it. */
static int sg_texture_decode(int st, uint32_t w, uint32_t h)
{
    size_t n = (size_t)w * h;
    uint32_t x, y, argb;

    if (n > s_sg_decode_texels[st]) {
        uint32_t *grown = (uint32_t *)realloc(s_sg_decode[st], n * 4);
        if (!grown)
            return 0;
        s_sg_decode[st] = grown;
        s_sg_decode_texels[st] = n;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            if (!sg_stage_sample(st, x, y, &argb))
                return 0;
            /* ARGB -> R,G,B,A bytes in memory */
            s_sg_decode[st][(size_t)y * w + x] = ((argb >> 16) & 0xFFu) | (argb & 0xFF00u)
                                               | ((argb & 0xFFu) << 16) | (argb & 0xFF000000u);
        }
    return 1;
}

/* The GPU texture for stage st. Returns 0 to leave the batch to software. */
static int sg_texture_for_stage(int st, SgTextureUse *use)
{
    const Texture *tx = sg_stage_texture(st);
    uint32_t w = tx->width, h = tx->height;
    SgCachedTexture *slot = &s_sg_textures[0];
    uint64_t key;
    int i;

    if (!w || !h || w > 4096 || h > 4096)
        return 0;
    if (!s_texture_snapshot[st].ready || !s_texture_snapshot[st].pixels)
        return 0;
    /* Render-to-texture: the title samples a surface it drew (screen
   * effects, replays). If the GPU drew it, guest RAM -- which the snapshot
   * read -- is stale: finish the queue and download it first. Addresses
   * are compared as physical (contiguous-window and physical aliases of
   * the same 64 MB). */
    {
        uint32_t lo = tx->offset & 0x03FFFFFFu;
        uint32_t hi = lo + (uint32_t)s_texture_snapshot[st].bytes;
        for (i = 0; i < SG_MAX_SURFACES; i++) {
            SgSurface *s = &s_sg.surf[i];
            uint32_t slo = s->guest & 0x03FFFFFFu, shi = slo + s->pitch * s->h;
            if (s->color && s->owner == SG_OWNER_GPU && lo < shi && slo < hi) {
                sg_queue_flush();
                sg_download(s);
                xbox_texture_snapshot_refresh(st);
            }
        }
    }

    key = sg_texture_key(st);
    s_sg_texture_tick++;
    for (i = 0; i < SG_MAX_TEXTURES; i++) {
        SgCachedTexture *c = &s_sg_textures[i];
        if (c->key == key && c->w == w && c->h == h) {
            c->last_use = s_sg_texture_tick;
            c->epoch_used = s_sg_queue.epoch;
            *use = (SgTextureUse){ c->tex, NULL, w, h, NULL, 0, sg_sampler_for_stage(st) };
            return 1;
        }
    }

    /* Evict the least recently used slot that no queued draw still needs;
     * if every slot is in flight, flush the queue first. */
    slot = NULL;
    for (i = 0; i < SG_MAX_TEXTURES; i++) {
        SgCachedTexture *c = &s_sg_textures[i];
        if (c->epoch_used == s_sg_queue.epoch)
            continue;
        if (!slot || c->last_use < slot->last_use)
            slot = c;
    }
    if (!slot) {
        sg_queue_flush();
        slot = &s_sg_textures[0];
        for (i = 1; i < SG_MAX_TEXTURES; i++)
            if (s_sg_textures[i].last_use < slot->last_use)
                slot = &s_sg_textures[i];
    }

    if (!sg_texture_decode(st, w, h))
        return 0;
    if (!slot->tex || slot->w != w || slot->h != h) {
        /* A full mip chain, generated on the GPU after each upload */
        uint32_t levels = 1, big = w > h ? w : h;
        while (big > 1) { big >>= 1; levels++; }
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            .width = w, .height = h, .layer_count_or_depth = 1, .num_levels = levels,
            .num_levels = levels
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
    slot->epoch_used = s_sg_queue.epoch;      /* reserved by this batch */
    *use = (SgTextureUse){ slot->tex, s_sg_decode[st], w, h, slot, key, sg_sampler_for_stage(st)
    };

    return 1;
}

static int sg_texture_stage0(SgTextureUse *use)
{
    return sg_texture_for_stage(0, use);
    use ->sampler = sg_sampler_for_stage(0);
}

/* The upload for use->pending is in the command buffer: later batches may hit it. */
static void sg_texture_commit(SgTextureUse *use)
{
    if (use->pending)
        use->pending->key = use->pending_key;
    use->pending = NULL;
}