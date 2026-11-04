// D3D11 and host-memory probes: what can cross from a D3D11 device on the render card (R) to D3D12 on the
// display card (D), and what each hop costs.
#include "common.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace {

constexpr UINT W = 3840, H = 2160;
constexpr DXGI_FORMAT FMT = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr UINT PITCH = W * 4; // 15360, already 256-aligned: footprint == tight rows
constexpr size_t BYTES = size_t(PITCH) * H;

HWND OffscreenWindow()
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"xapi_probe";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"xapi_probe", WS_POPUP, -4000,
                             -4000, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

// 10-bit values per channel, alpha 2 bits: every bit pattern is a valid R10G10B10A2 texel.
std::vector<uint8_t> TexPattern(uint32_t seed) { return Pattern(BYTES, seed); }

struct Ts12
{
    ComPtr<ID3D12QueryHeap> qh;
    ComPtr<ID3D12Resource> rb;
    uint64_t freq = 0;
    bool Init(D12& d, bool copyQ)
    {
        D3D12_QUERY_HEAP_DESC q {};
        q.Type = copyQ ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        q.Count = 2;
        if (FAILED(d.dev->CreateQueryHeap(&q, IID_PPV_ARGS(&qh))))
            return false;
        rb = Committed(d.dev.Get(), D3D12_HEAP_TYPE_READBACK, BufDesc(16), D3D12_RESOURCE_STATE_COPY_DEST);
        freq = copyQ ? d.tsFreqCopy : d.tsFreqDirect;
        return rb != nullptr;
    }
    void Begin(ID3D12GraphicsCommandList* l) { l->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0); }
    void End(ID3D12GraphicsCommandList* l)
    {
        l->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        l->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, rb.Get(), 0);
    }
    double Ms()
    {
        uint64_t* p = nullptr;
        D3D12_RANGE r { 0, 16 };
        rb->Map(0, &r, (void**) &p);
        double ms = double(p[1] - p[0]) * 1000.0 / double(freq);
        rb->Unmap(0, nullptr);
        return ms;
    }
};

struct Ts11
{
    ComPtr<ID3D11Query> dis, t0, t1;
    bool Init(ID3D11Device* dev)
    {
        D3D11_QUERY_DESC q { D3D11_QUERY_TIMESTAMP_DISJOINT };
        dev->CreateQuery(&q, &dis);
        q.Query = D3D11_QUERY_TIMESTAMP;
        dev->CreateQuery(&q, &t0);
        dev->CreateQuery(&q, &t1);
        return dis && t0 && t1;
    }
    void Begin(ID3D11DeviceContext* c) { c->Begin(dis.Get()), c->End(t0.Get()); }
    void End(ID3D11DeviceContext* c) { c->End(t1.Get()), c->End(dis.Get()); }
    double Ms(ID3D11DeviceContext* c)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj {};
        while (c->GetData(dis.Get(), &dj, sizeof dj, 0) == S_FALSE)
            Sleep(0);
        UINT64 a = 0, b = 0;
        while (c->GetData(t0.Get(), &a, sizeof a, 0) == S_FALSE)
            Sleep(0);
        while (c->GetData(t1.Get(), &b, sizeof b, 0) == S_FALSE)
            Sleep(0);
        return dj.Disjoint ? -1.0 : double(b - a) * 1000.0 / double(dj.Frequency);
    }
};

