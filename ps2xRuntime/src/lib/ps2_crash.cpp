// Crash handler: see include/ps2_crash.h. Kept in its own file because it needs
// <windows.h>, which clashes with raylib's names in the runtime's other files.
#include "ps2_crash.h"
#include "ps2_call_trace.h"
#include "ps2_host_stack.h"
#include "ps2_watchdog.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

namespace
{
    std::atomic<bool> g_installed{false};
    std::function<std::string()> g_guestReport;

#if defined(_WIN32)
    LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
    std::atomic<DWORD> g_crashingThread{0};

    constexpr DWORD kCppExceptionCode = 0xE06D7363u; // MSVC C++ throw

    const char *exceptionName(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        case 0xC0000409u: return "STACK_BUFFER_OVERRUN/FAST_FAIL";
        case 0xC0000374u: return "HEAP_CORRUPTION";
        default: return "unknown";
        }
    }

    void writeMinidump(EXCEPTION_POINTERS *info)
    {
        const char *path = std::getenv("PS2X_CRASH_DUMP");
        if (!path || !*path)
        {
            return;
        }
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            std::fprintf(stderr, "[crash] cannot create minidump %s\n", path);
            return;
        }
        MINIDUMP_EXCEPTION_INFORMATION exception{};
        exception.ThreadId = GetCurrentThreadId();
        exception.ExceptionPointers = info;
        exception.ClientPointers = FALSE;
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                                     MiniDumpScanMemory | MiniDumpWithThreadInfo),
                                          &exception, nullptr, nullptr);
        CloseHandle(file);
        std::fprintf(stderr, "[crash] minidump %s %s\n", ok ? "written to" : "FAILED for", path);
    }

    void report(EXCEPTION_POINTERS *info)
    {
        const EXCEPTION_RECORD *record = info->ExceptionRecord;
        const DWORD code = record->ExceptionCode;
        const uint64_t where = reinterpret_cast<uint64_t>(record->ExceptionAddress);
        std::fprintf(stderr, "\n[crash] ===== unhandled exception 0x%08lx %s", code, exceptionName(code));
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2)
        {
            const ULONG_PTR kind = record->ExceptionInformation[0];
            std::fprintf(stderr, " (%s at 0x%llx)", kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute" : "access",
                         static_cast<unsigned long long>(record->ExceptionInformation[1]));
        }
        std::fprintf(stderr, " =====\n");
        std::fflush(stderr);

        char name[1400];
        if (!ps2x_hoststack::preloaded())
        {
            std::fprintf(stderr, "[crash] (symbols still loading: exe frames as offsets)\n");
        }
        ps2x_hoststack::describeAddressNoWait(where, name, sizeof(name));
        const std::string thread = ps2x_hoststack::threadName(GetCurrentThread());
        std::fprintf(stderr, "[crash] host thread %lu%s%s%s at %s\n", GetCurrentThreadId(),
                     thread.empty() ? "" : " \"", thread.c_str(), thread.empty() ? "" : "\"", name);

        constexpr size_t kMaxFrames = 64;
        uint64_t frames[kMaxFrames];
        CONTEXT context = *info->ContextRecord;
#if defined(_M_X64)
        if (context.Rip == 0 && context.Rsp != 0)
        {
            // A call through a null pointer: the return address is on top of the stack.
            context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
            context.Rsp += 8;
        }
