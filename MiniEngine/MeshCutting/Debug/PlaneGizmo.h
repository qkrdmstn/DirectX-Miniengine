#pragma once

// 절단 평면을 마우스로 옮기는 기즈모. 설계: Docs/SceneViewControlsDesign.md 3절.
// 광선, 피킹, 드래그 계산과 메시 굽기는 Model과 Core를 모르는 순수 함수라서 GPU 없이 테스트한다
// (tests/PlaneSlicerTests.cpp). 화면에 그리는 일은 CutDebugView가 한다.

#include "../Geometry/CutPlane.h"
#include "../Geometry/RenderBake.h"
#include <vector>

namespace MeshCutting
{
    enum class GizmoTool
    {
        Hidden,     // Q: 기즈모를 숨긴다
        Move,       // W
        Rotate,     // E
        Scale       // R
    };

    // 이동은 world 축, 회전과 크기는 평면의 로컬 축을 쓴다 (U = 로컬 X, V = 로컬 Y, N = 로컬 Z = 법선).
    enum class GizmoHandle
    {
        None,
        MoveX, MoveY, MoveZ,                // 축 화살표
        MoveYZ, MoveZX, MoveXY,             // 두 축을 동시에 옮기는 사각형 (이름의 두 축)
        RotateU, RotateV, RotateN,          // 링
        ScaleU, ScaleV, ScaleUniform        // 가장자리 핸들 둘과 가운데 핸들. 법선 핸들은 없다 (평면에는 두께가 없다)
    };

    struct GizmoRay
    {
        DirectX::XMFLOAT3 origin = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 direction = { 0.0f, 0.0f, 1.0f };     // 길이 1
    };

