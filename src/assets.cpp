// Runtime assets: build engine objects from loose files at runtime, without cooking and
// without registering packages (which this build refuses for new package IDs). Everything
// goes through BlueprintCallable engine functions
// that exist in the shipping build:
//   KismetRenderingLibrary::ImportFileAsTexture2D / ImportBufferAsTexture2D   PNG/JPG/... -> UTexture2D
//   StaticMesh::CreateStaticMeshDescription + MeshDescription API + BuildFromStaticMeshDescriptions
//   KismetMaterialLibrary::CreateDynamicMaterialInstance
#include "ue.h"
#include <fstream>
#include <sstream>
#include <map>
#include <unordered_map>
#include <tuple>

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

// Same decoder as ImportTexture (ImportFileAsTexture2D reads the file and calls this).
GUObject* ImportTextureFromMemory(const void* data, size_t size, const char* name) {
    if (!OnGameThread("ImportTextureFromMemory") || !data || !size) return nullptr;
    const char* label = name ? name : "<memory>";
    if (size > 0x7fffffff) { LOGE("ImportTextureFromMemory %s: %zu bytes is too large", label, size); return nullptr; }
    Params p(Lib("KismetRenderingLibrary"), "ImportBufferAsTexture2D");
    if (p.ArgSize(1) != sizeof(GML_TArray)) { LOGE("ImportBufferAsTexture2D has an unexpected signature"); return nullptr; }
    p.Set(0, WorldContext());
    *(GML_TArray*)p.Arg(1) =GML_TArray{(void*)data, (int32_t)size, (int32_t)size};  // read-only input: our memory
    UObject* tex = p.Call() ? p.RetAs<UObject*>() : nullptr;
    if (tex) KeepAlive(tex);
    LOGI("ImportTextureFromMemory %s (%zu bytes) -> %s", label, size, tex ? FullName(tex).c_str() : "FAILED");
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

static bool ParseObj(std::istream& f, ObjData& d, std::string& err) {
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

static bool ParseObj(const std::wstring& path, ObjData& d, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    return ParseObj(f, d, err);
}

// Read-only istream over a block of memory (an embedded resource), without copying it.
struct MemBuf : std::streambuf {
    MemBuf(const void* p, size_t n) {
        char* b = (char*)p;  // get area only: never written through
        setg(b, b, b + n);
    }
};

// ------------------------------------------------------------------ mesh build: Geometry Scripting

// The primary path. UStaticMesh::BuildFromStaticMeshDescriptions (below) is reachable from
// Blueprint but its BP API cannot set normals or tangents, and the result renders black in this
// build. Geometry Scripting (compiled into the shipping game) takes explicit per-vertex normals and
// UVs, computes MikkT tangents, and CopyMeshToStaticMesh writes all of it into a real UStaticMesh.

static int FieldOffset(UStruct* s, const char* name) {
    FProperty* p = s ? FindProperty(s, name) : nullptr;
    return p ? PropOffset(p) : -1;
}
static UStruct* ParamStruct(UFunction* fn, const char* name) {
    FProperty* p = fn ? FindProperty((UStruct*)fn, name) : nullptr;
    GML_PropInfo i;
    return p && PropInfo(p, &i) ? i.structType : nullptr;
}
static bool SetArray(uint8_t* container, UStruct* s, const char* field, void* data, int count) {
    int off = FieldOffset(s, field);
    if (off < 0) return false;
    *(GML_TArray*)(container + off) = GML_TArray{data, count, count};  // read-only input: our memory
    return true;
}

// One vertex per unique (position, uv, normal) corner, grouped by material.
struct GroupBuffers {
    std::vector<double> pos, nrm, uv;
    std::vector<int32_t> tris;
    std::map<std::tuple<int, int, int>, int32_t> index;
};

// A UGeometryScriptDebug to pass as `Debug`, and a dump of what it collected.
static UObject* GsDebug() { return NewObject(FindClass("GeometryScriptDebug"), WorldContext()); }
static void LogGsDebug(UObject* dbg, const char* what) {
    if (!dbg) return;
    FProperty* p = FindProperty((UStruct*)ClassOf(dbg), "Messages");
    GML_PropInfo pi, ii;
    if (!p || !PropInfo(p, &pi) || !pi.inner || !PropInfo(pi.inner, &ii)) return;
    auto* arr = (GML_TArray*)((uint8_t*)dbg + pi.offset);
    int msgOff = FieldOffset(ii.structType, "Message");
    for (int i = 0; i < arr->Num && msgOff >= 0; i++)
        LOGW("%s: Geometry Scripting says: %s", what,
             TextToString((uint8_t*)arr->Data + (size_t)i * ii.size + msgOff).c_str());
}

// OBJ -> UDynamicMesh with the OBJ's normals, UV0, material IDs (groupIds[group]) and MikkT tangents.
static UObject* BuildDynamicMesh(const ObjData& d, const std::function<void(const float*, double*)>& toPos,
                                 bool swapYZ, bool flip, const std::vector<int>& groupIds) {
    UClass* dmClass = FindClass("DynamicMesh");
    UObject* editLib = Lib("GeometryScriptLibrary_MeshBasicEditFunctions");
    UObject* matLib = Lib("GeometryScriptLibrary_MeshMaterialFunctions");
    UObject* nrmLib = Lib("GeometryScriptLibrary_MeshNormalsFunctions");
    if (!dmClass || !editLib || !matLib || !nrmLib) {
        LOGW("Geometry Scripting is not available in this build");
        return nullptr;
    }
    UObject* dyn = NewObject(dmClass, WorldContext());
    if (!dyn) return nullptr;
    KeepAlive(dyn);
    { Params p(matLib, "EnableMaterialIDs"); p.Set(0, dyn); p.Call(); }

    std::vector<GroupBuffers> groups(d.groups.size());
    const bool haveNormals = !d.vn.empty();
    for (auto& t : d.tris) {
        GroupBuffers& g = groups[t.group];
        int32_t idx[3];
        for (int k = 0; k < 3; k++) {
            const ObjCorner& c = t.c[k];
            auto key = std::make_tuple(c.v, c.vt, c.vn);
            auto it = g.index.find(key);
            if (it == g.index.end()) {
                double p[3];
                toPos(&d.v[c.v * 3], p);
                g.pos.insert(g.pos.end(), {p[0], p[1], p[2]});
                double n[3] = {0, 0, 1};
                if (c.vn >= 0) {
                    n[0] = d.vn[c.vn * 3]; n[1] = d.vn[c.vn * 3 + 1]; n[2] = d.vn[c.vn * 3 + 2];
                    if (swapYZ) std::swap(n[1], n[2]);  // same (orthogonal) map as positions
                }
                g.nrm.insert(g.nrm.end(), {n[0], n[1], n[2]});
                g.uv.insert(g.uv.end(), {c.vt >= 0 ? (double)d.vt[c.vt * 2] : 0.0,
                                         c.vt >= 0 ? 1.0 - d.vt[c.vt * 2 + 1] : 0.0});  // OBJ V is bottom-up
                it = g.index.emplace(key, (int32_t)(g.pos.size() / 3 - 1)).first;
            }
            idx[flip ? 2 - k : k] = it->second;
        }
        g.tris.insert(g.tris.end(), {idx[0], idx[1], idx[2]});
    }

    UObject* dbg = GsDebug();
    for (size_t gi = 0; gi < groups.size(); gi++) {
        GroupBuffers& g = groups[gi];
        if (g.tris.empty()) continue;
        Params ap(editLib, "AppendBuffersToMesh");
        UStruct* bs = ParamStruct(ap.fn, "Buffers");
        uint8_t* buf = (uint8_t*)ap.Ptr("Buffers");
        int nv = (int)(g.pos.size() / 3);
        bool ok = bs && buf && SetArray(buf, bs, "Vertices", g.pos.data(), nv) &&
                  SetArray(buf, bs, "Normals", g.nrm.data(), nv) && SetArray(buf, bs, "UV0", g.uv.data(), nv) &&
                  SetArray(buf, bs, "Triangles", g.tris.data(), (int)(g.tris.size() / 3));
        if (!ok) { LOGE("unexpected FGeometryScriptSimpleMeshBuffers layout"); return nullptr; }
        ap.Set(0, dyn);
        if (void* mid = ap.Ptr("MaterialID")) *(int32_t*)mid = groupIds[gi];
        if (void* dp = ap.Ptr("Debug")) *(UObject**)dp = dbg;
        ap.Call();
    }
    if (!haveNormals) {
        Params rn(nrmLib, "RecomputeNormals");
        rn.Set(0, dyn);
        if (uint8_t* o = (uint8_t*)rn.Ptr("CalculateOptions")) { o[0] = 1; o[1] = 1; }  // angle + area weighted
        rn.Call();
    }
    {
        Params ct(nrmLib, "ComputeTangents");
        ct.Set(0, dyn);
        if (uint8_t* o = (uint8_t*)ct.Ptr("Options")) o[0] = 2;  // StandardMikkT, UV layer 0
        if (void* dp = ct.Ptr("Debug")) *(UObject**)dp = dbg;
        ct.Call();
    }
    LogGsDebug(dbg, "mesh build");
    size_t verts = 0, tris = 0;
    for (auto& g : groups) { verts += g.pos.size() / 3; tris += g.tris.size() / 3; }
    LOGI("dynamic mesh built: %zu vertices, %zu triangles, %zu material IDs, %s normals, MikkT tangents", verts, tris,
         groups.size(), haveNormals ? "OBJ" : "computed");
    return dyn;
}

// Writes a UDynamicMesh into a new UStaticMesh. Geometry Scripting only supports this in the editor
// in this build (it reports failure at runtime), so callers fall back.
static UObject* CopyToStaticMesh(UObject* dyn, const std::vector<UObject*>& mats, const std::vector<std::string>& slots) {
    UObject* smLib = Lib("GeometryScriptLibrary_StaticMeshFunctions");
    UObject* mesh = smLib ? NewObject(FindClass("StaticMesh"), WorldContext()) : nullptr;
    if (!mesh) return nullptr;
    std::vector<uint64_t> slotNames;
    for (auto& s : slots) slotNames.push_back(MakeName(s));
    Params cp(smLib, "CopyMeshToStaticMesh");
    UStruct* os = ParamStruct(cp.fn, "Options");
    uint8_t* opt = (uint8_t*)cp.Ptr("Options");
    if (!os || !opt) return nullptr;
    *(bool*)(opt + FieldOffset(os, "bReplaceMaterials")) = true;
    SetArray(opt, os, "NewMaterials", (void*)mats.data(), (int)mats.size());
    SetArray(opt, os, "NewMaterialSlotNames", slotNames.data(), (int)slotNames.size());
    UObject* dbg = GsDebug();
    if (void* dp = cp.Ptr("Debug")) *(UObject**)dp = dbg;
    cp.Set(0, dyn);
    cp.Set(1, mesh);
    cp.Call();
    uint8_t outcome = cp.Ptr("Outcome") ? *(uint8_t*)cp.Ptr("Outcome") : 0;
    if (outcome != 1) {
        LogGsDebug(dbg, "CopyMeshToStaticMesh");
        return nullptr;
    }
    KeepAlive(mesh);
    return mesh;
}

// ------------------------------------------------------------------ mesh build: MeshDescription (fallback)

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

    {
        UObject* fb = opts.defaultMaterial;
        if (!fb) fb = LoadObject("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial", false);
        std::vector<UObject*> mats;
        std::vector<std::string> slots;
        for (auto& g : d.groups) {
            UObject* m = fb;
            for (int i = 0; i < opts.materialCount; i++)
                if (opts.materialNames && opts.materialNames[i] && g == opts.materialNames[i] && opts.materials[i])
                    m = opts.materials[i];
            mats.push_back(m);
            slots.push_back(g);
        }
        std::vector<int> ids;
        for (size_t i = 0; i < d.groups.size(); i++) ids.push_back((int)i);
        UObject* dyn = BuildDynamicMesh(d, toUE, opts.axis == GML_AXIS_BLENDER_OBJ, flip, ids);
        if (UObject* mesh = dyn ? CopyToStaticMesh(dyn, mats, slots) : nullptr) {
            LOGI("ImportStaticMesh %s -> %s (Geometry Scripting)", fname.c_str(), FullName(mesh).c_str());
            return mesh;
        }
        LOGW("ImportStaticMesh: falling back to the MeshDescription build, which has no usable normals in this "
             "build and renders dark - prefer ImportDynamicMesh + AddDynamicMeshComponent");
    }

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

// ------------------------------------------------------------------ dynamic meshes (the working path)

// OBJ -> UDynamicMesh. Material ID i corresponds to opts->materialNames[i]; groups not listed get
// IDs after those, in first-use order. Pair with AddDynamicMeshComponent to render it.
static UObject* DynamicMeshFromObj(std::istream& in, const std::string& fname, const GML_MeshImport* optsIn) {
    GML_MeshImport opts{};
    opts.size = sizeof opts;
    opts.flipWinding = -1;
    if (optsIn) memcpy(&opts, optsIn, optsIn->size < sizeof opts ? optsIn->size : sizeof opts);
    ObjData d;
    std::string err;
    if (!ParseObj(in, d, err)) { LOGE("ImportDynamicMesh %s: %s", fname.c_str(), err.c_str()); return nullptr; }
    const float scale = opts.scale != 0 ? opts.scale : (opts.axis == GML_AXIS_BLENDER_OBJ ? 100.f : 1.f);
    const bool swapYZ = opts.axis == GML_AXIS_BLENDER_OBJ;
    auto toUE = [&](const float* p, double out[3]) {
        double x = p[0], y = p[1], z = p[2];
        if (swapYZ) std::swap(y, z);  // OBJ(x,y,z) -> UE(x,z,y)
        out[0] = x * scale + opts.offset[0];
        out[1] = y * scale + opts.offset[1];
        out[2] = z * scale + opts.offset[2];
    };
    std::vector<int> ids;
    int next = opts.materialCount;
    for (auto& g : d.groups) {
        int id = -1;
        for (int i = 0; i < opts.materialCount && id < 0; i++)
            if (opts.materialNames && opts.materialNames[i] && g == opts.materialNames[i]) id = i;
        ids.push_back(id >= 0 ? id : next++);
    }
    UObject* dyn = BuildDynamicMesh(d, toUE, swapYZ, opts.flipWinding == 1, ids);
    LOGI("ImportDynamicMesh %s -> %s", fname.c_str(), dyn ? FullName(dyn).c_str() : "FAILED");
    return dyn;
}

GUObject* ImportDynamicMesh(const wchar_t* file, const GML_MeshImport* opts) {
    if (!OnGameThread("ImportDynamicMesh") || !file) return nullptr;
    std::ifstream f(file, std::ios::binary);
    if (!f) { LOGE("ImportDynamicMesh %s: cannot open file", Narrow(file).c_str()); return nullptr; }
    return DynamicMeshFromObj(f, Narrow(file), opts);
}

GUObject* ImportDynamicMeshFromMemory(const void* objText, size_t size, const char* name, const GML_MeshImport* opts) {
    if (!OnGameThread("ImportDynamicMeshFromMemory") || !objText || !size) return nullptr;
    MemBuf buf(objText, size);
    std::istream in(&buf);
    return DynamicMeshFromObj(in, name ? name : "<memory>", opts);
}

// Adds a UDynamicMeshComponent to `actor` (attached to its root), gives it a copy of `mesh`, the
// materials by material ID, externally-provided (MikkT) tangents and no collision.
GUObject* AddDynamicMeshComponent(GUObject* actor, GUObject* mesh, GUObject* const* materials, int count) {
    if (!OnGameThread("AddDynamicMeshComponent") || !actor || !mesh) return nullptr;
    UClass* compClass = FindClass("DynamicMeshComponent");
    if (!compClass) { LOGE("DynamicMeshComponent is not available in this build"); return nullptr; }

    Params mt(Lib("KismetMathLibrary"), "MakeTransform");  // identity, built by the engine
    double one[3] = {1, 1, 1};
    if (void* s = mt.Ptr("Scale")) memcpy(s, one, sizeof one);
    mt.Call();

    Params add(actor, "AddComponentByClass");
    *(UClass**)add.Ptr("Class") = compClass;
    *(bool*)add.Ptr("bManualAttachment") = false;
    memcpy(add.Ptr("RelativeTransform"), mt.Ret(), mt.RetSize());
    *(bool*)add.Ptr("bDeferredFinish") = false;
    add.Call(true);
    UObject* comp = add.RetAs<UObject*>();
    if (!comp) { LOGE("AddDynamicMeshComponent: AddComponentByClass failed on %s", FullName(actor).c_str()); return nullptr; }

    if (FProperty* tp = FindProperty((UStruct*)ClassOf(comp), "TangentsType"))
        *((uint8_t*)comp + PropOffset(tp)) = 2;  // ExternallyProvided: the MikkT tangents computed at import

    Params gm(comp, "GetDynamicMesh");
    gm.Call();
    UObject* target = gm.RetAs<UObject*>();
    Params ap(Lib("GeometryScriptLibrary_MeshBasicEditFunctions"), "AppendMesh");
    ap.Set(0, target);
    ap.Set(1, mesh);
    memcpy(ap.Ptr("AppendTransform"), mt.Ret(), mt.RetSize());  // AppendOptions zeroed = keep all attributes
    ap.Call();

    std::vector<UObject*> mats(materials, materials + (count > 0 ? count : 0));
    Params cm(comp, "ConfigureMaterialSet");
    *(GML_TArray*)cm.Ptr("NewMaterialSet") = GML_TArray{mats.data(), (int32_t)mats.size(), (int32_t)mats.size()};
    *(bool*)cm.Ptr("bDeleteExtraSlots") = true;
    cm.Call();

    Params col(comp, "SetCollisionEnabled");
    col.Set(0, (uint8_t)0);  // NoCollision: rendering only
    col.Call();
    Params nm(comp, "NotifyMeshModified");
    nm.Call();
    auto triCount = [](UObject* m) {
        Params tc(m, "GetTriangleCount");
        tc.Call();
        return tc.RetAs<int32_t>();
    };
    int srcTris = triCount(mesh), dstTris = triCount(target);
    if (dstTris <= 0) LOGW("AddDynamicMeshComponent: the copy on %s is empty (source %s has %d triangles)",
                           ObjName(actor).c_str(), FullName(mesh).c_str(), srcTris);
    LOGI("AddDynamicMeshComponent: %s on %s (%d triangles)", FullName(comp).c_str(), ObjName(actor).c_str(), dstTris);
    return comp;
}

}  // namespace gml::assets
