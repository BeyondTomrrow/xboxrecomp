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
    uint32_t count, cap;
} SgXfList;

/* std140 mirror of the `Shade` block in shaders/xf.frag. Every member there
 * is a uvec4 or an array of them, so plain uint32_t arrays match it. */
typedef struct {
    uint32_t prog[4]; /* control (0 = combiners off), stage_prog, final0, final1 */
    uint32_t misc[4]; /* specfog[0], specfog[1], sampled-stage bits, unused */
    uint32_t alpha[4]; /* test on, func, ref, unused */
    uint32_t tex[4][4]; /* per stage: width, height, wrap u, wrap v */
    uint32_t factor0[8], factor1[8];
    uint32_t color_icw[8], color_ocw[8];
    uint32_t alpha_icw[8], alpha_ocw[8];
    uint32_t fog[4];
    uint32_t mode[4]; /* texture-shader mode per stage (sg_stage_mode) */
    uint32_t rect[4]; /* stage coordinates in texels (linear texture) */
    uint32_t depth[4];
} SgXfShade;

/* std140 mirror of `Viewport` in shaders/xf.vert. */
typedef struct {
    float surface_size[2];
    float depth_scale;
    float pad;
} SgXfViewport;

/*
* frame, print each 3D batch that covers surface pixel (x,y), in draw order,
* with the state that decides its colour there. The last one printed is
* what the pixel shows. A diagnostic for surfaces drawn wrong or not at all. */
static struct {
    int on, x, y;
    uint32_t frame; /* s_sg_frame being captured; 0 = none */
    uint64_t last_ms;
    int hit; /* the current batch covers the pixel */
    XfVert v[3]; /* its first covering triangle */
} s_probe;

static void sg_probe_init(void) {
    static int done;
    const char *e;
    if (done)
        return;
    done = 1;
    e = getenv("RECOMP_SDLGPU_PROBE");
    if (e && sscanf(e, "%d,%d", &s_probe.x, &s_probe.y) == 2)
        s_probe.on = 1;
}

/* Called once per flip, after s_sg_frame advances: start a capture every 10 s. */
static void sg_probe_flip(void) {
    uint64_t now;
    if (!s_probe.on)
        return;
    now = SDL_GetTicks();
    if (now - s_probe.last_ms < 10000)
        return;
    s_probe.last_ms = now;
    s_probe.frame = s_sg_frame;
    fprintf(stderr, "  [PROBE] ===== frame %u, pixel %d,%d =====\n",
            s_sg_frame, s_probe.x, s_probe.y);
}

static int sg_probe_active(void) {
    return s_probe.on && s_probe.frame == s_sg_frame;
}

