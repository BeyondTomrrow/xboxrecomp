/* ── SDL3 GPU renderer: vertex-program shader cache ─────────────────────
 *
 * One entry per distinct program (hash of its words + start slot). On
 * first sight the program is translated on the draw thread (string
 * building, microseconds) and handed to a worker thread that compiles it
 * to SPIR-V; until then its batches use the CPU path, so the game never
 * waits for a compile (D3D11 learned that the hard way: a match brings a
 * few hundred programs). When the SPIR-V is ready, the draw thread turns it
 * into an SDL shader on its next use.
 *
 * Entry states: EMPTY -> COMPILING (worker) -> SPIRV -> READY (draw thread),
 * or FAILED (untranslatable or rejected by the compiler: CPU path for good).
 */

#define SG_VP_CACHE 1024               /* power of two */

enum { SG_VP_EMPTY, SG_VP_COMPILING, SG_VP_SPIRV, SG_VP_READY, SG_VP_FAILED };

typedef struct {
    uint32_t       key, start;         /* written by the draw thread before publishing */
    uint32_t       inmask;             /* inputs v0..v15 the program reads */
    SDL_AtomicInt  state;
    char          *src;                /* GLSL, handed to the worker */
    uint32_t      *spirv;              /* the worker's result */
    size_t         words;
    SDL_GPUShader *vs;                 /* READY */
} SgVpProg;

static SgVpProg s_sg_vp[SG_VP_CACHE];

static struct {
    SDL_Thread    *thread;
    SDL_Mutex     *lock;
    SDL_Condition *wake;
    SgVpProg      *jobs[SG_VP_CACHE];  /* never more jobs than entries */
    unsigned       head, tail;
    int            tried;
} s_sg_vpq;

/* FNV-1a over the program's words, as gpu_vp_hash. */
static uint32_t sg_vp_hash(uint32_t start)
{
    uint32_t h = 2166136261u ^ start, slot, k;
    for (slot = start; slot < VP_PROG_LEN; slot++) {
        for (k = 0; k < 4; k++)
            h = (h ^ s_vp.prog[slot][k]) * 16777619u;
        if (vp_field(s_vp.prog[slot], 3, 0, 1))
            break;
    }
    return h;
}

/* Inputs the program reads, so only those are fetched per vertex (most
 * programs read 3-6 of the 16). */
static uint32_t sg_vp_input_mask(uint32_t start)
{
    uint32_t slot, mask = 0;
    for (slot = start; slot < VP_PROG_LEN; slot++) {
        const uint32_t *t = s_vp.prog[slot];
        if (vp_field(t, 2, 26, 2) == 2 || vp_field(t, 2, 11, 2) == 2 || vp_field(t, 3, 28, 2) == 2)
            mask |= 1u << vp_field(t, 1, 9, 4);
        if (vp_field(t, 3, 0, 1))
            break;
    }
    return mask;
}

static int SDLCALL sg_vp_worker(void *arg)
{
    (void)arg;

    /* glslang keeps per-thread state and builds its built-in symbol tables
     * used from two threads, it failed with "Unable to parse
     * built-ins". This thread is the only one that ever calls it --
     * initialisation, the self-test and every compile. */
    sg_compiler_init();
    {
        static const char probe[] = "#version 450\nvoid main() { gl_Position = vec4(0.0); }\n";
        size_t n = 0;
        uint32_t *spirv = sg_glsl_to_spirv(probe, 1, &n);
        if (spirv)
            fprintf(stderr, "  [SDLGPU] runtime shader compiler ready (glslang, %zu words)\n", n);
        else
            fprintf(stderr, "  [SDLGPU] runtime shader compiler unavailable\n");
        free(spirv);
    }
    for (;;) {
        SgVpProg *p;
        SDL_LockMutex(s_sg_vpq.lock);
        while (s_sg_vpq.head == s_sg_vpq.tail)
            SDL_WaitCondition(s_sg_vpq.wake, s_sg_vpq.lock);
        p = s_sg_vpq.jobs[s_sg_vpq.head++ % SG_VP_CACHE];
        SDL_UnlockMutex(s_sg_vpq.lock);

        p->spirv = sg_glsl_to_spirv(p->src, 1, &p->words);
        free(p->src);
        p->src = NULL;
        SDL_SetAtomicInt(&p->state, p->spirv ? SG_VP_SPIRV : SG_VP_FAILED);
    }
    return 0;
}

