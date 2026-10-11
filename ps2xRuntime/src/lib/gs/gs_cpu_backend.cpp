#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_raster_pool.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
// SSE4.1 for the per-pixel arithmetic (x86-64 builds assume it: Intel since Penryn 2008, AMD since
// Bulldozer 2011); same results as the scalar code.
#define PS2X_RASTER_SSE 1
#include <smmintrin.h>
#else
#define PS2X_RASTER_SSE 0
#endif
#include <chrono>
#include <condition_variable>
#include <thread>
#include <type_traits>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <tuple>
#include <iostream>
#include <string>
#include <stdexcept>

using namespace GSInternal;

// The per-pixel helpers must be inlined into the span loops (MSVC leaves the bigger ones as
// calls at /O2, which costs a call with by-reference arguments per pixel).
#if defined(_MSC_VER)
#define PS2X_RASTER_INLINE __forceinline
#else
#define PS2X_RASTER_INLINE inline __attribute__((always_inline))
#endif

namespace
{
    GSPageRange pageRangeFor(uint8_t psm, uint32_t bp, uint32_t bw, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);

    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    PS2X_RASTER_INLINE u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    PS2X_RASTER_INLINE u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    PS2X_RASTER_INLINE u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    PS2X_RASTER_INLINE uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    PS2X_RASTER_INLINE int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    PS2X_RASTER_INLINE TextureCombineResult combineTexture(uint8_t tfx,
                                        bool textureHasAlpha,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    // PS2X_GS_DUMP=1500,1560 saves GS local memory (4 MB, native layout) at the first
    // presented frame at or after each VBlank tick, with the display registers and the
    // presented picture: gsdump_<tick>.bin / .txt / .rgba in PS2X_GS_DUMP_DIR (default:
    // the working directory). Read them with tools/gsview.py.
    void maybeDumpGsMemory(const GSPresentationRequest &request,
                           const std::vector<uint8_t> &vram,
                           const PresentationFrame &frame)
    {
        static std::vector<uint64_t> s_ticks = []
        {
            std::vector<uint64_t> ticks;
            if (const char *value = std::getenv("PS2X_GS_DUMP"))
            {
                std::string list(value);
                size_t pos = 0;
                while (pos < list.size())
                {
                    const size_t comma = list.find(',', pos);
                    const std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    if (!item.empty())
                        ticks.push_back(std::strtoull(item.c_str(), nullptr, 0));
                    if (comma == std::string::npos)
                        break;
                    pos = comma + 1;
                }
                std::sort(ticks.begin(), ticks.end());
            }
            return ticks;
        }();
        static size_t s_next = 0;
        if (s_next >= s_ticks.size() || request.vsyncTick < s_ticks[s_next])
            return;
        while (s_next < s_ticks.size() && s_ticks[s_next] <= request.vsyncTick)
            ++s_next;

        const char *dir = std::getenv("PS2X_GS_DUMP_DIR");
        const std::string base = std::string(dir && dir[0] ? dir : ".") + "/gsdump_" + std::to_string(request.vsyncTick);
        std::ofstream bin(base + ".bin", std::ios::binary | std::ios::trunc);
        bin.write(reinterpret_cast<const char *>(vram.data()), static_cast<std::streamsize>(vram.size()));
        std::ofstream txt(base + ".txt", std::ios::trunc);
        txt << std::hex << "tick=" << std::dec << request.vsyncTick << std::hex
            << "\npmode=0x" << request.pmode << "\nsmode2=0x" << request.smode2
            << "\ndispfb1=0x" << request.dispfb1 << "\ndisplay1=0x" << request.display1
            << "\ndispfb2=0x" << request.dispfb2 << "\ndisplay2=0x" << request.display2
            << "\nbgcolor=0x" << request.bgcolor << std::dec;
        for (int i = 0; i < 2; ++i)
            txt << "\nctx" << i << "frame=fbp:" << request.contextFrames[i].fbp << " fbw:" << request.contextFrames[i].fbw
                << " psm:" << static_cast<uint32_t>(request.contextFrames[i].psm);
        txt << "\npresented=" << frame.width << "x" << frame.height << " rowstride=" << kHostFrameWidth
            << " displayFbp=" << frame.displayFbp << " sourceFbp=" << frame.sourceFbp
            << " usedPreferred=" << frame.usedPreferred << "\n";
        if (!frame.pixels.empty())
        {
            std::ofstream rgba(base + ".rgba", std::ios::binary | std::ios::trunc);
            rgba.write(reinterpret_cast<const char *>(frame.pixels.data()), static_cast<std::streamsize>(frame.pixels.size()));
        }
        std::cerr << "[gs:dump] " << base << ".bin tick=" << request.vsyncTick << std::endl;
    }

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    // What one read circuit shows, worked out the way the GS CRTC does it (GS User's Manual,
    // DISPLAY1/2 and SMODE2; PCSX2's GSState::GetDisplayRect does the same):
    //   - DW and DH are in CRTC units: DW counts VCK cycles (MAGH + 1 per pixel), DH counts
    //     raster lines of the output. Interlaced, those are frame lines (448 for a full NTSC
    //     picture); non-interlaced, lines of the progressive raster.
    //   - The circuit reads (DH + 1) / (MAGV + 1) buffer lines, except in interlaced FRAME mode
    //     (SMODE2 INT = 1, FFMD = 1), where each field reads every line of the buffer, so each
    //     field shows half of the frame lines: (DH + 1) / (MAGV + 1) / 2 buffer lines, and each
    //     buffer line covers two frame lines. libgraph sets DH = 2h - 1 in that mode
    //     (sceGsSetDefDispEnv, SDK 3.1.0 copy at 0x10c738), so an h-line buffer fills the screen.
    //   - Interlaced FIELD mode (FFMD = 0) reads every other buffer line per field: the buffer
    //     has as many lines as the frame (the presenter shows one field per VBlank, see
    //     applyFieldPresentation).
    // sourceRows = buffer lines read; outputRows = lines presented (frame lines when interlaced).
    struct GSDisplayGeometry
    {
        uint32_t width = 0u;
        uint32_t sourceRows = 0u;
        uint32_t outputRows = 0u;
    };

    GSDisplayGeometry decodeDisplayGeometry(uint64_t display64, uint64_t smode2)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        const uint32_t magv = static_cast<uint32_t>((display64 >> 27) & 0x03u);
        const bool interlaced = (smode2 & 0x1ull) != 0ull;
        const bool frameMode = ((smode2 >> 1) & 0x1ull) != 0ull;

        GSDisplayGeometry g{};
        g.width = (dw + 1u) / (magh + 1u);
        g.outputRows = dh + 1u;
        g.sourceRows = (dh + 1u) / (magv + 1u);
        if (interlaced && frameMode)
            g.sourceRows /= 2u;
        if (g.width < 64u || g.outputRows < 64u || g.sourceRows == 0u)
        {
            g.width = kDefaultDisplayWidth;
            g.outputRows = kDefaultDisplayHeight;
            g.sourceRows = kDefaultDisplayHeight;
        }
        g.width = std::min<uint32_t>(g.width, kHostFrameWidth);
        if (g.outputRows > kHostFrameHeight)
        {
            // Crop, don't squash: keep the source:output line ratio for the lines that fit
            // (e.g. 640x448 in frame mode, DH 895: the first 256 buffer lines, each twice).
            g.sourceRows = std::max<uint32_t>(1u, static_cast<uint32_t>(
                (static_cast<uint64_t>(kHostFrameHeight) * g.sourceRows) / g.outputRows));
            g.outputRows = kHostFrameHeight;
        }
        g.sourceRows = std::min<uint32_t>(g.sourceRows, g.outputRows);
        return g;
    }

    // Stretches the first sourceRows lines of a host frame (kHostFrameWidth row stride) over
    // outputRows lines, as the CRTC repeats each buffer line (MAGV, frame mode). In place:
    // output line y reads source line y * sourceRows / outputRows <= y, so going bottom-up
    // never reads a line already overwritten.
    void expandDisplayRows(std::vector<uint8_t> &pixels, uint32_t width, uint32_t sourceRows, uint32_t outputRows)
    {
        if (pixels.empty() || sourceRows == 0u || outputRows <= sourceRows)
            return;
        for (uint32_t y = outputRows; y-- > 0u;)
        {
            const uint32_t sourceY = static_cast<uint32_t>((static_cast<uint64_t>(y) * sourceRows) / outputRows);
            if (sourceY != y)
                std::memmove(pixels.data() + y * kHostFrameWidth * 4u,
                             pixels.data() + sourceY * kHostFrameWidth * 4u,
                             width * 4u);
        }
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
                sourceY = height - 1u;
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    ++m_clutGeneration;
    m_paletteKey = ~0ull;
    m_decodedTextures.clear();
    StampAllPages();
    m_texturePageCache.Invalidate();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    DrawPrimitive(batch);
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;

    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    LoadClutUnlocked(tex0, texclut);
}

void GSCpuBackend::LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;
    ++m_clutGeneration;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = ReadTextureVramUnlocked(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY); 
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            m_clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            m_clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
}

void GSCpuBackend::Flush()
{
    // CPU backend is immediate. GPU backends may submit command buffers here.
}

