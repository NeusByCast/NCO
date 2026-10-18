#include "toast.hpp"
#include <cmath>
#include <climits>
namespace nco {
void ToastSurface::reset() {
    if (dc_ && previous_) SelectObject(dc_, previous_);
    if (bitmap_) DeleteObject(bitmap_);
    if (dc_) DeleteDC(dc_);
    bitmap_ = nullptr;
    dc_ = nullptr;
    previous_ = nullptr;
}
void ToastSurface::prepare(const std::wstring& message, float dpi,
                           uint32_t background, uint32_t foreground) {
    reset();
    size_ = {int(280 * dpi / 96), int(48 * dpi / 96)};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size_.cx;
    info.bmiHeader.biHeight = -size_.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* raw = nullptr;
    dc_ = CreateCompatibleDC(nullptr);
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!dc_ || !bitmap_ || !raw) throw std::runtime_error("Cannot render notification");
    previous_ = SelectObject(dc_, bitmap_);
    auto pixels = static_cast<uint32_t*>(raw);
    std::fill_n(pixels, size_t(size_.cx) * size_.cy, background & 0x00ffffffu);
    SetBkMode(dc_, TRANSPARENT);
    SetTextColor(dc_, RGB((foreground >> 16) & 255, (foreground >> 8) & 255, foreground & 255));
    HFONT font = CreateFontW(-int(15 * dpi / 96), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto oldFont = SelectObject(dc_, font);
    RECT bounds{0, 0, size_.cx, size_.cy};
    DrawTextW(dc_, message.c_str(), -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc_, oldFont);
    DeleteObject(font);
    GdiFlush();
    const float radius = 10 * dpi / 96;
    for (LONG y = 0; y < size_.cy; ++y)
        for (LONG x = 0; x < size_.cx; ++x) {
            const float dx = std::max({radius - (x + .5f), (x + .5f) - (size_.cx - radius), 0.f});
            const float dy = std::max({radius - (y + .5f), (y + .5f) - (size_.cy - radius), 0.f});
            const float coverage = std::clamp(radius + .5f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
            auto& pixel = pixels[size_t(y) * size_.cx + x];
            const UINT a = UINT(coverage * 255);
            const UINT b = (pixel & 255) * a / 255;
            const UINT g = ((pixel >> 8) & 255) * a / 255;
            const UINT r = ((pixel >> 16) & 255) * a / 255;
            pixel = (a << 24) | (r << 16) | (g << 8) | b;
        }
    lastAlpha_ = -1;
    lastPosition_ = {LONG_MIN, LONG_MIN};
}
void ToastSurface::frame(HWND window, int x, int y, BYTE alpha) {
    if (!dc_ || (lastAlpha_ == alpha && lastPosition_.x == x && lastPosition_.y == y)) return;
    POINT position{x, y}, source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(window, nullptr, &position, &size_, dc_, &source, 0, &blend, ULW_ALPHA))
        throw std::runtime_error("Notification composition failed");
    lastAlpha_ = alpha;
    lastPosition_ = position;
}
}
