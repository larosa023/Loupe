// Loupe - decoding (see image.h)
#include "image.h"
#include "exif.h"
#include "util.h"
#include <shlwapi.h>
#include <shobjidl.h>
#include <propvarutil.h>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "src/webp/decode.h"
#include "src/webp/demux.h"
#include <wincodecsdk.h>

// Missing from the MinGW headers
#ifndef __IWICBitmapSourceTransform_INTERFACE_DEFINED__
struct IWICBitmapSourceTransform : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* prc, UINT w, UINT h, WICPixelFormatGUID* fmt, WICBitmapTransformOptions t,
                                                 UINT stride, UINT size, BYTE* buf) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestSize(UINT* w, UINT* h) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestPixelFormat(WICPixelFormatGUID* fmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoesSupportTransform(WICBitmapTransformOptions t, BOOL* supported) = 0;
};
__CRT_UUID_DECL(IWICBitmapSourceTransform, 0x3b16811b, 0x6a43, 0x4ec9, 0xb7, 0x13, 0x3d, 0x5a, 0x0c, 0x13, 0xb9, 0x40)
#endif

static const size_t kAnimBudget = 384ull * 1024 * 1024;   // composited animation frames, in bytes

// ---------------------------------------------------------------- WIC
IWICImagingFactory* Wic() {
    static IWICImagingFactory* f = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    });
    return f;
}

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// RAII holder for COM pointers within this file
template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { Rel(p); }
    T** operator&() { Rel(p); return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

const std::vector<std::wstring>& SupportedExtensions() {
    static const std::vector<std::wstring> exts = {
        L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".apng", L".gif", L".webp", L".avif", L".heic", L".heif",
        L".bmp", L".dib", L".tif", L".tiff", L".ico", L".svg",
        L".cr2", L".cr3", L".nef", L".arw", L".dng",
    };
    return exts;
}

bool IsSupportedExtension(const std::wstring& ext) {
    for (auto& e : SupportedExtensions()) if (e == ext) return true;
    return false;
}

std::wstring ExtOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    return Lower(path.substr(dot));
}

// ---------------------------------------------------------------- pixel helpers
bool ScalePixels(const Pixels& in, UINT w, UINT h, Pixels& out) {
    if (in.empty() || !w || !h) return false;
    IWICImagingFactory* f = Wic();
    Com<IWICBitmap> bmp;
    if (FAILED(f->CreateBitmapFromMemory(in.w, in.h, GUID_WICPixelFormat32bppPBGRA, in.w * 4, (UINT)in.px.size(),
                                         (BYTE*)in.px.data(), &bmp))) return false;
    Com<IWICBitmapScaler> sc;
    if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(bmp, w, h, WICBitmapInterpolationModeFant))) return false;
    Pixels o;
    o.w = w; o.h = h;
    o.px.resize((size_t)w * h * 4);
    if (FAILED(sc->CopyPixels(nullptr, w * 4, (UINT)o.px.size(), o.px.data()))) return false;
    out = std::move(o);
    return true;
}

void OrientPixels(Pixels& p, int o) {
    if (o <= 1 || o > 8 || p.empty()) return;
    const UINT W = p.w, H = p.h;
    bool swap = o >= 5;
    UINT nw = swap ? H : W, nh = swap ? W : H;
    std::vector<uint8_t> dst((size_t)nw * nh * 4);
    const uint32_t* s = (const uint32_t*)p.px.data();
    uint32_t* d = (uint32_t*)dst.data();
    for (UINT y = 0; y < H; ++y) {
        for (UINT x = 0; x < W; ++x) {
            UINT dx, dy;
            switch (o) {
                case 2: dx = W - 1 - x; dy = y; break;               // mirror
                case 3: dx = W - 1 - x; dy = H - 1 - y; break;       // 180
                case 4: dx = x; dy = H - 1 - y; break;               // flip vertical
                case 5: dx = y; dy = x; break;                       // transpose
                case 6: dx = H - 1 - y; dy = x; break;               // 90 clockwise
                case 7: dx = H - 1 - y; dy = W - 1 - x; break;       // transverse
                default: dx = y; dy = W - 1 - x; break;              // 8: 90 counter-clockwise
            }
            d[(size_t)dy * nw + dx] = s[(size_t)y * W + x];
        }
    }
    p.px.swap(dst);
    p.w = nw;
    p.h = nh;
}

static bool HasAlpha(const Pixels& p) {
    const uint8_t* a = p.px.data() + 3;
    size_t n = (size_t)p.w * p.h;
    for (size_t i = 0; i < n; i += 7) if (a[i * 4] != 255) return true;
    return false;
}

// Fit (w, h) inside (maxW, maxH) without enlarging. 0 = no limit.
static void FitSize(UINT w, UINT h, UINT maxW, UINT maxH, UINT& ow, UINT& oh) {
    double s = 1.0;
    if (maxW && w > maxW) s = std::min(s, (double)maxW / w);
    if (maxH && h > maxH) s = std::min(s, (double)maxH / h);
    ow = std::max<UINT>(1, (UINT)std::lround(w * s));
    oh = std::max<UINT>(1, (UINT)std::lround(h * s));
}

// ---------------------------------------------------------------- color management
// Photos with an embedded profile (Adobe RGB, Display P3…) are converted to sRGB.
static bool IsSrgbProfile(IWICColorContext* cc) {
    UINT n = 0;
    if (FAILED(cc->GetProfileBytes(0, nullptr, &n)) || n < 132) return true;
    std::string b(n, '\0');
    if (FAILED(cc->GetProfileBytes(n, (BYTE*)&b[0], &n))) return true;
    // the description tag almost always names sRGB profiles (ASCII or UTF-16BE); cheap and good enough
    auto has = [&](const char* needle, size_t n) { return std::search(b.begin(), b.end(), needle, needle + n) != b.end(); };
    return has("sRGB", 4) || has("\0s\0R\0G\0B", 8) || has("IEC61966", 8);
}

