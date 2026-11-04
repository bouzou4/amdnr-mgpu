// Multi-GPU harness: a minimal D3D12 "game" (borderless window on the primary monitor, moving bars, tearing
// present). Put OptiScaler.dll beside it as dxgi.dll to exercise the device redirect and the proxy swapchain.
//   mgpu_harness.exe [seconds] [--nullAdapter]
#include <windows.h>
#include <winternl.h>
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

static int pci_bus(LUID luid)
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
    double seconds = argc > 1 ? atof(argv[1]) : 10.0;
    bool nullAdapter = false;
    for (int i = 1; i < argc; ++i)
        nullAdapter |= !strcmp(argv[i], "--nullAdapter");
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
    printf("adapter0 bus=%d outputs=%u %ls\n", pci_bus(ad.AdapterLuid), outputs, ad.Description);

    ComPtr<ID3D12Device> dev;
    HRESULT hr = D3D12CreateDevice(nullAdapter ? nullptr : a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr))
    {
        printf("D3D12CreateDevice %08lx\n", hr);
        return 2;
    }
    LUID dl = dev->GetAdapterLuid();
    printf("device bus=%d\n", pci_bus(dl));

    const int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
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
    sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc1;
    hr = f->CreateSwapChainForHwnd(q.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    printf("CreateSwapChainForHwnd %08lx %dx%d\n", hr, W, H);
    if (FAILED(hr))
        return 3;
    ComPtr<IDXGISwapChain3> sc;
    sc1.As(&sc);

    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> bb[3];
    for (UINT i = 0; i < 3; ++i)
    {
        sc->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
        auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * inc;
        dev->CreateRenderTargetView(bb[i].Get(), nullptr, h);
    }
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

    LARGE_INTEGER fq, t0, t;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    UINT64 frames = 0;
    for (;;)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
        QueryPerformanceCounter(&t);
        double el = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (el > seconds)
            break;
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
        for (int k = 0; k < 8; ++k)
        {
            int x = int((frames * (4 + k * 3) + k * 400) % (UINT64) W);
            D3D12_RECT r { x, k * H / 8, (std::min)(x + W / 16, W), (k + 1) * H / 8 - 8 };
            float c[4] = { (k & 1) ? 1.f : 0.2f, (k & 2) ? 1.f : 0.3f, (k & 4) ? 1.f : 0.4f, 1 };
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
        sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        q->Signal(fence.Get(), ++v);
        fv[i] = v;
        ++frames;
    }
    QueryPerformanceCounter(&t);
    double el = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
    printf("frames=%llu seconds=%.2f fps=%.1f\n", frames, el, frames / el);
    q->Signal(fence.Get(), ++v);
    fence->SetEventOnCompletion(v, ev);
    WaitForSingleObject(ev, 2000);
    return 0;
}
