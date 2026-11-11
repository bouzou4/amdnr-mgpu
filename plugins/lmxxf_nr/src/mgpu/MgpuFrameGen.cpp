#include "MgpuCommon.h"
#include "MgpuFrameGen.h"
#include "../GpuWait.h"

#include <ffx_api.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_dx12.h>

#include <detours/detours.h>

#include <algorithm>
#include <cstdio>
#include <format>

namespace Mgpu
{
namespace
{
std::atomic<int> s_liveFg { -1 }, s_liveNr { -1 }, s_liveFgHud { -1 }, s_liveFgSyn { -1 }, s_liveFgOrder { -1 };
std::atomic<bool> s_gameFg { false };
std::atomic<DisplayFg*> s_timed { nullptr };

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PFN_Present o_Present = nullptr;
PFN_Present1 o_Present1 = nullptr;

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* self, UINT s, UINT f)
{
    if (!(f & DXGI_PRESENT_TEST))
        if (auto* t = s_timed.load())
            t->OnDxgiPresent();
    return o_Present(self, s, f);
}
HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* self, UINT s, UINT f, const DXGI_PRESENT_PARAMETERS* p)
{
    if (!(f & DXGI_PRESENT_TEST))
        if (auto* t = s_timed.load())
            t->OnDxgiPresent();
    return o_Present1(self, s, f, p);
}

const FfxFns& Ffx() { return CurrentEnv().ffx; }

D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER v {};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    return v;
}

D3D12_RESOURCE_STATES D3dState(uint32_t s)
{
    switch (s)
    {
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case FFX_API_RESOURCE_STATE_PRESENT:
    default:
        return D3D12_RESOURCE_STATE_PRESENT;
    }
}
} // namespace

bool DisplayFg::Available()
{
    const auto& f = Ffx();
    return f.create && f.destroy && f.configure && f.dispatch;
}

HRESULT DisplayFg::CreateSwapChain(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
                                   const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                   IDXGISwapChain4** out)
{
    if (!Available())
        return E_NOTIMPL;
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC f {};
    if (fs)
        f = *fs;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 c {};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    c.swapchain = out;
    c.hwnd = hwnd;
    c.desc = &d;
    c.fullscreenDesc = fs ? &f : nullptr;
    c.dxgiFactory = factory;
    c.gameQueue = queue;
    const uint32_t r = Ffx().create(&_sc, &c.header, nullptr);
    if (r != FFX_API_RETURN_OK || !*out)
    {
        Log(2, std::format("display FG: FG swapchain failed ({}): plain swapchain", r));
        _sc = nullptr;
        return E_FAIL;
    }
    s_timed = this;
    _scObj = *out;
    const auto& st = CurrentSettings();
    Log(0, std::format("display FG: FFX frame generation swapchain {}x{} fmt {} on the {} queue", d.Width, d.Height,
                       (int) d.Format, st.fgOwnQueue ? "own FG" : "presenter's"));
    if (st.fgPtMargin >= 0)
    {
        FfxApiSwapchainFramePacingTuning t {};
        t.safetyMarginInMs = st.fgPtMargin;
        t.varianceFactor = st.fgPtVariance;
        t.allowHybridSpin = st.fgPtHybridSpin;
        t.hybridSpinTime = 2;
        t.allowWaitForSingleObjectOnFence = st.fgPtFenceWait;
        ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 kv {};
        kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
        kv.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
        kv.ptr = &t;
        const uint32_t pr = Ffx().configure(&_sc, &kv.header);
        Log(pr == FFX_API_RETURN_OK ? 0 : 2,
            std::format("display FG: pacing tuning margin {:.2f} ms variance {:.2f} hybrid spin {} fence wait {} ({})",
                        t.safetyMarginInMs, t.varianceFactor, t.allowHybridSpin, t.allowWaitForSingleObjectOnFence, pr));
    }
    return S_OK;
}

