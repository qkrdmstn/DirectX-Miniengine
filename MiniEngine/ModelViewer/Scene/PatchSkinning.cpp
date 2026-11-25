#include "PatchSkinning.h"
#include "CutPerfLog.h"
#include "GeometryRevision.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "Util/CommandLineArg.h"
#include "PipelineState.h"
#include "ReadbackBuffer.h"
#include <cstring>
#include "CompiledShaders/PatchSkinBuildCS.h"
#include "CompiledShaders/PatchSkinRecutCS.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <array>
#include <unordered_set>
#include <stdexcept>

namespace Viewer
{
    // 패치 출처와 원본 draw를 묶어 GPU에서 bind 속성을 복원하고, 재분배가 켜지면 A 조각 mesh의 비경계 복사 정점 weight를 다시 나눈다.
    bool BuildPatchSkinVertices(const GeometryRevision& source, ByteAddressBuffer& triangleSources,
        const GeometryBuffers& piece, const PatchSkinSnapshot& snapshot, PatchSkinVertices& out,
        const std::vector<std::array<uint32_t, 2>>& triangleBodies,
        const RemainLengthTable* lengthTable, const std::vector<uint32_t>* lengthMeshes,
        const std::function<const std::vector<uint32_t>*()>& boundaryProvider)
    {
        // u16 강체 슬롯과 최초 원본 revision을 요구한다.
        if (!snapshot.SkinningAllowed || source.IsGenerated() || snapshot.JointsAtCut.empty()
            || snapshot.JointsAtCut.size() > 65535 || !triangleSources.GetBufferSize())
            return false;
        struct DrawInput
        {
            uint32_t Section, Draw, VbOffset, SourceStride;
            uint32_t IbOffset, Index32, StartIndex, BaseVertex;
            uint32_t SourceFlags, StartJoint;
        };
        struct Parameters
        {
            Math::Matrix4 InverseNode;
            uint32_t FirstVertex, VertexCount, Flags, Stride;
            uint32_t DrawCount, RigidJoint, PatchTriangles, Follow;
            uint32_t LengthEntries, BoundaryCount, Pad0, Pad1;
        };
        static_assert(sizeof(DrawInput) == 40 && sizeof(Parameters) == 112, "Patch skin shader layout");
        std::vector<DrawInput> draws;
        uint32_t flags = 259 | kSkinExtFlag;
        for (uint32_t s = 0; s < source.GetSections().size(); ++s)
        {
            const auto& mesh = source.GetMesh(s);
            flags |= mesh.psoFlags & 28;
            if (uint64_t(mesh.startJoint) + mesh.numJoints > snapshot.JointsAtCut.size())
                return false;
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const auto& draw = mesh.draw[d];
                draws.push_back({ s, d, mesh.vbOffset, mesh.vbStride, mesh.ibOffset,
                    mesh.ibFormat == DXGI_FORMAT_R32_UINT, draw.startIndex, draw.baseVertex,
                    mesh.psoFlags, mesh.startJoint });
            }
        }

        // cutter의 빈 capacity 구간은 건너뛰고 실제 게시 VB 범위만 생성한다.
        uint32_t end = 0;
        for (const auto& mesh : piece.Meshes)
        {
            if (mesh.vbStride != 28 || mesh.vbOffset % 28 || mesh.vbSize % 28
                || (mesh.psoFlags & 31) != 31
                || uint64_t(mesh.vbOffset) + mesh.vbSize > piece.Data.GetBufferSize())
                return false;
            end = std::max(end, mesh.vbOffset / 28 + mesh.vbSize / 28);
        }
        const uint32_t stride = 32 + kSkinExtSize + ((flags & 4) ? 4 : 0) + ((flags & 8) ? 4 : 0) + ((flags & 16) ? 4 : 0);
        if (!end || draws.empty() || uint64_t(end) * stride > UINT32_MAX
            || uint64_t(end) * 32 > piece.Provenance.GetBufferSize())
            return false;

        // 기존 descriptor 뒤에 양쪽 body를 붙여 별도 GPU 버퍼를 만들지 않는다.
        const uint32_t drawCount = uint32_t(draws.size());
        for (const auto& bodies : triangleBodies)
        {
            DrawInput entry{};
            entry.Section = bodies[0];
            entry.Draw = bodies[1];
            draws.push_back(entry);
        }

