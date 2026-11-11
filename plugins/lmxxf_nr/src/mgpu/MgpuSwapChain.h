// Proxy swapchain: back buffers on the render card, frames sent through a cross-adapter ring to the
// display card, which owns the real flip-model swapchain on the game's window.
#pragma once
#include "MgpuCore.h"
#include "MgpuLatency.h"
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>

namespace Mgpu
{
using Microsoft::WRL::ComPtr;
class DisplayFg;

class DECLSPEC_UUID("6d1b3a52-7f0e-4c8b-9a41-2f6b8e0d5c13") ProxySwapChain final : public IDXGISwapChain4
{
  public:
    static HRESULT Create(PFN_CreateSwapChainForHwnd original, IDXGIFactory2* factory, ID3D12CommandQueue* gameQueue,
                          HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                          IDXGIOutput* restrict, IDXGISwapChain1** out);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override;
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override;
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** pp) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fs, IDXGIOutput* t) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** t) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* m) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override;
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** pp) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override;
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT l) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* l) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override;
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT* mask,
                                             IUnknown* const* queues) override;
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* m) override;

    // Render card, on the game's list: copy into the staging of the back buffer being drawn.
    void RecordHudless(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, D3D12_RESOURCE_STATES state);
    void RecordGuides(ID3D12GraphicsCommandList* cl, const Guides& g);
    static ProxySwapChain* Active();

  private:
    ProxySwapChain() = default;
    ~ProxySwapChain();
    HRESULT Init(PFN_CreateSwapChainForHwnd original, IDXGIFactory2* factory, ID3D12CommandQueue* gameQueue, HWND hwnd,
                 const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict);
    HRESULT CreateBuffers(bool keepGameBuffers = false);
    void ReleaseBuffers(bool keepGameBuffers = false, bool retire = false);
    void ReleaseRetired();
    void Alive(const char* where);
    HRESULT CreateFallbackLists();
    HRESULT CreateIngestLists();
    HRESULT RenewReal();
    HRESULT SwapReal(bool fg);
    HRESULT EnterFallback();
    HRESULT PresentFallback(UINT sync, UINT flags);
    void LogOutput();
    void StartPresenter();
    void StopPresenter();
    void PresenterLoop();
    void DrainRender();
    bool WaitSend(UINT64 g);
    HRESULT DoPresent(UINT sync, UINT flags);

    static constexpr UINT K = 3; // ring slots

    std::atomic<ULONG> _ref { 1 };
    HWND _hwnd = nullptr;
    PFN_CreateSwapChainForHwnd _original = nullptr;
    ComPtr<IDXGIFactory2> _factory;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC _fsDesc {};
    bool _hasFs = false;
    DXGI_SWAP_CHAIN_DESC1 _gameDesc {};
    UINT _gameCount = 2;

    // render card
    ComPtr<ID3D12Device> _rdev;
    ComPtr<ID3D12CommandQueue> _gameQueue, _rcopy;
    ComPtr<ID3D12Fence> _rgameFence; // game queue -> copy queue
    UINT64 _rgameValue = 0;
    ComPtr<ID3D12Fence> _filledR, _releasedR;
    std::vector<ComPtr<ID3D12Resource>> _bb;
    std::vector<UINT64> _bbFrame; // frame id whose send copy last read each back buffer
    UINT _bbIndex = 0;
    ComPtr<ID3D12Resource> _slotR[K];
    std::vector<ComPtr<ID3D12CommandAllocator>> _rAlloc;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> _rList; // [bb * K + slot]
    ComPtr<ID3D12QueryHeap> _rQuery;
    ComPtr<ID3D12Resource> _rReadback;
    UINT64 _rTsFreq = 0;
    bool _rTimestamps = false;

    // display card
    ComPtr<ID3D12Device> _ddev;
    ComPtr<ID3D12CommandQueue> _dq;
    ComPtr<IDXGISwapChain4> _real;
    UINT _realCount = 3;
    ComPtr<ID3D12Fence> _filledD, _releasedD, _dFence;
    UINT64 _dValue = 0;
    std::vector<ComPtr<ID3D12Resource>> _realBb;
    ComPtr<ID3D12Resource> _slotD[K];
    ComPtr<ID3D12Heap> _heapD, _heapR;
    std::vector<ComPtr<ID3D12CommandAllocator>> _dAlloc;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> _dList; // [slot * realCount + realBb]
    ComPtr<ID3D12QueryHeap> _dQuery;
    ComPtr<ID3D12Resource> _dReadback;
    UINT64 _dTsFreq = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT _fp {};
    UINT64 _slotBytes = 0, _payloadBytes = 0;
    bool _tearing = false;

    // ring state, guarded by _m
    std::mutex _m;
    std::condition_variable _cv;
    UINT64 _scheduled = 0, _consumed = 0;
    UINT64 _picked[K] {}; // last frame the display card took from each slot
    UINT _sync[K] {}, _flags[K] {};
    std::chrono::steady_clock::time_point _tSched[K];
    bool _stop = false;
    std::thread _thread;
    std::mutex _scMutex; // serialises the game's calls on the real swapchain (not the presenter's Present)
    std::atomic<BOOL> _fsCached { FALSE };
    UINT _presentFails = 0, _renewals = 0;
    UINT _sendStallLogs = 0, _idleSkips = 0, _idleSlow = 0;
    double _lastIdleMs = -1;
    static constexpr DWORD kPresentIdleMs = 1000; // under TdrDelay (2 s)
    std::atomic<bool> _tearingOff { false };
    std::mutex _fsM;
    ComPtr<IDXGIOutput> _fsOutput;

    HANDLE _waitable = nullptr; // game-facing frame latency semaphore
    UINT _maxLatency = 1;
    UINT _presentCount = 0;
    std::chrono::steady_clock::time_point _lastRender {}, _lastDisplay {};
    bool _broken = false;
    // Display card removed: the presenter stops, the next game Present moves to a swapchain on the render card
    std::atomic<bool> _lostPending { false };
    bool _lost = false;
    std::vector<ComPtr<ID3D12CommandAllocator>> _fAlloc;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> _fList; // [bb * realCount + realBb]
    std::atomic<bool> _nrReset { false }; // extent or format changed: no network history
    UINT _loggedCs = ~0u;
    std::vector<ComPtr<IUnknown>> _retired; // the previous size's resources, see ReleaseBuffers
    std::vector<class UiComposite*> _retiredUi;
    UINT _retireAfter = 0;
    bool _sizeHold = false; // window and swapchain sizes differ: no NR
    std::chrono::steady_clock::time_point _mismatchSince {};
    HRESULT _lastPresentHr = S_OK;
    std::atomic<bool> _occluded { false }; // minimised / occluded: the presenter drops frames
    std::atomic<int> _probeFrames { 0 }; // fenced removal checks after a (re)start
    std::atomic<UINT64> _dumpFrom { 0 }, _dumpTo { 0 }; // frame ids [from, to) to dump

    // HUD-less colour and motion vectors: render-card staging per game back buffer, extra regions in each slot.
    bool _wantHud = false, _wantMv = false;
    std::mutex _capM;
    std::vector<ComPtr<ID3D12Resource>> _hudR, _mvR, _graveR;
    std::vector<UINT64> _hudTag, _mvTag; // frame id each staging holds
    std::vector<Guides> _mvConst;
    UINT64 _hudOffset = 0, _mvOffset = 0, _mvCap = 0;
    ComPtr<ID3D12CommandAllocator> _rxAlloc[K];
    ComPtr<ID3D12GraphicsCommandList> _rxList[K];
    struct SlotMeta
    {
        bool hud = false, mv = false, dp = false;
        Guides g;
        D3D12_RESOURCE_DESC mvDesc {};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT dpFp {}; // depth plane 0 in its staging buffer, offset 0
    } _meta[K];
    // depth for frame generation: render-card staging buffers (plane 0 footprint) per game back buffer
    bool _wantDp = false;
    std::vector<ComPtr<ID3D12Resource>> _dpR;
    std::vector<UINT64> _dpTag;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> _dpFp;
    UINT64 _dpOffset = 0, _dpCap = 0;
    bool _loggedDpFmt = false;
    ComPtr<ID3D12Resource> _dpD, _fgZeroDepth, _fgZeroMv;
    DisplayFg* _fgen = nullptr;
    // FFX's game queue: never holds a wait on the network's answer (that hung the display card under a Present)
    ComPtr<ID3D12CommandQueue> _fq;
    ComPtr<ID3D12Fence> _fqFence;
    UINT64 _fqValue = 0;
    std::chrono::steady_clock::time_point _prevSched {}, _fgLog {};
    UINT64 _fgDumpAt = 0, _fgPresents = 0;
    // what the game set on the swapchain, for a real swapchain made later (SwapReal)
    bool _csSet = false;
    DXGI_COLOR_SPACE_TYPE _cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    DXGI_HDR_METADATA_TYPE _hdrType = DXGI_HDR_METADATA_TYPE_NONE;
    std::vector<uint8_t> _hdrData;
    // presenter time per frame spent on the FG swapchain, summed over a log window (ms)
    double _fgQueueWaitMs = 0, _fgIdleWaitMs = 0, _fgFrameMs = 0, _fgPresentMs = 0;
    bool FgStep(UINT64 g, UINT s, UINT m, const SlotMeta& meta, bool haveMv, bool haveDp, bool haveHud, bool useHud, bool nrWrote,
                bool sizeHold, bool dump, std::chrono::steady_clock::time_point tSched);
    bool _loggedHudFmt = false, _loggedMvFmt = false;

    // display card
    ComPtr<ID3D12Resource> _hudD, _mvD, _nrD, _bbD, _compD;
    std::vector<ComPtr<ID3D12Resource>> _graveD;
    ComPtr<ID3D12CommandAllocator> _dxAlloc[K], _duAlloc[K]; // ingest of the extras, UI pass
    ComPtr<ID3D12GraphicsCommandList> _dxList[K], _duList[K];
    class UiComposite* _ui = nullptr;
    UINT64 _gateUntil = 0;
    bool _gateOn = false;
    uint32_t _hudMissStreak = 0; // frames in a row without a HUD-less colour
    bool _wholeFallback = false;
    std::chrono::steady_clock::time_point _lastNr {};
    UINT64 _lastMvW = 0, _lastMvH = 0;
    RenderClock _clock; // render card frame completions (low latency)
    Pacer _pacer;
    int _llMode = -1; // MgpuLowLatency, switchable by trigger
    bool _earlySend = false;
};
} // namespace Mgpu
