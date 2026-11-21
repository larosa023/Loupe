// Loupe - the window: device and theme, the image cache, navigation, commands and input.
#include "app.h"
#include "util.h"
#include "image.h"
#include "shellops.h"
#include "json.h"
#include "ui.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <thread>
#include <mutex>
#include <ctime>
#include <shlwapi.h>

namespace app {
HWND hwnd = nullptr;
float scale = 1, W = 0, H = 0;
Theme t;
std::vector<ThemeDef> customThemes;
ID2D1HwndRenderTarget* rt = nullptr;
ID2D1DeviceContext* dc = nullptr;
ID2D1SolidColorBrush* br = nullptr;
Canvas c;
UINT maxBitmap = 4096;
std::wstring folder;
std::vector<FileEntry> files;
int index = -1;
std::wstring path;
bool datesLoaded = false;
std::vector<std::unique_ptr<Entry>> cache;
float zoom = 1, zoomTarget = 1, cx = 0, cy = 0, cxTarget = 0, cyTarget = 0;
bool fitMode = true;
int frameIndex = 0;
bool paused = false;
D2D1_RECT_F viewRect{};
bool gridMode = false, compareMode = false, slideshow = false, fullscreen = false, editing = false, settingsOpen = false,
     shortcutsOpen = false, welcome = false;
int comparePane = 1;
std::wstring comparePath[2];
EditParams edit;
bool editDirty = false;
float chromeAlpha = 1;
std::vector<HotRect> hot;
HotRect hover{{0, 0, 0, 0}, Hit::None, 0, true, nullptr}, pressed{{0, 0, 0, 0}, Hit::None, 0, true, nullptr};
float gridScroll = 0, gridScrollTarget = 0, settingsScroll = 0, settingsScrollTarget = 0, filmScroll = 0, filmScrollTarget = 0;
float settingsContentH = 0, gridContentH = 0;
Dialog dialog;
TextField accentField;
UpdateState update;
bool updateDismissed = false;
std::map<std::wstring, Thumb> thumbs;
std::map<std::wstring, PhotoInfo> infos;
std::map<std::wstring, bool> infoPending;
std::wstring toastText, toastAction;
float toastAlpha = 0;
DWORD toastUntil = 0;
std::vector<int> gridMarks;
std::vector<updater::Previous> prevVersions;
} // namespace app

using namespace app;
using namespace gfx;

// ---------------------------------------------------------------- local state
enum Timer : UINT_PTR { TIMER_ANIM = 1, TIMER_IDLE, TIMER_UPDATE, TIMER_FIRSTCHECK, TIMER_SLIDE, TIMER_FULL, TIMER_SMOKE, TIMER_HEALTHY };

static bool s_rtOpaque = false;
static uint64_t s_nextId = 100;
static uint64_t s_folderGen = 0;
static UINT s_previewW = 1920, s_previewH = 1080;
static DWORD s_lastMove = 0;
static POINT s_lastMousePt{-1, -1};
static bool s_tracking = false;
static bool s_animating = false;
static LARGE_INTEGER s_qpf{}, s_lastAnim{};
static DWORD s_frameStart = 0;
static int s_loopsDone = 0;
static bool s_panning = false;
static float s_panX = 0, s_panY = 0;
static bool s_dragSlider = false, s_dragCrop = false, s_dragScroll = false, s_dragFilm = false;
static int s_dragArg = 0;
static float s_filmDragX = 0, s_filmDragStart = 0;
static bool s_filmDragged = false;
static updater::AfterUpdate s_after;
static bool s_healthySent = false;
static bool s_firstPixelsLogged = false;
static double s_firstPixelsMs = -1;
static LARGE_INTEGER s_startQpc{};
static WINDOWPLACEMENT s_wpPrev{sizeof(WINDOWPLACEMENT)};
static LONG s_stylePrev = 0;
static bool s_fullscreenBeforeSlide = false;
static uint64_t s_fadeFromId = 0;
static float s_fade = 0;
static float s_cmpMul = 1, s_cmpU = 0.5f, s_cmpV = 0.5f;
static bool s_maxHot = false, s_maxDown = false;
static std::vector<EditParams> s_editHistory;
static std::wstring s_pendingSelect;          // after a folder refresh, keep showing this file
static bool s_quitting = false;
static std::mt19937 s_rng{std::random_device{}()};

struct UndoItem { enum Kind { Delete, Save, Rename } kind; std::wstring a, b; int index = -1; };
static std::vector<UndoItem> s_undo;

struct TaskDone {
    enum Kind { CopyImage, Wallpaper, SaveReplace, SaveCopy, Restore, UndoSave } kind;
    bool ok = false;
    std::wstring path, path2, message;
    Pixels pix;
    int index = -1;
};

// smoke test hooks (smoke.cpp)
void SmokeStart(const std::wstring& log, const std::wstring& folder);
void SmokeTick();
bool SmokeActive();
void SmokeOnDecoded(uint64_t id, bool full, double ms, const std::wstring& path);

// ---------------------------------------------------------------- GPU images
void GpuImage::Release() {
    for (auto& tl : tiles) if (tl.bmp) tl.bmp->Release();
    tiles.clear();
    w = h = 0;
}

static bool Upload(const Pixels& p, GpuImage& out) {
    out.Release();
    if (p.empty() || !rt) return false;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    UINT tile = std::min<UINT>(maxBitmap, 8192);
    out.w = p.w;
    out.h = p.h;
    for (UINT y = 0; y < p.h; y += tile) {
        for (UINT x = 0; x < p.w; x += tile) {
            GpuImage::Tile tl;
            tl.x = x; tl.y = y;
            tl.w = std::min(tile, p.w - x);
            tl.h = std::min(tile, p.h - y);
            if (FAILED(rt->CreateBitmap(D2D1::SizeU(tl.w, tl.h), &p.px[((size_t)y * p.w + x) * 4], p.w * 4, bp, &tl.bmp)) || !tl.bmp) {
                out.Release();
                return false;
            }
            out.tiles.push_back(tl);
        }
    }
    return true;
}

// ---------------------------------------------------------------- startup prefetch
// The first image starts decoding before the window exists, so it's ready about when the window is.
static struct {
    std::mutex m;
    std::wstring path;
    DecodeResult* result = nullptr;
    HWND hwnd = nullptr;
    uint64_t id = 0;
    bool running = false;
} s_pre;

static void ComputePreviewSize(HMONITOR mon) {
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
        s_previewW = std::max<LONG>(640, mi.rcMonitor.right - mi.rcMonitor.left);
        s_previewH = std::max<LONG>(480, mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
}

void UiPrefetch(const std::wstring& p) {
    RECT r{g_cfg.windowX, g_cfg.windowY, g_cfg.windowX + std::max(g_cfg.windowW, 100), g_cfg.windowY + std::max(g_cfg.windowH, 100)};
    HMONITOR mon = g_cfg.windowX == CW_USEDEFAULT ? MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY) : MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY);
    ComputePreviewSize(mon);
    s_pre.path = p;
    s_pre.running = true;
    UINT w = s_previewW, h = s_previewH;
    std::thread([p, w, h] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* r = new DecodeResult;
        r->path = p;
        LARGE_INTEGER a, b, f;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        DecodeImage(p, w, h, true, r->data);
        QueryPerformanceCounter(&b);
        r->ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        std::lock_guard<std::mutex> g(s_pre.m);
        s_pre.running = false;
        if (s_pre.hwnd && s_pre.id) {
            r->id = s_pre.id;
            if (!PostMessageW(s_pre.hwnd, WM_LOUPE_DECODED, 0, (LPARAM)r)) delete r;
        } else {
            s_pre.result = r;
        }
    }).detach();
}

// true if the prefetch covers this path (it will post its result for `id`)
static bool AdoptPrefetch(const std::wstring& p, uint64_t id) {
    std::lock_guard<std::mutex> g(s_pre.m);
    if (s_pre.path.empty() || !IEquals(s_pre.path, p)) return false;
    s_pre.path.clear();
    if (s_pre.result) {
        s_pre.result->id = id;
        if (!PostMessageW(hwnd, WM_LOUPE_DECODED, 0, (LPARAM)s_pre.result)) delete s_pre.result;
        s_pre.result = nullptr;
        return true;
    }
    if (!s_pre.running) return false;
    s_pre.hwnd = hwnd;
    s_pre.id = id;
    return true;
}

// ---------------------------------------------------------------- helpers
namespace app {

void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

void StartAnim() {
    if (s_animating) return;
    s_animating = true;
    QueryPerformanceCounter(&s_lastAnim);
    SetTimer(hwnd, TIMER_ANIM, 15, nullptr);
}

Entry* Current() { return path.empty() ? nullptr : FindEntry(path); }

Entry* FindEntry(const std::wstring& p) {
    for (auto& e : cache) if (IEquals(e->path, p)) return e.get();
    return nullptr;
}

static int64_t FileStamp(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((int64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

Entry* EnsureEntry(const std::wstring& p, int priority) {
    Entry* e = FindEntry(p);
    if (e && !e->viewPending && e->viewReady && e->stamp && e->stamp != FileStamp(p)) {
        // changed on disk since we decoded it
        e->Release();
        e->viewReady = false;
        e->fullPending = false;
    }
    if (!e) {
        auto ne = std::make_unique<Entry>();
        ne->id = ++s_nextId;
        ne->path = p;
        e = ne.get();
        cache.push_back(std::move(ne));
    }
    e->lastUse = GetTickCount();
    if (!e->viewReady && !e->viewPending) {
        e->viewPending = true;
        e->stamp = FileStamp(p);
        if (!AdoptPrefetch(p, e->id)) loader::Decode(e->id, p, false, s_previewW, s_previewH, priority);
    }
    return e;
}

bool HasImage() { Entry* e = Current(); return e && e->ready(); }

std::wstring CurrentName() { return PathName(path); }

int ThumbPixels() {
    int px = (int)std::ceil(std::max<float>((float)g_cfg.gridSize, 96.0f) * scale);
    return std::clamp((px + 63) / 64 * 64, 128, 640);
}

void RequestThumb(const std::wstring& p) {
    Thumb& th = thumbs[p];
    th.lastUse = GetTickCount();
    UINT want = (UINT)ThumbPixels();
    if (th.failed) return;
    if (!th.pending && th.bmp && th.size >= want * 3 / 4) return;
    th.pending = true;
    loader::Thumb(p, want);   // re-queued every frame while visible; the queue drops what scrolled away
}

ID2D1Bitmap* ThumbBitmap(const std::wstring& p, UINT* w, UINT* h) {
    auto it = thumbs.find(p);
    if (it == thumbs.end() || !it->second.bmp) return nullptr;
    it->second.lastUse = GetTickCount();
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    return it->second.bmp;
}

} // namespace app

static void ReleaseThumbs() {
    for (auto& kv : thumbs) if (kv.second.bmp) kv.second.bmp->Release();
    thumbs.clear();
}

static void TrimThumbs() {
    if (thumbs.size() < 900) return;
    std::vector<std::pair<DWORD, std::wstring>> order;
    for (auto& kv : thumbs) if (!kv.second.pending) order.push_back({kv.second.lastUse, kv.first});
    std::sort(order.begin(), order.end());
    for (size_t i = 0; i < order.size() && thumbs.size() > 600; ++i) {
        auto it = thumbs.find(order[i].second);
        if (it->second.bmp) it->second.bmp->Release();
        thumbs.erase(it);
    }
}

// ---------------------------------------------------------------- device and theme
static void DiscardDevice();
void UiDeviceLost() { DiscardDevice(); }
static void DiscardDevice() {
    for (auto& e : cache) {
        if (e->viewReady) { e->Release(); e->viewReady = false; e->viewPending = false; e->fullPending = false; }
    }
    ReleaseThumbs();
    if (br) { br->Release(); br = nullptr; }
    if (dc) { dc->Release(); dc = nullptr; }
    if (rt) { rt->Release(); rt = nullptr; }
}

static int s_deviceGen = 0;
int DeviceGeneration() { return s_deviceGen; }
void UiDeviceLost();

static bool EnsureDevice() {
    if (rt) return true;
    ++s_deviceGen;
    RECT rc;
    GetClientRect(hwnd, &rc);
    s_rtOpaque = !t.backdrop;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, s_rtOpaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED));
    UINT dpi = GetDpiForWindow(hwnd);
    props.dpiX = props.dpiY = (float)dpi;
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)),
                                           D2D1_PRESENT_OPTIONS_NONE), &rt))) return false;
    rt->SetTextAntialiasMode(s_rtOpaque ? D2D1_TEXT_ANTIALIAS_MODE_DEFAULT : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    rt->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&dc);
    maxBitmap = rt->GetMaximumBitmapSize();
    c.rt = rt;
    c.br = br;
    // anything that lost its bitmaps decodes again
    for (auto& e : cache) {
        if (!e->viewReady && !e->viewPending) {
            e->viewPending = true;
            loader::Decode(e->id, e->path, false, s_previewW, s_previewH, IEquals(e->path, path) ? 0 : 5);
        }
    }
    return true;
}

// Native menus follow the theme (uxtheme's undocumented dark-mode switches, as Explorer uses)
static void ThemeMenus(bool dark) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    auto set = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set) set(dark ? 2 : 3);
    if (flush) flush();
}

