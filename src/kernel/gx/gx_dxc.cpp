/* ── HLSL to SPIR-V / DXIL at run time, through dxcompiler.dll ───────────
 *
 * The SDL backend (gx_sdl.c) compiles the renderer's HLSL -- the same source
 * Direct3D 11 compiles with D3DCompile -- for Vulkan (SPIR-V) or Direct3D 12
 * (DXIL). Run time, not build time, because pixel shaders are specialised per
 * combiner state while the game runs (nv2a_gpu_ps.inc.c).
 *
 * dxcompiler.dll and dxil.dll (the signer DXIL needs) ship next to the exe;
 * without them the SDL backend is unavailable and Direct3D 11 is used.
 *
 * Besides the code, each compile reports the resource counts SDL_GPU needs
 * when the shader is created (samplers, storage buffers, uniform buffers),
 * read from the compiled shader itself. */
#include <windows.h>
#include <unknwn.h>      /* the build uses WIN32_LEAN_AND_MEAN: COM base types by hand */
#include <objidl.h>
#include <oleauto.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>

#include "../../../third_party/dxc/dxcapi.h"
#include <initguid.h>
#include "../../../third_party/dxc/d3d12shader.h"

#include "gx_dxc.h"

static DxcCreateInstanceProc s_create;
static int s_tried;

static int dxc_load(void)
{
    if (s_tried)
        return s_create != nullptr;
    s_tried = 1;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    path[n] = 0;
    std::wstring dll = std::wstring(path) + L"dxcompiler.dll";
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        fprintf(stderr, "  [GX] dxcompiler.dll not found next to the exe; Vulkan/Direct3D 12 unavailable\n");
        return 0;
    }
    s_create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
    return s_create != nullptr;
}

extern "C" int gx_dxc_available(void)
{
    return dxc_load();
}

/* One compiler per thread: DXC instances are not shared between threads, and
 * pixel-shader variants compile on worker threads. */
static thread_local IDxcCompiler3 *tl_compiler;
static thread_local IDxcUtils *tl_utils;

/* ---- SPIR-V reflection: just the descriptor counts SDL needs ------------ */

enum { SpvOpTypeImage = 25, SpvOpTypeSampledImage = 27, SpvOpTypeStruct = 30, SpvOpTypePointer = 32,
       SpvOpVariable = 59, SpvOpDecorate = 71 };
enum { SpvDecBlock = 2, SpvDecBufferBlock = 3, SpvDecBinding = 33, SpvDecSet = 34 };
enum { SpvScUniformConstant = 0, SpvScUniform = 2, SpvScStorageBuffer = 12 };

static void spirv_reflect(const uint32_t *w, size_t nw, GxReflect *r)
{
    struct Id { uint32_t op = 0, a = 0, b = 0, c = 0; int set = -1, binding = -1, block = 0, bufblock = 0; };
    uint32_t bound = nw > 3 ? w[3] : 0;
    std::vector<Id> ids(bound + 1);
    size_t i = 5;
    while (i < nw) {
        uint32_t op = w[i] & 0xFFFF, len = w[i] >> 16;
        if (!len || i + len > nw) break;
        const uint32_t *o = w + i + 1;
        switch (op) {
        case SpvOpTypeImage:        if (o[0] <= bound) { ids[o[0]].op = op; ids[o[0]].a = o[2]; } break;   /* a: Dim */
        case SpvOpTypeSampledImage: if (o[0] <= bound) { ids[o[0]].op = op; ids[o[0]].a = o[1]; } break;   /* a: image */
        case SpvOpTypeStruct:       if (o[0] <= bound) ids[o[0]].op = op; break;
        case SpvOpTypePointer:      if (o[0] <= bound) { ids[o[0]].op = op; ids[o[0]].a = o[1]; ids[o[0]].b = o[2]; } break;
        case SpvOpVariable:         if (o[1] <= bound) { ids[o[1]].op = op; ids[o[1]].a = o[0]; ids[o[1]].b = o[2]; } break;
        case SpvOpDecorate:
            if (o[0] <= bound) {
                if (o[1] == SpvDecBinding) ids[o[0]].binding = (int)o[2];
                else if (o[1] == SpvDecSet) ids[o[0]].set = (int)o[2];
                else if (o[1] == SpvDecBlock) ids[o[0]].block = 1;
                else if (o[1] == SpvDecBufferBlock) ids[o[0]].bufblock = 1;
            }
            break;
        }
        i += len;
    }
    for (uint32_t v = 1; v <= bound; v++) {
        const Id &var = ids[v];
        if (var.op != SpvOpVariable || var.set < 0 || var.binding < 0 || var.a > bound) continue;
        const Id &ptr = ids[var.a];
        if (ptr.op != SpvOpTypePointer || ptr.b > bound) continue;
        const Id &ty = ids[ptr.b];
        uint32_t b = (uint32_t)var.binding;
        if ((var.set == 0 || var.set == 2) && ty.op == SpvOpTypeSampledImage) {
            if (b + 1 > r->nsamplers) r->nsamplers = b + 1;
            if (ty.a <= bound && ids[ty.a].op == SpvOpTypeImage && ids[ty.a].a == 3 /* Cube */ && b < 32)
                r->cube_mask |= 1u << b;
        } else if ((var.set == 1 || var.set == 3) && ty.op == SpvOpTypeStruct && ty.block && var.b == SpvScUniform) {
            if (b + 1 > r->nuniform) r->nuniform = b + 1;
        } else if ((var.set == 0 || var.set == 2) && ty.op == SpvOpTypeStruct &&
                   ((var.b == SpvScUniform && ty.bufblock) || var.b == SpvScStorageBuffer)) {
            r->nstorage++;
        }
    }
}