void GSCpuBackend::TextureFlush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_texturePageCache.Invalidate();
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!m_vram)
        return 0u;

    return GSMem::ReadTexture(m_texturePageCache, m_vram, psm, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    StampAllPages();
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

// ---------------------------------------------------------------------------------------------
// Rasteriser.
//
// GS rules (GS User's Manual, "Drawing"; PCSX2's software rasteriser agrees):
//  * vertex X/Y are 12.4 fixed point; the window position is XYZ - XYOFFSET, fraction kept;
//  * pixel (x, y) is sampled at the point (x.0, y.0): a triangle draws it when that point lies
//    inside; a point exactly on an edge belongs to the triangle only for a top or left edge, so
//    triangles sharing an edge (strips, fans, quads) cover each pixel exactly once; a sprite
//    covers x in [ceil(x0), ceil(x1)) and y in [ceil(y0), ceil(y1));
//  * every attribute is interpolated linearly to the sampling point: Z, colour, fog and U/V are
//    integers at the vertices, and the value at a pixel is floor of the exact rational
//    (a0 * area + B1 * (a1 - a0) + B2 * (a2 - a0)) / area, stepped across a row as an integer
//    quotient and remainder (an exact DDA: an attribute equal at all vertices is that value at
//    every pixel, which the GEQUAL depth test of SRWZ's title logo relies on);
//  * S/T/Q are interpolated the same way in double and divided per pixel.
//
// Speed: everything that depends only on the draw state (frame and Z buffer layout, tests,
// blend, texture format, wrap, filter, the CLUT decoded to RGBA with TEXA applied) is worked
// out once per primitive in a GSRasterSetup; the span loops are specialised on shading, texture
// and fog mode; frame and Z addresses come from one page-table row per scan line; textures are
// read from a decoded copy (indices or RGBA, linear layout) kept across draws and invalidated by
// page when local memory is written; bilinear filtering is integer.
// ---------------------------------------------------------------------------------------------

namespace
{
    using GSMem::PixelStorageMode;
    using GSMem::PixelStorageTraits;

    template <PixelStorageMode PSM>
    const typename PixelStorageTraits<PSM>::PageLookupTableT &pageTableFor()
    {
        if constexpr (PSM == GSMem::C32 || PSM == GSMem::C24 || PSM == GSMem::P8H || PSM == GSMem::P4HL || PSM == GSMem::P4HH)
            return GSMem::PageTableC32;
        else if constexpr (PSM == GSMem::Z32 || PSM == GSMem::Z24)
            return GSMem::PageTableZ32;
        else if constexpr (PSM == GSMem::C16)
            return GSMem::PageTableC16;
        else if constexpr (PSM == GSMem::C16S)
            return GSMem::PageTableC16S;
        else if constexpr (PSM == GSMem::Z16)
            return GSMem::PageTableZ16;
        else if constexpr (PSM == GSMem::Z16S)
            return GSMem::PageTableZ16S;
        else if constexpr (PSM == GSMem::P8)
            return GSMem::PageTableP8;
        else
            return GSMem::PageTableP4;
    }

    constexpr uint32_t log2Pow2(uint32_t v)
    {
        uint32_t n = 0;
        while ((1u << n) < v)
            ++n;
        return n;
    }

    // Page extent of a format in pixels (GS User's Manual, local memory layout).
    void pageExtentFor(uint8_t psm, uint32_t &pageW, uint32_t &pageH)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            pageW = 64u;
            pageH = 64u;
            break;
        case GS_PSM_T8:
            pageW = 128u;
            pageH = 64u;
            break;
        case GS_PSM_T4:
            pageW = 128u;
            pageH = 128u;
            break;
        default:
            pageW = 64u;
            pageH = 32u;
            break;
        }
    }

    // The 8 KB pages that the rectangle [x0, x1] x [y0, y1] of a buffer at block bp, width bw
    // (units of 64 pixels) touches (the page tables keep a buffer that starts inside a page
    // within that page, so the last corner's page is the last page). Addresses wrap at 4 MB
    // (512 pages): the range is first + count pages, modulo 512; count 512 is all of memory.
    GSPageRange pageRangeFor(uint8_t psm, uint32_t bp, uint32_t bw, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
    {
        uint32_t pageW, pageH;
        pageExtentFor(psm, pageW, pageH);
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, (std::max<uint32_t>(bw, 1u) * 64u) / pageW);
        const uint32_t base = bp / 32u;
        const uint32_t first = base + (y0 / pageH) * pagesPerRow + x0 / pageW;
        const uint32_t last = base + (y1 / pageH) * pagesPerRow + x1 / pageW;
        GSPageRange range;
        range.first = first % GSPageRange::kPages;
        range.count = std::min<uint32_t>(GSPageRange::kPages, last >= first ? last - first + 1u : 0u);
        return range;
    }

    // Whether two (wrapping) page ranges share a page.
    bool pageRangesOverlap(const GSPageRange &a, const GSPageRange &b)
    {
        if (a.count == 0u || b.count == 0u)
            return false;
        if (a.count >= GSPageRange::kPages || b.count >= GSPageRange::kPages)
            return true;
        // Distance from a.first to b.first going up, modulo 512: b starts inside a, or a inside b.
        const uint32_t ab = (b.first + GSPageRange::kPages - a.first) % GSPageRange::kPages;
        const uint32_t ba = (a.first + GSPageRange::kPages - b.first) % GSPageRange::kPages;
        return ab < a.count || ba < b.count;
    }

    // One scan line of a buffer: the page-table row for (block, y) and the page the row starts
    // in, so that a pixel's byte address is one shift, one mask, one table load and a multiply.
    struct RowAddr
    {
        const uint16_t *table = nullptr;
        uint32_t page = 0;
        uint32_t pageShift = 6;
        uint32_t pageMask = 63;
        uint32_t pixelsPerPage = 2048;
        uint32_t stride = 4;
        uint32_t wrapMask = 0;

        PS2X_RASTER_INLINE uint32_t at(uint32_t x) const
        {
            return (((page + (x >> pageShift)) * pixelsPerPage + table[x & pageMask]) * stride) & wrapMask;
        }
    };

    template <PixelStorageMode PSM>
    void rowSetup(RowAddr &row, uint32_t bp, uint32_t bw, uint32_t y)
    {
        using Tr = PixelStorageTraits<PSM>;
        constexpr auto extent = Tr::PageExtent();
        constexpr uint32_t pageW = extent.x, pageH = extent.y;
        const auto &table = pageTableFor<PSM>();
        row.table = &table[bp % Tr::BlocksPerPage()][y % pageH][0];
        row.page = bp / static_cast<uint32_t>(Tr::BlocksPerPage()) + (y / pageH) * ((bw * 64u) / pageW);
        row.pageShift = log2Pow2(pageW);
        row.pageMask = pageW - 1u;
        row.pixelsPerPage = static_cast<uint32_t>(Tr::PixelsPerPage());
        row.stride = GSMem::UnpackedBitWidth(PSM) / 8u;
        row.wrapMask = static_cast<uint32_t>(GSMem::MEMORY_SIZE - sizeof(typename Tr::PackedT));
    }

    using RowSetupFn = void (*)(RowAddr &, uint32_t, uint32_t, uint32_t);

    enum class PixelStore : uint8_t
    {
        Word32, // whole 32-bit word
        Word24, // low 24 bits, the high byte kept
        Half16  // 16 bits
    };

    PS2X_RASTER_INLINE uint32_t loadPixel(const uint8_t *vram, PixelStore store, uint32_t address)
    {
        if (store == PixelStore::Half16)
        {
            uint16_t v;
            std::memcpy(&v, vram + address, sizeof(v));
            return v;
        }
        uint32_t v;
        std::memcpy(&v, vram + address, sizeof(v));
        return store == PixelStore::Word24 ? (v & 0x00FFFFFFu) : v;
    }

    PS2X_RASTER_INLINE void storePixel(uint8_t *vram, PixelStore store, uint32_t address, uint32_t value)
    {
        if (store == PixelStore::Half16)
        {
            const uint16_t v = static_cast<uint16_t>(value);
            std::memcpy(vram + address, &v, sizeof(v));
            return;
        }
        if (store == PixelStore::Word24)
        {
            uint32_t old;
            std::memcpy(&old, vram + address, sizeof(old));
            value = (old & 0xFF000000u) | (value & 0x00FFFFFFu);
        }
        std::memcpy(vram + address, &value, sizeof(value));
    }

    bool pixelStoreFor(uint8_t psm, RowSetupFn &fn, PixelStore &store)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
            fn = rowSetup<GSMem::C32>;
            store = PixelStore::Word32;
            return true;
        case GS_PSM_CT24:
            fn = rowSetup<GSMem::C24>;
            store = PixelStore::Word24;
            return true;
        case GS_PSM_CT16:
            fn = rowSetup<GSMem::C16>;
            store = PixelStore::Half16;
            return true;
        case GS_PSM_CT16S:
            fn = rowSetup<GSMem::C16S>;
            store = PixelStore::Half16;
            return true;
        case GS_PSM_Z32:
            fn = rowSetup<GSMem::Z32>;
            store = PixelStore::Word32;
            return true;
        case GS_PSM_Z24:
            fn = rowSetup<GSMem::Z24>;
            store = PixelStore::Word24;
            return true;
        case GS_PSM_Z16:
            fn = rowSetup<GSMem::Z16>;
            store = PixelStore::Half16;
            return true;
        case GS_PSM_Z16S:
            fn = rowSetup<GSMem::Z16S>;
            store = PixelStore::Half16;
            return true;
        default:
            return false;
        }
    }

    // PS2X_GS_STATS=1: the rasteriser's work, once per host second on stdout and at exit:
    // primitives by kind, rows (spans) shaded, bounding-box / covered / textured pixels, texture
    // decodes, primitives shaded by the worker pool.
    struct GsRasterStats
    {
        std::atomic<uint64_t> triangles{0}, sprites{0}, lines{0}, points{0};
        std::atomic<uint64_t> rows{0}, bboxPixels{0}, coveredPixels{0}, texturedPixels{0}, linearPixels{0};
        std::atomic<uint64_t> bigPrimitives{0}; // bounding box >= 4096 pixels
        std::atomic<uint64_t> textureDecodes{0}, textureDecodedTexels{0}, directTextureDraws{0}, parallelPrimitives{0};
    };
    GsRasterStats s_rasterStats;
    inline void statAdd(std::atomic<uint64_t> &counter, uint64_t n = 1u)
    {
        counter.fetch_add(n, std::memory_order_relaxed);
    }
    bool gsStatsEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_GS_STATS");
            return value && value[0] == '1';
        }();
        return enabled;
    }
    void gsStatsPrint(const char *when, double seconds)
    {
        GsRasterStats &st = s_rasterStats;
        auto take = [](std::atomic<uint64_t> &v)
        { return static_cast<unsigned long long>(v.exchange(0, std::memory_order_relaxed)); };
        std::printf("[gs:stats] %s %.2fs: tris=%llu sprites=%llu lines=%llu points=%llu big=%llu parallel=%llu rows=%llu bboxPx=%llu "
                    "coveredPx=%llu texPx=%llu linearPx=%llu texDecodes=%llu texDecodedTexels=%llu directTexDraws=%llu\n",
                    when, seconds, take(st.triangles), take(st.sprites), take(st.lines), take(st.points), take(st.bigPrimitives),
                    take(st.parallelPrimitives), take(st.rows), take(st.bboxPixels), take(st.coveredPixels), take(st.texturedPixels),
                    take(st.linearPixels), take(st.textureDecodes), take(st.textureDecodedTexels), take(st.directTextureDraws));
        std::fflush(stdout);
    }
    struct GsStatsReporter
    {
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        void maybeReport()
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(1))
                return;
            gsStatsPrint("per", std::chrono::duration<double>(now - last).count());
            last = now;
        }
        ~GsStatsReporter()
        {
            if (gsStatsEnabled())
                gsStatsPrint("exit", std::chrono::duration<double>(std::chrono::steady_clock::now() - last).count());
        }
    };
    GsStatsReporter s_statsReporter;

    // Raster workers: runtime/gs/gs_raster_pool.h (chunked rows, completion counted in chunks).
    using RasterPool = ps2x::gs::RasterPool;

    // Rows [y0, y1] of a primitive with a bounding box of boxPixels: fn(yStart, yEnd) over
    // chunks, on the pool when the primitive is big enough to pay for the hand-off. A
    // primitive that samples the pages it draws into (serial) runs top to bottom on the
    // calling thread: its rows read what earlier rows wrote, so splitting them would make the
    // result depend on the thread count.
    template <typename Fn>
    void forEachRowChunk(int y0, int y1, int64_t boxPixels, bool serial, Fn &&fn)
    {
        if (y1 < y0)
            return;
        RasterPool &pool = RasterPool::instance();
        const int threads = pool.threads();
        if (!serial && threads > 1 && boxPixels >= 4096 && (y1 - y0) >= 8)
        {
            if (gsStatsEnabled())
                statAdd(s_rasterStats.parallelPrimitives);
            using F = std::remove_reference_t<Fn>;
            // About four chunks per thread, so the threads finish together.
            const int rowsPerChunk = std::max(2, (y1 - y0 + 1) / (threads * 4));
            pool.run(y0, y1, rowsPerChunk, [](void *context, int ys, int ye)
                     { (*static_cast<F *>(context))(ys, ye); },
                     &fn);
            return;
        }
        fn(y0, y1);
    }
}

// Texture state of one draw. A texel is read from the decoded copy when there is one
// (indices for CLUT formats, RGBA with TEXA applied otherwise), else straight from local
// memory through fetch(); (u, v) are wrapped already.
struct GSTexSetup
{
    bool enabled = false;
    bool linear = false;
    bool fst = false;
    uint8_t psm = 0;
    uint8_t tfx = 0;
    bool tcc = false;
    int w = 1;
    int h = 1;
    uint32_t tbp0 = 0;
    uint32_t tbw = 1;
    uint8_t wrapU = 0;
    uint8_t wrapV = 0;
    int minU = 0;
    int maxU = 0;
    int minV = 0;
    int maxV = 0;
    GSTexaReg texa{};
    const uint32_t *palette = nullptr;
    const uint8_t *vram = nullptr;
    const uint8_t *decodedIndices = nullptr;
    const uint32_t *decodedRgba = nullptr;
    bool selfSampling = false; // the texture shares pages with the draw's frame or Z buffer
    uint32_t (*fetch)(const GSTexSetup &, int u, int v) = nullptr;
};

