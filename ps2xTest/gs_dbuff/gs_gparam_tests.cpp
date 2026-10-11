// libgraph's graphics-mode plumbing against SDK 3.1.0 libgraph (the copies in SLPS-25887,
// Super Robot Taisen Z), not against the runtime:
//   sceGsGetGParam  0x10c610  `lui v0, 0x3f; jr ra; addiu v0, v0, 0x3ca0`: returns libgraph's one
//                             sceGsGParam variable (0x3f3ca0 there); every libgraph function reads
//                             it through this getter, and so does the game's own code.
//   sceGsResetGraph 0x10c480  mode 0 and 5: gp = sceGsGetGParam() (first jal, 0x10c4ec);
//                             gp[0] = interlace, gp[1] = omode (shorts, as given),
//                             gp[3] = (GS CSR >> 16) & 0xff, gp[2] = (ffmode != 0); +8..+15 are
//                             libgraph's vsync-callback state, not written here.
//   sceGszbufaddr   0x10cb70  (psm a0, short w a1, short h a2): ((w + 63) >> 6) * ((h + 31) >> 5)
//                             ((h + 63) >> 6 when psm & 2), times 1 when the gparam doubleword
//                             & 0xffff0000ffff == 1 (interlace 1, ffmode 0), else times 2.
//   sceGsSetDefDrawEnv 0x10cc38 (env, psm, w, h, ztest t0, zpsm t1): see gs_dbuff_tests.cpp;
//                             returns 8. sceGsSetDefDrawEnv2 (not linked in SRWZ) is the same
//                             with FRAME_2/ZBUF_2/XYOFFSET_2/SCISSOR_2/TEST_2 (0x4d/0x4f/0x19/0x41/0x48).
// The guest libgraph below is a minimal stand-in with the same shape: the getter's three
// instructions, and a sceGsResetGraph whose first jal is the getter and which loads the GS
// register base (lui 0x1200), called with jal so that ra - 8 names it.
// Expected arrays: gen_gparam.py. Returns non-zero on any failure.

