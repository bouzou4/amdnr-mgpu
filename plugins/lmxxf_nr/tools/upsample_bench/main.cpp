// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Offline edit upsample bench on one adapter (no window, no swapchain).
//   upsample_bench apply <dir> <entry> [--bus N] [--reps N] [--cs X] [--up 0|1] [--frames N] [--stab] [--cadence P,D]
//       dir: src_NN.f32 (4K, WxHx3 linear), fed_NN.f32 / enh_NN.f32 (feed, linear), size.txt "W H w h".
//       --stab: the edit stabiliser on, history across frames, optional mv_NN.f32 (4K px toward the previous frame).
//       --cadence P,D (with --stab): NrCadence as the plugin runs it. The pair of every P-th frame, applied D frames
//       later (1 <= D <= P) at the accumulated motion; the other frames hold the reprojected history. Writes cd_NN.f32.
//       Prints the cadence guard's sums per frame (HistoryCheck: moving, missed moved, missed in place, changed in place).
//       Runs the plugin shader <entry> (ApplyTerminal, HUD-less = Src) and writes up_NN.f32; prints the GPU ms.
//   upsample_bench fsr <dir> <ffx dir> [--bus N] [--version S] [--frames N] [--sharp X] [--reps N]
//       dir: col_NN.f32 (render size, linear), mv_NN.f32 (render px toward the previous frame, no jitter),
//       optional depth_NN.f32, jitter.txt (one "jx jy" per frame, FFX convention), size.txt.
//       FSR upscaler through the FFX API modules in <ffx dir>; writes fsr_NN.f32; prints the GPU ms.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3dkmthk.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "../../src/Shaders.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

[[noreturn]] static void Die(const char* m, HRESULT hr = 0)
{
    std::fprintf(stderr, "error: %s (%08lx)\n", m, (unsigned long) hr);
    std::exit(1);
}
#define CK(x)                                                                                                         \
    do                                                                                                                \
    {                                                                                                                 \
        HRESULT hr_ = (x);                                                                                            \
        if (FAILED(hr_))                                                                                              \
            Die(#x, hr_);                                                                                             \
    } while (0)

static int PciBus(LUID luid)
{
    D3DKMT_OPENADAPTERFROMLUID open {};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0)
        return -1;
    D3DKMT_ADAPTERADDRESS addr {};
    D3DKMT_QUERYADAPTERINFO q {};
    q.hAdapter = open.hAdapter;
    q.Type = KMTQAITYPE_ADAPTERADDRESS;
    q.pPrivateDriverData = &addr;
    q.PrivateDriverDataSize = sizeof addr;
    int bus = D3DKMTQueryAdapterInfo(&q) == 0 && addr.BusNumber != 0xFFFFFFFF ? int(addr.BusNumber) : -1;
    D3DKMT_CLOSEADAPTER close { open.hAdapter };
    D3DKMTCloseAdapter(&close);
    return bus;
}

static std::vector<float> ReadF(const std::string& p, size_t n, bool optional = false)
{
    std::vector<float> v;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f)
    {
        if (optional)
            return v;
        Die(("missing " + p).c_str());
    }
    v.resize(n);
    if (std::fread(v.data(), 4, n, f) != n)
        Die(("short " + p).c_str());
    std::fclose(f);
    return v;
}

static void WriteF(const std::string& p, const std::vector<float>& v)
{
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f)
        Die(("write " + p).c_str());
    std::fwrite(v.data(), 4, v.size(), f);
    std::fclose(f);
}

static std::string Nn(const std::string& dir, const char* stem, unsigned t)
{
    char b[64];
    std::snprintf(b, sizeof b, "/%s_%02u.f32", stem, t);
    return dir + b;
}

struct Gpu
{
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fv = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<ID3D12QueryHeap> qh;
    ComPtr<ID3D12Resource> qrb;
    double freq = 1;

