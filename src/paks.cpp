// Cooked containers (.utoc/.ucas/.pak/.sig) shipped by plugins, deployed at process entry - before the
// engine's pak platform file scans Content\Paks.
//
// 2.5: every plugin's containers live in ONE place, GML\plugins\Assets\:
//
//   GML\plugins (folder)
//     Assets\                         <- all paks live here (the loader owns this folder's layout)
//       .gml-owners                   <- which Assets\ subfolders the loader made (see step 3)
//       Patches\zzz_Patches_P.utoc    <- moved here from plugins\Patches\Paks\ (owner = "Patches")
//       MyHandMadePack\x.pak          <- put here by hand: never moved, never removed, always mounted
//       loose.pak                     <- directly in Assets\: always mounted
//     Patches (folder)
//       Patches.dll
//       Paks\zzz_Patches_P.utoc       <- "shipped separately": moved into Assets\Patches\ at launch
//
// Each launch:
//   1. Loose containers anywhere under plugins\ (outside Assets\) are MOVED into Assets\<owner>\, where
//      <owner> is the plugin's top-level folder under plugins\ (plugins\Patches\Paks\x.pak -> owner
//      "Patches"). A newer copy (a plugin update) replaces the one already there.
//      Skipped, as before: folders holding disabled.txt (left where they are, not mounted), and Unreal
//      mod plugin folders (<Name>.uplugin), whose containers go to <game>\Mods with them (mods.cpp).
//   2. Folders that held only those containers (e.g. an emptied Paks\) are removed.
//   3. Ownership: Assets\.gml-owners lists the subfolders the loader created by moving. If one's plugin
//      folder (plugins\<owner>) is gone, the plugin was uninstalled, so its Assets\<owner>\ is deleted -
//      the same "remove the plugin, its content goes too" rule as before. Folders made by hand aren't
//      listed and are never deleted.
//   4. Everything still in Assets\ is mirrored into <game>\Content\Paks\~GML\ (hard links when on the
//      same drive, so no second copy on disk), except the containers of a plugin whose folder holds
//      disabled.txt (plugins\<owner>\disabled.txt) or whose Assets\<owner>\ holds disabled.txt.
//   5. Anything in ~GML that no longer comes from Assets\ is deleted (~GML is the loader's).
//
// In this build Content\Paks containers deliver overrides of existing packages; containers declaring
// new package IDs mount but those packages do not register (see README "Known limitations") - ship new
// assets as a mod plugin instead (mods.cpp).
#include "internal.h"
#include <fstream>
#include <map>
#include <set>

namespace gml {

// The folder under plugins\ that holds every container. Reserved: a plugin can't be named this.
static const wchar_t kAssetsName[] = L"Assets";
// The list of Assets\ subfolders the loader created (one owner name per line).
static const wchar_t kOwnersFile[] = L".gml-owners";

// ------------------------------------------------------------------------------------------ helpers

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Is this file a cooked container part? (.pak/.utoc/.ucas, plus the .sig signature file)
static bool IsPakFile(const std::wstring& n) {
    size_t dot = n.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = n.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L".pak" || ext == L".utoc" || ext == L".ucas" || ext == L".sig";
}

// Same size and same last-write time = same file (good enough to skip a re-copy; a hard link matches
// its source exactly).
static bool SameFile(const std::wstring& a, const std::wstring& b) {
    WIN32_FILE_ATTRIBUTE_DATA fa, fb;
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (!GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb)) return false;
    return fa.nFileSizeHigh == fb.nFileSizeHigh && fa.nFileSizeLow == fb.nFileSizeLow &&
           CompareFileTime(&fa.ftLastWriteTime, &fb.ftLastWriteTime) == 0;
}

// Path relative to GML\ for log lines ("plugins\Assets\Patches\x.utoc").
static std::string Rel(const std::wstring& p) {
    return p.size() > g_paths.gmlRoot.size() ? Narrow(p.substr(g_paths.gmlRoot.size() + 1)) : Narrow(p);
}

static bool SameName(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

static std::wstring AssetsRoot() { return g_paths.plugins + L"\\" + kAssetsName; }

// Delete a folder and everything in it.
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
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);  // clear read-only so delete works
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Put file s at d: a hard link when both are on one drive (no extra disk space, no copy time), a copy
// otherwise. Deleting the link later leaves s alone. Same approach as mods.cpp.
static bool Place(const std::wstring& s, const std::wstring& d, bool* linked) {
    *linked = false;
    if (Exists(d)) {
        SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(d.c_str())) return CopyFileW(s.c_str(), d.c_str(), FALSE) != 0;  // in use: overwrite
    }
    if (CreateHardLinkW(d.c_str(), s.c_str(), nullptr)) return *linked = true;
    return CopyFileW(s.c_str(), d.c_str(), FALSE) != 0;
}

