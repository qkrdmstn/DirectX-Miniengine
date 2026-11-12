#pragma once
#include "AttachedRecutChecks.h"
#include "../SkeletalPatchCopy.h"

namespace Viewer
{
    // D5 게시의 후속 요청 수명과 선택적 자동 진단을 함께 추적한다.
    struct RigBoneCutChecks
    {
        bool Enabled = false, Done = false, Passed[5]{};
        RigBoneCutPublication Pending{}, Active{};
        unsigned Wait = 0, Stage = 0, Observe = 0;
        ComponentId Attached = 0, Detached = 0;
        uint64_t Topology = 0;
        size_t Broken = 0, Dynamic = 0;
        float StartY = 0, MaxError = 0, NodeMove = 0, NodeRot = 0;
        Math::Matrix4 NodeStart{ Math::kIdentity };
        PhysicsSystem::QueuedBreakDiagnostics BreakBefore{};
        bool StateRead = false, FailureResult = false, FailureRejected = false;
        size_t ComponentCount = 0;
        uint64_t FailureRevision = 0, FailureEdit = 0;
        const NodeAttachment* FailureAttachment = nullptr;
        bool Body = false;
        std::vector<uint32_t> Mask;

        // 관절과 Dynamic body의 실제 상태를 같은 형식으로 읽는다.
        static bool RigState(PhysicsSystem& physics, ObjectId owner, size_t& broken, size_t& dynamic)
        {
            // 진단 읽기 실패는 정상 상태와 구분한다.
            SkeletalRigState state;
            if (!physics.GetSkeletalPhysics().ReadDiagnosticState(owner, state))
                return false;

            // 읽기에 성공한 상태에서만 두 카운터를 갱신한다.
            broken = 0;
            dynamic = 0;
            for (const auto& joint : state.Joints)
                broken += joint.Broken;
            for (const auto& body : state.Bodies)
                dynamic += body.Motion == MiniPhysics::BodyMotion::Dynamic;
            return true;
        }

        // 결과 경로와 동기 거절 모두 같은 게시 로그를 남긴다.
        void Result(Scene& scene, bool published, ComponentId other, const char* reason)
        {
            // 실제 Attachment 유무로 A/B의 역할을 확인한다.
            auto* owner = scene.FindObject(Active.Owner);
            auto* a = owner ? owner->FindMeshComponent(Active.Component) : nullptr;
            auto* b = owner ? owner->FindMeshComponent(other) : nullptr;
            Attached = published && b && b->Attachment ? other : Active.Component;
            Detached = published ? (Attached == other ? Active.Component : other) : 0;
            char line[512];
            sprintf_s(line, "RigBoneCut result owner=%llu node=%u published=%d attached=%llu detached=%llu reason=%s",
                Active.Owner, Active.Node, int(published), Attached, Detached, reason);
            AttachedRecutChecks::Log(line, published, false);

            // 실패 주입은 실제 요청 결과를 받은 뒤에만 보존 검사로 넘어간다.
            if (Enabled && Stage == 4)
            {
                FailureResult = true;
                FailureRejected &= !published;
                Active = {};
                return;
            }

            // 자동 진단은 게시 프레임의 낙하 기준점과 node pose를 보존한다.
            if (Enabled && Stage == 1)
            {
                Passed[0] = published && a && b && bool(a->Attachment) != bool(b->Attachment);
                if (!Passed[0])
                {
                    Finish();
                    return;
                }
                StartY = float(owner->FindMeshComponent(Detached)->GetWorldTransform(owner->GetTransform()).GetTranslation().GetY());
                if (!scene.TryGetAttachmentNodeWorld(*owner->FindMeshComponent(Attached), NodeStart))
                {
                    Finish();
                    return;
                }
                Stage = 2;
            }
            Active = {};
        }

