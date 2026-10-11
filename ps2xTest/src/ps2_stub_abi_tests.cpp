// Typed stub ABI layer tests (ps2xRuntime/include/ps2_stub_abi.h).
//
//  - decoder: hand-built register files and stacks through small typed handlers declared here
//    (integer arguments 1-8 in a0-t3, 9+ in 8-byte stack slots from sp+0; floats in f12-f19,
//    the ninth on the stack; narrowing; returns in v0 / v0+v1 / f0; varargs slots);
//  - dispatcher: PS2_STUB_DISPATCH reaches the typed or the legacy form as the switch says;
//  - registry: every converted handler is registered, aliases resolve, the JSON lists them;
//  - differential: every handler with a typed and a legacy form (the transition pairs) is run
//    on random register / stack states, legacy twice (a nondeterminism guard) then typed, and
//    everything observable is compared: the register file, guest RAM, scratchpad, IOP RAM, GS
//    VRAM, VU memories (restored after each call), the DMA/GIF/GS/VIF counters, whether it
//    threw or crashed. Any difference fails the test.
//
// Environment: PS2X_ABI_DIFF_ITERS (default 64; 256 for a release check) states per handler; PS2X_ABI_DIFF_ONLY=<name
// part,...> limits the differential test; PS2X_ABI_DIFF_VERBOSE=1 prints every handler's line
// and the first differing state in full.

#include "MiniTest.h"
#include "poison_guard.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stub_abi.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_snapshot_hooks.h"
#include "Stubs/Audio.h"
#include "Stubs/MPEG.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

// ---- test-only typed handlers (registered as stubs named abiSelfTest*) ----------------------
namespace ps2_abi_selftest
{
    using ps2_abi::gptr;

    // Ten integer arguments: the ninth and tenth come from sp+0 and sp+8.
    PS2_STUB(int64_t, abiSelfTestTen, int32_t a1, uint32_t a2, int64_t a3, uint64_t a4, gptr a5, int32_t a6,
             int32_t a7, int32_t a8, int32_t a9, uint64_t a10)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        // A weighted sum that tells every argument apart.
        return static_cast<int64_t>(a1) * 1 + static_cast<int64_t>(a2) * 2 + a3 * 3 + static_cast<int64_t>(a4) * 4 +
               static_cast<int64_t>(static_cast<uint32_t>(a5)) * 5 + static_cast<int64_t>(a6) * 6 +
               static_cast<int64_t>(a7) * 7 + static_cast<int64_t>(a8) * 8 + static_cast<int64_t>(a9) * 9 +
               static_cast<int64_t>(a10) * 10;
    }

    // Narrowing: short / unsigned short / char take the low bits of their slot.
    PS2_STUB(int32_t, abiSelfTestNarrow, int16_t s, uint16_t us, int8_t c, uint8_t uc)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        // Stored into guest memory so each value is checked on its own.
        int32_t *out = reinterpret_cast<int32_t *>(rdram + 0x00100000u);
        out[0] = s;
        out[1] = us;
        out[2] = c;
        out[3] = uc;
        return s; // sign-extended into v0
    }

    // Floats: f12-f19, the ninth on the stack at sp+0; mixed with an integer (a0).
    PS2_STUB(float, abiSelfTestFloats, gptr m, float f1, float f2, float f3, float f4, float f5, float f6, float f7,
             float f8, float f9)
    {
        (void)ctx;
        (void)runtime;
        float *out = reinterpret_cast<float *>(rdram + (static_cast<uint32_t>(m) & 0x01FFFFFFu));
        const float all[9] = {f1, f2, f3, f4, f5, f6, f7, f8, f9};
        std::memcpy(out, all, sizeof(all));
        return f9 * 2.0f;
    }

    // Varargs after two fixed arguments: the first vararg is slot 2 (a2).
    PS2_STUB(uint32_t, abiSelfTestVarArgs, gptr dst, uint32_t count, ps2_abi::VarArgs args)
    {
        (void)ctx;
        (void)runtime;
        uint64_t *out = reinterpret_cast<uint64_t *>(rdram + (static_cast<uint32_t>(dst) & 0x01FFFFFFu));
        for (uint32_t i = 0; i < count && i < 12u; ++i)
            out[i] = args.nextU64();
        return args.firstSlot();
    }

    PS2_STUB0(void, abiSelfTestVoid)
    {
        (void)rdram;
        (void)runtime;
        SET_GPR_U32(ctx, 8, 0x5A5A5A5Au); // proves it ran; v0 must stay untouched
    }

    // A transition pair whose two forms agree.
    std::atomic<int> g_typedCalls{0};
    std::atomic<int> g_legacyCalls{0};

    PS2_STUB_TYPED(int32_t, abiSelfTestPair, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e)
    {
        (void)c;
        (void)d;
        (void)rdram;
        (void)ctx;
        (void)runtime;
        g_typedCalls.fetch_add(1);
        return a - b + e;
    }

    void abiSelfTestPair_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        g_legacyCalls.fetch_add(1);
        setReturnS32(ctx, static_cast<int32_t>(getRegU32(ctx, 4)) - static_cast<int32_t>(getRegU32(ctx, 5)) +
                              static_cast<int32_t>(getRegU32(ctx, 8)));
    }

    PS2_STUB_DISPATCH(abiSelfTestPair)

    // A transition pair whose legacy form reads argument 5 from the O32 home slot sp+0x10 (the
    // Walls 7/8 bug): the differential test must flag it.
    PS2_STUB_TYPED(int32_t, abiSelfTestBadPair, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        return a + b + c + d + e;
    }

    void abiSelfTestBadPair_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t sp = getRegU32(ctx, 29);
        uint32_t e = 0;
        std::memcpy(&e, rdram + ((sp + 0x10u) & 0x01FFFFFFu), sizeof(e));
        setReturnS32(ctx, static_cast<int32_t>(getRegU32(ctx, 4) + getRegU32(ctx, 5) + getRegU32(ctx, 6) +
                                               getRegU32(ctx, 7) + e));
    }

    PS2_STUB_DISPATCH(abiSelfTestBadPair)

    PS2_STUB_ALIAS(abiSelfTestAlias, abiSelfTestTen, ::ps2_abi::Kind::Stub)

    // A pair that differs only in library state (a SIF software register set from a0 or a1, v0 the
    // same): the differential must see it through the libraries' saved state.
    void setSregForSelfTest(uint8_t *rdram, PS2Runtime *runtime, uint32_t value)
    {
        R5900Context c{};
        SET_GPR_U32(&c, 4, 0x77u);
        SET_GPR_U32(&c, 5, value);
        ps2_stubs::sceSifSetSreg(rdram, &c, runtime);
    }

    PS2_STUB_TYPED(int32_t, abiSelfTestLibPair, uint32_t a, uint32_t b)
    {
        (void)ctx;
        (void)b;
        setSregForSelfTest(rdram, runtime, a);
        return 0;
    }

    void abiSelfTestLibPair_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setSregForSelfTest(rdram, runtime, getRegU32(ctx, 5));
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(abiSelfTestLibPair)

    // A pair that differs only in a memory-card file's content (a0 or a1 written): the differential
    // must see it through the memory-card tree (names starting abiSelfTestMc get the file reset).
    void writeMcFileForSelfTest(uint32_t value)
    {
        const std::filesystem::path p = PS2Runtime::getIoPaths().mcRoot / "selftest.bin";
        if (std::FILE *f = std::fopen(p.string().c_str(), "wb"))
        {
            std::fwrite(&value, sizeof(value), 1, f);
            std::fclose(f);
        }
    }

    PS2_STUB_TYPED(int32_t, abiSelfTestMcFilePair, uint32_t a, uint32_t b)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        (void)b;
        writeMcFileForSelfTest(a);
        return 0;
    }

    void abiSelfTestMcFilePair_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        writeMcFileForSelfTest(getRegU32(ctx, 5));
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(abiSelfTestMcFilePair)
}

namespace
{
    // The process id, for scratch folders that must not be shared between concurrent test runs.
    unsigned long currentProcessId()
    {
#ifdef _WIN32
        return static_cast<unsigned long>(_getpid());
#else
        return static_cast<unsigned long>(getpid());
#endif
    }
}

namespace
{
    using Handler = ps2_abi::Handler;

    constexpr uint32_t kBufBase = 0x00200000u; // pointer targets, 8 KB each
    constexpr uint32_t kBufStride = 0x2000u;
    constexpr uint32_t kBufCount = 16u;
    constexpr uint32_t kHeapBase = 0x00800000u;
    constexpr uint32_t kHeapLimit = 0x01800000u;
    constexpr uint32_t kStackArea = 0x01E00000u;
    constexpr uint32_t kStackWindow = 0x40u; // sp+0x00..0x3f randomised

