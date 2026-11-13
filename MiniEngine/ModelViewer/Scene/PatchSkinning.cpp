#include "PatchSkinning.h"

namespace Viewer
{
    // 절단 시점의 고정 본을 현재 부착 프레임으로 옮기며 normal 행렬도 다시 계산한다.
    void BuildPatchPalette(const PatchSkinSnapshot& snapshot, const Joint* currentJoints,
        const Math::Matrix4* currentNodes, std::vector<Joint>& out)
    {
        // 전체 owner 팔레트를 유지하고 부착 노드의 모델 공간 변화량을 구한다.
        const size_t count = snapshot.JointsAtCut.size();
        out.assign(currentJoints, currentJoints + count);
        const Math::Matrix4 delta = currentNodes[snapshot.AttachNode] * Math::Invert(snapshot.AttachAtCut);

        // 고정 본만 절단 pose로 치환하고 최종 위치 행렬의 역전치로 법선을 만든다.
        for (size_t k = 0; k < count; ++k)
        {
            if (!snapshot.Frozen[k]) continue;
            out[k].posXform = delta * snapshot.JointsAtCut[k].posXform;
            out[k].nrmXform = Math::InverseTranspose(out[k].posXform.Get3x3());
        }

        // 새 경계와 cap 정점이 사용할 단일 강체 슬롯을 마지막에 추가한다.
        Joint rigid;
        rigid.posXform = delta;
        rigid.nrmXform = Math::InverseTranspose(delta.Get3x3());
        out.push_back(rigid);
    }
}
