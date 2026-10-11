# lean_codegen: leaner generated code (default off)

`[general] lean_codegen = true` in the recompiler config makes ps2_recomp emit leaner C++ for
every guest function. Off (the default) the output is byte-for-byte what it was. The switch is
independent of `resume_into_owner`. Runtime side: `ps2xRuntime/include/ps2_runtime_lean_macros.h`,
included only by lean output (after `ps2_runtime_macros.h`, which it partly redefines; it is not
in the runner's precompiled header, so the redefinition takes effect per translation unit).

## What changes and why each change is safe

1. **`ctx->pc` stores only where the pc can be observed.** Before, every instruction stored its
   address (1.45 stores per emitted instruction in SRWZ, counting the branch bookkeeping). The
   runtime reads `ctx->pc` only in code the generated function calls into: `dispatchGuestBranch`
   (sets it itself from the target argument and relies on the callee's `switch (ctx->pc)`),
   stub/syscall handlers (log it; `handleSyscall`), `SignalException`/`handleTrap`/`handleBreak`
   (`raiseCop0Exception` copies it to EPC), `Load*/Store*` on special addresses (may raise an
   address error), `eeCheckpointDue()` (the scheduler resumes at `ctx->pc`), and the crash and
   missing-function reports. So the lean output keeps the store: at function entry; before every
   call, direct function call, dispatch, relocation stub call, syscall, `goto label` (the branch
   target, also needed for the delay-slot resume check `if (ctx->pc == delayPc)`), checkpoint and
   return; as the fallthrough of every branch; at the end of a function that falls off; and before
   any instruction whose translation can reach the runtime (`CodeGenerator::leanTranslationObservesPc`:
   the text contains `runtime->`, `ps2_syscalls::`, `ps2_stubs::`, `throw`, its own `ctx->pc`, or a
   lambda-style `READn(`/`WRITEn(` macro). Dropped: the store before a pure register instruction,
   the duplicate entry store, the branch instruction's own pc (nothing reads it: a trapping delay
   slot reports `branch_pc`, which the delay-slot protocol still sets).
   Memory accesses with a non-constant address publish the pc on their slow path only (item 2).
   **Remaining pc precision:** exact whenever the runtime runs (so every stub/syscall log, every
   exception EPC, every scheduler resume address and every dispatch trace are as before);
   between runtime interactions `ctx->pc` holds the most recent published point: normally the
   current function's last branch target/fallthrough, call return or runtime-reaching
   instruction; after a runtime call that rewrote pc itself (handleSyscall sets the syscall's
   continuation, SignalException/handleTrap/handleBreak the exception vector) it holds that
   value until the next store, exactly as before the trim. The crash handler's "guest live pc" therefore names the
   right function and basic block; the exact instruction only when the crash is inside a runtime
   call, which is where native crashes happen (pure register code cannot fault; fast RAM access
   is masked). The watchdog's debug pc is published by the scheduler and is unchanged.

2. **Memory accesses as inline functions carrying the pc** (`PS2X_READn(addr, pc)`,
   `PS2X_WRITEn(addr, val, pc)`, `_DS` forms for delay slots) instead of the `READn`/`WRITEn`
   lambdas. The fast path (`Ps2FastRead/Write`, a masked RAM access that cannot fault) stores
   nothing; the slow path (`PS2Runtime::isSpecialAddress`: MMIO, scratchpad, kseg2/3) stores
   `ctx->pc = pc`, and in a delay slot `in_delay_slot = true; branch_pc = <branch>` before and
   `in_delay_slot = false` after, exactly the state the old emitted code had around the whole
   delay slot. Same `Load*/Store*` calls, same value evaluation order (the value expression is a
   register read, evaluated once in both shapes). No `ps2TraceGuestWrite` call (an inline empty
   function marked for deletion). Constant-address writes (`genFastWrite`) become a plain
   `FAST_WRITEn(addr, val)`. LL/SC use the same helpers. The VU macro-mode helpers that still use
   `READ128`/`WRITE128` are left as they were (and count as runtime-reaching, so they keep their
   pc store).

3. **Delay-slot flags only around delay slots that can reach the runtime.** `in_delay_slot` and
   `branch_pc` are read only by `raiseCop0Exception` and `handleSyscall`, i.e. only when the delay
   slot's code calls into the runtime. A delay slot that cannot (register ops; lean memory helpers,
   which set the state themselves on the slow path) is emitted bare. One that can (an
   overflow-checking add, a trap, a syscall, VCALLMS, TLB ops, the GIF-kick override, a lambda
   macro) keeps the full protocol (`pc`, `in_delay_slot`, `branch_pc`, code, `in_delay_slot = false`).
   Under `resume_into_owner` the resume-at-delay-slot path translates the instruction as a plain
   instruction (no `_DS` helper), matching the owner branch's semantics.

