#include "MgpuCommon.h"
#include "MgpuComposite.h"
#include "MgpuCore.h"

#include <d3dcompiler.h>
#include <format>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace Mgpu
{
static const char* kShader = R"(
Texture2D<float4> bbT : register(t0);
Texture2D<float4> hudT : register(t1);
Texture2D<float4> nrT : register(t2);
RWTexture2D<float4> outT : register(u0);
RWByteAddressBuffer cnt : register(u1);
cbuffer C : register(b0) { uint w; uint h; uint mode; float t0; float t1; };
groupshared uint gs;
[numthreads(16, 16, 1)]
void main(uint3 p : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) gs = 0;
    GroupMemoryBarrierWithGroupSync();
    if (p.x < w && p.y < h)
    {
        float4 b = bbT.Load(int3(p.xy, 0));
        float4 u = hudT.Load(int3(p.xy, 0));
        float3 d = abs(b.rgb - u.rgb);
        float e = max(d.r, max(d.g, d.b));
        float m = smoothstep(t0, t1, e);
        if (e > t0) InterlockedAdd(gs, 1u);
        if (mode == 1)
        {
            float4 n = nrT.Load(int3(p.xy, 0));
            outT[p.xy] = m >= 1.0 ? b : (m <= 0.0 ? n : lerp(n, b, m));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gs > 0)
    {
        uint o;
        cnt.InterlockedAdd(0, gs, o);
    }
}
)";

using PFN_Compile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

static D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER v {};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    return v;
}

bool UiComposite::Init(ID3D12Device* dev, DXGI_FORMAT outFormat)
{
    _dev = dev;
    _fmt = outFormat;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? (PFN_Compile) GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile)
    {
        Log(2, "UI composite: no d3dcompiler_47");
        return false;
    }
    ComPtr<ID3DBlob> cs, err;
    if (FAILED(compile(kShader, strlen(kShader), "mgpu_ui_composite", nullptr, nullptr, "main", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err)))
    {
        Log(2, std::format("UI composite: compile failed: {}", err ? (const char*) err->GetBufferPointer() : "?"));
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 3 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 2, ranges };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 0, 0, 5 };
    D3D12_ROOT_SIGNATURE_DESC rd { 2, params };
    ComPtr<ID3DBlob> rs;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err)) ||
        FAILED(dev->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&_root))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = _root.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&_pso))))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5 * kSlots,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
        return false;
    _inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_RESOURCE_DESC b {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = 16 * kSlots;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES dp { D3D12_HEAP_TYPE_DEFAULT }, up { D3D12_HEAP_TYPE_UPLOAD }, rp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bu = b;
    bu.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&dp, D3D12_HEAP_FLAG_NONE, &bu, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&_counter))) ||
        FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&_zero))) ||
        FAILED(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&_readback))))
        return false;
    void* z = nullptr;
    if (SUCCEEDED(_zero->Map(0, nullptr, &z)))
    {
        memset(z, 0, 16 * kSlots);
        _zero->Unmap(0, nullptr);
    }
    return true;
}

