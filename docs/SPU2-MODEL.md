# SPU2 model (ps2xIOP/src/emulator/core/iop_spu2.*)

## Goal
A timing-accurate model of the PS2 sound processor on the IOP side, written from the public
register descriptions of the hardware (no code from any emulator), so that the real Sony IRX
sound drivers (libsd, multi_streamer, modhsyn, ezbgm, sdrdrv, ...) see what they expect:
voices that play through sound RAM at the pitch rate, envelopes that end, end flags,
interrupts and transfers that complete in step with the 48 kHz sample clock. It is
game-agnostic and meant for every PS2 port on this runtime; nothing in it is tuned to one
game. The output stage (section "Mixing and output") sits on the same model: it mixes what
the timing side already computes into 48 kHz stereo and is only switched on by an output
sink (WAV dump or host playback); without one the timing side runs exactly as before.

## What is modelled
- Registers 0x1F900000-0x1F9007FF, 16-bit, both cores (`+0x400` for core 1), with the
  shared SPDIF block at 0x7C0 (IRQINFO at 0x7C2 names the core that raised the IRQ).
- 2 MB sound RAM. Addresses in registers are halfword addresses split hi (bits 16-19) /
  lo (bits 0-15); SSA/LSAX/NAX are block (8 halfword) aligned.
- 24 voices per core: ADPCM block decoding (shift, filter 0-4, 28 samples per 16-byte
  block), flags (bit 0 end, bit 1 repeat, bit 2 loop start -> LSAX unless LSAX was written
  since KON), pitch counter (0x1000 = 48 kHz, capped at 0x3FFF), NAX following the block
  being decoded, ADSR envelope (ENVX 0-0x7FFF, attack/decay/sustain/release with the
  linear/exponential rate rules) readable at the voice's ENVX register.
- KON (clears the voice's ENDX bit, restarts at SSA), KOFF (release), ENDX (set at an end
  block). A write to ENDX (any value; libsd writes 0) clears the half written: 0x340 voices
  0-15, 0x342 voices 16-23 (no public description says one half's write clears the other;
  libsd always writes both). An end block jumps to LSAX and sets ENDX; without the repeat bit
  it also forces the envelope into release at level 0, but the voice keeps playing from LSAX:
  NAX keeps moving and the silent voice can still hit IRQA. Likewise a voice whose release
  reaches 0 stays in release at 0 and keeps playing until the next KON (voices never stop).
- IRQA: a voice fetching a block that contains IRQA, or a transfer writing/reading it,
  with ATTR bit 6 set, sets IRQINFO and raises IOP interrupt 9; clearing ATTR bit 6 clears
  the flag (how libsd acknowledges it).
