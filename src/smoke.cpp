// Loupe - end-to-end smoke test (Loupe.exe --smoke <log> <folder>), run by CI on Windows.
//
// Opens every image in the folder the way a user would, then exercises zoom, pan, the filmstrip,
// the info panel, the grid, compare, animation, editing + saving a copy, settings, themes, the
// slideshow, the clipboard, rename + undo and delete + undo. It writes a log and screenshots, and
// exits non-zero if anything didn't work.
#include "app.h"
#include "util.h"
#include "image.h"
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace app;

double UiFirstPixelsMs();
void UiTestSaveCopy(const std::wstring& target);
void UiTestDialogPress(int b);
void UiTestPanBy(float dx, float dy);

namespace {

struct SmokeStep {
    std::wstring name;
    std::function<void()> start;
    std::function<bool()> done;
    DWORD timeoutMs = 15000;
    std::function<void()> after;
};

std::vector<SmokeStep> s_steps;
size_t s_cur = 0;
bool s_started = false, s_active = false;
DWORD s_stepStart = 0;
std::wstring s_log, s_dir, s_shots;
std::vector<std::wstring> s_failures;
int s_exit = 1;
int s_shotNo = 0;

void Line(const std::wstring& text) {
    HANDLE h = CreateFileW(s_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string u = WideToUtf8(text + L"\r\n");
    DWORD w;
    WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(h);
}

void Fail(const std::wstring& why) {
    s_failures.push_back(why);
    Line(L"  FAIL: " + why);
}

void Shot(const std::wstring& name) {
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    bool ok = PrintWindow(hwnd, mem, 3 /*PW_CLIENTONLY | PW_RENDERFULLCONTENT*/) != 0;
    if (ok && bits) {
        Pixels p;
        p.w = w;
        p.h = h;
        p.px.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)w * h * 4);
        for (size_t i = 3; i < p.px.size(); i += 4) p.px[i] = 255;
        wchar_t prefix[16];
        swprintf(prefix, 16, L"%02d-", ++s_shotNo);
        std::wstring err;
        EncodeImage(p, s_shots + L"\\" + prefix + name + L".png", L"png", 90, L"", &err);
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

bool Elapsed(DWORD ms) { return GetTickCount() - s_stepStart >= ms; }

std::wstring Num(double v, const wchar_t* fmt = L"%.0f") {
    wchar_t b[64];
    swprintf(b, 64, fmt, v);
    return b;
}

void Add(const std::wstring& name, std::function<void()> start, std::function<bool()> done, DWORD timeout = 15000, std::function<void()> after = nullptr) {
    s_steps.push_back({name, std::move(start), std::move(done), timeout, std::move(after)});
}

int FindFile(const std::function<bool(const std::wstring&)>& pred) {
    for (size_t i = 0; i < files.size(); ++i) if (pred(Lower(files[i].name))) return (int)i;
    return -1;
}

bool CurrentSettled() {
    Entry* e = Current();
    return e && (e->ready() || e->failed());
}

void BuildSteps() {
    s_steps.reserve(2048);   // steps are added while running; never reallocate under a running step
    Add(L"first image", [] {}, [] { return CurrentSettled(); }, 20000, [] {
        Entry* e = Current();
        Line(L"  first image: " + PathName(path) + L" · " + Num(UiFirstPixelsMs()) + L" ms from process start to pixels ready (decode " +
             Num(e ? e->decodeMs : -1) + L" ms)");
        if (!HasImage()) Fail(L"the first image didn't open");
        Shot(L"first-image");
    });
    Add(L"folder listing", [] {}, [] { return !files.empty() && index >= 0; }, 10000, [] {
        Line(L"  " + std::to_wstring(files.size()) + L" images in the folder");
        PROCESS_MEMORY_COUNTERS_EX pm{sizeof(pm)};
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm)))
            Line(L"  memory: " + Num(pm.PrivateUsage / 1048576.0, L"%.1f") + L" MB private, " + Num(pm.WorkingSetSize / 1048576.0, L"%.1f") + L" MB working set");
    });
    // every image in the folder
    Add(L"open every image", [] {
        std::vector<std::wstring> names;
        for (auto& f : files) names.push_back(f.name);
        for (auto& n : names) {
            std::wstring p = folder + L"\\" + n;
            Add(L"open " + n, [p] { OpenPath(p); }, [] { return CurrentSettled(); }, 20000, [n] {
                Entry* e = Current();
                std::wstring ln = Lower(n);
                bool expectFail = ln.find(L"broken") != std::wstring::npos;
                if (!e) { Fail(n + L": no entry"); return; }
                std::wstring d = L"  " + n + L": " + (e->ready() ? e->format + L" " + std::to_wstring(e->fullW) + L"x" + std::to_wstring(e->fullH) +
                                                                     L", " + std::to_wstring(e->frames.size()) + L" frame(s), " + Num(e->decodeMs) + L" ms"
                                                                   : L"error: " + e->error);
                Line(d);
                if (expectFail && e->ready()) Fail(n + L" should have failed to open");
                if (!expectFail && !e->ready()) Fail(n + L" didn't open: " + e->error);
                if (ln.find(L"anim") != std::wstring::npos && e->ready() && e->frames.size() < 2) Fail(n + L" should be animated");
                if (ln.find(L"rot6") != std::wstring::npos && e->ready() && e->fullW >= e->fullH) Fail(n + L": EXIF orientation 6 wasn't applied");
                Shot(L"open-" + n);
            });
        }
        // move the remaining steps after the per-file ones
        std::rotate(s_steps.begin() + s_cur + 1, s_steps.end() - names.size(), s_steps.end());
    }, [] { return true; });

    Add(L"navigation and preloading", [] { GoTo(0); }, [] { return CurrentSettled() && Elapsed(1500); }, 10000, [] {
        int before = index;
        DoCmd(Cmd::Next);
        Entry* e = Current();
        bool preloaded = e && e->ready();
        Line(L"  next image was " + std::wstring(preloaded ? L"already decoded (no blank frame)" : L"not ready yet"));
        if (index != (before + 1) % (int)files.size()) Fail(L"Next didn't move to the next image");
        if (!preloaded) Fail(L"the next image wasn't preloaded");
        DoCmd(Cmd::Prev);
        if (index != before) Fail(L"Previous didn't go back");
    });
    Add(L"zoom to 100%", [] {
        int big = FindFile([](const std::wstring& n) { return n.find(L"large") != std::wstring::npos; });
        if (big >= 0) GoTo(big);
    }, [] { return CurrentSettled() && Elapsed(800); }, 10000, [] {
        DoCmd(Cmd::Zoom100);
    });
    Add(L"zoom settles", [] {}, [] { Entry* e = Current(); return Elapsed(1200) && e && (e->viewIsFull || e->fullReady); }, 15000, [] {
        float pct = zoomTarget * scale * 100;
        Line(L"  zoom is " + Num(pct, L"%.1f") + L"%");
        if (std::fabs(pct - 100) > 1) Fail(L"Zoom100 didn't reach 100%");
        Entry* e = Current();
        if (e && !e->viewIsFull && !e->fullReady) Fail(L"the full-resolution image didn't load for 100%");
        Shot(L"zoom-100");
        UiTestPanBy(-300, -200);
        Shot(L"zoom-100-panned");
        DoCmd(Cmd::ZoomIn);
        DoCmd(Cmd::ZoomFit);
    });
    Add(L"fit", [] {}, [] { return Elapsed(600); }, 3000, [] { if (!fitMode) Fail(L"Fit didn't return to fit"); });
    Add(L"filmstrip", [] { if (!g_cfg.filmstrip) DoCmd(Cmd::Filmstrip); }, [] { return Elapsed(1500); }, 3000, [] {
        Shot(L"filmstrip");
        int n = 0;
        for (auto& kv : thumbs) n += kv.second.bmp != nullptr;
        Line(L"  " + std::to_wstring(n) + L" thumbnails decoded");
        if (n == 0) Fail(L"no filmstrip thumbnails");
    });
    Add(L"info panel", [] {
        int ex = FindFile([](const std::wstring& n) { return n.find(L"exif") != std::wstring::npos; });
        if (ex >= 0) GoTo(ex);
        if (!g_cfg.infoPanel) DoCmd(Cmd::Info);
    }, [] { return CurrentSettled() && infos.count(path) && Elapsed(500); }, 10000, [] {
        const PhotoInfo& pi = infos[path];
        Line(L"  info: camera '" + pi.make + L" " + pi.model + L"', ISO " + std::to_wstring(pi.iso) + L", " + FormatExposure(pi.exposureTime) +
             L", f/" + Num(pi.fNumber, L"%.1f") + (pi.hasGps ? L", GPS " + Num(pi.lat, L"%.4f") + L"," + Num(pi.lon, L"%.4f") : L", no GPS") +
             (pi.hasTaken ? L", taken " + FormatTaken(pi.taken) : L""));
        if (Lower(PathName(path)).find(L"exif") != std::wstring::npos) {
            if (pi.model.empty()) Fail(L"camera model missing from EXIF");
            if (!pi.hasGps) Fail(L"GPS missing from EXIF");
            if (!pi.hasTaken) Fail(L"date taken missing from EXIF");
        }
        Shot(L"info-panel");
        DoCmd(Cmd::Info);
    });
    Add(L"grid", [] { DoCmd(Cmd::Grid); }, [] { return Elapsed(2000); }, 4000, [] {
        if (!gridMode) Fail(L"grid didn't open");
        Shot(L"grid");
        DoCmd(Cmd::Grid);
    });
    Add(L"compare", [] { DoCmd(Cmd::Compare); }, [] {
        Entry* a = FindEntry(comparePath[0]);
        Entry* b = FindEntry(comparePath[1]);
        return compareMode && a && b && (a->ready() || a->failed()) && (b->ready() || b->failed()) && Elapsed(800);
    }, 15000, [] {
        Shot(L"compare");
        DoCmd(Cmd::Escape);
        if (compareMode) Fail(L"Esc didn't leave compare");
    });
    Add(L"animation", [] {
        int a = FindFile([](const std::wstring& n) { return n.find(L"anim") != std::wstring::npos && n.find(L".gif") != std::wstring::npos; });
        if (a >= 0) GoTo(a);
    }, [] { return CurrentSettled() && Elapsed(1500); }, 10000, [] {
        Entry* e = Current();
        if (!e || !e->animated) { Line(L"  (no animated GIF in the test folder)"); return; }
        Line(L"  playing: frame " + std::to_wstring(frameIndex + 1) + L" of " + std::to_wstring(e->frames.size()));
        int f0 = frameIndex;
        DoCmd(Cmd::PlayPause);
        if (!paused) Fail(L"P didn't pause the animation");
        DoCmd(Cmd::FrameNext);
        if (frameIndex != (f0 + 1) % (int)e->frames.size()) Fail(L"frame step didn't advance one frame");
        DoCmd(Cmd::PlayPause);
        Shot(L"animated");
    });
    static UINT s_origW = 0, s_origH = 0;
    static std::wstring s_editedPath;
    Add(L"edit", [] {
        int j = FindFile([](const std::wstring& n) { return n.find(L"exif") != std::wstring::npos; });
        if (j < 0) j = FindFile([](const std::wstring& n) { return n.size() > 4 && n.substr(n.size() - 4) == L".jpg"; });
        if (j >= 0) GoTo(j);
    }, [] { return CurrentSettled() && Elapsed(500); }, 10000, [] {
        Entry* e = Current();
        if (!e || !e->ready()) { Fail(L"no image to edit"); return; }
        s_origW = e->fullW;
        s_origH = e->fullH;
        DoCmd(Cmd::RotateRight);
        if (!editing) { Fail(L"rotate didn't start editing"); return; }
        SetEditValue(0, 6.0f);
        SetEditValue(1, 0.5f);
        SetEditValue(3, 0.4f);
        SetEditValue(4, 0.6f);
        editDirty = true;
        Shot(L"edit");
        s_editedPath = s_shots + L"\\edited-copy.jpg";
        DeleteFileW(s_editedPath.c_str());
        UiTestSaveCopy(s_editedPath);
    });
    Add(L"save a copy", [] {}, [] { return FileExists(s_editedPath) && !editing; }, 30000, [] {
        ImageData d;
        if (!DecodeImage(s_editedPath, 0, 0, false, d) || d.frames.empty()) { Fail(L"the saved copy doesn't open"); return; }
        Line(L"  saved copy: " + std::to_wstring(d.fullW) + L"x" + std::to_wstring(d.fullH) + L" from " + std::to_wstring(s_origW) + L"x" + std::to_wstring(s_origH));
        // rotated a quarter turn, then cropped a little by straightening
        if (s_origW > s_origH && d.fullW > d.fullH) Fail(L"the saved copy isn't rotated");
        if (d.fullW > s_origH || d.fullH > s_origW) Fail(L"the saved copy is bigger than the rotated original");
    });
    Add(L"settings", [] { DoCmd(Cmd::Settings); }, [] { return Elapsed(700); }, 3000, [] {
        if (!settingsOpen) Fail(L"settings didn't open");
        Shot(L"settings");
        settingsScroll = settingsScrollTarget = 900;
        Shot(L"settings-more");
        settingsScroll = settingsScrollTarget = 5000;
        Shot(L"settings-end");
        DoCmd(Cmd::Escape);
    });
    Add(L"themes", [] {}, [] { return Elapsed(300); }, 3000, [] {
        std::wstring saved = g_cfg.theme;
        for (const wchar_t* id : {L"light", L"midnight", L"sunset", L"paper"}) {
            g_cfg.theme = id;
            ApplyTheme();
            Shot(std::wstring(L"theme-") + id);
        }
        g_cfg.theme = saved;
        ApplyTheme();
    });
    Add(L"slideshow", [] { DoCmd(Cmd::Slideshow); }, [] { return Elapsed(1500); }, 4000, [] {
        if (!slideshow) Fail(L"the slideshow didn't start");
        Shot(L"slideshow");
        DoCmd(Cmd::Escape);
        if (slideshow) Fail(L"Esc didn't stop the slideshow");
    });
    Add(L"copy image", [] {
        GoTo(0);
        if (OpenClipboard(hwnd)) { EmptyClipboard(); CloseClipboard(); }
    }, [] { return CurrentSettled() && Elapsed(500); }, 10000, [] { DoCmd(Cmd::CopyImage); });
    Add(L"clipboard", [] {}, [] { return IsClipboardFormatAvailable(CF_DIBV5) != 0; }, 15000, [] {
        Line(L"  the image is on the clipboard");
    });
    static std::wstring s_renamed, s_original;
    Add(L"rename", [] {}, [] { return Elapsed(200); }, 2000, [] {
        s_original = path;
        DoCmd(Cmd::Rename);
        if (!dialog.open) { Fail(L"F2 didn't open the rename box"); return; }
        Shot(L"rename");
        dialog.input.text = L"renamed by smoke test" + ExtOf(path);
        s_renamed = folder + L"\\" + dialog.input.text;
        UiTestDialogPress(0);
        if (!FileExists(s_renamed)) Fail(L"rename didn't rename the file");
        DoCmd(Cmd::Undo);
        if (!FileExists(s_original)) Fail(L"undo didn't restore the name");
    });
    static std::wstring s_deleted;
    Add(L"delete", [] {}, [] { return Elapsed(300); }, 2000, [] {
        s_deleted = path;
        DoCmd(Cmd::Delete);
        if (FileExists(s_deleted)) Fail(L"delete didn't remove the file");
        else Line(L"  moved " + PathName(s_deleted) + L" to the Recycle Bin");
        DoCmd(Cmd::Undo);
    });
    Add(L"undo delete", [] {}, [] { return FileExists(s_deleted); }, 20000, [] { Line(L"  restored from the Recycle Bin"); });
}

} // namespace

