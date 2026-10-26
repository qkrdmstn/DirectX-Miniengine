#pragma once
#include "CollisionUnits.h"
#include <algorithm>
#include <cmath>

// Unreal BodyUtils.cpp(5.3)의 질량 계산을 m·kg 단위로 옮긴다. 형상은 균일 밀도 가정이다.
namespace MiniCollision
{
    // 기본 밀도 g/cm³. Unreal UPhysicalMaterial::Density 기본값 1.0.
    constexpr double DefaultDensityGPerCm3 = 1.0;

    // 최소 밀도 kg/m³. Unreal BodyUtils의 KgPerM3ToKgPerCm3(0.09f)(수소 밀도) 하한이다.
    constexpr double MinDensityKgPerM3 = 0.09;

    // 질량 지수. Unreal RaiseMassToPower 기본값 0.75(ApplyMassPropertiesModifiers에서 [UE_KINDA_SMALL_NUMBER, 1]로 clamp).
    constexpr double RaiseMassToPower = 0.75;

    // 최소 질량 kg. Unreal ApplyMassPropertiesModifiers의 max(Mass, 0.001) (1 g).
    constexpr double MinMassKg = 0.001;

    // 부피(m³)와 MassScale로 Unreal 방식 질량(kg)을 돌려준다.
    // 환산: V_cm³ = V_m³·1e6, 밀도 kg/cm³ = g/cm³·1e-3이므로 기본 질량 kg = V_m³·1000·밀도(g/cm³)다. 이후 pow(질량, 0.75)·MassScale을 최소 0.001 kg으로 제한한다.
    inline double CalculateUnrealMass(double volumeM3, double massScale = 1.0, double densityGPerCm3 = DefaultDensityGPerCm3)
    {
        // 밀도를 kg/m³로 바꾸고 최소 밀도 하한을 적용한다.
        const double densityKgPerM3 = std::max(MinDensityKgPerM3, densityGPerCm3 * 1000.0);

        // 균일 밀도 기본 질량에 지수를 올리고 MassScale을 곱한다.
        const double baseMass = std::max(0.0, volumeM3) * densityKgPerM3;
        const double mass = massScale * std::pow(baseMass, RaiseMassToPower);

        // 최소 질량 아래로는 내려가지 않는다.
        return std::max(mass, MinMassKg);
    }
}