static void ColorManage(IWICBitmapFrameDecode* frame, Com<IWICBitmapSource>& src) {
    IWICImagingFactory* f = Wic();
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0 || count > 8) return;
    IWICColorContext* ctx[8] = {};
    for (UINT i = 0; i < count; ++i) f->CreateColorContext(&ctx[i]);
    UINT got = 0;
    bool ok = SUCCEEDED(frame->GetColorContexts(count, ctx, &got)) && got > 0;
    IWICColorContext* profile = nullptr;
    if (ok) {
        for (UINT i = 0; i < got; ++i) {
            WICColorContextType t;
            if (SUCCEEDED(ctx[i]->GetType(&t)) && t == WICColorContextProfile) { profile = ctx[i]; break; }
        }
    }
    if (profile && !IsSrgbProfile(profile)) {
        Com<IWICColorContext> srgb;
        Com<IWICFormatConverter> pre;
        Com<IWICColorTransform> xf;
        if (SUCCEEDED(f->CreateColorContext(&srgb)) && SUCCEEDED(srgb->InitializeFromExifColorSpace(1)) &&
            SUCCEEDED(f->CreateFormatConverter(&pre)) &&
            SUCCEEDED(pre->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(f->CreateColorTransformer(&xf)) &&
            SUCCEEDED(xf->Initialize(pre, profile, srgb, GUID_WICPixelFormat32bppBGRA))) {
            src.p->Release();
            src.p = xf.p;
            xf.p = nullptr;
        }
    }
    for (UINT i = 0; i < count; ++i) if (ctx[i]) ctx[i]->Release();
}

// ---------------------------------------------------------------- WIC stills
// Decodes one frame, scaled to fit maxW x maxH (after orientation), color managed and oriented.
static bool WicFrame(IWICBitmapFrameDecode* frame, int orientation, UINT maxW, UINT maxH, Pixels& out, UINT* fullW, UINT* fullH) {
    IWICImagingFactory* f = Wic();
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) return false;
    bool swap = orientation >= 5 && orientation <= 8;
    if (fullW) *fullW = swap ? h : w;
    if (fullH) *fullH = swap ? w : h;
    UINT tw, th;
    FitSize(w, h, swap ? maxH : maxW, swap ? maxW : maxH, tw, th);

    Com<IWICBitmapSource> src;
    frame->QueryInterface(IID_PPV_ARGS(&src));
    std::vector<uint8_t> scratch;
    if (tw < w) {
        // JPEG (and some others) can decode straight to 1/2, 1/4 or 1/8 size: much faster
        Com<IWICBitmapSourceTransform> tr;
        UINT cw = tw, ch = th;
        if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&tr))) && SUCCEEDED(tr->GetClosestSize(&cw, &ch)) && cw >= tw && ch >= th && cw < w) {
            WICPixelFormatGUID pf;
            frame->GetPixelFormat(&pf);
            if (SUCCEEDED(tr->GetClosestPixelFormat(&pf))) {
                Com<IWICComponentInfo> ci;
                Com<IWICPixelFormatInfo> pfi;
                UINT bpp = 32;
                if (SUCCEEDED(f->CreateComponentInfo(pf, &ci)) && SUCCEEDED(ci->QueryInterface(IID_PPV_ARGS(&pfi)))) pfi->GetBitsPerPixel(&bpp);
                UINT stride = (cw * bpp + 31) / 32 * 4;
                scratch.resize((size_t)stride * ch);
                Com<IWICBitmap> mem;
                if (SUCCEEDED(tr->CopyPixels(nullptr, cw, ch, &pf, WICBitmapTransformRotate0, stride, (UINT)scratch.size(), scratch.data())) &&
                    SUCCEEDED(f->CreateBitmapFromMemory(cw, ch, pf, stride, (UINT)scratch.size(), scratch.data(), &mem))) {
                    src.p->Release();
                    mem->QueryInterface(IID_PPV_ARGS(&src.p));
                    w = cw; h = ch;
                }
            }
        }
        if (tw < w) {
            Com<IWICBitmapScaler> sc;
            if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(src, tw, th, WICBitmapInterpolationModeFant))) {
                src.p->Release();
                sc->QueryInterface(IID_PPV_ARGS(&src.p));
                w = tw; h = th;
            }
        }
    }
    ColorManage(frame, src);
    Com<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    out.w = w; out.h = h;
    if ((uint64_t)w * h * 4 > 0xFFFFFFF0ull) return false;   // WIC buffers are 32-bit
    out.px.resize((size_t)w * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()))) return false;
    OrientPixels(out, orientation);
    return true;
}

static std::wstring ContainerName(IWICBitmapDecoder* dec) {
    GUID g{};
    if (FAILED(dec->GetContainerFormat(&g))) return L"Image";
    if (g == GUID_ContainerFormatJpeg) return L"JPEG";
    if (g == GUID_ContainerFormatPng) return L"PNG";
    if (g == GUID_ContainerFormatGif) return L"GIF";
    if (g == GUID_ContainerFormatBmp) return L"BMP";
    if (g == GUID_ContainerFormatTiff) return L"TIFF";
    if (g == GUID_ContainerFormatIco) return L"Icon";
    if (g == GUID_ContainerFormatWmp) return L"JPEG XR";
    if (g == GUID_ContainerFormatDds) return L"DDS";
    // HEIF and WebP container GUIDs (newer SDKs only)
    static const GUID heif = {0xe1e62521, 0x6787, 0x405b, {0xa3, 0x39, 0x50, 0x07, 0x15, 0xb5, 0x76, 0x3f}};
    static const GUID webp = {0xe094b0e2, 0x67f2, 0x45b3, {0xb0, 0xea, 0x11, 0x53, 0x37, 0xca, 0x7c, 0xf3}};
    if (g == heif) return L"HEIF";
    if (g == webp) return L"WebP";
    return L"Image";
}

