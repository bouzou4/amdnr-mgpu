// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Legacy mode, NR side: an x64 process beside a 32-bit game. It opens the game proxy's shared frame texture and fence,
// estimates motion from the colour alone, runs NR on the whole frame (NrEngine::RunOwn) and hands the frame back.
// Usage: lmxxf_legacy_host.exe <game pid> <ini>
#include "../../src/Nr.h"
#include "../../src/mgpu/MgpuCore.h"
#include "../LegacyShared.h"
#include "OpticalFlow.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <string>

const OptiPluginApi* g_api = nullptr;
OptiPlugin* g_plugin = nullptr;

namespace
{
FILE* g_log = nullptr;
std::wstring g_ini, g_dumpDir;

void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l)
{
    q->ExecuteCommandLists(n, l);
}

void HostLog(OptiPlugin*, OptiLogLevel level, const char* msg)
{
    if (g_log == nullptr)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    static const char* lv[] = { "debug", "info", "warn", "error" };
    fprintf(g_log, "%02d:%02d:%02d.%03d [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            lv[std::clamp<int>(level, 0, 3)], msg);
    fflush(g_log);
}

int IniInt(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"Host", key, def, g_ini.c_str()); }
std::wstring IniStr(const wchar_t* sec, const wchar_t* key)
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(sec, key, L"", buf, MAX_PATH * 2, g_ini.c_str());
    return buf;
}
float IniFloat(const wchar_t* key, float def)
{
    const std::wstring s = IniStr(L"Host", key);
    return s.empty() ? def : (float) _wtof(s.c_str());
}

// Vectors: 0 none, 1 zero (static history), 2 optical flow. History itself is the Temporal key; with it off the
// vectors still go to the flicker meter and the sequence dumps.
int g_flow = 2;

NrSettings ReadSettings()
{
    NrSettings s;
    s.enabled = IniInt(L"Enabled", 1) != 0;
    s.placement = Placement::Terminal;
    s.size = (NetworkSize) std::clamp(IniInt(L"NetworkSize", 0), 0, 2);
    s.model = std::clamp(IniInt(L"Model", 1), 0, 1);
    s.passes = std::clamp(IniInt(L"MultiPass", 1), 1, 3);
    s.style = std::clamp(IniInt(L"Style", 0), 0, 2);
    s.strength = std::clamp(IniFloat(L"Strength", 1.0f), 0.0f, 2.0f);
    s.temporal = IniInt(L"Temporal", 1) != 0;
    s.meter = IniInt(L"FlickerMeter", 1) != 0;
    g_flow = std::clamp(IniInt(L"OpticalFlow", 2), 0, 2);
    return s;
}

// <DumpDir>\lmxxf_nr_settings.trigger: Key=Value lines (Model, NetworkSize, MultiPass, Temporal, Strength, Enabled,
// OpticalFlow), applied live
void PollSettings(NrSettings& s)
{
    static uint32_t n = 0;
    if (g_dumpDir.empty() || n++ % 30 != 0)
        return;
    const std::wstring path = g_dumpDir + L"\\lmxxf_nr_settings.trigger";
    std::ifstream f(path);
    if (!f)
        return;
    std::string line, applied;
    while (std::getline(f, line))
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq);
        const float v = (float) atof(line.c_str() + eq + 1);
        if (k == "Model")
            s.model = std::clamp(int(v), 0, 1);
        else if (k == "NetworkSize")
            s.size = (NetworkSize) std::clamp(int(v), 0, 2);
        else if (k == "MultiPass")
            s.passes = std::clamp(int(v), 1, 3);
        else if (k == "Temporal")
            s.temporal = v != 0;
        else if (k == "Strength")
            s.strength = std::clamp(v, 0.0f, 2.0f);
        else if (k == "Enabled")
            s.enabled = v != 0;
        else if (k == "OpticalFlow")
            g_flow = std::clamp(int(v), 0, 2);
        else
            continue;
        applied += line + " ";
    }
    f.close();
    DeleteFileW(path.c_str());
    Engine().Configure(s);
    Log(OptiLog_Info, "settings trigger: %s(flow %d, temporal %d)", applied.c_str(), g_flow, s.temporal);
}

