/* ── SDL3 GPU renderer: 3D (vertex-program) draws ────────────────────────
 *
 * The vertex program still runs on the CPU (vp_run); batch_triangles_xf()
 * hands over the same transformed, near-plane-clipped triangles the
 * software rasterizer draws. shaders/xf.* rasterize and shade them: the
 * register combiners when active, otherwise texel MODULATE diffuse, as
 * raster_triangle_xf does.
 *
 * Not yet on the GPU (the batch stays in software): depth and stencil
 * tests (milestone 2, step 3). Culling is off in software unless
 * RECOMP_CULL=1, and off here.
 */



typedef struct {
    SgXfVertex *v;
    uint32_t    count, cap;
} SgXfList;

/* std140 mirror of the `Shade` block in shaders/xf.frag. Every member there
 * is a uvec4 or an array of them, so plain uint32_t arrays match it. */
typedef struct {
    uint32_t prog[4];          /* control (0 = combiners off), stage_prog, final0, final1 */
    uint32_t misc[4];          /* specfog[0], specfog[1], sampled-stage bits, unused */
    uint32_t alpha[4];         /* test on, func, ref, unused */
    uint32_t tex[4][4];        /* per stage: width, height, wrap u, wrap v */
    uint32_t factor0[8], factor1[8];
    uint32_t color_icw[8], color_ocw[8];
    uint32_t alpha_icw[8], alpha_ocw[8];
} SgXfShade;

/* std140 mirror of `Viewport` in shaders/xf.vert. */
typedef struct {
    float surface_size[2];
    float depth_scale;
    float pad;
} SgXfViewport;

/* batch_triangles_xf() callback: one transformed triangle. */
static void sg_collect_xf(void *ctx, const XfVert *a, const XfVert *b, const XfVert *c)
{
    SgXfList *list = (SgXfList *)ctx;
    const XfVert *t[3] = { a, b, c };
    int k, affine = 0;

    if (!a->ok || !b->ok || !c->ok)
        return;                                       /* raster_triangle_xf rejects these */

    /* Some 2D vertex programs leave w at 0 or below with valid screen
     * positions (NFL 2K5's coach-matchup pie charts), and software draws
     * them. To a GPU, w <= 0 is behind the eye and gets clipped away, so such
     * triangles use w = 1: plain screen-space interpolation, which equals the
     * software result whenever the three w agree (the pie-chart case). */
    for (k = 0; k < 3; k++)
        if (!(t[k]->w > 1e-3f) || !isfinite(t[k]->w))
            affine = 1;
    /* RECOMP_SDLGPU_UVTRACE=1: describe large triangles (the field, the
* stands) once per texture -- what the vertex program hands over, to
* find why big ground surfaces come out untextured in both renderers. */
    {
        static int trace = -1;
        static uint32_t seen[64];
        static int nseen;
        float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
        if (trace < 0)
            trace = getenv("RECOMP_SDLGPU_UVTRACE") != NULL;
        if (trace && fabsf(area) > 0.05f * 640.0f * 480.0f && nseen < 64) {
            uint32_t off = s_gpu.tex.valid ? s_gpu.tex.offset : 0;
            int j, known = 0;
            for (j = 0; j < nseen; j++)
                known |= seen[j] == off;
            if (!known) {
                seen[nseen++] = off;
                fprintf(stderr, "  [UVTRACE] tex %08X %ux%u fmt %02X wrap %u/%u valid %d en %d"
                                " comb %d prog %08X area %.0f\n",
                        off, s_gpu.tex.width, s_gpu.tex.height, s_gpu.tex.color,
                        s_gpu.tex.addr_u, s_gpu.tex.addr_v, s_gpu.tex.valid, s_gpu.tex.enabled,
                        comb_active(), s_comb.stage_prog, area);

                for (j = 0; j < 3; j++)
                    fprintf(stderr, "  [UVTRACE]   v%d xy %.1f %.1f z %.0f w %.4f uv %.2f %.2f"
                                    " col %.2f %.2f %.2f %.2f\n",
                            j, t[j]->x, t[j]->y, t[j]->z, t[j]->w, t[j]->uv[0], t[j]->uv[1],
                            t[j]->col[0], t[j]->col[1], t[j]->col[2], t[j]->col[3]);
            }
        }
    }

    if (list->count + 3 > list->cap) {
        uint32_t cap = list->cap ? list->cap * 2 : 192;
        SgXfVertex *v = (SgXfVertex *)realloc(list->v, cap * sizeof *v);
        if (!v)
            return;
        list->v = v;
        list->cap = cap;
    }
    for (k = 0; k < 3; k++) {
        SgXfVertex *o = &list->v[list->count++];
        o->pos[0] = t[k]->x;
        o->pos[1] = t[k]->y;
        o->pos[2] = t[k]->z;
        o->pos[3] = affine ? 1.0f : t[k]->w;
        memcpy(o->col, t[k]->col, sizeof o->col);
        memcpy(o->spec, t[k]->spec, sizeof o->spec);
        o->uv[0][0] = t[k]->uv[0];
        o->uv[0][1] = t[k]->uv[1];
        memcpy(o->uv[1], t[k]->uvn, sizeof t[k]->uvn);   /* oT1..oT3 */
    }
}

