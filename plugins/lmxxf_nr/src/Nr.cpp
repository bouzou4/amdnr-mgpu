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

#include <algorithm>
#include <cstdarg>
#include <cstdio>

PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;

namespace
{
constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kHeapSlots = 64;
constexpr uint32_t kSlotDescriptors = 5;

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

bool IsExposureFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R16_TYPELESS ||
           f == DXGI_FORMAT_R32_TYPELESS;
}
} // namespace

NrEngine& Engine()
{
    static NrEngine engine;
    return engine;
}

NrEngine& DisplayEngine()
{
    static NrEngine engine;
    return engine;
}

bool NrEngine::LoadRuntime(const std::wstring& extraDir, const std::wstring& exeDir, const std::wstring& optiDir)
{
    std::lock_guard lock(_mutex);
    if (_rt.Load(extraDir, exeDir, optiDir))
    {
        Log(OptiLog_Info,
            "runtime %ls (%s, %zu exports, %s table %u bytes, max input %ux%u, hip_ready %u); env: free_res %d, style %d, "
            "multi_pass %d",
            _rt.dllPath.c_str(), _rt.LayoutName(), _rt.exports, _rt.hasTimings ? "full" : "v1", _rt.tableSize,
            _rt.caps.max_input_width, _rt.caps.max_input_height, _rt.caps.hip_ready, _rt.envFreeRes, _rt.envStyle,
            _rt.envMultiPass);
        return true;
    }
    _stats.state = "no runtime";
    _stats.error = _rt.error;
    Log(OptiLog_Warn, "%s", _rt.error.c_str());
    return false;
}

void NrEngine::Configure(const NrSettings& s)
{
    std::lock_guard lock(_mutex);
    _settings = s;
    _dirty = s.size != _active.size || s.model != _active.model || s.style != _active.style;
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
        SetRuntimeEnv("DLSS5_STYLE", styles[std::clamp(_settings.style, 0, 2)]);
    }
    if (_rt.envMultiPass)
        SetRuntimeEnv("DLSS5_MULTI_PASS", _settings.model == 1 ? "0.5" : "1");
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

    ComPtr<ID3DBlob> down, apply, terminal, err;
    if (FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "Down", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &down, &err)) ||
        FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "Apply", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &apply, &err)) ||
        FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "lmxxf_nr", nullptr, nullptr, "ApplyTerminal",
                       "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &terminal, &err)))
    {
        Fail(std::string("shader compile failed: ") + (err ? (const char*) err->GetBufferPointer() : "?"));
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 };
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

    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSlots * kSlotDescriptors,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    if (FAILED(_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
        return Fail("descriptor heap creation failed"), false;
    _heapStride = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void NrEngine::Dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const ShaderConstants& c,
                        ID3D12Resource* const srv[4], const DXGI_FORMAT srvFmt[4], ID3D12Resource* uav,
                        DXGI_FORMAT uavFmt, uint32_t w, uint32_t h)
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
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud {};
    ud.Format = UavFormat(uavFmt);
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(uav, nullptr, &ud, { cpu.ptr + SIZE_T(4) * _heapStride });

    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_rootSig.Get());
    list->SetPipelineState(pso);
    list->SetComputeRoot32BitConstants(0, sizeof(ShaderConstants) / 4, &c, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, { gpu.ptr + UINT64(4) * _heapStride });
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
    return true;
}

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
    fi.flags = 0;
    fi.transfer_strength = 1.0f;
    fi.color_strength = 1.0f;
    fi.model_scale = 1.0f;
    fi.pre_exposure = 1.0f;
    fi.exposure_scale = 1.0f;
    if (frame != nullptr && _settings.exposure && frame->exposure != nullptr)
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
    if (_settings.temporal && _rt.temporal != 0 && _motion.res != nullptr)
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
        t.history_reset = _motion.reset ? 1 : 0;
        t.passes = 1;
        t.exposure = fi.exposure;
        t.exposure_state = fi.exposure_state;
        t.pre_exposure = fi.pre_exposure;
        t.exposure_scale = fi.exposure_scale;
        for (uint32_t size : { (uint32_t) sizeof(t), 120u })
        {
            if (_rt.temporalSize != 0 && size != _rt.temporalSize)
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
        if (_rt.temporal != 1)
        {
            Log(OptiLog_Info, "runtime takes no motion-fed history (%s): single-frame NR", _rt.LastError().c_str());
            _rt.temporal = 0;
            _stats.temporal = false;
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
        if (f->GetCompletedValue() < v)
        {
            f->SetEventOnCompletion(v, _event);
            WaitForSingleObject(_event, 10000);
        }
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
    _ownAlloc->Reset();
    _ownList->Reset(_ownAlloc.Get(), nullptr);
    _motion = {};
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
    Log(OptiLog_Info, "network ready for %ux%u in %lu ms", _feedW, _feedH, GetTickCount() - t0);
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
        _state = State::Ready;
    }
    _mutex.unlock();
}

