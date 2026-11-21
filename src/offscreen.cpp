// Loupe - offscreen Direct2D device (see offscreen.h)
#include "offscreen.h"
#include "util.h"
#include <d3d11.h>
#include <dxgi.h>
#include <shlwapi.h>
#include <algorithm>

namespace offscreen {

static std::mutex s_lock;
static Device s_dev;
static bool s_failed = false;

std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(s_lock); }

void Reset() {
    if (s_dev.dc) s_dev.dc->Release();
    if (s_dev.device) s_dev.device->Release();
    if (s_dev.factory) s_dev.factory->Release();
    s_dev = Device{};
    s_failed = false;
}

Device* Get() {
    if (s_dev.dc) return &s_dev;
    if (s_failed) return nullptr;
    ID3D11Device* d3d = nullptr;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = E_FAIL;
    D3D_FEATURE_LEVEL got{};
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &d3d, &got, nullptr);
        if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, &d3d, &got, nullptr);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr) || !d3d) { s_failed = true; Log(L"offscreen: no D3D11 device (0x%08X)", (unsigned)hr); return nullptr; }
    IDXGIDevice* dxgi = nullptr;
    d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi);
    D2D1_FACTORY_OPTIONS fo{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)&s_dev.factory);
    if (s_dev.factory && dxgi) s_dev.factory->CreateDevice(dxgi, &s_dev.device);
    if (s_dev.device) s_dev.device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &s_dev.dc);
    if (dxgi) dxgi->Release();
    d3d->Release();
    if (!s_dev.dc) { Reset(); s_failed = true; Log(L"offscreen: no D2D device"); return nullptr; }
    s_dev.maxSize = s_dev.dc->GetMaximumBitmapSize();
    s_dev.dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    return &s_dev;
}

bool ReadBack(Device* d, ID2D1Bitmap1* target, Pixels& out) {
    D2D1_SIZE_U sz = target->GetPixelSize();
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap1* cpu = nullptr;
    if (FAILED(d->dc->CreateBitmap(sz, nullptr, 0, bp, &cpu)) || !cpu) return false;
    bool ok = false;
    if (SUCCEEDED(cpu->CopyFromBitmap(nullptr, target, nullptr))) {
        D2D1_MAPPED_RECT m;
        if (SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m))) {
            out.w = sz.width;
            out.h = sz.height;
            out.px.resize((size_t)sz.width * sz.height * 4);
            for (UINT y = 0; y < sz.height; ++y) memcpy(&out.px[(size_t)y * sz.width * 4], m.bits + (size_t)y * m.pitch, sz.width * 4);
            cpu->Unmap();
            ok = true;
        }
    }
    cpu->Release();
    return ok;
}

// ---------------------------------------------------------------- SVG
static ID2D1SvgDocument* OpenSvg(ID2D1DeviceContext5* dc5, const std::string& data, D2D1_SIZE_F viewport) {
    IStream* st = SHCreateMemStream((const BYTE*)data.data(), (UINT)data.size());
    if (!st) return nullptr;
    ID2D1SvgDocument* doc = nullptr;
    dc5->CreateSvgDocument(st, viewport, &doc);
    st->Release();
    return doc;
}

static bool LengthAttr(ID2D1SvgElement* root, const wchar_t* name, float& out) {
    D2D1_SVG_LENGTH len{};
    if (FAILED(root->GetAttributeValue(name, &len))) return false;
    if (len.units != D2D1_SVG_LENGTH_UNITS_NUMBER || len.value <= 0) return false;
    out = len.value;
    return true;
}

static bool NaturalSize(ID2D1SvgDocument* doc, float& w, float& h) {
    ID2D1SvgElement* root = nullptr;
    doc->GetRoot(&root);
    if (!root) return false;
    float aw = 0, ah = 0;
    bool hw = LengthAttr(root, L"width", aw), hh = LengthAttr(root, L"height", ah);
    D2D1_SVG_VIEWBOX vb{};
    bool hasVb = SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) && vb.width > 0 && vb.height > 0;
    root->Release();
    if (hw && hh) { w = aw; h = ah; return true; }
    if (hasVb) {
        if (hw) { w = aw; h = aw * vb.height / vb.width; return true; }
        if (hh) { h = ah; w = ah * vb.width / vb.height; return true; }
        w = vb.width; h = vb.height;
        // tiny icon viewBoxes (0 0 24 24) look better at a sensible size
        float s = std::max(1.0f, 512.0f / std::max(w, h));
        w *= s; h *= s;
        return true;
    }
    w = 1024; h = 1024;
    return true;
}

bool SvgSize(const std::string& data, float& w, float& h) {
    auto lock = Lock();
    Device* d = Get();
    if (!d) return false;
    ID2D1DeviceContext5* dc5 = nullptr;
    if (FAILED(d->dc->QueryInterface(__uuidof(ID2D1DeviceContext5), (void**)&dc5)) || !dc5) return false;
    ID2D1SvgDocument* doc = OpenSvg(dc5, data, D2D1::SizeF(1024, 1024));
    bool ok = doc && NaturalSize(doc, w, h);
    if (doc) doc->Release();
    dc5->Release();
    return ok;
}

bool RasterSvg(const std::string& data, UINT w, UINT h, Pixels& out) {
    auto lock = Lock();
    Device* d = Get();
    if (!d || w == 0 || h == 0) return false;
    w = std::min(w, d->maxSize);
    h = std::min(h, d->maxSize);
    ID2D1DeviceContext5* dc5 = nullptr;
    if (FAILED(d->dc->QueryInterface(__uuidof(ID2D1DeviceContext5), (void**)&dc5)) || !dc5) return false;
    bool ok = false;
    float nw = 0, nh = 0;
    ID2D1SvgDocument* doc = OpenSvg(dc5, data, D2D1::SizeF(1024, 1024));
    if (doc && NaturalSize(doc, nw, nh)) {
        // lay the document out at its natural size, then scale it to the requested pixels
        doc->SetViewportSize(D2D1::SizeF(nw, nh));
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ID2D1Bitmap1* target = nullptr;
        if (SUCCEEDED(d->dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bp, &target)) && target) {
            d->dc->SetTarget(target);
            d->dc->BeginDraw();
            d->dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            d->dc->SetTransform(D2D1::Matrix3x2F::Scale(w / nw, h / nh));
            dc5->DrawSvgDocument(doc);
            d->dc->SetTransform(D2D1::Matrix3x2F::Identity());
            HRESULT hr = d->dc->EndDraw();
            d->dc->SetTarget(nullptr);
            ok = SUCCEEDED(hr) && ReadBack(d, target, out);
            if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { target->Release(); target = nullptr; doc->Release(); dc5->Release(); Reset(); return false; }
            target->Release();
        }
    }
    if (doc) doc->Release();
    dc5->Release();
    return ok;
}

} // namespace offscreen
