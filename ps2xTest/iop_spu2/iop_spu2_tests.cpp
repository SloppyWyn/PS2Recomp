// Unit tests for the SPU2 timing model (ps2xIOP/src/emulator/core/iop_spu2.*) and its
// plumbing in IopMemory (register map at 0x1F900000, DMA channels 4/7).
#include "emulator/core/iop_memory.h"
#include "emulator/core/iop_spu2.h"
#include "emulator/core/iop_spu2_reverb.h"
#include "emulator/core/iop_call_wait.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using ps2x::iop::detail::IopMemory;
using ps2x::iop::detail::Spu2;

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

    constexpr uint64_t kSample = Spu2::CyclesPerSample;

    // One ADPCM block: shift 12, filter 0 (samples = sign-extended nibbles), the given
    // flags, nibble i = (i + seed) & 0xF.
    std::array<uint16_t, 8> makeBlock(uint8_t flags, uint32_t seed)
    {
        std::array<uint16_t, 8> block{};
        block[0] = static_cast<uint16_t>(12u | (static_cast<uint16_t>(flags) << 8u));
        for (uint32_t i = 0u; i < 28u; ++i)
            block[1u + i / 4u] |= static_cast<uint16_t>(((i + seed) & 0xFu) << ((i & 3u) * 4u));
        return block;
    }

    void putBlock(Spu2 &spu, uint32_t halfword, const std::array<uint16_t, 8> &block)
    {
        for (uint32_t i = 0u; i < 8u; ++i)
            spu.ram()[halfword + i] = block[i];
    }

    void setAddress(Spu2 &spu, uint32_t offset, uint32_t halfword)
    {
        spu.writeRegister(offset, static_cast<uint16_t>(halfword >> 16u));
        spu.writeRegister(offset + 2u, static_cast<uint16_t>(halfword & 0xFFFFu));
    }

    uint32_t getAddress(Spu2 &spu, uint32_t offset)
    {
        return (static_cast<uint32_t>(spu.readRegister(offset)) << 16u) | spu.readRegister(offset + 2u);
    }

    void advance(uint64_t &clock, Spu2 &spu, uint64_t samples)
    {
        clock += samples * kSample;
        spu.catchUp();
    }

    void testDecode()
    {
        const auto block = makeBlock(0u, 0u);
        int16_t prev1 = 0, prev2 = 0;
        std::array<int16_t, 28> out{};
        const uint8_t flags = Spu2::decodeBlock(block.data(), prev1, prev2, out);
        check(flags == 0u, "decode: flags");
        bool ok = true;
        for (uint32_t i = 0u; i < 28u; ++i)
        {
            const int expected = static_cast<int>(i & 0xFu) >= 8 ? static_cast<int>(i & 0xFu) - 16 : static_cast<int>(i & 0xFu);
            ok = ok && out[i] == expected;
        }
        check(ok, "decode: shift 12 filter 0 gives sign-extended nibbles");
        check(prev1 == out[27] && prev2 == out[26], "decode: history carried");

        // Filter 1 (60/64 of the previous sample), shift 0: a single top nibble of 1 gives
        // 0x1000, then zeros decay by 60/64 each step.
        std::array<uint16_t, 8> filtered{};
        filtered[0] = 0x0010u; // shift 0, filter 1
        filtered[1] = 0x0001u; // first nibble 1 -> 1 << 12
        prev1 = prev2 = 0;
        Spu2::decodeBlock(filtered.data(), prev1, prev2, out);
        check(out[0] == 0x1000 && out[1] == (0x1000 * 60 + 32) / 64, "decode: filter 1 applied");
    }

    void testNaxAndFlags()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        std::vector<uint8_t> iopRam(1024u);
        spu.bind(&clock, iopRam.data(), static_cast<uint32_t>(iopRam.size()));

        // Four blocks at 0x1000: plain, loop-start, plain, end+repeat.
        putBlock(spu, 0x1000u, makeBlock(0u, 1u));
        putBlock(spu, 0x1008u, makeBlock(4u, 2u));
        putBlock(spu, 0x1010u, makeBlock(0u, 3u));
        putBlock(spu, 0x1018u, makeBlock(3u, 4u));

        setAddress(spu, 0x1C0u, 0x1000u);     // SSA voice 0
        spu.writeRegister(0x004u, 0x1000u);   // pitch 1.0
        spu.writeRegister(0x006u, 0x000Fu);   // ADSR1: attack shift 0 step +7, sustain level max
        spu.writeRegister(0x008u, 0x0000u);   // ADSR2: sustain linear increase, release shift 0
        spu.writeRegister(0x1A0u, 0x0001u);   // KON voice 0
        check(getAddress(spu, 0x1C8u) == 0x1000u, "KON: NAX = SSA");
        check(spu.voice(0u, 0u).running, "KON: voice running");

        advance(clock, spu, 28u);
        check(getAddress(spu, 0x1C8u) == 0x1000u, "NAX stays on block 0 for 28 samples");
        advance(clock, spu, 1u);
        check(getAddress(spu, 0x1C8u) == 0x1008u, "NAX moves to block 1 on sample 29");
        check(getAddress(spu, 0x1C4u) == 0x1008u, "loop-start flag sets LSAX");
        advance(clock, spu, 28u * 3u);
        // Block 3 (end + repeat) finished: back to LSA, ENDX set, still running.
        check(getAddress(spu, 0x1C8u) == 0x1008u, "end+repeat: NAX = LSAX");
        check((spu.readRegister(0x340u) & 1u) != 0u, "end flag sets ENDX");
        check(spu.voice(0u, 0u).running, "end+repeat keeps the voice running");
        spu.writeRegister(0x340u, 0u);
        check(spu.readRegister(0x340u) == 0u, "ENDX cleared by a write");

        // Pitch 2.0: two samples per output sample, a block every 14.
        spu.writeRegister(0x004u, 0x2000u);
        const uint32_t before = getAddress(spu, 0x1C8u);
        advance(clock, spu, 14u);
        check(getAddress(spu, 0x1C8u) == before + 8u, "pitch 0x2000 advances a block per 14 samples");

        // Manual LSAX write wins over data loop-start flags until the next KON.
        setAddress(spu, 0x1C4u, 0x1018u);
        advance(clock, spu, 14u * 4u);
        check(getAddress(spu, 0x1C4u) == 0x1018u, "manual LSAX kept across a loop-start block");

        // End without repeat: jump to LSAX, ENDX set, envelope forced into release at 0, but
        // the voice keeps playing from the loop address: NAX moves on and IRQA can still hit.
        putBlock(spu, 0x1018u, makeBlock(1u, 5u));
        spu.writeRegister(0x340u, 0u);
        spu.writeRegister(0x004u, 0x1000u);
        spu.writeRegister(0x1A0u, 0x0001u); // KON again from 0x1000 (LSAX stays 0x1018 until a loop-start block)
        advance(clock, spu, 28u * 4u + 1u);
        check(spu.voice(0u, 0u).running, "end without repeat: the voice keeps playing");
        check(spu.voice(0u, 0u).phase == Spu2::EnvelopePhase::Release, "end without repeat: envelope in release");
        check(spu.readRegister(0x00Au) == 0u, "end without repeat: ENVX 0");
        check((spu.readRegister(0x340u) & 1u) != 0u, "end without repeat sets ENDX");
        check(getAddress(spu, 0x1C8u) == 0x1008u, "end without repeat: NAX = LSAX (block 1 loop start)");
        setAddress(spu, 0x19Cu, 0x1014u);   // IRQA inside block 2
        spu.writeRegister(0x19Au, 0x0040u); // IRQ enable
        (void)spu.takePendingIrqs();
        advance(clock, spu, 28u);
        check(getAddress(spu, 0x1C8u) == 0x1010u, "after the end, NAX keeps moving from the loop address");
        check(spu.readRegister(0x00Au) == 0u, "after the end, ENVX stays 0");
        check(spu.takePendingIrqs() == Spu2::IrqSpu2, "after the end, the silent voice still hits IRQA");
        spu.writeRegister(0x19Au, 0x0000u);
        spu.writeRegister(0x1A0u, 0x0001u);
        check(spu.readRegister(0x340u) == 0u, "KON clears the voice's ENDX bit");

        // ENDX halves: 0x340 holds voices 0-15, 0x342 voices 16-23; a write clears its half.
        putBlock(spu, 0x4000u, makeBlock(3u, 0u)); // end + repeat: ENDX every block
        setAddress(spu, 0x1C0u, 0x4000u);           // voice 0
        setAddress(spu, 0x1C0u + 17u * 0xCu, 0x4000u); // voice 17
        spu.writeRegister(0x004u, 0x1000u);
        spu.writeRegister(0x004u + 17u * 0x10u, 0x1000u);
        spu.writeRegister(0x1A0u, 0x0001u);
        spu.writeRegister(0x1A2u, 0x0002u);
        advance(clock, spu, 29u);
        check(spu.readRegister(0x340u) == 0x0001u && spu.readRegister(0x342u) == 0x0002u, "ENDX: both halves set");
        spu.writeRegister(0x004u, 0u);               // stop both voices moving
        spu.writeRegister(0x004u + 17u * 0x10u, 0u);
        spu.writeRegister(0x340u, 0u);
        check(spu.readRegister(0x340u) == 0u && spu.readRegister(0x342u) == 0x0002u, "ENDX: a write to 0x340 clears voices 0-15 only");
        spu.writeRegister(0x342u, 0x1234u);
        check(spu.readRegister(0x342u) == 0u, "ENDX: a write to 0x342 (any value) clears voices 16-23");
    }

    void testEnvelope()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        spu.bind(&clock, nullptr, 0u);
        putBlock(spu, 0x2000u, makeBlock(2u, 0u)); // repeat-only flags: no end, loops never matter
        setAddress(spu, 0x1C0u, 0x2000u);
        spu.writeRegister(0x004u, 0x1000u);
        spu.writeRegister(0x006u, 0x000Fu); // attack linear shift 0 step 7, sustain level 0xF
        spu.writeRegister(0x008u, 0x0000u); // release linear shift 0
        spu.writeRegister(0x1A0u, 0x0001u);
        advance(clock, spu, 1u);
        check(spu.readRegister(0x00Au) == 7 << 11, "attack: +7<<11 after one sample");
        advance(clock, spu, 4u);
        check(spu.readRegister(0x00Au) == 0x7FFF, "attack reaches 0x7FFF");
        advance(clock, spu, 100u);
        check(spu.readRegister(0x00Au) == 0x7FFF, "sustain holds at level 0x7FFF with sustain level max");
        spu.writeRegister(0x1A4u, 0x0001u); // KOFF
        advance(clock, spu, 1u);
        check(spu.readRegister(0x00Au) == 0x7FFF - (8 << 11), "release: -8<<11 after one sample");
        advance(clock, spu, 3u);
        check(spu.readRegister(0x00Au) == 0u, "release reaches 0");
        check(spu.voice(0u, 0u).running && spu.voice(0u, 0u).phase == Spu2::EnvelopePhase::Release,
              "at release 0 the voice stays in release (silent, still playing)");
        const uint32_t naxAtZero = getAddress(spu, 0x1C8u);
        advance(clock, spu, 28u);
        check(getAddress(spu, 0x1C8u) != naxAtZero, "NAX keeps moving after release 0");
        check(spu.readRegister(0x00Au) == 0u, "ENVX stays 0 after release 0");

        // Exponential release (shift 11: one step per sample, scaled by the level).
        spu.writeRegister(0x008u, 0x0020u | 0x0Bu);
        spu.writeRegister(0x1A0u, 0x0001u);
        advance(clock, spu, 10u);
        spu.writeRegister(0x1A4u, 0x0001u);
        advance(clock, spu, 2u);
        check(spu.voice(0u, 0u).envLevel < 0x7FFF, "exponential release decreases");
    }

    void testIrqa()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        spu.bind(&clock, nullptr, 0u);
        putBlock(spu, 0x3000u, makeBlock(0u, 0u));
        putBlock(spu, 0x3008u, makeBlock(0u, 0u));
        putBlock(spu, 0x3010u, makeBlock(3u, 0u));
        setAddress(spu, 0x19Cu, 0x300Cu);     // IRQA inside block 1
        spu.writeRegister(0x19Au, 0xC040u);   // core enable, unmute, IRQ enable
        setAddress(spu, 0x1C0u, 0x3000u);
        spu.writeRegister(0x004u, 0x1000u);
        spu.writeRegister(0x006u, 0x000Fu);
        spu.writeRegister(0x1A0u, 0x0001u);
        advance(clock, spu, 28u);
        check(spu.takePendingIrqs() == 0u, "no IRQ before block 1 is fetched");
        advance(clock, spu, 1u);
        check(spu.takePendingIrqs() == Spu2::IrqSpu2, "IRQ raised when the voice fetches the IRQA block");
        check((spu.readRegister(0x7C2u) & 4u) != 0u, "IRQINFO core 0 bit set");
        spu.writeRegister(0x19Au, 0xC000u);   // driver clears IRQ enable
        check((spu.readRegister(0x7C2u) & 4u) == 0u, "IRQINFO cleared with the IRQ enable bit");
        advance(clock, spu, 28u * 2u);
        check(spu.takePendingIrqs() == 0u, "no IRQ while disabled");

        // A FIFO write landing on IRQA also raises it.
        spu.writeRegister(0x19Au, 0xC040u);
        setAddress(spu, 0x1A8u, 0x300Cu);
        spu.writeRegister(0x1ACu, 0x1234u);
        spu.writeRegister(0x19Au, 0xC050u);   // transfer mode 1: FIFO flushed to TSA
        check(spu.ram()[0x300Cu] == 0x1234u, "FIFO write lands at TSA");
        check(spu.takePendingIrqs() == Spu2::IrqSpu2, "transfer hitting IRQA raises the IRQ");
    }

    void testDmaThroughIopMemory()
    {
        uint64_t clock = 0u;
        IopMemory memory;
        memory.bindClock(&clock);
        const uint32_t iopBuffer = 0x80000u;
        for (uint32_t i = 0u; i < 1024u; ++i)
            memory.write8(iopBuffer + i, static_cast<uint8_t>(i * 7u));

        memory.write16(0x1F9001A8u, 0x0001u); // TSA = 0x10800 halfwords
        memory.write16(0x1F9001AAu, 0x0800u);
        memory.write16(0x1F90019Au, 0xC020u); // DMA write mode
        memory.write32(0x1F8010C0u, iopBuffer);
        memory.write32(0x1F8010C4u, (16u << 16u) | 0x10u); // 16 blocks of 16 words = 1024 bytes
        memory.write32(0x1F8010C8u, 0x01000201u);
        check((memory.read32(0x1F8010C8u) & (1u << 24u)) != 0u, "CHCR start bit reads busy");
        check((memory.read16(0x1F900344u) & 0x80u) == 0u, "STATX bit 7 clear while the DMA runs");
        check((memory.read16(0x1F900344u) & 0x400u) != 0u, "STATX bit 10 set while the DMA runs");
        check(memory.spu2().takePendingIrqs() == 0u, "no DMA interrupt at start");
        const uint64_t expected = (1024u + Spu2::DmaBytesPerSample - 1u) / Spu2::DmaBytesPerSample;
        check(memory.spu2().nextEventCycle(UINT64_MAX) == expected * kSample, "DMA completion scheduled on the sample clock");
        clock = expected * kSample;
        memory.spu2().catchUp();
        check(memory.spu2().takePendingIrqs() == Spu2::IrqDmaCore0, "DMA completion raises channel 4");
        check((memory.read32(0x1F8010C8u) & (1u << 24u)) == 0u, "CHCR start bit cleared");
        check((memory.read16(0x1F900344u) & 0x80u) != 0u, "STATX bit 7 set once the DMA is done");
        check((memory.read16(0x1F900344u) & 0x400u) == 0u, "STATX bit 10 clear once the DMA is done");
        bool ok = true;
        for (uint32_t i = 0u; i < 512u; ++i)
        {
            const uint16_t expectedWord = static_cast<uint16_t>(static_cast<uint8_t>(i * 14u) | (static_cast<uint8_t>(i * 14u + 7u) << 8u));
            ok = ok && memory.spu2().ram()[0x10800u + i] == expectedWord;
        }
        check(ok, "DMA data copied to sound RAM at TSA");
        check(memory.read16(0x1F9001AAu) == 0x0800u, "TSA register keeps the written value");

        // Core 1 read-back DMA: sound RAM -> IOP RAM.
        memory.write16(0x1F9005A8u, 0x0001u);
        memory.write16(0x1F9005AAu, 0x0800u);
        memory.write16(0x1F90059Au, 0xC030u);
        memory.write32(0x1F801500u, 0x90000u);
        memory.write32(0x1F801504u, (4u << 16u) | 0x10u);
        memory.write32(0x1F801508u, 0x01000200u);
        clock += 2u * kSample;
        memory.spu2().catchUp();
        check(memory.spu2().takePendingIrqs() == Spu2::IrqDmaCore1, "core 1 DMA completion raises channel 7");
        check(memory.read8(0x90001u) == 7u, "DMA read copies sound RAM into IOP RAM");

        // Snapshot round trip keeps the voice state (plain blocks at 0x10800 first: the DMA
        // data above has flag bits set in its headers).
        for (uint32_t i = 0u; i < 16u; ++i)
            memory.spu2().ram()[0x10800u + i * 8u] = 0x000Cu;
        memory.write16(0x1F9001C0u, 0x0001u);
        memory.write16(0x1F9001C2u, 0x0800u);
        memory.write16(0x1F900004u, 0x1000u);
        memory.write16(0x1F900006u, 0x000Fu);
        memory.write16(0x1F9001A0u, 0x0001u);
        clock += 29u * kSample;
        memory.spu2().catchUp();
        ps2x::StateWriter writer;
        memory.saveState(writer);
        IopMemory restored;
        restored.bindClock(&clock);
        ps2x::StateReader reader(writer.data().data(), writer.data().size());
        restored.loadState(reader);
        check(restored.read16(0x1F9001CAu) == 0x0808u && restored.read16(0x1F9001C8u) == 0x0001u, "snapshot restores NAX");
        check(restored.spu2().voice(0u, 0u).running, "snapshot restores the running voice");
    }

    // AutoDMA data lands in the core's input area at 0x2000; a write there that covers IRQA
    // raises the SPU2 interrupt like any other transfer.
    void testAutoDmaIrqa()
    {
        uint64_t clock = 0u;
        IopMemory memory;
        memory.bindClock(&clock);
        const uint32_t iopBuffer = 0x80000u;
        for (uint32_t i = 0u; i < 1024u; ++i)
            memory.write8(iopBuffer + i, static_cast<uint8_t>(i));
        memory.write16(0x1F90019Cu, 0x0000u); // IRQA = 0x2010 (left half 0, word 0x10)
        memory.write16(0x1F90019Eu, 0x2010u);
        memory.write16(0x1F9001B0u, 0x0001u); // ADMAS core 0
        memory.write16(0x1F90019Au, 0xC060u); // DMA write mode, IRQ enable
        memory.write32(0x1F8010C0u, iopBuffer);
        memory.write32(0x1F8010C4u, (16u << 16u) | 0x10u); // 1024 bytes
        memory.write32(0x1F8010C8u, 0x01000201u);
        check((memory.read16(0x1F900344u) & 0x400u) != 0u, "AutoDMA: STATX bit 10 while transferring");
        clock = memory.spu2().nextEventCycle(UINT64_MAX);
        memory.spu2().catchUp();
        const uint32_t irqs = memory.spu2().takePendingIrqs();
        check((irqs & Spu2::IrqDmaCore0) != 0u, "AutoDMA completion raises channel 4");
        check((irqs & Spu2::IrqSpu2) != 0u, "AutoDMA write into the input area hits IRQA");
        check(memory.spu2().ram()[0x2010u] == static_cast<uint16_t>(0x20u | (0x21u << 8u)), "AutoDMA data at the input area");
        check((memory.read16(0x1F9007C2u) & 4u) != 0u, "AutoDMA IRQA: IRQINFO core 0 bit");
    }

    // ---- Mixer (output stage) ----

    struct FrameCollector
    {
        std::vector<int16_t> samples; // interleaved L/R
        void attach(Spu2 &spu)
        {
            spu.setOutputSink([this](const int16_t *frames, size_t count)
                              { samples.insert(samples.end(), frames, frames + count * 2u); });
        }
        [[nodiscard]] size_t frames() const { return samples.size() / 2u; }
        [[nodiscard]] int left(size_t i) const { return samples[i * 2u]; }
        [[nodiscard]] int right(size_t i) const { return samples[i * 2u + 1u]; }
    };

    // One ADPCM block with a given shift (filter 0): sample i = nibble((i * 5 + seed) & 0xF)
    // sign-extended, << 12 >> shift.
    std::array<uint16_t, 8> makeShiftBlock(uint8_t flags, uint32_t seed, uint32_t shift)
    {
        std::array<uint16_t, 8> block{};
        block[0] = static_cast<uint16_t>((shift & 0xFu) | (static_cast<uint16_t>(flags) << 8u));
        for (uint32_t i = 0u; i < 28u; ++i)
            block[1u + i / 4u] |= static_cast<uint16_t>(((i * 5u + seed) & 0xFu) << ((i & 3u) * 4u));
        return block;
    }

    int scale(int value, int volume) { return (value * volume) >> 15; }
    int clampS16(int value) { return value < -32768 ? -32768 : (value > 32767 ? 32767 : value); }

    // Keys a voice on with the envelope held at 0x7FFF (ENVX written after KON, attack to
    // max at once, sustain level max, sustain rising: the level never moves).
    void keyOnFullLevel(Spu2 &spu, uint32_t core, uint32_t voice, uint32_t ssa, uint16_t pitch)
    {
        const uint32_t c = core * 0x400u;
        setAddress(spu, c + 0x1C0u + voice * 0xCu, ssa);
        spu.writeRegister(c + voice * 0x10u + 4u, pitch);
        spu.writeRegister(c + voice * 0x10u + 6u, 0x000Fu);
        spu.writeRegister(c + voice * 0x10u + 8u, 0x0000u);
        spu.writeRegister(c + (voice < 16u ? 0x1A0u : 0x1A2u), static_cast<uint16_t>(1u << (voice & 15u)));
        spu.writeRegister(c + voice * 0x10u + 0xAu, 0x7FFFu);
    }

    // A known ADPCM block played at pitch 0x1000 on core 1: the output is the decoded
    // samples (one sample of interpolation delay), times envelope, VOLL/VOLR and MVOL.
    void testMixVoicePitch1000()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        std::vector<uint8_t> iopRam(1024u);
        spu.bind(&clock, iopRam.data(), static_cast<uint32_t>(iopRam.size()));
        const auto block0 = makeShiftBlock(0u, 3u, 1u);
        const auto block1 = makeShiftBlock(0u, 9u, 2u);
        putBlock(spu, 0x1000u, block0);
        putBlock(spu, 0x1008u, block1);
        std::vector<int> decoded;
        {
            int16_t p1 = 0, p2 = 0;
            std::array<int16_t, 28> out{};
            Spu2::decodeBlock(block0.data(), p1, p2, out);
            decoded.insert(decoded.end(), out.begin(), out.end());
            Spu2::decodeBlock(block1.data(), p1, p2, out);
            decoded.insert(decoded.end(), out.begin(), out.end());
        }
        FrameCollector out;
        out.attach(spu);
        spu.writeRegister(0x400u, 0x3FFFu);   // core 1 voice 0 VOLL: max (0x7FFE)
        spu.writeRegister(0x402u, 0x1000u);   // VOLR: a quarter (0x2000): panned left
        spu.writeRegister(0x588u, 0x0001u);   // VMIXL voice 0
        spu.writeRegister(0x590u, 0x0001u);   // VMIXR voice 0
        spu.writeRegister(0x598u, 0x0C00u);   // MMIX: voices dry L/R
        spu.writeRegister(0x788u, 0x3FFFu);   // core 1 MVOLL
        spu.writeRegister(0x78Au, 0x6000u);   // MVOLR: fixed, bit 14 = sign: 0x6000 -> -0x4000
        keyOnFullLevel(spu, 1u, 0u, 0x1000u, 0x1000u);
        advance(clock, spu, 50u);
        spu.flushOutput();
        check(out.frames() == 50u, "mixer: one stereo frame per sample");
        check(spu.readRegister(0x40Cu) == 0x7FFEu && spu.readRegister(0x40Eu) == 0x2000u, "VOLX reads the fixed volumes doubled");
        bool okL = true, okR = true;
        for (size_t k = 0u; k < 50u && k < out.frames(); ++k)
        {
            // Frame k: sample k + 1 of the voice; the interpolation lags one sample.
            const int sample = k >= 1u ? decoded[k - 1u] : 0;
            const int outx = scale(sample, 0x7FFF);
            const int left = scale(clampS16(scale(outx, 0x7FFE)), 0x7FFE);
            const int right = scale(clampS16(scale(outx, 0x2000)), -0x4000);
            okL = okL && out.left(k) == left;
            okR = okR && out.right(k) == right;
        }
        check(okL, "pitch 0x1000: left = decoded samples x envelope x VOLL x MVOLL");
        check(okR, "pitch 0x1000: right = decoded samples x VOLR (pan) x negative MVOLR");
        check(out.left(11u) != 0 && out.right(11u) != 0, "mixer output is not silent"); // decoded[10]: nibble 5

        // VMIX off for the right side: only the left carries the voice.
        spu.writeRegister(0x590u, 0x0000u);
        out.samples.clear();
        advance(clock, spu, 4u);
        spu.flushOutput();
        check(out.frames() == 4u && out.right(0u) == 0 && out.right(3u) == 0 && out.left(3u) != 0, "VMIXR off: right silent");
        // MMIX voice bits off: nothing.
        spu.writeRegister(0x598u, 0x0000u);
        out.samples.clear();
        advance(clock, spu, 4u);
        spu.flushOutput();
        check(out.left(3u) == 0 && out.right(3u) == 0, "MMIX voice bits off: silent");
    }

    // Core 0's output (after its MVOL) is core 1's external input (AVOL, MMIX bits 2-3).
    void testMixCoreRouting()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        std::vector<uint8_t> iopRam(1024u);
        spu.bind(&clock, iopRam.data(), static_cast<uint32_t>(iopRam.size()));
        // A constant block: shift 0, every nibble 4 -> 0x4000, filter 0.
        std::array<uint16_t, 8> block{};
        block[0] = 0x0300u; // shift 0, filter 0, end + repeat: loops on itself (LSAX below)
        for (uint32_t i = 1u; i < 8u; ++i)
            block[i] = 0x4444u;
        putBlock(spu, 0x2800u, block);
        setAddress(spu, 0x1C4u, 0x2800u); // LSAX voice 0 core 0
        FrameCollector out;
        out.attach(spu);
        spu.writeRegister(0x000u, 0x3FFFu); // core 0 voice 0 VOLL/VOLR max
        spu.writeRegister(0x002u, 0x3FFFu);
        spu.writeRegister(0x188u, 0x0001u); // VMIXL
        spu.writeRegister(0x190u, 0x0001u); // VMIXR
        spu.writeRegister(0x198u, 0x0800u); // core 0 MMIX: voices dry L only
        spu.writeRegister(0x760u, 0x1000u); // core 0 MVOLL: fixed 0x1000 = +0x2000 (a quarter)
        spu.writeRegister(0x762u, 0x3FFFu); // core 0 MVOLR
        spu.writeRegister(0x598u, 0x000Cu); // core 1 MMIX: external input L/R
        spu.writeRegister(0x790u, 0x4000u); // core 1 AVOLL: plain signed 16-bit, half
        spu.writeRegister(0x792u, 0x7FFFu); // core 1 AVOLR
        spu.writeRegister(0x788u, 0x3FFFu); // core 1 MVOLL/R
        spu.writeRegister(0x78Au, 0x3FFFu);
        keyOnFullLevel(spu, 0u, 0u, 0x2800u, 0x1000u);
        advance(clock, spu, 8u);
        spu.flushOutput();
        const int outx = scale(0x4000, 0x7FFF);
        const int core0L = scale(clampS16(scale(outx, 0x7FFE)), 0x2000);
        const int expectedL = scale(clampS16(scale(core0L, 0x4000)), 0x7FFE);
        check(out.frames() == 8u && out.left(5u) == expectedL, "core 0 output reaches core 1 through AVOLL and both MVOLs");
        check(out.right(5u) == 0, "core 0 MMIX without the right voice bit: right silent");
        spu.writeRegister(0x598u, 0x0000u); // core 1 ignores its external input
        out.samples.clear();
        advance(clock, spu, 2u);
        spu.flushOutput();
        check(out.left(1u) == 0, "core 1 MMIX external bits off: core 0 not heard");
    }

    // Sweeps: VOLX / MVOLX follow a linear sweep at the documented rate.
    void testVolumeSweep()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        std::vector<uint8_t> iopRam(1024u);
        spu.bind(&clock, iopRam.data(), static_cast<uint32_t>(iopRam.size()));
        putBlock(spu, 0x1000u, makeShiftBlock(3u, 0u, 12u));
        keyOnFullLevel(spu, 0u, 0u, 0x1000u, 0x1000u);
        spu.writeRegister(0x000u, static_cast<uint16_t>(0x8000u | (10u << 2u))); // linear increase, shift 10, step 7: +14/sample
        spu.writeRegister(0x760u, 0x3FFFu);                                     // MVOLL fixed 0x7FFE
        spu.writeRegister(0x760u, static_cast<uint16_t>(0x8000u | 0x2000u | (10u << 2u))); // linear decrease: -16/sample
        advance(clock, spu, 100u);
        check(spu.readRegister(0x00Cu) == 1400u, "VOLL linear sweep: VOLXL = 14 x samples");
        check(spu.readRegister(0x770u) == 0x7FFEu - 1600u, "MVOLL linear sweep down from the fixed level: MVOLXL");
        spu.writeRegister(0x000u, static_cast<uint16_t>(0x8000u | 0x1000u | (10u << 2u))); // negative phase
        advance(clock, spu, 1u);
        check(static_cast<int16_t>(spu.readRegister(0x00Cu)) == -1414, "sweep with the phase bit reads negative");
    }

    // AutoDMA memory input: 4 KB of PCM played at one stereo sample per output sample
    // through BVOL / MMIX bits 6-7 (core 0) into core 1; the DMA ends when the last chunk
    // goes in, i.e. after the first half of the input area has been played twice.
    void testAutoDmaInputMix()
    {
        uint64_t clock = 0u;
        IopMemory memory;
        memory.bindClock(&clock);
        Spu2 &spu = memory.spu2();
        const uint32_t iopBuffer = 0x80000u;
        auto pcmL = [](uint32_t n) { return static_cast<int16_t>(static_cast<int32_t>(n * 97u % 4001u) - 2000); };
        auto pcmR = [](uint32_t n) { return static_cast<int16_t>(1500 - static_cast<int32_t>(n * 31u % 3001u)); };
        for (uint32_t chunk = 0u; chunk < 4u; ++chunk)
            for (uint32_t i = 0u; i < 256u; ++i)
            {
                const uint32_t n = chunk * 256u + i;
                const uint32_t at = iopBuffer + chunk * 1024u;
                const uint16_t l = static_cast<uint16_t>(pcmL(n));
                const uint16_t r = static_cast<uint16_t>(pcmR(n));
                memory.write8(at + i * 2u, static_cast<uint8_t>(l));
                memory.write8(at + i * 2u + 1u, static_cast<uint8_t>(l >> 8u));
                memory.write8(at + 512u + i * 2u, static_cast<uint8_t>(r));
                memory.write8(at + 512u + i * 2u + 1u, static_cast<uint8_t>(r >> 8u));
            }
        FrameCollector out;
        out.attach(spu);
        memory.write16(0x1F900198u, 0x00C0u); // core 0 MMIX: memory input L/R
        memory.write16(0x1F90076Cu, 0x7FFFu); // core 0 BVOLL/R (memory input)
        memory.write16(0x1F90076Eu, 0x7FFFu);
        memory.write16(0x1F900760u, 0x3FFFu); // core 0 MVOL
        memory.write16(0x1F900762u, 0x3FFFu);
        memory.write16(0x1F900598u, 0x000Cu); // core 1 MMIX: external input
        memory.write16(0x1F900790u, 0x7FFFu); // core 1 AVOL (external input)
        memory.write16(0x1F900792u, 0x7FFFu);
        memory.write16(0x1F900788u, 0x3FFFu); // core 1 MVOL
        memory.write16(0x1F90078Au, 0x3FFFu);
        memory.write16(0x1F9001B0u, 0x0001u); // ADMAS core 0
        memory.write16(0x1F90019Au, 0xC020u); // DMA write mode
        memory.write32(0x1F8010C0u, iopBuffer);
        memory.write32(0x1F8010C4u, (64u << 16u) | 0x10u); // 4096 bytes
        memory.write32(0x1F8010C8u, 0x01000201u);
        uint32_t irqAt = 0u;
        for (uint32_t s = 1u; s <= 1100u && irqAt == 0u; ++s)
        {
            clock = s * kSample;
            spu.catchUp();
            if ((spu.takePendingIrqs() & Spu2::IrqDmaCore0) != 0u)
                irqAt = s;
        }
        check(irqAt == 513u, "AutoDMA 4 KB: DMA interrupt when the 4th chunk goes in (sample 513), got " + std::to_string(irqAt));
        clock = 1024u * kSample;
        spu.catchUp();
        spu.flushOutput();
        bool ok = out.frames() == 1024u;
        for (uint32_t n = 0u; ok && n < 1024u; ++n)
        {
            const int left = scale(clampS16(scale(scale(clampS16(scale(pcmL(n), 0x7FFF)), 0x7FFE), 0x7FFF)), 0x7FFE);
            const int right = scale(clampS16(scale(scale(clampS16(scale(pcmR(n), 0x7FFF)), 0x7FFE), 0x7FFF)), 0x7FFE);
            ok = out.left(n) == left && out.right(n) == right;
            if (!ok)
                std::printf("  frame %u: got %d/%d want %d/%d\n", n, out.left(n), out.right(n), left, right);
        }
        check(ok, "AutoDMA input: 1024 frames = the PCM stream in order, through BVOL, MVOL, AVOL, MVOL");
    }

    // AutoDMA edge cases: clearing ADMAS mid-transfer stops it with no DMA-done interrupt; a
    // short last chunk splits into L and R halves; the core's playback reads hit IRQA.
    void testAutoDmaEdges()
    {
        uint64_t clock = 0u;
        IopMemory memory;
        memory.bindClock(&clock);
        Spu2 &spu = memory.spu2();
        const uint32_t iopBuffer = 0x80000u;
        for (uint32_t i = 0u; i < 8192u; ++i)
            memory.write8(iopBuffer + i, static_cast<uint8_t>(i >> 1u)); // halfword n = (n & 0xFF) * 0x101
        memory.write16(0x1F9001B0u, 0x0001u);
        memory.write16(0x1F90019Au, 0xC020u);
        memory.write32(0x1F8010C0u, iopBuffer);
        memory.write32(0x1F8010C4u, (64u << 16u) | 0x10u); // 4096 bytes
        memory.write32(0x1F8010C8u, 0x01000201u);
        clock = 100u * kSample;
        spu.catchUp();
        check(spu.dmaActive(0u) && spu.takePendingIrqs() == 0u, "AutoDMA running, no interrupt yet");
        memory.write16(0x1F9001B0u, 0x0000u); // ADMAS off mid-transfer
        check(!spu.dmaActive(0u), "ADMAS cleared: the transfer stops");
        check((memory.read32(0x1F8010C8u) & (1u << 24u)) == 0u, "ADMAS cleared: CHCR no longer busy");
        clock = 2000u * kSample;
        spu.catchUp();
        check(spu.takePendingIrqs() == 0u, "ADMAS cleared mid-transfer: no DMA-done interrupt");

        // A 1536-byte transfer: chunk 0 full, chunk 1 = 512 bytes = 128 L then 128 R samples.
        memory.write16(0x1F9001B0u, 0x0001u);
        memory.write32(0x1F8010C4u, (24u << 16u) | 0x10u); // 1536 bytes
        memory.write32(0x1F8010C8u, 0x01000201u);
        clock += kSample;
        spu.catchUp();
        check((spu.takePendingIrqs() & Spu2::IrqDmaCore0) != 0u, "short transfer completes once its last chunk is in");
        const auto hw = [](uint32_t n) { return static_cast<uint16_t>((n & 0xFFu) * 0x101u); };
        check(spu.ram()[0x2100u] == hw(512u) && spu.ram()[0x217Fu] == hw(639u), "short chunk: first half -> L of half 1");
        check(spu.ram()[0x2300u] == hw(640u) && spu.ram()[0x237Fu] == hw(767u), "short chunk: second half -> R of half 1");

        // IRQA inside the input area, hit by the core's playback (data written long before).
        memory.write16(0x1F90019Cu, 0x0000u); // IRQA = 0x2050 (L, half 0, sample 0x50)
        memory.write16(0x1F90019Eu, 0x2050u);
        memory.write16(0x1F90019Au, 0xC060u); // IRQ enable
        (void)spu.takePendingIrqs();
        bool hit = false;
        for (uint32_t s = 0u; s < 512u && !hit; ++s)
        {
            clock += kSample;
            spu.catchUp();
            hit = (spu.takePendingIrqs() & Spu2::IrqSpu2) != 0u;
        }
        check(hit, "playback of the input area hits IRQA");
    }

    // Noise (NON) replaces the ADPCM data; pitch modulation (PMON) speeds a voice up by the
    // previous voice's output.
    void testNoiseAndPitchModulation()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        std::vector<uint8_t> iopRam(1024u);
        spu.bind(&clock, iopRam.data(), static_cast<uint32_t>(iopRam.size()));
        // Silent data (all zero, end+repeat looping on itself) for the noise voice.
        std::array<uint16_t, 8> silent{};
        silent[0] = 0x0300u;
        putBlock(spu, 0x3000u, silent);
        spu.writeRegister(0x19Au, 0x3F00u); // ATTR noise clock: fastest
        spu.writeRegister(0x184u, 0x0004u); // NON voice 2
        setAddress(spu, 0x1C4u + 2u * 0xCu, 0x3000u);
        keyOnFullLevel(spu, 0u, 2u, 0x3000u, 0x1000u);
        std::vector<int16_t> seen;
        for (int i = 0; i < 32; ++i)
        {
            advance(clock, spu, 1u);
            seen.push_back(spu.voice(0u, 2u).outx);
        }
        std::sort(seen.begin(), seen.end());
        const size_t distinct = static_cast<size_t>(std::unique(seen.begin(), seen.end()) - seen.begin());
        check(distinct >= 8u, "NON: a voice over silent data outputs varying noise");

        // PMON: voice 0 outputs a constant 0x7000; voice 1 (pitch 0x1000, PMON) then runs at
        // 0x1000 x (0x6FFF + 0x8000) / 0x8000 ~ 1.87x; 8 plain blocks each.
        uint64_t clock2 = 0u;
        Spu2 pm;
        pm.bind(&clock2, iopRam.data(), static_cast<uint32_t>(iopRam.size()));
        for (uint32_t b = 0u; b < 16u; ++b)
        {
            std::array<uint16_t, 8> constant{};
            constant[0] = 0x0000u;
            for (uint32_t i = 1u; i < 8u; ++i)
                constant[i] = 0x7777u;
            putBlock(pm, 0x4000u + b * 8u, constant);
            putBlock(pm, 0x5000u + b * 8u, constant);
        }
        pm.writeRegister(0x180u, 0x0002u); // PMON voice 1
        keyOnFullLevel(pm, 0u, 0u, 0x4000u, 0x1000u);
        keyOnFullLevel(pm, 0u, 1u, 0x5000u, 0x1000u);
        advance(clock2, pm, 100u);
        const uint32_t nax0 = pm.voice(0u, 0u).nax - 0x4000u;
        const uint32_t nax1 = pm.voice(0u, 1u).nax - 0x5000u;
        check(nax0 == 3u * 8u, "PMON: the plain voice is on block 3 after 100 samples");
        check(nax1 == 6u * 8u, "PMON: the modulated voice is on block 6 (~1.87x), got block " + std::to_string(nax1 / 8u));
    }

    // With no output sink nothing is mixed and idle time is still skipped.
    void testMixerOffByDefault()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        std::vector<uint8_t> iopRam(64u);
        spu.bind(&clock, iopRam.data(), static_cast<uint32_t>(iopRam.size()));
        check(!spu.outputEnabled(), "mixer off without a sink");
        advance(clock, spu, 1000u);
        check(spu.sampleCount() == 1000u, "idle samples counted");
        FrameCollector out;
        out.attach(spu);
        check(spu.outputEnabled(), "mixer on with a sink");
        advance(clock, spu, 700u);
        check(out.frames() == 512u, "frames handed over in batches of 512");
        spu.setOutputSink(nullptr);
        check(out.frames() == 700u, "turning the sink off hands over the rest");
    }

    // ---- Reverb (iop_spu2_reverb.*) ----
    using ps2x::iop::detail::Spu2Reverb;

    Spu2Reverb::Params reverbTestParams()
    {
        Spu2Reverb::Params p;
        p.enabled = true;
        p.start = 0x10000u;
        p.end = 0x100FFu; // 256 halfwords
        return p;
    }

    void testReverbFilter()
    {
        const auto &h = Spu2Reverb::filter();
        int32_t sum = 0;
        bool symmetric = true, halfBand = true;
        const uint32_t c = Spu2Reverb::Taps / 2u;
        for (uint32_t k = 0u; k < Spu2Reverb::Taps; ++k)
        {
            sum += h[k];
            symmetric = symmetric && h[k] == h[Spu2Reverb::Taps - 1u - k];
            const int d = static_cast<int>(k) - static_cast<int>(c);
            if (d != 0 && (d & 1) == 0)
                halfBand = halfBand && h[k] == 0;
        }
        check(sum == 0x8000, "reverb filter: DC gain exactly 1.0 (Q15)");
        check(symmetric, "reverb filter: symmetric (linear phase)");
        check(halfBand && h[c] > 0x3F00 && h[c] < 0x4100, "reverb filter: half-band (even taps zero, centre ~0.5)");
    }

    // The formula, one step at a time, against hand-computed values.
    void testReverbProcess()
    {
        std::vector<uint16_t> ram(Spu2::RamHalfwords, 0u);
        const uint32_t mask = Spu2::RamHalfwords - 1u;
        Spu2Reverb::Params p = reverbTestParams();
        Spu2Reverb::State s;
        s.position = p.start;
        using A = Spu2Reverb::Address;
        using C = Spu2Reverb::Coef;
        p.address[A::IirDestA0] = 0x10u;
        p.address[A::IirDestA1] = 0x20u;
        p.address[A::IirSrcA0] = 0x30u;
        p.address[A::IirSrcA1] = 0x31u;
        p.address[A::IirDestB0] = 0x40u;
        p.address[A::IirDestB1] = 0x50u;
        p.address[A::IirSrcB0] = 0x60u;
        p.address[A::IirSrcB1] = 0x61u;
        p.address[A::AccSrcA0] = 0x70u;
        p.address[A::AccSrcA1] = 0x71u;
        p.address[A::MixDestA0] = 0x80u;
        p.address[A::MixDestA1] = 0x88u;
        p.address[A::MixDestB0] = 0x90u;
        p.address[A::MixDestB1] = 0x98u;
        p.address[A::FbSrcA] = 2u;
        p.address[A::FbSrcB] = 3u;
        p.coef[C::InCoefL] = 0x4000;  // 0.5
        p.coef[C::InCoefR] = 0x2000;  // 0.25
        p.coef[C::IirAlpha] = 0x6000; // 0.75
        p.coef[C::IirCoef] = 0x4000;  // wall 0.5
        p.coef[C::AccCoefA] = 0x4000;
        p.coef[C::FbAlpha] = 0x2000;
        p.coef[C::FbX] = -0x4000;
        const uint32_t pos = p.start;
        auto at = [&](uint32_t off) -> int16_t & { return reinterpret_cast<int16_t &>(ram[(p.start + ((pos - p.start + off) & 0xFFu)) & mask]); };
        at(0x10u - 1u) = 100;  // previous L same-side sample
        at(0x30u) = 400;       // dLSAME tap
        at(0x60u) = -800;      // dRDIFF tap (feeds L diff)
        at(0x70u) = 1000;      // comb L
        at(0x71u) = -2000;     // comb R
        at(0x80u - 2u) = 300;  // APF1 L delayed
        at(0x90u - 3u) = 50;   // APF2 L delayed
        int32_t outL = 0, outR = 0;
        Spu2Reverb::process(s, p, ram.data(), mask, 2000, 4000, outL, outR);
        // Same side L: lin = 1000; ((1000 + 400*0.5 - 100) * 0.75) + 100 = 925
        check(at(0x10u) == 925, "reverb: same-side IIR (input, wall tap, previous sample)");
        // Same side R: rin = 1000; previous 0, tap 0: 1000 * 0.75 = 750
        check(at(0x20u) == 750, "reverb: right same-side uses IN_COEF_R");
        // Cross L: (1000 + (-800)*0.5 - 0) * 0.75 = 450
        check(at(0x40u) == 450, "reverb: cross-side IIR reads the other side's tap");
        // Comb L = 1000*0.5 = 500; APF1: 500 - 300*0.25 = 425 stored, out 425*0.25 + 300 = 406
        check(at(0x80u) == 425, "reverb: APF1 stores input - coef x delayed");
        // APF2: 406 - 50*(-0.5) = 431 stored, out 431*(-0.5) + 50 = -165 (floor: -216 + 50)
        check(at(0x90u) == 431, "reverb: APF2 stores its own value");
        check(outL == ((431 * -0x4000) >> 15) + 50, "reverb: output after both all-pass stages");
        // Comb R = -1000; APF1 R delayed 0 -> -1000 stored; out -1000*0.25 = -250; APF2 -> -250, out 125
        check(at(0x88u) == -1000 && outR == ((-250 * -0x4000) >> 15), "reverb: right channel through its own taps");
        check(s.position == p.start + 1u, "reverb: the buffer pointer advances one halfword per step");
        for (int i = 0; i < 255; ++i)
            Spu2Reverb::process(s, p, ram.data(), mask, 0, 0, outL, outR);
        check(s.position == p.start, "reverb: the pointer wraps from EEA's end back to ESA");
        bool inside = true;
        for (uint32_t i = 0u; i < Spu2::RamHalfwords && inside; ++i)
            if (ram[i] != 0u && (i < p.start || i > p.end))
                inside = false;
        check(inside, "reverb: every access stays inside [ESA, end]");

        // Disabled: nothing written, silent output.
        std::vector<uint16_t> clean(Spu2::RamHalfwords, 0u);
        Spu2Reverb::State s2;
        Spu2Reverb::Params off = p;
        off.enabled = false;
        Spu2Reverb::process(s2, off, clean.data(), mask, 30000, 30000, outL, outR);
        check(outL == 0 && outR == 0 && std::all_of(clean.begin(), clean.end(), [](uint16_t v) { return v == 0u; }),
              "reverb: effects disabled -> no buffer writes (clean buffer: silent)");
        check(s2.position == off.start + 1u, "reverb: effects disabled -> the pointer still advances");
        // Disabled still reads the buffer and outputs (only writes stop): a comb tap with data.
        s2.position = off.start;
        reinterpret_cast<int16_t &>(clean[off.start + 0x70u]) = 1000;
        Spu2Reverb::process(s2, off, clean.data(), mask, 0, 0, outL, outR);
        check(outL != 0 && clean[off.start + 0x80u] == 0u, "reverb: effects disabled -> output from the buffer, no APF write");
    }

    // A pure echo (one comb tap D steps behind the same-side write, all-pass stages as
    // 1-step delays) through the 48 kHz resampling: silence first, then the input's level.
    void testReverbEcho()
    {
        std::vector<uint16_t> ram(Spu2::RamHalfwords, 0u);
        const uint32_t mask = Spu2::RamHalfwords - 1u;
        Spu2Reverb::Params p = reverbTestParams();
        using A = Spu2Reverb::Address;
        using C = Spu2Reverb::Coef;
        const uint32_t delay = 40u; // reverb steps (80 output samples)
        // Each tap moves one halfword per step: left and right taps sit far apart (+0x800) so
        // one side's writes never land where the other reads.
        p.end = p.start + 0xFFFu;
        p.address[A::IirDestA0] = 0x80u;
        p.address[A::IirDestA1] = 0x880u;
        p.address[A::AccSrcA0] = 0x80u - delay;
        p.address[A::AccSrcA1] = 0x880u - delay;
        p.address[A::MixDestA0] = 0x100u;
        p.address[A::MixDestA1] = 0x900u;
        p.address[A::MixDestB0] = 0x180u;
        p.address[A::MixDestB1] = 0x980u;
        p.address[A::FbSrcA] = 1u;
        p.address[A::FbSrcB] = 1u;
        p.address[A::IirDestB0] = 0x400u; // cross-side writes somewhere harmless
        p.address[A::IirDestB1] = 0xC00u;
        p.coef[C::InCoefL] = 0x7FFF;
        p.coef[C::InCoefR] = 0x7FFF;
        p.coef[C::IirAlpha] = 0x7FFF;
        p.coef[C::AccCoefA] = 0x7FFF;
        Spu2Reverb::State s;
        int32_t outL = 0, outR = 0;
        int firstNonZero = -1;
        int32_t last = 0;
        for (int n = 0; n < 2000; ++n)
        {
            Spu2Reverb::step(s, p, ram.data(), mask, 10000, -10000, outL, outR);
            if (firstNonZero < 0 && outL != 0)
                firstNonZero = n;
            last = outL;
        }
        // Input FIR (19) + echo (2 * 42 steps incl. the two all-pass delays) + output FIR (19).
        check(firstNonZero >= 2 * static_cast<int>(delay) && firstNonZero < 2 * static_cast<int>(delay) + 2 * 19 + 8,
              "reverb echo: silent until the comb delay has passed, through both filters");
        check(last > 9900 && last <= 10000 && outR < -9900 && outR >= -10000,
              "reverb echo: steady state at the input level (L and R): " + std::to_string(last) + " " + std::to_string(outR) + " first " + std::to_string(firstNonZero));
    }

    // Wired into the mixer: a voice routed to the effects only (VMIXEL, MMIX wet bit) is heard
    // only through EVOL, with ATTR bit 7 set.
    void testReverbInMixer()
    {
        uint64_t clock = 0u;
        Spu2 spu;
        std::vector<uint8_t> iopRam(1024u);
        spu.bind(&clock, iopRam.data(), static_cast<uint32_t>(iopRam.size()));
        std::array<uint16_t, 8> block{};
        block[0] = 0x0300u; // constant 0x4000, loops on itself
        for (uint32_t i = 1u; i < 8u; ++i)
            block[i] = 0x4444u;
        putBlock(spu, 0x2800u, block);
        setAddress(spu, 0x5C4u, 0x2800u); // LSAX core 1 voice 0
        FrameCollector out;
        out.attach(spu);
        spu.writeRegister(0x400u, 0x3FFFu);
        spu.writeRegister(0x402u, 0x3FFFu);
        spu.writeRegister(0x58Cu, 0x0001u); // VMIXEL voice 0
        spu.writeRegister(0x594u, 0x0001u); // VMIXER voice 0
        spu.writeRegister(0x598u, 0x0300u); // MMIX: voices to effects L/R only
        spu.writeRegister(0x788u, 0x3FFFu); // MVOL
        spu.writeRegister(0x78Au, 0x3FFFu);
        // Work area 0x70000-0x7FFFF, the echo of testReverbEcho with a 16-step comb delay.
        setAddress(spu, 0x6E0u, 0x70000u);
        spu.writeRegister(0x73Cu, 0x0007u);
        auto addr = [&](uint32_t index, uint32_t value) { setAddress(spu, 0x6E4u + index * 4u, value); };
        using A = Spu2Reverb::Address;
        addr(A::IirDestA0, 0x80u);
        addr(A::IirDestA1, 0x880u);
        addr(A::AccSrcA0, 0x80u - 16u);
        addr(A::AccSrcA1, 0x880u - 16u);
        addr(A::MixDestA0, 0x100u);
        addr(A::MixDestA1, 0x900u);
        addr(A::MixDestB0, 0x180u);
        addr(A::MixDestB1, 0x980u);
        addr(A::IirDestB0, 0x400u);
        addr(A::IirDestB1, 0xC00u);
        addr(A::FbSrcA, 1u);
        addr(A::FbSrcB, 1u);
        const uint32_t coef = 0x774u + 0x28u;
        spu.writeRegister(coef + 2u * Spu2Reverb::IirAlpha, 0x7FFFu);
        spu.writeRegister(coef + 2u * Spu2Reverb::AccCoefA, 0x7FFFu);
        spu.writeRegister(coef + 2u * Spu2Reverb::InCoefL, 0x7FFFu);
        spu.writeRegister(coef + 2u * Spu2Reverb::InCoefR, 0x7FFFu);
        keyOnFullLevel(spu, 1u, 0u, 0x2800u, 0x1000u);
        advance(clock, spu, 300u);
        spu.flushOutput();
        check(out.frames() == 300u && out.left(299u) == 0, "reverb in mixer: effects off (ATTR bit 7 clear) -> wet-only voice silent");
        spu.writeRegister(0x59Au, 0x0080u); // ATTR: effects on
        out.samples.clear();
        advance(clock, spu, 300u);
        spu.flushOutput();
        check(out.left(299u) == 0, "reverb in mixer: EVOL 0 -> nothing returns");
        spu.writeRegister(0x78Cu, 0x4000u); // core 1 EVOLL: half
        spu.writeRegister(0x78Eu, 0x7FFFu); // core 1 EVOLR
        out.samples.clear();
        advance(clock, spu, 300u);
        spu.flushOutput();
        const int wet = scale(scale(0x4000, 0x7FFF), 0x7FFE); // OUTX x VOL
        const int expectL = scale(clampS16(scale(wet, 0x4000)), 0x7FFE);
        check(std::abs(out.left(299u) - expectL) <= expectL / 50 + 4, "reverb in mixer: the echo returns at EVOLL x MVOL");
        check(out.right(299u) > out.left(299u), "reverb in mixer: EVOLR applies to the right side");
        bool inArea = true;
        for (uint32_t i = 0u; i < Spu2::RamHalfwords && inArea; ++i)
            if (spu.ram()[i] != 0u && (i < 0x70000u || i > 0x7FFFFu) && (i < 0x2800u || i >= 0x2808u))
                inArea = false;
        check(inArea, "reverb in mixer: writes stay in the core's work area");
    }
    // ---- Blocking waits outside an IOP thread (core/iop_call_wait.h) ----
    using ps2x::iop::detail::CallWaitHooks;
    using ps2x::iop::detail::CallWaitResult;

    // Hooks over a bare Spu2: a core 0 DMA completion interrupt sets `flag`.
    struct DmaWaitRig
    {
        uint64_t clock = 0u;
        Spu2 spu;
        std::vector<uint8_t> iopRam = std::vector<uint8_t>(0x50000u);
        bool flag = false;
        bool reentrant = false;
        int services = 0;
        CallWaitHooks hooks;
        DmaWaitRig()
        {
            spu.bind(&clock, iopRam.data(), static_cast<uint32_t>(iopRam.size()));
            hooks.reentrant = [this] { return reentrant; };
            hooks.service = [this] {
                ++services;
                spu.catchUp();
                if (spu.takePendingIrqs() & Spu2::IrqDmaCore0)
                    flag = true;
            };
            hooks.now = [this] { return clock; };
            hooks.nextEvent = [this](uint64_t limit, uint64_t &next, bool &transfer) {
                if (!spu.dmaActive(0u))
                    return false;
                next = std::min(next, spu.nextEventCycle(limit));
                transfer = true;
                return true;
            };
            hooks.advanceTo = [this](uint64_t cycle) { clock = cycle; };
        }
    };

    void testCallContextWait()
    {
        constexpr uint64_t cap = 36864000u / 50u;
        {
            DmaWaitRig rig;
            rig.spu.startDma(0u, true, 0x100u, 1024u); // 4 samples at 256 bytes/sample
            const auto result = ps2x::iop::detail::waitInCallContext(rig.hooks, [&] { return rig.flag; }, cap);
            check(result == CallWaitResult::Ready && rig.flag, "call wait: satisfied by the DMA completion interrupt");
            check(rig.clock == 4u * kSample, "call wait: time moved exactly to the DMA completion");
        }
        {
            // A transfer longer than the cap (256 KB: 1024 samples, ~21 ms) is waited for: the
            // time is bounded by the transfer, not by the cap.
            DmaWaitRig rig;
            rig.spu.startDma(0u, true, 0x100u, 0x40000u);
            const auto result = ps2x::iop::detail::waitInCallContext(rig.hooks, [&] { return rig.flag; }, cap);
            check(result == CallWaitResult::Ready && rig.clock == 1024u * kSample && rig.clock > cap,
                  "call wait: a transfer longer than the cap is waited for to its completion");
        }
        {
            DmaWaitRig rig;
            rig.clock = 12345u;
            const auto result = ps2x::iop::detail::waitInCallContext(rig.hooks, [&] { return rig.flag; }, cap);
            check(result == CallWaitResult::NothingScheduled && rig.clock == 12345u,
                  "call wait: nothing scheduled -> gives up at once, time unchanged");
        }
        {
            DmaWaitRig rig;
            rig.reentrant = true;
            rig.spu.startDma(0u, true, 0x100u, 1024u);
            const auto result = ps2x::iop::detail::waitInCallContext(rig.hooks, [&] { return rig.flag; }, cap);
            check(result == CallWaitResult::Reentrant && rig.clock == 0u && rig.services == 0,
                  "call wait: re-entrant (interrupts being serviced) -> gives up at once, nothing serviced, time unchanged");
        }
        {
            DmaWaitRig rig;
            rig.flag = true;
            rig.reentrant = true;
            check(ps2x::iop::detail::waitInCallContext(rig.hooks, [&] { return rig.flag; }, cap) == CallWaitResult::Ready,
                  "call wait: already satisfied -> ready even when re-entrant");
        }
        {
            // Events keep coming (every 1000 cycles) without satisfying the wait: the cap ends it.
            CallWaitHooks hooks;
            uint64_t clock = 0u;
            int passes = 0;
            hooks.reentrant = [] { return false; };
            hooks.service = [&] { ++passes; };
            hooks.now = [&] { return clock; };
            bool transferEvents = false;
            hooks.nextEvent = [&](uint64_t, uint64_t &next, bool &transfer) {
                next = std::min(next, clock + 1000u);
                transfer = transferEvents;
                return true;
            };
            hooks.advanceTo = [&](uint64_t cycle) { clock = cycle; };
            const auto result = ps2x::iop::detail::waitInCallContext(hooks, [] { return false; }, 100000u);
            check(result == CallWaitResult::Cap && clock == 100000u && passes == 101, "call wait: endless non-transfer events stop at the cap");
            clock = 0u;
            passes = 0;
            transferEvents = true;
            const auto hard = ps2x::iop::detail::waitInCallContext(hooks, [] { return false; }, 100000u);
            check(hard == CallWaitResult::Cap && clock == 100000u * ps2x::iop::detail::TransferCapFactor,
                  "call wait: endless transfer events stop at the hard limit");
        }
    }
}

