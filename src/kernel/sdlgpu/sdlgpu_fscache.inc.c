/* ── SDL3 GPU renderer: specialised fragment shaders ────────────────────
 *
 * shaders/xf.frag is an interpreter: per pixel it walks the register
 * combiner program held in its uniforms, through a 16-entry register file
 * indexed at run time. That made 3D batches fragment-bound -- ~12 ms of GPU
 * a frame for the front end at 1x on Apple silicon, ~85 ms at 3x.
 *
 * Here each distinct combiner setup gets its own variant: xf.frag's source
 * (embedded, xf_frag_src.h) with the control words prepended as constants
 * (see "control words" in xf.frag), so the shader compiler unrolls and folds
 * it. Colours, factors, texture sizes and depth ranges stay uniforms, so
 * one variant serves every draw with the same program.
 *
 * Variants compile on the vertex-program worker (glslang is used from that
 * one thread only); until one is ready the batch draws with the generic
 * shader, so nothing waits. RECOMP_SDLGPU_NOSPEC=1 turns variants off.
 */

#include "xf_frag_src.h"

#define SG_FS_CACHE 512                /* power of two */
#define SG_FS_KEY   61

static SgVpProg s_sg_fsv[SG_FS_CACHE];

static SDL_GPUShader *sg_fs_shader(uint32_t slot)
{
    return slot && slot <= SG_FS_CACHE ? s_sg_fsv[slot - 1].vs : NULL;
}

/* The control words a variant bakes in. Words that cannot affect the
 * result (stages past the stage count, combiner words with combiners off,
 * the alpha function with the test off) are zeroed so they don't split
 * variants. */
static void sg_fs_key(const SgXfShade *sh, uint32_t w[SG_FS_KEY])
{
    uint32_t control = sh->prog[0];
    int stages = control ? (int) (control & 0xFFu) : 0, s;
    float zmax, cmin, cmax;

    memset(w, 0, SG_FS_KEY * sizeof *w);
    if (stages > 8) stages = 8;
    if (control)
        memcpy(&w[0], sh->prog, 16);
    w[4] = sh->misc[2];
    w[5] = sh->alpha[0];
    w[6] = sh->alpha[0] ? sh->alpha[1] : 0;
    for (s = 0; s < stages; s++) {
        w[7 + s]  = sh->color_icw[s];
        w[15 + s] = sh->color_ocw[s];
        w[23 + s] = sh->alpha_icw[s];
        w[31 + s] = sh->alpha_ocw[s];
    }
    memcpy(&w[39], sh->mode, 16);
    memcpy(&w[43], sh->rect, 16);
    w[47] = sh->depth[0];
    memcpy(&zmax, &sh->depth[1], 4);
    memcpy(&cmin, &sh->depth[2], 4);
    memcpy(&cmax, &sh->depth[3], 4);
    /* No w-buffer and the clip range is the whole range: the rasterised
     * depth is what the shader would write. */
    w[48] = !(sh->depth[0] & 5u) && cmin == 0.0f && cmax == zmax;
    memcpy(&w[49], sh->dotmap, 16);
    memcpy(&w[53], sh->sinput, 16);
    memcpy(&w[57], sh->cube, 16);
}

static uint32_t sg_fs_hash(const uint32_t w[SG_FS_KEY])
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < SG_FS_KEY; i++)
        h = (h ^ w[i]) * 16777619u;
    return h;
}

static void sg_fs_append(char **at, char *end, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(*at, (size_t) (end - *at), fmt, ap);
    va_end(ap);
    if (n > 0) *at += n < end - *at ? n : end - *at - 1;
}

