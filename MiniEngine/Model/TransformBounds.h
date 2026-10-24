#pragma once
#include "VectorMath.h"
#include "Math/BoundingSphere.h"
#include <cmath>
#include <algorithm>

// A^T*A의 최대 절대 행합으로 affine 최대 신장의 상한을 구한다.
// 직교 TRS에서는 최대 축 크기이며 합성 전단이 있어도 보수적인 경계다.
inline float AffineRadiusScale(const Math::AffineTransform& transform)
{
    const Math::Vector3 axes[] = { transform.GetX(), transform.GetY(), transform.GetZ() };
    float bound = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        float sum = 0.0f;
        for (int j = 0; j < 3; ++j)
            sum += std::abs(float(Math::Dot(axes[i], axes[j])));
        bound = (std::max)(bound, sum);
    }
    return std::sqrt(bound);
}

// 중심에는 전체 affine을, 반지름에는 최대 신장 상한을 적용한다.
inline Math::BoundingSphere TransformBoundingSphere(const Math::AffineTransform& transform,
    const Math::BoundingSphere& sphere)
{
    return Math::BoundingSphere(transform * sphere.GetCenter(), AffineRadiusScale(transform) * sphere.GetRadius());
}
