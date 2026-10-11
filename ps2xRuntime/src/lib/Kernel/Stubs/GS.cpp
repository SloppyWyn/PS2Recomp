#include "Common.h"
#include "GS.h"
#include "ps2_log.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/ee_scheduler.h"

namespace ps2_stubs
{
    namespace
    {
        uint64_t makeClearPrim(bool useContext2)
        {
            return static_cast<uint64_t>(GS_PRIM_SPRITE) |
                   (static_cast<uint64_t>(useContext2 ? 1u : 0u) << 9);
        }

        // Q = 1.0 like libgraph's sceGsSetDefClear: the GS keeps Q latched for later ST primitives.
        uint64_t makeClearRgbaq(uint32_t rgba)
        {
            return static_cast<uint64_t>(rgba) | 0x3f80000000000000ull;
        }

        // TEST after a clear, as libgraph's sceGsSetDefClear builds it: only ztest 0 means no test.
        uint64_t makeClearRestoreTest(uint32_t ztest)
        {
            return ztest == 0u ? 0x30000ull : ((static_cast<uint64_t>(ztest) & 3ull) << 17) | 0x10000ull;
        }

        uint64_t makeClearXyz(int32_t x, int32_t y)
        {
            return static_cast<uint64_t>(static_cast<uint16_t>(x << 4)) |
                   (static_cast<uint64_t>(static_cast<uint16_t>(y << 4)) << 16);
        }

        void seedGsClearPacket(GsClearMem &clear,
                               int32_t width,
                               int32_t height,
                               uint32_t rgba,
                               uint32_t ztest,
                               bool useContext2)
        {
            const int32_t offX = 0x800 - (width >> 1);
            const int32_t offY = 0x800 - (height >> 1);
            const uint64_t clearTest = makeTest(0u);
            const uint64_t restoreTest = makeClearRestoreTest(ztest);
            const uint64_t prim = makeClearPrim(useContext2);
            const uint64_t rgbaq = makeClearRgbaq(rgba);
            const uint64_t xyz0 = makeClearXyz(offX, offY);
            const uint64_t xyz1 = makeClearXyz(offX + width, offY + height);
            const uint64_t testReg = useContext2 ? GS_REG_TEST_2 : GS_REG_TEST_1;

            clear.testa = {clearTest, testReg};
            clear.prim = {prim, GS_REG_PRIM};
            clear.rgbaq = {rgbaq, GS_REG_RGBAQ};
            clear.xyz2a = {xyz0, GS_REG_XYZ2};
            clear.xyz2b = {xyz1, GS_REG_XYZ2};
            clear.testb = {restoreTest, testReg};
        }

        bool hasSeededGsClearPacket(const GsClearMem &clear)
        {
            return clear.rgbaq.reg == GS_REG_RGBAQ &&
                   clear.xyz2a.reg == GS_REG_XYZ2 &&
                   clear.xyz2b.reg == GS_REG_XYZ2;
        }

        struct GsTrailingArgs3
        {
            uint32_t arg0 = 0u;
            uint32_t arg1 = 0u;
            uint32_t arg2 = 0u;
        };

        // Arguments 5-7 of the libgraph setup calls. The EE ABI passes arguments 5-8 in
        // t0-t3 (regs 8-11); all-zero values are valid (e.g. ztest = 0, zpsm = PSMZ32).
        GsTrailingArgs3 decodeGsTrailingArgs3(R5900Context *ctx)
        {
            return {getRegU32(ctx, 8), getRegU32(ctx, 9), getRegU32(ctx, 10)};
        }

