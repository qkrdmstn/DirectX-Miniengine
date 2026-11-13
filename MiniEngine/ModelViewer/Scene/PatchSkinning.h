#pragma once
#include "Model.h"
#include <vector>

namespace Viewer
{
    struct PatchSkinSnapshot
    {
        uint32_t AttachNode = UINT32_MAX;
        Math::Matrix4 AttachAtCut{ Math::kIdentity };
        Math::Matrix4 MeshNodeAtCut{ Math::kIdentity };
        std::vector<Joint> JointsAtCut;
        std::vector<uint8_t> Frozen;
        bool SkinningAllowed = true;
    };

    // 현재 팔레트의 고정 본을 부착 노드 상대 pose로 바꾸고 강체 슬롯 K를 덧붙인다.
    void BuildPatchPalette(const PatchSkinSnapshot& snapshot, const Joint* currentJoints,
        const Math::Matrix4* currentNodes, std::vector<Joint>& out);
}
