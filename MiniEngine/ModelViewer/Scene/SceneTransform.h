#pragma once
#include "VectorMath.h"
#include <cmath>
#include <initializer_list>

namespace Viewer
{
    class SceneTransform
    {
    public:
        // 항등 TRS를 만든다.
        SceneTransform(Math::EIdentityTag = Math::kIdentity) {}
        // 기존 균일 변환을 축별 TRS로 옮긴다.
        SceneTransform(const Math::UniformTransform& value)
            : m_Position(value.GetTranslation()), m_Rotation(value.GetRotation()),
              m_Scale(float(value.GetScale()), float(value.GetScale()), float(value.GetScale())) {}
        // 위치와 회전 및 축별 크기를 지정한다.
        SceneTransform(Math::Quaternion rotation, Math::Vector3 scale, Math::Vector3 position)
            : m_Position(position), m_Rotation(rotation), m_Scale(scale) {}
        // 위치를 읽는다.
        Math::Vector3 GetTranslation() const { return m_Position; }
        // 회전을 읽는다.
        Math::Quaternion GetRotation() const { return m_Rotation; }
        // 축별 크기를 읽는다.
        Math::Vector3 GetScale() const { return m_Scale; }
        // 위치를 설정한다.
        void SetTranslation(Math::Vector3 value) { m_Position = value; }
        // 회전을 설정한다.
        void SetRotation(Math::Quaternion value) { m_Rotation = value; }
        // 축별 크기를 설정한다.
        void SetScale(Math::Vector3 value) { m_Scale = value; }
        // 전단 없이 R*S 기저와 위치를 렌더러에 전달한다.
        Math::AffineTransform ToAffine() const
        {
            return Math::AffineTransform(Math::Matrix3(m_Rotation) * Math::Matrix3::MakeScale(m_Scale), m_Position);
        }
        // 월드 행렬이 양수 TRS로 재현될 때만 out을 갱신한다. 전단·반사·퇴화는 거절한다.
        static bool TryFromAffine(const Math::AffineTransform& world, SceneTransform& out)
        {
            // 축 길이를 크기로 분리하고 비유한 값과 허용 범위 밖 크기를 거절한다.
            using namespace Math;
            const Vector3 axes[] = { world.GetX(), world.GetY(), world.GetZ() };
            float scale[3];
            for (int i = 0; i < 3; ++i)
            {
                scale[i] = Length(axes[i]);
                if (!std::isfinite(scale[i]) || scale[i] < 0.0001f || scale[i] > 1e8f)
                    return false;
            }

            // 정규화 축의 직교성과 오른손 기저를 검사해 전단과 반사를 걸러낸다.
            const Vector3 x = axes[0] / scale[0], y = axes[1] / scale[1], z = axes[2] / scale[2];
            if (std::abs(float(Dot(x, y))) > 1e-5f || std::abs(float(Dot(y, z))) > 1e-5f ||
                std::abs(float(Dot(z, x))) > 1e-5f || float(Dot(Cross(x, y), z)) < 0.99999f)
                return false;

            // 쿼터니언 변환 뒤 기저 재현 오차까지 확인하고 성공한 값만 게시한다.
            SceneTransform result(Quaternion(Matrix3(x, y, z)), Vector3(scale[0], scale[1], scale[2]), world.GetTranslation());
            if (!result.Normalize())
                return false;
            const AffineTransform rebuilt = result.ToAffine();
            const Vector3 rebuiltAxes[] = { rebuilt.GetX(), rebuilt.GetY(), rebuilt.GetZ() };
            for (int i = 0; i < 3; ++i)
                if (float(Length(rebuiltAxes[i] - axes[i])) > scale[i] * 2e-5f)
                    return false;
            out = result;
            return true;
        }
        // 잘못된 TRS를 거부하고 회전을 정규화한다.
        bool Normalize()
        {
            using namespace DirectX;
            const float length = XMVectorGetX(XMQuaternionLengthSq(m_Rotation));
            if (XMVector3IsNaN(m_Position) || XMVector3IsInfinite(m_Position) ||
                XMVector4IsNaN(m_Rotation) || XMVector4IsInfinite(m_Rotation) ||
                !std::isfinite(length) || length < 1.0e-12f)
                return false;
            Math::XMFLOAT3 scale;
            XMStoreFloat3(&scale, m_Scale);
            for (float value : { scale.x, scale.y, scale.z })
                if (!std::isfinite(value) || value < 0.0001f || value > 1.0e8f)
                    return false;
            m_Rotation = Math::Quaternion(XMQuaternionNormalize(m_Rotation));
            return true;
        }
    private:
        Math::Vector3 m_Position{ Math::kZero };
        Math::Quaternion m_Rotation{ Math::kIdentity };
        Math::Vector3 m_Scale{ 1.0f, 1.0f, 1.0f };
    };
}
