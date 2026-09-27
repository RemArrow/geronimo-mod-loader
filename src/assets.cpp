// Runtime assets: build engine objects from loose files at runtime, without cooking and
// without registering packages (which this build refuses for new package IDs). Everything
// goes through BlueprintCallable engine functions
// that exist in the shipping build:
//   KismetRenderingLibrary::ImportFileAsTexture2D     PNG/JPG/... -> UTexture2D
//   StaticMesh::CreateStaticMeshDescription + MeshDescription API + BuildFromStaticMeshDescriptions
//   KismetMaterialLibrary::CreateDynamicMaterialInstance
#include "ue.h"
#include <fstream>
#include <sstream>
#include <map>
#include <unordered_map>

namespace gml::assets {

using namespace ue;

static bool OnGameThread(const char* what) {
    if (GetCurrentThreadId() == g_gameThreadId && UEReady()) return true;
    LOGE("%s must be called on the game thread after ENGINE_READY", what);
    return false;
}

// ------------------------------------------------------------------ textures / materials

GUObject* ImportTexture(const wchar_t* file) {
    if (!OnGameThread("ImportTexture") || !file) return nullptr;
    if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
        LOGE("ImportTexture: file not found: %s", Narrow(file).c_str());
        return nullptr;
    }
    Params p(Lib("KismetRenderingLibrary"), "ImportFileAsTexture2D");
    FStr path(file);
    p.Set(0, WorldContext());
    p.SetStr(1, path);
    UObject* tex = p.Call() ? p.RetAs<UObject*>() : nullptr;
    if (tex) KeepAlive(tex);
    LOGI("ImportTexture %s -> %s", Narrow(file).c_str(), tex ? FullName(tex).c_str() : "FAILED");
    return tex;
}

GUObject* CreateMaterialInstance(GUObject* parent) {
    if (!OnGameThread("CreateMaterialInstance") || !parent) return nullptr;
    Params p(Lib("KismetMaterialLibrary"), "CreateDynamicMaterialInstance");
    p.Set(0, WorldContext());
    p.Set(1, parent);
    // OptionalName = None, CreationFlags = 0: zeroed already
    UObject* mid = p.Call() ? p.RetAs<UObject*>() : nullptr;
    if (mid) KeepAlive(mid);
    return mid;
}

int SetMaterialTexture(GUObject* mid, const char* param, GUObject* tex) {
    if (!OnGameThread("SetMaterialTexture") || !mid || !param) return 0;
    Params p(mid, "SetTextureParameterValue");
    p.Set(0, MakeName(param));
    p.Set(1, tex);
    return p.Call() ? 1 : 0;
}

int SetMaterialScalar(GUObject* mid, const char* param, float v) {
    if (!OnGameThread("SetMaterialScalar") || !mid || !param) return 0;
    Params p(mid, "SetScalarParameterValue");
    p.Set(0, MakeName(param));
    p.Set(1, v);
    return p.Call() ? 1 : 0;
}

int SetMaterialVector(GUObject* mid, const char* param, const float* rgba) {  // rgba[4]
    if (!OnGameThread("SetMaterialVector") || !mid || !param || !rgba) return 0;
    Params p(mid, "SetVectorParameterValue");
    p.Set(0, MakeName(param));
    if (p.ArgSize(1) != 16) return 0;  // FLinearColor
    memcpy(p.Arg(1), rgba, 16);
    return p.Call() ? 1 : 0;
}

int ExecConsoleCommand(const char* cmd) {
    if (!OnGameThread("ExecConsoleCommand") || !cmd) return 0;
    Params p(Lib("KismetSystemLibrary"), "ExecuteConsoleCommand");
    FStr c(Widen(cmd));
    p.Set(0, WorldContext());
    p.SetStr(1, c);
    return p.Call() ? 1 : 0;
}

// ------------------------------------------------------------------ OBJ

struct ObjCorner { int v, vt, vn; };
struct ObjTri { ObjCorner c[3]; int group; };
struct ObjData {
    std::vector<float> v, vt, vn;   // packed xyz / uv / xyz
    std::vector<ObjTri> tris;
    std::vector<std::string> groups; // material names in first-use order
};

