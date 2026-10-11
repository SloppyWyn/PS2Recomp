#include "Common.h"
#include "RPC.h"
#include "../../ps2_iop_transport.h"
#include "../ps2_snapshot_hooks.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    // Guest time between a NOWAIT sceSifCallRpc served by the IOP and its completion on the
    // EE (end function, semaphore, client idle): ~200 us of EE cycles. An estimate of the IOP
    // turnaround (SIF DMA of the packet, the server's work, the END packet back), not a
    // measurement; docs/SPU2-MODEL.md.
    constexpr uint64_t kSifRpcNowaitCompletionCycles = 200u * 294912000ull / 1000000ull;
}

namespace ps2_syscalls
{
    namespace
    {
        SifRpcDebugEvent makeRpcDebugEvent(const char *op, R5900Context *ctx, PS2Runtime *runtime)
        {
            SifRpcDebugEvent event{};
            event.op = op;
            event.pc = ctx ? ctx->pc : 0u;
            event.ra = ctx ? getRegU32(ctx, 31) : 0u;
            event.threadId = runtime ? static_cast<uint32_t>(runtime->eeScheduler().currentThreadId()) : 0u;
            return event;
        }

        void pushSifRpcDebugEventLocked(SifRpcDebugEvent event)
        {
            event.seq = ++g_sif_rpc_debug_next_seq;
            g_sif_rpc_debug_history[event.seq % kSifRpcDebugHistoryCount] = event;
        }

        void pushSifRpcDebugEvent(SifRpcDebugEvent event)
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            pushSifRpcDebugEventLocked(event);
        }

        void fillRpcDebugPreview(const uint8_t *rdram, uint32_t addr, uint32_t size, uint8_t *preview, uint32_t &previewSize)
        {
            previewSize = 0u;
            if (!rdram || !addr || !preview || size == 0u)
            {
                return;
            }

            const uint32_t count = std::min<uint32_t>(size, static_cast<uint32_t>(kSifRpcDebugPreviewBytes));
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t *ptr = getConstMemPtr(rdram, addr + i);
                if (!ptr)
                {
                    break;
                }
                preview[i] = *ptr;
                ++previewSize;
            }
        }

        std::string formatRpcTraceBytes(const uint8_t *bytes, uint32_t count)
        {
            std::string out;
            if (!bytes || count == 0u)
            {
                return out;
            }

            char item[4] = {};
            for (uint32_t i = 0; i < count; ++i)
            {
                std::snprintf(item, sizeof(item), "%02X", bytes[i]);
                if (!out.empty())
                {
                    out.push_back(' ');
                }
                out += item;
            }
            return out;
        }

#ifndef PS2X_ENABLE_IOP_RPC_TRACE
#define PS2X_ENABLE_IOP_RPC_TRACE 1
#endif

#if PS2X_ENABLE_IOP_RPC_TRACE
        std::string loadedIopModuleTraceSummary()
        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            std::string out;
            uint32_t count = 0u;
            for (const auto &entry : g_sif_modules_by_id)
            {
                const SifModuleRecord &module = entry.second;
                if (!module.loaded)
                {
                    continue;
                }

                if (!out.empty())
                {
                    out += "; ";
                }
                out += module.pathKey.empty() ? module.path : module.pathKey;
                ++count;
                if (count >= 6u)
                {
                    break;
                }
            }
            return out;
        }

        void logUnhandledRpcTrace(const SifRpcDebugEvent &event)
        {
            static std::unordered_set<uint64_t> loggedSignatures;
            if (std::strcmp(event.op ? event.op : "", "CallRpc") != 0)
            {
                return;
            }

            uint64_t signature = 1469598103934665603ull;
            auto mixSignature = [&](uint32_t value)
            {
                signature ^= static_cast<uint64_t>(value);
                signature *= 1099511628211ull;
            };
            mixSignature(event.sid);
            mixSignature(event.rpcNum);
            mixSignature(event.sendSize);
            mixSignature(event.recvSize);
            if (!loggedSignatures.insert(signature).second || loggedSignatures.size() > 128u)
            {
                return;
            }

            const std::string modules = loadedIopModuleTraceSummary();
            std::cerr << "[IOP/RPC trace:unhandled]"
                      << " sid=0x" << std::hex << event.sid
                      << " rpc=0x" << event.rpcNum
                      << " pc=0x" << event.pc
                      << " ra=0x" << event.ra
                      << " send=0x" << event.sendBuf << "/" << std::dec << event.sendSize
                      << " recv=0x" << std::hex << event.recvBuf << "/" << std::dec << event.recvSize
                      << " sendBytes=[" << formatRpcTraceBytes(event.sendPreview, event.sendPreviewSize) << "]"
                      << " loadedModules=[" << modules << "]"
                      << std::dec << std::endl;
        }
