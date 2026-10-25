// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Body glows, drawn over the world.
//
// Retail lights a glowing unit by how much each point of its body turns away from the camera (a
// fresnel term): the whole body takes the colour, brightest along its silhouette.
//
//   1. wxl.gx hands every model batch the client draws. A batch of a glowing unit's model is drawn
//      again into a mask, tested against the scene's depth, with a vertex shader of ours that skins
//      the client's own vertex stream as the client's model shaders do (its bone rows from c31,
//      three a bone, already in camera space; the projection in c2-c5) and hands on the normal.
//      The mask keeps the unit's slot (red) and how much the surface faces the camera (green). A
//      vertex stream that shader cannot read falls back to the client's own vertex shader, the
//      silhouette alone, facing taken as half.
//   2. At the end of the world, one full-screen pass: inside a glowing unit, its colours by
//      (1 - facing)^power, tinting the body; just outside, a soft halo.
//
// Everything set on the device is a state the client sets itself, so invalidate_state has it send
// them again. The glow's numbers go in through a small texture, not shader constants: the client
// uploads a constant only when its own value changes, so one changed behind its back would stay.

#include "Glow.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "wxl/objects.hpp"
#include "wxl/sdk.hpp"
#include "wxl/services/gx.h"

namespace glow
{
    namespace
    {
        constexpr const char* kTag   = "retail-spellvisuals";
        constexpr uint32_t    kSlots = 6;      // glowing units drawn at once
        constexpr uint32_t    kParamTexels = 1 + kSlots * 5;
        constexpr float       kStrength = 0.7f;   // retail's numbers read strong against 3.3.5's lighting

        wxl::Service<wxl_gx_api> g_gx(WXL_GX_SERVICE, WXL_GX_SERVICE_VERSION);
        void (*g_provider)(std::vector<Active>&) = nullptr;
        uint32_t g_watch = 0;

        struct Slot
        {
            void*         root = nullptr;
            const Params* params = nullptr;
            float         strength = 0.0f;
        };
        Slot     g_slots[kSlots];
        uint32_t g_slotCount = 0;

        // What happened, logged every few seconds while anything glows: where a glow got lost.
        struct Stats
        {
            uint32_t frames = 0, actives = 0, unresolved = 0, slots = 0, batches = 0, skinned = 0, flat = 0,
                     maskFailed = 0, painted = 0, paintFailed = 0;
        } g_stats;
        ULONGLONG g_statsAt = 0;

        void FlushStats()
        {
            const ULONGLONG now = GetTickCount64();
            if (now - g_statsAt < 5000) return;
            g_statsAt = now;
            if (g_stats.actives == 0) return;
            char line[360];
            std::snprintf(line, sizeof line,
                          "glow: %u frame(s), %u glowing unit(s) asked, %u not in view, %u slot(s), %u batch(es) "
                          "matched, %u masked with normals, %u flat (%u failed), %u painted (%u failed)",
                          g_stats.frames, g_stats.actives, g_stats.unresolved, g_stats.slots, g_stats.batches,
                          g_stats.skinned, g_stats.flat, g_stats.maskFailed, g_stats.painted, g_stats.paintFailed);
            wxl::Log(WXL_LOG_INFO, kTag, line);
            g_stats = {};
        }

        template <class T>
        void Release(T*& p)
        {
            if (p) p->Release();
            p = nullptr;
        }

        IDirect3DDevice9* Device()
        {
            return g_gx ? static_cast<IDirect3DDevice9*>(g_gx->d3d9_device()) : nullptr;
        }

        // --- shaders ----------------------------------------------------------------------------

        using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                              LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        D3DCompileFn g_compile      = nullptr;
        bool         g_compileTried = false;
        bool         g_broken       = false;
        bool         g_capsKnown    = false;
        bool         g_flatProfile3 = true;  // the client's vertex shaders are vs_3_0 (else ps_2_0 for the flat mask)
        bool         g_skinnedOff   = false; // our vertex shader could not draw the client's stream

