#include "pointer.hpp"
#include <dxgi.h>
namespace nco {
void PointerOverlay::draw(IDXGISurface* surface, const RECT& screenRegion, POINT position, float scale) {
    CURSORINFO info{sizeof(info)};
    if (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING))
        return;
    if (!factory_)
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()),
              "Cursor rendering factory");
    if (!target_)
        check(factory_->CreateDxgiSurfaceRenderTarget(
                  surface, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96),
                  &target_), "Cursor GPU surface");
    if (cursor_ != info.hCursor || !bitmap_) {
        ICONINFO icon{};
        if (!GetIconInfo(info.hCursor, &icon))
            return;
        hotspot_ = {LONG(icon.xHotspot), LONG(icon.yHotspot)};
        BITMAP dimensions{};
        GetObjectW(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(dimensions), &dimensions);
        int width = dimensions.bmWidth;
        int height = icon.hbmColor ? dimensions.bmHeight : dimensions.bmHeight / 2;
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
        if (width <= 0 || height <= 0 || width > 256 || height > 256)
            return;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HDC dc = CreateCompatibleDC(nullptr);
        void* pixels = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc || !dib || !pixels) {
            if (dib) DeleteObject(dib);
            if (dc) DeleteDC(dc);
            return;
        }
        auto previous = SelectObject(dc, dib);
        const size_t bytes = size_t(width) * height * 4;
        memset(pixels, 0, bytes);
        DrawIconEx(dc, 0, 0, info.hCursor, width, height, 0, nullptr, DI_NORMAL);
        GdiFlush();
        std::vector<uint8_t> black(bytes);
        memcpy(black.data(), pixels, bytes);
        memset(pixels, 255, bytes);
        DrawIconEx(dc, 0, 0, info.hCursor, width, height, 0, nullptr, DI_NORMAL);
        GdiFlush();
        auto white = static_cast<const uint8_t*>(pixels);
        for (size_t i = 0; i < bytes; i += 4) {
            int difference = std::max({std::abs(int(white[i]) - black[i]),
                                       std::abs(int(white[i + 1]) - black[i + 1]),
                                       std::abs(int(white[i + 2]) - black[i + 2])});
            black[i + 3] = uint8_t(255 - difference);
        }
        SelectObject(dc, previous);
        DeleteObject(dib);
        DeleteDC(dc);
        bitmap_.Reset();
        check(target_->CreateBitmap(D2D1::SizeU(width, height), black.data(), width * 4,
                  D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                          D2D1_ALPHA_MODE_PREMULTIPLIED)),
                  &bitmap_), "Cursor bitmap");
        cursor_ = info.hCursor;
    }
    // DXGI position is already the shape's top-left, not the hotspot.
    const float x = float(position.x - screenRegion.left);
    const float y = float(position.y - screenRegion.top);
    auto size = bitmap_->GetSize();
    target_->BeginDraw();
    target_->DrawBitmap(bitmap_.Get(), {x, y, x + size.width * scale, y + size.height * scale});
    check(target_->EndDraw(), "Composite cursor");
}
}
