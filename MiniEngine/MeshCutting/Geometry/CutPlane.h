#pragma once

// 절단 평면: 어떤 joint에도 묶이지 않는 world 공간의 유한 사각형과 마우스 기즈모. Model, Core에 의존하지 않는다.
// 설계: Docs/PlaneFirstCutDesign.md 2절, Docs/SceneViewControlsDesign.md 3절, Docs/ClassLayoutDesign.md 1.3절.

#include "MeshGeometry.h"
#include "../ProceduralMesh.h"
#include <cfloat>
#include <string>
#include <vector>

namespace MeshCutting
{
    // 한 팔레트의 뼈대를 어떤 자세에서 본 모습 (패치 정점과 같은 공간)
    struct SkeletonPose
    {
        std::vector<DirectX::XMFLOAT3> jointPositions;
        std::vector<uint32_t> parents;      // 팔레트 로컬 부모 joint, 없으면 kInvalidIndex
    };

    // 평면이 지나는 부모 -> 자식 뼈 선분 하나
    struct SegmentHit
    {
        uint32_t parentJoint = kInvalidIndex;     // 자를 joint (선분의 부모 쪽 끝)
        uint32_t childJoint = kInvalidIndex;
        float t = 0.0f;                     // 부모 0 -> 자식 1
        DirectX::XMFLOAT3 point = { 0.0f, 0.0f, 0.0f };     // 선분과 평면이 만나는 점
    };

