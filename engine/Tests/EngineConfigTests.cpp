#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "InitialCameraConfig.h"
#include "engine.h"
#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/IgSync.h>
#include <aerovista/sync/SyncConfig.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "Testutil.h"

using aerovista::sync::HostSync;
using aerovista::sync::OffsetDeg;

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

// 以下测试编码 doc/design/多通道同步/多通道同步模块设计.md §3.1 契约，以及
// doc/design/多通道同步/lla位姿传输设计.md（EllipsoidModel 装配）。
// 断言经 init 时可观测结果（角色、场景/相机语义、加载成功/失败）。

namespace
{
    const char* kDefaultJson = R"({"model":"models/lz.vsgt","viewDefinition":{"vfov":30.0,"near":1.0,"far":1000.0,"window":{"posX":0,"posY":0,"width":1920,"height":1080}}})";
    // engine 配置不含 hostConfig（含它即未知键拒绝）；kMainJson 对应纯 IG 通道配置。
    const char* kMainJson =
        R"({"viewDefinition":{"viewID":0,"offsetDeg":{"heading":0.0,"pitch":0.0,"roll":0.0},"requireConnectedIg":true,"vfov":30.0,"near":1.0,"far":1000.0,"window":{"posX":640,"posY":0,"width":640,"height":1080}},"injectEllipsoidIfMissing":true,"igConfig":{"udpRecvPort":8001,"hostAddr":"127.0.0.1","hostTcpPort":8100,"hostUdpRecvPort":8000},"model":"models/lz.vsgt"})";
    const char* kLeftJson =
        R"({"viewDefinition":{"viewID":1,"offsetDeg":{"heading":-18.05,"pitch":0.0,"roll":0.0},"requireConnectedIg":false,"vfov":30.0,"near":1.0,"far":1000.0,"window":{"posX":0,"posY":0,"width":640,"height":1080}},"injectEllipsoidIfMissing":true,"igConfig":{"udpRecvPort":8003,"hostAddr":"127.0.0.1","hostTcpPort":8100,"hostUdpRecvPort":8000},"model":"models/lz.vsgt"})";

    bool nearlyEqual(double a, double b, double eps = 1e-9)
    {
        return std::abs(a - b) <= eps;
    }

    // Role-relevant field compares.
    bool igConfigEquals(const IgConfig& a, const IgConfig& b)
    {
        return a.udpRecvPort == b.udpRecvPort && a.hostAddr == b.hostAddr &&
               a.hostTcpPort == b.hostTcpPort && a.hostUdpRecvPort == b.hostUdpRecvPort;
    }

    bool offsetEquals(const OffsetDeg& a, const OffsetDeg& b)
    {
        return nearlyEqual(a.yaw, b.yaw) && nearlyEqual(a.pitch, b.pitch) && nearlyEqual(a.roll, b.roll);
    }

    void requireConfigEquals(const EngineChannelConfig& actual, const EngineChannelConfig& expected)
    {
        REQUIRE(actual.viewDefinition.viewID == expected.viewDefinition.viewID);
        REQUIRE(offsetEquals(actual.viewDefinition.offsetDeg, expected.viewDefinition.offsetDeg));
        REQUIRE(actual.igConfig.has_value() == expected.igConfig.has_value());
        if (actual.igConfig)
            REQUIRE(igConfigEquals(*actual.igConfig, *expected.igConfig));
        REQUIRE(actual.model == expected.model);
        REQUIRE(actual.viewDefinition.window.posX == expected.viewDefinition.window.posX);
        REQUIRE(actual.viewDefinition.window.posY == expected.viewDefinition.window.posY);
        REQUIRE(actual.viewDefinition.window.width == expected.viewDefinition.window.width);
        REQUIRE(actual.viewDefinition.window.height == expected.viewDefinition.window.height);
    }

    const char* kMinimalWindow =
        R"("viewDefinition": { "vfov": 30.0, "near": 1.0, "far": 1000.0, "window": { "posX": 0, "posY": 0, "width": 640, "height": 480 } })";
    const char* kMinimalModel = R"("model": "models/lz.vsgt")";
    const char* kReadymapModel = R"("model": "models/readymap.vsgt")";

    std::string jsonIgConfig(int udpRecv = 8003)
    {
        return std::string(
                   R"("igConfig": { "udpRecvPort": )") +
               std::to_string(udpRecv) +
               R"(, "hostAddr": "127.0.0.1", "hostTcpPort": 8100, "hostUdpRecvPort": 8000 })";
    }
} // namespace

// =============================================================================
// 验收：injectEllipsoidIfMissing → 场景 EllipsoidModel 注入（lla位姿传输设计.md §2）。
// 运行时坐标系由「场景有无 EllipsoidModel」决定；此开关只在无椭球时决定是否注入。
// =============================================================================

