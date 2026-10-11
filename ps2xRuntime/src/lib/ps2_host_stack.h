#pragma once
// Native (host) stack walking and symbol names, shared by the stall watchdog
// (ps2_watchdog.cpp) and the crash handler (ps2_crash.cpp). Windows x64 only; elsewhere
// the functions return nothing. Exe frames are named from the linker map beside the exe
// (<exe>.map, the runner links with /MAP; generated functions carry their guest address
// in their name, e.g. FUN_00100230_0x100230), system DLLs from their exports (dbghelp).
// Kept free of <windows.h> so it can be included anywhere.
#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2x_hoststack
{
    bool supported() noexcept;

    // "module+0xRVA symbol+0xOFF". Loads the map on first use (can take a few seconds
    // for a large exe, and allocates).
    std::string describeAddress(uint64_t address);

    // Load the map and start dbghelp on a background thread (the crash handler calls it
    // at install, so the crash path never has to).
    void preload();
    bool preloaded() noexcept;

    // For the crash path: never waits for the symbol lock and never loads anything; when
    // the lock is busy or preload() has not finished, prints the bare (exe-relative)
    // address. Returns the length written.
    size_t describeAddressNoWait(uint64_t address, char *text, size_t size);

    // Test only (PS2X_CRASH_TEST_HOLD=symbols): a thread takes the symbol lock and keeps it.
    void testHoldSymbolLock();

    // Frames of a thread that the caller has suspended (a HANDLE). Does not allocate.
    size_t walkSuspendedThread(void *threadHandle, uint64_t *frames, size_t maxFrames);

    // Frames starting from a captured register context (a CONTEXT*, e.g. from an
    // exception record). The context is copied, not modified. Does not allocate.
    size_t walkContext(const void *context, uint64_t *frames, size_t maxFrames);

    // The thread's description (SetThreadDescription), or empty.
    std::string threadName(void *threadHandle);
}
