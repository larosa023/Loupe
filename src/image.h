// Loupe - decoding every supported format into premultiplied BGRA.
//
//   JPEG, PNG, BMP, TIFF, ICO, HEIC/HEIF, AVIF   Windows Imaging Component (HEIF/AVIF use the Store codecs)
//   GIF, APNG                                    WIC frames, composited here (disposal, blending, timing)
//   WebP (still + animated)                      libwebp, bundled
//   SVG                                          Direct2D's SVG renderer (offscreen device)
//   CR2, CR3, NEF, ARW, DNG                      the embedded JPEG preview (full size where the camera stores one)
//
// Decoding can be asked for a maximum size: JPEGs are then scaled inside the decoder (1/2, 1/4, 1/8),
// which is what makes opening and flipping through large photos fast.
#pragma once
#include "common.h"
#include "offscreen.h"
#include <wincodec.h>

struct Frame {
    Pixels pix;
    UINT delayMs = 0;
};

struct ImageData {
    std::wstring format;          // "JPEG", "PNG", "Canon RAW (CR3)"…
    UINT fullW = 0, fullH = 0;    // the image's real size, after EXIF orientation
    bool hasAlpha = false;
    bool animated = false;
    bool fullRes = true;          // frames are at full resolution (not scaled down)
    bool vector = false;          // SVG: can be rasterized sharper on demand
    int loops = 0;                // animation: 0 = forever
    std::vector<Frame> frames;
    std::wstring error;           // user-facing reason when decoding failed
    std::wstring codecName, codecStoreId;   // a Microsoft Store codec that would open it
};

IWICImagingFactory* Wic();        // free-threaded; each thread still needs COM initialized

const std::vector<std::wstring>& SupportedExtensions();   // ".jpg"… lowercase
bool IsSupportedExtension(const std::wstring& ext);
std::wstring ExtOf(const std::wstring& path);              // lowercase, with the dot

// maxW/maxH = 0: full size. Never enlarges (except SVG, which is drawn at the asked size).
bool DecodeImage(const std::wstring& path, UINT maxW, UINT maxH, bool animation, ImageData& out);

// Downscale with WIC (Fant)
bool ScalePixels(const Pixels& in, UINT w, UINT h, Pixels& out);
// EXIF orientation 1-8 applied to pixels
void OrientPixels(Pixels& p, int orientation);

// Encoding (Save / Save as copy / clipboard / wallpaper). format: "jpg", "png", "bmp", "tif", "gif", "webp"→png
bool EncodeImage(const Pixels& p, const std::wstring& path, const std::wstring& format, int jpegQuality,
                 const std::wstring& metadataFrom, std::wstring* error);
bool EncodePngToMemory(const Pixels& p, std::string& out);
bool CanEncodeExtension(const std::wstring& ext);
