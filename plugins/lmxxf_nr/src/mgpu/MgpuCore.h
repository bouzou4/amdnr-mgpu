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
    bool wholeFrameFallback = true; // HUD-less mode with no HUD-less colour for a while: NR on the whole frame
    bool presenter = true; // false: let Windows copy the frame (cross-adapter present)
    bool fallbackInline = true;
    bool nrEveryFrame = false; // the presenter waits for the network's answer each frame
    bool temporal = false;     // send the render card's motion vectors; NR keeps a network history
    double uiGate = 0.90;      // HUD-less: above this share of differing pixels the pair is wrong: no NR that frame
    unsigned maxLead = 1;      // frames the game may be ahead of the display card
    int lowLatency = 2;        // 1: Present throttles on the render card, 2: and paces the next frame (Pacer)
    bool earlySend = false;    // the send copy is queued before the wait for the display card
    bool presentIdle = false;  // the real Present goes only to an idle display queue (bounded CPU wait)
    int dred = 0;              // DRED on the display device: 1 breadcrumbs + page faults, 2 page faults only
    bool forwardOutputs = true; // the render adapter's EnumOutputs returns the display card's outputs
    // frame generation on the display card (FFX FG swapchain), needs depth and motion vectors
    bool frameGen = false;
    bool fgHudless = true; // the NR'd HUD-less colour as FFX's HUDLessColor (UI not interpolated)
    bool fgAsync = false, fgHdr = true, fgInfiniteDepth = true, fgDebug = false, fgSynthetic = false;
    float fgCameraNear = 0.1f, fgCameraFar = 100000.0f, fgVFov = 1.0472f;
    bool fgStartOn = true; // FG on before any live switch (MgpuFgOn)
    std::string fgVersion; // FG provider by version name prefix ("3.1"), empty: the host's default (the newest)
    float fgSyntheticScale = 1.0f; // synthetic depth / motion size relative to the frame
    bool fgOwnQueue = true; // FFX on its own queue (_fq); false: on the presenter's queue (_dq)
    bool fgSwapOnDemand = false; // the real swapchain is the FFX one only while FG is on (swapped live)
    // FFX swapchain pacing tuning; fgPtMargin < 0: FFX defaults
    float fgPtMargin = -1.0f, fgPtVariance = 0.1f;
    bool fgPtHybridSpin = false, fgPtFenceWait = false;
};

// FFX API of the host (OptiScaler's own modules), ffxContext* contexts
struct FfxFns
{
    uint32_t (*create)(void** context, void* desc, const void* memCb) = nullptr;
    uint32_t (*destroy)(void** context, const void* memCb) = nullptr;
    uint32_t (*configure)(void** context, const void* desc) = nullptr;
    uint32_t (*query)(void** context, void* desc) = nullptr;
    uint32_t (*dispatch)(void** context, const void* desc) = nullptr;
};

// Once per device: removed reason and, with DRED on, the page fault and the last breadcrumbs
void LogDeviceRemoved(ID3D12Device* device, const char* where);

// Upscaler inputs of one frame, taken on the render card at the upscaler's Evaluate.
struct Guides
{
    ID3D12Resource* motion = nullptr;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    UINT motionW = 0, motionH = 0; // active extent of the vectors
    UINT renderW = 0, renderH = 0;
    float mvScaleX = 1, mvScaleY = 1, jitterX = 0, jitterY = 0;
    bool reset = false, depthInverted = false, jitteredMv = false;
    ID3D12Resource* depth = nullptr; // frame generation only
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
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
    double* answerWaitMs = nullptr; // out: CPU wait for the network answer inside nrFrame
};

struct Env
{
    std::function<void(int level, const std::string&)> log; // 0 info, 1 warn, 2 error
    std::function<IDXGIFactory1*()> createFactory;            // unspoofed factory, caller releases
    // D3D12CreateDevice that the host neither redirects nor takes as the game's device
    std::function<HRESULT(IUnknown* adapter, REFIID riid, void** device)> createDevice;
    std::function<void(const std::function<void()>&)> noSpoof; // runs fn with adapter spoofing off
    // NR on the display card, recorded and submitted on `queue`. True = wrote `in.dst`.
    std::function<bool(ID3D12Device*, ID3D12CommandQueue*, const NrInput& in)> nrFrame;
    std::function<bool()> nrWanted;
    std::function<bool()> nrSettle; // before the present of a frame nrFrame ran on; false: do not present it
    std::function<void()> nrQuiesce; // NR's queued work is done: the resources it was given may be freed
    std::function<void(const char*)> nrIdle; // no NR this frame, and why (sequence dump requests log it)
    std::function<std::string()> nrStatus;
    std::function<void()> onPresent; // game Present on the proxy (render thread), before the send
    std::function<void(ID3D12Device*)> hookExecute; // latency probe: the game queue's submits
    std::wstring dumpDir; // frame dumps go here when mgpu_dump.trigger appears in it
    FfxFns ffx;
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
    double sendWaitMs = 0, answerWaitMs = 0;         // presenter CPU waits: the send landing, the network answer
    double sendGBs = 0, ingestGBs = 0;
    double latencyMs = 0;                            // proxy Present -> real Present (CPU)
    double nrMs = 0;                                 // CPU time of the NR record+submit
    uint32_t ringDepth = 0, ringSlots = 0;
    uint64_t slotBytes = 0;
    double renderBusy = -1, displayBusy = -1;        // 3D engine use %, -1 = unknown
    std::string nrStatus;
    uint64_t hudlessFrames = 0, hudlessMissed = 0, gateFallbacks = 0, guideFrames = 0, sendStalls = 0;
    double uiShare = -1; // share of pixels the UI composite took from the back buffer, last measured
    std::string nrSource; // what NR runs on (HUD-less mode: the HUD-less colour or the whole-frame fallback)
    bool fallback = false; // display card lost: single-GPU passthrough, no NR
    std::string fallbackReason;
    std::string output; // the display card's output desc
    bool outputHdr = false;
    uint32_t colorSpace = 0; // the game's SetColorSpace1
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

// The topology's display adapter (unspoofed factory) and the render card's LUID; null / zero without a pair
IDXGIAdapter1* DisplayAdapter();
LUID RenderLuid();

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
