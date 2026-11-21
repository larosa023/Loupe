// Loupe - quick edits (see edit.h)
#include "edit.h"
#include "util.h"
#include <d2d1effects.h>
#include <algorithm>
#include <cmath>

// Effect CLSIDs (defined here so no extra import library is needed)
static const GUID kExposure = {0xb56c8cfa, 0xf634, 0x41ee, {0xbe, 0xe0, 0xff, 0xa6, 0x17, 0x10, 0x60, 0x04}};
static const GUID kContrast = {0xb648a78a, 0x0ed5, 0x4f80, {0xa9, 0x4a, 0x8e, 0x82, 0x5a, 0xca, 0x6b, 0x77}};
static const GUID kTemperatureTint = {0x89176087, 0x8af9, 0x4a08, {0xae, 0xb1, 0x89, 0x5f, 0x38, 0xdb, 0x17, 0x66}};
static const GUID kColorMatrix = {0x921f03d6, 0x641c, 0x47df, {0x85, 0x2d, 0xb4, 0xbb, 0x61, 0x53, 0xae, 0x11}};

const AspectPreset kAspects[] = {
    {L"Free", 0, 0}, {L"Original", -1, -1}, {L"Square", 1, 1}, {L"4:3", 4, 3}, {L"3:2", 3, 2}, {L"16:9", 16, 9}, {L"5:4", 5, 4}, {L"7:5", 7, 5},
};
const int kAspectCount = sizeof(kAspects) / sizeof(kAspects[0]);

void OrientedSize(const EditParams& e, float w, float h, float& ow, float& oh) {
    bool odd = (e.rot90 & 1) != 0;
    ow = odd ? h : w;
    oh = odd ? w : h;
}

D2D1_MATRIX_3X2_F EditMatrix(const EditParams& e, float w, float h) {
    float ow, oh;
    OrientedSize(e, w, h, ow, oh);
    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-w / 2, -h / 2) *
                         D2D1::Matrix3x2F::Rotation(90.0f * (e.rot90 & 3)) *
                         D2D1::Matrix3x2F::Scale(e.flipH ? -1.0f : 1.0f, e.flipV ? -1.0f : 1.0f) *
                         D2D1::Matrix3x2F::Rotation(e.straighten) *
                         D2D1::Matrix3x2F::Translation(ow / 2, oh / 2);
    return m;
}

// Is a point of the oriented frame on the straightened image?
static bool OnImage(float x, float y, float ow, float oh, float deg) {
    float a = -deg * 3.14159265f / 180.0f;
    float dx = x - ow / 2, dy = y - oh / 2;
    float rx = dx * std::cos(a) - dy * std::sin(a), ry = dx * std::sin(a) + dy * std::cos(a);
    const float eps = 0.01f;
    return std::fabs(rx) <= ow / 2 + eps && std::fabs(ry) <= oh / 2 + eps;
}

static bool RectInside(float l, float t, float r, float b, float ow, float oh, float deg) {
    return OnImage(l, t, ow, oh, deg) && OnImage(r, t, ow, oh, deg) && OnImage(l, b, ow, oh, deg) && OnImage(r, b, ow, oh, deg);
}

bool CropInside(const EditParams& e, float w, float h) {
    float ow, oh;
    OrientedSize(e, w, h, ow, oh);
    return RectInside(e.cropL * ow, e.cropT * oh, e.cropR * ow, e.cropB * oh, ow, oh, e.straighten);
}

void FitCropInside(EditParams& e, float w, float h) {
    float ow, oh;
    OrientedSize(e, w, h, ow, oh);
    e.cropL = std::clamp(e.cropL, 0.0f, 1.0f); e.cropR = std::clamp(e.cropR, 0.0f, 1.0f);
    e.cropT = std::clamp(e.cropT, 0.0f, 1.0f); e.cropB = std::clamp(e.cropB, 0.0f, 1.0f);
    float cx = (e.cropL + e.cropR) / 2 * ow, cy = (e.cropT + e.cropB) / 2 * oh;
    float hw = (e.cropR - e.cropL) / 2 * ow, hh = (e.cropB - e.cropT) / 2 * oh;
    if (RectInside(cx - hw, cy - hh, cx + hw, cy + hh, ow, oh, e.straighten)) return;
    float lo = 0, hi = 1;
    for (int i = 0; i < 30; ++i) {
        float s = (lo + hi) / 2;
        if (RectInside(cx - hw * s, cy - hh * s, cx + hw * s, cy + hh * s, ow, oh, e.straighten)) lo = s; else hi = s;
    }
    e.cropL = (cx - hw * lo) / ow; e.cropR = (cx + hw * lo) / ow;
    e.cropT = (cy - hh * lo) / oh; e.cropB = (cy + hh * lo) / oh;
}

void ResetCropForAspect(EditParams& e, float w, float h) {
    float ow, oh;
    OrientedSize(e, w, h, ow, oh);
    const AspectPreset& a = kAspects[std::clamp(e.aspect, 0, kAspectCount - 1)];
    float ratio;
    if (a.w == 0) ratio = ow / oh;
    else if (a.w < 0) ratio = w / h;
    else ratio = a.w / a.h;
    if (a.w > 0 && a.w != a.h && e.aspectPortrait) ratio = 1 / ratio;
    if (a.w < 0 && e.aspectPortrait) ratio = 1 / ratio;
    float cw = ow, ch = ow / ratio;
    if (ch > oh) { ch = oh; cw = oh * ratio; }
    e.cropL = (ow - cw) / 2 / ow; e.cropR = 1 - e.cropL;
    e.cropT = (oh - ch) / 2 / oh; e.cropB = 1 - e.cropT;
    FitCropInside(e, w, h);
}

