# srwz-timer: where this stands (2026-10-07)

Done: PS2X_BATCH_TIMING (default off) implemented, unit-tested (ps2x_tests 457/457), measured
(docs/BATCH-TIMING.md has the design, the throughput table, the correctness evidence and the
1x movie-stall note). Rebased onto srwz 5736176 (snapshots merged); snapshot save flushes first and carries the batch state (0497bb4, test 480/480).
Build: local\wt-timer\out\build (configured; ps2EntryRunner built 13:40 with all commits).
Next step if continuing: more 1x batched runs through the movie (the one stall in three runs,
tm1-b1, is the open question), then the runtime-reviewer. Nothing touches local\recomp\output.
