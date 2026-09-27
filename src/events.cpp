// ProcessEvent detour: the loader's heartbeat.
//
// Every Blueprint event, BlueprintImplementableEvent, delegate broadcast and timer that
// reaches script goes through UObject::ProcessEvent on the game thread. The detour uses
// that to:
//   - on the first call (UObject system is up by then): run the chainloader, fire ENGINE_READY,
//   - drain RunOnGameThread tasks and drive the approximate per-frame TICK,
//   - dispatch UFunction hooks (pre/post, by pointer or by name spec),
//   - fire GAME_INSTANCE_INIT / WORLD_BEGIN_PLAY.
#include "ue.h"
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <deque>

namespace gml {

using namespace ue;

struct Sub { GML_Plugin* mod; GML_EventFn fn; void* user; };
struct Task { GML_Plugin* mod; GML_TaskFn fn; void* user; };
struct Hook {
    int id;
    GML_Plugin* mod;
    GML_HookFn pre, post;
    void* user;
    // exactly one of these identifies the target
    UFunction* fn;
    std::string cls;   // "" when fn is set; "*" for any class; or a full path when funcName is ""
    std::string func;
};

static std::vector<Sub> s_subs[GML_EVENT_COUNT];
static std::mutex s_subMtx;

static std::mutex s_taskMtx;
static std::deque<Task> s_tasks;
static std::atomic<bool> s_hasTasks{false};

static std::shared_mutex s_hookMtx;
static std::vector<Hook> s_hooks;
static int s_nextHookId = 1;
// UFunction* -> indices into s_hooks (empty vector = known to have no hooks)
static std::unordered_map<UFunction*, std::vector<int>> s_resolved;
static std::atomic<bool> s_anyHooks{false};

static std::atomic<bool> s_ready{false};
static bool s_ueOk = false;
static LARGE_INTEGER s_qpf, s_lastTick;

static std::unordered_set<UFunction*> s_traced;
static UClass* s_clsGameInstance = nullptr;
static UClass* s_clsGameModeBase = nullptr;

bool UEReady() { return s_ready.load(); }

static void Fire(GML_Event ev, void* data) {
    std::vector<Sub> subs;
    {
        std::lock_guard g(s_subMtx);
        subs = s_subs[ev];
    }
    static const char* names[] = {"ENGINE_READY", "GAME_INSTANCE_INIT", "WORLD_BEGIN_PLAY", "TICK"};
    for (auto& s : subs) {
        if (s.mod->faulted) continue;
        GuardedCall(s.mod, names[ev], [&] { s.fn(s.user, data); });
    }
}

void Subscribe(GML_Plugin* mod, GML_Event ev, GML_EventFn fn, void* user) {
    if (ev < 0 || ev >= GML_EVENT_COUNT || !fn) return;
    std::lock_guard g(s_subMtx);
    s_subs[ev].push_back({mod ? mod : LoaderSelf(), fn, user});
}

void RunOnGameThread(GML_Plugin* mod, GML_TaskFn fn, void* user) {
    if (!fn) return;
    std::lock_guard g(s_taskMtx);
    s_tasks.push_back({mod, fn, user});
    s_hasTasks = true;
}

static void DrainTasks() {
    std::deque<Task> run;
    {
        std::lock_guard g(s_taskMtx);
        run.swap(s_tasks);
        s_hasTasks = false;
    }
    for (auto& t : run)
        if (!t.mod || !t.mod->faulted) GuardedCall(t.mod, "game-thread task", [&] { t.fn(t.user); });
}

// ------------------------------------------------------------------ hooks

static bool SpecMatches(const Hook& h, UFunction* fn) {
    if (h.fn) return h.fn == fn;
    if (h.func.empty()) return PathName((UObject*)fn) == h.cls;  // full path spec
    if (ObjName((UObject*)fn) != h.func) return false;
    if (h.cls == "*") return true;
    UObject* owner = OuterOf((UObject*)fn);
    return owner && ObjName(owner) == h.cls;
}

static void Invalidate() {
    s_resolved.clear();
    s_anyHooks = !s_hooks.empty();
}

int HookFunctionSpec(GML_Plugin* mod, const char* spec, GML_HookFn pre, GML_HookFn post, void* user) {
    if (!spec || (!pre && !post)) return 0;
    std::string s = spec;
    Hook h{0, mod, pre, post, user, nullptr, {}, {}};
    size_t colon = s.rfind(':');
    if (s[0] == '/') {
        h.cls = s;  // full path, matched against the UFunction's path name
    } else if (colon != std::string::npos) {
        h.cls = s.substr(0, colon);
        h.func = s.substr(colon + 1);
    } else {
        h.cls = "*";
        h.func = s;
    }
    std::unique_lock g(s_hookMtx);
    h.id = s_nextHookId++;
    s_hooks.push_back(h);
    Invalidate();
    LOGI("hook #%d on %s (by %s)", h.id, spec, mod ? mod->name.c_str() : "GML");
    return h.id;
}

int HookUFunction(GML_Plugin* mod, GUFunction* fn, GML_HookFn pre, GML_HookFn post, void* user) {
    if (!fn || (!pre && !post)) return 0;
    std::unique_lock g(s_hookMtx);
    Hook h{s_nextHookId++, mod, pre, post, user, fn, {}, {}};
    s_hooks.push_back(h);
    Invalidate();
    return h.id;
}

void Unhook(int id) {
    std::unique_lock g(s_hookMtx);
    for (size_t i = 0; i < s_hooks.size(); i++)
        if (s_hooks[i].id == id) { s_hooks.erase(s_hooks.begin() + i); break; }
    Invalidate();
}

// Returns the hooks for fn, resolving (and caching) name specs on first sight.
static std::vector<Hook> HooksFor(UFunction* fn) {
    {
        std::shared_lock g(s_hookMtx);
        auto it = s_resolved.find(fn);
        if (it != s_resolved.end()) {
            std::vector<Hook> r;
            for (int i : it->second) r.push_back(s_hooks[i]);
            return r;
        }
    }
    std::unique_lock g(s_hookMtx);
    std::vector<int> idx;
    for (int i = 0; i < (int)s_hooks.size(); i++)
        if (SpecMatches(s_hooks[i], fn)) idx.push_back(i);
    std::vector<Hook> r;
    for (int i : idx) r.push_back(s_hooks[i]);
    s_resolved[fn] = std::move(idx);
    return r;
}

// ------------------------------------------------------------------ detour

static thread_local int t_depth = 0;

// TICK is gated on the engine's frame counter (KismetSystemLibrary::GetFrameCount = GFrameCounter),
// polled at most once per millisecond, so it fires exactly once per frame that runs any script.
static UObject* s_ksl = nullptr;
static UFunction* s_fnFrameCount = nullptr;
static int s_frameRetOffset = 0;
static int64_t s_lastFrame = -1;
static LARGE_INTEGER s_lastPoll;

static void OnFirstGameThreadCall() {
    s_ueOk = ValidateGObjects();
    if (!s_ueOk) return;
    s_clsGameInstance = FindClass("GameInstance");
    s_clsGameModeBase = FindClass("GameModeBase");
    s_ksl = Lib("KismetSystemLibrary");
    s_fnFrameCount = s_ksl ? FindFunction((UStruct*)ClassOf(s_ksl), "GetFrameCount") : nullptr;
    if (FProperty* ret = s_fnFrameCount ? FindProperty((UStruct*)s_fnFrameCount, "ReturnValue") : nullptr)
        s_frameRetOffset = PropOffset(ret);
    else
        s_fnFrameCount = nullptr;
    if (!s_fnFrameCount) LOGW("GetFrameCount not found - TICK falls back to TickIntervalMs timing");
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_lastTick);
    s_lastPoll = s_lastTick;
    s_ready = true;
    // Like BepInEx's chainloader starting once the engine is up: plugins Awake here, with the
    // UObject system live, then ENGINE_READY fires (so subscriptions made in Awake receive it).
    RunChainloader();
    LOGI("engine ready - firing ENGINE_READY");
    Fire(GML_EVENT_ENGINE_READY, nullptr);
}

