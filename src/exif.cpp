// Loupe - photo metadata (see exif.h)
#include "exif.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

// ---------------------------------------------------------------- file reader
// Keeps the first chunk of the file in memory; anything further is read on demand.
namespace {

class Reader {
public:
    explicit Reader(const std::wstring& path, size_t head = 256 * 1024) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz;
        if (GetFileSizeEx(h_, &sz)) size_ = (uint64_t)sz.QuadPart;
        head_.resize((size_t)std::min<uint64_t>(size_, head));
        DWORD got = 0;
        if (!head_.empty() && (!ReadFile(h_, &head_[0], (DWORD)head_.size(), &got, nullptr) || got != head_.size())) head_.resize(got);
    }
    ~Reader() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    bool ok() const { return h_ != INVALID_HANDLE_VALUE && size_ > 0; }
    uint64_t size() const { return size_; }
    const std::string& head() const { return head_; }

    bool Read(uint64_t off, void* dst, size_t n) {
        if (off + n > size_) return false;
        if (off + n <= head_.size()) { memcpy(dst, head_.data() + off, n); return true; }
        OVERLAPPED ov{};
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(h_, dst, (DWORD)n, &got, &ov) && got == n;
    }
    bool ReadStr(uint64_t off, size_t n, std::string& out) {
        out.resize(n);
        return n == 0 || Read(off, &out[0], n);
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    std::string head_;
};

uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint64_t BE64(const uint8_t* p) { return ((uint64_t)BE32(p) << 32) | BE32(p + 4); }

// ---------------------------------------------------------------- TIFF
struct Entry {
    uint16_t tag = 0, type = 0;
    uint32_t count = 0;
    uint64_t valuePos = 0;   // absolute file offset of the value
};

class Tiff {
public:
    Tiff(Reader& r, uint64_t base) : r_(r), base_(base) {}
    bool Init() {
        uint8_t h[8];
        if (!r_.Read(base_, h, 8)) return false;
        if (h[0] == 'I' && h[1] == 'I') le_ = true;
        else if (h[0] == 'M' && h[1] == 'M') le_ = false;
        else return false;
        if (U16(h + 2) != 42) return false;
        first_ = U32(h + 4);
        return true;
    }
    uint32_t FirstIfd() const { return first_; }
    uint64_t Base() const { return base_; }

    bool ReadIfd(uint32_t off, std::vector<Entry>& out, uint32_t* next) {
        out.clear();
        if (next) *next = 0;
        if (off == 0 || off < 8 || base_ + off + 2 > r_.size()) return false;
        uint8_t c[2];
        if (!r_.Read(base_ + off, c, 2)) return false;
        uint16_t n = U16(c);
        if (n == 0 || n > 1000) return false;
        std::string buf;
        if (!r_.ReadStr(base_ + off + 2, n * 12 + 4, buf)) {
            if (!r_.ReadStr(base_ + off + 2, n * 12, buf)) return false;
            buf.append(4, '\0');
        }
        const uint8_t* p = (const uint8_t*)buf.data();
        for (uint16_t i = 0; i < n; ++i, p += 12) {
            Entry e;
            e.tag = U16(p);
            e.type = U16(p + 2);
            e.count = U32(p + 4);
            uint64_t bytes = (uint64_t)TypeSize(e.type) * e.count;
            e.valuePos = bytes <= 4 ? base_ + off + 2 + i * 12 + 8 : base_ + U32(p + 8);
            out.push_back(e);
        }
        if (next) *next = U32((const uint8_t*)buf.data() + n * 12);
        return true;
    }

    static const Entry* Find(const std::vector<Entry>& v, uint16_t tag) {
        for (auto& e : v) if (e.tag == tag) return &e;
        return nullptr;
    }

