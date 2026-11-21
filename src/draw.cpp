// Loupe - drawing: the image view, chrome (top bar, arrows, filmstrip), grid, compare, slideshow
// controls, toasts, dialogs, tooltips and the small widgets everything shares.
#include "app.h"
#include "util.h"
#include "image.h"
#include <algorithm>
#include <cmath>

using namespace app;
using namespace gfx;

// from ui.cpp
float CompareMul();
float CompareU();
float CompareV();
float SlideFade();
uint64_t SlideFadeFrom();
bool SlidePaused();
bool MaxButtonHot();
bool MaxButtonDown();
void UiDeviceLost();
int DeviceGeneration();

static uint64_t s_lastShownId = 0;      // the last image drawn, shown while the next one decodes
static DWORD s_loadingSince = 0;
static std::wstring s_loadingPath;
static int s_filmLastIndex = -2;
static HotRect s_tipFor{{0, 0, 0, 0}, Hit::None, 0, true, nullptr};
static DWORD s_tipSince = 0;
static ID2D1BitmapBrush* s_checker = nullptr;
static int s_checkerGen = -1;
static ID2D1Layer* s_layer = nullptr;
static int s_layerGen = -1;

namespace app {

// ---------------------------------------------------------------- hit helpers
void AddHot(const D2D1_RECT_F& r, Hit kind, int arg, bool enabled, const wchar_t* tip) {
    hot.push_back({r, kind, arg, enabled, tip});
}
bool IsHover(Hit kind, int arg) { return hover.kind == kind && hover.arg == arg; }
bool IsPressed(Hit kind, int arg) { return pressed.kind == kind && pressed.arg == arg && IsHover(kind, arg); }

// ---------------------------------------------------------------- widgets
float ButtonWidth(const std::wstring& label, wchar_t glyph) {
    float w = MeasureWidth(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28;
    if (glyph) w += label.empty() ? 6 : 22;
    return std::max(w, label.empty() ? 34.0f : 76.0f);
}

void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph, Btn style, Hit kind, int arg, bool enabled, const wchar_t* tip) {
    bool hotB = enabled && IsHover(kind, arg);
    bool down = enabled && IsPressed(kind, arg);
    D2D1_COLOR_F bg, fg;
    switch (style) {
        case Btn::Primary: bg = hotB ? t.accentHover : t.accent; fg = t.onAccent; break;
        case Btn::Danger: bg = hotB ? Alpha(t.bad, 0.22f) : Alpha(t.bad, 0.12f); fg = t.bad; break;
        case Btn::Subtle: bg = hotB ? t.surfaceHover : Alpha(t.surface, 0.0f); fg = t.text; break;
        case Btn::Chrome: bg = hotB ? Alpha(t.text, 0.10f) : Alpha(t.text, 0.0f); fg = t.text; break;
        default: bg = hotB ? t.surfaceStrong : Mix(t.surface, t.surfaceStrong, 0.55f); fg = t.text; break;
    }
    if (!enabled) { bg = style == Btn::Chrome || style == Btn::Subtle ? Alpha(t.text, 0.0f) : Alpha(t.surfaceStrong, 0.5f); fg = t.faint; }
    if (down) bg = Mix(bg, t.dark ? Rgb(0x000000) : Rgb(0xFFFFFF), 0.15f);
    c.FillRound(r, 6, bg);
    if (style == Btn::Secondary && enabled) c.StrokeRound(r, 6, t.divider);
    float w = r.right - r.left;
    auto* tf = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (glyph && label.empty()) {
        c.Glyph(glyph, Format(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, fg);
    } else if (glyph) {
        float tw = MeasureWidth(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 22;
        float x = r.left + (w - tw) / 2;
        c.Glyph(glyph, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true), D2D1::RectF(x, r.top, x + 18, r.bottom), fg);
        c.Text(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 22, r.top, r.right, r.bottom), fg);
    } else {
        c.Text(label, tf, r, fg);
    }
    AddHot(r, kind, arg, enabled, tip);
}

void Segmented(float x, float y, const std::vector<std::wstring>& opts, int sel, Hit kind, int base) {
    float cx0 = x;
    for (size_t k = 0; k < opts.size(); ++k) {
        bool s = (int)k == sel;
        auto* f = Format(13, s ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        float w = MeasureWidth(opts[k], Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28;
        D2D1_RECT_F r = D2D1::RectF(cx0, y, cx0 + w, y + 32);
        c.FillRound(r, 6, s ? Alpha(t.accent, 0.18f) : (IsHover(kind, base + (int)k) ? t.surfaceHover : Mix(t.surface, t.surfaceStrong, 0.3f)));
        if (s) c.StrokeRound(r, 6, Alpha(t.accent, 0.6f));
        c.Text(opts[k], f, r, s ? t.text : t.subtext);
        AddHot(r, kind, base + (int)k);
        cx0 += w + 6;
    }
}

void Toggle(float x, float y, bool on, Hit kind, int arg) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + 42, y + 22);
    bool h = IsHover(kind, arg);
    if (on) c.FillRound(r, 11, h ? t.accentHover : t.accent);
    else { c.FillRound(r, 11, h ? t.surfaceHover : Alpha(t.surfaceStrong, 0.6f)); c.StrokeRound(r, 11, Alpha(t.text, 0.45f), 1); }
    float kx = on ? r.right - 11 : r.left + 11;
    c.Circle(kx, y + 11, on ? 7.0f : 6.0f, on ? t.onAccent : Alpha(t.text, 0.8f));
    AddHot(D2D1::RectF(x - 6, y - 6, x + 48, y + 28), kind, arg);
}

void Slider(const D2D1_RECT_F& r, float value, float minV, float maxV, Hit kind, int arg, bool centered) {
    float cy = (r.top + r.bottom) / 2;
    float x0 = r.left + 8, x1 = r.right - 8;
    float tpos = (value - minV) / (maxV - minV);
    float kx = x0 + (x1 - x0) * std::clamp(tpos, 0.0f, 1.0f);
    c.FillRound(D2D1::RectF(x0, cy - 2, x1, cy + 2), 2, t.surfaceStrong);
    float from = centered ? x0 + (x1 - x0) * ((0 - minV) / (maxV - minV)) : x0;
    c.FillRound(D2D1::RectF(std::min(from, kx), cy - 2, std::max(from, kx), cy + 2), 2, t.accent);
    if (centered) c.Fill(D2D1::RectF(from - 0.5f, cy - 5, from + 0.5f, cy + 5), Alpha(t.text, 0.35f));
    bool h = IsHover(kind, arg) || IsPressed(kind, arg);
    c.Circle(kx, cy, h ? 9.0f : 8.0f, t.dark ? Rgb(0x454545) : Rgb(0xFFFFFF));
    c.Circle(kx, cy, h ? 6.0f : 5.0f, t.accent);
    AddHot(r, kind, arg);
}

void DrawTextField(const D2D1_RECT_F& r, TextField& f, Hit kind, const std::wstring& placeholder) {
    c.FillRound(r, 6, t.dark ? Alpha(Rgb(0x000000), 0.25f) : Rgb(0xFFFFFF));
    c.StrokeRound(r, 6, f.active ? t.accent : Alpha(t.text, 0.25f), f.active ? 1.5f : 1);
    if (f.active) c.Fill(D2D1::RectF(r.left + 4, r.bottom - 2, r.right - 4, r.bottom - 0.5f), t.accent);
    auto* fmt = Format(13.5f);
    D2D1_RECT_F tr = D2D1::RectF(r.left + 10, r.top, r.right - 10, r.bottom);
    if (f.text.empty() && !f.active) { c.Text(placeholder, fmt, tr, t.faint); AddHot(r, kind, 0); return; }
    IDWriteTextLayout* l = Layout(f.text.empty() ? std::wstring(L" ") : f.text, fmt, 10000, r.bottom - r.top);
    auto posX = [&](int i) {
        if (!l || f.text.empty()) return 0.0f;
        DWRITE_HIT_TEST_METRICS m;
        FLOAT px, py;
        l->HitTestTextPosition((UINT32)std::clamp(i, 0, (int)f.text.size()), FALSE, &px, &py, &m);
        return px;
    };
    float caretX = posX(f.caret);
    float shift = std::max(0.0f, caretX - (tr.right - tr.left) + 4);
    rt->PushAxisAlignedClip(tr, D2D1_ANTIALIAS_MODE_ALIASED);
    if (f.active && f.HasSelection()) {
        float a = posX(std::min(f.caret, f.anchor)), b = posX(std::max(f.caret, f.anchor));
        c.Fill(D2D1::RectF(tr.left + a - shift, r.top + 7, tr.left + b - shift, r.bottom - 7), Alpha(t.accent, 0.35f));
    }
    if (l && !f.text.empty()) {
        br->SetColor(t.text);
        rt->DrawTextLayout(D2D1::Point2F(tr.left - shift, r.top), l, br);
    }
    if (f.active && (GetTickCount() / 530) % 2 == 0) c.Fill(D2D1::RectF(tr.left + caretX - shift, r.top + 8, tr.left + caretX - shift + 1.2f, r.bottom - 8), t.text);
    rt->PopAxisAlignedClip();
    AddHot(r, kind, 0);
    if (f.active) StartAnim();
}

void DrawScrollbar(const D2D1_RECT_F& track, float scroll, float contentH, int arg) {
    float viewH = track.bottom - track.top;
    if (contentH <= viewH + 1) return;
    float th = std::max(36.0f, viewH * viewH / contentH);
    float ty = track.top + (viewH - th) * std::clamp(scroll / (contentH - viewH), 0.0f, 1.0f);
    bool h = IsHover(Hit::Scrollbar, arg) || IsPressed(Hit::Scrollbar, arg);
    float w = h ? 8.0f : 4.0f;
    c.FillRound(D2D1::RectF(track.right - w - 3, ty, track.right - 3, ty + th), w / 2, Alpha(t.text, h ? 0.45f : 0.25f));
    AddHot(D2D1::RectF(track.right - 14, track.top, track.right, track.bottom), Hit::Scrollbar, arg);
}

void DrawCaptionButtons(float right, float top, float h) {
    float x = right - kCaptionBtn;
    // close
    D2D1_RECT_F r = D2D1::RectF(x, top, right, top + h);
    bool hc = IsHover(Hit::CloseBtn, 0);
    if (hc) c.Fill(r, IsPressed(Hit::CloseBtn, 0) ? Rgb(0x94141A) : Rgb(0xC42B1C));
    c.Glyph(icon::Close, Format(10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, hc ? Rgb(0xFFFFFF) : t.text);
    AddHot(r, Hit::CloseBtn, 0, true, L"Close");
    x -= kCaptionBtn;
    r = D2D1::RectF(x, top, x + kCaptionBtn, top + h);
    if (MaxButtonHot() || IsHover(Hit::MaxBtn, 0)) c.Fill(r, Alpha(t.text, MaxButtonDown() ? 0.14f : 0.08f));
    c.Glyph(IsZoomed(hwnd) ? icon::Restore : icon::Maximize, Format(10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, t.text);
    AddHot(r, Hit::MaxBtn, 0);
    x -= kCaptionBtn;
    r = D2D1::RectF(x, top, x + kCaptionBtn, top + h);
    if (IsHover(Hit::MinBtn, 0)) c.Fill(r, Alpha(t.text, IsPressed(Hit::MinBtn, 0) ? 0.14f : 0.08f));
    c.Glyph(icon::Minimize, Format(10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, t.text);
    AddHot(r, Hit::MinBtn, 0, true, L"Minimize");
}

std::wstring ZoomText() {
    wchar_t b[32];
    float pct = zoomTarget * scale * 100;
    if (compareMode) {
        swprintf(b, 32, L"%.0f%%", CompareMul() * 100);
        return std::wstring(b);
    }
    if (pct >= 10 || pct == 0) swprintf(b, 32, L"%.0f%%", pct);
    else swprintf(b, 32, L"%.1f%%", pct);
    return b;
}

D2D1_COLOR_F ImageBackground() {
    switch (g_cfg.imageBg) {
        case ImageBg::Black: return Rgb(0x000000);
        case ImageBg::Gray: return Rgb(0x808080);
        default: return slideshow ? Rgb(0x000000) : Alpha(t.bg, 0.0f);
    }
}

// ---------------------------------------------------------------- the image
static ID2D1BitmapBrush* Checker() {
    if (s_checker && s_checkerGen == DeviceGeneration()) return s_checker;
    if (s_checker) { s_checker->Release(); s_checker = nullptr; }
    uint32_t px[16 * 16];
    uint32_t a = t.dark ? 0xFF3A3A3A : 0xFFFFFFFF, b = t.dark ? 0xFF2A2A2A : 0xFFE6E6E6;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) px[y * 16 + x] = ((x / 8) ^ (y / 8)) ? a : b;
    ID2D1Bitmap* bmp = nullptr;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(16, 16), px, 64, bp, &bmp))) {
        rt->CreateBitmapBrush(bmp, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), &s_checker);
        bmp->Release();
        s_checkerGen = DeviceGeneration();
    }
    return s_checker;
}