static HANDLE s_hangarStop = nullptr;
static void WatchHangar(bool on) {
    if (s_hangarStop) { SetEvent(s_hangarStop); s_hangarStop = nullptr; }
    if (!on) return;
    std::wstring dir = HangarDir();
    if (!DirExists(dir)) return;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_hangarStop = stop;
    HWND target = hwnd;
    std::thread([dir, stop, target] {
        HANDLE ch = FindFirstChangeNotificationW(dir.c_str(), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
        if (ch == INVALID_HANDLE_VALUE) { CloseHandle(stop); return; }
        HANDLE w[2] = {stop, ch};
        while (WaitForMultipleObjects(2, w, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            if (WaitForSingleObject(stop, 300) == WAIT_OBJECT_0) break;
            PostMessageW(target, WM_LOUPE_HANGAR_CHANGED, 0, 0);
            if (!FindNextChangeNotification(ch)) break;
        }
        FindCloseChangeNotification(ch);
        CloseHandle(stop);
    }).detach();
}

// With the frame extended for Mica/acrylic, DWM draws its own min/max/close wherever our pixels are
// see-through. Dropping WS_SYSMENU stops that (keeping the min/max boxes keeps Snap Layouts working);
// the system menu is then shown by hand on Alt+Space and a right-click on the title bar.
static void UpdateFrameStyle() {
    if (!hwnd || fullscreen) return;
    LONG st = GetWindowLongW(hwnd, GWL_STYLE);
    LONG want = t.backdrop ? (st & ~WS_SYSMENU) : (st | WS_SYSMENU);
    if (want != st) {
        SetWindowLongW(hwnd, GWL_STYLE, want);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

static void ShowSystemMenu(int sx, int sy) {
    HMENU m = GetSystemMenu(hwnd, FALSE);
    if (!m) return;
    bool max = IsZoomed(hwnd);
    EnableMenuItem(m, SC_RESTORE, max ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, SC_MOVE, max ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(m, SC_SIZE, max ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(m, SC_MAXIMIZE, max ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(m, SC_MINIMIZE, MF_ENABLED);
    EnableMenuItem(m, SC_CLOSE, MF_ENABLED);
    SetMenuDefaultItem(m, SC_CLOSE, FALSE);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, hwnd, nullptr);
    if (cmd) PostMessageW(hwnd, WM_SYSCOMMAND, cmd, 0);
}

namespace app {

void ApplyTheme() {
    std::wstring themeId = g_cfg.theme, accent = g_cfg.accent;
    if (themeId == L"hangar") {
        std::wstring hid, hacc;
        if (ReadHangarTheme(hid, hacc)) { themeId = hid; accent = hacc; }
        else themeId = L"dark";
    }
    ThemeDef def = FindTheme(themeId, customThemes);
    t = MakeTheme(def, accent);
    if (hwnd) {
        ApplyWindowTheme(hwnd, t);
        UpdateFrameStyle();
        if (rt && s_rtOpaque != !t.backdrop) DiscardDevice();
        ThemeMenus(t.dark);
        WatchHangar(g_cfg.theme == L"hangar");
    }
    Invalidate();
}

void SaveSettings() { ConfigSave(); }

void Toast(const std::wstring& text, const std::wstring& action) {
    toastText = text;
    toastAction = action;
    toastUntil = GetTickCount() + (action.empty() ? 2600 : 6000);
    StartAnim();
    if (hwnd) SetTimer(hwnd, 2 /*TIMER_IDLE*/, 250, nullptr);
    Invalidate();
}

void ShowDialog(const std::wstring& title, const std::wstring& body, std::vector<std::wstring> buttons, int primary, int cancel,
                std::function<void(int)> onClose, bool danger) {
    dialog = Dialog{};
    dialog.open = true;
    dialog.title = title;
    dialog.body = body;
    dialog.buttons = std::move(buttons);
    dialog.primary = primary;
    dialog.cancel = cancel;
    dialog.danger = danger;
    dialog.onClose = std::move(onClose);
    Invalidate();
}

static void CloseDialog(int button) {
    if (!dialog.open) return;
    auto cb = dialog.onClose;
    bool field = dialog.field;
    if (field && button != dialog.cancel) {
        // the callback may keep the dialog open (validation)
        if (cb) cb(button);
        Invalidate();
        return;
    }
    dialog.open = false;
    if (cb) cb(button);
    Invalidate();
}

bool chromePinned() {
    Entry* e = Current();
    return gridMode || settingsOpen || editing || dialog.open || !g_cfg.autoHideChrome || welcome || path.empty() ||
           (e && e->failed() && !slideshow) || (g_cfg.infoPanel && !slideshow);
}

// ---------------------------------------------------------------- layout and zoom
D2D1_RECT_F LayoutViewport() {
    float right = W, bottom = H;
    if (!slideshow) {
        if (editing || g_cfg.infoPanel) right -= kPanel;
        if (g_cfg.filmstrip && !editing && files.size() > 1) bottom -= kFilm;
    }
    viewRect = D2D1::RectF(0, 0, std::max(1.0f, right), std::max(1.0f, bottom));
    return viewRect;
}

float FitZoom(const Entry* e, const D2D1_RECT_F& r) {
    if (!e || !e->fullW || !e->fullH) return 1.0f / scale;
    float pad = (fullscreen || slideshow) ? 0 : 0;
    float z = std::min((r.right - r.left - pad) / e->fullW, (r.bottom - r.top - pad) / e->fullH);
    if (!g_cfg.enlargeSmall) z = std::min(z, 1.0f / scale);
    return std::max(z, 1e-4f);
}

static void ClampPan(float& ccx, float& ccy, float z, const Entry* e, const D2D1_RECT_F& r) {
    if (!e || !e->fullW) return;
    float vw = (r.right - r.left) / z, vh = (r.bottom - r.top) / z;
    if (vw >= e->fullW) ccx = e->fullW / 2.0f; else ccx = std::clamp(ccx, vw / 2, e->fullW - vw / 2);
    if (vh >= e->fullH) ccy = e->fullH / 2.0f; else ccy = std::clamp(ccy, vh / 2, e->fullH - vh / 2);
}

void ResetView() {
    Entry* e = Current();
    D2D1_RECT_F r = LayoutViewport();
    fitMode = true;
    zoom = zoomTarget = FitZoom(e, r);
    if (e) { cx = cxTarget = e->fullW / 2.0f; cy = cyTarget = e->fullH / 2.0f; }
    s_cmpMul = 1; s_cmpU = s_cmpV = 0.5f;
}

void ZoomTo(float z, float px, float py, bool animate) {
    Entry* e = Current();
    if (!e || !e->ready()) return;
    D2D1_RECT_F r = viewRect;
    float fit = FitZoom(e, r);
    float minZ = std::min(fit, 0.02f / scale), maxZ = 32.0f / scale;
    z = std::clamp(z, minZ, maxZ);
    float vcx = (r.left + r.right) / 2, vcy = (r.top + r.bottom) / 2;
    float ix = cxTarget + (px - vcx) / zoomTarget, iy = cyTarget + (py - vcy) / zoomTarget;
    zoomTarget = z;
    cxTarget = ix - (px - vcx) / z;
    cyTarget = iy - (py - vcy) / z;
    fitMode = std::fabs(z - fit) < fit * 0.001f;
    ClampPan(cxTarget, cyTarget, z, e, r);
    if (animate && g_cfg.smoothZoom) StartAnim();
    else { zoom = zoomTarget; cx = cxTarget; cy = cyTarget; }
    Invalidate();
}

} // namespace app

static float ViewCenterX() { return (viewRect.left + viewRect.right) / 2; }
static float ViewCenterY() { return (viewRect.top + viewRect.bottom) / 2; }

static void ZoomBy(float factor, float px, float py) {
    if (compareMode) {
        s_cmpMul = std::clamp(s_cmpMul * factor, 1.0f, 64.0f);
        Invalidate();
        return;
    }
    ZoomTo(zoomTarget * factor, px, py, true);
}

static void ZoomStep(bool in) {
    // the next "round" zoom level, like 25/33/50/67/100/150/200…
    static const float steps[] = {0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 1, 1.25f, 1.5f, 2, 3, 4, 5, 6, 8, 12, 16, 24, 32};
    if (compareMode) { ZoomBy(in ? 1.25f : 0.8f, ViewCenterX(), ViewCenterY()); return; }
    float pct = zoomTarget * scale;
    float next = pct;
    if (in) { for (float s : steps) if (s > pct * 1.01f) { next = s; break; } }
    else { for (int i = (int)(sizeof(steps) / sizeof(steps[0])) - 1; i >= 0; --i) if (steps[i] < pct * 0.99f) { next = steps[i]; break; } }
    Entry* e = Current();
    float fit = FitZoom(e, viewRect) * scale;
    // stop at "fit" on the way through
    if ((in && pct < fit * 0.99f && next > fit * 1.01f) || (!in && pct > fit * 1.01f && next < fit * 0.99f)) next = fit;
    ZoomTo(next / scale, ViewCenterX(), ViewCenterY(), true);
}

static void ZoomFit() {
    Entry* e = Current();
    if (!e) return;
    fitMode = true;
    zoomTarget = FitZoom(e, viewRect);
    cxTarget = e->fullW / 2.0f;
    cyTarget = e->fullH / 2.0f;
    s_cmpMul = 1; s_cmpU = s_cmpV = 0.5f;
    if (g_cfg.smoothZoom) StartAnim(); else { zoom = zoomTarget; cx = cxTarget; cy = cyTarget; }
    Invalidate();
}

// ---------------------------------------------------------------- cache policy
static std::vector<int> NeighborIndices(int radius) {
    std::vector<int> out;
    int n = (int)files.size();
    if (index < 0 || n == 0) return out;
    for (int d = 1; d <= radius; ++d) {
        for (int s : {1, -1}) {
            int i = index + s * d;
            if (g_cfg.loopFolder) i = ((i % n) + n) % n;
            if (i >= 0 && i < n && i != index && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
        }
    }
    return out;
}

static std::wstring PathAt(int i) { return folder + L"\\" + files[i].name; }

static void Preload() {
    if (slideshow && g_cfg.slideshowShuffle) return;   // the next one is picked at random; it preloads then
    int pr = 3;
    for (int i : NeighborIndices(2)) EnsureEntry(PathAt(i), pr++);
}

static void TrimCache() {
    std::vector<std::wstring> keep = {path};
    for (int i : NeighborIndices(2)) keep.push_back(PathAt(i));
    if (compareMode) { keep.push_back(comparePath[0]); keep.push_back(comparePath[1]); }
    for (auto& e : cache) if (e->id == s_fadeFromId) keep.push_back(e->path);
    auto keepIt = [&](const Entry& e) {
        for (auto& k : keep) if (IEquals(k, e.path)) return true;
        return false;
    };
    // full-resolution bitmaps only for what's on screen
    for (auto& e : cache) {
        bool onScreen = IEquals(e->path, path) || (compareMode && (IEquals(e->path, comparePath[0]) || IEquals(e->path, comparePath[1])));
        if (!onScreen && e->fullReady) { e->full.Release(); e->fullReady = false; }
    }
    std::vector<uint64_t> ids;
    for (auto& e : cache) if (keepIt(*e)) ids.push_back(e->id);
    loader::CancelDecodes(ids);
    if (cache.size() <= 7) return;
    std::sort(cache.begin(), cache.end(), [](const std::unique_ptr<Entry>& a, const std::unique_ptr<Entry>& b) { return a->lastUse > b->lastUse; });
    for (size_t i = cache.size(); i-- > 0 && cache.size() > 7;) {
        if (keepIt(*cache[i])) continue;
        cache[i]->Release();
        cache.erase(cache.begin() + i);
    }
}

static void MaybeRequestFull(Entry* e, int priority) {
    if (!e || !e->ready() || e->animated || e->viewIsFull || e->fullReady || e->fullPending) return;
    e->fullPending = true;
    loader::Decode(e->id, e->path, true, 0, 0, priority);
}

// zoomed in past what the screen-sized decode can show sharply
static void CheckFullNeeded() {
    Entry* e = Current();
    if (!e || !e->ready() || e->frames.empty()) return;
    float shownPx = zoomTarget * scale * e->fullW;
    if (compareMode) return;
    if (shownPx > e->frames[0].w * 1.05f) MaybeRequestFull(e, 1);
}

// ---------------------------------------------------------------- navigation
static void UpdateTitle() {
    std::wstring title = path.empty() ? L"Loupe" : PathName(path) + L" - Loupe";
    SetWindowTextW(hwnd, title.c_str());
}

static void SortFiles() {
    std::wstring cur = PathName(path);
    auto key = g_cfg.sort;
    bool desc = g_cfg.sortDescending;
    std::stable_sort(files.begin(), files.end(), [key, desc](const FileEntry& a, const FileEntry& b) {
        int c = 0;
        switch (key) {
            case SortKey::Name: c = StrCmpLogicalW(a.name.c_str(), b.name.c_str()); break;
            case SortKey::DateModified: c = a.modified < b.modified ? -1 : a.modified > b.modified ? 1 : 0; break;
            case SortKey::Size: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
            case SortKey::DateTaken: {
                int64_t x = a.taken ? a.taken : a.modified, y = b.taken ? b.taken : b.modified;
                c = x < y ? -1 : x > y ? 1 : 0;
                break;
            }
        }
        if (c == 0) c = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
        return desc ? c > 0 : c < 0;
    });
    index = -1;
    for (size_t i = 0; i < files.size(); ++i) if (IEquals(files[i].name, cur)) index = (int)i;
}

static void RequestDates() {
    if (datesLoaded || files.empty()) return;
    std::vector<std::wstring> names;
    for (auto& f : files) names.push_back(f.name);
    loader::ReadDates(s_folderGen, folder, names);
}

static void LeaveEditing() {
    editing = false;
    editDirty = false;
    s_editHistory.clear();
    edit = EditParams{};
    ResetView();
}

static void ConfirmLeaveEditing(std::function<void()> then) {
    if (!editing) { then(); return; }
    if (!editDirty) { LeaveEditing(); then(); return; }
    ShowDialog(L"Discard your edits?", L"You have changes that aren't saved.", {L"Discard", L"Keep editing"}, 1, 1, [then](int b) {
        if (b == 0) { LeaveEditing(); then(); }
    }, true);
}

namespace app {

void OpenPath(const std::wstring& p0, bool keepFolder) {
    std::wstring p = p0;
    if (p.empty()) return;
    welcome = false;
    std::wstring dir = PathParent(p);
    bool sameFolder = IEquals(dir, folder);
    path = p;
    if (!sameFolder) {
        folder = dir;
        files.clear();
        index = -1;
        datesLoaded = false;
        gridMarks.clear();
        ++s_folderGen;
        loader::ListFolder(s_folderGen, folder);
        loader::WatchFolder(folder);
        filmScroll = filmScrollTarget = -1;   // center on the current image once listed
    } else {
        index = -1;
        for (size_t i = 0; i < files.size(); ++i) if (IEquals(files[i].name, PathName(p))) index = (int)i;
    }
    (void)keepFolder;
    Entry* e = EnsureEntry(p, 0);
    frameIndex = 0;
    s_loopsDone = 0;
    s_frameStart = GetTickCount();
    paused = false;
    if (e->ready()) ResetView();
    UpdateTitle();
    if (g_cfg.infoPanel && !infos.count(p) && !infoPending[p]) { infoPending[p] = true; loader::ReadInfo(p); }
    if (e->ready()) Preload();
    TrimCache();
    KillTimer(hwnd, TIMER_FULL);
    SetTimer(hwnd, TIMER_FULL, 700, nullptr);
    if (slideshow) SetTimer(hwnd, TIMER_SLIDE, g_cfg.slideshowSeconds * 1000, nullptr);
    Invalidate();
}

void GoTo(int idx) {
    int n = (int)files.size();
    if (n == 0) return;
    if (g_cfg.loopFolder || slideshow) idx = ((idx % n) + n) % n;
    else idx = std::clamp(idx, 0, n - 1);
    if (idx == index && !path.empty()) return;
    Entry* prev = Current();
    if (slideshow && prev && prev->ready()) { s_fadeFromId = prev->id; s_fade = 1; StartAnim(); }
    index = idx;
    OpenPath(PathAt(idx), true);
    index = idx;
}

} // namespace app

static void Step(int delta) {
    if (compareMode) {
        // flip the active pane through the folder
        int n = (int)files.size();
        if (!n) return;
        int i = -1;
        for (int k = 0; k < n; ++k) if (IEquals(PathAt(k), comparePath[comparePane])) i = k;
        i = ((i + delta) % n + n) % n;
        comparePath[comparePane] = PathAt(i);
        EnsureEntry(comparePath[comparePane], 0);
        if (comparePane == 1 || comparePath[0].empty()) {}
        TrimCache();
        Invalidate();
        return;
    }
    if (files.empty()) return;
    if (index < 0) return;
    if (slideshow && g_cfg.slideshowShuffle && files.size() > 2) {
        int n = (int)files.size(), next = index;
        while (next == index) next = (int)(s_rng() % n);
        GoTo(next);
        return;
    }
    int n = (int)files.size();
    int to = index + delta;
    if (!g_cfg.loopFolder && !slideshow && (to < 0 || to >= n)) {
        Toast(to < 0 ? L"This is the first image" : L"This is the last image");
        return;
    }
    GoTo(to);
}

// ---------------------------------------------------------------- fullscreen and slideshow
static void SetFullscreen(bool on) {
    if (on == fullscreen) return;
    fullscreen = on;
    if (on) {
        s_stylePrev = GetWindowLongW(hwnd, GWL_STYLE);
        GetWindowPlacement(hwnd, &s_wpPrev);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, (s_stylePrev & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, s_stylePrev);
        SetWindowPlacement(hwnd, &s_wpPrev);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    ApplyWindowTheme(hwnd, t);
    UpdateFrameStyle();
    chromeAlpha = on ? 0.0f : 1.0f;
    Invalidate();
}

static void SetSlideshow(bool on) {
    if (on == slideshow) return;
    if (on && files.size() < 2 && !HasImage()) { Toast(L"Open a folder of images first"); return; }
    slideshow = on;
    if (on) {
        gridMode = false;
        compareMode = false;
        s_fullscreenBeforeSlide = fullscreen;
        SetFullscreen(true);
        SetTimer(hwnd, TIMER_SLIDE, g_cfg.slideshowSeconds * 1000, nullptr);
        Toast(L"Slideshow · Space pauses · ↑↓ change speed · Esc stops");
    } else {
        KillTimer(hwnd, TIMER_SLIDE);
        if (!s_fullscreenBeforeSlide) SetFullscreen(false);
        s_fadeFromId = 0;
    }
    ResetView();
    Invalidate();
}

static bool s_slidePaused = false;

// ---------------------------------------------------------------- delete, rename, undo
static void RemoveFromList(const std::wstring& p) {
    std::wstring name = PathName(p);
    for (size_t i = 0; i < files.size(); ++i) {
        if (IEquals(files[i].name, name)) { files.erase(files.begin() + i); break; }
    }
    for (size_t i = 0; i < cache.size(); ++i) if (IEquals(cache[i]->path, p)) { cache[i]->Release(); cache.erase(cache.begin() + i); break; }
    auto th = thumbs.find(p);
    if (th != thumbs.end()) { if (th->second.bmp) th->second.bmp->Release(); thumbs.erase(th); }
    gridMarks.clear();
}

static void ShowAfterRemoval(int oldIndex) {
    if (files.empty()) {
        path.clear();
        index = -1;
        welcome = true;
        UpdateTitle();
        Invalidate();
        return;
    }
    int i = std::min(oldIndex, (int)files.size() - 1);
    index = -1;
    path.clear();
    GoTo(std::max(0, i));
}

static void DeleteCurrent(const std::wstring& target, int targetIndex) {
    std::wstring err;
    if (!shell::Recycle(target, &err)) { Toast(err); return; }
    RemoveFromList(target);
    s_undo.push_back({UndoItem::Delete, target, L"", targetIndex});
    if (s_undo.size() > 20) s_undo.erase(s_undo.begin());
    if (IEquals(target, path)) ShowAfterRemoval(targetIndex);
    Toast(L"Moved " + PathName(target) + L" to the Recycle Bin", L"Undo");
}

static void DoDelete() {
    std::wstring target = path;
    int idx = index;
    if (gridMode && index >= 0 && index < (int)files.size()) target = PathAt(index);
    if (target.empty()) return;
    if (editing) { Toast(L"Finish editing first"); return; }
    if (g_cfg.confirmDelete) {
        ShowDialog(L"Delete this image?", PathName(target) + L" goes to the Recycle Bin.", {L"Delete", L"Cancel"}, 0, 1, [target, idx](int b) {
            if (b == 0) DeleteCurrent(target, idx);
        }, true);
    } else {
        DeleteCurrent(target, idx);
    }
}

static void PostTask(TaskDone* t) { if (!PostMessageW(hwnd, WM_LOUPE_TASK_DONE, 0, (LPARAM)t)) delete t; }

static void DoUndo() {
    if (editing) {
        if (!s_editHistory.empty()) { edit = s_editHistory.back(); s_editHistory.pop_back(); editDirty = !edit.IsIdentity(); Invalidate(); }
        return;
    }
    if (s_undo.empty()) { Toast(L"Nothing to undo"); return; }
    UndoItem u = s_undo.back();
    s_undo.pop_back();
    if (u.kind == UndoItem::Delete) {
        std::wstring p = u.a;
        int idx = u.index;
        std::thread([p, idx] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            auto* t = new TaskDone{TaskDone::Restore};
            t->path = p;
            t->index = idx;
            t->ok = shell::RestoreFromRecycleBin(p, FILETIME{});
            CoUninitialize();
            PostTask(t);
        }).detach();
    } else if (u.kind == UndoItem::Save) {
        // put the original back over the edited file
        bool ok = CopyFileW(u.b.c_str(), u.a.c_str(), FALSE) != 0;
        auto* t = new TaskDone{TaskDone::UndoSave};
        t->ok = ok;
        t->path = u.a;
        PostTask(t);
    } else if (u.kind == UndoItem::Rename) {
        if (MoveFileExW(u.b.c_str(), u.a.c_str(), 0)) {
            for (auto& f : files) if (IEquals(f.name, PathName(u.b))) f.name = PathName(u.a);
            if (IEquals(path, u.b)) { path = u.a; UpdateTitle(); }
            for (auto& e : cache) if (IEquals(e->path, u.b)) e->path = u.a;
            SortFiles();
            Toast(L"Renamed back to " + PathName(u.a));
        } else {
            Toast(L"Couldn't rename it back");
        }
    }
    Invalidate();
}

static const wchar_t* kBadNameChars = L"\\/:*?\"<>|";

static void DoRename() {
    if (path.empty() || editing) return;
    std::wstring name = PathName(path);
    size_t dot = name.find_last_of(L'.');
    ShowDialog(L"Rename", L"", {L"Rename", L"Cancel"}, 0, 1, nullptr);
    dialog.field = true;
    dialog.input.SetText(name, 0, dot == std::wstring::npos ? (int)name.size() : (int)dot);
    dialog.input.active = true;
    std::wstring old = path;
    dialog.onClose = [old](int b) {
        if (b != 0) return;
        std::wstring nn = Trim(dialog.input.text);
        if (nn.empty()) { dialog.fieldError = L"Type a name."; return; }
        if (nn.find_first_of(kBadNameChars) != std::wstring::npos) { dialog.fieldError = L"A name can't contain \\ / : * ? \" < > |"; return; }
        if (nn == PathName(old)) { dialog.open = false; return; }
        std::wstring np = PathParent(old) + L"\\" + nn;
        if (FileExists(np) && !IEquals(np, old)) { dialog.fieldError = L"There's already a file with that name."; return; }
        if (!MoveFileExW(old.c_str(), np.c_str(), 0)) { dialog.fieldError = L"Windows couldn't rename it. Is it open in another app?"; return; }
        dialog.open = false;
        for (auto& f : files) if (IEquals(f.name, PathName(old))) f.name = nn;
        for (auto& e : cache) if (IEquals(e->path, old)) e->path = np;
        auto th = thumbs.find(old);
        if (th != thumbs.end()) { Thumb tb = th->second; thumbs.erase(th); thumbs[np] = tb; }
        if (infos.count(old)) { infos[np] = infos[old]; infos.erase(old); }
        if (IEquals(path, old)) path = np;
        SortFiles();
        UpdateTitle();
        s_undo.push_back({UndoItem::Rename, old, np});
        Toast(L"Renamed to " + nn, L"Undo");
    };
}

// ---------------------------------------------------------------- copy, wallpaper
static void DoCopyImage() {
    if (!HasImage()) return;
    std::wstring p = path;
    loader::Run([p] {
        auto* t = new TaskDone{TaskDone::CopyImage};
        ImageData d;
        t->path = p;
        if (DecodeImage(p, 0, 0, false, d) && !d.frames.empty()) { t->pix = std::move(d.frames[0].pix); t->ok = true; }
        PostTask(t);
    });
    Toast(L"Copying…");
}

static void DoWallpaper() {
    if (!HasImage()) return;
    std::wstring p = path;
    loader::Run([p] {
        auto* t = new TaskDone{TaskDone::Wallpaper};
        std::wstring ext = ExtOf(p), use = p;
        if (ext != L".jpg" && ext != L".jpeg" && ext != L".png" && ext != L".bmp") {
            ImageData d;
            use = DataDir() + L"\\wallpaper\\Loupe wallpaper.jpg";
            EnsureDir(PathParent(use));
            if (!DecodeImage(p, 0, 0, false, d) || d.frames.empty() || !EncodeImage(d.frames[0].pix, use, L"jpg", 95, L"", &t->message)) {
                if (t->message.empty()) t->message = L"Couldn't prepare the image for the desktop.";
                PostTask(t);
                return;
            }
        }
        t->ok = shell::SetWallpaper(use, &t->message);
        PostTask(t);
    });
}

// ---------------------------------------------------------------- editing
static void PushEdit() {
    s_editHistory.push_back(edit);
    if (s_editHistory.size() > 100) s_editHistory.erase(s_editHistory.begin());
}

static bool BeginEditing() {
    Entry* e = Current();
    if (!e || !e->ready()) return false;
    if (e->animated) { Toast(L"Animated images can't be edited"); return false; }
    if (editing) return true;
    if (compareMode) compareMode = false;
    if (gridMode) gridMode = false;
    editing = true;
    editDirty = false;
    edit = EditParams{};
    s_editHistory.clear();
    Invalidate();
    return true;
}

static void EditGeometry(std::function<void(EditParams&)> f) {
    if (!BeginEditing()) return;
    Entry* e = Current();
    PushEdit();
    f(edit);
    // keep the crop the same shape on the turned image
    float ow, oh;
    OrientedSize(edit, (float)e->fullW, (float)e->fullH, ow, oh);
    FitCropInside(edit, (float)e->fullW, (float)e->fullH);
    editDirty = !edit.IsIdentity();
    Invalidate();
}

static std::wstring UpperExt(const std::wstring& ext) {
    std::wstring u = ext.size() > 1 ? ext.substr(1) : ext;
    if (!u.empty()) CharUpperBuffW(&u[0], (DWORD)u.size());
    return u;
}

static std::wstring SuggestCopyName(const std::wstring& p, const std::wstring& ext) {
    std::wstring dir = PathParent(p), name = PathName(p);
    std::wstring stem = name.substr(0, name.find_last_of(L'.'));
    for (int n = 1; n < 1000; ++n) {
        std::wstring cand = dir + L"\\" + stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (!FileExists(cand)) return cand;
    }
    return dir + L"\\" + stem + L" (edited)" + ext;
}

static void SaveEdited(bool replace, const std::wstring& target, const std::wstring& format) {
    std::wstring src = path;
    EditParams ep = edit;
    int quality = g_cfg.jpegQuality;
    Toast(L"Saving…");
    loader::Run([src, ep, replace, target, format, quality] {
        auto* t = new TaskDone{replace ? TaskDone::SaveReplace : TaskDone::SaveCopy};
        t->path = target;
        ImageData d;
        if (!DecodeImage(src, 0, 0, false, d) || d.frames.empty()) { t->message = d.error.empty() ? L"Couldn't read the original." : d.error; PostTask(t); return; }
        Pixels out;
        if (!RenderEdited(d.frames[0].pix, ep, out, &t->message)) { PostTask(t); return; }
        std::wstring metaFrom = src;
        if (replace) {
            // keep the original for Undo
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t stamp[64];
            swprintf(stamp, 64, L"%04d%02d%02d-%02d%02d%02d-", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            std::wstring backup = DataDir() + L"\\backup\\" + stamp + PathName(src);
            EnsureDir(PathParent(backup));
            if (!CopyFileW(src.c_str(), backup.c_str(), FALSE)) { t->message = L"Couldn't keep a backup of the original, so it wasn't replaced."; PostTask(t); return; }
            t->path2 = backup;
            metaFrom = backup;
        }
        t->ok = EncodeImage(out, target, format, quality, metaFrom, &t->message);
        PostTask(t);
    });
}

static void DoSave(bool copy) {
    if (!editing) return;
    Entry* e = Current();
    if (!e) return;
    if (edit.IsIdentity() && !copy) { Toast(L"No changes to save"); return; }
    std::wstring ext = ExtOf(path);
    bool canWrite = CanEncodeExtension(ext);
    auto saveCopy = [ext, canWrite] {
        std::wstring fmt;
        std::wstring suggestion = SuggestCopyName(path, canWrite ? ext : std::wstring(L".jpg"));
        std::wstring target = shell::PickSaveAs(hwnd, suggestion, fmt);
        if (target.empty()) return;
        SaveEdited(false, target, fmt);
    };
    if (copy || !canWrite) {
        if (!canWrite && !copy) {
            ShowDialog(L"Save a copy?", L"Loupe can't write " + UpperExt(ext) + L" files, so your edit will be saved as a new JPEG or PNG.",
                       {L"Save a copy", L"Cancel"}, 0, 1, [saveCopy](int b) { if (b == 0) saveCopy(); });
            return;
        }
        saveCopy();
        return;
    }
    ShowDialog(L"Replace the original?", L"This saves your edit over " + PathName(path) + L". You can undo it afterwards with Ctrl+Z.",
               {L"Save a copy", L"Replace", L"Cancel"}, 1, 2, [saveCopy](int b) {
                   if (b == 0) saveCopy();
                   else if (b == 1) SaveEdited(true, path, ExtOf(path).substr(1));
               });
}

// ---------------------------------------------------------------- context and "more" menus
enum MenuId {
    M_OPEN = 1, M_COPY, M_COPYPATH, M_EXPLORER, M_STRATA, M_OPENWITH, M_WALLPAPER, M_RENAME, M_DELETE, M_PRINT,
    M_ROTL, M_ROTR, M_FLIPH, M_FLIPV, M_EDIT, M_COMPARE, M_SLIDESHOW, M_FULLSCREEN, M_INFO, M_FILM, M_GRID, M_SETTINGS,
    M_SORT_NAME, M_SORT_TAKEN, M_SORT_MOD, M_SORT_SIZE, M_SORT_DESC, M_SHORTCUTS, M_DEFAULT,
};

static Cmd MenuToCmd(int id) {
    switch (id) {
        case M_OPEN: return Cmd::Open; case M_COPY: return Cmd::CopyImage; case M_COPYPATH: return Cmd::CopyPath;
        case M_EXPLORER: return Cmd::Explorer; case M_STRATA: return Cmd::Strata; case M_OPENWITH: return Cmd::OpenWith;
        case M_WALLPAPER: return Cmd::Wallpaper; case M_RENAME: return Cmd::Rename; case M_DELETE: return Cmd::Delete;
        case M_PRINT: return Cmd::Print; case M_ROTL: return Cmd::RotateLeft; case M_ROTR: return Cmd::RotateRight;
        case M_FLIPH: return Cmd::FlipH; case M_FLIPV: return Cmd::FlipV; case M_EDIT: return Cmd::Edit;
        case M_COMPARE: return Cmd::Compare; case M_SLIDESHOW: return Cmd::Slideshow; case M_FULLSCREEN: return Cmd::Fullscreen;
        case M_INFO: return Cmd::Info; case M_FILM: return Cmd::Filmstrip; case M_GRID: return Cmd::Grid;
        case M_SETTINGS: return Cmd::Settings; case M_SORT_NAME: return Cmd::SortName; case M_SORT_TAKEN: return Cmd::SortTaken;
        case M_SORT_MOD: return Cmd::SortModified; case M_SORT_SIZE: return Cmd::SortSize; case M_SORT_DESC: return Cmd::SortReverse;
        case M_SHORTCUTS: return Cmd::Shortcuts;
        default: return Cmd::None;
    }
}

static HMENU SortMenu() {
    HMENU s = CreatePopupMenu();
    auto add = [&](int id, const wchar_t* label, bool check) { AppendMenuW(s, MF_STRING | (check ? MF_CHECKED : 0), id, label); };
    add(M_SORT_NAME, L"Name", g_cfg.sort == SortKey::Name);
    add(M_SORT_TAKEN, L"Date taken", g_cfg.sort == SortKey::DateTaken);
    add(M_SORT_MOD, L"Date modified", g_cfg.sort == SortKey::DateModified);
    add(M_SORT_SIZE, L"Size", g_cfg.sort == SortKey::Size);
    AppendMenuW(s, MF_SEPARATOR, 0, nullptr);
    add(M_SORT_DESC, L"Descending", g_cfg.sortDescending);
    return s;
}

static void ShowMenu(int x, int y, bool full) {
    HMENU m = CreatePopupMenu();
    bool img = HasImage();
    UINT dis = img ? 0 : MF_GRAYED;
    auto add = [&](int id, const wchar_t* label, UINT flags = 0) { AppendMenuW(m, MF_STRING | flags, id, label); };
    auto sep = [&] { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };
    add(M_COPY, L"Copy image\tCtrl+C", dis);
    add(M_COPYPATH, L"Copy path\tCtrl+Shift+C", path.empty() ? MF_GRAYED : 0);
    sep();
    add(M_EXPLORER, L"Show in File Explorer\tCtrl+E", path.empty() ? MF_GRAYED : 0);
    if (!shell::StrataExe().empty()) add(M_STRATA, L"Open folder in Strata", path.empty() ? MF_GRAYED : 0);
    add(M_OPENWITH, L"Open with…", path.empty() ? MF_GRAYED : 0);
    add(M_PRINT, L"Print…\tCtrl+P", dis);
    add(M_WALLPAPER, L"Set as desktop background\tCtrl+B", dis);
    sep();
    add(M_ROTL, L"Rotate left\tShift+R", dis);
    add(M_ROTR, L"Rotate right\tR", dis);
    add(M_FLIPH, L"Flip horizontal\tH", dis);
    add(M_FLIPV, L"Flip vertical\tV", dis);
    add(M_EDIT, L"Edit\tE", dis);
    sep();
    add(M_RENAME, L"Rename\tF2", path.empty() ? MF_GRAYED : 0);
    add(M_DELETE, L"Delete\tDel", path.empty() ? MF_GRAYED : 0);
    if (full) {
        sep();
        add(M_COMPARE, L"Compare\tC", files.size() > 1 ? 0 : MF_GRAYED);
        add(M_SLIDESHOW, L"Slideshow\tSpace", dis);
        add(M_FULLSCREEN, fullscreen ? L"Exit full screen\tF11" : L"Full screen\tF11");
        add(M_INFO, L"Info\tI", g_cfg.infoPanel ? MF_CHECKED : 0);
        add(M_FILM, L"Filmstrip\tF", g_cfg.filmstrip ? MF_CHECKED : 0);
        add(M_GRID, L"All images\tG", gridMode ? MF_CHECKED : 0);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)SortMenu(), L"Sort by");
        sep();
        add(M_OPEN, L"Open…\tCtrl+O");
        add(M_SHORTCUTS, L"Keyboard shortcuts\tF1");
        add(M_SETTINGS, L"Settings\tCtrl+,");
    }
    POINT pt{x, y};
    ClientToScreen(hwnd, &pt);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    Cmd c = MenuToCmd(id);
    if (c != Cmd::None) DoCmd(c);
}

// ---------------------------------------------------------------- compare
static void EnterCompare() {
    if (files.size() < 2 && gridMarks.size() < 2) { Toast(L"Compare needs at least two images in the folder"); return; }
    ConfirmLeaveEditing([] {
        if (gridMarks.size() == 2) {
            comparePath[0] = PathAt(gridMarks[0]);
            comparePath[1] = PathAt(gridMarks[1]);
        } else {
            comparePath[0] = path;
            int n = (int)files.size();
            int j = index >= 0 ? (index + 1) % n : 0;
            comparePath[1] = PathAt(j);
        }
        gridMarks.clear();
        gridMode = false;
        compareMode = true;
        comparePane = 1;
        s_cmpMul = 1; s_cmpU = s_cmpV = 0.5f;
        EnsureEntry(comparePath[0], 0);
        EnsureEntry(comparePath[1], 0);
        Toast(L"← → change the highlighted side · Tab switches sides · Esc ends");
        Invalidate();
    });
}

static void LeaveCompare() {
    if (!compareMode) return;
    compareMode = false;
    std::wstring keep = comparePath[comparePane];
    if (!keep.empty() && !IEquals(keep, path)) OpenPath(keep, true);
    ResetView();
    Invalidate();
}

// shared by draw.cpp
float CompareMul() { return s_cmpMul; }
float CompareU() { return s_cmpU; }
float CompareV() { return s_cmpV; }
float SlideFade() { return s_fade; }
uint64_t SlideFadeFrom() { return s_fadeFromId; }
bool SlidePaused() { return s_slidePaused; }
bool MaxButtonHot() { return s_maxHot; }
bool MaxButtonDown() { return s_maxDown; }

// ---------------------------------------------------------------- commands
namespace app {

void DoCmd(Cmd cmd) {
    Entry* e = Current();
    switch (cmd) {
        case Cmd::Prev: ConfirmLeaveEditing([] { Step(-1); }); break;
        case Cmd::Next: ConfirmLeaveEditing([] { Step(1); }); break;
        case Cmd::First: ConfirmLeaveEditing([] { if (!files.empty()) GoTo(0); }); break;
        case Cmd::Last: ConfirmLeaveEditing([] { if (!files.empty()) GoTo((int)files.size() - 1); }); break;
        case Cmd::ZoomIn: if (!editing) ZoomStep(true); break;
        case Cmd::ZoomOut: if (!editing) ZoomStep(false); break;
        case Cmd::ZoomFit: if (!editing) ZoomFit(); break;
        case Cmd::Zoom100:
            if (!editing) {
                if (compareMode) { Entry* a = FindEntry(comparePath[comparePane]); if (a) { s_cmpMul = std::max(1.0f, 1.0f / scale / FitZoom(a, viewRect)); Invalidate(); } }
                else ZoomTo(1.0f / scale, ViewCenterX(), ViewCenterY(), true);
            }
            break;
        case Cmd::Filmstrip:
            g_cfg.filmstrip = !g_cfg.filmstrip;
            SaveSettings();
            filmScroll = filmScrollTarget = -1;
            LayoutViewport();
            if (fitMode) ResetView();
            Invalidate();
            break;
        case Cmd::Grid:
            if (gridMode) { gridMode = false; ResetView(); Invalidate(); break; }
            ConfirmLeaveEditing([] {
                if (files.empty()) { Toast(L"Open an image to see its folder"); return; }
                compareMode = false;
                gridMode = true;
                gridScroll = gridScrollTarget = -1;   // scroll to the current image
                Invalidate();
            });
            break;
        case Cmd::Info:
            g_cfg.infoPanel = !g_cfg.infoPanel;
            SaveSettings();
            if (g_cfg.infoPanel && !path.empty() && !infos.count(path) && !infoPending[path]) { infoPending[path] = true; loader::ReadInfo(path); }
            LayoutViewport();
            if (fitMode) ResetView();
            Invalidate();
            break;
        case Cmd::Edit:
            if (editing) DoCmd(Cmd::EditDone);
            else BeginEditing();
            break;
        case Cmd::RotateLeft:
        case Cmd::RotateRight: {
            bool cw = cmd == Cmd::RotateRight;
            EditGeometry([cw](EditParams& p) {
                // flips are applied after the turn, so with one flip on, turning the inner image the other way looks right
                bool mirrored = p.flipH != p.flipV;
                p.rot90 = (p.rot90 + ((cw != mirrored) ? 1 : 3)) & 3;
                float l = p.cropL, tp = p.cropT, r = p.cropR, b = p.cropB;
                if (cw) { p.cropL = 1 - b; p.cropT = l; p.cropR = 1 - tp; p.cropB = r; }
                else { p.cropL = tp; p.cropT = 1 - r; p.cropR = b; p.cropB = 1 - l; }
                p.aspectPortrait = !p.aspectPortrait;
            });
            break;
        }
        case Cmd::FlipH: EditGeometry([](EditParams& p) { p.flipH = !p.flipH; float l = p.cropL; p.cropL = 1 - p.cropR; p.cropR = 1 - l; p.straighten = -p.straighten; }); break;
        case Cmd::FlipV: EditGeometry([](EditParams& p) { p.flipV = !p.flipV; float tp = p.cropT; p.cropT = 1 - p.cropB; p.cropB = 1 - tp; p.straighten = -p.straighten; }); break;
        case Cmd::Compare: if (compareMode) LeaveCompare(); else EnterCompare(); break;
        case Cmd::Slideshow:
            if (slideshow) { s_slidePaused = !s_slidePaused; Toast(s_slidePaused ? L"Paused" : L"Playing"); }
            else ConfirmLeaveEditing([] { s_slidePaused = false; SetSlideshow(true); });
            break;
        case Cmd::Fullscreen: if (slideshow) SetSlideshow(false); else SetFullscreen(!fullscreen); break;
        case Cmd::CopyImage: DoCopyImage(); break;
        case Cmd::CopyPath:
            if (!path.empty() && shell::CopyText(hwnd, path)) Toast(L"Copied the path");
            break;
        case Cmd::Explorer: if (!path.empty()) shell::ShowInExplorer(path); break;
        case Cmd::Strata:
            if (path.empty()) break;
            if (!shell::OpenInStrata(folder)) Toast(L"Strata isn't installed. You can get it from Hangar.");
            break;
        case Cmd::OpenWith: if (!path.empty()) shell::OpenWith(hwnd, path); break;
        case Cmd::Print: if (HasImage()) ShellExecuteW(hwnd, L"print", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Cmd::Wallpaper: DoWallpaper(); break;
        case Cmd::Delete: DoDelete(); break;
        case Cmd::Rename: DoRename(); break;
        case Cmd::Open:
            ConfirmLeaveEditing([] {
                std::wstring p = shell::PickImage(hwnd, folder);
                if (!p.empty()) { gridMode = false; compareMode = false; OpenPath(p); }
            });
            break;
        case Cmd::Settings:
            settingsOpen = !settingsOpen;
            if (settingsOpen) {
                prevVersions = updater::PreviousVersions();
                customThemes = LoadCustomThemes();
                settingsScroll = settingsScrollTarget = 0;
                accentField.active = false;
            }
            Invalidate();
            break;
        case Cmd::Save: DoSave(false); break;
        case Cmd::SaveCopy: DoSave(true); break;
        case Cmd::Revert:
            if (editing && !edit.IsIdentity()) { PushEdit(); edit = EditParams{}; editDirty = false; Invalidate(); }
            break;
        case Cmd::Undo: DoUndo(); break;
        case Cmd::PlayPause:
            if (e && e->animated) { paused = !paused; s_frameStart = GetTickCount(); s_loopsDone = 0; Toast(paused ? L"Paused" : L"Playing"); Invalidate(); }
            break;
        case Cmd::FrameNext:
        case Cmd::FramePrev:
            if (e && e->animated && !e->frames.empty()) {
                paused = true;
                int n = (int)e->frames.size();
                frameIndex = ((frameIndex + (cmd == Cmd::FrameNext ? 1 : -1)) % n + n) % n;
                Toast(L"Frame " + std::to_wstring(frameIndex + 1) + L" of " + std::to_wstring(n));
                Invalidate();
            }
            break;
        case Cmd::Shortcuts: shortcutsOpen = !shortcutsOpen; Invalidate(); break;
        case Cmd::SortName: case Cmd::SortTaken: case Cmd::SortModified: case Cmd::SortSize: {
            SortKey k = cmd == Cmd::SortName ? SortKey::Name : cmd == Cmd::SortTaken ? SortKey::DateTaken : cmd == Cmd::SortModified ? SortKey::DateModified : SortKey::Size;
            g_cfg.sort = k;
            SaveSettings();
            if (k == SortKey::DateTaken && !datesLoaded) { RequestDates(); Toast(L"Reading dates…"); }
            SortFiles();
            Preload();
            Invalidate();
            break;
        }
        case Cmd::SortReverse:
            g_cfg.sortDescending = !g_cfg.sortDescending;
            SaveSettings();
            SortFiles();
            Preload();
            Invalidate();
            break;
        case Cmd::EditDone:
            if (!editing) break;
            if (editDirty) {
                ShowDialog(L"Save your edit?", L"Save it over the original, as a copy, or throw it away.", {L"Discard", L"Save a copy", L"Save"}, 2, -1, [](int b) {
                    if (b == 0) LeaveEditing();
                    else if (b == 1) DoSave(true);
                    else if (b == 2) DoSave(false);
                });
                dialog.cancel = -1;
            } else {
                LeaveEditing();
            }
            Invalidate();
            break;
        case Cmd::EditCancel: ConfirmLeaveEditing([] {}); Invalidate(); break;
        case Cmd::Close: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case Cmd::More: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            ShowMenu(p.x, p.y + 10, true);
            break;
        }
        case Cmd::Escape:
            if (shortcutsOpen) { shortcutsOpen = false; Invalidate(); }
            else if (settingsOpen) { settingsOpen = false; Invalidate(); }
            else if (slideshow) SetSlideshow(false);
            else if (editing) DoCmd(Cmd::EditCancel);
            else if (compareMode) LeaveCompare();
            else if (gridMode) { gridMode = false; ResetView(); Invalidate(); }
            else if (fullscreen) SetFullscreen(false);
            else if (!fitMode) ZoomFit();
            break;
        default: break;
    }
}

} // namespace app

// ---------------------------------------------------------------- clicks
static void OpenFromGrid(int i) {
    if (i < 0 || i >= (int)files.size()) return;
    gridMode = false;
    GoTo(i);
    ResetView();
}

namespace app {

void Click(const HotRect& h, float x, float y) {
    switch (h.kind) {
        case Hit::MinBtn: ShowWindow(hwnd, SW_MINIMIZE); break;
        case Hit::MaxBtn: ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case Hit::CloseBtn: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case Hit::Tool: DoCmd((Cmd)h.arg); break;
        case Hit::NavPrev: DoCmd(Cmd::Prev); break;
        case Hit::NavNext: DoCmd(Cmd::Next); break;
        case Hit::Film:
            if (s_filmDragged) break;
            if (compareMode) {
                comparePath[comparePane] = PathAt(h.arg);
                EnsureEntry(comparePath[comparePane], 0);
                TrimCache();
                Invalidate();
            } else {
                int i = h.arg;
                ConfirmLeaveEditing([i] { GoTo(i); });
            }
            break;
        case Hit::GridCell: {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (ctrl) {
                auto it = std::find(gridMarks.begin(), gridMarks.end(), h.arg);
                if (it != gridMarks.end()) gridMarks.erase(it);
                else { if (gridMarks.size() >= 2) gridMarks.erase(gridMarks.begin()); gridMarks.push_back(h.arg); }
                if (gridMarks.size() == 2) Toast(L"Press C to compare these two");
            } else {
                index = h.arg;
                path = PathAt(h.arg);
                EnsureEntry(path, 0);
                UpdateTitle();
            }
            Invalidate();
            break;
        }
        case Hit::Pane: comparePane = h.arg; Invalidate(); break;
        case Hit::PanelClose: if (editing) DoCmd(Cmd::EditCancel); else DoCmd(Cmd::Info); break;
        case Hit::InfoMaps: {
            auto it = infos.find(path);
            if (it != infos.end() && it->second.hasGps) {
                wchar_t url[160];
                swprintf(url, 160, L"https://www.google.com/maps/search/?api=1&query=%.6f,%.6f", it->second.lat, it->second.lon);
                shell::OpenUrl(url);
            }
            break;
        }
        case Hit::InfoFolder: DoCmd(Cmd::Explorer); break;
        case Hit::CodecLink: {
            Entry* e = Current();
            if (e && !e->codecStoreId.empty()) shell::OpenUrl(L"ms-windows-store://pdp/?ProductId=" + e->codecStoreId);
            break;
        }
        case Hit::Welcome:
            if (h.arg == 0) DoCmd(Cmd::Open);
            else if (h.arg == 1) shell::OpenDefaultAppsSettings();
            else if (h.arg == 2) DoCmd(Cmd::Settings);
            break;
        case Hit::DialogButton: CloseDialog(h.arg); break;
        case Hit::ToastAction: toastUntil = 0; DoUndo(); break;
        case Hit::UpdateRestart: {
            if (update.kind != UpdateState::Ready) break;
            ConfirmLeaveEditing([] {
                std::wstring err, args = path.empty() ? L"" : L"\"" + path + L"\"";
                ConfigSave();
                if (updater::Apply(hwnd, update.staged, update.version, args, false, &err)) {
                    s_quitting = true;
                    DestroyWindow(hwnd);
                } else {
                    SkipVersion(update.version);
                    ConfigSave();
                    update = UpdateState{};
                    Toast(err);
                }
            });
            break;
        }
        case Hit::UpdateDismiss: updateDismissed = true; Invalidate(); break;
        case Hit::SlideControl:
            if (h.arg == 0) DoCmd(Cmd::Prev);
            else if (h.arg == 1) { s_slidePaused = !s_slidePaused; Invalidate(); }
            else if (h.arg == 2) DoCmd(Cmd::Next);
            else if (h.arg == 3) { g_cfg.slideshowSeconds = std::max(1, g_cfg.slideshowSeconds - 1); SaveSettings(); SetTimer(hwnd, TIMER_SLIDE, g_cfg.slideshowSeconds * 1000, nullptr); Invalidate(); }
            else if (h.arg == 4) { g_cfg.slideshowSeconds = std::min(60, g_cfg.slideshowSeconds + 1); SaveSettings(); SetTimer(hwnd, TIMER_SLIDE, g_cfg.slideshowSeconds * 1000, nullptr); Invalidate(); }
            else if (h.arg == 5) { g_cfg.slideshowShuffle = !g_cfg.slideshowShuffle; SaveSettings(); Invalidate(); }
            else if (h.arg == 6) SetSlideshow(false);
            break;
        case Hit::SortMenu: {
            HMENU m = SortMenu();
            POINT pt{(int)(h.r.left * scale), (int)(h.r.bottom * scale)};
            ClientToScreen(hwnd, &pt);
            int id = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            Cmd c = MenuToCmd(id);
            if (c != Cmd::None) DoCmd(c);
            break;
        }
        case Hit::Shortcuts: shortcutsOpen = false; Invalidate(); break;
        case Hit::EditAspect: case Hit::EditPortrait: case Hit::EditButton: case Hit::EditSlider: case Hit::CropHandle:
            if (h.kind == Hit::EditButton || h.kind == Hit::EditAspect || h.kind == Hit::EditPortrait) PushEdit();
            EditClick(h, x, y);
            editDirty = !edit.IsIdentity();
            break;
        case Hit::SetTheme: case Hit::SetAccent: case Hit::SetSeg: case Hit::SetToggle: case Hit::SetButton: case Hit::SetStep:
        case Hit::AccentField: case Hit::SetBack:
            SettingsClick(h, x, y);
            break;
        default: break;
    }
    Invalidate();
}

} // namespace app

// settings.cpp calls these
void UiCheckUpdates() { updater::Check(hwnd, true); }
void UiRollback(int i) {
    if (i < 0 || i >= (int)prevVersions.size()) return;
    std::wstring err, args = path.empty() ? L"" : L"\"" + path + L"\"";
    auto pv = prevVersions[i];
    SkipVersion(LOUPE_VERSION);
    ConfigSave();
    if (updater::Apply(hwnd, pv.exe, pv.version, args, false, &err)) { s_quitting = true; DestroyWindow(hwnd); }
    else Toast(err);
}
void UiSetSlideTimer() { if (slideshow) SetTimer(hwnd, TIMER_SLIDE, g_cfg.slideshowSeconds * 1000, nullptr); }
void UiPushEdit() { PushEdit(); }

// ---------------------------------------------------------------- results from workers
static void OnDecoded(DecodeResult* r) {
    Entry* e = nullptr;
    for (auto& x : cache) if (x->id == r->id) e = x.get();
    if (SmokeActive()) SmokeOnDecoded(r->id, r->full, r->ms, r->path);
    if (!e) return;
    if (!EnsureDevice()) return;
    if (r->full) {
        e->fullPending = false;
        if (!r->data.frames.empty() && Upload(r->data.frames[0].pix, e->full)) e->fullReady = true;
        Invalidate();
        return;
    }
    bool wasReady = e->ready();
    e->viewPending = false;
    e->viewReady = true;
    e->decodeMs = r->ms;
    ImageData& d = r->data;
    e->format = d.format;
    e->fullW = d.fullW;
    e->fullH = d.fullH;
    e->hasAlpha = d.hasAlpha;
    e->animated = d.animated;
    e->vector = d.vector;
    e->loops = d.loops;
    e->error = d.error;
    e->codecName = d.codecName;
    e->codecStoreId = d.codecStoreId;
    e->viewIsFull = d.fullRes;
    for (auto& f : e->frames) f.Release();
    e->frames.clear();
    e->delays.clear();
    for (auto& f : d.frames) {
        GpuImage g;
        if (!Upload(f.pix, g)) break;
        std::vector<uint8_t>().swap(f.pix.px);   // the GPU has it now
        e->frames.push_back(std::move(g));
        e->delays.push_back(f.delayMs);
    }
    if (e->frames.empty() && e->error.empty()) e->error = L"Not enough graphics memory to show this image.";
    if (IEquals(e->path, path) && !wasReady) {
        ResetView();
        frameIndex = 0;
        s_frameStart = GetTickCount();
        if (e->animated) StartAnim();
        if (!s_firstPixelsLogged) {
            s_firstPixelsLogged = true;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            FILETIME ct, xt, kt, ut;
            double sinceProcess = -1;
            if (GetProcessTimes(GetCurrentProcess(), &ct, &xt, &kt, &ut)) {
                FILETIME nowFt;
                GetSystemTimePreciseAsFileTime(&nowFt);
                ULARGE_INTEGER a, b;
                a.LowPart = ct.dwLowDateTime; a.HighPart = ct.dwHighDateTime;
                b.LowPart = nowFt.dwLowDateTime; b.HighPart = nowFt.dwHighDateTime;
                sinceProcess = (b.QuadPart - a.QuadPart) / 10000.0;
            }
            s_firstPixelsMs = sinceProcess;
            Log(L"first image ready: %.0f ms after start (decode %.0f ms, %ls %ux%u)", sinceProcess, r->ms, e->format.c_str(), e->fullW, e->fullH);
        }
        Preload();
    }
    if (compareMode) Invalidate();
    Invalidate();
}

static void OnThumb(ThumbResult* r) {
    auto it = thumbs.find(r->path);
    if (it == thumbs.end()) return;
    Thumb& th = it->second;
    th.pending = false;
    if (r->failed || r->pix.empty()) { th.failed = true; Invalidate(); return; }
    if (!EnsureDevice()) return;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ID2D1Bitmap* b = nullptr;
    if (r->pix.w <= maxBitmap && r->pix.h <= maxBitmap && SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(r->pix.w, r->pix.h), r->pix.px.data(), r->pix.w * 4, bp, &b))) {
        if (th.bmp) th.bmp->Release();
        th.bmp = b;
        th.w = r->pix.w;
        th.h = r->pix.h;
        th.size = r->size;
    }
    TrimThumbs();
    Invalidate();
}

static void OnFolder(FolderResult* r) {
    if (r->gen != s_folderGen || !IEquals(r->folder, folder)) return;
    // keep dates already read
    std::map<std::wstring, int64_t> taken;
    for (auto& f : files) if (f.takenRead) taken[Lower(f.name)] = f.taken;
    files = std::move(r->files);
    for (auto& f : files) {
        auto it = taken.find(Lower(f.name));
        if (it != taken.end()) { f.taken = it->second; f.takenRead = true; }
    }
    std::wstring want = s_pendingSelect.empty() ? path : s_pendingSelect;
    s_pendingSelect.clear();
    if (!want.empty() && !IEquals(want, path)) { path = want; EnsureEntry(path, 0); }
    SortFiles();
    if (g_cfg.sort == SortKey::DateTaken) {
        bool missing = false;
        for (auto& f : files) missing |= !f.takenRead;
        if (missing) { datesLoaded = false; RequestDates(); }
    }
    if (index < 0 && !path.empty() && !FileExists(path) && !files.empty()) {
        // the shown image was deleted or renamed elsewhere
        Entry* e = Current();
        if (e) e->error = L"This image was moved or deleted.";
    }
    Preload();
    LayoutViewport();
    if (fitMode && HasImage()) ResetView();
    Invalidate();
}

static void OnDates(DatesResult* r) {
    if (r->gen != s_folderGen) return;
    std::map<std::wstring, int64_t> m;
    for (auto& p : r->taken) m[Lower(p.first)] = p.second;
    for (auto& f : files) {
        auto it = m.find(Lower(f.name));
        if (it != m.end()) { f.taken = it->second; f.takenRead = true; }
    }
    datesLoaded = true;
    if (g_cfg.sort == SortKey::DateTaken) { SortFiles(); Preload(); }
    Invalidate();
}

static void OnTask(TaskDone* t) {
    switch (t->kind) {
        case TaskDone::CopyImage:
            if (t->ok && shell::CopyImage(hwnd, t->pix, t->path)) Toast(L"Copied the image");
            else Toast(L"Couldn't copy the image");
            break;
        case TaskDone::Wallpaper:
            Toast(t->ok ? L"Set as your desktop background" : t->message);
            break;
        case TaskDone::SaveReplace:
        case TaskDone::SaveCopy:
            if (!t->ok) { Toast(t->message.empty() ? L"Saving failed." : t->message); break; }
            if (t->kind == TaskDone::SaveReplace) {
                s_undo.push_back({UndoItem::Save, t->path, t->path2});
                if (Entry* e = FindEntry(t->path)) { e->Release(); e->viewReady = false; e->viewPending = false; }
                auto th = thumbs.find(t->path);
                if (th != thumbs.end()) { if (th->second.bmp) th->second.bmp->Release(); thumbs.erase(th); }
                infos.erase(t->path);
                LeaveEditing();
                EnsureEntry(path, 0);
                Toast(L"Saved", L"Undo");
            } else {
                LeaveEditing();
                Toast(L"Saved a copy: " + PathName(t->path));
            }
            break;
        case TaskDone::Restore:
            if (t->ok) {
                s_pendingSelect = t->path;
                ++s_folderGen;
                if (IEquals(PathParent(t->path), folder)) loader::ListFolder(s_folderGen, folder);
                else OpenPath(t->path);
                path = t->path;
                EnsureEntry(path, 0);
                UpdateTitle();
                Toast(L"Restored " + PathName(t->path));
            } else {
                Toast(L"Couldn't restore it. Look in the Recycle Bin.");
            }
            break;
        case TaskDone::UndoSave:
            if (t->ok) {
                if (Entry* e = FindEntry(t->path)) { e->Release(); e->viewReady = false; e->viewPending = false; }
                auto th = thumbs.find(t->path);
                if (th != thumbs.end()) { if (th->second.bmp) th->second.bmp->Release(); thumbs.erase(th); }
                EnsureEntry(path, 0);
                Toast(L"Put the original back");
            } else {
                Toast(L"Couldn't put the original back");
            }
            break;
    }
    Invalidate();
}

static void OnUpdate(UpdateState* s) {
    bool manual = s->manual;
    if (s->kind == UpdateState::Error && !manual && update.kind == UpdateState::Ready) return;   // keep a ready update
    update = *s;
    if (s->kind == UpdateState::Ready) updateDismissed = false;
    if (manual && s->kind == UpdateState::UpToDate && settingsOpen) Toast(std::wstring(L"Loupe ") + LOUPE_VERSION + L" is up to date");
    if (manual && s->kind == UpdateState::Error) Toast(s->message);
    if (s->kind == UpdateState::UpToDate || s->kind == UpdateState::Ready) {
        g_cfg.lastUpdateCheck = (long long)time(nullptr);
        ConfigSave();
    }
    Invalidate();
}

// ---------------------------------------------------------------- animation
static bool Ease(float& v, float target, float k, float snap) {
    if (std::fabs(v - target) <= snap) { v = target; return false; }
    v += (target - v) * k;
    return true;
}

static bool AnimationsEnabled() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

static void Animate() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)((now.QuadPart - s_lastAnim.QuadPart) / (double)s_qpf.QuadPart);
    s_lastAnim = now;
    dt = std::clamp(dt, 0.0f, 0.1f);
    bool anim = AnimationsEnabled();
    float k = anim ? 1 - std::exp(-dt * 16) : 1;
    bool more = false;
    more |= Ease(zoom, zoomTarget, k, zoomTarget * 0.0005f);
    more |= Ease(cx, cxTarget, k, 0.05f);
    more |= Ease(cy, cyTarget, k, 0.05f);
    // chrome fades
    bool idle = GetTickCount() - s_lastMove > 1800;
    bool overChrome = hover.kind != Hit::None && hover.kind != Hit::Viewport && hover.kind != Hit::Pane;
    {
        // the pointer resting on the title bar (non-client, so no hover) keeps it up too
        POINT cp;
        RECT rc;
        if (GetCursorPos(&cp) && ScreenToClient(hwnd, &cp) && GetClientRect(hwnd, &rc) && PtInRect(&rc, cp) && cp.y < kTopBar * scale &&
            GetForegroundWindow() == hwnd) overChrome = true;
    }
    float want = (chromePinned() || !idle || overChrome || pressed.kind != Hit::None) ? 1.0f : 0.0f;
    if (slideshow && idle) want = 0;
    more |= Ease(chromeAlpha, want, anim ? 1 - std::exp(-dt * (want > chromeAlpha ? 18.0f : 7.0f)) : 1, 0.01f);
    // toast
    bool toastOn = !toastText.empty() && GetTickCount() < toastUntil;
    more |= Ease(toastAlpha, toastOn ? 1.0f : 0.0f, anim ? 1 - std::exp(-dt * 12) : 1, 0.01f);
    if (!toastOn && toastAlpha <= 0.01f) toastText.clear();
    more |= toastOn;
    // slideshow crossfade
    if (s_fade > 0) {
        s_fade = std::max(0.0f, s_fade - dt / 0.35f);
        if (!anim) s_fade = 0;
        if (s_fade <= 0) s_fadeFromId = 0;
        more = true;
    }
    // scrolling
    if (gridScrollTarget >= 0) more |= Ease(gridScroll, gridScrollTarget, anim ? 1 - std::exp(-dt * 18) : 1, 0.3f);
    if (filmScrollTarget >= 0) more |= Ease(filmScroll, filmScrollTarget, anim ? 1 - std::exp(-dt * 18) : 1, 0.3f);
    more |= Ease(settingsScroll, settingsScrollTarget, anim ? 1 - std::exp(-dt * 18) : 1, 0.3f);
    // animated images
    Entry* e = Current();
    if (e && e->animated && e->frames.size() > 1 && !paused && !editing) {
        DWORD nowT = GetTickCount();
        UINT delay = e->delays.empty() ? 100 : e->delays[std::min<size_t>(frameIndex, e->delays.size() - 1)];
        if (nowT - s_frameStart >= delay) {
            int n = (int)e->frames.size();
            if (frameIndex + 1 >= n) {
                ++s_loopsDone;
                if (e->loops > 0 && s_loopsDone >= e->loops) { paused = true; }
                else frameIndex = 0;
            } else {
                ++frameIndex;
            }
            s_frameStart = nowT - std::min<DWORD>(nowT - s_frameStart - delay, delay);
        }
        more |= !paused;
    }
    if (compareMode) {
        for (auto& p : comparePath) if (Entry* x = FindEntry(p); x && x->animated) more = true;
    }
    if (slideshow) more = true;   // the controls fade
    Invalidate();
    if (!more) {
        s_animating = false;
        KillTimer(hwnd, TIMER_ANIM);
        CheckFullNeeded();
        // nothing left to fade: stop waking up
        if ((chromeAlpha <= 0.0f || chromePinned()) && toastText.empty()) KillTimer(hwnd, TIMER_IDLE);
    }
}

// ---------------------------------------------------------------- hit testing and the window frame
static HotRect HitAt(float x, float y) {
    for (size_t i = hot.size(); i-- > 0;) {
        const HotRect& h = hot[i];
        if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return h;
    }
    return HotRect{{0, 0, 0, 0}, Hit::None, 0, true, nullptr};
}

static bool SameHit(const HotRect& a, const HotRect& b) { return a.kind == b.kind && a.arg == b.arg; }

static LRESULT NcHitTest(int sx, int sy) {
    POINT p{sx, sy};
    ScreenToClient(hwnd, &p);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (fullscreen) return HTCLIENT;
    int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, GetDpiForWindow(hwnd)) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(hwnd));
    if (!IsZoomed(hwnd) && p.y >= 0 && p.y < border / 2 + 1) {
        if (p.x < border) return HTTOPLEFT;
        if (p.x >= rc.right - border) return HTTOPRIGHT;
        return HTTOP;
    }
    float x = p.x / scale, y = p.y / scale;
    HotRect h = HitAt(x, y);
    if (h.kind == Hit::MaxBtn) return HTMAXBUTTON;
    if (h.kind == Hit::Caption && chromeAlpha > 0.5f) return HTCAPTION;
    return HTCLIENT;
}

