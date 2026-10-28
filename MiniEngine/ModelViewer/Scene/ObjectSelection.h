#pragma once

#include "Scene.h"
#include "EditTarget.h"
#include "TransformHistory.h"

namespace Viewer
{
    class PhysicsSystem;
    // 편집 대상의 TRS와 물리 body를 함께 갱신한다.
    bool ApplyEditTransform(Scene& scene, PhysicsSystem* physics, const EditTarget& target, const SceneTransform& world);
    // 대상 유효성과 변환 모드를 확인한 뒤 공통 편집 경로로 이력을 복구한다.
    bool ApplyHistoryCommand(Scene& scene, PhysicsSystem* physics, const TransformHistory::Command& command);
    struct PickingRay
    {
        Math::Vector3 Origin;
        Math::Vector3 Direction;
    };

    // Coordinates are normalized across the full rendered viewport, with Y down.
    PickingRay MakePickingRay(const Math::Camera& camera, float x, float y);
    // 모델 구의 교점을 월드 거리와 함께 반환한다.
    PickHit PickObject(const Scene& scene, const PickingRay& ray);
    // pick 결과를 R8 규칙(월드 독립 컴포넌트는 Mesh, 그 외는 Owner)의 편집 대상으로 바꾼다.
    EditTarget ResolveEditTarget(const Scene& scene, const PickHit& hit);
    // 대상 객체와 컴포넌트가 살아 있고 종류와 맞는지 확인한다.
    bool IsEditTargetValid(const Scene& scene, const EditTarget& target);
    // 기즈모와 표시가 쓰는 대상의 월드 TRS를 구한다. owner는 편집 TRS, Mesh는 컴포넌트 월드 TRS다.
    bool TryGetEditTargetTransform(const Scene& scene, const EditTarget& target, SceneTransform& out);
}
