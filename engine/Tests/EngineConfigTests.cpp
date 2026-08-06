#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "InitialCameraConfig.h"
#include "engine.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

// Tests below encode doc/design/多通道同步模块设计.md §3.1 contracts, and
// doc/design/lla位姿传输设计.md §2.1 / §6 / §7 (coordFrame → EllipsoidModel assembly).
// New LLA/coordFrame cases are expected to fail until parse + inject land.
// Avoid referencing members not yet on EngineChannelConfig; assert via init-time
// observables (role, scene/camera semantics, load success/failure).

namespace
{
    const char* kDefaultJson = R"({"model":"models/lz.vsgt","window":{"x":0,"y":0,"width":1920,"height":1080}})";
    const char* kMainJson =
        R"({"channelId":0,"offsetDeg":{"yaw":0.0,"pitch":0.0,"roll":0.0},"igLocal":{"addr":"127.0.0.1","udpPortSend":8000,"udpPortRecv":8001},"hostEndpoint":{"addr":"127.0.0.1","tcpPort":8100,"udpPortRecv":8000},"hostLocal":{"addr":"127.0.0.1","udpPortSend":8001,"udpPortRecv":8000,"tcpPort":8100},"model":"models/lz.vsgt","window":{"x":640,"y":0,"width":640,"height":1080},"hostEyeStalePolicy":"ReuseLast","requireIgConnect":true})";
    const char* kLeftJson =
        R"({"channelId":1,"offsetDeg":{"yaw":18.05,"pitch":0.0,"roll":0.0},"igLocal":{"addr":"127.0.0.1","udpPortSend":8000,"udpPortRecv":8003},"hostEndpoint":{"addr":"127.0.0.1","tcpPort":8100,"udpPortRecv":8000},"model":"models/lz.vsgt","window":{"x":0,"y":0,"width":640,"height":1080},"hostEyeStalePolicy":"ReuseLast","requireIgConnect":false})";

    bool nearlyEqual(double a, double b, double eps = 1e-9)
    {
        return std::abs(a - b) <= eps;
    }

    // Role-relevant field compares (design address split). Unused AddressConfig
    // members are ignored so tests stay valid after IgLocal/HostEndpoint types land.
    bool hostLocalEquals(const AddressConfig& a, const AddressConfig& b)
    {
        return a.addr == b.addr && a.udpPortSend == b.udpPortSend && a.udpPortRecv == b.udpPortRecv &&
               a.tcpPort == b.tcpPort;
    }

    bool igLocalEquals(const AddressConfig& a, const AddressConfig& b)
    {
        return a.addr == b.addr && a.udpPortSend == b.udpPortSend && a.udpPortRecv == b.udpPortRecv;
    }

    bool hostEndpointEquals(const AddressConfig& a, const AddressConfig& b)
    {
        return a.addr == b.addr && a.tcpPort == b.tcpPort && a.udpPortRecv == b.udpPortRecv;
    }

    bool offsetEquals(const OffsetDeg& a, const OffsetDeg& b)
    {
        return nearlyEqual(a.yaw, b.yaw) && nearlyEqual(a.pitch, b.pitch) && nearlyEqual(a.roll, b.roll);
    }

    void requireConfigEquals(const EngineChannelConfig& actual, const EngineChannelConfig& expected)
    {
        REQUIRE(actual.channelId == expected.channelId);
        REQUIRE(offsetEquals(actual.offsetDeg, expected.offsetDeg));
        REQUIRE(igLocalEquals(actual.igLocal, expected.igLocal));
        REQUIRE(hostEndpointEquals(actual.hostEndpoint, expected.hostEndpoint));
        REQUIRE(hostLocalEquals(actual.hostLocal, expected.hostLocal));
        REQUIRE(actual.model == expected.model);
        REQUIRE(actual.window.x == expected.window.x);
        REQUIRE(actual.window.y == expected.window.y);
        REQUIRE(actual.window.width == expected.window.width);
        REQUIRE(actual.window.height == expected.window.height);
        REQUIRE(actual.hostEyeStalePolicy == expected.hostEyeStalePolicy);
        // When EngineChannelConfig::requireIgConnect exists, also:
        // REQUIRE(actual.requireIgConnect == expected.requireIgConnect);
    }

