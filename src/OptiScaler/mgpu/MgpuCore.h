// Multi-GPU core: the game renders on one card, the other card owns the swapchain and runs NR.
// Plugin-ready: it knows nothing of OptiScaler; MgpuHost.cpp fills Env and calls the entry points.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <functional>
#include <string>
#include <cstdint>
#include <mutex>

namespace Mgpu
{
struct Settings
{
    bool enabled = false;
    int renderBus = -1;  // -1: the card with no outputs
    int displayBus = -1; // -1: the card with outputs
    bool hudless = false;
    bool presenter = true; // false: let Windows copy the frame (cross-adapter present)
    bool fallbackInline = true;
};

struct Env
{
    std::function<void(int level, const std::string&)> log; // 0 info, 1 warn, 2 error
    std::function<IDXGIFactory1*()> createFactory;            // unspoofed factory, caller releases
    std::function<void(const std::function<void()>&)> noSpoof; // runs fn with adapter spoofing off
    // Whole-frame NR on the display card, recorded and submitted on `queue`. True = wrote `target`.
    std::function<bool(ID3D12Device*, ID3D12CommandQueue*, ID3D12Resource* target)> nrFrame;
    std::function<bool()> nrWanted;
    std::function<std::string()> nrStatus;
};

struct AdapterView
{
    std::string name;
    int bus = -1;
    LUID luid {};
    UINT outputs = 0;
    uint64_t vram = 0;
};

struct Stats
{
    bool topology = false, redirected = false, presenter = false, nrOnDisplay = false;
    AdapterView render, display;
    std::string state;
    uint32_t width = 0, height = 0, format = 0;
    uint64_t produced = 0, presented = 0, skipped = 0;
    double renderFrameMs = 0, displayFrameMs = 0;    // EMA of present-to-present
    double sendMs = 0, ingestMs = 0;                 // GPU time of the copy on each side
    double sendGBs = 0, ingestGBs = 0;
    double latencyMs = 0;                            // proxy Present -> real Present (CPU)
    double nrMs = 0;                                 // CPU time of the NR record+submit
    uint32_t ringDepth = 0, ringSlots = 0;
    uint64_t slotBytes = 0;
    double renderBusy = -1, displayBusy = -1;        // 3D engine use %, -1 = unknown
    std::string nrStatus;
};

void Configure(const Settings& s, const Env& e);
const Settings& CurrentSettings();
const Env& CurrentEnv();

// D3D12CreateDevice hook: returns the adapter the device should be created on.
IUnknown* RedirectAdapter(IUnknown* adapter);
// True while Mgpu itself creates a device: the hook must call the original untouched.
bool InternalCreate();
bool Redirected();

// Swapchain hook: S_OK with *out = proxy, or E_NOTIMPL when the swapchain is not ours.
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);
HRESULT CreateSwapChainForHwnd(PFN_CreateSwapChainForHwnd original, IDXGIFactory2* factory, ID3D12CommandQueue* queue,
                               HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict,
                               IDXGISwapChain1** out);

// NR runs on the display card's presenter (render card's NR must stay off).
bool NrOnDisplay();
Stats GetStats();

// internal
void Log(int level, const std::string& s);
struct Topology;
Topology* GetTopology();
ID3D12Device* DisplayDevice();
ID3D12CommandQueue* DisplayQueue();
Stats& MutableStats();
std::mutex& StatsMutex();
void StartLoadSampler();
} // namespace Mgpu
