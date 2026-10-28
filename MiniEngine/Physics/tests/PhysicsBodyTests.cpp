#include "CollisionTestUtils.h"
#include "PhysicsWorldInternal.h"
#include "Collision/AggregateGeom.h"
#include "Collision/BodyMass.h"
#include <algorithm>
#include <cstdio>

using namespace MiniPhysics;

// 중심과 반크기로 8개 모서리의 18-DOP hull 요소 하나를 가진 지오메트리를 만든다.
static AggregateGeom MakeBoxGeom(Vec3d c, Vec3d half)
{
    // 모서리 8개를 모아 hull을 만든다.
    std::vector<Vec3d> corners;
    for (int i = 0; i < 8; ++i)
    {
        corners.push_back({ c.x + ((i & 1) ? half.x : -half.x), c.y + ((i & 2) ? half.y : -half.y), c.z + ((i & 4) ? half.z : -half.z) });
    }
    AggregateGeom geom;
    Check(GenerateKDop18(corners, geom) && geom.ConvexElems.size() == 1, "Box geometry builds");
    return geom;
}

// 지오메트리 요소 부피의 합을 구한다. 18-DOP는 모서리를 약간 깎으므로 해석값 대신 실제 hull 부피를 기준으로 삼는다.
static double Volume(const AggregateGeom& geom)
{
    // 요소별 부피를 더한다.
    double sum = 0.0;
    for (const auto& elem : geom.ConvexElems) sum += elem.GetVolume();
    return sum;
}

// 핸들에 대응하는 Jolt body 잠금으로 body 상태를 읽는다.
struct BodyReader
{
    PhysicsWorld& World;
    JPH::BodyID Id;

    // 핸들을 내부 BodyID로 바꾼다.
    BodyReader(PhysicsWorld& world, BodyHandle handle) : World(world), Id(PhysicsWorldAccess::GetImpl(world)->SimBodies.at(handle)) {}
    // Jolt body 잠금 인터페이스를 반환한다.
    const JPH::BodyLockInterface& Locks() const { return PhysicsWorldAccess::GetImpl(World)->World.GetBodyLockInterface(); }
    // body 질량을 역질량에서 읽는다. 정적 body는 0이다.
    double Mass() const
    {
        const JPH::BodyLockRead lock(Locks(), Id);
        const float inverse = lock.GetBody().GetMotionProperties()->GetInverseMass();
        return inverse > 0.0f ? 1.0 / inverse : 0.0;
    }
    // body 원점 위치를 읽는다.
    JPH::RVec3 Position() const { const JPH::BodyLockRead lock(Locks(), Id); return lock.GetBody().GetPosition(); }
    // body 질량 중심의 월드 위치를 읽는다.
    JPH::RVec3 CenterOfMass() const { const JPH::BodyLockRead lock(Locks(), Id); return lock.GetBody().GetCenterOfMassPosition(); }
    // body 회전을 읽는다.
    JPH::Quat Rotation() const { const JPH::BodyLockRead lock(Locks(), Id); return lock.GetBody().GetRotation(); }
    // shape의 body 로컬 경계 상자를 읽는다.
    JPH::AABox LocalBounds() const { const JPH::BodyLockRead lock(Locks(), Id); return lock.GetBody().GetShape()->GetLocalBounds(); }
    // Jolt가 계산한 shape 부피를 읽는다.
    float ShapeVolume() const { const JPH::BodyLockRead lock(Locks(), Id); return lock.GetBody().GetShape()->GetVolume(); }
    // body shape의 하위 형식을 읽는다.
    JPH::EShapeSubType SubType() const { const JPH::BodyLockRead lock(Locks(), Id); return lock.GetBody().GetShape()->GetSubType(); }
    // 관성 대각 성분 세 개를 오름차순으로 읽는다.
    std::array<double, 3> SortedInertia() const
    {
        const JPH::BodyLockRead lock(Locks(), Id);
        const JPH::Vec3 inv = lock.GetBody().GetMotionProperties()->GetInverseInertiaDiagonal();
        std::array<double, 3> inertia{ 1.0 / inv.GetX(), 1.0 / inv.GetY(), 1.0 / inv.GetZ() };
        std::sort(inertia.begin(), inertia.end());
        return inertia;
    }
};

// 지오메트리 포인터와 기본값으로 동적 body 요청을 만든다.
static BodyDesc MakeDesc(const AggregateGeom& geom)
{
    BodyDesc desc;
    desc.Geometry = &geom;
    return desc;
}