static bool UInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT& out) {
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = SUCCEEDED(q->GetMetadataByName(name, &v));
    if (ok) {
        switch (v.vt) {
            case VT_UI1: out = v.bVal; break;
            case VT_UI2: out = v.uiVal; break;
            case VT_UI4: out = v.ulVal; break;
            case VT_BOOL: out = v.boolVal ? 1 : 0; break;
            default: ok = false;
        }
    }
    PropVariantClear(&v);
    return ok;
}

// ---------------------------------------------------------------- GIF
static bool DecodeGif(IWICBitmapDecoder* dec, UINT maxW, UINT maxH, ImageData& out) {
    IWICImagingFactory* f = Wic();
    UINT count = 0;
    dec->GetFrameCount(&count);
    if (count == 0) return false;
    UINT cw = 0, ch = 0;
    Com<IWICMetadataQueryReader> gq;
    if (SUCCEEDED(dec->GetMetadataQueryReader(&gq))) {
        UInt(gq, L"/logscrdesc/Width", cw);
        UInt(gq, L"/logscrdesc/Height", ch);
        // NETSCAPE2.0 loop count; without it a GIF plays once
        out.loops = 1;
        PROPVARIANT app, data;
        PropVariantInit(&app);
        PropVariantInit(&data);
        if (SUCCEEDED(gq->GetMetadataByName(L"/appext/application", &app)) && app.vt == (VT_UI1 | VT_VECTOR) && app.caub.cElems == 11 &&
            (memcmp(app.caub.pElems, "NETSCAPE2.0", 11) == 0 || memcmp(app.caub.pElems, "ANIMEXTS1.0", 11) == 0) &&
            SUCCEEDED(gq->GetMetadataByName(L"/appext/data", &data)) && data.vt == (VT_UI1 | VT_VECTOR) && data.caub.cElems >= 4 && data.caub.pElems[0] >= 3 && data.caub.pElems[1] == 1) {
            out.loops = data.caub.pElems[2] | (data.caub.pElems[3] << 8);
        }
        PropVariantClear(&app);
        PropVariantClear(&data);
    }
    if (!cw || !ch) {
        Com<IWICBitmapFrameDecode> f0;
        if (FAILED(dec->GetFrame(0, &f0))) return false;
        f0->GetSize(&cw, &ch);
    }
    if (!cw || !ch) return false;
    out.fullW = cw;
    out.fullH = ch;
    UINT tw, th;
    FitSize(cw, ch, maxW, maxH, tw, th);
    double bytes = (double)tw * th * 4 * count;
    if (bytes > kAnimBudget) {
        double s = std::sqrt(kAnimBudget / bytes);
        tw = std::max<UINT>(1, (UINT)(tw * s));
        th = std::max<UINT>(1, (UINT)(th * s));
    }
    out.fullRes = tw == cw && th == ch;
    std::vector<uint32_t> canvas((size_t)cw * ch, 0), saved;
    UINT prevDisposal = 0, px = 0, py = 0, pw = 0, ph = 0;
    for (UINT i = 0; i < count; ++i) {
        Com<IWICBitmapFrameDecode> fr;
        if (FAILED(dec->GetFrame(i, &fr))) break;
        Com<IWICMetadataQueryReader> q;
        UINT left = 0, top = 0, fw = 0, fh = 0, delay = 0, disposal = 0;
        fr->GetSize(&fw, &fh);
        if (SUCCEEDED(fr->GetMetadataQueryReader(&q))) {
            UInt(q, L"/imgdesc/Left", left);
            UInt(q, L"/imgdesc/Top", top);
            UInt(q, L"/grctlext/Delay", delay);
            UInt(q, L"/grctlext/Disposal", disposal);
        }
        // undo the previous frame
        if (prevDisposal == 2) {
            for (UINT y = py; y < std::min(ch, py + ph); ++y)
                for (UINT x = px; x < std::min(cw, px + pw); ++x) canvas[(size_t)y * cw + x] = 0;
        } else if (prevDisposal == 3 && !saved.empty()) {
            canvas = saved;
        }
        if (disposal == 3) saved = canvas;
        Com<IWICFormatConverter> conv;
        std::vector<uint32_t> fp((size_t)fw * fh);
        if (SUCCEEDED(f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, fw * fh * 4, (BYTE*)fp.data()))) {
            for (UINT y = 0; y < fh && top + y < ch; ++y)
                for (UINT x = 0; x < fw && left + x < cw; ++x) {
                    uint32_t s = fp[(size_t)y * fw + x];
                    if (s >> 24) canvas[(size_t)(top + y) * cw + left + x] = s;
                }
        }
        prevDisposal = disposal; px = left; py = top; pw = fw; ph = fh;
        Frame frame;
        frame.delayMs = delay * 10;
        if (frame.delayMs <= 10) frame.delayMs = 100;   // what browsers do with 0/10 ms delays
        Pixels full;
        full.w = cw; full.h = ch;
        full.px.assign((uint8_t*)canvas.data(), (uint8_t*)canvas.data() + canvas.size() * 4);
        if (tw != cw || th != ch) ScalePixels(full, tw, th, frame.pix); else frame.pix = std::move(full);
        out.frames.push_back(std::move(frame));
    }
    out.animated = out.frames.size() > 1;
    out.hasAlpha = !out.frames.empty() && HasAlpha(out.frames[0].pix);
    return !out.frames.empty();
}

// ---------------------------------------------------------------- APNG
static uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    });
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void PutBe32(std::string& s, uint32_t v) { s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v; }

static void AppendChunk(std::string& png, const char* type, const uint8_t* data, size_t n) {
    PutBe32(png, (uint32_t)n);
    size_t start = png.size();
    png.append(type, 4);
    png.append((const char*)data, n);
    PutBe32(png, Crc32((const uint8_t*)png.data() + start, n + 4));
}

