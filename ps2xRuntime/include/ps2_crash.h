#pragma once
// Crash handler for the runner (on by default; PS2X_CRASH_HANDLER=0 turns it off).
//
// On an unhandled native exception (access violation, illegal instruction, divide by
// zero, stack overflow, ...) in any host thread it prints to stderr, as [crash] lines:
// the exception and faulting address, the faulting thread's host stack (exe frames named
// from the linker map, so generated functions show their guest address), the guest state
// (live pc/ra/sp of the running EE context with guest names, registers, EE thread), the
// last runtime-bound SDK calls and the last guest dispatches, both named. Then it flushes
// stdout/stderr and the call trace and ends the process with the exception code.
//
// PS2X_CRASH_DUMP=<path>     also write a minidump there.
// PS2X_CRASH_TEST=<stub>[:n] test only: the n-th call (default 1st) of a runtime-bound SDK
//                            stub whose name contains <stub> writes through a null pointer.
// PS2X_CRASH_TEST_HOLD=symbols|stderr  test only, with PS2X_CRASH_TEST: just before the
//                            crash a thread takes that lock and keeps it (the report then
//                            prints bare addresses / hits the 10 s timeout and still exits).
//
// The report never waits for the symbol lock and the map is loaded at install, off the
// crash path. If the report blocks anyway (heap, loader or stdio lock held by a frozen
// thread), a thread created at install ends the process 10 s after the crash.
//
// Unhandled C++ exceptions keep going to std::terminate (main.cpp's logger). A debugger,
// when attached, gets the exception first. Windows only; elsewhere install() does nothing.
#include <functional>
#include <string>

namespace ps2x_crash
{
    // guestReport: text about the guest state, one or more "[crash] ..." lines. Called
    // inside the handler: must not take locks the crashing thread might hold.
    void install(std::function<std::string()> guestReport);
    bool installed() noexcept;
    // Reserve stack for the report on the calling thread (call at the start of threads
    // that run guest code; install() does it for its own thread).
    void prepareThread() noexcept;
    // Test only (PS2X_CRASH_TEST_HOLD): "symbols" or "stderr".
    void testHoldLock(const char *which);
}