// 동적 body가 Unreal 질량과 상자 관성 비율로 만들어지고 pose가 body 원점인지 검사한다.
static void TestDynamicBox()
{
    // 1 x 2 x 3 상자를 (1,2,3)에 준비하고 게시한다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 1.0, 1.5 });
    BodyDesc desc = MakeDesc(geom);
    desc.Position = { 1, 2, 3 };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared) && prepared.IsValid(), "Prepare dynamic box");
    Check(world.GetReservedCount() == 1 && world.GetBodyCount() == 0, "Prepared reserves only");
    const BodyHandle handle = world.CommitBody(std::move(prepared), true);
    Check(handle != InvalidBody && !prepared.IsValid(), "Commit hands over ownership");
    Check(world.GetBodyCount() == 1 && world.GetReservedCount() == 1, "Committed counts");

    // 질량은 Unreal 공식과 같고 관성 비율은 상자 해석값이다.
    const BodyReader reader(world, handle);
    const double expected = MiniCollision::CalculateUnrealMass(Volume(geom));
    CheckNear(reader.Mass(), expected, expected * 1e-4, "Unreal mass");
    const auto inertia = reader.SortedInertia();
    CheckNear(inertia[0] / inertia[2], 5.0 / 13.0, 0.01, "Inertia ratio low/high");
    CheckNear(inertia[1] / inertia[2], 10.0 / 13.0, 0.01, "Inertia ratio mid/high");
    CheckNear(inertia[2], expected / 12.0 * 13.0, expected * 0.02, "Inertia scaled to mass");

    // 대칭 상자의 원점은 요청 위치다.
    const auto p = reader.Position();
    CheckNear(p.GetX(), 1.0, 1e-5, "Origin x");
    CheckNear(p.GetY(), 2.0, 1e-5, "Origin y");
    CheckNear(p.GetZ(), 3.0, 1e-5, "Origin z");
}

// scale이 정점에 곱해져 부피가 두 배가 되고 MassScale이 지수 적용 뒤 곱해지는지 검사한다.
static void TestScaleAndMassScale()
{
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });

    // scale (2,1,1)이면 부피 2의 질량이다.
    BodyDesc desc = MakeDesc(geom);
    desc.Scale = { 2, 1, 1 };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Prepare scaled");
    const BodyHandle scaled = world.CommitBody(std::move(prepared), false);
    const double volume2 = MiniCollision::CalculateUnrealMass(Volume(geom) * 2.0);
    CheckNear(BodyReader(world, scaled).Mass(), volume2, volume2 * 1e-4, "Scaled volume mass");

    // MassScale 3은 질량을 3배로 만든다.
    desc.Scale = { 1, 1, 1 };
    desc.MassScale = 3.0;
    Check(world.PrepareBody(desc, prepared), "Prepare mass scale");
    const BodyHandle heavy = world.CommitBody(std::move(prepared), false);
    const double volume1 = MiniCollision::CalculateUnrealMass(Volume(geom), 3.0);
    CheckNear(BodyReader(world, heavy).Mass(), volume1, volume1 * 1e-4, "Mass scale");
}

// 요소가 둘이면 StaticCompound가 되고 질량은 부피 합을 따르며 정적 body는 질량을 만들지 않는지 검사한다.
static void TestCompoundAndStatic()
{
    // 단위 상자 두 개를 떨어뜨려 한 지오메트리에 담는다.
    PhysicsWorld world;
    world.Initialize(8);
    AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    geom.ConvexElems.push_back(MakeBoxGeom({ 3, 0, 0 }, { 0.5, 0.5, 0.5 }).ConvexElems[0]);

    // 동적 compound의 질량은 부피 2 기준이다.
    PreparedBody prepared;
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare compound");
    const BodyHandle compound = world.CommitBody(std::move(prepared), false);
    const BodyReader reader(world, compound);
    const double expected = MiniCollision::CalculateUnrealMass(Volume(geom));
    Check(reader.SubType() == JPH::EShapeSubType::StaticCompound, "Two elements make a compound");
    CheckNear(reader.Mass(), expected, expected * 1e-3, "Compound mass");

    // 정적 body는 질량 0이고 Static 계층이다.
    BodyDesc desc = MakeDesc(geom);
    desc.Motion = BodyMotion::Static;
    Check(world.PrepareBody(desc, prepared), "Prepare static");
    const BodyHandle fixed = world.CommitBody(std::move(prepared), true);
    const BodyReader fixedReader(world, fixed);
    const JPH::BodyLockRead lock(fixedReader.Locks(), fixedReader.Id);
    Check(lock.GetBody().IsStatic() && lock.GetBody().GetObjectLayer() == ObjectLayers::Static, "Static body layer");
}

