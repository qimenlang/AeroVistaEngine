#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "InitialCameraConfig.h"
#include "engine.h"

#include <vsgXchange/all.h>

#include "Common.h"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

// 位姿配置设计.md — entity/camera 双轨摆模与初始相机；含 shipped resource 系统测。

namespace
{
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

    CigiEntityCtrlV4 makeEntityCtrl(int id, CigiBaseEntityCtrl::EntityStateGrp state, std::uint8_t alpha = 255)
    {
        CigiEntityCtrlV4 ctrl;
        ctrl.SetEntityID(static_cast<Cigi_uint16>(id));
        ctrl.SetEntityState(state);
        ctrl.SetAlpha(alpha);
        return ctrl;
    }

    CigiEntityPositionCtrlV4 makeEntityPositionLla(int id, const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg)
    {
        CigiEntityPositionCtrlV4 pose;
        pose.SetEntityID(static_cast<Cigi_uint16>(id));
        pose.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
        pose.SetLat(lla.x);
        pose.SetLon(lla.y);
        pose.SetAlt(lla.z);
        pose.SetYaw(static_cast<float>(eulerYprDeg.x));
        pose.SetPitch(static_cast<float>(eulerYprDeg.y));
        pose.SetRoll(static_cast<float>(eulerYprDeg.z));
        return pose;
    }

