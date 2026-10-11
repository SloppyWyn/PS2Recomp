#pragma once

// Test cheats: PS2X_CHEATS=<file> applies RAM patches at every guest VBlank, so test runs can reach
// a scene faster (a unit with enough movement, HP that survives). The file uses PCSX2's pnach
// syntax, so one file serves both sides of a paired comparison:
//   patch=1,EE,<code>,extended,<value>   applied at every VBlank
//   patch=0,EE,<code>,extended,<value>   applied once, at the first VBlank
// <code> is a raw PS2 code: the top nibble is the type (0 = 8-bit, 1 = 16-bit, 2 = 32-bit), the
// rest the address (e.g. 20558560 = 32-bit write at 0x00558560). Other lines ("//" comments,
// gametitle=, comment=, blank) are ignored; lines it cannot parse are reported once on stderr.
// Off when PS2X_CHEATS is unset: one cached flag test per VBlank.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x_cheats
{
    struct Patch
    {
        bool everyFrame = true;
        uint32_t address = 0;
        uint32_t width = 4; // bytes: 1, 2 or 4
        uint32_t value = 0;
    };

    // Parses pnach text; lines it skips as malformed are counted in *bad (if given).
    std::vector<Patch> parsePnach(const std::string &text, size_t *bad = nullptr);

    // Writes the patches into RDRAM (little endian, address masked to 32 MB). firstFrame: also
    // the patch=0 ones.
    void apply(const std::vector<Patch> &patches, uint8_t *rdram, size_t ramSize, bool firstFrame);

    bool enabled() noexcept;

    // EE executor, VBlank start.
    void atVBlank(uint8_t *rdram, size_t ramSize);
}
