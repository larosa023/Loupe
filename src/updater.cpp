// Loupe - built-in updater (see updater.h)
#include "updater.h"
#include "net.h"
#include "util.h"
#include "json.h"
#include "config.h"
#include <thread>
#include <atomic>
#include <ctime>
#include <shellapi.h>
#include <algorithm>

namespace updater {

static std::atomic<bool> s_busy{false};

struct Asset { std::wstring name, url, sha256; uint64_t size = 0; };
struct Release { std::wstring tag, version, notes; bool prerelease = false; std::vector<Asset> assets; };

static const Asset* FindAsset(const Release& r, const std::wstring& name) {
    for (auto& a : r.assets) if (IEquals(a.name, name)) return &a;
    return nullptr;
}

static bool ParseReleases(const json::Value& arr, std::vector<Release>& out) {
    if (arr.type != json::Value::Array) return false;
    for (auto& v : arr.arr) {
        if (v.type != json::Value::Object || v.GetBool(L"draft", false)) continue;
        Release r;
        r.tag = v.GetStr(L"tag_name");
        r.version = NormalizeVersion(r.tag);
        if (r.version.empty() || !iswdigit(r.version[0])) continue;
        r.prerelease = v.GetBool(L"prerelease", false) || r.version.find(L'-') != std::wstring::npos;
        r.notes = v.GetStr(L"body");
        if (const json::Value* as = v.Get(L"assets"); as && as->type == json::Value::Array) {
            for (auto& a : as->arr) {
                Asset x;
                x.name = a.GetStr(L"name");
                x.url = a.GetStr(L"browser_download_url");
                x.size = (uint64_t)a.GetNum(L"size", 0);
                std::wstring d = a.GetStr(L"digest");
                if (IStartsWith(d, L"sha256:")) x.sha256 = Lower(d.substr(7));
                r.assets.push_back(x);
            }
        }
        out.push_back(std::move(r));
    }
    return true;
}

// Hangar's release index (raw.githubusercontent.com, no API limit), when it's fresh
static bool FromIndex(std::vector<Release>& out) {
    net::Response r;
    if (!net::Get(L"https://raw.githubusercontent.com/larosa023/Hangar/index/releases.json", r) || r.status != 200) return false;
    json::Value v;
    if (!json::Parser(Utf8ToWide(r.body)).Parse(v) || v.type != json::Value::Object) return false;
    long long generated = (long long)v.GetNum(L"generated", 0);
    if (time(nullptr) - generated > 24 * 3600) return false;
    const json::Value* repos = v.Get(L"repos");
    const json::Value* mine = repos ? repos->Get(L"larosa023/loupe") : nullptr;
    return mine && ParseReleases(*mine, out) && !out.empty();
}

static bool FromApi(std::vector<Release>& out, std::wstring& err) {
    net::Response r;
    if (!net::Get(net::ApiBase() + L"/repos/larosa023/Loupe/releases?per_page=10", r, L"", true)) { err = r.error; return false; }
    if (r.status == 403 || r.status == 429) { err = L"GitHub's hourly limit was reached. Loupe will try again later."; return false; }
    if (r.status == 404) { err = L"No releases yet."; return false; }
    if (r.status != 200) { err = L"GitHub answered with HTTP " + std::to_wstring(r.status) + L"."; return false; }
    json::Value v;
    if (!json::Parser(Utf8ToWide(r.body)).Parse(v) || !ParseReleases(v, out)) { err = L"GitHub's answer couldn't be read."; return false; }
    return true;
}

static std::wstring HashFromSums(const std::string& body, const std::wstring& name) {
    std::wstring text = Utf8ToWide(body);
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = Trim(text.substr(i, e - i));
        i = e + 1;
        size_t sp = line.find_first_of(L" \t");
        if (sp == std::wstring::npos) continue;
        std::wstring hash = Lower(line.substr(0, sp)), file = Trim(line.substr(sp));
        if (!file.empty() && file[0] == L'*') file.erase(0, 1);
        if (hash.size() == 64 && IEquals(file, name)) return hash;
    }
    return L"";
}

static void Post(HWND hwnd, UpdateState* s) {
    if (!hwnd || !PostMessageW(hwnd, WM_LOUPE_UPDATE, 0, (LPARAM)s)) delete s;
}