SCENARIO("injectEllipsoidIfMissing injects EllipsoidModel on scene; otherwise lz has none",
         "[acceptance][config][ellipsoid-inject][CFG-inject-ellipsoid-on-scene]")
{
    GIVEN("a channel config that uses lz.vsgt (model has no built-in EllipsoidModel)")
    {
        WHEN("injectEllipsoidIfMissing is true")
        {
            const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, )") + kMinimalModel + ", " +
                                      kMinimalWindow + "}");
            Engine engine;
            REQUIRE(engine.loadConfig(file.path()));
            engine.showWindow = false;
            REQUIRE(engine.init());

            THEN("the scene carries an EllipsoidModel after init")
            {
                REQUIRE(engine.ellipsoidModel());
            }
        }

        WHEN("injectEllipsoidIfMissing is omitted (defaults to false)")
        {
            const TempConfigFile file(std::string("{") + kMinimalModel + ", " + kMinimalWindow + "}");
            Engine engine;
            REQUIRE(engine.loadConfig(file.path()));
            engine.showWindow = false;
            REQUIRE(engine.init());

            THEN("the scene has no EllipsoidModel after init")
            {
                REQUIRE_FALSE(engine.ellipsoidModel());
            }
        }
    }
}

// =============================================================================
// 模型自带椭球时，注入开关省略或显式打开都不覆盖它（位姿配置设计.md §7，码 CFG-keep-builtin-ellipsoid）。
// 两条路径都要求 init 成功，并且场景里仍有 EllipsoidModel。
// =============================================================================
SCENARIO("model-built-in EllipsoidModel is kept regardless of injectEllipsoidIfMissing",
         "[acceptance][config][ellipsoid-inject][CFG-keep-builtin-ellipsoid]")
{
    GIVEN("a channel config that uses readymap.vsgt (model already has EllipsoidModel)")
    {
        WHEN("injectEllipsoidIfMissing is omitted (defaults to false)")
        {
            const TempConfigFile file(std::string("{") + kReadymapModel + ", " + kMinimalWindow + "}");
            Engine engine;
            REQUIRE(engine.loadConfig(file.path()));
            engine.showWindow = false;

            THEN("init succeeds and the scene keeps the built-in EllipsoidModel")
            {
                REQUIRE(engine.init());
                REQUIRE(engine.ellipsoidModel());
            }
        }

        WHEN("injectEllipsoidIfMissing is true")
        {
            const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, )") + kReadymapModel + ", " +
                                      kMinimalWindow + "}");
            Engine engine;
            REQUIRE(engine.loadConfig(file.path()));
            engine.showWindow = false;

            THEN("init succeeds and the built-in EllipsoidModel is kept (no re-inject)")
            {
                REQUIRE(engine.init());
                REQUIRE(engine.ellipsoidModel());
            }
        }
    }
}

// 位姿配置设计.md §4：Ellipsoid 下默认初始相机由 AABB 计算，不写死北京。
// 覆盖「注入在相机创建前」：lz 无自带椭球，靠 injectEllipsoidIfMissing 注入后才能建 EllipsoidPerspective。
// 无 pose 实体默认摆在地心，会触发 fallback 到北京上空（见 §4.2 fallback 判据）。

// =============================================================================
// 无自带椭球的模型靠注入才能建椭球相机，眼点用回退 LLA（位姿配置设计.md §7，码 CFG-inject-before-camera）。
// 无 pose 的实体在地心，触发回退。眼点是 (39.9, 116.4, 500) 的 ECEF，朝向为正北。
// =============================================================================
SCENARIO("injectEllipsoidIfMissing injects before camera create with fallback LLA LookAt",
         "[acceptance][config][ellipsoid][initial-lla][CFG-inject-before-camera]")
{
    // NOTE: 当前实现仍写死北京 500m，尚未实现 AABB 计算。本测试先写定新设计目标，
    //       待实现后移除 !hide 标签并验证 AABB 关系。
    GIVEN("lz.vsgt with injectEllipsoidIfMissing (WGS-84 EllipsoidModel)")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, )") + kMinimalModel + ", " +
                                  kMinimalWindow + "}");
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE(engine.init());
        REQUIRE(engine.ellipsoidModel());

        WHEN("the main camera LookAt is inspected before any tick")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);
            auto ep = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
            REQUIRE(ep);
            REQUIRE(ep->ellipsoidModel);

            THEN("eye matches ECEF of fallback LLA (39.9, 116.4, 500) with YPR=0 (north, level) "
                 "because entity has no pose and sits at origin, triggering fallback")
            {
                // 无 pose 实体默认摆在地心，触发 fallback 判据 3（|centre| < radiusPolar * 0.9）
                const vsg::dvec3 fallbackLla{39.9, 116.4, 500.0};
                const vsg::dvec3 expectedEye =
                    ep->ellipsoidModel->convertLatLongAltitudeToECEF(fallbackLla);
                REQUIRE(vsg::length(lookAt->eye - expectedEye) < 1e-3);

                const vsg::dmat4 localToWorld =
                    ep->ellipsoidModel->computeLocalToWorldTransform(fallbackLla);
                // YPR=0 → ENU forward = North = LocalToWorld 的第 1 列。
                const vsg::dvec3 expectedForward =
                    vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
                const vsg::dvec3 actualForward = vsg::normalize(lookAt->center - lookAt->eye);
                REQUIRE(vsg::length(actualForward - expectedForward) < 1e-6);
            }
        }
    }
}

