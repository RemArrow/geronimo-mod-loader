// gmlcheck - offline validation of the loader against the game executable on disk.
// No game launch (and no headset) needed.
//
//   gmlcheck [exe] [ini]          verify offsets + signatures, prologue decode, hook self-test
//   gmlcheck lde <exe> <rva> <n>  print n decoded instruction lengths from rva (diff vs dumpbin)
//   gmlcheck coverage <exe>       decode every function prologue listed in .pdata
#include "../src/internal.h"
#include "../src/signatures.h"
#include <cstdio>

using namespace gml;

static const wchar_t* kDefaultExe =
    L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\GERONIMO\\Geronimo\\Binaries\\Win64\\"
    L"Geronimo-Win64-Shipping.exe";

static int s_fail = 0;
static void Check(bool ok, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf(ok ? "  [ OK ] " : "  [FAIL] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) s_fail++;
}

static uintptr_t MapImage(const wchar_t* exe) {
    HMODULE h = LoadLibraryExW(exe, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) {
        wprintf(L"cannot map %s (error %lu)\n", exe, GetLastError());
        return 0;
    }
    return (uintptr_t)h & ~(uintptr_t)3;  // resource-mapped handles are tagged in the low bits
}

static uint32_t IniHex(const wchar_t* ini, const wchar_t* key, uint32_t def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Offsets", key, L"", buf, 64, ini);
    return buf[0] ? (uint32_t)wcstoul(buf, nullptr, 0) : def;
}

static void CheckFunction(uintptr_t base, const char* name, uint32_t rva, const char* sig) {
    printf("%s (configured +0x%X)\n", name, rva);
    Check(BytesMatch((uint8_t*)base + rva, sig), "signature matches at the configured RVA");
    int n = 0;
    uint8_t* hit = FindPattern(TextSection(base), sig, &n);
    Check(n == 1, "signature is unique in .text (%d match%s)", n, n == 1 ? "" : "es");
    if (hit) Check((uint32_t)(hit - (uint8_t*)base) == rva, "unique match is at +0x%llX",
                   (unsigned long long)(hit - (uint8_t*)base));
    // The hook steals whole instructions covering >= 5 bytes.
    const uint8_t* p = (uint8_t*)base + rva;
    size_t total = 0;
    printf("         prologue:");
    while (total < 5) {
        size_t l = InsnLength(p + total);
        if (!l) break;
        printf(" %zu", l);
        total += l;
    }
    printf("\n");
    Check(total >= 5, "prologue decodes to %zu hookable bytes", total);
}

// ------------------------------------------------------------------ hook self-test

static volatile int s_detourHits = 0;
static int (*s_orig)(int, int) = nullptr;

__declspec(noinline) static int Target(int a, int b) {
    volatile int x = a * 7;
    for (int i = 0; i < b; i++) x += i ^ a;
    return x;
}
static int Detour(int a, int b) {
    s_detourHits++;
    return s_orig(a, b) + 1;
}

static void HookSelfTest() {
    printf("inline hook self-test\n");
    int before = Target(3, 10);
    std::string err;
    bool ok = HookInstall((void*)&Target, (void*)&Detour, (void**)&s_orig, &err);
    Check(ok, "install (%s)", ok ? "ok" : err.c_str());
    if (!ok) return;
    int during = Target(3, 10);
    Check(s_detourHits == 1 && during == before + 1, "detour runs and trampoline calls the original");
    Check(HookRemove((void*)&Target), "remove");
    Check(Target(3, 10) == before && s_detourHits == 1, "original restored");
}

// ------------------------------------------------------------------ modes

static int ModeLde(uintptr_t base, uint32_t rva, int n) {
    const uint8_t* p = (uint8_t*)base + rva;
    for (int i = 0; i < n; i++) {
        size_t l = InsnLength(p);
        printf("%llX %zu\n", 0x140000000ull + (unsigned long long)(p - (uint8_t*)base), l);
        if (!l) return 1;
        p += l;
    }
    return 0;
}

static int ModeCoverage(uintptr_t base) {
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto rf = (RUNTIME_FUNCTION*)(base + dir.VirtualAddress);
    size_t count = dir.Size / sizeof(RUNTIME_FUNCTION), ok = 0, fail = 0;
    uint32_t prev = ~0u;
    for (size_t i = 0; i < count; i++) {
        if (rf[i].BeginAddress == prev) continue;
        prev = rf[i].BeginAddress;
        // Chained unwind entries point mid-function; only count entries with a real prologue.
        const uint8_t* p = (uint8_t*)base + rf[i].BeginAddress;
        size_t total = 0;
        bool good = true;
        while (total < 5) {
            size_t l = InsnLength(p + total);
            if (!l) { good = false; break; }
            total += l;
        }
        good ? ok++ : fail++;
    }
    printf("prologue decode coverage: %zu / %zu functions (%.2f%%), %zu refused\n", ok, ok + fail,
           100.0 * ok / (double)(ok + fail), fail);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 5 && !wcscmp(argv[1], L"lde")) {
        uintptr_t base = MapImage(argv[2]);
        return base ? ModeLde(base, (uint32_t)wcstoul(argv[3], nullptr, 0), _wtoi(argv[4])) : 2;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"coverage")) {
        uintptr_t base = MapImage(argc >= 3 ? argv[2] : kDefaultExe);
        return base ? ModeCoverage(base) : 2;
    }

    const wchar_t* exe = argc >= 2 ? argv[1] : kDefaultExe;
    std::wstring ini = argc >= 3 ? argv[2] : L"";
    if (ini.empty()) {
        std::wstring e = exe;
        ini = e.substr(0, e.find_last_of(L'\\')) + L"\\GML\\config\\GML.cfg";  // BepInEx-format, INI-readable
    }
    uintptr_t base = MapImage(exe);
    if (!base) return 2;
    wprintf(L"exe: %s\nini: %s%s\n\n", exe, ini.c_str(),
            GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES ? L" (absent - using built-in offsets)" : L"");

    Config def;
    CheckFunction(base, "UObject::ProcessEvent", IniHex(ini.c_str(), L"ProcessEvent", def.rvaProcessEvent),
                  sig::ProcessEvent);
    CheckFunction(base, "FName::AppendString", IniHex(ini.c_str(), L"AppendString", def.rvaAppendString),
                  sig::AppendString);

    uint32_t gobj = IniHex(ini.c_str(), L"GObjects", def.rvaGObjects);
    printf("GObjects (configured +0x%X)\n", gobj);
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    const char* in = "?";
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (gobj >= sec->VirtualAddress && gobj < sec->VirtualAddress + sec->Misc.VirtualSize) in = (char*)sec->Name;
    Check(!strncmp(in, ".data", 5), "lies in a writable data section (%.8s) - contents validated at runtime", in);

    HookSelfTest();
    printf("\n%s\n", s_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return s_fail ? 1 : 0;
}
