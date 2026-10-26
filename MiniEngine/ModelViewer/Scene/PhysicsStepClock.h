#pragma once

#include <cmath>

namespace Viewer
{
    // 프레임 시간을 고정 step 수로 바꾸며 pause나 과부하의 시간을 다음 프레임에 넘기지 않는다.
    class PhysicsStepClock
    {
    public:
        static constexpr double FixedSeconds = 1.0 / 60.0;
        static constexpr unsigned MaxSteps = 4;

        // 실행 토글이나 장면 초기화 시 남은 분수 시간을 비운다.
        void Reset()
        {
            // 시뮬레이션 body의 속도와 sleep은 건드리지 않고 시계만 초기화한다.
            m_Accumulator = 0.0;
        }

        // 유효한 실행 시간만 누적하고 이번 프레임에서 수행할 최대 네 step을 반환한다.
        unsigned Advance(double deltaSeconds, bool running)
        {
            // pause 중에는 시간을 쌓지 않으며 이전 실행의 분수 시간도 버린다.
            if (!running)
            {
                Reset();
                return 0;
            }

            // 잘못된 delta는 이전에 모은 유효한 시간을 바꾸지 않고 무시한다.
            if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0)
                return 0;

            // 네 step에 도달하면 분수 시간을 포함한 나머지를 버려 catch-up 폭주를 막는다.
            const double limit = FixedSeconds * MaxSteps;
            if (deltaSeconds >= limit - m_Accumulator)
            {
                Reset();
                return MaxSteps;
            }

            // 한도 미만에서는 완전한 step만 소비하고 분수 시간을 다음 프레임에 보관한다.
            m_Accumulator += deltaSeconds;
            unsigned steps = 0;
            while (m_Accumulator >= FixedSeconds)
            {
                m_Accumulator -= FixedSeconds;
                ++steps;
            }
            return steps;
        }

    private:
        double m_Accumulator = 0.0;
    };
}
