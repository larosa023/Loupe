// Loupe - background work: decoding, thumbnails, folder listings, metadata, folder watching.
//
// Everything here runs on worker threads and posts its result to the window with PostMessage.
// The UI thread never waits on the disk.
#pragma once
#include "common.h"
#include "image.h"
#include "exif.h"
#include <functional>

struct FileEntry {
    std::wstring name;
    uint64_t size = 0;
    int64_t modified = 0;       // FILETIME as a number
    int64_t taken = 0;          // date taken (local, FILETIME units); 0 = unknown
    bool takenRead = false;
};

struct DecodeResult {
    uint64_t id = 0;            // the cache entry it was for
    std::wstring path;
    bool full = false;          // full-resolution decode (vs. the screen-sized one)
    ImageData data;
    double ms = 0;
};

struct ThumbResult {
    std::wstring path;
    UINT size = 0;
    Pixels pix;
    bool failed = false;
};

struct FolderResult {
    uint64_t gen = 0;
    std::wstring folder;
    std::vector<FileEntry> files;
};

struct DatesResult {
    uint64_t gen = 0;
    std::vector<std::pair<std::wstring, int64_t>> taken;   // name -> date taken (0 = none)
};

struct InfoResult {
    std::wstring path;
    PhotoInfo info;
    bool ok = false;
};

namespace loader {

void Init(HWND notify);
void Shutdown();

// Decoding. Requests with the same (id, full) are merged. Lower priority runs first.
void Decode(uint64_t id, const std::wstring& path, bool full, UINT maxW, UINT maxH, int priority);
void CancelDecodes(const std::vector<uint64_t>& keepIds);   // drops queued requests for other entries

// Thumbnails. Call BeginThumbs() before (re)queuing the visible ones in order; queued ones that weren't
// asked for again are dropped.
void BeginThumbs();
void Thumb(const std::wstring& path, UINT size);

void ListFolder(uint64_t gen, const std::wstring& folder);
void ReadDates(uint64_t gen, const std::wstring& folder, const std::vector<std::wstring>& names);
void ReadInfo(const std::wstring& path);
void WatchFolder(const std::wstring& folder);              // "" stops watching

// Runs any job on the misc worker (saving, recycling…)
void Run(std::function<void()> job);

} // namespace loader
