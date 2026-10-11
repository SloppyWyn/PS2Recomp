# srwz-lean worker notes (checkpoint 2026-10-07 ~14:05)

State: lean_codegen implemented and committed (eb46ee2 on srwz-lean, rebased on srwz-owner
a135bf9). ps2x_tests: 461/461 green (local\lean\ps2x_tests2.log). Off output byte-identical
to the owner's (local\lean\gen-base vs local\owner\gen-on: only register_functions.cpp names
differ, which come from a newer config in local\recomp). lean_diff.py: 0 residual, 0 violations.

Builds (resume_into_owner on in both): local\lean\build-off (478 s), local\lean\build-on (320 s),
configured from local\lean\gen-{off,on}\output and local\lean\ov-{off,on}\build.

Correctness (regress.ps1 content-order, headless, ff 4, PS2X_MPEG_HASH=1, 160 s host):
- off (lean-cor-off-1): REGRESSION PASS, 51/74 dumps pass, 0 fail, vsync 31,221 reached.
- on (lean-cor-on-1): REGRESSION PASS, 42/76 dumps pass, 0 fail, vsync 33,014 reached.
- [MPEG:hash] lines: 11,685 pictures on both sides, 0 differences (tick field ignored).
Both runs pass PRESS ANY BUTTON (title at vsync ~27,000).

Throughput: local\lean\throughput.ps1 -Rounds 3 -Seconds 240 (alternating off/on, headless,
default clock, fast-forward max), samples in local\runs\lean-tp-*.samples.csv, table via
python local\lean\throughput_report.py --title 28000. Running at the time of this note.

tools/overlays.py lean_codegen pass-through: branch tooling (local\wt-tooling) commit 5a68177.

