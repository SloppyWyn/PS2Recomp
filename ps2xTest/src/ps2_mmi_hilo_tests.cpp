// MMI moves between GPRs and the 128-bit HI/LO pair (PMFHL.*, PMTHL.LW, PMFHI, PMFLO, PMTHI,
// PMTLO): the runtime helpers against the EE Core Instruction Set Manual, and the recompiler's
// emitters (they must pass all four halves: LO = {lo1, lo}, HI = {hi1, hi}).
// Wall 21: `multu` + `pmfhl.lw` is how EE code takes a full 64-bit product; the old PMFHL.LW
// returned sign-extended LO0 in the low doubleword, so soft-float __muldf3 lost its mantissa
// product and looped forever normalising a zero.
#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"

#include <cstdint>
#include <cstring>
#include <string>

using namespace ps2recomp;

namespace
{
    struct Words
    {
        uint32_t w[4];
    };

    Words words(__m128i v)
    {
        Words out{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out.w), v);
        return out;
    }

    uint16_t half(__m128i v, int i)
    {
        uint16_t h[8];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(h), v);
        return h[i];
    }

    uint64_t dword(__m128i v, int i)
    {
        uint64_t d[2];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d), v);
        return d[i];
    }

    // The code ps2_recomp emits for `multu rs, rt` (pipeline 0).
    void multu(R5900Context &ctx, uint32_t a, uint32_t b)
    {
        const uint64_t result = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
        ctx.lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(result)));
        ctx.hi = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(result >> 32)));
    }

    Instruction makeMmi(uint32_t function, uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa)
    {
        Instruction inst{};
        inst.address = 0x1000;
        inst.opcode = OPCODE_MMI;
        inst.function = function;
        inst.rs = rs;
        inst.rt = rt;
        inst.rd = rd;
        inst.sa = sa;
        inst.raw = (OPCODE_MMI << 26) | (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
                   (static_cast<uint32_t>(rd) << 11) | (static_cast<uint32_t>(sa) << 6) | function;
        inst.isMMI = true;
        return inst;
    }

    bool has(const std::string &s, const char *needle)
    {
        return s.find(needle) != std::string::npos;
    }
}

