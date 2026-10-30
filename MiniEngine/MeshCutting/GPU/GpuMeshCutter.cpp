#include "GpuMeshCutter.h"
#include "GraphicsCore.h"
#include "CommandListManager.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Renderer.h"
#include "ConstantBuffers.h"
#include "DescriptorHeap.h"
#include "Texture.h"
#include "GraphicsCommon.h"
#include "CompiledShaders/ClipTrianglesCS.h"
#include "CompiledShaders/BoundaryKeysCS.h"
#include "CompiledShaders/LinkBoundaryCS.h"
#include "CompiledShaders/RankBoundaryCS.h"
#include "CompiledShaders/CapEarsCS.h"
#include "CompiledShaders/FinalizeCutCS.h"
#include "CompiledShaders/CutBoundsCS.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace Math;
namespace MeshCutting
{
    namespace
    {
        RootSignature Root;
        ComputePSO Pipelines[7];
        uint16_t CapTextureTable;
        // 공유 root로 clip·경계·cap 각 단계를 초기화한다.
        void Initialize()
        {
            static bool initialized = false;
            if (initialized) return;
            Root.Reset(13);
            Root[0].InitAsConstantBuffer(0);
            Root[1].InitAsConstants(1, 1);
            for (uint32_t i = 0; i < 3; ++i) Root[i + 2].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 8; ++i) Root[i + 5].InitAsBufferUAV(i);
            Root.Finalize(L"Static cut");
            const void* shaders[] = { g_pClipTrianglesCS, g_pBoundaryKeysCS, g_pLinkBoundaryCS,
                g_pRankBoundaryCS, g_pCapEarsCS, g_pFinalizeCutCS, g_pCutBoundsCS };
            const size_t sizes[] = { sizeof(g_pClipTrianglesCS), sizeof(g_pBoundaryKeysCS), sizeof(g_pLinkBoundaryCS),
                sizeof(g_pRankBoundaryCS), sizeof(g_pCapEarsCS), sizeof(g_pFinalizeCutCS), sizeof(g_pCutBoundsCS) };
            for (uint32_t i = 0; i < 7; ++i)
            {
                Pipelines[i].SetRootSignature(Root);
                Pipelines[i].SetComputeShader(shaders[i], sizes[i]);
                Pipelines[i].Finalize();
            }
            const auto handles = Renderer::s_TextureHeap.Alloc(kNumTextures);
            const auto offset = Renderer::s_TextureHeap.GetOffsetOfHandle(handles);
            if (offset > 65535) throw std::length_error("Cap texture descriptor offset overflow");
            CapTextureTable = uint16_t(offset);
            D3D12_CPU_DESCRIPTOR_HANDLE textures[kNumTextures] = {
                Graphics::GetDefaultTexture(Graphics::kWhiteOpaque2D),Graphics::GetDefaultTexture(Graphics::kWhiteOpaque2D),
                Graphics::GetDefaultTexture(Graphics::kWhiteOpaque2D),Graphics::GetDefaultTexture(Graphics::kBlackTransparent2D),
                Graphics::GetDefaultTexture(Graphics::kDefaultNormalMap)};
            uint32_t count = kNumTextures, counts[kNumTextures] = {1,1,1,1,1};
            Graphics::g_Device->CopyDescriptors(1,&handles,&count,kNumTextures,textures,counts,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            initialized = true;
        }
        // GPU ordered-float 경계를 다시 실수로 복원한다.
        float Unorder(uint32_t value)
        {
            const uint32_t bits = value & 0x80000000 ? value ^ 0x80000000 : ~value;
            float result; std::memcpy(&result, &bits, 4); return result;
        }
    }

    // 계산 파이프라인과 cap 재질 descriptor를 게임플레이 전에 준비한다.
    void GpuMeshCutter::Prepare()
    {
        Initialize();
    }

