#include "common.h"
#include <random>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

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
    q.PrivateDriverDataSize = sizeof addr;
    int bus = D3DKMTQueryAdapterInfo(&q) == 0 && addr.BusNumber != 0xFFFFFFFF ? int(addr.BusNumber) : -1;
    D3DKMT_CLOSEADAPTER close { open.hAdapter };
    D3DKMTCloseAdapter(&close);
    return bus;
}

bool PickAdapters(Adapter& r, Adapter& d)
{
    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
        return false;
    bool haveR = false, haveD = false;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapters1(i, &a) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 ad {};
        a->GetDesc1(&ad);
        if ((ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || ad.VendorId != 0x1002)
            continue;
        UINT outs = 0;
        for (ComPtr<IDXGIOutput> o; a->EnumOutputs(outs, &o) == S_OK; o.Reset())
            ++outs;
        Adapter x { a, ad.AdapterLuid, PciBus(ad.AdapterLuid), outs, ad.Description };
        printf("adapter %u: bus %d, outputs %u, %ls\n", i, x.bus, outs, ad.Description);
        if (outs == 0 && !haveR)
            r = x, haveR = true;
        else if (outs > 0 && !haveD)
            d = x, haveD = true;
    }
    // Default-adapter view (what a nullptr adapter gets) and the GPU-preference order.
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_UNSPECIFIED, IID_PPV_ARGS(&a)) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 ad {};
        a->GetDesc1(&ad);
        printf("gpu-preference(unspecified) %u: bus %d\n", i, PciBus(ad.AdapterLuid));
    }
    return haveR && haveD;
}

bool D12::Init(IDXGIAdapter1* a)
{
    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
        return false;
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct));
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&copy));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&calloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, calloc.Get(), nullptr, IID_PPV_ARGS(&clist));
    list->Close();
    clist->Close();
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    direct->GetTimestampFrequency(&tsFreqDirect);
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 {};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof o3);
    copyTs = o3.CopyQueueTimestampQueriesSupported;
    if (copyTs)
        copy->GetTimestampFrequency(&tsFreqCopy);
    return true;
}

void D12::Begin(bool c)
{
    if (c)
        calloc->Reset(), clist->Reset(calloc.Get(), nullptr);
    else
        alloc->Reset(), list->Reset(alloc.Get(), nullptr);
}

void D12::Submit(bool c)
{
    ID3D12GraphicsCommandList* l = c ? clist.Get() : list.Get();
    l->Close();
    ID3D12CommandList* ls[] = { l };
    (c ? copy : direct)->ExecuteCommandLists(1, ls);
}

bool D12::WaitCpu(ID3D12Fence* f, uint64_t v, DWORD ms)
{
    if (f->GetCompletedValue() >= v)
        return true;
    f->SetEventOnCompletion(v, ev);
    return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0;
}

bool D12::Flush(bool c)
{
    (c ? copy : direct)->Signal(fence.Get(), ++fv);
    return WaitCpu(fence.Get(), fv);
}

D3D12_RESOURCE_DESC BufDesc(uint64_t size, D3D12_RESOURCE_FLAGS f)
{
    D3D12_RESOURCE_DESC b {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = size;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    b.Flags = f;
    return b;
}

D3D12_RESOURCE_DESC TexDesc(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS f)
{
    D3D12_RESOURCE_DESC t {};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = w;
    t.Height = h;
    t.DepthOrArraySize = t.MipLevels = 1;
    t.Format = fmt;
    t.SampleDesc.Count = 1;
    t.Flags = f;
    return t;
}

ComPtr<ID3D12Resource> Committed(ID3D12Device* dev, D3D12_HEAP_TYPE t, const D3D12_RESOURCE_DESC& d,
                                 D3D12_RESOURCE_STATES st, D3D12_HEAP_FLAGS hf, HRESULT* out)
{
    D3D12_HEAP_PROPERTIES hp { t };
    ComPtr<ID3D12Resource> r;
    HRESULT hr = dev->CreateCommittedResource(&hp, hf, &d, st, nullptr, IID_PPV_ARGS(&r));
    if (out)
        *out = hr;
    return r;
}

bool UploadBuffer(D12& d, ID3D12Resource* dst, const void* data, size_t size)
{
    auto up = Committed(d.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, BufDesc(size), D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up)
        return false;
    void* p = nullptr;
    up->Map(0, nullptr, &p);
    memcpy(p, data, size);
    up->Unmap(0, nullptr);
    d.Begin(true);
    d.clist->CopyBufferRegion(dst, 0, up.Get(), 0, size);
    d.Submit(true);
    return d.Flush(true);
}

bool UploadTexture(D12& d, ID3D12Resource* dst, const void* data, UINT rowBytes, UINT rows)
{
    D3D12_RESOURCE_DESC td = dst->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total;
    d.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto up = Committed(d.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, BufDesc(total), D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up)
        return false;
    uint8_t* p = nullptr;
    up->Map(0, nullptr, (void**) &p);
    for (UINT y = 0; y < rows; ++y)
        memcpy(p + fp.Offset + size_t(y) * fp.Footprint.RowPitch, (const uint8_t*) data + size_t(y) * rowBytes, rowBytes);
    up->Unmap(0, nullptr);
    d.Begin(true);
    D3D12_TEXTURE_COPY_LOCATION s { up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    s.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION t { dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    d.clist->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
    d.Submit(true);
    return d.Flush(true);
}

bool ReadbackBuffer(D12& d, ID3D12Resource* src, uint64_t offset, size_t size, std::vector<uint8_t>& out)
{
    auto rb = Committed(d.dev.Get(), D3D12_HEAP_TYPE_READBACK, BufDesc(size), D3D12_RESOURCE_STATE_COPY_DEST);
    if (!rb)
        return false;
    d.Begin(true);
    d.clist->CopyBufferRegion(rb.Get(), 0, src, offset, size);
    d.Submit(true);
    if (!d.Flush(true))
        return false;
    void* p = nullptr;
    D3D12_RANGE rr { 0, size };
    rb->Map(0, &rr, &p);
    out.assign((uint8_t*) p, (uint8_t*) p + size);
    rb->Unmap(0, nullptr);
    return true;
}

std::vector<uint8_t> Pattern(size_t size, uint32_t seed)
{
    std::vector<uint8_t> v(size);
    std::mt19937 g(seed);
    for (size_t i = 0; i + 4 <= size; i += 4)
    {
        uint32_t x = g();
        memcpy(&v[i], &x, 4);
    }
    return v;
}

size_t Mismatch(const uint8_t* a, const uint8_t* b, size_t n)
{
    size_t m = 0;
    for (size_t i = 0; i < n; ++i)
        m += a[i] != b[i];
    return m;
}

bool D11::Init(IDXGIAdapter1* a)
{
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1 };
    ComPtr<ID3D11Device> d;
    ComPtr<ID3D11DeviceContext> c;
    if (FAILED(D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &d, nullptr, &c)))
        return false;
    return SUCCEEDED(d.As(&dev)) && SUCCEEDED(c.As(&ctx));
}