static ID2D1Layer* Layer() {
    if (s_layer && s_layerGen == DeviceGeneration()) return s_layer;
    if (s_layer) { s_layer->Release(); s_layer = nullptr; }
    rt->CreateLayer(&s_layer);
    s_layerGen = DeviceGeneration();
    return s_layer;
}

static float Snap(float v) { return std::round(v * scale) / scale; }

// Which bitmap to draw: the full-resolution one once it's there and actually sharper
static const GpuImage* PickBitmap(Entry* e, float destW) {
    const GpuImage* g = nullptr;
    if (!e->frames.empty()) {
        int fi = e->animated ? std::clamp(frameIndex, 0, (int)e->frames.size() - 1) : 0;
        if (!IEquals(e->path, path)) fi = 0;
        g = &e->frames[fi];
    }
    if (!editing && e->fullReady && !e->full.empty() && g && destW * scale > g->w * 1.02f) g = &e->full;
    return g;
}

static D2D1_INTERPOLATION_MODE PickInterpolation(float destW, UINT bmpW) {
    float ratio = destW * scale / std::max<UINT>(1, bmpW);
    if (std::fabs(ratio - 1) < 0.002f) return D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    if (ratio >= 3.0f) return D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;     // pixel peeping
    if (std::fabs(zoom - zoomTarget) > zoomTarget * 0.01f) return D2D1_INTERPOLATION_MODE_LINEAR;   // mid-animation
    return D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
}