// Everything the per-pixel code needs for one primitive.
struct GSRasterSetup
{
    uint8_t *vram = nullptr;
    int scissorX0 = 0, scissorX1 = 0, scissorY0 = 0, scissorY1 = 0;

    // Frame buffer.
    uint32_t fbp = 0; // block
    uint32_t fbw = 1;
    uint8_t fpsm = 0;
    uint32_t fbmsk = 0;
    bool fba = false;
    bool frame16 = false;
    RowSetupFn frameRow = nullptr;
    PixelStore frameStore = PixelStore::Word32;

    // Z buffer.
    uint32_t zbp = 0; // block
    bool zmsk = false;
    uint8_t ztst = 1;
    RowSetupFn zRow = nullptr;
    PixelStore zStore = PixelStore::Word32;

    // Tests.
    bool ate = false;
    uint8_t atst = 1;
    uint8_t aref = 0;
    uint8_t afail = 0;
    bool date = false;
    bool datm = false;
    bool dateReadsFrame = false;

    // Blend and fog.
    bool abe = false;
    bool pabe = false;
    uint8_t asel = 0, bsel = 0, csel = 0, dsel = 0, fix = 0;
    bool fge = false;
    uint8_t fogR = 0, fogG = 0, fogB = 0;

    GSTexSetup tex;
};

namespace
{
    // ---- texel fetch from local memory -----------------------------------------------------------

    template <PixelStorageMode PSM>
    uint32_t fetchTexel(const GSTexSetup &t, int u, int v)
    {
        using Tr = PixelStorageTraits<PSM>;
        const uint32_t raw = static_cast<uint32_t>(Tr::Read(pageTableFor<PSM>(), t.vram, t.tbp0, t.tbw,
                                                             static_cast<uint32_t>(u), static_cast<uint32_t>(v), nullptr));
        if constexpr (PSM == GSMem::C32 || PSM == GSMem::Z32 || PSM == GSMem::C24 || PSM == GSMem::Z24)
            return applyTexa(t.texa, t.psm, raw);
        else if constexpr (PSM == GSMem::C16 || PSM == GSMem::C16S || PSM == GSMem::Z16 || PSM == GSMem::Z16S)
            return applyTexa(t.texa, t.psm, Rgba5551ToRgba8888(static_cast<u16>(raw)));
        else
            return t.palette[raw & 0xFFu];
    }

    // The raw index of a CLUT-format texel.
    template <PixelStorageMode PSM>
    uint8_t fetchIndex(const GSTexSetup &t, int u, int v)
    {
        using Tr = PixelStorageTraits<PSM>;
        return static_cast<uint8_t>(Tr::Read(pageTableFor<PSM>(), t.vram, t.tbp0, t.tbw,
                                             static_cast<uint32_t>(u), static_cast<uint32_t>(v), nullptr));
    }

    bool texelFetchFor(uint8_t psm, uint32_t (*&fn)(const GSTexSetup &, int, int), uint8_t (*&indexFn)(const GSTexSetup &, int, int))
    {
        indexFn = nullptr;
        switch (psm)
        {
        case GS_PSM_CT32:
            fn = fetchTexel<GSMem::C32>;
            return true;
        case GS_PSM_CT24:
            fn = fetchTexel<GSMem::C24>;
            return true;
        case GS_PSM_CT16:
            fn = fetchTexel<GSMem::C16>;
            return true;
        case GS_PSM_CT16S:
            fn = fetchTexel<GSMem::C16S>;
            return true;
        case GS_PSM_Z32:
            fn = fetchTexel<GSMem::Z32>;
            return true;
        case GS_PSM_Z24:
            fn = fetchTexel<GSMem::Z24>;
            return true;
        case GS_PSM_Z16:
            fn = fetchTexel<GSMem::Z16>;
            return true;
        case GS_PSM_Z16S:
            fn = fetchTexel<GSMem::Z16S>;
            return true;
        case GS_PSM_T8:
            fn = fetchTexel<GSMem::P8>;
            indexFn = fetchIndex<GSMem::P8>;
            return true;
        case GS_PSM_T8H:
            fn = fetchTexel<GSMem::P8H>;
            indexFn = fetchIndex<GSMem::P8H>;
            return true;
        case GS_PSM_T4:
            fn = fetchTexel<GSMem::P4>;
            indexFn = fetchIndex<GSMem::P4>;
            return true;
        case GS_PSM_T4HL:
            fn = fetchTexel<GSMem::P4HL>;
            indexFn = fetchIndex<GSMem::P4HL>;
            return true;
        case GS_PSM_T4HH:
            fn = fetchTexel<GSMem::P4HH>;
            indexFn = fetchIndex<GSMem::P4HH>;
            return true;
        default:
            return false;
        }
    }

    uint32_t fetchTexelUnsupported(const GSTexSetup &, int, int)
    {
        return 0xFFFF00FFu;
    }

    // ---- sampling: texel coordinates in 1/256 texel ---------------------------------------------

    PS2X_RASTER_INLINE int wrapU(const GSTexSetup &t, int u)
    {
        return wrapTextureCoordinate(u, t.w, t.wrapU, static_cast<uint16_t>(t.minU), static_cast<uint16_t>(t.maxU));
    }

    PS2X_RASTER_INLINE int wrapV(const GSTexSetup &t, int v)
    {
        return wrapTextureCoordinate(v, t.h, t.wrapV, static_cast<uint16_t>(t.minV), static_cast<uint16_t>(t.maxV));
    }

    PS2X_RASTER_INLINE uint32_t texelAt(const GSTexSetup &t, int u, int v)
    {
        if (t.decodedIndices)
            return t.palette[t.decodedIndices[static_cast<size_t>(v) * static_cast<size_t>(t.w) + static_cast<size_t>(u)]];
        if (t.decodedRgba)
            return t.decodedRgba[static_cast<size_t>(v) * static_cast<size_t>(t.w) + static_cast<size_t>(u)];
        return t.fetch(t, u, v);
    }

    PS2X_RASTER_INLINE uint32_t sampleNearest(const GSTexSetup &t, int u256, int v256)
    {
        return texelAt(t, wrapU(t, u256 >> 8), wrapV(t, v256 >> 8));
    }

    // Bilinear: the texels around (u - 0.5, v - 0.5), weighted by the 8-bit fractions
    // (exact for FST's 1/16 texel coordinates), channels rounded half up.
    PS2X_RASTER_INLINE uint32_t sampleBilinear(const GSTexSetup &t, int u256, int v256)
    {
        const int su = u256 - 128;
        const int sv = v256 - 128;
        const int u0 = su >> 8;
        const int v0 = sv >> 8;
        const uint32_t fx = static_cast<uint32_t>(su & 255);
        const uint32_t fy = static_cast<uint32_t>(sv & 255);
        const int ua = wrapU(t, u0), ub = wrapU(t, u0 + 1);
        const int va = wrapV(t, v0), vb = wrapV(t, v0 + 1);
        const uint32_t c00 = texelAt(t, ua, va);
        const uint32_t c10 = texelAt(t, ub, va);
        const uint32_t c01 = texelAt(t, ua, vb);
        const uint32_t c11 = texelAt(t, ub, vb);
#if PS2X_RASTER_SSE
        // top = c00 (256 - fx) + c10 fx, bottom likewise, result = (top (256 - fy) + bottom fy + 2^15) >> 16
        // per channel: the same sum of four products as the scalar code.
        const __m128i zero = _mm_setzero_si128();
        const __m128i t00 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(c00)), zero); // 16-bit R G B A
        const __m128i t10 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(c10)), zero);
        const __m128i t01 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(c01)), zero);
        const __m128i t11 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(c11)), zero);
        const __m128i wx = _mm_set1_epi32(static_cast<int>((256u - fx) | (fx << 16)));           // [256-fx, fx] per channel
        const __m128i top = _mm_madd_epi16(_mm_unpacklo_epi16(t00, t10), wx);                     // 32-bit per channel
        const __m128i bottom = _mm_madd_epi16(_mm_unpacklo_epi16(t01, t11), wx);
        const __m128i sum = _mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(top, _mm_set1_epi32(static_cast<int>(256u - fy))),
                                                        _mm_mullo_epi32(bottom, _mm_set1_epi32(static_cast<int>(fy)))),
                                          _mm_set1_epi32(0x8000));
        const __m128i result = _mm_srli_epi32(sum, 16);
        return static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(result, zero), zero)));
#else
        const uint64_t w00 = (256u - fx) * (256u - fy);
        const uint64_t w10 = fx * (256u - fy);
        const uint64_t w01 = (256u - fx) * fy;
        const uint64_t w11 = fx * fy;
        // Two channels per 64-bit lane pair (R and B, then G and A): each product is below 2^24.
        auto lanes = [](uint32_t c, unsigned shift) -> uint64_t
        { return static_cast<uint64_t>((c >> shift) & 0xFFu) | (static_cast<uint64_t>((c >> (shift + 16u)) & 0xFFu) << 32u); };
        constexpr uint64_t kHalf = 0x8000ull | (0x8000ull << 32u);
        const uint64_t rb = (lanes(c00, 0) * w00 + lanes(c10, 0) * w10 + lanes(c01, 0) * w01 + lanes(c11, 0) * w11 + kHalf) >> 16u;
        const uint64_t ga = (lanes(c00, 8) * w00 + lanes(c10, 8) * w10 + lanes(c01, 8) * w01 + lanes(c11, 8) * w11 + kHalf) >> 16u;
        return static_cast<uint32_t>(rb & 0xFFu) | (static_cast<uint32_t>(ga & 0xFFu) << 8u) |
               (static_cast<uint32_t>((rb >> 32u) & 0xFFu) << 16u) | (static_cast<uint32_t>((ga >> 32u) & 0xFFu) << 24u);
#endif
    }

    PS2X_RASTER_INLINE uint32_t sampleTexel(const GSTexSetup &t, int u256, int v256)
    {
        return t.linear ? sampleBilinear(t, u256, v256) : sampleNearest(t, u256, v256);
    }

    // S/T/Q (FST = 0): texel coordinates from the interpolated homogeneous values, to 1/256.
    inline int texelFixedFromST(double st, double q, int size)
    {
        const double coordinate = (st / fabsQ(static_cast<float>(q))) * static_cast<double>(size) * 256.0;
        return static_cast<int>(std::floor(std::clamp(coordinate, -1.0e9, 1.0e9)));
    }

    PS2X_RASTER_INLINE uint32_t textureColour(const GSTexSetup &t, uint32_t texel, uint32_t rgba)
    {
#if PS2X_RASTER_SSE
        if (t.tfx == 0u)
        {
            // MODULATE: min((texel * vertex) >> 7, 255) per channel; alpha from the vertex when TCC = 0.
            const __m128i zero = _mm_setzero_si128();
            const __m128i tx = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(texel)), zero);
            const __m128i vx = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(rgba)), zero);
            const __m128i m = _mm_min_epu16(_mm_srli_epi16(_mm_mullo_epi16(tx, vx), 7), _mm_set1_epi16(255));
            const uint32_t out = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(m, zero)));
            return t.tcc ? out : ((out & 0x00FFFFFFu) | (rgba & 0xFF000000u));
        }
