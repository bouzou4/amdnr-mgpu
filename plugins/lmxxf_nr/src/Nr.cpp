// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
//
// Per frame (one job in flight, lmxxf's documented call order spread over two frames):
//   frame N, in the game's list:  [consume job N-1: RecordOutputs, Retire, apply its edit to frame N]
//                                 [feed frame N: downsample, PrepareFrame, RecordInputs]
//   when that list is executed:   game queue waits for the network of N-1 before it, then signals; the session
//                                 queue waits for that signal and runs EnqueueHip for job N.
// The network's edit (its output minus what it was fed) lands one frame late and is moved by the motion vectors.
#include "Nr.h"

#include <d3dcompiler.h>

#include "mgpu/MgpuCore.h"
#include "GpuWait.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;

namespace
{
constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kHeapSlots = 64;
constexpr uint32_t kSlotDescriptors = 6;

DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    default:
        return f;
    }
}

// UAVs can't be sRGB
DXGI_FORMAT UavFormat(DXGI_FORMAT f)
{
    f = ViewFormat(f);
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (res == nullptr || from == to)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    list->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav,
                               D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    if (state & D3D12_RESOURCE_STATE_RENDER_TARGET)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

float Median(const float* samples, uint32_t count)
{
    const uint32_t n = std::min<uint32_t>(count, 61);
    if (n == 0)
        return 0.0f;
    float tmp[61];
    std::copy(samples, samples + n, tmp);
    std::nth_element(tmp, tmp + n / 2, tmp + n);
    return tmp[n / 2];
}

// A state shaders can read in, or the one to move to for our passes
bool Readable(D3D12_RESOURCE_STATES s)
{
    return (s & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) != 0 &&
           (s & ~(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                  D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)) == 0;
}

bool IsExposureFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R16_TYPELESS ||
           f == DXGI_FORMAT_R32_TYPELESS;
}
} // namespace

// Never destroyed: at process exit a joinable worker would terminate, and D3D12 / HIP teardown in DLL detach is unsafe
NrEngine& Engine()
{
    static NrEngine* engine = new NrEngine();
    return *engine;
}

NrEngine& DisplayEngine()
{
    static NrEngine* engine = new NrEngine();
    return *engine;
}

bool NrEngine::LoadRuntime(const RuntimePaths& paths)
{
    std::lock_guard lock(_mutex);
    if (_rt.Load(paths))
    {
        Log(OptiLog_Info,
            "runtime %ls (%s, from %s, assets %ls, %zu modules hashed, %zu exports, %s table %u bytes, max input %ux%u, "
            "hip_ready %u, history %u, native %d); env: free_res %d, style %d, multi_pass %d",
            _rt.dllPath.c_str(), _rt.LayoutName(), _rt.source.c_str(), _rt.assets.c_str(), _rt.hashed, _rt.exports,
            _rt.hasTimings ? "full" : "v1",
            _rt.tableSize, _rt.caps.max_input_width, _rt.caps.max_input_height, _rt.caps.hip_ready,
            _rt.caps.history_supported, _rt.Native(), _rt.envFreeRes, _rt.envStyle, _rt.envMultiPass);
        for (const auto& s : _rt.searched)
            Log(OptiLog_Info, "  skipped %s", s.c_str());
        return true;
    }
    _stats.state = "no runtime";
    _stats.error = _rt.error;
    Log(OptiLog_Warn, "%s", _rt.error.c_str());
    for (const auto& s : _rt.searched)
        Log(OptiLog_Warn, "  searched %s", s.c_str());
    return false;
}

void NrEngine::Configure(const NrSettings& s)
{
    std::lock_guard lock(_mutex);
    _settings = s;
    // A history session drops adaptive ViT reuse: toggling history rebuilds it off the render thread
    _dirty = s.size != _active.size || s.model != _active.model || s.Style() != _active.Style() ||
             s.Passes() != _active.Passes() || (_rt.Native() && s.History() != _active.History());
}

NrSettings NrEngine::Settings()
{
    std::lock_guard lock(_mutex);
    return _settings;
}

NrStats NrEngine::Stats()
{
    std::unique_lock lock(_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return _stats; // a session is being prepared, the copy may be a frame old
    NrStats s = _stats;
    s.hipMs = Median(_hipSamples, _hipCount);
    s.networkMs = Median(_netSamples, _netCount);
    s.frameInfoSize = _frameInfoSize;
    auto mean = [](const float* v, uint32_t n)
    {
        const uint32_t k = std::min(n, kMeterWindow);
        double sum = 0;
        for (uint32_t i = 0; i < k; i++)
            sum += v[i];
        return k ? float(sum / k) : NAN;
    };
    s.flkStatic = mean(_flkStatic, _flkStaticN);
    s.flkMc = mean(_flkMc, _flkMcN);
    s.flkFrames = std::min(_flkStaticN, kMeterWindow);
    if (_ctx != nullptr && _rt.api.GetStatus != nullptr && _state == State::Ready)
    {
        char buf[512] = {};
        _rt.api.GetStatus(_ctx, buf, sizeof(buf));
        s.status = buf;
    }
    return s;
}

void NrEngine::Fail(const std::string& why)
{
    _stats.state = "failed";
    _stats.error = why;
    _state = State::Failed;
    Log(OptiLog_Error, "%s", why.c_str());
}

void NrEngine::FeedSize(uint32_t w, uint32_t h, uint32_t* fw, uint32_t* fh)
{
    uint32_t boxW = 1920, boxH = 1080;
    if (_rt.envFreeRes && _settings.size == NetworkSize::Native1440)
        boxW = 2560, boxH = 1440;
    else if (_rt.envFreeRes && _settings.size == NetworkSize::Native)
        boxW = 3840, boxH = 2176;
    const double s = std::min({ 1.0, double(boxW) / w, double(boxH) / h });
    *fw = std::max<uint32_t>(64, uint32_t(w * s + 0.5) & ~1u);
    *fh = std::max<uint32_t>(64, uint32_t(h * s + 0.5) & ~1u);
}

void NrEngine::ApplyEnv()
{
    // Read by the runtime when it builds its network, the session is recreated after a change
    if (_rt.envFreeRes)
        SetRuntimeEnv("DLSS5_NETWORK_FREE_RES", _settings.size == NetworkSize::Tier1080 ? "0" : "1");
    if (_rt.envStyle)
    {
        const char* styles[] = { "0", "1", "2" };
        SetRuntimeEnv("DLSS5_STYLE", styles[std::clamp(_settings.Style(), 0, 2)]);
    }
    if (_rt.envMultiPass)
    {
        const char* passes[] = { "1", "2", "3" };
        SetRuntimeEnv("DLSS5_MULTI_PASS", _settings.model == 1 ? "0.5" : passes[std::clamp(_settings.Passes(), 1, 3) - 1]);
    }
}

bool NrEngine::InitDevice(ID3D12Device* device, ID3D12CommandQueue* gameQueue)
{
    _device = device;
    _gameQueue = gameQueue;

    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&_queue))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_gameFence))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_hipFence))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_ownAlloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _ownAlloc.Get(), nullptr,
                                         IID_PPV_ARGS(&_ownList))))
        return false;
    _queue->SetName(L"lmxxf_nr session queue");
    _ownList->Close();
    _event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Timestamps around EnqueueHip, resolved into a persistently mapped readback buffer
    D3D12_QUERY_HEAP_DESC qh { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimingSlots * 2 };
    D3D12_HEAP_PROPERTIES rb { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kTimingSlots * 16;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&_queryHeap))) &&
        SUCCEEDED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&_readback))))
    {
        UINT64 freq = 0;
        _queue->GetTimestampFrequency(&freq);
        _tsFreq = double(freq);
        for (uint32_t i = 0; i < kTimingSlots * 2; i++)
        {
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_tsAlloc[i]));
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _tsAlloc[i].Get(), nullptr,
                                      IID_PPV_ARGS(&_tsList[i]));
            _tsList[i]->EndQuery(_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
            if (i & 1)
                _tsList[i]->ResolveQueryData(_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i - 1, 2, _readback.Get(),
                                             (i / 2) * 16);
            _tsList[i]->Close();
        }
    }

    return CreatePipelines();
}

