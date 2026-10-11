#include "Common.h"
#include "MPEG.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_snapshot.h"

#if !defined(PS2X_HAS_FFMPEG)
#define PS2X_HAS_FFMPEG 1
#endif

#if PS2X_HAS_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}
#endif

#include <deque>
#include <memory>

#include "Syscalls/Helpers/State.h"
#include "ps2_snapshot_hooks.h"

#include <sstream>

namespace ps2_stubs
{
    namespace
    {
        struct MpegDecodedFrame
        {
            int width = 0;
            int height = 0;
            int repeatPict = 0;
            int64_t pts90k = -1;
            std::vector<uint8_t> rgba;
        };

#if PS2X_HAS_FFMPEG
        std::string ffmpegErrorString(int err)
        {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
            if (av_strerror(err, buffer.data(), buffer.size()) < 0)
            {
                return "unknown FFmpeg error";
            }
            return std::string(buffer.data());
        }

        void configureFfmpegLogLevel()
        {
            static std::once_flag s_once;
            std::call_once(s_once, []
                           {
#if AGRESSIVE_LOGS
                               av_log_set_level(AV_LOG_WARNING);
#else
                               av_log_set_level(AV_LOG_ERROR);
#endif
                           });
        }

        class MpegFfmpegDecoder
        {
        public:
            MpegFfmpegDecoder() = default;

            ~MpegFfmpegDecoder()
            {
                reset();
            }

            MpegFfmpegDecoder(const MpegFfmpegDecoder &) = delete;
            MpegFfmpegDecoder &operator=(const MpegFfmpegDecoder &) = delete;

            bool feed(const uint8_t *data, size_t size, std::deque<MpegDecodedFrame> &frames, int64_t pts90k = -1, int64_t dts90k = -1)
            {
                if (!data || size == 0)
                {
                    return true;
                }

                if (!ensureInitialized())
                {
                    return false;
                }

                static uint32_t s_feedLogCount = 0u;
                const bool shouldLog = (s_feedLogCount < 32u);
                if (shouldLog)
                    ++s_feedLogCount;

                size_t totalParsed = 0u;
                size_t totalPacketsSent = 0u;
                size_t framesBefore = frames.size();

                const uint8_t *cursor = data;
                size_t remaining = size;
                int64_t parserPts = pts90k >= 0 ? pts90k : AV_NOPTS_VALUE;
                int64_t parserDts = dts90k >= 0 ? dts90k : AV_NOPTS_VALUE;
                while (remaining > 0)
                {
                    uint8_t *packetData = nullptr;
                    int packetSize = 0;
                    const int chunk = static_cast<int>(std::min<size_t>(
                        remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
                    const int used = av_parser_parse2(
                        m_parser,
                        m_codecCtx,
                        &packetData,
                        &packetSize,
                        cursor,
                        chunk,
                        parserPts,
                        parserDts,
                        0);
                    if (used < 0)
                    {
                        std::cerr << "[MPEG] parser failed: " << ffmpegErrorString(used) << std::endl;
                        return false;
                    }
                    if (used == 0 && packetSize == 0)
                    {
                        break;
                    }

                    totalParsed += static_cast<size_t>(used);
                    cursor += used;
                    remaining -= static_cast<size_t>(used);

                    if (used > 0)
                    {
                        parserPts = AV_NOPTS_VALUE;
                        parserDts = AV_NOPTS_VALUE;
                    }

                    if (packetSize > 0)
                    {
                        ++totalPacketsSent;
                        if (!sendPacket(packetData, static_cast<size_t>(packetSize), frames, m_parser->pts, m_parser->dts))
                        {
                            return false;
                        }
                    }
                }

                if (shouldLog)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:feed] inSize=" << size
                                  << " parsed=" << totalParsed
                                  << " packets=" << totalPacketsSent
                                  << " newFrames=" << (frames.size() - framesBefore)
                                  << " totalFrames=" << frames.size()
                                  << std::endl;
                    });
                }

                return true;
            }

            bool flush(std::deque<MpegDecodedFrame> &frames)
            {
                if (!m_initialized || m_drained)
                {
                    return true;
                }

                if (m_parser)
                {
                    uint8_t *packetData = nullptr;
                    int packetSize = 0;
                    const int used = av_parser_parse2(
                        m_parser,
                        m_codecCtx,
                        &packetData,
                        &packetSize,
                        nullptr,
                        0,
                        AV_NOPTS_VALUE,
                        AV_NOPTS_VALUE,
                        0);
                    (void)used;
                    if (packetSize > 0 && !sendPacket(packetData, static_cast<size_t>(packetSize), frames, m_parser->pts, m_parser->dts))
                    {
                        return false;
                    }
                }

                const int sendRet = avcodec_send_packet(m_codecCtx, nullptr);
                if (sendRet < 0 && sendRet != AVERROR_EOF)
                {
                    std::cerr << "[MPEG] decoder flush failed: " << ffmpegErrorString(sendRet) << std::endl;
                    return false;
                }

                const bool ok = receiveFrames(frames);
                m_drained = true;
                return ok;
            }

            void reset()
            {
                if (m_swsCtx)
                {
                    sws_freeContext(m_swsCtx);
                    m_swsCtx = nullptr;
                }
                if (m_frame)
                {
                    av_frame_free(&m_frame);
                }
                if (m_packet)
                {
                    av_packet_free(&m_packet);
                }
                if (m_codecCtx)
                {
                    avcodec_free_context(&m_codecCtx);
                }
                if (m_parser)
                {
                    av_parser_close(m_parser);
                    m_parser = nullptr;
                }

                m_swsWidth = 0;
                m_swsHeight = 0;
                m_swsFormat = AV_PIX_FMT_NONE;
                m_initialized = false;
                m_drained = false;
            }

        private:
            bool ensureInitialized()
            {
                if (m_initialized)
                {
                    return true;
                }

                configureFfmpegLogLevel();

                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                if (!codec)
                {
                    std::cerr << "[MPEG] FFmpeg MPEG-2 decoder not found." << std::endl;
                    return false;
                }

                m_parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
                if (!m_parser)
                {
                    std::cerr << "[MPEG] FFmpeg MPEG-video parser not found." << std::endl;
                    return false;
                }

                m_codecCtx = avcodec_alloc_context3(codec);
                m_frame = av_frame_alloc();
                m_packet = av_packet_alloc();
                if (!m_codecCtx || !m_frame || !m_packet)
                {
                    std::cerr << "[MPEG] failed to allocate FFmpeg decoder state." << std::endl;
                    reset();
                    return false;
                }

                m_codecCtx->thread_count = 1;
                m_codecCtx->pkt_timebase = AVRational{1, 90000};
                // feedElementaryStream() does not create the decoder until a valid
                // MPEG sequence header has been found.  Dropping non-key pictures
                // here therefore throws away real presentation frames and makes
                // movies finish early once the EE is fast enough to drain them.
                m_codecCtx->skip_frame = AVDISCARD_DEFAULT;
                m_codecCtx->err_recognition = 0;
                const int ret = avcodec_open2(m_codecCtx, codec, nullptr);
                if (ret < 0)
                {
                    std::cerr << "[MPEG] failed to open MPEG decoder: " << ffmpegErrorString(ret) << std::endl;
                    reset();
                    return false;
                }

                m_initialized = true;
                m_drained = false;
                return true;
            }

            bool sendPacket(const uint8_t *data,
                            size_t size,
                            std::deque<MpegDecodedFrame> &frames,
                            int64_t pts = AV_NOPTS_VALUE,
                            int64_t dts = AV_NOPTS_VALUE)
            {
                if (!data || size == 0)
                {
                    return true;
                }

                av_packet_unref(m_packet);
                const int allocRet = av_new_packet(m_packet, static_cast<int>(size));
                if (allocRet < 0)
                {
                    std::cerr << "[MPEG] failed to allocate packet: " << ffmpegErrorString(allocRet) << std::endl;
                    return false;
                }
                std::memcpy(m_packet->data, data, size);
                m_packet->pts = pts;
                m_packet->dts = dts;

                int ret = avcodec_send_packet(m_codecCtx, m_packet);
                if (ret == AVERROR(EAGAIN))
                {
                    if (!receiveFrames(frames))
                    {
                        av_packet_unref(m_packet);
                        return false;
                    }
                    ret = avcodec_send_packet(m_codecCtx, m_packet);
                }
                av_packet_unref(m_packet);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                {
                    static uint32_t s_rejectedPacketLogCount = 0u;
                    if (s_rejectedPacketLogCount < 32u)
                    {
                        std::cerr << "[MPEG] decoder rejected packet, dropping: "
                                  << ffmpegErrorString(ret) << std::endl;
                        ++s_rejectedPacketLogCount;
                    }
                    return true;
                }

                return receiveFrames(frames);
            }

            bool receiveFrames(std::deque<MpegDecodedFrame> &frames)
            {
                while (true)
                {
                    const int ret = avcodec_receive_frame(m_codecCtx, m_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    {
                        return true;
                    }
                    if (ret < 0)
                    {
                        static uint32_t s_receiveErrorLogCount = 0u;
                        if (s_receiveErrorLogCount < 32u)
                        {
                            std::cerr << "[MPEG] decoder receive failed, dropping: "
                                      << ffmpegErrorString(ret) << std::endl;
                            ++s_receiveErrorLogCount;
                        }
                        return true;
                    }

                    if (!convertFrame(frames))
                    {
                        av_frame_unref(m_frame);
                        return false;
                    }
                    av_frame_unref(m_frame);
                }
            }

            bool convertFrame(std::deque<MpegDecodedFrame> &frames)
            {
                const int width = m_frame->width;
                const int height = m_frame->height;
                const AVPixelFormat srcFormat = static_cast<AVPixelFormat>(m_frame->format);
                if (width <= 0 || height <= 0 || srcFormat == AV_PIX_FMT_NONE)
                {
                    return false;
                }

                if (!m_swsCtx ||
                    m_swsWidth != width ||
                    m_swsHeight != height ||
                    m_swsFormat != srcFormat)
                {
                    if (m_swsCtx)
                    {
                        sws_freeContext(m_swsCtx);
                        m_swsCtx = nullptr;
                    }
                    m_swsCtx = sws_getContext(
                        width,
                        height,
                        srcFormat,
                        width,
                        height,
                        AV_PIX_FMT_RGBA,
                        SWS_BILINEAR,
                        nullptr,
                        nullptr,
                        nullptr);
                    if (!m_swsCtx)
                    {
                        std::cerr << "[MPEG] failed to create FFmpeg scaler." << std::endl;
                        return false;
                    }
                    m_swsWidth = width;
                    m_swsHeight = height;
                    m_swsFormat = srcFormat;
                }

                MpegDecodedFrame decoded;
                decoded.width = width;
                decoded.height = height;
                decoded.repeatPict = std::max(0, m_frame->repeat_pict);
                decoded.pts90k = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                     ? m_frame->best_effort_timestamp
                                     : -1;
                decoded.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

                uint8_t *dstData[4] = {decoded.rgba.data(), nullptr, nullptr, nullptr};
                int dstLinesize[4] = {width * 4, 0, 0, 0};
                const int scaledRows = sws_scale(
                    m_swsCtx,
                    m_frame->data,
                    m_frame->linesize,
                    0,
                    height,
                    dstData,
                    dstLinesize);
                if (scaledRows <= 0)
                {
                    std::cerr << "[MPEG] FFmpeg scaler produced no rows." << std::endl;
                    return false;
                }

                frames.push_back(std::move(decoded));
                return true;
            }

            AVCodecParserContext *m_parser = nullptr;
            AVCodecContext *m_codecCtx = nullptr;
            AVFrame *m_frame = nullptr;
            AVPacket *m_packet = nullptr;
            SwsContext *m_swsCtx = nullptr;
            int m_swsWidth = 0;
            int m_swsHeight = 0;
            AVPixelFormat m_swsFormat = AV_PIX_FMT_NONE;
            bool m_initialized = false;
            bool m_drained = false;
        };
#else
        // TODO
        class MpegFfmpegDecoder
        {
        public:
            bool feed(const uint8_t *, size_t, std::deque<MpegDecodedFrame> &, int64_t = -1, int64_t = -1)
            {
                static bool s_warnedNoFfmpeg = false;
                if (!s_warnedNoFfmpeg)
                {
                    std::cerr << "[MPEG] runtime built without FFmpeg; MPEG video decode is disabled." << std::endl;
                    s_warnedNoFfmpeg = true;
                }
                return false;
            }

            bool flush(std::deque<MpegDecodedFrame> &)
            {
                return true;
            }

            void reset() {}
        };
#endif

        struct MpegRegisteredCallback
        {
            uint32_t type = 0u;
            uint32_t streamId = 0u;
            uint32_t func = 0u;
            uint32_t data = 0u;
            uint32_t handle = 0u;
            bool stream = false;
        };

        // Snapshots (docs/SNAPSHOT-DESIGN.md): FFmpeg's decoder cannot be serialised, so
        // each playback logs every call it makes into its decoder since the second-newest
        // MPEG sequence header (about two GOPs). A restore feeds the log to a fresh decoder,
        // which then holds the same reference pictures; the replayed output is dropped
        // (those pictures were already produced, and the undelivered ones are restored
        // from the snapshot's queue).
        struct MpegDecoderCall
        {
            std::vector<uint8_t> data; // empty when flush
            int64_t pts90k = -1;
            int64_t dts90k = -1;
            bool flush = false;
        };

        // Largest decoder replay log kept per playback (docs/SNAPSHOT-DESIGN.md): 64 MB, or
        // PS2X_MPEG_REPLAY_MB. A stream whose sequence headers are far apart (or that has
        // only one) needs the log to cover everything since the last header, so a snapshot
        // late in a long movie needs a larger limit; the bytes stay in host memory.
        size_t mpegReplayLimitBytes()
        {
            static const size_t limit = []
            {
                size_t megabytes = 64u;
                if (const char *value = std::getenv("PS2X_MPEG_REPLAY_MB"); value && *value)
                {
                    const unsigned long parsed = std::strtoul(value, nullptr, 10);
                    if (parsed >= 1ul && parsed <= 4096ul)
                        megabytes = static_cast<size_t>(parsed);
                }
                return megabytes * 1024u * 1024u;
            }();
            return limit;
        }

        uint64_t mpegFrameFingerprint(const MpegDecodedFrame &frame)
        {
            // FNV-1a over the size and every 61st byte: cheap enough for every picture, and
            // neighbouring movie pictures differ in sampled bytes.
            uint64_t hash = 1469598103934665603ull;
            const auto mix = [&hash](uint64_t value)
            {
                hash ^= value;
                hash *= 1099511628211ull;
            };
            mix(static_cast<uint64_t>(frame.width));
            mix(static_cast<uint64_t>(frame.height));
            for (size_t i = 0; i < frame.rgba.size(); i += 61u)
                mix(frame.rgba[i]);
            return hash;
        }

        struct MpegStreamCallbackEvent
        {
            uint32_t mpegAddr = 0u;
            uint32_t streamType = 0u;
            uint32_t dataAddr = 0u;
            uint32_t len = 0u;
            uint64_t pts = 0xFFFFFFFFFFFFFFFFull;
            uint64_t dts = 0xFFFFFFFFFFFFFFFFull;
            std::vector<MpegRegisteredCallback> callbacks;
            // Where the packet sits in the playback's PSS byte stream (MpegPlaybackState::
            // pssBufferStartPos counts every byte the demuxer has taken in): its start code
            // and its payload.
            uint64_t packetPos = 0u;
            uint64_t payloadPos = 0u;
            // The same, as offsets into the input of the demux call that delivers the event
            // (or into a replay region, see MpegDemuxReplay); packetOffset < 0: the packet
            // began in an earlier call's input, which the caller has already moved past.
            int64_t packetOffset = -1;
            uint32_t payloadOffset = 0u;
        };

        // libmpeg's demuxer stops at a packet whose stream callback returns 0 (the game's
        // ring for that stream is full) and reports only the bytes before it as consumed, so
        // the caller presents that packet again later. The runtime parses (and feeds the
        // video in) everything a demux call is given before the callbacks run, so when it
        // reports fewer bytes it keeps the unreported tail here: the bytes, and the stream
        // events in them still to be delivered. When the caller presents those bytes again,
        // their events are delivered from here instead of parsing (and decoding) them twice.
        // Invariant: while it is not empty, the region ends where the parser's input ends.
        struct MpegDemuxReplay
        {
            std::vector<uint8_t> bytes;
            std::vector<MpegStreamCallbackEvent> events; // offsets relative to bytes[0]
        };

        constexpr uint64_t kPictureClockOne = 1ull << 32u;
        // NTSC-style fields at ~59.94 Hz to keep MPEG timing yet (29.97 fps).
        constexpr uint64_t kDefaultPictureIntervalQ32 = 2ull * kPictureClockOne;
        constexpr size_t kMpegTimingScanLimit = 4096u;
        constexpr size_t kMaxDecodedPicturesAhead = 8u;

        struct MpegPlaybackState
        {
            uint32_t picturesServed = 0u;
            uint32_t width = 320u;
            uint32_t height = 240u;
            uint32_t decodeMode = 0u;
            uint32_t imageBufferAddr = 0u;
            bool sawInput = false;
            bool sawSequenceEnd = false;
            // The sceMpegCbBackground callback ran during the current GetPicture call (not
            // saved in snapshots: a loaded run may call it once more).
            bool backgroundCalled = false;
            bool streamEnded = false;
            bool decoderFailed = false;
            uint64_t cdStreamGeneration = 0u;
            bool waitingForVideoSequenceHeader = true;
            std::vector<uint8_t> videoSequenceSyncBuffer;
            std::vector<uint8_t> pssBuffer;
            std::vector<uint32_t> pssGuestAddrs;
            uint64_t pssBufferStartPos = 0u; // stream position of pssBuffer[0]
            MpegDemuxReplay demuxReplay;
            // A demux call's synchronous stream callbacks are still running (see
            // finishDemuxWithCallbacks). Not saved: a snapshot waits until the chain is over.
            bool demuxRunInFlight = false;
            std::deque<MpegDecodedFrame> decodedFrames;
            std::unique_ptr<MpegFfmpegDecoder> decoder;
            uint8_t frameRateCode = 0u;
            uint8_t frameRateExtensionN = 0u;
            uint8_t frameRateExtensionD = 0u;
            bool hasFrameRateExtension = false;
            std::vector<uint8_t> videoTimingScanBuffer;
            uint64_t pictureIntervalQ32 = kDefaultPictureIntervalQ32;
            uint64_t nextPictureTickQ32 = std::numeric_limits<uint64_t>::max();
            uint64_t presentationEndTickQ32 = std::numeric_limits<uint64_t>::max();
            int64_t firstPresentedPts90k = -1;
            uint64_t ptsPresentationBaseTickQ32 = 0u;

