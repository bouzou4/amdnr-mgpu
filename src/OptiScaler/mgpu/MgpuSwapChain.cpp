#include "pch.h"
#include "MgpuSwapChain.h"

#include <format>

namespace Mgpu
{
static std::mutex s_statsMutex;
std::mutex& StatsMutex() { return s_statsMutex; }

static inline UINT64 AlignUp(UINT64 v, UINT64 a) { return (v + a - 1) & ~(a - 1); }
static inline double Ema(double prev, double v) { return prev <= 0 ? v : prev * 0.95 + v * 0.05; }

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
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 {};
    _rdev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    _rTimestamps = o3.CopyQueueTimestampQueriesSupported != FALSE;
    _rcopy->GetTimestampFrequency(&_rTsFreq);
    _dq->GetTimestampFrequency(&_dTsFreq);

    if (desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        _waitable = CreateSemaphoreW(nullptr, (LONG) _maxLatency, 64, nullptr);

    if (FAILED(hr = CreateBuffers()))
        return hr;
    StartPresenter();
    {
        std::lock_guard g(StatsMutex());
        MutableStats().presenter = true;
    }
    Log(0, std::format("proxy swapchain {}x{} fmt {} game buffers {} real buffers {} tearing {} waitable {}",
                       _gameDesc.Width, _gameDesc.Height, (int) _gameDesc.Format, _gameCount, _realCount, _tearing,
                       _waitable != nullptr));
    return S_OK;
}

HRESULT ProxySwapChain::CreateBuffers()
{
    HRESULT hr;
    DXGI_SWAP_CHAIN_DESC1 rd {};
    _real->GetDesc1(&rd);
    _gameDesc.Width = rd.Width;
    _gameDesc.Height = rd.Height;
    _gameDesc.Format = rd.Format;
    _realCount = rd.BufferCount;

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
    _bb.resize(_gameCount);
    _bbFrame.assign(_gameCount, 0);
    for (auto& p : _picked)
        p = 0;
    _bbIndex = 0;
    for (UINT i = 0; i < _gameCount; ++i)
    {
        if (FAILED(hr = _rdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_PRESENT,
                                                       nullptr, IID_PPV_ARGS(&_bb[i]))))
            return hr;
        _bb[i]->SetName(std::format(L"Mgpu proxy back buffer {}", i).c_str());
    }

    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    _rdev->GetCopyableFootprints(&t, 0, 1, 0, &_fp, &rows, &rowBytes, &total);
    _payloadBytes = UINT64(rows) * rowBytes;
    _slotBytes = AlignUp(total, 65536);

    D3D12_HEAP_DESC hd {};
    hd.SizeInBytes = _slotBytes * K;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    if (FAILED(hr = _ddev->CreateHeap(&hd, IID_PPV_ARGS(&_heapD))))
        return hr;
    HANDLE h = nullptr;
    if (FAILED(hr = _ddev->CreateSharedHandle(_heapD.Get(), nullptr, GENERIC_ALL, nullptr, &h)))
        return hr;
    hr = _rdev->OpenSharedHandle(h, IID_PPV_ARGS(&_heapR));
    CloseHandle(h);
    if (FAILED(hr))
        return hr;

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
    }
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    if (FAILED(_ddev->CreateQueryHeap(&qd, IID_PPV_ARGS(&_dQuery))) ||
        FAILED(_ddev->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&_dReadback))))
        _dQuery.Reset();

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

    // Display side: one prerecorded ingest per (slot, real back buffer).
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

    std::lock_guard g(StatsMutex());
    auto& st = MutableStats();
    st.width = rd.Width;
    st.height = rd.Height;
    st.format = rd.Format;
    st.ringSlots = K;
    st.slotBytes = _slotBytes;
    return S_OK;
}

