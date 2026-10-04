/*
 * GML.h - Geronimo Mod Loader, plugin-facing C API (v2). Structured like BepInEx.
 *
 *   Binaries\Win64\
 *     version.dll            doorstop: loads GML\core\GML.dll at process entry
 *     doorstop_config.ini
 *     GML\
 *       core\                the loader
 *       config\              GML.cfg + one <GUID>.cfg per plugin (created by ConfigBind)
 *       patchers\            patcher DLLs: run at process entry, before the engine
 *       plugins\             plugin DLLs, any subfolder depth, plus their content (Paks\ ...)
 *       LogOutput.log
 *
 * PLUGIN (plugins\): a DLL that exports plugin metadata and an Awake function.
 *
 *     GML_EXPORT const GML_PluginInfo GML_PluginMetadata = { sizeof(GML_PluginInfo),
 *         GML_API_VERSION, "com.you.myplugin", "My Plugin", "1.0.0" };
 *     GML_EXPORT int GML_Awake(const GML_API* api, GML_Plugin* self);
 *
 *   The chainloader reads GML_PluginMetadata from the file WITHOUT running the DLL, sorts
 *   plugins by dependency, skips ones with missing/incompatible dependencies, then loads
 *   each one and calls GML_Awake - once the engine is up (UObjects exist), on the game
 *   thread, before GML_EVENT_ENGINE_READY fires. Return 0 for success.
 *   (GML.hpp: GML_PLUGIN(...) and GML_AWAKE() { ... }.)
 *
 * PATCHER (patchers\): a DLL exporting
 *
 *     GML_EXPORT int GML_Patch(const GML_API* api, GML_Plugin* self);
 *
 *   called at process entry, BEFORE the CRT and engine start - no UObjects yet. For native
 *   hooks that must be in place before engine code first runs. GML_Plugin metadata is
 *   optional for patchers. (GML.hpp: GML_PATCH() { ... }.)
 *
 * The API is a plain C function table so plugins can be built with any compiler.
 *
 * Threading: every function marked [GT] must run on the game thread. Awake, events and
 * hooks already run there; from anywhere else use RunOnGameThread.
 */
#ifndef GML_H
#define GML_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GML_API_VERSION 2
#define GML_EXPORT __declspec(dllexport)

/* Opaque engine handles. Pointer-compatible with the Dumper-7 SDK's
 * UObject / UClass / UFunction / UStruct / FProperty. */
typedef struct GUObject   GUObject;
typedef struct GUClass    GUClass;
typedef struct GUStruct   GUStruct;
typedef struct GUFunction GUFunction;
typedef struct GFProperty GFProperty;

typedef struct GML_Plugin      GML_Plugin;       /* per-plugin handle owned by the loader */
typedef struct GML_ConfigEntry GML_ConfigEntry;  /* one bound setting */

/* ------------------------------------------------------------------ plugin metadata */

#define GML_MAX_DEPENDENCIES      16
#define GML_MAX_INCOMPATIBILITIES 8

typedef enum GML_DependencyFlags {
    GML_DEPENDENCY_HARD = 0,  /* plugin is skipped if the dependency is missing or too old */
    GML_DEPENDENCY_SOFT = 1   /* only affects load order */
} GML_DependencyFlags;

typedef struct GML_Dependency {
    char     guid[128];
    char     minVersion[32];   /* "" = any */
    uint32_t flags;            /* GML_DependencyFlags */
} GML_Dependency;

/* Plain data with no pointers, so the chainloader can read it from the DLL file without
 * loading it. Export it as `GML_PluginMetadata`. Unused array slots stay zeroed. */
typedef struct GML_PluginInfo {
    uint32_t       size;         /* sizeof(GML_PluginInfo) */
    uint32_t       apiVersion;   /* GML_API_VERSION the plugin was built against */
    char           guid[128];    /* unique, reverse-domain style: "com.author.plugin" */
    char           name[128];
    char           version[32];  /* "major.minor.patch" */
    GML_Dependency dependencies[GML_MAX_DEPENDENCIES];
    char           incompatibilities[GML_MAX_INCOMPATIBILITIES][128]; /* GUIDs */
} GML_PluginInfo;

typedef int (*GML_AwakeFn)(const struct GML_API* api, GML_Plugin* self);
typedef int (*GML_PatchFn)(const struct GML_API* api, GML_Plugin* self);

/* ------------------------------------------------------------------ logging / events */

