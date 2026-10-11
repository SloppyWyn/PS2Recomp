// Sampling profiler for the runner's own host threads (ps2_host_profile.h).
//
// A background thread suspends each host thread of the process every PS2X_HOST_PROFILE ms,
// walks its stack (ps2_host_stack: no allocation while the target is suspended), resumes it,
// and only then names the frames (names are cached per address). Per thread it counts, for
// each function, the samples where it was on top (self) and anywhere on the stack
// (inclusive, once per sample). The report goes to stderr (and PS2X_HOST_PROFILE_FILE) when
// the process exits, or every PS2X_HOST_PROFILE_EVERY seconds. Windows x64 only.

#include "ps2_host_profile.h"
#include "ps2_host_stack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace
{
    struct ThreadProfile
    {
        std::string name;
        uint64_t samples = 0;
        std::unordered_map<std::string, uint64_t> self;
        std::unordered_map<std::string, uint64_t> inclusive;
    };

    std::mutex g_mutex;
    std::map<uint32_t, ThreadProfile> g_threads;
    std::unordered_map<uint64_t, std::string> g_names; // address -> function (offset dropped)
    std::atomic<bool> g_running{false};
    uint32_t g_samplerThreadId = 0;
    std::string g_file;

    // "ps2EntryRunner+0x1234 FUN_00143408_0x143408+0x2c" -> "FUN_00143408_0x143408"
    std::string functionOf(uint64_t address)
    {
        auto found = g_names.find(address);
        if (found != g_names.end())
            return found->second;
        std::string text = ps2x_hoststack::describeAddress(address);
        const size_t space = text.find(' ');
        std::string name = space == std::string::npos ? text : text.substr(space + 1);
        const size_t plus = name.rfind("+0x");
        if (plus != std::string::npos && plus > 0)
            name.resize(plus);
        g_names.emplace(address, name);
        return name;
    }

    void writeReport(std::FILE *out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::fprintf(out, "[hostprof] report: %zu threads sampled\n", g_threads.size());
        std::vector<const std::pair<const uint32_t, ThreadProfile> *> order;
        for (const auto &entry : g_threads)
            order.push_back(&entry);
        std::sort(order.begin(), order.end(), [](auto *a, auto *b) { return a->second.samples > b->second.samples; });
        for (const auto *entry : order)
        {
            const ThreadProfile &t = entry->second;
            if (t.samples == 0)
                continue;
            std::fprintf(out, "[hostprof] thread %u \"%s\": %llu samples\n", entry->first, t.name.c_str(),
                         static_cast<unsigned long long>(t.samples));
            for (const auto *table : {&t.self, &t.inclusive})
            {
                std::vector<std::pair<std::string, uint64_t>> rows(table->begin(), table->end());
                std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                const bool self = table == &t.self;
                for (size_t i = 0; i < rows.size() && i < (self ? 25u : 40u); ++i)
                {
                    std::fprintf(out, "[hostprof]   %s %5.1f%%  %s\n", self ? "self" : "incl",
                                 100.0 * static_cast<double>(rows[i].second) / static_cast<double>(t.samples),
                                 rows[i].first.c_str());
                }
            }
        }
        std::fflush(out);
    }

    void report()
    {
        writeReport(stderr);
        if (!g_file.empty())
        {
            if (std::FILE *f = std::fopen(g_file.c_str(), "w"))
            {
                writeReport(f);
                std::fclose(f);
            }
        }
    }

#ifdef _WIN32
    std::vector<DWORD> processThreads()
    {
        std::vector<DWORD> ids;
        const DWORD pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return ids;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snap, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != g_samplerThreadId)
                    ids.push_back(entry.th32ThreadID);
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snap, &entry));
        }
        CloseHandle(snap);
        return ids;
    }

    void samplerMain(unsigned intervalMs, unsigned reportEverySeconds)
    {
        g_samplerThreadId = GetCurrentThreadId();
        constexpr size_t kMaxFrames = 64;
        struct Open
        {
            HANDLE handle;
            std::string name;
        };
        std::unordered_map<DWORD, Open> open;
        auto lastEnumerate = std::chrono::steady_clock::time_point{};
        auto lastReport = std::chrono::steady_clock::now();
        uint64_t frames[kMaxFrames];
        while (g_running.load(std::memory_order_acquire))
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastEnumerate > std::chrono::seconds(1))
            {
                lastEnumerate = now;
                for (DWORD id : processThreads())
                {
                    if (open.count(id))
                        continue;
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
                    if (h)
                        open.emplace(id, Open{h, ps2x_hoststack::threadName(h)});
                }
            }
            for (auto it = open.begin(); it != open.end();)
            {
                DWORD code = 0;
                if (!GetExitCodeThread(it->second.handle, &code) || code != STILL_ACTIVE)
                {
                    CloseHandle(it->second.handle);
                    it = open.erase(it);
                    continue;
                }
                size_t count = 0;
                if (SuspendThread(it->second.handle) != static_cast<DWORD>(-1))
                {
                    count = ps2x_hoststack::walkSuspendedThread(it->second.handle, frames, kMaxFrames);
                    ResumeThread(it->second.handle);
                }
                if (count != 0)
                {
                    // Named after the resume: naming allocates and may take locks the target held.
                    std::lock_guard<std::mutex> lock(g_mutex);
                    ThreadProfile &t = g_threads[it->first];
                    if (t.name.empty())
                        t.name = it->second.name.empty() ? "?" : it->second.name;
                    ++t.samples;
                    std::unordered_set<std::string> seen;
                    for (size_t i = 0; i < count; ++i)
                    {
                        const std::string fn = functionOf(frames[i]);
                        if (i == 0)
                            ++t.self[fn];
                        if (seen.insert(fn).second)
                            ++t.inclusive[fn];
                    }
                }
                ++it;
            }
            if (reportEverySeconds != 0 && now - lastReport > std::chrono::seconds(reportEverySeconds))
            {
                lastReport = now;
                report();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }
        for (auto &entry : open)
            CloseHandle(entry.second.handle);
    }
#endif
}

namespace ps2x_host_profile
{
    void startFromEnvironment()
    {
#ifdef _WIN32
        const char *value = std::getenv("PS2X_HOST_PROFILE");
        if (!value || !*value || std::strcmp(value, "0") == 0 || g_running.exchange(true))
            return;
        unsigned interval = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
        if (interval == 0u || interval > 1000u)
            interval = 2u;
        unsigned every = 0u;
        if (const char *e = std::getenv("PS2X_HOST_PROFILE_EVERY"); e && *e)
            every = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
        if (const char *f = std::getenv("PS2X_HOST_PROFILE_FILE"); f && *f)
            g_file = f;
        ps2x_hoststack::preload();
        std::fprintf(stderr, "[hostprof] sampling every %u ms%s\n", interval, g_file.empty() ? "" : (" -> " + g_file).c_str());
        std::thread(samplerMain, interval, every).detach();
        std::atexit([] { stop(); });
#endif
    }

    void stop()
    {
        static std::atomic<bool> s_done{false};
        if (!g_running.exchange(false) || s_done.exchange(true))
            return;
        // Let the sampler leave its loop (it never holds a thread suspended across a sleep).
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        report();
    }
}
