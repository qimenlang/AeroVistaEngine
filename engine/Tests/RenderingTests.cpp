#include <catch2/catch_test_macros.hpp>

#include "ImageCompare.h"
#include "engine.h"

// =============================================================================
// 验收：Engine 加载 / 渲染对外行为
// =============================================================================

// =============================================================================
// 有效茶壶模型 init 成功，并且能渲染一帧（引擎加载与渲染.md §2，码 RND-load-frame）。
// 离屏。不比对像素。
// =============================================================================
SCENARIO("Engine loads a valid model and renders one frame", "[acceptance][render][load][RND-load-frame]")
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

// =============================================================================
// 模型路径不存在时，离屏引擎 init 失败（引擎加载与渲染.md §2，码 RND-load-missing）。
// =============================================================================
SCENARIO("Engine fails to initialize when the model path does not exist",
         "[acceptance][render][load][failure][RND-load-missing]")
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

// =============================================================================
// 默认识图渲染一帧后，截图与黄金图的差异在容差内（引擎加载与渲染.md §2，码 RND-golden-teapot）。
// 尺寸必须一致。差异比例、最大绝对差和 PSNR 用 CHECK：小于 0.1、小于 32、大于 40。
// =============================================================================
SCENARIO("default teapot view matches the golden reference image",
         "[acceptance][render][golden][RND-golden-teapot]")
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