        void applyGsClearPacket(PS2Runtime *runtime, const GsClearMem &clear)
        {
            if (!runtime->syncCoreSubsystems() || !hasSeededGsClearPacket(clear))
            {
                return;
            }

            runtime->gs().writeRegister(static_cast<uint8_t>(clear.testa.reg & 0xFFu), clear.testa.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.prim.reg & 0xFFu), clear.prim.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.rgbaq.reg & 0xFFu), clear.rgbaq.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.xyz2a.reg & 0xFFu), clear.xyz2a.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.xyz2b.reg & 0xFFu), clear.xyz2b.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.testb.reg & 0xFFu), clear.testb.value);
        }

        void refreshPacketBuilderPendingCount(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr);
        void writePacketBuilderCurrent(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t currentAddr);

        void initPacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            const uint32_t baseAddr = getRegU32(ctx, 5);
            const uint32_t words[4] = {baseAddr, baseAddr, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
        }

        uint32_t terminatePacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return 0u;
            }

            const uint32_t zero = 0u;
            while ((currentAddr & 0xCu) != 0u)
            {
                writeGuestBytes(rdram,
                                runtime,
                                currentAddr,
                                reinterpret_cast<const uint8_t *>(&zero),
                                sizeof(zero));
                currentAddr += 4u;
            }

            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr + 8u,
                            reinterpret_cast<const uint8_t *>(&zero),
                            sizeof(zero));
            return currentAddr;
        }

        void resetPacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            uint32_t baseAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr + 4u, baseAddr))
            {
                setReturnU32(ctx, 0u);
                return;
            }

            const uint32_t words[4] = {baseAddr, baseAddr, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            setReturnU32(ctx, baseAddr);
        }

        bool tryReadQwordFromGuest(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint64_t &outQword)
        {
            uint32_t low = 0u;
            uint32_t high = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, addr, low) ||
                !tryReadWordFromGuest(rdram, runtime, addr + 4u, high))
            {
                return false;
            }

            outQword = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32u);
            return true;
        }

        void writeGuestU32(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint32_t value)
        {
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&value),
                            sizeof(value));
        }

        void writeGuestU64(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint64_t value)
        {
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&value),
                            sizeof(value));
        }

        void writeGuestVec128(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, __m128i value)
        {
            alignas(16) __m128i temp = value;
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&temp),
                            sizeof(temp));
        }

        void refreshPacketBuilderPendingCount(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr)
        {
            uint32_t currentAddr = 0u;
            uint32_t pendingCountAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
                !tryReadWordFromGuest(rdram, runtime, stateAddr + 8u, pendingCountAddr) ||
                pendingCountAddr == 0u ||
                currentAddr <= pendingCountAddr)
            {
                return;
            }

            uint32_t countWord = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, pendingCountAddr, countWord))
            {
                return;
            }

            const uint32_t deltaBytes = currentAddr - pendingCountAddr;
            uint32_t deltaQwords = 0u;
            if (deltaBytes >= 16u)
            {
                deltaQwords = (deltaBytes >> 4u) - 1u;
            }

            countWord = (countWord & 0xFFFF0000u) | (deltaQwords & 0xFFFFu);
            writeGuestU32(rdram, runtime, pendingCountAddr, countWord);
        }

        void writePacketBuilderCurrent(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t currentAddr)
        {
            writeGuestU32(rdram, runtime, stateAddr, currentAddr);
            refreshPacketBuilderPendingCount(rdram, runtime, stateAddr);
        }

        uint32_t reservePacketBuilderWords(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t wordCount)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return 0u;
            }

            const uint32_t reservedAddr = currentAddr;
            currentAddr += wordCount * 4u;
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
            return reservedAddr;
        }

        void alignPacketBuilderState(uint8_t *rdram,
                                     PS2Runtime *runtime,
                                     uint32_t stateAddr,
                                     uint32_t alignMode,
                                     uint32_t reserveWords)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return;
            }

            const uint32_t adjusted = (alignMode + 2u) & 31u;
            const uint32_t shift = (32u - adjusted) & 31u;
            const uint32_t lowMask = 0xFFFFFFFFu >> shift;
            const uint32_t alignedBase = currentAddr & ~lowMask;
            uint32_t targetAddr = alignedBase + (reserveWords << 2u);
            if (targetAddr < currentAddr)
            {
                targetAddr = (targetAddr + 1u) + lowMask;
            }

            const uint32_t zero = 0u;
            while (currentAddr < targetAddr)
            {
                writeGuestU32(rdram, runtime, currentAddr, zero);
                currentAddr += 4u;
            }
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
        }

        void openPacketGifTag(uint8_t *rdram,
                              R5900Context *ctx,
                              PS2Runtime *runtime,
                              uint32_t stateAddr,
                              uint32_t openAddrOffset)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return;
            }

            writeGuestVec128(rdram, runtime, currentAddr, GPR_VEC(ctx, 5));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
            writeGuestU32(rdram, runtime, stateAddr + openAddrOffset, currentAddr);
        }

        void closePacketGifTag(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t openAddrOffset)
        {
            uint32_t openAddr = 0u;
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr + openAddrOffset, openAddr) ||
                !tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
                openAddr == 0u)
            {
                return;
            }

            uint64_t tagValue = 0u;
            if (!tryReadQwordFromGuest(rdram, runtime, openAddr, tagValue))
            {
                return;
            }

            uint32_t packetQwords = ((currentAddr - openAddr) >> 3u) - 2u;
            const uint32_t flag = static_cast<uint32_t>((tagValue >> 58u) & 0x3u);
            if (flag != 1u)
            {
                packetQwords >>= 1u;
            }
            if (flag != 2u)
            {
                uint32_t nreg = static_cast<uint32_t>((tagValue >> 60u) & 0xFu);
                if (nreg == 0u)
                {
                    nreg = 16u;
                }
                packetQwords = (packetQwords + nreg - 1u) / nreg;
            }

            tagValue += static_cast<uint64_t>(packetQwords);
            writeGuestU32(rdram, runtime, stateAddr + openAddrOffset, 0u);
            writeGuestU64(rdram, runtime, openAddr, tagValue);

            while ((currentAddr & 0xCu) != 0u)
            {
                writeGuestU32(rdram, runtime, currentAddr, 0u);
                currentAddr += 4u;
            }
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
        }
    }

    void sceGifPkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint64_t dataValue = GPR_U64(ctx, 6);
        const uint64_t regValue = static_cast<uint64_t>(getRegU32(ctx, 5));
        writeGuestU64(rdram, runtime, currentAddr, dataValue);
        writeGuestU64(rdram, runtime, currentAddr + 8u, regValue);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
    }

    void sceGifPkAddGsData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        writeGuestU64(rdram, runtime, currentAddr, GPR_U64(ctx, 5));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 8u);
    }

    void sceGifPkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        closePacketGifTag(rdram, runtime, getRegU32(ctx, 4), 12u);
    }

    void sceGifPkCnt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t countValue = getRegU32(ctx, 5);
        const uint32_t extraValue = getRegU32(ctx, 6);
        const uint32_t tagWord = getRegU32(ctx, 7) | 0x10000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, 0u, countValue, extraValue};
        const uint32_t nextAddr = packetAddr + 16u;

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, nextAddr);
    }

    void sceGifPkEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t countValue = getRegU32(ctx, 5);
        const uint32_t extraValue = getRegU32(ctx, 6);
        const uint32_t tagWord = getRegU32(ctx, 7) | 0x70000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, countValue, extraValue, 0u};
        const uint32_t nextAddr = packetAddr + 16u;

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, nextAddr);
    }

    void sceGifPkInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        initPacketBuilderState(rdram, ctx, runtime);
    }

    void sceGifPkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        openPacketGifTag(rdram, ctx, runtime, getRegU32(ctx, 4), 12u);
    }

    void sceGifPkRef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t refAddr = getRegU32(ctx, 5) & 0x9FFFFFFFu;
        const uint32_t tagWord = getRegU32(ctx, 9) | getRegU32(ctx, 6) | 0x30000000u;
        const uint32_t extra0 = getRegU32(ctx, 7);
        const uint32_t extra1 = getRegU32(ctx, 8);
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, refAddr, extra0, extra1};

        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);
    }

    void sceGifPkRefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t dbp = getRegU32(ctx, 5) & 0xFFFFu;
        const uint32_t dpsm = getRegU32(ctx, 6) & 0xFFu;
        const uint32_t dbw = getRegU32(ctx, 7) & 0xFFFFu;
        uint32_t dataAddr = getRegU32(ctx, 8);
        uint32_t qwcRemaining = getRegU32(ctx, 9);
        const uint32_t dsax = getRegU32(ctx, 10);
        const uint32_t dsay = getRegU32(ctx, 11);
        const uint32_t width = readStackU32(rdram, ctx, 0);
        const uint32_t height = readStackU32(rdram, ctx, 8);

        // Open a 4-register A+D GIF tag and emit the GS load-image setup.
        {
            const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t words[4] = {0x10000000u, 0u, 0u, 0u};
            writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
            writeGuestBytes(rdram,
                            runtime,
                            packetAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);

            // Seed an open A+D tag (nloop=0, EOP clear): closePacketGifTag adds the true
            // appended qword count, so a pre-set nloop would double-count. Open variant
            // (not makeGiftagAplusD) because that always sets EOP on this chained tag.
            const uint64_t giftag[2] = {makeGiftagAplusDOpen(0u), 0xEULL};
            uint32_t currentAddr = packetAddr + 16u;
            writeGuestBytes(rdram, runtime, currentAddr, reinterpret_cast<const uint8_t *>(giftag), sizeof(giftag));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
            writeGuestU32(rdram, runtime, stateAddr + 12u, currentAddr);

            const uint64_t bitbltbuf =
                (static_cast<uint64_t>(dbp) << 32u) |
                (static_cast<uint64_t>(dbw & 0xFFu) << 48u) |
                (static_cast<uint64_t>(dpsm) << 56u);
            const uint64_t trxpos =
                (static_cast<uint64_t>(dsax) << 32u) |
                (static_cast<uint64_t>(dsay) << 48u);
            const uint64_t trxreg =
                static_cast<uint64_t>(width) |
                (static_cast<uint64_t>(height) << 32u);

            {
                uint32_t addr = 0u;
                if (!tryReadWordFromGuest(rdram, runtime, stateAddr, addr))
                {
                    return;
                }
                writeGuestU64(rdram, runtime, addr, bitbltbuf);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_BITBLTBUF));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, trxpos);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXPOS));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, trxreg);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXREG));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, 0u);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXDIR));
                addr += 16u;
                writePacketBuilderCurrent(rdram, runtime, stateAddr, addr);
                closePacketGifTag(rdram, runtime, stateAddr, 12u);
            }
        }

        while (qwcRemaining != 0u)
        {
            const uint32_t chunkQwc = std::min<uint32_t>(qwcRemaining, 32767u);

            const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t words[4] = {0x10000000u, 0u, 0u, 0u};
            writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
            writeGuestBytes(rdram,
                            runtime,
                            packetAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);

            const uint32_t reservedAddr = reservePacketBuilderWords(rdram, runtime, stateAddr, 4u);
            const bool isLastChunk = (chunkQwc == qwcRemaining);
            const uint64_t gifTag =
                static_cast<uint64_t>(chunkQwc) |
                (isLastChunk ? 0x0800000000008000ULL : 0x0800000000000000ULL);
            writeGuestU64(rdram, runtime, reservedAddr, gifTag);
            writeGuestU64(rdram, runtime, reservedAddr + 8u, 0u);

            const uint32_t refPacketAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t refWords[4] = {0x30000000u | chunkQwc, dataAddr & 0x9FFFFFFFu, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            refPacketAddr,
                            reinterpret_cast<const uint8_t *>(refWords),
                            sizeof(refWords));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, refPacketAddr + 16u);

            qwcRemaining -= chunkQwc;
            dataAddr += chunkQwc * 16u;
        }
    }

    void sceGifPkReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resetPacketBuilderState(rdram, ctx, runtime);
    }

    void sceGifPkReserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, reservePacketBuilderWords(rdram, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5)));
    }

    void sceGifPkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, terminatePacketBuilderState(rdram, ctx, runtime));
    }

    void sceGsExecLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        uint32_t srcAddr = getRegU32(ctx, 5);

        GsImageMem img{};
        if (!runtime || !runtime->syncCoreSubsystems() || !readGsImage(rdram, imgAddr, img))
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t rowBytes = bytesForPixels(img.psm, static_cast<uint32_t>(img.width));
        if (rowBytes == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t fbw = img.vram_width ? img.vram_width : std::max<uint32_t>(1, (img.width + 63) / 64);
        const uint32_t totalImageBytes = rowBytes * static_cast<uint32_t>(img.height);
        const uint32_t headerQwc = 6u;
        const uint32_t imageQwc = (totalImageBytes + 15u) / 16u;
        const uint32_t totalQwc = headerQwc + imageQwc;

        uint32_t pktAddr = runtime->guestMalloc(totalQwc * 16u, 16u);
        if (pktAddr == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint8_t *pkt = getMemPtr(rdram, pktAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (!pkt || !src)
        {
            runtime->guestFree(pktAddr);
            setReturnS32(ctx, -1);
            return;
        }

        // libgraph keeps the GS block address itself (sceGsSetDefLoadImage, game copy 0x10d570,
        // puts it straight into BITBLTBUF.DBP): no scaling.
        uint32_t dbp = static_cast<uint32_t>(img.vram_addr);
        uint32_t dsax = static_cast<uint32_t>(img.x);
        uint32_t dsay = static_cast<uint32_t>(img.y);

        // Full messy
        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
        q[0] = makeGiftagAplusD(4u);
        q[1] = 0xEULL;
        q[2] = (static_cast<uint64_t>(img.psm & 0x3Fu) << 24) | (static_cast<uint64_t>(1u) << 16) |
               (static_cast<uint64_t>(dbp & 0x3FFFu) << 32) | (static_cast<uint64_t>(fbw & 0x3Fu) << 48) |
               (static_cast<uint64_t>(img.psm & 0x3Fu) << 56);
        q[3] = 0x50ULL;
        q[4] = (static_cast<uint64_t>(dsay & 0x7FFu) << 48) | (static_cast<uint64_t>(dsax & 0x7FFu) << 32);
        q[5] = 0x51ULL;
        q[6] = (static_cast<uint64_t>(img.height) << 32) | static_cast<uint64_t>(img.width);
        q[7] = 0x52ULL;
        q[8] = 0ULL;
        q[9] = 0x53ULL;
        q[10] = (static_cast<uint64_t>(2) << 58) | (static_cast<uint64_t>(imageQwc) & 0x7FFF) |
                (1ULL << 15);
        q[11] = 0ULL;

        std::memcpy(pkt + headerQwc * 16u, src, totalImageBytes);

        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
        auto &mem = runtime->memory();
        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
        mem.writeIORegister(GIF_CHANNEL + 0x20u, totalQwc & 0xFFFFu);
        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
        mem.processPendingTransfers();
        runtime->guestFree(pktAddr);

        setReturnS32(ctx, 0);
    }

    void sceGsExecStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        uint32_t dstAddr = getRegU32(ctx, 5);

        GsImageMem img{};
        if (!runtime || !runtime->syncCoreSubsystems() || !readGsImage(rdram, imgAddr, img))
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t rowBytes = bytesForPixels(img.psm, static_cast<uint32_t>(img.width));
        if (rowBytes == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t fbw = img.vram_width ? img.vram_width : std::max<uint32_t>(1, (img.width + 63) / 64);
        const uint32_t totalImageBytes = rowBytes * static_cast<uint32_t>(img.height);

        uint8_t *dst = getMemPtr(rdram, dstAddr);
        if (!dst)
        {
            setReturnS32(ctx, -1);
            return;
        }

        // libgraph keeps the GS block address itself (sceGsSetDefStoreImage, game copy 0x10d758,
        // puts it straight into BITBLTBUF.SBP): no scaling.
        uint32_t sbp = static_cast<uint32_t>(img.vram_addr);
        uint64_t bitbltbuf = (static_cast<uint64_t>(sbp & 0x3FFFu) << 0) |
                             (static_cast<uint64_t>(fbw & 0x3Fu) << 16) |
                             (static_cast<uint64_t>(img.psm & 0x3Fu) << 24) |
                             (static_cast<uint64_t>(0u) << 32) |
                             (static_cast<uint64_t>(1u) << 48) |
                             (static_cast<uint64_t>(0u) << 56);
        uint64_t trxpos = (static_cast<uint64_t>(img.x & 0x7FFu) << 0) |
                          (static_cast<uint64_t>(img.y & 0x7FFu) << 16) |
                          (static_cast<uint64_t>(0u) << 32) |
                          (static_cast<uint64_t>(0u) << 48);
        uint64_t trxreg = static_cast<uint64_t>(img.height) << 32 | static_cast<uint64_t>(img.width);

        uint32_t pktAddr = runtime->guestMalloc(80u, 16u);
        if (pktAddr == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint8_t *pkt = getMemPtr(rdram, pktAddr);
        if (!pkt)
        {
            runtime->guestFree(pktAddr);
            setReturnS32(ctx, -1);
            return;
        }

        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
        q[0] = makeGiftagAplusD(4u);
        q[1] = 0xEULL;
        q[2] = bitbltbuf;
        q[3] = 0x50ULL;
        q[4] = trxpos;
        q[5] = 0x51ULL;
        q[6] = trxreg;
        q[7] = 0x52ULL;
        q[8] = 1ULL;
        q[9] = 0x53ULL;

        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
        auto &mem = runtime->memory();
        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
        mem.writeIORegister(GIF_CHANNEL + 0x20u, 5u);
        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
        mem.processPendingTransfers();

        ps2TraceGuestRangeWrite(rdram, dstAddr, totalImageBytes, "sceGsExecStoreImage", ctx);
        runtime->gs().consumeLocalToHostBytes(dst, totalImageBytes);
        runtime->guestFree(pktAddr);

        setReturnS32(ctx, 0);
    }

    // Helpers that follow SDK 3.1.0 libgraph field by field (checked against the copies in
    // SLPS-25887: sceGsSetDefDispEnv 0x10c738, sceGszbufaddr 0x10cb70, sceGsSetDefDrawEnv
    // 0x10cc38, sceGsSetDefClear 0x10ce20, sceGsSetDefDBuff 0x10cf28).
    namespace
    {
        constexpr uint32_t kDBuffGiftag0Offset = 0x50u;
        constexpr uint32_t kDBuffGiftag1Offset = 0x140u;

        // ---- libgraph's graphics mode (sceGsGParam) ----
        // libgraph keeps it in one .data variable that only sceGsGetGParam hands out (a 12-byte
        // getter, `lui rt, hi; jr ra; addiu v0, rt, lo`, HI16/LO16 relocations at +0 and +8 in
        // the SDK symbol database), and every libgraph function reads it through that getter.
        // In SLPS-25887 the getter is 0x10c610 and the variable 0x3f3ca0; the game's own code
        // (not libgraph) reads it too, so the runtime must keep the guest's copy, not one of its
        // own. g_gparam is the host mirror the libgraph helpers below read.
        GsGParam g_gparam{1, 2, 1, 3}; // until sceGsResetGraph: interlaced NTSC, frame mode
        uint32_t g_gparamVar = 0u;        // libgraph's own variable, once located (0 = not yet)
        uint32_t g_gparamBlock = 0u;      // the runtime's block, once sceGsGetGParam handed one out
        bool g_gparamResetDone = false;   // sceGsResetGraph (mode 0/5) has set the mirror
        const PS2Runtime *g_gparamRuntime = nullptr;

        bool readGuestInsn(uint8_t *rdram, uint32_t addr, uint32_t &word)
        {
            if ((addr & 3u) != 0u)
                return false;
            const uint8_t *p = getConstMemPtr(rdram, addr);
            if (!p)
                return false;
            std::memcpy(&word, p, sizeof(word));
            return true;
        }

        // `jal target` at pc: true and the target.
        bool decodeJal(uint32_t word, uint32_t pc, uint32_t &target)
        {
            if ((word >> 26) != 3u)
                return false;
            target = ((pc + 4u) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
            return true;
        }

        // sceGsGetGParam's body: `lui rt, hi; jr ra; addiu v0, rt, lo`. Returns the variable.
        uint32_t decodeGParamGetter(uint8_t *rdram, uint32_t fn)
        {
            uint32_t w0 = 0, w1 = 0, w2 = 0;
            if (!readGuestInsn(rdram, fn, w0) || !readGuestInsn(rdram, fn + 4u, w1) ||
                !readGuestInsn(rdram, fn + 8u, w2))
                return 0u;
            const uint32_t rt = (w0 >> 16) & 31u;
            const bool lui = (w0 >> 26) == 0x0Fu && ((w0 >> 21) & 31u) == 0u;
            const bool jrRa = w1 == 0x03E00008u;
            const bool addiuV0 = (w2 >> 26) == 0x09u && ((w2 >> 21) & 31u) == rt && ((w2 >> 16) & 31u) == 2u;
            if (!lui || !jrRa || !addiuV0)
                return 0u;
            const uint32_t addr = ((w0 & 0xFFFFu) << 16) + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w2 & 0xFFFFu)));
            return getConstMemPtr(rdram, addr) ? addr : 0u;
        }

        // The guest function this stub stands in for. Not ctx->pc: the caller does set
        // ctx->pc = target before every jal/j/jalr (control_flow_emitter.cpp emitStaticJump /
        // emitRegisterJump), but the recompiler's stub wrapper then runs `ctx->pc =
        // getRegU32(ctx, 31)` before calling the handler (ps2_recompiler.cpp, generated e.g. as
        // FUN_0010c480_0x10c480), so here ctx->pc == ra. Instead, the call instruction at ra - 8:
        // `jal target`, or `jalr rs` with rs still holding the target (the wrapper touches no
        // GPR; refused when the delay slot may write rs). A tail call through j/jr is not seen:
        // 0. Callers check what they find against a fixed instruction pattern anyway.
        uint32_t calledGuestFunction(uint8_t *rdram, R5900Context *ctx)
        {
            if (!ctx)
                return 0u;
            const uint32_t ra = getRegU32(ctx, 31);
            uint32_t word = 0, target = 0;
            if (ra < 8u || !readGuestInsn(rdram, ra - 8u, word))
                return 0u;
            if (decodeJal(word, ra - 8u, target))
                return target;
            const bool jalr = (word >> 26) == 0u && (word & 0x3Fu) == 0x09u;
            if (!jalr)
                return 0u;
            const uint32_t rs = (word >> 21) & 31u;
            uint32_t slot = 0;
            if (rs == 0u || !readGuestInsn(rdram, ra - 4u, slot))
                return 0u;
            const uint32_t op = slot >> 26;
            const bool slotWritesRs = slot != 0u &&
                                      ((op == 0u && ((slot >> 11) & 31u) == rs) ||           // SPECIAL: rd
                                       (op != 0u && op != 2u && op != 3u && ((slot >> 16) & 31u) == rs)); // I-type: rt
            return slotWritesRs ? 0u : getRegU32(ctx, static_cast<int>(rs));
        }

        // From a call to sceGsResetGraph: libgraph's body (SDK 2.x-3.1, 400 bytes) calls
        // sceGsGetGParam with its first jal (+80 or +108 by the symbol database; +0x6c at 0x10c4ec
        // in SLPS-25887) and loads the GS privileged register base (lui 0x1200). Both must hold,
        // so a stray jal before the return address cannot pick some other getter.
        uint32_t locateGParamFromResetGraph(uint8_t *rdram, uint32_t fn)
        {
            if (fn == 0u)
                return 0u;
            uint32_t getterVar = 0u;
            bool firstJalSeen = false;
            bool gsBase = false;
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t word = 0, target = 0;
                const uint32_t pc = fn + i * 4u;
                if (!readGuestInsn(rdram, pc, word))
                    return 0u;
                if ((word >> 26) == 0x0Fu && (word & 0xFFFFu) == 0x1200u)
                    gsBase = true;
                if (!firstJalSeen && decodeJal(word, pc, target))
                {
                    firstJalSeen = true;
                    getterVar = decodeGParamGetter(rdram, target);
                }
                if (word == 0x03E00008u)
                    break;
            }
            return gsBase ? getterVar : 0u;
        }

        void resetGParamStateIfNewRuntime(PS2Runtime *runtime)
        {
            if (runtime == g_gparamRuntime)
                return;
            g_gparamRuntime = runtime;
            g_gparam = {1, 2, 1, 3};
            g_gparamVar = 0u;
            g_gparamBlock = 0u;
            g_gparamResetDone = false;
        }

        void writeGParamTo(uint8_t *rdram, uint32_t addr)
        {
            if (uint8_t *p = addr ? getMemPtr(rdram, addr) : nullptr)
                std::memcpy(p, &g_gparam, sizeof(g_gparam)); // the four shorts only, as libgraph
        }

        // Host mirror <- libgraph's variable (the game's: anything may write it), then
        // mirror -> the runtime's block, if one was handed out, so its holders see the same.
        void refreshGParam(uint8_t *rdram, PS2Runtime *runtime)
        {
            resetGParamStateIfNewRuntime(runtime);
            if (g_gparamVar != 0u)
                if (const uint8_t *p = getConstMemPtr(rdram, g_gparamVar))
                    std::memcpy(&g_gparam, p, sizeof(g_gparam));
            writeGParamTo(rdram, g_gparamBlock);
        }

        // Mirror -> libgraph's variable and the runtime's block.
        void publishGParam(uint8_t *rdram, PS2Runtime *runtime)
        {
            resetGParamStateIfNewRuntime(runtime);
            writeGParamTo(rdram, g_gparamVar);
            writeGParamTo(rdram, g_gparamBlock);
        }

        // libgraph's variable found. If sceGsResetGraph already ran (its values are only in the
        // mirror), they go into the variable; otherwise the variable (its .data, or what the
        // game's own code put there) is the truth and the mirror takes it.
        void adoptGuestGParam(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr)
        {
            resetGParamStateIfNewRuntime(runtime);
            if (addr == 0u || addr == g_gparamVar)
                return;
            g_gparamVar = addr;
            std::cerr << "[gs:gparam] libgraph's sceGsGParam at 0x" << std::hex << addr << std::dec
                      << (g_gparamResetDone ? " (ResetGraph values written in)" : "") << std::endl;
            if (g_gparamResetDone)
                publishGParam(rdram, runtime);
            else
                refreshGParam(rdram, runtime);
        }

        // sceGsGetGParam's answer. libgraph's variable once known. Before that, a 16-byte block from
        // the runtime's guest heap, kept for the whole run: that heap also backs the game's malloc
        // (bound to the runtime), so nothing else is ever handed that memory; not scratchpad,
        // which belongs to the game. Allocated on the first sceGsGetGParam that needs it, long
        // after crt0's SetupHeap (which resets the heap). If libgraph's variable is found later,
        // new calls return the variable and the block keeps being updated with every change the
        // runtime makes or reads (publish/refresh), so an old pointer never goes stale. (Writes
        // the game makes through the block itself are not carried back: libgraph's own
        // sceGsGetGParam result is read-only by convention.)
        uint32_t gparamPointer(uint8_t *rdram, PS2Runtime *runtime)
        {
            resetGParamStateIfNewRuntime(runtime);
            if (g_gparamVar != 0u)
                return g_gparamVar;
            if (g_gparamBlock == 0u && runtime)
            {
                const uint32_t block = runtime->guestMalloc(16u, 16u);
                if (uint8_t *p = block ? getMemPtr(rdram, block) : nullptr)
                {
                    std::memset(p, 0, 16u);
                    g_gparamBlock = block;
                }
            }
            writeGParamTo(rdram, g_gparamBlock);
            return g_gparamBlock;
        }

        // sceGsSetDefDispEnv's sceGsDispEnv (see the stub below for the field values).
        GsDispEnvMem buildDefDispEnv(uint32_t psm, uint32_t w, uint32_t h, int32_t dx, int32_t dy)
        {
            if (w == 0)
                w = 640;
            if (h == 0)
                h = 448;

            const uint32_t fbw = (w + 63) / 64;
            const uint32_t magh = std::max<uint32_t>(1u, (w + 2559u) / w);
            const bool pal = (g_gparam.omode == 3);
            const bool interlaced = (g_gparam.interlace & 0x1) != 0;
            const int32_t originX = pal ? 656 : 636;
            const int32_t originY = interlaced ? (pal ? 72 : 50) : (pal ? 36 : 25);
            // DH counts frame lines; in interlaced FRAME mode (ffmode 1) each field shows the
            // whole h-line buffer, so the picture spans 2h lines (libgraph: DH = 2h - 1).
            const bool frameModeDh = interlaced && (g_gparam.ffmode & 0x1) != 0;
            GsDispEnvMem env{};
            env.pmode = makePmode(0u, 1u, 1u, 1u, 0u, 0u);
            env.smode2 = interlaced ? (1ull | (static_cast<uint64_t>(g_gparam.ffmode & 0x1) << 1)) : 2ull;
            // libgraph: (psm & 0xf) << 15.
            env.dispfb = makeDispFb(0, fbw, psm & 0xFu, 0, 0);
            env.display = makeDisplay(static_cast<uint32_t>(dx * static_cast<int32_t>(magh) + originX),
                                      static_cast<uint32_t>(dy + originY),
                                      magh - 1u, 0u, w * magh - 1u, frameModeDh ? 2u * h - 1u : h - 1u);
            env.bgcolor = 0ull;
            return env;
        }

        // sceGszbufaddr(psm, w, h) at 0x10cb70: pages of one w x h buffer (32 lines per page
        // for 32-bit formats, 64 when psm & 2), times 2 unless the GS runs interlaced in field
        // mode (gparam interlace 1, ffmode 0), returned as a short.
        int32_t libgraphZbufAddr(int32_t psm, int16_t w, int16_t h)
        {
            const int32_t w32 = w;
            const int32_t h32 = h;
            const int32_t wPages = (w32 + 63) >= 0 ? (w32 + 63) >> 6 : (w32 + 126) >> 6;
            int32_t hPages;
            if ((psm & 2) == 0)
                hPages = (h32 + 31) >= 0 ? (h32 + 31) >> 5 : (h32 + 62) >> 5;
            else
                hPages = (h32 + 63) >= 0 ? (h32 + 63) >> 6 : (h32 + 126) >> 6;
            const int32_t pages = wPages * hPages;
            const bool fieldInterlaced = g_gparam.interlace == 1 && g_gparam.ffmode == 0;
            const uint32_t scaled = static_cast<uint32_t>(pages) * (fieldInterlaced ? 0x10000u : 0x20000u);
            return static_cast<int32_t>(scaled) >> 16;
        }

        // sceGsSetDefDrawEnv(env, psm, w, h, ztest, zpsm) at 0x10cc38. PRMODECONT and COLCLAMP
        // get bit 0 ORed in and DTHE gets bit 0 set or cleared: libgraph keeps the other bits
        // of those three values as they were in memory, so env holds the guest's bytes on entry.
        // sceGsSetDefDrawEnv2 (not linked in SLPS-25887) is the same code for context 2: by the
        // SDK symbol database it is 480 bytes against 484 with its two sceGszbufaddr calls at
        // +132/+184 against +136/+188, and sceGsDrawEnv2 has the same layout with the _2 registers.
        struct DrawEnvRegs
        {
            uint64_t frame, zbuf, xyoffset, scissor, test;
        };
        constexpr DrawEnvRegs kDrawEnv1Regs{GS_REG_FRAME_1, GS_REG_ZBUF_1, GS_REG_XYOFFSET_1, GS_REG_SCISSOR_1, GS_REG_TEST_1};
        constexpr DrawEnvRegs kDrawEnv2Regs{GS_REG_FRAME_2, GS_REG_ZBUF_2, GS_REG_XYOFFSET_2, GS_REG_SCISSOR_2, GS_REG_TEST_2};

        void libgraphSetDefDrawEnv(GsDrawEnv1Mem &env, int16_t psm, int16_t w, int16_t h, int16_t ztest, int16_t zpsm,
                                   const DrawEnvRegs &regs = kDrawEnv1Regs)
        {
            const uint64_t psm64 = static_cast<uint64_t>(static_cast<int64_t>(psm));
            const int64_t ztest64 = ztest;
            const int64_t w64 = w;
            const int64_t h64 = h;

            env.frame1 = {((static_cast<uint64_t>((w64 + 0x3f) >> 6) & 0x3fu) << 16) | ((psm64 & 0xfu) << 24),
                          regs.frame};
            const int64_t zbp = static_cast<int16_t>(libgraphZbufAddr(psm, w, h));
            env.zbuf1 = {static_cast<uint64_t>(zbp) | ((static_cast<uint64_t>(static_cast<int64_t>(zpsm)) & 0xfu) << 24) |
                             (ztest64 == 0 ? 0x100000000ull : 0ull),
                         regs.zbuf};
            env.xyoffset1 = {static_cast<uint64_t>((0x800 - (w64 >> 1)) * 0x10) |
                                 (static_cast<uint64_t>(0x800 - (h64 >> 1)) << 36),
                             regs.xyoffset};
            env.scissor1 = {(static_cast<uint64_t>(w64 - 1) << 16) | (static_cast<uint64_t>(h64 - 1) << 48),
                            regs.scissor};
            env.prmodecont = {env.prmodecont.value | 1u, GS_REG_PRMODECONT};
            env.colclamp = {env.colclamp.value | 1u, GS_REG_COLCLAMP};
            env.dthe = {(psm64 & 2u) != 0u ? (env.dthe.value | 1u) : (env.dthe.value & ~1ull), GS_REG_DTHE};
            env.test1 = {ztest64 == 0 ? 0x30000ull : ((static_cast<uint64_t>(ztest64) & 3u) << 17) | 0x10000ull,
                         regs.test};
        }

        static_assert(sizeof(GsDrawEnv1Mem) == sizeof(GsDrawEnv2Mem), "sceGsDrawEnv1/2 layouts differ");

        // sceGsSetDefClear(cp, ztest, x, y, w, h, r, g, b, a, z) at 0x10ce20: the 6 register pairs.
        void libgraphSetDefClear(GsClearMem &clear, int64_t ztest, int64_t x, int64_t y, int64_t w, int64_t h,
                                 uint64_t r, uint64_t g, uint64_t b, uint64_t a, uint64_t z)
        {
            clear.testa = {0x30000ull, GS_REG_TEST_1};
            clear.prim = {6ull, GS_REG_PRIM};
            clear.rgbaq = {r | (g << 8) | (b << 16) | (a << 24) | 0x3f80000000000000ull, GS_REG_RGBAQ};
            clear.xyz2a = {static_cast<uint64_t>(x << 4) | (static_cast<uint64_t>(y << 4) << 16) | (z << 32), GS_REG_XYZ2};
            clear.xyz2b = {static_cast<uint64_t>((x + w) << 4) | (static_cast<uint64_t>((y + h) << 4) << 16) | (z << 32),
                           GS_REG_XYZ2};
            clear.testb = {makeClearRestoreTest(static_cast<uint32_t>(ztest)), GS_REG_TEST_1};
        }

        // sceGsPutDrawEnv(sceGifTag *giftag): libgraph starts GIF DMA (channel 2, normal mode)
        // on the giftag and the NLOOP qwords after it. Same path here: the runtime's GIF channel.
        bool putDrawEnvGiftag(uint8_t *rdram, PS2Runtime *runtime, uint32_t giftagAddr)
        {
            const uint8_t *tag = getConstMemPtr(rdram, giftagAddr);
            if (!runtime || !tag || !runtime->syncCoreSubsystems())
                return false;
            uint64_t lo = 0;
            std::memcpy(&lo, tag, sizeof(lo));
            const uint32_t qwc = static_cast<uint32_t>(lo & 0x7FFFu) + 1u;
            constexpr uint32_t GIF_CHANNEL = 0x1000A000;
            constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
            auto &mem = runtime->memory();
            mem.writeIORegister(GIF_CHANNEL + 0x10u, giftagAddr);
            mem.writeIORegister(GIF_CHANNEL + 0x20u, qwc);
            mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
            mem.processPendingTransfers();
            return true;
        }

        // sceGsSetDefDrawEnv / sceGsSetDefDrawEnv2 after decoding (typed and legacy share it).
        int32_t setDefDrawEnvBody(uint8_t *rdram, PS2Runtime *runtime, uint32_t envAddr, int16_t psm, int16_t w,
                                  int16_t h, int16_t ztest, int16_t zpsm, const DrawEnvRegs &regs)
        {
            refreshGParam(rdram, runtime);
            uint8_t *const ptr = getMemPtr(rdram, envAddr);
            if (!ptr || !getMemPtr(rdram, envAddr + static_cast<uint32_t>(sizeof(GsDrawEnv1Mem)) - 1u))
            {
                return 8;
            }
            GsDrawEnv1Mem env{};
            std::memcpy(&env, ptr, sizeof(env));
            libgraphSetDefDrawEnv(env, psm, w, h, ztest, zpsm, regs);
            std::memcpy(ptr, &env, sizeof(env));
            return 8;
        }

        // The legacy decoding of sceGsSetDefDrawEnv(2) (PS2X_TYPED_STUBS=0).
        void setDefDrawEnvCommon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const DrawEnvRegs &regs)
        {
            const uint32_t envAddr = getRegU32(ctx, 4);
            const int16_t psm = static_cast<int16_t>(getRegU32(ctx, 5) & 0xFFFFu);
            const int16_t w = static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFFu);
            const int16_t h = static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFFu);
            const int16_t ztest = static_cast<int16_t>(getRegU32(ctx, 8) & 0xFFFFu);
            const int16_t zpsm = static_cast<int16_t>(getRegU32(ctx, 9) & 0xFFFFu);

            setReturnS32(ctx, setDefDrawEnvBody(rdram, runtime, envAddr, psm, w, h, ztest, zpsm, regs));
        }
    }

    // sceGsGetGParam(): libgraph returns its sceGsGParam variable (four shorts: interlace, omode,
    // ffmode, version). When a game binds the getter itself, its body still sits in guest memory
    // and names that variable; otherwise (or before it is known) the runtime's own block.
    void sceGsGetGParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        adoptGuestGParam(rdram, runtime, decodeGParamGetter(rdram, calledGuestFunction(rdram, ctx)));
        refreshGParam(rdram, runtime);
        setReturnU32(ctx, gparamPointer(rdram, runtime));
    }

    void sceGsPutDispEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        GsDispEnvMem env{};
        if (!readGsDispEnv(rdram, envAddr, env))
        {
            setReturnS32(ctx, -1);
            return;
        }
        applyGsDispEnv(runtime, env);
        setReturnS32(ctx, 0);
    }

    // sceGsPutDrawEnv(sceGifTag *giftag): the argument is the giftag in front of a draw
    // environment (NLOOP A+D pairs), not the environment itself.
    void sceGsPutDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t giftagAddr = getRegU32(ctx, 4);
        setReturnS32(ctx, putDrawEnvGiftag(rdram, runtime, giftagAddr) ? 0 : -1);
    }

    // sceGsResetGraph(short mode, short interlace, short omode, short ffmode), libgraph at
    // 0x10c480 in SLPS-25887: modes 0 (reset) and 5 (mode change only) store into sceGsGetGParam()
    // interlace (+0) and omode (+2) as given, ffmode != 0 (+4) and (GS CSR >> 16) & 0xff, the
    // GS revision (+6), then SetGsCrt(interlace & 1, omode & 0xff, ffmode & 1). Mode 1 only
    // writes CSR 0x100. The variable is the guest's own (located from the guest's
    // sceGsResetGraph, see locateGParamFromResetGraph) and the runtime's block if sceGsGetGParam
    // handed one out; the host mirror always.
    void sceGsResetGraph(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int16_t mode = static_cast<int16_t>(getRegU32(ctx, 4) & 0xFFFFu);
        uint32_t interlace = getRegU32(ctx, 5);
        uint32_t omode = getRegU32(ctx, 6);
        uint32_t ffmode = getRegU32(ctx, 7);

        if (mode == 0 || mode == 5)
        {
            adoptGuestGParam(rdram, runtime, locateGParamFromResetGraph(rdram, calledGuestFunction(rdram, ctx)));
            const uint64_t csr = runtime ? runtime->memory().gs().csr.load() : 0ull;
            g_gparam.interlace = static_cast<int16_t>(interlace & 0xFFFFu);
            g_gparam.omode = static_cast<int16_t>(omode & 0xFFFFu);
            g_gparam.ffmode = static_cast<int16_t>((ffmode & 0xFFFFu) != 0u ? 1 : 0);
            g_gparam.version = static_cast<int16_t>((csr >> 16) & 0xFFu);
            g_gparamResetDone = true;
            publishGParam(rdram, runtime);
        }

        if (mode == 0)
        {
            if (runtime && !runtime->syncCoreSubsystems())
            {
                setReturnS32(ctx, -1);
                return;
            }
            uint64_t pmode = makePmode(1, 0, 0, 0, 0, 0x80);
            uint64_t smode2 = (interlace & 0x1) | ((ffmode & 0x1) << 1);
            uint64_t dispfb = makeDispFb(0, 10, 0, 0, 0);
            uint64_t display = makeDisplay(0, 0, 0, 0, 639, 447);
            uint64_t bgcolor = 0ULL;

            if (runtime)
            {
                uint32_t pktAddr = runtime->guestMalloc(128u, 16u);
                if (pktAddr != 0u)
                {
                    uint8_t *pkt = getMemPtr(rdram, pktAddr);
                    if (pkt)
                    {
                        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
                        q[0] = makeGiftagAplusD(7u);
                        q[1] = 0xEULL;
                        q[2] = pmode;
                        q[3] = 0x41ULL;
                        q[4] = smode2;
                        q[5] = 0x42ULL;
                        q[6] = dispfb;
                        q[7] = 0x59ULL;
                        q[8] = display;
                        q[9] = 0x5aULL;
                        q[10] = dispfb;
                        q[11] = 0x5bULL;
                        q[12] = display;
                        q[13] = 0x5cULL;
                        q[14] = bgcolor;
                        q[15] = 0x5fULL;
                        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
                        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
                        auto &mem = runtime->memory();
                        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
                        mem.writeIORegister(GIF_CHANNEL + 0x20u, 8u);
                        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
                        mem.processPendingTransfers();
                        runtime->guestFree(pktAddr);
                    }
                    else
                    {
                        runtime->guestFree(pktAddr);
                    }
                }
            }
        }

        setReturnS32(ctx, 0);
    }

    void sceGsResetPath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    // sceGsSetDefClear(sceGsClear *cp, short ztest, short x, short y, short w, short h,
    //                  u_char r, u_char g, u_char b, u_char a, u_int z): fills the clear packet games
    // send with their draw environment (TEST with ZTST ALWAYS, a sprite from (x, y) to
    // (x + w, y + h) at depth z in the given colour, TEST back to ztest) and returns its 6
    // register pairs. Same values as SDK 3.1.0 libgraph (checked against SLPS-25887's copy).
    // Arguments 5-8 arrive in t0-t3, 9-11 on the stack in 8-byte slots.
    PS2_STUB_TYPED(int32_t, sceGsSetDefClear, ps2_abi::gptr cp, int16_t ztest, int16_t x, int16_t y, int16_t w,
                   int16_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint32_t z)
    {
        (void)ctx;
        GsClearMem packet{};
        libgraphSetDefClear(packet, ztest, x, y, w, h, r, g, b, a, z);
        if (!writeGuestBytes(rdram, runtime, cp, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet)))
        {
            return 0;
        }
        return 6;
    }

    void sceGsSetDefClear_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cp = getRegU32(ctx, 4);
        const int64_t ztest = static_cast<int16_t>(getRegU32(ctx, 5) & 0xFFFFu);
        const int64_t x = static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFFu);
        const int64_t y = static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFFu);
        const int64_t w = static_cast<int16_t>(getRegU32(ctx, 8) & 0xFFFFu);
        const int64_t h = static_cast<int16_t>(getRegU32(ctx, 9) & 0xFFFFu);
        const uint64_t r = getRegU32(ctx, 10) & 0xFFu;
        const uint64_t g = getRegU32(ctx, 11) & 0xFFu;
        const uint64_t b = readStackU32(rdram, ctx, 0) & 0xFFu;
        const uint64_t a = readStackU32(rdram, ctx, 8) & 0xFFu;
        const uint64_t z = readStackU32(rdram, ctx, 16);

        GsClearMem packet{};
        libgraphSetDefClear(packet, ztest, x, y, w, h, r, g, b, a, z);
        if (!writeGuestBytes(rdram, runtime, cp, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet)))
        {
            setReturnS32(ctx, 0);
            return;
        }
        setReturnS32(ctx, 6);
    }

    PS2_STUB_DISPATCH(sceGsSetDefClear)

    // sceGsSetDefDBuffDc after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t setDefDBuffDcBody(uint8_t *rdram, PS2Runtime *runtime, uint32_t envAddr, uint32_t psm, uint32_t w,
                                     uint32_t h, uint32_t ztest, uint32_t zpsm, uint32_t clear)
    {
        if (w == 0u)
        {
            w = 640u;
        }
        if (h == 0u)
        {
            h = 448u;
        }

        refreshGParam(rdram, runtime);
        const uint32_t fbw = std::max<uint32_t>(1u, (w + 63u) / 64u);
        const uint64_t pmode = makePmode(1u, 1u, 0u, 0u, 0u, 0x80u);
        const uint64_t smode2 =
            (static_cast<uint64_t>(g_gparam.interlace & 0x1) << 0) |
            (static_cast<uint64_t>(g_gparam.ffmode & 0x1) << 1);
        // DH in frame lines: 2h - 1 in interlaced FRAME mode, as libgraph's sceGsSetDefDispEnv.
        const bool frameModeDh = (g_gparam.interlace & 0x1) != 0 && (g_gparam.ffmode & 0x1) != 0;
        const uint64_t display = makeDisplay(636u, 32u, 0u, 0u, w - 1u, frameModeDh ? 2u * h - 1u : h - 1u);

        const int32_t drawWidth = static_cast<int32_t>(w);
        const int32_t drawHeight = static_cast<int32_t>(h);

        // sceGszbufaddr(psm, w, h), as libgraph's sceGsSetDefDBuffDc calls it (the old code passed
        // this function's a0-a3 on, so the old sceGszbufaddr took psm from a0, the env address).
        const uint32_t zbufAddr = static_cast<uint32_t>(libgraphZbufAddr(
            static_cast<int32_t>(psm), static_cast<int16_t>(w & 0xFFFFu), static_cast<int16_t>(h & 0xFFFFu)));

        const uint32_t fbp1 = zbufAddr;
        const uint64_t dispfb0 = makeDispFb(fbp1, fbw, psm, 0u, 0u);
        const uint64_t dispfb1 = makeDispFb(0u, fbw, psm, 0u, 0u);

        GsDBuffDcMem db{};
        db.disp[0].pmode = pmode;
        db.disp[0].smode2 = smode2;
        db.disp[0].dispfb = dispfb0;
        db.disp[0].display = display;
        db.disp[0].bgcolor = 0u;
        db.disp[1] = db.disp[0];
        db.disp[1].dispfb = dispfb1;

        const bool seedClear = clear != 0u;
        db.giftag0 = {makeGiftagAplusD(seedClear ? 22u : 16u), 0xEULL};
        seedGsDrawEnv1(db.draw01, drawWidth, drawHeight, 0u, fbw, psm, zbufAddr, zpsm, ztest, false);
        seedGsDrawEnv2(db.draw02, drawWidth, drawHeight, 0u, fbw, psm, zbufAddr, zpsm, ztest, false);
        db.giftag1 = db.giftag0;
        seedGsDrawEnv1(db.draw11, drawWidth, drawHeight, fbp1, fbw, psm, zbufAddr, zpsm, ztest, false);
        seedGsDrawEnv2(db.draw12, drawWidth, drawHeight, fbp1, fbw, psm, zbufAddr, zpsm, ztest, false);
        if (seedClear)
        {
            seedGsClearPacket(db.clear0, drawWidth, drawHeight, 0u, ztest, false);
            seedGsClearPacket(db.clear1, drawWidth, drawHeight, 0u, ztest, true);
        }

        if (!writeGsDBuffDc(rdram, envAddr, db))
        {
            return -1;
        }
        return 0;
    }

    // sceGsSetDefDBuffDc(sceGsDBuffDc *db, short psm, short w, short h, short ztest, short zpsm,
    //                    short clear). Declared u32 (as the old decoding read them), not short: see
    // NOTES-typed.md, behaviour differences.
    PS2_STUB_TYPED(int32_t, sceGsSetDefDBuffDc, ps2_abi::gptr env, uint32_t psm, uint32_t w, uint32_t h, uint32_t ztest,
                   uint32_t zpsm, uint32_t clear)
    {
        (void)ctx;
        return setDefDBuffDcBody(rdram, runtime, env, psm, w, h, ztest, zpsm, clear);
    }

    void sceGsSetDefDBuffDc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t psm = getRegU32(ctx, 5);
        uint32_t w = getRegU32(ctx, 6);
        uint32_t h = getRegU32(ctx, 7);
        const GsTrailingArgs3 trailing = decodeGsTrailingArgs3(ctx);
        const uint32_t ztest = trailing.arg0;
        const uint32_t zpsm = trailing.arg1;
        const uint32_t clear = trailing.arg2;

        setReturnS32(ctx, setDefDBuffDcBody(rdram, runtime, envAddr, psm, w, h, ztest, zpsm, clear));
    }

    PS2_STUB_DISPATCH(sceGsSetDefDBuffDc)
    PS2_STUB_DEVIATION(sceGsSetDefDBuffDc, "SDK: psm/w/h/ztest/zpsm/clear are short; declared u32 as the old decoding read them (a w/h with bit 15 set would differ)")

    // sceGsSetDefDBuff(db, psm, w, h, ztest, zpsm, clear), like libgraph's copy at 0x10cf28:
    // both sceGsDispEnv through sceGsSetDefDispEnv(psm, w, h, 0, 0), both draw environments
    // through sceGsSetDefDrawEnv(psm, w, h, ztest, zpsm) (so DTHE follows psm & 2), with clear
    // set both clear packets through sceGsSetDefClear(ztest, 0x800 - w/2, 0x800 - h/2, w, h,
    // 0, 0, 0, 0, 0), then both giftags (A+D, NLOOP 8 or 14 with the clear, EOP, NREG 1,
    // REGS 0xe), and unless the GS runs interlaced in field mode, the second buffer
    // (sceGszbufaddr / 2 pages up) as draw0's FRAME and disp[1]'s DISPFB. Arguments 5-7 arrive
    // in t0-t2 (EE calling convention); all six are shorts. Memory libgraph does not write
    // (the clear packets without clear, the upper bits of PRMODECONT/COLCLAMP/DTHE) is kept.
    // sceGsSetDefDBuff after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t setDefDBuffBody(uint8_t *rdram, PS2Runtime *runtime, uint32_t envAddr, int16_t psm, int16_t w,
                                   int16_t h, int16_t ztest, int16_t zpsm, int16_t clear)
    {
        // libgraph divides by w in sceGsSetDefDispEnv (a trap for 0); keep the runtime's defaults.
        if (w == 0)
            w = 640;
        if (h == 0)
            h = 448;

        refreshGParam(rdram, runtime);
        GsDBuffMem db{};
        if (!readGsDBuff(rdram, envAddr, db))
        {
            return -1;
        }

        db.disp[0] = buildDefDispEnv(static_cast<uint32_t>(psm), static_cast<uint32_t>(w), static_cast<uint32_t>(h), 0, 0);
        db.disp[1] = db.disp[0];
        libgraphSetDefDrawEnv(db.draw0, psm, w, h, ztest, zpsm);
        libgraphSetDefDrawEnv(db.draw1, psm, w, h, ztest, zpsm);
        if (clear != 0)
        {
            const int64_t x = 0x800 - (w >> 1);
            const int64_t y = 0x800 - (h >> 1);
            libgraphSetDefClear(db.clear0, ztest, x, y, w, h, 0u, 0u, 0u, 0u, 0u);
            libgraphSetDefClear(db.clear1, ztest, x, y, w, h, 0u, 0u, 0u, 0u, 0u);
        }

        db.giftag0 = {makeGiftagAplusD(clear != 0 ? 14u : 8u), 0xEull};
        db.giftag1 = db.giftag0;

        const int32_t zbuf = libgraphZbufAddr(psm, w, h);
        if ((g_gparam.interlace == 1 && g_gparam.ffmode == 1) || g_gparam.interlace == 0)
        {
            const uint64_t fbp1 = static_cast<uint64_t>(static_cast<int16_t>(zbuf >> 1)) & 0x1FFu;
            db.draw0.frame1.value = (db.draw0.frame1.value & ~0x1FFull) | fbp1;
            db.disp[1].dispfb = (db.disp[1].dispfb & ~0x1FFull) | fbp1;
        }

        if (!writeGsDBuff(rdram, envAddr, db))
        {
            return -1;
        }
        return 0;
    }

    PS2_STUB_TYPED(int32_t, sceGsSetDefDBuff, ps2_abi::gptr env, int16_t psm, int16_t w, int16_t h, int16_t ztest, int16_t zpsm,
                   int16_t clear)
    {
        (void)ctx;
        return setDefDBuffBody(rdram, runtime, env, psm, w, h, ztest, zpsm, clear);
    }

    void sceGsSetDefDBuff_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        const int16_t psm = static_cast<int16_t>(getRegU32(ctx, 5) & 0xFFFFu);
        int16_t w = static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFFu);
        int16_t h = static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFFu);
        const int16_t ztest = static_cast<int16_t>(getRegU32(ctx, 8) & 0xFFFFu);
        const int16_t zpsm = static_cast<int16_t>(getRegU32(ctx, 9) & 0xFFFFu);
        const int16_t clear = static_cast<int16_t>(getRegU32(ctx, 10) & 0xFFFFu);

        setReturnS32(ctx, setDefDBuffBody(rdram, runtime, envAddr, psm, w, h, ztest, zpsm, clear));
    }

    PS2_STUB_DISPATCH(sceGsSetDefDBuff)

    // sceGsSetDefDispEnv(sceGsDispEnv *disp, short psm, short w, short h, short dx, short dy): psm, w
    // and h declared u32 (as the old decoding read them), dx/dy short (arguments 5-6 in t0/t1).
    // Defaults: see the legacy form below.
    PS2_STUB_TYPED(int32_t, sceGsSetDefDispEnv, ps2_abi::gptr env, uint32_t psm, uint32_t w, uint32_t h, int16_t dx,
                   int16_t dy)
    {
        (void)ctx;
        refreshGParam(rdram, runtime);
        const GsDispEnvMem disp = buildDefDispEnv(psm, w, h, dx, dy);
        writeGsDispEnv(rdram, env, disp);
        return 0;
    }

    void sceGsSetDefDispEnv_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t psm = getRegU32(ctx, 5);
        uint32_t w = getRegU32(ctx, 6);
        uint32_t h = getRegU32(ctx, 7);
        // Arguments 5 and 6 arrive in $t0/$t1 (EE ABI: a0-a3, t0-t3), as signed shorts.
        const int32_t dx = static_cast<int16_t>(getRegU32(ctx, 8) & 0xFFFFu);
        const int32_t dy = static_cast<int16_t>(getRegU32(ctx, 9) & 0xFFFFu);

        // Same defaults as libgraph's sceGsSetDefDispEnv (checked against SDK 3.1.0's
        // copy in SRW Z, 0x10c738): PMODE 0x66 (EN1=0 EN2=1 CRTMD=1 MMOD=1 AMOD=1 SLBG=0 ALP=0),
        // SMODE2 from the interlace/field mode given to sceGsResetGraph, DISPFB(0, w/64,
        // psm, 0, 0), DISPLAY in VCK units (MAGH = ceil(2560/w) - 1, DW = w*MAGH - 1,
        // DH = h - 1) at the screen origin (NTSC DX 636 DY 50, PAL DX 656 DY 72;
        // non-interlaced DY 25 / 36), BGCOLOR 0, and DH = 2h - 1 when interlaced with
        // ffmode != 0 (FRAME mode: the presenter shows (DH + 1) / 2 buffer lines, each twice,
        // as the CRTC does). Not reproduced: 480p. Width/height 0 default to 640x448 (libgraph
        // would trap).
        refreshGParam(rdram, runtime);
        const GsDispEnvMem env = buildDefDispEnv(psm, w, h, dx, dy);
        writeGsDispEnv(rdram, envAddr, env);
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceGsSetDefDispEnv)
    PS2_STUB_DEVIATION(sceGsSetDefDispEnv, "SDK: psm/w/h are short; declared u32 as the old decoding read them (a w/h with bit 15 set would differ)")

    // sceGsSetDefDrawEnv(sceGsDrawEnv1 *env, short psm, short w, short h, short ztest, short zpsm),
    // libgraph at 0x10cc38 in SLPS-25887 (libgraphSetDefDrawEnv above): arguments 5-6 in t0/t1,
    // returns 8 (register pairs). Read-modify-write, as libgraph keeps the upper bits of
    // PRMODECONT, COLCLAMP and DTHE. No 640x448 default: libgraph has none (and no division).
    PS2_STUB_TYPED(int32_t, sceGsSetDefDrawEnv, ps2_abi::gptr env, int16_t psm, int16_t w, int16_t h, int16_t ztest,
                   int16_t zpsm)
    {
        (void)ctx;
        return setDefDrawEnvBody(rdram, runtime, env, psm, w, h, ztest, zpsm, kDrawEnv1Regs);
    }

    void sceGsSetDefDrawEnv_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setDefDrawEnvCommon(rdram, ctx, runtime, kDrawEnv1Regs);
    }

    PS2_STUB_DISPATCH(sceGsSetDefDrawEnv)

    // sceGsSetDefDrawEnv2: the same for context 2 (FRAME_2, ZBUF_2, XYOFFSET_2, SCISSOR_2, TEST_2).
    PS2_STUB_TYPED(int32_t, sceGsSetDefDrawEnv2, ps2_abi::gptr env, int16_t psm, int16_t w, int16_t h, int16_t ztest,
                   int16_t zpsm)
    {
        (void)ctx;
        return setDefDrawEnvBody(rdram, runtime, env, psm, w, h, ztest, zpsm, kDrawEnv2Regs);
    }

    void sceGsSetDefDrawEnv2_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setDefDrawEnvCommon(rdram, ctx, runtime, kDrawEnv2Regs);
    }

    PS2_STUB_DISPATCH(sceGsSetDefDrawEnv2)

    // sceGsSetDefLoadImage(sceGsLoadImage *lp, short dbp, short dbw, short dpsm, short x, short y,
    //                      short w, short h): a1-a3 = bp, bw, psm; t0-t3 = x, y, w, h. Declared u32
    // as the old decoding read them (the fields are 16 and 8 bits, so the result is the same).
    PS2_STUB_TYPED(int32_t, sceGsSetDefLoadImage, ps2_abi::gptr imgAddr, uint32_t vramAddr, uint32_t vramWidth,
                   uint32_t psm, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        (void)ctx;
        (void)runtime;
        GsImageMem img{};
        img.x = static_cast<uint16_t>(x);
        img.y = static_cast<uint16_t>(y);
        img.width = static_cast<uint16_t>(width);
        img.height = static_cast<uint16_t>(height);
        img.vram_addr = static_cast<uint16_t>(vramAddr);
        img.vram_width = static_cast<uint8_t>(vramWidth);
        img.psm = static_cast<uint8_t>(psm);

        writeGsImage(rdram, imgAddr, img);
        return 0;
    }

    void sceGsSetDefLoadImage_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        const GsSetDefImageArgs args = decodeGsSetDefImageArgs(ctx);

        GsImageMem img{};
        img.x = static_cast<uint16_t>(args.x);
        img.y = static_cast<uint16_t>(args.y);
        img.width = static_cast<uint16_t>(args.width);
        img.height = static_cast<uint16_t>(args.height);
        img.vram_addr = static_cast<uint16_t>(args.vramAddr);
        img.vram_width = static_cast<uint8_t>(args.vramWidth);
        img.psm = static_cast<uint8_t>(args.psm);

        writeGsImage(rdram, imgAddr, img);
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceGsSetDefLoadImage)
    PS2_STUB_DEVIATION(sceGsSetDefLoadImage, "SDK: bp/bw/psm/x/y/w/h are short; declared u32 as the old decoding read them (the stored fields are 8/16 bits, so the result is the same)")

    PS2_STUB_ALIAS(sceGsSetDefStoreImage, sceGsSetDefLoadImage, ::ps2_abi::Kind::Stub)

    void sceGsSwapDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        const uint32_t which = getRegU32(ctx, 5) & 1u;

        GsDBuffDcMem db{};
        if (!runtime || !readGsDBuffDc(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }

        applyGsDispEnv(runtime, db.disp[which]);
        static uint32_t s_swapDbuffLogCount = 0u;
        if (s_swapDbuffLogCount < 32u)
        {
            const uint32_t dispFbp = static_cast<uint32_t>(db.disp[which].dispfb & 0x1FFu);
            const uint32_t clearContext = (which == 0u)
                                              ? static_cast<uint32_t>((db.clear0.prim.value >> 9) & 0x1u)
                                              : static_cast<uint32_t>((db.clear1.prim.value >> 9) & 0x1u);
            PS2_IF_AGRESSIVE_LOGS({
                RUNTIME_LOG("[gs:swapdbuff] which=" << which
                                                    << " env=0x" << std::hex << envAddr
                                                    << " dispfb=0x" << db.disp[which].dispfb
                                                    << " display=0x" << db.disp[which].display
                                                    << " pmode=0x" << db.disp[which].pmode
                                                    << " dispFbp=" << dispFbp
                                                    << " clearCtxt=" << clearContext
                                                    << std::dec << std::endl);
            });
            ++s_swapDbuffLogCount;
        }
        if (which == 0u)
        {
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw01), 8u);
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw02), 8u);
            if (hasSeededGsClearPacket(db.clear0))
            {
                const uint32_t clearContext = static_cast<uint32_t>((db.clear0.prim.value >> 9) & 0x1u);
                runtime->gs().clearFramebufferContext(clearContext, static_cast<uint32_t>(db.clear0.rgbaq.value));
            }
            applyGsClearPacket(runtime, db.clear0);
        }
        else
        {
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw11), 8u);
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw12), 8u);
            if (hasSeededGsClearPacket(db.clear1))
            {
                const uint32_t clearContext = static_cast<uint32_t>((db.clear1.prim.value >> 9) & 0x1u);
                runtime->gs().clearFramebufferContext(clearContext, static_cast<uint32_t>(db.clear1.rgbaq.value));
            }
            applyGsClearPacket(runtime, db.clear1);
        }

        setReturnS32(ctx, static_cast<int32_t>(which ^ 1u));
    }

    // sceGsSwapDBuff(db, id): libgraph (92 bytes; per the SDK symbol database it calls
    // sceGsPutDispEnv once and sceGsPutDrawEnv on one of two branches) puts disp[id & 1] and
    // then the draw packet of that half: giftag0 at +0x50 or giftag1 at +0x140, which carries
    // the draw environment and, when sceGsSetDefDBuff seeded one, the clear (NLOOP 14).
    // The guest's giftag decides what is sent, as on the console.
    void sceGsSwapDBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        const uint32_t which = getRegU32(ctx, 5) & 1u;

        GsDBuffMem db{};
        if (!runtime || !readGsDBuff(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }

        applyGsDispEnv(runtime, db.disp[which]);
        putDrawEnvGiftag(rdram, runtime, envAddr + (which == 0u ? kDBuffGiftag0Offset : kDBuffGiftag1Offset));

        setReturnS32(ctx, static_cast<int32_t>(which ^ 1u));
    }

    void sceGsSyncPath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int32_t mode = static_cast<int32_t>(getRegU32(ctx, 4));
        auto &mem = runtime->memory();

        if (mode == 0)
        {
            mem.processPendingTransfers();

            uint32_t count = 0;
            constexpr uint32_t kTimeout = 0x1000000;

            while ((mem.readIORegister(0x10009000) & 0x100) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x1000A000) & 0x100) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x10003C00) & 0x1F000003) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x10003020) & 0xC00) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            setReturnS32(ctx, 0);
        }
        else
        {
            uint32_t result = 0;

            if ((mem.readIORegister(0x10009000) & 0x100) != 0)
                result |= 1;
            if ((mem.readIORegister(0x1000A000) & 0x100) != 0)
                result |= 2;
            if ((mem.readIORegister(0x10003C00) & 0x1F000003) != 0)
                result |= 4;
            if ((mem.readIORegister(0x10003020) & 0xC00) != 0)
                result |= 0x10;

            setReturnS32(ctx, result);
        }
    }

    void sceGsSyncV(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        refreshGParam(rdram, runtime);
        ps2_syscalls::WaitVSyncTick(rdram,
                                    ctx,
                                    runtime,
                                    g_gparam.interlace != 0 ? -1 : 1);
    }

    void sceGsSyncVCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newCallback = getRegU32(ctx, 4);
        const uint32_t callerPc = ctx ? ctx->pc : 0u;
        const uint32_t callerRa = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t sp = getRegU32(ctx, 29);

        EeScheduler &ee = runtime->eeScheduler();
        ee.bindMainContextForSyscall(*ctx, rdram);
        const uint32_t oldCallback = ee.setGsVSyncCallback(newCallback, gp, sp);

        static uint32_t s_syncVCallbackLogCount = 0u;
        if (s_syncVCallbackLogCount < 128u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                RUNTIME_LOG("[sceGsSyncVCallback:set] new=0x" << std::hex << newCallback
                                                              << " old=0x" << oldCallback
                                                              << " callerPc=0x" << callerPc
                                                              << " callerRa=0x" << callerRa
                                                              << " gp=0x" << gp
                                                              << " sp=0x" << sp
                                                              << std::dec << std::endl);
            });
            ++s_syncVCallbackLogCount;
        }

        setReturnU32(ctx, oldCallback);
    }

    // sceGszbufaddr(psm, short w, short h), libgraph at 0x10cb70 in SLPS-25887: arguments in
    // a0-a2; the gparam test reads sceGsGParam's first 8 bytes as one doubleword masked with
    // 0xffff0000ffff, i.e. interlace == 1 && ffmode == 0 (libgraphZbufAddr above).
    void sceGszbufaddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        refreshGParam(rdram, runtime);
        const int32_t psm = static_cast<int32_t>(getRegU32(ctx, 4));
        const int16_t w = static_cast<int16_t>(getRegU32(ctx, 5) & 0xFFFFu);
        const int16_t h = static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFFu);
        setReturnS32(ctx, libgraphZbufAddr(psm, w, h));
    }

    void Ps2SwapDBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int logCount = 0;
        if (logCount < 8)
        {
            RUNTIME_LOG("ps2_stub Ps2SwapDBuff");
            ++logCount;
        }
        setReturnS32(ctx, 0);
    }

    void sceVif1PkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint64_t dataValue = GPR_U64(ctx, 6);
        const uint32_t words[4] = {
            static_cast<uint32_t>(dataValue & 0xFFFFFFFFu),
            static_cast<uint32_t>(dataValue >> 32u),
            getRegU32(ctx, 5),
            0u,
        };
        writeGuestBytes(rdram,
                        runtime,
                        currentAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
    }

    void sceVif1PkAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        alignPacketBuilderState(rdram,
                                runtime,
                                getRegU32(ctx, 4),
                                getRegU32(ctx, 5),
                                getRegU32(ctx, 6));
    }

    void sceVif1PkCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t refAddr = getRegU32(ctx, 5) & 0x9FFFFFFFu;
        const uint32_t tagWord = getRegU32(ctx, 6) | 0x50000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, refAddr};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
    }

    void sceVif1PkCloseDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        uint32_t openAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
            !tryReadWordFromGuest(rdram, runtime, stateAddr + 12u, openAddr) ||
            openAddr == 0u)
        {
            return;
        }

        const uint32_t currentMinusTag = currentAddr - 4u;
        const uint32_t wordCount = (currentMinusTag - openAddr) >> 2u;
        const uint32_t qwordCount = wordCount >> 2u;
        uint32_t tagWord = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, openAddr, tagWord))
        {
            return;
        }

        tagWord += qwordCount;
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writeGuestU32(rdram, runtime, openAddr, tagWord);
    }

    void sceVif1PkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        closePacketGifTag(rdram, runtime, getRegU32(ctx, 4), 20u);
    }

    void sceVif1PkCnt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t tagWord = getRegU32(ctx, 5) | 0x10000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, 0u};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
    }

    void sceVif1PkEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t tagWord = getRegU32(ctx, 5) | 0x70000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, 0u};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
    }

    void sceVif1PkInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        initPacketBuilderState(rdram, ctx, runtime);
        writeGuestU32(rdram, runtime, getRegU32(ctx, 4) + 20u, 0u);
    }

    void sceVif1PkOpenDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        alignPacketBuilderState(rdram, runtime, stateAddr, 2u, 3u);

        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint32_t tagWord = (getRegU32(ctx, 5) != 0u) ? 0xD0000000u : 0x50000000u;
        writeGuestU32(rdram, runtime, currentAddr, tagWord);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 4u);
        writeGuestU32(rdram, runtime, stateAddr + 12u, currentAddr);
    }

    void sceVif1PkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        openPacketGifTag(rdram, ctx, runtime, getRegU32(ctx, 4), 20u);
    }

    void sceVif1PkReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resetPacketBuilderState(rdram, ctx, runtime);
        writeGuestU32(rdram, runtime, getRegU32(ctx, 4) + 20u, 0u);
    }

    void sceVif1PkReserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t wordCount = getRegU32(ctx, 5);
        uint32_t currentAddr = 0u;
        tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr);
        const uint32_t reservedAddr = reservePacketBuilderWords(rdram, runtime, stateAddr, wordCount);
        setReturnU32(ctx, reservedAddr);
    }

    void sceVif1PkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, terminatePacketBuilderState(rdram, ctx, runtime));
    }
}

#include "ps2_snapshot_hooks.h"

namespace ps2_stubs::snapshot
{
    void saveGs(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
        w.pod(g_gparam);
        w.u32(g_gparamVar);
        w.u32(g_gparamBlock);
        w.boolean(g_gparamResetDone);
    }

    void loadGs(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        snapshotLoadSupport(r);
        r.pod(g_gparam);
        g_gparamVar = r.u32();
        g_gparamBlock = r.u32();
        g_gparamResetDone = r.boolean();
        g_gparamRuntime = &runtime; // the restored state belongs to this runtime
    }
}
