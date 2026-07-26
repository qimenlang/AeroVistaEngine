#include <catch2/catch_test_macros.hpp>

#include "function/sync/HostSync.h"
#include "function/sync/IgSync.h"

// 1.HostSync和IgSync的初始化、连接测试
namespace
{
    // Defaults from doc/多通道同步模块设计.md:
    //   HostSync UDP: send → 8001, recv ← 8000
    //   IgSync   UDP: send → 8000, recv ← 8001
    //   TCP command: 8100
    // Initialize(local) = local bind / listen identity
    // IgSync::Connect(hostEndpoint) only

    AddressConfig makeHostLocal(const std::string& addr = "127.0.0.1")
    {
        return AddressConfig{addr, 8001, 8000, 8100};
    }

    AddressConfig makeIgLocal(const std::string& localAddr = "127.0.0.1", int udpRecvPort = 8001)
    {
        // udpPortSend=8000 → Host recv; udpPortRecv = local bind (unique per co-located IG)
        return AddressConfig{localAddr, 8000, udpRecvPort, 8100};
    }

    AddressConfig makeHostEndpoint(const std::string& addr = "127.0.0.1")
    {
        return AddressConfig{addr, 8001, 8000, 8100};
    }
} // namespace

SCENARIO("HostSync or IgSync initializes successfully", "[bdd][sync][initialize]")
{
    GIVEN("a new HostSync")
    {
        HostSync host;

        WHEN("it is initialized")
        {
            const bool ok = host.Initialize(makeHostLocal());

            THEN("initialization succeeds and no IG is ready yet")
            {
                REQUIRE(ok);
                REQUIRE_FALSE(host.hasReadyIg());
                REQUIRE(host.readyIgCount() == 0);
            }
        }
    }

    GIVEN("a new IgSync")
    {
        IgSync ig;

        WHEN("it is initialized")
        {
            const bool ok = ig.Initialize(makeIgLocal());

            THEN("initialization succeeds and it is not connected to a Host yet")
            {
                REQUIRE(ok);
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

SCENARIO("IgSync connect fails when Host is not running", "[bdd][sync][connect][failure]")
{
    GIVEN("an IgSync initialized without a running HostSync")
    {
        IgSync ig;
        REQUIRE(ig.Initialize(makeIgLocal()));

        WHEN("the IG connects to a Host endpoint")
        {
            const bool connected = ig.Connect(makeHostEndpoint());

            THEN("connect fails and neither plane reports connected")
            {
                REQUIRE_FALSE(connected);
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

SCENARIO("IgSync connect fails then succeeds after HostSync starts", "[bdd][sync][connect][reconnect]")
{
    GIVEN("an IgSync initialized without a running HostSync")
    {
        IgSync ig;
        REQUIRE(ig.Initialize(makeIgLocal()));

        WHEN("the IG connects while the Host is down")
        {
            REQUIRE_FALSE(ig.Connect(makeHostEndpoint()));
            REQUIRE_FALSE(ig.tcpConnected());
            REQUIRE_FALSE(ig.udpSynced());

            AND_WHEN("the HostSync is initialized and the IG connects again")
            {
                HostSync host;
                REQUIRE(host.Initialize(makeHostLocal()));

                const bool connected = ig.Connect(makeHostEndpoint());

                THEN("connect succeeds on both planes and Host sees a ready IG")
                {
                    REQUIRE(connected);
                    REQUIRE(ig.tcpConnected());
                    REQUIRE(ig.udpSynced());
                    REQUIRE(host.hasReadyIg());
                    REQUIRE(host.readyIgCount() == 1);
                }
            }
        }
    }
}

SCENARIO("IgSync connects to an initialized HostSync successfully", "[bdd][sync][connect]")
{
    GIVEN("a HostSync that has been initialized and is waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.Initialize(makeHostLocal()));
        REQUIRE_FALSE(host.hasReadyIg());

        AND_GIVEN("an IgSync that has been initialized")
        {
            IgSync ig;
            REQUIRE(ig.Initialize(makeIgLocal()));

            WHEN("the IG connects to the Host endpoint")
            {
                // HostSync does not call Connect; only IgSync initiates.
                const bool connected = ig.Connect(makeHostEndpoint());

                THEN("IG reports TCP and UDP sync, and Host has one ready IG")
                {
                    REQUIRE(connected);
                    REQUIRE(ig.tcpConnected());
                    REQUIRE(ig.udpSynced());
                    REQUIRE(host.hasReadyIg());
                    REQUIRE(host.readyIgCount() == 1);
                }
            }
        }
    }
}

SCENARIO("IgSync connect fails when UDP peer ports are wrong but TCP port is valid", "[bdd][sync][connect][failure]")
{
    GIVEN("a HostSync waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.Initialize(makeHostLocal()));

        AND_GIVEN("an IgSync initialized with correct local ports")
        {
            IgSync ig;
            REQUIRE(ig.Initialize(makeIgLocal()));

            WHEN("the IG connects using a Host endpoint with wrong UDP ports")
            {
                // TCP 8100 still matches Host; UDP recv port 9999 will not sync.
                AddressConfig badUdpEndpoint{"127.0.0.1", 8001, 9999, 8100};
                const bool connected = ig.Connect(badUdpEndpoint);

                THEN("overall connect fails and UDP is not synced")
                {
                    REQUIRE_FALSE(connected);
                    REQUIRE_FALSE(ig.udpSynced());
                    REQUIRE_FALSE(host.hasReadyIg());
                }
            }
        }
    }
}

SCENARIO("HostSync accepts multiple IgSync connections", "[bdd][sync][connect][multi-ig]")
{
    GIVEN("a HostSync waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.Initialize(makeHostLocal()));

        WHEN("two co-located IGs initialize on distinct UDP recv ports and connect")
        {
            // Same machine cannot bind two IGs to UDP 8001; use 8001 and 8003.
            // Host learns each IG recv port when the IG connects (e.g. via TCP handshake).
            IgSync ig1;
            IgSync ig2;
            REQUIRE(ig1.Initialize(makeIgLocal("127.0.0.1", 8001)));
            REQUIRE(ig2.Initialize(makeIgLocal("127.0.0.1", 8003)));

            REQUIRE(ig1.Connect(makeHostEndpoint()));
            REQUIRE(ig2.Connect(makeHostEndpoint()));

            THEN("both IGs are synced and Host reports two ready IGs")
            {
                REQUIRE(ig1.tcpConnected());
                REQUIRE(ig1.udpSynced());
                REQUIRE(ig2.tcpConnected());
                REQUIRE(ig2.udpSynced());
                REQUIRE(host.readyIgCount() == 2);
            }
        }
    }
}

// 2.host ig 状态同步测试