// Loupe - Windows shell work: clipboard, Explorer, wallpaper, Recycle Bin, file dialogs, file types.
#pragma once
#include "common.h"
#include "offscreen.h"

namespace shell {

bool CopyImage(HWND owner, const Pixels& p, const std::wstring& path);   // bitmap + PNG + the file itself
bool CopyText(HWND owner, const std::wstring& text);
void ShowInExplorer(const std::wstring& path);
std::wstring StrataExe();                                     // "" when Strata isn't installed
bool OpenInStrata(const std::wstring& folder);
void OpenWith(HWND owner, const std::wstring& path);
void OpenUrl(const std::wstring& url);

// Recycle Bin (call from a worker thread; COM is initialized there)
bool Recycle(const std::wstring& path, std::wstring* error);
bool RestoreFromRecycleBin(const std::wstring& originalPath, FILETIME deletedAfter);

bool SetWallpaper(const std::wstring& imagePath, std::wstring* error);

// File dialogs
std::wstring PickImage(HWND owner, const std::wstring& folder);
// Returns the chosen path; `format` gets "jpg"/"png"/... from the chosen type
std::wstring PickSaveAs(HWND owner, const std::wstring& suggested, std::wstring& format);

// File types: registers Loupe as an "Open with" choice and a Default Apps candidate (per user, no admin)
void RegisterFileTypes();
void UnregisterFileTypes();
bool FileTypesRegistered();
void OpenDefaultAppsSettings();

} // namespace shell
