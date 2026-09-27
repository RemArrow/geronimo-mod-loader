# Geronimo Mod Loader (GML)

A native mod loader for **Geronimo** (Dark Matter Studios, Unreal Engine 5.7), **structured like
BepInEx**. It has a doorstop DLL, a `core`, `plugins`, `patchers`, and `config` with one `.cfg`
per plugin, plus `LogOutput.log`. Plugins declare GUIDs, versions and dependencies, and a
chainloader resolves the load order. Plugins are plain **C/C++ DLLs**. It needs no UE4SS, Lua,
Python or injector.

Plugins can:

- find, load, create and inspect any UObject; read/write properties; call any UFunction,
- hook any UFunction that goes through `ProcessEvent` (pre/post, by name, with parameter access),
- inline-hook any native function in the game (patchers can do it before the engine starts),
- **build assets at runtime from loose files**: OBJ → `UStaticMesh`, PNG/JPG → `UTexture2D`,
  and dynamic material instances. This needs no UE editor, cooking or pak,
- ship cooked IoStore containers (`.utoc`/`.ucas`/`.pak`), which the loader deploys for them.

**Supported build:** Geronimo on Steam, UE 5.7.4 (CL 51494982), Windows x64. After a game
update, see [After a game update](#after-a-game-update).

---

## Install

1. Download `GML-<version>.zip` from [Releases](../../releases).
2. Close the game, then extract the zip into
   `...\steamapps\common\GERONIMO\Geronimo\Binaries\Win64\` (the folder containing
   `Geronimo-Win64-Shipping.exe`).
3. Put plugins in `GML\plugins\`. Launch the game, then check `GML\LogOutput.log`.

**Disable:** set `enabled = false` in `doorstop_config.ini`.
**Uninstall:** delete `version.dll`, `doorstop_config.ini`, `GML_INSTALL.txt`, `GML\`, and
`Geronimo\Content\Paks\~GML\`.

GML coexists with UE4SS: both hook ProcessEvent, and both keep working.

> Starting `Geronimo-Win64-Shipping.exe` directly makes it exit immediately and relaunch
> through Steam, without your command-line arguments. The second process is the real game, and
> `LogOutput.log` belongs to it.

---

## Layout

```
Geronimo\Binaries\Win64\
  version.dll                  doorstop (like BepInEx's winhttp.dll): loads GML\core\GML.dll
  doorstop_config.ini          enabled / target / process
  GML\
    core\GML.dll               the loader
    config\
      GML.cfg                  loader settings (like BepInEx.cfg)
      <GUID>.cfg               one per plugin, created by Config.Bind(...)
    patchers\*.dll             run at process entry, before the engine starts
    plugins\                   plugins, any folder depth, plus their content
      MyPlugin\
        MyPlugin.dll
        Paks\*.utoc/.ucas/.pak -> copied to Content\Paks\~GML\ at launch
    LogOutput.log              the log, rewritten every launch
```

| BepInEx | GML |
|---|---|
| `winhttp.dll` + `doorstop_config.ini` | `version.dll` + `doorstop_config.ini` |
| `BepInEx\core\` | `GML\core\GML.dll` |
| `BepInEx\plugins\`, `BepInEx\patchers\` | `GML\plugins\`, `GML\patchers\` |
| `BepInEx\config\BepInEx.cfg`, `<GUID>.cfg` | `GML\config\GML.cfg`, `<GUID>.cfg` (same file format) |
| `LogOutput.log`, `[Info   :Source] msg` | same |
| `[BepInPlugin(GUID, Name, Version)]` | `GML_PLUGIN(guid, name, version)` |
| `[BepInDependency(GUID, version, flags)]` | `GML_PLUGIN(..., {{guid, minVersion, GML_DEPENDENCY_HARD/SOFT}})` |
| `[BepInIncompatibility(GUID)]` | `GML_PLUGIN(..., {deps}, {"guid"})` |
| `Awake()` | `GML_AWAKE() { ... }` |
| `Config.Bind(section, key, default, description)` | same, `gml::Config.Bind(...)` |
| `Logger.LogInfo/Message/Warning/Error` | `gml::Log/Message/Warn/Error` |
| preloader patchers | `GML_PATCH() { ... }` in `GML\patchers\` |

GML differs from BepInEx in four ways:

- Strings in `.cfg` files are written raw, so Windows paths need no escaping.
- A plugin folder containing `disabled.txt` is skipped, along with its paks.
- There is no assembly cache. Metadata is read straight from each DLL's export table, which
  takes microseconds.
- Plugins can't be hot-reloaded, the same as in BepInEx.

---

## Writing a plugin

Get `GML-SDK-<version>.zip` from Releases, or clone this repo. A minimal plugin
([`examples/HelloGML`](examples/HelloGML/HelloGML.cpp)):

```cpp
#include <GML/GML.hpp>
using namespace gml;

GML_PLUGIN("com.you.myplugin", "My Plugin", "1.0.0");

GML_AWAKE() {                                     // the engine is up; UObjects exist
    auto verbose = Config.Bind("General", "Verbose", false, "Log every level start");
    if (verbose.Value())
        On(GML_EVENT_WORLD_BEGIN_PLAY, [](void* gameMode) {
            Log("level started: {}", Object((GUObject*)gameMode).FullName());
        });
    HookAfter("BP_GameInstance_C:GetItems", [](Object gi, GUFunction*, void*) {
        Log("spawner list built on {}", gi.Name());
    });
    return 0;                                     // non-zero = failed; dependents are skipped
}
```

Compile it as an x64 DLL with C++20 (MSVC: `cl /std:c++20 /LD /MT /EHsc /I<sdk>\include MyPlugin.cpp`)
and drop it in `GML\plugins\MyPlugin\`. The API itself is plain C
([`GML.h`](include/GML/GML.h)), so any compiler works. [`GML.hpp`](include/GML/GML.hpp) is the
header-only C++20 layer used above.

### Metadata and the chainloader

```cpp
GML_PLUGIN("com.you.b", "B", "1.2.0",
           {{"com.you.a", "1.0.0", GML_DEPENDENCY_HARD},     // must exist, v1.0.0+
            {"com.other.x", "", GML_DEPENDENCY_SOFT}},       // load after it if present
           {"com.someone.conflicting"});                     // incompatible with
```

At process entry the chainloader reads this struct from each DLL under `plugins\` without
loading the DLL. DLLs without it (helper libraries) are ignored. It then:

1. keeps only the newest version of each GUID,
2. drops plugins whose incompatibilities are present,
3. drops plugins with missing or too-old hard dependencies (cascading),
4. sorts the rest so dependencies, hard or soft, load first,
5. once the engine is up, loads each one and calls `GML_AWAKE`. If a plugin's Awake fails, the
   plugins that hard-depend on it are skipped.

Each step is logged in `LogOutput.log`, e.g. `Could not load [X 1.0.0] because it has missing
dependencies: ...`. Other plugins can check `API->IsPluginLoaded("guid")`.

### Lifecycle

| Stage | When | Who |
|---|---|---|
| `GML_PATCH` | process entry, before the CRT and engine start | patchers |
| `GML_AWAKE` | first game-thread ProcessEvent; UObjects exist | plugins, dependency order |
| `ENGINE_READY` | right after every Awake | event, `data` = NULL |
| `GAME_INSTANCE_INIT` | after `UGameInstance::ReceiveInit` | event, `data` = game instance |
| `WORLD_BEGIN_PLAY` | after each GameMode's `ReceiveBeginPlay` (every level, incl. main menu) | event, `data` = game mode |
| `TICK` | once per engine frame | event, `data` = `float*` delta seconds |

Everything except `GML_PATCH` runs on the game thread. From another thread use
`RunOnGameThread`. Anything marked `[GT]` in `GML.h` must be called on the game thread.

### Config

```cpp
auto scale = Config.Bind("Mesh", "Scale", 1.0f, "Uniform scale");  // bool, ints, float/double, strings
float s = scale.Value();
scale.Set(2.0f);                                                   // saves the file
```

This writes `GML\config\<GUID>.cfg` in BepInEx's format: `##` description, `# Setting type`,
`# Default value`, `Key = value`. Values the user edits survive; descriptions and defaults are
regenerated. Keys nobody binds any more are kept, as in BepInEx.

### Paths

`gml::PluginPath(L"Assets\\thing.obj")` resolves against the plugin's own folder.
`gml::GetPath(GML_PATH_CONFIG / _PLUGINS / _PATCHERS / _CORE / _GAME_ROOT / _EXECUTABLE)`
gives the rest.

### Patchers

```cpp
GML_PLUGIN("com.you.mypatcher", "My Patcher", "1.0.0");   // optional for patchers
GML_PATCH() {
    void* fn = API->FindPattern("48 89 5C 24 ?? 57 48 83 EC 20 ...");
    API->HookNative(fn, &MyDetour, (void**)&s_original);   // before any engine code runs
    return 0;
}
```

Put the DLL in `GML\patchers\`. Patchers run in alphabetical order and have config access too.

### Hooks

```cpp
Hook("Class:Function", pre, post);    // e.g. "BP_GameInstance_C:GetItems"
Hook("*:Function", pre, post);        // any class - filter on self.IsA(...)
Hook("/Script/Engine.Actor:ReceiveBeginPlay", pre, post);  // exact path
```

A pre-hook returning `true` skips the original. Hooks can name Blueprint functions that are
not loaded yet; they resolve when first called. Read parameters with
`gml::Param<T>(fn, params, "Name")`.

**What ProcessEvent hooks see:** every call from native code into script. That includes
BlueprintImplementableEvents (`ReceiveBeginPlay`, `ReceiveTick`, …), delegates, timers, input and
UI events, and calls made by other plugins. They do **not** see Blueprint-to-Blueprint calls inside
the script VM, nor Blueprint calls into native functions. For those, hook the native code with
`HookNative`.

To find hook targets, set `[Engine] TraceProcessEvent = true` in `GML.cfg`: every distinct
UFunction that passes through ProcessEvent is logged once.

### Calling functions and touching properties

```cpp
Object gi = FindFirstOf("BP_GameInstance_C");
bool inSession = gi.GetBool("bIsInSession");
gi.Set("NumPlayers", 2);

Object comp = actor.GetObj("StaticMeshComponent");
Params(comp, "SetStaticMesh").SetObj("NewMesh", mesh).Call();

Params p(Lib("KismetSystemLibrary"), "GetFrameCount");   // static functions: call on the CDO
p.Call();
int64_t frame = p.Return<int64_t>();
```

Blueprint struct members carry GUID suffixes (`GunAssets_9_E68A…`); `FindProperty("GunAssets")`
matches them without the suffix.

### Runtime assets

```cpp
GUObject* tex = API->ImportTexture(PluginPath(L"Assets\\basecolor.png").c_str());
GUObject* mat = API->CreateMaterialInstance(someGameMaterial);
API->SetMaterialTexture(mat, "BaseColor", tex);

GML_MeshImport o{sizeof o};
o.axis = GML_AXIS_BLENDER_OBJ;          // Blender's default OBJ export, metres -> cm
o.defaultMaterial = mat;
GUObject* mesh = API->ImportStaticMesh(PluginPath(L"Assets\\thing.obj").c_str(), &o);
```

- **OBJ from Blender:** File → Export → Wavefront, default axes (forward −Z, up Y).
  `GML_AXIS_BLENDER_OBJ` then applies the same transform as UE's FBX import (Y negated, ×100).
  Checked: a 30k-triangle rifle comes out with bounds identical to the UE-editor import of the
  same geometry.
- `usemtl` groups become material slots; map them to materials with `materials`/`materialNames`.
- Positions are split by normal, so hard edges survive the build's normal computation.
- Imported objects are kept alive automatically (added to `GameInstance.ReferencedObjects`).
  Anything you create yourself with `NewObject` that nothing references: call `KeepAlive`.
- If a mesh renders inside-out, set `flipWinding = 1`.

Runtime assets matter in this game because cooked *new* packages don't register (see
[Known limitations](#known-limitations)). An asset built at runtime has no package at all.

---

## Configuration: `GML\config\GML.cfg`

Created with descriptions on first launch.

| Section / key | Default | |
|---|---|---|
| `[Logging.Console] Enabled` | false | console window mirroring the log (flat-screen debugging) |
| `[Logging.Disk] LogLevel` | Info | Debug / Info / Message / Warning / Error / Fatal |
| `[Logging.Disk] Timestamps` | false | prefix log lines with the time of day |
| `[Paks] Sync` | true | deploy containers found under `plugins\` |
| `[Engine] TraceProcessEvent` | false | log each distinct UFunction seen once |
| `[Engine] TickIntervalMs` | 8 | fallback tick rate if the frame counter can't be resolved |
| `[Offsets] GObjects / AppendString / ProcessEvent` | Dumper-7 values | RVAs |

`doorstop_config.ini` (next to `version.dll`): `enabled`, `target` (default
`GML\core\GML.dll`) and `process` (default `Geronimo-Win64-Shipping.exe`). The doorstop is a plain
`version.dll` proxy in any other process.

---

## Building from source

Requires Visual Studio 2022 (MSVC, C++20) on Windows x64.

```bat
build.bat            :: build into build\  (build\game\ mirrors Binaries\Win64\)
build.bat install    :: + copy into the game; set GML_GAME_DIR if it isn't in the default Steam library
build.bat package    :: + write the release zips to build\release\
```

| Folder | What |
|---|---|
| `src/doorstop/` | `version.dll`: forwards the real `version.dll`, patches the exe entry point, loads the core |
| `src/` | `GML.dll`: config, log, chainloader, ProcessEvent hook, reflection, hooks, runtime assets |
| `include/GML/` | the plugin SDK (`GML.h` C API, `GML.hpp` C++20 layer) |
| `examples/` | example plugins (built, never installed) |
| `tests/` | in-game self-test plugins (built, never installed) |
| `tools/gmlcheck.cpp` | offline checker for the game exe (see below) |

---

## After a game update

The loader depends on three RVAs, found with [Dumper-7](https://github.com/Encryqed/Dumper-7), plus
the UE 5.7.4 struct layouts in [`src/ue.h`](src/ue.h).

1. Run `gmlcheck.exe` (in the SDK zip, or `build\gmlcheck.exe`). It checks the configured RVAs
   (from `GML.cfg`) against built-in signatures in the exe on disk, re-scans for them, and
   self-tests the hook engine. No game launch is needed.
2. If a signature no longer matches but is still found uniquely, the loader already copes (it
   logs the new RVA). Put that value in `GML.cfg [Offsets]`.
3. If not found: re-run Dumper-7 and copy `Offsets::GObjects/AppendString/ProcessEvent` from
   `CppSDK/SDK/Basic.hpp` into `GML.cfg`. If the engine version changed, diff the layouts too.
4. A bad GObjects value is caught when the engine starts (logged, plugins not loaded) rather than
   crashing. Patchers still run.

---

## Troubleshooting

| You see | Meaning |
|---|---|
| no `GML\LogOutput.log` | `version.dll` not in `Binaries\Win64`, `enabled = false`, or wrong `process`. Check for `doorstop_error.log` next to the exe (written if `GML.dll` can't be loaded) |
| `configured offset verified` ×2, `ProcessEvent hooked` | normal start |
| `signature scan found 0 candidates` | game updated, see above. Patchers run; plugins don't |
| `GObjects ... failed validation` | wrong GObjects RVA |
| `Could not load [X] because it has missing dependencies` | install the dependency (or a new enough version) |
| `Skipping [X] because a newer version exists` | two copies of one plugin; delete the old one |
| `[X] ... raised exception 0x...` | that plugin crashed in a callback; its callbacks are disabled, the game continues |
| `must be called on the game thread` | a `[GT]` function was called from the wrong thread. Use `RunOnGameThread` |

**Self-test.** `tests/` holds a self-test plugin and three helpers. To run it:

1. Copy `GMLSelfTest.dll`, with any `test.obj` (Blender default export) and `test.png` beside it,
   into `GML\plugins\GMLSelfTest\`.
2. Copy `GMLTestDep.dll` and `GMLTestMissingDep.dll` into their own folders under `GML\plugins\`.
3. Copy `GMLTestPatcher.dll` into `GML\patchers\`.
4. Launch to the menu and read the PASS/FAIL lines ending in `SELFTEST COMPLETE`.
5. Remove all four, and `config\com.remarrow.gmlselftest.cfg`.

---

## Known limitations

- **New cooked packages don't register in this build.** Containers are mounted (overrides of
  existing packages work), but packages with *new* IDs never become loadable. This was proven with
  retoc-built and engine-built (UAT) containers alike. For new content, use runtime assets.
- **Small intentional leaks.** GML borrows the engine allocator through a reflected call
  (`KismetStringLibrary::LeftPad` returns an engine-allocated buffer), because `FMemory` isn't
  exported. When a TArray grows, the old buffer is not freed, and FStrings returned by engine
  calls are never freed either. These are bytes to kilobytes, once per operation.
- ProcessEvent-level hooks only; see *What ProcessEvent hooks see* above.
- Inline hooks installed after startup patch 5 bytes non-atomically. That's safe in a patcher
  (single thread, before the engine), so install native hooks there when possible.
- The runtime OBJ importer has no vertex colours and uses only the first UV channel.
- Runtime-built meshes are verified numerically (render data, bounds, material slots) but their
  shading hasn't been reviewed in a headset yet.