    // The handlers converted so far (the pilot set, then one block per library batch): the registry
    // must hold each of them with both forms, or as an alias.
    struct Expected
    {
        const char *name;
        ps2_abi::Kind kind;
        bool alias;
    };
    const Expected kConverted[] = {
        {"sceSifSendCmd", ps2_abi::Kind::Syscall, false},
        {"SifCallRpc", ps2_abi::Kind::Syscall, false},
        {"SifRegisterRpc", ps2_abi::Kind::Syscall, false},
        {"sceSifCallRpc", ps2_abi::Kind::Syscall, true},
        {"sceSifSendCmd", ps2_abi::Kind::Stub, false},
        {"sceSifRegisterRpc", ps2_abi::Kind::Stub, true},
        {"sceGsSetDefDBuff", ps2_abi::Kind::Stub, false},
        {"sceGsSetDefDBuffDc", ps2_abi::Kind::Stub, false},
        {"sceGsSetDefDispEnv", ps2_abi::Kind::Stub, false},
        {"sceGsSetDefDrawEnv", ps2_abi::Kind::Stub, false},
        {"sceGsSetDefDrawEnv2", ps2_abi::Kind::Stub, false},
        {"sceGsSetDefClear", ps2_abi::Kind::Stub, false},
        {"sceGsSetDefLoadImage", ps2_abi::Kind::Stub, false},
        {"sceGsSetDefStoreImage", ps2_abi::Kind::Stub, true},
        {"sceMpegAddStrCallback", ps2_abi::Kind::Stub, false},
        {"sceMpegDemuxPssRing", ps2_abi::Kind::Stub, false},
        {"sceSdRemote", ps2_abi::Kind::Stub, false},
        {"printf", ps2_abi::Kind::Stub, false},
        {"sprintf", ps2_abi::Kind::Stub, false},
        {"snprintf", ps2_abi::Kind::Stub, false},
        {"fprintf", ps2_abi::Kind::Stub, false},
        {"vfprintf", ps2_abi::Kind::Stub, false},
        {"vsprintf", ps2_abi::Kind::Stub, false},
        {"printf_r", ps2_abi::Kind::Stub, false},
        {"scePrintf", ps2_abi::Kind::Stub, false},
        // Batch: SIF / RPC (Stubs/SIF.cpp, Syscalls/RPC.cpp, Syscalls/System.cpp)
        {"SifStopModule", ps2_abi::Kind::Syscall, false},
        {"SifLoadModule", ps2_abi::Kind::Syscall, false},
        {"SifInitRpc", ps2_abi::Kind::Syscall, false},
        {"SifBindRpc", ps2_abi::Kind::Syscall, false},
        {"SifCheckStatRpc", ps2_abi::Kind::Syscall, false},
        {"SifSetRpcQueue", ps2_abi::Kind::Syscall, false},
        {"SifRemoveRpcQueue", ps2_abi::Kind::Syscall, false},
        {"SifRemoveRpc", ps2_abi::Kind::Syscall, false},
        {"sceRpcGetPacket", ps2_abi::Kind::Syscall, false},
        {"SifLoadElfPart", ps2_abi::Kind::Syscall, false},
        {"sceSifLoadElf", ps2_abi::Kind::Syscall, false},
        {"sceSifLoadModuleBuffer", ps2_abi::Kind::Syscall, false},
        {"sceSifLoadElfPart", ps2_abi::Kind::Syscall, true},
        {"sceSifLoadModule", ps2_abi::Kind::Syscall, true},
        {"sceSifAddCmdHandler", ps2_abi::Kind::Stub, false},
        {"sceSifAllocIopHeap", ps2_abi::Kind::Stub, false},
        {"sceSifAllocSysMemory", ps2_abi::Kind::Stub, false},
        {"sceSifDmaStat", ps2_abi::Kind::Stub, false},
        {"sceSifExecRequest", ps2_abi::Kind::Stub, false},
        {"sceSifExitCmd", ps2_abi::Kind::Stub, false},
        {"sceSifExitRpc", ps2_abi::Kind::Stub, false},
        {"sceSifFreeIopHeap", ps2_abi::Kind::Stub, false},
        {"sceSifFreeSysMemory", ps2_abi::Kind::Stub, false},
        {"sceSifGetDataTable", ps2_abi::Kind::Stub, false},
        {"sceSifGetIopAddr", ps2_abi::Kind::Stub, false},
        {"sceSifGetNextRequest", ps2_abi::Kind::Stub, false},
        {"sceSifGetOtherData", ps2_abi::Kind::Stub, false},
        {"sceSifGetReg", ps2_abi::Kind::Stub, false},
        {"sceSifGetSreg", ps2_abi::Kind::Stub, false},
        {"sceSifInitCmd", ps2_abi::Kind::Stub, false},
        {"sceSifInitIopHeap", ps2_abi::Kind::Stub, false},
        {"sceSifIsAliveIop", ps2_abi::Kind::Stub, false},
        {"sceSifLoadFileReset", ps2_abi::Kind::Stub, false},
        {"sceSifLoadIopHeap", ps2_abi::Kind::Stub, false},
        {"sceSifRebootIop", ps2_abi::Kind::Stub, false},
        {"sceSifRemoveCmdHandler", ps2_abi::Kind::Stub, false},
        {"sceSifResetIop", ps2_abi::Kind::Stub, false},
        {"sceSifRpcLoop", ps2_abi::Kind::Stub, false},
        {"sceSifSetCmdBuffer", ps2_abi::Kind::Stub, false},
        {"sceSifSetDChain", ps2_abi::Kind::Stub, false},
        {"sceSifSetDma", ps2_abi::Kind::Stub, false},
        {"sceSifSetIopAddr", ps2_abi::Kind::Stub, false},
        {"sceSifSetReg", ps2_abi::Kind::Stub, false},
        {"sceSifSetSreg", ps2_abi::Kind::Stub, false},
        {"sceSifSetSysCmdBuffer", ps2_abi::Kind::Stub, false},
        {"sceSifStopDma", ps2_abi::Kind::Stub, false},
        {"sceSifSyncIop", ps2_abi::Kind::Stub, false},
        {"sceSifWriteBackDCache", ps2_abi::Kind::Stub, false},
        {"sceSifLoadModule", ps2_abi::Kind::Stub, true},
        {"sceSifBindRpc", ps2_abi::Kind::Stub, true},
        {"sceSifCheckStatRpc", ps2_abi::Kind::Stub, true},
        {"sceSifInitRpc", ps2_abi::Kind::Stub, true},
        {"sceSifLoadElf", ps2_abi::Kind::Stub, true},
        {"sceSifLoadElfPart", ps2_abi::Kind::Stub, true},
        {"sceSifLoadModuleBuffer", ps2_abi::Kind::Stub, true},
        {"sceSifRemoveRpc", ps2_abi::Kind::Stub, true},
        {"sceSifRemoveRpcQueue", ps2_abi::Kind::Stub, true},
        {"sceSifSetRpcQueue", ps2_abi::Kind::Stub, true},
        {"isceSifSetDChain", ps2_abi::Kind::Stub, true},
        {"isceSifSetDma", ps2_abi::Kind::Stub, true},
        // Batch: CDVD (Stubs/CD.cpp)
        {"sceCdRead", ps2_abi::Kind::Stub, false},
        {"sceCdSync", ps2_abi::Kind::Stub, false},
        {"sceCdGetError", ps2_abi::Kind::Stub, false},
        {"sceCdApplyNCmd", ps2_abi::Kind::Stub, false},
        {"sceCdBreak", ps2_abi::Kind::Stub, false},
        {"sceCdCallback", ps2_abi::Kind::Stub, false},
        {"sceCdChangeThreadPriority", ps2_abi::Kind::Stub, false},
        {"sceCdDelayThread", ps2_abi::Kind::Stub, false},
        {"sceCdDiskReady", ps2_abi::Kind::Stub, false},
        {"sceCdGetDiskType", ps2_abi::Kind::Stub, false},
        {"sceCdGetReadPos", ps2_abi::Kind::Stub, false},
        {"sceCdGetToc", ps2_abi::Kind::Stub, false},
        {"sceCdInit", ps2_abi::Kind::Stub, false},
        {"sceCdInitEeCB", ps2_abi::Kind::Stub, false},
        {"sceCdIntToPos", ps2_abi::Kind::Stub, false},
        {"sceCdMmode", ps2_abi::Kind::Stub, false},
        {"sceCdNcmdDiskReady", ps2_abi::Kind::Stub, false},
        {"sceCdPause", ps2_abi::Kind::Stub, false},
        {"sceCdPosToInt", ps2_abi::Kind::Stub, false},
        {"sceCdReadChain", ps2_abi::Kind::Stub, false},
        {"sceCdReadClock", ps2_abi::Kind::Stub, false},
        {"sceCdSearchFile", ps2_abi::Kind::Stub, false},
        {"sceCdSeek", ps2_abi::Kind::Stub, false},
        {"sceCdStandby", ps2_abi::Kind::Stub, false},
        {"sceCdStatus", ps2_abi::Kind::Stub, false},
        {"sceCdStInit", ps2_abi::Kind::Stub, false},
        {"sceCdStop", ps2_abi::Kind::Stub, false},
        {"sceCdStPause", ps2_abi::Kind::Stub, false},
        {"sceCdStRead", ps2_abi::Kind::Stub, false},
        {"sceCdStream", ps2_abi::Kind::Stub, false},
        {"sceCdStResume", ps2_abi::Kind::Stub, false},
        {"sceCdStSeek", ps2_abi::Kind::Stub, false},
        {"sceCdStSeekF", ps2_abi::Kind::Stub, false},
        {"sceCdStStart", ps2_abi::Kind::Stub, false},
        {"sceCdStStat", ps2_abi::Kind::Stub, false},
        {"sceCdStStop", ps2_abi::Kind::Stub, false},
        {"sceCdSyncS", ps2_abi::Kind::Stub, false},
        {"sceCdTrayReq", ps2_abi::Kind::Stub, false},
        {"sceCdReadIOPm", ps2_abi::Kind::Stub, true},
        // Batch: sound (Stubs/Audio.cpp)
        {"sceSdRemoteInit", ps2_abi::Kind::Stub, false},
        {"sceSSyn_SetOutputMode", ps2_abi::Kind::Stub, false},
        // Batch: pad (Stubs/Pad.cpp)
        {"scePadEnd", ps2_abi::Kind::Stub, false},
        {"scePadEnterPressMode", ps2_abi::Kind::Stub, false},
        {"scePadExitPressMode", ps2_abi::Kind::Stub, false},
        {"scePadGetButtonMask", ps2_abi::Kind::Stub, false},
        {"scePadGetDmaStr", ps2_abi::Kind::Stub, false},
        {"scePadGetFrameCount", ps2_abi::Kind::Stub, false},
        {"scePadGetModVersion", ps2_abi::Kind::Stub, false},
        {"scePadGetPortMax", ps2_abi::Kind::Stub, false},
        {"scePadGetReqState", ps2_abi::Kind::Stub, false},
        {"scePadGetSlotMax", ps2_abi::Kind::Stub, false},
        {"scePadGetState", ps2_abi::Kind::Stub, false},
        {"scePadInfoAct", ps2_abi::Kind::Stub, false},
        {"scePadInfoComb", ps2_abi::Kind::Stub, false},
        {"scePadInfoMode", ps2_abi::Kind::Stub, false},
        {"scePadInfoPressMode", ps2_abi::Kind::Stub, false},
        {"scePadInit", ps2_abi::Kind::Stub, false},
        {"scePadPortClose", ps2_abi::Kind::Stub, false},
        {"scePadPortOpen", ps2_abi::Kind::Stub, false},
        {"scePadRead", ps2_abi::Kind::Stub, false},
        {"scePadReqIntToStr", ps2_abi::Kind::Stub, false},
        {"scePadSetActAlign", ps2_abi::Kind::Stub, false},
        {"scePadSetActDirect", ps2_abi::Kind::Stub, false},
        {"scePadSetButtonInfo", ps2_abi::Kind::Stub, false},
        {"scePadSetMainMode", ps2_abi::Kind::Stub, false},
        {"scePadSetReqState", ps2_abi::Kind::Stub, false},
        {"scePadSetVrefParam", ps2_abi::Kind::Stub, false},
        {"scePadSetWarningLevel", ps2_abi::Kind::Stub, false},
        {"scePadStateIntToStr", ps2_abi::Kind::Stub, false},
        {"scePadInit2", ps2_abi::Kind::Stub, true},
        // Batch: memory card (Stubs/MemoryCard.cpp)
        {"sceMcChangeThreadPriority", ps2_abi::Kind::Stub, false},
        {"sceMcChdir", ps2_abi::Kind::Stub, false},
        {"sceMcClose", ps2_abi::Kind::Stub, false},
        {"sceMcDelete", ps2_abi::Kind::Stub, false},
        {"sceMcEnd", ps2_abi::Kind::Stub, false},
        {"sceMcFlush", ps2_abi::Kind::Stub, false},
        {"sceMcFormat", ps2_abi::Kind::Stub, false},
        {"sceMcGetDir", ps2_abi::Kind::Stub, false},
        {"sceMcGetEntSpace", ps2_abi::Kind::Stub, false},
        {"sceMcGetInfo", ps2_abi::Kind::Stub, false},
        {"sceMcGetSlotMax", ps2_abi::Kind::Stub, false},
        {"sceMcInit", ps2_abi::Kind::Stub, false},
        {"sceMcMkdir", ps2_abi::Kind::Stub, false},
        {"sceMcOpen", ps2_abi::Kind::Stub, false},
        {"sceMcRead", ps2_abi::Kind::Stub, false},
        {"sceMcReadFast", ps2_abi::Kind::Stub, false},
        {"sceMcRename", ps2_abi::Kind::Stub, false},
        {"sceMcSeek", ps2_abi::Kind::Stub, false},
        {"sceMcSetFileInfo", ps2_abi::Kind::Stub, false},
        {"sceMcSync", ps2_abi::Kind::Stub, false},
        {"sceMcUnformat", ps2_abi::Kind::Stub, false},
        {"sceMcWrite", ps2_abi::Kind::Stub, false},
        {"sceMcWriteFast", ps2_abi::Kind::Stub, false},
    };