#endif
        const TextureCombineResult c = combineTexture(t.tfx, t.tcc,
                                                      static_cast<uint8_t>(rgba), static_cast<uint8_t>(rgba >> 8),
                                                      static_cast<uint8_t>(rgba >> 16), static_cast<uint8_t>(rgba >> 24),
                                                      static_cast<uint8_t>(texel), static_cast<uint8_t>(texel >> 8),
                                                      static_cast<uint8_t>(texel >> 16), static_cast<uint8_t>(texel >> 24));
        return pack32(c.r, c.g, c.b, c.a);
    }

    // ---- the pixel pipeline: fog, alpha test, destination alpha test, depth test, blend, masks ---

    PS2X_RASTER_INLINE bool alphaTestPasses(uint8_t atst, uint8_t alpha, uint8_t aref)
    {
        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        default:
            return alpha != aref;
        }
    }

    // The caller guarantees x within the scissor range and that frameRow / zRow are set up
    // for row y.
    PS2X_RASTER_INLINE void writePixel(const GSRasterSetup &s, const RowAddr &frameRow, const RowAddr &zRow,
                           int x, uint32_t z, uint32_t rgba, uint8_t fog)
    {
        uint8_t r = static_cast<uint8_t>(rgba);
        uint8_t g = static_cast<uint8_t>(rgba >> 8);
        uint8_t b = static_cast<uint8_t>(rgba >> 16);
        uint8_t a = static_cast<uint8_t>(rgba >> 24);

        if (s.fge)
        {
            const uint32_t inverseFog = 255u - fog;
            auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
            {
                return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
            };
            r = applyFog(r, s.fogR);
            g = applyFog(g, s.fogG);
            b = applyFog(b, s.fogB);
        }

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        bool writeRgb = true, writeAlpha = true, writeDepth = true;
        if (s.ate && !alphaTestPasses(s.atst, a, s.aref))
        {
            switch (s.afail)
            {
            case 1: // FB_ONLY
                writeDepth = false;
                break;
            case 2: // ZB_ONLY
                writeRgb = false;
                writeAlpha = false;
                break;
            case 3: // RGB_ONLY: distinct only for RGBA32, FB_ONLY otherwise
                writeDepth = false;
                writeAlpha = s.fpsm != GS_PSM_CT32;
                break;
            default: // KEEP
                return;
            }
        }
        const bool writesFramebuffer = writeRgb || writeAlpha;
        if (!writesFramebuffer && (!writeDepth || s.zmsk))
            return;

        const bool preserveDestinationAlpha = writeRgb && !writeAlpha && s.fpsm == GS_PSM_CT32;
        const bool readFrame = s.dateReadsFrame || (writesFramebuffer && (s.fbmsk != 0u || s.abe || preserveDestinationAlpha));

        uint32_t frameAddr = 0u;
        uint32_t rawFramebufferPixel = 0u;
        uint32_t fbrgba = 0u;
        if (writesFramebuffer || readFrame)
            frameAddr = frameRow.at(static_cast<uint32_t>(x));
        if (readFrame)
        {
            rawFramebufferPixel = loadPixel(s.vram, s.frameStore, frameAddr);
            fbrgba = rawFramebufferPixel;
            if (s.frame16)
                fbrgba = Rgba5551ToRgba8888(static_cast<u16>(fbrgba));
            else if (s.fpsm == GS_PSM_CT24)
                fbrgba |= 0x80000000u; // the GS supplies 0x80 as destination alpha for RGB24 blending
        }

        if (s.dateReadsFrame)
        {
            const bool msb = ((rawFramebufferPixel >> (s.frame16 ? 15u : 31u)) & 0x1u) != 0u;
            if (msb != s.datm)
                return;
        }

        if (s.ztst == 0u)
            return;
        const bool zNeeded = s.ztst >= 2u || (writeDepth && !s.zmsk);
        uint32_t zAddr = 0u;
        if (zNeeded)
            zAddr = zRow.at(static_cast<uint32_t>(x));
        if (s.ztst >= 2u)
        {
            const uint32_t storedZ = loadPixel(s.vram, s.zStore, zAddr);
            if (s.ztst == 2u ? !(z >= storedZ) : !(z > storedZ))
                return;
        }

        if (writesFramebuffer)
        {
            // PABE disables alpha blending when the source alpha MSB is clear.
            if (s.abe && !(s.pabe && (a & 0x80u) == 0u))
            {
#if PS2X_RASTER_SSE
                // ((A - B) * C >> 7) + D per channel in 32-bit lanes (A - B can be negative).
                const __m128i zero = _mm_setzero_si128();
                const __m128i cs = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(pack32(r, g, b, a))), zero), zero);
                const __m128i cd = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(fbrgba)), zero), zero);
                auto pickVec = [&](uint8_t sel) -> __m128i
                { return sel == 0u ? cs : (sel == 1u ? cd : zero); };
                const int cAlpha = (s.csel == 0u) ? a : (s.csel == 1u) ? static_cast<int>(fbrgba >> 24) : s.fix;
                const __m128i diff = _mm_sub_epi32(pickVec(s.asel), pickVec(s.bsel));
                const __m128i blended = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(diff, _mm_set1_epi32(cAlpha)), 7), pickVec(s.dsel));
                const uint32_t out = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(blended, zero), zero)));
                r = static_cast<uint8_t>(out);
                g = static_cast<uint8_t>(out >> 8);
                b = static_cast<uint8_t>(out >> 16);
#else
                const int dr = static_cast<int>(fbrgba & 0xFFu);
                const int dg = static_cast<int>((fbrgba >> 8) & 0xFFu);
                const int db = static_cast<int>((fbrgba >> 16) & 0xFFu);
                const int da = static_cast<int>((fbrgba >> 24) & 0xFFu);
                auto pick = [](uint8_t sel, int cs, int cd) -> int
                { return sel == 0u ? cs : (sel == 1u ? cd : 0); };
                const int cAlpha = (s.csel == 0u) ? a : (s.csel == 1u) ? da : s.fix;
                r = clampU8(((pick(s.asel, r, dr) - pick(s.bsel, r, dr)) * cAlpha >> 7) + pick(s.dsel, r, dr));
                g = clampU8(((pick(s.asel, g, dg) - pick(s.bsel, g, dg)) * cAlpha >> 7) + pick(s.dsel, g, dg));
                b = clampU8(((pick(s.asel, b, db) - pick(s.bsel, b, db)) * cAlpha >> 7) + pick(s.dsel, b, db));
