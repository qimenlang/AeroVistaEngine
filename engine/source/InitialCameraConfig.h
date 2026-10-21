#pragma once

namespace initial_camera
{
    // 未配置 camera.pose 时，用场景 AABB 的 centre / radius 取景（位姿配置设计.md §4）。
    // 引擎与相机验收测试共用。有 pose 只覆盖 LookAt；Host 眼点到达后仍会再覆盖 LookAt。
    // 投影（视场角、near/far）不随眼点包改变。

    // 半径低于此值（米）视为空场景：不再用半径乘倍数，改走绝对兜底眼点。
    // 本地兜底 eye=(0,0,10)；椭球另有「中心落入地底」判据，不在这个常数里。
    inline constexpr double kRadiusThreshold = 0.1;

    // 本地世界系眼点沿 −Y 退到 centre 后方，距离 = 倍数 × radius，使 30° 视场大致框住场景。
    inline constexpr double kLocalBackMultiplier = 3.5;

    // 椭球眼点沿当地北向后退，距离 = 倍数 × radius。数值与本地后退相同，方向是 ENU 北。
    inline constexpr double kEllipsoidBackMultiplier = 3.5;

    // 椭球眼点再沿当地天向抬高，距离 = 倍数 × radius，避免贴着目标水平看。
    inline constexpr double kEllipsoidUpMultiplier = 0.3;

    // 本地透视 far = 倍数 × radius。有 camera.pose 且眼点离 AABB 更远时，far 会再抬到能包住场景。
    inline constexpr double kLocalFarMultiplier = 4.5;

    // 垂直视场角与近远比来自配置 viewDefinition.vfov / near / far（缺省 30° 与 1/1000）。
    // 本地 near = (near/far) × radius；椭球 near = 每帧动态 far × (near/far)。

    // 椭球远裁在地平线之外再留出的山高（米）。0 表示远裁贴地平线。
    // 须显式传入：VSG EllipsoidPerspective 缺省 horizonMountainHeight 为 1000 m。
    inline constexpr double kEllipsoidHorizonMountainHeight = 0.0;
} // namespace initial_camera
