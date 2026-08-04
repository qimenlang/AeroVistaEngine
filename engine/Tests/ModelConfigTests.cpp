#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

// 位姿配置设计.md — entity/camera 双轨摆模与初始相机；含 shipped resource 系统测。

namespace
{
    class TempConfigFile
    {
    public:
        explicit TempConfigFile(const std::string& jsonBody)
        {
            _path = (std::filesystem::temp_directory_path() /
                     ("ave_model_cfg_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".json"))
                        .string();
            std::ofstream out(_path, std::ios::binary);
            REQUIRE(out);
            out << jsonBody;
        }

        ~TempConfigFile()
        {
            std::error_code ec;
            std::filesystem::remove(_path, ec);
        }

        const std::string& path() const { return _path; }

    private:
        std::string _path;
    };

    const char* kWindow = R"("window": { "x": 0, "y": 0, "width": 640, "height": 480 })";
    const char* kTeapot = "models/teapot.vsgt";
    const char* kLz = "models/lz.vsgt";

    bool nearlyEqual(double a, double b, double eps = 1e-6)
    {
        return std::abs(a - b) <= eps;
    }

    void requireDVec3Near(const vsg::dvec3& actual, double x, double y, double z, double eps = 1e-6)
    {
        REQUIRE(nearlyEqual(actual.x, x, eps));
        REQUIRE(nearlyEqual(actual.y, y, eps));
        REQUIRE(nearlyEqual(actual.z, z, eps));
    }

    void requireDVec3Near(const vsg::dvec3& actual, const vsg::dvec3& expected, double eps = 1e-6)
    {
        requireDVec3Near(actual, expected.x, expected.y, expected.z, eps);
    }

    std::string jsonVec3(const vsg::dvec3& v)
    {
        return "[" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + "]";
    }

    std::string jsonLocalPose(const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg)
    {
        return R"({ "position": )" + jsonVec3(position) + R"(, "eulerYprDeg": )" + jsonVec3(eulerYprDeg) + " }";
    }

    std::string jsonEllipsoidPose(const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg)
    {
        return R"({ "lla": { "lat": )" + std::to_string(lla.x) + R"(, "lon": )" + std::to_string(lla.y) +
               R"(, "alt": )" + std::to_string(lla.z) + R"( }, "eulerYprDeg": )" + jsonVec3(eulerYprDeg) + " }";
    }

    std::string jsonEntity(int id, const std::string& model, const std::string& poseObject = {},
                           const std::string& name = {})
    {
        std::string s = R"({ "id": )" + std::to_string(id) + R"(, "model": ")" + model + "\"";
        if (!name.empty())
            s += R"(, "name": ")" + name + "\"";
        if (!poseObject.empty())
            s += R"(, "pose": )" + poseObject;
        s += " }";
        return s;
    }

    std::string jsonPoseLocalOnly(const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg)
    {
        return R"({ "local": )" + jsonLocalPose(position, eulerYprDeg) + " }";
    }

    std::string jsonPoseEllipsoidOnly(const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg)
    {
        return R"({ "ellipsoid": )" + jsonEllipsoidPose(lla, eulerYprDeg) + " }";
    }

    std::string jsonPoseBoth(const vsg::dvec3& localPos, const vsg::dvec3& localYpr, const vsg::dvec3& lla,
                             const vsg::dvec3& ellYpr)
    {
        return R"({ "local": )" + jsonLocalPose(localPos, localYpr) + R"(, "ellipsoid": )" +
               jsonEllipsoidPose(lla, ellYpr) + " }";
    }

    /// `entitiesArrayBody` is a JSON array literal, e.g. `[ {...}, {...} ]`.
    std::string channelJson(const char* coordFrame, const std::string& entitiesArrayBody,
                            const std::string& cameraObject = {})
    {
        std::string s = std::string("{ \"coordFrame\": \"") + coordFrame + "\", \"entities\": " + entitiesArrayBody;
        if (!cameraObject.empty())
            s += ", \"camera\": " + cameraObject;
        s += ", " + std::string(kWindow) + " }";
        return s;
    }

