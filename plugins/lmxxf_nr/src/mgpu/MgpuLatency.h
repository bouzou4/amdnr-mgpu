// Per-frame latency probe: QPC times of the proxy present and of fence completions on both cards.
// On when LMXXF_LATENCY_CSV names a file; one CSV row per frame.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <atomic>

namespace Mgpu::Latency
{
enum Col : int
{
    Present = 0, // game's Present reached the proxy
    Submit,      // send copy submitted (after the lead wait)
    GameDone,    // render card finished the game's frame
    Sent,        // send copy done
    RealPresent, // real Present returned on the display card
    DisplayDone, // display queue passed the real Present
    LastExec,    // the game queue's last ExecuteCommandLists before the Present
    Count
};

bool On();
void Mark(uint64_t frame, Col c);
// QPC of the completion of fence >= value, recorded on a watcher thread
void Watch(ID3D12Fence* fence, uint64_t value, uint64_t frame, Col c);
void Skipped(uint64_t frame);
void SetGameQueue(void* queue);
void NoteExecute(void* queue); // from the ExecuteCommandLists detour
void Flush();
} // namespace Mgpu::Latency

namespace Mgpu
{
int64_t QpcTicks();
void PreciseSleepUntil(int64_t qpc); // high resolution timer, the last 1.5 ms spun

// Completion time (QPC) of each value of a fence that counts frames, recorded on its own thread
class RenderClock
{
  public:
    ~RenderClock();
    void Push(ID3D12Fence* fence, uint64_t value);
    // 0 while not done
    int64_t Done(uint64_t value) const;
    int64_t WaitDone(uint64_t value, unsigned ms);

  private:
    static constexpr uint64_t kRing = 64;
    ID3D12Fence* _fence = nullptr;
    struct Slot
    {
        std::atomic<uint64_t> value { 0 };
        std::atomic<int64_t> qpc { 0 };
    } _slot[kRing];
    std::atomic<uint64_t> _pushed { 0 };
    std::atomic<bool> _stop { false };
    void* _thread = nullptr;
    void* _wake = nullptr;
    void* _done = nullptr;
    static unsigned long __stdcall Run(void* self);
};

// Low latency for the render card, which has no present of its own (MgpuLowLatency):
// 1: the proxy Present returns once the render card has finished the previous frame (a real swapchain's throttle);
// 2: also holds the return so that the next frame's work arrives just as the render card frees up.
class Pacer
{
  public:
    // after the frame's signal on the game queue; returns the ms slept
    double Pace(int mode, RenderClock& clock, uint64_t value, int64_t presentQpc);
    double SleepMs() const { return _sleepMs; }
    double QueueMs() const { return _queueMs; }

  private:
    double _sleepMs = 0, _queueMs = 0;
};
} // namespace Mgpu