    // Handlers whose result changes on every call by design, so no two runs ever agree and the
    // differential cannot compare them (their arguments are not used by the body either):
    // scePadGetFrameCount returns a free-running counter.
    bool alwaysNondeterministic(const std::string &label)
    {
        return label == "stub scePadGetFrameCount";
    }

    const char *kindName(ps2_abi::Kind k) { return k == ps2_abi::Kind::Stub ? "stub" : "syscall"; }

    bool isSelfTest(const char *name) { return std::strncmp(name, "abiSelfTest", 11) == 0; }

    uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ull)
    {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    void setGpr64(R5900Context &ctx, int reg, uint64_t lo, uint64_t hi = 0)
    {
        ctx.r[reg] = _mm_set_epi64x(static_cast<int64_t>(hi), static_cast<int64_t>(lo));
    }

    uint64_t gpr64(const R5900Context &ctx, int reg)
    {
        uint64_t v = 0;
        std::memcpy(&v, &ctx.r[reg], sizeof(v));
        return v;
    }

    // ---- the differential harness ---------------------------------------------------------
    struct Region
    {
        const char *name;
        uint8_t *mem;
        size_t size;
        std::vector<uint8_t> base;
    };

    struct PageDiff
    {
        uint8_t region;
        uint32_t page;
        uint64_t hash;
        bool operator==(const PageDiff &o) const { return region == o.region && page == o.page && hash == o.hash; }
    };

    struct Observation
    {
        uint32_t crashCode = 0;
        bool threw = false;
        std::string what;
        std::vector<uint8_t> ctx;
        std::vector<PageDiff> pages;
        uint64_t counters[4]{};
        uint64_t v0 = 0, v1 = 0;
        float f0 = 0.0f;
        uint64_t libHash = 0; // the stub libraries' saved state after the call (snapshot hooks)
        uint64_t fsHash = 0;  // the memory-card fixture tree after the call (sceMc* handlers)

        bool operator==(const Observation &o) const
        {
            return crashCode == o.crashCode && threw == o.threw && what == o.what && ctx == o.ctx &&
                   pages == o.pages && std::equal(std::begin(counters), std::end(counters), std::begin(o.counters)) &&
                   libHash == o.libHash && fsHash == o.fsHash;
        }
    };

    struct State
    {
        uint64_t lo[8];   // a0-t3, low 64 bits
        uint64_t hi[8];   // upper 64 bits of the 128-bit registers
        float f[8];       // f12-f19
        uint32_t stack[kStackWindow / 4];
        uint64_t v0, v1;
    };

    std::string describeState(const State &s)
    {
        std::ostringstream os;
        os << std::hex;
        const char *names[8] = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"};
        for (int i = 0; i < 8; ++i)
            os << " " << names[i] << "=0x" << s.lo[i];
        os << " sp+0..:";
        for (uint32_t i = 0; i < 8; ++i)
            os << " 0x" << s.stack[i];
        return os.str();
    }

    std::string describeDiff(const Observation &a, const Observation &b)
    {
        std::ostringstream os;
        os << std::hex;
        if (a.crashCode != b.crashCode)
            os << " crash 0x" << a.crashCode << "/0x" << b.crashCode;
        if (a.threw != b.threw || a.what != b.what)
            os << " threw " << a.threw << "(" << a.what.substr(0, 40) << ")/" << b.threw << "(" << b.what.substr(0, 40) << ")";
        if (a.v0 != b.v0)
            os << " v0 0x" << a.v0 << "/0x" << b.v0;
        if (a.v1 != b.v1)
            os << " v1 0x" << a.v1 << "/0x" << b.v1;
        if (a.ctx != b.ctx && a.v0 == b.v0 && a.v1 == b.v1)
        {
            size_t i = 0;
            while (i < a.ctx.size() && i < b.ctx.size() && a.ctx[i] == b.ctx[i])
                ++i;
            os << " ctx@+0x" << i;
        }
        if (!(a.pages == b.pages))
            os << " pages(" << std::dec << a.pages.size() << "/" << b.pages.size() << std::hex << ")";
        for (int i = 0; i < 4; ++i)
            if (a.counters[i] != b.counters[i])
                os << " counter" << i;
        if (a.libHash != b.libHash)
            os << " library-state";
        if (a.fsHash != b.fsHash)
            os << " mc-files";
        return os.str();
    }

    struct Harness
    {
        PS2Runtime *runtime = nullptr; // leaked on purpose: handlers keep pointers to it
        uint8_t *rdram = nullptr;
        uint32_t sp = 0;
        std::vector<Region> regions;
        std::atomic<const char *> current{nullptr};
        std::atomic<int64_t> callStartMs{0};
        std::string currentState; // written before callStartMs is set, read only after a timeout

        // ---- library fixture (prepareLibraryFixture) ----------------------------------------
        // Every call starts from the same saved state of the stateful stub libraries and the
        // syscall tables (their snapshot hooks), and every sceMc* call from the same memory-card
        // files, so a handler with library state (RPC ids, DMA transfer ids, pad ports, open
        // memory-card files) gives the same result on every run; the state each call leaves is
        // compared too (Observation::libHash / fsHash).
        struct LibHook
        {
            const char *name;
            void (*save)(ps2x::StateWriter &);
            void (*load)(ps2x::StateReader &, PS2Runtime &);
            std::vector<uint8_t> pre;  // before the fixture (restored when the test ends)
            std::vector<uint8_t> base; // the fixture every call starts from
        };
        std::vector<LibHook> libs;
        bool libsReady = false;
        std::filesystem::path scratch;
        std::vector<uint8_t> mcEmpty; // MemoryCard state with no file open (the file reset needs them closed)
        static constexpr uint32_t kFixturePadDma = 0x00240000u;  // outside the random buffers
        static constexpr uint32_t kFixtureMcPath = 0x00240100u;

        static std::vector<uint8_t> saveHook(void (*save)(ps2x::StateWriter &))
        {
            ps2x::StateWriter w;
            save(w);
            return w.data();
        }

        void loadHook(const LibHook &h, const std::vector<uint8_t> &blob)
        {
            ps2x::StateReader r(blob.data(), blob.size());
            h.load(r, *runtime);
        }

        LibHook *findLib(const char *name)
        {
            for (LibHook &h : libs)
                if (std::strcmp(h.name, name) == 0)
                    return &h;
            return nullptr;
        }

        static void writeFile(const std::filesystem::path &p, size_t size, uint8_t seed)
        {
            std::vector<char> bytes(size);
            for (size_t i = 0; i < size; ++i)
                bytes[i] = static_cast<char>(static_cast<uint8_t>(seed + i * 7u));
            std::FILE *f = std::fopen(p.string().c_str(), "wb");
            if (f)
            {
                std::fwrite(bytes.data(), 1, bytes.size(), f);
                std::fclose(f);
            }
        }

        // mc0 / mc1 (the runtime's memory-card roots under the scratch folder) from mcfix0 / mcfix1.
        // Leaves the card trees alone when they already match the fixture (fewer file operations for
        // another process to trip over), else copies them again and checks the result, retrying: a
        // remove/copy can fail now and then on Windows while a scanner holds a file.
        uint64_t mcFixtureHash = 0; // the card trees as the fixture made them (0 = not known yet)

        void copyMcFixture()
        {
            std::error_code ec;
            for (const char *slot : {"0", "1"})
            {
                const std::filesystem::path root = scratch / (std::string("mc") + slot);
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::copy(scratch / (std::string("mcfix") + slot), root,
                                      std::filesystem::copy_options::recursive, ec);
            }
        }

        void resetMcFiles()
        {
            if (mcFixtureHash == 0)
            {
                copyMcFixture();
                mcFixtureHash = mcFilesHash();
                return;
            }
            if (mcFilesHash() == mcFixtureHash)
                return;
            for (int attempt = 0; attempt < 200; ++attempt)
            {
                copyMcFixture();
                if (mcFilesHash() == mcFixtureHash)
                {
                    if (attempt > 0)
                    {
                        ++retriedRestores;
                        retriedLibs.insert("mc-files");
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            throw std::runtime_error("memory-card fixture files could not be restored");
        }

        uint64_t mcFilesHash() const
        {
            uint64_t h = 1469598103934665603ull;
            for (const char *slot : {"mc0", "mc1"})
            {
                const std::filesystem::path root = scratch / slot;
                std::vector<std::filesystem::path> entries;
                std::error_code ec;
                for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
                     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                    entries.push_back(it->path());
                std::sort(entries.begin(), entries.end());
                for (const auto &p : entries)
                {
                    const std::string rel = slot + std::string("/") + std::filesystem::relative(p, root, ec).generic_string();
                    h = fnv(reinterpret_cast<const uint8_t *>(rel.data()), rel.size(), h);
                    if (std::filesystem::is_directory(p, ec))
                    {
                        h = fnv(reinterpret_cast<const uint8_t *>("/"), 1, h);
                        continue;
                    }
                    std::ifstream in(p, std::ios::binary);
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    const uint64_t size = bytes.size();
                    h = fnv(reinterpret_cast<const uint8_t *>(&size), sizeof(size), h);
                    h = fnv(bytes.data(), bytes.size(), h);
                }
            }
            return h;
        }

        uint64_t libStateHash() const
        {
            uint64_t h = 1469598103934665603ull;
            for (const LibHook &lib : libs)
            {
                const std::vector<uint8_t> blob = saveHook(lib.save);
                h = fnv(blob.data(), blob.size(), h);
            }
            return h;
        }

        void restoreLibraries(bool mcFiles)
        {
            if (!libsReady)
                return;
            if (mcFiles)
            {
                if (LibHook *mc = findLib("mc"))
                    loadHook(*mc, mcEmpty); // closes every open memory-card file
                resetMcFiles();
            }
            for (const LibHook &lib : libs)
                restoreOne(lib);
        }

        // Puts one library back to its fixture state and checks that it got there. A library whose
        // saved state already equals the fixture is left alone: reloading the memory card reopens its
        // fixture file (fopen "r+b") on every call, and on Windows that open fails now and then while
        // another process (the virus scanner, the indexer) has the freshly written file open; the
        // card library then silently lacks fd 1 and that one run differs from the others ("typed and
        // legacy differ: ... library-state", seen for sceSifSetCmdBuffer, sceSifStopDma, ...: harness
        // flakes, not decoding). A reload that does not reach the fixture is retried.
        uint32_t retriedRestores = 0; // reloads that needed more than one attempt (reported)
        std::set<std::string> retriedLibs;

        void restoreOne(const LibHook &lib)
        {
            if (saveHook(lib.save) == lib.base)
                return;
            for (int attempt = 0; attempt < 200; ++attempt)
            {
                loadHook(lib, lib.base);
                if (saveHook(lib.save) == lib.base)
                {
                    if (attempt > 0)
                    {
                        ++retriedRestores;
                        retriedLibs.insert(lib.name);
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            throw std::runtime_error(std::string("library fixture could not be restored: ") + lib.name);
        }

        void fixtureCall(Handler fn, std::initializer_list<uint32_t> args)
        {
            R5900Context c{};
            int reg = 4;
            for (const uint32_t a : args)
            {
                SET_GPR_U32(&c, reg, a);
                ++reg;
            }
            SET_GPR_U32(&c, 29, sp);
            SET_GPR_U32(&c, 31, 0x00100008u);
            fn(rdram, &c, runtime);
        }

        // The fixture: files for the CD root and the memory cards under `dir` (the IoPaths the
        // test set), then the libraries initialised the way a game starts them (RPC, SIF
        // commands, CD, pad port 0 open, libsdr, memory card with one file open as fd 1).
        void prepareLibraryFixture(const std::filesystem::path &dir)
        {
            scratch = dir;
            libs = {
                {"sif", &ps2_stubs::snapshot::saveSif, &ps2_stubs::snapshot::loadSif, {}, {}},
                {"rpc", &ps2_stubs::snapshot::saveRpc, &ps2_stubs::snapshot::loadRpc, {}, {}},
                {"cd", &ps2_stubs::snapshot::saveCd, &ps2_stubs::snapshot::loadCd, {}, {}},
                {"pad", &ps2_stubs::snapshot::savePad, &ps2_stubs::snapshot::loadPad, {}, {}},
                {"mc", &ps2_stubs::snapshot::saveMemoryCard, &ps2_stubs::snapshot::loadMemoryCard, {}, {}},
                {"audio", &ps2_stubs::snapshot::saveAudio, &ps2_stubs::snapshot::loadAudio, {}, {}},
                {"syscalls", &ps2_syscalls::snapshot::save, &ps2_syscalls::snapshot::load, {}, {}},
            };
            for (LibHook &lib : libs)
                lib.pre = saveHook(lib.save);

            // Names the random states point at: the format strings in buffers 8-15 ("%d %d %d %d",
            // "%s %d"; the ones with '|' are not valid Windows names and stay missing).
            std::error_code ec;
            writeFile(dir / "%d %d %d %d", 4u * 2048u, 0x11);
            writeFile(dir / "%s %d", 2048u, 0x22);
            std::filesystem::create_directories(dir / "mcfix0" / "%s %d", ec);
            std::filesystem::create_directories(dir / "mcfix1", ec);
            writeFile(dir / "mcfix0" / "%d %d %d %d", 300u, 0x33);
            writeFile(dir / "mcfix0" / "%s %d" / "icon.sys", 964u, 0x44);
            writeFile(dir / "mcfix1" / "%d %d %d %d", 100u, 0x55);
            resetMcFiles();

            const char path[] = "/%d %d %d %d";
            std::memcpy(rdram + kFixtureMcPath, path, sizeof(path));
            fixtureCall(&ps2_syscalls::SifInitRpc, {0u});
            fixtureCall(&ps2_stubs::sceSifInitCmd, {});
            fixtureCall(&ps2_stubs::sceCdInit, {0u});
            fixtureCall(&ps2_stubs::scePadInit, {0u});
            fixtureCall(&ps2_stubs::scePadPortOpen, {0u, 0u, kFixturePadDma});
            fixtureCall(&ps2_stubs::sceSdRemoteInit, {});
            fixtureCall(&ps2_stubs::sceMcInit, {});
            mcEmpty = saveHook(&ps2_stubs::snapshot::saveMemoryCard);
            fixtureCall(&ps2_stubs::sceMcOpen, {0u, 0u, kFixtureMcPath, 2u /* O_RDWR */});

            for (LibHook &lib : libs)
                lib.base = saveHook(lib.save);
            for (Region &r : regions)
                r.base.assign(r.mem, r.mem + r.size);
            libsReady = true;
        }

        // The fixture really reaches the success paths: fd 1 reads the card file, pad port 0 reads,
        // sceCdSearchFile finds the CD-root file. Returns a description of the first failure.
        std::string checkLibraryFixture()
        {
            constexpr uint32_t kOut = 0x00250000u;
            auto call = [this](Handler fn, std::initializer_list<uint32_t> args) -> int32_t
            {
                R5900Context c{};
                int reg = 4;
                for (const uint32_t a : args)
                {
                    SET_GPR_U32(&c, reg, a);
                    ++reg;
                }
                SET_GPR_U32(&c, 29, sp);
                fn(rdram, &c, runtime);
                return static_cast<int32_t>(getRegU32(&c, 2));
            };
            std::string why;
            restoreLibraries(true);
            call(&ps2_stubs::sceMcRead, {1u, kOut, 16u});
            const int32_t synced = call(&ps2_stubs::sceMcSync, {0u, kOut + 0x100u, kOut + 0x104u});
            int32_t cmd = 0, result = 0;
            std::memcpy(&cmd, rdram + kOut + 0x100u, 4);
            std::memcpy(&result, rdram + kOut + 0x104u, 4);
            if (synced != 1 || result != 16 || rdram[kOut] != 0x33u || rdram[kOut + 1] != static_cast<uint8_t>(0x33u + 7u))
                why += " sceMcRead(fd 1): sync " + std::to_string(synced) + " result " + std::to_string(result) + ";";
            restoreLibraries(false);
            if (call(&ps2_stubs::scePadRead, {0u, 0u, kOut + 0x200u}) != 1)
                why += " scePadRead(0, 0) failed;";
            restoreLibraries(false);
            if (call(&ps2_stubs::sceCdSearchFile, {kOut + 0x300u, kBufBase + 8u * kBufStride}) != 1)
                why += " sceCdSearchFile(\"%d %d %d %d\") failed;";
            restoreLibraries(true);
            for (Region &r : regions)
                std::memcpy(r.mem, r.base.data(), r.size);
            return why;
        }

        // Back to the libraries' state from before the fixture (closes the fixture's files).
        void releaseLibraryFixture()
        {
            if (!libsReady)
                return;
            if (LibHook *mc = findLib("mc"))
                loadHook(*mc, mcEmpty);
            for (const LibHook &lib : libs)
                loadHook(lib, lib.pre);
            libsReady = false;
        }

        static int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        bool setUp()
        {
            runtime = new PS2Runtime();
            if (!runtime->memory().initialize())
                return false;
            PS2Memory &m = runtime->memory();
            rdram = m.getRDRAM();
            sp = kStackArea + 0x800u; // in RAM, 16-byte aligned

            // Buffers 0-7 zero; 8-15 format strings (the varargs handlers read them as formats,
            // the others as plain data). 14: ten conversions, so printf-style handlers take
            // varargs from the stack (sp+0..) as well; 13 and 15 mix 64-bit, string and
            // character conversions.
            const char *formats[8] = {"%d %d %d %d", "%x|%u|%c|%d", "%s %d", "%08x %-4d %hd %hhu",
                                      "%d %d %d %d", "%lld %s %llx %c %d", "%d %d %d %d %d %d %d %d %d %d",
                                      "%ld %p %lu %% %i"};
            for (uint32_t i = 8; i < kBufCount; ++i)
                std::memcpy(rdram + kBufBase + i * kBufStride, formats[i - 8], std::strlen(formats[i - 8]) + 1);

            regions.push_back({"rdram", rdram, PS2_RAM_SIZE, {}});
            regions.push_back({"scratch", m.getScratchpad(), PS2_SCRATCHPAD_SIZE, {}});
            regions.push_back({"iop", m.getIOPRAM(), 2u * 1024u * 1024u, {}});
            regions.push_back({"gsvram", m.getGSVRAM(), PS2_GS_VRAM_SIZE, {}});
            regions.push_back({"vu0code", m.getVU0Code(), PS2_VU0_CODE_SIZE, {}});
            regions.push_back({"vu0data", m.getVU0Data(), PS2_VU0_DATA_SIZE, {}});
            regions.push_back({"vu1code", m.getVU1Code(), PS2_VU1_CODE_SIZE, {}});
            regions.push_back({"vu1data", m.getVU1Data(), PS2_VU1_DATA_SIZE, {}});
            for (auto &r : regions)
            {
                if (!r.mem)
                    return false;
                r.base.assign(r.mem, r.mem + r.size);
            }

            poison_guard::install();
            std::thread([this]()
                        {
                            for (;;)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                const int64_t start = callStartMs.load();
                                const char *name = current.load();
                                if (start != 0 && name && nowMs() - start > 20000)
                                {
                                    std::string msg = std::string("[abi-diff] TIMEOUT in handler ") + name + ", state" +
                                                      currentState + "\n";
                                    poison_guard::writeRealStderr(msg.c_str());
                                    std::_Exit(97);
                                }
                            }
                        })
                .detach();
            return true;
        }

        // Argument values: zero, small integers, pointers to the buffers (start or inside),
        // int16 edge values, low RAM, and garbage in the upper 32 bits (the handlers must use the
        // low 32). No huge sizes: a pointer-sized count makes some handlers sweep RAM. No
        // scratchpad pointers: a pointer-sized length from there runs off the 16 KB host buffer
        // in handlers that take one direct pointer (sceMpegDemuxPssRing crashes that way, in
        // both forms; NOTES-typed.md), and a native crash leaves the handler's locks held.
        static uint64_t randomValue(std::mt19937_64 &rng)
        {
            static const uint64_t kEdges[] = {0x7FFFu, 0x8000u, 0xFFFFu, 0x18000u, 0x10000u, 640u, 448u, 0x80u,
                                              0x3Fu, 0x1u};
            switch (rng() % 9u)
            {
            case 0:
                return 0u;
            case 1:
                return rng() % 17u;
            case 2:
                return kBufBase + (rng() % kBufCount) * kBufStride;
            case 3:
                return kBufBase + (rng() % kBufCount) * kBufStride + (rng() % 256u) * 4u;
            case 4:
                return kEdges[rng() % std::size(kEdges)];
            case 5:
                return (rng() & 0xFFFFFFFF00000000ull) | (kBufBase + (rng() % kBufCount) * kBufStride);
            case 6:
                return (rng() & 0xFFFFFFFF00000000ull) | (rng() % 0x4000u);
            case 7:
                return 0x00100000u + (rng() % 0x1000u) * 4u;
            default:
                return rng() % 0x4000u;
            }
        }

        static State randomState(std::mt19937_64 &rng)
        {
            State s{};
            for (int i = 0; i < 8; ++i)
            {
                s.lo[i] = randomValue(rng);
                s.hi[i] = (rng() % 4u == 0u) ? rng() : 0u;
                s.f[i] = static_cast<float>(static_cast<int64_t>(rng() % 20001u) - 10000) / 16.0f;
            }
            for (uint32_t i = 0; i < kStackWindow / 4; ++i)
                s.stack[i] = static_cast<uint32_t>((i & 1u) ? ((rng() % 4u == 0u) ? rng() : 0u) : randomValue(rng));
            s.v0 = rng();
            s.v1 = rng();
            return s;
        }

        struct CallArgs
        {
            Handler fn;
            uint8_t *rdram;
            R5900Context *ctx;
            PS2Runtime *runtime;
            bool threw;
            std::string what;
        };

        static void callCpp(void *p)
        {
            auto *a = static_cast<CallArgs *>(p);
            try
            {
                a->fn(a->rdram, a->ctx, a->runtime);
            }
            catch (const std::exception &ex)
            {
                a->threw = true;
                a->what = ex.what();
            }
            catch (...)
            {
                a->threw = true;
                a->what = "(not a std::exception)";
            }
        }

        Observation run(const char *name, Handler fn, const State &s)
        {
            R5900Context ctx{};
            for (int i = 0; i < 8; ++i)
            {
                setGpr64(ctx, 4 + i, s.lo[i], s.hi[i]);
                ctx.f[12 + i] = s.f[i];
            }
            SET_GPR_U32(&ctx, 29, sp);
            SET_GPR_U32(&ctx, 31, 0x00100008u);
            setGpr64(ctx, 2, s.v0);
            setGpr64(ctx, 3, s.v1);
            ctx.pc = 0x00100000u;
            std::memcpy(rdram + sp, s.stack, kStackWindow);

            // Every call starts from the same library state (no stream callbacks, RPC servers or
            // transfers left by an earlier call or handler).
            ps2_stubs::resetMpegStubState();
            ps2_stubs::resetAudioStubState();
            ps2_stubs::resetSifState();
            const bool mcFiles = std::strncmp(name, "sceMc", 5) == 0 || std::strncmp(name, "abiSelfTestMc", 13) == 0;
            restoreLibraries(mcFiles);

            PS2Memory &m = runtime->memory();
            const uint64_t c0[4] = {m.dmaStartCount(), m.gifCopyCount(), m.gsWriteCount(), m.vifWriteCount()};
            runtime->configureGuestHeap(kHeapBase, kHeapLimit);
            CallArgs ca{fn, rdram, &ctx, runtime, false, {}};
            currentState = describeState(s); // for the timeout report
            current.store(name);
            callStartMs.store(nowMs());
            const uint32_t code = poison_guard::callGuarded(&Harness::callCpp, &ca);
            callStartMs.store(0);

            Observation o;
            o.crashCode = code;
            o.threw = ca.threw;
            o.what = ca.what;
            o.v0 = gpr64(ctx, 2);
            o.v1 = gpr64(ctx, 3);
            o.f0 = ctx.f[0];
            ctx.insn_count = 0;
            ctx.cop0_count = 0;
            o.ctx.assign(reinterpret_cast<const uint8_t *>(&ctx), reinterpret_cast<const uint8_t *>(&ctx) + sizeof(ctx));
            if (libsReady)
            {
                o.libHash = libStateHash();
                if (mcFiles)
                    o.fsHash = mcFilesHash();
            }
            const uint64_t c1[4] = {m.dmaStartCount(), m.gifCopyCount(), m.gsWriteCount(), m.vifWriteCount()};
            for (int i = 0; i < 4; ++i)
                o.counters[i] = c1[i] - c0[i];
            for (size_t r = 0; r < regions.size(); ++r)
            {
                Region &reg = regions[r];
                for (size_t off = 0; off < reg.size; off += 4096)
                {
                    const size_t n = std::min<size_t>(4096, reg.size - off);
                    if (std::memcmp(reg.mem + off, reg.base.data() + off, n) != 0)
                    {
                        o.pages.push_back({static_cast<uint8_t>(r), static_cast<uint32_t>(off / 4096), fnv(reg.mem + off, n)});
                        std::memcpy(reg.mem + off, reg.base.data() + off, n);
                    }
                }
            }
            return o;
        }
    };

    Harness &harness()
    {
        static Harness *h = nullptr;
        if (!h)
        {
            h = new Harness();
            if (!h->setUp())
            {
                static Harness broken;
                return broken;
            }
        }
        return *h;
    }

    struct DiffResult
    {
        std::string label;
        uint32_t iterations = 0, compared = 0, nondet = 0, diffs = 0, threw = 0, crashed = 0, transient = 0;
        std::string firstTransient;
        int64_t elapsedMs = 0;
        bool stoppedOnCrash = false;
        bool stoppedOnBudget = false;
        std::string crashState;
        std::string firstThrow;
        std::string firstDiff;
        std::string firstNondet;
    };

    // budgetMs > 0: stop once the handler has used that much time (the count says how many
    // states ran). A native crash stops the handler: it may have left a lock held.
    DiffResult differential(const ps2_abi::Entry &e, uint32_t iterations, bool verbose, int64_t budgetMs = 0)
    {
        Harness &h = harness();
        DiffResult r;
        r.label = std::string(kindName(e.kind)) + " " + e.name;
        uint64_t seed = 0x9E3779B97F4A7C15ull ^ fnv(reinterpret_cast<const uint8_t *>(e.name), std::strlen(e.name));
        std::mt19937_64 rng(seed);
        const int64_t start = Harness::nowMs();
        for (uint32_t it = 0; it < iterations; ++it)
        {
            if (budgetMs > 0 && Harness::nowMs() - start > budgetMs)
            {
                r.stoppedOnBudget = true;
                break;
            }
            const State s = Harness::randomState(rng);
            ++r.iterations;
            Observation l1, l2, t;
            try
            {
                l1 = h.run(e.name, e.legacy, s);
                if (!l1.crashCode)
                {
                    l2 = h.run(e.name, e.legacy, s);
                    t = h.run(e.name, e.typed, s);
                }
            }
            catch (const std::exception &ex)
            {
                // The per-call reset failed: a lock left held by an earlier native crash.
                r.stoppedOnCrash = true;
                r.firstThrow = std::string("harness: ") + ex.what();
                r.crashState = "iteration " + std::to_string(it) + ":" + describeState(s);
                break;
            }
            if (l1.crashCode)
            {
                char code[32];
                std::snprintf(code, sizeof(code), "0x%x", l1.crashCode);
                r.crashed += 1;
                r.stoppedOnCrash = true;
                r.firstThrow = std::string("native exception ") + code + " (legacy form)";
                r.crashState = "iteration " + std::to_string(it) + ":" + describeState(s);
                break;
            }
            r.threw += l1.threw ? 1 : 0;
            if (r.firstThrow.empty() && l1.threw)
                r.firstThrow = l1.what.substr(0, 80);
            if (l2.crashCode || t.crashCode)
            {
                r.crashed += 1;
                r.stoppedOnCrash = true;
                r.crashState = "iteration " + std::to_string(it) + " (" + (t.crashCode ? "typed" : "legacy, second call") +
                               "):" + describeState(s);
                if (t.crashCode && !l2.crashCode)
                {
                    ++r.compared;
                    ++r.diffs;
                    if (r.firstDiff.empty())
                        r.firstDiff = "iteration " + std::to_string(it) + ":" + describeDiff(l1, t);
                }
                break;
            }
            if (!(l1 == l2))
            {
                ++r.nondet;
                if (r.firstNondet.empty())
                    r.firstNondet = describeDiff(l1, l2);
                continue;
            }
            if (!(l1 == t))
            {
                // A third legacy run after the typed one: if it no longer matches the first two,
                // the state changed under the handler (the host clock ticked to a new second, as
                // sceCdReadClock reads it), which is nondeterminism, not a decoding difference.
                Observation l3 = h.run(e.name, e.legacy, s);
                if (!(l3 == l1))
                {
                    ++r.nondet;
                    if (r.firstNondet.empty())
                        r.firstNondet = describeDiff(l1, l3);
                    continue;
                }
                // And the typed form a second time: if it now agrees, the first typed run was hit by a
                // change from outside the call (a runtime thread finishing work an earlier handler
                // started, e.g. IOP module loads), not by its decoding. A decoding difference is
                // deterministic and differs again.
                Observation t2 = h.run(e.name, e.typed, s);
                if (t2 == l1)
                {
                    ++r.nondet;
                    ++r.transient;
                    if (r.firstTransient.empty())
                        r.firstTransient = "iteration " + std::to_string(it) + ":" + describeDiff(l1, t);
                    continue;
                }
            }
            ++r.compared;
            if (!(l1 == t))
            {
                ++r.diffs;
                if (r.firstDiff.empty() || verbose)
                {
                    const std::string d = "iteration " + std::to_string(it) + ":" + describeDiff(l1, t) +
                                          (verbose || r.firstDiff.empty() ? "\n        state:" + describeState(s) : "");
                    if (r.firstDiff.empty())
                        r.firstDiff = d;
                }
            }
        }
        r.elapsedMs = Harness::nowMs() - start;
        return r;
    }

    // Silences stdout/stderr for its lifetime, also when the test leaves by an exception.
    struct Silence
    {
        Silence() { poison_guard::silenceStdio(true); }
        ~Silence() { poison_guard::silenceStdio(false); }
        Silence(const Silence &) = delete;
        Silence &operator=(const Silence &) = delete;
    };

    bool nameListed(const char *name, const char *list)
    {
        if (!list || !*list)
            return true;
        std::stringstream ss(list);
        std::string part;
        while (std::getline(ss, part, ','))
            if (!part.empty() && std::string(name).find(part) != std::string::npos)
                return true;
        return false;
    }

    // Calls a handler on a hand-built context (no harness needed: rdram is a plain buffer).
    struct Mini
    {
        std::vector<uint8_t> ram = std::vector<uint8_t>(PS2_RAM_SIZE, 0);
        R5900Context ctx{};
        uint32_t sp = 0x00080000u;

        Mini() { SET_GPR_U32(&ctx, 29, sp); }
        void stack64(uint32_t slot, uint64_t v) { std::memcpy(ram.data() + sp + slot * 8u, &v, 8); }
        void call(Handler h) { h(ram.data(), &ctx, nullptr); }
    };
}

void register_ps2_stub_abi_tests()
{
    MiniTest::Case("StubAbi", [](TestCase &tc)
    {
        tc.Run("decoder: ten integer arguments, the 9th and 10th from sp+0 and sp+8", [](TestCase &t)
        {
            Mini m;
            const uint64_t vals[8] = {static_cast<uint64_t>(-3), 0xFFFFFFFF00000005ull, static_cast<uint64_t>(-7ll),
                                      0x100000000ull, 0xABCD0000000000F0ull, 11, 12, 13};
            for (int i = 0; i < 8; ++i)
                setGpr64(m.ctx, 4 + i, vals[i], 0xDEADBEEFDEADBEEFull);
            m.stack64(0, 0xFFFFFFFF00000014ull); // a9 = 20 (int32: low word)
            m.stack64(1, 0x0000000100000002ull); // a10 = 0x1_0000_0002 (uint64: both words)
            m.stack64(2, 0x7777777777777777ull); // above the arguments: must not be read
            m.call(&ps2_abi_selftest::abiSelfTestTen);
            const int64_t expected = -3 * 1 + 5 * 2 + -7 * 3 + 0x100000000ll * 4 + 0xF0ll * 5 + 11 * 6 + 12 * 7 +
                                     13 * 8 + 20 * 9 + 0x100000002ll * 10;
            t.Equals(gpr64(m.ctx, 2), static_cast<uint64_t>(expected), "64-bit return in v0");
            t.Equals(gpr64(m.ctx, 3), static_cast<uint64_t>(static_cast<uint32_t>(static_cast<uint64_t>(expected) >> 32)),
                     "64-bit return: high word in v1 (setReturnU64 convention)");
        });

        tc.Run("decoder: narrowing to short and char, s32 return sign-extended", [](TestCase &t)
        {
            Mini m;
            setGpr64(m.ctx, 4, 0x18000u); // int16 -> -32768
            setGpr64(m.ctx, 5, 0xFFFF1234u);
            setGpr64(m.ctx, 6, 0x1FFu); // int8 -> -1
            setGpr64(m.ctx, 7, 0x380u); // uint8 -> 0x80
            m.call(&ps2_abi_selftest::abiSelfTestNarrow);
            int32_t out[4];
            std::memcpy(out, m.ram.data() + 0x00100000u, sizeof(out));
            t.Equals(out[0], -32768, "int16 of 0x1_8000");
            t.Equals(out[1], 0x1234, "uint16 of 0xFFFF1234");
            t.Equals(out[2], -1, "int8 of 0x1FF");
            t.Equals(out[3], 0x80, "uint8 of 0x380");
            t.Equals(gpr64(m.ctx, 2), 0xFFFFFFFFFFFF8000ull, "s32 return sign-extended to 64 bits");
        });

        tc.Run("decoder: floats in f12-f19, the ninth at sp+0, float return in f0", [](TestCase &t)
        {
            Mini m;
            setGpr64(m.ctx, 4, 0x00110000u);
            for (int i = 0; i < 8; ++i)
                m.ctx.f[12 + i] = 1.5f + static_cast<float>(i);
            float ninth = -250.25f;
            uint32_t bits = 0;
            std::memcpy(&bits, &ninth, 4);
            m.stack64(0, 0xCAFEBABE00000000ull | bits);
            const uint64_t v0Before = 0x1122334455667788ull;
            setGpr64(m.ctx, 2, v0Before);
            m.call(&ps2_abi_selftest::abiSelfTestFloats);
            float out[9];
            std::memcpy(out, m.ram.data() + 0x00110000u, sizeof(out));
            bool ok = true;
            for (int i = 0; i < 8; ++i)
                ok = ok && out[i] == 1.5f + static_cast<float>(i);
            t.IsTrue(ok, "f1-f8 from f12-f19");
            t.Equals(out[8], ninth, "the ninth float from the low word of sp+0");
            t.Equals(m.ctx.f[0], ninth * 2.0f, "float return in f0");
            t.Equals(gpr64(m.ctx, 2), v0Before, "a float return leaves v0 alone");
        });

        tc.Run("decoder: varargs continue the integer slots (a2.., then sp+0 in 8-byte slots)", [](TestCase &t)
        {
            Mini m;
            setGpr64(m.ctx, 4, 0x00120000u);
            setGpr64(m.ctx, 5, 9);
            for (int i = 2; i < 8; ++i)
                setGpr64(m.ctx, 4 + i, 0x1000000000ull + static_cast<uint64_t>(i), 0xFFFFFFFFFFFFFFFFull);
            m.stack64(0, 0x8888888877777777ull);
            m.stack64(1, 0x0000000000000042ull);
            m.stack64(2, 0x1234567890ABCDEFull);
            m.call(&ps2_abi_selftest::abiSelfTestVarArgs);
            uint64_t out[9];
            std::memcpy(out, m.ram.data() + 0x00120000u, sizeof(out));
            bool regs = true;
            for (int i = 0; i < 6; ++i)
                regs = regs && out[i] == 0x1000000000ull + static_cast<uint64_t>(i + 2);
            t.IsTrue(regs, "varargs 0-5 from a2-t3, 64 bits each");
            t.Equals(out[6], 0x8888888877777777ull, "vararg 6 from sp+0 (one 8-byte slot)");
            t.Equals(out[7], 0x42ull, "vararg 7 from sp+8");
            t.Equals(out[8], 0x1234567890ABCDEFull, "vararg 8 from sp+16");
            t.Equals(gpr64(m.ctx, 2), 2ull, "first vararg slot = 2");
        });

        tc.Run("decoder: a void handler leaves v0 untouched", [](TestCase &t)
        {
            Mini m;
            setGpr64(m.ctx, 2, 0xFEEDFACE12345678ull);
            m.call(&ps2_abi_selftest::abiSelfTestVoid);
            t.Equals(gpr64(m.ctx, 2), 0xFEEDFACE12345678ull, "v0 untouched");
            t.Equals(getRegU32(&m.ctx, 8), 0x5A5A5A5Au, "the body ran");
        });

        tc.Run("dispatcher follows the switch", [](TestCase &t)
        {
            Mini m;
            setGpr64(m.ctx, 4, 10);
            setGpr64(m.ctx, 5, 3);
            setGpr64(m.ctx, 8, 100);
            ps2_abi_selftest::g_typedCalls = 0;
            ps2_abi_selftest::g_legacyCalls = 0;
            ps2_abi::setTypedEnabledForTests(1);
            t.IsTrue(ps2_abi::typedEnabled(), "forced on");
            m.call(&ps2_abi_selftest::abiSelfTestPair);
            t.Equals(ps2_abi_selftest::g_typedCalls.load(), 1, "typed form called when on");
            t.Equals(ps2_abi_selftest::g_legacyCalls.load(), 0, "legacy form not called when on");
            t.Equals(getRegU32(&m.ctx, 2), 107u, "typed result (a0 - a1 + arg 5 in t0)");
            ps2_abi::setTypedEnabledForTests(0);
            t.IsFalse(ps2_abi::typedEnabled(), "forced off");
            m.call(&ps2_abi_selftest::abiSelfTestPair);
            t.Equals(ps2_abi_selftest::g_typedCalls.load(), 1, "typed form not called when off");
            t.Equals(ps2_abi_selftest::g_legacyCalls.load(), 1, "legacy form called when off");
            t.Equals(getRegU32(&m.ctx, 2), 107u, "legacy result");
            ps2_abi::setTypedEnabledForTests(-1);
            const char *env = std::getenv("PS2X_TYPED_STUBS");
            const bool expected = env && *env ? !(env[0] == '0' && env[1] == '\0') : (PS2X_TYPED_STUBS_DEFAULT != 0);
            t.Equals(ps2_abi::typedEnabled(), expected, "back to the environment / build default");
            std::printf("[abi] typed stubs %s (PS2X_TYPED_STUBS=%s, build default %d)\n",
                        ps2_abi::typedEnabled() ? "ON" : "off", env ? env : "(unset)", PS2X_TYPED_STUBS_DEFAULT);
        });

        tc.Run("registry holds every converted handler; aliases resolve; JSON lists them", [](TestCase &t)
        {
            for (const Expected &x : kConverted)
            {
                const ps2_abi::Entry *e = ps2_abi::find(x.kind, x.name);
                t.IsNotNull(e, std::string("registered: ") + kindName(x.kind) + " " + x.name);
                if (!e)
                    continue;
                if (x.alias)
                {
                    t.IsNotNull(e->aliasOf, std::string("alias: ") + x.name);
                    const ps2_abi::Entry *target = ps2_abi::resolveAlias(*e);
                    t.IsTrue(target && target != e && target->legacy, std::string("alias resolves to a pair: ") + x.name);
                }
                else
                {
                    t.IsTrue(e->typed && e->legacy && e->dispatch, std::string("typed + legacy + dispatch: ") + x.name);
                }
            }
            // No name registered twice in one table.
            std::set<std::pair<int, std::string>> seen;
            for (const ps2_abi::Entry &e : ps2_abi::registry())
                t.IsTrue(seen.insert({static_cast<int>(e.kind), e.name}).second, std::string("registered once: ") + e.name);

            const ps2_abi::Entry *ten = ps2_abi::find(ps2_abi::Kind::Stub, "abiSelfTestTen");
            t.IsTrue(ten && ten->sig.intArgs == 10 && ten->sig.stackSlots == 2 && ten->sig.ret == ps2_abi::Type::S64,
                     "signature of a 10-argument handler: 10 ints, 2 stack slots, s64");
            const ps2_abi::Entry *fl = ps2_abi::find(ps2_abi::Kind::Stub, "abiSelfTestFloats");
            t.IsTrue(fl && fl->sig.intArgs == 1 && fl->sig.floatArgs == 9 && fl->sig.stackSlots == 1,
                     "signature with nine floats: one stack slot");
            const ps2_abi::Entry *va = ps2_abi::find(ps2_abi::Kind::Stub, "abiSelfTestVarArgs");
            t.IsTrue(va && va->sig.varargs && va->sig.intArgs == 2, "varargs signature");
            const ps2_abi::Entry *rpc = ps2_abi::find(ps2_abi::Kind::Syscall, "SifCallRpc");
            t.IsTrue(rpc && rpc->sig.selfReturn && rpc->sig.intArgs == 9 && rpc->sig.stackSlots == 1,
                     "SifCallRpc: 9 integer arguments, end_para at sp+0, self return");
            const ps2_abi::Entry *clr = ps2_abi::find(ps2_abi::Kind::Stub, "sceGsSetDefClear");
            t.IsTrue(clr && clr->sig.intArgs == 11 && clr->sig.stackSlots == 3, "sceGsSetDefClear: 11 arguments, 3 on the stack");

            const std::string json = ps2_abi::registryJson();
            int depth = 0, minDepth = 0;
            bool inString = false;
            for (size_t i = 0; i < json.size(); ++i)
            {
                const char c = json[i];
                if (inString)
                {
                    if (c == '\\')
                        ++i;
                    else if (c == '"')
                        inString = false;
                    continue;
                }
                if (c == '"')
                    inString = true;
                else if (c == '{' || c == '[')
                    ++depth;
                else if (c == '}' || c == ']')
                    minDepth = std::min(minDepth, --depth);
            }
            t.IsTrue(depth == 0 && minDepth == 0 && !inString, "JSON brackets balance");
            for (const Expected &x : kConverted)
            {
                const std::string needle = std::string("\"name\": \"") + x.name + "\", \"kind\": \"" + kindName(x.kind) + "\"";
                t.IsTrue(json.find(needle) != std::string::npos, "JSON lists " + needle);
            }
            t.IsTrue(json.find("\"unresolved\"") == std::string::npos, "every alias in the JSON resolves");
            t.IsTrue(json.find("\"alias_of\": \"SifRegisterRpc\", \"alias_kind\": \"syscall\"") != std::string::npos,
                     "sceSifRegisterRpc names its syscall target");

            // Deliberate deviations from the SDK prototypes are marked for the tooling.
            for (const char *name : {"scePrintf", "sceGsSetDefDBuffDc", "sceGsSetDefDispEnv", "sceGsSetDefLoadImage",
                                     "sceSdRemote"})
                t.IsNotNull(ps2_abi::deviation(ps2_abi::Kind::Stub, name), std::string("deviation noted: ") + name);
            t.IsTrue(ps2_abi::deviation(ps2_abi::Kind::Stub, "sceGsSetDefDBuff") == nullptr,
                     "no deviation where the declaration follows the SDK");
            const auto entryJson = [&json](const char *name, const char *kind) -> std::string
            {
                const std::string needle = std::string("{\"name\": \"") + name + "\", \"kind\": \"" + kind + "\"";
                const size_t at = json.find(needle);
                return at == std::string::npos ? std::string() : json.substr(at, json.find('}', at) - at);
            };
            t.IsTrue(entryJson("scePrintf", "stub").find("\"deviation\": \"SDK: int scePrintf") != std::string::npos,
                     "JSON: scePrintf carries its deviation");
            t.IsTrue(entryJson("sceGsSetDefStoreImage", "stub").find("\"deviation\"") != std::string::npos,
                     "JSON: an alias inherits its target's deviation");
            t.IsTrue(entryJson("sceGsSetDefDBuff", "stub").find("\"deviation\"") == std::string::npos,
                     "JSON: no deviation field where none is declared");
        });

        tc.Run("decoder: stack words through the runtime memory map when there is no direct pointer", [](TestCase &t)
        {
            // readGuestWord takes the direct pointer (getConstMemPtr) whenever rdram is given: that
            // resolves every address (scratchpad, KSEG and uncached mirrors, anything else masked into
            // RAM), so the typed and the legacy readers agree on any sp. The runtime's memory map is
            // the fallback when there is no direct pointer (no rdram): checked here through a typed
            // handler with sp in RAM, in VU1 data memory and in an unmapped range.
            Harness &h = harness();
            t.IsNotNull(h.runtime, "harness set-up");
            if (!h.runtime)
                return;
            PS2Memory &mem = h.runtime->memory();
            uint8_t *vu1 = mem.getVU1Data();
            const uint32_t spRam = kStackArea + 0x400u;
            const uint32_t spVu = PS2_VU1_DATA_BASE + 0x100u;
            const uint32_t spVuAlias = spVu & PS2_RAM_MASK; // where getConstMemPtr puts that sp in RAM
            std::vector<uint8_t> savedRam(h.rdram + spRam, h.rdram + spRam + 16);
            std::vector<uint8_t> savedAlias(h.rdram + spVuAlias, h.rdram + spVuAlias + 16);
            std::vector<uint8_t> savedVu(vu1 + 0x100u, vu1 + 0x110u);
            const auto put = [](uint8_t *p, uint64_t slot0, uint64_t slot1)
            {
                std::memcpy(p, &slot0, 8);
                std::memcpy(p + 8, &slot1, 8);
            };
            put(h.rdram + spRam, 0xFFFFFFFF00000014ull, 0x0000000100000002ull);
            put(vu1 + 0x100u, 30u, 3u);
            put(h.rdram + spVuAlias, 40u, 4u);

            const auto call = [&h](uint8_t *rdram, uint32_t sp) -> int64_t
            {
                R5900Context ctx{};
                for (int i = 0; i < 8; ++i)
                    setGpr64(ctx, 4 + i, static_cast<uint64_t>(i + 1));
                SET_GPR_U32(&ctx, 29, sp);
                ps2_abi_selftest::abiSelfTestTen(rdram, &ctx, h.runtime);
                return static_cast<int64_t>(gpr64(ctx, 2));
            };
            // Arguments 1-8 are 1..8 (weights 1..8): 204; then a9 * 9 + a10 * 10.
            const int64_t regs = 1 * 1 + 2 * 2 + 3 * 3 + 4 * 4 + 5 * 5 + 6 * 6 + 7 * 7 + 8 * 8;
            t.Equals(call(nullptr, spRam), regs + 20 * 9 + 0x100000002ll * 10, "no rdram, sp in RAM: the map reads RAM");
            t.Equals(call(nullptr, spVu), regs + 30 * 9 + 3 * 10, "no rdram, sp in VU1 data: the map reads VU1 memory");
            t.Equals(call(h.rdram, spVu), regs + 40 * 9 + 4 * 10,
                     "with rdram the direct pointer wins (VU1 address aliased into RAM, as the legacy readers do)");
            t.Equals(call(nullptr, 0x0A000000u), regs, "no rdram, sp unmapped: the stack arguments read as 0");

            std::memcpy(h.rdram + spRam, savedRam.data(), savedRam.size());
            std::memcpy(h.rdram + spVuAlias, savedAlias.data(), savedAlias.size());
            std::memcpy(vu1 + 0x100u, savedVu.data(), savedVu.size());
        });

        tc.Run("differential: typed and legacy forms agree on random register and stack states", [](TestCase &t)
        {
            Harness &h = harness();
            t.IsNotNull(h.runtime, "harness set-up");
            if (!h.runtime)
                return;
            uint32_t iterations = 64;
            int64_t budgetMs = 3000; // per handler, only when the count is not given
            if (const char *s = std::getenv("PS2X_ABI_DIFF_ITERS"))
            {
                iterations = static_cast<uint32_t>(std::max(1, std::atoi(s)));
                budgetMs = 0;
            }
            const char *only = std::getenv("PS2X_ABI_DIFF_ONLY");
            const bool verbose = std::getenv("PS2X_ABI_DIFF_VERBOSE") != nullptr;

            const PS2Runtime::IoPaths savedPaths = PS2Runtime::getIoPaths();
            std::error_code ec;
            const std::filesystem::path scratch = std::filesystem::temp_directory_path(ec) /
                ("ps2x_abi_diff_test_" + std::to_string(currentProcessId())); // per process: two test runs at once must not share it
            std::filesystem::remove_all(scratch, ec);
            std::filesystem::create_directories(scratch / "mc0", ec);
            {
                PS2Runtime::IoPaths p;
                p.elfDirectory = scratch;
                p.hostRoot = scratch;
                p.cdRoot = scratch;
                p.mcRoot = scratch / "mc0";
                PS2Runtime::setIoPaths(p);
            }
            std::string fixtureProblem;
            {
                Silence quiet;
                h.prepareLibraryFixture(scratch);
                fixtureProblem = h.checkLibraryFixture();
            }
            t.IsTrue(fixtureProblem.empty(), "library fixture reaches the success paths:" + fixtureProblem);

            // The harness must see a wrong decoding (the bad pair reads sp+0x10).
            const ps2_abi::Entry *bad = ps2_abi::find(ps2_abi::Kind::Stub, "abiSelfTestBadPair");
            const ps2_abi::Entry *good = ps2_abi::find(ps2_abi::Kind::Stub, "abiSelfTestPair");
            t.IsTrue(bad && good, "self-test pairs registered");
            if (bad && good)
            {
                DiffResult b, g;
                {
                    Silence quiet;
                    b = differential(*bad, 64, false);
                    g = differential(*good, 64, false);
                }
                t.IsTrue(b.diffs > 0, "a legacy form reading sp+0x10 must differ from the typed one");
                t.IsTrue(g.diffs == 0 && g.nondet == 0, "an agreeing pair must not differ");
            }
            const ps2_abi::Entry *lib = ps2_abi::find(ps2_abi::Kind::Stub, "abiSelfTestLibPair");
            const ps2_abi::Entry *mcf = ps2_abi::find(ps2_abi::Kind::Stub, "abiSelfTestMcFilePair");
            t.IsTrue(lib && mcf, "library / file self-test pairs registered");
            if (lib && mcf)
            {
                DiffResult l, f;
                {
                    Silence quiet;
                    l = differential(*lib, 64, false);
                    f = differential(*mcf, 64, false);
                }
                t.IsTrue(l.diffs > 0 && l.nondet == 0, "a pair differing only in library state must differ");
                t.IsTrue(f.diffs > 0 && f.nondet == 0, "a pair differing only in a memory-card file must differ");
            }


            const auto t0 = std::chrono::steady_clock::now();
            std::vector<DiffResult> results;
            {
                Silence quiet;
                for (const ps2_abi::Entry &e : ps2_abi::registry())
                {
                    if (!e.legacy || isSelfTest(e.name) || !nameListed(e.name, only))
                        continue;
                    results.push_back(differential(e, iterations, verbose, budgetMs));
                }
            }
            {
                Silence quiet;
                h.releaseLibraryFixture();
            }
            PS2Runtime::setIoPaths(savedPaths);
            std::filesystem::remove_all(scratch, ec);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            std::ostringstream report;
            uint32_t totalIter = 0, totalCompared = 0, totalNondet = 0, totalDiffs = 0;
            for (const DiffResult &r : results)
            {
                totalIter += r.iterations;
                totalCompared += r.compared;
                totalNondet += r.nondet;
                totalDiffs += r.diffs;
                report << "[abi-diff] " << r.label << ": " << r.iterations << " states, " << r.compared << " compared, "
                       << r.nondet << " nondeterministic, " << r.diffs << " differ"
                       << (r.threw ? ", threw " + std::to_string(r.threw) : std::string())
                       << (r.crashed ? ", crashed " + std::to_string(r.crashed) : std::string()) << ", "
                       << r.elapsedMs << " ms\n";
                if (r.stoppedOnBudget)
                    report << "      (stopped at the " << budgetMs << " ms budget; PS2X_ABI_DIFF_ITERS=N runs all N)\n";
                if (!r.firstThrow.empty())
                    report << "      first exception: " << r.firstThrow << "\n";
                if (r.stoppedOnCrash)
                    report << "      stopped on a native crash, " << r.crashState << "\n";
                if (!r.firstDiff.empty())
                    report << "      first diff " << r.firstDiff << "\n";
                if (r.transient)
                    report << "      " << r.transient << " transient (one typed run disturbed from outside the call, "
                           << "a repeat agreed; counted as nondeterministic), first " << r.firstTransient << "\n";
                if (verbose && !r.firstNondet.empty())
                    report << "      first nondet" << r.firstNondet << "\n";
            }
            report << "[abi-diff] fixture: " << h.retriedRestores
                   << " library reloads needed a retry (a reload that did not reach the fixture state)";
            for (const std::string &lib : h.retriedLibs)
                report << " " << lib;
            report << "\n";
            report << "[abi-diff] handlers " << results.size() << ": states " << totalIter << ", compared " << totalCompared
                   << ", nondeterministic " << totalNondet << ", differ " << totalDiffs << ", "
                   << static_cast<int>(secs) << " s\n";
            poison_guard::writeRealStdout(report.str().c_str());

            for (const DiffResult &r : results)
            {
                t.IsTrue(r.diffs == 0, "typed and legacy differ: " + r.label + " (" + r.firstDiff + ")");
                // Each pair must actually be compared, not only skipped as nondeterministic.
                if (!alwaysNondeterministic(r.label))
                    t.IsTrue(r.compared > 0, "never compared (every state nondeterministic): " + r.label);
                // A native crash (same in both forms or not) leaves locks held for every later call.
                t.IsTrue(!r.stoppedOnCrash, "native crash: " + r.label + " " + r.firstThrow + " " + r.crashState);
            }
            if (!only)
            {
                size_t pairs = 0;
                for (const Expected &x : kConverted)
                    pairs += x.alias ? 0u : 1u;
                t.Equals(results.size(), pairs, "every transition pair was tested");
            }
        });
    });
}
