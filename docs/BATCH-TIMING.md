# Batched EE timer / IOP accounting (`PS2X_BATCH_TIMING`, default off)

Branch `srwz-timer`, 2026-10-07. A speed change to how guest time is charged; off by default
until proven on more than one game.

## The problem

Every inter-function guest call charges 8 EE cycles (`EeScheduler::kGuestDispatchCycles`) and
every backward branch 32 (`kGeneratedCheckpointCycles`) through `checkpointDue` ->
`accountCycles`, which did, on every charge:

- `PS2Memory::advanceEeTimers(n)`: a 64-bit div/mod per running timer (and a GIF_STAT lookup
  to clear FQC);
- `advanceIopEeCycles(n)` -> `IopEmulator::runEeCycles` -> `runCycles(1)`: one full IOP
  scheduler iteration (`servicePendingDmaInterrupts`, `servicePendingGuestCallbacks`,
  `timrman.serviceDue`, `beginNextReady`, a one-instruction `runCpu`, `endTimeslice`).

That is several hundred ns of fixed cost per guest call, most of it IOP bookkeeping for an
IOP that is idle nearly all the time.

## The change

`PS2X_BATCH_TIMING=1` (or `=N`, N >= 8, the bound in EE cycles; `1` = the default bound of
1024 cycles, about 3.5 us) switches `accountCycles` to: `m_eeCycle += n;
memory.deferEeTimerCycles(n);` and a comparison against the next flush cycle. Everything else
happens at a **flush** (`EeScheduler::flushDeferredTime`), which applies the deferred cycles
to the timers in one `advanceEeTimers` call, runs the IOP for all EE cycles charged since its
last run in one `runEeCycles` call, and computes the next flush cycle as the earliest of:

| trigger | how |
|---|---|
| EE timer compare / overflow | `PS2Memory::cyclesUntilNextEeTimerInterrupt()`: the exact cycle (clock remainder included), so the interrupt is raised by the same charge as before |
| IOP event | `IopEmulator::eeCyclesUntilNextEvent()`: the next delayed-thread wake, DMA completion, scheduled callback or IOP timer, in EE cycles (x8 minus the carry). A runnable IOP thread means "within the bound" |
| timer register write | `PS2Memory::eeTimerGeneration()` changes on any T0-T3 write (and when a sync raises an interrupt); `accountCycles` recomputes when it sees a new value |
| the bound | `PS2X_BATCH_TIMING=N`, default 1024 |
| a checkpoint with work | `processPendingEvents` flushes when a deadline, an event or a timer interrupt is pending (a plain return to the scheduler is not a flush) |
| the idle wait | `waitForEvent` flushes before computing its deadline |
| EE-visible IOP operations | `PS2Runtime::syncIopTime()` before `handleIopRpc`, `notifyIopSifTransfer`, `read/write/zeroIopMemory`, `loadIopModule*`, `stopIopModule` (executor thread only; a no-op when nothing is pending) |
| a realtime catch-up chunk | `catchUpToHostClock` defers the chunk and flushes when it reaches the deadline |

Guest reads and writes of T0-T3 COUNT/MODE/COMP/HOLD and of GIF_STAT (its FQC field is cleared
by `advanceEeTimers`) catch up lazily inside `PS2Memory::readIORegister/writeIORegister`
(`syncEeTimers`): the guest never sees a stale count. Interrupt bits a lazy sync raises are
kept in `PS2Memory` until the scheduler's next flush takes them (`takeEeTimerInterrupts`).

With batching on, the IOP also clips a thread's slice inside `runCycles` at its next scheduled
event (`IopEmulator::setBatchedTiming`): the one-cycle calls did that implicitly. Off, the IOP
is byte-for-byte the old code path.

### What stays the same
- `m_eeCycle` (guest time, VBlank and alarm deadlines, time slices): identical charges.
- EE timer interrupts: raised by the same charge as before (the flush deadline is the exact
  compare cycle; one big `advanceEeTimers` equals the sum of small ones, unit-tested).
- Timer register values as the guest reads them.
- IOP cycles per EE cycle (8:1) and IOP-internal event timing (events clip the slices).
- SIF RPC: synchronous in this runtime (`IopRpcBridge::handleRpc` runs the IOP function to
  completion), so its latency is unchanged; a flush precedes it anyway.

### What changes
- EE/IOP interleaving granularity: the IOP runs in chunks of up to the bound (1024 EE cycles =
  128 IOP cycles) instead of one cycle per guest call. An IOP write to EE RAM (a SIF DMA
  completion flag polled by the EE) becomes visible up to one bound later; an EE RAM write
  becomes visible to IOP code up to one bound later. Real hardware has no such lockstep at all.
- A realtime catch-up chunk that does not reach a deadline is deferred to the next flush
  instead of being applied in place (the timer counts the guest reads are unchanged: lazy
  sync).

## Switch off
One `if (m_batchTiming)` per charge, one `if (m_eeTimerDeferredCycles != 0)` per timer
register access, and the IOP's clip flag test per slice. The cherry-picked e846188 (one table
lookup per dispatch, overlay lookup without `std::string`, GIF_STAT pointer cache) is in this
branch too and is on regardless.

## Measurements (2026-10-07, shared machine, this worktree's build with e846188)

Throughput: headless, `PS2X_REALTIME_CLOCK=0` (guest time = charges only, so guest ms is a
measure of guest work done), `PS2X_FASTFORWARD=max`, 150 s host each, off and on alternating.
Guest ms per EE-thread CPU second (`eeCpuMs` on the tick line) over the same guest stretch,
eeMs 2,000 -> 12,000 (boot, logos, the start of the movie):

