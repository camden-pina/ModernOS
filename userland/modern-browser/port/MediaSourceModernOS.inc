// Real ISO-BMFF MSE demuxing and CPU decoding. WebCore retains ownership of
// coded-frame processing, timestamp offsets, append windows, eviction and seek.
// Only complete, bounded MP4 boxes reach FFmpeg's memory-only demuxer.
static MediaPlayer::SupportsType nativeMSESupport(const ContentType &type) {
    bool audio = type.containerType() == "audio/mp4"_s;
    if (!audio && type.containerType() != "video/mp4"_s)
        return MediaPlayer::SupportsType::IsNotSupported;
    for (auto &codec : type.codecs()) {
        if (codec.startsWith("avc1."_s) || codec.startsWith("avc3."_s)) {
            if (audio)
                return MediaPlayer::SupportsType::IsNotSupported;
        } else if (!codec.startsWith("mp4a.40."_s))
            return MediaPlayer::SupportsType::IsNotSupported;
    }

    return type.codecs().isEmpty() ? MediaPlayer::SupportsType::MayBeSupported
                                   : MediaPlayer::SupportsType::IsSupported;
}

class NativeMediaDescription final : public MediaDescription {
  public:
    NativeMediaDescription(String codec, bool video)
        : MediaDescription(codec.isolatedCopy()), m_video(video) {
    }

    bool isVideo() const final {
        return m_video;
    }

    bool isAudio() const final {
        return !m_video;
    }

    bool isText() const final {
        return false;
    }

  private:
    bool m_video;
};

class NativeVideoTrack final : public VideoTrackPrivate {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(NativeVideoTrack);

  public:
    explicit NativeVideoTrack(TrackID id) : m_id(id) {
        setSelected(true);
    }

    TrackID id() const final {
        return m_id;
    }

    Kind kind() const final {
        return Kind::Main;
    }

    std::optional<bool> defaultEnabled() const final {
        return true;
    }

  private:
    TrackID m_id;
};

class NativeAudioTrack final : public AudioTrackPrivate {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(NativeAudioTrack);

  public:
    explicit NativeAudioTrack(TrackID id) : m_id(id) {
        setEnabled(true);
    }

    TrackID id() const final {
        return m_id;
    }

    Kind kind() const final {
        return Kind::Main;
    }

    std::optional<bool> defaultEnabled() const final {
        return true;
    }

  private:
    TrackID m_id;
};

struct NativeCodecConfiguration {
    AVCodecParameters *parameters = avcodec_parameters_alloc();

    explicit NativeCodecConfiguration(const AVCodecParameters &source) {
        if (parameters && avcodec_parameters_copy(parameters, &source) < 0)
            avcodec_parameters_free(&parameters);
    }

    NativeCodecConfiguration(const NativeCodecConfiguration &) = delete;
    NativeCodecConfiguration &operator=(const NativeCodecConfiguration &) = delete;

    ~NativeCodecConfiguration() {
        avcodec_parameters_free(&parameters);
    }
};

class NativeMediaSample final : public MediaSample {
  public:
    NativeMediaSample(const AVPacket &packet, TrackID id, MediaTime pts, MediaTime dts,
                      MediaTime duration, FloatSize size,
                      std::shared_ptr<const NativeCodecConfiguration> configuration)
        : m_packet(av_packet_clone(&packet)), m_id(id), m_pts(pts), m_dts(dts),
          m_duration(duration), m_size(size),
          m_flags(static_cast<SampleFlags>(HasSyncInfo |
                                           (packet.flags & AV_PKT_FLAG_KEY ? IsSync : 0))),
          m_configuration(std::move(configuration)) {
    }

    ~NativeMediaSample() final {
        av_packet_free(&m_packet);
    }

    MediaTime presentationTime() const final {
        return m_pts;
    }

    MediaTime decodeTime() const final {
        return m_dts;
    }

    MediaTime duration() const final {
        return m_duration;
    }

    TrackID trackID() const final {
        return m_id;
    }

    size_t sizeInBytes() const final {
        return m_packet ? m_packet->size : 0;
    }

    FloatSize presentationSize() const final {
        return m_size;
    }