// ---------------------------------------------------------------- input
static float ScrollAmount(int delta) { return -delta / 120.0f * 96.0f; }

static void OnMouseWheel(int delta, float x, float y, bool horizontal) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    s_lastMove = GetTickCount();
    if (dialog.open) return;
    if (shortcutsOpen) return;
    if (settingsOpen) {
        float maxS = std::max(0.0f, settingsContentH - (H - kTopBar));
        settingsScrollTarget = std::clamp(settingsScrollTarget + ScrollAmount(delta), 0.0f, maxS);
        StartAnim();
        return;
    }
    if (gridMode) {
        if (ctrl) {
            g_cfg.gridSize = std::clamp(g_cfg.gridSize + (delta > 0 ? 16 : -16), 96, 320);
            SaveSettings();
            Invalidate();
            return;
        }
        float maxS = std::max(0.0f, gridContentH - (H - kTopBar));
        if (gridScrollTarget < 0) gridScrollTarget = gridScroll;
        gridScrollTarget = std::clamp(gridScrollTarget + ScrollAmount(delta) * 1.5f, 0.0f, maxS);
        StartAnim();
        return;
    }
    // filmstrip: scroll it
    HotRect h = HitAt(x, y);
    if (h.kind == Hit::Film || h.kind == Hit::FilmArea) {
        if (filmScrollTarget < 0) filmScrollTarget = filmScroll;
        filmScrollTarget += ScrollAmount(delta) * (horizontal ? -1 : 1);
        StartAnim();
        return;
    }
    if (editing) return;
    if (horizontal) { DoCmd(delta > 0 ? Cmd::Next : Cmd::Prev); return; }
    bool zoomIt = g_cfg.wheelZooms != ctrl;
    if (zoomIt && HasImage()) {
        float notches = delta / 120.0f;
        ZoomBy(std::pow(1.2f, notches), x, y);
    } else {
        // one image per notch; precision touchpads send small deltas, so collect them
        static int acc = 0;
        acc += delta;
        if (std::abs(acc) >= 120) {
            DoCmd(acc > 0 ? Cmd::Prev : Cmd::Next);
            acc = 0;
        }
    }
}

