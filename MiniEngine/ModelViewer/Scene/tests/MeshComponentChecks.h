#pragma once

#include "../Scene.h"
#include "Renderer.h"
#include "Utility.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

namespace Viewer
{
    // Release에서도 실패를 숨기지 않는 GPU 통합 진단의 조건을 검사한다.
    inline void CheckComponent(bool condition, const char* message)
    {
        // 마지막 성공 조건도 추적할 수 있도록 모든 검사 결과를 즉시 파일에 남긴다.
        std::ofstream log("MeshComponentChecks.log", std::ios::app);
        log << (condition ? "PASS" : "FAIL") << " condition=" << condition << ": " << message << std::endl;

        // 실패하면 기록을 마친 뒤 예외를 전달한다.
        if (!condition)
            throw std::runtime_error(message);
    }

    // 선택 section이 모두 Generated일 때만 Model hull 부피 합과 convex·정점 수를 센다. 하나라도 아니면 모두 0이다.
    inline void SumModelHulls(const GeometryRevision& revision, double& volume, size_t& convex, size_t& vertices)
    {
        // 합계를 비운다.
        volume = 0.0;
        convex = vertices = 0;

        // 선택 section의 hull을 누적하고, Generated가 아닌 section을 만나면 전체를 0으로 되돌린다.
        for (const GeometrySection& section : revision.GetSections())
        {
            const Model& model = *revision.GetSource();
            if (model.GetMeshCollisionStatus(section.SourceMesh) != MeshCollisionStatus::Generated)
            {
                volume = 0.0;
                convex = vertices = 0;
                return;
            }
            for (const MiniCollision::ConvexElem& elem : model.GetMeshCollision(section.SourceMesh).ConvexElems)
            {
                volume += elem.GetVolume();
                ++convex;
                vertices += elem.VertexData.size();
            }
        }
    }

    // 모든 hull 정점을 SourceFrame 역변환(켤레 쿼터니언 회전 후 크기 나눗셈, double)과 비교한다. 허용오차는 extent 대비 1e-9다.
    inline void CheckCollisionAgainstFrame(const MeshComponent& mesh, const SceneTransform& frame)
    {
        // 프레임의 TRS를 double로 꺼낸다.
        DirectX::XMFLOAT4 q;
        DirectX::XMFLOAT3 s, t;
        XMStoreFloat4(&q, frame.GetRotation());
        XMStoreFloat3(&s, frame.GetScale());
        XMStoreFloat3(&t, frame.GetTranslation());
        const double n = std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
        const double u[3] = { -q.x / n, -q.y / n, -q.z / n };
        const double w = q.w / n;

        // 선택 section의 Model hull을 같은 순서로 순회하며 컴포넌트 정점과 대조한다.
        const Model& model = *mesh.GetGeometry()->GetSource();
        const auto& elems = mesh.GetCollision().ConvexElems;
        size_t index = 0;
        for (const GeometrySection& section : mesh.GetGeometry()->GetSections())
        {
            if (model.GetMeshCollisionStatus(section.SourceMesh) != MeshCollisionStatus::Generated)
                continue;
            for (const MiniCollision::ConvexElem& source : model.GetMeshCollision(section.SourceMesh).ConvexElems)
            {
                CheckComponent(index < elems.size() && elems[index].VertexData.size() == source.VertexData.size(), "Frame hull shape");
                double extent = 1e-30;
                for (size_t i = 0; i < source.VertexData.size(); ++i)
                {
                    // p - T를 켤레 쿼터니언으로 회전한다: v' = v + 2w(u×v) + 2u×(u×v).
                    const double v[3] = { source.VertexData[i].x - t.x, source.VertexData[i].y - t.y, source.VertexData[i].z - t.z };
                    const double c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
                    const double c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
                    const double expected[3] = { (v[0] + 2 * (w * c1[0] + c2[0])) / s.x,
                        (v[1] + 2 * (w * c1[1] + c2[1])) / s.y, (v[2] + 2 * (w * c1[2] + c2[2])) / s.z };
                    const MiniCollision::Vec3d& actual = elems[index].VertexData[i];
                    extent = std::max({ extent, std::abs(expected[0]), std::abs(expected[1]), std::abs(expected[2]) });
                    CheckComponent(std::abs(actual.x - expected[0]) <= 1e-9 * extent
                        && std::abs(actual.y - expected[1]) <= 1e-9 * extent
                        && std::abs(actual.z - expected[2]) <= 1e-9 * extent, "Frame hull vertex");
                }
                ++index;
            }
        }
        CheckComponent(index == elems.size(), "Frame hull count");
    }

