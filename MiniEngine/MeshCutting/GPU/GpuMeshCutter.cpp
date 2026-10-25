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

    // 입력과 출력의 strong reference가 먼저 해제되지 않도록 GPU 완료를 보장한다.
    CutJob::~CutJob()
    {
        if (Fence) Graphics::g_CommandManager.WaitForFence(Fence);
    }

    // fence가 끝난 뒤 작은 metadata와 GPU timestamp만 읽는다.
    bool CutJob::Poll()
    {
        if (Collected) return true;
        if (!Graphics::g_CommandManager.IsFenceComplete(Fence)) return false;
        Status.resize(80 / 4 + Constants.Sections * 2);
        std::memcpy(Status.data(), StatusReadback.Map(), Status.size() * 4);
        StatusReadback.Unmap();
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
        const PosedGeometry& posed, const AffineTransform& world, const CutPlaneData& plane, float epsilon, uint32_t capMode, const std::vector<BoundingSphere>& bounds, bool snapshotDiagnostics)
    {
        if (!input || !posed.IsReady() || !std::isfinite(epsilon) || epsilon <= 0)
            throw std::invalid_argument("Static cut requires prepared geometry");
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
                total += s.Triangles; vertices += s.VertexCapacity;
                job->Sections.push_back(s); job->Templates.push_back(mesh);
            }
        }
        if (total == 0 || total > 1000000 || vertices + total * 3 > 1000000)
            throw std::length_error("Static cut capacity exceeds supported budget");
        // cap도 독립 section이며 양쪽에서 같은 topology를 반대로 사용한다.
        CutSection cap{}; cap.VertexStart = uint32_t(vertices); cap.VertexCapacity = uint32_t(total * 3); cap.Flags = 31;
        job->Sections.push_back(cap);
        Mesh capTemplate = job->Templates.front(); capTemplate.psoFlags = 31;
        capTemplate.materialCBV = 0xffff; capTemplate.srvTable = CapTextureTable;
        job->Templates.push_back(capTemplate);
        vertices += cap.VertexCapacity;
        const auto parent = job->Input->GetGenerated();
        const uint32_t generation = parent ? parent->Generation + 1 : 1;
        const uint64_t parentBytes = parent ? parent->LineageBytes : 0;
        if (generation > 32 || parentBytes + vertices * 240 > 512ull * 1024 * 1024)
            throw std::length_error("Cut retained history/output memory budget exceeded");
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
        c.MaxSegments = uint32_t(total); uint32_t buckets = 1;
        while (buckets < total * 4) buckets *= 2;
        c.HashMask = buckets - 1; c.HashOffset = uint32_t(total * 128); c.HashLinksOffset = c.HashOffset + buckets * 4;
        for (auto& out : job->Output)
        {
            out = std::make_shared<GeometryBuffers>(); out->Source = job->Input->GetSource();
            out->Parent = job->Input;
            out->Generation = generation; out->LineageBytes = parentBytes + vertices * 120;
            out->Data.Create(L"Cut surface/depth/index", uint32_t(vertices * 48), 1);
            out->Pose.Create(L"Canonical cut pose", uint32_t(vertices * 40), 1);
            out->Provenance.Create(L"Cut current-parent provenance", uint32_t(vertices * 32), 1);
            MaterialConstants material{};
            material.baseColorFactor[0]=0.8f;material.baseColorFactor[1]=0.22f;material.baseColorFactor[2]=0.12f;material.baseColorFactor[3]=1;
            material.normalTextureScale=1;material.roughnessFactor=0.8f;
            out->CapMaterial.Create(L"Cut cap material",1,sizeof(material),&material);
        }
        std::vector<uint32_t> initial(20 + c.Sections * 2, 0);
        for (uint32_t side = 0; side < 2; ++side) for (uint32_t k = 0; k < 3; ++k) initial[8 + side * 6 + k] = 0xffffffff;
        job->Metadata.Create(L"Cut counts/status/bounds", uint32_t(initial.size()), 4, initial.data());
        job->Scratch.Create(L"Cut boundary graph/hash", c.HashLinksOffset + c.MaxSegments * 8, 1);
        job->Descriptors.Create(L"Cut sections", c.Sections, sizeof(CutSection), job->Sections.data());
        job->StatusReadback.Create(L"Cut metadata readback", uint32_t(initial.size()), 4);
        job->TimeReadback.Create(L"Cut timestamp readback", 5, 8);
        D3D12_QUERY_HEAP_DESC q{}; q.Count = 5; q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        ASSERT_SUCCEEDED(Graphics::g_Device->CreateQueryHeap(&q, IID_PPV_ARGS(job->Queries.GetAddressOf())));
        GraphicsContext& graphics = GraphicsContext::Begin(L"Static mesh cut");
        auto& compute = graphics.GetComputeContext();
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
            job->DiagnosticInputPose.Create(L"Diagnostic parent pose",uint32_t(inputPose.GetBufferSize()),1);
            job->DiagnosticInputData.Create(L"Diagnostic parent geometry",uint32_t(inputData.GetBufferSize()),1);
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
