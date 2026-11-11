#include "MgpuLatency.h"
#include <wrl/client.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Mgpu::Latency
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint64_t kRing = 256, kLag = 32; // a row is written kLag frames after its present
struct Row
{
    uint64_t frame = 0;
    int64_t t[Count] {};
    bool skipped = false;
};

struct Watcher
{
    struct Item
    {
        ComPtr<ID3D12Fence> f;
        uint64_t value, frame;
        Col c;
    };
    std::mutex m;
    std::condition_variable cv;
    std::deque<Item> q;
    std::thread t;
};

struct Probe
{
    FILE* fp = nullptr;
    std::mutex m;
    Row ring[kRing];
    uint64_t written = 0;
    std::mutex wm;
    std::map<ID3D12Fence*, Watcher*> watchers;
};

int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

Probe* Get()
{
    static Probe* p = []() -> Probe*
    {
        char path[MAX_PATH] {};
        if (!GetEnvironmentVariableA("LMXXF_LATENCY_CSV", path, MAX_PATH))
        {
            // games started by a launcher: the path in lmxxf_latency_csv.txt beside the exe
            char exe[MAX_PATH] {};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string f = exe;
            f = f.substr(0, f.find_last_of("\\/") + 1) + "lmxxf_latency_csv.txt";
            FILE* in = fopen(f.c_str(), "r");
            if (!in)
                return nullptr;
            const bool ok = fgets(path, MAX_PATH, in) != nullptr;
            fclose(in);
            if (!ok)
                return nullptr;
            path[strcspn(path, "\r\n")] = 0;
        }
        auto* pr = new Probe;
        // one file per process: a launcher or crash handler loading the same dll must not truncate it
        pr->fp = fopen((std::string(path) + "." + std::to_string(GetCurrentProcessId())).c_str(), "w");
        if (!pr->fp)
            return nullptr;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        fprintf(pr->fp, "# qpc_freq=%lld\nframe,present,submit,game_done,sent,real_present,display_done,last_exec,skipped\n",
                f.QuadPart);
        return pr;
    }();
    return p;
}

void WriteRow(Probe* p, const Row& r)
{
    fprintf(p->fp, "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d\n", r.frame, r.t[0], r.t[1], r.t[2], r.t[3], r.t[4],
            r.t[5], r.t[6], r.skipped ? 1 : 0);
}

void Set(Probe* p, uint64_t frame, Col c, int64_t t)
{
    std::lock_guard g(p->m);
    auto& r = p->ring[frame % kRing];
    if (r.frame != frame)
        return;
    r.t[c] = t;
}
} // namespace

bool On() { return Get() != nullptr; }

static std::atomic<void*> s_gameQueue { nullptr };
static std::atomic<int64_t> s_lastExec { 0 };
void SetGameQueue(void* queue) { s_gameQueue = queue; }
void NoteExecute(void* queue)
{
    if (queue != nullptr && queue == s_gameQueue.load())
        s_lastExec = Qpc();
}

void Mark(uint64_t frame, Col c)
{
    auto* p = Get();
    if (!p)
        return;
    const int64_t t = Qpc();
    if (c == Present)
    {
        std::lock_guard g(p->m);
        // rows kLag behind are complete
        while (p->written + kLag < frame)
        {
            const uint64_t f = ++p->written;
            auto& r = p->ring[f % kRing];
            if (r.frame == f)
                WriteRow(p, r);
        }
        auto& r = p->ring[frame % kRing];
        r = {};
        r.frame = frame;
        r.t[Present] = t;
        r.t[LastExec] = s_lastExec.load();
        if (frame % 30 == 0)
            fflush(p->fp);
        return;
    }
    Set(p, frame, c, t);
}

void Skipped(uint64_t frame)
{
    auto* p = Get();
    if (!p)
        return;
    std::lock_guard g(p->m);
    auto& r = p->ring[frame % kRing];
    if (r.frame == frame)
        r.skipped = true;
}