double Median(std::vector<double> v)
{
    if (v.empty())
        return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::string MsLine(const char* what, double ms, size_t bytes)
{
    char b[160];
    snprintf(b, sizeof b, "%s %.3f ms (%.1f GB/s, %.4f ms/MB)", what, ms, double(bytes) / (ms * 1e6),
             ms / (double(bytes) / 1e6));
    return b;
}

// D3D11 texture with initial data, as the game's back buffer would be.
ComPtr<ID3D11Texture2D> Tex11(ID3D11Device* dev, const void* data, UINT misc, UINT bind)
{
    D3D11_TEXTURE2D_DESC t {};
    t.Width = W;
    t.Height = H;
    t.MipLevels = t.ArraySize = 1;
    t.Format = FMT;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = bind;
    t.MiscFlags = misc;
    D3D11_SUBRESOURCE_DATA s { data, PITCH, 0 };
    ComPtr<ID3D11Texture2D> tex;
    dev->CreateTexture2D(&t, data ? &s : nullptr, &tex);
    return tex;
}

const char* kPackCs = R"(
Texture2D<float4> src : register(t0);
RWBuffer<uint> dst : register(u0);
cbuffer C : register(b0) { uint pitchWords; uint w; uint h; uint pad; };
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    float4 v = src.Load(int3(id.xy, 0));
    uint4 q = uint4(round(saturate(v) * float4(1023, 1023, 1023, 3)));
    dst[id.y * pitchWords + id.x] = q.r | (q.g << 10) | (q.b << 20) | (q.a << 30);
}
)";

struct PackCs
{
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer> cb;
    bool Init(ID3D11Device* dev, std::string& err)
    {
        ComPtr<ID3DBlob> code, e;
        if (FAILED(D3DCompile(kPackCs, strlen(kPackCs), "pack", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &e)))
        {
            err = e ? (const char*) e->GetBufferPointer() : "compile";
            return false;
        }
        if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)))
            return false;
        UINT c[4] = { PITCH / 4, W, H, 0 };
        D3D11_BUFFER_DESC bd { 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER };
        D3D11_SUBRESOURCE_DATA s { c };
        return SUCCEEDED(dev->CreateBuffer(&bd, &s, &cb));
    }
    void Run(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, ID3D11UnorderedAccessView* uav)
    {
        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        ID3D11UnorderedAccessView* nu = nullptr;
        ID3D11ShaderResourceView* ns = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
        ctx->CSSetShaderResources(0, 1, &ns);
    }
};

// Cross-adapter committed buffer on R (opened on D) plus a cross-adapter fence created on R.
struct XBuf
{
    ComPtr<ID3D12Resource> r, d;
    HANDLE h = nullptr;
    ComPtr<ID3D12Fence> fr, fd;
    HANDLE fh = nullptr;
    HRESULT hr = S_OK;
    bool Init(D12& R, D12& D, size_t size, D3D12_RESOURCE_FLAGS extra)
    {
        r = Committed(R.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, BufDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | extra),
                      D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, &hr);
        if (!r || FAILED(hr = R.dev->CreateSharedHandle(r.Get(), nullptr, GENERIC_ALL, nullptr, &h)) ||
            FAILED(hr = D.dev->OpenSharedHandle(h, IID_PPV_ARGS(&d))))
            return false;
        if (FAILED(hr = R.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                           IID_PPV_ARGS(&fr))) ||
            FAILED(hr = R.dev->CreateSharedHandle(fr.Get(), nullptr, GENERIC_ALL, nullptr, &fh)) ||
            FAILED(hr = D.dev->OpenSharedHandle(fh, IID_PPV_ARGS(&fd))))
            return false;
        return true;
    }
    ~XBuf()
    {
        if (h)
            CloseHandle(h);
        if (fh)
            CloseHandle(fh);
    }
};

