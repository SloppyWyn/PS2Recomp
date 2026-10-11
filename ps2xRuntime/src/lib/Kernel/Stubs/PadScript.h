#pragma once

// Scripted pad input for unattended runs (PS2X_PAD_SCRIPT).
//
//   PS2X_PAD_SCRIPT="<tick>:<BTN>[|<BTN>...][+<hold>],..."
//   e.g.  PS2X_PAD_SCRIPT="11000:START+10,11300:CROSS+6,11400:UP|CROSS"
//
// <tick> is the EE scheduler's VBlank tick (EeScheduler::currentVSyncTick). Each entry
// holds its buttons down for <hold> ticks (default kPadScriptDefaultHold), counted from the
// first pad read at or after <tick>: during [first, first + hold). A game that polls less
// often than every <hold> ticks still sees the press once (buttonsAt is the plain
// [tick, tick + hold) window). Button names are the DualShock bit names (case-insensitive):
// SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE.
// Scripted buttons are ORed into whatever the pad stub reads (backend, raylib keyboard or
// gamepad, debug override), so a person can still play along. Each entry logs once,
// "[pad:script] tick=<tick> buttons=<names> (0x<mask>) hold=<n>", on the first pad read
// that sees it. With the variable unset the cost is one cached bool test per scePadRead.

#include <cstdint>
#include <string>
#include <vector>

namespace ps2_stubs::pad_script
{
    constexpr uint32_t kPadScriptDefaultHold = 6u;

    struct Event
    {
        uint64_t tick = 0;
        uint16_t buttons = 0; // active-high DualShock mask (bit 3 = START, bit 14 = CROSS, ...)
        uint32_t hold = kPadScriptDefaultHold;
    };

    // Parses a script. On error returns false and sets *error (if given); events is cleared.
    bool parse(const std::string &spec, std::vector<Event> &events, std::string *error = nullptr);

    // Active-high mask of the buttons the script holds at this tick.
    uint16_t buttonsAt(const std::vector<Event> &events, uint64_t tick);

    // "START|CROSS" for a mask (bit order).
    std::string buttonNames(uint16_t mask);

    // Process-wide script, read once from PS2X_PAD_SCRIPT on first use.
    bool enabled();
    // Active-high mask for this tick; logs each event the first time it is seen.
    uint16_t activeButtons(uint64_t tick);

    // Tests: replace the script (empty string disables) and pin the tick the pad stub uses
    // (tick < 0 restores the scheduler's tick).
    void setScriptForTesting(const std::string &spec);
    void setTickForTesting(int64_t tick);
    // The pad stub's tick source: the test tick if pinned, else the given scheduler tick.
    uint64_t resolveTick(uint64_t schedulerTick);
}