// 잘못된 입력은 false를 돌려주고 예약이나 body를 남기지 않는지 검사한다.
static void TestFailures()
{
    PhysicsWorld world;
    PreparedBody prepared;
    BodyDesc empty;
    Check(!world.PrepareBody(empty, prepared), "Uninitialized world rejects");
    world.Initialize(8);

    // 널과 빈 지오메트리를 거절한다.
    std::string error;
    Check(!world.PrepareBody(empty, prepared, &error) && !error.empty(), "Null geometry rejected with message");
    const AggregateGeom none;
    Check(!world.PrepareBody(MakeDesc(none), prepared), "Empty geometry rejected");

    // 정점이 모자라거나 납작한 hull을 거절한다.
    AggregateGeom few;
    few.ConvexElems.resize(1);
    few.ConvexElems[0].VertexData = { {0,0,0}, {1,0,0}, {0,1,0} };
    Check(!world.PrepareBody(MakeDesc(few), prepared), "Three vertices rejected");
    AggregateGeom flat;
    flat.ConvexElems.resize(1);
    flat.ConvexElems[0].VertexData = { {0,0,0}, {1,0,0}, {0,1,0}, {1,1,0}, {0.5,0.5,0} };
    Check(!world.PrepareBody(MakeDesc(flat), prepared, &error) && !error.empty(), "Flat hull rejected");

    // 인덱스 없는 요소는 부피 0이라 동적 body를 거절한다.
    AggregateGeom noIndex = MakeBoxGeom({ 0, 0, 0 }, { 1, 1, 1 });
    noIndex.ConvexElems[0].IndexData.clear();
    Check(!world.PrepareBody(MakeDesc(noIndex), prepared), "Zero volume dynamic rejected");

    // 비정상 scale·위치·회전·질량 배율을 거절한다.
    const AggregateGeom good = MakeBoxGeom({ 0, 0, 0 }, { 1, 1, 1 });
    BodyDesc bad = MakeDesc(good);
    bad.Scale = { 1, -1, 1 };
    Check(!world.PrepareBody(bad, prepared), "Negative scale rejected");
    bad = MakeDesc(good);
    bad.Position[1] = std::nan("");
    Check(!world.PrepareBody(bad, prepared), "NaN position rejected");
    bad = MakeDesc(good);
    bad.Rotation = { 0, 0, 0, 0 };
    Check(!world.PrepareBody(bad, prepared), "Zero rotation rejected");
    bad = MakeDesc(good);
    bad.MassScale = 0.0;
    Check(!world.PrepareBody(bad, prepared), "Zero mass scale rejected");

    // float 범위를 넘는 질량과 float에서 무한대가 되는 정점을 거절한다.
    bad = MakeDesc(good);
    bad.MassScale = 1.0e300;
    Check(!world.PrepareBody(bad, prepared, &error) && !error.empty(), "Mass beyond float rejected");
    bad = MakeDesc(good);
    bad.Scale = { 1.0e300, 1, 1 };
    Check(!world.PrepareBody(bad, prepared, &error) && !error.empty(), "Infinite float vertex rejected");
    bad = MakeDesc(good);
    bad.Motion = BodyMotion::Static;
    bad.Scale = { 1, 1.0e300, 1 };
    Check(!world.PrepareBody(bad, prepared), "Infinite float vertex rejected for static");

    // 모든 실패 뒤에 예약과 body가 남지 않는다.
    Check(!prepared.IsValid() && world.GetReservedCount() == 0 && world.GetBodyCount() == 0, "No leaked reservation");
    Check(PhysicsWorldAccess::GetImpl(world)->World.GetNumBodies() == 0, "No leaked Jolt body");
}