// =============================================================================
// 全球模型的默认眼点因包围盒中心靠近地心而回退到北京（位姿配置设计.md §7，码 CFG-builtin-ellipsoid-aabb）。
// 眼点对准北京回退点，并确认中心模长小于 0.9 倍极半径。
// =============================================================================
SCENARIO("model with built-in ellipsoid initializes camera from AABB, not hardcoded Beijing",
         "[acceptance][config][ellipsoid][initial-lla][CFG-builtin-ellipsoid-aabb]")
{
    // NOTE: 当前实现仍写死北京 500m，尚未实现 AABB 计算。本测试先写定新设计目标，
    //       待实现后移除 !hide 标签并验证 AABB 关系。
    GIVEN("an Engine initialized with readymap (built-in EllipsoidModel, no inject)")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, )") + kReadymapModel + ", " +
                                  kMinimalWindow + "}");
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE(engine.init());
        REQUIRE(engine.ellipsoidModel());

        WHEN("the main camera LookAt is inspected before any tick")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);
            auto ep = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
            REQUIRE(ep);
            REQUIRE(ep->ellipsoidModel);

            THEN("eye is positioned back from AABB centre by k_back=3.5*radius along north, "
                 "or falls back to Beijing if centre is near Earth core")
            {
                // readymap 覆盖全球，AABB centre ≈ 地心，应触发 fallback 到北京
                const vsg::dvec3 fallbackLla{39.9, 116.4, 500.0};
                const vsg::dvec3 expectedEye =
                    ep->ellipsoidModel->convertLatLongAltitudeToECEF(fallbackLla);
                REQUIRE(vsg::length(lookAt->eye - expectedEye) < 1e-3);

                // 验证确实是 fallback：centre 靠近地心，触发 |centre| < 0.9·radiusPolar 判据
                vsg::ComputeBounds computeBounds;
                engine.mainScene()->accept(computeBounds);
                vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
                double centreMag = vsg::length(centre);
                REQUIRE(centreMag < ep->ellipsoidModel->radiusPolar() * 0.9);
            }
        }
    }
}

// =============================================================================
// 验收：默认初始相机按 AABB 计算（位姿配置设计.md §4）
// 以下测试用例按新设计目标编写，当前实现未达标，标记
// =============================================================================

// =============================================================================
// 单个无 pose 的局部实体，默认眼点在包围盒中心沿 -Y 退 3.5 倍半径（位姿配置设计.md §7，码 CFG-local-single-aabb）。
// 注视点是包围盒中心，up 为 (0,0,1)。场景没有椭球。
// =============================================================================
SCENARIO("Local single entity no pose camera uses AABB with k_back=3.5",
         "[acceptance][config][initial-camera][aabb][CFG-local-single-aabb]")
{
    // A1: Local：单实体无 pose，无 camera → eye = AABB centre - Y方向 3.5·radius
    GIVEN("lz.vsgt in local cartesian without pose")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ )") + kMinimalModel + ", " +
                                  kMinimalWindow + "}");
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE(engine.init());
        REQUIRE_FALSE(engine.ellipsoidModel());

        WHEN("the main camera is inspected")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);
            auto perspective = engine.mainCamera()->projectionMatrix.cast<vsg::Perspective>();
            REQUIRE(perspective);

            THEN("eye is positioned at centre - Y*3.5*radius with up=(0,0,1)")
            {
                // 假设 Engine 暴露 mainScene() 访问（红测：先写测试，后实现接口）
                vsg::ComputeBounds computeBounds;
                engine.mainScene()->accept(computeBounds);
                vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
                double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;
                vsg::dvec3 expectedEye = centre + vsg::dvec3(0.0, -3.5 * radius, 0.0);
                REQUIRE(vsg::length(lookAt->eye - expectedEye) < 1e-6);
                REQUIRE(vsg::length(lookAt->center - centre) < 1e-6);
                REQUIRE(vsg::length(lookAt->up - vsg::dvec3(0.0, 0.0, 1.0)) < 1e-6);
            }
        }
    }
}

