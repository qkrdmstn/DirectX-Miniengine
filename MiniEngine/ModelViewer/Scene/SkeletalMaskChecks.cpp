#include "SkeletalMaskChecks.h"
#include "Scene.h"
#include "Renderer.h"
#include "BufferManager.h"
#include "ConstantBuffers.h"
#include "ShadowBuffer.h"
#include "ReadbackBuffer.h"
#include "SkeletalPatchCopy.h"
#include "CommandListManager.h"
#include <array>
#include <cstring>
#include <stdexcept>

namespace Viewer
{
    // 부모 제거 후 자식 추출과 마스크 없는 출력의 바이트 불변을 검사한다.
    void CheckMaskedPatch(GameObject& object, const std::vector<uint32_t>& state, uint32_t range, uint32_t overlap)
    {
        // 동일 pose와 평면에서 자식 본의 팔레트 선택을 고정한다.
        auto& component = *object.GetMeshComponents().front();
        const auto& geometry = *component.GetGeometry();
        const auto& model = *geometry.GetSource();
        std::vector<std::vector<uint32_t>> palette;
        for (size_t s = 0; s < geometry.GetSections().size(); ++s)
        {
            const auto& mesh = geometry.GetMesh(s);
            palette.emplace_back(mesh.numJoints, 0);
            for (uint32_t j = 0; j < mesh.numJoints; ++j)
                palette.back()[j] = model.m_JointIndices[mesh.startJoint + j] == 12;
        }
        MeshCutting::CutPlaneData plane;
        const float extent = float(model.m_BoundingSphere.GetRadius()) * 2;
        if (!MeshCutting::TryBuildCutPlaneData({ extent, extent },
            Math::AffineTransform(model.m_BoundingSphere.GetCenter()), plane))
            throw std::runtime_error("SP7d plane invalid");

        // 진단 제출은 실제 GPU 완료를 기다리고 CPU 출처 비교도 실행한다.
        const auto extract = [&](uint32_t expected)
        {
            // 결과의 유효 바이트만 이어 붙여 미초기화 출력 여유를 제외한다.
            SkeletalPatchJob job;
            if (!job.Submit(component, plane, palette, Math::Matrix4(component.GetWorldTransform(object.GetTransform())), 12, true))
                throw std::runtime_error("SP7d extraction rejected");
            Graphics::g_CommandManager.IdleGPU();
            SkeletalPatchResult result;
            std::shared_ptr<SkeletalPatchBuffers> patch;
            if (!job.Poll(result, patch) || result.PatchTriangles != expected)
                throw std::runtime_error("SP7d Q count mismatch");

            // 기준 평면이 실제로 교차해야 전체 제거 검사가 회귀를 검출한다.
            if (expected == range && !result.CrossingTriangles)
                throw std::runtime_error("SP7d baseline has no crossings");

            // 전체 제거에서는 출력과 교차·미포함 카운터가 모두 비어야 한다.
            if (!expected)
            {
                if (result.CrossingTriangles || result.UncoveredCrossing)
                    throw std::runtime_error("SP7d removed crossings counted");
                Utility::Printf("SP7d allRemoved patch/crossing/uncovered=0/0/0 PASS\n");
                return std::vector<uint8_t>{};
            }

            std::vector<uint8_t> bytes;
            auto& context = GraphicsContext::Begin(L"SP7d bit comparison");
            ReadbackBuffer data, origins, meta;
            data.Create(L"SP7d data", patch->Geometry->Data.GetBufferSize(), 1);
            origins.Create(L"SP7d origins", expected, 12);
            meta.Create(L"SP7d metadata", 44, 1);
            context.CopyBuffer(data, patch->Geometry->Data);
            context.TransitionResource(patch->TriangleSources, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.CopyBufferRegion(origins, 0, patch->TriangleSources, 0, expected * 12);
            context.CopyBuffer(meta, patch->LocalBounds);
            context.Finish(true);
            const auto* packed = static_cast<const uint8_t*>(data.Map());
            const auto& mesh = patch->Geometry->Meshes.front();
            bytes.insert(bytes.end(), packed, packed + mesh.vbSize);
            bytes.insert(bytes.end(), packed + mesh.vbDepthOffset, packed + mesh.vbDepthOffset + mesh.vbDepthSize);
            bytes.insert(bytes.end(), packed + mesh.ibOffset, packed + mesh.ibOffset + mesh.ibSize);
            const auto* source = static_cast<const uint8_t*>(origins.Map());
            bytes.insert(bytes.end(), source, source + expected * 12);
            const auto* counts = static_cast<const uint8_t*>(meta.Map());
            bytes.insert(bytes.end(), counts, counts + 44);
            data.Unmap(); origins.Unmap(); meta.Unmap();
            return bytes;
        };
        // 없음·부분 제거·전체 제거·0·해제 상태를 같은 입력에서 순서대로 비교한다.
        const auto before = extract(range);
        if (!component.SetTriangleMask(state)) throw std::runtime_error("SP7d mask rejected");
        extract(range - overlap);
        if (!component.SetTriangleMask(std::vector<uint32_t>(state.size(), 0x80000001u)))
            throw std::runtime_error("SP7d all removed mask rejected");
        extract(0);
        if (!component.SetTriangleMask(std::vector<uint32_t>(state.size()))) throw std::runtime_error("SP7d zero mask rejected");
        const auto zero = extract(range);
        component.ClearTriangleMask();
        const bool same = before == zero && before == extract(range);
        Utility::Printf("SP7d Xbot P=6 Q=12 range=%u alreadyRemoved=%u extracted=%u GPU==CPU=PASS noMaskBitIdentical=%s\n",
            range, overlap, range - overlap, same ? "PASS" : "FAIL");
        if (!same || !overlap || range <= overlap) throw std::runtime_error("SP7d mask regression");
    }

    // 원본 IB와 패치 추출 및 두 컴포넌트의 패스별 readback을 독립 비교한다.
    void RunSkeletalMaskChecks(Scene& scene)
    {
        // 원본 GPU 데이터를 읽고 각 모델의 지정 본을 선택한다.
        uint32_t models = 0;
        for (const auto& object : scene.GetObjects())
        {
            if (object->GetName() != "Xbot" && object->GetName() != "CesiumMan") continue;
            auto& component = *object->GetMeshComponents().front();
            const auto& geometry = *component.GetGeometry();
            const auto& model = *geometry.GetSource();
            const uint32_t joint = object->GetName() == "Xbot" ? 14 : 8;
            auto& source = const_cast<ByteAddressBuffer&>(model.m_DataBuffer);
            ReadbackBuffer sourceRead;
            sourceRead.Create(L"Mask original readback", source.GetBufferSize(), 1);
            auto& read = GraphicsContext::Begin(L"Mask original readback");
            read.TransitionResource(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
            read.CopyBufferRegion(sourceRead, 0, source, 0, source.GetBufferSize());
            read.TransitionResource(source, D3D12_RESOURCE_STATE_GENERIC_READ);
            read.Finish(true);
            const auto* bytes = static_cast<const uint8_t*>(sourceRead.Map());
            const std::vector<uint8_t> original(bytes, bytes + source.GetBufferSize());
            auto expected = original;
            std::vector<uint32_t> state;
            std::vector<uint32_t> parentState;
            uint32_t removed = 0, childRange = 0, childOverlap = 0;
            for (size_t s = 0; s < geometry.GetSections().size(); ++s)
            {
                const auto& mesh = geometry.GetMesh(s);
                const uint32_t width = mesh.ibFormat == DXGI_FORMAT_R32_UINT ? 4 : 2;
                for (uint32_t d = 0; d < mesh.numDraws; ++d)
                    for (uint32_t t = 0; t < mesh.draw[d].primCount / 3; ++t)
                    {
                        bool selected = false, child = false, parent = false;
                        const uint32_t at = mesh.ibOffset + (mesh.draw[d].startIndex + t * 3) * width;
                        for (uint32_t k = 0; k < 3 && (mesh.psoFlags & PSOFlags::kHasSkin); ++k)
                        {
                            uint32_t index = 0;
                            std::memcpy(&index, bytes + at + k * width, width);
                            const uint32_t vertex = index + mesh.draw[d].baseVertex;
                            uint16_t skin[8];
                            std::memcpy(skin, bytes + mesh.vbOffset + vertex * mesh.vbStride + mesh.vbStride - 16, 16);
                            for (uint32_t w = 0; w < 4; ++w)
                            {
                                selected |= skin[w] < mesh.numJoints && skin[w + 4] > 0
                                    && model.m_JointIndices[mesh.startJoint + skin[w]] == joint;
                                child |= skin[w] < mesh.numJoints && skin[w + 4] > 0
                                    && model.m_JointIndices[mesh.startJoint + skin[w]] == 12;
                                parent |= skin[w] < mesh.numJoints && skin[w + 4] > 0
                                    && model.m_JointIndices[mesh.startJoint + skin[w]] == 6;
                            }
                        }
                        childRange += child;
                        childOverlap += child && parent;
                        parentState.push_back(parent ? 0x80000001u : 0u);
                        state.push_back(selected ? 0x80000001u : 0u);
                        if (!selected) continue;
                        ++removed;
                        for (uint32_t k = 1; k < 3; ++k) std::memcpy(expected.data() + at + k * width, bytes + at, width);
                    }
            }
            sourceRead.Unmap();
            if (!removed) throw std::runtime_error("Mask fixture has no selected triangles");

            // Xbot Spine2를 제거한 뒤 자식 LeftShoulder를 별도 패치로 추출한다.
            if (object->GetName() == "Xbot") CheckMaskedPatch(*object, parentState, childRange, childOverlap);

            // 같은 Model과 pose를 공유하는 별도 컴포넌트를 같은 위치에서 검사한다.
            MeshComponent peer(ComponentId(100000), component.GetGeometry());
            peer.SetPlacement(component.GetTransform(), component.GetTransformMode(), component.GetDeformBinding());
            Math::Camera camera;
            const auto center = object->GetTransform().ToAffine() * model.m_BoundingSphere.GetCenter();
            camera.SetEyeAtUp(center + Math::Vector3(0, 0, 6), center, Math::Vector3(Math::kYUnitVector));
            camera.SetPerspectiveMatrix(0.8f, float(Graphics::g_SceneColorBuffer.GetHeight()) / Graphics::g_SceneColorBuffer.GetWidth(), 0.1f, 100.0f);
            camera.Update();
            // 각 패스는 비운 타깃에 같은 pose를 그리고 padding을 제외한 실제 픽셀을 읽는다.
            const auto render = [&](MeshComponent& target)
            {
                std::array<std::vector<uint8_t>, 3> result;
                for (uint32_t pass = 0; pass < 3; ++pass)
                {
                    auto& context = GraphicsContext::Begin(L"Mask pass readback");
                    target.Update(context, *object->GetModelInstance(), object->GetTransform());
                    GlobalConstants globals{};
                    globals.SunDirection = Math::Vector3(0, 1, 0);
                    globals.SunIntensity = Math::Vector3(1);
                    globals.SunShadowMatrix = Math::Matrix4(Math::kIdentity);
                    Renderer::MeshSorter sorter(pass == 0 ? Renderer::MeshSorter::kShadows : Renderer::MeshSorter::kDefault);
                    sorter.SetCamera(camera);
                    auto& depth = pass == 0 ? static_cast<DepthBuffer&>(Graphics::g_ShadowBuffer) : Graphics::g_SceneDepthBuffer;
                    sorter.SetDepthStencilTarget(depth);
                    sorter.AddRenderTarget(Graphics::g_SceneColorBuffer);
                    context.TransitionResource(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
                    context.ClearDepth(depth);
                    context.TransitionResource(Graphics::g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
                    context.ClearColor(Graphics::g_SceneColorBuffer);
                    context.TransitionResource(Graphics::g_SSAOFullScreen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    if (pass != 0) context.TransitionResource(Graphics::g_ShadowBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    target.Render(sorter);
                    sorter.Sort();
                    sorter.RenderMeshes(pass == 2 ? Renderer::MeshSorter::kOpaque : Renderer::MeshSorter::kZPass, context, globals);
                    ReadbackBuffer pixels;
                    auto& texture = pass == 2 ? static_cast<PixelBuffer&>(Graphics::g_SceneColorBuffer) : depth;
                    const uint32_t pitch = context.ReadbackTexture(pixels, texture);
                    context.Finish(true);
                    const auto* data = static_cast<const uint8_t*>(pixels.Map());
                    const uint32_t rowBytes = texture.GetWidth() * (pass == 0 ? 2 : 4);
                    for (uint32_t y = 0; y < texture.GetHeight(); ++y)
                        result[pass].insert(result[pass].end(), data + y * pitch, data + y * pitch + rowBytes);
                    pixels.Unmap();
                }
                return result;
            };
            const auto before = render(component);
            const auto peerBefore = render(peer);
            if (!component.SetTriangleMask(state)) throw std::runtime_error("Mask state rejected");
            const auto after = render(component);
            const uint32_t revision = component.GetTriangleMask()->Revision;
            if (!component.SetTriangleMask(state) || component.GetTriangleMask()->Revision != revision
                || component.GetTriangleMask()->AppliedRevision != revision)
                throw std::runtime_error("Mask revision mismatch");

            // 치환 IB는 전체 바이트를 CPU 퇴화 기대값과 비교한다.
            const auto& mask = *component.GetTriangleMask();
            ReadbackBuffer ib;
            ib.Create(L"Mask IB readback", mask.ByteCount, 1);
            auto& check = GraphicsContext::Begin(L"Mask IB verify");
            check.TransitionResource(const_cast<ByteAddressBuffer&>(mask.ReplacedIB), D3D12_RESOURCE_STATE_COPY_SOURCE);
            check.CopyBufferRegion(ib, 0, const_cast<ByteAddressBuffer&>(mask.ReplacedIB), 0, mask.ByteCount);
            check.TransitionResource(const_cast<ByteAddressBuffer&>(mask.ReplacedIB), D3D12_RESOURCE_STATE_INDEX_BUFFER);
            check.Finish(true);
            const bool ibPass = std::memcmp(ib.Map(), expected.data() + mask.FirstByte, mask.ByteCount) == 0;
            ib.Unmap();
            const auto peerAfter = render(peer);
            if (!component.SetTriangleMask(std::vector<uint32_t>(state.size())))
                throw std::runtime_error("Mask reset rejected");
            const auto reset = render(component);
            component.ClearTriangleMask();
            const auto restored = render(component);

            // 공유 Model 전체와 다른 컴포넌트 보존, 상태 갱신 및 해제 복원을 확인한다.
            auto& verify = GraphicsContext::Begin(L"Mask shared source verify");
            verify.TransitionResource(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
            verify.CopyBufferRegion(sourceRead, 0, source, 0, source.GetBufferSize());
            verify.TransitionResource(source, D3D12_RESOURCE_STATE_GENERIC_READ);
            verify.Finish(true);
            const bool sourcePass = std::memcmp(sourceRead.Map(), original.data(), original.size()) == 0;
            sourceRead.Unmap();
            bool pass = ibPass && sourcePass && before == peerBefore && peerBefore == peerAfter
                && before == reset && before == restored;

            // 단독 모델의 원래 커버리지 bounding rectangle 안에서 유효 픽셀이 clear(0)로 사라져야 한다.
            for (uint32_t p = 0; p < 3; ++p)
            {
                const uint32_t width = p == 0 ? Graphics::g_ShadowBuffer.GetWidth() : Graphics::g_SceneColorBuffer.GetWidth();
                const uint32_t pixelBytes = p == 0 ? 2 : 4;
                const uint32_t height = uint32_t(before[p].size() / width / pixelBytes);
                uint32_t left = width, top = height, right = 0, bottom = 0;
                for (uint32_t y = 0; y < height; ++y)
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        uint32_t value = 0;
                        std::memcpy(&value, before[p].data() + (size_t(y) * width + x) * pixelBytes, pixelBytes);
                        if (!value) continue;
                        left = std::min(left, x); right = std::max(right, x + 1);
                        top = std::min(top, y); bottom = std::max(bottom, y + 1);
                    }

                // 조명·깊이 값만 달라진 픽셀은 소실로 세지 않는다.
                size_t vanished = 0, appeared = 0, changed = 0;
                for (uint32_t y = top; y < bottom; ++y)
                    for (uint32_t x = left; x < right; ++x)
                    {
                        const size_t at = (size_t(y) * width + x) * pixelBytes;
                        uint32_t a = 0, b = 0;
                        std::memcpy(&a, before[p].data() + at, pixelBytes);
                        std::memcpy(&b, after[p].data() + at, pixelBytes);
                        uint32_t depthBefore = 0, depthAfter = 0;
                        if (p == 2)
                        {
                            std::memcpy(&depthBefore, before[1].data() + at, 4);
                            std::memcpy(&depthAfter, after[1].data() + at, 4);
                        }
                        vanished += a != 0 && b == 0 && (p != 2 || (depthBefore != 0 && depthAfter == 0));
                        appeared += a == 0 && b != 0;
                        for (uint32_t k = 0; k < pixelBytes; ++k) changed += before[p][at + k] != after[p][at + k];
                    }
                pass &= vanished > 0 && (p == 2 || appeared == 0);
                Utility::Printf("SP7c %s pass=%u removed=%u changedBytes=%zu vanished=%zu appeared=%zu ROI=%u,%u,%u,%u IB=%u source=%u peer=%u reset=%u restore=%u\n",
                    object->GetName().c_str(), p, removed, changed, vanished, appeared, left, top, right, bottom,
                    ibPass, sourcePass, peerBefore[p] == peerAfter[p], before[p] == reset[p], before[p] == restored[p]);
            }
            if (!pass) throw std::runtime_error("Mask readback mismatch");
            ++models;
        }
        if (models != 2) throw std::runtime_error("Mask diagnostic requires both models");
    }
}
