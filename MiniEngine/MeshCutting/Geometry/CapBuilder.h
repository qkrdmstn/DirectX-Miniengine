#pragma once

#include "CutLoops.h"

namespace MeshCutting
{
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
    // CapSeam. A CapRecord is appended to the piece.
    //
    // piece is modified only on success. loops must come from ExtractCutLoops for this piece.
    // Failures return CapTriangulationFailed with the reason in detail.
    SliceStatus BuildCap(PatchPiece& piece, const SlicePlane& plane, bool positiveSide, uint32_t cutId,
        const std::vector<CutLoop>& loops, float epsilonScale, float uvScale, std::string* detail = nullptr);
}
