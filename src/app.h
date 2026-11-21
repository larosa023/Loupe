// Loupe - state shared by the window code (ui.cpp), drawing (draw.cpp) and pages (pages.cpp).
// Everything here lives on the UI thread.
#pragma once
#include "common.h"
#include "gfx.h"
#include "themes.h"
#include "config.h"
#include "loader.h"
#include "edit.h"
#include "updater.h"
#include <d2d1_1.h>
#include <memory>
#include <map>
#include <functional>

// ---------------------------------------------------------------- images on the GPU
struct GpuImage {
    struct Tile { ID2D1Bitmap* bmp = nullptr; UINT x = 0, y = 0, w = 0, h = 0; };
    std::vector<Tile> tiles;
    UINT w = 0, h = 0;                      // pixel size of the whole image
    bool empty() const { return tiles.empty(); }
    void Release();
};

// One image in the cache: what we know about it, and its decoded frames
struct Entry {
    uint64_t id = 0;
    std::wstring path;
    std::wstring format;
    UINT fullW = 0, fullH = 0;              // real size after EXIF orientation
    bool hasAlpha = false, animated = false, vector = false;
    int loops = 0;
    std::wstring error, codecName, codecStoreId;
    bool viewPending = false, viewReady = false;
    bool viewIsFull = false;                // the screen-sized decode was already full resolution
    std::vector<GpuImage> frames;           // screen-sized (or full when small)
    std::vector<UINT> delays;
    bool fullPending = false, fullReady = false;
    GpuImage full;                          // still images: full resolution, decoded on demand
    double decodeMs = 0;
    DWORD lastUse = 0;
    int64_t stamp = 0;                      // the file's write time when decoded
    bool failed() const { return viewReady && frames.empty(); }
    bool ready() const { return !frames.empty(); }
    void Release() { for (auto& f : frames) f.Release(); frames.clear(); full.Release(); fullReady = false; }
};

struct Thumb {
    ID2D1Bitmap* bmp = nullptr;
    UINT w = 0, h = 0, size = 0;
    bool pending = false, failed = false;
    DWORD lastUse = 0;
};

// ---------------------------------------------------------------- hit testing
enum class Hit {
    None, Caption, MinBtn, MaxBtn, CloseBtn, Tool, NavPrev, NavNext, Film, FilmArea, GridCell, GridArea, Viewport, Pane,
    PanelClose, InfoMaps, InfoFolder, CodecLink, Welcome, EditAspect, EditPortrait, EditSlider, EditButton, CropHandle,
    SetTheme, SetAccent, SetSeg, SetToggle, SetButton, SetStep, AccentField, SetBack, SetScroll,
    DialogButton, DialogField, ToastAction, UpdateRestart, UpdateDismiss, SlideControl, Shortcuts, SortMenu, Scrollbar,
};

struct HotRect {
    D2D1_RECT_F r;
    Hit kind;
    int arg;
    bool enabled;
    const wchar_t* tip;
};

// ---------------------------------------------------------------- commands
enum class Cmd {
    None, Prev, Next, First, Last, ZoomIn, ZoomOut, ZoomFit, Zoom100, Filmstrip, Grid, Info, Edit, RotateLeft, RotateRight,
    FlipH, FlipV, Compare, Slideshow, Fullscreen, CopyImage, CopyPath, Explorer, Strata, OpenWith, Wallpaper, Delete,
    Rename, Open, Settings, Save, SaveCopy, Revert, Undo, PlayPause, FrameNext, FramePrev, More, Close, Shortcuts,
    SortName, SortTaken, SortModified, SortSize, SortReverse, EditDone, EditCancel, Escape, Print,
};

// ---------------------------------------------------------------- text field (rename, accent hex)
struct TextField {
    std::wstring text;
    int caret = 0, anchor = 0;
    bool active = false;
    void SetText(const std::wstring& s, int selStart, int selEnd) { text = s; anchor = selStart; caret = selEnd; }
    bool HasSelection() const { return caret != anchor; }
    bool Key(WPARAM vk, bool ctrl, bool shift, HWND hwnd);   // true if handled
    bool Char(wchar_t c);
};

struct Dialog {
    bool open = false;
    std::wstring title, body;
    std::vector<std::wstring> buttons;      // left to right
    int primary = -1;                       // accent button
    int cancel = -1;                        // Esc
    bool danger = false;
    bool field = false;
    TextField input;
    std::wstring fieldError;
    std::function<void(int)> onClose;       // button index (or the cancel index)
};

