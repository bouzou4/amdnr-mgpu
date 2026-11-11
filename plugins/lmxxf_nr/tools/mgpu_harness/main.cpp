// Multi-GPU harness: a minimal D3D12 "game" (borderless window on the primary monitor, moving bars, tearing
// present). Put OptiScaler (with the plugin) beside it as dxgi.dll to exercise the device redirect and the proxy
// swapchain without a game. Every swapchain call's result is printed; no input is read.
//   mgpu_harness.exe [seconds] [--format sdr|pq|fp16] [--fse <at>] [--resize <at>] [--minimize <at>] [--cycles <n>]
//                    [--minpause] [--nullAdapter]
//   --format  pq: R10G10B10A2 + ST 2084, fp16: R16G16B16A16 float + scRGB (bars up to 4.0 = 320 nits)
//   --fse     exclusive fullscreen from <at> s for 4 s (SetFullscreenState + ResizeBuffers both ways), --fsehold: stays
//   --resize  ResizeBuffers to half size at <at> s, back to full 4 s later (the window follows)
//   --minimize  from <at> s: minimise (no activation) for 4 s, restore, ResizeBuffers at the same size after each
//               step (as CONTROL does); --cycles repeats it every 10 s, --minpause stops presenting while minimised
//   Latency rig: --gpu-ms <ms> compute load per frame, --cpu-ms <ms> busy "simulation" after the input sample,
//   --al2 (Anti-Lag 2 called directly) or --reflex (Reflex through nvapi64.dll, markers included),
//   --phase <s> low latency off/on alternating every <s> s (off first), --waitable (latency 1 waitable object),
//   --csv <path> per frame: sample, present and GPU done times (QPC)
//   Hitch rig: --stall <at> --stall-ms <ms> [--stall-every <s>]: from <at> s the render queue waits on a fence the CPU
//   signals <ms> later (a loading hitch: the send lands late, no long GPU packet on the render card); the game keeps
//   presenting meanwhile
//   Fake upscaler (the plugin's TestHooks=true): --upscaler renders the scene at --render-scale <f> (0.667) with motion
//   vectors, calls the plugin's upscale callbacks around its own scaling pass and copies the result to the back
//   buffer; --hudless gives the plugin that picture before the frame counter strip as the HUD-less colour
//   Switch fuzzer: --fuzz <seed> [--fuzz-steps <n>] [--fuzz-classes <letters>] [--cap <fps>] [--mgpu]: a seeded
//   sequence of live switches through the plugin's trigger files (i ini keys, d frame dumps, s sequence dumps,
//   g multi-GPU triggers), window changes (r resize, m minimise) and upscaler changes (h HUD-less source on / off,
//   q render scale, u reset flag / no vectors). Every step is printed with its frame; the same seed and options give
//   the same steps at the same frames, --fuzz-classes keeps the sequence and drops the other classes. Stops at a
//   device loss (exit 5) or when NR fails (exit 6). --script <file> plays a fixed sequence instead (fuzz.h)
#include <windows.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include "../../../../src/external/AntiLag2-SDK/ffx_antilag2_dx12.h"
#include "fuzz.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3dcompiler.lib")

// nvapi's Reflex calls, by interface id (nvapi_interface.h)
namespace nv
{
struct SleepMode
{
    uint32_t version;
    uint8_t lowLatency, boost;
    uint32_t minIntervalUs;
    uint8_t markers, minQueue;
    uint8_t rsvd[30];
};
struct Marker
{
    uint32_t version;
    uint64_t frameId;
    int type;
    uint64_t rsvd0;
    uint8_t rsvd[56];
};
using Qi = void* (*) (uint32_t);
using Init = int (*)();
using SetSleepMode = int (*)(IUnknown*, SleepMode*);
using Sleep = int (*)(IUnknown*);
using SetMarker = int (*)(IUnknown*, Marker*);
} // namespace nv

