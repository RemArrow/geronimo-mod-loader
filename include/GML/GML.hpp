// GML.hpp - header-only C++20 convenience layer over GML.h, BepInEx-style.
//
//   #include <GML/GML.hpp>
//
//   GML_PLUGIN("com.you.myplugin", "My Plugin", "1.0.0");     // like [BepInPlugin(...)]
//
//   GML_AWAKE() {                                             // like Awake(); engine is up
//       auto greeting = gml::Config.Bind<std::string>("General", "Greeting", "hello",
//                                                     "What to log on startup");
//       gml::Log("{} from C++", greeting.Value());
//       gml::HookAfter("*:ReceiveBeginPlay", [](gml::Object self, GUFunction*, void*) {
//           gml::Log("BeginPlay: {}", self.FullName());
//       });
//       return 0;
//   }
//
// Dependencies go after the version, like [BepInDependency]:
//   GML_PLUGIN("com.you.b", "B", "1.0.0", {{"com.you.a", "1.2.0", GML_DEPENDENCY_HARD}});
// Incompatibilities (like [BepInIncompatibility]) come after the dependency list:
//   GML_PLUGIN("com.you.b", "B", "1.0.0", {}, {"com.someone.conflicting"});
#pragma once
#include "GML.h"
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

namespace gml {

inline const GML_API* API = nullptr;
inline GML_Plugin* Self = nullptr;

inline bool Init(const GML_API* api, GML_Plugin* self) {
    API = api;
    Self = self;
    return api && api->version >= GML_API_VERSION && api->size >= sizeof(GML_API);
}

// ------------------------------------------------------------------ logging (BepInEx levels)

template <class... A> void LogAt(GML_LogLevel l, std::format_string<A...> f, A&&... a) {
    API->Log(Self, l, std::format(f, std::forward<A>(a)...).c_str());
}
template <class... A> void Log(std::format_string<A...> f, A&&... a) { LogAt(GML_LOG_INFO, f, std::forward<A>(a)...); }
template <class... A> void Message(std::format_string<A...> f, A&&... a) { LogAt(GML_LOG_MESSAGE, f, std::forward<A>(a)...); }
template <class... A> void Warn(std::format_string<A...> f, A&&... a) { LogAt(GML_LOG_WARNING, f, std::forward<A>(a)...); }
template <class... A> void Error(std::format_string<A...> f, A&&... a) { LogAt(GML_LOG_ERROR, f, std::forward<A>(a)...); }
template <class... A> void Debug(std::format_string<A...> f, A&&... a) { LogAt(GML_LOG_DEBUG, f, std::forward<A>(a)...); }

// ------------------------------------------------------------------ paths

// The plugin's own folder (where its DLL is), optionally joined with a relative path.
inline std::wstring PluginPath(const std::wstring& rel = L"") {
    std::wstring d = API->PluginDir(Self);
    return rel.empty() ? d : d + L"\\" + rel;
}
inline std::wstring GetPath(GML_Path which) { return API->GetPath(which); }

// ------------------------------------------------------------------ config (like BepInEx ConfigFile)

namespace detail {
template <class T> constexpr GML_ConfigType ConfigTypeOf() {
    if constexpr (std::is_same_v<T, bool>) return GML_CONFIG_BOOL;
    else if constexpr (std::is_integral_v<T>) return GML_CONFIG_INT;
    else if constexpr (std::is_floating_point_v<T>) return GML_CONFIG_FLOAT;
    else return GML_CONFIG_STRING;
}
template <class T> std::string ConfigToString(const T& v) {
    if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
    else if constexpr (std::is_arithmetic_v<T>) return std::format("{}", v);
    else return std::string(v);
}
}  // namespace detail

template <class T> class ConfigEntry {
public:
    ConfigEntry(GML_ConfigEntry* e = nullptr) : e_(e) {}
    T Value() const {
        if constexpr (std::is_same_v<T, bool>) return API->ConfigGetBool(e_) != 0;
        else if constexpr (std::is_integral_v<T>) return (T)API->ConfigGetInt(e_);
        else if constexpr (std::is_floating_point_v<T>) return (T)API->ConfigGetFloat(e_);
        else {
            char buf[1024];
            int n = API->ConfigGetString(e_, buf, sizeof buf);
            if (n < (int)sizeof buf) return buf;
            std::string s(n + 1, '\0');
            API->ConfigGetString(e_, s.data(), n + 1);
            s.resize(n);
            return s;
        }
    }
    void Set(const T& v) const { API->ConfigSet(e_, detail::ConfigToString(v).c_str()); }
    explicit operator bool() const { return e_ != nullptr; }

private:
    GML_ConfigEntry* e_;
};

// GML\config\<GUID>.cfg - created/updated with defaults and descriptions as settings are bound.
struct ConfigFile {
    template <class T>
    ConfigEntry<T> Bind(const char* section, const char* key, const T& defaultValue, const char* description = "") const {
        return ConfigEntry<T>(API->ConfigBind(Self, section, key, detail::ConfigTypeOf<T>(),
                                              detail::ConfigToString(defaultValue).c_str(), description));
    }
    ConfigEntry<std::string> Bind(const char* section, const char* key, const char* defaultValue,
                                  const char* description = "") const {
        return Bind<std::string>(section, key, std::string(defaultValue), description);
    }
};
inline const ConfigFile Config;

// ------------------------------------------------------------------ strings

// An engine FString backed by our own memory; valid as an input parameter.
struct FStr {
    std::wstring s;
    GML_FString raw{};
    FStr(std::wstring w = L"") : s(std::move(w)) { raw = {s.data(), (int32_t)s.size() + 1, (int32_t)s.size() + 1}; }
    FStr(const FStr& o) : FStr(o.s) {}
};

// ------------------------------------------------------------------ objects

struct Object {
    GUObject* ptr = nullptr;
    Object() = default;
    Object(GUObject* p) : ptr(p) {}
    Object(GUClass* p) : ptr((GUObject*)p) {}
    explicit operator bool() const { return ptr && API->IsValid(ptr); }
    bool operator==(const Object& o) const { return ptr == o.ptr; }

