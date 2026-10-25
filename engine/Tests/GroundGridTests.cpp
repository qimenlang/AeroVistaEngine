#include <catch2/catch_test_macros.hpp>

#include "Testutil.h"
#include "engine.h"
#include "function/scene/GroundGrid.h"

#include <string>

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

namespace
{
    const vsg::dvec3 kGridLla{39.9087, 116.3975, 0.0};

    const char* kMinimalWindow =
        R"("viewDefinition": { "vfov": 30.0, "near": 1.0, "far": 1000.0, "window": { "posX": 0, "posY": 0, "width": 640, "height": 480 } })";
    const char* kMinimalModel = R"("model": "models/lz.vsgt")";
    const char* kGridLlaJson = R"("lla": { "lat": 39.9087, "lon": 116.3975, "alt": 0.0 })";

    vsg::ref_ptr<vsg::MatrixTransform> findPinnedGrid(vsg::ref_ptr<vsg::Node> node)
    {
        if (!node)
            return {};
        if (auto transform = node.cast<vsg::MatrixTransform>())
        {
            bool marked = false;
            if (transform->getValue(kGroundGridObjectKey, marked) && marked)
                return transform;
        }
        if (auto group = node.cast<vsg::Group>())
        {
            for (auto& child : group->children)
            {
                if (auto found = findPinnedGrid(child))
                    return found;
            }
        }
        if (auto sw = node.cast<vsg::Switch>())
        {
            for (auto& child : sw->children)
            {
                if (auto found = findPinnedGrid(child.node))
                    return found;
            }
        }
        return {};
    }

    vsg::dvec3 translation(const vsg::dmat4& m)
    {
        return {m(3, 0), m(3, 1), m(3, 2)};
    }
} // namespace

// =============================================================================
// 网格线数等于跨度除以格距再加一（多通道同步验收测量设计.md §7，码 MEAS-seam-grid）。
// 纯函数，不创建场景。
// =============================================================================
TEST_CASE("groundGrid line count is cells-across plus one",
          "[unit][sync][meas][MEAS-seam-grid]")
{
    REQUIRE(groundGridLineCount(20000.0, 200.0) == 201);
    REQUIRE(groundGridLineCount(1000.0, 100.0) == 21);
}

// =============================================================================
// 配置了地面网格但场景没有椭球时，init 失败（多通道同步验收测量设计.md §7，码 MEAS-seam-grid）。
// 不验网格几何。
// =============================================================================
TEST_CASE("groundGrid without EllipsoidModel fails init",
          "[unit][sync][meas][MEAS-seam-grid]")
{
    const TempConfigFile file(std::string(R"({ "groundGrid": { )") + kGridLlaJson + " }, " + kMinimalModel + ", " +
                              kMinimalWindow + "}");
    Engine engine;
    REQUIRE(engine.loadConfig(file.path()));
    engine.showWindow = false;
    REQUIRE_FALSE(engine.init());
}

// =============================================================================
// 配了 groundGrid 的椭球场景，网格父节点钉在配置 LLA 的 ECEF
// （验收测量设计.md §4.1，码 MEAS-seam-grid）。
// halfGroundLenM / gridLenM 缩小只为少建线，不改变钉点。
// =============================================================================
SCENARIO("configured groundGrid is pinned to ECEF at the origin LLA",
         "[unit][sync][meas][MEAS-seam-grid]")
{
    GIVEN("an ellipsoid scene with groundGrid at Tiananmen")
    {
        const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, "groundGrid": { )") +
                                  kGridLlaJson + R"(, "halfGroundLenM": 1000, "gridLenM": 100 }, )" + kMinimalModel + ", " +
                                  kMinimalWindow + "}");
        Engine engine;
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;
        REQUIRE(engine.init());

        WHEN("the scene graph is searched for the marked grid transform")
        {
            auto pinned = findPinnedGrid(engine.mainScene());

            THEN("the parent origin matches LocalToWorld of the configured LLA")
            {
                REQUIRE(pinned);
                REQUIRE(engine.ellipsoidModel());
                const vsg::dvec3 expected = engine.ellipsoidModel()->convertLatLongAltitudeToECEF(kGridLla);
                const vsg::dvec3 origin = translation(pinned->matrix);
                REQUIRE(vsg::length(origin - expected) < 1.0e-3);
            }
        }
    }
}

// =============================================================================
// 随仓接缝网格场景 init 之后，场景里有椭球和钉住的地面网格（多通道同步验收测量设计.md §7，码 MEAS-seam-grid）。
// 钉点公式在同码的另一条。这里只确认这份场景能起来，并且网格节点在场景图里。
// =============================================================================
SCENARIO("scene_seam_grid.json loads a pinned ground grid",
         "[system][config][meas][resource][MEAS-seam-grid]")
{
    GIVEN("the in-tree seam-grid channel config")
    {
        Engine engine;
        engine.showWindow = false;
        const std::string path = std::string(RESOURCE_DIR) + "/config/scene_seam_grid.json";
        REQUIRE(engine.loadConfig(path));
        REQUIRE(engine.config.groundGrid);
        REQUIRE(engine.init());

        WHEN("the scene is inspected")
        {
            THEN("EllipsoidModel and a pinned groundGrid are present")
            {
                REQUIRE(engine.ellipsoidModel());
                REQUIRE(findPinnedGrid(engine.mainScene()));
            }
        }
    }
}