static bool ParseObj(const std::wstring& path, ObjData& d, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    std::unordered_map<std::string, int> groupIdx;
    int cur = -1;
    auto useGroup = [&](const std::string& n) {
        auto it = groupIdx.find(n);
        if (it == groupIdx.end()) { it = groupIdx.emplace(n, (int)d.groups.size()).first; d.groups.push_back(n); }
        cur = it->second;
    };
    std::string line;
    std::vector<ObjCorner> poly;
    int lineNo = 0;
    while (std::getline(f, line)) {
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2 || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "v") { float x = 0, y = 0, z = 0; ss >> x >> y >> z; d.v.insert(d.v.end(), {x, y, z}); }
        else if (tag == "vt") { float u = 0, w = 0; ss >> u >> w; d.vt.insert(d.vt.end(), {u, w}); }
        else if (tag == "vn") { float x = 0, y = 0, z = 0; ss >> x >> y >> z; d.vn.insert(d.vn.end(), {x, y, z}); }
        else if (tag == "usemtl") { std::string n; ss >> n; useGroup(n); }
        else if (tag == "f") {
            if (cur < 0) useGroup("default");
            poly.clear();
            std::string tok;
            while (ss >> tok) {
                ObjCorner c{0, 0, 0};
                int* dst[3] = {&c.v, &c.vt, &c.vn};
                size_t start = 0;
                for (int k = 0; k < 3 && start <= tok.size(); k++) {
                    size_t slash = tok.find('/', start);
                    std::string part = tok.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                    if (!part.empty()) *dst[k] = std::stoi(part);
                    if (slash == std::string::npos) break;
                    start = slash + 1;
                }
                // 1-based; negative = relative to the end
                auto fix = [](int i, size_t count) { return i > 0 ? i - 1 : i < 0 ? (int)count + i : -1; };
                c.v = fix(c.v, d.v.size() / 3);
                c.vt = fix(c.vt, d.vt.size() / 2);
                c.vn = fix(c.vn, d.vn.size() / 3);
                if (c.v < 0 || c.v >= (int)(d.v.size() / 3)) {
                    err = "bad vertex index on line " + std::to_string(lineNo);
                    return false;
                }
                poly.push_back(c);
            }
            for (size_t k = 1; k + 1 < poly.size(); k++)  // fan triangulation
                d.tris.push_back({{poly[0], poly[k], poly[k + 1]}, cur});
        }
    }
    if (d.tris.empty()) { err = "no faces"; return false; }
    return true;
}

// ------------------------------------------------------------------ mesh build

// Pre-resolved argument slots so the per-vertex loop is just memcpy + ProcessEvent.
struct Call1 {
    Params p;
    std::vector<void*> a;
    Call1(UObject* o, const char* f, int nargs) : p(o, f) { for (int i = 0; i < nargs; i++) a.push_back(p.Arg(i)); }
    bool ok() const { if (!p.fn) return false; for (void* x : a) if (!x) return false; return true; }
    void go() { ProcessEvent(p.obj, p.fn, p.buf.data(), false); }
};

