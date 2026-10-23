#include "video_pipeline.hpp"
#include <avrt.h>
namespace nco {
VideoPipeline::VideoPipeline(const Config& config, Capture* capture, Size input,
                             bool synthetic, int testDelayMs, bool testFlushFailure)
    : capture_(capture), synthetic_(synthetic), testFlushFailure_(testFlushFailure),
      testDelayMs_(testDelayMs) {
    encoder_.open(config, capture, input, synthetic);
    if (!synthetic) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = input.w;
        desc.Height = input.h;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (auto& texture : textures_)
            check(capture->device()->CreateTexture2D(&desc, nullptr, &texture),
                  "Encoder mailbox texture");
    }
    thread_ = std::thread([this] { run(); });
}
VideoPipeline::~VideoPipeline() {
    {
        std::lock_guard lock(mutex_);
        finishing_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}
bool VideoPipeline::submit(ID3D11Texture2D* texture, int64_t pts, bool key) {
    int slot = -1;
    {
        std::lock_guard lock(mutex_);
        if (error_) std::rethrow_exception(error_);
        if (finishing_) return false;
        for (int i = 0; i < int(busy_.size()); ++i)
            if (!busy_[size_t(i)]) { slot = i; busy_[size_t(i)] = true; break; }
    }
    if (slot < 0) return false;
    // Copy before publishing; capture can immediately reuse its own mailbox.
    if (!synthetic_)
        capture_->context()->CopyResource(textures_[size_t(slot)].Get(), texture);
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({slot, pts, key});
    }
    cv_.notify_one();
    return true;
}
void VideoPipeline::publish(std::vector<Packet> input) {
    std::lock_guard lock(mutex_);
    for (auto& packet : input) {
        if (packetBytes_ + packet.bytes() > 32u * 1024u * 1024u)
            throw std::runtime_error("Encoded video queue exceeded 32 MiB");
        packetBytes_ += packet.bytes();
        packets_.push_back(std::move(packet));
    }
}
void VideoPipeline::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Playback", &task);
    try {
        while (true) {
            Job job{};
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return finishing_ || !jobs_.empty(); });
                if (jobs_.empty()) break;
                job = jobs_.front();
                jobs_.pop_front();
            }
            if (testDelayMs_) Sleep(DWORD(testDelayMs_));
            const auto begin = clockUs();
            auto output = synthetic_ ? encoder_.synthetic(job.pts, job.key)
                                     : encoder_.encode(textures_[size_t(job.slot)].Get(), job.pts, job.key);
            const auto elapsed = double(clockUs() - begin) / 1000;
            publish(std::move(output));
            {
                std::lock_guard lock(mutex_);
                busy_[size_t(job.slot)] = false;
                metrics_.frames = encoder_.submittedFrames;
                metrics_.readbackDropped = encoder_.readbackDrops;
                metrics_.readbackPending = encoder_.pendingReadbacks();
                metrics_.prepareMs = encoder_.prepareMs;
                metrics_.submitMs = encoder_.submitMs;
                metrics_.encodeMs = metrics_.encodeMs * .9 + elapsed * .1;
            }
        }
        if (synthetic_ && testFlushFailure_)
            throw std::runtime_error("Injected encoder drain failure");
        publish(encoder_.flush());
        std::lock_guard lock(mutex_);
        metrics_.frames = encoder_.submittedFrames;
        metrics_.readbackDropped = encoder_.readbackDrops;
                metrics_.readbackPending = encoder_.pendingReadbacks();
    } catch (...) {
        std::lock_guard lock(mutex_);
        error_ = std::current_exception();
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
    {
        std::lock_guard lock(mutex_);
        finished_ = true;
    }
    cv_.notify_all();
}
std::vector<Packet> VideoPipeline::collect(bool recoverOnError) {
    std::lock_guard lock(mutex_);
    // The engine's error path can recover completed packets before closing
    // the file, without masking a drain error during normal operation.
    if (error_ && !recoverOnError) std::rethrow_exception(error_);
    std::vector<Packet> result;
    result.swap(packets_);
    packetBytes_ = 0;
    return result;
}
void VideoPipeline::finish() {
    {
        std::lock_guard lock(mutex_);
        finishing_ = true;
    }
    cv_.notify_one();
}
bool VideoPipeline::finished() {
    std::lock_guard lock(mutex_);
    return finished_;
}
bool VideoPipeline::waitForFinish(std::chrono::milliseconds timeout) {
    finish();
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return finished_; });
}
std::vector<Packet> VideoPipeline::flush(std::chrono::milliseconds timeout) {
    if (!waitForFinish(timeout)) throw DrainTimeout();
    if (thread_.joinable()) thread_.join();
    return collect();
}
void VideoPipeline::retire(std::unique_ptr<VideoPipeline> video,
                           std::unique_ptr<Capture> capture) noexcept {
    // Never detach the encoder and destroy its resources underneath it. A
    // cleanup thread owns the entire failed pipeline, without Engine/UI refs.
    // If the driver recovers, join and free it there. A permanently stuck
    // driver retains this one pipeline until process exit; Engine blocks retries.
    auto* worker = video.release();
    auto* source = capture.release();
    try {
        std::thread([worker, source] {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            delete worker;
            delete source;
            if (SUCCEEDED(co)) CoUninitialize();
        }).detach();
    } catch (...) {
        // Thread creation failed. Retain the same bounded resources until exit
        // rather than blocking shutdown or freeing memory still used by a driver.
    }
}
VideoPipeline::Metrics VideoPipeline::metrics() {
    std::lock_guard lock(mutex_);
    auto result = metrics_;
    result.queued = jobs_.size();
    return result;
}
}