    // 컴포넌트 충돌이 Model hull과 일치하고 항상 현재 revision에 대응하는지 검사한다.
    inline void CheckComponentCollision(const MeshComponent& mesh, double scaleVolume, double tolerance)
    {
        // 현재 revision의 Model hull 합계를 구한다.
        double volume = 0.0;
        size_t convex = 0, vertices = 0;
        SumModelHulls(*mesh.GetGeometry(), volume, convex, vertices);

        // revision 대응과 상태를 확인한다.
        CheckComponent(mesh.GetCollisionRevisionId() == mesh.GetGeometry()->GetId(), "Collision revision id");
        if (convex == 0)
        {
            CheckComponent(mesh.GetCollisionStatus() == ComponentCollisionStatus::Unavailable
                && mesh.GetCollision().ConvexElems.empty(), "Collision unavailable without model hull");
            return;
        }
        CheckComponent(mesh.GetCollisionStatus() == ComponentCollisionStatus::FromModel, "Collision from model");
        CheckComponent(mesh.GetCollision().ConvexElems.size() == convex, "Collision convex count");

        // 부피와 정점 수가 기대값과 일치하는지 확인한다.
        double actual = 0.0;
        size_t actualVertices = 0;
        for (const auto& elem : mesh.GetCollision().ConvexElems)
        {
            actual += elem.GetVolume();
            actualVertices += elem.VertexData.size();
        }
        CheckComponent(std::abs(actual - volume * scaleVolume) <= tolerance * volume * scaleVolume, "Collision volume");
        CheckComponent(actualVertices == vertices, "Collision vertex count");
    }

    // 가짜 상태 목록으로 혼합·범위 밖·정상 section 선택의 all-or-nothing 결과를 검사한다. Generated hull이 없는 모델은 건너뛴다.
    inline void CheckMixedSectionStatus(const Model& model)
    {
        // 첫 Generated hull을 찾아 두 메시의 hull로 복제한다.
        const MiniCollision::AggregateGeom* hull = nullptr;
        for (uint32_t i = 0; i < model.m_NumMeshes && !hull; ++i)
            if (model.GetMeshCollisionStatus(i) == MeshCollisionStatus::Generated)
                hull = &model.GetMeshCollision(i);
        if (!hull)
            return;
        const std::vector<MiniCollision::AggregateGeom> hulls = { *hull, *hull };
        const double identity[3][4] = { {1,0,0,0}, {0,1,0,0}, {0,0,1,0} };
        MiniCollision::AggregateGeom out;
        std::string reason;

        // 모두 Generated이면 두 hull이 모인다.
        const std::vector<MeshCollisionStatus> allGenerated = { MeshCollisionStatus::Generated, MeshCollisionStatus::Generated };
        CheckComponent(GatherSectionHulls(allGenerated, hulls, { {0, 0}, {1, 0} }, identity, out, reason)
            && out.ConvexElems.size() == 2 * hull->ConvexElems.size(), "All generated sections gathered");

        // 스킨 section이 하나라도 섞이면 결과가 비고 이유에 section과 상태가 있어야 한다.
        const std::vector<MeshCollisionStatus> mixed = { MeshCollisionStatus::Generated, MeshCollisionStatus::Skinned };
        CheckComponent(!GatherSectionHulls(mixed, hulls, { {0, 0}, {1, 0} }, identity, out, reason)
            && out.ConvexElems.empty() && reason.find("section 1") != std::string::npos
            && reason.find("Skinned") != std::string::npos, "Mixed sections unavailable");

        // 범위 밖 section도 결과를 비운다.
        CheckComponent(!GatherSectionHulls(mixed, hulls, { {0, 0}, {5, 0} }, identity, out, reason)
            && out.ConvexElems.empty(), "Out of range section unavailable");
    }

