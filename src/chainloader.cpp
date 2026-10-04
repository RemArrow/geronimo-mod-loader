// Chainloader: discovers patchers and plugins, resolves plugin load order, loads them.
//
// Like BepInEx, plugin metadata is read from the DLL *file* (the exported GML_PluginMetadata
// struct, which is plain data) without executing the plugin, so missing dependencies,
// duplicates and incompatibilities are resolved before any plugin code runs.
//
//   process entry : RunPatchers()        patchers\*.dll -> GML_Patch
//                   PrepareChainloader() plugins\**\*.dll metadata -> ordered load list
//   engine ready  : RunChainloader()     LoadLibrary + GML_Awake, in dependency order
#include "internal.h"
#include <algorithm>
#include <map>
#include <set>

namespace gml {

static std::vector<GML_Plugin*> s_order;  // resolved plugin load order
static std::vector<GML_Plugin*> s_patchers;
static std::vector<GML_Plugin*> s_notLoaded;  // skipped while resolving, or in a disabled folder
static std::vector<GML_Plugin*> s_all;        // patchers, s_order, s_notLoaded (for GetPluginState)

// ------------------------------------------------------------------ metadata from file

struct DllExports {
    bool hasMetadata = false, hasAwake = false, hasPatch = false;
    GML_PluginInfo info{};
};

static bool ReadExports(const std::wstring& path, DllExports& out) {
    // Map as an image (sections at their RVAs) without running DllMain or resolving imports.
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) return false;
    auto base = (uint8_t*)((uintptr_t)h & ~(uintptr_t)3);
    bool ok = false;
    __try {
        auto dos = (IMAGE_DOS_HEADER*)base;
        auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE &&
            nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) {
            auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (dir.VirtualAddress && dir.Size) {
                auto exp = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
                auto names = (uint32_t*)(base + exp->AddressOfNames);
                auto ords = (uint16_t*)(base + exp->AddressOfNameOrdinals);
                auto funcs = (uint32_t*)(base + exp->AddressOfFunctions);
                for (uint32_t i = 0; i < exp->NumberOfNames; i++) {
                    const char* n = (const char*)(base + names[i]);
                    uint32_t rva = funcs[ords[i]];
                    if (!strcmp(n, "GML_Awake")) out.hasAwake = true;
                    else if (!strcmp(n, "GML_Patch")) out.hasPatch = true;
                    else if (!strcmp(n, "GML_PluginMetadata") && rva + sizeof(uint32_t) <= nt->OptionalHeader.SizeOfImage) {
                        auto* info = (const GML_PluginInfo*)(base + rva);
                        size_t n2 = info->size < sizeof(GML_PluginInfo) ? info->size : sizeof(GML_PluginInfo);
                        if (rva + n2 <= nt->OptionalHeader.SizeOfImage && n2 >= offsetof(GML_PluginInfo, dependencies)) {
                            memcpy(&out.info, info, n2);
                            out.hasMetadata = true;
                        }
                    }
                }
            }
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    FreeLibrary(h);
    // Force NUL termination of every fixed-size string.
    auto term = [](char* s, size_t n) { s[n - 1] = 0; };
    term(out.info.guid, sizeof out.info.guid);
    term(out.info.name, sizeof out.info.name);
    term(out.info.version, sizeof out.info.version);
    for (auto& d : out.info.dependencies) { term(d.guid, sizeof d.guid); term(d.minVersion, sizeof d.minVersion); }
    for (auto& g : out.info.incompatibilities) term(g, sizeof g);
    return ok;
}

// "1.2.10" > "1.2.9"; missing components count as 0.
static int CompareVersions(const std::string& a, const std::string& b) {
    const char *pa = a.c_str(), *pb = b.c_str();
    for (int i = 0; i < 4; i++) {
        long x = strtol(pa, (char**)&pa, 10), y = strtol(pb, (char**)&pb, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*pa == '.') pa++;
        if (*pb == '.') pb++;
    }
    return 0;
}

static std::string Label(const GML_Plugin* p) { return "[" + p->name + " " + p->version + "]"; }

// All DLLs under dir, recursively. Folders that contain disabled.txt are skipped; their DLLs go to
// `disabled` instead, if given (so a mod menu can list them and turn them back on).
static void FindDlls(const std::wstring& dir, std::vector<std::wstring>& out, std::vector<std::wstring>* disabled = nullptr,
                     bool ignoreDisabled = false) {
    if (!ignoreDisabled && GetFileAttributesW((dir + L"\\disabled.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
        LOGI("skipping %s (disabled.txt)", Narrow(dir.substr(g_paths.gmlRoot.size() + 1)).c_str());
        if (disabled) FindDlls(dir, *disabled, nullptr, true);
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) subdirs.push_back(dir + L"\\" + n);
        else if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".dll") == 0) out.push_back(dir + L"\\" + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(subdirs.begin(), subdirs.end());
    for (auto& s : subdirs) FindDlls(s, out, disabled, ignoreDisabled);
}

static std::wstring DirOf(const std::wstring& p) { return p.substr(0, p.find_last_of(L'\\')); }
static std::string Stem(const std::wstring& p) {
    std::wstring n = p.substr(p.find_last_of(L'\\') + 1);
    return Narrow(n.substr(0, n.find_last_of(L'.')));
}

static GML_Plugin* MakePlugin(const std::wstring& path, const DllExports& ex, bool patcher) {
    auto* p = new GML_Plugin;
    p->path = path;
    p->dir = DirOf(path);
    p->patcher = patcher;
    if (ex.hasMetadata && ex.info.guid[0]) {
        p->info = ex.info;
        p->guid = ex.info.guid;
        p->name = ex.info.name[0] ? ex.info.name : ex.info.guid;
        p->version = ex.info.version[0] ? ex.info.version : "0.0.0";
    } else {
        p->guid = p->name = Stem(path);
        p->version = "0.0.0";
    }
    return p;
}

// ------------------------------------------------------------------ patchers

void RunPatchers() {
    std::vector<std::wstring> dlls;
    FindDlls(g_paths.patchers, dlls);
    for (auto& path : dlls) {
        DllExports ex;
        if (!ReadExports(path, ex) || !ex.hasPatch) continue;  // not a patcher (maybe a dependency)
        GML_Plugin* p = MakePlugin(path, ex, true);
        s_patchers.push_back(p);
        LOGI("Loading patcher %s", Label(p).c_str());
        p->module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto fn = p->module ? (GML_PatchFn)GetProcAddress(p->module, "GML_Patch") : nullptr;
        if (!fn) {
            LOGE("patcher %s could not be loaded (error %lu)", Label(p).c_str(), GetLastError());
            continue;
        }
        int rc = -1;
        if (GuardedCall(p, "GML_Patch", [&] { rc = fn(&g_api, p); }) && rc == 0) p->loaded = true;
        else if (!p->faulted) { p->faulted = true; LOGE("patcher %s returned %d - disabled", Label(p).c_str(), rc); }
        if (p->cfg) CfgSave(p->cfg);
    }
    if (!s_patchers.empty()) LOGM("%zu patcher(s) run", s_patchers.size());
}

// ------------------------------------------------------------------ plugins: resolve

void PrepareChainloader() {
    std::vector<std::wstring> dlls, disabled;
    FindDlls(g_paths.plugins, dlls, &disabled);
    for (auto& path : disabled) {  // listed (GetPluginState), never loaded
        DllExports ex;
        if (!ReadExports(path, ex) || !ex.hasAwake || !ex.hasMetadata || !ex.info.guid[0]) continue;
        GML_Plugin* p = MakePlugin(path, ex, false);
        p->disabled = true;
        s_notLoaded.push_back(p);
    }
    auto skip = [](GML_Plugin* p) {
        p->skipped = true;
        s_notLoaded.push_back(p);
    };

    std::map<std::string, GML_Plugin*> byGuid;  // one per GUID after de-duplication
    for (auto& path : dlls) {
        DllExports ex;
        if (!ReadExports(path, ex)) continue;
        if (!ex.hasAwake) {
            if (ex.hasMetadata) LOGW("%s has plugin metadata but no GML_Awake export - skipped", Narrow(path).c_str());
            continue;  // a helper DLL, not a plugin
        }
        if (!ex.hasMetadata || !ex.info.guid[0]) {
            LOGE("%s exports GML_Awake but no GML_PluginMetadata (use GML_PLUGIN(...)) - skipped", Narrow(path).c_str());
            continue;
        }
        GML_Plugin* p = MakePlugin(path, ex, false);
        if (p->info.apiVersion > GML_API_VERSION) {
            LOGE("Skipping %s: built for GML API v%u, this loader is v%d", Label(p).c_str(), p->info.apiVersion,
                 GML_API_VERSION);
            skip(p);
            continue;
        }
        auto it = byGuid.find(p->guid);
        if (it != byGuid.end()) {
            int cmp = CompareVersions(p->version, it->second->version);
            GML_Plugin* keep = cmp > 0 ? p : it->second;
            GML_Plugin* drop = cmp > 0 ? it->second : p;
            LOGW("Skipping %s because %s exists (%s) [%s]", Label(drop).c_str(),
                 cmp == 0 ? "a duplicate with the same GUID" : "a newer version", Narrow(keep->path).c_str(),
                 Narrow(drop->path).c_str());
            it->second = keep;
            skip(drop);
            continue;
        }
        byGuid[p->guid] = p;
    }

    // Incompatibilities (checked against every plugin found, as BepInEx does).
    for (auto it = byGuid.begin(); it != byGuid.end();) {
        GML_Plugin* p = it->second;
        std::string clash;
        for (auto& g : p->info.incompatibilities)
            if (g[0] && byGuid.count(g)) clash = g;
        if (!clash.empty()) {
            LOGE("Could not load %s because it is incompatible with %s", Label(p).c_str(),
                 Label(byGuid[clash]).c_str());
            skip(p);
            it = byGuid.erase(it);
        } else ++it;
    }

    // Hard dependencies: present and new enough. Repeat until stable (removals cascade).
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = byGuid.begin(); it != byGuid.end();) {
            GML_Plugin* p = it->second;
            std::string missing;
            for (auto& d : p->info.dependencies) {
                if (!d.guid[0] || (d.flags & GML_DEPENDENCY_SOFT)) continue;
                auto dep = byGuid.find(d.guid);
                if (dep == byGuid.end() || (d.minVersion[0] && CompareVersions(dep->second->version, d.minVersion) < 0))
                    missing += std::string(missing.empty() ? "" : ", ") + d.guid + (d.minVersion[0] ? std::string(" (v") + d.minVersion + " or newer)" : "");
            }
            if (!missing.empty()) {
                LOGE("Could not load %s because it has missing dependencies: %s", Label(p).c_str(), missing.c_str());
                skip(p);
                it = byGuid.erase(it);
                changed = true;
            } else ++it;
        }
    }

