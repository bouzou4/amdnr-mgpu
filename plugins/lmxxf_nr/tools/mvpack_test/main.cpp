// Offline test of the motion vector packing (src/mgpu/MvPack): packs R16G16 dumps on the GPU, moves only the packed
// bytes into a buffer filled with garbage, unpacks and compares bit for bit. Prints the ratio and the GPU times.
//   mvpack_test.exe [--adapter N] [--synthetic] <file.raw> <width> <height> [<file.raw> <width> <height> ...]
// --synthetic also runs generated fields: constant, ramps across zero (-0 / +0), NaN / inf / denormal halves, noise.
#include "../../src/mgpu/MvPack.h"

#include <dxgi1_6.h>
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using Mgpu::MvPack;

struct Gpu
{
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> qh;
    ComPtr<ID3D12Resource> ts;
    UINT64 value = 0, freq = 0;
    void Run()
    {
        cl->Close();
        ID3D12CommandList* l[] = { cl.Get() };
        q->ExecuteCommandLists(1, l);
        q->Signal(fence.Get(), ++value);
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(value, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 n, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS f, D3D12_RESOURCE_STATES s)
    {
        D3D12_RESOURCE_DESC b {};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = (n + 255) & ~255ull;
        b.Height = b.DepthOrArraySize = b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        b.Flags = f;
        D3D12_HEAP_PROPERTIES hp { type };
        ComPtr<ID3D12Resource> r;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, s, nullptr, IID_PPV_ARGS(&r))))
        {
            fprintf(stderr, "buffer %llu failed\n", n);
            exit(2);
        }
        return r;
    }
};

static D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    return x;
}