            // Decoder replay log (see MpegDecoderCall). replayAnchors index the calls that
            // carried a sequence header (the last two are kept; the log starts at the first).
            std::vector<MpegDecoderCall> replayLog;
            std::vector<size_t> replayAnchors;
            size_t replayBytes = 0u;
            bool replayValid = true;
            uint64_t framesProduced = 0u;
            uint64_t framesProducedAtLogStart = 0u;
            uint64_t anchorFramesProduced = 0u; // framesProduced at the newest anchor
            int64_t lastProducedPts90k = -1;
            uint64_t lastProducedFingerprint = 0u;
        };

        void resetDecoderReplay(MpegPlaybackState &playback)
        {
            playback.replayLog.clear();
            playback.replayAnchors.clear();
            playback.replayBytes = 0u;
            playback.replayValid = true;
            playback.framesProducedAtLogStart = playback.framesProduced;
        }

        void recordDecoderCall(MpegPlaybackState &playback, const uint8_t *data, size_t size, int64_t pts90k, int64_t dts90k, bool flush);
        void noteProducedFrames(MpegPlaybackState &playback, size_t framesBefore);

        struct MpegStubState
        {
            bool initialized = false;
            uint32_t nextCallbackHandle = 1u;
            uint64_t cdStreamGeneration = 0u;
            uint64_t cdStreamBytesProduced = 0u;
            uint64_t cdStreamBytesDemuxed = 0u;
            bool cdStreamEofPending = false;
            bool currentCdStreamEofSeen = false;
            uint32_t feedEsTraceCount = 0u;
            uint32_t demuxPssTraceCount = 0u;
            uint32_t demuxRingTraceCount = 0u;
            uint32_t getPictureWaitTraceCount = 0u;
            uint32_t pictureTraceCount = 0u;
            uint32_t isEndTraceCount = 0u;
            // sceMpegGetPicture's no-data pull (see sceMpegGetPicture). Not saved in snapshots:
            // a loaded run starts the progress count afresh.
            uint32_t nodataTraceCount = 0u;
            uint64_t nodataCallsTotal = 0u;
            uint64_t nodataProgressMark = std::numeric_limits<uint64_t>::max();
            uint32_t nodataCallsWithoutProgress = 0u;
            std::unordered_map<uint32_t, std::vector<MpegRegisteredCallback>> callbacksByMpeg;
            std::unordered_map<uint32_t, MpegPlaybackState> playbackByMpeg;
        };

        std::mutex g_mpeg_stub_mutex;
        constexpr uint32_t kMpegPictureWaitType = 1u;
        MpegStubState g_mpeg_stub_state;

        // TODO this resolution should follow runtime resolution
        constexpr uint32_t kStubMovieWidth = 320u;
        constexpr uint32_t kStubMovieHeight = 240u;
        constexpr uint32_t kMpegStrM2V = 0u;
        constexpr uint32_t kMpegStrPCM = 1u;
        constexpr uint32_t kMpegStrADPCM = 2u;
        constexpr uint8_t kMpegPackHeader = 0xBAu;
        constexpr uint8_t kMpegSystemHeader = 0xBBu;
        constexpr uint8_t kMpegProgramEnd = 0xB9u;
        constexpr uint8_t kMpegSequenceEnd = 0xB7u;
        constexpr uint8_t kMpegPrivateStream1 = 0xBDu;
        constexpr size_t kStartCodeNotFound = std::numeric_limits<size_t>::max();
        constexpr uint32_t kMpegCallbackDataSize = 0x20u;

        uint64_t mpegPictureIntervalQ32(uint8_t frameRateCode, uint8_t frameRateExtensionN = 0u, uint8_t frameRateExtensionD = 0u)
        {
            uint64_t frameRateNumerator = 0u;
            uint64_t frameRateDenominator = 1u;
            switch (frameRateCode)
            {
            case 1u:
                frameRateNumerator = 24000u;
                frameRateDenominator = 1001u;
                break;
            case 2u:
                frameRateNumerator = 24u;
                break;
            case 3u:
                frameRateNumerator = 25u;
                break;
            case 4u:
                frameRateNumerator = 30000u;
                frameRateDenominator = 1001u;
                break;
            case 5u:
                frameRateNumerator = 30u;
                break;
            case 6u:
                frameRateNumerator = 50u;
                break;
            case 7u:
                frameRateNumerator = 60000u;
                frameRateDenominator = 1001u;
                break;
            case 8u:
                frameRateNumerator = 60u;
                break;
            default:
                return 0u;
            }

            const uint64_t extensionNumerator = static_cast<uint64_t>(frameRateExtensionN) + 1u;
            const uint64_t extensionDenominator = static_cast<uint64_t>(frameRateExtensionD) + 1u;
            const uint64_t denominator = 1001u * frameRateNumerator * extensionNumerator;
            const uint64_t numerator = (60000u * frameRateDenominator * extensionDenominator) << 32u;
            return std::max(kPictureClockOne, (numerator + denominator / 2u) / denominator);
        }

        uint32_t readMpegBits(const uint8_t *data, size_t bitOffset, uint32_t bitCount)
        {
            uint32_t value = 0u;
            for (uint32_t bit = 0u; bit < bitCount; ++bit)
            {
                const size_t absoluteBit = bitOffset + bit;
                const uint8_t source = data[absoluteBit >> 3u];
                value = (value << 1u) | ((source >> (7u - static_cast<uint32_t>(absoluteBit & 7u))) & 1u);
            }
            return value;
        }

        void updateMpegPictureTiming(MpegPlaybackState &playback, const uint8_t *data, size_t size)
        {
            if (!data || size == 0u)
            {
                return;
            }

            playback.videoTimingScanBuffer.insert(playback.videoTimingScanBuffer.end(), data, data + size);
            if (playback.videoTimingScanBuffer.size() > kMpegTimingScanLimit)
            {
                const size_t discard = playback.videoTimingScanBuffer.size() - kMpegTimingScanLimit;
                playback.videoTimingScanBuffer.erase(playback.videoTimingScanBuffer.begin(), playback.videoTimingScanBuffer.begin() + static_cast<std::ptrdiff_t>(discard));
            }

            const std::vector<uint8_t> &buffer = playback.videoTimingScanBuffer;
            size_t lastSequenceHeader = kStartCodeNotFound;
            for (size_t i = 0u; i + 7u < buffer.size(); ++i)
            {
                if (buffer[i + 0u] == 0x00u &&
                    buffer[i + 1u] == 0x00u &&
                    buffer[i + 2u] == 0x01u &&
                    buffer[i + 3u] == 0xB3u)
                {
                    const uint8_t frameRateCode = buffer[i + 7u] & 0x0Fu;
                    if (mpegPictureIntervalQ32(frameRateCode) != 0u)
                    {
                        lastSequenceHeader = i;
                    }
                }
            }

            if (lastSequenceHeader == kStartCodeNotFound)
            {
                return;
            }

            playback.frameRateCode = buffer[lastSequenceHeader + 7u] & 0x0Fu;
            playback.frameRateExtensionN = 0u;
            playback.frameRateExtensionD = 0u;
            playback.hasFrameRateExtension = false;
            playback.pictureIntervalQ32 = mpegPictureIntervalQ32(playback.frameRateCode);

            for (size_t i = lastSequenceHeader + 8u; i + 9u < buffer.size(); ++i)
            {
                if (buffer[i + 0u] != 0x00u ||
                    buffer[i + 1u] != 0x00u ||
                    buffer[i + 2u] != 0x01u)
                {
                    continue;
                }

                if (buffer[i + 3u] == 0xB3u)
                {
                    break;
                }
                if (buffer[i + 3u] != 0xB5u)
                {
                    continue;
                }

                const uint8_t *extension = buffer.data() + i + 4u;
                if (readMpegBits(extension, 0u, 4u) != 1u)
                {
                    continue;
                }

                playback.frameRateExtensionN = static_cast<uint8_t>(readMpegBits(extension, 41u, 2u));
                playback.frameRateExtensionD = static_cast<uint8_t>(readMpegBits(extension, 43u, 5u));
                playback.hasFrameRateExtension = true;
                playback.pictureIntervalQ32 = mpegPictureIntervalQ32(
                    playback.frameRateCode,
                    playback.frameRateExtensionN,
                    playback.frameRateExtensionD);
                break;
            }

            if (playback.pictureIntervalQ32 == 0u)
            {
                playback.pictureIntervalQ32 = kDefaultPictureIntervalQ32;
            }
        }

        uint64_t decodedFrameIntervalQ32(const MpegPlaybackState &playback,
                                         const MpegDecodedFrame &frame)
        {
            const uint64_t base = playback.pictureIntervalQ32 != 0u
                                      ? playback.pictureIntervalQ32
                                      : kDefaultPictureIntervalQ32;
            const uint64_t fields = static_cast<uint64_t>(2 + std::max(0, frame.repeatPict));
            return std::max(kPictureClockOne, (base * fields + 1u) / 2u);
        }

        constexpr uint64_t kMpegPtsWrap = 1ull << 33u;
        constexpr uint64_t kMpegPtsHalfWrap = 1ull << 32u;

        int64_t mpegPtsDelta90k(int64_t fromPts, int64_t toPts)
        {
            if (fromPts < 0 || toPts < 0)
            {
                return 0;
            }

            uint64_t from = static_cast<uint64_t>(fromPts) & (kMpegPtsWrap - 1u);
            uint64_t to = static_cast<uint64_t>(toPts) & (kMpegPtsWrap - 1u);
            uint64_t delta = (to - from) & (kMpegPtsWrap - 1u);
            if (delta >= kMpegPtsHalfWrap)
            {
                return -static_cast<int64_t>(kMpegPtsWrap - delta);
            }
            return static_cast<int64_t>(delta);
        }

        uint64_t mpegPtsDeltaToVSyncQ32(uint64_t delta90k)
        {
            // 90 kHz MPEG clock -> NTSC field clock (60000/1001 Hz):
            // fields = pts * 60000 / (90000 * 1001) = pts * 2 / 3003.
            constexpr uint64_t kPtsDivisor = 3003u;
            const uint64_t whole = delta90k / kPtsDivisor;
            const uint64_t remainder = delta90k % kPtsDivisor;
            const uint64_t wholeQ32 = whole * 2u * kPictureClockOne;
            const uint64_t remainderQ32 = ((remainder * 2u * kPictureClockOne) + kPtsDivisor / 2u) / kPtsDivisor;
            return wholeQ32 + remainderQ32;
        }

        uint64_t presentationTickForFrame(MpegPlaybackState &playback, const MpegDecodedFrame &frame, uint64_t currentTickQ32)
        {
            if (frame.pts90k < 0)
            {
                if (playback.nextPictureTickQ32 == std::numeric_limits<uint64_t>::max())
                {
                    playback.nextPictureTickQ32 = currentTickQ32;
                }
                return playback.nextPictureTickQ32;
            }

            if (playback.firstPresentedPts90k < 0)
            {
                playback.firstPresentedPts90k = frame.pts90k;
                playback.ptsPresentationBaseTickQ32 = currentTickQ32;
                return currentTickQ32;
            }

            const int64_t delta = mpegPtsDelta90k(playback.firstPresentedPts90k, frame.pts90k);
            if (delta >= 0)
            {
                return playback.ptsPresentationBaseTickQ32 + mpegPtsDeltaToVSyncQ32(static_cast<uint64_t>(delta));
            }

            const uint64_t backwards = mpegPtsDeltaToVSyncQ32(static_cast<uint64_t>(-delta));
            return playback.ptsPresentationBaseTickQ32 > backwards
                       ? playback.ptsPresentationBaseTickQ32 - backwards
                       : 0u;
        }

        uint32_t align16(uint32_t value)
        {
            return (value + 15u) & ~15u;
        }

        // Argument 5 (0-based index 4) is in t0 under the EE ABI. The game's copies of
        // sceMpegAddStrCallback (0x11a1f0) and sceMpegDemuxPssRing (0x119df0) use t0 directly;
        // a 0 there is a real 0, not a reason to read the caller's stack.
        uint32_t readAbiArg4(uint8_t *rdram, R5900Context *ctx)
        {
            (void)rdram;
            return ctx ? getRegU32(ctx, 8) : 0u;
        }

        MpegPlaybackState &getPlaybackState(uint32_t mpegAddr)
        {
            return g_mpeg_stub_state.playbackByMpeg[mpegAddr];
        }

        MpegPlaybackState makeFreshPlaybackState()
        {
            MpegPlaybackState playback{};
            playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            return playback;
        }

        MpegPlaybackState makeFreshPlaybackStatePreservingConfig(const MpegPlaybackState &oldPlayback)
        {
            MpegPlaybackState playback = makeFreshPlaybackState();
            playback.decodeMode = oldPlayback.decodeMode;
            playback.imageBufferAddr = oldPlayback.imageBufferAddr;
            playback.width = oldPlayback.width;
            playback.height = oldPlayback.height;
            return playback;
        }

