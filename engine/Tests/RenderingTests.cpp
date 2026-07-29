#include <catch2/catch_test_macros.hpp>

#include "ImageCompare.h"
#include "engine.h"

// =============================================================================
// 验收：Engine 加载 / 渲染对外行为
// =============================================================================

SCENARIO("Engine loads a valid model and renders one frame", "[acceptance][bdd][render][load]")
{
    GIVEN("an offscreen Engine and a valid teapot model")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";

        WHEN("the model is loaded and one frame is rendered")
        {
            const bool loaded = engine.init(modelPath);
            const bool rendered = loaded && engine.tickOnFrame();

            THEN("initialization and the frame both succeed")
            {
                REQUIRE(loaded);
                REQUIRE(rendered);
            }
        }
    }
}

SCENARIO("Engine fails to initialize when the model path does not exist",
         "[acceptance][bdd][render][load][failure]")
{
    GIVEN("an offscreen Engine and a model path that does not exist")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "missing.vsgt";

        WHEN("initialization is attempted with that path")
        {
            const bool loaded = engine.init(modelPath);

            THEN("initialization fails")
            {
                REQUIRE_FALSE(loaded);
            }
        }
    }
}

SCENARIO("default teapot view matches the golden reference image",
         "[acceptance][bdd][render][golden]")
{
    GIVEN("an offscreen Engine, teapot model, and golden reference image")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        const vsg::Path goldenPath = vsg::Path(RESOURCE_DIR) / "renderingTests" / "teapot.png";
        const vsg::Path actualPath = vsg::Path(RESOURCE_DIR) / "renderingTests" / "teapot_actual.png";

        WHEN("one frame is rendered and captured")
        {
            REQUIRE(engine.init(modelPath));
            REQUIRE(engine.tickOnFrame());
            REQUIRE(engine.captureToFile(actualPath));

            const auto result = compareImages(goldenPath, actualPath);

            THEN("the capture matches the golden image within tolerance")
            {
                INFO("golden = " << goldenPath);
                INFO("actual = " << actualPath);
                INFO("diff   = " << result.diffImagePath);
                CAPTURE(result.differingPercent, result.maxAbsDiff, result.psnrDb, result.width, result.height);

                REQUIRE(result.dimensionMatch);
                CHECK(result.differingPercent < 0.1);
                CHECK(result.maxAbsDiff < 32);
                CHECK(result.psnrDb > 40.0);
            }
        }
    }
}