// =============================================================================
// 多个无 pose 的局部实体，默认相机按整体包围盒取景（位姿配置设计.md §7，码 CFG-local-multi-aabb）。
// 中心落在两实体之间，眼点从中心沿 -Y 退 3.5 倍半径。
// =============================================================================
SCENARIO("Local multiple entities no pose camera uses overall AABB",
         "[acceptance][config][initial-camera][aabb][CFG-local-multi-aabb]")
{
    // A2: Local：多实体分散，无 camera → eye 按整体 AABB
    GIVEN("two lz.vsgt entities at different positions without camera pose")
    {
        Engine engine;
        engine.showWindow = false;
        EntitiesConfig cfg(R"([
                { "id": 1, "model": "models/lz.vsgt", "pose": { "local": { "position": [0, 0, 0], "eulerYprDeg": [0, 0, 0] } } },
                { "id": 2, "model": "models/lz.vsgt", "pose": { "local": { "position": [100, 0, 0], "eulerYprDeg": [0, 0, 0] } } }
            ])");
        REQUIRE(engine.loadConfig(cfg.cfgFile->path()));
        REQUIRE(engine.init());

        WHEN("the main camera is inspected")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);

            THEN("eye positions to enclose both entities in AABB")
            {
                vsg::ComputeBounds computeBounds;
                engine.mainScene()->accept(computeBounds);
                vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
                double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;

                // centre 应在两实体中间 (50, 0, 0) 附近
                REQUIRE((centre.x > 40.0 && centre.x < 60.0));
                // eye 应在 centre - Y * 3.5 * radius
                vsg::dvec3 expectedEye = centre + vsg::dvec3(0.0, -3.5 * radius, 0.0);
                REQUIRE(vsg::length(lookAt->eye - expectedEye) < 1e-6);
            }
        }
    }
}

// =============================================================================
// 椭球上带 LLA 的小模型，默认眼点可以在天安门附近，也可以回退北京（位姿配置设计.md §7，码 CFG-ellipsoid-lla-aabb）。
// 两种都算通过。小模型的包围盒中心仍可能靠近地心。
// =============================================================================
SCENARIO("Ellipsoid entity with LLA pose camera uses AABB or fallback",
         "[acceptance][config][initial-camera][aabb][CFG-ellipsoid-lla-aabb]")
{
    // A3: Ellipsoid：单实体有 LLA pose，无 camera → eye 按整体 AABB
    // 注意：小模型（lz.vsgt ~1.4km）即使摆放在天安门，AABB centre 的 ECEF 坐标
    // 仍可能很小，触发 fallback（|centre| < 0.9·radiusPolar）。这是合理的。
    GIVEN("lz.vsgt at Tiananmen LLA without camera pose")
    {
        Engine engine;
        engine.showWindow = false;
        EntitiesConfig cfg(R"([{
                    "id": 1,
                    "model": "models/lz.vsgt",
                    "pose": {
                        "ellipsoid": {
                            "lla": { "lat": 39.9087, "lon": 116.3975, "alt": 0.0 },
                            "eulerYprDeg": [0, 0, 0]
                        }
                    }
                }])",
                           {}, true);
        REQUIRE(engine.loadConfig(cfg.cfgFile->path()));
        REQUIRE(engine.init());
        REQUIRE(engine.ellipsoidModel());

        WHEN("the main camera is inspected")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);
            auto ep = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
            REQUIRE(ep);

            THEN("eye is positioned either by AABB or falls back to Beijing for small models")
            {
                vsg::dvec3 eyeLla = ep->ellipsoidModel->convertECEFToLatLongAltitude(lookAt->eye);
                // 小模型可能触发 fallback，检查是否在天安门附近或 fallback 北京
                const bool nearTiananmen = fabs(eyeLla.x - 39.9087) < 0.5 && fabs(eyeLla.y - 116.3975) < 0.5;
                const bool isFallback = fabs(eyeLla.x - 39.9) < 0.01 && fabs(eyeLla.y - 116.4) < 0.01;
                REQUIRE((nearTiananmen || isFallback));
            }
        }
    }
}

// =============================================================================
// 椭球上无 pose 的实体停在地心，默认眼点回退到北京正北（位姿配置设计.md §7，码 CFG-ellipsoid-beijing-fallback）。
// 眼点是 (39.9, 116.4, 500) 的 ECEF。
// =============================================================================
SCENARIO("Ellipsoid entity no pose triggers fallback to Beijing",
         "[acceptance][config][initial-camera][fallback][CFG-ellipsoid-beijing-fallback]")
{
    // A4 + B2: Ellipsoid：单实体无 pose（地心），无 camera → 触发 fallback
    GIVEN("lz.vsgt without pose with injected ellipsoid (sits at origin)")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, )") + kMinimalModel + ", " +
                                  kMinimalWindow + "}");
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE(engine.init());
        REQUIRE(engine.ellipsoidModel());

        WHEN("the main camera is inspected")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);
            auto ep = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
            REQUIRE(ep);

            THEN("eye falls back to hardcoded Beijing (39.9, 116.4, 500) with YPR=0")
            {
                const vsg::dvec3 fallbackLla{39.9, 116.4, 500.0};
                const vsg::dvec3 expectedEye =
                    ep->ellipsoidModel->convertLatLongAltitudeToECEF(fallbackLla);
                REQUIRE(vsg::length(lookAt->eye - expectedEye) < 1e-3);

                // YPR=0 → 朝北
                vsg::dmat4 localToWorld = ep->ellipsoidModel->computeLocalToWorldTransform(fallbackLla);
                vsg::dvec3 expectedForward =
                    vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
                vsg::dvec3 actualForward = vsg::normalize(lookAt->center - lookAt->eye);
                REQUIRE(vsg::length(actualForward - expectedForward) < 1e-6);
            }
        }
    }
}

