#pragma once

#include "ObjectSelection.h"
#include "GizmoMath.h"
#include "TransformHistory.h"

namespace Viewer
{
    class TransformGizmo
    {
    public:
        enum Mode { View, Translate, Rotate, Scale };
        // 허용된 크기 축을 표시·선택·드래그에 공통 적용하고 포인터 소비 여부를 반환한다.
        bool Update(Scene& scene, const EditTarget& target, const Math::Camera& camera, float width, float height,
            unsigned scaleAxisMask = 7);
        void Render(GraphicsContext& context, const Scene& scene, const EditTarget& target,
            const Math::Camera& camera, float width, float height) const;
        bool IsDragging() const { return m_DragId != InvalidObjectId; }
        Mode GetMode() const { return m_Mode; }
        // 크기는 항상 로컬 축이며 나머지 모드는 선택한 좌표계를 따른다.
        bool IsLocal() const { return m_Mode == Scale || m_Local; }
        // Mesh 대상 편집이 막혔을 때 잠시 보여줄 한 줄 안내를 읽는다. 없으면 null이다.
        const char* GetNotice() const { return m_NoticeFrames > 0 ? "Piece editing not ready" : nullptr; }

    private:
        enum Handle { None = -1, X, Y, Z, XY, YZ, ZX, Uniform };
        float HandleSize(Math::Vector3 pivot, const Math::Camera& camera, float height) const;
        Handle HitTest(Math::Vector3 pivot, float size, const PickingRay& ray,
            GizmoMath::Point mouse, const Math::Camera& camera, float width, float height) const;
        void Cancel(Scene& scene);
        void SetAxes(const SceneTransform& transform);
        // Mesh 대상 편집 거절 안내를 일정 프레임 동안 켠다.
        void RaiseNotice();

        ObjectId m_DragId = InvalidObjectId;
        Handle m_Active = None;
        Handle m_Hover = None;
        SceneTransform m_Start{ Math::kIdentity };
        Math::Vector3 m_Pivot{ Math::kZero };
        Math::Vector3 m_Normal{ Math::kZero };
        Math::Vector3 m_StartHit{ Math::kZero };
        Math::Vector3 m_Axes[3] = { Math::Vector3(Math::kXUnitVector),
            Math::Vector3(Math::kYUnitVector), Math::Vector3(Math::kZUnitVector) };
        Mode m_Mode = Translate;
        bool m_Local = false;
        unsigned m_ScaleAxisMask = 7;
        Math::Vector3 m_LastVector{ Math::kZero };
        float m_Angle = 0.0f;
        float m_StartY = 0.0f;
        float m_StartSize = 1.0f;
        TransformHistory m_History;
        int m_NoticeFrames = 0;
    };
}
