#pragma once

#include "PatchGeometry.h"

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
}