/* ---- DXIL reflection -------------------------------------------------- */

static void dxil_reflect(IDxcResult *res, GxReflect *r)
{
    IDxcBlob *rb = nullptr;
    ID3D12ShaderReflection *refl = nullptr;
    if (FAILED(res->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&rb), nullptr)) || !rb)
        return;
    DxcBuffer buf = { rb->GetBufferPointer(), rb->GetBufferSize(), 0 };
    if (SUCCEEDED(tl_utils->CreateReflection(&buf, IID_ID3D12ShaderReflection, (void **)&refl)) && refl) {
        D3D12_SHADER_DESC sd;
        refl->GetDesc(&sd);
        for (UINT k = 0; k < sd.BoundResources; k++) {
            D3D12_SHADER_INPUT_BIND_DESC b;
            if (FAILED(refl->GetResourceBindingDesc(k, &b))) continue;
            switch (b.Type) {
            case D3D_SIT_CBUFFER:
                if (b.BindPoint + 1 > r->nuniform) r->nuniform = b.BindPoint + 1;
                break;
            case D3D_SIT_TEXTURE:
                if (b.BindPoint + 1 > r->nsamplers) r->nsamplers = b.BindPoint + 1;
                if (b.Dimension == D3D_SRV_DIMENSION_TEXTURECUBE && b.BindPoint < 32) r->cube_mask |= 1u << b.BindPoint;
                break;
            case D3D_SIT_SAMPLER:
                if (b.BindPoint + 1 > r->nsamplers) r->nsamplers = b.BindPoint + 1;
                break;
            case D3D_SIT_STRUCTURED:
            case D3D_SIT_BYTEADDRESS:
                r->nstorage++;
                break;
            default:
                break;
            }
        }
        refl->Release();
    }
    rb->Release();
}

static std::wstring widen(const char *s)
{
    std::wstring w;
    while (s && *s) w.push_back((wchar_t)(unsigned char)*s++);
    return w;
}

extern "C" int gx_dxc_compile(const char *src, size_t len, const char *entry, const char *profile,
                              int spirv, int ieee, void **out, size_t *outlen, char *err, size_t errlen,
                              GxReflect *refl)
{
    *out = nullptr;
    *outlen = 0;
    if (err && errlen) err[0] = 0;
    memset(refl, 0, sizeof *refl);
    if (!dxc_load())
        return 0;
    if (!tl_compiler) {
        if (FAILED(s_create(CLSID_DxcCompiler, IID_PPV_ARGS(&tl_compiler))) ||
            FAILED(s_create(CLSID_DxcUtils, IID_PPV_ARGS(&tl_utils)))) {
            if (err) snprintf(err, errlen, "DxcCreateInstance failed");
            return 0;
        }
    }
    std::wstring we = widen(entry), wp = widen(profile);
    std::vector<LPCWSTR> args = { L"-E", we.c_str(), L"-T", wp.c_str(), L"-O3", L"-HV", L"2018",
                                  L"-D", L"GX_SDL=1" };
    if (ieee) args.push_back(L"-Gis");
    if (spirv) {
        args.push_back(L"-spirv");
        args.push_back(L"-fspv-target-env=vulkan1.0");
        args.push_back(L"-fvk-use-dx-layout");
        args.push_back(L"-D");
        args.push_back(L"GX_SPIRV=1");
    }
    DxcBuffer sb = { src, len, DXC_CP_UTF8 };
    IDxcResult *res = nullptr;
    if (FAILED(tl_compiler->Compile(&sb, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&res))) || !res) {
        if (err) snprintf(err, errlen, "Compile call failed");
        return 0;
    }
    HRESULT st = E_FAIL;
    res->GetStatus(&st);
    if (FAILED(st)) {
        IDxcBlobUtf8 *e = nullptr;
        if (SUCCEEDED(res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&e), nullptr)) && e) {
            if (err) snprintf(err, errlen, "%.*s", (int)e->GetStringLength(), e->GetStringPointer());
            e->Release();
        }
        res->Release();
        return 0;
    }
    IDxcBlob *code = nullptr;
    if (FAILED(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr)) || !code) {
        res->Release();
        return 0;
    }
    *outlen = code->GetBufferSize();
    *out = malloc(*outlen ? *outlen : 1);
    if (*out) memcpy(*out, code->GetBufferPointer(), *outlen);
    if (spirv)
        spirv_reflect((const uint32_t *)code->GetBufferPointer(), code->GetBufferSize() / 4, refl);
    else
        dxil_reflect(res, refl);
    code->Release();
    res->Release();
    return *out != nullptr;
}
