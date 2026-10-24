#include "CutPlaneSystem.h"

namespace Viewer
{
    // 잘못된 설명이나 TRS는 객체를 만들기 전에 거절한다.
    ObjectId CutPlaneSystem::CreatePlane(const MeshCutting::CutPlaneDesc& desc, const SceneTransform& transform)
    {
        SceneTransform valid = transform;
        MeshCutting::CutPlaneData data;
        if (!valid.Normalize() || !MeshCutting::TryBuildCutPlaneData(desc, valid.ToAffine(), data))
            return InvalidObjectId;
        GameObject& object = m_Scene.CreateObject("Cut Plane");
        object.SetTransform(valid);
        m_Planes.emplace(object.GetId(), desc);
        return object.GetId();
    }

    // 등록된 평면만 제거해 일반 모델 삭제를 방지한다.
    bool CutPlaneSystem::RemovePlane(ObjectId id)
    {
        if (m_Planes.erase(id) == 0)
            return false;
        m_Scene.DeleteObject(id);
        return true;
    }

    // 크기 변경 후에도 현재 변환으로 유효한 스냅샷을 만들 수 있어야 한다.
    bool CutPlaneSystem::SetSize(ObjectId id, const MeshCutting::CutPlaneDesc& desc)
    {
        const GameObject* object = m_Scene.FindObject(id);
        MeshCutting::CutPlaneData data;
        if (!Find(id) || !MeshCutting::TryBuildCutPlaneData(desc, object->GetTransform().ToAffine(), data))
            return false;
        m_Planes.at(id) = desc;
        return true;
    }

    // 장면 삭제 직후에도 오래된 등록 항목을 외부에 노출하지 않는다.
    const MeshCutting::CutPlaneDesc* CutPlaneSystem::Find(ObjectId id) const
    {
        const auto it = m_Planes.find(id);
        return it != m_Planes.end() && m_Scene.FindObject(id) ? &it->second : nullptr;
    }

    // 저장된 포인터 없이 ID로 객체를 찾아 현재 TRS를 계산에 반영한다.
    bool CutPlaneSystem::BuildData(ObjectId id, MeshCutting::CutPlaneData& data) const
    {
        const auto* desc = Find(id);
        return desc && MeshCutting::TryBuildCutPlaneData(*desc,
            m_Scene.FindObject(id)->GetTransform().ToAffine(), data);
    }

    // 모든 평면의 유한 직사각형 교점을 공통 선택 결과로 합친다.
    PickHit CutPlaneSystem::Pick(const PickingRay& ray) const
    {
        PickHit hit;
        for (const auto& plane : m_Planes)
        {
            MeshCutting::CutPlaneData data;
            float t;
            if (BuildData(plane.first, data) && MeshCutting::IntersectCutPlaneRectangle(data, ray.Origin, ray.Direction, t))
                hit = NearestPickHit(hit, {plane.first, t});
        }
        return hit;
    }

    // 일반 DeleteObject와 Clear가 남긴 등록 항목을 안전하게 지운다.
    void CutPlaneSystem::PruneStaleObjects()
    {
        for (auto it = m_Planes.begin(); it != m_Planes.end();)
            if (!m_Scene.FindObject(it->first))
                it = m_Planes.erase(it);
            else
                ++it;
    }
}
