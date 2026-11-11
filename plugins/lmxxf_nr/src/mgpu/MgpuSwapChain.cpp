#include "MgpuCommon.h"
#include "MgpuSwapChain.h"
#include "MgpuComposite.h"
#include "../GpuWait.h"
#include "MgpuLatency.h"

#include <format>

namespace Mgpu
{
static std::mutex s_statsMutex;
std::mutex& StatsMutex() { return s_statsMutex; }

static inline UINT64 AlignUp(UINT64 v, UINT64 a) { return (v + a - 1) & ~(a - 1); }
static inline double Ema(double prev, double v) { return prev <= 0 ? v : prev * 0.95 + v * 0.05; }
static void WaitFence(ID3D12Fence* f, UINT64 v, DWORD ms);
static bool WaitReuse(ID3D12Fence* f, UINT64 v, const char* what, const bool* stop = nullptr, const bool* lost = nullptr);

static std::atomic<ProxySwapChain*> s_active { nullptr };
ProxySwapChain* ProxySwapChain::Active() { return s_active.load(); }

bool WantHudless() { return CurrentSettings().hudless && s_active.load() != nullptr; }
bool WantGuides() { return CurrentSettings().temporal && s_active.load() != nullptr; }
void CaptureHudless(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, D3D12_RESOURCE_STATES state)
{
    if (auto* p = s_active.load())
        p->RecordHudless(cl, src, state);
}
void CaptureGuides(ID3D12GraphicsCommandList* cl, const Guides& g)
{
    if (auto* p = s_active.load())
        p->RecordGuides(cl, g);
}

// Same bits per texel and channel order: a texture copy between the two is legal.
static int FormatFamily(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return 1;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return 2;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 3;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
        return 5;
    default:
        return 0;
    }
}

static D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER v {};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    return v;
}

static ComPtr<ID3D12Resource> Tex(ID3D12Device* d, UINT64 w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl,
                                  D3D12_RESOURCE_STATES st)
{
    D3D12_RESOURCE_DESC t {};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = w;
    t.Height = h;
    t.DepthOrArraySize = 1;
    t.MipLevels = 1;
    t.Format = f;
    t.SampleDesc.Count = 1;
    t.Flags = fl;
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    ComPtr<ID3D12Resource> r;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &t, st, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static UINT RealFlags(UINT gameFlags, bool tearing)
{
    UINT f = gameFlags & ~(UINT) DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearing)
        f |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    return f;
}

HRESULT CreateSwapChainForHwnd(PFN_CreateSwapChainForHwnd original, IDXGIFactory2* factory, ID3D12CommandQueue* queue,
                               HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict,
                               IDXGISwapChain1** out)
{
    const auto& s = CurrentSettings();
    if (!s.enabled || !s.presenter || !Redirected() || !queue || !desc || desc->Width < 100 && desc->Width != 0)
        return E_NOTIMPL;
    ComPtr<ID3D12Device> dev;
    if (queue->GetDevice(IID_PPV_ARGS(&dev)) != S_OK)
        return E_NOTIMPL;
    auto* t = GetTopology();
    if (!t)
        return E_NOTIMPL;
    LUID l = dev->GetAdapterLuid();
    if (l.LowPart != MutableStats().render.luid.LowPart || l.HighPart != MutableStats().render.luid.HighPart)
        return E_NOTIMPL; // not the game's render-card queue
    HRESULT hr = ProxySwapChain::Create(original, factory, queue, hwnd, desc, fs, restrict, out);
    if (FAILED(hr))
        Log(2, std::format("proxy swapchain failed {:X}; falling back to the cross-adapter present", (uint32_t) hr));
    return hr;
}

HRESULT ProxySwapChain::Create(PFN_CreateSwapChainForHwnd original, IDXGIFactory2* factory,
                               ID3D12CommandQueue* gameQueue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict,
                               IDXGISwapChain1** out)
{
    auto* p = new ProxySwapChain();
    HRESULT hr = p->Init(original, factory, gameQueue, hwnd, desc, fs, restrict);
    if (FAILED(hr))
    {
        p->Release();
        return hr;
    }
    *out = p;
    return S_OK;
}

HRESULT ProxySwapChain::Init(PFN_CreateSwapChainForHwnd original, IDXGIFactory2* factory, ID3D12CommandQueue* gameQueue,
                             HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                             IDXGIOutput* restrict)
{
    _hwnd = hwnd;
    _original = original;
    _factory = factory;
    if (fs)
        _fsDesc = *fs, _hasFs = true;
    _gameDesc = *desc;
    _gameCount = (std::max)(2u, desc->BufferCount);
    _gameQueue = gameQueue;
    HRESULT hr = gameQueue->GetDevice(IID_PPV_ARGS(&_rdev));
    if (FAILED(hr))
        return hr;
    _ddev = DisplayDevice();
    _dq = DisplayQueue();
    if (!_ddev || !_dq)
        return E_FAIL;

    ComPtr<IDXGIFactory5> f5;
    BOOL tearing = FALSE;
    if (factory->QueryInterface(IID_PPV_ARGS(&f5)) == S_OK)
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    _tearing = tearing != FALSE;

    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.BufferCount = _realCount;
    d.Flags = RealFlags(desc->Flags, _tearing);
    d.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
    ComPtr<IDXGISwapChain1> sc1;
    hr = original(factory, _dq.Get(), hwnd, &d, fs, restrict, &sc1);
    if (FAILED(hr))
        return hr;
    if (FAILED(hr = sc1.As(&_real)))
        return hr;

    // Shared fences: filled (render signals), released (display signals).
    HANDLE h = nullptr;
    if (FAILED(hr = _rdev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                       IID_PPV_ARGS(&_filledR))) ||
        FAILED(hr = _rdev->CreateSharedHandle(_filledR.Get(), nullptr, GENERIC_ALL, nullptr, &h)))
        return hr;
    hr = _ddev->OpenSharedHandle(h, IID_PPV_ARGS(&_filledD));
    CloseHandle(h);
    if (FAILED(hr))
        return hr;
    if (FAILED(hr = _ddev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                       IID_PPV_ARGS(&_releasedD))) ||
        FAILED(hr = _ddev->CreateSharedHandle(_releasedD.Get(), nullptr, GENERIC_ALL, nullptr, &h)))
        return hr;
    hr = _rdev->OpenSharedHandle(h, IID_PPV_ARGS(&_releasedR));
    CloseHandle(h);
    if (FAILED(hr))
        return hr;
    if (FAILED(hr = _rdev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_rgameFence))) ||
        FAILED(hr = _ddev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_dFence))))
        return hr;

    D3D12_COMMAND_QUEUE_DESC q {};
    q.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(_rdev->CreateCommandQueue(&q, IID_PPV_ARGS(&_rcopy))))
    {
        q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(hr = _rdev->CreateCommandQueue(&q, IID_PPV_ARGS(&_rcopy))))
            return hr;
    }
    _rcopy->SetName(L"Mgpu send queue");
    q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    if (FAILED(hr = _ddev->CreateCommandQueue(&q, IID_PPV_ARGS(&_iq))))
        return hr;
    _iq->SetName(L"Mgpu ingest queue");
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = _ddev->CreateCommandQueue(&q, IID_PPV_ARGS(&_wq))))
        return hr;
    _wq->SetName(L"Mgpu work queue");
    if (FAILED(hr = _ddev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_iFence))) ||
        FAILED(hr = _ddev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_wFence))) ||
        FAILED(hr = _ddev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_drainFence))))
        return hr;
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 d3 {};
        _ddev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &d3, sizeof(d3));
        _iTimestamps = d3.CopyQueueTimestampQueriesSupported != FALSE;
        _iq->GetTimestampFrequency(&_iTsFreq);
    }
    _overlap = CurrentSettings().overlap;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 {};
    _rdev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    _rTimestamps = o3.CopyQueueTimestampQueriesSupported != FALSE;
    _rcopy->GetTimestampFrequency(&_rTsFreq);
    _dq->GetTimestampFrequency(&_dTsFreq);

    if (desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        _waitable = CreateSemaphoreW(nullptr, (LONG) _maxLatency, 64, nullptr);

    _wantHud = CurrentSettings().hudless;
    _wantMv = CurrentSettings().temporal;
    if (FAILED(hr = CreateBuffers()))
        return hr;
    StartPresenter();
    s_active = this;
    if (Latency::On() && CurrentEnv().hookExecute)
    {
        Latency::SetGameQueue(_gameQueue.Get());
        CurrentEnv().hookExecute(_rdev.Get());
    }
    {
        std::lock_guard g(StatsMutex());
        MutableStats().presenter = true;
    }
    {
        // Independent flip needs a window covering its monitor and per-monitor DPI awareness.
        RECT wr {};
        GetWindowRect(hwnd, &wr);
        MONITORINFO mi { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const auto aw = GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd));
        Log(0, std::format("window {}x{} at {},{}; monitor {}x{}; DPI awareness {} (2 = per monitor)", wr.right - wr.left,
                           wr.bottom - wr.top, wr.left, wr.top, mi.rcMonitor.right - mi.rcMonitor.left,
                           mi.rcMonitor.bottom - mi.rcMonitor.top, (int) aw));
    }
    Log(0, std::format("proxy swapchain {}x{} fmt {} game buffers {} real buffers {} tearing {} waitable {} hudless {} "
                       "motion {}",
                       _gameDesc.Width, _gameDesc.Height, (int) _gameDesc.Format, _gameCount, _realCount, _tearing,
                       _waitable != nullptr, _wantHud, _wantMv));
    LogOutput();
    return S_OK;
}

