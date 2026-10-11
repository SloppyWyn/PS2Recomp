#pragma once

// A blocking wait (WaitSema, WaitEventFlag) reached outside any IOP thread: an RPC server
// function the bridge calls directly. On the hardware the server is a thread that sleeps while
// the interrupt it waits for (typically a DMA completion) arrives. Here IOP time moves on from
// one scheduled hardware event to the next, servicing what is due, until the wait is satisfied.
// It gives up at once, without moving time, when nothing that could satisfy it is scheduled or
// when it is reached re-entrantly (from an interrupt handler or a callback being serviced, which
// must not run the service loop again). Time spent waiting for a transfer in flight (an SPU2
// DMA, a DMA interrupt already raised) is bounded by that transfer: it counts only against
// a hard limit of TransferCapFactor x `capCycles` (a 256 KB SPU2 upload alone takes ~22 ms).
// Time spent on other events (timers, callbacks) that keep coming without satisfying the wait
// counts against `capCycles`. The loop is separate from the emulator so it can be tested alone.
// Longer term: run RPC server functions on the IOP thread that registered them, so these waits
// go through the scheduler like any other (docs/SPU2-MODEL.md).

#include <algorithm>
#include <cstdint>
#include <functional>

namespace ps2x::iop::detail
{
    enum class CallWaitResult : uint8_t
    {
        Ready,            // satisfied
        NothingScheduled, // no event that could satisfy it; time not moved past the last event
        Reentrant,        // reached while interrupts/callbacks were being serviced; time not moved
        Cap,              // events kept coming past the cap without satisfying it
    };

    struct CallWaitHooks
    {
        std::function<bool()> reentrant;  // interrupts or callbacks are being serviced now
        std::function<void()> service;    // run what is due now (interrupts, callbacks, timers)
        std::function<uint64_t()> now;    // current IOP cycle
        // The next scheduled event at or before `limit` in `next`; false = nothing scheduled.
        // `transfer` = a transfer is in flight (its completion is one of the events).
        std::function<bool(uint64_t limit, uint64_t &next, bool &transfer)> nextEvent;
        std::function<void(uint64_t cycle)> advanceTo;
    };

    inline const char *callWaitResultName(CallWaitResult result) noexcept
    {
        switch (result)
        {
        case CallWaitResult::Ready:
            return "ok";
        case CallWaitResult::NothingScheduled:
            return "gave-up nothing-scheduled";
        case CallWaitResult::Reentrant:
            return "gave-up reentrant";
        case CallWaitResult::Cap:
            return "gave-up cap";
        }
        return "?";
    }

    constexpr uint64_t TransferCapFactor = 50u;

    inline CallWaitResult waitInCallContext(const CallWaitHooks &hooks, const std::function<bool()> &ready, uint64_t capCycles)
    {
        if (ready())
            return CallWaitResult::Ready;
        if (hooks.reentrant())
            return CallWaitResult::Reentrant;
        const uint64_t hardLimit = hooks.now() + capCycles * TransferCapFactor;
        uint64_t otherCycles = 0u; // time passed with no transfer in flight
        for (;;)
        {
            hooks.service();
            if (ready())
                return CallWaitResult::Ready;
            const uint64_t now = hooks.now();
            if (now >= hardLimit || otherCycles >= capCycles)
                return CallWaitResult::Cap;
            bool transfer = false;
            uint64_t next = hardLimit;
            if (!hooks.nextEvent(hardLimit, next, transfer))
                return CallWaitResult::NothingScheduled;
            const uint64_t stop = transfer ? hardLimit : std::min(hardLimit, now + (capCycles - otherCycles));
            const uint64_t target = std::max(now + 1u, std::min(next, stop));
            if (!transfer)
                otherCycles += target - now;
            hooks.advanceTo(target);
        }
    }
}