void register_ps2_mmi_hilo_tests()
{
    MiniTest::Case("PS2MmiHiLo", [](TestCase &tc)
                   {
        tc.Run("multu + pmfhl.lw gives the full 64-bit product (Wall 21, __muldf3)", [](TestCase &t)
        {
            R5900Context ctx{};
            std::memset(&ctx, 0, sizeof(ctx));
            // __muldf3's high mantissa words for 3.984375 * 2^-32: 0x1fe00000 * 0x10000000.
            multu(ctx, 0x1fe00000u, 0x10000000u);
            const __m128i rd = PS2_PMFHL_LW(ctx.hi, ctx.lo, ctx.hi1, ctx.lo1);
            t.Equals(dword(rd, 0), 0x01FE000000000000ull, "low doubleword = HI0[31:0]:LO0[31:0]");

            multu(ctx, 0xFFFFFFFFu, 0xFFFFFFFFu);
            t.Equals(dword(PS2_PMFHL_LW(ctx.hi, ctx.lo, ctx.hi1, ctx.lo1), 0), 0xFFFFFFFE00000001ull,
                     "sign-extended LO0/HI0 halves do not leak into the product");
        });

        tc.Run("PMFHL.LW / .UW pick the low / high word of each of LO0, HI0, LO1, HI1", [](TestCase &t)
        {
            const uint64_t lo = 0x1111111122222222ull, hi = 0x3333333344444444ull;
            const uint64_t lo1 = 0x5555555566666666ull, hi1 = 0x7777777788888888ull;
            Words lw = words(PS2_PMFHL_LW(hi, lo, hi1, lo1));
            t.Equals(lw.w[0], 0x22222222u, "w0 = LO0[31:0]");
            t.Equals(lw.w[1], 0x44444444u, "w1 = HI0[31:0]");
            t.Equals(lw.w[2], 0x66666666u, "w2 = LO1[31:0]");
            t.Equals(lw.w[3], 0x88888888u, "w3 = HI1[31:0]");
            Words uw = words(PS2_PMFHL_UW(hi, lo, hi1, lo1));
            t.Equals(uw.w[0], 0x11111111u, "w0 = LO0[63:32]");
            t.Equals(uw.w[1], 0x33333333u, "w1 = HI0[63:32]");
            t.Equals(uw.w[2], 0x55555555u, "w2 = LO1[63:32]");
            t.Equals(uw.w[3], 0x77777777u, "w3 = HI1[63:32]");
        });

        tc.Run("PMFHL.SLW saturates HI:LO to a signed word, sign-extended", [](TestCase &t)
        {
            // 0x00000000:7FFFFFFF fits; 0x00000001:00000000 saturates to 0x7FFFFFFF.
            __m128i v = PS2_PMFHL_SLW(0x0ull, 0x7FFFFFFFull, 0x1ull, 0x0ull);
            t.Equals(dword(v, 0), 0x000000007FFFFFFFull, "in range");
            t.Equals(dword(v, 1), 0x000000007FFFFFFFull, "positive overflow");
            // 0xFFFFFFFF:FFFFFFFE = -2 fits; 0xFFFFFFFE:00000000 saturates to -2^31.
            v = PS2_PMFHL_SLW(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFEull, 0x0ull);
            t.Equals(dword(v, 0), 0xFFFFFFFFFFFFFFFEull, "-2 in range");
            t.Equals(dword(v, 1), 0xFFFFFFFF80000000ull, "negative overflow");
            // The upper words of LO/HI are ignored: only HI[31:0]:LO[31:0] counts.
            v = PS2_PMFHL_SLW(0xABCD000000000000ull, 0x1234000000000005ull, 0, 0);
            t.Equals(dword(v, 0), 5ull, "upper halves ignored");
        });

        tc.Run("PMFHL.LH takes halfwords 0 and 2 of each doubleword", [](TestCase &t)
        {
            const uint64_t lo = 0xA003A002A001A000ull, hi = 0xB003B002B001B000ull;
            const uint64_t lo1 = 0xC003C002C001C000ull, hi1 = 0xD003D002D001D000ull;
            const __m128i v = PS2_PMFHL_LH(hi, lo, hi1, lo1);
            const uint16_t want[8] = {0xA000, 0xA002, 0xB000, 0xB002, 0xC000, 0xC002, 0xD000, 0xD002};
            for (int i = 0; i < 8; ++i)
                t.Equals(half(v, i), want[i], "halfword order LO0, HI0, LO1, HI1");
        });

        tc.Run("PMFHL.SH saturates every word to a signed halfword", [](TestCase &t)
        {
            const uint64_t lo = (0x00008000ull << 32) | 0x00007FFFull;   // 32768 -> 7FFF, 32767
            const uint64_t hi = (0xFFFF7FFFull << 32) | 0xFFFFFFFFull;   // -32769 -> 8000, -1
            const uint64_t lo1 = (0x00000001ull << 32) | 0x80000000ull;  // 1, INT_MIN -> 8000
            const uint64_t hi1 = (0x7FFFFFFFull << 32) | 0x00000000ull;  // INT_MAX -> 7FFF, 0
            const __m128i v = PS2_PMFHL_SH(hi, lo, hi1, lo1);
            const uint16_t want[8] = {0x7FFF, 0x7FFF, 0xFFFF, 0x8000, 0x8000, 0x0001, 0x0000, 0x7FFF};
            for (int i = 0; i < 8; ++i)
                t.Equals(half(v, i), want[i], "words LO0.w0, LO0.w1, HI0.w0, HI0.w1, LO1.w0, LO1.w1, HI1.w0, HI1.w1");
        });

        tc.Run("PMTHL.LW writes the low word of each half and keeps the upper words", [](TestCase &t)
        {
            uint64_t hi = 0xAAAAAAAA00000000ull, lo = 0xBBBBBBBB00000000ull;
            uint64_t hi1 = 0xCCCCCCCC00000000ull, lo1 = 0xDDDDDDDD00000000ull;
            ps2_pmthl_lw(_mm_set_epi32(0x44444444, 0x33333333, 0x22222222, 0x11111111), hi, lo, hi1, lo1);
            t.Equals(lo, 0xBBBBBBBB11111111ull, "LO0[31:0] = w0");
            t.Equals(hi, 0xAAAAAAAA22222222ull, "HI0[31:0] = w1");
            t.Equals(lo1, 0xDDDDDDDD33333333ull, "LO1[31:0] = w2");
            t.Equals(hi1, 0xCCCCCCCC44444444ull, "HI1[31:0] = w3");
            // Round trip: PMTHL.LW then PMFHL.LW gives the register back.
            const __m128i back = PS2_PMFHL_LW(hi, lo, hi1, lo1);
            t.Equals(words(back).w[3], 0x44444444u, "round trip");
        });

        tc.Run("emitters pass all four HI/LO halves", [](TestCase &t)
        {
            CodeGenerator gen({}, {});
            const std::string lw = gen.translateInstruction(makeMmi(MMI_PMFHL, 0, 0, 1, PMFHL_LW));
            t.IsTrue(has(lw, "PS2_PMFHL_LW(ctx->hi, ctx->lo, ctx->hi1, ctx->lo1)"), "PMFHL.LW reads HI1/LO1 too: " + lw);
            const std::string sh = gen.translateInstruction(makeMmi(MMI_PMFHL, 0, 0, 2, PMFHL_SH));
            t.IsTrue(has(sh, "PS2_PMFHL_SH(ctx->hi, ctx->lo, ctx->hi1, ctx->lo1)"), "PMFHL.SH: " + sh);
            const std::string thl = gen.translateInstruction(makeMmi(MMI_PMTHL, 3, 0, 0, PMFHL_LW));
            t.IsTrue(has(thl, "ps2_pmthl_lw(GPR_VEC(ctx, 3), ctx->hi, ctx->lo, ctx->hi1, ctx->lo1)"), "PMTHL.LW: " + thl);
            const std::string fhi = gen.translateInstruction(makeMmi(MMI_MMI2, 0, 0, 4, MMI2_PMFHI));
            t.IsTrue(has(fhi, "ctx->hi1") && has(fhi, "SET_GPR_VEC(ctx, 4"), "PMFHI moves all 128 bits: " + fhi);
            const std::string flo = gen.translateInstruction(makeMmi(MMI_MMI2, 0, 0, 5, MMI2_PMFLO));
            t.IsTrue(has(flo, "ctx->lo1") && has(flo, "SET_GPR_VEC(ctx, 5"), "PMFLO moves all 128 bits: " + flo);
            const std::string thi = gen.translateInstruction(makeMmi(MMI_MMI3, 6, 0, 0, MMI3_PMTHI));
            t.IsTrue(has(thi, "ctx->hi1 =") && has(thi, "GPR_VEC(ctx, 6)"), "PMTHI sets HI1 too: " + thi);
            const std::string tlo = gen.translateInstruction(makeMmi(MMI_MMI3, 7, 0, 0, MMI3_PMTLO));
            t.IsTrue(has(tlo, "ctx->lo1 =") && has(tlo, "GPR_VEC(ctx, 7)"), "PMTLO sets LO1 too: " + tlo);
        });

        tc.Run("PMFHI/PMFLO/PMTHI/PMTLO emitted code round-trips 128 bits", [](TestCase &t)
        {
            // The same expressions the emitters produce, run on a context.
            R5900Context c{};
            std::memset(&c, 0, sizeof(c));
            R5900Context *ctx = &c;
            ctx->r[6] = _mm_set_epi64x(0x0123456789ABCDEFll, 0x7EDCBA9876543210ll);
            { const __m128i v = GPR_VEC(ctx, 6); ctx->hi = (uint64_t)PS2_EXTRACT_EPI64_0(v); ctx->hi1 = (uint64_t)PS2_EXTRACT_EPI64_1(v); }
            SET_GPR_VEC(ctx, 4, _mm_set_epi64x((int64_t)ctx->hi1, (int64_t)ctx->hi));
            t.Equals(dword(ctx->r[4], 0), 0x7EDCBA9876543210ull, "HI0 back");
            t.Equals(dword(ctx->r[4], 1), 0x0123456789ABCDEFull, "HI1 back");
        }); });
}