static void sg_probe_print(void) {
    const XfVert *v = s_probe.v;
    fprintf(stderr, "  [PROBE] tex %08X %ux%u fmt %02X prog %08X comb %08X final %08X/%08X"
            " | blend %u %04X/%04X | depth %u %04X mask %u | alpha %u %04X %02X"
            " | cmask %08X | stencil %u\n",
            s_gpu.tex.valid ? s_gpu.tex.offset : 0, s_gpu.tex.width, s_gpu.tex.height,
            s_gpu.tex.color, s_comb.stage_prog, comb_active() ? s_comb.control : 0,
            s_comb.final0, s_comb.final1, s_gpu.blend, s_gpu.blend_sf, s_gpu.blend_df,
            s_vp.depth_test, s_vp.depth_func, s_vp.depth_mask, s_gpu.alpha_test,
            s_gpu.alpha_func, s_gpu.alpha_ref & 0xFF, g_color_mask, s_vp.stencil_test,
            s_tex_reg[(0x1B04 + 0x40 * 1 - NV_TEX_FIRST) / 4]);
    fprintf(stderr, "  [PROBE]   col %.2f %.2f %.2f %.2f spec %.2f %.2f %.2f %.2f"
            " uv %.1f %.1f | w %.1f %.1f %.1f | z %.0f\n",
            v[0].col[0], v[0].col[1], v[0].col[2], v[0].col[3],
            v[0].spec[0], v[0].spec[1], v[0].spec[2], v[0].spec[3],
            v[0].uv[0], v[0].uv[1], v[0].w, v[1].w, v[2].w, v[0].z);
    fprintf(stderr, "  [PROBE]   C0 %08X C1 %08X specfog %08X/%08X | stage1 %ux%u fmt %02X\n",
            s_comb.factor0[0], s_comb.factor1[0], s_comb.specfog[0], s_comb.specfog[1],
            s_stage[1].width, s_stage[1].height, s_stage[1].color);
    fprintf(stderr, "  [PROBE]   fog per vertex %.3f %.3f %.3f | enable %u mode %04X gen %04X"
            " params %g %g %g colour %08X\n",
            v[0].fog, v[1].fog, v[2].fog, s_fog.enable, s_fog.mode, s_fog.gen,
            s_fog.p[0], s_fog.p[1], s_fog.p[2], s_fog.color);
    fprintf(stderr, "  [PROBE]   stage0 rgb in %08X out %08X | alpha in %08X out %08X"
            " | C0[0..1] %08X %08X C1[0..1] %08X %08X | shaderctl %08X\n",
            s_comb.color_icw[0], s_comb.color_ocw[0], s_comb.alpha_icw[0], s_comb.alpha_ocw[0],
            s_comb.factor0[0], s_comb.factor0[1], s_comb.factor1[0], s_comb.factor1[1],
            s_comb.shaderctl);
}

static void sg_probe_watch(SgSurface *s, int vp)
{
    static uint32_t last, last_frame;
    uint32_t px;

    if (!sg_probe_active() || !s)
        return;
    if ((uint32_t)s_probe.x >= s->w || (uint32_t)s_probe.y >= s->h)
        return;
    if (last_frame != s_sg_frame) {          /* a new capture: report the first value */
        last_frame = s_sg_frame;
        last = 0xDEADBEEFu;
    }
    sg_queue_flush();
    if (s->owner == SG_OWNER_GPU)
        sg_download(s);
    memcpy(&px, sg_guest_bytes(s->guest) + (size_t)s_probe.y * s->pitch + (size_t)s_probe.x * 4u, 4);
    if (px == last)
        return;
    last = px;
    fprintf(stderr, "  [WATCH] pixel -> %08X after %s batch: tex %08X %ux%u prog %08X final %08X/%08X"
                    " | blend %u %04X/%04X | depth %u %04X mask %u | stencil %u func %04X"
                    " | alpha %u %04X %02X | clip %.0f..%.0f zmm %X\n",
            px, vp ? "VP" : "CPU", s_gpu.tex.valid ? s_gpu.tex.offset : 0, s_gpu.tex.width,
            s_gpu.tex.height, s_comb.stage_prog, s_comb.final0, s_comb.final1,
            s_gpu.blend, s_gpu.blend_sf, s_gpu.blend_df, s_vp.depth_test, s_vp.depth_func,
            s_vp.depth_mask, s_vp.stencil_test, s_vp.stencil_func, s_gpu.alpha_test,
            s_gpu.alpha_func, s_gpu.alpha_ref & 0xFF, s_vp.clip_min, s_vp.clip_max,
            (s_vp.zminmax >> 4) & 0xFu);
}


/* A stage's texture-shader mode (SET_SHADER_STAGE_PROGRAM, 5 bits a stage),
 * as the D3D11 path reads it: without active combiners, stage 0 is a plain
 * texture lookup. */
static uint32_t sg_stage_mode(int st) {
    uint32_t m = (s_comb.stage_prog >> (st * 5)) & 0x1Fu;
    if (st == 0 && !comb_active() && !m)
        m = 1;
    return m;
}

/* Whether a stage samples its texture (D3D11: NONE, PASSTHRU and DOTPRODUCT
 * read no texture). */
static int sg_stage_wants_texture(int st) {
    const Texture *tx = sg_stage_texture(st);
    uint32_t m = sg_stage_mode(st);
    if (st > 0 && !comb_active())
        return 0;
    return tx->valid && tx->enabled && m != 0 && m != 4 && m != 0x11;
}

