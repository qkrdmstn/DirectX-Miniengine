#pragma once

// 이 파일은 평면으로 조각을 자르는 공개 API(SlicePatch)를 담는다. 아래쪽 "내부용" 구역은 Slice.cpp가
// SliceCap.cpp의 루프 추출과 캡 생성을 부르기 위한 선언이다.

#include "Patch.h"

namespace MeshCutting
{
    enum class CapMode
    {
        None,       // no cap faces; the cut boundary stays open (loops are still checked)
        Create      // fill every cut loop with cap faces that share the loop nodes
    };

    struct SliceOptions
    {
        CapMode cap = CapMode::Create;
        float epsilonScale = kDefaultEpsilonScale;     // x piece bounds radius
        float capUvScale = 1.0f;
    };

    struct SliceResult
    {
        SliceStatus status = SliceStatus::InvalidInput;
        PatchPiece positive, negative;      // valid only when status == Ok
        // Cut loops in the positive piece's node numbering, in surface edge direction.
        std::vector<std::vector<uint32_t>> loops;
        std::vector<uint32_t> loopDepth;    // per loop: 0 = outer boundary, 1 = hole
        std::vector<double> loopArea;       // per loop: signed area in the plane basis (positive piece)
        std::string detail;                 // where a failure happened (face / edge / node ids)
    };

    // Cuts a piece with a plane. The positive piece keeps the side the normal points to.
    //
    // in is never modified. out is cleared first; on any failure only status and detail are
    // filled, so a caller that keeps its old pieces on !Ok never sees half-written results.
    // A plane that leaves every face on one side (touching included) returns NoCut.
    // Two shells on opposite sides of the plane with no crossing face are Ok with no loops.
    // Cut loops are extracted and validated from each piece's Cut boundary in every cap mode
    // (OpenLoop, NonManifoldLoop, InvalidLoop); CapMode::None only skips the cap faces.
    SliceStatus SlicePatch(const PatchPiece& in, const SlicePlane& plane, uint32_t cutId,
        const SliceOptions& options, SliceResult& out);

    // ---- Unreal 모양의 진입점 (UKismetProceduralMeshLibrary::SliceProceduralMesh 대응) ----

    // Unreal EProcMeshSliceCapOption 대응
    enum class SliceCapOption
    {
        NoCap,                      // cap 면을 만들지 않는다 (루프 검사는 한다)
        CreateNewSectionForCap,     // cap 면을 capMaterialSlot 재질의 새 section에 굽는다
        UseLastSectionForCap        // cap 면을 마지막 표면 section에 합쳐 굽는다
    };

    // SlicePatch를 Unreal 인자 순서로 감싼다. 성공하면 inOutMesh는 법선 쪽 조각이 되고, createOtherHalf이고
    // outOtherHalf가 있으면 반대쪽 조각을 받는다(createOtherHalf가 거짓이면 빈 조각). 실패하면 inOutMesh, outOtherHalf, outLoops를 바꾸지 않는다.
    // 평면은 패치와 같은 world 공간이다 (Unreal은 컴포넌트 로컬로 옮긴다고 추정되지만 패치가 이미 world라 옮기지 않는다).
    // cutId는 inOutMesh에 있는 가장 큰 cutId + 1이다. outLoops는 이 프로젝트의 확장으로, 양의 쪽 번호의 절단 루프를 받는다.
    bool SliceProceduralMesh(PatchPiece& inOutMesh, const DirectX::XMFLOAT3& planePosition,
        const DirectX::XMFLOAT3& planeNormal, bool createOtherHalf, PatchPiece* outOtherHalf,
        SliceCapOption capOption, uint32_t capMaterialSlot, SliceStatus* outStatus = nullptr,
        std::string* error = nullptr, std::vector<std::vector<uint32_t>>* outLoops = nullptr);

    // ---- 내부용: Slice.cpp -> SliceCap.cpp (CutLoops, CapBuilder) ----

    struct SliceLoop
    {
        std::vector<uint32_t> nodes;    // piece node ids, in the direction of the surface cut edges
        double signedArea = 0.0;        // in SlicePlaneBasis (u, v); the outer loop of the positive piece is > 0
        uint32_t depth = 0;             // 0 = outer boundary, 1 = hole
        uint32_t parent = kNoNode;      // index of the containing loop (holes only)
    };

    // Collects the Cut boundary edges of one piece whose ends both lie on the plane and turns
    // them into validated loops (design 1.6, 3.3, 3.4):
    //  - every node must have exactly one outgoing and one incoming cut edge;
    //  - a chain that ends at a Replacement/UnweldedSeam boundary is OpenLoop, anything else
    //    that is not a simple cycle is NonManifoldLoop;
    //  - loops are projected into the plane; self or mutual crossings and touching (closer than
    //    epsilon) are InvalidLoop;
    //  - nesting deeper than outer + hole is InvalidLoop, and the winding of every loop must
    //    match its depth: an outer loop of the positive piece is counter-clockwise in
    //    (u, v), of the negative piece clockwise, and holes are the opposite.
    //
    // onPlane[node] != 0 marks nodes that lie on the cutting plane (new cut nodes and nodes
    // classified 0). Cut edges that are not on the plane belong to an older cut and are ignored.
    // loops is cleared first. detail names the offending node, edge or loop.
    SliceStatus ExtractCutLoops(const PatchPiece& piece, const std::vector<uint8_t>& onPlane,
        const SlicePlane& plane, bool positiveSide, float epsilonScale,
        std::vector<SliceLoop>& loops, std::string* detail = nullptr);

    // Fills the cut loops of one output piece with cap faces (design 1.8, 3.5).
    //
    // The cap uses only loop nodes (no new points), so it shares every rim node with the surface
    // and cannot leave a gap. Each outer loop is triangulated together with its holes by ear
    // clipping: holes are joined to the outer ring with bridge edges, and nodes that lie on a
    // straight stretch of the rim are kept (they are never an ear tip), so no T-junction appears.
    //
    // Cap faces wind so that the cap faces away from the cutting plane's kept side: the normal of
    // the positive piece's cap is -n, the negative piece's is +n. Every cut edge is used once by
    // the surface and once, in the opposite direction, by the cap; those edges change from Cut to
    // CapSeam. A PatchCap is appended to the piece.
    //
    // piece is modified only on success. loops must come from ExtractCutLoops for this piece.
    // Failures return CapTriangulationFailed with the reason in detail.
    SliceStatus BuildCap(PatchPiece& piece, const SlicePlane& plane, bool positiveSide, uint32_t cutId,
        const std::vector<SliceLoop>& loops, float epsilonScale, float uvScale, std::string* detail = nullptr);
}