// 커밋 없이 파괴·이동·덮어쓰기한 PreparedBody가 예약을 정확히 반환하는지 검사한다.
static void TestPreparedOwnership()
{
    PhysicsWorld world;
    world.Initialize(4);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    auto* impl = PhysicsWorldAccess::GetImpl(world);

    // 범위를 벗어나면 예약이 풀린다.
    {
        PreparedBody scoped;
        Check(world.PrepareBody(MakeDesc(geom), scoped) && world.GetReservedCount() == 1, "Scoped prepare");
        Check(impl->World.GetNumBodies() == 1, "Jolt body exists while prepared");
    }
    Check(world.GetReservedCount() == 0 && impl->World.GetNumBodies() == 0, "Destroyed prepared released");

    // 이동하면 원본은 비고 대상만 예약을 유지한다.
    PreparedBody first, second;
    Check(world.PrepareBody(MakeDesc(geom), first), "Prepare for move");
    second = std::move(first);
    Check(!first.IsValid() && second.IsValid() && world.GetReservedCount() == 1, "Move keeps single reservation");

    // 같은 객체로 다시 준비하면 이전 body가 정리된다.
    Check(world.PrepareBody(MakeDesc(geom), second) && world.GetReservedCount() == 1, "Re-prepare replaces previous");
    second = PreparedBody();
    Check(world.GetReservedCount() == 0, "Assign empty releases");

    // Shutdown 뒤에 남은 PreparedBody는 고아이며 파괴해도 안전하고 새 world에서 Commit되지 않는다.
    PreparedBody orphan;
    Check(world.PrepareBody(MakeDesc(geom), orphan), "Prepare orphan");
    world.Shutdown();
    world.Initialize(4);
    Check(world.CommitBody(std::move(orphan), true) == InvalidBody && world.GetBodyCount() == 0, "Orphan cannot commit");
}

// 용량 2에서 조회·simulation·준비 body가 하나의 예약을 공유하고 파괴로 자리가 반환되는지 검사한다.
static void TestCapacity()
{
    PhysicsWorld world;
    world.Initialize(2);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });

    // 조회 body 하나와 준비 body 하나가 용량을 채운다.
    const ColliderHandle query = world.CreateQueryBox({ {5,5,5}, {6,6,6} });
    Check(query != InvalidCollider, "Query takes one slot");
    PreparedBody a, b;
    Check(world.PrepareBody(MakeDesc(geom), a) && world.GetReservedCount() == 2, "Prepared takes second slot");
    Check(!world.PrepareBody(MakeDesc(geom), b) && !b.IsValid(), "Third prepare fails cleanly");
    Check(world.CreateQueryBox({ {7,7,7}, {8,8,8} }) == InvalidCollider, "Query full while prepared holds slot");

    // 게시 후 파괴하면 자리가 돌아온다.
    const BodyHandle handle = world.CommitBody(std::move(a), false);
    Check(!world.PrepareBody(MakeDesc(geom), b), "Still full after commit");
    Check(world.DestroyBody(handle) && world.GetReservedCount() == 1, "Destroy frees slot");
    Check(world.PrepareBody(MakeDesc(geom), b) && world.GetReservedCount() == 2, "Slot reusable");
}

// 제거·재추가·파괴 수명과 handle 비재사용, Clear의 simulation body 정리를 검사한다.
static void TestLifecycle()
{
    PhysicsWorld world;
    world.Initialize(4);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    PreparedBody prepared;
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare lifecycle");
    const BodyHandle handle = world.CommitBody(std::move(prepared), false);
    auto* impl = PhysicsWorldAccess::GetImpl(world);
    const JPH::BodyID id = impl->SimBodies.at(handle);
    auto& bodies = impl->World.GetBodyInterface();

    // 제거하면 body는 살아 있지만 broadphase에는 없다.
    Check(bodies.IsAdded(id), "Committed body added");
    Check(world.RemoveBodyFromWorld(handle) && !bodies.IsAdded(id), "Removed from world");
    Check(!world.RemoveBodyFromWorld(handle) && world.GetBodyCount() == 1, "Double remove rejected, body kept");

    // 다시 올리면 broadphase에 돌아온다.
    Check(world.AddBodyToWorld(handle, false) && bodies.IsAdded(id), "Re-added");
    Check(!world.AddBodyToWorld(handle, false), "Double add rejected");

    // 제거된 상태에서도 파괴할 수 있고 handle은 재사용되지 않는다.
    Check(world.RemoveBodyFromWorld(handle) && world.DestroyBody(handle) && world.GetBodyCount() == 0, "Destroy removed body");
    Check(!world.DestroyBody(handle) && !world.RemoveBodyFromWorld(handle) && !world.AddBodyToWorld(handle, false), "Stale handle rejected");
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare after destroy");
    const BodyHandle next = world.CommitBody(std::move(prepared), false);
    Check(next > handle, "Handle not reused");

    // Clear는 simulation body도 정리하지만 준비만 된 body는 남긴다.
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare survivor");
    world.Clear();
    Check(world.GetBodyCount() == 0 && world.GetReservedCount() == 1 && prepared.IsValid(), "Clear removes simulation bodies only");
    Check(world.CommitBody(std::move(prepared), false) > next, "Prepared survives Clear");

    // handle은 Prepare에서 예약되어 Commit 결과와 같고, 고갈되면 Prepare가 실패한다.
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare reserves handle");
    const BodyHandle reserved = PhysicsWorldAccess::NextBodyHandle(world) - 1;
    Check(world.CommitBody(std::move(prepared), false) == reserved, "Committed handle equals reserved handle");
    PhysicsWorldAccess::NextBodyHandle(world) = std::numeric_limits<BodyHandle>::max();
    Check(!world.PrepareBody(MakeDesc(geom), prepared) && !prepared.IsValid(), "Handle exhaustion fails Prepare");
}

