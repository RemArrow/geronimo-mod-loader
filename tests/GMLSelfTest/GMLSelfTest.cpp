// GMLSelfTest - in-game verification of the loader API (a GML plugin). Not installed by default.
//
// Put in GML\plugins\GMLSelfTest\ together with:
//   test.obj   any OBJ (Blender default export axes)
//   test.png   any PNG
// Also needs tests\GMLTestPatcher in GML\patchers\, and tests\GMLTestDep and
// tests\GMLTestMissingDep in their own folders under GML\plugins\.
// Launch to the main menu, then read GML\LogOutput.log: every check prints PASS/FAIL and the run ends
// with "SELFTEST COMPLETE pass=N fail=M".
#include <GML/GML.hpp>
#include <windows.h>
#include <chrono>

using namespace gml;

static int s_pass = 0, s_fail = 0;
template <class... A> static void Check(bool ok, std::format_string<A...> f, A&&... a) {
    (ok ? s_pass : s_fail)++;
    LogAt(ok ? GML_LOG_INFO : GML_LOG_ERROR, "{} {}", ok ? "PASS" : "FAIL", std::format(f, std::forward<A>(a)...));
}

struct FVec { double x, y, z; };
struct FBox { FVec min, max; uint8_t valid; };

static void RunReflection() {
    // names & text
    uint64_t n = API->MakeName("GMLSelfTestName");
    char buf[256];
    API->NameToString(n, buf, sizeof buf);
    Check(!strcmp(buf, "GMLSelfTestName"), "FName round-trip -> '{}'", buf);
    uint8_t text[16];
    Check(API->MakeText("Hello text", text) && API->TextToString(text, buf, sizeof buf) && !strcmp(buf, "Hello text"),
          "FText round-trip -> '{}'", buf);

    // lookup
    GUClass* actor = FindClass("Actor");
    Check(actor && Object(actor).Path() == "/Script/Engine.Actor", "FindClass(Actor) -> {}", Object(actor).Path());
    Check(FindObject("/Script/Engine.Actor").ptr == (GUObject*)actor, "FindObject(/Script/Engine.Actor)");
    Check((bool)DefaultObject(actor), "DefaultObject(Actor) -> {}", DefaultObject(actor).FullName());
    auto actors = FindAllOf("Actor");
    Check(actors.size() > 5, "FindAllOf(Actor) -> {} live actors", actors.size());
    Object gi = FindFirstOf("GameInstance");
    Check(gi.IsA("BP_GameInstance_C"), "FindFirstOf(GameInstance) -> {}", gi.FullName());

    // loading
    Object mesh = LoadObject("/Game/Weapons/HK416a5/Mesh/HK_416_279_v2.HK_416_279_v2");
    Check(mesh.IsA("StaticMesh"), "LoadObject(HK_416_279_v2) -> {}", mesh.FullName());
    GUClass* recv = FindClass("Firearm_Rifle_HK416A5_279mm_C");
    std::string recvPath = Object(recv).Path();
    Check(recv && API->LoadClass(recvPath.c_str()) == recv, "LoadClass({})", recvPath);

    // reflection: read a known property and call a function with params + return value
    if (mesh) {
        Params p(mesh, "GetNumTriangles");
        p.Set("LODIndex", (int32_t)0).Call();
        Check(p.Return<int32_t>() > 1000, "call StaticMesh.GetNumTriangles(0) on HK416 -> {}", p.Return<int32_t>());
        Params bb(mesh, "GetBoundingBox");  // struct return value
        bb.Call();
        FBox b = bb.Return<FBox>();
        Check(b.max.x > b.min.x, "struct return: HK_416_279_v2 bounds X[{:.3f}..{:.3f}] Y[{:.3f}..{:.3f}] Z[{:.3f}..{:.3f}]",
              b.min.x, b.max.x, b.min.y, b.max.y, b.min.z, b.max.z);
    }

    // engine allocator + array growth
    void* block = API->EngineAlloc(4096);
    Check(block != nullptr, "EngineAlloc(4096) -> {}", block);
    GML_TArray arr{nullptr, 0, 0};
    bool grew = true;
    for (int i = 0; i < 100; i++) {
        int* slot = (int*)API->ArrayAdd(&arr, sizeof(int), 1);
        if (!slot) { grew = false; break; }
        *slot = i;
    }
    Check(grew && arr.Num == 100 && ((int*)arr.Data)[99] == 99, "ArrayAdd x100 -> Num={} Max={}", arr.Num, arr.Max);
}

