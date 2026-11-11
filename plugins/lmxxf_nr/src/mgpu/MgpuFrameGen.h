// Frame generation on the display card: FSR frame generation (FFX API, OptiScaler's own modules) over the
// display card's swapchain, fed with the frames, depth and motion vectors that came over the ring.
#pragma once
#include "MgpuCore.h"
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace Mgpu
{
using Microsoft::WRL::ComPtr;

struct FgFrame
{
    ID3D12Resource* depth = nullptr; // R32_FLOAT, render size, NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* motion = nullptr; // NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* hudless = nullptr; // swapchain format, NON_PIXEL_SHADER_RESOURCE; null: UI is interpolated
    Guides g;
    double frameMs = 16.7;
    bool reset = false;
    bool hudMode = false; // context made for a HUD-less colour (frames without one pass null)
    std::chrono::steady_clock::time_point tGame {}; // the game's Present of this frame
};

class DisplayFg
{
  public:
    static bool Available();
    // The FG swapchain on the display queue in place of a plain one
    HRESULT CreateSwapChain(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
                            const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                            IDXGISwapChain4** out);
    // Before the real Present of a frame. in == null or enabled == false: this frame is presented without FG.
    void Frame(ID3D12Device* dev, ID3D12CommandQueue* q, IDXGISwapChain4* sc, const FgFrame* in, bool enabled);
    void ReleaseContext(); // before ResizeBuffers, with the presenter stopped
    void Destroy();        // after the swapchain's last use
    void WaitForPresents();
    // generated frames are copied for the frame dumps: n frames from the next one
    void ArmDump(UINT n) { _dumpArm = n; }
    void FlushDumps(ID3D12Device* dev, ID3D12CommandQueue* q, const std::wstring& dir);
    bool Active() const { return _sc != nullptr; }

    struct Timing
    {
        uint64_t real = 0, generated = 0;
        std::vector<float> intervals; // present callback to present callback, ms
        std::vector<float> latency;   // game Present -> callback of its real frame, ms
        // DXGI Presents on the display card (FFX's present thread with FG, the presenter without)
        uint64_t dxgiReal = 0, dxgiGenerated = 0, dxgiUnknown = 0;
        std::vector<float> dxgiIntervals; // ms between DXGI Presents
        std::vector<float> dxgiLatency;   // game Present -> DXGI Present of that frame, ms
        std::vector<float> genToReal, realToGen; // DXGI Present gaps by kind, ms
    };
    Timing TakeTiming();
    void NoteGamePresent(uint64_t fgFrameId, std::chrono::steady_clock::time_point t);
    void OnDxgiPresent(); // from the DXGI Present detour
    // the DXGI Present detour (diagnostics); original = the host's unhooked CreateSwapChainForHwnd
    static void InstallPresentTiming(PFN_CreateSwapChainForHwnd original, IDXGIFactory2* f, ID3D12CommandQueue* q);

  private:
    bool EnsureContext(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, uint32_t flags, DXGI_FORMAT hudFmt,
                       const std::string& version);
    uint32_t OnPresent(void* params);
    uint32_t OnGenerate(void* params);

    void* _sc = nullptr; // ffxContext of the swapchain
    void* _fg = nullptr; // ffxContext of frame generation
    IDXGISwapChain4* _scObj = nullptr; // the FG swapchain (not owned)
    UINT _w = 0, _h = 0;
    DXGI_FORMAT _fmt = DXGI_FORMAT_UNKNOWN, _hudFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t _flags = 0;
    std::string _version; // provider preference the context was made with
    bool _loggedVersions = false;
    uint64_t _frameId = 0;
    bool _lastEnabled = false, _loggedFail = false;
    ComPtr<ID3D12CommandAllocator> _alloc[4];
    ComPtr<ID3D12GraphicsCommandList> _list;
    ComPtr<ID3D12Fence> _fence;
    UINT64 _fenceValue[4] {}, _counter = 0;

    std::mutex _tm;
    Timing _timing;
    std::chrono::steady_clock::time_point _lastCb {};
    static constexpr UINT kTimes = 64;
    std::chrono::steady_clock::time_point _gameT[kTimes] {};
    uint64_t _gameId[kTimes] {};
    struct Pending
    {
        bool generated = false;
        std::chrono::steady_clock::time_point tGame {};
    };
    std::vector<Pending> _pending; // presents FFX (or the presenter) will make, in order
    std::chrono::steady_clock::time_point _lastDxgi {};
    bool _pendingFgMode = false;
    int _lastKind = -1; // 0 real, 1 generated

    // generated frame dumps
    std::atomic<UINT> _dumpArm { 0 };
    static constexpr UINT kDumps = 6;
    ComPtr<ID3D12Resource> _dump[kDumps];
    uint64_t _dumpId[kDumps] {};
    bool _dumpGen[kDumps] {};
    std::atomic<UINT> _dumpCount { 0 };
};

// Live switches for batched probes: <dumpDir>\mgpu_live.txt with lines "fg=0|1", "nr=0|1" (polled)
bool PollLive(const std::wstring& dir); // true: a switch changed
int LiveFg(); // -1: setting
int LiveNr();
int LiveFgHud(); // "fghud=0|1": HUD-less colour to FFX, -1: setting
int LiveFgSyn(); // "fgsyn=<mask>": 1 synthetic depth, 2 synthetic motion, -1: setting
int LiveFgOrder(); // "fgorder=0|1": 1 configure before prepare (4.0.2 order, default), 0 prepare first
int LiveFgVer();   // "fgver=<major>": FG provider of that major version (3: FSR 3.1), 0 / -1: setting
void SetGameFg(bool on); // the game's own FG presents through the proxy: no display FG
bool GameFg();
} // namespace Mgpu