        IDirect3DVertexShader9*      g_maskVs = nullptr;
        IDirect3DPixelShader9*       g_skinPs[2][kSlots] = {};    // [alpha key][slot], after g_maskVs
        IDirect3DPixelShader9*       g_flatPs[2][2][kSlots] = {}; // [ps_3_0][alpha key][slot], after the client's
        IDirect3DPixelShader9*       g_paintPs = nullptr;
        IDirect3DVertexShader9*      g_paintVs = nullptr;
        IDirect3DVertexDeclaration9* g_paintDecl = nullptr;
        IDirect3DTexture9*           g_paramTex = nullptr;

        IDirect3DSurface9* g_maskSurface = nullptr;
        IDirect3DTexture9* g_maskTexture = nullptr;
        IDirect3DSurface9* g_maskLevel0  = nullptr;
        D3DSURFACE_DESC    g_maskDesc    = {};
        bool               g_maskCleared = false;
        bool               g_maskDrawn   = false;

        ID3DBlob* CompileBlob(const std::string& source, const char* target, bool fatal = true)
        {
            if (!g_compileTried)
            {
                g_compileTried = true;
                if (HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll"))
                    g_compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(module, "D3DCompile"));
                if (!g_compile) wxl::Log(WXL_LOG_ERROR, kTag, "d3dcompiler_47.dll is missing: no body glows");
            }
            if (!g_compile)
            {
                g_broken = true;
                return nullptr;
            }
            ID3DBlob* code   = nullptr;
            ID3DBlob* errors = nullptr;
            const HRESULT hr = g_compile(source.data(), source.size(), "glow", nullptr, nullptr, "main", target,
                                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(hr) || !code)
            {
                wxl::Log(WXL_LOG_ERROR, kTag,
                         std::string("glow shader did not compile: ") +
                             (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?"));
                Release(code);
                if (fatal) g_broken = true;
            }
            Release(errors);
            return code;
        }

        IDirect3DPixelShader9* MakePs(IDirect3DDevice9* device, const std::string& source, const char* target)
        {
            IDirect3DPixelShader9* ps = nullptr;
            if (ID3DBlob* code = CompileBlob(source, target))
            {
                device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
                code->Release();
            }
            return ps;
        }

        // The client's model vertex: position, 4 weights (UBYTE4N), 4 bone indices (UBYTE4) into
        // the batch's bone palette, normal, 2 texture coordinates. c2-c5: the projection's columns;
        // c31 on: each palette bone's three rows, camera space (CM2SceneRender::DrawBatch).
        constexpr const char* kMaskVs = R"hlsl(
float4 proj[4] : register(c2);
float4 bones[225] : register(c31);

struct In  { float3 pos : POSITION; float4 w : BLENDWEIGHT; float4 idx : BLENDINDICES; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct Out { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float3 at : TEXCOORD2; };

Out main(In i) {
    float3 p = 0, n = 0;
    float4 hp = float4(i.pos, 1.0);
    [unroll] for (int k = 0; k < 4; k++) {
        int b = (int)(i.idx[k] + 0.5) * 3;
        float4 r0 = bones[b], r1 = bones[b + 1], r2 = bones[b + 2];
        p += i.w[k] * float3(dot(r0, hp), dot(r1, hp), dot(r2, hp));
        n += i.w[k] * float3(dot(r0.xyz, i.n), dot(r1.xyz, i.n), dot(r2.xyz, i.n));
    }
    Out o;
    float4 vp = float4(p, 1.0);
    o.pos = float4(dot(proj[0], vp), dot(proj[1], vp), dot(proj[2], vp), dot(proj[3], vp));
    o.pos.z -= 0.0001 * o.pos.w;   // a hair nearer than the client's own: never behind its depth
    o.uv = i.uv;
    o.n = n;
    o.at = p;
    return o;
}
)hlsl";

        IDirect3DPixelShader9* SkinPs(IDirect3DDevice9* device, bool alphaKey, uint32_t slot)
        {
            IDirect3DPixelShader9*& ps = g_skinPs[alphaKey][slot];
            if (ps) return ps;
            char body[640];
            std::snprintf(body, sizeof body,
                          "sampler2D s0 : register(s0);\n"
                          "float4 main(float2 uv : TEXCOORD0, float3 n : TEXCOORD1, float3 at : TEXCOORD2) : COLOR {\n%s"
                          "    float facing = abs(dot(normalize(n), normalize(-at)));\n"
                          "    return float4(%.6f, facing, 0.0, 1.0);\n}\n",
                          alphaKey ? "    clip(tex2D(s0, uv).a - 0.875);\n" : "", (slot + 1) / 255.0f);
            ps = MakePs(device, body, "ps_3_0");
            return ps;
        }

        IDirect3DPixelShader9* FlatPs(IDirect3DDevice9* device, bool alphaKey, uint32_t slot)
        {
            IDirect3DPixelShader9*& ps = g_flatPs[g_flatProfile3][alphaKey][slot];
            if (ps) return ps;
            char body[320];
            std::snprintf(body, sizeof body,
                          "sampler2D s0 : register(s0);\n"
                          "float4 main(float2 uv : TEXCOORD0) : COLOR {\n%s"
                          "    return float4(%.6f, 0.5, 0.0, 1.0);\n}\n",
                          alphaKey ? "    clip(tex2D(s0, uv).a - 0.875);\n" : "", (slot + 1) / 255.0f);
            ps = MakePs(device, body, g_flatProfile3 ? "ps_3_0" : "ps_2_0");
            return ps;
        }

        constexpr const char* kPaintVs = R"hlsl(
float4 main(float4 p : POSITION) : POSITION { return p; }
)hlsl";

        // s0: the mask. s1: the numbers, one texel each: [0] (1/width, 1/height); then five a slot:
        // (C0, alpha middle) (C1, alpha edge) (C2, power) (halo colour, halo px) (halo alpha x strength, strength).
        constexpr const char* kPaintPs = R"hlsl(
sampler2D mask : register(s0);
sampler2D nums : register(s1);

#define TEXELS 31.0

float4 Num(float i) { return tex2Dlod(nums, float4((i + 0.5) / TEXELS, 0.5, 0, 0)); }
float SlotAt(float2 uv) { return floor(tex2Dlod(mask, float4(uv, 0, 0)).r * 255.0 + 0.5); }

float4 main(float2 vpos : VPOS) : COLOR {
    float2 px = Num(0).xy;
    float2 uv = (vpos + 0.5) * px;
    float4 here = tex2Dlod(mask, float4(uv, 0, 0));
    float s = floor(here.r * 255.0 + 0.5);
    if (s > 0.5) {
        float base = 1.0 + (s - 1.0) * 5.0;
        float4 a = Num(base), b = Num(base + 1), c = Num(base + 2), x = Num(base + 4);
        float f = pow(saturate(1.0 - here.g), max(c.w, 0.05));
        float3 col = f < 0.5 ? lerp(a.rgb, b.rgb, f * 2.0) : lerp(b.rgb, c.rgb, f * 2.0 - 1.0);
        float alpha = saturate(lerp(a.w, b.w, f) * x.y);
        // premultiplied: the body takes the colour (up to half of it replaced), brightest at the rim
        return float4(col * alpha, alpha * 0.5);
    }
    // outside: a soft halo from how much glowing body is within the halo's reach
    float any = 0, other = 0;
    [unroll] for (int r = 1; r <= 2; r++) {
        [unroll] for (int i = 0; i < 8; i++) {
            float ang = i * 0.785398 + r * 0.39;
            float q = SlotAt(uv + float2(cos(ang), sin(ang)) * (r * 3.5) * px);
            if (q > 0.5) { any += 1; other = q; }
        }
    }
    if (any < 0.5) return 0;
    float base = 1.0 + (other - 1.0) * 5.0;
    float4 h = Num(base + 3), x = Num(base + 4);
    float fade = smoothstep(0.0, 0.6, any / 16.0) * saturate(h.w / 6.0);
    return float4(h.rgb * x.x * fade, 0);
}
)hlsl";