void FunctionalD3D11(const Adapter& ra, const Adapter& da, D12& R, D12& D)
{
    D11 r11, d11;
    if (!r11.Init(ra.a.Get()) || !d11.Init(da.a.Get()))
    {
        Result("d3d11.devices", false, "D3D11CreateDevice failed");
        return;
    }
    Result("d3d11.devices", true, "D3D11 FL 11_1 on both cards");

    // Default adapter of a nullptr D3D11 / D3D12 device (the Windows GPU preference decides it).
    {
        ComPtr<ID3D11Device> dd;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dd, nullptr, nullptr);
        ComPtr<IDXGIDevice> xd;
        ComPtr<IDXGIAdapter> xa;
        DXGI_ADAPTER_DESC ad {};
        if (dd && SUCCEEDED(dd.As(&xd)) && SUCCEEDED(xd->GetAdapter(&xa)))
            xa->GetDesc(&ad);
        ComPtr<ID3D12Device> d12;
        D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12));
        Info("default_adapter", "D3D11(nullptr) bus " + std::to_string(PciBus(ad.AdapterLuid)) + ", D3D12(nullptr) bus " +
                                    std::to_string(d12 ? PciBus(d12->GetAdapterLuid()) : -1));
    }

    auto pat = TexPattern(1);

    // 1. D3D11 NT-shared texture on R, opened by D3D11 / D3D12 on D and by D3D12 on R.
    {
        auto t = Tex11(r11.dev.Get(), pat.data(), D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ComPtr<IDXGIResource1> xr;
        HANDLE h = nullptr;
        HRESULT hr = t ? t.As(&xr) : E_FAIL;
        if (SUCCEEDED(hr))
            hr = xr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
        if (FAILED(hr))
        {
            Result("d3d11.shared_tex.create", false, Hr(hr));
        }
        else
        {
            ComPtr<ID3D11Texture2D> o11;
            HRESULT a = d11.dev->OpenSharedResource1(h, IID_PPV_ARGS(&o11));
            Result("d3d11.shared_tex.open_d3d11_other", SUCCEEDED(a), Hr(a) + " (D3D11 on D opens R's texture)");
            ComPtr<ID3D12Resource> o12;
            HRESULT b = D.dev->OpenSharedHandle(h, IID_PPV_ARGS(&o12));
            Result("d3d11.shared_tex.open_d3d12_other", SUCCEEDED(b), Hr(b) + " (D3D12 on D opens R's texture)");
            ComPtr<ID3D12Resource> s12;
            HRESULT c = R.dev->OpenSharedHandle(h, IID_PPV_ARGS(&s12));
            Result("d3d11.shared_tex.open_d3d12_same", SUCCEEDED(c), Hr(c) + " (D3D12 on R, OptiScaler's bridge)");
            CloseHandle(h);
        }
    }

    // 2. Cross-adapter fences: D3D12 ones (made on R / on D) opened in D3D11 on R, and a D3D11 one opened on D.
    ComPtr<ID3D11Fence> f11;
    XBuf xb;
    bool xok = xb.Init(R, D, BYTES, D3D12_RESOURCE_FLAG_NONE);
    Result("d3d12.xadapter_committed_buffer", xok,
           Hr(xb.hr) + " (committed SHARED_CROSS_ADAPTER buffer; docs allow committed only for row-major textures)");
    for (int creator = 0; creator < 2; ++creator)
    {
        D12& C = creator == 0 ? R : D;
        ComPtr<ID3D12Fence> fc;
        HANDLE h = nullptr;
        ComPtr<ID3D11Fence> o;
        HRESULT hr = C.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&fc));
        if (SUCCEEDED(hr))
            hr = C.dev->CreateSharedHandle(fc.Get(), nullptr, GENERIC_ALL, nullptr, &h);
        if (SUCCEEDED(hr))
            hr = r11.dev->OpenSharedFence(h, IID_PPV_ARGS(&o));
        std::string det = Hr(hr);
        if (o)
        {
            // D3D11 on R signals, the D3D12 fence on D sees it.
            ComPtr<ID3D12Fence> onD = fc;
            if (creator == 0)
                D.dev->OpenSharedHandle(h, IID_PPV_ARGS(&onD));
            r11.ctx->Signal(o.Get(), 5);
            r11.ctx->Flush();
            bool seen = onD && D.WaitCpu(onD.Get(), 5, 2000);
            det += std::string(", D3D11 Signal(5) seen on D: ") + (seen ? "yes" : "no");
            if (creator == 0)
                f11 = o;
        }
        Result(creator == 0 ? "d3d11.open_d3d12_xfence_made_on_R" : "d3d11.open_d3d12_xfence_made_on_D", SUCCEEDED(hr), det);
        if (h)
            CloseHandle(h);
    }
    {
        ComPtr<ID3D11Fence> f;
        HANDLE h = nullptr;
        ComPtr<ID3D12Fence> onD;
        HRESULT hr = r11.dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED | D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&f));
        if (SUCCEEDED(hr))
            hr = f->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
        if (SUCCEEDED(hr))
            hr = D.dev->OpenSharedHandle(h, IID_PPV_ARGS(&onD));
        std::string det = Hr(hr);
        if (onD)
        {
            r11.ctx->Signal(f.Get(), 9);
            r11.ctx->Flush();
            det += std::string(", D3D11 Signal(9) seen on D: ") + (D.WaitCpu(onD.Get(), 9, 2000) ? "yes" : "no");
        }
        Result("d3d11.xfence_opened_on_D", SUCCEEDED(hr), det);
        if (h)
            CloseHandle(h);
    }

    // 3. The cross-adapter buffer opened as an ID3D11Buffer on R.
    ComPtr<ID3D11Buffer> b11;
    if (xok)
    {
        HRESULT hr = r11.dev->OpenSharedResource1(xb.h, IID_PPV_ARGS(&b11));
        std::string det = Hr(hr);
        if (b11)
        {
            D3D11_BUFFER_DESC bd {};
            b11->GetDesc(&bd);
            char x[128];
            snprintf(x, sizeof x, " ByteWidth %u Bind 0x%X Misc 0x%X", bd.ByteWidth, bd.BindFlags, bd.MiscFlags);
            det += x;
        }
        Result("d3d11.open_d3d12_xadapter_buffer", SUCCEEDED(hr), det);
    }

    // 4. D3D11 writes the cross-adapter buffer (CopyResource), signals the shared fence, D reads it.
    if (b11 && f11)
    {
        D3D11_BUFFER_DESC bd {};
        b11->GetDesc(&bd);
        D3D11_BUFFER_DESC sd { bd.ByteWidth, D3D11_USAGE_DEFAULT, 0 };
        D3D11_SUBRESOURCE_DATA s { pat.data() };
        ComPtr<ID3D11Buffer> src;
        r11.dev->CreateBuffer(&sd, &s, &src);
        r11.ctx->CopyResource(b11.Get(), src.Get());
        r11.ctx->Signal(f11.Get(), 1);
        r11.ctx->Flush();
        D.copy->Wait(xb.fd.Get(), 1);
        std::vector<uint8_t> out;
        bool ok = ReadbackBuffer(D, xb.d.Get(), 0, BYTES, out);
        size_t mm = ok ? Mismatch(out.data(), pat.data(), BYTES) : BYTES;
        Result("d3d11.write_xbuffer_read_on_D", ok && mm == 0,
               "D3D11 CopyResource into the D3D12 cross-adapter buffer, D3D11 Signal -> D Wait; mismatched bytes " +
                   std::to_string(mm));
    }

    // 5. D3D11 compute packs a texture straight into the cross-adapter buffer (typed UAV).
    if (b11 && f11)
    {
        std::string err;
        PackCs pk;
        ComPtr<ID3D11UnorderedAccessView> uav;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = UINT(BYTES / 4);
        HRESULT hr = r11.dev->CreateUnorderedAccessView(b11.Get(), &ud, &uav);
        if (!pk.Init(r11.dev.Get(), err) || FAILED(hr))
        {
            Result("d3d11.cs_pack_into_xbuffer", false, "UAV " + Hr(hr) + " " + err);
        }
        else
        {
            auto pat2 = TexPattern(2);
            auto tex = Tex11(r11.dev.Get(), pat2.data(), 0, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
            ComPtr<ID3D11ShaderResourceView> srv;
            r11.dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
            pk.Run(r11.ctx.Get(), srv.Get(), uav.Get());
            r11.ctx->Signal(f11.Get(), 2);
            r11.ctx->Flush();
            D.copy->Wait(xb.fd.Get(), 2);
            std::vector<uint8_t> out;
            bool ok = ReadbackBuffer(D, xb.d.Get(), 0, BYTES, out);
            size_t mm = ok ? Mismatch(out.data(), pat2.data(), BYTES) : BYTES;
            Result("d3d11.cs_pack_into_xbuffer", ok && mm == 0,
                   "R10G10B10A2 texture -> typed UAV over the cross-adapter buffer -> D; mismatched bytes " +
                       std::to_string(mm));
        }
    }

    // 6. D3D11 swapchain from the render card on a window of the display card's desktop (Windows' own
    //    cross-adapter present), flip discard + tearing.
    {
        HWND wnd = OffscreenWindow();
        ComPtr<IDXGIFactory2> f;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
        DXGI_SWAP_CHAIN_DESC1 sd {};
        sd.Width = 256;
        sd.Height = 256;
        sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        ComPtr<IDXGISwapChain1> sc;
        HRESULT hr = f->CreateSwapChainForHwnd(r11.dev.Get(), wnd, &sd, nullptr, nullptr, &sc);
        std::string det = "create " + Hr(hr);
        if (sc)
        {
            HRESULT last = S_OK;
            int okc = 0;
            for (int i = 0; i < 30; ++i)
            {
                ComPtr<ID3D11Texture2D> bb;
                sc->GetBuffer(0, IID_PPV_ARGS(&bb));
                ComPtr<ID3D11RenderTargetView> rtv;
                r11.dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
                float c[4] = { float(i & 1), 0.2f, 0.4f, 1 };
                r11.ctx->ClearRenderTargetView(rtv.Get(), c);
                last = sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
                okc += SUCCEEDED(last);
            }
            ComPtr<IDXGIOutput> out;
            HRESULT go = sc->GetContainingOutput(&out);
            det += ", presents ok " + std::to_string(okc) + "/30, last " + Hr(last) + ", GetContainingOutput " + Hr(go);
        }
        Result("d3d11.swapchain_from_render_card", SUCCEEDED(hr), det);
        sc.Reset();
        DestroyWindow(wnd);
    }
}

