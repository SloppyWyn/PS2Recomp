#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"
#include "ps2x/state_stream.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    class IopSubsystem
    {
    public:
        explicit IopSubsystem(IopHost &host);
        ~IopSubsystem();

        IopSubsystem(const IopSubsystem &) = delete;
        IopSubsystem &operator=(const IopSubsystem &) = delete;
        IopSubsystem(IopSubsystem &&) noexcept;
        IopSubsystem &operator=(IopSubsystem &&) noexcept;

        void reset();

        // Snapshots (docs/SNAPSHOT-DESIGN.md).
        void saveState(ps2x::StateWriter &w) const;
        void loadState(ps2x::StateReader &r);
        [[nodiscard]] bool quiescent(std::string *why) const;

        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result = nullptr);
        void runEeCycles(uint64_t eeCycles) noexcept;
        // EE cycles the IOP can be left alone for before it has work (0 = a thread is
        // runnable now, UINT64_MAX = nothing scheduled). Lets the EE batch runEeCycles.
        [[nodiscard]] uint64_t eeCyclesUntilNextEvent() const noexcept;
        // With batched EE timing, runEeCycles arrives in chunks: thread slices then stop at
        // the next scheduled IOP event, as the one-cycle calls did implicitly.
        void setBatchedTiming(bool enabled) noexcept;

        [[nodiscard]] RpcAbi selectRpcAbi(const RpcAbiRequest &request) const;
        [[nodiscard]] bool canBindRpc(uint32_t sid) const noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        void onSifTransfer(const SifTransfer &transfer);

        // Physical IOP RAM access shared by the emulator, SIF DMA, and HLE services. Addresses are IOP addresses.
        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] DebugSnapshot debugSnapshot() const;

        // SPU2 mixed output: 48 kHz interleaved 16-bit stereo frames, handed over in batches
        // on the thread that runs the IOP. A sink turns the SPU2 mixer on; null turns it off.
        // The sink must not block (it runs inside the emulation).
        using AudioOutputSink = std::function<void(const int16_t *frames, size_t frameCount)>;
        void setAudioOutput(AudioOutputSink sink);
        // Hands over the frames mixed so far (before closing a WAV, at shutdown).
        void flushAudioOutput();

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
