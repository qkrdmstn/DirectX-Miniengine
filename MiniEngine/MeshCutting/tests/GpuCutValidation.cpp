#include "../GPU/GpuMeshCutter.h"
#include "Utility.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace MeshCutting
{
    namespace
    {
        // 테스트 실패를 Release에서도 숨기지 않는다.
        void Require(bool condition, const char* message)
        {
            if (!condition) throw std::runtime_error(message);
        }
        // 정렬/alias 가정 없이 GPU buffer의 작은 값을 읽는다.
        template<class T> T Read(const uint8_t* data, size_t offset)
        {
            T value; std::memcpy(&value, data + offset, sizeof(value)); return value;
        }
        // 월드 직교 단면 좌표로 삼각형 면적을 비교한다.
        double Cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }
        // 좌표 연산을 double로 수행해 반복 절단의 부모/자식 면적 비교 오차를 줄인다.
        double TriangleArea(const Math::XMFLOAT3& a, const Math::XMFLOAT3& b, const Math::XMFLOAT3& c)
        {
            const double x = Cross(double(b.y)-a.y,double(b.z)-a.z,double(c.y)-a.y,double(c.z)-a.z);
            const double y = Cross(double(b.z)-a.z,double(b.x)-a.x,double(c.z)-a.z,double(c.x)-a.x);
            const double z = Cross(double(b.x)-a.x,double(b.y)-a.y,double(c.x)-a.x,double(c.y)-a.y);
            return std::sqrt(x*x+y*y+z*z)*0.5;
        }
        // 열린 메시에서도 물리적 부피가 아닌 방향 있는 삼각형 적분의 보존을 검사한다.
        double SignedVolume(const Math::XMFLOAT3& a, const Math::XMFLOAT3& b, const Math::XMFLOAT3& c)
        {
            return (a.x*Cross(b.y,b.z,c.y,c.z)+a.y*Cross(b.z,b.x,c.z,c.x)+a.z*Cross(b.x,b.y,c.x,c.y))/6;
        }
        // GPU와 같은 startIndex/baseVertex와 16/32비트 형식으로 부모 인덱스를 해석한다.
        uint32_t ParentIndex(const uint8_t* data, const CutSection& section, uint32_t i)
        {
            const size_t at = size_t(section.StartIndex)+i;
            return section.BaseVertex + (section.Index32 ? Read<uint32_t>(data,section.IbOffset+at*4) :
                Read<uint16_t>(data,section.IbOffset+at*2));
        }
    }

    // 명시적 진단에서만 전체 결과를 읽는다. 일반 절단/벤치마크는 작은 metadata만 읽는다.
    void CutJob::ValidateDiagnostics()
    {
        Require(Collected && Status[0] == 0, "GPU cut status failed");
        if (!HasSplit())
        {
            for (uint32_t side = 0; side < 2; ++side)
                Require(Status[20 + side * Constants.Sections + Constants.Sections - 1] == 0,
                    "Touch/no-split generated cap triangles");
            Utility::Printf("GPU cut no-split validation passed: surfaceSides=%u\n",Status[5]&3);
            return;
        }
        ReadbackBuffer boundary;
        boundary.Create(L"Diagnostic boundary", std::max(Status[1], 1u), 128);
        auto& context = GraphicsContext::Begin(L"Validate generated GPU geometry");
        context.CopyBufferRegion(boundary, 0, Scratch, 0, uint64_t(std::max(Status[1], 1u)) * 128);
        context.TransitionResource(Scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.Finish(true);
        const auto* ring = static_cast<const uint8_t*>(boundary.Map());
        std::map<uint32_t, double> loopAreas;
        for (uint32_t i = 0; i < Status[1]; ++i)
        {
            if (Read<uint32_t>(ring, i * 128 + 108) == 0) continue;
            const uint32_t leader = Read<uint32_t>(ring, i * 128 + 48);
            if ((Read<uint32_t>(ring, i * 128 + 52) & 5) || !Read<uint32_t>(ring, leader * 128 + 84)) continue;
            const auto a = Read<Math::XMFLOAT2>(ring, i * 128 + 24), b = Read<Math::XMFLOAT2>(ring, i * 128 + 32);
            loopAreas[leader] += Cross(a.x, a.y, b.x, b.y) * 0.5;
        }
        boundary.Unmap();
        double expectedArea = 0;
        for (const auto& loop : loopAreas) expectedArea += std::abs(loop.second);
        Require(HasDiagnosticInput,"Parent snapshot is required for cut diagnostics");
        const auto* parentPose = static_cast<const uint8_t*>(DiagnosticInputPose.Map());
        const auto* parentData = static_cast<const uint8_t*>(DiagnosticInputData.Map());
        std::vector<std::array<uint32_t,3>> parentIndices(Constants.Triangles);
        std::vector<double> parentAreas(Constants.Triangles), outputAreas(Constants.Triangles,0.0);
        double parentVolume = 0, absoluteVolume = 0, parentArea = 0, resultVolume = 0;
        for (uint32_t section = 0; section + 1 < Constants.Sections; ++section)
        {
            const auto& desc = Sections[section];
            for (uint32_t tri = 0; tri < desc.Triangles; ++tri)
            {
                auto& indices = parentIndices[desc.FirstTriangle+tri];
                Math::XMFLOAT3 p[3];
                for (uint32_t corner = 0; corner < 3; ++corner)
                {
                    indices[corner] = ParentIndex(parentData,desc,tri*3+corner);
                    Require(indices[corner] < desc.VertexCount,"Invalid parent triangle index");
                    p[corner] = Read<Math::XMFLOAT3>(parentPose,desc.PoseOffset+indices[corner]*40);
                }
                const double volume = SignedVolume(p[0],p[1],p[2]);
                parentVolume += volume; absoluteVolume += std::abs(volume);
                parentAreas[desc.FirstTriangle+tri] = TriangleArea(p[0],p[1],p[2]);
                parentArea += parentAreas[desc.FirstTriangle+tri];
            }
        }
        for (uint32_t side = 0; side < 2; ++side)
        {
            uint64_t surfaceCorners = 0;
            for (uint32_t section = 0; section + 1 < Constants.Sections; ++section)
                surfaceCorners += Status[20 + side * Constants.Sections + section];
            Require(surfaceCorners != 0,"Split result contains only cap triangles");
            auto& out = *Output[side];
            ReadbackBuffer data, pose, origin;
            data.Create(L"Diagnostic packed result", uint32_t(out.Data.GetBufferSize()), 1);
            pose.Create(L"Diagnostic pose result", uint32_t(out.Pose.GetBufferSize()), 1);
            origin.Create(L"Diagnostic provenance", uint32_t(out.Provenance.GetBufferSize()), 1);
            auto& read = GraphicsContext::Begin(L"Read explicit cut diagnostics");
            read.CopyBuffer(data, out.Data); read.CopyBuffer(pose, out.Pose); read.CopyBuffer(origin, out.Provenance);
            read.TransitionResource(out.Data, D3D12_RESOURCE_STATE_GENERIC_READ);
            read.TransitionResource(out.Pose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            read.TransitionResource(out.Provenance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            read.Finish(true);
            const auto* packed = static_cast<const uint8_t*>(data.Map());
            const auto* vertices = static_cast<const uint8_t*>(pose.Map());
            const auto* provenance = static_cast<const uint8_t*>(origin.Map());
            double capArea = 0, volume = 0; uint32_t triangles = 0;

            // 계승 hull의 요소별 지지 평면과 허용 오차를 준비한다. tol = 1e-4 x hull 최대 변 길이이며,
            // 18-DOP은 구성상 렌더 메시를 포함하고 cap 정점은 절단 평면 위에 있으므로 float 오차만 흡수하면 된다.
            std::vector<std::vector<MiniCollision::Plane>> hullPlanes;
            double hullTolerance = 0, hullExtent = 0, hullMaxExcess = -1.0e30;
            for (const MiniCollision::ConvexElem& elem : ContainmentHulls[side].ConvexElems)
            {
                hullPlanes.emplace_back();
                elem.GetPlanes(hullPlanes.back());
                hullExtent = std::max({hullExtent, elem.ElemBox.Max.x - elem.ElemBox.Min.x,
                    elem.ElemBox.Max.y - elem.ElemBox.Min.y, elem.ElemBox.Max.z - elem.ElemBox.Min.z});
            }
            hullTolerance = hullExtent * 1.0e-4;
            uint32_t hullChecked = 0;
            // 이 쪽 정점의 절단 평면까지 최대 거리(로컬 단위)다. 평면을 정규화하려고 법선 길이로 나눈다.
            double thickness = 0;
            const double planeLength = std::sqrt(double(float(Constants.Plane.GetX()))*float(Constants.Plane.GetX()) +
                double(float(Constants.Plane.GetY()))*float(Constants.Plane.GetY()) + double(float(Constants.Plane.GetZ()))*float(Constants.Plane.GetZ()));
            for (uint32_t section = 0; section < Constants.Sections; ++section)
            {
                const CutSection& desc = Sections[section];
                const uint32_t count = Status[20 + side * Constants.Sections + section];
                Require(count <= desc.VertexCapacity && count % 3 == 0, "GPU output capacity/count");
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t global = desc.VertexStart + i;
                    const auto p = Read<Math::XMFLOAT3>(vertices, global * 40);
                    const auto n = Read<Math::XMFLOAT3>(vertices, global * 40 + 12);
                    Require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Nonfinite result position");
                    Require(std::isfinite(n.x) && std::abs(n.x*n.x+n.y*n.y+n.z*n.z-1) < 0.01f, "Invalid result normal");
                    Require(std::memcmp(packed + global * 28, vertices + global * 40, 12) == 0, "Packed/posed position differs");
                    Require(Read<uint32_t>(packed, Constants.IndexOffset + global * 4) == i, "Invalid generated index");
                    const auto parent = Read<uint32_t>(provenance, global * 32);
                    Require(parent == 0xffffffff || parent < Input->GetSections().size(), "Invalid provenance section");
                    Require((parent == 0xffffffff) == (section + 1 == Constants.Sections),"Unexpected surface/cap provenance");
                    if (parent != 0xffffffff)
                    {
                        const Mesh& source = Input->GetMesh(parent);
                        const uint32_t draw = Read<uint32_t>(provenance, global * 32 + 4);
                        Require(draw < source.numDraws, "Invalid parent draw");
                        Require(Read<uint32_t>(provenance, global * 32 + 8) < source.vbSize/source.vbStride
                            && Read<uint32_t>(provenance, global * 32 + 12) < source.vbSize/source.vbStride, "Invalid parent vertex");
                        const float t = Read<float>(provenance, global * 32 + 16);
                        Require(std::isfinite(t) && t >= 0 && t <= 1, "Invalid source blend parameter");
                        const uint32_t triangle = Read<uint32_t>(provenance,global*32+20);
                        Require(parent == desc.SourceSection && draw == desc.SourceDraw && triangle < desc.Triangles,
                            "Surface does not belong to its parent draw/triangle");
                        const uint32_t ia = Read<uint32_t>(provenance,global*32+8), ib = Read<uint32_t>(provenance,global*32+12);
                        const auto& indices = parentIndices[desc.FirstTriangle+triangle];
                        Require(std::find(indices.begin(),indices.end(),ia) != indices.end() &&
                            std::find(indices.begin(),indices.end(),ib) != indices.end(),"Vertex endpoints are outside parent triangle");
                        const auto a = Read<Math::XMFLOAT3>(parentPose,desc.PoseOffset+ia*40);
                        const auto b = Read<Math::XMFLOAT3>(parentPose,desc.PoseOffset+ib*40);
                        const double tolerance = 2.0e-6*std::max({1.0,std::abs(double(a.x)),std::abs(double(a.y)),std::abs(double(a.z)),
                            std::abs(double(b.x)),std::abs(double(b.y)),std::abs(double(b.z))});
                        Require(std::abs(p.x-(double(a.x)+(double(b.x)-a.x)*t)) <= tolerance &&
                            std::abs(p.y-(double(a.y)+(double(b.y)-a.y)*t)) <= tolerance &&
                            std::abs(p.z-(double(a.z)+(double(b.z)-a.z)*t)) <= tolerance,"Output position differs from parent interpolation");
                    }
                    const float distance = Math::Dot(Constants.Plane, Math::Vector4(Math::Vector3(p), 1));
                    Require(side == 0 ? distance >= -Constants.Epsilon*4 : distance <= Constants.Epsilon*4, "Wrong side of cut plane");
                    thickness = std::max(thickness, std::abs(double(distance)) / planeLength);

                    // 정점이 계승 hull 요소 중 하나의 모든 지지 평면 안쪽(PlaneDot <= tol)에 있어야 한다.
                    if (!hullPlanes.empty())
                    {
                        double bestExcess = 1.0e30;
                        for (const auto& planes : hullPlanes)
                        {
                            double excess = -1.0e30;
                            for (const MiniCollision::Plane& plane : planes)
                                excess = std::max(excess, plane.PlaneDot({ p.x, p.y, p.z }));
                            bestExcess = std::min(bestExcess, excess);
                        }
                        hullMaxExcess = std::max(hullMaxExcess, bestExcess);
                        ++hullChecked;
                    }
                }
                for (uint32_t tri = 0; tri < count; tri += 3)
                {
                    const auto p0 = Read<Math::XMFLOAT3>(vertices, (desc.VertexStart + tri) * 40);
                    const auto p1 = Read<Math::XMFLOAT3>(vertices, (desc.VertexStart + tri + 1) * 40);
                    const auto p2 = Read<Math::XMFLOAT3>(vertices, (desc.VertexStart + tri + 2) * 40);
                    const Math::Vector3 a(p0), b(p1), c(p2);
                    volume += SignedVolume(p0,p1,p2);
                    if (section + 1 < Constants.Sections)
                    {
                        const uint32_t triangle = Read<uint32_t>(provenance,(desc.VertexStart+tri)*32+20);
                        for (uint32_t corner = 1; corner < 3; ++corner)
                            Require(Read<uint32_t>(provenance,(desc.VertexStart+tri+corner)*32+20) == triangle,
                                "Output triangle mixes different parent triangles");
                        outputAreas[desc.FirstTriangle+triangle] += TriangleArea(p0,p1,p2);
                    }
                    if (section + 1 == Constants.Sections)
                    {
                        const double ax = Math::Dot(Constants.ProjectU, Math::Vector4(a, 1)), ay = Math::Dot(Constants.ProjectV, Math::Vector4(a, 1));
                        const double bx = Math::Dot(Constants.ProjectU, Math::Vector4(b, 1)), by = Math::Dot(Constants.ProjectV, Math::Vector4(b, 1));
                        const double cx = Math::Dot(Constants.ProjectU, Math::Vector4(c, 1)), cy = Math::Dot(Constants.ProjectV, Math::Vector4(c, 1));
                        capArea += std::abs(Cross(bx-ax, by-ay, cx-ax, cy-ay)) * 0.5;
                        const float winding = Math::Dot(Math::Cross(b-a,c-a),Math::Vector3(Constants.Plane));
                        Require(side == 0 ? winding < 0 : winding > 0, "Cap winding mismatch");
                    }
                    ++triangles;
                }
            }
            data.Unmap(); pose.Unmap(); origin.Unmap();
            Require(std::abs(capArea-expectedArea) <= std::max(1.0e-8, expectedArea*0.001), "Cap area does not match closed boundary");
            Utility::Printf("GPU cut validation passed: side=%u triangles=%u capArea=%.9f expected=%.9f signedVolume=%.9f\n",
                side, triangles, capArea, expectedArea, volume);
            resultVolume += volume;
            if (hullChecked)
            {
                // 두께가 병합 거리(0.001)의 2배 이하인 얇은 조각은 hull 정점 병합·snap이 형상을 최대 그 정도 옮기므로 허용 오차를 넓힌다.
                if (thickness <= 2.0 * MiniCollision::MinHullVertDistance)
                    hullTolerance = std::max(hullTolerance, 2.0 * MiniCollision::MinHullVertDistance);
                Require(hullMaxExcess <= hullTolerance, "Result vertex lies outside inherited collision hull");
                Utility::Printf("GPU cut hull containment passed: side=%u hulls=%zu vertices=%u maxExcess=%.9g tolerance=%.9g\n",
                    side, hullPlanes.size(), hullChecked, hullMaxExcess, hullTolerance);
            }
            else if (InheritanceActive)
            {
                // 계승이 실행됐는데 hull이 빈 쪽은 병합 거리(0.001)보다 얇아 슬라이스가 사라진 경우에만 허용한다(2배 여유).
                Require(thickness <= 2.0 * MiniCollision::MinHullVertDistance, "Result side has geometry but inherited hull is empty");
                Utility::Printf("GPU cut hull containment: side=%u thin piece (thickness=%.9g) has no convex - body-less by policy (R3)\n", side, thickness);
            }
            else
                Utility::Printf("GPU cut hull containment skipped: side=%u (parent has no collision)\n", side);
        }
        DiagnosticInputPose.Unmap(); DiagnosticInputData.Unmap();
        double maximumAreaError = 0;
        for (uint32_t triangle = 0; triangle < Constants.Triangles; ++triangle)
        {
            const double error = std::abs(outputAreas[triangle]-parentAreas[triangle]);
            maximumAreaError = std::max(maximumAreaError,error);
            Require(error <= std::max(1.0e-10,parentAreas[triangle]*0.001+parentArea*1.0e-10),
                "Parent triangle area was lost or duplicated during split");
        }
        Require(std::abs(resultVolume-parentVolume) <= std::max(1.0e-8,absoluteVolume*1.0e-5),
            "Parent/output signed volume integral is not conserved");
        Utility::Printf("GPU cut parent conservation passed: triangles=%u maxAreaError=%.9g parentVolume=%.9f resultVolume=%.9f\n",
            Constants.Triangles,maximumAreaError,parentVolume,resultVolume);
    }
}