        // 기존 완료 큐에서 추적 대상 하나만 기록한다.
        void OnResult(Scene& scene, const CutResultEntry& result)
        {
            // 다른 Static 및 break 결과는 추적하지 않는다.
            if (!Active.Component || result.Owner != Active.Owner || result.Mesh != Active.Component)
                return;
            char reason[200];
            sprintf_s(reason, "status=%u error=%u %s", unsigned(result.Status), result.Error, result.Reason ? result.Reason : "completed");
            Result(scene, result.Status == CutResultStatus::Published && !result.Error, result.Other, reason);
        }

        // 미완료 항목을 포함해 다섯 판정과 종료 결과를 기록한다.
        void Finish()
        {
            // 실측값을 임계값 판정과 같은 줄에 출력한다.
            char follow[220];
            sprintf_s(follow, "RBC attachment follows frames=%u maxError=%.6fm <=1mm nodeMove=%.6fm nodeRot=%.6fdeg%s",
                Observe, MaxError, NodeMove, NodeRot,
                NodeMove < 0.001f && NodeRot < 0.5f ? " stationary observation" : "");
            const char* labels[] = { "RBC D5 hidden=1 AR published=1 no break", follow,
                "RBC detached kdop fall >=1cm", "RBC joint <=1mm rejected unchanged", "RBC forced failure attached mask preserved" };
            bool pass = true;
            for (unsigned i = 0; i < 5; ++i)
            {
                AttachedRecutChecks::Log(labels[i], Passed[i]);
                pass &= Passed[i];
            }
            AttachedRecutChecks::Log("Rig bone cut checks", pass);
            Done = true;
        }

        // 후보 준비 뒤에만 요청하고 stale·상한·강제 실패에서는 게시물을 보존한다.
        void Submit(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting, SkeletalPatchCopy& patch)
        {
            // 이 호출 뒤 게시되는 패치는 다음 프레임 SyncColliders 이후에만 꺼낸다.
            if (!Pending.Component && !Active.Component && patch.TakeRigBoneCutPublication(Pending))
                Wait = 0;
            if (!Pending.Component)
                return;
            auto* owner = scene.FindObject(Pending.Owner);
            auto* mesh = owner ? owner->FindMeshComponent(Pending.Component) : nullptr;
            if (!mesh || mesh->GetEditGeneration() != Pending.EditGeneration || mesh->GetGeometry()->GetId() != Pending.Revision)
            {
                AttachedRecutChecks::Log("RigBoneCut skipped stale", false, false);
                Pending = {};
                return;
            }

            // 결과를 받기 전에는 같은 실패 요청을 다시 제출하지 않는다.
            if (Enabled && Stage == 4)
                return;

            // SyncColliders가 IsEligible을 통과시킨 현재 collider만 준비로 인정한다.
            bool ready = false;
            for (const auto& collider : owner->GetColliderComponents())
                ready |= collider.MeshId == Pending.Component && owner->IsColliderCurrent(collider.Id);
            if (ready && !cutting.IsBusy())
            {
                // 실제 AR 후보 조회에서 node 원점 평면의 모호 거절을 주입한다.
                if (Enabled && Stage == 1 && !Passed[4])
                {
                    Math::Matrix4 node;
                    MeshCutting::CutPlaneData failurePlane;
                    if (!scene.TryGetAttachmentNodeWorld(*mesh, node) ||
                        !MeshCutting::TryBuildCutPlaneData({ 1000, 1000 },
                            Math::AffineTransform(Math::Matrix3(Math::kIdentity), Math::Vector3(node.GetW())), failurePlane))
                    {
                        Finish();
                        return;
                    }
                    Mask = owner->GetMeshComponents().front()->GetTriangleMask()->Pending;
                    ComponentCount = owner->GetMeshComponents().size();
                    FailureRevision = mesh->GetGeometry()->GetId();
                    FailureEdit = mesh->GetEditGeneration();
                    FailureAttachment = mesh->Attachment.get();
                    Attached = Pending.Component;
                    Active = Pending;
                    Stage = 4;
                    const auto rejected = physics.GetAmbiguousNodeSideCount();
                    const bool accepted = cutting.Request(Active.Owner, Active.Component, failurePlane);
                    FailureRejected = !accepted && !cutting.IsBusy() &&
                        physics.GetAmbiguousNodeSideCount() == rejected + 1 &&
                        std::strcmp(cutting.GetStatusText(), "No eligible attached mesh in the cut area") == 0;

                    // 동기 거절 반환도 실제 AR 원인과 함께 결과 처리 함수로 전달한다.
                    char rejection[240];
                    sprintf_s(rejection, "RBC actual rejection nodeDistance=%.6fm ambiguous=%llu/%llu reason=%s",
                        float(Math::Dot(failurePlane.WorldPlane, node.GetW())), rejected,
                        physics.GetAmbiguousNodeSideCount(), cutting.GetStatusText());
                    AttachedRecutChecks::Log(rejection, FailureRejected);
                    if (!accepted)
                        Result(scene, false, 0, cutting.GetStatusText());
                    return;
                }

                // 실패 보존 검사가 끝났으면 원래 평면을 정상 요청한다.
                Active = Pending;
                Pending = {};
                if (!cutting.Request(Active.Owner, Active.Component, Active.Plane))
                    Result(scene, false, 0, cutting.GetStatusText());
                return;
            }
            if (++Wait >= 10)
            {
                AttachedRecutChecks::Log("RigBoneCut request timeout", false, false);
                Pending = {};
            }
        }