void Check(HWND notify, bool manual) {
    if (s_busy.exchange(true)) return;
    bool beta = g_cfg.beta;
    std::thread([notify, manual, beta] {
        SetThreadDescription(GetCurrentThread(), L"Loupe updater");
        auto state = [&](UpdateState::Kind k, const std::wstring& ver = L"", const std::wstring& msg = L"", int pct = -1) {
            auto* s = new UpdateState;
            s->kind = k; s->version = ver; s->message = msg; s->percent = pct; s->manual = manual;
            Post(notify, s);
        };
        state(UpdateState::Checking);
        std::vector<Release> rels;
        std::wstring err;
        // the index can lag a few minutes behind a release; a manual check asks GitHub directly
        bool ok = (!manual && FromIndex(rels)) || FromApi(rels, err);
        if (!ok) { rels.clear(); ok = FromIndex(rels); }
        if (!ok) {
            Log(L"update check: %ls", err.c_str());
            state(UpdateState::Error, L"", err.empty() ? L"Couldn't reach GitHub." : err);
            s_busy = false;
            return;
        }
        const Release* best = nullptr;
        for (auto& r : rels) {
            if (r.prerelease && !beta) continue;
            if (!manual && IsSkippedVersion(r.version)) continue;
            if (!FindAsset(r, L"Loupe.exe")) continue;
            if (!best || CompareVersions(r.version, best->version) > 0) best = &r;
        }
        if (!best || CompareVersions(best->version, LOUPE_VERSION) <= 0) {
            state(UpdateState::UpToDate);
            s_busy = false;
            return;
        }
        const Release rel = *best;
        const Asset* asset = FindAsset(rel, L"Loupe.exe");
        std::wstring expected = asset->sha256;
        if (const Asset* sums = FindAsset(rel, L"SHA256SUMS.txt")) {
            net::Response sr;
            if (net::Get(sums->url, sr) && sr.status == 200) {
                std::wstring h = HashFromSums(sr.body, L"Loupe.exe");
                if (!h.empty() && !expected.empty() && h != expected) {
                    state(UpdateState::Error, rel.version, L"The checksums for Loupe " + rel.version + L" don't agree, so it won't be installed.");
                    s_busy = false;
                    return;
                }
                if (!h.empty()) expected = h;
            }
        }
        if (expected.empty()) {
            state(UpdateState::Error, rel.version, L"Loupe " + rel.version + L" has no checksum, so it can't be verified.");
            s_busy = false;
            return;
        }
        std::wstring path = DataDir() + L"\\updates\\" + rel.version + L"\\Loupe.exe";
        bool have = FileExists(path) && Sha256File(path) == expected;
        if (!have) {
            int last = -2;
            have = net::Download(asset->url, path, [&](uint64_t done, uint64_t total) {
                if (!total) total = asset->size;
                int pct = total ? (int)(done * 100 / total) : -1;
                if (pct != last) { last = pct; state(UpdateState::Downloading, rel.version, L"", pct); }
                return true;
            }, &err);
            if (have && Sha256File(path) != expected) {
                DeleteFileW(path.c_str());
                have = false;
                err = L"The download didn't match its checksum.";
            }
        }
        if (!have) {
            Log(L"update download: %ls", err.c_str());
            state(UpdateState::Error, rel.version, err);
            s_busy = false;
            return;
        }
        // tidy older staged updates
        WIN32_FIND_DATAW fd;
        std::wstring updates = DataDir() + L"\\updates";
        HANDLE f = FindFirstFileW((updates + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L".." || n == rel.version || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                DeleteFileW((updates + L"\\" + n + L"\\Loupe.exe").c_str());
                RemoveDirectoryW((updates + L"\\" + n).c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        Log(L"update: %ls downloaded and verified", rel.version.c_str());
        auto* s = new UpdateState;
        s->kind = UpdateState::Ready;
        s->version = rel.version;
        s->notes = rel.notes;
        s->staged = path;
        s->manual = manual;
        Post(notify, s);
        s_busy = false;
    }).detach();
}

bool Apply(HWND hwnd, const std::wstring& newExe, const std::wstring& version, const std::wstring& reopenArgs, bool background, std::wstring* error) {
    std::wstring exe = ExePath();
    std::wstring old = exe + L".old";
    DeleteFileW(old.c_str());
    // Windows lets a running exe be renamed, not overwritten
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = L"Loupe can't update itself in " + ExeDir() + L" (no permission). Move Loupe.exe to a folder you own.";
        return false;
    }
    if (!CopyFileW(newExe.c_str(), exe.c_str(), FALSE)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        *error = L"Couldn't put the new Loupe.exe in place.";
        return false;
    }
    wchar_t evName[96];
    swprintf(evName, 96, L"Local\\Loupe.larosa023.Healthy.%lu", GetCurrentProcessId());
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName);
    if (hwnd && IsWindow(hwnd)) ShowWindow(hwnd, SW_HIDE);
    ReleaseInstanceMutex();
    wchar_t args[256];
    swprintf(args, 256, L"--after-update %ls --healthy-event %ls --old-pid %lu%ls", LOUPE_VERSION, evName, GetCurrentProcessId(),
             background ? L" --background" : L"");
    std::wstring cmd = L"\"" + exe + L"\" " + args + (reopenArgs.empty() ? L"" : L" " + reopenArgs);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    bool started = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi) != 0;
    bool healthy = false;
    if (started) {
        CloseHandle(pi.hThread);
        HANDLE waits[2] = {ev, pi.hProcess};
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 20000);
        healthy = w == WAIT_OBJECT_0;
        if (!healthy) {
            Log(L"update: Loupe %ls didn't start (wait result %lu); rolling back", version.c_str(), w);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hProcess);
    }
    if (ev) CloseHandle(ev);
    if (healthy) {
        Log(L"update: handed over to Loupe %ls", version.c_str());
        return true;
    }
    // roll back: put ourselves back and carry on
    for (int i = 0; i < 20 && !DeleteFileW(exe.c_str()) && FileExists(exe); ++i) Sleep(100);
    MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    ReacquireInstanceMutex();
    if (hwnd && IsWindow(hwnd) && !background) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
    *error = L"Loupe " + version + L" didn't start, so you're still on " + LOUPE_VERSION + L". Loupe will skip that version.";
    return false;
}

