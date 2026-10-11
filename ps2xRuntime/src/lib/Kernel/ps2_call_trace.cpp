// Filtered guest call trace: see include/ps2_call_trace.h.
#include "ps2_call_trace.h"
#include "ps2_guest_names.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace ps2x_calltrace
{
    std::atomic<bool> g_active{false};

    namespace
    {
        struct State
        {
            bool initialized = false;
            bool all = false;
            bool returns = false;
            std::vector<uint64_t> bitmap; // one bit per word of the dense function table
            uint32_t bitmapBase = 0;
            uint32_t bitmapSlots = 0;
            std::vector<std::pair<uint32_t, uint32_t>> otherRanges; // outside the table
            FILE *out = nullptr;
            bool ownsFile = false;
            uint64_t maxLines = 1000000;
            std::atomic<uint64_t> lines{0};
            std::atomic<bool> capped{false};
        };

        // Never destroyed: the flusher thread and a crash report may still use it while
        // static destructors run.
        State &state()
        {
            static State *s = new State();
            return *s;
        }

        std::mutex g_initMutex;
        std::mutex g_fileMutex; // s.out against flusher / flush / shutdown
        std::mutex g_flusherMutex;
        std::condition_variable g_flusherCv;
        bool g_flusherStop = false;
        std::thread g_flusher;
        thread_local uint32_t t_pendingJump = 0xFFFFFFFFu;

        std::string trim(const std::string &text)
        {
            size_t a = 0;
            size_t b = text.size();
            while (a < b && (text[a] == ' ' || text[a] == '\t'))
                ++a;
            while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t'))
                --b;
            return text.substr(a, b - a);
        }

        bool parseNumber(const std::string &text, uint32_t &value)
        {
            if (text.empty())
            {
                return false;
            }
            char *end = nullptr;
            const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
            if (!end || *end != '\0' || parsed > 0xFFFFFFFFull)
            {
                return false;
            }
            value = static_cast<uint32_t>(parsed);
            return true;
        }

        bool looksNumeric(const std::string &text)
        {
            return !text.empty() && (text[0] >= '0' && text[0] <= '9');
        }

        void flusherMain()
        {
            std::unique_lock lock(g_flusherMutex);
            while (!g_flusherCv.wait_for(lock, std::chrono::milliseconds(200), []
                                         { return g_flusherStop; }))
            {
                flush();
            }
        }

        void writeLine(const char *text, int length)
        {
            State &s = state();
            if (length <= 0 || !s.out)
            {
                return;
            }
            const uint64_t n = s.lines.fetch_add(1, std::memory_order_relaxed);
            if (n >= s.maxLines)
            {
                if (!s.capped.exchange(true, std::memory_order_relaxed))
                {
                    std::fprintf(s.out, "[calltrace] cap of %llu lines reached (PS2X_TRACE_CALLS_MAX), trace stopped\n",
                                 static_cast<unsigned long long>(s.maxLines));
                    std::fflush(s.out);
                    g_active.store(false, std::memory_order_relaxed);
                }
                return;
            }
            std::fwrite(text, 1, static_cast<size_t>(length), s.out);
        }
    }

    bool parseSpec(const std::string &spec, std::vector<std::pair<uint32_t, uint32_t>> &ranges, bool &all,
                   std::string *error)
    {
        all = false;
        size_t start = 0;
        while (start <= spec.size())
        {
            const size_t comma = spec.find(',', start);
            const std::string item = trim(spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            start = comma == std::string::npos ? spec.size() + 1 : comma + 1;
            if (item.empty())
            {
                continue;
            }
            if (item == "*" || item == "1" || item == "all")
            {
                all = true;
                continue;
            }
            const size_t dash = item.find('-');
            if (looksNumeric(item) && dash != std::string::npos)
            {
                uint32_t a = 0;
                uint32_t b = 0;
                if (!parseNumber(trim(item.substr(0, dash)), a) || !parseNumber(trim(item.substr(dash + 1)), b) || b <= a)
                {
                    if (error)
                        *error = "bad range '" + item + "'";
                    return false;
                }
                ranges.emplace_back(a, b);
                continue;
            }
            if (looksNumeric(item))
            {
                uint32_t a = 0;
                if (!parseNumber(item, a))
                {
                    if (error)
                        *error = "bad address '" + item + "'";
                    return false;
                }
                const PS2GuestFunctionName *e = ps2x_names::mainTable().find(a);
                if (e && e->end > e->start)
                {
                    ranges.emplace_back(e->start, e->end);
                }
                else
                {
                    ranges.emplace_back(a, a + 4u);
                }
                continue;
            }
            const size_t before = ranges.size();
            ps2x_names::findByName(item, ranges);
            if (ranges.size() == before && error)
            {
                *error += (error->empty() ? "" : "; ") + std::string("no function name contains '") + item + "'";
            }
        }
        return true;
    }

    void init()
    {
        std::lock_guard lock(g_initMutex);
        State &s = state();
        if (s.initialized)
        {
            return;
        }
        s.initialized = true;
        const char *spec = std::getenv("PS2X_TRACE_CALLS");
        if (!spec || !*spec || std::strcmp(spec, "0") == 0)
        {
            return;
        }
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        std::string error;
        if (!parseSpec(spec, ranges, s.all, &error))
        {
            std::fprintf(stderr, "[calltrace] PS2X_TRACE_CALLS: %s; trace off\n", error.c_str());
            return;
        }
        if (!error.empty())
        {
            std::fprintf(stderr, "[calltrace] note: %s\n", error.c_str());
        }
        if (!s.all && ranges.empty())
        {
            std::fprintf(stderr, "[calltrace] PS2X_TRACE_CALLS selects nothing; trace off\n");
            return;
        }

        s.bitmapBase = g_ps2RecompiledFunctionTableBase;
        s.bitmapSlots = g_ps2RecompiledFunctionTableSlotCount;
        s.bitmap.assign((static_cast<size_t>(s.bitmapSlots) + 63u) / 64u, 0ull);
        const uint64_t tableEnd = static_cast<uint64_t>(s.bitmapBase) + static_cast<uint64_t>(s.bitmapSlots) * 4u;
        for (const auto &[a, b] : ranges)
        {
            // The part inside the table goes in the bitmap, the rest stays a range.
            const uint64_t lo = std::max<uint64_t>(a, s.bitmapBase);
            const uint64_t hi = std::min<uint64_t>(b, tableEnd);
            if (lo < hi)
            {
                for (uint64_t slot = (lo - s.bitmapBase) >> 2; slot < ((hi - s.bitmapBase + 3u) >> 2); ++slot)
                {
                    s.bitmap[slot >> 6] |= 1ull << (slot & 63u);
                }
            }
            if (a < s.bitmapBase)
            {
                s.otherRanges.emplace_back(a, static_cast<uint32_t>(std::min<uint64_t>(b, s.bitmapBase)));
            }
            if (b > tableEnd)
            {
                s.otherRanges.emplace_back(static_cast<uint32_t>(std::max<uint64_t>(a, tableEnd)), b);
            }
        }

        if (const char *max = std::getenv("PS2X_TRACE_CALLS_MAX"); max && *max)
        {
            s.maxLines = std::strtoull(max, nullptr, 0);
        }
        const char *ret = std::getenv("PS2X_TRACE_CALLS_RET");
        s.returns = ret && *ret && std::strcmp(ret, "0") != 0;

        const char *file = std::getenv("PS2X_TRACE_CALLS_FILE");
        const std::string path = (file && *file) ? file : "ps2x_calls.txt";
        if (path == "-" || path == "stderr")
        {
            s.out = stderr;
        }
        else
        {
            s.out = std::fopen(path.c_str(), "w");
            if (!s.out)
            {
                std::fprintf(stderr, "[calltrace] cannot open %s; trace off\n", path.c_str());
                return;
            }
            s.ownsFile = true;
            std::setvbuf(s.out, nullptr, _IOFBF, 1u << 20);
        }
        std::fprintf(s.out, "[calltrace] filter \"%s\": %s%zu ranges, max %llu lines%s\n", spec,
                     s.all ? "everything, " : "", ranges.size(), static_cast<unsigned long long>(s.maxLines),
                     s.returns ? ", returns" : "");
        std::fprintf(stderr, "[calltrace] on: \"%s\" -> %s (%zu ranges)\n", spec, path.c_str(), ranges.size());
        g_flusher = std::thread(flusherMain);
        std::atexit([]
                    { shutdown(); });
        g_active.store(true, std::memory_order_release);
    }

    bool matches(uint32_t target) noexcept
    {
        const State &s = state();
        if (s.all)
        {
            return true;
        }
        const uint32_t offset = target - s.bitmapBase;
        if (target >= s.bitmapBase && (offset >> 2) < s.bitmapSlots)
        {
            const uint32_t slot = offset >> 2;
            return (s.bitmap[slot >> 6] >> (slot & 63u)) & 1u;
        }
        for (const auto &[a, b] : s.otherRanges)
        {
            if (target >= a && target < b)
            {
                return true;
            }
        }
        return false;
    }

    bool logReturns() noexcept
    {
        return state().returns;
    }

    bool enter(const R5900Context *ctx, uint32_t target, uint32_t source, Kind kind, int guestThread, uint64_t vsync)
    {
        if (!active() || !ctx || !matches(target))
        {
            return false;
        }
        if (kind == Kind::Jump)
        {
            t_pendingJump = target; // the scheduler runs it next: not a second line
        }
        char targetName[160];
        char raName[160];
        ps2x_names::format(targetName, sizeof(targetName), target);
        const uint32_t ra = getRegU32(ctx, 31);
        ps2x_names::format(raName, sizeof(raName), kind == Kind::Call ? ra : (source ? source : ra));
        const char *kindName = kind == Kind::Call ? "call" : kind == Kind::Jump ? "jump" : "sched";
        char line[512];
        const int n = std::snprintf(line, sizeof(line),
                                    "> %llu t%d %s %s a0=%x a1=%x a2=%x a3=%x %s=%x (%s) sp=%x\n",
                                    static_cast<unsigned long long>(vsync), guestThread, kindName, targetName,
                                    getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7),
                                    kind == Kind::Call || source == 0 ? "ra" : "from",
                                    kind == Kind::Call || source == 0 ? ra : source, raName, getRegU32(ctx, 29));
        writeLine(line, std::min(n, static_cast<int>(sizeof(line)) - 1));
        return true;
    }

    void schedulerEnter(const R5900Context *ctx, uint32_t target, int guestThread, uint64_t vsync)
    {
        if (t_pendingJump == target)
        {
            t_pendingJump = 0xFFFFFFFFu;
            return;
        }
        t_pendingJump = 0xFFFFFFFFu;
        if (!active() || !matches(target))
        {
            return;
        }
        ps2x_names::Resolved r;
        if (!ps2x_names::resolve(target, r) || r.start != target || !r.name ||
            (r.flags & PS2X_GUEST_NAME_FRAGMENT) != 0u)
        {
            return; // a resume inside a function or a fragment, not an entry
        }
        enter(ctx, target, 0u, Kind::Sched, guestThread, vsync);
    }

    void leave(const R5900Context *ctx, uint32_t target)
    {
        if (!active() || !ctx)
        {
            return;
        }
        char targetName[160];
        ps2x_names::format(targetName, sizeof(targetName), target);
        char line[256];
        const int n = std::snprintf(line, sizeof(line), "< %s v0=%x v1=%x\n", targetName, getRegU32(ctx, 2),
                                    getRegU32(ctx, 3));
        writeLine(line, std::min(n, static_cast<int>(sizeof(line)) - 1));
    }

    void flush() noexcept
    {
        std::unique_lock lock(g_fileMutex, std::try_to_lock); // never block a crash report
        State &s = state();
        if (lock.owns_lock() && s.out)
        {
            std::fflush(s.out);
        }
    }

    void shutdown() noexcept
    {
        g_active.store(false, std::memory_order_release);
        {
            std::lock_guard lock(g_flusherMutex);
            g_flusherStop = true;
        }
        g_flusherCv.notify_all();
        if (g_flusher.joinable() && g_flusher.get_id() != std::this_thread::get_id())
        {
            g_flusher.join();
        }
        std::lock_guard lock(g_fileMutex);
        State &s = state();
        if (s.out)
        {
            std::fflush(s.out);
            if (s.ownsFile)
            {
                std::fclose(s.out);
            }
            s.out = nullptr;
        }
    }
}