        // 실제 Rig C·60프레임 추종·낙하·거절·실패 보존을 순차 검사한다.
        void Tick(Scene& scene, PhysicsSystem& physics, SkeletalPatchCopy& patch, unsigned frame)
        {
            // 준비 실패와 GPU 대기의 무한 반복을 제한한다.
            using namespace Math;
            if (!Enabled || Done)
                return;
            if (frame > 1200)
            {
                AttachedRecutChecks::Log("RigBoneCut result timeout", false, false);
                Finish();
                return;
            }
            GameObject* owner = nullptr;
            for (const auto& object : scene.GetObjects())
            {
                if (object->GetName() == "Xbot")
                    owner = object.get();
            }
            if (!owner || frame < 60)
                return;
            auto& skeletal = physics.GetSkeletalPhysics();
            MeshCutting::CutPlaneData plane;
            if (Stage == 0)
            {
                // 두 C 요청 이전의 상태 읽기 성공 여부와 break 처리 누적값을 보존한다.
                StateRead = RigState(physics, owner->GetId(), Broken, Dynamic);
                BreakBefore = physics.GetQueuedBreakDiagnostics();
                Topology = skeletal.GetTopologyGeneration(owner->GetId());

                // joint 원점 요청이 실제 D5 모호 거절이며 원본 상태가 불변인지 확인한다.
                const auto count = owner->GetMeshComponents().size();
                auto* source = owner->GetMeshComponents().front().get();
                const auto* mask = source->GetTriangleMask();
                const auto revision = mask ? mask->Revision : 0;
                if (!BuildRigBoneCutPlane(scene, physics, plane, true))
                {
                    Finish();
                    return;
                }
                patch.Request(scene, plane, false, SkeletalJointMode::Rig, UINT32_MAX, &physics);
                Passed[3] = !patch.IsPending() && count == owner->GetMeshComponents().size()
                    && revision == (source->GetTriangleMask() ? source->GetTriangleMask()->Revision : 0);

                // 실제 머리 중간 절단을 요청한다.
                if (!BuildRigBoneCutPlane(scene, physics, plane, false))
                {
                    Finish();
                    return;
                }
                patch.Request(scene, plane, false, SkeletalJointMode::Rig, UINT32_MAX, &physics);
                Stage = 1;
            }
            if (Stage == 2)
            {
                // AR4와 같은 오차·낙하 함수를 현재 node 및 body에 적용한다.
                auto* a = owner->FindMeshComponent(Attached);
                auto* b = owner->FindMeshComponent(Detached);
                if (!a || !b)
                {
                    Finish();
                    return;
                }
                const float error = AttachedRecutChecks::FollowError(scene, *owner, *a);
                MaxError = std::isfinite(error) ? (std::max)(MaxError, error) : FLT_MAX;
                Body |= physics.GetBodyState(owner->GetId(), Detached) != PhysicsSystem::BodyState::None;

                // 게시 시점 대비 node 위치와 회전의 최대 변화량을 함께 측정한다.
                Matrix4 node;
                if (!scene.TryGetAttachmentNodeWorld(*a, node))
                {
                    Finish();
                    return;
                }
                NodeMove = (std::max)(NodeMove, float(Length(Vector3(node.GetW()) - Vector3(NodeStart.GetW()))));
                const float trace = float(Dot(Normalize(Vector3(node.GetX())), Normalize(Vector3(NodeStart.GetX()))))
                    + float(Dot(Normalize(Vector3(node.GetY())), Normalize(Vector3(NodeStart.GetY()))))
                    + float(Dot(Normalize(Vector3(node.GetZ())), Normalize(Vector3(NodeStart.GetZ()))));
                const float angle = std::acos((std::max)(-1.0f, (std::min)(1.0f, (trace - 1.0f) * 0.5f))) * 57.2957795f;
                NodeRot = (std::max)(NodeRot, angle);
                if (++Observe < 60)
                    return;
                Passed[1] = std::isfinite(MaxError) && MaxError <= 0.001f && (NodeMove >= 0.001f || NodeRot >= 0.5f);

                // 게시 순간 대비 낙하와 rig의 불변 상태를 판정한다.
                const float drop = StartY - float(b->GetWorldTransform(owner->GetTransform()).GetTranslation().GetY());
                Passed[2] = AttachedRecutChecks::Falls(std::strcmp(b->GetCollisionReason(), "detached kdop") == 0,
                    Body, b->Attachment != nullptr, physics.IsSimulationRunning(), drop);
                size_t broken = 0, dynamic = 0;
                const bool stateRead = RigState(physics, owner->GetId(), broken, dynamic);
                const auto breaks = physics.GetQueuedBreakDiagnostics();
                Passed[0] &= StateRead && stateRead && broken == Broken && dynamic == Dynamic
                    && Topology == skeletal.GetTopologyGeneration(owner->GetId())
                    && breaks.Processed == BreakBefore.Processed && breaks.NotFound == BreakBefore.NotFound;
                char detail[240];
                sprintf_s(detail, "RBC actual drop=%.6fm read=%d/%d broken=%zu/%zu dynamic=%zu/%zu breakProcessed=%llu/%llu",
                    drop, int(StateRead), int(stateRead), Broken, broken, Dynamic, dynamic, BreakBefore.Processed, breaks.Processed);
                AttachedRecutChecks::Log(detail, Passed[0] && Passed[2]);
                Finish();
                return;
            }
            if (Stage == 4)
            {
                // 실제 요청 결과가 없으면 전역 timeout까지 기다린다.
                if (!FailureResult)
                    return;

                // 컴포넌트 수와 부착 snapshot 및 숨긴 삼각형 마스크 전체를 비교한다.
                const auto* a = owner->FindMeshComponent(Attached);
                const auto* mask = owner->GetMeshComponents().front()->GetTriangleMask();
                size_t hidden = 0, hiddenAfter = 0;
                for (uint32_t value : Mask)
                    hidden += (value & 0x80000000u) != 0;
                if (mask)
                {
                    for (uint32_t value : mask->Pending)
                        hiddenAfter += (value & 0x80000000u) != 0;
                }
                Passed[4] = FailureRejected && a && a->Attachment.get() == FailureAttachment && a->PublishVisible
                    && a->GetGeometry()->GetId() == FailureRevision && a->GetEditGeneration() == FailureEdit
                    && owner->GetMeshComponents().size() == ComponentCount && mask && hidden > 0
                    && hidden == hiddenAfter && mask->Pending == Mask;
                char preserved[160];
                sprintf_s(preserved, "RBC failure preserved components=%zu/%zu hiddenTriangles=%zu/%zu",
                    ComponentCount, owner->GetMeshComponents().size(), hidden, hiddenAfter);
                AttachedRecutChecks::Log(preserved, Passed[4]);
                if (!Passed[4])
                {
                    Finish();
                    return;
                }
                Stage = 1;
            }
        }
    };
}
