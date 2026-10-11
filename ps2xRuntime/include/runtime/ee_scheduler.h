#pragma once

#include "ps2_runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// This exception is the EE equivalent of a longjmp to the dispatcher.  It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final
{
};

enum class EeThreadStatus : uint8_t
{
    Running,
    Ready,
    Waiting,
    WaitingSuspended,
    Suspended,
    Dormant,
};

enum class EeWaitReason : uint8_t
{
    None,
    Sleep,
    Semaphore,
    EventFlag,
    VSync,
    External,
    Mpeg,
};

struct EeSemaphoreWait
{
    int id = 0;
};

struct EeEventFlagWait
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t mode = 0;
    uint32_t resultAddress = 0;
};

struct EeVSyncWait
{
    uint64_t afterTick = 0;
    int fixedResult = -1;
};

struct EeExternalWait
{
    uint32_t type = 0;
    uint64_t token = 0;
};

using EeWaitPayload = std::variant<std::monostate,
                                   EeSemaphoreWait,
                                   EeEventFlagWait,
                                   EeVSyncWait,
                                   EeExternalWait>;

// A wait completion is a host closure (it re-runs a stub when the thread wakes). A
// snapshot cannot store a closure, so every closure comes with this tag: the closure's
// kind and the values it captured. Snapshot load rebuilds the closure from the tag
// (ps2_snapshot.cpp). A closure without a tag (kind None) blocks saving.
enum class EeResumeKind : uint32_t
{
    None = 0,
    SetPc = 1,          // args[0] = resume pc (PS2Runtime::eeWaitVSyncTicks)
    MpegGetPicture = 2, // re-run sceMpegGetPicture on the woken context
    CdStRead = 3,       // continue sceCdStRead: args = requested, buffer, error address, read
};

struct EeResumeTag
{
    EeResumeKind kind = EeResumeKind::None;
    std::array<uint64_t, 4> args{};
};

struct EeWaitState
{
    EeWaitReason reason = EeWaitReason::None;
    EeWaitPayload payload{};
    std::function<void(R5900Context &)> completion;
    EeResumeTag resumeTag{};
};

enum class GuestInvocationKind : uint8_t
{
    Interrupt,
    Alarm,
    GsCallback,
    RpcCallback,
    SyscallOverride,
    ExitHandler,
    HleCall,
    SifCommand,
};

// What an invocation's onComplete closure does, so a snapshot can store a thread's
// invocation stack and rebuild the closure (docs/SNAPSHOT-DESIGN.md). An invocation
// with an onComplete but no tag cannot be saved: a snapshot request waits for it.
enum class GuestInvocationCompleteKind : uint8_t
{
    None,
    GuestFree,        // runtime.guestFree(arg)
    CopyReturnValue,  // parent.r[2] = completed.r[2]
    ExitCurrent,      // scheduler.exitCurrent(arg != 0)
    IpuInitComplete,  // ps2_stubs::snapshot::makeIpuInitComplete
    RpcClientComplete, // ps2_stubs::snapshot::makeRpcClientComplete: arg = SIF RPC client address |
                       // call sequence << 32, arg2 = completion semaphore | signal count << 32
    MpegNodataComplete, // ps2_stubs::snapshot::makeMpegNodataComplete(arg = callback data)
};

struct GuestInvocationCompleteTag
{
    GuestInvocationCompleteKind kind = GuestInvocationCompleteKind::None;
    uint64_t arg = 0;
    uint64_t arg2 = 0;
};

struct GuestInvocation
{
    GuestInvocationKind kind = GuestInvocationKind::Interrupt;
    uint64_t sequence = 0;
    uint64_t tag = 0;
    R5900Context context{};
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
    GuestInvocationCompleteTag completeTag{}; // describes onComplete (see above)
};

struct GuestThread
{
    int id = 0;
    R5900Context context{};
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t arg = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    bool ownsStack = false;
    uint32_t tlsBase = 0;
    EeWaitState wait{};
    std::function<void(R5900Context &)> resumeCompletion;
    EeResumeTag resumeTag{}; // describes resumeCompletion (see EeResumeTag)
    std::vector<GuestInvocation> invocations;

