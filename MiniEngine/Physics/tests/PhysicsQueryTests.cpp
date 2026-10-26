#include "PhysicsWorldInternal.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <random>
#include <stdexcept>

using namespace MiniPhysics;
static size_t g_Checks = 0;

// Release에서도 실패를 예외로 보고한다.
static void Check(bool condition, const char* message)
{
    ++g_Checks;
    if (!condition) throw std::runtime_error(message);
}

// 중심과 반크기로 독립적인 CPU 기준 경계를 만든다.
static QueryBounds Box(float x, float y, float z, float radius = 1.0f)
{
    return { {x-radius, y-radius, z-radius}, {x+radius, y+radius, z+radius} };
}

// query 결과에서 해당 handle 존재 여부를 검사한다.
static bool Contains(const std::vector<ColliderHandle>& hits, ColliderHandle handle)
{
    return std::find(hits.begin(), hits.end(), handle) != hits.end();
}

// SDK와 독립적인 닫힌 AABB 겹침 기준을 계산한다.
static bool Overlap(const QueryBounds& a, const QueryBounds& b)
{
    for (size_t i = 0; i < 3; ++i)
        if (a.Minimum[i] > b.Maximum[i] || a.Maximum[i] < b.Minimum[i]) return false;
    return true;
}

// 생성·이동·크기 변경·접촉·삭제와 stale handle을 실제 Jolt world에서 검사한다.
static void TestLifetime()
{
    PhysicsWorld world;
    Check(!world.CreateQueryBox(Box(0,0,0)), "Uninitialized create");
    world.Initialize(8);
    const auto first = world.CreateQueryBox(Box(0,0,0));
    const auto second = world.CreateQueryBox(Box(10,0,0));
    Check(first && second && first != second && world.GetColliderCount() == 2, "Independent bodies");
    auto hits = world.QueryAabb(Box(0,0,0));
    Check(Contains(hits,first) && !Contains(hits,second), "Initial query");
    Check(Contains(world.QueryAabb(Box(2,0,0)),first), "Closed boundary contact");
    Check(world.UpdateQueryBox(first,Box(-10,4,2,0.5f)), "Move and resize");
    Check(!Contains(world.QueryAabb(Box(0,0,0)),first), "Old bounds removed");
    Check(Contains(world.QueryAabb(Box(-10,4,2)),first), "New bounds visible");
    QueryBounds invalid = Box(0,0,0);
    invalid.Minimum[0] = std::numeric_limits<float>::quiet_NaN();
    Check(!world.CreateQueryBox(invalid) && !world.UpdateQueryBox(first,invalid), "NaN rejected");
    invalid = {{1,0,0},{-1,0,0}};
    Check(!invalid.IsValid() && world.QueryAabb(invalid).empty(), "Inverted bounds");
    Check(world.RemoveCollider(first) && !world.RemoveCollider(first), "Delete once");
    Check(!world.UpdateQueryBox(first,Box(0,0,0)), "Stale handle update");
    const auto replacement = world.CreateQueryBox(Box(0,0,0));
    Check(replacement != first && !Contains(world.QueryAabb(Box(0,0,0)),first), "Recycled Jolt ID hidden");
    world.Clear();
    Check(world.GetColliderCount() == 0 && world.QueryAabb(Box(0,0,0)).empty(), "Clear");
    world.Shutdown();
    world.Initialize();
    Check(world.CreateQueryBox(Box(0,0,0)) > replacement, "Handle epoch survives reinitialize");
    PhysicsWorld other;
    other.Initialize();
    Check(other.CreateQueryBox(Box(50,0,0)) && other.QueryAabb(Box(0,0,0)).empty(), "Shared runtime independent worlds");
}

// capacity 초과는 기존 body를 보존하고 명시적인 실패 handle을 반환한다.
static void TestCapacity()
{
    PhysicsWorld world;
    world.Initialize(1);
    const auto handle = world.CreateQueryBox(Box(0,0,0));
    Check(handle && !world.CreateQueryBox(Box(5,0,0)), "Capacity failure");
    Check(Contains(world.QueryAabb(Box(0,0,0)),handle), "Capacity failure preserves input");
}

// 고정 난수의 CPU 전수 검사와 비교해 broadphase 후보 누락을 검출한다.
static void TestConservativeQueries()
{
    PhysicsWorld world;
    world.Initialize();
    std::mt19937 random(20261025);
    std::uniform_real_distribution<float> position(-30.0f,30.0f), extent(0.05f,5.0f);
    std::vector<QueryBounds> boxes;
    std::vector<ColliderHandle> handles;
    for (size_t i = 0; i < 64; ++i)
    {
        boxes.push_back(Box(position(random),position(random),position(random),extent(random)));
        handles.push_back(world.CreateQueryBox(boxes.back()));
        Check(handles.back() != InvalidCollider,"Random body creation");
    }
    for (size_t query = 0; query < 128; ++query)
    {
        const auto bounds = Box(position(random),position(random),position(random),extent(random));
        const auto hits = world.QueryAabb(bounds);
        Check(std::is_sorted(hits.begin(),hits.end()),"Stable result order");
        Check(std::adjacent_find(hits.begin(),hits.end()) == hits.end(),"No duplicates");
        for (size_t i = 0; i < boxes.size(); ++i)
            Check(!Overlap(bounds,boxes[i]) || Contains(hits,handles[i]),"Broadphase false negative");
    }
    const QueryBounds point{{0,0,0},{0,0,0}};
    const auto pointHandle = world.CreateQueryBox(point);
    Check(Contains(world.QueryAabb(point),pointHandle),"Zero extent conservative proxy");
}

