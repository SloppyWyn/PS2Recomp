#pragma once

// Where the SPU2 model's mixed output goes (docs/SPU2-MODEL.md, "Output"):
//   PS2X_SPU2_WAV=<path>  write it to a 48 kHz 16-bit stereo WAV (debugging; the header is
//                         rewritten every second, so a killed run still leaves a valid file).
//   PS2X_AUDIO_SPU2=1     play it on the host audio device through a raylib audio stream.
//                         Off by default; needs the device open, so PS2X_NO_AUDIO, PS2X_MUTE
//                         and headless runs (the run scripts' default) never play anything.
// With neither set no sink is installed and the SPU2 mixer stays off. Every path here is
// fed on the emulation thread and never blocks it: the host stream reads from a lock-free
// ring that drops what does not fit and plays silence when it runs dry.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::audio
{
    constexpr uint32_t kSpu2SampleRate = 48000u;

    // Streaming 16-bit PCM WAV writer.
    class WavWriter
    {
    public:
        WavWriter() = default;
        ~WavWriter();
        WavWriter(const WavWriter &) = delete;
        WavWriter &operator=(const WavWriter &) = delete;

        bool open(const std::string &path, uint32_t sampleRate, uint16_t channels);
        void write(const int16_t *samples, size_t frames);
        void close();
        [[nodiscard]] bool isOpen() const noexcept { return m_file != nullptr; }
        [[nodiscard]] uint64_t frames() const noexcept { return m_frames; }

    private:
        void patchHeader();

        std::FILE *m_file = nullptr;
        uint32_t m_sampleRate = 0;
        uint16_t m_channels = 0;
        uint64_t m_frames = 0;
        uint64_t m_framesAtPatch = 0;
    };

    // Single-producer / single-consumer ring of stereo frames (one atomic 32-bit word per
    // frame, so a frame is never torn). Neither side ever waits:
    //  - push() always stores the new frames; on overflow it drops the OLDEST ones by moving
    //    the read index past them (counted).
    //  - pop() fills what it lacks with silence (counted as an underrun); with `consume`
    //    != count it spreads that many source frames over the output (skip/repeat).
    //  - popRateMatched() keeps the fill near half the ring by consuming ~1% more or fewer
    //    frames than it plays: follows the drift between the emulation and the device clock.
    class StereoFrameRing
    {
    public:
        explicit StereoFrameRing(size_t capacityFrames);
        size_t push(const int16_t *frames, size_t count) noexcept;
        size_t pop(int16_t *out, size_t count, size_t consume = 0u) noexcept;
        size_t popRateMatched(int16_t *out, size_t count) noexcept;
        [[nodiscard]] size_t available() const noexcept;
        [[nodiscard]] size_t capacity() const noexcept { return m_capacity; }
        [[nodiscard]] uint64_t droppedFrames() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
        [[nodiscard]] uint64_t underrunFrames() const noexcept { return m_underrun.load(std::memory_order_relaxed); }
        [[nodiscard]] uint64_t rateAdjustments() const noexcept { return m_adjusted.load(std::memory_order_relaxed); }

    private:
        std::unique_ptr<std::atomic<uint32_t>[]> m_data;
        size_t m_capacity;
        std::atomic<uint64_t> m_adjusted{0};
        std::atomic<uint64_t> m_writeIndex{0};
        std::atomic<uint64_t> m_readIndex{0};
        std::atomic<uint64_t> m_dropped{0};
        std::atomic<uint64_t> m_underrun{0};
    };

    struct Spu2OutputSettings
    {
        std::string wavPath;   // PS2X_SPU2_WAV
        bool host = false;     // PS2X_AUDIO_SPU2
    };
    // Reads PS2X_SPU2_WAV / PS2X_AUDIO_SPU2 (null values = unset).
    Spu2OutputSettings parseSpu2OutputSettings(const char *wavEnv, const char *hostEnv);
    Spu2OutputSettings spu2OutputSettingsFromEnvironment();

    // Running level statistics of what was written (for the close summary).
    struct LevelStats
    {
        uint64_t frames = 0;
        uint64_t nonSilentFrames = 0; // |sample| > 64 on either channel
        uint64_t clippedSamples = 0;  // at -32768 or 32767
        int32_t peak = 0;
        double sumSquares = 0.0;
        void add(const int16_t *frames, size_t count) noexcept;
    };

    // The sink the runtime installs on the IOP (IopSubsystem::setAudioOutput).
    class Spu2Output
    {
    public:
        // Null when nothing is enabled. audioDeviceOpen: the host device is open (host output
        // is refused without it, with a log line).
        static std::unique_ptr<Spu2Output> create(const Spu2OutputSettings &settings, bool audioDeviceOpen);
        ~Spu2Output();

        void onFrames(const int16_t *frames, size_t count); // emulation thread
        void close();                                       // writes the WAV header, stops the stream

    private:
        Spu2Output() = default;
        bool startHostStream();
        void stopHostStream();

        WavWriter m_wav;
        LevelStats m_stats;
        std::string m_wavPath;
        std::unique_ptr<StereoFrameRing> m_ring;
        bool m_hostStarted = false;
        bool m_closed = false;
    };
}
