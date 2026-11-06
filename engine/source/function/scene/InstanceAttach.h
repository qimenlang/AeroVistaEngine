#pragma once

#include <vsg/all.h>

/// 把植被替身挂到场景根。模型文件读不到时不挂，调用方的 init 不因此失败。
void attachInstanceModel(vsg::Group& scene, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid,
                         vsg::ref_ptr<vsg::Options> options);
