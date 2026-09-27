// GMLTestPatcher - test patcher (goes in GML\patchers\). Checks that patchers run at process
// entry, before the engine, and can subscribe to engine events. Used by GMLSelfTest.
#include <GML/GML.hpp>

using namespace gml;

GML_PLUGIN("com.remarrow.gmltestpatcher", "GML TestPatcher", "1.0.0");

GML_PATCH() {
    Log("{} GML_Patch at process entry: UEReady={} (expected 0)", API->UEReady() == 0 ? "PASS" : "FAIL",
        API->UEReady());
    On(GML_EVENT_ENGINE_READY, [](void*) { Log("PASS patcher received ENGINE_READY"); });
    return 0;
}
