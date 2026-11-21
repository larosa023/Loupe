// Loupe - photo metadata (EXIF) and RAW preview discovery.
//
// One small TIFF/EXIF reader covers every format Loupe opens: JPEG (APP1), TIFF and TIFF-based RAW
// (CR2, NEF, ARW, DNG), Canon CR3 (ISO-BMFF CMT boxes), PNG (eXIf), WebP (EXIF chunk) and HEIF/AVIF
// (the Exif item). It reads only the few kilobytes it needs, so sorting a folder by date taken is fast.
#pragma once
#include "common.h"

struct PhotoInfo {
    int orientation = 1;                 // EXIF orientation 1-8
    std::wstring make, model, lens, software;
    bool hasTaken = false;
    SYSTEMTIME taken{};                  // local time as recorded by the camera
    double exposureTime = 0;             // seconds
    double fNumber = 0;
    double focal = 0;                    // mm
    int focal35 = 0;
    int iso = 0;
    double exposureBias = 0;
    int flash = -1;
    bool hasGps = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    double alt = 0;
    UINT pixelW = 0, pixelH = 0;
};

// quick=true stops after the date taken and orientation (for sorting)
bool ReadPhotoInfo(const std::wstring& path, PhotoInfo& out, bool quick = false);

// Embedded JPEG previews inside RAW files. Picks the largest one.
struct RawPreview {
    uint64_t offset = 0, length = 0;
    int orientation = 1;
};
bool FindRawPreview(const std::wstring& path, RawPreview& out);

// Lets the file reader be shared by the decoder (reads a byte range of a file)
bool ReadFileRange(const std::wstring& path, uint64_t offset, uint64_t length, std::string& out);

bool IsRawExtension(const std::wstring& ext);   // ".cr2" etc. (lowercase, with the dot)
std::wstring FormatExposure(double seconds);    // "1/250 s", "2 s"
std::wstring FormatTaken(const SYSTEMTIME& st); // "Saturday, November 15, 2026 at 4:12 PM"