    // 편집·물리 pose 두 세대의 증가 규칙을 setter별로 검사한다. attached는 Attached, independent는 WorldIndependent여야 한다.
    inline void CheckGenerationRules(GameObject& object, MeshComponent& attached, MeshComponent& independent)
    {
        using namespace Math;

        // 편집·물리 세대가 기준값에서 각각 edit, pose만큼 늘었는지 확인하는 도우미다.
        auto expect = [](const MeshComponent& mesh, uint64_t edit, uint64_t pose, uint64_t baseEdit, uint64_t basePose, const char* message)
        {
            CheckComponent(mesh.GetEditGeneration() == baseEdit + edit && mesh.GetPhysicsPoseGeneration() == basePose + pose, message);
        };

        // 검사가 이후 장면을 바꾸지 않도록 원래 배치를 기억한다.
        const SceneTransform original = independent.GetTransform();

        // 명시적 배치·source frame·geometry 교체는 편집 세대만 올린다.
        uint64_t e = independent.GetEditGeneration(), p = independent.GetPhysicsPoseGeneration();
        SceneTransform placed = independent.GetTransform();
        placed.SetScale(placed.GetScale() * Vector3(1.0f, 1.5f, 1.0f));
        CheckComponent(independent.SetTransform(placed, MeshTransformMode::WorldIndependent), "Generation SetTransform");
        expect(independent, 1, 0, e, p, "SetTransform bumps edit only");
        e = independent.GetEditGeneration();
        CheckComponent(independent.SetDeformBinding(independent.GetDeformBinding()), "Generation SetDeformBinding");
        expect(independent, 1, 0, e, p, "SetDeformBinding bumps edit only");
        e = independent.GetEditGeneration();
        CheckComponent(independent.SetGeometry(independent.GetGeometry()), "Generation SetGeometry");
        expect(independent, 1, 0, e, p, "SetGeometry bumps edit only");

        // 실패한 설정은 어느 세대도 올리지 않는다.
        e = independent.GetEditGeneration();
        SceneTransform invalid;
        invalid.SetScale(Vector3(0.0f, 1.0f, 1.0f));
        CheckComponent(!independent.SetTransform(invalid, MeshTransformMode::WorldIndependent), "Generation invalid TRS");
        expect(independent, 0, 0, e, p, "Rejected setter keeps generations");

        // SetPhysicsPose는 물리 세대만 올리고, 같은 pose는 올리지 않으며, 크기를 보존한다.
        const Vector3 scale = independent.GetTransform().GetScale();
        const Vector3 position = independent.GetTransform().GetTranslation() + Vector3(0.0f, 0.25f, 0.0f);
        const Quaternion rotation = Quaternion(Vector3(kYUnitVector), 0.3f);
        CheckComponent(independent.SetPhysicsPose(position, rotation), "SetPhysicsPose");
        expect(independent, 0, 1, e, p, "SetPhysicsPose bumps pose only");
        p = independent.GetPhysicsPoseGeneration();
        CheckComponent(independent.SetPhysicsPose(position, rotation), "SetPhysicsPose same");
        expect(independent, 0, 0, e, p, "Same pose keeps generations");
        CheckComponent(float(Length(independent.GetTransform().GetScale() - scale)) == 0.0f, "Pose keeps scale");
        const Vector3 point(0.3f, 0.7f, -0.2f);
        const Vector3 world = independent.GetWorldTransform(object.GetTransform()) * point;
        const Vector3 manual = AffineTransform(Matrix3(rotation) * Matrix3::MakeScale(scale), position) * point;
        CheckComponent(float(Length(world - manual)) < 0.0001f, "Pose world is T*R*S");

        // Attached에는 물리 pose를 적용할 수 없고 세대도 그대로여야 한다.
        const uint64_t ae = attached.GetEditGeneration(), ap = attached.GetPhysicsPoseGeneration();
        CheckComponent(!attached.SetPhysicsPose(position, rotation), "Attached rejects physics pose");
        expect(attached, 0, 0, ae, ap, "Rejected pose keeps generations");

        // 같은 pose를 다른 경로로 만든 Vector3(XMFLOAT3)로 넣어도 세대가 오르지 않는다.
        DirectX::XMFLOAT3 raw;
        XMStoreFloat3(&raw, position);
        CheckComponent(independent.SetPhysicsPose(Vector3(DirectX::XMFLOAT3(raw.x, raw.y, raw.z)), rotation), "SetPhysicsPose rebuilt");
        expect(independent, 0, 0, e, p, "Rebuilt same pose keeps generations");

        // owner 편집은 어느 컴포넌트의 편집 세대도 올리지 않는다(Attached는 world 행렬 비교로 검출).
        const SceneTransform ownerBefore = object.GetTransform();
        e = independent.GetEditGeneration();
        SceneTransform moved = ownerBefore;
        moved.SetTranslation(ownerBefore.GetTranslation() + Vector3(1.0f, 0.0f, 0.0f));
        CheckComponent(object.SetTransform(moved), "Owner edit");
        expect(attached, 0, 0, ae, ap, "Owner edit keeps Attached generation");
        expect(independent, 0, 0, e, p, "Owner edit ignored by WorldIndependent");
        CheckComponent(object.SetTransform(ownerBefore), "Owner restore");
        expect(attached, 0, 0, ae, ap, "Owner A-B-A keeps generations");
        CheckComponent(independent.SetTransform(original, MeshTransformMode::WorldIndependent), "Placement restore");
    }

