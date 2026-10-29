/* ── Direct3D 11 calls, on Direct3D 11 or on SDL3's GPU API (2026-10-29) ──
 *
 * Included by nv2a_gpu_d3d11.inc.c in place of COBJMACROS. Every D3D11/DXGI
 * method the renderer calls is a macro here: with the native backend it is
 * exactly the COBJMACROS call (This->lpVtbl->Method), with the SDL backend
 * (g_gx_sg) it goes to gx_sdl.c, which implements that subset of Direct3D 11
 * on SDL_GPU -- Vulkan or Direct3D 12, chosen at start-up (gx_select).
 *
 * The renderer's logic (surfaces, texture decode, combiners, presentation)
 * stays one piece of code for all three APIs; only this layer differs. */
#ifndef GX_D3D11_H
#define GX_D3D11_H

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0: native Direct3D 11. 1: SDL_GPU (Vulkan or Direct3D 12). Set once by
 * gx_D3D11CreateDevice, before any other call. */
extern int g_gx_sg;
/* "Direct3D 11", "Vulkan" or "Direct3D 12": what the renderer runs on. */
const char *gx_backend_name(void);

HRESULT gx_D3D11CreateDevice(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
                             const D3D_FEATURE_LEVEL *fls, UINT nfl, UINT sdk, ID3D11Device **dev,
                             D3D_FEATURE_LEVEL *fl, ID3D11DeviceContext **ctx);
HRESULT gx_D3DCompile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defs, ID3DInclude *inc,
                      LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **err);
#ifndef GX_IMPL                 /* gx_sdl.c calls the real ones */
#define D3D11CreateDevice gx_D3D11CreateDevice
#define D3DCompile gx_D3DCompile
#endif

/* SDL backend only (no Direct3D 11 equivalent). */
/* The next draws pull their vertices from `vb` (a vertex buffer, read as a
 * storage buffer by vs_pull) starting at `first`; tri_mode 1 rebuilds each
 * triangle's original corners in the vertex shader (the geometry shader's job
 * on Direct3D 11), w_buffer is depthp.x. */
void gxs_set_pull(ID3D11Buffer *vb, UINT first, UINT tri_mode, float w_buffer);
/* The same with two storage buffers (vertex-program draws: the raw guest
 * vertex bytes and the index list) and no automatic upload range (stride 0:
 * the caller reports what it wrote with gxs_buffer_written). */
void gxs_set_pull2(ID3D11Buffer *b0, ID3D11Buffer *b1, UINT first, UINT tri_mode, float w_buffer, UINT stride);
/* Bytes [lo, hi) of a dynamic buffer were written through Map since the last
 * draw that reads them: uploaded with the next flush. */
void gxs_buffer_written(ID3D11Buffer *buf, UINT lo, UINT hi);
/* The swap chain on the framebuffer window (claimed from its thread). NULL
 * until the window is claimed. */
IDXGISwapChain1 *gxs_swapchain(void *sdl_window, int *out_w, int *out_h);
/* Appended to a shader cache folder name so each API keeps its own
 * compiled code ("", "-vk", "-dx12"). */
const char *gx_cache_suffix(void);

/* ---- dispatch ---------------------------------------------------------- */

#define GX_CALL0(I, M, This)      (g_gx_sg ? gxs_##I##_##M((void *)(This)) : (This)->lpVtbl->M(This))
#define GX_CALL(I, M, This, ...)  (g_gx_sg ? gxs_##I##_##M((void *)(This), __VA_ARGS__) : (This)->lpVtbl->M((This), __VA_ARGS__))

ULONG gxs_obj_AddRef(void *o);
ULONG gxs_obj_Release(void *o);
HRESULT gxs_obj_QueryInterface(void *o, REFIID riid, void **out);

/* Interfaces the SDL backend never hands out: their methods cannot be
 * reached with it (QueryInterface refuses them), and fail if they were. */
#define GXS_FAIL_HR(...)   ((HRESULT)0x80004001L)        /* E_NOTIMPL */
#define GXS_FAIL_UL(...)   ((ULONG)0)
#define GXS_FAIL_VOID(...) ((void)0)
#define GXS_FAIL_U64(...)  ((UINT64)0)