/* batch_triangles_xf() callback: one transformed triangle. */
static void sg_collect_xf(void *ctx, const XfVert *a, const XfVert *b, const XfVert *c) {
    SgXfList *list = (SgXfList *) ctx;
    const XfVert *t[3] = {a, b, c};
    int k, affine = 0;

    if (!a->ok || !b->ok || !c->ok)
        return;
    /* Back-face culling, exactly as raster_triangle_xf does it (on with
     * RECOMP_CULL=1 for now, as there). Winding as the NV2A sees it: its
     * window space has y growing upward, so in these y-down screen
     * coordinates a positive area is counter-clockwise. Without culling the
     * backs of NFL 2K5's near-side stands were drawn over the field. */
    {
        static int cull_on = -1;
        if (cull_on < 0)
            cull_on = getenv("RECOMP_CULL") != NULL;
        if (cull_on && s_vp.cull) {
            float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
            int cw = area < 0;
            int front = (s_vp.front_face == 0x0900) ? cw : !cw;
            if ((s_vp.cull_face == 0x0404 && front) || (s_vp.cull_face == 0x0405 && !front) ||
                s_vp.cull_face == 0x0408)
                return;
        }
    }

    /* Pixel probe: does this triangle cover the probed pixel's centre?
    * (Same edge test as the software rasterizer.) */
    if (sg_probe_active() && !s_probe.hit) {
        float px = (float) s_probe.x + 0.5f, py = (float) s_probe.y + 0.5f;
        float w0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
        float w1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
        float w2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
        if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
            s_probe.hit = 1;
            s_probe.v[0] = *a;
            s_probe.v[1] = *b;
            s_probe.v[2] = *c;
        }
    }

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
                /* The state that decides whether these pixels survive. */
                fprintf(stderr, "  [UVTRACE]   depth test %u func %04X mask %u | stencil test %u"
                        " func %04X ref %02X fmask %02X wmask %02X ops %04X/%04X/%04X\n",
                        s_vp.depth_test, s_vp.depth_func, s_vp.depth_mask, s_vp.stencil_test,
                        s_vp.stencil_func, s_vp.stencil_ref & 0xFF, s_vp.stencil_fmask & 0xFF,
                        s_vp.stencil_wmask & 0xFF, s_vp.op_fail, s_vp.op_zfail, s_vp.op_zpass);
                fprintf(stderr, "  [UVTRACE]   alpha test %u func %04X ref %02X | blend %u"
                        " %04X/%04X eq %04X | colormask %08X\n",
                        s_gpu.alpha_test, s_gpu.alpha_func, s_gpu.alpha_ref & 0xFF, s_gpu.blend,
                        s_gpu.blend_sf, s_gpu.blend_df, s_gpu.blend_eq, g_color_mask);
                fprintf(stderr, "  [UVTRACE]   comb control %08X final %08X/%08X"
                        " stage1 valid %d en %d %ux%u fmt %02X\n",
                        s_comb.control, s_comb.final0, s_comb.final1,
                        s_stage[1].valid, s_stage[1].enabled, s_stage[1].width,
                        s_stage[1].height, s_stage[1].color);
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
        SgXfVertex *v = (SgXfVertex *) realloc(list->v, cap * sizeof *v);
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
        memcpy(o->tc, t[k]->tc, sizeof o->tc); /* oT0..oT3, raw */
        o->fog = t[k]->fog;
    }
}