        uint16_t readBe16(const uint8_t *p)
        {
            return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | static_cast<uint16_t>(p[1]));
        }

        bool isVideoStreamId(uint8_t streamId)
        {
            return streamId >= 0xE0u && streamId <= 0xEFu;
        }

        bool isAudioStreamId(uint8_t streamId)
        {
            return streamId == kMpegPrivateStream1 || (streamId >= 0xC0u && streamId <= 0xDFu);
        }

        bool isLengthPrefixedHeader(uint8_t streamId)
        {
            switch (streamId)
            {
            case kMpegSystemHeader:
            case 0xBCu: // program_stream_map
            case 0xBEu: // padding_stream
            case 0xBFu: // private_stream_2
            case 0xF0u: // ECM
            case 0xF1u: // EMM
            case 0xF2u: // DSMCC
            case 0xF8u: // ITU-T H.222.1 type E
            case 0xFFu: // program_stream_directory
                return true;
            default:
                return false;
            }
        }

        size_t findStartCode(const std::vector<uint8_t> &buffer, size_t from)
        {
            if (buffer.size() < 4 || from >= buffer.size() - 3u)
            {
                return kStartCodeNotFound;
            }

            for (size_t i = from; i + 3u < buffer.size(); ++i)
            {
                if (buffer[i] == 0x00u && buffer[i + 1u] == 0x00u && buffer[i + 2u] == 0x01u)
                {
                    return i;
                }
            }
            return kStartCodeNotFound;
        }

        bool containsMpegSequenceEnd(const uint8_t *data, size_t size)
        {
            if (!data || size < 4)
            {
                return false;
            }

            for (size_t i = 0; i + 3u < size; ++i)
            {
                if (data[i] == 0x00u &&
                    data[i + 1u] == 0x00u &&
                    data[i + 2u] == 0x01u &&
                    data[i + 3u] == kMpegSequenceEnd)
                {
                    return true;
                }
            }
            return false;
        }

        size_t findMpegSequenceHeader(const uint8_t *data, size_t size)
        {
            if (!data || size < 8u)
            {
                return kStartCodeNotFound;
            }

            for (size_t i = 0; i + 7u < size; ++i)
            {
                if (data[i] == 0x00u &&
                    data[i + 1u] == 0x00u &&
                    data[i + 2u] == 0x01u &&
                    data[i + 3u] == 0xB3u)
                {
                    const uint32_t width = (static_cast<uint32_t>(data[i + 4u]) << 4u) |
                                           (static_cast<uint32_t>(data[i + 5u]) >> 4u);
                    const uint32_t height = ((static_cast<uint32_t>(data[i + 5u]) & 0x0Fu) << 8u) |
                                            static_cast<uint32_t>(data[i + 6u]);
                    const uint8_t frameRateCode = data[i + 7u] & 0x0Fu;
                    if (width != 0u && height != 0u && width <= 4096u && height <= 4096u &&
                        mpegPictureIntervalQ32(frameRateCode) != 0u)
                    {
                        return i;
                    }
                }
            }
            return kStartCodeNotFound;
        }

        struct MpegPesHeader
        {
            size_t payloadOffset = 0u;
            int64_t pts90k = -1;
            int64_t dts90k = -1;
        };

        int64_t decodePesTimestamp90k(const uint8_t *p, size_t remaining)
        {
            if (!p || remaining < 5u)
            {
                return -1;
            }

            const uint64_t value =
                (static_cast<uint64_t>((p[0] >> 1u) & 0x07u) << 30u) |
                (static_cast<uint64_t>(p[1]) << 22u) |
                (static_cast<uint64_t>((p[2] >> 1u) & 0x7Fu) << 15u) |
                (static_cast<uint64_t>(p[3]) << 7u) |
                static_cast<uint64_t>((p[4] >> 1u) & 0x7Fu);
            return static_cast<int64_t>(value & (kMpegPtsWrap - 1u));
        }

        MpegPesHeader parsePesHeader(const uint8_t *packet, size_t packetSize)
        {
            MpegPesHeader result{};
            result.payloadOffset = packetSize;
            if (!packet || packetSize <= 6u)
            {
                return result;
            }

            size_t pos = 6u;
            if (packetSize >= 9u && (packet[pos] & 0xC0u) == 0x80u)
            {
                const uint8_t ptsDtsFlags = packet[pos + 1u] & 0xC0u;
                const size_t headerDataLength = static_cast<size_t>(packet[pos + 2u]);
                const size_t optionalStart = 9u;
                const size_t optionalEnd = std::min(packetSize, optionalStart + headerDataLength);
                if ((ptsDtsFlags == 0x80u || ptsDtsFlags == 0xC0u) && optionalEnd >= optionalStart + 5u)
                {
                    result.pts90k = decodePesTimestamp90k(packet + optionalStart, optionalEnd - optionalStart);
                }
                if (ptsDtsFlags == 0xC0u && optionalEnd >= optionalStart + 10u)
                {
                    result.dts90k = decodePesTimestamp90k(packet + optionalStart + 5u, optionalEnd - optionalStart - 5u);
                }
                result.payloadOffset = optionalEnd;
                return result;
            }

            // MPEG-1 PES.
            while (pos < packetSize && packet[pos] == 0xFFu)
            {
                ++pos;
            }
            if (pos + 1u < packetSize && (packet[pos] & 0xC0u) == 0x40u)
            {
                pos += 2u;
            }
            if (pos >= packetSize)
            {
                return result;
            }

            const uint8_t marker = packet[pos] & 0xF0u;
            if (marker == 0x20u && pos + 5u <= packetSize)
            {
                result.pts90k = decodePesTimestamp90k(packet + pos, packetSize - pos);
                pos += 5u;
            }
            else if (marker == 0x30u && pos + 10u <= packetSize)
            {
                result.pts90k = decodePesTimestamp90k(packet + pos, packetSize - pos);
                result.dts90k = decodePesTimestamp90k(packet + pos + 5u, packetSize - pos - 5u);
                pos += 10u;
            }
            else if (packet[pos] == 0x0Fu)
            {
                ++pos;
            }

            result.payloadOffset = std::min(packetSize, pos);
            return result;
        }

        void flushDecoder(MpegPlaybackState &playback)
        {
            if (!playback.decoder)
                return;
            recordDecoderCall(playback, nullptr, 0u, -1, -1, true);
            const size_t before = playback.decodedFrames.size();
            playback.decoder->flush(playback.decodedFrames);
            noteProducedFrames(playback, before);
        }

        void flushDecoderIfEnded(MpegPlaybackState &playback)
        {
            if (playback.streamEnded && playback.decoder)
            {
                flushDecoder(playback);
            }
        }

        void recordDecoderCall(MpegPlaybackState &playback, const uint8_t *data, size_t size, int64_t pts90k, int64_t dts90k, bool flush)
        {
            // The log exists only for snapshots (a heap copy of every chunk, a header scan):
            // nothing is recorded unless the run opted in (PS2SnapshotService::enabled).
            if (!playback.replayValid || !PS2SnapshotService::enabled())
                return;
            if (!flush && findMpegSequenceHeader(data, size) != kStartCodeNotFound)
            {
                playback.replayAnchors.push_back(playback.replayLog.size());
                if (playback.replayAnchors.size() > 2u)
                {
                    // Keep the log from the second-newest anchor on.
                    const size_t drop = playback.replayAnchors[playback.replayAnchors.size() - 2u];
                    size_t droppedBytes = 0u;
                    for (size_t i = 0; i < drop; ++i)
                        droppedBytes += playback.replayLog[i].data.size();
                    playback.replayLog.erase(playback.replayLog.begin(), playback.replayLog.begin() + static_cast<std::ptrdiff_t>(drop));
                    playback.replayBytes -= droppedBytes;
                    const size_t last = playback.replayAnchors.back();
                    playback.replayAnchors = {0u, last - drop};
                    // Pictures produced before the new log start are no longer replayed.
                    playback.framesProducedAtLogStart = playback.anchorFramesProduced;
                }
                playback.anchorFramesProduced = playback.framesProduced;
            }
            MpegDecoderCall call;
            if (!flush && data && size != 0u)
                call.data.assign(data, data + size);
            call.pts90k = pts90k;
            call.dts90k = dts90k;
            call.flush = flush;
            playback.replayBytes += call.data.size();
            playback.replayLog.push_back(std::move(call));
            if (playback.replayBytes > mpegReplayLimitBytes())
            {
                // A stream without repeated sequence headers: give up the exact restore
                // (a restore then resynchronises at the next sequence header).
                std::cerr << "[snapshot:mpeg] decoder replay log over " << (mpegReplayLimitBytes() >> 20)
                          << " MB after " << playback.framesProduced << " pictures: dropped; a snapshot of this"
                          << " playback restores pictures only from the next sequence header"
                          << " (PS2X_MPEG_REPLAY_MB raises the limit)" << std::endl;
                playback.replayLog.clear();
                playback.replayAnchors.clear();
                playback.replayBytes = 0u;
                playback.replayValid = false;
            }
        }

        void noteProducedFrames(MpegPlaybackState &playback, size_t framesBefore)
        {
            for (size_t i = framesBefore; i < playback.decodedFrames.size(); ++i)
            {
                const MpegDecodedFrame &frame = playback.decodedFrames[i];
                ++playback.framesProduced;
                playback.lastProducedPts90k = frame.pts90k;
                playback.lastProducedFingerprint = mpegFrameFingerprint(frame);
            }
        }

        // Video ES bytes handed to feedElementaryStream, all playbacks (tests: nothing is fed twice).
        std::atomic<uint64_t> g_mpegVideoBytesFed{0u};

        void feedElementaryStream(MpegPlaybackState &playback, const uint8_t *data, size_t size, int64_t pts90k = -1, int64_t dts90k = -1)
        {
            if (!data || size == 0)
            {
                return;
            }
            g_mpegVideoBytesFed.fetch_add(size, std::memory_order_relaxed);

            const uint32_t feedEsIdx = g_mpeg_stub_state.feedEsTraceCount++;
            if (feedEsIdx < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    char hexBuf[16] = {};
                    for (size_t i = 0; i < std::min<size_t>(4u, size); ++i)
                    {
                        ::snprintf(hexBuf + i * 2, 3, "%02x", data[i]);
                    }
                    std::cerr << "[MPEG:feedES] #" << feedEsIdx
                              << " size=" << size
                              << " first4=" << hexBuf
                              << " decoderFailed=" << playback.decoderFailed
                              << " waitSeq=" << playback.waitingForVideoSequenceHeader
                              << std::endl;
                });
            }

            playback.sawInput = true;
            updateMpegPictureTiming(playback, data, size);
            if (playback.waitingForVideoSequenceHeader)
            {
                playback.videoSequenceSyncBuffer.insert(
                    playback.videoSequenceSyncBuffer.end(),
                    data,
                    data + size);

                constexpr size_t kMaxVideoSequenceSyncBytes = 2u * 1024u * 1024u;
                if (playback.videoSequenceSyncBuffer.size() > kMaxVideoSequenceSyncBytes)
                {
                    const size_t keepFrom = playback.videoSequenceSyncBuffer.size() - 3u;
                    playback.videoSequenceSyncBuffer.erase(
                        playback.videoSequenceSyncBuffer.begin(),
                        playback.videoSequenceSyncBuffer.begin() + static_cast<std::ptrdiff_t>(keepFrom));
                }

                const size_t sequenceHeader = findMpegSequenceHeader(
                    playback.videoSequenceSyncBuffer.data(),
                    playback.videoSequenceSyncBuffer.size());
                if (sequenceHeader == kStartCodeNotFound)
                {
                    return;
                }

                if (sequenceHeader != 0u)
                {
                    playback.videoSequenceSyncBuffer.erase(
                        playback.videoSequenceSyncBuffer.begin(),
                        playback.videoSequenceSyncBuffer.begin() + static_cast<std::ptrdiff_t>(sequenceHeader));
                }

                data = playback.videoSequenceSyncBuffer.data();
                size = playback.videoSequenceSyncBuffer.size();
                if (playback.pictureIntervalQ32 == 0u)
                {
                    playback.pictureIntervalQ32 = kDefaultPictureIntervalQ32;
                }
                playback.nextPictureTickQ32 = std::numeric_limits<uint64_t>::max();
                playback.presentationEndTickQ32 = std::numeric_limits<uint64_t>::max();
                playback.firstPresentedPts90k = -1;
                playback.ptsPresentationBaseTickQ32 = 0u;
                playback.waitingForVideoSequenceHeader = false;
                playback.decoderFailed = false;
                playback.decoder.reset();
                playback.decodedFrames.clear();
            }

            if (containsMpegSequenceEnd(data, size))
            {
                playback.sawSequenceEnd = true;
                playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            }

            if (!playback.decoder)
            {
                playback.decoder = std::make_unique<MpegFfmpegDecoder>();
                resetDecoderReplay(playback);
            }

            recordDecoderCall(playback, data, size, pts90k, dts90k, false);
            const size_t framesBefore = playback.decodedFrames.size();
            const bool fed = playback.decoder->feed(data, size, playback.decodedFrames, pts90k, dts90k);
            noteProducedFrames(playback, framesBefore);
            if (!fed)
            {
                playback.decoder.reset();
                playback.waitingForVideoSequenceHeader = true;
                playback.videoSequenceSyncBuffer.clear();
                playback.decoderFailed = false;
                return;
            }

            playback.videoSequenceSyncBuffer.clear();
            flushDecoderIfEnded(playback);
        }

        void erasePssPrefix(MpegPlaybackState &playback, size_t count)
        {
            std::vector<uint8_t> &buffer = playback.pssBuffer;
            std::vector<uint32_t> &guestAddrs = playback.pssGuestAddrs;
            const size_t clamped = std::min(count, buffer.size());
            if (clamped == 0u)
            {
                return;
            }

            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(clamped));
            playback.pssBufferStartPos += clamped;
            if (guestAddrs.size() >= clamped)
            {
                guestAddrs.erase(guestAddrs.begin(), guestAddrs.begin() + static_cast<std::ptrdiff_t>(clamped));
            }
            else
            {
                guestAddrs.clear();
            }
        }

        std::vector<MpegRegisteredCallback> matchingStreamCallbacks(uint32_t mpegAddr, uint32_t streamType)
        {
            std::vector<MpegRegisteredCallback> out;
            auto it = g_mpeg_stub_state.callbacksByMpeg.find(mpegAddr);
            if (it == g_mpeg_stub_state.callbacksByMpeg.end())
            {
                return out;
            }

            for (const MpegRegisteredCallback &callback : it->second)
            {
                if (callback.stream && callback.type == streamType)
                {
                    out.push_back(callback);
                }
            }
            return out;
        }

        void queueStreamCallbackEvent(uint32_t mpegAddr,
                                      uint32_t streamType,
                                      uint32_t dataAddr,
                                      uint32_t len,
                                      std::vector<MpegStreamCallbackEvent> &callbackEvents,
                                      uint64_t packetPos,
                                      uint64_t payloadPos,
                                      int64_t pts90k = -1,
                                      int64_t dts90k = -1)
        {
            MpegStreamCallbackEvent event{};
            event.packetPos = packetPos;
            event.payloadPos = payloadPos;
            event.mpegAddr = mpegAddr;
            event.streamType = streamType;
            event.dataAddr = dataAddr;
            event.len = len;
            event.pts = pts90k >= 0 ? static_cast<uint64_t>(pts90k) : 0xFFFFFFFFFFFFFFFFull;
            event.dts = dts90k >= 0 ? static_cast<uint64_t>(dts90k) : 0xFFFFFFFFFFFFFFFFull;
            event.callbacks = matchingStreamCallbacks(mpegAddr, streamType);
            if (!event.callbacks.empty())
            {
                callbackEvents.push_back(std::move(event));
            }
        }

        void processPssBuffer(uint32_t mpegAddr,
                              MpegPlaybackState &playback,
                              std::vector<MpegStreamCallbackEvent> &callbackEvents,
                              bool finalChunk = false)
        {
            std::vector<uint8_t> &buffer = playback.pssBuffer;

            while (true)
            {
                if (playback.streamEnded)
                {
                    erasePssPrefix(playback, buffer.size());
                    return;
                }

                const size_t start = findStartCode(buffer, 0u);
                if (start == kStartCodeNotFound)
                {
                    if (finalChunk)
                    {
                        erasePssPrefix(playback, buffer.size());
                        return;
                    }
                    if (buffer.size() > 3u)
                    {
                        erasePssPrefix(playback, buffer.size() - 3u);
                    }
                    return;
                }

                if (start > 0u)
                {
                    erasePssPrefix(playback, start);
                }

                if (buffer.size() < 4u)
                {
                    return;
                }

                const uint8_t streamId = buffer[3u];

                if (streamId == kMpegProgramEnd)
                {
                    playback.streamEnded = true;
                    playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
                    flushDecoderIfEnded(playback);
                    erasePssPrefix(playback, buffer.size());
                    return;
                }

                if (streamId == kMpegPackHeader)
                {
                    if (buffer.size() < 12u)
                    {
                        if (finalChunk)
                        {
                            erasePssPrefix(playback, buffer.size());
                        }
                        return;
                    }

                    size_t packSize = 12u;
                    if ((buffer[4u] & 0xC0u) == 0x40u)
                    {
                        if (buffer.size() < 14u)
                        {
                            if (finalChunk)
                            {
                                erasePssPrefix(playback, buffer.size());
                            }
                            return;
                        }
                        packSize = 14u + static_cast<size_t>(buffer[13u] & 0x07u);
                    }
                    if (buffer.size() < packSize)
                    {
                        if (finalChunk)
                        {
                            erasePssPrefix(playback, buffer.size());
                        }
                        return;
                    }
                    erasePssPrefix(playback, packSize);
                    continue;
                }

                if (buffer.size() < 6u)
                {
                    if (finalChunk)
                    {
                        erasePssPrefix(playback, buffer.size());
                    }
                    return;
                }

                const uint16_t packetLength = readBe16(buffer.data() + 4u);
                if (isLengthPrefixedHeader(streamId))
                {
                    const size_t packetEnd = 6u + static_cast<size_t>(packetLength);
                    if (buffer.size() < packetEnd)
                    {
                        if (finalChunk)
                        {
                            erasePssPrefix(playback, buffer.size());
                        }
                        return;
                    }
                    erasePssPrefix(playback, packetEnd);
                    continue;
                }

                size_t packetEnd = 0u;
                if (packetLength != 0u)
                {
                    packetEnd = 6u + static_cast<size_t>(packetLength);
                    if (buffer.size() < packetEnd)
                    {
                        if (!finalChunk)
                        {
                            return;
                        }
                        packetEnd = buffer.size();
                    }
                }
                else
                {
                    const size_t next = findStartCode(buffer, 6u);
                    if (next == kStartCodeNotFound)
                    {
                        if (!finalChunk)
                        {
                            return;
                        }
                        packetEnd = buffer.size();
                    }
                    else
                    {
                        packetEnd = next;
                    }
                }

                if (isVideoStreamId(streamId))
                {
                    const MpegPesHeader pes = parsePesHeader(buffer.data(), packetEnd);
                    const size_t payloadStart = pes.payloadOffset;
                    if (payloadStart < packetEnd)
                    {
                        if (payloadStart < playback.pssGuestAddrs.size())
                        {
                            queueStreamCallbackEvent(
                                mpegAddr,
                                kMpegStrM2V,
                                playback.pssGuestAddrs[payloadStart],
                                static_cast<uint32_t>(packetEnd - payloadStart),
                                callbackEvents,
                                playback.pssBufferStartPos,
                                playback.pssBufferStartPos + payloadStart,
                                pes.pts90k,
                                pes.dts90k);
                        }
                        feedElementaryStream(
                            playback,
                            buffer.data() + payloadStart,
                            packetEnd - payloadStart,
                            pes.pts90k,
                            pes.dts90k);
                    }
                }
                else if (isAudioStreamId(streamId))
                {
                    const MpegPesHeader pes = parsePesHeader(buffer.data(), packetEnd);
                    const size_t payloadStart = pes.payloadOffset;
                    if (payloadStart < packetEnd && payloadStart < playback.pssGuestAddrs.size())
                    {
                        queueStreamCallbackEvent(
                            mpegAddr,
                            kMpegStrPCM,
                            playback.pssGuestAddrs[payloadStart],
                            static_cast<uint32_t>(packetEnd - payloadStart),
                            callbackEvents,
                            playback.pssBufferStartPos,
                            playback.pssBufferStartPos + payloadStart,
                            pes.pts90k,
                            pes.dts90k);
                        queueStreamCallbackEvent(
                            mpegAddr,
                            kMpegStrADPCM,
                            playback.pssGuestAddrs[payloadStart],
                            static_cast<uint32_t>(packetEnd - payloadStart),
                            callbackEvents,
                            playback.pssBufferStartPos,
                            playback.pssBufferStartPos + payloadStart,
                            pes.pts90k,
                            pes.dts90k);
                    }
                }

                erasePssPrefix(playback, packetEnd);
            }
        }

        void finishPlaybackStream(uint32_t mpegAddr, MpegPlaybackState &playback)
        {
            std::vector<MpegStreamCallbackEvent> ignoredCallbacks;
            processPssBuffer(mpegAddr, playback, ignoredCallbacks, true);
            playback.streamEnded = true;
            playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            flushDecoderIfEnded(playback);
        }

        void finalizeCdStreamEofUnlocked(std::vector<uint32_t> &completedMpegIds, bool &changed)
        {
            g_mpeg_stub_state.cdStreamEofPending = false;
            g_mpeg_stub_state.currentCdStreamEofSeen = true;
            for (auto &[mpegAddr, playback] : g_mpeg_stub_state.playbackByMpeg)
            {
                completedMpegIds.push_back(mpegAddr);
                if (!playback.sawInput || playback.streamEnded)
                {
                    continue;
                }

                finishPlaybackStream(mpegAddr, playback);
                changed = true;
            }
        }

        bool mpegDemuxBackpressured(const MpegPlaybackState &playback)
        {
            // Let EOF finalization drain any tail that is already in the guest
            // ring, otherwise bound decode lead to a handful of pictures.
            //
            // Important: do not park sceMpegDemuxPss/Ring here. Code Veronica
            // explicitly wakes its video thread before every demux call and that
            // thread sleeps again after presenting one picture. A single host
            // decoder feed can enqueue more than kMaxDecodedPicturesAhead frames;
            // parking the producer then leaves the consumer asleep after draining
            // just one frame, with nobody left to issue the next WakeupThread.
            // Returning 0 bytes consumed instead leaves the guest ring intact and
            // lets the game's producer loop wake the consumer again. Backpressure
            // still propagates naturally to sceCdStRead because the ring does not
            // advance while this is true.
            return !g_mpeg_stub_state.currentCdStreamEofSeen &&
                   playback.decodedFrames.size() >= kMaxDecodedPicturesAhead;
        }

        void recordCdStreamBytesDemuxedUnlocked(
            size_t consumed,
            std::vector<uint32_t> &completedMpegIds,
            bool &changed)
        {
            g_mpeg_stub_state.cdStreamBytesDemuxed += consumed;
            if (g_mpeg_stub_state.cdStreamEofPending &&
                g_mpeg_stub_state.cdStreamBytesDemuxed >= g_mpeg_stub_state.cdStreamBytesProduced)
            {
                finalizeCdStreamEofUnlocked(completedMpegIds, changed);
            }
        }

        void appendPssBytes(uint32_t mpegAddr,
                            MpegPlaybackState &playback,
                            const uint8_t *data,
                            size_t size,
                            uint32_t guestAddr,
                            std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            if (!data || size == 0)
            {
                return;
            }

            // A fresh playback keeps counting stream positions where the old one stopped (the
            // demux call that resets it computes its packets' offsets from them).
            const uint64_t streamPos = playback.pssBufferStartPos + playback.pssBuffer.size();
            if (playback.sawInput && playback.cdStreamGeneration != g_mpeg_stub_state.cdStreamGeneration)
            {
                playback = makeFreshPlaybackState();
                playback.pssBufferStartPos = streamPos;
            }

            if (playback.streamEnded)
            {
                if (playback.cdStreamGeneration == g_mpeg_stub_state.cdStreamGeneration)
                {
                    playback.sawInput = true;
                    return;
                }

                playback = makeFreshPlaybackState();
                playback.pssBufferStartPos = streamPos;
            }

            if (!playback.sawInput)
            {
                playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            }
            playback.sawInput = true;

            playback.pssBuffer.insert(playback.pssBuffer.end(), data, data + size);
            playback.pssGuestAddrs.reserve(playback.pssGuestAddrs.size() + size);
            for (size_t i = 0; i < size; ++i)
            {
                playback.pssGuestAddrs.push_back(guestAddr + static_cast<uint32_t>(i));
            }
            processPssBuffer(mpegAddr, playback, callbackEvents);
        }

        size_t appendGuestBytes(uint32_t mpegAddr,
                                MpegPlaybackState &playback,
                                const uint8_t *rdram,
                                uint32_t addr,
                                size_t size,
                                std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            size_t copied = 0u;
            while (copied < size)
            {
                const uint32_t curAddr = addr + static_cast<uint32_t>(copied);
                // Bound the chunk by the region the address resolves to: main RAM (the guest
                // address wraps at its end, as before) or the 16 KB scratchpad, which ends
                // there (the next address would resolve to main RAM: stop instead).
                uint32_t offset = 0u;
                bool scratch = false;
                if (!ps2ResolveGuestPointer(curAddr, offset, scratch))
                {
                    break;
                }
                const size_t regionSize = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                if (offset >= regionSize)
                {
                    break;
                }
                const size_t chunk = std::min<size_t>(size - copied, regionSize - offset);
                if (chunk == 0u)
                {
                    break;
                }

                const uint8_t *src = getConstMemPtr(rdram, curAddr);
                if (!src)
                {
                    break;
                }

                appendPssBytes(
                    mpegAddr,
                    playback,
                    src,
                    chunk,
                    curAddr,
                    callbackEvents);
                copied += chunk;
                if (scratch && offset + chunk >= PS2_SCRATCHPAD_SIZE)
                {
                    break; // the scratchpad ends here
                }
            }
            return copied;
        }

        // The input of one sceMpegDemuxPss/Ring call: `size` bytes from dataAddr; for
        // sceMpegDemuxPssRing they wrap at the end of the ring [ringBase, ringBase + ringSize)
        // (ringSize 0: no ring).
        struct MpegDemuxInput
        {
            uint32_t dataAddr = 0u;
            uint32_t size = 0u;
            uint32_t ringBase = 0u;
            uint32_t ringSize = 0u;
        };

        // Length of the input's first linear piece (at dataAddr); the rest is at ringBase.
        uint32_t demuxInputFirstLen(const MpegDemuxInput &in)
        {
            if (in.ringBase != 0u && in.ringSize != 0u && in.ringSize <= PS2_RAM_SIZE)
            {
                const uint32_t base = in.ringBase & PS2_RAM_MASK;
                const uint32_t data = in.dataAddr & PS2_RAM_MASK;
                const uint32_t ringOffset = (data - base) & PS2_RAM_MASK;
                if (ringOffset < in.ringSize)
                {
                    return std::min<uint32_t>(in.size, in.ringSize - ringOffset);
                }
            }
            return in.size;
        }

        uint32_t demuxInputAddr(const MpegDemuxInput &in, uint32_t offset)
        {
            const uint32_t first = demuxInputFirstLen(in);
            return offset < first ? in.dataAddr + offset : in.ringBase + (offset - first);
        }

        bool readDemuxInput(const uint8_t *rdram, const MpegDemuxInput &in, uint32_t offset, uint32_t len,
                            std::vector<uint8_t> &out)
        {
            out.resize(len);
            for (uint32_t i = 0; i < len; ++i)
            {
                const uint8_t *b = getConstMemPtr(rdram, demuxInputAddr(in, offset + i));
                if (!b)
                {
                    return false;
                }
                out[i] = *b;
            }
            return true;
        }

        // Parses the input from `from` on (the bytes before it were parsed by an earlier call);
        // returns how many bytes it took in.
        size_t appendDemuxInput(uint32_t mpegAddr,
                                MpegPlaybackState &playback,
                                const uint8_t *rdram,
                                const MpegDemuxInput &in,
                                uint32_t from,
                                std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            const uint32_t first = demuxInputFirstLen(in);
            size_t copied = 0u;
            if (from < first)
            {
                const size_t want = first - from;
                copied = appendGuestBytes(mpegAddr, playback, rdram, in.dataAddr + from, want, callbackEvents);
                if (copied < want)
                {
                    return copied;
                }
            }
            const uint32_t secondFrom = std::max(from, first);
            if (in.size > secondFrom)
            {
                copied += appendGuestBytes(mpegAddr, playback, rdram, in.ringBase + (secondFrom - first),
                                           in.size - secondFrom, callbackEvents);
            }
            return copied;
        }

        // Drops the first `count` bytes of the replay region (delivered) and their events.
        void advanceDemuxReplay(MpegDemuxReplay &replay, size_t count)
        {
            if (count >= replay.bytes.size())
            {
                replay = MpegDemuxReplay{};
                return;
            }
            replay.bytes.erase(replay.bytes.begin(), replay.bytes.begin() + static_cast<std::ptrdiff_t>(count));
            std::vector<MpegStreamCallbackEvent> kept;
            for (MpegStreamCallbackEvent &event : replay.events)
            {
                if (event.packetOffset >= static_cast<int64_t>(count))
                {
                    event.packetOffset -= static_cast<int64_t>(count);
                    event.payloadOffset -= static_cast<uint32_t>(count);
                    kept.push_back(std::move(event));
                }
            }
            replay.events = std::move(kept);
        }

        std::atomic<uint64_t> g_mpegDemuxReplayMismatches{0u};
        std::atomic<uint64_t> g_mpegDemuxNested{0u};

        // A demux of a playback whose previous demux call still runs its stream callbacks.
        void noteDemuxWhileRunInFlight(uint32_t mpegAddr)
        {
            const uint64_t n = g_mpegDemuxNested.fetch_add(1u) + 1u;
            if (n <= 8u || (n % 1024u) == 0u)
            {
                std::cerr << "[MPEG:demux:nested] mpeg=0x" << std::hex << mpegAddr << std::dec
                          << " demux while its stream callbacks run: consumes nothing (count=" << n << ")" << std::endl;
            }
        }

        struct MpegDemuxResult
        {
            size_t consumed = 0u; // input bytes the call reports consumed (unless a callback stops it)
            size_t parsed = 0u;   // of those, the bytes the parser took in for the first time
        };

        void noteAdpcmParsed(uint32_t streamType);

        // One demux call's work under g_mpeg_stub_mutex: delivers the events of a replay
        // region the caller presents again (see MpegDemuxReplay), then parses the new bytes.
        // Every event gets its offset in this call's input.
        MpegDemuxResult demuxInputUnlocked(uint32_t mpegAddr,
                                           MpegPlaybackState &playback,
                                           const uint8_t *rdram,
                                           const MpegDemuxInput &in,
                                           std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            MpegDemuxResult result;
            uint32_t from = 0u;
            MpegDemuxReplay &replay = playback.demuxReplay;
            if (!replay.bytes.empty())
            {
                const size_t regionLen = replay.bytes.size();
                const uint32_t shown = static_cast<uint32_t>(std::min<size_t>(in.size, regionLen));
                bool same = playback.cdStreamGeneration == g_mpeg_stub_state.cdStreamGeneration;
                for (uint32_t i = 0; same && i < shown; ++i)
                {
                    const uint8_t *b = getConstMemPtr(rdram, demuxInputAddr(in, i));
                    same = b != nullptr && *b == replay.bytes[i];
                }
                if (!same)
                {
                    // Not the bytes it was told were left: the caller moved on (a seek, a new
                    // stream). What it gives is parsed as new data.
                    const uint64_t n = g_mpegDemuxReplayMismatches.fetch_add(1u) + 1u;
                    if (n <= 16u || (n % 1024u) == 0u)
                    {
                        std::cerr << "[MPEG:demux:replay] mpeg=0x" << std::hex << mpegAddr << std::dec
                                  << " input does not continue the rolled-back region (" << regionLen
                                  << " bytes): dropped (mismatches=" << n << ")" << std::endl;
                    }
                    replay = MpegDemuxReplay{};
                }
                else
                {
                    size_t stop = shown;
                    for (const MpegStreamCallbackEvent &event : replay.events)
                    {
                        if (static_cast<size_t>(event.payloadOffset) + event.len > shown)
                        {
                            stop = static_cast<size_t>(event.packetOffset);
                            break;
                        }
                        MpegStreamCallbackEvent delivered = event;
                        delivered.dataAddr = demuxInputAddr(in, event.payloadOffset);
                        callbackEvents.push_back(std::move(delivered));
                    }
                    if (stop < regionLen)
                    {
                        // Only part of the region presented: the rest waits for the next call.
                        result.consumed = stop;
                        advanceDemuxReplay(replay, stop);
                        return result;
                    }
                    // All of it: the unfinished packet the parser carries at the region's end
                    // now lives here.
                    const size_t carried = std::min(playback.pssGuestAddrs.size(), regionLen);
                    const size_t base = playback.pssGuestAddrs.size() - carried;
                    for (size_t j = 0; j < carried; ++j)
                    {
                        playback.pssGuestAddrs[base + j] =
                            demuxInputAddr(in, static_cast<uint32_t>(regionLen - carried + j));
                    }
                    replay = MpegDemuxReplay{};
                    from = static_cast<uint32_t>(regionLen);
                }
            }

            const uint64_t basePos = playback.pssBufferStartPos + playback.pssBuffer.size();
            const size_t firstNew = callbackEvents.size();
            result.parsed = appendDemuxInput(mpegAddr, playback, rdram, in, from, callbackEvents);
            result.consumed = from + result.parsed;
            for (size_t i = firstNew; i < callbackEvents.size(); ++i)
            {
                MpegStreamCallbackEvent &event = callbackEvents[i];
                const int64_t packetOffset =
                    static_cast<int64_t>(from) + static_cast<int64_t>(event.packetPos) - static_cast<int64_t>(basePos);
                const int64_t payloadOffset =
                    static_cast<int64_t>(from) + static_cast<int64_t>(event.payloadPos) - static_cast<int64_t>(basePos);
                event.packetOffset = packetOffset >= 0 ? packetOffset : -1;
                event.payloadOffset = payloadOffset >= 0 ? static_cast<uint32_t>(payloadOffset) : 0u;
                noteAdpcmParsed(event.streamType);
            }
            return result;
        }

        bool writeMpegCallbackData(uint8_t *rdram, uint32_t addr, const MpegStreamCallbackEvent &event)
        {
            if (!rdram || addr == 0u)
            {
                return false;
            }

            uint8_t *data = getMemPtr(rdram, addr);
            if (!data)
            {
                return false;
            }

            std::memset(data, 0, kMpegCallbackDataSize);
            *reinterpret_cast<uint32_t *>(data + 0x00u) = event.streamType;
            *reinterpret_cast<uint32_t *>(data + 0x08u) = event.dataAddr;
            *reinterpret_cast<uint32_t *>(data + 0x0Cu) = event.len;
            *reinterpret_cast<uint64_t *>(data + 0x10u) = event.pts;
            *reinterpret_cast<uint64_t *>(data + 0x18u) = event.dts;
            return true;
        }

        // PS2X_MPEG_STRCB_CHECK=1 (debug): after every stream callback, check that its payload
        // bytes are still what they were when the demux produced the event, i.e. that the
        // callback copied the right data; counts on stderr as [MPEG:strcb] every 4096 callbacks.
        bool mpegStrCallbackCheckEnabled()
        {
            static const bool enabled = []
            {
                const char *v = std::getenv("PS2X_MPEG_STRCB_CHECK");
                return v && v[0] == '1';
            }();
            return enabled;
        }

        uint64_t mpegGuestBytesHash(const uint8_t *rdram, uint32_t addr, uint32_t len)
        {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < len; ++i)
            {
                const uint8_t *b = getConstMemPtr(rdram, addr + i);
                h = (h ^ (b ? *b : 0u)) * 1099511628211ull;
            }
            return h;
        }

        // GuestInvocation::tag of a queued stream callback (see takePendingInvocations use).
        constexpr uint64_t kMpegStrCallbackInvocationTag = 0x4D50454753545243ull; // "MPEGSTRC"

        struct MpegStrCallbackCheckCounts
        {
            std::atomic<uint64_t> syncOk{0u}, syncStale{0u}, queuedOk{0u}, queuedStale{0u};
            std::atomic<uint64_t> returnedZero{0u}; // backpressure: libmpeg would stop the demux there
            std::atomic<uint64_t> returnedZeroByType[3]{}; // M2V, PCM, ADPCM
        };
        MpegStrCallbackCheckCounts g_mpegStrCheck;

        void noteStrCallbackCheck(bool sync, bool stale, uint32_t streamType, bool returnedZero)
        {
            auto &counter = sync ? (stale ? g_mpegStrCheck.syncStale : g_mpegStrCheck.syncOk)
                                 : (stale ? g_mpegStrCheck.queuedStale : g_mpegStrCheck.queuedOk);
            counter.fetch_add(1u, std::memory_order_relaxed);
            if (returnedZero)
            {
                g_mpegStrCheck.returnedZero.fetch_add(1u, std::memory_order_relaxed);
                if (streamType < 3u)
                {
                    g_mpegStrCheck.returnedZeroByType[streamType].fetch_add(1u, std::memory_order_relaxed);
                }
            }
            const uint64_t total = g_mpegStrCheck.syncOk + g_mpegStrCheck.syncStale +
                                   g_mpegStrCheck.queuedOk + g_mpegStrCheck.queuedStale;
            static std::atomic<uint32_t> s_staleLogs{0u};
            if (stale && s_staleLogs.fetch_add(1u) < 16u)
            {
                std::cerr << "[MPEG:strcb] stale payload: type=" << streamType << (sync ? " sync" : " queued") << std::endl;
            }
            if ((total % 4096u) == 0u)
            {
                std::cerr << "[MPEG:strcb] callbacks=" << total
                          << " sync ok=" << g_mpegStrCheck.syncOk << " stale=" << g_mpegStrCheck.syncStale
                          << " queued ok=" << g_mpegStrCheck.queuedOk << " stale=" << g_mpegStrCheck.queuedStale
                          << " returned0=" << g_mpegStrCheck.returnedZero
                          << " (video " << g_mpegStrCheck.returnedZeroByType[0]
                          << " pcm " << g_mpegStrCheck.returnedZeroByType[1]
                          << " adpcm " << g_mpegStrCheck.returnedZeroByType[2] << ")"
                          << std::endl;
            }
        }

        // PS2X_MPEG_STRCB_CHECK=1 also follows every ADPCM packet: parsed (first time the demux
        // sees it), accepted by the game's callback (once per packet, with an order-sensitive
        // hash of the accepted payloads), rolled back (callback returned 0, the demux stopped
        // there and the packet is presented again) or dropped (returned 0, lost). Printed as
        // [MPEG:adpcm] every 256 accepted packets.
        enum class MpegAdpcmOutcome
        {
            Accepted,
            RolledBack,
            Dropped,
        };

        struct MpegAdpcmCounts
        {
            std::atomic<uint64_t> parsed{0u}, accepted{0u}, rolledBack{0u}, dropped{0u};
            std::atomic<uint64_t> starved{0u}; // of the dropped: no rollback, the picture decode was waiting
            std::atomic<uint64_t> acceptedHash{1469598103934665603ull};
        };
        MpegAdpcmCounts g_mpegAdpcm;

        void noteAdpcmParsed(uint32_t streamType)
        {
            if (streamType == kMpegStrADPCM && mpegStrCallbackCheckEnabled())
            {
                g_mpegAdpcm.parsed.fetch_add(1u, std::memory_order_relaxed);
            }
        }

        void noteAdpcmOutcome(const uint8_t *rdram, uint32_t streamType, uint32_t dataAddr, uint32_t len,
                              MpegAdpcmOutcome outcome)
        {
            if (streamType != kMpegStrADPCM || !mpegStrCallbackCheckEnabled())
            {
                return;
            }
            if (outcome == MpegAdpcmOutcome::RolledBack)
            {
                g_mpegAdpcm.rolledBack.fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            if (outcome == MpegAdpcmOutcome::Dropped)
            {
                g_mpegAdpcm.dropped.fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            // Stream callbacks run on the EE executor thread only: no concurrent update.
            const uint64_t payload = mpegGuestBytesHash(rdram, dataAddr, len);
            const uint64_t hash = (g_mpegAdpcm.acceptedHash.load(std::memory_order_relaxed) ^ payload) * 1099511628211ull;
            g_mpegAdpcm.acceptedHash.store(hash, std::memory_order_relaxed);
            const uint64_t accepted = g_mpegAdpcm.accepted.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((accepted % 256u) == 0u)
            {
                std::cerr << "[MPEG:adpcm] parsed=" << g_mpegAdpcm.parsed << " accepted=" << accepted
                          << " rolledback=" << g_mpegAdpcm.rolledBack << " dropped=" << g_mpegAdpcm.dropped
                          << " (starved " << g_mpegAdpcm.starved << ")"
                          << " hash=" << std::hex << hash << std::dec << std::endl;
            }
        }

        // Builds the invocation of one stream callback: callback(mpeg, &cbdata, data), the
        // sceMpegCbDataStr block in guest memory, freed when the callback returns.
        bool makeGuestStreamCallbackInvocation(uint8_t *rdram,
                                               R5900Context *callerCtx,
                                               PS2Runtime *runtime,
                                               const MpegStreamCallbackEvent &event,
                                               const MpegRegisteredCallback &callback,
                                               GuestInvocation &invocation,
                                               bool sync = false)
        {
            if (!rdram || !callerCtx || !runtime || callback.func == 0u || !runtime->hasFunction(callback.func))
            {
                return false;
            }

            const uint32_t cbDataAddr = runtime->guestMalloc(kMpegCallbackDataSize, 16u);
            if (cbDataAddr == 0u)
            {
                return false;
            }
            if (!writeMpegCallbackData(rdram, cbDataAddr, event))
            {
                runtime->guestFree(cbDataAddr);
                return false;
            }

            R5900Context callbackCtx = *callerCtx;
            SET_GPR_U32(&callbackCtx, 4, event.mpegAddr);
            SET_GPR_U32(&callbackCtx, 5, cbDataAddr);
            SET_GPR_U32(&callbackCtx, 6, callback.data);
            SET_GPR_U32(&callbackCtx, 7, 0u);
            SET_GPR_U32(&callbackCtx, 29, 0u);
            SET_GPR_U32(&callbackCtx, 31, 0u);
            callbackCtx.pc = callback.func;

            invocation = GuestInvocation{};
            invocation.kind = GuestInvocationKind::RpcCallback;
            invocation.tag = kMpegStrCallbackInvocationTag;
            invocation.context = callbackCtx;
            invocation.onComplete = [runtime, cbDataAddr](const R5900Context &, R5900Context &)
            {
                runtime->guestFree(cbDataAddr);
            };
            if (mpegStrCallbackCheckEnabled())
            {
                const uint32_t dataAddr = event.dataAddr;
                const uint32_t len = event.len;
                const uint32_t type = event.streamType;
                const uint64_t expected = mpegGuestBytesHash(rdram, dataAddr, len);
                invocation.onComplete = [runtime, rdram, cbDataAddr, dataAddr, len, type, expected, sync](const R5900Context &completed, R5900Context &)
                {
                    noteStrCallbackCheck(sync, mpegGuestBytesHash(rdram, dataAddr, len) != expected, type,
                                         getRegU32(&completed, 2) == 0u);
                    if (!sync)
                    {
                        // Queued: its return value comes after the demux has returned.
                        noteAdpcmOutcome(rdram, type, dataAddr, len,
                                         getRegU32(&completed, 2) != 0u ? MpegAdpcmOutcome::Accepted
                                                                        : MpegAdpcmOutcome::Dropped);
                    }
                    runtime->guestFree(cbDataAddr);
                };
            }
            invocation.completeTag = {GuestInvocationCompleteKind::GuestFree, cbDataAddr};
            return true;
        }

        void dispatchGuestStreamCallback(uint8_t *rdram,
                                         R5900Context *callerCtx,
                                         PS2Runtime *runtime,
                                         const MpegStreamCallbackEvent &event,
                                         const MpegRegisteredCallback &callback)
        {
            GuestInvocation invocation;
            if (makeGuestStreamCallbackInvocation(rdram, callerCtx, runtime, event, callback, invocation))
            {
                runtime->eeScheduler().queueInvocation(std::move(invocation));
            }
        }

        void dispatchStreamCallbacks(uint8_t *rdram,
                                     R5900Context *ctx,
                                     PS2Runtime *runtime,
                                     const std::vector<MpegStreamCallbackEvent> &events)
        {
            if (events.empty())
            {
                return;
            }

            for (const MpegStreamCallbackEvent &event : events)
            {
                for (const MpegRegisteredCallback &callback : event.callbacks)
                {
                    dispatchGuestStreamCallback(rdram, ctx, runtime, event, callback);
                }
            }
        }


        // libmpeg's callback types (sceMpegAddCallback).
        constexpr uint32_t kMpegCbNodata = 1u;
        constexpr uint32_t kMpegCbBackground = 4u;
        // GuestInvocation::tag of a no-data callback run by sceMpegGetPicture.
        constexpr uint64_t kMpegNodataInvocationTag = 0x4D50454743424E44ull; // "MPEGCBND"
        // No-data callbacks that demux nothing new before GetPicture gives up and waits (a
        // game's callback may only drain its own ES ring, 4 KB per call in SRWZ, before it reads).
        constexpr uint32_t kMpegNodataMaxCallsWithoutProgress = 4096u;

        bool mpegInsideNodataCallback(PS2Runtime *runtime)
        {
            return runtime != nullptr && runtime->eeScheduler().currentThread() != nullptr &&
                   runtime->eeScheduler().hasInvocation(GuestInvocationKind::HleCall, kMpegNodataInvocationTag);
        }

        // The current CD stream's program end has been demuxed for this playback. Inside a
        // no-data callback, sceMpegDemuxPss/Ring then consume nothing (return 0): a callback
        // like SRWZ's 0x18bb50 feeds until its video ES ring holds 4 KB, but that ring is
        // filled by the video stream callbacks, which the runtime queues and runs only after
        // the callback returns; past the end the runtime keeps "consuming" the disc data behind
        // the movie, so the callback would read forever (mpon1: stuck at the movie's last
        // picture). On hardware the IPU finds the sequence end in data it already has and
        // _getpic never calls the callback there.
        bool mpegPlaybackEndedUnlocked(const MpegPlaybackState &playback)
        {
            return playback.streamEnded && playback.cdStreamGeneration == g_mpeg_stub_state.cdStreamGeneration;
        }

        // Where sceMpegDemuxPss/Ring run the stream callbacks (PS2X_MPEG_SYNC_STRCB):
        // - Always (default, "1"): synchronously, before the demux call returns, as libmpeg's
        //   demuxer calls them (_sceMpegDispatchMpegCallback 0x115df0), whenever a guest thread
        //   is current; else queued.
        // - InsideNodata ("nodata"): only inside the game's no-data/background callback (the
        //   behaviour before the demux backpressure); queued elsewhere.
        // - Never ("0"): always queued (they run at the next dispatch; A/B testing only).
        enum class MpegStrCallbackMode : int
        {
            Never = 0,
            InsideNodata = 1,
            Always = 2,
        };
        std::atomic<int> g_mpegStrCallbackModeOverride{-1};
        std::atomic<int> g_mpegDemuxRollbackOverride{-1};

        MpegStrCallbackMode mpegStrCallbackMode()
        {
            const int forced = g_mpegStrCallbackModeOverride.load(std::memory_order_relaxed);
            if (forced >= 0)
            {
                return static_cast<MpegStrCallbackMode>(forced);
            }
            static const MpegStrCallbackMode mode = []
            {
                const char *v = std::getenv("PS2X_MPEG_SYNC_STRCB");
                if (v && std::strcmp(v, "0") == 0)
                {
                    return MpegStrCallbackMode::Never;
                }
                if (v && std::strcmp(v, "nodata") == 0)
                {
                    return MpegStrCallbackMode::InsideNodata;
                }
                return MpegStrCallbackMode::Always;
            }();
            return mode;
        }

        // PS2X_MPEG_DEMUX_BACKPRESSURE=0: a stream callback that returns 0 does not stop the
        // demux (its packet is dropped, the behaviour before; A/B testing only).
        bool mpegDemuxRollbackEnabled()
        {
            const int forced = g_mpegDemuxRollbackOverride.load(std::memory_order_relaxed);
            if (forced >= 0)
            {
                return forced != 0;
            }
            static const bool enabled = []
            {
                const char *v = std::getenv("PS2X_MPEG_DEMUX_BACKPRESSURE");
                return !(v && v[0] == '0' && v[1] == '\0');
            }();
            return enabled;
        }

        // The synchronous stream callbacks of one demux call, run one after another on the
        // calling thread; each one's completion starts the next (one invocation at a time,
        // so they all use the stack of the same invocation depth).
        struct MpegDemuxCallbackRun
        {
            uint8_t *rdram = nullptr;
            PS2Runtime *runtime = nullptr;
            uint32_t mpegAddr = 0u;
            MpegDemuxInput input;
            size_t consumed = 0u;
            bool rollbackAllowed = true;
            std::vector<MpegStreamCallbackEvent> events;
            std::vector<std::pair<size_t, size_t>> items; // (event, callback) in call order
            size_t next = 0u;
        };

        // libmpeg's backpressure: the callback of events[eventIndex] returned 0. The demux
        // reports the bytes before that packet as consumed; the rest of the input becomes the
        // front of the playback's replay region (the caller presents it again). Returns false
        // when the packet cannot be presented again (it began in an earlier call's input).
        bool rollbackDemuxAt(MpegDemuxCallbackRun &run, size_t eventIndex, R5900Context &caller)
        {
            const MpegStreamCallbackEvent &stopped = run.events[eventIndex];
            if (stopped.packetOffset < 0 || static_cast<size_t>(stopped.packetOffset) >= run.consumed)
            {
                return false;
            }
            const uint32_t keepFrom = static_cast<uint32_t>(stopped.packetOffset);
            MpegDemuxReplay front;
            if (!readDemuxInput(run.rdram, run.input, keepFrom, static_cast<uint32_t>(run.consumed) - keepFrom,
                                front.bytes))
            {
                return false;
            }
            for (size_t i = eventIndex; i < run.events.size(); ++i)
            {
                MpegStreamCallbackEvent event = run.events[i];
                if (event.packetOffset < static_cast<int64_t>(keepFrom))
                {
                    continue; // (a second callback of an earlier event cannot follow; defensive)
                }
                event.packetOffset -= keepFrom;
                event.payloadOffset -= keepFrom;
                front.events.push_back(std::move(event));
            }
            {
                std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
                const auto it = g_mpeg_stub_state.playbackByMpeg.find(run.mpegAddr);
                if (it == g_mpeg_stub_state.playbackByMpeg.end())
                {
                    return false;
                }
                MpegDemuxReplay &replay = it->second.demuxReplay;
                const uint32_t shift = static_cast<uint32_t>(front.bytes.size());
                for (MpegStreamCallbackEvent &event : replay.events)
                {
                    event.packetOffset += shift;
                    event.payloadOffset += shift;
                    front.events.push_back(std::move(event));
                }
                front.bytes.insert(front.bytes.end(), replay.bytes.begin(), replay.bytes.end());
                replay = std::move(front);
            }
            setReturnS32(&caller, static_cast<int32_t>(keepFrom));
            return true;
        }

        void invokeNextDemuxCallback(const std::shared_ptr<MpegDemuxCallbackRun> &run, const R5900Context &caller);

        void setDemuxRunInFlight(uint32_t mpegAddr, bool inFlight)
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            const auto it = g_mpeg_stub_state.playbackByMpeg.find(mpegAddr);
            if (it != g_mpeg_stub_state.playbackByMpeg.end())
            {
                it->second.demuxRunInFlight = inFlight;
            }
        }

        // Audio packets a stream callback refused that the demux could not roll back, so they
        // are lost (always counted, logged first 8 then every 256th): GetPicture was waiting
        // for a picture inside its no-data callback, or the packet began in an earlier call.
        std::atomic<uint64_t> g_mpegDemuxLostStarved{0u};
        std::atomic<uint64_t> g_mpegDemuxLostEarlier{0u};

        void noteDemuxPacketLost(uint32_t mpegAddr, uint32_t streamType, bool starved)
        {
            const uint64_t starvedCount = starved ? g_mpegDemuxLostStarved.fetch_add(1u) + 1u : g_mpegDemuxLostStarved.load();
            const uint64_t earlierCount = starved ? g_mpegDemuxLostEarlier.load() : g_mpegDemuxLostEarlier.fetch_add(1u) + 1u;
            const uint64_t total = starvedCount + earlierCount;
            if (total <= 8u || (total % 256u) == 0u)
            {
                std::cerr << "[MPEG:demux:lost] mpeg=0x" << std::hex << mpegAddr << std::dec << " type=" << streamType
                          << " packet refused and not presented again ("
                          << (starved ? "GetPicture waiting for a picture" : "packet began in an earlier call")
                          << "): lost=" << total << " (starved " << starvedCount << ", earlier " << earlierCount << ")"
                          << std::endl;
            }
        }

        void onDemuxCallbackDone(const std::shared_ptr<MpegDemuxCallbackRun> &run, size_t item,
                                 const R5900Context &completed, R5900Context &caller)
        {
            const size_t eventIndex = run->items[item].first;
            const MpegStreamCallbackEvent &event = run->events[eventIndex];
            const bool accepted = getRegU32(&completed, 2) != 0u;
            MpegAdpcmOutcome outcome = MpegAdpcmOutcome::Accepted;
            // A video callback's 0 does not stop the demux: the runtime decodes the video it
            // demuxes itself, and the game's video ES ring is drained only by IPU DMA, which
            // the runtime does not model, so it reports "full" most of the time; stopping
            // there would stall the picture decode on a ring nothing reads.
            if (!accepted && event.streamType != kMpegStrM2V && mpegDemuxRollbackEnabled() &&
                run->rollbackAllowed && rollbackDemuxAt(*run, eventIndex, caller))
            {
                setDemuxRunInFlight(run->mpegAddr, false);
                noteAdpcmOutcome(run->rdram, event.streamType, event.dataAddr, event.len, MpegAdpcmOutcome::RolledBack);
                return;
            }
            if (!accepted)
            {
                outcome = MpegAdpcmOutcome::Dropped;
                if (!run->rollbackAllowed && event.streamType == kMpegStrADPCM && mpegStrCallbackCheckEnabled())
                {
                    g_mpegAdpcm.starved.fetch_add(1u, std::memory_order_relaxed);
                }
                if (event.streamType != kMpegStrM2V && mpegDemuxRollbackEnabled())
                {
                    noteDemuxPacketLost(run->mpegAddr, event.streamType, !run->rollbackAllowed);
                }
            }
            noteAdpcmOutcome(run->rdram, event.streamType, event.dataAddr, event.len, outcome);
            run->next = item + 1u;
            invokeNextDemuxCallback(run, caller);
        }

        // Starts the next callback of the run (never returns when there is one).
        void invokeNextDemuxCallback(const std::shared_ptr<MpegDemuxCallbackRun> &run, const R5900Context &caller)
        {
            while (run->next < run->items.size())
            {
                const size_t item = run->next;
                const MpegStreamCallbackEvent &event = run->events[run->items[item].first];
                const MpegRegisteredCallback &callback = event.callbacks[run->items[item].second];
                R5900Context templateCtx = caller;
                GuestInvocation invocation;
                if (!makeGuestStreamCallbackInvocation(run->rdram, &templateCtx, run->runtime, event, callback,
                                                       invocation, true))
                {
                    ++run->next;
                    continue;
                }
                invocation.kind = GuestInvocationKind::HleCall;
                auto freeAndCheck = std::move(invocation.onComplete);
                invocation.onComplete = [run, item, freeAndCheck](const R5900Context &completed, R5900Context &parent)
                {
                    freeAndCheck(completed, parent);
                    onDemuxCallbackDone(run, item, completed, parent);
                };
                // The completion carries the run's state: a snapshot waits until it is over.
                invocation.completeTag = {};
                run->runtime->eeScheduler().invokeCurrent(std::move(invocation));
            }
            setDemuxRunInFlight(run->mpegAddr, false); // the last callback has returned
        }

        // The end of sceMpegDemuxPss/Ring: v0 = bytes consumed, then the stream callbacks.
        // Synchronous callbacks (see MpegStrCallbackMode) see their payload in the caller's
        // PSS buffer, which SRWZ's feeder (0x18bd70) refills as soon as the demux returns, and
        // SRWZ's no-data callback (0x18bb50) loops until the video callback has put 4 KB into
        // its ES ring. A synchronous callback that returns 0 stops the demux at its packet
        // (rollbackDemuxAt). Queued callbacks run after the demux has returned: their return
        // value comes too late, and a 0 there loses the packet (counted by
        // PS2X_MPEG_STRCB_CHECK).
        void finishDemuxWithCallbacks(uint8_t *rdram,
                                      R5900Context *ctx,
                                      PS2Runtime *runtime,
                                      uint32_t mpegAddr,
                                      const MpegDemuxInput &input,
                                      std::vector<MpegStreamCallbackEvent> &&events,
                                      size_t consumed,
                                      bool inNodataCallback,
                                      bool pictureStarved)
        {
            setReturnS32(ctx, static_cast<int32_t>(consumed));
            if (events.empty())
            {
                return;
            }
            const MpegStrCallbackMode mode = mpegStrCallbackMode();
            const bool sync = runtime != nullptr && runtime->eeScheduler().currentThread() != nullptr &&
                              (mode == MpegStrCallbackMode::Always ||
                               (mode == MpegStrCallbackMode::InsideNodata && inNodataCallback));
            if (!sync)
            {
                dispatchStreamCallbacks(rdram, ctx, runtime, events);
                return;
            }
            auto run = std::make_shared<MpegDemuxCallbackRun>();
            run->rdram = rdram;
            run->runtime = runtime;
            run->mpegAddr = mpegAddr;
            run->input = input;
            run->consumed = consumed;
            // The game's sceMpegGetPicture is waiting inside its no-data callback for a picture
            // the runtime has not decoded yet: no rollback then. The game drains its audio ring
            // only from its player loop, after GetPicture returns; stopping at a full audio
            // ring here would leave the decoder without video and GetPicture parked for good.
            // (On hardware the audio drains in real time and the IPU needs less of the stream
            // ahead than the host decoder, so the stall does not arise there.)
            run->rollbackAllowed = !pictureStarved;
            run->events = std::move(events);
            for (size_t i = 0; i < run->events.size(); ++i)
            {
                for (size_t c = 0; c < run->events[i].callbacks.size(); ++c)
                {
                    run->items.emplace_back(i, c);
                }
            }
            // The demux caller resumes at its return address with v0 set above (or lowered by
            // a rollback) once the last callback returns. Until then a demux of the same
            // playback (a callback that demuxes, or another thread while a callback blocks)
            // consumes nothing: it would parse past the bytes this run may still roll back to.
            setDemuxRunInFlight(mpegAddr, true);
            invokeNextDemuxCallback(run, *ctx);
        }

        // PS2X_MPEG_BACKGROUND_CB=0: no sceMpegCbBackground call from sceMpegGetPicture.
        bool mpegBackgroundCallEnabled()
        {
            static const bool enabled = []
            {
                const char *v = std::getenv("PS2X_MPEG_BACKGROUND_CB");
                return !(v && v[0] == '0' && v[1] == 0);
            }();
            return enabled;
        }

        // PS2X_MPEG_NODATA_CB=0: sceMpegGetPicture parks as before instead of calling the
        // game's no-data callback (A/B testing only).
        bool mpegNodataPullEnabled()
        {
            static const bool enabled = []
            {
                const char *v = std::getenv("PS2X_MPEG_NODATA_CB");
                return !(v && v[0] == '0' && v[1] == '\0');
            }();
            return enabled;
        }

        // The callback sceMpegAddCallback(mpeg, type, ...) installed last (libmpeg keeps one
        // per type and slot; a later call replaces it).
        bool findMpegCallbackUnlocked(uint32_t mpegAddr, uint32_t type, MpegRegisteredCallback &out)
        {
            const auto it = g_mpeg_stub_state.callbacksByMpeg.find(mpegAddr);
            if (it == g_mpeg_stub_state.callbacksByMpeg.end())
            {
                return false;
            }
            for (auto cb = it->second.rbegin(); cb != it->second.rend(); ++cb)
            {
                if (!cb->stream && cb->type == type)
                {
                    out = *cb;
                    return out.func != 0u;
                }
            }
            return false;
        }

        std::function<void(const R5900Context &, R5900Context &)> makeNodataComplete(uint8_t *rdram,
                                                                                     PS2Runtime *runtime,
                                                                                     uint32_t cbDataAddr)
        {
            return [rdram, runtime, cbDataAddr](const R5900Context &, R5900Context &parent)
            {
                runtime->guestFree(cbDataAddr);
                // The game's GetPicture call resumes: parent holds its arguments unchanged.
                sceMpegGetPicture(rdram, &parent, runtime);
            };
        }

        // Runs one of the game's libmpeg callbacks on the calling thread, as libmpeg does from
        // inside sceMpegGetPicture: sceMpegCbNodata when the IPU input runs dry
        // (_sceMpegDispatchMpegCbNodata), sceMpegCbBackground while it waits for the IPU:
        // callback(mpeg, &{type}, data). Afterwards GetPicture runs again from the start.
        // Returns false (nothing done) when it cannot be called.
        bool invokeMpegNodataCallback(uint8_t *rdram,
                                      R5900Context *ctx,
                                      PS2Runtime *runtime,
                                      uint32_t mpegAddr,
                                      const MpegRegisteredCallback &callback,
                                      uint32_t callbackType = kMpegCbNodata)
        {
            if (!rdram || !ctx || !runtime || !runtime->hasFunction(callback.func))
            {
                return false;
            }
            const uint32_t cbDataAddr = runtime->guestMalloc(kMpegCallbackDataSize, 16u);
            if (cbDataAddr == 0u)
            {
                return false;
            }
            uint8_t *cbData = getMemPtr(rdram, cbDataAddr);
            if (!cbData)
            {
                runtime->guestFree(cbDataAddr);
                return false;
            }
            std::memset(cbData, 0, kMpegCallbackDataSize);
            *reinterpret_cast<uint32_t *>(cbData) = callbackType;

            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::HleCall;
            invocation.tag = kMpegNodataInvocationTag;
            invocation.context = *ctx;
            SET_GPR_U32(&invocation.context, 4, mpegAddr);
            SET_GPR_U32(&invocation.context, 5, cbDataAddr);
            SET_GPR_U32(&invocation.context, 6, callback.data);
            SET_GPR_U32(&invocation.context, 7, 0u);
            SET_GPR_U32(&invocation.context, 29, 0u); // the invocation stack
            SET_GPR_U32(&invocation.context, 31, 0u); // return to the scheduler
            invocation.context.pc = callback.func;
            invocation.onComplete = makeNodataComplete(rdram, runtime, cbDataAddr);
            invocation.completeTag = {GuestInvocationCompleteKind::MpegNodataComplete, cbDataAddr};

            // Stream callbacks queued by earlier demux calls outside a callback have not run
            // yet: run them first, so the game's rings get their packets in stream order
            // before the callback demuxes more. They run one after another before the
            // callback starts, so they share its stack.
            std::vector<GuestInvocation> sequence = runtime->eeScheduler().takePendingInvocations(
                GuestInvocationKind::RpcCallback, kMpegStrCallbackInvocationTag);
            if (sequence.empty())
            {
                runtime->eeScheduler().invokeCurrent(std::move(invocation));
            }
            const uint32_t sharedSp = runtime->eeScheduler().invocationStackTop();
            SET_GPR_U32(&invocation.context, 29, sharedSp);
            for (GuestInvocation &pending : sequence)
            {
                SET_GPR_U32(&pending.context, 29, sharedSp);
            }
            sequence.push_back(std::move(invocation));
            runtime->eeScheduler().invokeCurrentSequence(std::move(sequence));
        }

        void writeBlankMpegFrame(uint8_t *rdram, uint32_t destAddr, uint32_t width, uint32_t height)
        {
            if (!rdram || destAddr == 0u)
            {
                return;
            }

            const uint32_t outWidth = align16(width == 0u ? kStubMovieWidth : width);
            const uint32_t outHeight = align16(height == 0u ? kStubMovieHeight : height);
            const uint32_t macroblockColumns = outWidth / 16u;
            for (uint32_t mbx = 0u; mbx < macroblockColumns; ++mbx)
            {
                const size_t stripOffset =
                    static_cast<size_t>(mbx) * static_cast<size_t>(outHeight) * 16u * 4u;
                for (uint32_t y = 0u; y < outHeight; ++y)
                {
                    uint8_t *dst = getMemPtr(
                        rdram,
                        destAddr + static_cast<uint32_t>(stripOffset + static_cast<size_t>(y) * 16u * 4u));
                    if (!dst)
                    {
                        continue;
                    }
                    for (uint32_t x = 0u; x < 16u; ++x)
                    {
                        dst[x * 4u + 0u] = 0u;
                        dst[x * 4u + 1u] = 0u;
                        dst[x * 4u + 2u] = 0u;
                        dst[x * 4u + 3u] = 0x80u;
                    }
                }
            }
        }

        void writeDecodedFrameToGuest(uint8_t *rdram, uint32_t destAddr, const MpegDecodedFrame &frame)
        {
            if (!rdram || destAddr == 0u || frame.rgba.empty() || frame.width <= 0 || frame.height <= 0)
            {
                return;
            }

            const uint32_t width = static_cast<uint32_t>(frame.width);
            const uint32_t height = static_cast<uint32_t>(frame.height);
            const uint32_t outWidth = align16(width);
            const uint32_t outHeight = align16(height);
            const uint32_t macroblockColumns = outWidth / 16u;

            for (uint32_t mbx = 0u; mbx < macroblockColumns; ++mbx)
            {
                const size_t stripOffset =
                    static_cast<size_t>(mbx) * static_cast<size_t>(outHeight) * 16u * 4u;
                for (uint32_t y = 0u; y < outHeight; ++y)
                {
                    uint8_t *dst = getMemPtr(
                        rdram,
                        destAddr + static_cast<uint32_t>(stripOffset + static_cast<size_t>(y) * 16u * 4u));
                    if (!dst)
                    {
                        continue;
                    }

                    for (uint32_t x = 0u; x < 16u; ++x)
                    {
                        const uint32_t srcX = mbx * 16u + x;
                        const uint8_t *src = nullptr;
                        if (srcX < width && y < height)
                        {
                            src = frame.rgba.data() +
                                  (static_cast<size_t>(y) * static_cast<size_t>(width) + srcX) * 4u;
                        }

                        if (src)
                        {
                            dst[x * 4u + 0u] = src[0u];
                            dst[x * 4u + 1u] = src[1u];
                            dst[x * 4u + 2u] = src[2u];
                            dst[x * 4u + 3u] = 0x80u;
                        }
                        else
                        {
                            dst[x * 4u + 0u] = 0u;
                            dst[x * 4u + 1u] = 0u;
                            dst[x * 4u + 2u] = 0u;
                            dst[x * 4u + 3u] = 0x80u;
                        }
                    }
                }
            }
        }

        void resetMpegStubStateUnlocked()
        {
            g_mpeg_stub_state.initialized = false;
            g_mpeg_stub_state.nextCallbackHandle = 1u;
            g_mpeg_stub_state.cdStreamGeneration = 0u;
            g_mpeg_stub_state.cdStreamBytesProduced = 0u;
            g_mpeg_stub_state.cdStreamBytesDemuxed = 0u;
            g_mpeg_stub_state.cdStreamEofPending = false;
            g_mpeg_stub_state.currentCdStreamEofSeen = false;
            g_mpeg_stub_state.feedEsTraceCount = 0u;
            g_mpeg_stub_state.demuxPssTraceCount = 0u;
            g_mpeg_stub_state.demuxRingTraceCount = 0u;
            g_mpeg_stub_state.getPictureWaitTraceCount = 0u;
            g_mpeg_stub_state.pictureTraceCount = 0u;
            g_mpeg_stub_state.isEndTraceCount = 0u;
            g_mpeg_stub_state.nodataTraceCount = 0u;
            g_mpeg_stub_state.nodataCallsTotal = 0u;
            g_mpeg_stub_state.nodataProgressMark = std::numeric_limits<uint64_t>::max();
            g_mpeg_stub_state.nodataCallsWithoutProgress = 0u;
            g_mpeg_stub_state.callbacksByMpeg.clear();
            g_mpeg_stub_state.playbackByMpeg.clear();
        }
    }

    void resetMpegStubState()
    {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        resetMpegStubStateUnlocked();
    }

    void setMpegStreamCallbackModeForTesting(int mode)
    {
        g_mpegStrCallbackModeOverride.store(mode, std::memory_order_relaxed);
    }

    void setMpegDemuxRollbackForTesting(int enabled)
    {
        g_mpegDemuxRollbackOverride.store(enabled, std::memory_order_relaxed);
    }

    uint64_t mpegVideoBytesFedForTesting()
    {
        return g_mpegVideoBytesFed.load(std::memory_order_relaxed);
    }

    size_t mpegDemuxReplayBytesForTesting(uint32_t mpegAddr)
    {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        const auto it = g_mpeg_stub_state.playbackByMpeg.find(mpegAddr);
        return it == g_mpeg_stub_state.playbackByMpeg.end() ? 0u : it->second.demuxReplay.bytes.size();
    }

    void enqueueMpegDecodedFrameForTesting(uint32_t mpegAddr)
    {
        constexpr int kTestFrameWidth = 16;
        constexpr int kTestFrameHeight = 16;

        MpegDecodedFrame frame;
        frame.width = kTestFrameWidth;
        frame.height = kTestFrameHeight;
        frame.rgba.resize(static_cast<size_t>(kTestFrameWidth * kTestFrameHeight * 4), 0x80u);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        playback.sawInput = true;
        playback.decodedFrames.push_back(std::move(frame));
    }

    void notifyMpegCdStreamStart(PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        ++g_mpeg_stub_state.cdStreamGeneration;
        g_mpeg_stub_state.cdStreamBytesProduced = 0u;
        g_mpeg_stub_state.cdStreamBytesDemuxed = 0u;
        g_mpeg_stub_state.cdStreamEofPending = false;
        g_mpeg_stub_state.currentCdStreamEofSeen = false;
        g_mpeg_stub_state.feedEsTraceCount = 0u;
        g_mpeg_stub_state.demuxPssTraceCount = 0u;
        g_mpeg_stub_state.demuxRingTraceCount = 0u;
        g_mpeg_stub_state.getPictureWaitTraceCount = 0u;
        g_mpeg_stub_state.pictureTraceCount = 0u;
        g_mpeg_stub_state.isEndTraceCount = 0u;

        for (auto &[mpegAddr, playback] : g_mpeg_stub_state.playbackByMpeg)
        {
            playback = makeFreshPlaybackStatePreservingConfig(playback);
        }
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[MPEG:CdStreamStart] generation=" << g_mpeg_stub_state.cdStreamGeneration
                      << " reopened=" << g_mpeg_stub_state.playbackByMpeg.size() << std::endl;
        });
    }

    void notifyMpegCdStreamDataProduced(uint32_t byteCount, bool endOfStream)
    {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.cdStreamBytesProduced += byteCount;
        if (endOfStream)
        {
            g_mpeg_stub_state.cdStreamEofPending = true;
        }
    }

    void notifyMpegCdStreamEof(PS2Runtime *runtime)
    {
        std::vector<uint32_t> completedMpegIds;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            finalizeCdStreamEofUnlocked(completedMpegIds, changed);
        }

        if (changed)
        {
            static uint32_t s_eofLogCount = 0u;
            if (s_eofLogCount < 8u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[MPEG:CdStreamEof] finalized active MPEG playback" << std::endl;
                });
                ++s_eofLogCount;
            }
        }
        if (runtime)
        {
            for (const uint32_t mpegAddr : completedMpegIds)
            {
                runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
            }
        }
    }

    void sceMpegFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        bool wakePictureWaiter = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            const size_t framesBefore = playback.decodedFrames.size();
            if (playback.decoder)
            {
                flushDecoder(playback);
            }
            wakePictureWaiter = playback.decodedFrames.size() != framesBefore ||
                                playback.streamEnded ||
                                playback.decoderFailed;
        }
        if (wakePictureWaiter)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        setReturnS32(ctx, 0);
    }

    void sceMpegAddBs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t dataAddr = getRegU32(ctx, 5);
        const uint32_t byteCount = getRegU32(ctx, 6);

        size_t copied = 0u;
        bool wakePictureWaiter = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            const size_t framesBefore = playback.decodedFrames.size();
            while (copied < byteCount)
            {
                const uint32_t curAddr = dataAddr + static_cast<uint32_t>(copied);
                const uint32_t offset = curAddr & PS2_RAM_MASK;
                const size_t chunk = std::min<size_t>(static_cast<size_t>(byteCount) - copied, PS2_RAM_SIZE - offset);
                const uint8_t *src = getConstMemPtr(rdram, curAddr);
                if (!src || chunk == 0u)
                {
                    break;
                }
                feedElementaryStream(playback, src, chunk);
                copied += chunk;
            }
            wakePictureWaiter = playback.decodedFrames.size() != framesBefore || playback.streamEnded || playback.decoderFailed;
        }

        if (wakePictureWaiter)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        setReturnS32(ctx, static_cast<int32_t>(copied));
    }

    void sceMpegAddCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t callbackType = getRegU32(ctx, 5);
        const uint32_t callbackFunc = getRegU32(ctx, 6);
        const uint32_t callbackData = getRegU32(ctx, 7);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        (void)getPlaybackState(mpegAddr);

        const uint32_t handle = g_mpeg_stub_state.nextCallbackHandle++;
        g_mpeg_stub_state.callbacksByMpeg[mpegAddr].push_back(
            MpegRegisteredCallback{callbackType, 0u, callbackFunc, callbackData, handle, false});

        setReturnU32(ctx, handle);
    }

    // sceMpegAddStrCallback(sceMpeg *mp, sceMpegStrType type, int id, sceMpegCallback func,
    //                       void *data): the fifth argument is in t0 (game's copy 0x11a1f0).
    PS2_STUB_TYPED(int32_t, sceMpegAddStrCallback, ps2_abi::gptr mpegAddr, uint32_t streamType, uint32_t streamId,
                   ps2_abi::gptr callbackFunc, ps2_abi::gptr callbackData)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        (void)getPlaybackState(mpegAddr);
        const uint32_t handle = g_mpeg_stub_state.nextCallbackHandle++;
        g_mpeg_stub_state.callbacksByMpeg[mpegAddr].push_back(
            MpegRegisteredCallback{streamType, streamId, callbackFunc, callbackData, handle, true});
        return 0;
    }

    void sceMpegAddStrCallback_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t streamType = getRegU32(ctx, 5);
        const uint32_t streamId = getRegU32(ctx, 6);
        const uint32_t callbackFunc = getRegU32(ctx, 7);
        const uint32_t callbackData = readAbiArg4(rdram, ctx);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        (void)getPlaybackState(mpegAddr);
        const uint32_t handle = g_mpeg_stub_state.nextCallbackHandle++;
        g_mpeg_stub_state.callbacksByMpeg[mpegAddr].push_back(
            MpegRegisteredCallback{streamType, streamId, callbackFunc, callbackData, handle, true});
        setReturnU32(ctx, 0u);
    }

    PS2_STUB_DISPATCH(sceMpegAddStrCallback)

    void sceMpegClearRefBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        (void)runtime;
        static const uint32_t kRefGlobalAddrs[] = {
            0x171800u, 0x17180Cu, 0x171818u, 0x171804u, 0x171810u, 0x17181Cu};
        for (uint32_t addr : kRefGlobalAddrs)
        {
            uint8_t *p = getMemPtr(rdram, addr);
            if (!p)
                continue;
            uint32_t ptr = *reinterpret_cast<uint32_t *>(p);
            if (ptr != 0u)
            {
                uint8_t *q = getMemPtr(rdram, ptr + 0x28u);
                if (q)
                    *reinterpret_cast<uint32_t *>(q) = 0u;
            }
        }
        setReturnU32(ctx, 1u);
    }

    static void mpegGuestWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (uint8_t *p = getMemPtr(rdram, addr))
            *reinterpret_cast<uint32_t *>(p) = value;
    }
    static void mpegGuestWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        if (uint8_t *p = getMemPtr(rdram, addr))
        {
            *reinterpret_cast<uint32_t *>(p) = static_cast<uint32_t>(value);
            *reinterpret_cast<uint32_t *>(p + 4) = static_cast<uint32_t>(value >> 32);
        }
    }

    void sceMpegCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t param_1 = getRegU32(ctx, 4); // a0
        const uint32_t param_2 = getRegU32(ctx, 5); // a1
        const uint32_t param_3 = getRegU32(ctx, 6); // a2

        const uint32_t uVar3 = (param_2 + 3u) & 0xFFFFFFFCu;
        const int32_t iVar2_signed = static_cast<int32_t>(param_3) - static_cast<int32_t>(uVar3 - param_2);

        if (iVar2_signed <= 0x117)
        {
            setReturnU32(ctx, 0u);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            getPlaybackState(param_1) = makeFreshPlaybackState();
        }

        const uint32_t puVar4 = uVar3 + 0x108u;
        const uint32_t innerSize = static_cast<uint32_t>(iVar2_signed) - 0x118u;

        mpegGuestWrite32(rdram, param_1 + 0x40, uVar3);

        const uint32_t a1_init = uVar3 + 0x118u;
        mpegGuestWrite32(rdram, puVar4 + 0x0, a1_init);
        mpegGuestWrite32(rdram, puVar4 + 0x4, innerSize);
        mpegGuestWrite32(rdram, puVar4 + 0x8, a1_init);
        mpegGuestWrite32(rdram, puVar4 + 0xC, a1_init);

        const uint32_t allocResult = runtime ? runtime->guestMalloc(0x600, 8u) : (uVar3 + 0x200u);
        mpegGuestWrite32(rdram, uVar3 + 0x44, allocResult);

        // param_1[0..2] = 0; param_1[4..0xe] = 0xffffffff/0 as per decompilation
        mpegGuestWrite32(rdram, param_1 + 0x00, 0);
        mpegGuestWrite32(rdram, param_1 + 0x04, 0);
        mpegGuestWrite32(rdram, param_1 + 0x08, 0);
        mpegGuestWrite64(rdram, param_1 + 0x10, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x18, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x20, 0);
        mpegGuestWrite64(rdram, param_1 + 0x28, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x30, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x38, 0);

        static const unsigned s_zeroOffsets[] = {
            0xB4, 0xB8, 0xBC, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD4, 0xD8, 0xDC, 0xE0, 0xE4, 0xE8, 0xF8,
            0x0C, 0x14, 0x2C, 0x34, 0x3C,
            0x48, 0xFC, 0x100, 0x104, 0x70, 0x90, 0xAC};
        for (unsigned off : s_zeroOffsets)
            mpegGuestWrite32(rdram, uVar3 + off, 0u);
        mpegGuestWrite64(rdram, uVar3 + 0x78, 0);
        mpegGuestWrite64(rdram, uVar3 + 0x88, 0);

        mpegGuestWrite64(rdram, uVar3 + 0xF0, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite32(rdram, uVar3 + 0x1C, 0x1209F8u);
        mpegGuestWrite32(rdram, uVar3 + 0x24, 0x120A08u);
        mpegGuestWrite32(rdram, uVar3 + 0xB0, 1u);
        mpegGuestWrite32(rdram, uVar3 + 0x9C, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x80, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x94, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x98, 0xFFFFFFFFu);

        mpegGuestWrite32(rdram, 0x1717BCu, param_1);

        static const uint32_t s_refValues[] = {
            0x171A50u, 0x171C58u, 0x171CC0u, 0x171D28u, 0x171D90u,
            0x171AB8u, 0x171B20u, 0x171B88u, 0x171BF0u};
        for (unsigned i = 0; i < 9u; ++i)
            mpegGuestWrite32(rdram, 0x171800u + i * 4u, s_refValues[i]);

        uint32_t setDynamicRet = a1_init;
        if (uint8_t *p = getMemPtr(rdram, puVar4 + 8))
            setDynamicRet = *reinterpret_cast<uint32_t *>(p);
        mpegGuestWrite32(rdram, puVar4 + 12, setDynamicRet);

        setReturnU32(ctx, setDynamicRet);
    }

    void sceMpegDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;

        const uint32_t mpegAddr = getRegU32(ctx, 4);
        if (mpegStrCallbackCheckEnabled())
        {
            std::cerr << "[MPEG:adpcm] at sceMpegDelete: parsed=" << g_mpegAdpcm.parsed
                      << " accepted=" << g_mpegAdpcm.accepted << " rolledback=" << g_mpegAdpcm.rolledBack
                      << " dropped=" << g_mpegAdpcm.dropped << " (starved " << g_mpegAdpcm.starved << ") hash="
                      << std::hex << g_mpegAdpcm.acceptedHash << std::dec << std::endl;
        }
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            g_mpeg_stub_state.callbacksByMpeg.erase(mpegAddr);
            g_mpeg_stub_state.playbackByMpeg.erase(mpegAddr);
        }
        runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_WAIT_DELETE);
        setReturnU32(ctx, 0u);
    }

    void sceMpegDemuxPss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t dataAddr = getRegU32(ctx, 5);
        const uint32_t byteCount = getRegU32(ctx, 6);
        const MpegDemuxInput input{dataAddr, byteCount, 0u, 0u};

        std::vector<MpegStreamCallbackEvent> callbackEvents;
        std::vector<uint32_t> completedMpegIds;
        size_t consumed = 0u;
        size_t decodedBefore = 0u;
        size_t decodedCount = 0u;
        uint32_t traceIdx = 0u;
        bool eofChanged = false;
        bool backpressured = false;
        const bool inNodataCallback = mpegInsideNodataCallback(runtime);
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            decodedBefore = playback.decodedFrames.size();
            if (playback.demuxRunInFlight)
            {
                noteDemuxWhileRunInFlight(mpegAddr);
            }
            backpressured = playback.demuxRunInFlight || mpegDemuxBackpressured(playback) ||
                            (inNodataCallback && mpegPlaybackEndedUnlocked(playback));
            if (!backpressured)
            {
                const MpegDemuxResult result = demuxInputUnlocked(mpegAddr, playback, rdram, input, callbackEvents);
                consumed = result.consumed;
                recordCdStreamBytesDemuxedUnlocked(result.parsed, completedMpegIds, eofChanged);
            }
            decodedCount = playback.decodedFrames.size();
            traceIdx = g_mpeg_stub_state.demuxPssTraceCount++;
        }

        if (backpressured)
        {
            if (traceIdx < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[MPEG:DemuxPss:BACKPRESSURE] mpeg=0x" << std::hex << mpegAddr << std::dec << " decoded=" << decodedCount << std::endl;
                });
            }
            setReturnS32(ctx, 0);
            return;
        }
        const bool currentStreamCompleted = std::find(completedMpegIds.begin(), completedMpegIds.end(), mpegAddr) != completedMpegIds.end();
        if (decodedCount != decodedBefore || eofChanged || currentStreamCompleted)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        for (const uint32_t completedMpegId : completedMpegIds)
        {
            if (completedMpegId != mpegAddr)
            {
                runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, completedMpegId, KE_OK);
            }
        }

        if (traceIdx < 32u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:DemuxPss] mpeg=0x" << std::hex << mpegAddr
                          << " data=0x" << dataAddr << std::dec
                          << " bytes=" << byteCount
                          << " consumed=" << consumed
                          << " decoded=" << decodedCount
                          << " callbacks=" << callbackEvents.size()
                          << std::endl;
            });
        }

        finishDemuxWithCallbacks(rdram, ctx, runtime, mpegAddr, input, std::move(callbackEvents), consumed,
                                 inNodataCallback, inNodataCallback && decodedCount == 0u);
    }

    // sceMpegDemuxPssRing after decoding, shared by the typed and the legacy form; returns v0 (the
    // stream callbacks it dispatches run before the return value is written, in both forms).
    static int32_t mpegDemuxPssRingBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t mpegAddr,
                                        uint32_t dataAddr, uint32_t availableBytes, uint32_t ringBaseAddr,
                                        uint32_t ringSize)
    {
        static std::atomic<uint32_t> s_demuxRingEntryCount{0u};
        const uint32_t entryIdx = s_demuxRingEntryCount.fetch_add(1u, std::memory_order_relaxed);
        if (entryIdx < 4u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:DemuxPssRing:ENTER] call #" << entryIdx
                          << " pc=0x" << std::hex << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            });
        }

        const MpegDemuxInput input{dataAddr, availableBytes, ringBaseAddr, ringSize};
        std::vector<MpegStreamCallbackEvent> callbackEvents;
        std::vector<uint32_t> completedMpegIds;
        size_t consumed = 0u;
        size_t decodedBefore = 0u;
        size_t decodedCount = 0u;
        uint32_t traceIdx = 0u;
        bool eofChanged = false;
        bool backpressured = false;
        const bool inNodataCallback = mpegInsideNodataCallback(runtime);
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            decodedBefore = playback.decodedFrames.size();
            if (playback.demuxRunInFlight)
            {
                noteDemuxWhileRunInFlight(mpegAddr);
            }
            backpressured = playback.demuxRunInFlight || mpegDemuxBackpressured(playback) ||
                            (inNodataCallback && mpegPlaybackEndedUnlocked(playback));
            if (!backpressured)
            {
                const MpegDemuxResult result = demuxInputUnlocked(mpegAddr, playback, rdram, input, callbackEvents);
                consumed = result.consumed;
                recordCdStreamBytesDemuxedUnlocked(result.parsed, completedMpegIds, eofChanged);
            }
            decodedCount = playback.decodedFrames.size();
            traceIdx = g_mpeg_stub_state.demuxRingTraceCount++;
        }

        if (backpressured)
        {
            if (traceIdx < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[MPEG:DemuxPssRing:BACKPRESSURE] mpeg=0x" << std::hex << mpegAddr
                              << std::dec << " decoded=" << decodedCount
                              << " avail=" << availableBytes << std::endl;
                });
            }
            return 0;
        }
        const bool currentStreamCompleted = std::find(completedMpegIds.begin(), completedMpegIds.end(), mpegAddr) != completedMpegIds.end();
        if (decodedCount != decodedBefore || eofChanged || currentStreamCompleted)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        for (const uint32_t completedMpegId : completedMpegIds)
        {
            if (completedMpegId != mpegAddr)
            {
                runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, completedMpegId, KE_OK);
            }
        }

        if (traceIdx < 32u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:DemuxPssRing] mpeg=0x" << std::hex << mpegAddr
                          << " data=0x" << dataAddr
                          << " ring=0x" << ringBaseAddr << std::dec
                          << " avail=" << availableBytes
                          << " consumed=" << consumed
                          << " decoded=" << decodedCount
                          << " callbacks=" << callbackEvents.size()
                          << std::endl;
            });
        }

        finishDemuxWithCallbacks(rdram, ctx, runtime, mpegAddr, input, std::move(callbackEvents), consumed,
                                 inNodataCallback, inNodataCallback && decodedCount == 0u);
        return static_cast<int32_t>(consumed);
    }

    // sceMpegDemuxPssRing(sceMpeg *mp, u_char *data, int avail, u_char *ring, int ringSize): the
    // ring size is the fifth argument, in t0 (game's copy 0x119df0).
    PS2_STUB_TYPED(int32_t, sceMpegDemuxPssRing, ps2_abi::gptr mpegAddr, ps2_abi::gptr dataAddr, uint32_t availableBytes,
                   ps2_abi::gptr ringBaseAddr, uint32_t ringSize)
    {
        return mpegDemuxPssRingBody(rdram, ctx, runtime, mpegAddr, dataAddr, availableBytes, ringBaseAddr, ringSize);
    }

    void sceMpegDemuxPssRing_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t dataAddr = getRegU32(ctx, 5);
        const uint32_t availableBytes = getRegU32(ctx, 6);
        const uint32_t ringBaseAddr = getRegU32(ctx, 7);
        const uint32_t ringSize = readAbiArg4(rdram, ctx);

        setReturnS32(ctx, mpegDemuxPssRingBody(rdram, ctx, runtime, mpegAddr, dataAddr, availableBytes, ringBaseAddr,
                                               ringSize));
    }

    PS2_STUB_DISPATCH(sceMpegDemuxPssRing)

    void sceMpegDispCenterOffX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegDispCenterOffY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegDispHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).height);
    }

    void sceMpegDispWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).width);
    }

    void sceMpegGetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).decodeMode);
    }

    void sceMpegGetPicture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t imageAddr = getRegU32(ctx, 5);
        uint32_t width = kStubMovieWidth;
        uint32_t height = kStubMovieHeight;
        uint32_t frameCount = 0u;
        bool haveFrame = false;
        bool sequenceEnded = false;
        MpegDecodedFrame frame;
        {
            std::unique_lock<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            // libmpeg calls sceMpegCbBackground while it waits for the IPU, i.e. during every
            // picture's decode; games feed the demuxer from it (SRWZ's 0x18bcd0 feeds for a
            // picture-size-dependent number of timer-0 ticks, the hardware's main feed). Once
            // per GetPicture call here, before anything else.
            MpegRegisteredCallback background{};
            if (!playback.backgroundCalled && playback.sawInput && !playback.streamEnded &&
                !playback.decoderFailed && mpegBackgroundCallEnabled() &&
                findMpegCallbackUnlocked(mpegAddr, kMpegCbBackground, background))
            {
                playback.backgroundCalled = true;
                lock.unlock();
                if (invokeMpegNodataCallback(rdram, ctx, runtime, mpegAddr, background, kMpegCbBackground))
                {
                    return; // not reached: invokeCurrent transfers to the dispatcher
                }
                lock.lock();
            }
            if (playback.decodedFrames.empty() &&
                !g_mpeg_stub_state.currentCdStreamEofSeen &&
                !playback.streamEnded &&
                !playback.decoderFailed)
            {
                // libmpeg never parks here: _getpic decodes from the data already given to
                // the IPU and, when that runs dry, calls the game's sceMpegCbNodata callback
                // from inside GetPicture to get more (SRWZ's 0x18bb50 reads the CD stream and
                // demuxes, blocking, until there is data). Parking instead deadlocks a player
                // whose only feeder is the loop GetPicture was called from (SRWZ feeds while
                // timer 0 < 500 after GetPicture; under host load that budget is gone by the
                // time it gets there and the queue runs dry).
                MpegRegisteredCallback nodata{};
                if (mpegNodataPullEnabled() &&
                    findMpegCallbackUnlocked(mpegAddr, kMpegCbNodata, nodata))
                {
                    MpegStubState &s = g_mpeg_stub_state;
                    if (s.cdStreamBytesDemuxed != s.nodataProgressMark)
                    {
                        s.nodataProgressMark = s.cdStreamBytesDemuxed;
                        s.nodataCallsWithoutProgress = 0u;
                    }
                    if (s.nodataCallsWithoutProgress < kMpegNodataMaxCallsWithoutProgress)
                    {
                        ++s.nodataCallsWithoutProgress;
                        ++s.nodataCallsTotal;
                        if (s.nodataTraceCount < 32u || (s.nodataCallsTotal % 1024u) == 0u)
                        {
                            ++s.nodataTraceCount;
                            std::cerr << "[MPEG:GetPicture:nodata] call=" << s.nodataCallsTotal
                                      << " mpeg=0x" << std::hex << mpegAddr
                                      << " cb=0x" << nodata.func << std::dec
                                      << " served=" << playback.picturesServed
                                      << " demuxed=" << s.cdStreamBytesDemuxed << std::endl;
                        }
                        lock.unlock();
                        if (invokeMpegNodataCallback(rdram, ctx, runtime, mpegAddr, nodata))
                        {
                            return; // not reached: invokeCurrent transfers to the dispatcher
                        }
                        lock.lock();
                    }
                    else if (s.nodataCallsWithoutProgress == kMpegNodataMaxCallsWithoutProgress)
                    {
                        ++s.nodataCallsWithoutProgress;
                        std::cerr << "[MPEG:GetPicture:nodata] " << kMpegNodataMaxCallsWithoutProgress
                                  << " no-data callbacks demuxed nothing: waiting for a demux instead, mpeg=0x"
                                  << std::hex << mpegAddr << std::dec << std::endl;
                    }
                }
                if (g_mpeg_stub_state.getPictureWaitTraceCount < 32u)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:GetPicture] waiting for frames, mpeg=0x" << std::hex << mpegAddr
                                  << std::dec << " ended=" << playback.streamEnded
                                  << " failed=" << playback.decoderFailed
                                  << " sawInput=" << playback.sawInput << std::endl;
                    });
                    ++g_mpeg_stub_state.getPictureWaitTraceCount;
                }
                lock.unlock();
                runtime->eeScheduler().waitExternal(
                    EeWaitReason::Mpeg,
                    kMpegPictureWaitType,
                    mpegAddr,
                    [rdram, runtime](R5900Context &resumeContext)
                    {
                        if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
                        {
                            return;
                        }
                        sceMpegGetPicture(rdram, &resumeContext, runtime);
                    },
                    EeResumeTag{EeResumeKind::MpegGetPicture, {}});
            }

            if (!playback.decodedFrames.empty())
            {
                const uint64_t currentTick = runtime->eeScheduler().currentVSyncTick();
                const uint64_t currentTickQ32 = currentTick << 32u;
                const MpegDecodedFrame &nextFrame = playback.decodedFrames.front();
                const uint64_t frameIntervalQ32 = decodedFrameIntervalQ32(playback, nextFrame);
                uint64_t presentationTargetQ32 = presentationTickForFrame(playback, nextFrame, currentTickQ32);

                if (currentTickQ32 > presentationTargetQ32 && currentTickQ32 - presentationTargetQ32 >= frameIntervalQ32)
                {
                    const uint64_t correction = currentTickQ32 - presentationTargetQ32;
                    presentationTargetQ32 = currentTickQ32;
                    if (nextFrame.pts90k >= 0 && playback.firstPresentedPts90k >= 0)
                    {
                        playback.ptsPresentationBaseTickQ32 += correction;
                    }
                    playback.nextPictureTickQ32 = currentTickQ32;
                }

                if (currentTickQ32 < presentationTargetQ32)
                {
                    const uint64_t eligibleTick = (presentationTargetQ32 + kPictureClockOne - 1u) >> 32u;
                    lock.unlock();
                    runtime->eeScheduler().waitVSync(
                        eligibleTick - 1u,
                        -1,
                        [rdram, runtime](R5900Context &resumeContext)
                        {
                            if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
                            {
                                return;
                            }
                            sceMpegGetPicture(rdram, &resumeContext, runtime);
                        },
                        EeResumeTag{EeResumeKind::MpegGetPicture, {}});
                }

                frame = std::move(playback.decodedFrames.front());
                playback.decodedFrames.pop_front();
                playback.width = static_cast<uint32_t>(frame.width);
                playback.height = static_cast<uint32_t>(frame.height);
                width = playback.width;
                height = playback.height;
                frameCount = playback.picturesServed;
                playback.picturesServed += 1u;
                // PS2X_MPEG_HASH=N: log an FNV-1a hash of every Nth served picture (and its
                // VBlank tick), to check that two runs show the same pictures whatever the timing.
                static const uint32_t s_hashEvery = []
                {
                    const char *v = std::getenv("PS2X_MPEG_HASH");
                    return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0u;
                }();
                if (s_hashEvery != 0u && (frameCount % s_hashEvery) == 0u)
                {
                    uint64_t h = 1469598103934665603ull;
                    for (const uint8_t b : frame.rgba)
                    {
                        h = (h ^ b) * 1099511628211ull;
                    }
                    std::cout << "[MPEG:hash] picture=" << frameCount << " tick="
                              << (runtime ? runtime->eeScheduler().currentVSyncTick() : 0u)
                              << " size=" << frame.width << "x" << frame.height << " fnv=" << std::hex << h
                              << std::dec << std::endl;
                }
                playback.nextPictureTickQ32 = presentationTargetQ32 + frameIntervalQ32;
                playback.presentationEndTickQ32 = playback.nextPictureTickQ32;
                haveFrame = true;
                if (g_mpeg_stub_state.pictureTraceCount < 32u)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:GetPicture:FRAME] mpeg=0x" << std::hex << mpegAddr
                                  << std::dec << " generation=" << g_mpeg_stub_state.cdStreamGeneration
                                  << " frame=" << frameCount
                                  << " queued=" << playback.decodedFrames.size()
                                  << " size=" << width << "x" << height << std::endl;
                    });
                    ++g_mpeg_stub_state.pictureTraceCount;
                }
            }
            else
            {
                width = playback.width;
                height = playback.height;
                frameCount = playback.picturesServed;
                // libmpeg's _getpic (0x115358 in SRWZ) clears sys->isEnd on entry and sets it
                // when the decoder reaches the sequence end instead of a picture; games read
                // it inline (SRWZ's sceMpegIsEnd at 0x115d18 is `return *mpeg->sys`) to leave
                // their playback loop. Here: the stream's end has been demuxed (program end
                // code or CD EOF; the decoder is flushed then), every decoded picture has been
                // served and the last one has had its display time.
                const uint64_t currentTickQ32 = runtime != nullptr
                                                    ? (runtime->eeScheduler().currentVSyncTick() << 32u)
                                                    : std::numeric_limits<uint64_t>::max();
                const uint64_t presentationEnd = playback.presentationEndTickQ32;
                const bool presentationComplete =
                    presentationEnd == std::numeric_limits<uint64_t>::max() || currentTickQ32 >= presentationEnd;
                sequenceEnded = playback.streamEnded && playback.sawInput && presentationComplete;
            }
            // This call returns to the game now (every wait above transferred instead).
            playback.backgroundCalled = false;
        }

        mpegGuestWrite32(rdram, mpegAddr + 0x00u, width);
        mpegGuestWrite32(rdram, mpegAddr + 0x04u, height);
        mpegGuestWrite32(rdram, mpegAddr + 0x08u, frameCount);

        if (uint8_t *base = getMemPtr(rdram, mpegAddr))
        {
            const uint32_t iVar1 = *reinterpret_cast<uint32_t *>(base + 0x40);
            if (iVar1 != 0u) // sys not set up (no sceMpegCreate yet): leave guest memory alone
            {
                mpegGuestWrite32(rdram, iVar1 + 0x00u, sequenceEnded ? 1u : 0u); // sys->isEnd
                mpegGuestWrite32(rdram, iVar1 + 0xb0u, 1u);
                mpegGuestWrite32(rdram, iVar1 + 0xd8u, (getRegU32(ctx, 5) & 0x0FFFFFFFu) | 0x20000000u);
                mpegGuestWrite32(rdram, iVar1 + 0xe4u, getRegU32(ctx, 6));
                mpegGuestWrite32(rdram, iVar1 + 0xdcu, 0u);
                mpegGuestWrite32(rdram, iVar1 + 0xe0u, 0u);
            }
        }

        if (haveFrame)
        {
            writeDecodedFrameToGuest(rdram, imageAddr, frame);
        }
        else if (frameCount == 0u)
        {
            writeBlankMpegFrame(rdram, imageAddr, width, height);
        }

        setReturnS32(ctx, 0);
    }

    void sceMpegGetPictureRAW8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceMpegGetPictureRAW8", rdram, ctx, runtime);
    }

    void sceMpegGetPictureRAW8xy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceMpegGetPictureRAW8xy", rdram, ctx, runtime);
    }

    void sceMpegInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        const uint64_t cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
        const uint64_t cdStreamBytesProduced = g_mpeg_stub_state.cdStreamBytesProduced;
        const uint64_t cdStreamBytesDemuxed = g_mpeg_stub_state.cdStreamBytesDemuxed;
        const bool cdStreamEofPending = g_mpeg_stub_state.cdStreamEofPending;
        const bool currentCdStreamEofSeen = g_mpeg_stub_state.currentCdStreamEofSeen;
        resetMpegStubStateUnlocked();
        g_mpeg_stub_state.initialized = true;
        g_mpeg_stub_state.cdStreamGeneration = cdStreamGeneration;
        g_mpeg_stub_state.cdStreamBytesProduced = cdStreamBytesProduced;
        g_mpeg_stub_state.cdStreamBytesDemuxed = cdStreamBytesDemuxed;
        g_mpeg_stub_state.cdStreamEofPending = cdStreamEofPending;
        g_mpeg_stub_state.currentCdStreamEofSeen = currentCdStreamEofSeen;
        setReturnU32(ctx, 0u);
    }

    void sceMpegIsEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t mpegAddr = getRegU32(ctx, 4);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        // Only the producer/demux EOF is authoritative. A sequence_end_code can
        // be observed while more PSS data is still buffered, and a decoder
        // failure before producer EOF may still recover on a later sequence.
        const bool producerEnded =
            g_mpeg_stub_state.currentCdStreamEofSeen &&
            playback.cdStreamGeneration == g_mpeg_stub_state.cdStreamGeneration;
        const bool ended = producerEnded &&
                           (playback.streamEnded || (playback.decoderFailed && playback.sawInput));
        const uint64_t presentationEnd = playback.presentationEndTickQ32;
        const uint64_t currentTickQ32 = runtime != nullptr
                                            ? (runtime->eeScheduler().currentVSyncTick() << 32u)
                                            : std::numeric_limits<uint64_t>::max();
        const bool presentationComplete =
            presentationEnd == std::numeric_limits<uint64_t>::max() ||
            currentTickQ32 >= presentationEnd;

        if (g_mpeg_stub_state.isEndTraceCount < 16u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:IsEnd] mpeg=0x" << std::hex << mpegAddr << std::dec
                          << " ended=" << ended
                          << " producerEof=" << producerEnded
                          << " seqEnd=" << playback.sawSequenceEnd
                          << " streamEnded=" << playback.streamEnded
                          << " presentationComplete=" << presentationComplete
                          << " frames=" << playback.decodedFrames.size()
                          << " sawInput=" << playback.sawInput << std::endl;
            });
            ++g_mpeg_stub_state.isEndTraceCount;
        }

        setReturnS32(ctx, (ended && playback.decodedFrames.empty() && presentationComplete) ? 1 : 0);
    }

    void sceMpegIsRefBuffEmpty(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        const MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        setReturnS32(ctx, playback.decodedFrames.empty() ? 1 : 0);
    }

    void sceMpegReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t param_1 = getRegU32(ctx, 4);
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(param_1);
            MpegPlaybackState resetState = makeFreshPlaybackStatePreservingConfig(playback);
            if (playback.streamEnded || playback.decoderFailed)
            {
                resetState.sawInput = true;
                resetState.streamEnded = true;
                resetState.cdStreamGeneration = playback.cdStreamGeneration;
            }
            playback = std::move(resetState);
        }
        uint8_t *base = getMemPtr(rdram, param_1);
        if (!base)
        {
            return;
        }
        uint32_t inner = *reinterpret_cast<uint32_t *>(base + 0x40);
        if (inner == 0u)
            return;
        mpegGuestWrite32(rdram, param_1 + 0x00u, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x04u, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x08u, 0u);
        mpegGuestWrite32(rdram, inner + 0x00, 0u);
        mpegGuestWrite32(rdram, inner + 0x04, 0u);
        mpegGuestWrite32(rdram, inner + 0x08, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x08, 0u);
        mpegGuestWrite32(rdram, inner + 0x80, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, inner + 0xAC, 0u);
        mpegGuestWrite32(rdram, 0x171904u, 0u);
    }

    void sceMpegResetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t mode = getRegU32(ctx, 5);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        getPlaybackState(mpegAddr).decodeMode = mode;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetImageBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t imageBufferAddr = getRegU32(ctx, 5);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        getPlaybackState(mpegAddr).imageBufferAddr = imageBufferAddr;
        setReturnS32(ctx, 0);
    }
}

