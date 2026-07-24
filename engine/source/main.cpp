#include "engine.h"

#include <iostream>

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

int main()
{
    Engine engine;

    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models/lz.vsgt";

    if (!engine.init(modelPath))
    {
        std::cerr << "Engine init failed for " << modelPath << std::endl;
        return 1;
    }

    engine.run();
    return 0;
}
