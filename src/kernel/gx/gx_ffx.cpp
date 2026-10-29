/* ── AMD FidelityFX Super Resolution 3 on the SDL_GPU device (2026-10-29) ──
 *
 * Direct3D 12 for now. SDL_GPU does not expose its native objects; the local
 * SDL build adds SDL_GetGPUNativeD3D12 / SDL_GetGPUTextureNativeD3D12 (see
 * H:\2K5Remade-local\sdl-src, SDL_gpu_d3d12.c). FSR records into a command
 * list of its own, executed on SDL's queue right after the frame's SDL
 * submission (gxs_fsr_upscale in gx_sdl.c submits first), so queue order
 * does the synchronisation. Every texture is handed to FSR in the state SDL
 * leaves it in between commands, and FSR's backend puts it back there.
 *
 * Inputs (render resolution): colour, depth (R32 float copy, target 2 of
 * the frame) and motion vectors (target 1, UV offset to last frame's
 * position). Output: an R8G8B8A8 texture at display resolution. */
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

#include "gx_ffx.h"

extern "C" bool SDL_GetGPUNativeD3D12(SDL_GPUDevice *device, void **d3d12Device, void **commandQueue);
extern "C" void *SDL_GetGPUTextureNativeD3D12(SDL_GPUTexture *texture, int *state);

#define GXF_FRAMES 3

static struct {
    int tried, ok;
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc[GXF_FRAMES];
    UINT64 alloc_fence[GXF_FRAMES];
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 fence_value;
    HANDLE event;
    int slot;
    void *scratch[3];
    FfxFsr3Context ctx;
    int ctx_ok;
    uint32_t rw, rh, ow, oh;
    int fg;                              /* context made for frame generation */
    uint64_t frame;
} F;

static void fsr_message(FfxMsgType type, const wchar_t *msg)
{
    static int told;
    if (told++ < 20)
        fprintf(stderr, "  [FSR] %s: %ls\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

static void wait_idle(void)
{
    if (!F.fence) return;
    F.fence_value++;
    F.queue->Signal(F.fence, F.fence_value);
    if (F.fence->GetCompletedValue() < F.fence_value) {
        F.fence->SetEventOnCompletion(F.fence_value, F.event);
        WaitForSingleObject(F.event, 5000);
    }
}

extern "C" int gxf_init(SDL_GPUDevice *device)
{
    void *dev = NULL, *queue = NULL;
    int i;
    if (F.tried) return F.ok;
    F.tried = 1;
    if (!SDL_GetGPUNativeD3D12(device, &dev, &queue) || !dev || !queue) {
        fprintf(stderr, "  [FSR] needs Direct3D 12 (native objects unavailable)\n");
        return 0;
    }
    F.dev = (ID3D12Device *)dev;
    F.queue = (ID3D12CommandQueue *)queue;
    for (i = 0; i < GXF_FRAMES; i++)
        if (FAILED(F.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&F.alloc[i]))))
            return 0;
    if (FAILED(F.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, F.alloc[0], NULL, IID_PPV_ARGS(&F.list))))
        return 0;
    F.list->Close();
    if (FAILED(F.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&F.fence))))
        return 0;
    F.event = CreateEventA(NULL, FALSE, FALSE, NULL);
    F.ok = 1;
    fprintf(stderr, "  [FSR] AMD FidelityFX Super Resolution 3 ready (Direct3D 12)\n");
    return 1;
}

static int make_interface(FfxInterface *iface, int k)
{
    size_t size = ffxGetScratchMemorySizeDX12(FFX_FSR3_CONTEXT_COUNT);
    if (!F.scratch[k]) {
        F.scratch[k] = calloc(1, size);
        fprintf(stderr, "  [FSR] backend scratch %d: %.1f MB\n", k, size / 1048576.0);
    }
    if (!F.scratch[k]) return 0;
    return ffxGetInterfaceDX12(iface, ffxGetDeviceDX12(F.dev), F.scratch[k], size, FFX_FSR3_CONTEXT_COUNT) == FFX_OK;
}

