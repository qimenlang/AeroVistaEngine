#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "InitialCameraConfig.h"
#include "engine.h"

#include <vsgXchange/all.h>

#include "CigiBaseEntityCtrl.h"
#include "CigiBaseEntityPositionCtrl.h"
#include "CigiEntityCtrlV4.h"
#include "CigiEntityPositionCtrlV4.h"
#include "Testutil.h"
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
        auto mt = requireEntity(engine, id).transform();
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

// =============================================================================
// 局部实体的采样位姿等于配置里的位置和姿态角（位姿配置设计.md §7，码 POSE-local-entity-sampled）。
// 该配置不注入椭球。
// =============================================================================
SCENARIO("local entity pose from config matches sampled engine pose",
         "[acceptance][config][pose][local][entity][POSE-local-entity-sampled]")
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
            requireEntity(engine, 1).samplePose(position, eulerYprDeg);
            THEN("sampled pose matches the configured local position and YPR")
            {
                requireDVec3Near(position, kPos);
                requireDVec3Near(eulerYprDeg, kYpr);
            }
        }
    }
}

// =============================================================================
// 椭球实体采样到的是椭球那一半位姿（位姿配置设计.md §7，码 POSE-ellipsoid-not-local）。
// 两半故意写成不同的数，采样结果对不上局部那一半。
// =============================================================================
SCENARIO("ellipsoid entity pose matches EllipsoidPose and not LocalPose",
         "[acceptance][config][pose][ellipsoid][entity][POSE-ellipsoid-not-local]")
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
            requireEntity(engine, 1).samplePose(llaOrPos, ypr);
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

// =============================================================================
// 加载后的实体挂在 MatrixTransform 下（位姿配置设计.md §7，码 POSE-parented-matrix）。
// =============================================================================
SCENARIO("loaded entity is parented under a MatrixTransform",
         "[acceptance][config][pose][entity][POSE-parented-matrix]")
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
                REQUIRE(requireEntity(engine, 1).transform());
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Multi-entity runtime (acceptance)
// -----------------------------------------------------------------------------

// =============================================================================
// 多条实体都登记在 id 表里，并挂进场景（实体管理设计.md §10，码 ENT-02-preload）。
// 未知 id 查不到。挂上的实例数等于目录条数。
// =============================================================================
SCENARIO("multiple entities: catalog entries are registered and hung on the scene",
         "[acceptance][config][pose][entities][id][ENT-02-preload]")
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
                REQUIRE(engine.findEntity(1));
                REQUIRE(engine.findEntity(2));
                REQUIRE_FALSE(engine.findEntity(99));
            }
            THEN("hung scene instances equal the catalog size and each transform is on the tree")
            {
                REQUIRE(hungEntityInstanceCount(engine.mainScene()) == engine.entitySize());
                REQUIRE(sceneHangsEntityTransform(engine.mainScene(), requireEntity(engine, 1).transform()));
                REQUIRE(sceneHangsEntityTransform(engine.mainScene(), requireEntity(engine, 2).transform()));
            }
        }
    }
}

// =============================================================================
// 多条局部实体的矩阵与各自配置的位姿一致（位姿配置设计.md §7，码 POSE-multi-local-matrix）。
// 矩阵是平移乘 Rz·Rx·Ry。
// =============================================================================
SCENARIO("multiple entities: local pose writes MatrixTransform matching config",
         "[acceptance][config][pose][entities][local][transform][POSE-multi-local-matrix]")
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

// =============================================================================
// 多条椭球实体的矩阵来自 LLA 的地心变换，不是局部那一半（位姿配置设计.md §7，码 POSE-multi-ellipsoid-ecef）。
// 平移列对不上局部坐标 100。
// =============================================================================
SCENARIO("multiple entities: ellipsoid pose writes MatrixTransform matching ECEF config",
         "[acceptance][config][pose][entities][ellipsoid][transform][POSE-multi-ellipsoid-ecef]")
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
            auto mtA = requireEntity(engine, 1).transform();
            auto mtB = requireEntity(engine, 2).transform();
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
// Entity name / optional pose / single-entity map / sample-by-id
// -----------------------------------------------------------------------------

