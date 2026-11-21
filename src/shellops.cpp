// Loupe - Windows shell work (see shellops.h)
#include "shellops.h"
#include "image.h"
#include "util.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <shlwapi.h>
#include <algorithm>

namespace shell {

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------- clipboard
static HGLOBAL GlobalFrom(const void* data, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return nullptr;
    if (void* p = GlobalLock(g)) { memcpy(p, data, n); GlobalUnlock(g); }
    return g;
}

bool CopyImage(HWND owner, const Pixels& p, const std::wstring& path) {
    if (p.empty()) return false;
    // CF_DIBV5: bottom-up, straight alpha
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = (LONG)p.w;
    bh.bV5Height = (LONG)p.h;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5SizeImage = p.w * p.h * 4;
    bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    bh.bV5CSType = LCS_sRGB;
    bh.bV5Intent = LCS_GM_IMAGES;
    std::vector<uint8_t> dib(sizeof(bh) + bh.bV5SizeImage);
    memcpy(dib.data(), &bh, sizeof(bh));
    uint8_t* dst = dib.data() + sizeof(bh);
    for (UINT y = 0; y < p.h; ++y) {
        const uint8_t* s = &p.px[(size_t)(p.h - 1 - y) * p.w * 4];
        uint8_t* d = dst + (size_t)y * p.w * 4;
        for (UINT x = 0; x < p.w; ++x, s += 4, d += 4) {
            uint8_t a = s[3];
            if (a == 255 || a == 0) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
            else { d[0] = (uint8_t)std::min(255, s[0] * 255 / a); d[1] = (uint8_t)std::min(255, s[1] * 255 / a); d[2] = (uint8_t)std::min(255, s[2] * 255 / a); }
            d[3] = a;
        }
    }
    std::string png;
    EncodePngToMemory(p, png);
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    if (HGLOBAL g = GlobalFrom(dib.data(), dib.size())) if (!SetClipboardData(CF_DIBV5, g)) GlobalFree(g);
    if (!png.empty()) {
        UINT fmt = RegisterClipboardFormatW(L"PNG");
        if (HGLOBAL g = GlobalFrom(png.data(), png.size())) if (!SetClipboardData(fmt, g)) GlobalFree(g);
    }
    if (!path.empty()) {
        // the file too, so pasting into Explorer, Discord or a mail copies the original
        std::vector<uint8_t> drop(sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t), 0);
        auto* df = (DROPFILES*)drop.data();
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(drop.data() + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
        if (HGLOBAL g = GlobalFrom(drop.data(), drop.size())) if (!SetClipboardData(CF_HDROP, g)) GlobalFree(g);
    }
    CloseClipboard();
    return true;
}

bool CopyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL g = GlobalFrom(text.c_str(), (text.size() + 1) * sizeof(wchar_t))) {
        ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
        if (!ok) GlobalFree(g);
    }
    CloseClipboard();
    return ok;
}

// ---------------------------------------------------------------- Explorer, Strata, Open with
void ShowInExplorer(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
    } else {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }
}

std::wstring StrataExe() {
    std::wstring p = KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\Strata\\Strata.exe";
    return FileExists(p) ? p : L"";
}

bool OpenInStrata(const std::wstring& folder) {
    std::wstring exe = StrataExe();
    if (exe.empty()) return false;
    return LaunchDetached(exe, L"\"" + folder + L"\"");
}

void OpenWith(HWND owner, const std::wstring& path) {
    OPENASINFO oi{};
    oi.pcszFile = path.c_str();
    oi.oaifInFlags = OAIF_EXEC | OAIF_HIDE_REGISTRATION;
    SHOpenWithDialog(owner, &oi);
}

void OpenUrl(const std::wstring& url) { ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ---------------------------------------------------------------- Recycle Bin
bool Recycle(const std::wstring& path, std::wstring* error) {
    IFileOperation* op = nullptr;
    IShellItem* item = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) &&
        SUCCEEDED(op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE)) &&
        SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
        SUCCEEDED(op->DeleteItem(item, nullptr))) {
        HRESULT hr = op->PerformOperations();
        BOOL aborted = FALSE;
        op->GetAnyOperationsAborted(&aborted);
        ok = SUCCEEDED(hr) && !aborted && !FileExists(path);
        if (!ok && error) *error = aborted ? L"Deleting was cancelled." : L"Couldn't move it to the Recycle Bin. Is it open in another app?";
    } else if (error) {
        *error = L"Couldn't move it to the Recycle Bin.";
    }
    Rel(item);
    Rel(op);
    return ok;
}

