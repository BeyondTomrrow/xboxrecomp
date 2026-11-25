/* ── SDL3 GPU renderer: SPIR-V -> Metal Shading Language ──────────────────
 *
 * SDL's Metal backend takes MSL source, not SPIR-V. Rather than keep a
 * second copy of every shader, the SPIR-V the renderer already has (the
 * build-time fixed shaders and the glslang output for vertex programs) is
 * cross-compiled with SPIRV-Cross when the device wants MSL.
 *
 * Bindings follow SDL's Metal convention (SDL_CreateGPUShader docs): per
 * stage, uniform buffers at [[buffer(0..)]], sampled textures at
 * [[texture(0..)]] with their samplers at the same [[sampler(i)]]. The
 * SPIR-V side uses SDL's sets: vertex uniforms set 1, fragment textures set 2,
 * fragment uniforms set 3. SPIRV-Cross renames the entry point main0.
 *
 * Thread-safe: each call has its own SPIRV-Cross context, so the
 * vertex-program worker converts off the draw thread.
 */

#define SG_MSL_ENTRY "main0"

#ifdef SG_HAVE_SPIRV_CROSS
#include <spirv_cross/spirv_cross_c.h>

static void sg_msl_bind(spvc_compiler comp, SpvExecutionModel model,
                        unsigned set, unsigned binding, unsigned slot)
{
    spvc_msl_resource_binding b;
    spvc_msl_resource_binding_init(&b);
    b.stage = model;
    b.desc_set = set;
    b.binding = binding;
    b.msl_buffer = slot;
    b.msl_texture = slot;
    b.msl_sampler = slot;
    spvc_compiler_msl_add_resource_binding(comp, &b);
}

/* SPIR-V -> malloc'd NUL-terminated MSL, or NULL after logging why. */
static char *sg_spirv_to_msl(const uint32_t *words, size_t count, int vertex)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler comp = NULL;
    spvc_compiler_options opts = NULL;
    const char *src = NULL;
    char *out = NULL;
    SpvExecutionModel model = vertex ? SpvExecutionModelVertex : SpvExecutionModelFragment;
    unsigned i;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS)
        return NULL;
    if (spvc_context_parse_spirv(ctx, (const SpvId *)words, count, &ir) != SPVC_SUCCESS ||
        spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir,
                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS ||
        spvc_compiler_create_compiler_options(comp, &opts) != SPVC_SUCCESS)
        goto fail;

    spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_PLATFORM,
                                   SPVC_MSL_PLATFORM_MACOS);
    spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION,
                                   SPVC_MAKE_MSL_VERSION(2, 1, 0));
    if (spvc_compiler_install_compiler_options(comp, opts) != SPVC_SUCCESS)
        goto fail;

    if (vertex) {
        sg_msl_bind(comp, model, 1, 0, 0);                   /* uniforms */
    } else {
        for (i = 0; i < 8; i++)
            sg_msl_bind(comp, model, 2, i, i);               /* textures: 2D 0..3, cube 4..7 */
        sg_msl_bind(comp, model, 3, 0, 0);                   /* uniforms */
    }

    if (spvc_compiler_compile(comp, &src) != SPVC_SUCCESS || !src)
        goto fail;
    out = strdup(src);
    spvc_context_destroy(ctx);
    if (getenv("RECOMP_SDLGPU_MSLDUMP"))
        fprintf(stderr, "  [SDLGPU] MSL %s shader:\n%s\n", vertex ? "vertex" : "fragment", out);
    return out;

fail:
    fprintf(stderr, "  [SDLGPU] SPIR-V -> MSL failed: %s\n",
            ctx ? spvc_context_get_last_error_string(ctx) : "no context");
    spvc_context_destroy(ctx);
    return NULL;
}
#endif /* SG_HAVE_SPIRV_CROSS */

/* The device's shader format: SPIR-V where it takes it (Vulkan), MSL on
 * Metal when SPIRV-Cross is built in. 0 = none usable. */
static SDL_GPUShaderFormat sg_pick_shader_format(SDL_GPUDevice *dev)
{
    SDL_GPUShaderFormat have = SDL_GetGPUShaderFormats(dev);
    if (have & SDL_GPU_SHADERFORMAT_SPIRV)
        return SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef SG_HAVE_SPIRV_CROSS
    if (have & SDL_GPU_SHADERFORMAT_MSL)
        return SDL_GPU_SHADERFORMAT_MSL;
#endif
    return 0;
}

/* Create a shader from SPIR-V in whatever format the device takes. */
static SDL_GPUShader *sg_shader_from_spirv(const uint32_t *words, size_t bytes,
                                           SDL_GPUShaderStage stage,
                                           Uint32 samplers, Uint32 uniform_buffers)
{
    SDL_GPUShaderCreateInfo info = {
        .code = (const Uint8 *)words, .code_size = bytes, .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = stage,
        .num_samplers = samplers, .num_uniform_buffers = uniform_buffers,
    };
#ifdef SG_HAVE_SPIRV_CROSS
    if (s_sg.shader_format == SDL_GPU_SHADERFORMAT_MSL) {
        char *msl = sg_spirv_to_msl(words, bytes / sizeof *words,
                                    stage == SDL_GPU_SHADERSTAGE_VERTEX);
        SDL_GPUShader *sh;
        if (!msl)
            return NULL;
        info.code = (const Uint8 *)msl;
        info.code_size = strlen(msl);
        info.entrypoint = SG_MSL_ENTRY;
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        sh = SDL_CreateGPUShader(s_sg.dev, &info);
        free(msl);
        return sh;
    }
#endif
    return SDL_CreateGPUShader(s_sg.dev, &info);
}

/* Same, for MSL the vertex-program worker already converted. */
static SDL_GPUShader *sg_shader_from_msl(const char *msl, SDL_GPUShaderStage stage,
                                         Uint32 samplers, Uint32 uniform_buffers)
{
    SDL_GPUShaderCreateInfo info = {
        .code = (const Uint8 *)msl, .code_size = strlen(msl), .entrypoint = SG_MSL_ENTRY,
        .format = SDL_GPU_SHADERFORMAT_MSL, .stage = stage,
        .num_samplers = samplers, .num_uniform_buffers = uniform_buffers,
    };
    return SDL_CreateGPUShader(s_sg.dev, &info);
}
