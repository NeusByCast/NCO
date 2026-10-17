#include "media.hpp"
#include "video_pipeline.hpp"
#include <cmath>
#include <fstream>
#include <limits>

namespace nco {
void platformSelfTest(std::ostream& report);
namespace {
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}

struct StreamResult {
    size_t packets = 0, decodedFrames = 0;
    uint64_t bytes = 0;
    int64_t first = INT64_MAX, end = INT64_MIN;
    AVCodecID codec = AV_CODEC_ID_NONE;
    int width = 0, height = 0;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace space = AVCOL_SPC_UNSPECIFIED;
};

std::vector<StreamResult> inspect(const std::filesystem::path& path, bool decodeVideo = false) {
    AVFormatContext* in = nullptr;
    avcheck(avformat_open_input(&in, utf8(path.wstring()).c_str(), nullptr, nullptr), "Test open");
    AVPacket* packet = av_packet_alloc();
    try {
        avcheck(avformat_find_stream_info(in, nullptr), "Test stream info");
        std::vector<StreamResult> result(in->nb_streams);
        for (unsigned i = 0; i < in->nb_streams; ++i) {
            const auto p = in->streams[i]->codecpar;
            result[i].codec = p->codec_id;
            result[i].width = p->width;
            result[i].height = p->height;
            result[i].range = p->color_range;
            result[i].space = p->color_space;
        }
        auto releaseDecoder = [](AVCodecContext* c) { avcodec_free_context(&c); };
        auto releaseFrame = [](AVFrame* f) { av_frame_free(&f); };
        std::unique_ptr<AVCodecContext, decltype(releaseDecoder)> decoder(nullptr, releaseDecoder);
        std::unique_ptr<AVFrame, decltype(releaseFrame)> decoded(nullptr, releaseFrame);
        const int videoStream = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (decodeVideo && videoStream >= 0) {
            const auto parameters = in->streams[videoStream]->codecpar;
            const auto codec = avcodec_find_decoder(parameters->codec_id);
            require(codec != nullptr, "Verification decoder is unavailable");
            decoder.reset(avcodec_alloc_context3(codec));
            decoded.reset(av_frame_alloc());
            require(decoder && decoded, "Cannot allocate verification decoder");
            avcheck(avcodec_parameters_to_context(decoder.get(), parameters), "Decode parameters");
            decoder->thread_count = 1;
            decoder->err_recognition = AV_EF_EXPLODE;
            avcheck(avcodec_open2(decoder.get(), codec, nullptr), "Open verification decoder");
        }
        auto receiveFrames = [&] {
            while (decoder) {
                const int received = avcodec_receive_frame(decoder.get(), decoded.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
                avcheck(received, "Decode saved recording frame");
                auto& stream = result[size_t(videoStream)];
                require(!(decoded->flags & AV_FRAME_FLAG_CORRUPT) &&
                        decoded->width == stream.width && decoded->height == stream.height,
                        "Saved recording has a corrupt frame or incorrect decoded dimensions");
                ++stream.decodedFrames;
                av_frame_unref(decoded.get());
            }
        };
        int status;
        while ((status = av_read_frame(in, packet)) >= 0) {
            auto& s = result.at(size_t(packet->stream_index));
            auto tb = in->streams[packet->stream_index]->time_base;
            require(packet->pts != AV_NOPTS_VALUE, "Output packet has no presentation timestamp");
            ++s.packets;
            s.bytes += size_t(packet->size);
            s.first = std::min(s.first, av_rescale_q(packet->pts, tb, {1, 1000000}));
            s.end = std::max(s.end, av_rescale_q(packet->pts + packet->duration, tb, {1, 1000000}));
            if (decoder && packet->stream_index == videoStream) {
                avcheck(avcodec_send_packet(decoder.get(), packet), "Submit saved packet to decoder");
                receiveFrames();
            }
            av_packet_unref(packet);
        }
        require(status == AVERROR_EOF, "Output did not end cleanly");
        if (decoder) {
            avcheck(avcodec_send_packet(decoder.get(), nullptr), "Drain verification decoder");
            receiveFrames();
            require(result[size_t(videoStream)].decodedFrames == result[size_t(videoStream)].packets,
                    "Saved recording packet/frame counts do not match");
        }
        for (auto& s : result)
            require(s.packets > 0 && s.end > s.first, "Empty output stream");
        av_packet_free(&packet);
        avformat_close_input(&in);
        return result;
    } catch (...) {
        av_packet_free(&packet);
        avformat_close_input(&in);
        throw;
    }
}

std::vector<float> tone(int64_t sample, int count) {
    std::vector<float> result(size_t(count) * 2);
    for (int i = 0; i < count; ++i) {
        result[size_t(i) * 2] = result[size_t(i) * 2 + 1] =
            float(.1 * std::sin((sample + i) * 6.28318530718 * 440 / 48000));
    }
    return result;
}

void containerTest(const std::filesystem::path& dir, const std::string& format) {
    Config config;
    config.bitrateKbps = 0;
    config.fps = 30;
    config.resolutionMode = 5;
    config.width = 320;
    config.height = 180;
    config.encoder = format == "webm" ? "libvpx-vp9" : "libx264";
    config.format = format;
    VideoEncoder video;
    video.open(config, nullptr, {320, 180}, true);
    AudioEncoder audio;
    audio.open(format == "webm", 1);
    std::vector<Track> tracks = {video.track(), audio.track()};
    auto path = dir / ("synthetic." + format);
    Muxer mux;
    mux.open(path, tracks);
    std::vector<Packet> packets;
    ReplayRing<Packet> ring;
    bool sawFirstVideoPacket = false;
    bool firstVideoPacketWasKey = false;
    auto accept = [&](std::vector<Packet> input) {
        for (auto& packet : input) {
            if (packet.stream == 0 && !sawFirstVideoPacket) {
                sawFirstVideoPacket = true;
                firstVideoPacketWasKey = packet.key();
            }
            mux.write(packet);
            ring.push(packet);
            packets.push_back(packet);
        }
    };
    int64_t sample = 0;
    for (int frame = 0; frame < 90; ++frame) {
        accept(video.synthetic(frame, frame % 30 == 0));
        while (sample < (frame + 1) * 48000 / 30) {
            accept(audio.encode(tone(sample, audio.frameSize()), sample));
            sample += audio.frameSize();
        }
    }
    accept(video.flush());
    accept(audio.flush());
    mux.close();
    require(verifyFile(path, false), "Completed recording has no readable first keyframe");
    require(sawFirstVideoPacket && firstVideoPacketWasKey,
            "Encoder did not mark its forced first frame as a keyframe");
    auto streams = inspect(path, true);
    require(streams[0].width == 320 && streams[0].height == 180 &&
            streams[0].range == AVCOL_RANGE_MPEG && streams[0].space == AVCOL_SPC_BT709,
            "Encoded resolution or BT.709 limited-range metadata changed");
    require(streams.size() == 2 && streams[0].packets == 90,
            "Video/audio stream count or video frame count mismatch");
    require(std::abs(streams[0].first - streams[1].first) < 100000,
            "Synthetic A/V start timestamp mismatch");
    require(std::abs(streams[0].end - streams[1].end) < 100000,
            "Synthetic A/V end timestamp mismatch");
    require(streams[0].end - streams[0].first > 2800000, "Synthetic output duration too short");

    Muxer replay;
    auto replayPath = dir / ("replay." + format);
    replay.open(replayPath, tracks);
    for (auto& p : ring.snapshot())
        replay.write(p);
    replay.close();
    require(verifyFile(replayPath), "Replay could not be read");

    Muxer pause;
    auto pausePath = dir / ("pause." + format);
    pause.open(pausePath, tracks);
    for (auto& p : packets) {
        if (p.us() < 1000000)
            pause.write(p);
        else if (p.us() >= 2000000)
            pause.write(p, 1000000);
    }
    pause.close();
    auto paused = inspect(pausePath);
    require(paused[0].packets == 60, "Pause did not remove exactly one second of video");
    require(std::abs(paused[0].end - paused[1].end) < 100000, "Pause A/V timestamp mismatch");

    if (format == "mkv") {
        auto target = dir / "remux.mp4";
        std::filesystem::remove(target);
        remuxFile(path, target);
        auto remuxed = inspect(target);
        require(remuxed.size() == streams.size(), "Remux lost a stream");
        for (size_t i = 0; i < streams.size(); ++i) {
            require(remuxed[i].codec == streams[i].codec, "Remux changed codec");
            require(remuxed[i].packets == streams[i].packets, "Remux lost encoded packets");
        }
        std::mutex eventsMutex;
        std::vector<std::string> errors;
        int saved = 0, remuxedFiles = 0;
        std::filesystem::remove(dir / "manual-remux.mp4");
        std::filesystem::remove(dir / "threaded-recording.mp4");
        {
            DiskWriter writer([&](std::string event, bool error) {
                std::lock_guard lock(eventsMutex);
                if (error)
                    errors.push_back(event);
                if (event == "Replay saved")
                    ++saved;
                if (event == "Remux complete; original retained")
                    ++remuxedFiles;
            });
            writer.open(dir / "threaded-recording.mp4", tracks, true, "mp4");
            for (size_t i = 0; i < packets.size(); ++i) {
                writer.write(packets[i], 0);
                if (i == 30 || i == 60 || i == 90) {
                    writer.replay(dir / ("threaded-replay-" + std::to_string(i) + ".mkv"), tracks,
                                  ring.snapshot());
                }
            }
            writer.close();
            writer.remux(path, dir / "manual-remux.mp4");
        }
        require(remuxedFiles == 1 && std::filesystem::exists(path),
                "Manual remux must complete asynchronously and retain its source");
        auto manual = inspect(dir / "manual-remux.mp4");
        require(manual[0].packets == streams[0].packets, "Manual remux lost video packets");
        bool overwriteRejected = false;
        try { remuxFile(path, dir / "manual-remux.mp4"); }
        catch (const std::exception&) { overwriteRejected = true; }
        require(overwriteRejected && verifyFile(path), "Remux overwrite must be rejected safely");
        require(errors.empty(), errors.empty() ? "" : errors.front());
        require(saved == 3, "Concurrent replay exports did not finish");
        {
            const auto folder = dir / L"Selected folder \u03a9 with spaces";
            const auto recording = uniquePath(folder, "NCO_", "mkv");
            const auto replay = uniquePath(folder, "Replay_", "mkv");
            const auto another = uniquePath(folder, "NCO_", "mkv");
            require(recording != another, "Output folder generated duplicate names");
            std::vector<std::string> folderErrors;
            {
                DiskWriter writer([&](std::string event, bool error) {
                    if (error) { std::lock_guard lock(eventsMutex); folderErrors.push_back(event); }
                });
                writer.open(recording, tracks, false, "mkv");
                for (auto& packet : packets) writer.write(packet, 0);
                writer.close();
                writer.replay(replay, tracks, ring.snapshot());
            }
            require(folderErrors.empty() && inspect(recording, true)[0].decodedFrames == 90 &&
                    inspect(replay, true)[0].decodedFrames > 0,
                    "Recording or replay failed in the selected Unicode output folder");
            const auto blocked = folder / "regular-file";
            { std::ofstream sentinel(blocked); sentinel << "preserve"; }
            bool rejected = false;
            try { uniquePath(blocked, "NCO_", "mkv"); }
            catch (const std::filesystem::filesystem_error&) { rejected = true; }
            std::ifstream sentinel(blocked);
            std::string unchanged;
            sentinel >> unchanged;
            require(rejected && unchanged == "preserve", "Invalid output folder was accepted or overwritten");
        }
        require(inspect(dir / "threaded-recording.mp4")[0].packets == 90,
                "Concurrent export interrupted the recording writer");
        require(!std::filesystem::exists(dir / "threaded-recording.mp4.recovery.mkv"),
                "Successful remux did not clean up its recovery file");
    }
}

void separateTrackRemux(const std::filesystem::path& dir) {
    Config config;
    config.bitrateKbps = 0;
    config.fps = 30;
    config.encoder = "libx264";
    VideoEncoder video;
    video.open(config, nullptr, {320, 180}, true);
    AudioEncoder mic, desktop;
    mic.open(false, 1);
    desktop.open(false, 2);
    auto tracks = std::vector<Track>{video.track(), mic.track(), desktop.track()};
    tracks[1].name = "Microphone";
    tracks[2].name = "Desktop audio";
    auto source = dir / "separate-tracks.mkv";
    auto target = dir / "separate-tracks.mp4";
    Muxer mux;
    mux.open(source, tracks);
    auto write = [&](std::vector<Packet> packets) {
        for (auto& p : packets)
            mux.write(p);
    };
    int64_t sample = 0;
    for (int frame = 0; frame < 60; ++frame) {
        write(video.synthetic(frame, frame == 0));
        while (sample < (frame + 1) * 48000 / 30) {
            auto samples = tone(sample, mic.frameSize());
            write(mic.encode(samples, sample));
            for (auto& value : samples)
                value *= .5f;
            write(desktop.encode(samples, sample));
            sample += mic.frameSize();
        }
    }
    write(video.flush());
    write(mic.flush());
    write(desktop.flush());
    mux.close();
    std::filesystem::remove(target);
    remuxFile(source, target);
    auto before = inspect(source), after = inspect(target);
    require(before.size() == 3 && after.size() == 3, "Remux lost an independent audio track");
    for (size_t i = 0; i < before.size(); ++i) {
        require(before[i].codec == after[i].codec && before[i].packets == after[i].packets,
                "Independent track remux changed codec or packet count");
        require(std::abs(after[0].end - after[i].end) < 100000,
                "Independent track remux lost A/V synchronization");
    }
}

void videoMatrix(const std::filesystem::path& dir) {
    std::ofstream matrix(dir / "recording-matrix.csv");
    matrix << "encoder,fps,quality,rate_control,bitrate_kbps,width,height,decoded_frames,qp_avg,qp_max,video_mbps\n";
    size_t cases = 0;
    for (const char* name : {"libx264", "libvpx-vp9"}) {
      for (int fps : {15, 30, 60}) {
        for (int quality = 0; quality < 5; ++quality) {
            for (int rateControl = 0; rateControl < 3; ++rateControl) {
              double firstCqp = -1;
              for (int bitrateKbps : {0, 8000}) {
                Config config;
                config.bitrateKbps = bitrateKbps;
                config.rateControl = rateControl;
                config.fps = fps;
                config.quality = quality;
                config.encoder = name;
                config.format = "mkv";
                // Exercise native size and custom scaling, including even-dimension rounding.
                config.resolutionMode = quality % 2 ? 5 : 0;
                config.width = 641;
                config.height = 361;
                VideoEncoder encoder;
                encoder.open(config, nullptr, {320, 180}, true);
                Muxer mux;
                auto path = dir / ("matrix-" + config.encoder + "-" + std::to_string(fps) + "-" +
                                   std::to_string(quality) + "-" +
                                   std::to_string(rateControl) + "-" + std::to_string(bitrateKbps) + ".mkv");
                auto check = [&](bool ok, const char* message) {
                    require(ok, path.filename().string() + ": " + message);
                };
                mux.open(path, {encoder.track()});
                double qpSum = 0, qpMax = 0;
                size_t qpCount = 0;
                const bool vp9 = config.encoder == "libvpx-vp9";
                constexpr int h264Qp[] = {36, 32, 27, 23, 19};
                constexpr int vp9Qp[] = {44, 38, 32, 26, 20};
                const int selectedQp = vp9 ? vp9Qp[quality] : h264Qp[quality];
                auto accept = [&](std::vector<Packet> packets) {
                    for (auto& packet : packets) {
                        const double actual = packet.quantizer();
                        check(actual >= 0, "Encoder did not expose its actual quantizer");
                        qpSum += actual;
                        qpMax = std::max(qpMax, actual);
                        ++qpCount;
                        if (rateControl == 1 && (vp9 || !packet.key()))
                            check(std::abs(actual - selectedQp) <= 1.1, "CQP did not keep the selected quantizer");
                        if (rateControl == 0)
                            check(actual <= selectedQp + 4, "VBR exceeded the preset's quantizer limit");
                        mux.write(packet);
                    }
                };
                for (int frame = 0; frame < fps; ++frame)
                    accept(encoder.synthetic(frame, frame == 0));
                accept(encoder.flush());
                mux.close();
                auto result = inspect(path, true);
                const auto expected = resolution(config.resolutionMode, config.width, config.height, {320, 180});
                check(result[0].width == expected.w && result[0].height == expected.h,
                      "Preset or rate control changed the recording resolution");
                check(result[0].decodedFrames == size_t(fps) && qpCount == size_t(fps),
                      "FPS/quality matrix lost a video frame");
                check(result[0].end - result[0].first > 850000, "FPS matrix duration incorrect");
                const double average = qpSum / qpCount;
                if (rateControl == 1) {
                    if (firstCqp >= 0) check(std::abs(average - firstCqp) < .2, "Bitrate changed CQP quality");
                    firstCqp = average;
                }
                matrix << name << ',' << fps << ',' << quality << ',' << rateControl << ','
                       << bitrateKbps << ',' << expected.w << ',' << expected.h << ','
                       << result[0].decodedFrames << ',' << average << ',' << qpMax << ','
                       << double(result[0].bytes) * 8 / (result[0].end - result[0].first) << '\n';
                matrix.flush();
                ++cases;
              }
            }
        }
      }
    }
    require(cases == 180, "Recording matrix did not exercise every case");
}

void resolutionOutputs(const std::filesystem::path& dir) {
    for (const char* name : {"libx264", "libvpx-vp9"}) {
        for (int mode = 0; mode <= 5; ++mode) {
            Config config;
            config.encoder = name;
            config.format = "mkv";
            config.rateControl = 1;
            config.resolutionMode = mode;
            config.width = 643;
            config.height = 363;
            VideoEncoder video;
            video.open(config, nullptr, {128, 72}, true);
            auto path = dir / ("resolution-" + config.encoder + "-" + std::to_string(mode) + ".mkv");
            Muxer mux;
            mux.open(path, {video.track()});
            for (auto& packet : video.synthetic(0, true)) mux.write(packet);
            for (auto& packet : video.flush()) mux.write(packet);
            mux.close();
            const auto actual = inspect(path, true);
            const auto expected = resolution(mode, config.width, config.height, {128, 72});
            require(actual[0].width == expected.w && actual[0].height == expected.h &&
                    actual[0].decodedFrames == 1, "Resolution preset produced the wrong decoded frame");
        }
    }
}

void availableHardwareOutputs(const std::filesystem::path& dir, std::ostream& report) {
    size_t devices = 0, encoded = 0;
    for (const auto& choice : probeEncoders()) {
        if (!choice.gpu) continue;
        ++devices;
        for (int quality = 0; quality < 5; ++quality) {
            for (int rate = 0; rate < 3; ++rate) {
                Config config;
                config.encoder = choice.name;
                config.format = "mkv";
                config.fps = 30;
                config.quality = quality;
                config.rateControl = rate;
                config.bitrateKbps = 8000;
                VideoEncoder video;
                try { video.open(config, nullptr, {320, 180}, true); }
                catch (const std::exception& error) {
                    report << "UNAVAILABLE " << choice.name << " quality=" << quality
                           << " rate=" << rate << ": " << error.what() << '\n';
                    continue;
                }
                const auto path = dir / (choice.name + "-" + std::to_string(quality) + "-" + std::to_string(rate) + ".mkv");
                Muxer mux;
                mux.open(path, {video.track()});
                for (int i = 0; i < 6; ++i)
                    for (auto& packet : video.synthetic(i, i == 0)) mux.write(packet);
                for (auto& packet : video.flush()) mux.write(packet);
                mux.close();
                require(inspect(path, true)[0].decodedFrames == 6,
                        choice.name + " lost or corrupted a hardware-encoded frame");
                ++encoded;
            }
        }
    }
    av_log_set_level(AV_LOG_ERROR);
    if (!devices) report << "SKIP physical GPU encode/decode: no device encoder available on this runner\n";
    else if (!encoded) report << "SKIP hardware encode/decode: probed encoders could not open the requested cases\n";
    else report << "PASS available hardware encode/decode: " << encoded << " supported cases across " << devices << " encoders\n";
}

void replayDurations(const std::filesystem::path& dir) {
    Config config;
    config.bitrateKbps = 0;
    config.fps = 15;
    config.encoder = "libx264";
    config.format = "mkv";
    VideoEncoder video;
    video.open(config, nullptr, {64, 64}, true);
    AudioEncoder audio;
    audio.open(false, 1);
    std::vector<Track> tracks = {video.track(), audio.track()};
    ReplayRing<Packet> rings[3];
    rings[0].seconds = 15;
    rings[1].seconds = 30;
    rings[2].seconds = 60;
    auto accept = [&](std::vector<Packet> input) {
        for (auto& p : input)
            for (auto& r : rings)
                r.push(p);
    };
    int64_t sample = 0;
    // Accelerated synthetic time; this is not a 67-second wall-clock stability test.
    for (int frame = 0; frame < 67 * 15; ++frame) {
        accept(video.synthetic(frame, frame % 30 == 0));
        while (sample < (frame + 1) * 48000 / 15) {
            accept(audio.encode(tone(sample, audio.frameSize()), sample));
            sample += audio.frameSize();
        }
    }
    accept(video.flush());
    accept(audio.flush());
    for (auto& ring : rings) {
        auto packets = ring.snapshot();
        require(!packets.empty() && packets.front().key(),
                "Replay does not begin at a video keyframe");
        auto span = packets.back().us() - packets.front().us();
        require(span >= (ring.seconds - 1) * 1000000LL && span <= (ring.seconds + 3) * 1000000LL,
                "Replay duration outside its GOP tolerance");
        auto path = dir / ("replay-" + std::to_string(ring.seconds) + ".mkv");
        Muxer mux;
        mux.open(path, tracks);
        for (auto& p : packets)
            mux.write(p);
        mux.close();
        auto result = inspect(path, true);
        require(result.size() == 2, "Replay lost audio");
        require(std::abs(result[0].end - result[1].end) < 100000, "Replay A/V timestamp mismatch");
    }
}
} // namespace

int mediaSelfTest() {
    auto dir = std::filesystem::path(L"nco-self-test");
    std::filesystem::create_directories(dir);
    std::ofstream report(dir / L"results.txt");
    try {
        av_log_set_level(AV_LOG_ERROR);
        encoderConfigurationSelfTest();
        report << "PASS AMF H264/HEVC/AV1: all quality/rate modes, bounded overlap, no analysis/preencode, balanced workload, quantizer limits and packet statistics\n";
        report << "PASS compiled NVENC/QSV/Media Foundation settings and actual libx264/VP9 runtime rate-control modes with automatic/manual bitrates\n";
        platformSelfTest(report);
        {
            Config config;
            config.encoder = "libx264";
            VideoPipeline unused(config, nullptr, {320, 180}, true);
            unused.finish();
            const auto deadline = clockUs() + 2000000;
            while (!unused.finished() && clockUs() < deadline) Sleep(1);
            require(unused.finished() && unused.collect().empty() && unused.metrics().frames == 0,
                    "Unused encoder startup session did not shut down cleanly");
            report << "PASS unused encoder startup session teardown before capture-route retry\n";
        }
        {
            Config config;
            config.bitrateKbps = 0;
            config.encoder = "libx264";
            config.fps = 30;
            VideoPipeline pipeline(config, nullptr, {320, 180}, true, 250);
            const auto track = pipeline.track();
            const auto begin = clockUs();
            require(pipeline.submit(nullptr, 0, true), "First encoder mailbox rejected");
            require(pipeline.submit(nullptr, 1, false), "Second encoder mailbox rejected");
            require(!pipeline.submit(nullptr, 2, false), "Encoder mailbox limit was not enforced");
            require(clockUs() - begin < 100000, "Capture waited on a slow encoder");
            const auto finishBegin = clockUs();
            pipeline.finish();
            require(clockUs() - finishBegin < 100000,
                    "Stop request waited on the encoder worker");
            require(!pipeline.submit(nullptr, 2, false),
                    "Encoder accepted a frame after the drain request");
            std::vector<Packet> encoded;
            const auto deadline = clockUs() + 3000000;
            while (!pipeline.finished() && clockUs() < deadline) {
                auto ready = pipeline.collect();
                encoded.insert(encoded.end(), ready.begin(), ready.end());
                Sleep(5);
            }
            require(pipeline.finished(), "Encoder did not finish within its drain deadline");
            auto tail = pipeline.flush();
            encoded.insert(encoded.end(), tail.begin(), tail.end());
            require(encoded.size() == 2 && pipeline.metrics().frames == 2,
                    "Worker finalization lost accepted frames");
            require(encoded[0].p->pts < encoded[1].p->pts,
                    "Worker changed video timestamp order");
            require(track.par->codec_type == AVMEDIA_TYPE_VIDEO, "Worker metadata missing");
        }
        {
            Config config;
            config.encoder = "libx264";
            VideoPipeline pipeline(config, nullptr, {320, 180}, true, 0, true);
            require(pipeline.submit(nullptr, 0, true), "Recovery test frame rejected");
            pipeline.finish();
            const auto deadline = clockUs() + 3000000;
            while (!pipeline.finished() && clockUs() < deadline) Sleep(5);
            require(pipeline.finished(), "Failed drain did not signal completion");
            bool failed = false;
            try { pipeline.collect(); }
            catch (const std::runtime_error& error) {
                failed = std::string(error.what()) == "Injected encoder drain failure";
            }
            require(failed, "Encoder drain failure was hidden");
            auto recovered = pipeline.collect(true);
            require(recovered.size() == 1 && recovered.front().key(),
                    "Drain failure lost the completed recording packet");
        }
        report << "PASS bounded encoder worker: asynchronous stop, ordered accepted frames, drain failure reporting and completed-packet recovery\n";
        for (const auto& format : {"mp4", "mkv", "mov", "webm"}) {
            containerTest(dir, format);
            report << "PASS " << format
                   << ": synthetic A/V, timestamps, packet replay, pause timeline\n";
            report.flush();
        }
        videoMatrix(dir);
        report << "PASS 180 decoded H264/VP9 recordings: 15/30/60 FPS, five presets, three rate modes, automatic/manual bitrate, actual quantizers and native/custom scaling\n";
        resolutionOutputs(dir);
        report << "PASS decoded Native/720p/1080p/1440p/4K/Custom outputs from both software encoders\n";
        availableHardwareOutputs(dir, report);
        replayDurations(dir);
        report << "PASS 15/30/60-second replay rings with accelerated synthetic A/V\n";
        report << "PASS automatic/manual background remux preserves codecs, packets and original; overwrite rejected\n";
        report << "PASS recording/replay decode from selected Unicode folder, unique names and invalid-folder rejection\n";
        separateTrackRemux(dir);
        report << "PASS independent microphone/desktop tracks survive verified MKV-to-MP4 remux\n";
        report << "NOT EXERCISED by this media suite: live hardware capture/WASAPI, long-run "
                  "stability, physical DPI and gaming performance; UI checks run separately\n";
        return 0;
    } catch (const std::exception& e) {
        report << "FAIL " << e.what() << "\n";
        Log::get().write(e.what());
        return 1;
    }
}
} // namespace nco