void NrEngine::ReadTimings()
{
    FinishDump();
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
        return;

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
    if (produced)
    {
        _jobState = Job::Recorded;
        _feedValid = true;
        _stats.framesRun++;
        _failures = 0;
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

    // Source of this frame (what we feed and what the edit is added to) and where the result goes
    ID3D12Resource* src = nullptr;
    ID3D12Resource* target = nullptr;
    D3D12_RESOURCE_DESC srcDesc {};
    if (pre)
    {
        src = color;
        srcDesc = color->GetDesc();
        if (!_override || _override->GetDesc().Width != srcDesc.Width || _override->GetDesc().Height != srcDesc.Height)
            _override = Texture(_device.Get(), (uint32_t) srcDesc.Width, srcDesc.Height,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, true, colorState);
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

        const D3D12_RESOURCE_STATES targetState = pre ? colorState : outputState;
        Barrier(list, target, targetState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* srv[4] = { src, enh, _feed.Get(), c.useMv ? mv : nullptr };
        const DXGI_FORMAT fmt[4] = { srcDesc.Format, ed.Format, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     mv ? mv->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT };
        Dispatch(list, _psoApply.Get(), c, srv, fmt, target, pre ? DXGI_FORMAT_R16G16B16A16_FLOAT : srcDesc.Format,
                 w, h);
        UavBarrier(list, target);
        Barrier(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, targetState);
        MaybeDump(list, enh, target, targetState);
        if (pre)
            f->colorOverride = target;
    }

    // Motion vectors of this frame for a temporal runtime, scaled to feed pixels
    _motion = {};
    if (mv != nullptr)
    {
        const bool lowResMv = !(f->flags & OptiUpscale_DisplayResMV);
        const float spaceW = float(lowResMv ? f->renderWidth : f->outputWidth);
        const float spaceH = float(lowResMv ? f->renderHeight : f->outputHeight);
        _motion.res = mv;
        _motion.state = D3D12_RESOURCE_STATES(f->motionVectorsState);
        _motion.w = (uint32_t) spaceW;
        _motion.h = (uint32_t) spaceH;
        _motion.scaleX = f->mvScaleX * float(_feedW) / spaceW;
        _motion.scaleY = f->mvScaleY * float(_feedH) / spaceH;
        _motion.reset = (f->flags & OptiUpscale_Reset) != 0;
    }

    // Feed this frame: the source before any edit
    {
        ShaderConstants c {};
        c.sizeX = _feedW;
        c.sizeY = _feedH;
        c.srcUvScaleX = float(w) / float(srcDesc.Width);
        c.srcUvScaleY = float(h) / float(srcDesc.Height);
        Barrier(list, _feed.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* srv[4] = { src, nullptr, nullptr, nullptr };
        const DXGI_FORMAT fmt[4] = { srcDesc.Format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
        Dispatch(list, _psoDown.Get(), c, srv, fmt, _feed.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, _feedW, _feedH);
        UavBarrier(list, _feed.Get());
        Barrier(list, _feed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    }

    Produce(list, f, wait);
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
    if (f->hudless == nullptr)
    {
        if (_hudlessMissing++ % 600 == 0)
            Log(OptiLog_Info, "terminal: no HUD-less colour this frame (%llu frames so far)",
                (unsigned long long) _hudlessMissing);
        _stats.framesSkipped++;
        return;
    }

    auto* bb = static_cast<ID3D12Resource*>(f->backBuffer);
    auto* hud = static_cast<ID3D12Resource*>(f->hudless);
    auto* queue = static_cast<ID3D12CommandQueue*>(f->queue);
    const auto bd = bb->GetDesc();
    const uint32_t w = (uint32_t) bd.Width, h = bd.Height;

    if (!Begin(static_cast<ID3D12Device*>(f->device), queue, w, h))
        return;

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
    const auto hudState = D3D12_RESOURCE_STATES(f->hudlessState);
    const uint32_t encoding = f->colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2
                              : f->colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709  ? 0
                                                                                           : 1;

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
        c.uiLow = _settings.uiLow;
        c.uiHigh = _settings.uiHigh;
        c.encoding = encoding;
        ID3D12Resource* srv[4] = { _termCopy.Get(), enh, _feed.Get(), hud };
        const DXGI_FORMAT fmt[4] = { bd.Format, ed.Format, DXGI_FORMAT_R16G16B16A16_FLOAT, hud->GetDesc().Format };
        Dispatch(list, _psoTerminal.Get(), c, srv, fmt, _termOut.Get(), bd.Format, w, h);
        Barrier(list, _termOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(bb, _termOut.Get());
        Barrier(list, bb, D3D12_RESOURCE_STATE_COPY_DEST, bbState);
        MaybeDump(list, enh, bb, bbState);
    }

    // Feed the HUD-less colour, linearised (no motion vectors at present time)
    _motion = {};
    {
        const auto hd = hud->GetDesc();
        ShaderConstants c {};
        c.sizeX = _feedW;
        c.sizeY = _feedH;
        c.srcUvScaleX = float(w) / float(hd.Width);
        c.srcUvScaleY = float(h) / float(hd.Height);
        c.encoding = encoding;
        Barrier(list, _feed.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* srv[4] = { hud, nullptr, nullptr, nullptr };
        const DXGI_FORMAT fmt[4] = { hd.Format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
        Dispatch(list, _psoDown.Get(), c, srv, fmt, _feed.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, _feedW, _feedH);
        UavBarrier(list, _feed.Get());
        Barrier(list, _feed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    }
    Barrier(list, hud, kRead, hudState);

    Produce(list, nullptr, wait);
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
    if (_presentFence->GetCompletedValue() < _presentValue[*slot])
    {
        _presentFence->SetEventOnCompletion(_presentValue[*slot], _event);
        WaitForSingleObject(_event, 1000);
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

bool NrEngine::EnsureCopies(const D3D12_RESOURCE_DESC& bd)
{
    if (_termCopy && _termCopy->GetDesc().Width == bd.Width && _termCopy->GetDesc().Height == bd.Height &&
        _termCopy->GetDesc().Format == bd.Format)
        return true;
    _termCopy = Texture(_device.Get(), (uint32_t) bd.Width, bd.Height, bd.Format, false, kRead);
    _termOut = Texture(_device.Get(), (uint32_t) bd.Width, bd.Height, UavFormat(bd.Format), true,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!_termCopy || !_termOut)
    {
        Fail("copy textures failed for format " + std::to_string(bd.Format));
        return false;
    }
    return true;
}

bool NrEngine::RunOwn(ID3D12Device* device, ID3D12CommandQueue* queue, const OwnFrame& in)
{
    if (!RuntimeLoaded() || in.src == nullptr || in.dst == nullptr || !Settings().enabled)
        return false;
    const auto sd = in.src->GetDesc();
    const uint32_t w = (uint32_t) sd.Width, h = sd.Height;
    if (!Begin(device, queue, w, h))
        return false;

    std::unique_lock lock(_mutex);
    if (_state != State::Ready || !EnsureCopies(sd))
        return false;
    _stats.srcWidth = w;
    _stats.srcHeight = h;
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
        c.encoding = in.encoding;
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
        MaybeDump(list, enh, in.dst, in.dstState);
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
            wrote = true;
        }
        lock.unlock();
        SubmitOwn(queue, slot, list, wait);
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
    feed(list);
    Produce(list, nullptr, wait);
    lock.unlock();
    SubmitOwn(queue, slot, list, wait);
    return wrote;
}

void NrEngine::MaybeDump(ID3D12GraphicsCommandList* list, ID3D12Resource* enh, ID3D12Resource* result,
                         D3D12_RESOURCE_STATES resultState)
{
    if (_dumpArmed || dumpDir.empty() || enh == nullptr || (_dumpCount++ % 30) != 0)
        return;
    const std::wstring trigger = dumpDir + L"\\lmxxf_nr_dump.trigger";
    if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    DeleteFileW(trigger.c_str());

    ID3D12Resource* src[3] = { enh, _feed.Get(), result };
    const D3D12_RESOURCE_STATES st[3] = { kRead, kRead, resultState };
    for (int i = 0; i < 3; i++)
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
    Log(OptiLog_Info, "dump recorded (%ls)", name.c_str());
}

void NrEngine::FinishDump()
{
    if (!_dumpArmed)
        return;
    const bool ownDone = _presentFence && _dumpOwnValue != 0 && _presentFence->GetCompletedValue() >= _dumpOwnValue;
    const bool gameDone = _dumpGameValue != 0 && _gameFence->GetCompletedValue() >= _dumpGameValue;
    if (!ownDone && !gameDone)
        return;
    const wchar_t* names[3] = { L"enh", L"fed", L"result" };
    for (int i = 0; i < 3; i++)
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
