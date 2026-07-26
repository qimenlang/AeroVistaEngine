#include "SynchronSystem.h"

#include <iostream>

SynchronSystem::SynchronSystem() = default;

SynchronSystem::~SynchronSystem()
{
    Shutdown();
}

bool SynchronSystem::Initialize(const SyncRoleConfig& role)
{
    Shutdown();
    _role = role;

    if (role.enableHost)
    {
        _host = std::make_unique<HostSync>();
        if (!_host->Initialize(role.hostLocal))
        {
            std::cerr << "SynchronSystem: HostSync Initialize failed\n";
            Shutdown();
            return false;
        }
        _host->SetPaceConfig(SyncPaceConfig{});
        _host->Run();
    }

    if (role.enableIg)
    {
        _ig = std::make_unique<IgSync>();
        if (!_ig->Initialize(role.igLocal))
        {
            std::cerr << "SynchronSystem: IgSync Initialize failed\n";
            Shutdown();
            return false;
        }

        if (!_ig->Connect(role.hostEndpoint))
        {
            std::cerr << "SynchronSystem: IgSync Connect failed\n";
            Shutdown();
            return false;
        }
    }

    return true;
}

void SynchronSystem::Shutdown()
{
    if (_ig)
    {
        _ig->Shutdown();
        _ig.reset();
    }
    if (_host)
    {
        _host->Shutdown();
        _host.reset();
    }
    _role = {};
}

void SynchronSystem::preFrame()
{
    if (_ig)
        _ig->Update(/*sendSof=*/true);
}

void SynchronSystem::postFrame(double simTimeMs)
{
    if (_host)
        _host->Update(simTimeMs);
}

HostSync& SynchronSystem::hostSync()
{
    return *_host;
}

IgSync& SynchronSystem::igSync()
{
    return *_ig;
}