static const char* kLoadHlsl = R"(
RWByteAddressBuffer o : register(u0);
cbuffer C : register(b0) { uint iters; };
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    float a = id.x * 1e-6, b = 1.0001;
    for (uint i = 0; i < iters; ++i) { a = a * b + 0.5; b = b * 0.99999 + a * 1e-9; }
    o.Store((id.x & 0x3FFFF) * 4, asuint(a + b));
}
)";

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
    double seconds = 10.0, fseAt = -1, resizeAt = -1, minAt = -1;
    int cycles = 1;
    bool nullAdapter = false, minPause = false, fseHold = false;
    bool useAl2 = false, useReflex = false, useWaitable = false;
    double gpuMs = 0, cpuMs = 0, phase = 0;
    double stallAt = -1, stallMs = 2500, stallEvery = 0;
    const char* csvPath = nullptr;
    const char* format = "sdr";
    bool useUpscaler = false, useHudless = false, fuzz = false, mgpuHint = false;
    float renderScale = 0.667f;
    UINT64 fuzzSeed = 0;
    int fuzzSteps = 200;
    double cap = 0;
    const char* fuzzClasses = nullptr;
    const char* scriptPath = nullptr;
    setvbuf(stdout, nullptr, _IOLBF, 4096);
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
        else if (!strcmp(argv[i], "--minimize") && i + 1 < argc)
            minAt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc)
            cycles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fsehold"))
            fseHold = true;
        else if (!strcmp(argv[i], "--minpause"))
            minPause = true;
        else if (!strcmp(argv[i], "--gpu-ms") && i + 1 < argc)
            gpuMs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cpu-ms") && i + 1 < argc)
            cpuMs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--phase") && i + 1 < argc)
            phase = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stall") && i + 1 < argc)
            stallAt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stall-ms") && i + 1 < argc)
            stallMs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stall-every") && i + 1 < argc)
            stallEvery = atof(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc)
            csvPath = argv[++i];
        else if (!strcmp(argv[i], "--al2"))
            useAl2 = true;
        else if (!strcmp(argv[i], "--reflex"))
            useReflex = true;
        else if (!strcmp(argv[i], "--waitable"))
            useWaitable = true;
        else if (!strcmp(argv[i], "--upscaler"))
            useUpscaler = true;
        else if (!strcmp(argv[i], "--hudless"))
            useHudless = true;
        else if (!strcmp(argv[i], "--mgpu"))
            mgpuHint = true;
        else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc)
            renderScale = (float) atof(argv[++i]);
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc)
            fuzz = true, fuzzSeed = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--fuzz-steps") && i + 1 < argc)
            fuzzSteps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fuzz-classes") && i + 1 < argc)
            fuzzClasses = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            fuzz = true, scriptPath = argv[++i];
        else if (!strcmp(argv[i], "--cap") && i + 1 < argc)
            cap = atof(argv[++i]);
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
    {
        // The plugin's Validation key turned the debug layer on: our rectangle clears are not what is tested
        ComPtr<ID3D12InfoQueue> iq;
        if (SUCCEEDED(dev.As(&iq)))
        {
            D3D12_MESSAGE_ID deny[] = { D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE };
            D3D12_INFO_QUEUE_FILTER filter {};
            filter.DenyList.NumIDs = 1;
            filter.DenyList.pIDList = deny;
            iq->AddStorageFilterEntries(&filter);
            printf("debug layer on\n");
        }
    }

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
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | (useWaitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
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
        // validation runs are slow: until done or the device is gone
        for (int i = 0; i < 30 && WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0; ++i)
            if (fence->GetCompletedValue() >= v || FAILED(dev->GetDeviceRemovedReason()))
                break;
    };
    // Fake upscaler / HUD-less source through the plugin's test hooks
    const bool pipe = useUpscaler || useHudless;
    hx::Hooks hooks;
    hx::Pipeline pl;
    bool hudOn = useHudless;
    if (pipe || fuzz)
        printf("plugin test hooks: %s\n", hooks.Load() ? "found" : "not found (TestHooks=true, lmxxf_nr.asi loaded?)");
    if (pipe && (!hooks.upscale || !pl.Init(dev.Get(), q.Get(), fmt, fp16 ? 4.0f : 1.0f)))
    {
        printf("fake upscaler unavailable\n");
        return 6;
    }
    auto targets = [&]
    {
        if (!pipe)
            return;
        hooks.hudless(nullptr, 0);
        if (!pl.Targets((UINT) W, (UINT) H, useUpscaler ? renderScale : 1.0f))
            printf("fake upscaler targets failed\n");
        else
            printf("fake upscaler: render %ux%u -> %ux%u\n", pl.rw, pl.rh, pl.ow, pl.oh);
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
        targets();
    };
    targets();

    HANDLE waitable = nullptr;
    if (useWaitable)
    {
        hr = sc->SetMaximumFrameLatency(1);
        waitable = sc->GetFrameLatencyWaitableObject();
        printf("waitable %p SetMaximumFrameLatency(1) %08lx\n", waitable, hr);
    }

    // GPU load: a compute loop, calibrated to gpuMs with timestamps
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> loadBuf;
    UINT iters = 0;
    const UINT groups = 4096;
    if (gpuMs > 0)
    {
        ComPtr<ID3DBlob> cs, err, rsb;
        if (FAILED(D3DCompile(kLoadHlsl, strlen(kLoadHlsl), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err)))
        {
            printf("load shader: %s\n", err ? (const char*) err->GetBufferPointer() : "?");
            return 5;
        }
        D3D12_ROOT_PARAMETER rp[2] {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[1].Constants.Num32BitValues = 1;
        D3D12_ROOT_SIGNATURE_DESC rd { 2, rp };
        D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
        dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = rs.Get();
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 1 << 20;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                     IID_PPV_ARGS(&loadBuf));
        ComPtr<ID3D12QueryHeap> qh;
        D3D12_QUERY_HEAP_DESC qd2 { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 };
        dev->CreateQueryHeap(&qd2, IID_PPV_ARGS(&qh));
        D3D12_HEAP_PROPERTIES rh { D3D12_HEAP_TYPE_READBACK };
        bd.Width = 16;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> rb;
        dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&rb));
        ComPtr<ID3D12CommandAllocator> ca;
        ComPtr<ID3D12GraphicsCommandList> cc;
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cc));
        cc->Close();
        ComPtr<ID3D12Fence> cf;
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cf));
        UINT64 freq = 0;
        q->GetTimestampFrequency(&freq);
        UINT n = 2000;
        for (int pass = 0; pass < 6; ++pass)
        {
            ca->Reset();
            cc->Reset(ca.Get(), pso.Get());
            cc->SetComputeRootSignature(rs.Get());
            cc->SetComputeRootUnorderedAccessView(0, loadBuf->GetGPUVirtualAddress());
            cc->SetComputeRoot32BitConstant(1, n, 0);
            cc->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            cc->Dispatch(groups, 1, 1);
            cc->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            cc->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, rb.Get(), 0);
            cc->Close();
            ID3D12CommandList* l[] = { cc.Get() };
            q->ExecuteCommandLists(1, l);
            q->Signal(cf.Get(), pass + 1);
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            cf->SetEventOnCompletion(pass + 1, e);
            WaitForSingleObject(e, 5000);
            CloseHandle(e);
            UINT64* p = nullptr;
            rb->Map(0, nullptr, (void**) &p);
            const double ms = double(p[1] - p[0]) * 1000.0 / double(freq);
            rb->Unmap(0, nullptr);
            printf("load calibration: %u iters %.3f ms\n", n, ms);
            if (ms > 0.05)
                n = UINT((std::max)(1.0, double(n) * gpuMs / ms));
        }
        iters = n;
    }

    // Anti-Lag 2 directly, or Reflex through nvapi64.dll (OptiScaler's fakenvapi on AMD)
    AMD::AntiLag2DX12::Context al2 {};
    if (useAl2)
    {
        LoadLibraryW(L"amdxc64.dll");
        hr = AMD::AntiLag2DX12::Initialize(&al2, dev.Get());
        printf("AntiLag2 Initialize %08lx\n", hr);
    }
    nv::SetSleepMode nvSleepMode = nullptr;
    nv::Sleep nvSleep = nullptr;
    nv::SetMarker nvMarker = nullptr;
    if (useReflex)
    {
        HMODULE nvm = LoadLibraryW(L"nvapi64.dll");
        auto qi = nvm ? (nv::Qi) GetProcAddress(nvm, "nvapi_QueryInterface") : nullptr;
        if (qi)
        {
            auto init = (nv::Init) qi(0x0150e828);
            printf("NvAPI_Initialize %d\n", init ? init() : -999);
            nvSleepMode = (nv::SetSleepMode) qi(0xac1ca9e0);
            nvSleep = (nv::Sleep) qi(0x852cd1d2);
            nvMarker = (nv::SetMarker) qi(0xd9984c05);
        }
        printf("Reflex: nvapi %p sleep mode %p sleep %p marker %p\n", nvm, nvSleepMode, nvSleep, nvMarker);
    }
    bool llOn = false, llSet = false;
    auto setLowLatency = [&](bool on)
    {
        if (llSet && on == llOn)
            return;
        llSet = true;
        llOn = on;
        if (nvSleepMode)
        {
            nv::SleepMode m {};
            m.version = sizeof(m) | (1 << 16);
            m.lowLatency = on;
            m.markers = 1;
            printf("Reflex low latency %d: %d\n", (int) on, nvSleepMode(dev.Get(), &m));
        }
        if (useAl2)
            printf("AntiLag2 %d\n", (int) on);
    };
    auto marker = [&](UINT64 id, int type)
    {
        if (!nvMarker)
            return;
        nv::Marker m {};
        m.version = sizeof(m) | (1 << 16);
        m.frameId = id;
        m.type = type;
        nvMarker(dev.Get(), &m);
    };

    // Per frame QPC times; GPU done from a watcher thread on the frame's fence value
    struct FrameTimes
    {
        LONGLONG sample = 0, present = 0, presented = 0;
        std::atomic<LONGLONG> gpu { 0 };
        bool ll = false;
    };
    std::vector<FrameTimes> times(csvPath ? 20000 : 0);
    std::atomic<UINT64> watchTo { 0 };
    std::atomic<bool> watchStop { false };
    std::thread watcher;

    LARGE_INTEGER fq, t0, t;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    auto qpc = []
    {
        LARGE_INTEGER x;
        QueryPerformanceCounter(&x);
        return x.QuadPart;
    };
    UINT64 frames = 0, failed = 0;
    ComPtr<ID3D12Fence> stallFence;
    UINT64 stallValue = 0;
    int fseStep = 0, resizeStep = 0, minStep = 0;
    bool minimized = false;
    BOOL fs = FALSE;
    const float k = fp16 ? 4.0f : 1.0f; // scRGB: 1.0 = 80 nits
    std::vector<UINT64> frameValue(times.size());
    if (csvPath)
    {
        watcher = std::thread(
            [&]
            {
                HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                UINT64 f = 0;
                while (!watchStop)
                {
                    if (f >= watchTo.load())
                    {
                        Sleep(0);
                        continue;
                    }
                    const UINT64 val = frameValue[f];
                    if (fence->GetCompletedValue() < val)
                    {
                        fence->SetEventOnCompletion(val, e);
                        if (WaitForSingleObject(e, 500) != WAIT_OBJECT_0)
                            continue;
                    }
                    times[f].gpu = qpc();
                    ++f;
                }
            });
    }
    // Switch fuzzer state
    hx::Fuzzer fz;
    std::vector<hx::Action> script;
    std::wstring dumpDir;
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        dumpDir = exe;
        dumpDir = dumpDir.substr(0, dumpDir.find_last_of(L'\\')) + L"\\lmxxf_nr_dumps";
    }
    UINT64 fuzzNext = 120; // frame of the first step
    int fuzzDone = 0, fuzzSkipped = 0;
    UINT64 minimiseEnd = 0, resizeAtFrame = 0;
    bool fuzzMinimised = false, minResize = false;
    int pendW = 0, pendH = 0, resetLeft = 0, noMvLeft = 0;
    uint32_t lastStatus = 0;
    UINT64 stopAt = 0, fuzzEnd = 0;
    int result = 0;
    if (fuzz)
    {
        CreateDirectoryW(dumpDir.c_str(), nullptr);
        fz.Init(fuzzSeed, mgpuHint, useUpscaler, fuzzClasses, fullW, fullH);
        if (scriptPath)
        {
            if (!hx::LoadScript(scriptPath, script))
                return printf("script %s: no steps\n", scriptPath), 7;
            fuzzSteps = (int) script.size();
            fuzzNext += script[0].gap;
        }
        if (cap <= 0)
            cap = 120;
        printf("fuzz seed %llu, %d steps, classes %s, cap %.0f fps, upscaler %d, hudless %d, mgpu %d\n", fuzzSeed,
               fuzzSteps, fuzzClasses ? fuzzClasses : "all", cap, (int) useUpscaler, (int) useHudless, (int) mgpuHint);
    }
    LONGLONG capNext = 0;
    for (;;)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
        if (cap > 0)
        {
            const LONGLONG step = LONGLONG(double(fq.QuadPart) / cap);
            LONGLONG now = qpc();
            while (now < capNext)
            {
                Sleep(capNext - now > fq.QuadPart / 500 ? 1 : 0);
                now = qpc();
            }
            capNext = (std::max)(capNext + step, now - step);
        }
        QueryPerformanceCounter(&t);
        double el = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (el > seconds)
            break;
        if ((stopAt && frames >= stopAt) || (fuzzEnd && frames >= fuzzEnd))
            break;
        // Plugin state: a lost device or a failed NR ends the run a few frames later (its log gets the DRED lines)
        if (hooks.status && frames % 8 == 0 && !stopAt)
        {
            const uint32_t st = hooks.status();
            if (st != lastStatus)
            {
                printf("plugin status %x at frame %llu, %.2f s, after step %d%s%s%s%s\n", st, frames, el,
                       fuzzDone + fuzzSkipped - 1,
                       (st & 1) ? ": GAME DEVICE REMOVED" : "", (st & 2) ? ": DISPLAY CARD LOST" : "",
                       (st & 4) ? ": NR FAILED (game card)" : "", (st & 8) ? ": NR FAILED (display card)" : "");
                lastStatus = st;
                if (fuzz && st)
                {
                    stopAt = frames + 40;
                    result = (st & 3) ? 5 : 6;
                }
            }
        }
        if (fuzz && !stopAt)
        {
            if (fuzzMinimised && frames >= minimiseEnd)
            {
                fuzzMinimised = false;
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                printf("fuzz: restored at frame %llu\n", frames);
                if (minResize)
                    resize(W, H, "after restore");
            }
            if (resizeAtFrame && frames >= resizeAtFrame)
            {
                resizeAtFrame = 0;
                resize(pendW, pendH, "fuzz");
            }
            if (frames >= fuzzNext)
            {
                if (fuzzDone + fuzzSkipped >= fuzzSteps)
                {
                    // the last switches play out, then the run ends
                    if (!fuzzEnd)
                        fuzzEnd = frames + 240;
                }
                else
                {
                    const size_t at = size_t(fuzzDone + fuzzSkipped);
                    hx::Action a = script.empty() ? fz.Next(frames) : script[at];
                    fuzzNext = frames + (script.empty() ? a.gap : at + 1 < script.size() ? script[at + 1].gap : 0);
                    SYSTEMTIME lt {};
                    GetLocalTime(&lt);
                    printf("fuzz %d frame %llu t %.2f at %02u:%02u:%02u.%03u%s: %s\n", fuzzDone + fuzzSkipped, frames, el,
                           lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds, a.skip ? " (masked)" : "", a.what.c_str());
                    if (a.skip)
                        ++fuzzSkipped;
                    else
                    {
                        ++fuzzDone;
                        for (auto& f : a.files)
                            if (!hx::WriteTrigger(dumpDir, f.first, f.second))
                                printf("fuzz: trigger %ls not written\n", f.first.c_str());
                        switch (a.kind)
                        {
                        case hx::Action::Hud:
                            hudOn = a.a != 0;
                            break;
                        case hx::Action::Resize:
                            SetWindowPos(hwnd, nullptr, 0, 0, a.a, a.b, SWP_NOZORDER | SWP_NOACTIVATE);
                            pendW = a.a, pendH = a.b;
                            if (a.c == 0)
                                resize(pendW, pendH, "fuzz");
                            else
                                resizeAtFrame = frames + a.c;
                            break;
                        case hx::Action::Minimise:
                            fuzzMinimised = true;
                            minResize = a.b != 0;
                            minimiseEnd = frames + a.a;
                            ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
                            if (minResize)
                                resize(W, H, "after minimise");
                            break;
                        case hx::Action::Scale:
                            renderScale = a.f;
                            idle();
                            targets();
                            break;
                        case hx::Action::Reset:
                            resetLeft = a.a;
                            break;
                        case hx::Action::NoMv:
                            noMvLeft = a.a;
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
        }
        if (fseAt >= 0 && fseStep == 0 && el > fseAt)
        {
            fseStep = 1;
            idle();
            hr = sc->SetFullscreenState(TRUE, nullptr);
            sc->GetFullscreenState(&fs, nullptr);
            printf("SetFullscreenState TRUE %08lx, now %d\n", hr, fs);
            resize(fullW, fullH, "after entering fullscreen");
        }
        if (fseStep == 1 && !fseHold && el > fseAt + 4)
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
        if (minAt >= 0 && minStep < 2 * cycles && el > minAt + (minStep / 2) * 10.0 + (minStep % 2) * 4.0)
        {
            minimized = (minStep % 2) == 0;
            ShowWindow(hwnd, minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
            RECT rc {};
            GetClientRect(hwnd, &rc);
            printf("cycle %d: %s at %.2f s, client %ldx%ld, frame %llu\n", minStep / 2 + 1,
                   minimized ? "minimised" : "restored", el, rc.right - rc.left, rc.bottom - rc.top, frames);
            ++minStep;
            resize(fullW, fullH, minimized ? "after minimise" : "after restore");
        }
        if (minimized && minPause)
        {
            Sleep(16);
            continue;
        }
        UINT i = sc->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < fv[i])
        {
            fence->SetEventOnCompletion(fv[i], ev);
            WaitForSingleObject(ev, 1000);
        }
        if (waitable)
            WaitForSingleObjectEx(waitable, 1000, TRUE);
        const bool wantLl = phase > 0 ? (int(el / phase) & 1) != 0 : (useAl2 || useReflex);
        setLowLatency(wantLl);
        if (nvSleep)
        {
            nvSleep(dev.Get());
            marker(frames + 1, 0); // SIMULATION_START
            marker(frames + 1, 6); // INPUT_SAMPLE
        }
        if (useAl2 && al2.m_pAntiLagAPI)
            AMD::AntiLag2DX12::Update(&al2, llOn, 0);
        const LONGLONG tSample = qpc();
        if (cpuMs > 0)
        {
            const LONGLONG until = tSample + LONGLONG(cpuMs * 1e-3 * double(fq.QuadPart));
            while (qpc() < until)
                ;
        }
        marker(frames + 1, 1); // SIMULATION_END
        marker(frames + 1, 2); // RENDERSUBMIT_START
        al[i]->Reset();
        cl->Reset(al[i].Get(), nullptr);
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { bb[i].Get(), 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * inc;
        float bg[4] = { 0.1f, 0.12f, 0.2f, 1 };
        if (pipe)
        {
            pl.Frame(cl.Get(), bb[i].Get(), i, frames, hooks, useUpscaler, resetLeft > 0, noMvLeft > 0);
            resetLeft -= resetLeft > 0;
            noMvLeft -= noMvLeft > 0;
        }
        else
        {
            cl->ResourceBarrier(1, &b);
            cl->ClearRenderTargetView(h, bg, 0, nullptr);
        }
        if (iters)
        {
            cl->SetPipelineState(pso.Get());
            cl->SetComputeRootSignature(rs.Get());
            cl->SetComputeRootUnorderedAccessView(0, loadBuf->GetGPUVirtualAddress());
            cl->SetComputeRoot32BitConstant(1, iters, 0);
            cl->Dispatch(groups, 1, 1);
        }
        // Moving bars: position from the frame count, so dumps can be matched frame by frame.
        for (int j = 0; j < 8 && !pipe; ++j)
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
        if (stallAt >= 0 && el > stallAt)
        {
            if (!stallFence)
                dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stallFence));
            q->Wait(stallFence.Get(), ++stallValue);
            printf("stall %llu: render queue held %.0f ms from %.2f s, frame %llu\n", stallValue, stallMs, el, frames);
            std::thread([f = stallFence, v = stallValue, ms = stallMs] {
                Sleep(DWORD(ms));
                f->Signal(v);
            }).detach();
            stallAt = stallEvery > 0 ? el + stallEvery : -1;
        }
        ID3D12CommandList* ls[] = { cl.Get() };
        q->ExecuteCommandLists(1, ls);
        if (pipe)
            hooks.hudless(hudOn ? pl.hud.Get() : nullptr, hx::Pipeline::kRead);
        marker(frames + 1, 3); // RENDERSUBMIT_END
        marker(frames + 1, 4); // PRESENT_START
        const LONGLONG tPresent = qpc();
        hr = sc->Present(0, fs ? 0 : DXGI_PRESENT_ALLOW_TEARING);
        const LONGLONG tPresented = qpc();
        marker(frames + 1, 5); // PRESENT_END
        if (FAILED(hr) && failed++ < 5)
            printf("Present %08lx at frame %llu\n", hr, frames);
        if (fuzz && !stopAt && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET))
        {
            printf("DEVICE LOST: Present %08lx, removed reason %08lx at frame %llu, %.2f s, after step %d\n", hr,
                   dev->GetDeviceRemovedReason(), frames, el, fuzzDone + fuzzSkipped - 1);
            stopAt = frames + 3;
            result = 5;
        }
        q->Signal(fence.Get(), ++v);
        fv[i] = v;
        if (frames < times.size())
        {
            auto& ft = times[frames];
            ft.sample = tSample;
            ft.present = tPresent;
            ft.presented = tPresented;
            ft.ll = llOn;
            frameValue[frames] = v;
            watchTo = frames + 1;
        }
        ++frames;
    }
    QueryPerformanceCounter(&t);
    double el = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
    printf("frames=%llu seconds=%.2f fps=%.1f failed=%llu\n", frames, el, frames / el, failed);
    if (fuzz)
        printf("fuzz end: seed %llu, %d steps applied, %d masked, result %d (%s)\n", fuzzSeed, fuzzDone, fuzzSkipped, result,
               result == 5 ? "device lost" : result == 6 ? "NR failed" : fuzzDone + fuzzSkipped >= fuzzSteps ? "all steps" : "time up");
    if (pipe)
        hooks.hudless(nullptr, 0);
    if (csvPath)
    {
        idle();
        Sleep(100);
        watchStop = true;
        watcher.join();
        if (FILE* fp = fopen(csvPath, "w"))
        {
            fprintf(fp, "# qpc_freq=%lld gpu_ms=%.2f cpu_ms=%.2f iters=%u al2=%d reflex=%d waitable=%d\n", fq.QuadPart,
                    gpuMs, cpuMs, iters, (int) useAl2, (int) useReflex, (int) useWaitable);
            fprintf(fp, "frame,ll,sample,present,presented,gpu_done\n");
            for (UINT64 f = 0; f < (std::min<UINT64>)(frames, times.size()); ++f)
                fprintf(fp, "%llu,%d,%lld,%lld,%lld,%lld\n", f, (int) times[f].ll, times[f].sample, times[f].present,
                        times[f].presented, times[f].gpu.load());
            fclose(fp);
        }
    }
    if (fs)
        sc->SetFullscreenState(FALSE, nullptr);
    idle();
    if (result)
        return result;
    return failed ? 4 : 0;
}
