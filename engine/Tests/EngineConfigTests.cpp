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

    void requireAddressEquals(const AddressConfig& actual,
                              const char* addr,
                              int udpPortSend,
                              int udpPortRecv,
                              int tcpPort)
    {
        REQUIRE(actual.addr == addr);
        REQUIRE(actual.udpPortSend == udpPortSend);
        REQUIRE(actual.udpPortRecv == udpPortRecv);
        REQUIRE(actual.tcpPort == tcpPort);
    }
} // namespace

// =============================================================================
// Engine 默认配置 = main.json；无 -c 时 init 后带 Host
// 目标 API（待实现）：构造/默认即填充 engine.config；Engine::init() 按默认 config 启 Host+IG
// =============================================================================

SCENARIO("Engine init with defaults matches main.json and starts Host", "[bdd][config][sync]")
{
    GIVEN("an Engine with no explicit loadConfig (defaults = main.json)")
    {
        Engine engine;

        WHEN("Engine is constructed")
        {
            THEN("engine.config matches main.json fields")
            {
                const auto& cfg = engine.config;

                REQUIRE(cfg.channelId == 0);

                REQUIRE(nearlyEqual(cfg.offsetDeg.yaw, 0.0));
                REQUIRE(nearlyEqual(cfg.offsetDeg.pitch, 0.0));
                REQUIRE(nearlyEqual(cfg.offsetDeg.roll, 0.0));

                requireAddressEquals(cfg.igLocal, "127.0.0.1", 8000, 8001, 8100);
                requireAddressEquals(cfg.hostEndpoint, "127.0.0.1", 8001, 8000, 8100);
                requireAddressEquals(cfg.hostLocal, "127.0.0.1", 8001, 8000, 8100);

                REQUIRE(cfg.model == "models/lz.vsgt");
                REQUIRE(cfg.window.x == 640);
                REQUIRE(cfg.window.y == 0);
                REQUIRE(cfg.window.width == 640);
                REQUIRE(cfg.window.height == 1080);
                REQUIRE(cfg.hostEyeStalePolicy == HostEyeStalePolicy::REUSE_LAST);
            }
        }

        AND_WHEN("Engine initializes with default config (headless)")
        {
            engine.showWindow = false;
            REQUIRE(engine.init());

            THEN("Host and IG are started with addresses from default config")
            {
                REQUIRE(engine.synchronSystem().hasHost());
                REQUIRE(engine.synchronSystem().hasIg());

                requireAddressEquals(engine.synchronSystem().hostSync().addressConfig(),
                                     "127.0.0.1",
                                     8001,
                                     8000,
                                     8100);
                requireAddressEquals(engine.synchronSystem().igSync().addressConfig(),
                                     "127.0.0.1",
                                     8000,
                                     8001,
                                     8100);
            }
        }
    }
}

// =============================================================================
// Engine 加载通道 JSON 后，engine.config 与文件字段一致
// 目标 API（待实现）：Engine::loadConfig(path) → bool；Engine::config 保存解析结果
// =============================================================================

SCENARIO("Engine loadConfig(main.json) fills engine.config to match file", "[bdd][config]")
{
    GIVEN("an Engine and path to main.json")
    {
        Engine engine;
        const std::string path = configPath("main.json");

        WHEN("Engine loads the config file")
        {
            REQUIRE(engine.loadConfig(path));

            THEN("engine.config matches main.json fields")
            {
                const auto& cfg = engine.config;

                REQUIRE(cfg.channelId == 0);

                REQUIRE(nearlyEqual(cfg.offsetDeg.yaw, 0.0));
                REQUIRE(nearlyEqual(cfg.offsetDeg.pitch, 0.0));
                REQUIRE(nearlyEqual(cfg.offsetDeg.roll, 0.0));

                requireAddressEquals(cfg.igLocal, "127.0.0.1", 8000, 8001, 8100);
                requireAddressEquals(cfg.hostEndpoint, "127.0.0.1", 8001, 8000, 8100);
                requireAddressEquals(cfg.hostLocal, "127.0.0.1", 8001, 8000, 8100);

                REQUIRE(cfg.model == "models/lz.vsgt");
                REQUIRE(cfg.window.x == 640);
                REQUIRE(cfg.window.y == 0);
                REQUIRE(cfg.window.width == 640);
                REQUIRE(cfg.window.height == 1080);
                REQUIRE(cfg.hostEyeStalePolicy == HostEyeStalePolicy::REUSE_LAST);
            }
        }
    }
}

// =============================================================================
// loadConfig + init 后：IgSync 地址与配置一致；配置端口号为 0 时启动 Host 且地址一致
// 目标 API（待实现）：
//   Engine::loadConfig / Engine::init（按 config 初始化 SynchronSystem）
//   IgSync::addressConfig() / HostSync::addressConfig()
//   SynchronSystem::hasHost()
// =============================================================================

