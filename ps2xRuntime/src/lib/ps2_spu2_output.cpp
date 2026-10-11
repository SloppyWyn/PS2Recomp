#include "runtime/ps2_spu2_output.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#if !defined(PLATFORM_VITA)
#include "raylib.h"
#endif

namespace ps2x::audio
{
    namespace
    {
        void putU16(uint8_t *p, uint16_t v)
        {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8u);
        }

        void putU32(uint8_t *p, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                p[i] = static_cast<uint8_t>(v >> (8 * i));
        }

        void buildHeader(uint8_t (&h)[44], uint32_t sampleRate, uint16_t channels, uint64_t frames)
        {
            const uint64_t dataBytes64 = frames * channels * 2u;
            const uint32_t dataBytes = dataBytes64 > 0xFFFFFFD3ull ? 0xFFFFFFD3u : static_cast<uint32_t>(dataBytes64);
            std::memcpy(h, "RIFF", 4);
            putU32(h + 4, 36u + dataBytes);
            std::memcpy(h + 8, "WAVEfmt ", 8);
            putU32(h + 16, 16u);
            putU16(h + 20, 1u); // PCM
            putU16(h + 22, channels);
            putU32(h + 24, sampleRate);
            putU32(h + 28, sampleRate * channels * 2u);
            putU16(h + 32, static_cast<uint16_t>(channels * 2u));
            putU16(h + 34, 16u);
            std::memcpy(h + 36, "data", 4);
            putU32(h + 40, dataBytes);
        }