// =============================================================================
// 省略 name 时用模型文件名，写了 name 则保留（实体管理设计.md §10，码 CFG-entity-name-basename）。
// =============================================================================
SCENARIO("entity name defaults to model basename and explicit name is kept",
         "[acceptance][config][pose][entities][name][CFG-entity-name-basename]")
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
            THEN("omitted name is basename(model); explicit name is preserved")
            {
                REQUIRE(requireEntity(engine, 1).name() == "lz.vsgt");
                REQUIRE(requireEntity(engine, 2).name() == "tower");
            }
        }
    }
}

// =============================================================================
// 没有 pose 的实体仍有矩阵，采样位姿在原点（实体管理设计.md §10，码 ENT-02-pose-default）。
// =============================================================================
SCENARIO("entity without pose uses an origin MatrixTransform",
         "[acceptance][config][pose][entities][default-place][ENT-02-pose-default]")
{
    GIVEN("a Local entity with id/model but no pose")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.entitySize() == 1);
        REQUIRE(engine.findEntity(1));

        WHEN("transform and pose are queried by id")
        {
            vsg::dvec3 position{};
            vsg::dvec3 ypr{};
            THEN("a MatrixTransform exists and sampled pose is the origin")
            {
                REQUIRE(requireEntity(engine, 1).transform());
                requireEntity(engine, 1).samplePose(position, ypr);
                requireDVec3Near(position, vsg::dvec3{0.0, 0.0, 0.0});
                requireDVec3Near(ypr, vsg::dvec3{0.0, 0.0, 0.0});
            }
        }
    }
}

// =============================================================================
// 目录里只有一条实体时，仍按 id 登记，矩阵与局部 pose 一致（实体管理设计.md §10，码 ENT-02-preload）。
// 与多实体那条共用预加载码。
// =============================================================================
SCENARIO("single entities entry still registers in the id map",
         "[acceptance][config][pose][entities][id][ENT-02-preload]")
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
                REQUIRE(engine.findEntity(1));
                requireEntityLocalTransform(engine, 1, kPos, kYpr);
            }
        }
    }
}

// =============================================================================
// 按 id 采样的局部位姿与矩阵、配置三者一致（位姿配置设计.md §7，码 POSE-sampled-local-matrix）。
// =============================================================================
SCENARIO("sampled entity pose matches MatrixTransform for local pose",
         "[acceptance][config][pose][entities][local][sample][POSE-sampled-local-matrix]")
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
            requireEntity(engine, 1).samplePose(position, ypr);
            THEN("sample matches config and is consistent with the transform matrix")
            {
                requireDVec3Near(position, kPos);
                requireDVec3Near(ypr, kYpr);
                requireEntityLocalTransform(engine, 1, position, ypr);
            }
        }
    }
}

// =============================================================================
// 两条实体可以同名，查找只认 id（实体管理设计.md §10，码 CFG-dup-names-id-lookup）。
// 两个名字都是 twin。未知 id 查不到。
// =============================================================================
SCENARIO("duplicate entity names are allowed; lookup is by id only",
         "[acceptance][config][pose][entities][name][id][CFG-dup-names-id-lookup]")
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
            REQUIRE(engine.entitySize() == 2);
            REQUIRE(engine.findEntity(1));
            REQUIRE(engine.findEntity(2));
            REQUIRE_FALSE(engine.findEntity(99));
            THEN("both keep name twin; name is not a map key")
            {
                REQUIRE(requireEntity(engine, 1).name() == "twin");
                REQUIRE(requireEntity(engine, 2).name() == "twin");
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 启动装配（实体管理设计.md §8 / §10 ②；验收码 ENT-02-*）
// -----------------------------------------------------------------------------

// =============================================================================
// 启动时 Standby 实体在表里，但是不可见（实体管理设计.md §10，码 ENT-02-visibility）。
// 没有 Host。
// =============================================================================
SCENARIO("Standby entity is hidden at startup without a Host",
         "[acceptance][config][entities][visibility][ENT-02-visibility]")
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
                REQUIRE(engine.findEntity(1));
                REQUIRE_FALSE(requireEntity(engine, 1).visible());
            }
        }
    }
}

// =============================================================================
// 省略初始状态时，启动后实体可见（实体管理设计.md §10，码 ENT-02-visibility）。
// 缺省是 Active。没有 Host。
// =============================================================================
SCENARIO("Active entity is visible at startup without a Host",
         "[acceptance][config][entities][visibility][ENT-02-visibility]")
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
                REQUIRE(engine.findEntity(1));
                REQUIRE(requireEntity(engine, 1).visible());
            }
        }
    }
}