    void offsetTimestampsBy(const MediaTime &offset) final {
        m_pts += offset;
        m_dts += offset;
    }

    void setTimestamps(const MediaTime &pts, const MediaTime &dts) final {
        m_pts = pts;
        m_dts = dts;
    }

    Ref<MediaSample> createNonDisplayingCopy() const final {
        Ref copy = adoptRef(*new NativeMediaSample(*m_packet, m_id, m_pts, m_dts, m_duration,
                                                   m_size, m_configuration));
        copy->m_flags = static_cast<SampleFlags>(m_flags | IsNonDisplaying);
        copy->m_trim = m_trim;
        return copy;
    }

    Ref<MediaSample> createCopyWithAdjustedStartTime(const MediaTime &offset) const final {
        auto trim = std::clamp(offset, MediaTime::zeroTime(), m_duration);
        Ref copy = adoptRef(*new NativeMediaSample(*m_packet, m_id, m_pts + trim, m_dts + trim,
                                                   m_duration - trim, m_size, m_configuration));
        copy->m_flags = m_flags;
        copy->m_trim = m_trim + trim;
        return copy;
    }

    SampleFlags flags() const final {
        return m_flags;
    }

    PlatformSample platformSample() const final {
        return PlatformSample{static_cast<const void *>(this)};
    }

    Type type() const final {
        return Type::ModernOS;
    }

    const AVPacket *packet() const {
        return m_packet;
    }

    double trim() const {
        return m_trim.toDouble();
    }

    const std::shared_ptr<const NativeCodecConfiguration> &configuration() const {
        return m_configuration;
    }

  private:
    AVPacket *m_packet;
    TrackID m_id;
    MediaTime m_pts, m_dts, m_duration, m_trim = MediaTime::zeroTime();
    FloatSize m_size;
    SampleFlags m_flags;
    std::shared_ptr<const NativeCodecConfiguration> m_configuration;
};

struct NativeMSEInput {
    std::span<const uint8_t> bytes;
    size_t position = 0;

    static int read(void *opaque, uint8_t *out, int wanted) {
        auto &input = *static_cast<NativeMSEInput *>(opaque);
        if (input.position >= input.bytes.size())
            return AVERROR_EOF;
        size_t size = std::min<size_t>(wanted, input.bytes.size() - input.position);
        memcpy(out, input.bytes.data() + input.position, size);
        input.position += size;
        return size;
    }

    static int64_t seek(void *opaque, int64_t offset, int whence) {
        auto &input = *static_cast<NativeMSEInput *>(opaque);
        if (whence == AVSEEK_SIZE)
            return input.bytes.size();
        whence &= ~AVSEEK_FORCE;
        int64_t base = whence == SEEK_SET   ? 0
                       : whence == SEEK_CUR ? input.position
                       : whence == SEEK_END ? input.bytes.size()
                                            : -1;
        if (base < 0 || offset < -base || offset > static_cast<int64_t>(input.bytes.size()) - base)
            return AVERROR(EINVAL);
        input.position = base + offset;
        return input.position;
    }
};

class NativeSourceBuffer final : public SourceBufferPrivate {
  public:
    NativeSourceBuffer(NativeMediaSource &source, unsigned number)
        : SourceBufferPrivate(source), m_state(source.state()), m_number(number) {
        setMaximumBufferSize(48 * 1024 * 1024);
    }

    constexpr MediaPlatformType platformType() const final {
        return MediaPlatformType::ModernOS;
    }

    bool canSwitchToType(const ContentType &type) final {
        return nativeMSESupport(type) != MediaPlayer::SupportsType::IsNotSupported;
    }

    void resume() {
        for (auto &decoder : m_decoders)
            provideMediaData(decoder->id);
    }

#if !RELEASE_LOG_DISABLED
    const Logger &sourceBufferLogger() const final {
        return m_logger;
    }

    uint64_t sourceBufferLogIdentifier() final {
        return reinterpret_cast<uintptr_t>(this);
    }

#endif
  private:
    struct SampleStamp {
        double time, duration, trim;
        bool display;
    };

    struct Decoder {
        AVCodecContext *context = nullptr;
        AVFrame *frame = nullptr;
        SwsContext *conversion = nullptr;
        SwrContext *resampler = nullptr;
        TrackID id = 0;
        bool video = false, drained = false;
        double last = 0;
        std::shared_ptr<const NativeCodecConfiguration> streamConfiguration, activeConfiguration;

