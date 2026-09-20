#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Common.h"
#include "engine.h"
#include "function/scene/GroundGrid.h"

#include <cmath>
#include <string>

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

namespace
{
    const vsg::dvec3 kGridLla{39.9087, 116.3975, 0.0};

    const char* kMinimalWindow = R"("window": { "x": 0, "y": 0, "width": 640, "height": 480 })";
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

TEST_CASE("groundGrid line count is cells-across plus one",
          "[unit][sync][meas][MEAS-seam-grid]")
{
    REQUIRE(groundGridLineCount(20000.0, 200.0) == 201);
    REQUIRE(groundGridLineCount(1000.0, 100.0) == 21);
}

TEST_CASE("loadEngineChannelConfig parses groundGrid with defaults",
          "[unit][sync][meas][MEAS-seam-grid]")
{
    const TempConfigFile file(std::string(R"({ "groundGrid": { )") + kGridLlaJson + " }, " + kMinimalModel + ", " +
                              kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE(cfg.groundGrid);
    REQUIRE(cfg.groundGrid->lla.x == Catch::Approx(39.9087));
    REQUIRE(cfg.groundGrid->lla.y == Catch::Approx(116.3975));
    REQUIRE(cfg.groundGrid->lla.z == Catch::Approx(0.0));
    REQUIRE(cfg.groundGrid->halfExtentM == Catch::Approx(20000.0));
    REQUIRE(cfg.groundGrid->cellM == Catch::Approx(200.0));
    REQUIRE(cfg.groundGrid->majorEvery == 5);
}

TEST_CASE("loadEngineChannelConfig rejects groundGrid without lla",
          "[unit][sync][meas][MEAS-seam-grid]")
{
    const TempConfigFile file(std::string(R"({ "groundGrid": { "cellM": 100 }, )") + kMinimalModel + ", " +
                              kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_THAT(error, Catch::Matchers::ContainsSubstring("lla"));
}

TEST_CASE("loadEngineChannelConfig rejects non-positive groundGrid.cellM",
          "[unit][sync][meas][MEAS-seam-grid]")
{
    const TempConfigFile file(std::string(R"({ "groundGrid": { )") + kGridLlaJson + R"(, "cellM": 0 }, )" +
                              kMinimalModel + ", " + kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_THAT(error, Catch::Matchers::ContainsSubstring("cellM"));
}

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

SCENARIO("configured groundGrid is pinned to ECEF at the origin LLA",
         "[unit][sync][meas][MEAS-seam-grid]")
{
    GIVEN("an ellipsoid scene with groundGrid at Tiananmen")
    {
        const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, "groundGrid": { )") +
                                  kGridLlaJson + R"(, "halfExtentM": 1000, "cellM": 100 }, )" + kMinimalModel + ", " +
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

SCENARIO("scene_seam_grid.json loads a pinned ground grid",
         "[system][bdd][config][meas][resource][MEAS-seam-grid]")
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
