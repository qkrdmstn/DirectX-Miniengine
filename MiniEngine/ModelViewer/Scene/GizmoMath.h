#pragma once

#include "VectorMath.h"
#include "SceneTransform.h"
#include <cmath>

namespace Viewer
{
    namespace GizmoMath
    {
        struct Point { float X, Y; };

        // 정규화하지 않은 로컬 ray도 원래 매개변수 거리를 보존하며 양면 삼각형과 비교한다.
        inline bool IntersectTriangle(Math::Vector3 origin, Math::Vector3 direction,
            Math::Vector3 a, Math::Vector3 b, Math::Vector3 c, float& distance)
        {
            // 삼각형 크기에 비례한 기준으로 평행하거나 퇴화한 입력을 거절한다.
            const Math::Vector3 ab = b - a, ac = c - a;
            const Math::Vector3 p = Math::Cross(direction, ac);
            const float determinant = Math::Dot(ab, p);
            const float magnitude = std::sqrt(float(Math::LengthSquare(ab) * Math::LengthSquare(p)));
            if (!std::isfinite(determinant) || std::abs(determinant) <= magnitude * 1e-6f) return false;

            // 무게중심 좌표와 앞쪽 교차 거리를 검사한다.
            const Math::Vector3 offset = origin - a, q = Math::Cross(offset, ab);
            const float u = float(Math::Dot(offset, p)) / determinant;
            const float v = float(Math::Dot(direction, q)) / determinant;
            distance = float(Math::Dot(ac, q)) / determinant;
            return u >= -1e-5f && v >= -1e-5f && u + v <= 1.00001f &&
                distance >= 0.0f && std::isfinite(distance);
        }

        // 회전·크기를 바꿔도 시작 변환의 로컬 피벗이 같은 월드 위치에 남도록 위치를 보정한다.
        inline void KeepPivot(const SceneTransform& start, Math::Vector3 pivot, SceneTransform& changed)
        {
            // 시작 월드 피벗을 로컬로 돌리고 새 회전·크기가 만든 오프셋을 제거한다.
            const Math::Vector3 local(Math::Invert(Math::Matrix4(start.ToAffine())) * pivot);
            changed.SetTranslation(pivot - changed.GetRotation() * (changed.GetScale() * local));
        }

        inline Math::Vector3 RayDirection(const Math::Matrix4& viewProjection, Math::Vector3 eye, float x, float y)
        {
            // Interior depth avoids either projection convention's infinite far plane.
            const Math::Vector3 point = Math::MakeHomogeneous(Math::Invert(viewProjection) *
                Math::Vector4(2.0f * x - 1.0f, 1.0f - 2.0f * y, 0.5f, 1.0f));
            return Math::Normalize(point - eye);
        }

        inline bool Project(Math::Vector3 position, const Math::Matrix4& viewProjection,
            float width, float height, Point& result)
        {
            const Math::Vector4 clip = viewProjection * position;
            if (float(clip.GetW()) <= 0.0001f)
                return false;
            const Math::Vector3 ndc = Math::MakeHomogeneous(clip);
            result = { (float(ndc.GetX()) + 1.0f) * 0.5f * width,
                (1.0f - float(ndc.GetY())) * 0.5f * height };
            return true;
        }

        inline float SegmentDistance(Point point, Point a, Point b)
        {
            const float x = b.X - a.X, y = b.Y - a.Y;
            const float length = x * x + y * y;
            float t = length > 0.0001f ? ((point.X - a.X) * x + (point.Y - a.Y) * y) / length : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float dx = point.X - a.X - x * t, dy = point.Y - a.Y - y * t;
            return std::sqrt(dx * dx + dy * dy);
        }

        inline bool ArrowHead(Point start, Point tip, Point (&triangle)[3])
        {
            const float dx = tip.X - start.X, dy = tip.Y - start.Y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length < 1.0f)
                return false;
            const float headLength = length < 30.0f ? length * 0.4f : 12.0f;
            const float halfWidth = length < 30.0f ? length / 6.0f : 5.0f;
            const float ux = dx / length, uy = dy / length;
            const Point base = { tip.X - ux * headLength, tip.Y - uy * headLength };
            triangle[0] = tip;
            triangle[1] = { base.X - uy * halfWidth, base.Y + ux * halfWidth };
            triangle[2] = { base.X + uy * halfWidth, base.Y - ux * halfWidth };
            return true;
        }

        inline bool IntersectPlane(Math::Vector3 origin, Math::Vector3 direction,
            Math::Vector3 point, Math::Vector3 normal, Math::Vector3& hit)
        {
            const float denominator = Math::Dot(direction, normal);
            if (std::abs(denominator) < 0.0001f)
                return false;
            const float distance = float(Math::Dot(point - origin, normal)) / denominator;
            if (distance < 0.0f || !std::isfinite(distance))
                return false;
            hit = origin + direction * distance;
            return true;
        }

        // The axis drag plane contains the axis and faces the ray as much as possible.
        inline bool AxisPlaneNormal(Math::Vector3 axis, Math::Vector3 viewDirection, Math::Vector3& normal)
        {
            const Math::Vector3 perpendicular = viewDirection - axis * Math::Dot(axis, viewDirection);
            if (float(Math::LengthSquare(perpendicular)) < 0.0001f)
                return false;
            normal = Math::Normalize(perpendicular);
            return true;
        }

        inline float SignedAngle(Math::Vector3 from, Math::Vector3 to, Math::Vector3 axis)
        {
            return std::atan2(float(Math::Dot(axis, Math::Cross(from, to))), float(Math::Dot(from, to)));
        }

        // 표시·선택·드래그에 공통으로 쓰는 카메라 독립 축을 구한다.
        inline void Basis(Math::Quaternion rotation, bool local, Math::Vector3 (&axes)[3])
        {
            const Math::Vector3 unit[] = { Math::Vector3(Math::kXUnitVector),
                Math::Vector3(Math::kYUnitVector), Math::Vector3(Math::kZUnitVector) };
            for (int i = 0; i < 3; ++i)
                axes[i] = local ? rotation * unit[i] : unit[i];
        }
        // 드래그 배율을 양수 유한 범위로 제한한다.
        inline float UniformScale(float initialScale, float upwardPixels)
        {
            const float exponent = upwardPixels * 0.01f;
            const float bounded = exponent < -12.0f ? -12.0f : (exponent > 12.0f ? 12.0f : exponent);
            const float scale = initialScale * std::exp(bounded);
            return scale < 0.0001f ? 0.0001f : (scale > 1.0e8f ? 1.0e8f : scale);
        }
        // 허용된 축만 배율을 적용하며 중앙 핸들(-1)은 허용 축 전체를 조절한다.
        inline Math::Vector3 ScaleComponents(Math::Vector3 initial, int axis, float drag, unsigned axisMask = 7)
        {
            Math::XMFLOAT3 values;
            DirectX::XMStoreFloat3(&values, initial);
            if ((axisMask & 1) && (axis == -1 || axis == 0)) values.x = UniformScale(values.x, drag);
            if ((axisMask & 2) && (axis == -1 || axis == 1)) values.y = UniformScale(values.y, drag);
            if ((axisMask & 4) && (axis == -1 || axis == 2)) values.z = UniformScale(values.z, drag);
            return Math::Vector3(values);
        }
    }
}