        ~Decoder() {
            sws_freeContext(conversion);
            swr_free(&resampler);
            av_frame_free(&frame);
            avcodec_free_context(&context);
        }
    };

    std::shared_ptr<DecodeState> m_state;
    unsigned m_number;
    std::vector<uint8_t> m_pending, m_init;
    std::vector<std::unique_ptr<Decoder>> m_decoders;
#if !RELEASE_LOG_DISABLED
    Ref<const Logger> m_logger = Logger::create(this);
#endif

    static uint32_t be32(const uint8_t *p) {
        return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
               static_cast<uint32_t>(p[2]) << 8 | p[3];
    }

    Ref<MediaPromise> appendInternal(Ref<SharedBuffer> &&data) final {
        if (m_state->cancelled)
            return MediaPromise::createAndReject(PlatformMediaError::Cancelled);
        if (data->size() > 16 * 1024 * 1024 || m_pending.size() > 16 * 1024 * 1024 - data->size())
            return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
        auto bytes = data->span();
        m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
        size_t complete = 0, safeEnd = 0;
        bool haveMoov = false, haveMdat = false, insideFragment = false;
        while (m_pending.size() - complete >= 8) {
            const auto *box = m_pending.data() + complete;
            uint64_t length = be32(box);
            size_t header = 8;
            if (length == 1) {
                if (m_pending.size() - complete < 16)
                    break;
                length = static_cast<uint64_t>(be32(box + 8)) << 32 | be32(box + 12);
                header = 16;
            }

            if (length < header || length > 16 * 1024 * 1024)
                return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
            if (length > m_pending.size() - complete)
                break;
            uint32_t type = be32(box + 4);
            haveMoov |= type == 0x6d6f6f76;
            haveMdat |= type == 0x6d646174;
            if (type == 0x6d6f6f66)
                insideFragment = true;
            if (type == 0x6d646174)
                insideFragment = false;
            complete += length;
            if (!insideFragment)
                safeEnd = complete;
        }

        complete = safeEnd;
        if (!complete || (!haveMoov && !haveMdat))
            return MediaPromise::createAndResolve();
        std::vector<uint8_t> segment;
        if (haveMoov) {
            size_t at = 0;
            std::vector<uint8_t> init;
            while (at < complete) {
                uint64_t length = be32(m_pending.data() + at);
                if (length == 1)
                    length = static_cast<uint64_t>(be32(m_pending.data() + at + 8)) << 32 |
                             be32(m_pending.data() + at + 12);
                uint32_t type = be32(m_pending.data() + at + 4);
                if (type == 0x66747970 || type == 0x6d6f6f76)
                    init.insert(init.end(), m_pending.begin() + at,
                                m_pending.begin() + at + length);
                at += length;
            }

            if (init.empty() || init.size() > 1024 * 1024)
                return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
            m_init = std::move(init);
            segment.assign(m_pending.begin(), m_pending.begin() + complete);
        } else {
            if (m_init.empty())
                return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
            segment = m_init;
            segment.insert(segment.end(), m_pending.begin(), m_pending.begin() + complete);
        }

        m_pending.erase(m_pending.begin(), m_pending.begin() + complete);
        if (!demux(segment, haveMoov))
            return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
        return MediaPromise::createAndResolve();
    }

    void resetParserStateInternal() final {
        m_pending.clear();
    }