// =============================================================================
// 同一模型的两条实体共用几何节点，变换节点各自一份（实体管理设计.md §10，码 ENT-02-shared-geom）。
// =============================================================================
SCENARIO("two entities with the same model share one geometry node",
         "[acceptance][config][entities][shared-geometry][ENT-02-shared-geom]")
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
                auto nodeA = requireEntity(engine, 1).node();
                auto nodeB = requireEntity(engine, 2).node();
                REQUIRE(nodeA);
                REQUIRE(nodeB);
                REQUIRE(nodeA == nodeB);
                REQUIRE(requireEntity(engine, 1).transform());
                REQUIRE(requireEntity(engine, 2).transform());
                REQUIRE(requireEntity(engine, 1).transform() != requireEntity(engine, 2).transform());
            }
        }
    }
}

// =============================================================================
// 隐藏的 Standby 兄弟不进入场景包围盒（实体管理设计.md §10，码 ENT-02-aabb-hidden）。
// 可见实体在原点，隐藏实体在 +X 远处。包围盒中心的 x 小于 20。
// =============================================================================
SCENARIO("hidden Standby sibling is excluded from the scene AABB",
         "[acceptance][config][entities][aabb][ENT-02-aabb-hidden]")
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

// =============================================================================
// 一条模型加载失败时，init 仍成功，该 id 不进目录（实体管理设计.md §10，码 ENT-02-load-skip）。
// 有效的那条仍在，目录大小为 1。
// =============================================================================
SCENARIO("init continues when one entity model fails to load",
         "[acceptance][config][entities][negative][ENT-02-load-skip]")
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
                REQUIRE(engine.findEntity(1));
                REQUIRE_FALSE(engine.findEntity(2));
                REQUIRE(engine.entitySize() == 1);
            }
        }
    }
}

// =============================================================================
// 实体全是 Standby 时，局部相机用空包围盒回退（实体管理设计.md §10，码 ENT-02-aabb-hidden）。
// 眼点在 (0,0,10)，注视原点。与隐藏兄弟那条共用码。
// =============================================================================
SCENARIO("all Standby entities use the local camera fallback",
         "[acceptance][config][entities][aabb][camera][ENT-02-aabb-hidden]")
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
// 运行期 EntityCtrl（实体管理设计.md §9 / §10 ③；验收码 ENT-03-*）
// 测试直接调公开 onEntityCtrl / onEntityPose（与既有位姿注入同层），不改 ENT-02。
// -----------------------------------------------------------------------------

// =============================================================================
// EntityCtrl Active 让隐藏的 Standby 实体显示（实体管理设计.md §10，码 ENT-03-active）。
// 直接投递，不走网络。没有位置包也能显示。
// =============================================================================
SCENARIO("Host EntityCtrl Active shows a hidden Standby entity",
         "[acceptance][entities][entity-ctrl][ENT-03-active]")
{
    GIVEN("a Local Standby teapot with no pose packet yet")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, {}, {}, "Standby") + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE_FALSE(requireEntity(engine, 1).visible());

        WHEN("Host sends EntityCtrl Active")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("the entity is visible without a position packet")
            {
                REQUIRE(engine.findEntity(1));
                REQUIRE(requireEntity(engine, 1).visible());
            }
        }
    }
}

// =============================================================================
// EntityCtrl Standby 隐藏实体，实例仍留在目录里（实体管理设计.md §10，码 ENT-03-standby）。
// 几何节点还是原来的那个。目录大小不变。
// =============================================================================
SCENARIO("Host EntityCtrl Standby hides a visible entity and keeps the instance",
         "[acceptance][entities][entity-ctrl][ENT-03-standby]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(requireEntity(engine, 1).visible());
        const auto geometry = requireEntity(engine, 1).node();

        WHEN("Host sends EntityCtrl Standby")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Standby));

            THEN("the entity is hidden and still registered")
            {
                REQUIRE(engine.findEntity(1));
                REQUIRE_FALSE(requireEntity(engine, 1).visible());
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(requireEntity(engine, 1).node() == geometry);
            }
        }
    }
}