bool NrEngine::CreatePipelines()
{
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (compile == nullptr)
    {
        Fail("d3dcompiler_47.dll not available");
        return false;
    }

    ComPtr<ID3DBlob> down, apply, terminal, stats, err;
    if (FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "Down", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &down, &err)) ||
        FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "Apply", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &apply, &err)) ||
        FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "ApplyTerminal",
                       "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &terminal, &err)) ||
        FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "Stats", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &stats, &err)))
    {
        Fail(std::string("shader compile failed: ") + (err ? (const char*) err->GetBufferPointer() : "?"));
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[3] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, sizeof(ShaderConstants) / 4 };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &ranges[0] };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = { 1, &ranges[1] };
    D3D12_STATIC_SAMPLER_DESC samplers[2] {};
    for (int i = 0; i < 2; i++)
    {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC rd { 3, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&_rootSig))))
    {
        Fail("root signature creation failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = _rootSig.Get();
    pd.CS = { down->GetBufferPointer(), down->GetBufferSize() };
    if (FAILED(_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&_psoDown))))
        return Fail("pipeline creation failed"), false;
    pd.CS = { apply->GetBufferPointer(), apply->GetBufferSize() };
    if (FAILED(_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&_psoApply))))
        return Fail("pipeline creation failed"), false;
    pd.CS = { terminal->GetBufferPointer(), terminal->GetBufferSize() };
    if (FAILED(_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&_psoTerminal))))
        return Fail("pipeline creation failed"), false;
    pd.CS = { stats->GetBufferPointer(), stats->GetBufferSize() };
    if (FAILED(_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&_psoStats))))
        return Fail("pipeline creation failed"), false;
    ComPtr<ID3DBlob> meter;
    if (SUCCEEDED(compile(kMeterSource, sizeof(kMeterSource) - 1, "lmxxf_nr_meter", nullptr, nullptr, "Meter", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &meter, &err)))
    {
        pd.CS = { meter->GetBufferPointer(), meter->GetBufferSize() };
        _device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&_psoMeter));
    }
    if (!_psoMeter)
        Log(OptiLog_Warn, "flicker meter shader failed: %s", err ? (const char*) err->GetBufferPointer() : "?");

    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSlots * kSlotDescriptors,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    if (FAILED(_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
        return Fail("descriptor heap creation failed"), false;
    _heapStride = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void NrEngine::Dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const ShaderConstants& c,
                        ID3D12Resource* const srv[4], const DXGI_FORMAT srvFmt[4], ID3D12Resource* uav,
                        DXGI_FORMAT uavFmt, uint32_t w, uint32_t h, ID3D12Resource* norm)
{
    // A ring of descriptor slots, the GPU is never kHeapSlots dispatches behind
    const uint32_t slot = (_heapCursor++ % kHeapSlots) * kSlotDescriptors;
    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * _heapStride;
    gpu.ptr += UINT64(slot) * _heapStride;

    for (int i = 0; i < 4; i++)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = srv[i] ? ViewFormat(srvFmt[i]) : DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h2 { cpu.ptr + SIZE_T(i) * _heapStride };
        _device->CreateShaderResourceView(srv[i], &sd, h2);
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(norm, &sd, { cpu.ptr + SIZE_T(4) * _heapStride });
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud {};
    ud.Format = UavFormat(uavFmt);
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(uav, nullptr, &ud, { cpu.ptr + SIZE_T(5) * _heapStride });

    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_rootSig.Get());
    list->SetPipelineState(pso);
    list->SetComputeRoot32BitConstants(0, sizeof(ShaderConstants) / 4, &c, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, { gpu.ptr + UINT64(5) * _heapStride });
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

bool NrEngine::CreateSession()
{
    LmxxfNrCreateInfo ci {};
    ci.struct_size = sizeof(ci);
    ci.device = _device.Get();
    ci.queue = _queue.Get();
    ci.assets_directory = _rt.assets.c_str();
    ci.flags = 0;
    int32_t rc = _rt.api.Create(&ci, &_ctx);
    if (rc != LMXXF_NR_OK || _ctx == nullptr)
        return Fail("Create failed: " + std::to_string(rc) + " " + _rt.LastError()), false;
    rc = _rt.api.PrepareSession != nullptr ? _rt.api.PrepareSession(_ctx) : LMXXF_NR_OK;
    if (rc != LMXXF_NR_OK)
        return Fail("PrepareSession failed: " + std::to_string(rc) + " " + _rt.LastError()), false;
    _frameInfoSize = 0;
    _sessionFull = false;
    _mpHistoryRefused = false;
    return true;
}

// The runtime keeps the pass count of the last FULL_NETWORK frame process-wide (dlss5_mode::Request)
static bool g_fullSent = false;

bool NrEngine::PrepareJob(ID3D12GraphicsCommandList* list, const OptiUpscaleFrame* frame)
{
    LmxxfNrFrameInfo fi {};
    fi.session_id = 1;
    fi.frame_id = ++_frameId;
    fi.list_generation = _frameId;
    fi.command_list = list;
    fi.color_width = _feedW;
    fi.color_height = _feedH;
    fi.color = _feed.Get();
    fi.color_state = kRead;
    // FULL_NETWORK pins the runtime to one Full pass; 2-3 passes are DLSS5_MULTI_PASS without the flag
    const bool full = _rt.Native() && ((_settings.model == 0 && _settings.Passes() == 1) || _forceFull);
    fi.flags = full ? LMXXF_NR_FRAME_FLAG_FULL_NETWORK_EXT : 0;
    // a FULL frame then one without it in the same session releases the pin
    if (full)
        g_fullSent = _sessionFull = true;
    else if (_sessionFull)
        g_fullSent = _sessionFull = false;
    fi.transfer_strength = 1.0f;
    fi.color_strength = 1.0f;
    fi.model_scale = 1.0f;
    fi.pre_exposure = 1.0f;
    fi.exposure_scale = 1.0f;
    if (frame != nullptr && _settings.exposure && frame->exposure != nullptr && !_feedNormalised)
    {
        auto* exp = static_cast<ID3D12Resource*>(frame->exposure);
        const auto d = exp->GetDesc();
        if (d.Width == 1 && d.Height == 1 && IsExposureFormat(d.Format))
        {
            fi.exposure = exp;
            fi.exposure_state = frame->exposureState;
            fi.pre_exposure = frame->preExposure > 0.0f ? frame->preExposure : 1.0f;
        }
    }

    _job = {};
    _job.struct_size = sizeof(_job);

    // Motion-fed history: the temporal layout, sizes with and without the exposure tail
    _stats.temporal = false;
    if (_settings.History() && _rt.temporal != 0 && _motion.res != nullptr && !(_settings.Passes() > 1 && _mpHistoryRefused))
    {
        LmxxfNrFrameInfoTemporalExt t {};
        memcpy(&t, &fi, LMXXF_NR_FRAME_INFO_V1_SIZE);
        t.flags |= LMXXF_NR_FRAME_FLAG_TEMPORAL_EXT;
        t.motion = _motion.res;
        t.motion_state = _motion.state;
        t.motion_width = _motion.w;
        t.motion_height = _motion.h;
        t.motion_scale_x = _motion.scaleX;
        t.motion_scale_y = _motion.scaleY;
        t.history_reset = (_motion.reset || _extentReset) ? 1 : 0;
        if (t.history_reset)
            _resetFrame = _frameId;
        _extentReset = false;
        t.passes = (uint32_t) _settings.Passes();
        t.exposure = fi.exposure;
        t.exposure_state = fi.exposure_state;
        t.pre_exposure = fi.pre_exposure;
        t.exposure_scale = fi.exposure_scale;
        for (uint32_t size : { (uint32_t) sizeof(t), 120u })
        {
            if ((_rt.temporalSize != 0 && size != _rt.temporalSize) || (_rt.Native() && size != sizeof(t)))
                continue;
            t.struct_size = size;
            const int32_t trc = _rt.api.PrepareFrame(_ctx, reinterpret_cast<const LmxxfNrFrameInfo*>(&t), &_job);
            if (trc == LMXXF_NR_OK)
            {
                if (_rt.temporal != 1)
                    Log(OptiLog_Info, "temporal frame info %u (motion-fed history)", size);
                _rt.temporal = 1;
                _rt.temporalSize = size;
                _stats.temporal = true;
                const int32_t r2 = _rt.api.RecordInputs(_ctx, _job.handle, list);
                if (r2 == LMXXF_NR_OK)
                    return true;
                _stats.error = "RecordInputs " + std::to_string(r2) + ": " + _rt.LastError();
                _rt.api.CancelUnsubmitted(_ctx, _job.handle);
                return false;
            }
            if (_rt.temporal == 1)
                break;
        }
        if (_settings.Passes() > 1)
        {
            // an older runtime takes history with one pass only: this session runs without it
            if (!_mpHistoryRefused)
                Log(OptiLog_Info, "runtime takes no motion-fed history with %d passes (%s): single-frame NR",
                    _settings.Passes(), _rt.LastError().c_str());
            _mpHistoryRefused = true;
        }
        else if (_rt.temporal != 1)
        {
            Log(OptiLog_Info, "runtime takes no motion-fed history (%s): single-frame NR", _rt.LastError().c_str());
            _rt.temporal = 0;
        }
        _job = {};
        _job.struct_size = sizeof(_job);
    }

    // The newest frame info size the runtime takes. A modified runtime (more exports than lmxxf's) gets the
    // 64 byte one, whose fields every lmxxf derived runtime reads the same way.
    const uint32_t first = _rt.exports == 1 ? 0 : 2;
    int32_t rc = LMXXF_NR_INVALID_ARGUMENT;
    for (uint32_t i = first; i < std::size(Runtime::kFrameInfoSizes); i++)
    {
        const uint32_t size = Runtime::kFrameInfoSizes[i];
        if (_frameInfoSize != 0 && size != _frameInfoSize)
            continue;
        fi.struct_size = size;
        rc = _rt.api.PrepareFrame(_ctx, &fi, &_job);
        if (rc == LMXXF_NR_OK)
        {
            if (_frameInfoSize == 0)
                Log(OptiLog_Info, "frame info size %u", size);
            _frameInfoSize = size;
            break;
        }
        if (rc != LMXXF_NR_INVALID_ARGUMENT || _rt.LastError().find("struct_size") == std::string::npos)
            break;
    }
    if (rc != LMXXF_NR_OK)
    {
        _stats.error = "PrepareFrame " + std::to_string(rc) + ": " + _rt.LastError();
        return false;
    }
    rc = _rt.api.RecordInputs(_ctx, _job.handle, list);
    if (rc != LMXXF_NR_OK)
    {
        _stats.error = "RecordInputs " + std::to_string(rc) + ": " + _rt.LastError();
        _rt.api.CancelUnsubmitted(_ctx, _job.handle);
        return false;
    }
    return true;
}

void NrEngine::WaitIdle()
{
    auto wait = [this](ID3D12CommandQueue* q, ID3D12Fence* f, uint64_t& v)
    {
        if (q == nullptr)
            return;
        q->Signal(f, ++v);
        GpuWaitDone(f, v); // before the session's resources go: until done or the device is gone
    };
    wait(_gameQueue.Get(), _gameFence.Get(), _gameValue);
    wait(_queue.Get(), _hipFence.Get(), _hipValue);
}

void NrEngine::DrainJob()
{
    if (_jobState == Job::Enqueued)
    {
        _ownAlloc->Reset();
        _ownList->Reset(_ownAlloc.Get(), nullptr);
        _rt.api.RecordOutputs(_ctx, _job.handle, _ownList.Get());
        _ownList->Close();
        ID3D12CommandList* l[] = { _ownList.Get() };
        o_ExecuteCommandLists(_queue.Get(), 1, l);
        _rt.api.Retire(_ctx, _job.handle);
    }
    else if (_jobState == Job::Recorded)
    {
        _rt.api.CancelUnsubmitted(_ctx, _job.handle);
    }
    _jobState = Job::None;
    _feedValid = false;
}

// Builds the network for the feed size off the render thread: one job on the session queue
bool NrEngine::WarmUp()
{
    const DWORD t0 = GetTickCount();
    // A pin from an earlier Full session is released only by a FULL frame followed by one without it
    if (_rt.Native() && g_fullSent && !(_settings.model == 0 && _settings.Passes() == 1) && !_forceFull)
    {
        _forceFull = true;
        const bool ok = WarmUp();
        _forceFull = false;
        if (!ok)
            return false;
    }
    _ownAlloc->Reset();
    _ownList->Reset(_ownAlloc.Get(), nullptr);
    _motion = {};
    if (_settings.History() && _rt.Native() && !_forceFull)
    {
        // Builds the history network now rather than at the first frame with vectors (values unused on a reset)
        _motion.res = _feed.Get();
        _motion.w = _feedW;
        _motion.h = _feedH;
        _motion.reset = true;
    }
    _lastFedUpscale = 0;
    if (!PrepareJob(_ownList.Get(), nullptr))
    {
        _ownList->Close();
        return Fail("warm-up: " + _stats.error), false;
    }
    _ownList->Close();
    ID3D12CommandList* l[] = { _ownList.Get() };
    o_ExecuteCommandLists(_queue.Get(), 1, l);
    int32_t rc = _rt.api.EnqueueHip(_ctx, _job.handle, _queue.Get());
    if (rc != LMXXF_NR_OK)
        return Fail("warm-up EnqueueHip " + std::to_string(rc) + ": " + _rt.LastError()), false;
    _jobState = Job::Enqueued;
    WaitIdle();
    DrainJob();
    WaitIdle();
    _motion = {};
    Log(OptiLog_Info, "network ready for %ux%u in %lu ms (%s, %d pass(es), history %d)%s", _feedW, _feedH,
        GetTickCount() - t0, _rt.Native() ? (_settings.model == 0 || _forceFull ? "Full" : "Small") : "runtime's model",
        _forceFull ? 1 : _settings.Passes(), (int) _stats.temporal, _forceFull ? " [model pin reset]" : "");
    return true;
}

void NrEngine::Worker(uint32_t feedW, uint32_t feedH, bool recreate)
{
    // The game's list holding our last producer must be submitted first, its EnqueueHip needs the session
    for (;;)
    {
        for (int i = 0; i < 2000 && _pendingList.load() != nullptr; i++)
            Sleep(1);
        _mutex.lock();
        if (_pendingList.load() == nullptr)
            break;
        _mutex.unlock();
    }

    _stats.state = "preparing";
    if (_ctx != nullptr)
    {
        DrainJob();
        WaitIdle();
    }
    if (recreate && _ctx != nullptr)
    {
        _rt.api.Drain(_ctx);
        _rt.api.Destroy(_ctx);
        _ctx = nullptr;
    }

    bool ok = true;
    if (_ctx == nullptr)
    {
        ApplyEnv();
        ok = CreateSession();
    }
    if (ok && (feedW != _feedW || feedH != _feedH || !_feed))
    {
        _feed = Texture(_device.Get(), feedW, feedH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, kRead);
        _feedW = feedW;
        _feedH = feedH;
        ok = _feed != nullptr || (Fail("feed texture creation failed"), false);
    }
    if (ok)
        ok = WarmUp();
    if (ok)
    {
        _active = _settings;
        _dirty = false;
        _failures = 0;
        _stats.state = "running";
        _stats.error.clear();
        _stats.feedWidth = _feedW;
        _stats.feedHeight = _feedH;
        _readyFrame = _frameId;
        _meterPrevValid = false;
        _state = State::Ready;
    }
    _mutex.unlock();
}

void NrEngine::ReadTimings()
{
    FinishDump();
    SeqPoll();
    MeterRead();
    if (_readback && _tsFreq > 0)
    {
        const uint64_t done = _hipFence->GetCompletedValue();
        for (uint32_t i = 0; i < kTimingSlots; i++)
        {
            if (_tsValue[i] == 0 || _tsValue[i] > done)
                continue;
            uint64_t* t = nullptr;
            D3D12_RANGE r { i * 16, i * 16 + 16 };
            if (SUCCEEDED(_readback->Map(0, &r, (void**) &t)))
            {
                const uint64_t* p = t + i * 2;
                if (p[1] > p[0])
                    _hipSamples[_hipCount++ % 61] = float(double(p[1] - p[0]) * 1000.0 / _tsFreq);
                D3D12_RANGE none { 0, 0 };
                _readback->Unmap(0, &none);
            }
            _tsValue[i] = 0;
        }
    }

    if (_rt.hasTimings && _ctx != nullptr)
    {
        LmxxfNrTimings t {};
        t.struct_size = sizeof(t);
        if (_rt.api.GetTimings(_ctx, &t) == LMXXF_NR_OK && t.valid && t.frame_id != _lastTimed)
        {
            _lastTimed = t.frame_id;
            _netSamples[_netCount++ % 61] = t.network_ms;
        }
    }
}

bool NrEngine::Begin(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t srcW, uint32_t srcH)
{
    const State state = _state;
    if (state == State::Failed)
        return false;
    if (state == State::Preparing)
    {
        _stats.framesSkipped++;
        return false;
    }

    uint32_t feedW, feedH;
    bool dirty;
    {
        std::lock_guard lock(_mutex);
        FeedSize(srcW, srcH, &feedW, &feedH);
        dirty = _dirty;
        if (srcW != _histSrcW || srcH != _histSrcH)
        {
            if (_histSrcW != 0)
            {
                _extentReset = true;
                Log(OptiLog_Info, "NR (%ls): source %ux%u -> %ux%u, history reset", name.c_str(), _histSrcW, _histSrcH,
                    srcW, srcH);
            }
            _histSrcW = srcW;
            _histSrcH = srcH;
        }
    }

    if (state == State::Idle)
    {
        std::lock_guard lock(_mutex);
        if (!InitDevice(device, queue))
        {
            if (_state != State::Failed)
                Fail("device setup failed");
            return false;
        }
        extern bool InstallQueueHook(ID3D12Device * device);
        if (!InstallQueueHook(_device.Get()))
            return Fail("ExecuteCommandLists hook failed"), false;
        _state = State::Preparing;
        _worker = std::thread(&NrEngine::Worker, this, feedW, feedH, false);
        return false;
    }

    // Ready: a new network size or new runtime settings rebuild off the render thread
    if (dirty || feedW != _feedW || feedH != _feedH)
    {
        {
            std::lock_guard lock(_mutex);
            SeqRestart("the network is being rebuilt");
        }
        _state = State::Preparing;
        if (_worker.joinable())
            _worker.join();
        _worker = std::thread(&NrEngine::Worker, this, feedW, feedH, dirty);
        _stats.framesSkipped++;
        return false;
    }
    return true;
}

void NrEngine::OnUpscale(OptiUpscaleFrame* f, bool after)
{
    if (f->api != OptiApi_D3D12 || f->device == nullptr || f->commandList == nullptr || !RuntimeLoaded())
        return;

    const uint32_t gameApi =
        f->size >= offsetof(OptiUpscaleFrame, gameApi) + sizeof(f->gameApi) ? f->gameApi : uint32_t(OptiApi_D3D12);
    if (gameApi != _stats.gameApi)
    {
        std::lock_guard lock(_mutex);
        _stats.gameApi = gameApi;
        Log(OptiLog_Info, "upscale callbacks: game API %u%s", gameApi,
            (f->flags & OptiUpscale_Bridged) ? ", through OptiScaler's D3D12 bridge" : "");
    }

    NrSettings s;
    {
        std::lock_guard lock(_mutex);
        s = _settings;
    }
    if (!s.enabled || s.placement == Placement::Terminal)
        return;
    const bool pre = s.placement == Placement::BeforeUpscale;
    if (after == pre)
        return;

    auto* src = static_cast<ID3D12Resource*>(pre ? f->color : f->output);
    if (src == nullptr)
        return;
    const uint32_t srcW = pre ? f->renderWidth : f->outputWidth;
    const uint32_t srcH = pre ? f->renderHeight : f->outputHeight;
    if (srcW < 64 || srcH < 64)
        return;

    auto* list = static_cast<ID3D12GraphicsCommandList*>(f->commandList);
    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;

    if (!Begin(static_cast<ID3D12Device*>(f->device), static_cast<ID3D12CommandQueue*>(f->commandQueue), srcW, srcH))
        return SeqIdle(_state == State::Failed ? "NR failed" : "the network is being built");

    std::lock_guard lock(_mutex);
    if (_state != State::Ready)
        return;
    _srcW = srcW;
    _srcH = srcH;
    _stats.srcWidth = srcW;
    _stats.srcHeight = srcH;
    Record(f, pre);
}

// The previous job's answer: RecordOutputs into list. *enh is its output when it can be applied this frame.
void NrEngine::Consume(ID3D12GraphicsCommandList* list, uint64_t* wait, ID3D12Resource** enh)
{
    *wait = 0;
    *enh = nullptr;
    if (_jobState == Job::Enqueued)
    {
        const int32_t rc = _rt.api.RecordOutputs(_ctx, _job.handle, list);
        if (rc == LMXXF_NR_OK)
        {
            _rt.api.Retire(_ctx, _job.handle);
            *wait = _jobHipValue;
            if (_feedValid)
                *enh = static_cast<ID3D12Resource*>(_job.private_output);
            if (*enh != nullptr && _jobSeq >= 0 && _jobSeq < (int32_t) _seq.size())
            {
                // Read on this list after the wait for the network (see OnExecute / SubmitOwn)
                SeqCopy(list, *enh, kRead, _seq[_jobSeq].enh);
                _seqWait = 0;
            }
        }
        else
        {
            _stats.error = "RecordOutputs " + std::to_string(rc) + ": " + _rt.LastError();
        }
    }
    else if (_jobState == Job::Recorded)
    {
        // Its list was never submitted (recorded again before the last one was executed)
        _rt.api.CancelUnsubmitted(_ctx, _job.handle);
    }
    _jobState = Job::None;
}

// This frame's job: PrepareFrame + RecordInputs on _feed, submitted with list
void NrEngine::Produce(ID3D12GraphicsCommandList* list, const OptiUpscaleFrame* frame, uint64_t wait)
{
    const bool produced = PrepareJob(list, frame);
    _jobSeq = -1;
    if (produced && _seqActive && _seqNext > 0 && _resetFrame == _frameId)
        SeqRestart("history reset");
    _jobMotion = _motion;
    _jobReset = _motion.reset || _resetFrame == _frameId;
    _lastFedUpscale = produced && _motion.res != nullptr ? _motionUpscale : 0;
    if (produced)
    {
        _jobState = Job::Recorded;
        _feedValid = true;
        _stats.framesRun++;
        _failures = 0;
        if (_seqActive && _seqNext < _seq.size())
        {
            auto& slot = _seq[_seqNext];
            _jobSeq = int32_t(_seqNext++);
            slot.frame = _frameId;
            slot.motion = _motion;
            slot.temporal = _stats.temporal;
            SeqCopy(list, _feed.Get(), kRead, slot.fed);
            if (_motion.res != nullptr)
                SeqCopy(list, _motion.res, _motion.state, slot.mv);
        }
        if (_stats.framesRun % 600 == 0)
        {
            char status[512] = {};
            if (_rt.api.GetStatus != nullptr)
                _rt.api.GetStatus(_ctx, status, sizeof(status));
            const char* hip = strstr(status, "hip_ms=");
            Log(OptiLog_Info, "stats (%ls): GPU network+handoff %.2f ms median, network %.2f ms, %llu run, %llu skipped, %ux%u -> %ux%u, history %d, runtime %.14s",
                name.c_str(), Median(_hipSamples, _hipCount), Median(_netSamples, _netCount),
                (unsigned long long) _stats.framesRun, (unsigned long long) _stats.framesSkipped, _stats.srcWidth,
                _stats.srcHeight, _feedW, _feedH, (int) _stats.temporal, hip ? hip : "-");
        }
        if (_stats.framesRun % 180 == 0 && (_flkStaticN || _flkMcN))
        {
            {
                double fs = 0, fm = 0;
                const uint32_t ns = std::min(_flkStaticN, kMeterWindow), nm = std::min(_flkMcN, kMeterWindow);
                for (uint32_t i = 0; i < ns; i++)
                    fs += _flkStatic[i];
                for (uint32_t i = 0; i < nm; i++)
                    fm += _flkMc[i];
                Log(OptiLog_Info, "flicker (%ls): still %+.5f, moving %+.5f (answer minus input change, last %u / %u frames)",
                    name.c_str(), ns ? fs / ns : 0.0, nm ? fm / nm : 0.0, ns, nm);
            }
        }
    }
    else
    {
        _feedValid = false;
        _stats.framesSkipped++;
        if (++_failures == 1 || _failures % 600 == 0)
            Log(OptiLog_Warn, "%s", _stats.error.c_str());
        if (_failures >= 120)
            Fail("giving up after repeated failures: " + _stats.error);
    }

    if (produced || wait != 0)
    {
        _pendingWait = wait;
        _pendingProducer = produced;
        _pendingList = list;
    }
}

void NrEngine::Record(OptiUpscaleFrame* f, bool pre)
{
    auto* list = static_cast<ID3D12GraphicsCommandList*>(f->commandList);
    auto* color = static_cast<ID3D12Resource*>(f->color);
    auto* output = static_cast<ID3D12Resource*>(f->output);
    auto* mv = static_cast<ID3D12Resource*>(f->motionVectors);
    const auto colorState = D3D12_RESOURCE_STATES(f->colorState);
    const auto outputState = D3D12_RESOURCE_STATES(f->outputState);

    ReadTimings();

    // Our passes read the colour (before upscaling) and the vectors: move them to a shader state when needed
    const auto mvState = D3D12_RESOURCE_STATES(f->motionVectorsState);
    const bool colorMove = pre && !Readable(colorState);
    const bool mvMove = mv != nullptr && !Readable(mvState);
    if (colorMove)
        Barrier(list, color, colorState, kRead);
    if (mvMove)
        Barrier(list, mv, mvState, kRead);

    // Source of this frame (what we feed and what the edit is added to) and where the result goes
    ID3D12Resource* src = nullptr;
    ID3D12Resource* target = nullptr;
    D3D12_RESOURCE_DESC srcDesc {};
    if (pre)
    {
        src = color;
        srcDesc = color->GetDesc();
        if (!_override || _override->GetDesc().Width != srcDesc.Width || _override->GetDesc().Height != srcDesc.Height ||
            _overrideState != colorState)
        {
            _override = Texture(_device.Get(), (uint32_t) srcDesc.Width, srcDesc.Height,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, true, colorState);
            _overrideState = colorState;
        }
        target = _override.Get();
    }
    else
    {
        const auto od = output->GetDesc();
        if (!_srcCopy || _srcCopy->GetDesc().Width != od.Width || _srcCopy->GetDesc().Height != od.Height ||
            _srcCopy->GetDesc().Format != od.Format)
            _srcCopy = Texture(_device.Get(), (uint32_t) od.Width, od.Height, od.Format, false, kRead);
        if (!_srcCopy)
            return;
        Barrier(list, output, outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, _srcCopy.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(_srcCopy.Get(), output);
        Barrier(list, _srcCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        Barrier(list, output, D3D12_RESOURCE_STATE_COPY_SOURCE, outputState);
        src = _srcCopy.Get();
        srcDesc = od;
        target = output;
    }
    if (target == nullptr)
        return;
    const uint32_t w = pre ? f->renderWidth : f->outputWidth;
    const uint32_t h = pre ? f->renderHeight : f->outputHeight;

    // HDR colour is in scene units: exposure-mapped into the network range, the guard checks the result
    const uint32_t encoding = (f->flags & OptiUpscale_Hdr) ? 4 : 0;
    _stats.encoding = encoding;
    _feedNormalised = encoding == 4;
    ID3D12Resource* norm = RunStats(list, f, src, srcDesc.Format, w, h, encoding);

    // The previous job's answer for the previous frame, moved onto this one
    uint64_t wait = 0;
    ID3D12Resource* enh = nullptr;
    Consume(list, &wait, &enh);
    if (enh != nullptr && !(f->flags & OptiUpscale_Reset))
    {
        const auto ed = enh->GetDesc();
        const bool lowResMv = !(f->flags & OptiUpscale_DisplayResMV);
        const float mvSpaceW = float(lowResMv ? f->renderWidth : f->outputWidth);
        const float mvSpaceH = float(lowResMv ? f->renderHeight : f->outputHeight);
        ShaderConstants c {};
        c.sizeX = w;
        c.sizeY = h;
        if (mv != nullptr && _settings.motion)
        {
            const auto md = mv->GetDesc();
            c.mvUvScaleX = mvSpaceW / float(md.Width);
            c.mvUvScaleY = mvSpaceH / float(md.Height);
            c.mvToUvX = f->mvScaleX / mvSpaceW;
            c.mvToUvY = f->mvScaleY / mvSpaceH;
            c.useMv = 1;
        }
        c.enhUvScaleX = float(_feedW) / float(ed.Width);
        c.enhUvScaleY = float(_feedH) / float(ed.Height);
        c.fedUvScaleX = c.fedUvScaleY = 1.0f;
        c.strength = _settings.strength;
        c.colorStrength = _settings.ColorStrength(true);
        c.upMode = uint32_t(_settings.upsample);
        c.encoding = encoding;
        c.useNorm = norm != nullptr;

        const D3D12_RESOURCE_STATES targetState = pre ? colorState : outputState;
        Barrier(list, target, targetState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* srv[4] = { src, enh, _feed.Get(), c.useMv ? mv : nullptr };
        const DXGI_FORMAT fmt[4] = { srcDesc.Format, ed.Format, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     mv ? mv->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT };
        Dispatch(list, _psoApply.Get(), c, srv, fmt, target, pre ? DXGI_FORMAT_R16G16B16A16_FLOAT : srcDesc.Format,
                 w, h, norm);
        UavBarrier(list, target);
        Barrier(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, targetState);
        MaybeDump(list, enh, target, targetState, src);
        if (pre)
            f->colorOverride = target;
    }

    // Motion vectors of this frame for a temporal runtime, as they are (render or output size)
    _motion = {};
    if (mv != nullptr)
    {
        const bool lowResMv = !(f->flags & OptiUpscale_DisplayResMV);
        LogMotion(f);
        SetMotion(mv, mvMove ? kRead : mvState, lowResMv ? f->renderWidth : f->outputWidth,
                  lowResMv ? f->renderHeight : f->outputHeight, f->mvScaleX, f->mvScaleY, f->frameId,
                  (f->flags & OptiUpscale_Reset) != 0);
    }

    // Feed this frame: the source before any edit
    {
        ShaderConstants c {};
        c.sizeX = _feedW;
        c.sizeY = _feedH;
        c.srcUvScaleX = float(w) / float(srcDesc.Width);
        c.srcUvScaleY = float(h) / float(srcDesc.Height);
        c.encoding = encoding;
        Barrier(list, _feed.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* srv[4] = { src, nullptr, nullptr, nullptr };
        const DXGI_FORMAT fmt[4] = { srcDesc.Format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
        Dispatch(list, _psoDown.Get(), c, srv, fmt, _feed.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, _feedW, _feedH,
                 norm);
        UavBarrier(list, _feed.Get());
        Barrier(list, _feed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    }

    Produce(list, f, wait);

    if (colorMove)
        Barrier(list, color, kRead, colorState);
    if (mvMove)
        Barrier(list, mv, kRead, mvState);
}

// Stats pass on this frame's source (see the Stats shader); nullptr when it can't run, Apply then has no guard
ID3D12Resource* NrEngine::RunStats(ID3D12GraphicsCommandList* list, const OptiUpscaleFrame* f, ID3D12Resource* src,
                                   DXGI_FORMAT srcFmt, uint32_t w, uint32_t h, uint32_t encoding)
{
    if (!_norm)
    {
        _norm = Texture(_device.Get(), 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true, kRead);
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kNormSlots * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* p = nullptr;
        if (_norm && SUCCEEDED(_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&_normReadback))) &&
            SUCCEEDED(_normReadback->Map(0, nullptr, &p)))
            _normMapped = static_cast<const float*>(p);
        if (!_norm || !_normMapped)
        {
            _norm.Reset();
            _normReadback.Reset();
            _normMapped = nullptr;
            Log(OptiLog_Warn, "stats pass unavailable: no exposure mapping or HDR guard");
            return nullptr;
        }
    }

    // The oldest slot of the ring, a few frames old (a status: the GPU applies the guard itself)
    if (_normFrame >= kNormSlots)
    {
        const float* v = _normMapped + ((_normFrame + 1) % kNormSlots) * (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT / 4);
        const bool bypassed = v[1] > 0.5f;
        if (bypassed != _stats.bypassed)
            Log(bypassed ? OptiLog_Warn : OptiLog_Info,
                bypassed ? "HDR guard: fed median above 1.5 after mapping (scale %g), NR bypassed"
                         : "HDR guard: fed median back in range (scale %g), NR applied",
                v[0]);
        _stats.netScale = v[0];
        _stats.guardShare = v[1];
        _stats.bypassed = bypassed;
    }

    // Game exposure (1x1 float) unless disabled or absent, auto-exposure otherwise
    auto* exp = static_cast<ID3D12Resource*>(f->exposure);
    if (exp != nullptr)
    {
        const auto d = exp->GetDesc();
        if (d.Width != 1 || d.Height != 1 || !IsExposureFormat(d.Format))
            exp = nullptr;
    }
    const bool useGame = encoding == 4 && _settings.exposure && exp != nullptr;
    _stats.gameExposure = useGame;
    const auto expState = D3D12_RESOURCE_STATES(f->exposureState);
    const bool expMove = useGame && !Readable(expState);
    if (expMove)
        Barrier(list, exp, expState, kRead);

    ShaderConstants c {};
    c.sizeX = c.sizeY = 8; // one group
    c.srcUvScaleX = float(w) / float(src->GetDesc().Width);
    c.srcUvScaleY = float(h) / float(src->GetDesc().Height);
    c.encoding = encoding;
    c.expoMul = useGame ? 1.0f / (f->preExposure > 0.0f ? f->preExposure : 1.0f) : 0.0f;
    Barrier(list, _norm.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* srv[4] = { src, nullptr, nullptr, useGame ? exp : nullptr };
    const DXGI_FORMAT fmt[4] = { srcFmt, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                                 useGame ? exp->GetDesc().Format : DXGI_FORMAT_UNKNOWN };
    Dispatch(list, _psoStats.Get(), c, srv, fmt, _norm.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 8);
    UavBarrier(list, _norm.Get());
    Barrier(list, _norm.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst {}, from {};
    dst.pResource = _normReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = (_normFrame % kNormSlots) * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
    from.pResource = _norm.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
    Barrier(list, _norm.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
    _normFrame++;
    if (expMove)
        Barrier(list, exp, kRead, expState);
    return _norm.Get();
}

// History is dropped on a cut, after a frame that was not fed and when the vector extent changes
bool NrEngine::HistoryBreak(uint64_t upscaleId, uint32_t mvW, uint32_t mvH, bool reset)
{
    const bool gap = _lastFedUpscale == 0 || upscaleId != _lastFedUpscale + 1;
    const bool resized = mvW != _lastMvW || mvH != _lastMvH;
    _lastMvW = mvW;
    _lastMvH = mvH;
    return reset || gap || resized;
}

// w x h: the vectors' extent (render or output size), sx/sy: vector -> pixels of that extent
void NrEngine::SetMotion(ID3D12Resource* mv, D3D12_RESOURCE_STATES state, uint32_t w, uint32_t h, float sx, float sy,
                         uint64_t upscaleId, bool reset)
{
    _motion = {};
    _motionUpscale = 0;
    if (mv == nullptr || w == 0 || h == 0)
        return;
    // The runtime samples them over the feed's UV range: pixels of that extent -> feed pixels
    _motion.res = mv;
    _motion.state = state;
    _motion.w = w;
    _motion.h = h;
    _motion.scaleX = sx * float(_feedW) / float(w);
    _motion.scaleY = sy * float(_feedH) / float(h);
    _motion.reset = upscaleId != 0 ? HistoryBreak(upscaleId, w, h, reset) : reset;
    _motionUpscale = upscaleId;
}

void NrEngine::LogMotion(const OptiUpscaleFrame* f)
{
    const uint32_t flags = f->flags & (OptiUpscale_JitteredMV | OptiUpscale_DisplayResMV);
    if (flags == _loggedMvFlags)
        return;
    _loggedMvFlags = flags;
    const auto d = static_cast<ID3D12Resource*>(f->motionVectors)->GetDesc();
    Log(OptiLog_Info, "motion vectors %llux%u fmt %d, %s size, scale %.3f %.3f, jitter %.3f %.3f%s", d.Width, d.Height,
        (int) d.Format, (flags & OptiUpscale_DisplayResMV) ? "output" : "render", f->mvScaleX, f->mvScaleY, f->jitterX,
        f->jitterY, (flags & OptiUpscale_JitteredMV) ? ", jittered (history expects unjittered vectors)" : "");
}

// Terminal: this evaluate's vectors, copied on the game's list; the present of the same frame takes them
void NrEngine::CaptureMotion(OptiUpscaleFrame* f)
{
    auto* mv = static_cast<ID3D12Resource*>(f->motionVectors);
    auto* list = static_cast<ID3D12GraphicsCommandList*>(f->commandList);
    std::lock_guard lock(_mutex);
    _mvCap = {};
    // Also without history: sequence dumps score against the vectors
    if (mv == nullptr || list == nullptr || _state != State::Ready || f->device != _device.Get() ||
        list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    LogMotion(f);
    const auto md = mv->GetDesc();
    if (!_mvCopy || _mvCopy->GetDesc().Width != md.Width || _mvCopy->GetDesc().Height != md.Height ||
        _mvCopy->GetDesc().Format != ViewFormat(md.Format))
        _mvCopy = Texture(_device.Get(), (uint32_t) md.Width, md.Height, ViewFormat(md.Format), false, kRead);
    if (!_mvCopy)
        return;
    const auto st = D3D12_RESOURCE_STATES(f->motionVectorsState);
    Barrier(list, mv, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, _mvCopy.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(_mvCopy.Get(), mv);
    Barrier(list, _mvCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    Barrier(list, mv, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    const bool display = (f->flags & OptiUpscale_DisplayResMV) != 0;
    _mvCap.frameId = f->frameId;
    _mvCap.w = display ? f->outputWidth : f->renderWidth;
    _mvCap.h = display ? f->outputHeight : f->renderHeight;
    _mvCap.scaleX = f->mvScaleX;
    _mvCap.scaleY = f->mvScaleY;
    _mvCap.reset = (f->flags & OptiUpscale_Reset) != 0;
}

// Terminal placement: NR on the HUD-less colour, its edit added to the back buffer outside the UI, recorded into
// our own list on the present queue before OptiScaler draws its overlay
void NrEngine::OnPresent(OptiPresentFrame* f)
{
    if (f->api != OptiApi_D3D12 || f->phase != OptiPresent_BeforeOverlay || !RuntimeLoaded() ||
        f->backBuffer == nullptr || f->device == nullptr || f->queue == nullptr)
        return;

    NrSettings s;
    {
        std::lock_guard lock(_mutex);
        s = _settings;
    }
    if (!s.enabled || s.placement != Placement::Terminal)
        return;

    // The game's frame generation presents generated frames here too: whole frame on every present (consistent).
    // At the game's Present on its FG swapchain the HUD-less colour is this frame's FG input: HUD-less NR, and the
    // edit also goes onto that input so the generated frames are made from NR'd frames
    constexpr size_t kFgEnd = offsetof(OptiPresentFrame, frameGeneration) + sizeof(uint32_t);
    constexpr size_t kFgScEnd = offsetof(OptiPresentFrame, fgSwapchain) + sizeof(uint32_t);
    const bool fgSc = f->size >= kFgScEnd && f->fgSwapchain != 0;
    const bool gameFg = f->size >= kFgEnd && f->frameGeneration != OptiFrameGen_Off && !fgSc;
    const uint32_t hudSource = f->size >= kFgEnd ? f->hudlessSource : OptiHudless_Hudfix;
    const bool haveHud = f->hudless != nullptr && !gameFg;
    const bool editHud = haveHud && fgSc && hudSource == OptiHudless_FfxFG && f->frameGeneration != OptiFrameGen_Off;
    _hudlessStreak = haveHud ? 0 : _hudlessStreak + 1;
    // A lone missed pick keeps the HUD-less mode (no NR that frame); a run of them falls back
    const bool whole = !haveHud && s.wholeFrameFallback && (gameFg || _hudlessStreak > 30);
    {
        std::lock_guard lock(_mutex);
        _stats.source = haveHud ? (editHud ? "HUD-less colour at the game's FG swapchain (generated frames from NR'd frames)"
                                   : hudSource == OptiHudless_FfxFG ? "HUD-less colour (the game's FSR FG input)"
                                                                    : "HUD-less colour (OptiScaler HUDfix)")
                        : whole ? (gameFg ? "whole frame, UI included (the game's frame generation: every present)"
                                          : "whole frame, UI included (no HUD-less colour)")
                        : s.wholeFrameFallback ? "waiting for a HUD-less colour"
                                               : "off: no HUD-less colour (WholeFrameFallback off)";
    }
    if (!haveHud && !whole)
    {
        if (_hudlessMissing++ % 600 == 0)
            Log(OptiLog_Info, "terminal: no HUD-less colour this frame (%llu frames so far)",
                (unsigned long long) _hudlessMissing);
        _stats.framesSkipped++;
        return;
    }
    if (whole && _hudlessStreak % 600 == (gameFg ? 1 : 31))
        Log(OptiLog_Info, "terminal: whole frame NR, UI included (%s)",
            gameFg ? "the game's frame generation" : "no HUD-less colour");

    auto* bb = static_cast<ID3D12Resource*>(f->backBuffer);
    auto* hud = haveHud ? static_cast<ID3D12Resource*>(f->hudless) : nullptr;
    auto* queue = static_cast<ID3D12CommandQueue*>(f->queue);
    const auto bd = bb->GetDesc();
    const uint32_t w = (uint32_t) bd.Width, h = bd.Height;

    if (!Begin(static_cast<ID3D12Device*>(f->device), queue, w, h))
        return SeqIdle(_state == State::Failed ? "NR failed" : "the network is being built");

    std::unique_lock lock(_mutex);
    if (_state != State::Ready)
        return;
    _stats.srcWidth = w;
    _stats.srcHeight = h;
    ReadTimings();

    if (!EnsureCopies(bd))
        return;
    uint32_t slot = 0;
    auto* list = NextOwnList(&slot);
    if (list == nullptr)
        return;

    const auto bbState = D3D12_RESOURCE_STATES(f->backBufferState);
    auto hudState = D3D12_RESOURCE_STATES(f->hudlessState);
    if (whole)
    {
        // The back buffer's copy stands in for the HUD-less colour: the UI mask is empty, the feed is the frame
        Barrier(list, bb, bbState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, _termCopy.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(_termCopy.Get(), bb);
        Barrier(list, _termCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        Barrier(list, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, bbState);
        hud = _termCopy.Get();
        hudState = kRead;
    }
    const uint32_t encoding = f->colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2
                              : f->colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709  ? 3
                                                                                           : 1;
    _stats.encoding = encoding;

    // The FG input is edited in place: the feed and the edit read its copy
    ID3D12Resource* hudTarget = nullptr;
    if (editHud && EnsureHudCopies(hud->GetDesc()))
    {
        hudTarget = hud;
        Barrier(list, hud, hudState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, _hudCopy.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(_hudCopy.Get(), hud);
        Barrier(list, _hudCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        Barrier(list, hud, D3D12_RESOURCE_STATE_COPY_SOURCE, hudState);
        hud = _hudCopy.Get();
        hudState = kRead;
    }
    Barrier(list, hud, hudState, kRead);

    uint64_t wait = 0;
    ID3D12Resource* enh = nullptr;
    Consume(list, &wait, &enh);
    if (enh != nullptr)
    {
        const auto ed = enh->GetDesc();
        Barrier(list, bb, bbState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, _termCopy.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(_termCopy.Get(), bb);
        Barrier(list, _termCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        Barrier(list, _termOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ShaderConstants c {};
        c.sizeX = w;
        c.sizeY = h;
        c.enhUvScaleX = float(_feedW) / float(ed.Width);
        c.enhUvScaleY = float(_feedH) / float(ed.Height);
        c.fedUvScaleX = c.fedUvScaleY = 1.0f;
        c.strength = _settings.strength;
        c.colorStrength = _settings.ColorStrength(false);
        c.upMode = uint32_t(_settings.upsample);
        c.uiLow = _settings.uiLow;
        c.uiHigh = _settings.uiHigh;
        c.encoding = encoding;
        c.paperWhite = _settings.paperWhite / 80.0f;
        ID3D12Resource* srv[4] = { _termCopy.Get(), enh, _feed.Get(), hud };
        const DXGI_FORMAT fmt[4] = { bd.Format, ed.Format, DXGI_FORMAT_R16G16B16A16_FLOAT, hud->GetDesc().Format };
        Dispatch(list, _psoTerminal.Get(), c, srv, fmt, _termOut.Get(), bd.Format, w, h);
        Barrier(list, _termOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(bb, _termOut.Get());
        Barrier(list, bb, D3D12_RESOURCE_STATE_COPY_DEST, bbState);
        if (hudTarget != nullptr)
        {
            // Same edit on the HUD-less input, all of it scene (no UI mask: it is compared with itself)
            const auto hd = hud->GetDesc();
            ID3D12Resource* hsrv[4] = { hud, enh, _feed.Get(), hud };
            const DXGI_FORMAT hfmt[4] = { hd.Format, ed.Format, DXGI_FORMAT_R16G16B16A16_FLOAT, hd.Format };
            Barrier(list, _hudOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Dispatch(list, _psoTerminal.Get(), c, hsrv, hfmt, _hudOut.Get(), hd.Format, w, h);
            Barrier(list, _hudOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const auto ts = D3D12_RESOURCE_STATES(f->hudlessState);
            Barrier(list, hudTarget, ts, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(hudTarget, _hudOut.Get());
            Barrier(list, hudTarget, D3D12_RESOURCE_STATE_COPY_DEST, ts);
        }
        MaybeDump(list, enh, bb, bbState, _termCopy.Get(), hud, hudTarget, D3D12_RESOURCE_STATES(f->hudlessState));
        Meter(list, enh, false);
    }

    // Feed the HUD-less colour, linearised, with the vectors of the evaluate of this frame
    _motion = {};
    if (_mvCopy && _mvCap.frameId != 0 && _mvCap.frameId == f->upscaleFrameId)
        SetMotion(_mvCopy.Get(), kRead, _mvCap.w, _mvCap.h, _mvCap.scaleX, _mvCap.scaleY, _mvCap.frameId, _mvCap.reset);
    {
        const auto hd = hud->GetDesc();
        ShaderConstants c {};
        c.sizeX = _feedW;
        c.sizeY = _feedH;
        c.srcUvScaleX = float(w) / float(hd.Width);
        c.srcUvScaleY = float(h) / float(hd.Height);
        c.encoding = encoding;
        c.paperWhite = _settings.paperWhite / 80.0f;
        Barrier(list, _feed.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* srv[4] = { hud, nullptr, nullptr, nullptr };
        const DXGI_FORMAT fmt[4] = { hd.Format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
        Dispatch(list, _psoDown.Get(), c, srv, fmt, _feed.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, _feedW, _feedH);
        UavBarrier(list, _feed.Get());
        Barrier(list, _feed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    }
    Barrier(list, hud, kRead, hudState);

    Produce(list, nullptr, wait);
    _mvCap = {};
    lock.unlock();
    SubmitOwn(queue, slot, list, wait);
}

ID3D12GraphicsCommandList* NrEngine::NextOwnList(uint32_t* slot)
{
    if (!_presentList)
    {
        for (auto& a : _presentAlloc)
            _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _presentAlloc[0].Get(), nullptr,
                                   IID_PPV_ARGS(&_presentList));
        if (_presentList)
            _presentList->Close();
        _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_presentFence));
    }
    if (!_presentList || !_presentFence)
        return nullptr;
    *slot = uint32_t(_presentCounter % kPresentLists);
    // the slot's list must be done before its allocator is reset: no fixed cap
    if (!GpuWaitDone(_presentFence.Get(), _presentValue[*slot]))
    {
        Log(OptiLog_Warn, "NR (%ls): own list still in flight: frame skipped", name.c_str());
        return nullptr;
    }
    _presentAlloc[*slot]->Reset();
    _presentList->Reset(_presentAlloc[*slot].Get(), nullptr);
    _presentValue[*slot] = ++_presentCounter; // signalled by SubmitOwn
    return _presentList.Get();
}

// Closes and submits one of our lists; a producer in it goes through OnExecute like a game list
void NrEngine::SubmitOwn(ID3D12CommandQueue* queue, uint32_t slot, ID3D12GraphicsCommandList* list, uint64_t wait)
{
    list->Close();
    _ownQueue = queue;
    ID3D12CommandList* l[] = { list };
    if (_pendingList.load() == list)
    {
        OnExecute(queue, 1, l);
    }
    else
    {
        if (wait != 0)
            queue->Wait(_hipFence.Get(), wait);
        o_ExecuteCommandLists(queue, 1, l);
    }
    queue->Signal(_presentFence.Get(), _presentValue[slot]);
}

// A present queued behind the wait on the network page-faults the display device: present once it is done.
// False: not done (device gone or 60 s), the caller must not present (a 1 s cap queued the Present behind the wait).
bool NrEngine::SettleOwn()
{
    const uint64_t value = _answerWait;
    _answerWait = 0;
    if (value == 0 || !_hipFence || _hipFence->GetCompletedValue() >= value)
        return true;
    const bool done = GpuWaitDone(_hipFence.Get(), value);
    if (!done)
        Log(OptiLog_Error, "NR (%ls): network answer not done: present skipped", name.c_str());
    return done;
}

void NrEngine::Quiesce()
{
    if (_state != State::Ready)
        return;
    std::lock_guard lock(_mutex);
    WaitIdle();
}

bool NrEngine::Shutdown()
{
    if (_worker.joinable())
        _worker.join();
    for (int i = 0; i < 2000 && _pendingList.load() != nullptr; i++)
        Sleep(1);
    std::lock_guard lock(_mutex);
    if (_pendingList.load() != nullptr)
        return Log(OptiLog_Warn, "hot reload: a game list with NR work is not submitted yet"), false;
    if (_ctx != nullptr)
        DrainJob();
    if (_event != nullptr)
    {
        WaitIdle();
        if (_presentFence)
            GpuWaitDone(_presentFence.Get(), _presentCounter); // the present lists before they are freed
    }
    if (_ctx != nullptr)
    {
        _rt.api.Drain(_ctx);
        _rt.api.Destroy(_ctx);
        _ctx = nullptr;
    }
    _state = State::Idle;
    return true;
}

bool NrEngine::EnsureCopies(const D3D12_RESOURCE_DESC& bd)
{
    if (_termCopy && _termCopy->GetDesc().Width == bd.Width && _termCopy->GetDesc().Height == bd.Height &&
        _termCopy->GetDesc().Format == bd.Format)
        return true;
    _termCopy = Texture(_device.Get(), (uint32_t) bd.Width, bd.Height, bd.Format, false, kRead);
    _termOut = Texture(_device.Get(), (uint32_t) bd.Width, bd.Height, UavFormat(bd.Format), true,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (_termCopy && _termOut)
        _termCopy->SetName(L"lmxxf_nr source copy"), _termOut->SetName(L"lmxxf_nr result");
    if (!_termCopy || !_termOut)
    {
        Fail("copy textures failed for format " + std::to_string(bd.Format) + ", device " +
             std::to_string((uint32_t) _device->GetDeviceRemovedReason()));
        return false;
    }
    return true;
}

bool NrEngine::EnsureHudCopies(const D3D12_RESOURCE_DESC& hd)
{
    if (_hudCopy && _hudCopy->GetDesc().Width == hd.Width && _hudCopy->GetDesc().Height == hd.Height &&
        _hudCopy->GetDesc().Format == hd.Format)
        return true;
    _hudCopy = Texture(_device.Get(), (uint32_t) hd.Width, hd.Height, hd.Format, false, kRead);
    _hudOut = Texture(_device.Get(), (uint32_t) hd.Width, hd.Height, UavFormat(hd.Format), true,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (_hudCopy && _hudOut)
        return true;
    _hudCopy.Reset();
    _hudOut.Reset();
    Log(OptiLog_Warn, "HUD-less copies failed for format %d: generated frames without NR", (int) hd.Format);
    return false;
}

bool NrEngine::RunOwn(ID3D12Device* device, ID3D12CommandQueue* queue, const OwnFrame& in)
{
    if (!RuntimeLoaded() || in.src == nullptr || in.dst == nullptr || !Settings().enabled)
        return false;
    const auto sd = in.src->GetDesc();
    const uint32_t w = (uint32_t) sd.Width, h = sd.Height;
    if (!Begin(device, queue, w, h))
    {
        SeqIdle(_state == State::Failed ? "NR failed" : "the network is being built");
        return false;
    }

    std::unique_lock lock(_mutex);
    if (_state != State::Ready || !EnsureCopies(sd))
        return false;
    _stats.srcWidth = w;
    _stats.srcHeight = h;
    _stats.encoding = in.encoding;
    ReadTimings();

    // The source as it is now (src may be dst)
    auto copySource = [&](ID3D12GraphicsCommandList* list)
    {
        Barrier(list, in.src, in.srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, _termCopy.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(_termCopy.Get(), in.src);
        Barrier(list, _termCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        Barrier(list, in.src, D3D12_RESOURCE_STATE_COPY_SOURCE, in.srcState);
    };
    auto feed = [&](ID3D12GraphicsCommandList* list)
    {
        ShaderConstants c {};
        c.sizeX = _feedW;
        c.sizeY = _feedH;
        c.srcUvScaleX = c.srcUvScaleY = 1.0f;
        c.encoding = in.encoding;
        c.paperWhite = _settings.paperWhite / 80.0f;
        Barrier(list, _feed.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* srv[4] = { _termCopy.Get(), nullptr, nullptr, nullptr };
        const DXGI_FORMAT fmt[4] = { sd.Format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
        Dispatch(list, _psoDown.Get(), c, srv, fmt, _feed.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, _feedW, _feedH);
        UavBarrier(list, _feed.Get());
        Barrier(list, _feed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    };
    auto apply = [&](ID3D12GraphicsCommandList* list, ID3D12Resource* enh, bool motion)
    {
        const auto ed = enh->GetDesc();
        ShaderConstants c {};
        c.sizeX = w;
        c.sizeY = h;
        if (motion && in.motion != nullptr && in.motionW > 0 && in.motionH > 0)
        {
            const auto md = in.motion->GetDesc();
            c.mvUvScaleX = float(in.motionW) / float(md.Width);
            c.mvUvScaleY = float(in.motionH) / float(md.Height);
            c.mvToUvX = in.mvScaleX / float(in.motionW);
            c.mvToUvY = in.mvScaleY / float(in.motionH);
            c.useMv = 1;
        }
        c.enhUvScaleX = float(_feedW) / float(ed.Width);
        c.enhUvScaleY = float(_feedH) / float(ed.Height);
        c.fedUvScaleX = c.fedUvScaleY = 1.0f;
        c.strength = _settings.strength;
        c.colorStrength = _settings.ColorStrength(false);
        c.upMode = uint32_t(_settings.upsample);
        c.encoding = in.encoding;
        c.paperWhite = _settings.paperWhite / 80.0f;
        Barrier(list, _termOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* srv[4] = { _termCopy.Get(), enh, _feed.Get(), c.useMv ? in.motion : nullptr };
        const DXGI_FORMAT fmt[4] = { sd.Format, ed.Format, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     in.motion ? in.motion->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT };
        if (c.useMv)
            Barrier(list, in.motion, in.motionState, kRead);
        Dispatch(list, _psoApply.Get(), c, srv, fmt, _termOut.Get(), sd.Format, w, h);
        if (c.useMv)
            Barrier(list, in.motion, kRead, in.motionState);
        Barrier(list, _termOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, in.dst, in.dstState, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(in.dst, _termOut.Get());
        Barrier(list, in.dst, D3D12_RESOURCE_STATE_COPY_DEST, in.dstState);
        // whole frame: the source copy pairs the result with this frame's feed
        MaybeDump(list, enh, in.dst, in.dstState, in.src == in.dst ? _termCopy.Get() : nullptr);
    };

    _motion = {};
    if (in.motion != nullptr && in.motionW > 0 && in.motionH > 0)
    {
        _motion.res = in.motion;
        _motion.state = in.motionState;
        _motion.w = in.motionW;
        _motion.h = in.motionH;
        _motion.scaleX = in.mvScaleX * float(_feedW) / float(in.motionW);
        _motion.scaleY = in.mvScaleY * float(_feedH) / float(in.motionH);
        _motion.reset = in.reset;
    }
    _motionUpscale = 0;

    uint32_t slot = 0;
    bool wrote = false;
    uint64_t wait = 0;
    ID3D12Resource* enh = nullptr;
    if (in.waitAnswer)
    {
        // This frame's answer: feed and enqueue, then consume it once the network is done
        auto* list = NextOwnList(&slot);
        if (list == nullptr)
            return false;
        Consume(list, &wait, &enh);
        copySource(list);
        feed(list);
        Produce(list, nullptr, wait);
        lock.unlock();
        SubmitOwn(queue, slot, list, wait);
        _answerWait = wait; // the present must not queue behind that list's wait
        lock.lock();
        if (_jobState != Job::Enqueued)
            return false;
        list = NextOwnList(&slot);
        if (list == nullptr)
            return false;
        Consume(list, &wait, &enh);
        if (enh != nullptr)
        {
            apply(list, enh, false);
            Meter(list, enh, true);
            wrote = true;
        }
        lock.unlock();
        SubmitOwn(queue, slot, list, wait);
        _answerWait = wait;
        return wrote;
    }

    // The previous frame's answer, moved by this frame's motion vectors
    auto* list = NextOwnList(&slot);
    if (list == nullptr)
        return false;
    copySource(list);
    Consume(list, &wait, &enh);
    if (enh != nullptr && !in.reset)
    {
        apply(list, enh, true);
        wrote = true;
    }
    Meter(list, enh, false); // the vectors are this frame's, not the answer's
    feed(list);
    Produce(list, nullptr, wait);
    lock.unlock();
    SubmitOwn(queue, slot, list, wait);
    _answerWait = wait;
    return wrote;
}

void NrEngine::MaybeDump(ID3D12GraphicsCommandList* list, ID3D12Resource* enh, ID3D12Resource* result,
                         D3D12_RESOURCE_STATES resultState, ID3D12Resource* source, ID3D12Resource* hudless,
                         ID3D12Resource* hudResult, D3D12_RESOURCE_STATES hudResultState)
{
    if (_dumpArmed || dumpDir.empty() || enh == nullptr || (_dumpCount++ % 30) != 0)
        return;
    const std::wstring trigger = dumpDir + L"\\lmxxf_nr_dump.trigger";
    if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    DeleteFileW(trigger.c_str());

    ID3D12Resource* src[6] = { enh, _feed.Get(), result, source, hudless, hudResult };
    const D3D12_RESOURCE_STATES st[6] = { kRead, kRead, resultState, kRead, kRead, hudResultState };
    for (int i = 0; i < 6; i++)
    {
        if (src[i] == nullptr)
            continue;
        auto d = src[i]->GetDesc();
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        _dumpTex[i] = Texture(_device.Get(), (uint32_t) d.Width, d.Height, d.Format, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!_dumpTex[i])
            continue;
        Barrier(list, src[i], st[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(_dumpTex[i].Get(), src[i]);
        Barrier(list, src[i], D3D12_RESOURCE_STATE_COPY_SOURCE, st[i]);
    }
    _dumpArmed = true;
    _dumpGameValue = 0;
    _dumpOwnValue = _presentCounter; // own lists: done once the present fence reaches it
    Log(OptiLog_Info, "dump recorded (%ls): source fmt %u, encoding %u", name.c_str(),
        source ? (unsigned) source->GetDesc().Format : 0u, (unsigned) _stats.encoding);
}

void NrEngine::Meter(ID3D12GraphicsCommandList* list, ID3D12Resource* enh, bool jobMotionValid)
{
    if (!_settings.meter || !_psoMeter || enh == nullptr || !_feed || !_presentFence)
        return;
    const auto ed = enh->GetDesc();
    if (ed.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || ed.Width < _feedW || ed.Height < _feedH)
        return;
    if (!_meterPrev || _meterPrev->GetDesc().Width != 2ull * _feedW || _meterPrev->GetDesc().Height != _feedH)
    {
        _meterPrev = Texture(_device.Get(), 2 * _feedW, _feedH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, kRead);
        _meterPrevValid = false;
    }
    if (!_meterAcc)
    {
        _meterAcc = Texture(_device.Get(), 8, 1, DXGI_FORMAT_R32_UINT, true, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_DESC b {};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = 256;
        b.Height = b.DepthOrArraySize = b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES up { D3D12_HEAP_TYPE_UPLOAD }, rp { D3D12_HEAP_TYPE_READBACK };
        _device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&_meterZero));
        b.Width = 256 * kMeterSlots;
        _device->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&_meterReadback));
        void* z = nullptr;
        if (_meterZero && SUCCEEDED(_meterZero->Map(0, nullptr, &z)))
            memset(z, 0, 256), _meterZero->Unmap(0, nullptr);
    }
    if (!_meterPrev || !_meterAcc || !_meterZero || !_meterReadback)
        return;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    fp.Footprint = { DXGI_FORMAT_R32_UINT, 8, 1, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION acc { _meterAcc.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    const uint32_t slot = _meterNext % kMeterSlots;
    if (_meterPrevValid && !_jobReset && _meterValue[slot] == 0)
    {
        D3D12_TEXTURE_COPY_LOCATION zero { _meterZero.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        zero.PlacedFootprint = fp;
        list->CopyTextureRegion(&acc, 0, 0, 0, &zero, nullptr);
        Barrier(list, _meterAcc.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ShaderConstants c {};
        c.sizeX = _feedW;
        c.sizeY = _feedH;
        ID3D12Resource* mv = jobMotionValid ? _jobMotion.res : nullptr;
        if (mv != nullptr && _jobMotion.w > 0 && _jobMotion.h > 0)
        {
            c.useMv = 1;
            c.srcUvScaleX = float(_jobMotion.w);
            c.srcUvScaleY = float(_jobMotion.h);
            c.mvToUvX = _jobMotion.scaleX;
            c.mvToUvY = _jobMotion.scaleY;
            if (_jobMotion.state != kRead)
                Barrier(list, mv, _jobMotion.state, kRead);
        }
        ID3D12Resource* srv[4] = { _feed.Get(), enh, _meterPrev.Get(), c.useMv ? mv : nullptr };
        const DXGI_FORMAT fmt[4] = { DXGI_FORMAT_R16G16B16A16_FLOAT, ed.Format, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     c.useMv ? mv->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT };
        Dispatch(list, _psoMeter.Get(), c, srv, fmt, _meterAcc.Get(), DXGI_FORMAT_R32_UINT, _feedW, _feedH);
        if (c.useMv && _jobMotion.state != kRead)
            Barrier(list, mv, kRead, _jobMotion.state);
        Barrier(list, _meterAcc.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rb { _meterReadback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        rb.PlacedFootprint = fp;
        rb.PlacedFootprint.Offset = 256ull * slot;
        list->CopyTextureRegion(&rb, 0, 0, 0, &acc, nullptr);
        Barrier(list, _meterAcc.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        _meterValue[slot] = _presentCounter; // this own list's present fence value
        _meterNext++;
    }
    // this pair is the next one's previous
    const D3D12_BOX box { 0, 0, 0, _feedW, _feedH, 1 };
    D3D12_TEXTURE_COPY_LOCATION prev { _meterPrev.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION fed { _feed.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION out { enh, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    Barrier(list, _meterPrev.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, _feed.Get(), kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, enh, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTextureRegion(&prev, 0, 0, 0, &fed, &box);
    list->CopyTextureRegion(&prev, _feedW, 0, 0, &out, &box);
    Barrier(list, enh, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
    Barrier(list, _feed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
    Barrier(list, _meterPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    _meterPrevValid = true;
}

void NrEngine::MeterRead()
{
    if (!_meterReadback || !_presentFence)
        return;
    const uint64_t done = _presentFence->GetCompletedValue();
    for (uint32_t i = 0; i < kMeterSlots; i++)
    {
        if (_meterValue[i] == 0 || _meterValue[i] > done)
            continue;
        _meterValue[i] = 0;
        const uint32_t* a = nullptr;
        D3D12_RANGE r { 256ull * i, 256ull * i + 24 };
        if (FAILED(_meterReadback->Map(0, &r, (void**) &a)))
            continue;
        const uint32_t* v = a + 64 * i;
        if (v[0] >= 1000)
            _flkStatic[_flkStaticN++ % kMeterWindow] = float((double(v[1]) - double(v[2])) / (kMeterScale * v[0]));
        if (v[3] >= 1000)
            _flkMc[_flkMcN++ % kMeterWindow] = float((double(v[4]) - double(v[5])) / (kMeterScale * v[3]));
        D3D12_RANGE none { 0, 0 };
        _meterReadback->Unmap(0, &none);
    }
}

void NrEngine::SeqCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES state, SeqImage& img)
{
    img.desc = res->GetDesc();
    UINT rows = 0;
    UINT64 total = 0;
    _device->GetCopyableFootprints(&img.desc, 0, 1, 0, &img.fp, &rows, &img.rowBytes, &total);
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC b {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = total;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    img.buf.Reset();
    if (FAILED(_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&img.buf))))
        return;
    D3D12_TEXTURE_COPY_LOCATION dst { img.buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = img.fp;
    D3D12_TEXTURE_COPY_LOCATION src { res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    Barrier(list, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

void NrEngine::SeqPoll()
{
    if (dumpDir.empty())
        return;
    if (!_seqActive)
    {
        if ((_frameId % 8) != 0)
            return;
        if (_seqPending == 0)
        {
            const std::wstring trigger = dumpDir + L"\\lmxxf_nr_seq.trigger";
            if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES)
                return;
            int n = 0;
            if (FILE* t = _wfopen(trigger.c_str(), L"rb"))
            {
                if (fscanf(t, "%d", &n) != 1)
                    n = 0;
                fclose(t);
            }
            DeleteFileW(trigger.c_str());
            _seqPending = n >= 2 ? std::min(n, 48) : 12;
        }
        // a sequence never spans a session rebuild or a history reset
        if (_dirty)
            return SeqIdle("settings changed, the network is about to be rebuilt");
        if (_frameId < _readyFrame + kSeqSettle)
            return SeqIdle("network rebuilt, waiting for it to settle");
        if (_frameId < _resetFrame + kSeqSettle)
            return SeqIdle("history reset, waiting for it to settle");
        _seq.clear();
        _seq.resize(_seqPending);
        _seqPending = 0;
        _seqNext = 0;
        _seqWait = 0;
        _jobSeq = -1;
        _seqActive = true;
        Log(OptiLog_Info, "sequence dump of %zu frames started (%ls)", _seq.size(), name.c_str());
        return;
    }
    if (_seqNext < _seq.size() || ++_seqWait < 4)
        return;
    SeqWrite();
}

void NrEngine::SeqIdle(const char* why)
{
    if (dumpDir.empty())
        return;
    const bool pending = _seqPending != 0 || _seqActive ||
                         GetFileAttributesW((dumpDir + L"\\lmxxf_nr_seq.trigger").c_str()) != INVALID_FILE_ATTRIBUTES;
    const DWORD now = GetTickCount();
    if (!pending || (now - _seqIdleTick < 10000 && _seqIdleWhy == why))
        return;
    _seqIdleTick = now;
    _seqIdleWhy = why;
    Log(OptiLog_Info, "sequence dump pending (%ls): %s", name.c_str(), why);
}

void NrEngine::SeqRestart(const char* why)
{
    if (!_seqActive)
        return;
    _seqPending = (uint32_t) _seq.size();
    _seq.clear();
    _seqActive = false;
    _jobSeq = -1;
    Log(OptiLog_Info, "sequence dump restarted (%ls): %s", name.c_str(), why);
}

void NrEngine::SeqWrite()
{
    // Every list holding a copy ran before these signals
    ComPtr<ID3D12Fence> fence;
    _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    uint64_t v = 0;
    for (ID3D12CommandQueue* q : { _gameQueue.Get(), _ownQueue })
    {
        if (q == nullptr || !fence)
            continue;
        q->Signal(fence.Get(), ++v);
        GpuWaitDone(fence.Get(), v); // the sequence's readbacks are read next
    }

    const std::wstring dir = dumpDir + L"\\seq_" + name + L"_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    auto write = [&](SeqImage& img, const std::wstring& file) -> bool
    {
        uint8_t* p = nullptr;
        if (!img.buf || FAILED(img.buf->Map(0, nullptr, (void**) &p)))
            return false;
        if (FILE* out = _wfopen((dir + L"\\" + file).c_str(), L"wb"))
        {
            for (UINT y = 0; y < img.desc.Height; ++y)
                fwrite(p + img.fp.Offset + UINT64(y) * img.fp.Footprint.RowPitch, 1, (size_t) img.rowBytes, out);
            fclose(out);
        }
        D3D12_RANGE none { 0, 0 };
        img.buf->Unmap(0, &none);
        return true;
    };
    std::string json = "{\"feed\":[" + std::to_string(_feedW) + "," + std::to_string(_feedH) + "],\"frames\":[\n";
    char line[512];
    for (size_t k = 0; k < _seq.size(); ++k)
    {
        auto& s = _seq[k];
        wchar_t idx[8];
        swprintf(idx, 8, L"%02zu", k);
        const bool fed = write(s.fed, std::wstring(L"fed_") + idx + L".raw");
        const bool mv = write(s.mv, std::wstring(L"mv_") + idx + L".raw");
        const bool enh = write(s.enh, std::wstring(L"enh_") + idx + L".raw");
        snprintf(line, sizeof(line),
                 "%s{\"frame\":%llu,\"fed\":%s,\"fedSize\":[%llu,%u],\"fedFormat\":%d,\"enh\":%s,\"enhSize\":[%llu,%u],"
                 "\"enhFormat\":%d,\"mv\":%s,\"mvSize\":[%llu,%u],\"mvFormat\":%d,\"mvUsed\":[%u,%u],"
                 "\"mvScale\":[%.6f,%.6f],\"reset\":%d,\"temporal\":%d}\n",
                 k ? "," : "", (unsigned long long) s.frame, fed ? "true" : "false", s.fed.desc.Width, s.fed.desc.Height,
                 (int) s.fed.desc.Format, enh ? "true" : "false", s.enh.desc.Width, s.enh.desc.Height,
                 (int) s.enh.desc.Format, mv ? "true" : "false", s.mv.desc.Width, s.mv.desc.Height,
                 (int) s.mv.desc.Format, s.motion.w, s.motion.h, s.motion.scaleX, s.motion.scaleY, (int) s.motion.reset,
                 (int) s.temporal);
        json += line;
    }
    json += "]}\n";
    if (FILE* out = _wfopen((dir + L"\\seq.json").c_str(), L"wb"))
    {
        fwrite(json.data(), 1, json.size(), out);
        fclose(out);
    }
    Log(OptiLog_Info, "sequence dump of %zu frames written to %ls", _seq.size(), dir.c_str());
    _seq.clear();
    _seqActive = false;
    _jobSeq = -1;
}

void NrEngine::FinishDump()
{
    if (!_dumpArmed)
        return;
    const bool ownDone = _presentFence && _dumpOwnValue != 0 && _presentFence->GetCompletedValue() >= _dumpOwnValue;
    const bool gameDone = _dumpGameValue != 0 && _gameFence->GetCompletedValue() >= _dumpGameValue;
    if (!ownDone && !gameDone)
        return;
    const wchar_t* names[6] = { L"enh", L"fed", L"result", L"source", L"hudless", L"hudless_result" };
    for (int i = 0; i < 6; i++)
    {
        if (_dumpTex[i])
            Mgpu::DumpTexture(_device.Get(), _queue.Get(), _dumpTex[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              dumpDir + L"\\lmxxf_nr_" + name + L"_" + names[i]);
        _dumpTex[i].Reset();
    }
    _dumpArmed = false;
    Log(OptiLog_Info, "dump written to %ls", dumpDir.c_str());
}

void NrEngine::OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    ID3D12CommandList* pending = _pendingList.load();
    if (pending == nullptr || queue == _queue.Get())
        return o_ExecuteCommandLists(queue, count, lists);

    UINT idx = count;
    for (UINT i = 0; i < count; i++)
    {
        if (lists[i] == pending)
        {
            idx = i;
            break;
        }
    }
    if (idx == count)
        return o_ExecuteCommandLists(queue, count, lists);

    std::lock_guard lock(_mutex);
    if (_pendingList.load() != pending)
        return o_ExecuteCommandLists(queue, count, lists);

    // The consumer in that list reads the previous network's output
    if (_pendingWait != 0)
    {
        if (idx > 0)
            o_ExecuteCommandLists(queue, idx, lists);
        queue->Wait(_hipFence.Get(), _pendingWait);
        o_ExecuteCommandLists(queue, count - idx, lists + idx);
    }
    else
    {
        o_ExecuteCommandLists(queue, count, lists);
    }
    if (_gameQueue.Get() != queue)
        _gameQueue = queue;
    if (_dumpArmed && _dumpGameValue == 0)
    {
        queue->Signal(_gameFence.Get(), ++_gameValue);
        _dumpGameValue = _gameValue;
    }

    if (_pendingProducer && _jobState == Job::Recorded)
    {
        queue->Signal(_gameFence.Get(), ++_gameValue);
        _queue->Wait(_gameFence.Get(), _gameValue);

        const uint32_t slot = uint32_t(_tsFrame++ % kTimingSlots);
        const bool timed = _readback && _tsValue[slot] == 0;
        if (timed)
        {
            ID3D12CommandList* l[] = { _tsList[slot * 2].Get() };
            o_ExecuteCommandLists(_queue.Get(), 1, l);
        }
        const int32_t rc = _rt.api.EnqueueHip(_ctx, _job.handle, _queue.Get());
        if (timed)
        {
            ID3D12CommandList* l[] = { _tsList[slot * 2 + 1].Get() };
            o_ExecuteCommandLists(_queue.Get(), 1, l);
        }
        _queue->Signal(_hipFence.Get(), ++_hipValue);
        if (timed)
            _tsValue[slot] = _hipValue;
        _jobHipValue = _hipValue;
        if (_settings.serialize)
            queue->Wait(_hipFence.Get(), _hipValue);

        if (rc == LMXXF_NR_OK)
        {
            _jobState = Job::Enqueued;
        }
        else
        {
            _stats.error = "EnqueueHip " + std::to_string(rc) + ": " + _rt.LastError();
            _rt.api.CancelUnsubmitted(_ctx, _job.handle);
            _jobState = Job::None;
            _feedValid = false;
        }
    }

    _pendingWait = 0;
    _pendingProducer = false;
    _pendingList = nullptr;
}

void Log(OptiLogLevel level, const char* fmt, ...)
{
    extern const OptiPluginApi* g_api;
    extern OptiPlugin* g_plugin;
    if (g_api == nullptr)
        return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_api->Log(g_plugin, level, buf);
}