void UiComposite::Record(ID3D12GraphicsCommandList* cl, UINT slot, ID3D12Resource* bb, ID3D12Resource* hud,
                         ID3D12Resource* nr, ID3D12Resource* out, UINT w, UINT h)
{
    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * 5 * _inc;
    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(slot) * 5 * _inc;
    ID3D12Resource* srvs[3] = { bb, hud, nr ? nr : hud };
    for (auto* r : srvs)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s {};
        s.Format = _fmt;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        _dev->CreateShaderResourceView(r, &s, cpu);
        cpu.ptr += _inc;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
    u.Format = _fmt;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _dev->CreateUnorderedAccessView(nr ? out : nullptr, nullptr, &u, cpu);
    cpu.ptr += _inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC c {};
    c.Format = DXGI_FORMAT_R32_TYPELESS;
    c.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    c.Buffer.FirstElement = slot * 4;
    c.Buffer.NumElements = 4;
    c.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    _dev->CreateUnorderedAccessView(_counter.Get(), nullptr, &c, cpu);

    auto b0 = Tr(_counter.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier(1, &b0);
    cl->CopyBufferRegion(_counter.Get(), slot * 16, _zero.Get(), 0, 16);
    auto b1 = Tr(_counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->ResourceBarrier(1, &b1);

    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(_root.Get());
    cl->SetPipelineState(_pso.Get());
    cl->SetComputeRootDescriptorTable(0, gpu);
    struct
    {
        UINT w, h, mode;
        float t0, t1;
    } k { w, h, nr ? 1u : 0u, kT0, kT1 };
    cl->SetComputeRoot32BitConstants(1, 5, &k, 0);
    cl->Dispatch((w + 15) / 16, (h + 15) / 16, 1);

    D3D12_RESOURCE_BARRIER b2[2] = { Tr(_counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_COPY_SOURCE) };
    UINT nb = 1;
    if (nr && out)
    {
        b2[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b2[1].UAV.pResource = out;
        nb = 2;
    }
    cl->ResourceBarrier(nb, b2);
    cl->CopyBufferRegion(_readback.Get(), slot * 16, _counter.Get(), slot * 16, 16);
    auto b3 = Tr(_counter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    cl->ResourceBarrier(1, &b3);
}

UINT UiComposite::Count(UINT slot)
{
    UINT* p = nullptr;
    D3D12_RANGE r { slot * 16, slot * 16 + 4 };
    if (FAILED(_readback->Map(0, &r, (void**) &p)))
        return 0;
    UINT v = p[slot * 4];
    D3D12_RANGE w { 0, 0 };
    _readback->Unmap(0, &w);
    return v;
}
} // namespace Mgpu

namespace Mgpu
{
static const char* kDiffShader = R"(
Texture2D<float4> bbT : register(t0);
Texture2D<float4> hudT : register(t1);
RWByteAddressBuffer flags : register(u0);
cbuffer C : register(b0) { uint w; uint h; uint tx; uint base; };
groupshared uint gs;
[numthreads(16, 16, 1)]
void main(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) gs = 0;
    GroupMemoryBarrierWithGroupSync();
    uint d = 0;
    [unroll] for (uint y = 0; y < 4; ++y)
    [unroll] for (uint x = 0; x < 4; ++x)
    {
        uint2 p = g.xy * 64 + t.xy + uint2(x, y) * 16;
        if (p.x < w && p.y < h)
            d |= any(asuint(bbT.Load(int3(p, 0))) != asuint(hudT.Load(int3(p, 0)))) ? 1u : 0u;
    }
    if (d) InterlockedOr(gs, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) flags.Store((base + g.y * tx + g.x) * 4, gs);
}
)";

bool TileDiff::Init(ID3D12Device* dev, DXGI_FORMAT fmt, UINT w, UINT h, UINT buffers)
{
    _dev = dev;
    _fmt = fmt;
    _w = w;
    _h = h;
    _tx = (w + kTile - 1) / kTile;
    _ty = (h + kTile - 1) / kTile;
    _n = _tx * _ty;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? (PFN_Compile) GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile)
        return false;
    ComPtr<ID3DBlob> cs, err;
    if (FAILED(compile(kDiffShader, strlen(kDiffShader), "mgpu_tile_diff", nullptr, nullptr, "main", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err)))
    {
        Log(2, std::format("tile diff: compile failed: {}", err ? (const char*) err->GetBufferPointer() : "?"));
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 2, ranges };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 0, 0, 4 };
    D3D12_ROOT_SIGNATURE_DESC rd { 2, params };
    ComPtr<ID3DBlob> rs;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err)) ||
        FAILED(dev->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&_root))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = _root.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&_pso))))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * buffers,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
        return false;
    _inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_RESOURCE_DESC b {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = UINT64(_n) * 4 * buffers;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES dp { D3D12_HEAP_TYPE_DEFAULT }, rp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bu = b;
    bu.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(dev->CreateCommittedResource(&dp, D3D12_HEAP_FLAG_NONE, &bu, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&_flags))) &&
           SUCCEEDED(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&_readback)));
}

void TileDiff::Record(ID3D12GraphicsCommandList* cl, UINT i, ID3D12Resource* bb, ID3D12Resource* hud)
{
    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(i) * 3 * _inc;
    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(i) * 3 * _inc;
    for (auto* r : { bb, hud })
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s {};
        s.Format = _fmt;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        _dev->CreateShaderResourceView(r, &s, cpu);
        cpu.ptr += _inc;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = UINT(_flags->GetDesc().Width / 4);
    u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    _dev->CreateUnorderedAccessView(_flags.Get(), nullptr, &u, cpu);

    auto b0 = Tr(_flags.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->ResourceBarrier(1, &b0);
    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(_root.Get());
    cl->SetPipelineState(_pso.Get());
    cl->SetComputeRootDescriptorTable(0, gpu);
    const UINT k[4] = { _w, _h, _tx, i * _n };
    cl->SetComputeRoot32BitConstants(1, 4, k, 0);
    cl->Dispatch(_tx, _ty, 1);
    auto b1 = Tr(_flags.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1, &b1);
    cl->CopyBufferRegion(_readback.Get(), UINT64(i) * _n * 4, _flags.Get(), UINT64(i) * _n * 4, UINT64(_n) * 4);
    auto b2 = Tr(_flags.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    cl->ResourceBarrier(1, &b2);
}

const UINT* TileDiff::Map(UINT i)
{
    UINT* p = nullptr;
    D3D12_RANGE r { SIZE_T(i) * _n * 4, SIZE_T(i + 1) * _n * 4 };
    if (FAILED(_readback->Map(0, &r, (void**) &p)))
        return nullptr;
    return p + SIZE_T(i) * _n;
}

void TileDiff::Unmap()
{
    D3D12_RANGE w { 0, 0 };
    _readback->Unmap(0, &w);
}
} // namespace Mgpu
