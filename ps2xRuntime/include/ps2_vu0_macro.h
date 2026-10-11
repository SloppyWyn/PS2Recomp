#ifndef PS2_VU0_MACRO_H
#define PS2_VU0_MACRO_H
// COP2 macro-mode (VU0) semantics for recompiled EE code. The recompiler emits calls to these
// helpers for every COP2 arithmetic instruction (vu_translation_helpers.cpp), so they are the one
// place that decides how a VU0 macro instruction behaves on the host. They follow the VU's own
// rules, the same ones the runtime's VU interpreter (src/lib/vu) applies to micro mode:
//   - VF0 is hardwired to (0, 0, 0, 1) and VI0 to 0: writes to them are dropped (games write
//     VF0 on purpose to set flags without keeping the result, e.g. "vsub.xyw vf0, vf12, vf0").
//   - VU floats have no Inf, NaN or denormals: an operand with exponent 0 is +-0, one with
//     exponent 255 is a number (taken as +-MAX here); a result that overflows becomes +-MAX
//     (O flag), one that underflows becomes +-0 (U and Z flags).
//   - Every FMAC operation (ADD/SUB/MUL/MADD/MSUB and their ACC, bc, q, i forms, OPMULA/OPMSUB)
//     sets the MAC flags (Z bits 0-3, S 4-7, U 8-11, O 12-15; x is the high bit of each nibble,
//     fields outside the dest mask read 0) and the status flags Z S U O (bits 0-3, the OR over
//     the written fields) plus their sticky copies (bits 6-9). dest == 0 leaves the flags alone.
//   - DIV/SQRT/RSQRT set I (bit 4) and D (bit 5) plus sticky IS/DS (bits 10/11): x/0 sets D and
//     0/0 sets I, both giving +-MAX in Q; SQRT and RSQRT of a negative number set I and use |x|.
//   - FTOI saturates to INT32_MIN/MAX; CLIP compares against |w| and shifts the 24-bit clip flags.
//   - CTC2 to STATUS writes only the sticky bits (6-11); bits 0-5 are read only.
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>

#include "ps2_runtime.h"

namespace ps2x_vu0
{
    constexpr uint32_t kMax = 0x7F7FFFFFu;

    // COP2 dest field (x = 8, y = 4, z = 2, w = 1) as an all-ones lane mask.
    inline __m128 laneMask(uint32_t dest)
    {
        return _mm_castsi128_ps(_mm_set_epi32((dest & 1u) ? -1 : 0, (dest & 2u) ? -1 : 0,
                                              (dest & 4u) ? -1 : 0, (dest & 8u) ? -1 : 0));
    }

    // Lanes of value where dest is set, old elsewhere.
    inline __m128 select(__m128 old, __m128 value, uint32_t dest)
    {
        const __m128 mask = laneMask(dest);
        return _mm_or_ps(_mm_and_ps(mask, value), _mm_andnot_ps(mask, old));
    }

    // Writes value into dst under the dest mask; dst == nullptr drops the write (VF0).
    inline void store(__m128 *dst, __m128 value, uint32_t dest)
    {
        if (dst)
            *dst = select(*dst, value, dest);
    }

    // _mm_movemask_ps order (bit 0 = x) to the COP2 field order (x = bit 3).
    inline uint32_t fieldBits(uint32_t laneBits)
    {
        return ((laneBits & 1u) << 3) | ((laneBits & 2u) << 1) | ((laneBits & 4u) >> 1) | ((laneBits & 8u) >> 3);
    }

    // An operand as the VU reads it: exponent 0 -> +-0, exponent 255 -> +-MAX.
    inline __m128 operand(__m128 v)
    {
        const __m128i bits = _mm_castps_si128(v);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i exp = _mm_and_si128(bits, expMask);
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i isZeroExp = _mm_cmpeq_epi32(exp, _mm_setzero_si128());
        const __m128i isMaxExp = _mm_cmpeq_epi32(exp, expMask);
        const __m128i special = _mm_or_si128(isZeroExp, isMaxExp);
        __m128i out = _mm_andnot_si128(special, bits);
        out = _mm_or_si128(out, _mm_and_si128(isZeroExp, sign));
        out = _mm_or_si128(out, _mm_and_si128(isMaxExp, _mm_or_si128(sign, _mm_set1_epi32(static_cast<int>(kMax)))));
        return _mm_castsi128_ps(out);
    }

