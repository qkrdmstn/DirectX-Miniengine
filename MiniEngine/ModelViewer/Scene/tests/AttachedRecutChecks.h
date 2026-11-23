#pragma once
#include "../CuttingSystem.h"
#include "CommandContext.h"
#include "ReadbackBuffer.h"
#include "Utility.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Viewer
{
    // 실제 부착 패치와 독립 조각의 Static 재절단을 프레임 경계에서 검사한다.
    struct AttachedRecutChecks
    {
        bool Enabled = false, Done = false;
        bool Passed[5]{};
        unsigned Stage = 0, PublishedFrame = 0, Observe = 0;
        ObjectId Owner = 0;
        ComponentId Input = 0, Attached = 0, Detached = 0;
        size_t Count = 0;
        float StartY = 0;
        bool MeasuredPublish = false, HaveOrigin = false;
        Math::Vector3 FirstPiece{ Math::kZero }, FirstNode{ Math::kZero };
        bool Follows = true, Body = false, Collision = false, NodePositive = false;
        // RS-4: 진단 옵션 값(2 이상이면 스키닝 승계 조각을 한 번 더 재절단한다)과 연속 재절단 상태.
        unsigned Mode = 1, SecondObserve = 0;
        bool SecondPassed = false, SecondSkinned = false, SecondPublishOk = false, SecondMeasured = false, SecondAfterOk = false;
        ComponentId Second = 0;
        float MaxKLocal = 0;

        // 각 판정과 요약을 debugger와 기존 패치 검사 파일에 함께 남긴다.
        static void Log(const char* label, bool pass, bool verdict = true)
        {
            // 고정 길이 한 줄을 포맷하고 파일을 즉시 닫는다.
            char line[1024], path[MAX_PATH];
            sprintf_s(line, verdict ? "%s %s\n" : "%s\n", label, pass ? "PASS" : "FAIL");
            Utility::Print(line);
            FILE* file = nullptr;
            if (GetTempPathA(MAX_PATH, path))
            {
                strcat_s(path, "SP6-SkeletalPatchChecks.log");
                if (!fopen_s(&file, path, "a"))
                {
                    fputs(line, file);
                    fclose(file);
                }
            }
        }

        // 미완료 항목도 FAIL로 출력하고 정상 정리 후 반환할 결과를 확정한다.
        void Finish()
        {
            // 다섯 항목을 빠짐없이 한 줄씩 기록한다.
            const char* labels[] = { "AR4 recut published results=2", "AR4 attachment follows 60 frames <=1mm",
                "AR4 detached kdop body fall >=1cm", "AR4 ambiguous node side rejected unchanged", "AR4 detached Static recut published" };
            bool pass = true;
            for (unsigned i = 0; i < 5; ++i)
            {
                Log(labels[i], Passed[i]);
                pass &= Passed[i];
            }

            // RS-4: 연속 재절단 진단은 별도 판정 줄로 남기고 전체 결과에 포함한다.
            if (Mode >= 2)
            {
                char second[200];
                sprintf_s(second, "RS4 second recut skinned=%d boundary publish=%d after60=%d", int(SecondSkinned), int(SecondPublishOk), int(SecondAfterOk));
                SecondPassed = SecondSkinned && SecondPublishOk && SecondAfterOk;
                Log(second, SecondPassed);
                pass &= SecondPassed;
            }
            Log("Attached recut checks", pass);
            Done = true;
        }

        // 실제 생성 정점을 읽어 월드 bounds 중심과 교차하는 평면을 만든다.
        static bool Plane(const GameObject& owner, const MeshComponent& mesh, const Math::Vector3* node,
            MeshCutting::CutPlaneData& plane)
        {
            // 생성 버퍼를 한 번 동기화하고 section VB에서 월드 정점과 AABB를 계산한다.
            using namespace Math;
            const auto& geometry = *mesh.GetGeometry();
            if (!geometry.IsGenerated()) return false;

            // 스키닝 조각은 현재 PG(40 B)를, 그 외에는 Data(28 B)를 읽는다.
            const bool skinned = mesh.HasSkinnedPatch() && mesh.GetPosedGeometry().IsReady();
            // 진단 readback은 상태 전이만 하므로 공유 revision 버퍼의 const를 제거한다.
            auto& data = const_cast<ByteAddressBuffer&>(skinned ? mesh.GetPosedGeometry().GetBuffer() : geometry.GetGenerated()->Data);
            ReadbackBuffer readback;
            readback.Create(L"AR4 vertices", uint32_t(data.GetBufferSize()), 1);
            auto& context = GraphicsContext::Begin(L"AR4 read vertices");
            context.TransitionResource(data, D3D12_RESOURCE_STATE_COPY_SOURCE, true);
            context.CopyBufferRegion(readback, 0, data, 0, data.GetBufferSize());
            context.TransitionResource(data, D3D12_RESOURCE_STATE_GENERIC_READ);
            context.Finish(true);
            const auto* bytes = static_cast<const uint8_t*>(readback.Map());
            std::vector<Vector3> points;
            Vector3 lo(FLT_MAX), hi(-FLT_MAX);
            const auto world = mesh.GetWorldTransform(owner.GetTransform());
            for (size_t section = 0; section < geometry.GetSections().size(); ++section)
            {
                const auto& vb = geometry.GetMesh(section);
                const size_t offset = skinned ? mesh.GetPosedGeometry().GetOffset(geometry.GetSections()[section].SourceMesh) : vb.vbOffset;
                const size_t stride = skinned ? 40 : vb.vbStride;
                for (size_t v = 0; v < vb.vbSize / vb.vbStride; ++v)
                {
                    DirectX::XMFLOAT3 p;
                    std::memcpy(&p, bytes + offset + v * stride, sizeof(p));
                    const Vector3 point = world * Vector3(p.x, p.y, p.z);
                    points.push_back(point);
                    lo = Min(lo, point);
                    hi = Max(hi, point);
                }
            }
            readback.Unmap();
            if (points.empty()) return false;

            // 중심에서 node 방향을 우선하고 세 축도 검사해 양쪽 정점과 1mm 초과 거리를 보장한다.
            const Vector3 center = (lo + hi) * 0.5f;
            Vector3 normals[] = { node ? *node - center : hi - lo, Vector3(kXUnitVector), Vector3(kYUnitVector), Vector3(kZUnitVector) };
            for (const auto& axis : normals)
            {
                if (float(Length(axis)) < 1.0e-6f) continue;
                const Vector3 normal = Normalize(axis);
                if (node && std::abs(float(Dot(normal, *node - center))) <= 0.001f) continue;
                float low = FLT_MAX, high = -FLT_MAX;
                for (const auto& point : points)
                {
                    const float d = float(Dot(normal, point - center));
                    low = (std::min)(low, d);
                    high = (std::max)(high, d);
                }
                if (low >= -1.0e-5f || high <= 1.0e-5f) continue;
                const auto x = Normalize(Cross(normal, std::abs(float(normal.GetY())) < 0.9f ? Vector3(kYUnitVector) : Vector3(kXUnitVector)));
                if (MeshCutting::TryBuildCutPlaneData({ 1000, 1000 }, AffineTransform(Matrix3(x, normal, Cross(x, normal)), center), plane)) return true;
            }
            return false;
        }

        // GPU 버퍼 전체를 읽어 CPU 바이트로 복사하고 상태를 GENERIC_READ로 되돌린다.
        static bool ReadBytes(const ByteAddressBuffer& source, std::vector<uint8_t>& bytes)
        {
            // 같은 크기의 readback 버퍼로 복사하고 GPU 완료를 기다린다.
            const uint32_t size = uint32_t(source.GetBufferSize());
            if (!size) return false;
            ReadbackBuffer readback;
            readback.Create(L"RS3 readback", size, 1);
            auto& context = GraphicsContext::Begin(L"RS3 read");
            auto& buffer = const_cast<ByteAddressBuffer&>(source);
            context.CopyBuffer(readback, buffer);
            context.TransitionResource(buffer, D3D12_RESOURCE_STATE_GENERIC_READ);
            context.Finish(true);

            // 매핑한 내용을 복사해 readback 수명과 분리한다.
            const auto* mapped = static_cast<const uint8_t*>(readback.Map());
            if (!mapped) return false;
            bytes.assign(mapped, mapped + size);
            readback.Unmap();
            return true;
        }

        // RS-3: 스키닝 조각의 복사 정점(고정 joint weight 0)을 같은 프레임 원본 PG의 가장 가까운 정점과 비교한다.
        // 최대 차이를 로그로 남긴다.
        static bool MeasureRecutBoundary(const GameObject& owner, const MeshComponent& piece, const char* frame)
        {
            // 조각 skin VB, 조각 PG, 원본 PG를 모두 읽을 수 있어야 측정한다.
            using namespace Math;
            char line[256];
            const auto* source = piece.Attachment ? owner.FindMeshComponent(piece.Attachment->SourceMesh) : nullptr;
            std::vector<uint8_t> skin, pieceBytes, sourceBytes;
            if (!piece.PatchSkin || !source || !piece.GetPosedGeometry().IsReady() || !source->GetPosedGeometry().IsReady()
                || !ReadBytes(piece.PatchSkin->Vertices.Data, skin) || !ReadBytes(piece.GetPosedGeometry().GetBuffer(), pieceBytes)
                || !ReadBytes(source->GetPosedGeometry().GetBuffer(), sourceBytes))
            {
                sprintf_s(line, "RS3 boundary frame=%s unreadable", frame);
                Log(line, false);
                return false;
            }

            // 원본 PG의 모든 정점을 월드로 옮겨 둔다.
            const Matrix4 sourceWorld(source->GetWorldTransform(owner.GetTransform()));
            const Matrix4 pieceWorld(piece.GetWorldTransform(owner.GetTransform()));
            std::vector<Vector3> origin(sourceBytes.size() / 40);
            for (size_t i = 0; i < origin.size(); ++i)
            {
                DirectX::XMFLOAT3 p;
                std::memcpy(&p, sourceBytes.data() + i * 40, sizeof(p));
                origin[i] = Vector3(sourceWorld * Vector4(Vector3(p.x, p.y, p.z), 1));
            }

            // 고정 joint weight가 없는 복사 정점만 골라 가장 가까운 원본 정점과의 거리를 구한다.
            const auto& skinData = *piece.PatchSkin;
            const uint32_t stride = skinData.Vertices.Stride, rigidSlot = uint32_t(skinData.Snapshot.JointsAtCut.size());
            const auto& geometry = *piece.GetGeometry();
            float maximum = 0;
            uint32_t measured = 0, frozen = 0;
            for (size_t s = 0; s < skinData.Sections.size(); ++s)
            {
                const size_t pgOffset = piece.GetPosedGeometry().GetOffset(geometry.GetSections().at(s).SourceMesh);
                for (uint32_t v = 0; v < skinData.Sections[s].VertexCount; ++v)
                {
                    // 내부 교점과 강체 슬롯(K) 정점은 원본 정점이 없어 제외한다.
                    const uint8_t* record = skin.data() + size_t(skinData.Sections[s].FirstVertex + v) * stride;
                    uint32_t internal = 0;
                    uint16_t joints[4], weights[4];
                    std::memcpy(&internal, record + stride - kSkinExtInternal, sizeof(internal));
                    std::memcpy(joints, record + stride - 16, sizeof(joints));
                    std::memcpy(weights, record + stride - 8, sizeof(weights));
                    if (internal || joints[0] == rigidSlot) continue;

                    // 고정 joint에 weight가 있으면 원본과 달라지는 것이 정상이므로 따로 센다.
                    bool hasFrozen = false;
                    for (int i = 0; i < 4; ++i)
                        hasFrozen |= weights[i] && joints[i] < skinData.Snapshot.Frozen.size() && skinData.Snapshot.Frozen[joints[i]];
                    if (hasFrozen)
                    {
                        ++frozen;
                        continue;
                    }

                    // 조각 PG 위치를 월드로 옮겨 원본 정점 전체에서 최근접 거리를 찾는다.
                    DirectX::XMFLOAT3 p;
                    std::memcpy(&p, pieceBytes.data() + pgOffset + size_t(v) * 40, sizeof(p));
                    const Vector3 position(pieceWorld * Vector4(Vector3(p.x, p.y, p.z), 1));
                    float nearest = FLT_MAX;
                    for (const auto& other : origin)
                        nearest = (std::min)(nearest, float(Length(position - other)));
                    maximum = (std::max)(maximum, nearest);
                    ++measured;
                }
            }

            // 월드 거리를 컴포넌트 로컬 단위로 환산해 local 1e-6 이하를 판정한다.
            const float scale = (std::max)({ float(Length(Vector3(pieceWorld.GetX()))), float(Length(Vector3(pieceWorld.GetY()))),
                float(Length(Vector3(pieceWorld.GetZ()))) });
            const float local = scale > 0 ? maximum / scale : FLT_MAX;
            sprintf_s(line, "RS3 boundary frame=%s measured=%u frozenSkipped=%u max=%.3g local (%.4f mm, scale %.1f)",
                frame, measured, frozen, local, maximum * 1000.0f, scale);
            const bool pass = measured > 0 && local <= 1.0e-6f;
            Log(line, pass);
            return pass;
        }

        // RS-4: 스키닝 조각의 K(새 절단면) 정점 PG 월드 위치와 node 기대 월드로 옮긴 구운 위치를 비교한다.
        // 최대 월드 오차를 돌려주고 컴포넌트 local 환산값은 localError로 낸다.
        static float SkinnedFollowError(Scene& scene, const GameObject& owner, const MeshComponent& piece, float* localError)
        {
            // 부착 snapshot, 게시된 PG, K 정점 skin, 구운 Data를 모두 읽을 수 있어야 한다.
            using namespace Math;
            Matrix4 node;
            std::vector<uint8_t> skin, pg, data;
            if (!piece.Attachment || !piece.PatchSkin || !piece.GetPosedGeometry().IsReady() || !piece.GetGeometry()->IsGenerated()
                || !scene.TryGetAttachmentNodeWorld(piece, node) || !ReadBytes(piece.PatchSkin->Vertices.Data, skin)
                || !ReadBytes(piece.GetPosedGeometry().GetBuffer(), pg) || !ReadBytes(piece.GetGeometry()->GetGenerated()->Data, data))
                return FLT_MAX;

            // node 기대 월드 변환과 실제 조각 월드 변환을 구한다.
            const auto& snapshot = *piece.Attachment;
            const Matrix4 expected = node * Invert(snapshot.NodeWorldAtCut) * snapshot.PieceWorldAtCut;
            const Matrix4 actual(piece.GetWorldTransform(owner.GetTransform()));

            // 강체 슬롯(K)에 붙은 정점마다 두 월드 위치의 거리를 잰다.
            const auto& skinData = *piece.PatchSkin;
            const auto& geometry = *piece.GetGeometry();
            const uint32_t stride = skinData.Vertices.Stride, rigidSlot = uint32_t(skinData.Snapshot.JointsAtCut.size());
            float maximum = 0;
            uint32_t measured = 0;
            for (size_t s = 0; s < skinData.Sections.size(); ++s)
            {
                const auto& vb = geometry.GetMesh(s);
                const size_t pgOffset = piece.GetPosedGeometry().GetOffset(geometry.GetSections().at(s).SourceMesh);
                for (uint32_t v = 0; v < skinData.Sections[s].VertexCount; ++v)
                {
                    const uint8_t* record = skin.data() + size_t(skinData.Sections[s].FirstVertex + v) * stride;
                    uint32_t internal = 0;
                    uint16_t joints[4];
                    std::memcpy(&internal, record + stride - kSkinExtInternal, sizeof(internal));
                    std::memcpy(joints, record + stride - 16, sizeof(joints));
                    if (internal || joints[0] != rigidSlot) continue;

                    // 구운 위치는 기대 월드로, PG 위치는 현재 조각 월드로 옮겨 비교한다.
                    DirectX::XMFLOAT3 baked, posed;
                    std::memcpy(&baked, data.data() + vb.vbOffset + size_t(v) * vb.vbStride, sizeof(baked));
                    std::memcpy(&posed, pg.data() + pgOffset + size_t(v) * 40, sizeof(posed));
                    const Vector3 want(expected * Vector4(Vector3(baked.x, baked.y, baked.z), 1));
                    const Vector3 have(actual * Vector4(Vector3(posed.x, posed.y, posed.z), 1));
                    maximum = (std::max)(maximum, float(Length(want - have)));
                    ++measured;
                }
            }

            // 측정한 정점이 없으면 판정할 수 없으므로 실패 값을 낸다.
            if (!measured) return FLT_MAX;
            const float scale = (std::max)({ float(Length(Vector3(actual.GetX()))), float(Length(Vector3(actual.GetY()))),
                float(Length(Vector3(actual.GetZ()))) });
            if (localError) *localError = scale > 0 ? maximum / scale : FLT_MAX;
            return maximum;
        }

        // AR과 RBC가 같은 node 상대 원점 오차를 측정한다.
        static float FollowError(Scene& scene, const GameObject& owner, const MeshComponent& mesh, float* localError = nullptr)
        {
            // 스키닝 조각은 source W가 월드이므로 K 정점 추종으로 판정한다(RS-4, D7).
            if (mesh.HasSkinnedPatch())
                return SkinnedFollowError(scene, owner, mesh, localError);

            // 정적 조각은 현재 node와 게시 snapshot으로 기대 월드 원점을 복원한다.
            Math::Matrix4 node;
            if (!mesh.Attachment || !scene.TryGetAttachmentNodeWorld(mesh, node)) return FLT_MAX;
            const auto& snapshot = *mesh.Attachment;
            const auto expected = node * Invert(snapshot.NodeWorldAtCut) * snapshot.PieceWorldAtCut;
            return float(Math::Length(Math::Vector3(expected.GetW()) - mesh.GetWorldTransform(owner.GetTransform()).GetTranslation()));
        }

        // 두 진단의 KDOP 낙하 조건을 동일하게 적용한다.
        static bool Falls(bool collision, bool body, bool attached, bool running, float drop)
        {
            // 기존 AR4의 1cm 기준과 필수 조건을 유지한다.
            return collision && body && !attached && running && drop >= 0.01f;
        }

        // DrainResults의 대상 ID와 실제 게시된 두 컴포넌트로 결과를 확인한다.
        void OnResult(Scene& scene, const CutResultEntry& result)
        {
            // 초기 패치 절단과 다른 대상의 결과는 소비하지 않는다.
            if (!Enabled || Done || result.Owner != Owner || result.Mesh != (Stage == 4 ? Detached : Stage == 7 ? Attached : Input)) return;
            auto* owner = scene.FindObject(Owner);
            auto* a = owner ? owner->FindMeshComponent(result.Mesh) : nullptr;
            auto* b = owner ? owner->FindMeshComponent(result.Other) : nullptr;
            const bool published = result.Status == CutResultStatus::Published && !result.Error && a && b;
            if (Stage == 1 && published)
            {
                Passed[0] = owner->GetMeshComponents().size() == Count + 1;
                Attached = NodePositive ? a->GetId() : b->GetId();
                Detached = NodePositive ? b->GetId() : a->GetId();
                auto* detached = owner->FindMeshComponent(Detached);
                Collision = !detached->Attachment && std::strcmp(detached->GetCollisionReason(), "detached kdop") == 0;

                // RS-3: node 쪽 결과가 스키닝을 유지하는지 기록한다.
                char skinLine[96];
                sprintf_s(skinLine, "RS3 node side skinned=%d", int(owner->FindMeshComponent(Attached)->HasSkinnedPatch()));
                Log(skinLine, true, false);
                StartY = float(detached->GetWorldTransform(owner->GetTransform()).GetTranslation().GetY());
                Stage = 2;
            }
            else if (Stage == 4)
            {
                Passed[4] = published && !a->Attachment && !b->Attachment && owner->GetMeshComponents().size() == Count + 1;

                // RS-4: 연속 재절단 모드면 스키닝 승계 조각을 한 번 더 자른다.
                if (Mode >= 2 && Passed[4] && owner->FindMeshComponent(Attached) && owner->FindMeshComponent(Attached)->HasSkinnedPatch())
                    Stage = 5;
                else Finish();
            }
            else if (Stage == 7)
            {
                // RS-4: 두 번째 결과의 node 쪽(Attachment 보유)이 스키닝을 유지하는지 확인한다.
                auto* node = published ? (a->Attachment ? a : b->Attachment ? b : nullptr) : nullptr;
                if (!node)
                {
                    Finish();
                    return;
                }
                Second = node->GetId();
                SecondSkinned = node->HasSkinnedPatch();
                Stage = 6;
            }
            else Finish();
        }

        // break 게시 뒤 세 프레임을 기다리고 60프레임 관찰과 두 후속 요청을 진행한다.
        void Tick(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting, bool patchPending, unsigned frame)
        {
            // 무한 대기와 준비 실패를 모두 최종 FAIL로 끝낸다.
            using namespace Math;
            if (!Enabled || Done) return;
            if (frame > 1200 || !physics.IsSimulationEnabled())
            {
                Finish();
                return;
            }
            auto* owner = scene.FindObject(Owner);
            if (Stage == 0 && !patchPending && frame > 60)
            {
                // A/B가 모두 게시된 owner를 찾고 GPU 준비 이후 세 프레임을 더 기다린다.
                for (const auto& object : scene.GetObjects())
                    for (const auto& mesh : object->GetMeshComponents())
                        if (mesh->PublishVisible && mesh->Attachment && mesh->HasPoseBounds())
                        {
                            if (!PublishedFrame) PublishedFrame = frame;
                            if (frame < PublishedFrame + 3) return;
                            Matrix4 node;
                            MeshCutting::CutPlaneData plane;
                            if (!scene.TryGetAttachmentNodeWorld(*mesh, node)) continue;
                            const Vector3 position(node.GetW());
                            if (!Plane(*object, *mesh, &position, plane)) continue;
                            Owner = object->GetId();
                            Input = mesh->GetId();
                            Count = object->GetMeshComponents().size();
                            NodePositive = float(Dot(plane.WorldPlane, node.GetW())) > 0;
                            Stage = 1;
                            if (!cutting.Request(Owner, Input, plane)) Finish();
                            return;
                        }
            }

            // RS-4: node 쪽 조각을 한 번 더 재절단하는 요청을 낸다.
            if (Stage == 5 && owner && !cutting.IsBusy())
            {
                auto* piece = owner->FindMeshComponent(Attached);
                Matrix4 node;
                MeshCutting::CutPlaneData plane;
                if (!piece || !piece->HasPoseBounds() || !scene.TryGetAttachmentNodeWorld(*piece, node))
                {
                    Finish();
                    return;
                }
                const Vector3 position(node.GetW());
                Stage = 7;
                if (!Plane(*owner, *piece, &position, plane) || !cutting.Request(Owner, Attached, plane)) Finish();
                return;
            }

            // RS-4: 두 번째 node 쪽 조각의 경계 오차를 게시 직후와 60프레임 뒤에 잰다.
            if (Stage == 6 && owner)
            {
                auto* second = owner->FindMeshComponent(Second);
                if (!second || !SecondSkinned)
                {
                    Finish();
                    return;
                }
                if (second->HasSkinnedPatch() && second->HasPoseBounds() && second->GetPosedGeometry().IsReady())
                {
                    if (!SecondMeasured)
                    {
                        SecondMeasured = true;
                        SecondPublishOk = MeasureRecutBoundary(*owner, *second, "second-publish");
                    }
                    if (++SecondObserve >= 60)
                    {
                        SecondAfterOk = MeasureRecutBoundary(*owner, *second, "second-after60");
                        Finish();
                    }
                }
                return;
            }
            if (Stage != 2 || !owner) return;

            // 같은 프레임 node 기준 기대 월드와 실제 원점을 매 프레임 비교한다.
            auto* attached = owner->FindMeshComponent(Attached);
            auto* detached = owner->FindMeshComponent(Detached);
            Matrix4 node;
            if (!attached || !detached || !attached->Attachment || !scene.TryGetAttachmentNodeWorld(*attached, node))
            {
                Finish();
                return;
            }
            // RS-3: 스키닝 조각은 PG가 준비된 첫 프레임과 60프레임째에 원본과의 경계 오차를 잰다.
            if (attached->HasSkinnedPatch() && attached->HasPoseBounds() && attached->GetPosedGeometry().IsReady())
            {
                if (!MeasuredPublish)
                {
                    MeasuredPublish = true;
                    MeasureRecutBoundary(*owner, *attached, "publish");
                }
                if (Observe == 59)
                    MeasureRecutBoundary(*owner, *attached, "after60");
            }
            // RS-4 준비: 조각 월드 원점(source W)과 node 기대 원점의 이동량을 비교한다.
            // FollowError FAIL이 판정 방식 때문인지 이 로그로 확인한다.
            const Vector3 pieceOrigin = attached->GetWorldTransform(owner->GetTransform()).GetTranslation();
            const Vector3 nodeOrigin(node.GetW());
            if (!HaveOrigin)
            {
                HaveOrigin = true;
                FirstPiece = pieceOrigin;
                FirstNode = nodeOrigin;
            }
            if (Observe == 59)
            {
                char moveLine[256];
                const Vector3 pieceMove = pieceOrigin - FirstPiece, nodeMove = nodeOrigin - FirstNode;
                sprintf_s(moveLine, "RS3 follow diag pieceMove=%.4f nodeMove=%.4f diff=%.4f m skinned=%d",
                    float(Length(pieceMove)), float(Length(nodeMove)), float(Length(pieceMove - nodeMove)), int(attached->HasSkinnedPatch()));
                Log(moveLine, true, false);
            }
            float local = 0;
            const float error = FollowError(scene, *owner, *attached, &local);
            if (attached->HasSkinnedPatch() && std::isfinite(error))
                MaxKLocal = (std::max)(MaxKLocal, local);
            if (Observe == 59 && attached->HasSkinnedPatch())
            {
                char followLine[160];
                sprintf_s(followLine, "RS4 K follow last=%.4f mm maxLocal=%.3g", error * 1000.0f, MaxKLocal);
                Log(followLine, true, false);
            }
            Follows &= std::isfinite(error) && error <= 0.001f;
            Body |= physics.GetBodyState(Owner, Detached) != PhysicsSystem::BodyState::None;
            if (++Observe < 60) return;
            Passed[1] = Follows;
            const float drop = StartY - float(detached->GetWorldTransform(owner->GetTransform()).GetTranslation().GetY());
            Passed[2] = Falls(Collision, Body, detached->Attachment != nullptr, physics.IsSimulationRunning(), drop);

            // 낙하 판정의 세부 조건을 실패 원인 구분용으로 남긴다.
            char detail[160];
            sprintf_s(detail, "AR4 detail collision=%d body=%d attached=%d running=%d drop=%.4f",
                int(Collision), int(Body), int(detached->Attachment != nullptr), int(physics.IsSimulationRunning()), drop);
            Log(detail, Passed[2]);

            // node를 통과하는 두 번째 요청은 명시적 거절 카운터와 조각 수로 판정한다.
            MeshCutting::CutPlaneData plane;
            const size_t count = owner->GetMeshComponents().size();
            const auto rejected = physics.GetAmbiguousNodeSideCount();
            const Vector3 center = attached->GetBoundingSphere(owner->GetTransform()).GetCenter();
            const Vector3 toward = center - Vector3(node.GetW());
            const Vector3 normal = float(Length(toward)) > 1.0e-6f
                ? Normalize(Cross(Normalize(toward), std::abs(float(Normalize(toward).GetY())) < 0.9f ? Vector3(kYUnitVector) : Vector3(kXUnitVector))) : Vector3(kYUnitVector);
            const Vector3 x = Normalize(Cross(normal, std::abs(float(normal.GetY())) < 0.9f ? Vector3(kYUnitVector) : Vector3(kXUnitVector)));
            MeshCutting::TryBuildCutPlaneData({ 1000, 1000 }, AffineTransform(Matrix3(x, normal, Cross(x, normal)), Vector3(node.GetW())), plane);
            const bool accepted = cutting.Request(Owner, Attached, plane);
            Passed[3] = !accepted && !cutting.IsBusy() && physics.GetAmbiguousNodeSideCount() > rejected
                && owner->GetMeshComponents().size() == count;
            if (accepted)
            {
                Finish();
                return;
            }

            // 독립 조각은 현재 물리 pose에서 일반 Static 직접 요청 경로로 한 번 더 자른다.
            Count = count;
            Stage = 4;
            if (!Plane(*owner, *detached, nullptr, plane) || !cutting.Request(Owner, Detached, plane)) Finish();
        }
    };
}
