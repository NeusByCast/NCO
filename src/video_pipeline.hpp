#pragma once
#include "media.hpp"
#include <array>
namespace nco {
// Two immutable GPU mailboxes: one encoding, one pending. Never wait for a slot.
class VideoPipeline {
    struct Job { int slot; int64_t pts; bool key; };
    VideoEncoder encoder_;
    Capture* capture_ = nullptr;
    std::array<Com<ID3D11Texture2D>, 2> textures_;
    std::array<bool, 2> busy_{};
    std::deque<Job> jobs_;
    std::vector<Packet> packets_;
    size_t packetBytes_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool finishing_ = false, finished_ = false, synthetic_ = false, testFlushFailure_ = false;
    int testDelayMs_ = 0;
    std::exception_ptr error_;
    void run();
    void publish(std::vector<Packet> packets);
  public:
    class DrainTimeout : public std::runtime_error {
      public:
        DrainTimeout() : std::runtime_error("Encoder shutdown timed out") {}
    };
    struct Metrics {
        uint64_t frames = 0, readbackDropped = 0;
        double prepareMs = 0, submitMs = 0, encodeMs = 0;
        size_t queued = 0, readbackPending = 0;
    };
  private:
    Metrics metrics_;
  public:
    VideoPipeline(const Config& config, Capture* capture, Size input,
                  bool synthetic = false, int testDelayMs = 0, bool testFlushFailure = false);
    ~VideoPipeline();
    Track track() const { return encoder_.track(); }
    const std::string& selected() const { return encoder_.selected; }
    const std::string& adapter() const { return encoder_.selectedAdapter; }
    bool gpu() const { return encoder_.gpu(); }
    bool submit(ID3D11Texture2D* texture, int64_t pts, bool key);
    std::vector<Packet> collect(bool recoverOnError = false);
    void finish();
    bool finished();
    bool waitForFinish(std::chrono::milliseconds timeout);
    std::vector<Packet> flush(std::chrono::milliseconds timeout = std::chrono::seconds(12));
    // Transfer both objects together: the worker retains a raw Capture pointer.
    // Used only after a timeout, when joining on the caller would hang the app.
    static void retire(std::unique_ptr<VideoPipeline> video, std::unique_ptr<Capture> capture) noexcept;
    Metrics metrics();
};
}