static void DrawGpu(const GpuImage& g, const D2D1_RECT_F& dest, float opacity, D2D1_INTERPOLATION_MODE mode, ID2D1Image* effected) {
    float sx = (dest.right - dest.left) / g.w, sy = (dest.bottom - dest.top) / g.h;
    (void)effected;
    for (auto& tl : g.tiles) {
        D2D1_RECT_F d = D2D1::RectF(dest.left + tl.x * sx, dest.top + tl.y * sy, dest.left + (tl.x + tl.w) * sx, dest.top + (tl.y + tl.h) * sy);
        if (g.tiles.size() > 1) { d.left = Snap(d.left); d.top = Snap(d.top); d.right = Snap(d.right); d.bottom = Snap(d.bottom); }
        if (dc) dc->DrawBitmap(tl.bmp, d, opacity, mode, nullptr, nullptr);
        else rt->DrawBitmap(tl.bmp, d, opacity, mode == D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
}

void DrawImage(Entry* e, const D2D1_RECT_F& dest0, float opacity, bool allowEffects) {
    if (!e || !e->ready()) return;
    D2D1_RECT_F dest = dest0;
    float dw = dest.right - dest.left;
    const GpuImage* g = PickBitmap(e, dw);
    if (!g || g->empty()) return;
    if (g_cfg.imageBg == ImageBg::Checker && e->hasAlpha) {
        if (ID2D1BitmapBrush* cb = Checker()) {
            cb->SetTransform(D2D1::Matrix3x2F::Translation(dest.left, dest.top));
            cb->SetOpacity(opacity);
            rt->FillRectangle(dest, cb);
        }
    }
    auto mode = editing ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC : PickInterpolation(dw, g->w);
    // pixel-exact at 100%
    if (mode == D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR && std::fabs(dw * scale - g->w) < 1) {
        dest.left = Snap(dest.left); dest.top = Snap(dest.top);
        dest.right = dest.left + g->w / scale; dest.bottom = dest.top + g->h / scale;
    }
    ID2D1Image* effected = nullptr;
    std::vector<ID2D1Effect*> fx;
    if (allowEffects && dc && editing && edit.HasAdjustments() && g->tiles.size() == 1) {
        ID2D1Bitmap1* b1 = nullptr;
        g->tiles[0].bmp->QueryInterface(__uuidof(ID2D1Bitmap1), (void**)&b1);
        if (b1) {
            effected = ApplyAdjustments(dc, b1, edit, fx);
            b1->Release();
        }
    }
    if (effected) {
        float sx = (dest.right - dest.left) / g->w, sy = (dest.bottom - dest.top) / g->h;
        D2D1_MATRIX_3X2_F old;
        dc->GetTransform(&old);
        // DIPs per bitmap pixel; DrawImage measures in DIPs at the target's DPI, and bitmaps were made at 96 DPI
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(dest.left, dest.top) * old);
        dc->DrawImage(effected, D2D1::Point2F(0, 0), D2D1::RectF(0, 0, (float)g->w, (float)g->h), mode);
        dc->SetTransform(old);
        effected->Release();
        for (auto* f : fx) f->Release();
    } else {
        DrawGpu(*g, dest, opacity, mode, nullptr);
    }
}

static D2D1_RECT_F DestFor(Entry* e, float z, float ccx, float ccy, const D2D1_RECT_F& view) {
    float vcx = (view.left + view.right) / 2, vcy = (view.top + view.bottom) / 2;
    float l = vcx - ccx * z, tp = vcy - ccy * z;
    return D2D1::RectF(l, tp, l + e->fullW * z, tp + e->fullH * z);
}

static void DrawSpinner(float cx0, float cy0, float r) {
    float a0 = (GetTickCount() % 1000) / 1000.0f * 6.2832f;
    for (int i = 0; i < 8; ++i) {
        float a = a0 + i * 6.2832f / 8;
        c.Circle(cx0 + std::cos(a) * r, cy0 + std::sin(a) * r, 2.2f, Alpha(t.text, 0.15f + 0.1f * i));
    }
    StartAnim();
}

static void DrawError(Entry* e, const D2D1_RECT_F& v) {
    float mx = (v.left + v.right) / 2, my = (v.top + v.bottom) / 2;
    c.Glyph(icon::Photo, Format(40, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(mx - 40, my - 110, mx + 40, my - 50), t.faint);
    c.Text(PathName(e->path), Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(v.left + 20, my - 40, v.right - 20, my - 14), t.text);
    float w = std::min(520.0f, v.right - v.left - 40);
    std::wstring msg = e->error.empty() ? L"This image couldn't be opened." : e->error;
    float th = MeasureHeight(msg, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true), w);
    c.Text(msg, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true), D2D1::RectF(mx - w / 2, my - 6, mx + w / 2, my - 6 + th + 4), t.subtext);
    if (!e->codecStoreId.empty()) {
        std::wstring l = L"Get " + e->codecName;
        float bw = ButtonWidth(l, icon::Download);
        Button(D2D1::RectF(mx - bw / 2, my + th + 12, mx + bw / 2, my + th + 46), l, icon::Download, Btn::Primary, Hit::CodecLink);
    }
}