/* The combiner program and per-batch state, for shaders/xf.frag. */
static void sg_xf_shade_fill(SgXfShade *sh, uint32_t sampled) {
    int st;

    memset(sh, 0, sizeof *sh);
    sh->prog[0] = comb_active() ? s_comb.control : 0;
    sh->prog[1] = s_comb.stage_prog;
    sh->prog[2] = s_comb.final0;
    sh->prog[3] = s_comb.final1;
    sh->misc[0] = s_comb.specfog[0];
    sh->misc[1] = s_comb.specfog[1];
    sh->misc[2] = sampled;
    /* RECOMP_SDLGPU_NOALPHATEST=1: never discard by alpha (diagnostic). */
    static int no_alpha_test = -1;
    if (no_alpha_test < 0)
        no_alpha_test = getenv("RECOMP_SDLGPU_NOALPHATEST") != NULL;
    sh->alpha[0] = s_gpu.alpha_test != 0 && !no_alpha_test;
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
    sh->fog[0] = s_fog.color;
    for (st = 0; st < 4; st++) {
        sh->mode[st] = sg_stage_mode(st);
        sh->rect[st] = !tex_size_from_format(sg_stage_texture(st)->color);
    }

    {
        float zmax = SG_DEPTH_MAX;
        int ranged = s_vp.clip_max > s_vp.clip_min;
        float cmin = ranged ? s_vp.clip_min : 0.0f;
        float cmax = ranged ? s_vp.clip_max : zmax;
        sh->depth[0] = ((s_vp.control0 >> 16) & 1u)
                     | ((((s_vp.zminmax >> 4) & 0xFu) == 0) ? 2u : 0u);
        memcpy(&sh->depth[1], &zmax, sizeof zmax);
        memcpy(&sh->depth[2], &cmin, sizeof cmin);
        memcpy(&sh->depth[3], &cmax, sizeof cmax);
    }
}

/* ── 3D batches with the vertex program on the GPU ──────────────────────
 * The inputs v0..v15 of each unique vertex are fetched on the CPU
 * (fetch_attr knows every attribute format); the generated shader runs the
 * program and the GPU clips in homogeneous space. Triangle assembly as the
 * D3D11 path (gpu_vp_build). */

static struct {
    float (*in)[NV_VERTEX_ATTRS][4]; /* inputs of each unique vertex */
    uint32_t in_cap;
    uint32_t *ix; /* 32-bit indices into in[] */
    uint32_t ix_cap;
    uint32_t map[65536], map_gen[65536], gen; /* guest index -> in[] */
    uint32_t nv, inmask;
} s_sgvp;

/* RECOMP_SDLGPU_NOVP=1 keeps vertex programs on the CPU (comparison). */
static int sg_vp_wanted(void) {
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_SDLGPU_NOVP") == NULL;
    return on;
}

/* in[] slot of guest vertex `index`, fetching its inputs on first use. */
static uint32_t sg_vp_vertex(uint32_t index) {
    uint32_t a, k;
    index &= 0xFFFF;
    if (s_sgvp.map_gen[index] == s_sgvp.gen)
        return s_sgvp.map[index];
    k = s_sgvp.nv++;
    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        float *o = s_sgvp.in[k][a];
        if (s_sgvp.inmask >> a & 1) {
            fetch_attr(&s_gpu.attr[a], index, o);
        } else {
            o[0] = o[1] = o[2] = 0.0f;
            o[3] = 1.0f;
        }
    }
    s_sgvp.map_gen[index] = s_sgvp.gen;
    s_sgvp.map[index] = k;
    return k;
}

/* Draw the batch with its vertex program on the GPU. Returns 0 to use the
 * CPU path instead: program still compiling or not translatable, points or
 * lines, or out of memory. */