    std::string Name() const { return Str(API->GetName); }
    std::string Path() const { return Str(API->GetPathName); }
    std::string FullName() const { return Str(API->GetFullName); }
    GUClass* Class() const { return ptr ? API->GetClass(ptr) : nullptr; }
    GUStruct* Struct() const { return (GUStruct*)Class(); }
    bool IsA(GUClass* c) const { return ptr && c && API->IsA(ptr, c); }
    bool IsA(const char* cls) const { return IsA(API->FindClass(cls)); }

    // Address of a property's value inside this object (nullptr if the property is absent).
    void* PropPtr(const char* name) const {
        GFProperty* p = ptr ? API->FindProperty(Struct(), name) : nullptr;
        if (!p) return nullptr;
        GML_PropInfo i;
        API->GetPropertyInfo(p, &i);
        return (uint8_t*)ptr + i.offset;
    }
    template <class T> T Get(const char* prop) const {
        T v{};
        if (void* p = PropPtr(prop)) std::memcpy(&v, p, sizeof(T));
        return v;
    }
    template <class T> bool Set(const char* prop, const T& v) const {
        void* p = PropPtr(prop);
        if (p) std::memcpy(p, &v, sizeof(T));
        return p != nullptr;
    }
    bool GetBool(const char* prop) const {
        GML_PropInfo i;
        if (!Info(prop, i)) return false;
        return (((uint8_t*)ptr)[i.offset + i.boolByteOffset] & i.boolFieldMask) != 0;
    }
    bool SetBool(const char* prop, bool v) const {
        GML_PropInfo i;
        if (!Info(prop, i)) return false;
        uint8_t& b = ((uint8_t*)ptr)[i.offset + i.boolByteOffset];
        b = v ? (b | i.boolFieldMask) : (b & ~i.boolFieldMask);
        return true;
    }
    Object GetObj(const char* prop) const { return Object(Get<GUObject*>(prop)); }
    bool Call(const char* func, void* params = nullptr) const { return ptr && API->CallFunction(ptr, func, params); }

