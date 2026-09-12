#include "Entity.h"

#include "function/config/EngineConfig.h"

#include "CigiBaseEntityCtrl.h"
#include "CigiEntityCtrlV4.h"

namespace
{
    vsg::dvec3 toDVec3(const Vec3Config& v)
    {
        return vsg::dvec3{v.x, v.y, v.z};
    }

    /// R = Rz(yaw)*Rx(pitch)*Ry(roll) 的 3×3（与 Engine::setCameraPose 同一轴序）。
    vsg::dmat4 rotationMatrixYpr(const vsg::dvec3& eulerYprDeg)
    {
        const vsg::dquat qRoll(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dquat qPitch(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0));
        const vsg::dquat qYaw(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0));
        const auto rotate = [&](const vsg::dvec3& v) { return qYaw * (qPitch * (qRoll * v)); };
        const vsg::dvec3 x = rotate(vsg::dvec3(1.0, 0.0, 0.0));
        const vsg::dvec3 y = rotate(vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 z = rotate(vsg::dvec3(0.0, 0.0, 1.0));
        vsg::dmat4 m = vsg::dmat4(1.0);
        m(0, 0) = x.x;
        m(0, 1) = x.y;
        m(0, 2) = x.z;
        m(1, 0) = y.x;
        m(1, 1) = y.y;
        m(1, 2) = y.z;
        m(2, 0) = z.x;
        m(2, 1) = z.y;
        m(2, 2) = z.z;
        return m;
    }
} // namespace

Entity Entity::assemble(const EntityConfig& cfg, vsg::ref_ptr<vsg::Node> geometry,
                        vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid, vsg::Group& root)
{
    Entity entity;
    entity._id = cfg.id;
    entity._name = cfg.name;
    entity._path = cfg.model;
    entity.fillPoseFromConfig(cfg, ellipsoid);
    entity._node = geometry;

    auto transform = vsg::MatrixTransform::create();
    transform->addChild(geometry);
    entity._transform = transform;
    entity.recomputeTransform();

    const bool initiallyVisible = cfg.initialEntityState != EntityInitialState::STANDBY;
    auto visibility = vsg::Switch::create();
    visibility->addChild(initiallyVisible, transform);
    entity._visibility = visibility;
    root.addChild(visibility);
    return entity;
}

bool Entity::visible() const
{
    if (!_visibility)
        return false;
    const auto& children = _visibility->children;
    if (children.empty())
        return false;
    return children.front().mask != vsg::MASK_OFF;
}

void Entity::samplePose(vsg::dvec3& positionOrLla, vsg::dvec3& eulerYprDeg) const
{
    positionOrLla = _positionOrLla;
    eulerYprDeg = _eulerYprDeg;
}

void Entity::applyCtrl(const CigiEntityCtrlV4& ctrl)
{
    if (_visibility)
        _visibility->setAllChildren(ctrl.GetEntityState() == CigiBaseEntityCtrl::Active);

    _alpha = ctrl.GetAlpha();
    _inheritAlpha = ctrl.GetInheritAlpha() == CigiBaseEntityCtrl::Inherit;
    _collisionDetectEn = ctrl.GetCollisionDetectEn() == CigiBaseEntityCtrl::Enable;
    _smoothingEn = ctrl.GetSmoothingEn();
}

void Entity::setPoseLla(const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg)
{
    _positionOrLla = lla;
    _eulerYprDeg = eulerYprDeg;
    recomputeTransform();
}

void Entity::fillPoseFromConfig(const EntityConfig& cfg, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid)
{
    _ellipsoidModel = ellipsoid;
    if (_ellipsoidModel)
    {
        _positionOrLla = toDVec3(cfg.ellipsoidPose.lla);
        _eulerYprDeg = toDVec3(cfg.ellipsoidPose.eulerYprDeg);
        return;
    }
    _positionOrLla = toDVec3(cfg.localPose.position);
    _eulerYprDeg = toDVec3(cfg.localPose.eulerYprDeg);
}

void Entity::recomputeTransform()
{
    if (!_transform)
        return;
    if (_ellipsoidModel)
        _transform->matrix =
            _ellipsoidModel->computeLocalToWorldTransform(_positionOrLla) * rotationMatrixYpr(_eulerYprDeg);
    else
        _transform->matrix = vsg::translate(_positionOrLla) * rotationMatrixYpr(_eulerYprDeg);
}