    [[nodiscard]] R5900Context &activeContext()
    {
        return invocations.empty() ? context : invocations.back().context;
    }

    [[nodiscard]] const R5900Context &activeContext() const
    {
        return invocations.empty() ? context : invocations.back().context;
    }
};

struct EeSemaphore
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    int initCount = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    std::deque<int> waiters;
};

struct EeEventFlag
{
    int id = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t initBits = 0;
    uint32_t bits = 0;
    std::deque<int> waiters;
};

struct EeAlarm
{
    int id = 0;
    uint16_t ticks = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
};

struct EeIrqHandler
{
    int id = 0;
    uint32_t cause = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
    bool enabled = true;
    int order = 0;
};

struct EeThreadSnapshot
{
    int id = 0;
    uint32_t pc = 0;
    uint32_t ra = 0;
    uint32_t sp = 0;
    uint32_t contextGp = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    EeWaitReason waitReason = EeWaitReason::None;
    int waitId = 0;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    uint32_t invocationDepth = 0;
};

struct EeSemaphoreSnapshot
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    uint32_t waiters = 0;
};

struct EeEventFlagSnapshot
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t initBits = 0;
    uint32_t attr = 0;
    uint32_t waiters = 0;
};

struct EeKernelSnapshot
{
    uint64_t sequence = 0;
    uint64_t eeCycle = 0;
    uint64_t sliceEndCycle = 0;
    uint64_t nextEventCycle = 0;
    int runningThreadId = 0;
    std::vector<EeThreadSnapshot> threads;
    std::vector<EeSemaphoreSnapshot> semaphores;
    std::vector<EeEventFlagSnapshot> eventFlags;
};

enum class EeEventType : uint8_t
{
    Stop,
    VBlankStart,
    VBlankEnd,
    Dmac,
    ExternalWake,
    Alarm,
    DeferredInvocation, // id = key in m_deferredInvocations: queue it when due
};

struct EeEvent
{
    EeEventType type = EeEventType::ExternalWake;
    uint32_t id = 0;
    uint64_t value = 0;
};

struct EeThreadCreateParams
{
    uint32_t attr = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int priority = 0;
    uint32_t option = 0;
};

class EeScheduler
{
    friend struct PS2SnapshotAccess;

public:
    static constexpr int kMainThreadId = 1;
    static constexpr int kFirstThreadId = 2;
    static constexpr int kLastThreadId = 255;
    static constexpr int kPriorityCount = 128;
    static constexpr uint64_t kEeClockHz = 294912000ull;
    static constexpr uint32_t kGeneratedCheckpointCycles = 32u;
    static constexpr uint32_t kGuestDispatchCycles = 8u;
    static constexpr uint64_t kDefaultTimeSliceCycles = 65536ull;
    // PS2X_BATCH_TIMING=1: the EE timers and the IOP catch up at least this often (~3.5 us).
    static constexpr uint64_t kDefaultBatchTimingCycles = 1024ull;

    explicit EeScheduler(PS2Runtime &runtime);
    ~EeScheduler();

    EeScheduler(const EeScheduler &) = delete;
    EeScheduler &operator=(const EeScheduler &) = delete;

    void reset(uint8_t *rdram, const R5900Context &mainContext);
    void run();
    void requestStop();
    void postEvent(EeEvent event);
    [[nodiscard]] bool checkpointDue(uint32_t cycles = kGeneratedCheckpointCycles) noexcept;
    // True from the moment checkpointDue() answers true until the scheduler dispatches guest
    // code again: generated code is then returning, frame by frame, to the scheduler, and
    // ctx->pc is the point to resume at. PS2Runtime::dispatchGuestBranch reads it so that a
    // callee unwinding with ctx->pc at its own entry (a nested call to the same function, a
    // loop back to its first instruction) is not taken for a callee that returned.
    [[nodiscard]] bool unwindingToDispatcher() const noexcept { return m_unwindingToDispatcher; }
    void accountCycles(uint32_t cycles) noexcept;
    // Tops m_eeCycle up to wall-clock time when the guest has fallen behind it
    // (sampled every few checkpoints; PS2X_REALTIME_CLOCK=0 disables it).
    void catchUpToHostClock() noexcept;
    [[nodiscard]] bool isExecutingGuest() const noexcept;

