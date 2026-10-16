#pragma once
#include "config.hpp"
#include "audio_core.hpp"
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
} // namespace nco
