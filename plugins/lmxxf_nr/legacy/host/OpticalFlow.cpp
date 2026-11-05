// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
#include "OpticalFlow.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

#include "../../../../src/plugin_sdk/optiscaler_plugin.h"
void Log(OptiLogLevel level, const char* fmt, ...);

namespace
{
constexpr uint32_t kBlock = 8;
constexpr uint32_t kWorkWidth = 960; // level 0 width cap
constexpr uint32_t kConsts = 12;
constexpr uint32_t kSlots = 64, kSlotDesc = 4;

// Level 0 is the source scaled to at most kWorkWidth; vectors are searched coarse to fine on 8x8 luma blocks, each
// level starting from the doubled vectors of its parent and the parent's 4 neighbours, then refined by +-Radius and a
// parabola for the sub-pixel part. Pixel: each level-0 pixel picks the best of its 3x3 neighbouring block vectors
// on a 3x3 window, so vectors follow edges instead of block borders.
constexpr const char kSource[] = R"(
Texture2D<float4> T0 : register(t0);
Texture2D<float4> T1 : register(t1);
Texture2D<float4> T2 : register(t2);
RWTexture2D<float4> Dst : register(u0);
SamplerState Lin : register(s0);
cbuffer C : register(b0)
{
    uint2 OutSize;
    uint2 InSize;
    float2 InInv;
    int Radius;
    uint HasParent;
    uint2 ParentBlocks;
    float ZeroBias;
    float Linear; // Luma: the source is linear, encode it for matching
};

[numthreads(8, 8, 1)] void Luma(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutSize)) return;
    float3 c = saturate(T0.SampleLevel(Lin, (id.xy + 0.5) / float2(OutSize), 0).rgb);
    if (Linear != 0) c = pow(c, 1 / 2.2);
    Dst[id.xy] = dot(c, float3(0.299, 0.587, 0.114));
}

[numthreads(8, 8, 1)] void Down(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutSize)) return;
    int2 p = int2(id.xy) * 2, m = int2(InSize) - 1;
    float s = T0.Load(int3(min(p, m), 0)).x + T0.Load(int3(min(p + int2(1, 0), m), 0)).x +
              T0.Load(int3(min(p + int2(0, 1), m), 0)).x + T0.Load(int3(min(p + 1, m), 0)).x;
    Dst[id.xy] = s * 0.25;
}

static const float kStill = 2.0 / 255; // mean luma difference treated as no motion

float BlockSad(int2 o, float2 v)
{
    float s = 0;
    int2 m = int2(InSize) - 1;
    [loop] for (int y = 0; y < 8; ++y)
        [unroll] for (int x = 0; x < 8; ++x)
        {
            int2 q = min(o + int2(x, y), m);
            float c = T0.Load(int3(q, 0)).x;
            float p = T1.SampleLevel(Lin, (q + 0.5 + v) * InInv, 0).x;
            s += abs(c - p);
        }
    return s;
}

[numthreads(8, 8, 1)] void Search(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutSize)) return;
    int2 o = int2(id.xy) * 8;
    float2 best = 0;
    float bestS = BlockSad(o, 0) * ZeroBias;
    // Unchanged within noise: still, no search (flat or static areas would pick noise vectors)
    if (bestS < kStill * 64) { Dst[id.xy] = float4(0, 0, bestS / 64, 0); return; }
    if (HasParent)
    {
        int2 pb = int2(id.xy) / 2, pm = int2(ParentBlocks) - 1;
        int2 off[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        float2 v = T2.Load(int3(min(pb, pm), 0)).xy * 2;
        float s = BlockSad(o, v);
        if (s < bestS) { bestS = s; best = v; }
        [unroll] for (int i = 0; i < 4; ++i)
        {
            v = T2.Load(int3(clamp(pb + off[i], 0, pm), 0)).xy * 2;
            s = BlockSad(o, v);
            if (s < bestS) { bestS = s; best = v; }
        }
    }
    float2 c = best;
    [loop] for (int dy = -Radius; dy <= Radius; ++dy)
        [loop] for (int dx = -Radius; dx <= Radius; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            float2 v = c + float2(dx, dy);
            float s = BlockSad(o, v);
            if (s < bestS) { bestS = s; best = v; }
        }
    // Sub-pixel: parabola through the neighbours
    float l = BlockSad(o, best - float2(1, 0)), r = BlockSad(o, best + float2(1, 0));
    float u = BlockSad(o, best - float2(0, 1)), d = BlockSad(o, best + float2(0, 1));
    float dxs = l + r - 2 * bestS, dys = u + d - 2 * bestS;
    float2 sub = float2(dxs > 1e-5 ? 0.5 * (l - r) / dxs : 0, dys > 1e-5 ? 0.5 * (u - d) / dys : 0);
    Dst[id.xy] = float4(best + clamp(sub, -0.5, 0.5), bestS / 64, 0);
}

