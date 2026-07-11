#include <catch2/catch_test_macros.hpp>

#include "RenderingEngine.h"

TEST_CASE("offline rendering", "rendering tests")
{
    RenderingEngine engine;
    engine.extent = {1920, 1080};

    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
    const vsg::Path outputPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.png";

    REQUIRE(engine.init(modelPath));
    REQUIRE(engine.run(outputPath));
}