    // first value as an unsigned integer (BYTE, SHORT, LONG)
    bool UInt(const Entry& e, uint32_t& out, uint32_t index = 0) {
        if (index >= e.count) return false;
        uint8_t b[4];
        switch (e.type) {
            case 1: case 7: if (!r_.Read(e.valuePos + index, b, 1)) return false; out = b[0]; return true;
            case 3: case 8: if (!r_.Read(e.valuePos + index * 2, b, 2)) return false; out = U16(b); return true;
            case 4: case 9: case 13: if (!r_.Read(e.valuePos + index * 4, b, 4)) return false; out = U32(b); return true;
            default: return false;
        }
    }
    bool Rational(const Entry& e, double& out, uint32_t index = 0) {
        if ((e.type != 5 && e.type != 10) || index >= e.count) {
            uint32_t u;
            if (UInt(e, u, index)) { out = u; return true; }
            return false;
        }
        uint8_t b[8];
        if (!r_.Read(e.valuePos + index * 8, b, 8)) return false;
        if (e.type == 5) {
            uint32_t n = U32(b), d = U32(b + 4);
            if (d == 0) return false;
            out = (double)n / d;
        } else {
            int32_t n = (int32_t)U32(b), d = (int32_t)U32(b + 4);
            if (d == 0) return false;
            out = (double)n / d;
        }
        return true;
    }
    bool Ascii(const Entry& e, std::wstring& out) {
        if (e.count == 0 || e.count > 4096) return false;
        std::string s;
        if (!r_.ReadStr(e.valuePos, e.count, s)) return false;
        size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        // camera strings are ASCII, but some phones write UTF-8
        out = Trim(Utf8ToWide(s));
        return !out.empty();
    }

private:
    Reader& r_;
    uint64_t base_;
    bool le_ = true;
    uint32_t first_ = 0;
    uint16_t U16(const uint8_t* p) const { return le_ ? (uint16_t)(p[0] | (p[1] << 8)) : BE16(p); }
    uint32_t U32(const uint8_t* p) const {
        return le_ ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)) : BE32(p);
    }
    static int TypeSize(uint16_t t) {
        switch (t) {
            case 1: case 2: case 6: case 7: return 1;
            case 3: case 8: return 2;
            case 4: case 9: case 11: case 13: return 4;
            case 5: case 10: case 12: return 8;
            default: return 1;
        }
    }
};

