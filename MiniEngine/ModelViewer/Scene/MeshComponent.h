#pragma once

#include "ComponentId.h"
#include "SceneTransform.h"
#include "GeometryRevision.h"
#include <string>

namespace Viewer
{
    enum class MeshTransformMode { Attached, WorldIndependent };
    enum class PoseMode { FollowModel, Frozen };

    struct DeformBinding
    {
        // SourceFrame은 정점의 기준 프레임이며 사용자가 움직이는 target Transform과 별개다.
        SceneTransform SourceFrame;
        PoseMode Pose = PoseMode::FollowModel;
    };

    // 컴포넌트 충돌의 출처. Inherited는 2b-2에서 절단 결과 계승에 사용한다.
    enum class ComponentCollisionStatus { None, FromModel, Inherited, Unavailable };

    // 선택 section이 모두 Generated hull을 가질 때만 hull에 3x4 역변환을 적용해 out에 모은다. 아니면 out을 비우고 이유를 남긴다.
    bool GatherSectionHulls(const std::vector<MeshCollisionStatus>& statuses, const std::vector<MiniCollision::AggregateGeom>& hulls,
        const std::vector<GeometrySection>& sections, const double inverse[3][4], MiniCollision::AggregateGeom& out, std::string& reason);

    class MeshComponent
    {
    public:
        // 공유 원본 revision을 사용하되 GPU pose·렌더 상수는 새로 만든다.
        MeshComponent(ComponentId id, std::shared_ptr<const GeometryRevision> geometry);
        // 이전 프레임의 GPU 사용이 끝난 뒤 렌더 자원을 해제한다.
        ~MeshComponent();
        // owner 안에서 재사용하지 않는 ID를 읽는다.
        ComponentId GetId() const { return m_Id; }
        // 지연 절단 결과가 이전 편집 상태를 덮어쓰지 않도록 상태 세대를 읽는다.
        uint64_t GetStateVersion() const { return m_StateVersion; }
        // 현재 geometry의 강한 참조를 읽는다. 교체 전 snapshot도 별도 보관할 수 있다.
        const std::shared_ptr<const GeometryRevision>& GetGeometry() const { return m_Geometry; }
        // 이 컴포넌트에만 속한 현재 자세 버퍼를 읽는다.
        const PosedGeometry& GetPosedGeometry() const { return *m_PosedGeometry; }
        // 편집할 target TRS를 읽는다.
        const SceneTransform& GetTransform() const { return m_Transform; }
        // owner 종속 여부를 읽는다.
        MeshTransformMode GetTransformMode() const { return m_TransformMode; }
        // 현재 source 프레임과 pose 정책을 읽는다.
        const DeformBinding& GetDeformBinding() const { return m_DeformBinding; }
        // 유효한 target TRS를 저장한다. WorldIndependent에서는 인수가 월드 TRS다.
        bool SetTransform(const SceneTransform& transform, MeshTransformMode mode);
        // source 프레임과 pose 추종/고정 정책을 검증하고 적용한다.
        bool SetDeformBinding(const DeformBinding& binding);
        // source 프레임과 target 배치를 한 번에 게시한다. 화면 보존 시 Attached의 target을 SourceFrame으로 맞춘다.
        bool SetPlacement(const SceneTransform& transform, MeshTransformMode mode, const DeformBinding& binding);
        // 같은 Model의 새 geometry를 게시하고 이 컴포넌트의 pose 출력을 무효화한다.
        bool SetGeometry(std::shared_ptr<const GeometryRevision> geometry);
        // 현재 revision에 대응하는 컴포넌트 로컬 충돌을 읽는다. revision이 다르면 빈 형상이다.
        const MiniCollision::AggregateGeom& GetCollision() const;
        // 충돌 출처를 읽는다. revision이 다르면 Unavailable이다.
        ComponentCollisionStatus GetCollisionStatus() const;
        // 생성 revision에 부모에서 잘라 계승한 충돌을 저장한다. revisionId가 현재 생성 geometry ID와 다르면 거절한다.
        // geom이 비면 body 없는 조각으로 Unavailable이 되며 emptyReason을 이유로 남긴다.
        bool SetInheritedCollision(MiniCollision::AggregateGeom geom, uint64_t revisionId, const char* emptyReason);
        // 충돌이 없거나 쓸 수 없는 이유를 읽는다.
        const char* GetCollisionReason() const
        {
            // 저장된 이유 문자열을 돌려준다.
            return m_CollisionReason.c_str();
        }
        // 충돌이 대응하는 GeometryRevision ID를 읽는다.
        uint64_t GetCollisionRevisionId() const
        {
            // 충돌이 만들어진 revision ID를 돌려준다.
            return m_CollisionRevisionId;
        }
        // -collision-log 1일 때 컴포넌트 충돌 요약을 출력한다.
        void LogCollision(uint32_t ownerId) const;
        // 렌더·선택·후속 collider가 공통으로 사용할 월드 행렬을 계산한다.
        Math::AffineTransform GetWorldTransform(const SceneTransform& owner) const;
        // 첫 pose 준비 전이나 기준 변경 직후에는 선택을 보류한다.
        bool HasPoseBounds() const { return m_Ready && !m_SectionBounds.empty(); }
        // 절단 section AABB를 만들기 위한 component-local 경계를 읽는다.
        Math::BoundingSphere GetLocalSectionBounds(size_t section) const { return m_SectionBounds.at(section); }
        // 현재 geometry의 보수적인 월드 경계 구를 반환한다.
        Math::BoundingSphere GetBoundingSphere(const SceneTransform& owner) const;
        // 공유 CPU pose를 읽어 컴포넌트별 GPU 정점과 렌더 상수를 준비한다.
        void Update(GraphicsContext& context, const ModelInstance& pose, const SceneTransform& owner);
        // 현재 revision의 section만 기본·깊이·그림자 pass에 등록한다.
        void Render(Renderer::MeshSorter& sorter) const;

    private:
        // 현재 revision과 SourceFrame으로 컴포넌트 로컬 충돌을 다시 만든다.
        void RebuildCollision();

        ComponentId m_Id;
        uint64_t m_StateVersion = 1;
        SceneTransform m_Transform;
        MeshTransformMode m_TransformMode = MeshTransformMode::Attached;
        DeformBinding m_DeformBinding;
        std::shared_ptr<const GeometryRevision> m_Geometry;
        std::unique_ptr<PosedGeometry> m_PosedGeometry;
        UploadBuffer m_ConstantsCPU;
        ByteAddressBuffer m_ConstantsGPU;
        Math::AffineTransform m_PreparedWorld{ Math::kIdentity };
        std::vector<Math::BoundingSphere> m_SectionBounds;
        std::vector<Math::Matrix4> m_FrozenNodes;
        std::vector<Joint> m_FrozenJoints;
        const Joint* m_RenderJoints = nullptr;
        bool m_Ready = false;
        MiniCollision::AggregateGeom m_Collision;   // 컴포넌트 로컬(SourceFrame⁻¹ × model-local) 충돌
        ComponentCollisionStatus m_CollisionStatus = ComponentCollisionStatus::None;
        std::string m_CollisionReason;
        uint64_t m_CollisionRevisionId = 0;         // m_Collision이 만들어진 GeometryRevision ID
    };
}
