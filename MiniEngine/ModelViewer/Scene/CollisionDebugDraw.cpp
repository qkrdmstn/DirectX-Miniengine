#include "CollisionDebugDraw.h"
#include "CollisionDebugMath.h"
#include "GizmoDraw.h"
#include "Utility.h"
#include <array>
#include <chrono>

namespace Viewer
{
    namespace
    {
        constexpr float LineThickness = 1.5f;

        // 한 hull의 뷰 공간 정점과 남긴 모서리, 그릴 색을 모은 작업이다.
        struct HullJob
        {
            std::vector<CollisionDebug::ViewPoint> Points;
            std::vector<CollisionDebug::HullEdge> Edges;
            CollisionDebugDraw::ColorIndex Color;
        };

        // 색 인덱스에 대응하는 선 색을 돌려준다.
        Color LineColor(CollisionDebugDraw::ColorIndex index)
        {
            // 깨어 있음 초록, 잠듦 파랑, body 없음 노랑, 바닥 회색이다.
            static const Color colors[CollisionDebugDraw::ColorCount] = {
                Color(0.20f, 0.90f, 0.25f), Color(0.25f, 0.50f, 1.00f),
                Color(1.00f, 0.85f, 0.10f), Color(0.60f, 0.60f, 0.60f) };
            return colors[index];
        }

        // body 상태로 메시 hull의 선 색을 정한다.
        CollisionDebugDraw::ColorIndex ColorForBody(PhysicsSystem::BodyState state)
        {
            // body가 없으면 노랑, 있으면 깨어 있음/잠듦으로 나눈다.
            if (state == PhysicsSystem::BodyState::Awake)
                return CollisionDebugDraw::Green;
            if (state == PhysicsSystem::BodyState::Asleep)
                return CollisionDebugDraw::Blue;
            return CollisionDebugDraw::Yellow;
        }

        // 뷰 공간 선분을 근평면으로 자르고 화면에 투영해 선 하나를 낸다. 그렸으면 true다.
        bool EmitSegment(GizmoDraw& draw, const Math::Camera& camera, CollisionDebug::ViewPoint a,
            CollisionDebug::ViewPoint b, float width, float height, Color color)
        {
            // 카메라 뒤쪽 부분을 투영 전에 잘라낸다.
            if (!CollisionDebug::ClipSegmentToNearPlane(a, b, camera.GetNearClip()))
                return false;

            // 잘린 두 끝점을 화면 좌표로 투영한다.
            GizmoMath::Point pa;
            GizmoMath::Point pb;
            if (!GizmoMath::Project(Math::Vector3(a.X, a.Y, a.Z), camera.GetProjMatrix(), width, height, pa) ||
                !GizmoMath::Project(Math::Vector3(b.X, b.Y, b.Z), camera.GetProjMatrix(), width, height, pb))
                return false;

            // 가는 overlay 선으로 내고 실제로 정점이 만들어졌는지 돌려준다.
            return draw.Line(pa, pb, color, LineThickness);
        }

        // 월드 점을 카메라 뷰 공간 점으로 바꾼다.
        CollisionDebug::ViewPoint ToView(const Math::Matrix4& view, Math::Vector3 world)
        {
            // 뷰 행렬은 아핀이므로 동차 나눗셈 없이 성분을 읽는다.
            const Math::Vector3 v = Math::MakeHomogeneous(view * world);
            return { float(v.GetX()), float(v.GetY()), float(v.GetZ()) };
        }
    }

