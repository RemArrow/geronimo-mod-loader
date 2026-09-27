// Mirror every cooked container found under GML\plugins\ (any depth; conventionally a plugin's
// Paks\ folder) into <game>\Content\Paks\~GML\ at process entry, before the engine's pak
// platform file scans Content\Paks. Verified: the engine mounts containers from that subfolder
// (the game file-locks them and serves their packages). In this build that delivers overrides
// of existing packages; containers declaring new package IDs mount but those packages do not
// register (see README "Known limitations").
//
// ~GML is owned by the loader: anything in it that no plugin provides is deleted, so removing
// or disabling (disabled.txt) a plugin removes its content on the next launch.
#include "internal.h"
#include <map>

namespace gml {

static bool IsPakFile(const std::wstring& n) {
    size_t dot = n.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = n.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L".pak" || ext == L".utoc" || ext == L".ucas" || ext == L".sig";
}

static bool SameFile(const std::wstring& a, const std::wstring& b) {
    WIN32_FILE_ATTRIBUTE_DATA fa, fb;
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (!GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb)) return false;
    return fa.nFileSizeHigh == fb.nFileSizeHigh && fa.nFileSizeLow == fb.nFileSizeLow &&
           CompareFileTime(&fa.ftLastWriteTime, &fb.ftLastWriteTime) == 0;
}

static std::string Rel(const std::wstring& p) { return Narrow(p.substr(g_paths.gmlRoot.size() + 1)); }

// file name -> source path
static void Collect(const std::wstring& dir, std::map<std::wstring, std::wstring>& want) {
    if (GetFileAttributesW((dir + L"\\disabled.txt").c_str()) != INVALID_FILE_ATTRIBUTES) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring full = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { subdirs.push_back(full); continue; }
        if (!IsPakFile(n)) continue;
        auto it = want.find(n);
        if (it != want.end())
            LOGW("paks: %s is provided twice (%s and %s) - using the second", Narrow(n).c_str(),
                 Rel(it->second).c_str(), Rel(full).c_str());
        want[n] = full;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& s : subdirs) Collect(s, want);
}

void SyncPluginPaks() {
    const std::wstring dst = g_paths.gameRoot + L"\\Content\\Paks\\~GML";
    std::map<std::wstring, std::wstring> want;
    Collect(g_paths.plugins, want);
    CreateDirectoryW(dst.c_str(), nullptr);

    // Remove stale files first.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dst + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!want.count(fd.cFileName)) {
                std::wstring p = dst + L"\\" + fd.cFileName;
                if (DeleteFileW(p.c_str())) LOGI("paks: removed stale %s", Narrow(fd.cFileName).c_str());
                else LOGW("paks: could not remove %s (error %lu)", Narrow(fd.cFileName).c_str(), GetLastError());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    int copied = 0;
    for (auto& [name, src] : want) {
        std::wstring target = dst + L"\\" + name;
        if (SameFile(src, target)) continue;
        if (CopyFileW(src.c_str(), target.c_str(), FALSE)) {
            copied++;
            LOGI("paks: %s -> Content\\Paks\\~GML", Rel(src).c_str());
        } else {
            LOGE("paks: copy of %s failed (error %lu)", Rel(src).c_str(), GetLastError());
        }
    }
    LOGI("paks: %zu file(s) managed, %d updated", want.size(), copied);
}

}  // namespace gml