    void requireLoadFails(const std::string& jsonBody)
    {
        const TempConfigFile file(jsonBody);
        EngineChannelConfig cfg;
        std::string error;
        REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
        REQUIRE_FALSE(error.empty());
    }

    void initOffscreen(Engine& engine, const std::string& configPath)
    {
        engine.extent = {640, 480};
        engine.showWindow = false;
        REQUIRE(engine.loadConfig(configPath));
        REQUIRE(engine.init());
    }

    std::string resourceConfigPath(const char* fileName)
    {
        return std::string(RESOURCE_DIR) + "/config/" + fileName;
    }

    void requireLookAtMatchesLocalPose(Engine& engine, const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);
        const auto rotate = [&](const vsg::dvec3& v) {
            const vsg::dvec3 afterRoll =
                vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) * v;
            const vsg::dvec3 afterPitch =
                vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) * afterRoll;
            return vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) * afterPitch;
        };
        const vsg::dvec3 expectedForward = rotate(vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 expectedUp = rotate(vsg::dvec3(0.0, 0.0, 1.0));
        REQUIRE(vsg::length(lookAt->eye - position) < 1e-6);
        REQUIRE(vsg::length(vsg::normalize(lookAt->center - lookAt->eye) - vsg::normalize(expectedForward)) < 1e-6);
        REQUIRE(vsg::length(vsg::normalize(lookAt->up) - vsg::normalize(expectedUp)) < 1e-6);
    }

    vsg::dvec3 rotateEnuToEcef(const vsg::dmat4& localToWorld, const vsg::dvec3& enuDir)
    {
        const vsg::dvec3 east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
        const vsg::dvec3 north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
        const vsg::dvec3 upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
        return enuDir.x * east + enuDir.y * north + enuDir.z * upAxis;
    }

    void requireLookAtMatchesLlaPose(Engine& engine, const vsg::EllipsoidModel& ellipsoid, const vsg::dvec3& lla,
                                     const vsg::dvec3& eulerYprDeg)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);
        const auto rotate = [&](const vsg::dvec3& v) {
            const vsg::dvec3 afterRoll =
                vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) * v;
            const vsg::dvec3 afterPitch =
                vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) * afterRoll;
            return vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) * afterPitch;
        };
        const vsg::dmat4 localToWorld = ellipsoid.computeLocalToWorldTransform(lla);
        const vsg::dvec3 eye = ellipsoid.convertLatLongAltitudeToECEF(lla);
        const vsg::dvec3 forward = vsg::normalize(rotateEnuToEcef(localToWorld, rotate(vsg::dvec3(0.0, 1.0, 0.0))));
        const vsg::dvec3 up = vsg::normalize(rotateEnuToEcef(localToWorld, rotate(vsg::dvec3(0.0, 0.0, 1.0))));
        REQUIRE(vsg::length(lookAt->eye - eye) < 1e-2);
        REQUIRE(vsg::length(vsg::normalize(lookAt->center - lookAt->eye) - forward) < 1e-5);
        REQUIRE(vsg::length(vsg::normalize(lookAt->up) - up) < 1e-5);
    }

    vsg::dmat4 rotationMatrixYpr(const vsg::dvec3& eulerYprDeg)
    {
        const vsg::dquat qRoll(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dquat qPitch(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0));
        const vsg::dquat qYaw(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0));
        const auto rotate = [&](const vsg::dvec3& v) { return qYaw * (qPitch * (qRoll * v)); };
        const vsg::dvec3 x = rotate(vsg::dvec3(1.0, 0.0, 0.0));
        const vsg::dvec3 y = rotate(vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 z = rotate(vsg::dvec3(0.0, 0.0, 1.0));
        vsg::dmat4 m = vsg::dmat4(1.0);
        m(0, 0) = x.x;
        m(0, 1) = x.y;
        m(0, 2) = x.z;
        m(1, 0) = y.x;
        m(1, 1) = y.y;
        m(1, 2) = y.z;
        m(2, 0) = z.x;
        m(2, 1) = z.y;
        m(2, 2) = z.z;
        return m;
    }

    vsg::dmat4 expectedLocalEntityMatrix(const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg)
    {
        return vsg::translate(position) * rotationMatrixYpr(eulerYprDeg);
    }

    vsg::dmat4 expectedEllipsoidEntityMatrix(const vsg::EllipsoidModel& ellipsoid, const vsg::dvec3& lla,
                                             const vsg::dvec3& eulerYprDeg)
    {
        return ellipsoid.computeLocalToWorldTransform(lla) * rotationMatrixYpr(eulerYprDeg);
    }

    void requireMatrixNear(const vsg::dmat4& actual, const vsg::dmat4& expected, double eps = 1e-5)
    {
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
                REQUIRE(nearlyEqual(actual(r, c), expected(r, c), eps));
        }
    }

    void requireEntityLocalTransform(Engine& engine, int id, const vsg::dvec3& position, const vsg::dvec3& ypr,
                                     double eps = 1e-5)
    {
        auto mt = engine.entityTransform(id);
        REQUIRE(mt);
        requireMatrixNear(mt->matrix, expectedLocalEntityMatrix(position, ypr), eps);
    }
} // namespace