#include "ps2_runtime.h"
#include "ps2_stubs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    int g_pass = 0;
    int g_fail = 0;

    void check(bool ok, const std::string &what)
    {
        if (ok)
            ++g_pass;
        else
        {
            ++g_fail;
            std::printf("FAIL %s\n", what.c_str());
        }
    }

    void setReg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
    }

    constexpr uint32_t kGetter = 0x01800000u;
    constexpr uint32_t kResetGraph = 0x01800100u;
    constexpr uint32_t kCallReset = 0x01800400u;  // jal kResetGraph
    constexpr uint32_t kCallGetter = 0x01800410u; // jal kGetter
    constexpr uint32_t kJalrReset = 0x01800420u;  // jalr t9 (t9 = kResetGraph), nop
    constexpr uint32_t kJalrResetBad = 0x01800430u; // jalr t9, delay slot addiu t9, t9, 4
    constexpr uint32_t kVar = 0x01803ca0u;
    constexpr uint32_t kEnv = 0x01810000u;
    constexpr uint32_t kStack = 0x01820000u;
    constexpr uint64_t kCsr = 0x551b0000ull; // GS ID 0x55, REV 0x1b

    uint32_t jal(uint32_t target) { return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu); }

    // Each harness lives until the end of main: the runtime keeps per-runtime graphics-mode
    // state keyed by the PS2Runtime, and a freed one could be reallocated at the same address.
    struct Harness
    {
        std::unique_ptr<PS2Runtime> runtime = std::make_unique<PS2Runtime>();
        uint8_t *rdram = nullptr;

        bool init()
        {
            if (!runtime->memory().initialize())
                return false;
            rdram = runtime->memory().getRDRAM();
            runtime->memory().gs().csr.store(kCsr);
            return true;
        }

        void put32(uint32_t addr, uint32_t v) { std::memcpy(rdram + addr, &v, 4); }
        int16_t s16(uint32_t addr) const
        {
            int16_t v = 0;
            std::memcpy(&v, rdram + addr, 2);
            return v;
        }
        uint64_t q(uint32_t addr) const
        {
            uint64_t v = 0;
            std::memcpy(&v, rdram + addr, 8);
            return v;
        }

        // A guest libgraph: getter, sceGsResetGraph, two call sites; the variable starts as in
        // SLPS-25887's ELF (1, 2, 1, 3) followed by 8 guard bytes.
        void layoutGuestLibgraph()
        {
            put32(kGetter + 0, 0x3C020000u | (kVar >> 16)); // lui v0, hi
            put32(kGetter + 4, 0x03E00008u);                // jr ra
            put32(kGetter + 8, 0x24420000u | (kVar & 0xFFFFu)); // addiu v0, v0, lo
            const uint32_t body[] = {
                0x27BDFFB0u, 0x00042400u, 0xFFB30030u, 0x24020001u, // prologue-ish
                jal(kGetter), 0x00000000u,                          // gp = sceGsGetGParam()
                0x3C031200u, 0x24040200u, 0x34631000u,              // lui v1, 0x1200 (GS CSR)
                jal(0x01800300u), 0x00000000u,                      // a later call, not the getter
                0x03E00008u, 0x27BD0050u,                           // jr ra
            };
            for (uint32_t i = 0; i < sizeof(body) / 4u; ++i)
                put32(kResetGraph + i * 4u, body[i]);
            put32(kCallReset, jal(kResetGraph));
            put32(kCallReset + 4, 0u);
            put32(kCallGetter, jal(kGetter));
            put32(kCallGetter + 4, 0u);
            put32(kJalrReset, 0x0320F809u);         // jalr ra, t9
            put32(kJalrReset + 4, 0u);
            put32(kJalrResetBad, 0x0320F809u);      // jalr ra, t9
            put32(kJalrResetBad + 4, 0x27390004u);  // addiu t9, t9, 4: the slot rewrites t9
            const int16_t init[4] = {1, 2, 1, 3};
            std::memcpy(rdram + kVar, init, 8);
            std::memset(rdram + kVar + 8, 0xAB, 8);
        }

        bool varIs(int16_t il, int16_t om, int16_t ff, int16_t ver) const
        {
            return s16(kVar) == il && s16(kVar + 2) == om && s16(kVar + 4) == ff && s16(kVar + 6) == ver;
        }
        bool varGuardIntact() const
        {
            for (uint32_t i = 8; i < 16; ++i)
                if (rdram[kVar + i] != 0xAB)
                    return false;
            return true;
        }
        bool scratchUntouched() const
        {
            const uint8_t *sp = runtime->memory().getScratchpad();
            for (uint32_t i = 0; i < 0x200; ++i)
                if (sp[i] != 0)
                    return false;
            return true;
        }

        void resetGraph(uint32_t mode, uint32_t il, uint32_t om, uint32_t ff, uint32_t ra, uint32_t t9 = 0u)
        {
            R5900Context ctx{};
            setReg(ctx, 4, mode);
            setReg(ctx, 5, il);
            setReg(ctx, 6, om);
            setReg(ctx, 7, ff);
            setReg(ctx, 31, ra);
            setReg(ctx, 25, t9);
            ctx.pc = ra; // as the recompiler's stub wrapper leaves it
            ps2_stubs::sceGsResetGraph(rdram, &ctx, runtime.get());
        }

        uint32_t getGParam(uint32_t ra, uint32_t t9 = 0u)
        {
            R5900Context ctx{};
            setReg(ctx, 31, ra);
            setReg(ctx, 25, t9);
            ctx.pc = ra; // as the recompiler's stub wrapper leaves it
            ps2_stubs::sceGsGetGParam(rdram, &ctx, runtime.get());
            return getRegU32(&ctx, 2);
        }

        int32_t zbufaddr(uint32_t psm, uint32_t w, uint32_t h)
        {
            R5900Context ctx{};
            setReg(ctx, 4, psm);
            setReg(ctx, 5, w);
            setReg(ctx, 6, h);
            setReg(ctx, 7, 0x7777u); // junk where the old stub read h
            ps2_stubs::sceGszbufaddr(rdram, &ctx, runtime.get());
            return static_cast<int32_t>(getRegU32(&ctx, 2));
        }
    };

    std::vector<std::unique_ptr<Harness>> g_harnesses;

    Harness *newHarness(const std::string &tag)
    {
        g_harnesses.push_back(std::make_unique<Harness>());
        Harness *hx = g_harnesses.back().get();
        if (!hx->init())
        {
            check(false, tag + ": runtime init");
            return nullptr;
        }
        return hx;
    }

    // The game's libgraph is called (ResetGraph bound, the getter not): the runtime must write
    // libgraph's own variable, hand it out, and read the game's changes to it.
    void testGuestVariableFromResetGraph()
    {
        const std::string t = "guest gparam via sceGsResetGraph";
        Harness *hx = newHarness(t);
        if (!hx)
            return;
        hx->layoutGuestLibgraph();

        hx->resetGraph(0u, 1u, 3u, 2u, kCallReset + 8u);
        check(hx->varIs(1, 3, 1, 0x1b), t + ": ResetGraph(0, 1, 3, 2) stores 1, 3, (2 != 0) = 1, CSR REV 0x1b (got " +
                                            std::to_string(hx->s16(kVar)) + "," + std::to_string(hx->s16(kVar + 2)) + "," +
                                            std::to_string(hx->s16(kVar + 4)) + "," + std::to_string(hx->s16(kVar + 6)) + ")");
        check(hx->varGuardIntact(), t + ": bytes +8..+15 (vsync-callback state) untouched");
        check(hx->scratchUntouched(), t + ": scratchpad untouched");

        check(hx->getGParam(0u) == kVar, t + ": sceGsGetGParam returns libgraph's variable");

        // The game's own code reads and writes the variable: the stubs follow it.
        int16_t ff = 0;
        std::memcpy(hx->rdram + kVar + 4, &ff, 2);
        check(hx->zbufaddr(0u, 640u, 448u) == 140, t + ": field mode written by the guest -> zbufaddr 140");
        ff = 1;
        std::memcpy(hx->rdram + kVar + 4, &ff, 2);
        check(hx->zbufaddr(0u, 640u, 448u) == 280, t + ": frame mode written by the guest -> zbufaddr 280");

        hx->resetGraph(5u, 0u, 2u, 0u, kCallReset + 8u);
        check(hx->varIs(0, 2, 0, 0x1b), t + ": ResetGraph(5, 0, 2, 0) stores 0, 2, 0, 0x1b");
        hx->resetGraph(1u, 1u, 3u, 1u, kCallReset + 8u);
        check(hx->varIs(0, 2, 0, 0x1b), t + ": ResetGraph(1, ...) leaves the variable alone");
    }

    // A game that binds sceGsGetGParam itself: the getter's body names the variable.
    void testGuestVariableFromGetter()
    {
        const std::string t = "guest gparam via bound sceGsGetGParam";
        Harness *hx = newHarness(t);
        if (!hx)
            return;
        hx->layoutGuestLibgraph();
        check(hx->getGParam(kCallGetter + 8u) == kVar, t + ": returns the getter's variable");
        check(hx->varIs(1, 2, 1, 3), t + ": variable not rewritten by the getter");
        hx->resetGraph(0u, 1u, 2u, 0u, 0u); // called through a pointer: no jal to decode
        check(hx->varIs(1, 2, 0, 0x1b), t + ": a later ResetGraph(0, 1, 2, 0) writes it");
        check(hx->varGuardIntact(), t + ": bytes +8..+15 untouched");
    }

    // ResetGraph runs first where its caller cannot be decoded; the getter is found later. The
    // reset values (only in the runtime's mirror so far) must go into the variable, not the
    // variable's initial contents into the mirror.
    void testResetBeforeLocate()
    {
        const std::string t = "ResetGraph before the variable is found";
        Harness *hx = newHarness(t);
        if (!hx)
            return;
        hx->layoutGuestLibgraph();
        hx->resetGraph(0u, 1u, 2u, 0u, 0u); // through a pointer: nothing to decode
        check(hx->varIs(1, 2, 1, 3), t + ": variable still the ELF's 1, 2, 1, 3 meanwhile");
        check(hx->getGParam(kCallGetter + 8u) == kVar, t + ": the bound getter names the variable");
        check(hx->varIs(1, 2, 0, 0x1b), t + ": variable now holds the reset values 1, 2, 0, 0x1b");
        check(hx->zbufaddr(0u, 640u, 448u) == 140, t + ": zbufaddr still sees field mode (140)");
        check(hx->varGuardIntact(), t + ": bytes +8..+15 untouched");
    }

    // sceGsGetGParam handed out the runtime's block first; then ResetGraph finds libgraph's
    // variable. The block must not go stale: it follows every change.
    void testBlockThenLocate()
    {
        const std::string t = "block handed out, then the variable is found";
        Harness *hx = newHarness(t);
        if (!hx)
            return;
        hx->layoutGuestLibgraph();
        const uint32_t block = hx->getGParam(0u);
        check(block != 0u && block != kVar && block + 8u <= 0x02000000u, t + ": first a runtime block");
        if (block == 0u || block + 8u > 0x02000000u)
            return;
        auto blockIs = [&](int16_t a, int16_t b, int16_t c, int16_t d)
        { return hx->s16(block) == a && hx->s16(block + 2) == b && hx->s16(block + 4) == c && hx->s16(block + 6) == d; };
        hx->resetGraph(0u, 0u, 2u, 0u, kCallReset + 8u);
        check(hx->varIs(0, 2, 0, 0x1b), t + ": located ResetGraph writes the variable");
        check(blockIs(0, 2, 0, 0x1b), t + ": ... and the old block");
        check(hx->getGParam(0u) == kVar, t + ": later calls return the variable");
        const int16_t il = 1;
        std::memcpy(hx->rdram + kVar, &il, 2); // the game's own code changes the variable
        check(hx->zbufaddr(0u, 640u, 448u) == 140, t + ": the stubs read the game's change (field, 140)");
        check(blockIs(1, 2, 0, 0x1b), t + ": the old block follows the game's change");
    }

    // Called through jalr: the target is still in rs at stub entry. Refused when the delay
    // slot writes rs (then rs no longer holds the target).
    void testJalr()
    {
        {
            const std::string t = "ResetGraph called through jalr";
            Harness *hx = newHarness(t);
            if (!hx)
                return;
            hx->layoutGuestLibgraph();
            hx->resetGraph(0u, 1u, 2u, 0u, kJalrReset + 8u, kResetGraph);
            check(hx->varIs(1, 2, 0, 0x1b), t + ": variable located and written");
            check(hx->getGParam(0u) == kVar, t + ": GetGParam returns it");
        }
        {
            const std::string t = "ResetGraph through jalr, slot rewrites rs";
            Harness *hx = newHarness(t);
            if (!hx)
                return;
            hx->layoutGuestLibgraph();
            hx->resetGraph(0u, 1u, 2u, 0u, kJalrResetBad + 8u, kResetGraph + 4u);
            check(hx->varIs(1, 2, 1, 3), t + ": not located (variable untouched)");
            check(hx->getGParam(0u) != kVar, t + ": GetGParam falls back to the runtime block");
        }
    }

    // No guest libgraph to find: a runtime-owned block in RDRAM, not scratchpad, in the
    // sceGsGParam layout.
    void testFallbackBlock()
    {
        const std::string t = "runtime gparam block";
        Harness *hx = newHarness(t);
        if (!hx)
            return;
        hx->resetGraph(0u, 1u, 2u, 1u, 0u);
        const uint32_t a = hx->getGParam(0u);
        check(a != 0u && a + 8u <= 0x02000000u, t + ": in RDRAM, not scratchpad (got 0x" + [&]
                                                   {
                                                       char b[16];
                                                       std::snprintf(b, sizeof(b), "%x", a);
                                                       return std::string(b); }() + ")");
        if (a != 0u && a + 8u <= 0x02000000u)
        {
            int16_t v[4] = {};
            std::memcpy(v, hx->rdram + a, 8);
            check(v[0] == 1 && v[1] == 2 && v[2] == 1 && v[3] == 0x1b, t + ": four shorts 1, 2, 1, 0x1b");
            hx->resetGraph(0u, 1u, 2u, 0u, 0u);
            check(hx->getGParam(0u) == a, t + ": same block after another ResetGraph");
            std::memcpy(v, hx->rdram + a, 8);
            check(v[0] == 1 && v[1] == 2 && v[2] == 0 && v[3] == 0x1b, t + ": four shorts 1, 2, 0, 0x1b");
        }
        check(hx->scratchUntouched(), t + ": scratchpad untouched");
    }

    struct ZbufCase
    {
        const char *name;
        uint32_t psm, w, h;
        uint32_t gparam[3]; // interlace, omode, ffmode
        int32_t expected;
    };

    const ZbufCase kZbufCases[] = {
        {"ct32_640x224_frame", 0, 640, 224, {1, 2, 1}, 140},
        {"ct16_640x448_frame", 2, 640, 448, {1, 2, 1}, 140},
        {"ct32_640x448_field", 0, 640, 448, {1, 2, 0}, 140},
        {"ct16s_512x448_field", 10, 512, 448, {1, 2, 0}, 56},
        {"ct32_320x240_progressive", 0, 320, 240, {0, 2, 0}, 80},
    };

    void testZbufaddr(const ZbufCase &c)
    {
        const std::string t = std::string("sceGszbufaddr ") + c.name;
        Harness *hx = newHarness(t);
        if (!hx)
            return;
        hx->resetGraph(0u, c.gparam[0], c.gparam[1], c.gparam[2], 0u);
        const int32_t got = hx->zbufaddr(c.psm, c.w, c.h);
        check(got == c.expected, t + ": expected " + std::to_string(c.expected) + " got " + std::to_string(got));
    }

    struct DrawCase
    {
        const char *name;
        bool ctx2;
        uint32_t psm, w, h, ztest, zpsm;
        uint32_t gparam[3];
        uint8_t fill;
        uint64_t q[16];
    };

    const DrawCase kDrawCases[] = {
{"env1_ct32_640x224_z24_ztest1_frame", false, 0, 640, 224, 1, 0x31, {1, 2, 1}, 0x5a, {
    0x00000000000a0000ull, 0x000000000000004cull, 0x000000000100008cull, 0x000000000000004eull,
    0x0000790000006c00ull, 0x0000000000000018ull, 0x00df0000027f0000ull, 0x0000000000000040ull,
    0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull,
    0x5a5a5a5a5a5a5a5aull, 0x0000000000000045ull, 0x0000000000030000ull, 0x0000000000000047ull,
}},
{"env1_ct16_640x448_z16_ztest0_field", false, 2, 640, 448, 0, 0x32, {1, 2, 0}, 0x00, {
    0x00000000020a0000ull, 0x000000000000004cull, 0x0000000102000046ull, 0x000000000000004eull,
    0x0000720000006c00ull, 0x0000000000000018ull, 0x01bf0000027f0000ull, 0x0000000000000040ull,
    0x0000000000000001ull, 0x000000000000001aull, 0x0000000000000001ull, 0x0000000000000046ull,
    0x0000000000000001ull, 0x0000000000000045ull, 0x0000000000030000ull, 0x0000000000000047ull,
}},
{"env2_ct32_640x224_z24_ztest2_frame", true, 0, 640, 224, 2, 0x31, {1, 2, 1}, 0x5a, {
    0x00000000000a0000ull, 0x000000000000004dull, 0x000000000100008cull, 0x000000000000004full,
    0x0000790000006c00ull, 0x0000000000000019ull, 0x00df0000027f0000ull, 0x0000000000000041ull,
    0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull,
    0x5a5a5a5a5a5a5a5aull, 0x0000000000000045ull, 0x0000000000050000ull, 0x0000000000000048ull,
}},
{"env2_ct16s_512x448_z32_ztest3_field", true, 10, 512, 448, 3, 0x30, {1, 2, 0}, 0x5a, {
    0x000000000a080000ull, 0x000000000000004dull, 0x0000000000000038ull, 0x000000000000004full,
    0x0000720000007000ull, 0x0000000000000019ull, 0x01bf000001ff0000ull, 0x0000000000000041ull,
    0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull,
    0x5a5a5a5a5a5a5a5bull, 0x0000000000000045ull, 0x0000000000070000ull, 0x0000000000000048ull,
}},
    };

    void testDrawEnv(const DrawCase &c)
    {
        const std::string t = std::string(c.ctx2 ? "sceGsSetDefDrawEnv2 " : "sceGsSetDefDrawEnv ") + c.name;
        Harness *hx = newHarness(t);
        if (!hx)
            return;
        hx->resetGraph(0u, c.gparam[0], c.gparam[1], c.gparam[2], 0u);
        std::memset(hx->rdram + kEnv - 16u, 0xEE, 16u);
        std::memset(hx->rdram + kEnv, c.fill, 0x80u);
        std::memset(hx->rdram + kEnv + 0x80u, 0xEE, 16u);
        std::memset(hx->rdram + kStack, 0x77, 64u);

        R5900Context ctx{};
        setReg(ctx, 4, kEnv);
        setReg(ctx, 5, c.psm);
        setReg(ctx, 6, c.w);
        setReg(ctx, 7, c.h);
        setReg(ctx, 8, c.ztest);
        setReg(ctx, 9, c.zpsm);
        setReg(ctx, 29, kStack);
        if (c.ctx2)
            ps2_stubs::sceGsSetDefDrawEnv2(hx->rdram, &ctx, hx->runtime.get());
        else
            ps2_stubs::sceGsSetDefDrawEnv(hx->rdram, &ctx, hx->runtime.get());

        for (uint32_t i = 0; i < 16u; ++i)
        {
            const uint64_t got = hx->q(kEnv + i * 8u);
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s: +0x%02x expected 0x%016llx got 0x%016llx", t.c_str(), i * 8u,
                          static_cast<unsigned long long>(c.q[i]), static_cast<unsigned long long>(got));
            check(got == c.q[i], buf);
        }
        bool guards = true;
        for (uint32_t i = 0; i < 16u; ++i)
            guards = guards && hx->rdram[kEnv - 16u + i] == 0xEE && hx->rdram[kEnv + 0x80u + i] == 0xEE;
        check(guards, t + ": nothing written outside the 0x80-byte sceGsDrawEnv");
        check(getRegU32(&ctx, 2) == 8u, t + ": returns 8");
    }

    // sceGsSetDefDBuffDc: its ZBUF comes from sceGszbufaddr(psm, w, h).
    void testDBuffDcZbuf()
    {
        const std::string t = "sceGsSetDefDBuffDc ct16 640x448 frame";
        Harness *hx = newHarness(t);
        if (!hx)
            return;
        hx->resetGraph(0u, 1u, 2u, 1u, 0u);
        R5900Context ctx{};
        setReg(ctx, 4, kEnv);
        setReg(ctx, 5, 2u);
        setReg(ctx, 6, 640u);
        setReg(ctx, 7, 448u);
        setReg(ctx, 8, 1u);
        setReg(ctx, 9, 0x31u);
        setReg(ctx, 10, 0u);
        setReg(ctx, 29, kStack);
        ps2_stubs::sceGsSetDefDBuffDc(hx->rdram, &ctx, hx->runtime.get());
        // draw01 at +0x60 (ZBUF_1 value +0x70), draw02 at +0xe0 (ZBUF_2 value +0xf0).
        const uint64_t z1 = hx->q(kEnv + 0x70u) & 0x1FFu;
        const uint64_t z2 = hx->q(kEnv + 0xF0u) & 0x1FFu;
        check(z1 == 140u && z2 == 140u, t + ": ZBP = sceGszbufaddr(2, 640, 448) = 140 (got " + std::to_string(z1) + ", " +
                                            std::to_string(z2) + ")");
    }
}

int main()
{
    testGuestVariableFromResetGraph();
    testGuestVariableFromGetter();
    testResetBeforeLocate();
    testBlockThenLocate();
    testJalr();
    testFallbackBlock();
    for (const ZbufCase &c : kZbufCases)
        testZbufaddr(c);
    for (const DrawCase &c : kDrawCases)
        testDrawEnv(c);
    testDBuffDcZbuf();
    std::printf("gs_gparam: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