/* Blob */
LPVOID gxs_ID3D10Blob_GetBufferPointer(void *b);
SIZE_T gxs_ID3D10Blob_GetBufferSize(void *b);
#define gxs_ID3D10Blob_Release gxs_obj_Release
#define ID3D10Blob_GetBufferPointer(This) GX_CALL0(ID3D10Blob, GetBufferPointer, This)
#define ID3D10Blob_GetBufferSize(This)    GX_CALL0(ID3D10Blob, GetBufferSize, This)
#define ID3D10Blob_Release(This)          GX_CALL0(ID3D10Blob, Release, This)

/* Releases and reference counts */
#define gxs_ID3D11BlendState_Release        gxs_obj_Release
#define gxs_ID3D11DepthStencilState_Release gxs_obj_Release
#define gxs_ID3D11DepthStencilView_Release  gxs_obj_Release
#define gxs_ID3D11RenderTargetView_Release  gxs_obj_Release
#define gxs_ID3D11SamplerState_Release      gxs_obj_Release
#define gxs_ID3D11ShaderResourceView_Release gxs_obj_Release
#define gxs_ID3D11ShaderResourceView_AddRef gxs_obj_AddRef
#define gxs_ID3D11Texture2D_Release         gxs_obj_Release
#define gxs_ID3D11Texture2D_QueryInterface  gxs_obj_QueryInterface
#define gxs_ID3D11VertexShader_Release      gxs_obj_Release
#define gxs_ID3D11PixelShader_Release       gxs_obj_Release
#define gxs_ID3D11GeometryShader_Release    gxs_obj_Release
#define gxs_ID3D11InputLayout_Release       gxs_obj_Release
#define gxs_ID3D11RasterizerState_Release   gxs_obj_Release
#define gxs_ID3D11Buffer_Release            gxs_obj_Release
#define gxs_ID3D11Device_Release            gxs_obj_Release
#define gxs_ID3D11DeviceContext_Release     gxs_obj_Release
#define gxs_ID3D11Device_QueryInterface     gxs_obj_QueryInterface
#define ID3D11BlendState_Release(This)         GX_CALL0(ID3D11BlendState, Release, This)
#define ID3D11DepthStencilState_Release(This)  GX_CALL0(ID3D11DepthStencilState, Release, This)
#define ID3D11DepthStencilView_Release(This)   GX_CALL0(ID3D11DepthStencilView, Release, This)
#define ID3D11RenderTargetView_Release(This)   GX_CALL0(ID3D11RenderTargetView, Release, This)
#define ID3D11SamplerState_Release(This)       GX_CALL0(ID3D11SamplerState, Release, This)
#define ID3D11ShaderResourceView_Release(This) GX_CALL0(ID3D11ShaderResourceView, Release, This)
#define ID3D11ShaderResourceView_AddRef(This)  GX_CALL0(ID3D11ShaderResourceView, AddRef, This)
#define ID3D11Texture2D_Release(This)          GX_CALL0(ID3D11Texture2D, Release, This)
#define ID3D11Texture2D_QueryInterface(This, a, b) GX_CALL(ID3D11Texture2D, QueryInterface, This, a, b)
#define ID3D11VertexShader_Release(This)       GX_CALL0(ID3D11VertexShader, Release, This)
#define ID3D11PixelShader_Release(This)        GX_CALL0(ID3D11PixelShader, Release, This)
#define ID3D11GeometryShader_Release(This)     GX_CALL0(ID3D11GeometryShader, Release, This)
#define ID3D11InputLayout_Release(This)        GX_CALL0(ID3D11InputLayout, Release, This)
#define ID3D11RasterizerState_Release(This)    GX_CALL0(ID3D11RasterizerState, Release, This)
#define ID3D11Buffer_Release(This)             GX_CALL0(ID3D11Buffer, Release, This)
#define ID3D11Device_Release(This)             GX_CALL0(ID3D11Device, Release, This)
#define ID3D11DeviceContext_Release(This)      GX_CALL0(ID3D11DeviceContext, Release, This)
#define ID3D11Device_QueryInterface(This, a, b) GX_CALL(ID3D11Device, QueryInterface, This, a, b)

