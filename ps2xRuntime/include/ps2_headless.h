#pragma once
// Headless runs and unattended stop requests (host side only, no guest-visible effect).
//
// PS2X_HEADLESS=1   never create a window, a GL context or an audio device. The main loop,
//                   the presentation latch (and with it PS2X_GS_DUMP), the pad script, the
//                   watchdog and the [run:tick] lines run exactly as with a window; the loop
//                   is paced by the host clock at the rate the window loop would run
//                   (60 Hz, or 60 x PS2X_FASTFORWARD). Window-only features (keyboard and
//                   gamepad input, the debug UI, sound output) are skipped.
//                   A desktop (GLFW) build that finds no monitor at startup switches to
//                   headless on its own and says "[run] no monitor: headless" (raylib 5.5's
//                   InitWindow crashes in rlglInit when GLFW reports no monitor).
// PS2X_STOP_FILE=<path>
//                   the main loop stops the run cleanly (guest stopped, stdout/stderr
//                   flushed, exit code 0) once that file exists. Checked ~10 times a second,
//                   windowed or headless. Ctrl-C / Ctrl-Break on the runner's console do the
//                   same.
// PS2X_NO_AUDIO=1   (or PS2X_MUTE=1, or headless) never open the host audio device: no
//                   InitAudioDevice, so no WASAPI stream and no device open/close event per
//                   run. The audio backend then drops every sound (its null sink); the guest
//                   never waits on host audio (sounds are fire-and-forget, nothing paces the
//                   guest by playback), so game behaviour is the same as with a device.
#include <cstdint>

namespace ps2x::headless
{
    // Whether this run is headless (decided once by PS2Runtime::initialize). Kept here rather
    // than in PS2Runtime so the generated game code's headers do not change.
    bool active() noexcept;
    void setActive(bool headless) noexcept;

    // PS2X_HEADLESS set to anything but "" or "0".
    bool requestedByEnv() noexcept;

    // An environment switch is on: set to anything but "" or "0".
    bool envFlagOn(const char *value) noexcept;

    // Whether to open the host audio device, and why not. Pure (values passed in) so it can
    // be tested; audioDeviceDecision() reads PS2X_NO_AUDIO and PS2X_MUTE.
    enum class AudioDevice
    {
        Open,
        SkipHeadless, // headless run (PS2X_HEADLESS or no monitor)
        SkipNoAudio,  // PS2X_NO_AUDIO
        SkipMute      // PS2X_MUTE: a muted run needs no device either
    };
    AudioDevice decideAudioDevice(bool headless, const char *noAudioEnv, const char *muteEnv) noexcept;
    AudioDevice audioDeviceDecision(bool headless) noexcept;
    // The "[audio] ..." line for a skip decision ("" for Open).
    const char *describeAudioDevice(AudioDevice decision) noexcept;

    // Desktop GLFW builds: whether GLFW reports at least one monitor (a probe that
    // initialises and terminates GLFW before raylib does). Other builds: true.
    bool hostHasMonitor() noexcept;

    // 1 ms host timer resolution for the headless loop's sleeps (Windows; raylib does the
    // same in InitPlatform). Balanced calls.
    void beginHighResTimer() noexcept;
    void endHighResTimer() noexcept;

    // CPU time (user + kernel, ms) a host thread has consumed so far, from its native handle
    // (std::thread::native_handle()); 0 where the host has no such query. The [run:tick]
    // line prints it for the EE thread (eeCpuMs): guest ms per EE CPU second is the
    // throughput measure that does not depend on the host loop's pacing.
    uint64_t threadCpuTimeMs(void *nativeThreadHandle) noexcept;

    // Installs the Ctrl-C / Ctrl-Break handler and reads PS2X_STOP_FILE. Call once.
    void installStopSources() noexcept;

    // Non-empty reason once a stop was requested (stop file seen, console break).
    // Cheap: the stop file is looked at no more than every 100 ms.
    const char *pollStopRequest() noexcept;
}
