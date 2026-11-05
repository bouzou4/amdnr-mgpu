// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Offline check of the host's compute part (no window, no present): a recorded sequence (data prepared by
// prep_seq.py) is played through OpticalFlow + NrEngine::RunOwn on one adapter, in passes of N frames that start
// with a history reset; the last pass's answers and vectors are written for scoring.
// lmxxf_legacy_host.exe --selftest <in> <out> <off|of|zero|game> <model 0|1> <pci bus> <runtime> <modules> <weights>
//     [Key=v,...: EditStabilise, EditUpsample, NetworkSize, MultiPass]
#include "../../src/Nr.h"
#include "OpticalFlow.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winternl.h>
#include <d3dkmthk.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
int PciBus(LUID luid)
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
    q.PrivateDriverDataSize = sizeof(addr);
    int bus = -1;
    if (D3DKMTQueryAdapterInfo(&q) == 0)
        bus = (int) addr.BusNumber;
    D3DKMT_CLOSEADAPTER close { open.hAdapter };
    D3DKMTCloseAdapter(&close);
    return bus;
}

struct Gpu
{
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ID3D12GraphicsCommandList* Begin()
    {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        return list.Get();
    }
    void Submit()
    {
        list->Close();
        ID3D12CommandList* l[] = { list.Get() };
        q->ExecuteCommandLists(1, l);
        Idle();
    }
    void Idle()
    {
        q->Signal(fence.Get(), ++value);
        fence->SetEventOnCompletion(value, ev);
        WaitForSingleObject(ev, 10000);
    }
};

ComPtr<ID3D12Resource> Buffer(ID3D12Device* d, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp { type };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

ComPtr<ID3D12Resource> Tex2D(ID3D12Device* d, uint32_t w, uint32_t h, DXGI_FORMAT f)
{
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                               IID_PPV_ARGS(&r));
    return r;
}

void Transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

// Texture <-> tight CPU rows through one staging buffer
struct Staging
{
    ComPtr<ID3D12Resource> buf;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 row = 0, total = 0;
    void Init(ID3D12Device* d, ID3D12Resource* tex, D3D12_HEAP_TYPE type)
    {
        const auto desc = tex->GetDesc();
        UINT rows = 0;
        d->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row, &total);
        buf = Buffer(d, total, type,
                     type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST);
    }
};