GUObject* ImportStaticMesh(const wchar_t* file, const GML_MeshImport* optsIn) {
    if (!OnGameThread("ImportStaticMesh") || !file) return nullptr;
    GML_MeshImport opts{};
    opts.size = sizeof opts;
    opts.flipWinding = -1;
    if (optsIn) memcpy(&opts, optsIn, optsIn->size < sizeof opts ? optsIn->size : sizeof opts);
    const std::string fname = Narrow(file);

    ObjData d;
    std::string err;
    if (!ParseObj(file, d, err)) { LOGE("ImportStaticMesh %s: %s", fname.c_str(), err.c_str()); return nullptr; }
    LOGI("ImportStaticMesh %s: %zu verts, %zu uvs, %zu normals, %zu tris, %zu material groups", fname.c_str(),
         d.v.size() / 3, d.vt.size() / 2, d.vn.size() / 3, d.tris.size(), d.groups.size());

    float scale = opts.scale != 0 ? opts.scale : (opts.axis == GML_AXIS_BLENDER_OBJ ? 100.f : 1.f);
    // Both conversions preserve apparent winding (a single mirror takes RH -> LH), so auto = no flip.
    bool flip = opts.flipWinding == 1;
    auto toUE = [&](const float* p, double out[3]) {
        double x = p[0], y = p[1], z = p[2];
        if (opts.axis == GML_AXIS_BLENDER_OBJ) { double t = y; y = z; z = t; }  // OBJ(x,y,z) -> UE(x,z,y)
        out[0] = x * scale + opts.offset[0];
        out[1] = y * scale + opts.offset[1];
        out[2] = z * scale + opts.offset[2];
    };

    UClass* smClass = FindClass("StaticMesh");
    UObject* mesh = NewObject(smClass, WorldContext());
    if (!mesh) { LOGE("ImportStaticMesh: could not create UStaticMesh"); return nullptr; }
    KeepAlive(mesh);

    Params cdp(DefaultObject(smClass), "CreateStaticMeshDescription");
    cdp.Set(0, mesh);
    UObject* desc = cdp.Call() ? cdp.RetAs<UObject*>() : nullptr;
    if (!desc) { LOGE("ImportStaticMesh: CreateStaticMeshDescription failed"); return nullptr; }

    // Reserve
    for (auto [fn, n] : {std::pair{"ReserveNewVertices", d.v.size() / 3},
                         std::pair{"ReserveNewVertexInstances", d.tris.size() * 3},
                         std::pair{"ReserveNewTriangles", d.tris.size()}}) {
        Params r(desc, fn);
        r.Set(0, (int32_t)n);
        r.Call();
    }

    // Material slots + polygon groups
    UObject* fallbackMat = opts.defaultMaterial;
    if (!fallbackMat) fallbackMat = FindObject("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");
    if (!fallbackMat) fallbackMat = LoadObject("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial", false);
    std::vector<int32_t> pgIds;
    for (auto& g : d.groups) {
        UObject* mat = fallbackMat;
        for (int i = 0; i < opts.materialCount; i++)
            if (opts.materialNames && opts.materialNames[i] && g == opts.materialNames[i] && opts.materials[i])
                mat = opts.materials[i];
        Params am(mesh, "AddMaterial");
        am.Set(0, mat);
        uint64_t slot = am.Call() ? am.RetAs<uint64_t>() : 0;

        Params cpg(desc, "CreatePolygonGroup");
        int32_t pg = cpg.Call() ? cpg.RetAs<int32_t>() : -1;
        Params sn(desc, "SetPolygonGroupMaterialSlotName");
        sn.Set(0, pg);
        sn.Set(1, slot);
        sn.Call();
        pgIds.push_back(pg);
        LOGD("  group '%s' -> polygon group %d, slot %s", g.c_str(), pg, NameStr(slot).c_str());
    }

    Call1 cv(desc, "CreateVertex", 0), svp(desc, "SetVertexPosition", 2);
    Call1 cvi(desc, "CreateVertexInstance", 1), suv(desc, "SetVertexInstanceUV", 3);
    Call1 ct(desc, "CreateTriangle", 3);
    if (!cv.ok() || !svp.ok() || !cvi.ok() || !suv.ok() || !ct.ok()) {
        LOGE("ImportStaticMesh: MeshDescription API not found in this build");
        return nullptr;
    }
    const bool vecIsDouble = svp.p.ArgSize(1) == 24;
    const bool uvIsDouble = suv.p.ArgSize(1) == 16;
    void* cvRet = cv.p.Ret();
    void* cviRet = cvi.p.Ret();

    // Split positions by normal so hard edges survive the normal computation.
    std::map<std::pair<int, int>, int32_t> vertOf;
    std::map<std::tuple<int, int, int>, int32_t> instOf;
    auto vertex = [&](const ObjCorner& c) {
        auto key = std::make_pair(c.v, c.vn);
        auto it = vertOf.find(key);
        if (it != vertOf.end()) return it->second;
        cv.go();
        int32_t id = *(int32_t*)cvRet;
        double pos[3];
        toUE(&d.v[c.v * 3], pos);
        *(int32_t*)svp.a[0] = id;
        if (vecIsDouble) memcpy(svp.a[1], pos, 24);
        else { float f[3] = {(float)pos[0], (float)pos[1], (float)pos[2]}; memcpy(svp.a[1], f, 12); }
        svp.go();
        vertOf.emplace(key, id);
        return id;
    };
    auto instance = [&](const ObjCorner& c) {
        auto key = std::make_tuple(c.v, c.vt, c.vn);
        auto it = instOf.find(key);
        if (it != instOf.end()) return it->second;
        *(int32_t*)cvi.a[0] = vertex(c);
        cvi.go();
        int32_t id = *(int32_t*)cviRet;
        if (c.vt >= 0) {
            double uv[2] = {d.vt[c.vt * 2], 1.0 - d.vt[c.vt * 2 + 1]};  // OBJ V is bottom-up
            *(int32_t*)suv.a[0] = id;
            if (uvIsDouble) memcpy(suv.a[1], uv, 16);
            else { float f[2] = {(float)uv[0], (float)uv[1]}; memcpy(suv.a[1], f, 8); }
            *(int32_t*)suv.a[2] = 0;
            suv.go();
        }
        instOf.emplace(key, id);
        return id;
    };

    int32_t triIds[3];
    GML_TArray triArr{triIds, 3, 3};
    for (auto& t : d.tris) {
        for (int k = 0; k < 3; k++) triIds[flip ? 2 - k : k] = instance(t.c[k]);
        *(int32_t*)ct.a[0] = pgIds[t.group];
        memcpy(ct.a[1], &triArr, sizeof triArr);
        ct.go();  // a[2] (out edge ids) is engine-owned and reused across calls
    }

    Params build(mesh, "BuildFromStaticMeshDescriptions");
    GML_TArray descs{&desc, 1, 1};
    build.Set(0, descs);
    build.Set(1, (uint8_t)(opts.buildCollision ? 1 : 0));
    build.Set(2, (uint8_t)0);  // bFastBuild = false: lets the build compute normals/tangents
    if (!build.Call()) { LOGE("ImportStaticMesh: build call failed"); return nullptr; }

    LOGI("ImportStaticMesh %s -> %s (%zu vertices, %zu instances)", fname.c_str(), FullName(mesh).c_str(),
         vertOf.size(), instOf.size());
    return mesh;
}

}  // namespace gml::assets
