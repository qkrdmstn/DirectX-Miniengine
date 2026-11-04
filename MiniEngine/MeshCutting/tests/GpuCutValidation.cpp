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
        // 진단 readback 매핑을 정상 반환과 예외 전파 모두에서 해제한다.
        struct ScopedReadback
        {
            ReadbackBuffer& Buffer;
            const uint8_t* Data;

            // 버퍼를 매핑하고 수명을 현재 검사 범위에 묶는다.
            explicit ScopedReadback(ReadbackBuffer& buffer) : Buffer(buffer)
            {
                // 성공한 매핑만 소멸자에서 해제한다.
                Data = static_cast<const uint8_t*>(Buffer.Map());
            }

            // 검사 도중 예외가 발생해도 매핑을 해제한다.
            ~ScopedReadback()
            {
                // 소유한 매핑을 한 번 해제한다.
                Buffer.Unmap();
            }

            ScopedReadback(const ScopedReadback&) = delete;
            ScopedReadback& operator=(const ScopedReadback&) = delete;
        };

        // 유한성과 오차를 검사하고 실패 좌표 및 허용치를 출력한다.
        bool CheckValue(const char* kind, uint32_t section, uint32_t vertex, uint32_t axis,
            double actual, double expected, double tolerance)
        {
            // NaN도 실패로 분류하고 첫 예외 전에 구체적인 값을 남긴다.
            const bool valid = std::isfinite(actual) && std::isfinite(expected)
                && std::abs(actual - expected) <= tolerance;
            if (!valid)
                Utility::Printf("GPU cut diagnostic FAIL: kind=%s section=%u vertex=%u axis=%u actual=%.9g expected=%.9g tolerance=%.9g\n",
                    kind, section, vertex, axis, actual, expected, tolerance);
            return valid;
        }

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

        // GPU ReadVertex와 같은 행 우선 3x4의 mul(S, float4(p, 1))로 입력 위치를 복원한다.
        Math::XMFLOAT3 ParentPosition(const uint8_t* data, const CutSection& desc, uint32_t section, uint32_t vertex)
        {
            // Data의 float3 위치를 읽고 GPU와 같은 float 연산으로 세 행을 적용한다.
            const auto p = Read<Math::XMFLOAT3>(data, size_t(desc.VbOffset) + size_t(vertex) * desc.Stride);
            float result[3];
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                const float* row = desc.SectionToComponent + axis * 4;
                result[axis] = row[0] * p.x + row[1] * p.y + row[2] * p.z + row[3];
                Require(CheckValue("parent position finite", section, vertex, axis, result[axis], result[axis], 0),
                    "Nonfinite parent position");
            }
            return Math::XMFLOAT3(result[0], result[1], result[2]);
        }

        // 행렬의 유한성·역전치와 생성 revision의 단위 변환 계약을 검사한다.
        void ValidateSectionFrames(const CutJob& job)
        {
            // 모든 입력 draw와 cap의 행렬을 검사하고 기존 로그 형식을 유지한다.
            bool valid = true;
            double maxError = 0;
            uint64_t vertices = 0;
            for (uint32_t section = 0; section < job.Constants.Sections; ++section)
            {
                const auto& desc = job.Sections[section];
                const float* s = desc.SectionToComponent;
                const float* n = desc.NormalToComponent;
                vertices += desc.VertexCount;

                // 3x3의 여인수 행렬을 double로 계산해 역전치 기준을 독립적으로 만든다.
                double cofactor[3][3];
                for (uint32_t row = 0; row < 3; ++row)
                    for (uint32_t col = 0; col < 3; ++col)
                        cofactor[row][col] = double(s[((row + 1) % 3) * 4 + (col + 1) % 3]) * s[((row + 2) % 3) * 4 + (col + 2) % 3]
                            - double(s[((row + 1) % 3) * 4 + (col + 2) % 3]) * s[((row + 2) % 3) * 4 + (col + 1) % 3];
                const double determinant = s[0] * cofactor[0][0] + s[1] * cofactor[0][1] + s[2] * cofactor[0][2];

                // axis는 행 우선 원소 번호이며 행렬 검사에는 정점 미해당 표식을 쓴다.
                for (uint32_t axis = 0; axis < 12; ++axis)
                {
                    valid &= CheckValue("section finite", section, 0xffffffffu, axis, s[axis], s[axis], 0);
                    const double expected = axis % 4 == 3 ? 0 : cofactor[axis / 4][axis % 4] / determinant;
                    const double tolerance = 1.0e-5 * std::max(1.0, std::abs(expected));
                    valid &= CheckValue("normal inverse-transpose", section, 0xffffffffu, axis, n[axis], expected, tolerance);
                    maxError = std::max(maxError, std::abs(double(n[axis]) - expected));
                    if (job.Input->IsGenerated() || section + 1 == job.Constants.Sections)
                    {
                        const double identity = axis / 4 == axis % 4 ? 1 : 0;
                        valid &= CheckValue("generated position identity", section, 0xffffffffu, axis, s[axis], identity, 1.0e-5);
                        valid &= CheckValue("generated normal identity", section, 0xffffffffu, axis, n[axis], identity, 1.0e-5);
                        maxError = std::max({maxError, std::abs(double(s[axis]) - identity), std::abs(double(n[axis]) - identity)});
                    }
                }
            }

            // 실패 정보 뒤에도 집계 로그를 남긴 다음 예외를 전파한다.
            Utility::Printf("GPU cut section frames %s: generated=%u vertices=%llu maxError=%.9g tolerance=1e-5(relative)\n",
                valid ? "PASS" : "FAIL", job.Input->IsGenerated() ? 1u : 0u,
                static_cast<unsigned long long>(vertices), maxError);
            Require(valid, "Invalid cut section matrices");
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

        // 정수의 바이트를 고정 순서로 결합해 위치 비트를 그대로 해시한다.
        void HashWord(uint64_t& hash, uint32_t word)
        {
            // FNV-1a에 하위 바이트부터 넣어 부동소수점 연산을 피한다.
            for (uint32_t byte = 0; byte < 4; ++byte)
            {
                hash ^= (word >> (byte * 8)) & 0xffu;
                hash *= 1099511628211ull;
            }
        }

        // winding을 보존하는 회전만 정규화하고 삼각형 순서와 무관한 cap checksum을 출력한다.
        void PrintCapChecksum(const uint8_t* vertices, const CutSection& section, uint32_t count, uint32_t side)
        {
            // 세 위치의 원시 비트를 읽고 가장 작은 순환 회전을 고른다.
            std::vector<uint64_t> hashes;
            for (uint32_t tri = 0; tri < count; tri += 3)
            {
                std::array<std::array<uint32_t, 3>, 3> points;
                for (uint32_t corner = 0; corner < 3; ++corner)
                    points[corner] = Read<std::array<uint32_t, 3>>(vertices, size_t(section.VertexStart + tri + corner) * 40);
                uint32_t first = 0;
                for (uint32_t rotation = 1; rotation < 3; ++rotation)
                {
                    for (uint32_t corner = 0; corner < 3; ++corner)
                    {
                        const auto& candidate = points[(rotation + corner) % 3];
                        const auto& current = points[(first + corner) % 3];
                        if (candidate == current) continue;
                        if (candidate < current) first = rotation;
                        break;
                    }
                }

                // 정규화한 삼각형을 해시하되 반대 winding은 합치지 않는다.
                uint64_t hash = 14695981039346656037ull;
                for (uint32_t corner = 0; corner < 3; ++corner)
                    for (uint32_t word : points[(first + corner) % 3]) HashWord(hash, word);
                hashes.push_back(hash);
            }

            // 정렬 후 결합해 출력 예약 순서를 무시하고 중복 개수는 보존한다.
            std::sort(hashes.begin(), hashes.end());
            uint64_t checksum = 14695981039346656037ull;
            for (uint64_t hash : hashes)
            {
                HashWord(checksum, uint32_t(hash));
                HashWord(checksum, uint32_t(hash >> 32));
            }
            Utility::Printf("Static cut cap checksum: side=%u triangles=%zu hash=0x%016llx\n",
                side, hashes.size(), static_cast<unsigned long long>(checksum));
        }

        // 각 면 출력 정점을 읽어 CPU에서 double로 18-DOP을 구하고 GPU 값과 절대+상대 허용오차로 비교한다.
        void ValidateSideDop(CutJob& job)
        {
            for (uint32_t side = 0; side < 2; ++side)
            {
                // CutBoundsCS와 같은 section별 유효 범위(min(count, capacity))의 정점 수를 센다.
                uint64_t total = 0;
                for (uint32_t section = 0; section < job.Constants.Sections; ++section)
                {
                    const uint32_t count = job.Status[20 + side * job.Constants.Sections + section];
                    total += std::min(count, job.Sections[section].VertexCapacity);
                }

                // 출력 정점이 없는 면은 GPU도 DOP을 비워야 한다.
                if (total == 0)
                {
                    Require(!job.SideDopValid[side], "GPU DOP exists for a side without output");
                    Utility::Printf("GPU cut DOP check: side=%u empty (no output)\n", side);
                    continue;
                }

                // 출력 Pose 버퍼를 읽어 오고 원래 상태로 되돌린다.
                auto& out = *job.Output[side];
                ReadbackBuffer pose;
                pose.Create(L"Diagnostic DOP pose", uint32_t(out.Pose.GetBufferSize()), 1);
                auto& read = GraphicsContext::Begin(L"Read cut DOP pose");
                read.CopyBuffer(pose, out.Pose);
                read.TransitionResource(out.Pose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                read.Finish(true);
                const ScopedReadback verticesMapping(pose);
                const auto* vertices = verticesMapping.Data;

                // CPU에서 9개 축의 최솟값/최댓값과 기여 정점 수를 double로 구한다.
                double cpuMin[9];
                double cpuMax[9];
                for (uint32_t axis = 0; axis < 9; ++axis)
                {
                    cpuMin[axis] = 1.0e300;
                    cpuMax[axis] = -1.0e300;
                }
                uint64_t contributing = 0;
                for (uint32_t section = 0; section < job.Constants.Sections; ++section)
                {
                    const uint32_t count = std::min(job.Status[20 + side * job.Constants.Sections + section], job.Sections[section].VertexCapacity);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const auto p = Read<Math::XMFLOAT3>(vertices, (job.Sections[section].VertexStart + i) * 40);
                        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
                        ++contributing;
                        for (uint32_t axis = 0; axis < 9; ++axis)
                        {
                            const double d = double(p.x) * KDopAxes9[axis][0] + double(p.y) * KDopAxes9[axis][1] + double(p.z) * KDopAxes9[axis][2];
                            cpuMin[axis] = std::min(cpuMin[axis], d);
                            cpuMax[axis] = std::max(cpuMax[axis], d);
                        }
                    }
                }

                // 유한한 출력 정점이 하나도 없으면 GPU도 DOP을 비워야 하며 극값은 비교하지 않는다.
                if (contributing == 0)
                {
                    Require(!job.SideDopValid[side], "GPU DOP exists but CPU has no finite vertices");
                    Utility::Printf("GPU cut DOP check: side=%u no finite vertices (GPU DOP empty)\n", side);
                    continue;
                }

                // GPU 값이 있어야 하며 개수가 CPU와 같아야 한다.
                Require(job.SideDopValid[side], "GPU DOP is missing for a side with output");
                const KDop18& gpu = job.SideDop[side];
                Require(gpu.VertexCount == contributing, "GPU DOP vertex count differs from CPU");

                // 허용오차 1e-6 x extent + 1e-7로 모든 축의 최솟값/최댓값을 비교한다.
                double extent = 0;
                for (uint32_t axis = 0; axis < 9; ++axis) extent = std::max(extent, cpuMax[axis] - cpuMin[axis]);
                const double tolerance = 1.0e-6 * extent + 1.0e-7;
                double maxError = 0;
                for (uint32_t axis = 0; axis < 9; ++axis)
                {
                    maxError = std::max({maxError, std::abs(double(gpu.Min[axis]) - cpuMin[axis]), std::abs(double(gpu.Max[axis]) - cpuMax[axis])});
                }
                Require(maxError <= tolerance, "GPU DOP differs from CPU reference");
                Utility::Printf("GPU cut DOP check passed: side=%u vertices=%u maxAbsError=%.3g tolerance=%.3g extent=%.6g axis0=[%.6f,%.6f] axis1=[%.6f,%.6f] axis2=[%.6f,%.6f]\n",
                    side, gpu.VertexCount, maxError, tolerance, extent,
                    gpu.Min[0], gpu.Max[0], gpu.Min[1], gpu.Max[1], gpu.Min[2], gpu.Max[2]);
            }
        }
    }

    // 입력 행렬 계약과 Data 기반 부모 재구성 및 출력·충돌의 포함을 검사한다.
    void CutJob::ValidateDiagnostics()
    {
        // GPU 완료 상태를 확인한다.
        Require(Collected && Status[0] == 0, "GPU cut status failed");

        // PG 없이 section 행렬의 유한성·역전치·생성 단위 행렬을 검사한다.
        Require(HasDiagnosticInput, "Parent snapshot is required for cut diagnostics");
        ValidateSectionFrames(*this);

        // 분할 여부와 무관하게 출력이 있는 모든 면의 GPU 18-DOP을 CPU 기준과 비교한다.
        ValidateSideDop(*this);

        // 접촉·비교차는 cap이 없음을 확인한 뒤 종료한다.
        if (!HasSplit())
        {
            for (uint32_t side = 0; side < 2; ++side)
                Require(Status[20 + side * Constants.Sections + Constants.Sections - 1] == 0,
                    "Touch/no-split generated cap triangles");
            Utility::Printf("GPU cut no-split validation passed: surfaceSides=%u\n",Status[5]&3);
            return;
        }

        // GPU 경계 고리에서 닫힌 단면의 기준 면적을 구한다.
        ReadbackBuffer boundary;
        boundary.Create(L"Diagnostic boundary", std::max(Status[1], 1u), 128);
        auto& context = GraphicsContext::Begin(L"Validate generated GPU geometry");
        context.CopyBufferRegion(boundary, 0, Scratch, 0, uint64_t(std::max(Status[1], 1u)) * 128);
        context.TransitionResource(Scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.Finish(true);
        const ScopedReadback ringMapping(boundary);
        const auto* ring = ringMapping.Data;
        std::map<uint32_t, double> loopAreas;
        for (uint32_t i = 0; i < Status[1]; ++i)
        {
            if (Read<uint32_t>(ring, i * 128 + 108) == 0) continue;
            const uint32_t leader = Read<uint32_t>(ring, i * 128 + 48);
            if ((Read<uint32_t>(ring, i * 128 + 52) & 5) || !Read<uint32_t>(ring, leader * 128 + 84)) continue;
            const auto a = Read<Math::XMFLOAT2>(ring, i * 128 + 24), b = Read<Math::XMFLOAT2>(ring, i * 128 + 32);
            loopAreas[leader] += Cross(a.x, a.y, b.x, b.y) * 0.5;
        }

        // 입력 Data를 매핑하고 부모 삼각형별 면적·부피를 component-local에서 구한다.
        double expectedArea = 0;
        for (const auto& loop : loopAreas) expectedArea += std::abs(loop.second);
        const ScopedReadback parentDataMapping(DiagnosticInputData);
        const auto* parentData = parentDataMapping.Data;
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
                    p[corner] = ParentPosition(parentData, desc, section, indices[corner]);
                }
                const double volume = SignedVolume(p[0],p[1],p[2]);
                parentVolume += volume;
                absoluteVolume += std::abs(volume);
                parentAreas[desc.FirstTriangle+tri] = TriangleArea(p[0],p[1],p[2]);
                parentArea += parentAreas[desc.FirstTriangle+tri];
            }
        }

        // 양쪽 출력과 출처를 읽어 부모 보간·단면·충돌 포함을 검사한다.
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
            read.CopyBuffer(data, out.Data);
            read.CopyBuffer(pose, out.Pose);
            read.CopyBuffer(origin, out.Provenance);
            read.TransitionResource(out.Data, D3D12_RESOURCE_STATE_GENERIC_READ);
            read.TransitionResource(out.Pose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            read.TransitionResource(out.Provenance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            read.Finish(true);
            const ScopedReadback packedMapping(data);
            const auto* packed = packedMapping.Data;
            const ScopedReadback verticesMapping(pose);
            const auto* vertices = verticesMapping.Data;
            const ScopedReadback provenanceMapping(origin);
            const auto* provenance = provenanceMapping.Data;
            double capArea = 0, volume = 0;
            uint32_t triangles = 0;

            // 계승 hull의 요소별 지지 평면과 허용 오차를 준비한다.
            // hull 최대 변 길이에 비례한 float 오차와 snap 후 MinHullVertDistance 이내 병합 근사를 흡수한다.
            std::array<std::vector<std::vector<MiniCollision::Plane>>, 2> hullPlanes;
            double hullExtent = 0;
            std::array<double, 2> hullMaxExcess{ -1.0e30, -1.0e30 };
            for (const MiniCollision::ConvexElem& elem : ContainmentHulls[side].ConvexElems)
            {
                hullExtent = std::max({hullExtent, elem.ElemBox.Max.x - elem.ElemBox.Min.x,
                    elem.ElemBox.Max.y - elem.ElemBox.Min.y, elem.ElemBox.Max.z - elem.ElemBox.Min.z});
            }
            double hullTolerance = hullExtent * 1.0e-4 + MiniCollision::MinHullVertDistance + std::sqrt(MiniCollision::DefaultSnapDistSq);
            std::array<uint32_t, 2> hullChecked{};

            // 두 집합의 지지 평면을 준비하되 허용오차는 슬라이스 범위에서만 구한다.
            for (uint32_t kind = 0; kind < 2; ++kind)
            {
                const auto& aggregate = kind == 0 ? ContainmentHulls[side] : RefitContainmentHulls[side];
                for (const MiniCollision::ConvexElem& elem : aggregate.ConvexElems)
                {
                    hullPlanes[kind].emplace_back();
                    elem.GetPlanes(hullPlanes[kind].back());
                }
            }

            // 이 쪽 정점의 절단 평면까지 최대 거리(로컬 단위)다. 평면을 정규화하려고 법선 길이로 나눈다.
            double thickness = 0;
            const double planeLength = std::sqrt(double(float(Constants.Plane.GetX()))*float(Constants.Plane.GetX()) +
                double(float(Constants.Plane.GetY()))*float(Constants.Plane.GetY()) + double(float(Constants.Plane.GetZ()))*float(Constants.Plane.GetZ()));

            // section별 유효 출력 위치와 부모 출처를 검증한다.
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

                    // 출력 Data/Pose 위치의 기존 비트 일치와 유한성 검사를 축별로 보고한다.
                    const auto packedPosition = Read<Math::XMFLOAT3>(packed, size_t(global) * 28);
                    const float actualPosition[3] = { p.x, p.y, p.z };
                    const float expectedPosition[3] = { packedPosition.x, packedPosition.y, packedPosition.z };
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        const bool finite = CheckValue("output position", section, i, axis, actualPosition[axis], expectedPosition[axis], 0);
                        const bool sameBits = std::memcmp(packed + size_t(global) * 28 + axis * 4,
                            vertices + size_t(global) * 40 + axis * 4, 4) == 0;
                        if (finite && !sameBits)
                            Utility::Printf("GPU cut diagnostic FAIL: kind=position bits section=%u vertex=%u axis=%u actual=%.9g expected=%.9g tolerance=0\n",
                                section, i, axis, actualPosition[axis], expectedPosition[axis]);
                        Require(finite && sameBits, "Packed/posed position differs or is nonfinite");
                    }
                    Require(std::isfinite(n.x) && std::abs(n.x*n.x+n.y*n.y+n.z*n.z-1) < 0.01f, "Invalid result normal");
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
                        const auto a = ParentPosition(parentData, desc, section, ia);
                        const auto b = ParentPosition(parentData, desc, section, ib);
                        const double tolerance = 2.0e-6*std::max({1.0,std::abs(double(a.x)),std::abs(double(a.y)),std::abs(double(a.z)),
                            std::abs(double(b.x)),std::abs(double(b.y)),std::abs(double(b.z))});

                        // 부모 보간 오차의 축과 실제·기대 좌표를 실패 전에 남긴다.
                        const float actual[3] = { p.x, p.y, p.z };
                        const double expected[3] = { double(a.x) + (double(b.x) - a.x) * t,
                            double(a.y) + (double(b.y) - a.y) * t, double(a.z) + (double(b.z) - a.z) * t };
                        for (uint32_t axis = 0; axis < 3; ++axis)
                            Require(CheckValue("parent interpolation", section, i, axis, actual[axis], expected[axis], tolerance),
                                "Output position differs from parent interpolation");
                    }

                    // 평면 반대편으로 벗어난 위치도 실제 거리와 허용치를 보고한다.
                    const float distance = Math::Dot(Constants.Plane, Math::Vector4(Math::Vector3(p), 1));
                    const double expectedDistance = side == 0 ? std::max(0.0, double(distance)) : std::min(0.0, double(distance));
                    Require(CheckValue("cut plane distance", section, i, 3, distance, expectedDistance, Constants.Epsilon * 4),
                        "Wrong side of cut plane");
                    thickness = std::max(thickness, std::abs(double(distance)) / planeLength);

                    // 정점이 계승 hull 요소 중 하나의 모든 지지 평면 안쪽(PlaneDot <= tol)에 있어야 한다.
                    for (uint32_t kind = 0; kind < 2; ++kind)
                    {
                        if (hullPlanes[kind].empty()) continue;
                        double bestExcess = 1.0e30;
                        for (const auto& planes : hullPlanes[kind])
                        {
                            double excess = -1.0e30;
                            for (const MiniCollision::Plane& plane : planes)
                                excess = std::max(excess, plane.PlaneDot({ p.x, p.y, p.z }));
                            bestExcess = std::min(bestExcess, excess);
                        }
                        hullMaxExcess[kind] = std::max(hullMaxExcess[kind], bestExcess);
                        ++hullChecked[kind];
                    }
                }

                // 출력 삼각형별 부모 면적·방향 부피와 cap 면적을 누적한다.
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

            // 기존 검사를 마친 cap 위치로 순서 무관 checksum을 기록한다.
            PrintCapChecksum(vertices, Sections.back(), Status[20 + side * Constants.Sections + Constants.Sections - 1], side);

            // 경계 기준 cap 면적과 양쪽 결과의 방향 부피를 기록한다.
            Require(std::abs(capArea-expectedArea) <= std::max(1.0e-8, expectedArea*0.001), "Cap area does not match closed boundary");
            Utility::Printf("GPU cut validation passed: side=%u triangles=%u capArea=%.9f expected=%.9f signedVolume=%.9f\n",
                side, triangles, capArea, expectedArea, volume);
            resultVolume += volume;

            // 기존 얇은 조각 규칙으로 확장한 허용오차를 두 집합에 동일하게 적용한다.
            if (thickness <= 2.0 * MiniCollision::MinHullVertDistance)
                hullTolerance = std::max(hullTolerance, 2.0 * MiniCollision::MinHullVertDistance);
            for (uint32_t kind = 0; kind < 2; ++kind)
            {
                const char* name = kind == 0 ? "sliced" : "refit";
                if (hullChecked[kind])
                {
                    Require(hullMaxExcess[kind] <= hullTolerance, "Result vertex lies outside collision hull");
                    Utility::Printf("GPU cut hull containment passed: side=%u kind=%s hulls=%zu vertices=%u maxExcess=%.9g tolerance=%.9g\n",
                        side, name, hullPlanes[kind].size(), hullChecked[kind], hullMaxExcess[kind], hullTolerance);
                }
                else if (InheritanceActive)
                {
                    // 충돌이 빈 쪽은 기존과 같이 병합 거리의 두 배 이하 두께일 때만 허용한다.
                    Require(thickness <= 2.0 * MiniCollision::MinHullVertDistance, "Result side has geometry but collision hull is empty");
                    Utility::Printf("GPU cut hull containment: side=%u kind=%s thin piece (thickness=%.9g) has no convex - body-less by policy (R3)\n",
                        side, name, thickness);
                }
                else
                    Utility::Printf("GPU cut hull containment skipped: side=%u kind=%s (parent has no collision)\n", side, name);
            }
        }

        // 부모 삼각형별 면적과 전체 방향 부피가 양쪽 결과에 보존되는지 확인한다.
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