void gxs_ID3D11Texture2D_GetDesc(void *t, D3D11_TEXTURE2D_DESC *d);
#define ID3D11Texture2D_GetDesc(This, a) GX_CALL(ID3D11Texture2D, GetDesc, This, a)

/* Device */
HRESULT gxs_ID3D11Device_CreateBlendState(void *d, const D3D11_BLEND_DESC *desc, ID3D11BlendState **out);
HRESULT gxs_ID3D11Device_CreateBuffer(void *d, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, ID3D11Buffer **out);
HRESULT gxs_ID3D11Device_CreateDepthStencilState(void *d, const D3D11_DEPTH_STENCIL_DESC *desc, ID3D11DepthStencilState **out);
HRESULT gxs_ID3D11Device_CreateDepthStencilView(void *d, ID3D11Resource *res, const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, ID3D11DepthStencilView **out);
HRESULT gxs_ID3D11Device_CreateGeometryShader(void *d, const void *code, SIZE_T len, ID3D11ClassLinkage *l, ID3D11GeometryShader **out);
HRESULT gxs_ID3D11Device_CreateInputLayout(void *d, const D3D11_INPUT_ELEMENT_DESC *el, UINT n, const void *code, SIZE_T len, ID3D11InputLayout **out);
HRESULT gxs_ID3D11Device_CreatePixelShader(void *d, const void *code, SIZE_T len, ID3D11ClassLinkage *l, ID3D11PixelShader **out);
HRESULT gxs_ID3D11Device_CreateRasterizerState(void *d, const D3D11_RASTERIZER_DESC *desc, ID3D11RasterizerState **out);
HRESULT gxs_ID3D11Device_CreateRenderTargetView(void *d, ID3D11Resource *res, const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView **out);
HRESULT gxs_ID3D11Device_CreateSamplerState(void *d, const D3D11_SAMPLER_DESC *desc, ID3D11SamplerState **out);
HRESULT gxs_ID3D11Device_CreateShaderResourceView(void *d, ID3D11Resource *res, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView **out);
HRESULT gxs_ID3D11Device_CreateTexture2D(void *d, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out);
HRESULT gxs_ID3D11Device_CreateVertexShader(void *d, const void *code, SIZE_T len, ID3D11ClassLinkage *l, ID3D11VertexShader **out);
D3D_FEATURE_LEVEL gxs_ID3D11Device_GetFeatureLevel(void *d);
#define ID3D11Device_CreateBlendState(This, a, b)           GX_CALL(ID3D11Device, CreateBlendState, This, a, b)
#define ID3D11Device_CreateBuffer(This, a, b, c)            GX_CALL(ID3D11Device, CreateBuffer, This, a, b, c)
#define ID3D11Device_CreateDepthStencilState(This, a, b)    GX_CALL(ID3D11Device, CreateDepthStencilState, This, a, b)
#define ID3D11Device_CreateDepthStencilView(This, a, b, c)  GX_CALL(ID3D11Device, CreateDepthStencilView, This, a, b, c)
#define ID3D11Device_CreateGeometryShader(This, a, b, c, d) GX_CALL(ID3D11Device, CreateGeometryShader, This, a, b, c, d)
#define ID3D11Device_CreateInputLayout(This, a, b, c, d, e) GX_CALL(ID3D11Device, CreateInputLayout, This, a, b, c, d, e)
#define ID3D11Device_CreatePixelShader(This, a, b, c, d)    GX_CALL(ID3D11Device, CreatePixelShader, This, a, b, c, d)
#define ID3D11Device_CreateRasterizerState(This, a, b)      GX_CALL(ID3D11Device, CreateRasterizerState, This, a, b)
#define ID3D11Device_CreateRenderTargetView(This, a, b, c)  GX_CALL(ID3D11Device, CreateRenderTargetView, This, a, b, c)
#define ID3D11Device_CreateSamplerState(This, a, b)         GX_CALL(ID3D11Device, CreateSamplerState, This, a, b)
#define ID3D11Device_CreateShaderResourceView(This, a, b, c) GX_CALL(ID3D11Device, CreateShaderResourceView, This, a, b, c)
#define ID3D11Device_CreateTexture2D(This, a, b, c)         GX_CALL(ID3D11Device, CreateTexture2D, This, a, b, c)
#define ID3D11Device_CreateVertexShader(This, a, b, c, d)   GX_CALL(ID3D11Device, CreateVertexShader, This, a, b, c, d)
#define ID3D11Device_GetFeatureLevel(This)                  GX_CALL0(ID3D11Device, GetFeatureLevel, This)