static int sg_vp_worker_start(void)
{
#ifdef SG_HAVE_GLSLANG
    if (s_sg_vpq.thread)
        return 1;
    if (s_sg_vpq.tried)
        return 0;
    s_sg_vpq.tried = 1;

    s_sg_vpq.lock = SDL_CreateMutex();
    s_sg_vpq.wake = SDL_CreateCondition();
    if (s_sg_vpq.lock && s_sg_vpq.wake)
        s_sg_vpq.thread = SDL_CreateThread(sg_vp_worker, "sg_vp_compile", NULL);
    if (!s_sg_vpq.thread)
        fprintf(stderr, "  [SDLGPU] no shader compile thread: %s\n", SDL_GetError());
    return s_sg_vpq.thread != NULL;
#else
    return 0;                          /* built without glslang: CPU path */
#endif
}

/* The compiled shader for the current program (s_vp.start), or NULL to draw
 * this batch through the CPU path: compiling, untranslatable, failed, or
 * the cache is full. *slot_out receives the cache slot. */
static SDL_GPUShader *sg_vp_program(uint32_t *slot_out)
{
    uint32_t key = sg_vp_hash(s_vp.start), i;

    for (i = 0; i < SG_VP_CACHE; i++) {
        uint32_t at = (key + i) & (SG_VP_CACHE - 1);
        SgVpProg *p = &s_sg_vp[at];
        int st = SDL_GetAtomicInt(&p->state);

        if (st == SG_VP_EMPTY) {
            /* First sight: translate here, compile on the worker. */
            char *src;
            if (!sg_vp_worker_start())
                return NULL;
            p->key = key;
            p->start = s_vp.start;
            p->inmask = sg_vp_input_mask(s_vp.start);
            src = sg_vp_translate(s_vp.start, NULL);
            if (!src) {
                SDL_SetAtomicInt(&p->state, SG_VP_FAILED);
                return NULL;
            }
            p->src = src;
            SDL_SetAtomicInt(&p->state, SG_VP_COMPILING);
            SDL_LockMutex(s_sg_vpq.lock);
            s_sg_vpq.jobs[s_sg_vpq.tail++ % SG_VP_CACHE] = p;
            SDL_SignalCondition(s_sg_vpq.wake);
            SDL_UnlockMutex(s_sg_vpq.lock);
            return NULL;
        }
        if (p->key != key || p->start != s_vp.start)
            continue;                  /* another program: keep probing */

        if (st == SG_VP_SPIRV) {
            /* Compiled: make the SDL shader here, on the draw thread. */
            SDL_GPUShaderCreateInfo info = {
                .code = (const Uint8 *)p->spirv, .code_size = p->words * sizeof *p->spirv,
                .entrypoint = "main", .format = SDL_GPU_SHADERFORMAT_SPIRV,
                .stage = SDL_GPU_SHADERSTAGE_VERTEX, .num_uniform_buffers = 1,
            };
            p->vs = SDL_CreateGPUShader(s_sg.dev, &info);
            free(p->spirv);
            p->spirv = NULL;
            if (!p->vs)
                sg_gpu_error("vertex program shader");
            st = p->vs ? SG_VP_READY : SG_VP_FAILED;
            SDL_SetAtomicInt(&p->state, st);
        }
        if (st != SG_VP_READY)
            return NULL;
        *slot_out = at;
        return p->vs;
    }
    return NULL;
}