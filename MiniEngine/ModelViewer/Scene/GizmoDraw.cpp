#include "GizmoDraw.h"
#include "CommandContext.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CompiledShaders/GizmoVS.h"
#include "CompiledShaders/GizmoPS.h"

namespace Viewer
{
    void GizmoDraw::Triangle(GizmoMath::Point a, GizmoMath::Point b, GizmoMath::Point c, Color color)
    {
        for (const auto point : { a, b, c })
            m_Vertices.push_back({ point.X, point.Y, color.R() * color.A(),
                color.G() * color.A(), color.B() * color.A(), color.A() });
    }

    void GizmoDraw::Quad(const GizmoMath::Point points[4], Color color)
    {
        Triangle(points[0], points[1], points[2], color);
        Triangle(points[0], points[2], points[3], color);
    }

    // 두 점을 잇는 두께 있는 선을 사각형 두 삼각형으로 낸다. 길이가 0.01 px 미만이면 내지 않고 false다.
    bool GizmoDraw::Line(GizmoMath::Point a, GizmoMath::Point b, Color color, float thickness)
    {
        const float dx = b.X - a.X, dy = b.Y - a.Y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.01f)
            return false;
        const float x = -dy * thickness * 0.5f / length, y = dx * thickness * 0.5f / length;
        const GizmoMath::Point corners[] = { {a.X + x, a.Y + y}, {b.X + x, b.Y + y},
            {b.X - x, b.Y - y}, {a.X - x, a.Y - y} };
        Quad(corners, color);
        return true;
    }

    void GizmoDraw::Arrow(GizmoMath::Point a, GizmoMath::Point b, Color color)
    {
        GizmoMath::Point head[3];
        if (!GizmoMath::ArrowHead(a, b, head))
            return;
        const GizmoMath::Point base = { (head[1].X + head[2].X) * 0.5f, (head[1].Y + head[2].Y) * 0.5f };
        Line(a, base, color);
        Triangle(head[0], head[1], head[2], color);
    }

    void GizmoDraw::Render(GraphicsContext& context, float width, float height)
    {
        if (m_Vertices.empty())
            return;
        static RootSignature root;
        static GraphicsPSO pso(L"Transform Gizmo Overlay");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(0, 0);
            root.Finalize(L"Transform Gizmo", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
            const D3D12_INPUT_ELEMENT_DESC layout[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
            pso.SetRootSignature(root);
            pso.SetRasterizerState(Graphics::RasterizerTwoSided);
            pso.SetBlendState(Graphics::BlendPreMultiplied);
            pso.SetDepthStencilState(Graphics::DepthStateDisabled);
            pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
            pso.SetInputLayout(_countof(layout), layout);
            pso.SetRenderTargetFormat(Graphics::g_OverlayBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
            pso.SetVertexShader(g_pGizmoVS, sizeof(g_pGizmoVS));
            pso.SetPixelShader(g_pGizmoPS, sizeof(g_pGizmoPS));
            pso.Finalize();
            initialized = true;
        }
        for (auto& vertex : m_Vertices)
        {
            vertex.X = 2.0f * vertex.X / width - 1.0f;
            vertex.Y = 1.0f - 2.0f * vertex.Y / height;
        }
        context.SetRootSignature(root);
        context.SetPipelineState(pso);
        context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context.SetDynamicVB(0, m_Vertices.size(), sizeof(Vertex), m_Vertices.data());
        context.Draw(static_cast<UINT>(m_Vertices.size()));
        m_Vertices.clear();
    }
}