/* Frame generation here is dispatched by hand (gxf_upscale), not by AMD's
 * frame-generation swap chain: the window's swap chain is SDL's, and the
 * interpolated frame is composed like a real one. The backend's swap-chain
 * hook is replaced by this no-op. */
static FfxErrorCode no_swapchain_config(FfxFrameGenerationConfig const *config)
{
    (void)config;
    return FFX_OK;
}

static FfxErrorCode fg_callback(const FfxFrameGenerationDispatchDescription *d, void *ctx)
{
    (void)ctx;
    return ffxFsr3DispatchFrameGeneration(d);
}

static int ensure_context(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int fg)
{
    FfxFsr3ContextDescription d;
    FfxErrorCode e;
    if (F.ctx_ok && F.rw == rw && F.rh == rh && F.ow == ow && F.oh == oh && F.fg == fg)
        return 1;
    if (F.ctx_ok) {
        wait_idle();
        ffxFsr3ContextDestroy(&F.ctx);
        F.ctx_ok = 0;
    }
    memset(&d, 0, sizeof d);
    d.flags = FFX_FSR3_ENABLE_AUTO_EXPOSURE | (fg ? 0u : (uint32_t)FFX_FSR3_ENABLE_UPSCALING_ONLY);
    d.maxRenderSize.width = rw; d.maxRenderSize.height = rh;
    d.maxUpscaleSize.width = ow; d.maxUpscaleSize.height = oh;
    d.displaySize.width = ow; d.displaySize.height = oh;
    d.fpMessage = fsr_message;
    d.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    if (!make_interface(&d.backendInterfaceSharedResources, 0) || !make_interface(&d.backendInterfaceUpscaling, 1) ||
        !make_interface(&d.backendInterfaceFrameInterpolation, 2))
        return 0;
    d.backendInterfaceFrameInterpolation.fpSwapChainConfigureFrameGeneration = no_swapchain_config;
    memset(&F.ctx, 0, sizeof F.ctx);
    e = ffxFsr3ContextCreate(&F.ctx, &d);
    if (e != FFX_OK) {
        fprintf(stderr, "  [FSR] context %ux%u -> %ux%u failed (%d)\n", rw, rh, ow, oh, (int)e);
        return 0;
    }
    F.ctx_ok = 1;
    F.rw = rw; F.rh = rh; F.ow = ow; F.oh = oh;
    F.fg = fg;
    F.frame = 0;
    if (fg) {
        FfxFrameGenerationConfig c;
        memset(&c, 0, sizeof c);
        c.frameGenerationEnabled = true;
        c.frameGenerationCallback = fg_callback;      /* unused here: gxf_upscale dispatches it */
        c.interpolationRect.width = (int32_t)ow; c.interpolationRect.height = (int32_t)oh;
        ffxFsr3ConfigureFrameGeneration(&F.ctx, &c);
        fprintf(stderr, "  [FSR] frame generation on (%ux%u)\n", ow, oh);
    }
    fprintf(stderr, "  [FSR] upscaling %ux%u -> %ux%u\n", rw, rh, ow, oh);
    return 1;
}

extern "C" void gxf_jitter(uint32_t rw, uint32_t ow, float *jx, float *jy)
{
    if (getenv("RECOMP_FSR_NOJITTER")) { *jx = *jy = 0.0f; return; }   /* diagnostics */
    int32_t phases = ffxFsr3GetJitterPhaseCount((int32_t)rw, (int32_t)ow);
    if (phases < 1) phases = 1;
    ffxFsr3GetJitterOffset(jx, jy, (int32_t)(F.frame % (uint64_t)phases), phases);
}

static FfxResource res(SDL_GPUTexture *t, const wchar_t *name)
{
    int state = 0;
    ID3D12Resource *r = (ID3D12Resource *)SDL_GetGPUTextureNativeD3D12(t, &state);
    FfxResourceStates st = (state & D3D12_RESOURCE_STATE_RENDER_TARGET) ? FFX_RESOURCE_STATE_RENDER_TARGET
                         : FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    if (!r) {
        FfxResource none;
        memset(&none, 0, sizeof none);
        return none;
    }
    return ffxGetResourceDX12(r, ffxGetResourceDescriptionDX12(r), name, st);
}