void ProxySwapChain::ReleaseBuffers()
{
    _rList.clear();
    _rAlloc.clear();
    _dList.clear();
    _dAlloc.clear();
    for (UINT s = 0; s < K; ++s)
    {
        _slotR[s].Reset();
        _slotD[s].Reset();
    }
    _heapR.Reset();
    _heapD.Reset();
    _rQuery.Reset();
    _rReadback.Reset();
    _dQuery.Reset();
    _dReadback.Reset();
    _bb.clear();
    _realBb.clear();
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

void ProxySwapChain::DrainRender()
{
    if (_gameQueue && _rgameFence)
    {
        _gameQueue->Signal(_rgameFence.Get(), ++_rgameValue);
        WaitFence(_rgameFence.Get(), _rgameValue, 3000);
    }
    WaitFence(_filledR.Get(), _scheduled, 3000);
}

void ProxySwapChain::StartPresenter()
{
    _stop = false;
    _thread = std::thread([this] { PresenterLoop(); });
}

void ProxySwapChain::StopPresenter()
{
    {
        std::lock_guard g(_m);
        _stop = true;
    }
    _cv.notify_all();
    if (_thread.joinable())
        _thread.join();
    if (_dq && _dFence)
    {
        _dq->Signal(_dFence.Get(), ++_dValue);
        WaitFence(_dFence.Get(), _dValue, 3000);
    }
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
        lk.unlock();

        // Timestamps of this slot's previous ingest, if done.
        double ingestMs = -1;
        if (slotDisplayValue[s] && _dFence->GetCompletedValue() >= slotDisplayValue[s])
            ingestMs = ReadTs(_dReadback.Get(), s, _dTsFreq);

        double nrMs = 0;
        HRESULT hr;
        {
            std::lock_guard sg(_scMutex);
            const UINT m = _real->GetCurrentBackBufferIndex();
            _dq->Wait(_filledD.Get(), g);
            ID3D12CommandList* l[] = { _dList[s * _realCount + m].Get() };
            _dq->ExecuteCommandLists(1, l);
            _dq->Signal(_releasedD.Get(), g);
            const bool dump = g >= _dumpFrom && g < _dumpTo;
            if (dump)
                DumpTexture(_ddev.Get(), _dq.Get(), _realBb[m].Get(), D3D12_RESOURCE_STATE_PRESENT,
                            env.dumpDir + std::format(L"\\f{:06}_ingest", g));
            if (env.nrWanted && env.nrWanted() && env.nrFrame)
            {
                auto t0 = std::chrono::steady_clock::now();
                env.nrFrame(_ddev.Get(), _dq.Get(), _realBb[m].Get());
                nrMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            if (dump && nrMs > 0)
                DumpTexture(_ddev.Get(), _dq.Get(), _realBb[m].Get(), D3D12_RESOURCE_STATE_PRESENT,
                            env.dumpDir + std::format(L"\\f{:06}_display", g));
            BOOL fs = FALSE;
            _real->GetFullscreenState(&fs, nullptr);
            UINT pf = flags & ~(UINT) DXGI_PRESENT_ALLOW_TEARING;
            if (sync == 0 && _tearing && !fs)
                pf |= DXGI_PRESENT_ALLOW_TEARING;
            hr = _real->Present(sync, pf);
            _dq->Signal(_dFence.Get(), ++_dValue);
            slotDisplayValue[s] = _dValue;
        }
        if (FAILED(hr))
            Log(2, std::format("real Present failed {:X}", (uint32_t) hr));

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
        if (_lastDisplay.time_since_epoch().count())
            st.displayFrameMs = Ema(st.displayFrameMs, std::chrono::duration<double, std::milli>(now - _lastDisplay).count());
        _lastDisplay = now;
        st.latencyMs = Ema(st.latencyMs, std::chrono::duration<double, std::milli>(now - tSched).count());
        if (ingestMs > 0)
        {
            st.ingestMs = Ema(st.ingestMs, ingestMs);
            st.ingestGBs = double(_payloadBytes) / (st.ingestMs * 1e6);
        }
        if (nrMs > 0)
            st.nrMs = Ema(st.nrMs, nrMs);
        st.ringDepth = UINT(_scheduled - g);
        if (st.presented % 600 == 0)
            Log(0, std::format("stats: render {:.2f} ms ({:.0f}% busy) display {:.2f} ms ({:.0f}% busy) send {:.2f} ms {:.1f} GB/s "
                               "ingest {:.2f} ms {:.1f} GB/s latency {:.2f} ms nr {:.2f} ms produced {} presented {} skipped {}",
                               st.renderFrameMs, st.renderBusy, st.displayFrameMs, st.displayBusy, st.sendMs, st.sendGBs,
                               st.ingestMs, st.ingestGBs, st.latencyMs, st.nrMs, st.produced, st.presented, st.skipped));
    }
}

HRESULT ProxySwapChain::DoPresent(UINT sync, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST)
        return S_OK;
    const UINT i = _bbIndex;
    const auto& env = CurrentEnv();
    const UINT64 next = _scheduled + 1;
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
    }
    if (next >= _dumpFrom && next < _dumpTo)
        DumpTexture(_rdev.Get(), _gameQueue.Get(), _bb[i].Get(), D3D12_RESOURCE_STATE_PRESENT,
                    env.dumpDir + std::format(L"\\f{:06}_render", next));
    _gameQueue->Signal(_rgameFence.Get(), ++_rgameValue);

    double sendMs = -1;
    {
        std::unique_lock lk(_m);
        // Bound the lead over the display card to two frames.
        _cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return _scheduled < _consumed + 2 || _stop; });
        const UINT64 g = _scheduled + 1;
        const UINT s = UINT(g % K);
        if (g > K && _filledR->GetCompletedValue() >= g - K && _rQuery)
            sendMs = ReadTs(_rReadback.Get(), s, _rTsFreq);
        _rcopy->Wait(_rgameFence.Get(), _rgameValue);
        if (_picked[s])
            _rcopy->Wait(_releasedR.Get(), _picked[s]);
        ID3D12CommandList* l[] = { _rList[i * K + s].Get() };
        _rcopy->ExecuteCommandLists(1, l);
        _rcopy->Signal(_filledR.Get(), g);
        _sync[s] = sync;
        _flags[s] = flags;
        _tSched[s] = std::chrono::steady_clock::now();
        _scheduled = g;
        _bbFrame[i] = g;
    }
    _cv.notify_all();

    // The game's next work on the next back buffer waits for that buffer's last send.
    _bbIndex = (i + 1) % _gameCount;
    if (_bbFrame[_bbIndex])
        _gameQueue->Wait(_filledR.Get(), _bbFrame[_bbIndex]);
    _presentCount++;

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
    return S_OK;
}