    // ID·revision·collider·변환 계약을 검사하고 같은 owner 아래 두 번째 메시를 배치한다.
    inline void PrepareMeshComponentChecks(Scene& scene)
    {
        // 진단 시작 시 이전 실행 로그를 비우고 시작 지점을 즉시 기록한다.
        {
            std::ofstream log("MeshComponentChecks.log", std::ios::trunc);
            log << "MeshComponent checks: begin" << std::endl;
        }

        using namespace Math;
        size_t checked = 0;
        for (const auto& object : scene.GetObjects())
        {
            if (!object->GetModelInstance())
                continue;

            // owner의 첫 컴포넌트와 원본 revision을 잡고 소유권을 확인한다.
            const ModelInstance* pose = object->GetModelInstance();
            MeshComponent& first = *object->GetMeshComponents().front();
            const auto original = first.GetGeometry();
            const ComponentId firstId = first.GetId();
            CheckComponent(original->GetSource() == pose->GetModel(), "Source ownership");

            // collider 대응과 초기 충돌 상태를 확인한다.
            const ComponentId colliderId = object->AddColliderComponent(firstId).Id;
            CheckComponent(object->IsColliderCurrent(colliderId), "Initial collider revision");
            CheckComponentCollision(first, 1.0, 1e-9);

            // 빈 revision으로 바꾸면 collider가 낡고 hull이 남지 않아야 한다.
            const auto empty = original->SelectSections({});
            CheckComponent(empty->GetId() != original->GetId(), "Revision identity");
            CheckComponent(first.SetGeometry(empty), "Geometry replacement");
            CheckComponent(!object->IsColliderCurrent(colliderId), "Stale collider rejected");
            CheckComponent(first.GetCollisionStatus() == ComponentCollisionStatus::Unavailable
                && first.GetCollision().ConvexElems.empty(), "Empty revision has no stale hull");

            // 원본으로 복원하면 collider와 충돌이 다시 현재 revision에 대응해야 한다.
            CheckComponent(first.SetGeometry(original), "Geometry snapshot restore");
            CheckComponentCollision(first, 1.0, 1e-9);
            if (original->GetSections().size() > 1)
            {
                const auto subset = original->SelectSections({ uint32_t(original->GetSections().size() - 1) });
                CheckComponent(first.SetGeometry(subset), "Subset geometry");
                CheckComponentCollision(first, 1.0, 1e-9);
                CheckComponent(first.SetGeometry(original), "Subset restore");
            }

            // 원본 → 생성 → 생성 부분집합 → 원본 순서로 바꿔도 생성 revision은 hull이 없고 원본은 정확히 복원되어야 한다.
            auto buffers = std::make_shared<GeometryBuffers>();
            buffers->Source = original->GetSource();
            const auto generated = GeometryRevision::FromGenerated(buffers);
            for (const auto& revision : { generated, generated->SelectSections({}) })
            {
                CheckComponent(first.SetGeometry(revision), "Generated geometry");
                CheckComponent(first.GetCollisionStatus() == ComponentCollisionStatus::Unavailable
                    && first.GetCollision().ConvexElems.empty()
                    && first.GetCollisionRevisionId() == revision->GetId(), "Generated revision has no hull");
            }
            CheckComponent(first.SetGeometry(original), "Generated restore");
            CheckComponentCollision(first, 1.0, 1e-9);
            CheckCollisionAgainstFrame(first, SceneTransform());

            // 정적·스킨 section이 섞이면 하나의 강체가 될 수 없으므로 전체가 Unavailable이어야 한다(실제 혼합 모델이 없어 가짜 상태 목록으로 검사한다).
            CheckMixedSectionStatus(*original->GetSource());
            CheckComponent(object->IsColliderCurrent(colliderId), "Restored collider revision");
            CheckComponent(object->RemoveColliderComponent(colliderId), "Collider removal");


            // 컴포넌트 제거와 ID 비재사용, 독립 pose 소유권을 확인한다.
            const ComponentId removed = object->AddMeshComponent(original).GetId();
            const ComponentId dependent = object->AddColliderComponent(removed).Id;
            CheckComponent(object->RemoveMeshComponent(removed), "Mesh removal");
            CheckComponent(!object->FindMeshComponent(removed) && !object->IsColliderCurrent(dependent), "Removed references");
            CheckComponent(!object->RemoveMeshComponent(removed), "Repeated removal rejected");
            MeshComponent& second = object->AddMeshComponent(original);
            CheckComponent(second.GetId() > dependent, "Component IDs not reused");
            CheckComponent(object->GetModelInstance() == pose, "One animator per owner");
            CheckComponent(&first.GetPosedGeometry() != &second.GetPosedGeometry(), "Independent posed ownership");


            // 잘못된 TRS와 배치의 원자성을 확인한다.
            SceneTransform invalid;
            invalid.SetScale(Vector3(0.0f, 1.0f, 1.0f));
            CheckComponent(!second.SetTransform(invalid, MeshTransformMode::Attached), "Invalid TRS rejected");
            DeformBinding binding;
            binding.SourceFrame = SceneTransform(Quaternion(Vector3(kYUnitVector), 0.35f),
                Vector3(1.2f, 0.7f, 1.1f), Vector3(0.5f, 0.3f, 0.0f));
            binding.Pose = PoseMode::Frozen;
            CheckComponent(!second.SetPlacement(invalid, MeshTransformMode::Attached, binding), "Placement validation");
            CheckComponent(second.GetTransformMode() == MeshTransformMode::Attached, "Failed placement is atomic");
            CheckComponent(second.SetPlacement(binding.SourceFrame, MeshTransformMode::Attached, binding), "Source placement");


            // 비항등 SourceFrame의 hull은 Model hull × SourceFrame⁻¹이며 부피는 1/(sx·sy·sz)배다.
            const Vector3 sourceScale = binding.SourceFrame.GetScale();
            CheckComponentCollision(second, 1.0 / (double(sourceScale.GetX()) * double(sourceScale.GetY()) * double(sourceScale.GetZ())), 1e-4);
            CheckCollisionAgainstFrame(second, binding.SourceFrame);

            // 같은 revision에서 SourceFrame을 두 번 바꿔도 누적 없이 마지막 값만 반영되어야 한다.
            DeformBinding other = binding;
            other.SourceFrame = SceneTransform(Quaternion(Vector3(kXUnitVector), -0.6f),
                Vector3(0.8f, 1.5f, 0.9f), Vector3(-1.0f, 0.25f, 2.0f));
            CheckComponent(second.SetDeformBinding(other), "Second source frame");
            CheckCollisionAgainstFrame(second, other.SourceFrame);
            CheckComponent(second.SetDeformBinding(binding), "Source frame restore");
            CheckCollisionAgainstFrame(second, binding.SourceFrame);

            // SourceFrame이 월드 배치에서 정확히 한 번만 상쇄되는지 확인한다.
            const Vector3 point(0.3f, 0.7f, -0.2f);
            const Vector3 expected = object->GetTransform().ToAffine() * point;
            const Vector3 actual = second.GetWorldTransform(object->GetTransform())
                * Vector3(Invert(binding.SourceFrame.ToAffine()) * point);
            CheckComponent(float(Length(actual - expected)) < 0.001f, "Source frame cancels once");

            // 기본 장면 owner는 균일 크기이므로 이 합성에는 전단이 없다.
            SceneTransform target(object->GetTransform().GetRotation() * binding.SourceFrame.GetRotation(),
                object->GetTransform().GetScale() * binding.SourceFrame.GetScale(),
                object->GetTransform().ToAffine() * binding.SourceFrame.GetTranslation() + Vector3(0.0f, 4.5f, 0.0f));
            CheckComponent(second.SetPlacement(target, MeshTransformMode::WorldIndependent, binding), "Detached placement");
            SceneTransform changed = object->GetTransform();
            changed.SetTranslation(Vector3(100.0f, 20.0f, -30.0f));
            const Vector3 independent = second.GetWorldTransform(changed) * point;
            const Vector3 unchanged = second.GetWorldTransform(object->GetTransform()) * point;
            CheckComponent(float(Length(independent - unchanged)) < 0.0001f, "Detached ignores owner");

            // 두 세대의 증가 규칙을 검사한다.
            CheckGenerationRules(*object, first, second);

            // 두 컴포넌트에 collider를 붙여 이후 프레임 검사를 준비한다.
            object->AddColliderComponent(firstId);
            object->AddColliderComponent(second.GetId());
            ++checked;
        }
        Utility::Printf("MeshComponent checks: structure passed, owners=%zu\n", checked);
        // CPU 구조 검사가 끝났음을 영속 로그에 기록한다.
        std::ofstream log("MeshComponentChecks.log", std::ios::app);
        log << "PASS structure owners=" << checked << std::endl;
    }