static void RunAssets() {
    // texture from PNG
    auto t0 = std::chrono::steady_clock::now();
    Object tex = API->ImportTexture(PluginPath(L"test.png").c_str());
    int32_t w = 0;
    if (tex) { Params p(tex, "Blueprint_GetSizeX"); p.Call(); w = p.Return<int32_t>(); }
    Check(tex.IsA("Texture2D") && w > 0, "ImportTexture(test.png) -> {} width={}", tex.FullName(), w);

    // dynamic material from the HK416's first material
    Object hk = FindObject("/Game/Weapons/HK416a5/Mesh/HK_416_279_v2.HK_416_279_v2");
    Object parent;
    if (hk) { Params p(hk, "GetMaterial"); p.Set("MaterialIndex", (int32_t)0).Call(); parent = p.Return<GUObject*>(); }
    Object mid = parent ? Object(API->CreateMaterialInstance(parent.ptr)) : Object();
    Check(mid.IsA("MaterialInstanceDynamic"), "CreateMaterialInstance(parent {}) -> {}", parent.Name(), mid.FullName());
    if (mid && tex) API->SetMaterialTexture(mid.ptr, "BaseColor", tex.ptr);

    // static mesh from OBJ
    GML_MeshImport o{};
    o.size = sizeof o;
    o.axis = GML_AXIS_BLENDER_OBJ;
    o.flipWinding = -1;
    o.defaultMaterial = mid.ptr;
    auto t1 = std::chrono::steady_clock::now();
    Object sm = API->ImportStaticMesh(PluginPath(L"test.obj").c_str(), &o);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
    Check(sm.IsA("StaticMesh"), "ImportStaticMesh(test.obj) -> {} in {} ms", sm.FullName(), ms);
    if (!sm) return;

    Params lods(sm, "GetNumLODs");
    lods.Call();
    Params tris(sm, "GetNumTriangles");
    tris.Set("LODIndex", (int32_t)0).Call();
    Params verts(sm, "GetNumVertices");
    verts.Set("LODIndex", (int32_t)0).Call();
    Check(lods.Return<int32_t>() >= 1 && tris.Return<int32_t>() > 0,
          "runtime mesh has render data: LODs={} triangles={} vertices={}", lods.Return<int32_t>(),
          tris.Return<int32_t>(), verts.Return<int32_t>());
    Params bb(sm, "GetBoundingBox");
    bb.Call();
    FBox b = bb.Return<FBox>();
    Log("runtime mesh bounds cm: X[{:.3f}..{:.3f}] Y[{:.3f}..{:.3f}] Z[{:.3f}..{:.3f}]", b.min.x, b.max.x, b.min.y,
        b.max.y, b.min.z, b.max.z);
    Params gm(sm, "GetMaterial");
    gm.Set("MaterialIndex", (int32_t)0).Call();
    Check(gm.Return<GUObject*>() == mid.ptr, "runtime mesh slot 0 uses our material instance");
    (void)t0;
}

static int64_t FrameCount() {
    Params p(Lib("KismetSystemLibrary"), "GetFrameCount");
    p.Call();
    return p.Return<int64_t>();
}

static int s_ticks = 0;
static std::chrono::steady_clock::time_point s_tickStart;
static bool s_ranOnce = false;

// Soft dependency on GMLTestDep: must load after it (its GUID sorts after ours, so only the
// dependency can put it first), but would load without it too.
GML_PLUGIN("com.remarrow.gmlselftest", "GML SelfTest", "1.0.0",
           {{"com.remarrow.zz.gmltestdep", "1.0.0", GML_DEPENDENCY_SOFT}});

