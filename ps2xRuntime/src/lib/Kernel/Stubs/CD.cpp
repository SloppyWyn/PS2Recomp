#include "runtime/ps2_deterministic.h"
#include "Common.h"
#include "CD.h"
#include "MPEG.h"
#include "runtime/ee_scheduler.h"

namespace ps2_stubs
{
    namespace
    {
        constexpr uint32_t kCdStreamBlocking = 1u;
        constexpr uint32_t kDvdSectorsPerSecondX1 = 675u;
        constexpr uint32_t kDvdSectorsPerSecondX4 = kDvdSectorsPerSecondX1 * 4u;
        constexpr uint64_t kNtScFieldsPerSecondNumerator = 60000u;
        constexpr uint64_t kNtScFieldsPerSecondDenominator = 1001u;

        struct CdStreamTimingState
        {
            bool initialized = false;
            bool active = false;
            bool paused = false;
            uint32_t capacitySectors = 64u;
            uint32_t bankCount = 4u;
            uint32_t sectorsPerBank = 16u;
            uint32_t sectorsPerSecond = kDvdSectorsPerSecondX4;
            uint64_t producedSectors = 0u;
            uint64_t consumedSectors = 0u;
            uint64_t productionRemainder = 0u;
            uint64_t lastVSyncTick = 0u;
        };

        uint32_t g_cdStReadTraceCount = 0u;
        CdStreamTimingState g_cdStreamTiming;

        uint64_t currentCdStreamTick(PS2Runtime *runtime)
        {
            return runtime != nullptr ? runtime->eeScheduler().currentVSyncTick() : 0u;
        }

        uint32_t dvdStreamSectorsPerSecond(uint8_t spindleControl)
        {
            switch (spindleControl)
            {
            case 2u: // SCECdSpinX1
                return kDvdSectorsPerSecondX1;
            case 3u: // SCECdSpinX2
                return kDvdSectorsPerSecondX1 * 2u;
            case 11u: // SCECdSpin1p6
                return 1080u;
            case 4u:  // SCECdSpinX4
            case 0u:  // SCECdSpinStm / max
            case 1u:  // optimized
            case 20u: // max
            default:
                return kDvdSectorsPerSecondX4;
            }
        }

        void resetCdStreamProduction(PS2Runtime *runtime)
        {
            g_cdStreamTiming.producedSectors = 0u;
            g_cdStreamTiming.consumedSectors = 0u;
            g_cdStreamTiming.productionRemainder = 0u;
            g_cdStreamTiming.lastVSyncTick = currentCdStreamTick(runtime);
        }

        uint64_t totalCdStreamSectors()
        {
            if (g_cdStreamingEndLbn == 0xFFFFFFFFu || g_cdStreamingEndLbn < g_cdStreamingLbn)
            {
                return std::numeric_limits<uint64_t>::max();
            }
            return g_cdStreamTiming.consumedSectors + static_cast<uint64_t>(g_cdStreamingEndLbn - g_cdStreamingLbn);
        }

        void updateCdStreamProduction(PS2Runtime *runtime)
        {
            if (!g_cdStreamTiming.active || g_cdStreamTiming.paused || runtime == nullptr)
            {
                return;
            }

            const uint64_t tick = currentCdStreamTick(runtime);
            if (tick <= g_cdStreamTiming.lastVSyncTick)
            {
                return;
            }

            const uint64_t elapsedTicks = tick - g_cdStreamTiming.lastVSyncTick;
            g_cdStreamTiming.lastVSyncTick = tick;

            // 59.94 Hz field clock: sectors = fields * sectors/s * 1001 / 60000.
            const uint64_t unitsPerTick = static_cast<uint64_t>(g_cdStreamTiming.sectorsPerSecond) * kNtScFieldsPerSecondDenominator;
            const uint64_t accumulated = g_cdStreamTiming.productionRemainder + elapsedTicks * unitsPerTick;
            const uint64_t elapsedProduction = accumulated / kNtScFieldsPerSecondNumerator;
            g_cdStreamTiming.productionRemainder = accumulated % kNtScFieldsPerSecondNumerator;
            if (elapsedProduction == 0u)
            {
                return;
            }

            const uint64_t buffered = g_cdStreamTiming.producedSectors - g_cdStreamTiming.consumedSectors;
            const uint64_t effectiveCapacity = std::max<uint32_t>(1u, g_cdStreamTiming.capacitySectors);
            const uint64_t space = buffered < effectiveCapacity ? effectiveCapacity - buffered : 0u;
            uint64_t newlyProduced = std::min(elapsedProduction, space);

            const uint64_t streamTotal = totalCdStreamSectors();
            if (streamTotal != std::numeric_limits<uint64_t>::max())
            {
                const uint64_t remainingToProduce = streamTotal > g_cdStreamTiming.producedSectors
                                                        ? streamTotal - g_cdStreamTiming.producedSectors
                                                        : 0u;
                newlyProduced = std::min(newlyProduced, remainingToProduce);
            }

            g_cdStreamTiming.producedSectors += newlyProduced;
            if (elapsedProduction > newlyProduced)
            {
                g_cdStreamTiming.productionRemainder = 0u;
            }
        }

        uint32_t bufferedCdStreamSectors(PS2Runtime *runtime)
        {
            updateCdStreamProduction(runtime);
            const uint64_t buffered = g_cdStreamTiming.producedSectors - g_cdStreamTiming.consumedSectors;
            return static_cast<uint32_t>(std::min<uint64_t>(buffered, std::numeric_limits<uint32_t>::max()));
        }

        uint32_t readableCdStreamSectors(PS2Runtime *runtime)
        {
            const uint32_t buffered = bufferedCdStreamSectors(runtime);
            if (buffered == 0u)
            {
                return 0u;
            }

            const uint64_t streamTotal = totalCdStreamSectors();
            if (streamTotal != std::numeric_limits<uint64_t>::max() && g_cdStreamTiming.producedSectors >= streamTotal)
            {
                return buffered;
            }

            const uint32_t bank = std::max(1u, g_cdStreamTiming.sectorsPerBank);
            return (buffered / bank) * bank;
        }

