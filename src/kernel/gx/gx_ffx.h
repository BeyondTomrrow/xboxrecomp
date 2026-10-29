/* gx_ffx.cpp: AMD FidelityFX Super Resolution 3 on the SDL_GPU device. */
#ifndef GX_FFX_H
#define GX_FFX_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUTexture SDL_GPUTexture;

/* 1 when FSR can run on this device (Direct3D 12 for now). */
int gxf_init(SDL_GPUDevice *device);
/* This frame's sub-pixel camera offset, in render pixels (-0.5..0.5). */
void gxf_jitter(uint32_t rw, uint32_t ow, float *jx, float *jy);
/* Upscales colour (rw x rh) into out (ow x oh); with interp, also the frame
 * between the previous output and this one (frame generation). The caller
 * has submitted everything that renders the inputs. */
int gxf_upscale(SDL_GPUTexture *color, SDL_GPUTexture *depth, SDL_GPUTexture *mv, SDL_GPUTexture *reactive,
                SDL_GPUTexture *out, SDL_GPUTexture *interp, void *native_list,
                uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, float jx, float jy,
                float dt_ms, int reset, float sharpness);

#ifdef __cplusplus
}
#endif
#endif