    // PS2X_SCHED_STATS=1 (or =0x<lo>-0x<hi> to also count dispatches landing in that pc
    // range): where the scheduler's dispatches land, by the name table (ps2_guest_names.h):
    // a function's start, the middle of a function (a resume through its pc switch), or a
    // resume fragment (entry_*). Free when off: one predictable branch per dispatch.
    struct DispatchStats
    {
        uint64_t total = 0;
        uint64_t intoStart = 0;
        uint64_t intoOwner = 0;
        uint64_t intoFragment = 0;
        uint64_t intoOverlay = 0; // a code overlay's function (ps2_overlays.h), start or not
        uint64_t watched = 0;
    };
    [[nodiscard]] bool dispatchStatsEnabled() const noexcept { return m_dispatchStatsEnabled; }
    [[nodiscard]] DispatchStats dispatchStats() const noexcept;

    // Batched timing (PS2X_BATCH_TIMING, default off). Off: every charge advances the EE
    // timers and runs the IOP for the charged cycles (one IOP scheduler iteration per guest
    // call). On: charges only move m_eeCycle; the timers and the IOP catch up in one step
    // at the next flush, which comes at the earliest of: the next EE timer compare/overflow
    // (exact cycle, from PS2Memory::cyclesUntilNextEeTimerInterrupt), the IOP's next event
    // (a runnable thread, a delay, a DMA completion, a callback, an IOP timer), a timer
    // register write (PS2Memory::eeTimerGeneration), maxBatchCycles, a checkpoint
    // (processPendingEvents), the idle wait, and any EE-visible IOP operation
    // (PS2Runtime::syncIopTime: RPC, SIF transfer, IOP memory access). Guest reads of
    // timer registers catch up lazily inside PS2Memory. Interrupts therefore fire at the
    // same guest cycle as unbatched; only the EE/IOP interleaving is coarser (<= maxBatch).
    void setBatchTiming(uint64_t maxBatchCycles) noexcept;
    // Tests: deterministic guest time (no wall-clock top-ups), like PS2X_REALTIME_CLOCK=0.
    void setRealtimeClock(bool enabled) noexcept { m_realtimeClock = enabled; }
    [[nodiscard]] bool batchTimingEnabled() const noexcept { return m_batchTiming; }
    void flushDeferredTime() noexcept;
    // The same, requested by an EE-visible IOP operation (counted separately).
    void flushDeferredTimeForIopAccess() noexcept
    {
        ++m_batchStats.externalSyncs;
        flushDeferredTime();
    }
    [[nodiscard]] bool isExecutorThread() const noexcept { return m_executorThread == std::this_thread::get_id(); }
    [[nodiscard]] uint64_t eeCycle() const noexcept { return m_eeCycle; }
    struct BatchTimingStats
    {
        uint64_t charges = 0;      // accountCycles calls
        uint64_t flushes = 0;      // flushDeferredTime calls that applied anything
        uint64_t iopRuns = 0;      // runEeCycles calls made by flushes
        uint64_t timerSyncs = 0;   // flushes that found deferred timer cycles
        uint64_t externalSyncs = 0; // flushes requested by PS2Runtime::syncIopTime
    };
    [[nodiscard]] const BatchTimingStats &batchTimingStats() const noexcept { return m_batchStats; }