[numthreads(8, 8, 1)] void Pixel(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutSize)) return;
    int2 p = int2(id.xy), m = int2(OutSize) - 1, bm = int2(ParentBlocks) - 1, b = p / 8;
    float2 best = 0;
    float bestS = 0;
    [unroll] for (int y0 = -1; y0 <= 1; ++y0)
        [unroll] for (int x0 = -1; x0 <= 1; ++x0)
        {
            int2 q = clamp(p + int2(x0, y0), 0, m);
            bestS += abs(T0.Load(int3(q, 0)).x - T1.Load(int3(q, 0)).x);
        }
    if (bestS < kStill * 9) { Dst[id.xy] = 0; return; }
    bestS *= ZeroBias;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            float2 v = T2.Load(int3(clamp(b + int2(i, j), 0, bm), 0)).xy;
            float s = 0;
            [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    int2 q = clamp(p + int2(x, y), 0, m);
                    s += abs(T0.Load(int3(q, 0)).x - T1.SampleLevel(Lin, (q + 0.5 + v) * InInv, 0).x);
                }
            if (s < bestS) { bestS = s; best = v; }
        }
    Dst[id.xy] = float4(best, 0, 0);
}

[numthreads(8, 8, 1)] void Zero(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutSize)) return;
    Dst[id.xy] = 0;
}
)";

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (a == b)
        return;
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

void UavBarrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    x.UAV.pResource = r;
    l->ResourceBarrier(1, &x);
}

constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

ComPtr<ID3D12Resource> Tex(ID3D12Device* d, uint32_t w, uint32_t h, DXGI_FORMAT f)
{
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, kRead, nullptr, IID_PPV_ARGS(&r));
    return r;
}

DXGI_FORMAT SrvFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}
} // namespace

bool OpticalFlow::Init(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    _dev = device;
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, kConsts };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 2, ranges };
    D3D12_STATIC_SAMPLER_DESC samp {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs { 2, params, 1, &samp };
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root))))
        return false;

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? (pD3DCompile) GetProcAddress(compiler, "D3DCompile") : nullptr;
    if (compile == nullptr)
        return false;
    auto make = [&](const char* entry, ComPtr<ID3D12PipelineState>& pso)
    {
        ComPtr<ID3DBlob> cs, e;
        if (FAILED(compile(kSource, sizeof(kSource) - 1, "of", nullptr, nullptr, entry, "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e)))
        {
            Log(OptiLog_Error, "optical flow: %s failed to compile: %s", entry, e ? (const char*) e->GetBufferPointer() : "");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC d {};
        d.pRootSignature = _root.Get();
        d.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        return SUCCEEDED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)));
    };
    if (!make("Luma", _psoLuma) || !make("Down", _psoDown) || !make("Search", _psoSearch) ||
        !make("Pixel", _psoPixel) || !make("Zero", _psoZero))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * kSlotDesc,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
        return false;
    _stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_QUERY_HEAP_DESC qd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * 8 };
    device->CreateQueryHeap(&qd, IID_PPV_ARGS(&_qh));
    D3D12_HEAP_PROPERTIES rb { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 2 * 8 * sizeof(uint64_t);
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                    IID_PPV_ARGS(&_qrb));
    uint64_t f = 0;
    queue->GetTimestampFrequency(&f);
    _freq = double(f);
    return true;
}