static std::string Slurp(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(GetFileSize(f, nullptr), '\0');
    DWORD n = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &n, nullptr);
    CloseHandle(f);
    return s;
}

GML_AWAKE() {
    Check(API->version == GML_API_VERSION && API->size == sizeof(GML_API), "API table v{} size {}", API->version, API->size);
    Check(API->UEReady() && API->IsGameThread(), "Awake runs on the game thread with the engine up");
    Check(API->IsPluginLoaded("com.remarrow.zz.gmltestdep") == 1,
          "soft dependency (GML TestDep) was loaded before this plugin despite sorting after it");
    Check(API->IsPluginLoaded("com.remarrow.gmltestpatcher") == 1, "test patcher ran at process entry");
    Check(API->IsPluginLoaded("com.remarrow.gmltestmissingdep") == 0,
          "plugin with a missing hard dependency was skipped");
    Check(std::string(API->PluginGUID(Self)) == "com.remarrow.gmlselftest" &&
          std::string(API->PluginVersion(Self)) == "1.0.0", "own metadata: {} {} v{}", API->PluginGUID(Self),
          API->PluginName(Self), API->PluginVersion(Self));

    // BepInEx-style config: bind, read defaults, set, read back
    auto b = Config.Bind("General", "Enabled", true, "A boolean setting");
    auto i = Config.Bind("General", "Count", 42, "An integer setting");
    auto f = Config.Bind("Tuning", "Scale", 1.5f, "A float setting");
    auto s = Config.Bind("Tuning", "Path", "Assets\\thing.obj", "A string setting (raw backslashes)");
    Check(b.Value() && i.Value() == 42 && f.Value() == 1.5f && s.Value() == "Assets\\thing.obj",
          "Config.Bind defaults: {} {} {} {}", b.Value(), i.Value(), f.Value(), s.Value());
    auto runs = Config.Bind("State", "Runs", 0, "Incremented every launch (persistence test)");
    int before = runs.Value();
    runs.Set(before + 1);
    Check(runs.Value() == before + 1, "ConfigSet round-trip: Runs {} -> {}", before, runs.Value());

    On(GML_EVENT_ENGINE_READY, [](void*) {
        Check(API->IsGameThread() == 1, "ENGINE_READY on the game thread, after Awake");
        std::string cfg = Slurp(GetPath(GML_PATH_CONFIG) + L"\\com.remarrow.gmlselftest.cfg");
        Check(cfg.find("## Settings file was created by plugin GML SelfTest v1.0.0") != std::string::npos &&
              cfg.find("# Setting type: Int32") != std::string::npos && cfg.find("[Tuning]") != std::string::npos,
              "<GUID>.cfg written in BepInEx format ({} bytes)", cfg.size());
    });
    static int beginPlays = 0;
    HookAfter("*:ReceiveBeginPlay", [](Object, GUFunction*, void*) { beginPlays++; });

    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void* gm) {
        if (s_ranOnce) return;
        s_ranOnce = true;
        Log("---- self-test on {} ----", Object((GUObject*)gm).FullName());
        RunReflection();
        RunAssets();
        Check(beginPlays > 0, "\"*:ReceiveBeginPlay\" wildcard hook fired {} times", beginPlays);
        s_tickStart = std::chrono::steady_clock::now();
        static int64_t frame0 = FrameCount();
        On(GML_EVENT_TICK, [](void*) {
            if (s_ticks < 0) return;
            s_ticks++;
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_tickStart).count();
            if (secs >= 3.0) {
                int64_t frames = FrameCount() - frame0;
                Check(s_ticks > 60 && s_ticks <= frames + 1 && s_ticks >= frames - 2,
                      "TICK once per frame: {} ticks, {} engine frames in {:.1f}s ({:.0f} fps)", s_ticks, frames,
                      secs, frames / secs);
                s_ticks = -1;
                Log("SELFTEST COMPLETE pass={} fail={}", s_pass, s_fail);
            }
        });
    });
    return 0;
}
