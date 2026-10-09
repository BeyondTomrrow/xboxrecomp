/* Specialised pixel shaders for the Direct3D 11 renderer.
 *
 * The generic pixel shader (ps_main in nv2a_gpu_d3d11.hlsl) reads the texture
 * stage modes and the register-combiner program from the constant buffer and
 * indexes its 16 combiner registers at run time. GPUs keep such an array in
 * local memory, and the shader also writes SV_Depth (no early Z), so every
 * covered pixel paid the whole generic program: ~2 ns a pixel on an RTX 2060
 * SUPER, and at 8x internal resolution even the menus fell to 22 fps with the
 * GPU at 100% (2026-10-03).
 *
 * Here each distinct state gets its own variant: the same HLSL with that
 * state inserted as compile-time constants at the //@PS_SPEC@ marker, so the
 * compiler folds every mode test and the combiner registers become plain
 * temporaries. Variants compile on worker threads; until one is ready the
 * draw uses the generic shader, which gives identical results. Bytecode is
 * kept in cache\ps\ (keyed by the variant's source) so later runs skip the
 * compile. RECOMP_GPU_PS_SPEC=0 turns this off. */

typedef struct {
    uint32_t stage[8][4];
    uint32_t misc[4], alpha[4], texon[4], mode[4], dotmap[4], input[4], cube[4], rect[4], show[4];
} GpuPsKey;

typedef struct {
    GpuPsKey k;
    uint32_t hash;
    volatile LONG state;                 /* 0 empty, 1 queued, 2 ready, 3 failed */
    ID3D11PixelShader *ps;
} GpuPsEnt;

#define GPU_PS_CACHE 4096                /* power of two */
#define GPU_PS_PROBE 16
#define GPU_PS_JOBS 1024
static GpuPsEnt s_ps_cache[GPU_PS_CACHE];
static GpuPsEnt *s_psjob[GPU_PS_JOBS];
static volatile LONG s_psjob_head, s_psjob_tail;
static HANDLE s_psjob_event;
static volatile LONG s_ps_ready, s_ps_failed, s_ps_loaded;

static int gpu_ps_spec_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = env_cached("RECOMP_GPU_PS_SPEC");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* The state that selects a variant, with the parts the shader cannot read
 * zeroed so equivalent states share one. */
static void gpu_ps_make_key(const GpuConsts *c, GpuPsKey *k)
{
    uint32_t ns, i;
    memset(k, 0, sizeof *k);
    memcpy(k->alpha, c->alpha, sizeof k->alpha);
    memcpy(k->texon, c->texon, sizeof k->texon);
    memcpy(k->mode, c->mode, sizeof k->mode);
    memcpy(k->dotmap, c->dotmap, sizeof k->dotmap);
    memcpy(k->input, c->input, sizeof k->input);
    memcpy(k->cube, c->cube, sizeof k->cube);
    memcpy(k->rect, c->rect, sizeof k->rect);
    memcpy(k->show, c->show, sizeof k->show);
    if (!k->alpha[0]) k->alpha[1] = k->alpha[2] = 0;     /* no alpha test */
    else if (k->alpha[1] == 0x200u || k->alpha[1] == 0x207u) k->alpha[2] = 0;   /* never / always */
    if (c->alpha[3]) {                                    /* combiners active */
        ns = c->misc[0] & 255u; if (ns > 8) ns = 8;
        for (i = 0; i < ns; i++)
            memcpy(k->stage[i], c->stage[i], sizeof k->stage[i]);
        k->misc[0] = c->misc[0]; k->misc[2] = c->misc[2]; k->misc[3] = c->misc[3];
    }
    for (i = 0; i < 4; i++)
        if (!k->mode[i]) { k->texon[i] = 0; k->cube[i] = 0; k->rect[i] = 0; k->dotmap[i] = 0; k->input[i] = 0; }
}

static uint32_t gpu_ps_key_hash(const GpuPsKey *k)
{
    const uint32_t *w = (const uint32_t *)k;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < sizeof *k / 4; i++)
        h = (h ^ w[i]) * 16777619u;
    return h ^ (h >> 16);
}

static char *gpu_ps_source(const GpuPsKey *k)
{
    static const char marker[] = "//@PS_SPEC@";
    const char *at = strstr(k_gpu_hlsl, marker);
    size_t head, cap;
    char *s;
    int n, i;
    if (!at)
        return NULL;
    head = (size_t)(at - k_gpu_hlsl);
    cap = strlen(k_gpu_hlsl) + 4096;
    s = (char *)malloc(cap);
    if (!s)
        return NULL;
    memcpy(s, k_gpu_hlsl, head);
    n = (int)head;
#define PS_U4(name, v) n += snprintf(s + n, cap - n, "static const uint4 k_" name " = uint4(%uu, %uu, %uu, %uu);\n", \
                                     (v)[0], (v)[1], (v)[2], (v)[3])
    n += snprintf(s + n, cap - n, "#define PS_SPEC 1\nstatic const uint4 k_stg[8] = {\n");
    for (i = 0; i < 8; i++)
        n += snprintf(s + n, cap - n, "    uint4(%uu, %uu, %uu, %uu)%s\n", k->stage[i][0], k->stage[i][1],
                      k->stage[i][2], k->stage[i][3], i < 7 ? "," : "");
    n += snprintf(s + n, cap - n, "};\n");
    PS_U4("misc", k->misc); PS_U4("alpha", k->alpha); PS_U4("texon", k->texon);
    PS_U4("mode", k->mode); PS_U4("dotmap", k->dotmap); PS_U4("input", k->input);
    PS_U4("cube", k->cube); PS_U4("rect", k->rect); PS_U4("show", k->show);
