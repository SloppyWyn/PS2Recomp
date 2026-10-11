# Runner snapshots (save / restore the whole runtime)

`PS2X_SNAPSHOT_SAVE` / `PS2X_SNAPSHOT_AT` / F5 write a snapshot of everything the runner
holds; `PS2X_SNAPSHOT_LOAD` resumes from one. Game-agnostic: nothing in the code knows
which game runs.

## Verdict: tractable, at the EE dispatcher's loop head

Guest EE threads are **not** host threads or fibers. `EeScheduler::run()`
(`ps2xRuntime/src/lib/Kernel/EeScheduler.cpp:173`) is one dispatcher loop on one host
thread ("GameThread", `ps2_runtime.cpp:2424`); a guest thread is a `GuestThread` record
holding an `R5900Context` (`include/runtime/ee_scheduler.h`). The dispatcher calls the
recompiled function containing `context.pc` (`EeScheduler.cpp:313-324`).

Recompiled functions can be entered mid-function: every generated function starts with
`switch (ctx->pc)` over all its internal labels (call-return points and loop heads), e.g.
`FUN_0018b260_0x18b260.cpp`. Guest calls nest natively (`PS2Runtime::dispatchGuestBranch`,
`ps2_runtime.cpp:1397`), but the native stack is only a cache:

- a backward branch or a call charges cycles and, when an event or the end of the time
  slice is due, returns with `ctx->pc` at the loop head / call target
  (`control_flow_emitter.cpp:180`, `ps2_runtime.cpp:1411`); every caller sees
  `ctx->pc != fallthroughPc` and returns too (`ps2_runtime.cpp:1458-1463`), so the native
  stack unwinds completely back to the dispatcher;
- a blocking kernel call (WaitSema, WaitEventFlag, SleepThread, VSync, MPEG/CD waits)
  throws `EeDispatcherTransfer` (`EeScheduler.cpp:1739-1748`), which unwinds to the
  dispatcher; the stub wrapper has already set `ctx->pc = $ra`
  (`WaitSema_0x11b0a0.cpp`), so the thread later resumes at the caller's return label.

So at the top of the dispatcher loop no guest frame lives on the host stack: a thread is
completely described by its `R5900Context` plus guest RAM (its stack frames are in RDRAM).
Restoring a context and letting the dispatcher run it is exactly what the dispatcher does
after every preemption. No new re-entry capability is needed.

The IOP is an R3000 interpreter (`ps2xIOP/src/emulator/iop_emulator.cpp`) driven
synchronously from the same host thread: `EeScheduler::accountCycles` ->
`PS2Runtime::advanceIopEeCycles` -> `IopEmulator::runEeCycles` -> `runCycles`, which runs
to the end of its slice (`iop_emulator.cpp:556-589`); RPCs are synchronous calls from EE
stubs. Between those calls the IOP is quiescent: every IOP thread's `IopCpuState` is in
the kernel's thread map (`m_currentThread == nullptr` after `endTimeslice`,
`iop_kernel.cpp:710`), and nested host calls (`callFunction`) have returned.

The only other host threads are the window/present loop (reads GS memory under the GS
locks, polls the keyboard), the watchdog (debug only) and raylib's audio callback (own
mutex). None owns guest state.

### Snapshot point and quiescence

`EeScheduler::run()` checks a "snapshot wanted" flag at the loop head, right after
`processPendingEvents()`. It saves only when:

1. no thread has an active `GuestInvocation` (interrupt handler, alarm, RPC end
   function, SIF command handler, MPEG stream callback, exit handler) and none is pending:
   their `onComplete` closures are host lambdas that cannot be serialised. They last
   microseconds of guest time, so the save just waits for the next loop head;
2. every wait completion closure carries a serialisable tag (below);
3. the GIF arbiter queue is empty.

If a request stays unsatisfiable for 600 VBlanks the runner logs why.

### Wait completions (the one design change in the scheduler)

