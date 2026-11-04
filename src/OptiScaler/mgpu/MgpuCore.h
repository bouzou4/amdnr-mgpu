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
    bool nrEveryFrame = false; // the presenter waits for the network's answer each frame
    bool temporal = false;     // send the render card's motion vectors; NR keeps a network history
    double uiGate = 0.15;      // HUD-less: above this share of differing pixels the frame runs whole-frame NR
};

// Upscaler inputs of one frame, taken on the render card at the upscaler's Evaluate.
struct Guides
{
    ID3D12Resource* motion = nullptr;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    UINT motionW = 0, motionH = 0; // active extent of the vectors
    UINT renderW = 0, renderH = 0;
    float mvScaleX = 1, mvScaleY = 1, jitterX = 0, jitterY = 0;
    bool reset = false, depthInverted = false;
};

// What NR gets on the display card for one frame. `src` is read, the result goes to `dst` (both in their states).
struct NrInput
{
    ID3D12Resource* src = nullptr;
    D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12Resource* dst = nullptr;
    D3D12_RESOURCE_STATES dstState = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12Resource* motion = nullptr; // null: no motion (no-motion carry)
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Guides guides;     // scalars of the frame `motion` belongs to (resources not valid here)
    bool reset = false; // history must not be used (resize, gap, camera cut)
    bool waitAnswer = false;
};

struct Env
{
    std::function<void(int level, const std::string&)> log; // 0 info, 1 warn, 2 error
    std::function<IDXGIFactory1*()> createFactory;            // unspoofed factory, caller releases
    std::function<void(const std::function<void()>&)> noSpoof; // runs fn with adapter spoofing off
    // NR on the display card, recorded and submitted on `queue`. True = wrote `in.dst`.
    std::function<bool(ID3D12Device*, ID3D12CommandQueue*, const NrInput& in)> nrFrame;
    std::function<bool()> nrWanted;
    std::function<std::string()> nrStatus;
    std::function<void()> onPresent; // game Present on the proxy (render thread), before the send
    std::wstring dumpDir; // frame dumps go here when mgpu_dump.trigger appears in it
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
    uint64_t hudlessFrames = 0, hudlessMissed = 0, gateFallbacks = 0, guideFrames = 0;
    double uiShare = -1; // share of pixels the UI composite took from the back buffer, last measured
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

// Render card, recorded on the game's command list. Each copies into the staging of the frame being built.
bool WantGuides();
bool WantHudless();
void CaptureGuides(ID3D12GraphicsCommandList* cl, const Guides& g);
void CaptureHudless(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, D3D12_RESOURCE_STATES state);
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
bool DumpTexture(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12Resource* tex, D3D12_RESOURCE_STATES state,
                 const std::wstring& base);
} // namespace Mgpu
