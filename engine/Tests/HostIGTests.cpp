#include <catch2/catch_test_macros.hpp>

#include "function/sync/SynchronSystem.h"

SCENARIO("Host&IG initializes successfully", "[bdd][host][initialize]")
{
    GIVEN("a new SynchronSystem")
    {
        SynchronSystem synchronSystem;
        WHEN("the host is initialized")
        {
            HostIGConfig hostIGConfig = {HostIGType::HOST, "127.0.0.1", 8001, 8000};
            const bool hostInitialized = synchronSystem.Initialize(hostIGConfig);
            THEN("the host initializes successfully")
            {
                REQUIRE(hostInitialized);
            }
        }

        WHEN("the IG is initialized")
        {
            HostIGConfig hostIGConfig = {HostIGType::IG, "127.0.0.1", 8000, 8001};

            const bool igInitialized = synchronSystem.Initialize(hostIGConfig);
            THEN("the IG initializes successfully")
            {
                REQUIRE(igInitialized);
            }
        }
    }
}

SCENARIO("IG Connects to Host fails", "[bdd][ig][connect]")
{
    GIVEN("host and IG initialized")
    {
        SynchronSystem igSynchronSystem;
        HostIGConfig igIGConfig = {HostIGType::IG, "127.0.0.1", 8000, 8001};
        REQUIRE(igSynchronSystem.Initialize(igIGConfig));
        WHEN("the IG connects to the host")
        {
            const bool connected = igSynchronSystem.Connect();
            REQUIRE(!connected);
        }
    }
}

SCENARIO("IG Connects to Host fails and reconnects successfully", "[bdd][host][connect]")
{
    GIVEN("IG initialized")
    {
        SynchronSystem igSynchronSystem;
        HostIGConfig igIGConfig = {HostIGType::IG, "127.0.0.1", 8000, 8001};
        REQUIRE(igSynchronSystem.Initialize(igIGConfig));

        WHEN("the ig connects to the non-existent host")
        {
            const bool connected = igSynchronSystem.Connect();
            REQUIRE(!connected);
        }
        WHEN("the ig reconnects to the host")
        {
            SynchronSystem hostSynchronSystem;
            HostIGConfig hostIGConfig = {HostIGType::HOST, "127.0.0.1", 8001, 8000};
            REQUIRE(hostSynchronSystem.Initialize(hostIGConfig));

            const bool connected = igSynchronSystem.Connect();
            REQUIRE(connected);
        }
    }
}

SCENARIO("IG connects to host successfully", "[bdd][ig][connect]")
{
    GIVEN("host and IG initialized")
    {
        SynchronSystem hostSynchronSystem;
        HostIGConfig hostIGConfig = {HostIGType::HOST, "127.0.0.1", 8001, 8000};
        REQUIRE(hostSynchronSystem.Initialize(hostIGConfig));

        SynchronSystem igSynchronSystem;
        HostIGConfig igIGConfig = {HostIGType::IG, "127.0.0.1", 8000, 8001};
        REQUIRE(igSynchronSystem.Initialize(igIGConfig));

        WHEN("the IG connects to the host")
        {
            const bool connected = igSynchronSystem.Connect();
            REQUIRE(connected);
        }
    }
}