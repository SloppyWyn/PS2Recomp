#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_call_wait.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;
        constexpr int kSpu2Irq = 9;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            memory.bindClock(&totalCycles);
            kernel.setCallContextWait([this](const std::function<bool()> &ready) { return waitInCallContextLogged(ready); });
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            rpcAheadCycles = 0;
            rpcDrains = 0;
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        // SPU2 interrupts (DMA channel completion, IRQA hit) raised by the model since the
        // last call: dispatched like the other hardware interrupts, at the current cycle.
        void schedulePendingDma()
        {
            const uint32_t irqs = memory.spu2().takePendingIrqs();
            if (irqs == 0u)
                return;
            if (irqs & Spu2::IrqSpu2)
                pendingDmaInterrupts[kSpu2Irq] = totalCycles;
            if (irqs & Spu2::IrqDmaCore0)
                pendingDmaInterrupts[kDmaSpu0Irq] = totalCycles;
            if (irqs & Spu2::IrqDmaCore1)
                pendingDmaInterrupts[kDmaSpu1Irq] = totalCycles;
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        // PS2X_IOP_TRACE_IMPORTS=<lib,lib,...|*> logs every import call of those libraries
        // (library:ordinal, a0..a3, pc) on stderr as [iop:import], up to
        // PS2X_IOP_TRACE_MAX lines (default 20000). Free when unset.
        static const std::vector<std::string> &tracedImportLibraries()
        {
            static const std::vector<std::string> s_libs = [] {
                std::vector<std::string> libs;
                const char *value = std::getenv("PS2X_IOP_TRACE_IMPORTS");
                if (!value || !*value)
                    return libs;
                std::string item;
                for (const char *p = value;; ++p)
                {
                    if (*p == ',' || *p == '\0')
                    {
                        if (!item.empty())
                            libs.push_back(item);
                        item.clear();
                        if (*p == '\0')
                            break;
                    }
                    else
                        item.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
                }
                return libs;
            }();
            return s_libs;
        }

        void traceImport(const IopImportCall &call, const CpuState &cpu)
        {
            const auto &libs = tracedImportLibraries();
            if (libs.empty())
                return;
            std::string lib(call.library);
            for (char &ch : lib)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            bool match = false;
            for (const std::string &item : libs)
                if (item == "*" || item == lib)
                    match = true;
            if (!match)
                return;
            static int s_lines = 0;
            static const int s_max = [] {
                const char *value = std::getenv("PS2X_IOP_TRACE_MAX");
                return value && *value ? std::atoi(value) : 20000;
            }();
            if (s_lines >= s_max)
                return;
            ++s_lines;
            std::fprintf(stderr, "[iop:import] %s:%u a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X pc=0x%08X cyc=%llu\n",
                         lib.c_str(), static_cast<unsigned>(call.ordinal), cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                         cpu.gpr[7], cpu.pc, static_cast<unsigned long long>(totalCycles));
        }

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            traceImport(call, cpu);
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (const auto import = imports.decode(cpu.pc))
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            if (!cpu.stopped)
            {
                // The function did not return: its budget ran out (v0 is whatever it held) or
                // it blocked (a wait reached while a thread was current). Both are anomalies.
                static int s_lines = 0;
                if (s_lines < 32)
                {
                    ++s_lines;
                    std::fprintf(stderr, "[iop:call] fn=0x%X did not return (%s) budget=%u pc=0x%X depth=%u cyc=%llu\n",
                                 address, cpu.yielded ? "blocked" : "budget", budget, cpu.pc, depth,
                                 static_cast<unsigned long long>(totalCycles));
                }
            }
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // IopGuestExecutor: an EE RPC request is about to be served by a server whose thread has
        // priority `priority`. The RPC bridge calls server functions directly, at whatever
        // point the IOP's last time slice ended; a thread of higher priority may be preempted
        // in the middle of its work there (our slices end on a cycle budget, the hardware's only
        // when the thread blocks or a higher-priority thread is readied). Its state would be
        // seen half-updated, and what the server changes can break the work that thread resumes
        // (SRWZ: ezmidi's stop freed modhsyn voices while ezmidi's tick thread, priority above
        // the server's, was preempted in the middle of modhsyn's voice-list walk; the walk then
        // never got back to the list head, the tick never ended, and the EE polled forever).
        // So run, before the server, every ready thread that would run before the server's
        // thread on the hardware (priority <= the server's), until none is ready, servicing due
        // interrupts and timers between slices as runCycles does. Idle time is not skipped.
        // The IOP time this takes is repaid from the next EE time slices (rpcAheadCycles), so
        // the IOP clock stays in step with the EE's. Capped at kCallContextWaitLimitCycles per
        // request; PS2X_IOP_RPC_PRIORITY=0 turns it off (A/B).
        void runThreadsBeforeServer(uint32_t priority) override
        {
            static const bool s_enabled = [] {
                const char *value = std::getenv("PS2X_IOP_RPC_PRIORITY");
                return !(value && *value == '0');
            }();
            if (!s_enabled || priority == 0u || activeCpu != nullptr || callDepth != 0u ||
                servicingDmaInterrupts || servicingGuestCallbacks)
                return;
            const uint64_t start = totalCycles;
            const uint64_t limit = start + kCallContextWaitLimitCycles;
            uint32_t slices = 0u;
            std::string first;
            while (totalCycles < limit)
            {
                memory.spu2().catchUp();
                schedulePendingDma();
                servicePendingDmaInterrupts();
                servicePendingGuestCallbacks();
                timrman.serviceDue(totalCycles, *this);
                IopThread *next = kernel.beginNextReadyBefore(totalCycles, priority);
                if (!next)
                    break;
                if (slices++ == 0u)
                {
                    char item[48];
                    std::snprintf(item, sizeof(item), "%d:%u:%X", next->id, next->priority, next->cpu.pc);
                    first = item;
                }
                const uint64_t before = totalCycles;
                uint64_t budget = std::min<uint64_t>(kDefaultSlice, limit - totalCycles);
                if (clipSlicesToEvents)
                {
                    // As in runCycles: do not run a slice past a due timer/interrupt.
                    const uint64_t nextEvent = nextScheduledEventCycle(limit);
                    if (nextEvent > totalCycles)
                        budget = std::min(budget, nextEvent - totalCycles);
                    budget = std::max<uint64_t>(budget, 1u);
                }
                runCpu(next->cpu, static_cast<uint32_t>(budget));
                kernel.endTimeslice(*next, kThreadReturnSentinel);
                if (totalCycles == before)
                    ++totalCycles;
            }
            if (slices == 0u)
                return;
            const uint64_t spent = totalCycles - start;
            rpcAheadCycles += spent;
            ++rpcDrains;
            // PS2X_IOP_RPC_LOG: the first 64 drains; the cap is always reported.
            static int s_lines = 0;
            static const bool s_log = [] {
                const char *value = std::getenv("PS2X_IOP_RPC_LOG");
                return value && *value && *value != '0';
            }();
            const bool capped = totalCycles >= limit;
            if ((s_log || capped) && s_lines < 64)
            {
                ++s_lines;
                std::fprintf(stderr, "[iop:rpc:before] prio=%u first=%s slices=%u cycles=%llu%s drains=%llu\n", priority,
                             first.c_str(), slices, static_cast<unsigned long long>(spent), capped ? " CAPPED" : "",
                             static_cast<unsigned long long>(rpcDrains));
            }
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        // The next cycle at which a sleeping IOP has something to do (a delayed thread, a DMA
        // completion, a scheduled callback, a timer, the SPU2), or fallback when nothing is
        // scheduled.
        // IopKernel::CallContextWait: a WaitSema/WaitEventFlag outside any IOP thread (an RPC
        // server function the bridge calls directly, e.g. sdrdrv -> libsd clearing the reverb
        // work area chunk by chunk, each waiting for its DMA interrupt). See core/iop_call_wait.h:
        // IOP time moves from one scheduled hardware event (DMA/SPU2 completion, scheduled
        // callback, timer) to the next while `ready` does not hold; threads do not run meanwhile.
        // Nothing scheduled, or a wait reached while interrupts/callbacks are being serviced:
        // gives up at once. Every give-up is logged ([iop:callwait] gave-up ...), the first 32
        // satisfied waits too.
        bool waitInCallContextLogged(const std::function<bool()> &ready)
        {
            CallWaitHooks hooks;
            hooks.reentrant = [this] { return servicingDmaInterrupts || servicingGuestCallbacks; };
            hooks.service = [this] {
                memory.spu2().catchUp();
                schedulePendingDma();
                servicePendingDmaInterrupts();
                servicePendingGuestCallbacks();
                timrman.serviceDue(totalCycles, *this);
            };
            hooks.now = [this] { return totalCycles; };
            hooks.nextEvent = [this](uint64_t limit, uint64_t &next, bool &transfer) {
                // Hardware events only: a delayed thread's wake time says nothing here.
                bool any = false;
                for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                {
                    next = std::min(next, completionCycle);
                    any = transfer = true;
                }
                if (!pendingGuestCallbacks.empty())
                {
                    next = std::min(next, pendingGuestCallbacks.begin()->first);
                    any = true;
                }
                const uint64_t timer = timrman.nextEventCycle(limit);
                if (timer < limit)
                {
                    next = std::min(next, timer);
                    any = true;
                }
                if (memory.spu2().dmaActive(0u) || memory.spu2().dmaActive(1u))
                {
                    next = std::min(next, memory.spu2().nextEventCycle(limit));
                    any = transfer = true;
                }
                return any;
            };
            hooks.advanceTo = [this](uint64_t cycle) { totalCycles = cycle; };
            const uint64_t start = totalCycles;
            const CallWaitResult result = waitInCallContext(hooks, ready, kCallContextWaitLimitCycles);
            static int s_okLines = 0;
            if (result != CallWaitResult::Ready || s_okLines < 32)
            {
                if (result == CallWaitResult::Ready)
                    ++s_okLines;
                std::fprintf(stderr, "[iop:callwait] %s cycles=%llu at=%llu\n", callWaitResultName(result),
                             static_cast<unsigned long long>(totalCycles - start), static_cast<unsigned long long>(totalCycles));
            }
            return result == CallWaitResult::Ready;
        }

        uint64_t nextScheduledEventCycle(uint64_t fallback) const noexcept
        {
            uint64_t nextWake = kernel.nextWakeCycle(fallback);
            for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                nextWake = std::min(nextWake, completionCycle);
            if (!pendingGuestCallbacks.empty())
                nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
            nextWake = timrman.nextEventCycle(nextWake);
            return memory.spu2().nextEventCycle(nextWake);
        }

        // PS2X_IOP_WATCH=<hex addr[,addr...]> prints those IOP RAM words (32-bit) once per
        // second of IOP time on stderr as [iop:watch], for following a driver's state
        // variables without a debugger. Free when unset.
        void serviceWatch()
        {
            static const std::vector<uint32_t> s_addresses = [] {
                std::vector<uint32_t> list;
                const char *value = std::getenv("PS2X_IOP_WATCH");
                if (!value || !*value)
                    return list;
                const char *p = value;
                while (*p)
                {
                    char *end = nullptr;
                    const unsigned long parsed = std::strtoul(p, &end, 16);
                    if (end == p)
                        break;
                    list.push_back(static_cast<uint32_t>(parsed));
                    p = end;
                    while (*p == ',' || *p == ' ')
                        ++p;
                }
                return list;
            }();
            if (s_addresses.empty() || totalCycles < watchNextCycle)
                return;
            watchNextCycle = totalCycles + kIopClockHz;
            std::string line = "[iop:watch] cyc=" + std::to_string(totalCycles);
            char item[48];
            for (const uint32_t address : s_addresses)
            {
                std::snprintf(item, sizeof(item), " %05X=%08X", address, memory.read32(address));
                line += item;
            }
            line += '\n';
            std::fputs(line.c_str(), stderr);
            // PS2X_IOP_WATCH_THREADS=1 (with PS2X_IOP_WATCH): IOP threads, semaphores, event
            // flags and timers on the same cadence, as [iop:watch:thr] / [iop:watch:sync].
            static const bool s_threads = [] {
                const char *value = std::getenv("PS2X_IOP_WATCH_THREADS");
                return value && *value && *value != '0';
            }();
            if (s_threads)
            {
                const std::string threads = "[iop:watch:thr] " + kernel.describeThreads(false) + " | " +
                                            timrman.describe(totalCycles) + " | pend=" +
                                            std::to_string(pendingDmaInterrupts.size()) + "/" +
                                            std::to_string(pendingGuestCallbacks.size()) + "\n";
                std::fputs(threads.c_str(), stderr);
                const std::string sync = "[iop:watch:sync] " + kernel.describeSyncObjects() + "\n";
                std::fputs(sync.c_str(), stderr);
            }
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                serviceWatch();
                while (totalCycles < target)
                {
                    memory.spu2().catchUp();
                    schedulePendingDma();
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        const uint64_t nextWake = nextScheduledEventCycle(target);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    uint64_t budget = std::min<uint64_t>(kDefaultSlice, target - totalCycles);
                    if (clipSlicesToEvents)
                    {
                        // Batched EE timing runs the IOP in chunks instead of one cycle per
                        // guest call: keep a thread's slice from running past an event that
                        // the one-cycle calls would have serviced on time.
                        const uint64_t nextEvent = nextScheduledEventCycle(target);
                        if (nextEvent > totalCycles)
                            budget = std::min(budget, nextEvent - totalCycles);
                        budget = std::max<uint64_t>(budget, 1u);
                    }
                    runCpu(next->cpu, static_cast<uint32_t>(budget));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " size=0x" << modules[result.moduleId].size
                << " start=" << std::dec << result.startResult
                << " name=" << modules[result.moduleId].name;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            // SCE SDK callers pass an IOP address (the IRX was copied there by SIF DMA);
            // homebrew-style callers pass an EE buffer. Try IOP memory first.
            if (!IopModuleLoader::readElfFromIop(memory, guestAddress, image) &&
                !IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        void saveState(ps2x::StateWriter &w) const
        {
            memory.saveState(w);
            kernel.saveState(w);
            cdvd.saveState(w);
            rpc.saveState(w);
            intrman.saveState(w);
            timrman.saveState(w);
            ioman.saveState(w);
            imports.saveState(w);
            w.size(modules.size());
            for (const auto &[id, module] : modules)
            {
                w.i32(module.id);
                w.str(module.path);
                w.str(module.name);
                w.u32(module.base);
                w.u32(module.size);
                w.u32(module.entry);
                w.u32(module.gp);
                w.boolean(module.resident);
            }
            w.podMap(pendingDmaInterrupts);
            w.size(pendingGuestCallbacks.size());
            for (const auto &[cycle, callback] : pendingGuestCallbacks)
            {
                w.u64(cycle);
                w.pod(callback);
            }
            w.u32(nextModuleId);
            w.u32(moduleCursor);
            w.u64(totalCycles);
            w.u64(totalInstructions);
            w.u64(eeCycleCarry);
            w.u64(rpcAheadCycles);
            w.pod(secrMcCommandHandler);
            w.pod(secrMcDevIdHandler);
            w.pod(checkKelfPathCallback);
        }

        void loadState(ps2x::StateReader &r)
        {
            memory.loadState(r);
            kernel.loadState(r);
            cdvd.loadState(r);
            rpc.loadState(r);
            intrman.loadState(r);
            timrman.loadState(r);
            ioman.loadState(r);
            imports.loadState(r);
            modules.clear();
            const size_t moduleCount = r.size();
            for (size_t i = 0; i < moduleCount; ++i)
            {
                Module module;
                module.id = r.i32();
                module.path = r.str();
                module.name = r.str();
                module.base = r.u32();
                module.size = r.u32();
                module.entry = r.u32();
                module.gp = r.u32();
                module.resident = r.boolean();
                modules[module.id] = std::move(module);
            }
            r.podMap(pendingDmaInterrupts);
            pendingGuestCallbacks.clear();
            const size_t callbackCount = r.size();
            for (size_t i = 0; i < callbackCount; ++i)
            {
                const uint64_t cycle = r.u64();
                pendingGuestCallbacks.emplace(cycle, r.pod<ScheduledGuestCallback>());
            }
            nextModuleId = r.u32();
            moduleCursor = r.u32();
            // Snapshots from before the shared pool kept module images outside the allocator, with no
            // allocation records: claim each saved module's image so the pool does not hand it out.
            for (const auto &[id, module] : modules)
            {
                (void)id;
                if (module.size != 0u && !memory.allocationContaining(module.base))
                    (void)memory.allocate(module.size, 4u, module.base);
            }
            totalCycles = r.u64();
            totalInstructions = r.u64();
            eeCycleCarry = r.u64();
            rpcAheadCycles = r.u64();
            r.pod(secrMcCommandHandler);
            r.pod(secrMcDevIdHandler);
            r.pod(checkKelfPathCallback);
            activeCpu = nullptr;
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            lastError.clear();
        }

        bool quiescent(std::string *why) const
        {
            const char *reason = nullptr;
            if (activeCpu != nullptr || callDepth != 0u)
                reason = "IOP code is executing";
            else if (servicingDmaInterrupts || servicingGuestCallbacks)
                reason = "IOP is servicing interrupts or callbacks";
            if (reason && why)
                *why = reason;
            return reason == nullptr;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        bool clipSlicesToEvents = false;
        // IOP cycles run ahead of the EE by runThreadsBeforeServer, repaid by runEeCycles.
        uint64_t rpcAheadCycles = 0;
        uint64_t rpcDrains = 0;
        uint64_t watchNextCycle = 0;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    void IopEmulator::saveState(ps2x::StateWriter &w) const
    {
        m_impl->saveState(w);
    }

    void IopEmulator::loadState(ps2x::StateReader &r)
    {
        m_impl->loadState(r);
    }

    bool IopEmulator::quiescent(std::string *why) const
    {
        return m_impl->quiescent(why);
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        // IOP time already run ahead of the EE (threads run before an RPC server) is repaid.
        const uint64_t repaid = std::min(iopCycles, m_impl->rpcAheadCycles);
        m_impl->rpcAheadCycles -= repaid;
        iopCycles -= repaid;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    void IopEmulator::setBatchedTiming(bool enabled) noexcept
    {
        m_impl->clipSlicesToEvents = enabled;
    }

    uint64_t IopEmulator::eeCyclesUntilNextEvent() const noexcept
    {
        const Impl &impl = *m_impl;
        if (impl.kernel.hasRunnableThread())
            return 0u;
        constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();
        const uint64_t next = impl.nextScheduledEventCycle(kNever);
        if (next == kNever)
            return kNever;
        if (next <= impl.totalCycles)
            return 0u;
        const uint64_t iopCycles = next - impl.totalCycles + impl.rpcAheadCycles;
        if (iopCycles > (kNever / 8u))
            return kNever;
        // runEeCycles turns 8 EE cycles (plus the carry) into one IOP cycle.
        const uint64_t eeCycles = iopCycles * 8u;
        return eeCycles > impl.eeCycleCarry ? eeCycles - impl.eeCycleCarry : 1u;
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

    void IopEmulator::setAudioOutput(std::function<void(const int16_t *, size_t)> sink)
    {
        m_impl->memory.spu2().setOutputSink(std::move(sink));
    }

    void IopEmulator::flushAudioOutput()
    {
        m_impl->memory.spu2().flushOutput();
    }

}
