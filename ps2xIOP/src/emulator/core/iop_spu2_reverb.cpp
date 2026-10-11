#include "iop_spu2_reverb.h"

#include <algorithm>
#include <cmath>

namespace ps2x::iop::detail
{
    namespace
    {
        int32_t clamp16(int64_t value) noexcept
        {
            return static_cast<int32_t>(std::clamp<int64_t>(value, -32768, 32767));
        }

        // (a * b) >> 15 with a 16-bit coefficient: the reverb's volume/coefficient product.
        int64_t mul(int64_t value, int16_t coef) noexcept
        {
            return (value * coef) >> 15;
        }

        // The halfword at `offset` (+ `back`, -1 = one sample earlier) from the current
        // position, wrapped inside the work area.
        uint32_t areaAddress(const Spu2Reverb::State &state, const Spu2Reverb::Params &params, int64_t offset, int32_t back = 0) noexcept
        {
            const int64_t size = static_cast<int64_t>(params.end - params.start) + 1;
            int64_t rel = (static_cast<int64_t>(state.position - params.start) + offset + back) % size;
            if (rel < 0)
                rel += size;
            return params.start + static_cast<uint32_t>(rel);
        }
    }

    const std::array<int32_t, Spu2Reverb::Taps> &Spu2Reverb::filter() noexcept
    {
        // Half-band low-pass (cutoff at a quarter of 48 kHz): windowed sinc, Blackman window;
        // every other tap is zero except the centre. Normalised to a DC gain of exactly 1.0
        // in Q15 (the centre absorbs the rounding).
        static const std::array<int32_t, Taps> s_taps = [] {
            constexpr double pi = 3.14159265358979323846;
            constexpr int centre = static_cast<int>(Taps / 2u);
            std::array<int32_t, Taps> taps{};
            int32_t sum = 0;
            for (int n = 0; n < static_cast<int>(Taps); ++n)
            {
                const int k = n - centre;
                if (k == 0 || (k & 1) == 0)
                    continue;
                const double sinc = std::sin(pi * k / 2.0) / (pi * k);
                const double window = 0.42 - 0.5 * std::cos(2.0 * pi * n / (Taps - 1)) + 0.08 * std::cos(4.0 * pi * n / (Taps - 1));
                taps[n] = static_cast<int32_t>(std::lround(sinc * window * 32768.0));
                sum += taps[n];
            }
            taps[centre] = 32768 - sum;
            return taps;
        }();
        return s_taps;
    }

