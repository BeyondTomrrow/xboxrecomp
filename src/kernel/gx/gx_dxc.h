/* gx_dxc.cpp: HLSL to SPIR-V or DXIL with dxcompiler.dll. */
#ifndef GX_DXC_H
#define GX_DXC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* What SDL_CreateGPUShader needs to know about a compiled shader. */
typedef struct {
    uint32_t nsamplers;     /* texture-sampler slots (highest used + 1) */
    uint32_t nstorage;      /* storage buffers */
    uint32_t nuniform;      /* uniform buffers (highest used + 1) */
    uint32_t cube_mask;     /* sampler slots that are cube maps */
} GxReflect;

int gx_dxc_available(void);
/* 1 on success: *out (malloc'd) holds the code. On failure err holds the
 * compiler's message. */
int gx_dxc_compile(const char *src, size_t len, const char *entry, const char *profile,
                   int spirv, int ieee, void **out, size_t *outlen, char *err, size_t errlen,
                   GxReflect *refl);

#ifdef __cplusplus
}
#endif
#endif
