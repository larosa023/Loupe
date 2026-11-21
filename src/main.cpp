// Loupe - a fast, modern photo viewer for Windows
//
//   Loupe.exe "photo.jpg"          open an image (in the open window when "same window" is on)
//   Loupe.exe                      open Loupe without an image
//   Loupe.exe --quit               close every Loupe window gracefully (for Hangar)
//   Loupe.exe --uninstall          remove file associations and settings (for Hangar)
//   Loupe.exe --register           register the file types now
//   Loupe.exe --version            exit 0 (smoke tests); --write-version <file> writes the version
//   Loupe.exe --smoke <log> <dir>  automated end-to-end test used by CI
//   Loupe.exe --update-test <log>  update this exe to the latest release (CI)
#include "common.h"
#include "ui.h"
#include "util.h"
#include "config.h"
#include "net.h"
#include "updater.h"
#include "shellops.h"
#include "image.h"
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>

int SmokeExitCode();

static HANDLE g_mutex;
static const wchar_t* kMutexName = L"Local\\Loupe.larosa023.Instance";

void ReleaseInstanceMutex() {
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}
void ReacquireInstanceMutex() {
    if (!g_mutex) g_mutex = CreateMutexW(nullptr, FALSE, kMutexName);
}

static std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, nullptr);
    return n && n < MAX_PATH * 4 ? std::wstring(buf, n) : p;
}

static void CloseAll(DWORD waitMs) {
    std::vector<HWND> wins;
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, LOUPE_WINDOW_CLASS, nullptr)) != nullptr) wins.push_back(w);
    for (HWND x : wins) PostMessageW(x, WM_LOUPE_QUIT, 0, 0);
    DWORD start = GetTickCount();
    while (GetTickCount() - start < waitMs) {
        bool any = false;
        for (HWND x : wins) any |= IsWindow(x) != FALSE;
        if (!any) break;
        Sleep(100);
    }
}

// ---------------------------------------------------------------- --update-test (CI)
static std::wstring s_testLog;
static int s_testExit = 1;

static void TestLog(const std::wstring& line) {
    std::wstring prev;
    ReadTextFile(s_testLog, prev);
    WriteTextFileAtomic(s_testLog, prev + line + L"\r\n");
}

static LRESULT CALLBACK UpdateTestProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LOUPE_UPDATE) {
        auto* s = (UpdateState*)lp;
        switch (s->kind) {
            case UpdateState::Checking: TestLog(L"checking"); break;
            case UpdateState::Downloading: if (s->percent == 0 || s->percent == 100) TestLog(L"downloading " + s->version + L" " + std::to_wstring(s->percent) + L"%"); break;
            case UpdateState::UpToDate: TestLog(L"up to date (nothing newer than " LOUPE_VERSION L")"); s_testExit = 0; PostQuitMessage(0); break;
            case UpdateState::Error: TestLog(L"error: " + s->message); s_testExit = 3; PostQuitMessage(0); break;
            case UpdateState::Ready: {
                TestLog(L"ready: " + s->version + L" verified at " + s->staged);
                std::wstring err;
                bool ok = updater::Apply(nullptr, s->staged, s->version, L"", true, &err);
                TestLog(ok ? L"applied: the new version started and reported healthy" : L"apply failed: " + err);
                s_testExit = ok ? 0 : 2;
                PostQuitMessage(0);
                break;
            }
            default: break;
        }
        delete s;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int UpdateTest(HINSTANCE inst, const std::wstring& log) {
    s_testLog = log;
    WriteTextFileAtomic(log, std::wstring(L"update test from ") + LOUPE_VERSION + L"\r\n");
    net::Init();
    WNDCLASSW wc{};
    wc.lpfnWndProc = UpdateTestProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"Loupe.UpdateTest";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    updater::Check(h, true);
    SetTimer(h, 1, 240 * 1000, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_TIMER) { TestLog(L"timed out"); s_testExit = 4; break; }
        DispatchMessageW(&m);
    }
    return s_testExit;
}