#undef PS_U4
    n += snprintf(s + n, cap - n,
                  "#define stg k_stg\n#define misc k_misc\n#define alpha k_alpha\n#define texon k_texon\n"
                  "#define mode k_mode\n#define dotmap k_dotmap\n#define input k_input\n#define cube k_cube\n"
                  "#define rect k_rect\n#define show k_show\n");
    memcpy(s + n, at + sizeof marker - 1, strlen(at + sizeof marker - 1) + 1);
    return s;
}

static void gpu_ps_compile(GpuPsEnt *e)
{
    char path[MAX_PATH];
    char *src = gpu_ps_source(&e->k);
    uint64_t sh;
    void *code = NULL;
    SIZE_T size = 0;
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;
    FILE *f;

    if (!src) {
        InterlockedExchange(&e->state, 3);
        return;
    }
    sh = gpu_vp_srchash(src);
    snprintf(path, sizeof path, "cache\\ps\\%016llX.cso", (unsigned long long)sh);
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size = (SIZE_T)ftell(f);
        fseek(f, 0, SEEK_SET);
        code = malloc(size ? size : 1);
        if (!code || fread(code, 1, size, f) != size) { free(code); code = NULL; size = 0; }
        fclose(f);
        if (code) InterlockedIncrement(&s_ps_loaded);
    }
    if (!code) {
        HRESULT hr = D3DCompile(src, strlen(src), "nv2a_ps", NULL, NULL, "ps_main", "ps_4_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (FAILED(hr)) {
            if (InterlockedIncrement(&s_ps_failed) <= 3)
                fprintf(stderr, "  [GPUPS] variant failed to compile: %s\n",
                        err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
            if (err) ID3D10Blob_Release(err);
            free(src);
            InterlockedExchange(&e->state, 3);
            return;
        }
        if (err) ID3D10Blob_Release(err);
        code = ID3D10Blob_GetBufferPointer(blob);
        size = ID3D10Blob_GetBufferSize(blob);
        CreateDirectoryA("cache", NULL);
        CreateDirectoryA("cache\\ps", NULL);
        f = fopen(path, "wb");
        if (f) { fwrite(code, 1, size, f); fclose(f); }
    }
    free(src);
    if (FAILED(ID3D11Device_CreatePixelShader(s_gpu11.dev, code, size, NULL, &ps))) {
        InterlockedExchange(&e->state, 3);
    } else {
        LONG n;
        e->ps = ps;
        MemoryBarrier();
        InterlockedExchange(&e->state, 2);
        n = InterlockedIncrement(&s_ps_ready);
        if (n <= 3 || (n % 50) == 0)
            fprintf(stderr, "  [GPUPS] %ld pixel shader variants ready (%ld from cache\\ps)\n", n, s_ps_loaded);
    }
    if (blob) ID3D10Blob_Release(blob); else free(code);
}

static DWORD WINAPI gpu_ps_worker(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        LONG t = s_psjob_tail;
        if (t == s_psjob_head) {
            WaitForSingleObject(s_psjob_event, 100);
            continue;
        }
        {
            GpuPsEnt *e = s_psjob[t % GPU_PS_JOBS];
            if (InterlockedCompareExchange(&s_psjob_tail, t + 1, t) != t)
                continue;
            gpu_ps_compile(e);
        }
    }
}

/* The pixel shader for this draw's constants: its variant when compiled,
 * else the generic shader. Called on the draw path (one thread). */
static ID3D11PixelShader *gpu_ps_shader(const GpuConsts *c)
{
    static GpuPsKey last_key;
    static GpuPsEnt *last_ent;
    static int workers;
    GpuPsKey k;
    GpuPsEnt *e = NULL, *free_e = NULL;
    uint32_t h, i;

    if (!gpu_ps_spec_on())
        return s_gpu11.ps;
    gpu_ps_make_key(c, &k);
    if (last_ent && !memcmp(&k, &last_key, sizeof k)) {
        e = last_ent;
    } else {
        h = gpu_ps_key_hash(&k);
        for (i = 0; i < GPU_PS_PROBE; i++) {
            GpuPsEnt *p = &s_ps_cache[(h + i) & (GPU_PS_CACHE - 1)];
            if (p->state == 0) { if (!free_e) free_e = p; break; }
            if (p->hash == h && !memcmp(&p->k, &k, sizeof k)) { e = p; break; }
        }
        if (!e) {
            LONG head = s_psjob_head;
            if (!free_e || head - s_psjob_tail >= GPU_PS_JOBS)
                return s_gpu11.ps;                 /* table full or queue full: generic */
            if (!workers) {
                int nw;
                SYSTEM_INFO si;
                GetSystemInfo(&si);
                nw = si.dwNumberOfProcessors >= 8 ? 3 : 1;
                s_psjob_event = CreateEventA(NULL, FALSE, FALSE, NULL);
                for (workers = 0; workers < nw; workers++)
                    CloseHandle(CreateThread(NULL, 0, gpu_ps_worker, NULL, 0, NULL));
            }
            e = free_e;
            e->k = k;
            e->hash = h;
            e->ps = NULL;
            e->state = 1;
            s_psjob[head % GPU_PS_JOBS] = e;
            MemoryBarrier();
            InterlockedExchange(&s_psjob_head, head + 1);
            SetEvent(s_psjob_event);
        }
        last_key = k;
        last_ent = e;
    }
    return e->state == 2 ? e->ps : s_gpu11.ps;
}
