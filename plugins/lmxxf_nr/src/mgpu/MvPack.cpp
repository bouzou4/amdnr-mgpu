// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
#include "MvPack.h"

#include <d3dcompiler.h>
#include <cstring>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace Mgpu
{
static const char* kMvPackShader = R"(
ByteAddressBuffer inB : register(t0);
RWByteAddressBuffer outB : register(u0);
RWByteAddressBuffer auxB : register(u1);
cbuffer C : register(b0) { uint w, h, pitch, tilesX, tiles, dataBase, totOff; };
groupshared uint gMin0, gMin1, gMax0, gMax1;
groupshared uint gWords[64];
groupshared uint gPart[1024];

// order-preserving key of a half's bits: -x < -0 < +0 < x, a bijection on 16 bits
uint Key(uint u) { return (u & 0x8000) ? (~u & 0xFFFF) : (u | 0x8000); }
uint Unkey(uint k) { return (k & 0x8000) ? (k & 0x7FFF) : (~k & 0xFFFF); }
uint Bits(uint lo, uint hi) { return hi > lo ? firstbithigh(hi - lo) + 1 : 0; }
uint TileWords(uint x) { return 2 * ((x & 31) + ((x >> 5) & 31)); }

[numthreads(8, 8, 1)]
void measure(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) { gMin0 = 0xFFFF; gMin1 = 0xFFFF; gMax0 = 0; gMax1 = 0; }
    GroupMemoryBarrierWithGroupSync();
    uint2 p = g.xy * 8 + t.xy;
    if (p.x < w && p.y < h)
    {
        uint v = inB.Load(p.y * pitch + p.x * 4);
        uint k0 = Key(v & 0xFFFF), k1 = Key(v >> 16);
        InterlockedMin(gMin0, k0); InterlockedMax(gMax0, k0);
        InterlockedMin(gMin1, k1); InterlockedMax(gMax1, k1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
        outB.Store2((g.y * tilesX + g.x) * 8, uint2(gMin0 | (gMin1 << 16), Bits(gMin0, gMax0) | (Bits(gMin1, gMax1) << 5)));
}

// one group: data offset of every tile (in 2-word units, from bit 10 of the header's second word) and the total
[numthreads(1024, 1, 1)]
void scan(uint gi : SV_GroupIndex)
{
    uint per = (tiles + 1023) / 1024, a = gi * per, e = min(a + per, tiles), s = 0, i;
    for (i = a; i < e; ++i)
        s += TileWords(outB.Load(i * 8 + 4));
    gPart[gi] = s;
    GroupMemoryBarrierWithGroupSync();
    for (uint o = 1; o < 1024; o <<= 1)
    {
        uint v = gi >= o ? gPart[gi - o] : 0;
        GroupMemoryBarrierWithGroupSync();
        gPart[gi] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint off = gPart[gi] - s;
    for (i = a; i < e; ++i)
    {
        uint x = outB.Load(i * 8 + 4);
        outB.Store(i * 8 + 4, (x & 1023) | ((off >> 1) << 10));
        off += TileWords(x);
    }
    if (gi == 1023)
        auxB.Store(totOff, dataBase + gPart[1023] * 4);
}

void Put(uint r, uint b, uint bit)
{
    if (b == 0) return;
    uint i = bit >> 5, s = bit & 31;
    InterlockedOr(gWords[i], r << s);
    if (s + b > 32) InterlockedOr(gWords[i + 1], r >> (32 - s));
}

[numthreads(8, 8, 1)]
void pack(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    gWords[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint tile = g.y * tilesX + g.x;
    uint2 hd = outB.Load2(tile * 8);
    uint m0 = hd.x & 0xFFFF, m1 = hd.x >> 16, b0 = hd.y & 31, b1 = (hd.y >> 5) & 31;
    uint2 p = g.xy * 8 + t.xy;
    if (p.x < w && p.y < h)
    {
        uint v = inB.Load(p.y * pitch + p.x * 4);
        Put(Key(v & 0xFFFF) - m0, b0, gi * b0);
        Put(Key(v >> 16) - m1, b1, 64 * b0 + gi * b1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 2 * (b0 + b1))
        outB.Store(dataBase + ((hd.y >> 10) * 2 + gi) * 4, gWords[gi]);
}

uint Get(uint b, uint base, uint bit)
{
    if (b == 0) return 0;
    uint i = bit >> 5, s = bit & 31;
    uint v = inB.Load(base + i * 4) >> s;
    if (s + b > 32) v |= inB.Load(base + i * 4 + 4) << (32 - s);
    return v & ((1u << b) - 1);
}

[numthreads(8, 8, 1)]
void decode(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    uint2 p = g.xy * 8 + t.xy;
    if (p.x >= w || p.y >= h) return;
    uint2 hd = inB.Load2((g.y * tilesX + g.x) * 8);
    uint b0 = hd.y & 31, b1 = (hd.y >> 5) & 31, base = dataBase + (hd.y >> 10) * 8;
    uint k0 = Get(b0, base, gi * b0) + (hd.x & 0xFFFF);
    uint k1 = Get(b1, base, 64 * b0 + gi * b1) + (hd.x >> 16);
    outB.Store(p.y * pitch + p.x * 4, Unkey(k0) | (Unkey(k1) << 16));
}
)";

typedef HRESULT(WINAPI* PFN_MvCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                       UINT, UINT, ID3DBlob**, ID3DBlob**);

bool MvPack::Init(ID3D12Device* dev)
{
    _dev = dev;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? (PFN_MvCompile) GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile)
        return _err = "no d3dcompiler_47", false;
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 1 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 2, ranges };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 0, 0, 7 };
    D3D12_ROOT_SIGNATURE_DESC rd { 2, params };
    ComPtr<ID3DBlob> rs, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err)) ||
        FAILED(dev->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&_root))))
        return _err = "root signature", false;
    const char* entries[4] = { "measure", "scan", "pack", "decode" };
    ComPtr<ID3D12PipelineState>* psos[4] = { &_measure, &_scan, &_pack, &_decode };
    for (int i = 0; i < 4; ++i)
    {
        ComPtr<ID3DBlob> cs;
        if (FAILED(compile(kMvPackShader, strlen(kMvPackShader), "mgpu_mv_pack", nullptr, nullptr, entries[i], "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err)))
            return _err = err ? (const char*) err->GetBufferPointer() : "compile", false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = _root.Get();
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&*psos[i]))))
            return _err = "pipeline", false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * 2 * kSlots,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
        return _err = "descriptor heap", false;
    _inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void MvPack::Bind(ID3D12GraphicsCommandList* cl, UINT slot, ID3D12Resource* srv, ID3D12Resource* uav0,
                  ID3D12Resource* uav1, const UINT* k)
{
    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * 3 * _inc;
    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(slot) * 3 * _inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC s {};
    s.Format = DXGI_FORMAT_R32_TYPELESS;
    s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    s.Buffer.NumElements = UINT(srv->GetDesc().Width / 4);
    s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    _dev->CreateShaderResourceView(srv, &s, cpu);
    for (ID3D12Resource* r : { uav0, uav1 })
    {
        cpu.ptr += _inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = UINT(r->GetDesc().Width / 4);
        u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        _dev->CreateUnorderedAccessView(r, nullptr, &u, cpu);
    }
    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(_root.Get());
    cl->SetComputeRootDescriptorTable(0, gpu);
    cl->SetComputeRoot32BitConstants(1, 7, k, 0);
}