        bool EnsurePaint(IDirect3DDevice9* device)
        {
            if (g_paintPs && g_paintVs && g_paintDecl && g_paramTex) return true;
            if (g_broken) return false;
            if (!g_paintVs)
                if (ID3DBlob* vs = CompileBlob(kPaintVs, "vs_3_0"))
                {
                    device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()), &g_paintVs);
                    vs->Release();
                }
            if (!g_paintPs) g_paintPs = MakePs(device, kPaintPs, "ps_3_0");
            if (!g_paintDecl)
            {
                const D3DVERTEXELEMENT9 elements[] = {
                    {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
                device->CreateVertexDeclaration(elements, &g_paintDecl);
            }
            if (!g_paramTex &&
                FAILED(device->CreateTexture(kParamTexels, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F,
                                             D3DPOOL_DEFAULT, &g_paramTex, nullptr)))
            {
                wxl::Log(WXL_LOG_WARN, kTag, "no float textures: no body glows");
                g_broken = true;
            }
            if (!g_paintPs || !g_paintVs || !g_paintDecl) g_broken = true;
            return !g_broken;
        }

        void ReleaseTargets()
        {
            Release(g_maskSurface);
            Release(g_maskLevel0);
            Release(g_maskTexture);
            Release(g_paramTex);
            g_maskDesc = {};
        }

        void ReleaseAll()
        {
            ReleaseTargets();
            for (auto& key : g_skinPs)
                for (IDirect3DPixelShader9*& ps : key) Release(ps);
            for (auto& profile : g_flatPs)
                for (auto& key : profile)
                    for (IDirect3DPixelShader9*& ps : key) Release(ps);
            Release(g_maskVs);
            Release(g_paintPs);
            Release(g_paintVs);
            Release(g_paintDecl);
        }

        bool EnsureMask(IDirect3DDevice9* device, IDirect3DSurface9* target)
        {
            D3DSURFACE_DESC desc{};
            if (FAILED(target->GetDesc(&desc))) return false;
            if (g_maskSurface && desc.Width == g_maskDesc.Width && desc.Height == g_maskDesc.Height &&
                desc.MultiSampleType == g_maskDesc.MultiSampleType &&
                desc.MultiSampleQuality == g_maskDesc.MultiSampleQuality)
                return true;
            Release(g_maskSurface);
            Release(g_maskLevel0);
            Release(g_maskTexture);
            g_maskDesc = {};
            if (FAILED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                             D3DPOOL_DEFAULT, &g_maskTexture, nullptr)) ||
                FAILED(g_maskTexture->GetSurfaceLevel(0, &g_maskLevel0)))
            {
                ReleaseTargets();
                return false;
            }
            if (desc.MultiSampleType == D3DMULTISAMPLE_NONE)
            {
                g_maskSurface = g_maskLevel0;
                g_maskSurface->AddRef();
            }
            else if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8, desc.MultiSampleType,
                                                       desc.MultiSampleQuality, FALSE, &g_maskSurface, nullptr)))
            {
                ReleaseTargets();
                return false;
            }
            g_maskDesc = desc;
            return true;
        }

        // --- 1. the mask ----------------------------------------------------------------------

        HRESULT DrawAgain(IDirect3DDevice9* device, const wxl_gx_model_draw* draw)
        {
            return device->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(draw->primitive_type), draw->base_vertex,
                                                draw->min_index, draw->vertex_count, draw->start_index,
                                                draw->primitive_count);
        }

        void WXL_CALL OnModelDraw(const wxl_gx_model_draw* draw, void*)
        {
            if (g_slotCount == 0 || g_broken || !draw || draw->blend_mode >= 2) return;
            uint32_t slot = kSlots;
            for (uint32_t i = 0; i < g_slotCount; ++i)
                if (g_slots[i].root == draw->root_model)
                {
                    slot = i;
                    break;
                }
            if (slot == kSlots) return;
            ++g_stats.batches;

            IDirect3DDevice9* device = Device();
            if (!device) return;
            if (!g_capsKnown)
            {
                g_capsKnown = true;
                D3DCAPS9 caps{};
                device->GetDeviceCaps(&caps);
                if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
                {
                    wxl::Log(WXL_LOG_WARN, kTag, "the device has no shader model 3: no body glows");
                    g_broken = true;
                    return;
                }
                if (ID3DBlob* vs = CompileBlob(kMaskVs, "vs_3_0", false))
                {
                    device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()), &g_maskVs);
                    vs->Release();
                }
                if (!g_maskVs) g_skinnedOff = true;
            }

            IDirect3DSurface9* target = nullptr;
            if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
            if (!EnsureMask(device, target))
            {
                target->Release();
                return;
            }
            const bool alphaKey = draw->blend_mode == 1;
            device->SetRenderTarget(0, g_maskSurface);
            if (!g_maskCleared)
            {
                device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
                g_maskCleared = true;
            }
            D3DVIEWPORT9 viewport{draw->viewport_x,     draw->viewport_y,     draw->viewport_width,
                                  draw->viewport_height, draw->viewport_min_z, draw->viewport_max_z};
            device->SetViewport(&viewport);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);

            HRESULT hr = E_FAIL;
            if (!g_skinnedOff)
                if (IDirect3DPixelShader9* ps = SkinPs(device, alphaKey, slot))
                {
                    device->SetVertexShader(g_maskVs);
                    device->SetPixelShader(ps);
                    hr = DrawAgain(device, draw);
                    if (SUCCEEDED(hr)) ++g_stats.skinned;
                    else
                    {
                        g_skinnedOff = true;
                        wxl::Log(WXL_LOG_WARN, kTag, "glow: the model vertex stream did not fit; silhouettes only");
                    }
                }
            if (FAILED(hr))
            {
                // the client's own vertex shader (put back by invalidate_state), the silhouette alone
                g_gx->invalidate_state();
                if (IDirect3DPixelShader9* ps = FlatPs(device, alphaKey, slot))
                {
                    device->SetPixelShader(ps);
                    hr = DrawAgain(device, draw);
                    if (FAILED(hr) && g_flatProfile3)
                    {
                        g_flatProfile3 = false; // drawn without a vs_3_0 shader: its ps_2_0 twin
                        if (IDirect3DPixelShader9* twin = FlatPs(device, alphaKey, slot))
                        {
                            device->SetPixelShader(twin);
                            hr = DrawAgain(device, draw);
                        }
                    }
                    if (SUCCEEDED(hr)) ++g_stats.flat;
                }
            }
            if (SUCCEEDED(hr)) g_maskDrawn = true;
            else ++g_stats.maskFailed;
            device->SetRenderTarget(0, target);
            target->Release();
            g_gx->invalidate_state();
        }

        // --- 2. the paint ---------------------------------------------------------------------

        bool FillParams()
        {
            D3DLOCKED_RECT locked{};
            if (FAILED(g_paramTex->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD))) return false;
            float* t = static_cast<float*>(locked.pBits);
            std::memset(t, 0, kParamTexels * 4 * sizeof(float));
            // a float texel is (r, g, b, a) in memory for A32B32G32R32F
            t[0] = 1.0f / static_cast<float>(g_maskDesc.Width);
            t[1] = 1.0f / static_cast<float>(g_maskDesc.Height);
            for (uint32_t i = 0; i < g_slotCount; ++i)
            {
                const Params& p = *g_slots[i].params;
                const float   k = g_slots[i].strength * kStrength;
                float* o = t + 4 + i * 20;
                for (int j = 0; j < 3; ++j)
                {
                    o[j]      = p.colors[0][j];
                    o[4 + j]  = p.colors[1][j];
                    o[8 + j]  = p.colors[2][j];
                    o[12 + j] = p.halo[j];
                }
                o[3]  = p.alphaMiddle;
                o[7]  = p.alphaEdge;
                o[11] = p.power;
                o[15] = std::min(p.haloPx, 8.0f);
                o[16] = p.haloAlpha * k;
                o[17] = k;
            }
            g_paramTex->UnlockRect(0);
            return true;
        }

        void PointClamp(IDirect3DDevice9* device, DWORD stage)
        {
            device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }

        void Paint()
        {
            IDirect3DDevice9* device = Device();
            if (!device || !g_maskTexture || !EnsurePaint(device) || !FillParams()) return;
            IDirect3DSurface9* target = nullptr;
            if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
            if (g_maskSurface != g_maskLevel0)
                device->StretchRect(g_maskSurface, nullptr, g_maskLevel0, nullptr, D3DTEXF_NONE);

            D3DVIEWPORT9 full{0, 0, g_maskDesc.Width, g_maskDesc.Height, 0.0f, 1.0f};
            device->SetViewport(&full);
            const float quad[4][4] = {{-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}};
            device->SetVertexDeclaration(g_paintDecl);
            device->SetVertexShader(g_paintVs);
            device->SetPixelShader(g_paintPs);
            device->SetTexture(0, g_maskTexture);
            device->SetTexture(1, g_paramTex);
            PointClamp(device, 0);
            PointClamp(device, 1);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
            device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            if (SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]))) ++g_stats.painted;
            else ++g_stats.paintFailed;
            device->SetTexture(0, nullptr);
            device->SetTexture(1, nullptr);
            target->Release();
            g_gx->invalidate_state();
        }

        class Render final : public wxl::RenderScript
        {
        public:
            void OnWorldRenderBegin(const wxl_frame_args&) override
            {
                g_maskCleared = false;
                g_maskDrawn   = false;
                g_slotCount   = 0;
                if (!g_provider || g_broken) return;
                static std::vector<Active> actives;
                actives.clear();
                g_provider(actives);
                if (!actives.empty()) ++g_stats.frames;
                g_stats.actives += static_cast<uint32_t>(actives.size());
                for (const Active& a : actives)
                {
                    if (g_slotCount == kSlots) break;
                    if (!a.params || a.strength <= 0.0f) continue;
                    const wxl::Object object = wxl::Object::Resolve(a.unit);
                    void* const root = object.HasHandle() ? g_gx->object_model(object.Handle()) : nullptr;
                    if (!root)
                    {
                        ++g_stats.unresolved;
                        continue;
                    }
                    bool taken = false;
                    for (uint32_t i = 0; i < g_slotCount; ++i) taken = taken || g_slots[i].root == root;
                    if (taken) continue; // a unit glows with its first glow
                    g_slots[g_slotCount++] = {root, a.params, std::min(a.strength, 1.0f)};
                }
                g_stats.slots += g_slotCount;
            }

            void OnWorldRenderEnd(const wxl_frame_args&) override
            {
                if (g_maskDrawn && g_slotCount) Paint();
                g_slotCount = 0;
                FlushStats();
            }

            void OnSwapchainLost(const wxl_swapchain_args&) override { ReleaseTargets(); }
        };

        Render g_render;
    }

    bool Init(void (*provider)(std::vector<Active>&))
    {
        if (!g_gx)
        {
            wxl::Log(WXL_LOG_INFO, kTag, "wxl.gx is not published: body glows are left out");
            return false;
        }
        g_provider = provider;
        if (g_gx->watch_model_draws(&OnModelDraw, nullptr, &g_watch) != WXL_OK) return false;
        wxl::ScriptMgr::Add(&g_render);
        return true;
    }

    void Shutdown()
    {
        if (g_gx && g_watch) g_gx->unwatch_model_draws(g_watch);
        g_watch = 0;
        g_slotCount = 0;
        ReleaseAll();
    }
}