void FunctionalHost(D12& R, D12& D)
{
    // ID3D12Device3::OpenExistingHeapFromAddress on both cards over the same pages.
    const size_t size = 64ull << 20;
    void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ComPtr<ID3D12Heap> hr_, hd_;
    HRESULT a = R.dev->OpenExistingHeapFromAddress(p, IID_PPV_ARGS(&hr_));
    HRESULT b = D.dev->OpenExistingHeapFromAddress(p, IID_PPV_ARGS(&hd_));
    std::string det = "R " + Hr(a) + ", D " + Hr(b);
    if (hr_)
    {
        auto hd = hr_->GetDesc();
        char x[160];
        snprintf(x, sizeof x, "; heap type %d cpu %d pool %d flags 0x%X size %llu", hd.Properties.Type,
                 hd.Properties.CPUPageProperty, hd.Properties.MemoryPoolPreference, hd.Flags, hd.SizeInBytes);
        det += x;
    }
    Result("host.open_existing_heap_both", SUCCEEDED(a) && SUCCEEDED(b), det);
    if (hr_ && hd_)
    {
        ComPtr<ID3D12Resource> br, bd;
        HRESULT c = S_OK, e = S_OK;
        for (D3D12_RESOURCE_FLAGS fl : { D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_FLAG_NONE })
        {
            auto desc = BufDesc(size, fl);
            c = R.dev->CreatePlacedResource(hr_.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&br));
            e = D.dev->CreatePlacedResource(hd_.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bd));
            if (br && bd)
                break;
        }
        if (!br || !bd)
        {
            Result("host.placed_buffers", false, "R " + Hr(c) + ", D " + Hr(e));
        }
        else
        {
            auto pat = Pattern(size, 7);
            auto src = Committed(R.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, BufDesc(size), D3D12_RESOURCE_STATE_COMMON);
            UploadBuffer(R, src.Get(), pat.data(), size);
            R.Begin(true);
            R.clist->CopyBufferRegion(br.Get(), 0, src.Get(), 0, size);
            R.Submit(true);
            R.Flush(true);
            bool cpuSees = memcmp(p, pat.data(), size) == 0;
            std::vector<uint8_t> out;
            bool ok = ReadbackBuffer(D, bd.Get(), 0, size, out);
            size_t mm = ok ? Mismatch(out.data(), pat.data(), size) : size;
            Result("host.R_writes_D_reads", ok && mm == 0,
                   "R copy -> host pages -> D copy; mismatched bytes " + std::to_string(mm) + ", CPU sees R's write " +
                       (cpuSees ? "yes" : "no"));
        }
    }
    hr_.Reset();
    hd_.Reset();
    VirtualFree(p, 0, MEM_RELEASE);
}