| run | switch | guest ms / EE CPU s | notes |
|---|---|---|---|
| b1-0 | off | - | boot stalled (eeMs 5,000 after 150 s; shared machine) |
| b1-1 | on | - | same, eeMs 10,000 |
| b2-0 | off | 360 | |
| b2-1 | on | 514 | |
| b3-0 | off | 382 | |
| b3-1 | on | 462 | |

On: +25-40% guest work per EE CPU second in this stretch (two pairs; the noise between runs is
large, the direction is consistent). The batch stats of the on runs: ~35 charges per flush on
average (bound 1024 cycles, charges of 8/32), and the movie's pacing loop reads RCNT0 every
iteration (256 M lazy timer reads in b2-1), each a one-call catch-up.

Correctness (headless, normal realtime clock, `PS2X_MPEG_HASH=25`, GS dumps at the regress
ticks plus every 200 from 26,800, `PS2X_PAD_SCRIPT=28300:START+8`):

| pair | switch | regress (content order vs baseline) | MPEG pictures | title + first menu |
|---|---|---|---|---|
| ff max, 220 s | off | FAIL only on 1,300 blank ticks after the START (the START landed in the movie; then the load to the title) | 182 hashed | title with menu at 30,003 |
| ff max, 220 s | on | PASS 61/82, 0 fail | 330 hashed, all 182 common pictures identical | title with menu at 28,601-30,010 (menu ref mad 3.8) |
| ff x4, 250 s | off | PASS 62/77, 0 fail | 336 hashed | run ended at 29,318 before the title showed |
| ff x4, 250 s | on | PASS 45/81, 0 fail | 306 hashed, all 306 common pictures identical | title with menu at 28,621-29,806 (menu ref mad 3.7) |

Same screens in the same order in all four runs (Bandai Namco, Banpresto, copyright, movie,
title, first menu). The one visible difference: after the scripted START stops the movie, the
eager runs show ~1,300 ticks of black before the title and the batched ones ~200-300. In host
time both loads take ~2.5-2.8 s (the tick lines); at fast-forward the guest clock counts
blocked host time differently depending on how busy the EE thread is, so this is a measure of
the fast-forward clock, not of the guest. A 1x pair settles it (see the end of this file).

## Tests (`ps2x_tests`)
- lazy COUNT read equals eager advancing, clock remainder included;
- register writes apply deferred cycles under the old configuration and bump the generation;
- a sync raises and hands over the compare interrupt; generation bumps;
- one batched step equals many small charges (5 modes x 3 starts x 3 compares, ZRET on/off,
  compare and overflow, all four clocks);
- `cyclesUntilNextEeTimerInterrupt` is exact for every clock and remainder;
- the scheduler fires the Timer2 compare IRQ at the same EE cycle batched and unbatched, and
  the handler reads COUNT == COMP through the lazy path.

## Merge notes
- Snapshots (merged in srwz): `PS2SnapshotService::save()` calls
  `EeScheduler::flushDeferredTime()` before any section is written; the EE section stores
  the batch flag, flush deadline, IOP-synced cycle and timer generation, the memory section
  the deferred cycles, pending interrupt bits and generation, with a `StateError` on save or
  load if anything is still deferred (zero after the flush, asserted). A loading run keeps its
  own `PS2X_BATCH_TIMING` setting. Format version 3. Unit test in `ps2_snapshot_tests.cpp`.
- `[run:tick]` has a new last field `eeCpuMs` (the EE thread's CPU time; `tools\run-report.ps1`
  matches by name, unaffected).

## Real time (1x) runs, headless, 520 s / 400 s, `PS2X_WATCHDOG=5`

| run | switch | trace | result |
|---|---|---|---|
| tm1-b0 | off | - | regress PASS 80/80; title with the first menu at 28,813 (menu ref mad 3.8); movie 413 pictures hashed |
| tm1-b1 | on | - | **movie stalled** at picture 8,850 (tick 19,811): the EE thread parked in `sceMpegGetPicture`'s `waitExternal` with no decoded frame, black from then on; the 355 common pictures identical; guest clock = wall clock throughout (eeMs = hostMs) |
| tm1t-b1 | on | `PS2X_TRACE_STUBS=Mpeg,CdSt` | no stall in 400 s (picture 7,550 at the end); 303 common pictures identical; EE CPU 133 s |
| tm1t-b0 | off | same trace | no stall in 400 s (picture 7,950); EE CPU 195 s for the same progress (-32% CPU batched) |

Pictures per VBlank tick, in windows of 1,000 pictures: the stalled run played at the normal
0.49 right up to its stall; the eager tm1-b0 dropped to 0.17-0.29 over pictures 8,000-10,300
(the same stretch of the movie) and recovered; the traced batched run ran at 0.26-0.37 from
picture 2,000 on (the trace costs host time). That stretch starves the 1x player in both modes:
the game feeds the demuxer only while timer 0 < 500 (guest time, which at 1x is mostly
wall-clock catch-up), decoding is synchronous inside the feed, and once a frame-less
`sceMpegGetPicture` parks the thread nothing feeds any more (the runtime's known 1x hazard,
see the comment in `catchUpToHostClock`). The batched run hit the full stall once out of three
1x runs; the eager run degraded in the same place without stalling (one run). Not shown to be
caused by batching, not shown to be independent of it either: the reviewer should treat the
1x movie as the place to watch, and a second untraced batched 1x run is listed below.
| tm1-b1r | on | - (watchdog on) | second untraced run, the build with the idle-wait fix: whole movie (11,150 pictures, 447 hashed), START taken at 28,301, title reached; one dip to 0.18 pictures/tick at 3,000-4,000, recovered; EE CPU 399 s over 517 s |

So: three batched 1x runs, one stall (in the movie stretch where the eager run also starved).