bool DisplayFg::EnsureContext(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, uint32_t flags, DXGI_FORMAT hudFmt)
{
    if (_fg && w == _w && h == _h && fmt == _fmt && flags == _flags && hudFmt == _hudFmt)
        return true;
    ReleaseContext();
    ffxCreateBackendDX12Desc be {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = dev;
    ffxCreateContextDescFrameGenerationHudless hd {};
    hd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hd.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(hudFmt);
    hd.header.pNext = &be.header;
    ffxCreateContextDescFrameGeneration c {};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    c.header.pNext = hudFmt != DXGI_FORMAT_UNKNOWN ? &hd.header : &be.header;
    c.flags = flags;
    c.displaySize = { w, h };
    c.maxRenderSize = { w, h };
    c.backBufferFormat = ffxApiGetSurfaceFormatDX12(fmt);
    const uint32_t r = Ffx().create(&_fg, &c.header, nullptr);
    if (r != FFX_API_RETURN_OK)
    {
        if (!_loggedFail)
            Log(2, std::format("display FG: context failed ({})", r));
        _loggedFail = true;
        _fg = nullptr;
        return false;
    }
    _w = w, _h = h, _fmt = fmt, _flags = flags, _hudFmt = hudFmt;
    _frameId = 0;
    if (!_list)
    {
        for (auto& a : _alloc)
            dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _alloc[0].Get(), nullptr, IID_PPV_ARGS(&_list));
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
        if (_list)
            _list->Close();
    }
    // version of the FG provider that answered
    ffxQueryDescGetVersions vq {};
    vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    vq.device = dev;
    uint64_t count = 0;
    vq.outputCount = &count;
    std::string ver = "?";
    if (Ffx().query && Ffx().query(nullptr, &vq.header) == FFX_API_RETURN_OK && count)
    {
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        vq.versionIds = ids.data();
        vq.versionNames = names.data();
        if (Ffx().query(nullptr, &vq.header) == FFX_API_RETURN_OK && names[0])
            ver = names[0];
    }
    Log(0, std::format("display FG: context {}x{} fmt {} flags {:X} hudless fmt {}, provider {}", w, h, (int) fmt, flags,
                       (int) hudFmt, ver));
    return true;
}

void DisplayFg::ReleaseContext()
{
    if (!_fg)
        return;
    if (_sc)
    {
        ffxConfigureDescFrameGeneration off {};
        off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.frameGenerationEnabled = false;
        off.swapChain = _scObj;
        off.frameID = ++_frameId;
        Ffx().configure(&_fg, &off.header);
        WaitForPresents();
    }
    Ffx().destroy(&_fg, nullptr);
    _fg = nullptr;
    _lastEnabled = false;
}

void DisplayFg::WaitForPresents()
{
    if (!_sc)
        return;
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 w {};
    w.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    Ffx().dispatch(&_sc, &w.header);
}

void DisplayFg::Destroy()
{
    ReleaseContext();
    DisplayFg* self = this;
    s_timed.compare_exchange_strong(self, nullptr);
    if (_sc)
        Ffx().destroy(&_sc, nullptr);
    _sc = nullptr;
    _scObj = nullptr;
    for (auto& d : _dump)
        d.Reset();
}

// Every presented frame, real and generated, on FFX's present thread: the copy the default callback makes, plus
// timing and the dump copies
uint32_t DisplayFg::OnPresent(void* p)
{
    auto* c = static_cast<ffxCallbackDescFrameGenerationPresent*>(p);
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(c->commandList);
    auto* src = static_cast<ID3D12Resource*>(c->currentBackBuffer.resource);
    auto* dst = static_cast<ID3D12Resource*>(c->outputSwapChainBuffer.resource);
    const auto ss = D3dState(c->currentBackBuffer.state), ds = D3dState(c->outputSwapChainBuffer.state);
    if (src && dst && src != dst)
    {
        D3D12_RESOURCE_BARRIER b[2] = { Tr(src, ss, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                        Tr(dst, ds, D3D12_RESOURCE_STATE_COPY_DEST) };
        const UINT nb0 = ss == D3D12_RESOURCE_STATE_COPY_SOURCE ? 1 : 0;
        cl->ResourceBarrier(2 - nb0, b + nb0);
        cl->CopyResource(dst, src);
        if (_dumpArm.load() && src)
        {
            const UINT k = _dumpCount.load();
            if (k < kDumps)
            {
                if (!_dump[k])
                {
                    ComPtr<ID3D12Device> dev;
                    src->GetDevice(IID_PPV_ARGS(&dev));
                    auto d = src->GetDesc();
                    d.Flags = D3D12_RESOURCE_FLAG_NONE;
                    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
                    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&_dump[k]));
                }
                if (_dump[k])
                {
                    cl->CopyResource(_dump[k].Get(), src);
                    _dumpId[k] = c->frameID;
                    _dumpGen[k] = c->isGeneratedFrame;
                    _dumpCount = k + 1;
                }
                if (k + 1 >= kDumps || k + 1 >= _dumpArm.load())
                    _dumpArm = 0;
            }
        }
        for (auto& x : b)
            std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        cl->ResourceBarrier(2 - nb0, b + nb0);
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard g(_tm);
    if (_lastCb.time_since_epoch().count())
        _timing.intervals.push_back(std::chrono::duration<float, std::milli>(now - _lastCb).count());
    _lastCb = now;
    Pending pe;
    pe.generated = c->isGeneratedFrame;
    if (c->isGeneratedFrame)
        _timing.generated++;
    else
    {
        _timing.real++;
        const UINT k = UINT(c->frameID % kTimes);
        if (_gameId[k] == c->frameID && _gameT[k].time_since_epoch().count())
        {
            _timing.latency.push_back(std::chrono::duration<float, std::milli>(now - _gameT[k]).count());
            pe.tGame = _gameT[k];
        }
    }
    if (!_pendingFgMode)
        _pending.clear(), _pendingFgMode = true;
    if (_pending.size() < 16)
        _pending.push_back(pe);
    return FFX_API_RETURN_OK;
}