static int sg_draw_vp(SgSurface *s, SgTextureUse tex[4], uint32_t sampled) {
    const uint16_t *src = s_gpu.idx;
    uint32_t n = s_gpu.idx_count, i, ni = 0, slot = 0;
    SDL_GPUGraphicsPipeline *pipe;

    if (!sg_vp_wanted())
        return 0;
    switch (s_gpu.prim) {
        case NV_PRIM_TRIANGLES:
        case NV_PRIM_TRIANGLE_STRIP:
        case NV_PRIM_TRIANGLE_FAN:
        case NV_PRIM_POLYGON:
        case NV_PRIM_QUADS:
        case NV_PRIM_QUAD_STRIP:
            break;
        default:
            return 0; /* points and lines: not on the GPU yet */
    }
    if (!sg_vp_program(&slot))
        return 0; /* compiling (or cannot): CPU this time */
    pipe = sg_pipeline_current(1, slot + 1);
    if (!pipe)
        return 0;

    /* Room for n unique vertices and up to 3n indices (fans, quads). */
    if (n > s_sgvp.in_cap) {
        void *g = realloc(s_sgvp.in, (size_t) n * sizeof *s_sgvp.in);
        if (!g) return 0;
        s_sgvp.in = (float (*)[NV_VERTEX_ATTRS][4]) g;
        s_sgvp.in_cap = n;
    }
    if (3 * n > s_sgvp.ix_cap) {
        void *g = realloc(s_sgvp.ix, (size_t) 3 * n * sizeof *s_sgvp.ix);
        if (!g) return 0;
        s_sgvp.ix = (uint32_t *) g;
        s_sgvp.ix_cap = 3 * n;
    }
    if (++s_sgvp.gen == 0) {
        memset(s_sgvp.map_gen, 0, sizeof s_sgvp.map_gen);
        s_sgvp.gen = 1;
    }
    s_sgvp.nv = 0;
    s_sgvp.inmask = s_sg_vp[slot].inmask;

#define VPT(a_, b_, c_) do { s_sgvp.ix[ni++] = sg_vp_vertex(src[a_]); \
                             s_sgvp.ix[ni++] = sg_vp_vertex(src[b_]); \
                             s_sgvp.ix[ni++] = sg_vp_vertex(src[c_]); } while (0)
    switch (s_gpu.prim) {
        case NV_PRIM_TRIANGLES:
            for (i = 0; i + 2 < n; i += 3)
                VPT(i, i + 1, i + 2);
            break;
        case NV_PRIM_TRIANGLE_STRIP:
            for (i = 0; i + 2 < n; i++) {
                if (i & 1)
                    VPT(i + 1, i, i + 2);
                else
                    VPT(i, i + 1, i + 2);
            }
            break;
        case NV_PRIM_TRIANGLE_FAN:
        case NV_PRIM_POLYGON:
            for (i = 1; i + 1 < n; i++)
                VPT(0, i, i + 1);
            break;
        case NV_PRIM_QUADS:
            for (i = 0; i + 3 < n; i += 4) {
                VPT(i, i + 1, i + 2);
                VPT(i, i + 2, i + 3);
            }
            break;
        case NV_PRIM_QUAD_STRIP:
            for (i = 0; i + 3 < n; i += 2) {
                VPT(i, i + 1, i + 3);
                VPT(i, i + 3, i + 2);
            }
            break;
        default:
            break;
    }
#undef VPT
    if (!ni)
        return 1; /* nothing to draw */

    {
        static SgVpUniforms u; /* 3 KB: not on the stack */
        SgXfShade shade;
        float zmax = SG_DEPTH_MAX;
        SgDraw d = {
            .surface = s, .pipe = pipe,
            .vertices = s_sgvp.in, .vertex_bytes = s_sgvp.nv * (uint32_t) sizeof *s_sgvp.in,
            .vertex_count = s_sgvp.nv,
            .indices = s_sgvp.ix, .index_count = ni,
            .tex = tex, .tex_count = 4,
            .vs_uniform = &u, .vs_uniform_size = sizeof u,
            .fs_uniform = &shade, .fs_uniform_size = sizeof shade,
        };

        /* The program's constants and the epilogue's parameters (as
         * gpu_vp_bind fills them; our surfaces have no x offset). */
        memcpy(u.c, s_vp.c, sizeof u.c);
        u.info[0] = (float) s->w;
        u.info[1] = (float) s->h;
        u.info[2] = zmax;
        u.info[3] = 0.0f;
        u.clipr[0] = s_vp.clip_min;
        u.clipr[1] = s_vp.clip_max;
        u.clipr[2] = s_vp.clip_max > s_vp.clip_min ? 1.0f : 0.0f;
        u.clipr[3] = 0.0f;
        u.fogp[0] = s_fog.enable ? 1.0f : 0.0f;
        memcpy(&u.fogp[1], &s_fog.mode, 4);
        u.fogp[2] = s_fog.p[0];
        u.fogp[3] = s_fog.p[1];
        u.lightp[0] = s_vp.specular_enable ? 1.0f : 0.0f;
        u.lightp[1] = (s_vp.light_control & (1u << 17)) ? 1.0f : 0.0f;
        u.lightp[2] = u.lightp[3] = 0.0f;

        sg_xf_shade_fill(&shade, sampled);
        if (!sg_queue_draw(&d))
            return 0;
        s_sg_stats.gpuvp++;
        return 1;
    }
}


