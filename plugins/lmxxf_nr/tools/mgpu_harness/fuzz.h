// A fake upscaler and HUD-less source for the harness (the plugin's test hooks stand in for OptiScaler's upscale
// callbacks and HUD-less pick), and the seeded switch fuzzer. See main.cpp's header for the options.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "../../../../src/plugin_sdk/optiscaler_plugin.h"

namespace hx
{
using Microsoft::WRL::ComPtr;

// splitmix64: the whole switch sequence follows from the seed
struct Rng
{
    uint64_t s = 0;
    uint64_t Next()
    {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t Below(uint32_t n) { return n ? uint32_t(Next() % n) : 0; }
    bool Chance(uint32_t percent) { return Below(100) < percent; }
};

// lmxxf_nr.asi's test entry points ([Plugin.lmxxf_nr] TestHooks=true)
struct Hooks
{
    void (*upscale)(OptiUpscaleFrame*, int32_t) = nullptr;
    void (*hudless)(void*, uint32_t) = nullptr;
    uint32_t (*status)() = nullptr;
    bool Load()
    {
        HMODULE m = GetModuleHandleW(L"lmxxf_nr.asi");
        if (m == nullptr)
            return false;
        upscale = (decltype(upscale)) GetProcAddress(m, "LmxxfNrTestUpscale");
        hudless = (decltype(hudless)) GetProcAddress(m, "LmxxfNrTestHudless");
        status = (decltype(status)) GetProcAddress(m, "LmxxfNrTestStatus");
        return upscale != nullptr && hudless != nullptr;
    }
};

inline const char* kBlitHlsl = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { float2 invDst; float gain; float pad; };
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (float2(id.xy) + 0.5) * invDst;
    dst[id.xy] = float4(src.SampleLevel(lin, uv, 0).rgb * gain, 1);
}
)";

// Scene at render size -> [before callback] -> scaled to the output size -> [after callback] -> HUD-less colour in
// the back buffer's format -> back buffer. The caller draws its UI on the back buffer after Frame().
struct Pipeline
{
    static constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    DXGI_FORMAT bbFmt = DXGI_FORMAT_R10G10B10A2_UNORM;
    float gain = 1;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> rtv, views;
    UINT rtvInc = 0, viewInc = 0;
    ComPtr<ID3D12Resource> color, mv, out, hud;
    UINT rw = 0, rh = 0, ow = 0, oh = 0;
    uint64_t upscaleId = 0;

