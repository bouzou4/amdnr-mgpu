// CPU wait on a D3D12 fence before reusing what the GPU may still read (allocator reset, resource release) or before a
// Present. No fixed cap: a hung GPU gets removed by TDR, which completes every fence. Returns false only when the
// device is gone, abort() asked to stop, or after maxMs; the caller must then skip the reuse / present.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <chrono>

inline bool GpuWaitDone(ID3D12Fence* f, UINT64 v, DWORD maxMs = 60000, bool (*abort)(void*) = nullptr, void* ctx = nullptr,
                        double* waitedMs = nullptr)
{
    if (waitedMs)
        *waitedMs = 0;
    if (!f || f->GetCompletedValue() >= v)
        return true;
    const auto t0 = std::chrono::steady_clock::now();
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool done = false;
    if (e && SUCCEEDED(f->SetEventOnCompletion(v, e)))
    {
        Microsoft::WRL::ComPtr<ID3D12Device> dev;
        f->GetDevice(IID_PPV_ARGS(&dev));
        for (;;)
        {
            WaitForSingleObject(e, 250);
            if (f->GetCompletedValue() >= v)
            {
                done = true;
                break;
            }
            if ((dev && FAILED(dev->GetDeviceRemovedReason())) || (abort && abort(ctx)))
                break;
            if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(maxMs))
                break;
        }
    }
    if (e)
        CloseHandle(e);
    if (waitedMs)
        *waitedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // a removed device completes its fences (UINT64_MAX)
    return done || f->GetCompletedValue() >= v;
}
