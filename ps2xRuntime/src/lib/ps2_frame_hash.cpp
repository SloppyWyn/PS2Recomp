// Per-VBlank state hashes (ps2_frame_hash.h). Nothing here knows which game runs.

#include "runtime/ps2_frame_hash.h"

#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace
{
    struct FrameHashConfig
    {
        std::FILE *out = nullptr;
        uint64_t until = 0u;
        bool untilRelative = false; // "+N": N VBlanks after the first hashed one
        bool stopWritten = false;
    };

    FrameHashConfig &config()
    {
        static FrameHashConfig cfg = []
        {
            FrameHashConfig c;
            const char *path = std::getenv("PS2X_FRAME_HASH");
            if (!path || !*path)
                return c;
            c.out = std::fopen(path, "w");
            if (!c.out)
            {
                std::cerr << "[framehash] cannot write " << path << std::endl;
                return c;
            }
            if (const char *until = std::getenv("PS2X_FRAME_HASH_UNTIL"); until && *until)
            {
                c.untilRelative = until[0] == '+';
                c.until = std::strtoull(until + (c.untilRelative ? 1 : 0), nullptr, 0);
            }
            std::cerr << "[framehash] writing " << path
                      << (c.until ? std::string(c.untilRelative ? " for " : " until tick ") + std::to_string(c.until) +
                                        (c.untilRelative ? " VBlanks" : "")
                                  : std::string())
                      << std::endl;
            return c;
        }();
        return cfg;
    }

    void requestCleanStop(uint64_t tick)
    {
        const char *stopFile = std::getenv("PS2X_STOP_FILE");
        if (!stopFile || !*stopFile)
        {
            std::cerr << "[framehash] tick " << tick << " reached, but PS2X_STOP_FILE is not set: running on" << std::endl;
            return;
        }
        std::ofstream(stopFile) << "framehash until " << tick << "\n";
        std::cerr << "[framehash] tick " << tick << " reached: stop requested" << std::endl;
    }
}

namespace ps2x_frame_hash
{
    bool enabled() noexcept
    {
        static const bool on = config().out != nullptr;
        return on;
    }

    // 4 independent 64-bit lanes of multiply-xorshift over 8-byte words, folded at the end:
    // ~10 GB/s, so RDRAM and GS memory take a few ms per VBlank. Not cryptographic.
    uint64_t hashBytes(const uint8_t *data, size_t size) noexcept
    {
        constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
        uint64_t lanes[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull};
        size_t i = 0u;
        for (; i + 32u <= size; i += 32u)
        {
            for (int lane = 0; lane < 4; ++lane)
            {
                uint64_t word;
                std::memcpy(&word, data + i + lane * 8u, sizeof(word));
                lanes[lane] = (lanes[lane] ^ word) * kMul;
                lanes[lane] ^= lanes[lane] >> 29u;
            }
        }
        uint64_t hash = static_cast<uint64_t>(size) * kMul;
        for (; i < size; ++i)
            hash = (hash ^ data[i]) * kMul;
        for (const uint64_t lane : lanes)
        {
            hash = (hash ^ lane) * kMul;
            hash ^= hash >> 31u;
        }
        return hash;
    }

    void atVBlank(PS2Runtime &runtime, uint64_t tick, uint64_t eeCycle)
    {
        FrameHashConfig &cfg = config();
        if (!cfg.out)
            return;
        if (cfg.untilRelative)
        {
            cfg.until += tick;
            cfg.untilRelative = false;
        }
        runtime.gs().sync(); // asynchronous GS: GS memory after every command queued so far
        PS2Memory &memory = runtime.memory();
        const uint64_t ram = hashBytes(memory.getRDRAM(), PS2_RAM_SIZE);
        const uint64_t vram = hashBytes(memory.getGSVRAM(), PS2_GS_VRAM_SIZE);
        const auto &gs = memory.gs();
        const uint64_t regs[] = {gs.pmode, gs.smode2, gs.dispfb1, gs.display1, gs.dispfb2, gs.display2, gs.bgcolor};
        const uint64_t display = hashBytes(reinterpret_cast<const uint8_t *>(regs), sizeof(regs));
        std::fprintf(cfg.out, "tick=%llu cycle=%llu ram=%016llx vram=%016llx gs=%016llx\n",
                     static_cast<unsigned long long>(tick), static_cast<unsigned long long>(eeCycle),
                     static_cast<unsigned long long>(ram), static_cast<unsigned long long>(vram),
                     static_cast<unsigned long long>(display));
        std::fflush(cfg.out);
        if (cfg.until != 0u && tick >= cfg.until && !cfg.stopWritten)
        {
            cfg.stopWritten = true;
            requestCleanStop(tick);
        }
    }
}
