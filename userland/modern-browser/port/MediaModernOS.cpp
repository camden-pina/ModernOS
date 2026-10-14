// Native CPU media playback. Network bytes still go through WebCore's real
// media resource loader (CSP/CORS/redirects) and the certificate-checking broker.
#include "config.h"
#include "MediaModernOS.h"
#include "ContentType.h"
#include "DestinationColorSpace.h"
#include "GraphicsContext.h"
#include "MediaPlayer.h"
#include "MediaPlayerPrivate.h"
#include "NativeImage.h"
#include "PlatformMediaResourceLoader.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include "MediaSourcePrivate.h"
#include "MediaSourcePrivateClient.h"
#include "SourceBufferPrivate.h"
#include "SourceBufferPrivateClient.h"
#include "MediaSample.h"
#include "MediaDescription.h"
#include "AudioTrackPrivate.h"
#include "VideoTrackPrivate.h"
#include "VideoFrameMetadata.h"
#include "Logging.h"
#include "../../include/audio_protocol.h"
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkData.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/cpu.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

}

namespace ModernOS {
using namespace WebCore;
using namespace WTF;
static constexpr size_t rangeSize = 1024 * 1024;
static constexpr size_t maximumRange = 4 * 1024 * 1024;
static AudioSink audioSink;

void setAudioSink(AudioSink sink) {
    audioSink = std::move(sink);
}

static AudioQueue audioQueue;

void setAudioQueue(AudioQueue queue) {
    audioQueue = std::move(queue);
}

struct VideoPixels {
    std::vector<uint8_t> bytes;
    double time = 0, duration = 1.0 / 30;
    int width = 0, height = 0, stride = 0;

    void allocate() {
        // SIMD conversion rounds stores up to vector boundaries. Keep row
        // padding and a final guard inside the owned allocation, including
        // for common 426-pixel-wide streams. Skia uses this actual stride.
        stride = (width * 4 + 63) & ~63;
        bytes.resize(static_cast<size_t>(stride) * height + AV_INPUT_BUFFER_PADDING_SIZE);
    }
};

struct DecodeState {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> cancelled{false};
    bool fetched = false, failedFetch = false, metadata = false, ended = false;
    int error = 0, width = 0, height = 0;
    bool audio = false;
    double duration = 0, seek = -1, sought = -1;
    int64_t length = -1, requestedOffset = 0;
    std::vector<uint8_t> fetchedBytes;
    std::deque<VideoPixels> frames;
    struct AudioPixels {
        std::vector<int16_t> samples;
        double time;
    };

    std::deque<AudioPixels> sound;
    size_t soundFrames = 0;
    std::function<void(int64_t)> fetch;
    std::atomic<double> clock{0};
    std::atomic<bool> progressing{false};
};

class RangeClient final : public PlatformMediaResourceClient {
  public:
    explicit RangeClient(std::shared_ptr<DecodeState> state, int64_t offset)
        : m_state(std::move(state)), m_offset(offset) {
    }

    void responseReceived(PlatformMediaResource &, const ResourceResponse &response,
                          CompletionHandler<void(ShouldContinuePolicyCheck)> &&done) final {
        bool valid = false;
        int64_t length = -1;
        if (response.httpStatusCode() == 206) {
            auto range = response.httpHeaderField(HTTPHeaderName::ContentRange).utf8();
            long long start = -1, end = -1, total = -1;
            char extra;
            valid =
                sscanf(range.data(), "bytes %lld-%lld/%lld%c", &start, &end, &total, &extra) == 3 &&
                start == m_offset && end >= start && total > end &&
                end - start < static_cast<int64_t>(maximumRange);
            if (valid) {
                length = total;
                m_expected = end - start + 1;
            }
        } else if (response.httpStatusCode() == 200 && !m_offset) {
            length = response.expectedContentLength();
            valid = length > 0 && length <= static_cast<int64_t>(maximumRange);
            if (valid)
                m_expected = length;
        }

        {
            std::lock_guard lock(m_state->mutex);
            if (valid && m_state->length >= 0 && m_state->length != length)
                valid = false;
            if (valid)
                m_state->length = length;
        }

        m_valid = valid;
        if (!valid)
            finish(true);
        done(valid ? ShouldContinuePolicyCheck::Yes : ShouldContinuePolicyCheck::No);
    }

    void dataReceived(PlatformMediaResource &, const SharedBuffer &buffer) final {
        if (!m_valid || m_state->cancelled)
            return;
        if (buffer.size() > maximumRange - m_bytes.size()) {
            finish(true);
            return;
        }
        auto data = buffer.span();
        m_bytes.insert(m_bytes.end(), data.begin(), data.end());
    }

    void loadFinished(PlatformMediaResource &, const NetworkLoadMetrics &) final {
        finish(!m_valid || static_cast<int64_t>(m_bytes.size()) != m_expected);
    }

    void loadFailed(PlatformMediaResource &, const ResourceError &) final {
        finish(true);
    }

    void accessControlCheckFailed(PlatformMediaResource &, const ResourceError &) final {
        finish(true);
    }

  private:
    std::shared_ptr<DecodeState> m_state;
    int64_t m_offset, m_expected = -1;
    bool m_valid = false, m_done = false;
    std::vector<uint8_t> m_bytes;

    void finish(bool failed) {
        if (m_done)
            return;
        m_done = true;
        std::lock_guard lock(m_state->mutex);
        m_state->failedFetch = failed;
        m_state->fetchedBytes = std::move(m_bytes);
        m_state->fetched = true;
        m_state->changed.notify_all();
    }
};

// FFmpeg has no network or filesystem protocols in this build. Its only I/O
// is these bounded, validated ranges, fetched asynchronously on the DOM thread.
class MediaInput {
  public:
    explicit MediaInput(std::shared_ptr<DecodeState> state) : state(std::move(state)) {
    }

    std::shared_ptr<DecodeState> state;
    int64_t position = 0, cachedOffset = -1;
    std::vector<uint8_t> cache;

