#include "core.hpp"
#include <iostream>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (0)
struct P {
    int64_t t;
    bool k;
    size_t b;
    int64_t us() const {
        return t;
    }
    bool key() const {
        return k;
    }
    size_t bytes() const {
        return b;
    }
};
int main() {
    using namespace nco;
    // Enabling replay after a drain request must still wait for that worker.
    CHECK(!recordingStopComplete(true, true, false, true));
    CHECK(recordingStopComplete(true, true, true, false));
    CHECK(!recordingStopComplete(false, true, false, true));
    CHECK(recordingStopComplete(false, true, true, true));
    CHECK(recordingStopComplete(true, false, false, true));
    CHECK(!recordingStopComplete(true, false, false, false));
    for (int fps : {15, 30, 60}) {
        constexpr int64_t epoch = 1234567;
        int64_t previous = epoch - 1;
        for (int64_t frame = 0; frame < fps * 10; ++frame) {
            const int64_t deadline = frameDeadlineUs(epoch, frame, fps);
            CHECK(deadline > previous);
            CHECK((deadline - epoch) * fps / 1000000 == frame);
            if (frame)
                CHECK((deadline - epoch - 1) * fps / 1000000 == frame - 1);
            previous = deadline;
        }
    }
    CHECK(resolution(0, 0, 0, {1921, 1081}).w == 1920);
    CHECK(resolution(5, 1001, 701, {}).h == 700);
    const Size presetSizes[] = {{1920, 1080}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {642, 362}};
    for (int mode = 0; mode <= 5; ++mode) {
        const auto size = resolution(mode, 643, 363, {1920, 1080});
        CHECK(size.w == presetSizes[mode].w && size.h == presetSizes[mode].h);
    }
    auto wide = letterbox({2560, 1080}, {1920, 1080});
    CHECK(wide.w == 1920 && wide.h == 810 && wide.y == 134);
    auto tall = letterbox({1080, 1920}, {1920, 1080});
    CHECK(tall.h == 1080 && tall.w == 606 && tall.x == 656);
    bool rejected = false;
    try {
        resolution(5, 10, 10, {});
    } catch (...) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(bitrate({1920, 1080}, 60, 4, false) > bitrate({1920, 1080}, 30, 2, false));
    CHECK(bitrate({1920, 1080}, 60, 2, false) >= 29000000);
    for (const auto& name : {"h264_amf", "hevc_amf", "av1_amf"}) {
        CHECK(textureEncoderCompatible(name, 0x1002)); // Both AMD integrated and discrete GPUs.
        CHECK(!textureEncoderCompatible(name, 0x10de));
        CHECK(!textureEncoderCompatible(name, 0x8086));
    }
    CHECK(textureEncoderCompatible("h264_nvenc", 0x10de));
    CHECK(!textureEncoderCompatible("h264_nvenc", 0x1002));
    CHECK(!textureEncoderCompatible("h264_qsv", 0x8086)); // No QSV texture implementation yet.
    CHECK(!textureEncoderCompatible("libx264", 0x1002));
    CHECK(textureEncoderCompatible("h264_mf", 0x8086));
    CHECK(shouldRetryCaptureOnEncoderGpu(0, true, false, "h264_amf"));
    CHECK(shouldRetryCaptureOnEncoderGpu(0, true, false, "hevc_nvenc"));
    CHECK(shouldRetryCaptureOnEncoderGpu(0, true, false, "h264_mf"));
    CHECK(!shouldRetryCaptureOnEncoderGpu(1, true, false, "h264_amf"));
    CHECK(!shouldRetryCaptureOnEncoderGpu(2, true, false, "h264_amf"));
    CHECK(!shouldRetryCaptureOnEncoderGpu(0, false, false, "h264_amf"));
    CHECK(!shouldRetryCaptureOnEncoderGpu(0, true, true, "h264_amf"));
    CHECK(!shouldRetryCaptureOnEncoderGpu(0, true, false, "libx264"));
    CHECK(!shouldRetryCaptureOnEncoderGpu(0, true, false, "h264_qsv"));
    CHECK(!containerCompatible("webm", "h264_nvenc"));
    CHECK(containerCompatible("webm", "av1_amf"));
    CHECK(containerCompatible("webm", "auto"));
    CHECK(!containerCompatible("mov", "av1_nvenc"));
    CHECK(!containerCompatible("mov", "libvpx-vp9"));
    CHECK(containerCompatible("mov", "hevc_qsv"));
    CHECK(containerCompatible("mp4", "av1_nvenc"));
    CHECK(!containerCompatible("invalid", "h264_nvenc"));
    CHECK(duplicate({{1, 0}, {2, 0}}, 1, {1, 0}));
    CHECK(!duplicate({{1, 0}, {2, 0}}, 1, {0, 0}));
    ReplayRing<P> r;
    r.seconds = 15;
    r.cap = 1000;
    for (int i = 0; i < 40; ++i)
        r.push({i * 1000000LL, i % 2 == 0, 10});
    auto s = r.snapshot();
    CHECK(!s.empty() && s.front().key());
    CHECK(s.front().t >= 22000000 && s.front().t <= 24000000);
    CHECK(s.back().t - s.front().t >= 15000000);
    CHECK(s.back().t - s.front().t <= 17000000);
    CHECK(r.durationUs() <= 17000000);
    CHECK(r.keyframes() > 0);
    r.seconds = 15;
    auto shortReplay = r.snapshot();
    CHECK(shortReplay.front().key() && shortReplay.back().t - shortReplay.front().t <= 17000000);
    r.cap = 5;
    r.push({40000000, true, 20});
    CHECK(r.bytes() == 0);
    r.cap = 100;
    r.push({0, false, 5});
    CHECK(r.snapshot().empty());
    ReplayRing<P> reordered;
    reordered.push({10000, false, 10}); // Audio arrives before the asynchronous first keyframe.
    CHECK(reordered.snapshot().empty());
    reordered.push({0, true, 10});
    reordered.push({30000, false, 10});
    reordered.push({20000, false, 10});
    auto ordered = reordered.snapshot();
    CHECK(ordered.size() == 4 && ordered.front().key());
    CHECK(ordered[1].t == 10000 && ordered[2].t == 20000 && ordered[3].t == 30000);
    CHECK(reordered.bytes() == 40);
    std::cout << "NCO core tests passed\n";
}