AfterUpdate ParseArgs(const wchar_t* cmdLine) {
    AfterUpdate a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"x", &argc);
    if (!argv) return a;
    for (int i = 0; i < argc; ++i) {
        std::wstring k = argv[i];
        if (k == L"--background") a.background = true;
        if (i + 1 >= argc) continue;
        if (k == L"--after-update") { a.active = true; a.fromVersion = argv[i + 1]; }
        else if (k == L"--healthy-event") a.healthyEvent = argv[i + 1];
        else if (k == L"--old-pid") a.oldPid = (DWORD)_wtoi(argv[i + 1]);
    }
    LocalFree(argv);
    return a;
}

void SignalHealthy(const AfterUpdate& a) {
    if (!a.active || a.healthyEvent.empty()) return;
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, a.healthyEvent.c_str())) {
        SetEvent(ev);
        CloseHandle(ev);
    }
}

static std::wstring VersionsDir() { return DataDir() + L"\\versions"; }

static void KeepPrevious(AfterUpdate copy) {
    if (copy.oldPid) {
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, copy.oldPid)) { WaitForSingleObject(h, 30000); CloseHandle(h); }
    }
    std::wstring old = ExePath() + L".old";
    if (!FileExists(old)) return;
    std::wstring ver = NormalizeVersion(copy.fromVersion);
    bool kept = false;
    if (!ver.empty() && ver.find_first_of(L"\\/:") == std::wstring::npos && CompareVersions(ver, LOUPE_VERSION) != 0) {
        std::wstring dir = VersionsDir() + L"\\" + ver;
        EnsureDir(dir);
        kept = CopyFileW(old.c_str(), (dir + L"\\Loupe.exe").c_str(), FALSE) != 0;
    }
    for (int i = 0; i < 10 && FileExists(old); ++i) { if (DeleteFileW(old.c_str())) break; Sleep(500); }
    if (FileExists(old)) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    Log(L"after update from %ls: previous version %ls", ver.c_str(), kept ? L"kept for rollback" : L"removed");
    auto prev = PreviousVersions();
    for (size_t i = 2; i < prev.size(); ++i) {
        DeleteFileW(prev[i].exe.c_str());
        RemoveDirectoryW(PathParent(prev[i].exe).c_str());
    }
}

void FinishAfterUpdate(const AfterUpdate& a, bool wait) {
    if (!a.active) return;
    if (wait) { KeepPrevious(a); return; }
    std::thread([a] { KeepPrevious(a); }).detach();
}

std::vector<Previous> PreviousVersions() {
    std::vector<Previous> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((VersionsDir() + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring exe = VersionsDir() + L"\\" + n + L"\\Loupe.exe";
        if (FileExists(exe) && CompareVersions(n, LOUPE_VERSION) != 0) out.push_back({n, exe});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const Previous& a, const Previous& b) { return CompareVersions(a.version, b.version) > 0; });
    return out;
}

void CleanupOldExe() {
    std::wstring old = ExePath() + L".old";
    if (FileExists(old)) DeleteFileW(old.c_str());
}

} // namespace updater
