// PS2X_PAD_SCRIPT: parser, tick windows, and the injection into scePadRead's 32-byte
// status block (data[2..3], active-low DualShock buttons), which is what SRWZ reads:
// FUN_0018d3c0 calls scePadRead (0x1309b0, runtime-bound) with a2 = 0x59c220 and folds
// ~(data[2] << 8 | data[3]) into its button words at 0x59c24c (held) / 0x59c24e (new).
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "Stubs/Pad.h"
#include "Stubs/PadScript.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    namespace ps = ps2_stubs::pad_script;

    int g_pass = 0;
    int g_fail = 0;

    void check(bool ok, const std::string &what)
    {
        if (ok)
            ++g_pass;
        else
        {
            ++g_fail;
            std::printf("FAIL %s\n", what.c_str());
        }
    }

    constexpr uint16_t kStart = 1u << 3;
    constexpr uint16_t kUp = 1u << 4;
    constexpr uint16_t kTriangle = 1u << 12;
    constexpr uint16_t kCross = 1u << 14;
    constexpr uint16_t kSquare = 1u << 15;
    constexpr uint32_t kDataAddr = 0x59c220u; // SRWZ's own pad buffer (0x59c100 + 0x120)

    void setReg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
    }

    void testParser()
    {
        std::vector<ps::Event> ev;
        std::string err;

        check(ps::parse("11000:START+10,11300:CROSS+6", ev, &err), "parse brief example");
        check(ev.size() == 2, "two events");
        if (ev.size() == 2)
        {
            check(ev[0].tick == 11000 && ev[0].buttons == kStart && ev[0].hold == 10, "event 0 fields");
            check(ev[1].tick == 11300 && ev[1].buttons == kCross && ev[1].hold == 6, "event 1 fields");
        }

        check(ps::parse(" 5 : up | Triangle , 7:square ", ev, &err), "spaces, case, '|' combos");
        check(ev.size() == 2 && ev[0].buttons == (kUp | kTriangle) && ev[0].hold == ps::kPadScriptDefaultHold,
              "combo mask and default hold");
        check(ev.size() == 2 && ev[1].tick == 7 && ev[1].buttons == kSquare, "second entry");

        check(ps::parse("", ev, &err) && ev.empty(), "empty script parses to nothing");
        check(ps::parse("1:START,", ev, &err) && ev.size() == 1, "trailing comma tolerated");

        check(!ps::parse("START+3", ev, &err) && ev.empty(), "missing tick rejected");
        check(!ps::parse("x:START", ev, &err), "non-numeric tick rejected");
        check(!ps::parse("10:BOGUS", ev, &err) && err.find("BOGUS") != std::string::npos, "unknown button named in error");
        check(!ps::parse("10:START+0", ev, &err), "zero hold rejected");
        check(!ps::parse("10:START+", ev, &err), "empty hold rejected");
        check(!ps::parse("10:", ev, &err), "empty buttons rejected");
        check(!ps::parse("-5:START", ev, &err), "negative tick rejected");

        check(ps::buttonNames(kStart | kCross) == "START|CROSS", "buttonNames");
    }

    void testWindows()
    {
        std::vector<ps::Event> ev;
        ps::parse("100:START+3,102:CROSS+2", ev);
        check(ps::buttonsAt(ev, 99) == 0, "before first press");
        check(ps::buttonsAt(ev, 100) == kStart, "first tick of hold");
        check(ps::buttonsAt(ev, 102) == (kStart | kCross), "overlap ORs");
        check(ps::buttonsAt(ev, 103) == kCross, "START released after 3 ticks");
        check(ps::buttonsAt(ev, 104) == 0, "all released");
    }

    uint16_t readPad(std::vector<uint8_t> &rdram, R5900Context &ctx, uint32_t port = 0)
    {
        setReg(ctx, 4, port);
        setReg(ctx, 5, 0);
        setReg(ctx, 6, kDataAddr);
        ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
        const uint8_t *d = rdram.data() + kDataAddr;
        // libpad layout: data[2] = buttons 0-7, data[3] = buttons 8-15, active-low.
        return static_cast<uint16_t>(~(d[2] | (d[3] << 8)));
    }

    void testInjection()
    {
        std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
        R5900Context ctx{};

        ps2_stubs::clearPadOverrideState();
        for (uint32_t port = 0; port < 2; ++port)
        {
            setReg(ctx, 4, port);
            setReg(ctx, 5, 0);
            setReg(ctx, 6, 0x59c300u + port * 0x40u);
            ps2_stubs::scePadPortOpen(rdram.data(), &ctx, nullptr);
        }

        ps::setScriptForTesting("");
        ps::setTickForTesting(1000);
        check(readPad(rdram, ctx) == 0, "script off: no buttons");
        check(getRegU32(&ctx, 2) == 1, "scePadRead returns 1");

        ps::setScriptForTesting("1000:START+2,1001:CROSS");
        check(ps::enabled(), "script enabled");
        ps::setTickForTesting(999);
        check(readPad(rdram, ctx) == 0, "tick 999: nothing");
        ps::setTickForTesting(1000);
        check(readPad(rdram, ctx) == kStart, "tick 1000: START in data[2] bit 3");
        check(rdram[kDataAddr + 2] == 0xF7 && rdram[kDataAddr + 3] == 0xFF, "raw bytes F7 FF");
        ps::setTickForTesting(1001);
        check(readPad(rdram, ctx) == (kStart | kCross), "tick 1001: START|CROSS");
        check(rdram[kDataAddr + 3] == 0xBF, "CROSS is data[3] bit 6");
        check(readPad(rdram, ctx, 1) == 0, "port 1 untouched");
        ps::setTickForTesting(1002);
        check(readPad(rdram, ctx) == kCross, "tick 1002: CROSS only");
        ps::setTickForTesting(1001 + ps::kPadScriptDefaultHold);
        check(readPad(rdram, ctx) == 0, "released after default hold");

        // ORed into other input (the debug override stands in for a real pad here).
        ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~kSquare), 0x80, 0x80, 0x80, 0x80);
        ps::setTickForTesting(1000);
        check(readPad(rdram, ctx) == (kStart | kSquare), "script ORs into other input");
        ps2_stubs::clearPadOverrideState();

        // A game polling less often than the hold (SRWZ's title: one read per ~17 VBlanks)
        // still gets the press: the hold counts from the first read at or after the tick.
        ps::setScriptForTesting("27600:START+8");
        ps::setTickForTesting(27590);
        check(readPad(rdram, ctx) == 0, "sparse poll: before the event");
        ps::setTickForTesting(27611);
        check(readPad(rdram, ctx) == kStart, "sparse poll: first read after the window still presses");
        ps::setTickForTesting(27618);
        check(readPad(rdram, ctx) == kStart, "sparse poll: held for 8 ticks from the first read");
        ps::setTickForTesting(27619);
        check(readPad(rdram, ctx) == 0, "sparse poll: released after the hold");
        ps::setTickForTesting(27650);
        check(readPad(rdram, ctx) == 0, "sparse poll: never pressed again");

        ps::setScriptForTesting("not a script");
        check(!ps::enabled(), "bad script disables itself");
        check(readPad(rdram, ctx) == 0, "bad script injects nothing");

        ps::setScriptForTesting("");
        ps::setTickForTesting(-1);
    }
}

int main()
{
    testParser();
    testWindows();
    testInjection();
    std::printf("pad_script: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
