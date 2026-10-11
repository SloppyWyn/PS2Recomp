#pragma once

// Per-VBlank state hashes for determinism checks (tools\checkpoint.ps1 verify).
//
//   PS2X_FRAME_HASH=<path>        at every guest VBlank, on the EE executor (a point fixed in
//                                 guest time, unlike the presenter's), append one line
//                                 "tick=<n> cycle=<ee cycle> ram=<hash> vram=<hash> gs=<hash>":
//                                 RDRAM, GS local memory (after the GS queue drained) and the
//                                 GS privileged display registers. Two runs from one snapshot
//                                 that should be identical are compared line by line.
//   PS2X_FRAME_HASH_UNTIL=<tick>  at that tick, create the PS2X_STOP_FILE (clean stop);
//                                 "+N": N VBlanks after the first hashed one (e.g. after a load).
//
// Off when PS2X_FRAME_HASH is unset: one cached flag test per VBlank.

#include <cstdint>

class PS2Runtime;

namespace ps2x_frame_hash
{
    bool enabled() noexcept;

    // EE executor, VBlank start, after the tick counter advanced.
    void atVBlank(PS2Runtime &runtime, uint64_t tick, uint64_t eeCycle);

    // 64-bit hash of a byte range (exposed for tests).
    uint64_t hashBytes(const uint8_t *data, size_t size) noexcept;
}
