#pragma once

// Deterministic test mode: PS2X_DETERMINISTIC=1. Two runs from the same snapshot (or boot)
// with the same pad script compute the same guest state at every VBlank, so a determinism
// check (PS2X_FRAME_HASH, tools\checkpoint.ps1 verify) can name the first frame that differs.
//
// What it changes (each a game-agnostic property of the runtime, not of a game):
//   - Guest time comes only from EE cycle charges: no real-time catch-up (as
//     PS2X_REALTIME_CLOCK=0), and no waiting for host deadlines (as PS2X_FASTFORWARD=max): an
//     event is due when the EE cycle count reaches it, an idle guest jumps to its next event.
//     The run goes as fast as the host can; guest VBlanks are not 1/60 s of host time.
//   - The GS runs synchronously (as PS2X_GS_ASYNC=0) and no audio device opens.
//   - Wall-clock reads in the guest's path (the CD/RTC clock, file and memory-card times,
//     the time zone) return a fixed epoch plus guest time: PS2X_DETERMINISTIC_EPOCH
//     (seconds since 1970, UTC; default 2026-01-01 00:00:00) + VBlank count / 60, in UTC.
// Off (unset or 0): nothing changes, every check below is one cached flag test.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace ps2x::det
{
    constexpr std::time_t kDefaultEpoch = 1767225600; // 2026-01-01 00:00:00 UTC

    inline bool enabled()
    {
        static const bool on = []
        {
            const char *value = std::getenv("PS2X_DETERMINISTIC");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        return on;
    }

    inline std::time_t epoch()
    {
        static const std::time_t value = []
        {
            const char *text = std::getenv("PS2X_DETERMINISTIC_EPOCH");
            return text && *text ? static_cast<std::time_t>(std::strtoll(text, nullptr, 0)) : kDefaultEpoch;
        }();
        return value;
    }

    inline std::atomic<uint64_t> &vblankTick()
    {
        static std::atomic<uint64_t> tick{0u};
        return tick;
    }

    // EE executor, each VBlankStart.
    inline void publishVBlank(uint64_t tick) noexcept
    {
        vblankTick().store(tick, std::memory_order_release);
    }

    // The guest's wall clock: the host's, or in deterministic mode epoch + guest seconds.
    inline std::time_t now()
    {
        if (!enabled())
            return std::time(nullptr);
        return epoch() + static_cast<std::time_t>(vblankTick().load(std::memory_order_acquire) / 60u);
    }

    // Calendar time for the guest: the host's local time, or UTC in deterministic mode.
    inline bool calendar(const std::time_t *value, std::tm *out)
    {
#ifdef _WIN32
        return (enabled() ? gmtime_s(out, value) : localtime_s(out, value)) == 0;
#else
        return (enabled() ? gmtime_r(value, out) : localtime_r(value, out)) != nullptr;
#endif
    }
}
