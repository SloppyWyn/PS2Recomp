#pragma once

// SPU2 timing model (docs/SPU2-MODEL.md): both cores, 24 voices each, 2 MB sound RAM,
// per-voice ADPCM block decoding with loop/end flags, pitch-rate NAX advance, ADSR
// envelope (ENVX), KON/KOFF/ENDX, the IRQA address interrupt, transfers to sound RAM
// (IOP DMA channels 4 and 7, the FIFO port, AutoDMA timing), all driven from the IOP
// cycle clock at 768 IOP cycles per 48 kHz sample. The output stage (docs/SPU2-MODEL.md,
// "Mixing") mixes the voices (VOLL/VOLR with sweeps, VMIXL/VMIXR), the AutoDMA memory
// input (BVOL) and core 0's output into core 1 (AVOL) through MMIX and MVOL into 48 kHz
// stereo, handed to an output sink in batches; with no sink nothing is mixed. Noise (NON),
// pitch modulation (PMON) and the reverb unit (iop_spu2_reverb.h; EVOL) are modelled.
// Written from the public register descriptions of the hardware, not from any emulator.

#include "ps2x/state_stream.h"
#include "iop_spu2_reverb.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ps2x::iop::detail
{
    class Spu2
    {
    public:
        static constexpr uint32_t RamBytes = 2u * 1024u * 1024u;
        static constexpr uint32_t RamHalfwords = RamBytes / 2u;
        static constexpr uint32_t RegisterBytes = 0x800u; // 0x1F900000..0x1F9007FF
        static constexpr uint64_t CyclesPerSample = 768u;  // 36.864 MHz / 48 kHz
        static constexpr uint32_t VoicesPerCore = 24u;
        static constexpr uint32_t CoreCount = 2u;
        // Transfer rate, in bytes per 48 kHz sample, of a plain DMA/FIFO transfer to sound
        // RAM (~12 MB/s; a 64 KB voice buffer lands in ~5 ms). AutoDMA is paced by the
        // core's input playback instead: a 1 KB chunk (256 stereo samples) per half of the
        // input area played, i.e. 4 bytes per sample.
        static constexpr uint32_t DmaBytesPerSample = 256u;

        // IOP interrupt lines the model raises (bits of takePendingIrqs()).
        static constexpr uint32_t IrqSpu2 = 1u << 0u;    // INTC 9
        static constexpr uint32_t IrqDmaCore0 = 1u << 1u; // DMA channel 4 (0x24)
        static constexpr uint32_t IrqDmaCore1 = 1u << 2u; // DMA channel 7 (0x28)

        enum class EnvelopePhase : uint8_t
        {
            Off = 0,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        // A volume register (VOLL/VOLR, MVOLL/MVOLR) and its current value (VOLX/MVOLX):
        // bit 15 clear = fixed, bits 0-14 a signed 15-bit volume (applied doubled); bit 15
        // set = sweep: bit 14 exponential, bit 13 decrease, bit 12 negative phase, bits 0-6
        // the rate (shift bits 2-6, step bits 0-1, as the ADSR rates), the magnitude moving
        // between 0 and 0x7FFF.
        struct VolumeState
        {
            int32_t level = 0;     // current magnitude 0..0x7FFF (sweep) or signed volume (fixed)
            uint32_t counter = 0;
            [[nodiscard]] int32_t value(uint16_t reg) const noexcept;
        };

        struct Voice
        {
            uint32_t ssa = 0;      // halfword addresses, block (8 halfword) aligned
            uint32_t lsa = 0;
            uint32_t nax = 0;
            bool manualLoop = false; // LSAX written since KON: data loop-start flags ignored
            bool running = false;
            bool fresh = false;      // keyed on, first block not fetched yet
            uint8_t blockFlags = 0;
            uint8_t sampleIndex = 28; // 28 = next sample needs a new block
            uint16_t pitchCounter = 0;
            int16_t prev1 = 0;
            int16_t prev2 = 0;
            std::array<int16_t, 28> samples{};
            EnvelopePhase phase = EnvelopePhase::Off;
            int32_t envLevel = 0;
            uint32_t envCounter = 0;
            int16_t lastSample = 0;
            int16_t prevSample = 0;  // the sample before lastSample (linear interpolation)
            int16_t outx = 0;        // sample x envelope: the voice's output before volume
            VolumeState volL;
            VolumeState volR;
        };

        struct Transfer
        {
            bool active = false;
            bool toSpu = true;
            bool autoDma = false;
            uint32_t iopAddress = 0;
            uint32_t bytes = 0;
            uint32_t done = 0;        // AutoDMA: bytes already moved into the input area
            uint32_t target = 0;      // halfword address in sound RAM (TSA at start)
            uint64_t startCycle = 0;
        };

        // Per-sample sink for a later output stage: (core, voice, decoded sample before
        // volume/ADSR). Null = nothing is called.
        using SampleSink = std::function<void(uint32_t core, uint32_t voice, int16_t sample)>;
        // Mixed output: interleaved 16-bit stereo frames at 48 kHz (core 1's output, which
        // carries core 0's), in batches of up to OutputBatchFrames.
        using OutputSink = std::function<void(const int16_t *frames, size_t frameCount)>;
        static constexpr size_t OutputBatchFrames = 512u;

        Spu2();

        void reset();
        // The IOP cycle counter the model follows, and the IOP RAM transfers copy from/to.
        void bind(const uint64_t *clock, uint8_t *iopRam, uint32_t iopRamSize) noexcept;
        void setSampleSink(SampleSink sink) { m_sink = std::move(sink); }
        // A sink turns the mixer on: every sample is then stepped (no idle skipping), mixed
        // and handed over. Null = no mixing (the timing side is the same either way).
        void setOutputSink(OutputSink sink);
        // Hands the frames mixed so far to the sink (also done every OutputBatchFrames).
        void flushOutput();
        [[nodiscard]] bool outputEnabled() const noexcept { return static_cast<bool>(m_output); }

        // Register access (offset = address - 0x1F900000, 16-bit, even). Both catch up first.
        [[nodiscard]] uint16_t readRegister(uint32_t offset);
        void writeRegister(uint32_t offset, uint16_t value);

        // IOP DMA channel start (core 0 = channel 4, core 1 = channel 7).
        void startDma(uint32_t core, bool toSpu, uint32_t iopAddress, uint32_t bytes);
        [[nodiscard]] bool dmaActive(uint32_t core) const noexcept { return m_transfer[core & 1u].active; }

        // Advances the model to the bound clock (whole samples only).
        void catchUp();
        // Earliest cycle at which something observable happens, or `fallback`.
        [[nodiscard]] uint64_t nextEventCycle(uint64_t fallback) const noexcept;
        // IOP interrupts raised since the last call (Irq* bits).
        [[nodiscard]] uint32_t takePendingIrqs() noexcept;

        // Snapshots.
        void saveState(ps2x::StateWriter &w) const;
        void loadState(ps2x::StateReader &r);

        // Inspection (tests, logs).
        [[nodiscard]] const Voice &voice(uint32_t core, uint32_t index) const noexcept { return m_voices[core & 1u][index % VoicesPerCore]; }
        [[nodiscard]] std::vector<uint16_t> &ram() noexcept { return m_ram; }
        [[nodiscard]] const std::vector<uint16_t> &ram() const noexcept { return m_ram; }
        [[nodiscard]] uint64_t sampleCount() const noexcept { return m_samples; }
        [[nodiscard]] uint16_t endx(uint32_t core) const noexcept;

        // ADPCM block decode (16 bytes at `block`, little-endian halfwords) into 28 samples;
        // prev1/prev2 carry the filter history. Returns the block's flag byte.
        static uint8_t decodeBlock(const uint16_t *block, int16_t &prev1, int16_t &prev2, std::array<int16_t, 28> &out) noexcept;

    private:
        struct Core
        {
            uint32_t irqa = 0;       // halfword address
            uint32_t tsa = 0;        // halfword address (transfer start, advances)
            uint32_t endx = 0;       // 24 bits
            uint32_t konLatch = 0;   // last written (readback)
            uint32_t koffLatch = 0;
            std::array<uint16_t, 32> fifo{};
            uint32_t fifoCount = 0;
            // AutoDMA memory input: the core plays its input area (L at 0x2000 + core * 0x400,
            // R 0x200 further, two halves of 256 samples each) at one sample per output
            // sample while its ADMAS bit is set; the DMA refills a half once it is played.
            uint32_t admaPos = 0;     // 0..511
            uint32_t admaFilled = 0;  // bit per half holding data not yet played
            uint32_t admaNextHalf = 0; // the half the DMA fills next
            // Noise generator (NON), clocked by ATTR bits 8-13.
            uint32_t noiseLevel = 0;
            int32_t noiseTimer = 0;
            VolumeState mvolL;
            VolumeState mvolR;
            int16_t outL = 0; // the core's last output sample
            int16_t outR = 0;
            Spu2Reverb::State reverb; // effects unit (runs only while mixing)
        };

        [[nodiscard]] uint16_t reg(uint32_t offset) const noexcept { return m_regs[(offset >> 1u) & 0x3FFu]; }
        void setReg(uint32_t offset, uint16_t value) noexcept { m_regs[(offset >> 1u) & 0x3FFu] = value; }
        [[nodiscard]] static bool isCoreRegister(uint32_t offset) noexcept { return (offset & 0x3FFu) >= 0x180u; }
        [[nodiscard]] uint16_t attr(uint32_t core) const noexcept { return reg(0x19Au + core * 0x400u); }
        [[nodiscard]] uint32_t transferMode(uint32_t core) const noexcept { return (attr(core) >> 4u) & 3u; }
        [[nodiscard]] bool irqEnabled(uint32_t core) const noexcept { return (attr(core) & 0x40u) != 0u; }
        [[nodiscard]] uint32_t composeAddress(uint32_t hiOffset) const noexcept;
        void raiseIrq(uint32_t core);
        void checkIrqRange(uint32_t core, uint32_t halfword, uint32_t count);
        void keyOn(uint32_t core, uint32_t index);
        void keyOff(uint32_t core, uint32_t index);
        void stepSample();
        void stepVoice(uint32_t core, uint32_t index);
        void stepEnvelope(uint32_t core, uint32_t index, Voice &voice);
        void fetchBlock(uint32_t core, uint32_t index, Voice &voice);
        void flushFifo(uint32_t core);
        void writeRam(uint32_t core, uint32_t halfword, uint16_t value);
        void progressTransfers(uint64_t now);
        void completeTransfer(uint32_t core);
        [[nodiscard]] uint16_t statx(uint32_t core) const noexcept;
        [[nodiscard]] uint64_t transferCompletionCycle(const Transfer &transfer) const noexcept;
        [[nodiscard]] bool autoDmaEnabled(uint32_t core) const noexcept { return (reg(core * 0x400u + 0x1B0u) & (1u << core)) != 0u; }
        void serviceAutoDma(uint32_t core);
        void stepNoise(uint32_t core);
        void mixSample();
        [[nodiscard]] const Spu2Reverb::Params &reverbParams(uint32_t core) noexcept;
        void noteReverbRegisterWrite(uint32_t offset) noexcept;
        static bool reverbEnabledByEnv();
        static uint32_t mixLogEvery();
        void logMixerState() const;
        static void stepVolume(VolumeState &state, uint16_t reg) noexcept;
        static void writeVolume(VolumeState &state, uint16_t reg) noexcept;
        void logSetupWrite(uint32_t offset, uint16_t value) const;

        const uint64_t *m_clock = nullptr;
        uint8_t *m_iopRam = nullptr;
        uint32_t m_iopRamSize = 0;
        std::vector<uint16_t> m_ram;
        std::array<uint16_t, 0x400> m_regs{};
        std::array<std::array<Voice, VoicesPerCore>, CoreCount> m_voices{};
        std::array<Core, CoreCount> m_cores{};
        std::array<Transfer, CoreCount> m_transfer{};
        uint64_t m_sampleCycle = 0; // cycle of the last processed sample edge
        uint64_t m_samples = 0;
        uint32_t m_pendingIrqs = 0;
        uint32_t m_runningVoices = 0;
        SampleSink m_sink;
        OutputSink m_output;
        std::vector<int16_t> m_outBuffer; // interleaved stereo, not part of the state
        uint64_t m_outputFrames = 0;      // frames mixed since the sink was set (not state)
        // The reverb registers decoded per core, rebuilt after a write to them (not state).
        std::array<Spu2Reverb::Params, CoreCount> m_reverbParams{};
        std::array<bool, CoreCount> m_reverbParamsDirty{true, true};
    };
}