SCENARIO("after loadConfig and init, IgSync AddressConfig matches config; port 0 starts Host",
         "[bdd][config][sync]")
{
    GIVEN("an Engine and main.json whose port/channel id is 0 (starts Host)")
    {
        Engine engine;
        const std::string path = configPath("main.json");

        WHEN("Engine loads the config and initializes")
        {
            REQUIRE(engine.loadConfig(path));
            REQUIRE(engine.config.channelId == 0); // 约定：端口号/通道号为 0 → 启动 Host
            engine.showWindow = false;
            REQUIRE(engine.init());

            THEN("IgSync AddressConfig matches config.igLocal")
            {
                REQUIRE(engine.synchronSystem().hasIg());
                requireAddressEquals(engine.synchronSystem().igSync().addressConfig(),
                                     engine.config.igLocal.addr.c_str(),
                                     engine.config.igLocal.udpPortSend,
                                     engine.config.igLocal.udpPortRecv,
                                     engine.config.igLocal.tcpPort);
            }

            AND_THEN("Host is started and HostSync AddressConfig matches config.hostLocal")
            {
                REQUIRE(engine.synchronSystem().hasHost());
                requireAddressEquals(engine.synchronSystem().hostSync().addressConfig(),
                                     engine.config.hostLocal.addr.c_str(),
                                     engine.config.hostLocal.udpPortSend,
                                     engine.config.hostLocal.udpPortRecv,
                                     engine.config.hostLocal.tcpPort);
            }
        }
    }
}

SCENARIO("after loadConfig and init with non-zero port, IgSync matches config and Host is not started",
         "[bdd][config][sync]")
{
    GIVEN("an Engine and left.json whose port/channel id is greater than 0")
    {
        Engine engine;
        const std::string path = configPath("left.json");

        WHEN("Engine loads the config and initializes")
        {
            REQUIRE(engine.loadConfig(path));
            REQUIRE(engine.config.channelId > 0);
            engine.showWindow = false;
            REQUIRE(engine.init());

            THEN("IgSync AddressConfig matches config.igLocal")
            {
                REQUIRE(engine.synchronSystem().hasIg());
                requireAddressEquals(engine.synchronSystem().igSync().addressConfig(),
                                     engine.config.igLocal.addr.c_str(),
                                     engine.config.igLocal.udpPortSend,
                                     engine.config.igLocal.udpPortRecv,
                                     engine.config.igLocal.tcpPort);
            }

            AND_THEN("Host is not started")
            {
                REQUIRE_FALSE(engine.synchronSystem().hasHost());
            }

            AND_THEN("SynchronSystem uses config offsetDeg and hostEyeStalePolicy")
            {
                REQUIRE(nearlyEqual(engine.synchronSystem().offsetDeg().yaw, engine.config.offsetDeg.yaw));
                REQUIRE(nearlyEqual(engine.synchronSystem().offsetDeg().pitch, engine.config.offsetDeg.pitch));
                REQUIRE(nearlyEqual(engine.synchronSystem().offsetDeg().roll, engine.config.offsetDeg.roll));
                REQUIRE(engine.synchronSystem().hostEyeStalePolicy() == engine.config.hostEyeStalePolicy);
            }
        }
    }
}

// =============================================================================
// 命令行：engine.exe [-c pathToConfig]；无 -c 时默认 main.json
// 目标 API（待实现）：Engine::resolveConfigPath(argc, argv) → 配置文件路径
// =============================================================================

SCENARIO("command line -c selects config path; omit -c defaults to main.json", "[bdd][config][cli]")
{
    GIVEN("argv with only the program name")
    {
        char arg0[] = "engine.exe";
        char* argvNoArgs[] = {arg0};
        const int argcNoArgs = 1;

        WHEN("resolveConfigPath parses the command line")
        {
            const std::string path = Engine::resolveConfigPath(argcNoArgs, argvNoArgs);

            THEN("default config path is main.json")
            {
                REQUIRE(path == configPath("main.json"));
            }
        }
    }

    GIVEN("argv with -c and an explicit config path")
    {
        char arg0[] = "engine.exe";
        char argC[] = "-c";
        const std::string leftPath = configPath("left.json");
        std::string leftPathBuf = leftPath;
        char* argvWithC[] = {arg0, argC, leftPathBuf.data()};
        const int argcWithC = 3;

        WHEN("resolveConfigPath parses the command line")
        {
            const std::string path = Engine::resolveConfigPath(argcWithC, argvWithC);

            THEN("config path is the -c argument")
            {
                REQUIRE(path == leftPath);
            }
        }
    }
}

// =============================================================================
// init 后窗口 Traits / extent2D 与配置一致（需创建真实 window）
// =============================================================================

SCENARIO("after init, window traits x/y/width/height match config", "[bdd][config][window]")
{
    GIVEN("an Engine loaded from main.json with showWindow enabled")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("main.json")));
        engine.showWindow = true;

        WHEN("Engine initializes and creates a window")
        {
            REQUIRE(engine.init());

            THEN("WindowTraits match config.window")
            {
                auto win = engine.mainWindow();
                REQUIRE(win);
                auto traits = win->traits();
                REQUIRE(traits);

                REQUIRE(traits->x == engine.config.window.x);
                REQUIRE(traits->y == engine.config.window.y);
                REQUIRE(traits->width == static_cast<uint32_t>(engine.config.window.width));
                REQUIRE(traits->height == static_cast<uint32_t>(engine.config.window.height));
            }
        }
    }
}

SCENARIO("after init, window extent2D width/height match config", "[bdd][config][window]")
{
    GIVEN("an Engine loaded from main.json with showWindow enabled")
    {
        Engine engine;
        REQUIRE(engine.loadConfig(configPath("main.json")));
        engine.showWindow = true;

        WHEN("Engine initializes and creates a window")
        {
            REQUIRE(engine.init());

            THEN("extent2D matches config.window width and height")
            {
                auto win = engine.mainWindow();
                REQUIRE(win);

                const VkExtent2D extent = win->extent2D();
                REQUIRE(extent.width == static_cast<uint32_t>(engine.config.window.width));
                REQUIRE(extent.height == static_cast<uint32_t>(engine.config.window.height));
            }
        }
    }
}