    // 큰 입력의 동일 clip 판정으로 출력 개수만 읽고 실제 버퍼 할당은 예산 검사 뒤로 미룬다.
    CutCapacityPlan GpuMeshCutter::PlanCapacity(const GeometryRevision& input, const PosedGeometry& posed,
        const AffineTransform& world, const CutPlaneData& plane, float epsilon, bool force)
    {
        // 먼저 기존 draw 검사를 수행하고 작은 입력은 기존 최악 용량 경로를 유지한다.
        EstimateWorkingBytes(input, posed, false);
        uint64_t triangles = 0;
        for (size_t i = 0; i < input.GetSections().size(); ++i)
            for (uint32_t d = 0; d < input.GetMesh(i).numDraws; ++d)
                triangles += input.GetMesh(i).draw[d].primCount / 3;
        if (!force && triangles * 9 <= 1000000) return {};
        if (!posed.IsReady() || !std::isfinite(epsilon) || epsilon <= 0)
            throw std::invalid_argument("Cut count requires prepared geometry");

        // count descriptor는 최대 4096 draw로 제한하고 작은 upload page만 사용한다.
        std::vector<CutSection> sections;
        uint32_t first = 0;
        for (uint32_t i = 0; i < input.GetSections().size(); ++i)
        {
            const Mesh& mesh = input.GetMesh(i);
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                if (sections.size() == 4096)
                    throw std::length_error("Cut count draw limit exceeded (4096)");
                const auto& draw = mesh.draw[d];
                sections.push_back({ mesh.vbOffset, mesh.vbStride, mesh.ibOffset, mesh.ibFormat == DXGI_FORMAT_R32_UINT,
                    draw.startIndex, draw.baseVertex, draw.primCount / 3, posed.GetOffset(input.GetSections()[i].SourceMesh),
                    first, 0, draw.primCount * 2, mesh.psoFlags, i, d, mesh.vbSize / mesh.vbStride, 0 });
                first += draw.primCount / 3;
            }
        }
        sections.push_back({});

        // 기존 clip shader의 출력 용량 0 모드로 표면·경계 카운터만 갱신한다.
        Initialize();
        CutConstants constants{};
        constants.Plane = Transpose(Matrix4(world)) * plane.WorldPlane;
        constants.LocalToRectangle = plane.WorldToPlane * Matrix4(world);
        const Vector3 normal(plane.WorldPlane);
        const Vector3 u = Normalize(Cross(normal, Abs(normal.GetY()) < 0.9f ? Vector3(kYUnitVector) : Vector3(kXUnitVector)));
        constants.ProjectU = Transpose(Matrix4(world)) * Vector4(u, 0);
        constants.ProjectV = Transpose(Matrix4(world)) * Vector4(Cross(normal, u), 0);
        constants.Epsilon = epsilon;
        constants.Triangles = first;
        constants.Sections = uint32_t(sections.size());
        std::vector<uint32_t> initial(20 + sections.size() * 2, 0);
        ByteAddressBuffer metadata;
        StructuredBuffer descriptors;
        ReadbackBuffer readback;
        metadata.Create(L"Cut preflight counters", uint32_t(initial.size()), 4, initial.data());
        descriptors.Create(L"Cut preflight sections", uint32_t(sections.size()), sizeof(CutSection), sections.data());
        readback.Create(L"Cut preflight readback", uint32_t(initial.size()), 4);
        GraphicsContext& graphics = GraphicsContext::Begin(L"Cut capacity count");
        auto& compute = graphics.GetComputeContext();
        compute.SetRootSignature(Root);
        compute.SetPipelineState(Pipelines[0]);
        compute.SetConstant(1, 0, 0);
        compute.SetDynamicConstantBufferView(0, sizeof(constants), &constants);
        compute.SetBufferSRV(2, input.GetDataBuffer());
        compute.SetBufferSRV(3, posed.GetBuffer());
        compute.SetBufferSRV(4, descriptors);
        compute.TransitionResource(metadata, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (uint32_t slot = 5; slot < 13; ++slot) compute.SetBufferUAV(slot, metadata);
        compute.Dispatch1D(first, 64);
        graphics.CopyBuffer(readback, metadata);
        graphics.Finish(true);

        // CPU에는 작은 개수 배열만 읽고 오류나 불가능한 개수면 원본을 유지한다.
        std::memcpy(initial.data(), readback.Map(), initial.size() * 4);
        readback.Unmap();
        if (initial[0] || initial[1] > first)
            throw std::runtime_error("Cut capacity count failed");
        CutCapacityPlan plan;
        plan.MaxSegments = std::max(1u, initial[1]);
        for (size_t i = 0; i + 1 < sections.size(); ++i)
        {
            const uint32_t count = std::max(initial[20 + i], initial[20 + sections.size() + i]);
            if (count > sections[i].Triangles * 6 || count % 3)
                throw std::runtime_error("Cut capacity count exceeds triangle bound");
            plan.SurfaceVertices.push_back(count);
        }
        return plan;
    }