    class TempConfigFile
    {
    public:
        explicit TempConfigFile(const std::string& jsonBody)
        {
            _path = (std::filesystem::temp_directory_path() /
                     ("ave_engine_cfg_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".json"))
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

    const char* kMinimalWindow = R"("window": { "x": 0, "y": 0, "width": 640, "height": 480 })";
    const char* kMinimalModel = R"("model": "models/lz.vsgt")";
    const char* kReadymapModel = R"("model": "models/readymap.vsgt")";

    std::string jsonIgLocal(int udpRecv = 8003)
    {
        return std::string(R"("igLocal": { "addr": "127.0.0.1", "udpPortSend": 8000, "udpPortRecv": )") +
               std::to_string(udpRecv) + " }";
    }

    std::string jsonHostEndpoint()
    {
        return R"("hostEndpoint": { "addr": "127.0.0.1", "tcpPort": 8100, "udpPortRecv": 8000 })";
    }

    std::string jsonHostLocal()
    {
        return R"("hostLocal": { "addr": "127.0.0.1", "udpPortSend": 8001, "udpPortRecv": 8000, "tcpPort": 8100 })";
    }
} // namespace

// =============================================================================
// 验收：coordFrame → 场景 EllipsoidModel（lla位姿传输设计.md §2.1 / §6 / §7）
// JSON 意图："Ellipsoid" | "Local"（缺省 Local）；挂到 scene 的对象类型为 EllipsoidModel。
// =============================================================================

SCENARIO("coordFrame Ellipsoid places EllipsoidModel on scene; otherwise lz has none",
         "[acceptance][bdd][config][coordFrame]")
{
    GIVEN("a channel config that uses lz.vsgt (model has no built-in EllipsoidModel)")
    {
        WHEN("coordFrame is set to Ellipsoid")
        {
            const TempConfigFile file(std::string(R"({ "coordFrame": "Ellipsoid", )") + kMinimalModel + ", " +
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

        WHEN("coordFrame is omitted (defaults to Local)")
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

SCENARIO("model-built-in EllipsoidModel is kept when coordFrame is Local or omitted",
         "[acceptance][bdd][config][coordFrame]")
{
    GIVEN("a channel config that uses readymap.vsgt (model already has EllipsoidModel)")
    {
        WHEN("coordFrame is omitted")
        {
            const TempConfigFile file(std::string("{") + kReadymapModel + ", " + kMinimalWindow + "}");
            Engine engine;
            REQUIRE(engine.loadConfig(file.path()));
            engine.showWindow = false;
            REQUIRE(engine.init());

            THEN("the scene still carries EllipsoidModel (runtime stays ellipsoid)")
            {
                REQUIRE(engine.ellipsoidModel());
            }
        }

        WHEN("coordFrame is explicitly Local")
        {
            const TempConfigFile file(std::string(R"({ "coordFrame": "Local", )") + kReadymapModel + ", " +
                                      kMinimalWindow + "}");
            Engine engine;
            REQUIRE(engine.loadConfig(file.path()));
            engine.showWindow = false;
            REQUIRE(engine.init());

            THEN("the scene still carries EllipsoidModel (Local does not strip model ellipsoid)")
            {
                REQUIRE(engine.ellipsoidModel());
            }
        }
    }
}

// 位姿配置设计.md §4：Ellipsoid 下默认初始相机由 AABB 计算，不写死北京。
// 覆盖「注入在相机创建前」：lz 无自带椭球，靠 coordFrame 注入后才能建 EllipsoidPerspective。
// 无 pose 实体默认摆在地心，会触发 fallback 到北京上空（见 §4.2 fallback 判据）。
SCENARIO("coordFrame Ellipsoid injects before camera create with fallback LLA LookAt",
         "[acceptance][bdd][config][coordFrame][initial-lla]")
{
    // NOTE: 当前实现仍写死北京 500m，尚未实现 AABB 计算。本测试先写定新设计目标，
    //       待实现后移除 !hide 标签并验证 AABB 关系。
    GIVEN("lz.vsgt with coordFrame Ellipsoid (inject WGS-84 EllipsoidModel)")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "coordFrame": "Ellipsoid", )") + kMinimalModel + ", " +
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
                // YPR=0 → ENU forward = North = column 1 of LocalToWorld.
                const vsg::dvec3 expectedForward =
                    vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
                const vsg::dvec3 actualForward = vsg::normalize(lookAt->center - lookAt->eye);
                REQUIRE(vsg::length(actualForward - expectedForward) < 1e-6);
            }
        }
    }
}

SCENARIO("model with built-in ellipsoid initializes camera from AABB, not hardcoded Beijing",
         "[acceptance][bdd][config][coordFrame][initial-lla]")
{
    // NOTE: 当前实现仍写死北京 500m，尚未实现 AABB 计算。本测试先写定新设计目标，
    //       待实现后移除 !hide 标签并验证 AABB 关系。
    GIVEN("an Engine initialized with readymap (built-in EllipsoidModel, no inject)")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string("{") + kReadymapModel + ", " + kMinimalWindow + "}");
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

SCENARIO("Local single entity no pose camera uses AABB with k_back=3.5",
         "[acceptance][bdd][config][initial-camera][aabb]")
{
    // A1: Local：单实体无 pose，无 camera → eye = AABB centre - Y方向 3.5·radius
    GIVEN("lz.vsgt in Local coordFrame without pose")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "coordFrame": "Local", )") + kMinimalModel + ", " +
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

SCENARIO("Local multiple entities no pose camera uses overall AABB",
         "[acceptance][bdd][config][initial-camera][aabb]")
{
    // A2: Local：多实体分散，无 camera → eye 按整体 AABB
    GIVEN("two lz.vsgt entities at different positions without camera pose")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(R"({
            "coordFrame": "Local",
            "entities": [
                { "id": 1, "model": "models/lz.vsgt", "pose": { "local": { "position": [0, 0, 0], "eulerYprDeg": [0, 0, 0] } } },
                { "id": 2, "model": "models/lz.vsgt", "pose": { "local": { "position": [100, 0, 0], "eulerYprDeg": [0, 0, 0] } } }
            ],
        )" + std::string(kMinimalWindow) +
                                  "}");
        REQUIRE(engine.loadConfig(file.path()));
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

SCENARIO("Ellipsoid entity with LLA pose camera uses AABB or fallback",
         "[acceptance][bdd][config][initial-camera][aabb]")
{
    // A3: Ellipsoid：单实体有 LLA pose，无 camera → eye 按整体 AABB
    // 注意：小模型（lz.vsgt ~1.4km）即使摆放在天安门，AABB centre 的 ECEF 坐标
    // 仍可能很小，触发 fallback（|centre| < 0.9·radiusPolar）。这是合理的。
    GIVEN("lz.vsgt at Tiananmen LLA without camera pose")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(R"({
            "coordFrame": "Ellipsoid",
            "entities": [
                {
                    "id": 1,
                    "model": "models/lz.vsgt",
                    "pose": {
                        "ellipsoid": {
                            "lla": { "lat": 39.9087, "lon": 116.3975, "alt": 0.0 },
                            "eulerYprDeg": [0, 0, 0]
                        }
                    }
                }
            ],
        )" + std::string(kMinimalWindow) +
                                  "}");
        REQUIRE(engine.loadConfig(file.path()));
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

SCENARIO("Ellipsoid entity no pose triggers fallback to Beijing",
         "[acceptance][bdd][config][initial-camera][fallback]")
{
    // A4 + B2: Ellipsoid：单实体无 pose（地心），无 camera → 触发 fallback
    GIVEN("lz.vsgt without pose in Ellipsoid coordFrame (sits at origin)")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "coordFrame": "Ellipsoid", )") + kMinimalModel + ", " +
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

SCENARIO("camera pose overrides AABB computed position",
         "[acceptance][bdd][config][initial-camera][override]")
{
    // A5: 有 camera.pose → 覆盖 AABB
    GIVEN("lz.vsgt with both entity pose and camera pose in Local")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(R"({
            "coordFrame": "Local",
            "entities": [
                { "id": 1, "model": "models/lz.vsgt", "pose": { "local": { "position": [0, 0, 0], "eulerYprDeg": [0, 0, 0] } } }
            ],
            "camera": {
                "pose": {
                    "local": {
                        "position": [10, 20, 30],
                        "eulerYprDeg": [45, 0, 0]
                    }
                }
            },
        )" + std::string(kMinimalWindow) +
                                  "}");
        REQUIRE(engine.loadConfig(file.path()));
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

SCENARIO("Ellipsoid Perspective nearFarRatio is 0.001",
         "[acceptance][bdd][config][initial-camera][projection]")
{
    // D2: Ellipsoid：EllipsoidPerspective 的 nearFarRatio = 0.001
    GIVEN("any Ellipsoid scene")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "coordFrame": "Ellipsoid", )") + kMinimalModel + ", " +
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

SCENARIO("Local Perspective near far proportional to radius",
         "[acceptance][bdd][config][initial-camera][projection]")
{
    // D1: Local：Perspective 的 near/far = 0.001·radius / 4.5·radius
    GIVEN("lz.vsgt in Local coordFrame")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(std::string(R"({ "coordFrame": "Local", )") + kMinimalModel + ", " +
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

SCENARIO("Local camera pose recomputes near far to prevent clipping",
         "[acceptance][bdd][config][initial-camera][projection]")
{
    // D3: Local + camera.pose → far = max(4.5·radius, |eye-centre| + radius)
    // 此用例依赖设计实现，当前未实现
    GIVEN("lz.vsgt with camera pose far from origin")
    {
        Engine engine;
        engine.showWindow = false;
        const TempConfigFile file(R"({
            "coordFrame": "Local",
            "entities": [
                { "id": 1, "model": "models/lz.vsgt" }
            ],
            "camera": {
                "pose": {
                    "local": {
                        "position": [0, 1000, 0],
                        "eulerYprDeg": [0, 0, 0]
                    }
                }
            },
        )" + std::string(kMinimalWindow) +
                                  "}");
        REQUIRE(engine.loadConfig(file.path()));
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

SCENARIO("default Engine config matches the default channel file", "[acceptance][bdd][config]")
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

SCENARIO("default initialization does not start the sync subsystem", "[acceptance][bdd][config]")
{
    GIVEN("an Engine using the default config (no hostLocal / igLocal)")
    {
        Engine engine;
        engine.showWindow = false;

        WHEN("the Engine initializes headless")
        {
            REQUIRE(engine.init());

            THEN("neither Host nor IG is started")
            {
                REQUIRE_FALSE(engine.synchronSystem().hasHost());
                REQUIRE_FALSE(engine.synchronSystem().hasIg());
            }
        }
    }
}

SCENARIO("loading a channel file replaces Engine config with that file", "[acceptance][bdd][config]")
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

SCENARIO("main channel file starts Host and IG using configured addresses", "[acceptance][bdd][config]")
{
    GIVEN("an Engine loaded from a Host+IG+Endpoint config")
    {
        Engine engine;
        const TempConfigFile file(kMainJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("IG and Host run with addresses from the loaded config")
            {
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE(engine.synchronSystem().hasHost());
                REQUIRE(igLocalEquals(engine.synchronSystem().igSync().addressConfig(), engine.config.igLocal));
                REQUIRE(hostLocalEquals(engine.synchronSystem().hostSync().addressConfig(), engine.config.hostLocal));
            }
        }
    }
}

SCENARIO("IG-only channel file starts IG and does not start Host", "[acceptance][bdd][config]")
{
    GIVEN("an Engine loaded from a config with igLocal+hostEndpoint (no hostLocal)")
    {
        Engine engine;
        const TempConfigFile file(kLeftJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("IG is started and Host is not")
            {
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE_FALSE(engine.synchronSystem().hasHost());
                REQUIRE(igLocalEquals(engine.synchronSystem().igSync().addressConfig(), engine.config.igLocal));
            }
        }
    }
}

SCENARIO("channel offset and stale policy are applied to SynchronSystem after init",
         "[acceptance][bdd][config]")
{
    GIVEN("an Engine loaded from a channel config with non-default offset")
    {
        Engine engine;
        const TempConfigFile file(kLeftJson);
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE_FALSE(nearlyEqual(engine.config.offsetDeg.yaw, 0.0));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("SynchronSystem uses the channel offset and stale policy")
            {
                REQUIRE(offsetEquals(engine.synchronSystem().offsetDeg(), engine.config.offsetDeg));
                REQUIRE(engine.synchronSystem().hostEyeStalePolicy() == engine.config.hostEyeStalePolicy);
            }
        }
    }
}

SCENARIO("host-only config starts Host and does not start IG", "[acceptance][bdd][config]")
{
    GIVEN("a config that only contains hostLocal (plus model/window)")
    {
        const TempConfigFile file(std::string("{") + jsonHostLocal() + ", " + kMinimalModel + ", " +
                                  kMinimalWindow + "}");
        Engine engine;
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("only Host is started")
            {
                REQUIRE(engine.synchronSystem().hasHost());
                REQUIRE_FALSE(engine.synchronSystem().hasIg());
            }
        }
    }
}

SCENARIO("channelId does not start Host when hostLocal is absent", "[acceptance][bdd][config]")
{
    GIVEN("a config with channelId 0 but no hostLocal or igLocal")
    {
        const TempConfigFile file(std::string(R"({ "channelId": 0, )") + kMinimalModel + ", " + kMinimalWindow +
                                  "}");
        Engine engine;
        REQUIRE(engine.loadConfig(file.path()));
        REQUIRE(engine.config.channelId == 0);
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("sync stays off despite channelId 0")
            {
                REQUIRE_FALSE(engine.synchronSystem().hasHost());
                REQUIRE_FALSE(engine.synchronSystem().hasIg());
            }
        }
    }
}

// =============================================================================
// 验收：requireIgConnect（配置项决定连失败是否令 init 失败；缺省恒 false）
// =============================================================================

SCENARIO("IG-only with requireIgConnect false can init when Host is down", "[acceptance][bdd][config]")
{
    GIVEN("a config with requireIgConnect false and no Host process")
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
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE_FALSE(engine.synchronSystem().igLinked());
            }
        }
    }
}

SCENARIO("IG-only omitting requireIgConnect defaults to false and can init when Host is down",
         "[acceptance][bdd][config]")
{
    GIVEN("an IG-only config that does not set requireIgConnect")
    {
        const TempConfigFile file(std::string("{") + jsonIgLocal(18005) + ", " + jsonHostEndpoint() + ", " +
                                  kMinimalModel + ", " + kMinimalWindow + "}");
        Engine engine;
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes without a Host")
        {
            THEN("init succeeds (default requireIgConnect is false, no derivation)")
            {
                REQUIRE(engine.init());
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE_FALSE(engine.synchronSystem().igLinked());
            }
        }
    }
}

SCENARIO("IG-only with requireIgConnect true fails init when Host is down", "[acceptance][bdd][config]")
{
    GIVEN("an IG-only config that requires a successful connect")
    {
        const TempConfigFile file(std::string("{") + jsonIgLocal(18003) + ", " + jsonHostEndpoint() + ", " +
                                  R"("requireIgConnect": true, )" + kMinimalModel + ", " + kMinimalWindow + "}");
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

SCENARIO("main channel requireIgConnect true succeeds when Host is local", "[acceptance][bdd][config]")
{
    GIVEN("a config with Host+IG and requireIgConnect true")
    {
        Engine engine;
        const TempConfigFile file(kMainJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            THEN("init succeeds and IG is linked")
            {
                REQUIRE(engine.init());
                REQUIRE(engine.synchronSystem().hasHost());
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE(engine.synchronSystem().igLinked());
            }
        }
    }
}

// =============================================================================
// 单元 / 验收：解析校验（方案 A、跨字段、未知键）
// =============================================================================

TEST_CASE("resolveConfigPath defaults to default config when -c is omitted", "[unit][config][cli]")
{
    char arg0[] = "engine.exe";
    char* argv[] = {arg0};
    const std::string defaultPath = std::string(RESOURCE_DIR) + "/config/default.json";
    REQUIRE(Engine::resolveConfigPath(1, argv) == defaultPath);
}

TEST_CASE("resolveConfigPath uses the path after -c", "[unit][config][cli]")
{
    char arg0[] = "engine.exe";
    char argC[] = "-c";
    const std::string givenPath = std::string(RESOURCE_DIR) + "/config/some-channel.json";
    char* argv[] = {arg0, argC, const_cast<char*>(givenPath.c_str())};
    REQUIRE(Engine::resolveConfigPath(3, argv) == givenPath);
}

TEST_CASE("loadEngineChannelConfig rejects unknown top-level keys", "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + kMinimalModel + ", " + kMinimalWindow +
                              R"(, "hostLocl": { "addr": "127.0.0.1" }})");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects unknown nested keys", "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + kMinimalModel +
                              R"(, "window": { "x": 0, "y": 0, "width": 640, "height": 480, "fullscreen": true }})");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects igLocal without hostEndpoint", "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + jsonIgLocal() + ", " + kMinimalModel + ", " + kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects hostLocal+igLocal without hostEndpoint (no derivation)",
          "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + jsonHostLocal() + ", " + jsonIgLocal(8001) + ", " + kMinimalModel +
                              ", " + kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects hostEndpoint without igLocal", "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + jsonHostEndpoint() + ", " + kMinimalModel + ", " + kMinimalWindow +
                              "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects requireIgConnect without igLocal", "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + R"("requireIgConnect": true, )" + kMinimalModel + ", " +
                              kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects partial window object (scheme A)", "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + kMinimalModel + R"(, "window": { "width": 800 }})");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects partial offsetDeg object (scheme A)", "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + kMinimalModel + ", " + kMinimalWindow +
                              R"(, "offsetDeg": { "yaw": 1.0 }})");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects partial hostLocal object (scheme A)", "[unit][config][parse]")
{
    const TempConfigFile file(
        std::string(R"({ "hostLocal": { "addr": "127.0.0.1", "udpPortSend": 8001, "udpPortRecv": 8000 }, )") +
        kMinimalModel + ", " + kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects partial igLocal object (scheme A)", "[unit][config][parse]")
{
    const TempConfigFile file(std::string(R"({ "igLocal": { "addr": "127.0.0.1", "udpPortSend": 8000 }, )") +
                              jsonHostEndpoint() + ", " + kMinimalModel + ", " + kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects partial hostEndpoint object (scheme A)", "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + jsonIgLocal() +
                              R"(, "hostEndpoint": { "addr": "127.0.0.1", "tcpPort": 8100 }, )" + kMinimalModel +
                              ", " + kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects unknown igLocal.tcpPort (IgLocalConfig has no tcp)",
          "[unit][config][parse]")
{
    const TempConfigFile file(
        std::string(R"({ "igLocal": { "addr": "127.0.0.1", "udpPortSend": 8000, "udpPortRecv": 8003, "tcpPort": 8100 }, )") +
        jsonHostEndpoint() + ", " + kMinimalModel + ", " + kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig rejects unknown hostEndpoint.udpPortSend (HostEndpointConfig)",
          "[unit][config][parse]")
{
    const TempConfigFile file(std::string("{") + jsonIgLocal() +
                              R"(, "hostEndpoint": { "addr": "127.0.0.1", "tcpPort": 8100, "udpPortRecv": 8000, "udpPortSend": 8001 }, )" +
                              kMinimalModel + ", " + kMinimalWindow + "}");
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE_FALSE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("loadEngineChannelConfig accepts default config with only model and window", "[unit][config][parse]")
{
    const TempConfigFile file(kDefaultJson);
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE(cfg.model == "models/lz.vsgt");
    REQUIRE(cfg.window.width == 1920);
    REQUIRE(cfg.window.height == 1080);
}

TEST_CASE("loadEngineChannelConfig accepts Host+IG sample config", "[unit][config][parse]")
{
    const TempConfigFile file(kMainJson);
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE_FALSE(cfg.hostLocal.addr.empty());
    REQUIRE_FALSE(cfg.igLocal.addr.empty());
    REQUIRE_FALSE(cfg.hostEndpoint.addr.empty());
}

TEST_CASE("loadEngineChannelConfig accepts IG-only sample config", "[unit][config][parse]")
{
    const TempConfigFile file(kLeftJson);
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(file.path(), cfg, &error));
    REQUIRE(cfg.hostLocal.addr.empty());
    REQUIRE_FALSE(cfg.igLocal.addr.empty());
    REQUIRE_FALSE(cfg.hostEndpoint.addr.empty());
}

// =============================================================================
// 验收：窗口几何与通道配置一致
// =============================================================================

SCENARIO("on-screen window position and size match the channel config", "[acceptance][bdd][config]")
{
    GIVEN("an Engine loaded from a channel config with window shown")
    {
        Engine engine;
        const TempConfigFile file(kMainJson);
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

                REQUIRE(traits->x == engine.config.window.x);
                REQUIRE(traits->y == engine.config.window.y);
                REQUIRE(traits->width == static_cast<uint32_t>(engine.config.window.width));
                REQUIRE(traits->height == static_cast<uint32_t>(engine.config.window.height));

                const VkExtent2D extent = win->extent2D();
                REQUIRE(extent.width == static_cast<uint32_t>(engine.config.window.width));
                REQUIRE(extent.height == static_cast<uint32_t>(engine.config.window.height));
            }
        }
    }
}