static void DrawSingle(const D2D1_RECT_F& v) {
    Entry* e = Current();
    D2D1_COLOR_F bg = ImageBackground();
    if (bg.a > 0) c.Fill(v, bg);
    rt->PushAxisAlignedClip(v, D2D1_ANTIALIAS_MODE_ALIASED);
    if (e && e->ready()) {
        // slideshow crossfade
        if (SlideFade() > 0) {
            for (auto& x : cache) {
                if (x->id != SlideFadeFrom() || !x->ready()) continue;
                float z = FitZoom(x.get(), v);
                DrawImage(x.get(), DestFor(x.get(), z, x->fullW / 2.0f, x->fullH / 2.0f, v), 1, false);
            }
        }
        D2D1_RECT_F dest = DestFor(e, zoom, cx, cy, v);
        DrawImage(e, dest, 1 - SlideFade(), false);
        s_lastShownId = e->id;
        // zoomed in past the screen-sized decode: fetch the full resolution
        if (!e->viewIsFull && !e->fullReady && !e->fullPending && !e->animated && !e->frames.empty() &&
            (dest.right - dest.left) * scale > e->frames[0].w * 1.05f && std::fabs(zoom - zoomTarget) < zoomTarget * 0.02f) {
            e->fullPending = true;
            loader::Decode(e->id, e->path, true, 0, 0, 1);
        }
        s_loadingPath.clear();
    } else if (e && e->failed()) {
        DrawError(e, v);
        s_lastShownId = 0;
    } else {
        // still decoding: keep the previous image up (never a blank frame), or the thumbnail
        if (!IEquals(s_loadingPath, path)) { s_loadingPath = path; s_loadingSince = GetTickCount(); }
        UINT tw = 0, th = 0;
        ID2D1Bitmap* tb = ThumbBitmap(path, &tw, &th);
        bool drew = false;
        if (tb && tw && th) {
            float z = std::min((v.right - v.left) / tw, (v.bottom - v.top) / th);
            if (!g_cfg.enlargeSmall && e && e->fullW) z = std::min(z, 1.0f / scale * e->fullW / tw);
            float w = tw * z, h = th * z, l = (v.left + v.right - w) / 2, tp = (v.top + v.bottom - h) / 2;
            rt->DrawBitmap(tb, D2D1::RectF(l, tp, l + w, tp + h), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            drew = true;
        } else {
            for (auto& x : cache) {
                if (x->id != s_lastShownId || !x->ready()) continue;
                float z = FitZoom(x.get(), v);
                DrawImage(x.get(), DestFor(x.get(), z, x->fullW / 2.0f, x->fullH / 2.0f, v), 1, false);
                drew = true;
            }
        }
        if (GetTickCount() - s_loadingSince > 300 || !drew) DrawSpinner((v.left + v.right) / 2, (v.top + v.bottom) / 2, 14);
        else StartAnim();
    }
    rt->PopAxisAlignedClip();
    AddHot(v, Hit::Viewport, 0);
}

// ---------------------------------------------------------------- compare
static void DrawCompare(const D2D1_RECT_F& v) {
    float mid = (v.left + v.right) / 2;
    D2D1_COLOR_F bg = ImageBackground();
    for (int i = 0; i < 2; ++i) {
        D2D1_RECT_F pane = i == 0 ? D2D1::RectF(v.left, v.top, mid - 1, v.bottom) : D2D1::RectF(mid + 1, v.top, v.right, v.bottom);
        if (bg.a > 0) c.Fill(pane, bg);
        rt->PushAxisAlignedClip(pane, D2D1_ANTIALIAS_MODE_ALIASED);
        Entry* e = FindEntry(comparePath[i]);
        if (e && e->ready()) {
            float z = FitZoom(e, pane) * CompareMul();
            float ccx = CompareU() * e->fullW, ccy = CompareV() * e->fullH;
            float vw = (pane.right - pane.left) / z, vh = (pane.bottom - pane.top) / z;
            ccx = vw >= e->fullW ? e->fullW / 2.0f : std::clamp(ccx, vw / 2, e->fullW - vw / 2);
            ccy = vh >= e->fullH ? e->fullH / 2.0f : std::clamp(ccy, vh / 2, e->fullH - vh / 2);
            DrawImage(e, DestFor(e, z, ccx, ccy, pane), 1, false);
            if (e->frames.size() && z * scale * e->fullW > e->frames[0].w * 1.05f && !e->viewIsFull && !e->fullReady && !e->fullPending && !e->animated) {
                e->fullPending = true;
                loader::Decode(e->id, e->path, true, 0, 0, 1);
            }
        } else if (e && e->failed()) {
            DrawError(e, pane);
        } else {
            DrawSpinner((pane.left + pane.right) / 2, (pane.top + pane.bottom) / 2, 14);
        }
        rt->PopAxisAlignedClip();
        // label
        std::wstring name = PathName(comparePath[i]);
        auto* f = Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float w = std::min(MeasureWidth(name, f) + 24, pane.right - pane.left - 24);
        float top = pane.top + (chromeAlpha > 0.01f ? kTopBar * chromeAlpha : 0) + 10;
        D2D1_RECT_F lr = D2D1::RectF(pane.left + 12, top, pane.left + 12 + w, top + 26);
        bool active = comparePane == i;
        c.FillRound(lr, 13, active ? t.accent : Alpha(t.surface, 0.9f));
        c.Text(name, f, D2D1::RectF(lr.left + 12, lr.top, lr.right - 10, lr.bottom), active ? t.onAccent : t.text);
        if (active) c.StrokeRound(pane, 0, Alpha(t.accent, 0.8f), 2);
        AddHot(pane, Hit::Pane, i);
    }
    c.Fill(D2D1::RectF(mid - 1, v.top, mid + 1, v.bottom), t.divider);
}

// ---------------------------------------------------------------- top bar
struct ToolDef { Cmd cmd; wchar_t glyph; const wchar_t* tip; int priority; bool toggled; bool enabled; };

static void ToolButton(float& x, const ToolDef& d, float top, float h) {
    D2D1_RECT_F r = D2D1::RectF(x - 38, top + (h - 34) / 2, x, top + (h + 34) / 2);
    if (d.toggled) c.FillRound(r, 6, Alpha(t.accent, 0.18f));
    bool hv = d.enabled && IsHover(Hit::Tool, (int)d.cmd);
    if (hv) c.FillRound(r, 6, Alpha(t.text, IsPressed(Hit::Tool, (int)d.cmd) ? 0.14f : 0.08f));
    D2D1_COLOR_F fg = !d.enabled ? t.faint : d.toggled ? t.accent : t.text;
    auto* gf = Format(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true);
    if (d.cmd == Cmd::RotateLeft) {
        // the rotate glyph, mirrored
        D2D1_MATRIX_3X2_F old;
        rt->GetTransform(&old);
        float mx = (r.left + r.right) / 2;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1, D2D1::Point2F(mx, 0)) * old);
        c.Glyph(icon::Rotate, gf, r, fg);
        rt->SetTransform(old);
    } else if (d.cmd == Cmd::Filmstrip) {
        float mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2;
        c.StrokeRound(D2D1::RectF(mx - 8, my - 7, mx + 8, my + 3), 1.5f, fg, 1.3f);
        for (int k = 0; k < 3; ++k) c.FillRound(D2D1::RectF(mx - 8 + k * 6, my + 5, mx - 4 + k * 6, my + 8), 0.8f, fg);
    } else if (d.cmd == Cmd::Compare) {
        float mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2;
        c.StrokeRound(D2D1::RectF(mx - 9, my - 6, mx - 1, my + 6), 1.5f, fg, 1.3f);
        c.StrokeRound(D2D1::RectF(mx + 1, my - 6, mx + 9, my + 6), 1.5f, fg, 1.3f);
    } else {
        c.Glyph(d.glyph, gf, r, fg);
    }
    AddHot(r, Hit::Tool, (int)d.cmd, d.enabled, d.tip);
    x -= 40;
}