static void PanBy(float dx, float dy) {
    if (compareMode) {
        Entry* a = FindEntry(comparePath[comparePane]);
        if (!a || !a->fullW) return;
        D2D1_RECT_F half = viewRect;
        half.right = (viewRect.left + viewRect.right) / 2;
        float z = FitZoom(a, half) * s_cmpMul;
        s_cmpU = std::clamp(s_cmpU - dx / (z * a->fullW), 0.0f, 1.0f);
        s_cmpV = std::clamp(s_cmpV - dy / (z * a->fullH), 0.0f, 1.0f);
        Invalidate();
        return;
    }
    Entry* e = Current();
    if (!e) return;
    cxTarget -= dx / zoomTarget;
    cyTarget -= dy / zoomTarget;
    ClampPan(cxTarget, cyTarget, zoomTarget, e, viewRect);
    cx = cxTarget;
    cy = cyTarget;
    Invalidate();
}

static bool CanPan() {
    if (compareMode) return s_cmpMul > 1.001f;
    Entry* e = Current();
    if (!e || !e->ready()) return false;
    return e->fullW * zoomTarget > (viewRect.right - viewRect.left) + 0.5f || e->fullH * zoomTarget > (viewRect.bottom - viewRect.top) + 0.5f;
}

static void OnKey(WPARAM vk, bool repeat) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    s_lastMove = GetTickCount() - 1000;   // keys don't keep the chrome up long
    if (dialog.open) {
        if (vk == VK_ESCAPE) { CloseDialog(dialog.cancel); return; }
        if (vk == VK_RETURN) { CloseDialog(dialog.primary >= 0 ? dialog.primary : 0); return; }
        if (dialog.field && dialog.input.Key(vk, ctrl, shift, hwnd)) { dialog.fieldError.clear(); Invalidate(); }
        return;
    }
    if (shortcutsOpen) { shortcutsOpen = false; Invalidate(); return; }
    if (settingsOpen && accentField.active) {
        if (vk == VK_ESCAPE) { accentField.active = false; Invalidate(); return; }
        if (vk == VK_RETURN) { HotRect h{{0, 0, 0, 0}, Hit::AccentField, 1, true, nullptr}; SettingsClick(h, 0, 0); return; }
        if (accentField.Key(vk, ctrl, shift, hwnd)) { Invalidate(); return; }
    }
    if (ctrl && vk == VK_OEM_COMMA) { DoCmd(Cmd::Settings); return; }
    if (vk == VK_F1 || (shift && vk == VK_OEM_2)) { DoCmd(Cmd::Shortcuts); return; }
    if (ctrl && vk == 'O') { DoCmd(Cmd::Open); return; }
    if (ctrl && vk == 'W') { DoCmd(Cmd::Close); return; }
    if (vk == VK_F11) { DoCmd(Cmd::Fullscreen); return; }
    if (vk == VK_ESCAPE) { DoCmd(Cmd::Escape); return; }
    if (settingsOpen) {
        if (vk == VK_BACK) settingsOpen = false;
        else if (vk == VK_PRIOR || vk == VK_NEXT) { settingsScrollTarget = std::max(0.0f, settingsScrollTarget + (vk == VK_NEXT ? 1 : -1) * (H - 120)); StartAnim(); }
        Invalidate();
        return;
    }
    if (ctrl && vk == 'Z') { DoCmd(Cmd::Undo); return; }

    if (gridMode) {
        int n = (int)files.size();
        if (!n) return;
        float gl, gt, cell, gsz;
        int cols;
        GridGeometry(gl, gt, cell, gsz, cols);
        int i = std::max(index, 0);
        switch (vk) {
            case VK_LEFT: i -= 1; break;
            case VK_RIGHT: i += 1; break;
            case VK_UP: i -= cols; break;
            case VK_DOWN: i += cols; break;
            case VK_HOME: i = 0; break;
            case VK_END: i = n - 1; break;
            case VK_PRIOR: i -= cols * std::max(1, (int)((H - kTopBar) / cell)); break;
            case VK_NEXT: i += cols * std::max(1, (int)((H - kTopBar) / cell)); break;
            case VK_RETURN: case VK_SPACE: OpenFromGrid(index); return;
            case 'G': DoCmd(Cmd::Grid); return;
            case 'C': if (!ctrl) { DoCmd(Cmd::Compare); return; } break;
            case VK_DELETE: DoCmd(Cmd::Delete); return;
            case VK_F2: DoCmd(Cmd::Rename); return;
            case VK_OEM_PLUS: case VK_ADD: g_cfg.gridSize = std::min(320, g_cfg.gridSize + 16); SaveSettings(); Invalidate(); return;
            case VK_OEM_MINUS: case VK_SUBTRACT: g_cfg.gridSize = std::max(96, g_cfg.gridSize - 16); SaveSettings(); Invalidate(); return;
            default: break;
        }
        if (ctrl && vk == 'C') { DoCmd(shift ? Cmd::CopyPath : Cmd::CopyImage); return; }
        i = std::clamp(i, 0, n - 1);
        if (i != index) {
            index = i;
            path = PathAt(i);
            EnsureEntry(path, 0);
            UpdateTitle();
            // keep the selection on screen
            gridScrollTarget = -2;
            Invalidate();
        }
        return;
    }

    if (slideshow) {
        switch (vk) {
            case VK_SPACE: DoCmd(Cmd::Slideshow); return;
            case VK_UP: g_cfg.slideshowSeconds = std::max(1, g_cfg.slideshowSeconds - 1); SaveSettings(); UiSetSlideTimer(); Toast(std::to_wstring(g_cfg.slideshowSeconds) + L" seconds per image"); return;
            case VK_DOWN: g_cfg.slideshowSeconds = std::min(60, g_cfg.slideshowSeconds + 1); SaveSettings(); UiSetSlideTimer(); Toast(std::to_wstring(g_cfg.slideshowSeconds) + L" seconds per image"); return;
            default: break;
        }
    }

    if (editing) {
        if (ctrl && vk == 'S') { DoCmd(shift ? Cmd::SaveCopy : Cmd::Save); return; }
        if (vk == VK_RETURN) { DoCmd(Cmd::EditDone); return; }
    }

    switch (vk) {
        case VK_LEFT: if (ctrl) PanBy(60, 0); else if (!repeat || !editing) DoCmd(Cmd::Prev); return;
        case VK_RIGHT: if (ctrl) PanBy(-60, 0); else if (!repeat || !editing) DoCmd(Cmd::Next); return;
        case VK_UP: if (ctrl) PanBy(0, 60); else DoCmd(Cmd::ZoomIn); return;
        case VK_DOWN: if (ctrl) PanBy(0, -60); else DoCmd(Cmd::ZoomOut); return;
        case VK_PRIOR: case VK_BACK: DoCmd(Cmd::Prev); return;
        case VK_NEXT: DoCmd(Cmd::Next); return;
        case VK_HOME: DoCmd(Cmd::First); return;
        case VK_END: DoCmd(Cmd::Last); return;
        case VK_SPACE: DoCmd(Cmd::Slideshow); return;
        case VK_DELETE: DoCmd(Cmd::Delete); return;
        case VK_F2: DoCmd(Cmd::Rename); return;
        case VK_F5: if (!folder.empty()) { ++s_folderGen; loader::ListFolder(s_folderGen, folder); } return;
        case VK_OEM_PLUS: case VK_ADD: DoCmd(Cmd::ZoomIn); return;
        case VK_OEM_MINUS: case VK_SUBTRACT: DoCmd(Cmd::ZoomOut); return;
        case '0': case VK_NUMPAD0: DoCmd(Cmd::ZoomFit); return;
        case '1': case VK_NUMPAD1: DoCmd(Cmd::Zoom100); return;
        case VK_OEM_COMMA: DoCmd(Cmd::FramePrev); return;
        case VK_OEM_PERIOD: DoCmd(Cmd::FrameNext); return;
        default: break;
    }
    if (ctrl) {
        switch (vk) {
            case 'C': DoCmd(shift ? Cmd::CopyPath : Cmd::CopyImage); return;
            case 'E': DoCmd(shift ? Cmd::Strata : Cmd::Explorer); return;
            case 'B': DoCmd(Cmd::Wallpaper); return;
            case 'P': DoCmd(Cmd::Print); return;
            case 'S': if (!editing && HasImage()) { if (BeginEditing()) DoCmd(shift ? Cmd::SaveCopy : Cmd::Save); } return;
            default: return;
        }
    }
    switch (vk) {
        case 'F': DoCmd(Cmd::Filmstrip); break;
        case 'G': DoCmd(Cmd::Grid); break;
        case 'I': DoCmd(Cmd::Info); break;
        case 'E': DoCmd(Cmd::Edit); break;
        case 'C': DoCmd(Cmd::Compare); break;
        case 'P': case 'K': DoCmd(Cmd::PlayPause); break;
        case 'R': DoCmd(shift ? Cmd::RotateLeft : Cmd::RotateRight); break;
        case 'L': DoCmd(Cmd::RotateLeft); break;
        case 'H': DoCmd(Cmd::FlipH); break;
        case 'V': DoCmd(Cmd::FlipV); break;
        case 'S': { HotRect h{{W - 300, 0, W - 260, kTopBar}, Hit::SortMenu, 0, true, nullptr}; Click(h, 0, 0); break; }
        case VK_TAB: if (compareMode) { comparePane = 1 - comparePane; Invalidate(); } break;
        default: break;
    }
}

