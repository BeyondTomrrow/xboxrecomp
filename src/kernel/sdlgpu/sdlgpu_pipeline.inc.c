/* ── SDL3 GPU renderer: pipelines ────────────────────────────────────────
 *
 * The NV2A blend and write-mask state becomes SDL_GPU pipeline state, one
 * pipeline per distinct combination, cached. The mapping mirrors the
 * software path (shade_pixel / blend_factor in nv2a_pb_exec.c): the same
 * factor applies to colour and alpha, unknown factors mean ONE, unknown
 * equations mean ADD.
 *
 * Two kinds of pipeline share the cache: flat 2D (shaders/flat2d.*) and 3D
 * (shaders/xf.*). The kind is part of the key (SG_KEY_XF).
 */

#define SG_KEY_VALID (1u << 31)    /* never 0 when valid */
#define SG_KEY_XF    (1u << 30)    /* 3D pipeline: xf shaders, SgXfVertex */

/* NV2A (GL) blend factor -> SDL. INVALID when SDL has no equivalent. */
static SDL_GPUBlendFactor sg_blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return SDL_GPU_BLENDFACTOR_ZERO;
    case 0x0300: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 0x0301: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 0x0303: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case 0x0305: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 0x0307: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case 0x8002: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003:                                      /* CONSTANT_ALPHA: none in SDL */
    case 0x8004: return SDL_GPU_BLENDFACTOR_INVALID;
    default:     return SDL_GPU_BLENDFACTOR_ONE;      /* blend_factor() returns 255 */
    }
}

static SDL_GPUBlendOp sg_blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return SDL_GPU_BLENDOP_SUBTRACT;
    case 0x800B: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case 0x8007: return SDL_GPU_BLENDOP_MIN;
    case 0x8008: return SDL_GPU_BLENDOP_MAX;
    default:     return SDL_GPU_BLENDOP_ADD;
    }
}

/* The current state as one key, or 0 when SDL cannot express it.
 * Bits: 0 blend on, 1-4 colour write mask (R G B A), 5-9 src factor,
 * 10-14 dst factor, 15-17 blend op, 30 3D pipeline, 31 valid. */
static uint32_t sg_pipeline_key(int xf)
{
    uint32_t key = SG_KEY_VALID | (xf ? SG_KEY_XF : 0);
    uint32_t mask = 0;

    if (g_color_mask & 0x00010000u) mask |= SDL_GPU_COLORCOMPONENT_R;
    if (g_color_mask & 0x00000100u) mask |= SDL_GPU_COLORCOMPONENT_G;
    if (g_color_mask & 0x00000001u) mask |= SDL_GPU_COLORCOMPONENT_B;
    if (g_color_mask & 0x01000000u) mask |= SDL_GPU_COLORCOMPONENT_A;
    key |= mask << 1;

    if (s_gpu.blend) {
        SDL_GPUBlendFactor src = sg_blend_factor(s_gpu.blend_sf);
        SDL_GPUBlendFactor dst = sg_blend_factor(s_gpu.blend_df);
        if (src == SDL_GPU_BLENDFACTOR_INVALID || dst == SDL_GPU_BLENDFACTOR_INVALID)
            return 0;
        key |= 1u | (uint32_t)src << 5 | (uint32_t)dst << 10
             | (uint32_t)sg_blend_op(s_gpu.blend_eq) << 15;
    }
    return key;
}

static SDL_GPUGraphicsPipeline *sg_pipeline_create(uint32_t key)
{
    const int xf = (key & SG_KEY_XF) != 0;
    SDL_GPUVertexBufferDescription buffer = {
        .slot = 0,
        .pitch = xf ? (Uint32)sizeof(SgXfVertex) : (Uint32)sizeof(SgVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    };
    SDL_GPUVertexAttribute flat_attrs[3] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
          .offset = offsetof(SgVertex, x) },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
          .offset = offsetof(SgVertex, color) },
        { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
          .offset = offsetof(SgVertex, uv) },
    };
    SDL_GPUVertexAttribute xf_attrs[7] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
          .offset = offsetof(SgXfVertex, pos) },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
          .offset = offsetof(SgXfVertex, col) },
        { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
          .offset = offsetof(SgXfVertex, spec) },
        { .location = 3, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
          .offset = offsetof(SgXfVertex, uv) },
        { .location = 4, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
          .offset = offsetof(SgXfVertex, uv) + 8 },
        { .location = 5, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
          .offset = offsetof(SgXfVertex, uv) + 16 },
        { .location = 6, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
          .offset = offsetof(SgXfVertex, uv) + 24 },
    };
    SDL_GPUColorTargetDescription target = {
        .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        .blend_state = {
            .enable_blend = (key & 1u) != 0,
            .src_color_blendfactor = (SDL_GPUBlendFactor)((key >> 5) & 31u),
            .dst_color_blendfactor = (SDL_GPUBlendFactor)((key >> 10) & 31u),
            .color_blend_op = (SDL_GPUBlendOp)((key >> 15) & 7u),
            .src_alpha_blendfactor = (SDL_GPUBlendFactor)((key >> 5) & 31u),
            .dst_alpha_blendfactor = (SDL_GPUBlendFactor)((key >> 10) & 31u),
            .alpha_blend_op = (SDL_GPUBlendOp)((key >> 15) & 7u),
            .color_write_mask = (SDL_GPUColorComponentFlags)((key >> 1) & 15u),
            .enable_color_write_mask = true,
        },
    };
    SDL_GPUGraphicsPipelineCreateInfo info = {
        .vertex_shader = xf ? s_sg.xf_vs : s_sg.flat2d_vs,
        .fragment_shader = xf ? s_sg.xf_fs : s_sg.flat2d_fs,
        .vertex_input_state = {
            .vertex_buffer_descriptions = &buffer, .num_vertex_buffers = 1,
            .vertex_attributes = xf ? xf_attrs : flat_attrs,
            .num_vertex_attributes = xf ? 7 : 3,
        },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE },
        .target_info = { .color_target_descriptions = &target, .num_color_targets = 1 },
    };

    /* Blend off: factors are unused but must still be valid values. */
    if (!(key & 1u))
        target.blend_state.src_color_blendfactor = target.blend_state.dst_color_blendfactor =
        target.blend_state.src_alpha_blendfactor = target.blend_state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE;
    return SDL_CreateGPUGraphicsPipeline(s_sg.dev, &info);
}

/* The pipeline for the current state; NULL when it cannot be drawn on the GPU.
 * xf: 1 for a 3D (vertex-program) batch, 0 for flat 2D. */
static SDL_GPUGraphicsPipeline *sg_pipeline_current(int xf)
{
    uint32_t key = sg_pipeline_key(xf);
    SgPipeline *p;
    int i;

    if (!key)
        return NULL;
    for (i = 0; i < s_sg.pipe_count; i++)
        if (s_sg.pipes[i].key == key)
            return s_sg.pipes[i].pipe;
    if (s_sg.pipe_count == SG_MAX_PIPELINES)
        return NULL;                                  /* more states than any title uses */
    p = &s_sg.pipes[s_sg.pipe_count];
    p->pipe = sg_pipeline_create(key);
    if (!p->pipe) {
        fprintf(stderr, "  [SDLGPU] pipeline %08X failed: %s\n", key, SDL_GetError());
        return NULL;
    }
    p->key = key;
    s_sg.pipe_count++;
    return p->pipe;
}