// Per-card GPU engine use from the "GPU Engine" PDH counters (summed over processes, by LUID).
#include "MgpuCommon.h"
#include "MgpuCore.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <thread>
#include <vector>
#include <atomic>
#include <map>
#include <format>

#pragma comment(lib, "pdh.lib")

namespace Mgpu
{
static std::atomic<bool> s_loadStarted { false };

static void Sample()
{
    SetThreadDescription(GetCurrentThread(), L"Mgpu load sampler");
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER c = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS)
        return;
    if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) != ERROR_SUCCESS)
    {
        PdhCloseQuery(q);
        return;
    }
    PdhCollectQueryData(q);
    std::vector<BYTE> buf;
    for (;;)
    {
        Sleep(500);
        if (PdhCollectQueryData(q) != ERROR_SUCCESS)
            continue;
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) !=
            PDH_MORE_DATA)
            continue;
        buf.resize(size);
        auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*) buf.data();
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS)
            continue;
        Stats st;
        {
            std::lock_guard g(StatsMutex());
            st = MutableStats();
        }
        auto key = [](const LUID& l)
        { return std::format(L"luid_0x{:08X}_0x{:08X}", (uint32_t) l.HighPart, l.LowPart); };
        const std::wstring rk = key(st.render.luid), dk = key(st.display.luid);
        // Busiest engine type per card (3D, Compute, Copy...), summed across processes.
        std::map<std::wstring, double> r, d;
        for (DWORD i = 0; i < count; ++i)
        {
            std::wstring n = items[i].szName;
            auto e = n.find(L"engtype_");
            if (e == std::wstring::npos || items[i].FmtValue.CStatus != ERROR_SUCCESS)
                continue;
            std::wstring et = n.substr(e + 8);
            if (n.find(rk) != std::wstring::npos)
                r[et] += items[i].FmtValue.doubleValue;
            else if (n.find(dk) != std::wstring::npos)
                d[et] += items[i].FmtValue.doubleValue;
        }
        auto top = [](const std::map<std::wstring, double>& m)
        {
            double v = 0;
            for (auto& [k, x] : m)
                v = (std::max)(v, x);
            return (std::min)(v, 100.0);
        };
        const double net = CurrentEnv().nrBusy ? CurrentEnv().nrBusy() : -1;
        std::lock_guard g(StatsMutex());
        MutableStats().renderBusy = top(r);
        MutableStats().displayD3dBusy = top(d);
        MutableStats().displayNetBusy = net;
        MutableStats().displayBusy = (std::min)(top(d) + (std::max)(net, 0.0), 100.0);
    }
}

void StartLoadSampler()
{
    if (s_loadStarted.exchange(true))
        return;
    std::thread(Sample).detach();
}
} // namespace Mgpu