static void Separator(float& x, float top, float h) {
    c.Fill(D2D1::RectF(x - 6, top + h / 2 - 9, x - 5, top + h / 2 + 9), Alpha(t.text, 0.15f));
    x -= 11;
}

static void DrawTopBar(float alpha, bool gridBar) {
    if (alpha <= 0.01f) return;
    D2D1_RECT_F bar = D2D1::RectF(0, 0, W, kTopBar);
    bool overImage = !gridBar && !settingsOpen;
    D2D1_COLOR_F barBg = overImage ? Alpha(t.dark ? Mix(t.bg, Rgb(0x000000), 0.25f) : t.surface, (t.backdrop ? 0.80f : 0.88f) * alpha) : Alpha(t.bg, 0);
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha), Layer());
    c.Fill(bar, barBg);
    AddHot(bar, Hit::Caption, 0);
    float h = kTopBar;
    float right = W;
    if (!fullscreen) { DrawCaptionButtons(W, 0, h); right -= kCaptionBtn * 3; }
    else {
        D2D1_RECT_F r = D2D1::RectF(W - 46, 0, W, h);
        Button(r, L"", icon::BackToWindow, Btn::Chrome, Hit::Tool, (int)Cmd::Fullscreen, true, L"Exit full screen (F11)");
        right -= 46;
    }
    float x = right - 8;
    // title on the left
    float left = 14;
    std::wstring title, sub;
    if (gridBar) {
        title = PathName(folder);
        if (title.empty()) title = folder;
        sub = std::to_wstring(files.size()) + (files.size() == 1 ? L" image" : L" images");
    } else if (compareMode) {
        title = L"Compare";
        sub = L"";
    } else {
        title = PathName(path);
        if (index >= 0 && files.size() > 0) sub = std::to_wstring(index + 1) + L" / " + std::to_wstring(files.size());
        if (editing) sub = editDirty ? L"Editing · not saved" : L"Editing";
    }
    // tools on the right
    Entry* e = Current();
    bool img = e && e->ready();
    std::vector<ToolDef> tools;
    if (gridBar) {
        tools = {
            {Cmd::Grid, icon::Back, L"Back to the image (G)", 0, false, true},
            {Cmd::Compare, 0, L"Compare the two picked images (C)", 2, false, gridMarks.size() == 2},
            {Cmd::Slideshow, icon::Slideshow, L"Slideshow (Space)", 3, false, !files.empty()},
            {Cmd::Settings, icon::Settings, L"Settings (Ctrl+,)", 1, false, true},
        };
    } else if (!editing) {
        tools = {
            {Cmd::More, icon::More, L"More", 0, false, true},
            {Cmd::Delete, icon::Delete, L"Delete (Del)", 1, false, !path.empty()},
            {Cmd::Info, icon::Info, L"Info (I)", 1, g_cfg.infoPanel, !path.empty()},
            {Cmd::Edit, icon::Edit, L"Edit (E)", 1, false, img && !e->animated},
            {Cmd::RotateRight, icon::Rotate, L"Rotate right (R)", 2, false, img && !e->animated},
            {Cmd::RotateLeft, icon::Rotate, L"Rotate left (Shift+R)", 3, false, img && !e->animated},
            {Cmd::Slideshow, icon::Slideshow, L"Slideshow (Space)", 4, false, img},
            {Cmd::Compare, 0, L"Compare (C)", 4, compareMode, files.size() > 1},
            {Cmd::Filmstrip, 0, L"Filmstrip (F)", 3, g_cfg.filmstrip, files.size() > 1},
            {Cmd::Grid, icon::Grid, L"All images (G)", 2, false, files.size() > 0},
        };
    }
    float titleW = std::max(MeasureWidth(title, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)), 80.0f) + 120;
    float avail = x - left - std::min(titleW, 360.0f);
    // the zoom group needs ~150
    bool showZoom = !gridBar && !editing && img && avail > 150 + 40 * 4;
    if (showZoom) avail -= 160;
    for (int pr = 4; pr >= 1; --pr) {
        float need = 0;
        need = 40.0f * tools.size();
        if (need <= avail) break;
        tools.erase(std::remove_if(tools.begin(), tools.end(), [pr](const ToolDef& d) { return d.priority >= pr; }), tools.end());
    }
    for (size_t i = 0; i < tools.size(); ++i) {
        ToolButton(x, tools[i], 0, h);
        if (!gridBar && (tools[i].cmd == Cmd::Delete || tools[i].cmd == Cmd::RotateRight) && i + 1 < tools.size()) Separator(x, 0, h);
    }
    if (gridBar) {
        // sort
        static const wchar_t* names[] = {L"Name", L"Date taken", L"Date modified", L"Size"};
        std::wstring l = std::wstring(L"Sort: ") + names[(int)g_cfg.sort] + (g_cfg.sortDescending ? L" ↓" : L" ↑");
        float bw = ButtonWidth(l, icon::Sort);
        Button(D2D1::RectF(x - bw, (h - 32) / 2, x, (h + 32) / 2), l, icon::Sort, Btn::Chrome, Hit::SortMenu, 0, true, L"Sort order (S)");
        x -= bw + 8;
    }
    if (showZoom) {
        Separator(x, 0, h);
        ToolButton(x, {Cmd::ZoomIn, icon::ZoomIn, L"Zoom in (+)", 0, false, true}, 0, h);
        std::wstring zt = ZoomText();
        D2D1_RECT_F zr = D2D1::RectF(x - 58, (h - 30) / 2, x, (h + 30) / 2);
        bool zh = IsHover(Hit::Tool, (int)(fitMode ? Cmd::Zoom100 : Cmd::ZoomFit));
        if (zh) c.FillRound(zr, 6, Alpha(t.text, 0.08f));
        c.Text(zt, Format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), zr, t.text);
        AddHot(zr, Hit::Tool, (int)(fitMode ? Cmd::Zoom100 : Cmd::ZoomFit), true, fitMode ? L"Actual size (1)" : L"Fit to window (0)");
        x -= 60;
        ToolButton(x, {Cmd::ZoomOut, icon::ZoomOut, L"Zoom out (-)", 0, false, true}, 0, h);
    }
    // title
    float titleRight = x - 12;
    if (!title.empty() && titleRight > left + 40) {
        auto* tf = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float tw = std::min(MeasureWidth(title, tf), titleRight - left);
        c.Text(title, tf, D2D1::RectF(left, 0, left + tw + 1, h), t.text);
        if (!sub.empty() && left + tw + 12 < titleRight) c.Text(sub, Format(12), D2D1::RectF(left + tw + 12, 0, titleRight, h), t.subtext);
    }
    if (overImage && alpha > 0.5f) c.Fill(D2D1::RectF(0, h - 1, W, h), Alpha(t.text, 0.06f));
    rt->PopLayer();
}

