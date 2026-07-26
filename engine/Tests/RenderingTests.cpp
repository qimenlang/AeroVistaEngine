#include <catch2/catch_test_macros.hpp>

#include "ImageCompare.h"
#include "engine.h"

SCENARIO("Engine loads a model and renders one frame", "[bdd][engine][load]")
{
    GIVEN("an offscreen engine and a valid teapot model path")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";

        WHEN("the model is loaded")
        {
            const bool loaded = engine.init(modelPath);

            THEN("initialization succeeds")
            {
                REQUIRE(loaded);

                AND_WHEN("one frame is rendered")
                {
                    const bool rendered = engine.tickOnFrame();

                    THEN("the frame completes successfully")
                    {
                        REQUIRE(rendered);
                    }
                }
            }
        }
    }
}

SCENARIO("Engine fails to load a missing model", "[bdd][engine][load][failure]")
{
    GIVEN("an offscreen engine and a model path that does not exist")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "missing.vsgt";

        WHEN("the missing model is loaded")
        {
            const bool loaded = engine.init(modelPath);

            THEN("initialization fails")
            {
                REQUIRE_FALSE(loaded);
            }
        }
    }
}

SCENARIO("teapot default camera matches the golden image", "[bdd][render][golden]")
{
    GIVEN("an offscreen engine, teapot model, and golden reference image")
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
            REQUIRE(engine.CaptureToFile(actualPath));

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