#endif
        const size_t count = ps2x_hoststack::walkContext(&context, frames, kMaxFrames);
        std::fprintf(stderr, "[crash] host stack (%zu frames, innermost first; FUN_xxx_0xADDR / entry_xxx are guest code):\n", count);
        for (size_t i = 0; i < count; ++i)
        {
            ps2x_hoststack::describeAddressNoWait(frames[i], name, sizeof(name));
            std::fprintf(stderr, "[crash]   #%02zu %s\n", i, name);
        }
        std::fflush(stderr);

        if (g_guestReport)
        {
            const std::string guest = g_guestReport();
            std::fputs(guest.c_str(), stderr);
            std::fflush(stderr);
        }
        ps2x_watchdog::dumpStubRing("[crash]");
        writeMinidump(info);
    }

    // The report runs on a crashed process: if it blocks (a lock the crashed thread or a
    // frozen one holds: heap, loader, stdio), this thread, created at install, ends the
    // process kTimeoutMs after the report started. It writes with WriteFile, not stdio.
    constexpr DWORD kTimeoutMs = 10000;
    HANDLE g_reportStarted = nullptr;
    std::atomic<DWORD> g_exitCode{0};

    void rawStderr(const char *text)
    {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    }

    DWORD WINAPI timeoutMain(void *)
    {
        WaitForSingleObject(g_reportStarted, INFINITE);
        Sleep(kTimeoutMs);
        char text[160];
        std::snprintf(text, sizeof(text), "\n[crash] report did not finish within %lu ms; terminating with code 0x%08lx\n",
                      kTimeoutMs, g_exitCode.load());
        rawStderr(text);
        TerminateProcess(GetCurrentProcess(), g_exitCode.load());
        return 0;
    }

    LONG WINAPI crashFilter(EXCEPTION_POINTERS *info)
    {
        if (!info || !info->ExceptionRecord)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (info->ExceptionRecord->ExceptionCode == kCppExceptionCode)
        {
            // Unhandled C++ exception: the CRT's filter sends it to std::terminate.
            return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
        }
        const DWORD self = GetCurrentThreadId();
        DWORD expected = 0;
        if (!g_crashingThread.compare_exchange_strong(expected, self))
        {
            if (expected == self)
            {
                // Crashed again inside the report: give up on it.
                std::fprintf(stderr, "[crash] second exception 0x%08lx while reporting; exiting\n",
                             info->ExceptionRecord->ExceptionCode);
                std::fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
            }
            // Another thread is reporting: give it time to end the process, then end it.
            Sleep(kTimeoutMs + 5000);
            TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
        }

        g_exitCode.store(info->ExceptionRecord->ExceptionCode);
        if (g_reportStarted)
        {
            SetEvent(g_reportStarted);
        }
        report(info);

        const DWORD code = info->ExceptionRecord->ExceptionCode;
        std::fprintf(stderr, "[crash] ===== end of report, exiting with code 0x%08lx =====\n", code);
        ps2x_calltrace::flush();
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), code);
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif
}

namespace ps2x_crash
{
    void install(std::function<std::string()> guestReport)
    {
#if defined(_WIN32)
        const char *flag = std::getenv("PS2X_CRASH_HANDLER");
        if (flag && std::strcmp(flag, "0") == 0)
        {
            return;
        }
        if (g_installed.exchange(true))
        {
            return;
        }
        g_guestReport = std::move(guestReport);
        ps2x_watchdog::enableStubRing();
        ps2x_hoststack::preload(); // the map and dbghelp, off the crash path
        g_reportStarted = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (HANDLE timeout = CreateThread(nullptr, 64 * 1024, timeoutMain, nullptr, 0, nullptr))
        {
            CloseHandle(timeout);
        }
        prepareThread();
        g_previousFilter = SetUnhandledExceptionFilter(crashFilter);
#else
        (void)guestReport;
#endif
    }

    void prepareThread() noexcept
    {
#if defined(_WIN32)
        // Room for the report when the crash is a stack overflow.
        ULONG guarantee = 64u * 1024u;
        SetThreadStackGuarantee(&guarantee);
#endif
    }

    void testHoldLock(const char *which)
    {
#if defined(_WIN32)
        if (!which || !*which)
        {
            return;
        }
        if (std::strcmp(which, "symbols") == 0)
        {
            std::fprintf(stderr, "[crash-test] a thread now holds the symbol lock\n");
            ps2x_hoststack::testHoldSymbolLock();
        }
        else if (std::strcmp(which, "stderr") == 0)
        {
            std::fprintf(stderr, "[crash-test] a thread now holds the stderr lock\n");
            std::thread([]
                        {
                            _lock_file(stderr);
                            Sleep(INFINITE);
                        })
                .detach();
            Sleep(100);
        }
#else
        (void)which;
#endif
    }

    bool installed() noexcept
    {
        return g_installed.load();
    }
}
