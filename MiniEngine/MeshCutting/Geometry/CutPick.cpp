// 이 파일은 평면과 교차하는 삼각형을 찾고, 삼각형마다 루트에 가장 가까운 joint를 절단 joint로 고른다 (CutPick).
#include "../pch.h"
#include "CutPick.h"

#include <cmath>
#include <algorithm>
#include <map>

namespace MeshCutting
{
    namespace
    {
        // 팔레트 계층에서 루트까지의 단계 수 (루트는 0). 번호가 범위 밖이거나 부모가 순환하면 -1이다.
        int JointDepth(const std::vector<uint32_t>& parents, uint32_t joint)
        {
            int depth = 0;
            uint32_t at = joint;
            while (true)
            {
                if (at >= parents.size() || depth > int(parents.size()))
                    return -1;
                if (parents[at] == kNoNode)
                    return depth;
                at = parents[at];
                ++depth;
            }
        }

        // 평면 위의 선분 ab가 사각형(frame의 u, v 범위)과 만나는가. 선분을 사각형의 네 변으로 잘라 본다 (Liang-Barsky).
        bool SegmentHitsRectangle(const CutPlane::Frame& frame, const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
        {
            const V3 center = ToV3(frame.center), axisU = ToV3(frame.axisU), axisV = ToV3(frame.axisV);
            const V3 pa = ToV3(a) - center, pb = ToV3(b) - center;
            const double start[2] = { Dot(pa, axisU), Dot(pa, axisV) };
            const double delta[2] = { Dot(pb, axisU) - start[0], Dot(pb, axisV) - start[1] };
            const double half[2] = { frame.halfExtents.x, frame.halfExtents.y };
            double enter = 0.0, leave = 1.0;
            for (int axis = 0; axis < 2; ++axis)
            {
                // 이 축에서 start + t * delta가 [-half, half] 안인 t의 범위
                const double lowEdge = -half[axis] - start[axis], highEdge = half[axis] - start[axis];
                if (delta[axis] == 0.0)
                {
                    if (lowEdge > 0.0 || highEdge < 0.0)
                        return false;
                    continue;
                }
                double t0 = lowEdge / delta[axis], t1 = highEdge / delta[axis];
                if (t0 > t1)
                    std::swap(t0, t1);
                enter = t0 > enter ? t0 : enter;
                leave = t1 < leave ? t1 : leave;
                if (enter > leave)
                    return false;
            }
            return true;
        }

        struct Candidate
        {
            uint32_t joint = kNoNode;
            float weightSum = 0.0f;     // 세 정점의 이 joint 가중치 합
            float perVertex[3] = {};    // 정점마다의 이 joint 가중치 합 (같은 joint가 여러 칸에 있으면 더한다)
            bool above = false;         // 어느 한 정점에서 minWeight를 넘는가
            int depth = 0;
        };

        // 삼각형 세 정점에서 후보 joint를 모아 절단 joint를 고른다. 후보가 없으면 kNoNode.
        uint32_t ChooseJoint(const CutPickMesh& mesh, const uint32_t index[3], const std::vector<uint32_t>& parents, float minWeight)
        {
            Candidate candidates[12];
            int count = 0;
            for (int k = 0; k < 3; ++k)
            {
                const SkinVertex& skin = mesh.skins[index[k]];
                for (uint32_t i = 0; i < skin.jointCount && i < 4; ++i)
                {
                    if (!(skin.weights[i] > 0.0f))
                        continue;
                    int found = -1;
                    for (int c = 0; c < count; ++c)
                        if (candidates[c].joint == skin.joints[i])
                            found = c;
                    if (found < 0)
                    {
                        found = count++;
                        candidates[found].joint = skin.joints[i];
                        candidates[found].depth = JointDepth(parents, skin.joints[i]);
                    }
                    candidates[found].weightSum += skin.weights[i];
                    candidates[found].perVertex[k] += skin.weights[i];
                }
            }

            for (int c = 0; c < count; ++c)
                candidates[c].above = candidates[c].perVertex[0] > minWeight || candidates[c].perVertex[1] > minWeight || candidates[c].perVertex[2] > minWeight;

            // 루트에 가장 가까운 joint, 같으면 가중치 합이 큰 joint, 그것도 같으면 번호가 작은 joint
            int best = -1;
            for (int c = 0; c < count; ++c)
            {
                if (!candidates[c].above || candidates[c].depth < 0)
                    continue;
                if (best < 0 || candidates[c].depth < candidates[best].depth
                    || (candidates[c].depth == candidates[best].depth
                        && (candidates[c].weightSum > candidates[best].weightSum
                            || (candidates[c].weightSum == candidates[best].weightSum && candidates[c].joint < candidates[best].joint))))
                    best = c;
            }
            return best < 0 ? kNoNode : candidates[best].joint;
        }
    }