// The display card's output as the game would see it: HDR state, colour space, luminance
void ProxySwapChain::LogOutput()
{
    ComPtr<IDXGIOutput> o;
    ComPtr<IDXGIOutput6> o6;
    DXGI_OUTPUT_DESC1 d {};
    if (FAILED(_real->GetContainingOutput(&o)) || FAILED(o.As(&o6)) || FAILED(o6->GetDesc1(&d)))
    {
        Log(1, "display output: no IDXGIOutput6 desc");
        return;
    }
    const auto s = std::format("{}x{}, colour space {}, {} bits, luminance {:.0f}-{:.0f} nits (full frame {:.0f})",
                               d.DesktopCoordinates.right - d.DesktopCoordinates.left,
                               d.DesktopCoordinates.bottom - d.DesktopCoordinates.top, (int) d.ColorSpace,
                               d.BitsPerColor, d.MinLuminance, d.MaxLuminance, d.MaxFullFrameLuminance);
    Log(0, "display output " + s);
    std::lock_guard g(StatsMutex());
    MutableStats().output = s;
    MutableStats().outputHdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

HRESULT ProxySwapChain::CreateBuffers(bool keepGameBuffers)
{
    HRESULT hr;
    DXGI_SWAP_CHAIN_DESC1 rd {};
    _real->GetDesc1(&rd);
    _gameDesc.Width = rd.Width;
    _gameDesc.Height = rd.Height;
    _gameDesc.Format = rd.Format;
    _realCount = rd.BufferCount;
    _nrReset = true;
    for (auto& m : _meta)
        m = {};

    _realBb.resize(_realCount);
    for (UINT i = 0; i < _realCount; ++i)
        if (FAILED(hr = _real->GetBuffer(i, IID_PPV_ARGS(&_realBb[i]))))
            return hr;

    D3D12_RESOURCE_DESC t {};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = rd.Width;
    t.Height = rd.Height;
    t.DepthOrArraySize = 1;
    t.MipLevels = 1;
    t.Format = rd.Format;
    t.SampleDesc.Count = 1;
    t.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (_gameDesc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
        t.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    if (!keepGameBuffers)
    {
        _bb.resize(_gameCount);
        _bbFrame.assign(_gameCount, 0);
        _bbIndex = 0;
        for (UINT i = 0; i < _gameCount; ++i)
        {
            if (FAILED(hr = _rdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_PRESENT,
                                                           nullptr, IID_PPV_ARGS(&_bb[i]))))
                return hr;
            _bb[i]->SetName(std::format(L"Mgpu proxy back buffer {}", i).c_str());
        }
    }
    for (auto& p : _picked)
        p = 0;
    Log(0, std::format("buffers {}x{} fmt {}, game {} real {}{}", rd.Width, rd.Height, (int) rd.Format, _gameCount,
                       _realCount, _lost ? ", single-GPU fallback" : ""));
    if (_lost)
        return CreateFallbackLists();

    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    _rdev->GetCopyableFootprints(&t, 0, 1, 0, &_fp, &rows, &rowBytes, &total);
    _payloadBytes = UINT64(rows) * rowBytes;
    // Slot: back buffer, then the HUD-less colour (same footprint), then motion vectors (up to 4 bytes per pixel).
    UINT64 slot = AlignUp(total, 65536);
    _hudOffset = _mvOffset = _mvCap = 0;
    {
        std::lock_guard g(_capM);
        _hudR.clear();
        _mvR.clear();
        _hudTag.assign(_gameCount, 0);
        _mvTag.assign(_gameCount, 0);
        _mvConst.assign(_gameCount, Guides {});
        if (_wantHud)
        {
            _hudOffset = slot;
            slot += AlignUp(total, 65536);
            D3D12_RESOURCE_DESC s = t;
            s.Flags = D3D12_RESOURCE_FLAG_NONE;
            _hudR.resize(_gameCount);
            for (UINT i = 0; i < _gameCount; ++i)
            {
                if (FAILED(hr = _rdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &s, D3D12_RESOURCE_STATE_COMMON,
                                                               nullptr, IID_PPV_ARGS(&_hudR[i]))))
                    return hr;
                _hudR[i]->SetName(std::format(L"Mgpu hudless staging {}", i).c_str());
            }
        }
        if (_wantMv)
        {
            D3D12_RESOURCE_DESC m = t;
            m.Format = DXGI_FORMAT_R16G16_FLOAT;
            m.Flags = D3D12_RESOURCE_FLAG_NONE;
            UINT64 mt = 0;
            _rdev->GetCopyableFootprints(&m, 0, 1, 0, nullptr, nullptr, nullptr, &mt);
            _mvOffset = slot;
            _mvCap = AlignUp(mt, 65536);
            slot += _mvCap;
            _mvR.resize(_gameCount);
        }
    }
    _slotBytes = slot;

    D3D12_HEAP_DESC hd {};
    hd.SizeInBytes = _slotBytes * K;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    if (FAILED(hr = _ddev->CreateHeap(&hd, IID_PPV_ARGS(&_heapD))))
        return hr;
    _heapD->SetName(L"Mgpu ring heap D");
    HANDLE h = nullptr;
    if (FAILED(hr = _ddev->CreateSharedHandle(_heapD.Get(), nullptr, GENERIC_ALL, nullptr, &h)))
        return hr;
    hr = _rdev->OpenSharedHandle(h, IID_PPV_ARGS(&_heapR));
    CloseHandle(h);
    if (FAILED(hr))
        return hr;
    _heapR->SetName(L"Mgpu ring heap R");

    D3D12_RESOURCE_DESC b {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = _slotBytes;
    b.Height = 1;
    b.DepthOrArraySize = 1;
    b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    b.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    for (UINT s = 0; s < K; ++s)
    {
        if (FAILED(hr = _ddev->CreatePlacedResource(_heapD.Get(), s * _slotBytes, &b, D3D12_RESOURCE_STATE_COMMON,
                                                    nullptr, IID_PPV_ARGS(&_slotD[s]))) ||
            FAILED(hr = _rdev->CreatePlacedResource(_heapR.Get(), s * _slotBytes, &b, D3D12_RESOURCE_STATE_COMMON,
                                                    nullptr, IID_PPV_ARGS(&_slotR[s]))))
            return hr;
        _slotD[s]->SetName(std::format(L"Mgpu ring slot D {}", s).c_str());
        _slotR[s]->SetName(std::format(L"Mgpu ring slot R {}", s).c_str());
    }

    // Timestamps: two per slot on each side, resolved to readback at slot * 16.
    D3D12_QUERY_HEAP_DESC qd {};
    qd.Count = 2 * K;
    D3D12_RESOURCE_DESC rb = b;
    rb.Width = 16 * K;
    rb.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES rbp { D3D12_HEAP_TYPE_READBACK };
    if (_rTimestamps)
    {
        qd.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
        if (FAILED(_rdev->CreateQueryHeap(&qd, IID_PPV_ARGS(&_rQuery))) ||
            FAILED(_rdev->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&_rReadback))))
            _rQuery.Reset();
        else
            _rQuery->SetName(L"Mgpu send timestamps"), _rReadback->SetName(L"Mgpu send timestamp readback");
    }
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    if (FAILED(_ddev->CreateQueryHeap(&qd, IID_PPV_ARGS(&_dQuery))) ||
        FAILED(_ddev->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&_dReadback))))
        _dQuery.Reset();
    else
        _dQuery->SetName(L"Mgpu ingest timestamps"), _dReadback->SetName(L"Mgpu ingest timestamp readback");

    // Render side: one prerecorded copy per (back buffer, slot).
    _rAlloc.resize(_gameCount * K);
    _rList.resize(_gameCount * K);
    for (UINT i = 0; i < _gameCount; ++i)
        for (UINT s = 0; s < K; ++s)
        {
            UINT n = i * K + s;
            if (FAILED(hr = _rdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&_rAlloc[n]))) ||
                FAILED(hr = _rdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, _rAlloc[n].Get(), nullptr,
                                                     IID_PPV_ARGS(&_rList[n]))))
                return hr;
            auto* c = _rList[n].Get();
            _rAlloc[n]->SetName(L"Mgpu send allocator");
            c->SetName(std::format(L"Mgpu send list bb{} slot{}", i, s).c_str());
            if (_rQuery)
                c->EndQuery(_rQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2);
            D3D12_TEXTURE_COPY_LOCATION dst { _slotR[s].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint = _fp;
            D3D12_TEXTURE_COPY_LOCATION src { _bb[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            src.SubresourceIndex = 0;
            c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            if (_rQuery)
            {
                c->EndQuery(_rQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2 + 1);
                c->ResolveQueryData(_rQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2, 2, _rReadback.Get(), s * 16);
            }
            if (FAILED(hr = c->Close()))
                return hr;
        }

    if (FAILED(hr = CreateIngestLists()))
        return hr;

    // Extra regions: per-slot lists recorded at send (render) and at ingest (display).
    if (_wantHud || _wantMv)
    {
        for (UINT s = 0; s < K; ++s)
        {
            if (FAILED(hr = _rdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&_rxAlloc[s]))) ||
                FAILED(hr = _rdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, _rxAlloc[s].Get(), nullptr,
                                                     IID_PPV_ARGS(&_rxList[s]))) ||
                FAILED(hr = _rxList[s]->Close()) ||
                FAILED(hr = _ddev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_dxAlloc[s]))) ||
                FAILED(hr = _ddev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _dxAlloc[s].Get(), nullptr,
                                                     IID_PPV_ARGS(&_dxList[s]))) ||
                FAILED(hr = _dxList[s]->Close()) ||
                FAILED(hr = _ddev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_duAlloc[s]))) ||
                FAILED(hr = _ddev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _duAlloc[s].Get(), nullptr,
                                                     IID_PPV_ARGS(&_duList[s]))) ||
                FAILED(hr = _duList[s]->Close()))
                return hr;
            _rxAlloc[s]->SetName(L"Mgpu extras send allocator");
            _rxList[s]->SetName(std::format(L"Mgpu extras send list {}", s).c_str());
            _dxAlloc[s]->SetName(L"Mgpu extras ingest allocator");
            _dxList[s]->SetName(std::format(L"Mgpu extras ingest list {}", s).c_str());
            _duAlloc[s]->SetName(L"Mgpu UI allocator");
            _duList[s]->SetName(std::format(L"Mgpu UI list {}", s).c_str());
        }
    }
    if (_wantHud)
    {
        const auto ns = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        _hudD = Tex(_ddev.Get(), rd.Width, rd.Height, rd.Format, D3D12_RESOURCE_FLAG_NONE, ns);
        _nrD = Tex(_ddev.Get(), rd.Width, rd.Height, rd.Format, D3D12_RESOURCE_FLAG_NONE, ns);
        _bbD = Tex(_ddev.Get(), rd.Width, rd.Height, rd.Format, D3D12_RESOURCE_FLAG_NONE, ns);
        _compD = Tex(_ddev.Get(), rd.Width, rd.Height, rd.Format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!_ui)
        {
            _ui = new UiComposite();
            if (!_ui->Init(_ddev.Get(), rd.Format))
            {
                delete _ui;
                _ui = nullptr;
            }
        }
        for (auto [r, n] : { std::pair { _hudD.Get(), L"Mgpu hudless D" }, { _nrD.Get(), L"Mgpu nr D" },
                             { _bbD.Get(), L"Mgpu bb copy D" }, { _compD.Get(), L"Mgpu composite D" } })
            if (r)
                r->SetName(n);
        if (!_hudD || !_nrD || !_bbD || !_compD || !_ui)
        {
            Log(2, "HUD-less resources failed: whole frame");
            _wantHud = false;
        }
    }

    if (_overlap > 0 && FAILED(hr = CreateOverlapBuffers()))
        return hr;

    std::lock_guard g(StatsMutex());
    auto& st = MutableStats();
    st.width = rd.Width;
    st.height = rd.Height;
    st.format = rd.Format;
    st.ringSlots = K;
    st.slotBytes = _slotBytes;
    return S_OK;
}

HRESULT ProxySwapChain::CreateFallbackLists()
{
    HRESULT hr;
    _fAlloc.resize(_gameCount * _realCount);
    _fList.resize(_gameCount * _realCount);
    for (UINT i = 0; i < _gameCount; ++i)
        for (UINT m = 0; m < _realCount; ++m)
        {
            const UINT n = i * _realCount + m;
            if (FAILED(hr = _rdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_fAlloc[n]))) ||
                FAILED(hr = _rdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _fAlloc[n].Get(), nullptr,
                                                     IID_PPV_ARGS(&_fList[n]))))
                return hr;
            auto* c = _fList[n].Get();
            _fAlloc[n]->SetName(L"Mgpu fallback allocator");
            c->SetName(std::format(L"Mgpu fallback list bb{} real{}", i, m).c_str());
            D3D12_RESOURCE_BARRIER b[2] = { Tr(_bb[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                            Tr(_realBb[m].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                               D3D12_RESOURCE_STATE_COPY_DEST) };
            c->ResourceBarrier(2, b);
            c->CopyResource(_realBb[m].Get(), _bb[i].Get());
            for (auto& x : b)
                std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
            c->ResourceBarrier(2, b);
            if (FAILED(hr = c->Close()))
                return hr;
        }
    std::lock_guard g(StatsMutex());
    auto& st = MutableStats();
    st.width = _gameDesc.Width;
    st.height = _gameDesc.Height;
    st.format = _gameDesc.Format;
    return S_OK;
}

// Display side: one prerecorded ingest per (slot, real back buffer)
HRESULT ProxySwapChain::CreateIngestLists()
{
    HRESULT hr;
    _dAlloc.resize(K * _realCount);
    _dList.resize(K * _realCount);
    for (UINT s = 0; s < K; ++s)
        for (UINT m = 0; m < _realCount; ++m)
        {
            UINT n = s * _realCount + m;
            if (FAILED(hr = _ddev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_dAlloc[n]))) ||
                FAILED(hr = _ddev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _dAlloc[n].Get(), nullptr,
                                                     IID_PPV_ARGS(&_dList[n]))))
                return hr;
            auto* c = _dList[n].Get();
            _dAlloc[n]->SetName(L"Mgpu ingest allocator");
            c->SetName(std::format(L"Mgpu ingest list slot{} real{}", s, m).c_str());
            D3D12_RESOURCE_BARRIER br {};
            br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            br.Transition = { _realBb[m].Get(), 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST };
            c->ResourceBarrier(1, &br);
            if (_dQuery)
                c->EndQuery(_dQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2);
            D3D12_TEXTURE_COPY_LOCATION dst { _realBb[m].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src { _slotD[s].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint = _fp;
            c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            if (_dQuery)
            {
                c->EndQuery(_dQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2 + 1);
                c->ResolveQueryData(_dQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2, 2, _dReadback.Get(), s * 16);
            }
            std::swap(br.Transition.StateBefore, br.Transition.StateAfter);
            c->ResourceBarrier(1, &br);
            if (FAILED(hr = c->Close()))
                return hr;
        }
    return S_OK;
}

// DXGI left or refused exclusive fullscreen on its own (the window not in front) and rejects presents until the real
// swapchain is resized: the game's buffers are ours, so the presenter resizes the real one itself
HRESULT ProxySwapChain::RenewReal()
{
    _dq->Signal(_dFence.Get(), ++_dValue);
    if (!GpuWaitDone(_dFence.Get(), _dValue))
        return DXGI_ERROR_WAS_STILL_DRAWING; // the lists may still run: no resize now
    _dList.clear();
    _dAlloc.clear();
    _realBb.clear();
    HRESULT hr = _real->ResizeBuffers(_realCount, _gameDesc.Width, _gameDesc.Height, _gameDesc.Format,
                                      RealFlags(_gameDesc.Flags, _tearing));
    BOOL fs = FALSE;
    _real->GetFullscreenState(&fs, nullptr);
    _fsCached = fs;
    _realBb.resize(_realCount);
    for (UINT i = 0; i < _realCount && SUCCEEDED(hr); ++i)
        hr = _real->GetBuffer(i, IID_PPV_ARGS(&_realBb[i]));
    if (SUCCEEDED(hr))
        hr = CreateIngestLists();
    Log(FAILED(hr) ? 2 : 1, std::format("real swapchain resized after a refused present: {:X}, fullscreen {}", (uint32_t) hr, fs));
    return hr;
}
void ProxySwapChain::ReleaseBuffers(bool keepGameBuffers, bool retire)
{
    // retire: kept alive until the presenter has shown a few frames on the new buffers
    auto drop = [&](auto& p)
    {
        if (retire && p)
            _retired.push_back(p);
        p.Reset();
    };
    auto dropAll = [&](auto& v)
    {
        for (auto& p : v)
            drop(p);
        v.clear();
    };
    dropAll(_fList);
    dropAll(_fAlloc);
    if (retire && _ui)
        _retiredUi.push_back(_ui);
    else
        delete _ui; // its pipeline is per format
    _ui = nullptr;
    dropAll(_rList);
    dropAll(_rAlloc);
    dropAll(_dList);
    dropAll(_dAlloc);
    for (UINT s = 0; s < K; ++s)
    {
        drop(_slotR[s]);
        drop(_slotD[s]);
        drop(_rxList[s]);
        drop(_rxAlloc[s]);
        drop(_dxList[s]);
        drop(_dxAlloc[s]);
        drop(_duList[s]);
        drop(_duAlloc[s]);
    }
    drop(_heapR);
    drop(_heapD);
    drop(_rQuery);
    drop(_rReadback);
    drop(_dQuery);
    drop(_dReadback);
    if (!keepGameBuffers)
        _bb.clear();
    _realBb.clear();
    {
        std::lock_guard g(_capM);
        dropAll(_hudR);
        dropAll(_mvR);
        dropAll(_graveR);
    }
    for (UINT s = 0; s < K; ++s)
    {
        drop(_colO[s]);
        drop(_hudO[s]);
        drop(_mvO[s]);
        drop(_compO[s]);
        drop(_iList[s]);
        drop(_iAlloc[s]);
        drop(_wList[s]);
        drop(_wAlloc[s]);
        drop(_wpList[s]);
        drop(_wpAlloc[s]);
        drop(_pList[s]);
        drop(_pAlloc[s]);
    }
    drop(_iQuery);
    drop(_iReadback);
    drop(_hudD);
    drop(_mvD);
    drop(_nrD);
    drop(_bbD);
    drop(_compD);
    dropAll(_graveD);
}