    // node pose와 position·normal 팔레트의 모든 성분을 비교 가능한 값으로 복사한다.
    inline std::vector<float> CaptureComponentPose(const ModelInstance& pose)
    {
        // 행렬을 저장하여 SIMD 패딩을 제외한 성분만 순서대로 누적한다.
        std::vector<float> values;
        const auto append = [&values](const Math::Matrix4& matrix)
        {
            // 각 행렬의 열과 행 순서를 유지한다.
            DirectX::XMFLOAT4X4 stored;
            DirectX::XMStoreFloat4x4(&stored, matrix);
            for (unsigned row = 0; row < 4; ++row)
                for (unsigned col = 0; col < 4; ++col)
                    values.push_back(stored.m[row][col]);
        };

        // 전체 node와 모든 팔레트 entry를 빠짐없이 복사한다.
        const auto& model = pose.GetModel();
        for (uint32_t node = 0; node < model->m_NumNodes; ++node)
            append(pose.GetNodeTransforms()[node]);
        for (uint32_t joint = 0; joint < model->m_NumJoints; ++joint)
        {
            // normal 행렬도 세 축 모두 비교하도록 확장한다.
            append(pose.GetJoints()[joint].posXform);
            append(Math::Matrix4(pose.GetJoints()[joint].nrmXform));
        }
        return values;
    }