#endif
            }

            if (writeAlpha && s.fba && s.fpsm != GS_PSM_CT24)
                a = static_cast<uint8_t>(a | 0x80u);

            uint32_t pixel = pack32(r, g, b, a);
            if (s.fbmsk != 0u)
                pixel = (pixel & ~s.fbmsk) | (fbrgba & s.fbmsk);
            if (preserveDestinationAlpha)
                pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
            if (s.frame16)
                pixel = Rgba8888ToRgba5551(pixel);
            storePixel(s.vram, s.frameStore, frameAddr, pixel);
        }

        if (writeDepth && !s.zmsk)
            storePixel(s.vram, s.zStore, zAddr, z);
    }

    // A pixel at any position (points, lines): scissor test and row setup included.
    void writePixelAt(const GSRasterSetup &s, int x, int y, uint32_t z, uint32_t rgba, uint8_t fog)
    {
        if (x < s.scissorX0 || x > s.scissorX1 || y < s.scissorY0 || y > s.scissorY1)
            return;
        RowAddr frameRow, zRow;
        s.frameRow(frameRow, s.fbp, s.fbw, static_cast<uint32_t>(y));
        s.zRow(zRow, s.zbp, s.fbw, static_cast<uint32_t>(y));
        writePixel(s, frameRow, zRow, x, z, rgba, fog);
    }

    // ---- exact linear interpolation --------------------------------------------------------------

    // floor(n / d) and the remainder in [0, d), d > 0.
    inline void floorDivMod(int64_t n, int64_t d, int64_t &q, int64_t &r)
    {
        q = n / d;
        r = n % d;
        if (r < 0)
        {
            r += d;
            --q;
        }
    }

    // floor((a * b + c * d + e * f) / den) and the remainder, with a 128-bit numerator (den > 0;
    // the quotient fits in 64 bits: it lies between the vertex values).
    inline int64_t floorDivMod128(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t den, int64_t &rem)
    {
#if defined(_MSC_VER)
        int64_t h1, h2, h3;
        const int64_t l1 = _mul128(a, b, &h1);
        const int64_t l2 = _mul128(c, d, &h2);
        const int64_t l3 = _mul128(e, f, &h3);
        uint64_t lo = static_cast<uint64_t>(l1);
        int64_t hi = h1;
        unsigned char carry = _addcarry_u64(0, lo, static_cast<uint64_t>(l2), &lo);
        hi += h2 + carry;
        carry = _addcarry_u64(0, lo, static_cast<uint64_t>(l3), &lo);
        hi += h3 + carry;
        int64_t r;
        int64_t q = _div128(hi, static_cast<int64_t>(lo), den, &r);
#else
        const __int128 n = static_cast<__int128>(a) * b + static_cast<__int128>(c) * d + static_cast<__int128>(e) * f;
        int64_t q = static_cast<int64_t>(n / den);
        int64_t r = static_cast<int64_t>(n % den);
#endif
        if (r < 0)
        {
            r += den;
            --q;
        }
        rem = r;
        return q;
    }

    // value(x) = floor((v0 * den + B1 * d1 + B2 * d2) / den) stepped along a row as an integer
    // quotient and remainder: exact for any length of run (the per-pixel step's numerator fits
    // int64; the row start uses 128 bits). The setup is shared by every row of the primitive;
    // the running state (Run) is per row.
    struct AttrDda
    {
        int64_t dq = 0;
        int64_t dr = 0;
        int64_t den = 1;
        int64_t v0 = 0;
        int64_t d1 = 0;
        int64_t d2 = 0;

        struct Run
        {
            int64_t q = 0;
            int64_t r = 0;
        };

        // b1Step/b2Step: change of B1/B2 per pixel.
        void setup(int64_t a0, int64_t a1, int64_t a2, int64_t denominator, int64_t b1Step, int64_t b2Step)
        {
            den = denominator;
            v0 = a0;
            d1 = a1 - a0;
            d2 = a2 - a0;
            int64_t rem;
            dq = floorDivMod128(b1Step, d1, b2Step, d2, 0, 0, den, rem);
            dr = rem;
        }

        void start(Run &run, int64_t b1, int64_t b2) const
        {
            run.q = floorDivMod128(v0, den, b1, d1, b2, d2, den, run.r);
        }

        PS2X_RASTER_INLINE void step(Run &run) const
        {
            run.q += dq;
            run.r += dr;
            if (run.r >= den)
            {
                run.r -= den;
                ++run.q;
            }
        }

        PS2X_RASTER_INLINE int64_t value(const Run &run) const
        {
            return run.q;
        }
    };

    PS2X_RASTER_INLINE uint8_t attrU8(const AttrDda &d, const AttrDda::Run &run)
    {
        return static_cast<uint8_t>(std::clamp<int64_t>(d.value(run), 0, 255));
    }

    PS2X_RASTER_INLINE uint32_t attrZ(const AttrDda &d, const AttrDda::Run &run)
    {
        return static_cast<uint32_t>(std::clamp<int64_t>(d.value(run), 0, 4294967295ll));
    }

    PS2X_RASTER_INLINE int attrTexel256(const AttrDda &d, const AttrDda::Run &run)
    {
        return static_cast<int>(std::clamp<int64_t>(d.value(run), 0, 65535)) << 4;
    }

    struct TriangleEdge
    {
        int64_t stepX; // change of the edge function per pixel in x
        int64_t stepY; // per pixel in y
        int64_t row;   // value at (minX, minY), minus the bias
    };

    // floor(n / d) for d > 0.
    inline int64_t floorDivPos(int64_t n, int64_t d)
    {
        const int64_t q = n / d;
        return (n % d < 0) ? q - 1 : q;
    }

    // The pixels x in [minX, maxX] of one row that satisfy e + (x - minX) * stepX >= 0 for all
    // three edges (e = edge function at (minX, y) minus the bias): [xs, xe], empty if xs > xe.
    inline void rowSpan(const TriangleEdge (&edges)[3], const int64_t (&e)[3], int64_t minX, int64_t maxX, int64_t &xs, int64_t &xe)
    {
        xs = minX;
        xe = maxX;
        for (int k = 0; k < 3; ++k)
        {
            const int64_t stepX = edges[k].stepX;
            if (stepX > 0)
            {
                if (e[k] < 0)
                    xs = std::max(xs, minX - floorDivPos(e[k], stepX)); // ceil(-e / stepX)
            }
            else if (stepX < 0)
            {
                if (e[k] < 0)
                    xe = minX - 1;
                else
                    xe = std::min(xe, minX + floorDivPos(e[k], -stepX));
            }
            else if (e[k] < 0)
                xe = minX - 1;
        }
    }

    inline int64_t fixed16(float coordinate)
    {
        return static_cast<int64_t>(std::llround(static_cast<double>(coordinate) * 16.0));
    }

    inline int64_t ceilDiv16(int64_t v)
    {
        return (v >= 0) ? ((v + 15) >> 4) : -((-v) >> 4);
    }

    inline int64_t floorDiv16(int64_t v)
    {
        return (v >= 0) ? (v >> 4) : -((-v + 15) >> 4);
    }

    // ---- sprites -----------------------------------------------------------------------------
    //
    // Each primitive is set up once into a Job; the rows of a chunk are shaded by a function
    // that first copies the Job into locals: the Job's address is handed to the worker pool and
    // local-memory stores go through byte pointers, so anything reached through that pointer
    // would otherwise be reloaded after every pixel.

    struct SpriteJob
    {
        GSRasterSetup s;
        int64_t drawX0 = 0, drawX1 = 0;
        int64_t xa = 0, ya = 0, xStart = 0, yStart0 = 0;
        int64_t numY0 = 0, numY1 = 0, denY = 0;
        AttrDda u;
        double ua = 0.0, va = 0.0, du = 0.0, dv = 0.0;
        uint32_t z1 = 0, rgba = 0;
        uint8_t fog = 0;
    };

    // Tex: 0 = none, 1 = U/V (FST), 2 = S/T/Q.
    template <int Tex>
    void shadeSpriteRows(const SpriteJob &job, int yStart, int yEnd)
    {
        const GSRasterSetup s = job.s;
        const GSTexSetup &t = s.tex;
        const AttrDda u = job.u;
        const int64_t drawX0 = job.drawX0, drawX1 = job.drawX1, xStart = job.xStart, yStart0 = job.yStart0;
        const int64_t numY0 = job.numY0, numY1 = job.numY1, denY = job.denY, xa = job.xa, ya = job.ya;
        const double ua = job.ua, va = job.va, du = job.du, dv = job.dv;
        const uint32_t z1 = job.z1, rgba = job.rgba;
        const uint8_t fog = job.fog;
        RowAddr frameRow, zRow;
        AttrDda::Run uRun{};
        for (int64_t y = yStart; y <= yEnd; ++y)
        {
            s.frameRow(frameRow, s.fbp, s.fbw, static_cast<uint32_t>(y));
            s.zRow(zRow, s.zbp, s.fbw, static_cast<uint32_t>(y));
            int v256 = 0;
            if constexpr (Tex == 1)
            {
                int64_t vq = numY0, vr;
                if (denY > 0)
                    floorDivMod(numY0 * denY + (numY1 - numY0) * ((y << 4) - yStart0), denY, vq, vr);
                v256 = static_cast<int>(std::clamp<int64_t>(vq, 0, 65535)) << 4;
                u.start(uRun, (drawX0 << 4) - xStart, 0);
            }
            else if constexpr (Tex == 2)
            {
                const double texV = va + dv * static_cast<double>((y << 4) - ya);
                v256 = static_cast<int>(std::floor(std::clamp(texV * 256.0, -1.0e9, 1.0e9)));
            }
            for (int64_t x = drawX0; x <= drawX1; ++x)
            {
                if constexpr (Tex == 0)
                    writePixel(s, frameRow, zRow, static_cast<int>(x), z1, rgba, fog);
                else if constexpr (Tex == 1)
                {
                    const uint32_t texel = sampleTexel(t, attrTexel256(u, uRun), v256);
                    writePixel(s, frameRow, zRow, static_cast<int>(x), z1, textureColour(t, texel, rgba), fog);
                    u.step(uRun);
                }
                else
                {
                    const double texU = ua + du * static_cast<double>((x << 4) - xa);
                    const int u256 = static_cast<int>(std::floor(std::clamp(texU * 256.0, -1.0e9, 1.0e9)));
                    const uint32_t texel = sampleTexel(t, u256, v256);
                    writePixel(s, frameRow, zRow, static_cast<int>(x), z1, textureColour(t, texel, rgba), fog);
                }
            }
        }
    }

    template <int Tex>
    void drawSpriteImpl(const GSPrimitiveBatch &batch, const GSRasterSetup &s)
    {
        // Coverage: x in [ceil(x0), ceil(x1)), y in [ceil(y0), ceil(y1)) in window coordinates.
        // U/V (or S/T) are interpolated linearly from v0 to v1 to the sampling point (x.0, y.0);
        // colour, Z and fog come from v1.
        const GSDrawState &state = batch.state;
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];
        const auto &ctx = state.context;

        SpriteJob job;
        job.s = s;
        const int64_t xa = fixed16(v0.x) - static_cast<int64_t>(ctx.xyoffset.ofx);
        const int64_t ya = fixed16(v0.y) - static_cast<int64_t>(ctx.xyoffset.ofy);
        const int64_t xb = fixed16(v1.x) - static_cast<int64_t>(ctx.xyoffset.ofx);
        const int64_t yb = fixed16(v1.y) - static_cast<int64_t>(ctx.xyoffset.ofy);
        job.xa = xa;
        job.ya = ya;

        const int64_t drawX0 = std::max<int64_t>(ceilDiv16(std::min(xa, xb)), s.scissorX0);
        const int64_t drawX1 = std::min<int64_t>(ceilDiv16(std::max(xa, xb)) - 1, s.scissorX1);
        const int64_t drawY0 = std::max<int64_t>(ceilDiv16(std::min(ya, yb)), s.scissorY0);
        const int64_t drawY1 = std::min<int64_t>(ceilDiv16(std::max(ya, yb)) - 1, s.scissorY1);
        if (drawX0 > drawX1 || drawY0 > drawY1)
            return;
        job.drawX0 = drawX0;
        job.drawX1 = drawX1;

        job.z1 = static_cast<uint32_t>(std::clamp(v1.z, 0.0, 4294967295.0));
        job.rgba = pack32(v1.r, v1.g, v1.b, v1.a);
        job.fog = v1.fog;
        const GSTexSetup &t = s.tex;
        if (gsStatsEnabled())
        {
            const uint64_t pixels = static_cast<uint64_t>(drawX1 - drawX0 + 1) * static_cast<uint64_t>(drawY1 - drawY0 + 1);
            statAdd(s_rasterStats.rows, static_cast<uint64_t>(drawY1 - drawY0 + 1));
            statAdd(s_rasterStats.bboxPixels, pixels);
            statAdd(s_rasterStats.coveredPixels, pixels);
            if (Tex != 0)
                statAdd(s_rasterStats.texturedPixels, pixels);
            if (Tex != 0 && t.linear)
                statAdd(s_rasterStats.linearPixels, pixels);
            if (pixels >= 4096u)
                statAdd(s_rasterStats.bigPrimitives);
        }

        // FST: U at pixel x is ua + (ub - ua) (16 x - xa) / (xb - xa), exact; V likewise per row.
        int64_t denX = xb - xa, numX0 = static_cast<int64_t>(v0.u), numX1 = static_cast<int64_t>(v1.u);
        if (denX < 0)
        {
            denX = -denX;
            std::swap(numX0, numX1);
        }
        int64_t denY = yb - ya, numY0 = static_cast<int64_t>(v0.v), numY1 = static_cast<int64_t>(v1.v);
        if (denY < 0)
        {
            denY = -denY;
            std::swap(numY0, numY1);
        }
        job.xStart = std::min(xa, xb);
        job.yStart0 = std::min(ya, yb);
        job.numY0 = numY0;
        job.numY1 = numY1;
        job.denY = denY;
        if constexpr (Tex == 1)
            job.u.setup(numX0, numX1, numX0, std::max<int64_t>(denX, 1), 16, 0);

        // S/T/Q: texel coordinates at the two vertices, interpolated in double. A sprite has
        // one Q, the second vertex's (RGBAQ is latched with the kick; tools/gsref.py does the
        // same), so both ends divide by it.
        if constexpr (Tex == 2)
        {
            const double q1 = fabsQ(v1.q);
            const double q0 = q1;
            job.ua = (v0.s / q0) * t.w;
            job.va = (v0.t / q0) * t.h;
            const double ub = (v1.s / q1) * t.w;
            const double vb = (v1.t / q1) * t.h;
            job.du = (xb != xa) ? (ub - job.ua) / static_cast<double>(xb - xa) : 0.0;
            job.dv = (yb != ya) ? (vb - job.va) / static_cast<double>(yb - ya) : 0.0;
        }

        forEachRowChunk(static_cast<int>(drawY0), static_cast<int>(drawY1),
                        static_cast<int64_t>(drawX1 - drawX0 + 1) * static_cast<int64_t>(drawY1 - drawY0 + 1),
                        t.enabled && t.selfSampling, [&job](int ys, int ye)
                        { shadeSpriteRows<Tex>(job, ys, ye); });
    }

    // ---- triangles -----------------------------------------------------------------------------

    struct TriangleJob
    {
        GSRasterSetup s;
        TriangleEdge edges[3];
        int64_t minX = 0, maxX = 0, minY = 0;
        int64_t b1Row = 0, b2Row = 0;
        int64_t b1StepX = 0, b2StepX = 0, b1StepY = 0, b2StepY = 0;
        AttrDda z, r, g, b, a, u, v, fog;
        uint32_t flatRgba = 0;
        double s0 = 0.0, t0 = 0.0, q0 = 0.0;
        double ds1 = 0.0, ds2 = 0.0, dt1 = 0.0, dt2 = 0.0, dq1 = 0.0, dq2 = 0.0, invDen = 1.0;
    };

    template <bool Gouraud, int Tex, bool Fog>
    void shadeTriangleRows(const TriangleJob &job, int yStart, int yEnd)
    {
        const GSRasterSetup s = job.s;
        const GSTexSetup &t = s.tex;
        const TriangleEdge edges[3] = {job.edges[0], job.edges[1], job.edges[2]};
        const AttrDda z = job.z, r = job.r, g = job.g, b = job.b, a = job.a, u = job.u, v = job.v, fog = job.fog;
        const int64_t minX = job.minX, maxX = job.maxX, minY = job.minY;
        const int64_t b1Row = job.b1Row, b2Row = job.b2Row;
        const int64_t b1StepX = job.b1StepX, b2StepX = job.b2StepX, b1StepY = job.b1StepY, b2StepY = job.b2StepY;
        const uint32_t flatRgba = job.flatRgba;
        const double s0 = job.s0, t0 = job.t0, q0 = job.q0, invDen = job.invDen;
        const double ds1 = job.ds1, ds2 = job.ds2, dt1 = job.dt1, dt2 = job.dt2, dq1 = job.dq1, dq2 = job.dq2;
        const bool stats = gsStatsEnabled();

        RowAddr frameRow, zRow;
        AttrDda::Run zRun{}, rRun{}, gRun{}, bRun{}, aRun{}, uRun{}, vRun{}, fogRun{};
        uint64_t rows = 0, covered = 0;
        for (int64_t y = yStart; y <= yEnd; ++y)
        {
            // Row state from the row index (not from the row before: rows may be shaded on
            // different threads). The covered pixels of a row form one run: find its start,
            // shade until it ends.
            const int64_t dy = y - minY;
            const int64_t e[3] = {edges[0].row + dy * edges[0].stepY, edges[1].row + dy * edges[1].stepY, edges[2].row + dy * edges[2].stepY};
            int64_t x, xe;
            rowSpan(edges, e, minX, maxX, x, xe);
            if (x > xe)
                continue;
            s.frameRow(frameRow, s.fbp, s.fbw, static_cast<uint32_t>(y));
            s.zRow(zRow, s.zbp, s.fbw, static_cast<uint32_t>(y));
            int64_t b1 = b1Row + dy * b1StepY + (x - minX) * b1StepX;
            int64_t b2 = b2Row + dy * b2StepY + (x - minX) * b2StepX;
            z.start(zRun, b1, b2);
            if constexpr (Gouraud)
            {
                r.start(rRun, b1, b2);
                g.start(gRun, b1, b2);
                b.start(bRun, b1, b2);
                a.start(aRun, b1, b2);
            }
            if constexpr (Tex == 1)
            {
                u.start(uRun, b1, b2);
                v.start(vRun, b1, b2);
            }
            if constexpr (Fog)
                fog.start(fogRun, b1, b2);

            const int64_t spanStart = x;
            for (; x <= xe; ++x)
            {
                uint32_t rgba = flatRgba;
                if constexpr (Gouraud)
                    rgba = pack32(attrU8(r, rRun), attrU8(g, gRun), attrU8(b, bRun), attrU8(a, aRun));
                if constexpr (Tex == 1)
                    rgba = textureColour(t, sampleTexel(t, attrTexel256(u, uRun), attrTexel256(v, vRun)), rgba);
                else if constexpr (Tex == 2)
                {
                    // The GS DDA interpolates the homogeneous S, T and Q; texel coordinates come
                    // from S/Q and T/Q after interpolation.
                    const double fb1 = static_cast<double>(b1), fb2 = static_cast<double>(b2);
                    const double is = s0 + (fb1 * ds1 + fb2 * ds2) * invDen;
                    const double it = t0 + (fb1 * dt1 + fb2 * dt2) * invDen;
                    const double iq = q0 + (fb1 * dq1 + fb2 * dq2) * invDen;
                    rgba = textureColour(t, sampleTexel(t, texelFixedFromST(is, iq, t.w), texelFixedFromST(it, iq, t.h)), rgba);
                }
                uint8_t fogValue = 0u;
                if constexpr (Fog)
                    fogValue = attrU8(fog, fogRun);
                writePixel(s, frameRow, zRow, static_cast<int>(x), attrZ(z, zRun), rgba, fogValue);

                if constexpr (Tex == 2)
                {
                    b1 += b1StepX;
                    b2 += b2StepX;
                }
                z.step(zRun);
                if constexpr (Gouraud)
                {
                    r.step(rRun);
                    g.step(gRun);
                    b.step(bRun);
                    a.step(aRun);
                }
                if constexpr (Tex == 1)
                {
                    u.step(uRun);
                    v.step(vRun);
                }
                if constexpr (Fog)
                    fog.step(fogRun);
            }
            ++rows;
            covered += static_cast<uint64_t>(x - spanStart);
        }
        if (stats)
        {
            statAdd(s_rasterStats.rows, rows);
            statAdd(s_rasterStats.coveredPixels, covered);
            if (Tex != 0)
                statAdd(s_rasterStats.texturedPixels, covered);
            if (Tex != 0 && t.linear)
                statAdd(s_rasterStats.linearPixels, covered);
        }
    }

    template <bool Gouraud, int Tex, bool Fog>
    void drawTriangleImpl(const GSPrimitiveBatch &batch, const GSRasterSetup &s)
    {
        const GSDrawState &state = batch.state;
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];
        const GSVertex &v2 = batch.vertices[2];
        const auto &ctx = state.context;

        const int64_t ofx = static_cast<int64_t>(ctx.xyoffset.ofx);
        const int64_t ofy = static_cast<int64_t>(ctx.xyoffset.ofy);
        const int64_t x0 = fixed16(v0.x) - ofx, y0 = fixed16(v0.y) - ofy;
        const int64_t x1 = fixed16(v1.x) - ofx, y1 = fixed16(v1.y) - ofy;
        const int64_t x2 = fixed16(v2.x) - ofx, y2 = fixed16(v2.y) - ofy;

        // Twice the signed area, in 1/256 pixel units.
        const int64_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (area == 0)
            return;

        int64_t minX = ceilDiv16(std::min({x0, x1, x2}));
        int64_t maxX = floorDiv16(std::max({x0, x1, x2}));
        int64_t minY = ceilDiv16(std::min({y0, y1, y2}));
        int64_t maxY = floorDiv16(std::max({y0, y1, y2}));
        minX = std::max<int64_t>(minX, s.scissorX0);
        maxX = std::min<int64_t>(maxX, s.scissorX1);
        minY = std::max<int64_t>(minY, s.scissorY0);
        maxY = std::min<int64_t>(maxY, s.scissorY1);
        if (minX > maxX || minY > maxY)
            return;

        TriangleJob job;
        job.s = s;
        job.minX = minX;
        job.maxX = maxX;
        job.minY = minY;

        // Edges in counter-clockwise order (y down), so that every edge function is > 0 inside.
        int64_t ex[3] = {x0, x1, x2};
        int64_t ey[3] = {y0, y1, y2};
        if (area < 0)
        {
            std::swap(ex[1], ex[2]);
            std::swap(ey[1], ey[2]);
        }
        for (int k = 0; k < 3; ++k)
        {
            const int64_t ax = ex[k], ay = ey[k];
            const int64_t bx = ex[(k + 1) % 3], by = ey[(k + 1) % 3];
            const int64_t dx = bx - ax, dy = by - ay;
            // e(p) = dx (py - ay) - dy (px - ax); its gradient (-dy, dx) is the inward normal. A
            // left edge has the interior to its right (normal x > 0); a top edge is horizontal
            // with the interior below it (normal x == 0, normal y > 0). Points on a top or left
            // edge are drawn (bias 0), on the others not (bias 1).
            const bool topLeft = (-dy > 0) || (dy == 0 && dx > 0);
            job.edges[k].stepX = -dy * 16;
            job.edges[k].stepY = dx * 16;
            job.edges[k].row = dx * ((minY << 4) - ay) - dy * ((minX << 4) - ax) - (topLeft ? 0 : 1);
        }

        // Barycentric numerators of v1 and v2 (original vertex order), made positive inside:
        // B1 = (px - x0)(y2 - y0) - (x2 - x0)(py - y0), B2 = (x1 - x0)(py - y0) - (px - x0)(y1 - y0).
        const int64_t sign = area < 0 ? -1 : 1;
        const int64_t den = area * sign;
        job.b1StepX = sign * 16 * (y2 - y0);
        job.b2StepX = -sign * 16 * (y1 - y0);
        job.b1StepY = -sign * 16 * (x2 - x0);
        job.b2StepY = sign * 16 * (x1 - x0);
        job.b1Row = sign * (((minX << 4) - x0) * (y2 - y0) - (x2 - x0) * ((minY << 4) - y0));
        job.b2Row = sign * ((x1 - x0) * ((minY << 4) - y0) - ((minX << 4) - x0) * (y1 - y0));

        job.flatRgba = pack32(v2.r, v2.g, v2.b, v2.a);
        auto zInt = [](double value) -> int64_t
        { return static_cast<int64_t>(std::llround(std::clamp(value, 0.0, 4294967295.0))); };
        job.z.setup(zInt(v0.z), zInt(v1.z), zInt(v2.z), den, job.b1StepX, job.b2StepX);
        if constexpr (Gouraud)
        {
            job.r.setup(v0.r, v1.r, v2.r, den, job.b1StepX, job.b2StepX);
            job.g.setup(v0.g, v1.g, v2.g, den, job.b1StepX, job.b2StepX);
            job.b.setup(v0.b, v1.b, v2.b, den, job.b1StepX, job.b2StepX);
            job.a.setup(v0.a, v1.a, v2.a, den, job.b1StepX, job.b2StepX);
        }
        if constexpr (Tex == 1)
        {
            job.u.setup(v0.u, v1.u, v2.u, den, job.b1StepX, job.b2StepX);
            job.v.setup(v0.v, v1.v, v2.v, den, job.b1StepX, job.b2StepX);
        }
        if constexpr (Fog)
            job.fog.setup(v0.fog, v1.fog, v2.fog, den, job.b1StepX, job.b2StepX);
        // S/T/Q in double: value = s0 + (B1 ds1 + B2 ds2) / den.
        job.invDen = 1.0 / static_cast<double>(den);
        job.s0 = v0.s;
        job.t0 = v0.t;
        job.q0 = v0.q;
        job.ds1 = static_cast<double>(v1.s) - v0.s;
        job.ds2 = static_cast<double>(v2.s) - v0.s;
        job.dt1 = static_cast<double>(v1.t) - v0.t;
        job.dt2 = static_cast<double>(v2.t) - v0.t;
        job.dq1 = static_cast<double>(v1.q) - v0.q;
        job.dq2 = static_cast<double>(v2.q) - v0.q;

        if (gsStatsEnabled())
        {
            const uint64_t box = static_cast<uint64_t>(maxX - minX + 1) * static_cast<uint64_t>(maxY - minY + 1);
            statAdd(s_rasterStats.bboxPixels, box);
            if (box >= 4096u)
                statAdd(s_rasterStats.bigPrimitives);
        }

        forEachRowChunk(static_cast<int>(minY), static_cast<int>(maxY),
                        static_cast<int64_t>(maxX - minX + 1) * static_cast<int64_t>(maxY - minY + 1),
                        s.tex.enabled && s.tex.selfSampling, [&job](int ys, int ye)
                        { shadeTriangleRows<Gouraud, Tex, Fog>(job, ys, ye); });
    }

    template <bool Gouraud, int Tex>
    void drawTriangleFog(const GSPrimitiveBatch &batch, const GSRasterSetup &s)
    {
        if (s.fge)
            drawTriangleImpl<Gouraud, Tex, true>(batch, s);
        else
            drawTriangleImpl<Gouraud, Tex, false>(batch, s);
    }

    template <bool Gouraud>
    void drawTriangleTex(const GSPrimitiveBatch &batch, const GSRasterSetup &s)
    {
        if (!s.tex.enabled)
            drawTriangleFog<Gouraud, 0>(batch, s);
        else if (s.tex.fst)
            drawTriangleFog<Gouraud, 1>(batch, s);
        else
            drawTriangleFog<Gouraud, 2>(batch, s);
    }
}