void ProxySwapChain::ReleaseRetired()
{
    _retired.clear();
    for (auto* u : _retiredUi)
        delete u;
    _retiredUi.clear();
}
void ProxySwapChain::RecordHudless(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, D3D12_RESOURCE_STATES state)
{
    std::lock_guard g(_capM);
    const UINT i = _bbIndex;
    if (!_wantHud || i >= _hudR.size() || !_hudR[i] || !src || !cl)
        return;
    const auto d = src->GetDesc();
    if (d.Width != _gameDesc.Width || d.Height != _gameDesc.Height ||
        FormatFamily(d.Format) != FormatFamily(_gameDesc.Format) || FormatFamily(d.Format) == 0)
    {
        if (!_loggedHudFmt)
            Log(1, std::format("HUD-less candidate {}x{} fmt {} does not match the back buffer {}x{} fmt {}: skipped",
                               d.Width, d.Height, (int) d.Format, _gameDesc.Width, _gameDesc.Height,
                               (int) _gameDesc.Format));
        _loggedHudFmt = true;
        return;
    }
    D3D12_RESOURCE_BARRIER b[2] = { Tr(_hudR[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                                    Tr(src, state, D3D12_RESOURCE_STATE_COPY_SOURCE) };
    const UINT nb = state == D3D12_RESOURCE_STATE_COPY_SOURCE ? 1 : 2;
    cl->ResourceBarrier(nb, b);
    D3D12_TEXTURE_COPY_LOCATION dl { _hudR[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION sl { src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    for (auto& x : b)
        std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    cl->ResourceBarrier(nb, b);
    _hudTag[i] = _scheduled + 1;
}

void ProxySwapChain::RecordGuides(ID3D12GraphicsCommandList* cl, const Guides& gd)
{
    std::lock_guard g(_capM);
    const UINT i = _bbIndex;
    if (!_wantMv || i >= _mvR.size() || !gd.motion || !cl)
        return;
    auto d = gd.motion->GetDesc();
    UINT64 need = 0;
    D3D12_RESOURCE_DESC m = d;
    m.Flags = D3D12_RESOURCE_FLAG_NONE;
    m.MipLevels = 1;
    m.DepthOrArraySize = 1;
    m.Alignment = 0;
    _rdev->GetCopyableFootprints(&m, 0, 1, 0, nullptr, nullptr, nullptr, &need);
    if (FormatFamily(d.Format) != 5 || need > _mvCap || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        if (!_loggedMvFmt)
            Log(1, std::format("motion vectors {}x{} fmt {} not supported (R16G16 float, up to the frame's size): no "
                               "temporal input",
                               d.Width, d.Height, (int) d.Format));
        _loggedMvFmt = true;
        return;
    }
    if (!_mvR[i] || _mvR[i]->GetDesc().Width != m.Width || _mvR[i]->GetDesc().Height != m.Height ||
        _mvR[i]->GetDesc().Format != m.Format)
    {
        if (_mvR[i])
            _graveR.push_back(_mvR[i]);
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        _mvR[i].Reset();
        if (FAILED(_rdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &m, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&_mvR[i]))))
            return;
        _mvR[i]->SetName(std::format(L"Mgpu motion staging {}", i).c_str());
        Log(0, std::format("motion staging {} {}x{} fmt {}", i, m.Width, m.Height, (int) m.Format));
    }
    D3D12_RESOURCE_BARRIER b[2] = { Tr(_mvR[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                                    Tr(gd.motion, gd.motionState, D3D12_RESOURCE_STATE_COPY_SOURCE) };
    const UINT nb = gd.motionState == D3D12_RESOURCE_STATE_COPY_SOURCE ? 1 : 2;
    cl->ResourceBarrier(nb, b);
    D3D12_TEXTURE_COPY_LOCATION dl { _mvR[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION sl { gd.motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    for (auto& x : b)
        std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    cl->ResourceBarrier(nb, b);
    _mvConst[i] = gd;
    _mvConst[i].motion = nullptr;
    _mvTag[i] = _scheduled + 1;
}

static void WaitFence(ID3D12Fence* f, UINT64 v, DWORD ms)
{
    if (!f || f->GetCompletedValue() >= v)
        return;
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (f->SetEventOnCompletion(v, e) == S_OK)
        WaitForSingleObject(e, ms);
    CloseHandle(e);
}

// Before a slot's allocators are reset: its previous work must be done. No fixed cap (a 500 ms cap reset them in
// flight); false only on stop / device loss / 60 s, and the caller then skips the reuse.
static bool WaitReuse(ID3D12Fence* f, UINT64 v, const char* what, const bool* stop, const bool* lost)
{
    struct C { const bool *stop, *lost; } c { stop, lost };
    double ms = 0;
    const bool done = GpuWaitDone(f, v, 60000, [](void* x) {
        auto* c = static_cast<C*>(x);
        return (c->stop && *c->stop) || (c->lost && *c->lost); }, &c, &ms);
    static std::atomic<int> logged { 0 };
    if ((ms > 400 || !done) && logged++ < 20)
        Log(done ? 1 : 2, std::format("{}: slot reuse waited {:.0f} ms for {} (done {})", what, ms, v, done));
    return done;
}

// CPU wait for frame g's send (the render card's cross-adapter fence). false: stop / lost / 60 s, retry the loop.
bool ProxySwapChain::WaitSend(UINT64 g)
{
    double ms = 0;
    const bool done = GpuWaitDone(_filledD.Get(), g, 60000, [](void* x) {
        auto* p = static_cast<ProxySwapChain*>(x);
        return p->_stop || p->_lost; }, this, &ms);
    {
        std::lock_guard sg(StatsMutex());
        auto& st = MutableStats();
        if (ms > 250)
            st.sendStalls++;
        else
            st.sendWaitMs = Ema(st.sendWaitMs, ms); // hitches stay out of the per-frame figure
    }
    if ((ms > 250 || !done) && _sendStallLogs++ < 30)
        Log(done ? 1 : 2, std::format("frame {}: send landed after {:.0f} ms (render card hitch), nothing queued on the "
                                      "display card meanwhile (done {})", g, ms, done));
    return done;
}

void ProxySwapChain::DrainRender()
{
    if (_gameQueue && _rgameFence)
    {
        _gameQueue->Signal(_rgameFence.Get(), ++_rgameValue);
        GpuWaitDone(_rgameFence.Get(), _rgameValue);
    }
    if (!_lost && !GpuWaitDone(_filledR.Get(), _scheduled, 60000,
                               [](void* x) { return static_cast<ProxySwapChain*>(x)->_lost; }, this))
        Log(2, "drain: the send queue did not finish in 60 s");
}

// Diagnostics: logs the first point after a resize where the display device is found removed
void ProxySwapChain::Alive(const char* where)
{
    if (_lost || !_ddev)
        return;
    const HRESULT r = _ddev->GetDeviceRemovedReason();
    if (FAILED(r))
        Log(2, std::format("display device removed ({:X}) by: {}", (uint32_t) r, where));
}

void ProxySwapChain::StartPresenter()
{
    _probeFrames = 4;
    _stop = false;
    _stopO = false;
    _pipeline = _overlap > 0 && _colO[0] ? 1 : 0;
    if (_pipeline)
    {
        {
            std::lock_guard g(_m);
            _taken = _consumed;
        }
        {
            std::lock_guard g(_pm);
            _wk.clear();
            _pq.clear();
            _inFlight = 0;
            _presentedG = 0;
            for (auto& f : _slotFrameO)
                f = 0;
        }
        _thread = std::thread([this] { IngestLoop(); });
        _wthread = std::thread([this] { WorkLoop(); });
        _pthread = std::thread([this] { PresentLoop(); });
    }
    else
        _thread = std::thread([this] { PresenterLoop(); });
}

void ProxySwapChain::StopPresenter()
{
    {
        std::lock_guard g(_m);
        _stop = true;
    }
    {
        std::lock_guard g(_pm);
        _stopO = true;
    }
    _cv.notify_all();
    _pcv.notify_all();
    for (auto* t : { &_thread, &_wthread, &_pthread })
        if (t->joinable())
            t->join();
    if (_dq && _dFence)
    {
        _dq->Signal(_dFence.Get(), ++_dValue);
        GpuWaitDone(_dFence.Get(), _dValue);
    }
    // the overlap queues: frames taken and not presented
    for (auto* q : { _iq.Get(), _wq.Get() })
        if (_pipeline && q && _drainFence)
        {
            q->Signal(_drainFence.Get(), ++_drainValue);
            GpuWaitDone(_drainFence.Get(), _drainValue);
        }
    // the network may still read this side's motion vectors on its own queue
    if (CurrentEnv().nrQuiesce)
        CurrentEnv().nrQuiesce();
}

static double ReadTs(ID3D12Resource* rb, UINT slot, UINT64 freq)
{
    if (!rb || !freq)
        return -1;
    UINT64* p = nullptr;
    D3D12_RANGE r { slot * 16, slot * 16 + 16 };
    if (rb->Map(0, &r, (void**) &p) != S_OK)
        return -1;
    double ms = p[slot * 2 + 1] > p[slot * 2] ? double(p[slot * 2 + 1] - p[slot * 2]) * 1000.0 / double(freq) : -1;
    D3D12_RANGE w { 0, 0 };
    rb->Unmap(0, &w);
    return ms;
}

void ProxySwapChain::PresenterLoop()
{
    SetThreadDescription(GetCurrentThread(), L"Mgpu presenter");
    const auto& env = CurrentEnv();
    UINT64 slotDisplayValue[K] {};
    bool slotCounted[K] {};
    UINT64 slotFrame[K] {}, slotGame[K] {}; // the frame last presented from each slot and its game fence value
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const double qpcMs = 1000.0 / double(qf.QuadPart);
    double toDisplayMs = -1, toPresentMs = -1;
    const auto ns = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (;;)
    {
        std::unique_lock lk(_m);
        _cv.wait(lk, [&] { return _stop || _scheduled > _consumed; });
        if (_stop)
            break;
        const UINT64 g = _scheduled;
        const UINT s = UINT(g % K);
        const UINT64 skipped = _consumed ? g - _consumed - 1 : 0;
        const UINT64 released = g - _consumed;
        _picked[s] = g;
        const UINT sync = _sync[s], flags = _flags[s];
        const auto tSched = _tSched[s];
        const SlotMeta meta = _meta[s];
        const UINT64 gameVal = _gameVal[s];
        lk.unlock();

        // Recovery test: <dumpDir>\mgpu_remove.trigger removes the display device
        if (g % 60 == 0 && !env.dumpDir.empty())
        {
            const std::wstring trig = env.dumpDir + L"\\mgpu_remove.trigger";
            if (GetFileAttributesW(trig.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                DeleteFileW(trig.c_str());
                ComPtr<ID3D12Device5> d5;
                Log(1, std::format("test: removing the display device at frame {}", g));
                if (SUCCEEDED(_ddev.As(&d5)))
                    d5->RemoveDevice();
            }
        }

        // The send lands before anything of this frame is queued: no display queue waits on the render card's fence
        // (a hitch held submitted work there for seconds; a Present behind it hung the display card)
        if (!WaitSend(g))
            continue;

        // Minimised or occluded (an exclusive fullscreen left by DXGI): nothing goes to the real back buffers until a
        // test present says the window shows again. A present on them then hung the display card (DEVICE_HUNG).
        if (_occluded || IsIconic(_hwnd))
        {
            const HRESULT t = IsIconic(_hwnd) ? DXGI_STATUS_OCCLUDED : _real->Present(0, DXGI_PRESENT_TEST);
            if (t == DXGI_STATUS_OCCLUDED)
            {
                if (!_occluded)
                    Log(0, std::format("window minimised or occluded at frame {}: frames dropped until it shows", g));
                _occluded = true;
                _dq->Signal(_releasedD.Get(), g);
                _dq->Signal(_dFence.Get(), ++_dValue);
                slotDisplayValue[s] = _dValue;
                slotFrame[s] = 0;
                slotCounted[s] = false;
                if (env.nrIdle)
                    env.nrIdle("window minimised or occluded");
                lk.lock();
                _consumed = g;
                lk.unlock();
                _cv.notify_all();
                if (_waitable)
                    ReleaseSemaphore(_waitable, (LONG) (std::min<UINT64>)(released, 8), nullptr);
                continue;
            }
            if (_occluded)
                Log(0, std::format("window shows again at frame {} (test present {:X}): presenting", g, (uint32_t) t));
            _occluded = false;
            _nrReset = true;
        }

        // This slot's previous display work (timestamps, the UI count, the extra list) must be done.
        // not done (stop or 60 s): the slot is not reused, the loop comes back to the same frame
        if (!WaitReuse(_dFence.Get(), slotDisplayValue[s], "display", &_stop))
            continue;
        toDisplayMs = toPresentMs = -1;
        if (slotFrame[s])
        {
            // the slot's last frame: render done -> ingested -> display queue past its Present
            const int64_t tg = _clock.Done(slotGame[s]), ti = _ingestClock.Done(slotFrame[s]),
                          td = _shownClock.Done(slotDisplayValue[s]);
            if (tg && ti && td && ti >= tg && td >= ti)
                toDisplayMs = double(ti - tg) * qpcMs, toPresentMs = double(td - ti) * qpcMs;
            slotFrame[s] = 0;
        }
        double ingestMs = -1;
        if (slotDisplayValue[s])
            ingestMs = ReadTs(_dReadback.Get(), s, _dTsFreq);
        double uiShare = -1;
        if (slotCounted[s] && _ui)
        {
            uiShare = double(_ui->Count(s)) / (double(_gameDesc.Width) * _gameDesc.Height);
            if (uiShare > CurrentSettings().uiGate && !_gateOn)
            {
                _gateOn = true;
                Log(1, std::format("HUD-less gate: {:.1f}% of pixels differ from the back buffer (a wrong pairing?): no NR",
                                   uiShare * 100));
                std::lock_guard sg(StatsMutex());
                MutableStats().gateFallbacks++;
            }
            else if (uiShare <= CurrentSettings().uiGate && _gateOn)
            {
                _gateOn = false;
                Log(0, std::format("HUD-less gate: {:.1f}% of pixels differ: HUD-less again", uiShare * 100));
            }
        }
        slotCounted[s] = false;

        double nrMs = 0, nrAnswerMs = -1;
        bool hudFrame = false;
        HRESULT hr;
        UINT pfUsed = 0;
        {
            // no _scMutex: in exclusive fullscreen Present can wait on the window thread, which may be in a proxy call
            const UINT m = _real->GetCurrentBackBufferIndex();
            ID3D12CommandList* l[2] = { _dList[s * _realCount + m].Get(), nullptr };
            UINT nl = 1;
            const bool haveHud = meta.hud && _hudD && _ui;
            bool haveMv = false;
            if ((haveHud || meta.mv) && _dxList[s])
            {
                if (meta.mv && (!_mvD || _mvD->GetDesc().Width != meta.mvDesc.Width ||
                                _mvD->GetDesc().Height != meta.mvDesc.Height || _mvD->GetDesc().Format != meta.mvDesc.Format))
                {
                    if (_mvD)
                        _graveD.push_back(_mvD);
                    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
                    D3D12_RESOURCE_DESC md = meta.mvDesc;
                    _mvD.Reset();
                    _ddev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &md, ns, nullptr, IID_PPV_ARGS(&_mvD));
                    if (_mvD)
                        _mvD->SetName(L"Mgpu motion D");
                }
                haveMv = meta.mv && _mvD;
                _dxAlloc[s]->Reset();
                auto* c = _dxList[s].Get();
                c->Reset(_dxAlloc[s].Get(), nullptr);
                auto ingest = [&](ID3D12Resource* dstTex, UINT64 offset)
                {
                    auto dd = dstTex->GetDesc();
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
                    _ddev->GetCopyableFootprints(&dd, 0, 1, offset, &fp, nullptr, nullptr, nullptr);
                    auto b = Tr(dstTex, ns, D3D12_RESOURCE_STATE_COPY_DEST);
                    c->ResourceBarrier(1, &b);
                    D3D12_TEXTURE_COPY_LOCATION dl { dstTex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    D3D12_TEXTURE_COPY_LOCATION sl { _slotD[s].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    sl.PlacedFootprint = fp;
                    c->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
                    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                    c->ResourceBarrier(1, &b);
                };
                if (haveHud)
                    ingest(_hudD.Get(), _hudOffset);
                if (haveMv)
                    ingest(_mvD.Get(), _mvOffset);
                c->Close();
                l[nl++] = c;
            }
            _dq->ExecuteCommandLists(nl, l);
            _dq->Signal(_releasedD.Get(), g);
            _ingestClock.Push(_releasedD.Get(), g);
            const bool probe = _probeFrames > 0;
            if (probe)
            {
                _probeFrames--;
                Alive("before ingest");
                _dq->Signal(_dFence.Get(), ++_dValue);
                WaitFence(_dFence.Get(), _dValue, 2000);
                Alive(meta.mv ? "ingest (colour, HUD-less, motion)" : "ingest (colour, HUD-less)");
            }
            const bool dump = g >= _dumpFrom && g < _dumpTo;
            if (dump)
            {
                DumpTexture(_ddev.Get(), _dq.Get(), _realBb[m].Get(), D3D12_RESOURCE_STATE_PRESENT,
                            env.dumpDir + std::format(L"\\f{:06}_ingest", g));
                if (haveHud)
                    DumpTexture(_ddev.Get(), _dq.Get(), _hudD.Get(), ns, env.dumpDir + std::format(L"\\f{:06}_hudless", g));
                if (haveMv)
                    DumpTexture(_ddev.Get(), _dq.Get(), _mvD.Get(), ns, env.dumpDir + std::format(L"\\f{:06}_mv", g));
            }

            // UI mask from the back buffer against the HUD-less colour: count always, composite unless gated.
            const bool useHud = haveHud && !_gateOn;
            auto prepUi = [&]
            {
                _duAlloc[s]->Reset();
                auto* c = _duList[s].Get();
                c->Reset(_duAlloc[s].Get(), nullptr);
                D3D12_RESOURCE_BARRIER b[2] = { Tr(_realBb[m].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE),
                                                Tr(_bbD.Get(), ns, D3D12_RESOURCE_STATE_COPY_DEST) };
                c->ResourceBarrier(2, b);
                c->CopyResource(_bbD.Get(), _realBb[m].Get());
                for (auto& x : b)
                    std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
                c->ResourceBarrier(2, b);
                return c;
            };

            // The window resized ahead of its ResizeBuffers (or minimised): NR waits for the swapchain to follow
            RECT rc {};
            GetClientRect(_hwnd, &rc);
            const bool mismatch = UINT(rc.right - rc.left) != _gameDesc.Width || UINT(rc.bottom - rc.top) != _gameDesc.Height;
            if (!mismatch)
                _mismatchSince = {};
            else if (!_mismatchSince.time_since_epoch().count())
                _mismatchSince = std::chrono::steady_clock::now();
            // a swapchain that stays smaller than its window (stretch scaling) gets NR back after 5 s
            const bool sizeHold = mismatch && std::chrono::steady_clock::now() - _mismatchSince < std::chrono::seconds(5);
            if (sizeHold != _sizeHold)
            {
                _sizeHold = sizeHold;
                Log(0, std::format("window {}x{}, swapchain {}x{}: NR {}", rc.right - rc.left, rc.bottom - rc.top,
                                   _gameDesc.Width, _gameDesc.Height, sizeHold ? "held" : "resumed"));
            }

            // HUD-less mode runs NR over the UI only as the fallback for a game with no HUD-less colour (a run of
            // misses); a lone miss or the gate presents without NR
            _hudMissStreak = haveHud ? 0 : _hudMissStreak + 1;
            const bool whole = _wantHud && !haveHud && CurrentSettings().wholeFrameFallback && _hudMissStreak > 30;
            if (whole != _wholeFallback)
            {
                _wholeFallback = whole;
                Log(0, whole ? "no HUD-less colour: NR on the whole frame, UI included"
                             : "HUD-less colour back: NR on it, the UI kept as drawn");
                std::lock_guard sg(StatsMutex());
                MutableStats().nrSource = whole ? "whole frame, UI included (no HUD-less colour)" : "HUD-less colour";
            }
            const bool nrOn = env.nrWanted && env.nrWanted() && env.nrFrame;
            const char* idle = sizeHold ? "NR held: window and swapchain sizes differ"
                               : !nrOn  ? "NR off"
                               : _wantHud && !useHud && !whole ? (_gateOn ? "HUD-less gate" : "no HUD-less colour this frame")
                                                               : nullptr;
            if (idle && env.nrIdle)
                env.nrIdle(idle);
            if (!idle)
            {
                NrInput in;
                const auto now = std::chrono::steady_clock::now();
                // a frame dump's stall is not a pause: no history reset (the dumped frames would show a reset frame)
                const bool dumpStall = g >= _dumpFrom && g <= _dumpTo;
                const bool gap = _lastNr.time_since_epoch().count() && !dumpStall &&
                                 std::chrono::duration<double, std::milli>(now - _lastNr).count() > 250.0;
                _lastNr = now;
                if (haveMv)
                {
                    in.motion = _mvD.Get();
                    in.motionState = ns;
                    in.guides = meta.g;
                }
                const bool mvResized = haveMv && (meta.g.motionW != _lastMvW || meta.g.motionH != _lastMvH);
                if (haveMv)
                {
                    _lastMvW = meta.g.motionW;
                    _lastMvH = meta.g.motionH;
                }
                in.reset = _nrReset.exchange(false) || gap || mvResized || (haveMv && meta.g.reset);
                in.waitAnswer = CurrentSettings().nrEveryFrame;
                if (useHud)
                {
                    in.src = _hudD.Get();
                    in.srcState = ns;
                    in.dst = _nrD.Get();
                    in.dstState = ns;
                }
                else
                {
                    in.src = in.dst = _realBb[m].Get();
                    in.srcState = in.dstState = D3D12_RESOURCE_STATE_PRESENT;
                }
                ID3D12GraphicsCommandList* c = nullptr;
                auto t0 = std::chrono::steady_clock::now();
                double answerMs = 0;
                in.answerWaitMs = &answerMs;
                const bool wrote = env.nrFrame(_ddev.Get(), _dq.Get(), in);
                // nr stays record + submit time; the CPU wait for the answer is reported on its own
                nrMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() - answerMs;
                nrAnswerMs = answerMs;
                if (useHud)
                {
                    c = prepUi();
                    _ui->Record(c, s, _bbD.Get(), _hudD.Get(), wrote ? _nrD.Get() : _hudD.Get(), _compD.Get(),
                                _gameDesc.Width, _gameDesc.Height);
                    D3D12_RESOURCE_BARRIER b[2] = { Tr(_compD.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE),
                                                    Tr(_realBb[m].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                                       D3D12_RESOURCE_STATE_COPY_DEST) };
                    c->ResourceBarrier(2, b);
                    c->CopyResource(_realBb[m].Get(), _compD.Get());
                    for (auto& x : b)
                        std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
                    c->ResourceBarrier(2, b);
                    c->Close();
                    ID3D12CommandList* cl[] = { c };
                    _dq->ExecuteCommandLists(1, cl);
                    slotCounted[s] = true;
                    hudFrame = true;
                    if (dump && wrote)
                        DumpTexture(_ddev.Get(), _dq.Get(), _nrD.Get(), ns, env.dumpDir + std::format(L"\\f{:06}_nr", g));
                }
            }
            else if (haveHud)
            {
                // NR off: the mask is still measured (and dumped) so the gate can be checked without NR
                auto* c = prepUi();
                _ui->Record(c, s, _bbD.Get(), _hudD.Get(), nullptr, nullptr, _gameDesc.Width, _gameDesc.Height);
                c->Close();
                ID3D12CommandList* cl[] = { c };
                _dq->ExecuteCommandLists(1, cl);
                slotCounted[s] = true;
            }
            if (dump)
                DumpTexture(_ddev.Get(), _dq.Get(), _realBb[m].Get(), D3D12_RESOURCE_STATE_PRESENT,
                            env.dumpDir + std::format(L"\\f{:06}_display", g));
            bool settled = !env.nrSettle || env.nrSettle();
            // Present only on an idle display queue (bounded): never a Present behind work that may not finish
            double idleMs = -1;
            if (settled && CurrentSettings().presentIdle)
            {
                _dq->Signal(_dFence.Get(), ++_dValue);
                if (!GpuWaitDone(_dFence.Get(), _dValue, kPresentIdleMs, nullptr, nullptr, &idleMs))
                {
                    settled = false;
                    _lastIdleMs = -2;
                    if (_idleSkips++ < 20)
                        Log(2, std::format("frame {}: display queue not idle after {:.0f} ms: Present skipped", g, idleMs));
                }
                else if (idleMs > 100 && _idleSlow++ < 20)
                    Log(1, std::format("frame {}: display queue idle after {:.0f} ms before the Present", g, idleMs));
            }
            if (settled)
                _lastIdleMs = idleMs;
            if (probe)
            {
                _dq->Signal(_dFence.Get(), ++_dValue);
                WaitFence(_dFence.Get(), _dValue, 2000);
                Alive("NR and UI composite");
            }
            BOOL fs = FALSE;
            _real->GetFullscreenState(&fs, nullptr);
            _fsCached = fs;
            UINT pf = flags & ~(UINT) DXGI_PRESENT_ALLOW_TEARING;
            if (sync == 0 && _tearing && !fs && !_tearingOff)
                pf |= DXGI_PRESENT_ALLOW_TEARING;
            pfUsed = pf;
            const auto tp = std::chrono::steady_clock::now();
            // an unfinished network answer: never queue the Present behind its wait
            hr = settled ? _real->Present(sync, pf) : S_OK;
            const double presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count();
            if (presentMs > 250 || hr != _lastPresentHr)
                Log(presentMs > 250 ? 1 : 0, std::format("real Present {:X} in {:.0f} ms (frame {})", (uint32_t) hr, presentMs, g));
            _lastPresentHr = hr;
            if (hr == DXGI_STATUS_OCCLUDED)
                _occluded = true;
            if (hr == DXGI_ERROR_INVALID_CALL && (pf & DXGI_PRESENT_ALLOW_TEARING))
            {
                // DXGI still in (or leaving) exclusive fullscreen: no tearing until the next mode change
                _tearingOff = true;
                Log(1, "Present with tearing refused during a fullscreen change: tearing off until the next one");
                hr = _real->Present(sync, pf & ~(UINT) DXGI_PRESENT_ALLOW_TEARING);
            }
            Latency::Mark(g, Latency::RealPresent);
            _dq->Signal(_dFence.Get(), ++_dValue);
            Latency::Watch(_dFence.Get(), _dValue, g, Latency::DisplayDone);
            for (UINT64 f = _consumed + 1; skipped && f < g; ++f)
                Latency::Skipped(f);
            slotDisplayValue[s] = _dValue;
            _shownClock.Push(_dFence.Get(), _dValue);
            slotFrame[s] = g;
            slotGame[s] = gameVal;
        }
        if (hr == DXGI_ERROR_INVALID_CALL && SUCCEEDED(_ddev->GetDeviceRemovedReason()) && _renewals < 8)
        {
            _renewals++;
            if (SUCCEEDED(RenewReal()))
                hr = S_OK, _presentFails = 0;
        }
        if (FAILED(hr) || (g % 30 == 0 && FAILED(_ddev->GetDeviceRemovedReason())))
        {
            if (_presentFails++ < 3)
            {
                DXGI_SWAP_CHAIN_DESC1 rd {};
                DXGI_SWAP_CHAIN_FULLSCREEN_DESC fd {};
                _real->GetDesc1(&rd);
                _real->GetFullscreenDesc(&fd);
                Log(2, std::format("real Present({}, {:X}) failed {:X}: fullscreen {}, {}x{} fmt {} flags {:X}, windowed {}, "
                                   "queue idle before it {} ({:.0f} ms)",
                                   sync, pfUsed, (uint32_t) hr, (int) _fsCached, rd.Width, rd.Height, (int) rd.Format,
                                   rd.Flags, fd.Windowed, _lastIdleMs >= 0 ? "yes" : _lastIdleMs < -1.5 ? "no" : "unchecked", _lastIdleMs));
            }
            LogDeviceRemoved(_ddev.Get(), "display");
            const HRESULT why = _ddev->GetDeviceRemovedReason();
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
                FAILED(why))
            {
                {
                    std::lock_guard sg(StatsMutex());
                    MutableStats().fallbackReason = std::format("display device removed ({:X})", (uint32_t) why);
                }
                _lostPending = true;
                lk.lock();
                _consumed = _taken = _scheduled;
                lk.unlock();
                _cv.notify_all();
                if (_waitable)
                    ReleaseSemaphore(_waitable, 1, nullptr);
                Log(2, "presenter stopped: the next frame presents from the render card");
                break;
            }
        }

        if (!_retired.empty() && _presentCount >= _retireAfter)
            ReleaseRetired();
        lk.lock();
        _consumed = g;
        lk.unlock();
        _cv.notify_all();
        if (_waitable)
            ReleaseSemaphore(_waitable, (LONG) (std::min<UINT64>)(released, 8), nullptr);

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard sg(StatsMutex());
        auto& st = MutableStats();
        st.presented++;
        st.skipped += skipped;
        if (_wantHud)
        {
            if (hudFrame)
                st.hudlessFrames++;
            else if (!meta.hud)
                st.hudlessMissed++;
            if (uiShare >= 0)
                st.uiShare = uiShare;
        }
        if (meta.mv)
            st.guideFrames++;
        if (_lastDisplay.time_since_epoch().count())
            st.displayFrameMs = Ema(st.displayFrameMs, std::chrono::duration<double, std::milli>(now - _lastDisplay).count());
        _lastDisplay = now;
        st.latencyMs = Ema(st.latencyMs, std::chrono::duration<double, std::milli>(now - tSched).count());
        if (toDisplayMs >= 0)
        {
            st.renderToDisplayMs = Ema(st.renderToDisplayMs, toDisplayMs);
            st.displayToPresentMs = Ema(st.displayToPresentMs, toPresentMs);
            st.addedMs = st.renderToDisplayMs + st.displayToPresentMs;
        }
        if (ingestMs > 0)
        {
            st.ingestMs = Ema(st.ingestMs, ingestMs);
            st.ingestGBs = double(_payloadBytes) / (st.ingestMs * 1e6);
        }
        if (nrMs > 0)
            st.nrMs = Ema(st.nrMs, nrMs);
        if (nrAnswerMs >= 0 && nrAnswerMs < 250)
            st.answerWaitMs = Ema(st.answerWaitMs, nrAnswerMs);
        st.ringDepth = UINT(_scheduled - g);
        if (st.presented % 600 == 0)
            Log(0, std::format("stats: render {:.2f} ms ({:.0f}% busy) display {:.2f} ms ({:.0f}% busy) send {:.2f} ms {:.1f} GB/s "
                               "ingest {:.2f} ms {:.1f} GB/s latency {:.2f} ms nr {:.2f} ms produced {} presented {} skipped {} "
                               "hudless {} missed {} ui {:.2f}% gate {} motion {} cpu wait send {:.2f} answer {:.2f} ms stalls {} "
                               "added {:.2f} ms (render done -> display card {:.2f}, -> past present {:.2f}) overlap 0",
                               st.renderFrameMs, st.renderBusy, st.displayFrameMs, st.displayBusy, st.sendMs, st.sendGBs,
                               st.ingestMs, st.ingestGBs, st.latencyMs, st.nrMs, st.produced, st.presented, st.skipped,
                               st.hudlessFrames, st.hudlessMissed, st.uiShare * 100, st.gateFallbacks, st.guideFrames,
                               st.sendWaitMs, st.answerWaitMs, st.sendStalls, st.addedMs, st.renderToDisplayMs,
                               st.displayToPresentMs));
    }
}

HRESULT ProxySwapChain::CreateOverlapBuffers()
{
    HRESULT hr;
    const auto& d = _gameDesc;
    auto lists = [&](D3D12_COMMAND_LIST_TYPE t, ComPtr<ID3D12CommandAllocator>& a, ComPtr<ID3D12GraphicsCommandList>& l)
    {
        HRESULT r;
        if (FAILED(r = _ddev->CreateCommandAllocator(t, IID_PPV_ARGS(&a))) ||
            FAILED(r = _ddev->CreateCommandList(0, t, a.Get(), nullptr, IID_PPV_ARGS(&l))))
            return r;
        return l->Close();
    };
    for (UINT s = 0; s < K; ++s)
    {
        _colO[s] = Tex(_ddev.Get(), d.Width, d.Height, d.Format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        if (!_colO[s])
            return E_OUTOFMEMORY;
        _colO[s]->SetName(std::format(L"Mgpu overlap colour {}", s).c_str());
        if (_wantHud)
        {
            _hudO[s] = Tex(_ddev.Get(), d.Width, d.Height, d.Format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
            _compO[s] = Tex(_ddev.Get(), d.Width, d.Height, d.Format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COMMON);
            if (!_hudO[s] || !_compO[s])
                return E_OUTOFMEMORY;
            _hudO[s]->SetName(std::format(L"Mgpu overlap hudless {}", s).c_str());
            _compO[s]->SetName(std::format(L"Mgpu overlap composite {}", s).c_str());
        }
        if (FAILED(hr = lists(D3D12_COMMAND_LIST_TYPE_COPY, _iAlloc[s], _iList[s])) ||
            FAILED(hr = lists(D3D12_COMMAND_LIST_TYPE_DIRECT, _wpAlloc[s], _wpList[s])) ||
            FAILED(hr = lists(D3D12_COMMAND_LIST_TYPE_DIRECT, _wAlloc[s], _wList[s])) ||
            FAILED(hr = lists(D3D12_COMMAND_LIST_TYPE_DIRECT, _pAlloc[s], _pList[s])))
            return hr;
    }
    if (_iTimestamps)
    {
        D3D12_QUERY_HEAP_DESC qd { D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP, 2 * K };
        D3D12_RESOURCE_DESC rb {};
        rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb.Width = 16 * K;
        rb.Height = rb.DepthOrArraySize = rb.MipLevels = 1;
        rb.SampleDesc.Count = 1;
        rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rbp { D3D12_HEAP_TYPE_READBACK };
        if (FAILED(_ddev->CreateQueryHeap(&qd, IID_PPV_ARGS(&_iQuery))) ||
            FAILED(_ddev->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&_iReadback))))
            _iQuery.Reset();
    }
    std::lock_guard g(_pm);
    for (UINT s = 0; s < K; ++s)
        _slotFrameO[s] = _slotDispO[s] = 0;
    return S_OK;
}

// <dumpDir>\mgpu_overlap.trigger (0 serial, 1 overlap), switched live: the presenter restarts
void ProxySwapChain::ReadOverlapTrigger(UINT64 next)
{
    const auto& env = CurrentEnv();
    if (env.dumpDir.empty() || next % 30 != 0)
        return;
    const std::wstring trig = env.dumpDir + L"\\mgpu_overlap.trigger";
    FILE* fp = _wfopen(trig.c_str(), L"r");
    if (!fp)
        return;
    int v = -1;
    const bool ok = fscanf(fp, "%d", &v) == 1 && v >= 0 && v <= 1;
    fclose(fp);
    DeleteFileW(trig.c_str());
    if (!ok)
        return;
    {
        // the phase that ends: displayed frames per second since the last switch
        Stats st;
        {
            std::lock_guard g(StatsMutex());
            st = MutableStats();
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - _phaseT0).count();
        Log(0, std::format("overlap phase {} ended: {} frames in {:.1f} s = {:.1f} fps, render {:.2f} ms, display {:.2f} ms "
                           "({:.0f}% busy), latency {:.2f} ms, added {:.2f} ms ({:.2f} + {:.2f}), nr {:.2f} ms, cpu wait "
                           "send {:.2f} answer {:.2f} ms",
                           _overlap.load(), st.presented - _phaseN0, secs, secs > 0 ? (st.presented - _phaseN0) / secs : 0.0,
                           st.renderFrameMs, st.displayFrameMs, st.displayBusy, st.latencyMs, st.addedMs,
                           st.renderToDisplayMs, st.displayToPresentMs, st.nrMs, st.sendWaitMs, st.answerWaitMs));
        _phaseN0 = st.presented;
        _phaseT0 = std::chrono::steady_clock::now();
    }
    const int pipe = v > 0 ? 1 : 0;
    Log(0, std::format("overlap {} from the trigger at frame {}{}", v, next, pipe != _pipeline ? ": presenter restarts" : ""));
    if (pipe != _pipeline)
    {
        StopPresenter();
        if (v > 0 && !_colO[0] && FAILED(CreateOverlapBuffers()))
        {
            Log(2, "overlap buffers failed: serial presenter");
            v = 0;
        }
        _overlap = v;
        _nrReset = true;
        StartPresenter();
    }
}

static bool OverlapStopped(void* x) { return static_cast<std::atomic<bool>*>(x)->load(); }

// Overlap, stage 1: the ring slot into this slot's textures on the copy queue _iq, once the CPU has seen the send land.
// At most kDepth frames are taken and not yet presented. No queue of the display card waits on a fence: every hand-over
// between the stages is a CPU wait (the send, the ingest, the network answer, the work before the Present).
void ProxySwapChain::IngestLoop()
{
    SetThreadDescription(GetCurrentThread(), L"Mgpu ingest");
    const auto& env = CurrentEnv();
    const auto cm = D3D12_RESOURCE_STATE_COMMON;
    for (;;)
    {
        {
            std::unique_lock lk(_m);
            _cv.wait(lk, [&] { return _stop || _stopO.load() || _scheduled > _taken; });
            if (_stop || _stopO)
                break;
        }
        {
            std::unique_lock pl(_pm);
            _pcv.wait(pl, [&] { return _stopO.load() || _inFlight < kDepth; });
            if (_stopO)
                break;
        }
        Pending p;
        {
            std::lock_guard lk(_m);
            p.g = _scheduled;
            p.s = UINT(p.g % K);
            _picked[p.s] = p.g;
            p.sync = _sync[p.s];
            p.flags = _flags[p.s];
            p.tSched = _tSched[p.s];
            p.gameVal = _gameVal[p.s];
            p.meta = _meta[p.s];
        }
        const UINT64 g = p.g;
        const UINT s = p.s;

        if (g % 60 == 0 && !env.dumpDir.empty())
        {
            const std::wstring trig = env.dumpDir + L"\\mgpu_remove.trigger";
            if (GetFileAttributesW(trig.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                DeleteFileW(trig.c_str());
                ComPtr<ID3D12Device5> d5;
                Log(1, std::format("test: removing the display device at frame {}", g));
                if (SUCCEEDED(_ddev.As(&d5)))
                    d5->RemoveDevice();
            }
        }

        // The send lands before anything of this frame is queued (see PresenterLoop); not done: back to the newest frame
        if (!WaitSend(g))
            continue;
        UINT64 dispV;
        {
            // the slot's last frame must have been presented (a skip can bring it within kDepth)
            std::unique_lock pl(_pm);
            _pcv.wait(pl, [&] { return _stopO.load() || _presentedG >= _slotFrameO[s]; });
            if (_stopO)
                break;
            dispV = _slotDispO[s];
        }
        // and its present copy done: this slot's textures, lists and timestamps are free
        if (!WaitReuse(_dFence.Get(), dispV, "overlap display", &_stop))
            continue;
        {
            std::lock_guard lk(_m);
            p.skipped = _taken && g > _taken ? g - _taken - 1 : 0;
            _taken = g;
        }
        if (dispV && _iQuery)
            p.ingestMs = ReadTs(_iReadback.Get(), s, _iTsFreq);

        p.dropped = _occluded || IsIconic(_hwnd);
        p.haveHud = !p.dropped && p.meta.hud && _hudO[s] && _ui;
        bool mvOk = !p.dropped && p.meta.mv;
        if (mvOk && (!_mvO[s] || _mvO[s]->GetDesc().Width != p.meta.mvDesc.Width ||
                     _mvO[s]->GetDesc().Height != p.meta.mvDesc.Height || _mvO[s]->GetDesc().Format != p.meta.mvDesc.Format))
        {
            if (_mvO[s])
                _graveD.push_back(_mvO[s]);
            D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC md = p.meta.mvDesc;
            _mvO[s].Reset();
            _ddev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &md, cm, nullptr, IID_PPV_ARGS(&_mvO[s]));
            if (_mvO[s])
                _mvO[s]->SetName(std::format(L"Mgpu overlap motion {}", s).c_str());
        }
        p.mvOk = mvOk && _mvO[s];

        // copy queue: COMMON promotes and decays, no barriers
        _iAlloc[s]->Reset();
        auto* c = _iList[s].Get();
        c->Reset(_iAlloc[s].Get(), nullptr);
        auto ingest = [&](ID3D12Resource* dstTex, UINT64 offset)
        {
            auto dd = dstTex->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
            _ddev->GetCopyableFootprints(&dd, 0, 1, offset, &fp, nullptr, nullptr, nullptr);
            D3D12_TEXTURE_COPY_LOCATION dl { dstTex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION sl { _slotD[s].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            sl.PlacedFootprint = fp;
            c->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        };
        if (!p.dropped)
        {
            if (_iQuery)
                c->EndQuery(_iQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2);
            ingest(_colO[s].Get(), 0);
            if (_iQuery)
            {
                c->EndQuery(_iQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2 + 1);
                c->ResolveQueryData(_iQuery.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * 2, 2, _iReadback.Get(), s * 16);
            }
            if (p.haveHud)
                ingest(_hudO[s].Get(), _hudOffset);
            if (p.mvOk)
                ingest(_mvO[s].Get(), _mvOffset);
        }
        c->Close();
        ID3D12CommandList* l[] = { c };
        _iq->ExecuteCommandLists(1, l);
        _iq->Signal(_releasedD.Get(), g); // the ring slot is free once ingested
        _iq->Signal(_iFence.Get(), g);
        _ingestClock.Push(_releasedD.Get(), g);
        {
            std::lock_guard pg(_pm);
            _slotFrameO[s] = g;
            _inFlight++;
            _wk.push_back(p);
        }
        _pcv.notify_all();
    }
}

// Overlap, stage 2: NR and the UI composite into this slot's output on the work queue _wq. The network answer is
// awaited on the CPU inside nrFrame, while stage 1 ingests the next frame and stage 3 presents the previous one.
void ProxySwapChain::WorkLoop()
{
    SetThreadDescription(GetCurrentThread(), L"Mgpu work");
    const auto& env = CurrentEnv();
    const auto ns = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, cm = D3D12_RESOURCE_STATE_COMMON;
    bool slotCounted[K] {};
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const double qpcMs = 1000.0 / double(qf.QuadPart);
    int64_t tFree = 0, tWork = 0;      // QPC: the stage became free, started on a frame
    double workEma = 0, jitLead = 3.0; // ms: the stage's time per frame, how long before its end the next send starts
    for (;;)
    {
        Pending p;
        {
            std::unique_lock pl(_pm);
            _pcv.wait(pl, [&] { return _stopO.load() || !_wk.empty(); });
            if (_stopO)
                break;
            p = _wk.front();
            _wk.erase(_wk.begin());
        }
        const UINT64 g = p.g;
        const UINT s = p.s;
        const SlotMeta& meta = p.meta;

        // the ingest is done before anything that reads it is queued (ends only on stop or device removal)
        double ingestWait = 0;
        while (!GpuWaitDone(_iFence.Get(), g, 60000, OverlapStopped, &_stopO, &ingestWait) && !_stopO)
            Log(2, std::format("frame {}: ingest not done after {:.0f} ms, still waiting", g, ingestWait));
        if (_stopO)
            break;
        {
            // Just in time: the game may send the next frame so that it is ingested as this stage frees up.
            // ready - tFree > 0: this frame's ingest ended after the stage was free (sent too late), < 0: it waited.
            const int64_t ready = _ingestClock.Done(g);
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            if (ready && tFree)
                jitLead = std::clamp(jitLead + 0.2 * (double(ready - tFree) * qpcMs + 0.3), 0.5, 15.0);
            tWork = q.QuadPart;
            std::lock_guard lk(_m);
            _workFrame = g;
            _grantQpc = tWork + int64_t((std::max)(0.0, workEma - jitLead) / qpcMs);
        }
        _cv.notify_all();

        // the slot's last frame was presented: its UI count is final
        if (slotCounted[s] && _ui)
        {
            p.uiShare = double(_ui->Count(s)) / (double(_gameDesc.Width) * _gameDesc.Height);
            if (p.uiShare > CurrentSettings().uiGate && !_gateOn)
            {
                _gateOn = true;
                Log(1, std::format("HUD-less gate: {:.1f}% of pixels differ from the back buffer (a wrong pairing?): no NR",
                                   p.uiShare * 100));
                std::lock_guard sg(StatsMutex());
                MutableStats().gateFallbacks++;
            }
            else if (p.uiShare <= CurrentSettings().uiGate && _gateOn)
            {
                _gateOn = false;
                Log(0, std::format("HUD-less gate: {:.1f}% of pixels differ: HUD-less again", p.uiShare * 100));
            }
        }
        slotCounted[s] = false;

        ID3D12Resource* out = _colO[s].Get();
        p.mv = meta.mv;
        p.hudMissed = !meta.hud;
        const bool probe = _probeFrames > 0;
        const bool haveHud = p.haveHud, mvOk = p.mvOk;
        if (p.dropped)
        {
            if (env.nrIdle)
                env.nrIdle("window minimised or occluded");
            out = nullptr;
        }
        else
        {
            const bool useHud = haveHud && !_gateOn;
            RECT rc {};
            GetClientRect(_hwnd, &rc);
            const bool mismatch = UINT(rc.right - rc.left) != _gameDesc.Width || UINT(rc.bottom - rc.top) != _gameDesc.Height;
            if (!mismatch)
                _mismatchSince = {};
            else if (!_mismatchSince.time_since_epoch().count())
                _mismatchSince = std::chrono::steady_clock::now();
            const bool sizeHold = mismatch && std::chrono::steady_clock::now() - _mismatchSince < std::chrono::seconds(5);
            if (sizeHold != _sizeHold)
            {
                _sizeHold = sizeHold;
                Log(0, std::format("window {}x{}, swapchain {}x{}: NR {}", rc.right - rc.left, rc.bottom - rc.top,
                                   _gameDesc.Width, _gameDesc.Height, sizeHold ? "held" : "resumed"));
            }
            _hudMissStreak = haveHud ? 0 : _hudMissStreak + 1;
            const bool whole = _wantHud && !haveHud && CurrentSettings().wholeFrameFallback && _hudMissStreak > 30;
            if (whole != _wholeFallback)
            {
                _wholeFallback = whole;
                Log(0, whole ? "no HUD-less colour: NR on the whole frame, UI included"
                             : "HUD-less colour back: NR on it, the UI kept as drawn");
                std::lock_guard sg(StatsMutex());
                MutableStats().nrSource = whole ? "whole frame, UI included (no HUD-less colour)" : "HUD-less colour";
            }
            const bool nrOn = env.nrWanted && env.nrWanted() && env.nrFrame;
            const char* idle = sizeHold ? "NR held: window and swapchain sizes differ"
                               : !nrOn  ? "NR off"
                               : _wantHud && !useHud && !whole ? (_gateOn ? "HUD-less gate" : "no HUD-less colour this frame")
                                                               : nullptr;
            if (idle && env.nrIdle)
                env.nrIdle(idle);

            // inputs to their read states; the colour stays COMMON when NR runs on the whole frame
            D3D12_RESOURCE_BARRIER pre[4];
            UINT np = 0;
            if (haveHud)
            {
                pre[np++] = Tr(_colO[s].Get(), cm, ns);
                pre[np++] = Tr(_hudO[s].Get(), cm, ns);
                pre[np++] = Tr(_compO[s].Get(), cm, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (mvOk)
                pre[np++] = Tr(_mvO[s].Get(), cm, ns);
            if (np)
            {
                _wpAlloc[s]->Reset();
                auto* c = _wpList[s].Get();
                c->Reset(_wpAlloc[s].Get(), nullptr);
                c->ResourceBarrier(np, pre);
                c->Close();
                ID3D12CommandList* l[] = { c };
                _wq->ExecuteCommandLists(1, l);
            }

            bool wrote = false;
            if (!idle)
            {
                NrInput in;
                const auto now = std::chrono::steady_clock::now();
                // a frame dump's stall is not a pause: no history reset
                const bool dumpStall = g >= _dumpFrom && g <= _dumpTo;
                const bool gap = _lastNr.time_since_epoch().count() && !dumpStall &&
                                 std::chrono::duration<double, std::milli>(now - _lastNr).count() > 250.0;
                _lastNr = now;
                if (mvOk)
                {
                    in.motion = _mvO[s].Get();
                    in.motionState = ns;
                    in.guides = meta.g;
                }
                const bool mvResized = mvOk && (meta.g.motionW != _lastMvW || meta.g.motionH != _lastMvH);
                if (mvOk)
                {
                    _lastMvW = meta.g.motionW;
                    _lastMvH = meta.g.motionH;
                }
                in.reset = _nrReset.exchange(false) || gap || mvResized || (mvOk && meta.g.reset);
                in.waitAnswer = CurrentSettings().nrEveryFrame;
                if (useHud)
                {
                    in.src = _hudO[s].Get();
                    in.srcState = ns;
                    in.dst = _nrD.Get();
                    in.dstState = ns;
                }
                else
                {
                    in.src = in.dst = _colO[s].Get();
                    in.srcState = in.dstState = haveHud ? ns : cm;
                }
                double answerMs = 0;
                in.answerWaitMs = &answerMs;
                const auto t0 = std::chrono::steady_clock::now();
                wrote = env.nrFrame(_ddev.Get(), _wq.Get(), in);
                p.nrMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() - answerMs;
                p.answerMs = answerMs;
                // an answer that never came: its lists were dropped, the frame is not shown
                if (env.nrSettle && !env.nrSettle())
                    p.show = false;
            }

            // UI composite (or only the count), then everything back to COMMON for the next queue
            _wAlloc[s]->Reset();
            auto* c = _wList[s].Get();
            c->Reset(_wAlloc[s].Get(), nullptr);
            if (haveHud)
            {
                const bool comp = useHud && !idle;
                _ui->Record(c, s, _colO[s].Get(), _hudO[s].Get(), comp ? (wrote ? _nrD.Get() : _hudO[s].Get()) : nullptr,
                            comp ? _compO[s].Get() : nullptr, _gameDesc.Width, _gameDesc.Height);
                slotCounted[s] = true;
                if (comp)
                    out = _compO[s].Get(), p.hudFrame = true;
            }
            for (UINT i = 0; i < np; ++i)
                std::swap(pre[i].Transition.StateBefore, pre[i].Transition.StateAfter);
            if (np)
                c->ResourceBarrier(np, pre);
            c->Close();
            ID3D12CommandList* l[] = { c };
            _wq->ExecuteCommandLists(1, l);
        }
        _wq->Signal(_wFence.Get(), g);
        p.out = out;
        if (out && g >= _dumpFrom && g < _dumpTo)
        {
            DumpTexture(_ddev.Get(), _wq.Get(), _colO[s].Get(), cm, env.dumpDir + std::format(L"\\f{:06}_ingest", g));
            if (haveHud)
                DumpTexture(_ddev.Get(), _wq.Get(), _hudO[s].Get(), cm, env.dumpDir + std::format(L"\\f{:06}_hudless", g));
            if (mvOk)
                DumpTexture(_ddev.Get(), _wq.Get(), _mvO[s].Get(), cm, env.dumpDir + std::format(L"\\f{:06}_mv", g));
            if (p.hudFrame)
                DumpTexture(_ddev.Get(), _wq.Get(), _nrD.Get(), ns, env.dumpDir + std::format(L"\\f{:06}_nr", g));
            DumpTexture(_ddev.Get(), _wq.Get(), out, cm, env.dumpDir + std::format(L"\\f{:06}_display", g));
        }
        if (probe)
        {
            _probeFrames--;
            GpuWaitDone(_wFence.Get(), g, 2000);
            Alive(mvOk ? "overlap ingest, NR and UI (colour, HUD-less, motion)" : "overlap ingest, NR and UI");
        }
        {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            tFree = q.QuadPart;
            const double ms = double(tFree - tWork) * qpcMs;
            if (ms < 250)
                workEma = workEma <= 0 ? ms : workEma * 0.9 + ms * 0.1;
            p.jitLead = jitLead;
        }
        {
            std::lock_guard pg(_pm);
            _pq.push_back(p);
        }
        _pcv.notify_all();
    }
}

// Overlap, stage 3: presents in order, each once the CPU has seen its work done. _dq only ever gets the copy into the
// back buffer and the Present: nothing is queued on it behind a wait.
void ProxySwapChain::PresentLoop()
{
    SetThreadDescription(GetCurrentThread(), L"Mgpu present");
    UINT64 slotFrame[K] {}, slotGame[K] {}, slotDisp[K] {};
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const double qpcMs = 1000.0 / double(qf.QuadPart);
    for (;;)
    {
        Pending p;
        {
            std::unique_lock pl(_pm);
            _pcv.wait(pl, [&] { return _stopO.load() || !_pq.empty(); });
            if (_stopO)
                break;
            p = _pq.front();
            _pq.erase(_pq.begin());
        }
        const UINT s = p.s;
        double workWait = 0;
        while (!GpuWaitDone(_wFence.Get(), p.g, 60000, OverlapStopped, &_stopO, &workWait) && !_stopO)
            Log(2, std::format("frame {}: display work not done after {:.0f} ms, still waiting", p.g, workWait));
        if (_stopO)
            break;
        const bool removed = FAILED(_ddev->GetDeviceRemovedReason());

        double toDisplayMs = -1, toPresentMs = -1;
        if (slotFrame[s])
        {
            const int64_t tg = _clock.Done(slotGame[s]), ti = _ingestClock.Done(slotFrame[s]),
                          td = _shownClock.Done(slotDisp[s]);
            if (tg && ti && td && ti >= tg && td >= ti)
                toDisplayMs = double(ti - tg) * qpcMs, toPresentMs = double(td - ti) * qpcMs;
            slotFrame[s] = 0;
        }

        HRESULT hr = removed ? _ddev->GetDeviceRemovedReason() : S_OK;
        UINT pfUsed = 0;
        bool presented = false;
        ID3D12Resource* out = removed || !p.show ? nullptr : p.out;
        if (out && (_occluded || IsIconic(_hwnd)))
            out = nullptr;
        if (removed)
        {
        }
        else if (!out)
        {
            if (p.show)
            {
                // minimised or occluded: nothing on the real back buffers until a test present says the window shows
                const HRESULT t = IsIconic(_hwnd) ? DXGI_STATUS_OCCLUDED : _real->Present(0, DXGI_PRESENT_TEST);
                if (t == DXGI_STATUS_OCCLUDED)
                {
                    if (!_occluded)
                        Log(0, std::format("window minimised or occluded at frame {}: frames dropped until it shows", p.g));
                    _occluded = true;
                }
                else
                {
                    if (_occluded)
                        Log(0, std::format("window shows again at frame {} (test present {:X}): presenting", p.g, (uint32_t) t));
                    _occluded = false;
                    _nrReset = true;
                }
            }
            _dq->Signal(_dFence.Get(), ++_dValue);
        }
        else
        {
            const UINT m = _real->GetCurrentBackBufferIndex();
            _pAlloc[s]->Reset();
            auto* c = _pList[s].Get();
            c->Reset(_pAlloc[s].Get(), nullptr);
            D3D12_RESOURCE_BARRIER b[2] = { Tr(_realBb[m].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
                                            Tr(out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE) };
            c->ResourceBarrier(2, b);
            c->CopyResource(_realBb[m].Get(), out);
            for (auto& x : b)
                std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
            c->ResourceBarrier(2, b);
            c->Close();
            ID3D12CommandList* l[] = { c };
            _dq->ExecuteCommandLists(1, l);
            bool idle = true;
            double idleMs = -1;
            if (CurrentSettings().presentIdle)
            {
                _dq->Signal(_dFence.Get(), ++_dValue);
                idle = GpuWaitDone(_dFence.Get(), _dValue, kPresentIdleMs, nullptr, nullptr, &idleMs);
                if (!idle && _idleSkips++ < 20)
                    Log(2, std::format("frame {}: display queue not idle after {:.0f} ms: Present skipped", p.g, idleMs));
            }
            _lastIdleMs = idle ? idleMs : -2;
            BOOL fs = FALSE;
            _real->GetFullscreenState(&fs, nullptr);
            _fsCached = fs;
            UINT pf = p.flags & ~(UINT) DXGI_PRESENT_ALLOW_TEARING;
            if (p.sync == 0 && _tearing && !fs && !_tearingOff)
                pf |= DXGI_PRESENT_ALLOW_TEARING;
            pfUsed = pf;
            const auto tp = std::chrono::steady_clock::now();
            hr = idle ? _real->Present(p.sync, pf) : S_OK;
            const double presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count();
            if (presentMs > 250 || hr != _lastPresentHr)
                Log(presentMs > 250 ? 1 : 0, std::format("real Present {:X} in {:.0f} ms (frame {})", (uint32_t) hr, presentMs, p.g));
            _lastPresentHr = hr;
            if (hr == DXGI_STATUS_OCCLUDED)
                _occluded = true;
            if (hr == DXGI_ERROR_INVALID_CALL && (pf & DXGI_PRESENT_ALLOW_TEARING))
            {
                _tearingOff = true;
                Log(1, "Present with tearing refused during a fullscreen change: tearing off until the next one");
                hr = _real->Present(p.sync, pf & ~(UINT) DXGI_PRESENT_ALLOW_TEARING);
            }
            Latency::Mark(p.g, Latency::RealPresent);
            _dq->Signal(_dFence.Get(), ++_dValue);
            Latency::Watch(_dFence.Get(), _dValue, p.g, Latency::DisplayDone);
            _shownClock.Push(_dFence.Get(), _dValue);
            slotFrame[s] = p.g;
            slotGame[s] = p.gameVal;
            presented = true;
        }
        slotDisp[s] = _dValue;
        {
            std::lock_guard pg(_pm);
            _slotDispO[s] = _dValue;
            _presentedG = p.g;
            if (_inFlight)
                _inFlight--;
        }
        _pcv.notify_all();
        // the game is released after the Present, as in the serial presenter
        UINT64 released = 0;
        {
            std::lock_guard g(_m);
            for (UINT64 f = _consumed + 1; p.skipped && f < p.g; ++f)
                Latency::Skipped(f);
            if (p.g > _consumed)
                released = p.g - _consumed, _consumed = p.g;
        }
        _cv.notify_all();
        if (_waitable && released)
            ReleaseSemaphore(_waitable, (LONG) (std::min<UINT64>)(released, 8), nullptr);

        if (hr == DXGI_ERROR_INVALID_CALL && SUCCEEDED(_ddev->GetDeviceRemovedReason()) && _renewals < 8)
        {
            _renewals++;
            if (SUCCEEDED(RenewReal()))
                hr = S_OK, _presentFails = 0;
        }
        if (FAILED(hr) || (p.g % 30 == 0 && FAILED(_ddev->GetDeviceRemovedReason())))
        {
            if (_presentFails++ < 3)
            {
                DXGI_SWAP_CHAIN_DESC1 rd {};
                DXGI_SWAP_CHAIN_FULLSCREEN_DESC fd {};
                _real->GetDesc1(&rd);
                _real->GetFullscreenDesc(&fd);
                Log(2, std::format("real Present({}, {:X}) failed {:X}: fullscreen {}, {}x{} fmt {} flags {:X}, windowed {}, "
                                   "overlap {}, queue idle before it {} ({:.0f} ms)",
                                   p.sync, pfUsed, (uint32_t) hr, (int) _fsCached, rd.Width, rd.Height, (int) rd.Format,
                                   rd.Flags, fd.Windowed, _overlap.load(),
                                   _lastIdleMs >= 0 ? "yes" : _lastIdleMs < -1.5 ? "no" : "unchecked", _lastIdleMs));
            }
            LogDeviceRemoved(_ddev.Get(), "display");
            const HRESULT why = _ddev->GetDeviceRemovedReason();
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
                FAILED(why))
            {
                {
                    std::lock_guard sg(StatsMutex());
                    MutableStats().fallbackReason = std::format("display device removed ({:X})", (uint32_t) why);
                }
                _lostPending = true;
                {
                    std::lock_guard g(_m);
                    _consumed = _taken = _scheduled;
                }
                {
                    // the other stages stop too: nothing more goes to the removed device
                    std::lock_guard pg(_pm);
                    _stopO = true;
                }
                _cv.notify_all();
                _pcv.notify_all();
                if (_waitable)
                    ReleaseSemaphore(_waitable, 1, nullptr);
                Log(2, "presenter stopped: the next frame presents from the render card");
                break;
            }
        }
        if (!_retired.empty() && _presentCount >= _retireAfter)
            ReleaseRetired();

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard sg(StatsMutex());
        auto& st = MutableStats();
        st.presented++;
        st.skipped += p.skipped;
        if (_wantHud)
        {
            if (p.hudFrame)
                st.hudlessFrames++;
            else if (p.hudMissed)
                st.hudlessMissed++;
            if (p.uiShare >= 0)
                st.uiShare = p.uiShare;
        }
        if (p.mv)
            st.guideFrames++;
        if (presented)
        {
            if (_lastDisplay.time_since_epoch().count())
                st.displayFrameMs = Ema(st.displayFrameMs, std::chrono::duration<double, std::milli>(now - _lastDisplay).count());
            _lastDisplay = now;
            st.latencyMs = Ema(st.latencyMs, std::chrono::duration<double, std::milli>(now - p.tSched).count());
        }
        if (toDisplayMs >= 0)
        {
            st.renderToDisplayMs = Ema(st.renderToDisplayMs, toDisplayMs);
            st.displayToPresentMs = Ema(st.displayToPresentMs, toPresentMs);
            st.addedMs = st.renderToDisplayMs + st.displayToPresentMs;
        }
        if (p.ingestMs > 0)
        {
            st.ingestMs = Ema(st.ingestMs, p.ingestMs);
            st.ingestGBs = double(_payloadBytes) / (st.ingestMs * 1e6);
        }
        if (p.nrMs > 0)
            st.nrMs = Ema(st.nrMs, p.nrMs);
        if (p.answerMs >= 0 && p.answerMs < 250)
            st.answerWaitMs = Ema(st.answerWaitMs, p.answerMs);
        st.jitLeadMs = p.jitLead;
        st.ringDepth = UINT(_scheduled - p.g);
        if (st.presented % 600 == 0)
            Log(0, std::format("stats: render {:.2f} ms ({:.0f}% busy) display {:.2f} ms ({:.0f}% busy) send {:.2f} ms {:.1f} GB/s "
                               "ingest {:.2f} ms {:.1f} GB/s latency {:.2f} ms nr {:.2f} ms produced {} presented {} skipped {} "
                               "hudless {} missed {} ui {:.2f}% gate {} motion {} cpu wait send {:.2f} answer {:.2f} ms stalls {} "
                               "added {:.2f} ms (render done -> display card {:.2f}, -> past present {:.2f}) overlap {} "
                               "send lead {:.2f} ms lead wait {:.2f} ms",
                               st.renderFrameMs, st.renderBusy, st.displayFrameMs, st.displayBusy, st.sendMs, st.sendGBs,
                               st.ingestMs, st.ingestGBs, st.latencyMs, st.nrMs, st.produced, st.presented, st.skipped,
                               st.hudlessFrames, st.hudlessMissed, st.uiShare * 100, st.gateFallbacks, st.guideFrames,
                               st.sendWaitMs, st.answerWaitMs, st.sendStalls, st.addedMs, st.renderToDisplayMs,
                               st.displayToPresentMs, _overlap.load(), st.jitLeadMs, _leadWaitMs.load()));
    }
}

HRESULT ProxySwapChain::DoPresent(UINT sync, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST)
        return _occluded && !_lost ? DXGI_STATUS_OCCLUDED : S_OK;
    LARGE_INTEGER tEntry;
    QueryPerformanceCounter(&tEntry);
    const auto& env = CurrentEnv();
    if (env.onPresent)
        env.onPresent();
    if (_broken)
        return DXGI_ERROR_DEVICE_REMOVED;
    if (_lostPending && !_lost)
    {
        HRESULT hr = EnterFallback();
        if (FAILED(hr))
            return hr;
    }
    if (!_lost)
        ReadOverlapTrigger(_scheduled + 1);
    const UINT i = _bbIndex;
    const UINT64 next = _scheduled + 1;
    if (!_lost)
        Latency::Mark(next, Latency::Present);
    if (!env.dumpDir.empty() && next % 30 == 0)
    {
        const std::wstring trig = env.dumpDir + L"\\mgpu_dump.trigger";
        if (GetFileAttributesW(trig.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            UINT64 n = 1;
            if (FILE* fp = _wfopen(trig.c_str(), L"r"))
            {
                unsigned long long v = 0;
                if (fscanf(fp, "%llu", &v) == 1 && v > 0 && v < 100)
                    n = v;
                fclose(fp);
            }
            DeleteFileW(trig.c_str());
            _dumpFrom = next + 1;
            _dumpTo = next + 1 + n;
            Log(0, std::format("dumping frames {}..{}", next + 1, next + n));
        }
        // Robustness test: minimise without activation for N s (default 4), then restore
        const std::wstring mt = env.dumpDir + L"\\mgpu_minimize.trigger";
        if (GetFileAttributesW(mt.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            int secs = 4;
            if (FILE* fp = _wfopen(mt.c_str(), L"r"))
            {
                if (fscanf(fp, "%d", &secs) != 1 || secs < 1 || secs > 60)
                    secs = 4;
                fclose(fp);
            }
            DeleteFileW(mt.c_str());
            Log(1, std::format("test: minimising the window for {} s at frame {}", secs, next));
            ShowWindowAsync(_hwnd, SW_SHOWMINNOACTIVE);
            std::thread([h = _hwnd, secs] {
                Sleep(DWORD(secs) * 1000);
                ShowWindowAsync(h, SW_SHOWNOACTIVATE);
            }).detach();
        }
    }
    if (next >= _dumpFrom && next < _dumpTo)
        DumpTexture(_rdev.Get(), _gameQueue.Get(), _bb[i].Get(), D3D12_RESOURCE_STATE_PRESENT,
                    env.dumpDir + std::format(L"\\f{:06}_render", next));
    if (_lost)
        return PresentFallback(sync, flags);
    _gameQueue->Signal(_rgameFence.Get(), ++_rgameValue);
    Latency::Watch(_rgameFence.Get(), _rgameValue, next, Latency::GameDone);
    if (_llMode < 0)
    {
        _llMode = CurrentSettings().lowLatency;
        _earlySend = CurrentSettings().earlySend;
    }
    if (!env.dumpDir.empty() && next % 30 == 0)
    {
        // <dumpDir>\mgpu_ll.trigger: switch MgpuLowLatency live (0-2 inside)
        const std::wstring trig = env.dumpDir + L"\\mgpu_ll.trigger";
        if (FILE* fp = _wfopen(trig.c_str(), L"r"))
        {
            int v = -1;
            if (fscanf(fp, "%d", &v) == 1 && v >= 0 && v <= 2)
                _llMode = v;
            fclose(fp);
            DeleteFileW(trig.c_str());
            Log(0, std::format("low latency mode {} from the trigger at frame {}", _llMode, next));
        }
        const std::wstring es = env.dumpDir + L"\\mgpu_es.trigger";
        if (FILE* fp = _wfopen(es.c_str(), L"r"))
        {
            int v = -1;
            if (fscanf(fp, "%d", &v) == 1 && v >= 0 && v <= 1)
                _earlySend = v != 0;
            fclose(fp);
            DeleteFileW(es.c_str());
            Log(0, std::format("early send {} from the trigger at frame {}", _earlySend, next));
        }
    }
    _clock.Push(_rgameFence.Get(), _rgameValue); // low latency pacing and the added latency stat

    double sendMs = -1;
    UINT64 extraBytes = 0; // HUD-less colour and vectors sent beside the frame
    {
        std::unique_lock lk(_m);
        // Bound the lead over the display card ([Mgpu] MaxFramesAhead, default 1).
        // Overlap: one frame ahead, and a second one only when the work stage is about to be free (just in time). A
        // game free to run 2 frames ahead gave the same frame rate with a whole display frame more latency.
        // ... only while the game waits for the display card: with the render card the bound it never does, and a second
        // frame would only queue on the render card (a frame of latency, no frame rate)
        const bool jit = _pipeline != 0 && _leadWaitMs > 0.5;
        const UINT64 lead = (std::max)(1u, CurrentSettings().maxLead);
        // Early send: the copy goes to the GPU before the lead wait (the slot's last reader is done: consumed >= g - 2)
        const bool early = _earlySend && lead == 1 && !jit;
        if (!early)
        {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            const int64_t tw0 = QpcTicks();
            LARGE_INTEGER qf;
            QueryPerformanceFrequency(&qf);
            const int64_t limitQpc = QpcTicks() + qf.QuadPart / 4;
            auto granted = [&] { return jit && _scheduled < _consumed + 2 && _workFrame >= _scheduled; };
            auto ready = [&] {
                return _stop || _scheduled < _consumed + lead || (granted() && QpcTicks() >= _grantQpc);
            };
            while (!ready())
            {
                if (granted())
                {
                    // the time is known: a precise sleep outside the lock (a condition variable wakes a tick late)
                    const int64_t at = (std::min)(_grantQpc, limitQpc);
                    lk.unlock();
                    PreciseSleepUntil(at);
                    lk.lock();
                    if (at >= limitQpc)
                        break;
                }
                else if (_cv.wait_until(lk, limit) == std::cv_status::timeout)
                    break;
            }
            const double waited = double(QpcTicks() - tw0) * 1000.0 / double(qf.QuadPart);
            if (waited < 250)
                _leadWaitMs = _leadWaitMs.load() * 0.95 + waited * 0.05;
        }
        const UINT64 g = _scheduled + 1;
        const UINT s = UINT(g % K);
        if (g > K && _filledR->GetCompletedValue() >= g - K && _rQuery)
            sendMs = ReadTs(_rReadback.Get(), s, _rTsFreq);
        _rcopy->Wait(_rgameFence.Get(), _rgameValue);
        if (_picked[s])
            _rcopy->Wait(_releasedR.Get(), _picked[s]);
        ID3D12CommandList* l[2] = { _rList[i * K + s].Get(), nullptr };
        UINT nl = 1;
        SlotMeta meta {};
        if (_rxList[s])
        {
            std::lock_guard cg(_capM);
            meta.hud = _wantHud && i < _hudR.size() && _hudTag[i] == g;
            meta.mv = _wantMv && i < _mvR.size() && _mvR[i] && _mvTag[i] == g;
            // the slot's previous send list must be done before its allocator is reset: else no extras this frame
            if ((meta.hud || meta.mv) && g > K && !WaitReuse(_filledR.Get(), g - K, "send", &_stop, &_lost))
                meta.hud = meta.mv = false;
            if (meta.hud || meta.mv)
            {
                _rxAlloc[s]->Reset();
                auto* c = _rxList[s].Get();
                c->Reset(_rxAlloc[s].Get(), nullptr);
                auto send = [&](ID3D12Resource* srcTex, UINT64 offset)
                {
                    auto sd = srcTex->GetDesc();
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
                    UINT64 bytes = 0;
                    _rdev->GetCopyableFootprints(&sd, 0, 1, offset, &fp, nullptr, nullptr, &bytes);
                    extraBytes += bytes;
                    D3D12_TEXTURE_COPY_LOCATION dl { _slotR[s].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    dl.PlacedFootprint = fp;
                    D3D12_TEXTURE_COPY_LOCATION sl { srcTex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    c->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
                };
                if (meta.hud)
                    send(_hudR[i].Get(), _hudOffset);
                if (meta.mv)
                {
                    send(_mvR[i].Get(), _mvOffset);
                    meta.g = _mvConst[i];
                    meta.mvDesc = _mvR[i]->GetDesc();
                }
                c->Close();
                l[nl++] = c;
            }
        }
        _meta[s] = meta;
        _gameVal[s] = _rgameValue;
        _rcopy->ExecuteCommandLists(nl, l);
        _rcopy->Signal(_filledR.Get(), g);
        Latency::Mark(g, Latency::Submit);
        Latency::Watch(_filledR.Get(), g, g, Latency::Sent);
        if (early)
            _cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return _scheduled < _consumed + lead || _stop; });
        _sync[s] = sync;
        _flags[s] = flags;
        _tSched[s] = std::chrono::steady_clock::now();
        _scheduled = g;
        _bbFrame[i] = g;
    }
    _cv.notify_all();

    // The game's next work on the next back buffer waits for that buffer's last send.
    {
        std::lock_guard cg(_capM);
        _bbIndex = (i + 1) % _gameCount;
    }
    if (_bbFrame[_bbIndex])
        _gameQueue->Wait(_filledR.Get(), _bbFrame[_bbIndex]);
    _presentCount++;
    const double slept = _pacer.Pace(_llMode, _clock, _rgameValue, tEntry.QuadPart);

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard sg(StatsMutex());
    auto& st = MutableStats();
    st.produced++;
    if (_lastRender.time_since_epoch().count())
        st.renderFrameMs = Ema(st.renderFrameMs, std::chrono::duration<double, std::milli>(now - _lastRender).count());
    _lastRender = now;
    if (sendMs > 0)
    {
        st.sendMs = Ema(st.sendMs, sendMs);
        st.sendGBs = double(_payloadBytes) / (st.sendMs * 1e6);
    }
    st.frameBytes = Ema(st.frameBytes, double(_payloadBytes + extraBytes));
    if (_llMode && _presentCount % 600 == 0)
        Log(0, std::format("low latency {}: render queue {:.2f} ms, pacing sleep {:.2f} ms (last {:.2f})",
                           _llMode, _pacer.QueueMs(), _pacer.SleepMs(), slept));
    return S_OK;
}

// Display card gone: the game keeps its back buffers, a swapchain on the render card's queue presents copies of them
// (Windows' cross-adapter present), no NR
HRESULT ProxySwapChain::EnterFallback()
{
    Log(2, "display card lost: single-GPU passthrough from the render card, NR off");
    StopPresenter();
    // Send copies waiting on the display card's release fence would never run
    const HRESULT hs = _releasedR->Signal(_scheduled + 1000);
    WaitFence(_filledR.Get(), _scheduled, 2000);
    if (_filledR->GetCompletedValue() < _scheduled)
    {
        Log(1, std::format("send queue stuck (release signal {:X}): filled fence set from the CPU", (uint32_t) hs));
        _filledR->Signal(_scheduled);
    }
    DrainRender();
    {
        std::lock_guard g(_m);
        _consumed = _taken = _scheduled;
    }
    {
        std::lock_guard g(_capM);
        _wantHud = _wantMv = false;
    }
    _lost = true;
    ReleaseBuffers(true);
    {
        // releasing a swapchain in exclusive fullscreen raises an exception in DXGI (the game crashed)
        std::lock_guard g(_scMutex);
        BOOL fs = FALSE;
        if (SUCCEEDED(_real->GetFullscreenState(&fs, nullptr)) && fs)
            Log(1, std::format("leaving exclusive fullscreen before the fallback: {:X}",
                               (uint32_t) _real->SetFullscreenState(FALSE, nullptr)));
        _real.Reset();
        _fsCached = FALSE;
        _fsDesc.Windowed = TRUE;
    }
    DXGI_SWAP_CHAIN_DESC1 d = _gameDesc;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.BufferCount = _realCount;
    d.Flags = RealFlags(_gameDesc.Flags, _tearing);
    d.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 3 && FAILED(hr); ++attempt)
    {
        if (attempt)
            Sleep(100); // the removed swapchain may still be leaving the window
        hr = _original(_factory.Get(), _gameQueue.Get(), _hwnd, &d, _hasFs ? &_fsDesc : nullptr, nullptr, &sc1);
    }
    if (SUCCEEDED(hr))
        hr = sc1.As(&_real);
    if (SUCCEEDED(hr))
        hr = CreateBuffers(true);
    std::lock_guard g(StatsMutex());
    auto& st = MutableStats();
    st.presenter = false;
    st.fallback = SUCCEEDED(hr);
    if (FAILED(hr))
    {
        _broken = true;
        Log(2, std::format("single-GPU fallback swapchain failed {:X}", (uint32_t) hr));
        return hr;
    }
    Log(1, std::format("single-GPU fallback presenting {}x{} on bus {}", _gameDesc.Width, _gameDesc.Height, st.render.bus));
    return S_OK;
}

HRESULT ProxySwapChain::PresentFallback(UINT sync, UINT flags)
{
    if (CurrentEnv().nrIdle)
        CurrentEnv().nrIdle("display card lost: single-GPU fallback, NR off");
    const UINT i = _bbIndex;
    HRESULT hr;
    {
        std::lock_guard g(_scMutex);
        const UINT m = _real->GetCurrentBackBufferIndex();
        if (i * _realCount + m >= _fList.size())
            return DXGI_ERROR_INVALID_CALL;
        ID3D12CommandList* l[] = { _fList[i * _realCount + m].Get() };
        _gameQueue->ExecuteCommandLists(1, l);
        BOOL fs = FALSE;
        _real->GetFullscreenState(&fs, nullptr);
        UINT pf = flags & ~(UINT) DXGI_PRESENT_ALLOW_TEARING;
        if (sync == 0 && _tearing && !fs)
            pf |= DXGI_PRESENT_ALLOW_TEARING;
        hr = _real->Present(sync, pf);
    }
    if (hr != _lastPresentHr)
        Log(FAILED(hr) ? 2 : 0, std::format("fallback Present {:X}, window {}", (uint32_t) hr,
                                            IsIconic(_hwnd) ? "minimised" : "shown"));
    _lastPresentHr = hr;
    // a cross-adapter swapchain can report occluded while its window shows; games throttle on it
    if (hr == DXGI_STATUS_OCCLUDED && !IsIconic(_hwnd))
        hr = S_OK;
    {
        std::lock_guard cg(_capM);
        _bbIndex = (i + 1) % _gameCount;
    }
    {
        std::lock_guard g(_m);
        _consumed = _taken = ++_scheduled;
    }
    _presentCount++;
    if (_waitable)
        ReleaseSemaphore(_waitable, 1, nullptr);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard sg(StatsMutex());
    auto& st = MutableStats();
    st.produced++;
    st.presented++;
    if (_lastRender.time_since_epoch().count())
        st.renderFrameMs = Ema(st.renderFrameMs, std::chrono::duration<double, std::milli>(now - _lastRender).count());
    st.displayFrameMs = st.renderFrameMs;
    _lastRender = now;
    if (_presentCount % 600 == 0)
        Log(0, std::format("stats: fallback render {:.2f} ms presented {}", st.renderFrameMs, st.presented));
    return hr;
}

ProxySwapChain::~ProxySwapChain()
{
    ProxySwapChain* self = this;
    s_active.compare_exchange_strong(self, nullptr);
    if (_thread.joinable())
        StopPresenter();
    DrainRender();
    ReleaseBuffers();
    ReleaseRetired();
    delete _ui;
    _ui = nullptr;
    _real.Reset();
    if (_waitable)
        CloseHandle(_waitable);
    std::lock_guard g(StatsMutex());
    MutableStats().presenter = false;
}

// ---- IUnknown
HRESULT STDMETHODCALLTYPE ProxySwapChain::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4) || riid == __uuidof(ProxySwapChain))
    {
        AddRef();
        *ppv = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE ProxySwapChain::AddRef() { return ++_ref; }
ULONG STDMETHODCALLTYPE ProxySwapChain::Release()
{
    ULONG r = --_ref;
    if (r == 0)
        delete this;
    return r;
}

// ---- IDXGIObject / DeviceSubObject
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetPrivateData(REFGUID n, UINT s, const void* d)
{
    return _real->SetPrivateData(n, s, d);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetPrivateDataInterface(REFGUID n, const IUnknown* u)
{
    return _real->SetPrivateDataInterface(n, u);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetPrivateData(REFGUID n, UINT* s, void* d)
{
    return _real->GetPrivateData(n, s, d);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetParent(REFIID riid, void** pp) { return _real->GetParent(riid, pp); }
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetDevice(REFIID riid, void** pp)
{
    return _gameQueue->QueryInterface(riid, pp);
}

// ---- IDXGISwapChain
HRESULT STDMETHODCALLTYPE ProxySwapChain::Present(UINT sync, UINT flags) { return DoPresent(sync, flags); }
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetBuffer(UINT i, REFIID riid, void** pp)
{
    if (i >= _bb.size())
        return DXGI_ERROR_INVALID_CALL;
    return _bb[i]->QueryInterface(riid, pp);
}
// Mode changes run with the presenter stopped: DXGI sends them to the window thread while a Present may wait for it
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetFullscreenState(BOOL fs, IDXGIOutput* t)
{
    const bool running = _thread.joinable();
    if (running)
        StopPresenter();
    HRESULT hr;
    {
        std::lock_guard g(_scMutex);
        hr = _real->SetFullscreenState(fs, t);
        _tearingOff = false;
        BOOL now = FALSE;
        ComPtr<IDXGIOutput> o;
        _real->GetFullscreenState(&now, &o);
        _fsCached = now;
        std::lock_guard fg(_fsM);
        _fsOutput = o;
    }
    if (running)
        StartPresenter();
    Log(0, std::format("SetFullscreenState {} output {} -> {:X}, now {}", fs, (void*) t, (uint32_t) hr, (int) _fsCached));
    return hr;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetFullscreenState(BOOL* fs, IDXGIOutput** t)
{
    // the presenter keeps the state current; the real call could block behind a Present
    const BOOL now = _fsCached;
    if (fs)
        *fs = now;
    if (t)
    {
        std::lock_guard fg(_fsM);
        *t = now ? _fsOutput.Get() : nullptr;
        if (*t)
            (*t)->AddRef();
    }
    return S_OK;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* d)
{
    if (!d)
        return E_INVALIDARG;
    HRESULT hr;
    {
        std::lock_guard g(_scMutex);
        hr = _real->GetDesc(d);
    }
    if (SUCCEEDED(hr))
    {
        d->BufferCount = _gameCount;
        d->Flags = _gameDesc.Flags;
        d->SwapEffect = _gameDesc.SwapEffect;
        d->BufferUsage = _gameDesc.BufferUsage;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    Log(0, std::format("ResizeBuffers {} {}x{} fmt {} flags {:X}", n, w, h, (int) f, flags));
    if (!_lost)
        StopPresenter();
    Alive("resize: presenter stopped");
    DrainRender();
    {
        // frames sent but not presented belong to the old buffers
        std::lock_guard g(_m);
        const UINT64 dropped = _scheduled - _consumed;
        _consumed = _taken = _scheduled;
        if (dropped && _waitable)
            ReleaseSemaphore(_waitable, (LONG) (std::min<UINT64>)(dropped, 8), nullptr);
        if (dropped)
            Log(0, std::format("ResizeBuffers: {} unpresented frames dropped", dropped));
    }
    if (!_lost && FAILED(_ddev->GetDeviceRemovedReason()))
    {
        // lost before or during this resize: resize the single-GPU fallback instead
        {
            std::lock_guard sg(StatsMutex());
            MutableStats().fallbackReason =
                std::format("display device removed ({:X}) at a resize", (uint32_t) _ddev->GetDeviceRemovedReason());
        }
        _lostPending = true;
        if (FAILED(EnterFallback()))
            return DXGI_ERROR_DEVICE_REMOVED;
    }
    ReleaseBuffers(false, !_lost);
    _retireAfter = _presentCount + 30;
    Alive("resize: released");
    if (n)
        _gameCount = (std::max)(2u, n);
    _gameDesc.Flags = flags;
    HRESULT hr;
    {
        std::lock_guard g(_scMutex);
        hr = _real->ResizeBuffers(_realCount, w, h, f, RealFlags(flags, _tearing));
    }
    if (FAILED(hr))
        Log(2, std::format("real ResizeBuffers failed {:X}", (uint32_t) hr));
    Alive("resize: real resized");
    HRESULT hb = CreateBuffers();
    Alive("resize: buffers created");
    if (FAILED(hb))
        Log(2, std::format("proxy buffers failed {:X}", (uint32_t) hb));
    if (!_lost)
        StartPresenter();
    return FAILED(hr) ? hr : hb;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeTarget(const DXGI_MODE_DESC* m)
{
    const bool running = _thread.joinable();
    if (running)
        StopPresenter();
    HRESULT hr;
    {
        std::lock_guard g(_scMutex);
        hr = _real->ResizeTarget(m);
    }
    if (running)
        StartPresenter();
    if (m)
        Log(0, std::format("ResizeTarget {}x{} fmt {} -> {:X}", m->Width, m->Height, (int) m->Format, (uint32_t) hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetContainingOutput(IDXGIOutput** o)
{
    std::lock_guard g(_scMutex);
    return _real->GetContainingOutput(o);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* s)
{
    std::lock_guard g(_scMutex);
    return _real->GetFrameStatistics(s);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetLastPresentCount(UINT* c)
{
    if (!c)
        return E_INVALIDARG;
    *c = _presentCount;
    return S_OK;
}

// ---- IDXGISwapChain1
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* d)
{
    if (!d)
        return E_INVALIDARG;
    *d = _gameDesc;
    d->BufferCount = _gameCount;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d)
{
    std::lock_guard g(_scMutex);
    return _real->GetFullscreenDesc(d);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetHwnd(HWND* h)
{
    if (!h)
        return E_INVALIDARG;
    *h = _hwnd;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetCoreWindow(REFIID riid, void** pp) { return _real->GetCoreWindow(riid, pp); }
HRESULT STDMETHODCALLTYPE ProxySwapChain::Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*)
{
    return DoPresent(sync, flags);
}
BOOL STDMETHODCALLTYPE ProxySwapChain::IsTemporaryMonoSupported() { return _real->IsTemporaryMonoSupported(); }
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetRestrictToOutput(IDXGIOutput** o) { return _real->GetRestrictToOutput(o); }
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetBackgroundColor(const DXGI_RGBA* c) { return _real->SetBackgroundColor(c); }
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetBackgroundColor(DXGI_RGBA* c) { return _real->GetBackgroundColor(c); }
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetRotation(DXGI_MODE_ROTATION r) { return _real->SetRotation(r); }
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetRotation(DXGI_MODE_ROTATION* r) { return _real->GetRotation(r); }

// ---- IDXGISwapChain2
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetSourceSize(UINT w, UINT h)
{
    std::lock_guard g(_scMutex);
    return _real->SetSourceSize(w, h);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetSourceSize(UINT* w, UINT* h)
{
    std::lock_guard g(_scMutex);
    return _real->GetSourceSize(w, h);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetMaximumFrameLatency(UINT l)
{
    if (!_waitable)
        return DXGI_ERROR_INVALID_CALL;
    if (l > _maxLatency)
        ReleaseSemaphore(_waitable, LONG(l - _maxLatency), nullptr);
    _maxLatency = l ? l : 1;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetMaximumFrameLatency(UINT* l)
{
    if (!l)
        return E_INVALIDARG;
    *l = _maxLatency;
    return _waitable ? S_OK : DXGI_ERROR_INVALID_CALL;
}
HANDLE STDMETHODCALLTYPE ProxySwapChain::GetFrameLatencyWaitableObject()
{
    if (!_waitable)
        return nullptr;
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), _waitable, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return h;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* m)
{
    return _real->SetMatrixTransform(m);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* m)
{
    return _real->GetMatrixTransform(m);
}

// ---- IDXGISwapChain3 / 4
UINT STDMETHODCALLTYPE ProxySwapChain::GetCurrentBackBufferIndex() { return _bbIndex; }
HRESULT STDMETHODCALLTYPE ProxySwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s)
{
    HRESULT hr;
    {
        std::lock_guard g(_scMutex);
        hr = _real->CheckColorSpaceSupport(c, s);
    }
    static UINT logged = 0;
    if (c < 32 && !(logged & (1u << c)))
    {
        logged |= 1u << c;
        Log(0, std::format("CheckColorSpaceSupport {} -> {:X} support {:X}", (int) c, (uint32_t) hr, s ? *s : 0));
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE c)
{
    HRESULT hr;
    {
        std::lock_guard g(_scMutex);
        hr = _real->SetColorSpace1(c);
    }
    if ((UINT) c != _loggedCs)
    {
        _loggedCs = c;
        Log(0, std::format("SetColorSpace1 {} -> {:X}", (int) c, (uint32_t) hr));
        std::lock_guard g(StatsMutex());
        MutableStats().colorSpace = c;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                                         const UINT*, IUnknown* const*)
{
    return ResizeBuffers(n, w, h, f, flags);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* m)
{
    HRESULT hr;
    {
        std::lock_guard g(_scMutex);
        hr = _real->SetHDRMetaData(t, s, m);
    }
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        UINT maxNits = 0;
        if (t == DXGI_HDR_METADATA_TYPE_HDR10 && m && s >= sizeof(DXGI_HDR_METADATA_HDR10))
            maxNits = static_cast<DXGI_HDR_METADATA_HDR10*>(m)->MaxContentLightLevel;
        Log(0, std::format("SetHDRMetaData type {} size {} max content {} nits -> {:X}", (int) t, s, maxNits,
                           (uint32_t) hr));
    }
    return hr;
}
} // namespace Mgpu
