#pragma once

// Snapshot hooks of the SDK stubs and syscalls (docs/SNAPSHOT-DESIGN.md). Their state is
// file-local (anonymous namespaces), so each file saves and restores its own; the
// snapshot service calls them in a fixed order.

#include <cstdint>
#include <functional>

#include "ps2x/state_stream.h"

class PS2Runtime;
struct R5900Context;
struct EeResumeTag;
struct GuestInvocationCompleteTag;

namespace ps2_stubs::snapshot
{
    using Hook = void (*)(ps2x::StateWriter &);
    using LoadHook = void (*)(ps2x::StateReader &, PS2Runtime &);

    // One pair per stub source file (Kernel/Stubs/*.cpp).
    void saveAudio(ps2x::StateWriter &w);
    void loadAudio(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveCd(ps2x::StateWriter &w);
    void loadCd(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveCompatibility(ps2x::StateWriter &w);
    void loadCompatibility(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveDeci2(ps2x::StateWriter &w);
    void loadDeci2(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveDma(ps2x::StateWriter &w);
    void loadDma(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveFileIo(ps2x::StateWriter &w);
    void loadFileIo(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveFont(ps2x::StateWriter &w);
    void loadFont(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveGs(ps2x::StateWriter &w);
    void loadGs(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveIpu(ps2x::StateWriter &w);
    void loadIpu(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveLibC(ps2x::StateWriter &w);
    void loadLibC(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveMemoryCard(ps2x::StateWriter &w);
    void loadMemoryCard(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveMpeg(ps2x::StateWriter &w);
    void loadMpeg(ps2x::StateReader &r, PS2Runtime &runtime);
    void savePad(ps2x::StateWriter &w);
    void loadPad(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveRpc(ps2x::StateWriter &w);
    void loadRpc(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveSif(ps2x::StateWriter &w);
    void loadSif(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveSystem(ps2x::StateWriter &w);
    void loadSystem(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveTty(ps2x::StateWriter &w);
    void loadTty(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveUnimplemented(ps2x::StateWriter &w);
    void loadUnimplemented(ps2x::StateReader &r, PS2Runtime &runtime);
    void saveVu(ps2x::StateWriter &w);
    void loadVu(ps2x::StateReader &r, PS2Runtime &runtime);

    // Rebuild the wait-completion closures a snapshot stored as tags (EeResumeTag).
    std::function<void(R5900Context &)> makeCdStReadResume(PS2Runtime &runtime, const EeResumeTag &tag);
    std::function<void(R5900Context &)> makeMpegGetPictureResume(PS2Runtime &runtime);

    // Rebuild an invocation completion a snapshot stored as GuestInvocationCompleteKind::IpuInitComplete.
    std::function<void(const R5900Context &, R5900Context &)> makeIpuInitComplete(PS2Runtime &runtime);
    // ... or as RpcClientComplete: a deferred sceSifCallRpc (NOWAIT) completion, what the RPC
    // END packet does on the EE after the end function: signal the completion semaphore
    // (tag.arg2: id | count << 32) and mark the SIF RPC client idle unless a newer call has
    // started on it since (tag.arg: client address | call sequence << 32). Syscalls/Snapshot.cpp;
    // the live completion in RPC.cpp uses the same closure.
    std::function<void(const R5900Context &, R5900Context &)> makeRpcClientComplete(PS2Runtime &runtime,
                                                                                   const GuestInvocationCompleteTag &tag);
    // ... and as GuestInvocationCompleteKind::MpegNodataComplete (sceMpegGetPicture's no-data callback).
    std::function<void(const R5900Context &, R5900Context &)> makeMpegNodataComplete(PS2Runtime &runtime, uint32_t cbDataAddr);

    // False when an MPEG playback has a live decoder whose replay log was dropped (its
    // pictures could not be restored); `why` describes it. activeOnly: ignore playbacks
    // whose stream has ended (the decoder lingers until the game deletes the object).
    bool mpegReplayHealthy(std::string *why = nullptr, bool activeOnly = false);
}

namespace ps2_syscalls::snapshot
{
    // Kernel/Syscalls state (Helpers/State.h, DECI2, VAG accumulators).
    void save(ps2x::StateWriter &w);
    void load(ps2x::StateReader &r, PS2Runtime &runtime);
}