    bool fetch(int64_t offset) {
        {
            std::lock_guard lock(state->mutex);
            state->fetched = false;
            state->failedFetch = false;
        }
        auto fetch = state->fetch;
        callOnMainThread([state = state, fetch = std::move(fetch), offset] {
            if (!state->cancelled)
                fetch(offset);
        });
        std::unique_lock lock(state->mutex);
        if (!state->changed.wait_for(lock, std::chrono::seconds(35),
                                     [&] { return state->fetched || state->cancelled; }))
            return false;
        if (state->cancelled || state->failedFetch)
            return false;
        cache = std::move(state->fetchedBytes);
        cachedOffset = offset;
        return !cache.empty();
    }

    static int read(void *opaque, uint8_t *out, int count) {
        auto &input = *static_cast<MediaInput *>(opaque);
        if (input.state->cancelled)
            return AVERROR_EXIT;
        {
            std::lock_guard lock(input.state->mutex);
            if (input.state->length >= 0 && input.position >= input.state->length)
                return AVERROR_EOF;
        }
        if (input.position < input.cachedOffset ||
            input.position >= input.cachedOffset + static_cast<int64_t>(input.cache.size())) {
            if (!input.fetch(input.position))
                return AVERROR(EIO);
        }
        size_t relative = input.position - input.cachedOffset;
        size_t n = std::min(static_cast<size_t>(count), input.cache.size() - relative);
        memcpy(out, input.cache.data() + relative, n);
        input.position += n;
        return static_cast<int>(n);
    }

