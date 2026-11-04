// D3D11 upscaler harness: a minimal D3D11 "game" that upscales through OptiScaler's NGX D3D11 entry points
// (scrolling colour bars, jitter, zero vectors) and presents the output. Put OptiScaler (with the plugin) beside it
// as dxgi.dll. Window is not activated and reads no input.
//   d3d11_harness.exe [seconds] [--out WxH] [--scale s] [--dumpAt s]
//   --dumpAt  writes lmxxf_nr_dumps\lmxxf_nr_dump.trigger beside the exe at <s> seconds
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <dbghelp.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dbghelp.lib")

using Microsoft::WRL::ComPtr;

static LRESULT CALLBACK Wnd(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

// Crash report on stdout: exception, then module + RVA per frame
static LONG WINAPI Crash(EXCEPTION_POINTERS* e)
{
    auto where = [](DWORD64 a)
    {
        HMODULE m = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR) a, &m))
            GetModuleFileNameW(m, name, MAX_PATH);
        const wchar_t* base = wcsrchr(name, L'\\');
        printf("  %ls+0x%llx\n", base ? base + 1 : name, (unsigned long long) (a - (DWORD64) m));
    };
    printf("CRASH %08lx at\n", e->ExceptionRecord->ExceptionCode);
    CONTEXT c = *e->ContextRecord;
    STACKFRAME64 f {};
    f.AddrPC = { c.Rip, 0, AddrModeFlat };
    f.AddrFrame = { c.Rbp, 0, AddrModeFlat };
    f.AddrStack = { c.Rsp, 0, AddrModeFlat };
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &f, &c,
                                          nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++i)
        where(f.AddrPC.Offset);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static float Halton(int i, int b)
{
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= b)
    {
        f /= float(b);
        r += f * float(i % b);
    }
    return r;
}

static ComPtr<ID3D11Texture2D> Tex(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT f, UINT bind)
{
    D3D11_TEXTURE2D_DESC td { w, h, 1, 1, f, { 1, 0 }, D3D11_USAGE_DEFAULT, bind, 0, 0 };
    ComPtr<ID3D11Texture2D> t;
    d->CreateTexture2D(&td, nullptr, &t);
    return t;
}