        uint64_t cdStreamWakeTickForSectors(PS2Runtime *runtime, uint32_t sectorsNeeded)
        {
            const uint64_t now = currentCdStreamTick(runtime);
            if (sectorsNeeded == 0u || g_cdStreamTiming.sectorsPerSecond == 0u)
            {
                return now;
            }

            const uint64_t requiredUnits = static_cast<uint64_t>(sectorsNeeded) * kNtScFieldsPerSecondNumerator;
            const uint64_t remainingUnits = requiredUnits > g_cdStreamTiming.productionRemainder
                                                ? requiredUnits - g_cdStreamTiming.productionRemainder
                                                : 0u;
            const uint64_t unitsPerTick = static_cast<uint64_t>(g_cdStreamTiming.sectorsPerSecond) * kNtScFieldsPerSecondDenominator;
            const uint64_t ticks = std::max<uint64_t>(1u, (remainingUnits + unitsPerTick - 1u) / unitsPerTick);
            return now + ticks;
        }

        void restartCdStreamAt(uint32_t lbn, PS2Runtime *runtime)
        {
            g_cdStreamingLbn = lbn;
            g_cdStreamingEndLbn = cdStreamingEndLbnForStart(lbn);
            resetCdStreamProduction(runtime);
        }
    }

    CdDebugSnapshot getCdDebugSnapshot()
    {
        CdDebugSnapshot snapshot{};
        snapshot.initialized = g_cdInitialized;
        snapshot.lastError = g_lastCdError;
        snapshot.mode = g_cdMode;
        snapshot.streamingLbn = g_cdStreamingLbn;
        snapshot.streamingEndLbn = g_cdStreamingEndLbn;
        snapshot.nextPseudoLbn = g_nextPseudoLbn;
        snapshot.imageSizeBytes = g_cdImageSizeBytes;
        snapshot.imageSizeValid = g_cdImageSizeValid;
        snapshot.cdRoot = getCdRootPath();
        snapshot.cdImage = getCdImagePath();
        snapshot.imageSizePath = g_cdImageSizePath;
        snapshot.leafIndexRoot = g_cdLeafIndexRoot;
        snapshot.leafIndexBuilt = g_cdLeafIndexBuilt;
        snapshot.leafIndexCount = g_cdLeafIndex.size();
        snapshot.loosePathIndexCount = g_cdLoosePathIndex.size();

        snapshot.files.reserve(g_cdFilesByKey.size());
        for (const auto &[key, entry] : g_cdFilesByKey)
        {
            CdDebugFileEntry row{};
            row.key = key;
            row.hostPath = entry.hostPath;
            row.sizeBytes = entry.sizeBytes;
            row.baseLbn = entry.baseLbn;
            row.sectors = entry.sectors;
            snapshot.files.push_back(std::move(row));
        }
        std::sort(snapshot.files.begin(), snapshot.files.end(), [](const CdDebugFileEntry &a, const CdDebugFileEntry &b)
                  { return a.baseLbn < b.baseLbn; });
        return snapshot;
    }

