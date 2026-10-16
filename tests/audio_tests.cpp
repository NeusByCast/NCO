#include "audio_core.hpp"
#include <iostream>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
int main() {
    using namespace nco;
    AudioTimeline timeline;
    for (int packet = 0; packet < 50; ++packet) {
        AudioChunk c;
        c.us = packet * 20000LL + (packet ? (packet % 2 ? 35 : -25) : 0);
        c.samples.resize(960 * 2);
        for (int i = 0; i < 960; ++i)
            c.samples[i * 2] = c.samples[i * 2 + 1] = float(.2 * std::sin((packet * 960 + i) * .03));
        timeline.add(c, 0);
    }
    std::vector<float> out;
    timeline.read(0, 48000, 1, out);
    for (int i = 0; i < 48000; ++i)
        CHECK(std::abs(out[i * 2] - float(.2 * std::sin(i * .03))) < 1e-6f);
    timeline.read(48000, 960, 1, out);
    CHECK(std::all_of(out.begin(), out.end(), [](float f) { return f == 0; }));
    AudioChunk gap{1500000, std::vector<float>(960 * 2, .1f)};
    timeline.add(gap, 0);
    timeline.read(72000, 960, 1, out);
    CHECK(std::abs(out[0] - .1f) < 1e-6f);
    timeline.read(0, 100, 0, out);
    CHECK(out[0] == 0);
    AudioConditioner filter;
    std::vector<float> hiss(2048);
    for (int i = 0; i < 2048; ++i)
        hiss[i] = float(3e-5 * std::sin(i * .08));
    filter.process(hiss);
    CHECK(std::all_of(hiss.begin(), hiss.end(), [](float f) { return f == 0; }));
    double inputEnergy = 0, outputEnergy = 0;
    for (int packet = 0; packet < 100; ++packet) {
        std::vector<float> tone(960 * 2);
        for (int i = 0; i < 960; ++i)
            tone[i * 2] = tone[i * 2 + 1] = float(.1 * std::sin((packet * 960 + i) * .13));
        if (packet > 10)
            for (auto x : tone) inputEnergy += double(x) * x;
        filter.process(tone);
        if (packet > 10)
            for (auto x : tone) outputEnergy += double(x) * x;
    }
    CHECK(outputEnergy / inputEnergy > .95 && outputEnergy / inputEnergy < 1.05);
    for (int i = 0; i < 100; ++i) {
        std::vector<float> quiet(2048, 0);
        filter.process(quiet);
        if (i == 99)
            CHECK(std::all_of(quiet.begin(), quiet.end(), [](float f) { return std::abs(f) < 1e-8f; }));
    }
    std::vector<float> invalid(2048, std::numeric_limits<float>::quiet_NaN());
    filter.process(invalid);
    CHECK(std::all_of(invalid.begin(), invalid.end(), [](float f) { return std::isfinite(f); }));
    std::cout << "Audio timeline continuity, real gaps, quiet-noise suppression and tone preservation passed\n";
}
