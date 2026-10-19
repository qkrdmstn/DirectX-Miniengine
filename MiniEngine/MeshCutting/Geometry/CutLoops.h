#pragma once

#include "PatchGeometry.h"

namespace MeshCutting
{
    struct CutLoop
    {
        std::vector<uint32_t> nodes;    // piece node ids, in the direction of the surface cut edges
        double signedArea = 0.0;        // in PlaneBasis (u, v); the outer loop of the positive piece is > 0
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
        std::vector<CutLoop>& loops, std::string* detail = nullptr);
}
