#include "MiniTest.h"
#include "runtime/ps2_fastforward.h"

// Unlimited fast-forward's busy-scale governor rule (ps2x::ff::governBusyScale).
void register_ps2_fastforward_tests()
{
    MiniTest::Case("PS2FastForward", [](TestCase &tc)
    {
        using ps2x::ff::GovernorState;
        using ps2x::ff::governBusyScale;
        using ps2x::ff::kBusyVBlanksBeforeBackoff;

        tc.Run("unset scale starts at the configured maximum", [](TestCase &t)
        {
            const GovernorState s = governBusyScale({}, true, 8.0);
            t.IsTrue(s.scale == 8.0, "idle VBlank keeps the max");
            t.Equals(s.busyVBlanks, 0u, "busy count cleared");
        });

        tc.Run("halves after the busy run, not before, never below 1", [](TestCase &t)
        {
            GovernorState s{8.0, 0u};
            for (uint32_t i = 1; i < kBusyVBlanksBeforeBackoff; ++i)
            {
                s = governBusyScale(s, false, 8.0);
                t.IsTrue(s.scale == 8.0, "no change before the busy run completes");
            }
            s = governBusyScale(s, false, 8.0);
            t.IsTrue(s.scale == 4.0, "halved at the busy run");
            t.Equals(s.busyVBlanks, 0u, "busy count restarts");
            for (int k = 0; k < 10; ++k)
                for (uint32_t i = 0; i < kBusyVBlanksBeforeBackoff; ++i)
                    s = governBusyScale(s, false, 8.0);
            t.IsTrue(s.scale == 1.0, "floor at 1 (real-time charging)");
        });

        tc.Run("idle doubles back up to the max and resets the busy count", [](TestCase &t)
        {
            GovernorState s{1.0, 5u};
            s = governBusyScale(s, true, 8.0);
            t.IsTrue(s.scale == 2.0, "doubled");
            t.Equals(s.busyVBlanks, 0u, "busy count reset by idle");
            s = governBusyScale(s, true, 8.0);
            s = governBusyScale(s, true, 8.0);
            s = governBusyScale(s, true, 8.0);
            t.IsTrue(s.scale == 8.0, "capped at the max");
            GovernorState odd = governBusyScale({5.0, 0u}, true, 6.0);
            t.IsTrue(odd.scale == 6.0, "cap applies to non-power-of-two maxima");
        });

        tc.Run("a frame-paced guest (idle every other VBlank) keeps the full scale", [](TestCase &t)
        {
            GovernorState s{};
            for (int i = 0; i < 1000; ++i)
                s = governBusyScale(s, (i & 1) == 0, 8.0);
            t.IsTrue(s.scale == 8.0, "never backs off");
        });
    });
}
