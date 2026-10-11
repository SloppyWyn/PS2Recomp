#include "ps2_overlays.h"
#include "ps2_guest_names.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32_t kHeaderMagicOffset = 0x00;
    constexpr uint32_t kHeaderNameOffset = 0x20;
    constexpr uint32_t kHeaderNameSize = 0x20;

    struct Overlay
    {
        std::string name;
        std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> entries;
        std::vector<uint32_t> unitStarts; // sorted: lowest entry address of each function
    };

    // All overlays that load at one base address.
    struct Region
    {
        uint32_t base = 0;
        uint32_t end = 0; // the end of the largest overlay's code
        // Keys view Overlay::name (each Overlay lives on the heap for the whole run), so a
        // lookup hashes the name in guest memory without building a std::string.
        std::unordered_map<std::string_view, const Overlay *> byName;
    };

    // Filled by static initialisers before main() and read-only afterwards, so lookups
    // take no lock.
    struct Registry
    {
        std::vector<std::unique_ptr<Overlay>> overlays;
        std::vector<Region> regions;
        std::unordered_map<std::string, std::unique_ptr<ps2x_names::NameTable>> names;
    };

    Registry &registry()
    {
        static Registry r;
        return r;
    }

    // The overlay found by the last lookup on this thread, keyed by its region and the raw
    // name field of the header in guest memory: calls into overlay code mostly stay in the
    // overlay that is loaded, so a 32-byte compare replaces the name hash.
    struct LastOverlay
    {
        const Region *region = nullptr;
        const Overlay *overlay = nullptr;
        char rawName[kHeaderNameSize] = {};
    };
    thread_local LastOverlay t_lastOverlay;
}

bool ps2xRegisterOverlay(const char *name, uint32_t base, uint32_t codeEnd,
                         const PS2OverlayEntry *entries, size_t count)
{
    Registry &r = registry();
    auto overlay = std::make_unique<Overlay>();
    overlay->name = name ? name : "";
    overlay->entries.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        overlay->entries.emplace(entries[i].address, entries[i].function);
    }
    std::unordered_map<PS2Runtime::RecompiledFunction, uint32_t> firstAddress;
    for (const auto &[address, function] : overlay->entries)
    {
        auto [it, inserted] = firstAddress.emplace(function, address);
        if (!inserted)
        {
            it->second = std::min(it->second, address);
        }
    }
    for (const auto &[function, address] : firstAddress)
    {
        overlay->unitStarts.push_back(address);
    }
    std::sort(overlay->unitStarts.begin(), overlay->unitStarts.end());

    auto region = std::find_if(r.regions.begin(), r.regions.end(),
                               [base](const Region &existing) { return existing.base == base; });
    if (region == r.regions.end())
    {
        r.regions.push_back(Region{base, codeEnd, {}});
        region = std::prev(r.regions.end());
    }
    region->end = std::max(region->end, codeEnd);
    region->byName.erase(std::string_view(overlay->name)); // a later registration of a name wins
    region->byName.emplace(std::string_view(overlay->name), overlay.get());
    r.overlays.push_back(std::move(overlay));
    t_lastOverlay = LastOverlay{}; // regions may have moved
    return true;
}

namespace
{
    const Overlay *loadedOverlay(const uint8_t *rdram, uint32_t address)
    {
        if (!rdram)
        {
            return nullptr;
        }
        for (const Region &region : registry().regions)
        {
            if (address < region.base || address >= region.end)
            {
                continue;
            }
            const char *header = reinterpret_cast<const char *>(rdram + region.base);
            if (std::memcmp(header + kHeaderMagicOffset, "MWo3", 4) != 0)
            {
                return nullptr;
            }
            const char *nameStart = header + kHeaderNameOffset;
            const std::string name(nameStart, strnlen(nameStart, kHeaderNameSize));
            const auto overlay = region.byName.find(name);
            return overlay == region.byName.end() ? nullptr : overlay->second;
        }
        return nullptr;
    }
}

bool ps2xRegisterOverlayFunctionNames(const char *overlayFile, const PS2GuestFunctionName *names, size_t count)
{
    auto &table = registry().names[overlayFile ? overlayFile : ""];
    if (!table)
    {
        table = std::make_unique<ps2x_names::NameTable>();
    }
    table->add(names, count);
    return true;
}

bool ps2xOverlayDescribe(const uint8_t *rdram, uint32_t address, PS2OverlayAddressInfo &info)
{
    info = PS2OverlayAddressInfo{};
    const Overlay *overlay = loadedOverlay(rdram, address);
    if (!overlay)
    {
        return false;
    }
    info.overlayFile = overlay->name.c_str();
    const auto names = registry().names.find(overlay->name);
    if (names != registry().names.end())
    {
        info.names = names->second.get();
    }
    auto it = std::upper_bound(overlay->unitStarts.begin(), overlay->unitStarts.end(), address);
    if (it != overlay->unitStarts.begin())
    {
        info.entryStart = *std::prev(it);
    }
    return true;
}

void ps2xOverlayForEachNameTable(const std::function<void(const char *, const ps2x_names::NameTable &)> &visit)
{
    for (const auto &[file, table] : registry().names)
    {
        if (table)
        {
            visit(file.c_str(), *table);
        }
    }
}

PS2Runtime::RecompiledFunction ps2xOverlayLookup(const uint8_t *rdram, uint32_t address)
{
    if (!rdram)
    {
        return nullptr;
    }
    for (const Region &region : registry().regions)
    {
        if (address < region.base || address >= region.end)
        {
            continue;
        }
        const char *header = reinterpret_cast<const char *>(rdram + region.base);
        if (std::memcmp(header + kHeaderMagicOffset, "MWo3", 4) != 0)
        {
            return nullptr; // nothing loaded there (yet)
        }
        const char *nameStart = header + kHeaderNameOffset;
        LastOverlay &last = t_lastOverlay;
        const Overlay *overlay = nullptr;
        if (last.region == &region && std::memcmp(last.rawName, nameStart, kHeaderNameSize) == 0)
        {
            overlay = last.overlay;
        }
        else
        {
            const auto found = region.byName.find(std::string_view(nameStart, strnlen(nameStart, kHeaderNameSize)));
            if (found == region.byName.end())
            {
                return nullptr;
            }
            overlay = found->second;
            last.region = &region;
            last.overlay = overlay;
            std::memcpy(last.rawName, nameStart, kHeaderNameSize);
        }
        const auto entry = overlay->entries.find(address);
        return entry != overlay->entries.end() ? entry->second : nullptr;
    }
    return nullptr;
}