// =============================================================================
// 配置了相机 pose 时，眼点用该 pose，不用包围盒（位姿配置设计.md §7，码 CFG-camera-overrides-aabb）。
// 眼点钉在 (10, 20, 30)。偏航 45° 时前向落在 -X 与 +Y。
// =============================================================================
SCENARIO("camera pose overrides AABB computed position",
         "[acceptance][config][initial-camera][override][CFG-camera-overrides-aabb]")
{
    // A5: 有 camera.pose → 覆盖 AABB
    GIVEN("lz.vsgt with both entity pose and camera pose in Local")
    {
        Engine engine;
        engine.showWindow = false;
        EntitiesConfig cfg(
            R"([{ "id": 1, "model": "models/lz.vsgt", "pose": { "local": { "position": [0, 0, 0], "eulerYprDeg": [0, 0, 0] } } }])",
            R"({ "pose": { "local": { "position": [10, 20, 30], "eulerYprDeg": [45, 0, 0] } } })");
        REQUIRE(engine.loadConfig(cfg.cfgFile->path()));
        REQUIRE(engine.init());

        WHEN("the main camera is inspected")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);

            THEN("camera uses configured pose, not AABB")
            {
                REQUIRE(lookAt->eye.x == Catch::Approx(10.0).margin(1e-6));
                REQUIRE(lookAt->eye.y == Catch::Approx(20.0).margin(1e-6));
                REQUIRE(lookAt->eye.z == Catch::Approx(30.0).margin(1e-6));
                // forward 应旋转了 45 度（VSG 四元数约定：yaw=45 朝向 -X/+Y）
                vsg::dvec3 forward = vsg::normalize(lookAt->center - lookAt->eye);
                REQUIRE(forward.x < 0.0); // VSG yaw 正值朝向 -X
                REQUIRE(forward.y > 0.0); // yaw 正值朝向 +Y
            }
        }
    }
}

// =============================================================================
// 椭球透视的 nearFarRatio 为 0.001（位姿配置设计.md §7，码 CFG-ellipsoid-near-far-ratio）。
// =============================================================================
SCENARIO("Ellipsoid Perspective nearFarRatio is 0.001",
         "[acceptance][config][initial-camera][projection][CFG-ellipsoid-near-far-ratio]")
{
    // D2: Ellipsoid：EllipsoidPerspective 的 nearFarRatio = 0.001
    GIVEN("any Ellipsoid scene")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, )") + kMinimalModel + ", " +
                                  kMinimalWindow + "}");
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE(engine.init());
        REQUIRE(engine.ellipsoidModel());

        WHEN("the main camera projection is inspected")
        {
            auto ep = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
            REQUIRE(ep);

            THEN("nearFarRatio is 0.001")
            {
                REQUIRE(ep->nearFarRatio == Catch::Approx(0.001).margin(1e-9));
            }
        }
    }
}

// =============================================================================
// 局部透视的近远平面按包围盒半径的 0.001 与 4.5 倍取值（位姿配置设计.md §7，码 CFG-local-near-far-radius）。
// =============================================================================
SCENARIO("Local Perspective near far proportional to radius",
         "[acceptance][config][initial-camera][projection][CFG-local-near-far-radius]")
{
    // D1: Local：Perspective 的 near/far = 0.001·radius / 4.5·radius
    GIVEN("lz.vsgt in local cartesian")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ )") + kMinimalModel + ", " +
                                  kMinimalWindow + "}");
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE(engine.init());

        WHEN("the main camera projection is inspected")
        {
            auto perspective = engine.mainCamera()->projectionMatrix.cast<vsg::Perspective>();
            REQUIRE(perspective);

            THEN("near and far match 0.001*radius and 4.5*radius")
            {
                // 假设 Engine 暴露 mainScene() 访问（红测：先写测试，后实现接口）
                vsg::ComputeBounds computeBounds;
                engine.mainScene()->accept(computeBounds);
                double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;

                double expectedNear = 0.001 * radius;
                double expectedFar = 4.5 * radius;
                REQUIRE(perspective->nearDistance == Catch::Approx(expectedNear).margin(1e-9));
                REQUIRE(perspective->farDistance == Catch::Approx(expectedFar).margin(1e-9));
            }
        }
    }
}

