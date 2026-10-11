// Stall watchdog: see include/ps2_watchdog.h. Kept in its own file because it needs
// <windows.h>, which clashes with raylib's names in the runtime's other files.
#include "ps2_watchdog.h"
#include "ps2_guest_names.h"
#include "ps2_host_stack.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

// The native stack walk reads x64 CONTEXT registers (Rip/Rsp) and x64 unwind data.
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define PS2X_WATCHDOG_STACKS 1
#else
#define PS2X_WATCHDOG_STACKS 0
#endif

namespace
{
    int watchdogSeconds() noexcept
    {
        static const int seconds = []
        {
            const char *value = std::getenv("PS2X_WATCHDOG");
            if (!value || !*value)
            {
                return 0;
            }
            const int parsed = std::atoi(value);
            return parsed > 0 ? parsed : 0;
        }();
        return seconds;
    }

    std::atomic<bool> g_enabled{false};
    std::atomic<bool> g_stubRingEnabled{false}; // the watchdog or the crash handler is on
    std::atomic<uint64_t> g_uiBeats{0};

    struct StubRecord
    {
        const char *name;
        uint32_t address;
        uint32_t a0;
        uint32_t a1;
        uint32_t a2;
        uint32_t raOrV0;
        uint32_t hostThread;
        bool leaving;
        uint64_t seq;
    };
    constexpr size_t kStubRing = 64;
    std::array<StubRecord, kStubRing> g_stubRing{};
    std::atomic<uint64_t> g_stubSeq{0};

    std::thread g_thread;
    std::mutex g_stopMutex;
    std::condition_variable g_stopCv;
    bool g_stopRequested = false;
    uint32_t g_mainThreadId = 0;

#if PS2X_WATCHDOG_STACKS
    void dumpHostThreads()
    {
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            std::fprintf(stderr, "[watchdog] thread snapshot failed\n");
            return;
        }
        std::vector<DWORD> ids;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snap, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self)
                {
                    ids.push_back(entry.th32ThreadID);
                }
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snap, &entry));
        }
        CloseHandle(snap);

        constexpr size_t kMaxFrames = 48;
        for (DWORD id : ids)
        {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                       FALSE, id);
            if (!thread)
            {
                continue;
            }
            const std::string name = ps2x_hoststack::threadName(thread);
            uint64_t frames[kMaxFrames];
            size_t count = 0;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                count = ps2x_hoststack::walkSuspendedThread(thread, frames, kMaxFrames);
                ResumeThread(thread);
            }
            CloseHandle(thread);
            std::fprintf(stderr, "[watchdog] host thread %lu%s%s%s (%zu frames)\n", id,
                         name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"",
                         count);
            if (id == g_mainThreadId)
            {
                std::fprintf(stderr, "[watchdog]   (window/main thread)\n");
            }
            for (size_t i = 0; i < count; ++i)
            {
                std::fprintf(stderr, "[watchdog]   #%02zu %s\n", i, ps2x_hoststack::describeAddress(frames[i]).c_str());
            }
        }
    }
#else
    void dumpHostThreads()
    {
        std::fprintf(stderr, "[watchdog] host stacks: only implemented on Windows x64\n");
    }
#endif

    void watchdogMain(int seconds, std::function<uint64_t()> vsyncCount, std::function<std::string()> guestReport)
    {
        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        uint64_t lastUi = g_uiBeats.load(std::memory_order_relaxed);
        uint64_t lastVsync = vsyncCount();
        auto lastUiChange = clock::now();
        auto lastVsyncChange = clock::now();
        auto lastReport = clock::time_point{};
        int reports = 0;
        const auto limit = std::chrono::seconds(seconds);
        // PS2X_WATCHDOG_GUEST_EVERY=<s>: also print the guest report (EE threads, semaphores,
        // event flags) every <s> seconds while nothing is stalled: for a guest that keeps its
        // VBlank going but waits on something forever.
        const int everySeconds = []
        {
            const char *value = std::getenv("PS2X_WATCHDOG_GUEST_EVERY");
            return value && *value ? std::max(0, std::atoi(value)) : 0;
        }();
        auto lastPeriodic = clock::now();

        std::unique_lock lock(g_stopMutex);
        while (!g_stopCv.wait_for(lock, std::chrono::milliseconds(250), []
                                  { return g_stopRequested; }))
        {
            const auto now = clock::now();
            const uint64_t ui = g_uiBeats.load(std::memory_order_relaxed);
            const uint64_t vsync = vsyncCount();
            if (ui != lastUi)
            {
                lastUi = ui;
                lastUiChange = now;
            }
            if (vsync != lastVsync)
            {
                lastVsync = vsync;
                lastVsyncChange = now;
            }
            if (everySeconds > 0 && now - lastPeriodic >= std::chrono::seconds(everySeconds))
            {
                lastPeriodic = now;
                lock.unlock();
                const std::string guest = guestReport ? guestReport() : std::string();
                std::fprintf(stderr, "[watchdog] periodic guest report at hostMs=%lld vsync=%llu\n%s",
                             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count()),
                             static_cast<unsigned long long>(vsync), guest.c_str());
                std::fflush(stderr);
                lock.lock();
            }
            const bool uiStalled = now - lastUiChange >= limit;
            const bool vsyncStalled = now - lastVsyncChange >= limit;
            if (!uiStalled && !vsyncStalled)
            {
                reports = 0;
                continue;
            }
            if (reports >= 4 || (reports > 0 && now - lastReport < limit * 4))
            {
                continue;
            }
            ++reports;
            lastReport = now;
            lock.unlock();
            const auto ms = [&](clock::time_point t)
            { return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count()); };
            std::fprintf(stderr,
                         "[watchdog] STALL report %d at hostMs=%lld: window loop %s (frame %llu, %lld ms since it moved), "
                         "guest VBlank %s (%llu, %lld ms since it moved)\n",
                         reports, ms(start), uiStalled ? "STUCK" : "running",
                         static_cast<unsigned long long>(ui), ms(lastUiChange),
                         vsyncStalled ? "STUCK" : "running", static_cast<unsigned long long>(vsync),
                         ms(lastVsyncChange));
            dumpHostThreads();
            ps2x_watchdog::dumpStubRing("[watchdog]");
            // Last: the guest report takes the scheduler's snapshot lock.
            const std::string guest = guestReport ? guestReport() : std::string();
            std::fprintf(stderr, "%s[watchdog] end of report %d\n", guest.c_str(), reports);
            std::fflush(stderr);
            lock.lock();
        }
    }
}

