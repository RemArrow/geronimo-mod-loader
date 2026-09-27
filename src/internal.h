// Loader-internal declarations shared across the core's translation units.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

#include "GML/GML.h"

#define GML_VERSION_STRING "2.0.0"

namespace gml {

// ---------------------------------------------------------------- log.cpp
// BepInEx line format: "[Level  :    Source] message"
void LogInit(const std::wstring& path, GML_LogLevel minLevel, bool console, bool timestamps);
void LogWrite(GML_LogLevel lvl, const char* source, const char* msg);
void Log(GML_LogLevel lvl, const char* fmt, ...);  // source "GML"
#define LOGD(...) ::gml::Log(GML_LOG_DEBUG,   __VA_ARGS__)
#define LOGI(...) ::gml::Log(GML_LOG_INFO,    __VA_ARGS__)
#define LOGM(...) ::gml::Log(GML_LOG_MESSAGE, __VA_ARGS__)
#define LOGW(...) ::gml::Log(GML_LOG_WARNING, __VA_ARGS__)
#define LOGE(...) ::gml::Log(GML_LOG_ERROR,   __VA_ARGS__)

std::string  Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
int CopyOut(const std::string& s, char* buf, int cap); // returns s.size()

// ---------------------------------------------------------------- config.cpp (BepInEx .cfg)
struct CfgFile;
CfgFile*         CfgOpen(const std::wstring& path, const std::string& ownerName, const std::string& ownerVersion,
                         const std::string& ownerGuid);
GML_ConfigEntry* CfgBind(CfgFile* f, const char* section, const char* key, GML_ConfigType type, const char* def,
                         const char* desc);
void             CfgSave(CfgFile* f);  // writes only if the content changed
bool             CfgBool(GML_ConfigEntry* e);
int64_t          CfgInt(GML_ConfigEntry* e);
double           CfgFloat(GML_ConfigEntry* e);
std::string      CfgString(GML_ConfigEntry* e);
void             CfgSet(GML_ConfigEntry* e, const char* value);  // saves

// ---------------------------------------------------------------- core.cpp
struct Config {  // GML\config\GML.cfg
    GML_LogLevel logLevel = GML_LOG_INFO;
    bool         console = false;
    bool         timestamps = false;
    bool         syncPaks = true;
    int          tickIntervalMs = 8;  // fallback only; TICK normally follows GFrameCounter
    bool         traceProcessEvent = false;
    uint32_t     rvaGObjects     = 0x09929AE0;
    uint32_t     rvaAppendString = 0x01334100;
    uint32_t     rvaProcessEvent = 0x01503A90;
};

struct Paths {
    std::wstring gameRoot;  // ...\Geronimo
    std::wstring exeDir;    // ...\Geronimo\Binaries\Win64
    std::wstring gmlRoot;   // ...\Win64\GML
    std::wstring core, config, plugins, patchers;
    std::wstring logFile;
};

extern Config    g_cfg;
extern Paths     g_paths;
extern uintptr_t g_imageBase;
extern DWORD     g_gameThreadId;
extern const GML_API g_api;

}  // namespace gml

// A loaded (or loadable) plugin or patcher. Opaque to plugins.
struct GML_Plugin {
    std::string    guid, name, version;
    std::wstring   dir, path;          // folder and full path of the DLL
    HMODULE        module = nullptr;
    bool           patcher = false;
    bool           loaded = false;     // Awake/Patch returned 0
    bool           faulted = false;    // callbacks disabled (SEH fault or non-zero return)
    gml::CfgFile*  cfg = nullptr;      // <GUID>.cfg, opened on first ConfigBind
    GML_PluginInfo info{};
};

namespace gml {

// Invoke plugin code, containing SEH faults so one bad plugin doesn't take the game down.
bool GuardedCall(GML_Plugin* p, const char* what, const std::function<void()>& fn);
GML_Plugin* LoaderSelf();  // the loader's own handle ("GML")

// ---------------------------------------------------------------- chainloader.cpp
void RunPatchers();          // process entry
void PrepareChainloader();   // process entry: discover + resolve plugins (no plugin code runs)
void RunChainloader();       // engine ready: load + Awake in dependency order
bool IsPluginLoaded(const char* guid);

// ---------------------------------------------------------------- hook.cpp
bool HookInstall(void* target, void* detour, void** original, std::string* err = nullptr);
bool HookRemove(void* target);
size_t InsnLength(const uint8_t* p); // 0 = unsupported; exported for self-tests

// ---------------------------------------------------------------- scan.cpp
struct Section { uint8_t* base; size_t size; };
Section   TextSection(uintptr_t module);
uint8_t*  FindPattern(const Section& s, const char* ida, int* matches = nullptr);
bool      BytesMatch(const uint8_t* at, const char* ida);

// ---------------------------------------------------------------- events.cpp
bool EventsInit();                 // resolves offsets, installs ProcessEvent hook (process entry)
void Subscribe(GML_Plugin* p, GML_Event ev, GML_EventFn fn, void* user);
void RunOnGameThread(GML_Plugin* p, GML_TaskFn fn, void* user);
bool UEReady();
int  HookFunctionSpec(GML_Plugin* p, const char* spec, GML_HookFn pre, GML_HookFn post, void* user);
int  HookUFunction(GML_Plugin* p, GUFunction* fn, GML_HookFn pre, GML_HookFn post, void* user);
void Unhook(int id);

// ---------------------------------------------------------------- paks.cpp
void SyncPluginPaks();

}  // namespace gml