// DXGI's swapchain code, found on a throwaway swapchain: every DXGI Present in the process comes here (the game's
// goes to the proxy, which is not DXGI's)
void DisplayFg::InstallPresentTiming(PFN_CreateSwapChainForHwnd original, IDXGIFactory2* f, ID3D12CommandQueue* q)
{
    if (o_Present || !original || !f || !q)
        return;
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MgpuFgTiming";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 128, 128, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w)
        return;
    DXGI_SWAP_CHAIN_DESC1 d {};
    d.Width = d.Height = 128;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    if (SUCCEEDED(original(f, q, w, &d, nullptr, nullptr, &sc)) && sc)
    {
        void** vt = *reinterpret_cast<void***>(sc.Get());
        o_Present = (PFN_Present) vt[8];
        o_Present1 = (PFN_Present1) vt[22];
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) o_Present, hkPresent);
        DetourAttach(&(PVOID&) o_Present1, hkPresent1);
        const LONG err = DetourTransactionCommit();
        Log(err == NO_ERROR ? 0 : 2, std::format("display FG: DXGI Present timing {} ({})", err == NO_ERROR ? "on" : "failed", err));
    }
    sc.Reset();
    DestroyWindow(w);
}

void DisplayFg::OnDxgiPresent()
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard g(_tm);
    const float gap = _lastDxgi.time_since_epoch().count()
                          ? std::chrono::duration<float, std::milli>(now - _lastDxgi).count()
                          : -1.0f;
    if (gap >= 0)
        _timing.dxgiIntervals.push_back(gap);
    _lastDxgi = now;
    if (_pending.empty())
    {
        _timing.dxgiUnknown++;
        _lastKind = -1;
        return;
    }
    const Pending p = _pending.front();
    _pending.erase(_pending.begin());
    if (gap >= 0 && _lastKind == 1 && !p.generated)
        _timing.genToReal.push_back(gap);
    else if (gap >= 0 && _lastKind == 0 && p.generated)
        _timing.realToGen.push_back(gap);
    _lastKind = p.generated ? 1 : 0;
    if (p.generated)
        _timing.dxgiGenerated++;
    else
    {
        _timing.dxgiReal++;
        if (p.tGame.time_since_epoch().count())
            _timing.dxgiLatency.push_back(std::chrono::duration<float, std::milli>(now - p.tGame).count());
    }
}

void DisplayFg::NoteGamePresent(uint64_t fgFrameId, std::chrono::steady_clock::time_point t)
{
    std::lock_guard g(_tm);
    const UINT k = UINT(fgFrameId % kTimes);
    _gameId[k] = fgFrameId;
    _gameT[k] = t;
}

DisplayFg::Timing DisplayFg::TakeTiming()
{
    std::lock_guard g(_tm);
    Timing t = std::move(_timing);
    _timing = {};
    return t;
}

