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
