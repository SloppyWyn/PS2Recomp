#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "ps2x/state_stream.h"

namespace ps2x::iop::detail
{
    class IopEmulator
    {
    public:
        explicit IopEmulator(IopHost &host);
        ~IopEmulator();

        IopEmulator(const IopEmulator &) = delete;
        IopEmulator &operator=(const IopEmulator &) = delete;

        void reset();

        // Snapshots (docs/SNAPSHOT-DESIGN.md).
        void saveState(ps2x::StateWriter &w) const;
        void loadState(ps2x::StateReader &r);
        // True when no IOP code is executing (between runEeCycles / RPC calls).
        [[nodiscard]] bool quiescent(std::string *why) const;
        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result);
        void runEeCycles(uint64_t eeCycles) noexcept;
        // EE cycles that can pass before the IOP has anything to do (a runnable thread: 0,
        // nothing scheduled at all: UINT64_MAX): the batching bound for runEeCycles.
        [[nodiscard]] uint64_t eeCyclesUntilNextEvent() const noexcept;
        // Batched EE timing: thread slices inside runEeCycles stop at the next scheduled
        // IOP event (what one-cycle calls did implicitly). Off = the historical behaviour.
        void setBatchedTiming(bool enabled) noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        [[nodiscard]] bool hasRpcServer(uint32_t sid) const noexcept;
        void onSifTransfer(const SifTransfer &transfer);

        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] uint64_t cycles() const noexcept;
        [[nodiscard]] uint64_t instructions() const noexcept;
        [[nodiscard]] uint32_t loadedModuleCount() const noexcept;
        [[nodiscard]] uint32_t threadCount() const noexcept;
        [[nodiscard]] uint32_t rpcServerCount() const noexcept;

        // SPU2 mixed output (Spu2::setOutputSink): 48 kHz interleaved stereo frames.
        void setAudioOutput(std::function<void(const int16_t *, size_t)> sink);
        void flushAudioOutput();

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