    static int64_t seek(void *opaque, int64_t offset, int whence) {
        auto &input = *static_cast<MediaInput *>(opaque);
        if (input.state->cancelled)
            return AVERROR_EXIT;
        int64_t length;
        {
            std::lock_guard lock(input.state->mutex);
            length = input.state->length;
        }
        if (whence == AVSEEK_SIZE)
            return length >= 0 ? length : AVERROR(ENOSYS);
        whence &= ~AVSEEK_FORCE;
        int64_t base = whence == SEEK_SET   ? 0
                       : whence == SEEK_CUR ? input.position
                       : whence == SEEK_END ? length
                                            : -1;
        if (base < 0 || offset < -base || (offset > 0 && base > INT64_MAX - offset))
            return AVERROR(EINVAL);
        int64_t next = base + offset;
        if (length >= 0 && next > length)
            return AVERROR(EINVAL);
        input.position = next;
        return next;
    }
};

static void decodeVideo(const std::shared_ptr<DecodeState> &state) {
    MediaInput input(state);
    auto *buffer = static_cast<uint8_t *>(av_malloc(32768));
    AVIOContext *io = buffer ? avio_alloc_context(buffer, 32768, 0, &input, MediaInput::read,
                                                  nullptr, MediaInput::seek)
                             : nullptr;
    AVFormatContext *format = avformat_alloc_context();
    AVCodecContext *decoder = nullptr;
    AVCodecContext *audioDecoder = nullptr;
    AVFrame *audioFrame = av_frame_alloc();
    SwrContext *resampler = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    SwsContext *conversion = nullptr;
    int error = AVERROR(ENOMEM);
    auto interrupted = [](void *opaque) -> int {
        return static_cast<DecodeState *>(opaque)->cancelled;
    };

    if (io && format && frame && packet) {
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->probesize = 512 * 1024;
        format->max_analyze_duration = 2 * AV_TIME_BASE;
        format->max_streams = 8;
        format->interrupt_callback = {interrupted, state.get()};
        error = avformat_open_input(&format, nullptr, nullptr, nullptr);
        if (error >= 0)
            error = avformat_find_stream_info(format, nullptr);
        int streamIndex =
            error >= 0 ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
        int audioIndex =
            error >= 0 ? av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : -1;
        bool video = streamIndex >= 0;
        if (!video)
            streamIndex = audioIndex;
        if (streamIndex < 0 && error >= 0)
            error = AVERROR_STREAM_NOT_FOUND;
        if (error >= 0) {
            auto *stream = format->streams[streamIndex];
            const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
            decoder = video && codec ? avcodec_alloc_context3(codec) : nullptr;
            if (video && !decoder)
                error = AVERROR_DECODER_NOT_FOUND;
            else if (decoder) {
                error = avcodec_parameters_to_context(decoder, stream->codecpar);
                decoder->pkt_timebase = stream->time_base;
                decoder->thread_count = 2;
                decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
                if (error >= 0)
                    error = avcodec_open2(decoder, codec, nullptr);
            }

            if (error >= 0 && decoder &&
                (decoder->width <= 0 || decoder->height <= 0 || decoder->width > 1920 ||
                 decoder->height > 1080))
                error = AVERROR(EINVAL);
            if (error >= 0) {
                if (audioIndex >= 0) {
                    auto *audioStream = format->streams[audioIndex];
                    const auto *audioCodec = avcodec_find_decoder(audioStream->codecpar->codec_id);
                    audioDecoder = audioCodec ? avcodec_alloc_context3(audioCodec) : nullptr;
                    if (audioDecoder)
                        audioDecoder->pkt_timebase = audioStream->time_base;
                    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                    if (!audioDecoder || !audioFrame ||
                        avcodec_parameters_to_context(audioDecoder, audioStream->codecpar) < 0 ||
                        avcodec_open2(audioDecoder, audioCodec, nullptr) < 0 ||
                        audioDecoder->sample_rate < 8000 || audioDecoder->sample_rate > 192000 ||
                        audioDecoder->ch_layout.nb_channels < 1 ||
                        audioDecoder->ch_layout.nb_channels > 8 ||
                        swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, 48000,
                                            &audioDecoder->ch_layout, audioDecoder->sample_fmt,
                                            audioDecoder->sample_rate, 0, nullptr) < 0 ||
                        swr_init(resampler) < 0) {
                        error = AVERROR_DECODER_NOT_FOUND;
                    }
                }

                bool draining = false, seeking = false;
                double minimumTime = 0, lastTime = -1;
                auto decodeAudio = [&] {
                    if (!audioDecoder || error < 0)
                        return;
                    while (!state->cancelled &&
                           avcodec_receive_frame(audioDecoder, audioFrame) >= 0) {
                        int capacity = swr_get_out_samples(resampler, audioFrame->nb_samples);
                        if (capacity <= 0 || capacity > 16384) {
                            error = AVERROR_INVALIDDATA;
                            break;
                        }
                        DecodeState::AudioPixels sound;
                        sound.time = audioFrame->best_effort_timestamp == AV_NOPTS_VALUE
                                         ? 0
                                         : audioFrame->best_effort_timestamp *
                                               av_q2d(format->streams[audioIndex]->time_base);
                        sound.samples.resize(static_cast<size_t>(capacity) * 2);
                        uint8_t *output[] = {reinterpret_cast<uint8_t *>(sound.samples.data())};
                        int samples =
                            swr_convert(resampler, output, capacity,
                                        const_cast<const uint8_t **>(audioFrame->extended_data),
                                        audioFrame->nb_samples);
                        av_frame_unref(audioFrame);
                        if (samples < 0) {
                            error = samples;
                            break;
                        }
                        sound.samples.resize(static_cast<size_t>(samples) * 2);
                        if (sound.time + samples / 48000.0 < minimumTime)
                            continue;
                        std::unique_lock lock(state->mutex);
                        state->changed.wait(lock, [&] {
                            return state->cancelled || state->seek >= 0 ||
                                   state->soundFrames < 24000;
                        });
                        if (state->cancelled || state->seek >= 0)
                            break;
                        if (!video && seeking) {
                            state->sought = minimumTime;
                            seeking = false;
                        }
                        state->soundFrames += samples;
                        state->sound.push_back(std::move(sound));
                    }
                };

                double duration = format->duration != AV_NOPTS_VALUE
                                      ? format->duration / static_cast<double>(AV_TIME_BASE)
                                  : stream->duration != AV_NOPTS_VALUE
                                      ? stream->duration * av_q2d(stream->time_base)
                                      : 0;
                {
                    std::lock_guard lock(state->mutex);
                    state->width = decoder ? decoder->width : 0;
                    state->height = decoder ? decoder->height : 0;
                    state->audio = audioDecoder;
                    state->duration = duration;
                    state->metadata = true;
                }

                double frameDuration = stream->avg_frame_rate.num > 0
                                           ? av_q2d(av_inv_q(stream->avg_frame_rate))
                                           : 1.0 / 30;
                while (!state->cancelled && error >= 0) {
                    double seek;
                    {
                        std::unique_lock lock(state->mutex);
                        state->changed.wait(lock, [&] {
                            return state->cancelled || state->seek >= 0 ||
                                   (video ? state->frames.size() < 3 : state->soundFrames < 24000);
                        });
                        if (state->cancelled)
                            break;
                        seek = state->seek;
                        state->seek = -1;
                    }

                    if (seek >= 0) {
                        error =
                            av_seek_frame(format, streamIndex,
                                          static_cast<int64_t>(seek / av_q2d(stream->time_base)),
                                          AVSEEK_FLAG_BACKWARD);
                        if (error < 0)
                            break;
                        if (decoder)
                            avcodec_flush_buffers(decoder);
                        if (audioDecoder) {
                            avcodec_flush_buffers(audioDecoder);
                            swr_close(resampler);
                            swr_init(resampler);
                        }
                        draining = false;
                        seeking = true;
                        minimumTime = seek;
                        lastTime = -1;
                        std::lock_guard lock(state->mutex);
                        state->ended = false;
                    }

                    error = decoder    ? avcodec_receive_frame(decoder, frame)
                            : draining ? AVERROR_EOF
                                       : AVERROR(EAGAIN);
                    if (error == AVERROR(EAGAIN)) {
                        if (draining) {
                            error = AVERROR_INVALIDDATA;
                            break;
                        }
                        error = av_read_frame(format, packet);
                        if (error == AVERROR_EOF) {
                            draining = true;
                            error = decoder ? avcodec_send_packet(decoder, nullptr) : 0;
                            if (audioDecoder) {
                                avcodec_send_packet(audioDecoder, nullptr);
                                decodeAudio();
                            }
                        } else if (error >= 0) {
                            if (packet->stream_index == streamIndex && decoder)
                                error = avcodec_send_packet(decoder, packet);
                            else if (packet->stream_index == audioIndex && audioDecoder) {
                                error = avcodec_send_packet(audioDecoder, packet);
                                decodeAudio();
                            }
                            av_packet_unref(packet);
                        }

                        if (error < 0 && error != AVERROR(EAGAIN))
                            break;
                        continue;
                    }

                    if (error == AVERROR_EOF) {
                        error = 0;
                        std::unique_lock lock(state->mutex);
                        state->ended = true;
                        if (seeking) {
                            state->sought = minimumTime;
                            seeking = false;
                        }
                        state->changed.wait(lock,
                                            [&] { return state->cancelled || state->seek >= 0; });
                        continue;
                    }

                    if (error < 0)
                        break;
                    if (frame->width <= 0 || frame->height <= 0 || frame->width > 1920 ||
                        frame->height > 1080) {
                        error = AVERROR(EINVAL);
                        break;
                    }
                    double pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                     ? frame->best_effort_timestamp * av_q2d(stream->time_base)
                                     : lastTime + frameDuration;
                    lastTime = pts;
                    if (pts + frameDuration < minimumTime) {
                        av_frame_unref(frame);
                        continue;
                    }
                    if (state->progressing && pts + frameDuration < state->clock.load() - .05) {
                        av_frame_unref(frame);
                        continue;
                    }
                    VideoPixels pixels;
                    pixels.width = frame->width;
                    pixels.height = frame->height;
                    pixels.time = std::max(0.0, pts);
                    pixels.duration = frameDuration;
                    pixels.allocate();
                    conversion = sws_getCachedContext(conversion, frame->width, frame->height,
                                                      static_cast<AVPixelFormat>(frame->format),
                                                      pixels.width, pixels.height, AV_PIX_FMT_BGRA,
                                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!conversion) {
                        error = AVERROR(ENOMEM);
                        break;
                    }
                    uint8_t *planes[] = {pixels.bytes.data(), nullptr, nullptr, nullptr};
                    int strides[] = {pixels.stride, 0, 0, 0};
                    if (sws_scale(conversion, frame->data, frame->linesize, 0, frame->height,
                                  planes, strides) != pixels.height) {
                        error = AVERROR_INVALIDDATA;
                        break;
                    }
                    av_frame_unref(frame);
                    {
                        std::lock_guard lock(state->mutex);
                        if (state->seek >= 0)
                            continue;
                        if (seeking) {
                            state->sought = minimumTime;
                            seeking = false;
                        }
                        state->frames.push_back(std::move(pixels));
                    }
                }
            }
        }
    }

