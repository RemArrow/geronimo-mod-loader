// GML core (GML\core\GML.dll). Loaded by the doorstop (version.dll) at process entry, on the
// main thread, before the CRT and engine start; see src/doorstop/doorstop.cpp.
//
// Startup, mirroring BepInEx's preloader -> chainloader split:
//   process entry  GML_Bootstrap: config, log, ProcessEvent hook, pak sync, patchers,
//                  plugin discovery + dependency resolution (no plugin code runs)
//   engine ready   first game-thread ProcessEvent: chainloader loads plugins and calls Awake,
//                  then GML_EVENT_ENGINE_READY fires (events.cpp)
#include "ue.h"
#include <algorithm>

namespace gml {
namespace assets {
GUObject* ImportTexture(const wchar_t*);
GUObject* ImportStaticMesh(const wchar_t*, const GML_MeshImport*);
GUObject* CreateMaterialInstance(GUObject*);
int SetMaterialTexture(GUObject*, const char*, GUObject*);
int SetMaterialScalar(GUObject*, const char*, float);
int SetMaterialVector(GUObject*, const char*, const float*);
int ExecConsoleCommand(const char*);
}  // namespace assets

Config    g_cfg;
Paths     g_paths;
uintptr_t g_imageBase = 0;
DWORD     g_gameThreadId = 0;

static GML_Plugin s_self = [] {
    GML_Plugin p;
    p.guid = "GML";
    p.name = "GML";
    p.version = GML_VERSION_STRING;
    p.loaded = true;
    return p;
}();
GML_Plugin* LoaderSelf() { return &s_self; }

// ------------------------------------------------------------------ SEH guard

static DWORD s_faultCode = 0;
static int Filter(DWORD code) {
    s_faultCode = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool GuardedRaw(const std::function<void()>* fn) {
    __try {
        (*fn)();
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool GuardedCall(GML_Plugin* p, const char* what, const std::function<void()>& fn) {
    if (GuardedRaw(&fn)) return true;
    LOGE("[%s] %s raised exception 0x%08lX - its callbacks are now disabled", p ? p->name.c_str() : "?", what,
         s_faultCode);
    if (p) p->faulted = true;
    return false;
}

// ------------------------------------------------------------------ GML.cfg

static CfgFile* s_cfgFile = nullptr;

static void LoadConfig() {
    s_cfgFile = CfgOpen(g_paths.config + L"\\GML.cfg", "GML", GML_VERSION_STRING, "GML");
    CfgFile* f = s_cfgFile;
    auto b = [&](const char* sec, const char* key, GML_ConfigType t, const char* def, const char* desc) {
        return CfgBind(f, sec, key, t, def, desc);
    };
    g_cfg.console = CfgBool(b("Logging.Console", "Enabled", GML_CONFIG_BOOL, "false",
                              "Open a console window mirroring the log (handy on a flat screen)."));
    std::string lvl = CfgString(b("Logging.Disk", "LogLevel", GML_CONFIG_STRING, "Info",
                                  "Lowest level written to LogOutput.log: Debug, Info, Message, Warning, Error, Fatal."));
    static const char* names[] = {"debug", "info", "message", "warning", "error", "fatal"};
    std::transform(lvl.begin(), lvl.end(), lvl.begin(), ::tolower);
    for (int i = 0; i < 6; i++) if (lvl == names[i]) g_cfg.logLevel = (GML_LogLevel)i;
    g_cfg.timestamps = CfgBool(b("Logging.Disk", "Timestamps", GML_CONFIG_BOOL, "false",
                                 "Prefix each log line with the time of day."));
    g_cfg.syncPaks = CfgBool(b("Paks", "Sync", GML_CONFIG_BOOL, "true",
                               "Copy cooked containers (.utoc/.ucas/.pak) found under plugins\\ into\n"
                               "Content\\Paks\\~GML at launch, so plugins can ship them."));
    g_cfg.traceProcessEvent = CfgBool(b("Engine", "TraceProcessEvent", GML_CONFIG_BOOL, "false",
                                        "Log every distinct UFunction that passes through ProcessEvent, once each.\n"
                                        "Use it to find hook targets, then turn it off."));
    g_cfg.tickIntervalMs = (int)std::max<int64_t>(1, CfgInt(b("Engine", "TickIntervalMs", GML_CONFIG_INT, "8",
        "GML_EVENT_TICK follows the engine frame counter. This interval (ms) is only a fallback\n"
        "if the frame counter cannot be resolved.")));
    auto rva = [&](const char* key, uint32_t def, const char* what) {
        char d[16];
        snprintf(d, sizeof d, "0x%08X", def);
        std::string desc = std::string("RVA of ") + what + " in Geronimo-Win64-Shipping.exe (from Dumper-7,\n"
                           "CppSDK/SDK/Basic.hpp namespace Offsets). Verified against a signature at launch.";
        uint32_t v = (uint32_t)strtoul(CfgString(b("Offsets", key, GML_CONFIG_STRING, d, desc.c_str())).c_str(), nullptr, 0);
        return v ? v : def;
    };
    g_cfg.rvaGObjects = rva("GObjects", g_cfg.rvaGObjects, "GUObjectArray.ObjObjects");
    g_cfg.rvaAppendString = rva("AppendString", g_cfg.rvaAppendString, "FName::AppendString");
    g_cfg.rvaProcessEvent = rva("ProcessEvent", g_cfg.rvaProcessEvent, "UObject::ProcessEvent");
    CfgSave(f);
}

// ------------------------------------------------------------------ init

static std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

static void Init() {
    wchar_t exe[MAX_PATH], core[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Init, &self);
    GetModuleFileNameW(self, core, MAX_PATH);
    g_paths.exeDir = Parent(exe);
    g_paths.gameRoot = Parent(Parent(g_paths.exeDir));
    g_paths.core = Parent(core);
    g_paths.gmlRoot = Parent(g_paths.core);
    g_paths.config = g_paths.gmlRoot + L"\\config";
    g_paths.plugins = g_paths.gmlRoot + L"\\plugins";
    g_paths.patchers = g_paths.gmlRoot + L"\\patchers";
    g_paths.logFile = g_paths.gmlRoot + L"\\LogOutput.log";
    for (auto* d : {&g_paths.config, &g_paths.plugins, &g_paths.patchers}) CreateDirectoryW(d->c_str(), nullptr);

    LoadConfig();
    LogInit(g_paths.logFile, g_cfg.logLevel, g_cfg.console, g_cfg.timestamps);
    const wchar_t* exeName = wcsrchr(exe, L'\\');
    LOGM("Geronimo Mod Loader %s - %s", GML_VERSION_STRING, Narrow(exeName ? exeName + 1 : exe).c_str());
    LOGI("Running under Unreal Engine 5.7.4 (CL 51494982), API v%d", GML_API_VERSION);

    if (!EventsInit())
        LOGE("engine integration unavailable - patchers run, but plugins (which load at engine start) will not");
    if (g_cfg.syncPaks) SyncPluginPaks();
    RunPatchers();
    PrepareChainloader();
}

// ------------------------------------------------------------------ API table

namespace api {
using namespace ue;

static bool GT(const char* what) {
    if (GetCurrentThreadId() == g_gameThreadId && UEReady()) return true;
    LOGE("%s: must be called on the game thread after the engine is up", what);
    return false;
}
static bool Ready() { return UEReady(); }
static GML_Plugin* P(GML_Plugin* p) { return p ? p : LoaderSelf(); }

static void Log_(GML_Plugin* p, GML_LogLevel l, const char* s) { LogWrite(l, P(p)->name.c_str(), s ? s : ""); }
static const wchar_t* PluginDir(GML_Plugin* p) { return P(p)->dir.c_str(); }
static const char* PluginGUID(GML_Plugin* p) { return P(p)->guid.c_str(); }
static const char* PluginName(GML_Plugin* p) { return P(p)->name.c_str(); }
static const char* PluginVersion(GML_Plugin* p) { return P(p)->version.c_str(); }
static int IsPluginLoaded_(const char* g) { return IsPluginLoaded(g) ? 1 : 0; }
static const wchar_t* GetPath(GML_Path w) {
    switch (w) {
        case GML_PATH_GAME_ROOT: return g_paths.gameRoot.c_str();
        case GML_PATH_EXECUTABLE: return g_paths.exeDir.c_str();
        case GML_PATH_GML_ROOT: return g_paths.gmlRoot.c_str();
        case GML_PATH_CORE: return g_paths.core.c_str();
        case GML_PATH_CONFIG: return g_paths.config.c_str();
        case GML_PATH_PLUGINS: return g_paths.plugins.c_str();
        case GML_PATH_PATCHERS: return g_paths.patchers.c_str();
    }
    return L"";
}
static void* ImageBase() { return (void*)g_imageBase; }
static int UEReady_() { return UEReady() ? 1 : 0; }
static int IsGameThread() { return GetCurrentThreadId() == g_gameThreadId; }

static GML_ConfigEntry* ConfigBind(GML_Plugin* p, const char* sec, const char* key, GML_ConfigType t,
                                   const char* def, const char* desc) {
    p = P(p);
    if (p == LoaderSelf()) return nullptr;  // the loader's own file is not for plugins
    if (!p->cfg) p->cfg = CfgOpen(g_paths.config + L"\\" + Widen(p->guid) + L".cfg", p->name, p->version, p->guid);
    return CfgBind(p->cfg, sec, key, t, def, desc);
}
static int ConfigGetBool(GML_ConfigEntry* e) { return CfgBool(e) ? 1 : 0; }
static int64_t ConfigGetInt(GML_ConfigEntry* e) { return CfgInt(e); }
static double ConfigGetFloat(GML_ConfigEntry* e) { return CfgFloat(e); }
static int ConfigGetString(GML_ConfigEntry* e, char* b, int n) { return CopyOut(CfgString(e), b, n); }
static void ConfigSet(GML_ConfigEntry* e, const char* v) { CfgSet(e, v); }

static int HookNative(void* t, void* d, void** o) {
    std::string err;
    if (HookInstall(t, d, o, &err)) return 1;
    LOGE("HookNative(%p): %s", t, err.c_str());
    return 0;
}
static int UnhookNative(void* t) { return HookRemove(t) ? 1 : 0; }
static void* FindPattern_(const char* p) { return FindPattern(TextSection(g_imageBase), p); }
static void* ResolveRip(void* insn, int dispOff, int len) {
    return (uint8_t*)insn + len + *(int32_t*)((uint8_t*)insn + dispOff);
}

static int ObjectCount_() { return Ready() ? ObjectCount() : 0; }
static GUObject* ObjectAt_(int i) { return Ready() ? ObjectAt(i) : nullptr; }
static GUObject* FindObject_(const char* p) { return Ready() && p ? FindObject(p) : nullptr; }
static GUClass* FindClass_(const char* n) { return Ready() && n ? FindClass(n) : nullptr; }
static GUObject* FindFirstOf_(const char* c) { return Ready() && c ? FindFirstOf(FindClass(c)) : nullptr; }
static int FindAllOf_(const char* c, GUObject** out, int max) {
    if (!Ready() || !c) return 0;
    std::vector<GUObject*> v;
    FindAllOf(FindClass(c), v);
    for (int i = 0; i < max && i < (int)v.size(); i++) out[i] = v[i];
    return (int)v.size();
}
static GUObject* DefaultObject_(GUClass* c) { return Ready() ? DefaultObject(c) : nullptr; }
static GUObject* LoadObject_(const char* p) { return GT("LoadObject") && p ? LoadObject(p, false) : nullptr; }
static GUClass* LoadClass_(const char* p) { return GT("LoadClass") && p ? (GUClass*)LoadObject(p, true) : nullptr; }
static GUObject* NewObject_(GUClass* c, GUObject* o) { return GT("NewObject") && c ? NewObject(c, o) : nullptr; }
static void KeepAlive_(GUObject* o) { if (GT("KeepAlive")) KeepAlive(o); }
static int IsValid_(GUObject* o) { return Ready() && IsValid(o); }
static GUClass* GetClass(GUObject* o) { return o ? ClassOf(o) : nullptr; }
static GUObject* GetOuter(GUObject* o) { return o ? OuterOf(o) : nullptr; }
static GUStruct* GetSuper(GUStruct* s) { return s ? SuperOf(s) : nullptr; }
static int IsA_(GUObject* o, GUClass* c) { return IsA(o, c); }
static int GetName(GUObject* o, char* b, int n) { return CopyOut(ObjName(o), b, n); }
static int GetPathName(GUObject* o, char* b, int n) { return CopyOut(PathName(o), b, n); }
static int GetFullName(GUObject* o, char* b, int n) { return CopyOut(FullName(o), b, n); }

static uint64_t MakeName_(const char* s) { return GT("MakeName") && s ? MakeName(s) : 0; }
static int NameToString(uint64_t n, char* b, int c) { return CopyOut(NameStr(n), b, c); }
static int MakeText_(const char* s, void* out) { return GT("MakeText") && s && out ? MakeText(s, out) : 0; }
static int TextToString_(const void* t, char* b, int c) {
    return GT("TextToString") && t ? CopyOut(TextToString(t), b, c) : CopyOut("", b, c);
}
static int FStringToUtf8_(const GML_FString* s, char* b, int c) { return CopyOut(FStringToUtf8(s), b, c); }

static GUFunction* FindFunction_(GUStruct* s, const char* n) { return s && n ? FindFunction(s, n) : nullptr; }
static GFProperty* FindProperty_(GUStruct* s, const char* n) { return s && n ? FindProperty(s, n) : nullptr; }
static int GetPropertyInfo(GFProperty* p, GML_PropInfo* o) { return PropInfo(p, o); }
static GFProperty* NextProperty_(GUStruct* s, GFProperty* p) { return s ? NextProperty(s, p) : nullptr; }
static int GetPropertyName(GFProperty* p, char* b, int n) { return CopyOut(p ? PropName(p) : "", b, n); }
static int StructSize_(GUStruct* s) { return s ? StructSize(s) : 0; }
static void ProcessEvent_(GUObject* o, GUFunction* f, void* p) {
    if (GT("ProcessEvent") && o && f) ProcessEvent(o, f, p, true);
}
static int CallFunction(GUObject* o, const char* func, void* params) {
    if (!GT("CallFunction") || !o || !func) return 0;
    GUFunction* f = FindFunction((GUStruct*)ClassOf(o), func);
    if (!f) {
        LOGE("CallFunction: %s not found on %s", func, FullName(o).c_str());
        return 0;
    }
    std::vector<uint8_t> tmp;
    if (!params) { tmp.assign((size_t)StructSize((GUStruct*)f) + 16, 0); params = tmp.data(); }
    ProcessEvent(o, f, params, true);
    return 1;
}

static void* ArrayAdd_(GML_TArray* a, int es, int n) { return GT("ArrayAdd") ? ArrayAdd(a, es, n) : nullptr; }
static int MapForEach_(void* m, GFProperty* p, int (*cb)(void*, void*, void*), void* u) {
    return cb ? MapForEach(m, p, cb, u) : 0;
}
static void* EngineAlloc_(size_t n) { return GT("EngineAlloc") ? EngineAlloc(n) : nullptr; }

static GUObject* WorldContext_() { return Ready() ? WorldContext() : nullptr; }
}  // namespace api

const GML_API g_api = {
    GML_API_VERSION,
    sizeof(GML_API),
    api::Log_,
    api::PluginDir,
    api::PluginGUID,
    api::PluginName,
    api::PluginVersion,
    api::IsPluginLoaded_,
    api::GetPath,
    api::ImageBase,
    api::UEReady_,
    api::ConfigBind,
    api::ConfigGetBool,
    api::ConfigGetInt,
    api::ConfigGetFloat,
    api::ConfigGetString,
    api::ConfigSet,
    Subscribe,
    RunOnGameThread,
    api::IsGameThread,
    api::HookNative,
    api::UnhookNative,
    api::FindPattern_,
    api::ResolveRip,
    api::ObjectCount_,
    api::ObjectAt_,
    api::FindObject_,
    api::FindClass_,
    api::FindFirstOf_,
    api::FindAllOf_,
    api::DefaultObject_,
    api::LoadObject_,
    api::LoadClass_,
    api::NewObject_,
    api::KeepAlive_,
    api::IsValid_,
    api::GetClass,
    api::GetOuter,
    api::GetSuper,
    api::IsA_,
    api::GetName,
    api::GetPathName,
    api::GetFullName,
    api::MakeName_,
    api::NameToString,
    api::MakeText_,
    api::TextToString_,
    api::FStringToUtf8_,
    api::FindFunction_,
    api::FindProperty_,
    api::GetPropertyInfo,
    api::NextProperty_,
    api::GetPropertyName,
    api::StructSize_,
    api::ProcessEvent_,
    api::CallFunction,
    api::ArrayAdd_,
    api::MapForEach_,
    api::EngineAlloc_,
    HookFunctionSpec,
    HookUFunction,
    Unhook,
    assets::ImportTexture,
    assets::ImportStaticMesh,
    assets::CreateMaterialInstance,
    assets::SetMaterialTexture,
    assets::SetMaterialScalar,
    assets::SetMaterialVector,
    assets::ExecConsoleCommand,
    api::WorldContext_,
};

}  // namespace gml

// Called by the doorstop (version.dll). gameThreadId: the thread that will run the engine
// (the main thread when called from the patched entry point).
extern "C" __declspec(dllexport) void GML_Bootstrap(unsigned long gameThreadId) {
    static bool once = false;
    if (once) return;
    once = true;
    gml::g_gameThreadId = gameThreadId;
    gml::g_imageBase = (uintptr_t)GetModuleHandleW(nullptr);
    std::function<void()> init = gml::Init;
    if (!gml::GuardedRaw(&init)) LOGE("loader init crashed (0x%08lX) - continuing without mods", gml::s_faultCode);
}