    void CutPick::PickJoints(const CutPickMesh& mesh, const std::vector<uint32_t>& jointParents,
        const CutPlane& plane, float minWeight, float epsilon, std::vector<CutPickJoint>& out, CutPickStats* stats)
    {
        out.clear();
        if (stats)
            *stats = CutPickStats();
        const size_t vertexCount = mesh.positions.size();
        const SlicePlane slice = plane.ToSlicePlane();
        if (mesh.skins.size() != vertexCount || ValidatePlane(slice) != SliceStatus::Ok)
            return;

        // 정점마다 평면까지의 부호 거리와 부호 (슬라이서와 같은 규칙: epsilon 안은 0)
        const V3 normal = Normalize(ToV3(slice.normal));
        const V3 origin = ToV3(slice.point);
        const CutPlane::Frame frame = plane.GetFrame();
        std::vector<double> distance(vertexCount);
        std::vector<int> sign(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            distance[i] = Dot(ToV3(mesh.positions[i]) - origin, normal);
            if (!std::isfinite(distance[i]))
                sign[i] = 2;        // 계산할 수 없는 정점: 그 정점을 쓰는 삼각형은 건너뛴다
            else
                sign[i] = distance[i] > epsilon ? 1 : (distance[i] < -epsilon ? -1 : 0);
        }

        std::map<uint32_t, uint32_t> counts;
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
        {
            if (stats)
                ++stats->checkedTriangles;
            const uint32_t index[3] = { mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2] };
            if (index[0] >= vertexCount || index[1] >= vertexCount || index[2] >= vertexCount)
                continue;
            const int s[3] = { sign[index[0]], sign[index[1]], sign[index[2]] };
            if (s[0] == 2 || s[1] == 2 || s[2] == 2)
                continue;
            const bool hasPlus = s[0] == 1 || s[1] == 1 || s[2] == 1;
            const bool hasMinus = s[0] == -1 || s[1] == -1 || s[2] == -1;
            if (!hasPlus || !hasMinus)
                continue;       // 한쪽에만 있거나 정점이나 에지만 평면에 닿는 삼각형
            if (stats)
                ++stats->mixedTriangles;

            // 절단선은 평면 위의 선분이다: 끝점은 평면 위의 정점과 부호가 반대인 에지의 교점이다. 이 선분이 사각형과 만나야 한다.
            DirectX::XMFLOAT3 ends[3];
            int endCount = 0;
            for (int k = 0; k < 3; ++k)
                if (s[k] == 0)
                    ends[endCount++] = mesh.positions[index[k]];
            for (int k = 0; k < 3; ++k)
            {
                const int l = (k + 1) % 3;
                if (s[k] * s[l] != -1)
                    continue;
                const double a = distance[index[k]], b = distance[index[l]];
                const double ratio = a / (a - b);
                const DirectX::XMFLOAT3& p = mesh.positions[index[k]];
                const DirectX::XMFLOAT3& q = mesh.positions[index[l]];
                ends[endCount++] = { float(p.x + (double(q.x) - p.x) * ratio), float(p.y + (double(q.y) - p.y) * ratio),
                    float(p.z + (double(q.z) - p.z) * ratio) };
            }
            if (endCount < 2 || !SegmentHitsRectangle(frame, ends[0], ends[1]))
                continue;

            const uint32_t joint = ChooseJoint(mesh, index, jointParents, minWeight);
            if (joint != kNoNode)
            {
                ++counts[joint];
                if (stats)
                    ++stats->crossedTriangles;
            }
        }

        for (const auto& entry : counts)
        {
            CutPickJoint picked;
            picked.joint = entry.first;
            picked.triangleCount = entry.second;
            out.push_back(picked);
        }
    }
}
