#pragma once
#include "core.hpp"
#include <cmath>
#include <limits>

namespace nco {
struct AudioChunk {
    int64_t us = 0;
    std::vector<float> samples;
};
// QPC places the first packet; subsequent packets stay sample-contiguous across
// sub-millisecond endpoint timestamp jitter. Real gaps keep their wall-clock position.
class AudioTimeline {
    std::vector<float> data_ = std::vector<float>(48000 * 2 * 2);
    std::vector<int64_t> tags_ = std::vector<int64_t>(48000 * 2, -1);
    int64_t next_ = -1;
  public:
    void add(const AudioChunk& c, int64_t epoch) {
        int64_t start = (c.us - epoch) * 48000 / 1000000;
        if (next_ >= 0 && std::abs(start - next_) <= 96)
            start = next_;
        const auto frames = int64_t(c.samples.size() / 2);
        next_ = start + frames;
        for (int64_t i = 0; i < frames; ++i) {
            const int64_t sample = start + i;
            if (sample < 0)
                continue;
            const size_t slot = size_t(sample) % tags_.size();
            tags_[slot] = sample;
            data_[slot * 2] = c.samples[size_t(i) * 2];
            data_[slot * 2 + 1] = c.samples[size_t(i) * 2 + 1];
        }
    }
    void read(int64_t sample, int count, float gain, std::vector<float>& out) const {
        out.assign(size_t(count) * 2, 0);
        for (int i = 0; i < count; ++i) {
            const int64_t s = sample + i;
            if (s < 0)
                continue;
            const size_t slot = size_t(s) % tags_.size();
            if (tags_[slot] == s) {
                out[size_t(i) * 2] = data_[slot * 2] * gain;
                out[size_t(i) * 2 + 1] = data_[slot * 2 + 1] * gain;
            }
        }
    }
};
// Remove DC and very quiet endpoint noise, with hysteresis and smooth gain ramps.
// The threshold is below normal speech/music; stereo channels share one envelope.
class AudioConditioner {
    float previousIn_[2]{}, previousOut_[2]{}, gain_ = 0;
    bool open_ = false;
  public:
    void reset() { *this = AudioConditioner{}; }
    void process(std::vector<float>& samples) {
        if (samples.empty())
            return;
        double energy = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            const int ch = int(i % 2);
            float x = std::isfinite(samples[i]) ? samples[i] : 0.f;
            float y = x - previousIn_[ch] + .998692f * previousOut_[ch];
            previousIn_[ch] = x;
            previousOut_[ch] = y;
            samples[i] = y;
            energy += double(y) * y;
        }
        const float rms = float(std::sqrt(energy / samples.size()));
        if (rms > .0008f)
            open_ = true;
        else if (rms < .0004f)
            open_ = false;
        const float target = open_ ? 1.f : 0.f;
        const float step = open_ ? .00692f : .00052f;
        for (size_t i = 0; i + 1 < samples.size(); i += 2) {
            gain_ += (target - gain_) * step;
            if (!open_ && gain_ < .00001f)
                gain_ = 0;
            samples[i] *= gain_;
            samples[i + 1] *= gain_;
        }
    }
};
} // namespace nco
