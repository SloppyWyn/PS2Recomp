#pragma once
// Guest function names in the runtime: "name+offset" for any guest pc, for logs, the
// watchdog, the call trace (ps2_call_trace.h) and crash reports.
//
// Sources:
//  - the main program: register_functions.cpp (emitted by ps2_recomp) passes its function
//    list (start, end, name, kind) to ps2xRegisterGuestFunctionNames() from a static
//    initialiser. Output generated before this existed registers nothing: pcs then print
//    as bare addresses.
//  - code overlays (ps2_overlays.h): named after the overlay loaded at that address now,
//    "<overlay file>:<name>+off" when the overlay registered names
//    (ps2xRegisterOverlayFunctionNames), else "<overlay file>:sub_<start>+off", where start
//    is the entry point the overlay's own table maps that code to.
//
// Registration happens before main(); lookups take no lock (overlay lookups and the
// std::string helpers allocate).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum : uint32_t
{
    PS2X_GUEST_NAME_FRAGMENT = 1u, // a resume fragment ("entry_..."), not a real function
    PS2X_GUEST_NAME_STUB = 2u,     // bound to a runtime implementation (SDK stub / syscall)
    PS2X_GUEST_NAME_SKIPPED = 4u,  // skipped by the config (left as a stub)
};

struct PS2GuestFunctionName
{
    uint32_t start;
    uint32_t end; // exclusive; 0 or <= start = unknown size (covers just its start)
    const char *name;
    uint32_t flags;
};

// Main program (from register_functions.cpp). May be called more than once; the tables
// are merged. The arrays must outlive the program (static data).
bool ps2xRegisterGuestFunctionNames(const PS2GuestFunctionName *names, size_t count);

// Names for one overlay (by its MWo3 file name, e.g. "stg_001.bin"), optional.
bool ps2xRegisterOverlayFunctionNames(const char *overlayFile, const PS2GuestFunctionName *names, size_t count);

namespace ps2x_names
{
    // A sorted, searchable set of named ranges (exposed for the overlay code and tests).
    class NameTable
    {
    public:
        void add(const PS2GuestFunctionName *names, size_t count);
        // The function containing pc: a real function wins over a fragment inside it.
        const PS2GuestFunctionName *find(uint32_t pc) const;
        const PS2GuestFunctionName *findStart(uint32_t start) const; // exact start
        const std::vector<PS2GuestFunctionName> &entries() const { return m_entries; }
        bool empty() const { return m_entries.empty(); }

    private:
        std::vector<PS2GuestFunctionName> m_entries; // sorted by start
        uint32_t m_maxSpan = 4;                      // longest end - start seen
    };

    struct Resolved
    {
        const char *module = nullptr; // overlay file name, or null for the main program
        const char *name = nullptr;   // null: no name (sub_<start> when start != 0)
        uint32_t start = 0;           // start of the containing function (0 = unknown)
        uint32_t flags = 0;
    };

    // Guest RAM used for overlay lookups when the caller has none (set by the runtime).
    void setGuestMemory(const uint8_t *rdram);

    const NameTable &mainTable();
    bool resolve(uint32_t pc, Resolved &out, const uint8_t *rdram = nullptr);

    // "FUN_0018b260+0x3d0", "stg_001.bin:sub_00756dd0+0x14", or "0x18b630" when unnamed.
    // Writes at most size-1 chars plus NUL; returns the length written. Takes no lock;
    // allocates only for overlay addresses (the loaded overlay's name is read as a string).
    size_t format(char *buffer, size_t size, uint32_t pc, const uint8_t *rdram = nullptr);
    std::string describe(uint32_t pc, const uint8_t *rdram = nullptr);

    // All named ranges whose name contains `part` (main program and registered overlay
    // names), as [start, end) ranges. For the call-trace filter.
    void findByName(const std::string &part, std::vector<std::pair<uint32_t, uint32_t>> &ranges);
}