    bool Info(const char* prop, GML_PropInfo& out) const {
        GFProperty* p = ptr ? API->FindProperty(Struct(), prop) : nullptr;
        return p && API->GetPropertyInfo(p, &out);
    }

private:
    std::string Str(int (*f)(GUObject*, char*, int)) const {
        if (!ptr) return "None";
        char buf[512];
        int n = f(ptr, buf, sizeof buf);
        if (n < (int)sizeof buf) return buf;
        std::string s(n + 1, '\0');
        f(ptr, s.data(), n + 1);
        s.resize(n);
        return s;
    }
};

inline Object FindObject(const char* path) { return API->FindObject(path); }
inline GUClass* FindClass(const char* name) { return API->FindClass(name); }
inline Object FindFirstOf(const char* cls) { return API->FindFirstOf(cls); }
inline std::vector<Object> FindAllOf(const char* cls) {
    int n = API->FindAllOf(cls, nullptr, 0);
    std::vector<GUObject*> raw(n);
    if (n) API->FindAllOf(cls, raw.data(), n);
    return {raw.begin(), raw.end()};
}
inline Object LoadObject(const char* path) { return API->LoadObject(path); }
inline GUClass* LoadClass(const char* path) { return API->LoadClass(path); }
inline Object NewObject(GUClass* cls, Object outer = {}) { return API->NewObject(cls, outer.ptr); }
inline Object DefaultObject(GUClass* cls) { return API->DefaultObject(cls); }

// ------------------------------------------------------------------ calling UFunctions

// A zeroed parameter block for one UFunction, filled by parameter name.
class Params {
public:
    Params(Object target, const char* func) : obj_(target) {
        fn_ = target.ptr ? API->FindFunction(target.Struct(), func) : nullptr;
        if (fn_) buf_.assign((size_t)API->StructSize((GUStruct*)fn_) + 16, 0);
        else if (API) Error("function '{}' not found on {}", func, target.FullName());
    }
    explicit operator bool() const { return fn_ != nullptr; }
    void* Ptr(const char* name) {
        GFProperty* p = fn_ ? API->FindProperty((GUStruct*)fn_, name) : nullptr;
        if (!p) return nullptr;
        GML_PropInfo i;
        API->GetPropertyInfo(p, &i);
        return buf_.data() + i.offset;
    }
    template <class T> Params& Set(const char* name, const T& v) {
        if (void* p = Ptr(name)) std::memcpy(p, &v, sizeof(T));
        else if (fn_) Error("parameter '{}' not found", name);
        return *this;
    }
    Params& SetString(const char* name, const std::wstring& s) {
        strings_.emplace_back(s);
        return Set(name, strings_.back().raw);
    }
    Params& SetObj(const char* name, Object o) { return Set(name, o.ptr); }
    template <class T> T Get(const char* name) {
        T v{};
        if (void* p = Ptr(name)) std::memcpy(&v, p, sizeof(T));
        return v;
    }
    template <class T> T Return() { return Get<T>("ReturnValue"); }
    bool Call() {
        if (!fn_ || !obj_.ptr) return false;
        API->ProcessEvent(obj_.ptr, fn_, buf_.data());
        return true;
    }
    void* Data() { return buf_.data(); }

private:
    Object obj_;
    GUFunction* fn_ = nullptr;
    std::vector<uint8_t> buf_;
    std::vector<FStr> strings_;  // keeps SetString buffers alive until the call
};

// Static BlueprintFunctionLibrary call target, e.g. Lib("KismetSystemLibrary").
inline Object Lib(const char* cls) { return API->DefaultObject(API->FindClass(cls)); }

// Read a parameter out of a hook's param block.
template <class T> T Param(GUFunction* fn, void* params, const char* name) {
    T v{};
    GFProperty* p = API->FindProperty((GUStruct*)fn, name);
    GML_PropInfo i;
    if (p && API->GetPropertyInfo(p, &i)) std::memcpy(&v, (uint8_t*)params + i.offset, sizeof(T));
    return v;
}

// ------------------------------------------------------------------ events, tasks, hooks

namespace detail {
struct HookFns {
    std::function<bool(Object, GUFunction*, void*)> pre;
    std::function<void(Object, GUFunction*, void*)> post;
};
inline int PreThunk(void* u, GUObject* s, GUFunction* f, void* p) {
    auto* h = (HookFns*)u;
    return h->pre && h->pre(Object(s), f, p) ? 1 : 0;
}
inline int PostThunk(void* u, GUObject* s, GUFunction* f, void* p) {
    auto* h = (HookFns*)u;
    if (h->post) h->post(Object(s), f, p);
    return 0;
}
}  // namespace detail

// pre: return true to skip the original. Either callback may be empty.
inline int Hook(const char* spec, std::function<bool(Object self, GUFunction* fn, void* params)> pre,
                std::function<void(Object self, GUFunction* fn, void* params)> post = {}) {
    auto* h = new detail::HookFns{std::move(pre), std::move(post)};  // lives for the process
    return API->HookFunction(Self, spec, h->pre ? detail::PreThunk : nullptr, h->post ? detail::PostThunk : nullptr, h);
}
inline int HookAfter(const char* spec, std::function<void(Object self, GUFunction* fn, void* params)> post) {
    return Hook(spec, {}, std::move(post));
}

inline void On(GML_Event ev, std::function<void(void* data)> fn) {
    auto* f = new std::function<void(void*)>(std::move(fn));
    API->Subscribe(Self, ev, [](void* u, void* d) { (*(std::function<void(void*)>*)u)(d); }, f);
}

inline void RunOnGameThread(std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
    API->RunOnGameThread(Self, [](void* u) {
        auto* p = (std::function<void()>*)u;
        (*p)();
        delete p;
    }, f);
}

// ------------------------------------------------------------------ containers

template <class T> struct TArrayView {
    GML_TArray* a;
    int Num() const { return a ? a->Num : 0; }
    T& operator[](int i) const { return ((T*)a->Data)[i]; }
    T* begin() const { return a ? (T*)a->Data : nullptr; }
    T* end() const { return a ? (T*)a->Data + a->Num : nullptr; }
    bool Contains(const T& v) const { for (auto& x : *this) if (x == v) return true; return false; }
    bool Add(const T& v) const {
        T* slot = (T*)API->ArrayAdd(a, sizeof(T), 1);
        if (slot) std::memcpy((void*)slot, &v, sizeof(T));
        return slot != nullptr;
    }
};

}  // namespace gml