namespace ps2x_watchdog
{
    bool enabled() noexcept
    {
        return g_enabled.load(std::memory_order_relaxed);
    }

    void start(std::function<uint64_t()> vsyncCount, std::function<std::string()> guestReport)
    {
        const int seconds = watchdogSeconds();
        if (seconds <= 0 || g_thread.joinable())
        {
            return;
        }
#if defined(_WIN32)
        g_mainThreadId = GetCurrentThreadId();
#endif
        g_stopRequested = false;
        g_enabled.store(true, std::memory_order_relaxed);
        g_stubRingEnabled.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[watchdog] on: report when the window loop or guest VBlank stalls for %d s\n", seconds);
        g_thread = std::thread(watchdogMain, seconds, std::move(vsyncCount), std::move(guestReport));
    }

    void stop()
    {
        if (!g_thread.joinable())
        {
            return;
        }
        {
            std::lock_guard lock(g_stopMutex);
            g_stopRequested = true;
        }
        g_stopCv.notify_all();
        g_thread.join();
        g_enabled.store(false, std::memory_order_relaxed);
    }

    void uiHeartbeat() noexcept
    {
        g_uiBeats.fetch_add(1, std::memory_order_relaxed);
    }

    void noteStub(const char *name, uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2,
                  uint32_t raOrV0, bool leaving) noexcept
    {
        if (!g_stubRingEnabled.load(std::memory_order_relaxed))
        {
            return;
        }
        const uint64_t seq = g_stubSeq.fetch_add(1, std::memory_order_acq_rel);
        StubRecord &r = g_stubRing[seq % kStubRing];
        r.name = name;
        r.address = address;
        r.a0 = a0;
        r.a1 = a1;
        r.a2 = a2;
        r.raOrV0 = raOrV0;
#if defined(_WIN32)
        r.hostThread = GetCurrentThreadId();
#else
        r.hostThread = 0;
#endif
        r.leaving = leaving;
        r.seq = seq + 1;
    }

    void enableStubRing() noexcept
    {
        g_stubRingEnabled.store(true, std::memory_order_relaxed);
    }

    bool stubRingEnabled() noexcept
    {
        return g_stubRingEnabled.load(std::memory_order_relaxed);
    }

    void dumpStubRing(const char *prefix) noexcept
    {
        const uint64_t end = g_stubSeq.load(std::memory_order_acquire);
        const uint64_t begin = end > kStubRing ? end - kStubRing : 0;
        std::fprintf(stderr, "%s last %llu runtime-bound SDK calls (oldest first, '<' = return):\n", prefix,
                     static_cast<unsigned long long>(end - begin));
        for (uint64_t seq = begin; seq < end; ++seq)
        {
            const StubRecord &r = g_stubRing[seq % kStubRing];
            if (r.seq != seq + 1)
            {
                continue;
            }
            if (r.leaving)
            {
                std::fprintf(stderr, "%s   %llu < %s@0x%x v0=0x%x [host %u]\n", prefix,
                             static_cast<unsigned long long>(seq), r.name ? r.name : "?", r.address, r.raOrV0,
                             r.hostThread);
            }
            else
            {
                char caller[160];
                ps2x_names::format(caller, sizeof(caller), r.raOrV0);
                std::fprintf(stderr, "%s   %llu > %s@0x%x(a0=0x%x a1=0x%x a2=0x%x) ra=0x%x (%s) [host %u]\n", prefix,
                             static_cast<unsigned long long>(seq), r.name ? r.name : "?", r.address, r.a0, r.a1,
                             r.a2, r.raOrV0, caller, r.hostThread);
            }
        }
    }
}