void Watch(ID3D12Fence* fence, uint64_t value, uint64_t frame, Col c)
{
    auto* p = Get();
    if (!p || !fence)
        return;
    Watcher* w;
    {
        std::lock_guard g(p->wm);
        auto& slot = p->watchers[fence];
        if (!slot)
        {
            slot = new Watcher;
            Watcher* nw = slot;
            nw->t = std::thread(
                [p, nw]
                {
                    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    for (;;)
                    {
                        Watcher::Item it;
                        {
                            std::unique_lock lk(nw->m);
                            nw->cv.wait(lk, [&] { return !nw->q.empty(); });
                            it = std::move(nw->q.front());
                            nw->q.pop_front();
                        }
                        if (it.f->GetCompletedValue() < it.value && SUCCEEDED(it.f->SetEventOnCompletion(it.value, ev)))
                            WaitForSingleObject(ev, 2000);
                        Set(p, it.frame, it.c, Qpc());
                    }
                });
            nw->t.detach();
        }
        w = slot;
    }
    {
        std::lock_guard g(w->m);
        w->q.push_back({ fence, value, frame, c });
    }
    w->cv.notify_one();
}

void Flush()
{
    auto* p = Get();
    if (!p)
        return;
    std::lock_guard g(p->m);
    fflush(p->fp);
}
} // namespace Mgpu::Latency

namespace Mgpu
{
namespace
{
int64_t QpcNow()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double QpcMs(int64_t d)
{
    static const double f = []
    {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return double(x.QuadPart);
    }();
    return double(d) * 1000.0 / f;
}
// high resolution timer, the last 1.5 ms spun
void SleepUntil(int64_t until)
{
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const double left = QpcMs(until - QpcNow());
    if (left > 1.5 && timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -LONGLONG((left - 1.5) * 10000.0);
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer, 100);
    }
    while (QpcNow() < until)
        YieldProcessor();
}
} // namespace

int64_t QpcTicks() { return QpcNow(); }
void PreciseSleepUntil(int64_t qpc) { SleepUntil(qpc); }

RenderClock::~RenderClock()
{
    if (_thread)
    {
        _stop = true;
        SetEvent(_wake);
        WaitForSingleObject(_thread, 3000);
        CloseHandle(_thread);
        CloseHandle(_wake);
        CloseHandle(_done);
    }
}

unsigned long __stdcall RenderClock::Run(void* p)
{
    auto* self = (RenderClock*) p;
    uint64_t seen = 0;
    while (!self->_stop)
    {
        const uint64_t v = self->_pushed.load();
        if (v <= seen)
        {
            WaitForSingleObject(self->_wake, 50);
            continue;
        }
        // values come one per frame: wait for the oldest not seen
        const uint64_t next = seen + 1;
        if (self->_fence->GetCompletedValue() < next)
        {
            self->_fence->SetEventOnCompletion(next, self->_done);
            if (WaitForSingleObject(self->_done, 200) != WAIT_OBJECT_0)
                continue;
        }
        auto& s = self->_slot[next % kRing];
        s.qpc = QpcNow();
        s.value = next;
        seen = next;
    }
    return 0;
}

void RenderClock::Push(ID3D12Fence* fence, uint64_t value)
{
    if (!_thread)
    {
        _fence = fence;
        _wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _thread = CreateThread(nullptr, 0, Run, this, 0, nullptr);
        SetThreadPriority(_thread, THREAD_PRIORITY_HIGHEST);
    }
    _pushed = value;
    SetEvent(_wake);
}

int64_t RenderClock::Done(uint64_t value) const
{
    const auto& s = _slot[value % kRing];
    return s.value.load() == value ? s.qpc.load() : 0;
}

int64_t RenderClock::WaitDone(uint64_t value, unsigned ms)
{
    if (auto t = Done(value))
        return t;
    if (!_fence)
        return QpcNow();
    static thread_local HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (_fence->GetCompletedValue() < value && SUCCEEDED(_fence->SetEventOnCompletion(value, e)))
        WaitForSingleObject(e, ms);
    return QpcNow();
}

double Pacer::Pace(int mode, RenderClock& clock, uint64_t value, int64_t presentQpc)
{
    if (mode <= 0 || value < 2)
        return 0;
    // T: the render card finished the previous frame; Q > 0: this frame queued behind it, < 0: the card was idle
    const int64_t t = clock.WaitDone(value - 1, 250);
    const double q = QpcMs(t - presentQpc);
    _queueMs = _queueMs * 0.9 + q * 0.1;
    if (mode < 2)
        return 0;
    constexpr double target = 0.7, gain = 0.25;
    _sleepMs = (std::min)(40.0, (std::max)(0.0, _sleepMs + gain * (q - target)));
    const int64_t start = QpcNow();
    const int64_t base = (std::max)(t, presentQpc);
    SleepUntil(base + int64_t(_sleepMs / QpcMs(1)));
    return QpcMs(QpcNow() - start);
}
} // namespace Mgpu
