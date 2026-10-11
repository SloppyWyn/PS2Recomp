#include "runtime/ps2_cheats.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace ps2x_cheats
{
    namespace
    {
        std::string trim(const std::string &s)
        {
            const size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos)
                return {};
            const size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }

        bool parseHex(const std::string &s, uint32_t &out)
        {
            const std::string t = trim(s);
            if (t.empty() || t.size() > 8)
                return false;
            char *end = nullptr;
            const unsigned long v = std::strtoul(t.c_str(), &end, 16);
            if (!end || *end != '\0')
                return false;
            out = static_cast<uint32_t>(v);
            return true;
        }

        struct State
        {
            bool on = false;
            bool first = true;
            std::vector<Patch> patches;
        };

        State &state()
        {
            static State s = []
            {
                State st;
                const char *path = std::getenv("PS2X_CHEATS");
                if (!path || !*path)
                    return st;
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    std::cerr << "[cheats] cannot open " << path << "; cheats off" << std::endl;
                    return st;
                }
                std::stringstream buffer;
                buffer << in.rdbuf();
                size_t bad = 0;
                st.patches = parsePnach(buffer.str(), &bad);
                st.on = !st.patches.empty();
                std::cerr << "[cheats] " << path << ": " << st.patches.size() << " patch(es)"
                          << (bad ? ", " + std::to_string(bad) + " line(s) not understood" : std::string())
                          << std::endl;
                for (const Patch &p : st.patches)
                    std::cerr << "[cheats]   " << (p.everyFrame ? "every VBlank" : "once") << " "
                              << p.width * 8 << "-bit 0x" << std::hex << p.address << " = 0x" << p.value
                              << std::dec << std::endl;
                return st;
            }();
            return s;
        }
    }

    std::vector<Patch> parsePnach(const std::string &text, size_t *bad)
    {
        std::vector<Patch> out;
        size_t badCount = 0;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
        {
            line = trim(line);
            if (line.empty() || line.rfind("//", 0) == 0 || line.rfind("patch=", 0) != 0)
                continue;
            std::vector<std::string> f;
            std::string field;
            std::istringstream parts(line.substr(6));
            while (std::getline(parts, field, ','))
                f.push_back(trim(field));
            uint32_t code = 0, value = 0;
            if (f.size() < 5 || (f[0] != "0" && f[0] != "1") || f[1] != "EE" || f[3] != "extended" ||
                !parseHex(f[2], code) || !parseHex(f[4], value))
            {
                ++badCount;
                continue;
            }
            const uint32_t type = code >> 28;
            if (type > 2u)
            {
                ++badCount; // only raw 8/16/32-bit writes
                continue;
            }
            Patch p;
            p.everyFrame = f[0] == "1";
            p.address = code & 0x01FFFFFFu;
            p.width = type == 0u ? 1u : (type == 1u ? 2u : 4u);
            p.value = type == 0u ? (value & 0xFFu) : (type == 1u ? (value & 0xFFFFu) : value);
            out.push_back(p);
        }
        if (bad)
            *bad = badCount;
        return out;
    }

    void apply(const std::vector<Patch> &patches, uint8_t *rdram, size_t ramSize, bool firstFrame)
    {
        if (!rdram)
            return;
        for (const Patch &p : patches)
        {
            if (!p.everyFrame && !firstFrame)
                continue;
            if (p.address + p.width > ramSize)
                continue;
            std::memcpy(rdram + p.address, &p.value, p.width); // little endian host, as the EE
        }
    }

    bool enabled() noexcept
    {
        static const bool on = state().on;
        return on;
    }

    void atVBlank(uint8_t *rdram, size_t ramSize)
    {
        State &st = state();
        if (!st.on)
            return;
        apply(st.patches, rdram, ramSize, st.first);
        st.first = false;
    }
}
