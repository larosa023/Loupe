// Loupe - background work (see loader.h)
#include "loader.h"
#include "util.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <set>

namespace loader {

static HWND s_hwnd = nullptr;

template <class T> static void Post(UINT msg, T* obj, WPARAM wp = 0) {
    if (!s_hwnd || !PostMessageW(s_hwnd, msg, wp, (LPARAM)obj)) delete obj;
}

// ---------------------------------------------------------------- a small priority pool
class Pool {
public:
    struct Job {
        int prio = 0;
        uint64_t seq = 0;
        std::wstring key;
        uint64_t id = 0;
        std::function<void()> fn;
    };

    void Start(int n, const wchar_t* name) {
        for (int i = 0; i < n; ++i) {
            threads_.emplace_back([this, name] {
                SetThreadDescription(GetCurrentThread(), name);
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                Loop();
                CoUninitialize();
            });
        }
    }
    void Stop() {
        { std::lock_guard<std::mutex> g(m_); stop_ = true; q_.clear(); }
        cv_.notify_all();
        for (auto& t : threads_) t.detach();   // workers may be inside a long decode; don't wait at exit
        threads_.clear();
    }
    // returns false when the same key is already queued or running (its priority is raised instead)
    bool Push(Job j) {
        std::lock_guard<std::mutex> g(m_);
        if (stop_) return false;
        if (!j.key.empty()) {
            if (running_.count(j.key)) return false;
            for (auto& q : q_) if (q.key == j.key) { q.prio = std::min(q.prio, j.prio); return false; }
        }
        j.seq = ++seq_;
        q_.push_back(std::move(j));
        cv_.notify_one();
        return true;
    }
    template <class Pred> void RemoveIf(Pred p) {
        std::lock_guard<std::mutex> g(m_);
        q_.erase(std::remove_if(q_.begin(), q_.end(), p), q_.end());
    }
    void Clear() { std::lock_guard<std::mutex> g(m_); q_.clear(); }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Job> q_;
    std::set<std::wstring> running_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    uint64_t seq_ = 0;