bool RestoreFromRecycleBin(const std::wstring& original, FILETIME) {
    std::wstring dir = PathParent(original), name = PathName(original);
    std::wstring stem = name.substr(0, name.find_last_of(L'.'));
    IShellFolder* desktop = nullptr;
    IShellFolder2* bin = nullptr;
    PIDLIST_ABSOLUTE binPidl = nullptr;
    bool ok = false;
    if (FAILED(SHGetDesktopFolder(&desktop)) || FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &binPidl)) ||
        FAILED(desktop->BindToObject(binPidl, nullptr, IID_PPV_ARGS(&bin)))) {
        Rel(desktop);
        if (binPidl) CoTaskMemFree(binPidl);
        return false;
    }
    IEnumIDList* en = nullptr;
    PITEMID_CHILD best = nullptr;
    double bestDate = -1;
    const PROPERTYKEY deletedFrom = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 2};
    const PROPERTYKEY dateDeleted = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 3};
    if (SUCCEEDED(bin->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en)) && en) {
        PITEMID_CHILD child = nullptr;
        while (en->Next(1, &child, nullptr) == S_OK) {
            bool match = false;
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(bin->GetDetailsEx(child, &deletedFrom, &v)) && v.vt == VT_BSTR && v.bstrVal) {
                std::wstring from = v.bstrVal;
                while (!from.empty() && from.back() == L'\\') from.pop_back();
                std::wstring d2 = dir;
                while (!d2.empty() && d2.back() == L'\\') d2.pop_back();
                if (IEquals(from, d2)) {
                    STRRET sr;
                    wchar_t* disp = nullptr;
                    if (SUCCEEDED(bin->GetDisplayNameOf(child, SHGDN_INFOLDER, &sr)) && SUCCEEDED(StrRetToStrW(&sr, child, &disp)) && disp) {
                        match = IEquals(disp, name) || IEquals(disp, stem);
                        CoTaskMemFree(disp);
                    }
                }
            }
            VariantClear(&v);
            double when = 0;
            if (match) {
                VARIANT d;
                VariantInit(&d);
                if (SUCCEEDED(bin->GetDetailsEx(child, &dateDeleted, &d)) && d.vt == VT_DATE) when = d.date;
                VariantClear(&d);
            }
            if (match && when >= bestDate) {
                if (best) CoTaskMemFree(best);
                best = child;
                bestDate = when;
            } else {
                CoTaskMemFree(child);
            }
        }
        en->Release();
    }
    if (best) {
        IContextMenu* cm = nullptr;
        PCUITEMID_CHILD arr[1] = {best};
        if (SUCCEEDED(bin->GetUIObjectOf(nullptr, 1, arr, IID_IContextMenu, nullptr, (void**)&cm)) && cm) {
            HMENU menu = CreatePopupMenu();
            cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
            CMINVOKECOMMANDINFO ci{sizeof(ci)};
            ci.lpVerb = "undelete";
            ci.fMask = CMIC_MASK_FLAG_NO_UI;
            ci.nShow = SW_HIDE;
            ok = SUCCEEDED(cm->InvokeCommand(&ci));
            DestroyMenu(menu);
            cm->Release();
        }
        CoTaskMemFree(best);
    }
    Rel(bin);
    Rel(desktop);
    CoTaskMemFree(binPidl);
    for (int i = 0; ok && i < 20 && !FileExists(original); ++i) Sleep(50);
    return ok && FileExists(original);
}

// ---------------------------------------------------------------- wallpaper
static const GUID kClsidDesktopWallpaper = {0xC2CF3110, 0x460E, 0x4fc1, {0xB9, 0xD0, 0x8A, 0x1C, 0x0C, 0x9C, 0xC4, 0xBD}};

bool SetWallpaper(const std::wstring& image, std::wstring* error) {
    IDesktopWallpaper* dw = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(kClsidDesktopWallpaper, nullptr, CLSCTX_ALL, __uuidof(IDesktopWallpaper), (void**)&dw)) && dw) {
        ok = SUCCEEDED(dw->SetWallpaper(nullptr, image.c_str()));
        dw->Release();
    }
    if (!ok) ok = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)image.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != 0;
    if (!ok && error) *error = L"Windows didn't accept the wallpaper.";
    return ok;
}

