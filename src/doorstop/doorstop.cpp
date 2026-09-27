// Doorstop: the version.dll that sits next to Geronimo-Win64-Shipping.exe. The GML equivalent
// of BepInEx's UnityDoorstop (winhttp.dll): it does as little as possible and hands over to the
// real loader, GML\core\GML.dll, so the loader can be updated without touching the game folder.
//
//   1. Forwards all 17 version.dll exports to %SystemRoot%\System32\version.dll.
//   2. Reads doorstop_config.ini. Only activates in the configured process.
//   3. version.dll is a static import of the game exe, so DllMain runs under the loader lock
//      before any game code. It only patches the exe's entry point; the patched entry runs
//      with the lock released, loads GML.dll and calls GML_Bootstrap - still before the CRT
//      and engine start - then continues into the real entry point.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cwchar>

// ------------------------------------------------------------------ version.dll forwarding
// All version.dll exports take only integer/pointer arguments (at most 8), so one 8-argument
// pass-through is ABI-correct on x64: unused argument slots are read and ignored.

using Fwd = uintptr_t(WINAPI*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                               uintptr_t, uintptr_t);

static HMODULE RealVersion() {
    static HMODULE h = [] {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\version.dll");
        return LoadLibraryW(path);
    }();
    return h;
}

#define FORWARD(name)                                                                              \
    extern "C" uintptr_t WINAPI GML_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,  \
                                           uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) { \
        static Fwd real = (Fwd)GetProcAddress(RealVersion(), #name);                               \
        return real ? real(a, b, c, d, e, f, g, h) : 0;                                            \
    }

FORWARD(GetFileVersionInfoA)
FORWARD(GetFileVersionInfoByHandle)
FORWARD(GetFileVersionInfoExA)
FORWARD(GetFileVersionInfoExW)
FORWARD(GetFileVersionInfoSizeA)
FORWARD(GetFileVersionInfoSizeExA)
FORWARD(GetFileVersionInfoSizeExW)
FORWARD(GetFileVersionInfoSizeW)
FORWARD(GetFileVersionInfoW)
FORWARD(VerFindFileA)
FORWARD(VerFindFileW)
FORWARD(VerInstallFileA)
FORWARD(VerInstallFileW)
FORWARD(VerLanguageNameA)
FORWARD(VerLanguageNameW)
FORWARD(VerQueryValueA)
FORWARD(VerQueryValueW)

// ------------------------------------------------------------------ doorstop

static wchar_t s_target[MAX_PATH];  // absolute path of GML.dll
static uint8_t* s_entry = nullptr;
static uint8_t  s_entrySaved[14];

static void Report(const wchar_t* msg) {
    // No log exists yet if the core cannot load; leave a note next to the exe.
    OutputDebugStringW(msg);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy_s(wcsrchr(path, L'\\') + 1, MAX_PATH - (wcsrchr(path, L'\\') + 1 - path), L"doorstop_error.log");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[1024];
        int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof buf, nullptr, nullptr);
        DWORD w;
        WriteFile(f, buf, n > 0 ? n - 1 : 0, &w, nullptr);
        CloseHandle(f);
    }
}

static void Boot(DWORD gameThreadId) {
    HMODULE core = LoadLibraryExW(s_target, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto boot = core ? (void(*)(unsigned long))GetProcAddress(core, "GML_Bootstrap") : nullptr;
    if (!boot) {
        wchar_t msg[MAX_PATH + 128];
        swprintf_s(msg, L"GML doorstop: could not load %s (error %lu)\r\n", s_target, GetLastError());
        Report(msg);
        return;
    }
    boot(gameThreadId);
}

static DWORD WINAPI EntryThunk(void* peb) {
    DWORD old;
    VirtualProtect(s_entry, sizeof s_entrySaved, PAGE_EXECUTE_READWRITE, &old);
    memcpy(s_entry, s_entrySaved, sizeof s_entrySaved);
    VirtualProtect(s_entry, sizeof s_entrySaved, old, &old);
    FlushInstructionCache(GetCurrentProcess(), s_entry, sizeof s_entrySaved);
    Boot(GetCurrentThreadId());  // the main thread is UE's game thread
    return ((DWORD(WINAPI*)(void*))s_entry)(peb);
}

static bool PatchEntry() {
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    s_entry = (uint8_t*)base + nt->OptionalHeader.AddressOfEntryPoint;
    DWORD old;
    if (!VirtualProtect(s_entry, sizeof s_entrySaved, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(s_entrySaved, s_entry, sizeof s_entrySaved);
    uint8_t jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; dq EntryThunk
    *(uint64_t*)(jmp + 6) = (uint64_t)&EntryThunk;
    memcpy(s_entry, jmp, sizeof jmp);
    VirtualProtect(s_entry, sizeof s_entrySaved, old, &old);
    FlushInstructionCache(GetCurrentProcess(), s_entry, sizeof s_entrySaved);
    return true;
}

// Injected late (e.g. by a launcher): UE's game thread is the process's oldest thread.
static DWORD OldestThread() {
    DWORD best = 0;
    ULONGLONG bestTime = ~0ull;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(th, &c, &e, &k, &u)) {
            ULONGLONG t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
            if (t < bestTime) { bestTime = t; best = te.th32ThreadID; }
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return best;
}

// doorstop_config.ini next to this DLL:
//   [General]
//   enabled = true
//   target  = GML\core\GML.dll          (relative to this DLL's folder, or absolute)
//   process = Geronimo-Win64-Shipping.exe
static bool ShouldActivate(HMODULE self) {
    wchar_t dir[MAX_PATH], ini[MAX_PATH], exe[MAX_PATH], v[MAX_PATH];
    GetModuleFileNameW(self, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = 0;
    swprintf_s(ini, L"%s\\doorstop_config.ini", dir);

    GetPrivateProfileStringW(L"General", L"enabled", L"true", v, MAX_PATH, ini);
    if (_wcsicmp(v, L"true") != 0 && wcscmp(v, L"1") != 0) return false;

    GetPrivateProfileStringW(L"General", L"process", L"Geronimo-Win64-Shipping.exe", v, MAX_PATH, ini);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    if (_wcsicmp(name ? name + 1 : exe, v) != 0) return false;

    GetPrivateProfileStringW(L"General", L"target", L"GML\\core\\GML.dll", v, MAX_PATH, ini);
    if (v[0] && v[1] == L':') wcscpy_s(s_target, v);
    else swprintf_s(s_target, L"%s\\%s", dir, v);
    return true;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    if (!ShouldActivate(inst)) return TRUE;  // plain version.dll proxy
    if (reserved) {                          // static import: we run before the game's entry point
        PatchEntry();
    } else {
        static DWORD tid = OldestThread();
        CreateThread(nullptr, 0, [](void*) -> DWORD { Boot(tid); return 0; }, nullptr, 0, nullptr);
    }
    return TRUE;
}