static bool NewFrame(const LARGE_INTEGER& now) {
    if (!s_fnFrameCount) {  // fallback: time-based
        return double(now.QuadPart - s_lastTick.QuadPart) * 1000.0 / double(s_qpf.QuadPart) >= g_cfg.tickIntervalMs;
    }
    if ((now.QuadPart - s_lastPoll.QuadPart) * 1000 < s_qpf.QuadPart) return false;  // < 1 ms since last poll
    s_lastPoll = now;
    alignas(16) uint8_t params[32] = {};
    g_peRaw(s_ksl, s_fnFrameCount, params);
    int64_t frame = *(int64_t*)(params + s_frameRetOffset);
    if (frame == s_lastFrame) return false;
    bool first = s_lastFrame < 0;
    s_lastFrame = frame;
    return !first;
}

static void Heartbeat() {
    static bool first = true;
    if (first) {
        first = false;
        OnFirstGameThreadCall();
    }
    if (!s_ueOk) return;
    if (s_hasTasks) DrainTasks();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (NewFrame(now)) {
        float dt = (float)(double(now.QuadPart - s_lastTick.QuadPart) / double(s_qpf.QuadPart));
        s_lastTick = now;
        Fire(GML_EVENT_TICK, &dt);
    }
}

static void Trace(UFunction* fn, UObject* self) {
    if (!s_traced.insert(fn).second) return;
    LOGI("[trace] %s  (self: %s)", PathName((UObject*)fn).c_str(), ObjName((UObject*)ClassOf(self)).c_str());
}