// =============================================================================
// 局部相机 pose 远离原点时，远平面盖住眼点到包围盒中心的距离（位姿配置设计.md §7，码 CFG-local-pose-near-far）。
// 远平面至少为该距离加半径，近平面是远平面的 0.001。
// =============================================================================
SCENARIO("Local camera pose recomputes near far to prevent clipping",
         "[acceptance][config][initial-camera][projection][CFG-local-pose-near-far]")
{
    // D3: Local + camera.pose → far = max(4.5·radius, |eye-centre| + radius)
    // 此用例依赖设计实现，当前未实现
    GIVEN("lz.vsgt with camera pose far from origin")
    {
        Engine engine;
        engine.showWindow = false;
        EntitiesConfig cfg(R"([{ "id": 1, "model": "models/lz.vsgt" }])",
                           R"({ "pose": { "local": { "position": [0, 1000, 0], "eulerYprDeg": [0, 0, 0] } } })");
        REQUIRE(engine.loadConfig(cfg.cfgFile->path()));
        REQUIRE(engine.init());

        WHEN("the main camera projection is inspected")
        {
            auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
            REQUIRE(lookAt);
            auto perspective = engine.mainCamera()->projectionMatrix.cast<vsg::Perspective>();
            REQUIRE(perspective);

            THEN("far is extended to include eye distance from AABB centre")
            {
                // 假设 Engine 暴露 mainScene() 访问（红测：先写测试，后实现接口）
                vsg::ComputeBounds computeBounds;
                engine.mainScene()->accept(computeBounds);
                vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
                double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;

                double eyeDistance = vsg::length(lookAt->eye - centre);
                double minExpectedFar = eyeDistance + radius;
                REQUIRE(perspective->farDistance >= minExpectedFar);

                // near = 0.001 * far
                double expectedNear = 0.001 * perspective->farDistance;
                REQUIRE(perspective->nearDistance == Catch::Approx(expectedNear).margin(1e-9));
            }
        }
    }
}

// =============================================================================
// 验收：默认配置与角色规则（§3.1：默认 default.json，父键决定 enable）
// =============================================================================

// =============================================================================
// 未指定路径时，Engine 默认配置与默认通道文件读出的配置一致（多通道同步模块设计.md §11，码 CFG-default-matches-file）。
// =============================================================================
SCENARIO("default Engine config matches the default channel file", "[acceptance][config][CFG-default-matches-file]")
{
    GIVEN("an Engine constructed with no explicit config path")
    {
        Engine engine;

        AND_GIVEN("the default channel settings loaded from a config")
        {
            const TempConfigFile file(kDefaultJson);
            EngineChannelConfig fromFile;
            std::string error;
            REQUIRE(loadEngineChannelConfig(file.path(), fromFile, &error));

            WHEN("the default config is inspected")
            {
                THEN("it matches the default channel settings")
                {
                    requireConfigEquals(engine.config, fromFile);
                }
            }
        }
    }
}

// =============================================================================
// 默认配置没有 igConfig，init 之后不启动 IG（多通道同步模块设计.md §11，码 CFG-default-no-sync）。
// =============================================================================
SCENARIO("default initialization does not start the sync subsystem", "[acceptance][config][CFG-default-no-sync]")
{
    GIVEN("an Engine using the default config (no igConfig)")
    {
        Engine engine;
        engine.showWindow = false;

        WHEN("the Engine initializes headless")
        {
            REQUIRE(engine.init());

            THEN("no IG is started")
            {
                REQUIRE_FALSE(engine.hasIg());
            }
        }
    }
}

// =============================================================================
// 加载通道文件后，Engine 配置被换成该文件的内容（多通道同步模块设计.md §11，码 CFG-channel-replaces-config）。
// 与同一文件再解析一次的结果比较，不钉 JSON 字面量。
// =============================================================================
SCENARIO("loading a channel file replaces Engine config with that file", "[acceptance][config][CFG-channel-replaces-config]")
{
    GIVEN("an Engine and a channel file with left IG settings")
    {
        Engine engine;
        EngineChannelConfig fromFile;
        std::string error;
        const TempConfigFile file(kLeftJson);
        REQUIRE(loadEngineChannelConfig(file.path(), fromFile, &error));

        WHEN("the Engine loads that channel file")
        {
            REQUIRE(engine.loadConfig(file.path()));

            THEN("Engine config matches the loaded file")
            {
                requireConfigEquals(engine.config, fromFile);
            }
        }
    }
}

// =============================================================================
// 验收：init 后同步角色与配置一致（父键 enable，非 channelId）
// =============================================================================

// =============================================================================
// 通道文件带 igConfig 时，init 后 IG 地址与该配置一致（多通道同步模块设计.md §11，码 CFG-channel-starts-ig）。
// =============================================================================
SCENARIO("channel file starts IG using configured addresses", "[acceptance][config][CFG-channel-starts-ig]")
{
    GIVEN("an Engine loaded from a config with igConfig")
    {
        Engine engine;
        const TempConfigFile file(kLeftJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("IG runs with addresses from the loaded config")
            {
                REQUIRE(engine.hasIg());
                REQUIRE(igConfigEquals(engine.igSync().addressConfig(), *engine.config.igConfig));
            }
        }
    }
}

