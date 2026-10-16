#pragma once
#include "platform.hpp"
#include <d2d1.h>
#include <dxgi.h>
namespace nco {
class PointerOverlay {
    Com<ID2D1Factory> factory_;
    Com<ID2D1RenderTarget> target_;
    Com<ID2D1Bitmap> bitmap_;
    HCURSOR cursor_ = nullptr;
    POINT hotspot_{};
  public:
    void draw(IDXGISurface* surface, const RECT& screenRegion, POINT position);
};
}