// ---------------------------------------------------------------- text field
static bool ClipboardText(HWND h, std::wstring& out) {
    if (!OpenClipboard(h)) return false;
    bool ok = false;
    if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(d)) { out = p; ok = true; GlobalUnlock(d); }
    }
    CloseClipboard();
    return ok;
}

bool TextField::Key(WPARAM vk, bool ctrl, bool shift, HWND h) {
    int n = (int)text.size();
    auto del = [&] {
        int a = std::min(caret, anchor), b = std::max(caret, anchor);
        text.erase(a, b - a);
        caret = anchor = a;
    };
    switch (vk) {
        case VK_LEFT: if (HasSelection() && !shift) caret = anchor = std::min(caret, anchor); else { caret = std::max(0, caret - 1); if (!shift) anchor = caret; } return true;
        case VK_RIGHT: if (HasSelection() && !shift) caret = anchor = std::max(caret, anchor); else { caret = std::min(n, caret + 1); if (!shift) anchor = caret; } return true;
        case VK_HOME: caret = 0; if (!shift) anchor = 0; return true;
        case VK_END: caret = n; if (!shift) anchor = n; return true;
        case VK_BACK: if (HasSelection()) del(); else if (caret > 0) { text.erase(caret - 1, 1); anchor = --caret; } return true;
        case VK_DELETE: if (HasSelection()) del(); else if (caret < n) text.erase(caret, 1); return true;
        default: break;
    }
    if (ctrl && vk == 'A') { anchor = 0; caret = n; return true; }
    if (ctrl && (vk == 'C' || vk == 'X') && HasSelection()) {
        int a = std::min(caret, anchor), b = std::max(caret, anchor);
        shell::CopyText(h, text.substr(a, b - a));
        if (vk == 'X') del();
        return true;
    }
    if (ctrl && vk == 'V') {
        std::wstring s;
        if (ClipboardText(h, s)) {
            s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t ch) { return ch < 32; }), s.end());
            if (HasSelection()) del();
            text.insert(caret, s);
            caret += (int)s.size();
            anchor = caret;
        }
        return true;
    }
    return false;
}

