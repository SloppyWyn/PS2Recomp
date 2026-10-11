// Stub argument test with poisoned stack slots.
//
// The EE passes integer arguments 1-8 in a0-a3, t0-t3 and arguments 9 and up on the stack in
// 8-byte slots from sp+0. Above them (sp+0x10.. for a call with at most 10 arguments) the
// caller's stack holds its own locals. A runtime handler that reads arguments 5-8 from the
// O32 "home slots" sp+0x10..0x1c reads those locals instead (the Walls 7/8 class of bug).
//
// This test calls every runtime handler reachable through the syscall and stub tables
// (PS2_SYSCALL_LIST, PS2_STUB_LIST) with four register sets (a0-t3 all zero, small integers,
// pointers to zeroed buffers, pointers to "%d %d %d %d" strings). For each set: one warm-up call,
// then the same a0-t3 with poison in sp+0x00..0x3f that is zero, pointers into region X, pointers
// into region Y (same content), and zero again (above the declared stack arguments only, for the
// few handlers with 9+). It compares everything observable: the register file, guest RAM,
// scratchpad, IOP RAM, GS VRAM, VU memories (all restored after each call), the DMA/GIF/GS/VIF
// counters, whether it threw or crashed. The guest heap is reset before each call so
// allocations repeat. On Windows x64 the host page holding the poison is also made
// inaccessible during the call, so every read or write of the window is recorded with the host
// code that did it, even when nothing observable changes with these inputs.
//
//   DIFF    outputs differ with the poison only: the handler uses the caller's locals
//   READ    the handler reads the window (home = sp+0x10.., sp+0..f = argument 9/10 slots)
//   WRITE   the handler writes into the caller's stack window
//   NONDET  the zero-poison call gave different outputs twice (id counter, rand, allocator):
//           the comparison says nothing for that register set, the trap still does
//   TODO    unimplemented stub (TODO_NAMED throws for its first calls)
//   SHALLOW same results for every register set (often: the handler needs state the test does
//           not set up and took an early exit, so little of it ran)
//
// No fixture is set up (no IOP modules, files, GS state or EE threads): each handler runs
// whatever path that leaves it. tools/stubaudit.py is the static complement.
//
// Every flagged handler must be in kKnownReaders (with the reason); a new one fails the test,
// and so does a listed one that is no longer flagged (remove it from the list).
//
// Environment: PS2X_POISON_ONLY=<name part,...> limits the sweep; PS2X_POISON_VERBOSE=1 prints
// every handler's verdict; PS2X_POISON_TRACE=1 prints each call before it is made (to find a
// handler that hangs or ends the process); PS2X_POISON_TIMEOUT_MS (default 10000) ends the
// process naming the handler when a call takes longer; PS2X_POISON_FROM=<name> and
// PS2X_POISON_SKIP=<name[@regset],...> help work through such handlers.

#include "MiniTest.h"
#include "poison_guard.h"
#include "ps2_host_stack.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // The process id, for scratch folders that must not be shared between concurrent test runs.
    unsigned long currentProcessId()
    {
#ifdef _WIN32
        return static_cast<unsigned long>(_getpid());
#else
        return static_cast<unsigned long>(getpid());
#endif
    }
}

namespace
{
    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    struct Entry
    {
        const char *name;
        Handler fn;
        bool syscall;
    };

    const Entry kEntries[] = {
#define PS2X_POISON_SYSCALL(name) {#name, &ps2_syscalls::name, true},
        PS2_SYSCALL_LIST(PS2X_POISON_SYSCALL)
#undef PS2X_POISON_SYSCALL
#define PS2X_POISON_STUB(name) {#name, &ps2_stubs::name, false},
        PS2_STUB_LIST(PS2X_POISON_STUB)
#undef PS2X_POISON_STUB
    };

    // Handlers (or some of their register sets) not called, with the reason. Names are exact; a
    // "syscall:" / "stub:" prefix limits the rule to one table (sceSifCallRpc and sceSifSendCmd
    // are in both). regsets: bit i set = kRegSets[i] not run (kAll = handler not called at all).
    // Every handler runs without a fixture (no IOP, files, GS or threads set up), so one that
    // needs such state takes its early-out path: the trap and the comparison see only the code
    // that ran. tools/stubaudit.py covers the rest statically.
    constexpr uint32_t kAll = 0xFu;
    constexpr uint32_t kSmall = 1u << 1;
    constexpr uint32_t kPtr = 1u << 2;
    constexpr uint32_t kFmt = 1u << 3;
    constexpr uint32_t kBig = kPtr | kFmt; // a pointer-sized value as a count/size

    struct Skip
    {
        const char *name;
        uint32_t regsets;
        const char *reason;
    };

