/* ── SDL3 GPU renderer: pipelines ────────────────────────────────────────
 *
 * Blend, write mask, depth and stencil state become SDL_GPU pipeline
 * state, one pipeline per distinct combination, cached (SgPipeKey). The
 * mapping mirrors the software path:
 *   blend   shade_pixel / blend_factor: the same factor for colour and
 *           alpha, unknown factors mean ONE, unknown equations ADD
 *   depth   depth_pass: GL compare functions; writes only with the test on
 *           and the depth mask set (raster_triangle_xf)
 *   stencil stencil_cmp / stencil_apply: GL functions (reference on the
 *           left) and ops, compare mask fmask, write mask wmask
 * Flat 2D batches never test depth or stencil (raster_triangle ignores
 * them), so their keys leave both off.
 *
 * Two kinds of pipeline share the cache: flat 2D (shaders/flat2d.*) and 3D
 * (shaders/xf.*), told apart by SG_COLOR_XF.
 */

/* SgPipeKey.color */
#define SG_COLOR_BLEND   (1u << 0)        /* bits 1-4: write mask R G B A */
#define SG_COLOR_XF      (1u << 30)       /* 3D pipeline: xf shaders, SgXfVertex */
#define SG_COLOR_VALID   (1u << 31)       /* bits 5-9 src, 10-14 dst, 15-17 op */
/* SgPipeKey.depth: bit 0 test, bit 1 write, bits 4-7 compare op */
/* SgPipeKey.stencil: bit 0 test, bits 4-7 compare, 8-11 fail, 12-15 zfail, 16-19 pass */
/* SgPipeKey.masks: bits 0-7 compare mask, 8-15 write mask */

/* NV2A (GL) blend factor -> SDL. INVALID when SDL has no equivalent. */
static SDL_GPUBlendFactor sg_blend_factor(uint32_t f) {
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
        case 0x8003: /* CONSTANT_ALPHA: none in SDL */
        case 0x8004: return SDL_GPU_BLENDFACTOR_INVALID;
        default: return SDL_GPU_BLENDFACTOR_ONE; /* blend_factor() returns 255 */
    }
}

static SDL_GPUBlendOp sg_blend_op(uint32_t eq) {
    switch (eq) {
        case 0x800A: return SDL_GPU_BLENDOP_SUBTRACT;
        case 0x800B: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
        case 0x8007: return SDL_GPU_BLENDOP_MIN;
        case 0x8008: return SDL_GPU_BLENDOP_MAX;
        default: return SDL_GPU_BLENDOP_ADD;
    }
}

/* GL compare function (0x0200 NEVER .. 0x0207 ALWAYS) -> SDL. The two
 * enumerations list the same eight functions in the same order. Unknown
 * values pass, as depth_pass / stencil_cmp return 1 for them. */
static SDL_GPUCompareOp sg_compare_op(uint32_t func) {
    if (func >= 0x0200 && func <= 0x0207)
        return (SDL_GPUCompareOp) (SDL_GPU_COMPAREOP_NEVER + (func - 0x0200));
    return SDL_GPU_COMPAREOP_ALWAYS;
}

/* GL stencil op -> SDL, as stencil_apply. Unknown values KEEP. */
static SDL_GPUStencilOp sg_stencil_op(uint32_t op) {
    switch (op) {
        case 0x0000: return SDL_GPU_STENCILOP_ZERO;
        case 0x1E01: return SDL_GPU_STENCILOP_REPLACE;
        case 0x1E02: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
        case 0x1E03: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
        case 0x150A: return SDL_GPU_STENCILOP_INVERT;
        case 0x8507: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
        case 0x8508: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
        default: return SDL_GPU_STENCILOP_KEEP;
    }
}

/* The current state as a key. Returns 0 when SDL cannot express it.
 * xf: 1 for a 3D (vertex-program) batch, 0 for flat 2D. */
