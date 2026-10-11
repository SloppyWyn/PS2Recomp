// Runner snapshots: save and restore the whole runtime. Design and coverage:
// docs/SNAPSHOT-DESIGN.md. Nothing here knows which game runs.

#include "runtime/ps2_snapshot.h"
#include "runtime/ps2_fastforward.h"

#include "ps2_runtime.h"
#include "ps2_iop_host.h"
#include "ps2_snapshot_hooks.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "ps2x/iop/iop_subsystem.h"
#include "ps2x/state_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using ps2x::StateReader;
using ps2x::StateWriter;

namespace
{
    constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'S', 'N', 'A', 'P'};
    // 3: batched-timing fields (EE, memory); 4: queued/deferred invocations and the deferred id
    // counter, invocation completion tags with a second argument, the IOP's SPU2 model (in
    // place of the old DmaStart state), SIF RPC client call sequence, CD pseudo-LSN base 0x500000.
    // 5: MPEG playback stream position and demux replay region (stream-callback backpressure).
    // 6: IOP RPC servers carry their thread's priority; the IOP saves rpcAheadCycles (IOP time run
    // ahead of the EE before an RPC server function).
    // 7: VIF1 DIRECT image continuation (owed qwc and HL flag carried across transfers).
    // 8: durable snapshots. The file no longer names the runner build that may load it: a HEAD
    // block lists each section's own version and every type written raw with its size,
    // alignment and the sections holding it, and a load refuses only the sections that differ
    // (by name). From 8 on, a change to what one section writes bumps that section's version in
    // sectionVersions() below, not this number; this one changes only with the container.
    constexpr uint32_t kFormatVersion = 8u;
    constexpr uint32_t kLegacyFormatVersion = 7u; // still read (checkLegacySignature)

    // Per-section versions (path as written: "GS  ", "STUB/sRPC", "IOPS/..."). Bump a
    // section's entry whenever what it writes changes meaning or order. A change of length or
    // of a raw type's size is caught without a bump (section length check, type table), a
    // reorder of equal-sized fields or a new meaning for a field is not. Unlisted sections are 1.
    const std::map<std::string, uint32_t> &sectionVersions()
    {
        static const std::map<std::string, uint32_t> versions = {};
        return versions;
    }

    uint32_t sectionVersion(const std::string &path)
    {
        const auto &versions = sectionVersions();
        const auto found = versions.find(path);
        return found == versions.end() ? 1u : found->second;
    }

    // FNV-1a 64 over the whole snapshot but its last 8 bytes, which hold this value.
    uint64_t payloadChecksum(const uint8_t *data, size_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // Names the runner build that wrote a snapshot, for logs and checkpoint manifests only
    // (since format 8 a load does not depend on it): on Windows the executable's PE link time
    // stamp, image size and header checksum, else this file's compile date and time.
    std::string buildId()
    {
        std::ostringstream out;
#ifdef _WIN32
        if (const HMODULE module = GetModuleHandleW(nullptr))
        {
            const auto *base = reinterpret_cast<const uint8_t *>(module);
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE)
            {
                const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE)
                {
                    out << "pe:" << std::hex << nt->FileHeader.TimeDateStamp << ':' << nt->OptionalHeader.SizeOfImage
                        << ':' << nt->OptionalHeader.CheckSum;
                    return out.str();
                }
            }
        }
#endif
        out << "src:" << __DATE__ << ' ' << __TIME__;
        return out.str();
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    // ------------------------------------------------------------------ configuration
    struct SnapshotConfig
    {
        bool loaded = false;
        std::string savePattern = "ps2x_snapshot_{tick}.bin";
        std::vector<uint64_t> ticks; // PS2X_SNAPSHOT_AT, ascending
        size_t nextTick = 0u;
        bool exitAfterLast = false;
        // Refusal bookkeeping (the state was not serialisable at the request).
        bool refusing = false;
        uint64_t refusedSince = 0u;
        uint64_t lastRefusalLog = 0u;
    };

    SnapshotConfig &config()
    {
        static SnapshotConfig s;
        return s;
    }

    std::mutex &configMutex()
    {
        static std::mutex m;
        return m;
    }

    std::vector<uint64_t> parseTickList(const char *text)
    {
        std::vector<uint64_t> ticks;
        if (!text)
            return ticks;
        std::stringstream in(text);
        std::string item;
        while (std::getline(in, item, ','))
        {
            if (!item.empty())
                ticks.push_back(std::strtoull(item.c_str(), nullptr, 0));
        }
        std::sort(ticks.begin(), ticks.end());
        return ticks;
    }

    std::string expandPath(const std::string &pattern, uint64_t tick)
    {
        std::string path = pattern;
        const std::string token = "{tick}";
        for (size_t at = path.find(token); at != std::string::npos; at = path.find(token))
            path.replace(at, token.size(), std::to_string(tick));
        return path;
    }

    std::string gameIdentity()
    {
        return PS2Runtime::getIoPaths().elfPath.filename().string();
    }
}

// Friend of every runtime class with snapshot state (see the `friend struct
// PS2SnapshotAccess;` lines). Each pair writes and reads one block in the same order.
struct PS2SnapshotAccess
{
    // ------------------------------------------------------------------ quiescence
    static const char *invocationKindName(GuestInvocationKind kind)
    {
        switch (kind)
        {
        case GuestInvocationKind::Interrupt: return "interrupt handler";
        case GuestInvocationKind::Alarm: return "alarm handler";
        case GuestInvocationKind::GsCallback: return "GS VSync callback";
        case GuestInvocationKind::RpcCallback: return "RPC/stream callback";
        case GuestInvocationKind::SyscallOverride: return "syscall override";
        case GuestInvocationKind::ExitHandler: return "exit handler";
        case GuestInvocationKind::HleCall: return "HLE call";
        case GuestInvocationKind::SifCommand: return "SIF command handler";
        }
        return "?";
    }

    static std::string describeInvocation(const GuestInvocation &invocation, const GuestThread &thread)
    {
        std::ostringstream out;
        out << invocationKindName(invocation.kind) << " at pc 0x" << std::hex << invocation.context.pc
            << ", thread status " << std::dec << static_cast<int>(thread.status)
            << ", wait reason " << static_cast<int>(thread.wait.reason);
        return out.str();
    }