    // 같은 초기 상태의 기준 wrapper와 Scene 분리 경로의 전체 출력을 비교한다.
    inline void CheckScenePoseEquivalence(const Scene& source, GraphicsContext& context)
    {
        // 실제 장면의 모델을 공유하되 애니메이션 상태와 출력은 새로 소유한다.
        Scene split;
        std::vector<std::unique_ptr<ModelInstance>> references;
        for (const auto& object : source.GetObjects())
        {
            // 모델 없는 객체는 비교 대상에서 제외한다.
            if (!object->GetModelInstance())
                continue;
            const auto model = object->GetModelInstance()->GetModel();
            GameObject& owner = split.CreateObject(object->GetName(), model);
            references.push_back(std::make_unique<ModelInstance>(model));
            if (model->m_NumAnimations > 0)
            {
                // 두 경로 모두 같은 클립을 같은 시점에서 반복 재생한다.
                owner.PlayAnimation(0, true);
                references.back()->PlayAnimation(0, true);
            }
        }

        // 연속된 서로 다른 dt로 누락이나 중복 시간 진행을 검사한다.
        const float steps[] = { 0.0f, 1.0f / 60.0f, 1.0f / 30.0f, 0.25f };
        CheckComponent(!references.empty(), "BC-03b reference owners required");
        for (float dt : steps)
        {
            // 기존 ModelInstance wrapper와 Scene의 CPU 단계를 같은 dt로 진행한다.
            for (const auto& reference : references)
                reference->Update(dt);
            split.EvaluateAnimations(dt);
            CheckComponent(split.FinalizePoses(), "BC-03b reference finalize");

            // GPU 준비 직전의 전체 출력이 기준 wrapper와 정확히 같아야 한다.
            std::vector<std::vector<float>> before;
            for (size_t i = 0; i < references.size(); ++i)
            {
                // owner 순서대로 독립 snapshot을 저장한다.
                before.push_back(CaptureComponentPose(*split.GetObjects()[i]->GetModelInstance()));
                CheckComponent(before.back() == CaptureComponentPose(*references[i]),
                    "BC-03b wrapper/split node and position/normal palettes");
            }

            // GPU 준비가 node와 두 팔레트를 변경하지 않아야 한다.
            split.PrepareMeshes(context);
            for (size_t i = 0; i < references.size(); ++i)
                CheckComponent(before[i] == CaptureComponentPose(*split.GetObjects()[i]->GetModelInstance()),
                    "BC-03b PrepareMeshes preserves all pose outputs");
        }

        // 임시 Scene의 버퍼를 해제하기 전에 기록된 GPU 명령을 제출하고 완료를 기다린다.
        context.Flush(true);
    }