// -----------------------------------------------------------------------------
// Entity runtime (acceptance)
// -----------------------------------------------------------------------------

SCENARIO("local entity pose from config matches sampled engine pose",
         "[acceptance][bdd][config][pose][local][entity]")
{
    GIVEN("a Local channel config with entities[].pose.local")
    {
        constexpr vsg::dvec3 kPos{1.5, -2.0, 3.25};
        constexpr vsg::dvec3 kYpr{30.0, 5.0, -2.0};
        const TempConfigFile cfgFile(channelJson(
            "Local", "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr)) + "]"));
        Engine engine;
        initOffscreen(engine, cfgFile.path());
        REQUIRE(engine.config.coordFrame == CoordFrameIntent::LOCAL);

        WHEN("the entity pose is sampled by id")
        {
            vsg::dvec3 position{};
            vsg::dvec3 eulerYprDeg{};
            REQUIRE(engine.sampleEntityPoseById(1, position, eulerYprDeg));
            THEN("sampled pose matches the configured local position and YPR")
            {
                requireDVec3Near(position, kPos);
                requireDVec3Near(eulerYprDeg, kYpr);
            }
        }
    }
}

SCENARIO("ellipsoid entity pose matches EllipsoidPose and not LocalPose",
         "[acceptance][bdd][config][pose][ellipsoid][entity]")
{
    GIVEN("an Ellipsoid config with both pose halves deliberately different")
    {
        constexpr vsg::dvec3 kLla{39.9, 116.4, 12.0};
        constexpr vsg::dvec3 kEllYpr{15.0, 3.0, -1.0};
        const TempConfigFile cfgFile(channelJson(
            "Ellipsoid",
            "[" +
                jsonEntity(1, kTeapot,
                           jsonPoseBoth(vsg::dvec3{100, 200, 300}, vsg::dvec3{90, 0, 0}, kLla, kEllYpr)) +
                "]"));
        Engine engine;
        initOffscreen(engine, cfgFile.path());
        REQUIRE(engine.config.coordFrame == CoordFrameIntent::ELLIPSOID);

        WHEN("the entity pose is sampled by id")
        {
            vsg::dvec3 llaOrPos{};
            vsg::dvec3 ypr{};
            REQUIRE(engine.sampleEntityPoseById(1, llaOrPos, ypr));
            THEN("sample matches ellipsoid LLA/YPR and differs from local half")
            {
                requireDVec3Near(llaOrPos, kLla, 1e-4);
                requireDVec3Near(ypr, kEllYpr, 1e-4);
                REQUIRE_FALSE((nearlyEqual(llaOrPos.x, 100.0) && nearlyEqual(llaOrPos.y, 200.0)));
                REQUIRE_FALSE(nearlyEqual(ypr.x, 90.0));
            }
        }
    }
}

SCENARIO("loaded entity is parented under a MatrixTransform",
         "[acceptance][bdd][config][pose][entity]")
{
    GIVEN("a Local entity config with pose.local")
    {
        const TempConfigFile cfgFile(channelJson(
            "Local",
            "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{1, 2, 3}, vsg::dvec3{0, 0, 0})) + "]"));
        Engine engine;
        initOffscreen(engine, cfgFile.path());

        WHEN("entity parenting is inspected by id")
        {
            THEN("the entity parent is a MatrixTransform")
            {
                REQUIRE(engine.entityTransform(1));
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Multi-entity runtime (acceptance)
// -----------------------------------------------------------------------------

SCENARIO("multiple entities: entitySize matches config and ids resolve one-to-one",
         "[acceptance][bdd][config][pose][entities][id]")
{
    GIVEN("a Local channel config with two entities")
    {
        const std::string entities =
            "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{1, 0, 0}, vsg::dvec3{0, 0, 0})) + ", " +
            jsonEntity(2, kLz, jsonPoseLocalOnly(vsg::dvec3{0, 2, 0}, vsg::dvec3{10, 0, 0})) + "]";
        const TempConfigFile cfgFile(channelJson("Local", entities));
        Engine engine;
        initOffscreen(engine, cfgFile.path());

        WHEN("entity map size and ids are queried")
        {
            THEN("entitySize equals the number of configured models")
            {
                REQUIRE(engine.entitySize() == 2);
            }
            THEN("each configured id resolves; unknown id does not")
            {
                REQUIRE(engine.hasEntityId(1));
                REQUIRE(engine.hasEntityId(2));
                REQUIRE_FALSE(engine.hasEntityId(99));
            }
        }
    }
}

SCENARIO("multiple entities: local pose writes MatrixTransform matching config",
         "[acceptance][bdd][config][pose][entities][local][transform]")
{
    GIVEN("a Local config with two entities, each with distinct pose.local")
    {
        constexpr vsg::dvec3 kPosA{1.5, -2.0, 3.25};
        constexpr vsg::dvec3 kYprA{30.0, 5.0, -2.0};
        constexpr vsg::dvec3 kPosB{0.0, 4.0, -1.0};
        constexpr vsg::dvec3 kYprB{0.0, 15.0, 0.0};
        const std::string entities = "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPosA, kYprA)) + ", " +
                                     jsonEntity(2, kLz, jsonPoseLocalOnly(kPosB, kYprB)) + "]";
        const TempConfigFile cfgFile(channelJson("Local", entities));
        Engine engine;
        initOffscreen(engine, cfgFile.path());
        REQUIRE(engine.entitySize() == 2);

        WHEN("each entity MatrixTransform is read by id")
        {
            THEN("transform matrices match configured local poses (T*R, Rz*Rx*Ry)")
            {
                requireEntityLocalTransform(engine, 1, kPosA, kYprA);
                requireEntityLocalTransform(engine, 2, kPosB, kYprB);
            }
        }
    }
}

SCENARIO("multiple entities: ellipsoid pose writes MatrixTransform matching ECEF config",
         "[acceptance][bdd][config][pose][entities][ellipsoid][transform]")
{
    GIVEN("an Ellipsoid config with two entities and distinct pose.ellipsoid")
    {
        constexpr vsg::dvec3 kLlaA{39.9, 116.4, 12.0};
        constexpr vsg::dvec3 kYprA{15.0, 3.0, -1.0};
        constexpr vsg::dvec3 kLlaB{40.0, 116.5, 50.0};
        constexpr vsg::dvec3 kYprB{0.0, 0.0, 10.0};
        const std::string entities =
            "[" +
            jsonEntity(1, kTeapot,
                       jsonPoseBoth(vsg::dvec3{100, 200, 300}, vsg::dvec3{90, 0, 0}, kLlaA, kYprA)) +
            ", " + jsonEntity(2, kLz, jsonPoseEllipsoidOnly(kLlaB, kYprB)) + "]";
        const TempConfigFile cfgFile(channelJson("Ellipsoid", entities));
        Engine engine;
        initOffscreen(engine, cfgFile.path());
        REQUIRE(engine.ellipsoidModel());
        REQUIRE(engine.entitySize() == 2);
        auto ellipsoid = engine.ellipsoidModel();
        REQUIRE(ellipsoid);

        WHEN("each entity MatrixTransform is read by id")
        {
            auto mtA = engine.entityTransform(1);
            auto mtB = engine.entityTransform(2);
            REQUIRE(mtA);
            REQUIRE(mtB);
            THEN("transform matrices match LocalToWorld(lla)*R_enu(ypr), not the local half")
            {
                requireMatrixNear(mtA->matrix, expectedEllipsoidEntityMatrix(*ellipsoid, kLlaA, kYprA), 1e-4);
                requireMatrixNear(mtB->matrix, expectedEllipsoidEntityMatrix(*ellipsoid, kLlaB, kYprB), 1e-4);
                REQUIRE_FALSE(nearlyEqual(mtA->matrix(0, 3), 100.0));
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Parse rejects (unit)
// -----------------------------------------------------------------------------

TEST_CASE("loadEngineChannelConfig rejects entities item without model",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(channelJson("Local", R"([{ "id": 1, "pose": { "local": { "position": [0,0,0], "eulerYprDeg": [0,0,0] } } }])"));
}

TEST_CASE("loadEngineChannelConfig rejects entities item without id",
          "[unit][config][parse][pose][entities][id]")
{
    requireLoadFails(channelJson("Local", std::string("[{ \"model\": \"") + kTeapot + "\" }]"));
}

TEST_CASE("loadEngineChannelConfig rejects entities item with non-integer id",
          "[unit][config][parse][pose][entities][id]")
{
    requireLoadFails(channelJson("Local", std::string("[{ \"id\": \"a\", \"model\": \"") + kTeapot + "\" }]"));
}

TEST_CASE("loadEngineChannelConfig rejects duplicate entity ids",
          "[unit][config][parse][pose][entities][id]")
{
    requireLoadFails(channelJson(
        "Local", std::string("[{ \"id\": 1, \"model\": \"") + kTeapot + "\" }, { \"id\": 1, \"model\": \"" + kLz +
                     "\" }]"));
}

TEST_CASE("loadEngineChannelConfig rejects empty entities array",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(channelJson("Local", "[]"));
}

TEST_CASE("loadEngineChannelConfig rejects top-level model together with entities",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(std::string("{ \"model\": \"") + kLz + "\", \"entities\": [ " + jsonEntity(1, kTeapot) +
                     " ], " + kWindow + " }");
}

TEST_CASE("loadEngineChannelConfig rejects singular entity together with entities",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(std::string("{ \"entity\": { \"model\": \"") + kLz + "\" }, \"entities\": [ " +
                     jsonEntity(1, kTeapot) + " ], " + kWindow + " }");
}

TEST_CASE("loadEngineChannelConfig rejects pose when selected local half is missing",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(channelJson(
        "Local",
        "[" + jsonEntity(1, kTeapot, jsonPoseEllipsoidOnly(vsg::dvec3{39.9, 116.4, 0}, vsg::dvec3{0, 0, 0})) + "]"));
}

TEST_CASE("loadEngineChannelConfig rejects pose when selected ellipsoid half is missing",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(channelJson(
        "Ellipsoid", "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{0, 0, 0}, vsg::dvec3{0, 0, 0})) + "]"));
}

TEST_CASE("loadEngineChannelConfig rejects local pose with incomplete position array",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(channelJson(
        "Local",
        std::string("[{ \"id\": 1, \"model\": \"") + kTeapot +
            R"(", "pose": { "local": { "position": [1, 2], "eulerYprDeg": [0, 0, 0] } } }])"));
}

TEST_CASE("loadEngineChannelConfig rejects unknown key on entities item",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(channelJson(
        "Local", std::string("[{ \"id\": 1, \"model\": \"") + kTeapot + R"(", "scale": 2.0 }])"));
}

TEST_CASE("loadEngineChannelConfig rejects camera pose when selected local half is missing",
          "[unit][config][parse][pose][camera]")
{
    requireLoadFails(channelJson(
        "Local", "[" + jsonEntity(1, kTeapot) + "]",
        R"({ "pose": { "ellipsoid": { "lla": { "lat": 39.9, "lon": 116.4, "alt": 500 }, "eulerYprDeg": [0, 0, 0] } } })"));
}

TEST_CASE("loadEngineChannelConfig rejects camera local pose with incomplete eulerYprDeg",
          "[unit][config][parse][pose][camera]")
{
    requireLoadFails(channelJson(
        "Local", "[" + jsonEntity(1, kTeapot) + "]",
        R"({ "pose": { "local": { "position": [0, -10, 5], "eulerYprDeg": [0, 0] } } })"));
}

// -----------------------------------------------------------------------------
// Entity name / optional pose / single-entity map / sample-by-id
// -----------------------------------------------------------------------------

SCENARIO("entity name defaults to model basename and explicit name is kept",
         "[acceptance][bdd][config][pose][entities][name]")
{
    GIVEN("two entities: one omits name, one sets name explicitly")
    {
        const std::string entities =
            "[" + jsonEntity(1, kLz) + ", " + jsonEntity(2, kTeapot, {}, "tower") + "]";
        const TempConfigFile cfgFile(channelJson("Local", entities));
        Engine engine;
        initOffscreen(engine, cfgFile.path());

        WHEN("names are read by id")
        {
            std::string nameDefaulted;
            std::string nameExplicit;
            REQUIRE(engine.entityName(1, nameDefaulted));
            REQUIRE(engine.entityName(2, nameExplicit));
            THEN("omitted name is basename(model); explicit name is preserved")
            {
                REQUIRE(nameDefaulted == "lz.vsgt");
                REQUIRE(nameExplicit == "tower");
            }
        }
    }
}

SCENARIO("entity without pose has no MatrixTransform parent",
         "[acceptance][bdd][config][pose][entities][default-place]")
{
    GIVEN("a Local entity with id/model but no pose")
    {
        const TempConfigFile cfgFile(channelJson("Local", "[" + jsonEntity(1, kTeapot) + "]"));
        Engine engine;
        initOffscreen(engine, cfgFile.path());
        REQUIRE(engine.entitySize() == 1);
        REQUIRE(engine.hasEntityId(1));

        WHEN("transform is queried by id")
        {
            THEN("entityTransform is null (default place: no MatrixTransform)")
            {
                REQUIRE_FALSE(engine.entityTransform(1));
            }
        }
    }
}

SCENARIO("single entities entry still registers in the id map",
         "[acceptance][bdd][config][pose][entities][id]")
{
    GIVEN("a Local config with exactly one entities item and pose.local")
    {
        constexpr vsg::dvec3 kPos{2.0, 3.0, 4.0};
        constexpr vsg::dvec3 kYpr{5.0, 0.0, 0.0};
        const TempConfigFile cfgFile(
            channelJson("Local", "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr)) + "]"));
        Engine engine;
        initOffscreen(engine, cfgFile.path());

        WHEN("the entity map and transform are inspected")
        {
            THEN("size is 1, id resolves, and transform matches pose.local")
            {
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(engine.hasEntityId(1));
                requireEntityLocalTransform(engine, 1, kPos, kYpr);
            }
        }
    }
}

SCENARIO("sampleEntityPoseById matches MatrixTransform for local pose",
         "[acceptance][bdd][config][pose][entities][local][sample]")
{
    GIVEN("a Local entity with pose.local")
    {
        constexpr vsg::dvec3 kPos{1.0, -2.0, 3.0};
        constexpr vsg::dvec3 kYpr{12.0, 4.0, -3.0};
        const TempConfigFile cfgFile(
            channelJson("Local", "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr)) + "]"));
        Engine engine;
        initOffscreen(engine, cfgFile.path());

        WHEN("pose is sampled by id")
        {
            vsg::dvec3 position{};
            vsg::dvec3 ypr{};
            REQUIRE(engine.sampleEntityPoseById(1, position, ypr));
            THEN("sample matches config and is consistent with the transform matrix")
            {
                requireDVec3Near(position, kPos);
                requireDVec3Near(ypr, kYpr);
                requireEntityLocalTransform(engine, 1, position, ypr);
            }
        }
    }
}

SCENARIO("duplicate entity names are allowed; lookup is by id only",
         "[acceptance][bdd][config][pose][entities][name][id]")
{
    GIVEN("two entities sharing the same display name but different ids")
    {
        const std::string entities =
            "[" + jsonEntity(1, kTeapot, {}, "twin") + ", " + jsonEntity(2, kLz, {}, "twin") + "]";
        const TempConfigFile cfgFile(channelJson("Local", entities));
        Engine engine;
        initOffscreen(engine, cfgFile.path());

        WHEN("entities are resolved by id and name strings are read")
        {
            std::string nameLeft;
            std::string nameRight;
            REQUIRE(engine.entitySize() == 2);
            REQUIRE(engine.hasEntityId(1));
            REQUIRE(engine.hasEntityId(2));
            REQUIRE_FALSE(engine.hasEntityId(99));
            REQUIRE(engine.entityName(1, nameLeft));
            REQUIRE(engine.entityName(2, nameRight));
            THEN("both keep name twin; name is not a map key")
            {
                REQUIRE(nameLeft == "twin");
                REQUIRE(nameRight == "twin");
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Camera runtime (acceptance)
// -----------------------------------------------------------------------------

SCENARIO("local camera pose from config matches LookAt",
         "[acceptance][bdd][config][pose][local][camera]")
{
    GIVEN("a Local config with camera.pose.local")
    {
        constexpr vsg::dvec3 kPos{0.0, -50.0, 10.0};
        constexpr vsg::dvec3 kYpr{20.0, 5.0, 0.0};
        const TempConfigFile cfgFile(channelJson(
            "Local", "[" + jsonEntity(1, kTeapot) + "]",
            std::string(R"({ "pose": { "local": )") + jsonLocalPose(kPos, kYpr) + " } }"));
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;
        REQUIRE(engine.loadConfig(cfgFile.path()));
        REQUIRE(engine.config.hasCamera);
        REQUIRE(engine.config.camera.hasPoseLocal);
        REQUIRE(engine.init());

        WHEN("the main camera LookAt is inspected")
        {
            THEN("LookAt matches the configured local camera pose")
            {
                requireLookAtMatchesLocalPose(engine, kPos, kYpr);
            }
        }
    }
}

SCENARIO("ellipsoid camera pose matches EllipsoidPose not LocalPose",
         "[acceptance][bdd][config][pose][ellipsoid][camera]")
{
    GIVEN("an Ellipsoid config with different camera pose halves")
    {
        const TempConfigFile cfgFile(channelJson(
            "Ellipsoid", "[" + jsonEntity(1, kTeapot) + "]",
            std::string(R"({ "pose": )") +
                jsonPoseBoth(vsg::dvec3{0, -50, 10}, vsg::dvec3{90, 0, 0}, vsg::dvec3{39.9, 116.4, 500},
                             vsg::dvec3{0, 10, 0}) +
                " }"));
        Engine engine;
        initOffscreen(engine, cfgFile.path());
        REQUIRE(engine.ellipsoidModel());
        auto ep = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
        REQUIRE(ep);
        REQUIRE(ep->ellipsoidModel);

        WHEN("the main camera LookAt is inspected")
        {
            THEN("LookAt matches the ellipsoid half (lla + ENU YPR), not the local half")
            {
                requireLookAtMatchesLlaPose(engine, *ep->ellipsoidModel, vsg::dvec3{39.9, 116.4, 500.0},
                                            vsg::dvec3{0.0, 10.0, 0.0});
            }
        }
    }
}

// -----------------------------------------------------------------------------
// System: shipped resource configs (pose_local_teapot / pose_ellipsoid_tiananmen)
// -----------------------------------------------------------------------------

SCENARIO("system loads pose_local_teapot.json with one local entity and camera",
         "[system][bdd][config][pose][local][resource]")
{
    GIVEN("the shipped Local teapot pose config")
    {
        Engine engine;
        engine.showWindow = false;
        REQUIRE(engine.loadConfig(resourceConfigPath("pose_local_teapot.json")));
        REQUIRE(engine.init());

        WHEN("scene mode, entity map, entity pose, and camera are inspected")
        {
            THEN("there is no EllipsoidModel and exactly one entity id 1")
            {
                REQUIRE_FALSE(engine.ellipsoidModel());
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(engine.hasEntityId(1));
            }
            THEN("entity 1 pose and MatrixTransform match the config local placement")
            {
                vsg::dvec3 position{};
                vsg::dvec3 ypr{};
                REQUIRE(engine.sampleEntityPoseById(1, position, ypr));
                requireDVec3Near(position, vsg::dvec3{1.0, 0.0, 0.0});
                requireDVec3Near(ypr, vsg::dvec3{0.0, 0.0, 0.0});
                REQUIRE(engine.entityTransform(1));
            }
            THEN("main camera LookAt matches the config local camera pose")
            {
                REQUIRE(engine.mainCamera());
                requireLookAtMatchesLocalPose(engine, vsg::dvec3{0.0, 0.0, 0.0}, vsg::dvec3{-90.0, 0.0, 0.0});
            }
        }
    }
}

SCENARIO("system loads pose_ellipsoid_tiananmen.json with one ECEF entity and camera",
         "[system][bdd][config][pose][ellipsoid][resource]")
{
    GIVEN("the shipped Ellipsoid Tiananmen teapot pose config")
    {
        Engine engine;
        engine.showWindow = false;
        REQUIRE(engine.loadConfig(resourceConfigPath("pose_ellipsoid_tiananmen.json")));
        REQUIRE(engine.init());

        WHEN("scene mode, entity map, entity pose, and camera are inspected")
        {
            THEN("EllipsoidModel is present and exactly one entity id 1")
            {
                REQUIRE(engine.ellipsoidModel());
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(engine.hasEntityId(1));
            }
            THEN("entity 1 pose and MatrixTransform match the Tiananmen ground placement")
            {
                vsg::dvec3 lla{};
                vsg::dvec3 ypr{};
                REQUIRE(engine.sampleEntityPoseById(1, lla, ypr));
                requireDVec3Near(lla, vsg::dvec3{39.9087, 116.3975, 0.0}, 1e-4);
                requireDVec3Near(ypr, vsg::dvec3{0.0, 0.0, 0.0}, 1e-4);
                REQUIRE(engine.entityTransform(1));
            }
            THEN("main camera uses EllipsoidPerspective and LookAt matches south-side view")
            {
                REQUIRE(engine.mainCamera());
                auto ep = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
                REQUIRE(ep);
                REQUIRE(ep->ellipsoidModel);
                requireLookAtMatchesLlaPose(engine, *ep->ellipsoidModel, vsg::dvec3{39.90852, 116.3975, 3.0},
                                            vsg::dvec3{0.0, -12.0, 0.0});
            }
        }
    }
}
