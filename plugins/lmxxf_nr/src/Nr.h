// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Runtime.h"
#include "Shaders.h"
#include "../../../src/plugin_sdk/optiscaler_plugin.h"

using Microsoft::WRL::ComPtr;

enum class Placement : int32_t
{
    BeforeUpscale = 0,
    AfterUpscale = 1,
    Terminal = 2,
};

enum class NetworkSize : int32_t
{
    Tier1080 = 0,
    Native1440 = 1,
    Native = 2,
};

struct NrSettings
{
    bool enabled = true;
    Placement placement = Placement::AfterUpscale;
    NetworkSize size = NetworkSize::Tier1080;
    int32_t model = 0; // 0 Full (71 blocks), 1 Small (31 blocks)
    int32_t passes = 1; // Full only: DLSS5_MULTI_PASS 1..3, the network run N times per frame
    int32_t Passes() const { return model == 0 ? passes : 1; }
    bool History() const { return temporal && Passes() == 1; } // the runtime's history needs one pass
    int32_t style = 0; // NVIDIA NGX style, 0 = NVIDIA's default look
    float strength = 1.0f;
    float uiLow = 0.004f;  // terminal: back buffer vs HUD-less difference where UI starts (encoded units)
    float uiHigh = 0.02f;  // and where it is fully UI
    bool motion = true; // move the one frame old edit with the motion vectors
    bool exposure = true; // pass the game's exposure texture when the runtime takes it
    bool temporal = true; // motion-fed network history when the runtime takes it
    bool serialize = false; // the game queue waits for the network right away (measuring, like inline NR)
    float paperWhite = 200.0f; // HDR output: nits the network's 1.0 maps to
    bool wholeFrameFallback = true; // terminal without a HUD-less colour: NR on the whole frame, UI included
    bool meter = true; // flicker meter on the network's answers (own-list placements)
};

struct NrStats
{
    std::string state = "not started";
    std::string error;
    uint32_t feedWidth = 0, feedHeight = 0;
    uint32_t srcWidth = 0, srcHeight = 0;
    float hipMs = 0.0f;     // median, D3D12 timestamps around EnqueueHip on the session queue
    float networkMs = 0.0f; // median, the runtime's GetTimings (network only), 0 when unavailable
    uint64_t framesRun = 0;
    uint64_t framesSkipped = 0;
    uint32_t frameInfoSize = 0;
    bool temporal = false; // the runtime gets motion vectors and keeps a history
    uint32_t encoding = 1; // of the last source: 0 linear, 1 sRGB, 2 PQ, 3 scRGB, 4 scene HDR
    // Upscale placements: net units per scene unit, its source, and the HDR guard (a few frames old)
    float netScale = 1.0f;
    bool gameExposure = false;
    float guardShare = 0.0f; // share of fed samples above the guard after mapping
    bool bypassed = false;   // the guard tripped: the edit is not applied
    std::string status; // GetStatus
    std::string source; // terminal: what the network is fed and where the edit goes
    // Flicker meter, seq_metrics units: answer change minus input change, mean of the last frames (NAN: none)
    float flkStatic = NAN, flkMc = NAN;
    uint32_t flkFrames = 0;
};

// One lmxxf session on the game's device, driven from the upscale callbacks
class NrEngine
{
  public:
    void Configure(const NrSettings& s); // takes effect at the next frame, recreating the session when needed
    NrSettings Settings();
    NrStats Stats();
    Runtime& GetRuntime() { return _rt; }
    bool RuntimeLoaded() const { return _rt.module != nullptr; }
    bool LoadRuntime(const RuntimePaths& paths);

    void OnUpscale(OptiUpscaleFrame* frame, bool after);
    // Terminal: keeps this frame's motion vectors for the HUD-less NR at present
    void CaptureMotion(OptiUpscaleFrame* frame);
    void OnPresent(OptiPresentFrame* frame);