// ---------------------------------------------------------------- arrows
static void DrawNavArrows(const D2D1_RECT_F& v, float alpha) {
    if (alpha <= 0.01f || files.size() < 2 || editing || compareMode) return;
    float my = (v.top + v.bottom) / 2;
    for (int side = 0; side < 2; ++side) {
        float cxx = side == 0 ? v.left + 34 : v.right - 34;
        Hit k = side == 0 ? Hit::NavPrev : Hit::NavNext;
        bool hv = IsHover(k, 0);
        bool atEnd = !g_cfg.loopFolder && ((side == 0 && index <= 0) || (side == 1 && index >= (int)files.size() - 1));
        if (atEnd) continue;
        c.Circle(cxx, my, 20, Alpha(t.dark ? Rgb(0x202020) : Rgb(0xFFFFFF), (hv ? 0.95f : 0.70f) * alpha));
        c.Glyph(side == 0 ? icon::ChevronLeft : icon::ChevronRight, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
                D2D1::RectF(cxx - 20, my - 20, cxx + 20, my + 20), Alpha(t.text, alpha));
        if (alpha > 0.3f) AddHot(D2D1::RectF(cxx - 24, my - 24, cxx + 24, my + 24), k, 0, true, side == 0 ? L"Previous (←)" : L"Next (→)");
    }
}

