// Shared helpers for xapi_probe: adapters, D3D12 / D3D11 devices, results.
#pragma once
#include <windows.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

inline std::string Hr(HRESULT hr)
{
    char b[16];
    snprintf(b, sizeof b, "0x%08X", unsigned(hr));
    return b;
}

// One result line per probe: "RESULT <name> <OK|FAIL|INFO> <detail>".
inline void Result(const char* name, bool ok, const std::string& detail)
{
    printf("RESULT %-34s %-4s %s\n", name, ok ? "OK" : "FAIL", detail.c_str());
    fflush(stdout);
}
inline void Info(const char* name, const std::string& detail)
{
    printf("RESULT %-34s INFO %s\n", name, detail.c_str());
    fflush(stdout);
}

int PciBus(LUID luid);

struct Adapter
{
    ComPtr<IDXGIAdapter1> a;
    LUID luid {};
    int bus = -1;
    UINT outputs = 0;
    std::wstring name;
};

// R = AMD hardware adapter without outputs, D = the one with outputs.
bool PickAdapters(Adapter& r, Adapter& d);

struct D12
{
    ComPtr<ID3D12Device3> dev;
    ComPtr<ID3D12CommandQueue> direct, copy;
    ComPtr<ID3D12CommandAllocator> alloc, calloc;
    ComPtr<ID3D12GraphicsCommandList> list, clist;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    uint64_t fv = 0;
    uint64_t tsFreqDirect = 0, tsFreqCopy = 0;
    bool copyTs = false;

    bool Init(IDXGIAdapter1* a);
    void Begin(bool copyQueue);
    void Submit(bool copyQueue);
    bool Flush(bool copyQueue);
    bool WaitCpu(ID3D12Fence* f, uint64_t v, DWORD ms = 10000);
};

D3D12_RESOURCE_DESC BufDesc(uint64_t size, D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE);
D3D12_RESOURCE_DESC TexDesc(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE);
ComPtr<ID3D12Resource> Committed(ID3D12Device* dev, D3D12_HEAP_TYPE t, const D3D12_RESOURCE_DESC& d,
                                 D3D12_RESOURCE_STATES st, D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE,
                                 HRESULT* out = nullptr);

// Upload bytes into a buffer or a texture's subresource 0 (via an upload buffer + copy), blocking.
bool UploadBuffer(D12& d, ID3D12Resource* dst, const void* data, size_t size);
bool UploadTexture(D12& d, ID3D12Resource* dst, const void* data, UINT rowBytes, UINT rows);
// Copy a buffer range into a readback buffer and return it, blocking.
bool ReadbackBuffer(D12& d, ID3D12Resource* src, uint64_t offset, size_t size, std::vector<uint8_t>& out);

std::vector<uint8_t> Pattern(size_t size, uint32_t seed);
size_t Mismatch(const uint8_t* a, const uint8_t* b, size_t n);

struct D11
{
    ComPtr<ID3D11Device5> dev;
    ComPtr<ID3D11DeviceContext4> ctx;
    bool Init(IDXGIAdapter1* a);
};

void ProbesD3D(const Adapter& r, const Adapter& d, bool bench);
void ProbesVk(const Adapter& r, const Adapter& d, bool bench);
