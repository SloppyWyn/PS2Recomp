#pragma once
// Filtered guest call trace (debug switch, off unless PS2X_TRACE_CALLS is set).
//
// PS2X_TRACE_CALLS=<item>,<item>,...   which call targets to log:
//     *  or 1                  every guest call
//     0x18b000-0x18c000        targets in [start, end)
//     0x18bd70                 the named function containing that address (else just it)
//     FUN_0018bd70, sceCd      functions whose guest name contains the text
// PS2X_TRACE_CALLS_FILE=<path> output file (default ps2x_calls.txt in the working
//                              folder; "-" = stderr)
// PS2X_TRACE_CALLS_MAX=<n>     stop after n lines (default 1000000)
// PS2X_TRACE_CALLS_RET=1       also log "<" with v0 when a traced call returns in line
//
// A line: "> <vsync> t<guest thread> <kind> <target name> a0..a3 ra=<pc> (<name>) sp=".
// kind: call (jal/jalr through the dispatcher), jump (j/jr to another function), sched
// (the scheduler entered a function start: a call deferred by a checkpoint, a thread
// start, an interrupt handler or callback). Direct tail calls the recompiler emits as a
// plain C++ call (FUN_x(...); return;) bypass the dispatcher and are not seen.
//
// Hooked in PS2Runtime::dispatchGuestBranch and the EE scheduler's dispatch, not in the
// generated code. Cost when off: one load and a not-taken branch per dispatch. Lines go
// to a buffered file flushed every 200 ms by a background thread and on exit/crash.
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct R5900Context;

namespace ps2x_calltrace
{
    enum class Kind : uint8_t
    {
        Call,
        Jump,
        Sched,
    };

    // True once init() found PS2X_TRACE_CALLS (written before the guest starts).
    extern std::atomic<bool> g_active;
    inline bool active() noexcept { return g_active.load(std::memory_order_acquire); }

    // Reads the environment and builds the filter; call after the static initialisers
    // registered the names (from PS2Runtime::run). Idempotent.
    void init();

    // Filter test (bitmap over the dense function table, ranges elsewhere).
    bool matches(uint32_t target) noexcept;

    // Log one call when target passes the filter; returns true when logged.
    bool enter(const R5900Context *ctx, uint32_t target, uint32_t source, Kind kind, int guestThread, uint64_t vsync);
    // The scheduler is about to run guest code at target: logged as "sched" when target
    // is the start of a named (non-fragment) function and was not just logged as a jump.
    void schedulerEnter(const R5900Context *ctx, uint32_t target, int guestThread, uint64_t vsync);
    void leave(const R5900Context *ctx, uint32_t target);
    bool logReturns() noexcept;

    // Flush buffered lines (crash, exit). Safe to call when off.
    void flush() noexcept;
    // End of run: stop the trace and the flusher thread, flush and close the file.
    void shutdown() noexcept;

    // Filter parsing, exposed for tests: the ranges [start, end) a spec selects (names are
    // resolved through ps2_guest_names.h), or all = true. Returns false on a bad item.
    bool parseSpec(const std::string &spec, std::vector<std::pair<uint32_t, uint32_t>> &ranges, bool &all,
                   std::string *error = nullptr);
}