void GSCpuBackend::StampPages(const GSPageRange &range)
{
    ++m_writeStamp;
    for (uint32_t i = 0; i < range.count; ++i)
        m_pageWriteStamp[(range.first + i) % GSPageRange::kPages] = m_writeStamp;
}

void GSCpuBackend::StampAllPages()
{
    ++m_writeStamp;
    m_pageWriteStamp.fill(m_writeStamp);
}

// The decoded copy of the draw's texture (indices for CLUT formats, RGBA with TEXA applied for
// the others), decoded once and kept while the pages it reads are not written (StampPages) and
// it stays within the cache budget. Not used when the texture overlaps the draw's own frame
// or Z pages (the draw changes what it samples) or when a region wrap mode reads outside the
// 2^TW x 2^TH area the copy covers.
bool GSCpuBackend::LookupDecodedTexture(GSTexSetup &t, const GSPageRange &frameTarget, const GSPageRange &zTarget,
                                        uint8_t (*indexFetch)(const GSTexSetup &, int, int))
{
    auto axisWithin = [](uint8_t mode, int minimum, int maximum, int size)
    {
        switch (mode & 3u)
        {
        case 2:
            return maximum < size;
        case 3:
            return (minimum | maximum) < size;
        default:
            return true;
        }
    };
    if (!axisWithin(t.wrapU, t.minU, t.maxU, t.w) || !axisWithin(t.wrapV, t.minV, t.maxV, t.h))
        return false;

    const GSPageRange pages = pageRangeFor(t.psm, t.tbp0, t.tbw, 0u, 0u, static_cast<uint32_t>(t.w - 1), static_cast<uint32_t>(t.h - 1));
    if (pageRangesOverlap(pages, frameTarget) || pageRangesOverlap(pages, zTarget))
    {
        // The draw changes what it samples: local memory is read directly, and the rows are
        // shaded in order on the calling thread (see forEachRowChunk).
        t.selfSampling = true;
        return false;
    }

    const bool indexed = indexFetch != nullptr;
    const uint64_t key = static_cast<uint64_t>(t.tbp0) | (static_cast<uint64_t>(t.tbw) << 14) | (static_cast<uint64_t>(t.psm) << 20) |
                         (static_cast<uint64_t>(log2Pow2(static_cast<uint32_t>(t.w))) << 26) |
                         (static_cast<uint64_t>(log2Pow2(static_cast<uint32_t>(t.h))) << 30) |
                         (indexed ? 0ull : ((static_cast<uint64_t>(t.texa.ta0) << 34) | (static_cast<uint64_t>(t.texa.ta1) << 42) |
                                            (static_cast<uint64_t>(t.texa.aem) << 50)));
    uint64_t pagesWrittenAt = 0;
    for (uint32_t i = 0; i < pages.count; ++i)
        pagesWrittenAt = std::max(pagesWrittenAt, m_pageWriteStamp[(pages.first + i) % GSPageRange::kPages]);

    ++m_textureUseCounter;
    for (GSDecodedTexture &entry : m_decodedTextures)
    {
        if (entry.key != key)
            continue;
        if (entry.writeStamp < pagesWrittenAt)
            break; // stale: decode again below (the entry is replaced)
        entry.lastUse = m_textureUseCounter;
        t.decodedIndices = indexed ? entry.indices.data() : nullptr;
        t.decodedRgba = indexed ? nullptr : entry.rgba.data();
        return true;
    }

    // Decode (into a replaced stale entry or a new one), keeping the cache within budget.
    const size_t texels = static_cast<size_t>(t.w) * static_cast<size_t>(t.h);
    const size_t bytes = texels * (indexed ? 1u : 4u);
    GSDecodedTexture *slot = nullptr;
    for (GSDecodedTexture &entry : m_decodedTextures)
        if (entry.key == key)
            slot = &entry;
    if (!slot)
    {
        constexpr size_t kBudgetBytes = 64u << 20;
        constexpr size_t kMaxEntries = 64u;
        size_t used = 0;
        for (const GSDecodedTexture &entry : m_decodedTextures)
            used += entry.indices.size() + entry.rgba.size() * 4u;
        while (!m_decodedTextures.empty() && (used + bytes > kBudgetBytes || m_decodedTextures.size() >= kMaxEntries))
        {
            size_t oldest = 0;
            for (size_t i = 1; i < m_decodedTextures.size(); ++i)
                if (m_decodedTextures[i].lastUse < m_decodedTextures[oldest].lastUse)
                    oldest = i;
            used -= m_decodedTextures[oldest].indices.size() + m_decodedTextures[oldest].rgba.size() * 4u;
            m_decodedTextures.erase(m_decodedTextures.begin() + static_cast<std::ptrdiff_t>(oldest));
        }
        m_decodedTextures.emplace_back();
        slot = &m_decodedTextures.back();
    }
    slot->key = key;
    slot->writeStamp = m_writeStamp;
    slot->lastUse = m_textureUseCounter;
    // Decode rows on the pool (a texture row is independent of the others).
    if (indexed)
    {
        slot->rgba.clear();
        slot->rgba.shrink_to_fit();
        slot->indices.resize(texels);
        uint8_t *out = slot->indices.data();
        const int w = t.w;
        forEachRowChunk(0, t.h - 1, static_cast<int64_t>(texels), false, [&](int ys, int ye)
                        {
            for (int v = ys; v <= ye; ++v)
            {
                uint8_t *row = out + static_cast<size_t>(v) * static_cast<size_t>(w);
                for (int u = 0; u < w; ++u)
                    row[u] = indexFetch(t, u, v);
            } });
        t.decodedIndices = slot->indices.data();
        t.decodedRgba = nullptr;
    }
    else
    {
        slot->indices.clear();
        slot->indices.shrink_to_fit();
        slot->rgba.resize(texels);
        uint32_t *out = slot->rgba.data();
        const int w = t.w;
        forEachRowChunk(0, t.h - 1, static_cast<int64_t>(texels), false, [&](int ys, int ye)
                        {
            for (int v = ys; v <= ye; ++v)
            {
                uint32_t *row = out + static_cast<size_t>(v) * static_cast<size_t>(w);
                for (int u = 0; u < w; ++u)
                    row[u] = t.fetch(t, u, v);
            } });
        t.decodedIndices = nullptr;
        t.decodedRgba = slot->rgba.data();
    }
    if (gsStatsEnabled())
    {
        statAdd(s_rasterStats.textureDecodes);
        statAdd(s_rasterStats.textureDecodedTexels, texels);
    }
    return true;
}