void Upload(Gpu& g, Staging& s, ID3D12Resource* tex, const uint8_t* data)
{
    uint8_t* p = nullptr;
    s.buf->Map(0, nullptr, (void**) &p);
    for (UINT y = 0; y < s.fp.Footprint.Height; ++y)
        memcpy(p + s.fp.Offset + UINT64(y) * s.fp.Footprint.RowPitch, data + y * s.row, (size_t) s.row);
    s.buf->Unmap(0, nullptr);
    auto* l = g.Begin();
    Transition(l, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION src { s.buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    src.PlacedFootprint = s.fp;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(l, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    g.Submit();
}

bool Download(Gpu& g, Staging& s, ID3D12Resource* tex, D3D12_RESOURCE_STATES st, const std::wstring& path)
{
    auto* l = g.Begin();
    Transition(l, tex, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION dst { s.buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = s.fp;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(l, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    g.Submit();
    uint8_t* p = nullptr;
    if (FAILED(s.buf->Map(0, nullptr, (void**) &p)))
        return false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f)
    {
        for (UINT y = 0; y < s.fp.Footprint.Height; ++y)
            fwrite(p + s.fp.Offset + UINT64(y) * s.fp.Footprint.RowPitch, 1, (size_t) s.row, f);
        fclose(f);
    }
    D3D12_RANGE none { 0, 0 };
    s.buf->Unmap(0, &none);
    return f != nullptr;
}

std::vector<uint8_t> ReadAll(const std::wstring& path)
{
    std::vector<uint8_t> v;
    if (FILE* f = _wfopen(path.c_str(), L"rb"))
    {
        fseek(f, 0, SEEK_END);
        v.resize((size_t) _ftelli64(f));
        fseek(f, 0, SEEK_SET);
        fread(v.data(), 1, v.size(), f);
        fclose(f);
    }
    return v;
}
} // namespace

int SelfTest(int argc, wchar_t** argv)
{
    if (argc < 10)
        return 2;
    const std::wstring in = argv[2], out = argv[3], mode = argv[4];
    const int model = _wtoi(argv[5]), bus = _wtoi(argv[6]);
    uint32_t n = 0, w = 0, h = 0;
    if (FILE* f = _wfopen((in + L"\\meta.txt").c_str(), L"r"))
        fscanf(f, "%u %u %u", &n, &w, &h), fclose(f);
    const auto frames = ReadAll(in + L"\\frames.bin");
    const auto mvs = ReadAll(in + L"\\game_mv.bin");
    if (n == 0 || frames.size() != size_t(n) * w * h * 8 || mvs.size() != size_t(n) * w * h * 8)
    {
        Log(OptiLog_Error, "selftest: bad input (%u frames %ux%u)", n, w, h);
        return 3;
    }

    Gpu g;
    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) == S_OK; ++i, adapter.Reset())
    {
        DXGI_ADAPTER_DESC1 d {};
        adapter->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && PciBus(d.AdapterLuid) == bus)
        {
            Log(OptiLog_Info, "selftest: adapter %u '%ls' bus %d", i, d.Description, bus);
            break;
        }
    }
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (!adapter || FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev))) ||
        FAILED(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q))))
    {
        Log(OptiLog_Error, "selftest: no device on bus %d", bus);
        return 4;
    }
    g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc));
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr, IID_PPV_ARGS(&g.list));
    g.list->Close();
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));

    auto src = Tex2D(g.dev.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto dst = Tex2D(g.dev.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto gmv = Tex2D(g.dev.Get(), w, h, DXGI_FORMAT_R32G32_FLOAT);
    Staging upSrc, upMv, down, downMv;
    upSrc.Init(g.dev.Get(), src.Get(), D3D12_HEAP_TYPE_UPLOAD);
    upMv.Init(g.dev.Get(), gmv.Get(), D3D12_HEAP_TYPE_UPLOAD);
    down.Init(g.dev.Get(), dst.Get(), D3D12_HEAP_TYPE_READBACK);

    OpticalFlow of;
    if (!of.Init(g.dev.Get(), g.q.Get()))
        return 5;
    NrSettings s;
    s.placement = Placement::Terminal;
    s.model = model;
    s.temporal = mode != L"off";
    s.meter = false;
    for (std::wstring kv = argc > 10 ? argv[10] : L""; !kv.empty();)
    {
        const size_t c = kv.find(L','), e = kv.find(L'=');
        const std::wstring k = kv.substr(0, e);
        const int v = e == std::wstring::npos ? 0 : _wtoi(kv.c_str() + e + 1);
        if (k == L"EditStabilise")
            s.stabilise = v != 0;
        else if (k == L"EditUpsample")
            s.upsample = v;
        else if (k == L"NetworkSize")
            s.size = (NetworkSize) v;
        else if (k == L"MultiPass")
            s.passes = v;
        kv = c == std::wstring::npos ? L"" : kv.substr(c + 1);
    }
    Log(OptiLog_Info, "selftest: stabilise %d upsample %d size %d passes %d", s.stabilise, s.upsample, (int) s.size,
        s.passes);
    auto& nr = Engine();
    nr.name = L"selftest";
    nr.Configure(s);
    RuntimePaths paths;
    paths.candidates.push_back({ argv[7], argv[8], argv[9], "command line" });
    if (!nr.LoadRuntime(paths))
        return 6;

    auto run = [&](uint32_t k, bool reset, bool keep, uint32_t pass) -> bool
    {
        Upload(g, upSrc, src.Get(), frames.data() + size_t(k) * w * h * 8);
        Upload(g, upMv, gmv.Get(), mvs.data() + size_t(k) * w * h * 8);
        if (reset)
            of.Invalidate();
        auto* l = g.Begin();
        const bool haveOf = of.Run(l, src.Get(), D3D12_RESOURCE_STATE_COMMON, mode == L"zero", true);
        g.Submit();
        NrEngine::OwnFrame f;
        f.src = src.Get();
        f.dst = dst.Get();
        f.srcState = f.dstState = D3D12_RESOURCE_STATE_COMMON;
        f.encoding = 0;
        f.reset = reset;
        f.waitAnswer = true;
        if (mode == L"game" && !reset)
        {
            f.motion = gmv.Get();
            f.motionState = D3D12_RESOURCE_STATE_COMMON;
            f.motionW = w;
            f.motionH = h;
        }
        else if (mode != L"off" && haveOf)
        {
            f.motion = of.Vectors();
            f.motionW = of.Width();
            f.motionH = of.Height();
        }
        const bool ok = nr.RunOwn(g.dev.Get(), g.q.Get(), f);
        g.Idle();
        nr.SettleOwn();
        if (keep && ok)
        {
            wchar_t name[64];
            swprintf_s(name, L"\\res_%02u.raw", k);
            Download(g, down, dst.Get(), D3D12_RESOURCE_STATE_COMMON, out + name);
            if (haveOf)
            {
                if (!downMv.buf)
                    downMv.Init(g.dev.Get(), of.Vectors(), D3D12_HEAP_TYPE_READBACK);
                swprintf_s(name, L"\\of_%02u.raw", k);
                Download(g, downMv, of.Vectors(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, out + name);
            }
        }
        return ok;
    };

    // Build the session: frame 0 until NR answers
    for (int i = 0; i < 600 && !run(0, true, false, 0); ++i)
        Sleep(50);
    const uint32_t passes = 4;
    for (uint32_t p = 0; p < passes; ++p)
        for (uint32_t k = 0; k < n; ++k)
            if (!run(k, k == 0, p == passes - 1, p))
                Log(OptiLog_Warn, "selftest: frame %u of pass %u not processed", k, p);
    const auto st = nr.Stats();
    if (FILE* f = _wfopen((out + L"\\selftest.txt").c_str(), L"w"))
    {
        fprintf(f, "mode %ls model %d frames %u %ux%u of %ux%u\nhip_ms %.3f net_ms %.3f of_ms %.3f temporal %d\n",
                mode.c_str(), model, n, w, h, of.Width(), of.Height(), st.hipMs, st.networkMs, of.GpuMs(), st.temporal);
        fclose(f);
    }
    Log(OptiLog_Info, "selftest done: hip %.2f ms, net %.2f ms, of %.3f ms, temporal %d, frames run %llu", st.hipMs,
        st.networkMs, of.GpuMs(), st.temporal, st.framesRun);
    nr.Quiesce();
    return 0;
}