#endif

        bool signalRpcCompletionSema(PS2Runtime *runtime, uint32_t semaId)
        {
            if (!runtime || semaId == 0u || semaId > 0xFFFFu)
            {
                return false;
            }
            return runtime->eeScheduler().signalSemaphore(static_cast<int>(semaId), true) >= 0;
        }

    } // namespace

    // The body of sceSifStopModule after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sifStopModuleBody(uint8_t *rdram, PS2Runtime *runtime, int32_t moduleId, uint32_t resultAddr)
    {
        int32_t moduleResult = -1;
        const bool stoppedByEmulator = runtime->stopIopModule(moduleId, &moduleResult);
        uint32_t refsLeft = 0;
        const bool knownModule = trackSifModuleStop(moduleId, &refsLeft);
        const int32_t ret = (stoppedByEmulator || knownModule) ? 0 : -1;

        if (resultAddr != 0)
        {
            int32_t *hostResult = reinterpret_cast<int32_t *>(getMemPtr(rdram, resultAddr));
            if (hostResult)
            {
                *hostResult = stoppedByEmulator ? moduleResult : (knownModule ? 0 : -1);
            }
        }

        if (stoppedByEmulator || knownModule)
        {
            std::string modulePath;
            {
                std::lock_guard<std::mutex> lock(g_sif_module_mutex);
                auto it = g_sif_modules_by_id.find(moduleId);
                if (it != g_sif_modules_by_id.end())
                {
                    modulePath = it->second.path;
                }
            }
            logSifModuleAction(stoppedByEmulator ? "stop-emulated" : "stop", moduleId, modulePath, refsLeft);
        }

        return ret;
    }

    // int sceSifStopModule(int modid, int args, const char *argp, int *result)
    PS2_STUB_TYPED(int32_t, SifStopModule, int32_t moduleId, int32_t args, ps2_abi::gptr argp, ps2_abi::gptr result)
    {
        (void)ctx;
        (void)args;
        (void)argp;
        return sifStopModuleBody(rdram, runtime, moduleId, result);
    }

    void SifStopModule_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t moduleId = static_cast<int32_t>(getRegU32(ctx, 4)); // $a0
        const uint32_t resultAddr = getRegU32(ctx, 7);                    // $a3 (int* result, optional)

        setReturnS32(ctx, sifStopModuleBody(rdram, runtime, moduleId, resultAddr));
    }

    PS2_STUB_DISPATCH(SifStopModule)

    // The body of sceSifLoadModule after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sifLoadModuleBody(uint8_t *rdram, PS2Runtime *runtime, uint32_t pathAddr, uint32_t argumentSize,
                                     uint32_t argumentAddr)
    {
        const std::string modulePath = readGuestCStringBounded(rdram, pathAddr, kMaxSifModulePathBytes);
        if (modulePath.empty())
        {
            return -1;
        }

        std::vector<uint8_t> arguments;
        constexpr uint32_t kMaxIopModuleArguments = 64u * 1024u;
        if (!copyGuestBytesBounded(rdram, argumentAddr, argumentSize, kMaxIopModuleArguments, arguments))
        {
            return -1;
        }

        if (!runtime)
        {
            return -1;
        }

        const auto loaded = runtime->loadIopModule(modulePath, arguments.empty() ? nullptr : arguments.data(), static_cast<uint32_t>(arguments.size()));
        if (!loaded.handled || loaded.moduleId <= 0)
        {
            return -1;
        }

        trackSifModuleLoadExternal(modulePath, loaded.moduleId);
        logSifModuleAction("load-emulated", loaded.moduleId, modulePath, 1u);
        return loaded.moduleId;
    }

    // int sceSifLoadModule(const char *filename, int args, const char *argp)
    PS2_STUB_TYPED(int32_t, SifLoadModule, ps2_abi::gptr path, int32_t args, ps2_abi::gptr argp)
    {
        (void)ctx;
        return sifLoadModuleBody(rdram, runtime, path, static_cast<uint32_t>(args), argp);
    }

    void SifLoadModule_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pathAddr = getRegU32(ctx, 4);     // $a0
        const uint32_t argumentSize = getRegU32(ctx, 5); // $a1
        const uint32_t argumentAddr = getRegU32(ctx, 6); // $a2

        setReturnS32(ctx, sifLoadModuleBody(rdram, runtime, pathAddr, argumentSize, argumentAddr));
    }

    PS2_STUB_DISPATCH(SifLoadModule)

    // void sceSifInitRpc(unsigned int mode)
    PS2_STUB_TYPED(int32_t, SifInitRpc, uint32_t mode)
    {
        (void)rdram;
        (void)mode;
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (!g_rpc_initialized)
        {
            g_rpc_servers.clear();
            g_rpc_clients.clear();
            g_rpc_next_id = 1;
            g_rpc_packet_index = 0;
            g_rpc_server_index = 0;
            g_rpc_active_queue = 0;
            g_sif_rpc_debug_next_seq = 0;
            for (size_t i = 0; i < kSifRpcDebugHistoryCount; ++i)
            {
                g_sif_rpc_debug_history[i] = SifRpcDebugEvent{};
            }
            g_rpc_initialized = true;
            RUNTIME_LOG("[SifInitRpc] Initialized");
        }

        SifRpcDebugEvent event = makeRpcDebugEvent("InitRpc", ctx, runtime);
        event.result = 0;
        pushSifRpcDebugEventLocked(event);
        return 0;
    }

    void SifInitRpc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (!g_rpc_initialized)
        {
            g_rpc_servers.clear();
            g_rpc_clients.clear();
            g_rpc_next_id = 1;
            g_rpc_packet_index = 0;
            g_rpc_server_index = 0;
            g_rpc_active_queue = 0;
            g_sif_rpc_debug_next_seq = 0;
            for (size_t i = 0; i < kSifRpcDebugHistoryCount; ++i)
            {
                g_sif_rpc_debug_history[i] = SifRpcDebugEvent{};
            }
            g_rpc_initialized = true;
            RUNTIME_LOG("[SifInitRpc] Initialized");
        }

        SifRpcDebugEvent event = makeRpcDebugEvent("InitRpc", ctx, runtime);
        event.result = 0;
        pushSifRpcDebugEventLocked(event);
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(SifInitRpc)
    PS2_STUB_DEVIATION(SifInitRpc, "SDK: void sceSifInitRpc(unsigned int mode); the handler writes 0 to v0 (kept)")

    // The body of sceSifBindRpc after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sifBindRpcBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t clientPtr,
                                  uint32_t rpcId, uint32_t mode)
    {
        t_SifRpcClientData *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));

        if (!client)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("BindRpc", ctx, runtime);
            event.clientPtr = clientPtr;
            event.sid = rpcId;
            event.mode = mode;
            event.flags = kSifRpcDebugFlagMissingClient;
            event.result = -1;
            pushSifRpcDebugEvent(event);
            return -1;
        }

        client->command = 0;
        client->buf = 0;
        client->cbuf = 0;
        client->end_function = 0;
        client->end_param = 0;
        client->server = 0;
        client->hdr.pkt_addr = 0;
        client->hdr.sema_id = -1;
        client->hdr.mode = mode;

        uint32_t serverPtr = 0;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            client->hdr.rpc_id = g_rpc_next_id++;
            auto it = g_rpc_servers.find(rpcId);
            if (it != g_rpc_servers.end())
            {
                serverPtr = it->second.sd_ptr;
            }
            g_rpc_clients[clientPtr] = {};
            g_rpc_clients[clientPtr].sid = rpcId;
        }

        if (!serverPtr && PS2IopTransport::canBindRpc(runtime, rpcId))
        {
            // EE-side servers and HLE routes need a descriptor in guest RAM.
            // With an emulated IOP, only publish it after the IRX has actually
            // registered the SID; cd->server == nullptr is the SDK's retry
            // signal while the IOP server thread is still starting.
            serverPtr = rpcAllocServerAddr(rdram);
            if (serverPtr)
            {
                t_SifRpcServerData *dummy = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr));
                if (dummy)
                {
                    std::memset(dummy, 0, sizeof(*dummy));
                    dummy->sid = static_cast<int>(rpcId);
                }
                std::lock_guard<std::mutex> lock(g_rpc_mutex);
                g_rpc_servers[rpcId] = {rpcId, serverPtr};
            }
        }

        if (serverPtr)
        {
            t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr));
            client->server = serverPtr;
            client->buf = sd ? sd->buf : 0;
            client->cbuf = sd ? sd->cbuf : 0;
        }
        else
        {
            client->server = 0;
            client->buf = 0;
            client->cbuf = 0;
        }

        SifRpcDebugEvent event = makeRpcDebugEvent("BindRpc", ctx, runtime);
        event.clientPtr = clientPtr;
        event.serverPtr = serverPtr;
        event.sid = rpcId;
        event.mode = mode;
        event.result = 0;
        pushSifRpcDebugEvent(event);
        return 0;
    }

    // int sceSifBindRpc(sceSifClientData *cd, unsigned int request, unsigned int mode)
    PS2_STUB_TYPED(int32_t, SifBindRpc, ps2_abi::gptr cd, uint32_t request, uint32_t mode)
    {
        return sifBindRpcBody(rdram, ctx, runtime, cd, request, mode);
    }

    void SifBindRpc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clientPtr = getRegU32(ctx, 4);
        uint32_t rpcId = getRegU32(ctx, 5);
        uint32_t mode = getRegU32(ctx, 6);

        setReturnS32(ctx, sifBindRpcBody(rdram, ctx, runtime, clientPtr, rpcId, mode));
    }

    PS2_STUB_DISPATCH(SifBindRpc)

    // The body of SifCallRpc, shared by the typed and the legacy decoding (ps2_stub_abi.h). It
    // writes v0 itself, possibly from finishCall after a guest callback.
    static void sifCallRpcBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t clientPtr,
                               uint32_t rpcNum, uint32_t mode, uint32_t sendBuf, uint32_t sendSize,
                               uint32_t receiveBuffer, uint32_t receiveSize, uint32_t endFunction,
                               uint32_t endParameter)
    {
        std::lock_guard<std::recursive_mutex> rpcCallLock(g_sif_call_rpc_mutex);

        uint32_t sidHint = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            const auto clientIt = g_rpc_clients.find(clientPtr);
            if (clientIt != g_rpc_clients.end())
            {
                sidHint = clientIt->second.sid;
            }
        }

        auto *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));
        if (!client)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("CallRpc", ctx, runtime);
            event.clientPtr = clientPtr;
            event.sid = sidHint;
            event.rpcNum = rpcNum;
            event.mode = mode;
            event.sendBuf = sendBuf;
            event.sendSize = sendSize;
            event.recvBuf = receiveBuffer;
            event.recvSize = receiveSize;
            event.endFunc = endFunction;
            event.endParam = endParameter;
            event.flags = kSifRpcDebugFlagMissingClient | ((mode & kSifRpcModeNowait) ? kSifRpcDebugFlagNowait : 0u);
            event.result = -1;
            pushSifRpcDebugEvent(event);
            setReturnS32(ctx, -1);
            return;
        }

        client->command = rpcNum;
        client->end_function = endFunction;
        client->end_param = endParameter;
        client->hdr.mode = mode;

        uint32_t sid = 0u;
        uint32_t callSequence = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            auto &state = g_rpc_clients[clientPtr];
            state.busy = true;
            callSequence = ++state.callSequence;
            state.last_rpc = rpcNum;
            sid = state.sid;
            if (sid != 0u)
            {
                const auto serverIt = g_rpc_servers.find(sid);
                if (serverIt != g_rpc_servers.end() &&
                    serverIt->second.sd_ptr != 0u)
                {
                    client->server = serverIt->second.sd_ptr;
                }
            }
        }

        const uint32_t serverPtr = client->server;
        auto *server = serverPtr
                           ? reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr))
                           : nullptr;
        if (server)
        {
            server->client = clientPtr;
            server->pkt_addr = client->hdr.pkt_addr;
            server->rpc_number = rpcNum;
            server->size = static_cast<int>(sendSize);
            server->recvbuf = receiveBuffer;
            server->rsize = static_cast<int>(receiveSize);
            server->rmode = ((mode & kSifRpcModeNowait) && endFunction == 0u) ? 0 : 1;
            server->rid = 0;

            if (server->buf != 0u && sendBuf != 0u && sendSize != 0u)
            {
                rpcCopyToRdram(rdram, server->buf, sendBuf, sendSize);
            }
        }

        ps2x::iop::RpcResult iopResult{};
        uint32_t completionSemaphore = static_cast<uint32_t>(client->hdr.sema_id);
        if (completionSemaphore == 0xFFFFFFFFu || completionSemaphore == 0u)
        {
            completionSemaphore = endParameter;
        }

        {
            ps2x::iop::RpcRequest request{};
            request.clientAddress = clientPtr;
            request.serverAddress = serverPtr;
            request.serverFunction = server ? server->func : 0u;
            request.serverBuffer = server ? server->buf : 0u;
            request.sid = sid;
            request.function = rpcNum;
            request.mode = mode;
            request.send = {sendBuf, sendSize};
            request.receive = {receiveBuffer, receiveSize};
            request.endFunction = endFunction;
            request.endParameter = endParameter;

            iopResult = PS2IopTransport::handleRpc(runtime, rdram, ctx, request);
        }
        // Completion semaphore signals the IOP's answer asks for. A synchronous call gives
        // them now; a deferred NOWAIT call gives them at its completion (below), as the END
        // packet does on the hardware.
        uint32_t completionSignals = 0u;
        if (iopResult.signalNowaitCompletion && (mode & kSifRpcModeNowait) != 0u)
            ++completionSignals;
        if (iopResult.signalCompletion)
            ++completionSignals;

        uint32_t guestFunction = iopResult.guestFunction;
        uint32_t guestA0 = iopResult.guestArguments[0];
        uint32_t guestA1 = iopResult.guestArguments[1];
        uint32_t guestA2 = iopResult.guestArguments[2];
        uint32_t guestA3 = iopResult.guestArguments[3];
        uint32_t guestDefaultResult = iopResult.guestDefaultResultAddress;
        if (guestFunction == 0u && server && server->func != 0u &&
            iopResult.serverDispatchPolicy != ps2x::iop::ServerDispatchPolicy::Suppress)
        {
            guestFunction = server->func;
            guestA0 = rpcNum;
            guestA1 = server->buf;
            guestA2 = sendSize;
            guestDefaultResult = server->buf != 0u ? server->buf : receiveBuffer;
        }
        const bool serverDispatched = guestFunction != 0u && runtime->hasFunction(guestFunction);
        const bool deferredNowait = (mode & kSifRpcModeNowait) != 0u && !serverDispatched;
        if (!deferredNowait)
        {
            for (uint32_t i = 0u; i < completionSignals; ++i)
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
        }

        auto finishCall = [=](const R5900Context *guestResult, R5900Context &parent)
        {
            bool handled = iopResult.handled;
            uint32_t resultPointer = iopResult.resultAddress;
            bool copiedFallback = false;
            bool zeroedFallback = false;
            if (guestResult)
            {
                handled = true;
                resultPointer = getRegU32(guestResult, 2);
                if (resultPointer == 0u)
                {
                    resultPointer = guestDefaultResult;
                }
            }

            if (receiveBuffer != 0u && receiveSize != 0u)
            {
                if (handled && resultPointer != 0u && resultPointer != receiveBuffer)
                {
                    rpcCopyToRdram(rdram, receiveBuffer, resultPointer, receiveSize);
                }
                else if (!handled && sendBuf != 0u && sendSize != 0u && sendBuf != receiveBuffer)
                {
                    rpcCopyToRdram(rdram, receiveBuffer, sendBuf, std::min(sendSize, receiveSize));
                    copiedFallback = true;
                }
                else if (!handled)
                {
                    rpcZeroRdram(rdram, receiveBuffer, receiveSize);
                    zeroedFallback = true;
                }
            }

            auto completeClient = [=](R5900Context &base, bool callbackCompleted)
            {
                {
                    std::lock_guard<std::mutex> lock(g_rpc_mutex);
                    auto &state = g_rpc_clients[clientPtr];
                    if (state.callSequence == callSequence)
                        state.busy = false;
                }
                SifRpcDebugEvent event = makeRpcDebugEvent("CallRpc", &base, runtime);
                event.clientPtr = clientPtr;
                event.serverPtr = serverPtr;
                event.sid = sid;
                event.rpcNum = rpcNum;
                event.mode = mode;
                event.sendBuf = sendBuf;
                event.sendSize = sendSize;
                event.recvBuf = receiveBuffer;
                event.recvSize = receiveSize;
                event.resultPtr = resultPointer;
                event.endFunc = endFunction;
                event.endParam = endParameter;
                event.semaId = completionSemaphore;
                event.flags =
                    ((mode & kSifRpcModeNowait) ? kSifRpcDebugFlagNowait : 0u) |
                    (iopResult.handled ? kSifRpcDebugFlagHandledByHle : 0u) |
                    (callbackCompleted ? kSifRpcDebugFlagCallback : 0u) |
                    (serverDispatched ? kSifRpcDebugFlagServerDispatch : 0u) |
                    (!handled ? kSifRpcDebugFlagUnhandled : 0u) |
                    (copiedFallback ? kSifRpcDebugFlagFallbackCopy : 0u) |
                    (zeroedFallback ? kSifRpcDebugFlagFallbackZero : 0u);
                fillRpcDebugPreview(rdram, sendBuf, sendSize, event.sendPreview, event.sendPreviewSize);
                fillRpcDebugPreview(rdram, receiveBuffer, receiveSize, event.recvPreview, event.recvPreviewSize);
                event.result = 0;
#if PS2X_ENABLE_IOP_RPC_TRACE
                if ((event.flags & kSifRpcDebugFlagUnhandled) != 0u)
                {
                    logUnhandledRpcTrace(event);
                }
#endif
                pushSifRpcDebugEvent(event);
            };

            setReturnS32(&parent, 0);
            // PS2X_SIF_RPC_LOG=1: one stderr line per sceSifCallRpc completion (sid, function,
            // mode, end function and how it was handled), up to 4000 lines.
            static const bool s_rpcLog = [] {
                const char *value = std::getenv("PS2X_SIF_RPC_LOG");
                return value && *value && *value != '0';
            }();
            static int s_rpcLogLines = 0;
            auto rpcLog = [&](const char *what)
            {
                if (!s_rpcLog || s_rpcLogLines >= 4000)
                    return;
                ++s_rpcLogLines;
                std::fprintf(stderr, "[sif:rpc] sid=0x%X fn=%u mode=%u end=0x%X param=0x%X handled=%d: %s\n",
                             sid, rpcNum, mode, endFunction, endParameter, handled ? 1 : 0, what);
            };
            if (endFunction == 0u || iopResult.callbackPolicy == ps2x::iop::CallbackPolicy::Suppress)
            {
                rpcLog(endFunction == 0u ? "no end function" : "end function suppressed");
                completeClient(parent, endFunction != 0u);
                return;
            }

            uint32_t callbackFunction = endFunction;
            if (!runtime->hasFunction(callbackFunction) && callbackFunction >= 0x10000u &&
                runtime->hasFunction(callbackFunction - 0x10000u))
            {
                callbackFunction -= 0x10000u;
            }
            if (!runtime->hasFunction(callbackFunction))
            {
                rpcLog("end function unknown: skipped");
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
                completeClient(parent, false);
                return;
            }
            rpcLog("end function invoked");

            GuestInvocation callback{};
            callback.kind = GuestInvocationKind::RpcCallback;
            callback.context = parent;
            callback.context.pc = callbackFunction;
            SET_GPR_U32(&callback.context, 4, endParameter);
            SET_GPR_U32(&callback.context, 29, runtime->eeScheduler().invocationStackTop());
            SET_GPR_U32(&callback.context, 31, 0u);
            callback.onComplete = [completeClient](const R5900Context &, R5900Context &base)
            {
                completeClient(base, true);
            };
            runtime->eeScheduler().invokeCurrent(std::move(callback));
        };

        // A NOWAIT call served by the IOP completes later on the hardware: the IOP runs the
        // server, its RPC END packet raises a SIF interrupt on the EE, and that handler marks
        // the client done and runs the end function. Completing inside this call would let
        // the caller see the reply before sceSifCallRpc returns, so code that compares a reply
        // counter against the value it saved after the call (SRWZ's sound driver, which echoes
        // the IOP's "stream prepared" id back as the start command) never sees a change. The
        // reply is copied now and the client stays busy; the end function (or a bare
        // completion) runs as a queued invocation, which the dispatcher takes only after the
        // calling guest code has returned to it. On the hardware the END packet does all of
        // the completion at once: end function, completion semaphore, client idle; so the
        // semaphore signals wait for the completion too. (The reply words themselves are
        // already in the receive buffer: the IOP bridge writes them during the call.)
        if (deferredNowait)
        {
            const bool handled = iopResult.handled;
            const uint32_t resultPointer = iopResult.resultAddress;
            bool copiedFallback = false;
            bool zeroedFallback = false;
            if (receiveBuffer != 0u && receiveSize != 0u)
            {
                if (handled && resultPointer != 0u && resultPointer != receiveBuffer)
                {
                    rpcCopyToRdram(rdram, receiveBuffer, resultPointer, receiveSize);
                }
                else if (!handled && sendBuf != 0u && sendSize != 0u && sendBuf != receiveBuffer)
                {
                    rpcCopyToRdram(rdram, receiveBuffer, sendBuf, std::min(sendSize, receiveSize));
                    copiedFallback = true;
                }
                else if (!handled)
                {
                    rpcZeroRdram(rdram, receiveBuffer, receiveSize);
                    zeroedFallback = true;
                }
            }
            uint32_t callbackFunction = endFunction;
            if (callbackFunction != 0u && !runtime->hasFunction(callbackFunction) && callbackFunction >= 0x10000u &&
                runtime->hasFunction(callbackFunction - 0x10000u))
            {
                callbackFunction -= 0x10000u;
            }
            const bool runCallback = callbackFunction != 0u &&
                                     iopResult.callbackPolicy != ps2x::iop::CallbackPolicy::Suppress &&
                                     runtime->hasFunction(callbackFunction);
            if (endFunction != 0u && !runCallback)
                ++completionSignals;
            static const bool s_deferLog = [] {
                const char *value = std::getenv("PS2X_SIF_RPC_LOG");
                return value && *value && *value != '0';
            }();
            static int s_deferLogLines = 0;
            if (s_deferLog && s_deferLogLines < 4000)
            {
                ++s_deferLogLines;
                std::fprintf(stderr, "[sif:rpc] sid=0x%X fn=%u mode=%u end=0x%X param=0x%X handled=%d: deferred (%s)\n",
                             sid, rpcNum, mode, endFunction, endParameter, handled ? 1 : 0,
                             runCallback ? "end function queued" : "completion queued");
            }

            GuestInvocation deferred{};
            deferred.kind = GuestInvocationKind::RpcCallback;
            deferred.context = *ctx;
            deferred.context.pc = runCallback ? callbackFunction : 0u;
            SET_GPR_U32(&deferred.context, 4, endParameter);
            SET_GPR_U32(&deferred.context, 29, 0u);
            SET_GPR_U32(&deferred.context, 31, 0u);
            const uint32_t eventFlags =
                kSifRpcDebugFlagNowait |
                (handled ? kSifRpcDebugFlagHandledByHle : 0u) |
                (runCallback ? kSifRpcDebugFlagCallback : 0u) |
                (!handled ? kSifRpcDebugFlagUnhandled : 0u) |
                (copiedFallback ? kSifRpcDebugFlagFallbackCopy : 0u) |
                (zeroedFallback ? kSifRpcDebugFlagFallbackZero : 0u);
            deferred.completeTag = {GuestInvocationCompleteKind::RpcClientComplete,
                                    static_cast<uint64_t>(clientPtr) | (static_cast<uint64_t>(callSequence) << 32u),
                                    static_cast<uint64_t>(completionSemaphore) | (static_cast<uint64_t>(completionSignals) << 32u)};
            // The same closure a snapshot rebuilds from the tag, plus the debug event.
            auto complete = ps2_stubs::snapshot::makeRpcClientComplete(*runtime, deferred.completeTag);
            deferred.onComplete = [=](const R5900Context &completed, R5900Context &base)
            {
                complete(completed, base);
                SifRpcDebugEvent event = makeRpcDebugEvent("CallRpc", &base, runtime);
                event.clientPtr = clientPtr;
                event.serverPtr = serverPtr;
                event.sid = sid;
                event.rpcNum = rpcNum;
                event.mode = mode;
                event.sendBuf = sendBuf;
                event.sendSize = sendSize;
                event.recvBuf = receiveBuffer;
                event.recvSize = receiveSize;
                event.resultPtr = resultPointer;
                event.endFunc = endFunction;
                event.endParam = endParameter;
                event.semaId = completionSemaphore;
                event.flags = eventFlags;
                fillRpcDebugPreview(rdram, sendBuf, sendSize, event.sendPreview, event.sendPreviewSize);
                fillRpcDebugPreview(rdram, receiveBuffer, receiveSize, event.recvPreview, event.recvPreviewSize);
                event.result = 0;
                pushSifRpcDebugEvent(event);
            };
            setReturnS32(ctx, 0);
            // The IOP's turnaround (SIF DMA of the packet, the server's work, the END packet
            // back): a fraction of a millisecond. The dispatcher would otherwise run a plain
            // queued invocation as soon as this stub returns, still inside the caller.
            runtime->eeScheduler().queueInvocationAfter(std::move(deferred), kSifRpcNowaitCompletionCycles);
            return;
        }

        if (serverDispatched)
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::RpcCallback;
            invocation.context = *ctx;
            invocation.context.pc = guestFunction;
            SET_GPR_U32(&invocation.context, 4, guestA0);
            SET_GPR_U32(&invocation.context, 5, guestA1);
            SET_GPR_U32(&invocation.context, 6, guestA2);
            SET_GPR_U32(&invocation.context, 7, guestA3);
            SET_GPR_U32(&invocation.context, 29, runtime->eeScheduler().invocationStackTop());
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [finishCall](const R5900Context &completed, R5900Context &parent)
            {
                finishCall(&completed, parent);
            };
            runtime->eeScheduler().invokeCurrent(std::move(invocation));
        }
        finishCall(nullptr, *ctx);
    }

    // sceSifCallRpc(sceSifClientData *cd, int fno, u_int mode, void *send, int ssize, void *recv,
    //               int rsize, sceSifEndFunc end_func, void *end_para): EE ABI, as the game's own
    // copy (0x11f130) reads them: arguments 5-8 in t0-t3, the 9th (end_para) in the first 8-byte
    // stack slot at sp+0. Nothing is read from sp+0x10..: those are the caller's locals.
    PS2_STUB_TYPED(ps2_abi::SelfReturn<int32_t>, SifCallRpc, ps2_abi::gptr cd, uint32_t fno, uint32_t mode,
                   ps2_abi::gptr send, uint32_t ssize, ps2_abi::gptr recv, uint32_t rsize,
                   ps2_abi::gptr endFunc, uint32_t endPara)
    {
        sifCallRpcBody(rdram, ctx, runtime, cd, fno, mode, send, ssize, recv, rsize, endFunc, endPara);
        return {};
    }

    void SifCallRpc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t clientPtr = getRegU32(ctx, 4);
        const uint32_t rpcNum = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);
        const uint32_t sendBuf = getRegU32(ctx, 7);
        const uint32_t stackPointer = getRegU32(ctx, 29);

        // EE ABI, as the game's own copy (0x11f130) reads them: arguments 5-8 in t0-t3, the 9th
        // (end_param) in the first 8-byte stack slot at sp+0. Nothing is read from sp+0x10..: those
        // are the caller's locals.
        const uint32_t sendSize = getRegU32(ctx, 8);
        const uint32_t receiveBuffer = getRegU32(ctx, 9);
        const uint32_t receiveSize = getRegU32(ctx, 10);
        const uint32_t endFunction = getRegU32(ctx, 11);
        uint32_t endParameter = 0u;
        (void)readStackU32(rdram, stackPointer, 0x0u, endParameter);

        sifCallRpcBody(rdram, ctx, runtime, clientPtr, rpcNum, mode, sendBuf, sendSize, receiveBuffer, receiveSize,
                       endFunction, endParameter);
    }

    PS2_STUB_DISPATCH(SifCallRpc)

    // The body of SifRegisterRpc, shared by the typed and the legacy decoding; returns v0.
    static int32_t sifRegisterRpcBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t sdPtr,
                                      uint32_t sid, uint32_t func, uint32_t buf, uint32_t cfunc, uint32_t cbuf,
                                      uint32_t qd)
    {
        t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
        if (!sd)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("RegisterRpc", ctx, runtime);
            event.serverPtr = sdPtr;
            event.sid = sid;
            event.sendBuf = buf;
            event.recvBuf = cbuf;
            event.resultPtr = qd;
            event.endFunc = cfunc;
            event.flags = kSifRpcDebugFlagMissingClient;
            event.result = -1;
            pushSifRpcDebugEvent(event);
            return -1;
        }

        sd->sid = static_cast<int>(sid);
        sd->func = func;
        sd->buf = buf;
        sd->size = 0;
        sd->cfunc = cfunc;
        sd->cbuf = cbuf;
        sd->size2 = 0;
        sd->client = 0;
        sd->pkt_addr = 0;
        sd->rpc_number = 0;
        sd->recvbuf = 0;
        sd->rsize = 0;
        sd->rmode = 0;
        sd->rid = 0;
        sd->base = qd;
        sd->link = 0;
        sd->next = 0;

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);

            if (qd)
            {
                t_SifRpcDataQueue *queue = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qd));
                if (queue)
                {
                    if (!queue->link)
                    {
                        queue->link = sdPtr;
                    }
                    else
                    {
                        uint32_t curPtr = queue->link;
                        for (int guard = 0; guard < 1024 && curPtr; ++guard)
                        {
                            t_SifRpcServerData *cur = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, curPtr));
                            if (!cur)
                                break;
                            if (!cur->link)
                            {
                                cur->link = sdPtr;
                                break;
                            }
                            if (cur->link == sdPtr)
                                break;
                            curPtr = cur->link;
                        }
                    }
                }
            }

            g_rpc_servers[sid] = {sid, sdPtr};
            for (auto &entry : g_rpc_clients)
            {
                if (entry.second.sid == sid)
                {
                    t_SifRpcClientData *cd = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, entry.first));
                    if (cd)
                    {
                        cd->server = sdPtr;
                        cd->buf = sd->buf;
                        cd->cbuf = sd->cbuf;
                    }
                }
            }
        }

        RUNTIME_LOG("[SifRegisterRpc] sid=0x" << std::hex << sid << " sd=0x" << sdPtr << std::dec);
        SifRpcDebugEvent event = makeRpcDebugEvent("RegisterRpc", ctx, runtime);
        event.serverPtr = sdPtr;
        event.sid = sid;
        event.sendBuf = buf;
        event.recvBuf = cbuf;
        event.resultPtr = qd;
        event.endFunc = cfunc;
        event.result = 0;
        pushSifRpcDebugEvent(event);
        return 0;
    }

    // sceSifRegisterRpc(sceSifServeData *sd, int sid, sceSifRpcFunc func, void *buf,
    //                   sceSifRpcFunc cfunc, void *cbuf, sceSifQueueData *qd): arguments 5-7
    // (cfunc, cbuf, qd) are in t0-t2: the game's copy (0x11f408) stores t0/t1 in the server data
    // and links the server into the queue t2 points at.
    PS2_STUB_TYPED(int32_t, SifRegisterRpc, ps2_abi::gptr sd, uint32_t sid, ps2_abi::gptr func, ps2_abi::gptr buf,
                   ps2_abi::gptr cfunc, ps2_abi::gptr cbuf, ps2_abi::gptr qd)
    {
        return sifRegisterRpcBody(rdram, ctx, runtime, sd, sid, func, buf, cfunc, cbuf, qd);
    }

    void SifRegisterRpc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sdPtr = getRegU32(ctx, 4);
        uint32_t sid = getRegU32(ctx, 5);
        uint32_t func = getRegU32(ctx, 6);
        uint32_t buf = getRegU32(ctx, 7);
        // Arguments 5-7 (cfunc, cbuf, qd) are in t0-t2: the game's copy (0x11f408) stores t0/t1
        // in the server data and links the server into the queue t2 points at.
        uint32_t cfunc = getRegU32(ctx, 8);
        uint32_t cbuf = getRegU32(ctx, 9);
        uint32_t qd = getRegU32(ctx, 10);

        setReturnS32(ctx, sifRegisterRpcBody(rdram, ctx, runtime, sdPtr, sid, func, buf, cfunc, cbuf, qd));
    }

    PS2_STUB_DISPATCH(SifRegisterRpc)

    // int sceSifCheckStatRpc(sceSifRpcData *cd)
    PS2_STUB_TYPED(int32_t, SifCheckStatRpc, ps2_abi::gptr clientPtr)
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        auto it = g_rpc_clients.find(clientPtr);
        if (it == g_rpc_clients.end())
        {
            return 0;
        }
        return it->second.busy ? 1 : 0;
    }

    void SifCheckStatRpc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clientPtr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        auto it = g_rpc_clients.find(clientPtr);
        if (it == g_rpc_clients.end())
        {
            setReturnS32(ctx, 0);
            return;
        }
        setReturnS32(ctx, it->second.busy ? 1 : 0);
    }

    PS2_STUB_DISPATCH(SifCheckStatRpc)

    // The body of sceSifSetRpcQueue after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sifSetRpcQueueBody(uint8_t *rdram, uint32_t qdPtr, int threadId)
    {
        t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
        if (!qd)
        {
            return -1;
        }

        qd->thread_id = threadId;
        qd->active = 0;
        qd->link = 0;
        qd->start = 0;
        qd->end = 0;
        qd->next = 0;

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            if (!g_rpc_active_queue)
            {
                g_rpc_active_queue = qdPtr;
            }
            else
            {
                uint32_t curPtr = g_rpc_active_queue;
                for (int guard = 0; guard < 1024 && curPtr; ++guard)
                {
                    if (curPtr == qdPtr)
                        break;
                    t_SifRpcDataQueue *cur = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, curPtr));
                    if (!cur)
                        break;
                    if (!cur->next)
                    {
                        cur->next = qdPtr;
                        break;
                    }
                    curPtr = cur->next;
                }
            }
        }

        return 0;
    }

    // void sceSifSetRpcQueue(sceSifQueueData *qd, int thread_id)
    PS2_STUB_TYPED(int32_t, SifSetRpcQueue, ps2_abi::gptr qd, int32_t threadId)
    {
        (void)ctx;
        (void)runtime;
        return sifSetRpcQueueBody(rdram, qd, threadId);
    }

    void SifSetRpcQueue_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t qdPtr = getRegU32(ctx, 4);
        int threadId = static_cast<int>(getRegU32(ctx, 5));

        setReturnS32(ctx, sifSetRpcQueueBody(rdram, qdPtr, threadId));
    }

    PS2_STUB_DISPATCH(SifSetRpcQueue)
    PS2_STUB_DEVIATION(SifSetRpcQueue, "SDK: void sceSifSetRpcQueue(sceSifQueueData *qd, int thread_id); the handler writes 0 (or -1 for a bad queue pointer) to v0 (kept)")

    // The body of sceSifRemoveRpcQueue after decoding, shared by the typed and the legacy form; returns v0.
    static uint32_t sifRemoveRpcQueueBody(uint8_t *rdram, uint32_t qdPtr)
    {
        if (!qdPtr)
        {
            return 0;
        }

        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (!g_rpc_active_queue)
        {
            return 0;
        }

        if (g_rpc_active_queue == qdPtr)
        {
            t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
            g_rpc_active_queue = qd ? qd->next : 0;
            return qdPtr;
        }

        uint32_t curPtr = g_rpc_active_queue;
        for (int guard = 0; guard < 1024 && curPtr; ++guard)
        {
            t_SifRpcDataQueue *cur = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, curPtr));
            if (!cur)
                break;
            if (cur->next == qdPtr)
            {
                t_SifRpcDataQueue *rem = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
                cur->next = rem ? rem->next : 0;
                return qdPtr;
            }
            curPtr = cur->next;
        }

        return 0;
    }

    // sceSifQueueData *sceSifRemoveRpcQueue(sceSifQueueData *qd)
    PS2_STUB_TYPED(ps2_abi::gptr, SifRemoveRpcQueue, ps2_abi::gptr qd)
    {
        (void)ctx;
        (void)runtime;
        return sifRemoveRpcQueueBody(rdram, qd);
    }

    void SifRemoveRpcQueue_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t qdPtr = getRegU32(ctx, 4);

        setReturnU32(ctx, sifRemoveRpcQueueBody(rdram, qdPtr));
    }

    PS2_STUB_DISPATCH(SifRemoveRpcQueue)

    // The body of sceSifRemoveRpc after decoding, shared by the typed and the legacy form; returns v0.
    static uint32_t sifRemoveRpcBody(uint8_t *rdram, uint32_t sdPtr, uint32_t qdPtr)
    {
        t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
        if (!qd || !sdPtr)
        {
            return 0;
        }

        std::lock_guard<std::mutex> lock(g_rpc_mutex);

        if (qd->link == sdPtr)
        {
            t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
            qd->link = sd ? sd->link : 0;
            if (sd)
                sd->link = 0;
            return sdPtr;
        }

        uint32_t curPtr = qd->link;
        for (int guard = 0; guard < 1024 && curPtr; ++guard)
        {
            t_SifRpcServerData *cur = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, curPtr));
            if (!cur)
                break;
            if (cur->link == sdPtr)
            {
                t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
                cur->link = sd ? sd->link : 0;
                if (sd)
                    sd->link = 0;
                return sdPtr;
            }
            curPtr = cur->link;
        }

        return 0;
    }

    // sceSifServeData *sceSifRemoveRpc(sceSifServeData *sd, sceSifQueueData *qd)
    PS2_STUB_TYPED(ps2_abi::gptr, SifRemoveRpc, ps2_abi::gptr sd, ps2_abi::gptr qd)
    {
        (void)ctx;
        (void)runtime;
        return sifRemoveRpcBody(rdram, sd, qd);
    }

    void SifRemoveRpc_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sdPtr = getRegU32(ctx, 4);
        uint32_t qdPtr = getRegU32(ctx, 5);

        setReturnU32(ctx, sifRemoveRpcBody(rdram, sdPtr, qdPtr));
    }

    PS2_STUB_DISPATCH(SifRemoveRpc)

    PS2_STUB_ALIAS(sceSifCallRpc, SifCallRpc, ::ps2_abi::Kind::Syscall)

    // sceSifSendCmd(u_int cid, void *packet, int packet_size, void *src_extra, void *dest_extra,
    //               int size_extra): the game's copy (0x11e530) passes t0/t1 on as dest/size, so
    // they are arguments 5-6 in t0/t1.
    PS2_STUB_TYPED(int32_t, sceSifSendCmd, uint32_t cid, ps2_abi::gptr packetAddr, uint32_t packetSize,
                   ps2_abi::gptr srcExtra, ps2_abi::gptr destExtra, uint32_t sizeExtra)
    {
        if (sizeExtra > 0 && srcExtra && destExtra)
        {
            rpcCopyToRdram(rdram, destExtra, srcExtra, sizeExtra);
        }

        static int logCount = 0;
        if (logCount < 5)
        {
            RUNTIME_LOG("[sceSifSendCmd] cid=0x" << std::hex << cid
                                                 << " packet=0x" << static_cast<uint32_t>(packetAddr)
                                                 << " psize=0x" << packetSize
                                                 << " extra=0x" << static_cast<uint32_t>(destExtra) << std::dec << std::endl);
            ++logCount;
        }

        // Return non-zero on success.
        return 1;
    }

    void sceSifSendCmd_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t cid = getRegU32(ctx, 4);
        uint32_t packetAddr = getRegU32(ctx, 5);
        uint32_t packetSize = getRegU32(ctx, 6);
        uint32_t srcExtra = getRegU32(ctx, 7);

        // sceSifSendCmd(cid, packet, packet_size, src_extra, dest_extra, size_extra): the game's
        // copy (0x11e530) passes t0/t1 on as dest/size, so they are arguments 5-6 in t0/t1.
        uint32_t destExtra = getRegU32(ctx, 8);
        uint32_t sizeExtra = getRegU32(ctx, 9);

        if (sizeExtra > 0 && srcExtra && destExtra)
        {
            rpcCopyToRdram(rdram, destExtra, srcExtra, sizeExtra);
        }

        static int logCount = 0;
        if (logCount < 5)
        {
            RUNTIME_LOG("[sceSifSendCmd] cid=0x" << std::hex << cid
                                                 << " packet=0x" << packetAddr
                                                 << " psize=0x" << packetSize
                                                 << " extra=0x" << destExtra << std::dec << std::endl);
            ++logCount;
        }

        // Return non-zero on success.
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceSifSendCmd)

    // libsifrpc internal: void *_rpc_get_packet(struct rpc_data *rpc_data); returns its argument here.
    PS2_STUB_TYPED(ps2_abi::gptr, sceRpcGetPacket, ps2_abi::gptr queuePtr)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        return queuePtr;
    }

    void sceRpcGetPacket_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t queuePtr = getRegU32(ctx, 4);
        setReturnS32(ctx, static_cast<int32_t>(queuePtr));
    }

    PS2_STUB_DISPATCH(sceRpcGetPacket)
}