// ---------------------------------------------------------------- thumbnails
static void DrawThumbIn(const std::wstring& p, const D2D1_RECT_F& cell, bool cover, float radius) {
    UINT tw = 0, th = 0;
    ID2D1Bitmap* b = ThumbBitmap(p, &tw, &th);
    RequestThumb(p);
    c.FillRound(cell, radius, Alpha(t.text, 0.06f));
    if (!b || !tw || !th) {
        auto it = thumbs.find(p);
        if (it != thumbs.end() && it->second.failed)
            c.Glyph(icon::Photo, Format(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), cell, t.faint);
        return;
    }
    float cw = cell.right - cell.left, ch = cell.bottom - cell.top;
    float z = cover ? std::max(cw / tw, ch / th) : std::min(cw / tw, ch / th);
    if (!cover) z = std::min(z, 1.0f / scale * 2);
    float w = tw * z, h = th * z;
    D2D1_RECT_F d = D2D1::RectF((cell.left + cell.right - w) / 2, (cell.top + cell.bottom - h) / 2, (cell.left + cell.right + w) / 2, (cell.top + cell.bottom + h) / 2);
    if (cover) {
        ID2D1RoundedRectangleGeometry* g = nullptr;
        d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(cell, radius, radius), &g);
        if (g) rt->PushLayer(D2D1::LayerParameters(cell, g), Layer());
        if (dc) dc->DrawBitmap(b, d, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        else rt->DrawBitmap(b, d);
        if (g) { rt->PopLayer(); g->Release(); }
    } else {
        if (dc) dc->DrawBitmap(b, d, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        else rt->DrawBitmap(b, d);
    }
}

static void DrawFilmstrip(const D2D1_RECT_F& r) {
    c.Fill(r, t.dark ? Mix(t.bg, Rgb(0x000000), 0.2f) : t.sidebar);
    c.Fill(D2D1::RectF(r.left, r.top, r.right, r.top + 1), t.divider);
    AddHot(r, Hit::FilmArea, 0);
    int n = (int)files.size();
    if (!n) return;
    float cell = 64, gap = 6, step = cell + gap;
    float total = n * step - gap;
    float viewW = r.right - r.left - 24;
    float maxScroll = std::max(0.0f, total - viewW);
    float centerCur = index >= 0 ? index * step + cell / 2 - viewW / 2 : 0;
    if (filmScroll < 0 || filmScrollTarget == -1) { filmScroll = filmScrollTarget = std::clamp(centerCur, 0.0f, maxScroll); s_filmLastIndex = index; }
    if (index != s_filmLastIndex) {
        s_filmLastIndex = index;
        // follow the current image when it leaves the strip
        float curX = index * step - filmScroll;
        if (curX < step || curX > viewW - 2 * step) { filmScrollTarget = std::clamp(centerCur, 0.0f, maxScroll); StartAnim(); }
    }
    filmScroll = std::clamp(filmScroll, 0.0f, maxScroll);
    if (filmScrollTarget >= 0) filmScrollTarget = std::clamp(filmScrollTarget, 0.0f, maxScroll);
    float x0 = r.left + 12 - filmScroll;
    if (total < viewW) x0 = r.left + 12 + (viewW - total) / 2;
    float y = r.top + (r.bottom - r.top - cell) / 2;
    rt->PushAxisAlignedClip(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    int first = std::max(0, (int)((filmScroll - 12) / step) - 1), last = std::min(n - 1, (int)((filmScroll + viewW) / step) + 1);
    if (total < viewW) { first = 0; last = n - 1; }
    for (int i = first; i <= last; ++i) {
        D2D1_RECT_F cr = D2D1::RectF(x0 + i * step, y, x0 + i * step + cell, y + cell);
        std::wstring p = folder + L"\\" + files[i].name;
        DrawThumbIn(p, cr, true, 6);
        bool cur = compareMode ? IEquals(p, comparePath[comparePane]) : i == index;
        bool other = compareMode && IEquals(p, comparePath[1 - comparePane]);
        if (cur) c.StrokeRound(D2D1::RectF(cr.left - 2, cr.top - 2, cr.right + 2, cr.bottom + 2), 8, t.accent, 2.5f);
        else if (other) c.StrokeRound(D2D1::RectF(cr.left - 2, cr.top - 2, cr.right + 2, cr.bottom + 2), 8, Alpha(t.text, 0.5f), 1.5f);
        else if (IsHover(Hit::Film, i)) c.StrokeRound(cr, 6, Alpha(t.text, 0.4f), 1.5f);
        AddHot(cr, Hit::Film, i, true, files[i].name.c_str());
    }
    rt->PopAxisAlignedClip();
}

// ---------------------------------------------------------------- grid
} // namespace app

void app::GridGeometry(float& left, float& top, float& cell, float& size, int& cols) {
    size = (float)g_cfg.gridSize;
    float gap = 8;
    cell = size + gap;
    float avail = W - 32;
    cols = std::max(1, (int)((avail + gap) / cell));
    left = (W - (cols * cell - gap)) / 2;
    top = kTopBar + 12;
}

namespace app {

static void DrawGrid() {
    c.Fill(D2D1::RectF(0, 0, W, H), Alpha(t.bg, 0));
    int n = (int)files.size();
    float left, top, cell, size;
    int cols;
    GridGeometry(left, top, cell, size, cols);
    int rows = (n + cols - 1) / cols;
    gridContentH = rows * cell + 24;
    float viewH = H - kTopBar;
    float maxScroll = std::max(0.0f, gridContentH - viewH);
    int sel = std::max(0, index);
    float selTop = (sel / cols) * cell;
    if (gridScrollTarget == -1 || gridScroll < 0) {
        gridScroll = gridScrollTarget = std::clamp(selTop - viewH / 2 + cell / 2, 0.0f, maxScroll);
    } else if (gridScrollTarget == -2) {
        float tgt = gridScroll;
        if (selTop < gridScroll) tgt = selTop;
        else if (selTop + cell > gridScroll + viewH - 12) tgt = selTop + cell - viewH + 12;
        gridScrollTarget = std::clamp(tgt, 0.0f, maxScroll);
        StartAnim();
    }
    gridScroll = std::clamp(gridScroll, 0.0f, maxScroll);
    rt->PushAxisAlignedClip(D2D1::RectF(0, kTopBar, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
    AddHot(D2D1::RectF(0, kTopBar, W, H), Hit::GridArea, 0);
    int firstRow = std::max(0, (int)(gridScroll / cell) - 1), lastRow = std::min(rows - 1, (int)((gridScroll + viewH) / cell) + 1);
    for (int row = firstRow; row <= lastRow; ++row) {
        for (int col = 0; col < cols; ++col) {
            int i = row * cols + col;
            if (i >= n) break;
            float x = left + col * cell, y = top + row * cell - gridScroll;
            D2D1_RECT_F cr = D2D1::RectF(x, y, x + size, y + size);
            std::wstring p = folder + L"\\" + files[i].name;
            DrawThumbIn(p, cr, false, 8);
            bool hv = IsHover(Hit::GridCell, i);
            if (i == index) c.StrokeRound(D2D1::RectF(cr.left - 3, cr.top - 3, cr.right + 3, cr.bottom + 3), 10, t.accent, 2.5f);
            else if (hv) c.StrokeRound(cr, 8, Alpha(t.text, 0.35f), 1.5f);
            auto mk = std::find(gridMarks.begin(), gridMarks.end(), i);
            if (mk != gridMarks.end()) {
                float bx = cr.right - 16, by = cr.top + 16;
                c.Circle(bx, by, 11, t.accent);
                c.Text(std::to_wstring(mk - gridMarks.begin() + 1), Format(11.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                       D2D1::RectF(bx - 11, by - 11, bx + 11, by + 11), t.onAccent);
            }
            if (hv) {
                // name on hover
                auto* f = Format(11.5f);
                D2D1_RECT_F nr = D2D1::RectF(cr.left + 6, cr.bottom - 24, cr.right - 6, cr.bottom - 4);
                c.FillRound(nr, 4, Alpha(Rgb(0x000000), 0.55f));
                c.Text(files[i].name, f, D2D1::RectF(nr.left + 6, nr.top, nr.right - 6, nr.bottom), Rgb(0xFFFFFF));
            }
            AddHot(cr, Hit::GridCell, i, true, nullptr);
        }
    }
    rt->PopAxisAlignedClip();
    DrawScrollbar(D2D1::RectF(0, kTopBar + 4, W, H - 4), gridScroll, gridContentH, 1);
    if (n == 0) c.Text(L"No images in this folder", Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(0, kTopBar, W, H), t.subtext);
    DrawTopBar(1, true);
}

// ---------------------------------------------------------------- slideshow controls
static void DrawSlideControls() {
    float a = chromeAlpha;
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), Layer());
    float w = 400, h = 52;
    D2D1_RECT_F r = D2D1::RectF((W - w) / 2, H - h - 28, (W + w) / 2, H - 28);
    c.FillRound(r, 26, Alpha(Rgb(0x1C1C1C), 0.88f));
    float x = r.left + 12;
    auto btn = [&](int arg, wchar_t g, const wchar_t* tip) {
        D2D1_RECT_F b = D2D1::RectF(x, r.top + 8, x + 36, r.bottom - 8);
        if (IsHover(Hit::SlideControl, arg)) c.FillRound(b, 18, Rgb(0xFFFFFF, 0.12f));
        c.Glyph(g, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), b, Rgb(0xFFFFFF));
        AddHot(b, Hit::SlideControl, arg, true, tip);
        x += 40;
    };
    btn(0, icon::Previous, L"Previous");
    btn(1, SlidePaused() ? icon::Play : icon::Pause, L"Pause or play (Space)");
    btn(2, icon::Next, L"Next");
    x += 8;
    btn(3, icon::Remove, L"Faster (↑)");
    c.Text(std::to_wstring(g_cfg.slideshowSeconds) + L" s", Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
           D2D1::RectF(x - 4, r.top, x + 40, r.bottom), Rgb(0xFFFFFF));
    x += 40;
    btn(4, icon::Add, L"Slower (↓)");
    x += 8;
    D2D1_RECT_F sb = D2D1::RectF(x, r.top + 8, x + 36, r.bottom - 8);
    if (g_cfg.slideshowShuffle) c.FillRound(sb, 18, Alpha(t.accent, 0.35f));
    btn(5, icon::Shuffle, L"Shuffle");
    btn(6, icon::Close, L"Stop (Esc)");
    rt->PopLayer();
}

// ---------------------------------------------------------------- toasts, update prompt, dialog, tooltip
static void DrawToast(float bottom) {
    if (toastAlpha <= 0.01f || toastText.empty()) return;
    auto* f = Format(13);
    float tw = MeasureWidth(toastText, f);
    float aw = toastAction.empty() ? 0 : MeasureWidth(toastAction, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28;
    float w = std::min(W - 40, tw + 36 + aw);
    float y = bottom - 16 - 40 + (1 - toastAlpha) * 10;
    D2D1_RECT_F r = D2D1::RectF((W - w) / 2, y, (W + w) / 2, y + 40);
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), toastAlpha), Layer());
    c.FillRound(r, 20, t.dark ? Rgb(0x2B2B2B) : Rgb(0x2B2B2B));
    c.Text(toastText, f, D2D1::RectF(r.left + 18, r.top, r.right - 18 - aw, r.bottom), Rgb(0xF2F2F2));
    if (!toastAction.empty()) {
        D2D1_RECT_F ar = D2D1::RectF(r.right - aw - 6, r.top + 5, r.right - 6, r.bottom - 5);
        if (IsHover(Hit::ToastAction, 0)) c.FillRound(ar, 15, Rgb(0xFFFFFF, 0.12f));
        D2D1_COLOR_F ac = Luma(t.accent) < 0.35f ? Mix(t.accent, Rgb(0xFFFFFF), 0.45f) : t.accent;
        c.Text(toastAction, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), ar, ac);
        AddHot(ar, Hit::ToastAction, 0);
    }
    rt->PopLayer();
}