void Bench(const Adapter& ra, D12& R, D12& D)
{
    const bool rCopyTs = R.copyTs, dCopyTs = D.copyTs;
    auto pat = TexPattern(3);
    const int N = 30;

    // Display-side destination texture and ingest timer.
    auto dTex = Committed(D.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, TexDesc(W, H, FMT), D3D12_RESOURCE_STATE_COMMON);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_RESOURCE_DESC td = TexDesc(W, H, FMT);
    R.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
    Ts12 tsR, tsD;
    tsR.Init(R, rCopyTs);
    tsD.Init(D, dCopyTs);
    auto rList = [&](bool) { return rCopyTs ? R.clist.Get() : R.list.Get(); };
    auto dList = [&]() { return dCopyTs ? D.clist.Get() : D.list.Get(); };

    auto ingest = [&](ID3D12Resource* ringOnD) {
        D.Begin(dCopyTs);
        tsD.Begin(dList());
        D3D12_TEXTURE_COPY_LOCATION s { ringOnD, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        s.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION t { dTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dList()->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
        tsD.End(dList());
        D.Submit(dCopyTs);
        D.Flush(dCopyTs);
        return tsD.Ms();
    };
    auto verifyD = [&](const char* name) {
        // dTex -> buffer -> readback, compared with the source pattern.
        auto tmp = Committed(D.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, BufDesc(BYTES), D3D12_RESOURCE_STATE_COMMON);
        D.Begin(true);
        D3D12_TEXTURE_COPY_LOCATION s { dTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION t { tmp.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        t.PlacedFootprint = fp;
        D.clist->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
        D.Submit(true);
        D.Flush(true);
        std::vector<uint8_t> out;
        ReadbackBuffer(D, tmp.Get(), 0, BYTES, out);
        size_t mm = Mismatch(out.data(), pat.data(), BYTES);
        Result(name, mm == 0, "end-to-end mismatched bytes " + std::to_string(mm));
    };

    // Ring heap as the plugin makes it: created on D, opened on R, placed buffers on both.
    D3D12_HEAP_DESC hd {};
    hd.SizeInBytes = (BYTES + 65535) & ~size_t(65535);
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    ComPtr<ID3D12Heap> heapD, heapR;
    HANDLE h = nullptr;
    D.dev->CreateHeap(&hd, IID_PPV_ARGS(&heapD));
    D.dev->CreateSharedHandle(heapD.Get(), nullptr, GENERIC_ALL, nullptr, &h);
    R.dev->OpenSharedHandle(h, IID_PPV_ARGS(&heapR));
    CloseHandle(h);
    ComPtr<ID3D12Resource> ringR, ringD;
    auto bdesc = BufDesc(hd.SizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    R.dev->CreatePlacedResource(heapR.Get(), 0, &bdesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ringR));
    D.dev->CreatePlacedResource(heapD.Get(), 0, &bdesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ringD));

    auto sendFrom = [&](ID3D12Resource* srcTex, ID3D12Resource* ring) {
        R.Begin(rCopyTs);
        auto l = rList(true);
        tsR.Begin(l);
        D3D12_TEXTURE_COPY_LOCATION s { srcTex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION t { ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        t.PlacedFootprint = fp;
        l->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
        tsR.End(l);
        R.Submit(rCopyTs);
        R.Flush(rCopyTs);
        return tsR.Ms();
    };

    // B0: D3D12 game texture -> ring (today's path).
    {
        auto rTex = Committed(R.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, TexDesc(W, H, FMT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                              D3D12_RESOURCE_STATE_COMMON);
        UploadTexture(R, rTex.Get(), pat.data(), PITCH, H);
        std::vector<double> s, g;
        for (int i = 0; i < N; ++i)
            s.push_back(sendFrom(rTex.Get(), ringR.Get())), g.push_back(ingest(ringD.Get()));
        Result("bench.d3d12_send", true, MsLine("R tex->ring", Median(s), BYTES) + "; " + MsLine("D ingest", Median(g), BYTES));
        verifyD("bench.d3d12_verify");
    }

    // B1: D3D11 back buffer -> NT-shared texture (D3D11 CopyResource) -> D3D12 on R -> ring (OptiScaler's bridge).
    D11 r11;
    if (r11.Init(ra.a.Get()))
    {
        auto bb = Tex11(r11.dev.Get(), pat.data(), 0, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        auto sh = Tex11(r11.dev.Get(), nullptr, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ComPtr<IDXGIResource1> xr;
        sh.As(&xr);
        HANDLE th = nullptr;
        xr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &th);
        ComPtr<ID3D12Resource> sh12;
        R.dev->OpenSharedHandle(th, IID_PPV_ARGS(&sh12));
        CloseHandle(th);
        ComPtr<ID3D11Fence> f11;
        r11.dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f11));
        HANDLE fh = nullptr;
        f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh);
        ComPtr<ID3D12Fence> f12;
        R.dev->OpenSharedHandle(fh, IID_PPV_ARGS(&f12));
        CloseHandle(fh);
        Ts11 t11;
        t11.Init(r11.dev.Get());
        std::vector<double> c11, s, g, wall;
        LARGE_INTEGER qf;
        QueryPerformanceFrequency(&qf);
        for (int i = 0; i < N; ++i)
        {
            LARGE_INTEGER a0, a1;
            QueryPerformanceCounter(&a0);
            t11.Begin(r11.ctx.Get());
            r11.ctx->CopyResource(sh.Get(), bb.Get());
            t11.End(r11.ctx.Get());
            r11.ctx->Signal(f11.Get(), i + 1);
            r11.ctx->Flush();
            (rCopyTs ? R.copy : R.direct)->Wait(f12.Get(), i + 1);
            s.push_back(sendFrom(sh12.Get(), ringR.Get()));
            QueryPerformanceCounter(&a1);
            wall.push_back(double(a1.QuadPart - a0.QuadPart) * 1000.0 / double(qf.QuadPart));
            c11.push_back(t11.Ms(r11.ctx.Get()));
            g.push_back(ingest(ringD.Get()));
        }
        Result("bench.d3d11_bridge_send", true,
               MsLine("D3D11 copy", Median(c11), BYTES) + "; " + MsLine("R shared->ring", Median(s), BYTES) + "; " +
                   MsLine("D ingest", Median(g), BYTES) + "; CPU wall D3D11 submit->ring done " +
                   std::to_string(Median(wall)) + " ms");
        verifyD("bench.d3d11_bridge_verify");

        // B2: D3D11 compute packs straight into a cross-adapter buffer opened in D3D11.
        XBuf xb;
        ComPtr<ID3D11Buffer> b11;
        ComPtr<ID3D11Fence> xf11;
        ComPtr<ID3D11UnorderedAccessView> uav;
        PackCs pk;
        std::string err;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = UINT(BYTES / 4);
        if (xb.Init(R, D, BYTES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
            SUCCEEDED(r11.dev->OpenSharedResource1(xb.h, IID_PPV_ARGS(&b11))) &&
            SUCCEEDED(r11.dev->OpenSharedFence(xb.fh, IID_PPV_ARGS(&xf11))) &&
            SUCCEEDED(r11.dev->CreateUnorderedAccessView(b11.Get(), &ud, &uav)) && pk.Init(r11.dev.Get(), err))
        {
            ComPtr<ID3D11ShaderResourceView> srv;
            r11.dev->CreateShaderResourceView(bb.Get(), nullptr, &srv);
            std::vector<double> c, g2;
            for (int i = 0; i < N; ++i)
            {
                t11.Begin(r11.ctx.Get());
                pk.Run(r11.ctx.Get(), srv.Get(), uav.Get());
                t11.End(r11.ctx.Get());
                r11.ctx->Signal(xf11.Get(), 100 + i);
                r11.ctx->Flush();
                (dCopyTs ? D.copy : D.direct)->Wait(xb.fd.Get(), 100 + i);
                c.push_back(t11.Ms(r11.ctx.Get()));
                g2.push_back(ingest(xb.d.Get()));
            }
            Result("bench.d3d11_cs_direct", true,
                   MsLine("D3D11 CS pack into xbuffer", Median(c), BYTES) + "; " + MsLine("D ingest", Median(g2), BYTES));
            verifyD("bench.d3d11_cs_direct_verify");
        }
        else
        {
            Result("bench.d3d11_cs_direct", false, "setup failed " + err);
        }
    }

    // B3: host pages opened on both cards (OpenExistingHeapFromAddress) as the ring.
    {
        const size_t size = (BYTES + 65535) & ~size_t(65535);
        void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ComPtr<ID3D12Heap> a, b;
        ComPtr<ID3D12Resource> ra2, rd2;
        auto bd = BufDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        if (SUCCEEDED(R.dev->OpenExistingHeapFromAddress(p, IID_PPV_ARGS(&a))) &&
            SUCCEEDED(D.dev->OpenExistingHeapFromAddress(p, IID_PPV_ARGS(&b))) &&
            SUCCEEDED(R.dev->CreatePlacedResource(a.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ra2))) &&
            SUCCEEDED(D.dev->CreatePlacedResource(b.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rd2))))
        {
            auto rTex = Committed(R.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, TexDesc(W, H, FMT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                  D3D12_RESOURCE_STATE_COMMON);
            UploadTexture(R, rTex.Get(), pat.data(), PITCH, H);
            std::vector<double> s, g;
            for (int i = 0; i < N; ++i)
                s.push_back(sendFrom(rTex.Get(), ra2.Get())), g.push_back(ingest(rd2.Get()));
            Result("bench.host_pages_send", true,
                   MsLine("R tex->host pages", Median(s), BYTES) + "; " + MsLine("D ingest", Median(g), BYTES));
            verifyD("bench.host_pages_verify");
        }
        else
        {
            Result("bench.host_pages_send", false, "setup failed");
        }
        ra2.Reset(), rd2.Reset(), a.Reset(), b.Reset();
        VirtualFree(p, 0, MEM_RELEASE);
    }
}

} // namespace

void ProbesD3D(const Adapter& r, const Adapter& d, bool bench)
{
    D12 R, D;
    if (!R.Init(r.a.Get()) || !D.Init(d.a.Get()))
    {
        Result("d3d12.devices", false, "");
        return;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o {};
    R.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    Info("d3d12.options", std::string("CrossAdapterRowMajorTextureSupported ") +
                              (o.CrossAdapterRowMajorTextureSupported ? "1" : "0") + ", CrossNodeSharingTier " +
                              std::to_string(o.CrossNodeSharingTier) + ", copy-queue timestamps R " +
                              (R.copyTs ? "1" : "0") + " D " + (D.copyTs ? "1" : "0"));
    if (!bench)
    {
        FunctionalD3D11(r, d, R, D);
        FunctionalHost(R, D);
    }
    else
    {
        Bench(r, R, D);
    }
}