void SmokeStart(const std::wstring& log, const std::wstring& dir) {
    s_log = log;
    s_dir = dir;
    s_shots = PathParent(log) + L"\\shots";
    EnsureDir(s_shots);
    WriteTextFileAtomic(log, std::wstring(L"Loupe ") + LOUPE_VERSION + L" smoke test\r\n");
    s_active = true;
    BuildSteps();
}

bool SmokeActive() { return s_active; }
void SmokeOnDecoded(uint64_t, bool, double, const std::wstring&) {}
int SmokeExitCode() { return s_exit; }

void SmokeTick() {
    if (!s_active) return;
    if (s_cur >= s_steps.size()) {
        s_active = false;
        if (s_failures.empty()) { Line(L"SMOKE OK"); s_exit = 0; }
        else { Line(L"SMOKE FAILED (" + std::to_wstring(s_failures.size()) + L")"); s_exit = 1; }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return;
    }
    SmokeStep& st = s_steps[s_cur];
    if (!s_started) {
        Line(L"[" + st.name + L"]");
        s_started = true;
        s_stepStart = GetTickCount();
        if (st.start) st.start();
        return;
    }
    SmokeStep& cur = s_steps[s_cur];   // start() may have added steps
    bool done = cur.done ? cur.done() : true;
    if (!done && GetTickCount() - s_stepStart < cur.timeoutMs) return;
    if (!done) Fail(cur.name + L" timed out");
    else if (cur.after) cur.after();
    ++s_cur;
    s_started = false;
}
