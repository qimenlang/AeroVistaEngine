#pragma once

#include <vsg/all.h>

/// 把 BlackAlder LOD1–LOD6 挂到一个 vsg::LOD 下，按屏幕高度占比切换。
/// 任一档加载失败则整组不挂，调用方的 init 不因此失败。
void attachLodModel(vsg::Group& scene, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid,
                    vsg::ref_ptr<vsg::Options> options);