static bool DecodePngBytes(const std::string& png, Pixels& out) {
    IWICImagingFactory* f = Wic();
    Com<IWICStream> st;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> fr;
    if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)png.data(), (DWORD)png.size())) ||
        FAILED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) || FAILED(dec->GetFrame(0, &fr))) return false;
    return WicFrame(fr, 1, 0, 0, out, nullptr, nullptr);
}

struct ApngFrame {
    uint32_t w = 0, h = 0, x = 0, y = 0;
    uint16_t delayNum = 0, delayDen = 0;
    uint8_t dispose = 0, blend = 0;
    std::string data;   // concatenated image data (IDAT payloads)
};

static bool DecodeApng(const std::string& file, UINT maxW, UINT maxH, ImageData& out) {
    const uint8_t* p = (const uint8_t*)file.data();
    size_t n = file.size(), pos = 8;
    std::string ihdr, preIdat;   // ancillary chunks shared by every frame (PLTE, tRNS, iCCP…)
    std::vector<ApngFrame> frames;
    uint32_t plays = 0;
    bool sawIdat = false, idatIsFrame = false, hasActl = false;
    ApngFrame* cur = nullptr;
    while (pos + 12 <= n) {
        uint32_t len = Be32(p + pos);
        if (pos + 12 + (size_t)len > n) break;
        std::string type((const char*)p + pos + 4, 4);
        const uint8_t* d = p + pos + 8;
        if (type == "IHDR" && len == 13) ihdr.assign((const char*)d, 13);
        else if (type == "acTL" && len >= 8) { hasActl = true; plays = Be32(d + 4); }
        else if (type == "fcTL" && len >= 26) {
            frames.push_back(ApngFrame{});
            cur = &frames.back();
            cur->w = Be32(d + 4); cur->h = Be32(d + 8); cur->x = Be32(d + 12); cur->y = Be32(d + 16);
            cur->delayNum = (uint16_t)((d[20] << 8) | d[21]); cur->delayDen = (uint16_t)((d[22] << 8) | d[23]);
            cur->dispose = d[24]; cur->blend = d[25];
            if (!sawIdat) idatIsFrame = true;
        } else if (type == "IDAT") {
            sawIdat = true;
            if (idatIsFrame && cur) cur->data.append((const char*)d, len);
        } else if (type == "fdAT" && len > 4) {
            if (cur) cur->data.append((const char*)d + 4, len - 4);
        } else if (type == "IEND") break;
        else if (!sawIdat && type != "acTL" && type != "fcTL" && type != "IHDR") {
            preIdat.append((const char*)p + pos, 12 + len);
        }
        pos += 12 + len;
    }
    if (!hasActl || ihdr.size() != 13 || frames.size() < 2) return false;
    uint32_t cw = Be32((const uint8_t*)ihdr.data()), ch = Be32((const uint8_t*)ihdr.data() + 4);
    if (!cw || !ch || cw > 16384 || ch > 16384) return false;
    out.format = L"PNG (animated)";
    out.fullW = cw; out.fullH = ch;
    out.loops = (int)plays;
    UINT tw, th;
    FitSize(cw, ch, maxW, maxH, tw, th);
    double bytes = (double)tw * th * 4 * frames.size();
    if (bytes > kAnimBudget) { double s = std::sqrt(kAnimBudget / bytes); tw = std::max<UINT>(1, (UINT)(tw * s)); th = std::max<UINT>(1, (UINT)(th * s)); }
    out.fullRes = tw == cw && th == ch;
    std::vector<uint32_t> canvas((size_t)cw * ch, 0), saved;
    for (size_t i = 0; i < frames.size(); ++i) {
        ApngFrame& fr = frames[i];
        if (fr.data.empty() || fr.x + fr.w > cw || fr.y + fr.h > ch || !fr.w || !fr.h) continue;
        std::string png("\x89PNG\r\n\x1a\n", 8);
        std::string hd = ihdr;
        uint8_t* hp = (uint8_t*)&hd[0];
        hp[0] = (uint8_t)(fr.w >> 24); hp[1] = (uint8_t)(fr.w >> 16); hp[2] = (uint8_t)(fr.w >> 8); hp[3] = (uint8_t)fr.w;
        hp[4] = (uint8_t)(fr.h >> 24); hp[5] = (uint8_t)(fr.h >> 16); hp[6] = (uint8_t)(fr.h >> 8); hp[7] = (uint8_t)fr.h;
        AppendChunk(png, "IHDR", (const uint8_t*)hd.data(), 13);
        png += preIdat;
        AppendChunk(png, "IDAT", (const uint8_t*)fr.data.data(), fr.data.size());
        AppendChunk(png, "IEND", nullptr, 0);
        Pixels fp;
        if (!DecodePngBytes(png, fp) || fp.w != fr.w || fp.h != fr.h) continue;
        uint8_t dispose = fr.dispose;
        if (i == 0 && dispose == 2) dispose = 1;
        if (dispose == 2) saved = canvas;
        const uint32_t* s = (const uint32_t*)fp.px.data();
        for (uint32_t y = 0; y < fr.h; ++y) {
            uint32_t* row = &canvas[(size_t)(fr.y + y) * cw + fr.x];
            for (uint32_t x = 0; x < fr.w; ++x) {
                uint32_t sp = s[(size_t)y * fr.w + x];
                if (fr.blend == 0) { row[x] = sp; continue; }
                uint32_t sa = sp >> 24;
                if (sa == 255) { row[x] = sp; continue; }
                if (sa == 0) continue;
                // premultiplied "over"
                uint32_t dp = row[x], inv = 255 - sa, r = 0;
                for (int c = 0; c < 32; c += 8) {
                    uint32_t v = ((sp >> c) & 0xFF) + (((dp >> c) & 0xFF) * inv + 127) / 255;
                    r |= std::min<uint32_t>(v, 255) << c;
                }
                row[x] = r;
            }
        }
        Frame f;
        double den = fr.delayDen ? fr.delayDen : 100;
        f.delayMs = (UINT)std::lround(fr.delayNum * 1000.0 / den);
        if (f.delayMs < 20) f.delayMs = 20;
        Pixels full;
        full.w = cw; full.h = ch;
        full.px.assign((uint8_t*)canvas.data(), (uint8_t*)canvas.data() + canvas.size() * 4);
        if (tw != cw || th != ch) ScalePixels(full, tw, th, f.pix); else f.pix = std::move(full);
        out.frames.push_back(std::move(f));
        if (dispose == 1) {
            for (uint32_t y = 0; y < fr.h; ++y) memset(&canvas[(size_t)(fr.y + y) * cw + fr.x], 0, fr.w * 4);
        } else if (dispose == 2 && !saved.empty()) {
            canvas = saved;
        }
    }
    out.animated = out.frames.size() > 1;
    out.hasAlpha = true;
    return !out.frames.empty();
}

