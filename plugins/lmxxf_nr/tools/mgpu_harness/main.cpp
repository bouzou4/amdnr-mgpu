// Multi-GPU harness: a minimal D3D12 "game" (borderless window on the primary monitor, moving bars, tearing
// present). Put OptiScaler (with the plugin) beside it as dxgi.dll to exercise the device redirect and the proxy
// swapchain without a game. Every swapchain call's result is printed; no input is read.
//   mgpu_harness.exe [seconds] [--format sdr|pq|fp16] [--fse <at>] [--resize <at>] [--nullAdapter]
//   --format  pq: R10G10B10A2 + ST 2084, fp16: R16G16B16A16 float + scRGB (bars up to 4.0 = 320 nits)
//   --fse     exclusive fullscreen from <at> s for 4 s (SetFullscreenState + ResizeBuffers both ways)
//   --resize  ResizeBuffers to half size at <at> s, back to full 4 s later (the window follows)
#include <windows.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

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

static LRESULT CALLBACK Wnd(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv)
{
    double seconds = 10.0, fseAt = -1, resizeAt = -1;
    bool nullAdapter = false;
    const char* format = "sdr";
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--nullAdapter"))
            nullAdapter = true;
        else if (!strcmp(argv[i], "--format") && i + 1 < argc)
            format = argv[++i];
        else if (!strcmp(argv[i], "--fse") && i + 1 < argc)
            fseAt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--resize") && i + 1 < argc)
            resizeAt = atof(argv[++i]);
        else
            seconds = atof(argv[i]);
    }
    const bool pq = !strcmp(format, "pq"), fp16 = !strcmp(format, "fp16");
    const DXGI_FORMAT fmt = fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
    const DXGI_COLOR_SPACE_TYPE cs = pq     ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                     : fp16 ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))))
        return 1;
    ComPtr<IDXGIAdapter1> a;
    f->EnumAdapters1(0, &a);
    DXGI_ADAPTER_DESC1 ad {};
    a->GetDesc1(&ad);
    ComPtr<IDXGIOutput> out;
    UINT outputs = 0;
    while (a->EnumOutputs(outputs, &out) == S_OK)
        ++outputs, out.Reset();
    printf("adapter0 bus=%d outputs=%u %ls\n", PciBus(ad.AdapterLuid), outputs, ad.Description);

    ComPtr<ID3D12Device> dev;
    HRESULT hr = D3D12CreateDevice(nullAdapter ? nullptr : a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr))
    {
        printf("D3D12CreateDevice %08lx\n", hr);
        return 2;
    }
    printf("device bus=%d\n", PciBus(dev->GetAdapterLuid()));

    // What the game's device adapter reports after the redirect (outputs forwarded from the display card)
    {
        ComPtr<IDXGIAdapter1> da;
        if (f->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&da)) == S_OK)
        {
            ComPtr<IDXGIOutput> o;
            ComPtr<IDXGIOutput6> o6;
            DXGI_OUTPUT_DESC1 d {};
            hr = da->EnumOutputs(0, &o);
            if (hr == S_OK && o.As(&o6) == S_OK && o6->GetDesc1(&d) == S_OK)
                printf("device adapter output0: colour space %d, %u bits, max %.0f nits\n", (int) d.ColorSpace,
                       d.BitsPerColor, d.MaxLuminance);
            else
                printf("device adapter output0: %08lx\n", hr);
        }
    }

    int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
    const int fullW = W, fullH = H;
    WNDCLASSW wc {};
    wc.lpfnWndProc = Wnd;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mgpu_harness";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"mgpu_harness", WS_POPUP | WS_VISIBLE, 0, 0, W, H, nullptr,
                                nullptr, wc.hInstance, nullptr);

    D3D12_COMMAND_QUEUE_DESC qd {};
    ComPtr<ID3D12CommandQueue> q;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    DXGI_SWAP_CHAIN_DESC1 sd {};
    sd.Width = W;
    sd.Height = H;
    sd.Format = fmt;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc1;
    hr = f->CreateSwapChainForHwnd(q.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    printf("CreateSwapChainForHwnd %08lx %dx%d fmt %d\n", hr, W, H, (int) fmt);
    if (FAILED(hr))
        return 3;
    ComPtr<IDXGISwapChain4> sc;
    sc1.As(&sc);
    {
        UINT support = 0;
        hr = sc->CheckColorSpaceSupport(cs, &support);
        printf("CheckColorSpaceSupport %d %08lx support %x\n", (int) cs, hr, support);
        hr = sc->SetColorSpace1(cs);
        printf("SetColorSpace1 %d %08lx\n", (int) cs, hr);
        if (pq || fp16)
        {
            DXGI_HDR_METADATA_HDR10 md {};
            md.MaxMasteringLuminance = 1000;
            md.MaxContentLightLevel = 1000;
            md.MaxFrameAverageLightLevel = 400;
            hr = sc->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof md, &md);
            printf("SetHDRMetaData %08lx\n", hr);
        }
        ComPtr<IDXGIOutput> o;
        ComPtr<IDXGIOutput6> o6;
        DXGI_OUTPUT_DESC1 d {};
        hr = sc->GetContainingOutput(&o);
        if (hr == S_OK && o.As(&o6) == S_OK && o6->GetDesc1(&d) == S_OK)
            printf("containing output: colour space %d, max %.0f nits\n", (int) d.ColorSpace, d.MaxLuminance);
        else
            printf("GetContainingOutput %08lx\n", hr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> bb[3];
    auto views = [&]
    {
        for (UINT i = 0; i < 3; ++i)
        {
            sc->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
            auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += i * inc;
            dev->CreateRenderTargetView(bb[i].Get(), nullptr, h);
        }
    };
    views();
    ComPtr<ID3D12CommandAllocator> al[3];
    ComPtr<ID3D12GraphicsCommandList> cl;
    for (auto& x : al)
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&x));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al[0].Get(), nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ComPtr<ID3D12Fence> fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fv[3] {}, v = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto idle = [&]
    {
        q->Signal(fence.Get(), ++v);
        fence->SetEventOnCompletion(v, ev);
        WaitForSingleObject(ev, 2000);
    };
    auto resize = [&](int w, int h, const char* why)
    {
        idle();
        for (auto& b : bb)
            b.Reset();
        hr = sc->ResizeBuffers(3, w, h, fmt, sd.Flags);
        printf("ResizeBuffers %dx%d (%s) %08lx\n", w, h, why, hr);
        DXGI_SWAP_CHAIN_DESC1 d {};
        sc->GetDesc1(&d);
        W = (int) d.Width;
        H = (int) d.Height;
        views();
    };

    LARGE_INTEGER fq, t0, t;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    UINT64 frames = 0, failed = 0;
    int fseStep = 0, resizeStep = 0;
    BOOL fs = FALSE;
    const float k = fp16 ? 4.0f : 1.0f; // scRGB: 1.0 = 80 nits
    for (;;)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
        QueryPerformanceCounter(&t);
        double el = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (el > seconds)
            break;
        if (fseAt >= 0 && fseStep == 0 && el > fseAt)
        {
            fseStep = 1;
            idle();
            hr = sc->SetFullscreenState(TRUE, nullptr);
            sc->GetFullscreenState(&fs, nullptr);
            printf("SetFullscreenState TRUE %08lx, now %d\n", hr, fs);
            resize(fullW, fullH, "after entering fullscreen");
        }
        if (fseStep == 1 && el > fseAt + 4)
        {
            fseStep = 2;
            idle();
            hr = sc->SetFullscreenState(FALSE, nullptr);
            sc->GetFullscreenState(&fs, nullptr);
            printf("SetFullscreenState FALSE %08lx, now %d\n", hr, fs);
            resize(fullW, fullH, "after leaving fullscreen");
        }
        if (resizeAt >= 0 && resizeStep == 0 && el > resizeAt)
        {
            resizeStep = 1;
            SetWindowPos(hwnd, nullptr, 0, 0, fullW / 2, fullH / 2, SWP_NOZORDER | SWP_NOACTIVATE);
            resize(fullW / 2, fullH / 2, "half");
        }
        if (resizeStep == 1 && el > resizeAt + 4)
        {
            resizeStep = 2;
            SetWindowPos(hwnd, nullptr, 0, 0, fullW, fullH, SWP_NOZORDER | SWP_NOACTIVATE);
            resize(fullW, fullH, "full");
        }
        UINT i = sc->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < fv[i])
        {
            fence->SetEventOnCompletion(fv[i], ev);
            WaitForSingleObject(ev, 1000);
        }
        al[i]->Reset();
        cl->Reset(al[i].Get(), nullptr);
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { bb[i].Get(), 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        cl->ResourceBarrier(1, &b);
        auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * inc;
        float bg[4] = { 0.1f, 0.12f, 0.2f, 1 };
        cl->ClearRenderTargetView(h, bg, 0, nullptr);
        // Moving bars: position from the frame count, so dumps can be matched frame by frame.
        for (int j = 0; j < 8; ++j)
        {
            int x = int((frames * (4 + j * 3) + j * 400) % (UINT64) W);
            D3D12_RECT r { x, j * H / 8, (std::min)(x + W / 16, W), (j + 1) * H / 8 - 8 };
            float c[4] = { k * ((j & 1) ? 1.f : 0.2f), k * ((j & 2) ? 1.f : 0.3f), k * ((j & 4) ? 1.f : 0.4f), 1 };
            cl->ClearRenderTargetView(h, c, 1, &r);
        }
        // frame counter as a 32-bit binary strip at the bottom
        for (int bit = 0; bit < 32; ++bit)
        {
            D3D12_RECT r { bit * (W / 32), H - 24, (bit + 1) * (W / 32) - 2, H };
            float on[4] = { 1, 1, 1, 1 }, off[4] = { 0, 0, 0, 1 };
            cl->ClearRenderTargetView(h, ((frames >> bit) & 1) ? on : off, 1, &r);
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* ls[] = { cl.Get() };
        q->ExecuteCommandLists(1, ls);
        hr = sc->Present(0, fs ? 0 : DXGI_PRESENT_ALLOW_TEARING);
        if (FAILED(hr) && failed++ < 5)
            printf("Present %08lx at frame %llu\n", hr, frames);
        q->Signal(fence.Get(), ++v);
        fv[i] = v;
        ++frames;
    }
    QueryPerformanceCounter(&t);
    double el = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
    printf("frames=%llu seconds=%.2f fps=%.1f failed=%llu\n", frames, el, frames / el, failed);
    if (fs)
        sc->SetFullscreenState(FALSE, nullptr);
    idle();
    return failed ? 4 : 0;
}
