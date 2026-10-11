#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_snapshot.h"
#include "ps2x/state_stream.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Runner snapshots (docs/SNAPSHOT-DESIGN.md): every state block round-trips, and
// save -> load -> save gives the same bytes (so the same RDRAM, GS memory, IOP state...).
namespace
{
    constexpr uint32_t kHandlerPc = 0x00200000u;
    constexpr uint32_t kResumePc = 0x00200100u;

    void testHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = 0u;
    }

    // FNV-1a 64, as the snapshot's trailing checksum.
    void resealChecksum(std::vector<uint8_t> &file)
    {
        uint64_t hash = 1469598103934665603ull;
        const size_t payload = file.size() - sizeof(uint64_t);
        for (size_t i = 0; i < payload; ++i)
        {
            hash ^= file[i];
            hash *= 1099511628211ull;
        }
        std::memcpy(file.data() + payload, &hash, sizeof(hash));
    }

    // Offsets into a format-8 snapshot's HEAD block (magic, container version, HEAD tag and
    // length, game, build, tick, cycle, section table, type table).
    struct HeadMap
    {
        size_t buildAt = 0u;    // first byte of the build id string
        size_t buildSize = 0u;
        std::vector<std::pair<std::string, size_t>> sectionVersionAt; // path -> offset of its u32
        std::vector<std::pair<std::string, size_t>> typeSizeAt;       // type name -> offset of its size u32
    };

    HeadMap mapHead(const std::vector<uint8_t> &file)
    {
        HeadMap map;
        size_t at = 8u + 4u + 4u + 8u;
        const auto u64 = [&](size_t &p) { uint64_t v; std::memcpy(&v, file.data() + p, 8u); p += 8u; return v; };
        const auto str = [&](size_t &p) { const size_t n = static_cast<size_t>(u64(p)); std::string s(reinterpret_cast<const char *>(file.data() + p), n); p += n; return s; };
        (void)str(at);
        map.buildSize = static_cast<size_t>(u64(at));
        map.buildAt = at;
        at += map.buildSize;
        at += 16u;
        const size_t sections = static_cast<size_t>(u64(at));
        for (size_t i = 0; i < sections; ++i)
        {
            std::string path = str(at);
            map.sectionVersionAt.emplace_back(path, at);
            at += 4u;
        }
        const size_t types = static_cast<size_t>(u64(at));
        for (size_t i = 0; i < types; ++i)
        {
            std::string name = str(at);
            map.typeSizeAt.emplace_back(name, at);
            at += 8u;
            const size_t uses = static_cast<size_t>(u64(at));
            for (size_t j = 0; j < uses; ++j)
                (void)str(at);
        }
        return map;
    }

    void prepareTarget(PS2Runtime &second, TestCase &t)
    {
        t.IsTrue(second.memory().initialize(), "memory initialize");
        t.IsTrue(second.syncCoreSubsystems(), "core subsystems bind");
        second.registerFunction(kHandlerPc, testHandler);
        second.registerFunction(kResumePc, testHandler);
        R5900Context blank{};
        second.eeScheduler().reset(second.memory().getRDRAM(), blank);
    }

    // A runtime with something in every block the test can reach without a game.
    void populate(PS2Runtime &runtime, TestCase &t)
    {
        t.IsTrue(runtime.memory().initialize(), "memory initialize");
        t.IsTrue(runtime.syncCoreSubsystems(), "core subsystems bind");
        runtime.registerFunction(kHandlerPc, testHandler);
        runtime.registerFunction(kResumePc, testHandler);

        uint8_t *rdram = runtime.memory().getRDRAM();
        for (uint32_t i = 0; i < PS2_RAM_SIZE; i += 4093u)
            rdram[i] = static_cast<uint8_t>(i * 7u + 3u);
        uint8_t *vram = runtime.memory().getGSVRAM();
        for (uint32_t i = 0; i < PS2_GS_VRAM_SIZE; i += 509u)
            vram[i] = static_cast<uint8_t>(i ^ 0x5Au);
        runtime.memory().getScratchpad()[17] = 0x99u;
        runtime.memory().getVU1Data()[64] = 0x42u;
        runtime.memory().gs().dispfb1 = 0x1234u;
        runtime.memory().gs().csr.store(0x2000u);
        runtime.gs().writeRegister(0x4Cu, 0x0000000000140000ull); // FRAME_1
        runtime.vu1().state().vi[3] = 77;
        const uint8_t iopBytes[4] = {1, 2, 3, 4};
        t.IsTrue(runtime.writeIopMemory(0x00100000u, iopBytes, sizeof(iopBytes)), "IOP memory write");

        R5900Context main{};
        main.pc = kHandlerPc;
        SET_GPR_U32(&main, 29, 0x01F00000u);
        EeScheduler &ee = runtime.eeScheduler();
        ee.reset(rdram, main);
        ee.bindMainContextForSyscall(main, rdram);

        EeThreadCreateParams params{};
        params.entry = kHandlerPc;
        params.stack = 0x00180000u;
        params.stackSize = 0x1000u;
        params.priority = 20;
        const int thread = ee.createThread(params);
        t.IsTrue(thread > 1, "thread created");
        t.Equals(ee.startThread(thread, 0x55u, main, false), 0, "thread started");
        t.IsTrue(ee.createSemaphore(1, 4, 0u, 0u) > 0, "semaphore created");
        t.IsTrue(ee.createEventFlag(0x10u, 0u, 0u) > 0, "event flag created");
        t.IsTrue(ee.setAlarm(100u, kHandlerPc, 9u, 0u, 0u) > 0, "alarm set");
        t.IsTrue(ee.addIrqHandler(false, 2u, kHandlerPc, true, 0u, 0u, 0u) > 0, "INTC handler added");

        // The main thread parks in a VSync wait whose completion is a tagged closure.
        try
        {
            runtime.eeWaitVSyncTicks(3u, kResumePc);
        }
        catch (const EeDispatcherTransfer &)
        {
        }
    }
}