    // Topological sort: dependencies (hard and soft) first, otherwise by GUID.
    std::set<std::string> done, visiting;
    std::function<bool(GML_Plugin*)> visit = [&](GML_Plugin* p) -> bool {
        if (done.count(p->guid)) return true;
        if (visiting.count(p->guid)) {
            LOGE("Could not load %s because of a dependency cycle", Label(p).c_str());
            return false;
        }
        visiting.insert(p->guid);
        bool ok = true;
        for (auto& d : p->info.dependencies) {
            auto dep = byGuid.find(d.guid);
            if (d.guid[0] && dep != byGuid.end() && !visit(dep->second) && !(d.flags & GML_DEPENDENCY_SOFT)) ok = false;
        }
        visiting.erase(p->guid);
        if (ok) { done.insert(p->guid); s_order.push_back(p); }
        return ok;
    };
    for (auto& [g, p] : byGuid) visit(p);
    for (auto& [g, p] : byGuid)
        if (!done.count(g)) skip(p);  // in a dependency cycle

    s_all = s_patchers;
    s_all.insert(s_all.end(), s_order.begin(), s_order.end());
    s_all.insert(s_all.end(), s_notLoaded.begin(), s_notLoaded.end());
    LOGM("Chainloader ready: %zu plugin(s) to load", s_order.size());
}

