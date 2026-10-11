// sceGsSetDefDBuff / sceGsSwapDBuff / sceGsPutDrawEnv against SDK 3.1.0 libgraph.
//
// The expected sceGsDBuff images below are literal values computed from the decompiled
// libgraph copies in SLPS-25887 (Super Robot Taisen Z), not from the runtime:
//   sceGsSetDefDBuff   0x10cf28  layout: disp[0] +0x00, disp[1] +0x28 (sceGsSetDefDispEnv(psm,
//                                w, h, 0, 0) twice), giftag0 +0x50, draw0 +0x60, clear0 +0xe0,
//                                giftag1 +0x140, draw1 +0x150, clear1 +0x1d0; draw envs via
//                                sceGsSetDefDrawEnv(psm, w, h, ztest, zpsm); with clear != 0
//                                sceGsSetDefClear(ztest, 0x800 - (w >> 1), 0x800 - (h >> 1), w, h,
//                                0, 0, 0, 0, 0) (stack args b, a, z stored as 0 at 0x10d010-
//                                0x10d024); both giftags zeroed (por/sq at 0x10d078-0x10d088) then
//                                lo = NLOOP (8, or 14 with clear) | EOP 0x8000 | NREG 1 << 60,
//                                hi = REGS 0xe; zbuf = sceGszbufaddr(psm, w, h); when gparam is
//                                (interlace 1, ffmode 1) or interlace 0, draw0 FRAME (+0x60) and
//                                disp[1] DISPFB (+0x38) get FBP = (zbuf >> 1) & 0x1ff (0x10d11c-
//                                0x10d18c).
//   sceGsSetDefDispEnv 0x10c738  PMODE 0x66; SMODE2 2 (non-interlaced), 3 (interlaced, ffmode 1),
//                                1 (interlaced, ffmode 0); DISPFB (psm & 0xf) << 15 | fbw << 9;
//                                NTSC DISPLAY: mult = (w + 0x9ff) / w, DX 0x27c, DY 0x32 interlaced
//                                or 0x19, MAGH mult - 1, DW mult * w - 1, DH h - 1, or 2h - 1
//                                when interlaced with ffmode != 0 (FRAME mode: DH counts frame
//                                lines); BGCOLOR 0. The OSD display offsets libgraph
//                                reads through syscall 0x80 on newer ROMs are taken as 0.
//   sceGszbufaddr      0x10cb70  ((w + 63) >> 6) * ((h + 31) >> 5), or (h + 63) >> 6 when psm & 2,
//                                times 1 when gparam is (interlace 1, ffmode 0), else times 2.
//   sceGsSetDefDrawEnv 0x10cc38  FRAME_1 (0x4c) fbw << 16 | (psm & 0xf) << 24; ZBUF_1 (0x4e)
//                                zbuf | (zpsm & 0xf) << 24 | ZMSK when ztest == 0; XYOFFSET_1 (0x18)
//                                (0x800 - w/2) * 16 | (0x800 - h/2) << 36; SCISSOR_1 (0x40)
//                                (w - 1) << 16 | (h - 1) << 48; PRMODECONT (0x1a) and COLCLAMP
//                                (0x46) old | 1; DTHE (0x45) old | 1 if psm & 2 else old & ~1;
//                                TEST_1 (0x47) 0x30000 if ztest == 0 else (ztest & 3) << 17 | 0x10000.
//                                "old" is what the memory held: the dirty cases prefill 0x5a bytes.
//   sceGsSetDefClear   0x10ce20  TEST_1 0x30000; PRIM (0) 6; RGBAQ (1) rgba | Q 1.0 (0x3f800000 << 32);
//                                XYZ2 (5) x << 4 | (y << 4) << 16 | z << 32 and (x + w), (y + h);
//                                TEST_1 restored as above.
//   sceGsSwapDBuff (not linked in SRWZ): SDK 3.x symbol database (ps2xAnalyzer) shows 92 bytes
//                                calling sceGsPutDispEnv once and sceGsPutDrawEnv on two branches,
//                                i.e. PutDispEnv(&disp[id & 1]) then PutDrawEnv(&giftag0 / &giftag1);
//                                sceGsPutDrawEnv (232 bytes, no calls but printf) starts GIF DMA on
//                                the giftag and its NLOOP qwords.
//
// Expected arrays generated with an independent Python model of the lines above; gparam is
// set with sceGsResetGraph(0, interlace, omode, ffmode). Returns non-zero on any failure.