void register_ps2_snapshot_tests()
{
    MiniTest::Case("PS2Snapshot", [](TestCase &tc)
    {
        tc.Run("state streams round-trip and check section boundaries", [](TestCase &t)
        {
            ps2x::StateWriter w;
            w.beginSection("ABCD");
            w.u32(7u);
            w.str("hello");
            std::unordered_map<uint32_t, uint32_t> map{{3u, 30u}, {1u, 10u}};
            w.podMap(map);
            w.endSection();
            ps2x::StateReader r(w.data().data(), w.data().size());
            r.beginSection("ABCD");
            t.Equals(r.u32(), 7u, "u32");
            t.Equals(r.str(), std::string("hello"), "string");
            std::unordered_map<uint32_t, uint32_t> back;
            r.podMap(back);
            t.Equals(back.at(3u), 30u, "map value");
            r.endSection();
            t.IsTrue(r.atEnd(), "all consumed");

            ps2x::StateReader shortRead(w.data().data(), w.data().size());
            shortRead.beginSection("ABCD");
            (void)shortRead.u32();
            bool threw = false;
            try
            {
                shortRead.endSection();
            }
            catch (const ps2x::StateError &)
            {
                threw = true;
            }
            t.IsTrue(threw, "a block that reads less than it wrote is reported");

            ps2x::StateReader wrongTag(w.data().data(), w.data().size());
            threw = false;
            try
            {
                wrongTag.beginSection("WXYZ");
            }
            catch (const ps2x::StateError &)
            {
                threw = true;
            }
            t.IsTrue(threw, "a wrong section tag is reported");
        });

        tc.Run("save, load into a fresh runtime, save again: identical", [](TestCase &t)
        {
            PS2Runtime first;
            populate(first, t);
            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(first, saved, &error), "save: " + error);
            t.IsTrue(saved.size() > PS2_RAM_SIZE + PS2_GS_VRAM_SIZE, "RDRAM and GS memory are in the snapshot");

            PS2Runtime second;
            t.IsTrue(second.memory().initialize(), "memory initialize");
            t.IsTrue(second.syncCoreSubsystems(), "core subsystems bind");
            second.registerFunction(kHandlerPc, testHandler);
            second.registerFunction(kResumePc, testHandler);
            R5900Context blank{};
            second.eeScheduler().reset(second.memory().getRDRAM(), blank);
            t.IsTrue(PS2SnapshotService::loadFromBuffer(second, saved.data(), saved.size(), &error), "load: " + error);

            t.IsTrue(std::memcmp(first.memory().getRDRAM(), second.memory().getRDRAM(), PS2_RAM_SIZE) == 0,
                     "RDRAM restored");
            t.IsTrue(std::memcmp(first.memory().getGSVRAM(), second.memory().getGSVRAM(), PS2_GS_VRAM_SIZE) == 0,
                     "GS memory restored");
            t.Equals(second.memory().gs().dispfb1, static_cast<uint64_t>(0x1234u), "GS privileged registers restored");
            t.Equals(second.vu1().state().vi[3], 77, "VU1 registers restored");
            uint8_t iopBytes[4]{};
            t.IsTrue(second.readIopMemory(0x00100000u, iopBytes, sizeof(iopBytes)) && iopBytes[3] == 4u,
                     "IOP memory restored");
            t.Equals(second.eeScheduler().currentVSyncTick(), first.eeScheduler().currentVSyncTick(), "VBlank tick");

            const GuestThread *main = second.eeScheduler().thread(EeScheduler::kMainThreadId);
            t.IsTrue(main != nullptr && main->status == EeThreadStatus::Waiting &&
                         main->wait.reason == EeWaitReason::VSync,
                     "the main thread still waits for VSync");
            t.IsTrue(main != nullptr && static_cast<bool>(main->wait.completion) &&
                         main->wait.resumeTag.kind == EeResumeKind::SetPc,
                     "its completion closure is rebuilt from the tag");
            if (main && main->wait.completion)
            {
                R5900Context probe{};
                main->wait.completion(probe);
                t.Equals(probe.pc, kResumePc, "the rebuilt completion resumes at the saved pc");
            }

            std::vector<uint8_t> again;
            t.IsTrue(PS2SnapshotService::saveToBuffer(second, again, &error), "second save: " + error);
            t.Equals(again.size(), saved.size(), "same snapshot size");
            t.IsTrue(again == saved, "save -> load -> save gives identical bytes");
        });

        tc.Run("a long triangle strip in progress (kick counter past the queue size) saves and loads", [](TestCase &t)
        {
            PS2Runtime first;
            populate(first, t);
            first.gs().writeRegister(0x00u, 0x4ull); // PRIM: triangle strip, no PRIM write after it
            for (uint32_t i = 0; i < 40u; ++i)
                first.gs().writeRegister(0x05u, (static_cast<uint64_t>(i & 7u) << 4) | (static_cast<uint64_t>(i) << 20)); // XYZ2
            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(first, saved, &error), "save: " + error);

            PS2Runtime second;
            t.IsTrue(second.memory().initialize(), "memory initialize");
            t.IsTrue(second.syncCoreSubsystems(), "core subsystems bind");
            second.registerFunction(kHandlerPc, testHandler);
            second.registerFunction(kResumePc, testHandler);
            R5900Context blank{};
            second.eeScheduler().reset(second.memory().getRDRAM(), blank);
            t.IsTrue(PS2SnapshotService::loadFromBuffer(second, saved.data(), saved.size(), &error),
                     "load (SRWZ battle map, Wall 16: 'GS vertex queue indices out of range'): " + error);
            std::vector<uint8_t> again;
            t.IsTrue(PS2SnapshotService::saveToBuffer(second, again, &error), "second save: " + error);
            t.IsTrue(again == saved, "save -> load -> save gives identical bytes");
        });

        tc.Run("a thread inside a guest callback is saved when the completion is tagged", [](TestCase &t)
        {
            PS2Runtime runtime;
            populate(runtime, t);
            EeScheduler &ee = runtime.eeScheduler();
            GuestThread *main = ee.thread(EeScheduler::kMainThreadId);
            t.IsTrue(main != nullptr, "main thread exists");
            if (!main)
                return;
            // An untagged completion blocks the save; a tagged one does not.
            GuestInvocation untagged{};
            untagged.kind = GuestInvocationKind::RpcCallback;
            untagged.context.pc = kHandlerPc;
            untagged.onComplete = [](const R5900Context &, R5900Context &) {};
            main->invocations.push_back(std::move(untagged));
            std::string why;
            t.IsFalse(PS2SnapshotService::quiescent(runtime, &why), "an untagged completion defers the save");
            t.IsTrue(why.find("RPC/stream callback") != std::string::npos, "the reason names the callback kind: " + why);
            main->invocations.back().completeTag = {GuestInvocationCompleteKind::CopyReturnValue, 0u};
            SET_GPR_U32(&main->invocations.back().context, 2, 0x1234u);
            t.IsTrue(PS2SnapshotService::quiescent(runtime, &why), "a tagged completion is serialisable: " + why);

            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(runtime, saved, &error), "save: " + error);
            PS2Runtime second;
            t.IsTrue(second.memory().initialize() && second.syncCoreSubsystems(), "target ready");
            second.registerFunction(kHandlerPc, testHandler);
            second.registerFunction(kResumePc, testHandler);
            R5900Context blank{};
            second.eeScheduler().reset(second.memory().getRDRAM(), blank);
            t.IsTrue(PS2SnapshotService::loadFromBuffer(second, saved.data(), saved.size(), &error), "load: " + error);
            const GuestThread *restored = second.eeScheduler().thread(EeScheduler::kMainThreadId);
            t.IsTrue(restored != nullptr && restored->invocations.size() == 1u, "the invocation stack is restored");
            if (restored && restored->invocations.size() == 1u)
            {
                const GuestInvocation &invocation = restored->invocations.back();
                t.Equals(invocation.context.pc, kHandlerPc, "the callback context is restored");
                t.IsTrue(invocation.kind == GuestInvocationKind::RpcCallback, "the kind is restored");
                t.IsTrue(static_cast<bool>(invocation.onComplete), "the completion closure is rebuilt");
                R5900Context parent{};
                if (invocation.onComplete)
                    invocation.onComplete(invocation.context, parent);
                t.Equals(getRegU32(&parent, 2), 0x1234u, "the rebuilt completion copies the return value");
            }
            std::vector<uint8_t> again;
            t.IsTrue(PS2SnapshotService::saveToBuffer(second, again, &error), "second save: " + error);
            t.IsTrue(again == saved, "save -> load -> save gives identical bytes with an invocation in flight");
        });

        tc.Run("eager timing: a save after charged cycles loads, and GIF_STAT reads after the load", [](TestCase &t)
        {
            // Batching off is the default: m_iopSyncedCycle is never moved by the eager path, and
            // the loaded I/O register map must not leave advanceEeTimers with a stale GIF_STAT
            // slot pointer (the loaded map is a fresh one).
            constexpr uint32_t kGifStat = 0x10003020u;
            constexpr uint32_t kT0Count = 0x10000000u;
            constexpr uint32_t kT0Mode = 0x10000010u;
            PS2Runtime first;
            populate(first, t);
            first.eeScheduler().setRealtimeClock(false);
            first.memory().writeIORegister(kT0Count, 0u);
            first.memory().writeIORegister(kT0Mode, 0x82u);
            first.memory().write32(kGifStat, 0u);
            first.memory().read32(kGifStat); // the slot exists and is cached
            for (int i = 0; i < 8; ++i)
                first.eeScheduler().accountCycles(600u); // eager: timers and IOP advance at once
            t.IsTrue(first.eeScheduler().eeCycle() > 0u, "cycles were charged before the save");
            t.IsFalse(first.eeScheduler().batchTimingEnabled(), "batching is off");

            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(first, saved, &error), "save: " + error);

            PS2Runtime second;
            t.IsTrue(second.memory().initialize(), "memory initialize");
            t.IsTrue(second.syncCoreSubsystems(), "core subsystems bind");
            second.registerFunction(kHandlerPc, testHandler);
            second.registerFunction(kResumePc, testHandler);
            R5900Context blank{};
            second.eeScheduler().reset(second.memory().getRDRAM(), blank);
            second.eeScheduler().setRealtimeClock(false);
            second.memory().write32(kGifStat, 0u);
            second.memory().read32(kGifStat); // cache a slot that the load will invalidate
            t.IsTrue(PS2SnapshotService::loadFromBuffer(second, saved.data(), saved.size(), &error), "load after charged cycles: " + error);
            t.Equals(second.eeScheduler().eeCycle(), first.eeScheduler().eeCycle(), "EE cycle restored");
            t.Equals(second.memory().readIORegister(kT0Count), 4800u / 512u, "COUNT restored");
            // advanceEeTimers touches the GIF_STAT slot: with a stale pointer this is a use after free.
            second.eeScheduler().accountCycles(512u);
            t.Equals(second.memory().read32(kGifStat) & 0x1F000000u, 0u, "GIF_STAT readable after the load, FQC clear");
            t.Equals(second.memory().readIORegister(kT0Count), 5312u / 512u, "COUNT counts on after the load");

            std::vector<uint8_t> again;
            t.IsTrue(PS2SnapshotService::saveToBuffer(second, again, &error), "second save: " + error);
            t.IsTrue(again.size() == saved.size(), "same snapshot size");
        });

        tc.Run("batched timing: a save mid-batch flushes first and the load reads the same timer COUNT", [](TestCase &t)
        {
            // PS2X_BATCH_TIMING defers cycle charges (docs/BATCH-TIMING.md): a save must apply
            // them first, and the loaded runtime must count on from exactly the same state.
            constexpr uint32_t kT0Count = 0x10000000u;
            constexpr uint32_t kT0Mode = 0x10000010u;
            constexpr uint32_t kBusClockDiv256Cue = 0x82u;
            PS2Runtime first;
            populate(first, t);
            EeScheduler &ee = first.eeScheduler();
            ee.setRealtimeClock(false);
            ee.setBatchTiming(4096u);
            first.memory().writeIORegister(kT0Count, 0u);
            first.memory().writeIORegister(kT0Mode, kBusClockDiv256Cue);
            for (int i = 0; i < 5; ++i)
                ee.accountCycles(300u); // 1500 cycles, well inside the bound: deferred
            t.IsTrue(first.memory().eeTimerDeferredCycles() != 0u, "cycles are deferred before the save");

            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(first, saved, &error), "save: " + error);
            t.Equals(first.memory().eeTimerDeferredCycles(), static_cast<uint64_t>(0u), "the save flushed the deferred cycles");
            t.Equals(first.memory().readIORegister(kT0Count), 1500u / 512u, "COUNT after 1500 cycles at BUSCLK/256");

            PS2Runtime second;
            t.IsTrue(second.memory().initialize(), "memory initialize");
            t.IsTrue(second.syncCoreSubsystems(), "core subsystems bind");
            second.registerFunction(kHandlerPc, testHandler);
            second.registerFunction(kResumePc, testHandler);
            R5900Context blank{};
            second.eeScheduler().reset(second.memory().getRDRAM(), blank);
            second.eeScheduler().setRealtimeClock(false);
            second.eeScheduler().setBatchTiming(4096u);
            t.IsTrue(PS2SnapshotService::loadFromBuffer(second, saved.data(), saved.size(), &error), "load: " + error);
            t.Equals(second.memory().eeTimerDeferredCycles(), static_cast<uint64_t>(0u), "nothing deferred after the load");
            t.Equals(second.memory().readIORegister(kT0Count), first.memory().readIORegister(kT0Count), "COUNT restored");
            t.Equals(second.eeScheduler().eeCycle(), first.eeScheduler().eeCycle(), "EE cycle restored");

            // Both count on through the deferred path: the remainder survived the round trip.
            for (int i = 0; i < 4; ++i)
            {
                first.eeScheduler().accountCycles(300u);
                second.eeScheduler().accountCycles(300u);
            }
            t.Equals(second.memory().readIORegister(kT0Count), first.memory().readIORegister(kT0Count), "same COUNT after more charges");
            t.Equals(second.memory().readIORegister(kT0Count), 2700u / 512u, "COUNT after 2700 cycles");

            std::vector<uint8_t> again;
            t.IsTrue(PS2SnapshotService::saveToBuffer(second, again, &error), "second save: " + error);
            t.IsTrue(again.size() == saved.size(), "same snapshot size after the extra charges' flush");
        });

        tc.Run("durable: another runner build loads when every section and raw layout matches", [](TestCase &t)
        {
            // Today's failure: every relink changed the build id and refused every snapshot.
            PS2Runtime first;
            populate(first, t);
            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(first, saved, &error), "save: " + error);
            const HeadMap head = mapHead(saved);
            t.IsTrue(head.buildSize > 0u, "the build id is recorded");
            std::vector<uint8_t> relinked = saved;
            relinked[head.buildAt] ^= 0x01u; // another link of the same sources
            resealChecksum(relinked);
            PS2Runtime second;
            prepareTarget(second, t);
            t.IsTrue(PS2SnapshotService::loadFromBuffer(second, relinked.data(), relinked.size(), &error),
                     "a snapshot from another build of the same layouts loads: " + error);
            std::vector<uint8_t> again;
            t.IsTrue(PS2SnapshotService::saveToBuffer(second, again, &error), "second save: " + error);
            t.IsTrue(again == saved, "and saves back to the same bytes");
        });

        tc.Run("durable: a section version or a raw layout that differs is refused by name", [](TestCase &t)
        {
            PS2Runtime first;
            populate(first, t);
            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(first, saved, &error), "save: " + error);
            const HeadMap head = mapHead(saved);
            t.IsTrue(head.sectionVersionAt.size() > 10u, "the section table lists the sections: " +
                                                             std::to_string(head.sectionVersionAt.size()));
            t.IsTrue(!head.typeSizeAt.empty(), "the type table lists raw types");

            size_t gsAt = 0u;
            for (const auto &[path, at] : head.sectionVersionAt)
                if (path == "GS  ")
                    gsAt = at;
            t.IsTrue(gsAt != 0u, "the GS section is listed");
            std::vector<uint8_t> newerGs = saved;
            newerGs[gsAt] = 2u;
            resealChecksum(newerGs);
            PS2Runtime second;
            prepareTarget(second, t);
            t.IsFalse(PS2SnapshotService::loadFromBuffer(second, newerGs.data(), newerGs.size(), &error),
                      "a section version this runner does not read is refused");
            t.IsTrue(error.find("'GS  ' is version 2") != std::string::npos, "the refusal names the section: " + error);

            std::string contextName;
            size_t contextAt = 0u;
            for (const auto &[name, at] : head.typeSizeAt)
                if (name.find("R5900Context") != std::string::npos)
                {
                    contextName = name;
                    contextAt = at;
                }
            t.IsTrue(contextAt != 0u, "the EE context is recorded as a raw type");
            std::vector<uint8_t> grown = saved;
            grown[contextAt] += 16u;
            resealChecksum(grown);
            PS2Runtime third;
            prepareTarget(third, t);
            t.IsFalse(PS2SnapshotService::loadFromBuffer(third, grown.data(), grown.size(), &error),
                      "a raw struct of another size is refused");
            t.IsTrue(error.find("'EESC'") != std::string::npos && error.find("R5900Context") != std::string::npos,
                     "the refusal names the section and the type: " + error);
        });

        tc.Run("durable: a resume point with no function in this build is refused by name", [](TestCase &t)
        {
            // The function table is global in the test binary, so the missing function is an
            // address no test registers: the main thread's wait resumes there.
            constexpr uint32_t kOrphanPc = 0x00ABC000u;
            PS2Runtime first;
            populate(first, t); // the main thread waits with a SetPc completion
            GuestThread *main = first.eeScheduler().thread(EeScheduler::kMainThreadId);
            t.IsTrue(main != nullptr && main->wait.resumeTag.kind == EeResumeKind::SetPc, "main waits with a SetPc tag");
            if (!main)
                return;
            main->wait.resumeTag.args[0] = kOrphanPc;
            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(first, saved, &error), "save: " + error);
            PS2Runtime second;
            prepareTarget(second, t);
            t.IsFalse(PS2SnapshotService::loadFromBuffer(second, saved.data(), saved.size(), &error),
                      "resuming at an address with no code is refused");
            t.IsTrue(error.find("thread 1 wait resume at 0x00ABC000") != std::string::npos,
                     "the refusal names the thread and the address: " + error);
        });

        tc.Run("a queued guest callback defers the save; corrupt files are refused", [](TestCase &t)
        {
            PS2Runtime runtime;
            populate(runtime, t);
            runtime.eeScheduler().dispatchIrq(false, 2u); // queues the INTC handler invocation
            std::string why;
            t.IsFalse(PS2SnapshotService::quiescent(runtime, &why), "not serialisable with a queued callback");
            t.IsTrue(why.find("callback") != std::string::npos, "the reason names the callback: " + why);

            PS2Runtime clean;
            populate(clean, t);
            std::vector<uint8_t> saved;
            std::string error;
            t.IsTrue(PS2SnapshotService::saveToBuffer(clean, saved, &error), "save: " + error);
            std::vector<uint8_t> truncated(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(saved.size() / 2u));
            PS2Runtime target;
            t.IsTrue(target.memory().initialize() && target.syncCoreSubsystems(), "target ready");
            R5900Context blank{};
            target.eeScheduler().reset(target.memory().getRDRAM(), blank);
            t.IsFalse(PS2SnapshotService::loadFromBuffer(target, truncated.data(), truncated.size(), &error),
                      "a truncated snapshot is refused");
            std::vector<uint8_t> badMagic = saved;
            badMagic[0] = 'X';
            t.IsFalse(PS2SnapshotService::loadFromBuffer(target, badMagic.data(), badMagic.size(), &error),
                      "a file without the magic is refused");
        });
    });
}