// ---------------------------------------------------------------- WebP
static bool DecodeWebp(const std::string& data, UINT maxW, UINT maxH, bool animation, ImageData& out) {
    WebPBitstreamFeatures feat;
    if (WebPGetFeatures((const uint8_t*)data.data(), data.size(), &feat) != VP8_STATUS_OK) return false;
    out.format = feat.has_animation ? L"WebP (animated)" : (feat.format == 2 ? L"WebP (lossless)" : L"WebP");
    out.hasAlpha = feat.has_alpha != 0;
    if (feat.has_animation && animation) {
        WebPAnimDecoderOptions opt;
        if (!WebPAnimDecoderOptionsInit(&opt)) return false;
        opt.color_mode = MODE_bgrA;
        opt.use_threads = 1;
        WebPData wd{(const uint8_t*)data.data(), data.size()};
        WebPAnimDecoder* dec = WebPAnimDecoderNew(&wd, &opt);
        if (!dec) return false;
        WebPAnimInfo info;
        WebPAnimDecoderGetInfo(dec, &info);
        out.fullW = info.canvas_width;
        out.fullH = info.canvas_height;
        out.loops = (int)info.loop_count;
        UINT tw, th;
        FitSize(out.fullW, out.fullH, maxW, maxH, tw, th);
        double bytes = (double)tw * th * 4 * std::max<uint32_t>(1, info.frame_count);
        if (bytes > kAnimBudget) { double s = std::sqrt(kAnimBudget / bytes); tw = std::max<UINT>(1, (UINT)(tw * s)); th = std::max<UINT>(1, (UINT)(th * s)); }
        out.fullRes = tw == out.fullW && th == out.fullH;
        int prevTs = 0;
        while (WebPAnimDecoderHasMoreFrames(dec)) {
            uint8_t* buf = nullptr;
            int ts = 0;
            if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) break;
            Frame f;
            f.delayMs = (UINT)std::max(20, ts - prevTs);
            prevTs = ts;
            Pixels full;
            full.w = out.fullW; full.h = out.fullH;
            full.px.assign(buf, buf + (size_t)full.w * full.h * 4);
            if (!out.fullRes) ScalePixels(full, tw, th, f.pix); else f.pix = std::move(full);
            out.frames.push_back(std::move(f));
        }
        WebPAnimDecoderDelete(dec);
        out.animated = out.frames.size() > 1;
        return !out.frames.empty();
    }
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) return false;
    out.fullW = feat.width;
    out.fullH = feat.height;
    UINT tw, th;
    FitSize(feat.width, feat.height, maxW, maxH, tw, th);
    if (feat.has_animation) {
        // first frame only (thumbnails): demux it, then decode as a still
        WebPData wd{(const uint8_t*)data.data(), data.size()};
        WebPDemuxer* dmx = WebPDemux(&wd);
        if (!dmx) return false;
        WebPIterator it;
        bool ok = false;
        if (WebPDemuxGetFrame(dmx, 1, &it)) {
            ImageData one;
            std::string fragment((const char*)it.fragment.bytes, it.fragment.size);
            WebPDemuxReleaseIterator(&it);
            // a bare VP8/VP8L fragment decodes fine on its own
            WebPDecoderConfig c2;
            WebPInitDecoderConfig(&c2);
            if (WebPGetFeatures((const uint8_t*)fragment.data(), fragment.size(), &c2.input) == VP8_STATUS_OK) {
                Frame f;
                f.pix.w = c2.input.width; f.pix.h = c2.input.height;
                f.pix.px.resize((size_t)f.pix.w * f.pix.h * 4);
                c2.output.colorspace = MODE_bgrA;
                c2.output.is_external_memory = 1;
                c2.output.u.RGBA.rgba = f.pix.px.data();
                c2.output.u.RGBA.stride = (int)f.pix.w * 4;
                c2.output.u.RGBA.size = f.pix.px.size();
                if (WebPDecode((const uint8_t*)fragment.data(), fragment.size(), &c2) == VP8_STATUS_OK) {
                    if (f.pix.w > tw || f.pix.h > th) { Pixels s; if (ScalePixels(f.pix, tw, th, s)) f.pix = std::move(s); }
                    out.frames.push_back(std::move(f));
                    ok = true;
                }
                WebPFreeDecBuffer(&c2.output);
            }
        }
        WebPDemuxDelete(dmx);
        out.fullRes = false;
        return ok;
    }
    Frame f;
    f.pix.w = tw;
    f.pix.h = th;
    f.pix.px.resize((size_t)tw * th * 4);
    if (tw != (UINT)feat.width) {
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = (int)tw;
        cfg.options.scaled_height = (int)th;
    }
    cfg.options.use_threads = 1;
    cfg.output.colorspace = MODE_bgrA;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = f.pix.px.data();
    cfg.output.u.RGBA.stride = (int)tw * 4;
    cfg.output.u.RGBA.size = f.pix.px.size();
    VP8StatusCode st = WebPDecode((const uint8_t*)data.data(), data.size(), &cfg);
    WebPFreeDecBuffer(&cfg.output);
    if (st != VP8_STATUS_OK) return false;
    out.fullRes = tw == (UINT)feat.width;
    out.frames.push_back(std::move(f));
    return true;
}