// ---------------------------------------------------------------- entry
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring openPath, smokeLog, smokeDir, updateLog, versionFile;
    bool quit = false, uninstall = false, reg = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--version") { LocalFree(argv); return 0; }
        if (a == L"--quit") quit = true;
        else if (a == L"--uninstall") uninstall = true;
        else if (a == L"--register") reg = true;
        else if (a == L"--write-version" && i + 1 < argc) versionFile = argv[++i];
        else if (a == L"--smoke" && i + 2 < argc) { smokeLog = FullPath(argv[i + 1]); smokeDir = FullPath(argv[i + 2]); i += 2; }
        else if (a == L"--update-test" && i + 1 < argc) updateLog = FullPath(argv[++i]);
        else if ((a == L"--after-update" || a == L"--healthy-event" || a == L"--old-pid") && i + 1 < argc) ++i;
        else if (a == L"--background") {}
        else if (!a.empty() && a[0] != L'-' && openPath.empty()) openPath = FullPath(a);
    }
    LocalFree(argv);
    if (!versionFile.empty()) { WriteTextFileAtomic(versionFile, LOUPE_VERSION); return 0; }
    if (quit) { CloseAll(15000); return 0; }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (uninstall) {
        CloseAll(15000);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        shell::UnregisterFileTypes();
        if (!IsPortable()) {
            std::wstring dir = DataDir();
            std::vector<wchar_t> from(dir.begin(), dir.end());
            from.push_back(0);
            from.push_back(0);
            SHFILEOPSTRUCTW op{};
            op.wFunc = FO_DELETE;
            op.pFrom = from.data();
            op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
            SHFileOperationW(&op);
        }
        return 0;
    }
    if (reg) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        shell::RegisterFileTypes();
        return 0;
    }
    if (!updateLog.empty()) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        LogInit();
        return UpdateTest(inst, updateLog);
    }

    updater::AfterUpdate after = updater::ParseArgs(GetCommandLineW());
    ConfigLoad();
    if (!smokeLog.empty()) {
        // the smoke test opens the folder's first image the way a double-click would
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((smokeDir + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsSupportedExtension(ExtOf(fd.cFileName))) names.push_back(fd.cFileName); } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
        for (auto& n : names) if (ExtOf(n) == L".jpg") { openPath = smokeDir + L"\\" + n; break; }
        if (openPath.empty() && !names.empty()) openPath = smokeDir + L"\\" + names[0];
    } else if (!after.active && g_cfg.singleWindow) {
        // hand the file to the open window: it shows it without starting anything new
        if (HWND existing = FindWindowW(LOUPE_WINDOW_CLASS, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            COPYDATASTRUCT cd{0x4C4F5550, (DWORD)((openPath.size() + 1) * sizeof(wchar_t)), (void*)openPath.c_str()};
            DWORD_PTR res = 0;
            if (SendMessageTimeoutW(existing, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &res) && res) return 0;
        }
    }
    // start decoding right away: by the time the window exists, the pixels usually do too
    if (!openPath.empty() && !after.background) UiPrefetch(openPath);

    g_mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetCurrentProcessExplicitAppUserModelID(L"larosa023.Loupe");
    LogInit();
    Log(L"==== Loupe %ls starting (data: %ls%ls)%ls", LOUPE_VERSION, DataDir().c_str(), IsPortable() ? L", portable" : L"",
        after.active ? (after.background ? L" [update health check]" : L" [after update]") : L"");
    if (!after.active) updater::CleanupOldExe();
    net::Init();

    HWND hwnd = UiCreate(inst, show, openPath, after, smokeLog, smokeDir);
    if (!hwnd) {
        if (!after.background) MessageBoxW(nullptr, L"Loupe couldn't start. Details are in loupe.log.", L"Loupe", MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    net::Shutdown();
    Log(L"Loupe exiting");
    CoUninitialize();
    if (g_mutex) CloseHandle(g_mutex);
    return smokeLog.empty() ? 0 : SmokeExitCode();
}