/* Device1 (shared HUD textures from the HTML host): unavailable on SDL. */
#define gxs_ID3D11Device1_OpenSharedResource1 GXS_FAIL_HR
#define gxs_ID3D11Device1_Release             GXS_FAIL_UL
#define ID3D11Device1_OpenSharedResource1(This, a, b, c) GX_CALL(ID3D11Device1, OpenSharedResource1, This, a, b, c)
#define ID3D11Device1_Release(This)                      GX_CALL0(ID3D11Device1, Release, This)

/* Context */
void gxs_ID3D11DeviceContext_ClearDepthStencilView(void *c, ID3D11DepthStencilView *v, UINT flags, FLOAT depth, UINT8 stencil);
void gxs_ID3D11DeviceContext_ClearRenderTargetView(void *c, ID3D11RenderTargetView *v, const FLOAT col[4]);
void gxs_ID3D11DeviceContext_CopyResource(void *c, ID3D11Resource *dst, ID3D11Resource *src);
void gxs_ID3D11DeviceContext_CopySubresourceRegion(void *c, ID3D11Resource *dst, UINT dsub, UINT x, UINT y, UINT z,
                                                   ID3D11Resource *src, UINT ssub, const D3D11_BOX *box);
void gxs_ID3D11DeviceContext_Draw(void *c, UINT n, UINT start);
void gxs_ID3D11DeviceContext_DrawIndexed(void *c, UINT n, UINT start, INT base);
void gxs_ID3D11DeviceContext_Flush(void *c);
void gxs_ID3D11DeviceContext_GenerateMips(void *c, ID3D11ShaderResourceView *v);
void gxs_ID3D11DeviceContext_GSSetConstantBuffers(void *c, UINT start, UINT n, ID3D11Buffer *const *b);
void gxs_ID3D11DeviceContext_GSSetShader(void *c, ID3D11GeometryShader *s, ID3D11ClassInstance *const *ci, UINT n);
void gxs_ID3D11DeviceContext_IASetIndexBuffer(void *c, ID3D11Buffer *b, DXGI_FORMAT f, UINT off);
void gxs_ID3D11DeviceContext_IASetInputLayout(void *c, ID3D11InputLayout *l);
void gxs_ID3D11DeviceContext_IASetPrimitiveTopology(void *c, D3D11_PRIMITIVE_TOPOLOGY t);
void gxs_ID3D11DeviceContext_IASetVertexBuffers(void *c, UINT start, UINT n, ID3D11Buffer *const *b, const UINT *strides, const UINT *offs);
HRESULT gxs_ID3D11DeviceContext_Map(void *c, ID3D11Resource *r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *m);
void gxs_ID3D11DeviceContext_OMSetBlendState(void *c, ID3D11BlendState *b, const FLOAT f[4], UINT mask);
void gxs_ID3D11DeviceContext_OMSetDepthStencilState(void *c, ID3D11DepthStencilState *d, UINT ref);
void gxs_ID3D11DeviceContext_OMSetRenderTargets(void *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv);
void gxs_ID3D11DeviceContext_PSSetConstantBuffers(void *c, UINT start, UINT n, ID3D11Buffer *const *b);
void gxs_ID3D11DeviceContext_PSSetSamplers(void *c, UINT start, UINT n, ID3D11SamplerState *const *s);
void gxs_ID3D11DeviceContext_PSSetShader(void *c, ID3D11PixelShader *s, ID3D11ClassInstance *const *ci, UINT n);
void gxs_ID3D11DeviceContext_PSSetShaderResources(void *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v);
void gxs_ID3D11DeviceContext_RSSetScissorRects(void *c, UINT n, const D3D11_RECT *r);
void gxs_ID3D11DeviceContext_RSSetState(void *c, ID3D11RasterizerState *s);
void gxs_ID3D11DeviceContext_RSSetViewports(void *c, UINT n, const D3D11_VIEWPORT *v);
void gxs_ID3D11DeviceContext_Unmap(void *c, ID3D11Resource *r, UINT sub);
void gxs_ID3D11DeviceContext_UpdateSubresource(void *c, ID3D11Resource *dst, UINT sub, const D3D11_BOX *box,
                                               const void *data, UINT row, UINT depth);
