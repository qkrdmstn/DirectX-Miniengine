#pragma once

#include "Scene.h"
#include "PhysicsSystem.h"
#include "Camera.h"
#include <cstdint>

class GraphicsContext;

namespace Viewer
{
    // 조각별 충돌 convex hull 모서리를 화면 overlay 선으로 그린다. 캐시 없이 매 프레임 컴포넌트 충돌에서 다시 만든다.
    class CollisionDebugDraw
    {
    public:
        enum ColorIndex { Green, Blue, Yellow, Gray, ColorCount };

        // 마지막 Draw의 색별 모서리 수와 CPU 시간이다. Extracted는 근평면 자르기 전, Drawn은 자른 뒤 실제로 낸 선이다.
        struct Stats
        {
            uint32_t Hulls = 0;
            uint32_t Extracted[ColorCount] = {};
            uint32_t Drawn[ColorCount] = {};
            double Milliseconds = 0.0;
        };

        // 켜져 있을 때 호출한다. 충돌 hull과 바닥 상자의 모서리를 모아 선 overlay로 그린다.
        void Draw(GraphicsContext& context, const Scene& scene, const PhysicsSystem& physics,
            const Math::Camera& camera, float width, float height);
        // 통계가 바뀔 때만 로그를 출력할지 정한다(-collision-draw-log 1).
        void SetLogging(bool enabled) { m_Logging = enabled; }
        // 마지막 Draw의 통계를 읽는다.
        const Stats& GetStats() const { return m_Stats; }
        // 로그 사용 시 누적 프레임 수와 평균·최대 CPU 시간을 한 번 출력한다.
        void PrintSummary() const;

    private:
        Stats m_Stats;
        Stats m_LastLogged;
        bool m_Logging = false;
        uint64_t m_DrawFrames = 0;
        double m_TotalMilliseconds = 0.0;
        double m_MaxMilliseconds = 0.0;
    };
}