static void BuiltinEvents(UObject* self, UFunction* fn) {
    // Each Blueprint override is its own UFunction, so classify by name, cached per pointer.
    // Game thread only - no locking needed.
    static std::unordered_map<UFunction*, uint8_t> kind;  // 0 other, 1 ReceiveInit, 2 ReceiveBeginPlay
    auto it = kind.find(fn);
    if (it == kind.end()) {
        std::string n = ObjName((UObject*)fn);
        it = kind.emplace(fn, n == "ReceiveInit" ? 1 : n == "ReceiveBeginPlay" ? 2 : 0).first;
    }
    if (it->second == 1 && IsA(self, s_clsGameInstance)) {
        LOGI("GAME_INSTANCE_INIT (%s)", FullName(self).c_str());
        Fire(GML_EVENT_GAME_INSTANCE_INIT, self);
    } else if (it->second == 2 && IsA(self, s_clsGameModeBase)) {
        LOGI("WORLD_BEGIN_PLAY (%s)", FullName(self).c_str());
        Fire(GML_EVENT_WORLD_BEGIN_PLAY, self);
    }
}

static void __fastcall PEDetour(UObject* self, UFunction* fn, void* params) {
    const bool gameThread = GetCurrentThreadId() == g_gameThreadId;
    if (!gameThread || t_depth > 64) {
        g_peRaw(self, fn, params);
        return;
    }
    ++t_depth;
    if (t_depth == 1) Heartbeat();

    if (!s_ueOk) {
        g_peRaw(self, fn, params);
        --t_depth;
        return;
    }
    if (g_cfg.traceProcessEvent) Trace(fn, self);

    bool skip = false;
    std::vector<Hook> hooks;
    if (s_anyHooks) hooks = HooksFor(fn);
    for (auto& h : hooks) {
        if (!h.pre || (h.mod && h.mod->faulted)) continue;
        int r = 0;
        GuardedCall(h.mod, "pre-hook", [&] { r = h.pre(h.user, self, fn, params); });
        if (r) skip = true;
    }
    if (!skip) g_peRaw(self, fn, params);
    for (auto& h : hooks) {
        if (!h.post || (h.mod && h.mod->faulted)) continue;
        GuardedCall(h.mod, "post-hook", [&] { h.post(h.user, self, fn, params); });
    }
    BuiltinEvents(self, fn);
    --t_depth;
}

bool EventsInit() {
    if (!ResolveOffsets()) return false;
    void* orig = nullptr;
    std::string err;
    if (!HookInstall(g_processEvent, (void*)&PEDetour, &orig, &err)) {
        LOGE("could not hook ProcessEvent: %s", err.c_str());
        return false;
    }
    g_peRaw = (decltype(g_peRaw))orig;
    LOGI("ProcessEvent hooked");
    return true;
}

}  // namespace gml