void gxs_ID3D11DeviceContext_VSSetConstantBuffers(void *c, UINT start, UINT n, ID3D11Buffer *const *b);
void gxs_ID3D11DeviceContext_VSSetShader(void *c, ID3D11VertexShader *s, ID3D11ClassInstance *const *ci, UINT n);
void gxs_ID3D11DeviceContext_VSSetShaderResources(void *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v);
#define ID3D11DeviceContext_ClearDepthStencilView(This, a, b, c, d) GX_CALL(ID3D11DeviceContext, ClearDepthStencilView, This, a, b, c, d)
#define ID3D11DeviceContext_ClearRenderTargetView(This, a, b)       GX_CALL(ID3D11DeviceContext, ClearRenderTargetView, This, a, b)
#define ID3D11DeviceContext_CopyResource(This, a, b)                GX_CALL(ID3D11DeviceContext, CopyResource, This, a, b)
#define ID3D11DeviceContext_CopySubresourceRegion(This, a, b, c, d, e, f, g, h) GX_CALL(ID3D11DeviceContext, CopySubresourceRegion, This, a, b, c, d, e, f, g, h)
#define ID3D11DeviceContext_Draw(This, a, b)                        GX_CALL(ID3D11DeviceContext, Draw, This, a, b)
#define ID3D11DeviceContext_DrawIndexed(This, a, b, c)              GX_CALL(ID3D11DeviceContext, DrawIndexed, This, a, b, c)
#define ID3D11DeviceContext_Flush(This)                             GX_CALL0(ID3D11DeviceContext, Flush, This)
#define ID3D11DeviceContext_GenerateMips(This, a)                   GX_CALL(ID3D11DeviceContext, GenerateMips, This, a)
#define ID3D11DeviceContext_GSSetConstantBuffers(This, a, b, c)     GX_CALL(ID3D11DeviceContext, GSSetConstantBuffers, This, a, b, c)
#define ID3D11DeviceContext_GSSetShader(This, a, b, c)              GX_CALL(ID3D11DeviceContext, GSSetShader, This, a, b, c)
#define ID3D11DeviceContext_IASetIndexBuffer(This, a, b, c)         GX_CALL(ID3D11DeviceContext, IASetIndexBuffer, This, a, b, c)
#define ID3D11DeviceContext_IASetInputLayout(This, a)               GX_CALL(ID3D11DeviceContext, IASetInputLayout, This, a)
#define ID3D11DeviceContext_IASetPrimitiveTopology(This, a)         GX_CALL(ID3D11DeviceContext, IASetPrimitiveTopology, This, a)
#define ID3D11DeviceContext_IASetVertexBuffers(This, a, b, c, d, e) GX_CALL(ID3D11DeviceContext, IASetVertexBuffers, This, a, b, c, d, e)
#define ID3D11DeviceContext_Map(This, a, b, c, d, e)                GX_CALL(ID3D11DeviceContext, Map, This, a, b, c, d, e)
#define ID3D11DeviceContext_OMSetBlendState(This, a, b, c)          GX_CALL(ID3D11DeviceContext, OMSetBlendState, This, a, b, c)
#define ID3D11DeviceContext_OMSetDepthStencilState(This, a, b)      GX_CALL(ID3D11DeviceContext, OMSetDepthStencilState, This, a, b)
#define ID3D11DeviceContext_OMSetRenderTargets(This, a, b, c)       GX_CALL(ID3D11DeviceContext, OMSetRenderTargets, This, a, b, c)
#define ID3D11DeviceContext_PSSetConstantBuffers(This, a, b, c)     GX_CALL(ID3D11DeviceContext, PSSetConstantBuffers, This, a, b, c)
#define ID3D11DeviceContext_PSSetSamplers(This, a, b, c)            GX_CALL(ID3D11DeviceContext, PSSetSamplers, This, a, b, c)
#define ID3D11DeviceContext_PSSetShader(This, a, b, c)              GX_CALL(ID3D11DeviceContext, PSSetShader, This, a, b, c)
#define ID3D11DeviceContext_PSSetShaderResources(This, a, b, c)     GX_CALL(ID3D11DeviceContext, PSSetShaderResources, This, a, b, c)
#define ID3D11DeviceContext_RSSetScissorRects(This, a, b)           GX_CALL(ID3D11DeviceContext, RSSetScissorRects, This, a, b)
#define ID3D11DeviceContext_RSSetState(This, a)                     GX_CALL(ID3D11DeviceContext, RSSetState, This, a)
#define ID3D11DeviceContext_RSSetViewports(This, a, b)              GX_CALL(ID3D11DeviceContext, RSSetViewports, This, a, b)
#define ID3D11DeviceContext_Unmap(This, a, b)                       GX_CALL(ID3D11DeviceContext, Unmap, This, a, b)
#define ID3D11DeviceContext_UpdateSubresource(This, a, b, c, d, e, f) GX_CALL(ID3D11DeviceContext, UpdateSubresource, This, a, b, c, d, e, f)
#define ID3D11DeviceContext_VSSetConstantBuffers(This, a, b, c)     GX_CALL(ID3D11DeviceContext, VSSetConstantBuffers, This, a, b, c)
#define ID3D11DeviceContext_VSSetShader(This, a, b, c)              GX_CALL(ID3D11DeviceContext, VSSetShader, This, a, b, c)
#define ID3D11DeviceContext_VSSetShaderResources(This, a, b, c)     GX_CALL(ID3D11DeviceContext, VSSetShaderResources, This, a, b, c)