    const Skip kSkips[] = {
        {"sceSifSetDma", kBig, "a1 is an entry count: 0x202000 entries take minutes"},
        {"isceSifSetDma", kBig, "a1 is an entry count: 0x202000 entries take minutes"},
        {"sceCdRead", kBig, "a1 sectors: a pointer-sized count writes over all of RAM, the stack included"},
        {"sceCdReadIOPm", kBig, "a1 sectors: a pointer-sized count writes over all of RAM, the stack included"},
        {"sceVu0RotTransPersN", kBig, "a3 is a vector count: a pointer-sized count sweeps all of RAM"},
        {"sceVu0ClipAll", kBig, "t0 is a vertex count: a pointer-sized count sweeps all of RAM until a vertex is inside"},
    };

    // Handlers with 9+ integer-class arguments (or float arguments past f19): the stack slots
    // from sp+0 that hold real arguments, with legitimate values. Signatures from
    // tools/stubaudit.py (SDK reference) and docs/STUB-AUDIT.md (checked against SRWZ's copies).
    struct StackArgs
    {
        const char *name;
        uint32_t slots;
        uint64_t values[4];
    };

    const StackArgs kStackArgs[] = {
        // sceGsSetDefClear(cp, ztest, x, y, w, h, r, g, b, a, z): b, a, z
        {"sceGsSetDefClear", 3, {0x40, 0x80, 0x1234}},
        // sceGifPkRefLoadImage(pkt, bp, psm, bw, image, size, x, y, w, h): w, h
        {"sceGifPkRefLoadImage", 2, {16, 16}},
        // sceSifCallRpc(cd, fno, mode, send, ssize, recv, rsize, end_func, end_para): end_para
        {"sceSifCallRpc", 1, {0x00210000}},
        {"SifCallRpc", 1, {0x00210000}},
        // sceVu0ViewScreenMatrix(m, scrz, ax, ay, cx, cy, zmin, zmax, nearz, farz): farz (f12-f19
        // hold the first eight floats)
        {"sceVu0ViewScreenMatrix", 1, {0x447A0000u}},
        // Game-specific (not SDK): a0-t3 + strAddr at sp+0 and param14 at sp+0x18 per its code;
        // docs/STUB-AUDIT.md "Unconfirmed".
        {"sceeFontGenerateString", 4, {0x00210000, 0, 0, 0}},
        // sceeFontPrintfAt2(buf, x, y, w, h, align, fmt, ...): seven named arguments, the first
        // vararg in t3, the rest from sp+0. The "fmt" register set's four conversions take
        // three stack slots.
        {"sceeFontPrintfAt2", 3, {0x11, 0x22, 0x33}},
    };

    // Handlers known to read (or write) the poison window, with the reason. Keep in step with
    // docs/STUB-AUDIT.md.
    struct Known
    {
        const char *name;
        const char *why;
    };

    // Empty: sceeFontPrintfAt (varargs from the O32 home slots, writing the caller's frame) was
    // the last one; it now spills t0-t3 below its caller's sp (Stubs/Font.cpp).
    const std::vector<Known> kKnownReaders = {};

    // ---- guest layout ---------------------------------------------------------------------
    constexpr uint32_t kBufBase = 0x00200000u;   // a0..t3 pointer targets, 8 KB each
    constexpr uint32_t kBufStride = 0x2000u;
    constexpr uint32_t kPoisonX = 0x00300000u;   // poison words point into X or Y (same content)
    constexpr uint32_t kPoisonY = 0x00380000u;
    constexpr uint32_t kPoisonStep = 0x100u;
    constexpr uint32_t kHeapBase = 0x00800000u;  // runtime guest heap (guestMalloc)
    constexpr uint32_t kHeapLimit = 0x01800000u;
    constexpr uint32_t kStackArea = 0x01E00000u; // the guest sp lies in the page after this
    constexpr uint32_t kWindow = 0x40u;          // poison sp+0x00..0x3f
    constexpr uint32_t kHomeStart = 0x10u;       // O32 home slots start
    constexpr uint32_t kReturnAddr = 0x00100008u;

    enum class Poison
    {
        Zero,
        PtrX,
        PtrY
    };

    struct RegSet
    {
        const char *name;
        uint32_t values[8]; // a0-a3, t0-t3
    };

    const RegSet kRegSets[] = {
        {"zero", {0, 0, 0, 0, 0, 0, 0, 0}},
        {"small", {1, 2, 3, 4, 5, 6, 7, 8}},
        {"ptr",
         {kBufBase + 0 * kBufStride, kBufBase + 1 * kBufStride, kBufBase + 2 * kBufStride, kBufBase + 3 * kBufStride,
          kBufBase + 4 * kBufStride, kBufBase + 5 * kBufStride, kBufBase + 6 * kBufStride, kBufBase + 7 * kBufStride}},
        {"fmt",
         {kBufBase + 8 * kBufStride, kBufBase + 9 * kBufStride, kBufBase + 10 * kBufStride, kBufBase + 11 * kBufStride,
          kBufBase + 12 * kBufStride, kBufBase + 13 * kBufStride, kBufBase + 14 * kBufStride, kBufBase + 15 * kBufStride}},
    };

    // ---- memory regions and their baselines -------------------------------------------------
    struct Region
    {
        const char *name;
        uint8_t *mem;
        size_t size;
        std::vector<uint8_t> base;
    };

