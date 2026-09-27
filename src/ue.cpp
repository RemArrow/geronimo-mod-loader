#include "ue.h"
#include "signatures.h"
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

namespace gml::ue {

void* g_processEvent = nullptr;
void (*g_peRaw)(UObject*, UFunction*, void*) = nullptr;

static uint8_t* s_gobjects = nullptr;
static void (*s_appendString)(const uint64_t* name, GML_FString* out) = nullptr;

static uint8_t* Locate(const char* what, uint32_t rva, const char* sig) {
    uint8_t* at = (uint8_t*)g_imageBase + rva;
    if (rva && BytesMatch(at, sig)) {
        LOGI("%s at +0x%X (configured offset verified)", what, rva);
        return at;
    }
    int n = 0;
    uint8_t* hit = FindPattern(TextSection(g_imageBase), sig, &n);
    if (n == 1) {
        LOGW("%s: configured +0x%X did not match; signature found it at +0x%llX. Update GML\\config\\GML.cfg [Offsets].",
             what, rva, (unsigned long long)(hit - (uint8_t*)g_imageBase));
        return hit;
    }
    LOGE("%s: configured +0x%X does not match and signature scan found %d candidates. "
         "The game was probably updated - re-run Dumper-7 and update GML\\config\\GML.cfg [Offsets].", what, rva, n);
    return nullptr;
}

bool ResolveOffsets() {
    uint8_t* pe = Locate("UObject::ProcessEvent", g_cfg.rvaProcessEvent, sig::ProcessEvent);
    uint8_t* as = Locate("FName::AppendString", g_cfg.rvaAppendString, sig::AppendString);
    if (!pe || !as) return false;
    g_processEvent = pe;
    g_peRaw = (decltype(g_peRaw))pe;
    s_appendString = (decltype(s_appendString))as;
    s_gobjects = (uint8_t*)g_imageBase + g_cfg.rvaGObjects;
    return true;
}

// ------------------------------------------------------------------ GObjects

static UObject* ObjectAtRaw(int i) {
    int num = At<int32_t>(s_gobjects, L::ArrNumElements);
    if (i < 0 || i >= num) return nullptr;
    uint8_t** chunks = At<uint8_t**>(s_gobjects, L::ArrObjects);
    uint8_t* chunk = chunks ? chunks[i / L::PerChunk] : nullptr;
    if (!chunk) return nullptr;
    return At<UObject*>(chunk + (i % L::PerChunk) * L::ItemSize, L::ItemObject);
}

int ObjectCount() { return s_gobjects ? At<int32_t>(s_gobjects, L::ArrNumElements) : 0; }
UObject* ObjectAt(int i) { return s_gobjects ? ObjectAtRaw(i) : nullptr; }

static bool IsValidRaw(UObject* o) {
    __try {
        if (!o) return false;
        int idx = At<int32_t>(o, L::ObjIndex);
        return ObjectAtRaw(idx) == o && ClassOf(o) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool IsValid(UObject* o) { return s_gobjects && IsValidRaw(o); }

static int ProbeGObjects(UObject** first) {
    __try {
        int num = At<int32_t>(s_gobjects, L::ArrNumElements);
        int chunks = At<int32_t>(s_gobjects, L::ArrNumChunks);
        if (num < 1000 || num > 50'000'000 || chunks < 1 || chunks > 1024) return -1;
        UObject* o = ObjectAtRaw(0);
        if (!o || At<int32_t>(o, L::ObjIndex) != 0 || !ClassOf(o) || !ClassOf((UObject*)ClassOf(o))) return -2;
        *first = o;
        return num;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}

bool ValidateGObjects() {
    UObject* first = nullptr;
    int r = ProbeGObjects(&first);
    if (r < 0) {
        LOGE("GObjects at +0x%X failed validation (code %d). Engine features disabled - "
             "re-run Dumper-7 and update GML\\config\\GML.cfg [Offsets] GObjects.", g_cfg.rvaGObjects, r);
        s_gobjects = nullptr;
        return false;
    }
    std::string n0 = ObjName(first);
    LOGI("GObjects OK: %d objects, [0] = %s", r, n0.c_str());
    if (n0 != "/Script/CoreUObject") LOGW("object 0 is usually /Script/CoreUObject - check layouts");
    return true;
}

// ------------------------------------------------------------------ names

static std::shared_mutex s_nameMtx;
static std::unordered_map<uint64_t, std::string> s_names;

std::string NameStr(uint64_t fname) {
    {
        std::shared_lock g(s_nameMtx);
        auto it = s_names.find(fname);
        if (it != s_names.end()) return it->second;
    }
    wchar_t buf[1024];
    GML_FString fs{buf, 0, 1024};
    s_appendString(&fname, &fs);
    std::wstring w(buf, fs.Num > 0 ? fs.Num - (buf[fs.Num - 1] == 0 ? 1 : 0) : 0);
    std::string s = Narrow(w);
    std::unique_lock g(s_nameMtx);
    s_names.emplace(fname, s);
    return s;
}

std::string ObjName(UObject* o) { return o ? NameStr(At<uint64_t>(o, L::ObjName)) : "None"; }

static UClass* s_packageClass = nullptr;
static bool IsPackage(UObject* o) {
    UClass* c = ClassOf(o);
    if (s_packageClass) return c == s_packageClass;
    if (ObjName((UObject*)c) == "Package") { s_packageClass = c; return true; }
    return false;
}

std::string PathName(UObject* o) {
    if (!o) return "None";
    UObject* outer = OuterOf(o);
    if (!outer) return ObjName(o);
    std::string r = PathName(outer);
    UObject* outerOuter = OuterOf(outer);
    r += (!IsPackage(outer) && outerOuter && IsPackage(outerOuter)) ? ':' : '.';
    return r + ObjName(o);
}

std::string FullName(UObject* o) {
    if (!o) return "None";
    return ObjName((UObject*)ClassOf(o)) + " " + PathName(o);
}

bool IsChildOf(UStruct* s, UStruct* base) {
    for (; s; s = SuperOf(s)) if (s == base) return true;
    return false;
}
bool IsA(UObject* o, UClass* c) { return o && c && IsChildOf((UStruct*)ClassOf(o), (UStruct*)c); }

// ------------------------------------------------------------------ lookup

static std::mutex s_findMtx;
static std::unordered_map<std::string, UObject*> s_findCache;

static std::string Leaf(const std::string& path) {
    size_t p = path.find_last_of(".:");
    return p == std::string::npos ? path : path.substr(p + 1);
}

UObject* FindObject(const std::string& path) {
    {
        std::lock_guard g(s_findMtx);
        auto it = s_findCache.find(path);
        if (it != s_findCache.end() && IsValid(it->second) && PathName(it->second) == path) return it->second;
    }
    const std::string leaf = Leaf(path);
    const int n = ObjectCount();
    for (int i = 0; i < n; i++) {
        UObject* o = ObjectAt(i);
        if (!o || ObjName(o) != leaf) continue;
        if (PathName(o) == path) {
            std::lock_guard g(s_findMtx);
            s_findCache[path] = o;
            return o;
        }
    }
    return nullptr;
}

static UClass* s_classClass = nullptr;  // UClass's own class object ("Class")
static bool IsClassObject(UObject* o) {
    if (!s_classClass) {
        UObject* c = FindObject("/Script/CoreUObject.Class");
        if (!c) return false;
        s_classClass = (UClass*)c;
    }
    return IsA(o, s_classClass);
}

UClass* FindClass(const std::string& nameOrPath) {
    if (nameOrPath.find('/') != std::string::npos) {
        UObject* o = FindObject(nameOrPath);
        return (o && IsClassObject(o)) ? (UClass*)o : nullptr;
    }
    const std::string key = "class:" + nameOrPath;
    {
        std::lock_guard g(s_findMtx);
        auto it = s_findCache.find(key);
        if (it != s_findCache.end() && IsValid(it->second)) return (UClass*)it->second;
    }
    const int n = ObjectCount();
    for (int i = 0; i < n; i++) {
        UObject* o = ObjectAt(i);
        if (!o || ObjName(o) != nameOrPath || !IsClassObject(o)) continue;
        std::lock_guard g(s_findMtx);
        s_findCache[key] = o;
        return (UClass*)o;
    }
    return nullptr;
}

static bool IsTemplate(UObject* o) { return (FlagsOf(o) & (RF_ClassDefaultObject | RF_ArchetypeObject)) != 0; }

UObject* FindFirstOf(UClass* c) {
    if (!c) return nullptr;
    const int n = ObjectCount();
    for (int i = 0; i < n; i++) {
        UObject* o = ObjectAt(i);
        if (o && !IsTemplate(o) && IsA(o, c)) return o;
    }
    return nullptr;
}

int FindAllOf(UClass* c, std::vector<UObject*>& out) {
    if (!c) return 0;
    const int n = ObjectCount();
    for (int i = 0; i < n; i++) {
        UObject* o = ObjectAt(i);
        if (o && !IsTemplate(o) && IsA(o, c)) out.push_back(o);
    }
    return (int)out.size();
}

UObject* DefaultObject(UClass* c) {
    if (!c) return nullptr;
    const std::string key = "cdo:" + PathName((UObject*)c);
    {
        std::lock_guard g(s_findMtx);
        auto it = s_findCache.find(key);
        if (it != s_findCache.end() && IsValid(it->second)) return it->second;
    }
    const int n = ObjectCount();
    for (int i = 0; i < n; i++) {
        UObject* o = ObjectAt(i);
        if (o && ClassOf(o) == c && (FlagsOf(o) & RF_ClassDefaultObject)) {
            std::lock_guard g(s_findMtx);
            s_findCache[key] = o;
            return o;
        }
    }
    return nullptr;
}

UObject* Lib(const char* className) { return DefaultObject(FindClass(className)); }

// ------------------------------------------------------------------ reflection

std::string PropName(FProperty* p) { return NameStr(At<uint64_t>(p, L::FFName)); }
std::string PropType(FProperty* p) {
    void* fc = At<void*>(p, L::FFClass);
    return fc ? NameStr(At<uint64_t>(fc, L::FFClassName)) : "";
}

UFunction* FindFunction(UStruct* s, const std::string& name) {
    for (; s; s = SuperOf(s))
        for (UObject* f = At<UObject*>(s, L::StructChildren); f; f = At<UObject*>(f, L::FieldNext))
            if (ObjName(f) == name) return (UFunction*)f;
    return nullptr;
}

// Blueprint struct members are stored as Name_<n>_<32 hex GUID>.
static bool MangledMatch(const std::string& full, const std::string& want) {
    if (full.size() < want.size() + 35 || full.compare(0, want.size(), want) != 0 || full[want.size()] != '_')
        return false;
    size_t us = full.rfind('_');
    return us != std::string::npos && full.size() - us - 1 == 32 && us > want.size();
}

FProperty* FindProperty(UStruct* s, const std::string& name) {
    FProperty* mangled = nullptr;
    for (; s; s = SuperOf(s))
        for (FProperty* p = At<FProperty*>(s, L::StructChildProps); p; p = At<FProperty*>(p, L::FFNext)) {
            std::string n = PropName(p);
            if (n == name) return p;
            if (!mangled && MangledMatch(n, name)) mangled = p;
        }
    return mangled;
}

FProperty* NextProperty(UStruct* s, FProperty* prev) {
    return prev ? At<FProperty*>(prev, L::FFNext) : At<FProperty*>(s, L::StructChildProps);
}

bool PropInfo(FProperty* p, GML_PropInfo* o) {
    if (!p || !o) return false;
    static std::mutex m;
    static std::unordered_map<std::string, std::string> interned;  // stable c_str() for `type`
    std::string t = PropType(p);
    {
        std::lock_guard g(m);
        o->type = interned.emplace(t, t).first->second.c_str();
    }
    o->offset = PropOffset(p);
    o->size = PropSize(p);
    o->arrayDim = At<int32_t>(p, L::PropArrayDim);
    o->flags = PropFlags(p);
    o->boolByteOffset = o->boolFieldMask = 0;
    o->structType = nullptr; o->propertyClass = nullptr;
    o->inner = o->key = o->value = nullptr;
    if (t == "BoolProperty") {
        o->boolByteOffset = At<uint8_t>(p, 0x71);
        o->boolFieldMask = At<uint8_t>(p, 0x73);
    } else if (t == "StructProperty") {
        o->structType = At<UStruct*>(p, L::PropSub0);
    } else if (t == "ObjectProperty" || t == "ClassProperty" || t == "WeakObjectProperty" ||
               t == "SoftObjectProperty" || t == "SoftClassProperty" || t == "LazyObjectProperty" ||
               t == "InterfaceProperty") {
        o->propertyClass = At<UClass*>(p, L::PropSub0);
    } else if (t == "ArrayProperty") {
        o->inner = At<FProperty*>(p, L::PropSub1);
    } else if (t == "SetProperty") {
        o->inner = At<FProperty*>(p, L::PropSub0);
    } else if (t == "MapProperty") {
        o->key = At<FProperty*>(p, L::PropSub0);
        o->value = At<FProperty*>(p, L::PropSub1);
    }
    return true;
}

// ------------------------------------------------------------------ calling

void ProcessEvent(UObject* o, UFunction* f, void* params, bool hooked) {
    auto fn = hooked ? (decltype(g_peRaw))g_processEvent : g_peRaw;
    fn(o, f, params);
}

Params::Params(UObject* target, const char* func) : obj(target) {
    if (!target) return;
    fn = FindFunction((UStruct*)ClassOf(target), func);
    if (!fn) {
        LOGE("function %s not found on %s", func, FullName(target).c_str());
        return;
    }
    buf.assign((size_t)StructSize((UStruct*)fn) + 16, 0);
}

void* Params::Ptr(const char* name) {
    if (!fn) return nullptr;
    FProperty* p = FindProperty((UStruct*)fn, name);
    return p ? buf.data() + PropOffset(p) : nullptr;
}

static FProperty* NthArg(UFunction* fn, int index) {
    int i = 0;
    for (FProperty* p = At<FProperty*>(fn, L::StructChildProps); p; p = At<FProperty*>(p, L::FFNext)) {
        uint64_t fl = PropFlags(p);
        if (!(fl & CPF_Parm) || (fl & CPF_ReturnParm)) continue;
        if (i++ == index) return p;
    }
    return nullptr;
}
static FProperty* RetProp(UFunction* fn) {
    for (FProperty* p = At<FProperty*>(fn, L::StructChildProps); p; p = At<FProperty*>(p, L::FFNext))
        if (PropFlags(p) & CPF_ReturnParm) return p;
    return nullptr;
}

void* Params::Arg(int index) {
    FProperty* p = fn ? NthArg(fn, index) : nullptr;
    return p ? buf.data() + PropOffset(p) : nullptr;
}
int Params::ArgSize(int index) {
    FProperty* p = fn ? NthArg(fn, index) : nullptr;
    return p ? PropSize(p) : 0;
}
void* Params::Ret() {
    FProperty* p = fn ? RetProp(fn) : nullptr;
    return p ? buf.data() + PropOffset(p) : nullptr;
}
int Params::RetSize() {
    FProperty* p = fn ? RetProp(fn) : nullptr;
    return p ? PropSize(p) : 0;
}

bool Params::Call(bool hooked) {
    if (!fn || !obj) return false;
    ProcessEvent(obj, fn, buf.data(), hooked);
    return true;
}

// ------------------------------------------------------------------ engine helpers

uint64_t MakeName(const std::string& s) {
    Params p(Lib("KismetStringLibrary"), "Conv_StringToName");
    FStr str(Widen(s));
    p.SetStr(0, str);
    return p.Call() ? p.RetAs<uint64_t>() : 0;
}

bool MakeText(const std::string& s, void* out16) {
    Params p(Lib("KismetTextLibrary"), "Conv_StringToText");
    FStr str(Widen(s));
    p.SetStr(0, str);
    if (!p.Call() || p.RetSize() != 16) return false;
    memcpy(out16, p.Ret(), 16);  // we take over the returned reference
    return true;
}

std::string FStringToUtf8(const GML_FString* s) {
    if (!s || !s->Data || s->Num <= 0) return {};
    return Narrow(std::wstring(s->Data, s->Num - 1));
}

std::string TextToString(const void* ftext) {
    Params p(Lib("KismetTextLibrary"), "Conv_TextToString");
    if (!p || p.ArgSize(0) != 16) return {};
    memcpy(p.Arg(0), ftext, 16);
    if (!p.Call()) return {};
    return FStringToUtf8((GML_FString*)p.Ret());
}

UObject* LoadObject(const std::string& path, bool asClass) {
    // path string -> FSoft{Object,Class}Path -> TSoft{Object,Class}Ptr -> blocking load
    UObject* ksl = Lib("KismetSystemLibrary");
    Params mk(ksl, asClass ? "MakeSoftClassPath" : "MakeSoftObjectPath");
    FStr str(Widen(path));
    mk.SetStr(0, str);
    if (!mk.Call()) return nullptr;

    Params conv(ksl, asClass ? "Conv_SoftClassPathToSoftClassRef" : "Conv_SoftObjPathToSoftObjRef");
    if (!conv || conv.ArgSize(0) != mk.RetSize()) return nullptr;
    memcpy(conv.Arg(0), mk.Ret(), mk.RetSize());
    if (!conv.Call()) return nullptr;

    Params load(ksl, asClass ? "LoadClassAsset_Blocking" : "LoadAsset_Blocking");
    if (!load || load.ArgSize(0) != conv.RetSize()) return nullptr;
    memcpy(load.Arg(0), conv.Ret(), conv.RetSize());
    if (!load.Call()) return nullptr;
    UObject* r = load.RetAs<UObject*>();
    if (!r) LOGW("LoadObject: '%s' did not load", path.c_str());
    return r;
}

UObject* WorldContext() {
    static UObject* gi = nullptr;
    if (gi && IsValid(gi)) return gi;
    gi = FindFirstOf(FindClass("GameInstance"));
    return gi;
}

UObject* NewObject(UClass* c, UObject* outer) {
    if (!outer) outer = WorldContext();
    Params p(Lib("GameplayStatics"), "SpawnObject");
    p.Set(0, c);
    p.Set(1, outer);
    return p.Call() ? p.RetAs<UObject*>() : nullptr;
}

// FMemory-owned memory without knowing where FMemory lives: KismetStringLibrary::LeftPad
// returns an FString whose buffer the engine allocated. We keep the buffer.
void* EngineAlloc(size_t bytes) {
    Params p(Lib("KismetStringLibrary"), "LeftPad");
    if (!p) return nullptr;
    static wchar_t empty[1] = {0};
    GML_FString src{empty, 1, 1};
    p.Set(0, src);
    p.Set(1, (int32_t)(bytes / 2 + 1));
    if (!p.Call()) return nullptr;
    GML_FString* r = (GML_FString*)p.Ret();
    if (!r->Data || (size_t)r->Max * 2 < bytes) return nullptr;
    memset(r->Data, 0, bytes);
    return r->Data;
}

void* ArrayAdd(GML_TArray* arr, int elemSize, int count) {
    if (!arr || elemSize <= 0 || count <= 0) return nullptr;
    int need = arr->Num + count;
    if (need > arr->Max) {
        int newMax = need > arr->Max * 2 ? need : arr->Max * 2;
        if (newMax < 4) newMax = 4;
        void* nd = EngineAlloc((size_t)newMax * elemSize);
        if (!nd) return nullptr;
        if (arr->Data && arr->Num) memcpy(nd, arr->Data, (size_t)arr->Num * elemSize);
        // The old buffer is leaked: freeing it needs FMemory::Free, which we don't resolve.
        arr->Data = nd;
        arr->Max = newMax;
    }
    uint8_t* first = (uint8_t*)arr->Data + (size_t)arr->Num * elemSize;
    memset(first, 0, (size_t)count * elemSize);
    arr->Num = need;
    return first;
}

// TMap = TSet<TPair<K,V>> = TSparseArray (TArray of elements + TBitArray allocation flags).
int MapForEach(void* map, FProperty* mapProp, int (*cb)(void*, void*, void*), void* user) {
    if (!map || !mapProp || PropType(mapProp) != "MapProperty") return 0;
    FProperty* valProp = At<FProperty*>(mapProp, L::PropSub1);
    const int valOff = PropOffset(valProp);
    const int* lay = (const int*)((uint8_t*)mapProp + L::MapLayout);
    // FScriptMapLayout { ValueOffset; SetLayout { HashNextIdOffset; HashIndexOffset; Size } }
    int stride = 0;
    if (lay[0] == valOff && lay[3] >= valOff + PropSize(valProp) + 8) stride = lay[3];
    else {
        int end = valOff + PropSize(valProp);
        stride = ((end + 3) & ~3) + 8;
        stride = (stride + 7) & ~7;
        LOGW("MapForEach: map layout not recognised, assuming element stride %d", stride);
    }
    uint8_t* data = At<uint8_t*>(map, 0x00);
    int num = At<int32_t>(map, 0x08);
    // TBitArray<TInlineAllocator<4>>: uint32 inline[4] @0x10, uint32* secondary @0x20
    uint32_t* bits = At<uint32_t*>(map, 0x20);
    if (!bits) bits = (uint32_t*)((uint8_t*)map + 0x10);
    int visited = 0;
    for (int i = 0; i < num; i++) {
        if (!(bits[i / 32] & (1u << (i % 32)))) continue;
        uint8_t* e = data + (size_t)i * stride;
        visited++;
        if (cb(user, e, e + valOff)) break;
    }
    return visited;
}

void KeepAlive(UObject* o) {
    UObject* gi = WorldContext();
    if (!gi || !o) return;
    static FProperty* prop = nullptr;
    if (!prop) prop = FindProperty((UStruct*)ClassOf(gi), "ReferencedObjects");
    if (!prop) { LOGE("KeepAlive: GameInstance.ReferencedObjects not found"); return; }
    auto* arr = (GML_TArray*)((uint8_t*)gi + PropOffset(prop));
    for (int i = 0; i < arr->Num; i++) if (((UObject**)arr->Data)[i] == o) return;
    if (auto* slot = (UObject**)ArrayAdd(arr, sizeof(void*), 1)) *slot = o;
}

}  // namespace gml::ue
