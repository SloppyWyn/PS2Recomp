#include "PadScript.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <iostream>
#include <mutex>

namespace ps2_stubs::pad_script
{
    namespace
    {
        struct ButtonName
        {
            const char *name;
            uint16_t mask;
        };

        // DualShock bit order (libpad / scePadRead data[2..3], active-low there).
        constexpr ButtonName kButtons[] = {
            {"SELECT", 1u << 0}, {"L3", 1u << 1}, {"R3", 1u << 2}, {"START", 1u << 3},
            {"UP", 1u << 4}, {"RIGHT", 1u << 5}, {"DOWN", 1u << 6}, {"LEFT", 1u << 7},
            {"L2", 1u << 8}, {"R2", 1u << 9}, {"L1", 1u << 10}, {"R1", 1u << 11},
            {"TRIANGLE", 1u << 12}, {"CIRCLE", 1u << 13}, {"CROSS", 1u << 14}, {"SQUARE", 1u << 15},
        };

        std::string trim(const std::string &s)
        {
            size_t b = 0;
            size_t e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
                ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
                --e;
            return s.substr(b, e - b);
        }

        bool parseUnsigned(const std::string &text, uint64_t &out)
        {
            const std::string t = trim(text);
            if (t.empty() || !std::all_of(t.begin(), t.end(), [](char c)
                                          { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
                return false;
            if (t.size() > 18)
                return false;
            out = std::strtoull(t.c_str(), nullptr, 10);
            return true;
        }

        bool parseButton(const std::string &text, uint16_t &mask)
        {
            std::string upper = trim(text);
            std::transform(upper.begin(), upper.end(), upper.begin(), [](char c)
                           { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            for (const ButtonName &b : kButtons)
            {
                if (upper == b.name)
                {
                    mask = b.mask;
                    return true;
                }
            }
            return false;
        }

        struct ScriptState
        {
            std::mutex mutex;
            bool loaded = false;
            bool enabled = false;
            std::vector<Event> events;
            std::vector<bool> logged;
            // Tick of the first pad read at or after each event's tick: the hold counts from
            // there, so a press is never lost when the game polls less often than its hold
            // (the title screen polls about every 17 VBlanks).
            std::vector<uint64_t> started;
            int64_t pinnedTick = -1;
        };

        ScriptState &state()
        {
            static ScriptState s;
            return s;
        }

        void installLocked(ScriptState &s, const std::string &spec, const char *source)
        {
            s.loaded = true;
            s.enabled = false;
            s.events.clear();
            s.logged.clear();
            s.started.clear();
            if (trim(spec).empty())
                return;

            std::string error;
            std::vector<Event> events;
            if (!parse(spec, events, &error))
            {
                std::cerr << "[pad:script] " << source << " ignored: " << error << std::endl;
                return;
            }
            s.events = std::move(events);
            s.logged.assign(s.events.size(), false);
            s.started.assign(s.events.size(), 0u);
            s.enabled = !s.events.empty();
            std::cerr << "[pad:script] " << source << ": " << s.events.size() << " event(s)" << std::endl;
        }

        void loadOnce(ScriptState &s)
        {
            if (s.loaded)
                return;
            const char *env = std::getenv("PS2X_PAD_SCRIPT");
            installLocked(s, env ? env : "", "PS2X_PAD_SCRIPT");
        }

        // Cached after the first call so the pad stub pays one bool test when the script is off.
        std::atomic<bool> g_fastEnabled{false};
        std::atomic<bool> g_fastKnown{false};
    }

    bool parse(const std::string &spec, std::vector<Event> &events, std::string *error)
    {
        events.clear();
        auto fail = [&](const std::string &message)
        {
            events.clear();
            if (error)
                *error = message;
            return false;
        };

        size_t pos = 0;
        while (pos <= spec.size())
        {
            size_t comma = spec.find(',', pos);
            if (comma == std::string::npos)
                comma = spec.size();
            const std::string entry = trim(spec.substr(pos, comma - pos));
            pos = comma + 1;
            if (entry.empty())
            {
                if (comma == spec.size())
                    break;
                continue;
            }

            const size_t colon = entry.find(':');
            if (colon == std::string::npos)
                return fail("entry '" + entry + "' has no ':' (want <tick>:<buttons>[+<hold>])");

            Event ev;
            if (!parseUnsigned(entry.substr(0, colon), ev.tick))
                return fail("entry '" + entry + "': bad tick");

            std::string buttons = entry.substr(colon + 1);
            const size_t plus = buttons.find('+');
            if (plus != std::string::npos)
            {
                uint64_t hold = 0;
                if (!parseUnsigned(buttons.substr(plus + 1), hold) || hold == 0 || hold > 0xFFFFFFFFull)
                    return fail("entry '" + entry + "': bad hold");
                ev.hold = static_cast<uint32_t>(hold);
                buttons = buttons.substr(0, plus);
            }

            size_t bpos = 0;
            while (true)
            {
                const size_t bar = buttons.find('|', bpos);
                const std::string name = buttons.substr(bpos, bar == std::string::npos ? std::string::npos : bar - bpos);
                uint16_t mask = 0;
                if (!parseButton(name, mask))
                    return fail("entry '" + entry + "': unknown button '" + trim(name) + "'");
                ev.buttons = static_cast<uint16_t>(ev.buttons | mask);
                if (bar == std::string::npos)
                    break;
                bpos = bar + 1;
            }
            events.push_back(ev);
            if (comma == spec.size())
                break;
        }
        return true;
    }

    uint16_t buttonsAt(const std::vector<Event> &events, uint64_t tick)
    {
        uint16_t mask = 0;
        for (const Event &ev : events)
        {
            if (tick >= ev.tick && tick - ev.tick < ev.hold)
                mask = static_cast<uint16_t>(mask | ev.buttons);
        }
        return mask;
    }

    std::string buttonNames(uint16_t mask)
    {
        std::string out;
        for (const ButtonName &b : kButtons)
        {
            if (mask & b.mask)
            {
                if (!out.empty())
                    out += '|';
                out += b.name;
            }
        }
        return out;
    }

    bool enabled()
    {
        if (g_fastKnown.load(std::memory_order_acquire))
            return g_fastEnabled.load(std::memory_order_relaxed);
        ScriptState &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        loadOnce(s);
        g_fastEnabled.store(s.enabled, std::memory_order_relaxed);
        g_fastKnown.store(true, std::memory_order_release);
        return s.enabled;
    }

    uint16_t activeButtons(uint64_t tick)
    {
        ScriptState &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        loadOnce(s);
        if (!s.enabled)
            return 0;

        uint16_t mask = 0;
        for (size_t i = 0; i < s.events.size(); ++i)
        {
            const Event &ev = s.events[i];
            if (tick < ev.tick)
                continue;
            if (!s.logged[i])
            {
                // First read at or after the scheduled tick: the hold starts now.
                s.started[i] = tick;
            }
            if (tick < s.started[i] || tick - s.started[i] >= ev.hold)
                continue;
            mask = static_cast<uint16_t>(mask | ev.buttons);
            if (!s.logged[i])
            {
                s.logged[i] = true;
                std::cerr << "[pad:script] tick=" << tick << " buttons=" << buttonNames(ev.buttons);
                char hex[16];
                std::snprintf(hex, sizeof(hex), " (0x%04x)", ev.buttons);
                std::cerr << hex << " hold=" << ev.hold;
                if (tick != ev.tick)
                    std::cerr << " scheduled=" << ev.tick;
                std::cerr << std::endl;
            }
        }
        return mask;
    }

    void setScriptForTesting(const std::string &spec)
    {
        ScriptState &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        installLocked(s, spec, "test script");
        g_fastEnabled.store(s.enabled, std::memory_order_relaxed);
        g_fastKnown.store(true, std::memory_order_release);
    }

    void setTickForTesting(int64_t tick)
    {
        ScriptState &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pinnedTick = tick;
    }

    uint64_t resolveTick(uint64_t schedulerTick)
    {
        ScriptState &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.pinnedTick >= 0 ? static_cast<uint64_t>(s.pinnedTick) : schedulerTick;
    }
}