void DisplayFg::Frame(ID3D12Device* dev, ID3D12CommandQueue* q, IDXGISwapChain4* sc, const FgFrame* in, bool enabled)
{
    if (!_sc)
        return;
    DXGI_SWAP_CHAIN_DESC1 d {};
    sc->GetDesc1(&d);
    const bool canRun = enabled && in && in->depth && in->motion;
    if (!canRun)
    {
        if (_fg && _lastEnabled)
        {
            ffxConfigureDescFrameGeneration off {};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = sc;
            off.frameGenerationEnabled = false;
            off.frameID = ++_frameId;
            Ffx().configure(&_fg, &off.header);
            Log(0, "display FG: off");
        }
        _lastEnabled = false;
        std::lock_guard g(_tm);
        if (_pendingFgMode)
            _pending.clear(), _pendingFgMode = false;
        if (_pending.size() < 16)
            _pending.push_back({ false, in ? in->tGame : std::chrono::steady_clock::time_point {} });
        return;
    }
    const auto& gd = in->g;
    uint32_t flags = 0;
    if (gd.depthInverted)
        flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (CurrentSettings().fgInfiniteDepth)
        flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (gd.jitteredMv)
        flags |= FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (gd.motionW > gd.renderW || gd.motionH > gd.renderH)
        flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    if (CurrentSettings().fgAsync)
        flags |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
        flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE * (CurrentSettings().fgHdr ? 1 : 0);
    const DXGI_FORMAT hudFmt = in->hudMode ? d.Format : DXGI_FORMAT_UNKNOWN;
    if (!EnsureContext(dev, d.Width, d.Height, d.Format, flags, hudFmt))
        return;

    const uint64_t id = ++_frameId;
    NoteGamePresent(id, in->tGame);
    const UINT s = UINT(_counter % 4);
    // the allocator is reset next: never while its list may still run
    if (!GpuWaitDone(_fence.Get(), _fenceValue[s]))
    {
        Log(2, "display FG: prepare list still running after 60 s: FG off this frame");
        ffxConfigureDescFrameGeneration off {};
        off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.swapChain = sc;
        off.frameGenerationEnabled = false;
        off.frameID = id;
        Ffx().configure(&_fg, &off.header);
        _lastEnabled = false;
        return;
    }
    // FSR FG 4.0.2 requires configure, then prepare, then the generation dispatch (FFX's present)
    const bool docOrder = LiveFgOrder() != 0;
    ffxConfigureDescFrameGeneration cf {};
    cf.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cf.swapChain = sc;
    cf.presentCallback = [](ffxCallbackDescFrameGenerationPresent* p, void* self) -> ffxReturnCode_t
    { return static_cast<DisplayFg*>(self)->OnPresent(p); };
    cf.presentCallbackUserContext = this;
    cf.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* p, void* self) -> ffxReturnCode_t
    { return static_cast<DisplayFg*>(self)->OnGenerate(p); };
    cf.frameGenerationCallbackUserContext = this;
    cf.allowAsyncWorkloads = CurrentSettings().fgAsync;
    if (in->hudless)
        cf.HUDLessColor = ffxApiGetResourceDX12(in->hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    cf.flags = CurrentSettings().fgDebug ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES : 0;
    cf.onlyPresentGenerated = false;
    cf.generationRect = { 0, 0, (int) d.Width, (int) d.Height };
    cf.frameID = id;
    uint32_t rc = FFX_API_RETURN_OK;
    if (docOrder)
    {
        cf.frameGenerationEnabled = true;
        rc = Ffx().configure(&_fg, &cf.header);
    }

    _alloc[s]->Reset();
    _list->Reset(_alloc[s].Get(), nullptr);
    ffxDispatchDescFrameGenerationPrepare pr {};
    pr.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    pr.frameID = id;
    pr.flags = 0;
    pr.commandList = _list.Get();
    const auto dd = in->depth->GetDesc();
    pr.renderSize = { gd.renderW ? gd.renderW : (uint32_t) dd.Width, gd.renderH ? gd.renderH : dd.Height };
    pr.jitterOffset = { gd.jitterX, gd.jitterY };
    pr.motionVectorScale = { gd.mvScaleX, gd.mvScaleY };
    pr.frameTimeDelta = (float) in->frameMs;
    pr.cameraNear = CurrentSettings().fgCameraNear;
    pr.cameraFar = CurrentSettings().fgCameraFar;
    pr.cameraFovAngleVertical = CurrentSettings().fgVFov;
    pr.viewSpaceToMetersFactor = 1.0f;
    pr.depth = ffxApiGetResourceDX12(in->depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    pr.motionVectors = ffxApiGetResourceDX12(in->motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    uint32_t r = Ffx().dispatch(&_fg, &pr.header);
    _list->Close();
    if (r == FFX_API_RETURN_OK)
    {
        ID3D12CommandList* l[] = { _list.Get() };
        q->ExecuteCommandLists(1, l);
    }
    _fenceValue[s] = ++_counter;
    q->Signal(_fence.Get(), _fenceValue[s]);

    if (!docOrder)
    {
        cf.frameGenerationEnabled = r == FFX_API_RETURN_OK;
        rc = Ffx().configure(&_fg, &cf.header);
    }
    const bool on = r == FFX_API_RETURN_OK && rc == FFX_API_RETURN_OK;
    if (on != _lastEnabled || (!on && !_loggedFail))
    {
        const auto md = in->motion->GetDesc();
        Log(on ? 0 : 2, std::format("display FG: {} (prepare {}, configure {}, {} order), render {}x{}, depth {}x{} fmt {}, "
                                    "motion {}x{} fmt {} scale {:.3f},{:.3f} jitter {:.3f},{:.3f}, motion size {}x{}, "
                                    "inverted {}, jittered mv {}, hudless {}",
                                    on ? "on" : "failed", r, rc, docOrder ? "4.0.2" : "prepare-first",
                                    pr.renderSize.width, pr.renderSize.height, dd.Width, dd.Height, (int) dd.Format,
                                    md.Width, md.Height, (int) md.Format, gd.mvScaleX, gd.mvScaleY, gd.jitterX, gd.jitterY,
                                    gd.motionW, gd.motionH, gd.depthInverted, gd.jitteredMv, in->hudless != nullptr));
        if (!on)
            _loggedFail = true;
    }
    _lastEnabled = on;
}

// FFX's generation dispatch, on FFX's thread
uint32_t DisplayFg::OnGenerate(void* params)
{
    auto* p = static_cast<ffxDispatchDescFrameGeneration*>(params);
    return Ffx().dispatch(&_fg, &p->header);
}

void DisplayFg::FlushDumps(ID3D12Device* dev, ID3D12CommandQueue* q, const std::wstring& dir)
{
    const UINT n = _dumpCount.load();
    if (!n || _dumpArm.load())
        return;
    WaitForPresents();
    for (UINT k = 0; k < n; ++k)
        if (_dump[k])
            DumpTexture(dev, q, _dump[k].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        dir + std::format(L"\\fg{:06}_{}", _dumpId[k], _dumpGen[k] ? L"gen" : L"real"));
    Log(0, std::format("display FG: {} presented frames dumped", n));
    _dumpCount = 0;
}

bool PollLive(const std::wstring& dir)
{
    if (dir.empty())
        return false;
    FILE* f = _wfopen((dir + L"\\mgpu_live.txt").c_str(), L"r");
    if (!f)
        return false;
    char line[64];
    int fg = s_liveFg, nr = s_liveNr, fh = s_liveFgHud, fs = s_liveFgSyn, fo = s_liveFgOrder;
    while (fgets(line, sizeof(line), f))
    {
        int v = 0;
        if (sscanf(line, "fghud=%d", &v) == 1)
            fh = v;
        else if (sscanf(line, "fgsyn=%d", &v) == 1)
            fs = v;
        else if (sscanf(line, "fgorder=%d", &v) == 1)
            fo = v;
        else if (sscanf(line, "fg=%d", &v) == 1)
            fg = v;
        else if (sscanf(line, "nr=%d", &v) == 1)
            nr = v;
    }
    fclose(f);
    const bool changed = fg != s_liveFg || nr != s_liveNr || fh != s_liveFgHud || fs != s_liveFgSyn || fo != s_liveFgOrder;
    if (changed)
        Log(0, std::format("live: fg {} nr {} fghud {} fgsyn {} fgorder {}", fg, nr, fh, fs, fo));
    s_liveFg = fg;
    s_liveNr = nr;
    s_liveFgHud = fh;
    s_liveFgSyn = fs;
    s_liveFgOrder = fo;
    return changed;
}
int LiveFg() { return s_liveFg; }
int LiveFgHud() { return s_liveFgHud; }
int LiveFgSyn() { return s_liveFgSyn; }
int LiveFgOrder() { return s_liveFgOrder; }
int LiveNr() { return s_liveNr; }
void SetGameFg(bool on)
{
    if (s_gameFg.exchange(on) != on)
        Log(1, on ? "the game's own frame generation is on: display FG off" : "the game's frame generation is off");
}
bool GameFg() { return s_gameFg; }
} // namespace Mgpu
