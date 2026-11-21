// Loupe - an offscreen Direct2D device for worker threads: rasterizing SVG and rendering edits at
// full resolution for saving. Separate from the window's render target so it never blocks drawing.
#pragma once
#include "common.h"
#include <d2d1_3.h>
#include <mutex>

struct Pixels {
    UINT w = 0, h = 0;
    std::vector<uint8_t> px;     // BGRA, premultiplied alpha, stride w * 4
    bool empty() const { return w == 0 || h == 0 || px.empty(); }
};

namespace offscreen {

// The device is shared; hold the lock while using it.
struct Device {
    ID2D1Factory1* factory = nullptr;
    ID2D1Device* device = nullptr;
    ID2D1DeviceContext* dc = nullptr;
    UINT maxSize = 4096;
};
std::unique_lock<std::mutex> Lock();
Device* Get();               // nullptr if no D3D device could be made (call with the lock held)
void Reset();                // after a device-lost error

// Reads back a target bitmap into pixels
bool ReadBack(Device* d, ID2D1Bitmap1* target, Pixels& out);

// SVG: natural size (from width/height or viewBox), and rasterizing at a size
bool SvgSize(const std::string& data, float& w, float& h);
bool RasterSvg(const std::string& data, UINT w, UINT h, Pixels& out);

} // namespace offscreen