    // 지원 용량 계약과 Submit의 draw 해석에 맞춰 임시 자원 예약치를 계산한다.
    // 원본 geometry·부모 lineage·posed cache는 별도 소유이므로 포함하지 않는다.
    uint64_t GpuMeshCutter::EstimateWorkingBytes(const GeometryRevision& input, const PosedGeometry& posed,
        bool snapshotDiagnostics, const CutCapacityPlan* capacity)
    {
        // CPU draw 정보로 작업량과 출력 정점 용량을 합산한다.
        uint64_t triangles = 0;
        uint64_t surfaceVertices = 0;
        size_t capacityIndex = 0;
        uint64_t sectionCount = 1; // 양쪽이 공유하는 cap section이다.
        for (size_t i = 0; i < input.GetSections().size(); ++i)
        {
            const Mesh& mesh = input.GetMesh(i);
            if (mesh.psoFlags & PSOFlags::kHasSkin)
                throw std::invalid_argument("Skeletal cutting is not enabled");
            if ((mesh.psoFlags & 3) != 3 || mesh.vbStride == 0 ||
                (mesh.ibFormat != DXGI_FORMAT_R16_UINT && mesh.ibFormat != DXGI_FORMAT_R32_UINT) || mesh.ibOffset % 4 != 0)
                throw std::invalid_argument("Unsupported static index layout");

            // 각 draw의 index 범위와 정점 용량을 Submit과 같은 규칙으로 검사한다.
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const Mesh::Draw& draw = mesh.draw[d];
                const uint32_t indexStride = mesh.ibFormat == DXGI_FORMAT_R32_UINT ? 4 : 2;
                if (draw.primCount % 3 || (uint64_t(draw.startIndex) + draw.primCount) * indexStride > mesh.ibSize ||
                    uint64_t(mesh.ibOffset) + mesh.ibSize > input.GetDataBuffer().GetBufferSize() ||
                    draw.baseVertex > mesh.vbSize / mesh.vbStride)
                    throw std::invalid_argument("Invalid static draw range");
                const uint64_t drawTriangles = draw.primCount / 3;
                if (drawTriangles > 1000000 - triangles)
                    throw std::length_error("Cut input triangle limit exceeded (1000000)");
                triangles += drawTriangles;
                surfaceVertices += capacity && !capacity->SurfaceVertices.empty() ? capacity->SurfaceVertices.at(capacityIndex++) : drawTriangles * 6;
                ++sectionCount;
            }
        }

        // 입력 삼각형과 GPU byte offset 한계를 검사해 잘못된 예약을 거절한다.
        if (triangles == 0)
            throw std::length_error("Cut input has no triangles");
        if (triangles > 1000000)
            throw std::length_error("Cut input triangle limit exceeded (1000000)");
        const uint64_t segments = capacity && !capacity->SurfaceVertices.empty() ? capacity->MaxSegments : triangles;
        if (capacity && !capacity->SurfaceVertices.empty() &&
            (capacityIndex != capacity->SurfaceVertices.size() || segments == 0 || segments > triangles))
            throw std::length_error("Invalid cut capacity plan");
        const uint64_t vertices = surfaceVertices + segments * 3;
        if (vertices > std::numeric_limits<uint32_t>::max() / 48)
            throw std::length_error("Cut output byte offsets exceed uint32 range");
        uint64_t buckets = 1;
        while (buckets < segments * 4) buckets *= 2;

        // 개별 D3D12 buffer마다 64 KiB 정렬 여유를 더한다.
        constexpr uint64_t AllocationGranularity = 64 * 1024;
        const auto allocated = [](uint64_t bytes) -> uint64_t
        {
            constexpr uint64_t granularity = 64 * 1024;
            if (bytes > std::numeric_limits<uint64_t>::max() - (granularity - 1))
                throw std::length_error("Static cut estimate overflow");
            return ((bytes + granularity - 1) / granularity) * granularity;
        };
        uint64_t bytes = 0;

        // A/B의 Data·Pose·Provenance 출력 버퍼는 각 양쪽 크기로 따로 정렬한다.
        bytes += allocated(vertices * 48) * 2;
        bytes += allocated(vertices * 40) * 2;
        bytes += allocated(vertices * 32) * 2;
        bytes += allocated(sizeof(MaterialConstants)) * 2;

        // scratch, metadata, descriptor, readback의 논리 크기를 개별 정렬한다.
        const uint64_t scratchBytes = segments * 128 + buckets * 4 + segments * 8;
        const uint64_t metadataBytes = (20 + sectionCount * 2 + 2 * KDopWordsPerSide + 3) * 4;
        bytes += allocated(scratchBytes);
        bytes += allocated(metadataBytes);
        bytes += allocated(sectionCount * sizeof(CutSection));
        bytes += allocated(metadataBytes);
        bytes += allocated(5 * sizeof(uint64_t));

        // 13개 GPU buffer 외 counter·query heap·동적 constant allocator page를 더한다.
        bytes += AllocationGranularity * 3;