    if (error < 0 && !state->cancelled) {
        char description[256];
        av_strerror(error, description, sizeof(description));
        fprintf(stderr, "Native media decoder: %s\n", description);
        std::lock_guard lock(state->mutex);
        state->error = error;
    }

    sws_freeContext(conversion);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    swr_free(&resampler);
    av_frame_free(&audioFrame);
    avcodec_free_context(&audioDecoder);
    avformat_close_input(&format);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    } else
        av_free(buffer);
}

class MediaModernOS;
class NativeMediaSource final : public MediaSourcePrivate {
  public:
    static Ref<NativeMediaSource> create(MediaModernOS &, MediaSourcePrivateClient &,
                                         std::shared_ptr<DecodeState>);

    constexpr MediaPlatformType platformType() const final {
        return MediaPlatformType::ModernOS;
    }

    RefPtr<MediaPlayerPrivateInterface> player() const final;
    void setPlayer(MediaPlayerPrivateInterface *) final;

    MediaTime currentTime() const final {
        return MediaTime::createWithDouble(m_state->clock.load());
    }

    bool timeIsProgressing() const final {
        return m_state->progressing.load();
    }

    AddStatus addSourceBuffer(const ContentType &, const MediaSourceConfiguration &,
                              RefPtr<SourceBufferPrivate> &) final;

    void notifyActiveSourceBuffersChanged() final {
    }

    void durationChanged(const MediaTime &) final;
    void markEndOfStream(EndOfStreamStatus) final;

    void unmarkEndOfStream() final {
        MediaSourcePrivate::unmarkEndOfStream();
        std::lock_guard lock(m_state->mutex);
        m_state->ended = false;
    }

    void pump();

    const std::shared_ptr<DecodeState> &state() const {
        return m_state;
    }

#if !RELEASE_LOG_DISABLED
    const Logger &logger() const final {
        return m_logger;
    }

    ASCIILiteral logClassName() const final {
        return "NativeMediaSource"_s;
    }

    uint64_t logIdentifier() const final {
        return reinterpret_cast<uintptr_t>(this);
    }

    WTFLogChannel &logChannel() const final {
        return LogMediaSource;
    }

#endif
  private:
    NativeMediaSource(MediaModernOS &, MediaSourcePrivateClient &, std::shared_ptr<DecodeState>);
    ThreadSafeWeakPtr<MediaModernOS> m_parent;
    std::shared_ptr<DecodeState> m_state;
    std::atomic<bool> m_pumpPending{false};
    unsigned m_nextSource = 0;
#if !RELEASE_LOG_DISABLED
    Ref<const Logger> m_logger;
#endif
};