static int record(FfxFsr3DispatchUpscaleDescription *dd, ID3D12GraphicsCommandList *cl,
                  SDL_GPUTexture *color, SDL_GPUTexture *depth, SDL_GPUTexture *mv, SDL_GPUTexture *reactive,
                  SDL_GPUTexture *out, SDL_GPUTexture *interp, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
                  float jx, float jy, float dt_ms, int reset, float sharpness)
{
    FfxErrorCode e;
    dd->commandList = ffxGetCommandListDX12(cl);
    dd->color = res(color, L"NFL2K5_Color");
    dd->depth = res(depth, L"NFL2K5_Depth");
    dd->motionVectors = res(mv, L"NFL2K5_MotionVectors");
    if (reactive) dd->reactive = res(reactive, L"NFL2K5_Reactive");   /* HUD and text: no history */
    dd->upscaleOutput = res(out, L"NFL2K5_Upscaled");
    dd->jitterOffset.x = jx; dd->jitterOffset.y = jy;
    dd->motionVectorScale.x = (float)rw; dd->motionVectorScale.y = (float)rh;   /* ours are in UV units */
    dd->renderSize.width = rw; dd->renderSize.height = rh;
    dd->upscaleSize.width = ow; dd->upscaleSize.height = oh;
    dd->enableSharpening = sharpness > 0.0f;
    dd->sharpness = sharpness;
    dd->frameTimeDelta = dt_ms > 0.0f && dt_ms < 200.0f ? dt_ms : 16.7f;
    dd->preExposure = 1.0f;
    dd->reset = reset != 0 || F.frame == 0;
    dd->cameraNear = 0.1f; dd->cameraFar = 1000.0f;            /* the game's camera is not known here */
    dd->cameraFovAngleVertical = 0.785f;
    dd->viewSpaceToMetersFactor = 1.0f;
    dd->frameID = F.frame;
    e = ffxFsr3ContextDispatchUpscale(&F.ctx, dd);
    if (e == FFX_OK && interp && F.fg) {
        /* The frame between the previous upscaled image and this one. */
        FfxFsr3DispatchFrameGenerationPrepareDescription p;
        FfxFrameGenerationDispatchDescription g;
        memset(&p, 0, sizeof p);
        p.commandList = dd->commandList;
        p.depth = dd->depth; p.motionVectors = dd->motionVectors;
        p.jitterOffset = dd->jitterOffset; p.motionVectorScale = dd->motionVectorScale;
        p.frameTimeDelta = dd->frameTimeDelta; p.renderSize = dd->renderSize;
        p.cameraNear = dd->cameraNear; p.cameraFar = dd->cameraFar;
        p.cameraFovAngleVertical = dd->cameraFovAngleVertical; p.viewSpaceToMetersFactor = dd->viewSpaceToMetersFactor;
        p.frameID = dd->frameID;
        e = ffxFsr3ContextDispatchFrameGenerationPrepare(&F.ctx, &p);
        memset(&g, 0, sizeof g);
        g.commandList = dd->commandList;
        g.presentColor = dd->upscaleOutput;
        g.outputs[0] = res(interp, L"NFL2K5_Interpolated");
        g.numInterpolatedFrames = 1;
        g.reset = dd->reset;
        g.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        g.minMaxLuminance[0] = 0.0f; g.minMaxLuminance[1] = 1.0f;
        g.interpolationRect.width = (int32_t)ow; g.interpolationRect.height = (int32_t)oh;
        g.frameID = dd->frameID;
        if (e == FFX_OK)
            e = ffxFsr3DispatchFrameGeneration(&g);
        if (e != FFX_OK) {
            static int told;
            if (told++ < 5) fprintf(stderr, "  [FSR] frame generation failed (%d)\n", (int)e);
        }
    }
    if (e != FFX_OK) {
        static int told;
        if (told++ < 5) fprintf(stderr, "  [FSR] dispatch failed (%d)\n", (int)e);
        return 0;
    }
    return 1;
}