        // 추출 셰이더와 같은 root descriptor 방식으로 compute 파이프라인을 재사용한다.
        static RootSignature root;
        static ComputePSO pso(L"Build patch skin vertices");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(11);
            root[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 5; ++i)
                root[i + 1].InitAsBufferSRV(i);
            root[6].InitAsBufferUAV(0);
            root[7].InitAsBufferSRV(5);
            root[8].InitAsBufferSRV(6);
            root[9].InitAsBufferSRV(7);
            root[10].InitAsBufferUAV(1);
            root.Finalize(L"Patch skin build");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pPatchSkinBuildCS, sizeof(g_pPatchSkinBuildCS));
            pso.Finalize();
            initialized = true;
        }

        // section flags의 합집합을 단일 출력 레이아웃으로 보관한다.
        StructuredBuffer descriptors;
        descriptors.Create(L"Patch skin draws", uint32_t(draws.size()), sizeof(DrawInput), draws.data());
        StructuredBuffer cutJoints;
        cutJoints.Create(L"Patch skin joints at cut", uint32_t(snapshot.JointsAtCut.size()), sizeof(Joint), snapshot.JointsAtCut.data());
        // 재분배 대상이면 검증이 끝난 지금 경계 플래그를 얻고, 표와 플래그 크기가 맞을 때만 경로를 켠다.
        const std::vector<uint32_t>* boundaryFlags = lengthTable && boundaryProvider ? boundaryProvider() : nullptr;
        struct LengthEntry
        {
            float Ratio;
            uint32_t Descendant;
            int32_t Fallback;
            uint32_t Pad;
        };
        const bool lengthEnabled = lengthTable && boundaryFlags && !lengthTable->Ratio.empty() && !boundaryFlags->empty()
            && lengthTable->Ratio.size() == snapshot.JointsAtCut.size() && lengthTable->Fallback.size() == lengthTable->Ratio.size();
        std::vector<LengthEntry> entries(lengthEnabled ? lengthTable->Ratio.size() : 1, LengthEntry{ 1.0f, 0, -1, 0 });
        for (size_t k = 0; lengthEnabled && k < entries.size(); ++k)
            entries[k] = { lengthTable->Ratio[k], lengthTable->Descendant[k], lengthTable->Fallback[k], 0 };
        out.LengthEnabled = lengthEnabled;

        // 팔레트별 비율 표 버퍼를 만든다.
        StructuredBuffer lengthBuffer;
        lengthBuffer.Create(L"Patch skin length table", uint32_t(entries.size()), sizeof(LengthEntry), entries.data());

        // 경계 플래그 버퍼를 만든다(꺼져 있으면 1칸 더미).
        const uint32_t dummyFlag = 1;
        StructuredBuffer boundaryBuffer;
        boundaryBuffer.Create(L"Patch skin boundary flags", lengthEnabled ? uint32_t(boundaryFlags->size()) : 1, sizeof(uint32_t),
            lengthEnabled ? boundaryFlags->data() : &dummyFlag);

        // 계수 버퍼를 0으로 초기화해 만든다.
        const uint32_t zeros[16] = {};
        ByteAddressBuffer counters;
        counters.Create(L"Patch skin length counters", 16, sizeof(uint32_t), zeros);

        out.Data.Create(L"Patch skin vertices", end, stride);
        out.Flags = flags;
        out.Stride = stride;
        auto& graphics = GraphicsContext::Begin(L"Build patch skin vertices");
        CutPerf::Gpu cutPerfSkinBuild(graphics, "PatchSkinBuild-whole");
        auto& compute = graphics.GetComputeContext();
        compute.SetRootSignature(root);
        compute.SetPipelineState(pso);
        GpuBuffer* inputs[] = { &const_cast<ByteAddressBuffer&>(source.GetDataBuffer()), &triangleSources,
            &const_cast<ByteAddressBuffer&>(piece.Data), &const_cast<ByteAddressBuffer&>(piece.Provenance), &descriptors };
        for (uint32_t i = 0; i < 5; ++i)
        {
            compute.TransitionResource(*inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.SetBufferSRV(i + 1, *inputs[i]);
        }
        compute.TransitionResource(cutJoints, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetBufferSRV(7, cutJoints);
        compute.TransitionResource(lengthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetBufferSRV(8, lengthBuffer);
        compute.TransitionResource(boundaryBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetBufferSRV(9, boundaryBuffer);
        compute.TransitionResource(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetBufferUAV(10, counters);
        compute.TransitionResource(out.Data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetBufferUAV(6, out.Data);
        Parameters c{ Math::Invert(snapshot.MeshNodeAtCut), 0, 0, flags, stride,
            drawCount, uint32_t(snapshot.JointsAtCut.size()), uint32_t(triangleBodies.size()),
            PatchAnimFollowEnabled() && snapshot.AnimFollow ? 1u : 0u,
            0u, lengthEnabled ? uint32_t(boundaryFlags->size()) : 0u, 0, 0 };
        uint32_t meshIndex = 0;
        for (const auto& mesh : piece.Meshes)
        {
            // A 조각 section의 mesh만 재분배를 켠다(B mesh 정점은 원래 레코드를 유지한다).
            const bool inLength = lengthEnabled && lengthMeshes && std::find(lengthMeshes->begin(), lengthMeshes->end(), meshIndex) != lengthMeshes->end();
            ++meshIndex;
            c.LengthEntries = inLength ? uint32_t(entries.size()) : 0u;
            c.FirstVertex = mesh.vbOffset / 28;
            c.VertexCount = mesh.vbSize / 28;
            if (!c.VertexCount)
                continue;
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            const uint32_t groups = (c.VertexCount + 63) / 64;
            compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
        }

        // 생성 시 한 번 skin VB를 읽어 section별 bind 구를 만든다.
        ReadbackBuffer readback;
        readback.Create(L"Patch skin bind bounds", end, stride);
        graphics.CopyBuffer(readback, out.Data);
        ReadbackBuffer counterReadback;
        counterReadback.Create(L"Patch skin length counters", 16, sizeof(uint32_t));
        graphics.TransitionResource(counters, D3D12_RESOURCE_STATE_COPY_SOURCE);
        graphics.CopyBuffer(counterReadback, counters);

        // 원본·조각 Data는 이후 VB/IB로도 묶이므로 다른 절단 작업처럼 GENERIC_READ로 되돌린다.
        for (uint32_t i = 0; i < 5; ++i)
            graphics.TransitionResource(*inputs[i], D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(out.Data, D3D12_RESOURCE_STATE_GENERIC_READ);

        // 호출자가 보관한 원본·출처·조각과 임시 descriptor는 완료 뒤에만 해제할 수 있다.
        cutPerfSkinBuild.End();
        graphics.Finish(true);

        // 원본 경계 방식처럼 section 전체 구를 모든 joint에 공통 사용하며 K 위치도 그대로 포함한다.
        const auto* bytes = static_cast<const uint8_t*>(readback.Map());
        out.BindBounds.clear();
        for (const auto& mesh : piece.Meshes)
        {
            Math::Vector3 minimum(Math::kZero);
            Math::Vector3 maximum(Math::kZero);
            for (uint32_t v = 0; v < mesh.vbSize / 28; ++v)
            {
                DirectX::XMFLOAT3 position;
                std::memcpy(&position, bytes + (mesh.vbOffset / 28 + v) * stride, sizeof(position));
                const Math::Vector3 point(position);
                minimum = v ? Math::Min(minimum, point) : point;
                maximum = v ? Math::Max(maximum, point) : point;
            }
            const auto center = (minimum + maximum) * 0.5f;
            const float radius = std::max(float(Math::Length(maximum - minimum)) * 0.5f, 1.0e-6f);
            out.BindBounds.emplace_back(center, Math::Scalar(radius));
        }
        // 재분배 계수를 복사해 호출자가 로그로 남기게 한다.
        const auto* counts = static_cast<const uint32_t*>(counterReadback.Map());
        std::copy(counts, counts + 16, out.LengthCounters);
        counterReadback.Unmap();
        readback.Unmap();
        return true;
    }

    // 원본 VB/IB를 한 번 읽어 패치 꼭짓점이 원본에 남는 삼각형과 위치를 공유하는지 표로 만든다.
    // 위치는 section 노드 행렬로 공통 공간에 옮긴 뒤 10 마이크로 격자로 용접하고 이웃 27칸을 함께 조회한다.
    std::vector<uint32_t> BuildPatchBoundaryFlags(const GeometryRevision& source,
        const std::vector<uint32_t>& patchTriangles, const std::vector<uint32_t>& previousState,
        const std::vector<Math::Matrix4>& sectionNodes)
    {
        // 원본 Data 전체를 readback으로 복사하고 GPU 완료를 기다린다.
        auto& buffer = const_cast<ByteAddressBuffer&>(source.GetDataBuffer());
        const uint32_t size = uint32_t(buffer.GetBufferSize());
        ReadbackBuffer readback;
        readback.Create(L"Patch boundary source", size, 1);
        auto& context = GraphicsContext::Begin(L"Patch boundary readback");
        context.CopyBuffer(readback, buffer);
        context.TransitionResource(buffer, D3D12_RESOURCE_STATE_GENERIC_READ);
        context.Finish(true);

        // 전역 삼각형 번호에서 패치 삼각형 번호를 찾는 표를 만든다.
        uint32_t total = 0;
        for (size_t s = 0; s < source.GetSections().size(); ++s)
            for (uint32_t d = 0; d < source.GetMesh(s).numDraws; ++d)
                total += source.GetMesh(s).draw[d].primCount / 3;
        std::vector<uint32_t> patchOf(total, UINT32_MAX);
        for (uint32_t t = 0; t < patchTriangles.size(); ++t)
            if (patchTriangles[t] < total)
                patchOf[patchTriangles[t]] = t;

        // 모든 삼각형의 꼭짓점 위치를 읽어 패치 꼭짓점은 따로 두고, 남는 삼각형의 위치는 집합에 모은다.
        using Key = std::array<int64_t, 3>;
        struct KeyHash
        {
            size_t operator()(const Key& key) const
            {
                return size_t(uint64_t(key[0]) * 73856093ull ^ uint64_t(key[1]) * 19349663ull ^ uint64_t(key[2]) * 83492791ull);
            }
        };
        const auto* bytes = static_cast<const uint8_t*>(readback.Map());
        std::unordered_set<Key, KeyHash> kept;
        std::vector<Key> corners(patchTriangles.size() * 3);
        bool valid = bytes != nullptr;
        uint32_t global = 0;
        for (size_t s = 0; valid && s < source.GetSections().size(); ++s)
        {
            const auto& mesh = source.GetMesh(s);
            const bool index32 = mesh.ibFormat == DXGI_FORMAT_R32_UINT;
            for (uint32_t d = 0; valid && d < mesh.numDraws; ++d)
                for (uint32_t tri = 0; valid && tri < mesh.draw[d].primCount / 3; ++tri, ++global)
                {
                    // 이전 절단으로 이미 제거된 삼각형은 원본에 남는 삼각형이 아니다.
                    const bool inPatch = patchOf[global] != UINT32_MAX;
                    if (!inPatch && global < previousState.size() && (previousState[global] & 0x80000000u))
                        continue;
                    for (uint32_t corner = 0; valid && corner < 3; ++corner)
                    {
                        // PatchSkinBuildCS의 Index와 같은 방식으로 IB와 baseVertex를 해석해 정점 위치를 읽는다.
                        const uint32_t at = mesh.draw[d].startIndex + tri * 3 + corner;
                        const uint64_t indexAddress = uint64_t(mesh.ibOffset) + uint64_t(at) * (index32 ? 4u : 2u);
                        if (indexAddress + (index32 ? 4 : 2) > size)
                        {
                            valid = false;
                            break;
                        }
                        uint32_t index = 0;
                        std::memcpy(&index, bytes + indexAddress, index32 ? 4 : 2);
                        const uint64_t vertexAddress = uint64_t(mesh.vbOffset) + uint64_t(index + mesh.draw[d].baseVertex) * mesh.vbStride;
                        float p[3];
                        if (vertexAddress + sizeof(p) > size)
                        {
                            valid = false;
                            break;
                        }
                        std::memcpy(p, bytes + vertexAddress, sizeof(p));

                        // section 노드 행렬로 공통 공간 위치를 만든 뒤 격자 칸 번호로 바꾼다.
                        const Math::Vector3 local(p[0], p[1], p[2]);
                        const Math::Vector3 common = s < sectionNodes.size()
                            ? Math::Vector3(sectionNodes[s] * Math::Vector4(local, 1)) : local;
                        const Key key = { std::llround(float(common.GetX()) * 1e5f), std::llround(float(common.GetY()) * 1e5f),
                            std::llround(float(common.GetZ()) * 1e5f) };
                        if (inPatch)
                            corners[size_t(patchOf[global]) * 3 + corner] = key;
                        else
                            kept.insert(key);
                    }
                }
        }
        readback.Unmap();
        if (!valid)
            return {};

        // 패치 꼭짓점 주변 27칸에 남는 삼각형의 위치가 있으면 경계로 표시한다.
        std::vector<uint32_t> flags(corners.size());
        for (size_t i = 0; i < corners.size(); ++i)
            for (int64_t dx = -1; dx <= 1 && !flags[i]; ++dx)
                for (int64_t dy = -1; dy <= 1 && !flags[i]; ++dy)
                    for (int64_t dz = -1; dz <= 1 && !flags[i]; ++dz)
                        flags[i] = kept.count({ corners[i][0] + dx, corners[i][1] + dy, corners[i][2] + dz }) ? 1u : 0u;
        return flags;
    }

    // 부모 skin의 복사 정점 레코드는 통째로 잇고 새 절단면 정점은 K 슬롯 레코드로 자식 skin VB를 만든다.
    bool BuildRecutPatchSkinVertices(const PatchSkinPublication& parent, const GeometryBuffers& cutResult,
        const Math::Matrix4& kFrame, PatchSkinPublication& out, RecutSkinStats* stats, const RecutCheckInput* check)
    {
        // 부모 skin이 유효하고 결과가 28바이트 정점의 Data 레이아웃인지 검사한다.
        const uint32_t stride = parent.Vertices.Stride, flags = parent.Vertices.Flags;
        const uint32_t k = uint32_t(parent.Snapshot.JointsAtCut.size());
        if (!parent.Snapshot.SkinningAllowed || !k || k > 65535 || !stride || stride % 4
            || !(flags & kSkinExtFlag) || parent.Sections.empty() || !parent.Vertices.Data.GetBufferSize())
            return false;
        uint32_t end = 0;
        for (const auto& mesh : cutResult.Meshes)
        {
            if (mesh.vbStride != 28 || mesh.vbOffset % 28 || mesh.vbSize % 28 || (mesh.psoFlags & 31) != 31
                || uint64_t(mesh.vbOffset) + mesh.vbSize > cutResult.Data.GetBufferSize())
                return false;
            end = std::max(end, mesh.vbOffset / 28 + mesh.vbSize / 28);
        }
        if (!end || uint64_t(end) * stride > UINT32_MAX || uint64_t(end) * 32 > cutResult.Provenance.GetBufferSize())
            return false;

        // 부모 section의 첫 정점 번호를 GPU가 Origin.x로 찾도록 배열로 만든다.
        std::vector<uint32_t> parentFirst;
        for (const auto& section : parent.Sections)
            parentFirst.push_back(section.FirstVertex);
        struct Parameters
        {
            Math::Matrix4 InverseK;
            uint32_t FirstVertex, VertexCount, Flags, Stride;
            uint32_t RigidJoint, ParentSectionCount, Follow, ParentVertexCount;
        };
        static_assert(sizeof(Parameters) == 96, "Patch skin recut shader layout");

        // 재절단 compute 파이프라인을 처음 한 번만 만든다.
        static RootSignature root;
        static ComputePSO pso(L"Build recut patch skin vertices");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(6);
            root[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 4; ++i)
                root[i + 1].InitAsBufferSRV(i);
            root[5].InitAsBufferUAV(0);
            root.Finalize(L"Patch skin recut");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pPatchSkinRecutCS, sizeof(g_pPatchSkinRecutCS));
            pso.Finalize();
            initialized = true;
        }

        // 자식 skin 출력과 부모 section 배열 buffer를 만든다.
        StructuredBuffer firstVertices;
        firstVertices.Create(L"Recut parent section first vertices", uint32_t(parentFirst.size()), sizeof(uint32_t), parentFirst.data());
        out.Vertices.Data.Create(L"Recut patch skin vertices", end, stride);
        out.Vertices.Flags = flags;
        out.Vertices.Stride = stride;

        // 예외가 날 수 있는 readback 생성은 컨텍스트를 열기 전에 끝낸다.
        const auto readStart = std::chrono::steady_clock::now();
        ReadbackBuffer skinReadback, provenanceReadback;
        skinReadback.Create(L"Recut patch skin bounds", end, stride);
        provenanceReadback.Create(L"Recut patch skin provenance", end, 32);

        // RS-2b 검증은 진단 입력이 있을 때만 하며 부모 skin VB와 결과 Data 앞 end개 정점의 readback을 미리 만든다.
        const bool verify = check != nullptr;
        ReadbackBuffer parentReadback, dataReadback;
        const uint32_t parentCount = uint32_t(parent.Vertices.Data.GetBufferSize() / stride);
        if (verify)
        {
            parentReadback.Create(L"Recut parent skin check", parentCount, stride);
            dataReadback.Create(L"Recut result data check", end, 28);
        }

        // compute 컨텍스트에 루트 시그니처와 파이프라인을 설정한다.
        auto& graphics = GraphicsContext::Begin(L"Build recut patch skin vertices");
        auto& compute = graphics.GetComputeContext();
        compute.SetRootSignature(root);
        compute.SetPipelineState(pso);

        // 입력 SRV 4개와 출력 UAV를 바인딩한다.
        GpuBuffer* inputs[] = { &const_cast<ByteAddressBuffer&>(parent.Vertices.Data),
            &const_cast<ByteAddressBuffer&>(cutResult.Data), &const_cast<ByteAddressBuffer&>(cutResult.Provenance), &firstVertices };
        for (uint32_t i = 0; i < 4; ++i)
        {
            compute.TransitionResource(*inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.SetBufferSRV(i + 1, *inputs[i]);
        }
        compute.TransitionResource(out.Vertices.Data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetBufferUAV(5, out.Vertices.Data);

        // 파라미터를 만들고 결과 mesh마다 정점 범위를 dispatch한다.
        Parameters c{ Math::Invert(kFrame), 0, 0, flags, stride, k, uint32_t(parent.Sections.size()),
            PatchAnimFollowEnabled() && parent.Snapshot.AnimFollow ? 1u : 0u, parentCount };
        for (const auto& mesh : cutResult.Meshes)
        {
            c.FirstVertex = mesh.vbOffset / 28;
            c.VertexCount = mesh.vbSize / 28;
            if (!c.VertexCount)
                continue;
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            const uint32_t groups = (c.VertexCount + 63) / 64;
            compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
        }

        // skin VB는 크기가 같아 통째로, provenance는 용량이 더 클 수 있어 앞 end*32 바이트만 복사한다.
        auto& provenanceSource = const_cast<ByteAddressBuffer&>(cutResult.Provenance);
        graphics.CopyBuffer(skinReadback, out.Vertices.Data);
        graphics.TransitionResource(provenanceSource, D3D12_RESOURCE_STATE_COPY_SOURCE);
        graphics.CopyBufferRegion(provenanceReadback, 0, provenanceSource, 0, size_t(end) * 32);

        // 부모 skin VB와 결과 Data는 크기 제약 때문에 명시 전이 뒤 CopyBufferRegion으로 앞쪽만 복사한다.
        auto& parentSource = const_cast<ByteAddressBuffer&>(parent.Vertices.Data);
        auto& dataSource = const_cast<ByteAddressBuffer&>(cutResult.Data);
        if (verify)
        {
            graphics.TransitionResource(parentSource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            graphics.TransitionResource(dataSource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            graphics.CopyBufferRegion(parentReadback, 0, parentSource, 0, size_t(parentCount) * stride);
            graphics.CopyBufferRegion(dataReadback, 0, dataSource, 0, size_t(end) * 28);
        }

        // 입력 buffer는 다른 사용처를 위해 GENERIC_READ로 되돌린다.
        for (uint32_t i = 0; i < 3; ++i)
            graphics.TransitionResource(*inputs[i], D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(out.Vertices.Data, D3D12_RESOURCE_STATE_GENERIC_READ);

        // GPU 완료까지 기다려 부모 skin·결과 buffer를 호출자가 안전하게 해제할 수 있게 한다.
        graphics.Finish(true);

        // 자식 publication의 snapshot은 부모 값을 그대로 복사하고 section 범위는 결과 기준으로 새로 만든다.
        out.Snapshot = parent.Snapshot;
        out.Side = parent.Side;
        out.RimCapPairs.clear();
        out.Sections.clear();
        out.VertexCount = 0;

        // readback 버퍼를 매핑하고 집계 변수를 초기화한다.
        const auto* skin = static_cast<const uint8_t*>(skinReadback.Map());
        const auto* provenance = static_cast<const uint8_t*>(provenanceReadback.Map());
        const auto* parentSkin = verify ? static_cast<const uint8_t*>(parentReadback.Map()) : nullptr;
        const auto* data = verify ? static_cast<const uint8_t*>(dataReadback.Map()) : nullptr;
        RecutSkinStats local;
        local.Checked = verify;

        // kFrame이 요청 시점 N·palette[K].posXform과 같은지 최대 성분 차이를 잰다.
        if (verify && !check->Palette.empty())
        {
            const DirectX::XMMATRIX expectedFrame = check->Node * check->Palette.back();
            const DirectX::XMMATRIX actualFrame = kFrame;
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    local.KFrameDiff = std::max(local.KFrameDiff, std::fabs(expectedFrame.r[row].m128_f32[column] - actualFrame.r[row].m128_f32[column]));
        }

        // K 정점 검증용 행렬을 만든다(법선은 K의 역전치로 bind 방향을 월드 방향으로 되돌린다).
        const DirectX::XMMATRIX kMatrix = kFrame;
        const DirectX::XMMATRIX normalMatrix = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, kMatrix));
        // 10-10-10 UNORM으로 묶인 방향을 벡터로 복원하는 도우미다.
        auto decodeFrame = [](uint32_t packed)
        {
            // cutter와 같은 10-10-10 UNORM 방향을 [-1,1] 벡터로 복원한다.
            return DirectX::XMVectorSet(float(packed & 1023) / 1023.0f * 2 - 1, float((packed >> 10) & 1023) / 1023.0f * 2 - 1,
                float((packed >> 20) & 1023) / 1023.0f * 2 - 1, 0);
        };
        out.Vertices.BindBounds.clear();

        // section별 BindBounds를 구하고 복사 정점과 K 정점 수를 센다.
        for (const auto& mesh : cutResult.Meshes)
        {
            const uint32_t first = mesh.vbOffset / 28, count = mesh.vbSize / 28;
            out.Sections.push_back({ first, count });
            out.VertexCount += count;
            Math::Vector3 minimum(Math::kZero);
            Math::Vector3 maximum(Math::kZero);
            for (uint32_t v = 0; v < count; ++v)
            {
                DirectX::XMFLOAT3 position;
                std::memcpy(&position, skin + (first + v) * stride, sizeof(position));
                const Math::Vector3 point(position);
                minimum = v ? Math::Min(minimum, point) : point;
                maximum = v ? Math::Max(maximum, point) : point;

                // 셰이더와 같은 판정으로 복사 정점을 세고 K 정점은 joint 슬롯이 K인지 확인한다.
                uint32_t origin[4];
                std::memcpy(origin, provenance + (first + v) * 32, sizeof(origin));
                const bool noCap = origin[0] != 0xffffffff && origin[2] == origin[3];
                const bool copied = noCap && origin[0] < parent.Sections.size();
                if (noCap && !copied) ++local.BadParent;
                uint32_t joints = 0;
                std::memcpy(&joints, skin + (first + v) * stride + stride - 16, sizeof(joints));
                ++local.Vertices;

                // 끝점 보간 종류 1·2·3을 복사 여부와 상관없이 정점마다 센다.
                uint32_t kind = 0;
                std::memcpy(&kind, skin + size_t(first + v) * stride + stride - kSkinExtInternal, sizeof(kind));
                if (kind == 1) ++local.Skipped;
                else if (kind == 2) ++local.SkippedRim;
                else if (kind == 3) ++local.SkippedCap;
                if (copied)
                {
                    ++local.Copied;
                    if (origin[2] >= parent.Sections[origin[0]].VertexCount) ++local.BadParent;

                    // 복사 정점은 부모 레코드와 Stride 바이트가 같은지 비교하고 범위를 벗어나면 불일치로 센다.
                    if (verify)
                    {
                        const uint64_t parentVertex = uint64_t(parent.Sections[origin[0]].FirstVertex) + origin[2];
                        if (parentVertex >= parentCount
                            || std::memcmp(skin + size_t(first + v) * stride, parentSkin + size_t(parentVertex) * stride, stride) != 0)
                            ++local.CopyMismatch;

                        // 내부 교점 레코드는 끝점 보간이라 건너뛰고, 나머지는 요청 시점 팔레트로 독립 스키닝해 구운 위치와 비교한다.
                        uint32_t internal = 0;
                        std::memcpy(&internal, skin + size_t(first + v) * stride + stride - kSkinExtInternal, sizeof(internal));
                        if (internal)
                        {
                            // 끝점 보간 정점은 독립 스키닝 비교에서 뺀다.
                        }
                        else
                        {
                            uint16_t jointIds[4], weights[4];
                            std::memcpy(jointIds, skin + size_t(first + v) * stride + stride - 16, sizeof(jointIds));
                            std::memcpy(weights, skin + size_t(first + v) * stride + stride - 8, sizeof(weights));
                            DirectX::XMVECTOR sum = DirectX::XMVectorZero();
                            bool valid = true;
                            for (int i = 0; i < 4; ++i)
                            {
                                if (!weights[i])
                                    continue;
                                if (jointIds[i] >= check->Palette.size())
                                {
                                    valid = false;
                                    break;
                                }
                                const DirectX::XMMATRIX palette = check->Palette[jointIds[i]];
                                sum = DirectX::XMVectorAdd(sum, DirectX::XMVectorScale(
                                    DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&position), palette), float(weights[i]) / 65535.0f));
                            }
                            DirectX::XMFLOAT3 bakedCopied;
                            std::memcpy(&bakedCopied, data + size_t(first + v) * 28, sizeof(bakedCopied));
                            const DirectX::XMMATRIX node = check->Node;
                            const float skinError = valid ? DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(
                                DirectX::XMVector3Transform(sum, node), DirectX::XMLoadFloat3(&bakedCopied)))) : 1.0e9f;
                            local.CopiedSkinMax = std::max(local.CopiedSkinMax, skinError);
                        }
                    }
                }
                else
                {
                    ++local.K;
                    if (joints != k) ++local.BadK;
                    if (kind) ++local.Endpoint;
                    if (!verify)
                        continue;

                    // AF-5: 추종 중 끝점을 쓰지 못한 새 정점은 출처와 부모 레코드로 폴백 사유를 분류한다.
                    if (!kind && PatchAnimFollowEnabled() && parent.Snapshot.AnimFollow)
                    {
                        const bool isCap = origin[0] == 0xffffffff;
                        if (isCap && parent.Sections.size() != 1)
                            ++local.FallbackSection;
                        else if (!isCap && origin[0] >= parent.Sections.size())
                            ++local.FallbackRange;
                        else
                        {
                            const uint64_t base = parent.Sections[isCap ? 0 : origin[0]].FirstVertex;
                            const uint64_t ends[2] = { base + origin[2], base + origin[3] };
                            if (ends[0] >= parentCount || ends[1] >= parentCount)
                                ++local.FallbackRange;
                            else
                            {
                                uint32_t a = 0, b = 0;
                                std::memcpy(&a, parentSkin + size_t(ends[0]) * stride + stride - kSkinExtInternal, sizeof(a));
                                std::memcpy(&b, parentSkin + size_t(ends[1]) * stride + stride - kSkinExtInternal, sizeof(b));
                                if (a || b) ++local.FallbackInternal;
                            }
                        }
                    }

                    // AF-5: 끝점 정점은 두 끝점을 요청 시점 팔레트로 스키닝해 t로 보간한 위치를 구운 위치와 비교한다.
                    if (kind)
                    {
                        const uint8_t* record = skin + size_t(first + v) * stride;
                        float t = 0;
                        std::memcpy(&t, record + stride - kSkinExtT, sizeof(t));
                        DirectX::XMVECTOR ends[2];
                        bool valid = true;
                        for (uint32_t e = 0; e < 2; ++e)
                        {
                            const uint8_t* end = record + stride - kSkinExtEndpoint0 + e * kSkinEndpointSize;
                            DirectX::XMFLOAT3 bind;
                            uint16_t ids[4], ws[4];
                            std::memcpy(&bind, end, sizeof(bind));
                            std::memcpy(ids, end + kSkinEndpointJoints, sizeof(ids));
                            std::memcpy(ws, end + kSkinEndpointWeights, sizeof(ws));
                            const float total = std::max(1.0f, float(ws[0]) + ws[1] + ws[2] + ws[3]);
                            ends[e] = DirectX::XMVectorZero();
                            for (int i = 0; i < 4 && valid; ++i)
                            {
                                if (!ws[i]) continue;
                                valid = ids[i] < check->Palette.size();
                                if (valid)
                                    ends[e] = DirectX::XMVectorAdd(ends[e], DirectX::XMVectorScale(DirectX::XMVector3Transform(
                                        DirectX::XMLoadFloat3(&bind), check->Palette[ids[i]]), float(ws[i]) / total));
                            }
                        }
                        DirectX::XMFLOAT3 bakedEnd;
                        std::memcpy(&bakedEnd, data + size_t(first + v) * 28, sizeof(bakedEnd));
                        const DirectX::XMMATRIX node = check->Node;
                        const float endpointError = valid ? DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(
                            DirectX::XMVector3Transform(DirectX::XMVectorLerp(ends[0], ends[1], t), node), DirectX::XMLoadFloat3(&bakedEnd)))) : 1.0e9f;
                        float& worst = kind == 3 ? local.EndpointCapMax : local.EndpointSplitMax;
                        worst = std::max(worst, endpointError);
                    }

                    // K 정점은 bind 위치에 N·D를 곱한 값을 결과 Data의 구운 위치와 비교한다.
                    const uint8_t* baked = data + size_t(first + v) * 28;
                    DirectX::XMFLOAT3 bakedPosition;
                    std::memcpy(&bakedPosition, baked, sizeof(bakedPosition));
                    const auto moved = DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&position), kMatrix);
                    const float error = DirectX::XMVectorGetX(DirectX::XMVector3Length(
                        DirectX::XMVectorSubtract(moved, DirectX::XMLoadFloat3(&bakedPosition))));
                    local.KPositionMax = std::max(local.KPositionMax, error);

                    // 법선은 bind 방향을 역전치로 되돌려 구운 법선과의 각도 차를 잰다.
                    uint32_t bindPacked = 0, bakedPacked = 0;
                    std::memcpy(&bindPacked, skin + size_t(first + v) * stride + 12, sizeof(bindPacked));
                    std::memcpy(&bakedPacked, baked + 12, sizeof(bakedPacked));
                    const auto expected = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(decodeFrame(bindPacked), normalMatrix));
                    const auto actual = DirectX::XMVector3Normalize(decodeFrame(bakedPacked));
                    const float cosine = std::min(1.0f, std::max(-1.0f, DirectX::XMVectorGetX(DirectX::XMVector3Dot(expected, actual))));
                    local.KNormalMaxDeg = std::max(local.KNormalMaxDeg, std::acos(cosine) * 57.29578f);
                }
            }
            const auto center = (minimum + maximum) * 0.5f;
            const float radius = std::max(float(Math::Length(maximum - minimum)) * 0.5f, 1.0e-6f);
            out.Vertices.BindBounds.emplace_back(center, Math::Scalar(radius));
        }
        skinReadback.Unmap();
        provenanceReadback.Unmap();
        if (verify)
        {
            parentReadback.Unmap();
            dataReadback.Unmap();
        }
        local.ReadbackMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count();
        if (stats)
            *stats = local;
        return true;
    }

    // 추종 기본값은 1이며 클립 전환 시연은 0으로 재현한다. 명시한 값이 우선한다.
    bool PatchAnimFollowEnabled()
    {
        // 옵션은 최초 호출에서 읽고 이후 변하지 않는다.
        static const bool enabled = []
        {
            // 클립 전환 재현만 절단 시점 고정을 기본으로 사용한다.
            std::wstring next;
            uint32_t value = CommandLineArgs::GetString(L"skeletal-break-demo-next", next) ? 0u : 1u;
            CommandLineArgs::GetInteger(L"patch-anim-follow", value);
            return value != 0;
        }();
        return enabled;
    }

    // 길이 비율 재분배 옵션을 실행 중 한 번만 읽는다. 기본값은 0이며 0..2로 자른다.
    int PatchLengthWeightsMode()
    {
        // 옵션은 최초 호출에서 읽고 이후 변하지 않는다.
        static const int mode = []
        {
            uint32_t value = 0;
            CommandLineArgs::GetInteger(L"patch-length-weights", value);
            return int(std::min(value, 2u));
        }();
        return mode;
    }

    // 길이 혼합 옵션을 한 번 읽고 가중치 재분배와 함께 켜면 오류로 끝낸다.
    bool PatchLengthBlendEnabled()
    {
        // 기본값은 0이며 기존 옵션처럼 정수 값을 읽는다.
        static const bool enabled = []
        {
            // 혼합과 재분배를 동시에 적용하지 않도록 시작 시 검증한다.
            uint32_t value = 0;
            CommandLineArgs::GetInteger(L"patch-length-blend", value);
            if (value > 1)
                throw std::invalid_argument("patch-length-blend must be 0 or 1");
            if (value && PatchLengthWeightsMode())
                throw std::invalid_argument("patch-length-blend 1 cannot be combined with nonzero patch-length-weights");
            return value != 0;
        }();
        return enabled;
    }

    // 잘린 본 P_i별 남은 길이 비율 r_P를 모드와 무관하게 구해 팔레트 joint마다 채운다.
    RemainLengthTable BuildRemainLengthTable(const std::vector<RemainLengthCut>& cuts,
        const std::vector<Math::Vector3>& positions, const std::vector<uint32_t>& jointNodes,
        const std::vector<uint32_t>& nodeGroups)
    {
        // 모든 항목을 비율 1과 소속 없음으로 시작한다.
        RemainLengthTable table;
        table.Ratio.assign(jointNodes.size(), 1.0f);
        table.CutRoot.assign(jointNodes.size(), -1);
        table.Descendant.assign(jointNodes.size(), 0);
        table.Fallback.assign(jointNodes.size(), -1);

        // P_i마다 |I - parent| / |child - parent|를 [0, 1]로 잘라 r_P를 구한다(선분이 없으면 1).
        std::vector<float> remain(cuts.size(), 1.0f);
        for (size_t i = 0; i < cuts.size(); ++i)
        {
            const uint32_t parent = cuts[i].NodeId, child = cuts[i].ChildNodeId;
            if (parent >= positions.size() || child >= positions.size())
                continue;
            const float length = float(Math::Length(positions[child] - positions[parent]));
            if (length <= 1e-9f)
                continue;
            const float partial = float(Math::Length(cuts[i].Intersection - positions[parent]));
            remain[i] = std::min(std::max(partial / length, 0.0f), 1.0f);
        }

        // 노드 소속으로 자손(비율 0)과 P_i 자신(비율 r_P)을 정한다.
        for (size_t k = 0; k < jointNodes.size(); ++k)
        {
            const uint32_t node = jointNodes[k];
            if (node >= nodeGroups.size() || nodeGroups[node] == 0 || nodeGroups[node] > cuts.size())
                continue;
            const size_t cut = nodeGroups[node] - 1;
            table.CutRoot[k] = int(cut);
            table.Descendant[k] = node == cuts[cut].NodeId ? 0 : 1;
            table.Ratio[k] = table.Descendant[k] ? 0.0f : remain[cut];
        }
        return table;
    }

    // LB 식을 성분별로 재계산하고 절단 순간과 이후 프레임의 수치 상태를 기록한다.
    std::string CheckLengthBlendPalette(const PatchSkinSnapshot& snapshot, const std::vector<Joint>& palette,
        const Math::Matrix4* nodes, const Joint* animJoints, const Math::Matrix4* animNodes,
        const char* side, int frame)
    {
        // 렌더와 같은 추종 조건이 성립한 LB 조각만 검사한다.
        const size_t count = snapshot.JointsAtCut.size();
        if (!PatchLengthBlendEnabled() || !PatchAnimFollowEnabled() || !snapshot.AnimFollow
            || !nodes || !animJoints || !animNodes || snapshot.AnimOffset.size() != count
            || snapshot.LengthBlendRatio.size() != count || palette.size() != count + 1)
            return {};
        const Math::Matrix4 correction = nodes[snapshot.AttachNode] * Math::Invert(animNodes[snapshot.AttachNode]);
        const Math::Matrix4 delta = nodes[snapshot.AttachNode] * Math::Invert(snapshot.AttachAtCut);
        float error = 0, cutError = 0, minDet = INFINITY, minDetJ = 0, minDetF = 0;
        size_t minDetJoint = 0;
        uint32_t zero = 0, one = 0, middle = 0, compared = 0, badNormalDir = 0;

        // 정점 influence 입력이 없으므로 축 3개와 단위 대각선 4개의 법선 방향을 검사한다.
        const Math::Vector3 directions[] = {
            Math::Vector3(1, 0, 0), Math::Vector3(0, 1, 0), Math::Vector3(0, 0, 1),
            Math::Vector3(1, 1, 1), Math::Vector3(1, 1, -1),
            Math::Vector3(1, -1, 1), Math::Vector3(-1, 1, 1)
        };

        // 위치 16성분과 법선 9성분을 독립 가중합 기대값과 비교한다.
        for (size_t k = 0; k < count; ++k)
        {
            const float ratio = snapshot.LengthBlendRatio[k];
            zero += ratio == 0;
            one += ratio == 1;
            middle += ratio != 0 && ratio != 1;
            if (!snapshot.Frozen[k]) continue;
            ++compared;
            const Math::Matrix4 joint = correction * animJoints[k].posXform * snapshot.AnimOffset[k];
            const Math::Matrix4 frozen = delta * snapshot.JointsAtCut[k].posXform;
            const Math::Matrix4 actual[] = { palette[k].posXform, Math::Matrix4(palette[k].nrmXform) };
            const Math::Matrix4 j[] = { joint, Math::Matrix4(Math::InverseTranspose(joint.Get3x3())) };
            const Math::Matrix4 f[] = { frozen, Math::Matrix4(Math::InverseTranspose(frozen.Get3x3())) };
            const Math::Matrix4 cut[] = { snapshot.JointsAtCut[k].posXform,
                Math::Matrix4(Math::InverseTranspose(snapshot.JointsAtCut[k].posXform.Get3x3())) };
            for (int kind = 0; kind < 2; ++kind)
            {
                // 절단 시점 동일성은 위치와 법선의 원래 팔레트를 모두 비교한다.
                DirectX::XMFLOAT4X4 a, b, c, original;
                DirectX::XMStoreFloat4x4(&a, actual[kind]);
                DirectX::XMStoreFloat4x4(&b, j[kind]);
                DirectX::XMStoreFloat4x4(&c, f[kind]);
                DirectX::XMStoreFloat4x4(&original, cut[kind]);
                const int size = kind ? 3 : 4;
                for (int row = 0; row < size; ++row)
                    for (int col = 0; col < size; ++col)
                    {
                        const float value = a.m[row][col];
                        const float diff = std::abs(value - (ratio * b.m[row][col] + (1 - ratio) * c.m[row][col]));
                        error = std::isfinite(diff) ? std::max(error, diff) : INFINITY;
                        const float cutDiff = std::abs(value - original.m[row][col]);
                        cutError = std::isfinite(cutDiff) ? std::max(cutError, cutDiff) : INFINITY;
                    }
            }

            // 혼합 법선 행렬에 단위 방향을 적용해 비유한 결과와 길이 붕괴를 센다.
            for (const Math::Vector3& direction : directions)
            {
                const auto result = DirectX::XMVector3TransformNormal(
                    DirectX::XMVector3Normalize(direction), actual[1]);
                DirectX::XMFLOAT3 normal;
                DirectX::XMStoreFloat3(&normal, result);
                const float length = DirectX::XMVectorGetX(DirectX::XMVector3Length(result));
                badNormalDir += !std::isfinite(normal.x) || !std::isfinite(normal.y)
                    || !std::isfinite(normal.z) || !std::isfinite(length) || length < 1e-4f;
            }

            // 최소 혼합 행렬식의 joint와 원래 J/F 행렬식을 함께 기록한다.
            const float det = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(palette[k].posXform));
            const float candidate = std::isfinite(det) ? det : -INFINITY;
            if (compared == 1 || candidate < minDet)
            {
                minDet = candidate;
                minDetJoint = k;
                minDetJ = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(joint));
                minDetF = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(frozen));
            }
        }

        // cut 검사는 요청 시점에만 판정하고 이후에는 절단 pose와의 차이를 수치로 남긴다.
        char line[768], determinant[192];
        const bool checked = compared != 0;
        if (checked)
            sprintf_s(determinant, "minDet=%g minDetJoint=%zu detJ=%g detF=%g", minDet, minDetJoint, minDetJ, minDetF);
        else
            sprintf_s(determinant, "minDet=N/A minDetJoint=N/A detJ=N/A detF=N/A");
        sprintf_s(line, "LB2 side=%s frame=%d r0=%u r1=%u rMid=%u compared=%u maxAbs=%g formula=%s cutAbs=%g cutIdentity=%s %s badNormalDir=%u\n",
            side, frame, zero, one, middle, compared, error, !checked ? "N/A" : error <= 1e-5f ? "PASS" : "FAIL",
            cutError, frame != -1 || !checked ? "N/A" : cutError <= 1e-5f ? "PASS" : "FAIL", determinant, badNormalDir);
        return line;
    }

    // 고정 본의 따르기·고정 팔레트를 선택하고 LB에서는 위치와 법선을 각각 혼합한다.
    void BuildPatchPalette(const PatchSkinSnapshot& snapshot, const Joint* currentJoints,
        const Math::Matrix4* currentNodes, std::vector<Joint>& out,
        const Joint* animJoints, const Math::Matrix4* animNodes, bool animFollow)
    {
        // 전체 owner 팔레트를 유지하고 부착 노드의 모델 공간 변화량을 구한다.
        const size_t count = snapshot.JointsAtCut.size();
        out.assign(currentJoints, currentJoints + count);
        const Math::Matrix4 delta = currentNodes[snapshot.AttachNode] * Math::Invert(snapshot.AttachAtCut);

        // 애니메이션 추종 조건이 모두 충족될 때만 C(t) = G_att(t)·inv(G_att_anim(t))를 만든다.
        const bool follow = animFollow && snapshot.AnimFollow && animJoints && animNodes
            && snapshot.AnimOffset.size() == count;
        Math::Matrix4 correction(Math::kIdentity);
        if (follow)
            correction = currentNodes[snapshot.AttachNode] * Math::Invert(animNodes[snapshot.AttachNode]);

        // 기존 따르기 조건과 비율 배열이 유효할 때만 길이 혼합을 적용한다.
        const bool blend = follow && PatchLengthBlendEnabled() && snapshot.LengthBlendRatio.size() == count;

        // 고정 본만 치환하며 혼합 전 두 행렬의 역전치를 따로 구한다.
        for (size_t k = 0; k < count; ++k)
        {
            if (!snapshot.Frozen[k]) continue;
            out[k].posXform = follow ? correction * animJoints[k].posXform * snapshot.AnimOffset[k]
                : delta * snapshot.JointsAtCut[k].posXform;
            out[k].nrmXform = Math::InverseTranspose(out[k].posXform.Get3x3());
            if (blend)
            {
                // 고정 팔레트 F와 법선용 역전치 IT(F)를 준비한다.
                const float ratio = snapshot.LengthBlendRatio[k];
                const Math::Matrix4 frozen = delta * snapshot.JointsAtCut[k].posXform;
                const Math::Matrix3 normal = Math::InverseTranspose(frozen.Get3x3());

                // 위치 팔레트의 J와 F를 비율에 따라 혼합한다.
                const DirectX::XMMATRIX joint = out[k].posXform;
                const DirectX::XMMATRIX fixed = frozen;
                DirectX::XMMATRIX mixed;
                for (int row = 0; row < 4; ++row)
                    mixed.r[row] = DirectX::XMVectorAdd(DirectX::XMVectorScale(joint.r[row], ratio),
                        DirectX::XMVectorScale(fixed.r[row], 1.0f - ratio));
                out[k].posXform = Math::Matrix4(mixed);

                // 법선 팔레트의 IT(J)와 IT(F)를 같은 비율로 혼합한다.
                out[k].nrmXform = Math::Matrix3(
                    out[k].nrmXform.GetX() * ratio + normal.GetX() * (1.0f - ratio),
                    out[k].nrmXform.GetY() * ratio + normal.GetY() * (1.0f - ratio),
                    out[k].nrmXform.GetZ() * ratio + normal.GetZ() * (1.0f - ratio));
            }
        }

        // 새 경계와 cap 정점이 사용할 단일 강체 슬롯을 마지막에 추가한다.
        Joint rigid;
        rigid.posXform = delta;
        rigid.nrmXform = Math::InverseTranspose(delta.Get3x3());
        out.push_back(rigid);
    }
}