    bool demux(std::span<const uint8_t> bytes, bool initialize) {
        NativeMSEInput input{bytes};
        auto *buffer = static_cast<uint8_t *>(av_malloc(32768));
        AVIOContext *io = buffer
                              ? avio_alloc_context(buffer, 32768, 0, &input, NativeMSEInput::read,
                                                   nullptr, NativeMSEInput::seek)
                              : nullptr;
        AVFormatContext *format = avformat_alloc_context();
        AVPacket *packet = av_packet_alloc();
        bool success = false;
        if (io && format && packet) {
            format->pb = io;
            format->flags |= AVFMT_FLAG_CUSTOM_IO;
            format->probesize = 512 * 1024;
            format->max_streams = 2;
            format->interrupt_callback = {
                [](void *state) -> int { return static_cast<DecodeState *>(state)->cancelled; },
                m_state.get()};
            int error = avformat_open_input(&format, nullptr, av_find_input_format("mov"), nullptr);
            if (error >= 0 && format->nb_streams > 0 && format->nb_streams <= 2) {
                success = !initialize || initializeTracks(*format);
                if (success && m_decoders.size() != format->nb_streams)
                    success = false;
                while (success && !m_state->cancelled &&
                       (error = av_read_frame(format, packet)) >= 0) {
                    if (packet->stream_index < 0 ||
                        static_cast<size_t>(packet->stream_index) >= m_decoders.size() ||
                        packet->size < 1 || packet->size > 8 * 1024 * 1024 ||
                        av_packet_get_side_data(packet, AV_PKT_DATA_ENCRYPTION_INFO, nullptr)) {
                        success = false;
                        break;
                    }

                    auto &decoder = *m_decoders[packet->stream_index];
                    auto *stream = format->streams[packet->stream_index];
                    double scale = av_q2d(stream->time_base);
                    double pts = packet->pts != AV_NOPTS_VALUE ? packet->pts * scale : decoder.last;
                    double dts = packet->dts != AV_NOPTS_VALUE ? packet->dts * scale : pts;
                    double duration = packet->duration > 0 ? packet->duration * scale
                                      : decoder.video
                                          ? 1. / 30
                                          : 1024. / std::max(1, decoder.context->sample_rate);
                    if (!std::isfinite(pts) || !std::isfinite(dts) || duration <= 0 ||
                        duration > 10) {
                        success = false;
                        break;
                    }
                    auto *parameters = decoder.streamConfiguration->parameters;
                    Ref sample = adoptRef(*new NativeMediaSample(
                        *packet, decoder.id, MediaTime::createWithDouble(pts),
                        MediaTime::createWithDouble(dts), MediaTime::createWithDouble(duration),
                        decoder.video ? FloatSize(parameters->width, parameters->height)
                                      : FloatSize{},
                        decoder.streamConfiguration));
                    if (!sample->packet()) {
                        success = false;
                        break;
                    }
                    decoder.last = pts + duration;
                    didReceiveSample(WTF::move(sample));
                    av_packet_unref(packet);
                }

                success = success && (error == AVERROR_EOF || m_state->cancelled);
            }
        }

        av_packet_free(&packet);
        avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        } else
            av_free(buffer);
        return success;
    }

    bool initializeTracks(AVFormatContext &format) {
        SourceBufferPrivateClient::InitializationSegment segment;
        segment.duration = MediaTime::positiveInfiniteTime();
        std::vector<std::unique_ptr<Decoder>> decoders;
        for (unsigned i = 0; i < format.nb_streams; i++) {
            auto *parameters = format.streams[i]->codecpar;
            bool video = parameters->codec_type == AVMEDIA_TYPE_VIDEO;
            if ((video && parameters->codec_id != AV_CODEC_ID_H264) ||
                (!video && (parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
                            parameters->codec_id != AV_CODEC_ID_AAC)))
                return false;
            if (parameters->codec_tag == MKTAG('e', 'n', 'c', 'v') ||
                parameters->codec_tag == MKTAG('e', 'n', 'c', 'a'))
                return false;
            if (video && (parameters->width < 1 || parameters->height < 1 ||
                          parameters->width > 1920 || parameters->height > 1080))
                return false;
            auto decoder = std::make_unique<Decoder>();
            decoder->frame = av_frame_alloc();
            decoder->streamConfiguration = std::make_shared<NativeCodecConfiguration>(*parameters);
            if (!decoder->frame || !decoder->streamConfiguration->parameters ||
                !configure(*decoder, decoder->streamConfiguration))
                return false;
            decoder->id = static_cast<uint64_t>(m_number) << 32 |
                          static_cast<uint32_t>(format.streams[i]->id + 1);
            decoder->video = video;
            String codecName = "mp4a.40.2"_s;
            if (video && parameters->extradata_size >= 4) {
                char name[32];
                snprintf(name, sizeof(name), "avc1.%02X%02X%02X", parameters->extradata[1],
                         parameters->extradata[2], parameters->extradata[3]);
                codecName = String::fromUTF8(name);
            }

            Ref description = adoptRef(*new NativeMediaDescription(WTF::move(codecName), video));
            if (video)
                segment.videoTracks.append(
                    SourceBufferPrivateClient::InitializationSegment::VideoTrackInformation{
                        description.ptr(), adoptRef(*new NativeVideoTrack(decoder->id))});
            else
                segment.audioTracks.append(
                    SourceBufferPrivateClient::InitializationSegment::AudioTrackInformation{
                        description.ptr(), adoptRef(*new NativeAudioTrack(decoder->id))});
            {
                std::lock_guard lock(m_state->mutex);
                m_state->metadata = true;
                if (video) {
                    m_state->width = parameters->width;
                    m_state->height = parameters->height;
                } else
                    m_state->audio = true;
            }

            decoders.push_back(std::move(decoder));
        }

        m_decoders = std::move(decoders);
        fprintf(stderr, "Media tracks: %zu video, %zu audio\n", segment.videoTracks.size(),
                segment.audioTracks.size());
        didReceiveInitializationSegment(WTF::move(segment));
        return true;
    }

    bool configure(Decoder &decoder,
                   const std::shared_ptr<const NativeCodecConfiguration> &configuration) {
        if (decoder.activeConfiguration == configuration)
            return true;
        if (!configuration || !configuration->parameters)
            return false;
        const AVCodec *codec = avcodec_find_decoder(configuration->parameters->codec_id);
        AVCodecContext *context = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!context)
            return false;
        context->pkt_timebase = AV_TIME_BASE_Q;
        context->thread_count = 2;
        context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        if (avcodec_parameters_to_context(context, configuration->parameters) < 0 ||
            avcodec_open2(context, codec, nullptr) < 0) {
            avcodec_free_context(&context);
            return false;
        }
        if (decoder.context && !decoder.drained) {
            int error = avcodec_send_packet(decoder.context, nullptr);
            if (error < 0 && error != AVERROR_EOF) {
                avcodec_free_context(&context);
                return false;
            }
            receive(decoder);
        }

        // Buffered samples can belong to an older initialization segment.
        // Restore their exact extradata/parameters without changing the clock
        // or dropping already decoded pictures/PCM from the shared queues.
        avcodec_free_context(&decoder.context);
        av_frame_unref(decoder.frame);
        sws_freeContext(decoder.conversion);
        decoder.conversion = nullptr;
        swr_free(&decoder.resampler);
        decoder.context = context;
        decoder.activeConfiguration = configuration;
        decoder.drained = false;
        return true;
    }

    bool isReadyForMoreSamples(TrackID id) final {
        if (m_state->cancelled)
            return false;
        for (auto &decoder : m_decoders)
            if (decoder->id == id) {
                std::lock_guard lock(m_state->mutex);
                return !m_state->error &&
                       (decoder->video ? m_state->frames.size() < 3 : m_state->soundFrames < 24000);
            }
        return false;
    }

    void notifyClientWhenReadyForMoreSamples(
        TrackID) final { /* The presentation timer pumps this dispatcher's queues. */
    }

    void flush(TrackID id) final {
        for (auto &decoder : m_decoders)
            if (decoder->id == id) {
                avcodec_flush_buffers(decoder->context);
                swr_free(&decoder->resampler);
                decoder->drained = false;
                std::lock_guard lock(m_state->mutex);
                if (decoder->video)
                    m_state->frames.clear();
                else {
                    m_state->sound.clear();
                    m_state->soundFrames = 0;
                }
            }
    }

    void enqueueSample(Ref<MediaSample> &&sample, TrackID id) final {
        if (sample->type() != MediaSample::Type::ModernOS || m_state->cancelled)
            return;
        auto &native = static_cast<NativeMediaSample &>(sample.get());
        for (auto &decoder : m_decoders)
            if (decoder->id == id) {
                if (!configure(*decoder, native.configuration())) {
                    fail(AVERROR_INVALIDDATA);
                    return;
                }
                if (decoder->drained) {
                    avcodec_flush_buffers(decoder->context);
                    decoder->drained = false;
                }
                AVPacket *packet = av_packet_clone(native.packet());
                if (!packet) {
                    fail(AVERROR(ENOMEM));
                    return;
                }
                packet->pts =
                    llround((sample->presentationTime().toDouble() - native.trim()) * AV_TIME_BASE);
                packet->dts =
                    llround((sample->decodeTime().toDouble() - native.trim()) * AV_TIME_BASE);
                packet->duration =
                    llround((sample->duration().toDouble() + native.trim()) * AV_TIME_BASE);
                packet->time_base = AV_TIME_BASE_Q;
                av_buffer_unref(&packet->opaque_ref);
                packet->opaque_ref = av_buffer_alloc(sizeof(SampleStamp));
                if (!packet->opaque_ref) {
                    av_packet_free(&packet);
                    fail(AVERROR(ENOMEM));
                    return;
                }
                SampleStamp stamp{sample->presentationTime().toDouble(),
                                  sample->duration().toDouble(), native.trim(),
                                  !sample->isNonDisplaying()};
                memcpy(packet->opaque_ref->data, &stamp, sizeof(stamp));
                int error = avcodec_send_packet(decoder->context, packet);
                av_packet_free(&packet);
                if (error < 0) {
                    fail(error);
                    return;
                }
                receive(*decoder);
                return;
            }
    }

    void allSamplesInTrackEnqueued(TrackID id) final {
        for (auto &decoder : m_decoders)
            if (decoder->id == id && !decoder->drained) {
                decoder->drained = true;
                int error = avcodec_send_packet(decoder->context, nullptr);
                if (error < 0 && error != AVERROR_EOF) {
                    fail(error);
                    return;
                }
                receive(*decoder);
            }
    }

    void receive(Decoder &decoder) {
        int error;
        while ((error = avcodec_receive_frame(decoder.context, decoder.frame)) >= 0) {
            if (decoder.frame->opaque_ref &&
                decoder.frame->opaque_ref->size == sizeof(SampleStamp)) {
                SampleStamp decoded;
                memcpy(&decoded, decoder.frame->opaque_ref->data, sizeof(decoded));
                if (decoded.display)
                    present(decoder, decoded);
            }

            av_frame_unref(decoder.frame);
        }

        if (error != AVERROR(EAGAIN) && error != AVERROR_EOF)
            fail(error);
    }

    void fail(int error) {
        std::lock_guard lock(m_state->mutex);
        m_state->error = error;
    }

    void present(Decoder &decoder, const SampleStamp &sample) {
        auto *frame = decoder.frame;
        double time = frame->best_effort_timestamp == AV_NOPTS_VALUE
                          ? sample.time - sample.trim
                          : frame->best_effort_timestamp / 1000000.;
        if (decoder.video) {
            // Keep decoding dependencies, but avoid converting/presenting
            // stale frames when a slow compositor has fallen behind audio.
            if (m_state->progressing && time + sample.duration < m_state->clock.load() - .05)
                return;
            if (frame->width < 1 || frame->height < 1 || frame->width > 1920 ||
                frame->height > 1080) {
                fail(AVERROR(EINVAL));
                return;
            }
            decoder.conversion = sws_getCachedContext(
                decoder.conversion, frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
                AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!decoder.conversion) {
                fail(AVERROR(ENOMEM));
                return;
            }
            VideoPixels pixels;
            pixels.width = frame->width;
            pixels.height = frame->height;
            pixels.time = time;
            pixels.duration = sample.duration;
            pixels.allocate();
            uint8_t *planes[] = {pixels.bytes.data(), nullptr, nullptr, nullptr};
            int strides[] = {pixels.stride, 0, 0, 0};
            if (sws_scale(decoder.conversion, frame->data, frame->linesize, 0, frame->height,
                          planes, strides) != frame->height) {
                fail(AVERROR(EIO));
                return;
            }
            std::lock_guard lock(m_state->mutex);
            if (m_state->frames.size() < 16)
                m_state->frames.push_back(std::move(pixels));
            else
                m_state->error = AVERROR(ENOMEM);
        } else {
            if (frame->sample_rate < 8000 || frame->sample_rate > 192000 || frame->nb_samples < 1 ||
                frame->nb_samples > 32768) {
                fail(AVERROR(EINVAL));
                return;
            }
            if (!decoder.resampler) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                int error = swr_alloc_set_opts2(
                    &decoder.resampler, &stereo, AV_SAMPLE_FMT_S16, 48000, &frame->ch_layout,
                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
                if (error < 0 || !decoder.resampler || swr_init(decoder.resampler) < 0) {
                    fail(AVERROR(EINVAL));
                    return;
                }
            }

            int maximum = swr_get_out_samples(decoder.resampler, frame->nb_samples);
            if (maximum < 1 || maximum > 262144) {
                fail(AVERROR(EINVAL));
                return;
            }
            DecodeState::AudioPixels sound;
            sound.time = time;
            sound.samples.resize(maximum * 2);
            uint8_t *out = reinterpret_cast<uint8_t *>(sound.samples.data());
            int count =
                swr_convert(decoder.resampler, &out, maximum,
                            const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
            if (count < 0) {
                fail(count);
                return;
            }
            sound.samples.resize(count * 2);
            unsigned trim = std::min<unsigned>(count, std::max(0., sample.trim) * 48000);
            if (trim) {
                sound.samples.erase(sound.samples.begin(), sound.samples.begin() + trim * 2);
                sound.time += trim / 48000.;
                count -= trim;
            }
            if (!count)
                return;
            std::lock_guard lock(m_state->mutex);
            if (m_state->soundFrames + count > 288000) {
                m_state->error = AVERROR(ENOMEM);
                return;
            }
            m_state->soundFrames += count;
            m_state->sound.push_back(std::move(sound));
        }
    }
};