    std::string jsonEntity(int id, const std::string& model, const std::string& poseObject = {},
                           const std::string& name = {}, const std::string& initialEntityState = {})
    {
        std::string s = R"({ "id": )" + std::to_string(id) + R"(, "model": ")" + model + "\"";
        if (!name.empty())
            s += R"(, "name": ")" + name + "\"";
        if (!initialEntityState.empty())
            s += R"(, "initialEntityState": ")" + initialEntityState + "\"";
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

    /// 场景根下每个实体一个 Switch 实例（共享几何不算实例）。
    std::size_t hungEntityInstanceCount(vsg::ref_ptr<vsg::Node> scene)
    {
        const auto root = scene ? scene.cast<vsg::Group>() : vsg::ref_ptr<vsg::Group>{};
        if (!root)
            return 0;
        std::size_t n = 0;
        for (const auto& child : root->children)
        {
            if (child.cast<vsg::Switch>())
                ++n;
        }
        return n;
    }

    bool sceneHangsEntityTransform(vsg::ref_ptr<vsg::Node> scene, vsg::ref_ptr<vsg::MatrixTransform> transform)
    {
        const auto root = scene ? scene.cast<vsg::Group>() : vsg::ref_ptr<vsg::Group>{};
        if (!root || !transform)
            return false;
        for (const auto& child : root->children)
        {
            const auto visibility = child.cast<vsg::Switch>();
            if (!visibility)
                continue;
            for (const auto& instance : visibility->children)
            {
                if (instance.node == transform)
                    return true;
            }
        }
        return false;
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

    vsg::ref_ptr<vsg::Options> testOptions()
    {
        auto options = vsg::Options::create();
        options->sharedObjects = vsg::SharedObjects::create();
        options->add(vsgXchange::all::create());
        options->paths.push_back(vsg::Path(RESOURCE_DIR));
        return options;
    }

    void requireFiniteLookAt(const vsg::LookAt& lookAt)
    {
        for (const double c : {lookAt.eye.x, lookAt.eye.y, lookAt.eye.z, lookAt.center.x, lookAt.center.y,
                               lookAt.center.z, lookAt.up.x, lookAt.up.y, lookAt.up.z})
        {
            REQUIRE(std::isfinite(c));
        }
        REQUIRE(vsg::length(lookAt.up) > 1e-9);
        REQUIRE(vsg::length(lookAt.center - lookAt.eye) > 1e-9);
    }

    /// Mirror Engine::finishGraphicsAfterScene bounds (centre + radius factor 0.6).
    void computeNodeCentreRadius(vsg::ref_ptr<vsg::Node> root, vsg::dvec3& centre, double& radius)
    {
        vsg::ComputeBounds computeBounds;
        root->accept(computeBounds);
        centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
        radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;
        REQUIRE(radius > 0.0);
    }

    void requireLookAtMatchesLocalAabbDefault(Engine& engine, const vsg::dvec3& entityPosition,
                                              const vsg::dvec3& entityYpr)
    {
        auto options = testOptions();
        auto loaded = vsg::read_cast<vsg::Node>(vsg::Path(RESOURCE_DIR) / kTeapot, options);
        REQUIRE(loaded);
        auto mt = vsg::MatrixTransform::create();
        mt->matrix = expectedLocalEntityMatrix(entityPosition, entityYpr);
        mt->addChild(loaded);
        auto root = vsg::Group::create();
        root->addChild(mt);

        vsg::dvec3 centre{};
        double radius = 0.0;
        computeNodeCentreRadius(root, centre, radius);

        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);
        requireFiniteLookAt(*lookAt);
        const vsg::dvec3 expectedEye = centre + vsg::dvec3(0.0, -radius * initial_camera::kLocalBackMultiplier, 0.0);
        REQUIRE(vsg::length(lookAt->eye - expectedEye) < 1e-4);
        REQUIRE(vsg::length(lookAt->center - centre) < 1e-4);
        REQUIRE(vsg::length(vsg::normalize(lookAt->up) - vsg::dvec3(0.0, 0.0, 1.0)) < 1e-6);
    }

    /// 位姿配置设计.md §4 Ellipsoid：eye = centre - north·(1.5 R) + up·(0.35 R)，看向 centre。
    void requireLookAtMatchesEllipsoidAabbDefault(Engine& engine, const vsg::dvec3& entityLla,
                                                  const vsg::dvec3& entityYpr)
    {
        auto ellipsoid = engine.ellipsoidModel();
        REQUIRE(ellipsoid);

        auto options = testOptions();
        auto loaded = vsg::read_cast<vsg::Node>(vsg::Path(RESOURCE_DIR) / kTeapot, options);
        REQUIRE(loaded);
        auto mt = vsg::MatrixTransform::create();
        mt->matrix = expectedEllipsoidEntityMatrix(*ellipsoid, entityLla, entityYpr);
        mt->addChild(loaded);
        auto root = vsg::Group::create();
        root->addChild(mt);

        vsg::dvec3 centre{};
        double radius = 0.0;
        computeNodeCentreRadius(root, centre, radius);

        const vsg::dvec3 llaC = ellipsoid->convertECEFToLatLongAltitude(centre);
        const vsg::dmat4 localToWorld = ellipsoid->computeLocalToWorldTransform(llaC);
        const vsg::dvec3 north =
            vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
        const vsg::dvec3 up =
            vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
        const vsg::dvec3 expectedEye = centre - north * (initial_camera::kEllipsoidBackMultiplier * radius) + up * (initial_camera::kEllipsoidUpMultiplier * radius);

        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);
        requireFiniteLookAt(*lookAt);
        auto ep = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
        REQUIRE(ep);
        REQUIRE(ep->ellipsoidModel);

        REQUIRE(vsg::length(lookAt->eye - expectedEye) < 1.0); // metre-scale ECEF
        REQUIRE(vsg::length(lookAt->center - centre) < 1.0);
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
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr)) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE_FALSE(engine.config.injectEllipsoidIfMissing);

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
        EntitiesConfig cfg("[" +
                               jsonEntity(1, kTeapot,
                                          jsonPoseBoth(vsg::dvec3{100, 200, 300}, vsg::dvec3{90, 0, 0}, kLla, kEllYpr)) +
                               "]",
                           {}, true);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.config.injectEllipsoidIfMissing);

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
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{1, 2, 3}, vsg::dvec3{0, 0, 0})) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

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

