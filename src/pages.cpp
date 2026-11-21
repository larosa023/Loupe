// Loupe - pages and panels: Settings, the Info panel, the Edit panel (crop handles included),
// the keyboard shortcuts sheet and the welcome screen.
#include "app.h"
#include "util.h"
#include "image.h"
#include "shellops.h"
#include <shellapi.h>
#include <algorithm>
#include <cmath>

using namespace app;
using namespace gfx;

// from ui.cpp
void UiCheckUpdates();
void UiRollback(int i);
void UiSetSlideTimer();
float CompareMul();

namespace app {

// ---------------------------------------------------------------- shared bits
static void PanelHeader(const D2D1_RECT_F& r, const std::wstring& title, float top) {
    c.Text(title, Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 20, top, r.right - 60, top + 44), t.text);
    Button(D2D1::RectF(r.right - 48, top + 6, r.right - 14, top + 38), L"", icon::Cancel, Btn::Subtle, Hit::PanelClose, 0, true, L"Close");
}

static float SectionLabel(const std::wstring& s, float x, float y, float right) {
    c.Text(s, Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x, y, right, y + 18), t.subtext);
    return y + 24;
}

// ---------------------------------------------------------------- info panel
static float InfoRow(float x, float y, float w, const std::wstring& label, const std::wstring& value, Hit link = Hit::None) {
    if (value.empty()) return y;
    c.Text(label, Format(12), D2D1::RectF(x, y, x + w, y + 18), t.subtext);
    auto* vf = Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    float h = std::max(20.0f, MeasureHeight(value, vf, w));
    D2D1_RECT_F vr = D2D1::RectF(x, y + 18, x + w, y + 18 + h);
    bool hv = link != Hit::None && IsHover(link, 0);
    c.Text(value, vf, vr, link != Hit::None ? (hv ? t.accentHover : t.accent) : t.text);
    if (link != Hit::None) AddHot(vr, link, 0);
    return y + 18 + h + 12;
}

static std::wstring Dims(UINT w, UINT h) {
    wchar_t b[96];
    double mp = (double)w * h / 1e6;
    if (mp >= 0.95) swprintf(b, 96, L"%u × %u  (%.1f MP)", w, h, mp);
    else swprintf(b, 96, L"%u × %u", w, h);
    return b;
}

static std::wstring FileTimeText(int64_t ft) {
    if (!ft) return L"";
    FILETIME f{(DWORD)ft, (DWORD)(ft >> 32)}, local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&f, &local);
    FileTimeToSystemTime(&local, &st);
    return FormatTaken(st);
}

