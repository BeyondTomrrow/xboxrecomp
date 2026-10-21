/* ── SDL3 GPU renderer: runtime shader compilation ──────────────────────
 *
 * NV2A vertex programs are translated to GLSL at run time (one shader per
 * distinct program, sdlgpu_vp.inc.c) and compiled here to SPIR-V with
 * glslang's C interface. The fixed shaders (flat2d, xf) are still compiled
 * at build time; only generated code comes through here.
 *
 * Synchronous core. The vertex-program cache calls it from a worker
 * thread, so the game thread never waits for a compile.
 */

#ifdef SG_HAVE_GLSLANG
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

/* Once per process, before the first compile (glslang keeps global state). */
static void sg_compiler_init(void)
{
    static int done;
    if (!done) {
        glslang_initialize_process();
        done = 1;
    }
}

/* GLSL source -> SPIR-V words, malloc'd; *words_out receives the count.
 * NULL on failure, after printing the compiler's log (once per failing
 * shader: a program that fails is cached as failed and not retried). */
static uint32_t *sg_glsl_to_spirv(const char *source, int vertex, size_t *words_out)
{
    glslang_input_t input = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = vertex ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = GLSLANG_TARGET_VULKAN_1_0,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = GLSLANG_TARGET_SPV_1_0,
        .code = source,
        .default_version = 450,
        .default_profile = GLSLANG_NO_PROFILE,
        .force_default_version_and_profile = false,
        .forward_compatible = false,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = glslang_default_resource(),
    };
    glslang_shader_t *shader;
    glslang_program_t *program;
    uint32_t *words = NULL;

    shader = glslang_shader_create(&input);
    if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input)) {
        fprintf(stderr, "  [SDLGPU] generated shader failed to compile:\n%s\n",
                glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return NULL;
    }
    program = glslang_program_create();
    glslang_program_add_shader(program, shader);
    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        fprintf(stderr, "  [SDLGPU] generated shader failed to link:\n%s\n",
                glslang_program_get_info_log(program));
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }
    glslang_program_SPIRV_generate(program, input.stage);
    *words_out = glslang_program_SPIRV_get_size(program);
    words = (uint32_t *)malloc(*words_out * sizeof *words);
    if (words)
        glslang_program_SPIRV_get(program, words);
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return words;
}
#endif /* SG_HAVE_GLSLANG */