    static bool quiescent(PS2Runtime &runtime, std::string *why)
    {
        auto refuse = [why](const std::string &reason)
        {
            if (why)
                *why = reason;
            return false;
        };

        const EeScheduler &ee = *runtime.m_eeScheduler;
        {
            std::lock_guard<std::mutex> lock(ee.m_eventMutex);
            // Queued and time-deferred RPC completions (a NOWAIT sceSifCallRpc's end function,
            // due a fraction of a millisecond after every poll) are saved with their
            // completion tags; any other queued callback (interrupt handler, alarm, GS
            // callback) must run first.
            const auto describeQueued = [](const GuestInvocation &invocation)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "kind=%u pc=0x%08X", static_cast<unsigned>(invocation.kind),
                              invocation.context.pc);
                return std::string(text);
            };
            for (const GuestInvocation &invocation : ee.m_pendingInvocations)
            {
                if (invocation.kind != GuestInvocationKind::RpcCallback)
                    return refuse("a guest callback is queued (" + describeQueued(invocation) + ")");
                if (invocation.onComplete && invocation.completeTag.kind == GuestInvocationCompleteKind::None)
                    return refuse("a queued RPC callback has a completion that cannot be saved");
            }
            for (const auto &[id, invocation] : ee.m_deferredInvocations)
            {
                if (invocation.kind != GuestInvocationKind::RpcCallback)
                    return refuse("a deferred guest callback is waiting (" + describeQueued(invocation) + ")");
                if (invocation.onComplete && invocation.completeTag.kind == GuestInvocationCompleteKind::None)
                    return refuse("a deferred RPC callback has a completion that cannot be saved");
            }
        }
        for (const auto &[id, thread] : ee.m_threads)
        {
            // A thread inside a guest callback can be saved when every invocation on its
            // stack has a serialisable completion (GuestInvocationCompleteTag).
            for (const GuestInvocation &invocation : thread.invocations)
            {
                if (invocation.onComplete && invocation.completeTag.kind == GuestInvocationCompleteKind::None)
                    return refuse("thread " + std::to_string(id) + " is inside a guest callback invocation (" +
                                  describeInvocation(invocation, thread) + ") whose completion cannot be saved");
            }
            if (thread.wait.completion && thread.wait.resumeTag.kind == EeResumeKind::None)
                return refuse("thread " + std::to_string(id) + " waits with an untagged completion");
            if (thread.resumeCompletion && thread.resumeTag.kind == EeResumeKind::None)
                return refuse("thread " + std::to_string(id) + " resumes with an untagged completion");
        }
        if (ee.m_insideInterrupt)
            return refuse("inside an interrupt");
        if (!runtime.m_gifArbiter.empty())
            return refuse("the GIF arbiter queue is not empty");
        if (runtime.m_iopHost && runtime.m_iopHost->m_activeToken != 0u)
            return refuse("an IOP host call is in progress");
        std::string iopWhy;
        if (runtime.m_iopSubsystem && !runtime.m_iopSubsystem->quiescent(&iopWhy))
            return refuse(iopWhy);
        // A playback still running on a decoder whose replay log was dropped would restore
        // to a decoder that never produces a picture again (the game then waits forever).
        std::string mpegWhy;
        if (!ps2_stubs::snapshot::mpegReplayHealthy(&mpegWhy, true))
            return refuse(mpegWhy);
        return true;
    }

    // ------------------------------------------------------------------ EE scheduler
    static void saveResumeTag(StateWriter &w, const EeResumeTag &tag)
    {
        w.u32(static_cast<uint32_t>(tag.kind));
        w.pod(tag.args);
    }

    static EeResumeTag loadResumeTag(StateReader &r)
    {
        EeResumeTag tag;
        tag.kind = static_cast<EeResumeKind>(r.u32());
        r.pod(tag.args);
        return tag;
    }

    static std::function<void(const R5900Context &, R5900Context &)> rebuildInvocationComplete(PS2Runtime &runtime,
                                                                                             const GuestInvocationCompleteTag &tag)
    {
        PS2Runtime *rt = &runtime;
        switch (tag.kind)
        {
        case GuestInvocationCompleteKind::None:
            return {};
        case GuestInvocationCompleteKind::GuestFree:
        {
            const uint32_t address = static_cast<uint32_t>(tag.arg);
            return [rt, address](const R5900Context &, R5900Context &)
            {
                rt->guestFree(address);
            };
        }
        case GuestInvocationCompleteKind::CopyReturnValue:
            return [](const R5900Context &completed, R5900Context &parent)
            {
                parent.r[2] = completed.r[2];
            };
        case GuestInvocationCompleteKind::ExitCurrent:
        {
            const bool deleteThread = tag.arg != 0u;
            return [rt, deleteThread](const R5900Context &, R5900Context &)
            {
                rt->eeScheduler().exitCurrent(deleteThread);
            };
        }
        case GuestInvocationCompleteKind::IpuInitComplete:
            return ps2_stubs::snapshot::makeIpuInitComplete(runtime);
        case GuestInvocationCompleteKind::RpcClientComplete:
            return ps2_stubs::snapshot::makeRpcClientComplete(runtime, tag);
        case GuestInvocationCompleteKind::MpegNodataComplete:
            return ps2_stubs::snapshot::makeMpegNodataComplete(runtime, static_cast<uint32_t>(tag.arg));
        }
        throw ps2x::StateError("unknown invocation completion tag");
    }

    static std::function<void(R5900Context &)> rebuildResume(PS2Runtime &runtime, const EeResumeTag &tag)
    {
        switch (tag.kind)
        {
        case EeResumeKind::None:
            return {};
        case EeResumeKind::SetPc:
        {
            const uint32_t pc = static_cast<uint32_t>(tag.args[0]);
            return [pc](R5900Context &context)
            {
                context.pc = pc;
            };
        }
        case EeResumeKind::MpegGetPicture:
            return ps2_stubs::snapshot::makeMpegGetPictureResume(runtime);
        case EeResumeKind::CdStRead:
            return ps2_stubs::snapshot::makeCdStReadResume(runtime, tag);
        }
        throw ps2x::StateError("snapshot holds an unknown wait-completion kind " +
                               std::to_string(static_cast<uint32_t>(tag.kind)));
    }

    static void saveWait(StateWriter &w, const EeWaitState &wait)
    {
        w.u8(static_cast<uint8_t>(wait.reason));
        w.u8(static_cast<uint8_t>(wait.payload.index()));
        std::visit([&w](const auto &payload)
                   {
                       using T = std::decay_t<decltype(payload)>;
                       if constexpr (!std::is_same_v<T, std::monostate>)
                           w.pod(payload); },
                   wait.payload);
        w.boolean(static_cast<bool>(wait.completion));
        saveResumeTag(w, wait.resumeTag);
    }

    static void loadWait(StateReader &r, EeWaitState &wait, PS2Runtime &runtime)
    {
        wait = {};
        wait.reason = static_cast<EeWaitReason>(r.u8());
        switch (r.u8())
        {
        case 0:
            wait.payload = std::monostate{};
            break;
        case 1:
            wait.payload = r.pod<EeSemaphoreWait>();
            break;
        case 2:
            wait.payload = r.pod<EeEventFlagWait>();
            break;
        case 3:
            wait.payload = r.pod<EeVSyncWait>();
            break;
        case 4:
            wait.payload = r.pod<EeExternalWait>();
            break;
        default:
            throw ps2x::StateError("snapshot holds an unknown wait payload");
        }
        const bool hadCompletion = r.boolean();
        wait.resumeTag = loadResumeTag(r);
        if (hadCompletion)
            wait.completion = rebuildResume(runtime, wait.resumeTag);
    }

    static void saveScheduler(const EeScheduler &ee, StateWriter &w)
    {
        static_assert(std::variant_size_v<EeWaitPayload> == 5, "update saveWait/loadWait for the new payload");
        for (const auto &queue : ee.m_readyQueues)
            w.podSequence(queue);

        std::vector<int> ids;
        for (const auto &[id, thread] : ee.m_threads)
            ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        w.size(ids.size());
        for (const int id : ids)
        {
            const GuestThread &t = ee.m_threads.at(id);
            w.i32(t.id);
            w.pod(t.context);
            w.u32(t.entry);
            w.u32(t.stack);
            w.u32(t.stackSize);
            w.u32(t.gp);
            w.u32(t.attr);
            w.u32(t.option);
            w.u32(t.arg);
            w.i32(t.initialPriority);
            w.i32(t.currentPriority);
            w.u8(static_cast<uint8_t>(t.status));
            w.i32(t.suspendCount);
            w.u32(t.wakeupCount);
            w.boolean(t.ownsStack);
            w.u32(t.tlsBase);
            saveWait(w, t.wait);
            w.boolean(static_cast<bool>(t.resumeCompletion));
            saveResumeTag(w, t.resumeTag);
            // The invocation stack (guest callbacks in flight on this thread), innermost last.
            w.size(t.invocations.size());
            for (const GuestInvocation &invocation : t.invocations)
            {
                if (invocation.onComplete && invocation.completeTag.kind == GuestInvocationCompleteKind::None)
                    throw ps2x::StateError("thread " + std::to_string(id) + " has an invocation whose completion cannot be saved");
                w.u8(static_cast<uint8_t>(invocation.kind));
                w.u64(invocation.sequence);
                w.u64(invocation.tag);
                w.boolean(static_cast<bool>(invocation.onComplete));
                w.u8(static_cast<uint8_t>(invocation.completeTag.kind));
                w.u64(invocation.completeTag.arg);
                w.u64(invocation.completeTag.arg2);
                w.pod(invocation.context);
            }
        }

        w.sortedMap(ee.m_semaphores, [&w](const EeSemaphore &s)
                    {
                        w.i32(s.id);
                        w.i32(s.count);
                        w.i32(s.maxCount);
                        w.i32(s.initCount);
                        w.u32(s.attr);
                        w.u32(s.option);
                        w.podSequence(s.waiters); });
        w.sortedMap(ee.m_eventFlags, [&w](const EeEventFlag &f)
                    {
                        w.i32(f.id);
                        w.u32(f.attr);
                        w.u32(f.option);
                        w.u32(f.initBits);
                        w.u32(f.bits);
                        w.podSequence(f.waiters); });
        w.podMap(ee.m_alarms);
        w.podMap(ee.m_intcHandlers);
        w.podMap(ee.m_dmacHandlers);
        w.i32(ee.m_nextThreadId);
        w.i32(ee.m_nextInvocationThreadId);
        w.i32(ee.m_nextSemaphoreId);
        w.i32(ee.m_nextEventFlagId);
        w.i32(ee.m_nextAlarmId);
        w.i32(ee.m_nextIntcHandlerId);
        w.i32(ee.m_nextDmacHandlerId);
        w.i32(ee.m_intcHeadOrder);
        w.i32(ee.m_intcTailOrder);
        w.i32(ee.m_dmacHeadOrder);
        w.i32(ee.m_dmacTailOrder);
        w.u32(ee.m_enabledIntcMask);
        w.u32(ee.m_enabledDmacMask);
        w.i32(ee.m_currentThreadId);
        w.boolean(ee.m_rescheduleRequested);
        w.boolean(ee.m_timeSliceExpired);
        w.u32(ee.m_pendingEeTimerInterrupts);
        w.u64(ee.m_eeCycle);
        w.u64(ee.m_sliceEndCycle);
        // Batched timing (PS2X_BATCH_TIMING): save() flushed the deferred cycles first, so the
        // IOP is synced to m_eeCycle; the flush deadline is restored as is (a loading run with
        // batching on reuses it, one with it off ignores it; run() recomputes anyway).
        // With batching off the eager path keeps the IOP at m_eeCycle without touching
        // m_iopSyncedCycle: write m_eeCycle, which is the truth either way.
        if (ee.m_batchTiming && ee.m_iopSyncedCycle != ee.m_eeCycle)
            throw ps2x::StateError("snapshot save with batched IOP cycles pending (flush missing)");
        w.boolean(ee.m_batchTiming);
        w.u64(ee.m_batchTiming ? ee.m_batchFlushCycle : 0u);
        w.u64(ee.m_eeCycle);
        w.u32(ee.m_batchTimerGeneration);
        {
            std::lock_guard<std::mutex> lock(ee.m_eventMutex);
            w.podSequence(ee.m_events);
            w.size(ee.m_deadlines.size());
            for (const auto &scheduled : ee.m_deadlines)
            {
                // Host deadlines are re-derived from the cycle deadline at load.
                w.u64(scheduled.deadlineCycle);
                w.pod(scheduled.event);
                w.u64(scheduled.sequence);
            }
            // Queued and time-deferred invocations (GS callbacks, RPC end functions): saved
            // like a thread's invocation stack, each with its completion tag.
            const auto saveInvocation = [&w](const GuestInvocation &invocation, const char *where)
            {
                if (invocation.onComplete && invocation.completeTag.kind == GuestInvocationCompleteKind::None)
                    throw ps2x::StateError(std::string(where) + " invocation's completion cannot be saved");
                w.u8(static_cast<uint8_t>(invocation.kind));
                w.u64(invocation.sequence);
                w.u64(invocation.tag);
                w.boolean(static_cast<bool>(invocation.onComplete));
                w.u8(static_cast<uint8_t>(invocation.completeTag.kind));
                w.u64(invocation.completeTag.arg);
                w.u64(invocation.completeTag.arg2);
                w.pod(invocation.context);
            };
            w.size(ee.m_pendingInvocations.size());
            for (const GuestInvocation &invocation : ee.m_pendingInvocations)
                saveInvocation(invocation, "a queued");
            w.sortedMap(ee.m_deferredInvocations, [&saveInvocation](const GuestInvocation &invocation)
                        { saveInvocation(invocation, "a deferred"); });
            w.u32(ee.m_nextDeferredInvocationId);
        }
        w.u64(ee.m_eventSequence);
        w.u64(ee.m_invocationSequence);
        w.u64(ee.m_vsyncTick);
        w.u32(ee.m_vsyncFlagAddress);
        w.u32(ee.m_vsyncTickAddress);
        w.u32(ee.m_gsVSyncCallback);
        w.u32(ee.m_gsVSyncCallbackGp);
        w.u32(ee.m_gsVSyncCallbackSp);
        w.podMap(ee.m_invocationStackTops);
    }

    static void loadScheduler(EeScheduler &ee, StateReader &r, PS2Runtime &runtime)
    {
        for (auto &queue : ee.m_readyQueues)
            r.podSequence(queue);

        ee.m_threads.clear();
        const size_t threadCount = r.size(4096u);
        for (size_t i = 0; i < threadCount; ++i)
        {
            GuestThread t{};
            t.id = r.i32();
            r.pod(t.context);
            t.entry = r.u32();
            t.stack = r.u32();
            t.stackSize = r.u32();
            t.gp = r.u32();
            t.attr = r.u32();
            t.option = r.u32();
            t.arg = r.u32();
            t.initialPriority = r.i32();
            t.currentPriority = r.i32();
            t.status = static_cast<EeThreadStatus>(r.u8());
            t.suspendCount = r.i32();
            t.wakeupCount = r.u32();
            t.ownsStack = r.boolean();
            t.tlsBase = r.u32();
            loadWait(r, t.wait, runtime);
            const bool hadResume = r.boolean();
            t.resumeTag = loadResumeTag(r);
            if (hadResume)
                t.resumeCompletion = rebuildResume(runtime, t.resumeTag);
            const size_t invocationCount = r.size(64u);
            for (size_t k = 0; k < invocationCount; ++k)
            {
                GuestInvocation invocation{};
                invocation.kind = static_cast<GuestInvocationKind>(r.u8());
                invocation.sequence = r.u64();
                invocation.tag = r.u64();
                const bool hadComplete = r.boolean();
                invocation.completeTag.kind = static_cast<GuestInvocationCompleteKind>(r.u8());
                invocation.completeTag.arg = r.u64();
                invocation.completeTag.arg2 = r.u64();
                r.pod(invocation.context);
                if (hadComplete)
                    invocation.onComplete = rebuildInvocationComplete(runtime, invocation.completeTag);
                t.invocations.push_back(std::move(invocation));
            }
            const int id = t.id;
            ee.m_threads.emplace(id, std::move(t));
        }

        r.sortedMap(ee.m_semaphores, [&r](EeSemaphore &s)
                    {
                        s.id = r.i32();
                        s.count = r.i32();
                        s.maxCount = r.i32();
                        s.initCount = r.i32();
                        s.attr = r.u32();
                        s.option = r.u32();
                        r.podSequence(s.waiters); });
        r.sortedMap(ee.m_eventFlags, [&r](EeEventFlag &f)
                    {
                        f.id = r.i32();
                        f.attr = r.u32();
                        f.option = r.u32();
                        f.initBits = r.u32();
                        f.bits = r.u32();
                        r.podSequence(f.waiters); });
        r.podMap(ee.m_alarms);
        r.podMap(ee.m_intcHandlers);
        r.podMap(ee.m_dmacHandlers);
        ee.m_nextThreadId = r.i32();
        ee.m_nextInvocationThreadId = r.i32();
        ee.m_nextSemaphoreId = r.i32();
        ee.m_nextEventFlagId = r.i32();
        ee.m_nextAlarmId = r.i32();
        ee.m_nextIntcHandlerId = r.i32();
        ee.m_nextDmacHandlerId = r.i32();
        ee.m_intcHeadOrder = r.i32();
        ee.m_intcTailOrder = r.i32();
        ee.m_dmacHeadOrder = r.i32();
        ee.m_dmacTailOrder = r.i32();
        ee.m_enabledIntcMask = r.u32();
        ee.m_enabledDmacMask = r.u32();
        ee.m_currentThreadId = r.i32();
        ee.m_rescheduleRequested = r.boolean();
        ee.m_timeSliceExpired = r.boolean();
        ee.m_pendingEeTimerInterrupts = r.u32();
        ee.m_eeCycle = r.u64();
        ee.m_sliceEndCycle = r.u64();
        ee.m_insideInterrupt = false;
        {
            // Whether the saving run batched does not bind the loading run (run() reads the
            // environment); the synced cycle and the deadline are valid for either.
            const bool savedBatched = r.boolean();
            ee.m_batchFlushCycle = r.u64();
            ee.m_iopSyncedCycle = r.u64();
            ee.m_batchTimerGeneration = r.u32();
            if (ee.m_iopSyncedCycle != ee.m_eeCycle)
            {
                if (savedBatched)
                    throw ps2x::StateError("snapshot has batched IOP cycles pending");
                ee.m_iopSyncedCycle = ee.m_eeCycle; // an older eager save: the IOP is at m_eeCycle
            }
            ee.m_flushingDeferred = false;
        }
        {
            std::lock_guard<std::mutex> lock(ee.m_eventMutex);
            r.podSequence(ee.m_events);
            ee.m_deadlines.clear();
            const auto now = ps2x::ff::now();
            const size_t deadlineCount = r.size();
            for (size_t i = 0; i < deadlineCount; ++i)
            {
                EeScheduler::ScheduledEvent scheduled{};
                scheduled.deadlineCycle = r.u64();
                r.pod(scheduled.event);
                scheduled.sequence = r.u64();
                const uint64_t ahead = scheduled.deadlineCycle > ee.m_eeCycle ? scheduled.deadlineCycle - ee.m_eeCycle : 0u;
                scheduled.hostDeadline = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(eeCyclesToHostDuration(ahead));
                ee.m_deadlines.push_back(scheduled);
            }
            const auto loadInvocation = [&r, &runtime]()
            {
                GuestInvocation invocation;
                invocation.kind = static_cast<GuestInvocationKind>(r.u8());
                invocation.sequence = r.u64();
                invocation.tag = r.u64();
                const bool hadComplete = r.boolean();
                invocation.completeTag.kind = static_cast<GuestInvocationCompleteKind>(r.u8());
                invocation.completeTag.arg = r.u64();
                invocation.completeTag.arg2 = r.u64();
                r.pod(invocation.context);
                if (hadComplete)
                    invocation.onComplete = rebuildInvocationComplete(runtime, invocation.completeTag);
                return invocation;
            };
            ee.m_pendingInvocations.clear();
            const size_t pendingCount = r.size();
            for (size_t i = 0; i < pendingCount; ++i)
                ee.m_pendingInvocations.push_back(loadInvocation());
            r.sortedMap(ee.m_deferredInvocations, [&loadInvocation](GuestInvocation &invocation)
                        { invocation = loadInvocation(); });
            ee.m_nextDeferredInvocationId = r.u32();
            ee.updateNextDeadline();
        }
        ee.m_eventSequence = r.u64();
        ee.m_invocationSequence = r.u64();
        ee.m_vsyncTick = r.u64();
        ee.m_vsyncFlagAddress = r.u32();
        ee.m_vsyncTickAddress = r.u32();
        ee.m_gsVSyncCallback = r.u32();
        ee.m_gsVSyncCallbackGp = r.u32();
        ee.m_gsVSyncCallbackSp = r.u32();
        r.podMap(ee.m_invocationStackTops);

        ee.m_rdram = runtime.m_memory.getRDRAM();
        ee.m_stopRequested.store(false, std::memory_order_release);
        ee.m_checkpointPending.store(true, std::memory_order_release);
        ee.m_saveRequested.store(false, std::memory_order_release);
        ee.m_debugPublishCountdown = 0u;
        ee.publishSnapshot();
    }

    // ------------------------------------------------------------------ runtime
    static void saveRuntime(const PS2Runtime &rt, StateWriter &w)
    {
        w.pod(rt.m_cpuContext);
        {
            std::lock_guard<std::mutex> lock(rt.m_eeKernelStateMutex);
            w.sortedMap(rt.m_eeExitHandlers, [&w](const std::vector<PS2Runtime::EeExitHandlerRegistration> &handlers)
                        { w.podVector(handlers); });
            w.podMap(rt.m_eeSyscallOverrides);
            w.sortedSet(rt.m_eeSyscallMirrorAddresses);
        }
        {
            std::lock_guard<std::mutex> lock(rt.m_guestHeapMutex);
            w.podVector(rt.m_guestHeapBlocks);
            w.u32(rt.m_guestHeapBase);
            w.u32(rt.m_guestHeapEnd);
            w.u32(rt.m_guestHeapLimit);
            w.u32(rt.m_guestHeapSuggestedBase);
            w.boolean(rt.m_guestHeapConfigured);
        }
        {
            std::lock_guard<std::mutex> lock(rt.m_asyncCallbackStackMutex);
            w.u32(rt.m_asyncCallbackStackFloor);
            w.u32(rt.m_asyncCallbackStackTop);
        }
        w.size(rt.m_loadedModules.size());
        for (const auto &module : rt.m_loadedModules)
        {
            w.str(module.name);
            w.u32(module.baseAddress);
            w.size(module.size);
            w.boolean(module.active);
        }
    }

    static void loadRuntime(PS2Runtime &rt, StateReader &r)
    {
        r.pod(rt.m_cpuContext);
        {
            std::lock_guard<std::mutex> lock(rt.m_eeKernelStateMutex);
            r.sortedMap(rt.m_eeExitHandlers, [&r](std::vector<PS2Runtime::EeExitHandlerRegistration> &handlers)
                        { r.podVector(handlers); });
            r.podMap(rt.m_eeSyscallOverrides);
            r.sortedSet(rt.m_eeSyscallMirrorAddresses);
        }
        {
            std::lock_guard<std::mutex> lock(rt.m_guestHeapMutex);
            r.podVector(rt.m_guestHeapBlocks);
            rt.m_guestHeapBase = r.u32();
            rt.m_guestHeapEnd = r.u32();
            rt.m_guestHeapLimit = r.u32();
            rt.m_guestHeapSuggestedBase = r.u32();
            rt.m_guestHeapConfigured = r.boolean();
        }
        {
            std::lock_guard<std::mutex> lock(rt.m_asyncCallbackStackMutex);
            rt.m_asyncCallbackStackFloor = r.u32();
            rt.m_asyncCallbackStackTop = r.u32();
        }
        rt.m_loadedModules.clear();
        const size_t count = r.size();
        for (size_t i = 0; i < count; ++i)
        {
            PS2Runtime::LoadedModule module;
            module.name = r.str();
            module.baseAddress = r.u32();
            module.size = static_cast<size_t>(r.u64());
            module.active = r.boolean();
            rt.m_loadedModules.push_back(std::move(module));
        }
        rt.m_debugPc.store(rt.m_cpuContext.pc, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------ memory
    static void savePendingTransfers(StateWriter &w, const std::vector<PS2Memory::PendingTransfer> &transfers)
    {
        w.size(transfers.size());
        for (const auto &transfer : transfers)
        {
            w.boolean(transfer.fromScratchpad);
            w.u32(transfer.srcAddr);
            w.u32(transfer.qwc);
            w.podVector(transfer.chainData);
        }
    }

    static void loadPendingTransfers(StateReader &r, std::vector<PS2Memory::PendingTransfer> &transfers)
    {
        transfers.clear();
        const size_t count = r.size();
        for (size_t i = 0; i < count; ++i)
        {
            PS2Memory::PendingTransfer transfer;
            transfer.fromScratchpad = r.boolean();
            transfer.srcAddr = r.u32();
            transfer.qwc = r.u32();
            r.podVector(transfer.chainData);
            transfers.push_back(std::move(transfer));
        }
    }

    static void saveBlock(StateWriter &w, const uint8_t *data, size_t size)
    {
        w.size(data ? size : 0u);
        if (data)
            w.bytes(data, size);
    }

    static void loadBlock(StateReader &r, uint8_t *data, size_t size, const char *what)
    {
        const size_t stored = r.size(std::numeric_limits<uint32_t>::max());
        if (stored == 0u)
            return;
        if (!data || stored != size)
            throw ps2x::StateError(std::string("snapshot ") + what + " size differs from this build");
        r.bytes(data, size);
    }

    static void saveMemory(PS2Memory &m, StateWriter &w)
    {
        saveBlock(w, m.m_rdram, PS2_RAM_SIZE);
        saveBlock(w, m.m_scratchpad, PS2_SCRATCHPAD_SIZE);
        saveBlock(w, m.iop_ram, 2u * 1024u * 1024u);
        saveBlock(w, m.m_gsVRAM, PS2_GS_VRAM_SIZE);
        saveBlock(w, m.m_vu0Code, PS2_VU0_CODE_SIZE);
        saveBlock(w, m.m_vu0Data, PS2_VU0_DATA_SIZE);
        saveBlock(w, m.m_vu1Code, PS2_VU1_CODE_SIZE);
        saveBlock(w, m.m_vu1Data, PS2_VU1_DATA_SIZE);

        w.boolean(m.m_seenGifCopy);
        w.u64(m.m_dmaStartCount.load());
        w.u64(m.m_gifCopyCount.load());
        w.u64(m.m_gsWriteCount.load());
        w.u64(m.m_vifWriteCount.load());
        w.podMap(m.m_ioRegisters);

        const GSRegisters &gs = m.gs_regs;
        for (const uint64_t value : {gs.pmode, gs.smode1, gs.smode2, gs.srfsh, gs.synch1, gs.synch2, gs.syncv,
                                     gs.dispfb1, gs.display1, gs.dispfb2, gs.display2, gs.extbuf, gs.extdata,
                                     gs.extwrite, gs.bgcolor})
            w.u64(value);
        w.u64(gs.csr.load());
        w.u64(gs.vsyncTick.load());
        w.u64(gs.imr);
        w.u64(gs.busdir);
        w.u64(gs.siglblid);

        w.pod(m.vif0_regs);
        w.pod(m.vif1_regs);
        w.pod(m.dma_regs);
        w.podVector(m.m_tlbEntries);
        w.boolean(m.m_path3Masked);
        w.u32(m.m_vif1PendingPath2ImageQwc);
        w.boolean(m.m_vif1PendingPath2DirectHl);
        w.u32(m.m_vif1DirectRemainQwc);
        w.boolean(m.m_vif1DirectRemainHl);
        w.size(m.m_path3MaskedFifo.size());
        for (const auto &packet : m.m_path3MaskedFifo)
            w.podVector(packet);
        savePendingTransfers(w, m.m_pendingGifTransfers);
        savePendingTransfers(w, m.m_pendingVif0Transfers);
        savePendingTransfers(w, m.m_pendingVif1Transfers);
        {
            std::lock_guard<std::mutex> lock(m.m_completedDmacMutex);
            w.podVector(m.m_completedDmacCauses);
        }
        w.size(m.m_codeRegions.size());
        for (const auto &region : m.m_codeRegions)
        {
            w.u32(region.start);
            w.u32(region.end);
            w.boolVector(region.modified);
        }
        w.pod(m.m_eeTimers);
        // Batched timing: the save flushed first, so nothing is deferred or pending here;
        // the generation is kept so the scheduler's copy stays in step.
        if (m.m_eeTimerDeferredCycles != 0u || m.m_eeTimerPendingInterrupts != 0u)
            throw ps2x::StateError("snapshot save with deferred EE timer cycles (flush missing)");
        w.u64(m.m_eeTimerDeferredCycles);
        w.u32(m.m_eeTimerPendingInterrupts);
        w.u32(m.m_eeTimerGeneration);
    }

    static void loadMemory(PS2Memory &m, StateReader &r)
    {
        loadBlock(r, m.m_rdram, PS2_RAM_SIZE, "RDRAM");
        loadBlock(r, m.m_scratchpad, PS2_SCRATCHPAD_SIZE, "scratchpad");
        loadBlock(r, m.iop_ram, 2u * 1024u * 1024u, "IOP RAM mirror");
        loadBlock(r, m.m_gsVRAM, PS2_GS_VRAM_SIZE, "GS memory");
        loadBlock(r, m.m_vu0Code, PS2_VU0_CODE_SIZE, "VU0 code");
        loadBlock(r, m.m_vu0Data, PS2_VU0_DATA_SIZE, "VU0 data");
        loadBlock(r, m.m_vu1Code, PS2_VU1_CODE_SIZE, "VU1 code");
        loadBlock(r, m.m_vu1Data, PS2_VU1_DATA_SIZE, "VU1 data");
        m.markVU0CodeModified();
        m.markVU1CodeModified();

        m.m_seenGifCopy = r.boolean();
        m.m_dmaStartCount.store(r.u64());
        m.m_gifCopyCount.store(r.u64());
        m.m_gsWriteCount.store(r.u64());
        m.m_vifWriteCount.store(r.u64());
        r.podMap(m.m_ioRegisters);
        // podMap rebuilt the map: the cached GIF_STAT slot pointer (advanceEeTimers) is stale.
        m.m_gifStatSlot = nullptr;

        GSRegisters &gs = m.gs_regs;
        for (uint64_t *value : {&gs.pmode, &gs.smode1, &gs.smode2, &gs.srfsh, &gs.synch1, &gs.synch2, &gs.syncv,
                                &gs.dispfb1, &gs.display1, &gs.dispfb2, &gs.display2, &gs.extbuf, &gs.extdata,
                                &gs.extwrite, &gs.bgcolor})
            *value = r.u64();
        gs.csr.store(r.u64());
        gs.vsyncTick.store(r.u64());
        gs.imr = r.u64();
        gs.busdir = r.u64();
        gs.siglblid = r.u64();

        r.pod(m.vif0_regs);
        r.pod(m.vif1_regs);
        r.pod(m.dma_regs);
        r.podVector(m.m_tlbEntries);
        m.m_path3Masked = r.boolean();
        m.m_vif1PendingPath2ImageQwc = r.u32();
        m.m_vif1PendingPath2DirectHl = r.boolean();
        m.m_vif1DirectRemainQwc = r.u32();
        m.m_vif1DirectRemainHl = r.boolean();
        m.m_path3MaskedFifo.clear();
        const size_t fifo = r.size();
        for (size_t i = 0; i < fifo; ++i)
        {
            std::vector<uint8_t> packet;
            r.podVector(packet);
            m.m_path3MaskedFifo.push_back(std::move(packet));
        }
        loadPendingTransfers(r, m.m_pendingGifTransfers);
        loadPendingTransfers(r, m.m_pendingVif0Transfers);
        loadPendingTransfers(r, m.m_pendingVif1Transfers);
        {
            std::lock_guard<std::mutex> lock(m.m_completedDmacMutex);
            r.podVector(m.m_completedDmacCauses);
        }
        m.m_codeRegions.clear();
        const size_t regions = r.size();
        for (size_t i = 0; i < regions; ++i)
        {
            PS2Memory::CodeRegion region{};
            region.start = r.u32();
            region.end = r.u32();
            r.boolVector(region.modified);
            if (region.start > region.end || region.end > PS2_RAM_SIZE ||
                region.modified.size() > static_cast<size_t>(region.end - region.start) / 4u + 1u)
                throw ps2x::StateError("snapshot code region out of range");
            m.m_codeRegions.push_back(std::move(region));
        }
        r.pod(m.m_eeTimers);
        m.m_eeTimerDeferredCycles = r.u64();
        m.m_eeTimerPendingInterrupts = r.u32();
        m.m_eeTimerGeneration = r.u32();
        if (m.m_eeTimerDeferredCycles != 0u || m.m_eeTimerPendingInterrupts != 0u)
            throw ps2x::StateError("snapshot has deferred EE timer cycles");
    }

    // ------------------------------------------------------------------ GS
    static void saveGs(GS &gs, StateWriter &w)
    {
        gs.sync(); // asynchronous GS: everything queued is applied first
        std::lock_guard<std::recursive_mutex> lock(gs.m_stateMutex);
        w.pod(gs.m_ctx);
        w.pod(gs.m_prim);
        w.pod(gs.m_primRegister);
        w.pod(gs.m_prmodeRegister);
        w.u8(gs.m_curR);
        w.u8(gs.m_curG);
        w.u8(gs.m_curB);
        w.u8(gs.m_curA);
        w.pod(gs.m_curQ);
        w.pod(gs.m_curS);
        w.pod(gs.m_curT);
        w.u16(gs.m_curU);
        w.u16(gs.m_curV);
        w.u8(gs.m_curFog);
        w.u8(gs.m_fogR);
        w.u8(gs.m_fogG);
        w.u8(gs.m_fogB);
        w.boolean(gs.m_prmodecont);
        w.boolean(gs.m_pabe);
        w.u64(gs.m_scanmsk);
        w.u64(gs.m_dimx);
        w.u64(gs.m_dthe);
        w.u64(gs.m_colclamp);
        w.pod(gs.m_texa);
        w.pod(gs.m_texclut);
        w.pod(gs.m_bitbltbuf);
        w.pod(gs.m_trxpos);
        w.pod(gs.m_trxreg);
        w.u32(gs.m_trxdir);
        w.pod(gs.m_vtxQueue);
        w.i32(gs.m_vtxCount);
        w.i32(gs.m_vtxIndex);
        w.u32(gs.m_lastDisplayBaseBytes);
        w.pod(gs.m_preferredDisplaySourceFrame);
        w.u32(gs.m_preferredDisplayDestFbp);
        w.boolean(gs.m_hasPreferredDisplaySource);
        w.u64(gs.m_nativeImageUploadCount);
        w.u64(gs.m_nativePackedGIFPacketCount);

        std::lock_guard<std::mutex> backendLock(gs.m_backendLifetimeMutex);
        auto *backend = dynamic_cast<GSCpuBackend *>(gs.m_backend.get());
        w.boolean(backend != nullptr);
        if (backend)
        {
            std::lock_guard<std::mutex> cpuLock(backend->m_mutex);
            w.pod(backend->m_clut);
            w.pod(backend->m_clutCbp);
            w.pod(backend->m_transfer);
            w.pod(backend->m_transferState);
            w.podVector(backend->m_localToHostBuffer);
            w.size(backend->m_localToHostReadPos);
        }
    }

    static void loadGs(GS &gs, StateReader &r)
    {
        gs.sync();
        std::lock_guard<std::recursive_mutex> lock(gs.m_stateMutex);
        r.pod(gs.m_ctx);
        r.pod(gs.m_prim);
        r.pod(gs.m_primRegister);
        r.pod(gs.m_prmodeRegister);
        gs.m_curR = r.u8();
        gs.m_curG = r.u8();
        gs.m_curB = r.u8();
        gs.m_curA = r.u8();
        r.pod(gs.m_curQ);
        r.pod(gs.m_curS);
        r.pod(gs.m_curT);
        gs.m_curU = r.u16();
        gs.m_curV = r.u16();
        gs.m_curFog = r.u8();
        gs.m_fogR = r.u8();
        gs.m_fogG = r.u8();
        gs.m_fogB = r.u8();
        gs.m_prmodecont = r.boolean();
        gs.m_pabe = r.boolean();
        gs.m_scanmsk = r.u64();
        gs.m_dimx = r.u64();
        gs.m_dthe = r.u64();
        gs.m_colclamp = r.u64();
        r.pod(gs.m_texa);
        r.pod(gs.m_texclut);
        r.pod(gs.m_bitbltbuf);
        r.pod(gs.m_trxpos);
        r.pod(gs.m_trxreg);
        gs.m_trxdir = r.u32();
        r.pod(gs.m_vtxQueue);
        gs.m_vtxCount = r.i32();
        gs.m_vtxIndex = r.i32();
        // Both are kick counters, not queue positions: the queue is indexed modulo its size
        // (GS::vertexKick). m_vtxIndex counts every kick since the last PRIM write (a long
        // strip, e.g. a battle-map terrain mesh, passes the queue size), and m_vtxCount keeps
        // growing under an undefined primitive type. Only a negative value is corrupt.
        if (gs.m_vtxCount < 0 || gs.m_vtxIndex < 0)
            throw ps2x::StateError("snapshot GS vertex queue indices out of range");
        gs.m_lastDisplayBaseBytes = r.u32();
        r.pod(gs.m_preferredDisplaySourceFrame);
        gs.m_preferredDisplayDestFbp = r.u32();
        gs.m_hasPreferredDisplaySource = r.boolean();
        gs.m_nativeImageUploadCount = r.u64();
        gs.m_nativePackedGIFPacketCount = r.u64();

        std::lock_guard<std::mutex> backendLock(gs.m_backendLifetimeMutex);
        const bool hadBackend = r.boolean();
        auto *backend = dynamic_cast<GSCpuBackend *>(gs.m_backend.get());
        if (hadBackend != (backend != nullptr))
            throw ps2x::StateError("snapshot GS backend differs from this runner's");
        if (backend)
        {
            std::lock_guard<std::mutex> cpuLock(backend->m_mutex);
            r.pod(backend->m_clut);
            r.pod(backend->m_clutCbp);
            r.pod(backend->m_transfer);
            r.pod(backend->m_transferState);
            r.podVector(backend->m_localToHostBuffer);
            backend->m_localToHostReadPos = static_cast<size_t>(r.u64());
            // (a finished transfer keeps copiedPixels with totalPixels reset to 0: no order between them)
            if (backend->m_transfer.direction > 3u || backend->m_transferState.direction > 3u ||
                backend->m_transferState.localToHostPendingBytes > backend->m_localToHostBuffer.size() ||
                backend->m_localToHostReadPos > backend->m_localToHostBuffer.size())
                throw ps2x::StateError("snapshot GS transfer state out of range");
            backend->m_texturePageCache.Invalidate();
        }
    }

    // ------------------------------------------------------------------ VU
    static void saveVu(const VU1Interpreter &vu, StateWriter &w)
    {
        w.u8(static_cast<uint8_t>(vu.m_unit));
        w.pod(vu.m_state);
        w.pod(vu.m_flagPipeline);
        w.pod(vu.m_fdiv);
        w.pod(vu.m_efu);
        w.pod(vu.m_storePipeline);
        w.pod(vu.m_vfWritePipeline);
        w.pod(vu.m_viWritePipeline);
        w.pod(vu.m_accWritePipeline);
        w.pod(vu.m_xgkick);
        w.pod(vu.m_vfReady);
        w.pod(vu.m_viReady);
        w.pod(vu.m_accReady);
        w.pod(vu.m_vfLatestWrite);
        w.pod(vu.m_viLatestWrite);
        w.pod(vu.m_accLatestWrite);
        w.u64(vu.m_cycle);
        w.u64(vu.m_nextWriteSequence);
        w.u64(vu.m_efuResourceReady);
        w.u32(vu.m_workingClip);
        w.u32(vu.m_currentUpperInstruction);
        w.i32(vu.m_viBranchBackupValue);
        w.u8(vu.m_viBranchBackupReg);
        w.boolean(vu.m_viBranchBackupValid);
        w.boolean(vu.m_stopRequested);
        w.boolean(vu.m_pendingHaltD);
        w.boolean(vu.m_pendingHaltT);
    }

    static void loadVu(VU1Interpreter &vu, StateReader &r)
    {
        if (r.u8() != static_cast<uint8_t>(vu.m_unit))
            throw ps2x::StateError("snapshot VU unit order differs");
        r.pod(vu.m_state);
        r.pod(vu.m_flagPipeline);
        r.pod(vu.m_fdiv);
        r.pod(vu.m_efu);
        r.pod(vu.m_storePipeline);
        r.pod(vu.m_vfWritePipeline);
        r.pod(vu.m_viWritePipeline);
        r.pod(vu.m_accWritePipeline);
        vu.m_nextCommitCycle = 0; // not saved: rescan the loaded pipelines
        vu.m_pipeNext = {};
        r.pod(vu.m_xgkick);
        r.pod(vu.m_vfReady);
        r.pod(vu.m_viReady);
        r.pod(vu.m_accReady);
        r.pod(vu.m_vfLatestWrite);
        r.pod(vu.m_viLatestWrite);
        r.pod(vu.m_accLatestWrite);
        vu.m_cycle = r.u64();
        vu.m_nextWriteSequence = r.u64();
        vu.m_efuResourceReady = r.u64();
        vu.m_workingClip = r.u32();
        vu.m_currentUpperInstruction = r.u32();
        vu.m_viBranchBackupValue = r.i32();
        vu.m_viBranchBackupReg = r.u8();
        vu.m_viBranchBackupValid = r.boolean();
        vu.m_stopRequested = r.boolean();
        vu.m_pendingHaltD = r.boolean();
        vu.m_pendingHaltT = r.boolean();
        vu.m_decodedCodeCacheValid = false;
    }

    // ------------------------------------------------------------------ host adapter, VFS, audio
    static void saveIopHost(const PS2IopHostAdapter &host, StateWriter &w)
    {
        w.u64(host.m_nextToken);
        std::lock_guard<std::mutex> lock(host.m_hostFileMutex);
        w.u64(host.m_nextHostFileHandle);
    }

    static void loadIopHost(PS2IopHostAdapter &host, StateReader &r)
    {
        host.m_nextToken = r.u64();
        std::vector<std::FILE *> streams;
        {
            std::lock_guard<std::mutex> lock(host.m_hostFileMutex);
            // The IOP's CDVD dropped its handles at load (they reopen lazily).
            for (auto &[handle, file] : host.m_hostFiles)
                streams.push_back(file.stream);
            host.m_hostFiles.clear();
            host.m_nextHostFileHandle = std::max<uint64_t>(r.u64(), 1u);
        }
        for (std::FILE *stream : streams)
        {
            if (stream)
                std::fclose(stream);
        }
    }

    static void saveVfs(PS2Vfs &vfs, StateWriter &w)
    {
        std::lock_guard<std::mutex> lock(vfs.m_mutex);
        w.i32(vfs.m_nextDescriptor);
        std::vector<int32_t> ids;
        for (const auto &[id, descriptor] : vfs.m_descriptors)
            ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        w.size(ids.size());
        for (const int32_t id : ids)
        {
            PS2Vfs::OpenDescriptor &descriptor = vfs.m_descriptors.at(id);
            w.i32(id);
            w.str(descriptor.device);
            w.str(descriptor.path);
            w.u32(descriptor.flags);
            w.i64(descriptor.file ? descriptor.file->seek(0, SEEK_CUR) : -1);
        }
    }

    static void loadVfs(PS2Runtime &rt, StateReader &r)
    {
        PS2Vfs &vfs = rt.m_vfs;
        {
            std::lock_guard<std::mutex> lock(vfs.m_mutex);
            vfs.m_descriptors.clear();
        }
        const int32_t nextDescriptor = r.i32();
        const size_t count = r.size();
        const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
        const PS2VfsMounts mounts{paths.hostRoot, paths.cdRoot, paths.mcRoot};
        for (size_t i = 0; i < count; ++i)
        {
            const int32_t id = r.i32();
            const std::string device = r.str();
            const std::string path = r.str();
            const uint32_t flags = r.u32();
            const int64_t position = r.i64();
            // Reopen without creating or truncating: the file holds what the game wrote.
            const uint32_t reopenFlags = flags & ~(PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC | PS2_FIO_O_EXCL);
            const int32_t opened = vfs.open(path, reopenFlags, mounts, rt.m_romDevice);
            std::lock_guard<std::mutex> lock(vfs.m_mutex);
            const auto it = vfs.m_descriptors.find(opened);
            if (opened < 0 || it == vfs.m_descriptors.end())
            {
                std::cerr << "[snapshot] file descriptor " << id << " ('" << path << "') could not be reopened" << std::endl;
                continue;
            }
            PS2Vfs::OpenDescriptor descriptor = std::move(it->second);
            vfs.m_descriptors.erase(it);
            descriptor.device = device;
            descriptor.flags = flags;
            if (position > 0 && descriptor.file)
                (void)descriptor.file->seek(position, SEEK_SET);
            vfs.m_descriptors[id] = std::move(descriptor);
        }
        std::lock_guard<std::mutex> lock(vfs.m_mutex);
        vfs.m_nextDescriptor = nextDescriptor;
    }

    static void saveAudio(PS2AudioBackend &audio, StateWriter &w)
    {
        std::lock_guard<std::mutex> lock(audio.m_mutex);
        w.u32(audio.m_mostRecentSampleKey);
        w.size(audio.m_loadOrderSamples.size());
        for (const auto &sample : audio.m_loadOrderSamples)
        {
            w.podVector(sample.pcm);
            w.u32(sample.sampleRate);
        }
        w.podVector(audio.m_loadOrderSampleKeys);
        w.sortedMap(audio.m_sampleBank, [&w](const PS2AudioBackend::DecodedSample &sample)
                    {
                        w.podVector(sample.pcm);
                        w.u32(sample.sampleRate); });
    }

    static void loadAudio(PS2AudioBackend &audio, StateReader &r)
    {
        // Voices playing now (a fresh process: none) are not part of a snapshot.
        audio.stopAll();
        std::lock_guard<std::mutex> lock(audio.m_mutex);
        audio.m_mostRecentSampleKey = r.u32();
        audio.m_loadOrderSamples.clear();
        const size_t count = r.size();
        for (size_t i = 0; i < count; ++i)
        {
            PS2AudioBackend::DecodedSample sample;
            r.podVector(sample.pcm);
            sample.sampleRate = r.u32();
            audio.m_loadOrderSamples.push_back(std::move(sample));
        }
        r.podVector(audio.m_loadOrderSampleKeys);
        r.sortedMap(audio.m_sampleBank, [&r](PS2AudioBackend::DecodedSample &sample)
                    {
                        r.podVector(sample.pcm);
                        sample.sampleRate = r.u32(); });
    }

    // ------------------------------------------------------------------ stubs
    struct StubHook
    {
        const char tag[5];
        void (*save)(StateWriter &);
        void (*load)(StateReader &, PS2Runtime &);
    };

    static const std::vector<StubHook> &stubHooks()
    {
        namespace s = ps2_stubs::snapshot;
        static const std::vector<StubHook> hooks = {
            {"sAUD", s::saveAudio, s::loadAudio},
            {"sCD ", s::saveCd, s::loadCd},
            {"sCMP", s::saveCompatibility, s::loadCompatibility},
            {"sDC2", s::saveDeci2, s::loadDeci2},
            {"sDMA", s::saveDma, s::loadDma},
            {"sFIO", s::saveFileIo, s::loadFileIo},
            {"sFNT", s::saveFont, s::loadFont},
            {"sGS ", s::saveGs, s::loadGs},
            {"sIPU", s::saveIpu, s::loadIpu},
            {"sLBC", s::saveLibC, s::loadLibC},
            {"sMC ", s::saveMemoryCard, s::loadMemoryCard},
            {"sMPG", s::saveMpeg, s::loadMpeg},
            {"sPAD", s::savePad, s::loadPad},
            {"sRPC", s::saveRpc, s::loadRpc},
            {"sSIF", s::saveSif, s::loadSif},
            {"sSYS", s::saveSystem, s::loadSystem},
            {"sTTY", s::saveTty, s::loadTty},
            {"sUNI", s::saveUnimplemented, s::loadUnimplemented},
            {"sVU ", s::saveVu, s::loadVu},
        };
        return hooks;
    }

    // ------------------------------------------------------------------ whole state
    // HEAD block (format 8): what a later build needs to decide whether it can read the body.
    static void writeHead(StateWriter &w, const StateWriter &body, const EeScheduler &ee)
    {
        w.beginSection("HEAD");
        w.str(gameIdentity());
        w.str(buildId());
        w.u64(ee.m_vsyncTick);
        w.u64(ee.m_eeCycle);
        const std::set<std::string> &paths = body.sectionPaths();
        w.size(paths.size());
        for (const std::string &path : paths)
        {
            w.str(path);
            w.u32(sectionVersion(path));
        }
        const auto &registry = ps2x::typeRegistry();
        const auto &uses = body.typeUse();
        w.size(uses.size());
        for (const auto &[name, sections] : uses)
        {
            const auto layout = registry.find(name);
            if (layout == registry.end())
                throw ps2x::StateError("snapshot writer: raw type " + name + " is not registered");
            w.str(name);
            w.u32(layout->second.size);
            w.u32(layout->second.align);
            w.size(sections.size());
            for (const std::string &section : sections)
                w.str(section);
        }
        w.endSection();
    }

    struct Head
    {
        std::string game;
        std::string build;
        uint64_t tick = 0u;
        uint64_t eeCycle = 0u;
        std::vector<std::pair<std::string, uint32_t>> sections;
        struct Type
        {
            std::string name;
            ps2x::TypeLayout layout;
            std::vector<std::string> sections;
        };
        std::vector<Type> types;
    };

    static Head readHead(StateReader &r)
    {
        Head head;
        r.beginSection("HEAD");
        head.game = r.str();
        head.build = r.str();
        head.tick = r.u64();
        head.eeCycle = r.u64();
        const size_t sectionCount = r.size(1u << 16);
        for (size_t i = 0; i < sectionCount; ++i)
        {
            std::string path = r.str();
            const uint32_t version = r.u32();
            head.sections.emplace_back(std::move(path), version);
        }
        const size_t typeCount = r.size(1u << 16);
        for (size_t i = 0; i < typeCount; ++i)
        {
            Head::Type type;
            type.name = r.str();
            type.layout.size = r.u32();
            type.layout.align = r.u32();
            const size_t uses = r.size(1u << 16);
            for (size_t j = 0; j < uses; ++j)
                type.sections.push_back(r.str());
            head.types.push_back(std::move(type));
        }
        r.endSection();
        return head;
    }

    // Every reason this runner cannot read the snapshot, each naming its section; empty if it can.
    static std::vector<std::string> headMismatches(const Head &head)
    {
        std::vector<std::string> issues;
        if (head.game != gameIdentity())
            issues.push_back("written for " + head.game + ", this run is " + gameIdentity());
        for (const auto &[path, version] : head.sections)
        {
            const uint32_t mine = sectionVersion(path);
            if (version != mine)
            {
                issues.push_back("section '" + path + "' is version " + std::to_string(version) + " in the snapshot, " +
                                 std::to_string(mine) + " in this runner");
            }
        }
        const auto &registry = ps2x::typeRegistry();
        for (const Head::Type &type : head.types)
        {
            std::string where;
            for (const std::string &section : type.sections)
                where += (where.empty() ? "'" : ", '") + section + "'";
            const auto found = registry.find(type.name);
            if (found == registry.end())
            {
                issues.push_back("section " + where + " holds " + type.name + " raw, which this runner no longer reads raw");
            }
            else if (found->second.size != type.layout.size || found->second.align != type.layout.align)
            {
                issues.push_back("section " + where + " holds " + type.name + ": " + std::to_string(type.layout.size) +
                                 " bytes (align " + std::to_string(type.layout.align) + ") in the snapshot, " +
                                 std::to_string(found->second.size) + " (align " + std::to_string(found->second.align) +
                                 ") in this runner");
            }
        }
        return issues;
    }

    static std::string hexPc(uint32_t pc)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", pc);
        return text;
    }

    // After a load (memory and overlays restored): every guest address the scheduler will
    // dispatch to must have code in this build, or the run would park a thread or skip a
    // handler silently. Refuses naming each one.
    static void checkResumePoints(PS2Runtime &rt)
    {
        const EeScheduler &ee = *rt.m_eeScheduler;
        std::vector<std::string> bad;
        const auto check = [&](uint32_t pc, const std::string &what)
        {
            if (!rt.hasFunction(pc))
                bad.push_back(what + " at " + hexPc(pc));
        };
        const auto checkInvocation = [&](const GuestInvocation &invocation, const std::string &what)
        {
            if (invocation.context.pc != 0u)
                check(invocation.context.pc, what + " (" + invocationKindName(invocation.kind) + ")");
        };
        std::vector<int> ids;
        for (const auto &[id, thread] : ee.m_threads)
            ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        for (const int id : ids)
        {
            const GuestThread &t = ee.m_threads.at(id);
            if (t.status == EeThreadStatus::Dormant)
                continue;
            const std::string name = "thread " + std::to_string(id);
            // A pending SetPc completion replaces the context's pc when the thread resumes.
            if (t.wait.completion && t.wait.resumeTag.kind == EeResumeKind::SetPc)
                check(static_cast<uint32_t>(t.wait.resumeTag.args[0]), name + " wait resume");
            else if (t.resumeCompletion && t.resumeTag.kind == EeResumeKind::SetPc)
                check(static_cast<uint32_t>(t.resumeTag.args[0]), name + " resume");
            else
                check(t.context.pc, name + " pc");
            for (const GuestInvocation &invocation : t.invocations)
                checkInvocation(invocation, name + " invocation");
        }
        for (const GuestInvocation &invocation : ee.m_pendingInvocations)
            checkInvocation(invocation, "queued invocation");
        for (const auto &[id, invocation] : ee.m_deferredInvocations)
            checkInvocation(invocation, "deferred invocation " + std::to_string(id));
        for (const auto &[id, alarm] : ee.m_alarms)
            check(alarm.handler, "alarm " + std::to_string(id) + " handler");
        for (const auto &[id, handler] : ee.m_intcHandlers)
            check(handler.handler, "INTC handler " + std::to_string(id));
        for (const auto &[id, handler] : ee.m_dmacHandlers)
            check(handler.handler, "DMAC handler " + std::to_string(id));
        if (ee.m_gsVSyncCallback != 0u)
            check(ee.m_gsVSyncCallback, "GS VSync callback");
        if (bad.empty())
            return;
        std::sort(bad.begin(), bad.end());
        std::string text = "snapshot resumes guest code this build has no function for (function map changed): ";
        for (size_t i = 0; i < bad.size() && i < 12u; ++i)
            text += (i ? "; " : "") + bad[i];
        if (bad.size() > 12u)
            text += "; and " + std::to_string(bad.size() - 12u) + " more";
        throw ps2x::StateError(text);
    }

    static void save(PS2Runtime &rt, StateWriter &out)
    {
        StateWriter body;
        saveBody(rt, body);
        out.reserve(body.data().size() + 64u * 1024u);
        out.bytes(kMagic, sizeof(kMagic));
        out.u32(kFormatVersion);
        writeHead(out, body, *rt.m_eeScheduler);
        out.bytes(body.data().data(), body.data().size());
    }

    static void saveBody(PS2Runtime &rt, StateWriter &w)
    {
        w.reserve(48u * 1024u * 1024u);
        // Batched timing: apply the deferred EE timer cycles and run the IOP up to now, so
        // every section below is the state at m_eeCycle (a no-op with batching off).
        rt.m_eeScheduler->flushDeferredTime();
        // Asynchronous GS (PS2X_GS_ASYNC): GS memory (MEMR) and state (GS) after every queued command.
        rt.m_gs.sync();
        const EeScheduler &ee = *rt.m_eeScheduler;
        w.beginSection("INFO");
        w.u64(ee.m_vsyncTick);
        w.u64(ee.m_eeCycle);
        w.endSection();

        w.beginSection("EESC");
        saveScheduler(ee, w);
        w.endSection();
        w.beginSection("RTME");
        saveRuntime(rt, w);
        w.endSection();
        w.beginSection("MEMR");
        saveMemory(rt.m_memory, w);
        w.endSection();
        w.beginSection("GS  ");
        saveGs(rt.m_gs, w);
        w.endSection();
        w.beginSection("VU0 ");
        saveVu(rt.m_vu0, w);
        w.endSection();
        w.beginSection("VU1 ");
        saveVu(rt.m_vu1, w);
        w.endSection();
        w.beginSection("IOPS");
        rt.m_iopSubsystem->saveState(w);
        w.endSection();
        w.beginSection("IOPH");
        saveIopHost(*rt.m_iopHost, w);
        w.endSection();
        w.beginSection("VFS ");
        saveVfs(rt.m_vfs, w);
        w.endSection();
        w.beginSection("AUDB");
        saveAudio(rt.m_audioBackend, w);
        w.endSection();
        w.beginSection("STUB");
        for (const StubHook &hook : stubHooks())
        {
            w.beginSection(hook.tag);
            hook.save(w);
            w.endSection();
        }
        w.endSection();
        w.beginSection("SYSC");
        ps2_syscalls::snapshot::save(w);
        w.endSection();
        w.beginSection("END ");
        w.endSection();
    }

    static void load(PS2Runtime &rt, StateReader &r)
    {
        char magic[sizeof(kMagic)]{};
        r.bytes(magic, sizeof(magic));
        if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
            throw ps2x::StateError("not a runner snapshot (bad magic)");
        const uint32_t version = r.u32();
        if (version == kLegacyFormatVersion)
        {
            checkLegacySignature(r);
            loadBody(rt, r);
            return;
        }
        if (version != kFormatVersion)
        {
            throw ps2x::StateError("snapshot container format " + std::to_string(version) + ", this runner reads " +
                                   std::to_string(kLegacyFormatVersion) + " and " + std::to_string(kFormatVersion));
        }
        const Head head = readHead(r);
        if (const std::vector<std::string> issues = headMismatches(head); !issues.empty())
        {
            std::string text = "snapshot does not match this runner: ";
            for (size_t i = 0; i < issues.size(); ++i)
                text += (i ? "; " : "") + issues[i];
            throw ps2x::StateError(text);
        }
        if (head.build != buildId())
            std::cerr << "[snapshot] written by runner build " << head.build << ", this is " << buildId()
                      << ": every section and raw layout matches" << std::endl;
        loadBody(rt, r);
    }

    // Format 7 (before durable snapshots): its signature named the writing build. Its body is
    // the same sections as format 8's, so it loads when the game and the raw struct sizes it
    // recorded still match; the build id and function table range are no longer compared
    // (checkResumePoints covers the function map). Saving again writes format 8.
    static void checkLegacySignature(StateReader &r)
    {
        const size_t size = r.size(1u << 16);
        StateReader s(r.take(size), size);
        (void)s.u32(); // function table base
        (void)s.u32(); // end
        (void)s.u32(); // slots
        const std::pair<const char *, uint32_t> sizes[] = {
            {"R5900Context", static_cast<uint32_t>(sizeof(R5900Context))},
            {"VU1State", static_cast<uint32_t>(sizeof(VU1State))},
            {"GSContext", static_cast<uint32_t>(sizeof(GSContext))},
            {"GSVertex", static_cast<uint32_t>(sizeof(GSVertex))},
            {"VIFRegisters", static_cast<uint32_t>(sizeof(VIFRegisters))},
            {"EeIrqHandler", static_cast<uint32_t>(sizeof(EeIrqHandler))},
            {"EeEvent", static_cast<uint32_t>(sizeof(EeEvent))},
        };
        std::vector<std::string> issues;
        for (const auto &[name, mine] : sizes)
        {
            const uint32_t theirs = s.u32();
            if (theirs != mine)
                issues.push_back(std::string(name) + " is " + std::to_string(theirs) + " bytes in the snapshot, " +
                                 std::to_string(mine) + " in this runner");
        }
        const std::string game = s.str();
        const std::string build = s.str();
        if (game != gameIdentity())
            issues.push_back("written for " + game + ", this run is " + gameIdentity());
        if (!issues.empty())
        {
            std::string text = "format 7 snapshot does not match this runner: ";
            for (size_t i = 0; i < issues.size(); ++i)
                text += (i ? "; " : "") + issues[i];
            throw ps2x::StateError(text);
        }
        std::cerr << "[snapshot] format 7 snapshot (runner build " << build
                  << "): loading by its struct sizes; save again to get a durable format 8 file" << std::endl;
    }

    static void loadBody(PS2Runtime &rt, StateReader &r)
    {
        EeScheduler &ee = *rt.m_eeScheduler;
        rt.m_gs.sync();
        r.beginSection("INFO");
        (void)r.u64();
        (void)r.u64();
        r.endSection();

        r.beginSection("EESC");
        loadScheduler(ee, r, rt);
        r.endSection();
        r.beginSection("RTME");
        loadRuntime(rt, r);
        r.endSection();
        r.beginSection("MEMR");
        loadMemory(rt.m_memory, r);
        r.endSection();
        r.beginSection("GS  ");
        loadGs(rt.m_gs, r);
        r.endSection();
        r.beginSection("VU0 ");
        loadVu(rt.m_vu0, r);
        r.endSection();
        r.beginSection("VU1 ");
        loadVu(rt.m_vu1, r);
        r.endSection();
        r.beginSection("IOPS");
        rt.m_iopSubsystem->loadState(r);
        r.endSection();
        r.beginSection("IOPH");
        loadIopHost(*rt.m_iopHost, r);
        r.endSection();
        r.beginSection("VFS ");
        loadVfs(rt, r);
        r.endSection();
        r.beginSection("AUDB");
        loadAudio(rt.m_audioBackend, r);
        r.endSection();
        r.beginSection("STUB");
        for (const StubHook &hook : stubHooks())
        {
            r.beginSection(hook.tag);
            hook.load(r, rt);
            r.endSection();
        }
        r.endSection();
        r.beginSection("SYSC");
        ps2_syscalls::snapshot::load(r, rt);
        r.endSection();
        r.beginSection("END ");
        r.endSection();
        if (!r.atEnd())
            throw ps2x::StateError("snapshot has trailing data");
        checkResumePoints(rt);
    }

    static void reanchorHostClock(EeScheduler &ee)
    {
        ee.m_hostEpoch = ps2x::ff::now();
        ee.m_hostEpochCycle = ee.m_eeCycle;
    }

    static uint64_t vsyncTick(const PS2Runtime &rt)
    {
        return rt.m_eeScheduler->m_vsyncTick;
    }

    static EeScheduler &scheduler(PS2Runtime &rt)
    {
        return *rt.m_eeScheduler;
    }

    static void resetSchedulerForLoad(PS2Runtime &rt)
    {
        rt.m_eeScheduler->reset(rt.m_memory.getRDRAM(), rt.m_cpuContext);
    }
};

