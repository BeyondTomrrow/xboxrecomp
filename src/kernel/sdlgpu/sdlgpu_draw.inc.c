/* ── SDL3 GPU renderer: drawing ──────────────────────────────────────────
 *
 * sg_draw_init()   loads the shaders and small fixed resources, once.
 * sg_submit()      the shared tail of every draw: one transfer (vertices,
 *                  the surface's guest copy when stale, new textures), then
 *                  the render pass. Used by flat 2D here and 3D in
 *                  sdlgpu_xf.inc.c.
 * sg_draw_flat2d() pre-transformed (fixed-function) batches: menus, HUD,
 *                  text, movie quads. The triangles come from
 *                  batch_triangles(), the same assembly the software
 *                  rasterizer uses, and shaders/flat2d.* reproduce its
 *                  shading bit for bit. Blending is GPU fixed-function and
 *                  may differ from the software integer blend by up to 2/255.
 */

#ifdef SG_HAVE_SHADERS
#include "flat2d_vert.h"
#include "flat2d_frag.h"
#include "xf_vert.h"
#include "xf_frag.h"
#endif

typedef struct {
    SgVertex *v;
    uint32_t  count, cap;
} SgVertexList;

/* Everything one draw needs. Textures bind to fragment samplers 0..n-1;
 * an entry with tex == NULL binds the white texture. */
typedef struct {
    SgSurface               *surface;
    SDL_GPUGraphicsPipeline *pipe;
    const void              *vertices;
    uint32_t                 vertex_bytes, vertex_count;
    SgTextureUse            *tex;
    int                      tex_count;
    const void              *vs_uniform;
    uint32_t                 vs_uniform_size;
    const void              *fs_uniform;
    uint32_t                 fs_uniform_size;
} SgDraw;

#ifdef SG_HAVE_SHADERS
static SDL_GPUShader *sg_shader(const void *code, size_t size, SDL_GPUShaderStage stage,
                                Uint32 samplers)
{
    SDL_GPUShaderCreateInfo info = {
        .code = (const Uint8 *)code, .code_size = size, .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = stage,
        .num_samplers = samplers, .num_uniform_buffers = 1,
    };
    return SDL_CreateGPUShader(s_sg.dev, &info);
}
#endif

/* Load the shaders and the small fixed resources, once. */
static int sg_draw_init(void)
{
#ifdef SG_HAVE_SHADERS
    static int tried;
    SDL_GPUSamplerCreateInfo point = {
        .min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
    };
    SDL_GPUTextureCreateInfo white = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = 1, .height = 1,
        .layer_count_or_depth = 1, .num_levels = 1,
    };

    if (s_sg.draw_ready || tried)
        return s_sg.draw_ready;
    tried = 1;
    s_sg.flat2d_vs = sg_shader(k_sg_flat2d_vert, sizeof k_sg_flat2d_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0);
    s_sg.flat2d_fs = sg_shader(k_sg_flat2d_frag, sizeof k_sg_flat2d_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    s_sg.xf_vs = sg_shader(k_sg_xf_vert, sizeof k_sg_xf_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0);
    s_sg.xf_fs = sg_shader(k_sg_xf_frag, sizeof k_sg_xf_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 4);
    s_sg.point = SDL_CreateGPUSampler(s_sg.dev, &point);
    s_sg.white = SDL_CreateGPUTexture(s_sg.dev, &white);
    if (!s_sg.flat2d_vs || !s_sg.flat2d_fs || !s_sg.xf_vs || !s_sg.xf_fs || !s_sg.point || !s_sg.white) {
        fprintf(stderr, "  [SDLGPU] drawing unavailable: %s\n", SDL_GetError());
        return 0;
    }
    s_sg.draw_ready = 1;
    return 1;
#else
    static int told;
    if (!told++)
        fprintf(stderr, "  [SDLGPU] built without shaders (glslang missing); drawing stays in software\n");
    return 0;
#endif
}

/* Room for `bytes` in the upload transfer buffer. */
static int sg_upload_reserve(uint32_t bytes)
{
    SDL_GPUTransferBufferCreateInfo info = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD };
    if (bytes <= s_sg.upload_size)
        return 1;
    if (s_sg.upload)
        SDL_ReleaseGPUTransferBuffer(s_sg.dev, s_sg.upload);
    info.size = bytes;
    s_sg.upload = SDL_CreateGPUTransferBuffer(s_sg.dev, &info);
    s_sg.upload_size = s_sg.upload ? bytes : 0;
    return s_sg.upload != NULL;
}