        // 네 초기 upload 각각의 CPU allocator 사용을 2 MiB 단위로 올림한다.
        const auto uploadAllocation = [](uint64_t size) -> uint64_t
        {
            constexpr uint64_t pageSize = 2 * 1024 * 1024;
            if (size > std::numeric_limits<uint64_t>::max() - (pageSize - 1))
                throw std::length_error("Static cut upload estimate overflow");
            return ((size + pageSize - 1) / pageSize) * pageSize;
        };
        const uint64_t descriptorBytes = sectionCount * sizeof(CutSection);
        bytes += uploadAllocation(sizeof(MaterialConstants)) * 2;
        bytes += uploadAllocation(metadataBytes);
        bytes += uploadAllocation(descriptorBytes);

        // 명시적 진단은 원본 입력 snapshot과 추가 readback 버퍼를 만들어 그 임시 공간도 예약한다.
        if (snapshotDiagnostics)
        {
            const uint64_t poseSnapshotBytes = posed.GetBuffer().GetBufferSize();
            const uint64_t dataSnapshotBytes = input.GetDataBuffer().GetBufferSize();
            if (poseSnapshotBytes > std::numeric_limits<uint32_t>::max() ||
                dataSnapshotBytes > std::numeric_limits<uint32_t>::max())
                throw std::length_error("Static cut diagnostic snapshot exceeds supported buffer size");
            bytes += allocated(poseSnapshotBytes);
            bytes += allocated(dataSnapshotBytes);

            // ValidateDiagnostics의 boundary·양쪽 surface readback과 각 면 DOP pose readback을 합산한다.
            bytes += allocated(segments * 128);
            bytes += allocated(vertices * 48) * 2;
            bytes += allocated(vertices * 40) * 4;
            bytes += allocated(vertices * 32) * 2;
        }
        return bytes;
    }

    // 제출 입력·출력 resource를 GPU 완료 전 해제하지 않도록 작업 fence를 기다린다.
    CutJob::~CutJob()
    {
        // 제출한 command list가 입력·출력 resource를 모두 사용 완료할 때까지 기다린다.
        if (Fence != 0)
            Graphics::g_CommandManager.WaitForFence(Fence);

        // revision이나 renderer에 전달되지 않은 단독 소유 출력만 완료 후 직접 해제한다.
        if (Fence != 0)
        {
            for (const auto& output : Output)
                if (output && output.use_count() == 1)
                    output->ReleaseGpuResourcesAfterCompletion();
        }
    }

    // fence가 끝난 뒤 cap 누적 진단을 포함한 metadata와 GPU timestamp만 읽는다.
    bool CutJob::Poll()
    {
        if (Collected) return true;
        if (!Graphics::g_CommandManager.IsFenceComplete(Fence)) return false;

        // 기존 KDop 뒤에 추가한 세 DWORD까지 함께 읽는다.
        Status.resize(80 / 4 + Constants.Sections * 2 + 2 * KDopWordsPerSide + 3);
        std::memcpy(Status.data(), StatusReadback.Map(), Status.size() * 4);
        StatusReadback.Unmap();

        // section 수에 따른 진단 시작 위치에서 라운드·표시·실제 제거 합계를 해석한다.
        const uint32_t capBase = 58 + Constants.Sections * 2;
        CapRounds = Status[capBase];
        CapMarked = Status[capBase + 1];
        CapRemoved = Status[capBase + 2];

        // 기존 면별 18-DOP을 해석하며 기여 정점이 없는 면은 sentinel을 해석하지 않고 비운다.
        for (uint32_t side = 0; side < 2; ++side)
        {
            const uint32_t base = 80 / 4 + Constants.Sections * 2 + side * KDopWordsPerSide;
            if (Status[base + 18] == 0) continue;

            KDop18 dop;
            for (uint32_t axis = 0; axis < 9; ++axis)
            {
                dop.Min[axis] = Unorder(Status[base + axis]);
                dop.Max[axis] = Unorder(Status[base + 9 + axis]);
            }
            dop.VertexCount = Status[base + 18];
            SideDop[side] = dop;
            SideDopValid[side] = true;
        }
        const uint64_t* times = static_cast<const uint64_t*>(TimeReadback.Map());
        uint64_t frequency = 0;
        Graphics::g_CommandManager.GetCommandQueue()->GetTimestampFrequency(&frequency);
        GpuMilliseconds = double(times[4] - times[0]) * 1000.0 / double(frequency);
        for (uint32_t i = 0; i < 4; ++i) StageMs[i] = double(times[i+1]-times[i])*1000.0/double(frequency);
        TimeReadback.Unmap();
        ReadyMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
        Collected = true;
        return true;
    }

    // 실제 양쪽 표면이 있는 분할만 게시하며 원본 section과 cap을 각각 보존한다.
    std::array<std::shared_ptr<const GeometryRevision>, 2> CutJob::BuildResults()
    {
        if (!HasSplit()) return {};
        for (uint32_t side = 0; side < 2; ++side)
        {
            uint64_t surfaceCorners = 0;
            for (uint32_t i = 0; i + 1 < Constants.Sections; ++i)
                surfaceCorners += Status[20 + side * Constants.Sections + i];
            if (surfaceCorners == 0) return {};
        }
        std::array<std::shared_ptr<const GeometryRevision>, 2> result;
        for (uint32_t side = 0; side < 2; ++side)
        {
            const Vector3 minimum(Unorder(Status[8 + side * 6]), Unorder(Status[9 + side * 6]), Unorder(Status[10 + side * 6]));
            const Vector3 maximum(Unorder(Status[11 + side * 6]), Unorder(Status[12 + side * 6]), Unorder(Status[13 + side * 6]));
            const Vector3 center = (minimum + maximum) * 0.5f;
            const float radius = Length(maximum - center);
            auto& out = *Output[side];
            for (uint32_t i = 0; i < Constants.Sections; ++i)
            {
                const uint32_t count = Status[20 + side * Constants.Sections + i];
                if (!count) continue;
                const CutSection& s = Sections[i];
                Mesh mesh = Templates[i];
                mesh.meshCBV = 0; mesh.numJoints = mesh.startJoint = 0; mesh.numDraws = 1;
                mesh.psoFlags = uint16_t((mesh.psoFlags & ~PSOFlags::kHasSkin) | PSOFlags::kHasUV0);
                mesh.vbStride = 28; mesh.ibFormat = DXGI_FORMAT_R32_UINT;
                mesh.vbOffset = s.VertexStart * 28; mesh.vbSize = count * 28;
                mesh.vbDepthOffset = Constants.DepthOffset + s.VertexStart * 16;
                mesh.vbDepthSize = count * ((mesh.psoFlags & PSOFlags::kAlphaTest) ? 16 : 12);
                mesh.ibOffset = Constants.IndexOffset + s.VertexStart * 4; mesh.ibSize = count * 4;
                mesh.draw[0] = { count, 0, 0 };
                mesh.pso = Renderer::GetPSO(mesh.psoFlags);
                XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(mesh.bounds), Vector4(center, radius));
                out.Meshes.push_back(mesh);
                out.PoseOffsets.push_back(s.VertexStart * 40);
            }
            if (out.Meshes.empty()) return {};
            out.UncappedSegments = Status[2];
            result[side] = GeometryRevision::FromGenerated(Output[side]);
        }
        return result;
    }

    // GPU 절단을 제출하고 명시적 진단에서만 같은 시점의 부모 정점·인덱스를 보존한다.
    std::unique_ptr<CutJob> GpuMeshCutter::Submit(std::shared_ptr<const GeometryRevision> input,
        const PosedGeometry& posed, const AffineTransform& world, const CutPlaneData& plane, float epsilon, uint32_t capMode, const std::vector<BoundingSphere>& bounds, bool snapshotDiagnostics, const CutCapacityPlan* capacity, uint64_t retainedLimitBytes)
    {
        if (!input || !posed.IsReady() || !std::isfinite(epsilon) || epsilon <= 0)
            throw std::invalid_argument("Static cut requires prepared geometry");
        // Submit도 같은 지원 용량 계약을 확인해 scheduler 추정과 작업 생성 조건을 맞춘다.
        const uint64_t workingEstimate = EstimateWorkingBytes(*input, posed, snapshotDiagnostics, capacity);
        (void)workingEstimate;
        // 큰 입력은 작은 count 결과 없이 최악 용량 버퍼를 만들지 않는다.
        if ((!capacity || capacity->SurfaceVertices.empty()) && workingEstimate > 512ull * 1024 * 1024)
            throw std::length_error("Large cut requires a capacity plan");
        Initialize();
        auto job = std::make_unique<CutJob>();
        job->Started = std::chrono::steady_clock::now();
        job->Input = std::move(input);
        uint64_t total = 0, vertices = 0;
        for (uint32_t i = 0; i < job->Input->GetSections().size(); ++i)
        {
            const Mesh& mesh = job->Input->GetMesh(i);
            if (mesh.psoFlags & PSOFlags::kHasSkin) throw std::invalid_argument("Skeletal cutting is not enabled");
            if ((mesh.psoFlags & 3) != 3 || mesh.vbStride == 0 || (mesh.ibFormat != DXGI_FORMAT_R16_UINT && mesh.ibFormat != DXGI_FORMAT_R32_UINT)
                || mesh.ibOffset % 4 != 0)
                throw std::invalid_argument("Unsupported static index layout");
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const Mesh::Draw& draw = mesh.draw[d];
                const uint32_t indexStride = mesh.ibFormat == DXGI_FORMAT_R32_UINT ? 4 : 2;
                if (draw.primCount % 3 || (uint64_t(draw.startIndex) + draw.primCount) * indexStride > mesh.ibSize
                    || uint64_t(mesh.ibOffset) + mesh.ibSize > job->Input->GetDataBuffer().GetBufferSize()
                    || draw.baseVertex > mesh.vbSize / mesh.vbStride)
                    throw std::invalid_argument("Invalid static draw range");
                CutSection s = { mesh.vbOffset, mesh.vbStride, mesh.ibOffset, mesh.ibFormat == DXGI_FORMAT_R32_UINT,
                    draw.startIndex, draw.baseVertex, draw.primCount / 3,
                    posed.GetOffset(job->Input->GetSections()[i].SourceMesh), uint32_t(total), uint32_t(vertices),
                    draw.primCount * 2, mesh.psoFlags, i, d, mesh.vbSize / mesh.vbStride, 0 };
                if (capacity && !capacity->SurfaceVertices.empty())
                    s.VertexCapacity = capacity->SurfaceVertices.at(job->Sections.size());
                total += s.Triangles; vertices += s.VertexCapacity;
                job->Sections.push_back(s); job->Templates.push_back(mesh);
            }
        }
        // 기존 용량 상한은 유지하고 거절된 입력·출력 조건을 구분한다.
        if (total == 0)
            throw std::length_error("Cut input has no triangles");
        if (total > 1000000)
            throw std::length_error("Cut input triangle limit exceeded (1000000)");
        const uint64_t segments = capacity && !capacity->SurfaceVertices.empty() ? capacity->MaxSegments : total;
        // cap도 독립 section이며 양쪽에서 같은 topology를 반대로 사용한다.
        CutSection cap{}; cap.VertexStart = uint32_t(vertices); cap.VertexCapacity = uint32_t(segments * 3); cap.Flags = 31;
        job->Sections.push_back(cap);
        Mesh capTemplate = job->Templates.front(); capTemplate.psoFlags = 31;
        capTemplate.materialCBV = 0xffff; capTemplate.srvTable = CapTextureTable;
        job->Templates.push_back(capTemplate);
        vertices += cap.VertexCapacity;
        const auto parent = job->Input->GetGenerated();
        const uint32_t generation = parent ? parent->Generation + 1 : 1;
        const uint64_t parentBytes = parent ? parent->LineageBytes : 0;
        // 재절단 깊이 제한과 부모·양쪽 출력의 보관 예산을 별도로 진단한다.
        if (generation > 32)
            throw std::length_error("Cut history depth limit exceeded (32)");
        // 기본 512 MiB를 유지하며 명시 작업 예산도 최대 1024 MiB 안에서만 허용한다.
        retainedLimitBytes = std::max(512ull * 1024 * 1024, std::min(retainedLimitBytes, 1024ull * 1024 * 1024));
        if (parentBytes + vertices * 240 > retainedLimitBytes)
            throw std::length_error("Cut retained geometry budget exceeded (limitBytes=" + std::to_string(retainedLimitBytes) + ")");
        auto& c = job->Constants;
        c.Plane = Transpose(Matrix4(world)) * plane.WorldPlane;
        c.LocalToRectangle = plane.WorldToPlane * Matrix4(world);
        // 보수적 section AABB가 한쪽에 있으면 교차 검사 없이 그 section을 통째로 배정한다.
        for (size_t i = 0; i + 1 < job->Sections.size(); ++i)
        {
            auto& section = job->Sections[i];
            if (section.SourceSection >= bounds.size()) continue;
            const auto sphere = bounds[section.SourceSection];
            const float r = sphere.GetRadius();
            const float extent = r * (std::abs(float(c.Plane.GetX())) + std::abs(float(c.Plane.GetY())) + std::abs(float(c.Plane.GetZ())));
            const float distance = Dot(c.Plane, Vector4(sphere.GetCenter(),1));
            section.Reserved = distance > extent + epsilon ? 1u : distance < -extent - epsilon ? 2u : 0u;
        }
        const Vector3 n(plane.WorldPlane);
        const Vector3 u = Normalize(Cross(n, Abs(n.GetY()) < 0.9f ? Vector3(kYUnitVector) : Vector3(kXUnitVector)));
        const Vector3 v = Cross(n, u);
        c.ProjectU = Transpose(Matrix4(world)) * Vector4(u, 0);
        c.ProjectV = Transpose(Matrix4(world)) * Vector4(v, 0);
        c.Epsilon = epsilon; c.HalfWidth = plane.HalfWidth; c.HalfHeight = plane.HalfHeight;
        c.Triangles = uint32_t(total); c.Sections = uint32_t(job->Sections.size()); c.VertexCapacity = uint32_t(vertices);
        c.DepthOffset = uint32_t(vertices * 28); c.IndexOffset = uint32_t(vertices * 44);
        c.MaxSegments = uint32_t(segments); uint32_t buckets = 1;
        while (buckets < segments * 4) buckets *= 2;
        c.HashMask = buckets - 1; c.HashOffset = uint32_t(segments * 128); c.HashLinksOffset = c.HashOffset + buckets * 4;

        // 양쪽 cap이 공유하는 재질 상수를 요청별로 준비한다.
        MaterialConstants material{};
        material.baseColorFactor[0] = 0.8f;
        material.baseColorFactor[1] = 0.22f;
        material.baseColorFactor[2] = 0.12f;
        material.baseColorFactor[3] = 1;
        material.normalTextureScale = 1;
        material.roughnessFactor = 0.8f;
        for (auto& out : job->Output)
        {
            out = std::make_shared<GeometryBuffers>(); out->Source = job->Input->GetSource();
            out->Parent = job->Input;
            out->Generation = generation; out->LineageBytes = parentBytes + vertices * 120;
            out->Data.Create(L"Cut surface/depth/index", uint32_t(vertices * 48), 1);
            out->Pose.Create(L"Canonical cut pose", uint32_t(vertices * 40), 1);
            out->Provenance.Create(L"Cut current-parent provenance", uint32_t(vertices * 32), 1);
            out->CapMaterial.Create(L"Cut cap material",1,sizeof(material));
        }

        // KDop 뒤의 cap 진단 세 DWORD도 0으로 초기화한다.
        std::vector<uint32_t> initial(20 + c.Sections * 2 + 2 * KDopWordsPerSide + 3, 0);
        for (uint32_t side = 0; side < 2; ++side) for (uint32_t k = 0; k < 3; ++k) initial[8 + side * 6 + k] = 0xffffffff;
        // 면별 18-DOP의 최솟값 단어는 최대 sentinel로, 최댓값 단어와 개수는 0으로 시작한다.
        for (uint32_t side = 0; side < 2; ++side)
            for (uint32_t axis = 0; axis < 9; ++axis)
                initial[20 + c.Sections * 2 + side * KDopWordsPerSide + axis] = 0xffffffff;
        job->Metadata.Create(L"Cut counts/status/bounds", uint32_t(initial.size()), 4);
        job->Scratch.Create(L"Cut boundary graph/hash", c.HashLinksOffset + c.MaxSegments * 8, 1);
        job->Descriptors.Create(L"Cut sections", c.Sections, sizeof(CutSection));
        job->StatusReadback.Create(L"Cut metadata readback", uint32_t(initial.size()), 4);
        job->TimeReadback.Create(L"Cut timestamp readback", 5, 8);
        D3D12_QUERY_HEAP_DESC q{}; q.Count = 5; q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        // command list 기록 전에 query와 진단 snapshot을 모두 확보해 할당 실패를 원복한다.
        if (FAILED(Graphics::g_Device->CreateQueryHeap(&q, IID_PPV_ARGS(job->Queries.GetAddressOf()))))
            throw std::runtime_error("Cut timestamp query allocation failed");
        if (snapshotDiagnostics)
        {
            job->DiagnosticInputPose.Create(L"Diagnostic parent pose", uint32_t(posed.GetBuffer().GetBufferSize()), 1);
            job->DiagnosticInputData.Create(L"Diagnostic parent geometry", uint32_t(job->Input->GetDataBuffer().GetBufferSize()), 1);
        }
        GraphicsContext& graphics = GraphicsContext::Begin(L"Static mesh cut");
        auto& compute = graphics.GetComputeContext();

        // GPU 버퍼의 초기 내용을 현재 작업 command list에 비동기 업로드한다.
        const auto uploadInitialData = [&graphics](GpuBuffer& destination, const void* data, size_t bytes)
        {
            // 업로드 메모리를 복사하고 목적지를 읽기 상태로 전이한다.
            DynAlloc upload = graphics.ReserveUploadMemory(bytes);
            std::memcpy(upload.DataPtr, data, bytes);
            graphics.TransitionResource(destination, D3D12_RESOURCE_STATE_COPY_DEST);
            graphics.CopyBufferRegion(destination, 0, upload.Buffer, upload.Offset, bytes);
            graphics.TransitionResource(destination, D3D12_RESOURCE_STATE_GENERIC_READ);
        };

        // cap material과 dispatch metadata를 GPU 실행 전에 같은 queue에 기록한다.
        uploadInitialData(job->Output[0]->CapMaterial, &material, sizeof(material));
        uploadInitialData(job->Output[1]->CapMaterial, &material, sizeof(material));
        uploadInitialData(job->Metadata, initial.data(), initial.size() * sizeof(uint32_t));
        uploadInitialData(job->Descriptors, job->Sections.data(), job->Sections.size() * sizeof(CutSection));
        graphics.InsertTimeStamp(job->Queries.Get(), 0);
        compute.TransitionResource(job->Scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        compute.ClearUAV(job->Scratch);
        compute.InsertUAVBarrier(job->Scratch);
        compute.SetRootSignature(Root); compute.SetConstant(1, 0, 0);
        compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
        compute.SetBufferSRV(2, job->Input->GetDataBuffer()); compute.SetBufferSRV(3, posed.GetBuffer());
        compute.SetBufferSRV(4, job->Descriptors);
        GpuBuffer* buffers[] = { &job->Output[0]->Data, &job->Output[0]->Pose, &job->Output[1]->Data,
            &job->Output[1]->Pose, &job->Metadata, &job->Scratch, &job->Output[0]->Provenance, &job->Output[1]->Provenance };
        for (uint32_t i = 0; i < 8; ++i)
        { compute.TransitionResource(*buffers[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS); compute.SetBufferUAV(i + 5, *buffers[i]); }
        // 단계 사이 UAV barrier는 전역 의존성을 보장하며 그룹 barrier로 대체하지 않는다.
        const auto dispatch = [&](uint32_t pipeline, uint32_t threads)
        {
            compute.SetPipelineState(Pipelines[pipeline]); compute.Dispatch1D(threads, 64);
            compute.InsertUAVBarrier(job->Scratch); compute.InsertUAVBarrier(job->Metadata);
        };
        dispatch(0, c.Triangles);
        graphics.InsertTimeStamp(job->Queries.Get(), 1);
        // 모서리 후보의 양쪽 지지 확인 뒤 같은 해시를 끝점 연결에 재사용한다.
        compute.SetConstant(1, 0, 0); dispatch(1, c.MaxSegments);
        compute.SetConstant(1, 0, 1); dispatch(1, c.MaxSegments);
        compute.SetConstant(1, 0, 2); dispatch(1, c.HashMask + 1);
        compute.SetConstant(1, 0, 3); dispatch(1, c.MaxSegments * 2);
        dispatch(2, c.MaxSegments);
        for (uint32_t reach = 1; reach < c.MaxSegments; reach *= 2)
        {
            compute.SetConstant(1, 0, 0); dispatch(3, c.MaxSegments);
            compute.SetConstant(1, 0, 1); dispatch(3, c.MaxSegments);
        }
        graphics.InsertTimeStamp(job->Queries.Get(), 2);
        compute.SetConstant(1, 0, capMode);
        compute.SetPipelineState(Pipelines[4]);
        // 모든 경계 대표를 처리하도록 큰 입력은 2D dispatch로 펼친다.
        compute.Dispatch(std::min(c.MaxSegments, 65535u), (c.MaxSegments + 65534) / 65535);
        compute.InsertUAVBarrier(job->Scratch); compute.InsertUAVBarrier(job->Metadata);
        graphics.InsertTimeStamp(job->Queries.Get(), 3);
        dispatch(5, c.MaxSegments);
        compute.InsertUAVBarrier(job->Output[0]->Pose);
        compute.InsertUAVBarrier(job->Output[1]->Pose);
        dispatch(6, c.VertexCapacity);
        for (uint32_t i = 0; i < 4; ++i) compute.InsertUAVBarrier(*buffers[i]);
        graphics.InsertTimeStamp(job->Queries.Get(), 4);
        graphics.CopyBuffer(job->StatusReadback, job->Metadata);
        graphics.ResolveTimeStamps(job->TimeReadback.GetResource(), job->Queries.Get(), 5);
        if (snapshotDiagnostics)
        {
            auto& inputPose = const_cast<ByteAddressBuffer&>(posed.GetBuffer());
            auto& inputData = const_cast<ByteAddressBuffer&>(job->Input->GetDataBuffer());
            graphics.CopyBuffer(job->DiagnosticInputPose,inputPose);
            graphics.CopyBuffer(job->DiagnosticInputData,inputData);
            graphics.TransitionResource(inputPose,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            graphics.TransitionResource(inputData,D3D12_RESOURCE_STATE_GENERIC_READ);
            job->HasDiagnosticInput = true;
        }
        for (auto& out : job->Output)
        {
            graphics.TransitionResource(out->Data, D3D12_RESOURCE_STATE_GENERIC_READ);
            graphics.TransitionResource(out->Pose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            graphics.TransitionResource(out->Provenance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        job->Fence = graphics.Finish();
        return job;
    }
}