void GSCpuBackend::BuildRasterSetup(const GSDrawState &state, GSRasterSetup &s, const GSPageRange &frameTarget, const GSPageRange &zTarget)
{
    const auto &ctx = state.context;
    s.vram = m_vram;
    s.scissorX0 = ctx.scissor.x0;
    s.scissorX1 = ctx.scissor.x1;
    s.scissorY0 = ctx.scissor.y0;
    s.scissorY1 = ctx.scissor.y1;

    s.fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    s.fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    s.fpsm = ctx.frame.psm;
    s.fbmsk = ctx.frame.fbmsk;
    s.fba = (ctx.fba & 0x1ull) != 0ull;
    s.frame16 = bitsPerPixel(s.fpsm) == 16u;
    if (!pixelStoreFor(s.fpsm, s.frameRow, s.frameStore))
    {
        // Not a frame buffer format: nothing sensible to draw into.
        s.frameRow = rowSetup<GSMem::C32>;
        s.frameStore = PixelStore::Word32;
        s.scissorX1 = s.scissorX0 - 1;
    }

    s.zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    s.zmsk = ctx.zbuf.zmask;
    s.ztst = static_cast<uint8_t>((ctx.test >> 17) & 3u);
    if (!pixelStoreFor(ctx.zbuf.psm, s.zRow, s.zStore))
    {
        s.zRow = rowSetup<GSMem::Z32>;
        s.zStore = PixelStore::Word32;
    }

    s.ate = (ctx.test & 0x1u) != 0u;
    s.atst = static_cast<uint8_t>((ctx.test >> 1) & 0x7u);
    s.aref = static_cast<uint8_t>((ctx.test >> 4) & 0xFFu);
    s.afail = static_cast<uint8_t>((ctx.test >> 12) & 0x3u);
    s.date = ((ctx.test >> 14) & 0x1u) != 0u;
    s.datm = ((ctx.test >> 15) & 0x1u) != 0u;
    // RGB24 has no destination alpha, so DATE always passes there.
    s.dateReadsFrame = s.date && (s.fpsm == GS_PSM_CT32 || s.fpsm == GS_PSM_CT16 || s.fpsm == GS_PSM_CT16S);

    s.abe = state.prim.abe;
    s.pabe = state.pabe;
    s.asel = static_cast<uint8_t>(ctx.alpha & 3u);
    s.bsel = static_cast<uint8_t>((ctx.alpha >> 2) & 3u);
    s.csel = static_cast<uint8_t>((ctx.alpha >> 4) & 3u);
    s.dsel = static_cast<uint8_t>((ctx.alpha >> 6) & 3u);
    s.fix = static_cast<uint8_t>((ctx.alpha >> 32) & 0xFFu);
    s.fge = state.prim.fge;
    s.fogR = state.fogR;
    s.fogG = state.fogG;
    s.fogB = state.fogB;

    GSTexSetup &t = s.tex;
    t = GSTexSetup{};
    t.enabled = state.prim.tme;
    if (!t.enabled)
        return;
    const auto &tex = ctx.tex0;
    t.linear = state.linearFilter;
    t.fst = state.prim.fst;
    t.psm = tex.psm;
    t.tfx = tex.tfx;
    t.tcc = tex.tcc != 0u;
    t.w = state.textureWidth;
    t.h = state.textureHeight;
    t.tbp0 = tex.tbp0;
    t.tbw = tex.tbw;
    t.wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    t.wrapV = static_cast<uint8_t>((ctx.clamp >> 2) & 0x3u);
    t.minU = static_cast<int>((ctx.clamp >> 4) & 0x3FFu);
    t.maxU = static_cast<int>((ctx.clamp >> 14) & 0x3FFu);
    t.minV = static_cast<int>((ctx.clamp >> 24) & 0x3FFu);
    t.maxV = static_cast<int>((ctx.clamp >> 34) & 0x3FFu);
    t.texa = state.texa;
    t.vram = m_vram;
    uint8_t (*indexFetch)(const GSTexSetup &, int, int) = nullptr;
    if (!texelFetchFor(tex.psm, t.fetch, indexFetch))
    {
        t.fetch = fetchTexelUnsupported;
        return;
    }
    if (indexFetch)
        t.palette = DecodedPalette(state);
    if (!LookupDecodedTexture(t, frameTarget, zTarget, indexFetch) && gsStatsEnabled())
        statAdd(s_rasterStats.directTextureDraws);
}