typedef enum GML_LogLevel {  /* same levels as BepInEx */
    GML_LOG_DEBUG   = 0,
    GML_LOG_INFO    = 1,
    GML_LOG_MESSAGE = 2,
    GML_LOG_WARNING = 3,
    GML_LOG_ERROR   = 4,
    GML_LOG_FATAL   = 5
} GML_LogLevel;

typedef enum GML_Event {
    /* UObject system is live; fires once, right after every plugin's Awake. data = NULL. */
    GML_EVENT_ENGINE_READY = 0,
    /* After UGameInstance::ReceiveInit. data = GUObject* game instance. */
    GML_EVENT_GAME_INSTANCE_INIT = 1,
    /* After a GameModeBase's ReceiveBeginPlay, i.e. a level started. data = GUObject* game mode. */
    GML_EVENT_WORLD_BEGIN_PLAY = 2,
    /* Game thread, once per engine frame (gated on GFrameCounter). data = float* delta seconds. */
    GML_EVENT_TICK = 3,
    GML_EVENT_COUNT
} GML_Event;

typedef void (*GML_EventFn)(void* user, void* data);
typedef void (*GML_TaskFn)(void* user);

/* ProcessEvent hook. params points at the UFunction's parameter block (layout =
 * the function's properties; see FindProperty/GetPropertyInfo).
 * Pre-hook: return non-zero to SKIP the original function. Post-hook: return is ignored. */
typedef int (*GML_HookFn)(void* user, GUObject* self, GUFunction* fn, void* params);

/* ------------------------------------------------------------------ paths / config */

typedef enum GML_Path {
    GML_PATH_GAME_ROOT = 0,  /* ...\GERONIMO\Geronimo (the UE project dir) */
    GML_PATH_EXECUTABLE,     /* ...\Binaries\Win64 */
    GML_PATH_GML_ROOT,       /* ...\Binaries\Win64\GML */
    GML_PATH_CORE,
    GML_PATH_CONFIG,
    GML_PATH_PLUGINS,
    GML_PATH_PATCHERS
} GML_Path;

typedef enum GML_ConfigType {  /* written to the .cfg as BepInEx's type names */
    GML_CONFIG_BOOL = 0,   /* Boolean */
    GML_CONFIG_INT,        /* Int32 */
    GML_CONFIG_FLOAT,      /* Single */
    GML_CONFIG_STRING      /* String */
} GML_ConfigType;

/* A setting as ConfigGetInfo describes it (2.3). The strings stay valid while the game runs. */
typedef struct GML_ConfigInfo {
    uint32_t       size;          /* = sizeof(GML_ConfigInfo) */
    const char*    section;
    const char*    key;
    GML_ConfigType type;
    const char*    defaultValue;
    const char*    description;
} GML_ConfigInfo;

typedef enum GML_PluginStatus {  /* (2.3) */
    GML_PLUGIN_LOADED = 0,   /* running */
    GML_PLUGIN_FAULTED,      /* its Awake failed or one of its callbacks raised an exception: its callbacks are off */
    GML_PLUGIN_PENDING,      /* found, not loaded yet (the engine isn't up) */
    GML_PLUGIN_SKIPPED,      /* not loaded: a dependency is missing, too old or failed, it is incompatible with
                                another plugin, a duplicate, or built for a newer GML (the log says which) */
    GML_PLUGIN_DISABLED      /* its folder holds disabled.txt */
} GML_PluginStatus;

typedef struct GML_PluginState {  /* (2.3) */
    uint32_t         size;        /* = sizeof(GML_PluginState) */
    const char*      guid;
    const char*      name;
    const char*      version;
    const wchar_t*   dir;         /* the folder holding its DLL */
    GML_PluginStatus status;      /* this launch */
    int              patcher;     /* 1 for a patcher */
    int              enabledNext; /* 1 unless disabled.txt is in its folder now: the state from the next launch on */
} GML_PluginState;

/* ------------------------------------------------------------------ engine data */

/* Describes one reflected property. `type` is the engine's field class name
 * ("ObjectProperty", "StructProperty", "BoolProperty", "ArrayProperty", ...). */