#include "ps2_runtime.h"
#include "ps2_stubs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

    void setReg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
    }

    struct Case
    {
        const char *name;
        uint32_t psm, w, h, ztest, zpsm, clear;
        uint32_t gparam[3]; // interlace, omode, ffmode
        uint8_t fill;
        uint64_t q[0x230 / 8];
    };

    const Case kCases[] = {
// ct32_640x224_z24_ztest1_clear_frame
{"ct32_640x224_z24_ztest1_clear_frame", 0, 640, 224, 1, 0x31, 1, {1, 2, 1}, 0x00, {
    0x0000000000000066ull, 0x0000000000000003ull, 0x0000000000001400ull, 0x001bf9ff0183227cull,
    0x0000000000000000ull, 0x0000000000000066ull, 0x0000000000000003ull, 0x0000000000001446ull,
    0x001bf9ff0183227cull, 0x0000000000000000ull, 0x100000000000800eull, 0x000000000000000eull,
    0x00000000000a0046ull, 0x000000000000004cull, 0x000000000100008cull, 0x000000000000004eull,
    0x0000790000006c00ull, 0x0000000000000018ull, 0x00df0000027f0000ull, 0x0000000000000040ull,
    0x0000000000000001ull, 0x000000000000001aull, 0x0000000000000001ull, 0x0000000000000046ull,
    0x0000000000000000ull, 0x0000000000000045ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x0000000000000006ull, 0x0000000000000000ull,
    0x3f80000000000000ull, 0x0000000000000001ull, 0x0000000079006c00ull, 0x0000000000000005ull,
    0x0000000087009400ull, 0x0000000000000005ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x100000000000800eull, 0x000000000000000eull, 0x00000000000a0000ull, 0x000000000000004cull,
    0x000000000100008cull, 0x000000000000004eull, 0x0000790000006c00ull, 0x0000000000000018ull,
    0x00df0000027f0000ull, 0x0000000000000040ull, 0x0000000000000001ull, 0x000000000000001aull,
    0x0000000000000001ull, 0x0000000000000046ull, 0x0000000000000000ull, 0x0000000000000045ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x0000000000000006ull, 0x0000000000000000ull, 0x3f80000000000000ull, 0x0000000000000001ull,
    0x0000000079006c00ull, 0x0000000000000005ull, 0x0000000087009400ull, 0x0000000000000005ull,
    0x0000000000030000ull, 0x0000000000000047ull,
}},
// ct32_640x224_z24_ztest1_clear_frame_dirty
{"ct32_640x224_z24_ztest1_clear_frame_dirty", 0, 640, 224, 1, 0x31, 1, {1, 2, 1}, 0x5a, {
    0x0000000000000066ull, 0x0000000000000003ull, 0x0000000000001400ull, 0x001bf9ff0183227cull,
    0x0000000000000000ull, 0x0000000000000066ull, 0x0000000000000003ull, 0x0000000000001446ull,
    0x001bf9ff0183227cull, 0x0000000000000000ull, 0x100000000000800eull, 0x000000000000000eull,
    0x00000000000a0046ull, 0x000000000000004cull, 0x000000000100008cull, 0x000000000000004eull,
    0x0000790000006c00ull, 0x0000000000000018ull, 0x00df0000027f0000ull, 0x0000000000000040ull,
    0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull,
    0x5a5a5a5a5a5a5a5aull, 0x0000000000000045ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x0000000000000006ull, 0x0000000000000000ull,
    0x3f80000000000000ull, 0x0000000000000001ull, 0x0000000079006c00ull, 0x0000000000000005ull,
    0x0000000087009400ull, 0x0000000000000005ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x100000000000800eull, 0x000000000000000eull, 0x00000000000a0000ull, 0x000000000000004cull,
    0x000000000100008cull, 0x000000000000004eull, 0x0000790000006c00ull, 0x0000000000000018ull,
    0x00df0000027f0000ull, 0x0000000000000040ull, 0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull,
    0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull, 0x5a5a5a5a5a5a5a5aull, 0x0000000000000045ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x0000000000000006ull, 0x0000000000000000ull, 0x3f80000000000000ull, 0x0000000000000001ull,
    0x0000000079006c00ull, 0x0000000000000005ull, 0x0000000087009400ull, 0x0000000000000005ull,
    0x0000000000030000ull, 0x0000000000000047ull,
}},
// ct16_640x224_z32_ztest0_noclear_frame
{"ct16_640x224_z32_ztest0_noclear_frame", 2, 640, 224, 0, 0x30, 0, {1, 2, 1}, 0x5a, {
    0x0000000000000066ull, 0x0000000000000003ull, 0x0000000000011400ull, 0x001bf9ff0183227cull,
    0x0000000000000000ull, 0x0000000000000066ull, 0x0000000000000003ull, 0x0000000000011428ull,
    0x001bf9ff0183227cull, 0x0000000000000000ull, 0x1000000000008008ull, 0x000000000000000eull,
    0x00000000020a0028ull, 0x000000000000004cull, 0x0000000100000050ull, 0x000000000000004eull,
    0x0000790000006c00ull, 0x0000000000000018ull, 0x00df0000027f0000ull, 0x0000000000000040ull,
    0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull,
    0x5a5a5a5a5a5a5a5bull, 0x0000000000000045ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull,
    0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull,
    0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull,
    0x1000000000008008ull, 0x000000000000000eull, 0x00000000020a0000ull, 0x000000000000004cull,
    0x0000000100000050ull, 0x000000000000004eull, 0x0000790000006c00ull, 0x0000000000000018ull,
    0x00df0000027f0000ull, 0x0000000000000040ull, 0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull,
    0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000045ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull,
    0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull,
    0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull,
    0x5a5a5a5a5a5a5a5aull, 0x5a5a5a5a5a5a5a5aull,
}},
// ct32_512x448_z24_ztest0_clear_field
{"ct32_512x448_z24_ztest0_clear_field", 0, 512, 448, 0, 0x31, 1, {1, 2, 0}, 0x5a, {
    0x0000000000000066ull, 0x0000000000000001ull, 0x0000000000001000ull, 0x001bf9ff0203227cull,
    0x0000000000000000ull, 0x0000000000000066ull, 0x0000000000000001ull, 0x0000000000001000ull,
    0x001bf9ff0203227cull, 0x0000000000000000ull, 0x100000000000800eull, 0x000000000000000eull,
    0x0000000000080000ull, 0x000000000000004cull, 0x0000000101000070ull, 0x000000000000004eull,
    0x0000720000007000ull, 0x0000000000000018ull, 0x01bf000001ff0000ull, 0x0000000000000040ull,
    0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull,
    0x5a5a5a5a5a5a5a5aull, 0x0000000000000045ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x0000000000000006ull, 0x0000000000000000ull,
    0x3f80000000000000ull, 0x0000000000000001ull, 0x0000000072007000ull, 0x0000000000000005ull,
    0x000000008e009000ull, 0x0000000000000005ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x100000000000800eull, 0x000000000000000eull, 0x0000000000080000ull, 0x000000000000004cull,
    0x0000000101000070ull, 0x000000000000004eull, 0x0000720000007000ull, 0x0000000000000018ull,
    0x01bf000001ff0000ull, 0x0000000000000040ull, 0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull,
    0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull, 0x5a5a5a5a5a5a5a5aull, 0x0000000000000045ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x0000000000000006ull, 0x0000000000000000ull, 0x3f80000000000000ull, 0x0000000000000001ull,
    0x0000000072007000ull, 0x0000000000000005ull, 0x000000008e009000ull, 0x0000000000000005ull,
    0x0000000000030000ull, 0x0000000000000047ull,
}},
// ct16_512x448_z32_ztest1_clear_progressive
{"ct16_512x448_z32_ztest1_clear_progressive", 2, 512, 448, 1, 0x30, 1, {0, 2, 0}, 0x5a, {
    0x0000000000000066ull, 0x0000000000000002ull, 0x0000000000011000ull, 0x001bf9ff0201927cull,
    0x0000000000000000ull, 0x0000000000000066ull, 0x0000000000000002ull, 0x0000000000011038ull,
    0x001bf9ff0201927cull, 0x0000000000000000ull, 0x100000000000800eull, 0x000000000000000eull,
    0x0000000002080038ull, 0x000000000000004cull, 0x0000000000000070ull, 0x000000000000004eull,
    0x0000720000007000ull, 0x0000000000000018ull, 0x01bf000001ff0000ull, 0x0000000000000040ull,
    0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull,
    0x5a5a5a5a5a5a5a5bull, 0x0000000000000045ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x0000000000000006ull, 0x0000000000000000ull,
    0x3f80000000000000ull, 0x0000000000000001ull, 0x0000000072007000ull, 0x0000000000000005ull,
    0x000000008e009000ull, 0x0000000000000005ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x100000000000800eull, 0x000000000000000eull, 0x0000000002080000ull, 0x000000000000004cull,
    0x0000000000000070ull, 0x000000000000004eull, 0x0000720000007000ull, 0x0000000000000018ull,
    0x01bf000001ff0000ull, 0x0000000000000040ull, 0x5a5a5a5a5a5a5a5bull, 0x000000000000001aull,
    0x5a5a5a5a5a5a5a5bull, 0x0000000000000046ull, 0x5a5a5a5a5a5a5a5bull, 0x0000000000000045ull,
    0x0000000000030000ull, 0x0000000000000047ull, 0x0000000000030000ull, 0x0000000000000047ull,
    0x0000000000000006ull, 0x0000000000000000ull, 0x3f80000000000000ull, 0x0000000000000001ull,
    0x0000000072007000ull, 0x0000000000000005ull, 0x000000008e009000ull, 0x0000000000000005ull,
    0x0000000000030000ull, 0x0000000000000047ull,
}},
// ct16s_640x448_z32_ztest3_noclear_frame
{"ct16s_640x448_z32_ztest3_noclear_frame", 10, 640, 448, 3, 0x30, 0, {1, 2, 1}, 0x00, {
    0x0000000000000066ull, 0x0000000000000003ull, 0x0000000000051400ull, 0x0037f9ff0183227cull,
    0x0000000000000000ull, 0x0000000000000066ull, 0x0000000000000003ull, 0x0000000000051446ull,
    0x0037f9ff0183227cull, 0x0000000000000000ull, 0x1000000000008008ull, 0x000000000000000eull,
    0x000000000a0a0046ull, 0x000000000000004cull, 0x000000000000008cull, 0x000000000000004eull,
    0x0000720000006c00ull, 0x0000000000000018ull, 0x01bf0000027f0000ull, 0x0000000000000040ull,
    0x0000000000000001ull, 0x000000000000001aull, 0x0000000000000001ull, 0x0000000000000046ull,
    0x0000000000000001ull, 0x0000000000000045ull, 0x0000000000070000ull, 0x0000000000000047ull,
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    0x1000000000008008ull, 0x000000000000000eull, 0x000000000a0a0000ull, 0x000000000000004cull,
    0x000000000000008cull, 0x000000000000004eull, 0x0000720000006c00ull, 0x0000000000000018ull,
    0x01bf0000027f0000ull, 0x0000000000000040ull, 0x0000000000000001ull, 0x000000000000001aull,
    0x0000000000000001ull, 0x0000000000000046ull, 0x0000000000000001ull, 0x0000000000000045ull,
    0x0000000000070000ull, 0x0000000000000047ull, 0x0000000000000000ull, 0x0000000000000000ull,
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    0x0000000000000000ull, 0x0000000000000000ull,
}},
    };

    constexpr uint32_t kEnv = 0x00100000u;
    constexpr uint32_t kStack = 0x00080000u;
    constexpr uint32_t kDBuffSize = 0x230u;

    struct Harness
    {
        std::unique_ptr<PS2Runtime> runtime = std::make_unique<PS2Runtime>();
        uint8_t *rdram = nullptr;
        std::vector<std::vector<uint8_t>> packets;

        bool init(const uint32_t gparam[3])
        {
            if (!runtime->memory().initialize())
                return false;
            rdram = runtime->memory().getRDRAM();
            R5900Context ctx{};
            setReg(ctx, 4, 0u);
            setReg(ctx, 5, gparam[0]);
            setReg(ctx, 6, gparam[1]);
            setReg(ctx, 7, gparam[2]);
            ps2_stubs::sceGsResetGraph(rdram, &ctx, runtime.get());
            // Every GIF packet the runtime submits from here on is recorded, then drawn.
            PS2Runtime *rt = runtime.get();
            runtime->gifArbiter().setProcessPacketFn([this, rt](const uint8_t *data, uint32_t size)
                                                     {
                                                         packets.emplace_back(data, data + size);
                                                         rt->gs().processGIFPacket(data, size); });
            return true;
        }

        uint64_t q(uint32_t addr) const
        {
            uint64_t v = 0;
            std::memcpy(&v, rdram + addr, 8);
            return v;
        }
    };

    void runSwap(Harness &hx, const std::string &tag, uint32_t which, uint32_t giftagOffset)
    {
        hx.packets.clear();
        R5900Context ctx{};
        setReg(ctx, 4, kEnv);
        setReg(ctx, 5, which);
        ps2_stubs::sceGsSwapDBuff(hx.rdram, &ctx, hx.runtime.get());
        const uint32_t tagAddr = kEnv + giftagOffset;
        const uint32_t qwords = static_cast<uint32_t>(hx.q(tagAddr) & 0x7fffu) + 1u;
        check(hx.packets.size() == 1u, tag + ": one GIF packet (got " + std::to_string(hx.packets.size()) + ")");
        if (!hx.packets.empty())
        {
            const auto &p = hx.packets.front();
            check(p.size() == qwords * 16u, tag + ": packet is giftag + NLOOP qwords (" + std::to_string(qwords) +
                                                 " qw, got " + std::to_string(p.size() / 16u) + ")");
            check(p.size() == qwords * 16u && std::memcmp(p.data(), hx.rdram + tagAddr, p.size()) == 0,
                  tag + ": packet bytes are the guest's bytes");
        }
        const uint32_t disp = kEnv + which * 0x28u;
        const auto &regs = hx.runtime->memory().gs();
        check(regs.pmode == hx.q(disp + 0x00) && regs.smode2 == hx.q(disp + 0x08) &&
                  regs.dispfb2 == hx.q(disp + 0x10) && regs.display2 == hx.q(disp + 0x18) &&
                  regs.bgcolor == hx.q(disp + 0x20),
              tag + ": disp[" + std::to_string(which) + "] applied");
        check(static_cast<uint32_t>(getRegU32(&ctx, 2)) == (which ^ 1u), tag + ": returns id ^ 1");
    }

    void runCase(const Case &c)
    {
        const std::string name = c.name;
        Harness hx;
        if (!hx.init(c.gparam))
        {
            check(false, name + ": runtime init");
            return;
        }

        std::memset(hx.rdram + kEnv - 16u, 0xEE, 16u);
        std::memset(hx.rdram + kEnv, c.fill, kDBuffSize);
        std::memset(hx.rdram + kEnv + kDBuffSize, 0xEE, 16u);
        std::memset(hx.rdram + kStack, 0x77, 64u); // junk where a stack-reading stub would look

        R5900Context ctx{};
        setReg(ctx, 4, kEnv);
        setReg(ctx, 5, c.psm);
        setReg(ctx, 6, c.w);
        setReg(ctx, 7, c.h);
        setReg(ctx, 8, c.ztest);
        setReg(ctx, 9, c.zpsm);
        setReg(ctx, 10, c.clear);
        setReg(ctx, 29, kStack);
        ps2_stubs::sceGsSetDefDBuff(hx.rdram, &ctx, hx.runtime.get());

        for (uint32_t i = 0; i < kDBuffSize / 8u; ++i)
        {
            const uint64_t got = hx.q(kEnv + i * 8u);
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s: +0x%03x expected 0x%016llx got 0x%016llx", c.name, i * 8u,
                          static_cast<unsigned long long>(c.q[i]), static_cast<unsigned long long>(got));
            check(got == c.q[i], buf);
        }
        bool guards = true;
        for (uint32_t i = 0; i < 16u; ++i)
            guards = guards && hx.rdram[kEnv - 16u + i] == 0xEE && hx.rdram[kEnv + kDBuffSize + i] == 0xEE;
        check(guards, name + ": nothing written outside the 0x230-byte sceGsDBuff");

        runSwap(hx, name + " swap 0", 0u, 0x50u);
        runSwap(hx, name + " swap 1", 1u, 0x140u);

        // The guest's giftag decides what is sent: set NLOOP 8 by hand (drops a seeded clear).
        uint64_t lo = hx.q(kEnv + 0x140u);
        lo = (lo & ~0x7fffull) | 8u;
        std::memcpy(hx.rdram + kEnv + 0x140u, &lo, 8);
        runSwap(hx, name + " swap 1 (NLOOP 8 by hand)", 1u, 0x140u);
    }

    void runPutDrawEnv()
    {
        const uint32_t gparam[3] = {1u, 2u, 1u};
        Harness hx;
        if (!hx.init(gparam))
        {
            check(false, "sceGsPutDrawEnv: runtime init");
            return;
        }
        constexpr uint32_t kTag = 0x00110000u;
        const uint64_t pkt[8] = {
            0x1000000000008003ull, 0xEull,  // giftag: NLOOP 3, EOP, NREG 1, A+D
            0x0000000000000001ull, 0x1Aull, // PRMODECONT 1
            0x0000000000000001ull, 0x46ull, // COLCLAMP 1
            0x0000000000030000ull, 0x47ull, // TEST_1
        };
        std::memcpy(hx.rdram + kTag, pkt, sizeof(pkt));
        std::memset(hx.rdram + kTag + sizeof(pkt), 0xCC, 16u); // must not be sent
        hx.packets.clear();
        R5900Context ctx{};
        setReg(ctx, 4, kTag);
        ps2_stubs::sceGsPutDrawEnv(hx.rdram, &ctx, hx.runtime.get());
        check(hx.packets.size() == 1u && hx.packets.front().size() == sizeof(pkt) &&
                  std::memcmp(hx.packets.front().data(), pkt, sizeof(pkt)) == 0,
              "sceGsPutDrawEnv: sends the giftag and its 3 qwords, nothing more");
        check(getRegU32(&ctx, 2) == 0u, "sceGsPutDrawEnv: returns 0");
    }
}

int main()
{
    for (const Case &c : kCases)
        runCase(c);
    runPutDrawEnv();
    std::printf("gs_dbuff: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