namespace ps2_stubs::snapshot
{
    namespace
    {
        void saveFrame(ps2x::StateWriter &w, const MpegDecodedFrame &frame)
        {
            w.i32(frame.width);
            w.i32(frame.height);
            w.i32(frame.repeatPict);
            w.i64(frame.pts90k);
            w.podVector(frame.rgba);
        }

        void loadFrame(ps2x::StateReader &r, MpegDecodedFrame &frame)
        {
            frame.width = r.i32();
            frame.height = r.i32();
            frame.repeatPict = r.i32();
            frame.pts90k = r.i64();
            r.podVector(frame.rgba);
        }

        void savePlayback(ps2x::StateWriter &w, const MpegPlaybackState &p)
        {
            w.u32(p.picturesServed);
            w.u32(p.width);
            w.u32(p.height);
            w.u32(p.decodeMode);
            w.u32(p.imageBufferAddr);
            w.boolean(p.sawInput);
            w.boolean(p.sawSequenceEnd);
            w.boolean(p.streamEnded);
            w.boolean(p.decoderFailed);
            w.u64(p.cdStreamGeneration);
            w.boolean(p.waitingForVideoSequenceHeader);
            w.podVector(p.videoSequenceSyncBuffer);
            w.podVector(p.pssBuffer);
            w.podVector(p.pssGuestAddrs);
            w.u64(p.pssBufferStartPos);
            w.podVector(p.demuxReplay.bytes);
            w.size(p.demuxReplay.events.size());
            for (const MpegStreamCallbackEvent &event : p.demuxReplay.events)
            {
                w.u32(event.mpegAddr);
                w.u32(event.streamType);
                w.u32(event.dataAddr);
                w.u32(event.len);
                w.u64(event.pts);
                w.u64(event.dts);
                w.podVector(event.callbacks);
                w.u64(event.packetPos);
                w.u64(event.payloadPos);
                w.i64(event.packetOffset);
                w.u32(event.payloadOffset);
            }
            w.size(p.decodedFrames.size());
            for (const MpegDecodedFrame &frame : p.decodedFrames)
                saveFrame(w, frame);
            w.boolean(p.decoder != nullptr);
            w.u8(p.frameRateCode);
            w.u8(p.frameRateExtensionN);
            w.u8(p.frameRateExtensionD);
            w.boolean(p.hasFrameRateExtension);
            w.podVector(p.videoTimingScanBuffer);
            w.u64(p.pictureIntervalQ32);
            w.u64(p.nextPictureTickQ32);
            w.u64(p.presentationEndTickQ32);
            w.i64(p.firstPresentedPts90k);
            w.u64(p.ptsPresentationBaseTickQ32);

            w.size(p.replayLog.size());
            for (const MpegDecoderCall &call : p.replayLog)
            {
                w.podVector(call.data);
                w.i64(call.pts90k);
                w.i64(call.dts90k);
                w.boolean(call.flush);
            }
            w.podSequence(p.replayAnchors);
            w.size(p.replayBytes);
            w.boolean(p.replayValid);
            w.u64(p.framesProduced);
            w.u64(p.framesProducedAtLogStart);
            w.u64(p.anchorFramesProduced);
            w.i64(p.lastProducedPts90k);
            w.u64(p.lastProducedFingerprint);
        }