static int sg_pipeline_key(int xf, uint32_t vs, SgPipeKey *key)
{
    uint32_t mask = 0;

    memset(key, 0, sizeof *key);
    key->color = SG_COLOR_VALID | (xf ? SG_COLOR_XF : 0);
    key->vs = vs;
    if (g_color_mask & 0x00010000u) mask |= SDL_GPU_COLORCOMPONENT_R;
    if (g_color_mask & 0x00000100u) mask |= SDL_GPU_COLORCOMPONENT_G;
    if (g_color_mask & 0x00000001u) mask |= SDL_GPU_COLORCOMPONENT_B;
    if (g_color_mask & 0x01000000u) mask |= SDL_GPU_COLORCOMPONENT_A;
    key->color |= mask << 1;

    if (s_gpu.blend) {
        SDL_GPUBlendFactor src = sg_blend_factor(s_gpu.blend_sf);
        SDL_GPUBlendFactor dst = sg_blend_factor(s_gpu.blend_df);
        if (src == SDL_GPU_BLENDFACTOR_INVALID || dst == SDL_GPU_BLENDFACTOR_INVALID)
            return 0;
        key->color |= SG_COLOR_BLEND | (uint32_t) src << 5 | (uint32_t) dst << 10
                | (uint32_t) sg_blend_op(s_gpu.blend_eq) << 15;
    }

    /* RECOMP_SDLGPU_NODEPTH=1: draw 3D with no depth test (diagnostic: does
 * a missing surface reappear?). */
    static int no_depth = -1;
    if (no_depth < 0)
        no_depth = getenv("RECOMP_SDLGPU_NODEPTH") != NULL;

    if (xf && s_vp.depth_test && !no_depth)
        key->depth = 1u | (s_vp.depth_mask ? 2u : 0u)
                     | (uint32_t) sg_compare_op(s_vp.depth_func) << 4;
    if (xf && s_vp.stencil_test) {
        key->stencil = 1u
                       | (uint32_t) sg_compare_op(s_vp.stencil_func) << 4
                       | (uint32_t) sg_stencil_op(s_vp.op_fail) << 8
                       | (uint32_t) sg_stencil_op(s_vp.op_zfail) << 12
                       | (uint32_t) sg_stencil_op(s_vp.op_zpass) << 16;
        key->masks = (s_vp.stencil_fmask & 0xFFu) | (s_vp.stencil_wmask & 0xFFu) << 8;
    }
    {
        static int cull_on = -1;
        if (cull_on < 0)
            cull_on = getenv("RECOMP_CULL") != NULL;
        if (xf && cull_on && s_vp.cull && s_vp.cull_face != 0x0408) {
            uint32_t cm = (s_vp.cull_face == 0x0404) ? 1u : 2u; /* 0404 FRONT, 0405 BACK */
            uint32_t cw = (s_vp.front_face == 0x0900) ? 1u : 0u; /* 0900 CW, 0901 CCW */
            key->color |= (cm << 18) | (cw << 20);
        }
    }
    return 1;
}