    // NR on a device of our own (the display card): src is read, the result written to dst. waitAnswer runs the
    // network on this frame and waits for it; otherwise the previous frame's edit is applied (moved by motion).
    struct OwnFrame
    {
        ID3D12Resource* src = nullptr;
        D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12Resource* dst = nullptr;
        D3D12_RESOURCE_STATES dstState = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12Resource* motion = nullptr;
        D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        uint32_t motionW = 0, motionH = 0; // active extent of the vectors (render size)
        float mvScaleX = 1, mvScaleY = 1;
        bool reset = false;
        bool waitAnswer = false;
        uint32_t encoding = 1; // 0 linear, 1 sRGB, 2 PQ, 3 scRGB
    };
    bool RunOwn(ID3D12Device* device, ID3D12CommandQueue* queue, const OwnFrame& in);
    void SettleOwn(); // before presenting a RunOwn frame: its network answer is done
    void SeqIdle(const char* why); // no NR this frame: logs why a requested sequence dump waits (rate-limited)
    bool Shutdown(); // hot reload: GPU idle, worker joined, session destroyed; false if a game list holds our work
    void Quiesce();   // CPU wait for all queued network work: its inputs may be freed after
    // Leaves the session alone: the game card's engine while the display card runs NR
    void SetSuspended(bool s) { _suspended = s; }
    bool Suspended() const { return _suspended; }
    // ExecuteCommandLists detour body
    void OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);

  private:
    enum class State
    {
        Idle,
        Preparing,
        Ready,
        Failed,
    };
    enum class Job
    {
        None,
        Recorded, // RecordInputs is in pendingList, not submitted yet
        Enqueued, // EnqueueHip done, hipValue signals its end on the session queue
    };

    bool InitDevice(ID3D12Device* device, ID3D12CommandQueue* gameQueue);
    // Starts / rebuilds the session for this source size, true when frames can be recorded
    bool Begin(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t srcW, uint32_t srcH);
    void Consume(ID3D12GraphicsCommandList* list, uint64_t* wait, ID3D12Resource** enh);
    void Produce(ID3D12GraphicsCommandList* list, const OptiUpscaleFrame* frame, uint64_t wait);
    bool CreatePipelines();
    bool EnsureTextures(uint32_t srcW, uint32_t srcH, DXGI_FORMAT, uint32_t feedW, uint32_t feedH);
    void Worker(uint32_t feedW, uint32_t feedH, bool recreate);
    bool CreateSession();
    bool WarmUp();
    void DrainJob(); // consumes an enqueued job on the session queue, cancels a recorded one
    void WaitIdle();
    void Fail(const std::string& why);
    bool PrepareJob(ID3D12GraphicsCommandList* list, const OptiUpscaleFrame* frame);
    void ReadTimings();
    void Record(OptiUpscaleFrame* f, bool pre);
    void FeedSize(uint32_t w, uint32_t h, uint32_t* fw, uint32_t* fh);
    void ApplyEnv();
    void Dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const ShaderConstants& c,
                  ID3D12Resource* const srv[4], const DXGI_FORMAT srvFmt[4], ID3D12Resource* uav, DXGI_FORMAT uavFmt,
                  uint32_t w, uint32_t h, ID3D12Resource* norm = nullptr);

    Runtime _rt;
    std::mutex _mutex; // every runtime call, the job state and the stats
    NrSettings _settings;
    NrSettings _active; // what the session was created with
    bool _dirty = false;
    std::atomic<State> _state { State::Idle };
    std::thread _worker;
    NrStats _stats;

    ComPtr<ID3D12Device> _device;
    ComPtr<ID3D12CommandQueue> _queue; // session queue (direct), the runtime's EnqueueHip queue
    ComPtr<ID3D12Fence> _gameFence;    // game queue -> session queue
    ComPtr<ID3D12Fence> _hipFence;     // session queue -> game queue, network done
    uint64_t _gameValue = 0;
    uint64_t _hipValue = 0;
    ComPtr<ID3D12CommandAllocator> _ownAlloc;
    ComPtr<ID3D12GraphicsCommandList> _ownList;
    HANDLE _event = nullptr;

    ComPtr<ID3D12RootSignature> _rootSig;
    ComPtr<ID3D12PipelineState> _psoDown, _psoApply, _psoTerminal, _psoStats;
    // Upscale placements: Stats' 1x1 output and a readback ring of it for the menu
    static constexpr uint32_t kNormSlots = 3;
    ComPtr<ID3D12Resource> _norm, _normReadback;
    const float* _normMapped = nullptr;
    uint64_t _normFrame = 0;
    ID3D12Resource* RunStats(ID3D12GraphicsCommandList* list, const OptiUpscaleFrame* f, ID3D12Resource* src,
                             DXGI_FORMAT srcFmt, uint32_t w, uint32_t h, uint32_t encoding);

    // Terminal placement: own lists on the present queue
    static constexpr uint32_t kPresentLists = 6;
    bool _suspended = false;
    ID3D12GraphicsCommandList* NextOwnList(uint32_t* slot);
    void SubmitOwn(ID3D12CommandQueue* queue, uint32_t slot, ID3D12GraphicsCommandList* list, uint64_t wait);
    uint64_t _answerWait = 0;
    bool EnsureCopies(const D3D12_RESOURCE_DESC& d);
    // Frame dump on <dumpDir>\lmxxf_nr_dump.trigger: the network output, what it was fed, and the result
    void MaybeDump(ID3D12GraphicsCommandList* list, ID3D12Resource* enh, ID3D12Resource* result = nullptr,
                   D3D12_RESOURCE_STATES resultState = D3D12_RESOURCE_STATE_COPY_SOURCE, ID3D12Resource* source = nullptr,
                   ID3D12Resource* hudless = nullptr);
    void FinishDump();
    ComPtr<ID3D12Resource> _dumpTex[5];
    bool _dumpArmed = false;
    uint64_t _dumpGameValue = 0, _dumpOwnValue = 0;
    uint32_t _dumpCount = 0;
  public:
    std::wstring dumpDir;
    std::wstring name = L"game";
  private:
    ComPtr<ID3D12CommandAllocator> _presentAlloc[kPresentLists];
    ComPtr<ID3D12GraphicsCommandList> _presentList;
    ComPtr<ID3D12Fence> _presentFence;
    uint64_t _presentValue[kPresentLists] = {};
    uint64_t _presentCounter = 0;
    ComPtr<ID3D12Resource> _termCopy, _termOut;
    uint64_t _hudlessMissing = 0;
    uint32_t _hudlessStreak = 0; // presents in a row without a usable HUD-less colour
    ComPtr<ID3D12DescriptorHeap> _heap;
    uint32_t _heapCursor = 0, _heapStride = 0;

    ComPtr<ID3D12Resource> _feed;     // network input, RGBA16F
    ComPtr<ID3D12Resource> _srcCopy;  // after upscaling: copy of the output
    ComPtr<ID3D12Resource> _override; // before upscaling: the color the upscaler gets
    D3D12_RESOURCE_STATES _overrideState = D3D12_RESOURCE_STATE_COMMON;
    uint32_t _feedW = 0, _feedH = 0;
    uint32_t _srcW = 0, _srcH = 0;
    DXGI_FORMAT _srcFormat = DXGI_FORMAT_UNKNOWN;

    void* _ctx = nullptr;
    LmxxfNrJob _job {};
    Job _jobState = Job::None;
    uint64_t _jobHipValue = 0;
    std::atomic<ID3D12CommandList*> _pendingList { nullptr }; // game list holding the producer and/or consumer
    ComPtr<ID3D12CommandQueue> _gameQueue; // where that list was last executed
    uint32_t _failures = 0;
    bool _pendingProducer = false;
    uint64_t _pendingWait = 0; // hip fence value that list's consumer needs
    uint64_t _frameId = 0;
    uint32_t _frameInfoSize = 0;
    // Motion vectors for the job being prepared, in feed pixels (temporal runtimes)
    struct MotionIn
    {
        ID3D12Resource* res = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        uint32_t w = 0, h = 0;
        float scaleX = 1, scaleY = 1;
        bool reset = false;
    } _motion;
    bool _feedValid = false; // _feed holds what the enqueued job was fed
    bool _feedNormalised = false; // the feed is exposure-mapped: the runtime gets no exposure
    // History continuity: the last fed frame's upscale id and vector extent
    uint64_t _lastFedUpscale = 0;
    uint32_t _lastMvW = 0, _lastMvH = 0;
    uint32_t _histSrcW = 0, _histSrcH = 0; // source extent the history was built on
    bool _extentReset = false;
    bool _forceFull = false; // warm-up job that moves the runtime's model pin, see WarmUp
    bool _sessionFull = false; // this session's last frame had FULL_NETWORK
    uint32_t _loggedMvFlags = ~0u;
    uint64_t _motionUpscale = 0; // upscale frame of _motion
    void LogMotion(const OptiUpscaleFrame* f);
    bool HistoryBreak(uint64_t upscaleId, uint32_t mvW, uint32_t mvH, bool reset);
    void SetMotion(ID3D12Resource* mv, D3D12_RESOURCE_STATES state, uint32_t w, uint32_t h, float scaleX, float scaleY,
                   uint64_t upscaleId, bool reset);
    // Terminal: the vectors of the last evaluate, copied on the game's list
    ComPtr<ID3D12Resource> _mvCopy;
    struct
    {
        uint64_t frameId = 0;
        uint32_t w = 0, h = 0;
        float scaleX = 1, scaleY = 1;
        bool reset = false;
    } _mvCap;

    // Sequence dump on <dumpDir>\lmxxf_nr_seq.trigger (frame count inside, default 12): per job its input, its
    // vectors and its answer, read back once the last answer was recorded
    struct SeqImage
    {
        ComPtr<ID3D12Resource> buf;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        D3D12_RESOURCE_DESC desc {};
        UINT64 rowBytes = 0;
    };
    struct SeqSlot
    {
        SeqImage fed, mv, enh;
        MotionIn motion;
        uint64_t frame = 0;
        bool temporal = false;
    };
    std::vector<SeqSlot> _seq;
    uint32_t _seqNext = 0;
    int32_t _jobSeq = -1;     // slot of the job in flight
    uint32_t _seqWait = 0;    // frames since the last answer was recorded
    bool _seqActive = false;
    ID3D12CommandQueue* _ownQueue = nullptr; // where our own lists last ran
    // Flicker meter (kMeterSource): one dispatch per consumed answer on our own lists, read back frames later
    static constexpr uint32_t kMeterSlots = 4, kMeterWindow = 60;
    ComPtr<ID3D12PipelineState> _psoMeter;
    ComPtr<ID3D12Resource> _meterPrev, _meterAcc, _meterZero, _meterReadback;
    uint64_t _meterValue[kMeterSlots] = {};
    uint32_t _meterNext = 0;
    bool _meterPrevValid = false;
    float _flkStatic[kMeterWindow] = {}, _flkMc[kMeterWindow] = {};
    uint32_t _flkStaticN = 0, _flkMcN = 0;
    MotionIn _jobMotion; // of the job in flight
    bool _jobReset = false;
    void Meter(ID3D12GraphicsCommandList* list, ID3D12Resource* enh, bool jobMotionValid);
    void MeterRead();
    void SeqPoll();
    void SeqRestart(const char* why);
    static constexpr uint64_t kSeqSettle = 30; // jobs after a rebuild or a history reset before a sequence starts
    uint32_t _seqPending = 0; // frames of a requested sequence not started yet
    uint64_t _readyFrame = 0, _resetFrame = 0;
    DWORD _seqIdleTick = 0;
    const char* _seqIdleWhy = nullptr;
    void SeqCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES state, SeqImage& img);
    void SeqWrite();

    // Timestamps around EnqueueHip on the session queue
    static constexpr uint32_t kTimingSlots = 8;
    ComPtr<ID3D12QueryHeap> _queryHeap;
    ComPtr<ID3D12Resource> _readback;
    ComPtr<ID3D12CommandAllocator> _tsAlloc[kTimingSlots * 2];
    ComPtr<ID3D12GraphicsCommandList> _tsList[kTimingSlots * 2];
    uint64_t _tsValue[kTimingSlots] = {};
    uint64_t _tsFrame = 0;
    double _tsFreq = 0;
    float _hipSamples[61] = {};
    float _netSamples[61] = {};
    uint32_t _hipCount = 0, _netCount = 0;
    uint64_t _lastTimed = 0;
};

NrEngine& Engine();
NrEngine& DisplayEngine();

using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
extern PFN_ExecuteCommandLists o_ExecuteCommandLists;
void Log(OptiLogLevel level, const char* fmt, ...);