// =============================================================================
// EntityCtrl Destroyed 先隐藏，随后 Active 可以再显示（实体管理设计.md §10，码 ENT-03-destroyed）。
// 隐藏期间实例还在。
// =============================================================================
SCENARIO("Host EntityCtrl Destroyed hides like Standby and Active can show again",
         "[acceptance][entities][entity-ctrl][ENT-03-destroyed]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(requireEntity(engine, 1).visible());

        WHEN("Host sends EntityCtrl Destroyed")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Destroyed));

            THEN("the entity is hidden and the instance is kept")
            {
                REQUIRE(engine.findEntity(1));
                REQUIRE_FALSE(requireEntity(engine, 1).visible());
                REQUIRE(engine.entitySize() == 1);
            }

            AND_WHEN("Host sends EntityCtrl Active")
            {
                engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

                THEN("the entity is visible again")
                {
                    REQUIRE(engine.findEntity(1));
                    REQUIRE(requireEntity(engine, 1).visible());
                }
            }
        }
    }
}

// =============================================================================
// EntityCtrl Remove 与 Destroyed 一样只隐藏，不删实例（实体管理设计.md §10，码 ENT-03-destroyed）。
// =============================================================================
SCENARIO("Host EntityCtrl Remove hides like Destroyed",
         "[acceptance][entities][entity-ctrl][ENT-03-destroyed]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(requireEntity(engine, 1).visible());

        WHEN("Host sends EntityCtrl Remove")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Remove));

            THEN("the entity is hidden and the instance is kept")
            {
                REQUIRE(engine.findEntity(1));
                REQUIRE_FALSE(requireEntity(engine, 1).visible());
                REQUIRE(engine.entitySize() == 1);
            }
        }
    }
}

// =============================================================================
// 对已经可见的实体再发 Active，不增加实例（实体管理设计.md §10，码 ENT-03-active-idempotent）。
// 几何节点不变，目录大小仍是 1。
// =============================================================================
SCENARIO("repeating EntityCtrl Active keeps a single visible instance",
         "[acceptance][entities][entity-ctrl][ENT-03-active-idempotent]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        const auto geometry = requireEntity(engine, 1).node();
        REQUIRE(requireEntity(engine, 1).visible());
        REQUIRE(engine.entitySize() == 1);

        WHEN("Host sends EntityCtrl Active again")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("visibility and catalog size are unchanged")
            {
                REQUIRE(requireEntity(engine, 1).visible());
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(requireEntity(engine, 1).node() == geometry);
            }
        }
    }
}

// =============================================================================
// 目录里没有的 id，EntityCtrl 不创建实例，已有实体不变（实体管理设计.md §10，码 ENT-03-unmapped）。
// =============================================================================
SCENARIO("EntityCtrl for an id outside the catalog changes nothing",
         "[acceptance][entities][entity-ctrl][negative][ENT-03-unmapped]")
{
    GIVEN("one Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(requireEntity(engine, 1).visible());

        WHEN("Host sends EntityCtrl for an id not in the catalog")
        {
            engine.onEntityCtrl(makeEntityCtrl(99, CigiBaseEntityCtrl::Standby));

            THEN("no instance is created and the catalog entity is unchanged")
            {
                REQUIRE_FALSE(engine.findEntity(99));
                REQUIRE(engine.findEntity(1));
                REQUIRE(requireEntity(engine, 1).visible());
                REQUIRE(engine.entitySize() == 1);
            }
        }
    }
}

// =============================================================================
// 加载时被跳过的 id，再发 EntityCtrl 仍然不出现（实体管理设计.md §10，码 ENT-03-failed-id）。
// 有效实体保持可见，目录大小仍是 1。
// =============================================================================
SCENARIO("EntityCtrl for a skipped load id has no effect",
         "[acceptance][entities][entity-ctrl][negative][ENT-03-failed-id]")
{
    GIVEN("one valid teapot and one missing model path")
    {
        const std::string entities =
            "[" + jsonEntity(1, kTeapot) + ", " + jsonEntity(2, "models/missing.vsgt") + "]";
        EntitiesConfig cfg(entities);
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(engine.findEntity(1));
        REQUIRE_FALSE(engine.findEntity(2));
        REQUIRE(requireEntity(engine, 1).visible());

        WHEN("Host sends EntityCtrl Active for the skipped id")
        {
            engine.onEntityCtrl(makeEntityCtrl(2, CigiBaseEntityCtrl::Active));

            THEN("the skipped id stays absent and the valid entity is unchanged")
            {
                REQUIRE_FALSE(engine.findEntity(2));
                REQUIRE(engine.findEntity(1));
                REQUIRE(requireEntity(engine, 1).visible());
                REQUIRE(engine.entitySize() == 1);
            }
        }
    }
}