static SDL_GPUGraphicsPipeline *sg_pipeline_create(const SgPipeKey *key) {
    const int xf = (key->color & SG_COLOR_XF) != 0;
    /* A vertex-program pipeline: its generated shader, 16 vec4 inputs. */
    const int vp = key->vs != 0;
    SDL_GPUVertexAttribute vp_attrs[16];
    int k;
    for (k = 0; k < 16; k++)
        vp_attrs[k] = (SDL_GPUVertexAttribute){ .location = (Uint32)k, .buffer_slot = 0,
            .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = (Uint32)k * 16 };
    const uint32_t blend = key->color;
    SDL_GPUStencilOpState stencil = {
        .fail_op = (SDL_GPUStencilOp) ((key->stencil >> 8) & 15u),
        .depth_fail_op = (SDL_GPUStencilOp) ((key->stencil >> 12) & 15u),
        .pass_op = (SDL_GPUStencilOp) ((key->stencil >> 16) & 15u),
        .compare_op = (SDL_GPUCompareOp) ((key->stencil >> 4) & 15u),
    };
    SDL_GPUVertexBufferDescription buffer = {
        .slot = 0,
        .pitch = vp ? 256u : xf ? (Uint32)sizeof(SgXfVertex) : (Uint32)sizeof(SgVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    };
    SDL_GPUVertexAttribute flat_attrs[3] = {
        {
            .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
            .offset = offsetof(SgVertex, x)
        },
        {
            .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
            .offset = offsetof(SgVertex, color)
        },
        {
            .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
            .offset = offsetof(SgVertex, uv)
        },
    };
    SDL_GPUVertexAttribute xf_attrs[8] = {
        {
            .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
            .offset = offsetof(SgXfVertex, pos)
        },
        {
            .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
            .offset = offsetof(SgXfVertex, col)
        },
        {
            .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
            .offset = offsetof(SgXfVertex, spec)
        },
        {
            .location = 3, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
            .offset = offsetof(SgXfVertex, tc)
        },
        {
            .location = 4, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
            .offset = offsetof(SgXfVertex, tc) + 16
        },
        {
            .location = 5, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
            .offset = offsetof(SgXfVertex, tc) + 32
        },
        {
            .location = 6, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
            .offset = offsetof(SgXfVertex, tc) + 48
        },
        {
            .location = 7, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,
            .offset = offsetof(SgXfVertex, fog)
        },
    };
    SDL_GPUColorTargetDescription target = {
        .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        .blend_state = {
            .enable_blend = (blend & SG_COLOR_BLEND) != 0,
            .src_color_blendfactor = (SDL_GPUBlendFactor) ((blend >> 5) & 31u),
            .dst_color_blendfactor = (SDL_GPUBlendFactor) ((blend >> 10) & 31u),
            .color_blend_op = (SDL_GPUBlendOp) ((blend >> 15) & 7u),
            .src_alpha_blendfactor = (SDL_GPUBlendFactor) ((blend >> 5) & 31u),
            .dst_alpha_blendfactor = (SDL_GPUBlendFactor) ((blend >> 10) & 31u),
            .alpha_blend_op = (SDL_GPUBlendOp) ((blend >> 15) & 7u),
            .color_write_mask = (SDL_GPUColorComponentFlags) ((blend >> 1) & 15u),
            .enable_color_write_mask = true,
        },

    };
    SDL_GPUGraphicsPipelineCreateInfo info = {
        .vertex_shader = vp ? s_sg_vp[key->vs - 1].vs : xf ? s_sg.xf_vs : s_sg.flat2d_vs,
        .fragment_shader = xf ? s_sg.xf_fs : s_sg.flat2d_fs,
        .vertex_input_state = {
            .vertex_buffer_descriptions = &buffer, .num_vertex_buffers = 1,
            .vertex_attributes = vp ? vp_attrs : xf ? xf_attrs : flat_attrs,
            .num_vertex_attributes = vp ? 16 : xf ? 8 : 3,
        },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {
            .fill_mode = SDL_GPU_FILLMODE_FILL,
            .cull_mode = ((blend >> 18) & 3u) == 1u ? SDL_GPU_CULLMODE_FRONT :
                         ((blend >> 18) & 3u) == 2u ? SDL_GPU_CULLMODE_BACK :
                                                      SDL_GPU_CULLMODE_NONE,
            /* In y-down screen coordinates with Vulkan/SDL_GPU clip space,
             * NV2A's 0x0900 (CW in y-up) maps to COUNTER_CLOCKWISE in y-down,
             * matching sg_collect_xf's `int cw = area < 0` check. */
            .front_face = ((blend >> 20) & 1u)
                              ? SDL_GPU_FRONTFACE_CLOCKWISE
                              : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
            .enable_depth_clip = false
        },
        .depth_stencil_state = {
            .enable_depth_test = (key->depth & 1u) != 0,
            .enable_depth_write = (key->depth & 2u) != 0,
            .compare_op = (key->depth & 1u)
                              ? (SDL_GPUCompareOp) ((key->depth >> 4) & 15u)
                              : SDL_GPU_COMPAREOP_ALWAYS,
            .enable_stencil_test = (key->stencil & 1u) != 0,
            .front_stencil_state = stencil,
            .back_stencil_state = stencil, /* no culling: both faces alike */
            .compare_mask = (Uint8) (key->masks & 0xFFu),
            .write_mask = (Uint8) ((key->masks >> 8) & 0xFFu),
        },
        .target_info = {
            .color_target_descriptions = &target, .num_color_targets = 1,
            /* Every pass binds the surface's depth-stencil texture, so every
             * pipeline declares it, tests on or off. */
            .depth_stencil_format = s_sg.depth_format,
            .has_depth_stencil_target = true,
        },
    };

    /* Unused states must still hold valid values. */
    if (!(blend & SG_COLOR_BLEND))
        target.blend_state.src_color_blendfactor = target.blend_state.dst_color_blendfactor =
                                                   target.blend_state.src_alpha_blendfactor =
                                                   target.blend_state.dst_alpha_blendfactor =
                                                   SDL_GPU_BLENDFACTOR_ONE;
    if (!(key->stencil & 1u)) {
        SDL_GPUStencilOpState keep = {
            SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_KEEP,
            SDL_GPU_COMPAREOP_ALWAYS,
        };
        info.depth_stencil_state.front_stencil_state = keep;
        info.depth_stencil_state.back_stencil_state = keep;
    }
    return SDL_CreateGPUGraphicsPipeline(s_sg.dev, &info);
}

/* The pipeline for the current state; NULL when it cannot be drawn on the GPU. */
static SDL_GPUGraphicsPipeline *sg_pipeline_current(int xf, uint32_t vs) {
    SgPipeKey key;
    SgPipeline *p;
    int i;

    if (!sg_pipeline_key(xf, vs, &key))
        return NULL;
    for (i = 0; i < s_sg.pipe_count; i++)
        if (memcmp(&s_sg.pipes[i].key, &key, sizeof key) == 0)
            return s_sg.pipes[i].pipe;
    if (s_sg.pipe_count == SG_MAX_PIPELINES)
        return NULL; /* more states than any title uses */
    p = &s_sg.pipes[s_sg.pipe_count];
    p->pipe = sg_pipeline_create(&key);
    if (!p->pipe) {
        fprintf(stderr, "  [SDLGPU] pipeline %08X/%08X/%08X failed: %s\n",
                key.color, key.depth, key.stencil, SDL_GetError());
        return NULL;
    }
    p->key = key;
    s_sg.pipe_count++;
    return p->pipe;
}
