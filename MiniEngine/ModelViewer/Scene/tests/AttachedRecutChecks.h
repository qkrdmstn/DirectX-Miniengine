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
        bool Follows = true, Body = false, Collision = false, NodePositive = false;

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
            // 진단 readback은 상태 전이만 하므로 공유 revision 버퍼의 const를 제거한다.
            auto& data = const_cast<ByteAddressBuffer&>(geometry.GetGenerated()->Data);
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
                for (size_t v = 0; v < vb.vbSize / vb.vbStride; ++v)
                {
                    DirectX::XMFLOAT3 p;
                    std::memcpy(&p, bytes + vb.vbOffset + v * vb.vbStride, sizeof(p));
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

        // AR과 RBC가 같은 node 상대 원점 오차를 측정한다.
        static float FollowError(Scene& scene, const GameObject& owner, const MeshComponent& mesh)
        {
            // 현재 node와 게시 snapshot으로 기대 월드 원점을 복원한다.
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
            if (!Enabled || Done || result.Owner != Owner || result.Mesh != (Stage == 4 ? Detached : Input)) return;
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
                StartY = float(detached->GetWorldTransform(owner->GetTransform()).GetTranslation().GetY());
                Stage = 2;
            }
            else if (Stage == 4)
            {
                Passed[4] = published && !a->Attachment && !b->Attachment && owner->GetMeshComponents().size() == Count + 1;
                Finish();
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
            const float error = FollowError(scene, *owner, *attached);
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
