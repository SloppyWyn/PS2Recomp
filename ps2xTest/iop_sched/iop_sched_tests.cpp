// IOP scheduling around EE RPC requests (ps2xIOP/src/emulator/iop_emulator.cpp,
// runThreadsBeforeServer): an RPC server function runs where its thread would on the
// hardware, after every ready thread of higher or equal priority has blocked, and not
// before a lower-priority thread is done only when it does not have to be.
//
// The test module (built here as a small absolute IRX) starts two threads:
//   T (priority from the test): forever { X = 1; spin N iterations; X = 2; DelayThread(1 ms) }
//   S (priority 20): sceSifRegisterRpc(sid, server), then sceSifRpcLoop.
// The server function returns a pointer to X, so the RPC reply is X as the server saw it.
// The test runs the IOP until T is in the middle of its spin (X == 1), then sends the RPC.

#include "emulator/iop_emulator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ps2x::iop;
using namespace ps2x::iop::detail;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool ok, const std::string &what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("FAIL: %s\n", what.c_str());
        }
        else
            std::printf("ok:   %s\n", what.c_str());
    }

    class TestHost final : public IopHost
    {
    public:
        TestHost() : guest(0x10000u, 0u) {}

        bool readGuest(uint32_t address, void *destination, size_t size) const override
        {
            if (address > guest.size() || size > guest.size() - address)
                return false;
            if (size)
                std::memcpy(destination, guest.data() + address, size);
            return true;
        }
        bool writeGuest(uint32_t address, const void *source, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address)
                return false;
            if (size)
                std::memcpy(guest.data() + address, source, size);
            return true;
        }
        bool zeroGuest(uint32_t address, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address)
                return false;
            std::memset(guest.data() + address, 0, size);
            return true;
        }
        bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override
        {
            normalized = address;
            return address <= guest.size();
        }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}

        std::vector<uint8_t> guest;
    };

    // MIPS encoders (registers by number: zero 0, v0 2, a0-a3 4-7, t0-t2 8-10, sp 29, ra 31).
    constexpr uint32_t ZERO = 0, V0 = 2, A0 = 4, A1 = 5, A2 = 6, A3 = 7, T0 = 8, T1 = 9, T2 = 10, SP = 29, RA = 31;
    uint32_t iType(uint32_t op, uint32_t rs, uint32_t rt, int32_t imm) { return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<uint32_t>(imm) & 0xFFFFu); }
    uint32_t addiu(uint32_t rt, uint32_t rs, int32_t imm) { return iType(9, rs, rt, imm); }
    uint32_t ori(uint32_t rt, uint32_t rs, uint32_t imm) { return iType(13, rs, rt, static_cast<int32_t>(imm)); }
    uint32_t lui(uint32_t rt, uint32_t imm) { return iType(15, 0, rt, static_cast<int32_t>(imm)); }
    uint32_t sw(uint32_t rt, int32_t offset, uint32_t base) { return iType(43, base, rt, offset); }
    uint32_t lw(uint32_t rt, int32_t offset, uint32_t base) { return iType(35, base, rt, offset); }
    uint32_t addu(uint32_t rd, uint32_t rs, uint32_t rt) { return (rs << 21) | (rt << 16) | (rd << 11) | 0x21u; }
    uint32_t jal(uint32_t target) { return (3u << 26) | ((target >> 2) & 0x03FFFFFFu); }
    uint32_t jr(uint32_t rs) { return (rs << 21) | 8u; }
    // Branch from the instruction at `at` to `target` (both absolute).
    uint32_t bne(uint32_t rs, uint32_t rt, uint32_t at, uint32_t target) { return iType(5, rs, rt, (static_cast<int32_t>(target) - static_cast<int32_t>(at + 4u)) / 4); }
    uint32_t beq(uint32_t rs, uint32_t rt, uint32_t at, uint32_t target) { return iType(4, rs, rt, (static_cast<int32_t>(target) - static_cast<int32_t>(at + 4u)) / 4); }
    constexpr uint32_t NOP = 0u;

    constexpr uint32_t kBase = 0x00010000u;
    constexpr uint32_t kEntry = kBase + 0x000u;
    constexpr uint32_t kThreadT = kBase + 0x100u;
    constexpr uint32_t kThreadS = kBase + 0x180u;
    constexpr uint32_t kServer = kBase + 0x200u;
    constexpr uint32_t kThbase = kBase + 0x300u; // stubs: +0x14 CreateThread, +0x1C StartThread, +0x24 DelayThread
    constexpr uint32_t kSifcmd = kBase + 0x340u; // stubs: +0x14 sceSifRegisterRpc, +0x1C sceSifRpcLoop
    constexpr uint32_t kParamT = kBase + 0x400u;
    constexpr uint32_t kParamS = kBase + 0x420u;
    constexpr uint32_t kX = kBase + 0x600u;
    constexpr uint32_t kServerData = kBase + 0x700u;
    constexpr uint32_t kServerBuffer = kBase + 0x800u;
    constexpr uint32_t kImageSize = 0x1000u;
    constexpr uint32_t kSid = 0x5EC0DE01u;
    constexpr uint32_t kSpin = 20000u;

    void put(std::vector<uint8_t> &image, uint32_t address, const std::vector<uint32_t> &words)
    {
        std::memcpy(image.data() + (address - kBase), words.data(), words.size() * 4u);
    }

    // The test module as an ELF image (ET_EXEC, one PT_LOAD at kBase).
    std::vector<uint8_t> buildModule(uint32_t priorityT)
    {
        std::vector<uint8_t> image(kImageSize, 0u);
        const uint32_t createThread = kThbase + 0x14u, startThread = kThbase + 0x1Cu, delayThread = kThbase + 0x24u;
        const uint32_t registerRpc = kSifcmd + 0x14u, rpcLoop = kSifcmd + 0x1Cu;

        put(image, kEntry, {
            addiu(SP, SP, -0x20), sw(RA, 0x1C, SP),
            lui(A0, kParamT >> 16), ori(A0, A0, kParamT & 0xFFFFu), jal(createThread), NOP,
            addu(A0, V0, ZERO), addu(A1, ZERO, ZERO), jal(startThread), NOP,
            lui(A0, kParamS >> 16), ori(A0, A0, kParamS & 0xFFFFu), jal(createThread), NOP,
            addu(A0, V0, ZERO), addu(A1, ZERO, ZERO), jal(startThread), NOP,
            lw(RA, 0x1C, SP), addu(V0, ZERO, ZERO), addiu(SP, SP, 0x20), jr(RA), NOP,
        });
        // T: top: X = 1; spin; X = 2; DelayThread(1000 us); goto top
        const uint32_t top = kThreadT + 8u, loop = kThreadT + 0x18u;
        put(image, kThreadT, {
            lui(T1, kX >> 16), ori(T1, T1, kX & 0xFFFFu),
            addiu(T0, ZERO, 1), sw(T0, 0, T1),
            lui(T2, kSpin >> 16), ori(T2, T2, kSpin & 0xFFFFu),
            addiu(T2, T2, -1), bne(T2, ZERO, kThreadT + 0x1Cu, loop), NOP,
            addiu(T0, ZERO, 2), sw(T0, 0, T1),
            addiu(A0, ZERO, 1000), jal(delayThread), NOP,
            beq(ZERO, ZERO, kThreadT + 0x38u, top), NOP,
        });
        // S: sceSifRegisterRpc(serverData, sid, server, buffer, 0, 0, 0); for (;;) sceSifRpcLoop
        const uint32_t sLoop = kThreadS + 0x38u;
        put(image, kThreadS, {
            addiu(SP, SP, -0x20), sw(ZERO, 0x10, SP), sw(ZERO, 0x14, SP), sw(ZERO, 0x18, SP),
            lui(A0, kServerData >> 16), ori(A0, A0, kServerData & 0xFFFFu),
            lui(A1, kSid >> 16), ori(A1, A1, kSid & 0xFFFFu),
            lui(A2, kServer >> 16), ori(A2, A2, kServer & 0xFFFFu),
            lui(A3, kServerBuffer >> 16), ori(A3, A3, kServerBuffer & 0xFFFFu),
            jal(registerRpc), NOP,
            jal(rpcLoop), NOP,
            beq(ZERO, ZERO, kThreadS + 0x40u, sLoop), NOP,
        });
        // server(fn, buffer, size): return &X
        put(image, kServer, {lui(V0, kX >> 16), jr(RA), ori(V0, V0, kX & 0xFFFFu)});
        put(image, kThbase, {0x41E00000u, 0u, 0x101u, 0x61626874u, 0x00006573u, // "thbase"
                             jr(RA), addiu(ZERO, ZERO, 4), jr(RA), addiu(ZERO, ZERO, 6), jr(RA), addiu(ZERO, ZERO, 33), 0u, 0u});
        put(image, kSifcmd, {0x41E00000u, 0u, 0x101u, 0x63666973u, 0x0000646Du, // "sifcmd"
                             jr(RA), addiu(ZERO, ZERO, 17), jr(RA), addiu(ZERO, ZERO, 22), 0u, 0u});
        put(image, kParamT, {0x02000000u, 0u, kThreadT, 0x800u, priorityT});
        put(image, kParamS, {0x02000000u, 0u, kThreadS, 0x800u, 20u});

        // ELF header + program header in front of the image.
        std::vector<uint8_t> elf(0x100u + image.size(), 0u);
        const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
        std::memcpy(elf.data(), ident, sizeof(ident));
        auto w16 = [&](size_t at, uint16_t v) { std::memcpy(elf.data() + at, &v, 2); };
        auto w32 = [&](size_t at, uint32_t v) { std::memcpy(elf.data() + at, &v, 4); };
        w16(16, 2u); w16(18, 8u); w32(20, 1u); w32(24, kEntry); w32(28, 52u);
        w16(40, 52u); w16(42, 32u); w16(44, 1u);
        w32(52 + 0, 1u); w32(52 + 4, 0x100u); w32(52 + 8, kBase); w32(52 + 12, kBase);
        w32(52 + 16, kImageSize); w32(52 + 20, kImageSize); w32(52 + 24, 7u); w32(52 + 28, 4u);
        std::memcpy(elf.data() + 0x100u, image.data(), image.size());
        return elf;
    }

    uint32_t readX(const IopEmulator &iop)
    {
        uint32_t x = 0u;
        (void)iop.readMemory(kX, &x, 4u);
        return x;
    }

    struct Outcome
    {
        bool loaded = false;
        bool midSpin = false;  // X == 1 when the RPC was sent
        uint32_t reply = 0u;   // X as the server saw it
        uint64_t rpcCycles = 0u; // IOP cycles the RPC took
        uint64_t repaidCycles = 0u; // IOP cycles the next EE slice of the same length ran
    };

    Outcome runScenario(uint32_t priorityT)
    {
        Outcome out;
        TestHost host;
        IopEmulator iop(host);
        const std::vector<uint8_t> elf = buildModule(priorityT);
        constexpr uint32_t kLoadAt = 0x00100000u;
        if (!iop.writeMemory(kLoadAt, elf.data(), elf.size()))
            return out;
        const ModuleLoadResult load = iop.loadModuleBuffer(kLoadAt, nullptr, 0u);
        out.loaded = load.moduleId > 0 && iop.threadCount() == 2u;

        // Let both threads start: T spins once (X = 2), delays; S registers and sleeps.
        for (int i = 0; i < 4000 && !(iop.hasRpcServer(kSid) && readX(iop) == 2u); ++i)
            iop.runEeCycles(8u * 64u);
        // Run until T is in the middle of its next spin.
        for (int i = 0; i < 20000 && readX(iop) != 1u; ++i)
            iop.runEeCycles(8u * 64u);
        iop.runEeCycles(8u * 1000u);
        out.midSpin = readX(iop) == 1u;

        RpcRequest request{};
        request.sid = kSid;
        request.function = 1u;
        request.receive = {0x100u, 4u};
        const uint64_t before = iop.cycles();
        const RpcResult result = iop.handleRpc(request);
        out.rpcCycles = iop.cycles() - before;
        (void)result;
        std::memcpy(&out.reply, host.guest.data() + 0x100u, 4u);
        const uint64_t afterRpc = iop.cycles();
        iop.runEeCycles(8u * 100u);
        out.repaidCycles = iop.cycles() - afterRpc;
        return out;
    }
}