        // Feeds the replay log to a fresh decoder; returns a one-line report.
        std::string replayDecoder(MpegPlaybackState &p)
        {
            p.decoder = std::make_unique<MpegFfmpegDecoder>();
            if (!p.replayValid)
            {
                p.decoder.reset();
                p.waitingForVideoSequenceHeader = true;
                p.videoSequenceSyncBuffer.clear();
                return "replay log had been dropped (stream without repeated sequence headers): "
                       "decoding resumes at the next sequence header";
            }
            if (p.replayLog.empty())
            {
                return "decoder restarted (nothing fed yet)";
            }
            std::deque<MpegDecodedFrame> replayed;
            uint64_t replayedCount = 0u;
            for (const MpegDecoderCall &call : p.replayLog)
            {
                const size_t before = replayed.size();
                if (call.flush)
                {
                    p.decoder->flush(replayed);
                }
                else if (!p.decoder->feed(call.data.data(), call.data.size(), replayed, call.pts90k, call.dts90k))
                {
                    p.decoder.reset();
                    p.waitingForVideoSequenceHeader = true;
                    p.videoSequenceSyncBuffer.clear();
                    return "decoder replay failed: decoding resumes at the next sequence header";
                }
                replayedCount += replayed.size() - before;
                // Only the newest picture is compared: bound the memory.
                while (replayed.size() > 1u)
                    replayed.pop_front();
            }
            const uint64_t originalCount = p.framesProduced - p.framesProducedAtLogStart;
            std::ostringstream out;
            out << "decoder replayed " << p.replayLog.size() << " calls (" << p.replayBytes << " bytes): "
                << replayedCount << " pictures, the original decoder produced " << originalCount;
            if (originalCount == 0u)
            {
                out << "; no picture produced yet";
            }
            else if (!replayed.empty() &&
                     replayed.back().pts90k == p.lastProducedPts90k &&
                     mpegFrameFingerprint(replayed.back()) == p.lastProducedFingerprint)
            {
                out << "; last picture matches";
            }
            else
            {
                out << "; WARNING: last picture differs (pts " << (replayed.empty() ? -2 : replayed.back().pts90k)
                    << " vs " << p.lastProducedPts90k << ")";
            }
            return out.str();
        }

