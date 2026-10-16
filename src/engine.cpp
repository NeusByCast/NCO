#include "engine.hpp"
#include <avrt.h>
#include <cmath>
#include <psapi.h>
namespace nco {
Engine::Engine(std::function<void(std::string, bool)> notify) : notify_(std::move(notify)) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_)
        throw std::runtime_error("Cannot create recording wake event");
    try {
        thread_ = std::jthread([this](std::stop_token token) { run(token); });
    } catch (...) {
        CloseHandle(wake_);
        throw;
    }
}
Engine::~Engine() {
    stop();
    CloseHandle(wake_);
}
void Engine::stop() {
    std::lock_guard stopLock(stopMutex_);
    if (thread_.joinable()) {
        thread_.request_stop();
        SetEvent(wake_);
        thread_.join();
    }
}
void Engine::configure(const Config& cfg, const Source& s) {
    std::lock_guard lock(mutex_);
    config_ = cfg;
    source_ = s;
    ++generation_;
    SetEvent(wake_);
}
void Engine::recording() {
    std::lock_guard lock(mutex_);
    actions_.push_back(Record);
    SetEvent(wake_);
}
void Engine::pause() {
    std::lock_guard lock(mutex_);
    actions_.push_back(Pause);
    SetEvent(wake_);
}
void Engine::replay() {
    std::lock_guard lock(mutex_);
    actions_.push_back(ReplaySave);
    SetEvent(wake_);
}
void Engine::remux(std::filesystem::path source, std::filesystem::path target) {
    std::lock_guard lock(mutex_);
    if (remuxRequests_.size() >= 8)
        throw std::runtime_error("Remux queue is full; wait for pending exports");
    remuxRequests_.emplace_back(std::move(source), std::move(target));
    SetEvent(wake_);
}
void Engine::refreshAudio() {
    std::lock_guard lock(mutex_);
    actions_.push_back(AudioRefresh);
    SetEvent(wake_);
}
EngineStats Engine::stats() {
    std::lock_guard lock(mutex_);
    return stats_;
}
Preview Engine::preview(uint64_t seen) {
    std::lock_guard lock(mutex_);
    return seen == preview_.serial ? Preview{} : preview_;
}
void Engine::run(std::stop_token token) {
    FrameTimer frameTimer;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Playback", &task);
    DiskWriter disk([this](std::string message, bool error) {
        if (error && message.rfind("Recording failed:", 0) == 0)
            diskFailed_ = true;
        notify_(std::move(message), error);
    });
    std::unique_ptr<Capture> capture;
    std::unique_ptr<VideoEncoder> video;
    std::vector<std::unique_ptr<AudioEncoder>> audio;
    AudioCapture mic, desktop;
    AudioTimeline mt, dt;
    AudioConditioner micFilter, desktopFilter;
    std::vector<Track> tracks;
    ReplayRing<Packet> ring;
    Config cfg;
    Source source;
    uint64_t gen = 0;
    int64_t epoch = 0, nextVideo = 0, nextFrame = 0, audioPts = 0, recordStart = 0, pauseStart = 0,
            removed = 0, lastPreview = 0, lastUsage = 0;
    uint64_t lastCpu = 0, usageFrames = 0, videoBytes = 0, usageBytes = 0;
    bool recording = false, paused = false, gate = false, forceKey = false, replay = false;
    bool suspended = false;
    EngineStats local;
    Com<ID3D11Texture2D> texture;
    auto endPipeline = [&] {
        if (video) {
            for (auto& p : video->flush()) {
                if (recording && !paused)
                    disk.write(p, removed);
                if (replay)
                    ring.push(p);
            }
            for (auto& a : audio)
                for (auto& p : a->flush()) {
                    if (recording && !paused)
                        disk.write(p, removed);
                    if (replay)
                        ring.push(p);
                }
        }
        if (video)
            Log::get().write("Pipeline finalized frames=" + std::to_string(local.frames) +
                             " dropped=" + std::to_string(local.dropped));
        video.reset();
        audio.clear();
        mic.stop();
        desktop.stop();
        tracks.clear();
    };
    auto beginPipeline = [&] {
        if (video)
            return;
        if (!capture && (source.monitor || source.window)) {
            capture = std::make_unique<Capture>();
            capture->open(source, cfg.captureBackend);
            suspended = false;
            local.ready = true;
        }
        if (!capture)
            throw std::runtime_error("Choose an available capture source");
        video = std::make_unique<VideoEncoder>();
        video->open(cfg, capture.get(), capture->size());
        tracks = {video->track()};
        int count = cfg.separateTracks ? 2 : 1;
        for (int i = 0; i < count; ++i) {
            auto a = std::make_unique<AudioEncoder>();
            a->open(cfg.format == "webm", i + 1);
            tracks.push_back(a->track());
            tracks.back().name = cfg.separateTracks ? (i == 0 ? "Microphone" : "Desktop audio")
                                                    : "Microphone + desktop";
            audio.push_back(std::move(a));
        }
        epoch = clockUs();
        nextVideo = epoch;
        nextFrame = 0;
        audioPts = 0;
        mt = AudioTimeline();
        dt = AudioTimeline();
        micFilter.reset();
        desktopFilter.reset();
        ring.clear();
        mic.start(true, cfg.micDevice, [this](std::string e) { notify_(std::move(e), true); });
        desktop.start(false, cfg.desktopDevice,
                      [this](std::string e) { notify_(std::move(e), true); });
        local.encoder = video->selected;
        local.encoderAdapter = video->selectedAdapter;
        local.gpuTexturePath = video->gpu();
        local.gpu = video->selected.find("lib") != 0;
        local.frames = local.dropped = local.freshFrames = 0;
        local.encodeMs = local.captureMs = local.prepareMs = local.submitMs = local.actualFps = 0;
        local.output = resolution(cfg.resolutionMode, cfg.width, cfg.height, capture->size());
        usageFrames = videoBytes = usageBytes = 0;
        lastUsage = 0;
        Log::get().write("Pipeline started replay=" + std::to_string(cfg.replay) +
                         " audio_tracks=" + std::to_string(count) +
                         " auto_remux=" + std::to_string(cfg.remux) +
                         " capture=" + std::to_string(capture->size().w) + "x" +
                         std::to_string(capture->size().h));
    };
    auto packets = [&](std::vector<Packet> ps) {
        for (auto& p : ps) {
            if (p.stream == 0) videoBytes += p.bytes();
            if (replay)
                ring.push(p);
            if (recording && !paused) {
                if (gate) {
                    if (!p.key())
                        continue;
                    gate = false;
                }
                disk.write(p, removed);
            }
        }
    };
    while (!token.stop_requested()) {
        try {
            Config current;
            Source src;
            uint64_t newGen;
            std::deque<Action> actions;
            std::deque<std::pair<std::filesystem::path, std::filesystem::path>> remuxRequests;
            {
                std::lock_guard lock(mutex_);
                newGen = generation_;
                current = config_;
                src = source_;
                actions.swap(actions_);
                remuxRequests.swap(remuxRequests_);
            }
            for (auto& request : remuxRequests) {
                try {
                    disk.remux(std::move(request.first), std::move(request.second));
                } catch (const std::exception& e) {
                    notify_(std::string("Remux failed: ") + e.what(), true);
                }
            }
            if (newGen != gen) {
                bool captureChanged =
                    current.captureBackend != cfg.captureBackend || src.window != source.window ||
                    src.monitor != source.monitor || src.custom != source.custom ||
                    memcmp(&src.region, &source.region, sizeof(RECT)) != 0;
                bool pipelineChanged =
                    current.fps != cfg.fps || current.width != cfg.width ||
                    current.height != cfg.height || current.resolutionMode != cfg.resolutionMode ||
                    current.encoder != cfg.encoder || current.format != cfg.format ||
                    current.quality != cfg.quality || current.bitrateKbps != cfg.bitrateKbps ||
                    current.separateTracks != cfg.separateTracks ||
                    current.captureBackend != cfg.captureBackend;
                if (recording && pipelineChanged) {
                    current.fps = cfg.fps;
                    current.width = cfg.width;
                    current.height = cfg.height;
                    current.resolutionMode = cfg.resolutionMode;
                    current.encoder = cfg.encoder;
                    current.format = cfg.format;
                    current.quality = cfg.quality;
                    current.bitrateKbps = cfg.bitrateKbps;
                    current.separateTracks = cfg.separateTracks;
                    current.captureBackend = cfg.captureBackend;
                    notify_("Encoding changes will take effect after recording stops", true);
                }
                if (!recording && (pipelineChanged || captureChanged)) {
                    endPipeline();
                    ring.clear();
                }
                if (!recording && (captureChanged || !capture) && (src.monitor || src.window)) {
                    capture.reset();
                    texture.Reset();
                    capture = std::make_unique<Capture>();
                    capture->open(src, current.captureBackend);
                    source = src;
                    local.ready = true;
                }
                if (video && (current.micDevice != cfg.micDevice ||
                              current.desktopDevice != cfg.desktopDevice)) {
                    mic.start(true, current.micDevice,
                              [this](std::string e) { notify_(std::move(e), true); });
                    desktop.start(false, current.desktopDevice,
                                  [this](std::string e) { notify_(std::move(e), true); });
                    mt = AudioTimeline();
                    dt = AudioTimeline();
                    micFilter.reset();
                    desktopFilter.reset();
                }
                cfg = current;
                gen = newGen;
                ring.seconds = cfg.replaySeconds;
                replay = cfg.replay;
                if (replay)
                    beginPipeline();
                else {
                    ring.clear();
                    if (!recording)
                        endPipeline();
                }
            }
            for (auto action : actions) {
                if (action == Record) {
                    if (recording) {
                        if (!replay)
                            endPipeline();
                        disk.close();
                        recording = false;
                        paused = false;
                        local.durationUs = 0;
                        gen = 0;
                    } else {
                        beginPipeline();
                        auto path = uniquePath(cfg.directory, "NCO_", cfg.format);
                        disk.open(path, tracks, cfg.remux, cfg.format);
                        recording = true;
                        paused = false;
                        gate = true;
                        forceKey = true;
                        removed = 0;
                        recordStart = clockUs();
                    }
                } else if (action == Pause && recording) {
                    paused = !paused;
                    if (paused)
                        pauseStart = clockUs();
                    else {
                        removed += clockUs() - pauseStart;
                        gate = true;
                        forceKey = true;
                    }
                } else if (action == AudioRefresh && video) {
                    mic.start(true, cfg.micDevice,
                              [this](std::string e) { notify_(std::move(e), true); });
                    desktop.start(false, cfg.desktopDevice,
                                  [this](std::string e) { notify_(std::move(e), true); });
                    mt = AudioTimeline();
                    dt = AudioTimeline();
                    micFilter.reset();
                    desktopFilter.reset();
                } else if (action == ReplaySave) {
                    if (!replay) {
                        notify_("Enable replay in Recording settings first", true);
                        continue;
                    }
                    auto clip = ring.snapshot();
                    if (clip.empty()) {
                        notify_("Replay is warming up; wait for the first keyframe", true);
                        continue;
                    }
                    try {
                        disk.replay(uniquePath(cfg.directory, "Replay_", cfg.format), tracks,
                                    std::move(clip));
                    } catch (const std::exception& e) {
                        notify_(std::string("Replay save declined: ") + e.what(), true);
                    }
                }
            }
            if (diskFailed_.exchange(false) && recording) {
                recording = false;
                paused = false;
                disk.close();
                if (!replay)
                    endPipeline();
            }
            int64_t now = clockUs();
            const bool wasVisible = visible_.load();
            const bool previewVisible = cfg.preview && wasVisible;
            if (!video && !previewVisible && capture) {
                texture.Reset();
                capture.reset();
                suspended = true;
            } else if (suspended && previewVisible && !capture) {
                capture = std::make_unique<Capture>();
                capture->open(source, cfg.captureBackend);
                suspended = false;
            }
            if (capture && source.window && !IsWindow(source.window))
                throw std::runtime_error("The selected window closed. Select another source.");
            if (capture && (video || (cfg.preview && visible_))) {
                int64_t captureBegin = clockUs();
                bool fresh = capture->frame(texture);
                local.captureMs = local.captureMs * .9 + double(clockUs() - captureBegin) / 1000 * .1;
                if (fresh) ++local.freshFrames;
                now = clockUs();
                if (video && texture && now >= nextVideo) {
                    int64_t frameIndex = (now - epoch) * cfg.fps / 1000000;
                    if (frameIndex > nextFrame)
                        local.dropped += uint64_t(frameIndex - nextFrame);
                    nextFrame = frameIndex + 1;
                    int64_t begin = clockUs();
                    packets(video->encode(texture.Get(), frameIndex, forceKey));
                    forceKey = false;
                    local.prepareMs = video->prepareMs;
                    local.submitMs = video->submitMs;
                    local.encodeMs = local.encodeMs * .9 + double(clockUs() - begin) / 1000 * .1;
                    ++local.frames;
                    nextVideo = frameDeadlineUs(epoch, frameIndex + 1, cfg.fps);
                }
                if (cfg.preview && visible_ && texture && now - lastPreview >= (video ? (local.encodeMs > 800.0 / cfg.fps ? 100000 : 33333) : 16667)) {
                    try {
                        auto p = capture->thumbnail(texture.Get());
                        if (!p.second.empty()) {
                        std::lock_guard lock(mutex_);
                        preview_.pixels = std::move(p.second);
                        preview_.size = p.first;
                        ++preview_.serial;
                        }
                    } catch (const std::exception& e) {
                        cfg.preview = false;
                        Log::get().write(std::string("Preview disabled: ") + e.what());
                        notify_(std::string("Preview disabled: ") + e.what(), true);
                    }
                    lastPreview = now;
                }
            }
            if (video) {
                for (auto& chunk : mic.drain())
                    mt.add(chunk, epoch);
                for (auto& chunk : desktop.drain())
                    dt.add(chunk, epoch);
                // Keep 60 ms of capture headroom to absorb endpoint scheduling jitter.
                int64_t due = std::max<int64_t>(0, (clockUs() - epoch - 60000) * 48000 / 1000000);
                int frame = audio[0]->frameSize();
                int iterations = 0;
                while (audioPts + frame <= due && iterations++ < 12) {
                    std::vector<float> m, d;
                    mt.read(audioPts, frame, cfg.mic ? cfg.micVolume * cfg.micGain / 10000.f : 0,
                            m);
                    dt.read(audioPts, frame, cfg.desktop ? cfg.desktopVolume / 100.f : 0, d);
                    if (cfg.mic)
                        micFilter.process(m);
                    else
                        micFilter.reset();
                    if (cfg.desktop)
                        desktopFilter.process(d);
                    else
                        desktopFilter.reset();
                    int64_t pts = audioPts + int64_t(cfg.audioOffsetMs) * 48;
                    if (cfg.separateTracks) {
                        auto limit = [](float x) {
                            float a = std::abs(x);
                            return a <= .9f
                                       ? x
                                       : std::copysign(.9f + .1f * std::tanh((a - .9f) * 10), x);
                        };
                        for (auto& x : m)
                            x = limit(x);
                        for (auto& x : d)
                            x = limit(x);
                        packets(audio[0]->encode(m, pts));
                        packets(audio[1]->encode(d, pts));
                    } else {
                        for (size_t i = 0; i < m.size(); ++i)
                            m[i] =
                                std::abs(m[i] + d[i]) <= .9f
                                    ? m[i] + d[i]
                                    : std::copysign(
                                          .9f + .1f * std::tanh((std::abs(m[i] + d[i]) - .9f) * 10),
                                          m[i] + d[i]);
                        packets(audio[0]->encode(m, pts));
                    }
                    audioPts += frame;
                }
            }
            if (now - lastUsage >= 1000000) {
                FILETIME created, exited, kernel, user;
                if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
                    ULARGE_INTEGER k{}, u{};
                    k.LowPart = kernel.dwLowDateTime;
                    k.HighPart = kernel.dwHighDateTime;
                    u.LowPart = user.dwLowDateTime;
                    u.HighPart = user.dwHighDateTime;
                    uint64_t cpu = k.QuadPart + u.QuadPart;
                    if (lastUsage)
                        local.cpuPercent = double(cpu - lastCpu) / 10 / double(now - lastUsage) *
                                           100 / GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
                    lastCpu = cpu;
                }
                PROCESS_MEMORY_COUNTERS memory{};
                if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
                    local.workingSetMiB = double(memory.WorkingSetSize) / 1048576;
                if (lastUsage && video) {
                    local.actualFps = double(local.frames - usageFrames) * 1000000 / (now - lastUsage);
                    local.bitrateMbps = double(videoBytes - usageBytes) * 8 / (now - lastUsage);
                }
                usageFrames = local.frames;
                usageBytes = videoBytes;
                lastUsage = now;
                if (video)
                    Log::get().write("Frames=" + std::to_string(local.frames) +
                                         " dropped=" + std::to_string(local.dropped) +
                                         " fps=" + std::to_string(local.actualFps) +
                                         " video_Mbps=" + std::to_string(local.bitrateMbps) +
                                         " output=" + std::to_string(local.output.w) + "x" + std::to_string(local.output.h) +
                                         " prepare_ms=" + std::to_string(local.prepareMs) +
                                         " submit_ms=" + std::to_string(local.submitMs) +
                                         " encode_ms=" + std::to_string(local.encodeMs) +
                                         " capture_ms=" + std::to_string(local.captureMs) +
                                         " fresh_frames=" + std::to_string(local.freshFrames) +
                                         " replay_MiB=" + std::to_string(ring.bytes() / 1048576.0) +
                                         " cpu_percent=" + std::to_string(local.cpuPercent) +
                                         " working_set_MiB=" + std::to_string(local.workingSetMiB) +
                                         " mic_discontinuities=" + std::to_string(mic.discontinuities.load()) +
                                         " desktop_discontinuities=" + std::to_string(desktop.discontinuities.load()),
                                     false);
            }
            local.recording = recording;
            local.paused = paused;
            local.replay = replay;
            local.replayBytes = ring.bytes();
            if (recording)
                local.durationUs = (paused ? pauseStart : clockUs()) - recordStart - removed;
            {
                std::lock_guard lock(mutex_);
                stats_ = local;
            }
            int64_t waitUs =
                video ? (texture ? std::clamp(nextVideo - clockUs(), int64_t(1000), int64_t(10000))
                                 : 1000000 / cfg.fps)
                      : 16667;
            // High-resolution deadline wait; configuration/actions interrupt it immediately.
            if (!video && !previewVisible)
                WaitForSingleObject(wake_, INFINITE);
            else if (frameTimer.get()) {
                frameTimer.arm(waitUs);
                HANDLE events[] = {wake_, frameTimer.get()};
                WaitForMultipleObjects(2, events, FALSE, INFINITE);
            } else
                WaitForSingleObject(wake_, DWORD(std::max<int64_t>(1, waitUs / 1000)));

        } catch (const winrt::hresult_error& e) {
            std::string error = utf8(e.message().c_str());
            Log::get().write("Capture error: " + error);
            notify_("Pipeline stopped: " + error, true);
            recording = false;
            paused = false;
            replay = false;
            video.reset();
            audio.clear();
            mic.stop();
            desktop.stop();
            disk.close();
            capture.reset();
            suspended = false;
            texture.Reset();
            std::lock_guard lock(mutex_);
            gen = generation_;
            config_.replay = false;
            local.ready = false;
            local.recording = false;
            local.replay = false;
            stats_ = local;
        } catch (const std::exception& e) {
            Log::get().write(std::string("Pipeline error: ") + e.what());
            notify_(std::string("Pipeline stopped: ") + e.what(), true);
            recording = false;
            paused = false;
            replay = false;
            video.reset();
            audio.clear();
            mic.stop();
            desktop.stop();
            disk.close();
            ring.clear();
            capture.reset();
            suspended = false;
            texture.Reset();
            std::lock_guard lock(mutex_);
            gen = generation_;
            config_.replay = false;
            local.ready = false;
            local.recording = false;
            local.replay = false;
            stats_ = local;
        }
    }
    try {
        endPipeline();
        if (recording)
            disk.close();
    } catch (const std::exception& e) {
        notify_(e.what(), true);
    }
    texture.Reset();
    capture.reset();
    if (mm)
        AvRevertMmThreadCharacteristics(mm);
    winrt::uninit_apartment();
}
} // namespace nco
