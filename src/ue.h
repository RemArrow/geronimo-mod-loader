// Engine reflection core. Everything here works on raw memory using the UE 5.7.4
// layouts that Dumper-7 dumped from this exact build (CppSDK/SDK/Basic.hpp and
// CoreUObject_classes.hpp). If the game updates, re-run Dumper-7 and diff these.
#pragma once
#include "internal.h"
#include <string>
#include <vector>

namespace gml::ue {

namespace L {  // layout
// UObject
constexpr int ObjFlags = 0x08, ObjIndex = 0x0C, ObjClass = 0x10, ObjName = 0x18, ObjOuter = 0x20;
// UField / UStruct / UFunction
constexpr int FieldNext = 0x28;
constexpr int StructSuper = 0x40, StructChildren = 0x48, StructChildProps = 0x50, StructSize = 0x58;
constexpr int FuncFlags = 0xB0, FuncNative = 0xD8;
// FField / FFieldClass / FProperty
constexpr int FFClass = 0x08, FFNext = 0x18, FFName = 0x20;
constexpr int FFClassName = 0x00;
constexpr int PropArrayDim = 0x30, PropElemSize = 0x34, PropFlags = 0x38, PropOffset = 0x44;
constexpr int PropSub0 = 0x70, PropSub1 = 0x78;  // Struct/PropertyClass/Inner/Key | MetaClass/Inner(Array)/Value
constexpr int MapLayout = 0x80;                  // FScriptMapLayout inside FMapProperty
// GObjects (FUObjectArray::ObjObjects)
constexpr int ArrObjects = 0x00, ArrNumElements = 0x14, ArrNumChunks = 0x1C;
constexpr int ItemSize = 0x18, ItemObject = 0x08, PerChunk = 0x10000;
}  // namespace L

// EObjectFlags / EPropertyFlags we use
constexpr uint32_t RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20;
constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;

using UObject = GUObject;
using UClass = GUClass;
using UStruct = GUStruct;
using UFunction = GUFunction;
using FProperty = GFProperty;

template <class T = void*> inline T& At(const void* base, int off) {
    return *(T*)((uint8_t*)base + off);
}

// ---- setup
bool   ResolveOffsets();   // verifies/locates ProcessEvent + AppendString; called at entry
bool   ValidateGObjects(); // called once objects exist
extern void* g_processEvent;       // UObject::ProcessEvent (hookable address)
extern void (*g_peRaw)(UObject*, UFunction*, void*);  // original (trampoline once hooked)

// ---- objects
int         ObjectCount();
UObject*    ObjectAt(int i);
bool        IsValid(UObject* o);
inline UClass*  ClassOf(UObject* o) { return At<UClass*>(o, L::ObjClass); }
inline UObject* OuterOf(UObject* o) { return At<UObject*>(o, L::ObjOuter); }
inline uint32_t FlagsOf(UObject* o) { return At<uint32_t>(o, L::ObjFlags); }
inline UStruct* SuperOf(UStruct* s) { return At<UStruct*>(s, L::StructSuper); }
bool        IsA(UObject* o, UClass* c);
bool        IsChildOf(UStruct* s, UStruct* base);

std::string NameStr(uint64_t fname);
std::string ObjName(UObject* o);
std::string PathName(UObject* o);
std::string FullName(UObject* o);

UObject*    FindObject(const std::string& path);
UClass*     FindClass(const std::string& nameOrPath);
UObject*    FindFirstOf(UClass* c);
int         FindAllOf(UClass* c, std::vector<UObject*>& out);
UObject*    DefaultObject(UClass* c);

// ---- reflection
UFunction*  FindFunction(UStruct* s, const std::string& name);
FProperty*  FindProperty(UStruct* s, const std::string& name);
FProperty*  NextProperty(UStruct* s, FProperty* prev);
std::string PropName(FProperty* p);
std::string PropType(FProperty* p);
bool        PropInfo(FProperty* p, GML_PropInfo* out);
inline int  PropOffset(FProperty* p) { return At<int32_t>(p, L::PropOffset); }
inline int  PropSize(FProperty* p) { return At<int32_t>(p, L::PropElemSize); }
inline uint64_t PropFlags(FProperty* p) { return At<uint64_t>(p, L::PropFlags); }
inline int  StructSize(UStruct* s) { return At<int32_t>(s, L::StructSize); }

// ---- calling
// Calls ProcessEvent. `hooked` = go through GML's hook dispatch (as the game would).
void ProcessEvent(UObject* o, UFunction* f, void* params, bool hooked = true);

// An FString that points at our own buffer; valid as an *input* parameter.
struct FStr {
    std::wstring s;
    GML_FString raw{};
    explicit FStr(std::wstring w) : s(std::move(w)) { raw = {s.data(), (int32_t)s.size() + 1, (int32_t)s.size() + 1}; }
};

// A zeroed parameter block for one UFunction, addressable by parameter name or position.
struct Params {
    UObject*   obj = nullptr;
    UFunction* fn = nullptr;
    std::vector<uint8_t> buf;
    Params(UObject* target, const char* func);
    explicit operator bool() const { return fn != nullptr; }
    void* Ptr(const char* name);  // nullptr if absent
    void* Arg(int index);         // i-th non-return parameter
    void* Ret();
    int   ArgSize(int index);
    int   RetSize();
    template <class T> void Set(int index, const T& v) { if (void* p = Arg(index)) memcpy(p, &v, sizeof v); }
    void  SetStr(int index, const FStr& s) { Set(index, s.raw); }
    template <class T> T RetAs() { T v{}; if (void* p = Ret()) memcpy(&v, p, sizeof v); return v; }
    bool  Call(bool hooked = false);
};

// Library CDOs: KismetSystemLibrary, GameplayStatics, ...
UObject* Lib(const char* className);

// ---- engine helpers (game thread)
uint64_t MakeName(const std::string& s);
bool     MakeText(const std::string& s, void* out16);
std::string TextToString(const void* ftext);
std::string FStringToUtf8(const GML_FString* s);
UObject* LoadObject(const std::string& path, bool asClass);
UObject* NewObject(UClass* c, UObject* outer);
void*    EngineAlloc(size_t bytes);
void*    ArrayAdd(GML_TArray* arr, int elemSize, int count);
int      MapForEach(void* map, FProperty* mapProp, int (*cb)(void*, void*, void*), void* user);
UObject* WorldContext();
void     KeepAlive(UObject* o);

}  // namespace gml::ue