Three stubs park a thread with a host closure that re-runs the stub on wake-up
(`EeWaitState::completion` / `GuestThread::resumeCompletion`):
`sceMpegGetPicture` (waits for a decoded picture or its presentation VBlank,
`MPEG.cpp:2302,2339`), `sceCdStRead` (waits for streamed sectors, `CD.cpp:782`) and
`PS2Runtime::eeWaitVSyncTicks` (sets a resume pc). Each now passes an `EeResumeTag`
(kind + four integers: the closure's captured values) next to the closure; a snapshot
stores the tag and a load rebuilds the closure from it (`ps2_snapshot.cpp`,
`rebuildResume`). A closure without a tag blocks saving (rule 2), so a future stub that
forgets the tag fails safe.

## What a snapshot holds

Binary file, little-endian: `PS2XSNAP`, the container format (8), a `HEAD` block (below,
"Durable snapshots"), then tagged sections:

| Section | Contents | Where it lives |
|---|---|---|
| `EESC` | guest threads (contexts, status, priorities, wait records + tags, wakeup/suspend counts), ready queues, semaphores, event flags, alarms, INTC/DMAC handlers and masks, id counters, VSync flag/tick addresses, GS VSync callback, pending events, timed events (as cycle offsets), invocation stack map, `m_eeCycle`, VBlank tick, time slice | `EeScheduler` |
| `RTME` | main context copy, exit handlers, syscall overrides + mirror addresses, runtime guest heap blocks, async callback stack window, loaded module list | `PS2Runtime` |
| `MEMR` | RDRAM 32 MB, scratchpad 16 KB, GS VRAM 4 MB, IOP RAM mirror, VU0/VU1 code+data, GS privileged registers (CSR, VSync tick), VIF0/1 and DMA channel registers, IO register map, TLB, EE timers, GIF path-3 mask FIFO, pending DMA transfers, completed DMAC causes, code-modification regions | `PS2Memory` |
| `GSFE` | both GS contexts, PRIM/PRMODE, vertex queue and RGBAQ/ST/UV/FOG, TEXA, TEXCLUT, BITBLTBUF/TRXPOS/TRXREG/TRXDIR, DIMX/DTHE/COLCLAMP/SCANMSK, presentation-source state | `GS` |
| `GSBE` | CLUT buffer, in-flight image transfer, local-to-host buffer | `GSCpuBackend` |
| `VU0 ` `VU1 ` | registers, flags, all pipelines (FMAC/FDIV/EFU/stores/XGKICK), cycle counters (a program paused at its cycle budget resumes) | `VU1Interpreter` |
| `IOPS` | IOP RAM 2 MB + scratch + hardware register map + allocations + IRQ registers; IOP threads (R3000 state), semaphores, event flags; loaded IRX modules; export tables (imports); RPC servers; intrman handlers; timrman timers; ioman devices; CDVD state; pending DMA IRQs and callbacks; cycle counters; HLE module records | `ps2xIOP` |
| `IOPH` | host adapter call token counter | `PS2IopHostAdapter` |
| `VFS ` | open descriptors (path, flags, position) | `PS2Vfs` |
| `AUDB` | decoded sample bank | `PS2AudioBackend` |
| `STUB` | per stub file: audio (libsd transfers), CD (stream timing, streaming LBNs, pseudo-LBN registry), DMA env, GS (graphics mode / gparam), memory card (open files: path + position), MPEG (see below), pad ports, SIF (registers, command handlers), libc FILE handles, plus each file's copy of `Stubs/Helpers/Support.h` globals | `Kernel/Stubs/*.cpp` |
| `SYSC` | SIF RPC servers/clients and counters, boot mode, TLS, OSD config, cwd, SIF module records, DECI2 sessions, VAG accumulators | `Kernel/Syscalls` |

Host-side resources are reopened on load: VFS, memory-card and libc files by path at the
saved position; the IOP CDVD drops its host handles (they reopen lazily on the next
sector read) and rebuilds its virtual ISO index from the disc folder.

### MPEG decoder (FFmpeg)

FFmpeg's decoder state cannot be serialised. Each playback keeps a **replay log**: every
chunk fed to the decoder since the second-newest MPEG sequence header. On load a fresh
decoder is fed the log, the replayed pictures up to and including the last one the
original decoder had produced are dropped (matched by PTS and a sparse content hash), and
the already-decoded queue is restored from the file. The new decoder then holds the same
reference pictures as the old one. The log is recorded only when snapshots are opted in
(below). If it outgrows the limit (64 MB, `PS2X_MPEG_REPLAY_MB` raises it) it is dropped
with one `[snapshot:mpeg] ... dropped` line. From then on a save is **deferred while that
playback is still running** (`still waiting for a safe point ... replay log dropped`): a
restore of it would hold a decoder that never produces a picture again and the game would
wait forever (seen on SRWZ: a save at tick 27400 caught the movie's last seconds). Once
the stream has ended the save goes ahead with a `[snapshot] note:` line, since the game
normally deletes the object before playing anything else. A stream with a single sequence
header (SRWZ's opening movie: ~250 MB for the whole movie) needs a limit covering the
whole stream for an exact restore anywhere inside it.

### Guest callbacks in flight (invocation stacks)

A thread inside a guest callback (`GuestThread::invocations`: interrupt, alarm, GS VSync,
MPEG stream, SIF command, syscall override, exit handler, IPU init) has a host closure
`onComplete`. Each site also sets a `GuestInvocationCompleteTag` describing it
(`GuestFree(addr)`, `CopyReturnValue`, `ExitCurrent(delete)`, `IpuInitComplete`); the
snapshot stores every thread's invocation stack (kind, sequence, tag, context) and
rebuilds the closures on load. Only an invocation with an untagged closure (the RPC
server call and RPC end-function callbacks in `Syscalls/RPC.cpp`) defers the save, and
the log then names the callback kind, its pc and the thread's status. (SRWZ's movie
thread sits inside a stream callback for the whole movie, so this is what makes
mid-movie saves possible.)

The synchronous stream callbacks of one `sceMpegDemuxPss/Ring` call run as a chain whose
completions carry the call's state (untagged), so a blocking stream callback postpones
snapshots until its chain ends; between demux calls the playback's stream position and
replay region (the bytes a callback's 0 rolled back) are saved with the MPEG state.

### File integrity

The file ends with an FNV-1a 64 checksum of everything before it (a truncated or corrupt
file is refused before anything is restored). Raw-restored indices are range-checked (GS
vertex queue, GS transfer state and local-to-host read position, code regions).

### Durable snapshots (format 8)

Up to format 7 the header named the runner build (PE link time stamp), so every relink
refused every snapshot even when no state had changed, and each probe of a late scene cost a
replay. From format 8 a snapshot loads into any later build that can still read it, and is
refused, by name, only where it cannot:

- `HEAD` holds the game (ELF name), the writing build (for logs and checkpoint manifests
  only), the VBlank tick and EE cycle, every section path written with its **section
  version**, and every type written raw (`pod` and its containers) with its size, alignment
  and the sections holding it. The state stream records the raw types as they are written;
  the loader knows its own layouts from a registry filled at static initialisation
  (`ps2x::typeRegistry`, `state_stream.h`), so it checks without saving anything.
- A load refuses before restoring anything when the game differs, a section's version
  differs, or a raw type a section holds changed size or alignment (or is no longer read
  raw), listing every such section: e.g. `section 'EESC' holds struct R5900Context: 656
  bytes (align 16) in the snapshot, 672 (align 16) in this runner`.
- Fields written one by one are covered by the section length check: a section that reads
  more or less than it wrote names itself (`... its layout changed without a section version
  bump`).
- What none of that sees, a reorder of equal-sized fields or a new meaning for a field, is
  the writer's job: bump that section's entry in `sectionVersions()` (`ps2_snapshot.cpp`).
  The container version changes only with the container itself.
- After the load, every guest address the scheduler will dispatch to must have code in this
  build (each live thread's pc or pending `SetPc` resume point, invocation contexts, queued
  and deferred invocations, alarm, INTC and DMAC handlers, the GS VSync callback); one that
  does not refuses the snapshot naming the thread or handler and the address, instead of
  parking the thread or skipping the handler later. This replaces the old function-table
  range check: a changed function map is fine as long as the saved resume points exist.
- Format 7 files still load: their struct sizes and game are checked, the build id is not,
  and the resume-point check runs. Saving again writes format 8.

`tools/snapinfo.py <file> [--sections] [--types]` prints a snapshot's `HEAD` without a
runner; `tools\checkpoint.ps1` keeps named snapshots with their manifests.

## Not covered, and why

- **Sound being played by the host** (raylib voices). The sample bank is restored, voices
  in flight are not: they stop at the snapshot. libsd/IOP state is restored, so the game
  keeps issuing sound commands normally.
- **File contents** of memory-card and libc files: only path, flags and position are
  saved and the file is reopened; a file opened for writing ("w") is reopened `r+b` at
  its position, so bytes written before the snapshot stay as they are on disk at load.
- **The GS host presentation latch**: the presenter's last latched frame is not saved, so
  a loaded run shows a blank frame until the next latch (one VBlank).
- **Debug-only state**: GS debug history, stub-trace and watchdog rings, log counters,
  debug UI.
- **Wall-clock alignment**: the guest clock (`m_eeCycle`, VBlank tick) is restored; the
  host epoch is re-anchored to "now" at load, so real-time catch-up starts fresh.
- **Pad input** comes from the host each frame; `PS2X_PAD_SCRIPT` ticks are absolute
  VBlank ticks, so a script given to the loading run still fires at the same ticks.
- **Determinism**: the runner is not deterministic (wall-clock pacing), so a loaded run
  matches a straight run picture for picture only within the regression threshold, not
  bit for bit. Save -> load -> save gives identical RDRAM and GS memory (unit test).

## Usage

Snapshots are opt-in: with none of these variables set nothing is recorded for them and
F5 does nothing.

- `PS2X_SNAPSHOT_SAVE=<path>`: where to write (`{tick}` is replaced by the VBlank tick;
  default `ps2x_snapshot_{tick}.bin` in the working directory).
- `PS2X_SNAPSHOT_AT=<tick>[,<tick>...]`: save at the first quiescent point at or after
  each tick. `PS2X_SNAPSHOT_EXIT=1`: stop the runner after the last listed save.
- `PS2X_SNAPSHOT=1`: opt in without a timed save (F5 only).
- F5 in the game window: save now.
- `PS2X_SNAPSHOT_LOAD=<path>`: resume from a snapshot (same game; any runner build whose
  sections and raw layouts match, see "Durable snapshots").
- `PS2X_MPEG_REPLAY_MB=<n>`: the MPEG replay-log limit (default 64).
- Give a loading run only `PS2X_GS_DUMP` ticks after the load tick (the dump list is
  consumed by tick; an earlier tick gives one stray dump at the first presented frame).
- `PS2X_DETERMINISTIC=1` (`ps2_deterministic.h`): guest time from EE cycle charges only (no
  real-time catch-up, no waiting for host deadlines), synchronous GS, no audio device, and
  guest wall-clock reads (CD clock, file and memory-card times, time zone) from
  `PS2X_DETERMINISTIC_EPOCH` (default 2026-01-01 UTC) plus VBlanks / 60. Two runs from one
  snapshot then compute the same state at every VBlank.
- `PS2X_FRAME_HASH=<file>` (`ps2_frame_hash.h`): one line per VBlank, on the EE executor:
  tick, EE cycle, hashes of RDRAM, GS memory and the display registers;
  `PS2X_FRAME_HASH_UNTIL=<tick>|+<n>` then creates `PS2X_STOP_FILE`. `tools\checkpoint.ps1
  verify` compares two runs with it.
- `PS2X_MC_ROOT=<dir>`: the memory card folder (default `mc0` beside the ELF), e.g. a scratch
  copy per test run.

## Verified results (SRWZ, 2026-10-07)

Headless runs at fast-forward x4 (`tools\long-run.ps1 -Headless`), each loaded run scored
against the straight run that wrote its snapshot (`tools\refcompare.py --dirs`, mean abs
diff on a 64x48 grey thumbnail, PASS <= 10 within +-75 ticks):

- Title (load at tick 27400): 19/21 frames PASS, most at 0.0; the two over the threshold
  are the title's explosion animation one game frame apart (the title runs at ~3.5 game
  fps). First title frame 2 s after launch instead of ~120 s (x4) / ~480 s (real time);
  the load itself takes 0.2-1.1 s for a 45 MB file.
- Mid-movie (load at 9000, 113 MB): 23/23 PASS, 18 at 0.0; the decoder replay reports
  `2731 pictures, the original decoder produced 2731; last picture matches`; load 1.9 s.
- Late movie (load at 20000 with `PS2X_MPEG_REPLAY_MB=320`, 257 MB): 20/22 PASS, 18 at
  0.0; `8235 pictures ... last picture matches`; the two outliers are one movie picture
  apart in fast motion; load 5.7 s.
- For scale, two straight runs with the same dump ticks agree on only 17/44 frames (the
  runner's timing phase drifts from run to run); a loaded run keeps its parent's phase.
- Final build (review round, opt-in recording, checksum, build id): the title pair again.
  Without `PS2X_MPEG_REPLAY_MB` a save asked for at 27400 was deferred while the movie's
  last seconds still played and written at 27700 (47 MB, 78 ms); its load (245 ms)
  matched the straight run on 18/20 title frames (2 at the baseline edge, none failed).
  With `PS2X_MPEG_REPLAY_MB=320` the save happened at 27400 exactly (344 MB, 720 ms); the
  load replayed the whole movie (11,538 pictures, last picture matches, 10.3 s) and the
  movie's last frames matched at mad 0.0, the title's logo animation then differed in
  phase (13/21). A snapshot written by an earlier build is refused at load.
- Saves take 40-720 ms; `ps2x_tests` 455/455 (suite `PS2Snapshot`, 4 tests: stream
  round-trip, save -> load -> save identical, tagged invocation round-trip, untagged
  completion and queued callback defer, truncated and bad-magic files refused).

Building a worktree of the branch: `powershell -File tools\wt-build.ps1 -Worktree
local\wt-snap -Targets ps2x_tests` (no lock) and `-Targets ps2EntryRunner` (build lock).
