#pragma once
#include "config.hpp"
#include <atomic>
#include <audioclient.h>
#include <deque>
#include <mmdeviceapi.h>
#include <thread>
namespace nco {
struct AudioDevice {
    std::wstring id, name;
};
std::vector<AudioDevice> audioDevices(bool input);
struct AudioChunk {
    int64_t us = 0;
    std::vector<float> samples;
};
class AudioCapture {
    std::jthread thread_;
    std::mutex mutex_;
    std::deque<AudioChunk> queue_;
    size_t samples_ = 0;

  public:
    std::atomic<uint64_t> discontinuities = 0;
    void start(bool input, std::wstring device, std::function<void(std::string)> error);
    void stop();
    std::vector<AudioChunk> drain();
    ~AudioCapture() {
        stop();
    }
};
// Timestamp-indexed stereo samples, bounded to two seconds. Gaps become silence.
class AudioTimeline {
    std::vector<float> data_ = std::vector<float>(48000 * 2 * 2);
    std::vector<int64_t> tags_ = std::vector<int64_t>(48000 * 2, -1);

  public:
    void add(const AudioChunk& c, int64_t epoch);
    void read(int64_t sample, int count, float gain, std::vector<float>& out) const;
};
} // namespace nco