int main(int argc, char** argv)
{
    double seconds = 20.0, dumpAt = -1;
    UINT outW = 1920, outH = 1080;
    float scale = 1.5f;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            sscanf_s(argv[++i], "%ux%u", &outW, &outH);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = float(atof(argv[++i]));
        else if (!strcmp(argv[i], "--dumpAt") && i + 1 < argc)
            dumpAt = atof(argv[++i]);
        else
            seconds = atof(argv[i]);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(Crash);
    const UINT rw = UINT(outW / scale), rh = UINT(outH / scale);

    WNDCLASSW wc {};
    wc.lpfnWndProc = Wnd;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"d3d11_harness";
    RegisterClassW(&wc);
    RECT r { 0, 0, LONG(outW), LONG(outH) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"d3d11_harness", WS_OVERLAPPEDWINDOW, 0, 0,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC sd {};
    sd.BufferDesc = { outW, outH, { 0, 1 }, DXGI_FORMAT_R8G8B8A8_UNORM };
    sd.SampleDesc = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> sc;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    printf("D3D11CreateDeviceAndSwapChain %08lx, %ux%u -> %ux%u\n", hr, rw, rh, outW, outH);
    if (FAILED(hr))
        return 1;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ctx.As(&ctx1);

    // NGX through whatever dxgi.dll is loaded (OptiScaler exports the NGX D3D11 functions)
    HMODULE ngx = GetModuleHandleW(L"dxgi.dll");
    auto pInit = (decltype(&NVSDK_NGX_D3D11_Init)) GetProcAddress(ngx, "NVSDK_NGX_D3D11_Init");
    auto pCaps = (decltype(&NVSDK_NGX_D3D11_GetCapabilityParameters)) GetProcAddress(ngx, "NVSDK_NGX_D3D11_GetCapabilityParameters");
    auto pCreate = (decltype(&NVSDK_NGX_D3D11_CreateFeature)) GetProcAddress(ngx, "NVSDK_NGX_D3D11_CreateFeature");
    auto pEval = (decltype(&NVSDK_NGX_D3D11_EvaluateFeature)) GetProcAddress(ngx, "NVSDK_NGX_D3D11_EvaluateFeature");
    auto pRelease = (decltype(&NVSDK_NGX_D3D11_ReleaseFeature)) GetProcAddress(ngx, "NVSDK_NGX_D3D11_ReleaseFeature");
    if (!pInit || !pCaps || !pCreate || !pEval)
    {
        printf("dxgi.dll has no NGX D3D11 exports (OptiScaler not beside the exe?)\n");
        return 2;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path(exe).parent_path();
    NVSDK_NGX_Result res = pInit(0x1234, dir.c_str(), dev.Get(), nullptr, NVSDK_NGX_Version_API);
    printf("NGX D3D11 Init %08x\n", res);
    NVSDK_NGX_Parameter* p = nullptr;
    res = pCaps(&p);
    printf("GetCapabilityParameters %08x\n", res);
    if (NVSDK_NGX_FAILED(res) || p == nullptr)
        return 3;

    const int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, rw);
    p->Set(NVSDK_NGX_Parameter_Height, rh);
    p->Set(NVSDK_NGX_Parameter_OutWidth, outW);
    p->Set(NVSDK_NGX_Parameter_OutHeight, outH);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_MaxQuality);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    NVSDK_NGX_Handle* handle = nullptr;
    res = pCreate(ctx.Get(), NVSDK_NGX_Feature_SuperSampling, p, &handle);
    printf("CreateFeature %08x\n", res);
    if (NVSDK_NGX_FAILED(res))
        return 4;

    auto color = Tex(dev.Get(), rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto depth = Tex(dev.Get(), rw, rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto mv = Tex(dev.Get(), rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto out = Tex(dev.Get(), outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11RenderTargetView> colorRtv, depthRtv, mvRtv;
    dev->CreateRenderTargetView(color.Get(), nullptr, &colorRtv);
    dev->CreateRenderTargetView(depth.Get(), nullptr, &depthRtv);
    dev->CreateRenderTargetView(mv.Get(), nullptr, &mvRtv);
    ComPtr<ID3D11Texture2D> back;
    sc->GetBuffer(0, IID_PPV_ARGS(&back));

    if (dumpAt >= 0)
        std::filesystem::create_directories(dir / L"lmxxf_nr_dumps");

    LARGE_INTEGER freq, t0, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    double last = 0, lastReport = 0;
    unsigned frames = 0, ok = 0, failed = 0, reportFrames = 0;
    bool dumped = false;
    MSG msg {};
    for (;;)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (msg.message == WM_QUIT)
            break;
        QueryPerformanceCounter(&t);
        const double now = double(t.QuadPart - t0.QuadPart) / double(freq.QuadPart);
        if (now >= seconds)
            break;
        if (dumpAt >= 0 && !dumped && now >= dumpAt)
        {
            std::ofstream(dir / L"lmxxf_nr_dumps" / L"lmxxf_nr_dump.trigger") << "1";
            dumped = true;
            printf("dump trigger at %.1f s\n", now);
        }

        // Scrolling colour bars with a gradient row, at render size
        const float bg[4] = { 0.08f, 0.09f, 0.12f, 1.0f };
        ctx->ClearRenderTargetView(colorRtv.Get(), bg);
        const int bars = 24, shift = int(now * 60.0) % int(rw);
        for (int i = 0; i < bars; ++i)
        {
            const float h = float(i) / float(bars);
            const float c[4] = { 0.5f + 0.5f * sinf(6.2832f * h), 0.5f + 0.5f * sinf(6.2832f * (h + 0.33f)),
                                 0.5f + 0.5f * sinf(6.2832f * (h + 0.67f)), 1.0f };
            const LONG x0 = LONG((i * int(rw) / bars + shift) % int(rw));
            D3D11_RECT rc[2] = { { x0, LONG(rh / 8), x0 + LONG(rw / bars / 2), LONG(rh * 5 / 8) },
                                 { x0, LONG(rh * 3 / 4), x0 + 2, LONG(rh * 7 / 8) } };
            ctx1->ClearView(colorRtv.Get(), c, rc, 2);
        }
        const float d[4] = { 0.5f, 0, 0, 0 }, z[4] = {};
        ctx->ClearRenderTargetView(depthRtv.Get(), d);
        ctx->ClearRenderTargetView(mvRtv.Get(), z);

        const int phase = int(frames % 32) + 1;
        p->Set(NVSDK_NGX_Parameter_Color, (ID3D11Resource*) color.Get());
        p->Set(NVSDK_NGX_Parameter_Output, (ID3D11Resource*) out.Get());
        p->Set(NVSDK_NGX_Parameter_Depth, (ID3D11Resource*) depth.Get());
        p->Set(NVSDK_NGX_Parameter_MotionVectors, (ID3D11Resource*) mv.Get());
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, Halton(phase, 2) - 0.5f);
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, Halton(phase, 3) - 0.5f);
        p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
        p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
        p->Set(NVSDK_NGX_Parameter_Reset, frames == 0 ? 1 : 0);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, rw);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, rh);
        p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, float((now - last) * 1000.0));
        p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
        last = now;
        res = pEval(ctx.Get(), handle, p, nullptr);
        if (NVSDK_NGX_FAILED(res))
        {
            if (failed++ < 5)
                printf("EvaluateFeature %08x at frame %u\n", res, frames);
        }
        else
            ok++;

        ctx->CopyResource(back.Get(), out.Get());
        hr = sc->Present(0, 0);
        if (FAILED(hr))
        {
            printf("Present %08lx\n", hr);
            break;
        }
        frames++;
        reportFrames++;
        if (now - lastReport >= 5.0)
        {
            printf("%.0f s: %.1f fps, evaluates ok %u, failed %u\n", now, reportFrames / (now - lastReport), ok, failed);
            lastReport = now;
            reportFrames = 0;
        }
    }
    if (pRelease && handle)
        pRelease(handle);
    printf("done: %u frames, evaluates ok %u, failed %u\n", frames, ok, failed);
    return failed > frames / 2 ? 5 : 0;
}