bool TextField::Char(wchar_t ch) {
    if (ch < 32 || ch == 127) return false;
    if (HasSelection()) {
        int a = std::min(caret, anchor), b = std::max(caret, anchor);
        text.erase(a, b - a);
        caret = anchor = a;
    }
    if (text.size() >= 255) return true;
    text.insert(text.begin() + caret, ch);
    anchor = ++caret;
    return true;
}

// ---------------------------------------------------------------- window procedure
static void SaveWindowPlacement() {
    if (fullscreen) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd, &wp)) return;
    g_cfg.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    RECT r = wp.rcNormalPosition;
    g_cfg.windowX = r.left;
    g_cfg.windowY = r.top;
    g_cfg.windowW = r.right - r.left;
    g_cfg.windowH = r.bottom - r.top;
}

static void Shutdown() {
    SaveWindowPlacement();
    ConfigSave();
    loader::Shutdown();
    WatchHangar(false);
}

static void SignalHealthyOnce() {
    if (s_healthySent || !s_after.active) return;
    s_healthySent = true;
    updater::SignalHealthy(s_after);
    updater::FinishAfterUpdate(s_after, s_after.background);
    if (s_after.background) PostMessageW(hwnd, WM_LOUPE_QUIT, 0, 0);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            hwnd = h;
            scale = GetDpiForWindow(h) / 96.0f;
            customThemes = LoadCustomThemes();
            ApplyTheme();
            DragAcceptFiles(h, TRUE);
            return 0;
        case WM_NCCALCSIZE:
            if (wp && !fullscreen) {
                // no caption: the client area covers the title bar; keep the side and bottom borders
                auto* p = (NCCALCSIZE_PARAMS*)lp;
                LONG top = p->rgrc[0].top;
                DefWindowProcW(h, msg, wp, lp);
                p->rgrc[0].top = top;
                if (IsZoomed(h)) {
                    UINT dpi = GetDpiForWindow(h);
                    p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            LRESULT r = DefWindowProcW(h, msg, wp, lp);
            if (r != HTCLIENT) return r;
            return NcHitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        }
        case WM_NCMOUSEMOVE: {
            bool hot = wp == HTMAXBUTTON;
            if (hot != s_maxHot) { s_maxHot = hot; Invalidate(); }
            s_lastMove = GetTickCount();
            if (chromeAlpha < 1) StartAnim();
            if (hot) {
                // track leaving the button (non-client)
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE | TME_NONCLIENT, h, 0};
                TrackMouseEvent(&t);
                return 0;
            }
            break;
        }
        case WM_NCMOUSELEAVE:
            if (s_maxHot || s_maxDown) { s_maxHot = s_maxDown = false; Invalidate(); }
            break;
        case WM_NCLBUTTONDOWN:
            if (wp == HTMAXBUTTON) { s_maxDown = true; Invalidate(); return 0; }
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON) {
                if (s_maxDown) ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
                s_maxDown = false;
                Invalidate();
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            if (EnsureDevice()) {
                Paint();
                if (rt && !s_healthySent && (HasImage() || path.empty() || (Current() && Current()->failed()))) SetTimer(h, TIMER_HEALTHY, 50, nullptr);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: {
            if (rt) {
                if (rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp))) == (HRESULT)D2DERR_RECREATE_TARGET) DiscardDevice();
            }
            if (wp == SIZE_MINIMIZED) {
                gfx::ClearTextCaches();
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
                return 0;
            }
            W = LOWORD(lp) / scale;
            H = HIWORD(lp) / scale;
            LayoutViewport();
            if (fitMode) { Entry* e = Current(); zoom = zoomTarget = FitZoom(e, viewRect); if (e) { cx = cxTarget = e->fullW / 2.0f; cy = cyTarget = e->fullH / 2.0f; } }
            else { Entry* e = Current(); ClampPan(cxTarget, cyTarget, zoomTarget, e, viewRect); cx = cxTarget; cy = cyTarget; }
            Invalidate();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            float s = GetDpiForWindow(h) / 96.0f;
            mm->ptMinTrackSize.x = (LONG)(480 * s);
            mm->ptMinTrackSize.y = (LONG)(320 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            scale = HIWORD(wp) / 96.0f;
            RECT* r = (RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (rt) rt->SetDpi((float)HIWORD(wp), (float)HIWORD(wp));
            ComputePreviewSize(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
            gfx::ClearTextCaches();
            Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) { InvalidateSystemColors(); ApplyTheme(); }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            InvalidateSystemColors();
            ApplyTheme();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                if (fullscreen && chromeAlpha < 0.05f && GetTickCount() - s_lastMove > 1800) { SetCursor(nullptr); return TRUE; }
                LPCWSTR cur = IDC_ARROW;
                if (s_panning) cur = IDC_SIZEALL;
                else if (hover.kind == Hit::CropHandle) {
                    int a = hover.arg;
                    cur = a == 8 ? IDC_SIZEALL : (a == 0 || a == 4) ? IDC_SIZENWSE : (a == 2 || a == 6) ? IDC_SIZENESW : (a == 1 || a == 5) ? IDC_SIZENS : IDC_SIZEWE;
                } else if (hover.kind == Hit::Viewport && CanPan()) cur = IDC_HAND;
                else if (hover.kind == Hit::AccentField || hover.kind == Hit::DialogField) cur = IDC_IBEAM;
                else if (hover.kind != Hit::None && hover.kind != Hit::Viewport && hover.kind != Hit::Caption && hover.kind != Hit::FilmArea &&
                         hover.kind != Hit::GridArea && hover.kind != Hit::Pane && hover.enabled) cur = IDC_HAND;
                SetCursor(LoadCursorW(nullptr, cur));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            POINT now{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (now.x != s_lastMousePt.x || now.y != s_lastMousePt.y) {
                s_lastMove = GetTickCount();
                s_lastMousePt = now;
                if (chromeAlpha < 1) StartAnim();
                SetTimer(h, TIMER_IDLE, 250, nullptr);
            }
            if (!s_tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                s_tracking = true;
            }
            if (s_maxHot) { s_maxHot = false; Invalidate(); }
            if (s_panning) { PanBy(x - s_panX, y - s_panY); s_panX = x; s_panY = y; return 0; }
            if (s_dragSlider || s_dragCrop) { EditDrag(x, y); Invalidate(); return 0; }
            if (s_dragFilm) {
                if (std::fabs(x - s_filmDragX) > 4) s_filmDragged = true;
                filmScroll = filmScrollTarget = s_filmDragStart - (x - s_filmDragX);
                Invalidate();
                return 0;
            }
            if (s_dragScroll) {
                float trackH = H - kTopBar - 8;
                if (gridMode) {
                    float maxS = std::max(1.0f, gridContentH - (H - kTopBar));
                    gridScroll = gridScrollTarget = std::clamp((y - kTopBar) / trackH * gridContentH - (H - kTopBar) / 2, 0.0f, maxS);
                } else if (settingsOpen) {
                    float maxS = std::max(1.0f, settingsContentH - (H - kTopBar));
                    settingsScroll = settingsScrollTarget = std::clamp((y - kTopBar) / trackH * settingsContentH - (H - kTopBar) / 2, 0.0f, maxS);
                }
                Invalidate();
                return 0;
            }
            HotRect hh = HitAt(x, y);
            if (!SameHit(hh, hover)) { hover = hh; Invalidate(); }
            return 0;
        }
        case WM_MOUSELEAVE:
            s_tracking = false;
            hover = HotRect{{0, 0, 0, 0}, Hit::None, 0, true, nullptr};
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN: {
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            SetCapture(h);
            s_lastMove = GetTickCount();
            HotRect hh = HitAt(x, y);
            hover = hh;
            if (dialog.open && hh.kind != Hit::DialogButton && hh.kind != Hit::DialogField) { pressed = hh; return 0; }
            pressed = hh;
            if (accentField.active && hh.kind != Hit::AccentField) accentField.active = false;
            switch (hh.kind) {
                case Hit::Viewport: case Hit::Pane:
                    if (hh.kind == Hit::Pane) comparePane = hh.arg;
                    if (CanPan()) { s_panning = true; s_panX = x; s_panY = y; SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); }
                    break;
                case Hit::EditSlider: UiPushEdit(); s_dragSlider = true; s_dragArg = hh.arg; EditDrag(x, y); break;
                case Hit::CropHandle: UiPushEdit(); s_dragCrop = true; s_dragArg = hh.arg; EditClick(hh, x, y); break;
                case Hit::Film: case Hit::FilmArea:
                    s_dragFilm = true; s_filmDragged = false; s_filmDragX = x; s_filmDragStart = filmScroll;
                    break;
                case Hit::Scrollbar: s_dragScroll = true; break;
                default: break;
            }
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            ReleaseCapture();
            bool wasDrag = s_panning || s_dragSlider || s_dragCrop || s_dragScroll;
            if (s_dragSlider || s_dragCrop) editDirty = !edit.IsIdentity();
            s_panning = s_dragSlider = s_dragCrop = s_dragScroll = false;
            bool filmDrag = s_dragFilm && s_filmDragged;
            s_dragFilm = false;
            HotRect hh = HitAt(x, y);
            HotRect p = pressed;
            pressed = HotRect{{0, 0, 0, 0}, Hit::None, 0, true, nullptr};
            if (!wasDrag && !filmDrag && SameHit(hh, p) && hh.enabled && hh.kind != Hit::None) Click(hh, x, y);
            s_filmDragged = false;
            if (wasDrag) CheckFullNeeded();
            Invalidate();
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            HotRect hh = HitAt(x, y);
            if (hh.kind == Hit::Viewport && !editing && HasImage()) {
                Entry* e = Current();
                float fit = FitZoom(e, viewRect);
                if (fitMode || std::fabs(zoomTarget - fit) < fit * 0.01f) ZoomTo(std::max(1.0f / scale, fit * 2.0f), x, y, true);
                else ZoomFit();
            } else if (hh.kind == Hit::Pane) {
                s_cmpMul = s_cmpMul > 1.01f ? 1.0f : 2.0f;
                Invalidate();
            } else if (hh.kind == Hit::GridCell) {
                OpenFromGrid(hh.arg);
            } else if (hh.kind == Hit::EditSlider) {
                UiPushEdit();
                SetEditValue(hh.arg, 0);
                editDirty = !edit.IsIdentity();
                Invalidate();
            } else if (hh.kind == Hit::Caption && fullscreen) {
                SetFullscreen(false);
            } else if (hh.kind == Hit::Film) {
                // single click already opened it
            } else if (hh.kind == Hit::None || hh.kind == Hit::Viewport) {
                if (!HasImage()) DoCmd(Cmd::Open);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            HotRect hh = HitAt(x, y);
            if (dialog.open || settingsOpen) return 0;
            if (hh.kind == Hit::GridCell) {
                index = hh.arg;
                path = PathAt(hh.arg);
                EnsureEntry(path, 0);
                Invalidate();
            }
            if (hh.kind == Hit::Viewport || hh.kind == Hit::GridCell || hh.kind == Hit::Pane || hh.kind == Hit::Film || hh.kind == Hit::None)
                ShowMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
            (void)x; (void)y;
            return 0;
        }
        case WM_MBUTTONUP:
            if (!editing && HasImage()) DoCmd(fitMode ? Cmd::Zoom100 : Cmd::ZoomFit);
            return 0;
        case WM_XBUTTONUP:
            DoCmd(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? Cmd::Prev : Cmd::Next);
            return TRUE;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp), p.x / scale, p.y / scale, msg == WM_MOUSEHWHEEL);
            return 0;
        }
        case WM_NCRBUTTONUP:
            if (wp == HTCAPTION) { ShowSystemMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0; }
            break;
        case WM_SYSCHAR:
            if (wp == L' ') return 0;   // no beep: Alt+Space is handled on key down
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && wp == VK_RETURN) { DoCmd(Cmd::Fullscreen); return 0; }
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
            if (msg == WM_SYSKEYDOWN && wp == VK_SPACE) {
                RECT wr;
                GetWindowRect(h, &wr);
                ShowSystemMenu(wr.left + 8, wr.top + (int)(kTopBar * scale));
                return 0;
            }
            if (msg == WM_SYSKEYDOWN && wp != VK_F10) break;
            OnKey(wp, (lp & (1 << 30)) != 0);
            return 0;
        case WM_CHAR:
            if (dialog.open && dialog.field) { if (dialog.input.Char((wchar_t)wp)) { dialog.fieldError.clear(); Invalidate(); } return 0; }
            if (settingsOpen && accentField.active) { if (accentField.Char((wchar_t)wp)) Invalidate(); return 0; }
            return 0;
        case WM_TIMER:
            switch (wp) {
                case TIMER_ANIM: Animate(); break;
                case TIMER_IDLE:
                    if (!chromePinned() && chromeAlpha > 0 && GetTickCount() - s_lastMove > 1800) StartAnim();
                    if (!toastText.empty() && GetTickCount() >= toastUntil) StartAnim();
                    break;
                case TIMER_FIRSTCHECK:
                    KillTimer(h, TIMER_FIRSTCHECK);
                    if (g_cfg.autoUpdate && (time(nullptr) - g_cfg.lastUpdateCheck >= 3600)) updater::Check(h, false);
                    break;
                case TIMER_UPDATE:
                    if (g_cfg.autoUpdate && update.kind != UpdateState::Ready) updater::Check(h, false);
                    break;
                case TIMER_SLIDE:
                    if (slideshow && !s_slidePaused && !dialog.open) {
                        Entry* e = Current();
                        // let an animation play through once
                        if (e && e->animated && !paused && s_loopsDone < 1) break;
                        Step(1);
                    }
                    break;
                case TIMER_FULL:
                    KillTimer(h, TIMER_FULL);
                    if (Entry* e = Current(); e && e->ready() && (uint64_t)e->fullW * e->fullH <= 48ull * 1000 * 1000) MaybeRequestFull(e, 8);
                    break;
                case TIMER_SMOKE: SmokeTick(); break;
                case TIMER_HEALTHY: KillTimer(h, TIMER_HEALTHY); SignalHealthyOnce(); break;
            }
            return 0;
        case WM_LOUPE_DECODED: { auto* r = (DecodeResult*)lp; OnDecoded(r); delete r; return 0; }
        case WM_LOUPE_THUMB: { auto* r = (ThumbResult*)lp; OnThumb(r); delete r; return 0; }
        case WM_LOUPE_FOLDER: { auto* r = (FolderResult*)lp; OnFolder(r); delete r; return 0; }
        case WM_LOUPE_DATES: { auto* r = (DatesResult*)lp; OnDates(r); delete r; return 0; }
        case WM_LOUPE_INFO: {
            auto* r = (InfoResult*)lp;
            infoPending.erase(r->path);
            infos[r->path] = r->ok ? r->info : PhotoInfo{};
            delete r;
            Invalidate();
            return 0;
        }
        case WM_LOUPE_UPDATE: { auto* s = (UpdateState*)lp; OnUpdate(s); delete s; return 0; }
        case WM_LOUPE_TASK_DONE: { auto* t = (TaskDone*)lp; OnTask(t); delete t; return 0; }
        case WM_LOUPE_FOLDER_CHANGED:
            if (!folder.empty()) { ++s_folderGen; loader::ListFolder(s_folderGen, folder); }
            return 0;
        case WM_LOUPE_HANGAR_CHANGED:
            if (g_cfg.theme == L"hangar") { customThemes = LoadCustomThemes(); ApplyTheme(); }
            return 0;
        case WM_LOUPE_QUIT:
            s_quitting = true;
            DestroyWindow(h);
            return 0;
        case WM_COPYDATA: {
            // another Loupe.exe handed us a file to open
            auto* cd = (COPYDATASTRUCT*)lp;
            if (cd->dwData == 0x4C4F5550 /*LOUP*/ && cd->cbData >= sizeof(wchar_t)) {
                std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
                while (!p.empty() && p.back() == 0) p.pop_back();
                if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
                SetForegroundWindow(h);
                if (!p.empty()) {
                    std::wstring target = p;
                    ConfirmLeaveEditing([target] { gridMode = false; compareMode = false; settingsOpen = false; OpenPath(target); });
                }
            }
            return TRUE;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            wchar_t buf[MAX_PATH * 4];
            std::wstring first;
            if (DragQueryFileW(drop, 0, buf, MAX_PATH * 4)) first = buf;
            DragFinish(drop);
            if (first.empty()) return 0;
            if (DirExists(first)) {
                // a folder: open its first image
                WIN32_FIND_DATAW fd;
                HANDLE f = FindFirstFileW((first + L"\\*").c_str(), &fd);
                std::wstring best;
                if (f != INVALID_HANDLE_VALUE) {
                    do {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        if (!IsSupportedExtension(ExtOf(fd.cFileName))) continue;
                        if (best.empty() || StrCmpLogicalW(fd.cFileName, best.c_str()) < 0) best = fd.cFileName;
                    } while (FindNextFileW(f, &fd));
                    FindClose(f);
                }
                if (best.empty()) { Toast(L"No images in that folder"); return 0; }
                first = first + L"\\" + best;
            } else if (!IsSupportedExtension(ExtOf(first))) {
                Toast(L"Loupe doesn't open " + ExtOf(first) + L" files");
                return 0;
            }
            std::wstring target = first;
            ConfirmLeaveEditing([target] { gridMode = false; compareMode = false; settingsOpen = false; OpenPath(target); });
            SetForegroundWindow(h);
            return 0;
        }
        case WM_ACTIVATE:
            Invalidate();
            break;
        case WM_CLOSE:
            if (editing && editDirty && !s_quitting) {
                ShowDialog(L"Close without saving?", L"Your edit isn't saved.", {L"Close", L"Keep editing"}, 1, 1, [](int b) {
                    if (b == 0) { editDirty = false; s_quitting = true; DestroyWindow(hwnd); }
                }, true);
                return 0;
            }
            s_quitting = true;
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            Shutdown();
            // a downloaded update installs quietly on the way out (and rolls back if it won't start)
            bool install = g_cfg.autoUpdate && update.kind == UpdateState::Ready && !s_after.background && !SmokeActive();
            ShowWindow(h, SW_HIDE);
            if (install) {
                std::wstring err;
                if (!updater::Apply(nullptr, update.staged, update.version, L"", true, &err)) { SkipVersion(update.version); ConfigSave(); }
            }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND UiCreate(HINSTANCE inst, int show, const std::wstring& initialPath, const updater::AfterUpdate& after, const std::wstring& smokeLog,
              const std::wstring& smokeFolder) {
    s_after = after;
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_startQpc);
    if (!gfx::Init()) return nullptr;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = LOUPE_WINDOW_CLASS;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    float s = dpi / 96.0f;
    int w = g_cfg.windowW > 0 ? g_cfg.windowW : (int)(1200 * s);
    int hgt = g_cfg.windowH > 0 ? g_cfg.windowH : (int)(800 * s);
    int x = g_cfg.windowX, y = g_cfg.windowY;
    if (x != CW_USEDEFAULT) {
        RECT r{x, y, x + w, y + hgt};
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
    }
    HWND h = CreateWindowExW(WS_EX_APPWINDOW, LOUPE_WINDOW_CLASS, L"Loupe", WS_OVERLAPPEDWINDOW, x, y, w, hgt, nullptr, nullptr, inst, nullptr);
    if (!h) return nullptr;
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    ComputePreviewSize(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY));
    loader::Init(h);
    EnsureDevice();
    PruneSkippedVersions(LOUPE_VERSION);
    if (!initialPath.empty()) OpenPath(initialPath);
    else welcome = true;
    bool maximize = g_cfg.maximized && (show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT || show == SW_SHOW);
    if (after.background) show = SW_HIDE;
    else if (!smokeLog.empty()) { show = SW_SHOWNORMAL; maximize = false; }
    ShowWindow(h, maximize ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(h);
    SetTimer(h, TIMER_IDLE, 250, nullptr);
    if (after.background) {
        // installed on exit: just prove we start
        Paint();
        SignalHealthyOnce();
        return h;
    }
    if (!smokeLog.empty()) {
        SmokeStart(smokeLog, smokeFolder);
        SetTimer(h, TIMER_SMOKE, 100, nullptr);
        return h;
    }
    SetTimer(h, TIMER_FIRSTCHECK, 10 * 1000, nullptr);
    SetTimer(h, TIMER_UPDATE, 3 * 3600 * 1000, nullptr);
    if (after.active && CompareVersions(after.fromVersion, LOUPE_VERSION) < 0) Toast(std::wstring(L"Updated to Loupe ") + LOUPE_VERSION);
    else if (after.active) Toast(std::wstring(L"Rolled back to Loupe ") + LOUPE_VERSION);
    // housekeeping, off the UI thread
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (!IsPortable() && (!IEquals(g_cfg.registeredExe, ExePath()) || !shell::FileTypesRegistered())) {
            shell::RegisterFileTypes();
            PostMessageW(hwnd, WM_NULL, 0, 0);
        }
        // backups of replaced originals are kept a week
        std::wstring dir = DataDir() + L"\\backup";
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER n{{now.dwLowDateTime, now.dwHighDateTime}};
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ULARGE_INTEGER t{{fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}};
                ULARGE_INTEGER c{{fd.ftCreationTime.dwLowDateTime, fd.ftCreationTime.dwHighDateTime}};
                ULARGE_INTEGER newest = t.QuadPart > c.QuadPart ? t : c;
                if (n.QuadPart > newest.QuadPart && n.QuadPart - newest.QuadPart > 7ull * 24 * 3600 * 10000000ull) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        CoUninitialize();
    }).detach();
    if (!IsPortable()) g_cfg.registeredExe = ExePath();
    return h;
}

// smoke test hooks
double UiFirstPixelsMs() { return s_firstPixelsMs; }
void UiTestSaveCopy(const std::wstring& target) { SaveEdited(false, target, L"jpg"); }
void UiTestDialogPress(int b) { CloseDialog(b); }
void UiTestPanBy(float dx, float dy) { PanBy(dx, dy); }

