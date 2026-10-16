#include "engine.hpp"
#include "video_pipeline.hpp"
#include <avrt.h>
#include <cmath>
#include <psapi.h>
#include <utility>
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
        Log::get().write("Shutdown requested; waiting for recording pipeline finalization");
        thread_.request_stop();
        SetEvent(wake_);
        thread_.join();
        Log::get().write("Recording pipeline thread stopped");
    }
}
void Engine::configure(const Config& cfg, const Source& s) {
    std::lock_guard lock(mutex_);
    const bool sourceChanged = source_.monitor != s.monitor || source_.window != s.window ||
        source_.custom != s.custom || memcmp(&source_.region, &s.region, sizeof(RECT)) != 0;
    if (sourceChanged) {
        preview_.pixels.clear();
        preview_.size = {};
        ++preview_.serial;
        stats_.ready = false;
    }
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
    if (seen == preview_.serial) return {};
    Preview result{preview_.size, {}, preview_.serial};
    result.pixels.swap(preview_.pixels);
    return result;
}
bool Engine::previewChanged(uint64_t seen) {
    std::lock_guard lock(mutex_);
    return seen != preview_.serial;
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
    std::unique_ptr<VideoPipeline> video;
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
    uint64_t lastCpu = 0, usageFrames = 0, videoBytes = 0, usageBytes = 0, previousReadbackDrops = 0;
    double quantizerSum = 0, quantizerMax = -1;
    uint64_t quantizerCount = 0;
    bool recording = false, paused = false, gate = false, forceKey = false, replay = false;
    bool suspended = false, stoppingRecording = false, drainingVideo = false, previewDirty = true;
    int64_t stopCutoff = 0, stopRequested = 0, drainRequested = 0, latestVideoUs = -1;
    int64_t keyframeWaitStart = 0, lastKeyframeRequest = 0;
    EngineStats local;
    std::filesystem::path recordingPath;
    bool recordingOutputOpen = false;
    auto closeRecordingOutput = [&] {
        if (recordingOutputOpen) {
            disk.close();
            recordingOutputOpen = false;
        }
    };
    Com<ID3D11Texture2D> texture;
    std::vector<Packet> recordingPreroll;
    std::vector<float> micSamples, desktopSamples;
    std::function<void(std::vector<Packet>)> packets;
    auto endPipeline = [&](bool finalizeRecording = false) {
        if (video) {
            Log::get().write("Finalizing video encoder and draining queued frames");
            packets(video->flush());
            for (auto& a : audio) packets(a->flush());
            auto finalMetrics = video->metrics();
            local.frames = finalMetrics.frames;
            local.dropped += finalMetrics.readbackDropped - previousReadbackDrops;
            previousReadbackDrops = finalMetrics.readbackDropped;
        }
        if (video)
            Log::get().write("Pipeline finalized frames=" + std::to_string(local.frames) +
                             " dropped=" + std::to_string(local.dropped));
        if (finalizeRecording) {
            // Hardware teardown can be slow even after all packets drained.
            // Finalize the container and clear the stop state before releasing
            // driver objects so the recording file is not held open by cleanup.
            closeRecordingOutput();
            recording = paused = stoppingRecording = false;
            local.ready = local.recording = local.paused = local.finishing = local.starting = false;
            local.durationUs = 0;
            std::lock_guard lock(mutex_);
            stats_ = local;
        }
        video.reset();
        drainingVideo = false;
        audio.clear();
        mic.stop();
        desktop.stop();
        tracks.clear();
        if (finalizeRecording) local.ready = bool(capture);
    };
    auto configureAudio = [&](const Config& next, bool force) {
        if (force || next.mic != cfg.mic || next.micDevice != cfg.micDevice) {
            if (next.mic)
                mic.start(true, next.micDevice, [this](std::string e) { notify_(std::move(e), true); });
            else mic.stop();
            mt = AudioTimeline();
            micFilter.reset();
        }
        if (force || next.desktop != cfg.desktop || next.desktopDevice != cfg.desktopDevice) {
            if (next.desktop)
                desktop.start(false, next.desktopDevice, [this](std::string e) { notify_(std::move(e), true); });
            else desktop.stop();
            dt = AudioTimeline();
            desktopFilter.reset();
        }
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
        video = std::make_unique<VideoPipeline>(cfg, capture.get(), capture->size());
        if (shouldRetryCaptureOnEncoderGpu(cfg.captureBackend, capture->fallback(),
                                          video->gpu(), video->selected())) {
            // Startup only: keep the exact codec and image settings while
            // evaluating a capture device that can feed it GPU textures.
            Config sameEncoder = cfg;
            sameEncoder.encoder = video->selected();
            const auto originalSize = capture->size();
            auto originalCapture = std::move(capture);
            // Release the unused encoder before opening another hardware session.
            video.reset();
            Log::get().write("Auto capture: direct encoding unavailable on display GPU; trying encoder-local capture");
            try {
                auto candidateCapture = std::make_unique<Capture>();
                candidateCapture->open(source, 1);
                const auto candidateSize = candidateCapture->size();
                if (candidateSize.w != originalSize.w || candidateSize.h != originalSize.h)
                    throw std::runtime_error("Encoder-local capture changed source dimensions");
                auto candidateVideo = std::make_unique<VideoPipeline>(sameEncoder, candidateCapture.get(), candidateSize);
                if (!candidateVideo->gpu())
                    throw std::runtime_error("Encoder-local capture still requires frame upload");
                capture = std::move(candidateCapture);
                video = std::move(candidateVideo);
                texture.Reset();
                previewDirty = true;
                Log::get().write("Auto capture: encoder-local GPU texture path selected; CPU readback/upload avoided");
            } catch (const winrt::hresult_error& e) {
                Log::get().write("Encoder-local capture unavailable: " + utf8(e.message().c_str()));
            } catch (const std::exception& e) {
                Log::get().write(std::string("Encoder-local capture unavailable: ") + e.what());
            }
            if (!video) {
                capture = std::move(originalCapture);
                video = std::make_unique<VideoPipeline>(sameEncoder, capture.get(), originalSize);
                Log::get().write("Auto capture: retained compatible display capture with frame upload");
            }
        }
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
        ring.clear();
        configureAudio(cfg, true);
        local.encoder = video->selected();
        local.encoderAdapter = video->adapter();
        local.gpuTexturePath = video->gpu();
        local.gpu = video->selected().find("lib") != 0;
        local.frames = local.encodedFrames = local.dropped = local.freshFrames = 0;
        quantizerSum = quantizerCount = 0;
        quantizerMax = local.quantizerAvg = local.quantizerMax = -1;
        local.encodeMs = local.captureMs = local.prepareMs = local.submitMs = local.actualFps = local.bitrateMbps = 0;
        local.output = resolution(cfg.resolutionMode, cfg.width, cfg.height, capture->size());
        usageFrames = videoBytes = usageBytes = previousReadbackDrops = 0;
        latestVideoUs = -1;
        lastUsage = 0;
        Log::get().write("Pipeline started replay=" + std::to_string(cfg.replay) +
                         " audio_tracks=" + std::to_string(count) +
                         " auto_remux=" + std::to_string(cfg.remux) +
                         " capture=" + std::to_string(capture->size().w) + "x" +
                         std::to_string(capture->size().h));
        if (cfg.remux && cfg.format == "mkv")
            Log::get().write("Auto-remux not needed: selected target is already MKV");
        else if (cfg.remux)
            Log::get().write("Auto-remux enabled: will verify the recording, then remux without re-encoding");
    };
    packets = [&](std::vector<Packet> ps) {
        for (auto& p : ps) {
            if (p.stream == 0) {
                videoBytes += p.bytes();
                ++local.encodedFrames;
                const double qp = p.quantizer();
                if (qp >= 0) {
                    quantizerSum += qp;
                    ++quantizerCount;
                    quantizerMax = std::max(quantizerMax, qp);
                }
                latestVideoUs = std::max(latestVideoUs, p.us());
            }
            if (replay)
                ring.push(p);
            if (recording && !paused && (!stoppingRecording || p.us() <= stopCutoff)) {
                if (gate) {
                    if (!p.key()) {
                        if (p.stream != 0) {
                            // Keep bounded audio while an asynchronous first/resume IDR is
                            // pending. The old hard packet limit threw out of the whole
                            // capture loop after about five seconds on some AMF drivers.
                            while (!recordingPreroll.empty() &&
                                   p.us() - recordingPreroll.front().us() > 3000000)
                                recordingPreroll.erase(recordingPreroll.begin());
                            if (recordingPreroll.size() >= 192)
                                recordingPreroll.erase(recordingPreroll.begin());
                            recordingPreroll.push_back(p);
                        }
                        continue;
                    }
                    Log::get().write("Recording keyframe acquired after_ms=" +
                                     std::to_string((clockUs() - keyframeWaitStart) / 1000) +
                                     " encoder=" + local.encoder);
                    gate = false;
                    if (!recordingOutputOpen) {
                        disk.open(recordingPath, tracks, cfg.remux, cfg.format);
                        recordingOutputOpen = true;
                        recordStart = epoch + p.us() - removed;
                    }
                    disk.write(p, removed);
                    for (auto& audioPacket : recordingPreroll)
                        if (audioPacket.us() >= p.us() && (!stoppingRecording || audioPacket.us() <= stopCutoff))
                            disk.write(audioPacket, removed);
                    recordingPreroll.clear();
                    continue;
                }
                disk.write(p, removed);
            }
        }
    };
    auto failPipeline = [&](const std::string& error) {
        Log::get().write("Pipeline error: " + error);
        // Keep packets completed before a later encoder failure, then close the
        // file before releasing the encoder/driver resources.
        try {
            if (video) packets(video->collect(true));
        } catch (const std::exception&) {}
        recording = paused = replay = stoppingRecording = drainingVideo = false;
        local.ready = local.recording = local.paused = local.replay = local.finishing = local.starting = false;
        local.durationUs = 0;
        {
            std::lock_guard lock(mutex_);
            gen = generation_;
            config_.replay = false;
            stats_ = local;
        }
        closeRecordingOutput();
        notify_("Pipeline stopped: " + error + "; recording file retained", true);
        video.reset();
        audio.clear();
        mic.stop();
        desktop.stop();
        ring.clear();
        capture.reset();
        suspended = false;
        texture.Reset();
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
                    current.rateControl != cfg.rateControl ||
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
                    current.rateControl = cfg.rateControl;
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
                    previewDirty = true;
                    source = src;
                    local.ready = true;
                }
                if (video) configureAudio(current, false);
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
                        if (stoppingRecording) continue;
                        stoppingRecording = true;
                        stopRequested = clockUs();
                        stopCutoff = stopRequested - epoch;
                        Log::get().write("Recording finalization fence_us=" + std::to_string(stopCutoff));
                    } else {
                        stoppingRecording = false;
                        beginPipeline();
                        recordingPath = uniquePath(cfg.directory, "NCO_", cfg.format);
                        recording = true;
                        paused = false;
                        gate = true;
                        recordingPreroll.clear();
                        forceKey = true;
                        keyframeWaitStart = lastKeyframeRequest = clockUs();
                        removed = 0;
                        recordStart = clockUs();
                    }
                } else if (action == Pause && recording && !stoppingRecording) {
                    paused = !paused;
                    if (paused)
                        pauseStart = clockUs();
                    else {
                        removed += clockUs() - pauseStart;
                        gate = true;
                        recordingPreroll.clear();
                        forceKey = true;
                        keyframeWaitStart = lastKeyframeRequest = clockUs();
                    }
                } else if (action == AudioRefresh && video) {
                    configureAudio(cfg, true);
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
                closeRecordingOutput();
                Log::get().write("Recording stopped after disk writer failure");
                if (!replay)
                    endPipeline();
            }
            int64_t now = clockUs();
            if (recording && !paused && !stoppingRecording && gate && now - lastKeyframeRequest >= 500000) {
                // Some hardware encoders can miss a forced-frame request while
                // their asynchronous queue is starting. Retry at a low rate.
                forceKey = true;
                lastKeyframeRequest = now;
            }
            if (recording && !paused && !stoppingRecording && gate && keyframeWaitStart &&
                now - keyframeWaitStart >= 10000000) {
                Log::get().write("Recording aborted: encoder produced no decodable keyframe after repeated IDR requests");
                recording = paused = gate = false;
                stoppingRecording = false;
                recordingPreroll.clear();
                closeRecordingOutput();
                Log::get().write("Recording output closed after keyframe wait failure");
                if (!replay)
                    endPipeline();
                local.recording = local.paused = local.finishing = local.starting = false;
                local.durationUs = 0;
                notify_("Recording failed: encoder did not produce a keyframe. The capture pipeline remains available.", true);
            }
            const bool wasVisible = visible_.load();
            const bool previewVisible = cfg.preview && wasVisible;
            if (!video && !previewVisible && capture) {
                texture.Reset();
                capture.reset();
                suspended = true;
            } else if (suspended && previewVisible && !capture) {
                capture = std::make_unique<Capture>();
                capture->open(source, cfg.captureBackend);
                previewDirty = true;
                suspended = false;
            }
            if (capture && source.window && !IsWindow(source.window))
                throw std::runtime_error("The selected window closed. Select another source.");
            const int64_t previewInterval = video &&
                (local.encodeMs > 800.0 / cfg.fps || (local.actualFps > 0 && local.actualFps < cfg.fps * .9))
                ? 100000 : 33333;
            const bool previewDue = previewVisible && !recording && now - lastPreview >= previewInterval;
            // Audio and packet collection can wake more frequently than the
            // requested video rate. Acquire/copy a source frame only when due.
            if (capture && !drainingVideo && ((video && now >= nextVideo) || previewDue)) {
                int64_t captureBegin = clockUs();
                bool fresh = capture->frame(texture);
                local.captureMs = local.captureMs * .9 + double(clockUs() - captureBegin) / 1000 * .1;
                if (fresh) ++local.freshFrames;
                previewDirty = previewDirty || fresh;
                now = clockUs();
                if (video && texture && now >= nextVideo) {
                    int64_t frameIndex = (now - epoch) * cfg.fps / 1000000;
                    if (frameIndex > nextFrame)
                        local.dropped += uint64_t(frameIndex - nextFrame);
                    nextFrame = frameIndex + 1;
                    // Keep replay independently decodable with a one-second GOP.
                    // Some hardware encoders otherwise ignore the configured GOP
                    // interval when replay is the only consumer of the packets.
                    const bool replayKey = replay && frameIndex % cfg.fps == 0;
                    if (video->submit(texture.Get(), frameIndex, forceKey || replayKey))
                        forceKey = false;
                    else
                        ++local.dropped;
                    nextVideo = frameDeadlineUs(epoch, frameIndex + 1, cfg.fps);
                }
                if (previewDue && texture) {
                    try {
                        auto p = capture->thumbnail(texture.Get(), std::exchange(previewDirty, false));
                        if (!p.second.empty()) {
                        std::lock_guard lock(mutex_);
                        if (gen == generation_) {
                            preview_.pixels = std::move(p.second);
                            preview_.size = p.first;
                            ++preview_.serial;
                        }
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
                packets(video->collect());
                auto metrics = video->metrics();
                local.frames = metrics.frames;
                local.dropped += metrics.readbackDropped - previousReadbackDrops;
                previousReadbackDrops = metrics.readbackDropped;
                local.prepareMs = metrics.prepareMs;
                local.submitMs = metrics.submitMs;
                local.encodeMs = metrics.encodeMs;
                local.encodeQueue = metrics.queued;
                local.readbackQueue = metrics.readbackPending;
                local.readbackDropped = metrics.readbackDropped;
                for (auto& chunk : mic.drain())
                    mt.add(chunk, epoch);
                for (auto& chunk : desktop.drain())
                    dt.add(chunk, epoch);
                // Keep 60 ms of capture headroom to absorb endpoint scheduling jitter.
                int64_t due = std::max<int64_t>(0, (clockUs() - epoch - 60000) * 48000 / 1000000);
                int frame = audio[0]->frameSize();
                int iterations = 0;
                while (audioPts + frame <= due && iterations++ < 12) {
                    auto& m = micSamples;
                    auto& d = desktopSamples;
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
                        for (size_t i = 0; i < m.size(); ++i) {
                            const float sum = m[i] + d[i];
                            m[i] = std::abs(sum) <= .9f ? sum :
                                std::copysign(.9f + .1f * std::tanh((std::abs(sum) - .9f) * 10), sum);
                        }
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
                    local.actualFps = double(local.encodedFrames - usageFrames) * 1000000 / (now - lastUsage);
                    local.bitrateMbps = double(videoBytes - usageBytes) * 8 / (now - lastUsage);
                }
                local.quantizerAvg = quantizerCount ? quantizerSum / quantizerCount : -1;
                local.quantizerMax = quantizerMax;
                quantizerSum = quantizerCount = 0;
                quantizerMax = -1;
                usageFrames = local.encodedFrames;
                usageBytes = videoBytes;
                lastUsage = now;
                if (video)
                    Log::get().write("Frames=" + std::to_string(local.frames) +
                                         " encoded_frames=" + std::to_string(local.encodedFrames) +
                                         " qp_avg=" + std::to_string(local.quantizerAvg) +
                                         " qp_max=" + std::to_string(local.quantizerMax) +
                                         " dropped=" + std::to_string(local.dropped) +
                                         " fps=" + std::to_string(local.actualFps) +
                                         " video_Mbps=" + std::to_string(local.bitrateMbps) +
                                         " output=" + std::to_string(local.output.w) + "x" + std::to_string(local.output.h) +
                                         " prepare_ms=" + std::to_string(local.prepareMs) +
                                         " submit_ms=" + std::to_string(local.submitMs) +
                                         " encode_ms=" + std::to_string(local.encodeMs) +
                                         " encode_queue=" + std::to_string(local.encodeQueue) +
                                         " readback_queue=" + std::to_string(local.readbackQueue) +
                                         " readback_dropped=" + std::to_string(local.readbackDropped) +
                                         " capture_ms=" + std::to_string(local.captureMs) +
                                         " fresh_frames=" + std::to_string(local.freshFrames) +
                                         " replay_MiB=" + std::to_string(ring.bytes() / 1048576.0) +
                                         " replay_seconds=" + std::to_string(ring.durationUs() / 1000000.0) +
                                         " replay_keyframes=" + std::to_string(ring.keyframes()) +
                                         " cpu_percent=" + std::to_string(local.cpuPercent) +
                                         " working_set_MiB=" + std::to_string(local.workingSetMiB) +
                                         " mic_discontinuities=" + std::to_string(mic.discontinuities.load()) +
                                         " desktop_discontinuities=" + std::to_string(desktop.discontinuities.load()),
                                     false);
            }
            if (stoppingRecording && recording) {
                const bool fenceReached =
                    (latestVideoUs >= stopCutoff &&
                     audioPts * 1000000 / 48000 + int64_t(cfg.audioOffsetMs) * 1000 >= stopCutoff) ||
                    clockUs() - stopRequested >= 2000000;
                if (!replay && fenceReached && !drainingVideo) {
                    Log::get().write("Recording fence reached; requesting asynchronous encoder drain");
                    video->finish();
                    drainingVideo = true;
                    drainRequested = clockUs();
                }
                if (drainingVideo && !video->finished() && clockUs() - drainRequested >= 12000000)
                    throw std::runtime_error("Encoder shutdown timed out. Completed packets were saved; restart NCO if the graphics driver remains unresponsive");
                if ((replay && fenceReached) || (drainingVideo && video->finished())) {
                    if (!replay) endPipeline(true);
                    else closeRecordingOutput();
                    recording = paused = stoppingRecording = false;
                    local.durationUs = 0;
                    gen = 0;
                }
            }
            local.finishing = stoppingRecording && recording;
            local.starting = recording && !recordingOutputOpen;
            local.recording = recording;
            local.paused = paused;
            local.replay = replay;
            local.replayBytes = ring.bytes();
            if (recording && recordingOutputOpen)
                local.durationUs = (paused ? pauseStart : stoppingRecording ? stopRequested : clockUs()) - recordStart - removed;
            else local.durationUs = 0;
            {
                std::lock_guard lock(mutex_);
                stats_ = local;
            }
            int64_t waitUs = drainingVideo ? 10000 :
                video ? (texture ? std::clamp(nextVideo - clockUs(), int64_t(1000), int64_t(10000))
                                 : 1000000 / cfg.fps)
                      : 33333;
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
            failPipeline(utf8(e.message().c_str()));
        } catch (const std::exception& e) {
            failPipeline(e.what());
        }
    }
    try {
        endPipeline(recording);
        if (recording)
            closeRecordingOutput();
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
