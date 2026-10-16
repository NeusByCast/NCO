// CI-only replacement for ui_test.cpp. Never linked into the installer build.
#include "ui.hpp"
#include <fstream>

namespace nco {
void UI::smokeControls() {
    cfg_.animations = false;
    page_ = 1;
    scroll_ = 0;
    popup_.open = false;
    engine_->visible(false);
    std::ofstream report("nco-self-test/ui-benchmark.txt");
    auto cpuUs = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            throw std::runtime_error("Cannot measure UI thread CPU time");
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        return (k.QuadPart + u.QuadPart) / 10;
    };
    auto measure = [&](const char* name) {
        for (int i = 0; i < 8; ++i) draw();
        std::vector<int64_t> frames;
        const auto cpu = cpuUs();
        const auto start = clockUs();
        for (int i = 0; i < 90; ++i) {
            const auto begin = clockUs();
            draw();
            frames.push_back(clockUs() - begin);
        }
        const auto wall = clockUs() - start;
        const auto used = cpuUs() - cpu;
        std::sort(frames.begin(), frames.end());
        report << name << " mean_ms=" << double(wall) / 90000
               << " p95_ms=" << double(frames[85]) / 1000
               << " cpu_ms=" << double(used) / 90000 << '\n';
    };
    measure("settings");
    pressed_ = L"benchmark";
    dropdown({500, 150, 850, 190}, {L"15 FPS", L"30 FPS", L"60 FPS"}, 2, [](int) {});
    measure("dropdown");
    popup_.open = popup_.closing = false;
}
}