typedef struct GML_PropInfo {
    int32_t     offset;       /* byte offset inside the owning container */
    int32_t     size;         /* ElementSize */
    int32_t     arrayDim;
    uint64_t    flags;        /* EPropertyFlags */
    const char* type;         /* static lifetime */
    uint8_t     boolByteOffset, boolFieldMask; /* BoolProperty only */
    GUStruct*   structType;   /* StructProperty only */
    GUClass*    propertyClass;/* Object/Class property only */
    GFProperty* inner;        /* ArrayProperty / SetProperty element */
    GFProperty* key;          /* MapProperty */
    GFProperty* value;        /* MapProperty */
} GML_PropInfo;

/* Engine FString / TArray memory layout. Build an input FString by pointing Data at
 * your own NUL-terminated wchar_t buffer with Num = Max = wcslen+1. The engine only
 * reads input parameters, so your memory is never freed by it. */
typedef struct GML_FString { wchar_t* Data; int32_t Num; int32_t Max; } GML_FString;
typedef struct GML_TArray  { void*    Data; int32_t Num; int32_t Max; } GML_TArray;

typedef enum GML_Axis {
    GML_AXIS_UNREAL        = 0, /* OBJ is already X-forward, Z-up, left-handed, in cm */
    GML_AXIS_BLENDER_OBJ   = 1  /* Blender's default OBJ export (Y-up, -Z forward, metres) */
} GML_Axis;

typedef struct GML_MeshImport {
    uint32_t    size;          /* = sizeof(GML_MeshImport) */
    GML_Axis    axis;
    float       scale;         /* applied after axis conversion; 0 -> 1 (UNREAL) or 100 (BLENDER_OBJ) */
    float       offset[3];     /* added after scaling, in cm */
    int32_t     flipWinding;   /* -1 auto (no flip), 0 no, 1 yes */
    int32_t     buildCollision;/* build simple collision */
    /* Materials by OBJ `usemtl` name, parallel arrays; unmatched groups get defaultMaterial,
     * or the engine's DefaultMaterial when that is NULL. */
    int32_t     materialCount;
    const char* const* materialNames;
    GUObject* const*   materials;
    GUObject*   defaultMaterial;
} GML_MeshImport;

/* ------------------------------------------------------------------ the API table */