    // 카메라와 창 정보. viewProj는 DirectXMath의 행 벡터 규약(p * viewProj)이다. MiniEngine의 Matrix4는
    // XMMATRIX(matrix4)로 바꾸면 이 규약이 된다. 지터(TAA)는 뷰포트 오프셋으로 넣기 때문에 이 행렬에는 없다.
    struct GizmoView
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3 eye = { 0.0f, 0.0f, 0.0f };
        float width = 1.0f;         // 창 클라이언트 크기 (렌더 해상도가 아니다)
        float height = 1.0f;
        bool reversedZ = true;      // MiniEngine은 near = 1, far = 0
    };

    GizmoView MakeGizmoView(const DirectX::XMFLOAT4X4& viewProj, const DirectX::XMFLOAT3& eye,
        float clientWidth, float clientHeight, bool reversedZ);

    // 클라이언트 픽셀 좌표(왼쪽 위가 원점) -> NDC -> viewProj 역행렬로 만든 광선. 깊이 한 점은 near, 다른 한 점은
    // 그 사이의 값을 써서 far가 무한대인 투영에서도 유한한 값만 쓴다.
    GizmoRay MakeRay(const GizmoView& view, float clientX, float clientY);

    // world 점의 클라이언트 픽셀 좌표. 카메라 뒤(w <= 0)이면 false.
    bool ProjectToScreen(const DirectX::XMFLOAT3& world, const GizmoView& view, float& pixelX, float& pixelY);

    // 기즈모 한 단위의 world 길이. 화면 중앙에서 한 픽셀이 차지하는 각도로 구해서, 카메라 거리가 달라도
    // 화면에서는 같은 크기(약 100픽셀)로 보인다.
    float GizmoUnit(const GizmoView& view, const DirectX::XMFLOAT3& center);

    // ---- 핸들의 모양 ----
    // 피킹과 굽기가 같은 도형을 쓴다.
    enum class GizmoShape { Segment, Ring, Quad, Point };

    struct GizmoPrimitive
    {
        GizmoHandle handle = GizmoHandle::None;
        GizmoShape shape = GizmoShape::Point;
        std::vector<DirectX::XMFLOAT3> points;      // Segment 2개, Ring 닫힌 다각형, Quad 4개, Point 1개
        DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    // 도구에 해당하는 핸들의 도형. 크기는 GizmoUnit에서 정한다.
    std::vector<GizmoPrimitive> BuildGizmoPrimitives(const GizmoView& view, const CutPlane& plane, GizmoTool tool);

    // ---- 피킹 ----
    // 핸들을 화면에 투영해서 커서와의 픽셀 거리가 threshold 이하인 것 중, 카메라에 가장 가까운 핸들을 고른다
    // (깊이가 같으면 픽셀 거리가 작은 쪽). 카메라 뒤에 걸친 선분은 건너뛴다.
    GizmoHandle PickFromPrimitives(const GizmoView& view, const std::vector<GizmoPrimitive>& primitives,
        float cursorX, float cursorY, float thresholdPixels = 8.0f);
    GizmoHandle PickHandle(const GizmoView& view, const CutPlane& plane, GizmoTool tool,
        float cursorX, float cursorY, float thresholdPixels = 8.0f);

    // ---- 드래그 ----
    struct GizmoPointer
    {
        float x = 0.0f, y = 0.0f;       // 클라이언트 픽셀
        GizmoRay ray;
    };

    // 드래그를 시작한 순간의 평면과 기준 값. 이후 갱신은 모두 시작 평면에서의 변화량으로 계산해서 오차가 쌓이지 않는다.
    struct GizmoDrag
    {
        GizmoHandle handle = GizmoHandle::None;
        CutPlane startPlane;
        DirectX::XMFLOAT3 axisDir = { 1.0f, 0.0f, 0.0f };   // 이동·크기 축, 회전 축, 평면 이동의 법선
        float startParam = 0.0f;                            // 축 위의 시작 위치
        DirectX::XMFLOAT3 startHit = { 0.0f, 0.0f, 0.0f };  // 평면 위의 시작 점
        DirectX::XMFLOAT3 startVector = { 1.0f, 0.0f, 0.0f };   // 회전: 중심에서 시작 점까지의 단위 벡터
        float startExtent = 0.0f;                           // 크기: 시작할 때의 반 길이
        float startX = 0.0f, startY = 0.0f;                 // 균등 크기: 시작 커서
        float previousAngle = 0.0f, totalAngle = 0.0f;      // 회전: 직전 각도와 누적 각도
        int localAxis = 0;                                  // 회전: RotateAroundLocalAxis의 축
    };

    // 광선과 직선(origin + s * dir, dir는 단위 벡터)의 최근접점 매개변수 s. 두 직선이 거의 평행하면 false.
    bool ClosestParamOnAxis(const DirectX::XMFLOAT3& axisOrigin, const DirectX::XMFLOAT3& axisDir,
        const GizmoRay& ray, float& param);

    // 광선과 평면의 교점. 평행하거나 교점이 광선 뒤쪽이면 false.
    bool RayPlaneHit(const GizmoRay& ray, const DirectX::XMFLOAT3& point, const DirectX::XMFLOAT3& normal,
        DirectX::XMFLOAT3& hit);

    // 핸들을 잡은 순간의 기준을 만든다. 광선이 기준을 만들 수 없는 각도이면 false.
    bool BeginDrag(GizmoHandle handle, const CutPlane& plane, const GizmoPointer& pointer, GizmoDrag& drag);

    // 지금 커서가 가리키는 곳에 맞는 평면을 outPlane에 만든다. 크기는 [minExtent, maxExtent]로 제한한다.
    // 이번 프레임의 광선으로 값을 정할 수 없으면 false이고 outPlane은 그대로다.
    bool UpdateDrag(GizmoDrag& drag, const GizmoPointer& pointer, float minExtent, float maxExtent, CutPlane& outPlane);

    // ---- 상태 ----
    struct GizmoInput
    {
        bool cursorValid = false;   // 커서가 창 안에 있다
        float x = 0.0f, y = 0.0f;   // 클라이언트 픽셀
        bool leftDown = false;
        bool leftPressed = false;   // 이번 프레임에 눌렀다
        bool blocked = false;       // 카메라 조작 중이거나 Alt가 눌려 있다: 호버도 드래그 시작도 하지 않는다
    };

    class PlaneGizmo
    {
    public:
        void SetTool(GizmoTool tool);
        GizmoTool Tool() const { return m_Tool; }

        // 호버를 갱신하고 드래그를 진행한다. 평면이 바뀌었으면 true이고 newPlane에 새 평면을 돌려준다.
        bool Update(const GizmoView& view, const CutPlane& plane, const GizmoInput& input,
            float minExtent, float maxExtent, CutPlane& newPlane);

        bool Dragging() const { return m_Drag.handle != GizmoHandle::None; }
        GizmoHandle Hovered() const { return m_Hovered; }
        GizmoHandle Active() const { return m_Drag.handle; }
        void Cancel();

    private:
        GizmoTool m_Tool = GizmoTool::Move;
        GizmoHandle m_Hovered = GizmoHandle::None;
        GizmoDrag m_Drag;
    };

    // ---- 굽기 ----
    // 핸들을 가는 튜브로 만든다. 호버나 드래그 중인 핸들은 노랗고 조금 굵다. 굽기 전에 out을 비운다.
    void BakeGizmo(const GizmoView& view, const CutPlane& plane, GizmoTool tool,
        GizmoHandle hovered, GizmoHandle active, ProceduralMeshData& out);
}
