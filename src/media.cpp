#include "media.hpp"
#include <cmath>
#include <fstream>
extern "C" {
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}
namespace nco {
void avcheck(int n, const char* w) {
    if (n < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(n, b, sizeof(b));
        throw std::runtime_error(std::string(w) + ": " + b);
    }
}
static Track trackOf(AVCodecContext* c) {
    auto p = avcodec_parameters_alloc();
    if (!p)
        throw std::bad_alloc();
    std::shared_ptr<AVCodecParameters> par(
        p, [](AVCodecParameters* v) { avcodec_parameters_free(&v); });
    avcheck(avcodec_parameters_from_context(p, c), "Encoder parameters");
    return {par,
            c->time_base,
            c->codec_type == AVMEDIA_TYPE_VIDEO ? c->framerate : AVRational{0, 1},
            {}};
}
static std::vector<Packet> receive(AVCodecContext* c, int stream) {
    std::vector<Packet> out;
    while (true) {
        auto p = av_packet_alloc();
        if (!p)
            throw std::bad_alloc();
        int ret = avcodec_receive_packet(c, p);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&p);
            break;
        }
        if (ret < 0) {
            av_packet_free(&p);
            avcheck(ret, "Receive encoded packet");
        }
        out.push_back({std::shared_ptr<AVPacket>(p, [](AVPacket* v) { av_packet_free(&v); }),
                       stream, c->time_base});
    }
    return out;
}
static void configureVideo(AVCodecContext* c, const std::string& name, const Config& cfg,
                           Size output, AVPixelFormat format) {
    c->width = output.w;
    c->height = output.h;
    c->pix_fmt = format;
    c->time_base = {1, cfg.fps};
    c->framerate = {cfg.fps, 1};
    c->gop_size = cfg.fps * 2;
    c->max_b_frames = 0;
    c->bit_rate = cfg.bitrateKbps ? cfg.bitrateKbps * 1000
                                  : bitrate(output, cfg.fps, cfg.quality,
                                            name.find("264") == std::string::npos);
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    c->color_range = AVCOL_RANGE_MPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    if (name == "libx264") {
        av_opt_set(c->priv_data, "preset", "ultrafast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
        if (!cfg.bitrateKbps) {
            constexpr int crf[] = {32, 28, 23, 20, 17};
            av_opt_set_int(c->priv_data, "crf", crf[cfg.quality], 0);
        }
        c->thread_count = std::clamp(int(std::thread::hardware_concurrency() / 2), 1, 8);
    }
    if (name.find("nvenc") != std::string::npos) {
        av_opt_set(c->priv_data, "preset", "p3", 0);
        av_opt_set(c->priv_data, "tune", "ull", 0);
        av_opt_set_int(c->priv_data, "rc-lookahead", 0, 0);
    }
    if (name.find("amf") != std::string::npos) {
        av_opt_set(c->priv_data, "usage", "ultralowlatency", 0);
        av_opt_set(c->priv_data, "quality", "speed", 0);
    }
    if (name.find("qsv") != std::string::npos)
        av_opt_set(c->priv_data, "preset", "veryfast", 0);
    if (name == "libvpx-vp9") {
        av_opt_set(c->priv_data, "deadline", "realtime", 0);
        av_opt_set_int(c->priv_data, "cpu-used", 8, 0);
        av_opt_set_int(c->priv_data, "lag-in-frames", 0, 0);
        c->thread_count = 4;
    }
}
std::vector<EncoderChoice> probeEncoders() {
    av_log_set_level(AV_LOG_ERROR);
    std::vector<EncoderChoice> out;
    const EncoderChoice list[] = {{"h264_nvenc", L"NVIDIA · H.264", true},
                                  {"hevc_nvenc", L"NVIDIA · HEVC", true},
                                  {"av1_nvenc", L"NVIDIA · AV1", true},
                                  {"h264_amf", L"AMD · H.264", true},
                                  {"hevc_amf", L"AMD · HEVC", true},
                                  {"av1_amf", L"AMD · AV1", true},
                                  {"h264_qsv", L"Intel · H.264", true},
                                  {"hevc_qsv", L"Intel · HEVC", true},
                                  {"av1_qsv", L"Intel · AV1", true},
                                  {"libx264", L"Software · H.264", false},
                                  {"libvpx-vp9", L"Software · VP9 (WebM)", false}};
    Config cfg;
    cfg.fps = 30;
    for (auto& item : list) {
        auto codec = avcodec_find_encoder_by_name(item.name.c_str());
        if (!codec)
            continue;
        auto c = avcodec_alloc_context3(codec);
        if (!c)
            continue;
        configureVideo(c, item.name, cfg, {640, 360},
                       item.gpu ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P);
        if (avcodec_open2(c, codec, nullptr) >= 0)
            out.push_back(item);
        avcodec_free_context(&c);
    }
    return out;
}
VideoEncoder::~VideoEncoder() {
    av_frame_free(&frame_);
    avcodec_free_context(&codec_);
    av_buffer_unref(&frames_);
    av_buffer_unref(&device_);
    sws_freeContext(sws_);
}
void VideoEncoder::open(const Config& cfg, Capture* capture, Size input, bool synthetic) {
    capture_ = capture;
    input_ = input;
    output_ = resolution(cfg.resolutionMode, cfg.width, cfg.height, input);
    viewport_ = letterbox(input, output_);
    std::vector<std::string> names;
    if (cfg.encoder != "auto")
        names.push_back(cfg.encoder);
    else if (cfg.format == "webm")
        names = {"av1_nvenc", "av1_amf", "av1_qsv", "libvpx-vp9"};
    else
        names = {"h264_nvenc", "h264_amf", "h264_qsv", "libx264"};
    std::string failures;
    for (auto& name : names) {
        if (!containerCompatible(cfg.format, name))
            continue;
        const auto codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec)
            continue;
        bool canGpu =
            !synthetic && capture &&
            (name.find("nvenc") != std::string::npos || name.find("amf") != std::string::npos);
        for (int attempt = canGpu ? 0 : 1; attempt < 2; ++attempt) {
            try {
                codec_ = avcodec_alloc_context3(codec);
                if (!codec_)
                    throw std::bad_alloc();
                gpu_ = attempt == 0;
                AVPixelFormat fmt =
                    gpu_ ? AV_PIX_FMT_D3D11
                         : (name.find("lib") == 0 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12);
                configureVideo(codec_, name, cfg, output_, fmt);
                if (gpu_) {
                    device_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
                    if (!device_)
                        throw std::bad_alloc();
                    auto hw = reinterpret_cast<AVHWDeviceContext*>(device_->data);
                    auto d = reinterpret_cast<AVD3D11VADeviceContext*>(hw->hwctx);
                    d->device = capture->device();
                    d->device->AddRef();
                    d->device_context = capture->context();
                    d->device_context->AddRef();
                    avcheck(av_hwdevice_ctx_init(device_), "Hardware device");
                    frames_ = av_hwframe_ctx_alloc(device_);
                    if (!frames_)
                        throw std::bad_alloc();
                    auto f = reinterpret_cast<AVHWFramesContext*>(frames_->data);
                    f->format = AV_PIX_FMT_D3D11;
                    f->sw_format = AV_PIX_FMT_NV12;
                    f->width = output_.w;
                    f->height = output_.h;
                    f->initial_pool_size = 6;
                    auto fd = reinterpret_cast<AVD3D11VAFramesContext*>(f->hwctx);
                    fd->BindFlags = D3D11_BIND_RENDER_TARGET;
                    avcheck(av_hwframe_ctx_init(frames_), "Hardware frame pool");
                    codec_->hw_frames_ctx = av_buffer_ref(frames_);
                    check(capture->device()->QueryInterface(IID_PPV_ARGS(&vd_)),
                          "D3D video device");
                    check(capture->context()->QueryInterface(IID_PPV_ARGS(&vc_)),
                          "D3D video context");
                    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
                    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
                    desc.InputWidth = input.w;
                    desc.InputHeight = input.h;
                    desc.OutputWidth = output_.w;
                    desc.OutputHeight = output_.h;
                    desc.InputFrameRate = {UINT(cfg.fps), 1};
                    desc.OutputFrameRate = desc.InputFrameRate;
                    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
                    check(vd_->CreateVideoProcessorEnumerator(&desc, &enumerator_),
                          "GPU color conversion");
                    check(vd_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_),
                          "GPU video processor");
                }
                avcheck(avcodec_open2(codec_, codec, nullptr), "Open video encoder");
                frame_ = av_frame_alloc();
                if (!frame_)
                    throw std::bad_alloc();
                frame_->format = fmt;
                frame_->width = output_.w;
                frame_->height = output_.h;
                frame_->color_range = AVCOL_RANGE_MPEG;
                frame_->colorspace = AVCOL_SPC_BT709;
                if (!gpu_) {
                    avcheck(av_frame_get_buffer(frame_, 32), "Video frame buffer");
                    sws_ =
                        sws_getContext(input.w, input.h, AV_PIX_FMT_BGRA, viewport_.w, viewport_.h,
                                       fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    if (!sws_)
                        throw std::runtime_error("CPU color converter unavailable");
                    const int* coeff = sws_getCoefficients(SWS_CS_ITU709);
                    sws_setColorspaceDetails(sws_, coeff, 1, coeff, 0, 0, 1 << 16, 1 << 16);
                }
                selected = name;
                Log::get().write("Encoder: " + name +
                                 (gpu_ ? " GPU texture path" : " CPU upload path"));
                return;
            } catch (const std::exception& e) {
                failures += name + ": " + e.what() + "; ";
                av_frame_free(&frame_);
                avcodec_free_context(&codec_);
                av_buffer_unref(&frames_);
                av_buffer_unref(&device_);
                sws_freeContext(sws_);
                sws_ = nullptr;
                processor_.Reset();
                enumerator_.Reset();
                vd_.Reset();
                vc_.Reset();
            }
        }
    }
    throw std::runtime_error("No compatible encoder could start. " + failures);
}
Track VideoEncoder::track() const {
    return trackOf(codec_);
}
std::vector<Packet> VideoEncoder::send(AVFrame* f) {
    avcheck(avcodec_send_frame(codec_, f), "Submit video");
    return receive(codec_, 0);
}
void VideoEncoder::prepareCpu(const uint8_t* b, int stride) {
    avcheck(av_frame_make_writable(frame_), "Writable video frame");
    const uint8_t* src[] = {b};
    int lines[] = {stride};
    if (viewport_.w != output_.w || viewport_.h != output_.h) {
        ptrdiff_t strides[4] = {frame_->linesize[0], frame_->linesize[1], frame_->linesize[2], 0};
        avcheck(av_image_fill_black(frame_->data, strides, AVPixelFormat(frame_->format),
                                    AVCOL_RANGE_MPEG, output_.w, output_.h),
                "Letterbox fill");
    }
    uint8_t* dest[4] = {frame_->data[0] + viewport_.y * frame_->linesize[0] + viewport_.x,
                        frame_->data[1] + viewport_.y / 2 * frame_->linesize[1] +
                            (frame_->format == AV_PIX_FMT_NV12 ? viewport_.x : viewport_.x / 2),
                        frame_->data[2] ? frame_->data[2] + viewport_.y / 2 * frame_->linesize[2] +
                                              viewport_.x / 2
                                        : nullptr,
                        nullptr};
    sws_scale(sws_, src, lines, 0, input_.h, dest, frame_->linesize);
}
std::vector<Packet> VideoEncoder::encode(ID3D11Texture2D* t, int64_t pts, bool key) {
    if (gpu_) {
        av_frame_unref(frame_);
        avcheck(av_hwframe_get_buffer(frames_, frame_, 0), "GPU encoder frame");
        auto out = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
        UINT slice = UINT(reinterpret_cast<uintptr_t>(frame_->data[1]));
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        id.Texture2D.MipSlice = 0;
        id.Texture2D.ArraySlice = 0;
        Com<ID3D11VideoProcessorInputView> iv;
        check(vd_->CreateVideoProcessorInputView(t, enumerator_.Get(), &id, &iv), "GPU input view");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        od.Texture2DArray.MipSlice = 0;
        od.Texture2DArray.FirstArraySlice = slice;
        od.Texture2DArray.ArraySize = 1;
        Com<ID3D11VideoProcessorOutputView> ov;
        check(vd_->CreateVideoProcessorOutputView(out, enumerator_.Get(), &od, &ov),
              "GPU output view");
        RECT src{0, 0, input_.w, input_.h};
        RECT dst{viewport_.x, viewport_.y, viewport_.x + viewport_.w, viewport_.y + viewport_.h};
        RECT full{0, 0, output_.w, output_.h};
        vc_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &src);
        vc_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dst);
        vc_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &full);
        D3D11_VIDEO_COLOR black{};
        black.YCbCr = {16.f / 255, .5f, .5f, 1};
        vc_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), TRUE, &black);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
        rgb.RGB_Range = 0;
        rgb.YCbCr_Matrix = 1;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
        yuv.YCbCr_Matrix = 1;
        yuv.Nominal_Range = 1;
        vc_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &rgb);
        vc_->VideoProcessorSetOutputColorSpace(processor_.Get(), &yuv);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = iv.Get();
        check(vc_->VideoProcessorBlt(processor_.Get(), ov.Get(), 0, 1, &stream),
              "GPU scale and NV12 conversion");
    } else {
        auto b = capture_->download(t);
        prepareCpu(b.data(), input_.w * 4);
    }
    frame_->pts = pts;
    frame_->duration = 1;
    frame_->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    return send(frame_);
}
std::vector<Packet> VideoEncoder::synthetic(int64_t pts, bool key) {
    std::vector<uint8_t> b(size_t(input_.w) * input_.h * 4);
    for (int y = 0; y < input_.h; ++y)
        for (int x = 0; x < input_.w; ++x) {
            size_t o = (size_t(y) * input_.w + x) * 4;
            b[o] = uint8_t((x + pts) % 256);
            b[o + 1] = uint8_t((y + pts) % 256);
            b[o + 2] = 100;
            b[o + 3] = 255;
        }
    prepareCpu(b.data(), input_.w * 4);
    frame_->pts = pts;
    frame_->duration = 1;
    frame_->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    return send(frame_);
}
std::vector<Packet> VideoEncoder::flush() {
    return send(nullptr);
}
AudioEncoder::~AudioEncoder() {
    av_frame_free(&frame_);
    avcodec_free_context(&codec_);
    swr_free(&swr_);
}
void AudioEncoder::open(bool webm, int stream) {
    stream_ = stream;
    auto c = avcodec_find_encoder_by_name(webm ? "libopus" : "aac");
    if (!c)
        throw std::runtime_error("Audio encoder unavailable");
    codec_ = avcodec_alloc_context3(c);
    if (!codec_)
        throw std::bad_alloc();
    codec_->sample_rate = 48000;
    codec_->time_base = {1, 48000};
    av_channel_layout_default(&codec_->ch_layout, 2);
    codec_->sample_fmt = webm ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
    codec_->bit_rate = 160000;
    codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    avcheck(avcodec_open2(codec_, c, nullptr), "Open audio encoder");
    frame_ = av_frame_alloc();
    if (!frame_)
        throw std::bad_alloc();
    frame_->format = codec_->sample_fmt;
    frame_->sample_rate = 48000;
    frame_->nb_samples = codec_->frame_size;
    av_channel_layout_copy(&frame_->ch_layout, &codec_->ch_layout);
    avcheck(av_frame_get_buffer(frame_, 0), "Audio frame");
    AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
    avcheck(swr_alloc_set_opts2(&swr_, &layout, codec_->sample_fmt, 48000, &layout,
                                AV_SAMPLE_FMT_FLT, 48000, 0, nullptr),
            "Audio conversion context");
    avcheck(swr_init(swr_), "Audio converter");
}
Track AudioEncoder::track() const {
    return trackOf(codec_);
}
std::vector<Packet> AudioEncoder::send(AVFrame* f) {
    avcheck(avcodec_send_frame(codec_, f), "Submit audio");
    return receive(codec_, stream_);
}
std::vector<Packet> AudioEncoder::encode(const std::vector<float>& samples, int64_t pts) {
    avcheck(av_frame_make_writable(frame_), "Writable audio frame");
    const uint8_t* p = reinterpret_cast<const uint8_t*>(samples.data());
    int n = swr_convert(swr_, frame_->data, frame_->nb_samples, &p, frame_->nb_samples);
    avcheck(n, "Convert audio samples");
    frame_->pts = pts;
    frame_->duration = frame_->nb_samples;
    return send(frame_);
}
std::vector<Packet> AudioEncoder::flush() {
    return send(nullptr);
}
Muxer::~Muxer() {
    try {
        close();
    } catch (...) {
    }
}
void Muxer::open(const std::filesystem::path& path, const std::vector<Track>& tracks,
                 bool fragmented, bool preserveTimeline) {
    preserve_ = preserveTimeline;
    origin_ = preserve_ ? 0 : AV_NOPTS_VALUE;
    wrote_ = false;
    tracks_ = tracks;
    last_.assign(tracks.size(), AV_NOPTS_VALUE);
    std::filesystem::create_directories(path.parent_path());
    avcheck(avformat_alloc_output_context2(&ctx_, nullptr, nullptr, utf8(path.wstring()).c_str()),
            "Create media container");
    if (!ctx_)
        throw std::runtime_error("No muxer for this container");
    for (auto& t : tracks) {
        AVStream* s = avformat_new_stream(ctx_, nullptr);
        if (!s)
            throw std::bad_alloc();
        avcheck(avcodec_parameters_copy(s->codecpar, t.par.get()), "Mux codec parameters");
        s->codecpar->codec_tag = 0;
        s->time_base = t.tb;
        s->avg_frame_rate = t.frameRate;
        if (!t.name.empty()) {
            av_dict_set(&s->metadata, "title", t.name.c_str(), 0);
            av_dict_set(&s->metadata, "handler_name", t.name.c_str(), 0);
        }
    }
    avcheck(avio_open(&ctx_->pb, utf8(path.wstring()).c_str(), AVIO_FLAG_WRITE),
            "Open output file");
    AVDictionary* opts = nullptr;
    if (fragmented && (path.extension() == L".mp4" || path.extension() == L".mov"))
        av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    int ret = avformat_write_header(ctx_, &opts);
    av_dict_free(&opts);
    avcheck(ret, "Write container header");
}
void Muxer::write(const Packet& p, int64_t removed) {
    if (!ctx_)
        return;
    if (origin_ == AV_NOPTS_VALUE) {
        if (!p.key())
            return;
        origin_ = p.us() - removed;
    }
    int64_t shifted = p.us() - removed - origin_;
    if (shifted < 0 && !preserve_)
        return;
    AVPacket* q = av_packet_clone(p.p.get());
    if (!q)
        throw std::bad_alloc();
    int64_t offset = av_rescale_q(origin_ + removed, {1, 1000000}, p.tb);
    q->pts -= offset;
    if (q->dts != AV_NOPTS_VALUE)
        q->dts -= offset;
    q->stream_index = p.stream;
    av_packet_rescale_ts(q, p.tb, ctx_->streams[p.stream]->time_base);
    if (q->dts != AV_NOPTS_VALUE && last_[p.stream] != AV_NOPTS_VALUE &&
        q->dts <= last_[p.stream]) {
        av_packet_free(&q);
        return;
    }
    last_[p.stream] = q->dts;
    int ret = av_interleaved_write_frame(ctx_, q);
    av_packet_free(&q);
    avcheck(ret, "Write media packet");
    wrote_ = true;
}
void Muxer::close() {
    if (!ctx_)
        return;
    int ret = wrote_ ? av_write_trailer(ctx_) : 0;
    if (ctx_->pb)
        avio_closep(&ctx_->pb);
    avformat_free_context(ctx_);
    ctx_ = nullptr;
    avcheck(ret, "Finalize media file");
}
static bool verifyContents(const std::filesystem::path& p,
                           const std::vector<size_t>* expected = nullptr,
                           const std::vector<AVCodecID>* codecs = nullptr) {
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, utf8(p.wstring()).c_str(), nullptr, nullptr) < 0)
        return false;
    bool good = avformat_find_stream_info(ctx, nullptr) >= 0;
    std::vector<size_t> counts(ctx->nb_streams);
    if (expected && expected->size() != counts.size())
        good = false;
    if (codecs) {
        good = good && codecs->size() == counts.size();
        for (size_t i = 0; good && i < counts.size(); ++i)
            good = ctx->streams[i]->codecpar->codec_id == (*codecs)[i];
    }
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        avformat_close_input(&ctx);
        throw std::bad_alloc();
    }
    int ret = 0;
    while (good && (ret = av_read_frame(ctx, packet)) >= 0) {
        if (packet->stream_index >= 0 && unsigned(packet->stream_index) < counts.size())
            ++counts[packet->stream_index];
        av_packet_unref(packet);
    }
    good = good && ret == AVERROR_EOF && !counts.empty() &&
           std::all_of(counts.begin(), counts.end(), [](size_t n) { return n > 0; });
    if (expected)
        good = good && counts == *expected;
    av_packet_free(&packet);
    avformat_close_input(&ctx);
    return good;
}
bool verifyFile(const std::filesystem::path& p) {
    return verifyContents(p);
}
void remuxFile(const std::filesystem::path& source, const std::filesystem::path& target) {
    AVFormatContext* in = nullptr;
    avcheck(avformat_open_input(&in, utf8(source.wstring()).c_str(), nullptr, nullptr),
            "Open remux source");
    try {
        avcheck(avformat_find_stream_info(in, nullptr), "Read remux source");
        std::vector<Track> tracks;
        std::vector<size_t> counts(in->nb_streams);
        std::vector<AVCodecID> codecs;
        for (unsigned i = 0; i < in->nb_streams; ++i) {
            codecs.push_back(in->streams[i]->codecpar->codec_id);
            auto p = avcodec_parameters_alloc();
            if (!p)
                throw std::bad_alloc();
            avcheck(avcodec_parameters_copy(p, in->streams[i]->codecpar), "Remux parameters");
            tracks.push_back({std::shared_ptr<AVCodecParameters>(
                                  p, [](AVCodecParameters* v) { avcodec_parameters_free(&v); }),
                              in->streams[i]->time_base,
                              in->streams[i]->avg_frame_rate,
                              {}});
            auto title = av_dict_get(in->streams[i]->metadata, "title", nullptr, 0);
            if (title)
                tracks.back().name = title->value;
        }
        Muxer output;
        output.open(target, tracks, false, true);
        AVPacket* p = av_packet_alloc();
        if (!p)
            throw std::bad_alloc();
        int ret = 0;
        try {
            while ((ret = av_read_frame(in, p)) >= 0) {
                if (p->stream_index < 0 || unsigned(p->stream_index) >= counts.size())
                    throw std::runtime_error("Invalid stream in remux source; original retained");
                ++counts[size_t(p->stream_index)];
                AVPacket* q = av_packet_clone(p);
                if (!q)
                    throw std::bad_alloc();
                output.write({std::shared_ptr<AVPacket>(q, [](AVPacket* v) { av_packet_free(&v); }),
                              p->stream_index, in->streams[p->stream_index]->time_base});
                av_packet_unref(p);
            }
            avcheck(ret == AVERROR_EOF ? 0 : ret, "Read remux packet");
        } catch (...) {
            av_packet_free(&p);
            throw;
        }
        av_packet_free(&p);
        output.close();
        avformat_close_input(&in);
        if (!verifyContents(target, &counts, &codecs))
            throw std::runtime_error(
                "Remux output stream codecs or packet counts failed verification; original retained");
    } catch (...) {
        if (in)
            avformat_close_input(&in);
        throw;
    }
}
DiskWriter::DiskWriter(std::function<void(std::string, bool)> notify) : notify_(std::move(notify)) {
    exportThread_ = std::jthread([this] { exportRun(); });
    try {
        thread_ = std::jthread([this] { run(); });
    } catch (...) {
        pushExport({Job::Quit});
        exportThread_.join();
        throw;
    }
}
DiskWriter::~DiskWriter() {
    push({Job::Quit});
    if (thread_.joinable())
        thread_.join();
    // The recording worker has now submitted all finalization work.
    pushExport({Job::Quit});
    if (exportThread_.joinable())
        exportThread_.join();
}
void DiskWriter::push(Job job) {
    std::lock_guard lock(mutex_);
    const size_t amount = job.type == Job::Write ? job.packet.bytes() : 0;
    if (bytes_ + amount > 64u * 1024u * 1024u)
        throw std::runtime_error(
            "Recording disk queue exceeded 64 MiB; check storage speed and free space");
    bytes_ += amount;
    queue_.push_back(std::move(job));
    cv_.notify_one();
}
void DiskWriter::pushExport(Job job) {
    std::lock_guard lock(exportMutex_);
    size_t amount = 0;
    for (auto& packet : job.clip)
        amount += packet.bytes();
    if (exportBytes_ + amount > 256u * 1024u * 1024u)
        throw std::runtime_error("Replay export queue is full; wait for pending clips to finish");
    exportBytes_ += amount;
    exports_.push_back(std::move(job));
    exportCv_.notify_one();
}
void DiskWriter::open(std::filesystem::path path, std::vector<Track> tracks, bool remux,
                      std::string format) {
    Job job{Job::Open};
    job.path = std::move(path);
    job.tracks = std::move(tracks);
    job.remux = remux;
    job.format = std::move(format);
    push(std::move(job));
}
void DiskWriter::write(Packet packet, int64_t removed) {
    Job job{Job::Write};
    job.packet = std::move(packet);
    job.removed = removed;
    push(std::move(job));
}
void DiskWriter::close() {
    push({Job::Close});
}
void DiskWriter::replay(std::filesystem::path path, std::vector<Track> tracks,
                        std::vector<Packet> clip) {
    Job job{Job::Replay};
    job.path = std::move(path);
    job.tracks = std::move(tracks);
    job.clip = std::move(clip);
    pushExport(std::move(job));
}
void DiskWriter::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_ptr<Muxer> mux;
    std::filesystem::path final, temporary;
    bool remux = false;
    auto finalize = [&] {
        if (!mux)
            return;
        mux->close();
        mux.reset();
        notify_("Recording stopped", false);
        Job job{remux ? Job::Remux : Job::Verify};
        job.path = final;
        job.source = temporary;
        pushExport(std::move(job));
    };
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return !queue_.empty(); });
            job = std::move(queue_.front());
            queue_.pop_front();
            if (job.type == Job::Write)
                bytes_ -= job.packet.bytes();
        }
        try {
            if (job.type == Job::Quit) {
                finalize();
                break;
            }
            if (job.type == Job::Open) {
                finalize();
                final = job.path;
                remux = job.remux && job.format != "mkv";
                temporary =
                    remux ? std::filesystem::path(final.wstring() + L".recovery.mkv") : final;
                mux = std::make_unique<Muxer>();
                mux->open(temporary, job.tracks, !remux);
                notify_("Recording started", false);
            } else if (job.type == Job::Write) {
                if (mux)
                    mux->write(job.packet, job.removed);
            } else if (job.type == Job::Close)
                finalize();
        } catch (const std::exception& error) {
            Log::get().write(std::string("Recording write failure: ") + error.what());
            mux.reset();
            notify_(std::string("Recording failed: ") + error.what(), true);
            if (job.type == Job::Quit)
                break;
        }
    }
    CoUninitialize();
}
void DiskWriter::exportRun() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    while (true) {
        Job job;
        size_t amount = 0;
        {
            std::unique_lock lock(exportMutex_);
            exportCv_.wait(lock, [&] { return !exports_.empty(); });
            job = std::move(exports_.front());
            exports_.pop_front();
            // Keep the active clip charged to the budget until it has finished.
            for (auto& packet : job.clip)
                amount += packet.bytes();
        }
        if (job.type == Job::Quit)
            break;
        try {
            if (job.type == Job::Replay) {
                if (job.clip.empty())
                    throw std::runtime_error("Replay is still warming up");
                Muxer mux;
                mux.open(job.path, job.tracks);
                for (auto& packet : job.clip)
                    mux.write(packet);
                mux.close();
                if (!verifyFile(job.path))
                    throw std::runtime_error("Replay verification failed");
                notify_("Replay saved", false);
            } else if (job.type == Job::Remux) {
                remuxFile(job.source, job.path);
                std::filesystem::remove(job.source);
            } else if (job.type == Job::Verify && !verifyFile(job.path)) {
                throw std::runtime_error("Recording failed packet verification; file retained");
            }
        } catch (const std::exception& error) {
            const std::string prefix = job.type == Job::Replay  ? "Replay failed: "
                                       : job.type == Job::Remux ? "Remux failed: "
                                                                : "Verification failed: ";
            Log::get().write(prefix + error.what());
            notify_(prefix + error.what(), true);
        }
        {
            std::lock_guard lock(exportMutex_);
            exportBytes_ -= amount;
        }
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    CoUninitialize();
}
} // namespace nco
