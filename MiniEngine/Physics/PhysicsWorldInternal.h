#pragma once

// Jolt를 직접 포함하는 비공개 헤더다. Physics 내부 .cpp와 테스트만 포함하고 엔진 코드는 포함하지 않는다.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include "PhysicsWorld.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace MiniPhysics
{
    // 객체 계층: Query는 조회 전용, Static은 고정 지형, Moving은 simulation 대상이다.
    namespace ObjectLayers
    {
        constexpr JPH::ObjectLayer Query = 0;
        constexpr JPH::ObjectLayer Static = 1;
        constexpr JPH::ObjectLayer Moving = 2;
        constexpr JPH::uint Count = 3;
    }

    // broadphase 계층은 객체 계층과 1:1로 대응한다.
    namespace BroadPhaseLayers
    {
        constexpr JPH::BroadPhaseLayer Query(0);
        constexpr JPH::BroadPhaseLayer Static(1);
        constexpr JPH::BroadPhaseLayer Moving(2);
    }

    // Query는 어떤 접촉에도 참여하지 않고 Static끼리는 쌍을 만들지 않는다.
    struct PhysicsLayers final : JPH::BroadPhaseLayerInterface,
        JPH::ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayerPairFilter
    {
        // 객체 계층 수와 같은 개수의 broadphase 계층을 쓴다.
        JPH::uint GetNumBroadPhaseLayers() const override { return ObjectLayers::Count; }
        // 객체 계층 번호를 같은 번호의 broadphase 계층으로 변환한다.
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
        {
            return JPH::BroadPhaseLayer(JPH::BroadPhaseLayer::Type(layer));
        }
        // Static은 Moving만, Moving은 Static과 Moving을 검사하고 Query는 아무것도 검사하지 않는다.
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override
        {
            if (layer == ObjectLayers::Static) return broadPhase == BroadPhaseLayers::Moving;
            if (layer == ObjectLayers::Moving) return broadPhase == BroadPhaseLayers::Static || broadPhase == BroadPhaseLayers::Moving;
            return false;
        }
        // Static-Moving과 Moving-Moving만 simulation 접촉쌍이 된다.
        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
        {
            if (a == ObjectLayers::Moving) return b == ObjectLayers::Static || b == ObjectLayers::Moving;
            if (a == ObjectLayers::Static) return b == ObjectLayers::Moving;
            return false;
        }
    };

    // 여러 world가 같은 Jolt 등록 정보를 공유하도록 초기화와 해제를 맡는다.
    struct Runtime
    {
        // Jolt 전역 등록을 수행한다.
        Runtime();
        // Jolt 전역 등록을 해제한다.
        ~Runtime();
    };

    // 기존 world가 살아 있으면 동일 runtime을 재사용한다.
    std::shared_ptr<Runtime> AcquireRuntime();

    // 경계의 중심 반올림까지 포함하도록 Box 반크기를 보수적으로 만든다.
    JPH::RefConst<JPH::Shape> MakeBox(const QueryBounds& bounds, JPH::RVec3& center);

    // simulation body handle과 Jolt BodyID의 대응이다. query handle 맵과 별개다.
    using SimBodyMap = std::unordered_map<BodyHandle, JPH::BodyID>;

    // PhysicsWorld의 Jolt 상태다. Bodies는 query handle만 담고 body handle과 섞지 않는다.
    struct PhysicsWorld::Impl
    {
        std::shared_ptr<Runtime> RuntimeOwner = AcquireRuntime();
        PhysicsLayers Layers;
        JPH::PhysicsSystem World;
        std::unordered_map<ColliderHandle, JPH::BodyID> Bodies;
        // Initialize 용량이며 조회·simulation·준비 중 body 합계의 상한이다.
        uint32_t Capacity = 0;
        // 게시된 simulation body다. Initialize에서 용량만큼 예약해 Commit이 재해시하지 않는다.
        SimBodyMap SimBodies;
        // 준비되었지만 게시되지 않은 body 소유 정보다. Shutdown이 고아 처리한다.
        std::unordered_set<PreparedBody::Data*> Prepared;
        // 용량 예약 합계를 반환한다.
        size_t Reserved() const { return Bodies.size() + SimBodies.size() + Prepared.size(); }
    };

    // 같은 Physics 내부 파일과 테스트가 비공개 Impl에 접근하는 통로다.
    struct PhysicsWorldAccess
    {
        // 초기화되지 않았다면 nullptr을 반환한다.
        static PhysicsWorld::Impl* GetImpl(PhysicsWorld& world) { return world.m_Impl.get(); }
        // Jolt 비공개 구현 타입의 별칭이다.
        using ImplType = PhysicsWorld::Impl;
    };

    // PreparedBody가 쥔 준비 상태다. Node는 Prepare에서 미리 할당해 Commit이 할당하지 않게 한다.
    struct PreparedBody::Data
    {
        // 소속 world이며 Shutdown 이후에는 nullptr(고아)이다.
        PhysicsWorldAccess::ImplType* Owner = nullptr;
        JPH::BodyID Id;
        SimBodyMap::node_type Node;
    };
}
