#include "Common.h"
#include "SIF.h"
#include "../Syscalls/RPC.h"
#include "../../ps2_iop_transport.h"
#include "runtime/ps2_address.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace ps2_stubs
{
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    PS2_STUB_ALIAS(sceSifLoadModule, ps2_syscalls::SifLoadModule, ::ps2_abi::Kind::Syscall)

    // sceSifSendCmd(u_int cid, void *packet, int packet_size, void *src_extra, void *dest_extra,
    //               int size_extra). Shadowed by ps2_syscalls::sceSifSendCmd when bound by name.
    // dest and size are arguments 5-6, in t0/t1 (game's copy 0x11e530).
    PS2_STUB_TYPED(int32_t, sceSifSendCmd, uint32_t cid, ps2_abi::gptr packet, uint32_t packetSize,
                   ps2_abi::gptr srcAddr, ps2_abi::gptr dstAddr, uint32_t size)
    {
        (void)cid;
        (void)packet;
        (void)packetSize;
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            std::vector<uint8_t> payload(size);
            bool valid = runtime != nullptr;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                if (!src)
                {
                    valid = false;
                    break;
                }
                payload[i] = *src;
            }
            if (!valid || !runtime->writeIopMemory(dstAddr, payload.data(), payload.size()))
            {
                return 0;
            }
        }

        return 1;
    }

    void sceSifSendCmd_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Shadowed by ps2_syscalls::sceSifSendCmd when bound by name; kept in step with it.
        // dest and size are arguments 5-6, in t0/t1 (game's copy 0x11e530).
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = getRegU32(ctx, 8); // $t0
        const uint32_t size = getRegU32(ctx, 9);    // $t1
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            std::vector<uint8_t> payload(size);
            bool valid = runtime != nullptr;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                if (!src)
                {
                    valid = false;
                    break;
                }
                payload[i] = *src;
            }
            if (!valid || !runtime->writeIopMemory(dstAddr, payload.data(), payload.size()))
            {
                setReturnS32(ctx, 0);
                return;
            }
        }

        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceSifSendCmd)

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        struct SifCmdHandler
        {
            uint32_t function = 0u;
            uint32_t argument = 0u;
        };

        std::unordered_map<uint32_t, SifCmdHandler> g_sifCmdHandlers;
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        constexpr uint32_t kSifRegBootStatus = 0x4u;
        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;
        constexpr uint32_t kSifBootReadyMask = 0x00020000u;

        void seedDefaultSifRegsLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;

            g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x2u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                seedDefaultSifRegsLocked();
            }
        } g_sifStateInitializer;

        uint32_t allocateSifDmaTransferId()
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            return id;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (Ps2AddressInRange(addr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < PS2_EE_UNCACHED_RAM_MIRROR_BASE)
            {
                return true;
            }

            if (Ps2IsUncachedRamMirrorAddress(addr))
            {
                return true;
            }

            if (Ps2IsKseg01Address(addr))
            {
                return true;
            }

            return false;
        }

        bool canAccessEeRange(const uint8_t *rdram, uint32_t address, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }
            if (sizeBytes == 0u)
            {
                return true;
            }
            if (sizeBytes - 1u > std::numeric_limits<uint32_t>::max() - address)
            {
                return false;
            }
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t byteAddress = address + i;
                if (!isCopyableGuestAddress(byteAddress) || getConstMemPtr(rdram, byteAddress) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        bool readEeRange(const uint8_t *rdram, uint32_t address, void *destination, uint32_t sizeBytes)
        {
            if ((!destination && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            auto *bytes = static_cast<uint8_t *>(destination);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint8_t *source = getConstMemPtr(rdram, address + i);
                if (!source)
                    return false;
                bytes[i] = *source;
            }
            return true;
        }

        bool writeEeRange(uint8_t *rdram, uint32_t address, const void *source, uint32_t sizeBytes)
        {
            if ((!source && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            ps2TraceGuestRangeWrite(rdram, address, sizeBytes, "SIF IOP-to-EE DMA", nullptr);
            const auto *bytes = static_cast<const uint8_t *>(source);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                uint8_t *destination = getMemPtr(rdram, address + i);
                if (!destination)
                    return false;
                *destination = bytes[i];
            }
            return true;
        }
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
    }

    bool dispatchSifCommand(uint8_t *rdram,
                            PS2Runtime *runtime,
                            uint32_t commandId,
                            const void *packet,
                            size_t packetSize) noexcept
    {
        if (!rdram || !runtime || !packet || packetSize < 16u || packetSize > 112u)
            return false;

        SifCmdHandler registered{};
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            const auto handler = g_sifCmdHandlers.find(commandId);
            if (handler == g_sifCmdHandlers.end() || handler->second.function == 0u)
                return false;
            registered = handler->second;
        }

        if (!runtime->hasFunction(registered.function))
            return false;

        const uint32_t packetAddress = runtime->guestMalloc(static_cast<uint32_t>(packetSize), 16u);
        if (packetAddress == 0u)
            return false;

        uint8_t *const first = getMemPtr(rdram, packetAddress);
        uint8_t *const last = getMemPtr(rdram, packetAddress + static_cast<uint32_t>(packetSize - 1u));
        if (!first || !last || last < first || static_cast<size_t>(last - first) != packetSize - 1u)
        {
            runtime->guestFree(packetAddress);
            return false;
        }

        ps2TraceGuestRangeWrite(rdram, packetAddress, static_cast<uint32_t>(packetSize), "SIF command packet", nullptr);
        std::memcpy(first, packet, packetSize);

        try
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::SifCommand;
            invocation.tag = commandId;
            invocation.context = runtime->cpu();
            invocation.context.pc = registered.function;
            SET_GPR_U32(&invocation.context, 4, packetAddress);
            SET_GPR_U32(&invocation.context, 5, registered.argument);
            SET_GPR_U32(&invocation.context, 6, 0u);
            SET_GPR_U32(&invocation.context, 7, 0u);
            SET_GPR_U32(&invocation.context, 29, 0u);
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [runtime, packetAddress](const R5900Context &, R5900Context &)
            {
                runtime->guestFree(packetAddress);
            };
            invocation.completeTag = {GuestInvocationCompleteKind::GuestFree, packetAddress};
            runtime->eeScheduler().queueInvocation(std::move(invocation));
            return true;
        }
        catch (...)
        {
            runtime->guestFree(packetAddress);
            return false;
        }
    }

    // sceSifAddCmdHandler(u_int cid, sceSifCmdHandler handler, void *data)
    PS2_STUB_TYPED(int32_t, sceSifAddCmdHandler, uint32_t cid, ps2_abi::gptr handler, ps2_abi::gptr argument)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = SifCmdHandler{handler, argument};
        return 0;
    }

    void sceSifAddCmdHandler_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        const uint32_t argument = getRegU32(ctx, 6);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = SifCmdHandler{handler, argument};
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifAddCmdHandler)
    PS2_STUB_DEVIATION(sceSifAddCmdHandler, "SDK: void sceSifAddCmdHandler(u_int cid, sceSifCmdHandler handler, void *data); the handler writes 0 to v0 (kept)")

    // void *sceSifAllocIopHeap(int size)
    PS2_STUB_TYPED(ps2_abi::gptr, sceSifAllocIopHeap, int32_t size)
    {
        (void)rdram;
        return runtime ? runtime->allocateIopMemory(static_cast<uint32_t>(size), 64u) : 0u;
    }

    void sceSifAllocIopHeap_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t reqSize = getRegU32(ctx, 4);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(reqSize, 64u) : 0u);
    }

    PS2_STUB_DISPATCH(sceSifAllocIopHeap)

    // void *sceSifAllocSysMemory(int type, int size, void *addr)
    PS2_STUB_TYPED(ps2_abi::gptr, sceSifAllocSysMemory, int32_t type, int32_t size, ps2_abi::gptr addr)
    {
        (void)rdram;
        (void)type;
        (void)addr;
        return runtime ? runtime->allocateIopMemory(static_cast<uint32_t>(size), 64u) : 0u;
    }

    void sceSifAllocSysMemory_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t size = getRegU32(ctx, 5);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(size, 64u) : 0u);
    }

    PS2_STUB_DISPATCH(sceSifAllocSysMemory)

    PS2_STUB_ALIAS(sceSifBindRpc, ps2_syscalls::SifBindRpc, ::ps2_abi::Kind::Syscall)

    PS2_STUB_ALIAS(sceSifCheckStatRpc, ps2_syscalls::SifCheckStatRpc, ::ps2_abi::Kind::Syscall)

    // int sceSifDmaStat(unsigned int id)
    PS2_STUB_TYPED(int32_t, sceSifDmaStat, uint32_t id)
    {
        (void)rdram;
        (void)runtime;
        (void)id;

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        return -1;
    }

    void sceSifDmaStat_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        (void)getRegU32(ctx, 4); // trid

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        setReturnS32(ctx, -1);
    }

    PS2_STUB_DISPATCH(sceSifDmaStat)

    // void sceSifExecRequest(sceSifServeData *sd)
    PS2_STUB_TYPED(int32_t, sceSifExecRequest, ps2_abi::gptr sd)
    {
        (void)sd;
        return 0;
    }

    void sceSifExecRequest_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifExecRequest)
    PS2_STUB_DEVIATION(sceSifExecRequest, "SDK: void sceSifExecRequest(sceSifServeData *sd); the handler writes 0 to v0 (kept)")

    // void sceSifExitCmd(void)
    PS2_STUB0_TYPED(int32_t, sceSifExitCmd)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        return 0;
    }

    void sceSifExitCmd_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifExitCmd)
    PS2_STUB_DEVIATION(sceSifExitCmd, "SDK: void sceSifExitCmd(void); the handler writes 0 to v0 (kept)")

    // void sceSifExitRpc(void)
    PS2_STUB0_TYPED(int32_t, sceSifExitRpc)
    {
        return 0;
    }

    void sceSifExitRpc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifExitRpc)
    PS2_STUB_DEVIATION(sceSifExitRpc, "SDK: void sceSifExitRpc(void); the handler writes 0 to v0 (kept)")

    // int sceSifFreeIopHeap(void *addr)
    PS2_STUB_TYPED(int32_t, sceSifFreeIopHeap, ps2_abi::gptr addr)
    {
        (void)rdram;
        return runtime && runtime->freeIopMemory(addr) ? 0 : -1;
    }

    void sceSifFreeIopHeap_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    PS2_STUB_DISPATCH(sceSifFreeIopHeap)

    // int sceSifFreeSysMemory(void *addr)
    PS2_STUB_TYPED(int32_t, sceSifFreeSysMemory, ps2_abi::gptr addr)
    {
        (void)rdram;
        return runtime && runtime->freeIopMemory(addr) ? 0 : -1;
    }

    void sceSifFreeSysMemory_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    PS2_STUB_DISPATCH(sceSifFreeSysMemory)

    // unsigned int *sceSifGetDataTable(void)
    PS2_STUB0_TYPED(ps2_abi::gptr, sceSifGetDataTable)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        return g_sifCmdBuffer;
    }

    void sceSifGetDataTable_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    PS2_STUB_DISPATCH(sceSifGetDataTable)

    // sceSifGetIopAddr(addr): returns its argument (identity mapping).
    PS2_STUB_TYPED(uint32_t, sceSifGetIopAddr, uint32_t addr)
    {
        return addr;
    }

    void sceSifGetIopAddr_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    PS2_STUB_DISPATCH(sceSifGetIopAddr)

    // sceSifServeData *sceSifGetNextRequest(sceSifQueueData *qd)
    PS2_STUB_TYPED(ps2_abi::gptr, sceSifGetNextRequest, ps2_abi::gptr qd)
    {
        (void)qd;
        return 0u;
    }

    void sceSifGetNextRequest_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifGetNextRequest)

    // The body of sceSifGetOtherData after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sifGetOtherDataBody(uint8_t *rdram, PS2Runtime *runtime, uint32_t rdAddr, uint32_t srcAddr,
                                       uint32_t dstAddr, int32_t sizeSigned)
    {
        if (sizeSigned <= 0)
        {
            return 0;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            return -1;
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        std::vector<uint8_t> payload(size);
        if (!runtime || !runtime->isIopMemoryRange(srcAddr, size) ||
            !canAccessEeRange(rdram, dstAddr, size) ||
            !runtime->readIopMemory(srcAddr, payload.data(), payload.size()) ||
            !writeEeRange(rdram, dstAddr, payload.data(), size))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            return -1;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::AfterCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        return 0;
    }

    // int sceSifGetOtherData(sceSifReceiveData *rd, void *src, void *dest, int size, unsigned int mode)
    PS2_STUB_TYPED(int32_t, sceSifGetOtherData, ps2_abi::gptr rd, ps2_abi::gptr src, ps2_abi::gptr dest, int32_t size,
                   uint32_t mode)
    {
        (void)ctx;
        (void)mode;
        return sifGetOtherDataBody(rdram, runtime, rd, src, dest, size);
    }

    void sceSifGetOtherData_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        setReturnS32(ctx, sifGetOtherDataBody(rdram, runtime, rdAddr, srcAddr, dstAddr, sizeSigned));
    }

    PS2_STUB_DISPATCH(sceSifGetOtherData)

    // The body of sceSifGetReg after decoding (shared by both forms); returns v0.
    static uint32_t sifGetRegBody(R5900Context *ctx, uint32_t reg)
    {
        uint32_t value = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        return value;
    }

    // unsigned int sceSifGetReg(unsigned int reg)
    PS2_STUB_TYPED(uint32_t, sceSifGetReg, uint32_t reg)
    {
        (void)rdram;
        (void)runtime;
        return sifGetRegBody(ctx, reg);
    }

    void sceSifGetReg_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        setReturnU32(ctx, sifGetRegBody(ctx, reg));
    }

    PS2_STUB_DISPATCH(sceSifGetReg)

    // unsigned int sceSifGetSreg(int reg)
    PS2_STUB_TYPED(uint32_t, sceSifGetSreg, int32_t reg)
    {
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(static_cast<uint32_t>(reg));
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        return value;
    }

    void sceSifGetSreg_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    PS2_STUB_DISPATCH(sceSifGetSreg)

    // void sceSifInitCmd(void)
    PS2_STUB0_TYPED(int32_t, sceSifInitCmd)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        return 0;
    }

    void sceSifInitCmd_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifInitCmd)
    PS2_STUB_DEVIATION(sceSifInitCmd, "SDK: void sceSifInitCmd(void); the handler writes 0 to v0 (kept)")

    // int sceSifInitIopHeap(void)
    PS2_STUB0_TYPED(int32_t, sceSifInitIopHeap)
    {
        // The physical IOP allocator is initialized by IopSubsystem::reset().
        return 0;
    }

    void sceSifInitIopHeap_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The physical IOP allocator is initialized by IopSubsystem::reset().
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifInitIopHeap)

    PS2_STUB_ALIAS(sceSifInitRpc, ps2_syscalls::SifInitRpc, ::ps2_abi::Kind::Syscall)

    // int sceSifIsAliveIop(void)
    PS2_STUB0_TYPED(int32_t, sceSifIsAliveIop)
    {
        return 1;
    }

    void sceSifIsAliveIop_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceSifIsAliveIop)

    PS2_STUB_ALIAS(sceSifLoadElf, ps2_syscalls::sceSifLoadElf, ::ps2_abi::Kind::Syscall)

    PS2_STUB_ALIAS(sceSifLoadElfPart, ps2_syscalls::sceSifLoadElfPart, ::ps2_abi::Kind::Syscall)

    // void sceSifLoadFileReset(void)
    PS2_STUB0_TYPED(int32_t, sceSifLoadFileReset)
    {
        return 0;
    }

    void sceSifLoadFileReset_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifLoadFileReset)
    PS2_STUB_DEVIATION(sceSifLoadFileReset, "SDK: void sceSifLoadFileReset(void); the handler writes 0 to v0 (kept)")

    // int sceSifLoadIopHeap(const char *fname, void *addr)
    PS2_STUB_TYPED(int32_t, sceSifLoadIopHeap, ps2_abi::gptr fname, ps2_abi::gptr addr)
    {
        (void)fname;
        (void)addr;
        return 0;
    }

    void sceSifLoadIopHeap_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifLoadIopHeap)

    PS2_STUB_ALIAS(sceSifLoadModuleBuffer, ps2_syscalls::sceSifLoadModuleBuffer, ::ps2_abi::Kind::Syscall)

    // int sceSifRebootIop(const char *img)
    PS2_STUB_TYPED(int32_t, sceSifRebootIop, ps2_abi::gptr img)
    {
        (void)img;
        PS2IopTransport::reset(runtime);
        return 1;
    }

    void sceSifRebootIop_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceSifRebootIop)

    PS2_STUB_ALIAS(sceSifRegisterRpc, ps2_syscalls::SifRegisterRpc, ::ps2_abi::Kind::Syscall)

    // void sceSifRemoveCmdHandler(unsigned int cid)
    PS2_STUB_TYPED(int32_t, sceSifRemoveCmdHandler, uint32_t cid)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        return 0;
    }

    void sceSifRemoveCmdHandler_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifRemoveCmdHandler)
    PS2_STUB_DEVIATION(sceSifRemoveCmdHandler, "SDK: void sceSifRemoveCmdHandler(unsigned int cid); the handler writes 0 to v0 (kept)")

    PS2_STUB_ALIAS(sceSifRemoveRpc, ps2_syscalls::SifRemoveRpc, ::ps2_abi::Kind::Syscall)

    PS2_STUB_ALIAS(sceSifRemoveRpcQueue, ps2_syscalls::SifRemoveRpcQueue, ::ps2_abi::Kind::Syscall)

    // int sceSifResetIop(const char *arg, int mode): the game's copy (bound at 0x124748) reads a0
    // and a1; the handler resets the IOP whatever they say.
    PS2_STUB_TYPED(int32_t, sceSifResetIop, ps2_abi::gptr arg, int32_t mode)
    {
        (void)arg;
        (void)mode;
        PS2IopTransport::reset(runtime);
        return 1;
    }

    void sceSifResetIop_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceSifResetIop)

    // void sceSifRpcLoop(sceSifQueueData *qd)
    PS2_STUB_TYPED(int32_t, sceSifRpcLoop, ps2_abi::gptr qd)
    {
        (void)qd;
        return 0;
    }

    void sceSifRpcLoop_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifRpcLoop)
    PS2_STUB_DEVIATION(sceSifRpcLoop, "SDK: void sceSifRpcLoop(sceSifQueueData *qd); the handler writes 0 to v0 (kept)")

    // sceSifCmdData *sceSifSetCmdBuffer(sceSifCmdData *db, int size): returns the previous buffer.
    PS2_STUB_TYPED(ps2_abi::gptr, sceSifSetCmdBuffer, ps2_abi::gptr newBuffer, int32_t size)
    {
        (void)size;
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        return prev;
    }

    void sceSifSetCmdBuffer_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    PS2_STUB_DISPATCH(sceSifSetCmdBuffer)

    PS2_STUB_ALIAS(isceSifSetDChain, sceSifSetDChain, ::ps2_abi::Kind::Stub)

    PS2_STUB_ALIAS(isceSifSetDma, sceSifSetDma, ::ps2_abi::Kind::Stub)

    // void sceSifSetDChain(void)
    PS2_STUB0_TYPED(int32_t, sceSifSetDChain)
    {
        (void)rdram;
        (void)runtime;
        return 0;
    }

    void sceSifSetDChain_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifSetDChain)
    PS2_STUB_DEVIATION(sceSifSetDChain, "SDK: void sceSifSetDChain(void); the handler writes 0 to v0 (kept)")

    // The body of sceSifSetDma after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sifSetDmaBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t dmatAddr, uint32_t count)
    {
        PS2_IF_AGRESSIVE_LOGS({
            // Capped like the other per-call logs (a streaming movie calls this ~35 times
            // a second): 32 calls by default, PS2X_SIF_LOG_LIMIT=N raises the cap to N.
            static const uint32_t s_setDmaLogLimit = []() -> uint32_t
            {
                const char *value = std::getenv("PS2X_SIF_LOG_LIMIT");
                const unsigned long parsed = (value && *value) ? std::strtoul(value, nullptr, 10) : 0ul;
                return parsed == 0ul ? 32u : (parsed > 0xFFFFFFFul ? 0xFFFFFFFu : static_cast<uint32_t>(parsed));
            }();
            static std::atomic<uint32_t> s_setDmaLogCount{0u};
            const uint32_t logIndex = s_setDmaLogCount.fetch_add(1u, std::memory_order_relaxed);
            if (logIndex == s_setDmaLogLimit)
            {
                std::cerr << "[sceSifSetDma:CALL] log cap of " << s_setDmaLogLimit
                          << " calls reached; PS2X_SIF_LOG_LIMIT=N raises it" << std::endl;
            }
            if (logIndex < s_setDmaLogLimit)
            {
                std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << " list=0x" << dmatAddr
                          << " count=" << std::dec << count
                          << std::endl;

                // At most the 32 descriptors the call accepts: a bogus count (it is rejected
                // below) once logged millions of lines here.
                for (uint32_t i = 0; i < count && i < 32u; ++i)
                {
                    const uint32_t desc = dmatAddr + i * 16;
                    const uint32_t src = READ32(desc + 0);
                    const uint32_t dst = READ32(desc + 4);
                    const uint32_t size = READ32(desc + 8);
                    const uint32_t attr = READ32(desc + 12);

                    std::cerr << "[sceSifSetDma:DESC] i=" << i
                              << " src=0x" << std::hex << src
                              << " dst=0x" << dst
                              << " size=0x" << size
                              << " attr=0x" << attr
                              << " pc=0x" << ctx->pc
                              << " ra=0x" << getRegU32(ctx, 31)
                              << std::dec << std::endl;
                }
            }
        });

        if (!dmatAddr || count == 0u || count > 32u)
        {
            return 0;
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                break;
            }
            if (!runtime || !canAccessEeRange(rdram, xfer.src, sizeBytes) || !runtime->isIopMemoryRange(xfer.dest, sizeBytes))
            {
                ok = false;
                break;
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
                const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
                std::vector<uint8_t> payload(sizeBytes);
                if (!readEeRange(rdram, xfer.src, payload.data(), sizeBytes) || !runtime->writeIopMemory(xfer.dest, payload.data(), payload.size()))
                {
                    ok = false;
                    break;
                }
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::AfterCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
            }
        }

        if (!ok)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifSetDma failed dmat=0x" << std::hex << dmatAddr
                              << " count=0x" << count
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            return 0;
        }

        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);

        return static_cast<int32_t>(allocateSifDmaTransferId());
    }

    // unsigned int sceSifSetDma(sceSifDmaData *sdd, int len)
    PS2_STUB_TYPED(uint32_t, sceSifSetDma, ps2_abi::gptr sdd, int32_t len)
    {
        return static_cast<uint32_t>(sifSetDmaBody(rdram, ctx, runtime, sdd, static_cast<uint32_t>(len)));
    }

    void sceSifSetDma_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        const uint32_t listAddr = getRegU32(ctx, 4);
        (void)listAddr;
        setReturnS32(ctx, sifSetDmaBody(rdram, ctx, runtime, dmatAddr, count));
    }

    PS2_STUB_DISPATCH(sceSifSetDma)

    // sceSifSetIopAddr(addr, value): returns the second argument.
    PS2_STUB_TYPED(uint32_t, sceSifSetIopAddr, uint32_t addr, uint32_t value)
    {
        (void)addr;
        return value;
    }

    void sceSifSetIopAddr_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    PS2_STUB_DISPATCH(sceSifSetIopAddr)

    // The body of sceSifSetReg after decoding (shared by both forms); returns v0 (the previous value).
    static uint32_t sifSetRegBody(R5900Context *ctx, uint32_t reg, uint32_t value)
    {
        uint32_t prev = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        return prev;
    }

    // unsigned int sceSifSetReg(unsigned int reg, unsigned int value)
    PS2_STUB_TYPED(uint32_t, sceSifSetReg, uint32_t reg, uint32_t value)
    {
        (void)rdram;
        (void)runtime;
        return sifSetRegBody(ctx, reg, value);
    }

    void sceSifSetReg_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        setReturnU32(ctx, sifSetRegBody(ctx, reg, value));
    }

    PS2_STUB_DISPATCH(sceSifSetReg)

    PS2_STUB_ALIAS(sceSifSetRpcQueue, ps2_syscalls::SifSetRpcQueue, ::ps2_abi::Kind::Syscall)

    // unsigned int sceSifSetSreg(int reg, unsigned int value): returns the previous value.
    PS2_STUB_TYPED(uint32_t, sceSifSetSreg, int32_t reg, uint32_t value)
    {
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(static_cast<uint32_t>(reg));
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[static_cast<uint32_t>(reg)] = value;
        }
        return prev;
    }

    void sceSifSetSreg_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    PS2_STUB_DISPATCH(sceSifSetSreg)

    // sceSifCmdData *sceSifSetSysCmdBuffer(sceSifCmdData *db, int size): returns the previous buffer.
    PS2_STUB_TYPED(ps2_abi::gptr, sceSifSetSysCmdBuffer, ps2_abi::gptr newBuffer, int32_t size)
    {
        (void)size;
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        return prev;
    }

    void sceSifSetSysCmdBuffer_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    PS2_STUB_DISPATCH(sceSifSetSysCmdBuffer)

    // void sceSifStopDma(void)
    PS2_STUB0_TYPED(int32_t, sceSifStopDma)
    {
        return 0;
    }

    void sceSifStopDma_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifStopDma)
    PS2_STUB_DEVIATION(sceSifStopDma, "SDK: void sceSifStopDma(void); the handler writes 0 to v0 (kept)")

    // int sceSifSyncIop(void)
    PS2_STUB0_TYPED(int32_t, sceSifSyncIop)
    {
        return 1;
    }

    void sceSifSyncIop_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceSifSyncIop)

    // void sceSifWriteBackDCache(void *ptr, int size)
    PS2_STUB_TYPED(int32_t, sceSifWriteBackDCache, ps2_abi::gptr ptr, int32_t size)
    {
        (void)ptr;
        (void)size;
        return 0;
    }

    void sceSifWriteBackDCache_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSifWriteBackDCache)
    PS2_STUB_DEVIATION(sceSifWriteBackDCache, "SDK: void sceSifWriteBackDCache(void *ptr, int size); the handler writes 0 to v0 (kept)")
}

#include "ps2_snapshot_hooks.h"

namespace ps2_stubs::snapshot
{
    void saveSif(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            w.u32(g_nextSifDmaTransferId);
        }
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        w.podMap(g_sifRegs);
        w.podMap(g_sifSregs);
        w.podMap(g_sifCmdHandlers);
        w.u32(g_sifCmdBuffer);
        w.u32(g_sifSysCmdBuffer);
        w.boolean(g_sifCmdInitialized);
    }

    void loadSif(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        (void)runtime;
        snapshotLoadSupport(r);
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            g_nextSifDmaTransferId = r.u32();
        }
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        r.podMap(g_sifRegs);
        r.podMap(g_sifSregs);
        r.podMap(g_sifCmdHandlers);
        g_sifCmdBuffer = r.u32();
        g_sifSysCmdBuffer = r.u32();
        g_sifCmdInitialized = r.boolean();
    }
}