// ------------------------------------------------------------------------------- .gml-owners list

static std::set<std::wstring> ReadOwners() {
    std::set<std::wstring> out;
    std::ifstream in(AssetsRoot() + L"\\" + kOwnersFile);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();  // tolerate CRLF
        if (!line.empty() && line[0] != '#') out.insert(Widen(line));
    }
    return out;
}

static void WriteOwners(const std::set<std::wstring>& owners) {
    std::wstring file = AssetsRoot() + L"\\" + kOwnersFile;
    if (owners.empty()) { DeleteFileW(file.c_str()); return; }
    std::ofstream o(file, std::ios::trunc);
    o << "# Assets\\ folders the Geronimo Mod Loader made by moving a plugin's paks here. Each one is\n"
         "# deleted when plugins\\<name>\\ goes away. Folders not listed here are never touched.\n";
    for (auto& n : owners) o << Narrow(n) << "\n";
}

static bool InSet(const std::set<std::wstring>& s, const std::wstring& n) {
    for (auto& x : s) if (SameName(x, n)) return true;
    return false;
}

// ------------------------------------------------------------- step 1: find loose containers

struct Loose {
    std::wstring path;   // where it is now
    std::wstring owner;  // plugins\<owner>\..., or empty for a file directly in the plugins folder
};

// Walk dir (somewhere inside plugins\<owner>\) collecting container files.
static void FindLoose(const std::wstring& dir, const std::wstring& owner, std::vector<Loose>& out) {
    if (Exists(dir + L"\\disabled.txt")) return;  // disabled plugin: leave its paks where they are
    if (IsModPluginDir(dir)) return;              // an Unreal mod plugin: mods.cpp deploys it whole
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring full = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) subdirs.push_back(full);
        else if (IsPakFile(n)) out.push_back({full, owner});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& s : subdirs) FindLoose(s, owner, out);
}