// 동적 body가 QueryAabb에 나타나지 않는지 검사한다.
static void TestQueryIsolation()
{
    PhysicsWorld world;
    world.Initialize(4);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    PreparedBody prepared;
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare isolation");
    Check(world.CommitBody(std::move(prepared), true) != InvalidBody, "Commit isolation");
    const QueryBounds around{ {-1,-1,-1}, {1,1,1} };
    Check(world.QueryAabb(around).empty(), "Dynamic body hidden from QueryAabb");
    const ColliderHandle query = world.CreateQueryBox(around);
    const auto hits = world.QueryAabb(around);
    Check(hits.size() == 1 && hits[0] == query, "Only query box visible");
}

// 원점에서 떨어진 hull과 회전에서 body 위치가 질량 중심이 아닌 원점인지 검사한다.
static void TestOffCenter()
{
    // 로컬 +x 5 위치의 상자를 z축 90도 회전해 (1,2,3)에 놓는다.
    PhysicsWorld world;
    world.Initialize(4);
    const AggregateGeom geom = MakeBoxGeom({ 5, 0, 0 }, { 0.5, 1.0, 1.5 });
    BodyDesc desc = MakeDesc(geom);
    desc.Position = { 1, 2, 3 };
    const double h = std::sqrt(0.5);
    desc.Rotation = { 0, 0, h, h };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Prepare off-center");
    const BodyHandle handle = world.CommitBody(std::move(prepared), false);

    // body 위치는 요청 원점이고 질량 중심은 회전된 오프셋만큼 떨어져 있다.
    const BodyReader reader(world, handle);
    const auto p = reader.Position();
    CheckNear(p.GetX(), 1.0, 1e-5, "Origin x kept");
    CheckNear(p.GetY(), 2.0, 1e-5, "Origin y kept");
    CheckNear(p.GetZ(), 3.0, 1e-5, "Origin z kept");
    const auto com = reader.CenterOfMass();
    CheckNear(com.GetX(), 1.0, 1e-3, "COM x");
    CheckNear(com.GetY(), 7.0, 1e-3, "COM y");
    CheckNear(com.GetZ(), 3.0, 1e-3, "COM z");
}

// 매우 큰 크기의 쿼터니언이 오버플로 없이 정규화되어 받아들여지는지 검사한다.
static void TestHugeRotation()
{
    // {1e30,0,0,1e30}은 x축 90도 회전과 같다.
    PhysicsWorld world;
    world.Initialize(4);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    BodyDesc desc = MakeDesc(geom);
    desc.Rotation = { 1.0e30, 0, 0, 1.0e30 };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Huge quaternion accepted");

    // 저장된 회전은 정규화되어 있다.
    const BodyReader reader(world, world.CommitBody(std::move(prepared), false));
    const JPH::Quat q = reader.Rotation();
    CheckNear(q.GetX(), std::sqrt(0.5), 1e-5, "Huge quaternion x normalized");
    CheckNear(q.GetW(), std::sqrt(0.5), 1e-5, "Huge quaternion w normalized");
    CheckNear(q.LengthSq(), 1.0, 1e-5, "Huge quaternion unit length");

    // 극단적으로 작은 쿼터니언은 영벡터로 보고 거절한다.
    desc.Rotation = { 1.0e-40, 0, 0, 0 };
    Check(!world.PrepareBody(desc, prepared) && world.GetReservedCount() == 1, "Tiny quaternion rejected without leak");
}