static void DrawUpdatePrompt(float bottom) {
    if (update.kind != UpdateState::Ready || updateDismissed || slideshow || settingsOpen) return;
    std::wstring text = L"Update ready · Loupe " + update.version;
    auto* f = Format(12.5f);
    float w = MeasureWidth(text, f) + 30 + 84 + 34;
    D2D1_RECT_F r = D2D1::RectF(W - w - 16, bottom - 16 - 42, W - 16, bottom - 16);
    c.FillRound(r, 10, t.surface);
    c.StrokeRound(r, 10, Alpha(t.accent, 0.6f), 1);
    c.Glyph(icon::Update, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 8, r.top, r.left + 30, r.bottom), t.accent);
    c.Text(text, f, D2D1::RectF(r.left + 32, r.top, r.right - 118, r.bottom), t.text);
    Button(D2D1::RectF(r.right - 116, r.top + 6, r.right - 36, r.bottom - 6), L"Restart", 0, Btn::Primary, Hit::UpdateRestart, 0, true, L"Restart Loupe on the new version");
    Button(D2D1::RectF(r.right - 34, r.top + 6, r.right - 6, r.bottom - 6), L"", icon::Cancel, Btn::Subtle, Hit::UpdateDismiss, 0, true, L"Later");
}

static void DrawDialog() {
    if (!dialog.open) return;
    c.Fill(D2D1::RectF(0, 0, W, H), Rgb(0x000000, 0.45f));
    AddHot(D2D1::RectF(0, 0, W, H), Hit::None, 0);
    float w = std::min(460.0f, W - 40);
    auto* bf = Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    float bodyH = dialog.body.empty() ? 0 : MeasureHeight(dialog.body, bf, w - 48) + 8;
    float fieldH = dialog.field ? 52 + (dialog.fieldError.empty() ? 0 : 20) : 0;
    float h = 24 + 28 + bodyH + fieldH + 20 + 36 + 20;
    D2D1_RECT_F r = D2D1::RectF((W - w) / 2, (H - h) / 2, (W + w) / 2, (H + h) / 2);
    c.FillRound(r, 10, t.dark ? Mix(t.surface, Rgb(0xFFFFFF), 0.03f) : Rgb(0xFFFFFF));
    c.StrokeRound(r, 10, Alpha(t.text, 0.12f));
    float y = r.top + 22;
    c.Text(dialog.title, Format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 24, y, r.right - 24, y + 28), t.text);
    y += 34;
    if (bodyH > 0) { c.Text(dialog.body, bf, D2D1::RectF(r.left + 24, y, r.right - 24, y + bodyH), t.subtext); y += bodyH; }
    if (dialog.field) {
        DrawTextField(D2D1::RectF(r.left + 24, y + 4, r.right - 24, y + 40), dialog.input, Hit::DialogField, L"");
        y += 48;
        if (!dialog.fieldError.empty()) { c.Text(dialog.fieldError, Format(12), D2D1::RectF(r.left + 24, y, r.right - 24, y + 18), t.bad); y += 20; }
    }
    // buttons, right-aligned; the area under them is tinted like Windows dialogs
    D2D1_RECT_F foot = D2D1::RectF(r.left, r.bottom - 76, r.right, r.bottom);
    float bx = r.right - 24;
    for (int i = (int)dialog.buttons.size() - 1; i >= 0; --i) {
        float bw = std::max(96.0f, ButtonWidth(dialog.buttons[i], 0));
        Btn style = i == dialog.primary ? (dialog.danger ? Btn::Danger : Btn::Primary) : Btn::Secondary;
        if (dialog.danger && i == 0 && dialog.primary != 0) style = Btn::Danger;
        Button(D2D1::RectF(bx - bw, foot.bottom - 56, bx, foot.bottom - 22), dialog.buttons[i], 0, style, Hit::DialogButton, i);
        bx -= bw + 8;
    }
}

static void DrawTooltip() {
    if (hover.kind == Hit::None || pressed.kind != Hit::None) { s_tipFor = hover; s_tipSince = GetTickCount(); return; }
    const wchar_t* tip = nullptr;
    D2D1_RECT_F at{};
    for (auto it = hot.rbegin(); it != hot.rend(); ++it) {
        if (it->kind == hover.kind && it->arg == hover.arg) { tip = it->tip; at = it->r; break; }
    }
    if (hover.kind != s_tipFor.kind || hover.arg != s_tipFor.arg) { s_tipFor = hover; s_tipSince = GetTickCount(); }
    if (!tip || !*tip) return;
    if (GetTickCount() - s_tipSince < 550) { StartAnim(); return; }
    auto* f = Format(12);
    std::wstring s = tip;
    float w = MeasureWidth(s, f) + 20, h = 28;
    float x = std::clamp((at.left + at.right) / 2 - w / 2, 6.0f, W - w - 6);
    float y = at.bottom + 6;
    if (y + h > H - 4) y = at.top - h - 6;
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    c.FillRound(r, 6, t.dark ? Rgb(0x2C2C2C) : Rgb(0xF9F9F9));
    c.StrokeRound(r, 6, Alpha(t.text, 0.12f));
    c.Text(s, Format(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), r, t.text);
}

// ---------------------------------------------------------------- the frame
void Paint() {
    D2D1_SIZE_F sz = rt->GetSize();
    W = sz.width;
    H = sz.height;
    c.rt = rt;
    c.br = br;
    hot.clear();
    loader::BeginThumbs();
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(slideshow ? Rgb(0x000000) : t.bg);
    if (settingsOpen) {
        DrawSettings();
    } else if (gridMode) {
        DrawGrid();
    } else {
        D2D1_RECT_F v = LayoutViewport();
        if (compareMode) DrawCompare(v);
        else if (welcome || path.empty()) DrawWelcome();
        else if (editing) {
            Entry* e = Current();
            D2D1_COLOR_F bg = ImageBackground();
            if (bg.a > 0) c.Fill(v, bg);
            if (e && e->ready()) DrawCropOverlay(e);
            AddHot(v, Hit::Viewport, 0);
        } else DrawSingle(v);
        if (!slideshow) {
            float a = chromeAlpha;
            DrawNavArrows(v, a);
            if (g_cfg.filmstrip && !editing && files.size() > 1) DrawFilmstrip(D2D1::RectF(0, v.bottom, v.right, H));
            if (editing) DrawEditPanel(D2D1::RectF(v.right, 0, W, H));
            else if (g_cfg.infoPanel) DrawInfoPanel(D2D1::RectF(v.right, 0, W, H));
            if (!welcome && !path.empty()) DrawTopBar(chromePinned() ? 1.0f : a, false);
            else DrawTopBar(1, false);
        } else {
            DrawSlideControls();
        }
    }
    float bottom = H;
    if (!settingsOpen && !gridMode && !slideshow && g_cfg.filmstrip && !editing && files.size() > 1) bottom = viewRect.bottom;
    DrawUpdatePrompt(bottom);
    DrawToast(slideshow ? H - 90 : bottom);
    if (shortcutsOpen) DrawShortcuts();
    DrawDialog();
    DrawTooltip();
    HRESULT hr = rt->EndDraw();
    gfx::FrameDone();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) UiDeviceLost();
}

} // namespace app