static void UavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

void MvPack::Encode(ID3D12GraphicsCommandList* cl, UINT slot, ID3D12Resource* src, UINT w, UINT h,
                    ID3D12Resource* out, ID3D12Resource* total)
{
    const UINT tx = (w + kTile - 1) / kTile, ty = (h + kTile - 1) / kTile;
    const UINT k[7] = { w, h, Pitch(w), tx, tx * ty, UINT(HeaderBytes(w, h)), (slot % kSlots) * 4 };
    Bind(cl, (slot % kSlots) * 2, src, out, total, k);
    cl->SetPipelineState(_measure.Get());
    cl->Dispatch(tx, ty, 1);
    UavBarrier(cl, out);
    cl->SetPipelineState(_scan.Get());
    cl->Dispatch(1, 1, 1);
    UavBarrier(cl, out);
    cl->SetPipelineState(_pack.Get());
    cl->Dispatch(tx, ty, 1);
    UavBarrier(cl, out);
    UavBarrier(cl, total);
}

void MvPack::Decode(ID3D12GraphicsCommandList* cl, UINT slot, ID3D12Resource* packed, UINT w, UINT h,
                    ID3D12Resource* dst)
{
    const UINT tx = (w + kTile - 1) / kTile, ty = (h + kTile - 1) / kTile;
    const UINT k[7] = { w, h, Pitch(w), tx, tx * ty, UINT(HeaderBytes(w, h)), 0 };
    Bind(cl, (slot % kSlots) * 2 + 1, packed, dst, dst, k);
    cl->SetPipelineState(_decode.Get());
    cl->Dispatch(tx, ty, 1);
    UavBarrier(cl, dst);
}
} // namespace Mgpu
