#include "core.hpp"
#include <iostream>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (0)
struct P {
    int64_t t;
    bool k;
    size_t b;
    int64_t us() const {
        return t;
    }
    bool key() const {
        return k;
    }
    size_t bytes() const {
        return b;
    }
};
int main() {
    using namespace nco;
    CHECK(resolution(0, 0, 0, {1921, 1081}).w == 1920);
    CHECK(resolution(5, 1001, 701, {}).h == 700);
    auto wide = letterbox({2560, 1080}, {1920, 1080});
    CHECK(wide.w == 1920 && wide.h == 810 && wide.y == 134);
    auto tall = letterbox({1080, 1920}, {1920, 1080});
    CHECK(tall.h == 1080 && tall.w == 606 && tall.x == 656);
    bool rejected = false;
    try {
        resolution(5, 10, 10, {});
    } catch (...) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(bitrate({1920, 1080}, 60, 4, false) > bitrate({1920, 1080}, 30, 2, false));
    CHECK(!containerCompatible("webm", "h264_nvenc"));
    CHECK(containerCompatible("webm", "av1_amf"));
    CHECK(duplicate({{1, 0}, {2, 0}}, 1, {1, 0}));
    CHECK(!duplicate({{1, 0}, {2, 0}}, 1, {0, 0}));
    ReplayRing<P> r;
    r.seconds = 15;
    r.cap = 1000;
    for (int i = 0; i < 40; ++i)
        r.push({i * 1000000LL, i % 2 == 0, 10});
    auto s = r.snapshot();
    CHECK(!s.empty() && s.front().key());
    CHECK(s.front().t >= 22000000 && s.front().t <= 24000000);
    CHECK(s.back().t - s.front().t >= 15000000);
    r.cap = 5;
    r.push({40000000, true, 20});
    CHECK(r.bytes() == 0);
    r.cap = 100;
    r.push({0, false, 5});
    CHECK(r.snapshot().empty());
    std::cout << "NCO core tests passed\n";
}