SCENARIO("multiple entities: catalog entries are registered and hung on the scene",
         "[acceptance][bdd][config][pose][entities][id][ENT-02-preload]")
{
    GIVEN("a Local channel config with two entities")
    {
        const std::string entities =
            "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{1, 0, 0}, vsg::dvec3{0, 0, 0})) + ", " +
            jsonEntity(2, kLz, jsonPoseLocalOnly(vsg::dvec3{0, 2, 0}, vsg::dvec3{10, 0, 0})) + "]";
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("entity map, ids, and scene instances are queried")
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
            THEN("hung scene instances equal the catalog size and each transform is on the tree")
            {
                REQUIRE(hungEntityInstanceCount(engine.mainScene()) == engine.entitySize());
                REQUIRE(sceneHangsEntityTransform(engine.mainScene(), engine.entityTransform(1)));
                REQUIRE(sceneHangsEntityTransform(engine.mainScene(), engine.entityTransform(2)));
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
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
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
        EntitiesConfig cfg(entities, {}, true);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
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

TEST_CASE("loadEngineChannelConfig rejects singular entity key",
          "[unit][config][parse][pose][entities]")
{
    requireLoadFails(std::string("{ \"entity\": { \"model\": \"") + kLz + "\" }, " + kWindow + " }");
}

TEST_CASE("loadEntitiesFile accepts pose with only the non-selected half",
          "[unit][config][parse][pose][entities-catalog]")
{
    // 双轨自由解析（2026-09 收敛）：不再按 injectEllipsoidIfMissing 强制某半，两半均可解析。
    const TempConfigFile file(
        std::string(R"({ "entities": [ )") +
        jsonEntity(1, kTeapot, jsonPoseEllipsoidOnly(vsg::dvec3{39.9, 116.4, 0}, vsg::dvec3{0, 0, 0})) + R"( ] })");
    std::vector<EntityConfig> entities;
    std::string error;
    REQUIRE(loadEntitiesFile(file.path(), entities, &error));
    REQUIRE(entities.size() == 1);
    REQUIRE(entities[0].hasPoseEllipsoid);
    REQUIRE_FALSE(entities[0].hasPoseLocal);
}

TEST_CASE("loadEntitiesFile accepts pose with ellipsoid half absent", "[unit][config][parse][pose][entities-catalog]")
{
    // 双轨自由解析（2026-09 收敛）：不强制 pose 必须带某半。
    const TempConfigFile file(
        std::string(R"({ "entities": [ )") +
        jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{0, 0, 0}, vsg::dvec3{0, 0, 0})) + R"( ] })");
    std::vector<EntityConfig> entities;
    std::string error;
    REQUIRE(loadEntitiesFile(file.path(), entities, &error));
    REQUIRE(entities.size() == 1);
    REQUIRE(entities[0].hasPoseLocal);
    REQUIRE_FALSE(entities[0].hasPoseEllipsoid);
}

TEST_CASE("loadEngineChannelConfig accepts camera pose with only the non-selected half",
          "[unit][config][parse][pose][camera]")
{
    // 双轨自由解析（2026-09 收敛）：不强制 camera pose 必须带「选中」半，运行时按场景有无椭球选半。
    const TempConfigFile file(std::string("{ ") + kWindow +
                              R"(, "camera": { "pose": { "ellipsoid": { "lla": { "lat": 39.9, "lon": 116.4, "alt": 500 }, )"
                              R"("eulerYprDeg": [0, 0, 0] } } } })");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE(cfg.camera.has_value());
    REQUIRE(cfg.camera->hasPoseEllipsoid);
    REQUIRE_FALSE(cfg.camera->hasPoseLocal);
}

TEST_CASE("loadEngineChannelConfig rejects camera local pose with incomplete eulerYprDeg",
          "[unit][config][parse][pose][camera]")
{
    requireLoadFails(std::string("{ ") + kWindow +
                     R"(, "camera": { "pose": { "local": { "position": [0, -10, 5], "eulerYprDeg": [0, 0] } } } })");
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
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

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

SCENARIO("entity without pose uses an origin MatrixTransform",
         "[acceptance][bdd][config][pose][entities][default-place][ENT-02-pose-default]")
{
    GIVEN("a Local entity with id/model but no pose")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.entitySize() == 1);
        REQUIRE(engine.hasEntityId(1));

        WHEN("transform and pose are queried by id")
        {
            vsg::dvec3 position{};
            vsg::dvec3 ypr{};
            THEN("a MatrixTransform exists and sampled pose is the origin")
            {
                REQUIRE(engine.entityTransform(1));
                REQUIRE(engine.sampleEntityPoseById(1, position, ypr));
                requireDVec3Near(position, vsg::dvec3{0.0, 0.0, 0.0});
                requireDVec3Near(ypr, vsg::dvec3{0.0, 0.0, 0.0});
            }
        }
    }
}