/* The combiner program and per-batch state, for shaders/xf.frag. */
static void sg_xf_shade_fill(SgXfShade *sh, uint32_t sampled)
{
    int st;

    memset(sh, 0, sizeof *sh);
    sh->prog[0] = comb_active() ? s_comb.control : 0;
    sh->prog[1] = s_comb.stage_prog;
    sh->prog[2] = s_comb.final0;
    sh->prog[3] = s_comb.final1;
    sh->misc[0] = s_comb.specfog[0];
    sh->misc[1] = s_comb.specfog[1];
    sh->misc[2] = sampled;
    sh->alpha[0] = s_gpu.alpha_test != 0;
    sh->alpha[1] = s_gpu.alpha_func;
    sh->alpha[2] = s_gpu.alpha_ref & 0xFF;
    for (st = 0; st < 4; st++) {
        const Texture *tx = sg_stage_texture(st);
        sh->tex[st][0] = tx->width;
        sh->tex[st][1] = tx->height;
        sh->tex[st][2] = tx->addr_u;
        sh->tex[st][3] = tx->addr_v;
    }
    memcpy(sh->factor0, s_comb.factor0, sizeof sh->factor0);
    memcpy(sh->factor1, s_comb.factor1, sizeof sh->factor1);
    memcpy(sh->color_icw, s_comb.color_icw, sizeof sh->color_icw);
    memcpy(sh->color_ocw, s_comb.color_ocw, sizeof sh->color_ocw);
    memcpy(sh->alpha_icw, s_comb.alpha_icw, sizeof sh->alpha_icw);
    memcpy(sh->alpha_ocw, s_comb.alpha_ocw, sizeof sh->alpha_ocw);
}

/* gpu_draw_batch(0). Returns 0 to leave the batch to the software path. */
static int sg_draw_xf(void)
{
    static SgXfList list;
    SgTextureUse tex[4] = { { 0 } };
    uint32_t sampled = 0;
    SDL_GPUGraphicsPipeline *pipe;
    SgSurface *s;
    int st;

    if (s_gpu.idx_count < 3)
        return 0;
    if (!sg_draw_init())
        return sg_fallback(SG_FB_SHADERS, 1);
    s = sg_surface_current();
    if (!s)
        return sg_fallback(SG_FB_TARGET, 1);
    pipe = sg_pipeline_current(1);
    if (!pipe)
        return sg_fallback(SG_FB_BLEND, 1);                       /* blend state SDL cannot express */

    /* Stage 0 is sampled whenever enabled; stages 1..3 only feed the
     * combiners, and only when their stage program is set (as in
     * raster_triangle_xf). */
    if (s_gpu.tex.valid && s_gpu.tex.enabled) {
        if (!sg_texture_for_stage(0, &tex[0]))
            return sg_fallback(SG_FB_TEXTURE, 1);
        sampled |= 1u;
    }
    if (comb_active())
        for (st = 1; st < 4; st++)
            if (((s_comb.stage_prog >> (st * 5)) & 0x1Fu) && s_stage[st].valid && s_stage[st].enabled) {
                if (!sg_texture_for_stage(st, &tex[st]))
                    return sg_fallback(SG_FB_TEXTURE, 1);
                sampled |= 1u << st;
            }

    list.count = 0;
    batch_triangles_xf(sg_collect_xf, &list);
    if (!list.count)
        return 1;                        /* nothing visible, as in software */

    {
        SgXfShade shade;
        SgXfViewport view = { { (float)s->w, (float)s->h }, 1.0f / SG_DEPTH_MAX, 0.0f };
        SgDraw d = {
            .surface = s, .pipe = pipe,
            .vertices = list.v, .vertex_bytes = list.count * (uint32_t)sizeof(SgXfVertex),
            .vertex_count = list.count,
            .tex = tex, .tex_count = 4,
            .vs_uniform = &view, .vs_uniform_size = sizeof view,
            .fs_uniform = &shade, .fs_uniform_size = sizeof shade,
        };
        sg_xf_shade_fill(&shade, sampled);
        if (!sg_queue_draw(&d))
            return sg_fallback(SG_FB_RESOURCES, 1);
        s_sg_stats.gpu3d++;
        return 1;
    }
}