// COP2 macro-mode (VU0) semantics: the runtime helpers in ps2_vu0_macro.h that recompiled COP2
// instructions call, and the recompiler's emitters that pick them (VF0/VI0 writes dropped).
#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

using namespace ps2recomp;

namespace
{
    constexpr uint32_t kMaxBits = 0x7F7FFFFFu;

    uint32_t bitsOf(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    float floatOf(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }

    uint32_t laneBits(__m128 v, int lane)
    {
        alignas(16) uint32_t w[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(w), _mm_castps_si128(v));
        return w[lane];
    }

    float laneF(__m128 v, int lane)
    {
        return floatOf(laneBits(v, lane));
    }

    __m128 vec(float x, float y, float z, float w)
    {
        return _mm_set_ps(w, z, y, x);
    }

    struct Ctx
    {
        R5900Context ctx{};
        Ctx()
        {
            std::memset(&ctx, 0, sizeof(ctx));
            ctx.vu0_vf[0] = vec(0.0f, 0.0f, 0.0f, 1.0f);
        }
    };

    bool isVf0(__m128 v)
    {
        return laneBits(v, 0) == 0u && laneBits(v, 1) == 0u && laneBits(v, 2) == 0u && laneBits(v, 3) == 0x3F800000u;
    }

    Instruction cop2(uint8_t fd, uint8_t fs, uint8_t ft, uint8_t dest)
    {
        Instruction inst{};
        inst.opcode = OPCODE_COP2;
        inst.rs = COP2_CO | dest;
        inst.sa = fd;
        inst.rd = fs;
        inst.rt = ft;
        inst.vectorInfo.vectorField = dest;
        return inst;
    }
}

void register_ps2_vu0_macro_tests()
{
    MiniTest::Case("PS2VU0Macro", [](TestCase &tc)
    {
        tc.Run("operands: exponent 255 reads as +-MAX, exponent 0 as +-0", [](TestCase &t)
        {
            const __m128 in = _mm_castsi128_ps(_mm_set_epi32(static_cast<int>(0x80000001u), 0x00400000, static_cast<int>(0xFFC00000u), 0x7F800000));
            const __m128 out = ps2x_vu0::operand(in);
            t.Equals(laneBits(out, 0), kMaxBits, "+Inf bits read as +MAX");
            t.Equals(laneBits(out, 1), 0x80000000u | kMaxBits, "the host's default NaN (0xffc00000) reads as -MAX");
            t.Equals(laneBits(out, 2), 0u, "a positive denormal reads as +0");
            t.Equals(laneBits(out, 3), 0x80000000u, "a negative denormal reads as -0");
        });

        tc.Run("writes to VF0 are dropped but still set the flags", [](TestCase &t)
        {
            Ctx c;
            // The HiG RotTransPers clip idiom: ctc2 zero, STATUS; vsub.xyw vf0, vf12, vf0; vsub.xy vf0, vf29, vf12;
            // cfc2 STATUS & 0xc0 (ZS|SS) means the vertex is outside.
            c.ctx.vu0_vf[12] = vec(100.0f, -5.0f, 0.5f, 1.0f);
            c.ctx.vu0_vf[29] = vec(4096.0f, 4096.0f, 0.0f, 0.0f);
            ps2x_vu0::writeStatus(&c.ctx, 0u);
            ps2x_vu0::sub(&c.ctx, nullptr, c.ctx.vu0_vf[12], c.ctx.vu0_vf[0], 0xDu);
            ps2x_vu0::sub(&c.ctx, nullptr, c.ctx.vu0_vf[29], c.ctx.vu0_vf[12], 0xCu);
            t.IsTrue(isVf0(c.ctx.vu0_vf[0]), "VF0 must stay (0,0,0,1)");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status & 0xC0u), 0xC0u, "y < 0 sets sticky S, w-1 == 0 sets sticky Z");

            Ctx inside;
            inside.ctx.vu0_vf[12] = vec(100.0f, 200.0f, 0.5f, 2.0f);
            inside.ctx.vu0_vf[29] = vec(4096.0f, 4096.0f, 0.0f, 0.0f);
            ps2x_vu0::writeStatus(&inside.ctx, 0u);
            ps2x_vu0::sub(&inside.ctx, nullptr, inside.ctx.vu0_vf[12], inside.ctx.vu0_vf[0], 0xDu);
            ps2x_vu0::sub(&inside.ctx, nullptr, inside.ctx.vu0_vf[29], inside.ctx.vu0_vf[12], 0xCu);
            t.Equals(static_cast<uint32_t>(inside.ctx.vu0_status & 0xC0u), 0u, "a vertex on screen sets neither ZS nor SS");
            t.IsTrue(isVf0(inside.ctx.vu0_vf[0]), "VF0 must stay (0,0,0,1)");
        });

        tc.Run("MAC flags: field order, dest mask, Z and S", [](TestCase &t)
        {
            Ctx c;
            __m128 dst = vec(9.0f, 9.0f, 9.0f, 9.0f);
            ps2x_vu0::sub(&c.ctx, &dst, vec(1.0f, 2.0f, -3.0f, 4.0f), vec(1.0f, 0.0f, 0.0f, 0.0f), 0xEu);
            // x = 0 (Z, field bit 3), y = 2, z = -3 (S, field bit 1), w not written.
            t.Equals(c.ctx.vu0_mac_flags, 0x0028u, "MAC Z.x (bit 3) and S.z (bit 5)");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status), 0x00C3u, "status Z, S and their sticky bits");
            t.IsTrue(laneF(dst, 0) == 0.0f && laneF(dst, 1) == 2.0f && laneF(dst, 2) == -3.0f && laneF(dst, 3) == 9.0f,
                     "dest .xyz written, .w kept");

            ps2x_vu0::add(&c.ctx, &dst, vec(1.0f, 1.0f, 1.0f, 1.0f), vec(1.0f, 1.0f, 1.0f, 1.0f), 0x1u);
            t.Equals(c.ctx.vu0_mac_flags, 0u, "a positive non-zero result clears MAC");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status), 0x00C0u, "current Z/S cleared, sticky kept");

            const uint32_t before = c.ctx.vu0_mac_flags;
            ps2x_vu0::add(&c.ctx, &dst, vec(0, 0, 0, 0), vec(0, 0, 0, 0), 0x0u);
            t.Equals(c.ctx.vu0_mac_flags, before, "dest 0 leaves the flags alone");
        });

        tc.Run("overflow clamps to +-MAX with O, underflow flushes to 0 with U and Z", [](TestCase &t)
        {
            Ctx c;
            __m128 dst = _mm_setzero_ps();
            const float big = std::numeric_limits<float>::max();
            ps2x_vu0::mul(&c.ctx, &dst, vec(big, -big, 1e-30f, 2.0f), vec(2.0f, 2.0f, 1e-30f, 3.0f), 0xFu);
            t.Equals(laneBits(dst, 0), kMaxBits, "MAX*2 = +MAX");
            t.Equals(laneBits(dst, 1), 0x80000000u | kMaxBits, "-MAX*2 = -MAX");
            t.Equals(laneBits(dst, 2), 0u, "1e-30*1e-30 flushes to +0");
            t.IsTrue(laneF(dst, 3) == 6.0f, "ordinary lane");
            // O: x,y (bits 15,14); S: y (bit 6); U: z (bit 9); Z: z (bit 1).
            t.Equals(c.ctx.vu0_mac_flags, 0xC242u, "MAC O.xy, U.z, S.y, Z.z");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status & 0x3CFu), 0x3CFu, "status Z S U O and sticky ZS SS US OS");
        });

        tc.Run("Inf and NaN never appear: MAX*0 = 0, MAX-MAX = 0", [](TestCase &t)
        {
            Ctx c;
            __m128 dst = _mm_setzero_ps();
            const __m128 inf = _mm_castsi128_ps(_mm_set1_epi32(0x7F800000));
            ps2x_vu0::mul(&c.ctx, &dst, inf, _mm_setzero_ps(), 0xFu);
            t.Equals(laneBits(dst, 0), 0u, "Inf operand (MAX) * 0 = 0, not NaN");
            ps2x_vu0::sub(&c.ctx, &dst, inf, inf, 0xFu);
            t.Equals(laneBits(dst, 1), 0u, "MAX - MAX = 0");
            ps2x_vu0::madd(&c.ctx, &dst, inf, inf, vec(2, 2, 2, 2), 0xFu);
            t.Equals(laneBits(dst, 2), kMaxBits, "MAX + MAX*2 clamps to MAX");
        });

        tc.Run("MADD/MSUB read ACC; ACC as destination", [](TestCase &t)
        {
            Ctx c;
            c.ctx.vu0_acc = vec(1, 2, 3, 4);
            ps2x_vu0::madd(&c.ctx, &c.ctx.vu0_acc, c.ctx.vu0_acc, vec(2, 2, 2, 2), vec(3, 3, 3, 3), 0xFu);
            t.IsTrue(laneF(c.ctx.vu0_acc, 0) == 7.0f && laneF(c.ctx.vu0_acc, 3) == 10.0f, "ACC += 2*3");
            __m128 dst = _mm_setzero_ps();
            ps2x_vu0::msub(&c.ctx, &dst, c.ctx.vu0_acc, vec(1, 1, 1, 1), vec(7, 8, 9, 10), 0xFu);
            t.IsTrue(laneF(dst, 0) == 0.0f && laneF(dst, 1) == 0.0f && laneF(dst, 3) == 0.0f, "fd = ACC - fs*ft");
            t.Equals(c.ctx.vu0_mac_flags & 0xFu, 0xFu, "zero results set Z in every field");
        });

        tc.Run("OPMULA/OPMSUB give the cross product", [](TestCase &t)
        {
            Ctx c;
            const __m128 a = vec(1, 0, 0, 0), b = vec(0, 1, 0, 0);
            // vopmula.xyz ACC, a, b ; vopmsub.xyz d, b, a  => d = a x b
            ps2x_vu0::mul(&c.ctx, &c.ctx.vu0_acc, ps2x_vu0::yzx(a), ps2x_vu0::zxy(b), 0xEu);
            __m128 d = _mm_setzero_ps();
            ps2x_vu0::msub(&c.ctx, &d, c.ctx.vu0_acc, ps2x_vu0::yzx(b), ps2x_vu0::zxy(a), 0xEu);
            t.IsTrue(laneF(d, 0) == 0.0f && laneF(d, 1) == 0.0f && laneF(d, 2) == 1.0f, "x cross y = z");
        });

        tc.Run("DIV: x/0 gives +-MAX and D, 0/0 gives I; flags replace, sticky accumulate", [](TestCase &t)
        {
            Ctx c;
            ps2x_vu0::vdiv(&c.ctx, 1.0f, 0.0f);
            t.Equals(bitsOf(c.ctx.vu0_q), kMaxBits, "1/0 = +MAX");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status & 0xC30u), 0x820u, "D and sticky DS");
            ps2x_vu0::vdiv(&c.ctx, -1.0f, 0.0f);
            t.Equals(bitsOf(c.ctx.vu0_q), 0x80000000u | kMaxBits, "-1/0 = -MAX");
            ps2x_vu0::vdiv(&c.ctx, 1.0f, -0.0f);
            t.Equals(bitsOf(c.ctx.vu0_q), 0x80000000u | kMaxBits, "1/-0 = -MAX");
            ps2x_vu0::vdiv(&c.ctx, 0.0f, 0.0f);
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status & 0xC30u), 0xC10u, "0/0: I set, D cleared, DS and IS sticky");
            ps2x_vu0::vdiv(&c.ctx, 6.0f, 3.0f);
            t.IsTrue(c.ctx.vu0_q == 2.0f, "6/3");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status & 0xC30u), 0xC00u, "a good divide clears I and D, keeps IS/DS");
            ps2x_vu0::vdiv(&c.ctx, floatOf(0x00000001u), 2.0f);
            t.Equals(bitsOf(c.ctx.vu0_q), 0u, "a denormal numerator reads as 0");
        });

        tc.Run("SQRT and RSQRT use |ft|, flag negatives with I, RSQRT uses fs", [](TestCase &t)
        {
            Ctx c;
            ps2x_vu0::vsqrt(&c.ctx, -4.0f);
            t.IsTrue(c.ctx.vu0_q == 2.0f, "sqrt(|-4|) = 2");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status & 0x30u), 0x10u, "negative radicand sets I");
            ps2x_vu0::vrsqrt(&c.ctx, 3.0f, 4.0f);
            t.IsTrue(c.ctx.vu0_q == 1.5f, "rsqrt = fs / sqrt(ft) = 3/2");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status & 0x30u), 0u, "good rsqrt clears I/D");
            ps2x_vu0::vrsqrt(&c.ctx, -3.0f, 0.0f);
            t.Equals(bitsOf(c.ctx.vu0_q), 0x80000000u | kMaxBits, "-3/sqrt(0) = -MAX");
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status & 0x30u), 0x20u, "rsqrt by zero sets D");
        });

        tc.Run("FTOI saturates, ITOF scales", [](TestCase &t)
        {
            __m128 dst = _mm_setzero_ps();
            ps2x_vu0::ftoi(&dst, vec(3e9f, -3e9f, 1.5f, -1.75f), 0u, 0xFu);
            t.Equals(laneBits(dst, 0), 0x7FFFFFFFu, "FTOI0(3e9) saturates to INT32_MAX");
            t.Equals(laneBits(dst, 1), 0x80000000u, "FTOI0(-3e9) saturates to INT32_MIN");
            t.Equals(laneBits(dst, 2), 1u, "FTOI0 truncates 1.5");
            t.Equals(laneBits(dst, 3), static_cast<uint32_t>(-1), "FTOI0 truncates -1.75 toward 0");
            ps2x_vu0::ftoi(&dst, vec(1.5f, 0, 0, 0), 4u, 0x8u);
            t.Equals(laneBits(dst, 0), 24u, "FTOI4(1.5) = 24");
            ps2x_vu0::ftoi(&dst, _mm_castsi128_ps(_mm_set1_epi32(0x7F800000)), 0u, 0x8u);
            t.Equals(laneBits(dst, 0), 0x7FFFFFFFu, "FTOI0(Inf bits = MAX) saturates");
            ps2x_vu0::itof(&dst, _mm_castsi128_ps(_mm_set_epi32(0, 0, -16, 24)), 4u, 0xCu);
            t.IsTrue(laneF(dst, 0) == 1.5f && laneF(dst, 1) == -1.0f, "ITOF4");
            ps2x_vu0::itof(nullptr, dst, 0u, 0xFu); // VF0 destination: nothing to crash on
        });

        tc.Run("CLIP: +x/-x/+y/-y/+z/-z against |w|, 24-bit history", [](TestCase &t)
        {
            Ctx c;
            c.ctx.vu0_clip_flags = 0x3Fu;
            ps2x_vu0::clip(&c.ctx, vec(2.0f, -2.0f, 0.5f, 0.0f), -1.0f);
            t.Equals(c.ctx.vu0_clip_flags, (0x3Fu << 6) | 0x09u, "+x (bit 0) and -y (bit 3) with |w| = 1, history shifted");
            ps2x_vu0::clip(&c.ctx, vec(0, 0, -3.0f, 0), 2.0f);
            t.Equals(c.ctx.vu0_clip_flags & 0x3Fu, 0x20u, "-z is bit 5");
            for (int i = 0; i < 4; ++i)
                ps2x_vu0::clip(&c.ctx, vec(0, 0, 0, 0), 1.0f);
            t.Equals(c.ctx.vu0_clip_flags & ~0xFFFFFFu, 0u, "only 24 bits kept");
        });

        tc.Run("CTC2 STATUS writes only the sticky bits", [](TestCase &t)
        {
            Ctx c;
            c.ctx.vu0_status = 0x0013u;
            ps2x_vu0::writeStatus(&c.ctx, 0xFFFFu);
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status), 0x0FD3u, "bits 6-11 written, 0-5 kept");
            ps2x_vu0::writeStatus(&c.ctx, 0u);
            t.Equals(static_cast<uint32_t>(c.ctx.vu0_status), 0x0013u, "clearing the sticky bits");
        });

        tc.Run("R register: RINIT/RXOR/RNEXT/RGET keep exponent 0x7F and step the LFSR", [](TestCase &t)
        {
            Ctx c;
            ps2x_vu0::rinit(&c.ctx, floatOf(0x12345678u));
            t.Equals(ps2x_vu0::rValue(&c.ctx), 0x3F800000u | (0x12345678u & 0x7FFFFFu), "RINIT keeps the 23 mantissa bits");
            ps2x_vu0::rxor(&c.ctx, floatOf(0x00000001u));
            t.Equals(ps2x_vu0::rValue(&c.ctx), 0x3F800000u | ((0x12345678u ^ 1u) & 0x7FFFFFu), "RXOR");
            const uint32_t r = ps2x_vu0::rValue(&c.ctx);
            __m128 dst = _mm_setzero_ps();
            ps2x_vu0::rnext(&c.ctx, &dst, 0x8u);
            const uint32_t expected = 0x3F800000u | (((r << 1) ^ ((r >> 4) & 1u) ^ ((r >> 22) & 1u)) & 0x7FFFFFu);
            t.Equals(ps2x_vu0::rValue(&c.ctx), expected, "RNEXT steps the LFSR (taps 4 and 22)");
            t.Equals(laneBits(dst, 0), expected, "RNEXT writes R to the dest fields");
            t.Equals(laneBits(dst, 1), 0u, "fields outside dest untouched");
            ps2x_vu0::rget(&c.ctx, &dst, 0x1u);
            t.Equals(laneBits(dst, 3), expected, "RGET");
        });

        tc.Run("emitters drop VF0 and VI0 writes", [](TestCase &t)
        {
            CodeGenerator gen({}, {});
            Instruction add = cop2(0, 12, 0, 0xD);
            add.function = VU0_S1_VSUB;
            const std::string sub = gen.translateInstruction(add);
            t.IsTrue(sub.find("ps2x_vu0::sub(ctx, nullptr,") != std::string::npos, "vsub to VF0 computes flags only: " + sub);
            t.IsTrue(sub.find("ctx->vu0_vf[0] =") == std::string::npos, "no assignment to VF0");

            for (const std::string &code : {gen.translateVU_VMADD(cop2(0, 1, 2, 0xF)), gen.translateVU_VOPMSUB(cop2(0, 1, 2, 0xE)),
                                            gen.translateVU_VMULq(cop2(0, 1, 2, 0xF)), gen.translateVU_VADD_Field(cop2(0, 1, 2, 0xF)),
                                            gen.translateVU_VMAX(cop2(0, 1, 2, 0xF)), gen.translateVU_VMINIi(cop2(0, 1, 2, 0xF))})
            {
                t.IsTrue(code.find("ctx->vu0_vf[0] =") == std::string::npos && code.find("&ctx->vu0_vf[0]") == std::string::npos,
                         "FMAC/MAX/MINI to VF0 dropped: " + code);
            }

            Instruction s2 = cop2(0, 5, 0, 0xF);
            for (const std::string &code : {gen.translateVU_VITOF(s2, 4), gen.translateVU_VFTOI(s2, 0), gen.translateVU_VMFIR(s2),
                                            gen.translateVU_VLQI(s2), gen.translateVU_VRGET(s2), gen.translateVU_VRNEXT(s2)})
            {
                t.IsTrue(code.find("&ctx->vu0_vf[0]") == std::string::npos && code.find("ctx->vu0_vf[0] =") == std::string::npos,
                         "S2 op to VF0 dropped: " + code);
            }

            Instruction viDst{};
            viDst.sa = 0;
            viDst.rt = 0;
            viDst.rd = 3;
            for (const std::string &code : {gen.translateVU_VIADD(viDst), gen.translateVU_VISUB(viDst), gen.translateVU_VIAND(viDst),
                                            gen.translateVU_VIOR(viDst), gen.translateVU_VIADDI(viDst), gen.translateVU_VMTIR(viDst),
                                            gen.translateVU_VILWR(viDst)})
            {
                t.IsTrue(code.find("ctx->vi[0] =") == std::string::npos, "VI0 write dropped: " + code);
            }

            Instruction lqi = cop2(0, 0, 4, 0xF); // vlqi vf4, (vi0++)
            lqi.rd = 0;
            const std::string lqiCode = gen.translateVU_VLQI(lqi);
            t.IsTrue(lqiCode.find("ctx->vi[0] =") == std::string::npos, "VI0 post-increment dropped: " + lqiCode);
            t.IsTrue(lqiCode.find("0x11004000u") != std::string::npos, "VLQI reads VU0 data memory: " + lqiCode);

            Instruction lqc2{};
            lqc2.opcode = OPCODE_LQC2;
            lqc2.rt = 0;
            lqc2.rs = 4;
            const std::string lqc2Code = gen.translateInstruction(lqc2);
            t.IsTrue(lqc2Code.find("ctx->vu0_vf[0] =") == std::string::npos, "LQC2 into VF0 dropped: " + lqc2Code);

            Instruction qmtc2{};
            qmtc2.opcode = OPCODE_COP2;
            qmtc2.rs = COP2_QMTC2;
            qmtc2.rt = 8;
            qmtc2.rd = 0;
            const std::string qmtc2Code = gen.translateInstruction(qmtc2);
            t.IsTrue(qmtc2Code.find("ctx->vu0_vf[0] =") == std::string::npos, "QMTC2 into VF0 dropped: " + qmtc2Code);

            Instruction ctc2{};
            ctc2.opcode = OPCODE_COP2;
            ctc2.rs = COP2_CTC2;
            ctc2.rt = 0;
            ctc2.rd = VU0_CR_STATUS;
            t.IsTrue(gen.translateInstruction(ctc2).find("ps2x_vu0::writeStatus") != std::string::npos, "CTC2 STATUS keeps bits 0-5");
        });

        tc.Run("emitters: DIV/RSQRT fields, bc broadcast, ACC forms, CLIP", [](TestCase &t)
        {
            CodeGenerator gen({}, {});
            Instruction div{};
            div.rd = 7;
            div.rt = 9;
            div.vectorInfo.fsf = 3;
            div.vectorInfo.ftf = 1;
            const std::string d = gen.translateVU_VDIV(div);
            t.IsTrue(d.find("ps2x_vu0::vdiv(ctx, ps2x_vu0::lane(ctx->vu0_vf[7], 3u), ps2x_vu0::lane(ctx->vu0_vf[9], 1u))") != std::string::npos, d);
            const std::string r = gen.translateVU_VRSQRT(div);
            t.IsTrue(r.find("ps2x_vu0::vrsqrt(ctx, ps2x_vu0::lane(ctx->vu0_vf[7], 3u)") != std::string::npos, "RSQRT reads fs: " + r);

            Instruction madday = cop2(0, 5, 8, 0xF);
            madday.function = 1; // bc = y
            const std::string ma = gen.translateVU_VMADDA_Field(madday);
            t.IsTrue(ma.find("ps2x_vu0::madd(ctx, &ctx->vu0_acc, ctx->vu0_acc, ctx->vu0_vf[5], ps2x_vu0::broadcast(ctx->vu0_vf[8], 1u), 0xFu)") != std::string::npos, ma);

            Instruction clip{};
            clip.opcode = OPCODE_COP2;
            clip.rs = COP2_CO | 0xE;
            clip.rd = 13;
            clip.rt = 8;
            clip.function = 0x3F; // Special2, bc w
            clip.raw = (((VU0_S2_VCLIPw >> 2) & 0x1Fu) << 6) | (VU0_S2_VCLIPw & 0x3u);
            const std::string cl = gen.translateInstruction(clip);
            t.IsTrue(cl.find("ps2x_vu0::clip(ctx, ctx->vu0_vf[13], ps2x_vu0::lane(ctx->vu0_vf[8], 3u))") != std::string::npos, cl);
        });
    });
}
