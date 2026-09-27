// GMLTestMissingDep - test plugin with a hard dependency that does not exist. The chainloader
// must skip it without ever running its code. Used by GMLSelfTest.
#include <GML/GML.hpp>
#include <windows.h>

using namespace gml;

GML_PLUGIN("com.remarrow.gmltestmissingdep", "GML TestMissingDep", "1.0.0",
           {{"com.nobody.doesnotexist", "1.0.0", GML_DEPENDENCY_HARD}});

// Runs when the DLL is loaded - proves the chainloader never loaded it.
static int s_loaded = [] {
    OutputDebugStringA("GMLTestMissingDep: DLL WAS LOADED - chainloader should have skipped it\n");
    return 1;
}();

GML_AWAKE() {
    Error("FAIL this plugin has a missing hard dependency and should never have been loaded");
    return 0;
}
