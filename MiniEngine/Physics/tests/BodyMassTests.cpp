#include "CollisionTestUtils.h"
#include "Collision/BodyMass.h"
#include <algorithm>
#include <cstdio>

// Unreal 질량 공식을 m 단위와 cm 단위 계산에서 비교한다.
static void TestMass()
{
    // 1 m^3는 1000 kg의 0.75제곱이다.
    CheckNear(CalculateUnrealMass(1.0), std::pow(1000.0, 0.75), 1e-9, "1 m3 mass");
    CheckNear(CalculateUnrealMass(1.0), 177.8279410038923, 1e-9, "1 m3 literal");

    // 아주 작은 부피는 최소 질량 0.001 kg으로 제한된다.
    CheckNear(CalculateUnrealMass(1e-9), 0.001, 0.0, "mass floor");

    // MassScale은 지수 적용 후 곱해진다.
    CheckNear(CalculateUnrealMass(1.0, 2.0), 2.0 * std::pow(1000.0, 0.75), 1e-9, "mass scale");

    // 밀도가 최소보다 낮으면 0.09 kg/m^3로 제한된다.
    CheckNear(CalculateUnrealMass(1.0, 1.0, 0.0), std::pow(0.09, 0.75), 1e-12, "density floor");

    // Unreal cm 단위로 직접 계산한 값과 일치한다.
    const double volumesM3[] = { 0.5, 1.0, 2.37, 0.01 };
    for (double vM3 : volumesM3)
    {
        const double volumeCm3 = vM3 * 1e6;
        const double kgPerCm3 = std::max(0.09 * 1e-6, 1.0 * (1.0 / 1000.0));
        const double unreal = std::max(1.5 * std::pow(volumeCm3 * kgPerCm3, 0.75), 0.001);
        CheckNear(CalculateUnrealMass(vM3, 1.5), unreal, unreal * 1e-12, "cm units match");
    }
}

// 질량 테스트를 실행하고 검사 횟수를 출력한다.
int main()
{
    // 테스트를 실행하고 예외가 나면 실패로 끝낸다.
    try
    {
        TestMass();
    }
    catch (const std::exception& e)
    {
        // 실패 메시지와 지금까지의 검사 횟수를 출력한다.
        std::printf("FAILED: %s (after %zu checks)\n", e.what(), g_Checks);
        return 1;
    }

    // 통과한 검사 횟수를 출력한다.
    std::printf("BodyMassTests passed: %zu checks\n", g_Checks);
    return 0;
}