// ---------------------------------------------------------------- shell fallback
// The Windows thumbnail of last resort (e.g. a format only a Store codec reads, with a cached thumbnail)
static bool ShellPreview(const std::wstring& path, UINT size, Pixels& out) {
    Com<IShellItemImageFactory> fac;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&fac)))) return false;
    HBITMAP hb = nullptr;
    SIZE sz{(LONG)size, (LONG)size};
    if (FAILED(fac->GetImage(sz, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &hb)) || !hb) return false;   // never the file-type icon
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    bool ok = false;
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        out.w = bm.bmWidth; out.h = bm.bmHeight;
        out.px.resize((size_t)out.w * out.h * 4);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, hb, 0, out.h, out.px.data(), &bi, DIB_RGB_COLORS) == (int)out.h;
        ReleaseDC(nullptr, dc);
        if (ok) {
            // plain thumbnails come back with alpha 0 everywhere
            bool allZero = true;
            for (size_t i = 3; i < out.px.size() && allZero; i += 4) allZero = out.px[i] == 0;
            if (allZero) for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
        }
    }
    DeleteObject(hb);
    return ok;
}

// ---------------------------------------------------------------- dispatch
static void CodecHint(const std::wstring& ext, ImageData& out) {
    if (ext == L".heic" || ext == L".heif") {
        out.codecName = L"HEIF Image Extensions";
        out.codecStoreId = L"9PMMSR1CGPWG";
        out.error = L"Windows needs the HEIF Image Extensions (and, for iPhone photos, the HEVC Video Extensions) to open this photo.";
    } else if (ext == L".avif") {
        out.codecName = L"AV1 Video Extension";
        out.codecStoreId = L"9MVZQVXJBQ9V";
        out.error = L"Windows needs the AV1 Video Extension to open AVIF images.";
    } else if (IsRawExtension(ext)) {
        out.codecName = L"Raw Image Extension";
        out.codecStoreId = L"9NCTDW2W1BH8";
        out.error = L"This RAW file has no embedded preview, and Windows' Raw Image Extension isn't installed.";
    }
}

static std::wstring RawName(const std::wstring& ext) {
    if (ext == L".cr2") return L"Canon RAW (CR2)";
    if (ext == L".cr3") return L"Canon RAW (CR3)";
    if (ext == L".nef" || ext == L".nrw") return L"Nikon RAW (NEF)";
    if (ext == L".arw") return L"Sony RAW (ARW)";
    if (ext == L".dng") return L"Digital Negative (DNG)";
    return L"RAW";
}

static bool DecodeWithWic(IWICBitmapDecoder* dec, int orientation, UINT maxW, UINT maxH, bool animation, ImageData& out) {
    GUID g{};
    dec->GetContainerFormat(&g);
    if (out.format.empty()) out.format = ContainerName(dec);
    UINT count = 0;
    dec->GetFrameCount(&count);
    if (g == GUID_ContainerFormatGif && count > 1 && animation) return DecodeGif(dec, maxW, maxH, out);
    UINT index = 0;
    if (g == GUID_ContainerFormatIco && count > 1) {
        // the biggest icon in the file
        UINT best = 0;
        for (UINT i = 0; i < count; ++i) {
            Com<IWICBitmapFrameDecode> fr;
            UINT w = 0, h = 0;
            if (SUCCEEDED(dec->GetFrame(i, &fr)) && SUCCEEDED(fr->GetSize(&w, &h)) && w * h >= best) { best = w * h; index = i; }
        }
    }
    Com<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(index, &fr))) return false;
    Frame f;
    if (!WicFrame(fr, orientation, maxW, maxH, f.pix, &out.fullW, &out.fullH)) return false;
    out.fullRes = f.pix.w == out.fullW && f.pix.h == out.fullH;
    WICPixelFormatGUID pf{};
    fr->GetPixelFormat(&pf);
    if (g == GUID_ContainerFormatPng || g == GUID_ContainerFormatIco || g == GUID_ContainerFormatTiff || g == GUID_ContainerFormatBmp ||
        g == GUID_ContainerFormatGif || out.format == L"HEIF" || out.format == L"WebP")
        out.hasAlpha = HasAlpha(f.pix);
    out.frames.push_back(std::move(f));
    return true;
}

