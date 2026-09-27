// HelloGML - a minimal GML plugin: metadata, config, events, a UFunction hook, reflection.
//
// Build: build.bat (lands in build\examples\HelloGML\). Install: copy HelloGML.dll into
// <game>\Geronimo\Binaries\Win64\GML\plugins\HelloGML\ and launch. Output: GML\LogOutput.log,
// settings: GML\config\com.example.hellogml.cfg.
#include <GML/GML.hpp>

using namespace gml;

GML_PLUGIN("com.example.hellogml", "Hello GML", "1.0.0");

static ConfigEntry<bool> s_logLevels;
static int s_beginPlays = 0;

GML_AWAKE() {
    // Settings, written to GML\config\com.example.hellogml.cfg with these descriptions.
    s_logLevels = Config.Bind("General", "LogLevelStarts", true, "Log every level start.");
    auto greeting = Config.Bind("General", "Greeting", "Hello from a native plugin!", "Logged once at startup.");
    Log("{}", greeting.Value());

    // Awake runs once the engine is up, so reflection works right away.
    Object gi = FindFirstOf("GameInstance");
    Log("game instance class: {}", gi ? gi.FullName() : std::string("(not created yet)"));

    // Engine events.
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void* gameMode) {
        if (s_logLevels.Value()) Log("level started: {}", Object((GUObject*)gameMode).FullName());
        Log("{} actors have run BeginPlay so far", s_beginPlays);
    });

    // A UFunction hook on every class: count BeginPlay calls.
    HookAfter("*:ReceiveBeginPlay", [](Object self, GUFunction*, void*) { s_beginPlays++; });
    return 0;
}
