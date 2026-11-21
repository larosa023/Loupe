// Loupe - a fast, modern photo viewer for Windows (larosa023 suite)
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#ifndef LOUPE_VERSION
#define LOUPE_VERSION L"0.1.0"
#endif
#define LOUPE_REPO_OWNER L"larosa023"
#define LOUPE_REPO_NAME L"Loupe"
#define LOUPE_WINDOW_CLASS L"Loupe.larosa023.Main"

// Messages posted from worker threads to the main window. lParam owns a heap object that the
// window procedure takes over and deletes.
enum : UINT {
    WM_LOUPE_DECODED = WM_APP + 1,   // lParam: DecodeResult*
    WM_LOUPE_THUMB,                  // lParam: ThumbResult*
    WM_LOUPE_FOLDER,                 // lParam: FolderResult*
    WM_LOUPE_DATES,                  // lParam: DatesResult* (date taken for sorting)
    WM_LOUPE_INFO,                   // lParam: InfoResult* (info panel metadata)
    WM_LOUPE_UPDATE,                 // lParam: UpdateState*
    WM_LOUPE_FOLDER_CHANGED,         // the open folder changed on disk
    WM_LOUPE_HANGAR_CHANGED,         // Hangar's theme changed (Match Hangar)
    WM_LOUPE_TASK_DONE,              // lParam: TaskDone* (save, recycle, restore…)
    WM_LOUPE_QUIT,                   // --quit from Hangar
};