// ------------------------------------------------------------------ plugins: load

void RunChainloader() {
    LOGM("Chainloader started");
    for (GML_Plugin* p : s_order) {
        std::string failedDep;
        for (auto& d : p->info.dependencies)
            if (d.guid[0] && !(d.flags & GML_DEPENDENCY_SOFT) && !IsPluginLoaded(d.guid)) failedDep = d.guid;
        if (!failedDep.empty()) {
            LOGE("Skipping %s because its dependency %s failed to load", Label(p).c_str(), failedDep.c_str());
            p->skipped = true;
            continue;
        }
        LOGI("Loading %s", Label(p).c_str());
        p->module = LoadLibraryExW(p->path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto awake = p->module ? (GML_AwakeFn)GetProcAddress(p->module, "GML_Awake") : nullptr;
        if (!awake) {
            LOGE("%s could not be loaded (error %lu)", Label(p).c_str(), GetLastError());
            p->faulted = true;
            continue;
        }
        int rc = -1;
        if (GuardedCall(p, "Awake", [&] { rc = awake(&g_api, p); }) && rc == 0) {
            p->loaded = true;
        } else if (!p->faulted) {
            p->faulted = true;
            LOGE("%s Awake returned %d - plugin disabled", Label(p).c_str(), rc);
        }
        if (p->cfg) CfgSave(p->cfg);  // write <GUID>.cfg with everything the plugin bound
    }
    size_t ok = std::count_if(s_order.begin(), s_order.end(), [](GML_Plugin* p) { return p->loaded; });
    LOGM("Chainloader startup complete: %zu of %zu plugin(s) loaded", ok, s_order.size());
}

const std::vector<GML_Plugin*>& AllPlugins() { return s_all; }

GML_Plugin* FindPlugin(const char* guid) {
    if (!guid) return nullptr;
    for (GML_Plugin* p : s_all)  // a loaded plugin wins over a skipped duplicate of it
        if (p->guid == guid && !p->skipped && !p->disabled) return p;
    for (GML_Plugin* p : s_all)
        if (p->guid == guid) return p;
    return nullptr;
}

bool IsPluginLoaded(const char* guid) {
    if (!guid) return false;
    for (GML_Plugin* p : s_order)
        if (p->loaded && p->guid == guid) return true;
    for (GML_Plugin* p : s_patchers)
        if (p->loaded && p->guid == guid) return true;
    return false;
}

}  // namespace gml