bool OpticalFlow::Ensure(uint32_t srcW, uint32_t srcH)
{
    if (srcW == _srcW && srcH == _srcH && _mv)
        return true;
    _srcW = srcW;
    _srcH = srcH;
    _valid = false;
    uint32_t w = std::min(srcW, kWorkWidth), h = std::max(1u, uint32_t(uint64_t(srcH) * w / srcW));
    for (uint32_t l = 0; l < kLevels; ++l)
    {
        _w[l] = w;
        _h[l] = h;
        _bw[l] = (w + kBlock - 1) / kBlock;
        _bh[l] = (h + kBlock - 1) / kBlock;
        for (auto& p : _luma)
            p[l] = Tex(_dev.Get(), w, h, DXGI_FORMAT_R16_FLOAT);
        _vec[l] = Tex(_dev.Get(), _bw[l], _bh[l], DXGI_FORMAT_R16G16B16A16_FLOAT);
        w = std::max(1u, (w + 1) / 2);
        h = std::max(1u, (h + 1) / 2);
    }
    _mv = Tex(_dev.Get(), _w[0], _h[0], DXGI_FORMAT_R16G16_FLOAT);
    if (_mv)
        _mv->SetName(L"lmxxf_legacy optical flow");
    Log(OptiLog_Info, "optical flow: source %ux%u, level 0 %ux%u, %ux%u blocks", srcW, srcH, _w[0], _h[0], _bw[0], _bh[0]);
    return _mv != nullptr;
}