// =============================================================================
// 只有 IG 配置的通道文件 init 后启动 IG，地址与 igConfig 一致（多通道同步模块设计.md §11，码 CFG-ig-only-no-host）。
// 引擎配置不含 hostConfig，本条不另断言 Host 进程。
// =============================================================================
SCENARIO("IG-only channel file starts IG and does not start Host", "[acceptance][config][CFG-ig-only-no-host]")
{
    GIVEN("an Engine loaded from a config with igConfig")
    {
        Engine engine;
        const TempConfigFile file(kLeftJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("IG is started")
            {
                REQUIRE(engine.hasIg());
                REQUIRE(igConfigEquals(engine.igSync().addressConfig(), *engine.config.igConfig));
            }
        }
    }
}

// =============================================================================
// init 之后，通道 offset 写进相机驱动（多通道同步模块设计.md §11，码 CFG-channel-offset）。
// 先确认配置里的偏航不是 0，再与驱动里的 offset 比较。
// =============================================================================
SCENARIO("channel offset is applied to the camera driver after init",
         "[acceptance][config][CFG-channel-offset]")
{
    GIVEN("an Engine loaded from a channel config with non-default offset")
    {
        Engine engine;
        const TempConfigFile file(kLeftJson);
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE_FALSE(nearlyEqual(engine.config.viewDefinition.offsetDeg.yaw, 0.0));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("the camera driver uses the channel offset")
            {
                REQUIRE(offsetEquals(engine.cameraDriver().offsetDeg(), engine.config.viewDefinition.offsetDeg));
            }
        }
    }
}

// engine 配置不含 hostConfig；Host 进程由 sync 库 loadHostConfig 消费（CFG-host-* 在 sync 库测试）。

// =============================================================================
// 只有 viewID、没有 igConfig 时不启同步（多通道同步模块设计.md §3.1，码 CFG-channelid-no-sync）。
// 通道号读入 ViewDefinition.viewID，但不按该号决定是否建 IG。
// =============================================================================
SCENARIO("viewID does not enable sync when igConfig is absent", "[acceptance][config][CFG-channelid-no-sync]")
{
    GIVEN("a config with viewID 0 but no igConfig")
    {
        const TempConfigFile file(
            std::string(R"({ "viewDefinition": { "viewID": 0, "window": { "posX": 0, "posY": 0, "width": 640, "height": 480 } }, )") +
            kMinimalModel + "}");
        Engine engine;
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE(engine.config.viewDefinition.viewID == 0);
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("sync stays off despite viewID 0")
            {
                REQUIRE_FALSE(engine.hasIg());
            }
        }
    }
}

// =============================================================================
// 验收：requireConnectedIg（配置项决定连失败是否令 init 失败；缺省恒 false）
// =============================================================================

// =============================================================================
// requireConnectedIg 显式为 false 时，Host 未起仍能 init（多通道同步模块设计.md §11，码 CFG-ig-require-false-init）。
// IG 已创建，但未连上。
// =============================================================================
SCENARIO("IG-only with requireConnectedIg false can init when Host is down", "[acceptance][config][CFG-ig-require-false-init]")
{
    GIVEN("a config with requireConnectedIg false and no Host process")
    {
        Engine engine;
        const TempConfigFile file(kLeftJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            THEN("init succeeds even though IG cannot connect")
            {
                REQUIRE(engine.init());
                REQUIRE(engine.hasIg());
                REQUIRE_FALSE(engine.igLinked());
            }
        }
    }
}

// =============================================================================
// 省略 requireConnectedIg 时按 false 处理，Host 未起仍能 init（多通道同步模块设计.md §11，码 CFG-ig-require-omit-false）。
// IG 已创建，但未连上。不从别的字段推导该开关。
// =============================================================================
SCENARIO("IG-only omitting requireConnectedIg defaults to false and can init when Host is down",
         "[acceptance][config][CFG-ig-require-omit-false]")
{
    GIVEN("an IG-only config that does not set requireConnectedIg")
    {
        const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, )") + jsonIgConfig(18005) + ", " +
                                  kMinimalModel + ", " + kMinimalWindow + "}");
        Engine engine;
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes without a Host")
        {
            THEN("init succeeds (default requireConnectedIg is false, no derivation)")
            {
                REQUIRE(engine.init());
                REQUIRE(engine.hasIg());
                REQUIRE_FALSE(engine.igLinked());
            }
        }
    }
}

// =============================================================================
// requireConnectedIg 为 true 且 Host 未起时，init 失败（多通道同步模块设计.md §11，码 CFG-ig-require-true-fail）。
// =============================================================================
SCENARIO("IG-only with requireConnectedIg true fails init when Host is down", "[acceptance][config][CFG-ig-require-true-fail]")
{
    GIVEN("an IG-only config that requires a successful connect")
    {
        const TempConfigFile file(std::string(R"({ "injectEllipsoidIfMissing": true, )") + jsonIgConfig(18003) +
                                  R"(, "viewDefinition": { "requireConnectedIg": true, "window": { "posX": 0, "posY": 0, "width": 640, "height": 480 } }, )" +
                                  kMinimalModel + "}");
        Engine engine;
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes without a Host")
        {
            THEN("init fails")
            {
                REQUIRE_FALSE(engine.init());
            }
        }
    }
}