    void Init(int bus)
    {
        ComPtr<IDXGIFactory6> f;
        CK(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)));
        ComPtr<IDXGIAdapter1> pick;
        for (UINT i = 0;; i++)
        {
            ComPtr<IDXGIAdapter1> a;
            if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            int b = PciBus(d.AdapterLuid);
            std::printf("adapter %u bus=%d %ls\n", i, b, d.Description);
            if (!pick && (bus < 0 || b == bus))
                pick = a;
        }
        if (!pick)
            Die("no adapter on that bus");
        CK(D3D12CreateDevice(pick.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
        std::printf("device bus=%d\n", PciBus(dev->GetAdapterLuid()));
        D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
        CK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
        CK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
        CK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)));
        CK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        D3D12_QUERY_HEAP_DESC hd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 };
        CK(dev->CreateQueryHeap(&hd, IID_PPV_ARGS(&qh)));
        qrb = Buffer(16, D3D12_HEAP_TYPE_READBACK);
        UINT64 fr;
        q->GetTimestampFrequency(&fr);
        freq = double(fr);
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES hp { type };
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        CK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                       : D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&r)));
        return r;
    }
    ComPtr<ID3D12Resource> Tex(UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
    {
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        CK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(&r)));
        return r;
    }
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        if (a == b)
            return;
        D3D12_RESOURCE_BARRIER br {};
        br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
        list->ResourceBarrier(1, &br);
    }
    void Submit()
    {
        CK(list->Close());
        ID3D12CommandList* l[] = { list.Get() };
        q->ExecuteCommandLists(1, l);
        q->Signal(fence.Get(), ++fv);
        fence->SetEventOnCompletion(fv, ev);
        WaitForSingleObject(ev, INFINITE);
        CK(dev->GetDeviceRemovedReason());
        CK(alloc->Reset());
        CK(list->Reset(alloc.Get(), nullptr));
    }
    // float data (c channels per texel, padded to 4 or 2/1 as the format needs) into a COMMON texture
    void Upload(ID3D12Resource* t, const std::vector<float>& v, UINT c)
    {
        auto d = t->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        UINT64 total;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto up = Buffer(total, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* p;
        CK(up->Map(0, nullptr, (void**) &p));
        const UINT tc = d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 4 : d.Format == DXGI_FORMAT_R32G32_FLOAT ? 2 : 1;
        for (UINT y = 0; y < d.Height; y++)
        {
            float* row = (float*) (p + fp.Offset + UINT64(y) * fp.Footprint.RowPitch);
            for (UINT x = 0; x < UINT(d.Width); x++)
                for (UINT k = 0; k < tc; k++)
                    row[x * tc + k] = k < c ? v[(UINT64(y) * d.Width + x) * c + k] : 1.f;
        }
        up->Unmap(0, nullptr);
        Barrier(t, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, src { up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Submit();
    }
    // RGBA32F texture in `state` -> WxHx3 floats (ch 4: all four channels)
    std::vector<float> Download(ID3D12Resource* t, D3D12_RESOURCE_STATES state, int ch = 3)
    {
        auto d = t->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        UINT64 total;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto rb = Buffer(total, D3D12_HEAP_TYPE_READBACK);
        Barrier(t, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, dst { rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(t, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        Submit();
        std::vector<float> v(UINT64(d.Width) * d.Height * ch);
        uint8_t* p;
        CK(rb->Map(0, nullptr, (void**) &p));
        for (UINT y = 0; y < d.Height; y++)
        {
            const float* row = (const float*) (p + fp.Offset + UINT64(y) * fp.Footprint.RowPitch);
            for (UINT x = 0; x < UINT(d.Width); x++)
                for (int k = 0; k < ch; k++)
                    v[(UINT64(y) * d.Width + x) * ch + k] = row[x * 4 + k];
        }
        rb->Unmap(0, nullptr);
        return v;
    }
    void Stamp(UINT i) { list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i); }
    double Ms() // after Stamp(0) ... Stamp(1) and Submit-with-resolve
    {
        uint64_t* p;
        CK(qrb->Map(0, nullptr, (void**) &p));
        double ms = double(p[1] - p[0]) / freq * 1000.0;
        qrb->Unmap(0, nullptr);
        return ms;
    }
    void Resolve() { list->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb.Get(), 0); }
};

static void Median(std::vector<double>& v, const char* what)
{
    if (v.empty())
        return;
    std::sort(v.begin(), v.end());
    std::printf("%s ms: median %.3f min %.3f max %.3f (n %zu)\n", what, v[v.size() / 2], v.front(), v.back(), v.size());
}

static int RunApply(Gpu& g, const std::string& dir, const char* entry, int reps, float cs, uint32_t up, unsigned frames, UINT W, UINT H,
                    UINT w, UINT h, bool stab, unsigned period, unsigned delay)
{
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? reinterpret_cast<pD3DCompile>(GetProcAddress(comp, "D3DCompile")) : nullptr;
    if (!compile)
        Die("d3dcompiler_47.dll");
    ComPtr<ID3DBlob> cs_, err;
    if (FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, entry, "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_, &err)))
        Die(err ? (const char*) err->GetBufferPointer() : "compile");
    // the plugin's root signature (Nr.cpp CreatePipelines)
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[3] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, sizeof(ShaderConstants) / 4 };
    params[1].ParameterType = params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &ranges[0] };
    params[2].DescriptorTable = { 1, &ranges[1] };
    D3D12_STATIC_SAMPLER_DESC smp[2] {};
    for (int i = 0; i < 2; i++)
    {
        smp[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        smp[i].AddressU = smp[i].AddressV = smp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp[i].MaxLOD = D3D12_FLOAT32_MAX;
        smp[i].ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC rd { 3, params, 2, smp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D12RootSignature> rs;
    CK(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
    CK(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = rs.Get();
    pd.CS = { cs_->GetBufferPointer(), cs_->GetBufferSize() };
    ComPtr<ID3D12PipelineState> pso;
    CK(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));

    ComPtr<ID3D12PipelineState> psoAcc, psoCheck;
    if (period)
    {
        ComPtr<ID3DBlob> cb;
        if (FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "HistoryCheck", "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cb, &err)))
            Die(err ? (const char*) err->GetBufferPointer() : "compile");
        pd.CS = { cb->GetBufferPointer(), cb->GetBufferSize() };
        CK(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&psoCheck)));
        ComPtr<ID3DBlob> ab;
        if (FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "Accumulate", "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ab, &err)))
            Die(err ? (const char*) err->GetBufferPointer() : "compile");
        pd.CS = { ab->GetBufferPointer(), ab->GetBufferSize() };
        CK(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&psoAcc)));
    }

    const auto F = DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto src = g.Tex(W, H, F, false), enh = g.Tex(w, h, F, false), fed = g.Tex(w, h, F, false), dst = g.Tex(W, H, F, true);
    auto norm = g.Tex(1, 1, F, false);
    const auto F16 = DXGI_FORMAT_R16G16B16A16_FLOAT, A16 = DXGI_FORMAT_R16G16_FLOAT;
    ComPtr<ID3D12Resource> hist[2] = { g.Tex(W, H, F16, true), g.Tex(W, H, F16, true) };
    ComPtr<ID3D12Resource> acc[2] = { g.Tex(W, H, A16, true), g.Tex(W, H, A16, true) };
    auto mvt = g.Tex(W, H, DXGI_FORMAT_R32G32_FLOAT, false);
    const UINT bw = (W + 31) / 32, bh = (H + 31) / 32;
    auto stat = g.Tex(bw, bh, F, true);
    // descriptor sets of 10 (t0-t7, u0-u1): 0 the apply, 1 the accumulated motion; rewritten per frame
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 20, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    CK(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    const UINT st = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto bind = [&](int set, ID3D12Resource* const srvs[8], const DXGI_FORMAT sf[8], ID3D12Resource* u0, DXGI_FORMAT f0,
                    ID3D12Resource* u1)
    {
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(set) * 10 * st;
        for (int i = 0; i < 8; i++)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd {};
            sd.Format = sf[i];
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            g.dev->CreateShaderResourceView(srvs[i], &sd, { cpu.ptr + SIZE_T(i) * st });
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud {};
        ud.Format = f0;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.dev->CreateUnorderedAccessView(u0, nullptr, &ud, { cpu.ptr + SIZE_T(8) * st });
        ud.Format = F16;
        g.dev->CreateUnorderedAccessView(u1, nullptr, &ud, { cpu.ptr + SIZE_T(9) * st });
    };
    auto run = [&](int set, ID3D12PipelineState* p, const ShaderConstants& k)
    {
        ID3D12DescriptorHeap* hs[] = { heap.Get() };
        g.list->SetDescriptorHeaps(1, hs);
        g.list->SetComputeRootSignature(rs.Get());
        g.list->SetPipelineState(p);
        g.list->SetComputeRoot32BitConstants(0, sizeof k / 4, &k, 0);
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(set) * 10 * st;
        g.list->SetComputeRootDescriptorTable(1, gpu);
        g.list->SetComputeRootDescriptorTable(2, { gpu.ptr + UINT64(8) * st });
        g.list->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    };
    for (auto& t : hist)
        g.Barrier(t.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (auto& t : acc)
        g.Barrier(t.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.Submit();

    ShaderConstants c {};
    c.sizeX = W, c.sizeY = H;
    c.srcUvScaleX = c.srcUvScaleY = c.mvUvScaleX = c.mvUvScaleY = 1;
    c.enhUvScaleX = c.enhUvScaleY = c.fedUvScaleX = c.fedUvScaleY = 1;
    c.strength = 1;
    c.uiLow = 0.02f, c.uiHigh = 0.06f;
    c.encoding = 0;
    c.paperWhite = 1;
    c.colorStrength = cs;
    c.upMode = up;
    c.mvToUvX = 1.f / W, c.mvToUvY = 1.f / H;
    std::vector<double> ms, msHold, msAcc;
    int cur = 0, acur = 0;
    bool histValid = false;
    for (unsigned t = 0; t < frames; t++)
    {
        auto vs = ReadF(Nn(dir, "src", t), size_t(W) * H * 3, t > 0);
        if (vs.empty())
            break;
        g.Upload(src.Get(), vs, 3);
        // cadence: the pair fed D frames ago, when that frame was a fed one
        const bool arrive = !period || (t >= delay && (t - delay) % period == 0);
        if (arrive)
        {
            const unsigned s = period ? t - delay : t;
            g.Upload(enh.Get(), ReadF(Nn(dir, "enh", s), size_t(w) * h * 3), 3);
            g.Upload(fed.Get(), ReadF(Nn(dir, "fed", s), size_t(w) * h * 3), 3);
        }
        bool haveMv = false;
        if (stab)
        {
            auto mv = ReadF(Nn(dir, "mv", t), size_t(W) * H * 2, true);
            haveMv = !mv.empty();
            if (haveMv)
                g.Upload(mvt.Get(), mv, 2);
            c.histMode = (histValid ? 2u : 1u) | (haveMv ? 4u : 0u);
        }
        bool accValid = false;
        if (period && haveMv && t > 0)
        {
            // this frame -> the last fed frame before it; the chain starts again after a fed frame
            ShaderConstants k = c;
            k.cadence = (t - 1) % period != 0 ? 4u : 0u;
            ID3D12Resource* srvs[8] = { acc[acur].Get(), nullptr, nullptr, mvt.Get(), nullptr, nullptr, nullptr, nullptr };
            const DXGI_FORMAT sf[8] = { A16, F, F, DXGI_FORMAT_R32G32_FLOAT, F, F16, A16, A16 };
            bind(1, srvs, sf, acc[acur ^ 1].Get(), A16, nullptr);
            g.Barrier(acc[acur].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.Stamp(0);
            run(1, psoAcc.Get(), k);
            g.Stamp(1);
            g.Resolve();
            g.Barrier(acc[acur].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.Submit();
            msAcc.push_back(g.Ms());
            acur ^= 1;
            accValid = true;
        }
        if (period && histValid)
        {
            // the guard's check of this frame against the history as it stands
            ShaderConstants k = c;
            k.cadence = 0;
            k.histMode = haveMv ? 4u : 0u;
            ID3D12Resource* srvs[8] = { src.Get(), nullptr, nullptr, nullptr, nullptr, hist[cur].Get(), mvt.Get(), nullptr };
            const DXGI_FORMAT sf[8] = { F, F, F, F, F, F16, DXGI_FORMAT_R32G32_FLOAT, A16 };
            bind(1, srvs, sf, stat.Get(), F, nullptr);
            g.Barrier(stat.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.Barrier(hist[cur].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ID3D12DescriptorHeap* hs[] = { heap.Get() };
            g.list->SetDescriptorHeaps(1, hs);
            g.list->SetComputeRootSignature(rs.Get());
            g.list->SetPipelineState(psoCheck.Get());
            g.list->SetComputeRoot32BitConstants(0, sizeof k / 4, &k, 0);
            auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(10) * st;
            g.list->SetComputeRootDescriptorTable(1, gpu);
            g.list->SetComputeRootDescriptorTable(2, { gpu.ptr + UINT64(8) * st });
            g.Stamp(0);
            g.list->Dispatch((bw + 7) / 8, (bh + 7) / 8, 1);
            g.Stamp(1);
            g.Resolve();
            g.Barrier(hist[cur].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.Submit();
            const double ms1 = g.Ms();
            auto v = g.Download(stat.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 4);
            g.Barrier(stat.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            g.Submit();
            double n[4] = {};
            for (size_t i = 0; i < v.size(); i++)
                n[i & 3] += v[i];
            const double total = 16.0 * bw * bh;
            std::printf("check %02u: moving %.3f missed moved %.4f in place %.4f, changed in place %.4f of the frame (%.3f ms)\n", t,
                        n[0] / total, n[0] ? n[1] / n[0] : 0.0, n[0] ? n[2] / n[0] : 0.0, n[3] / total, ms1);
        }
        const bool hold = period && !arrive;
        if (period)
            c.cadence = (arrive ? 1u : 2u) | (arrive && accValid ? 8u : 0u);
        g.Barrier(dst.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (hold && !histValid)
        {
            // nothing to show yet: the frame as it is
            WriteF(Nn(dir, "cd", t), vs);
            g.Barrier(dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            g.Submit();
            continue;
        }
        if (stab)
            g.Barrier(hist[cur].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (accValid)
            g.Barrier(acc[acur].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        {
            ID3D12Resource* srvs[8] = { src.Get(), hold ? nullptr : enh.Get(), hold ? nullptr : fed.Get(), src.Get(), norm.Get(),
                                        hist[cur].Get(), mvt.Get(), accValid && arrive ? acc[acur].Get() : nullptr };
            const DXGI_FORMAT sf[8] = { F, F, F, F, F, F16, DXGI_FORMAT_R32G32_FLOAT, A16 };
            bind(0, srvs, sf, dst.Get(), F, stab ? hist[cur ^ 1].Get() : nullptr);
        }
        for (int r = 0; r < reps; r++)
        {
            g.Stamp(0);
            run(0, pso.Get(), c);
            g.Stamp(1);
            g.Resolve();
            g.Submit();
            if (r > 0)
                (hold ? msHold : ms).push_back(g.Ms());
        }
        WriteF(Nn(dir, period ? "cd" : stab ? "st" : "up", t), g.Download(dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        g.Barrier(dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (accValid)
            g.Barrier(acc[acur].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (stab)
        {
            g.Barrier(hist[cur].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cur ^= 1;
            histValid = true;
        }
        g.Submit();
    }
    Median(ms, entry);
    Median(msHold, "held frame");
    Median(msAcc, "Accumulate");
    return 0;
}

static int RunFsr(Gpu& g, const std::string& dir, const std::wstring& ffxDir, const char* want, unsigned frames,
                  float sharp, int reps, UINT W, UINT H, UINT w, UINT h)
{
    SetDllDirectoryW(ffxDir.c_str());
    HMODULE m = LoadLibraryExW((ffxDir + L"\\amd_fidelityfx_loader_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m)
        m = LoadLibraryExW((ffxDir + L"\\amd_fidelityfx_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m)
        Die("no FFX loader in the ffx dir");
    auto create = (PfnFfxCreateContext) GetProcAddress(m, "ffxCreateContext");
    auto destroy = (PfnFfxDestroyContext) GetProcAddress(m, "ffxDestroyContext");
    auto query = (PfnFfxQuery) GetProcAddress(m, "ffxQuery");
    auto dispatch = (PfnFfxDispatch) GetProcAddress(m, "ffxDispatch");
    if (!create || !destroy || !query || !dispatch)
        Die("FFX exports");

    uint64_t n = 16, ids[16] {};
    const char* names[16] {};
    ffxQueryDescGetVersions qv {};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    qv.device = g.dev.Get();
    qv.outputCount = &n;
    qv.versionIds = ids;
    qv.versionNames = names;
    if (query(nullptr, &qv.header) != 0)
        Die("version query");
    uint64_t pick = 0;
    for (uint64_t i = 0; i < n; i++)
    {
        std::printf("upscaler version %llu: %s\n", (unsigned long long) i, names[i] ? names[i] : "?");
        if (!pick && names[i] && std::strstr(names[i], want) == names[i])
            pick = ids[i];
    }
    if (!pick)
        Die("wanted version not offered");

    ffxOverrideVersion ov {};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = pick;
    ffxCreateBackendDX12Desc be {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.header.pNext = &ov.header;
    be.device = g.dev.Get();
    ffxCreateContextDescUpscale cd {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    cd.maxRenderSize = { w, h };
    cd.maxUpscaleSize = { W, H };
    cd.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    ffxContext ctx = nullptr;
    if (create(&ctx, &cd.header, nullptr) != 0)
        Die("ffxCreateContext");
    ffxQueryGetProviderVersion pv {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    query(&ctx, &pv.header);
    std::printf("provider: %s\n", pv.versionName ? pv.versionName : "?");

    const auto F4 = DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto col = g.Tex(w, h, F4, false), mv = g.Tex(w, h, DXGI_FORMAT_R32G32_FLOAT, false),
         dp = g.Tex(w, h, DXGI_FORMAT_R32_FLOAT, false), out = g.Tex(W, H, F4, true);
    std::vector<float> jit;
    if (FILE* f = std::fopen((dir + "/jitter.txt").c_str(), "r"))
    {
        float a, b;
        while (std::fscanf(f, "%f %f", &a, &b) == 2)
            jit.push_back(a), jit.push_back(b);
        std::fclose(f);
    }
    std::vector<double> ms;
    for (unsigned t = 0; t < frames; t++)
    {
        auto c = ReadF(Nn(dir, "col", t), size_t(w) * h * 3, t > 0);
        if (c.empty())
            break;
        g.Upload(col.Get(), c, 3);
        g.Upload(mv.Get(), ReadF(Nn(dir, "mv", t), size_t(w) * h * 2), 2);
        auto d = ReadF(Nn(dir, "depth", t), size_t(w) * h, true);
        if (d.empty())
            d.assign(size_t(w) * h, 0.5f); // no depth: a flat plane
        g.Upload(dp.Get(), d, 1);
        for (int r = 0; r < reps; r++)
        {
            ffxDispatchDescUpscale dd {};
            dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dd.commandList = g.list.Get();
            dd.color = ffxApiGetResourceDX12(col.Get(), FFX_API_RESOURCE_STATE_COMMON);
            dd.depth = ffxApiGetResourceDX12(dp.Get(), FFX_API_RESOURCE_STATE_COMMON);
            dd.motionVectors = ffxApiGetResourceDX12(mv.Get(), FFX_API_RESOURCE_STATE_COMMON);
            dd.output = ffxApiGetResourceDX12(out.Get(), FFX_API_RESOURCE_STATE_COMMON);
            dd.jitterOffset = { 2 * t < jit.size() ? jit[2 * t] : 0.f, 2 * t + 1 < jit.size() ? jit[2 * t + 1] : 0.f };
            dd.motionVectorScale = { 1.f, 1.f };
            dd.renderSize = { w, h };
            dd.upscaleSize = { W, H };
            dd.enableSharpening = sharp > 0;
            dd.sharpness = sharp;
            dd.frameTimeDelta = 16.7f;
            dd.preExposure = 1.f;
            dd.reset = t == 0 && r == 0;
            dd.cameraNear = 1e6f; // inverted infinite-ish, only for the flat default
            dd.cameraFar = 0.1f;
            dd.cameraFovAngleVertical = 1.0f;
            dd.viewSpaceToMetersFactor = 1.f;
            g.Stamp(0);
            if (dispatch(&ctx, &dd.header) != 0)
                Die("ffxDispatch");
            g.Stamp(1);
            g.Resolve();
            g.Submit();
            if (t > 0)
                ms.push_back(g.Ms());
            if (r + 1 < reps) // timing repeats re-run the same frame; keep the history of the last one only
                break;
        }
        WriteF(Nn(dir, "fsr", t), g.Download(out.Get(), D3D12_RESOURCE_STATE_COMMON));
    }
    destroy(&ctx, nullptr);
    Median(ms, "fsr dispatch");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 4)
        return std::fprintf(stderr, "usage: see source\n"), 2;
    std::string mode = argv[1], dir = argv[2];
    int bus = -1, reps = 20;
    unsigned frames = 64;
    float cs = 1, sharp = 0;
    uint32_t up = 0;
    bool stab = false;
    unsigned period = 0, delay = 1;
    std::string version = "3.1";
    for (int i = 4; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? argv[++i] : (Die("missing value"), ""); };
        if (a == "--bus")
            bus = std::atoi(next());
        else if (a == "--reps")
            reps = std::max(2, std::atoi(next()));
        else if (a == "--up")
            up = uint32_t(std::atoi(next()));
        else if (a == "--cs")
            cs = float(std::atof(next()));
        else if (a == "--frames")
            frames = unsigned(std::atoi(next()));
        else if (a == "--version")
            version = next();
        else if (a == "--stab")
            stab = true;
        else if (a == "--cadence")
        {
            if (std::sscanf(next(), "%u,%u", &period, &delay) < 1 || period < 1 || delay < 1 || delay > period)
                Die("--cadence P,D with 1 <= D <= P");
            stab = true;
        }
        else if (a == "--sharp")
            sharp = float(std::atof(next()));
    }
    UINT W = 3840, H = 2160, w = 1920, h = 1080;
    if (FILE* f = std::fopen((dir + "/size.txt").c_str(), "r"))
    {
        std::fscanf(f, "%u %u %u %u", &W, &H, &w, &h);
        std::fclose(f);
    }
    Gpu g;
    g.Init(bus);
    if (mode == "apply")
        return RunApply(g, dir, argv[3], reps, cs, up, frames, W, H, w, h, stab, period, delay);
    if (mode == "fsr")
    {
        std::wstring fd(argv[3], argv[3] + std::strlen(argv[3]));
        return RunFsr(g, dir, fd, version.c_str(), frames, sharp, 1, W, H, w, h);
    }
    return std::fprintf(stderr, "unknown mode\n"), 2;
}