ID2D1Image* ApplyAdjustments(ID2D1DeviceContext* dc, ID2D1Image* input, const EditParams& e, std::vector<ID2D1Effect*>& effects) {
    ID2D1Image* cur = input;
    cur->AddRef();
    auto chain = [&](ID2D1Effect* fx) {
        fx->SetInput(0, cur);
        cur->Release();
        fx->GetOutput(&cur);
        effects.push_back(fx);
    };
    ID2D1Effect* fx = nullptr;
    if (e.exposure != 0 && SUCCEEDED(dc->CreateEffect(kExposure, &fx)) && fx) {
        fx->SetValue(0 /*EXPOSURE_VALUE*/, e.exposure);
        chain(fx);
    }
    fx = nullptr;
    if (e.contrast != 0 && SUCCEEDED(dc->CreateEffect(kContrast, &fx)) && fx) {
        fx->SetValue(0 /*CONTRAST*/, std::clamp(e.contrast, -1.0f, 1.0f));
        chain(fx);
    }
    fx = nullptr;
    if (e.saturation != 0 && SUCCEEDED(dc->CreateEffect(kColorMatrix, &fx)) && fx) {
        float s = 1 + e.saturation;
        const float lr = 0.2126f, lg = 0.7152f, lb = 0.0722f;
        D2D1_MATRIX_5X4_F m = D2D1::Matrix5x4F(
            lr * (1 - s) + s, lr * (1 - s), lr * (1 - s), 0,
            lg * (1 - s), lg * (1 - s) + s, lg * (1 - s), 0,
            lb * (1 - s), lb * (1 - s), lb * (1 - s) + s, 0,
            0, 0, 0, 1,
            0, 0, 0, 0);
        fx->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
        fx->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
        chain(fx);
    }
    fx = nullptr;
    if (e.warmth != 0 && SUCCEEDED(dc->CreateEffect(kTemperatureTint, &fx)) && fx) {
        fx->SetValue(0 /*TEMPERATURE*/, std::clamp(e.warmth, -1.0f, 1.0f));
        fx->SetValue(1 /*TINT*/, 0.0f);
        chain(fx);
    }
    return cur;
}

bool RenderEdited(const Pixels& src, const EditParams& e, Pixels& out, std::wstring* error) {
    auto lock = offscreen::Lock();
    offscreen::Device* d = offscreen::Get();
    if (!d) { if (error) *error = L"Your graphics driver couldn't render the edit."; return false; }
    if (src.w > d->maxSize || src.h > d->maxSize) {
        if (error) *error = L"This image is too large to edit (over " + std::to_wstring(d->maxSize) + L" pixels on a side).";
        return false;
    }
    float w = (float)src.w, h = (float)src.h, ow, oh;
    OrientedSize(e, w, h, ow, oh);
    // whole pixels, so quarter turns and flips stay lossless
    long cl = std::lround(e.cropL * ow), ct = std::lround(e.cropT * oh), cr = std::lround(e.cropR * ow), cb = std::lround(e.cropB * oh);
    UINT outW = (UINT)std::max<long>(1, cr - cl);
    UINT outH = (UINT)std::max<long>(1, cb - ct);
    if (outW > d->maxSize || outH > d->maxSize) { if (error) *error = L"The result would be too large."; return false; }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap1* in = nullptr;
    if (FAILED(d->dc->CreateBitmap(D2D1::SizeU(src.w, src.h), src.px.data(), src.w * 4, bp, &in)) || !in) {
        if (error) *error = L"Not enough graphics memory to save the edit.";
        return false;
    }
    D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap1* target = nullptr;
    bool ok = false;
    if (SUCCEEDED(d->dc->CreateBitmap(D2D1::SizeU(outW, outH), nullptr, 0, tp, &target)) && target) {
        std::vector<ID2D1Effect*> fx;
        ID2D1Image* img = ApplyAdjustments(d->dc, in, e, fx);
        d->dc->SetTarget(target);
        d->dc->BeginDraw();
        d->dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        d->dc->SetTransform(EditMatrix(e, w, h) * D2D1::Matrix3x2F::Translation((float)-cl, (float)-ct));
        // quarter turns and flips map pixels exactly; straightening needs resampling
        bool exact = e.straighten == 0;
        d->dc->DrawImage(img, D2D1::Point2F(0, 0), D2D1::RectF(0, 0, w, h),
                         exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        d->dc->SetTransform(D2D1::Matrix3x2F::Identity());
        HRESULT hr = d->dc->EndDraw();
        d->dc->SetTarget(nullptr);
        img->Release();
        for (auto* f : fx) f->Release();
        ok = SUCCEEDED(hr) && offscreen::ReadBack(d, target, out);
        target->Release();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { in->Release(); offscreen::Reset(); if (error) *error = L"The graphics device was reset. Try again."; return false; }
        if (!ok && error) *error = L"Rendering the edit failed.";
    }
    in->Release();
    return ok;
}
