// Guest function names: see include/ps2_guest_names.h.
#include "ps2_guest_names.h"
#include "ps2_overlays.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace ps2x_names
{
    namespace
    {
        // Containment search: the candidate with the greatest start <= pc, then a few
        // earlier ones (functions rarely overlap or nest; fragments sit inside functions).
        constexpr int kBackwardProbe = 16;

        bool covers(const PS2GuestFunctionName &e, uint32_t pc)
        {
            return e.end > e.start ? (pc >= e.start && pc < e.end) : pc == e.start;
        }

        const PS2GuestFunctionName *findIn(const std::vector<PS2GuestFunctionName> &v, uint32_t pc, bool fragments)
        {
            auto it = std::upper_bound(v.begin(), v.end(), pc, [](uint32_t value, const PS2GuestFunctionName &e)
                                       { return value < e.start; });
            int probes = 0;
            while (it != v.begin() && probes < kBackwardProbe)
            {
                --it;
                const bool isFragment = (it->flags & PS2X_GUEST_NAME_FRAGMENT) != 0u;
                if (isFragment == fragments)
                {
                    ++probes;
                    if (covers(*it, pc))
                    {
                        return &*it;
                    }
                }
            }
            return nullptr;
        }

        NameTable &mainTableMutable()
        {
            static NameTable table;
            return table;
        }

        std::atomic<const uint8_t *> g_guestMemory{nullptr};
    }

    void NameTable::add(const PS2GuestFunctionName *names, size_t count)
    {
        if (!names || count == 0)
        {
            return;
        }
        m_entries.reserve(m_entries.size() + count);
        for (size_t i = 0; i < count; ++i)
        {
            if (!names[i].name || !*names[i].name)
            {
                continue;
            }
            m_entries.push_back(names[i]);
        }
        std::stable_sort(m_entries.begin(), m_entries.end(), [](const PS2GuestFunctionName &a, const PS2GuestFunctionName &b)
                         { return a.start < b.start; });
    }

    const PS2GuestFunctionName *NameTable::find(uint32_t pc) const
    {
        if (const PS2GuestFunctionName *real = findIn(m_entries, pc, false))
        {
            return real;
        }
        return findIn(m_entries, pc, true);
    }

    const PS2GuestFunctionName *NameTable::findStart(uint32_t start) const
    {
        auto it = std::lower_bound(m_entries.begin(), m_entries.end(), start, [](const PS2GuestFunctionName &e, uint32_t value)
                                   { return e.start < value; });
        const PS2GuestFunctionName *best = nullptr;
        for (; it != m_entries.end() && it->start == start; ++it)
        {
            if (!best || (best->flags & PS2X_GUEST_NAME_FRAGMENT))
            {
                best = &*it;
            }
        }
        return best;
    }

    void setGuestMemory(const uint8_t *rdram)
    {
        g_guestMemory.store(rdram, std::memory_order_release);
    }

    const NameTable &mainTable()
    {
        return mainTableMutable();
    }

    bool resolve(uint32_t pc, Resolved &out, const uint8_t *rdram)
    {
        out = Resolved{};
        if (!rdram)
        {
            rdram = g_guestMemory.load(std::memory_order_acquire);
        }
        // Overlay code first: overlays load over addresses the main table may not cover,
        // and an overlay's code is what runs there now.
        PS2OverlayAddressInfo info{};
        if (rdram && ps2xOverlayDescribe(rdram, pc, info))
        {
            out.module = info.overlayFile;
            if (info.names)
            {
                if (const PS2GuestFunctionName *e = info.names->find(pc))
                {
                    out.name = e->name;
                    out.start = e->start;
                    out.flags = e->flags;
                    return true;
                }
            }
            out.start = info.entryStart;
            return out.start != 0u;
        }
        if (const PS2GuestFunctionName *e = mainTable().find(pc))
        {
            out.name = e->name;
            out.start = e->start;
            out.flags = e->flags;
            return true;
        }
        return false;
    }

    size_t format(char *buffer, size_t size, uint32_t pc, const uint8_t *rdram)
    {
        if (!buffer || size == 0)
        {
            return 0;
        }
        Resolved r;
        int n = 0;
        if (!resolve(pc, r, rdram))
        {
            n = std::snprintf(buffer, size, "0x%x", pc);
        }
        else
        {
            char offset[24] = "";
            if (pc != r.start)
            {
                std::snprintf(offset, sizeof(offset), "+0x%x", pc - r.start);
            }
            if (r.name)
            {
                n = std::snprintf(buffer, size, "%s%s%s%s", r.module ? r.module : "", r.module ? ":" : "", r.name, offset);
            }
            else
            {
                n = std::snprintf(buffer, size, "%s%ssub_%08x%s", r.module ? r.module : "", r.module ? ":" : "", r.start, offset);
            }
        }
        if (n < 0)
        {
            buffer[0] = '\0';
            return 0;
        }
        return std::min(static_cast<size_t>(n), size - 1);
    }

    std::string describe(uint32_t pc, const uint8_t *rdram)
    {
        char buffer[256];
        const size_t n = format(buffer, sizeof(buffer), pc, rdram);
        return std::string(buffer, n);
    }

    void findByName(const std::string &part, std::vector<std::pair<uint32_t, uint32_t>> &ranges)
    {
        auto scan = [&](const NameTable &table)
        {
            for (const PS2GuestFunctionName &e : table.entries())
            {
                if (std::strstr(e.name, part.c_str()) != nullptr)
                {
                    ranges.emplace_back(e.start, e.end > e.start ? e.end : e.start + 4u);
                }
            }
        };
        scan(mainTable());
        ps2xOverlayForEachNameTable([&](const char *, const NameTable &table)
                                    { scan(table); });
    }
}

bool ps2xRegisterGuestFunctionNames(const PS2GuestFunctionName *names, size_t count)
{
    ps2x_names::mainTableMutable().add(names, count);
    return true;
}