// 계층 쌍 필터가 지정한 3x3 행렬과 정확히 같은지 검사한다.
static void TestLayerMatrix()
{
    PhysicsLayers layers;
    // 한정 호출은 가상 호출을 우회하므로 기반 클래스 참조로 호출한다.
    const JPH::ObjectLayerPairFilter& pairFilter = layers;
    const JPH::ObjectVsBroadPhaseLayerFilter& broadFilter = layers;
    const JPH::ObjectLayer order[3] = { ObjectLayers::Query, ObjectLayers::Static, ObjectLayers::Moving };
    // 행은 Query, Static, Moving 순이며 대칭 행렬이다.
    const bool pair[3][3] = { {false,false,false}, {false,false,true}, {false,true,true} };
    // 열은 Query, Static, Moving broadphase 계층 순이다.
    const bool broad[3][3] = { {false,false,false}, {false,false,true}, {false,true,true} };
    const JPH::BroadPhaseLayer broadOrder[3] = { BroadPhaseLayers::Query, BroadPhaseLayers::Static, BroadPhaseLayers::Moving };
    for (size_t i = 0; i < 3; ++i)
    {
        Check(layers.GetBroadPhaseLayer(order[i]) == broadOrder[i], "Broadphase mapping");
        for (size_t j = 0; j < 3; ++j)
        {
            Check(pairFilter.ShouldCollide(order[i], order[j]) == pair[i][j], "Pair filter matrix");
            Check(broadFilter.ShouldCollide(order[i], broadOrder[j]) == broad[i][j], "Object vs broadphase matrix");
        }
    }
    Check(layers.GetNumBroadPhaseLayers() == 3, "Broadphase layer count");
}

// Static·Moving 계층 body를 직접 추가해도 QueryAabb가 Query 계층만 반환하는지 검사한다.
static void TestQueryIsolation()
{
    // world와 query body를 만들고 내부 Impl을 얻는다.
    PhysicsWorld world;
    world.Initialize(8);
    const auto query = world.CreateQueryBox(Box(0,0,0));
    auto* impl = PhysicsWorldAccess::GetImpl(world);
    Check(impl != nullptr, "Impl access");
    auto& bodies = impl->World.GetBodyInterface();

    // Static과 Moving 계층 body를 직접 생성해 같은 위치에 추가한다.
    JPH::BodyID extra[2];
    const JPH::ObjectLayer layers[2] = { ObjectLayers::Static, ObjectLayers::Moving };
    for (size_t i = 0; i < 2; ++i)
    {
        // 반복마다 하나의 body 설정을 만든다.
        JPH::RVec3 center;
        const auto shape = MakeBox(Box(0,0,0), center);
        const auto motion = i == 0 ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic;
        JPH::BodyCreationSettings settings(shape, center, JPH::Quat::sIdentity(), motion, layers[i]);
        settings.mUserData = 900 + i;
        if (motion == JPH::EMotionType::Dynamic)
        {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = 1.0f;
        }


        // 설정한 body를 broadphase에 추가한다.
        extra[i] = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        Check(!extra[i].IsInvalid(), "Extra body creation");
    }

    // 필터 없는 Jolt 질의에는 세 body가 모두 보여 테스트 전제가 맞는지 확인한다.
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> all;
    impl->World.GetBroadPhaseQuery().CollideAABox(JPH::AABox(JPH::Vec3(-1,-1,-1), JPH::Vec3(1,1,1)), all);
    Check(all.mHits.size() == 3, "Unfiltered query sees all layers");

    // QueryAabb는 Query 계층의 handle만 반환해야 한다.
    const auto hits = world.QueryAabb(Box(0,0,0));
    Check(hits.size() == 1 && hits[0] == query, "QueryAabb sees only Query layer");

    // 직접 추가한 body를 제거한다.
    for (const auto id : extra)
    {
        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
    }
}

// 실제 SDK를 링크한 CPU 검증을 한 번에 실행한다.
int main()
{
    try
    {
        TestLifetime();
        TestCapacity();
        TestConservativeQueries();
        TestLayerMatrix();
        TestQueryIsolation();
        std::printf("PhysicsQueryTests: %zu checks passed\n",g_Checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr,"PhysicsQueryTests failed: %s\n",error.what());
        return 1;
    }
}