/* gpu_draw_batch(0). Returns 0 to leave the batch to the software path. */
static int sg_draw_xf(void) {
    static SgXfList list;
    SgTextureUse tex[4] = { 0 };
    uint32_t sampled = 0;
    SDL_GPUGraphicsPipeline *pipe;
    SgSurface *s;
    int st;

    if (s_gpu.idx_count < 3)
        return 0;
    sg_vp_selfcheck();
    /* RECOMP_SDLGPU_SKIPCUBE=1: don't draw batches whose texture stage 1 is
         * a cube map (stage program mode 3), which neither renderer implements
         * yet (diagnostic). */
    {
        static int skip_cube = -1;
        if (skip_cube < 0)
            skip_cube = getenv("RECOMP_SDLGPU_SKIPCUBE") != NULL;
        if (skip_cube && comb_active() && ((s_comb.stage_prog >> 5) & 0x1Fu) == 3)
            return 1; /* "drawn": nothing */
    }
    if (!sg_draw_init())
        return sg_fallback(SG_FB_SHADERS, 1);
    s = sg_surface_current();
    if (!s)
        return sg_fallback(SG_FB_TARGET, 1);
    pipe = sg_pipeline_current(1, 0);
    if (!pipe)
        return sg_fallback(SG_FB_BLEND, 1); /* blend state SDL cannot express */

    /* Stage 0 is sampled whenever enabled; stages 1..3 only feed the
     * combiners, and only when their stage program is set (as in
     * raster_triangle_xf). */
    /* Bind each stage's texture when its mode samples one (D3D11 rule). */
    for (st = 0; st < 4; st++)
        if (sg_stage_wants_texture(st)) {
            if (!sg_texture_for_stage(st, &tex[st]))
                return sg_fallback(SG_FB_TEXTURE, 1);
            sampled |= 1u << st;
        }

    /* The vertex program on the GPU when its shader is ready; otherwise */
    if (sg_draw_vp(s, tex, sampled)) {
        s_sg_stats.gpu3d++;
        sg_probe_watch(s, 1);
        return 1;
    }
    sg_probe_init();
    s_probe.hit = 0;
    list.count = 0;
    batch_triangles_xf(sg_collect_xf, &list);
    if (s_probe.hit)
        sg_probe_print();
    if (!list.count)
        return 1; /* nothing visible, as in software */

    {
        SgXfShade shade;
        SgXfViewport view = {{(float) s->w, (float) s->h}, 1.0f / SG_DEPTH_MAX, 0.0f};
        SgDraw d = {
            .surface = s, .pipe = pipe,
            .vertices = list.v, .vertex_bytes = list.count * (uint32_t) sizeof(SgXfVertex),
            .vertex_count = list.count,
            .tex = tex, .tex_count = 4,
            .vs_uniform = &view, .vs_uniform_size = sizeof view,
            .fs_uniform = &shade, .fs_uniform_size = sizeof shade,
        };
        sg_xf_shade_fill(&shade, sampled);
        if (!sg_queue_draw(&d))
            return sg_fallback(SG_FB_RESOURCES, 1);
        fprintf(stderr, "  [SDLGPU] xf resources: pipeline\n");   /* or: queue, or: vertex grow */
        s_sg_stats.gpu3d++;
        sg_probe_watch(s, 0);
        return 1;
    }
}
