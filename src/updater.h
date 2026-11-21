// Loupe - built-in updater (GitHub Releases of larosa023/Loupe).
//
//   1. On launch (at most hourly) and every few hours, a background job finds the newest release on
//      your channel: Stable, or Beta (pre-releases, and any version with a "-" such as 0.2.0-beta.1).
//      It reads Hangar's release index first (no GitHub rate limit), then the GitHub API.
//   2. It downloads Loupe.exe to DataDir()\updates\<version>\ and checks its SHA-256 against GitHub's
//      asset digest and SHA256SUMS.txt (both must agree; a release with no checksum is refused).
//   3. "Update ready, restart" swaps the exe (the running one is renamed to Loupe.exe.old), starts the
//      new one with --after-update and waits for it to signal that its window drew. If it doesn't within
//      20 seconds, or it quits, the old exe is put back, Loupe carries on and skips that version.
//      With automatic updates on, a ready update is also installed the same way when you close Loupe.
//   4. The previous exe is kept in DataDir()\versions\<version>\ (last 2) for rolling back from Settings.
#pragma once
#include "common.h"

struct UpdateState {
    enum Kind { Idle, Checking, UpToDate, Downloading, Ready, Error } kind = Idle;
    std::wstring version;        // the newer version (Downloading / Ready)
    std::wstring notes;          // its release notes
    std::wstring staged;         // verified exe on disk (Ready)
    std::wstring message;        // Error text
    int percent = -1;
    bool manual = false;         // started from "Check for updates now"
};

namespace updater {

void Check(HWND notify, bool manual);

// Swaps in `exe` (a staged update or a previous version) and hands over to it.
//   background=false: the new copy opens `reopenArgs` (the current image) and this one exits on success.
//   background=true:  (closing Loupe) the new copy only checks that it starts, then exits.
// Returns false, with a message, when it stayed on this version.
bool Apply(HWND hwnd, const std::wstring& exe, const std::wstring& version, const std::wstring& reopenArgs, bool background, std::wstring* error);

struct AfterUpdate {
    bool active = false;
    bool background = false;     // health check only: start, draw once, exit
    std::wstring fromVersion;
    std::wstring healthyEvent;
    DWORD oldPid = 0;
};
AfterUpdate ParseArgs(const wchar_t* cmdLine);
void SignalHealthy(const AfterUpdate& a);
void FinishAfterUpdate(const AfterUpdate& a, bool wait);   // keeps the previous exe for rollback

struct Previous { std::wstring version, exe; };
std::vector<Previous> PreviousVersions();                  // newest first
void CleanupOldExe();                                       // a leftover Loupe.exe.old

} // namespace updater

// main.cpp owns the single-instance mutex; it is released while handing over to a new version
void ReleaseInstanceMutex();
void ReacquireInstanceMutex();
