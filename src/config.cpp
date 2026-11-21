// Loupe - settings
#include "config.h"
#include "util.h"
#include "json.h"
#include <algorithm>

Config g_cfg;

static std::wstring ConfigPath() { return DataDir() + L"\\settings.json"; }

static const wchar_t* kSortNames[] = {L"name", L"taken", L"modified", L"size"};
static const wchar_t* kBgNames[] = {L"theme", L"black", L"gray", L"checker"};

void ConfigLoad() {
    std::wstring text;
    if (!ReadTextFile(ConfigPath(), text)) return;
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) { Log(L"settings.json is damaged; using defaults"); return; }
    Config c;
    c.theme = v.GetStr(L"theme", c.theme);
    c.accent = v.GetStr(L"accent", c.accent);
    std::wstring bg = v.GetStr(L"imageBackground", L"theme");
    for (int i = 0; i < 4; ++i) if (bg == kBgNames[i]) c.imageBg = (ImageBg)i;
    c.autoHideChrome = v.GetBool(L"autoHideChrome", c.autoHideChrome);
    std::wstring sort = v.GetStr(L"sort", L"name");
    for (int i = 0; i < 4; ++i) if (sort == kSortNames[i]) c.sort = (SortKey)i;
    c.sortDescending = v.GetBool(L"sortDescending", c.sortDescending);
    c.wheelZooms = v.GetBool(L"wheelZooms", c.wheelZooms);
    c.enlargeSmall = v.GetBool(L"enlargeSmall", c.enlargeSmall);
    c.loopFolder = v.GetBool(L"loopFolder", c.loopFolder);
    c.filmstrip = v.GetBool(L"filmstrip", c.filmstrip);
    c.infoPanel = v.GetBool(L"infoPanel", c.infoPanel);
    c.singleWindow = v.GetBool(L"singleWindow", c.singleWindow);
    c.smoothZoom = v.GetBool(L"smoothZoom", c.smoothZoom);
    c.slideshowSeconds = std::clamp((int)v.GetNum(L"slideshowSeconds", c.slideshowSeconds), 1, 60);
    c.slideshowShuffle = v.GetBool(L"slideshowShuffle", c.slideshowShuffle);
    c.gridSize = std::clamp((int)v.GetNum(L"gridSize", c.gridSize), 96, 320);
    c.confirmDelete = v.GetBool(L"confirmDelete", c.confirmDelete);
    c.jpegQuality = std::clamp((int)v.GetNum(L"jpegQuality", c.jpegQuality), 50, 100);
    c.autoUpdate = v.GetBool(L"autoUpdate", c.autoUpdate);
    c.beta = v.GetBool(L"beta", c.beta);
    c.skipVersion = v.GetStr(L"skipVersion");
    c.lastUpdateCheck = (long long)v.GetNum(L"lastUpdateCheck", 0);
    c.registeredExe = v.GetStr(L"registeredExe");
    if (const json::Value* w = v.Get(L"window"); w && w->type == json::Value::Object) {
        c.windowX = (int)w->GetNum(L"x", CW_USEDEFAULT);
        c.windowY = (int)w->GetNum(L"y", CW_USEDEFAULT);
        c.windowW = (int)w->GetNum(L"w", 0);
        c.windowH = (int)w->GetNum(L"h", 0);
        c.maximized = w->GetBool(L"maximized", false);
    }
    g_cfg = c;
}

void ConfigSave() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"theme", json::Value::Str(g_cfg.theme));
    v.Set(L"accent", json::Value::Str(g_cfg.accent));
    v.Set(L"imageBackground", json::Value::Str(kBgNames[(int)g_cfg.imageBg]));
    v.Set(L"autoHideChrome", json::Value::Boolean(g_cfg.autoHideChrome));
    v.Set(L"sort", json::Value::Str(kSortNames[(int)g_cfg.sort]));
    v.Set(L"sortDescending", json::Value::Boolean(g_cfg.sortDescending));
    v.Set(L"wheelZooms", json::Value::Boolean(g_cfg.wheelZooms));
    v.Set(L"enlargeSmall", json::Value::Boolean(g_cfg.enlargeSmall));
    v.Set(L"loopFolder", json::Value::Boolean(g_cfg.loopFolder));
    v.Set(L"filmstrip", json::Value::Boolean(g_cfg.filmstrip));
    v.Set(L"infoPanel", json::Value::Boolean(g_cfg.infoPanel));
    v.Set(L"singleWindow", json::Value::Boolean(g_cfg.singleWindow));
    v.Set(L"smoothZoom", json::Value::Boolean(g_cfg.smoothZoom));
    v.Set(L"slideshowSeconds", json::Value::Num(g_cfg.slideshowSeconds));
    v.Set(L"slideshowShuffle", json::Value::Boolean(g_cfg.slideshowShuffle));
    v.Set(L"gridSize", json::Value::Num(g_cfg.gridSize));
    v.Set(L"confirmDelete", json::Value::Boolean(g_cfg.confirmDelete));
    v.Set(L"jpegQuality", json::Value::Num(g_cfg.jpegQuality));
    v.Set(L"autoUpdate", json::Value::Boolean(g_cfg.autoUpdate));
    v.Set(L"beta", json::Value::Boolean(g_cfg.beta));
    v.Set(L"skipVersion", json::Value::Str(g_cfg.skipVersion));
    v.Set(L"lastUpdateCheck", json::Value::Num((double)g_cfg.lastUpdateCheck));
    v.Set(L"registeredExe", json::Value::Str(g_cfg.registeredExe));
    json::Value w = json::Value::MakeObject();
    w.Set(L"x", json::Value::Num(g_cfg.windowX));
    w.Set(L"y", json::Value::Num(g_cfg.windowY));
    w.Set(L"w", json::Value::Num(g_cfg.windowW));
    w.Set(L"h", json::Value::Num(g_cfg.windowH));
    w.Set(L"maximized", json::Value::Boolean(g_cfg.maximized));
    v.Set(L"window", w);
    std::wstring out;
    json::Write(v, out);
    if (!WriteTextFileAtomic(ConfigPath(), out)) Log(L"couldn't save settings.json");
}

static std::vector<std::wstring> SkipList() {
    std::vector<std::wstring> out;
    size_t i = 0;
    const std::wstring& s = g_cfg.skipVersion;
    while (i < s.size()) {
        size_t e = s.find(L',', i);
        if (e == std::wstring::npos) e = s.size();
        std::wstring v = Trim(s.substr(i, e - i));
        if (!v.empty()) out.push_back(v);
        i = e + 1;
    }
    return out;
}

static void SetSkipList(const std::vector<std::wstring>& list) {
    g_cfg.skipVersion.clear();
    for (auto& v : list) g_cfg.skipVersion += (g_cfg.skipVersion.empty() ? L"" : L",") + v;
}

bool IsSkippedVersion(const std::wstring& v) {
    for (auto& s : SkipList()) if (CompareVersions(s, v) == 0) return true;
    return false;
}

void SkipVersion(const std::wstring& v) {
    if (IsSkippedVersion(v)) return;
    auto list = SkipList();
    list.push_back(NormalizeVersion(v));
    SetSkipList(list);
}

void PruneSkippedVersions(const std::wstring& current) {
    std::vector<std::wstring> keep;
    for (auto& v : SkipList()) if (CompareVersions(v, current) > 0) keep.push_back(v);
    SetSkipList(keep);
}