    // 충돌이 있는 메시와 바닥 상자의 모서리를 모아 한 번에 overlay 선으로 낸다.
    void CollisionDebugDraw::Draw(GraphicsContext& context, const Scene& scene, const PhysicsSystem& physics,
        const Math::Camera& camera, float width, float height)
    {
        // CPU 시간 측정을 시작하고 통계를 비운다.
        const auto start = std::chrono::steady_clock::now();
        m_Stats = Stats();
        const Math::Matrix4& view = camera.GetViewMatrix();

        // 충돌 상태가 FromModel/Inherited인 메시의 로컬 hull을 월드 변환해 모서리를 만든다.
        std::vector<HullJob> jobs;
        size_t totalEdges = 0;
        for (const auto& object : scene.GetObjects())
        {
            for (const auto& mesh : object->GetMeshComponents())
            {
                const ComponentCollisionStatus status = mesh->GetCollisionStatus();
                if (status != ComponentCollisionStatus::FromModel && status != ComponentCollisionStatus::Inherited)
                    continue;
                if (physics.IsFloorMesh(object->GetId(), mesh->GetId()))
                    continue;

                const ColorIndex color = ColorForBody(physics.GetBodyState(object->GetId(), mesh->GetId()));
                const Math::AffineTransform world = mesh->GetWorldTransform(object->GetTransform());
                for (const auto& elem : mesh->GetCollision().ConvexElems)
                {
                    HullJob job;
                    job.Color = color;
                    job.Points.reserve(elem.VertexData.size());
                    for (const auto& vertex : elem.VertexData)
                        job.Points.push_back(ToView(view, world * Math::Vector3(float(vertex.x), float(vertex.y), float(vertex.z))));
                    CollisionDebug::ExtractHullEdges(elem.VertexData, elem.IndexData, job.Edges);
                    if (job.Edges.empty())
                        continue;
                    m_Stats.Extracted[color] += uint32_t(job.Edges.size());
                    totalEdges += job.Edges.size();
                    ++m_Stats.Hulls;
                    jobs.push_back(std::move(job));
                }
            }
        }

        // 바닥은 모델 hull이 아니라 실제 Static Box 12개 모서리를 회색으로 그린다.
        std::array<double, 3> center;
        std::array<double, 3> half;
        const bool hasFloor = physics.GetFloorBox(center, half);
        CollisionDebug::ViewPoint corners[8];
        if (hasFloor)
        {
            for (int i = 0; i < 8; ++i)
                corners[i] = ToView(view, Math::Vector3(
                    float(center[0] + ((i & 1) ? half[0] : -half[0])),
                    float(center[1] + ((i & 2) ? half[1] : -half[1])),
                    float(center[2] + ((i & 4) ? half[2] : -half[2]))));
            m_Stats.Extracted[Gray] = 12;
            totalEdges += 12;
        }

        // 예상 선 수만큼 정점 용량을 먼저 잡고 hull 모서리를 낸다.
        GizmoDraw draw;
        draw.Reserve(totalEdges);
        for (const HullJob& job : jobs)
            for (const auto& edge : job.Edges)
                if (EmitSegment(draw, camera, job.Points[edge.A], job.Points[edge.B], width, height, LineColor(job.Color)))
                    ++m_Stats.Drawn[job.Color];

        // 상자 모서리는 한 비트만 다른 꼭짓점 쌍 12개이다.
        if (hasFloor)
            for (int i = 0; i < 8; ++i)
                for (int bit = 1; bit <= 4; bit <<= 1)
                    if (!(i & bit) &&
                        EmitSegment(draw, camera, corners[i], corners[i | bit], width, height, LineColor(Gray)))
                        ++m_Stats.Drawn[Gray];

        // 모은 선을 한 번에 그린다.
        draw.Render(context, width, height);

        // 시간을 누적하고 통계가 바뀌었을 때만 로그를 남긴다.
        m_Stats.Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ++m_DrawFrames;
        m_TotalMilliseconds += m_Stats.Milliseconds;
        m_MaxMilliseconds = std::max(m_MaxMilliseconds, m_Stats.Milliseconds);
        bool changed = m_Stats.Hulls != m_LastLogged.Hulls;
        for (int i = 0; i < ColorCount; ++i)
            changed |= m_Stats.Extracted[i] != m_LastLogged.Extracted[i] || m_Stats.Drawn[i] != m_LastLogged.Drawn[i];
        if (m_Logging && changed)
        {
            Utility::Printf("Collision draw: frame=%llu hulls=%u extracted(g/b/y/gray)=%u/%u/%u/%u drawn=%u/%u/%u/%u ms=%.3f\n",
                static_cast<unsigned long long>(m_DrawFrames), m_Stats.Hulls,
                m_Stats.Extracted[Green], m_Stats.Extracted[Blue], m_Stats.Extracted[Yellow], m_Stats.Extracted[Gray],
                m_Stats.Drawn[Green], m_Stats.Drawn[Blue], m_Stats.Drawn[Yellow], m_Stats.Drawn[Gray], m_Stats.Milliseconds);
            m_LastLogged = m_Stats;
        }
    }

    // 누적 프레임 수와 평균·최대 CPU 시간, 마지막 통계를 출력한다.
    void CollisionDebugDraw::PrintSummary() const
    {
        // 한 번도 그리지 않았다면 출력하지 않는다.
        if (!m_Logging || m_DrawFrames == 0)
            return;

        // 평균과 최대 시간 및 마지막 색별 모서리 수를 낸다.
        Utility::Printf("Collision draw summary: frames=%llu avgMs=%.4f maxMs=%.4f hulls=%u extracted(g/b/y/gray)=%u/%u/%u/%u drawn=%u/%u/%u/%u\n",
            static_cast<unsigned long long>(m_DrawFrames), m_TotalMilliseconds / double(m_DrawFrames), m_MaxMilliseconds,
            m_Stats.Hulls, m_Stats.Extracted[Green], m_Stats.Extracted[Blue], m_Stats.Extracted[Yellow], m_Stats.Extracted[Gray],
            m_Stats.Drawn[Green], m_Stats.Drawn[Blue], m_Stats.Drawn[Yellow], m_Stats.Drawn[Gray]);
    }
}
