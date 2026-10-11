#include "poison_guard.h"
#include "ps2_host_stack.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>

#if defined(_WIN32) && defined(_M_X64)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h>

namespace
{
    constexpr size_t kMaxHits = 512;

    std::atomic<uint8_t *> g_page{nullptr};
    std::atomic<bool> g_armed{false};
    std::atomic<size_t> g_hitCount{0};
    std::atomic<size_t> g_dropped{0};
    poison_guard::Hit g_hits[kMaxHits];
    thread_local bool t_reprotect = false;
    bool g_installed = false;

    // CRT functions given garbage (fopen with an empty mode, ...) return EINVAL instead of
    // ending the process.
    void ignoreInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t) {}

    void protect(uint8_t *page, DWORD mode)
    {
        DWORD old = 0;
        VirtualProtect(page, poison_guard::kPageSize, mode, &old);
    }

    LONG CALLBACK onException(EXCEPTION_POINTERS *ep)
    {
        EXCEPTION_RECORD *er = ep->ExceptionRecord;
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        {
            uint8_t *page = g_page.load();
            const uintptr_t addr = static_cast<uintptr_t>(er->ExceptionInformation[1]);
            const uintptr_t base = reinterpret_cast<uintptr_t>(page);
            if (page && g_armed.load() && addr >= base && addr < base + poison_guard::kPageSize)
            {
                const size_t index = g_hitCount.fetch_add(1);
                if (index < kMaxHits)
                {
                    poison_guard::Hit &h = g_hits[index];
                    h.offset = static_cast<uint32_t>(addr - base);
                    h.write = er->ExceptionInformation[0] == 1;
                    h.frames[0] = reinterpret_cast<uint64_t>(er->ExceptionAddress);
                    h.frameCount = 1u + static_cast<uint32_t>(ps2x_hoststack::walkContext(
                        ep->ContextRecord, h.frames + 1, poison_guard::kMaxFrames - 1));
                }
                else
                {
                    g_hitCount.fetch_sub(1);
                    g_dropped.fetch_add(1);
                }
                protect(page, PAGE_READWRITE);
                ep->ContextRecord->EFlags |= 0x100; // trap flag: re-protect after this access
                t_reprotect = true;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_reprotect)
        {
            t_reprotect = false;
            uint8_t *page = g_page.load();
            if (page && g_armed.load())
            {
                protect(page, PAGE_NOACCESS);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    _invalid_parameter_handler g_previousInvalidParameter = nullptr;
    int g_realOut = -1;
    int g_realErr = -1;
    bool g_silenced = false;

    int callSeh(void (*fn)(void *), void *arg, uint32_t *code)
    {
        __try
        {
            fn(arg);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *code = GetExceptionCode();
            return 1;
        }
        return 0;
    }
}

namespace poison_guard
{
    bool supported() noexcept { return true; }

    void install()
    {
        if (!g_installed)
        {
            AddVectoredExceptionHandler(1, onException);
            g_installed = true;
        }
    }

    void arm(uint8_t *page)
    {
        g_hitCount.store(0);
        g_dropped.store(0);
        g_page.store(page);
        g_armed.store(true);
        protect(page, PAGE_NOACCESS);
    }

    void disarm()
    {
        g_armed.store(false);
        if (uint8_t *page = g_page.load())
        {
            protect(page, PAGE_READWRITE);
        }
    }

    size_t hitCount() noexcept { return g_hitCount.load(); }
    const Hit &hit(size_t index) noexcept { return g_hits[index]; }
    size_t droppedHits() noexcept { return g_dropped.load(); }

    uint32_t callGuarded(void (*fn)(void *), void *arg)
    {
        uint32_t code = 0;
        callSeh(fn, arg, &code);
        return code;
    }

    void silenceStdio(bool on)
    {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        if (on == g_silenced)
        {
            return;
        }
        if (on)
        {
            g_previousInvalidParameter = _set_invalid_parameter_handler(ignoreInvalidParameter);
            if (g_realOut < 0)
            {
                // High numbers: handlers given small integers as file descriptors (close(3),
                // write(4, ...)) must not hit the saved streams.
                const int out = _dup(1);
                const int err = _dup(2);
                g_realOut = (_dup2(out, 1001) == 0) ? 1001 : out;
                g_realErr = (_dup2(err, 1002) == 0) ? 1002 : err;
                if (g_realOut != out)
                    _close(out);
                if (g_realErr != err)
                    _close(err);
            }
            const int nul = _open("NUL", _O_WRONLY);
            if (nul >= 0)
            {
                _dup2(nul, 1);
                _dup2(nul, 2);
                _close(nul);
            }
        }
        else
        {
            _dup2(g_realOut, 1);
            _dup2(g_realErr, 2);
            _set_invalid_parameter_handler(g_previousInvalidParameter);
        }
        g_silenced = on;
    }

    void writeRealStderr(const char *text)
    {
        const int fd = g_silenced ? g_realErr : 2;
        std::fflush(stderr);
        _write(fd, text, static_cast<unsigned>(std::strlen(text)));
    }

    void writeRealStdout(const char *text)
    {
        const int fd = g_silenced ? g_realOut : 1;
        std::cout.flush();
        std::fflush(stdout);
        _write(fd, text, static_cast<unsigned>(std::strlen(text)));
    }
}

#else

namespace poison_guard
{
    namespace
    {
        Hit g_none{};
    }
    bool supported() noexcept { return false; }
    void install() {}
    void arm(uint8_t *) {}
    void disarm() {}
    size_t hitCount() noexcept { return 0; }
    const Hit &hit(size_t) noexcept { return g_none; }
    size_t droppedHits() noexcept { return 0; }
    uint32_t callGuarded(void (*fn)(void *), void *arg)
    {
        fn(arg);
        return 0;
    }
    void silenceStdio(bool)
    {
        std::cout.flush();
        std::cerr.flush();
    }
    void writeRealStderr(const char *text) { std::fputs(text, stderr); }
    void writeRealStdout(const char *text)
    {
        std::cout.flush();
        std::fputs(text, stdout);
        std::fflush(stdout);
    }
}

#endif