static int sg_vertex_buffer_reserve(uint32_t bytes)
{
    SDL_GPUBufferCreateInfo info = { .usage = SDL_GPU_BUFFERUSAGE_VERTEX };
    if (bytes <= s_sg.vertices_size)
        return 1;
    if (s_sg.vertices)
        SDL_ReleaseGPUBuffer(s_sg.dev, s_sg.vertices);
    info.size = bytes * 2;
    s_sg.vertices = SDL_CreateGPUBuffer(s_sg.dev, &info);
    s_sg.vertices_size = s_sg.vertices ? info.size : 0;
    return s_sg.vertices != NULL;
}

/* Upload and draw. Returns 0 (nothing submitted) if resources run out. */
static int sg_submit(const SgDraw *d)
{
    SgSurface *s = d->surface;
    uint32_t sbytes = s->owner == SG_OWNER_GUEST ? s->w * s->h * 4 : 0;
    uint32_t tbytes = 0, toff[4] = { 0 }, at;
    SDL_GPUCommandBuffer *cmd;
    uint8_t *stage;
    int i;

    /* Layout of the transfer: vertices | surface | texture 0 | texture 1 ... */
    at = d->vertex_bytes + sbytes;
    for (i = 0; i < d->tex_count; i++)
        if (d->tex[i].upload) {
            toff[i] = at;
            at += d->tex[i].w * d->tex[i].h * 4;
            tbytes += d->tex[i].w * d->tex[i].h * 4;
        }
    if (!sg_vertex_buffer_reserve(d->vertex_bytes) ||
        !sg_upload_reserve(d->vertex_bytes + sbytes + tbytes))
        return 0;

    stage = (uint8_t *)SDL_MapGPUTransferBuffer(s_sg.dev, s_sg.upload, true);
    memcpy(stage, d->vertices, d->vertex_bytes);
    if (sbytes)
        sg_surface_pack(s, stage + d->vertex_bytes);
    for (i = 0; i < d->tex_count; i++)
        if (d->tex[i].upload)
            memcpy(stage + toff[i], d->tex[i].upload, (size_t)d->tex[i].w * d->tex[i].h * 4);
    SDL_UnmapGPUTransferBuffer(s_sg.dev, s_sg.upload);

    cmd = SDL_AcquireGPUCommandBuffer(s_sg.dev);
    {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation from = { .transfer_buffer = s_sg.upload, .offset = 0 };
        SDL_GPUBufferRegion to = { .buffer = s_sg.vertices, .offset = 0, .size = d->vertex_bytes };
        SDL_UploadToGPUBuffer(copy, &from, &to, true);
        if (sbytes) {
            SDL_GPUTextureTransferInfo src = { .transfer_buffer = s_sg.upload, .offset = d->vertex_bytes };
            SDL_GPUTextureRegion dst = { .texture = s->color, .w = s->w, .h = s->h, .d = 1 };
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
        for (i = 0; i < d->tex_count; i++)
            if (d->tex[i].upload) {
                SDL_GPUTextureTransferInfo src = { .transfer_buffer = s_sg.upload, .offset = toff[i] };
                SDL_GPUTextureRegion dst = { .texture = d->tex[i].tex,
                                             .w = d->tex[i].w, .h = d->tex[i].h, .d = 1 };
                SDL_UploadToGPUTexture(copy, &src, &dst, false);
                sg_texture_commit(&d->tex[i]);
            }
        SDL_EndGPUCopyPass(copy);
    }
    {
        SDL_GPUColorTargetInfo target = {
            .texture = s->color, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE,
        };
        SDL_GPUDepthStencilTargetInfo depth = sg_depth_target(s, 0, 0);
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, &depth);
        SDL_GPUViewport view = { 0, 0, (float)s->w, (float)s->h, 0.0f, 1.0f };
        SDL_Rect clip = { (int)s_gpu.clip_x, (int)s_gpu.clip_y, (int)s_gpu.clip_w, (int)s_gpu.clip_h };
        SDL_GPUBufferBinding vb = { .buffer = s_sg.vertices, .offset = 0 };
        SDL_GPUTextureSamplerBinding tb[4];
        SDL_FColor constant = {
            (float)((s_gpu.blend_color >> 16) & 0xFF) / 255.0f, (float)((s_gpu.blend_color >> 8) & 0xFF) / 255.0f,
            (float)(s_gpu.blend_color & 0xFF) / 255.0f, (float)(s_gpu.blend_color >> 24) / 255.0f,
        };

        for (i = 0; i < d->tex_count; i++)
            tb[i] = (SDL_GPUTextureSamplerBinding){
                .texture = d->tex[i].tex ? d->tex[i].tex : s_sg.white, .sampler = s_sg.point };

        SDL_BindGPUGraphicsPipeline(pass, d->pipe);
        SDL_SetGPUViewport(pass, &view);
        SDL_SetGPUScissor(pass, &clip);
        SDL_SetGPUBlendConstants(pass, constant);
        SDL_SetGPUStencilReference(pass, (Uint8)(s_vp.stencil_ref & 0xFF));
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_BindGPUFragmentSamplers(pass, 0, tb, (Uint32)d->tex_count);
        SDL_PushGPUVertexUniformData(cmd, 0, d->vs_uniform, d->vs_uniform_size);
        SDL_PushGPUFragmentUniformData(cmd, 0, d->fs_uniform, d->fs_uniform_size);
        SDL_DrawGPUPrimitives(pass, d->vertex_count, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    s->owner = SG_OWNER_GPU;
    return 1;
}

/* batch_triangles() callback: one triangle, as the software path would draw it. */
static void sg_collect(void *ctx, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    SgVertexList *list = (SgVertexList *)ctx;
    const uint32_t idx[3] = { i0, i1, i2 };
    float p[3][4], uv[3][2];
    int k, textured;

    for (k = 0; k < 3; k++)
        if (!fetch_attr(&s_gpu.attr[0], idx[k], p[k]))
            return;                                   /* raster_indexed skips it too */
    textured = s_gpu.tex.valid;
    for (k = 0; k < 3 && textured; k++)
        textured = fetch_texcoord(idx[k], uv[k]);

    if (list->count + 3 > list->cap) {
        uint32_t cap = list->cap ? list->cap * 2 : 192;
        SgVertex *v = (SgVertex *)realloc(list->v, cap * sizeof *v);
        if (!v)
            return;
        list->v = v;
        list->cap = cap;
    }
    for (k = 0; k < 3; k++) {
        SgVertex *o = &list->v[list->count++];
        o->x = p[k][0];
        o->y = p[k][1];
        o->color[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        o->color[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        o->color[2] = (float)(argb & 0xFF) / 255.0f;
        o->color[3] = (float)(argb >> 24) / 255.0f;
        o->uv[0] = textured ? uv[k][0] : 0.0f;
        o->uv[1] = textured ? uv[k][1] : 0.0f;
        o->uv[2] = textured ? 1.0f : 0.0f;
    }
}

/* gpu_draw_batch(1). Returns 0 to leave the batch to the software path. */
static int sg_draw_flat2d(void)
{
    static SgVertexList list;
    SgTextureUse tex = { 0 };
    SDL_GPUGraphicsPipeline *pipe;
    SgSurface *s;

    if (s_gpu.idx_count < 3 || !batch_is_screen_space())
        return 0;                        /* raster_batch keeps its counters */
    if (!sg_draw_init())
        return 0;
    s = sg_surface_current();
    if (!s)
        return 0;                        /* 16-bit target */
    pipe = sg_pipeline_current(0);
    if (!pipe)
        return 0;                        /* blend state SDL cannot express */
    if (s_gpu.tex.valid && !sg_texture_stage0(&tex))
        return 0;                        /* unreadable texture: software decides */

    list.count = 0;
    batch_triangles(sg_collect, &list);
    if (!list.count)
        return 1;                        /* nothing visible, as in software */

    {
        float surface_size[2] = { (float)s->w, (float)s->h };
        struct { uint32_t tex_size[2], tex_wrap[2], alpha_test, alpha_func, alpha_ref, pad; } shade = {
            { s_gpu.tex.width, s_gpu.tex.height },
            { s_gpu.tex.addr_u == 1, s_gpu.tex.addr_v == 1 },
            s_gpu.alpha_test != 0, s_gpu.alpha_func, s_gpu.alpha_ref & 0xFF, 0,
        };
        SgDraw d = {
            .surface = s, .pipe = pipe,
            .vertices = list.v, .vertex_bytes = list.count * (uint32_t)sizeof(SgVertex),
            .vertex_count = list.count,
            .tex = &tex, .tex_count = 1,
            .vs_uniform = surface_size, .vs_uniform_size = sizeof surface_size,
            .fs_uniform = &shade, .fs_uniform_size = sizeof shade,
        };
        return sg_submit(&d);
    }
}