    // Kernel object API. All calls except postEvent/requestStop execute on the
    // EE executor and therefore need no host synchronization.
    void setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp);
    int createThread(const EeThreadCreateParams &params);
    int deleteThread(int id, uint32_t &ownedStack);
    int startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe);
    [[noreturn]] void exitCurrent(bool deleteThread);
    int terminateThread(int id, uint32_t &ownedStack, bool interruptSafe);
    int suspendThread(int id, bool interruptSafe);
    int resumeThread(int id, bool interruptSafe);
    void sleepCurrent();
    int wakeupThread(int id, bool interruptSafe);
    int cancelWakeup(int id);
    int changePriority(int id, int priority, bool interruptSafe, int &oldPriority);
    int rotateReadyQueue(int priority, bool interruptSafe);
    int releaseWait(int id, bool interruptSafe);
    void transferIfRequested(bool interruptSafe);

    int createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option);
    int deleteSemaphore(int id, bool interruptSafe);
    int signalSemaphore(int id, bool interruptSafe);
    int pollSemaphore(int id);
    void waitSemaphore(int id);

    int createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option);
    int deleteEventFlag(int id, bool interruptSafe);
    int setEventFlag(int id, uint32_t bits, bool interruptSafe);
    int clearEventFlag(int id, uint32_t mask);
    int pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits);
    void waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress);

    int setAlarm(uint16_t ticks, uint32_t handler, uint32_t argument, uint32_t gp, uint32_t sp);
    int cancelAlarm(int id);
    void queueInvocation(GuestInvocation invocation);
    // Queues the invocation once `delayCycles` of guest time have passed (a completion the
    // hardware delivers later, e.g. an IOP RPC end function): until then nothing runs it,
    // so code that runs on after the call sees the world unchanged, as on the hardware.
    void queueInvocationAfter(GuestInvocation invocation, uint64_t delayCycles);
    [[nodiscard]] bool hasDeferredInvocations() const noexcept { return !m_deferredInvocations.empty(); }
    [[noreturn]] void invokeCurrent(GuestInvocation invocation);
    [[noreturn]] void invokeCurrentSequence(std::vector<GuestInvocation> invocations);
    [[nodiscard]] bool hasInvocation(GuestInvocationKind kind, uint64_t tag) const;
    // Removes the queued (not yet started) invocations of this kind and tag, oldest first, so
    // a caller can run them itself (e.g. ahead of a nested call that must see their effect).
    [[nodiscard]] std::vector<GuestInvocation> takePendingInvocations(GuestInvocationKind kind, uint64_t tag);
    [[nodiscard]] uint32_t invocationStackTop();

    int addIrqHandler(bool dmac,
                      uint32_t cause,
                      uint32_t handler,
                      bool append,
                      uint32_t argument,
                      uint32_t gp,
                      uint32_t sp);
    int removeIrqHandler(bool dmac, uint32_t cause, int id);
    int setIrqHandlerEnabled(bool dmac, int id, bool enabled);
    int setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled);
    void dispatchIrq(bool dmac, uint32_t cause);
    void setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress);
    [[nodiscard]] uint64_t currentVSyncTick() const noexcept;
    // VBlanks at which the display buffer (DISPFB1/2) differed from the previous VBlank's: the
    // game's own frame rate (a new picture per flip), for the [run:tick] line.
    [[nodiscard]] uint64_t displayFlips() const noexcept { return m_displayFlips.load(std::memory_order_relaxed); }
    uint32_t setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp);

    [[noreturn]] void waitVSync(uint64_t afterTick, int fixedResult = -1, std::function<void(R5900Context &)> completion = {}, EeResumeTag resumeTag = {});
    void completeVSync(uint64_t tick);
    void completeExternalWait(uint32_t type, uint64_t token, int result);
    [[noreturn]] void waitExternal(EeWaitReason reason, uint32_t type, uint64_t token, std::function<void(R5900Context &)> completion = {}, EeResumeTag resumeTag = {});

    // Snapshots (ps2_snapshot.cpp). requestSnapshot() may be called from any thread: the
    // executor saves at the next dispatcher loop head where the state is serialisable.
    // setSnapshotTick(): request one automatically when the VBlank tick reaches `tick`.
    void requestSnapshot() noexcept;
    void setSnapshotTick(uint64_t tick) noexcept;
    // Snapshot load runs on another thread before run(); the executor then adopts it.
    void adoptExecutorThread() noexcept;

    [[nodiscard]] GuestThread *thread(int id);
    [[nodiscard]] const GuestThread *thread(int id) const;
    [[nodiscard]] EeSemaphore *semaphore(int id);
    [[nodiscard]] const EeSemaphore *semaphore(int id) const;
    [[nodiscard]] EeEventFlag *eventFlag(int id);
    [[nodiscard]] const EeEventFlag *eventFlag(int id) const;
    [[nodiscard]] GuestThread *currentThread();
    [[nodiscard]] const GuestThread *currentThread() const;
    [[nodiscard]] int currentThreadId() const noexcept;
    [[nodiscard]] R5900Context *currentContext();
    [[nodiscard]] uint8_t *rdram() const noexcept;

    // Direct syscall tests use the same main-thread record without starting a
    // second executor. Production execution calls reset() before run().
    void bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram);

    [[nodiscard]] EeKernelSnapshot snapshot() const;
    void publishSnapshot();

