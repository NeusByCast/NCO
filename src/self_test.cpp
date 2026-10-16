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
    size_t packets = 0;
    int64_t first = INT64_MAX, end = INT64_MIN;
    AVCodecID codec = AV_CODEC_ID_NONE;
};

std::vector<StreamResult> inspect(const std::filesystem::path& path) {
    AVFormatContext* in = nullptr;
    avcheck(avformat_open_input(&in, utf8(path.wstring()).c_str(), nullptr, nullptr), "Test open");
    AVPacket* packet = av_packet_alloc();
    try {
        avcheck(avformat_find_stream_info(in, nullptr), "Test stream info");
        std::vector<StreamResult> result(in->nb_streams);
        for (unsigned i = 0; i < in->nb_streams; ++i)
            result[i].codec = in->streams[i]->codecpar->codec_id;
        int status;
        while ((status = av_read_frame(in, packet)) >= 0) {
            auto& s = result.at(size_t(packet->stream_index));
            auto tb = in->streams[packet->stream_index]->time_base;
            require(packet->pts != AV_NOPTS_VALUE, "Output packet has no presentation timestamp");
            ++s.packets;
            s.first = std::min(s.first, av_rescale_q(packet->pts, tb, {1, 1000000}));
            s.end = std::max(s.end, av_rescale_q(packet->pts + packet->duration, tb, {1, 1000000}));
            av_packet_unref(packet);
        }
        require(status == AVERROR_EOF, "Output did not end cleanly");
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
    auto accept = [&](std::vector<Packet> input) {
        for (auto& packet : input) {
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
    auto streams = inspect(path);
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
    for (int fps : {15, 30, 60}) {
        for (int quality = 0; quality < 5; ++quality) {
            for (int rateControl = 0; rateControl < 3; ++rateControl) {
                Config config;
                config.bitrateKbps = 0;
                config.rateControl = rateControl;
                config.fps = fps;
                config.quality = quality;
                config.encoder = "libx264";
                config.format = "mkv";
                // Exercise native size and custom scaling, including even-dimension rounding.
                config.resolutionMode = quality % 2 ? 5 : 0;
                config.width = 641;
                config.height = 361;
                VideoEncoder encoder;
                encoder.open(config, nullptr, {320, 180}, true);
                Muxer mux;
                auto path = dir / ("matrix-" + std::to_string(fps) + "-" +
                                   std::to_string(quality) + "-" +
                                   std::to_string(rateControl) + ".mkv");
                mux.open(path, {encoder.track()});
                for (int frame = 0; frame < fps; ++frame)
                    for (auto& p : encoder.synthetic(frame, frame == 0))
                        mux.write(p);
                for (auto& p : encoder.flush())
                    mux.write(p);
                mux.close();
                auto result = inspect(path);
                require(result[0].packets == size_t(fps), "FPS/quality matrix lost a video frame");
                require(result[0].end - result[0].first > 850000, "FPS matrix duration incorrect");
            }
        }
    }
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
        auto result = inspect(path);
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
        platformSelfTest(report);
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
            auto encoded = pipeline.flush();
            require(encoded.size() == 2 && pipeline.metrics().frames == 2,
                    "Worker finalization lost accepted frames");
            require(encoded[0].p->pts < encoded[1].p->pts,
                    "Worker changed video timestamp order");
            require(track.par->codec_type == AVMEDIA_TYPE_VIDEO, "Worker metadata missing");
        }
        report << "PASS bounded encoder worker: slow encode does not block submission; overflow rejected; accepted frames drain with ordered timestamps\n";
        for (const auto& format : {"mp4", "mkv", "mov", "webm"}) {
            containerTest(dir, format);
            report << "PASS " << format
                   << ": synthetic A/V, timestamps, packet replay, pause timeline\n";
            report.flush();
        }
        videoMatrix(dir);
        report << "PASS 15/30/60 FPS, five quality levels, all three rate controls, native/custom scaling\n";
        replayDurations(dir);
        report << "PASS 15/30/60-second replay rings with accelerated synthetic A/V\n";
        report << "PASS automatic/manual background remux preserves codecs, packets and original; overwrite rejected\n";
        separateTrackRemux(dir);
        report << "PASS independent microphone/desktop tracks survive verified MKV-to-MP4 remux\n";
        report << "UNVERIFIED hardware capture, WASAPI, real audio synchronization, long-run "
                  "stability, interactive controls, DPI and gaming performance\n";
        return 0;
    } catch (const std::exception& e) {
        report << "FAIL " << e.what() << "\n";
        Log::get().write(e.what());
        return 1;
    }
}
} // namespace nco
