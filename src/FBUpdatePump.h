#pragma once

namespace FB::UpdatePump
{
    // Starts a single self-rescheduling game-thread pump.
    // INVARIANT: at most one pump active, and at most one PumpOnce queued/executing at a time.
    void Start();

    // Optional: stops the pump (safe to call multiple times).
    void Stop();
}