NativeMediaSource::NativeMediaSource(MediaModernOS &parent, MediaSourcePrivateClient &client,
                                     std::shared_ptr<DecodeState> state)
    : MediaSourcePrivate(client), m_parent(parent), m_state(std::move(state))
#if !RELEASE_LOG_DISABLED
      ,
      m_logger(Logger::create(this))
#endif
{
}

Ref<NativeMediaSource> NativeMediaSource::create(MediaModernOS &parent,
                                                 MediaSourcePrivateClient &client,
                                                 std::shared_ptr<DecodeState> state) {
    return adoptRef(*new NativeMediaSource(parent, client, std::move(state)));
}

RefPtr<MediaPlayerPrivateInterface> NativeMediaSource::player() const {
    return m_parent.get();
}

void NativeMediaSource::setPlayer(MediaPlayerPrivateInterface *player) {
    m_parent = player ? static_cast<MediaModernOS *>(player) : nullptr;
}

MediaSourcePrivate::AddStatus NativeMediaSource::addSourceBuffer(const ContentType &type,
                                                                 const MediaSourceConfiguration &,
                                                                 RefPtr<SourceBufferPrivate> &out) {
    if (nativeMSESupport(type) == MediaPlayer::SupportsType::IsNotSupported)
        return AddStatus::NotSupported;
    {
        Locker locker{m_lock};
        if (m_sourceBuffers.size() >= 4)
            return AddStatus::ReachedIdLimit;
        Ref buffer = adoptRef(*new NativeSourceBuffer(*this, ++m_nextSource));
        out = buffer.ptr();
        m_sourceBuffers.append(WTF::move(buffer));
    }

    out->setMediaSourceDuration(duration());
    return AddStatus::Ok;
}

void NativeMediaSource::durationChanged(const MediaTime &duration) {
    MediaSourcePrivate::durationChanged(duration);
    std::lock_guard lock(m_state->mutex);
    m_state->duration = duration.isFinite() ? duration.toDouble() : 0;
}

void NativeMediaSource::markEndOfStream(EndOfStreamStatus status) {
    MediaSourcePrivate::markEndOfStream(status);
    std::lock_guard lock(m_state->mutex);
    m_state->ended = true;
}

void NativeMediaSource::pump() {
    if (m_pumpPending.exchange(true) || m_state->cancelled)
        return;
    m_dispatcher->dispatch([self = Ref{*this}] {
        if (!self->m_state->cancelled)
            for (auto &source : self->sourceBuffers())
                static_cast<NativeSourceBuffer &>(source.get()).resume();
        self->m_pumpPending = false;
    });
}
