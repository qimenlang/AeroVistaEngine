#pragma once

// 通道 / 实体 JSON 的结构体和解析在 AeroVistaConfig。这里只把引擎用到的名字引进全局。
#include <aerovista/config/EngineChannelConfig.h>

using aerovista::config::CameraConfig;
using aerovista::config::EllipsoidPoseConfig;
using aerovista::config::EngineChannelConfig;
using aerovista::config::EntityConfig;
using aerovista::config::EntityInitialState;
using aerovista::config::GroundGridConfig;
using aerovista::config::IgConfig;
using aerovista::config::loadEngineChannelConfig;
using aerovista::config::loadEntitiesFile;
using aerovista::config::LocalPoseConfig;
using aerovista::config::OffsetDeg;
using aerovista::config::Vec3Config;
using aerovista::config::ViewDefinition;
using aerovista::config::WindowConfig;
