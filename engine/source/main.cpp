#include "engine.h"

#include <iostream>

int main(int argc, char** argv)
{
    Engine engine;

    const std::string configPath = Engine::resolveConfigPath(argc, argv);
    if (!engine.loadConfig(configPath))
    {
        std::cerr << "Engine loadConfig failed for " << configPath << std::endl;
        return 1;
    }

    if (!engine.init())
    {
        std::cerr << "Engine init failed (config=" << configPath << ", model=" << engine.config.model << ")\n";
        return 1;
    }

    engine.run();
    return 0;
}
