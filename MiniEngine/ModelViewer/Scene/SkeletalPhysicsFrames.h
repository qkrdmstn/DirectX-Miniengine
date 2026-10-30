#pragma once

#include "VectorMath.h"
#include "../../Physics/Skeletal/SkeletalPhysicsAsset.h"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Viewer
{
    namespace SkeletalFrames
    {
        // 모든 성분이 유한하고 마지막 행이 affine인지 검사한다.
        inline bool IsFiniteAffine(const Math::Matrix4& matrix)
        {
            // Math는 열벡터 순서를 쓰며 DirectX 저장 행은 그 열에 해당한다.
            DirectX::XMFLOAT4X4 values;
            DirectX::XMStoreFloat4x4(&values, matrix);
            for (const auto& column : values.m)
                for (float value : column)
                    if (!std::isfinite(value)) return false;
            return std::abs(values._14) <= 1e-6f && std::abs(values._24) <= 1e-6f &&
                std::abs(values._34) <= 1e-6f && std::abs(values._44 - 1.0f) <= 1e-6f;
        }

        // 중립 강체 frame을 검증하고 Math 행렬로 옮긴다.
        inline bool TryRigidMatrix(const MiniPhysics::SkeletalRigidFrame& frame, Math::Matrix4& output)
        {
            // double 입력을 float로 옮기기 전에 회전 단위 길이와 유한값을 검사한다.
            double norm = 0;
            for (double value : frame.Rotation)
            {
                if (!std::isfinite(value)) return false;
                norm += value * value;
            }
            if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-4) return false;
            for (double value : frame.Position)
                if (!std::isfinite(value) || std::abs(value) > FLT_MAX) return false;

            // 미세한 쿼터니언 오차만 정규화하고 성공한 행렬을 게시한다.
            const auto& q = frame.Rotation;
            const auto& p = frame.Position;
            const Math::Quaternion rotation(DirectX::XMVectorSet(float(q[0]), float(q[1]), float(q[2]), float(q[3])));
            output = Math::Matrix4(Math::OrthogonalTransform(Math::Normalize(rotation), Math::Vector3(float(p[0]), float(p[1]), float(p[2]))));
            return true;
        }

        // 양수 균일 배율을 강체 frame에서 분리하며 실패하면 두 출력을 보존한다.
        inline bool TrySplitUniformFrame(const Math::Matrix4& matrix,
            MiniPhysics::SkeletalRigidFrame& frame, float& scale)
        {
            // 열 길이의 상대 오차 1e-4와 정규화 축의 전단 오차 1e-5를 검사한다.
            using namespace Math;
            if (!IsFiniteAffine(matrix)) return false;

            // 길이 제곱과 정규화를 double로 계산해 float 범위 안의 큰 배율과 작은 배율을 보존한다.
            const Vector3 axes[] = { Vector3(matrix.GetX()), Vector3(matrix.GetY()), Vector3(matrix.GetZ()) };
            double lengths[3];
            Vector3 normalized[3];
            for (int i = 0; i < 3; ++i)
            {
                DirectX::XMFLOAT3 v;
                DirectX::XMStoreFloat3(&v, axes[i]);
                lengths[i] = std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z);
                if (lengths[i] <= 0 || lengths[i] > FLT_MAX) return false;
                normalized[i] = Vector3(float(v.x / lengths[i]), float(v.y / lengths[i]), float(v.z / lengths[i]));
            }
            const double maximum = (std::max)({ lengths[0], lengths[1], lengths[2] });
            const double minimum = (std::min)({ lengths[0], lengths[1], lengths[2] });
            if (maximum - minimum > maximum * 1e-4) return false;
            const Vector3 x = normalized[0], y = normalized[1], z = normalized[2];
            if (std::abs(float(Dot(x, y))) > 1e-5f || std::abs(float(Dot(y, z))) > 1e-5f ||
                std::abs(float(Dot(z, x))) > 1e-5f || float(Dot(Cross(x, y), z)) < 0.99999f) return false;

            // 배율을 제거한 오른손 기저만 쿼터니언으로 옮긴다.
            DirectX::XMFLOAT4 q;
            DirectX::XMFLOAT3 p;
            DirectX::XMStoreFloat4(&q, Normalize(Quaternion(Matrix3(x, y, z))));
            DirectX::XMStoreFloat3(&p, Vector3(matrix.GetW()));
            const MiniPhysics::SkeletalRigidFrame result{ { p.x, p.y, p.z }, { q.x, q.y, q.z, q.w } };
            frame = result;
            scale = float(lengths[0]);
            return true;
        }

        // E*Gnode의 배율을 제거한 뒤 월드 m 단위 body offset을 한 번 적용한다.
        inline bool TryBuildBodyFrame(const Math::Matrix4& modelToWorld, const Math::Matrix4& nodePose,
            const MiniPhysics::SkeletalRigidFrame& boneFromBody,
            MiniPhysics::SkeletalRigidFrame& bodyWorld, float& nodeScale)
        {
            // 입력 frame과 합성 본 pose를 모두 검증한다.
            if (!IsFiniteAffine(modelToWorld) || !IsFiniteAffine(nodePose)) return false;
            MiniPhysics::SkeletalRigidFrame bone;
            float scale = 0;
            Math::Matrix4 offset, rigidBone;
            if (!TryRigidMatrix(boneFromBody, offset) ||
                !TrySplitUniformFrame(modelToWorld * nodePose, bone, scale) || !TryRigidMatrix(bone, rigidBone)) return false;

            // offset에는 본 배율을 곱하지 않으며 성공한 결과만 게시한다.
            MiniPhysics::SkeletalRigidFrame result;
            float rigidScale = 0;
            if (!TrySplitUniformFrame(rigidBone * offset, result, rigidScale)) return false;
            bodyWorld = result;
            nodeScale = scale;
            return true;
        }

        // body 원점에서 offset을 제거하고 저장된 배율을 복원해 model node pose로 되돌린다.
        inline bool TryRestoreNodePose(const Math::Matrix4& modelToWorld,
            const MiniPhysics::SkeletalRigidFrame& bodyWorld,
            const MiniPhysics::SkeletalRigidFrame& boneFromBody, float nodeScale, Math::Matrix4& nodePose)
        {
            // 역변환 가능한 E와 유한한 양수 배율 및 두 강체 frame을 확인한다.
            using namespace Math;
            if (!IsFiniteAffine(modelToWorld) || !std::isfinite(nodeScale) || nodeScale <= 0) return false;
            Matrix4 body, offset;
            if (!TryRigidMatrix(bodyWorld, body) || !TryRigidMatrix(boneFromBody, offset)) return false;
            const Matrix4 inverseE = Invert(modelToWorld);
            if (!IsFiniteAffine(inverseE)) return false;

            // G = inverse(E) * B * inverse(offset) * S(s)이며 출력은 검증 뒤 게시한다.
            const Matrix4 result = inverseE * body * Invert(offset) * Matrix4::MakeScale(nodeScale);
            if (!IsFiniteAffine(result)) return false;
            nodePose = result;
            return true;
        }
    }
}
