#pragma once

#include <vsg/all.h>

/// 场景图上标记 shader cube 根节点，供测试查找（非 JSON 字段）。
inline constexpr const char* kShaderCubeObjectKey = "aerovista.shaderCube";

/// 单位立方体，fragment 来自可编辑 GLSL；失败（读文件 / 无 glslang / 编译失败）返回空。
vsg::ref_ptr<vsg::Node> createShaderCube(const vsg::Path& fragmentPath, vsg::ref_ptr<vsg::Options> options);
