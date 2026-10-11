// Call trace for the generated stub wrappers (the guest functions bound to runtime
// implementations: SDK calls such as sceOpen, sceCdRead, sceSifLoadModule).
//
// PS2X_TRACE_STUBS=1            trace every stub call
// PS2X_TRACE_STUBS=Cd,Open,Sif  only stubs whose guest name contains one of these
//
// Each call prints its arguments (a0..a3, ra with the caller's guest name; a0 also as a
// string when it points at one) and, on return, v0. Written to stderr, which is
// unbuffered, so nothing is lost when the process is killed.
#include "ps2_runtime.h"
#include "ps2_crash.h"
#include "ps2_guest_names.h"
#include "ps2_watchdog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    struct StubTraceConfig
    {
        bool enabled = false;
        std::vector<std::string> filters;
    };

    const StubTraceConfig &stubTraceConfig()
    {
        static const StubTraceConfig config = []
        {
            StubTraceConfig c;
            const char *value = std::getenv("PS2X_TRACE_STUBS");
            if (!value || !*value || std::string(value) == "0")
            {
                return c;
            }
            c.enabled = true;
            const std::string spec(value);
            if (spec == "1")
            {
                return c;
            }
            size_t start = 0;
            while (start <= spec.size())
            {
                const size_t end = spec.find(',', start);
                const std::string part = spec.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!part.empty())
                {
                    c.filters.push_back(part);
                }
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return c;
        }();
        return config;
    }

    // PS2X_CRASH_TEST=<name part>[:n] (test only, ps2_crash.h): the n-th entering call of a
    // stub whose name contains the text writes through a null pointer.
    struct CrashTest
    {
        bool enabled = false;
        std::string part;
        int remaining = 1;
    };

    CrashTest &crashTest()
    {
        static CrashTest test = []
        {
            CrashTest t;
            const char *value = std::getenv("PS2X_CRASH_TEST");
            if (!value || !*value)
            {
                return t;
            }
            std::string spec(value);
            const size_t colon = spec.rfind(':');
            if (colon != std::string::npos)
            {
                t.remaining = std::max(1, std::atoi(spec.c_str() + colon + 1));
                spec.resize(colon);
            }
            t.part = spec;
            t.enabled = !t.part.empty();
            if (t.enabled)
            {
                std::fprintf(stderr, "[crash-test] armed: call %d of a stub containing \"%s\" dereferences null\n",
                             t.remaining, t.part.c_str());
            }
            return t;
        }();
        return test;
    }

    void maybeCrashForTest(const char *name)
    {
        CrashTest &test = crashTest();
        if (!test.enabled || !name || std::string(name).find(test.part) == std::string::npos)
        {
            return;
        }
        if (--test.remaining > 0)
        {
            return;
        }
        test.enabled = false;
        std::fprintf(stderr, "[crash-test] %s: writing through a null pointer now\n", name);
        ps2x_crash::testHoldLock(std::getenv("PS2X_CRASH_TEST_HOLD"));
        volatile int *null = nullptr;
        *null = 0x5032; // deliberate access violation (PS2X_CRASH_TEST)
    }

    // a0 as text when it points at a short printable string in guest RAM.
    std::string guestString(uint8_t *rdram, uint32_t address)
    {
        if (!rdram || address == 0 || (address & PS2_RAM_MASK) != address)
        {
            return {};
        }
        std::string text;
        for (uint32_t i = 0; i < 96 && address + i < PS2_RAM_SIZE; ++i)
        {
            const char ch = static_cast<char>(rdram[address + i]);
            if (ch == '\0')
            {
                return text.size() >= 2 ? text : std::string();
            }
            if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7e)
            {
                return {};
            }
            text.push_back(ch);
        }
        return {};
    }
}

void ps2xStubTrace(const char *name, uint32_t address, uint8_t *rdram, R5900Context *ctx, bool leaving)
{
    if (ctx && ps2x_watchdog::stubRingEnabled())
    {
        ps2x_watchdog::noteStub(name, address, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                                leaving ? getRegU32(ctx, 2) : getRegU32(ctx, 31), leaving);
    }
    if (!leaving && crashTest().enabled) [[unlikely]]
    {
        maybeCrashForTest(name);
    }
    const StubTraceConfig &config = stubTraceConfig();
    if (!config.enabled || !ctx)
    {
        return;
    }
    if (!config.filters.empty())
    {
        const std::string n(name ? name : "");
        bool match = false;
        for (const std::string &filter : config.filters)
        {
            if (n.find(filter) != std::string::npos)
            {
                match = true;
                break;
            }
        }
        if (!match)
        {
            return;
        }
    }

    if (leaving)
    {
        std::fprintf(stderr, "[stub] %s@0x%x -> v0=0x%x\n", name, address, getRegU32(ctx, 2));
        return;
    }
    const uint32_t a0 = getRegU32(ctx, 4);
    const std::string text = guestString(rdram, a0);
    char caller[160];
    ps2x_names::format(caller, sizeof(caller), getRegU32(ctx, 31), rdram);
    std::fprintf(stderr, "[stub] %s@0x%x(a0=0x%x%s%s%s a1=0x%x a2=0x%x a3=0x%x) ra=0x%x (%s)\n",
                 name, address, a0,
                 text.empty() ? "" : " \"", text.c_str(), text.empty() ? "" : "\"",
                 getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 31), caller);
}
