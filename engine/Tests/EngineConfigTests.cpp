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

// Tests below encode doc/多通道同步模块设计.md §3.1 contracts.
// They are expected to fail until EngineConfig / load / Engine::init are aligned.
// Avoid referencing members not yet on EngineChannelConfig (e.g. wire requireIgConnect
// asserts when the field lands); role is asserted via hasHost/hasIg after init, and via
// load success/failure for parse rules.

namespace
{
    std::string configPath(const char* fileName)
    {
        return std::string(RESOURCE_DIR) + "/config/" + fileName;
    }

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
// 验收：默认配置与角色规则（§3.1：默认 default.json，父键决定 enable）
// =============================================================================

SCENARIO("default Engine config matches the default channel file", "[acceptance][bdd][config]")
{
    GIVEN("an Engine constructed with no explicit config path")
    {
        Engine engine;

        AND_GIVEN("the same settings loaded from default.json")
        {
            EngineChannelConfig fromFile;
            std::string error;
            REQUIRE(loadEngineChannelConfig(configPath("default.json"), fromFile, &error));

            WHEN("the default config is inspected")
            {
                THEN("it matches default.json")
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
    GIVEN("an Engine and the left channel file")
    {
        Engine engine;
        EngineChannelConfig fromFile;
        std::string error;
        const std::string path = configPath("left.json");
        REQUIRE(loadEngineChannelConfig(path, fromFile, &error));

        WHEN("the Engine loads that channel file")
        {
            REQUIRE(engine.loadConfig(path));

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
    GIVEN("an Engine loaded from main.json (hostLocal + igLocal + hostEndpoint)")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("main.json")));
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
    GIVEN("an Engine loaded from left.json (igLocal + hostEndpoint, no hostLocal)")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("left.json")));
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
    GIVEN("an Engine loaded from the left channel file with non-default offset")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("left.json")));
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
    GIVEN("left.json (requireIgConnect false) and no Host process")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("left.json")));
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
    GIVEN("main.json with Host+IG and requireIgConnect true")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("main.json")));
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

TEST_CASE("resolveConfigPath defaults to default.json when -c is omitted", "[unit][config][cli]")
{
    char arg0[] = "engine.exe";
    char* argv[] = {arg0};
    REQUIRE(Engine::resolveConfigPath(1, argv) == configPath("default.json"));
}

TEST_CASE("resolveConfigPath uses the path after -c", "[unit][config][cli]")
{
    char arg0[] = "engine.exe";
    char argC[] = "-c";
    std::string leftPath = configPath("left.json");
    char* argv[] = {arg0, argC, leftPath.data()};
    REQUIRE(Engine::resolveConfigPath(3, argv) == leftPath);
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

TEST_CASE("loadEngineChannelConfig accepts default.json with only model and window", "[unit][config][parse]")
{
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(configPath("default.json"), cfg, &error));
    REQUIRE(cfg.model == "models/lz.vsgt");
    REQUIRE(cfg.window.width == 1920);
    REQUIRE(cfg.window.height == 1080);
}

TEST_CASE("loadEngineChannelConfig accepts main.json Host+IG sample", "[unit][config][parse]")
{
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(configPath("main.json"), cfg, &error));
    REQUIRE_FALSE(cfg.hostLocal.addr.empty());
    REQUIRE_FALSE(cfg.igLocal.addr.empty());
    REQUIRE_FALSE(cfg.hostEndpoint.addr.empty());
}

TEST_CASE("loadEngineChannelConfig accepts left.json IG-only sample", "[unit][config][parse]")
{
    EngineChannelConfig cfg;
    std::string error;
    REQUIRE(loadEngineChannelConfig(configPath("left.json"), cfg, &error));
    REQUIRE(cfg.hostLocal.addr.empty());
    REQUIRE_FALSE(cfg.igLocal.addr.empty());
    REQUIRE_FALSE(cfg.hostEndpoint.addr.empty());
}

// =============================================================================
// 验收：窗口几何与通道配置一致
// =============================================================================

SCENARIO("on-screen window position and size match the channel config", "[acceptance][bdd][config]")
{
    GIVEN("an Engine loaded from the main channel file with window shown")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("main.json")));
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