// =============================================================================
// requireConnectedIg 为 true 且 Host 已在听时，init 成功并连上（多通道同步模块设计.md §11，码 CFG-ig-require-true-ok）。
// 只要求链路已建立。
// =============================================================================
SCENARIO("IG-only with requireConnectedIg true succeeds when Host is running", "[acceptance][config][CFG-ig-require-true-ok]")
{
    GIVEN("an IG config that requires a successful connect, and an independent HostSync on those ports")
    {
        // kMainJson 端口（ig target tcp=8100 / udpRecv=8000；host 本地 tcp=8100 / udpRecv=8000 / udpSend=8001）。
        HostSync host;
        REQUIRE(host.initialize(HostConfig{8000, 8100}));

        Engine engine;
        const TempConfigFile file(kMainJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes with the Host running")
        {
            THEN("init succeeds and IG is linked")
            {
                REQUIRE(engine.init());
                REQUIRE(engine.hasIg());
                REQUIRE(engine.igLinked());
            }
        }
    }
}

// =============================================================================
// 命令行路径。通道 JSON 的解析校验在 thirdparty/config/tests。
// =============================================================================

// =============================================================================
// 命令行没有 -c 时，配置路径是随仓 default.json（多通道同步模块设计.md §11，码 CFG-cli-default-path）。
// 只比较路径字符串，不读文件。
// =============================================================================
TEST_CASE("resolveConfigPath defaults to default config when -c is omitted", "[unit][config][cli][CFG-cli-default-path]")
{
    char arg0[] = "engine.exe";
    char* argv[] = {arg0};
    const std::string defaultPath = std::string(RESOURCE_DIR) + "/config/default.json";
    REQUIRE(Engine::resolveConfigPath(1, argv) == defaultPath);
}

// =============================================================================
// -c 后面的参数原样作为配置路径（多通道同步模块设计.md §11，码 CFG-cli-c-path）。
// 只比较路径字符串，不读文件。
// =============================================================================
TEST_CASE("resolveConfigPath uses the path after -c", "[unit][config][cli][CFG-cli-c-path]")
{
    char arg0[] = "engine.exe";
    char argC[] = "-c";
    const std::string givenPath = std::string(RESOURCE_DIR) + "/config/some-channel.json";
    char* argv[] = {arg0, argC, const_cast<char*>(givenPath.c_str())};
    REQUIRE(Engine::resolveConfigPath(3, argv) == givenPath);
}

// =============================================================================
// 随仓 viewhost IG 通道文件指向 entities.json，目录里有 5 条实体（实体管理设计.md §10，码 CFG-viewhost-ig-entities-path）。
// 读随仓文件，核对路径字符串和条数。
// =============================================================================
TEST_CASE("shipped viewhost_ig_main.json points at entities.json", "[unit][config][parse][entities-catalog][CFG-viewhost-ig-entities-path]")
{
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(std::string(RESOURCE_DIR) + "/config/viewhost_ig_main.json", cfg, &error));
    REQUIRE(cfg.entitiesFilePath == "config/entities.json");

    std::vector<EntityConfig> entities;
    REQUIRE(loadEntitiesFile(std::string(RESOURCE_DIR) + "/config/entities.json", entities, &error));
    REQUIRE(entities.size() == 5);
}

// =============================================================================
// 验收：窗口几何与通道配置一致
// =============================================================================

// =============================================================================
// 开窗后，窗口位置和尺寸与通道 window 一致（多通道同步模块设计.md §11，码 CFG-window-matches-channel）。
// traits 与 extent2D 都要与配置相同。
// =============================================================================
SCENARIO("on-screen window position and size match the channel config", "[acceptance][config][CFG-window-matches-channel]")
{
    GIVEN("an Engine loaded from a channel config with window shown")
    {
        Engine engine;
        const TempConfigFile file(kLeftJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = true;

        WHEN("the Engine initializes and creates a window")
        {
            REQUIRE(engine.init());

            THEN("window traits and extent match config.window")
            {
                auto win = engine.mainWindow();
                REQUIRE(win);
                auto traits = win->traits();
                REQUIRE(traits);

                REQUIRE(traits->x == engine.config.viewDefinition.window.posX);
                REQUIRE(traits->y == engine.config.viewDefinition.window.posY);
                REQUIRE(traits->width == static_cast<uint32_t>(engine.config.viewDefinition.window.width));
                REQUIRE(traits->height == static_cast<uint32_t>(engine.config.viewDefinition.window.height));

                const VkExtent2D extent = win->extent2D();
                REQUIRE(extent.width == static_cast<uint32_t>(engine.config.viewDefinition.window.width));
                REQUIRE(extent.height == static_cast<uint32_t>(engine.config.viewDefinition.window.height));
            }
        }
    }
}
