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
        // Returns true when the gizmo consumes this frame's pointer interaction.
        bool Update(Scene& scene, ObjectId selection, const Math::Camera& camera, float width, float height);
        void Render(GraphicsContext& context, const Scene& scene, ObjectId selection,
            const Math::Camera& camera, float width, float height) const;
        bool IsDragging() const { return m_DragId != InvalidObjectId; }
        Mode GetMode() const { return m_Mode; }
        bool IsLocal() const { return m_Local; }

    private:
        enum Handle { None = -1, X, Y, Z, XY, YZ, ZX, Uniform };
        float HandleSize(Math::Vector3 pivot, const Math::Camera& camera, float height) const;
        Handle HitTest(Math::Vector3 pivot, float size, const PickingRay& ray,
            GizmoMath::Point mouse, const Math::Camera& camera, float width, float height) const;
        void Cancel(Scene& scene);
        void SetAxes(const Math::UniformTransform& transform);

        ObjectId m_DragId = InvalidObjectId;
        Handle m_Active = None;
        Handle m_Hover = None;
        Math::UniformTransform m_Start{ Math::kIdentity };
        Math::Vector3 m_Pivot{ Math::kZero };
        Math::Vector3 m_Normal{ Math::kZero };
        Math::Vector3 m_StartHit{ Math::kZero };
        Math::Vector3 m_Axes[3] = { Math::Vector3(Math::kXUnitVector),
            Math::Vector3(Math::kYUnitVector), Math::Vector3(Math::kZUnitVector) };
        Mode m_Mode = Translate;
        bool m_Local = false;
        Math::Vector3 m_LastVector{ Math::kZero };
        float m_Angle = 0.0f;
        float m_StartY = 0.0f;
        TransformHistory m_History;
    };
}