// texels (tight) -> result: 0 bit-exact, 1 mismatch
static int Test(Gpu& g, MvPack& pk, const char* name, const std::vector<uint32_t>& tex, UINT w, UINT h)
{
    const UINT pitch = MvPack::Pitch(w);
    const UINT64 lin = UINT64(pitch) * h, maxB = MvPack::MaxBytes(w, h);
    const auto U = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto ns = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto up = g.Buffer(std::max(lin, maxB), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto src = g.Buffer(lin, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto out = g.Buffer(maxB, D3D12_HEAP_TYPE_DEFAULT, U, ua);
    auto total = g.Buffer(256, D3D12_HEAP_TYPE_DEFAULT, U, ua);
    auto moved = g.Buffer(maxB, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto dst = g.Buffer(lin, D3D12_HEAP_TYPE_DEFAULT, U, ua);
    auto rb = g.Buffer(lin + 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    uint8_t* p = nullptr;
    up->Map(0, nullptr, (void**) &p);
    for (UINT y = 0; y < h; ++y)
        memcpy(p + UINT64(y) * pitch, &tex[UINT64(y) * w], w * 4);
    up->Unmap(0, nullptr);
    g.cl->CopyBufferRegion(src.Get(), 0, up.Get(), 0, lin);
    auto b0 = Tr(src.Get(), D3D12_RESOURCE_STATE_COPY_DEST, ns);
    g.cl->ResourceBarrier(1, &b0);
    g.cl->EndQuery(g.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    pk.Encode(g.cl.Get(), 1, src.Get(), w, h, out.Get(), total.Get());
    g.cl->EndQuery(g.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    auto b1 = Tr(total.Get(), ua, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.cl->ResourceBarrier(1, &b1);
    g.cl->CopyBufferRegion(rb.Get(), lin, total.Get(), 0, 16);
    g.Run();
    UINT32 bytes = 0;
    rb->Map(0, nullptr, (void**) &p);
    memcpy(&bytes, p + lin + 4, 4);
    rb->Unmap(0, nullptr);
    if (bytes == 0 || bytes > maxB)
    {
        printf("%s: bad packed size %u\n", name, bytes);
        return 1;
    }
    // the moved buffer: garbage, then exactly the packed bytes
    up->Map(0, nullptr, (void**) &p);
    std::mt19937 rng(7);
    for (UINT64 i = 0; i < maxB; ++i)
        p[i] = uint8_t(rng());
    up->Unmap(0, nullptr);
    g.cl->CopyBufferRegion(moved.Get(), 0, up.Get(), 0, maxB);
    auto b2 = Tr(out.Get(), ua, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.cl->ResourceBarrier(1, &b2);
    g.cl->CopyBufferRegion(moved.Get(), 0, out.Get(), 0, bytes);
    auto b3 = Tr(moved.Get(), D3D12_RESOURCE_STATE_COPY_DEST, ns);
    g.cl->ResourceBarrier(1, &b3);
    g.cl->EndQuery(g.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    pk.Decode(g.cl.Get(), 1, moved.Get(), w, h, dst.Get());
    g.cl->EndQuery(g.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
    g.cl->ResolveQueryData(g.qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, g.ts.Get(), 0);
    auto b4 = Tr(dst.Get(), ua, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.cl->ResourceBarrier(1, &b4);
    g.cl->CopyBufferRegion(rb.Get(), 0, dst.Get(), 0, lin);
    g.Run();
    UINT64 t[4] {};
    void* tp = nullptr;
    g.ts->Map(0, nullptr, &tp);
    memcpy(t, tp, sizeof t);
    g.ts->Unmap(0, nullptr);
    rb->Map(0, nullptr, (void**) &p);
    UINT64 bad = 0, first = ~0ull;
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            uint32_t v;
            memcpy(&v, p + UINT64(y) * pitch + x * 4, 4);
            if (v != tex[UINT64(y) * w + x])
            {
                if (!bad)
                    first = UINT64(y) * w + x;
                ++bad;
            }
        }
    rb->Unmap(0, nullptr);
    const double raw = double(w) * h * 4;
    printf("%s %ux%u: raw %.2f MB packed %.2f MB (%.2fx, %.1f%%) encode %.3f ms decode %.3f ms: %s", name, w, h,
           raw / 1e6, bytes / 1e6, raw / bytes, 100.0 * bytes / raw, double(t[1] - t[0]) * 1e3 / g.freq,
           double(t[3] - t[2]) * 1e3 / g.freq, bad ? "MISMATCH" : "bit-exact");
    if (bad)
        printf(" (%llu texels, first at %llu: %08X)", bad, first, tex[first]);
    printf("\n");
    return bad ? 1 : 0;
}

int main(int argc, char** argv)
{
    int adapter = 0;
    bool synthetic = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--adapter") && i + 1 < argc)
            adapter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--synthetic"))
            synthetic = true;
        else
            args.push_back(argv[i]);
    }
    ComPtr<IDXGIFactory6> f;
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapters1(adapter, &a)))
        return fprintf(stderr, "no adapter %d\n", adapter), 2;
    DXGI_ADAPTER_DESC1 ad {};
    a->GetDesc1(&ad);
    printf("adapter %d: %ls\n", adapter, ad.Description);
    Gpu g;
    if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev))))
        return fprintf(stderr, "device failed\n"), 2;
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q));
    g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc));
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr, IID_PPV_ARGS(&g.cl));
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    D3D12_QUERY_HEAP_DESC qh { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0 };
    g.dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.qh));
    g.ts = g.Buffer(64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.q->GetTimestampFrequency(&g.freq);
    MvPack pk;
    if (!pk.Init(g.dev.Get()))
        return fprintf(stderr, "MvPack init: %s\n", pk.Error()), 2;
    int fails = 0;
    for (size_t i = 0; i + 2 < args.size(); i += 3)
    {
        const UINT w = UINT(atoi(args[i + 1].c_str())), h = UINT(atoi(args[i + 2].c_str()));
        std::vector<uint32_t> tex(size_t(w) * h);
        FILE* fp = fopen(args[i].c_str(), "rb");
        if (!fp || fread(tex.data(), 4, tex.size(), fp) != tex.size())
            return fprintf(stderr, "read %s failed\n", args[i].c_str()), 2;
        fclose(fp);
        const char* n = strrchr(args[i].c_str(), '\\');
        fails += Test(g, pk, n ? n + 1 : args[i].c_str(), tex, w, h);
    }
    if (synthetic)
    {
        const UINT w = 1283, h = 727; // not multiples of the tile
        std::vector<uint32_t> t(size_t(w) * h);
        std::mt19937 rng(1);
        auto run = [&](const char* n) { fails += Test(g, pk, n, t, w, h); };
        for (auto& v : t)
            v = 0x3C003C00;
        run("constant");
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                // halves around zero: both signs, -0 and +0, denormals
                const int a = int(x) - int(w / 2), b = int(y) - int(h / 2);
                const uint32_t ha = a < 0 ? 0x8000u | uint32_t(-a) : uint32_t(a), hb = b < 0 ? 0x8000u | uint32_t(-b) : uint32_t(b);
                t[size_t(y) * w + x] = ha | hb << 16;
            }
        run("ramps across zero");
        const uint32_t special[] = { 0x7C00, 0xFC00, 0x7E00, 0x7C01, 0xFFFF, 0x8000, 0x0000, 0x0001, 0x8001, 0x03FF, 0x7BFF };
        for (auto& v : t)
            v = special[rng() % 11] | special[rng() % 11] << 16;
        run("inf / NaN / denormal / zeros");
        for (auto& v : t)
            v = rng();
        run("noise");
    }
    printf(fails ? "FAILED %d\n" : "all bit-exact\n", fails);
    return fails ? 1 : 0;
}