SCENARIO("single entities entry still registers in the id map",
         "[acceptance][bdd][config][pose][entities][id][ENT-02-preload]")
{
    GIVEN("a Local config with exactly one entities item and pose.local")
    {
        constexpr vsg::dvec3 kPos{2.0, 3.0, 4.0};
        constexpr vsg::dvec3 kYpr{5.0, 0.0, 0.0};
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr)) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

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
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr)) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

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
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

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
// 启动装配（实体与运动控制设计.md §7.2 / §11 ②；验收码 ENT-02-*）
// -----------------------------------------------------------------------------

SCENARIO("Standby entity is hidden at startup without a Host",
         "[acceptance][bdd][config][entities][visibility][ENT-02-visibility]")
{
    GIVEN("a Local entity configured Standby")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, {}, {}, "Standby") + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("startup visibility is queried")
        {
            THEN("the entity is in the map and not visible")
            {
                REQUIRE(engine.hasEntityId(1));
                REQUIRE_FALSE(engine.entityVisible(1));
            }
        }
    }
}

SCENARIO("Active entity is visible at startup without a Host",
         "[acceptance][bdd][config][entities][visibility][ENT-02-visibility]")
{
    GIVEN("a Local entity that omits initialEntityState")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("startup visibility is queried")
        {
            THEN("the entity is visible")
            {
                REQUIRE(engine.hasEntityId(1));
                REQUIRE(engine.entityVisible(1));
            }
        }
    }
}

SCENARIO("two entities with the same model share one geometry node",
         "[acceptance][bdd][config][entities][shared-geometry][ENT-02-shared-geom]")
{
    GIVEN("two Local teapot entities at different poses")
    {
        const std::string entities =
            "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{0, 0, 0}, vsg::dvec3{0, 0, 0})) + ", " +
            jsonEntity(2, kTeapot, jsonPoseLocalOnly(vsg::dvec3{10, 0, 0}, vsg::dvec3{0, 0, 0})) + "]";
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("geometry nodes and transforms are compared")
        {
            THEN("the nodes are the same instance and the transforms differ")
            {
                auto nodeA = engine.entityNode(1);
                auto nodeB = engine.entityNode(2);
                REQUIRE(nodeA);
                REQUIRE(nodeB);
                REQUIRE(nodeA == nodeB);
                REQUIRE(engine.entityTransform(1));
                REQUIRE(engine.entityTransform(2));
                REQUIRE(engine.entityTransform(1) != engine.entityTransform(2));
            }
        }
    }
}

SCENARIO("hidden Standby sibling is excluded from the scene AABB",
         "[acceptance][bdd][config][entities][aabb][ENT-02-aabb-hidden]")
{
    GIVEN("an Active teapot at the origin and a Standby teapot far on +X")
    {
        const std::string entities =
            "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{0, 0, 0}, vsg::dvec3{0, 0, 0})) + ", " +
            jsonEntity(2, kTeapot, jsonPoseLocalOnly(vsg::dvec3{100, 0, 0}, vsg::dvec3{0, 0, 0}), {}, "Standby") + "]";
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("the scene AABB is computed")
        {
            vsg::ComputeBounds computeBounds;
            engine.mainScene()->accept(computeBounds);
            const vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
            THEN("the centre stays near the visible entity, not the midpoint")
            {
                REQUIRE(centre.x < 20.0);
            }
        }
    }
}

