#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MiniCollision { struct AggregateGeom; }

namespace MiniPhysics
{
    // 조회 전용 보수적 경계다. 낙하용 shape나 질량 계산에 사용하지 않는다.
    struct QueryBounds
    {
        std::array<float, 3> Minimum{}, Maximum{};
        // 유한한 순서 있는 경계인지 검사한다.
        bool IsValid() const;
    };

    using ColliderHandle = uint64_t;
    constexpr ColliderHandle InvalidCollider = 0;
    constexpr uint32_t MaxColliderCapacity = 65536;

    // 조회용 collider handle과 simulation body handle은 서로 다른 번호 공간이다. 둘을 섞어 쓰지 않는다.
    using BodyHandle = uint64_t;
    constexpr BodyHandle InvalidBody = 0;

    // Static은 고정 지형 계층, Dynamic과 Kinematic은 simulation 계층에 들어간다.
    enum class BodyMotion { Static, Dynamic, Kinematic };

    // body 생성 요청이다. Geometry의 hull 정점은 컴포넌트 로컬 좌표(에셋 단위)이며 Scale이 정점에 곱해진다.
    // body pose는 Position/Rotation(xyzw 쿼터니언)만 담고 scale은 담지 않는다. Geometry는 PrepareBody 호출 중에만 참조한다.
    struct BodyDesc
    {
        const MiniCollision::AggregateGeom* Geometry = nullptr;
        // Geometry가 없고 Motion이 Static일 때만 쓰는 월드 단위 Box 반크기다. 이 경로는 Scale이 항등이어야 한다.
        std::array<double, 3> StaticBoxHalfExtent{};
        std::array<double, 3> Scale{ 1, 1, 1 };
        std::array<double, 3> Position{};
        std::array<double, 4> Rotation{ 0, 0, 0, 1 };
        BodyMotion Motion = BodyMotion::Dynamic;
        double MassScale = 1.0;
        float Friction = 0.7f;
        float Restitution = 0.3f;
    };

    // 준비만 되고 world에 올라가지 않은 body의 소유자다. 이동만 가능하며 CommitBody 없이 파괴되면 body와 용량 예약을 되돌린다.
    // world가 먼저 Shutdown되면 이 객체는 고아가 되어 이후 파괴는 아무 일도 하지 않고 Commit은 InvalidBody를 반환한다.
    class PreparedBody
    {
    public:
        // 빈 객체를 만든다.
        PreparedBody();
        // 미게시 body가 남아 있으면 파괴하고 예약을 반환한다.
        ~PreparedBody();
        PreparedBody(PreparedBody&& other) noexcept;
        PreparedBody& operator=(PreparedBody&& other) noexcept;
        PreparedBody(const PreparedBody&) = delete;
        PreparedBody& operator=(const PreparedBody&) = delete;
        // 준비된 body를 쥐고 있는지 반환한다.
        bool IsValid() const { return m_Data != nullptr; }
        // 구현 상세이며 Jolt 내부 파일만 정의를 본다.
        struct Data;

    private:
        friend class PhysicsWorld;
        friend struct PhysicsWorldAccess;
        std::unique_ptr<Data> m_Data;
    };

    // Jolt 타입과 body 수명을 감춘다. 변경과 조회는 같은 장면 스레드에서 수행한다.
    class PhysicsWorld
    {
    public:
        // body 원점 위치의 각 성분에 허용하는 절대값 한계다.
        static constexpr double MaxBodyPosition = 1.0e9;

        // SDK 초기화는 명시적인 Initialize 호출까지 보류한다.
        PhysicsWorld();
        // 남은 조회 body와 SDK 자원을 순서대로 해제한다.
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;
        // 조회용 Jolt world를 만들며 이미 초기화된 경우 그대로 유지한다.
        void Initialize(uint32_t capacity = 4096);
        // 모든 body를 제거하고 world를 닫는다. handle은 재사용하지 않는다.
        void Shutdown();
        // 조회 전용 Box를 등록한다. 용량 부족이나 무효 경계는 0을 반환한다.
        ColliderHandle CreateQueryBox(const QueryBounds& bounds);
        // 같은 handle의 경계를 교체하고 오래된 handle은 거절한다.
        bool UpdateQueryBox(ColliderHandle handle, const QueryBounds& bounds);
        // body를 broadphase에서 제거한 뒤 파괴한다.
        bool RemoveCollider(ColliderHandle handle);
        // 등록된 모든 body를 제거하되 world와 handle 세대는 유지한다.
        void Clear();
        // AABB와 겹칠 수 있는 collider handle을 반환한다. 정밀 교차 판정은 아니다.
        std::vector<ColliderHandle> QueryAabb(const QueryBounds& bounds) const;
        // 실제 등록된 조회 body 수를 읽는다.
        size_t GetColliderCount() const;

