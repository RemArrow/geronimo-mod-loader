// Mirror every Unreal mod plugin found under GML\plugins\ (any depth: a folder that holds a
// <Name>.uplugin, conventionally plugins\<Plugin>\Mods\<Name>\) into <game>\Mods\<Name>\ at
// process entry, before the engine's plugin manager scans that folder. Verified (2026-10-10, the
// RD704): the engine mounts a mod plugin's Content\Paks\Windows containers there and merges its
// AssetRegistry.bin, so its NEW packages register - a GunAsset in it even shows up in
// BP_GameInstance::GetItems on its own. That is not true of containers in Content\Paks (paks.cpp).
//
// Only folders the loader put there are managed (listed in <game>\Mods\.gml-managed): removing or
// disabling (disabled.txt) the plugin removes its mod on the next launch, and a mod installed by
// hand is never touched or overwritten.
#include "internal.h"
#include <fstream>
#include <map>
#include <set>

namespace gml {

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::string Rel(const std::wstring& p) { return Narrow(p.substr(g_paths.gmlRoot.size() + 1)); }

// The mod plugin name of dir: its <Name>.uplugin, where Name is also the folder name (the engine's rule).
static std::wstring ModName(const std::wstring& dir) {
    std::wstring name = dir.substr(dir.find_last_of(L'\\') + 1);
    return Exists(dir + L"\\" + name + L".uplugin") ? name : std::wstring();
}

bool IsModPluginDir(const std::wstring& dir) { return !ModName(dir).empty(); }

// mod name -> source folder
static void Collect(const std::wstring& dir, std::map<std::wstring, std::wstring>& want) {
    if (Exists(dir + L"\\disabled.txt")) return;
    std::wstring name = ModName(dir);
    if (!name.empty()) {
        for (auto& [n, src] : want)
            if (_wcsicmp(n.c_str(), name.c_str()) == 0) {
                LOGW("mods: %s is provided twice (%s and %s) - using the second", Narrow(name).c_str(),
                     Rel(src).c_str(), Rel(dir).c_str());
                want.erase(n);
                break;
            }
        want[name] = dir;
        return;  // a mod's own folders are copied whole, not searched for more mods
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        std::wstring n = fd.cFileName;
        if (n != L"." && n != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) subdirs.push_back(dir + L"\\" + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& s : subdirs) Collect(s, want);
}

static bool SameFile(const std::wstring& a, const std::wstring& b) {
    WIN32_FILE_ATTRIBUTE_DATA fa, fb;
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (!GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb)) return false;
    return fa.nFileSizeHigh == fb.nFileSizeHigh && fa.nFileSizeLow == fb.nFileSizeLow &&
           CompareFileTime(&fa.ftLastWriteTime, &fb.ftLastWriteTime) == 0;
}

static void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring full = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(full);
            else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Make dst an exact copy of src. Returns the number of files copied; *failed counts errors.
static int Mirror(const std::wstring& src, const std::wstring& dst, int* failed) {
    CreateDirectoryW(dst.c_str(), nullptr);
    std::set<std::wstring> keep;  // lower-cased names present in src
    int copied = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring lower = n;
            for (auto& c : lower) c = (wchar_t)towlower(c);
            keep.insert(lower);
            std::wstring s = src + L"\\" + n, d = dst + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { copied += Mirror(s, d, failed); continue; }
            if (SameFile(s, d)) continue;
            if (CopyFileW(s.c_str(), d.c_str(), FALSE)) copied++;
            else { (*failed)++; LOGE("mods: copy of %s failed (error %lu)", Rel(s).c_str(), GetLastError()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    h = FindFirstFileW((dst + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::vector<std::pair<std::wstring, bool>> stale;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring lower = n;
            for (auto& c : lower) c = (wchar_t)towlower(c);
            if (!keep.count(lower)) stale.push_back({dst + L"\\" + n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (auto& [p, isDir] : stale) {
            if (isDir) RemoveTree(p);
            else DeleteFileW(p.c_str());
        }
    }
    return copied;
}

static std::set<std::wstring> ReadManaged(const std::wstring& file) {
    std::set<std::wstring> out;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] != '#') out.insert(Widen(line));
    }
    return out;
}

static void WriteManaged(const std::wstring& file, const std::set<std::wstring>& names) {
    if (names.empty()) { DeleteFileW(file.c_str()); return; }
    std::ofstream o(file, std::ios::trunc);
    o << "# Mod plugins installed by the Geronimo Mod Loader from GML\\plugins. It removes them when their\n"
         "# GML plugin goes away. Folders not listed here are never touched.\n";
    for (auto& n : names) o << Narrow(n) << "\n";
}

static bool InSet(const std::set<std::wstring>& s, const std::wstring& n) {
    for (auto& x : s) if (_wcsicmp(x.c_str(), n.c_str()) == 0) return true;
    return false;
}

void SyncPluginMods() {
    const std::wstring dst = g_paths.gameRoot + L"\\Mods";
    const std::wstring list = dst + L"\\.gml-managed";
    std::map<std::wstring, std::wstring> want;
    Collect(g_paths.plugins, want);
    std::set<std::wstring> managed = ReadManaged(list), now;

    for (auto& n : managed) {
        bool wanted = false;
        for (auto& [w, s] : want) if (_wcsicmp(w.c_str(), n.c_str()) == 0) wanted = true;
        if (wanted) continue;
        RemoveTree(dst + L"\\" + n);
        if (Exists(dst + L"\\" + n)) { LOGW("mods: could not remove %s", Narrow(n).c_str()); now.insert(n); }
        else LOGI("mods: removed %s (no plugin provides it any more)", Narrow(n).c_str());
    }

    int copied = 0, failed = 0;
    if (!want.empty()) CreateDirectoryW(dst.c_str(), nullptr);
    for (auto& [name, src] : want) {
        std::wstring target = dst + L"\\" + name;
        if (Exists(target) && !InSet(managed, name)) {
            LOGW("mods: Mods\\%s was installed by hand - leaving it alone (remove it to let %s provide it)",
                 Narrow(name).c_str(), Rel(src).c_str());
            continue;
        }
        int n = Mirror(src, target, &failed);
        copied += n;
        now.insert(name);
        if (n) LOGI("mods: %s -> Mods\\%s (%d file(s))", Rel(src).c_str(), Narrow(name).c_str(), n);
    }
    WriteManaged(list, now);
    if (now.empty()) RemoveDirectoryW(dst.c_str());  // only if nothing else is in it
    LOGI("mods: %zu mod plugin(s) managed, %d file(s) updated%s", now.size(), copied, failed ? ", some copies FAILED" : "");
}

}  // namespace gml