// The CLUT buffer as 256 RGBA8888 entries (TEXA applied) for the draw's CPSM/CSM/CSA and texture
// format; rebuilt only when one of those or the buffer itself changed.
const uint32_t *GSCpuBackend::DecodedPalette(const GSDrawState &state)
{
    const auto &tex = state.context.tex0;
    const bool fourBit = isFourBitIndexedPsm(tex.psm);
    const uint64_t key = static_cast<uint64_t>(tex.cpsm) | (static_cast<uint64_t>(tex.csm) << 8) |
                         (static_cast<uint64_t>(tex.csa) << 16) | (static_cast<uint64_t>(fourBit) << 24) |
                         (static_cast<uint64_t>(state.texa.ta0) << 32) | (static_cast<uint64_t>(state.texa.ta1) << 40) |
                         (static_cast<uint64_t>(state.texa.aem) << 48) | (static_cast<uint64_t>(m_clutGeneration) << 49);
    if (key == m_paletteKey)
        return m_palette.data();
    m_paletteKey = key;

    const bool sixteenBit = tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(tex.csa) & csaMask) << 4u;
    for (uint32_t index = 0; index < 256u; ++index)
    {
        const uint32_t sourceIndex = fourBit ? (index & 0x0Fu) : index;
        uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (!sixteenBit && tex.csm == 0u && !fourBit)
        {
            const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
            clutIndex = block + (sourceIndex & 0x0Fu);
        }
        uint32_t colour = 0xFFFF00FFu;
        switch (tex.cpsm)
        {
        case GS_PSM_CT32:
            colour = applyTexa(state.texa, tex.cpsm,
                               static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u));
            break;
        case GS_PSM_CT24:
            colour = applyTexa(state.texa, tex.cpsm,
                               (static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u)) & 0x00FFFFFFu);
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            colour = applyTexa(state.texa, tex.cpsm, Rgba5551ToRgba8888(m_clut[clutIndex]));
            break;
        default:
            break;
        }
        m_palette[index] = colour;
    }
    return m_palette.data();
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch, const GSRasterSetup &s)
{
    if (!s.tex.enabled)
        drawSpriteImpl<0>(batch, s);
    else if (s.tex.fst)
        drawSpriteImpl<1>(batch, s);
    else
        drawSpriteImpl<2>(batch, s);
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch, const GSRasterSetup &s)
{
    // Gouraud with the same colour at the three vertices is that colour everywhere (the exact
    // DDA would say so too): the flat path, which skips four interpolators per pixel.
    const auto &v0 = batch.vertices[0], &v1 = batch.vertices[1], &v2 = batch.vertices[2];
    const bool gouraud = batch.state.prim.iip &&
                         !(v0.r == v1.r && v1.r == v2.r && v0.g == v1.g && v1.g == v2.g &&
                           v0.b == v1.b && v1.b == v2.b && v0.a == v1.a && v1.a == v2.a);
    if (gouraud)
        drawTriangleTex<true>(batch, s);
    else
        drawTriangleTex<false>(batch, s);
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch, const GSRasterSetup &s)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        writePixelAt(s, x0, y0, static_cast<u32>(z), pack32(r, g, b, a), fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    // PS2X_GS_PRIM_UNIQUE=1: log the first primitive of every distinct draw setup (type, texture,
    // CLUT, target), however many draws there are; the plain [gs:prim] log stops after 64.
    static const bool s_primUnique = []
    {
        const char *value = std::getenv("PS2X_GS_PRIM_UNIQUE");
        return value && value[0] == '1';
    }();
    if (s_primUnique)
    {
        static std::mutex s_seenMutex;
        static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t>> s_seen;
        const auto key = std::make_tuple(static_cast<uint32_t>(state.prim.type) | (static_cast<uint32_t>(state.prim.tme) << 4) |
                                             (static_cast<uint32_t>(state.prim.fst) << 5) | (static_cast<uint32_t>(state.prim.abe) << 6),
                                         static_cast<uint32_t>(ctx.tex0.tbp0), static_cast<uint32_t>(ctx.tex0.tbw),
                                         static_cast<uint32_t>(ctx.tex0.psm), static_cast<uint32_t>(ctx.tex0.cbp),
                                         static_cast<uint32_t>(ctx.frame.fbp), static_cast<uint32_t>(ctx.tex0.tfx) | (static_cast<uint32_t>(ctx.tex0.tcc) << 4),
                                         static_cast<uint64_t>(ctx.test) | (static_cast<uint64_t>(ctx.zbuf.zbp) << 32) |
                                             (static_cast<uint64_t>(ctx.zbuf.psm) << 48) | (static_cast<uint64_t>(ctx.zbuf.zmask) << 56));
        bool fresh = false;
        {
            std::lock_guard<std::mutex> lock(s_seenMutex);
            fresh = s_seen.insert(key).second;
        }
        if (fresh)
            std::cout << "[gs:prim-new] type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme) << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe) << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp << " fbw=" << ctx.frame.fbw << std::hex
                      << " tbp0=0x" << ctx.tex0.tbp0 << std::dec << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << std::hex << " psm=0x" << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw) << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc) << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << std::hex << " cbp=0x" << ctx.tex0.cbp << " cpsm=0x" << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << std::hex << " test=0x" << ctx.test << " alpha=0x" << ctx.alpha << " clamp=0x" << ctx.clamp << std::dec
                      << " zbuf=(zbp=" << ctx.zbuf.zbp << std::hex << " psm=0x" << static_cast<uint32_t>(ctx.zbuf.psm) << std::dec
                      << " zmsk=" << static_cast<uint32_t>(ctx.zbuf.zmask) << ") z0=" << static_cast<uint32_t>(batch.vertices[0].z)
                      << " ofs=(" << (ctx.xyoffset.ofx >> 4) << "," << (ctx.xyoffset.ofy >> 4) << ")"
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ") uv0=(" << batch.vertices[0].u / 16.0f << "," << batch.vertices[0].v / 16.0f << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ") uv1=(" << batch.vertices[1].u / 16.0f << "," << batch.vertices[1].v / 16.0f << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << "," << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << "," << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << std::endl;
    }
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    // Pixel bounding box of the primitive within the scissor (one pixel of slack): nothing to
    // draw outside it, and the frame / Z pages under it are stamped as written so that decoded
    // textures reading them are refreshed (and not used by this draw itself).
    int64_t bx0 = INT64_MAX, bx1 = INT64_MIN, by0 = INT64_MAX, by1 = INT64_MIN;
    for (uint32_t i = 0; i < std::min<uint32_t>(batch.vertexCount, 3u); ++i)
    {
        const int64_t vx = fixed16(batch.vertices[i].x) - static_cast<int64_t>(ctx.xyoffset.ofx);
        const int64_t vy = fixed16(batch.vertices[i].y) - static_cast<int64_t>(ctx.xyoffset.ofy);
        bx0 = std::min(bx0, vx);
        bx1 = std::max(bx1, vx);
        by0 = std::min(by0, vy);
        by1 = std::max(by1, vy);
    }
    bx0 = std::max<int64_t>(floorDiv16(bx0) - 1, ctx.scissor.x0);
    bx1 = std::min<int64_t>(ceilDiv16(bx1) + 1, ctx.scissor.x1);
    by0 = std::max<int64_t>(floorDiv16(by0) - 1, ctx.scissor.y0);
    by1 = std::min<int64_t>(ceilDiv16(by1) + 1, ctx.scissor.y1);
    if (bx0 > bx1 || by0 > by1)
        return;
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const GSPageRange frameTarget = pageRangeFor(ctx.frame.psm, GSInternal::framePageBaseToBlock(ctx.frame.fbp), fbw,
                                                 static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
                                                 static_cast<uint32_t>(bx1), static_cast<uint32_t>(by1));
    StampPages(frameTarget);
    GSPageRange zTarget; // empty unless Z is written
    if (!ctx.zbuf.zmask)
    {
        zTarget = pageRangeFor(ctx.zbuf.psm, GSInternal::framePageBaseToBlock(ctx.zbuf.zbp), fbw,
                               static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
                               static_cast<uint32_t>(bx1), static_cast<uint32_t>(by1));
        StampPages(zTarget);
    }

    GSRasterSetup setup;
    BuildRasterSetup(state, setup, frameTarget, zTarget);
    const bool stats = gsStatsEnabled();
    if (stats)
        s_statsReporter.maybeReport();
    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        if (stats)
            statAdd(s_rasterStats.sprites);
        DrawSprite(batch, setup);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        if (stats)
            statAdd(s_rasterStats.triangles);
        DrawTriangle(batch, setup);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        if (stats)
            statAdd(s_rasterStats.lines);
        DrawLine(batch, setup);
        break;
    case GS_PRIM_POINT:
    {
        if (stats)
            statAdd(s_rasterStats.points);
        const GSVertex &v = batch.vertices[0];
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        writePixelAt(setup, px, py, static_cast<u32>(v.z), pack32(v.r, v.g, v.b, v.a), v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 0u || command.direction == 2u)
    {
        // Pages the transfer writes: decoded textures reading them are refreshed.
        const uint32_t rrw = std::max<uint32_t>(command.trxreg.rrw, 1u);
        const uint32_t rrh = std::max<uint32_t>(command.trxreg.rrh, 1u);
        StampPages(pageRangeFor(command.bitbltbuf.dpsm, command.bitbltbuf.dbp, std::max<uint32_t>(command.bitbltbuf.dbw, 1u),
                                command.trxpos.dsax, command.trxpos.dsay, command.trxpos.dsax + rrw - 1u,
                                command.trxpos.dsay + rrh - 1u));
    }
    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    StampPages(pageRangeFor(context.frame.psm, fbp, fbw, x0, y0, x1, y1));
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory, then perform the expensive display conversion without holding
    // the producer-side raster lock. The copy takes the backend lock per 64 KiB slice, not for
    // the whole 4 MiB: the presenter latches once per VBlank at an arbitrary point of the
    // guest's frame, and one long hold (a 4 MiB memcpy plus the wait to get the lock between
    // primitives) stalled the EE thread's next primitive for up to a millisecond, every frame.
    // On the console the display read-out never stalls drawing. A picture copied while the
    // guest draws can mix primitives across slices; it could mix them across the whole copy
    // before as well (the latch has never been synchronised with the guest's frame), and the
    // displayed buffer is normally the one not being drawn.
    thread_local std::vector<uint8_t> snapshot;
    {
        constexpr size_t kSlice = 64u * 1024u;
        size_t size = 0u;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size = (m_vram && m_vramSize != 0u) ? m_vramSize : 0u;
        }
        snapshot.resize(size);
        for (size_t offset = 0u; offset < size; offset += kSlice)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_vram || m_vramSize != size)
            {
                snapshot.clear();
                break;
            }
            std::memcpy(snapshot.data() + offset, m_vram + offset, std::min(kSlice, size - offset));
        }
    }
    if (snapshot.empty())
    {
        static uint32_t s_emptySnapshotLogCount = 0u;
        if (s_emptySnapshotLogCount++ < 8u)
            std::cerr << "[gs:present:none] why=empty-vram-snapshot tick=" << request.vsyncTick << std::endl;
        return {};
    }

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    PresentationFrame frame = snapshotBackend.PresentFromLocalMemory(request);
    maybeDumpGsMemory(request, snapshot, frame);
    return frame;
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    // SMODE2 FFMD = 0 is the GS's FIELD mode (each field reads every other buffer line, the
    // buffer is frame-high): shown one field per VBlank. FFMD = 1 (FRAME mode, field-high
    // buffer) is handled by the line repeat in decodeDisplayGeometry/expandDisplayRows.
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    const GSDisplayGeometry geom1 = decodeDisplayGeometry(request.display1, request.smode2);
    const GSDisplayGeometry geom2 = decodeDisplayGeometry(request.display2, request.smode2);
    const uint32_t width1 = geom1.width, height1 = geom1.outputRows;
    const uint32_t width2 = geom2.width, height2 = geom2.outputRows;
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    // Diagnostics for "nothing presented" (the runner shows magenta): say why, a few times.
    static uint32_t s_presentFailLogCount = 0u;
    static std::string s_lastPresentFailKey;
    auto logPresentFailure = [&](const char *why)
    {
        // Log whenever the reason or the display registers change (first 64 changes).
        if (s_presentFailLogCount >= 64u)
            return;
        const std::string key = std::string(why) + "|" + std::to_string(request.pmode) + "|" +
                                std::to_string(request.dispfb1) + "|" + std::to_string(request.display1) + "|" +
                                std::to_string(request.dispfb2) + "|" + std::to_string(request.display2);
        if (key != s_lastPresentFailKey && s_presentFailLogCount < 64u)
        {
            s_lastPresentFailKey = key;
            std::cerr << "[gs:present:none] why=" << why
                      << " pmode=0x" << std::hex << request.pmode
                      << " dispfb1=0x" << request.dispfb1 << " display1=0x" << request.display1
                      << " dispfb2=0x" << request.dispfb2 << " display2=0x" << request.display2
                      << std::dec << " crt1=" << pmode.enableCrt1 << " crt2=" << pmode.enableCrt2
                      << " fb1=(fbp=0x" << std::hex << displayFrame1.fbp << " fbw=" << std::dec << displayFrame1.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(displayFrame1.psm) << std::dec
                      << " x=" << origin1.x << " y=" << origin1.y << ") size1=" << width1 << "x" << height1
                      << " vram=" << m_vramSize << " tick=" << request.vsyncTick << std::endl;
            ++s_presentFailLogCount;
        }
    };
    if (!valid1 && !valid2)
    {
        logPresentFailure("no-crt-enabled-or-no-display-setup");
        return result;
    }

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          const GSDisplayGeometry &geom,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        const uint32_t width = geom.width;
        const uint32_t height = geom.sourceRows;
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, height) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        expandDisplayRows(pixels, width, height, geom.outputRows);
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, geom1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, geom2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    const GSDisplayGeometry &geom = valid1 ? geom1 : geom2;
    result.width = geom.width;
    result.height = geom.outputRows;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, geom, true, false, selected, result.pixels, result.usedPreferred))
    {
        logPresentFailure("copy-frame-failed");
        return {};
    }
    if (fieldMode)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
