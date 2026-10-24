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
        void Line(GizmoMath::Point a, GizmoMath::Point b, Color color, float thickness = 3.0f);
        void Arrow(GizmoMath::Point a, GizmoMath::Point b, Color color);
        void Quad(const GizmoMath::Point points[4], Color color);
        void Render(GraphicsContext& context, float width, float height);

    private:
        struct alignas(16) Vertex { float X, Y, R, G, B, A, Padding0 = 0.0f, Padding1 = 0.0f; };
        void Triangle(GizmoMath::Point a, GizmoMath::Point b, GizmoMath::Point c, Color color);
        std::vector<Vertex> m_Vertices;
    };
}