    void Loop() {
        while (true) {
            Job j;
            {
                std::unique_lock<std::mutex> g(m_);
                cv_.wait(g, [this] { return stop_ || !q_.empty(); });
                if (stop_) return;
                auto best = std::min_element(q_.begin(), q_.end(), [](const Job& a, const Job& b) {
                    return a.prio != b.prio ? a.prio < b.prio : a.seq < b.seq;
                });
                j = std::move(*best);
                q_.erase(best);
                if (!j.key.empty()) running_.insert(j.key);
            }
            j.fn();
            if (!j.key.empty()) { std::lock_guard<std::mutex> g(m_); running_.erase(j.key); }
        }
    }
};

static Pool s_decode, s_thumbs, s_misc;
static std::atomic<int> s_thumbOrder{0};

void Init(HWND notify) {
    s_hwnd = notify;
    unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    s_decode.Start(2, L"Loupe decode");
    s_thumbs.Start((int)std::clamp(cores / 3, 2u, 4u), L"Loupe thumbnails");
    s_misc.Start(1, L"Loupe misc");
}

void Shutdown() {
    s_hwnd = nullptr;
    WatchFolder(L"");
    s_decode.Stop();
    s_thumbs.Stop();
    s_misc.Stop();
}

static double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

void Decode(uint64_t id, const std::wstring& path, bool full, UINT maxW, UINT maxH, int priority) {
    Pool::Job j;
    j.prio = priority;
    j.id = id;
    j.key = std::to_wstring(id) + (full ? L":full" : L":view");
    j.fn = [id, path, full, maxW, maxH] {
        auto* r = new DecodeResult;
        r->id = id;
        r->path = path;
        r->full = full;
        double t0 = NowMs();
        DecodeImage(path, full ? 0 : maxW, full ? 0 : maxH, true, r->data);
        r->ms = NowMs() - t0;
        Post(WM_LOUPE_DECODED, r);
    };
    s_decode.Push(std::move(j));
}

void CancelDecodes(const std::vector<uint64_t>& keep) {
    s_decode.RemoveIf([&](const Pool::Job& j) { return std::find(keep.begin(), keep.end(), j.id) == keep.end(); });
}

void BeginThumbs() {
    s_thumbs.Clear();
    s_thumbOrder = 0;
}

void Thumb(const std::wstring& path, UINT size) {
    Pool::Job j;
    j.prio = ++s_thumbOrder;
    j.key = path;
    j.fn = [path, size] {
        auto* r = new ThumbResult;
        r->path = path;
        r->size = size;
        ImageData d;
        if (DecodeImage(path, size, size, false, d) && !d.frames.empty()) r->pix = std::move(d.frames[0].pix);
        else r->failed = true;
        Post(WM_LOUPE_THUMB, r);
    };
    s_thumbs.Push(std::move(j));
}

void ListFolder(uint64_t gen, const std::wstring& folder) {
    Pool::Job j;
    j.prio = -10;
    j.fn = [gen, folder] {
        auto* r = new FolderResult;
        r->gen = gen;
        r->folder = folder;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                std::wstring name = fd.cFileName;
                if (!IsSupportedExtension(ExtOf(name))) continue;
                FileEntry e;
                e.name = name;
                e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                e.modified = ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                r->files.push_back(std::move(e));
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        Post(WM_LOUPE_FOLDER, r);
    };
    s_misc.Push(std::move(j));
}

void ReadDates(uint64_t gen, const std::wstring& folder, const std::vector<std::wstring>& names) {
    // on the thumbnail pool's priority scale: after visible thumbnails, but it still runs in parallel
    std::thread([gen, folder, names] {
        SetThreadDescription(GetCurrentThread(), L"Loupe dates");
        auto* r = new DatesResult;
        r->gen = gen;
        r->taken.reserve(names.size());
        for (auto& n : names) {
            PhotoInfo info;
            int64_t t = 0;
            if (ReadPhotoInfo(folder + L"\\" + n, info, true) && info.hasTaken) {
                FILETIME ft;
                if (SystemTimeToFileTime(&info.taken, &ft)) t = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            }
            r->taken.emplace_back(n, t);
        }
        Post(WM_LOUPE_DATES, r);
    }).detach();
}

void ReadInfo(const std::wstring& path) {
    Pool::Job j;
    j.prio = -5;
    j.fn = [path] {
        auto* r = new InfoResult;
        r->path = path;
        r->ok = ReadPhotoInfo(path, r->info, false);
        Post(WM_LOUPE_INFO, r);
    };
    s_misc.Push(std::move(j));
}

void Run(std::function<void()> job) {
    Pool::Job j;
    j.prio = 0;
    j.fn = std::move(job);
    s_misc.Push(std::move(j));
}

// ---------------------------------------------------------------- folder watching
static HANDLE s_watchStop = nullptr;
static std::atomic<unsigned> s_watchGen{0};

void WatchFolder(const std::wstring& folder) {
    if (s_watchStop) { SetEvent(s_watchStop); s_watchStop = nullptr; }
    if (folder.empty()) return;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_watchStop = stop;
    unsigned gen = ++s_watchGen;
    std::thread([folder, stop, gen] {
        SetThreadDescription(GetCurrentThread(), L"Loupe folder watch");
        HANDLE ch = FindFirstChangeNotificationW(folder.c_str(), FALSE,
                                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
        if (ch == INVALID_HANDLE_VALUE) { CloseHandle(stop); return; }
        HANDLE waits[2] = {stop, ch};
        while (true) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0 + 1) break;
            // let a burst of changes (a copy of many files) settle
            if (WaitForSingleObject(stop, 400) == WAIT_OBJECT_0) break;
            if (gen == s_watchGen && s_hwnd) PostMessageW(s_hwnd, WM_LOUPE_FOLDER_CHANGED, gen, 0);
            if (!FindNextChangeNotification(ch)) break;
        }
        FindCloseChangeNotification(ch);
        CloseHandle(stop);
    }).detach();
}

} // namespace loader