// The IOP heap must reuse freed blocks (first fit): ezmidi_driver allocates a 43 KB sector
// staging buffer per request and frees it; a bump cursor ran the heap dry after ~20
// requests and the driver then retried sceCdRead into address 0 forever (Wall 14).
void testHeapReuse()
{
    IopMemory memory;
    const uint32_t total = IopMemory::HeapLimit - IopMemory::HeapBase;
    const uint32_t first = memory.allocate(43008u);
    check(first == IopMemory::HeapBase, "first allocation at the heap base");
    const uint32_t keep = memory.allocate(0x1000u);
    check(keep == first + 43008u, "second allocation follows the first");
    check(memory.freeAllocation(first), "free the first block");
    check(memory.allocate(43008u) == first, "freed block reused (first fit)");
    bool ok = true;
    for (int i = 0; i < 200 && ok; ++i)
    {
        const uint32_t block = memory.allocate(43008u, 16u);
        ok = block != 0u && memory.freeAllocation(block);
    }
    check(ok, "200 allocate/free cycles never exhaust the heap");
    check(memory.maxFreeMemory() == total - 43008u - 0x1000u, "max free = the gap above the live blocks");
    check(memory.allocate(total, 16u) == 0u, "oversize allocation fails");
    check(memory.allocate(0x100u, 16u, IopMemory::HeapBase + 0x20000u) == IopMemory::HeapBase + 0x20000u, "fixed allocation");
    check(memory.allocate(0x20000u) != 0u, "allocation skips over the fixed block");
}

int main()
{
    testHeapReuse();
    testDecode();
    testNaxAndFlags();
    testEnvelope();
    testIrqa();
    testDmaThroughIopMemory();
    testAutoDmaIrqa();
    testMixVoicePitch1000();
    testMixCoreRouting();
    testVolumeSweep();
    testAutoDmaInputMix();
    testAutoDmaEdges();
    testNoiseAndPitchModulation();
    testMixerOffByDefault();
    testReverbFilter();
    testReverbProcess();
    testReverbEcho();
    testReverbInMixer();
    testCallContextWait();
    std::printf("iop_spu2 tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
