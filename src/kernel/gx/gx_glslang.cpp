/* ── HLSL to SPIR-V at run time with glslang (Android / Linux) ────────────
 *
 * The same contract as gx_dxc.cpp (gx_dxc.h), for hosts without
 * dxcompiler: glslang's HLSL front end, Vulkan 1.0 SPIR-V. Two differences
 * from DXC, both handled here:
 *  - [[vk::combinedImageSampler]] is not understood. The renderer always
 *    pairs texture k with sampler k, so textures become sampled images and
 *    the samplers go (glslang's "auto sampled textures").
 *  - Resources a shader declares but never uses stay in the module (DXC
 *    drops them). The counts SDL_GPU is given cover only what the entry
 *    point uses, as DXC's do; unused declarations need no binding.
 * Checked against the shaders a Direct3D 12 run compiles: all compile and
 * give DXC's descriptor layout (android/README.md). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include "gx_dxc.h"

namespace {

std::once_flag s_init;

/* Descriptor counts over the variables the module's code refers to. */
void reflect_used(const std::vector<uint32_t> &w, GxReflect *r)
{
    enum { TypeImage = 25, TypeSampledImage = 27, TypeStruct = 30, TypePointer = 32, Function = 54,
           Variable = 59, Decorate = 71 };
    struct Id { uint32_t op = 0, a = 0, b = 0; int set = -1, binding = -1, block = 0, bufblock = 0, used = 0; };
    size_t nw = w.size(), i = 5;
    uint32_t bound = nw > 3 ? w[3] : 0;
    std::vector<Id> ids(bound + 1);
    bool in_code = false;
    while (i < nw) {
        uint32_t op = w[i] & 0xFFFF, len = w[i] >> 16;
        if (!len || i + len > nw) break;
        const uint32_t *o = &w[i + 1];
        if (op == Function) in_code = true;
        if (in_code) {
            for (uint32_t k = 0; k + 1 < len; k++)       /* any operand naming a variable */
                if (o[k] <= bound && ids[o[k]].op == Variable) ids[o[k]].used = 1;
        } else switch (op) {
        case TypeImage:        if (o[0] <= bound) { ids[o[0]].op = op; ids[o[0]].a = o[2]; } break;
        case TypeSampledImage: if (o[0] <= bound) { ids[o[0]].op = op; ids[o[0]].a = o[1]; } break;
        case TypeStruct:       if (o[0] <= bound) ids[o[0]].op = op; break;
        case TypePointer:      if (o[0] <= bound) { ids[o[0]].op = op; ids[o[0]].a = o[1]; ids[o[0]].b = o[2]; } break;
        case Variable:         if (o[1] <= bound) { ids[o[1]].op = op; ids[o[1]].a = o[0]; ids[o[1]].b = o[2]; } break;
        case Decorate:
            if (o[0] <= bound) {
                if (o[1] == 33) ids[o[0]].binding = (int)o[2];
                else if (o[1] == 34) ids[o[0]].set = (int)o[2];
                else if (o[1] == 2) ids[o[0]].block = 1;
                else if (o[1] == 3) ids[o[0]].bufblock = 1;
            }
            break;
        }
        i += len;
    }
    for (uint32_t v = 1; v <= bound; v++) {
        const Id &var = ids[v];
        if (var.op != Variable || !var.used || var.set < 0 || var.binding < 0 || var.a > bound) continue;
        const Id &ptr = ids[var.a];
        if (ptr.op != TypePointer || ptr.b > bound) continue;
        const Id &ty = ids[ptr.b];
        uint32_t b = (uint32_t)var.binding;
        if ((var.set == 0 || var.set == 2) && ty.op == TypeSampledImage) {
            if (b + 1 > r->nsamplers) r->nsamplers = b + 1;
            if (ty.a <= bound && ids[ty.a].op == TypeImage && ids[ty.a].a == 3 && b < 32) r->cube_mask |= 1u << b;
        } else if ((var.set == 1 || var.set == 3) && ty.op == TypeStruct && ty.block && var.b == 2) {
            if (b + 1 > r->nuniform) r->nuniform = b + 1;
        } else if ((var.set == 0 || var.set == 2) && ty.op == TypeStruct &&
                   ((var.b == 2 && ty.bufblock) || var.b == 12)) {
            r->nstorage++;
        }
    }
}

} // namespace

extern "C" int gx_dxc_available(void) { return 1; }

extern "C" int gx_dxc_compile(const char *src, size_t len, const char *entry, const char *profile,
                              int spirv, int ieee, void **out, size_t *outlen, char *err, size_t errlen,
                              GxReflect *refl)
{
    (void)ieee;
    *out = nullptr;
    *outlen = 0;
    if (err && errlen) err[0] = 0;
    memset(refl, 0, sizeof *refl);
    if (!spirv) {
        if (err) snprintf(err, errlen, "glslang: SPIR-V only");
        return 0;
    }
    std::call_once(s_init, [] { glslang::InitializeProcess(); });
    EShLanguage stage = profile && profile[0] == 'v' ? EShLangVertex : EShLangFragment;
    glslang::TShader sh(stage);
    const char *text = src;
    int text_len = (int)len;
    sh.setStringsWithLengths(&text, &text_len, 1);
    sh.setEntryPoint(entry);
    sh.setSourceEntryPoint(entry);
    sh.setPreamble("#define GX_SDL 1\n#define GX_SPIRV 1\n#define GX_GLSLANG 1\n");
    sh.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    sh.setTextureSamplerTransformMode(EShTexSampTransUpgradeTextureRemoveSampler);
    EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, msgs)) {
        if (err) snprintf(err, errlen, "%s", sh.getInfoLog());
        return 0;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs)) {
        if (err) snprintf(err, errlen, "%s", prog.getInfoLog());
        return 0;
    }
    std::vector<uint32_t> spv;
    glslang::SpvOptions opt;
    opt.disableOptimizer = true;
    glslang::GlslangToSpv(*prog.getIntermediate(stage), spv, &opt);
    if (spv.empty()) {
        if (err) snprintf(err, errlen, "glslang: no SPIR-V");
        return 0;
    }
    reflect_used(spv, refl);
    *outlen = spv.size() * 4;
    *out = malloc(*outlen);
    if (!*out) return 0;
    memcpy(*out, spv.data(), *outlen);
    return 1;
}
