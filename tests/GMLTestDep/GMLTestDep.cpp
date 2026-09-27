// GMLTestDep - trivial test plugin that GMLSelfTest soft-depends on. Its GUID sorts after the
// self-test's, so the self-test can only load second if dependency ordering works.
#include <GML/GML.hpp>

using namespace gml;

GML_PLUGIN("com.remarrow.zz.gmltestdep", "GML TestDep", "1.0.0");

GML_AWAKE() {
    Log("loaded");
    return 0;
}
