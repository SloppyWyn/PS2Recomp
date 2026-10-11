#include "iop_spu2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kAddressMask = Spu2::RamHalfwords - 1u;
        constexpr uint32_t kBlockHalfwords = 8u;
        constexpr uint64_t kIdleWakeCycles = Spu2::CyclesPerSample * 48u; // 1 ms while voices run

        // Core-relative register offsets.
        constexpr uint32_t kAttr = 0x19Au;
        constexpr uint32_t kIrqaHi = 0x19Cu;
        constexpr uint32_t kKonLo = 0x1A0u;
        constexpr uint32_t kKonHi = 0x1A2u;
        constexpr uint32_t kKoffLo = 0x1A4u;
        constexpr uint32_t kKoffHi = 0x1A6u;
        constexpr uint32_t kTsaHi = 0x1A8u;
        constexpr uint32_t kTsaLo = 0x1AAu;
        constexpr uint32_t kData = 0x1ACu;
        constexpr uint32_t kAdmas = 0x1B0u;
        constexpr uint32_t kVoiceAddr = 0x1C0u; // + voice * 0xC: SSA hi/lo, LSAX hi/lo, NAX hi/lo
        constexpr uint32_t kEndxLo = 0x340u;
        constexpr uint32_t kEndxHi = 0x342u;
        constexpr uint32_t kStatx = 0x344u;
        constexpr uint32_t kIrqInfo = 0x7C2u; // SPDIF block, shared by both cores
        constexpr uint32_t kAutoDmaArea = 0x2000u; // halfwords; core 1 at +0x400
        constexpr uint32_t kAutoDmaChunkBytes = 1024u; // 256 L samples then 256 R samples
        // Core-relative mixer registers (bits for voices 0-15 at the offset, 16-23 at +2).
        constexpr uint32_t kPmon = 0x180u;
        constexpr uint32_t kNon = 0x184u;
        constexpr uint32_t kVmixL = 0x188u;
        constexpr uint32_t kVmixR = 0x190u;
        constexpr uint32_t kMmix = 0x198u;
        // MMIX: external input (core 1: core 0's output; AVOL), memory input (AutoDMA; BVOL)
        // and the voices' mix, each to the core's dry output left/right (even bits) and to
        // the effects unit's input (odd bits; voices through VMIXEL/VMIXER).
        constexpr uint16_t kMmixExtR = 1u << 2u;
        constexpr uint16_t kMmixExtL = 1u << 3u;
        constexpr uint16_t kMmixMemR = 1u << 6u;
        constexpr uint16_t kMmixMemL = 1u << 7u;
        constexpr uint16_t kMmixVoiceR = 1u << 10u;
        constexpr uint16_t kMmixVoiceL = 1u << 11u;
        constexpr uint16_t kMmixExtWetR = 1u << 0u;
        constexpr uint16_t kMmixExtWetL = 1u << 1u;
        constexpr uint16_t kMmixMemWetR = 1u << 4u;
        constexpr uint16_t kMmixMemWetL = 1u << 5u;
        constexpr uint16_t kMmixVoiceWetR = 1u << 8u;
        constexpr uint16_t kMmixVoiceWetL = 1u << 9u;
        constexpr uint32_t kVmixEL = 0x18Cu;
        constexpr uint32_t kVmixER = 0x194u;
        constexpr uint16_t kAttrEffects = 1u << 7u;
        // Per-core volume block (absolute offsets, core 1 at +0x28): MVOLL/R, EVOLL/R,
        // AVOLL/R, BVOLL/R, MVOLXL/R.
        constexpr uint32_t kVolumeBlock = 0x760u;
        constexpr uint32_t kVolumeBlockStride = 0x28u;
        constexpr uint32_t kMvol = 0x0u;
        constexpr uint32_t kEvol = 0x4u; // effects return volume
        constexpr uint32_t kAvol = 0x8u; // external input volume (libsd sets core 1's to 0x7FFF)
        constexpr uint32_t kBvol = 0xCu; // memory (AutoDMA) input volume
        constexpr uint32_t kMvolx = 0x10u;

        // PS2X_IOP_SPU2_LOG=N: key on/off, transfers, ends and interrupts on stderr, up to N
        // lines (1 = 4000).
        int eventLogLimit()
        {
            static const int s_limit = [] {
                const char *value = std::getenv("PS2X_IOP_SPU2_LOG");
                if (!value || !*value || *value == '0')
                    return 0;
                const int n = std::atoi(value);
                return n > 1 ? n : 4000;
            }();
            return s_limit;
        }

        // outFrame >= 0: the mixer's output frame index at this point (the position in a
        // PS2X_SPU2_WAV dump), printed as out=.
        void logEvent(const char *fmt, uint32_t a, uint32_t b, uint32_t c, uint64_t sample, int64_t outFrame = -1)
        {
            static int s_lines = 0;
            if (s_lines >= eventLogLimit())
                return;
            ++s_lines;
            std::fprintf(stderr, "[iop:spu2] sample=%llu ", static_cast<unsigned long long>(sample));
            if (outFrame >= 0)
                std::fprintf(stderr, "out=%lld ", static_cast<long long>(outFrame));
            std::fprintf(stderr, fmt, a, b, c);
            std::fputc('\n', stderr);
        }

        int16_t clamp16(int32_t value) noexcept
        {
            return static_cast<int16_t>(std::clamp(value, -32768, 32767));
        }

        uint32_t voiceBits(const std::array<uint16_t, 0x400> &regs, uint32_t offset) noexcept
        {
            return static_cast<uint32_t>(regs[offset >> 1u]) | (static_cast<uint32_t>(regs[(offset >> 1u) + 1u] & 0xFFu) << 16u);
        }

        // A volume block offset (absolute 0x760..0x7AF) -> core and register within it.
        bool volumeBlockRegister(uint32_t offset, uint32_t &core, uint32_t &rel) noexcept
        {
            if (offset < kVolumeBlock || offset >= kVolumeBlock + 2u * kVolumeBlockStride)
                return false;
            core = (offset - kVolumeBlock) / kVolumeBlockStride;
            rel = (offset - kVolumeBlock) % kVolumeBlockStride;
            return true;
        }
    }

    int32_t Spu2::VolumeState::value(uint16_t reg) const noexcept
    {
        if ((reg & 0x8000u) == 0u)
            return level;
        return (reg & 0x1000u) != 0u ? -level : level;
    }

    void Spu2::writeVolume(VolumeState &state, uint16_t reg) noexcept
    {
        state.counter = 0u;
        if ((reg & 0x8000u) == 0u)
            state.level = static_cast<int16_t>(static_cast<uint16_t>(reg << 1u)); // signed 15-bit, doubled
        else
            state.level = std::clamp(state.level < 0 ? -state.level : state.level, 0, 0x7FFF); // a sweep starts at the current level
    }

    void Spu2::stepVolume(VolumeState &state, uint16_t reg) noexcept
    {
        if ((reg & 0x8000u) == 0u)
            return;
        // Same rate rules as the ADSR phases: shift = rate bits 2-6, step 7-n up / -8+n down.
        const bool exponential = (reg & 0x4000u) != 0u;
        const bool decrease = (reg & 0x2000u) != 0u;
        const uint32_t rate = reg & 0x7Fu;
        const uint32_t shift = rate >> 2u;
        const int32_t step = decrease ? -8 + static_cast<int32_t>(rate & 3u) : 7 - static_cast<int32_t>(rate & 3u);
        uint32_t cycles = 1u << (shift > 11u ? shift - 11u : 0u);
        int32_t stepValue = step * (1 << (shift < 11u ? 11u - shift : 0u));
        if (exponential && !decrease && state.level > 0x6000)
            cycles *= 4u;
        if (exponential && decrease)
        {
            stepValue = (stepValue * state.level) / 0x8000;
            if (stepValue == 0)
                stepValue = -1;
        }
        if (++state.counter < cycles)
            return;
        state.counter = 0u;
        state.level = std::clamp(state.level + stepValue, 0, 0x7FFF);
    }

    Spu2::Spu2()
        : m_ram(RamHalfwords)
    {
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_regs.fill(0u);
        for (auto &core : m_voices)
            for (auto &voice : core)
                voice = Voice{};
        for (auto &core : m_cores)
            core = Core{};
        for (auto &transfer : m_transfer)
            transfer = Transfer{};
        m_sampleCycle = 0u;
        m_samples = 0u;
        m_pendingIrqs = 0u;
        m_runningVoices = 0u;
        m_outBuffer.clear();
        m_reverbParamsDirty = {true, true};
    }

    void Spu2::setOutputSink(OutputSink sink)
    {
        flushOutput();
        m_output = std::move(sink);
        m_outBuffer.clear();
        m_outputFrames = 0u;
        if (m_output)
            m_outBuffer.reserve(OutputBatchFrames * 2u);
    }

    void Spu2::flushOutput()
    {
        if (m_output && !m_outBuffer.empty())
            m_output(m_outBuffer.data(), m_outBuffer.size() / 2u);
        m_outBuffer.clear();
    }

    void Spu2::bind(const uint64_t *clock, uint8_t *iopRam, uint32_t iopRamSize) noexcept
    {
        m_clock = clock;
        m_iopRam = iopRam;
        m_iopRamSize = iopRamSize;
    }

    uint32_t Spu2::composeAddress(uint32_t hiOffset) const noexcept
    {
        return ((static_cast<uint32_t>(reg(hiOffset)) << 16u) | reg(hiOffset + 2u)) & kAddressMask;
    }

    uint16_t Spu2::endx(uint32_t core) const noexcept
    {
        return static_cast<uint16_t>(m_cores[core & 1u].endx & 0xFFFFu);
    }

    uint16_t Spu2::statx(uint32_t core) const noexcept
    {
        // Bit 7: a DMA transfer mode is selected and the transfer unit is idle (drivers wait
        // for it after the DMA interrupt); bit 10: a transfer is in progress (DMA or AutoDMA;
        // FIFO writes land at once, so the FIFO path never sees it set).
        const uint32_t mode = transferMode(core);
        uint16_t value = 0u;
        if ((mode == 2u || mode == 3u) && !m_transfer[core].active)
            value |= 0x80u;
        if (m_transfer[core].active)
            value |= 0x400u;
        return value;
    }

    uint16_t Spu2::readRegister(uint32_t offset)
    {
        catchUp();
        offset &= RegisterBytes - 2u;
        {
            uint32_t volumeCore = 0u, volumeRel = 0u;
            if (volumeBlockRegister(offset, volumeCore, volumeRel) && (volumeRel == kMvolx || volumeRel == kMvolx + 2u))
            {
                const Core &state = m_cores[volumeCore];
                const uint32_t base = kVolumeBlock + volumeCore * kVolumeBlockStride + kMvol;
                return volumeRel == kMvolx ? static_cast<uint16_t>(state.mvolL.value(reg(base)))
                                           : static_cast<uint16_t>(state.mvolR.value(reg(base + 2u)));
            }
        }
        const uint32_t core = offset >> 10u;
        const uint32_t rel = offset & 0x3FFu;
        if (core < CoreCount)
        {
            if (rel < 0x180u)
            {
                const uint32_t index = rel >> 4u;
                if ((rel & 0xFu) == 0xAu && index < VoicesPerCore)
                    return static_cast<uint16_t>(m_voices[core][index].envLevel);
                if (((rel & 0xFu) == 0xCu || (rel & 0xFu) == 0xEu) && index < VoicesPerCore)
                {
                    // VOLXL/VOLXR: the current (possibly sweeping) volume.
                    const Voice &voice = m_voices[core][index];
                    const uint32_t volumeReg = core * 0x400u + index * 0x10u + ((rel & 0xFu) == 0xCu ? 0u : 2u);
                    return static_cast<uint16_t>((rel & 0xFu) == 0xCu ? voice.volL.value(reg(volumeReg)) : voice.volR.value(reg(volumeReg)));
                }
            }
            else if (rel >= kVoiceAddr && rel < kVoiceAddr + VoicesPerCore * 0xCu)
            {
                const uint32_t index = (rel - kVoiceAddr) / 0xCu;
                const uint32_t part = (rel - kVoiceAddr) % 0xCu;
                const Voice &voice = m_voices[core][index];
                const uint32_t address = part < 4u ? voice.ssa : (part < 8u ? voice.lsa : voice.nax);
                return (part & 2u) ? static_cast<uint16_t>(address & 0xFFFFu) : static_cast<uint16_t>(address >> 16u);
            }
            else if (rel == kEndxLo)
                return static_cast<uint16_t>(m_cores[core].endx & 0xFFFFu);
            else if (rel == kEndxHi)
                return static_cast<uint16_t>((m_cores[core].endx >> 16u) & 0xFFu);
            else if (rel == kStatx)
                return statx(core);
            else if (rel == kKonLo || rel == kKonHi)
                return static_cast<uint16_t>(m_cores[core].konLatch >> (rel == kKonHi ? 16u : 0u));
            else if (rel == kKoffLo || rel == kKoffHi)
                return static_cast<uint16_t>(m_cores[core].koffLatch >> (rel == kKoffHi ? 16u : 0u));
        }
        return reg(offset);
    }

    void Spu2::writeRegister(uint32_t offset, uint16_t value)
    {
        catchUp();
        offset &= RegisterBytes - 2u;
        logSetupWrite(offset, value);
        noteReverbRegisterWrite(offset);
        {
            uint32_t volumeCore = 0u, volumeRel = 0u;
            if (volumeBlockRegister(offset, volumeCore, volumeRel))
            {
                if (volumeRel == kMvolx || volumeRel == kMvolx + 2u)
                    return; // MVOLX is read-only
                setReg(offset, value);
                if (volumeRel == kMvol)
                    writeVolume(m_cores[volumeCore].mvolL, value);
                else if (volumeRel == kMvol + 2u)
                    writeVolume(m_cores[volumeCore].mvolR, value);
                return;
            }
        }
        const uint32_t core = offset >> 10u;
        const uint32_t rel = offset & 0x3FFu;
        if (core >= CoreCount)
        {
            setReg(offset, value);
            return;
        }
        Core &state = m_cores[core];
        if (rel < 0x180u)
        {
            setReg(offset, value); // voice volume / pitch / ADSR; ENVX writes set the level
            const uint32_t index = rel >> 4u;
            if (index < VoicesPerCore)
            {
                Voice &voice = m_voices[core][index];
                if ((rel & 0xFu) == 0xAu)
                    voice.envLevel = static_cast<int16_t>(value) < 0 ? 0 : (value & 0x7FFFu);
                else if ((rel & 0xFu) == 0x0u)
                    writeVolume(voice.volL, value);
                else if ((rel & 0xFu) == 0x2u)
                    writeVolume(voice.volR, value);
            }
            return;
        }
        if (rel >= kVoiceAddr && rel < kVoiceAddr + VoicesPerCore * 0xCu)
        {
            setReg(offset, value);
            const uint32_t index = (rel - kVoiceAddr) / 0xCu;
            const uint32_t part = (rel - kVoiceAddr) % 0xCu;
            Voice &voice = m_voices[core][index];
            const uint32_t base = core * 0x400u + kVoiceAddr + index * 0xCu;
            if (part < 4u)
                voice.ssa = composeAddress(base) & ~(kBlockHalfwords - 1u);
            else if (part < 8u)
            {
                voice.lsa = composeAddress(base + 4u) & ~(kBlockHalfwords - 1u);
                voice.manualLoop = true;
            }
            // NAX is read-only: it follows the decoder.
            return;
        }
        switch (rel)
        {
        case kAttr:
        {
            const uint16_t previous = reg(offset);
            setReg(offset, value);
            if ((previous & 0x40u) != 0u && (value & 0x40u) == 0u)
                setReg(kIrqInfo, static_cast<uint16_t>(reg(kIrqInfo) & ~(4u << core)));
            if (((value >> 4u) & 3u) == 1u)
                flushFifo(core);
            return;
        }
        case kIrqaHi:
        case kIrqaHi + 2u:
            setReg(offset, value);
            state.irqa = composeAddress(core * 0x400u + kIrqaHi);
            return;
        case kKonLo:
        case kKonHi:
        {
            setReg(offset, value);
            const uint32_t shift = rel == kKonHi ? 16u : 0u;
            state.konLatch = (state.konLatch & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(value) << shift);
            for (uint32_t bit = 0u; bit < 16u; ++bit)
                if ((value & (1u << bit)) != 0u && bit + shift < VoicesPerCore)
                    keyOn(core, bit + shift);
            return;
        }
        case kKoffLo:
        case kKoffHi:
        {
            setReg(offset, value);
            const uint32_t shift = rel == kKoffHi ? 16u : 0u;
            state.koffLatch = (state.koffLatch & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(value) << shift);
            for (uint32_t bit = 0u; bit < 16u; ++bit)
                if ((value & (1u << bit)) != 0u && bit + shift < VoicesPerCore)
                    keyOff(core, bit + shift);
            return;
        }
        case kTsaHi:
        case kTsaLo:
            setReg(offset, value);
            state.tsa = composeAddress(core * 0x400u + kTsaHi);
            return;
        case kData:
            if (transferMode(core) == 1u)
                writeRam(core, state.tsa++, value);
            else if (state.fifoCount < state.fifo.size())
                state.fifo[state.fifoCount++] = value;
            return;
        case kEndxLo:
            // A write (any value: libsd writes 0) clears the end flags of the half written:
            // 0x340 voices 0-15, 0x342 voices 16-23. No public description says one half's
            // write clears the other; libsd always writes both.
            state.endx &= ~0xFFFFu;
            return;
        case kEndxHi:
            state.endx &= 0xFFFFu;
            return;
        case kStatx:
            return; // read-only
        case kAdmas:
        {
            const bool was = autoDmaEnabled(core);
            setReg(offset, value);
            const bool now = autoDmaEnabled(core);
            if (now && !was)
            {
                // Input playback starts over with both halves empty.
                state.admaPos = 0u;
                state.admaFilled = 0u;
                state.admaNextHalf = 0u;
            }
            else if (!now && was && m_transfer[core].active && m_transfer[core].autoDma)
            {
                // Nothing consumes the rest any more: the transfer stops where it is,
                // without a DMA-done interrupt (it did not complete).
                m_transfer[core].active = false;
                logEvent("adma-stop core=%u done=%u bytes=%u", core, m_transfer[core].done, m_transfer[core].bytes, m_samples);
            }
            return;
        }
        default:
            setReg(offset, value);
            return;
        }
    }

    void Spu2::flushFifo(uint32_t core)
    {
        Core &state = m_cores[core];
        for (uint32_t i = 0u; i < state.fifoCount; ++i)
            writeRam(core, state.tsa++, state.fifo[i]);
        state.fifoCount = 0u;
    }

    void Spu2::writeRam(uint32_t core, uint32_t halfword, uint16_t value)
    {
        m_ram[halfword & kAddressMask] = value;
        checkIrqRange(core, halfword & kAddressMask, 1u);
    }

    void Spu2::raiseIrq(uint32_t core)
    {
        const uint16_t info = reg(kIrqInfo);
        const uint16_t bit = static_cast<uint16_t>(4u << core);
        if ((info & bit) != 0u)
            return;
        setReg(kIrqInfo, static_cast<uint16_t>(info | bit));
        m_pendingIrqs |= IrqSpu2;
        logEvent("irq core=%u irqa=0x%05X (%u)", core, m_cores[core].irqa, 0u, m_samples);
    }

    void Spu2::checkIrqRange(uint32_t core, uint32_t halfword, uint32_t count)
    {
        // Either core's IRQA may be hit by any access; each core raises its own flag.
        for (uint32_t c = 0u; c < CoreCount; ++c)
        {
            if (!irqEnabled(c))
                continue;
            const uint32_t irqa = m_cores[c].irqa;
            if (irqa >= halfword && irqa < halfword + count)
                raiseIrq(c);
        }
    }

    void Spu2::keyOn(uint32_t core, uint32_t index)
    {
        Voice &voice = m_voices[core][index];
        if (!voice.running)
            ++m_runningVoices;
        voice.running = true;
        voice.fresh = true;
        voice.nax = voice.ssa;
        voice.manualLoop = false;
        voice.blockFlags = 0u;
        voice.sampleIndex = 28u;
        voice.pitchCounter = 0u;
        voice.prev1 = 0;
        voice.prev2 = 0;
        voice.phase = EnvelopePhase::Attack;
        voice.envLevel = 0;
        voice.envCounter = 0u;
        voice.lastSample = 0;
        voice.prevSample = 0;
        voice.outx = 0;
        m_cores[core].endx &= ~(1u << index);
        logEvent("kon core=%u voice=%u ssa=0x%05X", core, index, voice.ssa, m_samples,
                 m_output ? static_cast<int64_t>(m_outputFrames) : -1);
        if (eventLogLimit() > 0)
        {
            // The flag bytes of the first eight blocks at SSA (end 1, repeat 2, loop-start 4),
            // one hex digit each, block 0 first: shows how a stream's buffer was laid out.
            uint32_t flags = 0u;
            for (uint32_t block = 0u; block < 8u; ++block)
                flags |= static_cast<uint32_t>((m_ram[(voice.ssa + block * kBlockHalfwords) & kAddressMask] >> 8u) & 0xFu) << (28u - block * 4u);
            logEvent("    pitch=0x%04X flags[0..7]=%08X adsr1=0x%04X", reg(core * 0x400u + index * 0x10u + 4u), flags,
                     reg(core * 0x400u + index * 0x10u + 6u), m_samples);
        }
    }

    void Spu2::keyOff(uint32_t core, uint32_t index)
    {
        Voice &voice = m_voices[core][index];
        if (!voice.running || voice.phase == EnvelopePhase::Release)
            return;
        voice.phase = EnvelopePhase::Release;
        voice.envCounter = 0u;
        logEvent("koff core=%u voice=%u envx=%u", core, index, static_cast<uint32_t>(voice.envLevel), m_samples);
    }

    uint8_t Spu2::decodeBlock(const uint16_t *block, int16_t &prev1, int16_t &prev2, std::array<int16_t, 28> &out) noexcept
    {
        static constexpr int32_t kFilter0[5] = {0, 60, 115, 98, 122};
        static constexpr int32_t kFilter1[5] = {0, 0, -52, -55, -60};
        const uint16_t header = block[0];
        uint32_t shift = header & 0xFu;
        if (shift > 12u)
            shift = 9u;
        const uint32_t filter = std::min<uint32_t>((header >> 4u) & 0xFu, 4u);
        const uint8_t flags = static_cast<uint8_t>(header >> 8u);
        int32_t s1 = prev1;
        int32_t s2 = prev2;
        for (uint32_t i = 0u; i < 28u; ++i)
        {
            const uint16_t word = block[1u + (i >> 2u)];
            const uint32_t nibble = (word >> ((i & 3u) * 4u)) & 0xFu;
            int32_t sample = static_cast<int16_t>(nibble << 12u) >> shift;
            sample += (s1 * kFilter0[filter] + s2 * kFilter1[filter] + 32) >> 6;
            sample = std::clamp(sample, -32768, 32767);
            out[i] = static_cast<int16_t>(sample);
            s2 = s1;
            s1 = sample;
        }
        prev1 = static_cast<int16_t>(s1);
        prev2 = static_cast<int16_t>(s2);
        return flags;
    }

    void Spu2::fetchBlock(uint32_t core, uint32_t index, Voice &voice)
    {
        (void)index;
        voice.nax &= kAddressMask & ~(kBlockHalfwords - 1u);
        voice.blockFlags = decodeBlock(&m_ram[voice.nax], voice.prev1, voice.prev2, voice.samples);
        voice.sampleIndex = 0u;
        voice.fresh = false;
        if ((voice.blockFlags & 4u) != 0u && !voice.manualLoop)
            voice.lsa = voice.nax;
        checkIrqRange(core, voice.nax, kBlockHalfwords);
    }

    void Spu2::stepEnvelope(uint32_t core, uint32_t index, Voice &voice)
    {
        const uint16_t adsr1 = reg(core * 0x400u + index * 0x10u + 6u);
        const uint16_t adsr2 = reg(core * 0x400u + index * 0x10u + 8u);
        bool exponential = false;
        bool increase = false;
        uint32_t shift = 0u;
        int32_t step = 0;
        if (voice.phase == EnvelopePhase::Decay &&
            voice.envLevel <= ((static_cast<int32_t>(adsr1 & 0xFu) + 1) << 11u))
            voice.phase = EnvelopePhase::Sustain; // decay ends once the level is at the sustain level
        switch (voice.phase)
        {
        case EnvelopePhase::Attack:
            exponential = (adsr1 & 0x8000u) != 0u;
            increase = true;
            shift = (adsr1 >> 10u) & 0x1Fu;
            step = 7 - static_cast<int32_t>((adsr1 >> 8u) & 3u);
            break;
        case EnvelopePhase::Decay:
            exponential = true;
            shift = (adsr1 >> 4u) & 0xFu;
            step = -8;
            break;
        case EnvelopePhase::Sustain:
            exponential = (adsr2 & 0x8000u) != 0u;
            increase = (adsr2 & 0x4000u) == 0u;
            shift = (adsr2 >> 8u) & 0x1Fu;
            step = increase ? 7 - static_cast<int32_t>((adsr2 >> 6u) & 3u)
                            : -8 + static_cast<int32_t>((adsr2 >> 6u) & 3u);
            break;
        case EnvelopePhase::Release:
            exponential = (adsr2 & 0x20u) != 0u;
            shift = adsr2 & 0x1Fu;
            step = -8;
            break;
        case EnvelopePhase::Off:
            return;
        }

        uint32_t cycles = 1u << (shift > 11u ? shift - 11u : 0u);
        int32_t stepValue = step * (1 << (shift < 11u ? 11u - shift : 0u));
        if (exponential && increase && voice.envLevel > 0x6000)
            cycles *= 4u;
        if (exponential && !increase)
        {
            stepValue = (stepValue * voice.envLevel) / 0x8000;
            if (stepValue == 0)
                stepValue = -1;
        }
        if (++voice.envCounter < cycles)
            return;
        voice.envCounter = 0u;
        voice.envLevel = std::clamp(voice.envLevel + stepValue, 0, 0x7FFF);

        switch (voice.phase)
        {
        case EnvelopePhase::Attack:
            if (voice.envLevel >= 0x7FFF)
                voice.phase = EnvelopePhase::Decay;
            break;
        case EnvelopePhase::Decay:
        {
            const int32_t sustainLevel = (static_cast<int32_t>(adsr1 & 0xFu) + 1) << 11u;
            if (voice.envLevel <= sustainLevel)
                voice.phase = EnvelopePhase::Sustain;
            break;
        }
        case EnvelopePhase::Release:
            // At level 0 the voice stays in release, silent, but keeps playing: NAX keeps
            // moving through sound RAM (and can hit IRQA) until the next KON.
            if (voice.envLevel <= 0)
                voice.envLevel = 0;
            break;
        default:
            break;
        }
    }

    void Spu2::stepVoice(uint32_t core, uint32_t index)
    {
        Voice &voice = m_voices[core][index];
        if (!voice.running)
            return;
        const uint32_t base = core * 0x400u;
        const uint16_t pitchReg = reg(base + index * 0x10u + 4u);
        uint32_t pitch = std::min<uint32_t>(pitchReg, 0x3FFFu);
        if (index > 0u && ((voiceBits(m_regs, base + kPmon) >> index) & 1u) != 0u)
        {
            // Pitch modulation: the previous voice's output (-0x8000..0x7FFF) scales the
            // pitch by 0..2 (factor = OUTX + 0x8000, /0x8000); the product keeps 16 bits.
            const int32_t factor = static_cast<int32_t>(m_voices[core][index - 1u].outx) + 0x8000;
            const int32_t modulated = (static_cast<int32_t>(static_cast<int16_t>(pitchReg)) * factor) >> 15;
            pitch = std::min<uint32_t>(static_cast<uint32_t>(modulated) & 0xFFFFu, 0x3FFFu);
        }
        uint32_t counter = voice.pitchCounter + pitch;
        while (counter >= 0x1000u)
        {
            counter -= 0x1000u;
            if (voice.sampleIndex >= 28u)
            {
                if (!voice.fresh)
                {
                    if ((voice.blockFlags & 1u) != 0u)
                    {
                        // Loop end: jump to the loop address and set ENDX. Without the repeat
                        // bit the envelope is forced into release at level 0, but the voice
                        // keeps playing from the loop address (NAX moves, IRQA can still hit).
                        m_cores[core].endx |= 1u << index;
                        voice.nax = voice.lsa;
                        if ((voice.blockFlags & 2u) == 0u && voice.phase != EnvelopePhase::Release)
                        {
                            voice.envLevel = 0;
                            voice.envCounter = 0u;
                            voice.phase = EnvelopePhase::Release;
                            logEvent("end core=%u voice=%u nax=0x%05X", core, index, voice.nax, m_samples);
                        }
                        else if ((voice.blockFlags & 2u) == 0u)
                            voice.envLevel = 0;
                    }
                    else
                        voice.nax = (voice.nax + kBlockHalfwords) & kAddressMask;
                }
                fetchBlock(core, index, voice);
            }
            voice.prevSample = voice.lastSample;
            voice.lastSample = voice.samples[voice.sampleIndex++];
        }
        voice.pitchCounter = static_cast<uint16_t>(counter);
        stepEnvelope(core, index, voice);

        // The voice's output: linear interpolation between the last two decoded samples at
        // the pitch counter's fraction (the hardware uses a 4-point gaussian filter), or
        // the core's noise generator for a NON voice, times the envelope.
        int32_t sample;
        if (((voiceBits(m_regs, base + kNon) >> index) & 1u) != 0u)
            sample = static_cast<int16_t>(static_cast<uint16_t>(m_cores[core].noiseLevel));
        else
            sample = voice.prevSample + (((static_cast<int32_t>(voice.lastSample) - voice.prevSample) * static_cast<int32_t>(counter)) >> 12);
        voice.outx = clamp16((sample * voice.envLevel) >> 15);
        stepVolume(voice.volL, reg(base + index * 0x10u));
        stepVolume(voice.volR, reg(base + index * 0x10u + 2u));
        if (m_sink)
            m_sink(core, index, voice.lastSample);
    }

    void Spu2::stepNoise(uint32_t core)
    {
        // Noise clock in ATTR bits 8-13: shift bits 10-13, step 4 + bits 8-9; a 16-bit
        // shift register fed with the parity of bits 15, 12, 11 and 10 (inverted).
        Core &state = m_cores[core];
        const uint16_t value = attr(core);
        const uint32_t shift = (value >> 10u) & 0xFu;
        const int32_t step = 4 + static_cast<int32_t>((value >> 8u) & 3u);
        state.noiseTimer -= step;
        if (state.noiseTimer < 0)
        {
            const uint32_t level = state.noiseLevel;
            const uint32_t parity = ((level >> 15u) ^ (level >> 12u) ^ (level >> 11u) ^ (level >> 10u) ^ 1u) & 1u;
            state.noiseLevel = ((level << 1u) | parity) & 0xFFFFu;
            state.noiseTimer += static_cast<int32_t>(0x20000u >> shift);
            if (state.noiseTimer < 0)
                state.noiseTimer += static_cast<int32_t>(0x20000u >> shift);
        }
    }

    void Spu2::serviceAutoDma(uint32_t core)
    {
        Core &state = m_cores[core];
        Transfer &transfer = m_transfer[core];
        const uint32_t area = kAutoDmaArea + core * 0x400u;
        if (transfer.active && transfer.autoDma)
        {
            // The DMA moves the next 1 KB chunk (256 L then 256 R samples) into each half
            // the core has finished playing; the transfer ends once its last chunk is in.
            while (transfer.done < transfer.bytes && (state.admaFilled & (1u << state.admaNextHalf)) == 0u)
            {
                // A short last chunk keeps the layout: its first half is L, its second R.
                const uint32_t bytes = std::min(kAutoDmaChunkBytes, transfer.bytes - transfer.done);
                const uint32_t perChannel = bytes / 4u; // samples per channel in this chunk
                for (uint32_t channel = 0u; channel < 2u; ++channel)
                    for (uint32_t i = 0u; i < perChannel; ++i)
                    {
                        const uint32_t src = transfer.iopAddress + transfer.done + channel * perChannel * 2u + i * 2u;
                        const uint16_t value = src + 1u < m_iopRamSize
                                                   ? static_cast<uint16_t>(m_iopRam[src] | (m_iopRam[src + 1u] << 8u))
                                                   : 0u;
                        const uint32_t at = (area + channel * 0x200u + state.admaNextHalf * 0x100u + i) & kAddressMask;
                        m_ram[at] = value;
                        checkIrqRange(core, at, 1u);
                    }
                state.admaFilled |= 1u << state.admaNextHalf;
                state.admaNextHalf ^= 1u;
                transfer.done += bytes;
            }
            if (transfer.done >= transfer.bytes)
                completeTransfer(core);
        }
        // Play one sample of the input area (played halves keep their data: an underrun
        // replays it, as the hardware does). The core's reads check IRQA like a voice's.
        checkIrqRange(core, (area + state.admaPos) & kAddressMask, 1u);
        checkIrqRange(core, (area + 0x200u + state.admaPos) & kAddressMask, 1u);
        if (++state.admaPos == 256u)
            state.admaFilled &= ~1u;
        else if (state.admaPos >= 512u)
        {
            state.admaPos = 0u;
            state.admaFilled &= ~2u;
        }
    }

    void Spu2::mixSample()
    {
        int32_t externalL = 0;
        int32_t externalR = 0;
        for (uint32_t core = 0u; core < CoreCount; ++core)
        {
            Core &state = m_cores[core];
            const uint32_t base = core * 0x400u;
            const uint32_t vmixL = voiceBits(m_regs, base + kVmixL);
            const uint32_t vmixR = voiceBits(m_regs, base + kVmixR);
            const uint32_t vmixEL = voiceBits(m_regs, base + kVmixEL);
            const uint32_t vmixER = voiceBits(m_regs, base + kVmixER);
            int32_t dryL = 0;
            int32_t dryR = 0;
            int32_t wetVoiceL = 0;
            int32_t wetVoiceR = 0;
            for (uint32_t index = 0u; index < VoicesPerCore; ++index)
            {
                const Voice &voice = m_voices[core][index];
                if (!voice.running || voice.outx == 0)
                    continue;
                const uint32_t bit = 1u << index;
                if ((vmixL | vmixEL) & bit)
                {
                    const int32_t v = (voice.outx * voice.volL.value(reg(base + index * 0x10u))) >> 15;
                    if (vmixL & bit)
                        dryL += v;
                    if (vmixEL & bit)
                        wetVoiceL += v;
                }
                if ((vmixR | vmixER) & bit)
                {
                    const int32_t v = (voice.outx * voice.volR.value(reg(base + index * 0x10u + 2u))) >> 15;
                    if (vmixR & bit)
                        dryR += v;
                    if (vmixER & bit)
                        wetVoiceR += v;
                }
            }
            const uint16_t mmix = reg(base + kMmix);
            const uint32_t volumes = kVolumeBlock + core * kVolumeBlockStride;
            int32_t left = 0;
            int32_t right = 0;
            int32_t wetL = 0;
            int32_t wetR = 0;
            if (mmix & kMmixVoiceL)
                left += dryL;
            if (mmix & kMmixVoiceR)
                right += dryR;
            if (mmix & kMmixVoiceWetL)
                wetL += wetVoiceL;
            if (mmix & kMmixVoiceWetR)
                wetR += wetVoiceR;
            if (autoDmaEnabled(core))
            {
                // The sample just played by serviceAutoDma (position already advanced).
                const uint32_t pos = (state.admaPos + 511u) & 511u;
                const uint32_t area = kAutoDmaArea + core * 0x400u;
                const int32_t memL = (static_cast<int16_t>(m_ram[(area + pos) & kAddressMask]) * static_cast<int16_t>(reg(volumes + kBvol))) >> 15;
                const int32_t memR = (static_cast<int16_t>(m_ram[(area + 0x200u + pos) & kAddressMask]) * static_cast<int16_t>(reg(volumes + kBvol + 2u))) >> 15;
                if (mmix & kMmixMemL)
                    left += memL;
                if (mmix & kMmixMemR)
                    right += memR;
                if (mmix & kMmixMemWetL)
                    wetL += memL;
                if (mmix & kMmixMemWetR)
                    wetR += memR;
            }
            {
                const int32_t extL = (externalL * static_cast<int16_t>(reg(volumes + kAvol))) >> 15;
                const int32_t extR = (externalR * static_cast<int16_t>(reg(volumes + kAvol + 2u))) >> 15;
                if (mmix & kMmixExtL)
                    left += extL;
                if (mmix & kMmixExtR)
                    right += extR;
                if (mmix & kMmixExtWetL)
                    wetL += extL;
                if (mmix & kMmixExtWetR)
                    wetR += extR;
            }
            // The effects unit: its input is the wet sum, its output comes back times EVOL.
            int32_t fxL = 0;
            int32_t fxR = 0;
            if (reverbEnabledByEnv())
                Spu2Reverb::step(state.reverb, reverbParams(core), m_ram.data(), kAddressMask, wetL, wetR, fxL, fxR);
            left += (fxL * static_cast<int16_t>(reg(volumes + kEvol))) >> 15;
            right += (fxR * static_cast<int16_t>(reg(volumes + kEvol + 2u))) >> 15;
            left = clamp16(left);
            right = clamp16(right);
            state.outL = clamp16((left * state.mvolL.value(reg(volumes + kMvol))) >> 15);
            state.outR = clamp16((right * state.mvolR.value(reg(volumes + kMvol + 2u))) >> 15);
            // Core 0's output is core 1's external input.
            externalL = state.outL;
            externalR = state.outR;
        }
        m_outBuffer.push_back(m_cores[1].outL);
        m_outBuffer.push_back(m_cores[1].outR);
        ++m_outputFrames;
        if (m_outBuffer.size() >= OutputBatchFrames * 2u)
            flushOutput();
        if (mixLogEvery() != 0u && m_outputFrames % mixLogEvery() == 0u)
            logMixerState();
    }

    bool Spu2::reverbEnabledByEnv()
    {
        // PS2X_SPU2_REVERB=0: the reverb unit is left out of the mix (A/B level checks).
        static const bool s_enabled = [] {
            const char *v = std::getenv("PS2X_SPU2_REVERB");
            return !(v && v[0] == '0' && v[1] == 0);
        }();
        return s_enabled;
    }

    void Spu2::noteReverbRegisterWrite(uint32_t offset) noexcept
    {
        const uint32_t rel = offset & 0x3FFu;
        if (offset < kVolumeBlock && (rel == kAttr || (rel >= Spu2Reverb::EsaRegister && rel < kEndxLo)))
            m_reverbParamsDirty[(offset >> 10u) & 1u] = true;
        uint32_t core = 0u, volumeRel = 0u;
        if (volumeBlockRegister(offset, core, volumeRel) && volumeRel >= Spu2Reverb::CoefRegisters - kVolumeBlock)
            m_reverbParamsDirty[core] = true;
    }

    const Spu2Reverb::Params &Spu2::reverbParams(uint32_t core) noexcept
    {
        if (!m_reverbParamsDirty[core])
            return m_reverbParams[core];
        m_reverbParamsDirty[core] = false;
        Spu2Reverb::Params &params = m_reverbParams[core];
        const uint32_t base = core * 0x400u;
        params.enabled = (attr(core) & kAttrEffects) != 0u;
        params.start = composeAddress(base + Spu2Reverb::EsaRegister);
        params.end = ((static_cast<uint32_t>(reg(base + Spu2Reverb::EeaRegister)) << 16u) | 0xFFFFu) & kAddressMask;
        for (uint32_t i = 0u; i < Spu2Reverb::AddressCount; ++i)
            params.address[i] = composeAddress(base + Spu2Reverb::AddressRegisters + i * 4u);
        const uint32_t coefs = Spu2Reverb::CoefRegisters + core * kVolumeBlockStride;
        for (uint32_t i = 0u; i < Spu2Reverb::CoefCount; ++i)
            params.coef[i] = static_cast<int16_t>(reg(coefs + i * 2u));
        return params;
    }

    void Spu2::logSetupWrite(uint32_t offset, uint16_t value) const
    {
        // PS2X_SPU2_SETUP_LOG=N (1 = 4000 lines): writes to the mixer/effects setup registers
        // (per core 0x180-0x19F: PMON, NON, VMIX*, MMIX, ATTR, IRQA; 0x1B0 ADMAS; 0x2E0-0x33F:
        // effects area and reverb; the volume block 0x760-0x7AF), with the value written.
        static const int s_limit = [] {
            const char *v = std::getenv("PS2X_SPU2_SETUP_LOG");
            if (!v || !*v || *v == '0')
                return 0;
            const int n = std::atoi(v);
            return n > 1 ? n : 4000;
        }();
        static int s_lines = 0;
        if (s_lines >= s_limit)
            return;
        const uint32_t rel = offset & 0x3FFu;
        const bool setup = (offset >= kVolumeBlock && offset < kVolumeBlock + 2u * kVolumeBlockStride) ||
                           (offset < 0x760u && ((rel >= 0x180u && rel < 0x1A0u) || rel == kAdmas || (rel >= 0x2E0u && rel < 0x340u)));
        if (!setup)
            return;
        ++s_lines;
        std::fprintf(stderr, "[iop:spu2:setup] sample=%llu reg=0x%03X value=0x%04X\n", static_cast<unsigned long long>(m_samples), offset, value);
    }

    uint32_t Spu2::mixLogEvery()
    {
        // PS2X_SPU2_MIX_LOG=<ms>: the mixer state of both cores every <ms> of output.
        static const uint32_t s_every = [] {
            const char *value = std::getenv("PS2X_SPU2_MIX_LOG");
            const int ms = value ? std::atoi(value) : 0;
            return ms > 0 ? static_cast<uint32_t>(ms) * 48u : 0u;
        }();
        return s_every;
    }

    void Spu2::logMixerState() const
    {
        for (uint32_t core = 0u; core < CoreCount; ++core)
        {
            const uint32_t base = core * 0x400u;
            const uint32_t volumes = kVolumeBlock + core * kVolumeBlockStride;
            uint32_t running = 0u, audible = 0u, dry = 0u, wetOnly = 0u, zeroVol = 0u;
            for (uint32_t index = 0u; index < VoicesPerCore; ++index)
            {
                const Voice &voice = m_voices[core][index];
                if (!voice.running)
                    continue;
                ++running;
                if (voice.envLevel == 0)
                    continue;
                ++audible;
                const bool dryBit = (((voiceBits(m_regs, base + kVmixL) | voiceBits(m_regs, base + kVmixR)) >> index) & 1u) != 0u;
                const bool wetBit = (((voiceBits(m_regs, base + 0x18Cu) | voiceBits(m_regs, base + 0x194u)) >> index) & 1u) != 0u;
                dry += dryBit ? 1u : 0u;
                wetOnly += (!dryBit && wetBit) ? 1u : 0u;
                zeroVol += (voice.volL.value(reg(base + index * 0x10u)) == 0 && voice.volR.value(reg(base + index * 0x10u + 2u)) == 0) ? 1u : 0u;
            }
            std::fprintf(stderr,
                         "[iop:spu2:mix] out=%llu core=%u attr=%04X mmix=%03X mvol=%04X/%04X mvolx=%d/%d evol=%04X/%04X avol=%04X/%04X bvol=%04X/%04X "
                         "voices running=%u env>0=%u dry=%u wet_only=%u zero_vol=%u out=%d/%d\n",
                         static_cast<unsigned long long>(m_outputFrames), core, attr(core), reg(base + kMmix), reg(volumes), reg(volumes + 2u),
                         m_cores[core].mvolL.value(reg(volumes)), m_cores[core].mvolR.value(reg(volumes + 2u)), reg(volumes + 4u), reg(volumes + 6u),
                         reg(volumes + kAvol), reg(volumes + kAvol + 2u), reg(volumes + kBvol), reg(volumes + kBvol + 2u), running, audible, dry,
                         wetOnly, zeroVol, m_cores[core].outL, m_cores[core].outR);
        }
    }

    void Spu2::stepSample()
    {
        for (uint32_t core = 0u; core < CoreCount; ++core)
            stepNoise(core);
        if (m_runningVoices != 0u)
            for (uint32_t core = 0u; core < CoreCount; ++core)
                for (uint32_t index = 0u; index < VoicesPerCore; ++index)
                    stepVoice(core, index);
        for (uint32_t core = 0u; core < CoreCount; ++core)
            if (autoDmaEnabled(core))
                serviceAutoDma(core);
        if (m_output)
            mixSample();
        for (uint32_t core = 0u; core < CoreCount; ++core)
        {
            const uint32_t volumes = kVolumeBlock + core * kVolumeBlockStride;
            stepVolume(m_cores[core].mvolL, reg(volumes + kMvol));
            stepVolume(m_cores[core].mvolR, reg(volumes + kMvol + 2u));
        }
    }

    uint64_t Spu2::transferCompletionCycle(const Transfer &transfer) const noexcept
    {
        if (transfer.autoDma)
        {
            // Consumer-paced (serviceAutoDma): the next chance to move data is the next
            // sample if a half is free, else when the half being played is finished.
            const uint32_t core = static_cast<uint32_t>(&transfer - m_transfer.data()) & 1u;
            const Core &state = m_cores[core];
            if ((state.admaFilled & (1u << state.admaNextHalf)) == 0u)
                return m_sampleCycle + CyclesPerSample;
            return m_sampleCycle + static_cast<uint64_t>(256u - (state.admaPos & 0xFFu)) * CyclesPerSample;
        }
        const uint64_t samples = (transfer.bytes + DmaBytesPerSample - 1u) / DmaBytesPerSample;
        return transfer.startCycle + std::max<uint64_t>(samples, 1u) * CyclesPerSample;
    }

    void Spu2::startDma(uint32_t core, bool toSpu, uint32_t iopAddress, uint32_t bytes)
    {
        catchUp();
        core &= 1u;
        Transfer &transfer = m_transfer[core];
        if (transfer.active)
            completeTransfer(core); // a restart while busy: finish the old one first
        transfer.active = true;
        transfer.toSpu = toSpu;
        transfer.autoDma = toSpu && (reg(core * 0x400u + kAdmas) & (1u << core)) != 0u;
        transfer.iopAddress = iopAddress & 0x1FFFFFu;
        transfer.bytes = bytes & ~1u;
        transfer.done = 0u;
        transfer.target = m_cores[core].tsa;
        transfer.startCycle = m_sampleCycle;
        logEvent("dma core=%u bytes=%u tsa=0x%05X", core, transfer.bytes, transfer.target, m_samples);
        logEvent("    to_spu=%u iop=0x%06X admas=%u", toSpu ? 1u : 0u, transfer.iopAddress, transfer.autoDma ? 1u : 0u, m_samples);
        if (eventLogLimit() > 0 && m_iopRam && transfer.iopAddress + 16u <= m_iopRamSize)
        {
            uint32_t words[4];
            std::memcpy(words, m_iopRam + transfer.iopAddress, sizeof(words));
            logEvent("    iop bytes: %08X %08X %08X", words[0], words[1], words[2], m_samples);
        }
        if (transfer.bytes == 0u)
            completeTransfer(core);
    }

    void Spu2::completeTransfer(uint32_t core)
    {
        Transfer &transfer = m_transfer[core];
        if (!transfer.active)
            return;
        const uint32_t halfwords = transfer.bytes / 2u;
        if (transfer.toSpu)
        {
            if (transfer.autoDma)
            {
                // The data went into the input area chunk by chunk (serviceAutoDma); a
                // transfer cut short by clearing ADMAS drops the rest.
            }
            else
            {
                for (uint32_t i = 0u; i < halfwords; ++i)
                {
                    const uint32_t src = transfer.iopAddress + i * 2u;
                    const uint16_t value = src + 1u < m_iopRamSize
                                               ? static_cast<uint16_t>(m_iopRam[src] | (m_iopRam[src + 1u] << 8u))
                                               : 0u;
                    m_ram[(transfer.target + i) & kAddressMask] = value;
                }
                checkIrqRange(core, transfer.target, halfwords);
                m_cores[core].tsa = (transfer.target + halfwords) & kAddressMask;
            }
        }
        else
        {
            for (uint32_t i = 0u; i < halfwords; ++i)
            {
                const uint32_t dst = transfer.iopAddress + i * 2u;
                if (dst + 1u >= m_iopRamSize)
                    break;
                const uint16_t value = m_ram[(transfer.target + i) & kAddressMask];
                m_iopRam[dst] = static_cast<uint8_t>(value);
                m_iopRam[dst + 1u] = static_cast<uint8_t>(value >> 8u);
            }
            checkIrqRange(core, transfer.target, halfwords);
            m_cores[core].tsa = (transfer.target + halfwords) & kAddressMask;
        }
        transfer.active = false;
        m_pendingIrqs |= core == 0u ? IrqDmaCore0 : IrqDmaCore1;
        logEvent("dma-done core=%u bytes=%u tsa=0x%05X", core, transfer.bytes, m_cores[core].tsa, m_samples);
        if (eventLogLimit() > 0)
        {
            const uint32_t at = transfer.target & kAddressMask;
            logEvent("    spu halfwords at start: %04X %04X %04X", m_ram[at], m_ram[(at + 1u) & kAddressMask],
                     m_ram[(at + 2u) & kAddressMask], m_samples);
        }
    }

    void Spu2::progressTransfers(uint64_t now)
    {
        for (uint32_t core = 0u; core < CoreCount; ++core)
            if (m_transfer[core].active && !m_transfer[core].autoDma && transferCompletionCycle(m_transfer[core]) <= now)
                completeTransfer(core);
    }

    void Spu2::catchUp()
    {
        if (!m_clock)
            return;
        const uint64_t now = *m_clock;
        while (m_sampleCycle + CyclesPerSample <= now)
        {
            // Idle (no voice ever keyed on, no transfer, no AutoDMA input playing, no
            // mixer): nothing observable happens per sample, so skip ahead.
            const bool transferring = m_transfer[0].active || m_transfer[1].active;
            if (m_runningVoices == 0u && !transferring && !m_output && !autoDmaEnabled(0u) && !autoDmaEnabled(1u))
            {
                const uint64_t skipped = (now - m_sampleCycle) / CyclesPerSample;
                m_sampleCycle += skipped * CyclesPerSample;
                m_samples += skipped;
                break;
            }
            m_sampleCycle += CyclesPerSample;
            ++m_samples;
            stepSample();
            progressTransfers(m_sampleCycle);
        }
    }

    uint64_t Spu2::nextEventCycle(uint64_t fallback) const noexcept
    {
        uint64_t next = fallback;
        for (const Transfer &transfer : m_transfer)
            if (transfer.active)
                next = std::min(next, transferCompletionCycle(transfer));
        if (m_runningVoices != 0u)
            next = std::min(next, m_sampleCycle + kIdleWakeCycles);
        return next;
    }

    uint32_t Spu2::takePendingIrqs() noexcept
    {
        const uint32_t result = m_pendingIrqs;
        m_pendingIrqs = 0u;
        return result;
    }

    void Spu2::saveState(ps2x::StateWriter &w) const
    {
        w.beginSection("SPU2");
        w.podVector(m_ram);
        w.pod(m_regs);
        w.pod(m_voices);
        w.pod(m_cores);
        w.pod(m_transfer);
        w.u64(m_sampleCycle);
        w.u64(m_samples);
        w.u32(m_pendingIrqs);
        w.u32(m_runningVoices);
        w.endSection();
    }

    void Spu2::loadState(ps2x::StateReader &r)
    {
        r.beginSection("SPU2");
        r.podVector(m_ram);
        if (m_ram.size() != RamHalfwords)
            throw ps2x::StateError("SPU2 RAM size differs from this build");
        r.pod(m_regs);
        r.pod(m_voices);
        r.pod(m_cores);
        r.pod(m_transfer);
        m_sampleCycle = r.u64();
        m_samples = r.u64();
        m_pendingIrqs = r.u32();
        m_runningVoices = r.u32();
        r.endSection();
        m_reverbParamsDirty = {true, true};
    }
}
