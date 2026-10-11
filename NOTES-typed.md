# Typed stub ABI layer (RECOMPILER-IDEAS item 11) — handover, 2026-10-07 (updated after the srwz-typed2 batches)

Branch `srwz-typed2` (worktree `local\wt-typed2`, from srwz d1ab9b8, which merged `srwz-typed`;
the Commands below name `local\wt-typed`: use `local\wt-typed2`). Tooling changes go on
`local\wt-tooling` (branch `tooling`; `git merge main` first; commit only your files). Wyn
approved the layer **behind a switch / opt-in, with evidence of equal semantics**. Never edit the
main working tree; never push or merge. Commit messages end with
`Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` (or the current worker's line;
second and third workers: `Claude Opus 5.5 (1M context)`).

Quiet mode is over (Wyn said so 2026-10-07): normal priority and all cores are fine, still
through `tools\launch.ps1` (no windows, no focus steal), game runs headless (`PS2X_HEADLESS=1`,
`PS2X_NO_AUDIO=1`). ps2x_tests builds need no build lock; runner builds queue on it.

## srwz-typed2 (third worker, 2026-10-07): library batches after the review

srwz-typed was reviewed and merged into srwz at d1ab9b8 ("commit as is"); srwz-typed2 (worktree
`local\wt-typed2`) starts there. One commit per library batch, in the order the in-game path after
START meets them: SIF/RPC, CDVD, sound, pad, memory card.

