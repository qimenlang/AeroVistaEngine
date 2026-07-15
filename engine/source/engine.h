#pragma once

#include <vsg/all.h>

class Engine
{
public:
    bool init(const vsg::Path& modelPath);
    void run();

private:
    vsg::ref_ptr<vsg::Options> options;
    vsg::ref_ptr<vsg::Viewer> viewer;
    vsg::ref_ptr<vsg::Node> scene;
};
