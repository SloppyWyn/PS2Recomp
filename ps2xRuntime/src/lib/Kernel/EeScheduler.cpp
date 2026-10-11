#include "runtime/ee_scheduler.h"
#include "runtime/ps2_snapshot.h"
#include "runtime/ps2_frame_hash.h"
#include "runtime/ps2_cheats.h"
#include "runtime/ps2_deterministic.h"
#include "ps2_call_trace.h"
#include "ps2_guest_names.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_fastforward.h"
#include "runtime/ps2_pace_log.h"

#include <algorithm>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    // PS2X_PACE_LOG (ps2_pace_log.h): guest cycles charged by code and added by the real-time
    // catch-up since the last VBlankStart, and host time the executor spent in pacing waits.
    struct PaceCounters
    {
        uint64_t charged = 0;
        uint64_t caughtUp = 0;
        int64_t pacedUs = 0;
        int64_t idleUs = 0;
    };
    PaceCounters g_pace;

    // Timed waits of the executor (a VBlank, a timer or a deferred completion that is due in
    // guest cycles but not yet in host time; an idle guest waiting for its next event) must end
    // on time: a VBlank interrupt delivered late shortens the frame the game has left before
    // the next one. A condition-variable wait on Windows ends up to 1-2 ms after its deadline
    // even with a 1 ms timer period (measured: VBlank-end delivered 0.6-1.4 ms late, a 200 us
    // RPC completion holding the running thread for 1.3 ms), which made SRWZ's main loop, whose
    // frame ends ~1.5 ms before the next VBlank, miss every other VBlank. So: sleep on the
    // condition variable until kWaitSpinMargin before the deadline, then poll the rest.
    // PS2X_PACE_SPIN_US=<us> sets the margin (0 = plain condition-variable waits).
    std::chrono::microseconds waitSpinMargin()
    {
        static const std::chrono::microseconds margin = []
        {
            long long us = 2000;
            if (const char *value = std::getenv("PS2X_PACE_SPIN_US"); value && *value)
                us = std::max(0ll, std::min(20000ll, std::atoll(value)));
            return std::chrono::microseconds(us);
        }();
        return margin;
    }

    inline void cpuRelax() noexcept
    {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }

    // Like cv.wait_until(lock, deadline, pred) (true = pred became true), but on time: the last
    // waitSpinMargin() is polled with the lock released between polls, so posters get through.
    template <typename Pred>
    bool waitUntilOnTime(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                         std::chrono::steady_clock::time_point deadline, Pred pred)
    {
        const auto margin = waitSpinMargin();
        if (margin.count() == 0)
            return cv.wait_until(lock, deadline, pred);
        const auto coarse = deadline - margin;
        if (std::chrono::steady_clock::now() < coarse && cv.wait_until(lock, coarse, pred))
            return true;
        while (!pred())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return pred();
            lock.unlock();
            for (int i = 0; i < 32; ++i)
                cpuRelax();
            lock.lock();
        }
        return true;
    }

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    uint64_t hostDurationToEeCycles(std::chrono::nanoseconds duration)
    {
        if (duration.count() <= 0)
        {
            return 0u;
        }
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t nanoseconds = static_cast<uint64_t>(duration.count());
        const uint64_t wholeSeconds = nanoseconds / kNanosecondsPerSecond;
        const uint64_t remainingNanoseconds = nanoseconds % kNanosecondsPerSecond;
        return wholeSeconds * EeScheduler::kEeClockHz + remainingNanoseconds * EeScheduler::kEeClockHz / kNanosecondsPerSecond;
    }

    // Real-time catch-up: a single top-up never exceeds this; beyond it the host is
    // considered to have stalled (debugger, window drag) and the excess is dropped.
    constexpr uint64_t kMaxCatchUpCycles = EeScheduler::kEeClockHz / 4u;

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_deferredInvocations.clear();
    m_nextDeferredInvocationId = 1;
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_unwindingToDispatcher = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_flushingDeferred = false;
    m_batchFlushCycle = 0u;
    m_iopSyncedCycle = 0u;
    m_batchStats = {};
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  ps2x::ff::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);
    if (const char *realtime = std::getenv("PS2X_REALTIME_CLOCK"); (realtime && realtime[0] == '0') || ps2x::det::enabled())
    {
        m_realtimeClock = false;
    }
    if (const char *stats = std::getenv("PS2X_SCHED_STATS"); stats && stats[0] != '\0' && std::strcmp(stats, "0") != 0)
    {
        m_dispatchStatsEnabled = true;
        char *end = nullptr;
        const unsigned long lo = std::strtoul(stats, &end, 0);
        if (end && *end == '-')
        {
            const unsigned long hi = std::strtoul(end + 1, nullptr, 0);
            if (hi > lo)
            {
                m_dispatchWatchLo = static_cast<uint32_t>(lo);
                m_dispatchWatchHi = static_cast<uint32_t>(hi);
            }
        }
        std::cout << "[sched:stats] counting dispatches (PS2X_SCHED_STATS)";
        if (m_dispatchWatchHi > m_dispatchWatchLo)
        {
            std::cout << ", watching 0x" << std::hex << m_dispatchWatchLo << "-0x" << m_dispatchWatchHi << std::dec;
        }
        std::cout << std::endl;
    }
    m_hostEpoch = ps2x::ff::now();
    m_hostEpochCycle = m_eeCycle;
    RUNTIME_LOG("[ee:clock] realtime catch-up " << (m_realtimeClock ? "on" : "off (PS2X_REALTIME_CLOCK=0)") << std::endl);
    // PS2X_BATCH_TIMING: unset/0 = off; 1 = on with the default bound; N >= 8 = on, flush at
    // least every N EE cycles. A runtime caller may already have set it (setBatchTiming).
    if (const char *batch = std::getenv("PS2X_BATCH_TIMING"); batch && batch[0] != '\0' && std::strcmp(batch, "0") != 0)
    {
        const unsigned long long value = std::strtoull(batch, nullptr, 10);
        setBatchTiming(value >= 8ull ? static_cast<uint64_t>(value) : kDefaultBatchTimingCycles);
    }
    if (m_batchTiming)
    {
        RUNTIME_LOG("[ee:clock] batched timer/IOP accounting (PS2X_BATCH_TIMING): flush at least every "
                    << m_batchMaxCycles << " EE cycles" << std::endl);
    }
    if (const ps2x::ff::Settings &ff = ps2x::ff::settings(); ff.active)
    {
        std::cout << "[ee:clock] fast-forward (PS2X_FASTFORWARD): "
                  << (ff.unlimited ? "unlimited, busy scale " : "x") << ff.factor
                  << ", catch-up step <= " << (ff.catchUpCapCycles / ps2x::ff::kEeCyclesPerMicrosecond) << " us" << std::endl;
    }

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        // Snapshot point: no guest frame is on the host stack here (see
        // docs/SNAPSHOT-DESIGN.md). It comes before processPendingEvents(): events that are
        // due but not yet processed stay in m_deadlines/m_events (saved, and they fire after a
        // load), whereas right after processing an INTC handler would already be queued as a
        // guest invocation and the save refused. The service still declines while the state
        // is not serialisable (a callback queued or in flight) and is asked again.
        if (m_saveRequested.load(std::memory_order_acquire))
        {
            if (PS2SnapshotService::atSafePoint(m_runtime, m_vsyncTick))
            {
                m_saveRequested.store(false, std::memory_order_release);
            }
            if (m_stopRequested.load(std::memory_order_acquire))
            {
                break;
            }
        }

        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            running->resumeTag = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            {
                // A thread whose code jumped to address 0 (its entry function returned with
                // ra = 0, or a smashed return address): it ends here without ExitThread. Say
                // so (first 8 times), with the registers and the last guest dispatches.
                static int s_lines = 0;
                if (running->id > 0 && s_lines < 8) // not the scheduler's own dispatcher thread
                {
                    ++s_lines;
                    std::cerr << "[ee:thread-end] thread " << running->id
                              << " reached pc 0 (no ExitThread): dormant\n"
                              << m_runtime.crashGuestReport() << std::flush;
                }
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        m_runtime.m_activeGuestContext.store(&context, std::memory_order_relaxed);
        if (m_dispatchStatsEnabled) [[unlikely]]
        {
            recordDispatch(context.pc);
        }
        if (ps2x_calltrace::active()) [[unlikely]]
        {
            ps2x_calltrace::schedulerEnter(&context, context.pc, m_currentThreadId,
                                           m_runtime.memory().gs().vsyncTick.load(std::memory_order_relaxed));
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            m_unwindingToDispatcher = false;
            function(m_rdram, &context, &m_runtime);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            m_runtime.m_activeGuestContext.store(nullptr, std::memory_order_relaxed);
            publishSnapshot();
            throw;
        }
        // The context can go away with its thread (deleted, exited): no stale pointer
        // for a crash report outside guest code.
        m_runtime.m_activeGuestContext.store(nullptr, std::memory_order_relaxed);

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    if (m_batchTiming)
    {
        flushDeferredTime();
        RUNTIME_LOG("[ee:clock] batch stats: charges=" << m_batchStats.charges
                    << " flushes=" << m_batchStats.flushes
                    << " iopRuns=" << m_batchStats.iopRuns
                    << " timerSyncs=" << m_batchStats.timerSyncs
                    << " externalSyncs=" << m_batchStats.externalSyncs
                    << " lazyTimerReads=" << m_runtime.memory().eeTimerLazySyncCount()
                    << " eeCycle=" << m_eeCycle << std::endl);
    }
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::recordDispatch(uint32_t pc) noexcept
{
    m_dispatchTotal.fetch_add(1u, std::memory_order_relaxed);
    if (m_dispatchWatchHi > m_dispatchWatchLo && pc >= m_dispatchWatchLo && pc < m_dispatchWatchHi)
    {
        m_dispatchWatched.fetch_add(1u, std::memory_order_relaxed);
    }
    // A fragment starting exactly at pc counts as a fragment dispatch even when a real
    // function contains it (find() prefers the real function for naming).
    const PS2GuestFunctionName *exact = ps2x_names::mainTable().findStart(pc);
    const PS2GuestFunctionName *entry = ps2x_names::mainTable().find(pc);
    if ((exact && (exact->flags & PS2X_GUEST_NAME_FRAGMENT) != 0u) ||
        (entry && (entry->flags & PS2X_GUEST_NAME_FRAGMENT) != 0u))
    {
        m_dispatchIntoFragment.fetch_add(1u, std::memory_order_relaxed);
    }
    else if (entry && pc == entry->start)
    {
        m_dispatchIntoStart.fetch_add(1u, std::memory_order_relaxed);
    }
    else if (entry)
    {
        // Mid-function: the owner's pc switch.
        m_dispatchIntoOwner.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        // Not in the main program's table: a code overlay (ps2xOverlayLookup, by the overlay
        // loaded over pc now) or unregistered code; the latter cannot be dispatched, so this is
        // the overlay count.
        m_dispatchIntoOverlay.fetch_add(1u, std::memory_order_relaxed);
    }
}

EeScheduler::DispatchStats EeScheduler::dispatchStats() const noexcept
{
    DispatchStats stats{};
    stats.total = m_dispatchTotal.load(std::memory_order_relaxed);
    stats.intoStart = m_dispatchIntoStart.load(std::memory_order_relaxed);
    stats.intoOwner = m_dispatchIntoOwner.load(std::memory_order_relaxed);
    stats.intoFragment = m_dispatchIntoFragment.load(std::memory_order_relaxed);
    stats.intoOverlay = m_dispatchIntoOverlay.load(std::memory_order_relaxed);
    stats.watched = m_dispatchWatched.load(std::memory_order_relaxed);
    return stats;
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

// PS2X_FASTFORWARD=max assumes only the scheduler thread posts EE events (see ps2_fastforward.h).
void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    // Every caller that gets true returns straight to the scheduler (generated code: `if
    // (runtime->eeCheckpointDue()) return;`, dispatchGuestBranch: return false), so the flag
    // holds until run() dispatches again and clears it.
    const bool due = checkpointDueNow(cycles);
    if (due)
    {
        m_unwindingToDispatcher = true;
    }
    return due;
}

bool EeScheduler::checkpointDueNow(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    if (ps2x::pace::enabled()) [[unlikely]]
        g_pace.charged += elapsed;
    if (m_batchTiming)
    {
        ++m_batchStats.charges;
        PS2Memory &memory = m_runtime.memory();
        memory.deferEeTimerCycles(elapsed);
        if (m_eeCycle >= m_batchFlushCycle || memory.eeTimerGeneration() != m_batchTimerGeneration)
        {
            flushDeferredTime();
        }
    }
    else
    {
        m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
        m_runtime.advanceIopEeCycles(elapsed);
    }
    if (m_realtimeClock && (++m_realtimeSampleCounter & 31u) == 0u)
    {
        catchUpToHostClock();
    }
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::setBatchTiming(uint64_t maxBatchCycles) noexcept
{
    if (maxBatchCycles == 0u)
    {
        if (m_batchTiming)
        {
            flushDeferredTime();
        }
        m_batchTiming = false;
        m_runtime.setIopBatchedTiming(false);
        return;
    }
    m_batchTiming = true;
    m_runtime.setIopBatchedTiming(true);
    m_batchMaxCycles = std::max<uint64_t>(8u, maxBatchCycles);
    // Everything up to now was applied eagerly; the first deadline is computed at once.
    m_iopSyncedCycle = m_eeCycle;
    flushDeferredTime();
}

void EeScheduler::flushDeferredTime() noexcept
{
    if (!m_batchTiming || m_flushingDeferred)
    {
        return;
    }
    PS2Memory &memory = m_runtime.memory();
    // Nothing deferred and the deadline still valid: the common case for the external syncs
    // (every SIF/IOP memory access) right after a flush.
    if (memory.eeTimerDeferredCycles() == 0u && m_eeCycle == m_iopSyncedCycle &&
        memory.eeTimerGeneration() == m_batchTimerGeneration && m_batchFlushCycle > m_eeCycle)
    {
        return;
    }
    m_flushingDeferred = true;
    ++m_batchStats.flushes;

    if (memory.eeTimerDeferredCycles() != 0u)
    {
        ++m_batchStats.timerSyncs;
        memory.syncEeTimers();
    }
    m_pendingEeTimerInterrupts |= memory.takeEeTimerInterrupts();

    // The IOP runs for the cycles the EE has charged since its last run. Set the mark
    // before running: IOP code can call back into guest code, which charges cycles and
    // comes back here (the guard above makes that a no-op).
    if (m_eeCycle > m_iopSyncedCycle)
    {
        const uint64_t iopCycles = m_eeCycle - m_iopSyncedCycle;
        m_iopSyncedCycle = m_eeCycle;
        ++m_batchStats.iopRuns;
        m_runtime.advanceIopEeCycles(iopCycles);
    }

    // Next flush: the earliest of the EE timers' next interrupt (exact), the IOP's next
    // scheduled event and the bound. A runnable IOP thread (0) runs within the bound: the
    // EE/IOP interleaving is what the bound trades, their cycle ratio stays 8:1.
    uint64_t next = m_batchMaxCycles;
    next = std::min(next, memory.cyclesUntilNextEeTimerInterrupt());
    if (const uint64_t iopNext = m_runtime.iopEeCyclesUntilNextEvent(); iopNext != 0u)
    {
        next = std::min(next, iopNext);
    }
    // Nested charges (guest code run by the IOP) may have moved m_eeCycle meanwhile; the
    // deadline counts from the cycle the timers and the IOP are synced to.
    m_batchFlushCycle = m_iopSyncedCycle + std::max<uint64_t>(1u, next);
    m_batchTimerGeneration = memory.eeTimerGeneration();

    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_flushingDeferred = false;
}

void EeScheduler::catchUpToHostClock() noexcept
{
    // Guest time otherwise advances only by the fixed charges per call and loop edge.
    // Code that spins on a timer or waits for VBlank around runtime calls (which cost
    // host time but few guest cycles) would see the clock crawl; on real hardware time
    // passes regardless. The pacing in processDueDeadlines still waits when the guest
    // is ahead of the host, so this only ever moves the clock forward to "now".
    const auto now = ps2x::ff::now();
    auto pacedElapsed = now - m_hostEpoch;
    if (const ps2x::ff::Settings &ff = ps2x::ff::settings();
        ff.unlimited && m_ffBusyScale > 0.0 && m_ffBusyScale != ff.factor)
    {
        // The governor has lowered the busy scale: the paced clock runs at ff.factor.
        pacedElapsed = std::chrono::steady_clock::duration(static_cast<std::chrono::steady_clock::rep>(
            static_cast<double>(pacedElapsed.count()) * m_ffBusyScale / ff.factor));
    }
    const uint64_t hostCycle = m_hostEpochCycle + hostDurationToEeCycles(pacedElapsed);
    if (hostCycle <= m_eeCycle)
    {
        if (ps2x::ff::settings().unlimited)
        {
            // Unlimited fast-forward skips idle time, so the guest is routinely ahead of
            // the paced clock; measure busy time from here instead of from boot.
            m_hostEpoch = now;
            m_hostEpochCycle = m_eeCycle;
        }
        return;
    }
    uint64_t behind = hostCycle - m_eeCycle;
    // Beyond this the host is taken to have stalled and the excess is dropped. While fast-
    // forwarding the limit scales with the factor, so a long synchronous runtime call at x4
    // (100 ms host = 400 ms guest) is carried, not mistaken for a stall. 1x: unchanged.
    const uint64_t stallCycles = ps2x::ff::settings().active
                                     ? static_cast<uint64_t>(static_cast<double>(kMaxCatchUpCycles) * ps2x::ff::settings().factor)
                                     : kMaxCatchUpCycles;
    if (behind > stallCycles)
    {
        behind = stallCycles;
        m_hostEpoch = now;
        m_hostEpochCycle = m_eeCycle + behind;
    }
    // Fast-forward also limits each single step (ps2x::ff::Settings::catchUpCapCycles) and
    // carries the rest to the next top-ups: host time spent inside one runtime call (an MPEG
    // decode, a GS upload) times the factor must not land as tens of milliseconds of guest
    // time at once, or code that budgets work against a timer gets no budget (the movie
    // player feeds its stream only while timer 0 < 500; with no feed, sceMpegGetPicture
    // waits for a picture forever). Measured: uncapped x3/x4 hung at the movie's start.
    if (ps2x::ff::settings().active && behind > ps2x::ff::settings().catchUpCapCycles)
    {
        behind = ps2x::ff::settings().catchUpCapCycles;
    }
    m_eeCycle += behind;
    if (ps2x::pace::enabled()) [[unlikely]]
        g_pace.caughtUp += behind;
    if (m_batchTiming)
    {
        // Deferred like any charge; a chunk that reaches the flush deadline (a timer compare,
        // an IOP event, the bound) is applied now, as the eager path would detect it now.
        m_runtime.memory().deferEeTimerCycles(behind);
        if (m_eeCycle >= m_batchFlushCycle)
        {
            flushDeferredTime();
        }
        return;
    }
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(behind);
    m_runtime.advanceIopEeCycles(behind);
}

// Unlimited fast-forward charges busy host time at the busy scale. Host work that grows with
// guest time (VBlank and timer handlers, the IOP, per-frame runtime work) is busy time too, so
// it feeds back: each guest second costs host time that is charged as more guest seconds. With
// a per-guest-second host cost r the VBlank count stretches by 1/(1 - scale*r) (SRWZ, idle
// host: x1.2-1.7) and diverges once scale*r >= 1: a loaded host then spends minutes on a
// logo screen, delivering dozens of VBlanks per game frame. The governor watches for the
// symptom, VBlanks passing with the guest never idle: after kBusyVBlanksBeforeBackoff of them
// it halves the scale (down to 1, i.e. real-time charging), and every VBlank with an idle skip
// doubles it back (up to the configured scale). A frame-paced guest idles every frame or two
// and keeps the full scale; a movie player polling a timer between pictures idles at each
// picture's VSync wait. Runs on the executor thread, at each VBlankStart.
void EeScheduler::governBusyScaleAtVBlank() noexcept
{
    const ps2x::ff::Settings &ff = ps2x::ff::settings();
    if (!ff.unlimited)
    {
        return;
    }
    const double before = m_ffBusyScale > 0.0 ? m_ffBusyScale : ff.factor;
    const ps2x::ff::GovernorState next =
        ps2x::ff::governBusyScale({m_ffBusyScale, m_ffBusyVBlanks}, m_ffIdleSinceVBlank, ff.factor);
    m_ffBusyVBlanks = next.busyVBlanks;
    m_ffIdleSinceVBlank = false;
    if (next.scale == before)
    {
        m_ffBusyScale = next.scale;
        return;
    }
    // Busy time so far was charged at the old scale: settle it at that scale first, then
    // switch and measure from here at the new one. PS2X_REALTIME_CLOCK=0 has no catch-up.
    if (m_realtimeClock)
    {
        m_ffBusyScale = before;
        catchUpToHostClock();
    }
    m_ffBusyScale = next.scale;
    m_hostEpoch = ps2x::ff::now();
    m_hostEpochCycle = std::max(m_hostEpochCycle, m_eeCycle);
    // A movie or load can flip the scale every few VBlanks: log the first changes only.
    constexpr uint32_t kMaxScaleChangeLogs = 256u;
    if (m_ffScaleChangeLogs < kMaxScaleChangeLogs)
    {
        ++m_ffScaleChangeLogs;
        std::cout << "[ee:clock] fast-forward busy scale " << before << " -> " << next.scale
                  << " at vsync " << m_vsyncTick << (next.scale < before ? " (no idle for "
                  + std::to_string(ps2x::ff::kBusyVBlanksBeforeBackoff) + " VBlanks)" : " (idle)")
                  << (m_ffScaleChangeLogs == kMaxScaleChangeLogs ? "; further changes not logged" : "")
                  << std::endl;
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (ps2x::pace::enabled()) [[unlikely]]
        ps2x::pace::line("PRIO id=%d %d->%d isr=%d cur=%d", id, oldPriority, priority, interruptSafe ? 1 : 0, m_currentThreadId);
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  ps2x::ff::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

void EeScheduler::queueInvocationAfter(GuestInvocation invocation, uint64_t delayCycles)
{
    assertExecutor();
    if (delayCycles == 0u)
    {
        queueInvocation(std::move(invocation));
        return;
    }
    uint32_t id = m_nextDeferredInvocationId++;
    while (id == 0u || m_deferredInvocations.contains(id))
        id = m_nextDeferredInvocationId++;
    m_deferredInvocations.emplace(id, std::move(invocation));
    const uint64_t microseconds = (delayCycles + ps2x::ff::kEeCyclesPerMicrosecond - 1u) / ps2x::ff::kEeCyclesPerMicrosecond;
    scheduleEvent(m_eeCycle + delayCycles,
                  ps2x::ff::now() + std::chrono::microseconds(microseconds),
                  EeEvent{EeEventType::DeferredInvocation, id, 0});
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

std::vector<GuestInvocation> EeScheduler::takePendingInvocations(GuestInvocationKind kind, uint64_t tag)
{
    assertExecutor();
    std::vector<GuestInvocation> taken;
    for (auto it = m_pendingInvocations.begin(); it != m_pendingInvocations.end();)
    {
        if (it->kind == kind && it->tag == tag)
        {
            taken.push_back(std::move(*it));
            it = m_pendingInvocations.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return taken;
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    std::cerr << "[ee:invocation-stack] thread=" << owner->id << " depth=" << depth
              << " sp=0x" << std::hex << top << std::dec << std::endl;
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion, EeResumeTag resumeTag)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion),
        resumeTag});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion,
                                            EeResumeTag resumeTag)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion), resumeTag};
    blockCurrent(std::move(wait));
}