        bool flagOn(const char *value)
        {
            return value && *value && std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "off") != 0;
        }
    }

    // ---- WavWriter ----

    WavWriter::~WavWriter()
    {
        close();
    }

    bool WavWriter::open(const std::string &path, uint32_t sampleRate, uint16_t channels)
    {
        close();
        m_file = std::fopen(path.c_str(), "wb");
        if (!m_file)
            return false;
        std::setvbuf(m_file, nullptr, _IOFBF, 1u << 16u);
        m_sampleRate = sampleRate;
        m_channels = channels;
        m_frames = 0u;
        m_framesAtPatch = 0u;
        uint8_t header[44];
        buildHeader(header, m_sampleRate, m_channels, 0u);
        std::fwrite(header, 1, sizeof(header), m_file);
        return true;
    }

    void WavWriter::write(const int16_t *samples, size_t frames)
    {
        if (!m_file || frames == 0u)
            return;
        // WAV data is little-endian, as are all hosts this runs on.
        std::fwrite(samples, sizeof(int16_t) * m_channels, frames, m_file);
        m_frames += frames;
        if (m_frames - m_framesAtPatch >= m_sampleRate)
            patchHeader(); // about once a second of audio: a killed run keeps a valid file
    }

    void WavWriter::patchHeader()
    {
        if (!m_file)
            return;
        uint8_t header[44];
        buildHeader(header, m_sampleRate, m_channels, m_frames);
        // Back to the end with SEEK_END: no long (32-bit) file positions, so files past
        // 2 GiB keep working (the RIFF sizes themselves saturate near 4 GiB).
        std::fseek(m_file, 0, SEEK_SET);
        std::fwrite(header, 1, sizeof(header), m_file);
        std::fseek(m_file, 0, SEEK_END);
        std::fflush(m_file);
        m_framesAtPatch = m_frames;
    }

    void WavWriter::close()
    {
        if (!m_file)
            return;
        patchHeader();
        std::fclose(m_file);
        m_file = nullptr;
    }

    // ---- StereoFrameRing ----

    namespace
    {
        uint32_t packFrame(int16_t l, int16_t r) noexcept
        {
            return static_cast<uint16_t>(l) | (static_cast<uint32_t>(static_cast<uint16_t>(r)) << 16u);
        }
    }

    StereoFrameRing::StereoFrameRing(size_t capacityFrames)
        : m_data(new std::atomic<uint32_t>[std::max<size_t>(capacityFrames, 2u)]), m_capacity(std::max<size_t>(capacityFrames, 2u))
    {
        for (size_t i = 0; i < m_capacity; ++i)
            m_data[i].store(0u, std::memory_order_relaxed);
    }

    size_t StereoFrameRing::available() const noexcept
    {
        const uint64_t write = m_writeIndex.load(std::memory_order_acquire);
        const uint64_t read = m_readIndex.load(std::memory_order_acquire);
        return write > read ? static_cast<size_t>(write - read) : 0u;
    }

    size_t StereoFrameRing::push(const int16_t *frames, size_t count) noexcept
    {
        if (count > m_capacity)
        {
            // More than the whole ring: only the newest capacity frames can survive.
            m_dropped.fetch_add(count - m_capacity, std::memory_order_relaxed);
            frames += (count - m_capacity) * 2u;
            count = m_capacity;
        }
        const uint64_t write = m_writeIndex.load(std::memory_order_relaxed);
        // Overflow: drop the oldest frames by moving the read index past them (the consumer
        // advances it with a compare-exchange, so neither side ever waits).
        uint64_t read = m_readIndex.load(std::memory_order_acquire);
        while (write + count - read > m_capacity)
        {
            const uint64_t wanted = write + count - m_capacity;
            if (m_readIndex.compare_exchange_weak(read, wanted, std::memory_order_acq_rel))
            {
                m_dropped.fetch_add(wanted - read, std::memory_order_relaxed);
                break;
            }
        }
        for (size_t i = 0; i < count; ++i)
            m_data[static_cast<size_t>((write + i) % m_capacity)].store(packFrame(frames[i * 2u], frames[i * 2u + 1u]), std::memory_order_relaxed);
        m_writeIndex.store(write + count, std::memory_order_release);
        return count;
    }

    size_t StereoFrameRing::pop(int16_t *out, size_t count, size_t consume) noexcept
    {
        // `consume` source frames are spread over `count` output frames (nearest neighbour:
        // a few skipped or repeated frames), the rate matching's small resample.
        if (consume == 0u)
            consume = count;
        uint64_t read = m_readIndex.load(std::memory_order_acquire);
        const uint64_t write = m_writeIndex.load(std::memory_order_acquire);
        const size_t have = write > read ? static_cast<size_t>(write - read) : 0u;
        if (have < consume)
        {
            // Not enough: play what there is 1:1, then silence.
            const size_t n = std::min(have, count);
            for (size_t i = 0; i < n; ++i)
            {
                const uint32_t f = m_data[static_cast<size_t>((read + i) % m_capacity)].load(std::memory_order_relaxed);
                out[i * 2u] = static_cast<int16_t>(f & 0xFFFFu);
                out[i * 2u + 1u] = static_cast<int16_t>(f >> 16u);
            }
            std::memset(out + n * 2u, 0, (count - n) * 2u * sizeof(int16_t));
            m_underrun.fetch_add(count - n, std::memory_order_relaxed);
            m_readIndex.compare_exchange_strong(read, read + n, std::memory_order_acq_rel); // lost to an overflow drop: fine
            return n;
        }
        for (size_t i = 0; i < count; ++i)
        {
            const size_t src = count > 1u ? i * (consume - 1u) / (count - 1u) : 0u;
            const uint32_t f = m_data[static_cast<size_t>((read + src) % m_capacity)].load(std::memory_order_relaxed);
            out[i * 2u] = static_cast<int16_t>(f & 0xFFFFu);
            out[i * 2u + 1u] = static_cast<int16_t>(f >> 16u);
        }
        m_readIndex.compare_exchange_strong(read, read + consume, std::memory_order_acq_rel);
        return count;
    }

    size_t StereoFrameRing::popRateMatched(int16_t *out, size_t count) noexcept
    {
        // Keep the fill near half the ring: the emulation's 48 kHz and the device clock
        // drift apart, and fast-forward or a stall moves the fill. Above 3/4 consume ~1%
        // more source frames than played (skip), below 1/4 ~1% fewer (repeat).
        const size_t fill = available();
        const size_t step = std::max<size_t>(1u, count / 100u);
        size_t consume = count;
        if (fill > m_capacity * 3u / 4u)
            consume = count + step;
        else if (fill < m_capacity / 4u && count > step)
            consume = count - step;
        if (consume != count)
            m_adjusted.fetch_add(1u, std::memory_order_relaxed);
        return pop(out, count, consume);
    }

    // ---- settings / stats ----

    Spu2OutputSettings parseSpu2OutputSettings(const char *wavEnv, const char *hostEnv)
    {
        Spu2OutputSettings settings;
        if (wavEnv && *wavEnv)
            settings.wavPath = wavEnv;
        settings.host = flagOn(hostEnv);
        return settings;
    }

    Spu2OutputSettings spu2OutputSettingsFromEnvironment()
    {
        return parseSpu2OutputSettings(std::getenv("PS2X_SPU2_WAV"), std::getenv("PS2X_AUDIO_SPU2"));
    }

    void LevelStats::add(const int16_t *samples, size_t count) noexcept
    {
        for (size_t i = 0; i < count; ++i)
        {
            const int32_t l = samples[i * 2u];
            const int32_t r = samples[i * 2u + 1u];
            const int32_t al = l < 0 ? -l : l;
            const int32_t ar = r < 0 ? -r : r;
            peak = std::max(peak, std::max(al, ar));
            if (al > 64 || ar > 64)
                ++nonSilentFrames;
            clippedSamples += (l == 32767 || l == -32768) + (r == 32767 || r == -32768);
            sumSquares += static_cast<double>(l) * l + static_cast<double>(r) * r;
        }
        frames += count;
    }

    // ---- Spu2Output ----

    namespace
    {
        // raylib's stream callback carries no user pointer: the one active ring.
        std::atomic<StereoFrameRing *> g_hostRing{nullptr};
#if !defined(PLATFORM_VITA)
        AudioStream g_stream{};

        void hostStreamCallback(void *buffer, unsigned int frames)
        {
            StereoFrameRing *ring = g_hostRing.load(std::memory_order_acquire);
            if (!ring)
            {
                std::memset(buffer, 0, static_cast<size_t>(frames) * 4u);
                return;
            }
            ring->popRateMatched(static_cast<int16_t *>(buffer), frames);
        }
#endif
    }

    std::unique_ptr<Spu2Output> Spu2Output::create(const Spu2OutputSettings &settings, bool audioDeviceOpen)
    {
        const bool wantHost = settings.host && audioDeviceOpen;
        if (settings.host && !audioDeviceOpen)
            std::cout << "[audio:spu2] PS2X_AUDIO_SPU2 ignored: no audio device open (PS2X_NO_AUDIO/PS2X_MUTE/headless)" << std::endl;
        if (settings.wavPath.empty() && !wantHost)
            return nullptr;
        std::unique_ptr<Spu2Output> output(new Spu2Output());
        if (!settings.wavPath.empty())
        {
            if (output->m_wav.open(settings.wavPath, kSpu2SampleRate, 2u))
            {
                output->m_wavPath = settings.wavPath;
                std::cout << "[audio:spu2] writing the mixed SPU2 output to " << settings.wavPath << " (48 kHz stereo)" << std::endl;
            }
            else
                std::cout << "[audio:spu2] cannot create " << settings.wavPath << std::endl;
        }
        if (wantHost && output->startHostStream())
            std::cout << "[audio:spu2] host output on (PS2X_AUDIO_SPU2)" << std::endl;
        if (!output->m_wav.isOpen() && !output->m_hostStarted)
            return nullptr;
        return output;
    }

    bool Spu2Output::startHostStream()
    {
#if defined(PLATFORM_VITA)
        return false;
#else
        if (g_hostRing.load() != nullptr || !IsAudioDeviceReady())
            return false;
        // ~170 ms ring between the emulation and the device, held near half full (~85 ms
        // latency) by popRateMatched: enough to ride out the IOP's bursty catch-up.
        m_ring = std::make_unique<StereoFrameRing>(8192u);
        g_hostRing.store(m_ring.get(), std::memory_order_release);
        // raylib 5.5 has no per-stream buffer size: set the default for this one load
        // (1024 frames, ~21 ms per sub-buffer) and put it back to raylib's own default (0 =
        // device rate / 30) so other streams are not affected.
        SetAudioStreamBufferSizeDefault(1024);
        g_stream = LoadAudioStream(kSpu2SampleRate, 16u, 2u);
        SetAudioStreamBufferSizeDefault(0);
        if (!IsAudioStreamValid(g_stream))
        {
            g_hostRing.store(nullptr);
            m_ring.reset();
            return false;
        }
        SetAudioStreamCallback(g_stream, hostStreamCallback);
        PlayAudioStream(g_stream);
        m_hostStarted = true;
        return true;
#endif
    }

    void Spu2Output::stopHostStream()
    {
#if !defined(PLATFORM_VITA)
        if (!m_hostStarted)
            return;
        if (IsAudioDeviceReady())
        {
            StopAudioStream(g_stream);
            UnloadAudioStream(g_stream);
        }
        g_hostRing.store(nullptr, std::memory_order_release);
        std::cout << "[audio:spu2] host output closed dropped_frames=" << m_ring->droppedFrames()
                  << " underrun_frames=" << m_ring->underrunFrames() << " rate_adjusted_callbacks=" << m_ring->rateAdjustments()
                  << std::endl;
        m_hostStarted = false;
#endif
    }

    void Spu2Output::onFrames(const int16_t *frames, size_t count)
    {
        if (m_closed)
            return;
        if (m_wav.isOpen())
        {
            m_wav.write(frames, count);
            m_stats.add(frames, count);
        }
        if (m_hostStarted && m_ring)
            m_ring->push(frames, count);
    }

    void Spu2Output::close()
    {
        if (m_closed)
            return;
        m_closed = true;
        if (m_wav.isOpen())
        {
            m_wav.close();
            const double seconds = static_cast<double>(m_stats.frames) / kSpu2SampleRate;
            const double rms = m_stats.frames ? std::sqrt(m_stats.sumSquares / (2.0 * static_cast<double>(m_stats.frames))) : 0.0;
            std::cout << "[audio:spu2] wav closed " << m_wavPath << " frames=" << m_stats.frames << " seconds=" << seconds
                      << " non_silent_frames=" << m_stats.nonSilentFrames << " peak=" << m_stats.peak
                      << " rms=" << rms << " clipped_samples=" << m_stats.clippedSamples << std::endl;
        }
        stopHostStream();
    }

    Spu2Output::~Spu2Output()
    {
        close();
    }
}