    // Scene의 분리 평가와 override 전달 및 GPU 준비의 pose 보존을 검사한다.
    inline void CheckScenePosePhases(Scene& scene, GraphicsContext& context)
    {
        // 초기 상태가 같은 별도 인스턴스로 시간 진행과 GPU 준비의 무변경을 검사한다.
        CheckScenePoseEquivalence(scene, context);

        // 모델 없는 객체와 삭제된 ID는 최종 pose 전달을 거절해야 한다.
        const ObjectId emptyId = scene.CreateObject("BC-03b empty").GetId();
        CheckComponent(!scene.FinalizeObjectPose(emptyId, nullptr, 0), "BC-03b empty owner rejected");
        CheckComponent(scene.DeleteObject(emptyId), "BC-03b delete empty owner");
        CheckComponent(!scene.FinalizeObjectPose(emptyId, nullptr, 0), "BC-03b deleted ID rejected");

        // 여러 시간 간격에서 Scene의 분리 평가와 최종 확정 순서를 검사한다.
        const float steps[] = { 0.0f, 1.0f / 60.0f, 1.0f / 30.0f };
        for (float dt : steps)
        {
            // 현재 애니메이션 상태를 복제하지 않고 다음 평가의 phase 유효성을 검사한다.
            scene.EvaluateAnimations(dt);
            for (const auto& object : scene.GetObjects())
                if (object->GetModelInstance())
                    CheckComponent(!object->GetModelInstance()->HasValidPose(), "BC-03b unfinalized pose");
            CheckComponent(scene.FinalizePoses(), "BC-03b finalize all owners");

            // GPU 준비는 확정된 node와 팔레트를 재평가하거나 애니메이션 시간을 진행하지 않는다.
            for (const auto& object : scene.GetObjects())
            {
                // 각 owner의 애니메이션 root를 이동한 pose로 최종 확정한다.
                const ModelInstance* pose = object->GetModelInstance();
                if (!pose)
                    continue;
                const NodePhysicsPose overridePose = { 0, Math::Matrix4(Math::Matrix3(Math::kIdentity),
                    Math::Vector3(1.0f, 2.0f, 3.0f)) * pose->GetNodeTransforms()[0] };
                CheckComponent(scene.FinalizeObjectPose(object->GetId(), &overridePose, 1), "BC-03b owner override");
                const Math::Matrix4 expected = pose->GetNodeTransforms()[0];
                scene.PrepareMeshes(context);
                DirectX::XMFLOAT4X4 actualValues, expectedValues;
                DirectX::XMStoreFloat4x4(&actualValues, pose->GetNodeTransforms()[0]);
                DirectX::XMStoreFloat4x4(&expectedValues, expected);
                for (unsigned row = 0; row < 4; ++row)
                    for (unsigned col = 0; col < 4; ++col)
                        CheckComponent(actualValues.m[row][col] == expectedValues.m[row][col],
                            "BC-03b GPU preparation preserves override");

                // 잘못된 override 후 GPU 준비는 이전 프레임의 준비 상태도 해제한다.
                const NodePhysicsPose invalid = { UINT32_MAX, Math::Matrix4(Math::kIdentity) };
                CheckComponent(!scene.FinalizeObjectPose(object->GetId(), &invalid, 1), "BC-03b invalid override");
                scene.PrepareMeshes(context);
                for (const auto& mesh : object->GetMeshComponents())
                    CheckComponent(!mesh->HasPoseBounds(), "BC-03b invalid pose clears readiness");
                CheckComponent(scene.FinalizeObjectPose(object->GetId(), nullptr, 0), "BC-03b animation restore");
            }
        }

        // 원래 애니메이션 pose로 GPU 준비를 복구하여 기존 컴포넌트 검사를 이어간다.
        scene.PrepareMeshes(context);
        Utility::Printf("BC-03b PASS: wrapper/split samples=4, node/position/normal, phase validity, owner override, stale ID, invalid pose, GPU preservation\n");
    }

