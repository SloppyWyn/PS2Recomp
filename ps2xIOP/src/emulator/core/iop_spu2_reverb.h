#pragma once

// SPU2 reverb (effects) unit, one per core (docs/SPU2-MODEL.md, "Reverb"). Written from the
// public descriptions of the PS1 SPU / PS2 SPU2 reverb (register list and the reverb
// formula: same-side and cross-side IIR reflections, four comb taps, two all-pass stages),
// not from any emulator's code.
//
// The unit runs at half the output rate (24 kHz): the effects input is low-pass filtered and
// decimated by 2, one reverb step computes both sides and advances the buffer pointer by one
// halfword through the work area [ESA, EEA end], and the output is interpolated back to
// 48 kHz with the same filter. The filter is a half-band windowed-sinc FIR of our own design
// (the hardware's exact coefficients are not modelled).

#include <array>
#include <cstdint>

namespace ps2x::iop::detail
{
    struct Spu2Reverb
    {
        static constexpr uint32_t Taps = 39u; // odd: centre tap at Taps / 2

        // Address registers, core-relative offsets of their hi halfword (lo at +2); each a
        // halfword offset from the current buffer position. PS1 names in the comments.
        enum Address : uint32_t
        {
            FbSrcA = 0,    // dAPF1
            FbSrcB,        // dAPF2
            IirDestA0,     // mLSAME
            IirDestA1,     // mRSAME
            AccSrcA0,      // mLCOMB1
            AccSrcA1,      // mRCOMB1
            AccSrcB0,      // mLCOMB2
            AccSrcB1,      // mRCOMB2
            IirSrcA0,      // dLSAME
            IirSrcA1,      // dRSAME
            IirDestB0,     // mLDIFF
            IirDestB1,     // mRDIFF
            AccSrcC0,      // mLCOMB3
            AccSrcC1,      // mRCOMB3
            AccSrcD0,      // mLCOMB4
            AccSrcD1,      // mRCOMB4
            IirSrcB1,      // dLDIFF
            IirSrcB0,      // dRDIFF
            MixDestA0,     // mLAPF1
            MixDestA1,     // mRAPF1
            MixDestB0,     // mLAPF2
            MixDestB1,     // mRAPF2
            AddressCount
        };
        // Coefficients, in the order of the volume block after MVOLX (0x774 + 2 * n, core 1
        // +0x28).
        enum Coef : uint32_t
        {
            IirAlpha = 0, // vIIR
            AccCoefA,     // vCOMB1
            AccCoefB,     // vCOMB2
            AccCoefC,     // vCOMB3
            AccCoefD,     // vCOMB4
            IirCoef,      // vWALL
            FbAlpha,      // vAPF1
            FbX,          // vAPF2
            InCoefL,      // vLIN
            InCoefR,      // vRIN
            CoefCount
        };

        static constexpr uint32_t EsaRegister = 0x2E0u;      // hi/lo: start of the work area
        static constexpr uint32_t AddressRegisters = 0x2E4u; // FB_SRC_A .. MIX_DEST_B1, 4 bytes each
        static constexpr uint32_t EeaRegister = 0x33Cu;      // bits 16-19 of the area's last halfword
        static constexpr uint32_t CoefRegisters = 0x774u;    // absolute; core 1 at +0x28

        // Everything one step needs, read from the registers by the caller.
        struct Params
        {
            bool enabled = false;   // ATTR bit 7 (effects enable): clear = no buffer writes; reads,
                                    // output and the pointer go on
            uint32_t start = 0;     // ESA, halfwords
            uint32_t end = 0;       // last halfword of the area: (EEA << 16) | 0xFFFF
            std::array<uint32_t, AddressCount> address{};
            std::array<int16_t, CoefCount> coef{};
        };

        // Per-core state (plain data: saved with the SPU2 state).
        struct State
        {
            uint32_t position = 0;  // current buffer halfword address
            uint32_t phase = 0;     // 48 kHz sample parity: a reverb step on phase 0
            uint32_t ring = 0;      // index of the newest entry in the histories below
            std::array<int16_t, Taps> inL{};  // effects input at 48 kHz (decimation filter)
            std::array<int16_t, Taps> inR{};
            std::array<int16_t, Taps> upL{};  // reverb output zero-stuffed to 48 kHz
            std::array<int16_t, Taps> upR{};
        };

        // One 48 kHz sample: takes the effects input, returns the effects output (before
        // EVOL). `ram` is the 2 MB sound RAM as halfwords (mask = halfword count - 1).
        static void step(State &state, const Params &params, uint16_t *ram, uint32_t ramMask,
                         int32_t inL, int32_t inR, int32_t &outL, int32_t &outR) noexcept;

        // One 24 kHz reverb step on already decimated inputs (the formula itself; tests).
        static void process(State &state, const Params &params, uint16_t *ram, uint32_t ramMask,
                            int32_t inL, int32_t inR, int32_t &outL, int32_t &outR) noexcept;

        // The half-band filter, Q15, Taps entries (sum of the taps = 0x8000).
        static const std::array<int32_t, Taps> &filter() noexcept;
    };
}