void DrawInfoPanel(const D2D1_RECT_F& r) {
    c.Fill(r, t.sidebar);
    c.Fill(D2D1::RectF(r.left, r.top, r.left + 1, r.bottom), t.divider);
    AddHot(r, Hit::None, 0);
    float top = kTopBar;
    PanelHeader(r, L"Info", top);
    float x = r.left + 20, w = r.right - r.left - 40, y = top + 52;
    if (path.empty()) return;
    rt->PushAxisAlignedClip(D2D1::RectF(r.left, top + 48, r.right, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    auto* nf = Format(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    std::wstring name = PathName(path);
    float nh = MeasureHeight(name, nf, w);
    c.Text(name, nf, D2D1::RectF(x, y, x + w, y + nh), t.text);
    y += nh + 4;
    y = InfoRow(x, y, w, L"", PathParent(path), Hit::InfoFolder) + 4;
    Entry* e = Current();
    const FileEntry* fe = nullptr;
    if (index >= 0 && index < (int)files.size()) fe = &files[index];
    auto it = infos.find(path);
    const PhotoInfo* pi = it != infos.end() ? &it->second : nullptr;
    if (e && e->ready()) {
        y = InfoRow(x, y, w, L"Dimensions", Dims(e->fullW, e->fullH));
        std::wstring type = e->format;
        if (e->animated) type += L" · " + std::to_wstring(e->frames.size()) + L" frames" + (e->loops == 0 ? L", loops" : L"");
        y = InfoRow(x, y, w, L"Type", type);
    }
    if (fe) {
        y = InfoRow(x, y, w, L"File size", FormatBytes(fe->size));
        y = InfoRow(x, y, w, L"Modified", FileTimeText(fe->modified));
    }
    if (pi) {
        if (pi->hasTaken) y = InfoRow(x, y, w, L"Date taken", FormatTaken(pi->taken));
        std::wstring cam = pi->model;
        if (!pi->make.empty() && !IStartsWith(pi->model, pi->make.substr(0, pi->make.find(L' ')))) cam = pi->make + L" " + pi->model;
        y = InfoRow(x, y, w, L"Camera", Trim(cam));
        y = InfoRow(x, y, w, L"Lens", pi->lens);
        // exposure triangle, as photographers read it
        std::wstring ex;
        wchar_t b[64];
        auto add = [&](const std::wstring& s) { if (!s.empty()) ex += (ex.empty() ? L"" : L"   ") + s; };
        if (pi->iso) add(L"ISO " + std::to_wstring(pi->iso));
        if (pi->exposureTime > 0) add(FormatExposure(pi->exposureTime));
        if (pi->fNumber > 0) { swprintf(b, 64, L"f/%.1f", pi->fNumber); add(b); }
        if (pi->focal > 0) {
            swprintf(b, 64, L"%.0f mm", pi->focal);
            std::wstring f = b;
            if (pi->focal35 > 0 && std::fabs(pi->focal35 - pi->focal) > 1) f += L" (" + std::to_wstring(pi->focal35) + L" mm equiv.)";
            add(f);
        }
        if (pi->exposureBias != 0) { swprintf(b, 64, L"%+.1f EV", pi->exposureBias); add(b); }
        y = InfoRow(x, y, w, L"Exposure", ex);
        if (pi->flash >= 0) y = InfoRow(x, y, w, L"Flash", (pi->flash & 1) ? L"Fired" : L"Off");
        if (pi->hasGps) {
            swprintf(b, 64, L"%.5f° %lc, %.5f° %lc", std::fabs(pi->lat), pi->lat >= 0 ? L'N' : L'S', std::fabs(pi->lon), pi->lon >= 0 ? L'E' : L'W');
            std::wstring loc = b;
            if (pi->hasAlt) { swprintf(b, 64, L"  ·  %.0f m", pi->alt); loc += b; }
            y = InfoRow(x, y, w, L"Location", loc);
            Button(D2D1::RectF(x, y - 4, x + ButtonWidth(L"Open in maps", icon::MapPin), y + 28), L"Open in maps", icon::MapPin, Btn::Secondary, Hit::InfoMaps, 0, true,
                   L"Opens the spot in Google Maps in your browser");
            y += 44;
        }
        y = InfoRow(x, y, w, L"Software", pi->software);
    } else if (infoPending.count(path)) {
        c.Text(L"Reading details…", Format(12.5f), D2D1::RectF(x, y, x + w, y + 20), t.faint);
        y += 28;
    }
    if (e && e->ready() && e->decodeMs > 0) {
        wchar_t b[64];
        swprintf(b, 64, L"Opened in %.0f ms", e->decodeMs);
        c.Text(b, Format(11.5f), D2D1::RectF(x, y, x + w, y + 18), t.faint);
    }
    rt->PopAxisAlignedClip();
}

// ---------------------------------------------------------------- editing: geometry on screen
static D2D1::Matrix3x2F M(const D2D1_MATRIX_3X2_F& m) { return D2D1::Matrix3x2F(m._11, m._12, m._21, m._22, m._31, m._32); }

static float EditZoom(Entry* e, float& ow, float& oh) {
    OrientedSize(edit, (float)e->fullW, (float)e->fullH, ow, oh);
    D2D1_RECT_F v = viewRect;
    float pad = 48;
    float z = std::min((v.right - v.left - 2 * pad) / ow, (v.bottom - v.top - kTopBar - 2 * pad) / oh);
    return std::max(1e-4f, std::min(z, 4.0f / scale));
}

D2D1_RECT_F EditImageRect(Entry* e) {
    float ow, oh;
    float z = EditZoom(e, ow, oh);
    D2D1_RECT_F v = viewRect;
    float cxx = (v.left + v.right) / 2, cyy = (v.top + kTopBar + v.bottom) / 2;
    return D2D1::RectF(cxx - ow * z / 2, cyy - oh * z / 2, cxx + ow * z / 2, cyy + oh * z / 2);
}

D2D1_MATRIX_3X2_F EditDisplayMatrix(Entry* e) {
    float ow, oh;
    float z = EditZoom(e, ow, oh);
    D2D1_RECT_F r = EditImageRect(e);
    return M(EditMatrix(edit, (float)e->fullW, (float)e->fullH)) * D2D1::Matrix3x2F::Scale(z, z) * D2D1::Matrix3x2F::Translation(r.left, r.top);
}

static D2D1_RECT_F CropScreenRect(Entry* e) {
    D2D1_RECT_F r = EditImageRect(e);
    float w = r.right - r.left, h = r.bottom - r.top;
    return D2D1::RectF(r.left + edit.cropL * w, r.top + edit.cropT * h, r.left + edit.cropR * w, r.top + edit.cropB * h);
}

static void CenterCropWithRatio(Entry* e, float ratio) {
    float ow, oh;
    OrientedSize(edit, (float)e->fullW, (float)e->fullH, ow, oh);
    float cw = ow, ch = ow / ratio;
    if (ch > oh) { ch = oh; cw = oh * ratio; }
    edit.cropL = (ow - cw) / 2 / ow; edit.cropR = 1 - edit.cropL;
    edit.cropT = (oh - ch) / 2 / oh; edit.cropB = 1 - edit.cropT;
    FitCropInside(edit, (float)e->fullW, (float)e->fullH);
}

static float CropRatio(Entry* e) {
    float ow, oh;
    OrientedSize(edit, (float)e->fullW, (float)e->fullH, ow, oh);
    return std::max(0.01f, (edit.cropR - edit.cropL) * ow) / std::max(0.01f, (edit.cropB - edit.cropT) * oh);
}

static float AspectTarget(Entry* e) {
    const AspectPreset& a = kAspects[std::clamp(edit.aspect, 0, kAspectCount - 1)];
    if (a.w == 0) return 0;
    float r = a.w < 0 ? (float)e->fullW / e->fullH : a.w / a.h;
    if (edit.aspectPortrait && r > 1) r = 1 / r;
    if (!edit.aspectPortrait && a.w < 0) {}
    return r;
}

void DrawCropOverlay(Entry* e) {
    D2D1_RECT_F v = viewRect;
    rt->PushAxisAlignedClip(v, D2D1_ANTIALIAS_MODE_ALIASED);
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(M(EditDisplayMatrix(e)) * M(old));
    DrawImage(e, D2D1::RectF(0, 0, (float)e->fullW, (float)e->fullH), 1, true);
    rt->SetTransform(old);
    D2D1_RECT_F cr = CropScreenRect(e);
    // dim what's cropped away
    D2D1_COLOR_F dim = Rgb(0x000000, 0.55f);
    c.Fill(D2D1::RectF(v.left, v.top, v.right, cr.top), dim);
    c.Fill(D2D1::RectF(v.left, cr.bottom, v.right, v.bottom), dim);
    c.Fill(D2D1::RectF(v.left, cr.top, cr.left, cr.bottom), dim);
    c.Fill(D2D1::RectF(cr.right, cr.top, v.right, cr.bottom), dim);
    // thirds while dragging, a finer grid while straightening
    bool dragging = pressed.kind == Hit::CropHandle;
    bool straightening = pressed.kind == Hit::EditSlider && pressed.arg == 0;
    int lines = straightening ? 8 : dragging ? 3 : 0;
    for (int i = 1; i < lines; ++i) {
        float gx = cr.left + (cr.right - cr.left) * i / lines, gy = cr.top + (cr.bottom - cr.top) * i / lines;
        c.Line(gx, cr.top, gx, cr.bottom, Rgb(0xFFFFFF, 0.35f), 1);
        c.Line(cr.left, gy, cr.right, gy, Rgb(0xFFFFFF, 0.35f), 1);
    }
    c.StrokeRound(cr, 0, Rgb(0xFFFFFF, 0.9f), 1.5f);
    // handles: corners 0 2 4 6, edges 1 3 5 7, inside 8
    float hl = 18, th = 4;
    D2D1_COLOR_F hc = Rgb(0xFFFFFF);
    auto corner = [&](float x, float y, float sx, float sy) {
        c.Fill(D2D1::RectF(std::min(x, x + sx * hl), std::min(y, y + sy * th), std::max(x, x + sx * hl), std::max(y, y + sy * th)), hc);
        c.Fill(D2D1::RectF(std::min(x, x + sx * th), std::min(y, y + sy * hl), std::max(x, x + sx * th), std::max(y, y + sy * hl)), hc);
    };
    corner(cr.left - 2, cr.top - 2, 1, 1);
    corner(cr.right + 2, cr.top - 2, -1, 1);
    corner(cr.right + 2, cr.bottom + 2, -1, -1);
    corner(cr.left - 2, cr.bottom + 2, 1, -1);
    float mx = (cr.left + cr.right) / 2, my = (cr.top + cr.bottom) / 2;
    c.Fill(D2D1::RectF(mx - 12, cr.top - 2, mx + 12, cr.top + 2), hc);
    c.Fill(D2D1::RectF(mx - 12, cr.bottom - 2, mx + 12, cr.bottom + 2), hc);
    c.Fill(D2D1::RectF(cr.left - 2, my - 12, cr.left + 2, my + 12), hc);
    c.Fill(D2D1::RectF(cr.right - 2, my - 12, cr.right + 2, my + 12), hc);
    rt->PopAxisAlignedClip();
    float g = 12;
    AddHot(D2D1::RectF(cr.left + g, cr.top + g, cr.right - g, cr.bottom - g), Hit::CropHandle, 8);
    AddHot(D2D1::RectF(cr.left + g, cr.top - g, cr.right - g, cr.top + g), Hit::CropHandle, 1);
    AddHot(D2D1::RectF(cr.right - g, cr.top + g, cr.right + g, cr.bottom - g), Hit::CropHandle, 3);
    AddHot(D2D1::RectF(cr.left + g, cr.bottom - g, cr.right - g, cr.bottom + g), Hit::CropHandle, 5);
    AddHot(D2D1::RectF(cr.left - g, cr.top + g, cr.left + g, cr.bottom - g), Hit::CropHandle, 7);
    AddHot(D2D1::RectF(cr.left - g, cr.top - g, cr.left + g, cr.top + g), Hit::CropHandle, 0);
    AddHot(D2D1::RectF(cr.right - g, cr.top - g, cr.right + g, cr.top + g), Hit::CropHandle, 2);
    AddHot(D2D1::RectF(cr.right - g, cr.bottom - g, cr.right + g, cr.bottom + g), Hit::CropHandle, 4);
    AddHot(D2D1::RectF(cr.left - g, cr.bottom - g, cr.left + g, cr.bottom + g), Hit::CropHandle, 6);
    // output size
    float ow, oh;
    OrientedSize(edit, (float)e->fullW, (float)e->fullH, ow, oh);
    wchar_t b[64];
    swprintf(b, 64, L"%.0f × %.0f", (edit.cropR - edit.cropL) * ow, (edit.cropB - edit.cropT) * oh);
    if (dragging || edit.HasGeometry()) {
        auto* f = Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        float w = MeasureWidth(b, f) + 18;
        D2D1_RECT_F lr = D2D1::RectF(mx - w / 2, cr.bottom + 10, mx + w / 2, cr.bottom + 32);
        if (lr.bottom < v.bottom - 4) { c.FillRound(lr, 11, Rgb(0x000000, 0.6f)); c.Text(b, f, lr, Rgb(0xFFFFFF)); }
    }
}

// crop dragging
static EditParams s_cropStart;
static float s_dragX = 0, s_dragY = 0;
static int s_handle = -1;
static D2D1_RECT_F s_sliderRect[5];

static void ApplyCropDrag(Entry* e, const EditParams& start, int handle, float dxn, float dyn, EditParams& out) {
    out = start;
    float ow, oh;
    OrientedSize(start, (float)e->fullW, (float)e->fullH, ow, oh);
    float L = start.cropL, T = start.cropT, R = start.cropR, B = start.cropB;
    const float minN = 0.04f;
    if (handle == 8) {
        float w = R - L, h = B - T;
        L = std::clamp(L + dxn, 0.0f, 1 - w); T = std::clamp(T + dyn, 0.0f, 1 - h);
        R = L + w; B = T + h;
    } else {
        bool left = handle == 0 || handle == 6 || handle == 7, right = handle == 2 || handle == 3 || handle == 4;
        bool topE = handle == 0 || handle == 1 || handle == 2, bottomE = handle == 4 || handle == 5 || handle == 6;
        if (left) L = std::clamp(L + dxn, 0.0f, R - minN);
        if (right) R = std::clamp(R + dxn, L + minN, 1.0f);
        if (topE) T = std::clamp(T + dyn, 0.0f, B - minN);
        if (bottomE) B = std::clamp(B + dyn, T + minN, 1.0f);
        float ratio = AspectTarget(e);
        if (ratio > 0) {
            // keep the shape: width drives height for corners and side edges, height drives width for top/bottom
            float wpx = (R - L) * ow, hpx = (B - T) * oh;
            if (handle == 1 || handle == 5) {
                float nw = hpx * ratio / ow, cxn = (L + R) / 2;
                L = cxn - nw / 2; R = cxn + nw / 2;
            } else {
                float nh = wpx / ratio / oh;
                if (handle == 3 || handle == 7) { float cyn = (T + B) / 2; T = cyn - nh / 2; B = cyn + nh / 2; }
                else if (topE) T = B - nh;
                else B = T + nh;
            }
        }
    }
    out.cropL = L; out.cropT = T; out.cropR = R; out.cropB = B;
}

void EditDrag(float x, float y) {
    Entry* e = Current();
    if (!e || !e->ready()) return;
    if (pressed.kind == Hit::EditSlider) {
        int id = pressed.arg;
        D2D1_RECT_F r = s_sliderRect[std::clamp(id, 0, 4)];
        float minV, maxV;
        bool centered;
        EditSliderValue(id, &minV, &maxV, &centered);
        float tpos = std::clamp((x - r.left - 8) / std::max(1.0f, r.right - r.left - 16), 0.0f, 1.0f);
        float v = minV + (maxV - minV) * tpos;
        if (std::fabs(v) < (maxV - minV) * 0.012f) v = 0;   // snap to the middle
        SetEditValue(id, v);
        return;
    }
    if (pressed.kind == Hit::CropHandle) {
        float ow, oh;
        float z = EditZoom(e, ow, oh);
        float dxn = (x - s_dragX) / (ow * z), dyn = (y - s_dragY) / (oh * z);
        EditParams cand;
        ApplyCropDrag(e, s_cropStart, s_handle, dxn, dyn, cand);
        if (!CropInside(cand, (float)e->fullW, (float)e->fullH)) {
            // go as far as the straightened image allows
            float lo = 0, hi = 1;
            for (int i = 0; i < 18; ++i) {
                float m = (lo + hi) / 2;
                ApplyCropDrag(e, s_cropStart, s_handle, dxn * m, dyn * m, cand);
                if (CropInside(cand, (float)e->fullW, (float)e->fullH)) lo = m; else hi = m;
            }
            ApplyCropDrag(e, s_cropStart, s_handle, dxn * lo, dyn * lo, cand);
        }
        edit.cropL = cand.cropL; edit.cropT = cand.cropT; edit.cropR = cand.cropR; edit.cropB = cand.cropB;
    }
}

float EditSliderValue(int id, float* minV, float* maxV, bool* centered) {
    *centered = true;
    switch (id) {
        case 0: *minV = -45; *maxV = 45; return edit.straighten;
        case 1: *minV = -2; *maxV = 2; return edit.exposure;
        case 2: *minV = -1; *maxV = 1; return edit.contrast;
        case 3: *minV = -1; *maxV = 1; return edit.saturation;
        default: *minV = -1; *maxV = 1; return edit.warmth;
    }
}

void SetEditValue(int id, float v) {
    Entry* e = Current();
    switch (id) {
        case 0: {
            if (!e) return;
            float ratio = CropRatio(e);
            edit.straighten = std::clamp(v, -45.0f, 45.0f);
            if (std::fabs(edit.straighten) < 0.2f) edit.straighten = 0;
            // straightening zooms the crop in so no empty corners show
            float target = AspectTarget(e);
            CenterCropWithRatio(e, target > 0 ? target : ratio);
            break;
        }
        case 1: edit.exposure = std::clamp(v, -2.0f, 2.0f); break;
        case 2: edit.contrast = std::clamp(v, -1.0f, 1.0f); break;
        case 3: edit.saturation = std::clamp(v, -1.0f, 1.0f); break;
        case 4: edit.warmth = std::clamp(v, -1.0f, 1.0f); break;
    }
    Invalidate();
}

void EditClick(const HotRect& h, float x, float y) {
    Entry* e = Current();
    if (!e || !e->ready()) return;
    switch (h.kind) {
        case Hit::CropHandle:
            s_cropStart = edit;
            s_dragX = x;
            s_dragY = y;
            s_handle = h.arg;
            break;
        case Hit::EditAspect:
            edit.aspect = h.arg;
            if (h.arg == 0) { Invalidate(); break; }
            CenterCropWithRatio(e, AspectTarget(e));
            break;
        case Hit::EditPortrait: {
            edit.aspectPortrait = !edit.aspectPortrait;
            float r = AspectTarget(e);
            if (r <= 0) r = 1 / CropRatio(e);
            CenterCropWithRatio(e, r);
            break;
        }
        case Hit::EditSlider: EditDrag(x, y); break;
        case Hit::EditButton:
            switch (h.arg) {
                case 0: DoCmd(Cmd::RotateLeft); break;
                case 1: DoCmd(Cmd::RotateRight); break;
                case 2: DoCmd(Cmd::FlipH); break;
                case 3: DoCmd(Cmd::FlipV); break;
                case 10: DoCmd(Cmd::Revert); break;
                case 11: DoCmd(Cmd::SaveCopy); break;
                case 12: DoCmd(Cmd::Save); break;
                case 13: DoCmd(Cmd::EditDone); break;
            }
            break;
        default: break;
    }
    Invalidate();
}

void DrawEditPanel(const D2D1_RECT_F& r) {
    c.Fill(r, t.sidebar);
    c.Fill(D2D1::RectF(r.left, r.top, r.left + 1, r.bottom), t.divider);
    AddHot(r, Hit::None, 0);
    float top = kTopBar;
    PanelHeader(r, L"Edit", top);
    float x = r.left + 20, right = r.right - 20, y = top + 54;
    // crop and rotate
    y = SectionLabel(L"CROP & ROTATE", x, y, right);
    float bx = x;
    for (int i = 0; i < kAspectCount; ++i) {
        std::wstring l = kAspects[i].label;
        float w = MeasureWidth(l, Format(12.5f)) + 22;
        if (bx + w > right) { bx = x; y += 34; }
        D2D1_RECT_F br2 = D2D1::RectF(bx, y, bx + w, y + 28);
        bool sel = edit.aspect == i;
        c.FillRound(br2, 14, sel ? Alpha(t.accent, 0.2f) : IsHover(Hit::EditAspect, i) ? t.surfaceHover : t.surface);
        if (sel) c.StrokeRound(br2, 14, t.accent, 1.2f);
        c.Text(l, Format(12.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), br2, t.text);
        AddHot(br2, Hit::EditAspect, i);
        bx += w + 6;
    }
    y += 38;
    // tools: rotate left / right, flip, orientation of the aspect
    struct T { int arg; wchar_t g; const wchar_t* tip; };
    T tools[] = {{0, icon::Rotate, L"Rotate left (Shift+R)"}, {1, icon::Rotate, L"Rotate right (R)"}, {2, 0, L"Flip horizontal (H)"}, {3, 0, L"Flip vertical (V)"}};
    bx = x;
    for (auto& tl : tools) {
        D2D1_RECT_F b = D2D1::RectF(bx, y, bx + 44, y + 36);
        bool hv = IsHover(Hit::EditButton, tl.arg);
        c.FillRound(b, 6, hv ? t.surfaceHover : t.surface);
        auto* gf = Format(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true);
        float mx = (b.left + b.right) / 2, my = (b.top + b.bottom) / 2;
        if (tl.arg == 0) {
            D2D1_MATRIX_3X2_F old;
            rt->GetTransform(&old);
            rt->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1, D2D1::Point2F(mx, 0)) * old);
            c.Glyph(icon::Rotate, gf, b, t.text);
            rt->SetTransform(old);
        } else if (tl.arg == 1) {
            c.Glyph(icon::Rotate, gf, b, t.text);
        } else if (tl.arg == 2) {
            // two triangles mirrored across a vertical line
            c.Line(mx, my - 10, mx, my + 10, Alpha(t.text, 0.6f), 1);
            ID2D1PathGeometry* g = nullptr;
            d2d->CreatePathGeometry(&g);
            ID2D1GeometrySink* s = nullptr;
            if (g && SUCCEEDED(g->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(mx - 3, my - 7), D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(D2D1::Point2F(mx - 11, my + 7)); s->AddLine(D2D1::Point2F(mx - 3, my + 7));
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->BeginFigure(D2D1::Point2F(mx + 3, my - 7), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1::Point2F(mx + 11, my + 7)); s->AddLine(D2D1::Point2F(mx + 3, my + 7));
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                s->Release();
                br->SetColor(t.text);
                rt->FillGeometry(g, br);
                rt->DrawGeometry(g, br, 1.2f);
            }
            if (g) g->Release();
        } else {
            c.Line(mx - 10, my, mx + 10, my, Alpha(t.text, 0.6f), 1);
            ID2D1PathGeometry* g = nullptr;
            d2d->CreatePathGeometry(&g);
            ID2D1GeometrySink* s = nullptr;
            if (g && SUCCEEDED(g->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(mx - 7, my - 3), D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(D2D1::Point2F(mx + 7, my - 11)); s->AddLine(D2D1::Point2F(mx + 7, my - 3));
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->BeginFigure(D2D1::Point2F(mx - 7, my + 3), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1::Point2F(mx + 7, my + 11)); s->AddLine(D2D1::Point2F(mx + 7, my + 3));
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                s->Release();
                br->SetColor(t.text);
                rt->FillGeometry(g, br);
                rt->DrawGeometry(g, br, 1.2f);
            }
            if (g) g->Release();
        }
        AddHot(b, Hit::EditButton, tl.arg, true, tl.tip);
        bx += 50;
    }
    // portrait / landscape for the aspect
    {
        D2D1_RECT_F b = D2D1::RectF(bx, y, bx + 44, y + 36);
        c.FillRound(b, 6, IsHover(Hit::EditPortrait, 0) ? t.surfaceHover : t.surface);
        float mx = (b.left + b.right) / 2, my = (b.top + b.bottom) / 2;
        if (edit.aspectPortrait) c.StrokeRound(D2D1::RectF(mx - 6, my - 9, mx + 6, my + 9), 2, t.text, 1.4f);
        else c.StrokeRound(D2D1::RectF(mx - 9, my - 6, mx + 9, my + 6), 2, t.text, 1.4f);
        AddHot(b, Hit::EditPortrait, 0, true, edit.aspectPortrait ? L"Portrait crop (click for landscape)" : L"Landscape crop (click for portrait)");
    }
    y += 50;
    auto slider = [&](int id, const std::wstring& label, const wchar_t* fmt, float mul) {
        float minV, maxV;
        bool centered;
        float v = EditSliderValue(id, &minV, &maxV, &centered);
        wchar_t b[32];
        swprintf(b, 32, fmt, v * mul);
        c.Text(label, Format(13), D2D1::RectF(x, y, right - 60, y + 20), t.text);
        c.Text(v == 0 ? std::wstring(L"0") : std::wstring(b), Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
               D2D1::RectF(right - 70, y, right, y + 20), t.subtext);
        D2D1_RECT_F sr = D2D1::RectF(x - 6, y + 20, right + 6, y + 48);
        s_sliderRect[id] = sr;
        Slider(sr, v, minV, maxV, Hit::EditSlider, id, centered);
        y += 58;
    };
    slider(0, L"Straighten", L"%+.1f°", 1);
    y += 6;
    y = SectionLabel(L"ADJUST", x, y, right);
    slider(1, L"Exposure", L"%+.2f", 1);
    slider(2, L"Contrast", L"%+.0f", 100);
    slider(3, L"Saturation", L"%+.0f", 100);
    slider(4, L"Warmth", L"%+.0f", 100);
    c.Text(L"Double-click a slider to reset it.", Format(11.5f), D2D1::RectF(x, y - 4, right, y + 14), t.faint);
    // footer
    float fy = std::max(y + 24, r.bottom - 120);
    float bw = (right - x - 8) / 2;
    Button(D2D1::RectF(x, fy, x + bw, fy + 34), L"Revert", icon::Undo, Btn::Secondary, Hit::EditButton, 10, !edit.IsIdentity(), L"Undo every change");
    Button(D2D1::RectF(x + bw + 8, fy, right, fy + 34), L"Save a copy", 0, Btn::Secondary, Hit::EditButton, 11, true, L"Save as a new file (Ctrl+Shift+S)");
    Button(D2D1::RectF(x, fy + 42, x + bw, fy + 76), L"Done", 0, Btn::Secondary, Hit::EditButton, 13, true, L"Leave editing (Enter)");
    Button(D2D1::RectF(x + bw + 8, fy + 42, right, fy + 76), L"Save", icon::Save, Btn::Primary, Hit::EditButton, 12, !edit.IsIdentity(), L"Save over the original, after asking (Ctrl+S)");
}

// ---------------------------------------------------------------- settings
enum SegId { SEG_BG = 1, SEG_WHEEL, SEG_SORT, SEG_CHANNEL };
enum ToggleId { TG_AUTOHIDE = 1, TG_ENLARGE, TG_SMOOTH, TG_LOOP, TG_SINGLE, TG_DESC, TG_SHUFFLE, TG_CONFIRM, TG_AUTOUPDATE };
enum ButtonId { BT_THEMEFOLDER = 1, BT_RELOAD, BT_DEFAULTAPPS, BT_REGISTER, BT_CHECK, BT_SHORTCUTS, BT_DATA, BT_REPO, BT_RESTART, BT_ROLLBACK = 20 };
enum StepId { ST_SLIDE = 1, ST_QUALITY };

static std::vector<ThemeDef> SettingsThemes() {
    std::vector<ThemeDef> all = BuiltinThemes();
    std::wstring hid, hacc;
    if (ReadHangarTheme(hid, hacc)) {
        ThemeDef h = FindTheme(hid, customThemes);
        h.id = L"hangar";
        h.name = L"Match Hangar";
        h.description = L"Follows Hangar's theme and accent";
        h.custom = false;
        all.insert(all.begin(), h);
    }
    all.insert(all.end(), customThemes.begin(), customThemes.end());
    return all;
}

static std::wstring HexColor(uint32_t col) {
    wchar_t b[16];
    swprintf(b, 16, L"#%06X", col & 0xFFFFFF);
    return b;
}

// a miniature Loupe window in the theme's colors
static void ThemeCard(const ThemeDef& def0, const D2D1_RECT_F& r, int idx, bool selected) {
    ThemeDef def = def0;
    if (def.followWindows) def = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
    Theme pt = MakeTheme(def, selected ? g_cfg.accent : L"theme");
    bool hv = IsHover(Hit::SetTheme, idx);
    c.FillRound(r, 10, hv ? t.surfaceHover : t.surface);
    D2D1_RECT_F pv = D2D1::RectF(r.left + 10, r.top + 10, r.right - 10, r.bottom - 48);
    ID2D1RoundedRectangleGeometry* clip = nullptr;
    d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(pv, 6, 6), &clip);
    ID2D1Layer* layer = nullptr;
    rt->CreateLayer(&layer);
    if (clip && layer) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
    c.Fill(pv, Rgb(def.bg));
    float w = pv.right - pv.left, h = pv.bottom - pv.top;
    // top bar
    D2D1_RECT_F bar = D2D1::RectF(pv.left, pv.top, pv.right, pv.top + h * 0.16f);
    c.Fill(bar, Rgb(def.surface, def.surfaceOpacity < 1 ? 0.3f : 1.0f));
    c.FillRound(D2D1::RectF(pv.left + 6, bar.top + 4, pv.left + 6 + w * 0.25f, bar.bottom - 4), 1.5f, pt.text);
    for (int k = 0; k < 3; ++k) c.Circle(pv.right - 8 - k * 9, (bar.top + bar.bottom) / 2, 2, pt.subtext);
    // a "photo": sky, sun, hills
    D2D1_RECT_F ph = D2D1::RectF(pv.left + w * 0.18f, bar.bottom + h * 0.08f, pv.right - w * 0.18f, pv.bottom - h * 0.24f);
    c.FillRound(ph, 2, Rgb(0x5B8DEF));
    c.Circle(ph.left + (ph.right - ph.left) * 0.72f, ph.top + (ph.bottom - ph.top) * 0.35f, (ph.bottom - ph.top) * 0.13f, Rgb(0xFFD36E));
    c.Fill(D2D1::RectF(ph.left, ph.top + (ph.bottom - ph.top) * 0.68f, ph.right, ph.bottom), Rgb(0x3F8F5A));
    // filmstrip with the selected thumb in accent
    float fy = pv.bottom - h * 0.17f;
    for (int k = 0; k < 6; ++k) {
        float tx = pv.left + w * 0.14f + k * w * 0.125f;
        D2D1_RECT_F tr = D2D1::RectF(tx, fy, tx + w * 0.1f, fy + h * 0.12f);
        c.FillRound(tr, 1.5f, k == 2 ? pt.accent : Rgb(def.surfaceStrong));
    }
    if (layer && clip) rt->PopLayer();
    if (clip) clip->Release();
    if (layer) layer->Release();
    c.StrokeRound(pv, 6, Alpha(t.text, 0.10f));
    c.Text(def0.name, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 12, r.bottom - 44, r.right - 12, r.bottom - 24), t.text);
    c.Text(def0.description, Format(11.5f), D2D1::RectF(r.left + 12, r.bottom - 25, r.right - 12, r.bottom - 8), t.subtext);
    if (selected) {
        c.StrokeRound(r, 10, t.accent, 2);
        c.Circle(r.right - 20, r.top + 20, 9, t.accent);
        c.Glyph(icon::Check, Format(9, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.right - 29, r.top + 11, r.right - 11, r.top + 29), t.onAccent);
    } else if (hv) {
        c.StrokeRound(r, 10, Alpha(t.accent, 0.5f));
    }
    AddHot(r, Hit::SetTheme, idx);
}

static void Stepper(float x, float y, const std::wstring& value, int id) {
    D2D1_RECT_F a = D2D1::RectF(x, y, x + 32, y + 32), b = D2D1::RectF(x + 104, y, x + 136, y + 32);
    Button(a, L"", icon::Remove, Btn::Secondary, Hit::SetStep, id * 2, true);
    c.Text(value, Format(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(x + 32, y, x + 104, y + 32), t.text);
    Button(b, L"", icon::Add, Btn::Secondary, Hit::SetStep, id * 2 + 1, true);
}

void DrawSettings() {
    float pad = 32;
    float left = pad, right = std::min(W - pad, pad + 860.0f);
    if (W > 860 + 2 * pad) { left = (W - 860) / 2; right = left + 860; }
    float viewTop = kTopBar;
    rt->PushAxisAlignedClip(D2D1::RectF(0, viewTop, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = viewTop + 18 - settingsScroll;
    c.Text(L"Settings", Format(24, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 36), t.text);
    y += 54;
    auto section = [&](const std::wstring& title) {
        c.Text(title, Format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 22), t.text);
        y += 32;
    };
    auto card = [&](float h) {
        D2D1_RECT_F r = D2D1::RectF(left, y, right, y + h);
        c.FillRound(r, 10, t.surface);
        return r;
    };
    auto row = [&](float ry, const std::wstring& label, const std::wstring& sub, float labelRight) {
        c.Text(label, Format(14), D2D1::RectF(left + 18, ry + (sub.empty() ? 18 : 10), labelRight, ry + (sub.empty() ? 46 : 30)), t.text);
        if (!sub.empty()) c.Text(sub, Format(12), D2D1::RectF(left + 18, ry + 30, labelRight, ry + 48), t.subtext);
    };
    auto divider = [&](float ry) { c.Line(left + 18, ry, right - 18, ry, t.divider); };
    const float RH = 64;

    // ---- appearance
    section(L"Appearance");
    {
        auto all = SettingsThemes();
        float gap = 12, minW = 180, avail = right - left;
        int cols = std::max(1, (int)((avail + gap) / (minW + gap)));
        float cw = (avail - gap * (cols - 1)) / cols, ch = std::min(160.0f, cw * 0.55f + 48);
        for (size_t k = 0; k < all.size(); ++k) {
            int col = (int)(k % cols), rowi = (int)(k / cols);
            D2D1_RECT_F r = D2D1::RectF(left + col * (cw + gap), y + rowi * (ch + gap), left + col * (cw + gap) + cw, y + rowi * (ch + gap) + ch);
            if (r.bottom > viewTop && r.top < H) ThemeCard(all[k], r, (int)k, all[k].id == g_cfg.theme);
        }
        y += ((int)all.size() + cols - 1) / cols * (ch + gap) + 4;
        float bx = right;
        auto btn = [&](const std::wstring& l, wchar_t g, int id, const wchar_t* tip) {
            float w = ButtonWidth(l, g);
            Button(D2D1::RectF(bx - w, y, bx, y + 32), l, g, Btn::Secondary, Hit::SetButton, id, true, tip);
            bx -= w + 8;
        };
        btn(L"Reload", icon::Refresh, BT_RELOAD, L"Load theme files again");
        btn(L"Theme files", icon::FolderOpen, BT_THEMEFOLDER, L"Hangar-format .json themes go here");
        c.Text(L"Same themes as Hangar and the rest of the suite. Theme files from Hangar's themes folder appear here too.", Format(12),
               D2D1::RectF(left, y, bx - 12, y + 32), t.subtext);
        y += 48;
    }
    {
        D2D1_RECT_F r = card(RH * 3 + 62);
        // accent
        c.Text(L"Accent color", Format(14), D2D1::RectF(left + 18, r.top + 14, right, r.top + 34), t.text);
        float ax = left + 18, ay = r.top + 42;
        ThemeDef cur = FindTheme(g_cfg.theme == L"hangar" ? L"dark" : g_cfg.theme, customThemes);
        auto chip = [&](const std::wstring& label, uint32_t color, int arg, bool sel) {
            auto* f = Format(12.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
            float w = MeasureWidth(label, Format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 40;
            D2D1_RECT_F cr = D2D1::RectF(ax, ay, ax + w, ay + 32);
            c.FillRound(cr, 16, sel ? Alpha(t.accent, 0.18f) : IsHover(Hit::SetAccent, arg) ? t.surfaceHover : Mix(t.surface, t.surfaceStrong, 0.4f));
            if (sel) c.StrokeRound(cr, 16, t.accent, 1.5f);
            c.Circle(cr.left + 16, ay + 16, 7, Rgb(color));
            c.Text(label, f, D2D1::RectF(cr.left + 29, ay, cr.right - 6, ay + 32), t.text);
            AddHot(cr, Hit::SetAccent, arg);
            ax += w + 8;
        };
        bool hangar = g_cfg.theme == L"hangar";
        chip(L"Theme's own", ResolveAccent(cur, L"theme"), 0, g_cfg.accent == L"theme" && !hangar);
        chip(L"Windows", SystemAccent(), 1, g_cfg.accent == L"windows" && !hangar);
        for (int k = 0; k < kAccentSwatchCount; ++k) {
            bool sel = !hangar && IEquals(g_cfg.accent, HexColor(kAccentSwatches[k]));
            float cxx = ax + 16;
            if (cxx + 16 > right - 250) break;
            if (sel) c.Circle(cxx, ay + 16, 15, t.text);
            else if (IsHover(Hit::SetAccent, 2 + k)) c.Circle(cxx, ay + 16, 15, Alpha(t.text, 0.3f));
            c.Circle(cxx, ay + 16, sel ? 12.5f : 13, Rgb(kAccentSwatches[k]));
            AddHot(D2D1::RectF(ax, ay, ax + 32, ay + 32), Hit::SetAccent, 2 + k);
            ax += 36;
        }
        // custom hex
        float fx = std::max(ax + 8, right - 236);
        if (!accentField.active && accentField.text.empty()) accentField.text = g_cfg.accent.size() == 7 && g_cfg.accent[0] == L'#' ? g_cfg.accent : L"";
        DrawTextField(D2D1::RectF(fx, ay, fx + 120, ay + 32), accentField, Hit::AccentField, L"#RRGGBB");
        Button(D2D1::RectF(fx + 128, ay, right - 18, ay + 32), L"Use", 0, Btn::Secondary, Hit::AccentField, 1);
        if (hangar) c.Text(L"Match Hangar uses Hangar's accent.", Format(11.5f), D2D1::RectF(left + 18, ay + 34, right, ay + 52), t.faint);
        float ry = r.top + 62 + 14;
        divider(ry);
        row(ry, L"Image background", L"Behind the photo; the checkerboard shows transparency", right - 380);
        Segmented(right - 18 - 340, ry + 16, {L"Theme", L"Black", L"Gray", L"Checkerboard"}, (int)g_cfg.imageBg, Hit::SetSeg, SEG_BG * 100);
        ry += RH;
        divider(ry);
        row(ry, L"Hide the toolbar while viewing", L"It comes back when you move the mouse", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.autoHideChrome, Hit::SetToggle, TG_AUTOHIDE);
        ry += RH;
        divider(ry);
        row(ry, L"Smooth zoom", L"Animate zooming and fitting", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.smoothZoom, Hit::SetToggle, TG_SMOOTH);
        y = r.bottom + 28;
    }

    // ---- viewing
    section(L"Viewing");
    {
        D2D1_RECT_F r = card(RH * 6);
        float ry = r.top;
        row(ry, L"Mouse wheel", g_cfg.wheelZooms ? L"Zooms at the cursor. Ctrl+wheel flips." : L"Flips through images. Ctrl+wheel zooms at the cursor.", right - 300);
        Segmented(right - 18 - 236, ry + 16, {L"Flips images", L"Zooms"}, g_cfg.wheelZooms ? 1 : 0, Hit::SetSeg, SEG_WHEEL * 100);
        ry += RH; divider(ry);
        row(ry, L"Sort images by", L"", right - 520);
        static const wchar_t* sortNames[] = {L"Name", L"Date taken", L"Date modified", L"Size"};
        Segmented(right - 18 - 470, ry + 16, {sortNames[0], sortNames[1], sortNames[2], sortNames[3]}, (int)g_cfg.sort, Hit::SetSeg, SEG_SORT * 100);
        ry += RH; divider(ry);
        row(ry, L"Newest or largest first", L"Reverse the sort order", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.sortDescending, Hit::SetToggle, TG_DESC);
        ry += RH; divider(ry);
        row(ry, L"Enlarge small images to fit the window", L"Off: small images show at their real size", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.enlargeSmall, Hit::SetToggle, TG_ENLARGE);
        ry += RH; divider(ry);
        row(ry, L"Wrap around at the end of the folder", L"Next on the last image goes back to the first", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.loopFolder, Hit::SetToggle, TG_LOOP);
        ry += RH; divider(ry);
        row(ry, L"Open images in the same window", L"Off: every image you open gets its own window", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.singleWindow, Hit::SetToggle, TG_SINGLE);
        y = r.bottom + 28;
    }

    // ---- slideshow
    section(L"Slideshow");
    {
        D2D1_RECT_F r = card(RH * 2);
        float ry = r.top;
        row(ry, L"Time per image", L"Space starts and pauses; ↑ ↓ change it while it runs", right - 180);
        Stepper(right - 18 - 136, ry + 16, std::to_wstring(g_cfg.slideshowSeconds) + L" s", ST_SLIDE);
        ry += RH; divider(ry);
        row(ry, L"Shuffle", L"Random order", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.slideshowShuffle, Hit::SetToggle, TG_SHUFFLE);
        y = r.bottom + 28;
    }

    // ---- files
    section(L"Files");
    {
        D2D1_RECT_F r = card(RH * 3);
        float ry = r.top;
        bool reg = shell::FileTypesRegistered();
        row(ry, L"Default photo viewer",
            IsPortable() ? L"Portable mode: register file types to make Loupe a choice in Windows"
                         : (reg ? L"Loupe is listed for JPEG, PNG, GIF, WebP, HEIC, RAW and more. Pick it in Windows' Default apps."
                                : L"Register Loupe's file types so Windows offers it"),
            right - 300);
        float bx = right - 18;
        float w1 = ButtonWidth(L"Default apps…", 0);
        Button(D2D1::RectF(bx - w1, ry + 16, bx, ry + 48), L"Default apps…", 0, Btn::Primary, Hit::SetButton, BT_DEFAULTAPPS, true, L"Opens Windows Settings at Loupe's page");
        bx -= w1 + 8;
        if (!reg) {
            float w2 = ButtonWidth(L"Register", 0);
            Button(D2D1::RectF(bx - w2, ry + 16, bx, ry + 48), L"Register", 0, Btn::Secondary, Hit::SetButton, BT_REGISTER);
        }
        ry += RH; divider(ry);
        row(ry, L"Ask before deleting", L"Deleted images go to the Recycle Bin; Ctrl+Z brings them back", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.confirmDelete, Hit::SetToggle, TG_CONFIRM);
        ry += RH; divider(ry);
        row(ry, L"JPEG quality when saving edits", L"Higher keeps more detail and makes bigger files", right - 180);
        Stepper(right - 18 - 136, ry + 16, std::to_wstring(g_cfg.jpegQuality), ST_QUALITY);
        y = r.bottom + 28;
    }

    // ---- updates
    section(L"Updates");
    {
        bool hasPrev = !prevVersions.empty();
        D2D1_RECT_F r = card(RH * 3 + (hasPrev ? RH : 0));
        float ry = r.top;
        std::wstring status;
        switch (update.kind) {
            case UpdateState::Checking: status = L"Checking GitHub…"; break;
            case UpdateState::UpToDate: status = L"You're up to date"; break;
            case UpdateState::Downloading: status = L"Downloading " + update.version + (update.percent >= 0 ? L" · " + std::to_wstring(update.percent) + L"%" : L""); break;
            case UpdateState::Ready: status = L"Loupe " + update.version + L" is downloaded and verified"; break;
            case UpdateState::Error: status = update.message; break;
            default: status = g_cfg.lastUpdateCheck ? L"Updates come from GitHub Releases and are checked with SHA-256" : L"Not checked yet"; break;
        }
        row(ry, std::wstring(L"Loupe ") + LOUPE_VERSION + (IsPortable() ? L" (portable)" : L""), status, right - 240);
        if (update.kind == UpdateState::Ready)
            Button(D2D1::RectF(right - 200, ry + 16, right - 18, ry + 48), L"Restart to update", icon::Update, Btn::Primary, Hit::SetButton, BT_RESTART);
        else
            Button(D2D1::RectF(right - 220, ry + 16, right - 18, ry + 48), L"Check for updates now", icon::Refresh, Btn::Secondary, Hit::SetButton, BT_CHECK,
                   update.kind != UpdateState::Checking && update.kind != UpdateState::Downloading);
        ry += RH; divider(ry);
        row(ry, L"Update automatically", L"Checks on launch and every few hours, downloads in the background, installs on restart", right - 80);
        Toggle(right - 60, ry + 21, g_cfg.autoUpdate, Hit::SetToggle, TG_AUTOUPDATE);
        ry += RH; divider(ry);
        row(ry, L"Channel", L"Beta gets new features first (versions with a \"-\")", right - 200);
        Segmented(right - 18 - 150, ry + 16, {L"Stable", L"Beta"}, g_cfg.beta ? 1 : 0, Hit::SetSeg, SEG_CHANNEL * 100);
        if (hasPrev) {
            ry += RH; divider(ry);
            row(ry, L"Previous versions", L"Go back if a new version gives you trouble", right - 360);
            float bx = right - 18;
            for (size_t k = 0; k < prevVersions.size() && k < 2; ++k) {
                std::wstring l = L"Roll back to " + prevVersions[k].version;
                float w = ButtonWidth(l, 0);
                Button(D2D1::RectF(bx - w, ry + 16, bx, ry + 48), l, 0, Btn::Secondary, Hit::SetButton, BT_ROLLBACK + (int)k);
                bx -= w + 8;
            }
        }
        y = r.bottom + 28;
    }

    // ---- about
    section(L"About");
    {
        D2D1_RECT_F r = card(RH * 2);
        float ry = r.top;
        row(ry, L"Keyboard shortcuts", L"F1 shows them anywhere", right - 200);
        Button(D2D1::RectF(right - 170, ry + 16, right - 18, ry + 48), L"Show shortcuts", icon::Keyboard, Btn::Secondary, Hit::SetButton, BT_SHORTCUTS);
        ry += RH; divider(ry);
        row(ry, L"Loupe · larosa023", DataDir(), right - 420);
        float bx = right - 18;
        float w1 = ButtonWidth(L"github.com/larosa023/Loupe", 0);
        Button(D2D1::RectF(bx - w1, ry + 16, bx, ry + 48), L"github.com/larosa023/Loupe", 0, Btn::Subtle, Hit::SetButton, BT_REPO);
        bx -= w1 + 8;
        float w2 = ButtonWidth(L"Data folder", icon::Folder);
        Button(D2D1::RectF(bx - w2, ry + 16, bx, ry + 48), L"Data folder", icon::Folder, Btn::Secondary, Hit::SetButton, BT_DATA, true, L"Settings, log and themes");
        y = r.bottom + 28;
    }
    c.Text(L"No accounts, no telemetry. Loupe only talks to GitHub to check for updates.", Format(12), D2D1::RectF(left, y, right, y + 20), t.faint);
    y += 40;
    settingsContentH = y + settingsScroll - viewTop;
    rt->PopAxisAlignedClip();
    DrawScrollbar(D2D1::RectF(0, viewTop + 4, W, H - 4), settingsScroll, settingsContentH, 2);
    // top bar
    D2D1_RECT_F bar = D2D1::RectF(0, 0, W, kTopBar);
    AddHot(bar, Hit::Caption, 0);
    Button(D2D1::RectF(8, 6, 44, kTopBar - 6), L"", icon::Back, Btn::Chrome, Hit::SetBack, 0, true, L"Back (Esc)");
    c.Text(L"Settings", Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(54, 0, 300, kTopBar), t.text);
    if (!fullscreen) DrawCaptionButtons(W, 0, kTopBar);
}

void SettingsClick(const HotRect& h, float, float) {
    switch (h.kind) {
        case Hit::SetBack: settingsOpen = false; break;
        case Hit::SetTheme: {
            auto all = SettingsThemes();
            if (h.arg >= 0 && h.arg < (int)all.size()) {
                g_cfg.theme = all[h.arg].id;
                SaveSettings();
                ApplyTheme();
            }
            break;
        }
        case Hit::SetAccent:
            if (g_cfg.theme == L"hangar") { Toast(L"Pick another theme to choose your own accent"); break; }
            if (h.arg == 0) g_cfg.accent = L"theme";
            else if (h.arg == 1) g_cfg.accent = L"windows";
            else if (h.arg >= 2 && h.arg < 2 + kAccentSwatchCount) g_cfg.accent = HexColor(kAccentSwatches[h.arg - 2]);
            accentField.text.clear();
            SaveSettings();
            ApplyTheme();
            break;
        case Hit::AccentField:
            if (h.arg == 0) { accentField.active = true; accentField.anchor = 0; accentField.caret = (int)accentField.text.size(); }
            else {
                std::wstring s = Trim(accentField.text);
                if (!s.empty() && s[0] != L'#') s = L"#" + s;
                bool ok = s.size() == 7 && s.find_first_not_of(L"#0123456789abcdefABCDEF", 1) == std::wstring::npos;
                if (!ok) { Toast(L"Type a color like #4F8CFF"); break; }
                if (g_cfg.theme == L"hangar") { Toast(L"Pick another theme to choose your own accent"); break; }
                CharUpperBuffW(&s[1], 6);
                g_cfg.accent = s;
                accentField.text = s;
                accentField.active = false;
                SaveSettings();
                ApplyTheme();
            }
            break;
        case Hit::SetSeg: {
            int id = h.arg / 100, v = h.arg % 100;
            if (id == SEG_BG) g_cfg.imageBg = (ImageBg)v;
            else if (id == SEG_WHEEL) g_cfg.wheelZooms = v == 1;
            else if (id == SEG_SORT) { DoCmd(v == 0 ? Cmd::SortName : v == 1 ? Cmd::SortTaken : v == 2 ? Cmd::SortModified : Cmd::SortSize); return; }
            else if (id == SEG_CHANNEL) { bool b = v == 1; if (b != g_cfg.beta) { g_cfg.beta = b; SaveSettings(); UiCheckUpdates(); } }
            SaveSettings();
            break;
        }
        case Hit::SetToggle:
            switch (h.arg) {
                case TG_AUTOHIDE: g_cfg.autoHideChrome = !g_cfg.autoHideChrome; break;
                case TG_ENLARGE: g_cfg.enlargeSmall = !g_cfg.enlargeSmall; break;
                case TG_SMOOTH: g_cfg.smoothZoom = !g_cfg.smoothZoom; break;
                case TG_LOOP: g_cfg.loopFolder = !g_cfg.loopFolder; break;
                case TG_SINGLE: g_cfg.singleWindow = !g_cfg.singleWindow; break;
                case TG_DESC: DoCmd(Cmd::SortReverse); return;
                case TG_SHUFFLE: g_cfg.slideshowShuffle = !g_cfg.slideshowShuffle; break;
                case TG_CONFIRM: g_cfg.confirmDelete = !g_cfg.confirmDelete; break;
                case TG_AUTOUPDATE: g_cfg.autoUpdate = !g_cfg.autoUpdate; if (g_cfg.autoUpdate) UiCheckUpdates(); break;
            }
            SaveSettings();
            break;
        case Hit::SetStep: {
            int id = h.arg / 2;
            bool plus = h.arg % 2 == 1;
            if (id == ST_SLIDE) { g_cfg.slideshowSeconds = std::clamp(g_cfg.slideshowSeconds + (plus ? 1 : -1), 1, 60); UiSetSlideTimer(); }
            else if (id == ST_QUALITY) g_cfg.jpegQuality = std::clamp(g_cfg.jpegQuality + (plus ? 5 : -5), 50, 100);
            SaveSettings();
            break;
        }
        case Hit::SetButton:
            switch (h.arg) {
                case BT_THEMEFOLDER: EnsureDir(ThemesDir()); ShellExecuteW(hwnd, L"open", ThemesDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
                case BT_RELOAD: customThemes = LoadCustomThemes(); ApplyTheme(); Toast(L"Loaded " + std::to_wstring(customThemes.size()) + L" theme file" + (customThemes.size() == 1 ? L"" : L"s")); break;
                case BT_DEFAULTAPPS: if (!shell::FileTypesRegistered()) shell::RegisterFileTypes(); shell::OpenDefaultAppsSettings(); break;
                case BT_REGISTER: shell::RegisterFileTypes(); g_cfg.registeredExe = ExePath(); SaveSettings(); Toast(L"Registered. Loupe now shows up in Open with and Default apps."); break;
                case BT_CHECK: UiCheckUpdates(); break;
                case BT_RESTART: { HotRect r{{0, 0, 0, 0}, Hit::UpdateRestart, 0, true, nullptr}; Click(r, 0, 0); break; }
                case BT_SHORTCUTS: shortcutsOpen = true; break;
                case BT_DATA: EnsureDir(DataDir()); ShellExecuteW(hwnd, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
                case BT_REPO: shell::OpenUrl(L"https://github.com/larosa023/Loupe"); break;
                default: if (h.arg >= BT_ROLLBACK) UiRollback(h.arg - BT_ROLLBACK); break;
            }
            break;
        default: break;
    }
    Invalidate();
}

// ---------------------------------------------------------------- shortcuts
void DrawShortcuts() {
    c.Fill(D2D1::RectF(0, 0, W, H), Rgb(0x000000, 0.5f));
    AddHot(D2D1::RectF(0, 0, W, H), Hit::Shortcuts, 0);
    static const wchar_t* items[][2] = {
        {L"← →  PgUp PgDn", L"Previous / next image"}, {L"Home  End", L"First / last image"},
        {L"Wheel", L"Flip (or zoom, in Settings)"}, {L"Ctrl+wheel", L"Zoom at the cursor"},
        {L"+  −  ↑  ↓", L"Zoom in / out"}, {L"0  /  1", L"Fit / actual size"},
        {L"Double-click", L"Toggle fit and zoom"}, {L"Drag", L"Pan when zoomed in"},
        {L"F", L"Filmstrip"}, {L"G", L"All images (grid)"},
        {L"I", L"Info"}, {L"C", L"Compare two images"},
        {L"E", L"Edit: crop, straighten, adjust"}, {L"R  Shift+R", L"Rotate right / left"},
        {L"H  V", L"Flip horizontal / vertical"}, {L"Ctrl+S", L"Save (asks first)"},
        {L"Ctrl+Shift+S", L"Save a copy"}, {L"Ctrl+Z", L"Undo (delete, save, rename, edit)"},
        {L"Space", L"Slideshow / pause"}, {L"F11", L"Full screen"},
        {L"P  ,  .", L"Play/pause animation, step frames"}, {L"Del", L"Delete to Recycle Bin"},
        {L"F2", L"Rename"}, {L"Ctrl+C", L"Copy image"},
        {L"Ctrl+Shift+C", L"Copy path"}, {L"Ctrl+E", L"Show in File Explorer"},
        {L"Ctrl+Shift+E", L"Open folder in Strata"}, {L"Ctrl+B", L"Set as desktop background"},
        {L"Ctrl+O", L"Open"}, {L"S", L"Sort order"},
        {L"Ctrl+,", L"Settings"}, {L"Esc", L"Back out of a mode"},
    };
    int n = sizeof(items) / sizeof(items[0]);
    int rows = (n + 1) / 2;
    float w = std::min(820.0f, W - 40), rh = 30, h = 70 + rows * rh + 20;
    D2D1_RECT_F r = D2D1::RectF((W - w) / 2, std::max(20.0f, (H - h) / 2), (W + w) / 2, std::max(20.0f, (H - h) / 2) + h);
    c.FillRound(r, 12, t.dark ? Mix(t.surface, Rgb(0xFFFFFF), 0.03f) : Rgb(0xFFFFFF));
    c.Text(L"Keyboard shortcuts", Format(18, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 26, r.top + 18, r.right, r.top + 46), t.text);
    float colW = (w - 52) / 2;
    for (int i = 0; i < n; ++i) {
        int col = i % 2, rowi = i / 2;
        float x = r.left + 26 + col * colW, y = r.top + 62 + rowi * rh;
        auto* kf = Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float kw = MeasureWidth(items[i][0], kf) + 14;
        c.FillRound(D2D1::RectF(x, y + 3, x + kw, y + 25), 5, t.surfaceStrong);
        c.Text(items[i][0], Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(x, y + 3, x + kw, y + 25), t.text);
        c.Text(items[i][1], Format(12.5f), D2D1::RectF(x + std::max(kw, 120.0f) + 10, y, x + colW - 8, y + 28), t.subtext);
    }
}

// ---------------------------------------------------------------- welcome
void DrawWelcome() {
    D2D1_RECT_F v = viewRect;
    float mx = (v.left + v.right) / 2, my = (v.top + v.bottom) / 2 - 30;
    // a loupe: lens over a little landscape
    c.Circle(mx, my - 70, 44, Alpha(t.accent, 0.14f));
    br->SetColor(t.accent);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mx - 6, my - 76), 26, 26), br, 5);
    rt->DrawLine(D2D1::Point2F(mx + 13, my - 57), D2D1::Point2F(mx + 30, my - 40), br, 8);
    c.Text(L"Loupe", Format(26, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(v.left, my - 10, v.right, my + 28), t.text);
    c.Text(L"Open an image, or drop one here.", Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(v.left, my + 30, v.right, my + 52), t.subtext);
    float y = my + 76;
    float w1 = ButtonWidth(L"Open image", icon::OpenFile) + 10;
    Button(D2D1::RectF(mx - w1 / 2, y, mx + w1 / 2, y + 36), L"Open image", icon::OpenFile, Btn::Primary, Hit::Welcome, 0, true, L"Ctrl+O");
    y += 48;
    std::wstring l2 = L"Make Loupe your photo viewer";
    float w2 = ButtonWidth(l2, 0);
    Button(D2D1::RectF(mx - w2 / 2, y, mx + w2 / 2, y + 34), l2, 0, Btn::Secondary, Hit::Welcome, 1, true, L"Opens Windows Default apps at Loupe");
    y += 44;
    float w3 = ButtonWidth(L"Settings", icon::Settings);
    Button(D2D1::RectF(mx - w3 / 2, y, mx + w3 / 2, y + 32), L"Settings", icon::Settings, Btn::Subtle, Hit::Welcome, 2);
    c.Text(L"F1 shows every keyboard shortcut", Format(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(v.left, v.bottom - 40, v.right, v.bottom - 16), t.faint);
}

} // namespace app