    struct PageDiff
    {
        uint8_t region;
        uint32_t page;
        uint64_t hash;
        bool operator==(const PageDiff &o) const { return region == o.region && page == o.page && hash == o.hash; }
    };

    const char *const kRegionNames[] = {"rdram", "scratch", "iop", "gsvram", "vu0code", "vu0data", "vu1code", "vu1data"};

    uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ull)
    {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    struct Observation
    {
        uint32_t crashCode = 0;
        bool threw = false;
        std::vector<uint8_t> ctx;             // normalised R5900Context bytes
        std::array<int16_t, kWindow> window{}; // -1 = byte still holds what was written there
        std::vector<PageDiff> pages;
        uint64_t counters[4]{};
        uint32_t v0 = 0, v1 = 0;

        bool operator==(const Observation &o) const
        {
            return crashCode == o.crashCode && threw == o.threw && ctx == o.ctx && window == o.window &&
                   pages == o.pages && std::equal(std::begin(counters), std::end(counters), std::begin(o.counters));
        }
    };

    std::string describeDiff(const Observation &a, const Observation &b)
    {
        std::ostringstream os;
        os << std::hex;
        if (a.crashCode != b.crashCode)
            os << " crash " << a.crashCode << "/" << b.crashCode;
        if (a.threw != b.threw)
            os << " threw " << a.threw << "/" << b.threw;
        if (a.v0 != b.v0)
            os << " v0 0x" << a.v0 << "/0x" << b.v0;
        if (a.v1 != b.v1)
            os << " v1 0x" << a.v1 << "/0x" << b.v1;
        if (a.ctx != b.ctx && a.v0 == b.v0 && a.v1 == b.v1)
        {
            size_t i = 0;
            while (i < a.ctx.size() && a.ctx[i] == b.ctx[i])
                ++i;
            os << " ctx@+0x" << i;
        }
        if (a.window != b.window)
            os << " stack-window-writes";
        if (!(a.pages == b.pages))
        {
            std::set<std::pair<int, uint32_t>> pa, pb;
            for (const auto &p : a.pages)
                pa.insert({p.region, p.page});
            for (const auto &p : b.pages)
                pb.insert({p.region, p.page});
            int shown = 0;
            for (const auto &p : a.pages)
            {
                const bool same = std::find(b.pages.begin(), b.pages.end(), p) != b.pages.end();
                if (!same && shown++ < 3)
                    os << " " << kRegionNames[p.region] << "+0x" << (p.page * 0x1000u);
            }
            for (const auto &p : b.pages)
            {
                if (!pa.count({p.region, p.page}) && shown++ < 3)
                    os << " " << kRegionNames[p.region] << "+0x" << (p.page * 0x1000u);
            }
        }
        for (int i = 0; i < 4; ++i)
            if (a.counters[i] != b.counters[i])
                os << " counter" << i;
        return os.str();
    }

    struct Hit
    {
        uint32_t offset; // from sp
        bool write;
        std::vector<uint64_t> frames;
    };

    struct Verdict
    {
        std::string name;
        bool syscall = false;
        std::string skipReason;
        std::string partialReason;
        int64_t elapsedMs = 0;
        bool crashed = false;
        uint32_t crashCode = 0;
        bool threw = false;
        bool nondet = false;
        bool unimplemented = false; // TODO_NAMED: throws "Unimplemented PS2 stub" (first 8 calls)
        std::string threwWhat;
        bool shallow = false;       // same outputs for every register set: arguments not exercised
        std::vector<std::string> nondetDetail;
        std::vector<std::string> diffs; // "regset: detail"
        std::vector<Hit> reads;         // in the window, outside legit slots
        std::vector<Hit> writes;
        bool readsLow = false;          // read sp+0x00..0x0f (args 9-10 slots)
        bool readsHome = false;         // read sp+0x10..0x3f
    };

    // ---- the harness ----------------------------------------------------------------------
    struct Harness
    {
        PS2Runtime *runtime = nullptr; // leaked on purpose: handlers keep pointers to it
        uint8_t *rdram = nullptr;
        uint32_t sp = 0;
        uint8_t *spHost = nullptr;
        std::vector<Region> regions;
        R5900Context ctxTemplate{};
        std::atomic<const char *> current{nullptr};
        std::atomic<int64_t> callStartMs{0};
        bool trace = false;
        int64_t timeoutMs = 10000;
        std::thread watchdog;

        static int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        bool setUp()
        {
            runtime = new PS2Runtime();
            if (!runtime->memory().initialize())
                return false;
            PS2Memory &m = runtime->memory();
            rdram = m.getRDRAM();

            // Host page alignment for the trap: rdram + sp must start a host page.
            const uintptr_t host = reinterpret_cast<uintptr_t>(rdram) + kStackArea;
            const uintptr_t aligned = (host + poison_guard::kPageSize - 1) & ~(uintptr_t)(poison_guard::kPageSize - 1);
            sp = kStackArea + static_cast<uint32_t>(aligned - host);
            spHost = rdram + sp;
            if ((sp & 0xF) != 0)
                return false;

            // Pointer targets: buffers 0-7 zero, buffers 8-15 a format string. Four conversions: fits in the registers of every printf-style handler (snprintf has
            // the fewest, a3-t3), so a stack read is never asked for by the format.
            const char *fmt = "%d %d %d %d";
            for (int i = 8; i < 16; ++i)
                std::memcpy(rdram + kBufBase + i * kBufStride, fmt, std::strlen(fmt) + 1);

            regions.push_back({"rdram", rdram, PS2_RAM_SIZE, {}});
            regions.push_back({"scratch", m.getScratchpad(), PS2_SCRATCHPAD_SIZE, {}});
            regions.push_back({"iop", m.getIOPRAM(), 2u * 1024u * 1024u, {}});
            regions.push_back({"gsvram", m.getGSVRAM(), PS2_GS_VRAM_SIZE, {}});
            regions.push_back({"vu0code", m.getVU0Code(), PS2_VU0_CODE_SIZE, {}});
            regions.push_back({"vu0data", m.getVU0Data(), PS2_VU0_DATA_SIZE, {}});
            regions.push_back({"vu1code", m.getVU1Code(), PS2_VU1_CODE_SIZE, {}});
            regions.push_back({"vu1data", m.getVU1Data(), PS2_VU1_DATA_SIZE, {}});
            for (auto &r : regions)
            {
                if (!r.mem)
                    return false;
                r.base.assign(r.mem, r.mem + r.size);
            }

            ctxTemplate = R5900Context{};
            SET_GPR_U32(&ctxTemplate, 29, sp);
            SET_GPR_U32(&ctxTemplate, 31, kReturnAddr);
            for (int i = 0; i < 8; ++i)
                ctxTemplate.f[12 + i] = 1.0f + static_cast<float>(i);
            ctxTemplate.pc = 0x00100000u;

            if (const char *t = std::getenv("PS2X_POISON_TIMEOUT_MS"))
                timeoutMs = std::max<int64_t>(100, std::atoll(t));
            trace = std::getenv("PS2X_POISON_TRACE") != nullptr;
            poison_guard::install();
            watchdog = std::thread([this]()
                                   {
                                       for (;;)
                                       {
                                           std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                           const int64_t start = callStartMs.load();
                                           const char *name = current.load();
                                           if (start != 0 && name && nowMs() - start > timeoutMs)
                                           {
                                               std::string msg = std::string("[poison] TIMEOUT in handler ") + name +
                                                                 ": add it to kSkips\n";
                                               poison_guard::writeRealStderr(msg.c_str());
                                               std::_Exit(98);
                                           }
                                       }
                                   });
            watchdog.detach();
            return true;
        }

        static void writeWindow(uint8_t *window, const StackArgs *args, Poison poison)
        {
            const uint32_t legit = args ? args->slots * 8u : 0u;
            for (uint32_t off = 0; off < kWindow; off += 4)
            {
                uint32_t word = 0;
                if (off < legit)
                {
                    const uint64_t v = args->values[off / 8];
                    word = (off & 4) ? static_cast<uint32_t>(v >> 32) : static_cast<uint32_t>(v);
                }
                else if (poison == Poison::PtrX)
                    word = kPoisonX + (off / 4) * kPoisonStep;
                else if (poison == Poison::PtrY)
                    word = kPoisonY + (off / 4) * kPoisonStep;
                std::memcpy(window + off, &word, 4);
            }
        }

        struct CallArgs
        {
            Handler fn;
            uint8_t *rdram;
            R5900Context *ctx;
            PS2Runtime *runtime;
            bool threw;
            bool unimplemented;
            std::string what;
        };

        static void callCpp(void *p)
        {
            auto *a = static_cast<CallArgs *>(p);
            try
            {
                a->fn(a->rdram, a->ctx, a->runtime);
            }
            catch (const std::exception &ex)
            {
                a->threw = true;
                a->unimplemented = std::strstr(ex.what(), "Unimplemented PS2 stub") != nullptr;
                a->what = ex.what();
            }
            catch (...)
            {
                a->threw = true;
            }
        }

        Observation run(const Entry &e, const RegSet &rs, Poison poison, const StackArgs *args, Verdict &v)
        {
            R5900Context ctx = ctxTemplate;
            for (int i = 0; i < 8; ++i)
                SET_GPR_U32(&ctx, 4 + i, rs.values[i]);

            uint8_t written[kWindow];
            writeWindow(written, args, poison);
            std::memcpy(spHost, written, kWindow);

            PS2Memory &m = runtime->memory();
            const uint64_t c0[4] = {m.dmaStartCount(), m.gifCopyCount(), m.gsWriteCount(), m.vifWriteCount()};

            if (trace)
            {
                std::string msg = std::string("[poison] call ") + e.name + " " + rs.name + "\n";
                poison_guard::writeRealStderr(msg.c_str());
            }
            // The same heap state for every call (allocations land at the same addresses), in a
            // range clear of the buffers, the poison targets and the stack.
            runtime->configureGuestHeap(kHeapBase, kHeapLimit);
            CallArgs ca{e.fn, rdram, &ctx, runtime, false, false};
            current.store(e.name);
            callStartMs.store(nowMs());
            poison_guard::arm(spHost);
            const uint32_t code = poison_guard::callGuarded(&Harness::callCpp, &ca);
            poison_guard::disarm();
            callStartMs.store(0);

            Observation o;
            o.crashCode = code;
            o.threw = ca.threw;
            v.unimplemented = v.unimplemented || ca.unimplemented;
            if (ca.threw && !ca.unimplemented && v.threwWhat.empty())
                v.threwWhat = ca.what.empty() ? std::string("(not a std::exception)") : ca.what;
            o.v0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx.r[2]));
            o.v1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx.r[3]));
            ctx.insn_count = 0;
            ctx.cop0_count = 0;
            o.ctx.assign(reinterpret_cast<const uint8_t *>(&ctx), reinterpret_cast<const uint8_t *>(&ctx) + sizeof(ctx));
            for (uint32_t i = 0; i < kWindow; ++i)
                o.window[i] = (spHost[i] == written[i]) ? int16_t(-1) : int16_t(spHost[i]);
            const uint64_t c1[4] = {m.dmaStartCount(), m.gifCopyCount(), m.gsWriteCount(), m.vifWriteCount()};
            for (int i = 0; i < 4; ++i)
                o.counters[i] = c1[i] - c0[i];

            // Window back to baseline before the page compare, so it never counts as a diff.
            std::memcpy(spHost, regions[0].base.data() + sp, kWindow);
            for (size_t r = 0; r < regions.size(); ++r)
            {
                Region &reg = regions[r];
                for (size_t off = 0; off < reg.size; off += 4096)
                {
                    const size_t n = std::min<size_t>(4096, reg.size - off);
                    if (std::memcmp(reg.mem + off, reg.base.data() + off, n) != 0)
                    {
                        o.pages.push_back({static_cast<uint8_t>(r), static_cast<uint32_t>(off / 4096), fnv(reg.mem + off, n)});
                        std::memcpy(reg.mem + off, reg.base.data() + off, n);
                    }
                }
            }

            const uint32_t legit = args ? args->slots * 8u : 0u;
            for (size_t i = 0; i < poison_guard::hitCount(); ++i)
            {
                const poison_guard::Hit &h = poison_guard::hit(i);
                if (h.offset >= kWindow || h.offset < legit)
                    continue;
                auto &list = h.write ? v.writes : v.reads;
                const bool dup = std::any_of(list.begin(), list.end(), [&](const Hit &x)
                                             { return x.offset == h.offset && !x.frames.empty() && x.frames[0] == h.frames[0]; });
                if (!h.write)
                {
                    if (h.offset < kHomeStart)
                        v.readsLow = true;
                    else
                        v.readsHome = true;
                }
                if (!dup && list.size() < 64)
                    list.push_back({h.offset, h.write, std::vector<uint64_t>(h.frames, h.frames + h.frameCount)});
            }
            return o;
        }

        Verdict check(const Entry &e, uint32_t skippedRegsets = 0)
        {
            Verdict v;
            v.name = e.name;
            v.syscall = e.syscall;
            const StackArgs *args = nullptr;
            for (const auto &a : kStackArgs)
                if (std::strcmp(a.name, e.name) == 0)
                    args = &a;

            std::vector<Observation> firstByRegSet;
            for (size_t ri = 0; ri < std::size(kRegSets); ++ri)
            {
                if (skippedRegsets & (1u << ri))
                    continue;
                const RegSet &rs = kRegSets[ri];
                // Warm-up: handlers that return the previous setting (GsPutIMR, sceGsSyncVCallback,
                // ...) or initialise on first use answer the same way from here on.
                (void)run(e, rs, Poison::Zero, args, v);
                const Observation z1 = run(e, rs, Poison::Zero, args, v);
                firstByRegSet.push_back(z1);
                const Observation x1 = run(e, rs, Poison::PtrX, args, v);
                const Observation y1 = run(e, rs, Poison::PtrY, args, v);
                const Observation z2 = run(e, rs, Poison::Zero, args, v);
                for (const Observation *o : {&z1, &x1, &y1, &z2})
                {
                    if (o->crashCode)
                    {
                        v.crashed = true;
                        v.crashCode = o->crashCode;
                    }
                    v.threw = v.threw || o->threw;
                }
                if (!(z1 == z2))
                {
                    // Same inputs, different outputs (an id counter, rand, an allocator): the
                    // comparison says nothing; the trap still does.
                    v.nondet = true;
                    v.nondetDetail.push_back(std::string(rs.name) + ":" + describeDiff(z1, z2));
                    continue;
                }
                if (!(z1 == x1))
                    v.diffs.push_back(std::string(rs.name) + " zero/X:" + describeDiff(z1, x1));
                else if (!(x1 == y1))
                    v.diffs.push_back(std::string(rs.name) + " X/Y:" + describeDiff(x1, y1));
            }
            v.shallow = firstByRegSet.size() > 1 &&
                        std::all_of(firstByRegSet.begin(), firstByRegSet.end(),
                                    [&](const Observation &o)
                                    {
                                        // The register file holds the inputs themselves: compare
                                        // results only.
                                        const Observation &f = firstByRegSet.front();
                                        return o.crashCode == f.crashCode && o.threw == f.threw && o.v0 == f.v0 &&
                                               o.v1 == f.v1 && o.pages == f.pages && o.window == f.window &&
                                               std::equal(std::begin(o.counters), std::end(o.counters),
                                                          std::begin(f.counters));
                                    });
            return v;
        }
    };

    bool nameListed(const char *name, const char *list)
    {
        if (!list || !*list)
            return true;
        std::string s(list);
        size_t pos = 0;
        while (pos <= s.size())
        {
            size_t end = s.find(',', pos);
            if (end == std::string::npos)
                end = s.size();
            const std::string part = s.substr(pos, end - pos);
            if (!part.empty() && std::string(name).find(part) != std::string::npos)
                return true;
            pos = end + 1;
        }
        return false;
    }

    const Skip *findSkip(const Entry &e)
    {
        const std::string qualified = std::string(e.syscall ? "syscall:" : "stub:") + e.name;
        for (const auto &s : kSkips)
            if (qualified == s.name || std::strcmp(s.name, e.name) == 0)
                return &s;
        return nullptr;
    }

    std::string hex(uint64_t v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
        return b;
    }

    std::string describeHits(const std::vector<Hit> &hits)
    {
        // One line per host call chain, with the window offsets it touched.
        std::map<std::string, std::set<uint32_t>> chains;
        for (const Hit &h : hits)
        {
            std::string chain, previous;
            size_t shown = 0;
            for (size_t i = 0; i < h.frames.size() && shown < 5; ++i)
            {
                std::string d = ps2x_hoststack::describeAddress(h.frames[i]);
                const size_t space = d.find(' ');
                if (space != std::string::npos)
                    d = d.substr(space + 1); // drop "module+0xRVA"
                if (d == previous)
                    continue;
                previous = d;
                chain += (shown++ ? " <- " : "") + d;
            }
            chains[chain].insert(h.offset);
        }
        std::ostringstream os;
        for (const auto &c : chains)
        {
            os << "\n      sp+";
            bool first = true;
            for (uint32_t off : c.second)
            {
                os << (first ? "" : ",") << hex(off);
                first = false;
            }
            os << " at " << c.first;
        }
        return os.str();
    }

    // Test-only handlers that check the harness itself.
    void selfTestReadsHome(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t sp = GPR_U32(ctx, 29);
        SET_GPR_U32(ctx, 2, FAST_READ32(sp + 0x14));
    }
    void selfTestReadsHomeIgnored(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t sp = GPR_U32(ctx, 29);
        volatile uint32_t sink = FAST_READ32(sp + 0x18);
        (void)sink;
        SET_GPR_U32(ctx, 2, GPR_U32(ctx, 8));
    }
    void selfTestRegistersOnly(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t dst = GPR_U32(ctx, 4);
        FAST_WRITE32(dst, GPR_U32(ctx, 8) + GPR_U32(ctx, 11));
        SET_GPR_U32(ctx, 2, GPR_U32(ctx, 9));
    }
    void selfTestWritesHome(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t sp = GPR_U32(ctx, 29);
        FAST_WRITE32(sp + 0x20, 1u);
    }
    void selfTestReadsLegitStackArg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        // sceGsSetDefClear-like: 11 arguments, the last three at sp+0, sp+8, sp+0x10.
        const uint32_t sp = GPR_U32(ctx, 29);
        SET_GPR_U32(ctx, 2, FAST_READ32(sp + 0x00) + FAST_READ32(sp + 0x08) + FAST_READ32(sp + 0x10));
    }
    void selfTestWritesThroughPoison(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t sp = GPR_U32(ctx, 29);
        const uint32_t p = FAST_READ32(sp + 0x10);
        if (p)
            FAST_WRITE32(p, 0x12345678u);
    }

    Harness &harness()
    {
        static Harness *h = nullptr;
        if (!h)
        {
            h = new Harness();
            if (!h->setUp())
            {
                delete h;
                h = nullptr;
                static Harness broken;
                return broken;
            }
        }
        return *h;
    }
}