private:
    [[nodiscard]] bool checkpointDueNow(uint32_t cycles) noexcept;

    struct ScheduledEvent
    {
        uint64_t deadlineCycle = 0;
        std::chrono::steady_clock::time_point hostDeadline{};
        EeEvent event{};
        uint64_t sequence = 0;
    };

    void assertExecutor() const;
    [[nodiscard]] int allocateThreadId();
    GuestThread &acquireInvocationThread();
    void enqueueReady(GuestThread &thread, bool front = false);
    void removeReady(GuestThread &thread);
    [[nodiscard]] GuestThread *selectReady();
    void makeRunning(GuestThread &thread);
    void makeDormant(GuestThread &thread);
    void removeFromWaitObject(GuestThread &thread);
    [[noreturn]] void blockCurrent(EeWaitState wait);
    void makeReady(GuestThread &thread, int result, bool interruptSafe);
    void requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe);
    void applyPendingPreemption();
    void processPendingEvents();
    void processDueDeadlines();
    void processEvent(const EeEvent &event);
    void finishEventWaiters(EeEventFlag &flag, bool interruptSafe);
    [[nodiscard]] static bool eventCondition(uint32_t current, uint32_t requested, uint32_t mode);
    static int waitObjectId(const EeWaitState &wait);
    void writeGuestU32(uint32_t address, uint32_t value);
    void waitForEvent();
    void scheduleEvent(uint64_t deadlineCycle, std::chrono::steady_clock::time_point hostDeadline, EeEvent event);
    void updateNextDeadline();
    [[nodiscard]] bool hasReadyAtOrAbovePriority(int priority) const;
    void renewTimeSlice();
    void copyMainContextToRuntime();
    void publishDebugContext(const R5900Context &context);
    void publishIdleDebugContext();

    PS2Runtime &m_runtime;
    uint8_t *m_rdram = nullptr;
    std::array<std::deque<int>, kPriorityCount> m_readyQueues{};
    std::unordered_map<int, GuestThread> m_threads;
    std::unordered_map<int, EeSemaphore> m_semaphores;
    std::unordered_map<int, EeEventFlag> m_eventFlags;
    std::unordered_map<int, EeAlarm> m_alarms;
    std::unordered_map<uint32_t, GuestInvocation> m_deferredInvocations; // queueInvocationAfter
    uint32_t m_nextDeferredInvocationId = 1;
    std::unordered_map<int, EeIrqHandler> m_intcHandlers;
    std::unordered_map<int, EeIrqHandler> m_dmacHandlers;
    int m_nextThreadId = kFirstThreadId;
    int m_nextInvocationThreadId = -1;
    int m_nextSemaphoreId = 1;
    int m_nextEventFlagId = 1;
    int m_nextAlarmId = 1;
    int m_nextIntcHandlerId = 1;
    int m_nextDmacHandlerId = 1;
    int m_intcHeadOrder = 0;
    int m_intcTailOrder = 1000;
    int m_dmacHeadOrder = 0;
    int m_dmacTailOrder = 1000;
    uint32_t m_enabledIntcMask = 0xFFFFFFFFu;
    uint32_t m_enabledDmacMask = 0xFFFFFFFFu;
    int m_currentThreadId = 0;
    bool m_rescheduleRequested = false;
    bool m_unwindingToDispatcher = false; // see unwindingToDispatcher(); executor thread only
    bool m_timeSliceExpired = false;
    bool m_insideInterrupt = false;
    uint32_t m_pendingEeTimerInterrupts = 0;
    uint64_t m_eeCycle = 0;
    uint64_t m_sliceEndCycle = kDefaultTimeSliceCycles;
    bool m_realtimeClock = true;
    uint32_t m_realtimeSampleCounter = 0u;
    std::chrono::steady_clock::time_point m_hostEpoch{};
    uint64_t m_hostEpochCycle = 0u;
    // Unlimited fast-forward busy-scale governor (see governBusyScaleAtVBlank): the busy scale in
    // effect (0 = not set yet: the configured one), VBlanks in a row with no idle skip, and
    // whether the guest went idle since the last VBlank.
    double m_ffBusyScale = 0.0;
    uint32_t m_ffBusyVBlanks = 0u;
    bool m_ffIdleSinceVBlank = false;
    uint32_t m_ffScaleChangeLogs = 0u;
    void governBusyScaleAtVBlank() noexcept;
    std::thread::id m_executorThread{};
    // Batched timing state (see setBatchTiming): the cycle at which the next flush is due,
    // the EE cycle up to which the IOP has run, the timer generation the deadline was
    // computed for, and a re-entrancy guard (the IOP can call back into guest code, which
    // charges cycles).
    bool m_batchTiming = false;
    bool m_flushingDeferred = false;
    uint64_t m_batchMaxCycles = 0u;
    uint64_t m_batchFlushCycle = 0u;
    uint64_t m_iopSyncedCycle = 0u;
    uint32_t m_batchTimerGeneration = 0u;
    BatchTimingStats m_batchStats{};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_guestExecuting{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_checkpointPending{false};
    uint32_t m_debugPublishCountdown = 0u;

    void recordDispatch(uint32_t pc) noexcept;
    bool m_dispatchStatsEnabled = false;
    uint32_t m_dispatchWatchLo = 0u;
    uint32_t m_dispatchWatchHi = 0u;
    std::atomic<uint64_t> m_dispatchTotal{0};
    std::atomic<uint64_t> m_dispatchIntoStart{0};
    std::atomic<uint64_t> m_dispatchIntoOwner{0};
    std::atomic<uint64_t> m_dispatchIntoFragment{0};
    std::atomic<uint64_t> m_dispatchIntoOverlay{0};
    std::atomic<uint64_t> m_dispatchWatched{0};

    mutable std::mutex m_eventMutex;
    std::condition_variable m_eventCv;
    std::deque<EeEvent> m_events;
    std::vector<ScheduledEvent> m_deadlines;
    std::deque<GuestInvocation> m_pendingInvocations;
    uint64_t m_eventSequence = 0;
    uint64_t m_invocationSequence = 0;
    uint64_t m_vsyncTick = 0;
    uint32_t m_vsyncFlagAddress = 0;
    uint32_t m_vsyncTickAddress = 0;
    std::atomic<uint64_t> m_displayFlips{0u};
    uint64_t m_lastDispfb = 0u;
    uint32_t m_gsVSyncCallback = 0;
    uint32_t m_gsVSyncCallbackGp = 0;
    uint32_t m_gsVSyncCallbackSp = 0;
    std::unordered_map<uint64_t, uint32_t> m_invocationStackTops;
    std::atomic<uint64_t> m_nextDeadlineCycle{0};

    mutable std::mutex m_snapshotMutex;
    EeKernelSnapshot m_snapshot;
    uint64_t m_snapshotSequence = 0;

    std::atomic<bool> m_saveRequested{false};
    std::atomic<uint64_t> m_saveAtTick{std::numeric_limits<uint64_t>::max()};
};
