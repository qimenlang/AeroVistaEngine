#include <catch2/catch_test_macros.hpp>

#include "engine.h"

#include <cmath>
#include <string>

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

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

    bool addressEquals(const AddressConfig& a, const AddressConfig& b)
    {
        return a.addr == b.addr && a.udpPortSend == b.udpPortSend && a.udpPortRecv == b.udpPortRecv &&
               a.tcpPort == b.tcpPort;
    }

    bool offsetEquals(const OffsetDeg& a, const OffsetDeg& b)
    {
        return nearlyEqual(a.yaw, b.yaw) && nearlyEqual(a.pitch, b.pitch) && nearlyEqual(a.roll, b.roll);
    }

    /// Compare channel configs without hard-coding main.json / left.json literals.
    void requireConfigEquals(const EngineChannelConfig& actual, const EngineChannelConfig& expected)
    {
        REQUIRE(actual.channelId == expected.channelId);
        REQUIRE(offsetEquals(actual.offsetDeg, expected.offsetDeg));
        REQUIRE(addressEquals(actual.igLocal, expected.igLocal));
        REQUIRE(addressEquals(actual.hostEndpoint, expected.hostEndpoint));
        REQUIRE(addressEquals(actual.hostLocal, expected.hostLocal));
        REQUIRE(actual.model == expected.model);
        REQUIRE(actual.window.x == expected.window.x);
        REQUIRE(actual.window.y == expected.window.y);
        REQUIRE(actual.window.width == expected.window.width);
        REQUIRE(actual.window.height == expected.window.height);
        REQUIRE(actual.hostEyeStalePolicy == expected.hostEyeStalePolicy);
    }
} // namespace

// =============================================================================
// 验收：默认配置与 channel 角色规则（钉行为，不钉 JSON 字面量）
// =============================================================================

SCENARIO("default Engine config matches the main channel file", "[acceptance][bdd][config]")
{
    GIVEN("an Engine constructed with no explicit config path")
    {
        Engine engine;

        AND_GIVEN("the same settings loaded from the main channel file")
        {
            EngineChannelConfig fromFile;
            std::string error;
            REQUIRE(loadEngineChannelConfig(configPath("main.json"), fromFile, &error));

            WHEN("the default config is inspected")
            {
                THEN("it matches the main channel file")
                {
                    requireConfigEquals(engine.config, fromFile);
                }
            }
        }
    }
}

SCENARIO("default initialization starts Host because channel id is 0", "[acceptance][bdd][config]")
{
    GIVEN("an Engine using default config (channel id 0)")
    {
        Engine engine;
        REQUIRE(engine.config.channelId == 0);
        engine.showWindow = false;

        WHEN("the Engine initializes headless")
        {
            REQUIRE(engine.init());

            THEN("both Host and IG are started")
            {
                REQUIRE(engine.synchronSystem().hasHost());
                REQUIRE(engine.synchronSystem().hasIg());
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
                REQUIRE(engine.config.channelId > 0);
            }
        }
    }
}

// =============================================================================
// 验收：init 后同步角色与配置一致（运行时地址对齐 config，而非写死端口）
// =============================================================================

SCENARIO("channel id 0 starts Host and IG using configured addresses", "[acceptance][bdd][config]")
{
    GIVEN("an Engine loaded from the main channel file (channel id 0)")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("main.json")));
        REQUIRE(engine.config.channelId == 0);
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("IG and Host run with addresses from the loaded config")
            {
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE(engine.synchronSystem().hasHost());
                REQUIRE(addressEquals(engine.synchronSystem().igSync().addressConfig(), engine.config.igLocal));
                REQUIRE(addressEquals(engine.synchronSystem().hostSync().addressConfig(), engine.config.hostLocal));
            }
        }
    }
}

SCENARIO("non-zero channel id starts IG only and does not start Host", "[acceptance][bdd][config]")
{
    GIVEN("an Engine loaded from the left channel file (channel id > 0)")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("left.json")));
        REQUIRE(engine.config.channelId > 0);
        engine.showWindow = false;

        WHEN("the Engine initializes")
        {
            REQUIRE(engine.init());

            THEN("IG is started and Host is not")
            {
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE_FALSE(engine.synchronSystem().hasHost());
                REQUIRE(addressEquals(engine.synchronSystem().igSync().addressConfig(), engine.config.igLocal));
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

// =============================================================================
// 单元：命令行解析（工具函数，非交付验收故事）
// =============================================================================

TEST_CASE("resolveConfigPath defaults to main.json when -c is omitted", "[unit][config][cli]")
{
    char arg0[] = "engine.exe";
    char* argv[] = {arg0};
    REQUIRE(Engine::resolveConfigPath(1, argv) == configPath("main.json"));
}

TEST_CASE("resolveConfigPath uses the path after -c", "[unit][config][cli]")
{
    char arg0[] = "engine.exe";
    char argC[] = "-c";
    std::string leftPath = configPath("left.json");
    char* argv[] = {arg0, argC, leftPath.data()};
    REQUIRE(Engine::resolveConfigPath(3, argv) == leftPath);
}

// =============================================================================
// 验收：窗口几何与通道配置一致（一 Scenario 一事）
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
