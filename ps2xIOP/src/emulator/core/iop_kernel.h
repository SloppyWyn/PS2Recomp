#pragma once

#include "iop_cpu.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include "ps2x/state_stream.h"

namespace ps2x::iop::detail
{
    class IopMemory;

    enum class IopThreadState : uint8_t
    {
        Dormant,
        Ready,
        Running,
        Sleep,
        Delay,
        Semaphore,
        EventFlag,
        Suspended,
        Dead,
    };

    struct IopThread
    {
        int id = 0;
        IopThreadState state = IopThreadState::Dormant;
        IopCpuState cpu;
        uint32_t entry = 0;
        uint32_t stackBase = 0;
        uint32_t stackSize = 0;
        uint32_t priority = 0x40;
        uint32_t initialPriority = 0x40;
        uint32_t option = 0;
        uint32_t attr = 0;
        uint64_t wakeCycle = 0;
        int waitId = 0;
        uint32_t waitBits = 0;
        uint32_t waitMode = 0;
        uint32_t waitResultAddress = 0;
        int wakeupCount = 0;
    };

    class IopKernel
    {
    public:
        explicit IopKernel(IopMemory &memory) noexcept;

        void reset();

        // Snapshots (docs/SNAPSHOT-DESIGN.md).
        void saveState(ps2x::StateWriter &w) const;
        void loadState(ps2x::StateReader &r);

        [[nodiscard]] bool dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] bool dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchEventImport(uint16_t ordinal, IopCpuState &cpu);

        [[nodiscard]] int createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits);
        [[nodiscard]] bool setInternalEventFlag(int id, uint32_t bits);

        void sleepCurrent(IopCpuState &cpu);
        void delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu);

        [[nodiscard]] IopThread *beginNextReady(uint64_t currentCycle);
        // Like beginNextReady, but only a thread that would run before a thread of priority
        // `priority` made ready now: priority numerically <= `priority` (an equal-priority
        // thread already ready or preempted mid-work goes first). nullptr = none; then nothing
        // is changed (Delay threads whose time has come still become Ready).
        [[nodiscard]] IopThread *beginNextReadyBefore(uint64_t currentCycle, uint32_t priority);
        // Priority of the thread running now; 0 outside any thread.
        [[nodiscard]] uint32_t currentThreadPriority() const noexcept { return m_currentThread ? m_currentThread->priority : 0u; }
        [[nodiscard]] uint64_t nextWakeCycle(uint64_t fallback) const;
        // A thread that can run now (Ready or Running) without waiting for any event.
        [[nodiscard]] bool hasRunnableThread() const noexcept;
        void endTimeslice(IopThread &thread, uint32_t returnSentinel);
        void cleanupDeadThreads();
        void terminateThreadsInRange(uint32_t base, uint32_t size);

        [[nodiscard]] size_t threadCount() const noexcept { return m_threads.size(); }

        // Debug text (PS2X_IOP_WATCH_THREADS, the RPC log): one item per thread,
        // "id:state:prio:pc" plus ":w<wait id>" for a blocked thread and ":k<n>" for pending
        // wakeups; states D dormant, R ready, X running, S sleep, L delay, M semaphore,
        // E event flag, U suspended, Z dead. readyOnly = Ready/Running threads only.
        [[nodiscard]] std::string describeThreads(bool readyOnly) const;
        // "id:count/max" per semaphore with ":<n>w" waiters, "id:bits" per event flag.
        [[nodiscard]] std::string describeSyncObjects() const;

        // A blocking wait (WaitSema, WaitEventFlag) reached outside any IOP thread: an RPC
        // server function or callback the runtime calls directly. On the hardware the RPC
        // server is a thread that sleeps while the interrupt it waits for (a DMA completion)
        // arrives. The hook lets IOP time pass, servicing interrupts, timers and callbacks,
        // until `ready` holds or nothing scheduled can make it hold; it returns whether it
        // holds. No hook (or false) = the old behaviour: the wait fails at once.
        using CallContextWait = std::function<bool(const std::function<bool()> &ready)>;
        void setCallContextWait(CallContextWait wait) { m_callContextWait = std::move(wait); }

    private:
        struct Semaphore
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            int current = 0;
            int maximum = 1;
        };

        struct EventFlag
        {
            int id = 0;
            uint32_t bits = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
        };

        [[nodiscard]] bool referThreadStatus(int id, uint32_t outputAddress);
        void wakeOneSemaphore(int id);
        [[nodiscard]] static bool eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode);
        void wakeEventWaiters(EventFlag &event);

        IopMemory &m_memory;
        std::map<int, IopThread> m_threads;
        std::map<int, Semaphore> m_semaphores;
        std::map<int, EventFlag> m_eventFlags;
        uint32_t m_nextThreadId = 1;
        uint32_t m_nextSemaphoreId = 1;
        uint32_t m_nextEventFlagId = 1;
        IopThread *m_currentThread = nullptr;
        CallContextWait m_callContextWait;
    };
}