bool DecodeImage(const std::wstring& path, UINT maxW, UINT maxH, bool animation, ImageData& out) {
    out = ImageData{};
    std::wstring ext = ExtOf(path);
    IWICImagingFactory* f = Wic();
    if (!f) { out.error = L"Windows Imaging Component isn't available."; return false; }

    // sniff the first bytes: the extension can lie (a .jpg that's really a PNG)
    std::string head;
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            out.error = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? L"The file isn't there anymore." : L"Loupe can't read this file (it may be open in another app).";
            return false;
        }
        char b[64];
        DWORD got = 0;
        ReadFile(h, b, sizeof(b), &got, nullptr);
        head.assign(b, got);
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        CloseHandle(h);
        if (sz.QuadPart == 0) { out.error = L"This file is empty."; return false; }
    }
    const uint8_t* hb = (const uint8_t*)head.data();
    bool isWebp = head.size() >= 12 && memcmp(hb, "RIFF", 4) == 0 && memcmp(hb + 8, "WEBP", 4) == 0;
    bool isPng = head.size() >= 8 && memcmp(hb, "\x89PNG", 4) == 0;
    bool isSvg = ext == L".svg";
    bool isRaw = IsRawExtension(ext);

    if (isWebp) {
        std::string data;
        if (ReadFileBytes(path, data) && DecodeWebp(data, maxW, maxH, animation, out)) return true;
        out = ImageData{};
        // fall through to WIC (the Store WebP codec), just in case
    }
    if (isSvg) {
        std::string data;
        float nw = 0, nh = 0;
        if (ReadFileBytes(path, data) && offscreen::SvgSize(data, nw, nh)) {
            out.format = L"SVG";
            out.vector = true;
            out.hasAlpha = true;
            out.fullW = std::max<UINT>(1, (UINT)std::lround(nw));
            out.fullH = std::max<UINT>(1, (UINT)std::lround(nh));
            // vectors are drawn at the asked size (sharp at any zoom), or at 2x their natural size
            UINT tw, th;
            if (maxW && maxH) {
                double s = std::min((double)maxW / out.fullW, (double)maxH / out.fullH);
                tw = std::max<UINT>(1, (UINT)(out.fullW * s));
                th = std::max<UINT>(1, (UINT)(out.fullH * s));
            } else {
                double s = std::min(2.0, 8192.0 / std::max(out.fullW, out.fullH));
                tw = std::max<UINT>(1, (UINT)(out.fullW * s));
                th = std::max<UINT>(1, (UINT)(out.fullH * s));
            }
            Frame fr;
            if (offscreen::RasterSvg(data, tw, th, fr.pix)) {
                out.fullRes = !(maxW && maxH);
                out.frames.push_back(std::move(fr));
                return true;
            }
        }
        out = ImageData{};
        out.error = L"This SVG couldn't be drawn. It may use features Windows' SVG renderer doesn't support.";
        return false;
    }
    if (isPng && animation) {
        std::string data;
        if (ReadFileBytes(path, data) && data.find("acTL") != std::string::npos && data.find("acTL") < data.find("IDAT")) {
            if (DecodeApng(data, maxW, maxH, out)) return true;
            out = ImageData{};
        }
    }
    if (isRaw) {
        RawPreview rp;
        std::string jpg;
        if (FindRawPreview(path, rp) && ReadFileRange(path, rp.offset, rp.length, jpg)) {
            Com<IWICStream> st;
            Com<IWICBitmapDecoder> dec;
            if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)jpg.data(), (DWORD)jpg.size())) &&
                SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
                out.format = RawName(ext) + L" · preview";
                if (DecodeWithWic(dec, rp.orientation, maxW, maxH, false, out)) return true;
            }
            out = ImageData{};
        }
    }

    // everything else: WIC
    int orientation = 1;
    bool heifLike = head.size() >= 12 && memcmp(hb + 4, "ftyp", 4) == 0;
    if (!heifLike && !isWebp) {
        PhotoInfo info;
        if (ReadPhotoInfo(path, info, true)) orientation = info.orientation;
    }
    Com<IWICBitmapDecoder> dec;
    HRESULT hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) {
        if (isRaw) out.format = RawName(ext);
        if (heifLike) {
            std::string brand = head.substr(8, 4);
            out.format = brand == "avif" || brand == "avis" ? L"AVIF" : L"HEIF";
        }
        if (DecodeWithWic(dec, orientation, maxW, maxH, animation, out)) return true;
        hr = E_FAIL;
    }
    std::wstring fmt = out.format;
    out = ImageData{};
    // a codec from the Store would open it; meanwhile show Windows' own thumbnail if there is one
    CodecHint(heifLike && ext != L".avif" && ext != L".heic" && ext != L".heif" ? std::wstring(L".heic") : ext, out);
    Frame fr;
    if (ShellPreview(path, std::max<UINT>(maxW ? std::max(maxW, maxH) : 1600, 256), fr.pix)) {
        out.format = fmt.empty() ? L"Preview" : fmt + L" · preview";
        out.fullW = fr.pix.w;
        out.fullH = fr.pix.h;
        out.fullRes = true;
        out.frames.push_back(std::move(fr));
        out.error.clear();
        return true;
    }
    if (out.error.empty()) {
        out.error = hr == (HRESULT)WINCODEC_ERR_COMPONENTNOTFOUND ? L"Windows doesn't have a decoder for this kind of image."
                                                                 : L"This image is damaged or in a format Loupe can't read.";
    }
    return false;
}

// ---------------------------------------------------------------- encoding
bool CanEncodeExtension(const std::wstring& ext) {
    return ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif" || ext == L".png" || ext == L".bmp" ||
           ext == L".dib" || ext == L".tif" || ext == L".tiff" || ext == L".gif";
}

static bool EncoderFor(const std::wstring& fmt, GUID& container, WICPixelFormatGUID& pf, const wchar_t*& orientPath) {
    orientPath = nullptr;
    if (fmt == L"jpg" || fmt == L"jpeg" || fmt == L"jpe" || fmt == L"jfif") { container = GUID_ContainerFormatJpeg; pf = GUID_WICPixelFormat24bppBGR; orientPath = L"/app1/ifd/{ushort=274}"; return true; }
    if (fmt == L"png") { container = GUID_ContainerFormatPng; pf = GUID_WICPixelFormat32bppBGRA; return true; }
    if (fmt == L"bmp" || fmt == L"dib") { container = GUID_ContainerFormatBmp; pf = GUID_WICPixelFormat32bppBGRA; return true; }
    if (fmt == L"tif" || fmt == L"tiff") { container = GUID_ContainerFormatTiff; pf = GUID_WICPixelFormat32bppBGRA; orientPath = L"/ifd/{ushort=274}"; return true; }
    if (fmt == L"gif") { container = GUID_ContainerFormatGif; pf = GUID_WICPixelFormat8bppIndexed; return true; }
    return false;
}

