#pragma once

#include <vsg/all.h>

#include <cstdint>
#include <string>

class CigiEntityCtrlV4;
struct EntityConfig;

/// IG 侧运行时实体：场景图实例 + 语义位姿 / 显隐 / 属性。
/// 存在性由启动预建写入 Engine 表；本类不负责 loadScene / compile。
class Entity
{
public:
    /// 建 transform + Switch，挂到 `root`，按配置 pose 与 `initialEntityState` 初始化。
    static Entity assemble(const EntityConfig& cfg, vsg::ref_ptr<vsg::Node> geometry,
                           vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid, vsg::Group& root);

    int id() const { return _id; }
    const std::string& name() const { return _name; }
    const std::string& path() const { return _path; }

    /// 读 Switch mask；无 Switch 为 false。
    bool visible() const;
    std::uint8_t alpha() const { return _alpha; }
    bool inheritAlpha() const { return _inheritAlpha; }
    bool collisionDetectEn() const { return _collisionDetectEn; }
    bool smoothingEn() const { return _smoothingEn; }
    vsg::ref_ptr<vsg::Node> node() const { return _node; }
    vsg::ref_ptr<vsg::MatrixTransform> transform() const { return _transform; }

    void samplePose(vsg::dvec3& positionOrLla, vsg::dvec3& eulerYprDeg) const;

    /// EntityCtrl：切显隐 + 套属性（Destroyed 非 Active → Switch OFF）。
    void applyCtrl(const CigiEntityCtrlV4& ctrl);

    /// 命令面摆放，恒 LLA。transform 由 assemble 恒建。
    void setPoseLla(const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg);

private:
    void fillPoseFromConfig(const EntityConfig& cfg, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid);
    void recomputeTransform();

    int _id = 0;
    std::string _name;
    std::string _path;
    /// 非空：`_positionOrLla` 为 LLA、`_eulerYprDeg` 为当地 ENU；空：本地笛卡尔。
    vsg::ref_ptr<vsg::EllipsoidModel> _ellipsoidModel;
    vsg::dvec3 _positionOrLla{};
    vsg::dvec3 _eulerYprDeg{};
    vsg::ref_ptr<vsg::Node> _node;
    vsg::ref_ptr<vsg::MatrixTransform> _transform;
    vsg::ref_ptr<vsg::Switch> _visibility;
    /// CIGI Alpha：0 透明 .. 255 不透明；缺省不透明（报文未到前）。
    std::uint8_t _alpha = 255;
    bool _inheritAlpha = false;
    bool _collisionDetectEn = false;
    bool _smoothingEn = false;
};