class MediaModernOS final : public MediaPlayerPrivateInterface,
                            public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<MediaModernOS> {
  public:
    explicit MediaModernOS(MediaPlayer &player)
        : m_player(player), m_timer(Ref{RunLoop::mainSingleton()}, "ModernOS media presentation"_s,
                                    [this] { update(); }) {
    }

    ~MediaModernOS() final {
        cancelLoad();
    }

    void ref() const final {
        ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref();
    }

    void deref() const final {
        ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref();
    }

    constexpr MediaPlayerType mediaPlayerType() const final {
        return MediaPlayerType::ModernOS;
    }

    void load(const URL &url, const LoadOptions &) final {
        Ref protectedThis{*this};
        auto player = m_player.get();
        if (!player)
            return;
        cancelLoad();
        fprintf(stderr, "Media backend: native file decoder\n");
        m_url = url;
        m_current = 0;
        m_paused = true;
        m_image = nullptr;
        m_size = {};
        m_duration = 0;
        m_audio = false;
        m_buffered = {};
        m_waiting = true;
        m_network = MediaPlayer::NetworkState::Loading;
        m_ready = MediaPlayer::ReadyState::HaveNothing;
        m_state = std::make_shared<DecodeState>();
        m_state->fetch = [this](int64_t offset) { requestRange(offset); };
        m_loader = player->mediaResourceLoader();
        m_worker = std::thread([state = m_state] { decodeVideo(state); });
        m_timer.startRepeating(8_ms);
        auto state = m_state;
        player->networkStateChanged();
        if (m_state == state)
            player->readyStateChanged();
    }

    void load(const URL &, const LoadOptions &, MediaSourcePrivateClient &client) final {
        Ref protectedThis{*this};
        cancelLoad();
        fprintf(stderr, "Media backend: native MSE decoder\n");
        m_current = 0;
        m_paused = true;
        m_image = nullptr;
        m_size = {};
        m_duration = 0;
        m_audio = false;
        m_buffered = {};
        m_waiting = true;
        m_network = MediaPlayer::NetworkState::Loading;
        m_ready = MediaPlayer::ReadyState::HaveNothing;
        m_state = std::make_shared<DecodeState>();
        m_source = NativeMediaSource::create(*this, client, m_state);
        m_timer.startRepeating(8_ms);
        client.setPrivateAndOpen(Ref<MediaSourcePrivate>{*m_source});
        if (auto player = m_player.get())
            player->networkStateChanged();
    }

    void readyStateFromMediaSourceChanged() final {
    }

    void characteristicsFromMediaSourceChanged() final {
    }

    void cancelLoad() final {
        m_timer.stop();
        if (m_state) {
            std::lock_guard lock(m_state->mutex);
            m_state->cancelled = true;
            m_state->changed.notify_all();
        }
        if (m_resource) {
            m_resource->setClient(nullptr);
            m_resource->shutdown();
            m_resource = nullptr;
        }
        if (m_worker.joinable())
            m_worker.join();
        m_state = nullptr;
        m_loader = nullptr;
        m_videoFrameMetadata.reset();
        m_presentedFrames = 0;
        if (m_source) {
            m_source->setPlayer(nullptr);
            m_source->shutdown();
            m_source = nullptr;
        }
        if (m_audioStarted && audioSink)
            audioSink(OS_AUDIO_RESET, {});
        m_audioStarted = false;
        resetAudioClock();
        m_audioClockChecked = m_audioClockAvailable = false;
        if (m_seekPromise) {
            m_seekPromise->reject(PlatformMediaError::Cancelled);
            m_seekPromise.reset();
        }
    }

    void play() final {
        if (!m_paused)
            return;
        m_started = MonotonicTime::now();
        m_paused = false;
        if (m_state)
            m_state->progressing = true;
        if (auto player = m_player.get())
            player->playbackStateChanged();
    }

    void pause() final {
        if (m_paused)
            return;
        updateAudioClock();
        m_current = currentTime().toDouble();
        m_paused = true;
        if (m_state)
            m_state->progressing = false;
        if (m_audioStarted && audioSink)
            audioSink(OS_AUDIO_RESET, {});
        // DMA buffers are dropped on reset. Return their unplayed original
        // samples to the decoder queue so resume preserves the paused sound.
        if (m_state && m_audioClockAvailable) {
            std::lock_guard lock(m_state->mutex);
            while (!m_outputSound.empty()) {
                auto sound = std::move(m_outputSound.back());
                m_outputSound.pop_back();
                m_state->soundFrames += sound.samples.size() / 2;
                m_state->sound.push_front(std::move(sound));
            }
        }

        m_audioStarted = false;
        resetAudioClock();
        if (auto player = m_player.get())
            player->playbackStateChanged();
    }

    bool paused() const final {
        return m_paused;
    }

    bool timeIsProgressing() const final {
        return !m_paused && !m_waiting;
    }

    std::optional<VideoFrameMetadata> videoFrameMetadata() final {
        return std::exchange(m_videoFrameMetadata, std::nullopt);
    }

    void startVideoFrameMetadataGathering() final {
        m_gatheringVideoMetadata = true;
    }

    void stopVideoFrameMetadataGathering() final {
        m_gatheringVideoMetadata = false;
        m_videoFrameMetadata.reset();
    }

    FloatSize naturalSize() const final {
        return m_size;
    }

    bool hasVideo() const final {
        return !m_size.isEmpty();
    }

    bool hasAudio() const final {
        return m_audio;
    }

    void setPageIsVisible(bool visible) final {
        m_visible = visible;
    }

    MediaTime duration() const final {
        return m_duration > 0 ? MediaTime::createWithDouble(m_duration)
                              : MediaTime::positiveInfiniteTime();
    }

    MediaTime currentTime() const final {
        double time;
        if (m_audio && m_rate == 1 && m_audioClockAvailable) {
            time = m_audioStarted ? m_audioClockTime : m_current;
            if (m_audioStarted && !m_paused)
                time += std::min((MonotonicTime::now() - m_audioClockUpdated).seconds(),
                                 m_audioQueued / 48000.0);
        } else
            time = m_current + (!m_paused && !m_waiting
                                    ? (MonotonicTime::now() - m_started).seconds() * m_rate
                                    : 0);
        if (m_duration > 0)
            time = std::min(time, m_duration);
        return MediaTime::createWithDouble(time);
    }

    Ref<MediaTimePromise> seekToTarget(const SeekTarget &target) final {
        if (!m_state || !target.time.isFinite() || target.time < MediaTime::zeroTime())
            return MediaTimePromise::createAndReject(PlatformMediaError::NotReady);
        if (m_seekPromise)
            m_seekPromise->reject(PlatformMediaError::Cancelled);
        m_seekPromise.emplace();
        double time = target.time.toDouble();
        if (m_duration > 0)
            time = std::min(time, m_duration);
        {
            std::lock_guard lock(m_state->mutex);
            m_state->frames.clear();
            m_state->sound.clear();
            m_state->soundFrames = 0;
            m_state->seek = time;
            m_state->sought = -1;
            m_state->changed.notify_all();
        }

        m_current = time;
        m_started = MonotonicTime::now();
        m_waiting = true;
        m_videoFrameMetadata.reset();
        if (m_source) {
            auto state = m_state;
            Ref source{*m_source};
            source->waitForTarget(target)->whenSettled(
                RunLoop::mainSingleton(), [state, source = WTF::move(source)](auto &&result) {
                    if (state->cancelled)
                        return;
                    if (!result) {
                        std::lock_guard lock(state->mutex);
                        state->error = AVERROR(EIO);
                        return;
                    }
                    double time = result->toDouble();
                    source->reenqueueMediaForTime(*result)->whenSettled(
                        RunLoop::mainSingleton(), [state, time](auto &&complete) {
                            std::lock_guard lock(state->mutex);
                            if (!state->cancelled) {
                                if (complete)
                                    state->sought = time;
                                else
                                    state->error = AVERROR(EIO);
                            }
                        });
                });
        }

        if (m_audioStarted && audioSink)
            audioSink(OS_AUDIO_RESET, {});
        m_audioStarted = false;
        resetAudioClock();
        return m_seekPromise->promise();
    }

    void setRateDouble(double rate) final {
        if (!std::isfinite(rate) || rate < 0 || rate > 4)
            return;
        if (rate == m_rate)
            return;
        updateAudioClock();
        m_current = currentTime().toDouble();
        m_started = MonotonicTime::now();
        m_rate = rate;
        if (m_audioStarted && audioSink)
            audioSink(OS_AUDIO_RESET, {});
        m_audioStarted = false;
        resetAudioClock();
        if (auto player = m_player.get())
            player->rateChanged();
    }

    double rate() const final {
        return m_rate;
    }

    void setVolume(float volume) final {
        if (std::isfinite(volume))
            m_volume = std::clamp(volume, 0.f, 1.f);
    }

    void setMuted(bool muted) final {
        m_muted = muted;
    }

    MediaPlayer::NetworkState networkState() const final {
        return m_network;
    }

    MediaPlayer::ReadyState readyState() const final {
        return m_ready;
    }

    const PlatformTimeRanges &buffered() const final {
        return m_buffered;
    }

    MediaTime maxTimeSeekable() const final {
        return m_duration > 0 ? duration() : MediaTime::zeroTime();
    }

    bool didLoadingProgress() const final {
        return std::exchange(m_progress, false);
    }

    DestinationColorSpace colorSpace() final {
        return DestinationColorSpace::SRGB();
    }

    RefPtr<NativeImage> nativeImageForCurrentTime() final {
        return m_image;
    }

    void paint(GraphicsContext &context, const FloatRect &rect) final {
        if (m_image && m_visible)
            context.drawNativeImage(*m_image, rect, {{}, naturalSize()});
    }

  private:
    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RunLoop::Timer m_timer;
    URL m_url;
    RefPtr<PlatformMediaResourceLoader> m_loader;
    RefPtr<PlatformMediaResource> m_resource;
    std::shared_ptr<DecodeState> m_state;
    RefPtr<NativeMediaSource> m_source;
    std::thread m_worker;
    RefPtr<NativeImage> m_image;
    FloatSize m_size;
    PlatformTimeRanges m_buffered;
    MediaPlayer::NetworkState m_network = MediaPlayer::NetworkState::Empty;
    MediaPlayer::ReadyState m_ready = MediaPlayer::ReadyState::HaveNothing;
    double m_current = 0, m_duration = 0, m_rate = 1;
    MonotonicTime m_started;
    bool m_paused = true, m_audio = false, m_visible = true;
    bool m_muted = false, m_audioStarted = false;
    bool m_waiting = true;
    bool m_gatheringVideoMetadata = false;
    unsigned m_presentedFrames = 0;
    std::optional<VideoFrameMetadata> m_videoFrameMetadata;
    bool m_audioClockChecked = false, m_audioClockAvailable = false;
    uint64_t m_audioSubmitted = 0, m_audioQueued = 0;
    double m_audioOrigin = 0, m_audioClockTime = 0;
    MonotonicTime m_audioClockUpdated;
    std::deque<DecodeState::AudioPixels> m_outputSound;
    float m_volume = 1;
    mutable bool m_progress = false;
    std::optional<MediaTimePromise::AutoRejectProducer> m_seekPromise;

    void resetAudioClock() {
        m_audioSubmitted = m_audioQueued = 0;
        m_audioOrigin = m_audioClockTime = m_current;
        m_audioClockUpdated = MonotonicTime::now();
        m_outputSound.clear();
    }

    void updateAudioClock() {
        if (!audioQueue || !m_audio || m_rate != 1)
            return;
        if (!m_audioClockChecked || m_audioStarted) {
            auto queued = audioQueue();
            m_audioClockChecked = true;
            if (!queued) {
                m_audioClockAvailable = false;
                return;
            }
            m_audioClockAvailable = true;
            if (!m_audioStarted)
                return;
            m_audioQueued = std::min(*queued, m_audioSubmitted);
            m_audioClockTime = m_audioOrigin + (m_audioSubmitted - m_audioQueued) / 48000.0;
            m_audioClockUpdated = MonotonicTime::now();
            while (!m_outputSound.empty()) {
                auto &sound = m_outputSound.front();
                size_t consumed = static_cast<size_t>(
                    std::min<double>(sound.samples.size() / 2,
                                     std::max(0.0, (m_audioClockTime - sound.time) * 48000.0)));
                if (!consumed)
                    break;
                sound.samples.erase(sound.samples.begin(), sound.samples.begin() + consumed * 2);
                sound.time += consumed / 48000.0;
                if (sound.samples.empty())
                    m_outputSound.pop_front();
                else
                    break;
            }
        }
    }

    void requestRange(int64_t offset) {
        if (!m_state || m_state->cancelled)
            return;
        if (m_resource) {
            m_resource->setClient(nullptr);
            m_resource->shutdown();
            m_resource = nullptr;
        }
        int64_t end = offset > INT64_MAX - static_cast<int64_t>(rangeSize) ? INT64_MAX
                                                                           : offset + rangeSize - 1;
        ResourceRequest request{URL(m_url)};
        request.setHTTPHeaderField(HTTPHeaderName::Range,
                                   makeString("bytes="_s, offset, "-"_s, end));
        m_resource = m_loader->requestResource(WTF::move(request),
                                               PlatformMediaResourceLoader::DisallowCaching);
        if (!m_resource) {
            std::lock_guard lock(m_state->mutex);
            m_state->failedFetch = m_state->fetched = true;
            m_state->changed.notify_all();
            return;
        }

        m_resource->setClient(adoptRef(*new RangeClient(m_state, offset)));
    }

    void update() {
        Ref protectedThis{*this};
        auto state = m_state;
        auto player = m_player.get();
        if (!state || !player)
            return;
        updateAudioClock();
        state->clock = currentTime().toDouble();
        if (m_source)
            m_source->pump();
        std::optional<VideoPixels> pixels;
        bool ended = false;
        int error = 0;
        MediaPlayer::ReadyState ready = m_ready;
        double sought = -1, lastBuffered = 0;
        auto oldSize = m_size;
        double oldDuration = m_duration;
        bool oldAudio = m_audio;
        bool audioData = false;
        {
            std::lock_guard lock(state->mutex);
            error = state->error;
            ended = state->ended;
            if (state->metadata) {
                m_size = {static_cast<float>(state->width), static_cast<float>(state->height)};
                m_duration = state->duration;
                m_audio = state->audio;
            }
            audioData = !state->sound.empty();
            sought = state->sought;
            state->sought = -1;
            if (!m_size.isEmpty() || m_audio)
                ready = MediaPlayer::ReadyState::HaveMetadata;
            double time = currentTime().toDouble();
            while (!state->frames.empty() &&
                   (!m_image || (sought >= 0) ||
                    (!m_paused && state->frames.front().time <= time + .005))) {
                pixels = std::move(state->frames.front());
                state->frames.pop_front();
                state->changed.notify_all();
                if (m_paused || (sought >= 0))
                    break;
            }

            if (m_image || pixels || (m_audio && m_size.isEmpty() && audioData))
                ready = MediaPlayer::ReadyState::HaveCurrentData;
            if (m_audio && m_size.isEmpty() && state->soundFrames > 0) {
                ready = MediaPlayer::ReadyState::HaveFutureData;
                lastBuffered =
                    state->sound.back().time + state->sound.back().samples.size() / 96000.0;
            }
            if (!state->frames.empty()) {
                lastBuffered = state->frames.back().time + state->frames.back().duration;
                ready = MediaPlayer::ReadyState::HaveFutureData;
            }
        }

        if (m_source) {
            auto coded = m_source->mediaPlayerReadyState();
            if (coded < MediaPlayer::ReadyState::HaveCurrentData)
                ready = coded;
            else if (m_image || pixels || (m_audio && m_size.isEmpty() && audioData))
                ready = coded;
        }

        // A buffering event leaves paused=false in the DOM, but media time
        // must stop until future data is available. Advancing wall time here
        // causes delayed audio packets and entire short videos to be skipped.
        bool waiting = ready <= MediaPlayer::ReadyState::HaveCurrentData && !ended;
        if (waiting != m_waiting) {
            m_current = currentTime().toDouble();
            m_started = MonotonicTime::now();
            m_waiting = waiting;
        }
        state->clock = currentTime().toDouble();
        state->progressing = !m_paused && !m_waiting;
        if (error) {
            m_network = m_size.isEmpty() ? MediaPlayer::NetworkState::FormatError
                                         : MediaPlayer::NetworkState::DecodeError;
            m_timer.stop();
            player->networkStateChanged();
            return;
        }

        if (pixels) {
            auto data = SkData::MakeWithCopy(pixels->bytes.data(), pixels->bytes.size());
            auto image = SkImages::RasterFromData(
                SkImageInfo::MakeN32Premul(pixels->width, pixels->height), data, pixels->stride);
            m_image = NativeImage::create(WTF::move(image));
            m_progress = true;
            if (m_image) {
                ++m_presentedFrames;
                if (m_gatheringVideoMetadata && m_visible) {
                    double submitted = MonotonicTime::now().secondsSinceEpoch().seconds();
                    m_videoFrameMetadata =
                        VideoFrameMetadata{.presentationTime = submitted,
                                           .expectedDisplayTime = submitted,
                                           .width = static_cast<unsigned>(pixels->width),
                                           .height = static_cast<unsigned>(pixels->height),
                                           .mediaTime = pixels->time,
                                           .presentedFrames = m_presentedFrames};
                }
            }
        }

        if (!m_paused && m_audio) {
            if (!m_audioClockChecked)
                updateAudioClock();
            // Fill the bounded 200-ms device target before yielding. AAC
            // packets are often only 21 ms; four packets leave the device
            // starved when a software-rendered frame takes longer than that.
            for (unsigned chunk = 0; chunk < 16; chunk++) {
                std::vector<int16_t> samples;
                size_t count = 0;
                double stamp = 0;
                bool silence = false;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->sound.empty())
                        break;
                    auto &sound = state->sound.front();
                    stamp = sound.time;
                    if (!std::isfinite(stamp)) {
                        state->error = AVERROR(EINVAL);
                        break;
                    }
                    if (m_audioClockAvailable && m_rate == 1) {
                        if (m_audioQueued >= 9600)
                            break;
                        double next = m_audioStarted ? m_audioOrigin + m_audioSubmitted / 48000.0
                                                     : currentTime().toDouble();
                        if (stamp + .001 < next) {
                            size_t skipped = static_cast<size_t>(std::min<double>(
                                sound.samples.size() / 2, (next - stamp) * 48000.0));
                            sound.samples.erase(sound.samples.begin(),
                                                sound.samples.begin() + skipped * 2);
                            sound.time += skipped / 48000.0;
                            state->soundFrames -= skipped;
                            if (sound.samples.empty()) {
                                state->sound.pop_front();
                                state->changed.notify_all();
                                continue;
                            }
                            stamp = sound.time;
                        }

                        if (stamp > next + .001) {
                            silence = true;
                            count = static_cast<size_t>(
                                std::min<double>(OS_AUDIO_MAX_FRAMES, (stamp - next) * 48000.0));
                            stamp = next;
                            samples.resize(count * 2);
                        }
                    } else if (stamp > currentTime().toDouble() + .04)
                        break;
                    if (!silence) {
                        count = std::min<size_t>(sound.samples.size() / 2, OS_AUDIO_MAX_FRAMES);
                        samples.assign(sound.samples.begin(), sound.samples.begin() + count * 2);
                    }
                }

                int result = 0;
                auto original = m_audioClockAvailable ? samples : std::vector<int16_t>{};
                if (audioSink && m_rate == 1 &&
                    (m_audioClockAvailable ||
                     stamp + count / 48000.0 >= currentTime().toDouble() - .1)) {
                    for (auto &value : samples)
                        value = m_muted ? 0 : static_cast<int16_t>(value * m_volume);
                    result = audioSink(OS_AUDIO_WRITE, samples);
                    if (!result) {
                        if (!m_audioStarted) {
                            m_audioOrigin = m_audioClockTime = stamp;
                            m_audioClockUpdated = MonotonicTime::now();
                        }
                        m_audioStarted = true;
                        if (m_audioClockAvailable) {
                            m_audioSubmitted += count;
                            m_audioQueued += count;
                            m_outputSound.push_back({std::move(original), stamp});
                        }
                    }
                }

                if (result == EAGAIN || result == EBUSY)
                    break;
                if (result && m_audioClockAvailable) {
                    m_current = currentTime().toDouble();
                    m_started = MonotonicTime::now();
                    m_audioClockAvailable = false;
                    m_audioStarted = false;
                    resetAudioClock();
                    fprintf(stderr, "Media audio output unavailable: errno=%d\n", result);
                }

                if (silence) {
                    if (result)
                        break;
                    continue;
                }
                std::lock_guard lock(state->mutex);
                auto &sound = state->sound.front();
                sound.samples.erase(sound.samples.begin(), sound.samples.begin() + count * 2);
                sound.time += count / 48000.0;
                state->soundFrames -= count;
                if (sound.samples.empty())
                    state->sound.pop_front();
                state->changed.notify_all();
            }
        }

        bool sizeChanged = oldSize != m_size;
        bool durationChanged = oldDuration != m_duration;
        bool readyChanged = ready != m_ready;
        bool networkChanged = ended && m_network == MediaPlayer::NetworkState::Loading;
        if (ended && m_audioClockAvailable && m_audioStarted && !m_audioQueued && m_duration > 0 &&
            currentTime().toDouble() >= m_duration - .1) {
            std::lock_guard lock(state->mutex);
            if (state->sound.empty())
                m_audioClockTime = m_duration;
        }

        bool playbackEnded =
            ended && !m_paused && m_duration > 0 && currentTime().toDouble() >= m_duration;
        m_buffered = m_source ? m_source->buffered() : PlatformTimeRanges{};
        if (!m_source && lastBuffered > currentTime().toDouble())
            m_buffered.add(currentTime(), MediaTime::createWithDouble(lastBuffered));
        m_ready = ready;
        if (networkChanged)
            m_network = MediaPlayer::NetworkState::Loaded;
        if (playbackEnded) {
            m_current = m_duration;
            m_paused = true;
        }
        if (sought >= 0 && m_seekPromise) {
            m_seekPromise->resolve(MediaTime::createWithDouble(sought));
            m_seekPromise.reset();
        }
        // DOM callbacks can change src or remove the element. Keep both objects
        // alive and stop notifying once this load has been replaced.
        if (pixels) {
            player->repaint();
            if (m_state != state)
                return;
        }
        if (sizeChanged) {
            player->sizeChanged();
            if (m_state != state)
                return;
        }
        if (sizeChanged || oldAudio != m_audio) {
            player->characteristicChanged();
            if (m_state != state)
                return;
        }
        if (durationChanged) {
            player->durationChanged();
            if (m_state != state)
                return;
        }
        if (readyChanged) {
            player->readyStateChanged();
            if (m_state != state)
                return;
        }
        if (sought >= 0) {
            player->timeChanged();
            if (m_state != state)
                return;
        }
        if (networkChanged) {
            player->networkStateChanged();
            if (m_state != state)
                return;
        }
        if (playbackEnded) {
            player->timeChanged();
            if (m_state == state)
                player->playbackStateChanged();
        }
    }
};