Reviewer notes folded in (d1ab9b8 review):
1. Deliberate deviations from the SDK prototype are marked: `PS2_STUB_DEVIATION(name, "why")`
   (ps2_stub_abi.h) -> `"deviation"` in the ps2x_stubsigs JSON (an alias inherits its target's);
   stubaudit reports DEVIATION (info) instead of SIGDIFF and DUMPDIFF when only one side marks
   it; bindcheck downgrades SIGTABLE to INFO SIGTABLE_DEV and prints the text. Marked so far:
   the GS u32-for-short trio, void scePrintf, variadic sceSdRemote, and every handler whose SDK
   function is void but whose handler writes v0 (kept: changing it is a behaviour change).
2. No float-taking handler is converted yet. When libvu0 comes: verify each declaration against
   the game's own SDK code (e.g. sceVu0ViewScreenMatrix), not against a legacy twin; mixed
   int/float stack overflow is untested: add a decoder test when a handler needs it.
3. **Gate before any default flip** (`PS2X_TYPED_STUBS_DEFAULT=ON`): a `PS2X_TRACE_STUBS` capture
   of real arguments from a game run, checked against the SDK prototypes of the converted
   handlers (and ideally the same for a second title). Not done yet.
4. The stack-word fallback through the runtime memory map has a test now ("decoder: stack words
   through the runtime memory map..."): with rdram given, getConstMemPtr resolves every address
   (scratchpad, KSEG/uncached mirrors, anything else masked into RAM), so typed and legacy
   readers agree on any sp and the fallback is reached only with no rdram; the test drives it
   with sp in RAM, in VU1 data memory (read through the map, not the RAM alias) and unmapped (0).

Harness changes (commit 82a467c, before the batches): the differential now starts every call
from the same saved state of the stateful libraries (snapshot hooks of SIF, RPC, CD, pad, memory
card, libsdr and the syscall tables), after a fixture init (SifInitRpc, sceSifInitCmd,
sceCdInit, scePadInit + scePadPortOpen(0,0), sceSdRemoteInit, sceMcInit + sceMcOpen of a card
file as fd 1), and compares the libraries' saved state after the call (libHash); sceMc* calls
also start from the same memory-card files (`mcfix0/1` copied to `mc0/1` under the scratch
folder) and the card tree after the call is compared (fsHash). The CD root holds files named like
the random format strings ("%d %d %d %d", "%s %d") so sceCdSearchFile/sceMcOpen can succeed.
Self-test pairs differing only in library state / a card file are detected. A timeout prints
the state. `scePadGetFrameCount` (free-running counter) is exempt from "compared > 0".
So the stateful batches are tested with fixtures, not only the shared-body pattern.

### Batch 1: SIF / RPC (Stubs/SIF.cpp, Syscalls/RPC.cpp, Syscalls/System.cpp)

46 typed+legacy pairs + 14 aliases. Shared body (decoding-only difference): sceSifGetOtherData,
sceSifGetReg, sceSifSetReg, sceSifSetDma, SifStopModule, SifLoadModule, SifBindRpc,
SifSetRpcQueue, SifRemoveRpcQueue, SifRemoveRpc, sceSifLoadModuleBuffer. Verbatim legacy: the
rest. Aliases (were one-line forwarders): stub sceSifLoadModule/BindRpc/CheckStatRpc/InitRpc/
LoadElf/LoadElfPart/LoadModuleBuffer/RemoveRpc/RemoveRpcQueue/SetRpcQueue -> the syscalls,
isceSifSetDChain/isceSifSetDma -> the stubs, syscall sceSifLoadElfPart -> SifLoadElfPart,
syscall sceSifLoadModule -> SifLoadModule. Not converted: sceSifCmdIntrHdlr, sceRpcFreePacket,
sceRpcGetFPacket(2) (TODO_NAMED stubs).
Declarations follow the SDK prototypes (unused trailing arguments declared too, e.g.
sceSifGetOtherData's mode, SifStopModule's args/argp); void SDK functions whose handler writes v0
are declared int32 + PS2_STUB_DEVIATION (sceSifAddCmdHandler, ExecRequest, ExitCmd, ExitRpc,
InitCmd, LoadFileReset, RemoveCmdHandler, RpcLoop, SetDChain, StopDma, WriteBackDCache,
SifInitRpc, SifSetRpcQueue). Unsure prototypes (no SDK header here; declared from the handler's
reads): sceSifGetIopAddr(u32), sceSifSetIopAddr(u32, u32), sceSifResetIop(void), sceRpcGetPacket.
Fixed on the way (both forms, shared body): sceSifSetDma's debug log looped over `count`
descriptors before the count check, so a bogus count (2 M) logged millions of lines (a 20 s hang
in the differential); the log now stops at the 32 the call accepts. RpcClientState got named,
zeroed padding (its snapshot bytes held stack garbage, so equal states saved differently).
Evidence (local\wt-typed2-sif-*.txt): Passed 496 Failed 0 with PS2X_TYPED_STUBS unset / 1 / 0;
poison flagged 0 (666 handlers) each way; differential at 256: 68 pairs, 17408 states, compared
17362, nondeterministic 46 (all sceSifAllocIopHeap: the IOP heap is not part of the fixture),
differ 0; ps2x_stubsigs 85 typed, 0 unresolved; stubaudit: LEGACYDIFF 0, IMPLREG 0, SIGDIFF 0,
22 declared deviations, dump 85/85 matched.

### Batch 2: CDVD (Stubs/CD.cpp)

38 typed+legacy pairs + 1 alias (sceCdReadIOPm -> sceCdRead). Shared body: sceCdRead (with its
alternate-layout recovery), sceCdReadChain, sceCdReadClock, sceCdSearchFile, sceCdStInit,
sceCdStStart, sceCdStRead (SelfReturn: it writes v0 itself, possibly after a VSync wait).
Verbatim legacy: the rest. Not converted: sceCdRI, sceCdRM (TODO_NAMED).
Deviation: sceCdIntToPos (SDK returns the sceCdlLOCCD pointer; the handler returns 1/0, kept).
Unsure prototypes (declared from libcdvd as remembered / ps2sdk; the handlers ignore them):
sceCdApplyNCmd(u8, ptr, u16), sceCdDelayThread(int), sceCdInitEeCB(int, ptr, int),
sceCdStream(u32 lbn, u32 n, ptr buf, int cmd, ptr mode).
Fixture: the CD root (scratch folder) holds "%d %d %d %d" (8 KB) and "%s %d", names the random
string arguments point at, so sceCdSearchFile can register a file and sceCdRead/ReadChain read
its pseudo-LSN sectors.
Evidence (local\wt-typed2-cd-*.txt): Passed 496 Failed 0 unset / 1 / 0; poison flagged 0 both
ways; differential at 256: 106 pairs (all batches so far), 27136 states, compared 27090,
nondeterministic 46 (sceSifAllocIopHeap only; sceCdReadClock can flip on a second boundary),
differ 0; stubsigs 124 typed; stubaudit LEGACYDIFF 0 / IMPLREG 0 / SIGDIFF 0, 23 deviations,
dump 124/124.
Found, not fixed (pre-existing, same in both forms): sceCdSearchFile with an empty name at an
address getConstMemPtr cannot resolve dereferences null in its log preview
(`*getConstMemPtr(rdram, pathAddr + i)`); unreachable with a normal rdram (every address
resolves), so left alone.

### Batch 3: sound (Stubs/Audio.cpp)

2 typed+legacy pairs: sceSdRemoteInit (void args, int return; libsdr), sceSSyn_SetOutputMode(int).
(sceSdRemote was in the pilot.) Everything else in the sound libraries is TODO_NAMED and stays
unconverted: sceSdCallBack, sceSdTransToIOP, the other 13 sceSSyn_*, all sceSynthesizer*.
Unsure: whether libsdr's sceSdRemoteInit returns int (the handler writes 0; no deviation noted).
Harness fix in this commit: when the typed run differs, the legacy form runs a third time; if
that no longer matches the first two the state changed under the handler (sceCdReadClock: the
host clock reached a new second between the second legacy run and the typed run, which showed
up once as a false "differ" at 256 states) and the state counts as nondeterministic.
Evidence (local\wt-typed2-sound-*.txt): Passed 496 Failed 0 unset / 1 / 0; poison flagged 0 both
ways; differential at 256: 108 pairs, 27648 states, compared 27601, nondeterministic 47
(sceSifAllocIopHeap 46, sceCdReadClock 1), differ 0; stubsigs 126; stubaudit LEGACYDIFF 0,
dump 126/126.

### Batch 4: pad (Stubs/Pad.cpp)

28 typed+legacy pairs + 1 alias (scePadInit2 -> scePadInit): every scePad* handler. Shared body:
scePadInfoMode, scePadRead, scePadStateIntToStr; verbatim legacy: the rest. port/slot are
declared int (the old `static_cast<int>(getRegU32())` is the same value).
Deviation: scePadGetDmaStr declares a third argument (a2): the handler returns it when port/slot
are out of range (SDK: two arguments) - kept, and declaring it keeps LEGACYDIFF at 0.
Fixture: pad port 0 is open (scePadPortOpen(0, 0, 0x240000)), so scePadRead/GetState/Info*/Set*
run their open-port paths (scePadRead reads the host keyboard/gamepad: nothing pressed in a test).
scePadGetFrameCount is never compared (a free-running counter: every call differs by design);
its arguments are unused.
Evidence (local\wt-typed2-pad-*.txt): Passed 496 Failed 0 unset / 1 / 0; poison flagged 0 both
ways; differential at 256: 136 pairs, 34816 states, compared 34513, nondeterministic 303
(scePadGetFrameCount 256, sceSifAllocIopHeap 46, sceCdReadClock 1), differ 0; stubsigs 155;
stubaudit LEGACYDIFF 0 / IMPLREG 0 / SIGDIFF 0, 24 deviations, dump 155/155.

### Batch 5: memory card (Stubs/MemoryCard.cpp)

23 typed+legacy pairs: every sceMc* handler (the mc* menu helpers below them are not libmc and
were not touched). Shared body for every handler with arguments (mechanically: the old function
became `<name>Body(rdram, ctx, runtime, uint32_t rN...)` with each `getRegU32(ctx, N)` replaced
by rN; the legacy form passes `getRegU32(ctx, N)`, the typed form its declared argument N-4, so
only the source of the raw words differs); sceMcRead/ReadFast/Write/WriteFast share
mcReadBody/mcWriteBody (mcReadCommon/mcWriteCommon still decode for the legacy forms). Verbatim
legacy: sceMcChangeThreadPriority, sceMcEnd, sceMcGetEntSpace, sceMcGetSlotMax, sceMcInit.
Declarations follow libmc, unused trailing arguments included (sceMcGetDir's mode,
sceMcSetFileInfo's info/valid, sceMcSync's mode, sceMcGetEntSpace's port/slot/path). Every
handler returns 0 in v0 and reports through sceMcSync, as before.
Fixture (file side): every sceMc* call starts from the same card files (mc0: "%d %d %d %d" 300
bytes, "%s %d"/icon.sys; mc1: "%d %d %d %d"), fd 1 open on mc0's file; the card trees after the
call are compared. A fixture check in the differential confirms the success paths are reached
(sceMcRead of fd 1 + sceMcSync gives 16 bytes of the file, scePadRead on port 0 succeeds,
sceCdSearchFile finds the CD-root file).
Evidence (local\wt-typed2-mc-*.txt): Passed 496 Failed 0 unset / 1 / 0; poison flagged 0 both
ways; differential at 256: 159 pairs, 40704 states, compared 40401, nondeterministic 303
(scePadGetFrameCount 256, sceSifAllocIopHeap 46, sceCdReadClock 1), differ 0; stubsigs 178;
stubaudit LEGACYDIFF 0 / IMPLREG 0 / SIGDIFF 0, 24 deviations, dump 178/178.

### After the batches: bindcheck, one declaration fixed, game runs

bindcheck (wt-tooling f72a4be) on this tree vs srwz-typed (d1ab9b8's typed code): ARG_IGNORED 7 -> 4
(sceSifGetOtherData, sceCdStream and sceMcSetFileInfo now declare the argument their guest copy
reads), INFO 199 -> 169, SIGTABLE 6 -> 6 after one fix: the game's copy of sceSifResetIop (bound
at 0x124748) reads a0 and a1, so it is declared `(const char *arg, int mode)` now (it was void;
the handler ignores both, nothing changes at run time). The remaining SIGTABLEs are pre-existing
and not typed-layer issues: the internal `_sceSifLoadModule` / `_sceSifLoadModuleBuffer` /
`_sceSifLoadElfPart` / `_sceSifSendCmd` wrappers bound to the public handlers read more registers
than the public prototypes, and sceSifGetIopAddr/SetIopAddr's guest copies read a0-a2 (the
builtin table said 1/2 arguments too).

Final evidence (HEAD, local\wt-typed2-final-*.txt): Passed 496 Failed 0 with PS2X_TYPED_STUBS
unset / 1 / 0; poison flagged 0 (666 handlers) each way; differential at 256 states: 159 pairs,
40704 states, compared 40401, nondeterministic 303 (scePadGetFrameCount 256, sceSifAllocIopHeap
46, sceCdReadClock 1), differ 0 (302 s); ps2x_stubsigs 178 typed, 0 unresolved; stubaudit
LEGACYDIFF 0 / IMPLREG 0 / SIGDIFF 0 / DUMPDIFF 0, 24 declared deviations, flagged 3 (the
sceeFont trio, unchanged).

Game runs: one runner built from srwz-typed2 06c2a74 (all five batches; the later sceSifResetIop
declaration change only adds two ignored decoded arguments) + main's recomp output, headless,
PS2X_NO_AUDIO=1:
- `tools\regress.ps1 -Headless -Seconds 60 -Exe ...` with PS2X_MPEG_HASH=30: t2on
  (PS2X_TYPED_STUBS=1) PASS 27/27, t2off (=0) PASS 27/27; MPEG picture hashes: 46 / 45 logged,
  the 45 both logged (pictures 0..1320 every 30) identical (local\wt-typed2-t2on-hashes.txt,
  local\wt-typed2-t2off-hashes.txt).
- Pad route into the first menu (fpsrun's START at tick 3000, then START at 3400 picks START on
  the menu -> SELECT SCENARIO), `tools\long-run.ps1 -Headless -FastForward 1 -Seconds 85
  -DumpEvery 60 -DumpFrom 3000` with PS2X_PAD_SCRIPT="3000:START+8,3400:START+8" and
  PS2X_TRACE_STUBS=sceMc,scePad,sceSif,sceCd,sceSd: t2padon (=1) vs t2padoff (=0): dumps scored
  by content order, `regress.ps1 -NoRun -Name t2padon -Baseline local\runs\t2padoffdump`: PASS
  36/36. Stub traces: pad and memory-card stubs run (scePadRead/GetState/SetActDirect ~7400 each,
  scePadInit/PortOpen/SetMainMode/InfoMode/InfoAct/SetActAlign; sceMcInit/GetInfo/GetDir/Sync at
  boot); the distinct (stub, arguments) and (stub, v0) sets of the two runs are equal except the
  sound driver's block-transfer polling: the on run made 7 more sceSdRemote(0x8100)/sceSifSetDma
  rounds (1091 vs 1084 SetDma), so later DMA transfer ids are shifted by 7. A second off run
  (t2padoff2: 1087) shows the same kind of difference against t2padoff (wall-clock pacing), so
  it is run-to-run timing, not decoding.

## State (2026-10-07, second worker: steps 1-8 done)

Commits on `srwz-typed` (after the WIP a58b070):
- `0fee8cb` compile-time argument layout (`argLayout`: each argument's register / stack slot is
  fixed at compile time, so decoding never depends on brace-init evaluation order, which MSVC
  has got wrong for constructor calls); a 32-bit or narrower stack argument reads only the low
  word of its slot; `Stubs/Common.h` / `Syscalls/Common.h` set `PS2_STUB_KIND` and include the
  header; CMake option `PS2X_TYPED_STUBS_DEFAULT` (OFF) -> `PS2X_TYPED_STUBS_DEFAULT=0/1` PUBLIC
  on `ps2_runtime`.
- `a48fd75` the pilot set converted (typed + legacy + dispatch) and
  `ps2xTest/src/ps2_stub_abi_tests.cpp` (decoder, dispatcher, registry/JSON, differential).
- `a62d478` `ps2x_stubsigs` dump tool (`ps2xTest/stubsigs/`), JSON sorted by table and name.
- (this notes update)
Tooling on `tooling` (wt-tooling): `7fc3340` stubaudit.py / bindcheck.py read the declarations.

Nothing changes at run time unless `PS2X_TYPED_STUBS=1` (or the build sets
`PS2X_TYPED_STUBS_DEFAULT=ON`): every dispatcher calls the legacy form by default.

How a conversion looks (two patterns, both in the pilot):
- short handler: `PS2_STUB_TYPED(ret, name, params) { converted body }`, then the old function
  renamed `name_legacy` **verbatim**, then `PS2_STUB_DISPATCH(name)`. The differential test then
  compares the whole bodies, so a slip in the converted body shows up too.
- long handler: the body moves into a `static` function taking the decoded values (returns v0,
  or writes it itself for `SelfReturn`); the typed impl and `name_legacy` (old decode lines
  verbatim) both call it, so only the decoding differs.
- aliases: `PS2_STUB_ALIAS(name, target, kind)` for the one-line forwarders.

## Design decisions (keep them unless a reason turns up)

1. **Impl signature keeps `rdram, ctx, runtime` first**: `ret name_impl(uint8_t *rdram,
   R5900Context *ctx, PS2Runtime *runtime, <typed args>)`, so a handler body converts
   mechanically: delete the `getRegU32`/`readStackU32` lines, turn `setReturnS32(ctx, X); return;`
   into `return X;`. Types: fixed-width C ints (int16_t gives the `static_cast<int16_t>(x & 0xFFFF)`
   narrowing the GS stubs do by hand), `float`, `ps2_abi::gptr` (converts to uint32_t), and
   `ps2_abi::VarArgs` last for printf-style handlers.
2. **Decoding rule** (`argLayout`, compile time; was `ArgCursor`): int-class args 1-8 from regs 4-11 (low 64 bits), floats from
   f12-f19 (separate counter), overflow of either class into 8-byte stack slots from sp+0 in
   argument order; VarArgs continue the int sequence (slot i<8 = reg 4+i, else sp+8(i-8)), the
   `Ps2VarArgCursor` semantics exactly. Stack words are read like `tryReadWordFromGuest`
   (direct pointer, then the runtime memory map, else 0) — note the legacy GS/RPC readers used
   `getConstMemPtr`/`getMemPtr` only (0 when sp is outside RAM/scratchpad): a difference only
   for an sp outside RAM; keep the test's sp in RAM and list it in the report. A 32-bit or
   narrower stack argument reads only the low word of its slot.
3. **Returns**: int/short/char/gptr → `setReturnS32` (sign-extended low 32, same as
   setReturnU32); int64/uint64 → `setReturnU64` (v0 + v1 high, the existing helper's convention);
   float → f0; `void` → v0 untouched; `ps2_abi::SelfReturn<T>` → the body writes the registers
   itself (for a shared body between typed and legacy; T recorded for tooling only).
4. **Opt-in switch**: `PS2_STUB_DISPATCH(name)` picks `name_typed` when
   `PS2X_TYPED_STUBS=1` (env) or `PS2X_TYPED_STUBS_DEFAULT=1` (compile define — still to add as a
   CMake option `PS2X_TYPED_STUBS_DEFAULT`, default OFF, on `ps2_runtime` PUBLIC), else
   `name_legacy`. Default stays legacy until the walls thread flips it. `PS2_STUB` (no legacy) is
   for new handlers only during the transition. `ps2_abi::setTypedEnabledForTests(int)` forces it.
5. **Shared-body pattern for long handlers** (SifCallRpc, SifRegisterRpc, sceMpegDemuxPssRing):
   move the body into `static void xBody(rdram, ctx, runtime, <decoded values>)`; the typed impl
   (`SelfReturn<int32_t>`) and `x_legacy` both call it, the legacy keeping the old
   `getRegU32`/`readStackU32` lines verbatim. Then the only difference under test is the decoding.
6. **Aliases**: `PS2_STUB_ALIAS(sceSifCallRpc, SifCallRpc, ::ps2_abi::Kind::Syscall)` for the
   one-line forwarders (stub `sceSifRegisterRpc` → `ps2_syscalls::SifRegisterRpc`, syscall
   `sceSifCallRpc` → `SifCallRpc`). The registry resolves the alias's signature lazily
   (`resolveAlias`), because the target's `_impl` is static in another TU.
7. **Namespace tag**: `Stubs/Common.h` must `#define PS2_STUB_KIND ::ps2_abi::Kind::Stub` and
   `Syscalls/Common.h` `::ps2_abi::Kind::Syscall`, each before `#include "ps2_stub_abi.h"`
   (same handler names exist in both namespaces: sceSifSendCmd, sceSifCallRpc).
8. **Static-library pitfall**: registrars live in the handler TUs; an exe that references no
   handler links none of them. `ps2x_tests` references every handler through the poison test's
   tables; the dump tool must take the address of every `PS2_STUB_LIST`/`PS2_SYSCALL_LIST`
   function (into a volatile array) to pull them in.

## Steps 1-8 as planned (all done; kept for reference)

1. Add the `PS2_STUB_KIND` defines + include to `Stubs/Common.h` and `Syscalls/Common.h`;
   add CMake option `PS2X_TYPED_STUBS_DEFAULT` (OFF) in `ps2xRuntime/CMakeLists.txt`; build
   `ps2x_tests` to shake out header errors (nothing uses the macros yet, so this is cheap).
2. Dump tool: `ps2xTest/stubsigs/` (CMake target `ps2x_stubsigs`, links `ps2_runtime` +
   `ps2_iop` like `ps2_test_lib`), writes `ps2_abi::registryJson()` to argv[1]
   (`ps2x_stub_signatures.json`). Add it to `tools\build-ps2recomp.bat`'s target list later (main
   tree, not now).
3. Convert the pilot set with `PS2_STUB_TYPED` + `_legacy` + `PS2_STUB_DISPATCH`, old reads kept
   verbatim in `_legacy` (the files and lines are in docs/STUB-AUDIT.md):
   - `Syscalls/RPC.cpp`: `sceSifSendCmd` (6 int: u32 cid, gptr pkt, u32 psize, gptr src, gptr dest,
     u32 size; returns s32 1), `SifCallRpc` (9: gptr cd, u32 fno, u32 mode, gptr send, u32 ssize,
     gptr recv, u32 rsize, gptr endfunc, u32 endpara at sp+0; SelfReturn, shared body),
     `SifRegisterRpc` (7: gptr sd, u32 sid, gptr func, gptr buf, gptr cfunc, gptr cbuf, gptr qd;
     s32), alias `sceSifCallRpc`.
   - `Stubs/SIF.cpp`: `sceSifSendCmd` (stub twin, same signature), alias `sceSifRegisterRpc`.
   - `Stubs/GS.cpp`: `sceGsSetDefDBuff` (gptr, int16 x6), `sceGsSetDefDBuffDc` (gptr, u32 psm/w/h,
     u32 ztest/zpsm/clear — keep u32 as today, the SDK says short: note it), `sceGsSetDefDispEnv`
     (gptr, u32 psm, u32 w, u32 h, int16 dx, int16 dy — keep u32 for the first three),
     `sceGsSetDefDrawEnv`/`2` (gptr, int16 x5; `setDefDrawEnvCommon` becomes the shared body),
     `sceGsSetDefClear` (gptr, int16 x5, u8 r,g,b,a, u32 z: b/a/z are stack slots 0/1/2),
     `sceGsSetDefLoadImage` (gptr, u32 x7), alias `sceGsSetDefStoreImage`.
   - `Stubs/MPEG.cpp`: `sceMpegAddStrCallback` (gptr mp, u32 type, u32 id, gptr func, gptr data),
     `sceMpegDemuxPssRing` (gptr mp, gptr data, u32 avail, gptr ring, u32 ringSize; shared body,
     SelfReturn). `readAbiArg4` then goes away.
   - `Stubs/Audio.cpp`: `sceSdRemote` (u32 rec, u32 cmd, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4;
     a0 of the call (`rec`) is unused today — keep it in the signature, 7 int args; u32 return).
   - printf family: `Stubs/LibC.cpp` `printf` (gptr fmt, VarArgs), `sprintf` (gptr s, gptr fmt,
     VarArgs), `snprintf` (gptr s, u32 n, gptr fmt, VarArgs), `fprintf` (u32 fd, gptr fmt, VarArgs);
     `Stubs/Compatibility.cpp` `printf_r` (gptr reent, gptr fmt, VarArgs); `Stubs/TTY.cpp`
     `scePrintf` (void return! it never sets v0 today; gptr fmt, VarArgs). Add
     `formatPs2StringWithVarArgs(rdram, runtime, fmt, ps2_abi::VarArgs&)` to Support.h beside
     `formatPs2StringWithArgs`, built on `formatPs2StringCore` with the VarArgs's nextU32/nextU64.
     `vfprintf`/`vsprintf` take a va_list pointer (gptr) — plain 3-arg conversions, mechanical.
   - Mechanical extras if time allows: the other handlers in `decodeGsSetDefImageArgs` users,
     `sceGsSetDefDrawEnv2`, anything in STUB-AUDIT's "checked and found correct" list.
   Rule: if the typed decoding would change behaviour (an old read that was wrong), keep the old
   behaviour as the default and list it in the report for the walls thread. Known candidates:
   `scePrintf` (v0 never written: keep void), GS `u32` vs SDK `short` (keep u32).
4. Tests, new file `ps2xTest/src/ps2_stub_abi_tests.cpp` (+ CMake + `main.cpp` registration):
   a. decoder unit tests: hand-built contexts; ints in a0-t3, 9th/10th at sp+0/8, floats f12-f19
      with the ninth at sp+0, narrowing (int16 of 0x1_8000 → -32768), returns (s32 sign
      extension, u64 → v0/v1, float → f0), VarArgs slot continuation = `Ps2VarArgCursor`.
   b. differential test: for every registry entry with `legacy != nullptr`, N (env
      `PS2X_ABI_DIFF_ITERS`, default 256) random states: a0-t3 random 64-bit values mixed with
      pointers into zeroed 8 KB buffers and "%d %d %d %d" strings (reuse the poison test's
      layout: kBufBase 0x200000 stride 0x2000, sp in RAM at 0x1E00000 page), random f12-f19,
      random sp+0..0x3f words, random v0/v1. Run legacy twice (nondeterminism guard, like the
      poison test's z1==z2), then typed; compare ctx bytes (insn_count/cop0_count zeroed), rdram
      pages, scratch, IOP RAM, GS VRAM page hashes, dma/gif/gs/vif counters, threw/crashed.
      Restore memory after each call; `runtime->configureGuestHeap(0x800000, 0x1800000)` before
      each call as the poison test does. Report per handler: iterations, nondet count, diffs
      (fail on any diff). Skip nothing: the pilot set has no size-count argument.
   c. dispatcher test: `setTypedEnabledForTests(1/0)` and check `dispatch` reaches
      typed/legacy (count via a test-only registered pair).
   d. JSON round trip: `registryJson()` parses and lists every converted handler.
5. Evidence runs (all via launch.ps1, MergeErr into files under `local\`):
   `ps2x_tests` default env → same results as baseline, `[poison] ... flagged 0`;
   `PS2X_TYPED_STUBS=1 ps2x_tests` → same, `flagged 0`; differential counts per handler;
   `ps2x_stubsigs local\wt-typed-stubsigs.json`.
6. Tooling (worktree `local\wt-tooling`, `git merge main` first):
   - `tools/stubaudit.py`: parse `PS2_STUB(_TYPED|0|0_TYPED)(ret, name, params…)` and
     `PS2_STUB_ALIAS(name, target, kind)` in the Kernel sources → `declared[name]`
     (int count, float count, varargs, stack slots, prototype) as signature source `declared`,
     preferred over `comment`/`builtin` (keep those as fallback). Synthesize the handler row for
     `name` (the dispatcher is a macro, DEF_RE does not see it): reads = regs 4..4+min(n,8)-1,
     sp+8k for the declared stack slots, plus the impl body's own reads (parse the macro header
     as a Func named `name_impl` whose body follows; flag `IMPLREG` if the impl reads an argument
     register directly). Fold `name_legacy` into the row as `legacy_reads`; add finding
     `LEGACYDIFF` (info) when they differ from the declared set — that is the behaviour-change
     list for the report. Exclude `_typed`/`_legacy`/`_impl` as separate handlers. New option
     `--signatures <json>` (the ps2x_stubsigs dump): cross-check counts, finding `DUMPDIFF`
     (severe) on disagreement; print "dump: N handlers, M matched". Extend `--selftest` with a
     macro sample. bindcheck.py uses `stubaudit.gather`/`effective`/`comment_signature`: add
     `stubaudit.declared_signature(f)` and call it first in `Runtime.reads()` (sig_source
     "declared"; fregs from the float count); handle the synthesized `name` Func in
     `Runtime.handler`. Run both selftests and `python tools/stubaudit.py --bound
     local/recomp/srwz.toml` against the worktree (point `RT`/`--root` at `local/wt-typed`:
     stubaudit's ROOT is derived from the script path — add `--root` like bindcheck has).
7. Docs: add the layer to `docs/STUB-AUDIT.md` status and CLAUDE.md (one line each: switch,
   macros, how to convert, the dump tool) — main-tree docs are the coordinator's to commit; put
   the text in the report.
8. Report: design, converted list, evidence (test output, differential counts, sweep lines),
   behaviour differences found, what stubaudit/bindcheck now read, risks for the runtime-reviewer
   (static-init order, alias resolution, the memory-map read path for stack slots, `v0` always
   written by typed non-void handlers where a legacy path might not have), and the plan for the
   remaining ~650 handlers: convert by touch, library by library (libgraph/libpkt first: pure,
   covered by ps2_gs_dbuff_tests), each batch = typed+legacy+dispatch, differential test green,
   poison sweep green under both switch values, then drop the legacy twin one release later and
   flip `PS2X_TYPED_STUBS_DEFAULT` once a full regress.ps1 run under `PS2X_TYPED_STUBS=1`
   matches the baseline.

## Converted (pilot set, 22 typed+legacy pairs + 3 aliases)

| Handler | File | Declared | Pattern |
|---|---|---|---|
| `sceSifSendCmd` (syscall) | Syscalls/RPC.cpp | s32 (u32 cid, gptr pkt, u32 psize, gptr src, gptr dest, u32 size) | verbatim legacy |
| `SifCallRpc` (syscall) | Syscalls/RPC.cpp | SelfReturn<s32> (gptr cd, u32 fno, u32 mode, gptr send, u32 ssize, gptr recv, u32 rsize, gptr endfunc, u32 endpara@sp+0) | shared body `sifCallRpcBody` |
| `SifRegisterRpc` (syscall) | Syscalls/RPC.cpp | s32 (gptr sd, u32 sid, gptr func, gptr buf, gptr cfunc, gptr cbuf, gptr qd) | shared body `sifRegisterRpcBody` |
| `sceSifCallRpc` (syscall) | Syscalls/RPC.cpp | alias of syscall `SifCallRpc` | alias |
| `sceSifSendCmd` (stub) | Stubs/SIF.cpp | as the syscall | verbatim legacy |
| `sceSifRegisterRpc` (stub) | Stubs/SIF.cpp | alias of syscall `SifRegisterRpc` | alias |
| `sceGsSetDefDBuff` | Stubs/GS.cpp | s32 (gptr, s16 x6) | shared body |
| `sceGsSetDefDBuffDc` | Stubs/GS.cpp | s32 (gptr, u32 x6) — SDK: short | shared body |
| `sceGsSetDefDispEnv` | Stubs/GS.cpp | s32 (gptr, u32 psm, u32 w, u32 h, s16 dx, s16 dy) — SDK: short | verbatim legacy |
| `sceGsSetDefDrawEnv`, `sceGsSetDefDrawEnv2` | Stubs/GS.cpp | s32 (gptr, s16 x5) | shared body `setDefDrawEnvBody` |
| `sceGsSetDefClear` | Stubs/GS.cpp | s32 (gptr, s16 x5, u8 r,g,b,a, u32 z): b/a/z at sp+0/8/0x10 | verbatim legacy |
| `sceGsSetDefLoadImage` | Stubs/GS.cpp | s32 (gptr, u32 x7) — SDK: short | verbatim legacy |
| `sceGsSetDefStoreImage` | Stubs/GS.cpp | alias of `sceGsSetDefLoadImage` | alias |
| `sceMpegAddStrCallback` | Stubs/MPEG.cpp | s32 (gptr mp, u32 type, u32 id, gptr func, gptr data) | verbatim legacy |
| `sceMpegDemuxPssRing` | Stubs/MPEG.cpp | s32 (gptr mp, gptr data, u32 avail, gptr ring, u32 ringSize) | shared body `mpegDemuxPssRingBody` |
| `sceSdRemote` | Stubs/Audio.cpp | u32 (u32 arg, u32 cmd, u32 x5) — SDK: variadic | shared body `sdRemoteBody` |
| `printf`, `sprintf`, `snprintf`, `fprintf` | Stubs/LibC.cpp | s32 (fixed..., VarArgs) | verbatim legacy |
| `vfprintf`, `vsprintf` | Stubs/LibC.cpp | s32 (u32/gptr, gptr fmt, gptr va_list) | verbatim legacy |
| `printf_r` | Stubs/Compatibility.cpp | s32 (gptr reent, gptr fmt, VarArgs) | verbatim legacy |
| `scePrintf` | Stubs/TTY.cpp | void (gptr fmt, VarArgs) — SDK: int | verbatim legacy |

`formatPs2StringWithVarArgs(rdram, runtime, fmt, ps2_abi::VarArgs&)` is in Support.h beside
`formatPs2StringWithArgs`; `ps2_abi::VarArgs::peekU32(i)` reads the i-th vararg without moving.
`readAbiArg4` (MPEG) and `decodeGsSetDefImageArgs` / `decodeGsTrailingArgs3` (GS) stay while the
legacy forms do.

## Behaviour differences, typed vs legacy (all kept equal by default; list for the walls thread)

None of these shows up in the tests; each was checked by reading both decodings.
1. `scePrintf` never writes v0 (the SDK's returns int): the typed form is declared `void` to keep
   that. Changing it to `int32_t` would make v0 the rendered length.
2. GS stubs read psm/w/h (DBuffDc, DispEnv) and bp/bw/psm/x/y/w/h (LoadImage) as u32 where the
   SDK says short; declared u32 to keep that. For LoadImage the fields stored are 8/16 bits, so
   short would give the same result; for DBuffDc/DispEnv a w/h with bit 15 set would differ.
3. Stack argument words (SifCallRpc end_para; sceGsSetDefClear b/a/z): the typed wrapper reads
   through `getConstMemPtr`, then the runtime memory map (0 if unmapped); the legacy readers use
   `getMemPtr` / `getConstMemPtr` only (0). Differs only for an sp no direct pointer resolves
   (never in practice: RAM, its KSEG mirrors and the scratchpad all resolve).
4. Every declared argument is decoded up front, before the body runs, including ones the body
   ignores (sceSdRemote's a0, the stub sceSifSendCmd's cid/packet/psize). Register reads have no
   effect; no pilot handler declares an unused stack argument.
5. Return-value writes: `uint32_t` / `int32_t` both go through setReturnS32 (sign-extended low
   32), as the legacy setReturnU32/S32 do: same bits. 64-bit returns would set v1 = high word
   (setReturnU64's convention) — no pilot handler returns 64 bits.
6. Moved code (both forms now, so not a typed/legacy difference, but a change from 3d86345):
   SifCallRpc reads its registers before taking `g_sif_call_rpc_mutex` (was after);
   sceMpegDemuxPssRing's entry-count log runs after decoding. Neither is observable.
7. Duplicated static log counters in the verbatim-legacy pattern (sceSifSendCmd's 5-line log,
   sprintf's watch log): each form has its own, so a process that flips the switch could log
   twice as many lines. The switch is read once per process (tests force it).
8. Varargs layout limit: a float argument that overflows to the stack before varargs would make
   the vararg stack slots collide (argLayout comment). No SDK printf-style function does this.

Found on the way (pre-existing, same in both forms, not fixed here):
- `sceMpegDemuxPssRing` -> `appendGuestBytes` (Stubs/MPEG.cpp ~1627) bounds each chunk by
  `PS2_RAM_SIZE - offset` only: with `data` in the scratchpad (0x70000000..) and `avail` larger
  than what is left of the 16 KB scratchpad, it reads past the host scratchpad buffer (access
  violation 0xc0000005 in the differential test, state: a0=0x700001f3 a1=0x700009da
  a2=0x21e0b4). Fix: bound the chunk by the region (`getConstMemPtr` + scratchpad size). The
  differential test no longer generates scratchpad pointers because of it.
- The stub `sceSifSendCmd` copies byte by byte through `getConstMemPtr` (slow for big sizes from
  the scratchpad, ~200 ms per call in the test); shadowed by the syscall for SRWZ.

## Evidence (files under local\)

- Baseline (3d86345, before any change): `wt-typed-tests0.txt` — Passed 487, Failed 0;
  `[poison] handlers 666: ... flagged 0`.
- After (a62d478), 4 runs (`wt-typed-tests5-<name>.txt`):
  - default env: Passed 495 Failed 0; `[abi] typed stubs off`; poison flagged 0;
    `[abi-diff] handlers 22: states 1408, compared 1408, nondeterministic 0, differ 0`.
  - `PS2X_TYPED_STUBS=1`: Passed 495 Failed 0; `[abi] typed stubs ON`; poison flagged 0
    (the sweep goes through the typed dispatchers); differential as above.
  - `PS2X_TYPED_STUBS=0`: same as default.
  - `PS2X_ABI_DIFF_ITERS=256`: `states 5632, compared 5632, nondeterministic 0, differ 0` (21 s).
  (+8 tests: the StubAbi suite.) Default per-handler budget 3 s keeps the suite at ~25 s.
- The harness catches a wrong decoding: `abiSelfTestBadPair` (legacy reads sp+0x10) differs; it
  also caught my own first `abiSelfTestPair`, declared with 3 arguments while its legacy form
  read t0.
- `ps2x_stubsigs local\wt-typed-stubsigs.json`: 25 typed handlers (of 666), 0 unresolved.
- stubaudit (wt-tooling) on this tree: `typed (ps2_stub_abi.h): 25 (25 with a legacy form, 3
  aliases); LEGACYDIFF 0, IMPLREG 0, SIGDIFF 0`, `dump: 25 handlers, 25 matched`, flagged 3
  (the sceeFont* trio, unchanged); on 3d86345 flagged 9 before the stride fix, 3 after.
- Game runs, one runner built from this branch (a62d478 + main's recomp output), headless, 60 s,
  `tools\regress.ps1 -Exe ... -Headless` against local\runs\baseline:
  - `PS2X_TYPED_STUBS=1`: typed1 PASS 27/27; typedh1 (`PS2X_MPEG_HASH=30`) PASS 27/27.
  - `PS2X_TYPED_STUBS=0`: typed0 PASS 26/27 (1 outside the baseline); typedh0 FAIL 25/27, the
    right content (logos, copyright) ~600 ticks late: the host was loaded (tick 2280 at 56.7 s
    against 3360 at 57.9 s in typedh1), not a decoding effect; the rerun typedh0b PASS 27/27.
  - Movie pictures: typedh1 logged 42 `[MPEG:hash]` lines, typedh0b 45; the 42 both logged
    (picture numbers 0, 30, 60, ...) have identical FNV hashes (local\wt-typed-typedh1-hashes.txt,
    local\wt-typed-typedh0b-hashes.txt). These 60 s cover boot, SIF RPC, libgraph setup, the
    sound driver and the start of the opening movie (DemuxPssRing), not the title (~470 s).
- bindcheck (wt-tooling) on this tree vs 3d86345: identical findings (1 ERROR, 384 WARN, 199
  INFO); typed rows say `sig N args, declared` and list the legacy form's reads.

## Risks for the runtime-reviewer

- Static-init order: registrars run in each handler TU's static init; the registry is a
  function-local static (safe). Alias signatures resolve lazily at read time.
- Static-library pitfall: a TU's registrars run only if the exe links that TU. The runner and
  ps2x_tests reference every handler through the call tables; ps2x_stubsigs takes their
  addresses on purpose. A future tool that reads the registry must do the same.
- `typedEnabled()` per call: one relaxed atomic load plus a function-local static guard; env read
  once per process (`PS2X_TYPED_STUBS`, `0` = legacy, anything else = typed).
- The typed path writes v0 for every non-void handler (writeReturn after the impl returns); a
  legacy path that returned early without writing v0 would differ. In the pilot set every legacy
  path writes v0 (checked; the differential compares the whole register file), except
  scePrintf, declared void.
- Native crashes inside a handler leave its std::mutex locked (SEH skips destructors); the
  differential test stops that handler and fails, so a crash is never hidden.
- Mixed int/float stack overflow and varargs after float overflow are untested in games.
- `name_typed` / `name_legacy` have external linkage in their namespace (not declared in the
  headers): fine for MSVC/clang, keeps them addressable from tests.

## Plan for the remaining handlers (updated after the srwz-typed2 batches)

Converted: 178 registry entries (159 typed+legacy pairs, 19 aliases) of 666 handlers: the pilot
(25), SIF/RPC (60), CDVD (39), sound (2), pad (29), memory card (23). Unconverted: ~360
implemented handlers plus 128 TODO_NAMED stubs (converting a TODO stub only declares a
signature; leave them until they are implemented).
Convert by touch, library by library, each batch one commit: typed + verbatim legacy (or shared
body) + dispatch, the batch's names added to `kConverted` in ps2_stub_abi_tests.cpp, ps2x_tests
green with `PS2X_TYPED_STUBS` unset / 1 / 0, differential 0 differ at 256 states, poison flagged 0
both ways, stubaudit `--signatures` all matched and LEGACYDIFF reviewed, bindcheck SIGTABLE /
ARG_IGNORED not worse (check each declaration against the game's own copy where bound).
A stateful library needs its snapshot hooks in the harness's LibHook list (Harness::
prepareLibraryFixture) and, if it touches host files, a file fixture like the memory card's.
Next, in order:
1. libmpeg / IPU (MPEG.cpp, IPU.cpp): stateful (decoder), the in-game movie path; add the MPEG
   snapshot hooks to the fixture only if loading them is cheap (decoder replay), else keep
   resetMpegStubState.
2. libgraph / libpkt (GS.cpp, ~55 left): pure, also covered by ps2_gs_dbuff_tests.
3. libvu0 (VU.cpp): the float ABI's first real users; verify against the game's own SDK code
   (sceVu0ViewScreenMatrix etc.), not a legacy twin; add the mixed int/float stack test.
4. libc / newlib (LibC.cpp, Compatibility.cpp), FileIO (Stubs and Syscalls), DMA, TTY, Deci2.
5. Syscalls (Thread/Sync/Interrupt/System): last, they are the most exercised.
Then the default flip gate (reviewer note 3 above): PS2X_TRACE_STUBS capture of real arguments
checked against the SDK prototypes (ideally a second title), a full `tools
egress.ps1` run with
`PS2X_MPEG_HASH` under `PS2X_TYPED_STUBS=1` matching the baseline; flip
`PS2X_TYPED_STUBS_DEFAULT` ON; one release later drop the `_legacy` twins (each typed impl is
already in its final form; shared bodies fold back into the impl).

## Docs text for the coordinator (main tree)

docs/STUB-AUDIT.md, Status: "Typed stub ABI layer (PS2Recomp srwz-typed/-typed2, opt-in): handlers
declared with `PS2_STUB` / `PS2_STUB_TYPED` + `PS2_STUB_DISPATCH` decode their arguments from
the declared C signature (ps2xRuntime/include/ps2_stub_abi.h); 178 converted (pilot: SIF RPC/cmd,
libgraph SetDef*, MPEG AddStrCallback/DemuxPssRing, sceSdRemote, printf family; then all of
SIF/RPC, CDVD, pad and memory card, and the implemented sound stubs), each with its old decoding
kept as `_legacy` behind `PS2X_TYPED_STUBS` (default legacy); deliberate deviations from the SDK
prototypes are marked with `PS2_STUB_DEVIATION` (24) and reported as such by stubaudit/bindcheck.
The differential test (ps2_stub_abi_tests.cpp) shows typed == legacy on 40704 random states (159
pairs), each call from the same library state (snapshot hooks) and memory-card files."

CLAUDE.md, beside the stub tests line: "Typed stub ABI layer (`ps2_stub_abi.h`, opt-in):
`PS2X_TYPED_STUBS=1` (or CMake `PS2X_TYPED_STUBS_DEFAULT=ON`) runs the typed decoding of the
converted handlers; convert one with `PS2_STUB_TYPED(ret, name, params) {...}` + the old body as
`name_legacy` + `PS2_STUB_DISPATCH(name)` (NOTES in the branch); `ps2x_stubsigs <out.json>`
dumps the declared signatures for `python tools/stubaudit.py --signatures <json>`;
`PS2X_ABI_DIFF_ITERS=N ps2x_tests` runs the typed/legacy differential on N states per handler;
`PS2_STUB_DEVIATION(name, why)` marks a declaration that deviates from the SDK on purpose."

## Commands

    # build tests (incremental once configured)
    powershell -File tools\wt-build.ps1 -Worktree local\wt-typed -Targets ps2x_tests -NoLock
    # run them quietly
    . tools\launch.ps1
    Invoke-Quiet local\wt-typed\out\build\ps2xTest\Release\ps2x_tests.exe @() -MergeErr > local\wt-typed-tests.txt
    Invoke-Quiet local\wt-typed\out\build\ps2xTest\Release\ps2x_tests.exe @() -MergeErr -Env @{PS2X_TYPED_STUBS='1'} > local\wt-typed-tests-typed.txt
    # differential at 256 states per handler, one handler family only
    Invoke-Quiet local\wt-typed\out\build\ps2xTest\Release\ps2x_tests.exe @() -MergeErr -Env @{PS2X_ABI_DIFF_ITERS='256'; PS2X_ABI_DIFF_ONLY='sceGs'; PS2X_POISON_ONLY='none'}
    # signature dump + audit
    Invoke-Quiet local\wt-typed\out\build\ps2xTest\stubsigs\Release\ps2x_stubsigs.exe @("$PWD\local\wt-typed-stubsigs.json") -MergeErr
    python local\wt-tooling\tools\stubaudit.py --root local\wt-typed --bound local\recomp\srwz.toml --signatures local\wt-typed-stubsigs.json