    inline __m128 operandScalar(float value)
    {
        return operand(_mm_set1_ps(value));
    }

    // Lane broadcast of an operand (the bc field: 0 = x .. 3 = w).
    inline __m128 broadcast(__m128 v, uint32_t field)
    {
        switch (field & 3u)
        {
        case 0:
            return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
        case 1:
            return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
        case 2:
            return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
        default:
            return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
        }
    }

    // A host result as the VU would produce it, and its MAC flags (all four fields).
    // lostUnderflow marks lanes whose exact result was non-zero although the host result is 0
    // (a product too small even for a host denormal): they flag U like any underflow.
    inline __m128 normalizeResult(__m128 v, uint32_t &mac, __m128i lostUnderflow = _mm_setzero_si128())
    {
        const __m128i bits = _mm_castps_si128(v);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i exp = _mm_and_si128(bits, expMask);
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i magnitudeZero = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128());
        const __m128i zeroExp = _mm_cmpeq_epi32(exp, _mm_setzero_si128());
        const __m128i underflow = _mm_or_si128(_mm_andnot_si128(magnitudeZero, zeroExp),
                                               _mm_and_si128(magnitudeZero, lostUnderflow));
        const __m128i overflow = _mm_cmpeq_epi32(exp, expMask);

        const uint32_t z = fieldBits(static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(zeroExp))));
        const uint32_t s = fieldBits(static_cast<uint32_t>(_mm_movemask_ps(v)));
        const uint32_t u = fieldBits(static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(underflow))));
        const uint32_t o = fieldBits(static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(overflow))));
        mac = z | (s << 4) | (u << 8) | (o << 12);

        __m128i out = _mm_andnot_si128(_mm_or_si128(zeroExp, overflow), bits);
        out = _mm_or_si128(out, _mm_and_si128(zeroExp, sign));
        out = _mm_or_si128(out, _mm_and_si128(overflow, _mm_or_si128(sign, _mm_set1_epi32(static_cast<int>(kMax)))));
        return _mm_castsi128_ps(out);
    }

    // Product inside MADD/MSUB: clamped like any result, flags not reported.
    inline __m128 product(__m128 a, __m128 b)
    {
        uint32_t ignored = 0u;
        return normalizeResult(_mm_mul_ps(operand(a), operand(b)), ignored);
    }

    // Records the flags of an FMAC operation that wrote the dest fields.
    inline void setFmacFlags(R5900Context *ctx, uint32_t mac, uint32_t dest)
    {
        dest &= 0xFu;
        if (dest == 0u)
            return;
        mac &= dest * 0x1111u;
        uint32_t current = 0u;
        if (mac & 0x000Fu)
            current |= 0x1u;
        if (mac & 0x00F0u)
            current |= 0x2u;
        if (mac & 0x0F00u)
            current |= 0x4u;
        if (mac & 0xF000u)
            current |= 0x8u;
        ctx->vu0_mac_flags = mac;
        ctx->vu0_status = static_cast<uint16_t>((ctx->vu0_status & 0xFF0u) | current | (current << 6));
    }

    // Finishes an FMAC operation: normalise the host result, set MAC/status, write dst under
    // dest (dst == nullptr: VF0, flags only).
    inline void fmac(R5900Context *ctx, __m128 *dst, __m128 raw, uint32_t dest,
                     __m128i lostUnderflow = _mm_setzero_si128())
    {
        uint32_t mac = 0u;
        const __m128 value = normalizeResult(raw, mac, lostUnderflow);
        setFmacFlags(ctx, mac, dest);
        store(dst, value, dest);
    }

    // fd = fs (op) ft, with operands read the VU way.
    inline void add(R5900Context *ctx, __m128 *dst, __m128 a, __m128 b, uint32_t dest)
    {
        fmac(ctx, dst, _mm_add_ps(operand(a), operand(b)), dest);
    }

    inline void sub(R5900Context *ctx, __m128 *dst, __m128 a, __m128 b, uint32_t dest)
    {
        fmac(ctx, dst, _mm_sub_ps(operand(a), operand(b)), dest);
    }

    // Lanes where both factors are non-zero: their product is never exactly zero.
    inline __m128i nonZeroFactors(__m128 a, __m128 b)
    {
        const __m128i magnitude = _mm_set1_epi32(0x7FFFFFFF);
        const __m128i zero = _mm_setzero_si128();
        const __m128i aZero = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(a), magnitude), zero);
        const __m128i bZero = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(b), magnitude), zero);
        return _mm_andnot_si128(_mm_or_si128(aZero, bZero), _mm_set1_epi32(-1));
    }

    inline void mul(R5900Context *ctx, __m128 *dst, __m128 a, __m128 b, uint32_t dest)
    {
        const __m128 x = operand(a);
        const __m128 y = operand(b);
        fmac(ctx, dst, _mm_mul_ps(x, y), dest, nonZeroFactors(x, y));
    }

    // fd = ACC + a*b / ACC - a*b. acc is read before dst is written (dst may be &ctx->vu0_acc).
    inline void madd(R5900Context *ctx, __m128 *dst, __m128 acc, __m128 a, __m128 b, uint32_t dest)
    {
        fmac(ctx, dst, _mm_add_ps(operand(acc), product(a, b)), dest);
    }

    inline void msub(R5900Context *ctx, __m128 *dst, __m128 acc, __m128 a, __m128 b, uint32_t dest)
    {
        fmac(ctx, dst, _mm_sub_ps(operand(acc), product(a, b)), dest);
    }

    // Outer-product helpers: fs.yzx and ft.zxy (w lane unchanged; OPMULA/OPMSUB use dest xyz).
    inline __m128 yzx(__m128 v)
    {
        return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 2, 1));
    }

    inline __m128 zxy(__m128 v)
    {
        return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 0, 2));
    }

    // MAX / MINI: no flags.
    inline void vmax(__m128 *dst, __m128 a, __m128 b, uint32_t dest)
    {
        store(dst, _mm_max_ps(operand(a), operand(b)), dest);
    }

    inline void vmini(__m128 *dst, __m128 a, __m128 b, uint32_t dest)
    {
        store(dst, _mm_min_ps(operand(a), operand(b)), dest);
    }

    inline int32_t floatToInt(float value, double scale)
    {
        const double scaled = static_cast<double>(value) * scale;
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    // FTOIn: truncating, saturating conversion with n fraction bits.
    inline void ftoi(__m128 *dst, __m128 src, uint32_t fractionBits, uint32_t dest)
    {
        alignas(16) float in[4];
        _mm_store_ps(in, operand(src));
        const double scale = static_cast<double>(1u << fractionBits);
        const __m128i out = _mm_set_epi32(floatToInt(in[3], scale), floatToInt(in[2], scale),
                                          floatToInt(in[1], scale), floatToInt(in[0], scale));
        store(dst, _mm_castsi128_ps(out), dest);
    }

    // ITOFn: integer with n fraction bits to float.
    inline void itof(__m128 *dst, __m128 src, uint32_t fractionBits, uint32_t dest)
    {
        const __m128 value = _mm_mul_ps(_mm_cvtepi32_ps(_mm_castps_si128(src)),
                                        _mm_set1_ps(1.0f / static_cast<float>(1u << fractionBits)));
        store(dst, value, dest);
    }

    inline float lane(__m128 v, uint32_t field)
    {
        return _mm_cvtss_f32(broadcast(v, field));
    }

    // Sets Q and the I/D status flags (current bits 4/5 replaced, sticky bits 10/11 ORed).
    inline void setQ(R5900Context *ctx, float q, uint32_t di)
    {
        uint32_t ignored = 0u;
        ctx->vu0_q = _mm_cvtss_f32(normalizeResult(_mm_set1_ps(q), ignored));
        di &= 0x30u;
        ctx->vu0_status = static_cast<uint16_t>((ctx->vu0_status & 0xFCFu) | di | (di << 6));
    }

    inline float signedMax(bool negative)
    {
        return negative ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }

    // DIV Q, fs.fsf, ft.ftf
    inline void vdiv(R5900Context *ctx, float fs, float ft)
    {
        const float num = _mm_cvtss_f32(operandScalar(fs));
        const float den = _mm_cvtss_f32(operandScalar(ft));
        if (den == 0.0f)
            setQ(ctx, signedMax(std::signbit(num) != std::signbit(den)), num == 0.0f ? 0x10u : 0x20u);
        else
            setQ(ctx, num / den, 0u);
    }

    // SQRT Q, ft.ftf: sqrt(|ft|), I when ft is negative.
    inline void vsqrt(R5900Context *ctx, float ft)
    {
        const float value = _mm_cvtss_f32(operandScalar(ft));
        setQ(ctx, std::sqrt(std::fabs(value)), value < 0.0f ? 0x10u : 0u);
    }

    // RSQRT Q, fs.fsf, ft.ftf: fs / sqrt(|ft|).
    inline void vrsqrt(R5900Context *ctx, float fs, float ft)
    {
        const float num = _mm_cvtss_f32(operandScalar(fs));
        const float radicand = _mm_cvtss_f32(operandScalar(ft));
        const float den = std::sqrt(std::fabs(radicand));
        if (den == 0.0f)
            setQ(ctx, signedMax(std::signbit(num)), num == 0.0f ? 0x10u : 0x20u);
        else
            setQ(ctx, num / den, radicand < 0.0f ? 0x10u : 0u);
    }

    // CLIPw.xyz fs, ft.w: +x (bit 0), -x (1), +y (2), -y (3), +z (4), -z (5) against |w|,
    // pushed onto the 24-bit clip flag history.
    inline void clip(R5900Context *ctx, __m128 fs, float w)
    {
        uint32_t wBits = 0u;
        std::memcpy(&wBits, &w, sizeof(wBits));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        alignas(16) uint32_t v[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(v), _mm_castps_si128(fs));
        uint32_t flags = 0u;
        for (uint32_t c = 0; c < 3u; ++c)
        {
            if (static_cast<int32_t>(v[c]) > limit)
                flags |= 1u << (2u * c);
            if (static_cast<int32_t>(v[c] ^ 0x80000000u) > limit)
                flags |= 2u << (2u * c);
        }
        ctx->vu0_clip_flags = ((ctx->vu0_clip_flags << 6) | flags) & 0x00FFFFFFu;
    }

    // CTC2 to STATUS: only the sticky flags (bits 6-11) are writable.
    inline void writeStatus(R5900Context *ctx, uint32_t value)
    {
        ctx->vu0_status = static_cast<uint16_t>((ctx->vu0_status & 0x03Fu) | (value & 0xFC0u));
    }

    // The R register (23-bit random number with exponent 0x7F) lives in lane 0 of vu0_r.
    inline uint32_t rValue(const R5900Context *ctx)
    {
        return static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(ctx->vu0_r)));
    }

    inline void setR(R5900Context *ctx, uint32_t r)
    {
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x3F800000u | (r & 0x007FFFFFu))));
    }

    // RNEXT: advance the LFSR, then write R to the dest fields of ft.
    inline void rnext(R5900Context *ctx, __m128 *dst, uint32_t dest)
    {
        const uint32_t r = rValue(ctx);
        const uint32_t x = (r >> 4) & 1u;
        const uint32_t y = (r >> 22) & 1u;
        setR(ctx, (r << 1) ^ x ^ y);
        store(dst, ctx->vu0_r, dest);
    }

    inline void rget(R5900Context *ctx, __m128 *dst, uint32_t dest)
    {
        store(dst, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(rValue(ctx)))), dest);
    }

    inline void rinit(R5900Context *ctx, float fs)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &fs, sizeof(bits));
        setR(ctx, bits);
    }

    inline void rxor(R5900Context *ctx, float fs)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &fs, sizeof(bits));
        setR(ctx, rValue(ctx) ^ bits);
    }
}

#endif // PS2_VU0_MACRO_H
