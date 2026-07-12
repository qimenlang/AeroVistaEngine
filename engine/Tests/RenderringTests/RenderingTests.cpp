#include <catch2/catch_test_macros.hpp>

#include "ImageCompare.h"
#include "RenderingEngine.h"

TEST_CASE("teapot renders identically", "[render][basic]")
{
    RenderingEngine engine;
    engine.extent = {1920, 1080};

    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
    const vsg::Path goldenPath = vsg::Path(RESOURCE_DIR) / "renderingTests" / "teapot.png";
    const vsg::Path actualPath = vsg::Path(RESOURCE_DIR) / "renderingTests" / "teapot_actual.png";

    REQUIRE(engine.init(modelPath));
    REQUIRE(engine.run(actualPath));

    const auto result = compareImages(goldenPath, actualPath);

    INFO("golden = " << goldenPath);
    INFO("actual = " << actualPath);
    INFO("diff   = " << result.diffImagePath);
    CAPTURE(result.differingPercent, result.maxAbsDiff, result.psnrDb, result.width, result.height);

    REQUIRE(result.dimensionMatch);
    CHECK(result.differingPercent < 0.1);
    CHECK(result.maxAbsDiff < 32);
    CHECK(result.psnrDb > 40.0);
}
