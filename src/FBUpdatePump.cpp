#include "FBUpdatePump.h"

#include "ActorManager.h"   // FB::ActorManager::Update
#include "RE/B/BSTimer.h"
#include "SKSE/SKSE.h"

#include <atomic>
#include <cstdint>

#include <spdlog/spdlog.h>
#include "REL/Relocation.h"


namespace
{
    std::atomic_bool g_running{ false };
    std::atomic_bool g_scheduled{ false };

    // Frame-advance guard (BSTimer performance counter)
    std::atomic<std::uint64_t> g_lastPerf{ 0 };

    // Conservative hitch guard: skip or clamp pathological dt
    constexpr float kMaxDtSeconds = 0.25f;

    void PumpOnce();

    void SchedulePump()
    {
        if (!g_running.load(std::memory_order_acquire)) {
            return;
        }

        bool expected = false;
        if (!g_scheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // already scheduled/in-flight
            return;
        }

        auto task = []() {
            PumpOnce();
            };

        SKSE::GetTaskInterface()->AddTask(task);
    }
    static RE::BSTimer* GetBSTimer() noexcept
    {
        // CommonLibSSE-NG provides this relocation for BSTimer singleton.
        // Your vcpkg header lacks GetSingleton(), so we replicate it locally.
        REL::Relocation<RE::BSTimer*> singleton{ RELOCATION_ID(523657, 410196) };
        return singleton.get();
    }

    void PumpOnce()
    {
        // Mark as no longer scheduled; we'll schedule again at the end.
        g_scheduled.store(false, std::memory_order_release);

        if (!g_running.load(std::memory_order_acquire)) {
            return;
        }

        auto* timer = GetBSTimer();

        if (!timer) {
            // No timer yet; try again next tick.
            SchedulePump();
            return;
        }

        const std::uint64_t perf = timer->lastPerformanceCount;

        // Busy-loop guard: only run Update when frame/time advances.
        const std::uint64_t last = g_lastPerf.load(std::memory_order_acquire);
        if (perf == last) {
            SchedulePump();
            return;
        }
        g_lastPerf.store(perf, std::memory_order_release);

        float dt = timer->delta;
        if (dt <= 0.0f) {
            SchedulePump();
            return;
        }

        // Required safety: clamp or skip pathological dt (loading/hitch/pause)
        if (dt > kMaxDtSeconds) {
            // Conservative: clamp instead of skipping so timelines still advance smoothly after hitches.
            dt = kMaxDtSeconds;
        }

        FB::ActorManager::Update(dt);

        // Reschedule
        SchedulePump();
    }
}

namespace FB::UpdatePump
{
    void Start()
    {
        bool expected = false;
        if (!g_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // already running
            return;
        }

        g_lastPerf.store(0, std::memory_order_release);
        SchedulePump();
        spdlog::info("[FB] UpdatePump started");
    }

    void Stop()
    {
        g_running.store(false, std::memory_order_release);
        spdlog::info("[FB] UpdatePump stopped");
    }
}
