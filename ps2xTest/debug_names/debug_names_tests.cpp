// Guest names (ps2_guest_names.h), overlay naming (ps2_overlays.h) and the call-trace
// filter and output (ps2_call_trace.h).
#include "ps2_runtime.h"
#include "ps2_call_trace.h"
#include "ps2_guest_names.h"
#include "ps2_overlays.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
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

    void checkEq(const std::string &got, const std::string &want, const std::string &what)
    {
        check(got == want, what + " (got \"" + got + "\", want \"" + want + "\")");
    }

    void setReg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
    }

    // Main-program names, as register_functions.cpp would register them (unsorted on
    // purpose). Addresses chosen away from anything else in the test binary.
    const PS2GuestFunctionName kMainNames[] = {
        {0x01E00100u, 0x01E00180u, "sceCdRead", PS2X_GUEST_NAME_STUB},
        {0x01E00000u, 0x01E00100u, "FUN_01e00000", 0u},
        {0x01E00040u, 0x01E00100u, "entry_1e00040", PS2X_GUEST_NAME_FRAGMENT},
        {0x01E00400u, 0x01E00440u, "entry_1e00400", PS2X_GUEST_NAME_FRAGMENT}, // outside any function
        {0x01E00500u, 0u, "unsized", 0u},
    };

    void testNameTable()
    {
        ps2x_names::NameTable table;
        table.add(kMainNames, sizeof(kMainNames) / sizeof(kMainNames[0]));
        check(table.entries().size() == 5, "all entries kept");
        check(table.entries().front().start == 0x01E00000u, "sorted by start");

        const PS2GuestFunctionName *e = table.find(0x01E00000u);
        check(e && std::strcmp(e->name, "FUN_01e00000") == 0, "function start");
        e = table.find(0x01E00044u);
        check(e && std::strcmp(e->name, "FUN_01e00000") == 0, "a real function wins over a fragment inside it");
        e = table.find(0x01E000FCu);
        check(e && std::strcmp(e->name, "FUN_01e00000") == 0, "last word of a function");
        e = table.find(0x01E00100u);
        check(e && std::strcmp(e->name, "sceCdRead") == 0, "end is exclusive, next function starts");
        check(table.find(0x01E00200u) == nullptr, "gap between functions");
        e = table.find(0x01E00404u);
        check(e && std::strcmp(e->name, "entry_1e00400") == 0, "a fragment outside any function names itself");
        e = table.find(0x01E00500u);
        check(e && std::strcmp(e->name, "unsized") == 0, "unsized entry covers its start");
        check(table.find(0x01E00504u) == nullptr, "unsized entry covers only its start");
        check(table.find(0x00000000u) == nullptr, "below every entry");

        e = table.findStart(0x01E00040u);
        check(e && std::strcmp(e->name, "entry_1e00040") == 0, "findStart exact");
        check(table.findStart(0x01E00044u) == nullptr, "findStart needs an exact start");
    }

    void testFormat()
    {
        ps2xRegisterGuestFunctionNames(kMainNames, sizeof(kMainNames) / sizeof(kMainNames[0]));
        checkEq(ps2x_names::describe(0x01E00000u), "FUN_01e00000", "start has no offset");
        checkEq(ps2x_names::describe(0x01E00044u), "FUN_01e00000+0x44", "name+offset");
        checkEq(ps2x_names::describe(0x01E00110u), "sceCdRead+0x10", "stub name+offset");
        checkEq(ps2x_names::describe(0x01E00200u), "0x1e00200", "unnamed pc prints as an address");

        char small[8];
        const size_t n = ps2x_names::format(small, sizeof(small), 0x01E00044u);
        check(n == 7 && std::strlen(small) == 7, "format truncates to the buffer");

        ps2x_names::Resolved r;
        check(ps2x_names::resolve(0x01E00110u, r) && r.start == 0x01E00100u && (r.flags & PS2X_GUEST_NAME_STUB),
              "resolve gives start and flags");
    }

    // Distinct bodies: the linker folds identical functions (/OPT:ICF), and overlay
    // naming groups entries by function pointer.
    volatile uint32_t g_sink = 0;
    void ovA(uint8_t *, R5900Context *, PS2Runtime *) { g_sink = 0xA; }
    void ovB(uint8_t *, R5900Context *, PS2Runtime *) { g_sink = 0xB; }
    void ovFragment(uint8_t *, R5900Context *, PS2Runtime *) { g_sink = 0xF; }

    constexpr uint32_t kOvBase = 0x01D00000u;

    void testOverlays()
    {
        static const PS2OverlayEntry entries[] = {
            {kOvBase + 0x90u, &ovA}, // a resume point inside ovA, listed first on purpose
            {kOvBase + 0x80u, &ovA},
            {kOvBase + 0xA0u, &ovB},
            {kOvBase + 0xB0u, &ovFragment},
        };
        ps2xRegisterOverlay("tst.bin", kOvBase, kOvBase + 0x100u, entries, 4);

        std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
        check(ps2x_names::describe(kOvBase + 0x84u, ram.data()) == "0x1d00084", "no overlay loaded: bare address");

        std::memcpy(ram.data() + kOvBase, "MWo3", 4);
        std::memcpy(ram.data() + kOvBase + 0x20u, "tst.bin", 8);
        checkEq(ps2x_names::describe(kOvBase + 0x84u, ram.data()), "tst.bin:sub_01d00080+0x4",
                "overlay code named after its entry point");
        checkEq(ps2x_names::describe(kOvBase + 0x94u, ram.data()), "tst.bin:sub_01d00080+0x14",
                "a resume point belongs to its owner");
        checkEq(ps2x_names::describe(kOvBase + 0xA0u, ram.data()), "tst.bin:sub_01d000a0", "second function");
        checkEq(ps2x_names::describe(kOvBase + 0xB8u, ram.data()), "tst.bin:sub_01d000b0+0x8", "fragment");

        std::memcpy(ram.data() + kOvBase + 0x20u, "other.bin", 10);
        checkEq(ps2x_names::describe(kOvBase + 0x84u, ram.data()), "0x1d00084", "another overlay loaded: no name");

        static const PS2GuestFunctionName names[] = {
            {kOvBase + 0x80u, kOvBase + 0xA0u, "OvInit", 0u},
        };
        ps2xRegisterOverlayFunctionNames("tst.bin", names, 1);
        std::memcpy(ram.data() + kOvBase + 0x20u, "tst.bin\0\0", 10);
        checkEq(ps2x_names::describe(kOvBase + 0x94u, ram.data()), "tst.bin:OvInit+0x14", "registered overlay name");
        checkEq(ps2x_names::describe(kOvBase + 0xA4u, ram.data()), "tst.bin:sub_01d000a0+0x4",
                "unnamed overlay code falls back to its entry point");

        // Default guest memory for callers without rdram (the runtime sets it).
        ps2x_names::setGuestMemory(ram.data());
        checkEq(ps2x_names::describe(kOvBase + 0x84u), "tst.bin:OvInit+0x4", "default guest memory");
        ps2x_names::setGuestMemory(nullptr);
    }

    bool hasRange(const std::vector<std::pair<uint32_t, uint32_t>> &ranges, uint32_t a, uint32_t b)
    {
        for (const auto &r : ranges)
            if (r.first == a && r.second == b)
                return true;
        return false;
    }

    void testParseSpec()
    {
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        bool all = false;
        std::string error;
        check(ps2x_calltrace::parseSpec("*", ranges, all) && all && ranges.empty(), "* selects everything");

        ranges.clear();
        check(ps2x_calltrace::parseSpec("0x18b000-0x18c000, sceCd ,0x01e00044", ranges, all, &error) && !all,
              "mixed spec parses");
        check(hasRange(ranges, 0x18b000u, 0x18c000u), "address range");
        check(hasRange(ranges, 0x01E00100u, 0x01E00180u), "name part selects the function's range");
        check(hasRange(ranges, 0x01E00000u, 0x01E00100u), "an address selects its containing function");
        check(error.empty(), "no warnings for a good spec: " + error);

        ranges.clear();
        error.clear();
        check(ps2x_calltrace::parseSpec("NoSuchName", ranges, all, &error) && ranges.empty() &&
                  error.find("NoSuchName") != std::string::npos,
              "unknown name is reported, not fatal");
        check(!ps2x_calltrace::parseSpec("0x200-0x100", ranges, all, &error), "reversed range rejected");
        check(!ps2x_calltrace::parseSpec("0x10zz", ranges, all, &error), "bad number rejected");
    }

    void setEnv(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    std::string readFile(const std::string &path)
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void testTraceOutput()
    {
        const std::string path = "debug_names_calltrace.txt";
        setEnv("PS2X_TRACE_CALLS", "sceCd,0x01e00400-0x01e00440");
        setEnv("PS2X_TRACE_CALLS_FILE", path.c_str());
        setEnv("PS2X_TRACE_CALLS_MAX", "3");
        setEnv("PS2X_TRACE_CALLS_RET", "1");
        ps2x_calltrace::init();
        check(ps2x_calltrace::active(), "trace on after init");
        check(ps2x_calltrace::logReturns(), "returns on");
        check(ps2x_calltrace::matches(0x01E00100u) && ps2x_calltrace::matches(0x01E0017Cu), "filter: named function");
        check(!ps2x_calltrace::matches(0x01E00180u) && !ps2x_calltrace::matches(0x01E00000u), "filter: outside");
        check(ps2x_calltrace::matches(0x01E00400u), "filter: range");

        R5900Context ctx{};
        setReg(ctx, 4, 0x11u);
        setReg(ctx, 5, 0x22u);
        setReg(ctx, 31, 0x01E00048u);
        setReg(ctx, 29, 0x01FFF000u);
        check(!ps2x_calltrace::enter(&ctx, 0x01E00000u, 0x01E00040u, ps2x_calltrace::Kind::Call, 1, 7),
              "unmatched call not logged");
        check(ps2x_calltrace::enter(&ctx, 0x01E00100u, 0x01E00040u, ps2x_calltrace::Kind::Call, 1, 7),
              "matched call logged");
        setReg(ctx, 2, 0x800u);
        ps2x_calltrace::leave(&ctx, 0x01E00100u);
        // A jump is logged once: the scheduler's dispatch of the same target is skipped.
        check(ps2x_calltrace::enter(&ctx, 0x01E00400u, 0x01E00044u, ps2x_calltrace::Kind::Jump, 2, 8), "jump logged");
        ps2x_calltrace::schedulerEnter(&ctx, 0x01E00400u, 2, 8);
        ps2x_calltrace::schedulerEnter(&ctx, 0x01E00100u, 2, 9); // the cap (3 lines) stops this one
        ps2x_calltrace::flush();
        check(!ps2x_calltrace::active(), "trace off after the cap");

        const std::string text = readFile(path);
        std::printf("--- %s ---\n%s---\n", path.c_str(), text.c_str());
        check(text.find("> 7 t1 call sceCdRead a0=11 a1=22 a2=0 a3=0 ra=1e00048 (FUN_01e00000+0x48) sp=1fff000") !=
                  std::string::npos,
              "call line");
        check(text.find("< sceCdRead v0=800") != std::string::npos, "return line");
        check(text.find("> 8 t2 jump entry_1e00400 a0=11") != std::string::npos &&
                  text.find("from=1e00044 (FUN_01e00000+0x44)") != std::string::npos,
              "jump line with its source");
        check(text.find("sched") == std::string::npos, "no duplicate sched line, nothing after the cap");
        check(text.find("cap of 3 lines reached") != std::string::npos, "cap noted");
        std::remove(path.c_str());
    }
}

int main()
{
    testNameTable();
    testFormat();
    testOverlays();
    testParseSpec();
    testTraceOutput();
    std::printf("debug_names: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