// The top level of plugins\: each folder (except Assets\) is a plugin and names the owner.
static std::vector<Loose> FindAllLoose() {
    std::vector<Loose> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_paths.plugins + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring full = g_paths.plugins + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!SameName(n, kAssetsName)) FindLoose(full, n, out);  // Assets\ is the destination, not a source
        } else if (IsPakFile(n)) {
            out.push_back({full, L""});  // dropped straight into plugins\: no owner, goes to Assets\ root
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// ----------------------------------------------------------- step 1+2: move them into Assets

// Remove now-empty folders from `dir` upward, stopping at (and keeping) the plugin's own folder.
static void PruneEmpty(std::wstring dir, const std::wstring& keep) {
    while (dir.size() > keep.size() && !SameName(dir, keep)) {
        if (!RemoveDirectoryW(dir.c_str())) break;  // fails if not empty: stop there
        dir = dir.substr(0, dir.find_last_of(L'\\'));
    }
}

// Returns the owners that received files this launch; *movedFiles counts the files moved.
static std::set<std::wstring> MoveLooseIntoAssets(int* movedFiles) {
    *movedFiles = 0;
    std::set<std::wstring> owners;
    std::vector<Loose> loose = FindAllLoose();
    if (loose.empty()) return owners;
    CreateDirectoryW(AssetsRoot().c_str(), nullptr);

    for (auto& l : loose) {
        std::wstring destDir = l.owner.empty() ? AssetsRoot() : AssetsRoot() + L"\\" + l.owner;
        CreateDirectoryW(destDir.c_str(), nullptr);
        std::wstring name = l.path.substr(l.path.find_last_of(L'\\') + 1);
        std::wstring dest = destDir + L"\\" + name;

        if (Exists(dest)) SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL);  // let REPLACE_EXISTING work
        // REPLACE_EXISTING: a plugin update's newer pak replaces the old one.
        // COPY_ALLOWED: if plugins\ and Assets\ were ever on different drives, copy + delete instead.
        if (MoveFileExW(l.path.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            LOGI("paks: moved %s -> %s", Rel(l.path).c_str(), Rel(dest).c_str());
            (*movedFiles)++;
            if (!l.owner.empty()) owners.insert(l.owner);
            PruneEmpty(l.path.substr(0, l.path.find_last_of(L'\\')), g_paths.plugins + L"\\" + l.owner);
        } else {
            // Not fatal: the file stays where it is and simply isn't mounted this launch.
            LOGE("paks: could not move %s into Assets (error %lu)", Rel(l.path).c_str(), GetLastError());
        }
    }
    return owners;
}

// -------------------------------------------------------- step 3: drop uninstalled plugins' paks

static std::set<std::wstring> UpdateOwners(const std::set<std::wstring>& movedNow) {
    std::set<std::wstring> owners = ReadOwners();
    for (auto& o : movedNow) if (!InSet(owners, o)) owners.insert(o);

    std::set<std::wstring> keep;
    for (auto& o : owners) {
        std::wstring assetsDir = AssetsRoot() + L"\\" + o;
        if (!Exists(assetsDir)) continue;  // already gone: forget it
        if (Exists(g_paths.plugins + L"\\" + o)) { keep.insert(o); continue; }  // plugin still installed
        RemoveTree(assetsDir);
        if (Exists(assetsDir)) { LOGW("paks: could not remove %s", Rel(assetsDir).c_str()); keep.insert(o); }
        else LOGI("paks: removed %s (plugins\\%s is gone)", Rel(assetsDir).c_str(), Narrow(o).c_str());
    }
    WriteOwners(keep);
    return keep;
}

// ------------------------------------------------------------- step 4: what gets mounted

// Is the Assets\<sub>\ folder switched off? Either its plugin is disabled, or the folder itself is.
static bool AssetsDirDisabled(const std::wstring& sub) {
    return Exists(g_paths.plugins + L"\\" + sub + L"\\disabled.txt") ||
           Exists(AssetsRoot() + L"\\" + sub + L"\\disabled.txt");
}

// Collect every container under dir (any depth) into want: file name -> source path.
// ~GML is flat, so two files with the same name can't both be mounted; the later one wins, with a warning.
static void CollectAssets(const std::wstring& dir, bool top, std::map<std::wstring, std::wstring>& want) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring full = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (top && AssetsDirDisabled(n)) { LOGI("paks: %s is disabled - not mounted", Rel(full).c_str()); continue; }
            subdirs.push_back(full);
            continue;
        }
        if (!IsPakFile(n)) continue;
        auto it = want.find(n);
        if (it != want.end())
            LOGW("paks: %s is provided twice (%s and %s) - using the second", Narrow(n).c_str(),
                 Rel(it->second).c_str(), Rel(full).c_str());
        want[n] = full;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& s : subdirs) CollectAssets(s, false, want);
}

// --------------------------------------------------------------------------------- entry point

void SyncPluginPaks() {
    // Steps 1-3: tidy every container into Assets\ and forget uninstalled plugins'.
    int movedFiles = 0;
    std::set<std::wstring> moved = MoveLooseIntoAssets(&movedFiles);
    UpdateOwners(moved);

    // Step 4: what should be mounted.
    std::map<std::wstring, std::wstring> want;
    if (Exists(AssetsRoot())) CollectAssets(AssetsRoot(), true, want);

    const std::wstring dst = g_paths.gameRoot + L"\\Content\\Paks\\~GML";
    CreateDirectoryW(dst.c_str(), nullptr);

    // Step 5: remove stale files from ~GML first.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dst + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!want.count(fd.cFileName)) {
                std::wstring p = dst + L"\\" + fd.cFileName;
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (DeleteFileW(p.c_str())) LOGI("paks: removed stale %s", Narrow(fd.cFileName).c_str());
                else LOGW("paks: could not remove %s (error %lu)", Narrow(fd.cFileName).c_str(), GetLastError());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Step 4 (cont.): place each wanted file in ~GML unless it's already up to date.
    int placed = 0;
    for (auto& [name, src] : want) {
        std::wstring target = dst + L"\\" + name;
        if (SameFile(src, target)) continue;
        bool linked = false;
        if (Place(src, target, &linked)) {
            placed++;
            LOGI("paks: %s %s -> Content\\Paks\\~GML", linked ? "linked" : "copied", Rel(src).c_str());
        } else {
            LOGE("paks: copy of %s failed (error %lu)", Rel(src).c_str(), GetLastError());
        }
    }
    LOGI("paks: %zu file(s) in plugins\\Assets mounted, %d moved in, %d updated", want.size(), movedFiles, placed);
}

}  // namespace gml
