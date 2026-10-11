#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <mutex>
#include <vector>

struct GSRasterSetup;
struct GSTexSetup;

// A range of 8 KB local-memory pages: count pages from first, modulo 512 (addresses wrap at
// 4 MB); count 0 is empty, 512 is all of local memory.
struct GSPageRange
{
    static constexpr uint32_t kPages = 512u;
    uint32_t first = 0;
    uint32_t count = 0;
};

// A texture decoded to a linear layout for sampling (see GSCpuBackend::LookupDecodedTexture).
struct GSDecodedTexture
{
    uint64_t key = 0;
    uint64_t writeStamp = 0;
    uint64_t lastUse = 0;
    std::vector<uint8_t> indices; // CLUT formats: one index per texel
    std::vector<uint32_t> rgba;   // other formats: RGBA8888, TEXA applied
};

class GSCpuBackend final : public GSRasterBackend
{
    friend struct PS2SnapshotAccess;

public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void BuildRasterSetup(const GSDrawState &state, GSRasterSetup &setup, const GSPageRange &frameTarget, const GSPageRange &zTarget);
    const uint32_t *DecodedPalette(const GSDrawState &state);
    bool LookupDecodedTexture(GSTexSetup &setup, const GSPageRange &frameTarget, const GSPageRange &zTarget,
                              uint8_t (*indexFetch)(const GSTexSetup &, int, int));
    void StampPages(const GSPageRange &range);
    void StampAllPages();
    void DrawSprite(const GSPrimitiveBatch &batch, const GSRasterSetup &setup);
    void DrawTriangle(const GSPrimitiveBatch &batch, const GSRasterSetup &setup);
    void DrawLine(const GSPrimitiveBatch &batch, const GSRasterSetup &setup);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    // The CLUT decoded to RGBA8888 for the last indexed draw (see DecodedPalette).
    std::array<uint32_t, 256> m_palette{};
    uint64_t m_paletteKey = ~0ull;
    uint32_t m_clutGeneration = 0;
    // Decoded textures and, per local-memory page, the m_writeStamp of its last write.
    std::vector<GSDecodedTexture> m_decodedTextures;
    std::array<uint64_t, 512> m_pageWriteStamp{};
    uint64_t m_writeStamp = 1;
    uint64_t m_textureUseCounter = 0;
    GSMem::TexturePageCache m_texturePageCache;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
