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

        // 부호 거리 -> 부호 (슬라이서와 같은 규칙: epsilon 안은 0, 계산할 수 없는 값은 2)
        int SignOf(double distance, double epsilon)
        {
            if (!std::isfinite(distance))
                return 2;
            return distance > epsilon ? 1 : (distance < -epsilon ? -1 : 0);
        }

        // 부호 셋이 평면 양쪽에 걸쳤는가 (한쪽에만 있거나 정점이나 에지만 평면에 닿으면 아니다)
        bool IsMixed(const int s[3])
        {
            const bool hasPlus = s[0] == 1 || s[1] == 1 || s[2] == 1;
            const bool hasMinus = s[0] == -1 || s[1] == -1 || s[2] == -1;
            return hasPlus && hasMinus;
        }

        // 걸친 삼각형의 절단선(평면 위의 선분)이 사각형과 만나는가. 끝점은 평면 위의 정점과 부호가 반대인 에지의 교점이다.
        // toWorld가 있으면 positions는 그 로컬 공간이고, 끝점만 world로 옮겨서 사각형과 비교한다.
        bool CutLineHitsRectangle(const CutPlane::Frame& frame, const DirectX::XMFLOAT3* positions, const double* distance,
            const uint32_t index[3], const int s[3], const DirectX::XMFLOAT4X4* toWorld)
        {
            DirectX::XMFLOAT3 ends[3];
            int endCount = 0;
            for (int k = 0; k < 3; ++k)
                if (s[k] == 0)
                    ends[endCount++] = positions[index[k]];
            for (int k = 0; k < 3; ++k)
            {
                const int l = (k + 1) % 3;
                if (s[k] * s[l] != -1)
                    continue;
                const double a = distance[index[k]], b = distance[index[l]];
                const double ratio = a / (a - b);
                const DirectX::XMFLOAT3& p = positions[index[k]];
                const DirectX::XMFLOAT3& q = positions[index[l]];
                ends[endCount++] = { float(p.x + (double(q.x) - p.x) * ratio), float(p.y + (double(q.y) - p.y) * ratio),
                    float(p.z + (double(q.z) - p.z) * ratio) };
            }
            if (endCount < 2)
                return false;
            if (toWorld != nullptr)
                for (int e = 0; e < 2; ++e)
                    DirectX::XMStoreFloat3(&ends[e], DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&ends[e]), DirectX::XMLoadFloat4x4(toWorld)));
            return SegmentHitsRectangle(frame, ends[0], ends[1]);
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
            sign[i] = SignOf(distance[i], epsilon);     // 2 = 계산할 수 없는 정점: 그 정점을 쓰는 삼각형은 건너뛴다
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
            if (!IsMixed(s))
                continue;
            if (stats)
                ++stats->mixedTriangles;
            if (!CutLineHitsRectangle(frame, mesh.positions.data(), distance.data(), index, s, nullptr))
                continue;       // 절단선이 사각형과 만나야 한다

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

    bool CutPick::SphereMayCross(const DirectX::XMFLOAT4& localSphere, const DirectX::XMFLOAT4X4& localToWorld,
        const CutPlane& plane, float epsilon)
    {
        using namespace DirectX;
        const XMVECTOR center = XMVector3TransformCoord(XMVectorSet(localSphere.x, localSphere.y, localSphere.z, 0.0f), XMLoadFloat4x4(&localToWorld));
        // 반지름 배율은 3x3의 최대 특이값의 상한 sqrt(최대 행 합 x 최대 열 합)이다: 구를 키우기만 한다 (보수적, 단위 행렬이면 1).
        double rowSum = 0.0, columnSum = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            double row = 0.0, column = 0.0;
            for (int j = 0; j < 3; ++j)
            {
                row += std::fabs(double(localToWorld.m[i][j]));
                column += std::fabs(double(localToWorld.m[j][i]));
            }
            rowSum = row > rowSum ? row : rowSum;
            columnSum = column > columnSum ? column : columnSum;
        }
        const double radius = double(localSphere.w) * std::sqrt(rowSum * columnSum);
        if (!(radius > 0.0) || !std::isfinite(radius) || !std::isfinite(XMVectorGetX(XMVector3LengthSq(center))))
            return true;        // 구를 믿을 수 없으면 건너뛰지 않는다

        const CutPlane::Frame frame = plane.GetFrame();
        XMFLOAT3 c;
        XMStoreFloat3(&c, center);
        const V3 offset = ToV3(c) - ToV3(frame.center);
        const double reach = radius + epsilon;
        return std::fabs(Dot(offset, ToV3(frame.normal))) <= reach
            && std::fabs(Dot(offset, ToV3(frame.axisU))) <= frame.halfExtents.x + reach
            && std::fabs(Dot(offset, ToV3(frame.axisV))) <= frame.halfExtents.y + reach;
    }

    uint32_t CutPick::CountCrossedTriangles(const std::vector<DirectX::XMFLOAT3>& localPositions, const std::vector<uint32_t>& indices,
        const DirectX::XMFLOAT4X4& localToWorld, const CutPlane& plane, float epsilon, CutPickStats* stats)
    {
        if (stats)
            *stats = CutPickStats();
        const size_t vertexCount = localPositions.size();
        const SlicePlane slice = plane.ToSlicePlane();
        if (ValidatePlane(slice) != SliceStatus::Ok)
            return 0;

        // 평면을 로컬로 옮긴다: world 거리 = n . (p M + t - o) = (M n) . p + (t . n - o . n). 법선을 늘이지 않아서 거리는 world 길이 그대로다 (epsilon 규칙이 같다).
        const V3 normal = Normalize(ToV3(slice.normal));
        const V3 origin = ToV3(slice.point);
        const double nv[3] = { normal.x, normal.y, normal.z };
        double localNormal[3] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                localNormal[i] += double(localToWorld.m[i][j]) * nv[j];
        double offset = -Dot(normal, origin);
        for (int j = 0; j < 3; ++j)
            offset += double(localToWorld.m[3][j]) * nv[j];

        const CutPlane::Frame frame = plane.GetFrame();
        std::vector<double> distance(vertexCount);
        std::vector<int> sign(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            const DirectX::XMFLOAT3& p = localPositions[i];
            distance[i] = localNormal[0] * p.x + localNormal[1] * p.y + localNormal[2] * p.z + offset;
            sign[i] = SignOf(distance[i], epsilon);
        }

        uint32_t crossed = 0;
        for (size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            if (stats)
                ++stats->checkedTriangles;
            const uint32_t index[3] = { indices[t], indices[t + 1], indices[t + 2] };
            if (index[0] >= vertexCount || index[1] >= vertexCount || index[2] >= vertexCount)
                continue;
            const int s[3] = { sign[index[0]], sign[index[1]], sign[index[2]] };
            if (s[0] == 2 || s[1] == 2 || s[2] == 2 || !IsMixed(s))
                continue;
            if (stats)
                ++stats->mixedTriangles;
            if (!CutLineHitsRectangle(frame, localPositions.data(), distance.data(), index, s, &localToWorld))
                continue;
            ++crossed;
            if (stats)
                ++stats->crossedTriangles;
        }
        return crossed;
    }
}
