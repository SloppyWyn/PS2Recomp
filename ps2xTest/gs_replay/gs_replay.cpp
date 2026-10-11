// ps2x_gs_replay: replay a PCSX2 GS dump (.gs, uncompressed: PCSX2 setting GSDumpCompression = 0)
// through the runtime's GS front end and CPU rasteriser, and write the resulting GS memory.
//
//   ps2x_gs_replay DUMP.gs OUT.bin [--frames N] [--repeat R] [--profile]
//
// --repeat R replays the same frames R times from the dump's initial state and reports the best
// time per frame (the rasteriser benchmark; the file load is not counted). --profile (Windows)
// samples the replaying thread's instruction pointer every 0.5 ms and prints the hottest
// source lines and functions at the end (needs the .pdb next to the .exe).
//
// The dump supplies the starting GS memory and drawing registers plus every GIF transfer of the
// frames it covers; N frames (default 1) are replayed (a frame ends at a vsync packet). OUT.bin is
// the 4 MiB GS memory afterwards, in native layout, for tools/gsview.py; compare it with the
// reference GS (tools/gsref.py --compare). Format notes: tools/pcsx2gs.py.
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;

    uint32_t u32At(const std::vector<uint8_t> &d, size_t off)
    {
        uint32_t v = 0;
        std::memcpy(&v, d.data() + off, 4);
        return v;
    }

    uint64_t u64At(const std::vector<uint8_t> &d, size_t off)
    {
        uint64_t v = 0;
        std::memcpy(&v, d.data() + off, 8);
        return v;
    }

    int fail(const std::string &message)
    {
        std::cerr << "ps2x_gs_replay: " << message << std::endl;
        return 1;
    }

#if defined(_WIN32)
    // --profile: instruction-pointer samples of the main thread, resolved at the end.
    class Profiler
    {
    public:
        void start()
        {
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
            m_thread = std::thread([this]
                                   { sample(); });
        }

        void stopAndReport()
        {
            m_stop.store(true);
            if (m_thread.joinable())
                m_thread.join();
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            if (!SymInitialize(process, nullptr, TRUE))
            {
                std::cerr << "profile: SymInitialize failed" << std::endl;
                return;
            }
            uint64_t total = 0;
            std::map<std::string, uint64_t> byLine, byFunction;
            for (const auto &[address, count] : m_samples)
            {
                total += count;
                char buffer[sizeof(SYMBOL_INFO) + 512] = {};
                SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 511;
                DWORD64 displacement = 0;
                std::string function = "?";
                if (SymFromAddr(process, address, &displacement, symbol))
                    function = symbol->Name;
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                std::string where = function;
                if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
                {
                    std::string file = line.FileName;
                    const size_t slash = file.find_last_of("\\/");
                    where = file.substr(slash == std::string::npos ? 0 : slash + 1) + ":" + std::to_string(line.LineNumber);
                }
                byLine[where] += count;
                byFunction[function] += count;
            }
            auto print = [&](const char *title, const std::map<std::string, uint64_t> &table, size_t limit)
            {
                std::vector<std::pair<uint64_t, std::string>> rows;
                for (const auto &[key, count] : table)
                    rows.emplace_back(count, key);
                std::sort(rows.rbegin(), rows.rend());
                std::cout << "[profile] " << title << " (" << total << " samples):" << std::endl;
                for (size_t i = 0; i < rows.size() && i < limit; ++i)
                    std::cout << "  " << std::fixed << std::setprecision(1) << (100.0 * rows[i].first / std::max<uint64_t>(total, 1)) << "%  "
                              << rows[i].second << std::endl;
            };
            print("functions", byFunction, 25);
            print("lines", byLine, 40);
            SymCleanup(process);
        }

    private:
        void sample()
        {
            while (!m_stop.load())
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                if (SuspendThread(m_target) == static_cast<DWORD>(-1))
                    continue;
                CONTEXT context = {};
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(m_target, &context))
                    ++m_samples[context.Rip];
                ResumeThread(m_target);
            }
        }

        HANDLE m_target = nullptr;
        std::thread m_thread;
        std::atomic<bool> m_stop{false};
        std::map<uint64_t, uint64_t> m_samples;
    };
#endif
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: ps2x_gs_replay DUMP.gs OUT.bin [--frames N]" << std::endl;
        return 2;
    }
    int frames = 1;
    int repeat = 1;
    for (int i = 3; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == "--frames")
            frames = std::stoi(argv[i + 1]);
        if (std::string(argv[i]) == "--repeat")
            repeat = std::max(1, std::stoi(argv[i + 1]));
    }
    bool profile = false;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--profile")
            profile = true;