// ---------------------------------------------------------------------- service
bool PS2SnapshotService::quiescent(PS2Runtime &runtime, std::string *why)
{
    return PS2SnapshotAccess::quiescent(runtime, why);
}

bool PS2SnapshotService::saveToBuffer(PS2Runtime &runtime, std::vector<uint8_t> &out, std::string *error)
{
    try
    {
        // Batched timing: the flush (deferred timer cycles, the IOP's pending chunk) can
        // raise interrupts or queue callbacks, so it goes before the quiescence check.
        runtime.eeScheduler().flushDeferredTime();
        std::string why;
        if (!PS2SnapshotAccess::quiescent(runtime, &why))
            throw ps2x::StateError("state is not serialisable now: " + why);
        StateWriter w;
        PS2SnapshotAccess::save(runtime, w);
        out = std::move(w.data());
        const uint64_t checksum = payloadChecksum(out.data(), out.size());
        const auto *bytes = reinterpret_cast<const uint8_t *>(&checksum);
        out.insert(out.end(), bytes, bytes + sizeof(checksum));
        return true;
    }
    catch (const std::exception &e)
    {
        if (error)
            *error = e.what();
        return false;
    }
}

bool PS2SnapshotService::loadFromBuffer(PS2Runtime &runtime, const uint8_t *data, size_t size, std::string *error)
{
    try
    {
        if (size < sizeof(uint64_t))
            throw ps2x::StateError("snapshot too short");
        const size_t payload = size - sizeof(uint64_t);
        uint64_t stored = 0u;
        std::memcpy(&stored, data + payload, sizeof(stored));
        if (stored != payloadChecksum(data, payload))
            throw ps2x::StateError("snapshot checksum mismatch (truncated or corrupt file)");
        StateReader r(data, payload);
        PS2SnapshotAccess::load(runtime, r);
        return true;
    }
    catch (const std::exception &e)
    {
        if (error)
            *error = e.what();
        return false;
    }
}