int main()
{
    {
        // T (priority 10) above the server's thread (20): T finishes its spin before the server.
        const Outcome o = runScenario(10u);
        check(o.loaded, "higher-priority thread: module loaded, two threads");
        check(o.midSpin, "higher-priority thread: RPC sent while T is mid-spin (X == 1)");
        check(o.reply == 2u, "higher-priority thread: the server runs after T blocks (sees X == 2), got " + std::to_string(o.reply));
        check(o.rpcCycles >= kSpin / 2u, "higher-priority thread: T's remaining spin ran before the server (" + std::to_string(o.rpcCycles) + " cycles)");
        check(o.repaidCycles == 0u, "higher-priority thread: that IOP time is repaid from the next EE slice (ran " + std::to_string(o.repaidCycles) + ")");
    }
    {
        // T (priority 20) equal to the server's: a thread already ready or preempted mid-work goes first.
        const Outcome o = runScenario(20u);
        check(o.midSpin && o.reply == 2u, "equal-priority thread: runs to its block before the server, got " + std::to_string(o.reply));
    }
    {
        // T (priority 30) below the server's thread: the server preempts it, as the hardware would.
        const Outcome o = runScenario(30u);
        check(o.loaded && o.midSpin, "lower-priority thread: RPC sent while T is mid-spin");
        check(o.reply == 1u, "lower-priority thread: the server runs at once (sees X == 1), got " + std::to_string(o.reply));
        check(o.rpcCycles < 64u, "lower-priority thread: no IOP time spent before the server (" + std::to_string(o.rpcCycles) + " cycles)");
        check(o.repaidCycles == 100u, "lower-priority thread: next EE slice runs in full (" + std::to_string(o.repaidCycles) + ")");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
