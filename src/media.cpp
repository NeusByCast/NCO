#include "media.hpp"
#include "quality_probe.hpp"
#include <cmath>
#include <fstream>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <utility>
#include <dxgi1_6.h>
extern "C" {
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
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
double Packet::quantizer() const {
    if (!p || stream != 0) return -1;
    size_t size = 0;
    const auto data = av_packet_get_side_data(p.get(), AV_PKT_DATA_QUALITY_STATS, &size);
    if (!data || size < 4) return -1;
    const uint32_t quality = AV_RL32(data);
    return quality == UINT32_MAX ? -1 : double(quality) / FF_QP2LAMBDA;
}
static void mediaLog(void*, int level, const char* format, va_list args) {
    if (level > AV_LOG_WARNING)
        return;
    char buffer[2048]{};
    vsnprintf(buffer, sizeof(buffer), format, args);
    std::string line(buffer);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    if (!line.empty())
        Log::get().write("Media: " + line);
}
static std::vector<int> preferredAdapters(UINT vendor = 0) {
    std::vector<int> result;
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
    Com<IDXGIFactory6> modern;
    factory.As(&modern);
    for (UINT rank = 0;; ++rank) {
        Com<IDXGIAdapter1> adapter;
        HRESULT hr = modern ? modern->EnumAdapterByGpuPreference(rank,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
            : factory->EnumAdapters1(rank, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr) || !adapter) break;
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (vendor && d.VendorId != vendor)) continue;
        for (UINT index = 0;; ++index) {
            Com<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 c{};
            if (candidate && SUCCEEDED(candidate->GetDesc1(&c)) &&
                c.AdapterLuid.HighPart == d.AdapterLuid.HighPart &&
                c.AdapterLuid.LowPart == d.AdapterLuid.LowPart) {
                result.push_back(int(index));
                break;
            }
        }
    }
    return result;
}
static UINT captureVendor(Capture* capture) {
    if (!capture) return 0;
    Com<IDXGIDevice> dx;
    Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(capture->device()->QueryInterface(IID_PPV_ARGS(&dx))) ||
        FAILED(dx->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&description))) return 0;
    return description.VendorId;
}
static std::vector<int> amdAdapters() {
    return preferredAdapters(0x1002);
}
static AVBufferRef* adapterDevice(int index) {
    AVBufferRef* device = nullptr;
    const std::string id = std::to_string(index);
    avcheck(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, id.c_str(), nullptr, 0),
            "Open encoder GPU adapter");
    return device;
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
    const int rateControl = std::clamp(cfg.rateControl, 0, 2);
    c->bit_rate = cfg.bitrateKbps ? cfg.bitrateKbps * 1000
                                  : bitrate(output, cfg.fps, cfg.quality,
                                            name.find("264") == std::string::npos);
    // VBR uses the requested bitrate as its target and a 1.5x peak. CBR is
    // constrained to its target. CQP intentionally ignores bitrate limits.
    c->rc_max_rate = rateControl == 0 ? c->bit_rate * 3 / 2
                    : rateControl == 2 ? c->bit_rate : 0;
    c->rc_buffer_size = rateControl == 1 ? 0 : int(std::min<int64_t>(
        int64_t(c->rc_max_rate) * 2, std::numeric_limits<int>::max()));
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    c->color_range = AVCOL_RANGE_MPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    if (name == "libx264rgb") {
        // Preserve desktop RGB samples. This is an explicit CPU mode, never an
        // automatic hardware fallback. Bitrate constraints would defeat lossless.
        c->bit_rate = c->rc_max_rate = c->rc_buffer_size = 0;
        c->color_range = AVCOL_RANGE_JPEG;
        c->colorspace = AVCOL_SPC_RGB;
        avcheck(av_opt_set(c->priv_data, "preset", "ultrafast", 0), "RGB encoder preset");
        avcheck(av_opt_set(c->priv_data, "tune", "zerolatency", 0), "RGB encoder latency");
        avcheck(av_opt_set(c->priv_data, "profile", "high444", 0), "RGB encoder profile");
        avcheck(av_opt_set_int(c->priv_data, "crf", 0, 0), "RGB lossless compression");
        c->thread_count = std::clamp(int(std::thread::hardware_concurrency() / 2), 1, 8);
    }
    if (name == "libx264") {
        av_opt_set(c->priv_data, "preset", "ultrafast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
        if (rateControl == 1) {
            constexpr int qp[] = {36, 32, 27, 23, 19};
            av_opt_set_int(c->priv_data, "qp", qp[std::clamp(cfg.quality, 0, 4)], 0);
        } else if (rateControl == 0 && !cfg.bitrateKbps) {
            constexpr int crf[] = {32, 28, 23, 20, 17};
            av_opt_set_int(c->priv_data, "crf", crf[cfg.quality], 0);
        }
        if (rateControl == 2)
            av_opt_set(c->priv_data, "nal-hrd", "cbr", 0);
        c->thread_count = std::clamp(int(std::thread::hardware_concurrency() / 2), 1, 8);
    }
    if (name.find("nvenc") != std::string::npos) {
        av_opt_set(c->priv_data, "preset", cfg.quality >= 3 ? "p5" : cfg.quality >= 1 ? "p4" : "p3", 0);
        av_opt_set(c->priv_data, "tune", cfg.quality >= 2 ? "hq" : "ull", 0);
        av_opt_set_int(c->priv_data, "rc-lookahead", 0, 0);
        const char* rc = rateControl == 1 ? "constqp" : rateControl == 2 ? "cbr" : "vbr";
        av_opt_set(c->priv_data, "rc", rc, 0);
        if (rateControl == 1) {
            constexpr int qp[] = {36, 32, 27, 23, 19};
            av_opt_set_int(c->priv_data, "qp", qp[std::clamp(cfg.quality, 0, 4)], 0);
        }
    }
    if (name.find("amf") != std::string::npos) {
        av_opt_set(c->priv_data, "usage", "lowlatency", 0);
        // Keep one frame of encoder overlap. LOW_DELAY makes FFmpeg poll each
        // submitted surface immediately; it is unnecessary for file recording.
        const char* amfQuality = cfg.quality == 0 ? "speed" : "balanced";
        int qualityResult = av_opt_set(c->priv_data, "quality", amfQuality, 0);
        if (qualityResult < 0)
            Log::get().write("AMF quality preset could not be set: " + std::string(amfQuality));

        const char* amfRc = rateControl == 1 ? "cqp"
                          : rateControl == 2 ? "cbr"
                                             : "vbr_peak";
        int rcResult = av_opt_set(c->priv_data, "rc", amfRc, 0);
        if (rcResult < 0)
            Log::get().write("AMF rate control could not be set: " + std::string(amfRc));

        // AMF can emit a non-IDR I picture for a generic AV_PICTURE_TYPE_I
        // request. Recordings and replay segments must start on a decodable
        // boundary, so explicitly ask AMF to turn forced I pictures into IDRs.
        av_opt_set_int(c->priv_data, "forced_idr", 1, 0);

        // Lookahead needs more input surfaces than a bounded real-time worker
        // can supply. Enabling it can stall AMF before the first output packet.
        av_opt_set_int(c->priv_data, "preencode", 0, 0);
        av_opt_set_int(c->priv_data, "preanalysis", 0, 0);
        av_opt_set_int(c->priv_data, "pa_lookahead_buffer_depth", 0, 0);
        av_opt_set_int(c->priv_data, "vbaq", 0, 0);
        // Image quality and encoder workload are separate: preserve detail
        // without selecting the slower AMF Quality processing preset.
        constexpr int qp[] = {36, 32, 27, 23, 19};
        c->qmin = 0;
        const int scale = name == "av1_amf" ? 4 : 1;
        c->qmax = rateControl == 0 ? (qp[std::clamp(cfg.quality, 0, 4)] + 3) * scale
                                  : name == "av1_amf" ? 255 : 51;
        if (rateControl == 1) {
            const int selectedQp = qp[std::clamp(cfg.quality, 0, 4)] * scale;
            av_opt_set_int(c->priv_data, "qp_i", selectedQp, 0);
            av_opt_set_int(c->priv_data, "qp_p", selectedQp, 0);
        }
        av_opt_set_int(c->priv_data, "frame_skipping", 0, 0);
        av_opt_set_int(c->priv_data, "async_depth", 3, 0);
    }
    if (name.find("_mf") != std::string::npos) {
        av_opt_set_int(c->priv_data, "hw_encoding", 1, 0);
        av_opt_set(c->priv_data, "scenario", "display_remoting", 0);
    }
    if (name.find("qsv") != std::string::npos)
        av_opt_set(c->priv_data, "preset", "veryfast", 0);
    if (name.find("qsv") != std::string::npos && rateControl == 1) {
        constexpr int qp[] = {36, 32, 27, 23, 19};
        c->flags |= AV_CODEC_FLAG_QSCALE;
        c->global_quality = qp[std::clamp(cfg.quality, 0, 4)] * FF_QP2LAMBDA;
    }
    if (name == "libvpx-vp9") {
        av_opt_set(c->priv_data, "deadline", "realtime", 0);
        av_opt_set_int(c->priv_data, "cpu-used", 8, 0);
        av_opt_set_int(c->priv_data, "lag-in-frames", 0, 0);
        c->thread_count = 4;
    }
}
void encoderConfigurationSelfTest() {
    for (const char* name : {"h264_amf", "hevc_amf", "av1_amf"}) {
        const auto codec = avcodec_find_encoder_by_name(name);
        if (!codec) throw std::runtime_error("AMF regression requires compiled encoder support");
        for (int quality = 0; quality <= 4; ++quality) for (int rate = 0; rate <= 2; ++rate) {
            auto release = [](AVCodecContext* c) { avcodec_free_context(&c); };
            std::unique_ptr<AVCodecContext, decltype(release)> c(avcodec_alloc_context3(codec), release);
            if (!c) throw std::bad_alloc();
            Config config;
            config.quality = quality;
            config.rateControl = rate;
            configureVideo(c.get(), name, config, {1920, 1080}, AV_PIX_FMT_D3D11);
            auto option = [&](const char* key) {
                int64_t value = -1;
                avcheck(av_opt_get_int(c->priv_data, key, 0, &value), key);
                return value;
            };
            // Each codec uses its own numeric preset values. Resolve the named
            // constant with FFmpeg instead of assuming H.264 values fit HEVC/AV1.
            const auto preset = av_opt_find(c->priv_data, quality == 0 ? "speed" : "balanced",
                                           "quality", 0, 0);
            constexpr int qp[] = {36, 32, 27, 23, 19};
            const int scale = std::string(name) == "av1_amf" ? 4 : 1;
            if (option("preencode") || option("preanalysis") || option("pa_lookahead_buffer_depth") ||
                option("async_depth") < 3 || c->max_b_frames || (c->flags & AV_CODEC_FLAG_LOW_DELAY) ||
                !preset || option("quality") != preset->default_val.i64)
                throw std::runtime_error("AMF real-time processing or bounded frame overlap regressed");
            if (c->qmin != 0 || (rate == 0 && c->qmax != (qp[quality] + 3) * scale) ||
                (rate == 1 && (option("qp_i") != qp[quality] * scale || option("qp_p") != qp[quality] * scale)) ||
                (rate == 2 && c->rc_max_rate != c->bit_rate))
                throw std::runtime_error("AMF quality/rate-control settings regressed");
        }
    }
    Packet packet{std::shared_ptr<AVPacket>(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); }), 0, {1, 60}};
    if (!packet.p) throw std::bad_alloc();
    if (packet.quantizer() != -1) throw std::runtime_error("Missing quantizer must be unknown");
    auto data = av_packet_new_side_data(packet.p.get(), AV_PKT_DATA_QUALITY_STATS, 4);
    if (!data) throw std::bad_alloc();
    AV_WL32(data, 22 * FF_QP2LAMBDA);
    if (packet.quantizer() != 22) throw std::runtime_error("Reported quantizer scale is incorrect");
    packet.stream = 1;
    if (packet.quantizer() != -1) throw std::runtime_error("Audio cannot report video quantizers");
}
std::vector<EncoderChoice> probeEncoders() {
    av_log_set_callback(mediaLog);
    av_log_set_level(AV_LOG_WARNING);
    Log::get().write("FFmpeg codec version: " + std::to_string(avcodec_version()));
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
                                  {"h264_mf", L"Windows GPU · H.264", true},
                                  {"hevc_mf", L"Windows GPU · HEVC", true},
                                  {"libx264", L"CPU · H.264", false},
                                  {"libx264rgb", L"CPU · Full-colour RGB (lossless)", false},
                                  {"libvpx-vp9", L"Software · VP9 (WebM)", false}};
    Config cfg;
    cfg.fps = 30;
    for (auto& item : list) {
        auto codec = avcodec_find_encoder_by_name(item.name.c_str());
        if (!codec) {
            Log::get().write("Encoder not compiled: " + item.name);
            continue;
        }
        std::vector<int> adapters = {-1};
        if (item.name.find("amf") != std::string::npos) {
            auto amd = amdAdapters();
            adapters.insert(adapters.begin(), amd.begin(), amd.end());
        }
        for (int index : adapters) {
            auto c = avcodec_alloc_context3(codec);
            if (!c)
                continue;
            bool supported = false;
            try {
                configureVideo(c, item.name, cfg, {640, 360},
                               item.name == "libx264rgb" ? AV_PIX_FMT_BGR0
                               : item.gpu ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P);
                if (index >= 0)
                    c->hw_device_ctx = adapterDevice(index);
                avcheck(avcodec_open2(c, codec, nullptr), "Probe encoder");
                supported = true;
                Log::get().write("Encoder available: " + item.name + " adapter=" + std::to_string(index));
            } catch (const std::exception& e) {
                Log::get().write("Encoder probe failed: " + item.name + " adapter=" +
                                 std::to_string(index) + " " + e.what());
            }
            avcodec_free_context(&c);
            if (supported) {
                out.push_back(item);
                break;
            }
        }
    }
    return out;
}
VideoEncoder::VideoEncoder() = default;
VideoEncoder::~VideoEncoder() {
    if (qualityProbe_ || (capture_ && qualityState.load() == 1))
        failQualityComparison("Encoder stopped before the comparison completed");
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
        names = {"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf", "libx264"};
    if (cfg.encoder == "auto") {
        auto adapters = preferredAdapters();
        Com<IDXGIFactory1> factory;
        Com<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (!adapters.empty() && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapters1(UINT(adapters.front()), &adapter)) &&
            SUCCEEDED(adapter->GetDesc1(&desc))) {
            std::string vendor = desc.VendorId == 0x1002 ? "amf" :
                                 desc.VendorId == 0x10de ? "nvenc" : "qsv";
            std::stable_partition(names.begin(), names.end(), [&](const auto& name) {
                return name.find(vendor) != std::string::npos;
            });
            Log::get().write("Preferred encoder GPU: " + utf8(desc.Description));
        }
    }
    std::string failures;
    for (auto& name : names) {
        if (!containerCompatible(cfg.format, name))
            continue;
        const auto codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec)
            continue;
        const bool canGpu = !synthetic && capture &&
                            textureEncoderCompatible(name, captureVendor(capture));
        std::vector<int> attempts;
        if (canGpu)
            attempts.push_back(-2); // capture device, zero readback
        if (name.find("amf") != std::string::npos) {
            auto amd = amdAdapters();
            for (int index : amd)
                attempts.push_back(index);
        }
        attempts.push_back(-1); // library-selected device / software encoder
        for (int attempt : attempts) {
            try {
                codec_ = avcodec_alloc_context3(codec);
                if (!codec_)
                    throw std::bad_alloc();
                gpu_ = attempt == -2;
                AVPixelFormat fmt =
                    gpu_ ? AV_PIX_FMT_D3D11
                         : name == "libx264rgb" ? AV_PIX_FMT_BGR0
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
                    // Individual NV12 textures: several drivers reject render-target arrays.
                    f->initial_pool_size = 0;
                    auto fd = reinterpret_cast<AVD3D11VAFramesContext*>(f->hwctx);
                    fd->BindFlags = D3D11_BIND_RENDER_TARGET;
                    avcheck(av_hwframe_ctx_init(frames_), "Hardware frame pool");
                    AVFrame* probe = av_frame_alloc();
                    if (!probe) throw std::bad_alloc();
                    const int allocation = av_hwframe_get_buffer(frames_, probe, 0);
                    av_frame_free(&probe);
                    avcheck(allocation, "GPU encoder texture preflight");
                    codec_->hw_frames_ctx = av_buffer_ref(frames_);
                }
                if (capture && (gpu_ || fmt == AV_PIX_FMT_NV12)) {
                    try {
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
                    vc_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
                    vc_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
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
                    } catch (const std::exception& error) {
                        if (gpu_) throw;
                        Log::get().write(std::string("GPU upload conversion unavailable; using CPU conversion: ") + error.what());
                        processor_.Reset();
                        enumerator_.Reset();
                        vd_.Reset();
                        vc_.Reset();
                    }
                }
                if (attempt >= 0) {
                    device_ = adapterDevice(attempt);
                    codec_->hw_device_ctx = av_buffer_ref(device_);
                    if (!codec_->hw_device_ctx)
                        throw std::bad_alloc();
                }
                avcheck(avcodec_open2(codec_, codec, nullptr), "Open video encoder");
                frame_ = av_frame_alloc();
                if (!frame_)
                    throw std::bad_alloc();
                frame_->format = fmt;
                frame_->width = output_.w;
                frame_->height = output_.h;
                frame_->color_range = codec_->color_range;
                frame_->colorspace = codec_->colorspace;
                if (!gpu_) {
                    avcheck(av_frame_get_buffer(frame_, 32), "Video frame buffer");
                    // Native RGB frames can be copied directly, without scaling
                    // or any RGB/YUV colour conversion.
                    if (fmt != AV_PIX_FMT_BGR0 || input.w != viewport_.w || input.h != viewport_.h) {
                        const int scaling = fmt == AV_PIX_FMT_BGR0 ? SWS_BICUBIC | SWS_ACCURATE_RND
                                                                  : SWS_FAST_BILINEAR;
                        sws_ = sws_getContext(input.w, input.h, AV_PIX_FMT_BGRA, viewport_.w, viewport_.h,
                                              fmt, scaling, nullptr, nullptr, nullptr);
                        if (!sws_)
                            throw std::runtime_error("CPU color converter unavailable");
                        const int* coeff = sws_getCoefficients(SWS_CS_ITU709);
                        sws_setColorspaceDetails(sws_, coeff, 1, coeff, fmt == AV_PIX_FMT_BGR0 ? 1 : 0,
                                                 0, 1 << 16, 1 << 16);
                    }
                }
                nv12Readback_ = !gpu_ && capture && fmt == AV_PIX_FMT_NV12 && processor_;

                if (nv12Readback_) {
                    try {
                    D3D11_TEXTURE2D_DESC d{};
                    d.Width = output_.w;
                    d.Height = output_.h;
                    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
                    d.Format = DXGI_FORMAT_NV12;
                    d.BindFlags = D3D11_BIND_RENDER_TARGET;
                    check(capture->device()->CreateTexture2D(&d, nullptr, &uploadNv12_),
                          "NV12 upload conversion texture");
                    d.BindFlags = 0;
                    d.Usage = D3D11_USAGE_STAGING;
                    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    for (auto& stage : uploadStages_)
                        check(capture->device()->CreateTexture2D(&d, nullptr, &stage),
                              "NV12 upload staging texture");
                    } catch (const std::exception& error) {
                        Log::get().write(std::string("NV12 readback unavailable; retaining hardware encoding with CPU conversion: ") + error.what());
                        nv12Readback_ = false;
                        uploadNv12_.Reset();
                        for (auto& stage : uploadStages_) stage.Reset();
                    }
                }
                selected = name;
                selectedAdapter = name.find("lib") == 0 ? "CPU" : "library-selected adapter";
                if (attempt >= 0) {
                    Com<IDXGIFactory1> factory;
                    Com<IDXGIAdapter1> adapter;
                    DXGI_ADAPTER_DESC1 description{};
                    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                        SUCCEEDED(factory->EnumAdapters1(UINT(attempt), &adapter)) &&
                        SUCCEEDED(adapter->GetDesc1(&description)))
                        selectedAdapter = utf8(description.Description);
                } else if (gpu_) {
                    Com<IDXGIDevice> dxgi;
                    Com<IDXGIAdapter> adapter;
                    DXGI_ADAPTER_DESC description{};
                    if (SUCCEEDED(capture->device()->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
                        SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
                        SUCCEEDED(adapter->GetDesc(&description)))
                        selectedAdapter = utf8(description.Description);
                }
                Log::get().write("Encoding adapter: " + selectedAdapter);
                Log::get().write("Video output=" + std::to_string(output_.w) + "x" +
                    std::to_string(output_.h) + " fps=" + std::to_string(cfg.fps) +
                    " bitrate_bps=" + std::to_string(codec_->bit_rate) +
                    " quality=" + std::to_string(cfg.quality) + " container=" + cfg.format +
                    " GPU_NV12_readback=" + std::to_string(nv12Readback_));
                const char* preset = name == "libx264rgb" ? "ultrafast/zerolatency"
                                     : name.find("amf") != std::string::npos
                                         ? cfg.quality == 0 ? "speed" : "balanced"
                                     : name.find("nvenc") != std::string::npos
                                         ? cfg.quality >= 3 ? "p5/hq" : cfg.quality >= 1 ? "p4" : "p3/ull"
                                     : name.find("qsv") != std::string::npos ? "veryfast"
                                                                              : "codec-default";
                const char* rateMode = name == "libx264rgb" ? "lossless RGB (CRF 0)"
                                     : cfg.rateControl == 1 ? "CQP"
                                     : cfg.rateControl == 2 ? "CBR"
                                                            : "VBR peak constrained";
                const char* bitrateRole = name == "libx264rgb" ? "ignored_by_lossless"
                                       : cfg.rateControl == 1 ? "ignored_by_CQP" : "target";
                Log::get().write("Encoder quality mode=" + std::string(preset) +
                                 " rate_control=" + rateMode +
                                 " bitrate_role=" + bitrateRole +
                                 " target_bps=" + std::to_string(codec_->bit_rate) +
                                 " peak_bps=" + std::to_string(codec_->rc_max_rate) +
                                 " qmin=" + std::to_string(codec_->qmin) +
                                 " qmax=" + std::to_string(codec_->qmax) +
                                 " preencode=disabled lookahead=disabled video_auto_processing=disabled" +
                                 " pixel_format=" +
                                 (fmt == AV_PIX_FMT_D3D11 ? "d3d11/nv12"
                                  : fmt == AV_PIX_FMT_BGR0 ? "bgr0/RGB/full-range/4:4:4"
                                  : fmt == AV_PIX_FMT_NV12 ? "nv12/4:2:0"
                                                           : "yuv420p/4:2:0"));
                Log::get().write("Encoder: " + name +
                                 (gpu_ ? " GPU texture path" : name.find("lib") == 0 ? " software encoding" : " hardware encoding with frame upload") +
                                 " adapter=" + std::to_string(attempt));
                return;
            } catch (const std::exception& e) {
                failures += name + ": " + e.what() + "; ";
                Log::get().write("Encoder initialization failed: " + name + " adapter=" +
                                 std::to_string(attempt) + " " + e.what());
                av_frame_free(&frame_);
                avcodec_free_context(&codec_);
                av_buffer_unref(&frames_);
                av_buffer_unref(&device_);
                sws_freeContext(sws_);
                sws_ = nullptr;
                uploadNv12_.Reset();
                for (auto& stage : uploadStages_) stage.Reset();
                nv12Readback_ = false;
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
    auto output = receive(codec_, 0);
    int sent = avcodec_send_frame(codec_, f);
    if (sent == AVERROR(EAGAIN)) {
        auto pending = receive(codec_, 0);
        output.insert(output.end(), pending.begin(), pending.end());
        sent = avcodec_send_frame(codec_, f);
    }
    avcheck(sent, "Submit video");
    auto pending = receive(codec_, 0);
    output.insert(output.end(), pending.begin(), pending.end());
    if (qualityProbe_) {
        try {
            for (const auto& packet : output) {
                if (packet.p->pts != qualityProbe_->pts) continue;
                qualityProbe_->save(packet, track(), capture_);
                qualityProbe_.reset();
                break;
            }
        } catch (const std::exception& error) {
            failQualityComparison(error.what());
            qualityProbe_.reset();
        }
    }
    return output;
}
void VideoEncoder::prepareCpu(const uint8_t* b, int stride) {
    avcheck(av_frame_make_writable(frame_), "Writable video frame");
    const uint8_t* src[] = {b};
    int lines[] = {stride};
    if (frame_->format == AV_PIX_FMT_BGR0) {
        if (viewport_.w != output_.w || viewport_.h != output_.h) {
            for (int y = 0; y < output_.h; ++y)
                memset(frame_->data[0] + size_t(y) * frame_->linesize[0], 0, size_t(output_.w) * 4);
        }
        auto destination = frame_->data[0] + size_t(viewport_.y) * frame_->linesize[0] + viewport_.x * 4;
        if (input_.w == viewport_.w && input_.h == viewport_.h) {
            for (int y = 0; y < input_.h; ++y)
                memcpy(destination + size_t(y) * frame_->linesize[0], b + size_t(y) * stride,
                       size_t(input_.w) * 4);
        } else {
            uint8_t* dest[4] = {destination};
            sws_scale(sws_, src, lines, 0, input_.h, dest, frame_->linesize);
        }
        return;
    }
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
void VideoEncoder::convert(ID3D11Texture2D* t, ID3D11Texture2D* out, UINT slice) {
    auto input = std::find_if(inputViews_.begin(), inputViews_.end(),
                              [&](const auto& v) { return v.texture == t; });
    if (input == inputViews_.end()) {
        input = inputViews_.begin() + nextInputView_;
        nextInputView_ = (nextInputView_ + 1) % inputViews_.size();
        *input = {};
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        check(vd_->CreateVideoProcessorInputView(t, enumerator_.Get(), &desc, &input->view), "GPU input view");
        input->texture = t;
    }
    auto output = std::find_if(outputViews_.begin(), outputViews_.end(),
                               [&](const auto& v) { return v.texture == out && v.slice == slice; });
    if (output == outputViews_.end()) {
        output = outputViews_.begin() + nextOutputView_;
        nextOutputView_ = (nextOutputView_ + 1) % outputViews_.size();
        *output = {};
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
        D3D11_TEXTURE2D_DESC texture{};
        out->GetDesc(&texture);
        desc.ViewDimension = texture.ArraySize > 1 ? D3D11_VPOV_DIMENSION_TEXTURE2DARRAY
                                                  : D3D11_VPOV_DIMENSION_TEXTURE2D;
        desc.Texture2DArray.FirstArraySlice = slice;
        desc.Texture2DArray.ArraySize = 1;
        check(vd_->CreateVideoProcessorOutputView(out, enumerator_.Get(), &desc, &output->view), "GPU output view");
        output->texture = out;
        output->slice = slice;
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input->view.Get();
    check(vc_->VideoProcessorBlt(processor_.Get(), output->view.Get(), 0, 1, &stream),
          "GPU scale and NV12 conversion");
}
bool VideoEncoder::drainReadback(std::vector<Packet>& output) {
    int index = -1;
    for (int i = 0; i < int(readbacks_.size()); ++i)
        if (readbacks_[size_t(i)].pending &&
            (index < 0 || readbacks_[size_t(i)].pts < readbacks_[size_t(index)].pts))
            index = i;
    if (index < 0) return false;
    auto& job = readbacks_[size_t(index)];
    auto stage = uploadStages_[size_t(index)].Get();
    D3D11_MAPPED_SUBRESOURCE map{};
    const auto begin = clockUs();
    const HRESULT mapped = capture_->context()->Map(stage, 0, D3D11_MAP_READ,
        D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
    if (mapped == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    check(mapped, "Asynchronous NV12 readback");
    try {
        avcheck(av_frame_make_writable(frame_), "Writable NV12 frame");
        auto bytes = static_cast<const uint8_t*>(map.pData);
        for (int y = 0; y < output_.h; ++y)
            memcpy(frame_->data[0] + size_t(y) * frame_->linesize[0],
                   bytes + size_t(y) * map.RowPitch, output_.w);
        bytes += size_t(output_.h) * map.RowPitch;
        for (int y = 0; y < output_.h / 2; ++y)
            memcpy(frame_->data[1] + size_t(y) * frame_->linesize[1],
                   bytes + size_t(y) * map.RowPitch, output_.w);
    } catch (...) {
        capture_->context()->Unmap(stage, 0);
        throw;
    }
    capture_->context()->Unmap(stage, 0);
    frame_->pts = job.pts;
    frame_->duration = 1;
    frame_->pict_type = job.key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    frame_->flags &= ~AV_FRAME_FLAG_KEY;
    if (job.key)
        frame_->flags |= AV_FRAME_FLAG_KEY;
    job.pending = false;
    const auto submit = clockUs();
    auto packets = send(frame_);
    ++submittedFrames;
    prepareMs = prepareMs * .9 + double(submit - begin) / 1000 * .1;
    submitMs = submitMs * .9 + double(clockUs() - submit) / 1000 * .1;
    output.insert(output.end(), std::make_move_iterator(packets.begin()),
                  std::make_move_iterator(packets.end()));
    return true;
}
std::vector<Packet> VideoEncoder::encode(ID3D11Texture2D* t, int64_t pts, bool key) {
    const auto begin = clockUs();
    if (qualityProbe_ && begin > qualityProbe_->deadline) {
        failQualityComparison("No matching encoded frame arrived within five seconds");
        qualityProbe_.reset();
    }
    if (takeQualityComparison(begin)) {
        try {
            if (!gpu_) throw std::runtime_error("Comparison requires the GPU texture encoding path");
            qualityProbe_ = std::make_unique<QualityProbe>(capture_, t, output_, pts);
        } catch (const std::exception& error) {
            failQualityComparison(error.what());
        }
    }
    if (qualityProbe_ && qualityProbe_->pts == pts) key = true;
    if (nv12Readback_) {
        pendingKey_ = pendingKey_ || key;
        std::vector<Packet> output;
        while (drainReadback(output)) {}
        int free = -1;
        for (int i = 0; i < int(readbacks_.size()); ++i)
            if (!readbacks_[size_t(i)].pending) { free = i; break; }
        if (free >= 0) {
            convert(t, uploadNv12_.Get(), 0);
            capture_->context()->CopyResource(uploadStages_[size_t(free)].Get(), uploadNv12_.Get());
            // DO_NOT_WAIT maps never submit a queued copy on our behalf.
            capture_->context()->Flush();
            readbacks_[size_t(free)] = {true, pendingKey_, pts};
            pendingKey_ = false;
        } else {
            ++readbackDrops;
        }
        while (drainReadback(output)) {}
        auto ready = receive(codec_, 0);
        output.insert(output.end(), std::make_move_iterator(ready.begin()),
                      std::make_move_iterator(ready.end()));
        return output;
    }
    if (gpu_) {
        av_frame_unref(frame_);
        avcheck(av_hwframe_get_buffer(frames_, frame_, 0), "GPU encoder frame");
        auto out = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
        UINT slice = UINT(reinterpret_cast<uintptr_t>(frame_->data[1]));
        convert(t, out, slice);
        if (qualityProbe_ && qualityProbe_->pts == pts) {
            try {
                qualityProbe_->copyConverted(capture_, out, slice);
            } catch (const std::exception& error) {
                failQualityComparison(error.what());
                qualityProbe_.reset();
            }
        }
        // Submit the conversion before another API/device consumes the texture.
        capture_->context()->Flush();
    } else {
        auto b = capture_->download(t);
        prepareCpu(b.data(), input_.w * 4);
    }
    frame_->pts = pts;
    frame_->duration = 1;
    frame_->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    frame_->flags &= ~AV_FRAME_FLAG_KEY;
    if (key)
        frame_->flags |= AV_FRAME_FLAG_KEY;
    const auto submit = clockUs();
    auto packets = send(frame_);
    ++submittedFrames;
    prepareMs = prepareMs * .9 + double(submit - begin) / 1000 * .1;
    submitMs = submitMs * .9 + double(clockUs() - submit) / 1000 * .1;
    return packets;
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
    frame_->flags &= ~AV_FRAME_FLAG_KEY;
    if (key)
        frame_->flags |= AV_FRAME_FLAG_KEY;
    auto output = send(frame_);
    ++submittedFrames;
    return output;
}
std::vector<Packet> VideoEncoder::flush() {
    std::vector<Packet> output;
    if (nv12Readback_) {
        capture_->context()->Flush();
        const auto deadline = clockUs() + 2000000;
        while (pendingReadbacks()) {
            if (drainReadback(output)) continue;
            if (clockUs() >= deadline) {
                Log::get().write("GPU readback drain timed out; retaining encoded video and dropping " +
                                 std::to_string(pendingReadbacks()) + " pending frames");
                for (auto& job : readbacks_) {
                    if (job.pending) ++readbackDrops;
                    job.pending = false;
                }
                break;
            }
            Sleep(1);
        }
    }
    // A start cancelled before its first frame has no delayed video to drain.
    // Some hardware drivers do not complete an empty encoder drain.
    if (!submittedFrames) return output;
    auto tail = send(nullptr);
    output.insert(output.end(), std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()));
    return output;
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
        if (avformat_query_codec(ctx_->oformat, t.par->codec_id, FF_COMPLIANCE_NORMAL) == 0)
            throw std::runtime_error("Selected container does not support the encoded track");
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
                           const std::vector<AVCodecID>* codecs = nullptr, bool fullScan = true) {
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
        const bool firstVideo = packet->stream_index == 0 && !fullScan;
        if (firstVideo && !(packet->flags & AV_PKT_FLAG_KEY)) good = false;
        av_packet_unref(packet);
        if (firstVideo) break;
    }
    // Audio is optional at runtime: an endpoint can be unavailable or the user
    // can record a very short clip before an audio encoder emits its first
    // packet. The video track is required for a useful recording; requiring
    // packets in every declared audio track incorrectly rejects such files.
    good = good && (ret == AVERROR_EOF || (!fullScan && ret >= 0)) &&
           !counts.empty() && counts.front() > 0;
    if (expected)
        good = good && counts == *expected;
    av_packet_free(&packet);
    avformat_close_input(&ctx);
    return good;
}
bool verifyFile(const std::filesystem::path& p, bool fullScan) {
    return verifyContents(p, nullptr, nullptr, fullScan);
}
void remuxFile(const std::filesystem::path& source, const std::filesystem::path& target) {
    if (std::filesystem::exists(target))
        throw std::runtime_error("Remux destination already exists; choose a new filename");
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
    if (exports_.size() >= 8 || exportBytes_ + amount > 256u * 1024u * 1024u)
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
void DiskWriter::remux(std::filesystem::path source, std::filesystem::path target) {
    Job job{Job::Remux};
    job.source = std::move(source);
    job.path = std::move(target);
    job.keepSource = true;
    pushExport(std::move(job));
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
        if (remux)
            Log::get().write("Auto-remux queued after recording close; target=" +
                             utf8(final.filename().wstring()) + " recovery=" +
                             utf8(temporary.filename().wstring()));
        else
            Log::get().write("Recording container finalized: " +
                             utf8(final.filename().wstring()));
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
                if (!verifyFile(job.path, false))
                    throw std::runtime_error("Replay verification failed");
                notify_("Replay saved", false);
            } else if (job.type == Job::Remux) {
                if (!job.keepSource)
                    Log::get().write("Auto-remux started: " + utf8(job.source.filename().wstring()) +
                                     " -> " + utf8(job.path.filename().wstring()));
                remuxFile(job.source, job.path);
                if (!job.keepSource)
                    std::filesystem::remove(job.source);
                else
                    notify_("Remux complete; original retained", false);
                if (!job.keepSource) {
                    Log::get().write("Auto-remux complete and verified; recovery file removed");
                    notify_("Remux complete", false);
                }
            } else if (job.type == Job::Verify && !verifyFile(job.path, false)) {
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
