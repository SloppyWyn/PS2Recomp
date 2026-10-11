#pragma once

// Frame-pacing log (debug aid). PS2X_PACE_LOG=<file> writes one line per pacing-relevant event
// with the host time in microseconds since the first event: VBlank start/end deliveries (how
// late against their host deadline), scheduler pacing waits (which guest thread was held and for
// how long), idle waits, thread priority changes, DMA kicks (host time spent inside), and per
// VBlank the guest cycles charged by code vs. added by the real-time catch-up. Free when unset
// (one predictable branch on a cached flag).
//
//     python tools/pacelog.py <file>     per-frame table and 30/60 fps stretches

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace ps2x::pace
{
    struct Log
    {
        std::FILE *file = nullptr;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::mutex mutex;

        Log()
        {
            const char *path = std::getenv("PS2X_PACE_LOG");
            if (!path || !*path || (path[0] == '0' && path[1] == '\0'))
                return;
            file = std::fopen(path, "w");
            if (file)
            {
                std::setvbuf(file, nullptr, _IOFBF, 1 << 20);
                std::fprintf(stderr, "[pace] logging to %s\n", path);
            }
        }
        ~Log()
        {
            if (file)
                std::fclose(file);
        }
    };

    inline Log &log()
    {
        static Log instance;
        return instance;
    }

    inline bool enabled()
    {
        static const bool on = log().file != nullptr;
        return on;
    }

    inline int64_t usSince(std::chrono::steady_clock::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(t - log().start).count();
    }

    inline int64_t nowUs()
    {
        return usSince(std::chrono::steady_clock::now());
    }

    // One line: "<us> <text>\n".
    inline void line(const char *fmt, ...)
    {
        Log &l = log();
        if (!l.file)
            return;
        const int64_t t = nowUs();
        std::lock_guard<std::mutex> lock(l.mutex);
        std::fprintf(l.file, "%lld ", static_cast<long long>(t));
        va_list args;
        va_start(args, fmt);
        std::vfprintf(l.file, fmt, args);
        va_end(args);
        std::fputc('\n', l.file);
    }

    inline void flush()
    {
        Log &l = log();
        if (!l.file)
            return;
        std::lock_guard<std::mutex> lock(l.mutex);
        std::fflush(l.file);
    }
}
