#pragma once
#include "ObjectSelection.h"
#include "../../MeshCutting/Geometry/CutPlane.h"
#include <map>

namespace Viewer
{
    class CutPlaneSystem
    {
    public:
        // 객체의 수명과 TRS를 소유하는 장면을 연결한다.
        explicit CutPlaneSystem(Scene& scene) : m_Scene(scene) {}
        // 검증한 크기의 모델 없는 평면 객체를 생성한다.
        ObjectId CreatePlane(const MeshCutting::CutPlaneDesc& desc, const SceneTransform& transform);
        // 평면 설명과 장면 객체를 함께 제거한다.
        bool RemovePlane(ObjectId id);
        // 살아 있는 평면의 로컬 크기만 갱신한다.
        bool SetSize(ObjectId id, const MeshCutting::CutPlaneDesc& desc);
        // 삭제된 객체는 평면으로 반환하지 않는다.
        const MeshCutting::CutPlaneDesc* Find(ObjectId id) const;
        // 최신 객체 TRS로 불변 계산 스냅샷을 만든다.
        bool BuildData(ObjectId id, MeshCutting::CutPlaneData& data) const;
        // 모델과 같은 월드 거리 기준으로 가장 가까운 평면을 고른다.
        PickHit Pick(const PickingRay& ray) const;
        // 일반 장면 삭제로 남은 설명을 프레임 시작에 정리한다.
        void PruneStaleObjects();
        // 렌더러가 살아 있는 평면을 순회할 설명 목록을 읽는다.
        const std::map<ObjectId, MeshCutting::CutPlaneDesc>& GetPlanes() const { return m_Planes; }

    private:
        Scene& m_Scene;
        std::map<ObjectId, MeshCutting::CutPlaneDesc> m_Planes;
    };
}
