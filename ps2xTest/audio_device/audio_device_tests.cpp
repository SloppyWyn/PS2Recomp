// PS2X_NO_AUDIO / PS2X_MUTE / headless: the runner opens the host audio device only when
// none of them is set (ps2_headless.h decideAudioDevice). Without a device the audio
// backend is a null sink: sample uploads and play requests are accepted and dropped, and
// no raylib audio call needs a device.
#include "ps2_headless.h"
#include "runtime/ps2_audio.h"
#include "runtime/ps2_spu2_output.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

    using ps2x::headless::AudioDevice;
    using ps2x::headless::decideAudioDevice;

    void testDecision()
    {
        check(decideAudioDevice(false, nullptr, nullptr) == AudioDevice::Open, "nothing set: open");
        check(decideAudioDevice(false, "", "") == AudioDevice::Open, "empty values: open");
        check(decideAudioDevice(false, "0", "0") == AudioDevice::Open, "\"0\" values: open");
        check(decideAudioDevice(false, "1", nullptr) == AudioDevice::SkipNoAudio, "PS2X_NO_AUDIO=1: skip");
        check(decideAudioDevice(false, "yes", nullptr) == AudioDevice::SkipNoAudio, "PS2X_NO_AUDIO=yes: skip");
        check(decideAudioDevice(false, nullptr, "1") == AudioDevice::SkipMute, "PS2X_MUTE=1: skip");
        check(decideAudioDevice(false, "0", "1") == AudioDevice::SkipMute, "NO_AUDIO=0 MUTE=1: skip (mute)");
        check(decideAudioDevice(false, "1", "1") == AudioDevice::SkipNoAudio, "both: skip (no audio named)");
        check(decideAudioDevice(true, nullptr, nullptr) == AudioDevice::SkipHeadless, "headless: skip");
        check(decideAudioDevice(true, "1", "1") == AudioDevice::SkipHeadless, "headless wins");

        check(std::string(ps2x::headless::describeAudioDevice(AudioDevice::SkipNoAudio)) ==
                  "[audio] no audio device (PS2X_NO_AUDIO)",
              "no-audio line");
        check(std::string(ps2x::headless::describeAudioDevice(AudioDevice::SkipMute)) ==
                  "[audio] no audio device (PS2X_MUTE)",
              "mute line");
        check(std::string(ps2x::headless::describeAudioDevice(AudioDevice::SkipHeadless)) ==
                  "[audio] no audio device (headless)",
              "headless line (unchanged from srwz-headless)");
        check(std::string(ps2x::headless::describeAudioDevice(AudioDevice::Open)).empty(), "open: no line");

        check(!ps2x::headless::envFlagOn(nullptr) && !ps2x::headless::envFlagOn("") &&
                  !ps2x::headless::envFlagOn("0") && ps2x::headless::envFlagOn("1") &&
                  ps2x::headless::envFlagOn("00"),
              "envFlagOn: only unset, \"\" and \"0\" are off");
    }

    // A one-block VAG-style ADPCM sample (48-byte header + 16-byte blocks, flag 1 = end).
    std::vector<uint8_t> makeVag()
    {
        std::vector<uint8_t> v(48 + 16 * 4, 0);
        std::memcpy(v.data(), "VAGp", 4);
        const uint32_t dataSize = 16 * 4;
        v[12] = static_cast<uint8_t>(dataSize >> 24);
        v[13] = static_cast<uint8_t>(dataSize >> 16);
        v[14] = static_cast<uint8_t>(dataSize >> 8);
        v[15] = static_cast<uint8_t>(dataSize);
        const uint32_t rate = 22050;
        v[16] = static_cast<uint8_t>(rate >> 24);
        v[17] = static_cast<uint8_t>(rate >> 16);
        v[18] = static_cast<uint8_t>(rate >> 8);
        v[19] = static_cast<uint8_t>(rate);
        for (int b = 0; b < 4; ++b)
        {
            uint8_t *blk = v.data() + 48 + 16 * b;
            blk[0] = 0x02; // shift 2, filter 0
            blk[1] = (b == 3) ? 0x01 : 0x00;
            for (int i = 2; i < 16; ++i)
                blk[i] = static_cast<uint8_t>(0x17 * (i + b));
        }
        return v;
    }

    void testNullSink()
    {
        // No InitAudioDevice in this process: every path must drop the sound without
        // calling into raylib's device (which would assert/log "device not ready").
        PS2AudioBackend backend;
        backend.setAudioReady(false);
        std::vector<uint8_t> vag = makeVag();
        backend.onVagTransferFromBuffer(vag.data(), static_cast<uint32_t>(vag.size()), 0x1000);
        backend.play(0x1000);
        backend.play(0x1000, 2.0f, 0.5f, 0);
        backend.play(0xDEAD0);
        backend.stop(0);
        backend.stopAll();
        check(true, "null sink: uploads and plays without a device");
    }

    // SPU2 output (ps2_spu2_output.h): settings, WAV writer, non-blocking ring, and no host
    // stream without a device.
    void testSpu2Output()
    {
        using namespace ps2x::audio;
        const Spu2OutputSettings none = parseSpu2OutputSettings(nullptr, nullptr);
        check(none.wavPath.empty() && !none.host, "SPU2 output: off by default");
        check(!parseSpu2OutputSettings("", "0").host, "PS2X_AUDIO_SPU2=0: off");
        check(parseSpu2OutputSettings("x.wav", "1").host && parseSpu2OutputSettings("x.wav", "1").wavPath == "x.wav", "both set");
        check(Spu2Output::create(none, true) == nullptr, "nothing enabled: no sink");
        Spu2OutputSettings hostOnly;
        hostOnly.host = true;
        check(Spu2Output::create(hostOnly, false) == nullptr, "PS2X_AUDIO_SPU2 without a device (NO_AUDIO/MUTE/headless): no sink");

        const std::string path = "ps2x_spu2_output_test.wav";
        {
            Spu2OutputSettings wavOnly;
            wavOnly.wavPath = path;
            auto output = Spu2Output::create(wavOnly, false);
            check(output != nullptr, "WAV only: sink created without a device");
            std::vector<int16_t> frames(2u * 1000u);
            for (size_t i = 0; i < 1000u; ++i)
            {
                frames[i * 2u] = static_cast<int16_t>(i);
                frames[i * 2u + 1u] = static_cast<int16_t>(-static_cast<int>(i));
            }
            if (output)
            {
                output->onFrames(frames.data(), 600u);
                output->onFrames(frames.data() + 1200u, 400u);
                output->close();
            }
        }
        std::FILE *f = std::fopen(path.c_str(), "rb");
        std::vector<uint8_t> bytes;
        if (f)
        {
            uint8_t buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                bytes.insert(bytes.end(), buf, buf + n);
            std::fclose(f);
        }
        std::remove(path.c_str());
        auto u32 = [&](size_t at) { return static_cast<uint32_t>(bytes[at] | (bytes[at + 1] << 8) | (bytes[at + 2] << 16) | (bytes[at + 3] << 24)); };
        check(bytes.size() == 44u + 4000u, "WAV: header + 1000 stereo frames");
        if (bytes.size() == 44u + 4000u)
        {
            check(std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0, "WAV: RIFF/WAVE");
            check(u32(24) == 48000u && bytes[22] == 2 && bytes[34] == 16, "WAV: 48 kHz, stereo, 16-bit");
            check(u32(40) == 4000u && u32(4) == 4036u, "WAV: sizes patched on close");
            const int16_t last = static_cast<int16_t>(bytes[44 + 3996] | (bytes[44 + 3997] << 8));
            const int16_t lastR = static_cast<int16_t>(bytes[44 + 3998] | (bytes[44 + 3999] << 8));
            check(last == 999 && lastR == -999, "WAV: frames in order");
        }

        StereoFrameRing ring(4u);
        const int16_t in[12] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6};
        ring.push(in, 3u);
        ring.push(in + 6, 3u); // 6 frames into 4: the two oldest go
        check(ring.droppedFrames() == 2u && ring.available() == 4u, "ring: overflow drops the oldest frames, never blocks");
        int16_t popped[12] = {};
        check(ring.pop(popped, 6u) == 4u && popped[0] == 3 && popped[1] == -3 && popped[7] == -6 && popped[8] == 0 && popped[11] == 0,
              "ring: pop returns the newest frames in order, then silence");
        check(ring.underrunFrames() == 2u, "ring: underrun counted");
        check(ring.push(in + 8, 2u) == 2u && ring.pop(popped, 2u) == 2u && popped[0] == 5 && popped[3] == -6, "ring: wraps around");

        // Rate matching: above 3/4 full a callback consumes ~1% more frames than it plays,
        // below 1/4 ~1% fewer; in between exactly as many.
        StereoFrameRing big(1000u);
        std::vector<int16_t> ramp(2u * 1000u);
        for (size_t i = 0; i < 1000u; ++i)
            ramp[i * 2u] = ramp[i * 2u + 1u] = static_cast<int16_t>(i);
        big.push(ramp.data(), 900u);
        std::vector<int16_t> outBuf(2u * 200u);
        big.popRateMatched(outBuf.data(), 200u);
        check(big.available() == 900u - 202u && outBuf[0] == 0 && outBuf[2u * 199u] == 201, "rate match: full ring skips 2 of 202");
        big.popRateMatched(outBuf.data(), 200u);
        check(big.available() == 498u, "rate match: half-full ring plays 1:1");
        big.popRateMatched(outBuf.data(), 200u);
        big.popRateMatched(outBuf.data(), 100u);
        const size_t before = big.available();
        big.popRateMatched(outBuf.data(), 100u); // below 1/4: consumes 99, repeats one
        check(before == 198u && big.available() == 99u && big.rateAdjustments() == 2u, "rate match: low ring repeats 1 of 100");
    }
}

int main()
{
    testDecision();
    testNullSink();
    testSpu2Output();
    std::printf("audio_device: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