SCENARIO("init continues when one entity model fails to load",
         "[acceptance][bdd][config][entities][negative][ENT-02-load-skip]")
{
    GIVEN("one valid teapot and one missing model path")
    {
        const std::string entities =
            "[" + jsonEntity(1, kTeapot) + ", " + jsonEntity(2, "models/missing.vsgt") + "]";
        EntitiesConfig cfg(entities);
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;
        REQUIRE(engine.loadConfig(cfg.cfgFile->path()));

        WHEN("the engine initializes")
        {
            THEN("init succeeds and only the valid id is registered")
            {
                REQUIRE(engine.init());
                REQUIRE(engine.hasEntityId(1));
                REQUIRE_FALSE(engine.hasEntityId(2));
                REQUIRE(engine.entitySize() == 1);
            }
        }
    }
}

SCENARIO("all Standby entities use the local camera fallback",
         "[acceptance][bdd][config][entities][aabb][camera][ENT-02-aabb-hidden]")
{
    GIVEN("two Local Standby entities and no camera pose")
    {
        const std::string entities = "[" + jsonEntity(1, kTeapot, {}, {}, "Standby") + ", " +
                                     jsonEntity(2, kLz, {}, {}, "Standby") + "]";
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("the main camera LookAt is inspected")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);
            THEN("eye uses the empty-AABB fallback")
            {
                REQUIRE(vsg::length(lookAt->eye - vsg::dvec3(0.0, 0.0, 10.0)) < 1e-6);
                REQUIRE(vsg::length(lookAt->center - vsg::dvec3(0.0, 0.0, 0.0)) < 1e-6);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 运行期 EntityCtrl（实体与运动控制设计.md §9 / §11 ③；验收码 ENT-03-*）
// 测试直接调公开 onEntityCtrl / onEntityPose（与既有位姿注入同层），不改 ENT-02。
// -----------------------------------------------------------------------------

SCENARIO("Host EntityCtrl Active shows a hidden Standby entity",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-active]")
{
    GIVEN("a Local Standby teapot with no pose packet yet")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, {}, {}, "Standby") + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE_FALSE(engine.entityVisible(1));

        WHEN("Host sends EntityCtrl Active")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("the entity is visible without a position packet")
            {
                REQUIRE(engine.hasEntityId(1));
                REQUIRE(engine.entityVisible(1));
            }
        }
    }
}

SCENARIO("Host EntityCtrl Standby hides a visible entity and keeps the instance",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-standby]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.entityVisible(1));
        const auto geometry = engine.entityNode(1);

        WHEN("Host sends EntityCtrl Standby")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Standby));

            THEN("the entity is hidden and still registered")
            {
                REQUIRE(engine.hasEntityId(1));
                REQUIRE_FALSE(engine.entityVisible(1));
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(engine.entityNode(1) == geometry);
            }
        }
    }
}

SCENARIO("Host EntityCtrl Destroyed hides like Standby and Active can show again",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-destroyed]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.entityVisible(1));

        WHEN("Host sends EntityCtrl Destroyed")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Destroyed));

            THEN("the entity is hidden and the instance is kept")
            {
                REQUIRE(engine.hasEntityId(1));
                REQUIRE_FALSE(engine.entityVisible(1));
                REQUIRE(engine.entitySize() == 1);
            }

            AND_WHEN("Host sends EntityCtrl Active")
            {
                engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

                THEN("the entity is visible again")
                {
                    REQUIRE(engine.hasEntityId(1));
                    REQUIRE(engine.entityVisible(1));
                }
            }
        }
    }
}

