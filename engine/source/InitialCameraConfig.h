#pragma once

namespace initial_camera
{
    // Shared framing parameters used by the engine and camera acceptance tests.
    inline constexpr double kRadiusThreshold = 0.1;
    inline constexpr double kLocalBackMultiplier = 3.5;
    inline constexpr double kEllipsoidBackMultiplier = 3.5;
    inline constexpr double kEllipsoidUpMultiplier = 0.3;
    inline constexpr double kLocalFarMultiplier = 4.5;
    inline constexpr double kNearFarRatio = 0.001;
    inline constexpr double kFieldOfViewDegrees = 30.0;
    inline constexpr double kEllipsoidHorizonMountainHeight = 0.0;
} // namespace initial_camera