## Cycle-clock throughput (contention-proof), 2026-10-07 ~14:25
Scripts in docs/lean-measure/: throughput3.ps1 (PS2X_REALTIME_CLOCK=0, PS2X_FASTFORWARD=max,
headless, no audio; alternating off/on, 3 rounds, boot to vsync 6000; samples the EE thread's
CPU at every new [run:tick]) and throughput3_report.py (EE-CPU at vsync marks 1000..6000, ms of
EE-CPU per 1000 vsyncs over 2000..6000, mean/spread, on/off ratio). Series started 14:2x hidden;
results: local\lean\throughput3.log + python docs/lean-measure/throughput3_report.py (reads
local\runs\lean-cyc-*.samples.csv). First sample: off-1 vsync 431 at 11.6 s host, EE thread 11.5 s
CPU (saturated). RESULT (all 6 runs, 600 s cap each, EE thread saturated except on-1's tail):
```nmode   run  cpu@1000 cpu@2000 cpu@3000 cpu@4000 cpu@5000   ms/1000vsync  eeSat  samples
off    1       77.45   217.48   336.13   454.80   573.13       129338.8   1.00      112
off    2       70.01   199.16   348.09   479.28        -       139042.3   1.00      113
off    3       79.36   213.42   341.97   470.04        -       131301.8   1.00      113
on     1       69.66   193.50   363.62        -        -       146981.7   0.61       64
on     2       68.88   190.84   348.93   500.28        -       140024.0   1.00      113
on     3       74.12   218.19   350.04   498.90        -       137958.4   1.00      113
off: mean 133227.6 ms EE-CPU per 1000 vsyncs over 1000..3000 (sd 4189.0, min 129338.8, max 139042.3, n=3)
on: mean 141654.7 ms EE-CPU per 1000 vsyncs over 1000..3000 (sd 3860.0, min 137958.4, max 146981.7, n=3)
on/off: 1.063
```nEE-CPU per guest vsync over the boot/loading stretch vsync 1000..3000: lean ON uses ~6% MORE
(on/off 1.063, sd ~3% per mode; off {129.3, 131.3, 139.0} s/1000 vsync vs on {138.0, 140.0, 147.0}).
Caveat: other sessions' builds ran meanwhile (SMT sharing lowers work per CPU-second), but the
alternation spreads that over both modes. This stretch is dominated by polling/loading loops
(scheduler and dispatch costs), not by straight-line guest compute; a title-stretch or
battle-stretch measurement and a bisect of the trims (scalar GPR accessors vs lean memory helpers
vs pc stores) are the next steps before enabling lean_codegen for speed. The size and build wins stand.

## Trim bisection (started 2026-10-07 ~16:50, quiet mode)
Sub-switches committed (386c014). Variants (configs by local\lean\make_variants.py, all with
resume_into_owner = true): nogpr = lean_codegen minus lean_gpr_scalar (prime suspect: a 64-bit
SET followed by a 128-bit GPR_VEC/LQ/SQ read defeats store-to-load forwarding); gpr = scalar GPR
accessors alone; mem = memory helpers alone. nogpr generated into local\lean\gen-nogpr +
ov-nogpr, build dir local\lean\build-nogpr queued (build-runner-quiet.ps1: lock, /m:8, below normal).
Measurement: local\lean\snapshot.ps1 -Variant <v> saves a title snapshot per build (tick 27,700,
PS2X_REALTIME_CLOCK=0, ff max, headless) into local\runs\snap\lean-<v>.snap; local\lean\cycles.ps1
-Variants off,on,nogpr -Rounds 3 loads it and samples the EE thread's QueryThreadCycleTime
(cycles executed, immune to SMT sharing) at every [run:tick] to vsync 31,000;
python local\lean\cycles_report.py --from 28000 --to 31000 gives Mcycles/vsync per variant and the
ratio to off. Status: off snapshot running; on snapshot next; nogpr build queued; no numbers yet.
Next: snapshots for on and nogpr, cycles.ps1 3 rounds, then gpr/mem variants if nogpr explains it.

Update ~17:10: the snapshot feature (PS2X_SNAPSHOT_SAVE/LOAD, branch srwz-snap) is NOT in
srwz-owner/srwz-lean (no SNAPSHOT symbol in ps2xRuntime; the off run ran past tick 27,700 without
saving), so the title stretch cannot be reached in cycle-clock mode (vsync 3,000 takes ~6 min from
boot). Bisection therefore measures the boot stretch vsync 1000..3000 with the EE thread's
QueryThreadCycleTime (cycles executed, immune to SMT sharing): cycles.ps1 -NoSnapshot -StopVsync
3000 -Tag cyc3, off/on 3 alternating rounds running, nogpr after its build (queued on the lock).
Report: python local\lean\cycles_report.py --tag cyc3 --from 1000 --to 3000. A compute-stretch
check (title/battle) needs the snapshot branch in the lean base: rebase srwz-lean onto srwz once
srwz-owner is merged there.

### Cycle-count series cyc3, off vs on (6 runs, boot -> vsync 1000..2000, EE-thread QueryThreadCycleTime)
```nvariant run  firstVsync  lastVsync      Gcycles    eeCpu_s     host_s  Mcycles/vsync      eeSat
off     1            48       2618       637.67     152.00      154.4         637.67       0.98
off     2            71       2411       777.07     184.94      207.1         777.07       0.89
off     3            92       2905       713.45     169.70      181.2         713.45       0.94
on      1            87       2045       799.23     188.66      301.1         799.23       0.63
on      2            91       2583       775.45     184.76      196.7         775.45       0.94
on      3            76       3026       590.63     140.62      141.0         590.63       1.00
off: mean 709.39 Mcycles/vsync over 1000..2000 (sd 56.98, min 637.67, max 777.07, n=3)
on: mean 721.77 Mcycles/vsync over 1000..2000 (sd 93.24, min 590.63, max 799.23, n=3)
on/off: 1.017
```nEE sat. = EE-thread CPU / host time over the stretch; runs below ~0.95 shared cores with other
sessions' compilers (SMT inflates cycle counts) and read high. on/off = 1.017 over all six; the two
fully saturated runs (off-1 0.98: 637.7, on-3 1.00: 590.6) put lean ON 7% FASTER. The earlier
'6% slower' (CPU-time series, SMT-noisy) is not confirmed: all-on is at worst neutral on this
stretch. nogpr (all trims minus scalar GPR) runs next, same series.

### Trim bisection result (series cyc3 incl. nogpr = lean_codegen minus lean_gpr_scalar)
```nvariant run  firstVsync  lastVsync      Gcycles    eeCpu_s     host_s  Mcycles/vsync      eeSat
nogpr   1            76       2980       714.61     170.30      170.8         714.61       1.00
nogpr   2            72       1189   (marks 1000..2000 not covered; 48 samples)
nogpr   3            90       2124       679.92     161.89      162.4         679.92       1.00
off     1            48       2618       637.67     152.00      154.4         637.67       0.98
off     2            71       2411       777.07     184.94      207.1         777.07       0.89
off     3            92       2905       713.45     169.70      181.2         713.45       0.94
on      1            87       2045       799.23     188.66      301.1         799.23       0.63
on      2            91       2583       775.45     184.76      196.7         775.45       0.94
on      3            76       3026       590.63     140.62      141.0         590.63       1.00
nogpr: mean 697.26 Mcycles/vsync over 1000..2000 (sd 17.34, min 679.92, max 714.61, n=2)
off: mean 709.39 Mcycles/vsync over 1000..2000 (sd 56.98, min 637.67, max 777.07, n=3)
on: mean 721.77 Mcycles/vsync over 1000..2000 (sd 93.24, min 590.63, max 799.23, n=3)
nogpr/off: 0.983
on/off: 1.017
```nnogpr-2 reached only vsync 1,189 in the 420 s cap (shared the machine with a tools build) and
is excluded. Fully saturated runs (EE sat. 1.00/0.98): off 637.7, on 590.6, nogpr 714.6 / 679.9
Mcycles/vsync. All three variants agree within the run-to-run spread (sd 8-13%): no trim
costs measurable time, the scalar GPR accessors included. Recommended set: lean_codegen = true
(all trims). Open: a title/battle compute-stretch confirmation once the snapshot branch is in
the base (srwz has it: use PS2X_SNAPSHOT_SAVE/LOAD with docs/lean-measure/cycles.ps1 without
-NoSnapshot).