#if defined(_WIN32)
    Profiler profiler;
    if (profile)
        profiler.start();
#endif

    std::ifstream in(argv[1], std::ios::binary);
    if (!in)
        return fail("cannot open dump");
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 64 || u32At(d, 0) != 0xFFFFFFFFu)
        return fail("not a PCSX2 GS dump with a header (or compressed: set GSDumpCompression = 0)");
    const uint32_t headerSize = u32At(d, 4);
    const uint32_t stateVersion = u32At(d, 8);
    const uint32_t stateSize = u32At(d, 12);
    if (stateVersion != 9u)
        std::cerr << "warning: state version " << stateVersion << ", written for 9" << std::endl;
    size_t p = 8u + headerSize;
    if (p + stateSize + 8192u > d.size() || stateSize < kVramSize + 84u + 372u)
        return fail("truncated dump");
    const size_t state = p;
    p += stateSize + 8192u; // state, privileged registers

    std::vector<uint8_t> vram(kVramSize, 0u);
    int frame = 0;
    uint64_t transfers = 0;
    double bestMs = 0.0;
    std::vector<double> frameMs;
    for (int run = 0; run < repeat; ++run)
    {
    frame = 0;
    transfers = 0;
    std::vector<double> thisRunMs;
    p = state + stateSize + 8192u;
    GS gs;
    gs.init(vram.data(), kVramSize, nullptr);
    // VRAM sits before the GIF path state (4 x 20 bytes) and Q (4 bytes) at the end of the state.
    std::memcpy(vram.data(), d.data() + state + stateSize - 84u - kVramSize, kVramSize);

    // Drawing registers: u32 version, 15 env registers, then 2 x 12 context registers.
    static const uint8_t envAddr[] = {0x00, 0x1A, 0x1C, 0x22, 0x3B, 0x3D, 0x44, 0x45, 0x46, 0x49};
    static const uint8_t ctxAddr[] = {0x18, 0x06, 0x14, 0x08, 0x34, 0x36, 0x40, 0x42, 0x47, 0x4A, 0x4C, 0x4E};
    for (size_t i = 1; i < sizeof(envAddr); ++i)
        gs.writeRegister(envAddr[i], u64At(d, state + 4u + i * 8u));
    for (uint32_t c = 0; c < 2u; ++c)
        for (size_t i = 0; i < sizeof(ctxAddr); ++i)
        {
            uint64_t v = u64At(d, state + 124u + c * 96u + i * 8u);
            if (ctxAddr[i] == 0x06)
                v &= ~(7ull << 61); // CLD = 0: restoring TEX0 must not reload the CLUT
            gs.writeRegister(static_cast<uint8_t>(ctxAddr[i] + c), v);
        }
    gs.writeRegister(0x00, u64At(d, state + 4u)); // PRIM

    auto frameStart = std::chrono::steady_clock::now();
    while (p < d.size() && frame < frames)
    {
        const uint8_t type = d[p++];
        if (type == 0u)
        {
            const uint32_t size = u32At(d, p + 1u);
            if (p + 5u + size > d.size())
                return fail("truncated transfer");
            gs.processGIFPacket(d.data() + p + 5u, size);
            p += 5u + size;
            ++transfers;
        }
        else if (type == 1u)
        {
            p += 1u;
            ++frame;
            const auto now = std::chrono::steady_clock::now();
            thisRunMs.push_back(std::chrono::duration<double, std::milli>(now - frameStart).count());
            frameStart = now;
        }
        else if (type == 2u)
            p += 4u;
        else if (type == 3u)
            p += 8192u;
        else
            return fail("bad packet type");
    }

    double total = 0.0;
    for (double ms : thisRunMs)
        total += ms;
    if (run == 0 || total < bestMs)
    {
        bestMs = total;
        frameMs = thisRunMs;
    }
    }

    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char *>(vram.data()), kVramSize);
    if (!out)
        return fail("cannot write output");
    std::cout << "replayed " << frame << " frame(s), " << transfers << " transfers -> " << argv[2] << std::endl;
#if defined(_WIN32)
    if (profile)
        profiler.stopAndReport();
#endif
    std::cout << "best of " << repeat << ": " << bestMs << " ms total";
    for (size_t i = 0; i < frameMs.size(); ++i)
        std::cout << (i == 0 ? " (" : ", ") << "frame " << (i + 1) << " " << frameMs[i] << " ms";
    std::cout << (frameMs.empty() ? "" : ")") << std::endl;
    return 0;
}