void OpticalFlow::Dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const uint32_t* consts,
                           ID3D12Resource* const srv[3], const DXGI_FORMAT srvFmt[3], ID3D12Resource* uav,
                           DXGI_FORMAT uavFmt, uint32_t gx, uint32_t gy)
{
    const uint32_t slot = _cursor++ % kSlots;
    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * kSlotDesc * _stride;
    gpu.ptr += UINT64(slot) * kSlotDesc * _stride;
    for (uint32_t i = 0; i < 3; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        sd.Format = srv[i] ? srvFmt[i] : DXGI_FORMAT_R16_FLOAT;
        D3D12_CPU_DESCRIPTOR_HANDLE h { cpu.ptr + SIZE_T(i) * _stride };
        _dev->CreateShaderResourceView(srv[i], &sd, h);
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud {};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = uavFmt;
    _dev->CreateUnorderedAccessView(uav, nullptr, &ud, { cpu.ptr + SIZE_T(3) * _stride });
    list->SetPipelineState(pso);
    list->SetComputeRoot32BitConstants(0, kConsts, consts, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch(gx, gy, 1);
}

bool OpticalFlow::Run(ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, bool zero,
                      bool linear)
{
    const auto sd = src->GetDesc();
    if (!Ensure((uint32_t) sd.Width, sd.Height))
        return false;
    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_root.Get());
    const uint32_t q = uint32_t(_qFrame % 8) * 2;
    if (_qh)
        list->EndQuery(_qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);

    auto groups = [](uint32_t n) { return (n + 7) / 8; };
    auto consts = [](uint32_t* c, uint32_t ow, uint32_t oh, uint32_t iw, uint32_t ih, int radius, uint32_t parent,
                     uint32_t pbw, uint32_t pbh, float zeroBias)
    {
        float inv[2] = { 1.0f / float(iw), 1.0f / float(ih) };
        c[0] = ow, c[1] = oh, c[2] = iw, c[3] = ih;
        memcpy(&c[4], inv, 8);
        c[6] = uint32_t(radius), c[7] = parent, c[8] = pbw, c[9] = pbh;
        memcpy(&c[10], &zeroBias, 4);
        c[11] = 0;
    };
    const DXGI_FORMAT r16[3] = { DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
    uint32_t c[kConsts];
    bool wrote = false;
    if (zero)
    {
        consts(c, _w[0], _h[0], 1, 1, 0, 0, 1, 1, 1.0f);
        ID3D12Resource* none[3] = {};
        Barrier(list, _mv.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dispatch(list, _psoZero.Get(), c, none, r16, _mv.Get(), DXGI_FORMAT_R16G16_FLOAT, groups(_w[0]), groups(_h[0]));
        Barrier(list, _mv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        _valid = false;
        wrote = true;
    }
    else
    {
        auto& cur = _luma[_cur];
        auto& prev = _luma[_cur ^ 1];
        // Pyramid of this frame
        Barrier(list, src, srcState, kRead);
        for (uint32_t l = 0; l < kLevels; ++l)
        {
            ID3D12Resource* s[3] = { l == 0 ? src : cur[l - 1].Get(), nullptr, nullptr };
            const DXGI_FORMAT f[3] = { l == 0 ? SrvFormat(sd.Format) : DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT,
                                       DXGI_FORMAT_R16_FLOAT };
            consts(c, _w[l], _h[l], l ? _w[l - 1] : (uint32_t) sd.Width, l ? _h[l - 1] : sd.Height, 0, 0, 1, 1, 1.0f);
            const float lin = linear && l == 0 ? 1.0f : 0.0f;
            memcpy(&c[11], &lin, 4);
            Barrier(list, cur[l].Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Dispatch(list, l == 0 ? _psoLuma.Get() : _psoDown.Get(), c, s, f, cur[l].Get(), DXGI_FORMAT_R16_FLOAT,
                     groups(_w[l]), groups(_h[l]));
            Barrier(list, cur[l].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        }
        Barrier(list, src, kRead, srcState);
        if (_valid)
        {
            // Coarse to fine block search against the previous pyramid
            for (int l = int(kLevels) - 1; l >= 0; --l)
            {
                const bool top = l == int(kLevels) - 1;
                ID3D12Resource* s[3] = { cur[l].Get(), prev[l].Get(), top ? nullptr : _vec[l + 1].Get() };
                consts(c, _bw[l], _bh[l], _w[l], _h[l], top ? 6 : 2, top ? 0 : 1, top ? 1 : _bw[l + 1],
                       top ? 1 : _bh[l + 1], 0.95f);
                Barrier(list, _vec[l].Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Dispatch(list, _psoSearch.Get(), c, s, r16, _vec[l].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                         groups(_bw[l]), groups(_bh[l]));
                Barrier(list, _vec[l].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            }
            ID3D12Resource* s[3] = { cur[0].Get(), prev[0].Get(), _vec[0].Get() };
            consts(c, _w[0], _h[0], _w[0], _h[0], 0, 1, _bw[0], _bh[0], 0.9f);
            Barrier(list, _mv.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Dispatch(list, _psoPixel.Get(), c, s, r16, _mv.Get(), DXGI_FORMAT_R16G16_FLOAT, groups(_w[0]),
                     groups(_h[0]));
            Barrier(list, _mv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            wrote = true;
        }
        _valid = true;
        _cur ^= 1;
    }
    if (_qh)
    {
        list->EndQuery(_qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
        list->ResolveQueryData(_qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q, 2, _qrb.Get(), q * sizeof(uint64_t));
    }
    // Read the slot written 4 frames ago
    if (_qFrame >= 4 && _qrb)
    {
        const uint32_t r = uint32_t((_qFrame - 4) % 8) * 2;
        D3D12_RANGE range { r * sizeof(uint64_t), (r + 2) * sizeof(uint64_t) };
        uint64_t* p = nullptr;
        if (SUCCEEDED(_qrb->Map(0, &range, (void**) &p)))
        {
            if (p[r + 1] > p[r] && _freq > 0)
                _ms[_msN++ % 31] = float(double(p[r + 1] - p[r]) * 1000.0 / _freq);
            D3D12_RANGE none { 0, 0 };
            _qrb->Unmap(0, &none);
        }
    }
    ++_qFrame;
    return wrote;
}

float OpticalFlow::GpuMs()
{
    const uint32_t n = std::min<uint32_t>(_msN, 31);
    if (n == 0)
        return 0;
    float t[31];
    std::copy(_ms, _ms + n, t);
    std::nth_element(t, t + n / 2, t + n);
    return t[n / 2];
}
