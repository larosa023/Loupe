// Loupe - Direct2D / DirectWrite helpers and the theme (shared with the larosa023 suite)
#pragma once
#include "common.h"
#include <d2d1_1.h>
#include <dwrite.h>

namespace gfx {

extern ID2D1Factory1* d2d;
extern IDWriteFactory* dw;
bool Init();
const wchar_t* IconFont();      // Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets
const wchar_t* UiFont();        // Segoe UI Variable Text, else Segoe UI

// Cached text formats (never released; there are only a handful)
IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false, bool icon = false);

inline D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}
float Luma(D2D1_COLOR_F c);
inline D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
inline D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Theme {
    bool dark = true;
    D2D1_COLOR_F bg, sidebar, surface, surfaceHover, surfaceStrong, divider, text, subtext, faint;
    D2D1_COLOR_F accent, accentHover, onAccent, good, warn, bad, info;
    float radius = 8.0f;
    bool backdrop = false;          // a DWM backdrop shows through (Windows 11)
    int backdropType = 1;           // DWMSBT_*: 1 none, 2 Mica, 3 acrylic
    uint32_t captionRgb = 0x16161A; // title bar color when there's no backdrop
};
bool SystemUsesLightTheme();                  // cached; InvalidateSystemColors() on WM_SETTINGCHANGE
uint32_t SystemAccent();                      // 0xRRGGBB, cached
void InvalidateSystemColors();
bool BackdropSupported();
void ApplyWindowTheme(HWND hwnd, const Theme& t);

struct Canvas {
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;

    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c) { br->SetColor(c); rt->FillRectangle(r, br); }
    void FillRound(const D2D1_RECT_F& r, float rad, D2D1_COLOR_F c) {
        br->SetColor(c);
        if (rad <= 0.01f) rt->FillRectangle(r, br);
        else rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    }
    void StrokeRound(const D2D1_RECT_F& r, float rad, D2D1_COLOR_F c, float w = 1.0f) {
        br->SetColor(c);
        D2D1_RECT_F rr = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
        if (rad <= 0.01f) rt->DrawRectangle(rr, br, w);
        else rt->DrawRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), br, w);
    }
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w = 1.0f) {
        br->SetColor(c);
        rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), br, w);
    }
    void Circle(float cx, float cy, float r, D2D1_COLOR_F c) {
        br->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br);
    }
    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c);
    void Glyph(wchar_t g, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c) {
        wchar_t s[2] = {g, 0};
        Text(s, f, r, c);
    }
};

// Text layouts are cached between frames (creating one is the most expensive part of drawing text).
// Call FrameDone() once per frame; layouts unused for a few seconds are released.
IDWriteTextLayout* Layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h);
void FrameDone();
void ClearTextCaches();
float MeasureWidth(const std::wstring& s, IDWriteTextFormat* f);
float MeasureHeight(const std::wstring& s, IDWriteTextFormat* f, float width);

// Segoe Fluent Icons / MDL2 code points
namespace icon {
constexpr wchar_t Search = 0xE721, Grid = 0xF0E2, List = 0xE8FD, Refresh = 0xE72C, Back = 0xE72B, Forward = 0xE72A, Settings = 0xE713,
    Download = 0xE896, Play = 0xE768, Pause = 0xE769, Folder = 0xE8B7, FolderOpen = 0xE838, Delete = 0xE74D, Cancel = 0xE711,
    Update = 0xE895, Check = 0xE73E, Warning = 0xE7BA, Link = 0xE71B, Color = 0xE790, Info = 0xE946, Close = 0xE8BB,
    Globe = 0xE774, Star = 0xE734, ChevronLeft = 0xE76B, ChevronRight = 0xE76C, ChevronDown = 0xE70D, ChevronUp = 0xE70E,
    ZoomIn = 0xE8A3, ZoomOut = 0xE71F, FitPage = 0xE9A6, FullScreen = 0xE740, BackToWindow = 0xE73F, Rotate = 0xE7AD,
    Edit = 0xE70F, Crop = 0xE7A8, More = 0xE712, Copy = 0xE8C8, Save = 0xE74E, SaveAs = 0xE792, Undo = 0xE7A7, Redo = 0xE7A6,
    MapPin = 0xE707, Brightness = 0xE706, Slideshow = 0xE786, Sort = 0xE8CB, Rename = 0xE8AC, OpenFile = 0xE8E5,
    Personalize = 0xE771, Keyboard = 0xE765, Photo = 0xE91B, Camera = 0xE722, Add = 0xE710, Remove = 0xE738,
    Minimize = 0xE921, Maximize = 0xE922, Restore = 0xE923, Accept = 0xE8FB, Previous = 0xE892, Next = 0xE893,
    StepBack = 0xE892, StepForward = 0xE893, Shuffle = 0xE8B1, Contrast = 0xE793, OpenInNew = 0xE8A7;
}

} // namespace gfx
