#pragma once

// Code overlays: guest code loaded at run time over the same addresses (Metrowerks MWo3
// overlays: a 0x40-byte header at the load address, "MWo3" at +0 and the overlay's file
// name at +0x20). Each recompiled overlay registers its entry points once at start-up;
// a lookup in an overlay's range reads the header in guest memory to see which overlay
// is loaded there now and returns that overlay's function.

#include <cstddef>
#include <cstdint>
#include <functional>

#include "ps2_runtime.h"

struct PS2OverlayEntry
{
    uint32_t address;
    PS2Runtime::RecompiledFunction function;
};

// Called from the generated overlay files' static initialisers (before main()).
bool ps2xRegisterOverlay(const char *name, uint32_t base, uint32_t codeEnd,
                         const PS2OverlayEntry *entries, size_t count);

// The function at `address` in the overlay currently loaded over it, or null.
PS2Runtime::RecompiledFunction ps2xOverlayLookup(const uint8_t *rdram, uint32_t address);

// For naming (ps2_guest_names.h): which overlay is loaded over `address` now and which of
// its entry points the code at `address` belongs to (the lowest entry address that maps to
// the same recompiled function; for a resume fragment, the fragment's own start).
namespace ps2x_names
{
    class NameTable;
}
struct PS2OverlayAddressInfo
{
    const char *overlayFile = nullptr;           // MWo3 name, e.g. "stg_001.bin"
    uint32_t entryStart = 0;                     // 0 = address is not inside a known entry
    const ps2x_names::NameTable *names = nullptr; // registered names for this overlay, if any
};
bool ps2xOverlayDescribe(const uint8_t *rdram, uint32_t address, PS2OverlayAddressInfo &info);

void ps2xOverlayForEachNameTable(const std::function<void(const char *, const ps2x_names::NameTable &)> &visit);