    void Spu2Reverb::process(State &state, const Params &params, uint16_t *ram, uint32_t ramMask,
                             int32_t inL, int32_t inR, int32_t &outL, int32_t &outR) noexcept
    {
        outL = 0;
        outR = 0;
        if (params.end <= params.start)
            return;
        if (state.position < params.start || state.position > params.end)
            state.position = params.start;

        const auto &a = params.address;
        const auto &c = params.coef;
        auto rd = [&](int64_t offset, int32_t back = 0) -> int64_t {
            return static_cast<int16_t>(ram[areaAddress(state, params, offset, back) & ramMask]);
        };
        // Effects disabled (ATTR bit 7 clear): the unit still reads the buffer and outputs, it
        // only stops writing to it (public SPU description).
        auto wr = [&](int64_t offset, int64_t value) {
            if (!params.enabled)
                return;
            ram[areaAddress(state, params, offset) & ramMask] = static_cast<uint16_t>(static_cast<int16_t>(clamp16(value)));
        };

        // Input from the mixer (the effects send) times the input coefficients.
        const int64_t lin = mul(inL, c[InCoefL]);
        const int64_t rin = mul(inR, c[InCoefR]);

        // Same-side reflections (L to L, R to R): an IIR on the previous sample of the
        // destination, fed by the input and a delayed tap times the wall coefficient.
        const int64_t lsame = rd(a[IirDestA0], -1);
        const int64_t rsame = rd(a[IirDestA1], -1);
        wr(a[IirDestA0], mul(lin + mul(rd(a[IirSrcA0]), c[IirCoef]) - lsame, c[IirAlpha]) + lsame);
        wr(a[IirDestA1], mul(rin + mul(rd(a[IirSrcA1]), c[IirCoef]) - rsame, c[IirAlpha]) + rsame);

        // Cross-side reflections (R to L, L to R).
        const int64_t ldiff = rd(a[IirDestB0], -1);
        const int64_t rdiff = rd(a[IirDestB1], -1);
        wr(a[IirDestB0], mul(lin + mul(rd(a[IirSrcB0]), c[IirCoef]) - ldiff, c[IirAlpha]) + ldiff);
        wr(a[IirDestB1], mul(rin + mul(rd(a[IirSrcB1]), c[IirCoef]) - rdiff, c[IirAlpha]) + rdiff);

        // Early echo: four comb taps.
        int64_t l = mul(rd(a[AccSrcA0]), c[AccCoefA]) + mul(rd(a[AccSrcB0]), c[AccCoefB]) +
                    mul(rd(a[AccSrcC0]), c[AccCoefC]) + mul(rd(a[AccSrcD0]), c[AccCoefD]);
        int64_t r = mul(rd(a[AccSrcA1]), c[AccCoefA]) + mul(rd(a[AccSrcB1]), c[AccCoefB]) +
                    mul(rd(a[AccSrcC1]), c[AccCoefC]) + mul(rd(a[AccSrcD1]), c[AccCoefD]);

        // Late reverb: two all-pass stages, each delayed by FB_SRC_A / FB_SRC_B behind its
        // destination.
        auto allPass = [&](int64_t value, uint32_t dest, uint32_t delay, int16_t coef) {
            const int64_t delayed = rd(static_cast<int64_t>(dest) - static_cast<int64_t>(delay));
            value = clamp16(value - mul(delayed, coef));
            wr(dest, value);
            return clamp16(mul(value, coef) + delayed);
        };
        l = allPass(clamp16(l), a[MixDestA0], a[FbSrcA], c[FbAlpha]);
        r = allPass(clamp16(r), a[MixDestA1], a[FbSrcA], c[FbAlpha]);
        l = allPass(l, a[MixDestB0], a[FbSrcB], c[FbX]);
        r = allPass(r, a[MixDestB1], a[FbSrcB], c[FbX]);

        outL = clamp16(l);
        outR = clamp16(r);
        state.position = state.position >= params.end ? params.start : state.position + 1u;
    }

    void Spu2Reverb::step(State &state, const Params &params, uint16_t *ram, uint32_t ramMask,
                          int32_t inL, int32_t inR, int32_t &outL, int32_t &outR) noexcept
    {
        const auto &h = filter();
        state.ring = (state.ring + 1u) % Taps;
        state.inL[state.ring] = static_cast<int16_t>(clamp16(inL));
        state.inR[state.ring] = static_cast<int16_t>(clamp16(inR));

        int32_t wetL = 0;
        int32_t wetR = 0;
        if (state.phase == 0u)
        {
            // Decimate: the filtered input at this sample.
            int64_t sumL = 0;
            int64_t sumR = 0;
            for (uint32_t k = 0u; k < Taps; ++k)
            {
                const uint32_t at = (state.ring + Taps - k) % Taps;
                sumL += static_cast<int64_t>(h[k]) * state.inL[at];
                sumR += static_cast<int64_t>(h[k]) * state.inR[at];
            }
            process(state, params, ram, ramMask, clamp16(sumL >> 15), clamp16(sumR >> 15), wetL, wetR);
        }
        // Interpolate: the reverb output zero-stuffed to 48 kHz through the same filter, x2.
        state.upL[state.ring] = static_cast<int16_t>(wetL);
        state.upR[state.ring] = static_cast<int16_t>(wetR);
        int64_t sumL = 0;
        int64_t sumR = 0;
        for (uint32_t k = 0u; k < Taps; ++k)
        {
            const uint32_t at = (state.ring + Taps - k) % Taps;
            sumL += static_cast<int64_t>(h[k]) * state.upL[at];
            sumR += static_cast<int64_t>(h[k]) * state.upR[at];
        }
        outL = clamp16(sumL >> 14);
        outR = clamp16(sumR >> 14);
        state.phase ^= 1u;
    }
}