4. **Scalar GPR accessors.** `GPR_U32/S32/U64/S64` become 32/64-bit loads of the register's low
   half, `SET_GPR_U32/ZE32/S32/U64/S64` a 64-bit store of the low half (sign- or zero-extended
   exactly as the `_mm_cvtsi64_si128` forms did). The upper 64 bits of the 128-bit register are
   untouched, as with `_mm_move_sd` before. Semantics identical; the SSE load/blend/store
   round trip per write and the `movd` extraction per read are gone. `GPR_VEC`/`SET_GPR_VEC`
   (MMI, LQ/SQ) are unchanged. Known micro-architectural corner: a 128-bit `GPR_VEC` read right
   after a 64-bit `SET_GPR_*` of the same register no longer gets store-to-load forwarding (a
   stall of some cycles); this pattern is rare in scalar MIPS code.

5. **$zero operands folded** (`GPR_U32(ctx, 0)` -> `0u`, `GPR_S32` -> `0`, `GPR_U64` -> `0ull`,
   `GPR_S64` -> `0ll`, `GPR_VEC` -> `_mm_setzero_si128()`), a textual pass over each translated
   instruction. The compiler folded these anyway; this saves front-end work and makes the output
   readable. Writes to $zero (`SET_GPR_*(ctx, 0, ...)`) are unchanged (still a no-op).

Not done: `__restrict` on `rdram`/`ctx` (idea 10, third bullet). The scheduler and the IOP can
modify guest RAM and the active context during `eeCheckpointDue()` and dispatches through their
own pointers, which breaks the `restrict` contract in principle; the gain is unmeasured and the
failure mode (a hoisted guest load in a polling loop) would be silent. Compile parallelism and
unity batch sizing (idea 9, third bullet) are build-system knobs, not codegen.

## Reviewing the output

`local/lean/lean_diff.py <off dir> <on dir>` (SRWZ-native, private tooling) normalises the lean
output back to the old shape and reports anything that still differs, plus the per-instruction pc
invariant (every runtime-reaching line has a pc store of its own address in its instruction
record). Expected result: 0 residual differences, 0 violations.

## Measurements (SRWZ, resume_into_owner on in both)

MSVC 14.44 /O2, Ryzen 7 7800X3D.

| | off | on |
|---|---|---|
| ctx->pc stores in the output | 1,011,484 | 283,503 |
| in_delay_slot / branch_pc stores | 267,705 | 243 |
| lambda READn/WRITEn | 202,391 | 0 (202,391 inline helpers) |
| generated .cpp (main program) | 177.1 MB | 148.6 MB |
| overlays (206 ov_*.cpp) | 31.7 MB | 27.3 MB |
| ps2EntryRunner.exe | 87.4 MB | 53.3 MB |
| full runner build (ps2EntryRunner target, 16 threads) | 478 s | 320 s |
| group 00100000: c1xx / c2 time, obj | 10.4 s / 42.5 s, 737 KB | 10.4 s / 13.4 s, 290 KB |
| group 00140000: c1xx / c2 time, obj | 2.2 s / 2.3 s, 1,523 KB | 2.6 s / 33.8 s, 452 KB |
| group 0018a000 (movie loop): c2, obj | 1.4 s, 249 KB | 0.3 s, 119 KB |
| machine instructions in guest functions, 3 groups | 45,404 / 20,635 / 156,786 | 30,846 / 12,060 / 30,941 |

Build times of individual units can go up: with fewer stores and no lambdas the optimiser
inlines and schedules more (group 00140000), but the whole build is a third shorter. Speed and
correctness runs: see the worker report (LEAN-NOTES.md has the procedure).