        void loadPlayback(ps2x::StateReader &r, MpegPlaybackState &p, uint32_t mpegAddr)
        {
            p = MpegPlaybackState{};
            p.picturesServed = r.u32();
            p.width = r.u32();
            p.height = r.u32();
            p.decodeMode = r.u32();
            p.imageBufferAddr = r.u32();
            p.sawInput = r.boolean();
            p.sawSequenceEnd = r.boolean();
            p.streamEnded = r.boolean();
            p.decoderFailed = r.boolean();
            p.cdStreamGeneration = r.u64();
            p.waitingForVideoSequenceHeader = r.boolean();
            r.podVector(p.videoSequenceSyncBuffer);
            r.podVector(p.pssBuffer);
            r.podVector(p.pssGuestAddrs);
            p.pssBufferStartPos = r.u64();
            r.podVector(p.demuxReplay.bytes);
            p.demuxReplay.events.resize(r.size());
            for (MpegStreamCallbackEvent &event : p.demuxReplay.events)
            {
                event.mpegAddr = r.u32();
                event.streamType = r.u32();
                event.dataAddr = r.u32();
                event.len = r.u32();
                event.pts = r.u64();
                event.dts = r.u64();
                r.podVector(event.callbacks);
                event.packetPos = r.u64();
                event.payloadPos = r.u64();
                event.packetOffset = r.i64();
                event.payloadOffset = r.u32();
            }
            const size_t frames = r.size();
            for (size_t i = 0; i < frames; ++i)
            {
                MpegDecodedFrame frame;
                loadFrame(r, frame);
                p.decodedFrames.push_back(std::move(frame));
            }
            const bool hadDecoder = r.boolean();
            p.frameRateCode = r.u8();
            p.frameRateExtensionN = r.u8();
            p.frameRateExtensionD = r.u8();
            p.hasFrameRateExtension = r.boolean();
            r.podVector(p.videoTimingScanBuffer);
            p.pictureIntervalQ32 = r.u64();
            p.nextPictureTickQ32 = r.u64();
            p.presentationEndTickQ32 = r.u64();
            p.firstPresentedPts90k = r.i64();
            p.ptsPresentationBaseTickQ32 = r.u64();

            const size_t calls = r.size();
            p.replayLog.resize(calls);
            for (MpegDecoderCall &call : p.replayLog)
            {
                r.podVector(call.data);
                call.pts90k = r.i64();
                call.dts90k = r.i64();
                call.flush = r.boolean();
            }
            r.podSequence(p.replayAnchors);
            p.replayBytes = static_cast<size_t>(r.u64());
            p.replayValid = r.boolean();
            p.framesProduced = r.u64();
            p.framesProducedAtLogStart = r.u64();
            p.anchorFramesProduced = r.u64();
            p.lastProducedPts90k = r.i64();
            p.lastProducedFingerprint = r.u64();

            if (hadDecoder)
            {
                // The replay logs through the decoder's own channel: build the report first.
                const std::string report = replayDecoder(p);
                std::cerr << "[snapshot:mpeg] mpeg=0x" << std::hex << mpegAddr << std::dec << " " << report << std::endl;
            }
        }
    }

