#include "TransformGizmo.h"
#include "GizmoDraw.h"
#include "GameInput.h"

namespace Viewer
{
    using namespace Math;
    namespace
    {
        const Color Colors[] = { Color(1.0f, 0.2f, 0.15f), Color(0.25f, 1.0f, 0.25f), Color(0.25f, 0.55f, 1.0f) };
        const int PlaneAxes[][2] = { {0, 1}, {1, 2}, {2, 0} };
    }

    // 선택 좌표계의 축을 구해 선택과 드래그가 공유하도록 저장한다.
    void TransformGizmo::SetAxes(const SceneTransform& transform)
    {
        GizmoMath::Basis(transform.GetRotation(), IsLocal(), m_Axes);
    }

    // 화면에서 일정한 크기를 유지하는 월드 핸들 길이를 구한다.
    float TransformGizmo::HandleSize(Vector3 pivot, const Camera& camera, float height) const
    {
        const float depth = Dot(pivot - camera.GetPosition(), camera.GetForwardVec());
        if (depth <= camera.GetNearClip() || height <= 0.0f)
            return 0.0f;
        return 190.0f * depth / (height * float(camera.GetProjMatrix().GetY().GetY()));
    }

    // 공유 기저의 축·평면·링을 투영하여 포인터에 가장 가까운 핸들을 찾는다.
    TransformGizmo::Handle TransformGizmo::HitTest(Vector3 pivot, float size, const PickingRay& ray,
        GizmoMath::Point mouse, const Camera& camera, float width, float height) const
    {
        Handle result = None;
        float nearest = 9.0f;
        if (m_Mode == Scale)
        {
            GizmoMath::Point center;
            if (GizmoMath::Project(pivot, camera.GetViewProjMatrix(), width, height, center) &&
                std::abs(mouse.X - center.X) <= 10.0f && std::abs(mouse.Y - center.Y) <= 10.0f)
                return Uniform;
        }
        if (m_Mode == Rotate)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(float(Dot(ray.Direction, m_Axes[axis]))) < 0.06f)
                    continue;
                const Vector3 u = m_Axes[(axis + 1) % 3], v = m_Axes[(axis + 2) % 3];
                for (int segment = 0; segment < 64; ++segment)
                {
                    const float angle = segment * XM_2PI / 64.0f, next = (segment + 1) * XM_2PI / 64.0f;
                    GizmoMath::Point a, b;
                    if (!GizmoMath::Project(pivot + (u * std::cos(angle) + v * std::sin(angle)) * size,
                        camera.GetViewProjMatrix(), width, height, a) ||
                        !GizmoMath::Project(pivot + (u * std::cos(next) + v * std::sin(next)) * size,
                        camera.GetViewProjMatrix(), width, height, b))
                        continue;
                    const float distance = GizmoMath::SegmentDistance(mouse, a, b);
                    if (distance < nearest)
                    {
                        nearest = distance;
                        result = static_cast<Handle>(axis);
                    }
                }
            }
            return result;
        }
        // Thin axis handles take precedence over plane handles and scene selection.
        for (int axis = 0; axis < 3; ++axis)
        {
            if (m_Mode == Scale && !(m_ScaleAxisMask & (1u << axis)))
                continue;
            GizmoMath::Point a, b;
            Vector3 normal;
            if (!GizmoMath::AxisPlaneNormal(m_Axes[axis], ray.Direction, normal) ||
                !GizmoMath::Project(pivot + m_Axes[axis] * size * 0.15f, camera.GetViewProjMatrix(), width, height, a) ||
                !GizmoMath::Project(pivot + m_Axes[axis] * size, camera.GetViewProjMatrix(), width, height, b))
                continue;
            const float distance = GizmoMath::SegmentDistance(mouse, a, b);
            if (distance < nearest)
            {
                nearest = distance;
                result = static_cast<Handle>(axis);
            }
        }
        if (result != None || m_Mode == Scale)
            return result;
        for (int plane = 0; plane < 3; ++plane)
        {
            const Vector3 u = m_Axes[PlaneAxes[plane][0]], v = m_Axes[PlaneAxes[plane][1]];
            Vector3 hit;
            if (!GizmoMath::IntersectPlane(ray.Origin, ray.Direction, pivot, Cross(u, v), hit))
                continue;
            const float x = float(Dot(hit - pivot, u)) / size, y = float(Dot(hit - pivot, v)) / size;
            if (x >= 0.22f && x <= 0.45f && y >= 0.22f && y <= 0.45f)
                return static_cast<Handle>(XY + plane);
        }
        return None;
    }

    // 시작 TRS 전체를 복구하고 활성 드래그를 해제한다.
    void TransformGizmo::Cancel(Scene& scene, PhysicsSystem* physics)
    {
        // 공통 경로로 시작 월드 TRS를 복구한다.
        ApplyEditTransform(scene, physics, m_DragTarget, m_Start);
        m_DragTarget = EditTarget();
        m_Active = None;
    }

    // Owner와 Mesh의 드래그와 취소 및 공통 경로의 이력 복구를 처리한다.
    bool TransformGizmo::Update(Scene& scene, const EditTarget& target, const Camera& camera, float width, float height,
        unsigned scaleAxisMask, PhysicsSystem* physics)
    {
        // 이번 입력의 크기 축 제한을 저장한다.
        m_ScaleAxisMask = scaleAxisMask;

        // 마우스 위치를 읽는다.
        float x;
        float y;
        const bool validMouse = GameInput::GetMousePosition(x, y);
        if (IsDragging())
        {
            // 대상 소멸이나 Esc 입력은 시작 변환 복구로 처리한다.
            if (target != m_DragTarget || !IsEditTargetValid(scene, m_DragTarget) || !validMouse ||
                GameInput::IsFirstPressed(GameInput::kKey_escape))
            {
                Cancel(scene, physics);
                return true;
            }

            // 시작 월드 TRS를 기준으로 매 프레임 편집량을 계산한다.
            const PickingRay ray = MakePickingRay(camera, x, y);
            SceneTransform transform = m_Start;
            if (m_Mode == Scale)
            {
                // 유효한 교차점이 있을 때 시작 크기에서 배율을 계산한다.
                float drag = m_StartY - y * height;
                Vector3 hit;
                const bool validScale = m_Active == Uniform ||
                    GizmoMath::IntersectPlane(ray.Origin, ray.Direction, m_Pivot, m_Normal, hit);
                if (validScale)
                {
                    if (m_Active != Uniform)
                        drag = 100.0f * float(Dot(hit - m_StartHit, m_Axes[m_Active])) / m_StartSize;
                    transform.SetScale(GizmoMath::ScaleComponents(m_Start.GetScale(),
                        m_Active == Uniform ? -1 : int(m_Active), drag, m_ScaleAxisMask));
                    m_LastDragTransform = transform;
                }
            }
            else
            {
                Vector3 hit;
                if (GizmoMath::IntersectPlane(ray.Origin, ray.Direction, m_Pivot, m_Normal, hit))
                {
                    if (m_Mode == Rotate)
                    {
                        // 피벗에서 떨어진 입력만 회전 누적에 사용한다.
                        const Vector3 offset = hit - m_Pivot;
                        if (float(LengthSquare(offset)) > 0.000001f)
                        {
                            const Vector3 current = Normalize(offset);
                            m_Angle += GizmoMath::SignedAngle(m_LastVector, current, m_Normal);
                            m_LastVector = current;

                            // 누적 회전을 시작 자세에 적용해 마지막 유효 변환으로 보관한다.
                            transform.SetRotation(Quaternion(m_Normal, m_Angle) * m_Start.GetRotation());
                            m_LastDragTransform = transform;
                        }
                    }
                    else
                    {
                        // 시작 교차점에서 이동량을 구하고 축 핸들의 방향으로 제한한다.
                        Vector3 delta = hit - m_StartHit;
                        if (m_Active <= Z)
                            delta = m_Axes[m_Active] * Dot(delta, m_Axes[m_Active]);

                        // 시작 위치에 이동량을 적용해 마지막 유효 변환으로 보관한다.
                        transform.SetTranslation(m_Start.GetTranslation() + delta);
                        m_LastDragTransform = transform;
                    }
                }
            }

            // 계산이 실패한 프레임에도 마지막 유효 변환으로 body를 순간이동한다.
            ApplyEditTransform(scene, physics, m_DragTarget, m_LastDragTransform);

            // 버튼을 놓으면 드래그와 이력 기록을 마친다.
            if (!GameInput::IsPressed(GameInput::kMouse0))
            {
                // 대상의 종료 월드 TRS와 변환 모드를 함께 기록한다.
                SceneTransform after;
                if (TryGetEditTargetTransform(scene, m_DragTarget, after))
                {
                    HistoryTransformMode mode = HistoryTransformMode::Owner;
                    if (m_DragTarget.IsMesh())
                    {
                        const auto* mesh = scene.FindObject(m_DragTarget.Object)->FindMeshComponent(m_DragTarget.Mesh);
                        mode = mesh->GetTransformMode() == MeshTransformMode::Attached ?
                            HistoryTransformMode::Attached : HistoryTransformMode::WorldIndependent;
                    }
                    m_History.Record(m_DragTarget, mode, m_Start, after);
                }

                // 완료한 드래그의 활성 상태를 해제한다.
                m_DragTarget = EditTarget();
                m_Active = None;
            }
            return true;
        }

        // 현재 선택과 무관하게 전역 이력을 되돌린다.
        const bool control = GameInput::IsPressed(GameInput::kKey_lcontrol) ||
            GameInput::IsPressed(GameInput::kKey_rcontrol);
        if (control && GameInput::IsFirstPressed(GameInput::kKey_z))
        {
            // 공통 콜백으로 대상과 모드를 검사하고 물리 body도 복구한다.
            m_History.Undo([&scene, physics](const TransformHistory::Command& command)
            {
                // 진단과 같은 이력 적용 경로를 사용한다.
                return ApplyHistoryCommand(scene, physics, command);
            });
            m_Hover = None;
            return true;
        }

        // 카메라 입력 중이 아니면 편집 모드와 좌표계를 바꾼다.
        if (!GameInput::IsPressed(GameInput::kMouse1))
        {
            if (GameInput::IsFirstPressed(GameInput::kKey_q)) m_Mode = View;
            if (GameInput::IsFirstPressed(GameInput::kKey_w)) m_Mode = Translate;
            if (GameInput::IsFirstPressed(GameInput::kKey_e)) m_Mode = Rotate;
            if (GameInput::IsFirstPressed(GameInput::kKey_r)) m_Mode = Scale;
            if (GameInput::IsFirstPressed(GameInput::kKey_l)) m_Local = !m_Local;
        }

        // 대상의 월드 TRS에서 핸들을 선택한다.
        m_Hover = None;
        const GameObject* object = scene.FindObject(target.Object);
        SceneTransform frame;
        if (m_Mode == View || !object || !object->IsEditable() || !validMouse || GameInput::IsPressed(GameInput::kMouse1) ||
            !TryGetEditTargetTransform(scene, target, frame))
            return false;
        const Vector3 pivot = frame.GetTranslation();
        SetAxes(frame);
        const float size = HandleSize(pivot, camera, height);
        if (size <= 0.0f)
            return false;
        const PickingRay ray = MakePickingRay(camera, x, y);
        m_Hover = HitTest(pivot, size, ray, { x * width, y * height }, camera, width, height);
        if (m_Hover == None || !GameInput::IsFirstPressed(GameInput::kMouse0))
            return false;

        // 선택한 핸들에서 드래그 기저를 고정한다.
        bool canStart = m_Mode == Scale && m_Hover == Uniform;
        if (canStart)
        {
            m_StartY = y * height;
        }
        else if (m_Mode == Rotate)
        {
            m_Normal = m_Axes[m_Hover];
        }
        else if (m_Hover <= Z)
        {
            if (!GizmoMath::AxisPlaneNormal(m_Axes[m_Hover], ray.Direction, m_Normal))
                return true;
        }
        else
        {
            const int* axes = PlaneAxes[m_Hover - XY];
            m_Normal = Cross(m_Axes[axes[0]], m_Axes[axes[1]]);
        }
        if (!canStart)
            canStart = GizmoMath::IntersectPlane(ray.Origin, ray.Direction, pivot, m_Normal, m_StartHit);
        if (canStart && m_Mode == Rotate)
        {
            const Vector3 offset = m_StartHit - pivot;
            canStart = float(LengthSquare(offset)) > 0.000001f;
            if (canStart)
                m_LastVector = Normalize(offset);
            m_Angle = 0.0f;
        }

        // 드래그 시작 상태와 첫 프레임부터 재적용할 유효 변환을 저장한다.
        if (canStart)
        {
            m_StartSize = size;
            m_Start = frame;
            m_LastDragTransform = m_Start;
            m_Pivot = pivot;
            m_DragTarget = target;
            m_Active = m_Hover;
        }
        return true;
    }

    // 대상의 월드 TRS 위치에 공통 좌표계와 고정된 드래그 기저로 핸들을 그린다.
    void TransformGizmo::Render(GraphicsContext& context, const Scene& scene, const EditTarget& target,
        const Camera& camera, float width, float height) const
    {
        if (m_Mode == View)
            return;
        const GameObject* object = scene.FindObject(target.Object);
        SceneTransform frame;
        if (!object || !object->IsEditable() || !TryGetEditTargetTransform(scene, target, frame))
            return;
        const Vector3 pivot = frame.GetTranslation();
        const float size = HandleSize(pivot, camera, height);
        if (size <= 0.0f)
            return;
        GizmoDraw draw;
        const Handle highlight = IsDragging() ? m_Active : m_Hover;
        Vector3 axes[3];
        GizmoMath::Basis(frame.GetRotation(), IsLocal(), axes);
        if (IsDragging())
            for (int i = 0; i < 3; ++i)
                axes[i] = m_Axes[i];
        if (m_Mode == Rotate)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                const Vector3 u = axes[(axis + 1) % 3], v = axes[(axis + 2) % 3];
                const Color color = highlight == axis ? Color(1.0f, 0.85f, 0.1f) : Colors[axis];
                for (int segment = 0; segment < 64; ++segment)
                {
                    const float angle = segment * XM_2PI / 64.0f, next = (segment + 1) * XM_2PI / 64.0f;
                    GizmoMath::Point a, b;
                    if (GizmoMath::Project(pivot + (u * std::cos(angle) + v * std::sin(angle)) * size,
                        camera.GetViewProjMatrix(), width, height, a) &&
                        GizmoMath::Project(pivot + (u * std::cos(next) + v * std::sin(next)) * size,
                        camera.GetViewProjMatrix(), width, height, b))
                        draw.Line(a, b, color);
                }
            }
            draw.Render(context, width, height);
            return;
        }
        for (int plane = 0; m_Mode == Translate && plane < 3; ++plane)
        {
            const Vector3 u = axes[PlaneAxes[plane][0]] * size, v = axes[PlaneAxes[plane][1]] * size;
            const Vector3 corners[] = { pivot + u * 0.22f + v * 0.22f, pivot + u * 0.45f + v * 0.22f,
                pivot + u * 0.45f + v * 0.45f, pivot + u * 0.22f + v * 0.45f };
            GizmoMath::Point points[4];
            bool visible = true;
            for (int i = 0; i < 4; ++i)
                visible &= GizmoMath::Project(corners[i], camera.GetViewProjMatrix(), width, height, points[i]);
            if (visible)
            {
                const Color color = highlight == XY + plane ? Color(1.0f, 0.85f, 0.1f, 0.8f) :
                    Color(Colors[(plane + 2) % 3].R(), Colors[(plane + 2) % 3].G(), Colors[(plane + 2) % 3].B(), 0.3f);
                draw.Quad(points, color);
                for (int i = 0; i < 4; ++i)
                    draw.Line(points[i], points[(i + 1) % 4], color, 2.0f);
            }
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (m_Mode == Scale && !(m_ScaleAxisMask & (1u << axis)))
                continue;
            GizmoMath::Point a, b;
            if (GizmoMath::Project(pivot, camera.GetViewProjMatrix(), width, height, a) &&
                GizmoMath::Project(pivot + axes[axis] * size, camera.GetViewProjMatrix(), width, height, b))
            {
                const Color color = highlight == axis || highlight == Uniform ? Color(1.0f, 0.85f, 0.1f) : Colors[axis];
                if (m_Mode == Translate)
                    draw.Arrow(a, b, color);
                else
                {
                    draw.Line(a, b, color);
                    const GizmoMath::Point box[] = { {b.X - 4, b.Y - 4}, {b.X + 4, b.Y - 4},
                        {b.X + 4, b.Y + 4}, {b.X - 4, b.Y + 4} };
                    draw.Quad(box, color);
                }
            }
        }
        if (m_Mode == Scale)
        {
            GizmoMath::Point center;
            if (GizmoMath::Project(pivot, camera.GetViewProjMatrix(), width, height, center))
            {
                const GizmoMath::Point box[] = { {center.X - 7, center.Y - 7}, {center.X + 7, center.Y - 7},
                    {center.X + 7, center.Y + 7}, {center.X - 7, center.Y + 7} };
                draw.Quad(box, highlight == Uniform ? Color(1.0f, 0.85f, 0.1f) : Color(1.0f, 1.0f, 1.0f));
            }
        }
        draw.Render(context, width, height);
    }
}