bool PS2SnapshotService::saveToFile(PS2Runtime &runtime, const std::string &path, std::string *error)
{
    std::vector<uint8_t> data;
    if (!saveToBuffer(runtime, data, error))
        return false;
    const std::filesystem::path target(path);
    std::error_code ec;
    if (target.has_parent_path())
        std::filesystem::create_directories(target.parent_path(), ec);
    const std::filesystem::path temporary = target.string() + ".partial";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out)
        {
            if (error)
                *error = "cannot write " + temporary.string();
            return false;
        }
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec)
    {
        std::filesystem::remove(target, ec);
        std::filesystem::rename(temporary, target, ec);
    }
    if (ec)
    {
        if (error)
            *error = "cannot rename " + temporary.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool PS2SnapshotService::loadFromFile(PS2Runtime &runtime, const std::string &path, std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return loadFromBuffer(runtime, data.data(), data.size(), error);
}

bool PS2SnapshotService::atSafePoint(PS2Runtime &runtime, uint64_t requestTick)
{
    (void)requestTick;
    SnapshotConfig &cfg = config();
    const uint64_t tick = PS2SnapshotAccess::vsyncTick(runtime);
    std::string why;
    if (!PS2SnapshotAccess::quiescent(runtime, &why))
    {
        if (!cfg.refusing)
        {
            cfg.refusing = true;
            cfg.refusedSince = tick;
            cfg.lastRefusalLog = tick;
        }
        else if (tick >= cfg.lastRefusalLog + 600u)
        {
            cfg.lastRefusalLog = tick;
            std::cerr << "[snapshot] still waiting for a safe point since tick " << cfg.refusedSince
                      << " (now " << tick << "): " << why << std::endl;
        }
        return false;
    }
    cfg.refusing = false;

    std::string pattern;
    bool exitAfter = false;
    {
        std::lock_guard<std::mutex> lock(configMutex());
        pattern = cfg.savePattern;
        // Arm the next PS2X_SNAPSHOT_AT tick past this one.
        while (cfg.nextTick < cfg.ticks.size() && cfg.ticks[cfg.nextTick] <= tick)
            ++cfg.nextTick;
        if (cfg.nextTick < cfg.ticks.size())
            PS2SnapshotAccess::scheduler(runtime).setSnapshotTick(cfg.ticks[cfg.nextTick]);
        exitAfter = cfg.exitAfterLast && !cfg.ticks.empty() && cfg.nextTick >= cfg.ticks.size();
    }

    const std::string path = expandPath(pattern, tick);
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    const bool ok = saveToFile(runtime, path, &error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (ok)
    {
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(path, ec);
        std::cerr << "[snapshot] saved " << path << " tick=" << tick << " bytes=" << bytes
                  << " ms=" << elapsed.count() << std::endl;
        std::cout << "[snapshot] saved " << path << " tick=" << tick << std::endl;
        std::string mpegWhy;
        if (!ps2_stubs::snapshot::mpegReplayHealthy(&mpegWhy, false))
        {
            // Only an ended playback gets here (an active one defers the save): harmless
            // unless the game plays that object again without re-creating it.
            std::cerr << "[snapshot] note: " << mpegWhy << " (its stream has ended; fine unless the game reuses it)" << std::endl;
        }
    }
    else
    {
        std::cerr << "[snapshot] save failed at tick " << tick << ": " << error << std::endl;
    }
    // The save took host time the guest did not see: do not let real-time catch-up
    // jump the guest clock forward by it.
    PS2SnapshotAccess::reanchorHostClock(PS2SnapshotAccess::scheduler(runtime));
    if (exitAfter)
    {
        std::cerr << "[snapshot] PS2X_SNAPSHOT_EXIT: stopping after the last save" << std::endl;
        runtime.requestStop();
    }
    return true;
}

bool PS2SnapshotService::enabled()
{
    static const bool value = []
    {
        for (const char *name : {"PS2X_SNAPSHOT_SAVE", "PS2X_SNAPSHOT_AT", "PS2X_SNAPSHOT_LOAD", "PS2X_SNAPSHOT"})
        {
            const char *v = std::getenv(name);
            if (v && *v && std::strcmp(v, "0") != 0)
                return true;
        }
        return false;
    }();
    return value;
}

void PS2SnapshotService::requestFromHost(PS2Runtime &runtime)
{
    if (!enabled())
    {
        std::cerr << "[snapshot] F5 ignored: snapshots are off (set PS2X_SNAPSHOT=1, or PS2X_SNAPSHOT_SAVE/AT)" << std::endl;
        return;
    }
    std::cerr << "[snapshot] F5: snapshot requested" << std::endl;
    PS2SnapshotAccess::scheduler(runtime).requestSnapshot();
}

bool PS2SnapshotService::startup(PS2Runtime &runtime)
{
    SnapshotConfig &cfg = config();
    {
        std::lock_guard<std::mutex> lock(configMutex());
        if (const char *save = std::getenv("PS2X_SNAPSHOT_SAVE"); save && *save)
            cfg.savePattern = save;
        cfg.ticks = parseTickList(std::getenv("PS2X_SNAPSHOT_AT"));
        cfg.nextTick = 0u;
        if (const char *exitAfter = std::getenv("PS2X_SNAPSHOT_EXIT"); exitAfter && *exitAfter && std::strcmp(exitAfter, "0") != 0)
            cfg.exitAfterLast = true;
    }

    bool loaded = false;
    if (const char *load = std::getenv("PS2X_SNAPSHOT_LOAD"); load && *load)
    {
        const auto started = std::chrono::steady_clock::now();
        PS2SnapshotAccess::resetSchedulerForLoad(runtime);
        std::string error;
        if (!loadFromFile(runtime, load, &error))
            throw std::runtime_error(std::string("[snapshot] cannot load ") + load + ": " + error);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        std::cerr << "[snapshot] loaded " << load << " tick=" << PS2SnapshotAccess::vsyncTick(runtime)
                  << " ms=" << elapsed.count() << std::endl;
        std::cout << "[snapshot] loaded " << load << " tick=" << PS2SnapshotAccess::vsyncTick(runtime) << std::endl;
        loaded = true;
    }

    // Arm the first PS2X_SNAPSHOT_AT tick still ahead (a loaded run starts mid-way).
    {
        std::lock_guard<std::mutex> lock(configMutex());
        const uint64_t now = PS2SnapshotAccess::vsyncTick(runtime);
        while (cfg.nextTick < cfg.ticks.size() && loaded && cfg.ticks[cfg.nextTick] < now)
            ++cfg.nextTick;
        if (cfg.nextTick < cfg.ticks.size())
        {
            PS2SnapshotAccess::scheduler(runtime).setSnapshotTick(cfg.ticks[cfg.nextTick]);
            std::cerr << "[snapshot] armed: " << (cfg.ticks.size() - cfg.nextTick) << " save(s) from tick "
                      << cfg.ticks[cfg.nextTick] << " to " << cfg.savePattern << std::endl;
        }
    }
    return loaded;
}