// 8개 모서리로 정확한 상자 hull을 만든다. GenerateKDop18처럼 면을 부풀리지 않는다.
static AggregateGeom MakeExactBox(double hx, double hy, double hz)
{
    // 모서리 8개를 넣고 인덱스를 재구성한다.
    AggregateGeom geom;
    geom.ConvexElems.resize(1);
    for (int i = 0; i < 8; ++i)
    {
        geom.ConvexElems[0].VertexData.push_back({ (i & 1) ? hx : -hx, (i & 2) ? hy : -hy, (i & 4) ? hz : -hz });
    }
    geom.ConvexElems[0].UpdateElemBox();
    return geom;
}

// 두께 0.002 m와 0.0004 m의 정확한 얇은 상자를 cook해 결과를 기록한다. 성공하면 경계·부피·관성을 검사하고 실패하면 깨끗한 false여야 한다.
// 허용 오차는 Jolt hull tolerance 1e-3(면이 최대 이만큼 움직일 수 있음)과 float 반올림으로 잡는다. 부피 오차는 넓이 4 m² x 양면 x 1e-3이다.
static void TestThinHull()
{
    // 두께별로 준비를 시도하고 결과를 출력한다.
    const double halfThickness[2] = { 0.001, 0.0002 };
    for (const double half : halfThickness)
    {
        PhysicsWorld world;
        world.Initialize(4);
        const AggregateGeom geom = MakeExactBox(1.0, 1.0, half);
        Check(!geom.ConvexElems[0].IndexData.empty() && std::abs(Volume(geom) - 8.0 * half) < 1e-9, "Exact thin box volume");
        PreparedBody prepared;
        std::string error;
        const bool ok = world.PrepareBody(MakeDesc(geom), prepared, &error);

        // 실패하면 사유가 있고 예약이 남지 않아야 한다.
        if (!ok)
        {
            std::printf("Thin hull (%g m thick): cook failed (%s)\n", half * 2.0, error.c_str());
            Check(!error.empty() && !prepared.IsValid() && world.GetReservedCount() == 0, "Thin hull failure is clean");
            continue;
        }

        // 성공하면 로컬 경계, 부피, 관성을 입력과 비교한다.
        const BodyReader reader(world, world.CommitBody(std::move(prepared), false));
        const JPH::AABox bounds = reader.LocalBounds();
        const double dz = bounds.GetExtent().GetZ() - half;
        const auto inertia = reader.SortedInertia();
        std::printf("Thin hull (%g m thick): ok, half extents (%g, %g, %g) dz %g, volume %g vs %g, mass %g, inertia %g..%g\n", half * 2.0,
            bounds.GetExtent().GetX(), bounds.GetExtent().GetY(), bounds.GetExtent().GetZ(), dz,
            double(reader.ShapeVolume()), Volume(geom), reader.Mass(), inertia[0], inertia[2]);
        CheckNear(bounds.GetExtent().GetX(), 1.0, 2e-3, "Thin hull x bounds");
        CheckNear(bounds.GetExtent().GetY(), 1.0, 2e-3, "Thin hull y bounds");
        CheckNear(bounds.GetExtent().GetZ(), half, 2e-3, "Thin hull z bounds");
        CheckNear(reader.ShapeVolume(), Volume(geom), 8e-3, "Thin hull volume");
        Check(reader.Mass() >= 0.001 && std::isfinite(reader.Mass()), "Thin hull mass finite");
        Check(inertia[0] > 0.0 && std::isfinite(inertia[2]), "Thin hull inertia positive and finite");
    }
}

// 실제 Jolt를 링크한 body 수명 검증을 한 번에 실행한다.
int main()
{
    // 모든 검사를 순서대로 실행하고 예외가 나면 실패로 끝낸다.
    try
    {
        // 생성·scale·compound·실패 경로를 검사한다.
        TestDynamicBox();
        TestScaleAndMassScale();
        TestCompoundAndStatic();
        TestFailures();

        // 소유권·용량·수명을 검사한다.
        TestPreparedOwnership();
        TestCapacity();
        TestLifecycle();

        // 조회 격리와 pose, 극단 입력을 검사한다.
        TestQueryIsolation();
        TestOffCenter();
        TestHugeRotation();
        TestThinHull();

        // 통과한 검사 수를 출력한다.
        std::printf("PhysicsBodyTests: %zu checks passed\n", g_Checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "PhysicsBodyTests failed: %s\n", error.what());
        return 1;
    }
}
