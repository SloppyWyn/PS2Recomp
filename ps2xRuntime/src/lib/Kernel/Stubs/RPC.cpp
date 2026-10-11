#include "Common.h"
#include "RPC.h"

namespace ps2_stubs
{
    void sceRpcFreePacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceRpcFreePacket", rdram, ctx, runtime);
    }

    void sceRpcGetFPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceRpcGetFPacket", rdram, ctx, runtime);
    }

    void sceRpcGetFPacket2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceRpcGetFPacket2", rdram, ctx, runtime);
    }
}

#include "ps2_snapshot_hooks.h"

namespace ps2_stubs::snapshot
{
    // No state of its own: only this file's copy of the Helpers/Support.h globals.
    void saveRpc(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
    }

    void loadRpc(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        (void)runtime;
        snapshotLoadSupport(r);
    }
}