// ---------------------------------------------------------------- dialogs
std::wstring PickImage(HWND owner, const std::wstring& folder) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring result;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    std::wstring spec;
    for (auto& e : SupportedExtensions()) spec += (spec.empty() ? L"*" : L";*") + e;
    COMDLG_FILTERSPEC types[] = {{L"Images", spec.c_str()}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Open image");
    if (!folder.empty()) {
        IShellItem* f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&f)))) { dlg->SetFolder(f); f->Release(); }
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    return result;
}

std::wstring PickSaveAs(HWND owner, const std::wstring& suggested, std::wstring& format) {
    IFileSaveDialog* dlg = nullptr;
    std::wstring result;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    COMDLG_FILTERSPEC types[] = {
        {L"JPEG image", L"*.jpg;*.jpeg"}, {L"PNG image", L"*.png"}, {L"TIFF image", L"*.tif;*.tiff"}, {L"Bitmap", L"*.bmp"},
    };
    static const wchar_t* exts[] = {L"jpg", L"png", L"tif", L"bmp"};
    dlg->SetFileTypes(4, types);
    std::wstring ext = Lower(suggested.substr(suggested.find_last_of(L'.') + 1));
    UINT idx = 1;
    if (ext == L"png") idx = 2; else if (ext == L"tif" || ext == L"tiff") idx = 3; else if (ext == L"bmp") idx = 4;
    dlg->SetFileTypeIndex(idx);
    dlg->SetDefaultExtension(exts[idx - 1]);
    dlg->SetTitle(L"Save a copy");
    dlg->SetOptions(FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_STRICTFILETYPES);
    std::wstring dir = PathParent(suggested);
    IShellItem* f = nullptr;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&f)))) { dlg->SetFolder(f); f->Release(); }
    dlg->SetFileName(PathName(suggested).c_str());
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
            it->Release();
        }
        UINT ti = 1;
        dlg->GetFileTypeIndex(&ti);
        format = exts[std::clamp<UINT>(ti, 1, 4) - 1];
    }
    dlg->Release();
    return result;
}

// ---------------------------------------------------------------- file types
static const wchar_t* kProgId = L"Loupe.Image";
static const wchar_t* kCapabilities = L"Software\\larosa023\\Loupe\\Capabilities";

static void SetStr(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

void RegisterFileTypes() {
    std::wstring exe = ExePath();
    std::wstring cmd = L"\"" + exe + L"\" \"%1\"";
    std::wstring classes = L"Software\\Classes\\";
    SetStr(classes + kProgId, nullptr, L"Image");
    SetStr(classes + kProgId, L"FriendlyTypeName", L"Image");
    SetStr(classes + kProgId + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
    SetStr(classes + kProgId + L"\\shell\\open", L"FriendlyAppName", L"Loupe");
    SetStr(classes + kProgId + L"\\shell\\open\\command", nullptr, cmd);
    std::wstring app = classes + L"Applications\\Loupe.exe";
    SetStr(app, L"FriendlyAppName", L"Loupe");
    SetStr(app + L"\\shell\\open\\command", nullptr, cmd);
    SetStr(kCapabilities, L"ApplicationName", L"Loupe");
    SetStr(kCapabilities, L"ApplicationDescription", L"A fast, modern photo viewer.");
    SetStr(kCapabilities, L"ApplicationIcon", L"\"" + exe + L"\",0");
    for (auto& e : SupportedExtensions()) {
        SetStr(classes + e + L"\\OpenWithProgids", kProgId, L"");
        SetStr(app + L"\\SupportedTypes", e.c_str(), L"");
        SetStr(std::wstring(kCapabilities) + L"\\FileAssociations", e.c_str(), kProgId);
    }
    SetStr(L"Software\\RegisteredApplications", L"Loupe", kCapabilities);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void UnregisterFileTypes() {
    std::wstring classes = L"Software\\Classes\\";
    RegDeleteTreeW(HKEY_CURRENT_USER, (classes + kProgId).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (classes + L"Applications\\Loupe.exe").c_str());
    for (auto& e : SupportedExtensions()) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (classes + e + L"\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, kProgId);
            RegCloseKey(k);
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\larosa023\\Loupe");
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"Loupe");
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool FileTypesRegistered() {
    wchar_t buf[2048];
    DWORD n = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\Loupe.Image\\shell\\open\\command", nullptr, RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return false;
    return IContains(buf, ExePath());
}

void OpenDefaultAppsSettings() {
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Loupe", nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace shell
