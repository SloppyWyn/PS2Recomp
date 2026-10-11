#pragma once

// PS2X_FASTFORWARD=<factor>: run the guest clock <factor> times faster than wall time
// (e.g. 4). "max" means unlimited: the scheduler never waits for the host clock
// (idle time is skipped), and busy guest code advances the clock at
// PS2X_FF_BUSY_SCALE times wall time (default kDefaultUnlimitedBusyScale).
// Unset, empty, 0 or 1: real time, exactly as before (0 = off, like PS2X_REALTIME_CLOCK=0).
//
// Max mode assumes nothing outside the scheduler (executor) thread posts EE events: an idle
// guest jumps straight to its next deadline instead of waiting, so an event posted from
// another thread (EeScheduler::postEvent) lands at whatever guest time the jumps have reached,
// not at a host-time-consistent point.
//
// Max mode and guest time: the runtime does no host work asynchronously (CD reads complete
// inside the call, sceCdSync reports done, the IOP and SIF RPC run on guest EE cycles via
// advanceIopEeCycles), so skipping idle time can never let the guest clock outrun pending
// IOP/CD work. What max does change is how many guest VBlanks a busy phase costs: busy host
// time is charged at the busy scale, including the host cost of the guest-time-driven work
// itself (VBlank/timer handlers, the IOP), which feeds back. Without a limit the same content
// arrived at x1.2-1.7 the 1x VBlank count on an idle host and diverged on a loaded one (a
// logo screen for minutes, dozens of VBlanks per game frame). EeScheduler's governor
// (governBusyScaleAtVBlank) halves the busy scale after 8 VBlanks without an idle skip and
// restores it on idle, so loads run at real-time charging and frame-paced code keeps the full
// scale; SRWZ then reaches content at about the 1x VBlank count (movie start 1,175-2,583 vs
// 2,555; title loop 24,466 vs ~26,000). Compare runs by content anyway. Limit: a game that
// never idles (busy-waits on a timer or VBlank counter instead of blocking) stays at scale 1
// under max, i.e. busy time is charged at real time and only idle skips speed it up.
// Each scale change prints an [ee:clock] line (capped; see EeScheduler).
//
// The scheduler takes every host deadline and every real-time catch-up from now(),
// a "paced" clock that runs <factor> times faster than steady_clock; waits convert
// back with toHost(). The presenter drops its 60 fps throttle when fast-forwarding.

#include "runtime/ps2_deterministic.h"

#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ps2x::ff
{
    constexpr double kDefaultUnlimitedBusyScale = 8.0;

    // EE cycles (294.912 MHz) in one microsecond.
    constexpr unsigned long long kEeCyclesPerMicrosecond = 294912000ull / 1000000ull;
    constexpr unsigned long long kDefaultCatchUpCapMicroseconds = 2000ull;

    struct Settings
    {
        double factor = 1.0;     // the paced clock's speed relative to steady_clock
        bool unlimited = false;  // never wait for the host clock
        bool active = false;     // anything other than real time
        // Largest single real-time catch-up step while fast-forwarding (guest cycles).
        // PS2X_FF_CATCHUP_US overrides it (microseconds of guest time).
        unsigned long long catchUpCapCycles = kDefaultCatchUpCapMicroseconds * kEeCyclesPerMicrosecond;
    };

    inline Settings parseSettings(const char *value, const char *busyScale, const char *catchUpUs = nullptr)
    {
        Settings s{};
        if (catchUpUs && *catchUpUs)
        {
            const double us = std::strtod(catchUpUs, nullptr);
            if (us >= 1.0 && us <= 250000.0)
                s.catchUpCapCycles = static_cast<unsigned long long>(us * kEeCyclesPerMicrosecond);
        }
        if (!value || !*value)
            return s;
        if (std::strcmp(value, "max") == 0 || std::strcmp(value, "MAX") == 0)
        {
            s.unlimited = true;
            s.active = true;
            s.factor = kDefaultUnlimitedBusyScale;
            if (busyScale && *busyScale)
            {
                const double b = std::strtod(busyScale, nullptr);
                if (b >= 1.0 && b <= 1000.0)
                    s.factor = b;
            }
            return s;
        }
        const double f = std::strtod(value, nullptr);
        if (f > 1.0 && f <= 1000.0)
        {
            s.factor = f;
            s.active = true;
        }
        return s;
    }

    inline const Settings &settings()
    {
        // Deterministic mode (ps2_deterministic.h) never waits for the host clock: as "max".
        static const Settings s = parseSettings(ps2x::det::enabled() ? "max" : std::getenv("PS2X_FASTFORWARD"),
                                                std::getenv("PS2X_FF_BUSY_SCALE"), std::getenv("PS2X_FF_CATCHUP_US"));
        return s;
    }

    inline std::chrono::steady_clock::time_point base()
    {
        static const std::chrono::steady_clock::time_point b = std::chrono::steady_clock::now();
        return b;
    }

    // The paced clock: steady_clock itself at factor 1.
    inline std::chrono::steady_clock::time_point now()
    {
        const auto real = std::chrono::steady_clock::now();
        const Settings &s = settings();
        if (!s.active)
            return real;
        const auto b = base();
        const double scaled = static_cast<double>((real - b).count()) * s.factor;
        return b + std::chrono::steady_clock::duration(static_cast<std::chrono::steady_clock::rep>(scaled));
    }

    // The busy-scale governor's rule, applied at each VBlankStart under unlimited fast-forward
    // (EeScheduler::governBusyScaleAtVBlank): a VBlank after an idle skip doubles the scale (up
    // to maxScale) and clears the busy count; otherwise the busy count grows, and at
    // kBusyVBlanksBeforeBackoff it halves the scale (down to 1) and restarts.
    constexpr uint32_t kBusyVBlanksBeforeBackoff = 8u;
    struct GovernorState
    {
        double scale = 0.0;      // 0 = not set yet (taken as maxScale)
        uint32_t busyVBlanks = 0u;
    };
    inline GovernorState governBusyScale(GovernorState s, bool idleSinceVBlank, double maxScale)
    {
        if (s.scale <= 0.0)
            s.scale = maxScale;
        if (idleSinceVBlank)
        {
            s.busyVBlanks = 0u;
            s.scale = s.scale * 2.0 < maxScale ? s.scale * 2.0 : maxScale;
        }
        else if (++s.busyVBlanks >= kBusyVBlanksBeforeBackoff)
        {
            s.busyVBlanks = 0u;
            s.scale = s.scale * 0.5 > 1.0 ? s.scale * 0.5 : 1.0;
        }
        return s;
    }

    // Host (steady_clock) time at which the paced clock reaches `paced`.
    inline std::chrono::steady_clock::time_point toHost(std::chrono::steady_clock::time_point paced)
    {
        const Settings &s = settings();
        if (!s.active || paced == std::chrono::steady_clock::time_point::max())
            return paced;
        const auto b = base();
        // Rounded up: a wait must never end before the paced clock reaches the deadline.
        const double real = std::ceil(static_cast<double>((paced - b).count()) / s.factor);
        return b + std::chrono::steady_clock::duration(static_cast<std::chrono::steady_clock::rep>(real));
    }
}
