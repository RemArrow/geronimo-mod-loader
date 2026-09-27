#include "internal.h"

namespace gml {

Section TextSection(uintptr_t module) {
    auto dos = (IMAGE_DOS_HEADER*)module;
    auto nt = (IMAGE_NT_HEADERS64*)(module + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) == 0)
            return {(uint8_t*)(module + sec->VirtualAddress), sec->Misc.VirtualSize};
    }
    return {nullptr, 0};
}

// "48 8B ?? ?? 89" -> bytes + mask
static bool Parse(const char* ida, std::vector<int>& out) {
    out.clear();
    for (const char* p = ida; *p;) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { out.push_back(-1); p++; if (*p == '?') p++; continue; }
        char* end;
        long v = strtol(p, &end, 16);
        if (end == p || v < 0 || v > 255) return false;
        out.push_back((int)v);
        p = end;
    }
    return !out.empty();
}

static bool MatchRaw(const uint8_t* at, const int* pat, size_t n) {
    __try {
        for (size_t i = 0; i < n; i++)
            if (pat[i] >= 0 && at[i] != (uint8_t)pat[i]) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool BytesMatch(const uint8_t* at, const char* ida) {
    std::vector<int> pat;
    return Parse(ida, pat) && MatchRaw(at, pat.data(), pat.size());
}

uint8_t* FindPattern(const Section& s, const char* ida, int* matches) {
    std::vector<int> pat;
    if (matches) *matches = 0;
    if (!s.base || !Parse(ida, pat) || pat.size() > s.size) return nullptr;
    // Anchor on the first non-wildcard byte for speed.
    size_t anchor = 0;
    while (anchor < pat.size() && pat[anchor] < 0) anchor++;
    if (anchor == pat.size()) return nullptr;
    const uint8_t first = (uint8_t)pat[anchor];

    uint8_t* found = nullptr;
    const size_t last = s.size - pat.size();
    for (size_t i = 0; i <= last;) {
        const void* hit = memchr(s.base + i + anchor, first, last - i + 1);
        if (!hit) break;
        size_t start = (const uint8_t*)hit - s.base - anchor;
        bool ok = true;
        for (size_t k = 0; k < pat.size(); k++)
            if (pat[k] >= 0 && s.base[start + k] != (uint8_t)pat[k]) { ok = false; break; }
        if (ok) {
            if (!found) found = s.base + start;
            if (!matches) return found;
            ++*matches;
        }
        i = start + 1;
    }
    return found;
}

}  // namespace gml
