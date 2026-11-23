#include "PatchSkinning.h"
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
            uint32_t DrawCount, RigidJoint, PatchTriangles, Follow;
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
            drawCount, uint32_t(snapshot.JointsAtCut.size()), uint32_t(triangleBodies.size()),
            PatchAnimFollowEnabled() && snapshot.AnimFollow ? 1u : 0u };
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

    // 애니메이션 추종 옵션을 실행 중 한 번만 읽는다. 기본값은 0이다.
    bool PatchAnimFollowEnabled()
    {
        // 옵션은 최초 호출에서 읽고 이후 변하지 않는다.
        static const bool enabled = []
        {
            uint32_t value = 0;
            CommandLineArgs::GetInteger(L"patch-anim-follow", value);
            return value != 0;
        }();
        return enabled;
    }

    // 절단 시점의 고정 본을 현재 부착 프레임으로 옮기거나(기본) 애니메이션 joint를 따르게 하며 normal 행렬도 다시 계산한다.
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

        // 고정 본만 치환하고 최종 위치 행렬의 역전치로 법선을 만든다.
        for (size_t k = 0; k < count; ++k)
        {
            if (!snapshot.Frozen[k]) continue;
            out[k].posXform = follow ? correction * animJoints[k].posXform * snapshot.AnimOffset[k]
                : delta * snapshot.JointsAtCut[k].posXform;
            out[k].nrmXform = Math::InverseTranspose(out[k].posXform.Get3x3());
        }

        // 새 경계와 cap 정점이 사용할 단일 강체 슬롯을 마지막에 추가한다.
        Joint rigid;
        rigid.posXform = delta;
        rigid.nrmXform = Math::InverseTranspose(delta.Get3x3());
        out.push_back(rigid);
    }
}