SCENARIO("Host EntityCtrl Remove hides like Destroyed",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-destroyed]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.entityVisible(1));

        WHEN("Host sends EntityCtrl Remove")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Remove));

            THEN("the entity is hidden and the instance is kept")
            {
                REQUIRE(engine.hasEntityId(1));
                REQUIRE_FALSE(engine.entityVisible(1));
                REQUIRE(engine.entitySize() == 1);
            }
        }
    }
}

SCENARIO("repeating EntityCtrl Active keeps a single visible instance",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-active-idempotent]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        const auto geometry = engine.entityNode(1);
        REQUIRE(engine.entityVisible(1));
        REQUIRE(engine.entitySize() == 1);

        WHEN("Host sends EntityCtrl Active again")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("visibility and catalog size are unchanged")
            {
                REQUIRE(engine.entityVisible(1));
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(engine.entityNode(1) == geometry);
            }
        }
    }
}

SCENARIO("EntityCtrl for an id outside the catalog changes nothing",
         "[acceptance][bdd][entities][entity-ctrl][negative][ENT-03-unmapped]")
{
    GIVEN("one Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.entityVisible(1));

        WHEN("Host sends EntityCtrl for an id not in the catalog")
        {
            engine.onEntityCtrl(makeEntityCtrl(99, CigiBaseEntityCtrl::Standby));

            THEN("no instance is created and the catalog entity is unchanged")
            {
                REQUIRE_FALSE(engine.hasEntityId(99));
                REQUIRE(engine.hasEntityId(1));
                REQUIRE(engine.entityVisible(1));
                REQUIRE(engine.entitySize() == 1);
            }
        }
    }
}

SCENARIO("EntityCtrl for a skipped load id has no effect",
         "[acceptance][bdd][entities][entity-ctrl][negative][ENT-03-failed-id]")
{
    GIVEN("one valid teapot and one missing model path")
    {
        const std::string entities =
            "[" + jsonEntity(1, kTeapot) + ", " + jsonEntity(2, "models/missing.vsgt") + "]";
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.hasEntityId(1));
        REQUIRE_FALSE(engine.hasEntityId(2));
        REQUIRE(engine.entityVisible(1));

        WHEN("Host sends EntityCtrl Active for the skipped id")
        {
            engine.onEntityCtrl(makeEntityCtrl(2, CigiBaseEntityCtrl::Active));

            THEN("the skipped id stays absent and the valid entity is unchanged")
            {
                REQUIRE_FALSE(engine.hasEntityId(2));
                REQUIRE_FALSE(engine.entityVisible(2));
                REQUIRE(engine.hasEntityId(1));
                REQUIRE(engine.entityVisible(1));
                REQUIRE(engine.entitySize() == 1);
            }
        }
    }
}

SCENARIO("activating a Standby entity keeps the configured pose",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-standby-pose]")
{
    GIVEN("a Local Standby teapot with a configured pose")
    {
        constexpr vsg::dvec3 kPos{4.0, 2.0, 1.0};
        constexpr vsg::dvec3 kYpr{15.0, 0.0, 0.0};
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr), {}, "Standby") + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE_FALSE(engine.entityVisible(1));

        WHEN("Host sends EntityCtrl Active without a position packet")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("the entity is visible at the configured pose")
            {
                REQUIRE(engine.entityVisible(1));
                vsg::dvec3 position{};
                vsg::dvec3 ypr{};
                REQUIRE(engine.sampleEntityPoseById(1, position, ypr));
                requireDVec3Near(position, kPos);
                requireDVec3Near(ypr, kYpr);
            }
        }
    }
}

