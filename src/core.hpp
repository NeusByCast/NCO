#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace nco {
struct Size {
    int w = 0, h = 0;
};
struct FitRect {
    int x, y, w, h;
};
// Round deadlines up: truncation wakes before the next frame's integer timestamp
// at rates such as 30/60 FPS, submitting the preceding timestamp twice.
inline int64_t frameDeadlineUs(int64_t epoch, int64_t index, int fps) {
    return epoch + (index * 1000000 + fps - 1) / fps;
}
inline bool recordingStopComplete(bool replay, bool draining, bool encoderFinished, bool fenceReached) {
    return draining ? encoderFinished : replay && fenceReached;
}
inline FitRect letterbox(Size source, Size output) {
    const double scale = std::min(double(output.w) / source.w, double(output.h) / source.h);
    const int w = std::max(2, int(source.w * scale) & ~1);
    const int h = std::max(2, int(source.h * scale) & ~1);
    return {((output.w - w) / 2) & ~1, ((output.h - h) / 2) & ~1, w, h};
}
inline Size resolution(int mode, int w, int h, Size native) {
    static constexpr Size presets[] = {
        {0, 0}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    Size s = mode == 0 ? native : mode == 5 ? Size{w, h} : presets[std::clamp(mode, 1, 4)];
    if (s.w < 64 || s.h < 64 || s.w > 7680 || s.h > 4320)
        throw std::runtime_error("Resolution must be between 64x64 and 7680x4320");
    return {s.w & ~1, s.h & ~1};
}
inline int bitrate(Size s, int fps, int quality, bool efficient) {
    // High-motion game footage needs more bits than general desktop capture.
    // These remain resolution/FPS scaled and are capped below 150 Mbps.
    constexpr double bpp[] = {0.06, 0.10, 0.24, 0.32, 0.40};
    return std::clamp(static_cast<int>(s.w * double(s.h) * fps * bpp[std::clamp(quality, 0, 4)] *
                                       (efficient ? .65 : 1.0)),
                      500000, 150000000);
}
// Adapter speed ranking does not determine hardware texture compatibility.
// Prefer encoding on the capture adapter when the encoder supports its vendor.
inline bool textureEncoderCompatible(const std::string& codec, uint32_t vendor) {
    return (codec.find("amf") != std::string::npos && vendor == 0x1002) ||
           (codec.find("nvenc") != std::string::npos && vendor == 0x10de) ||
           codec.find("_mf") != std::string::npos;
}
inline bool shouldRetryCaptureOnEncoderGpu(int backend, bool desktopDuplication,
                                            bool gpuTexturePath, const std::string& codec) {
    return backend == 0 && desktopDuplication && !gpuTexturePath &&
           (codec.find("amf") != std::string::npos || codec.find("nvenc") != std::string::npos ||
            codec.find("_mf") != std::string::npos);
}
inline bool containerCompatible(const std::string& format, const std::string& codec) {
    if (format != "mp4" && format != "mkv" && format != "mov" && format != "webm")
        return false;
    if (codec == "auto")
        return true;
    const bool av1 = codec.find("av1") != std::string::npos;
    const bool vp9 = codec.find("vp9") != std::string::npos;
    const bool h264 = codec.find("264") != std::string::npos;
    const bool hevc = codec.find("hevc") != std::string::npos;
    if (format == "webm")
        return av1 || vp9;
    // FFmpeg's QuickTime MOV muxer does not accept AV1 or VP9.
    if (format == "mov")
        return h264 || hevc;
    return h264 || hevc || av1 || vp9;
}
struct Shortcut {
    unsigned key = 0, mods = 0;
    bool operator==(const Shortcut&) const = default;
};
inline bool duplicate(const std::vector<Shortcut>& keys, size_t index, Shortcut key) {
    if (!key.key)
        return false;
    for (size_t i = 0; i < keys.size(); ++i)
        if (i != index && keys[i] == key)
            return true;
    return false;
}
// Keep a decodable GOP at the front. Hard byte limit always takes priority.
template <class T> class ReplayRing {
    std::deque<T> packets_;
    size_t bytes_ = 0;

  public:
    size_t cap = 256u * 1024u * 1024u;
    int seconds = 30;
    void clear() {
        packets_.clear();
        bytes_ = 0;
    }
    void push(T p) {
        bytes_ += p.bytes();
        // Encoded video may arrive after newer audio on the asynchronous path.
        const auto before = [](const T& left, const T& right) {
            if (left.us() != right.us()) return left.us() < right.us();
            return left.key() && !right.key();
        };
        if (packets_.empty() || !before(p, packets_.back()))
            packets_.push_back(std::move(p));
        else {
            auto position = std::upper_bound(packets_.begin(), packets_.end(), p, before);
            packets_.insert(position, std::move(p));
        }
        const int64_t cutoff = packets_.back().us() - int64_t(seconds) * 1000000;
        while (packets_.size() > 1) {
            auto next = std::find_if(std::next(packets_.begin()), packets_.end(),
                                     [](const T& v) { return v.key(); });
            if (next == packets_.end())
                break;
            if (next->us() > cutoff && bytes_ <= cap)
                break;
            // Count the prefix once instead of searching for its terminating
            // keyframe after every removed packet.
            auto remaining = std::distance(packets_.begin(), next);
            while (remaining-- > 0) {
                bytes_ -= packets_.front().bytes();
                packets_.pop_front();
            }
        }
        if (bytes_ > cap)
            clear();
        const bool hasKey = std::any_of(packets_.begin(), packets_.end(),
                                        [](const T& packet) { return packet.key(); });
        while (hasKey && !packets_.empty() && !packets_.front().key()) {
            bytes_ -= packets_.front().bytes();
            packets_.pop_front();
        }
    }
    std::vector<T> snapshot() const {
        if (packets_.empty()) return {};
        const int64_t cutoff = packets_.back().us() - int64_t(std::clamp(seconds, 1, 60)) * 1000000;
        // Start at the newest keyframe at or before the requested window. This
        // keeps the clip decodable and limits the GOP overhang to one GOP.
        auto start = packets_.end();
        for (auto it = packets_.begin(); it != packets_.end(); ++it)
            if (it->key() && it->us() <= cutoff) start = it;
        if (start == packets_.end())
            start = std::find_if(packets_.begin(), packets_.end(),
                                 [](const T& packet) { return packet.key(); });
        if (start == packets_.end()) return {};
        return {start, packets_.end()};
    }
    size_t bytes() const {
        return bytes_;
    }
    int64_t durationUs() const {
        return packets_.size() < 2 ? 0 : packets_.back().us() - packets_.front().us();
    }
    size_t keyframes() const {
        return size_t(std::count_if(packets_.begin(), packets_.end(),
                                    [](const T& packet) { return packet.key(); }));
    }
};
} // namespace nco
