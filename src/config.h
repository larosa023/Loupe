// Loupe - settings (DataDir()\settings.json)
#pragma once
#include "common.h"

enum class SortKey { Name, DateTaken, DateModified, Size };
enum class ImageBg { Theme, Black, Gray, Checker };

struct Config {
    // appearance
    std::wstring theme = L"dark";       // built-in theme id, "hangar" (Match Hangar) or custom:<id>
    std::wstring accent = L"theme";     // theme | windows | #RRGGBB
    ImageBg imageBg = ImageBg::Theme;
    bool autoHideChrome = true;
    // viewing
    SortKey sort = SortKey::Name;
    bool sortDescending = false;
    bool wheelZooms = false;            // false: wheel flips images, Ctrl+wheel zooms
    bool enlargeSmall = false;          // fit also enlarges images smaller than the window
    bool loopFolder = true;             // wrap from the last image to the first
    bool filmstrip = false;             // F
    bool infoPanel = false;             // I
    bool singleWindow = true;           // opening another image reuses the open window
    bool smoothZoom = true;
    int slideshowSeconds = 4;
    bool slideshowShuffle = false;
    int gridSize = 160;                 // thumbnail size in the grid (DIPs)
    // files
    bool confirmDelete = false;         // the Recycle Bin + Undo is the safety net
    int jpegQuality = 92;
    // updates
    bool autoUpdate = true;             // check on launch and every few hours, download in the background
    bool beta = false;                  // Beta channel: pre-releases (versions with a "-")
    std::wstring skipVersion;           // versions that failed to start or were rolled back (comma-separated)
    long long lastUpdateCheck = 0;      // unix seconds
    // window
    int windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT, windowW = 0, windowH = 0;
    bool maximized = false;
    std::wstring registeredExe;         // file associations point at this exe
};

extern Config g_cfg;
void ConfigLoad();
void ConfigSave();
bool IsSkippedVersion(const std::wstring& v);
void SkipVersion(const std::wstring& v);
void PruneSkippedVersions(const std::wstring& current);