        // 지오메트리를 cook해 world에 올리지 않은 body를 만든다. 실패하면 out을 비우고 false를 반환하며 아무것도 남기지 않는다.
        // 용량 예약 1개를 즉시 차지하며 PreparedBody가 파괴되거나 Commit될 때 정리된다.
        bool PrepareBody(const BodyDesc& desc, PreparedBody& out, std::string* error = nullptr);
        // 준비된 shape와 pose의 월드 질량 중심을 읽는다. 무효·고아·다른 world이면 false이며 출력은 그대로다.
        bool GetPreparedCenterOfMass(const PreparedBody& body, std::array<double, 3>& worldCom) const;
        // 미게시 동적 body의 월드 선속도와 각속도를 설정한다(Jolt 최대 속도로 제한). 무효 입력이면 변경 없이 false다.
        bool SetPreparedVelocity(PreparedBody& body, const std::array<double, 3>& linear, const std::array<double, 3>& angular);
        // 준비된 body를 world에 올리고 handle을 발급한다. 할당과 실패 경로가 없다(handle 맵은 Initialize에서 예약, 노드는 Prepare에서 할당). 다른 world이거나 고아면 InvalidBody를 반환하고 body는 그대로 둔다.
        BodyHandle CommitBody(PreparedBody&& body, bool activate);
        // body를 world에서만 빼고 살려 둔다. 이미 빠졌거나 handle이 무효면 false다.
        bool RemoveBodyFromWorld(BodyHandle handle);
        // 빼 둔 body를 다시 world에 올린다(롤백용). 이미 올라가 있거나 handle이 무효면 false다.
        bool AddBodyToWorld(BodyHandle handle, bool activate);
        // 필요하면 world에서 빼고 body를 파괴하며 handle을 등록 해제한다. handle 번호는 재사용하지 않는다.
        bool DestroyBody(BodyHandle handle);
        // body 원점을 순간이동한다(Unreal SetBodyTransform). 속도는 그대로 두고 월드에 올라간 동적 body는 깨운다. 입력이나 handle이 무효면 false이며 body는 그대로다.
        bool SetBodyTransform(BodyHandle handle, const std::array<double, 3>& position, const std::array<double, 4>& rotation);
        // 등록된 Kinematic body의 원점 목표에 fixedDt 동안 도달할 COM 속도를 설정한다.
        bool MoveKinematic(BodyHandle handle, const std::array<double, 3>& position, const std::array<double, 4>& rotation, float fixedDt);
        // 등록된 비정적 body를 Kinematic 또는 Dynamic으로 전환하며 handle·pose·속도를 보존한다.
        bool SetBodyMotion(BodyHandle handle, BodyMotion motion);
        // 등록된 비정적 body의 COM 선속도와 월드 각속도를 Jolt 최대 속도로 제한해 설정한다.
        bool SetBodyVelocity(BodyHandle handle, const std::array<double, 3>& linear, const std::array<double, 3>& angular);
        // 등록된 Dynamic body에 월드 작용점의 impulse(kg·m/s)를 가한다. 무효 입력은 변경 없이 거절한다.
        bool AddBodyImpulse(BodyHandle handle, const std::array<double, 3>& impulse, const std::array<double, 3>& worldPoint);
        // shape와 질량을 원자적으로 교체한다(scale 편집용). 실패하면 false를 반환하며 shape·질량·pose·속도는 그대로다. 성공하면 월드에 올라간 동적 body는 깨운다.
        bool SetBodyShape(BodyHandle handle, const MiniCollision::AggregateGeom& geometry, const std::array<double, 3>& scale, double massScale, std::string* error = nullptr);
        // 게시된 simulation body 수를 반환한다.
        size_t GetBodyCount() const;
        // 조회 body, simulation body, 준비 중 body의 예약 합계를 반환한다. Initialize 용량을 넘지 않는다.
        size_t GetReservedCount() const;

        // 고정 시간 deltaSeconds만큼 simulation을 한 번(충돌 step 1회) 진행한다. 중력은 Y-up (0, -9.8, 0) m/s²(Unreal -980 cm/s²)다.
        // 초기화 전이거나 dt가 비유한·0 이하이거나 Jolt가 오류(접촉·body 쌍 용량 초과 등)를 보고하면 false를 반환하며, 오류여도 world는 계속 사용할 수 있다.
        bool Step(float deltaSeconds);
        // body 원점(컴포넌트 로컬 원점)의 월드 pose를 읽는다. 질량 중심이 아니다. handle이 무효이거나 제거된 body면 false다.
        bool GetBodyPose(BodyHandle handle, std::array<double, 3>& position, std::array<double, 4>& rotation) const;
        // body 질량 중심의 월드 위치를 읽는다. handle이 무효면 false다.
        bool GetBodyCenterOfMass(BodyHandle handle, std::array<double, 3>& position) const;
        // body가 깨어 있는지(Jolt IsActive) 읽는다. world에 올라가지 않았거나 handle이 무효면 false다.
        bool IsBodyAwake(BodyHandle handle) const;
        // body의 선속도(m/s)와 각속도(rad/s)를 읽는다. 정적 body는 0이다. handle이 무효면 false다.
        bool GetBodyVelocity(BodyHandle handle, std::array<double, 3>& linear, std::array<double, 3>& angular) const;
        // 진단에서 동적 body의 실제 질량을 읽는다.
        bool GetBodyMass(BodyHandle handle, double& mass) const;

    private:
        struct Impl;
        friend struct PhysicsWorldAccess;
        std::unique_ptr<Impl> m_Impl;
        ColliderHandle m_NextHandle = 1;
        // simulation body 전용 handle 발급 번호이며 m_NextHandle과 별개다.
        BodyHandle m_NextBodyHandle = 1;
    };
}
