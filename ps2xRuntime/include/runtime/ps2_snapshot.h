#pragma once

// Runner snapshots: save and restore the whole runtime (docs/SNAPSHOT-DESIGN.md).
//
//   PS2X_SNAPSHOT_SAVE=<path>        where saves go ("{tick}" -> VBlank tick;
//                                    default ps2x_snapshot_{tick}.bin)
//   PS2X_SNAPSHOT_AT=<tick>[,...]    save at the first safe point at or after each tick
//   PS2X_SNAPSHOT_EXIT=1             stop the runner after the last PS2X_SNAPSHOT_AT save
//   PS2X_SNAPSHOT_LOAD=<path>        resume from a snapshot at startup
//   PS2X_SNAPSHOT=1                  opt in without a timed save (F5 only)
//   F5 (game window)                 save now
//
// Snapshots are opt-in: with none of these set, nothing is recorded for them (the MPEG
// decoder replay log costs a heap copy of every chunk) and F5 does nothing.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

struct PS2SnapshotService
{
    // True when any of the variables above is set: state needed only for a later save
    // (the MPEG replay log) is recorded, F5 saves. Read once at first use.
    static bool enabled();

    // EE executor, dispatcher loop head, a save requested. True when the request is done
    // (saved or failed for good); false when the state is not serialisable yet (asked again).
    static bool atSafePoint(PS2Runtime &runtime, uint64_t requestTick);

    // Startup (PS2Runtime::run, before the game thread starts): arms PS2X_SNAPSHOT_AT and,
    // with PS2X_SNAPSHOT_LOAD set, loads that snapshot. Returns true if one was loaded.
    static bool startup(PS2Runtime &runtime);

    // Window thread: F5.
    static void requestFromHost(PS2Runtime &runtime);

    // Whether every piece of state can be serialised now; `why` names the first obstacle.
    static bool quiescent(PS2Runtime &runtime, std::string *why = nullptr);

    // The whole state, in memory or on disk. Save runs on the EE executor (or a test with
    // no executor running); load before the executor runs.
    static bool saveToBuffer(PS2Runtime &runtime, std::vector<uint8_t> &out, std::string *error = nullptr);
    static bool loadFromBuffer(PS2Runtime &runtime, const uint8_t *data, size_t size, std::string *error = nullptr);
    static bool saveToFile(PS2Runtime &runtime, const std::string &path, std::string *error = nullptr);
    static bool loadFromFile(PS2Runtime &runtime, const std::string &path, std::string *error = nullptr);
};