SCENARIO("hiding and showing does not restore the configured pose",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-pose-keep]")
{
    GIVEN("an Ellipsoid Active teapot at a configured LLA")
    {
        constexpr vsg::dvec3 kConfigLla{39.9087, 116.3975, 0.0};
        constexpr vsg::dvec3 kPlacedLla{40.0, 116.5, 50.0};
        constexpr vsg::dvec3 kPlacedYpr{30.0, 0.0, 0.0};
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseEllipsoidOnly(kConfigLla, vsg::dvec3{0, 0, 0})) + "]",
                           {}, true);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.entityVisible(1));

        WHEN("Host places a new pose then hides and shows the entity")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));
            engine.onEntityPose(makeEntityPositionLla(1, kPlacedLla, kPlacedYpr));
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Standby));
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("the sampled pose stays at the placed LLA")
            {
                REQUIRE(engine.entityVisible(1));
                vsg::dvec3 lla{};
                vsg::dvec3 ypr{};
                REQUIRE(engine.sampleEntityPoseById(1, lla, ypr));
                requireDVec3Near(lla, kPlacedLla, 1e-4);
                requireDVec3Near(ypr, kPlacedYpr, 1e-4);
            }
        }
    }
}

SCENARIO("hiding and showing reuses the prebuilt geometry",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-no-reload]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        const auto geometry = engine.entityNode(1);
        REQUIRE(geometry);

        WHEN("Host hides then shows the entity")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Standby));
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("the entity is visible on the same geometry node")
            {
                REQUIRE(engine.entityVisible(1));
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(engine.entityNode(1) == geometry);
            }
        }
    }
}

SCENARIO("Host EntityPositionCtrl places an Active entity",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-place]")
{
    GIVEN("an Ellipsoid Standby teapot")
    {
        constexpr vsg::dvec3 kConfigLla{39.9087, 116.3975, 0.0};
        constexpr vsg::dvec3 kPlacedLla{39.91, 116.40, 100.0};
        constexpr vsg::dvec3 kPlacedYpr{15.0, 0.0, 0.0};
        EntitiesConfig cfg(
            "[" + jsonEntity(1, kTeapot, jsonPoseEllipsoidOnly(kConfigLla, vsg::dvec3{0, 0, 0}), {}, "Standby") + "]",
            {}, true);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("Host activates then places the entity")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));
            engine.onEntityPose(makeEntityPositionLla(1, kPlacedLla, kPlacedYpr));

            THEN("the sampled pose matches the placed LLA")
            {
                REQUIRE(engine.entityVisible(1));
                vsg::dvec3 lla{};
                vsg::dvec3 ypr{};
                REQUIRE(engine.sampleEntityPoseById(1, lla, ypr));
                requireDVec3Near(lla, kPlacedLla, 1e-4);
                requireDVec3Near(ypr, kPlacedYpr, 1e-4);
            }
        }
    }
}

SCENARIO("EntityCtrl Alpha applies while the entity is already Active",
         "[acceptance][bdd][entities][entity-ctrl][ENT-03-attrs]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.entityVisible(1));
        REQUIRE(engine.entityAlpha(1) == std::uint8_t{255});

        WHEN("Host sends EntityCtrl Active with Alpha 128")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active, 128));

            THEN("the entity stays visible and alpha is 128")
            {
                REQUIRE(engine.entityVisible(1));
                REQUIRE(engine.entityAlpha(1) == std::uint8_t{128});
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
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]",
                           std::string(R"({ "pose": { "local": )") + jsonLocalPose(kPos, kYpr) + " } }");
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;
        REQUIRE(engine.loadConfig(cfg.cfgFile->path()));
        REQUIRE(engine.config.camera.has_value());
        REQUIRE(engine.config.camera->hasPoseLocal);
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
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]",
                           std::string(R"({ "pose": )") +
                               jsonPoseBoth(vsg::dvec3{0, -50, 10}, vsg::dvec3{90, 0, 0}, vsg::dvec3{39.9, 116.4, 500},
                                            vsg::dvec3{0, 10, 0}) +
                               " }",
                           true);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
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
// Default camera from scene AABB when camera.pose is omitted (位姿配置设计.md §4)
// -----------------------------------------------------------------------------