    bool Init(ID3D12Device* d, ID3D12CommandQueue* q, DXGI_FORMAT fmt, float k)
    {
        dev = d, queue = q, bbFmt = fmt, gain = k;
        ComPtr<ID3DBlob> cs, err, blob;
        if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err)))
        {
            printf("blit shader: %s\n", err ? (const char*) err->GetBufferPointer() : "?");
            return false;
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 } };
        D3D12_ROOT_PARAMETER rp[3] {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.Num32BitValues = 4;
        rp[1].ParameterType = rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable = { 1, &ranges[0] };
        rp[2].DescriptorTable = { 1, &ranges[1] };
        D3D12_STATIC_SAMPLER_DESC smp {};
        smp.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rd { 3, rp, 1, &smp };
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = rs.Get();
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        D3D12_DESCRIPTOR_HEAP_DESC rh2 { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
        D3D12_DESCRIPTOR_HEAP_DESC vh { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))) ||
            FAILED(dev->CreateDescriptorHeap(&rh2, IID_PPV_ARGS(&rtv))) ||
            FAILED(dev->CreateDescriptorHeap(&vh, IID_PPV_ARGS(&views))))
            return false;
        rs->SetName(L"harness blit root"), pso->SetName(L"harness blit"), rtv->SetName(L"harness scene RTVs");
        views->SetName(L"harness blit views");
        rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        viewInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    ComPtr<ID3D12Resource> Tex(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st,
                               const float* clear, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w, d.Height = h, d.DepthOrArraySize = 1, d.MipLevels = 1, d.Format = f, d.SampleDesc.Count = 1;
        d.Flags = flags;
        D3D12_CLEAR_VALUE cv {};
        cv.Format = f;
        if (clear)
            memcpy(cv.Color, clear, sizeof(cv.Color));
        ComPtr<ID3D12Resource> r;
        if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, clear ? &cv : nullptr, IID_PPV_ARGS(&r))))
            r->SetName(name);
        return r;
    }

    // The GPU is idle (the caller waited) and the plugin has no HUD-less pointer of ours
    bool Targets(UINT outW, UINT outH, float scale)
    {
        color.Reset(), mv.Reset(), out.Reset(), hud.Reset();
        ow = outW, oh = outH;
        rw = (std::max)(64u, UINT(outW * scale + 0.5f) & ~1u);
        rh = (std::max)(64u, UINT(outH * scale + 0.5f) & ~1u);
        const float bg[4] = { 0.1f, 0.12f, 0.2f, 1 }, zero[4] = {};
        color = Tex(rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kRead, bg,
                    L"harness render colour");
        mv = Tex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kRead, zero,
                 L"harness motion vectors");
        out = Tex(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, L"harness upscaled output");
        hud = Tex(ow, oh, bbFmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead, nullptr, L"harness HUD-less colour");
        if (!color || !mv || !out || !hud)
            return false;
        auto h = rtv->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(color.Get(), nullptr, h);
        h.ptr += rtvInc;
        dev->CreateRenderTargetView(mv.Get(), nullptr, h);
        return true;
    }

    static void Move(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER x {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
        cl->ResourceBarrier(1, &x);
    }

    void Blit(ID3D12GraphicsCommandList* cl, UINT base, ID3D12Resource* src, ID3D12Resource* dst, UINT w, UINT h, float k)
    {
        auto cpu = views->GetCPUDescriptorHandleForHeapStart();
        auto gpu = views->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(base) * viewInc;
        gpu.ptr += UINT64(base) * viewInc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = src->GetDesc().Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(src, &sd, cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud {};
        ud.Format = dst->GetDesc().Format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(dst, nullptr, &ud, { cpu.ptr + viewInc });
        ID3D12DescriptorHeap* heaps[] = { views.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(rs.Get());
        cl->SetPipelineState(pso.Get());
        const float c[4] = { 1.0f / float(w), 1.0f / float(h), k, 0 };
        cl->SetComputeRoot32BitConstants(0, 4, c, 0);
        cl->SetComputeRootDescriptorTable(1, gpu);
        cl->SetComputeRootDescriptorTable(2, { gpu.ptr + viewInc });
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = dst;
        cl->ResourceBarrier(1, &b);
    }

    // bb comes in PRESENT and is left in RENDER_TARGET. slot: the frame's allocator slot (0-2), its views are free.
    void Frame(ID3D12GraphicsCommandList* cl, ID3D12Resource* bb, UINT slot, uint64_t frame, const Hooks& hooks,
               bool callbacks, bool reset, bool noMv)
    {
        auto hc = rtv->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE hm { hc.ptr + rtvInc };
        Move(cl, color.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Move(cl, mv.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float bg[4] = { 0.1f, 0.12f, 0.2f, 1 }, zero[4] = {};
        cl->ClearRenderTargetView(hc, bg, 0, nullptr);
        cl->ClearRenderTargetView(hm, zero, 0, nullptr);
        for (int j = 0; j < 8; ++j)
        {
            // the bars of the direct path, in render pixels; vectors point to the previous frame
            const float toRender = float(rw) / float(ow);
            const int x = int(float((frame * (4 + j * 3) + j * 400) % (UINT64) ow) * toRender);
            D3D12_RECT r { x, LONG(j * rh / 8), LONG((std::min)(UINT(x) + rw / 16, rw)), LONG((j + 1) * rh / 8 - 4) };
            if (r.right <= r.left)
                continue;
            const float c[4] = { (j & 1) ? 1.f : 0.2f, (j & 2) ? 1.f : 0.3f, (j & 4) ? 1.f : 0.4f, 1 };
            const float v[4] = { -float(4 + j * 3) * toRender, 0, 0, 0 };
            cl->ClearRenderTargetView(hc, c, 1, &r);
            cl->ClearRenderTargetView(hm, v, 1, &r);
        }
        Move(cl, color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
        Move(cl, mv.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);

        OptiUpscaleFrame f {};
        f.size = sizeof(f);
        f.api = OptiApi_D3D12;
        f.gameApi = OptiApi_D3D12;
        f.device = dev;
        f.commandList = cl;
        f.commandQueue = queue;
        f.color = color.Get();
        f.output = out.Get();
        f.motionVectors = noMv ? nullptr : mv.Get();
        f.colorState = f.motionVectorsState = kRead;
        f.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        f.renderWidth = rw, f.renderHeight = rh, f.outputWidth = ow, f.outputHeight = oh;
        f.mvScaleX = f.mvScaleY = 1;
        f.preExposure = 1;
        f.flags = OptiUpscale_AutoExposure | OptiUpscale_StatesAssumed | (reset ? OptiUpscale_Reset : 0);
        f.frameId = ++upscaleId;
        if (callbacks)
            hooks.upscale(&f, 0);
        auto* src = f.colorOverride ? static_cast<ID3D12Resource*>(f.colorOverride) : color.Get();
        Blit(cl, slot * 4, src, out.Get(), ow, oh, 1);
        if (callbacks)
            hooks.upscale(&f, 1);
        // "post-processing": the output in the back buffer's format is the HUD-less colour
        Move(cl, out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        Move(cl, hud.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Blit(cl, slot * 4 + 2, out.Get(), hud.Get(), ow, oh, gain);
        Move(cl, out.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Move(cl, hud.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Move(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(bb, hud.Get());
        Move(cl, hud.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
        Move(cl, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
};

// One fuzz step. The harness applies it; files are trigger files for the plugin's dump folder.
struct Action
{
    enum Kind { Ini, Hud, Dump, Seq, Resize, Minimise, Scale, MgpuLive, Reset, NoMv } kind = Ini;
    char cls = 'i';
    std::string what;
    std::vector<std::pair<std::wstring, std::string>> files;
    int a = 0, b = 0, c = 0;
    float f = 0;
    uint32_t gap = 30; // frames to the next step
    bool skip = false; // its class is masked: drawn from the seed like any other, not applied
};

struct Fuzzer
{
    Rng rng;
    bool mgpu = false, upscaler = false;
    std::string classes; // empty: all
    uint32_t minGap = 6;
    int burst = 0;
    bool hud = true;
    uint64_t minimisedUntil = 0; // frame
    std::vector<std::pair<int, int>> sizes;
    std::vector<float> scales { 0.5f, 0.667f, 0.77f, 1.0f };

    void Init(uint64_t seed, bool multiGpu, bool withUpscaler, const char* only, int fullW, int fullH)
    {
        rng.s = seed;
        mgpu = multiGpu, upscaler = withUpscaler;
        classes = only ? only : "";
        sizes = { { fullW, fullH }, { fullW, fullH }, { fullW / 2, fullH / 2 }, { fullW * 2 / 3, fullH * 2 / 3 },
                  { 1280, 720 }, { 1366, 768 }, { 1920, 1080 }, { 2560, 1440 } };
        for (auto& s : sizes)
            s.first = (std::min)(s.first, fullW), s.second = (std::min)(s.second, fullH);
    }

    bool Allowed(char c) const { return classes.empty() || classes.find(c) != std::string::npos; }

    static const char* B(bool v) { return v ? "true" : "false"; }

    std::string IniLines()
    {
        struct Key { const char* name; uint32_t weight; };
        static const Key keys[] = { { "Preset", 14 }, { "Model", 8 }, { "NetworkSize", 10 }, { "MultiPass", 6 },
                                    { "Temporal", 8 }, { "Placement", 14 }, { "TerminalHudless", 8 },
                                    { "EditUpsample", 7 }, { "EditStabilise", 7 }, { "Enabled", 4 },
                                    { "MotionVectors", 4 }, { "FlickerMeter", 4 }, { "WholeFrameFallback", 3 },
                                    { "Style", 2 }, { "Serialize", 1 } };
        uint32_t total = 0;
        for (auto& k : keys)
            total += k.weight;
        const int n = 1 + (rng.Chance(35) ? 1 : 0) + (rng.Chance(15) ? 1 : 0);
        std::string out;
        for (int i = 0; i < n; i++)
        {
            uint32_t r = rng.Below(total);
            const Key* k = keys;
            while (r >= k->weight)
                r -= k->weight, k++;
            const std::string name = k->name;
            std::string v;
            if (name == "Preset")
                v = std::to_string(rng.Below(5));
            else if (name == "Model" || name == "EditUpsample")
                v = std::to_string(rng.Below(2));
            else if (name == "NetworkSize" || name == "Placement" || name == "Style")
                v = std::to_string(rng.Below(3));
            else if (name == "MultiPass")
                v = std::to_string(1 + rng.Below(3));
            else if (name == "Enabled")
                v = B(!rng.Chance(25));
            else
                v = B(rng.Chance(50));
            out += name + "=" + v + "\n";
        }
        return out;
    }

    uint32_t Gap()
    {
        if (burst > 0)
            return minGap;
        const uint32_t r = rng.Below(100);
        if (r < 50)
            return minGap + rng.Below(15);
        if (r < 85)
            return 30 + rng.Below(50);
        return 150 + rng.Below(100);
    }

    Action Next(uint64_t frame)
    {
        const bool minimised = frame < minimisedUntil;
        struct Cls { char c; uint32_t weight; };
        static const Cls table[] = { { 'i', 52 }, { 'b', 5 }, { 'h', 6 }, { 'd', 6 }, { 's', 6 }, { 'r', 8 },
                                     { 'm', 4 }, { 'q', 5 }, { 'g', 4 }, { 'u', 4 } };
        Action a;
        char cls = 'i';
        // every draw comes from the seed, also the ones a class mask then drops
        {
            uint32_t total = 0;
            for (auto& t : table)
                total += t.weight;
            uint32_t r = rng.Below(total);
            const Cls* t = table;
            while (r >= t->weight)
                r -= t->weight, t++;
            cls = t->c;
        }
        if (burst > 0)
            cls = 'i', burst--;
        else if (cls == 'b')
            cls = 'i', burst = 2 + int(rng.Below(4));
        if ((cls == 'g' && !mgpu) || (cls == 'q' && !upscaler) || (cls == 'u' && !upscaler) ||
            (minimised && (cls == 'r' || cls == 'm' || cls == 'q')))
            cls = 'i';
        a.cls = cls;
        switch (cls)
        {
        case 'i':
        {
            a.kind = Action::Ini;
            std::string lines = IniLines();
            a.files.push_back({ L"lmxxf_nr_ini.trigger", lines });
            for (auto& ch : lines)
                if (ch == '\n')
                    ch = ' ';
            a.what = "ini " + lines;
            break;
        }
        case 'h':
            a.kind = Action::Hud;
            hud = !hud;
            a.a = hud;
            a.what = std::string("HUD-less source ") + (hud ? "on" : "off");
            break;
        case 'd':
            a.kind = Action::Dump;
            a.files.push_back({ L"lmxxf_nr_dump.trigger", "1\n" });
            if (mgpu)
                a.files.push_back({ L"mgpu_dump.trigger", std::to_string(1 + rng.Below(3)) + "\n" });
            a.what = "frame dump";
            break;
        case 's':
            a.kind = Action::Seq;
            a.a = 4 << rng.Below(3);
            a.files.push_back({ L"lmxxf_nr_seq.trigger", std::to_string(a.a) + "\n" });
            a.what = "sequence dump of " + std::to_string(a.a);
            break;
        case 'r':
        {
            a.kind = Action::Resize;
            const auto& s = sizes[rng.Below((uint32_t) sizes.size())];
            a.a = s.first, a.b = s.second;
            a.c = int(rng.Below(4)); // frames the window is resized ahead of ResizeBuffers
            a.what = "resize " + std::to_string(a.a) + "x" + std::to_string(a.b) + ", buffers " + std::to_string(a.c) +
                     " frames after the window";
            break;
        }
        case 'm':
            a.kind = Action::Minimise;
            a.a = 20 + int(rng.Below(80));
            a.b = rng.Chance(50); // ResizeBuffers at the same size after each step, as CONTROL does
            a.what = "minimise for " + std::to_string(a.a) + " frames" + (a.b ? ", ResizeBuffers after each step" : "");
            minimisedUntil = frame + a.a + 2;
            break;
        case 'q':
            a.kind = Action::Scale;
            a.f = scales[rng.Below((uint32_t) scales.size())];
            a.what = "render scale " + std::to_string(a.f);
            break;
        case 'g':
            a.kind = Action::MgpuLive;
            if (rng.Chance(50))
                a.files.push_back({ L"mgpu_ll.trigger", std::to_string(rng.Below(3)) + "\n" }), a.what = "mgpu low latency";
            else
                a.files.push_back({ L"mgpu_es.trigger", std::to_string(rng.Below(2)) + "\n" }), a.what = "mgpu early send";
            a.what += " " + a.files.back().second.substr(0, 1);
            break;
        case 'u':
            a.kind = rng.Chance(50) ? Action::Reset : Action::NoMv;
            a.a = 1 + int(rng.Below(6));
            a.what = (a.kind == Action::Reset ? "upscale reset flag for " : "no motion vectors for ") + std::to_string(a.a) +
                     " frames";
            break;
        default:
            break;
        }
        a.gap = Gap();
        if (!Allowed(cls))
        {
            // the same sequence as the unmasked seed, minus this step
            a.skip = true;
            if (cls == 'h')
                hud = !hud;
        }
        return a;
    }
};

// tmp + rename: the plugin never reads half a file
inline bool WriteTrigger(const std::wstring& dir, const std::wstring& name, const std::string& text)
{
    const std::wstring path = dir + L"\\" + name, tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (f == nullptr)
        return false;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
} // namespace hx
