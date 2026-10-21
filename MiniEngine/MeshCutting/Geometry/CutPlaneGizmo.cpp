// 이 파일은 절단 평면을 마우스로 옮기는 기즈모의 계산과 상호작용(CutPlane의 기즈모 멤버)을 구현한다.
// 광선, 피킹, 드래그, 굽기는 Model과 Core를 모르는 순수 계산이라 GPU 없이 테스트한다 (tests/PlaneSlicerTests.cpp).
// 화면에 그리는 일은 CutDebugView가 한다. 설계: Docs/SceneViewControlsDesign.md 3절.
#include "../pch.h"
#include "CutPlane.h"
#include "../Rendering/RenderBake.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace MeshCutting
{
    namespace
    {
        constexpr float kGizmoPixels = 100.0f;      // 기즈모 한 단위가 화면에서 차지하는 픽셀
        constexpr double kPi = 3.14159265358979323846;

        const XMFLOAT4 kRed = { 0.95f, 0.25f, 0.25f, 1.0f };
        const XMFLOAT4 kGreen = { 0.4f, 0.85f, 0.3f, 1.0f };
        const XMFLOAT4 kBlue = { 0.3f, 0.5f, 1.0f, 1.0f };
        const XMFLOAT4 kGray = { 0.85f, 0.85f, 0.85f, 1.0f };
        const XMFLOAT4 kYellow = { 1.0f, 0.9f, 0.1f, 1.0f };

        // 반지름 radius의 조각을 이루는 점 수
        constexpr int kRingSegments = 48;

        // 점 p에서 선분 ab까지의 2D 거리. 가장 가까운 점의 선분 위 비율을 t에 돌려준다.
        double DistanceToSegment2D(double px, double py, double ax, double ay, double bx, double by, double& t)
        {
            const double dx = bx - ax, dy = by - ay;
            const double lengthSq = dx * dx + dy * dy;
            t = lengthSq > 0.0 ? Clamp(((px - ax) * dx + (py - ay) * dy) / lengthSq, 0.0, 1.0) : 0.0;
            const double cx = ax + dx * t - px, cy = ay + dy * t - py;
            return std::sqrt(cx * cx + cy * cy);
        }

        struct ScreenPoint
        {
            float x = 0.0f, y = 0.0f;
            bool valid = false;
        };

        ScreenPoint Project(const XMFLOAT3& world, const GizmoView& view)
        {
            ScreenPoint p;
            p.valid = CutPlane::ProjectToScreen(world, view, p.x, p.y);
            return p;
        }

        struct PickHit
        {
            float pixelDistance = 0.0f;
            float depth = 0.0f;
        };

        float DistanceFromEye(const GizmoView& view, const V3& world)
        {
            return float(Length(world - ToV3(view.eye)));
        }

        // 도형 하나에서 커서와 가장 가까운 곳의 픽셀 거리와 그 곳의 카메라 거리. 투영할 수 없으면 false.
        bool PrimitiveHit(const GizmoView& view, const GizmoPrimitive& primitive, float cursorX, float cursorY, PickHit& hit)
        {
            const std::vector<XMFLOAT3>& points = primitive.points;
            if (points.empty())
                return false;

            if (primitive.shape == GizmoShape::Point)
            {
                const ScreenPoint s = Project(points[0], view);
                if (!s.valid)
                    return false;
                hit.pixelDistance = float(std::hypot(double(s.x) - cursorX, double(s.y) - cursorY));
                hit.depth = DistanceFromEye(view, ToV3(points[0]));
                return true;
            }

            std::vector<ScreenPoint> screen(points.size());
            for (size_t i = 0; i < points.size(); ++i)
                screen[i] = Project(points[i], view);

            const bool closed = primitive.shape != GizmoShape::Segment;
            const size_t edges = closed ? points.size() : points.size() - 1;
            bool found = false;
            for (size_t i = 0; i < edges; ++i)
            {
                const size_t j = (i + 1) % points.size();
                if (!screen[i].valid || !screen[j].valid)
                    continue;
                double t = 0.0;
                const double distance = DistanceToSegment2D(cursorX, cursorY, screen[i].x, screen[i].y, screen[j].x, screen[j].y, t);
                if (!found || distance < hit.pixelDistance)
                {
                    const V3 a = ToV3(points[i]), b = ToV3(points[j]);
                    hit.pixelDistance = float(distance);
                    hit.depth = DistanceFromEye(view, a + (b - a) * t);
                    found = true;
                }
            }
            if (!found)
                return false;

            if (primitive.shape == GizmoShape::Quad)
            {
                // 사각형 안이면 거리 0 (칸 전체가 핸들이다).
                bool allValid = true;
                for (const ScreenPoint& s : screen)
                    allValid = allValid && s.valid;
                if (allValid)
                {
                    bool inside = false;
                    for (size_t i = 0, j = screen.size() - 1; i < screen.size(); j = i++)
                    {
                        const bool crosses = (screen[i].y > cursorY) != (screen[j].y > cursorY);
                        if (crosses && cursorX < (screen[j].x - screen[i].x) * (cursorY - screen[i].y) / (screen[j].y - screen[i].y) + screen[i].x)
                            inside = !inside;
                    }
                    if (inside)
                    {
                        hit.pixelDistance = 0.0f;
                        V3 centroid;
                        for (const XMFLOAT3& p : points)
                            centroid = centroid + ToV3(p) * (1.0 / double(points.size()));
                        hit.depth = DistanceFromEye(view, centroid);
                    }
                }
            }
            return true;
        }

        XMFLOAT3 Offset(const XMFLOAT3& origin, const V3& direction, double distance)
        {
            return ToFloat3(ToV3(origin) + direction * distance);
        }

        GizmoPrimitive MakeSegment(GizmoHandle handle, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& color)
        {
            GizmoPrimitive p;
            p.handle = handle;
            p.shape = GizmoShape::Segment;
            p.points = { a, b };
            p.color = color;
            return p;
        }

        GizmoPrimitive MakeQuad(GizmoHandle handle, const XMFLOAT3& center, const V3& axisA, const V3& axisB,
            double from, double to, const XMFLOAT4& color)
        {
            const V3 c = ToV3(center);
            GizmoPrimitive p;
            p.handle = handle;
            p.shape = GizmoShape::Quad;
            p.points = { ToFloat3(c + axisA * from + axisB * from), ToFloat3(c + axisA * to + axisB * from),
                ToFloat3(c + axisA * to + axisB * to), ToFloat3(c + axisA * from + axisB * to) };
            p.color = color;
            return p;
        }

        GizmoPrimitive MakeRing(GizmoHandle handle, const XMFLOAT3& center, const V3& axisA, const V3& axisB,
            double radius, const XMFLOAT4& color)
        {
            const V3 c = ToV3(center);
            GizmoPrimitive p;
            p.handle = handle;
            p.shape = GizmoShape::Ring;
            for (int i = 0; i < kRingSegments; ++i)
            {
                const double angle = 2.0 * kPi * double(i) / double(kRingSegments);
                p.points.push_back(ToFloat3(c + axisA * (radius * std::cos(angle)) + axisB * (radius * std::sin(angle))));
            }
            p.color = color;
            return p;
        }

        GizmoPrimitive MakePoint(GizmoHandle handle, const XMFLOAT3& point, const XMFLOAT4& color)
        {
            GizmoPrimitive p;
            p.handle = handle;
            p.shape = GizmoShape::Point;
            p.points = { point };
            p.color = color;
            return p;
        }

        // 단위 벡터 d에 수직인 두 단위 벡터
        void Perpendiculars(const V3& d, V3& a, V3& b)
        {
            const V3 helper = std::fabs(d.x) < 0.9 ? V3{ 1.0, 0.0, 0.0 } : V3{ 0.0, 1.0, 0.0 };
            a = Normalize(Cross(d, helper));
            b = Cross(d, a);
        }

        double SignedAngle(const V3& from, const V3& to, const V3& axis)
        {
            return std::atan2(Dot(Cross(from, to), axis), Dot(from, to));
        }

        // (-pi, pi]로 되돌린다.
        double WrapAngle(double angle)
        {
            while (angle > kPi)
                angle -= 2.0 * kPi;
            while (angle <= -kPi)
                angle += 2.0 * kPi;
            return angle;
        }
    }

    GizmoView GizmoView::Make(const XMFLOAT4X4& viewProj, const XMFLOAT3& eye, float clientWidth, float clientHeight, bool reversedZ)
    {
        GizmoView view;
        view.viewProj = viewProj;
        XMStoreFloat4x4(&view.invViewProj, XMMatrixInverse(nullptr, XMLoadFloat4x4(&viewProj)));
        view.eye = eye;
        view.width = clientWidth > 1.0f ? clientWidth : 1.0f;
        view.height = clientHeight > 1.0f ? clientHeight : 1.0f;
        view.reversedZ = reversedZ;
        return view;
    }

    // 클라이언트 픽셀 좌표(왼쪽 위가 원점) -> NDC -> viewProj 역행렬로 만든 광선. 깊이 한 점은 near, 다른 한 점은
    // 그 사이의 값을 써서 far가 무한대인 투영에서도 유한한 값만 쓴다.
    GizmoRay CutPlane::MakeRay(const GizmoView& view, float clientX, float clientY)
    {
        const float ndcX = clientX / view.width * 2.0f - 1.0f;
        const float ndcY = 1.0f - clientY / view.height * 2.0f;
        const XMMATRIX inverse = XMLoadFloat4x4(&view.invViewProj);
        const float nearDepth = view.reversedZ ? 1.0f : 0.0f;
        const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, nearDepth, 1.0f), inverse);
        const XMVECTOR midPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.5f, 1.0f), inverse);

        GizmoRay ray;
        XMStoreFloat3(&ray.origin, nearPoint);
        XMStoreFloat3(&ray.direction, XMVector3Normalize(XMVectorSubtract(midPoint, nearPoint)));
        return ray;
    }

    // world 점의 클라이언트 픽셀 좌표. 카메라 뒤(w <= 0)이면 false.
    bool CutPlane::ProjectToScreen(const XMFLOAT3& world, const GizmoView& view, float& pixelX, float& pixelY)
    {
        const XMVECTOR clip = XMVector4Transform(XMVectorSet(world.x, world.y, world.z, 1.0f), XMLoadFloat4x4(&view.viewProj));
        const float w = XMVectorGetW(clip);
        if (!(w > 1e-6f))
            return false;
        pixelX = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * view.width;
        pixelY = (0.5f - XMVectorGetY(clip) / w * 0.5f) * view.height;
        return true;
    }

    // 기즈모 한 단위의 world 길이. 화면 중앙에서 한 픽셀이 차지하는 각도로 구해서, 카메라 거리가 달라도
    // 화면에서는 같은 크기(약 100픽셀)로 보인다.
    float CutPlane::GetGizmoUnit(const GizmoView& view, const XMFLOAT3& center)
    {
        const double distance = Length(ToV3(center) - ToV3(view.eye));
        const GizmoRay a = MakeRay(view, view.width * 0.5f, view.height * 0.5f);
        const GizmoRay b = MakeRay(view, view.width * 0.5f, view.height * 0.5f + 1.0f);
        const double cosine = Clamp(Dot(ToV3(a.direction), ToV3(b.direction)), -1.0, 1.0);
        const double angle = std::acos(cosine);
        double unit = distance * angle * kGizmoPixels;
        if (!(unit > 0.0) || !std::isfinite(unit))
            unit = distance * 0.15;
        return float(unit);
    }

    // 도구에 해당하는 핸들의 도형. 크기는 GetGizmoUnit에서 정한다.
    std::vector<GizmoPrimitive> CutPlane::BuildGizmoPrimitives(const GizmoView& view, GizmoTool tool) const
    {
        std::vector<GizmoPrimitive> primitives;
        if (tool == GizmoTool::Hidden)
            return primitives;
        const Frame frame = GetFrame();
        const double unit = GetGizmoUnit(view, frame.center);
        const XMFLOAT3& c = frame.center;
        const V3 x = { 1.0, 0.0, 0.0 }, y = { 0.0, 1.0, 0.0 }, z = { 0.0, 0.0, 1.0 };
        const V3 u = ToV3(frame.axisU), v = ToV3(frame.axisV), n = ToV3(frame.normal);

        if (tool == GizmoTool::Move)
        {
            primitives.push_back(MakeSegment(GizmoHandle::MoveX, c, Offset(c, x, unit), kRed));
            primitives.push_back(MakeSegment(GizmoHandle::MoveY, c, Offset(c, y, unit), kGreen));
            primitives.push_back(MakeSegment(GizmoHandle::MoveZ, c, Offset(c, z, unit), kBlue));
            primitives.push_back(MakeQuad(GizmoHandle::MoveYZ, c, y, z, unit * 0.25, unit * 0.5, kRed));
            primitives.push_back(MakeQuad(GizmoHandle::MoveZX, c, z, x, unit * 0.25, unit * 0.5, kGreen));
            primitives.push_back(MakeQuad(GizmoHandle::MoveXY, c, x, y, unit * 0.25, unit * 0.5, kBlue));
        }
        else if (tool == GizmoTool::Rotate)
        {
            primitives.push_back(MakeRing(GizmoHandle::RotateU, c, v, n, unit * 0.9, kRed));
            primitives.push_back(MakeRing(GizmoHandle::RotateV, c, n, u, unit * 0.9, kGreen));
            primitives.push_back(MakeRing(GizmoHandle::RotateN, c, u, v, unit * 0.9, kBlue));
        }
        else
        {
            primitives.push_back(MakePoint(GizmoHandle::ScaleU, Offset(c, u, frame.halfExtents.x), kRed));
            primitives.push_back(MakePoint(GizmoHandle::ScaleV, Offset(c, v, frame.halfExtents.y), kGreen));
            primitives.push_back(MakePoint(GizmoHandle::ScaleUniform, c, kGray));
        }
        return primitives;
    }

    // 핸들을 화면에 투영해서 커서와의 픽셀 거리가 threshold 이하인 것 중, 카메라에 가장 가까운 핸들을 고른다
    // (깊이가 같으면 픽셀 거리가 작은 쪽). 카메라 뒤에 걸친 선분은 건너뛴다.
    GizmoHandle CutPlane::PickFromPrimitives(const GizmoView& view, const std::vector<GizmoPrimitive>& primitives,
        float cursorX, float cursorY, float thresholdPixels)
    {
        GizmoHandle best = GizmoHandle::None;
        PickHit bestHit;
        for (const GizmoPrimitive& primitive : primitives)
        {
            PickHit hit;
            if (!PrimitiveHit(view, primitive, cursorX, cursorY, hit) || hit.pixelDistance > thresholdPixels)
                continue;
            // 겹치면 카메라에 가까운 쪽이 먼저이고, 깊이가 같으면 커서에 더 가까운 쪽이다.
            const float sameDepth = 1e-5f * std::max(1.0f, bestHit.depth);
            const bool nearer = best == GizmoHandle::None || hit.depth < bestHit.depth - sameDepth
                || (std::fabs(hit.depth - bestHit.depth) <= sameDepth && hit.pixelDistance < bestHit.pixelDistance);
            if (nearer)
            {
                best = primitive.handle;
                bestHit = hit;
            }
        }
        return best;
    }

    GizmoHandle CutPlane::PickHandle(const GizmoView& view, GizmoTool tool, float cursorX, float cursorY,
        float thresholdPixels) const
    {
        return PickFromPrimitives(view, BuildGizmoPrimitives(view, tool), cursorX, cursorY, thresholdPixels);
    }

    // 광선과 직선(origin + s * dir, dir는 단위 벡터)의 최근접점 매개변수 s. 두 직선이 거의 평행하면 false.
    bool CutPlane::ClosestParamOnAxis(const XMFLOAT3& axisOrigin, const XMFLOAT3& axisDir, const GizmoRay& ray, float& param)
    {
        const V3 d = Normalize(ToV3(axisDir)), r = Normalize(ToV3(ray.direction));
        const V3 w0 = ToV3(axisOrigin) - ToV3(ray.origin);
        const double b = Dot(d, r), dw = Dot(d, w0), rw = Dot(r, w0);
        const double denominator = 1.0 - b * b;     // 두 직선 사이 각의 sin^2
        if (denominator < 1e-6)
            return false;
        const double s = (b * rw - dw) / denominator;
        if (!std::isfinite(s))
            return false;
        param = float(s);
        return true;
    }

    // 광선과 평면의 교점. 평행하거나 교점이 광선 뒤쪽이면 false.
    bool CutPlane::RayPlaneHit(const GizmoRay& ray, const XMFLOAT3& point, const XMFLOAT3& normal, XMFLOAT3& hit)
    {
        const V3 n = Normalize(ToV3(normal)), r = ToV3(ray.direction);
        const double denominator = Dot(n, r);
        if (std::fabs(denominator) < 1e-6)
            return false;
        const double t = Dot(n, ToV3(point) - ToV3(ray.origin)) / denominator;
        if (!(t > 0.0) || !std::isfinite(t))
            return false;
        hit = ToFloat3(ToV3(ray.origin) + r * t);
        return true;
    }

    // 핸들을 잡은 순간의 기준을 만든다. 광선이 기준을 만들 수 없는 각도이면 false.
    bool CutPlane::BeginDrag(GizmoHandle handle, const GizmoPointer& pointer, GizmoDrag& drag) const
    {
        const Frame frame = GetFrame();
        GizmoDrag next;
        next.handle = handle;
        next.start = m_Shape;
        next.startX = pointer.x;
        next.startY = pointer.y;

        switch (handle)
        {
        case GizmoHandle::MoveX:
        case GizmoHandle::MoveY:
        case GizmoHandle::MoveZ:
            next.axisDir = handle == GizmoHandle::MoveX ? XMFLOAT3(1.0f, 0.0f, 0.0f)
                : (handle == GizmoHandle::MoveY ? XMFLOAT3(0.0f, 1.0f, 0.0f) : XMFLOAT3(0.0f, 0.0f, 1.0f));
            if (!ClosestParamOnAxis(m_Shape.center, next.axisDir, pointer.ray, next.startParam))
                return false;
            break;

        case GizmoHandle::MoveYZ:
        case GizmoHandle::MoveZX:
        case GizmoHandle::MoveXY:
            // 이름의 두 축이 놓인 평면의 법선은 나머지 한 축이다.
            next.axisDir = handle == GizmoHandle::MoveYZ ? XMFLOAT3(1.0f, 0.0f, 0.0f)
                : (handle == GizmoHandle::MoveZX ? XMFLOAT3(0.0f, 1.0f, 0.0f) : XMFLOAT3(0.0f, 0.0f, 1.0f));
            if (!RayPlaneHit(pointer.ray, m_Shape.center, next.axisDir, next.startHit))
                return false;
            break;

        case GizmoHandle::RotateU:
        case GizmoHandle::RotateV:
        case GizmoHandle::RotateN:
        {
            next.localAxis = handle == GizmoHandle::RotateU ? 0 : (handle == GizmoHandle::RotateV ? 1 : 2);
            next.axisDir = next.localAxis == 0 ? frame.axisU : (next.localAxis == 1 ? frame.axisV : frame.normal);
            if (!RayPlaneHit(pointer.ray, m_Shape.center, next.axisDir, next.startHit))
                return false;
            const V3 from = ToV3(next.startHit) - ToV3(m_Shape.center);
            if (Length(from) < 1e-6)
                return false;
            next.startVector = ToFloat3(Normalize(from));
            break;
        }

        case GizmoHandle::ScaleU:
        case GizmoHandle::ScaleV:
            next.axisDir = handle == GizmoHandle::ScaleU ? frame.axisU : frame.axisV;
            next.startExtent = handle == GizmoHandle::ScaleU ? m_Shape.halfExtents.x : m_Shape.halfExtents.y;
            if (!ClosestParamOnAxis(m_Shape.center, next.axisDir, pointer.ray, next.startParam))
                return false;
            break;

        case GizmoHandle::ScaleUniform:
            break;

        case GizmoHandle::None:
            return false;
        }
        drag = next;
        return true;
    }

    // 지금 커서가 가리키는 곳에 맞게 이 평면을 바꾼다. 크기는 [m_MinExtent, m_MaxExtent]로 제한한다.
    // 이번 프레임의 광선으로 값을 정할 수 없으면 false이고 평면은 그대로다.
    bool CutPlane::UpdateDrag(GizmoDrag& drag, const GizmoPointer& pointer)
    {
        CutPlane result = *this;
        result.m_Shape = drag.start;
        switch (drag.handle)
        {
        case GizmoHandle::MoveX:
        case GizmoHandle::MoveY:
        case GizmoHandle::MoveZ:
        {
            float param = 0.0f;
            if (!ClosestParamOnAxis(drag.start.center, drag.axisDir, pointer.ray, param))
                return false;
            const float delta = param - drag.startParam;
            result.Translate({ drag.axisDir.x * delta, drag.axisDir.y * delta, drag.axisDir.z * delta });
            break;
        }

        case GizmoHandle::MoveYZ:
        case GizmoHandle::MoveZX:
        case GizmoHandle::MoveXY:
        {
            XMFLOAT3 hit;
            if (!RayPlaneHit(pointer.ray, drag.start.center, drag.axisDir, hit))
                return false;
            result.Translate({ hit.x - drag.startHit.x, hit.y - drag.startHit.y, hit.z - drag.startHit.z });
            break;
        }

        case GizmoHandle::RotateU:
        case GizmoHandle::RotateV:
        case GizmoHandle::RotateN:
        {
            XMFLOAT3 hit;
            if (!RayPlaneHit(pointer.ray, drag.start.center, drag.axisDir, hit))
                return false;
            const V3 to = ToV3(hit) - ToV3(drag.start.center);
            if (Length(to) < 1e-6)
                return false;
            // 시작 벡터에서 현재 벡터까지의 각도. 한 바퀴를 넘어도 이어지도록 직전 값과의 차이만 더한다.
            const double raw = SignedAngle(ToV3(drag.startVector), Normalize(to), ToV3(drag.axisDir));
            drag.totalAngle += float(WrapAngle(raw - drag.previousAngle));
            drag.previousAngle = float(raw);
            result.RotateAroundLocalAxis(drag.localAxis, drag.totalAngle);
            break;
        }

        case GizmoHandle::ScaleU:
        case GizmoHandle::ScaleV:
        {
            float param = 0.0f;
            if (!ClosestParamOnAxis(drag.start.center, drag.axisDir, pointer.ray, param))
                return false;
            const float extent = std::max(m_MinExtent, std::min(m_MaxExtent, drag.startExtent + (param - drag.startParam)));
            if (drag.handle == GizmoHandle::ScaleU)
                result.m_Shape.halfExtents.x = extent;
            else
                result.m_Shape.halfExtents.y = extent;
            break;
        }

        case GizmoHandle::ScaleUniform:
        {
            // 오른쪽 위로 끌면 커지고 왼쪽 아래로 끌면 작아진다. 150픽셀에 e배.
            const float movement = (pointer.x - drag.startX) - (pointer.y - drag.startY);
            result.ScaleUniform(std::exp(movement / 150.0f));
            break;
        }

        case GizmoHandle::None:
            return false;
        }
        m_Shape = result.m_Shape;
        return true;
    }

    void CutPlane::SetGizmoTool(GizmoTool tool)
    {
        if (tool != m_Tool)
            CancelGizmo();
        m_Tool = tool;
    }

    void CutPlane::CancelGizmo()
    {
        m_Drag = GizmoDrag();
        m_Hovered = GizmoHandle::None;
    }

    // 호버를 갱신하고 드래그를 진행한다. 드래그로 평면이 바뀌었으면 true.
    bool CutPlane::UpdateGizmo(const GizmoView& view, const GizmoInput& input)
    {
        if (IsDragging())
        {
            if (!input.leftDown)
            {
                m_Drag = GizmoDrag();       // 놓았다: 평면은 마지막 갱신 그대로 둔다
                return false;
            }
            if (!input.cursorValid)
                return false;               // 창 밖에서는 마지막 위치를 유지한다
            GizmoPointer pointer;
            pointer.x = input.x;
            pointer.y = input.y;
            pointer.ray = MakeRay(view, input.x, input.y);
            if (!UpdateDrag(m_Drag, pointer))
                return false;
            ClampExtents();
            return true;
        }

        m_Hovered = GizmoHandle::None;
        if (m_Tool == GizmoTool::Hidden || input.blocked || !input.cursorValid)
            return false;
        m_Hovered = PickHandle(view, m_Tool, input.x, input.y);
        if (m_Hovered != GizmoHandle::None && input.leftPressed)
        {
            GizmoPointer pointer;
            pointer.x = input.x;
            pointer.y = input.y;
            pointer.ray = MakeRay(view, input.x, input.y);
            if (!BeginDrag(m_Hovered, pointer, m_Drag))
                m_Drag = GizmoDrag();
        }
        return false;
    }

    // 핸들을 가는 튜브로 만든다. 호버나 드래그 중인 핸들은 노랗고 조금 굵다. 굽기 전에 out을 비운다.
    void CutPlane::BakeGizmo(const GizmoView& view, ProceduralMeshData& out) const
    {
        out = ProceduralMeshData();
        if (m_Tool == GizmoTool::Hidden)
            return;
        const Frame frame = GetFrame();
        const float unit = GetGizmoUnit(view, frame.center);
        const GizmoHandle hovered = m_Hovered, active = m_Drag.handle;
        for (const GizmoPrimitive& primitive : BuildGizmoPrimitives(view, m_Tool))
        {
            const bool hot = primitive.handle == hovered || primitive.handle == active;
            const XMFLOAT4 color = hot ? kYellow : primitive.color;
            const float radius = unit * (hot ? 0.03f : 0.018f);
            const std::vector<XMFLOAT3>& p = primitive.points;

            switch (primitive.shape)
            {
            case GizmoShape::Segment:
            {
                AppendDebugSegment(out, p[0], p[1], radius, color);
                // 화살촉: 끝에서 뒤로 물러난 네 점으로 이은 선
                const V3 direction = Normalize(ToV3(p[1]) - ToV3(p[0]));
                V3 a, b;
                Perpendiculars(direction, a, b);
                const V3 back = ToV3(p[1]) - direction * (unit * 0.18);
                const double spread = unit * 0.06;
                AppendDebugSegment(out, p[1], ToFloat3(back + a * spread), radius, color);
                AppendDebugSegment(out, p[1], ToFloat3(back - a * spread), radius, color);
                AppendDebugSegment(out, p[1], ToFloat3(back + b * spread), radius, color);
                AppendDebugSegment(out, p[1], ToFloat3(back - b * spread), radius, color);
                break;
            }
            case GizmoShape::Ring:
            case GizmoShape::Quad:
                for (size_t i = 0; i < p.size(); ++i)
                    AppendDebugSegment(out, p[i], p[(i + 1) % p.size()], radius, color);
                break;
            case GizmoShape::Point:
            {
                // 작은 십자 (평면의 세 로컬 축 방향). 가장자리 핸들은 가운데에서 이어지는 선도 그린다.
                const double arm = unit * 0.07;
                const V3 c = ToV3(p[0]);
                const V3 axes[3] = { ToV3(frame.axisU), ToV3(frame.axisV), ToV3(frame.normal) };
                for (const V3& axis : axes)
                    AppendDebugSegment(out, ToFloat3(c - axis * arm), ToFloat3(c + axis * arm), radius, color);
                if (primitive.handle == GizmoHandle::ScaleU || primitive.handle == GizmoHandle::ScaleV)
                    AppendDebugSegment(out, frame.center, p[0], radius * 0.6f, color);
                break;
            }
            }
        }
        if (!out.indices.empty())
            out.sections.push_back({ 0, uint32_t(out.indices.size()), 0 });
    }
}
