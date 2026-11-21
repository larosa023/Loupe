// Loupe - themes: the suite's 14 presets, custom theme files, accent colors (same as Hangar).
//
// A theme is a small JSON file in the suite's shared format (Hangar's docs/THEMES.md). Custom themes
// load from Loupe's own themes folder and from Hangar's, so a theme made in Hangar shows up here too.
#pragma once
#include "common.h"
#include "gfx.h"

struct ThemeDef {
    std::wstring id, name, description;
    bool dark = true;
    bool followWindows = false;     // "Windows": light or dark to match Windows
    std::wstring backdrop = L"mica"; // mica | acrylic | none
    float transparency = 0.20f;     // how much of the backdrop shows through the background (0-1)
    uint32_t bg = 0, sidebar = 0, surface = 0, surfaceHover = 0, surfaceStrong = 0;
    uint32_t text = 0, subtext = 0, faint = 0;
    uint32_t accent = 0;            // 0: use the Windows accent color
    float surfaceOpacity = 1.0f;    // < 1 for see-through cards (Glass)
    bool custom = false;
    std::wstring file;
};

const std::vector<ThemeDef>& BuiltinThemes();
std::vector<ThemeDef> LoadCustomThemes();                 // DataDir()\themes\*.json and Hangar's themes folder
std::wstring ThemesDir();
std::wstring HangarDir();                                 // %LOCALAPPDATA%\Hangar
// "Match Hangar": Hangar's current theme id and accent setting, from its settings.json
bool ReadHangarTheme(std::wstring& themeId, std::wstring& accent);
bool ParseThemeJson(const std::wstring& text, ThemeDef& out, std::wstring* error);
std::wstring ThemeToJson(const ThemeDef& t, const std::wstring& accentSetting);

// Finds a theme by id among the built-in and custom ones (falls back to Dark)
ThemeDef FindTheme(const std::wstring& id, const std::vector<ThemeDef>& custom);

// accentSetting: "theme" (the theme's own), "windows", or "#RRGGBB"
gfx::Theme MakeTheme(const ThemeDef& def, const std::wstring& accentSetting);
uint32_t ResolveAccent(const ThemeDef& def, const std::wstring& accentSetting);

extern const uint32_t kAccentSwatches[];
extern const int kAccentSwatchCount;
