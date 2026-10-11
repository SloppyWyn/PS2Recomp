#include "runtime/ps2_deterministic.h"
#include "ps2_headless.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#if defined(PS2X_HOST_GLFW)
// GLFW is compiled into raylib (rglfw.c); its C API is reachable without its headers.
extern "C"
{
    struct GLFWmonitor;
    int glfwInit(void);
    void glfwTerminate(void);
    GLFWmonitor **glfwGetMonitors(int *count);
}
#endif

namespace ps2x::headless
{
    namespace
    {
        std::atomic<bool> g_active{false};
        std::atomic<bool> g_consoleBreak{false};
        std::string g_stopFile;
        std::once_flag g_installOnce;

#if defined(_WIN32)
        BOOL WINAPI consoleHandler(DWORD type)
        {
            if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
            {
                g_consoleBreak.store(true, std::memory_order_release);
                return TRUE; // handled: the main loop stops the run cleanly
            }
            return FALSE;
        }
#endif
    }

    bool active() noexcept
    {
        return g_active.load(std::memory_order_acquire);
    }

    void setActive(bool headless) noexcept
    {
        g_active.store(headless, std::memory_order_release);
    }

    bool envFlagOn(const char *value) noexcept
    {
        return value && *value && std::strcmp(value, "0") != 0;
    }

    bool requestedByEnv() noexcept
    {
        return envFlagOn(std::getenv("PS2X_HEADLESS"));
    }

    AudioDevice decideAudioDevice(bool headless, const char *noAudioEnv, const char *muteEnv) noexcept
    {
        if (headless)
            return AudioDevice::SkipHeadless;
        if (envFlagOn(noAudioEnv))
            return AudioDevice::SkipNoAudio;
        if (envFlagOn(muteEnv))
            return AudioDevice::SkipMute;
        return AudioDevice::Open;
    }

    AudioDevice audioDeviceDecision(bool headless) noexcept
    {
        // Deterministic mode (ps2_deterministic.h) never opens one either.
        return decideAudioDevice(headless, ps2x::det::enabled() ? "1" : std::getenv("PS2X_NO_AUDIO"), std::getenv("PS2X_MUTE"));
    }

    const char *describeAudioDevice(AudioDevice decision) noexcept
    {
        switch (decision)
        {
        case AudioDevice::SkipHeadless:
            return "[audio] no audio device (headless)";
        case AudioDevice::SkipNoAudio:
            return "[audio] no audio device (PS2X_NO_AUDIO)";
        case AudioDevice::SkipMute:
            return "[audio] no audio device (PS2X_MUTE)";
        case AudioDevice::Open:
        default:
            return "";
        }
    }

    bool hostHasMonitor() noexcept
    {
#if defined(PS2X_HOST_GLFW)
        // raylib 5.5 (rcore_desktop_glfw.c InitPlatform) gives up when GLFW finds no monitor
        // for the new window, and InitWindow then calls rlglInit without a GL context: a
        // segmentation fault. Ask GLFW the same question first. GLFW may be initialised and
        // terminated again; raylib's own glfwInit follows.
        if (!glfwInit())
            return false; // no GLFW, no window either
        int count = 0;
        GLFWmonitor **monitors = glfwGetMonitors(&count);
        glfwTerminate();
        return monitors != nullptr && count > 0;
#else
        return true;
#endif
    }

    void beginHighResTimer() noexcept
    {
#if defined(_WIN32)
        timeBeginPeriod(1);
#endif
    }

    void endHighResTimer() noexcept
    {
#if defined(_WIN32)
        timeEndPeriod(1);
#endif
    }

    uint64_t threadCpuTimeMs(void *nativeThreadHandle) noexcept
    {
#if defined(_WIN32)
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (nativeThreadHandle == nullptr ||
            !GetThreadTimes(static_cast<HANDLE>(nativeThreadHandle), &creation, &exit, &kernel, &user))
        {
            return 0u;
        }
        const auto toUnits = [](const FILETIME &t)
        {
            return (static_cast<uint64_t>(t.dwHighDateTime) << 32u) | t.dwLowDateTime; // 100 ns units
        };
        return (toUnits(kernel) + toUnits(user)) / 10000u;
#else
        (void)nativeThreadHandle;
        return 0u;
#endif
    }

    void installStopSources() noexcept
    {
        std::call_once(g_installOnce, []()
                       {
            if (const char *path = std::getenv("PS2X_STOP_FILE"); path && *path)
            {
                g_stopFile = path;
                std::error_code ec;
                if (std::filesystem::exists(g_stopFile, ec))
                {
                    // A leftover from an earlier run would stop this one at once.
                    std::filesystem::remove(g_stopFile, ec);
                }
                std::cout << "[run] stop file: " << g_stopFile << std::endl;
            }
#if defined(_WIN32)
            SetConsoleCtrlHandler(consoleHandler, TRUE);
#endif
        });
    }

    const char *pollStopRequest() noexcept
    {
        if (g_consoleBreak.load(std::memory_order_acquire))
            return "console break";
        if (g_stopFile.empty())
            return nullptr;
        static auto s_nextCheck = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now < s_nextCheck)
            return nullptr;
        s_nextCheck = now + std::chrono::milliseconds(100);
        std::error_code ec;
        return std::filesystem::exists(g_stopFile, ec) ? "stop file" : nullptr;
    }
}
