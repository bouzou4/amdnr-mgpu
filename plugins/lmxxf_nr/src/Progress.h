// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// Display card progress, read back after a device removal: GPU markers (frame ids written by the queues as each
// stage starts / ends, in readback memory that stays mapped) and the presenter's CPU phase.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>

namespace Progress
{
enum Stage : uint32_t
{
    IngestOut,
    FeedIn,
    FeedOut,
    NetIn,
    NetOut,
    ApplyIn,
    ApplyOut,
    UiIn,
    UiOut,
    StageCount
};

enum Phase : uint32_t
{
    Idle,
    WaitSend,
    WaitReuse,
    Ingest,
    NrRecord,
    AnswerWait,
    UiRecord,
    Settle,
    Present,
    PhaseCount
};

namespace detail
{
constexpr uint32_t kRing = 8;  // session queue marker lists (the ring index stands for a frame)
struct State
{
    std::mutex m;
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb;
    volatile uint32_t* mapped = nullptr;
    // session queue: prerecorded lists writing ring index + 1 into NetIn / NetOut
    D3D12_COMMAND_LIST_TYPE ringType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> ringAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> ringIn[kRing], ringOut[kRing];
    std::atomic<uint64_t> ringFrame[kRing] {};
    uint32_t ringNext = 0;
    std::atomic<uint64_t> frame { 0 };
    std::atomic<uint32_t> phase { Idle };
    std::atomic<uint64_t> lastPresented { 0 };
    std::atomic<int64_t> lastPresentQpc { 0 };
    bool logged = false;
};
inline State& S()
{
    static State s;
    return s;
}
inline int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline void Write(ID3D12GraphicsCommandList* c, uint32_t stage, uint32_t value, bool in)
{
    auto& s = S();
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> c2;
    if (!s.rb || FAILED(c->QueryInterface(IID_PPV_ARGS(&c2))))
        return;
    Microsoft::WRL::ComPtr<ID3D12Device> d;
    if (FAILED(c->GetDevice(IID_PPV_ARGS(&d))) || d.Get() != s.dev.Get()) // the buffer is the display device's
        return;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p { s.rb->GetGPUVirtualAddress() + 4 * stage, value };
    D3D12_WRITEBUFFERIMMEDIATE_MODE m = in ? D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN : D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    c2->WriteBufferImmediate(1, &p, &m);
}
} // namespace detail

// Once per display device (a renewed device replaces it)
inline void Init(ID3D12Device* dev)
{
    auto& s = detail::S();
    std::lock_guard g(s.m);
    if (!dev || s.dev.Get() == dev)
        return;
    s.mapped = nullptr;
    s.rb.Reset();
    s.ringAlloc.Reset();
    for (auto& l : s.ringIn)
        l.Reset();
    for (auto& l : s.ringOut)
        l.Reset();
    s.dev = dev;
    s.logged = false;
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&s.rb))))
        return;
    void* p = nullptr;
    D3D12_RANGE r { 0, 256 };
    if (FAILED(s.rb->Map(0, &r, &p)))
    {
        s.rb.Reset();
        return;
    }
    s.mapped = static_cast<volatile uint32_t*>(p);
    for (uint32_t i = 0; i < 64; i++)
        s.mapped[i] = 0;
}

inline void SetFrame(uint64_t g) { detail::S().frame = g; }
inline void SetPhase(Phase p) { detail::S().phase = p; }
inline void Presented(uint64_t g)
{
    detail::S().lastPresented = g;
    detail::S().lastPresentQpc = detail::Qpc();
}

// Into a list recorded this frame: In at its start, Out at its end
inline void In(ID3D12GraphicsCommandList* c, Stage st) { detail::Write(c, st, uint32_t(detail::S().frame.load()), true); }
inline void Out(ID3D12GraphicsCommandList* c, Stage st) { detail::Write(c, st, uint32_t(detail::S().frame.load()), false); }

// The session queue runs the runtime's lists: markers go in prerecorded lists around them
inline bool RingReady(ID3D12CommandQueue* q)
{
    auto& s = detail::S();
    if (!s.rb || !q)
        return false;
    if (s.ringAlloc)
        return true;
    std::lock_guard g(s.m);
    s.ringType = q->GetDesc().Type;
    if (FAILED(s.dev->CreateCommandAllocator(s.ringType, IID_PPV_ARGS(&s.ringAlloc))))
        return false;
    for (uint32_t i = 0; i < detail::kRing; i++)
    {
        for (int k = 0; k < 2; k++)
        {
            auto& l = k ? s.ringOut[i] : s.ringIn[i];
            if (FAILED(s.dev->CreateCommandList(0, s.ringType, s.ringAlloc.Get(), nullptr, IID_PPV_ARGS(&l))))
            {
                s.ringAlloc.Reset();
                return false;
            }
            detail::Write(l.Get(), k ? NetOut : NetIn, i + 1, k == 0);
            l->Close();
        }
    }
    return true;
}
// The caller executes the returned list (nullptr: no markers) on q before / after the network's lists
inline ID3D12CommandList* NetBefore(ID3D12CommandQueue* q, uint32_t* slot)
{
    auto& s = detail::S();
    *slot = UINT32_MAX;
    Microsoft::WRL::ComPtr<ID3D12Device> d;
    if (!q || FAILED(q->GetDevice(IID_PPV_ARGS(&d))) || d.Get() != s.dev.Get() || !RingReady(q) ||
        q->GetDesc().Type != s.ringType)
        return nullptr;
    *slot = s.ringNext++ % detail::kRing;
    s.ringFrame[*slot] = s.frame.load();
    return s.ringIn[*slot].Get();
}
inline ID3D12CommandList* NetAfter(uint32_t slot)
{
    auto& s = detail::S();
    return slot < detail::kRing ? s.ringOut[slot].Get() : nullptr;
}

inline const char* PhaseName(uint32_t p)
{
    static const char* n[] = { "idle", "send wait", "slot reuse wait", "ingest", "NR record", "answer wait",
                               "UI record", "settle", "Present" };
    return p < PhaseCount ? n[p] : "?";
}

// Once per device: what the display card had started and finished when it was found removed
inline std::string Describe(const char* where)
{
    auto& s = detail::S();
    std::lock_guard g(s.m);
    if (s.logged || !s.mapped)
        return {};
    s.logged = true;
    uint32_t v[StageCount];
    for (uint32_t i = 0; i < StageCount; i++)
        v[i] = s.mapped[i];
    auto net = [&](uint32_t x) -> long long { return x >= 1 && x <= detail::kRing ? (long long) s.ringFrame[x - 1].load() : -1; };
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double sincePresent = s.lastPresentQpc ? double(detail::Qpc() - s.lastPresentQpc) * 1000.0 / double(f.QuadPart) : -1;
    return std::format("display progress ({}): frame {}, presenter in {}, last Present returned frame {} {:.0f} ms ago; "
                       "GPU started/finished: ingest -/{} feed {}/{} network {}/{} apply {}/{} UI {}/{}",
                       where, s.frame.load(), PhaseName(s.phase.load()), s.lastPresented.load(), sincePresent,
                       v[IngestOut], v[FeedIn], v[FeedOut], net(v[NetIn]), net(v[NetOut]), v[ApplyIn], v[ApplyOut], v[UiIn],
                       v[UiOut]);
}
} // namespace Progress