ProxySwapChain::~ProxySwapChain()
{
    if (_thread.joinable())
        StopPresenter();
    DrainRender();
    ReleaseBuffers();
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
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetFullscreenState(BOOL fs, IDXGIOutput* t)
{
    std::lock_guard g(_scMutex);
    return _real->SetFullscreenState(fs, t);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::GetFullscreenState(BOOL* fs, IDXGIOutput** t)
{
    std::lock_guard g(_scMutex);
    return _real->GetFullscreenState(fs, t);
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
    StopPresenter();
    DrainRender();
    ReleaseBuffers();
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
    HRESULT hb = CreateBuffers();
    if (FAILED(hb))
        Log(2, std::format("proxy buffers failed {:X}", (uint32_t) hb));
    StartPresenter();
    return FAILED(hr) ? hr : hb;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeTarget(const DXGI_MODE_DESC* m)
{
    std::lock_guard g(_scMutex);
    return _real->ResizeTarget(m);
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
    std::lock_guard g(_scMutex);
    return _real->CheckColorSpaceSupport(c, s);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE c)
{
    std::lock_guard g(_scMutex);
    return _real->SetColorSpace1(c);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                                         const UINT*, IUnknown* const*)
{
    return ResizeBuffers(n, w, h, f, flags);
}
HRESULT STDMETHODCALLTYPE ProxySwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* m)
{
    std::lock_guard g(_scMutex);
    return _real->SetHDRMetaData(t, s, m);
}
} // namespace Mgpu
