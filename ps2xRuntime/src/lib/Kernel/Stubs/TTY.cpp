#include "Common.h"
#include "TTY.h"
#include "ps2_log.h"

namespace ps2_stubs
{
    // void scePrintf(const char *format, ...). Declared void: the runtime never wrote v0 here
    // (the SDK returns int); kept as it was, see NOTES-typed.md, behaviour differences.
    PS2_STUB_TYPED(void, scePrintf, ps2_abi::gptr format_addr, ps2_abi::VarArgs args)
    {
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        if (format_addr == 0)
            return;
        std::string rendered = formatPs2StringWithVarArgs(rdram, runtime, formatOwned.c_str(), args);
        if (rendered.size() > 2048)
            rendered.resize(2048);
        PS2_IF_AGRESSIVE_LOGS({
            const std::string logLine = sanitizeForLog(rendered);
            uint32_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_printfLogMutex);
                count = ++g_printfLogCount;
            }
            if (count <= kMaxPrintfLogs)
            {
                RUNTIME_LOG("PS2 scePrintf: " << logLine);
                RUNTIME_LOG(std::flush);
            }
            else if (count == kMaxPrintfLogs + 1)
            {
                std::cerr << "PS2 printf logging suppressed after " << kMaxPrintfLogs << " lines" << std::endl;
            }
        });
    }

    void scePrintf_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t format_addr = getRegU32(ctx, 4);
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        if (format_addr == 0)
            return;
        std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 1);
        if (rendered.size() > 2048)
            rendered.resize(2048);
        PS2_IF_AGRESSIVE_LOGS({
            const std::string logLine = sanitizeForLog(rendered);
            uint32_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_printfLogMutex);
                count = ++g_printfLogCount;
            }
            if (count <= kMaxPrintfLogs)
            {
                RUNTIME_LOG("PS2 scePrintf: " << logLine);
                RUNTIME_LOG(std::flush);
            }
            else if (count == kMaxPrintfLogs + 1)
            {
                std::cerr << "PS2 printf logging suppressed after " << kMaxPrintfLogs << " lines" << std::endl;
            }
        });
    }

    PS2_STUB_DISPATCH(scePrintf)
    PS2_STUB_DEVIATION(scePrintf, "SDK: int scePrintf(const char *fmt, ...); declared void because the old handler never writes v0 (kept)")

    void sceResetttyinit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceResetttyinit", rdram, ctx, runtime);
    }

    void sceTtyHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyHandler", rdram, ctx, runtime);
    }

    void sceTtyInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyInit", rdram, ctx, runtime);
    }

    void sceTtyRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyRead", rdram, ctx, runtime);
    }

    void sceTtyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyWrite", rdram, ctx, runtime);
    }
}

#include "ps2_snapshot_hooks.h"

namespace ps2_stubs::snapshot
{
    // No state of its own: only this file's copy of the Helpers/Support.h globals.
    void saveTty(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
    }

    void loadTty(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        (void)runtime;
        snapshotLoadSupport(r);
    }
}
