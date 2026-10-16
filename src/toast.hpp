#pragma once
#include "platform.hpp"
#include <climits>
namespace nco {
// One premultiplied bitmap per notification; animation only changes DWM position/alpha.
class ToastSurface {
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    SIZE size_{};
    POINT lastPosition_{LONG_MIN, LONG_MIN};
    int lastAlpha_ = -1;
    void reset();
  public:
    ~ToastSurface() { reset(); }
    void prepare(const std::wstring& message, float dpi);
    void frame(HWND window, int x, int y, BYTE alpha);
};
}