typedef struct GML_API {
    uint32_t version;   /* GML_API_VERSION the loader implements */
    uint32_t size;      /* sizeof(GML_API) in the loader; check before using newer fields */

    /* ---- loader, plugins, paths ---- */
    void           (*Log)(GML_Plugin* self, GML_LogLevel level, const char* utf8);
    const wchar_t* (*PluginDir)(GML_Plugin* self);       /* folder containing the plugin's DLL */
    const char*    (*PluginGUID)(GML_Plugin* self);
    const char*    (*PluginName)(GML_Plugin* self);
    const char*    (*PluginVersion)(GML_Plugin* self);
    int            (*IsPluginLoaded)(const char* guid);  /* 1 once that plugin's Awake succeeded */
    const wchar_t* (*GetPath)(GML_Path which);
    void*          (*ImageBase)(void);                   /* Geronimo-Win64-Shipping.exe base */
    int            (*UEReady)(void);                     /* 1 once the engine is up */

    /* ---- config: <GUID>.cfg in GML\config, BepInEx format ---- */
    /* Returns the entry; the file is created/updated with the default + description. */
    GML_ConfigEntry* (*ConfigBind)(GML_Plugin* self, const char* section, const char* key,
                                   GML_ConfigType type, const char* defaultValue, const char* description);
    int      (*ConfigGetBool)(GML_ConfigEntry* e);
    int64_t  (*ConfigGetInt)(GML_ConfigEntry* e);
    double   (*ConfigGetFloat)(GML_ConfigEntry* e);
    int      (*ConfigGetString)(GML_ConfigEntry* e, char* buf, int cap);  /* returns full length */
    void     (*ConfigSet)(GML_ConfigEntry* e, const char* value);         /* saves the file */

    /* ---- lifecycle ---- */
    void (*Subscribe)(GML_Plugin* self, GML_Event ev, GML_EventFn fn, void* user);
    void (*RunOnGameThread)(GML_Plugin* self, GML_TaskFn fn, void* user); /* runs at the next game-thread ProcessEvent */
    int  (*IsGameThread)(void);

    /* ---- native code ---- */
    /* Inline-hook any function in the process. *original receives a callable trampoline. */
    int   (*HookNative)(void* target, void* detour, void** original);
    int   (*UnhookNative)(void* target);
    /* IDA-style pattern ("48 8B ?? ?? 89"), searched in the game module's .text. */
    void* (*FindPattern)(const char* pattern);
    /* Resolve a RIP-relative operand: insn + insnLen + *(int32*)(insn + dispOffset). */
    void* (*ResolveRip)(void* insn, int dispOffset, int insnLen);

    /* ---- objects ---- */
    int         (*ObjectCount)(void);
    GUObject*   (*ObjectAt)(int index);
    GUObject*   (*FindObject)(const char* pathName);   /* "/Game/Pkg/Asset.Asset", "/Script/Engine.Actor" */
    GUClass*    (*FindClass)(const char* name);        /* "Actor", "GunAsset_C" or a full path */
    GUObject*   (*FindFirstOf)(const char* className); /* live instances incl. subclasses, no CDOs */
    int         (*FindAllOf)(const char* className, GUObject** out, int max); /* returns total found */
    GUObject*   (*DefaultObject)(GUClass* cls);
    GUObject*   (*LoadObject)(const char* path);       /* [GT] blocking load of a cooked asset */
    GUClass*    (*LoadClass)(const char* path);        /* [GT] e.g. "/Game/X/BP_Y.BP_Y_C" */
    GUObject*   (*NewObject)(GUClass* cls, GUObject* outer); /* [GT] non-actor, non-component */
    void        (*KeepAlive)(GUObject* obj);           /* [GT] protect from GC (via GameInstance.ReferencedObjects) */
    int         (*IsValid)(GUObject* obj);
    GUClass*    (*GetClass)(GUObject* obj);
    GUObject*   (*GetOuter)(GUObject* obj);
    GUStruct*   (*GetSuper)(GUStruct* s);
    int         (*IsA)(GUObject* obj, GUClass* cls);
    int         (*GetName)(GUObject* obj, char* buf, int cap);     /* all return full length */
    int         (*GetPathName)(GUObject* obj, char* buf, int cap);
    int         (*GetFullName)(GUObject* obj, char* buf, int cap); /* "Class /Path.Name" */

    /* ---- names, text, strings ---- */
    uint64_t (*MakeName)(const char* utf8);                        /* [GT] FName as its 8 raw bytes */
    int      (*NameToString)(uint64_t name, char* buf, int cap);
    int      (*MakeText)(const char* utf8, void* outFText);        /* [GT] writes a 16-byte FText */
    int      (*TextToString)(const void* ftext, char* buf, int cap); /* [GT] */
    int      (*FStringToUtf8)(const GML_FString* s, char* buf, int cap);

    /* ---- reflection ---- */
    GUFunction* (*FindFunction)(GUStruct* cls, const char* name);  /* walks supers */
    /* Walks supers. Also matches Blueprint-mangled names: "GunAssets" finds
     * "GunAssets_9_E68A935B442B9DB0A278E7B5893C9566". */
    GFProperty* (*FindProperty)(GUStruct* s, const char* name);
    int         (*GetPropertyInfo)(GFProperty* p, GML_PropInfo* out);
    GFProperty* (*NextProperty)(GUStruct* s, GFProperty* prev);    /* own properties only; prev NULL = first */
    int         (*GetPropertyName)(GFProperty* p, char* buf, int cap);
    int         (*StructSize)(GUStruct* s);                        /* also a UFunction's param-block size */
    void        (*ProcessEvent)(GUObject* obj, GUFunction* fn, void* params); /* [GT] */
    /* [GT] Finds `func` on obj's class and calls it. params may be NULL for no-arg functions. */
    int         (*CallFunction)(GUObject* obj, const char* func, void* params);

    /* ---- containers (engine allocator) ---- */
    /* [GT] Grow an engine TArray by `count` zeroed elements; returns the first new element. */
    void* (*ArrayAdd)(GML_TArray* arr, int elemSize, int count);
    /* Iterate a TMap described by mapProp. cb returns non-zero to stop. Returns elements visited. */
    int   (*MapForEach)(void* tmap, GFProperty* mapProp,
                        int (*cb)(void* user, void* key, void* value), void* user);
    void* (*EngineAlloc)(size_t bytes); /* [GT] FMemory-owned block the engine may later free/realloc */

    /* ---- UFunction hooks (ProcessEvent level) ---- */
    /* spec: full path "/Script/Engine.Actor:ReceiveBeginPlay", "ClassName:Func",
     * or "*:Func" for any class. Matching functions are resolved lazily, so it is fine
     * to hook Blueprint functions that are not loaded yet. Returns a hook id (>0). */
    int  (*HookFunction)(GML_Plugin* self, const char* spec, GML_HookFn pre, GML_HookFn post, void* user);
    int  (*HookUFunction)(GML_Plugin* self, GUFunction* fn, GML_HookFn pre, GML_HookFn post, void* user);
    void (*Unhook)(int hookId);

    /* ---- runtime assets (no cooking, no pak) ---- */
    GUObject* (*ImportTexture)(const wchar_t* file);                   /* [GT] PNG/JPG/BMP/TGA/EXR -> UTexture2D */
    /* [GT] OBJ -> UStaticMesh. Known issue: the runtime static-mesh build has no usable normals in
     * this game build, so the result renders dark. Use ImportDynamicMesh + AddDynamicMeshComponent. */
    GUObject* (*ImportStaticMesh)(const wchar_t* objFile, const GML_MeshImport* opts);
    GUObject* (*CreateMaterialInstance)(GUObject* parentMaterial);    /* [GT] -> UMaterialInstanceDynamic */
    int       (*SetMaterialTexture)(GUObject* mid, const char* param, GUObject* texture); /* [GT] */
    int       (*SetMaterialScalar)(GUObject* mid, const char* param, float value);       /* [GT] */
    int       (*SetMaterialVector)(GUObject* mid, const char* param, const float rgba[4]); /* [GT] */
    int       (*ExecConsoleCommand)(const char* command);             /* [GT] */
    GUObject* (*WorldContext)(void);  /* the game instance, usable as WorldContextObject */

    /* ---- added in 2.1 (check api->size before use) ---- */
    /* [GT] OBJ -> UDynamicMesh (Geometry Scripting) with the OBJ's normals, UV0, MikkT tangents and
     * material IDs: ID i = opts->materialNames[i]. This is the recommended way to show runtime
     * geometry: in this build ImportStaticMesh cannot produce usable normals (it renders dark). */
    GUObject* (*ImportDynamicMesh)(const wchar_t* objFile, const GML_MeshImport* opts);
    /* [GT] Adds a UDynamicMeshComponent to `actor` (attached to its root at identity), with its own
     * copy of `dynamicMesh`, materials[i] on material ID i, and no collision. Returns the component. */
    GUObject* (*AddDynamicMeshComponent)(GUObject* actor, GUObject* dynamicMesh, GUObject* const* materials, int count);

    /* ---- added in 2.2 (check api->size before use) ---- */
    /* Assets built into the plugin's own DLL, so it ships as one file. A resource script
     * (`BODY_DIFFUSE RCDATA "Assets\\Body_Diffuse.png"`) linked into the DLL embeds them;
     * this returns resource `name`'s bytes (valid while the plugin is loaded) or NULL. */
    const void* (*PluginResource)(GML_Plugin* self, const char* name, size_t* size);
    /* [GT] As ImportTexture, from an encoded image (PNG/JPG/BMP/TGA/EXR) in memory. `name` labels logs. */
    GUObject* (*ImportTextureFromMemory)(const void* data, size_t size, const char* name);
    /* [GT] As ImportDynamicMesh, from OBJ text in memory. `name` labels logs. */
    GUObject* (*ImportDynamicMeshFromMemory)(const void* objText, size_t size, const char* name, const GML_MeshImport* opts);

    /* ---- added in 2.3 (check api->size before use) ---- */
    /* Every plugin the chainloader found (patchers too), in load order, then the ones it did not load.
     * For mod menus and diagnostics. */
    int (*PluginCount)(void);
    int (*GetPluginState)(int index, GML_PluginState* out);  /* 0 if index is out of range */
    /* Enables or disables a plugin from the next launch on, by removing or creating disabled.txt in its
     * folder (the chainloader skips folders that hold one). Refused (0) for a DLL directly in
     * plugins\ or patchers\, which has no folder of its own. */
    int (*SetPluginEnabled)(const char* guid, int enabled);
    /* Any plugin's settings, in the order it bound them (for mod menus). Use the ConfigGet... and
     * ConfigSet functions on the entries; ConfigSet saves the plugin's .cfg, and a plugin that reads
     * the entry's value again sees the change at once. */
    int              (*ConfigCount)(const char* guid);
    GML_ConfigEntry* (*ConfigAt)(const char* guid, int index);
    int              (*ConfigGetInfo)(GML_ConfigEntry* e, GML_ConfigInfo* out);
} GML_API;

#ifdef __cplusplus
}
#endif

#endif /* GML_H */