/* Debug layer: none on SDL (s_gpu_iq stays NULL). */
#define gxs_ID3D11InfoQueue_ClearStoredMessages  GXS_FAIL_VOID
#define gxs_ID3D11InfoQueue_GetMessage           GXS_FAIL_HR
#define gxs_ID3D11InfoQueue_GetNumStoredMessages GXS_FAIL_U64
#define ID3D11InfoQueue_ClearStoredMessages(This)   GX_CALL0(ID3D11InfoQueue, ClearStoredMessages, This)
#define ID3D11InfoQueue_GetMessage(This, a, b, c)   GX_CALL(ID3D11InfoQueue, GetMessage, This, a, b, c)
#define ID3D11InfoQueue_GetNumStoredMessages(This)  GX_CALL0(ID3D11InfoQueue, GetNumStoredMessages, This)

/* DXGI. The SDL backend's only DXGI object is its swap chain (gxs_swapchain). */
#define gxs_IDXGIAdapter_GetParent            GXS_FAIL_HR
#define gxs_IDXGIAdapter_QueryInterface       GXS_FAIL_HR
#define gxs_IDXGIAdapter_Release              GXS_FAIL_UL
#define gxs_IDXGIAdapter3_QueryVideoMemoryInfo GXS_FAIL_HR
#define gxs_IDXGIDevice_GetAdapter            GXS_FAIL_HR
#define gxs_IDXGIDevice_Release               GXS_FAIL_UL
#define gxs_IDXGIFactory2_CreateSwapChainForHwnd GXS_FAIL_HR
#define gxs_IDXGIFactory2_MakeWindowAssociation GXS_FAIL_HR
#define gxs_IDXGIFactory2_QueryInterface      GXS_FAIL_HR
#define gxs_IDXGIFactory2_Release             GXS_FAIL_UL
#define gxs_IDXGIFactory5_CheckFeatureSupport GXS_FAIL_HR
#define gxs_IDXGIFactory5_Release             GXS_FAIL_UL
#define gxs_IDXGIKeyedMutex_AcquireSync       GXS_FAIL_HR
#define gxs_IDXGIKeyedMutex_Release           GXS_FAIL_UL
#define gxs_IDXGIKeyedMutex_ReleaseSync       GXS_FAIL_HR
#define IDXGIAdapter_GetParent(This, a, b)              GX_CALL(IDXGIAdapter, GetParent, This, a, b)
#define IDXGIAdapter_QueryInterface(This, a, b)         GX_CALL(IDXGIAdapter, QueryInterface, This, a, b)
#define IDXGIAdapter_Release(This)                      GX_CALL0(IDXGIAdapter, Release, This)
#define IDXGIAdapter3_QueryVideoMemoryInfo(This, a, b, c) GX_CALL(IDXGIAdapter3, QueryVideoMemoryInfo, This, a, b, c)
#define IDXGIDevice_GetAdapter(This, a)                 GX_CALL(IDXGIDevice, GetAdapter, This, a)
#define IDXGIDevice_Release(This)                       GX_CALL0(IDXGIDevice, Release, This)
#define IDXGIFactory2_CreateSwapChainForHwnd(This, a, b, c, d, e, f) GX_CALL(IDXGIFactory2, CreateSwapChainForHwnd, This, a, b, c, d, e, f)
#define IDXGIFactory2_MakeWindowAssociation(This, a, b) GX_CALL(IDXGIFactory2, MakeWindowAssociation, This, a, b)
#define IDXGIFactory2_QueryInterface(This, a, b)        GX_CALL(IDXGIFactory2, QueryInterface, This, a, b)
#define IDXGIFactory2_Release(This)                     GX_CALL0(IDXGIFactory2, Release, This)
#define IDXGIFactory5_CheckFeatureSupport(This, a, b, c) GX_CALL(IDXGIFactory5, CheckFeatureSupport, This, a, b, c)
#define IDXGIFactory5_Release(This)                     GX_CALL0(IDXGIFactory5, Release, This)
#define IDXGIKeyedMutex_AcquireSync(This, a, b)         GX_CALL(IDXGIKeyedMutex, AcquireSync, This, a, b)
#define IDXGIKeyedMutex_Release(This)                   GX_CALL0(IDXGIKeyedMutex, Release, This)
#define IDXGIKeyedMutex_ReleaseSync(This, a)            GX_CALL(IDXGIKeyedMutex, ReleaseSync, This, a)

