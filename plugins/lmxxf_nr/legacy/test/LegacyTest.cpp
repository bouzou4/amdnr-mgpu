// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// x86 D3D9Ex title for the legacy pair without a game: squares moving 3 px per frame right on a hidden window.
// Usage: lmxxf_legacy_test.exe <path to proxy d3d9.dll> [seconds] [width] [height] [pause ms] [keys] [flags]
// keys: "ms:vk,ms:vk,..." key presses posted to the title's own window (the proxy's hotkeys and menu), no system input
// pause ms: one present-free gap at a third of the run, like a game's level load
// flags: 1 show the window (without activating it), 2 exclusive fullscreen
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 2;
    const double seconds = argc > 2 ? _wtof(argv[2]) : 10.0;
    const UINT w = argc > 3 ? _wtoi(argv[3]) : 1280, h = argc > 4 ? _wtoi(argv[4]) : 720;
    const DWORD pauseMs = argc > 5 ? DWORD(_wtoi(argv[5])) : 0;
    bool paused = false;
    struct Key { double at; UINT vk; bool sent; } keys[32] {};
    int nKeys = 0;
    for (const wchar_t* k = argc > 6 ? argv[6] : L""; *k && nKeys < 32;)
    {
        wchar_t* e = nullptr;
        const double ms = wcstod(k, &e);
        if (*e != L':')
            break;
        keys[nKeys++] = { ms / 1000.0, UINT(wcstoul(e + 1, &e, 0)), false };
        k = *e == L',' ? e + 1 : e;
    }
    const int flags = argc > 7 ? _wtoi(argv[7]) : 0;
    HMODULE d3d9 = LoadLibraryW(argv[1]);
    auto create = d3d9 ? (HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)) GetProcAddress(d3d9, "Direct3DCreate9Ex") : nullptr;
    if (!create)
        return printf("no Direct3DCreate9Ex\n"), 3;
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"lmxxf_legacy_test";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"lmxxf legacy test", WS_POPUP, 0, 0, w, h, nullptr,
                               nullptr, wc.hInstance, nullptr);
    if (flags & 1)
        ShowWindow(wnd, SW_SHOWNOACTIVATE);
    IDirect3D9Ex* d3d = nullptr;
    if (FAILED(create(D3D_SDK_VERSION, &d3d)))
        return printf("create failed\n"), 4;
    D3DPRESENT_PARAMETERS pp {};
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = (flags & 2) ? FALSE : TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    D3DDISPLAYMODEEX mode { sizeof(mode), w, h, 60, D3DFMT_X8R8G8B8, D3DSCANLINEORDERING_PROGRESSIVE };
    if (flags & 2)
        pp.FullScreen_RefreshRateInHz = 60;
    IDirect3DDevice9Ex* dev = nullptr;
    HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                     (flags & 2) ? &mode : nullptr, &dev);
    if (FAILED(hr))
        return printf("CreateDeviceEx %08lx\n", hr), 5;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int failed = 0, frames = 0;
    for (int i = 0;; ++i, ++frames)
    {
        QueryPerformanceCounter(&t1);
        if (double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart) >= seconds)
            break;
        if (pauseMs && !paused && double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart) >= seconds / 3)
            paused = true, Sleep(pauseMs);
        for (int k = 0; k < nKeys; ++k)
            if (!keys[k].sent && double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart) >= keys[k].at)
            {
                keys[k].sent = true;
                PostMessageW(wnd, WM_KEYDOWN, keys[k].vk, 1);
                PostMessageW(wnd, WM_KEYUP, keys[k].vk, 0xC0000001);
                printf("key %u at %.2f s\n", keys[k].vk, keys[k].at);
            }
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(70, 90, 110), 1.0f, 0);
        D3DRECT r[64];
        int n = 0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
            {
                const LONG s = 40, ox = (x * 160 + i * 3) % LONG(w + 160) - 80, oy = 30 + y * 85;
                r[n++] = { ox, oy, ox + s, oy + s };
            }
        dev->Clear(n, r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(230, 200, 60), 1.0f, 0);
        D3DRECT bar = { 0, LONG(h) - 40, LONG(w), LONG(h) }; // static strip
        dev->Clear(1, &bar, D3DCLEAR_TARGET, D3DCOLOR_XRGB(20, 20, 20), 1.0f, 0);
        hr = dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
        if (FAILED(hr))
            ++failed;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
    }
    QueryPerformanceCounter(&t1);
    const double s = double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
    printf("%d frames in %.2f s: %.1f fps, %d failed presents\n", frames, s, frames / s, failed);
    dev->Release();
    d3d->Release();
    return failed ? 1 : 0;
}
