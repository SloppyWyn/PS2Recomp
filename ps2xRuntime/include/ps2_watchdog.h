#pragma once
// Stall watchdog (debug switch, off unless PS2X_WATCHDOG is set).
//
// PS2X_WATCHDOG=<seconds>  when the window loop or the guest VBlank count has not moved
//                          for that long, print to stderr: every host thread's native
//                          stack (exe frames named from ps2EntryRunner.map, system DLLs
//                          from their exports), the guest state (debug pc/ra/sp, EE
//                          threads, semaphores, event flags) and the last runtime-bound
//                          SDK calls. Repeats while the stall lasts (up to 4 reports),
//                          re-arms once things move again.
//
// Cost when off: one relaxed atomic load per stub call and per window frame.
//
// Debugging tool only. A report suspends every other host thread to walk its stack; if
// a suspended thread holds a loader lock (or the heap/dbghelp lock the report needs), the
// report itself can freeze the process. Native stacks are walked on Windows x64 only.
#include <cstdint>
#include <functional>
#include <string>

namespace ps2x_watchdog
{
    bool enabled() noexcept;

    // Called once from the window (main) thread. vsyncCount: the guest VBlank counter
    // (must be lock-free); guestReport: text describing the guest state (may lock briefly).
    void start(std::function<uint64_t()> vsyncCount, std::function<std::string()> guestReport);
    void stop();

    // One per window-loop iteration.
    void uiHeartbeat() noexcept;

    // Ring of the last runtime-bound SDK calls (fed by ps2xStubTrace while the watchdog or
    // the crash handler is on; enableStubRing() turns it on without the watchdog).
    void enableStubRing() noexcept;
    bool stubRingEnabled() noexcept;
    void dumpStubRing(const char *prefix) noexcept; // to stderr, callers named
    void noteStub(const char *name, uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2,
                  uint32_t raOrV0, bool leaving) noexcept;
}