- Transfers to/from sound RAM at TSA: the FIFO port (0x1AC, flushed in transfer mode 1),
  IOP DMA channels 4/7 (MADR/BCR/CHCR at 0x1F8010C0 / 0x1F801500, CHCR bit 24 reads busy
  until completion), completing on the sample clock at `DmaBytesPerSample` per sample and
  raising the channel's interrupt (0x24 / 0x28). STATX bit 7 = a DMA mode is selected and
  the transfer unit is idle; bit 10 = a transfer (DMA or AutoDMA) is in progress. AutoDMA
  (ADMAS bit `1 << core` in the core's ADMAS register): the core plays its input area (L at
  0x2000 + core * 0x400, R 0x200 halfwords further, two halves of 256 samples) at one stereo
  sample per output sample while the bit is set, and the transfer is paced by that playback:
  the data is 1 KB chunks (256 L samples, then 256 R), one moved into each half as soon as the
  half is free (both at once at the start, then one per 256 samples played: 4 bytes/sample),
  and the transfer ends (channel interrupt) when its last chunk is in; a last chunk under
  1 KB puts its first half in L and its second in R. Writes there, and the core's reads as
  it plays, check IRQA. Setting the ADMAS bit restarts playback with both halves empty;
  clearing it stops a running AutoDMA transfer where it is, with no DMA-done interrupt (it did
  not complete). An underrun replays the old half, as the
  hardware does. (Before srwz-audio the transfer was paced as "2 KB at once, then 4 bytes
  per sample" per transfer, which ran a stream 512 samples fast at every restart.)
- Clock: 768 IOP cycles (36.864 MHz) per sample; the IOP itself runs at EE cycles / 8.
  The model catches up lazily (before every register access and in the IOP scheduler
  loop) and caps the scheduler's idle wake at 1 ms while voices run, so ENDX/ENVX/IRQ
  changes are seen within that.
- Snapshots: the whole model is part of the IOP memory state.

## Mixing and output (srwz-audio)
Per output sample (48 kHz), from the public register descriptions:
- Voice output (OUTX): the voice's sample at the pitch counter's position, by linear
  interpolation between the last two decoded samples (one sample of delay; the hardware uses
  a 4-point gaussian filter, not modelled), or the core's noise generator for a voice whose
  NON bit is set, times ENVX (`sample * env >> 15`). Computed for every running voice whether
  or not anything is mixed (pitch modulation needs it).
- Noise (NON 0x184/0x186): one generator per core, clocked by ATTR bits 8-13 (shift bits
  10-13, step 4 + bits 8-9): a timer decremented by the step each sample; when it goes
  negative the 16-bit level shifts left taking in the inverted parity of bits 15/12/11/10 and
  the timer is reloaded with `0x20000 >> shift`.
- Pitch modulation (PMON 0x180/0x182, voices 1-23): pitch = `(int16)PITCH * (OUTX[v-1] +
  0x8000) >> 15`, low 16 bits, capped at 0x3FFF like plain pitch. Affects NAX timing.
- Volumes VOLL/VOLR (voice +0/+2) and MVOLL/MVOLR (0x760/0x762, core 1 0x788/0x78A): bit 15
  clear = fixed signed 15-bit volume, used doubled (0x3FFF -> 0x7FFE, 0x4000 -> -0x8000); bit
  15 set = sweep (bit 14 exponential, bit 13 decrease, bit 12 negative phase, bits 0-6 rate
  with the ADSR rate rules), starting from the current level, 0..0x7FFF. The current values
  read back at VOLXL/VOLXR (voice +0xC/+0xE) and MVOLXL/MVOLXR (0x770/0x772, 0x798/0x79A);
  sweeps step per sample (voice sweeps while the voice runs). AVOL (0x768/0x76A, external
  input) and BVOL (0x76C/0x76E, memory input), core 1 at +0x28, are plain signed 16-bit
  volumes. Which is which was settled by what libsd writes: core 1 AVOL = 0x7FFF, BVOL = 0, core
  0 both 0 (SRWZ run aud2, `PS2X_SPU2_MIX_LOG`); with the two swapped core 0 was never heard.
- Per core: dry L/R = sum over voices with VMIXL/VMIXR bits (0x188/0x18A, 0x190/0x192) of
  `OUTX * VOLx >> 15`. MMIX (0x198) picks what reaches the core output: bits 11/10 voices
  L/R, bits 7/6 memory input L/R (the AutoDMA sample just played, times BVOL; only while the
  core's ADMAS bit is set), bits 3/2 external input L/R (times AVOL; for core 1 that is core
  0's output; core 0 has none). The odd "to effects" bits (9/8, 5/4, 1/0) feed the same
  sources to the core's reverb unit (voices through VMIXEL/VMIXER 0x18C/0x194), whose output
  comes back times EVOL (0x764/0x766, plain signed 16-bit; section "Reverb"). The sum is clamped to 16 bits, then times MVOL (clamped): the core's
  output. Core 0's output is core 1's external input; core 1's output is the PS2's audio out
  (what the sink receives). ATTR's enable/mute bits are not applied.
- Cost: with no sink, only OUTX/sweep/noise bookkeeping (a few operations per running voice).
  With a sink every sample is stepped (no idle skipping) and mixed: 48 voices, ~1-2 us per
  sample, on the emulation thread; frames go to the sink in batches of 512 (10.7 ms).
- Host side (ps2xRuntime, `runtime/ps2_spu2_output.h`), installed through
  `IopSubsystem::setAudioOutput`:
  - `PS2X_SPU2_WAV=<path>`: 48 kHz 16-bit stereo WAV of the output (header rewritten once per
    second of audio, so a killed run still leaves a valid file; no 32-bit file positions, so
    files past 2 GiB work); at the end stdout gets
    `[audio:spu2] wav closed <path> frames= seconds= non_silent_frames= peak= rms=
    clipped_samples=`.
  - `PS2X_AUDIO_SPU2=1`: plays it on the host device through a raylib audio stream (48 kHz;
    miniaudio resamples to the device). Off by default; it needs the device open, so with
    `PS2X_NO_AUDIO`, `PS2X_MUTE` or headless (what every run script sets) it logs
    `[audio:spu2] PS2X_AUDIO_SPU2 ignored: no audio device open` and plays nothing. The
    emulation thread pushes into a lock-free ring (8192 frames, ~170 ms; one atomic word per
    frame) and never waits: on overflow the oldest frames are dropped (the producer moves the
    read index past them), an empty ring plays silence. The device callback keeps the fill near
    half (~85 ms) by consuming ~1% more frames than it plays above 3/4 and ~1% fewer below 1/4
    (nearest-neighbour skip/repeat): that follows the drift between the emulation's 48 kHz and
    the device clock. Dropped/underrun/adjusted counts are logged when the stream closes. The
    stream's sub-buffer is 1024 frames, set for its own load only (raylib 5.5 has no per-stream
    size call: the default is set, the stream loaded, the default put back to 0).
  - With neither set no sink is installed and the mixer stays off.

## Reverb (srwz-sdremote; iop_spu2_reverb.*)
From the public PS1 SPU / PS2 SPU2 reverb descriptions (register list and the reverb formula),
no emulator code. One unit per core, run only while mixing (with an output sink):
- Work area: ESA (0x2E0/0x2E2, halfword address) to the area end (EEA 0x33C = bits 16-19 of
  the last halfword: end = EEA << 16 | 0xFFFF; libsd computes the same in bytes). The buffer
  pointer moves one halfword per reverb step and wraps from the end to ESA; every address
  register (0x2E4-0x33A, hi/lo pairs: FB_SRC_A/B = dAPF1/2, IIR_DEST_A0/A1 = mL/RSAME,
  ACC_SRC_A0..D1 = mL/RCOMB1-4, IIR_SRC_A0/A1 = dL/RSAME, IIR_DEST_B0/B1 = mL/RDIFF,
  IIR_SRC_B1/B0 = dL/RDIFF, MIX_DEST_A0..B1 = mL/RAPF1-2) is a halfword offset from the
  pointer, wrapped inside the area. Coefficients at 0x774-0x786 (core 1 +0x28): IIR_ALPHA,
  ACC_COEF_A-D, IIR_COEF (wall), FB_ALPHA, FB_X, IN_COEF_L/R.
- One step (24 kHz, both sides): input x IN_COEF; same-side IIR
  `[mSAME] = (in + [dSAME]*WALL - [mSAME-1]) * IIR_ALPHA + [mSAME-1]`, the cross-side one with
  the other side's d tap; four comb taps x ACC_COEF; two all-pass stages
  `out -= [mAPF - dAPF]*k; [mAPF] = out; out = out*k + [mAPF - dAPF]` (k = FB_ALPHA, FB_X);
  products are `(a*b) >> 15`, stores saturate to 16 bits.
- Rate: the effects input is low-pass filtered and decimated by 2, the output zero-stuffed and
  filtered back to 48 kHz. The filter is our own 39-tap half-band windowed sinc (Blackman), not
  the hardware's coefficients.
- ATTR bit 7 clear: the unit stops writing to the buffer but still reads it, outputs and moves
  the pointer (the public SPU description: the flag stops buffer writes, not the output).
- `PS2X_SPU2_REVERB=0` leaves the unit out of the mix entirely (A/B level checks).
- The decoded registers are cached per core and rebuilt after a write to ATTR, 0x2E0-0x33F or
  the coefficient registers.
- Not modelled: IRQA hits by reverb accesses; the hardware's exact resampling filter.
- SRWZ (libsd SetEffectAttr from sdrdrv, mode 4 = Studio C / large): core 0 ESA 0x8C810 EEA 8, core 1
  ESA 0xFC810 EEA 0xF (0x37F0 halfwords each), IN_COEF 0x8000, EVOL 0x3000 then 0x1E00/0x2300,
  ATTR 0xC080/0xC081. Tests: ps2_iop_spu2_tests (filter, formula against hand values, pointer
  wrap, area bounds, disabled, echo delay/level through the resampling, wet-only voice through
  the mixer with EVOL).

## What is not modelled (yet)
- The gaussian interpolation filter (linear instead), the voice/core output write-back into
  sound RAM (0x400-0x1000 areas), SPDIF output, ATTR mute/enable bits.
- Exact DMA bus rates: `DmaBytesPerSample` (256 bytes per sample, ~12 MB/s) is a plausible
  constant, not a measurement.
- AutoDMA: the initial fill is instant (no bus time); the details of the hardware's request
  handshake beyond "a chunk per free half".

## Debugging
- `PS2X_IOP_SPU2_LOG=1` (or `=N` lines; 1 = 4000): key on/off (with pitch, ADSR1 and the flag
  nibbles of the first eight blocks at SSA), transfers (IOP source, first bytes, first sound-RAM
  halfwords after the copy), end flags and IRQs on stderr (`[iop:spu2]`).
- `PS2X_IOP_SPU_LOG=1`: every SPU2 register halfword the IOP reads, at power-of-two counts.
- `PS2X_SPU2_SETUP_LOG=N` (1 = 4000 lines): every write to the mixer/effects setup registers
  (per core 0x180-0x19F, ADMAS, 0x2E0-0x33F; the volume block 0x760-0x7AF) with its value
  (`[iop:spu2:setup]`).
- `PS2X_IOP_TRACE_IMPORTS=lib,lib|*` (+ `PS2X_IOP_TRACE_MAX`): IOP import calls (`[iop:import]`).
- `PS2X_IOP_WATCH=addr,addr`: hex IOP RAM words once per second (`[iop:watch]`).
- `PS2X_IOP_WATCH_THREADS=1` (with `PS2X_IOP_WATCH`): on the same cadence, every IOP thread
  (`id:state:prio:pc`, `:w<id>` for a blocked wait, `:k<n>` pending wakeups; states D R X S L M
  E U Z), the allocated hard timers (`t<i>:run cmp= in=<cycles> fn=`), pending DMA interrupts /
  callbacks (`[iop:watch:thr]`), then semaphores and event flags (`[iop:watch:sync]`).
- With `PS2X_IOP_RPC_LOG`, each `[iop:rpc]` line ends with `ready=[...]` (IOP threads runnable,
  i.e. preempted mid-work, when the server function ran) and `intr-off`; `[iop:rpc:before]`
  lines show the threads run before a server (first 64). Always on: `[iop:call]` when a guest
  function the emulator calls (handler, callback, server) does not return (budget ran out or it
  blocked), `[iop:timer]` when a timer handler returns 0 and so stops its timer.
- `PS2X_IOP_RPC_LOG=1` (+ `_MAX`): EE -> IOP RPC calls served by IRX servers, send and reply
  words (`[iop:rpc]`); `PS2X_SIF_RPC_LOG=1`: how each sceSifCallRpc completion was handled.
- Tests: `ps2_iop_spu2_tests` (ps2xTest/iop_spu2: decode, loop/end flags, NAX advance, ENVX,
  IRQA, DMA through the IOP memory map, snapshot round trip, IOP heap reuse; mixer: a known
  ADPCM block at pitch 0x1000 through envelope/VOLL/VOLR/MVOL sample-exact, VMIX/MMIX gating,
  core 0 -> core 1 through AVOL, VOLX/MVOLX sweeps, AutoDMA 4 KB stream through BVOL with its
  interrupt at sample 513, ADMAS cleared mid-transfer without an interrupt, a short last chunk,
  IRQA hit by input playback, noise, pitch modulation, batching); `ps2_audio_device_tests` (WAV
  writer, the ring's drop-oldest and rate matching, PS2X_AUDIO_SPU2 refused without a device);
  the NOWAIT RPC test in
  ps2xTest/src/ps2_sif_rpc_tests.cpp.
- `PS2X_SPU2_MIX_LOG=<ms>` (with a sink set): every <ms> of output, per core: ATTR, MMIX,
  MVOL/MVOLX, EVOL, AVOL, BVOL, voices running / with ENVX > 0 / with dry VMIX bits / wet only
  / at zero volume, and the core's last output sample (`[iop:spu2:mix]` on stderr).
- `PS2X_SPU2_WAV=<path>` (above) with `PS2X_IOP_SPU2_LOG=1`: while a sink is set, the `kon`
  lines carry `out=<frame>`, the KON's frame index in the WAV.

## What the drivers needed (SRWZ, SDK 3.1 multi_streamer + libsd), for the record
Found with the IRX decompiles (tools/ghidra/IrxDecompile.java) and the logs above:
1. multi_streamer never touches SPU2 registers itself; everything goes through libsd. It
   polls NAX (GetAddr 0x2240) to refill a stream's half buffers and ENVX (GetParam 0x500) to
   see a voice end; VoiceTrans/VoiceTransStatus block on libsd's DMA interrupt, which spins on
   STATX bit 7 and on ATTR's transfer-mode bits clearing. ENDX alone releases nothing.
2. Three things outside the SPU2 kept the voice line from ever starting (fixed on the same
   branch): the IOP heap was a bump allocator (per-request staging buffers leaked it away,
   AllocSysMemory then returned 0 and ezmidi retried sceCdRead into address 0 forever, starving
   every IOP thread); NOWAIT sceSifCallRpc completed inside the call (the game's sound driver
   detects a new reply by comparing the reply counter against the value saved after the call,
   so its "start stream" echo never fired: completions now run ~200 us of guest time later, as
   the IOP's RPC END interrupt would. The 200 us is an estimate of the IOP turnaround, not a
   measurement. The completion does what the END packet does: end function, completion
   semaphore signals, client idle (only if no newer call has started on that client); the reply
   words are already in the receive buffer at call time, written by the IOP bridge); and the IOP's sceCdSearchFile answered from a virtual
   ISO while sceCdRead read the raw image (both now go through the host's CD file registry:
   one LSN space with the EE).

## Status (2026-10-07, evening)
Works: the SRWZ attract demo plays from the title through every scene a 1x run of 320 s reaches
(Gundam SEED Destiny, Mazinger Z, Orguss, Baldios, Z Gundam, Daitarn 3, Aquarion), in step
with PCSX2 (scene changes within one 300-tick dump of the reference). With the CD registry fix
the stream data is real ADPCM (plain block flags, multi_streamer's 6 in block 0): the 27 voice
lines logged last 0.6-5.6 s each (KON to the stream's end block) instead of ending 100-200
samples after KON, and the old stall on the Hyakunin-shu line is gone. Snapshots save at the
first attempt again (the attempt now runs before the scheduler turns due events into queued
interrupt handlers).

## Status (2026-10-07, night: srwz-audio)
Real output: a 1x headless run of the attract demo from the title snapshot with
`PS2X_SPU2_WAV` (run aud3, 320 s, same scenes and end tick as before) gives 319 s of 48 kHz
stereo: RMS -18.8 dBFS, peak 32766, 0 clipped samples, 0.3% of 100 ms windows below -70 dBFS
(the title before the demo), 2781 of 2785 KON clusters audible within 500 ms, all 27 voice-line
KONs (core 1 voice 22) followed by -11..-20 dBFS. SRWZ's libsd setup: MMIX core 0 0xFF0, core 1
0xFFC, MVOL 0x3FFF both, core 1 AVOL 0x7FFF, and as the model sees it EVOL 0 (see below).
Against a PCSX2 capture of a similar stretch (its own video capture, Null audio backend):
PCSX2 -16.5 dBFS RMS, 100 ms median -17.9 / p90 -12.8 dBFS; ours -19.6 / -24.8 / -13.8 for the
matching part (after the review fixes, run aud4: -18.9 / -24.2 / -13.4): loud parts agree
within ~1 dB, quieter passages are ~6 dB lower. No measurable CPU cost in the run (eeCpuMs
268 s vs 294 s for the run without the mixer).

Why the quiet passages are low (review round: `PS2X_SPU2_SETUP_LOG` and
`PS2X_TRACE_STUBS=sceSdRemote` from boot, run audsnapD): the game sets up its effects from the
EE through libsdr (`sceSdRemote`), and that call is bound to the runtime's HLE stub
(Kernel/Stubs/Audio.cpp), which keeps transfer bookkeeping only and drops everything else.
Dropped at boot: SetEffectAttr (0x8130, both cores), SetCoreAttr effects enable (0x8070, cores
0 and 1 = 1), SetSwitch (0x8050: 0x1D00/0x1D01 = 0x11FFFF/0x1FFFFF), SetParam EVOLL/EVOLR
(0x0B80/0x0C80 + core) = 0x3000, later 0x1E00, MVOL 0x3FFF (0x0980/0x0A80), and 0x0F80/0x1080
= 0x7FFF around the movie's BlockTrans. The IOP side only writes EVOL = 0 and ATTR without bit 7
(libsd init), so the model sees reverb off, while the IOP sequencer routes voices to the
effects (VMIXEL/VMIXER). The game has reverb on; the missing reverb return is the likely gap.
Fix in two steps: (1) stop binding `sceSdRemote` / `sceSdRemoteInit` / `sceSdTransToIOP` to the
HLE stub for games that load sdrdrv on the IOP (SRWZ: `local/recomp/srwz.toml` stubs list), so
the game's own libsdr RPCs reach sdrdrv and libsd (a recompile of the game; it also moves the
movie's BlockTrans PCM onto this model); (2) the reverb unit.
Open: those two, gaussian interpolation.

## Status (2026-10-08: srwz-sdremote)
SRWZ's libsdr now runs as guest code (config/guest.txt in the port: sceSdRemoteInit /
sceSdTransToIOP / sceSdRemote kept), so the effects setup reaches sdrdrv and libsd on the
emulated IOP. One IOP fix was needed: sdrdrv's SetEffectAttr clears the work area through libsd
VoiceTrans in 1 KB chunks, each waiting (WaitEventFlag) for the DMA interrupt; the RPC bridge
calls the server function outside any IOP thread, where the wait failed at once (-418) and
SetEffectAttr returned -210 after one chunk. Blocking waits outside a thread now let IOP time
pass to the next hardware event, servicing interrupts, timers and callbacks
(core/iop_call_wait.h). They give up at once, without moving time, when nothing that could
satisfy them is scheduled (no pending DMA interrupt, callback, timer or SPU2 transfer) or when
reached while interrupts/callbacks are being serviced. Time spent on a transfer in flight is
bounded by the transfer (hard limit 1 s): SRWZ's sound driver uploads 267 KB to core 1 with one
VoiceTrans (~22 ms at the model's DMA rate) and waits for it; time spent on other events
(timers, callbacks) that keep coming without satisfying the wait is capped at 20 ms. Other IOP
threads do not run during such a wait. `[iop:callwait]` logs every give-up and the first 32
satisfied waits. Longer term: run RPC server functions on the IOP thread that registered them,
so blocking waits go through the scheduler like any other thread's. The movie's BlockTrans PCM now plays through AutoDMA on core 0: the
whole movie streamed 75.6 MB at 4.0001 bytes per sample (= 192,000 B/s). With the reverb, a 1x
run of the attract demo: median 100 ms level -17.9 dBFS (before -24.0; PCSX2 -18.3), RMS -16.6
(PCSX2 -16.8), 0 clipped; the quietest tenth is now ~3-8 dB above PCSX2's (p10 -23 vs -31).
Review round (rebased on 617fb3d): A/B with `PS2X_SPU2_REVERB=0` (1x demo from the same
snapshot, 100 ms windows, 10-300 s): reverb off median -17.7 dBFS, on -17.5 (two runs), PCSX2
-18.3, before this branch -23.8. So the median gap closed mostly because the effects setup now
reaches the IOP (the old stub also returned 0 to every query the game made through
sceSdRemote), the reverb itself adds ~0.2 dB at the median and fills the quiet stretches (p10
-28.7 on vs PCSX2 -31.1). SRWZ's MMIX: core 0 0xFF0, core 1 0xFFC: the external-input effects
bits (0/1) are clear on both, so core 0's reverbed output is not sent through core 1's reverb.
The intermittent wait at a scene change (the game polling its sound driver, sid 0x12346 fn
0x8090, which kept replying 0x100) is fixed: see the next section.

## RPC servers and IOP thread priority (srwz-sndhang, 2026-10-08)
The RPC bridge calls a server function directly, at whatever point the IOP's last time slice
ended. Our slices end on a cycle budget (256 cycles), the hardware's only when a thread blocks or
a higher-priority thread is readied, so a server could run while a thread of higher priority
than its own thread was preempted in the middle of its work. SRWZ: ezmidi (sid 0x12346) has a
tick thread (priority 0x1d, woken at 240 Hz by a hard-timer handler) that copies the song's
"playing" bit into the word RPC 0x8090 returns, then runs sceMidi_ATick and sceHSyn_ATick; its
RPC server thread has priority 0x1e. The EE stops the song (RPC 0x20: SongPlaySwitch off, then
modhsyn frees the voices) and polls 0x8090 until 0. In the hangs the stop ran while the tick
thread was preempted inside modhsyn's ATick voice-list walk (`[iop:rpc] sid=0x12346 fn=32 ...
ready=[9:R:29:5EDCC]`, modhsyn+0x54cc); the walk then followed a freed node and never got back to
the list head, so the tick never finished (thread Ready forever, its wakeup count growing by 240
a second), the status word kept 0x100 and the EE polled forever (the song itself was stopped).
Fix: before a server function runs, every ready thread with priority <= that of the thread that
registered the server (recorded at sceSifRegisterRpc) runs until none is ready (due interrupts
and timers serviced between slices, idle time not skipped, at most 20 ms of IOP time); that IOP
time is repaid from the next EE slices so the IOP clock stays in step. `PS2X_IOP_RPC_PRIORITY=0`
turns it off. Measured: without it 30% of the RPCs of the fix-off runs found a priority-29 thread
preempted mid-work (mostly multi_streamer's per-frame status call; the 3 hangs were the ezmidi
stops among ~55 that landed there); with it 0 of ~126,000 RPCs to lower-priority servers did
(the only ready threads left were seen by sdrdrv's server, whose thread has priority 24, which
the hardware also lets preempt them). 0 hangs in 44 stops over 52 runs. Test:
ps2_iop_sched_tests.
Not done: interrupts and timer handlers still run between slices even while a thread has
interrupts disabled (CpuSuspendIntr); equal-priority threads alternate by id at slice ends
instead of running until they block.
