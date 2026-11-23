#include "PatchSkinning.h"
#include "GeometryRevision.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "ReadbackBuffer.h"
#include <cstring>
#include "CompiledShaders/PatchSkinBuildCS.h"
#include "CompiledShaders/PatchSkinRecutCS.h"
#include <chrono>
#include <algorithm>

namespace Viewer
{
    // 패치 출처와 원본 draw를 묶어 GPU에서 bind 속성을 복원한다.
    bool BuildPatchSkinVertices(const GeometryRevision& source, ByteAddressBuffer& triangleSources,
        const GeometryBuffers& piece, const PatchSkinSnapshot& snapshot, PatchSkinVertices& out,
        const std::vector<std::array<uint32_t, 2>>& triangleBodies)
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
            uint32_t DrawCount, RigidJoint, PatchTriangles, Padding;
        };
        static_assert(sizeof(DrawInput) == 40 && sizeof(Parameters) == 96, "Patch skin shader layout");
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
            root.Reset(7);
            root[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 5; ++i)
                root[i + 1].InitAsBufferSRV(i);
            root[6].InitAsBufferUAV(0);
            root.Finalize(L"Patch skin build");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pPatchSkinBuildCS, sizeof(g_pPatchSkinBuildCS));
            pso.Finalize();
            initialized = true;
        }

        // section flags의 합집합을 단일 출력 레이아웃으로 보관한다.
        StructuredBuffer descriptors;
        descriptors.Create(L"Patch skin draws", uint32_t(draws.size()), sizeof(DrawInput), draws.data());
        out.Data.Create(L"Patch skin vertices", end, stride);
        out.Flags = flags;
        out.Stride = stride;
        auto& graphics = GraphicsContext::Begin(L"Build patch skin vertices");
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
        compute.TransitionResource(out.Data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetBufferUAV(6, out.Data);
        Parameters c{ Math::Invert(snapshot.MeshNodeAtCut), 0, 0, flags, stride,
            drawCount, uint32_t(snapshot.JointsAtCut.size()), uint32_t(triangleBodies.size()), 0 };
        for (const auto& mesh : piece.Meshes)
        {
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

        // 원본·조각 Data는 이후 VB/IB로도 묶이므로 다른 절단 작업처럼 GENERIC_READ로 되돌린다.
        for (uint32_t i = 0; i < 5; ++i)
            graphics.TransitionResource(*inputs[i], D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(out.Data, D3D12_RESOURCE_STATE_GENERIC_READ);

        // 호출자가 보관한 원본·출처·조각과 임시 descriptor는 완료 뒤에만 해제할 수 있다.
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
        readback.Unmap();
        return true;
    }

    // 부모 skin의 복사 정점 레코드는 통째로 잇고 새 절단면 정점은 K 슬롯 레코드로 자식 skin VB를 만든다.
    bool BuildRecutPatchSkinVertices(const PatchSkinPublication& parent, const GeometryBuffers& cutResult,
        const Math::Matrix4& kFrame, PatchSkinPublication& out, RecutSkinStats* stats)
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
            uint32_t RigidJoint, ParentSectionCount, Padding0, Padding1;
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
        Parameters c{ Math::Invert(kFrame), 0, 0, flags, stride, k, uint32_t(parent.Sections.size()), 0, 0 };
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
        RecutSkinStats local;
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
                if (copied)
                {
                    ++local.Copied;
                    if (origin[2] >= parent.Sections[origin[0]].VertexCount) ++local.BadParent;
                }
                else
                {
                    ++local.K;
                    if (joints != k) ++local.BadK;
                }
            }
            const auto center = (minimum + maximum) * 0.5f;
            const float radius = std::max(float(Math::Length(maximum - minimum)) * 0.5f, 1.0e-6f);
            out.Vertices.BindBounds.emplace_back(center, Math::Scalar(radius));
        }
        skinReadback.Unmap();
        provenanceReadback.Unmap();
        local.ReadbackMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count();
        if (stats)
            *stats = local;
        return true;
    }

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