bool ParseDate(const std::wstring& s, SYSTEMTIME& st) {
    int Y, M, D, h = 0, m = 0, sec = 0;
    if (swscanf(s.c_str(), L"%d:%d:%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 3 &&
        swscanf(s.c_str(), L"%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 3) return false;
    if (Y < 1900 || Y > 2200 || M < 1 || M > 12 || D < 1 || D > 31) return false;
    st = SYSTEMTIME{};
    st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
    st.wHour = (WORD)std::clamp(h, 0, 23); st.wMinute = (WORD)std::clamp(m, 0, 59); st.wSecond = (WORD)std::clamp(sec, 0, 59);
    return true;
}

// Reads IFD0 (+ the Exif and GPS IFDs it points to). Separate IFD offsets are for CR3, where each
// lives in its own TIFF block.
void ReadIfd0(Tiff& t, uint32_t off, PhotoInfo& info, bool quick, uint32_t* exifOff, uint32_t* gpsOff) {
    std::vector<Entry> ifd;
    if (!t.ReadIfd(off, ifd, nullptr)) return;
    uint32_t u;
    if (auto* e = Tiff::Find(ifd, 0x0112); e && t.UInt(*e, u) && u >= 1 && u <= 8) info.orientation = (int)u;
    if (auto* e = Tiff::Find(ifd, 0x8769); e && t.UInt(*e, u)) *exifOff = u;
    if (auto* e = Tiff::Find(ifd, 0x8825); e && t.UInt(*e, u)) *gpsOff = u;
    if (!info.hasTaken)
        if (auto* e = Tiff::Find(ifd, 0x0132)) { std::wstring s; if (t.Ascii(*e, s)) info.hasTaken = ParseDate(s, info.taken); }
    if (quick) return;
    if (auto* e = Tiff::Find(ifd, 0x010F)) t.Ascii(*e, info.make);
    if (auto* e = Tiff::Find(ifd, 0x0110)) t.Ascii(*e, info.model);
    if (auto* e = Tiff::Find(ifd, 0x0131)) t.Ascii(*e, info.software);
}

void ReadExifIfd(Tiff& t, uint32_t off, PhotoInfo& info, bool quick) {
    std::vector<Entry> ifd;
    if (!t.ReadIfd(off, ifd, nullptr)) return;
    std::wstring s;
    // DateTimeOriginal beats DateTime (which editors rewrite)
    if (auto* e = Tiff::Find(ifd, 0x9003); e && t.Ascii(*e, s)) { SYSTEMTIME st; if (ParseDate(s, st)) { info.taken = st; info.hasTaken = true; } }
    else if (auto* e2 = Tiff::Find(ifd, 0x9004); e2 && t.Ascii(*e2, s)) { SYSTEMTIME st; if (ParseDate(s, st)) { info.taken = st; info.hasTaken = true; } }
    if (quick) return;
    uint32_t u;
    double d;
    if (auto* e = Tiff::Find(ifd, 0x829A); e && t.Rational(*e, d)) info.exposureTime = d;
    if (auto* e = Tiff::Find(ifd, 0x829D); e && t.Rational(*e, d)) info.fNumber = d;
    if (auto* e = Tiff::Find(ifd, 0x8827); e && t.UInt(*e, u)) info.iso = (int)u;
    if (auto* e = Tiff::Find(ifd, 0x9204); e && t.Rational(*e, d)) info.exposureBias = d;
    if (auto* e = Tiff::Find(ifd, 0x9209); e && t.UInt(*e, u)) info.flash = (int)u;
    if (auto* e = Tiff::Find(ifd, 0x920A); e && t.Rational(*e, d)) info.focal = d;
    if (auto* e = Tiff::Find(ifd, 0xA405); e && t.UInt(*e, u)) info.focal35 = (int)u;
    if (auto* e = Tiff::Find(ifd, 0xA434)) t.Ascii(*e, info.lens);
    if (auto* e = Tiff::Find(ifd, 0xA002); e && t.UInt(*e, u)) info.pixelW = u;
    if (auto* e = Tiff::Find(ifd, 0xA003); e && t.UInt(*e, u)) info.pixelH = u;
    if (info.fNumber == 0) if (auto* e = Tiff::Find(ifd, 0x9202); e && t.Rational(*e, d)) info.fNumber = std::pow(2.0, d / 2);  // ApertureValue
    if (info.exposureTime == 0) if (auto* e = Tiff::Find(ifd, 0x9201); e && t.Rational(*e, d)) info.exposureTime = std::pow(2.0, -d);
}

void ReadGpsIfd(Tiff& t, uint32_t off, PhotoInfo& info) {
    std::vector<Entry> ifd;
    if (!t.ReadIfd(off, ifd, nullptr)) return;
    auto coord = [&](uint16_t refTag, uint16_t tag, double& out) {
        const Entry* e = Tiff::Find(ifd, tag);
        if (!e || e->count < 3) return false;
        double dd, mm, ss;
        if (!t.Rational(*e, dd, 0) || !t.Rational(*e, mm, 1) || !t.Rational(*e, ss, 2)) return false;
        out = dd + mm / 60 + ss / 3600;
        std::wstring ref;
        if (const Entry* r = Tiff::Find(ifd, refTag)) t.Ascii(*r, ref);
        if (ref == L"S" || ref == L"W") out = -out;
        return true;
    };
    double lat, lon;
    if (coord(1, 2, lat) && coord(3, 4, lon) && !(lat == 0 && lon == 0) && std::fabs(lat) <= 90 && std::fabs(lon) <= 180) {
        info.hasGps = true;
        info.lat = lat;
        info.lon = lon;
        double a;
        if (auto* e = Tiff::Find(ifd, 6); e && t.Rational(*e, a)) {
            uint32_t ref = 0;
            if (auto* r = Tiff::Find(ifd, 5)) t.UInt(*r, ref);
            info.alt = ref == 1 ? -a : a;
            info.hasAlt = true;
        }
    }
}

bool ReadTiffAt(Reader& r, uint64_t base, PhotoInfo& info, bool quick) {
    Tiff t(r, base);
    if (!t.Init()) return false;
    uint32_t exifOff = 0, gpsOff = 0;
    ReadIfd0(t, t.FirstIfd(), info, quick, &exifOff, &gpsOff);
    if (exifOff) ReadExifIfd(t, exifOff, info, quick);
    if (gpsOff && !quick) ReadGpsIfd(t, gpsOff, info);
    return true;
}

// ---------------------------------------------------------------- containers
bool FromJpeg(Reader& r, PhotoInfo& info, bool quick) {
    uint64_t pos = 2;
    uint8_t m[4];
    while (pos + 4 <= r.size() && pos < 4 * 1024 * 1024) {
        if (!r.Read(pos, m, 4)) return false;
        if (m[0] != 0xFF) return false;
        uint8_t marker = m[1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { pos += 2; continue; }
        if (marker == 0xFF) { pos += 1; continue; }
        if (marker == 0xDA || marker == 0xD9) return false;   // image data: no EXIF before it
        uint16_t len = BE16(m + 2);
        if (len < 2) return false;
        if (marker == 0xE1 && len > 8) {
            char id[6];
            if (r.Read(pos + 4, id, 6) && memcmp(id, "Exif\0\0", 6) == 0) return ReadTiffAt(r, pos + 10, info, quick);
        }
        pos += 2 + len;
    }
    return false;
}

bool FromPng(Reader& r, PhotoInfo& info, bool quick) {
    uint64_t pos = 8;
    uint8_t h[8];
    while (pos + 12 <= r.size() && pos < 8 * 1024 * 1024) {
        if (!r.Read(pos, h, 8)) return false;
        uint32_t len = BE32(h);
        if (memcmp(h + 4, "eXIf", 4) == 0) {
            char id[6];
            uint64_t at = pos + 8;
            if (r.Read(at, id, 6) && memcmp(id, "Exif\0\0", 6) == 0) at += 6;
            return ReadTiffAt(r, at, info, quick);
        }
        if (memcmp(h + 4, "IEND", 4) == 0) return false;
        pos += 12 + (uint64_t)len;
    }
    return false;
}

bool FromWebp(Reader& r, PhotoInfo& info, bool quick) {
    uint64_t pos = 12;
    uint8_t h[8];
    while (pos + 8 <= r.size()) {
        if (!r.Read(pos, h, 8)) return false;
        uint32_t len = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
        if (memcmp(h, "EXIF", 4) == 0) {
            char id[6];
            uint64_t at = pos + 8;
            if (r.Read(at, id, 6) && memcmp(id, "Exif\0\0", 6) == 0) at += 6;
            return ReadTiffAt(r, at, info, quick);
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

// ISO base media file format (HEIF, AVIF, CR3)
struct Box { char type[5] = {}; uint64_t start = 0, data = 0, end = 0; };

bool NextBox(Reader& r, uint64_t pos, uint64_t limit, Box& b) {
    uint8_t h[16];
    if (pos + 8 > limit || !r.Read(pos, h, 8)) return false;
    uint64_t size = BE32(h);
    memcpy(b.type, h + 4, 4);
    b.start = pos;
    b.data = pos + 8;
    if (size == 1) {
        if (!r.Read(pos + 8, h + 8, 8)) return false;
        size = BE64(h + 8);
        b.data = pos + 16;
    } else if (size == 0) {
        size = limit - pos;
    }
    if (size < 8 || pos + size > limit) return false;
    b.end = pos + size;
    return true;
}

template <class F>
void ForEachBox(Reader& r, uint64_t from, uint64_t to, F f) {
    Box b;
    uint64_t pos = from;
    int guard = 0;
    while (NextBox(r, pos, to, b) && guard++ < 10000) {
        if (!f(b)) return;
        pos = b.end;
    }
}

const uint8_t kCanonUuid[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0, 0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};

bool FromCr3(Reader& r, PhotoInfo& info, bool quick) {
    bool found = false;
    ForEachBox(r, 0, r.size(), [&](const Box& top) {
        if (memcmp(top.type, "moov", 4) != 0) return true;
        ForEachBox(r, top.data, top.end, [&](const Box& b) {
            if (memcmp(b.type, "uuid", 4) != 0) return true;
            uint8_t id[16];
            if (!r.Read(b.data, id, 16) || memcmp(id, kCanonUuid, 16) != 0) return true;
            // CMT1 = IFD0, CMT2 = Exif IFD, CMT4 = GPS IFD, each a complete TIFF block
            ForEachBox(r, b.data + 16, b.end, [&](const Box& c) {
                if (memcmp(c.type, "CMT1", 4) == 0) {
                    Tiff t(r, c.data);
                    uint32_t e = 0, g = 0;
                    if (t.Init()) { ReadIfd0(t, t.FirstIfd(), info, quick, &e, &g); found = true; }
                } else if (memcmp(c.type, "CMT2", 4) == 0) {
                    Tiff t(r, c.data);
                    if (t.Init()) { ReadExifIfd(t, t.FirstIfd(), info, quick); found = true; }
                } else if (memcmp(c.type, "CMT4", 4) == 0 && !quick) {
                    Tiff t(r, c.data);
                    if (t.Init()) ReadGpsIfd(t, t.FirstIfd(), info);
                }
                return true;
            });
            return false;
        });
        return false;
    });
    return found;
}

bool FromHeif(Reader& r, PhotoInfo& info, bool quick) {
    uint64_t exifOff = 0, exifLen = 0;
    ForEachBox(r, 0, r.size(), [&](const Box& top) {
        if (memcmp(top.type, "meta", 4) != 0) return true;
        uint32_t exifId = 0;
        bool haveId = false;
        uint64_t children = top.data + 4;   // full box
        ForEachBox(r, children, top.end, [&](const Box& b) {
            if (memcmp(b.type, "iinf", 4) != 0) return true;
            uint8_t vh[4];
            if (!r.Read(b.data, vh, 4)) return false;
            uint64_t p = b.data + 4 + (vh[0] == 0 ? 2 : 4);
            ForEachBox(r, p, b.end, [&](const Box& e) {
                if (memcmp(e.type, "infe", 4) != 0) return true;
                uint8_t d[12];
                if (!r.Read(e.data, d, 12)) return true;
                int ver = d[0];
                if (ver < 2) return true;
                uint32_t id = ver == 2 ? BE16(d + 4) : BE32(d + 4);
                const uint8_t* type = ver == 2 ? d + 8 : d + 10;
                if (ver == 3) { uint8_t t4[4]; if (!r.Read(e.data + 10, t4, 4)) return true; if (memcmp(t4, "Exif", 4) == 0) { exifId = id; haveId = true; return false; } return true; }
                if (memcmp(type, "Exif", 4) == 0) { exifId = id; haveId = true; return false; }
                return true;
            });
            return false;
        });
        if (!haveId) return false;
        ForEachBox(r, children, top.end, [&](const Box& b) {
            if (memcmp(b.type, "iloc", 4) != 0) return true;
            std::string d;
            uint64_t n = std::min<uint64_t>(b.end - b.data, 64 * 1024);
            if (!r.ReadStr(b.data, (size_t)n, d)) return false;
            const uint8_t* p = (const uint8_t*)d.data();
            const uint8_t* end = p + d.size();
            int ver = p[0];
            int offSize = p[4] >> 4, lenSize = p[4] & 15, baseSize = p[5] >> 4, idxSize = (ver == 1 || ver == 2) ? (p[5] & 15) : 0;
            p += 6;
            auto rd = [&](int bytes) -> uint64_t {
                uint64_t v = 0;
                for (int i = 0; i < bytes && p < end; ++i) v = (v << 8) | *p++;
                return v;
            };
            uint32_t count = ver < 2 ? (uint32_t)rd(2) : (uint32_t)rd(4);
            for (uint32_t i = 0; i < count && p < end; ++i) {
                uint32_t id = ver < 2 ? (uint32_t)rd(2) : (uint32_t)rd(4);
                int method = 0;
                if (ver == 1 || ver == 2) method = (int)(rd(2) & 15);
                rd(2);   // data reference index
                uint64_t base = rd(baseSize);
                uint32_t extents = (uint32_t)rd(2);
                for (uint32_t k = 0; k < extents && p < end; ++k) {
                    if (idxSize) rd(idxSize);
                    uint64_t off = rd(offSize), len = rd(lenSize);
                    if (id == exifId && k == 0 && method == 0) { exifOff = base + off; exifLen = len; }
                }
                if (id == exifId) break;
            }
            return false;
        });
        return false;
    });
    if (!exifOff || exifLen < 10 || exifOff + exifLen > r.size()) return false;
    uint8_t h[4];
    if (!r.Read(exifOff, h, 4)) return false;
    uint64_t at = exifOff + 4 + BE32(h);
    char id[6];
    if (r.Read(at, id, 6) && memcmp(id, "Exif\0\0", 6) == 0) at += 6;
    return ReadTiffAt(r, at, info, quick);
}

} // namespace

// ---------------------------------------------------------------- public
bool IsRawExtension(const std::wstring& ext) {
    static const wchar_t* raws[] = {L".cr2", L".cr3", L".nef", L".arw", L".dng", L".nrw", L".orf", L".rw2", L".raf", L".pef", L".srw"};
    for (auto* r : raws) if (ext == r) return true;
    return false;
}

bool ReadPhotoInfo(const std::wstring& path, PhotoInfo& info, bool quick) {
    info = PhotoInfo{};
    Reader r(path, quick ? 64 * 1024 : 256 * 1024);
    if (!r.ok() || r.head().size() < 12) return false;
    const uint8_t* h = (const uint8_t*)r.head().data();
    if (h[0] == 0xFF && h[1] == 0xD8) return FromJpeg(r, info, quick);
    if (memcmp(h, "\x89PNG", 4) == 0) return FromPng(r, info, quick);
    if (memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WEBP", 4) == 0) return FromWebp(r, info, quick);
    if ((h[0] == 'I' && h[1] == 'I') || (h[0] == 'M' && h[1] == 'M')) return ReadTiffAt(r, 0, info, quick);
    if (memcmp(h + 4, "ftyp", 4) == 0) {
        if (memcmp(h + 8, "crx ", 4) == 0) return FromCr3(r, info, quick);
        return FromHeif(r, info, quick);
    }
    return false;
}

bool ReadFileRange(const std::wstring& path, uint64_t offset, uint64_t length, std::string& out) {
    if (length > 512ull * 1024 * 1024) return false;
    Reader r(path, 0);
    return r.ok() && r.ReadStr(offset, (size_t)length, out);
}

namespace {
struct Candidate { uint64_t off, len; };

void CollectTiffPreviews(Reader& r, Tiff& t, uint32_t off, int depth, std::vector<Candidate>& out) {
    if (depth > 3) return;
    std::vector<Entry> ifd;
    uint32_t next = 0;
    int chain = 0;
    while (off && chain++ < 8 && t.ReadIfd(off, ifd, &next)) {
        uint32_t a = 0, b = 0, comp = 0;
        if (auto* e1 = Tiff::Find(ifd, 0x0201))
            if (auto* e2 = Tiff::Find(ifd, 0x0202))
                if (t.UInt(*e1, a) && t.UInt(*e2, b) && b > 0) out.push_back({t.Base() + a, b});
        if (auto* c = Tiff::Find(ifd, 0x0103)) t.UInt(*c, comp);
        if (comp == 6 || comp == 7) {
            auto* so = Tiff::Find(ifd, 0x0111);
            auto* sc = Tiff::Find(ifd, 0x0117);
            if (so && sc && so->count == 1 && t.UInt(*so, a) && t.UInt(*sc, b) && b > 0) out.push_back({t.Base() + a, b});
        }
        if (auto* sub = Tiff::Find(ifd, 0x014A)) {
            std::vector<uint32_t> subs;
            for (uint32_t i = 0; i < sub->count && i < 8; ++i) {
                uint32_t s;
                if (sub->type == 13 || sub->type == 4) { if (t.UInt(*sub, s, i)) subs.push_back(s); }
            }
            for (uint32_t s : subs) CollectTiffPreviews(r, t, s, depth + 1, out);
        }
        if (depth > 0) break;   // SubIFDs don't chain in practice
        off = next;
    }
}

bool IsJpegAt(Reader& r, uint64_t off) {
    uint8_t m[3];
    return r.Read(off, m, 3) && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF;
}
} // namespace

bool FindRawPreview(const std::wstring& path, RawPreview& out) {
    Reader r(path, 256 * 1024);
    if (!r.ok() || r.head().size() < 16) return false;
    const uint8_t* h = (const uint8_t*)r.head().data();
    std::vector<Candidate> cands;
    PhotoInfo info;
    if (memcmp(h + 4, "ftyp", 4) == 0 && memcmp(h + 8, "crx ", 4) == 0) {
        FromCr3(r, info, true);
        // the first track of a CR3 is a full-resolution JPEG
        ForEachBox(r, 0, r.size(), [&](const Box& top) {
            if (memcmp(top.type, "moov", 4) != 0) return true;
            ForEachBox(r, top.data, top.end, [&](const Box& trak) {
                if (memcmp(trak.type, "trak", 4) != 0) return true;
                uint64_t off = 0, len = 0;
                std::function<void(uint64_t, uint64_t, int)> walk = [&](uint64_t a, uint64_t b, int depth) {
                    if (depth > 4) return;
                    ForEachBox(r, a, b, [&](const Box& x) {
                        if (!memcmp(x.type, "mdia", 4) || !memcmp(x.type, "minf", 4) || !memcmp(x.type, "stbl", 4)) walk(x.data, x.end, depth + 1);
                        else if (!memcmp(x.type, "stsz", 4)) {
                            uint8_t d[16];
                            if (r.Read(x.data, d, 16)) { len = BE32(d + 4); if (!len) len = BE32(d + 12); }
                        } else if (!memcmp(x.type, "co64", 4)) {
                            uint8_t d[16];
                            if (r.Read(x.data, d, 16)) off = BE64(d + 8);
                        } else if (!memcmp(x.type, "stco", 4)) {
                            uint8_t d[12];
                            if (r.Read(x.data, d, 12)) off = BE32(d + 8);
                        }
                        return true;
                    });
                };
                walk(trak.data, trak.end, 0);
                if (off && len) cands.push_back({off, len});
                return false;   // only the first track
            });
            return false;
        });
        // the smaller PRVW preview, in a top-level uuid box
        ForEachBox(r, 0, r.size(), [&](const Box& b) {
            if (memcmp(b.type, "uuid", 4) != 0 || b.end - b.data > 64 * 1024 * 1024) return true;
            std::string d;
            if (!r.ReadStr(b.data, (size_t)std::min<uint64_t>(b.end - b.data, 128), d)) return true;
            size_t p = d.find("PRVW");
            if (p == std::string::npos) return true;
            for (size_t k = p; k + 3 < d.size(); ++k) {
                if ((uint8_t)d[k] == 0xFF && (uint8_t)d[k + 1] == 0xD8 && k >= 4) {
                    uint32_t len = BE32((const uint8_t*)d.data() + k - 4);
                    cands.push_back({b.data + k, len});
                    break;
                }
            }
            return false;
        });
    } else {
        Tiff t(r, 0);
        if (!t.Init()) return false;
        uint32_t e = 0, g = 0;
        ReadIfd0(t, t.FirstIfd(), info, true, &e, &g);
        CollectTiffPreviews(r, t, t.FirstIfd(), 0, cands);
    }
    out = RawPreview{};
    out.orientation = info.orientation;
    for (auto& c : cands) {
        if (c.off + c.len > r.size() || c.len < 1024 || !IsJpegAt(r, c.off)) continue;
        if (c.len > out.length) { out.offset = c.off; out.length = c.len; }
    }
    return out.length > 0;
}

std::wstring FormatExposure(double s) {
    wchar_t b[32];
    if (s <= 0) return L"";
    if (s < 0.5) swprintf(b, 32, L"1/%.0f s", 1.0 / s);
    else if (std::fabs(s - std::round(s)) < 0.05) swprintf(b, 32, L"%.0f s", s);
    else swprintf(b, 32, L"%.1f s", s);
    return b;
}

std::wstring FormatTaken(const SYSTEMTIME& st) {
    wchar_t d[128] = {}, t[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, d, 128, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
    return std::wstring(d) + L", " + t;
}