static void sg_fs_uvec4x2(char **at, char *end, const char *name, const uint32_t *v)
{
    sg_fs_append(at, end, "const uvec4 %s[2] = uvec4[2](uvec4(%uu,%uu,%uu,%uu), uvec4(%uu,%uu,%uu,%uu));\n",
                 name, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

/* xf.frag with the key's words as constants. malloc'd. */
static char *sg_fs_source(const uint32_t w[SG_FS_KEY])
{
    const char *body = strchr(k_sg_xf_frag_src, '\n');   /* past "#version" */
    size_t cap = strlen(k_sg_xf_frag_src) + 2048;
    char *src = (char *) malloc(cap), *at = src, *end = src + cap;
    if (!src || !body)
        return free(src), NULL;
    sg_fs_append(&at, end, "#version 450\n#define SG_SPEC 1\n%s",
                 w[48] ? "#define P_NO_FRAGDEPTH 1\n" : "");
    sg_fs_append(&at, end, "const uvec4 P_PROG = uvec4(%uu,%uu,%uu,%uu);\n", w[0], w[1], w[2], w[3]);
    sg_fs_append(&at, end, "const uint P_SAMPLED = %uu;\nconst uint P_ALPHA_ON = %uu;\n"
                           "const uint P_ALPHA_FUNC = %uu;\nconst uint P_DEPTHX = %uu;\n",
                 w[4], w[5], w[6], w[47]);
    sg_fs_uvec4x2(&at, end, "P_CICW", &w[7]);
    sg_fs_uvec4x2(&at, end, "P_COCW", &w[15]);
    sg_fs_uvec4x2(&at, end, "P_AICW", &w[23]);
    sg_fs_uvec4x2(&at, end, "P_AOCW", &w[31]);
    sg_fs_append(&at, end, "const uvec4 P_MODE = uvec4(%uu,%uu,%uu,%uu);\n", w[39], w[40], w[41], w[42]);
    sg_fs_append(&at, end, "const uvec4 P_RECT = uvec4(%uu,%uu,%uu,%uu);\n", w[43], w[44], w[45], w[46]);
    sg_fs_append(&at, end, "const uvec4 P_DOTMAP = uvec4(%uu,%uu,%uu,%uu);\n", w[49], w[50], w[51], w[52]);
    sg_fs_append(&at, end, "const uvec4 P_INPUT = uvec4(%uu,%uu,%uu,%uu);\n", w[53], w[54], w[55], w[56]);
    sg_fs_append(&at, end, "const uvec4 P_CUBE = uvec4(%uu,%uu,%uu,%uu);\n", w[57], w[58], w[59], w[60]);
    sg_fs_append(&at, end, "%s", body);
    return src;
}

static int sg_fs_wanted(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_SDLGPU_NOSPEC") == NULL;
    return on && !s_sg_toggle_nospec;
}

/* The variant for this draw's shade: its cache slot + 1, or 0 to draw with
 * the generic shader (compiling, failed, full, or variants off). */
static uint32_t sg_fs_variant(const SgXfShade *sh)
{
    uint32_t w[SG_FS_KEY], key, i;

    if (!sg_fs_wanted() || !sg_vp_worker_start())
        return 0;
    sg_fs_key(sh, w);
    key = sg_fs_hash(w);
    for (i = 0; i < SG_FS_CACHE; i++) {
        uint32_t at = (key + i) & (SG_FS_CACHE - 1);
        SgVpProg *p = &s_sg_fsv[at];
        int st = SDL_GetAtomicInt(&p->state);

        if (st == SG_VP_EMPTY) {
            char *src = sg_fs_source(w);
            if (!src)
                return 0;
            p->key = key;
            p->frag = 1;
            memcpy(p->fkey, w, sizeof w);
            p->src = src;
            SDL_SetAtomicInt(&p->state, SG_VP_COMPILING);
            SDL_LockMutex(s_sg_vpq.lock);
            s_sg_vpq.jobs[s_sg_vpq.tail++ % SG_VP_JOBS] = p;
            SDL_SignalCondition(s_sg_vpq.wake);
            SDL_UnlockMutex(s_sg_vpq.lock);
            return 0;
        }
        if (p->key != key || memcmp(p->fkey, w, sizeof w) != 0)
            continue;
        if (st == SG_VP_SPIRV) {
            if (p->msl) {
                p->vs = sg_shader_from_msl(p->msl, SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1);
                free(p->msl);
                p->msl = NULL;
            } else {
                p->vs = sg_shader_from_spirv(p->spirv, p->words * sizeof *p->spirv,
                                             SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1);
            }
            free(p->spirv);
            p->spirv = NULL;
            if (!p->vs)
                sg_gpu_error("fragment variant shader");
            st = p->vs ? SG_VP_READY : SG_VP_FAILED;
            SDL_SetAtomicInt(&p->state, st);
        }
        return st == SG_VP_READY ? at + 1 : 0;
    }
    return 0;
}
