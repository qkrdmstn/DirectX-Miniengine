// VS Developer PowerShell, 저장소 루트에서 실행하며 산출물은 TEMP에 둔다.
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /Fo:%TEMP%/PhysicsStepClockTests.obj
//    /Fe:%TEMP%/PhysicsStepClockTests.exe MiniEngine/ModelViewer/Scene/tests/PhysicsStepClockTests.cpp
#include "../PhysicsStepClock.h"
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace
{
    unsigned Checks = 0;

    // Release에서도 검사 실패를 예외로 보고한다.
    void Check(bool condition, const char* message)
    {
        // 실행한 검사 수를 세고 거짓이면 즉시 실패시킨다.
        ++Checks;
        if (!condition)
            throw std::runtime_error(message);
    }

    // 정상 프레임의 분수 시간 누적과 최대 step 수를 검사한다.
    void TestAccumulation()
    {
        // 반 step 두 번은 정확히 한 step이 된다.
        Viewer::PhysicsStepClock clock;
        const double dt = Viewer::PhysicsStepClock::FixedSeconds;
        Check(clock.Advance(dt * 0.5, true) == 0, "Half step stays pending");
        Check(clock.Advance(dt * 0.5, true) == 1, "Two half steps produce one step");
        Check(clock.Advance(dt * 3, true) == 3, "Three steps fit in one frame");

        // 120 Hz의 일초 동안 60번의 고정 step을 소비한다.
        clock.Reset();
        unsigned steps = 0;
        for (int frame = 0; frame < 120; ++frame)
            steps += clock.Advance(dt * 0.5, true);
        Check(steps == 60, "120 Hz input produces 60 physics ticks");
    }

    // 긴 프레임은 네 step만 실행하고 분수 시간을 포함한 backlog를 폐기한다.
    void TestOverload()
    {
        // 반 step이 남은 상태의 긴 프레임도 최대 네 step만 돌린다.
        Viewer::PhysicsStepClock clock;
        const double dt = Viewer::PhysicsStepClock::FixedSeconds;
        Check(clock.Advance(dt * 0.5, true) == 0, "Pending fraction before overload");
        Check(clock.Advance(0.5, true) == 4, "Half-second stall capped at four ticks");
        Check(clock.Advance(dt * 0.5, true) == 0, "Overload discards old fraction");
        Check(clock.Advance(dt * 0.5, true) == 1, "Fresh fractions accumulate after overload");

        // 정확한 한도와 가장 큰 유한 delta도 오버플로 없이 처리한다.
        Check(clock.Advance(dt * 4, true) == 4, "Exact four-step limit");
        Check(clock.Advance(std::numeric_limits<double>::max(), true) == 4, "Huge finite delta capped");
        Check(clock.Advance(dt * 0.5, true) == 0, "Huge delta leaves no backlog");
    }

    // pause와 명시적 토글 초기화는 body 상태와 독립적인 시계 시간만 비운다.
    void TestPauseAndInvalidTime()
    {
        // pause 전후의 분수 시간을 합치지 않는다.
        Viewer::PhysicsStepClock clock;
        const double dt = Viewer::PhysicsStepClock::FixedSeconds;
        Check(clock.Advance(dt * 0.5, true) == 0, "Pending fraction before pause");
        Check(clock.Advance(10, false) == 0, "Pause ignores elapsed time");
        Check(clock.Advance(dt * 0.5, true) == 0, "Resume has no old fraction");

        // 무효한 프레임은 직전 유효한 분수 시간을 보존한다.
        Check(clock.Advance(0, true) == 0, "Zero delta ignored");
        Check(clock.Advance(-1, true) == 0, "Negative delta ignored");
        Check(clock.Advance(std::numeric_limits<double>::quiet_NaN(), true) == 0, "NaN delta ignored");
        Check(clock.Advance(std::numeric_limits<double>::infinity(), true) == 0, "Infinite delta ignored");
        Check(clock.Advance(dt * 0.5, true) == 1, "Valid fraction survives invalid frames");

        // pause 프레임 없이 토글하더라도 호출자의 Reset으로 backlog가 제거된다.
        Check(clock.Advance(dt * 0.5, true) == 0, "Pending fraction before explicit reset");
        clock.Reset();
        Check(clock.Advance(dt * 0.5, true) == 0, "Explicit reset clears toggle fraction");
    }
}

// 누적·과부하·pause 검사를 실행하고 실패하면 비정상 종료한다.
int main()
{
    try
    {
        // 모든 시계 상태 전이를 검사한다.
        TestAccumulation();
        TestOverload();
        TestPauseAndInvalidTime();
        std::printf("PhysicsStepClockTests: %u checks passed\n", Checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        // 실패 원인을 남겨 빌드 구성과 무관하게 검출한다.
        std::fprintf(stderr, "PhysicsStepClockTests failed: %s\n", error.what());
        return 1;
    }
}