    // The body of sceCdRead after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t cdReadBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a0, uint32_t a1,
                              uint32_t a2)
    {
        (void)runtime;
        struct CdReadArgs
        {
            uint32_t lbn = 0;
            uint32_t sectors = 0;
            uint32_t buf = 0;
            const char *tag = "";
        };

        auto clampReadBytes = [](uint32_t sectors, uint32_t offset) -> size_t
        {
            const uint64_t requested = static_cast<uint64_t>(sectors) * static_cast<uint64_t>(kCdSectorSize);
            if (requested == 0)
            {
                return 0;
            }

            const uint64_t maxBytes = static_cast<uint64_t>(PS2_RAM_SIZE - offset);
            const uint64_t clamped = std::min<uint64_t>(requested, maxBytes);
            return static_cast<size_t>(clamped);
        };

        auto tryRead = [&](const CdReadArgs &args) -> bool
        {
            const uint32_t offset = args.buf & PS2_RAM_MASK;
            const size_t bytes = clampReadBytes(args.sectors, offset);
            if (bytes == 0)
            {
                return true;
            }

            return readCdSectors(args.lbn, args.sectors, rdram + offset, bytes);
        };

        CdReadArgs selected{a0, a1, a2, "a0/a1/a2"};
        bool ok = tryRead(selected);

        if (!ok)
        {
            // Some game-side wrappers use a nonstandard register layout.
            // If primary decode does not resolve to a known LBN, try safe alternatives.
            constexpr uint32_t kMaxReasonableSectors = PS2_RAM_SIZE / kCdSectorSize;
            if (!isResolvableCdLbn(selected.lbn))
            {
                const std::array<CdReadArgs, 5> alternatives = {
                    CdReadArgs{a2, a1, a0, "a2/a1/a0"},
                    CdReadArgs{a0, a2, a1, "a0/a2/a1"},
                    CdReadArgs{a1, a0, a2, "a1/a0/a2"},
                    CdReadArgs{a1, a2, a0, "a1/a2/a0"},
                    CdReadArgs{a2, a0, a1, "a2/a0/a1"}};

                for (const CdReadArgs &candidate : alternatives)
                {
                    if (candidate.sectors > kMaxReasonableSectors)
                    {
                        continue;
                    }
                    if (!isResolvableCdLbn(candidate.lbn))
                    {
                        continue;
                    }

                    if (tryRead(candidate))
                    {
                        static uint32_t recoverLogCount = 0;
                        if (recoverLogCount < 16)
                        {
                            RUNTIME_LOG("[sceCdRead] recovered with alternate args " << candidate.tag
                                                                                     << " (pc=0x" << std::hex << ctx->pc
                                                                                     << " ra=0x" << getRegU32(ctx, 31)
                                                                                     << " a0=0x" << a0
                                                                                     << " a1=0x" << a1
                                                                                     << " a2=0x" << a2 << std::dec << ")" << std::endl);
                            ++recoverLogCount;
                        }
                        selected = candidate;
                        ok = true;
                        break;
                    }
                }
            }

            if (!ok)
            {
                const uint32_t offset = a2 & PS2_RAM_MASK;
                const size_t bytes = clampReadBytes(a1, offset);
                if (bytes > 0)
                {
                    std::memset(rdram + offset, 0, bytes);
                }

                static uint32_t unresolvedLogCount = 0;
                if (unresolvedLogCount < 32)
                {
                    std::cerr << "[sceCdRead] unresolved request pc=0x" << std::hex << ctx->pc
                              << " ra=0x" << getRegU32(ctx, 31)
                              << " a0=0x" << a0
                              << " a1=0x" << a1
                              << " a2=0x" << a2 << std::dec << std::endl;
                    ++unresolvedLogCount;
                }
            }
        }

        if (ok)
        {
            g_cdStreamingLbn = selected.lbn + selected.sectors;
            return 1; // command accepted/success
        }

        return 0;
    }

    // int sceCdRead(u_int lbn, u_int sectors, void *buf, sceCdRMode *mode)
    PS2_STUB_TYPED(int32_t, sceCdRead, uint32_t lbn, uint32_t sectors, ps2_abi::gptr buf, ps2_abi::gptr mode)
    {
        (void)mode;
        return cdReadBody(rdram, ctx, runtime, lbn, sectors, buf);
    }

    void sceCdRead_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4); // usually lbn
        const uint32_t a1 = getRegU32(ctx, 5); // usually sector count
        const uint32_t a2 = getRegU32(ctx, 6); // usually destination buffer

        setReturnS32(ctx, cdReadBody(rdram, ctx, runtime, a0, a1, a2));
    }

    PS2_STUB_DISPATCH(sceCdRead)

    // int sceCdSync(int mode): 0 = completed / not busy
    PS2_STUB_TYPED(int32_t, sceCdSync, int32_t mode)
    {
        (void)mode;
        return 0;
    }

    void sceCdSync_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0); // 0 = completed/not busy
    }

    PS2_STUB_DISPATCH(sceCdSync)

    // int sceCdGetError(void)
    PS2_STUB0_TYPED(int32_t, sceCdGetError)
    {
        return g_lastCdError;
    }

    void sceCdGetError_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_lastCdError);
    }

    PS2_STUB_DISPATCH(sceCdGetError)

    void sceCdRI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceCdRI", rdram, ctx, runtime);
    }

    void sceCdRM(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceCdRM", rdram, ctx, runtime);
    }

    // int sceCdApplyNCmd(u_char cmdNum, const void *inBuff, u_short inBuffSize)
    PS2_STUB_TYPED(int32_t, sceCdApplyNCmd, uint8_t cmdNum, ps2_abi::gptr inBuff, uint16_t inBuffSize)
    {
        (void)cmdNum;
        (void)inBuff;
        (void)inBuffSize;
        return 1;
    }

    void sceCdApplyNCmd_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdApplyNCmd)

    // int sceCdBreak(void)
    PS2_STUB0_TYPED(int32_t, sceCdBreak)
    {
        return 1;
    }

    void sceCdBreak_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdBreak)

    // void *sceCdCallback(void (*func)(int)): returns the previous callback (none here)
    PS2_STUB_TYPED(ps2_abi::gptr, sceCdCallback, ps2_abi::gptr func)
    {
        (void)func;
        return 0u;
    }

    void sceCdCallback_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceCdCallback)

    // int sceCdChangeThreadPriority(int priority)
    PS2_STUB_TYPED(int32_t, sceCdChangeThreadPriority, int32_t priority)
    {
        (void)priority;
        return 1;
    }

    void sceCdChangeThreadPriority_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdChangeThreadPriority)

    // sceCdDelayThread(int ticks) (libcdvd internal)
    PS2_STUB_TYPED(int32_t, sceCdDelayThread, int32_t ticks)
    {
        (void)ticks;
        return 0;
    }

    void sceCdDelayThread_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceCdDelayThread)

    // int sceCdDiskReady(int mode): 2 = SCECdComplete
    PS2_STUB_TYPED(int32_t, sceCdDiskReady, int32_t mode)
    {
        (void)mode;
        return 2;
    }

    void sceCdDiskReady_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 2);
    }

    PS2_STUB_DISPATCH(sceCdDiskReady)

    // int sceCdGetDiskType(void): SCECdPS2DVD
    PS2_STUB0_TYPED(int32_t, sceCdGetDiskType)
    {
        return 0x14;
    }

    void sceCdGetDiskType_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // SCECdPS2DVD
        setReturnS32(ctx, 0x14);
    }

    PS2_STUB_DISPATCH(sceCdGetDiskType)

    // u_int sceCdGetReadPos(void)
    PS2_STUB0_TYPED(uint32_t, sceCdGetReadPos)
    {
        return g_cdStreamingLbn;
    }

    void sceCdGetReadPos_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, g_cdStreamingLbn);
    }

    PS2_STUB_DISPATCH(sceCdGetReadPos)

    // int sceCdGetToc(u_char *toc)
    PS2_STUB_TYPED(int32_t, sceCdGetToc, ps2_abi::gptr tocAddr)
    {
        if (uint8_t *toc = getMemPtr(rdram, tocAddr))
        {
            std::memset(toc, 0, 1024);
        }
        return 1;
    }

    void sceCdGetToc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t tocAddr = getRegU32(ctx, 4);
        if (uint8_t *toc = getMemPtr(rdram, tocAddr))
        {
            std::memset(toc, 0, 1024);
        }
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdGetToc)

    // int sceCdInit(int init_mode)
    PS2_STUB_TYPED(int32_t, sceCdInit, int32_t initMode)
    {
        (void)initMode;
        g_cdInitialized = true;
        g_lastCdError = 0;
        g_cdStreamTiming = {};
        return 1;
    }

    void sceCdInit_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdInitialized = true;
        g_lastCdError = 0;
        g_cdStreamTiming = {};
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdInit)

    // int sceCdInitEeCB(int prio, void *stack, int stack_size)
    PS2_STUB_TYPED(int32_t, sceCdInitEeCB, int32_t prio, ps2_abi::gptr stack, int32_t stackSize)
    {
        (void)prio;
        (void)stack;
        (void)stackSize;
        return 1;
    }

    void sceCdInitEeCB_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdInitEeCB)

    // sceCdlLOCCD *sceCdIntToPos(int i, sceCdlLOCCD *p)
    PS2_STUB_TYPED(int32_t, sceCdIntToPos, int32_t lsnSigned, ps2_abi::gptr posAddr)
    {
        const uint32_t lsn = static_cast<uint32_t>(lsnSigned);
        uint8_t *pos = getMemPtr(rdram, posAddr);
        if (!pos)
        {
            return 0;
        }

        uint32_t adjusted = lsn + 150;
        const uint32_t minutes = adjusted / (60 * 75);
        adjusted %= (60 * 75);
        const uint32_t seconds = adjusted / 75;
        const uint32_t sectors = adjusted % 75;

        pos[0] = toBcd(minutes);
        pos[1] = toBcd(seconds);
        pos[2] = toBcd(sectors);
        pos[3] = 0;
        return 1;
    }

    void sceCdIntToPos_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t lsn = getRegU32(ctx, 4);
        uint32_t posAddr = getRegU32(ctx, 5);
        uint8_t *pos = getMemPtr(rdram, posAddr);
        if (!pos)
        {
            setReturnS32(ctx, 0);
            return;
        }

        uint32_t adjusted = lsn + 150;
        const uint32_t minutes = adjusted / (60 * 75);
        adjusted %= (60 * 75);
        const uint32_t seconds = adjusted / 75;
        const uint32_t sectors = adjusted % 75;

        pos[0] = toBcd(minutes);
        pos[1] = toBcd(seconds);
        pos[2] = toBcd(sectors);
        pos[3] = 0;
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdIntToPos)
    PS2_STUB_DEVIATION(sceCdIntToPos, "SDK: sceCdlLOCCD *sceCdIntToPos(int i, sceCdlLOCCD *p) returns p; the handler returns 1 (0 for a bad pointer) (kept)")

    // int sceCdMmode(int media)
    PS2_STUB_TYPED(int32_t, sceCdMmode, uint32_t media)
    {
        g_cdMode = media;
        return 1;
    }

    void sceCdMmode_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdMode = getRegU32(ctx, 4);
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdMmode)

    // int sceCdNcmdDiskReady(void)
    PS2_STUB0_TYPED(int32_t, sceCdNcmdDiskReady)
    {
        return 2;
    }

    void sceCdNcmdDiskReady_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 2);
    }

    PS2_STUB_DISPATCH(sceCdNcmdDiskReady)

    // int sceCdPause(void)
    PS2_STUB0_TYPED(int32_t, sceCdPause)
    {
        return 1;
    }

    void sceCdPause_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdPause)

    // int sceCdPosToInt(sceCdlLOCCD *p)
    PS2_STUB_TYPED(int32_t, sceCdPosToInt, ps2_abi::gptr posAddr)
    {
        const uint8_t *pos = getConstMemPtr(rdram, posAddr);
        if (!pos)
        {
            return -1;
        }

        const uint32_t minutes = fromBcd(pos[0]);
        const uint32_t seconds = fromBcd(pos[1]);
        const uint32_t sectors = fromBcd(pos[2]);
        const uint32_t absolute = (minutes * 60 * 75) + (seconds * 75) + sectors;
        const int32_t lsn = static_cast<int32_t>(absolute) - 150;
        return lsn;
    }

    void sceCdPosToInt_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t posAddr = getRegU32(ctx, 4);
        const uint8_t *pos = getConstMemPtr(rdram, posAddr);
        if (!pos)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t minutes = fromBcd(pos[0]);
        const uint32_t seconds = fromBcd(pos[1]);
        const uint32_t sectors = fromBcd(pos[2]);
        const uint32_t absolute = (minutes * 60 * 75) + (seconds * 75) + sectors;
        const int32_t lsn = static_cast<int32_t>(absolute) - 150;
        setReturnS32(ctx, lsn);
    }

    PS2_STUB_DISPATCH(sceCdPosToInt)

    // The body of sceCdReadChain after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t cdReadChainBody(uint8_t *rdram, uint32_t chainAddr)
    {
        bool ok = true;

        for (int i = 0; i < 64; ++i)
        {
            uint32_t *entry = reinterpret_cast<uint32_t *>(getMemPtr(rdram, chainAddr + (i * 16)));
            if (!entry)
            {
                ok = false;
                break;
            }

            const uint32_t lbn = entry[0];
            const uint32_t sectors = entry[1];
            const uint32_t buf = entry[2];
            if (lbn == 0xFFFFFFFFu || sectors == 0)
            {
                break;
            }

            uint32_t offset = buf & PS2_RAM_MASK;
            size_t bytes = static_cast<size_t>(sectors) * kCdSectorSize;
            const size_t maxBytes = PS2_RAM_SIZE - offset;
            if (bytes > maxBytes)
            {
                bytes = maxBytes;
            }

            if (!readCdSectors(lbn, sectors, rdram + offset, bytes))
            {
                ok = false;
                break;
            }

            g_cdStreamingLbn = lbn + sectors;
        }

        return ok ? 1 : 0;
    }

    // int sceCdReadChain(sceCdRChain *tag, sceCdRMode *mode)
    PS2_STUB_TYPED(int32_t, sceCdReadChain, ps2_abi::gptr tag, ps2_abi::gptr mode)
    {
        (void)ctx;
        (void)runtime;
        (void)mode;
        return cdReadChainBody(rdram, tag);
    }

    void sceCdReadChain_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t chainAddr = getRegU32(ctx, 4);
        setReturnS32(ctx, cdReadChainBody(rdram, chainAddr));
    }

    PS2_STUB_DISPATCH(sceCdReadChain)

    // The body of sceCdReadClock after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t cdReadClockBody(uint8_t *rdram, uint32_t clockAddr)
    {
        uint8_t *clockData = getMemPtr(rdram, clockAddr);
        if (!clockData)
        {
            return 0;
        }

        const std::time_t now = ps2x::det::now();
        std::tm localTm{};
        ps2x::det::calendar(&now, &localTm);

        // sceCdCLOCK format (BCD fields).
        clockData[0] = 0;
        clockData[1] = toBcd(static_cast<uint32_t>(localTm.tm_sec));
        clockData[2] = toBcd(static_cast<uint32_t>(localTm.tm_min));
        clockData[3] = toBcd(static_cast<uint32_t>(localTm.tm_hour));
        clockData[4] = 0;
        clockData[5] = toBcd(static_cast<uint32_t>(localTm.tm_mday));
        clockData[6] = toBcd(static_cast<uint32_t>(localTm.tm_mon + 1));
        clockData[7] = toBcd(static_cast<uint32_t>((localTm.tm_year + 1900) % 100));
        return 1;
    }

    // int sceCdReadClock(sceCdCLOCK *rtc)
    PS2_STUB_TYPED(int32_t, sceCdReadClock, ps2_abi::gptr rtc)
    {
        (void)ctx;
        (void)runtime;
        return cdReadClockBody(rdram, rtc);
    }

    void sceCdReadClock_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clockAddr = getRegU32(ctx, 4);
        setReturnS32(ctx, cdReadClockBody(rdram, clockAddr));
    }

    PS2_STUB_DISPATCH(sceCdReadClock)

    PS2_STUB_ALIAS(sceCdReadIOPm, sceCdRead, ::ps2_abi::Kind::Stub)

    // The body of sceCdSearchFile after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t cdSearchFileBody(uint8_t *rdram, R5900Context *ctx, uint32_t fileAddr, uint32_t pathAddr)
    {
        const std::string path = readPs2CStringBounded(rdram, pathAddr, 260);
        const std::string normalizedPath = normalizeCdPathNoPrefix(path);
        static uint32_t traceCount = 0;
        const uint32_t callerRa = getRegU32(ctx, 31);
        const bool shouldTrace = (traceCount < 128u) || ((traceCount % 512u) == 0u);
        if (shouldTrace)
        {
            RUNTIME_LOG("[sceCdSearchFile] pc=0x" << std::hex << ctx->pc
                                                  << " ra=0x" << callerRa
                                                  << " file=0x" << fileAddr
                                                  << " pathAddr=0x" << pathAddr
                                                  << " path=\"" << sanitizeForLog(path) << "\""
                                                  << std::dec << std::endl);
        }
        ++traceCount;

        if (path.empty())
        {
            static uint32_t emptyPathCount = 0;
            if (emptyPathCount < 64 || (emptyPathCount % 512u) == 0u)
            {
                std::ostringstream preview;
                preview << std::hex;
                for (uint32_t i = 0; i < 16; ++i)
                {
                    const uint8_t byte = *getConstMemPtr(rdram, pathAddr + i);
                    preview << (i == 0 ? "" : " ") << static_cast<uint32_t>(byte);
                }
                std::cerr << "[sceCdSearchFile] empty path at 0x" << std::hex << pathAddr
                          << " preview=" << preview.str()
                          << " ra=0x" << callerRa << std::dec << std::endl;
            }
            ++emptyPathCount;
            g_lastCdError = -1;
            return 0;
        }

        if (normalizedPath.empty())
        {
            static uint32_t emptyNormalizedCount = 0;
            if (emptyNormalizedCount < 64u || (emptyNormalizedCount % 512u) == 0u)
            {
                std::cerr << "sceCdSearchFile failed: " << sanitizeForLog(path)
                          << " (normalized path is empty, root: " << getCdRootPath().string() << ")"
                          << std::endl;
            }
            ++emptyNormalizedCount;
            g_lastCdError = -1;
            return 0;
        }

        CdFileEntry entry;
        bool found = registerCdFile(path, entry);
        CdFileEntry resolvedEntry = entry;
        std::string resolvedPath;

        if (!found)
        {
            static std::string lastFailedPath;
            static uint32_t samePathFailCount = 0;
            if (path == lastFailedPath)
            {
                ++samePathFailCount;
            }
            else
            {
                lastFailedPath = path;
                samePathFailCount = 1;
            }

            if (samePathFailCount <= 16u || (samePathFailCount % 512u) == 0u)
            {
                std::cerr << "sceCdSearchFile failed: " << sanitizeForLog(path)
                          << " (root: " << getCdRootPath().string()
                          << ", repeat=" << samePathFailCount << ")" << std::endl;
            }
            return 0;
        }

        if (!writeCdSearchResult(rdram, fileAddr, path, resolvedEntry))
        {
            g_lastCdError = -1;
            return 0;
        }

        g_cdStreamingLbn = resolvedEntry.baseLbn;
        g_cdStreamingEndLbn = resolvedEntry.baseLbn + resolvedEntry.sectors;
        if (shouldTrace)
        {
            RUNTIME_LOG("[sceCdSearchFile:ok] path=\"" << sanitizeForLog(path)
                                                       << "\" lsn=0x" << std::hex << resolvedEntry.baseLbn
                                                       << " size=0x" << resolvedEntry.sizeBytes
                                                       << " sectors=0x" << resolvedEntry.sectors
                                                       << std::dec << std::endl);
        }
        return 1;
    }

    // int sceCdSearchFile(sceCdlFILE *fp, const char *name)
    PS2_STUB_TYPED(int32_t, sceCdSearchFile, ps2_abi::gptr fp, ps2_abi::gptr name)
    {
        (void)runtime;
        return cdSearchFileBody(rdram, ctx, fp, name);
    }

    void sceCdSearchFile_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t fileAddr = getRegU32(ctx, 4);
        uint32_t pathAddr = getRegU32(ctx, 5);
        setReturnS32(ctx, cdSearchFileBody(rdram, ctx, fileAddr, pathAddr));
    }

    PS2_STUB_DISPATCH(sceCdSearchFile)

    // int sceCdSeek(u_int lbn)
    PS2_STUB_TYPED(int32_t, sceCdSeek, uint32_t lbn)
    {
        restartCdStreamAt(lbn, runtime);
        return 1;
    }

    void sceCdSeek_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        restartCdStreamAt(getRegU32(ctx, 4), runtime);
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdSeek)

    // int sceCdStandby(void)
    PS2_STUB0_TYPED(int32_t, sceCdStandby)
    {
        return 1;
    }

    void sceCdStandby_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdStandby)

    // int sceCdStatus(void)
    PS2_STUB0_TYPED(int32_t, sceCdStatus)
    {
        return g_cdInitialized ? 6 : 0;
    }

    void sceCdStatus_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_cdInitialized ? 6 : 0);
    }

    PS2_STUB_DISPATCH(sceCdStatus)

    // The body of sceCdStInit after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t cdStInitBody(PS2Runtime *runtime, uint32_t bufferSectors, uint32_t bankCount, uint32_t bufferAddr)
    {
        if (bufferSectors == 0u || bankCount == 0u || bufferAddr == 0u || bufferSectors / bankCount == 0u)
        {
            return 0;
        }

        g_cdStreamTiming.initialized = true;
        g_cdStreamTiming.active = false;
        g_cdStreamTiming.paused = false;
        g_cdStreamTiming.capacitySectors = bufferSectors;
        g_cdStreamTiming.bankCount = bankCount;
        g_cdStreamTiming.sectorsPerBank = std::max(1u, bufferSectors / bankCount);
        resetCdStreamProduction(runtime);
        return 1;
    }

    // int sceCdStInit(u_int bufmax, u_int bankmax, u_int iop_bufaddr)
    PS2_STUB_TYPED(int32_t, sceCdStInit, uint32_t bufmax, uint32_t bankmax, uint32_t iopBufAddr)
    {
        (void)rdram;
        (void)ctx;
        return cdStInitBody(runtime, bufmax, bankmax, iopBufAddr);
    }

    void sceCdStInit_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t bufferSectors = getRegU32(ctx, 4);
        const uint32_t bankCount = getRegU32(ctx, 5);
        const uint32_t bufferAddr = getRegU32(ctx, 6);

        setReturnS32(ctx, cdStInitBody(runtime, bufferSectors, bankCount, bufferAddr));
    }

    PS2_STUB_DISPATCH(sceCdStInit)

    // int sceCdStop(void)
    PS2_STUB0_TYPED(int32_t, sceCdStop)
    {
        return 1;
    }

    void sceCdStop_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdStop)

    // int sceCdStPause(void)
    PS2_STUB0_TYPED(int32_t, sceCdStPause)
    {
        updateCdStreamProduction(runtime);
        g_cdStreamTiming.paused = true;
        return 1;
    }

    void sceCdStPause_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        updateCdStreamProduction(runtime);
        g_cdStreamTiming.paused = true;
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdStPause)

    namespace
    {
        struct CdStReadContinuation
        {
            uint32_t requestedSectors = 0u;
            uint32_t buffer = 0u;
            uint32_t errorAddress = 0u;
            uint32_t sectorsRead = 0u;
        };

        void finishCdStRead(uint8_t *rdram, R5900Context *ctx, const CdStReadContinuation &state, int32_t error)
        {
            if (int32_t *errorOut = reinterpret_cast<int32_t *>(getMemPtr(rdram, state.errorAddress)); errorOut)
            {
                *errorOut = error;
            }
            setReturnS32(ctx, static_cast<int32_t>(state.sectorsRead));
        }

        void continueCdStRead(uint8_t *rdram,
                              R5900Context *ctx,
                              PS2Runtime *runtime,
                              CdStReadContinuation state)
        {
            if (!g_cdStreamTiming.active || state.requestedSectors == 0u)
            {
                finishCdStRead(rdram, ctx, state, 0);
                return;
            }

            for (;;)
            {
                uint32_t remaining = state.requestedSectors - state.sectorsRead;
                bool atEnd = false;
                if (g_cdStreamingEndLbn != 0xFFFFFFFFu)
                {
                    if (g_cdStreamingLbn >= g_cdStreamingEndLbn)
                    {
                        remaining = 0u;
                        atEnd = true;
                    }
                    else
                    {
                        const uint32_t streamRemaining = g_cdStreamingEndLbn - g_cdStreamingLbn;
                        if (remaining > streamRemaining)
                        {
                            remaining = streamRemaining;
                            atEnd = true;
                        }
                    }
                }

                if (remaining == 0u)
                {
                    if (atEnd || (g_cdStreamingEndLbn != 0xFFFFFFFFu && g_cdStreamingLbn >= g_cdStreamingEndLbn))
                    {
                        notifyMpegCdStreamEof(runtime);
                    }
                    finishCdStRead(rdram, ctx, state, 0);
                    return;
                }

                uint32_t available = readableCdStreamSectors(runtime);
                if (runtime == nullptr)
                {
                    available = remaining;
                }

                if (available == 0u)
                {
                    if (!runtime)
                    {
                        finishCdStRead(rdram, ctx, state, 0);
                        return;
                    }

                    const uint32_t bank = std::max(1u, g_cdStreamTiming.sectorsPerBank);
                    const uint32_t wakeSectors = std::min(remaining, bank);
                    const uint32_t buffered = bufferedCdStreamSectors(runtime);
                    const uint32_t needed = wakeSectors > buffered ? wakeSectors - buffered : 1u;
                    const uint64_t wakeTick = cdStreamWakeTickForSectors(runtime, needed);
                    runtime->eeScheduler().waitVSync(
                        wakeTick - 1u,
                        -1,
                        [rdram, runtime, state](R5900Context &resumeContext)
                        {
                            if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
                            {
                                return;
                            }
                            continueCdStRead(rdram, &resumeContext, runtime, state);
                        },
                        EeResumeTag{EeResumeKind::CdStRead,
                                    {state.requestedSectors, state.buffer, state.errorAddress, state.sectorsRead}});
                }

                uint32_t sectors = std::min(remaining, available);
                const uint64_t destination64 = static_cast<uint64_t>(state.buffer) + static_cast<uint64_t>(state.sectorsRead) * kCdSectorSize;
                const uint32_t destination = static_cast<uint32_t>(destination64);
                const uint32_t offset = destination & PS2_RAM_MASK;
                const size_t maxBytes = PS2_RAM_SIZE - offset;
                sectors = std::min<uint32_t>(sectors, static_cast<uint32_t>(maxBytes / kCdSectorSize));

                if (sectors == 0u)
                {
                    g_lastCdError = -1;
                    finishCdStRead(rdram, ctx, state, g_lastCdError);
                    return;
                }

                const uint32_t readLbn = g_cdStreamingLbn;
                const size_t readBytes = static_cast<size_t>(sectors) * kCdSectorSize;
                if (!readCdSectors(readLbn, sectors, rdram + offset, readBytes))
                {
                    finishCdStRead(rdram, ctx, state, g_lastCdError);
                    return;
                }

                g_cdStreamingLbn += sectors;
                if (runtime == nullptr)
                {
                    g_cdStreamTiming.producedSectors += sectors;
                }
                g_cdStreamTiming.consumedSectors += sectors;
                state.sectorsRead += sectors;

                const bool hitStreamEnd = g_cdStreamingEndLbn != 0xFFFFFFFFu && g_cdStreamingLbn >= g_cdStreamingEndLbn;
                notifyMpegCdStreamDataProduced(static_cast<uint32_t>(readBytes), hitStreamEnd);

                if (g_cdStReadTraceCount < 32u)
                {
                    std::cerr << "[sceCdStRead] requested=" << state.requestedSectors
                              << " accumulated=" << state.sectorsRead
                              << " chunk=" << sectors
                              << " buffered=" << readableCdStreamSectors(runtime)
                              << " lbn=0x" << std::hex << readLbn
                              << " end=0x" << g_cdStreamingEndLbn
                              << std::dec << std::endl;
                    ++g_cdStReadTraceCount;
                }

                if (state.sectorsRead >= state.requestedSectors || hitStreamEnd)
                {
                    finishCdStRead(rdram, ctx, state, 0);
                    return;
                }

                // STMBLK is implemented by repeatedly consuming the stream ring.
                // If a complete bank is still available, keep draining it before
                // yielding.  Otherwise the next iteration parks on the producer.
            }
        }
    }

    // The body of sceCdStRead after decoding, shared by the typed and the legacy form. It writes v0
    // itself, possibly after a wait for the stream (continueCdStRead).
    static void cdStReadBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t requestedSectors,
                             uint32_t buffer, uint32_t mode, uint32_t errorAddress)
    {
        CdStReadContinuation state{};
        state.requestedSectors = requestedSectors;
        state.buffer = buffer;
        state.errorAddress = errorAddress;

        if (int32_t *errorOut = reinterpret_cast<int32_t *>(getMemPtr(rdram, errorAddress)); errorOut)
        {
            *errorOut = 0;
        }

        if (!g_cdStreamTiming.active || requestedSectors == 0u)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (mode == kCdStreamBlocking)
        {
            continueCdStRead(rdram, ctx, runtime, state);
            return;
        }

        uint32_t remaining = requestedSectors;
        if (g_cdStreamingEndLbn != 0xFFFFFFFFu)
        {
            remaining = g_cdStreamingLbn < g_cdStreamingEndLbn
                            ? std::min(remaining, g_cdStreamingEndLbn - g_cdStreamingLbn)
                            : 0u;
        }

        const uint32_t available = runtime != nullptr
                                       ? readableCdStreamSectors(runtime)
                                       : remaining;
        const uint32_t sectors = std::min(remaining, available);
        if (sectors == 0u)
        {
            if (g_cdStreamingEndLbn != 0xFFFFFFFFu && g_cdStreamingLbn >= g_cdStreamingEndLbn)
            {
                notifyMpegCdStreamEof(runtime);
            }
            setReturnS32(ctx, 0);
            return;
        }

        state.requestedSectors = sectors;
        continueCdStRead(rdram, ctx, runtime, state);
    }

    // int sceCdStRead(u_int sectors, u_int *buf, u_int mode, u_int *err)
    PS2_STUB_TYPED(ps2_abi::SelfReturn<int32_t>, sceCdStRead, uint32_t sectors, ps2_abi::gptr buf, uint32_t mode,
                   ps2_abi::gptr err)
    {
        cdStReadBody(rdram, ctx, runtime, sectors, buf, mode, err);
        return {};
    }

    void sceCdStRead_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t requestedSectors = getRegU32(ctx, 4);
        const uint32_t buffer = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);
        const uint32_t errorAddress = getRegU32(ctx, 7);

        cdStReadBody(rdram, ctx, runtime, requestedSectors, buffer, mode, errorAddress);
    }

    PS2_STUB_DISPATCH(sceCdStRead)

    // int sceCdStream(u_int lbn, u_int nsectors, void *buf, int cmd, sceCdRMode *mode)
    PS2_STUB_TYPED(int32_t, sceCdStream, uint32_t lbn, uint32_t nsectors, ps2_abi::gptr buf, int32_t cmd, ps2_abi::gptr mode)
    {
        (void)lbn;
        (void)nsectors;
        (void)buf;
        (void)cmd;
        (void)mode;
        return 1;
    }

    void sceCdStream_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdStream)

    // int sceCdStResume(void)
    PS2_STUB0_TYPED(int32_t, sceCdStResume)
    {
        g_cdStreamTiming.paused = false;
        g_cdStreamTiming.lastVSyncTick = currentCdStreamTick(runtime);
        return 1;
    }

    void sceCdStResume_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdStreamTiming.paused = false;
        g_cdStreamTiming.lastVSyncTick = currentCdStreamTick(runtime);
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdStResume)

    // int sceCdStSeek(u_int lbn)
    PS2_STUB_TYPED(int32_t, sceCdStSeek, uint32_t lbn)
    {
        restartCdStreamAt(lbn, runtime);
        return 1;
    }

    void sceCdStSeek_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        restartCdStreamAt(getRegU32(ctx, 4), runtime);
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdStSeek)

    // int sceCdStSeekF(u_int lbn)
    PS2_STUB_TYPED(int32_t, sceCdStSeekF, uint32_t lbn)
    {
        restartCdStreamAt(lbn, runtime);
        return 1;
    }

    void sceCdStSeekF_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        restartCdStreamAt(getRegU32(ctx, 4), runtime);
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdStSeekF)

    // The body of sceCdStStart after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t cdStStartBody(uint8_t *rdram, PS2Runtime *runtime, uint32_t lbn, uint32_t modeAddr)
    {
        uint8_t spindleControl = 0u;
        if (const uint8_t *mode = getConstMemPtr(rdram, modeAddr); mode)
        {
            spindleControl = mode[1u];
        }

        restartCdStreamAt(lbn, runtime);
        g_cdStreamTiming.active = true;
        g_cdStreamTiming.paused = false;
        g_cdStreamTiming.sectorsPerSecond = dvdStreamSectorsPerSecond(spindleControl);
        g_cdStReadTraceCount = 0u;

        notifyMpegCdStreamStart(runtime);

        std::cerr << "[sceCdStStart] lbn=0x" << std::hex << g_cdStreamingLbn
                  << " endLbn=0x" << g_cdStreamingEndLbn << std::dec
                  << " rate=" << g_cdStreamTiming.sectorsPerSecond << " sectors/s"
                  << " buffer=" << g_cdStreamTiming.capacitySectors << " sectors"
                  << " bank=" << g_cdStreamTiming.sectorsPerBank << " sectors"
                  << std::endl;
        return 1;
    }

    // int sceCdStStart(u_int lbn, sceCdRMode *mode)
    PS2_STUB_TYPED(int32_t, sceCdStStart, uint32_t lbn, ps2_abi::gptr mode)
    {
        (void)ctx;
        return cdStStartBody(rdram, runtime, lbn, mode);
    }

    void sceCdStStart_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t lbn = getRegU32(ctx, 4);
        const uint32_t modeAddr = getRegU32(ctx, 5);

        setReturnS32(ctx, cdStStartBody(rdram, runtime, lbn, modeAddr));
    }

    PS2_STUB_DISPATCH(sceCdStStart)

    // int sceCdStStat(void)
    PS2_STUB0_TYPED(int32_t, sceCdStStat)
    {
        const uint32_t buffered = bufferedCdStreamSectors(runtime);
        const uint32_t bankSize = std::max(1u, g_cdStreamTiming.sectorsPerBank);
        return static_cast<int32_t>((buffered / bankSize) * bankSize);
    }

    void sceCdStStat_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t buffered = bufferedCdStreamSectors(runtime);
        const uint32_t bankSize = std::max(1u, g_cdStreamTiming.sectorsPerBank);
        setReturnS32(ctx, static_cast<int32_t>((buffered / bankSize) * bankSize));
    }

    PS2_STUB_DISPATCH(sceCdStStat)

    // int sceCdStStop(void)
    PS2_STUB0_TYPED(int32_t, sceCdStStop)
    {
        updateCdStreamProduction(runtime);
        g_cdStreamTiming.active = false;
        g_cdStreamTiming.paused = false;
        notifyMpegCdStreamEof(runtime);
        return 1;
    }

    void sceCdStStop_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        updateCdStreamProduction(runtime);
        g_cdStreamTiming.active = false;
        g_cdStreamTiming.paused = false;
        notifyMpegCdStreamEof(runtime);
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdStStop)

    // int sceCdSyncS(int mode)
    PS2_STUB_TYPED(int32_t, sceCdSyncS, int32_t mode)
    {
        (void)mode;
        return 0;
    }

    void sceCdSyncS_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceCdSyncS)

    // int sceCdTrayReq(int param, u_int *traycnt)
    PS2_STUB_TYPED(int32_t, sceCdTrayReq, int32_t param, ps2_abi::gptr statusPtr)
    {
        (void)param;
        if (uint32_t *status = reinterpret_cast<uint32_t *>(getMemPtr(rdram, statusPtr)); status)
        {
            *status = 0;
        }
        return 1;
    }

    void sceCdTrayReq_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t statusPtr = getRegU32(ctx, 5);
        if (uint32_t *status = reinterpret_cast<uint32_t *>(getMemPtr(rdram, statusPtr)); status)
        {
            *status = 0;
        }
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceCdTrayReq)

    // The CD file registry for the IOP side (PS2IopHostAdapter): the same pseudo-LSN space
    // the EE's sceCdSearchFile hands out, read back from the extracted files or the image.
    bool cdLookupFile(const std::string &ps2Path, uint32_t &lbn, uint32_t &sizeBytes)
    {
        CdFileEntry entry;
        if (!registerCdFile(ps2Path, entry))
            return false;
        lbn = entry.baseLbn;
        sizeBytes = entry.sizeBytes;
        return true;
    }

    bool cdReadSectorsByLbn(uint32_t lbn, uint32_t sectors, uint8_t *dst, size_t byteCount)
    {
        return readCdSectors(lbn, sectors, dst, byteCount);
    }
}

#include "ps2_snapshot_hooks.h"

namespace ps2_stubs::snapshot
{
    void saveCd(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
        w.pod(g_cdStreamTiming);
    }

    void loadCd(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        (void)runtime;
        snapshotLoadSupport(r);
        r.pod(g_cdStreamTiming);
    }

    std::function<void(R5900Context &)> makeCdStReadResume(PS2Runtime &runtime, const EeResumeTag &tag)
    {
        CdStReadContinuation state{};
        state.requestedSectors = static_cast<uint32_t>(tag.args[0]);
        state.buffer = static_cast<uint32_t>(tag.args[1]);
        state.errorAddress = static_cast<uint32_t>(tag.args[2]);
        state.sectorsRead = static_cast<uint32_t>(tag.args[3]);
        PS2Runtime *rt = &runtime;
        uint8_t *rdram = runtime.memory().getRDRAM();
        return [rdram, rt, state](R5900Context &resumeContext)
        {
            if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
            {
                return;
            }
            continueCdStRead(rdram, &resumeContext, rt, state);
        };
    }
}