extern "C" int gxf_upscale(SDL_GPUTexture *color, SDL_GPUTexture *depth, SDL_GPUTexture *mv, SDL_GPUTexture *reactive,
                           SDL_GPUTexture *out, SDL_GPUTexture *interp, void *native_list,
                           uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, float jx, float jy,
                           float dt_ms, int reset, float sharpness)
{
    FfxFsr3DispatchUpscaleDescription d;
    FfxErrorCode e;
    int s;
    if (!F.ok || getenv("RECOMP_FSR_NODISPATCH") || !ensure_context(rw, rh, ow, oh, interp != NULL))   /* diagnostics */
        return 0;
    /* SDL's own command list (SDL_GetGPUCommandBufferNativeD3D12): FSR goes
     * into the frame's command buffer, no submission or wait of its own. */
    if (native_list) {
        ID3D12GraphicsCommandList *cl = (ID3D12GraphicsCommandList *)native_list;
        memset(&d, 0, sizeof d);
        if (!record(&d, cl, color, depth, mv, reactive, out, interp, rw, rh, ow, oh, jx, jy, dt_ms, reset, sharpness))
            return 0;
        F.frame++;
        return 1;
    }
    /* One frame in flight at most: the previous frame's upscale (and so all
     * of its SDL work, queue order) is finished before this one is recorded.
     * Running further ahead let SDL's per-submission upload copies pile up
     * to gigabytes of system memory (measured 2026-10-29). */
    if (F.fence_value && F.fence->GetCompletedValue() < F.fence_value) {
        F.fence->SetEventOnCompletion(F.fence_value, F.event);
        WaitForSingleObject(F.event, 5000);
    }
    s = F.slot;
    F.slot = (F.slot + 1) % GXF_FRAMES;
    if (F.fence->GetCompletedValue() < F.alloc_fence[s]) {
        F.fence->SetEventOnCompletion(F.alloc_fence[s], F.event);
        WaitForSingleObject(F.event, 5000);
    }
    F.alloc[s]->Reset();
    F.list->Reset(F.alloc[s], NULL);
    memset(&d, 0, sizeof d);
    e = record(&d, F.list, color, depth, mv, reactive, out, interp, rw, rh, ow, oh, jx, jy, dt_ms, reset, sharpness)
        ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR;
    F.list->Close();
    if (e != FFX_OK) {
        static int told;
        if (told++ < 5) fprintf(stderr, "  [FSR] dispatch failed (%d)\n", (int)e);
        return 0;
    }
    {
        ID3D12CommandList *lists[1] = { F.list };
        F.queue->ExecuteCommandLists(1, lists);
    }
    F.fence_value++;
    F.queue->Signal(F.fence, F.fence_value);
    F.alloc_fence[s] = F.fence_value;
    F.frame++;
    if (F.frame % 600 == 1) {                           /* video memory: local (VRAM) and system */
        static IDXGIAdapter3 *ad;
        if (!ad) {
            IDXGIFactory4 *fac = NULL;
            LUID luid = F.dev->GetAdapterLuid();
            if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) {
                fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad));
                fac->Release();
            }
        }
        if (ad) {
            DXGI_QUERY_VIDEO_MEMORY_INFO l, n;
            ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l);
            ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n);
            fprintf(stderr, "  [FSR] video memory: local %llu / %llu MB, system %llu MB\n",
                    (unsigned long long)(l.CurrentUsage >> 20), (unsigned long long)(l.Budget >> 20),
                    (unsigned long long)(n.CurrentUsage >> 20));
        }
    }
    /* Wait for the GPU here (measured: no frame-rate cost at 60 fps, GPU
     * ~6 ms). Without it SDL kept every upload/uniform copy of the frames in
     * flight across the extra submission per frame -- 4.7 GB of system memory
     * in a match. RECOMP_FSR_ASYNC=1 skips the wait (to work on that). */
    if (!getenv("RECOMP_FSR_ASYNC")) {
        HRESULT why;
        wait_idle();
        why = F.dev->GetDeviceRemovedReason();
        if (why != S_OK) {
            fprintf(stderr, "  [FSR] device removed after frame %llu: 0x%08lX\n", (unsigned long long)F.frame, (unsigned long)why);
            fflush(stderr);
        }
    }
    return 1;
}
