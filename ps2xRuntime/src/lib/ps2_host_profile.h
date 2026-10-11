#pragma once
// Sampling profiler for the runner's host threads, for performance work without an external
// profiler. Off unless PS2X_HOST_PROFILE is set (no thread, no cost):
//   PS2X_HOST_PROFILE=<ms>           sample every host thread's native stack every <ms> ms
//                                    (1-1000, e.g. 2); report per thread the functions on top
//                                    (self %) and anywhere on the stack (incl %), named from the
//                                    runner's linker map, as [hostprof] lines on stderr at exit
//   PS2X_HOST_PROFILE_FILE=<path>    also write the report there
//   PS2X_HOST_PROFILE_EVERY=<s>      also report every <s> seconds (cumulative)
// Windows x64 only (ps2_host_stack.h); elsewhere it does nothing.

namespace ps2x_host_profile
{
    void startFromEnvironment();
    // Stop sampling and write the final report (also run at exit if never called).
    void stop();
}