// Plugin metadata, read by the chainloader straight from the DLL file (like [BepInPlugin]).
// Optional trailing arguments: {dependencies...}, {incompatible GUIDs...}
#define GML_PLUGIN(guid, name, version, ...)                                               \
    extern "C" GML_EXPORT const GML_PluginInfo GML_PluginMetadata = {                      \
        sizeof(GML_PluginInfo), GML_API_VERSION, guid, name, version, __VA_ARGS__}

// Plugin entry point: called once the engine is up, in dependency order (like Awake()).
// Return 0 for success.
#define GML_AWAKE()                                                                        \
    static int GmlAwake();                                                                 \
    extern "C" GML_EXPORT int GML_Awake(const GML_API* api, GML_Plugin* self) {           \
        if (!gml::Init(api, self)) return 1;                                               \
        return GmlAwake();                                                                 \
    }                                                                                      \
    static int GmlAwake()

// Patcher entry point (DLL in GML\patchers\): process entry, before the engine starts.
#define GML_PATCH()                                                                        \
    static int GmlPatch();                                                                 \
    extern "C" GML_EXPORT int GML_Patch(const GML_API* api, GML_Plugin* self) {           \
        if (!gml::Init(api, self)) return 1;                                               \
        return GmlPatch();                                                                 \
    }                                                                                      \
    static int GmlPatch()