template <typename T> T* Open(ID3D12Device* dev, const wchar_t* name)
{
    HANDLE h = nullptr;
    if (FAILED(dev->OpenSharedHandleByName(name, GENERIC_ALL, &h)))
        return nullptr;
    T* obj = nullptr;
    dev->OpenSharedHandle(h, IID_PPV_ARGS(&obj));
    CloseHandle(h);
    return obj;
}
} // namespace

// No game lists here: every list the engine records is submitted by RunOwn itself
bool InstallQueueHook(ID3D12Device*)
{
    o_ExecuteCommandLists = Execute;
    return true;
}

namespace Mgpu
{
void Log(int level, const std::string& s) { ::Log((OptiLogLevel) std::clamp(level + 1, 0, 3), "%s", s.c_str()); }
} // namespace Mgpu

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
        return 2;
    const DWORD gamePid = (DWORD) _wtoi(argv[1]);
    g_ini = argv[2];
    std::wstring logDir = IniStr(L"Legacy", L"LogDir");
    if (logDir.empty())
        logDir = g_ini.substr(0, g_ini.find_last_of(L"\\/"));
    _wfopen_s(&g_log, (logDir + L"\\lmxxf_legacy_host.log").c_str(), L"w");
    static OptiPluginApi api {};
    api.size = sizeof(api);
    api.Log = HostLog;
    g_api = &api;
    o_ExecuteCommandLists = Execute;
    Log(OptiLog_Info, "lmxxf legacy host: game pid %lu", gamePid);

    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
    wchar_t name[128];
    swprintf_s(name, Legacy::kBlockName, gamePid);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    auto* block = map ? (Legacy::Block*) MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Legacy::Block)) : nullptr;
    if (game == nullptr || block == nullptr || block->magic != Legacy::kMagic || block->version != Legacy::kVersion)
    {
        Log(OptiLog_Error, "no game block");
        return 3;
    }
    block->hostPid = GetCurrentProcessId();

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    const LUID luid { block->adapterLuidLow, block->adapterLuidHigh };
    if (!factory || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
    {
        Log(OptiLog_Error, "adapter of the game not found");
        return 4;
    }
    DXGI_ADAPTER_DESC1 ad {};
    adapter->GetDesc1(&ad);
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))) ||
        FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
    {
        Log(OptiLog_Error, "device failed");
        return 5;
    }
    swprintf_s(name, Legacy::kFenceName, gamePid);
    ComPtr<ID3D12Fence> fence;
    fence.Attach(Open<ID3D12Fence>(dev.Get(), name));
    if (!fence)
    {
        Log(OptiLog_Error, "shared fence not opened");
        return 6;
    }
    Log(OptiLog_Info, "device on %ls", ad.Description);

    g_dumpDir = IniStr(L"Host", L"DumpDir");
    if (!g_dumpDir.empty())
        CreateDirectoryW(g_dumpDir.c_str(), nullptr);
    RuntimePaths paths;
    paths.extra = IniStr(L"Host", L"RuntimeDir");
    paths.modules = IniStr(L"Host", L"ModulesDir");
    paths.weights = IniStr(L"Host", L"WeightsDir");
    NrSettings settings = ReadSettings();
    auto& nr = Engine();
    nr.name = L"legacy";
    nr.dumpDir = g_dumpDir;
    nr.Configure(settings);
    const bool loaded = nr.LoadRuntime(paths);
    Log(OptiLog_Info, "runtime %s, flow %d, temporal %d, model %d", loaded ? "loaded" : "NOT loaded", g_flow,
        settings.temporal, settings.model);

    OpticalFlow of;
    const bool ofOk = of.Init(dev.Get(), queue.Get());
    if (!ofOk)
        Log(OptiLog_Error, "optical flow init failed: no motion vectors");

    ComPtr<ID3D12CommandAllocator> alloc[4];
    for (auto& a : alloc)
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    ComPtr<ID3D12GraphicsCommandList> list;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ComPtr<ID3D12Fence> own;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own));
    uint64_t ownValue = 0, allocValue[4] = {};
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr), ownEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ComPtr<ID3D12Resource> frame;
    uint32_t generation = 0;
    uint64_t last = 0, processed = 0, nrFrames = 0;
    block->hostState = loaded ? 1 : 2;
    MemoryBarrier();
    block->hostReady = 1;
    ULONGLONG lastLog = GetTickCount64();
    bool reset = true;

    while (WaitForSingleObject(game, 0) == WAIT_TIMEOUT)
    {
        const uint64_t want = 2 * (last + 1) - 1;
        if (fence->GetCompletedValue() < want)
        {
            fence->SetEventOnCompletion(want, ev);
            if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0)
                continue;
        }
        const uint64_t n = last + 1;
        if (block->generation != generation)
        {
            generation = block->generation;
            swprintf_s(name, Legacy::kFrameName, gamePid, generation);
            frame.Reset();
            frame.Attach(Open<ID3D12Resource>(dev.Get(), name));
            block->hostGeneration = generation;
            of.Invalidate();
            reset = true;
            if (frame)
            {
                const auto d = frame->GetDesc();
                Log(OptiLog_Info, "frame texture %llux%u format %u, generation %u", d.Width, d.Height, d.Format,
                    generation);
            }
            else
                Log(OptiLog_Error, "frame texture generation %u not opened", generation);
        }
        if (block->reset)
            block->reset = 0, reset = true;
        PollSettings(settings);

        if (frame)
        {
            // Motion from this frame and the last
            const uint32_t i = uint32_t(processed % 4);
            if (own->GetCompletedValue() < allocValue[i])
            {
                own->SetEventOnCompletion(allocValue[i], ownEv);
                WaitForSingleObject(ownEv, 1000);
            }
            alloc[i]->Reset();
            list->Reset(alloc[i].Get(), nullptr);
            queue->Wait(fence.Get(), want);
            bool haveMv = false;
            if (ofOk && g_flow > 0)
                haveMv = of.Run(list.Get(), frame.Get(), D3D12_RESOURCE_STATE_COMMON, g_flow == 1);
            list->Close();
            ID3D12CommandList* l[] = { list.Get() };
            queue->ExecuteCommandLists(1, l);
            allocValue[i] = ++ownValue;
            queue->Signal(own.Get(), ownValue);

            NrEngine::OwnFrame f;
            f.src = f.dst = frame.Get();
            f.srcState = f.dstState = D3D12_RESOURCE_STATE_COMMON;
            if (haveMv)
            {
                f.motion = of.Vectors();
                f.motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                f.motionW = of.Width();
                f.motionH = of.Height();
            }
            f.reset = reset;
            f.waitAnswer = true;
            f.encoding = 1;
            if (nr.RunOwn(dev.Get(), queue.Get(), f))
                ++nrFrames, reset = false;
        }
        queue->Signal(fence.Get(), 2 * n);
        last = n;
        ++processed;

        const ULONGLONG now = GetTickCount64();
        if (now - lastLog >= 10000)
        {
            const auto st = nr.Stats();
            block->hostMs = st.hipMs;
            block->ofMs = of.GpuMs();
            block->flkStatic = st.flkStatic;
            block->flkMc = st.flkMc;
            block->hostState = st.state == "running" || nrFrames ? 1 : 2;
            snprintf(block->status, sizeof(block->status), "%s %ux%u", st.state.c_str(), st.feedWidth,
                     st.feedHeight);
            Log(OptiLog_Info,
                "frames %llu, NR %llu, state %s %s, feed %ux%u, hip %.2f ms, net %.2f ms, of %.3f ms, flk still %.5f "
                "moving %.5f (%u), temporal %d flow %d",
                processed, nrFrames, st.state.c_str(), st.error.c_str(), st.feedWidth, st.feedHeight, st.hipMs,
                st.networkMs, of.GpuMs(), st.flkStatic, st.flkMc, st.flkFrames, st.temporal, g_flow);
            lastLog = now;
        }
    }
    Log(OptiLog_Info, "game exited, %llu frames, %llu with NR", processed, nrFrames);
    nr.Quiesce();
    TerminateProcess(GetCurrentProcess(), 0); // skip HIP teardown at exit
    return 0;
}
