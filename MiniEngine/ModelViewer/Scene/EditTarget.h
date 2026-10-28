#pragma once
#include "ComponentId.h"
#include "PickHit.h"

namespace Viewer
{
    enum class EditTargetKind { Owner, Mesh };

    // 편집 대상은 owner 전체이거나 owner 안의 컴포넌트 하나다 (R8). Object가 0이면 선택 없음이다.
    struct EditTarget
    {
        EditTargetKind Kind = EditTargetKind::Owner;
        ObjectId Object = InvalidObjectId;
        ComponentId Mesh = InvalidComponentId;

        // owner 전체를 가리키는 대상을 만든다.
        static EditTarget MakeOwner(ObjectId object)
        {
            // 컴포넌트 ID 없이 owner만 담는다.
            return { EditTargetKind::Owner, object, InvalidComponentId };
        }

        // owner 안의 컴포넌트 하나를 가리키는 대상을 만든다.
        static EditTarget MakeMesh(ObjectId object, ComponentId mesh)
        {
            // owner와 컴포넌트 ID를 함께 담는다.
            return { EditTargetKind::Mesh, object, mesh };
        }

        // pick 결과를 R8 규칙으로 변환한다. 월드 독립 컴포넌트는 Mesh, 그 외는 Owner다.
        static EditTarget FromPick(const PickHit& hit, bool meshWorldIndependent)
        {
            // 빈 pick은 선택 없음이다.
            if (hit.Id == InvalidObjectId)
                return EditTarget();

            // 월드 독립 컴포넌트를 맞췄으면 그 컴포넌트가 대상이다.
            if (hit.MeshId != InvalidComponentId && meshWorldIndependent)
                return MakeMesh(hit.Id, hit.MeshId);

            // Attached 컴포넌트나 모델 없는 객체는 owner가 대상이다.
            return MakeOwner(hit.Id);
        }

        // 선택 없음인지 확인한다.
        bool IsNone() const { return Object == InvalidObjectId; }
        // 컴포넌트 대상인지 확인한다.
        bool IsMesh() const { return Object != InvalidObjectId && Kind == EditTargetKind::Mesh; }
        // 종류와 ID가 모두 같은지 비교한다.
        bool operator==(const EditTarget& other) const
        {
            // 세 필드를 모두 비교한다.
            return Kind == other.Kind && Object == other.Object && Mesh == other.Mesh;
        }
        // 두 대상이 다른지 확인한다.
        bool operator!=(const EditTarget& other) const
        {
            // 동등 비교를 뒤집는다.
            return !(*this == other);
        }
    };
}