// =============================================================================
// Standby 实体被 Active 点亮后，仍停在配置位姿（实体管理设计.md §10，码 ENT-03-standby-pose）。
// 这次没有位置包。
// =============================================================================
SCENARIO("activating a Standby entity keeps the configured pose",
         "[acceptance][entities][entity-ctrl][ENT-03-standby-pose]")
{
    GIVEN("a Local Standby teapot with a configured pose")
    {
        constexpr vsg::dvec3 kPos{4.0, 2.0, 1.0};
        constexpr vsg::dvec3 kYpr{15.0, 0.0, 0.0};
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot, jsonPoseLocalOnly(kPos, kYpr), {}, "Standby") + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE_FALSE(requireEntity(engine, 1).visible());

        WHEN("Host sends EntityCtrl Active without a position packet")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("the entity is visible at the configured pose")
            {
                REQUIRE(requireEntity(engine, 1).visible());
                vsg::dvec3 position{};
                vsg::dvec3 ypr{};
                requireEntity(engine, 1).samplePose(position, ypr);
                requireDVec3Near(position, kPos);
                requireDVec3Near(ypr, kYpr);
            }
        }
    }
}

// =============================================================================
// 藏起再显示不会把位姿恢复成配置值（实体管理设计.md §10，码 ENT-03-pose-keep）。
// 先摆到新的 LLA，再隐藏、再显示，采样仍是摆放后的值。
// =============================================================================
SCENARIO("hiding and showing does not restore the configured pose",
         "[acceptance][entities][entity-ctrl][ENT-03-pose-keep]")
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
        REQUIRE(requireEntity(engine, 1).visible());

        WHEN("Host places a new pose then hides and shows the entity")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));
            engine.onEntityPose(makeEntityPositionLla(1, kPlacedLla, kPlacedYpr));
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Standby));
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("the sampled pose stays at the placed LLA")
            {
                REQUIRE(requireEntity(engine, 1).visible());
                vsg::dvec3 lla{};
                vsg::dvec3 ypr{};
                requireEntity(engine, 1).samplePose(lla, ypr);
                requireDVec3Near(lla, kPlacedLla, 1e-4);
                requireDVec3Near(ypr, kPlacedYpr, 1e-4);
            }
        }
    }
}

// =============================================================================
// 藏起再显示复用原来的几何节点（实体管理设计.md §10，码 ENT-03-no-reload）。
// 目录大小不变。
// =============================================================================
SCENARIO("hiding and showing reuses the prebuilt geometry",
         "[acceptance][entities][entity-ctrl][ENT-03-no-reload]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        const auto geometry = requireEntity(engine, 1).node();
        REQUIRE(geometry);

        WHEN("Host hides then shows the entity")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Standby));
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active));

            THEN("the entity is visible on the same geometry node")
            {
                REQUIRE(requireEntity(engine, 1).visible());
                REQUIRE(engine.entitySize() == 1);
                REQUIRE(requireEntity(engine, 1).node() == geometry);
            }
        }
    }
}

// =============================================================================
// EntityPositionCtrl 把已激活实体摆到下发的 LLA（实体管理设计.md §10，码 ENT-03-place）。
// 先 Active 再摆放。场景是椭球。
// =============================================================================
SCENARIO("Host EntityPositionCtrl places an Active entity",
         "[acceptance][entities][entity-ctrl][ENT-03-place]")
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
                REQUIRE(requireEntity(engine, 1).visible());
                vsg::dvec3 lla{};
                vsg::dvec3 ypr{};
                requireEntity(engine, 1).samplePose(lla, ypr);
                requireDVec3Near(lla, kPlacedLla, 1e-4);
                requireDVec3Near(ypr, kPlacedYpr, 1e-4);
            }
        }
    }
}