    // 평면의 위치, 방향, 크기 (로컬 +Z가 법선, +X/+Y가 평면 축). 기즈모가 드래그 시작 값으로 보관한다.
    struct CutPlaneShape
    {
        DirectX::XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };    // 쿼터니언 (x, y, z, w)
        DirectX::XMFLOAT2 halfExtents = { 1.0f, 1.0f };             // 로컬 X/Y 방향 반 길이
    };

    enum class GizmoTool { Hidden, Move, Rotate, Scale };   // Q W E R

    // 이동은 world 축, 회전과 크기는 평면의 로컬 축 (U = 로컬 X, V = 로컬 Y, N = 로컬 Z = 법선)
    enum class GizmoHandle
    {
        None,
        MoveX, MoveY, MoveZ,                // 축 화살표
        MoveYZ, MoveZX, MoveXY,             // 두 축을 동시에 옮기는 사각형 (이름의 두 축)
        RotateU, RotateV, RotateN,          // 링
        ScaleU, ScaleV, ScaleUniform        // 가장자리 핸들 둘과 가운데 핸들
    };

    struct GizmoRay
    {
        DirectX::XMFLOAT3 origin = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 direction = { 0.0f, 0.0f, 1.0f };     // 길이 1
    };

    // 카메라와 창 정보. viewProj는 DirectXMath의 행 벡터 규약(p * viewProj)이고, TAA 지터는 들어 있지 않다.
    struct GizmoView
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3 eye = { 0.0f, 0.0f, 0.0f };
        float width = 1.0f;         // 창 클라이언트 크기 (렌더 해상도가 아니다)
        float height = 1.0f;
        bool reversedZ = true;      // MiniEngine은 near = 1, far = 0

        static GizmoView Make(const DirectX::XMFLOAT4X4& viewProj, const DirectX::XMFLOAT3& eye,
            float clientWidth, float clientHeight, bool reversedZ);
    };

    enum class GizmoShape { Segment, Ring, Quad, Point };

    struct GizmoPrimitive       // 핸들의 모양. 피킹과 굽기가 같은 도형을 쓴다.
    {
        GizmoHandle handle = GizmoHandle::None;
        GizmoShape shape = GizmoShape::Point;
        std::vector<DirectX::XMFLOAT3> points;      // Segment 2개, Ring 닫힌 다각형, Quad 4개, Point 1개
        DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    struct GizmoPointer
    {
        float x = 0.0f, y = 0.0f;       // 클라이언트 픽셀 (왼쪽 위가 원점)
        GizmoRay ray;
    };

    struct GizmoInput
    {
        bool cursorValid = false;   // 커서가 창 안에 있다
        float x = 0.0f, y = 0.0f;   // 클라이언트 픽셀
        bool leftDown = false;
        bool leftPressed = false;   // 이번 프레임에 눌렀다
        bool blocked = false;       // 카메라 조작 중이거나 Alt가 눌려 있다: 호버도 드래그 시작도 하지 않는다
    };

    // 드래그를 시작한 순간의 평면과 기준 값. 갱신은 모두 시작 평면에서의 변화량으로 계산해서 오차가 쌓이지 않는다.
    struct GizmoDrag
    {
        GizmoHandle handle = GizmoHandle::None;
        CutPlaneShape start;
        DirectX::XMFLOAT3 axisDir = { 1.0f, 0.0f, 0.0f };       // 이동·크기 축, 회전 축, 평면 이동의 법선
        float startParam = 0.0f;                                // 축 위의 시작 위치
        DirectX::XMFLOAT3 startHit = { 0.0f, 0.0f, 0.0f };      // 평면 위의 시작 점
        DirectX::XMFLOAT3 startVector = { 1.0f, 0.0f, 0.0f };   // 회전: 중심에서 시작 점까지의 단위 벡터
        float startExtent = 0.0f;                               // 크기: 시작할 때의 반 길이
        float startX = 0.0f, startY = 0.0f;                     // 균등 크기: 시작 커서
        float previousAngle = 0.0f, totalAngle = 0.0f;          // 회전: 직전 각도와 누적 각도
        int localAxis = 0;                                      // 회전: RotateAroundLocalAxis의 축
    };

    class CutPlane
    {
    public:
        // CutPlane에서 매번 계산한다. 축은 정규 직교이고 u x v = n이다.
        struct Frame
        {
            DirectX::XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
            DirectX::XMFLOAT3 axisU = { 1.0f, 0.0f, 0.0f };
            DirectX::XMFLOAT3 axisV = { 0.0f, 1.0f, 0.0f };
            DirectX::XMFLOAT3 normal = { 0.0f, 0.0f, 1.0f };
            DirectX::XMFLOAT2 halfExtents = { 1.0f, 1.0f };
        };

        CutPlane() {}
        CutPlane(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT4& rotation, const DirectX::XMFLOAT2& halfExtents);
        static CutPlane MakeInfinite(const DirectX::XMFLOAT3& point, const DirectX::XMFLOAT3& normal);     // 아주 큰 평면 (테스트용)
        static CutPlane MakeHorizontal(const DirectX::XMFLOAT3& center, float modelRadius);     // 법선 +Y, 크기 제한도 모델 반지름에서 정한다

        const DirectX::XMFLOAT3& GetCenter() const { return m_Shape.center; }
        const DirectX::XMFLOAT4& GetRotation() const { return m_Shape.rotation; }
        const DirectX::XMFLOAT2& GetHalfExtents() const { return m_Shape.halfExtents; }
        Frame GetFrame() const;
        SlicePlane ToSlicePlane() const;                            // 슬라이서에는 무한 평면으로 넘긴다
        bool Contains(const DirectX::XMFLOAT3& p, float margin) const;      // p를 평면에 내린 점이 사각형 안인가
        bool ContainsLoops(const MeshGeometry& positive, const std::vector<std::vector<uint32_t>>& loops, float margin,
            std::string* error = nullptr) const;

        void SetExtentLimits(float minExtent, float maxExtent);
        void Translate(const DirectX::XMFLOAT3& worldDelta);
        void RotateAroundLocalAxis(int axis, float radians);        // axis: 0 = U, 1 = V, 2 = 법선
        void ScaleUniform(float factor);
        void ScaleU(float factor);
        void ClampExtents();

        void PickSegments(const SkeletonPose& pose, std::vector<SegmentHit>& hits) const;
        static std::vector<uint32_t> CutJoints(const std::vector<SegmentHit>& hits);

        // ---- 기즈모 ----
        void SetGizmoTool(GizmoTool tool);
        GizmoTool GetGizmoTool() const { return m_Tool; }
        bool UpdateGizmo(const GizmoView& view, const GizmoInput& input);      // 드래그로 평면이 바뀌었으면 true
        void BakeGizmo(const GizmoView& view, ProceduralMeshData& out) const;
        void CancelGizmo();
        bool IsDragging() const { return m_Drag.handle != GizmoHandle::None; }
        GizmoHandle GetHoveredHandle() const { return m_Hovered; }
        GizmoHandle GetActiveHandle() const { return m_Drag.handle; }

        // 기즈모의 순수 계산. 테스트가 직접 부른다.
        static GizmoRay MakeRay(const GizmoView& view, float clientX, float clientY);
        static bool ProjectToScreen(const DirectX::XMFLOAT3& world, const GizmoView& view, float& pixelX, float& pixelY);
        static float GetGizmoUnit(const GizmoView& view, const DirectX::XMFLOAT3& center);
        std::vector<GizmoPrimitive> BuildGizmoPrimitives(const GizmoView& view, GizmoTool tool) const;
        static GizmoHandle PickFromPrimitives(const GizmoView& view, const std::vector<GizmoPrimitive>& primitives,
            float cursorX, float cursorY, float thresholdPixels = 8.0f);
        GizmoHandle PickHandle(const GizmoView& view, GizmoTool tool, float cursorX, float cursorY,
            float thresholdPixels = 8.0f) const;
        static bool ClosestParamOnAxis(const DirectX::XMFLOAT3& axisOrigin, const DirectX::XMFLOAT3& axisDir,
            const GizmoRay& ray, float& param);
        static bool RayPlaneHit(const GizmoRay& ray, const DirectX::XMFLOAT3& point, const DirectX::XMFLOAT3& normal,
            DirectX::XMFLOAT3& hit);
        bool BeginDrag(GizmoHandle handle, const GizmoPointer& pointer, GizmoDrag& drag) const;
        bool UpdateDrag(GizmoDrag& drag, const GizmoPointer& pointer);      // 이 평면을 드래그 결과로 바꾼다. 실패하면 그대로

    private:
        CutPlaneShape m_Shape;
        float m_MinExtent = 0.0f;       // halfExtents의 제한 (CutController가 모델 크기에서 정한다)
        float m_MaxExtent = FLT_MAX;
        GizmoTool m_Tool = GizmoTool::Move;
        GizmoHandle m_Hovered = GizmoHandle::None;
        GizmoDrag m_Drag;
    };
}