void register_ps2_stub_poison_tests()
{
    MiniTest::Case("StubPoison", [](TestCase &tc)
    {
        tc.Run("harness detects reads of the caller's home slots", [](TestCase &t)
        {
            Harness &h = harness();
            t.IsNotNull(h.runtime, "harness set-up");
            if (!h.runtime)
                return;
            poison_guard::silenceStdio(true);
            const Verdict a = h.check({"selfTestReadsHome", &selfTestReadsHome, false});
            const Verdict b = h.check({"selfTestReadsHomeIgnored", &selfTestReadsHomeIgnored, false});
            const Verdict c = h.check({"selfTestRegistersOnly", &selfTestRegistersOnly, false});
            const Verdict d = h.check({"selfTestWritesThroughPoison", &selfTestWritesThroughPoison, false});
            const Verdict w = h.check({"selfTestWritesHome", &selfTestWritesHome, false});
            // Borrows sceGsSetDefClear's declared stack arguments (three slots).
            const Verdict l = h.check({"sceGsSetDefClear", &selfTestReadsLegitStackArg, false});
            poison_guard::silenceStdio(false);
            t.IsFalse(a.diffs.empty(), "a handler returning sp+0x14 must show a DIFF");
            t.IsFalse(a.nondet || c.nondet, "the harness itself must be deterministic");
            t.IsTrue(b.diffs.empty(), "a read whose value is unused must not show a DIFF");
            t.IsTrue(c.diffs.empty() && c.reads.empty() && !c.crashed, "a register-only handler must be clean");
            t.IsFalse(d.diffs.empty(), "a write through a poison pointer must show a DIFF");
            t.IsTrue(l.diffs.empty() && l.reads.empty() && !l.nondet, "declared stack arguments are not poison");
            if (poison_guard::supported())
            {
                t.IsTrue(a.readsHome && !a.readsLow, "the trap must see the sp+0x14 read");
                t.IsTrue(b.readsHome, "the trap must see an unused read");
                t.IsTrue(c.reads.empty() && c.writes.empty(), "no trap hits for a register-only handler");
                t.IsTrue(!w.writes.empty() && w.writes.front().offset == 0x20, "the trap must see a write to sp+0x20");
            }
        });

        tc.Run("no runtime handler reads the caller's stack beyond its arguments", [](TestCase &t)
        {
            Harness &h = harness();
            t.IsNotNull(h.runtime, "harness set-up");
            if (!h.runtime)
                return;
            const char *only = std::getenv("PS2X_POISON_ONLY");
            const bool verbose = std::getenv("PS2X_POISON_VERBOSE") != nullptr;
            std::vector<Verdict> verdicts;
            size_t partial = 0;
            // File, memory card and CD handlers resolve paths against these roots: keep what
            // they create in a scratch folder, not in the working directory.
            const PS2Runtime::IoPaths savedPaths = PS2Runtime::getIoPaths();
            std::error_code ec;
            // One fixed folder, emptied before and after (a handler can leave a file open, or a
            // worker re-create a folder, so the removal at the end may not get everything).
            const std::filesystem::path scratch = std::filesystem::temp_directory_path(ec) /
                ("ps2x_poison_test_" + std::to_string(currentProcessId())); // per process: two test runs at once must not share it
            std::filesystem::remove_all(scratch, ec);
            std::filesystem::create_directories(scratch / "mc0", ec);
            {
                PS2Runtime::IoPaths p;
                p.elfDirectory = scratch;
                p.hostRoot = scratch;
                p.cdRoot = scratch;
                p.mcRoot = scratch / "mc0";
                PS2Runtime::setIoPaths(p);
            }
            const auto t0 = std::chrono::steady_clock::now();
            poison_guard::silenceStdio(true);
            // Discovery aids: PS2X_POISON_FROM=<name> starts the sweep at that handler,
            // PS2X_POISON_SKIP=<name[@regset],...> leaves more out.
            const char *from = std::getenv("PS2X_POISON_FROM");
            bool started = !from || !*from;
            std::vector<Skip> envSkips;
            std::vector<std::string> envSkipNames;
            if (const char *extra = std::getenv("PS2X_POISON_SKIP"))
            {
                std::stringstream ss(extra);
                std::string item;
                while (std::getline(ss, item, ','))
                {
                    if (item.empty())
                        continue;
                    uint32_t mask = kAll;
                    const size_t at = item.find('@');
                    if (at != std::string::npos)
                    {
                        mask = 0;
                        for (size_t ri = 0; ri < std::size(kRegSets); ++ri)
                            if (item.compare(at + 1, std::string::npos, kRegSets[ri].name) == 0)
                                mask = 1u << ri;
                        item.resize(at);
                    }
                    envSkipNames.push_back(item);
                    envSkips.push_back({nullptr, mask, "PS2X_POISON_SKIP"});
                }
                for (size_t i = 0; i < envSkips.size(); ++i)
                    envSkips[i].name = envSkipNames[i].c_str();
            }
            for (const Entry &e : kEntries)
            {
                if (!started && std::strcmp(e.name, from) == 0)
                    started = true;
                if (!started || !nameListed(e.name, only))
                    continue;
                const Skip *skip = findSkip(e);
                Skip merged{};
                for (const Skip &es : envSkips)
                {
                    if (std::strcmp(es.name, e.name) == 0)
                    {
                        merged = {e.name, (skip ? skip->regsets : 0u) | es.regsets, skip ? skip->reason : es.reason};
                        skip = &merged;
                    }
                }
                if (skip && skip->regsets == kAll)
                {
                    Verdict v;
                    v.name = e.name;
                    v.syscall = e.syscall;
                    v.skipReason = skip->reason;
                    verdicts.push_back(std::move(v));
                    continue;
                }
                const int64_t handlerStart = Harness::nowMs();
                verdicts.push_back(h.check(e, skip ? skip->regsets : 0u));
                verdicts.back().elapsedMs = Harness::nowMs() - handlerStart;
                if (skip)
                {
                    ++partial;
                    std::string sets;
                    for (size_t ri = 0; ri < std::size(kRegSets); ++ri)
                        if (skip->regsets & (1u << ri))
                            sets += std::string(sets.empty() ? "" : ",") + kRegSets[ri].name;
                    verdicts.back().partialReason = sets + " left out: " + skip->reason;
                }
            }
            poison_guard::silenceStdio(false);
            PS2Runtime::setIoPaths(savedPaths);
            std::filesystem::remove_all(scratch, ec);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            size_t tested = 0, skipped = 0, crashed = 0, nondet = 0, flagged = 0, todo = 0, shallow = 0;
            std::ostringstream report;
            std::set<std::string> flaggedNames;
            for (const Verdict &v : verdicts)
            {
                const std::string label = std::string(v.syscall ? "syscall " : "stub ") + v.name;
                if (!v.skipReason.empty())
                {
                    ++skipped;
                    if (verbose)
                        report << "  SKIP   " << label << ": " << v.skipReason << "\n";
                    continue;
                }
                ++tested;
                crashed += v.crashed ? 1 : 0;
                // An unimplemented stub throws for its first calls and then returns -1: its
                // outputs are not comparable, but the trap still sees any stack access.
                const bool nd = v.nondet && !v.unimplemented;
                nondet += nd ? 1 : 0;
                todo += v.unimplemented ? 1 : 0;
                shallow += (v.shallow && !v.unimplemented) ? 1 : 0;
                const bool bad = !v.diffs.empty() || v.readsHome || v.readsLow || !v.writes.empty();
                if (bad)
                {
                    ++flagged;
                    flaggedNames.insert(v.name);
                }
                if (bad || verbose || v.crashed)
                {
                    std::string tags;
                    if (!v.diffs.empty())
                        tags += " DIFF";
                    if (v.readsHome)
                        tags += " READ(home)";
                    if (v.readsLow)
                        tags += " READ(sp+0..f)";
                    if (!v.writes.empty())
                        tags += " WRITE";
                    if (nd)
                        tags += " NONDET";
                    if (v.unimplemented)
                        tags += " TODO";
                    if (v.crashed)
                        tags += " CRASH(" + hex(v.crashCode) + ")";
                    if (v.threw && !v.unimplemented)
                        tags += " THREW(" + v.threwWhat.substr(0, 60) + ")";
                    if (v.shallow && !v.unimplemented)
                        tags += " SHALLOW";
                    if (tags.empty())
                        tags = " clean";
                    report << " " << tags << "  " << label << "\n";
                    if (!v.partialReason.empty())
                        report << "      (" << v.partialReason << ")\n";
                    for (const auto &k : kKnownReaders)
                        if (bad && v.name == k.name)
                            report << "      known: " << k.why << "\n";
                    for (const auto &d : v.diffs)
                        report << "      " << d << "\n";
                    if (verbose)
                        for (const auto &d : v.nondetDetail)
                            report << "      nondet " << d << "\n";
                    if (!v.reads.empty())
                        report << "    reads:" << describeHits(v.reads) << "\n";
                    if (!v.writes.empty())
                        report << "    writes:" << describeHits(v.writes) << "\n";
                }
            }
            report << "[poison] handlers " << verdicts.size() << ": tested " << tested << " (" << partial << " with some register sets left out), skipped " << skipped
                   << ", crashed " << crashed << ", unimplemented (TODO_NAMED) " << todo << ", same result for every register set " << shallow << ", nondeterministic " << nondet << ", flagged " << flagged
                   << (poison_guard::supported() ? "" : " (no read trap on this platform)") << ", "
                   << static_cast<int>(secs) << " s\n";
            std::vector<const Verdict *> slowest;
            for (const Verdict &v : verdicts)
                slowest.push_back(&v);
            std::sort(slowest.begin(), slowest.end(), [](const Verdict *a, const Verdict *b)
                      { return a->elapsedMs > b->elapsedMs; });
            report << "[poison] slowest:";
            for (size_t i = 0; i < std::min<size_t>(5, slowest.size()); ++i)
                report << " " << slowest[i]->name << " " << slowest[i]->elapsedMs << " ms";
            report << "\n";
            poison_guard::writeRealStdout(report.str().c_str());

            std::set<std::string> known;
            for (const auto &k : kKnownReaders)
                known.insert(k.name);
            for (const auto &n : flaggedNames)
                t.IsTrue(known.count(n) != 0, "handler reads or writes the caller's stack: " + n);
            if (!only && !from && envSkips.empty() && poison_guard::supported())
            {
                for (const auto &k : kKnownReaders)
                {
                    t.IsTrue(flaggedNames.count(k.name) != 0,
                             std::string("known reader no longer flagged (fixed? remove from kKnownReaders): ") + k.name);
                }
            }
        });
    });
}