// =============================================================================
// 实体已经 Active 时，EntityCtrl 的 Alpha 生效且保持可见（实体管理设计.md §10，码 ENT-03-attrs）。
// 透明度从 255 变成 128。
// =============================================================================
SCENARIO("EntityCtrl Alpha applies while the entity is already Active",
         "[acceptance][entities][entity-ctrl][ENT-03-attrs]")
{
    GIVEN("a Local Active teapot")
    {
        EntitiesConfig cfg("[" + jsonEntity(1, kTeapot) + "]");
        Engine engine;
        initOffscreen(engine, cfg.cfgFile->path());
        REQUIRE(requireEntity(engine, 1).visible());
        REQUIRE(requireEntity(engine, 1).alpha() == std::uint8_t{255});

        WHEN("Host sends EntityCtrl Active with Alpha 128")
        {
            engine.onEntityCtrl(makeEntityCtrl(1, CigiBaseEntityCtrl::Active, 128));

            THEN("the entity stays visible and alpha is 128")
            {
                REQUIRE(requireEntity(engine, 1).visible());
                REQUIRE(requireEntity(engine, 1).alpha() == std::uint8_t{128});
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Camera runtime (acceptance)
// -----------------------------------------------------------------------------

// =============================================================================
// 局部相机 pose 写进 LookAt（位姿配置设计.md §7，码 CAM-local-config-lookat）。
// =============================================================================
SCENARIO("local camera pose from config matches LookAt",
         "[acceptance][config][pose][local][camera][CAM-local-config-lookat]")
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

// =============================================================================
// 椭球相机用椭球那一半 pose，不用局部那一半（位姿配置设计.md §7，码 CAM-ellipsoid-config-lookat）。
// 两半故意不同。投影是 EllipsoidPerspective。
// =============================================================================
SCENARIO("ellipsoid camera pose matches EllipsoidPose not LocalPose",
         "[acceptance][config][pose][ellipsoid][camera][CAM-ellipsoid-config-lookat]")
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

// =============================================================================
// 没有 camera 配置时，局部默认 LookAt 框住实体包围盒（位姿配置设计.md §7，码 CAM-local-default-aabb）。
// 配置里没有 camera 键。
// =============================================================================
SCENARIO("no camera config: Local default LookAt frames entities AABB",
         "[acceptance][config][pose][local][camera][aabb-default][CAM-local-default-aabb]")
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

// =============================================================================
// 没有 camera 配置时，椭球默认 LookAt 按包围盒取景（位姿配置设计.md §7，码 CAM-ellipsoid-default-aabb）。
// 模型摆在远离北京的位置，避免回退点偶然通过。
// =============================================================================
SCENARIO("no camera config: Ellipsoid default LookAt frames entities AABB",
         "[acceptance][config][pose][ellipsoid][camera][aabb-default][CAM-ellipsoid-default-aabb]")
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

// =============================================================================
// 一份局部场景配置能加载出一条实体和一台相机（位姿配置设计.md §7，码 CFG-load-scene-local）。
// 内嵌快照。没有椭球。实体矩阵和 LookAt 都对上配置。
// =============================================================================
SCENARIO("system loads scene_local config with one local entity and camera",
         "[system][config][pose][local][resource][CFG-load-scene-local]")
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
                REQUIRE(engine.findEntity(1));
            }
            THEN("entity 1 pose and MatrixTransform match the config local placement")
            {
                vsg::dvec3 position{};
                vsg::dvec3 ypr{};
                requireEntity(engine, 1).samplePose(position, ypr);
                requireDVec3Near(position, vsg::dvec3{10.0, 0.0, -2.0});
                requireDVec3Near(ypr, vsg::dvec3{90.0, 0.0, 0.0});
                REQUIRE(requireEntity(engine, 1).transform());
            }
            THEN("main camera LookAt matches the config local camera pose")
            {
                REQUIRE(engine.mainCamera());
                requireLookAtMatchesLocalPose(engine, vsg::dvec3{0.0, 0.0, 0.0}, vsg::dvec3{-90.0, 0.0, 0.0});
            }
        }
    }
}

// =============================================================================
// 一份地心场景配置能加载出天安门处的实体和南侧相机（位姿配置设计.md §7，码 CFG-load-scene-ecef）。
// 有椭球。相机投影是 EllipsoidPerspective。
// =============================================================================
SCENARIO("system loads scene_ecef config with one ECEF entity and camera",
         "[system][config][pose][ellipsoid][resource][CFG-load-scene-ecef]")
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
                REQUIRE(engine.findEntity(1));
            }
            THEN("entity 1 pose and MatrixTransform match the Tiananmen ground placement")
            {
                vsg::dvec3 lla{};
                vsg::dvec3 ypr{};
                requireEntity(engine, 1).samplePose(lla, ypr);
                requireDVec3Near(lla, vsg::dvec3{39.9087, 116.3975, 0.0}, 1e-4);
                requireDVec3Near(ypr, vsg::dvec3{0.0, 0.0, 0.0}, 1e-4);
                REQUIRE(requireEntity(engine, 1).transform());
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