    // 첫 프레임 pose와 분리 단계의 GPU 자원 및 collider 대응을 확인한다.
    inline void CheckPreparedMeshComponents(Scene& scene, GraphicsContext& context)
    {
        // 진단 옵션에서만 분리 단계의 실패와 복구 경로를 실제 GPU 문맥으로 검사한다.
        CheckScenePosePhases(scene, context);

        // 기존 컴포넌트별 자원 독립성과 collider 대응을 검사한다.
        for (const auto& object : scene.GetObjects())
        {
            if (!object->GetModelInstance())
                continue;
            const auto& meshes = object->GetMeshComponents();
            CheckComponent(meshes.size() == 2, "Two components per owner");
            for (const auto& mesh : meshes)
            {
                CheckComponent(mesh->HasPoseBounds(), "Prepared pose bounds");
                if (Renderer::UseComputePose)
                    CheckComponent(mesh->GetPosedGeometry().IsReady(), "Compute pose ready");
            }
            CheckComponent(meshes[0]->GetPosedGeometry().GetBuffer().GetGpuVirtualAddress()
                != meshes[1]->GetPosedGeometry().GetBuffer().GetGpuVirtualAddress(), "Distinct GPU buffers");
            ModelInstance invalidPose(nullptr);
            meshes.front()->Update(context, invalidPose, object->GetTransform());
            CheckComponent(!meshes.front()->HasPoseBounds(), "Invalid pose clears query readiness");
            meshes.front()->Update(context, *object->GetModelInstance(), object->GetTransform());
            for (const auto& collider : object->GetColliderComponents())
                CheckComponent(object->IsColliderCurrent(collider.Id), "Live collider target");
        }
        Utility::Printf("MeshComponent checks: GPU preparation passed\n");
        // GPU 준비와 Scene 단계 검사가 끝났음을 영속 로그에 기록한다.
        std::ofstream log("MeshComponentChecks.log", std::ios::app);
        log << "PASS GPU preparation" << std::endl;
    }
}