// ---------------------------------------------------------------- app state
namespace app {
using namespace gfx;

extern HWND hwnd;
extern float scale, W, H;                   // DPI scale; client size in DIPs
extern Theme t;
extern std::vector<ThemeDef> customThemes;
extern ID2D1HwndRenderTarget* rt;
extern ID2D1DeviceContext* dc;              // the same target, for effects and high-quality scaling
extern ID2D1SolidColorBrush* br;
extern Canvas c;
extern UINT maxBitmap;

// folder and current image
extern std::wstring folder;
extern std::vector<FileEntry> files;
extern int index;                           // into files; -1 until the folder is listed
extern std::wstring path;                   // the image being shown
extern bool datesLoaded;

// cache
extern std::vector<std::unique_ptr<Entry>> cache;
Entry* Current();
Entry* FindEntry(const std::wstring& path);
Entry* EnsureEntry(const std::wstring& path, int priority);

// view
extern float zoom, zoomTarget, cx, cy, cxTarget, cyTarget;
extern bool fitMode;
extern int frameIndex;
extern bool paused;
extern D2D1_RECT_F viewRect;                // where the image is drawn (DIPs)

// modes
extern bool gridMode, compareMode, slideshow, fullscreen, editing, settingsOpen, shortcutsOpen, welcome;
extern int comparePane;                     // 0 left, 1 right (active)
extern std::wstring comparePath[2];
extern EditParams edit;
extern bool editDirty;

// chrome
extern float chromeAlpha;                   // top bar / arrows (auto-hides)
extern bool chromePinned();
extern std::vector<HotRect> hot;
extern HotRect hover, pressed;
extern float gridScroll, gridScrollTarget, settingsScroll, settingsScrollTarget, filmScroll, filmScrollTarget;
extern float settingsContentH, gridContentH;
extern Dialog dialog;
extern TextField accentField;
extern UpdateState update;
extern bool updateDismissed;
extern std::map<std::wstring, Thumb> thumbs;
extern std::map<std::wstring, PhotoInfo> infos;
extern std::map<std::wstring, bool> infoPending;
extern std::wstring toastText, toastAction;
extern float toastAlpha;
extern DWORD toastUntil;
extern std::vector<int> gridMarks;          // up to two images picked for Compare in the grid
extern std::vector<updater::Previous> prevVersions;

// layout constants (DIPs)
constexpr float kTopBar = 44, kFilm = 86, kPanel = 336, kCaptionBtn = 46;

// ---- ui.cpp
void Invalidate();
void StartAnim();
void DoCmd(Cmd c);
void Click(const HotRect& h, float x, float y);
void Toast(const std::wstring& text, const std::wstring& action = L"");
void ShowDialog(const std::wstring& title, const std::wstring& body, std::vector<std::wstring> buttons, int primary, int cancel,
                std::function<void(int)> onClose, bool danger = false);
void OpenPath(const std::wstring& p, bool keepFolder = false);
void GoTo(int idx);
void ApplyTheme();
void SaveSettings();
D2D1_RECT_F LayoutViewport();
float FitZoom(const Entry* e, const D2D1_RECT_F& r);
void ZoomTo(float z, float px, float py, bool animate);
void ResetView();
int ThumbPixels();
std::wstring CurrentName();
bool HasImage();
void RequestThumb(const std::wstring& path);
ID2D1Bitmap* ThumbBitmap(const std::wstring& path, UINT* w, UINT* h);

// ---- draw.cpp
void Paint();
void GridGeometry(float& left, float& top, float& cell, float& size, int& cols);
void AddHot(const D2D1_RECT_F& r, Hit kind, int arg = 0, bool enabled = true, const wchar_t* tip = nullptr);
bool IsHover(Hit kind, int arg = 0);
bool IsPressed(Hit kind, int arg = 0);
enum class Btn { Primary, Secondary, Subtle, Danger, Chrome };
float ButtonWidth(const std::wstring& label, wchar_t glyph);
void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph, Btn style, Hit kind, int arg = 0, bool enabled = true, const wchar_t* tip = nullptr);
void Segmented(float x, float y, const std::vector<std::wstring>& opts, int sel, Hit kind, int base);
void Toggle(float x, float y, bool on, Hit kind, int arg);
void Slider(const D2D1_RECT_F& r, float value, float minV, float maxV, Hit kind, int arg, bool centered);
void DrawTextField(const D2D1_RECT_F& r, TextField& f, Hit kind, const std::wstring& placeholder);
void DrawImage(Entry* e, const D2D1_RECT_F& dest, float opacity, bool allowEffects);
void DrawCaptionButtons(float right, float top, float h);
void DrawScrollbar(const D2D1_RECT_F& track, float scroll, float contentH, int arg);
std::wstring ZoomText();
D2D1_COLOR_F ImageBackground();

// ---- pages.cpp
void DrawSettings();
void SettingsClick(const HotRect& h, float x, float y);
void DrawInfoPanel(const D2D1_RECT_F& r);
void DrawEditPanel(const D2D1_RECT_F& r);
void EditClick(const HotRect& h, float x, float y);
void EditDrag(float x, float y);
void DrawCropOverlay(Entry* e);
void DrawShortcuts();
void DrawWelcome();
void SetEditValue(int slider, float value);
float EditSliderValue(int slider, float* minV, float* maxV, bool* centered);
D2D1_RECT_F EditImageRect(Entry* e);   // where the oriented, straightened image sits while editing
D2D1_MATRIX_3X2_F EditDisplayMatrix(Entry* e);

} // namespace app
