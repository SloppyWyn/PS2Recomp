#pragma once
// Host-side helpers for the stub poison test (ps2_stub_poison_tests.cpp), kept apart so
// <windows.h> never meets the runtime's handler names (CreateThread, read, write, ...).
//
// - A read/write trap on one host page of guest RAM: the page is made inaccessible while a
//   handler runs; each access is recorded (offset into the page, read or write, host
//   frames) and then let through with a single step, after which the page is protected again.
//   Windows x64 only; elsewhere supported() is false and nothing is recorded.
// - A call wrapper that turns a native crash (access violation, ...) into an exception code.
// - Silencing of stdout/stderr (handlers log a lot) with a way to still print to the real
//   streams.
#include <cstddef>
#include <cstdint>

namespace poison_guard
{
    constexpr size_t kPageSize = 4096;
    constexpr size_t kMaxFrames = 10;

    struct Hit
    {
        uint32_t offset;  // byte offset of the access into the guarded page
        bool write;
        uint32_t frameCount;
        uint64_t frames[kMaxFrames];
    };

    bool supported() noexcept;

    // Installs the vectored exception handler (once).
    void install();

    // Protects [page, page + kPageSize); page must be page aligned. Clears the hit list.
    void arm(uint8_t *page);
    // Unprotects the page. Hits stay readable until the next arm().
    void disarm();
    size_t hitCount() noexcept;
    const Hit &hit(size_t index) noexcept;
    size_t droppedHits() noexcept;

    // Runs fn(arg); returns 0, or the code of a native exception that escaped fn.
    uint32_t callGuarded(void (*fn)(void *), void *arg);

    // Sends fds 1 and 2 to the null device (true) or back (false). Flushes C and C++ streams.
    // While on, CRT functions given bad arguments (fopen with an empty mode) return EINVAL
    // instead of ending the process.
    void silenceStdio(bool on);
    // Writes to the real stderr / stdout even while silenced.
    void writeRealStderr(const char *text);
    void writeRealStdout(const char *text);
}
