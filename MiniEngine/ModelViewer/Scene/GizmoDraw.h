#pragma once

#include "GizmoMath.h"
#include "Color.h"
#include <vector>

class GraphicsContext;

namespace Viewer
{
    // A small overlay triangle batch, shared by all gizmo handle shapes.
    class GizmoDraw
    {
    public:
        // 두께 있는 선 사각형을 낸다. 너무 짧아 버렸으면 false를 돌려준다.
        bool Line(GizmoMath::Point a, GizmoMath::Point b, Color color, float thickness = 3.0f);
        void Arrow(GizmoMath::Point a, GizmoMath::Point b, Color color);
        void Quad(const GizmoMath::Point points[4], Color color);
        // 선 lineCount개(선당 정점 6개, 192 B)의 정점 용량을 미리 확보한다.
        void Reserve(size_t lineCount)
        {
            m_Vertices.reserve(m_Vertices.size() + lineCount * 6);
        }
        void Render(GraphicsContext& context, float width, float height);

    private:
        struct alignas(16) Vertex { float X, Y, R, G, B, A, Padding0 = 0.0f, Padding1 = 0.0f; };
        void Triangle(GizmoMath::Point a, GizmoMath::Point b, GizmoMath::Point c, Color color);
        std::vector<Vertex> m_Vertices;
    };
}