    void saveMpeg(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        const MpegStubState &s = g_mpeg_stub_state;
        w.boolean(s.initialized);
        w.u32(s.nextCallbackHandle);
        w.u64(s.cdStreamGeneration);
        w.u64(s.cdStreamBytesProduced);
        w.u64(s.cdStreamBytesDemuxed);
        w.boolean(s.cdStreamEofPending);
        w.boolean(s.currentCdStreamEofSeen);
        w.sortedMap(s.callbacksByMpeg, [&w](const std::vector<MpegRegisteredCallback> &callbacks)
                    { w.podVector(callbacks); });
        w.sortedMap(s.playbackByMpeg, [&w](const MpegPlaybackState &playback)
                    { savePlayback(w, playback); });
    }

    void loadMpeg(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        (void)runtime;
        snapshotLoadSupport(r);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        resetMpegStubStateUnlocked();
        MpegStubState &s = g_mpeg_stub_state;
        s.initialized = r.boolean();
        s.nextCallbackHandle = r.u32();
        s.cdStreamGeneration = r.u64();
        s.cdStreamBytesProduced = r.u64();
        s.cdStreamBytesDemuxed = r.u64();
        s.cdStreamEofPending = r.boolean();
        s.currentCdStreamEofSeen = r.boolean();
        r.sortedMap(s.callbacksByMpeg, [&r](std::vector<MpegRegisteredCallback> &callbacks)
                    { r.podVector(callbacks); });
        const size_t count = r.size();
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t mpegAddr = r.u32();
            loadPlayback(r, s.playbackByMpeg[mpegAddr], mpegAddr);
        }
    }

    std::function<void(R5900Context &)> makeMpegGetPictureResume(PS2Runtime &runtime)
    {
        PS2Runtime *rt = &runtime;
        uint8_t *rdram = runtime.memory().getRDRAM();
        return [rdram, rt](R5900Context &resumeContext)
        {
            if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
            {
                return;
            }
            sceMpegGetPicture(rdram, &resumeContext, rt);
        };
    }

    std::function<void(const R5900Context &, R5900Context &)> makeMpegNodataComplete(PS2Runtime &runtime, uint32_t cbDataAddr)
    {
        return makeNodataComplete(runtime.memory().getRDRAM(), &runtime, cbDataAddr);
    }

    bool mpegReplayHealthy(std::string *why, bool activeOnly)
    {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        for (const auto &[addr, playback] : g_mpeg_stub_state.playbackByMpeg)
        {
            if (!playback.decoder || playback.replayValid)
                continue;
            if (activeOnly && playback.streamEnded)
                continue;
            if (why)
            {
                std::ostringstream out;
                out << "MPEG playback 0x" << std::hex << addr << std::dec
                    << (playback.streamEnded ? " (ended)" : " (still playing)")
                    << " has its decoder replay log dropped (over " << (mpegReplayLimitBytes() >> 20)
                    << " MB): a restore could not decode its pictures; PS2X_MPEG_REPLAY_MB raises the limit";
                *why = out.str();
            }
            return false;
        }
        return true;
    }
}