SCENARIO("no camera config: Local default LookAt frames entities AABB",
         "[acceptance][bdd][config][pose][local][camera][aabb-default]")
{
    GIVEN("a Local entities config without camera")
    {
        constexpr vsg::dvec3 kPos{2.0, 0.0, 0.0};
        constexpr vsg::dvec3 kYpr{0.0, 0.0, 0.0};
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr)) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("config and main camera are inspected")
        {
            THEN("camera key is absent and LookAt matches AABB default (设计 §4 Local)")
            {
                REQUIRE_FALSE(engine.config.camera);
                REQUIRE(engine.mainCamera());
                requireLookAtMatchesLocalAabbDefault(engine, kPos, kYpr);
            }
        }
    }
}

SCENARIO("no camera config: Ellipsoid default LookAt frames entities AABB",
         "[acceptance][bdd][config][pose][ellipsoid][camera][aabb-default]")
{
    GIVEN("an Ellipsoid entities config without camera, model pinned away from Beijing default")
    {
        // 远离硬编码 (39.9,116.4,500)，确保北京回退不会偶然通过。
        constexpr vsg::dvec3 kLla{-33.8688, 151.2093, 0.0};
        constexpr vsg::dvec3 kYpr{0.0, 0.0, 0.0};
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseEllipsoidOnly(kLla, kYpr)) + "]",
                           {}, true);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());

        WHEN("config and main camera are inspected")
        {
            THEN("camera key is absent and LookAt matches AABB→ENU default (设计 §4 Ellipsoid)")
            {
                REQUIRE_FALSE(engine.config.camera);
                REQUIRE(engine.mainCamera());
                REQUIRE(engine.ellipsoidModel());
                requireLookAtMatchesEllipsoidAabbDefault(engine, kLla, kYpr);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// System: scene configs via inline snapshot (kSceneLocalJson / kSceneEcefJson)
// -----------------------------------------------------------------------------

SCENARIO("system loads scene_local config with one local entity and camera",
         "[system][bdd][config][pose][local][resource]")
{
    GIVEN("a Local teapot pose config")
    {
        Engine engine;
        engine.showWindow = false;
        const std::string entities =
            "[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(vsg::dvec3{10.0, 0.0, -2.0}, vsg::dvec3{90.0, 0.0, 0.0})) + "]";
        const std::string cameraObject =
            std::string(R"({ "pose": { "local": )") + jsonLocalPose(vsg::dvec3{0.0, 0.0, 0.0}, vsg::dvec3{-90.0, 0.0, 0.0}) + " } }";
        EntitiesConfig cfg(entities, cameraObject);
        REQUIRE(engine.loadConfig(cfg.cfgFile->path()));
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
                requireDVec3Near(position, vsg::dvec3{10.0, 0.0, -2.0});
                requireDVec3Near(ypr, vsg::dvec3{90.0, 0.0, 0.0});
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

SCENARIO("system loads scene_ecef config with one ECEF entity and camera",
         "[system][bdd][config][pose][ellipsoid][resource]")
{
    GIVEN("an Ellipsoid Tiananmen teapot pose config")
    {
        Engine engine;
        engine.showWindow = false;
        const std::string entities =
            "[" + jsonEntity(1, kTeapot, jsonPoseEllipsoidOnly(vsg::dvec3{39.9087, 116.3975, 0.0}, vsg::dvec3{0.0, 0.0, 0.0})) + "]";
        const std::string cameraObject =
            std::string(R"({ "pose": { "ellipsoid": { "lla": { "lat": 39.90852, "lon": 116.3975, "alt": 3.0 }, )") +
            R"("eulerYprDeg": [0.0, -12.0, 0.0] } } })";
        EntitiesConfig cfg(entities, cameraObject, true);
        REQUIRE(engine.loadConfig(cfg.cfgFile->path()));
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