bool EncodeImage(const Pixels& p, const std::wstring& path, const std::wstring& format, int quality,
                 const std::wstring& metadataFrom, std::wstring* error) {
    IWICImagingFactory* f = Wic();
    GUID container;
    WICPixelFormatGUID pf;
    const wchar_t* orientPath;
    if (!EncoderFor(Lower(format), container, pf, orientPath)) { if (error) *error = L"Loupe can't save in that format."; return false; }
    std::wstring tmp = path + L".loupe-tmp";
    bool ok = false;
    {
        Com<IWICStream> st;
        Com<IWICBitmapEncoder> enc;
        Com<IWICBitmapFrameEncode> fe;
        Com<IPropertyBag2> props;
        Com<IWICBitmap> src;
        Com<IWICFormatConverter> conv;
        HRESULT hr = f->CreateStream(&st);
        if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = f->CreateEncoder(container, nullptr, &enc);
        if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fe, &props);
        if (SUCCEEDED(hr) && container == GUID_ContainerFormatJpeg && props) {
            PROPBAG2 opt{};
            opt.pstrName = (LPOLESTR)L"ImageQuality";
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = std::clamp(quality, 1, 100) / 100.0f;
            props->Write(1, &opt, &v);
        }
        if (SUCCEEDED(hr)) hr = fe->Initialize(props);
        if (SUCCEEDED(hr)) hr = fe->SetSize(p.w, p.h);
        WICPixelFormatGUID want = pf;
        if (SUCCEEDED(hr)) hr = fe->SetPixelFormat(&want);
        // keep the original's metadata (camera, date, GPS) when the container matches
        if (SUCCEEDED(hr) && !metadataFrom.empty()) {
            Com<IWICBitmapDecoder> sd;
            Com<IWICBitmapFrameDecode> sf;
            Com<IWICMetadataBlockReader> br;
            Com<IWICMetadataBlockWriter> bw;
            GUID sc{};
            if (SUCCEEDED(f->CreateDecoderFromFilename(metadataFrom.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &sd)) &&
                SUCCEEDED(sd->GetContainerFormat(&sc)) && sc == container && SUCCEEDED(sd->GetFrame(0, &sf)) &&
                SUCCEEDED(sf->QueryInterface(IID_PPV_ARGS(&br))) && SUCCEEDED(fe->QueryInterface(IID_PPV_ARGS(&bw)))) {
                if (SUCCEEDED(bw->InitializeFromBlockReader(br)) && orientPath) {
                    // pixels are saved upright now
                    Com<IWICMetadataQueryWriter> qw;
                    if (SUCCEEDED(fe->GetMetadataQueryWriter(&qw))) {
                        PROPVARIANT v;
                        PropVariantInit(&v);
                        v.vt = VT_UI2;
                        v.uiVal = 1;
                        qw->SetMetadataByName(orientPath, &v);
                    }
                }
            }
        }
        if (SUCCEEDED(hr)) hr = f->CreateBitmapFromMemory(p.w, p.h, GUID_WICPixelFormat32bppPBGRA, p.w * 4, (UINT)p.px.size(), (BYTE*)p.px.data(), &src);
        if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr)) {
            Com<IWICPalette> pal;
            if (want == GUID_WICPixelFormat8bppIndexed) {
                f->CreatePalette(&pal);
                if (pal) pal->InitializeFromBitmap(src, 256, TRUE);
                fe->SetPalette(pal);
            }
            hr = conv->Initialize(src, want, pal ? WICBitmapDitherTypeErrorDiffusion : WICBitmapDitherTypeNone, pal, 0,
                                  pal ? WICBitmapPaletteTypeCustom : WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(hr)) hr = fe->WriteSource(conv, nullptr);
        if (SUCCEEDED(hr)) hr = fe->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        ok = SUCCEEDED(hr);
        if (!ok && error) { wchar_t b[96]; swprintf(b, 96, L"Saving failed (error 0x%08X).", (unsigned)hr); *error = b; }
    }
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        if (error) *error = L"Couldn't write the file. Is it read-only or open in another app?";
        return false;
    }
    return true;
}

bool EncodePngToMemory(const Pixels& p, std::string& out) {
    IWICImagingFactory* f = Wic();
    IStream* ms = SHCreateMemStream(nullptr, 0);
    if (!ms) return false;
    bool ok = false;
    {
        Com<IWICBitmapEncoder> enc;
        Com<IWICBitmapFrameEncode> fe;
        Com<IWICBitmap> src;
        Com<IWICFormatConverter> conv;
        WICPixelFormatGUID want = GUID_WICPixelFormat32bppBGRA;
        HRESULT hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        if (SUCCEEDED(hr)) hr = enc->Initialize(ms, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fe, nullptr);
        if (SUCCEEDED(hr)) hr = fe->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = fe->SetSize(p.w, p.h);
        if (SUCCEEDED(hr)) hr = fe->SetPixelFormat(&want);
        if (SUCCEEDED(hr)) hr = f->CreateBitmapFromMemory(p.w, p.h, GUID_WICPixelFormat32bppPBGRA, p.w * 4, (UINT)p.px.size(), (BYTE*)p.px.data(), &src);
        if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr)) hr = conv->Initialize(src, want, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = fe->WriteSource(conv, nullptr);
        if (SUCCEEDED(hr)) hr = fe->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        if (SUCCEEDED(hr)) {
            STATSTG s{};
            ms->Stat(&s, STATFLAG_NONAME);
            LARGE_INTEGER zero{};
            ms->Seek(zero, STREAM_SEEK_SET, nullptr);
            out.resize((size_t)s.cbSize.QuadPart);
            ULONG got = 0;
            ok = SUCCEEDED(ms->Read(&out[0], (ULONG)out.size(), &got)) && got == out.size();
        }
    }
    ms->Release();
    return ok;
}
