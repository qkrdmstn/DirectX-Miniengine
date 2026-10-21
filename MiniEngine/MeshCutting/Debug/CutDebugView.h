#pragma once

// 절단 데모의 화면 쪽: 작업별 절단 조각, 평면, 교차 선분, 기즈모, 상태 줄. 컨트롤러를 읽기만 한다.

#include "../Geometry/CutPlane.h"
#include "../ProceduralMesh.h"
#include "../Rendering/ProceduralMeshRenderer.h"
#include "../Runtime/CutJob.h"
#include "VectorMath.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>

class GraphicsContext;
class TextContext;

namespace MeshCutting
{
    class CutController;

    // V: 렌더링 모드. 원본 모델과 잘린 조각에 같이 적용된다.
    enum class RenderMode
    {
        Solid,
        SolidAndWire,
        Wire,
        Debug       // Wire + 구역(section) 분리 + 경계 튜브 + 작업 색 선
    };

    class CutDebugView
    {
    public:
        void SetRenderMode(RenderMode mode) { m_RenderMode = mode; }
        void CycleRenderMode() { m_RenderMode = RenderMode((int(m_RenderMode) + 1) % 4); }
        RenderMode GetRenderMode() const { return m_RenderMode; }
        static const wchar_t* RenderModeName(RenderMode mode);

        // 기즈모를 그릴 때 필요한 카메라 정보. 프레임마다 Draw 전에 부른다. 도구, 호버, 드래그 상태는 평면이 가지고 있다.
        void SetGizmoView(const GizmoView& view);

        // 컨트롤러의 작업에 맞춰 메시를 갱신하고(사라진 작업의 메시는 renderer에서 해제) 그린다.
        void Draw(GraphicsContext& context, ProceduralMeshRenderer& renderer, const CutController& cut,
            const Math::Matrix4& viewProj, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor);

        // 상태 줄. jointName은 (대상, 팔레트, 팔레트 안 joint 번호)를 이름으로 바꾼다 (뷰는 Model을 모른다).
        using JointNameFunction = std::function<std::wstring(CutTargetId, uint32_t, uint32_t)>;
        void DrawUI(TextContext& text, const CutController& cut, const JointNameFunction& jointName) const;

        // 모든 메시의 GPU 버퍼를 renderer에 돌려준다 (renderer를 Shutdown하기 전에).
        void Release(ProceduralMeshRenderer& renderer);

    private:
        // 작업 하나의 표시 메시. std::map에 두어 주소가 바뀌지 않는다.
        struct JobMeshes
        {
            ProceduralMesh positive, negative;
            ProceduralMesh boundaryPositive, boundaryNegative;
            uint64_t pieceGeneration = 0;
            bool bakedSectionSplit = false;
        };

        void Sync(ProceduralMeshRenderer& renderer, const CutController& cut);
        bool SectionSplit() const { return m_RenderMode == RenderMode::Debug; }

        std::map<CutJobKey, JobMeshes> m_JobMeshes;
        ProceduralMesh m_PlaneDebug;
        ProceduralMesh m_SegmentDebug;          // 평면이 지나는 뼈 선분 (모든 작업)
        ProceduralMesh m_GizmoMesh;

        RenderMode m_RenderMode = RenderMode::Solid;
        GizmoView m_GizmoView;
        bool m_HaveGizmoView = false;
    };
}
