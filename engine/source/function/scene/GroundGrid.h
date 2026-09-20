#pragma once

#include "function/config/EngineConfig.h"

#include <vsg/all.h>

/// 场景图上标记接缝网格父节点，供测试查找（非 JSON 字段）。
inline constexpr const char* kGroundGridObjectKey = "aerovista.groundGrid";

/// 沿东/北各方向的格线数（含两端），= cellCount + 1。
int groundGridLineCount(double halfExtentM, double cellM);

/// 本地 ENU 切平面上的线网格（X=东、Y=北），尚未钉到 ECEF。
vsg::ref_ptr<vsg::Node> createEnuGroundGrid(const GroundGridConfig& spec);

/// `LocalToWorld(lla)` 把切平面钉到椭球；父节点带 `kGroundGridObjectKey`。
vsg::ref_ptr<vsg::MatrixTransform> createPinnedGroundGrid(const vsg::EllipsoidModel& ellipsoid,
                                                          const GroundGridConfig& spec);