#include "MediaSourceModernOS.inc"

class MediaFactory final : public MediaPlayerFactory {
  public:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final {
        return MediaPlayerEnums::MediaEngineIdentifier::ModernOS;
    }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer &player) const final {
        return adoptRef(*new MediaModernOS(player));
    }

    void getSupportedTypes(HashSet<String> &types) const final {
        for (auto type :
             {"video/mp4"_s, "video/webm"_s, "audio/mp4"_s, "audio/x-m4a"_s, "audio/webm"_s,
              "audio/ogg"_s, "audio/mpeg"_s, "audio/aac"_s, "audio/wav"_s, "audio/x-wav"_s})
            types.add(type);
    }

    MediaPlayer::SupportsType
    supportsTypeAndCodecs(const MediaEngineSupportParameters &parameters) const final {
        if (parameters.platformType == PlatformMediaDecodingType::MediaSource)
            return nativeMSESupport(parameters.type);
        if (parameters.platformType != PlatformMediaDecodingType::FileOrHLS)
            return MediaPlayer::SupportsType::IsNotSupported;
        const auto &type = parameters.type;
        auto container = type.containerType();
        bool mp4 = container == "video/mp4"_s || container == "audio/mp4"_s ||
                   container == "audio/x-m4a"_s;
        bool webm = container == "video/webm"_s || container == "audio/webm"_s;
        bool ogg = container == "audio/ogg"_s;
        bool mp3 = container == "audio/mpeg"_s;
        bool aac = container == "audio/aac"_s;
        bool wav = container == "audio/wav"_s || container == "audio/x-wav"_s;
        if (!mp4 && !webm && !ogg && !mp3 && !aac && !wav)
            return MediaPlayer::SupportsType::IsNotSupported;
        for (auto &codec : type.codecs()) {
            bool supported =
                (container == "video/mp4"_s &&
                 (codec.startsWith("avc1"_s) || codec.startsWith("avc3"_s))) ||
                (container == "video/webm"_s &&
                 (codec == "vp8"_s || codec == "vp9"_s || codec.startsWith("vp09"_s))) ||
                ((mp4 || aac) &&
                 (codec == "mp4a.40.2"_s || codec == "mp4a.40.5"_s || codec == "mp4a.40.29"_s)) ||
                ((webm || ogg) && (codec == "opus"_s || codec == "vorbis"_s)) ||
                (mp3 && codec == "mp3"_s) || (wav && codec == "1"_s);
            if (!supported)
                return MediaPlayer::SupportsType::IsNotSupported;
        }

        return type.codecs().isEmpty() ? MediaPlayer::SupportsType::MayBeSupported
                                       : MediaPlayer::SupportsType::IsSupported;
    }
};

void registerMediaEngine() {
    av_max_alloc(64 * 1024 * 1024);
    // The kernel saves x87/MMX/XMM with FXSAVE. Keep real SSE decoding while
    // excluding every instruction family that needs extended YMM/ZMM state.
    av_force_cpu_flags(av_get_cpu_flags() &
                       ~(AV_CPU_FLAG_AVX | AV_CPU_FLAG_AVX2 | AV_CPU_FLAG_AVX512 |
                         AV_CPU_FLAG_AVX512ICL | AV_CPU_FLAG_XOP | AV_CPU_FLAG_FMA3 |
                         AV_CPU_FLAG_FMA4));
    MediaPlayerFactorySupport::callRegisterMediaEngine(
        [](MediaEngineRegistrar registrar) { registrar(makeUnique<MediaFactory>()); });
}
}
