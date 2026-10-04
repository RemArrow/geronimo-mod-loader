// BepInEx-format configuration files (.cfg).
//
//   ## Settings file was created by plugin Hello GML v1.0.0
//   ## Plugin GUID: com.example.hellogml
//
//   [General]
//
//   ## Log every level start.
//   # Setting type: Boolean
//   # Default value: true
//   LogLevelStarts = true
//
// Values the user edited are kept; descriptions/defaults are regenerated from the Bind calls.
// Keys present in the file but not (yet) bound are kept as-is ("orphans"), as BepInEx does.
// Difference from BepInEx: strings are written raw (no backslash escaping), so Windows paths
// read naturally.
#include "internal.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

struct GML_ConfigEntry {
    gml::CfgFile*  file;
    std::string    section, key, def, desc, value;
    GML_ConfigType type;
};

namespace gml {

struct CfgFile {
    std::wstring path;
    std::string ownerName, ownerVersion, ownerGuid;
    std::mutex mtx;
    std::vector<GML_ConfigEntry*> bound;                  // in bind order
    std::map<std::pair<std::string, std::string>, std::string> loaded;  // section,key -> value from disk
};

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static const char* TypeName(GML_ConfigType t) {
    switch (t) {
        case GML_CONFIG_BOOL: return "Boolean";
        case GML_CONFIG_INT: return "Int32";
        case GML_CONFIG_FLOAT: return "Single";
        default: return "String";
    }
}

// Canonical text for a value of the given type; falls back to `fallback` if unparseable.
static std::string Normalize(GML_ConfigType t, const std::string& v, const std::string& fallback) {
    std::string s = Trim(v);
    switch (t) {
        case GML_CONFIG_BOOL: {
            std::string l = s;
            std::transform(l.begin(), l.end(), l.begin(), ::tolower);
            if (l == "true" || l == "1" || l == "yes") return "true";
            if (l == "false" || l == "0" || l == "no") return "false";
            return fallback;
        }
        case GML_CONFIG_INT: {
            char* end = nullptr;
            long long x = strtoll(s.c_str(), &end, 0);
            return (end && *end == 0 && !s.empty()) ? std::to_string(x) : fallback;
        }
        case GML_CONFIG_FLOAT: {
            char* end = nullptr;
            double x = strtod(s.c_str(), &end);
            if (!end || *end != 0 || s.empty()) return fallback;
            char buf[64];
            snprintf(buf, sizeof buf, "%g", x);
            return buf;
        }
        default: return v;
    }
}

static void Load(CfgFile* f) {
    std::ifstream in(f->path, std::ios::binary);
    if (!in) return;
    std::string line, section;
    while (std::getline(in, line)) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') { section = Trim(t.substr(1, t.size() - 2)); continue; }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        f->loaded[{section, Trim(t.substr(0, eq))}] = Trim(t.substr(eq + 1));
    }
}

CfgFile* CfgOpen(const std::wstring& path, const std::string& ownerName, const std::string& ownerVersion,
                 const std::string& ownerGuid) {
    auto* f = new CfgFile{path, ownerName, ownerVersion, ownerGuid};
    Load(f);
    return f;
}

GML_ConfigEntry* CfgBind(CfgFile* f, const char* section, const char* key, GML_ConfigType type, const char* def,
                         const char* desc) {
    if (!f || !section || !key) return nullptr;
    std::lock_guard g(f->mtx);
    for (auto* e : f->bound)
        if (e->section == section && e->key == key) return e;  // BepInEx: binding twice returns the same entry
    auto* e = new GML_ConfigEntry{f, section, key, "", desc ? desc : "", "", type};
    e->def = Normalize(type, def ? def : "", type == GML_CONFIG_BOOL ? "false" : type == GML_CONFIG_STRING ? "" : "0");
    auto it = f->loaded.find({section, key});
    e->value = it != f->loaded.end() ? Normalize(type, it->second, e->def) : e->def;
    f->bound.push_back(e);
    return e;
}

static std::string Render(CfgFile* f) {
    std::ostringstream o;
    o << "## Settings file was created by plugin " << f->ownerName << " v" << f->ownerVersion << "\n";
    o << "## Plugin GUID: " << f->ownerGuid << "\n";

    std::vector<std::string> sections;
    for (auto* e : f->bound)
        if (std::find(sections.begin(), sections.end(), e->section) == sections.end()) sections.push_back(e->section);
    for (auto& [k, v] : f->loaded)
        if (std::find(sections.begin(), sections.end(), k.first) == sections.end()) sections.push_back(k.first);
    std::sort(sections.begin(), sections.end());

    for (auto& s : sections) {
        o << "\n[" << s << "]\n";
        for (auto* e : f->bound) {
            if (e->section != s) continue;
            o << "\n";
            std::istringstream d(e->desc);
            for (std::string l; std::getline(d, l);) o << "## " << l << "\n";
            o << "# Setting type: " << TypeName(e->type) << "\n";
            o << "# Default value: " << e->def << "\n";
            o << e->key << " = " << e->value << "\n";
        }
        for (auto& [k, v] : f->loaded) {  // orphans: in the file, not bound by anyone
            if (k.first != s) continue;
            bool isBound = false;
            for (auto* e : f->bound) isBound |= (e->section == k.first && e->key == k.second);
            if (!isBound) o << "\n" << k.second << " = " << v << "\n";
        }
    }
    return o.str();
}

void CfgSave(CfgFile* f) {
    if (!f) return;
    std::lock_guard g(f->mtx);
    if (f->bound.empty()) return;
    std::string text = Render(f);
    std::ifstream in(f->path, std::ios::binary);
    std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (old == text) return;
    std::ofstream out(f->path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) LOGE("could not write %s", Narrow(f->path).c_str());
}

bool CfgBool(GML_ConfigEntry* e) { return e && e->value == "true"; }
int64_t CfgInt(GML_ConfigEntry* e) { return e ? strtoll(e->value.c_str(), nullptr, 0) : 0; }
double CfgFloat(GML_ConfigEntry* e) { return e ? strtod(e->value.c_str(), nullptr) : 0.0; }
std::string CfgString(GML_ConfigEntry* e) { return e ? e->value : ""; }

int CfgCount(CfgFile* f) {
    if (!f) return 0;
    std::lock_guard g(f->mtx);
    return (int)f->bound.size();
}

GML_ConfigEntry* CfgAt(CfgFile* f, int index) {
    if (!f) return nullptr;
    std::lock_guard g(f->mtx);
    return index >= 0 && index < (int)f->bound.size() ? f->bound[index] : nullptr;
}

bool CfgInfo(GML_ConfigEntry* e, GML_ConfigInfo* out) {
    if (!e || !out || out->size < sizeof(GML_ConfigInfo)) return false;
    out->section = e->section.c_str();  // entries live for the process; these strings never change
    out->key = e->key.c_str();
    out->type = e->type;
    out->defaultValue = e->def.c_str();
    out->description = e->desc.c_str();
    return true;
}

void CfgSet(GML_ConfigEntry* e, const char* value) {
    if (!e || !value) return;
    {
        std::lock_guard g(e->file->mtx);
        e->value = Normalize(e->type, value, e->value);
        e->file->loaded[{e->section, e->key}] = e->value;
    }
    CfgSave(e->file);
}

}  // namespace gml
