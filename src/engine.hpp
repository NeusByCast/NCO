#pragma once
#include "audio.hpp"
#include "media.hpp"
namespace nco {
struct EngineStats {
    bool recording = false, paused = false, replay = false, ready = false, gpu = false, finishing = false,
         starting = false;
    uint64_t frames = 0, dropped = 0, freshFrames = 0;
    size_t replayBytes = 0, encodeQueue = 0, readbackQueue = 0;
    uint64_t readbackDropped = 0;
    int64_t durationUs = 0;
    Size output{};
    double actualFps = 0, bitrateMbps = 0, prepareMs = 0, submitMs = 0;
    double encodeMs = 0, captureMs = 0, cpuPercent = 0, workingSetMiB = 0;
    std::string encoder, encoderAdapter;
    bool gpuTexturePath = false;
};
struct Preview {
    Size size;
    std::vector<uint8_t> pixels;
    uint64_t serial = 0;
};
class Engine {
    enum Action { Record, Pause, ReplaySave, AudioRefresh };
    std::jthread thread_;
    std::mutex mutex_, stopMutex_;
    HANDLE wake_ = nullptr;
    Config config_;
    Source source_;
    uint64_t generation_ = 0;
    std::deque<Action> actions_;
    std::deque<std::pair<std::filesystem::path, std::filesystem::path>> remuxRequests_;
    EngineStats stats_;
    Preview preview_;
    std::atomic<bool> visible_ = true;
    std::function<void(std::string, bool)> notify_;
    std::atomic<bool> diskFailed_ = false;
    void run(std::stop_token token);

  public:
    explicit Engine(std::function<void(std::string, bool)> notify);
    ~Engine();
    void configure(const Config& cfg, const Source& source);
    void recording();
    void pause();
    void replay();
    void refreshAudio();
    void remux(std::filesystem::path source, std::filesystem::path target);
    void visible(bool value) {
        visible_ = value;
        SetEvent(wake_);
    }
    EngineStats stats();
    Preview preview(uint64_t seen = 0);
    bool previewChanged(uint64_t seen);
    void stop();
};
} // namespace nco