void EeScheduler::requestSnapshot() noexcept
{
    // Host thread (F5) or executor: only atomics are touched here, the tick is read by
    // the executor when it serves the request.
    m_saveRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::setSnapshotTick(uint64_t tick) noexcept
{
    m_saveAtTick.store(tick, std::memory_order_release);
}

void EeScheduler::adoptExecutorThread() noexcept
{
    m_executorThread = std::this_thread::get_id();
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.resumeTag = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    const EeResumeTag resumeTag = item.wait.resumeTag;
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    item.resumeTag = resumeTag;
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    // A checkpoint with something to do (a due deadline, a posted event, a timer interrupt):
    // guest handlers run next and may observe the timers and the IOP. A plain return to the
    // scheduler (a fragment boundary, a thread switch) needs no flush: timer reads catch up
    // lazily and IOP accesses sync themselves.
    if (m_batchTiming && (m_checkpointPending.load(std::memory_order_acquire) || m_pendingEeTimerInterrupts != 0u))
    {
        flushDeferredTime();
    }
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = ps2x::ff::now();
            const ScheduledEvent *pacingItem = nullptr;
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                    pacingItem = &item;
                }
            }
            const int pacingType = pacingItem ? static_cast<int>(pacingItem->event.type) : -1;
            const unsigned pacingId = pacingItem ? pacingItem->event.id : 0u;
            const long long pacingLeadCycles = pacingItem ? static_cast<long long>(m_eeCycle - pacingItem->deadlineCycle) : 0;

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline && !ps2x::ff::settings().unlimited)
            {
                const auto waitStart = std::chrono::steady_clock::now();
                waitUntilOnTime(m_eventCv, lock, ps2x::ff::toHost(pacingDeadline), [this]()
                                { return !m_events.empty() ||
                                         m_stopRequested.load(std::memory_order_acquire); });
                if (ps2x::pace::enabled()) [[unlikely]]
                {
                    const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart).count();
                    g_pace.pacedUs += us;
                    const GuestThread *held = thread(m_currentThreadId);
                    ps2x::pace::line("PACE thr=%d pc=0x%x us=%lld ev=%d id=%u lead=%lld", m_currentThreadId,
                                     held ? held->activeContext().pc : 0u, static_cast<long long>(us),
                                     pacingType, pacingId, pacingLeadCycles);
                }
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = ps2x::ff::now();
            const bool unlimited = ps2x::ff::settings().unlimited;
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow, unlimited](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       (unlimited || item.hostDeadline <= pacedNow); });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              scheduled.hostDeadline + kVBlankPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            if (ps2x::pace::enabled() && (scheduled.event.type == EeEventType::VBlankStart ||
                                          scheduled.event.type == EeEventType::VBlankEnd)) [[unlikely]]
            {
                const auto hostNow = ps2x::ff::now();
                const long long lateUs = std::chrono::duration_cast<std::chrono::microseconds>(hostNow - scheduled.hostDeadline).count();
                const long long aheadCycles = static_cast<long long>(m_eeCycle) - static_cast<long long>(scheduled.deadlineCycle);
                if (scheduled.event.type == EeEventType::VBlankStart)
                {
                    ps2x::pace::line("VS v=%llu late=%lld ahead=%lld thr=%d charged=%llu caught=%llu paced=%lld idle=%lld",
                                     static_cast<unsigned long long>(m_vsyncTick + 1u), lateUs, aheadCycles, m_currentThreadId,
                                     static_cast<unsigned long long>(g_pace.charged), static_cast<unsigned long long>(g_pace.caughtUp),
                                     static_cast<long long>(g_pace.pacedUs), static_cast<long long>(g_pace.idleUs));
                    g_pace = {};
                }
                else
                {
                    ps2x::pace::line("VE v=%llu late=%lld ahead=%lld thr=%d", static_cast<unsigned long long>(m_vsyncTick),
                                     lateUs, aheadCycles, m_currentThreadId);
                }
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::DeferredInvocation:
    {
        const auto deferred = m_deferredInvocations.find(event.id);
        if (deferred != m_deferredInvocations.end())
        {
            GuestInvocation invocation = std::move(deferred->second);
            m_deferredInvocations.erase(deferred);
            queueInvocation(std::move(invocation));
        }
        break;
    }
    case EeEventType::VBlankStart:
        // Asynchronous GS (PS2X_GS_ASYNC): the EE runs at most one VBlank ahead of the GS.
        m_runtime.gs().throttleAtVBlank();
        governBusyScaleAtVBlank();
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        ps2x::det::publishVBlank(m_vsyncTick);
        {
            const auto &gsRegs = m_runtime.memory().gs();
            const uint64_t dispfb = gsRegs.dispfb1 ^ (gsRegs.dispfb2 << 1);
            if (dispfb != m_lastDispfb)
            {
                m_lastDispfb = dispfb;
                m_displayFlips.fetch_add(1u, std::memory_order_relaxed);
            }
        }
        if (ps2x_cheats::enabled()) [[unlikely]]
        {
            ps2x_cheats::atVBlank(m_runtime.memory().getRDRAM(), PS2_RAM_SIZE);
        }
        if (ps2x_frame_hash::enabled()) [[unlikely]]
        {
            ps2x_frame_hash::atVBlank(m_runtime, m_vsyncTick, m_eeCycle);
        }
        completeVSync(m_vsyncTick);
        if (m_vsyncTick >= m_saveAtTick.load(std::memory_order_acquire))
        {
            m_saveAtTick.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
            requestSnapshot();
        }
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    if (m_batchTiming)
    {
        // Idle: bring the timers and the IOP up to now, then take the deadline from the
        // synced timers. Before the lock: the IOP's chunk may run guest code that posts
        // events (postEvent takes m_eventMutex).
        flushDeferredTime();
    }
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = ps2x::ff::now() + eeCyclesToHostDuration(timerCycles);
        // Unlimited (and deterministic) runs jump to the deadline instead of waiting for it, so
        // the earlier one in guest cycles wins; comparing host deadlines (which carry host
        // scheduling jitter) could jump past a timer interrupt, a different path each run.
        const bool timerFirst = m_deadlines.empty() ||
                                (ps2x::ff::settings().unlimited ? m_eeCycle + timerCycles < deadlineCycle
                                                                : timerHostDeadline < hostDeadline);
        if (timerFirst)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    // Unlimited fast-forward: an idle guest jumps straight to its next deadline.
    const auto idleStart = std::chrono::steady_clock::now();
    const bool signaled = ps2x::ff::settings().unlimited
                              ? false
                              : waitUntilOnTime(m_eventCv, lock, ps2x::ff::toHost(hostDeadline), [this]()
                                                { return !m_events.empty() ||
                                                         m_stopRequested.load(std::memory_order_acquire); });
    if (ps2x::pace::enabled()) [[unlikely]]
        g_pace.idleUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - idleStart).count();
    if (!signaled)
    {
        m_ffIdleSinceVBlank = true;
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
