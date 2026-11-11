// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
#include "Validation.h"

#include "Nr.h"

#include <d3d12sdklayers.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
int g_level = 0;
std::mutex g_mutex;
struct Device
{
    ID3D12Device* device = nullptr;
    char label[24] = {};
    bool removedLogged = false;
    std::unordered_map<int, uint64_t> counts; // per message id
};
// never freed: the callback context outlives the device
std::vector<Device*> g_devices;

const char* Severity(D3D12_MESSAGE_SEVERITY s)
{
    switch (s)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        return "CORRUPTION";
    case D3D12_MESSAGE_SEVERITY_ERROR:
        return "ERROR";
    case D3D12_MESSAGE_SEVERITY_WARNING:
        return "WARNING";
    case D3D12_MESSAGE_SEVERITY_INFO:
        return "INFO";
    default:
        return "MESSAGE";
    }
}

// The first 8 of an id per device, then every power of two: GPU-based validation repeats per dispatch
void CALLBACK OnMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text,
                        void* context)
{
    auto* d = static_cast<Device*>(context);
    // the live object report at process exit
    if (id == D3D12_MESSAGE_ID_UNKNOWN && text != nullptr && (strstr(text, "Live ") != nullptr || strstr(text, "terminating") != nullptr))
        return;
    uint64_t n;
    {
        std::lock_guard lock(g_mutex);
        n = ++d->counts[(int) id];
    }
    if (n > 8 && (n & (n - 1)) != 0)
        return;
    const OptiLogLevel level = severity <= D3D12_MESSAGE_SEVERITY_ERROR     ? OptiLog_Error
                               : severity == D3D12_MESSAGE_SEVERITY_WARNING ? OptiLog_Warn
                                                                            : OptiLog_Info;
    Log(level, "d3d12 %s #%d %s x%llu: %s", d->label, (int) id, Severity(severity), (unsigned long long) n,
        text ? text : "");
}

const char* const kOps[] = {
    "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
    "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
    "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present",
    "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint",
    "AtomicCopyBufferUint64", "ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1",
    "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1", "BuildRaytracingAccelerationStructure",
    "EmitRaytracingAccelerationStructurePostbuildInfo", "CopyRaytracingAccelerationStructure", "DispatchRays",
    "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1",
    "InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame",
    "ResolveEncoderOutputMetadata", "Barrier", "BeginCommandList", "DispatchGraph", "SetProgram",
};
} // namespace

namespace Validation
{
int Level() { return g_level; }

void Enable(int level)
{
    if (level <= 0 || g_level != 0)
        return;
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        Log(OptiLog_Error, "validation: no D3D12 debug layer (install the Graphics Tools optional feature)");
        return;
    }
    debug->EnableDebugLayer();
    ComPtr<ID3D12Debug1> debug1;
    if (level >= 2 && SUCCEEDED(debug.As(&debug1)))
        debug1->SetEnableGPUBasedValidation(TRUE);
    ComPtr<ID3D12Debug5> debug5;
    if (SUCCEEDED(debug.As(&debug5)))
        debug5->SetEnableAutoName(TRUE);
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
    {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
    g_level = level;
    Log(OptiLog_Warn, "validation %d: D3D12 debug layer%s, DRED breadcrumbs and page faults, on every device from here on",
        level, level >= 2 ? ", GPU-based validation" : "");
}

void Attach(ID3D12Device* device, const char* label)
{
    if (g_level == 0 || device == nullptr)
        return;
    Device* d = nullptr;
    {
        std::lock_guard lock(g_mutex);
        for (auto* x : g_devices)
            if (x->device == device)
                return;
        d = new Device();
        d->device = device;
        snprintf(d->label, sizeof(d->label), "%s", label);
        g_devices.push_back(d);
    }
    ComPtr<ID3D12InfoQueue> queue;
    ComPtr<ID3D12InfoQueue1> queue1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
    {
        Log(OptiLog_Warn, "validation: device %s (%p) has no info queue (created before the debug layer was on)", label,
            (void*) device);
        return;
    }
    queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    D3D12_MESSAGE_SEVERITY deny[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };
    D3D12_INFO_QUEUE_FILTER filter {};
    filter.DenyList.NumSeverities = (UINT) std::size(deny);
    filter.DenyList.pSeverityList = deny;
    queue->AddStorageFilterEntries(&filter);
    // What was stored before the callback: device creation up to here
    const UINT64 stored = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < stored; i++)
    {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> buf(size);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (size != 0 && SUCCEEDED(queue->GetMessage(i, m, &size)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            OnMessage(m->Category, m->Severity, m->ID, m->pDescription, d);
    }
    queue->ClearStoredMessages();
    DWORD cookie = 0;
    if (SUCCEEDED(queue.As(&queue1)) &&
        SUCCEEDED(queue1->RegisterMessageCallback(OnMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, d, &cookie)))
        Log(OptiLog_Info, "validation: device %s (%p) messages go to this log", label, (void*) device);
    else
        Log(OptiLog_Warn, "validation: device %s (%p) has no message callback (ID3D12InfoQueue1)", label, (void*) device);
}

bool CheckRemoved(ID3D12Device* device, const char* label)
{
    if (device == nullptr)
        return false;
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
        return false;
    {
        std::lock_guard lock(g_mutex);
        Device* d = nullptr;
        for (auto* x : g_devices)
            if (x->device == device)
                d = x;
        if (d == nullptr)
        {
            d = new Device();
            d->device = device;
            g_devices.push_back(d);
        }
        if (std::exchange(d->removedLogged, true))
            return true;
    }
    Log(OptiLog_Error, "device %s (%p) removed: reason %08X", label, (void*) device, (unsigned) reason);
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return true;
    D3D12_DRED_PAGE_FAULT_OUTPUT pf {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
    {
        Log(OptiLog_Error, "DRED %s: page fault at %llX", label, (unsigned long long) pf.PageFaultVA);
        int n = 0;
        for (auto* a = pf.pHeadExistingAllocationNode; a && n < 32; a = a->pNext, n++)
            Log(OptiLog_Error, "DRED %s:   live allocation '%s' type %d", label, a->ObjectNameA ? a->ObjectNameA : "?",
                (int) a->AllocationType);
        n = 0;
        for (auto* a = pf.pHeadRecentFreedAllocationNode; a && n < 32; a = a->pNext, n++)
            Log(OptiLog_Error, "DRED %s:   freed allocation '%s' type %d", label, a->ObjectNameA ? a->ObjectNameA : "?",
                (int) a->AllocationType);
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc {};
    if (FAILED(dred->GetAutoBreadcrumbsOutput(&bc)))
        return true;
    int lists = 0;
    for (auto* node = bc.pHeadAutoBreadcrumbNode; node && lists < 32; node = node->pNext)
    {
        const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        if (done >= node->BreadcrumbCount)
            continue; // finished
        lists++;
        std::string ops;
        for (UINT i = done > 3 ? done - 3 : 0; i < node->BreadcrumbCount && i < done + 5; i++)
        {
            const UINT op = (UINT) node->pCommandHistory[i];
            const char* name = op < std::size(kOps) ? kOps[op] : "?";
            ops += i == done ? std::string("[") + name + "] " : std::string(name) + " ";
        }
        Log(OptiLog_Error, "DRED %s: list '%s' on queue '%s' stopped at op %u of %u: %s", label,
            node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "?",
            node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?", done, node->BreadcrumbCount, ops.c_str());
    }
    if (lists == 0)
        Log(OptiLog_Error, "DRED %s: no unfinished list in the breadcrumbs", label);
    return true;
}
} // namespace Validation