HRESULT gxs_IDXGISwapChain1_GetBuffer(void *s, UINT i, REFIID riid, void **out);
HRESULT gxs_IDXGISwapChain1_Present(void *s, UINT sync, UINT flags);
HRESULT gxs_IDXGISwapChain1_ResizeBuffers(void *s, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags);
HRESULT gxs_IDXGISwapChain1_ResizeTarget(void *s, const DXGI_MODE_DESC *d);
HRESULT gxs_IDXGISwapChain1_SetFullscreenState(void *s, BOOL fs, IDXGIOutput *o);
#define gxs_IDXGISwapChain1_Release gxs_obj_Release
#define IDXGISwapChain1_GetBuffer(This, a, b, c)         GX_CALL(IDXGISwapChain1, GetBuffer, This, a, b, c)
#define IDXGISwapChain1_Present(This, a, b)              GX_CALL(IDXGISwapChain1, Present, This, a, b)
#define IDXGISwapChain1_Release(This)                    GX_CALL0(IDXGISwapChain1, Release, This)
#define IDXGISwapChain1_ResizeBuffers(This, a, b, c, d, e) GX_CALL(IDXGISwapChain1, ResizeBuffers, This, a, b, c, d, e)
#define IDXGISwapChain1_ResizeTarget(This, a)            GX_CALL(IDXGISwapChain1, ResizeTarget, This, a)
#define IDXGISwapChain1_SetFullscreenState(This, a, b)   GX_CALL(IDXGISwapChain1, SetFullscreenState, This, a, b)

#ifdef __cplusplus
}
#endif

#endif /* GX_D3D11_H */
