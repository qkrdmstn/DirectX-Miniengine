// VS Developer PowerShell, 저장소 루트: 산출물은 TEMP에만 둔다.
// $bc = Join-Path $env:TEMP 'BreakConstraintTests'; New-Item -ItemType Directory -Force $bc | Out-Null
// $sdk = 'MiniEngine/Build/ThirdParty/Jolt/build'
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /MDd /DJPH_OBJECT_LAYER_BITS=16 /I MiniEngine/Physics /I "$sdk/_deps/joltsource-src" "/Fo:$bc/" "/Fe:$bc/PhysicsBodyTests.exe" MiniEngine/Physics/tests/PhysicsBodyTests.cpp MiniEngine/Physics/PhysicsWorld.cpp MiniEngine/Physics/PhysicsWorldBodies.cpp MiniEngine/Physics/PhysicsWorldConstraints.cpp MiniEngine/Physics/PhysicsWorldStep.cpp MiniEngine/Physics/Collision/ConvexElem.cpp MiniEngine/Physics/Collision/AggregateGeom.cpp "$sdk/lib/Debug/Jolt.lib" /link "/PDB:$bc/PhysicsBodyTests.pdb"
// & "$bc/PhysicsBodyTests.exe"; Release는 /MDd 대신 /MD /O2 /DNDEBUG, lib/Release/Jolt.lib를 쓴다.
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

// 세 축의 값을 같은 허용 오차로 비교한다.
static void CheckVector(const std::array<double, 3>& actual, const std::array<double, 3>& expected, double tolerance)
{
    // 축마다 검사해 어느 성분도 빠뜨리지 않는다.
    for (size_t axis = 0; axis < 3; ++axis)
        CheckNear(actual[axis], expected[axis], tolerance, "Prepared vector component");
}

// 비대칭 오프셋 hull의 준비 COM과 커밋 후 속도·한 스텝 운동을 검사한다.
static void TestPreparedMotion()
{
    // 비대칭 사면체를 회전해 원점에서 떨어진 위치에 준비한다.
    PhysicsWorld world;
    world.Initialize(4);
    AggregateGeom geom;
    geom.ConvexElems.resize(1);
    geom.ConvexElems[0].VertexData = { {2,0,0}, {6,0,0}, {2,2,0}, {2,0,1} };
    geom.ConvexElems[0].UpdateElemBox();
    BodyDesc desc = MakeDesc(geom);
    desc.Position = { 10, 20, 30 };
    const double halfRotation = std::sqrt(0.5);
    desc.Rotation = { 0, 0, halfRotation, halfRotation };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Prepare asymmetric rotated hull");
    std::array<double, 3> before{};
    Check(world.GetPreparedCenterOfMass(prepared, before), "Read prepared COM");
    CheckVector(before, { 9.5, 23, 30.25 }, 2e-3);

    // 미게시 상태에서 속도를 설정하고 COM·예약·비활성 상태가 유지되는지 확인한다.
    const std::array<double, 3> linear{ 3, 4, -2 };
    const std::array<double, 3> angular{ 0, 0, 1.5 };
    Check(world.SetPreparedVelocity(prepared, linear, angular), "Set prepared velocity");
    auto* impl = PhysicsWorldAccess::GetImpl(world);
    Check(world.GetBodyCount() == 0 && world.GetReservedCount() == 1, "Velocity preserves reservation");
    Check(impl->World.GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0, "Prepared velocity does not activate");
    std::array<double, 3> com{};
    Check(world.GetPreparedCenterOfMass(prepared, com), "Read COM after setting velocity");
    CheckVector(com, before, 0.0);
    const BodyHandle handle = world.CommitBody(std::move(prepared), true);
    Check(handle != InvalidBody && world.IsBodyAwake(handle), "Commit activates moving body");
    Check(world.GetBodyCenterOfMass(handle, com), "Read committed COM");
    CheckVector(com, before, 0.0);
    std::array<double, 3> v{};
    std::array<double, 3> w{};
    Check(world.GetBodyVelocity(handle, v, w), "Read velocity before stepping");
    CheckVector(v, linear, 1e-6);
    CheckVector(w, angular, 1e-6);

    // 회전하는 오프셋 hull은 원점 대신 COM 이동을 기본 감쇠·적분 오차 허용치 안에서 비교한다
    constexpr float dt = 1.0f / 60.0f;
    Check(world.Step(dt), "Step inherited velocity");
    Check(world.GetBodyCenterOfMass(handle, com), "Read stepped COM");
    CheckVector(com, { before[0] + linear[0] * dt, before[1] + linear[1] * dt - 4.9 * dt * dt,
        before[2] + linear[2] * dt }, 0.002);
    Check(world.GetBodyVelocity(handle, v, w), "Read stepped angular velocity");
    CheckVector(w, angular, 0.003);
    std::array<double, 3> position{};
    std::array<double, 4> rotation{};
    Check(world.GetBodyPose(handle, position, rotation), "Read stepped rotation");
    Check(std::abs(rotation[2] - desc.Rotation[2]) > 0.001, "Prepared angular velocity rotates body");
}

// 부모의 월드 COM과 속도로 서로 다른 두 조각의 강체 속도 상속 공식을 검사한다.
static void TestPreparedInheritance()
{
    // 부모의 속도와 COM을 실제 게시 body에서 읽는다.
    PhysicsWorld world;
    world.Initialize(4);
    const AggregateGeom parentGeom = MakeBoxGeom({ 0, 0, 0 }, { 2, 1, 1 });
    BodyDesc desc = MakeDesc(parentGeom);
    desc.Position = { 10, 20, 30 };
    const double halfRotation = std::sqrt(0.5);
    desc.Rotation = { 0, 0, halfRotation, halfRotation };
    PreparedBody parent;
    Check(world.PrepareBody(desc, parent), "Prepare moving parent");
    Check(world.SetPreparedVelocity(parent, { 2, -1, 3 }, { 0.5, -0.75, 2 }), "Set parent velocity");
    const BodyHandle parentHandle = world.CommitBody(std::move(parent), true);
    std::array<double, 3> parentCom{};
    std::array<double, 3> v{};
    std::array<double, 3> w{};
    Check(world.GetBodyCenterOfMass(parentHandle, parentCom), "Read parent COM");
    Check(world.GetBodyVelocity(parentHandle, v, w), "Read parent velocity");

    // 서로 다른 로컬 중심을 가진 두 조각에 v + w x r을 커밋 전에 적용한다.
    for (const double offset : { -1.0, 1.0 })
    {
        const AggregateGeom pieceGeom = MakeBoxGeom({ offset, 0, 0 }, { 0.5, 0.5, 0.5 });
        desc.Geometry = &pieceGeom;
        PreparedBody piece;
        Check(world.PrepareBody(desc, piece), "Prepare offset piece");
        std::array<double, 3> com{};
        Check(world.GetPreparedCenterOfMass(piece, com), "Read piece COM");
        CheckVector(com, { 10, 20 + offset, 30 }, 1e-3);
        const std::array<double, 3> r{ com[0] - parentCom[0], com[1] - parentCom[1], com[2] - parentCom[2] };
        const std::array<double, 3> inherited{ v[0] + w[1] * r[2] - w[2] * r[1],
            v[1] + w[2] * r[0] - w[0] * r[2], v[2] + w[0] * r[1] - w[1] * r[0] };
        Check(world.SetPreparedVelocity(piece, inherited, w), "Set inherited piece velocity");
        const BodyHandle handle = world.CommitBody(std::move(piece), true);
        std::array<double, 3> actualV{};
        std::array<double, 3> actualW{};
        Check(world.GetBodyVelocity(handle, actualV, actualW), "Read inherited piece velocity");
        CheckVector(actualV, inherited, 1e-5);
        CheckVector(actualW, w, 1e-6);
    }
}

// float 최대 성분도 Jolt 내부 길이 제곱을 넘치게 하지 않고 방향을 보존해 제한하는지 검사한다.
static void TestPreparedVelocityLimit()
{
    // 양수·음수 최대 성분을 가진 선속도와 각속도를 미게시 body에 설정한다.
    PhysicsWorld world;
    world.Initialize(4);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 1, 1, 1 });
    PreparedBody prepared;
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare float limit velocity");
    const double maximum = double(std::numeric_limits<float>::max());
    Check(world.SetPreparedVelocity(prepared, { maximum, -maximum, maximum }, { -maximum, maximum, -maximum }), "Float limit velocity accepted");
    const BodyHandle handle = world.CommitBody(std::move(prepared), true);
    std::array<double, 3> v{};
    std::array<double, 3> w{};
    Check(world.GetBodyVelocity(handle, v, w), "Read limited velocity");

    // Jolt의 설정된 최대 길이와 단위 방향에 일치해야 한다.
    const BodyReader reader(world, handle);
    const JPH::BodyLockRead lock(reader.Locks(), reader.Id);
    const auto* motion = lock.GetBody().GetMotionProperties();
    const double linear = double(motion->GetMaxLinearVelocity()) / std::sqrt(3.0);
    const double angular = double(motion->GetMaxAngularVelocity()) / std::sqrt(3.0);
    CheckVector(v, { linear, -linear, linear }, 1e-3);
    CheckVector(w, { -angular, angular, -angular }, 1e-4);
}

// 잘못된 입력·소유권·수명이 준비 상태와 기존 속도를 변경하지 않는지 검사한다.
static void TestPreparedVelocityFailures()
{
    // 빈 객체와 초기화되지 않은 world는 출력도 건드리지 않는다.
    PhysicsWorld world;
    PreparedBody prepared;
    const std::array<double, 3> sentinel{ 7, 8, 9 };
    std::array<double, 3> com = sentinel;
    Check(!world.GetPreparedCenterOfMass(prepared, com) && com == sentinel, "Uninitialized COM rejected");
    Check(!world.SetPreparedVelocity(prepared, {}, {}), "Uninitialized velocity rejected");
    world.Initialize(8);
    Check(!world.GetPreparedCenterOfMass(prepared, com) && com == sentinel, "Empty prepared COM rejected");
    Check(!world.SetPreparedVelocity(prepared, {}, {}), "Empty prepared velocity rejected");
    const AggregateGeom geom = MakeBoxGeom({ 2, 0, 0 }, { 1, 1, 1 });
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare failure survivor");
    const std::array<double, 3> linear{ 1, 2, 3 };
    const std::array<double, 3> angular{ -1, 0.5, 2 };
    Check(world.SetPreparedVelocity(prepared, linear, angular), "Seed velocity before failures");
    std::array<double, 3> originalCom{};
    Check(world.GetPreparedCenterOfMass(prepared, originalCom), "Read original COM");

    // 각 배열의 모든 성분에서 NaN·무한대·float 범위 초과를 거절한다.
    for (const double invalid : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        double(std::numeric_limits<float>::max()) * 2, -double(std::numeric_limits<float>::max()) * 2 })
    {
        for (size_t axis = 0; axis < 3; ++axis)
        {
            auto bad = linear;
            bad[axis] = invalid;
            Check(!world.SetPreparedVelocity(prepared, bad, {}), "Invalid linear rejected atomically");
            Check(!world.SetPreparedVelocity(prepared, {}, bad), "Invalid angular rejected atomically");
        }
    }

    // 다른 world에서는 접근을 거절하며 원래 소유자는 같은 COM과 속도로 커밋할 수 있다.
    PhysicsWorld other;
    other.Initialize(4);
    Check(!other.GetPreparedCenterOfMass(prepared, com) && com == sentinel, "Other world COM rejected");
    Check(!other.SetPreparedVelocity(prepared, {}, {}), "Other world velocity rejected");
    Check(world.GetPreparedCenterOfMass(prepared, com), "Failure preserves prepared COM");
    CheckVector(com, originalCom, 0.0);
    const BodyHandle handle = world.CommitBody(std::move(prepared), true);
    std::array<double, 3> v{};
    std::array<double, 3> w{};
    Check(world.GetBodyVelocity(handle, v, w), "Read unchanged velocity after failures");
    CheckVector(v, linear, 1e-6);
    CheckVector(w, angular, 1e-6);
    Check(!world.SetPreparedVelocity(prepared, {}, {}), "Consumed prepared velocity rejected");

    // 정적 준비 body는 COM 조회만 허용하고 속도 설정 후에도 영속도를 유지한다.
    BodyDesc desc = MakeDesc(geom);
    desc.Motion = BodyMotion::Static;
    Check(world.PrepareBody(desc, prepared), "Prepare static failure survivor");
    Check(world.GetPreparedCenterOfMass(prepared, com), "Static prepared COM readable");
    Check(!world.SetPreparedVelocity(prepared, linear, angular), "Static velocity rejected");
    Check(world.GetBodyVelocity(world.CommitBody(std::move(prepared), true), v, w), "Read unchanged static velocity");
    CheckVector(v, {}, 0.0);
    CheckVector(w, {}, 0.0);

    // Shutdown과 재초기화 뒤에도 고아 객체는 그대로 남으며 새 world에 접근하지 않는다.
    Check(world.PrepareBody(MakeDesc(geom), prepared), "Prepare shutdown orphan");
    world.Shutdown();
    com = sentinel;
    Check(!world.GetPreparedCenterOfMass(prepared, com) && com == sentinel, "Shutdown COM rejected");
    Check(!world.SetPreparedVelocity(prepared, linear, angular) && prepared.IsValid(), "Shutdown velocity rejected");
    world.Initialize(4);
    Check(!world.SetPreparedVelocity(prepared, linear, angular) && prepared.IsValid(), "Reinitialized orphan rejected");

    // 소유 world가 파괴된 객체도 다른 살아 있는 world에서 안전하게 거절한다.
    PreparedBody orphan;
    {
        PhysicsWorld owner;
        owner.Initialize(4);
        Check(owner.PrepareBody(MakeDesc(geom), orphan), "Prepare destroyed world orphan");
    }
    Check(!world.GetPreparedCenterOfMass(orphan, com) && com == sentinel, "Destroyed world COM rejected");
    Check(!world.SetPreparedVelocity(orphan, linear, angular) && orphan.IsValid(), "Destroyed world velocity rejected");
}

// Kinematic 목표·COM·운동 방식 전환과 impulse 및 실패 보존을 검사한다.
static void TestKinematicMotion()
{
    // 오프셋 상자를 중력 없이 유지하고 Moving 계층과 준비된 관성을 확인한다.
    PhysicsWorld world;
    world.Initialize(8);
    const auto geom = MakeBoxGeom({ 2, 0, 0 }, { 0.5, 0.5, 0.5 });
    auto desc = MakeDesc(geom);
    desc.Motion = BodyMotion::Kinematic;
    desc.Position = { 0, 10, 0 };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Prepare kinematic");
    const auto handle = world.CommitBody(std::move(prepared), false);
    const BodyReader reader(world, handle);
    const double mass = MiniCollision::CalculateUnrealMass(Volume(geom));
    Check(PhysicsWorldAccess::GetImpl(world)->World.GetBodyInterface().GetObjectLayer(reader.Id) == ObjectLayers::Moving, "Kinematic moving layer");
    Check(world.Step(1.0f / 60), "Rest kinematic step");
    std::array<double, 3> position{}, linear{}, angular{};
    std::array<double, 4> rotation{};
    Check(world.GetBodyPose(handle, position, rotation), "Read kinematic rest");
    CheckVector(position, desc.Position, 1e-5);

    // 회전 목표의 원점과 COM이 다음 step에 도달하며 호출 즉시는 pose가 그대로다.
    const double h = std::sqrt(0.5);
    Check(world.MoveKinematic(handle, { 1, 10, 0 }, { 0, 0, h, h }, 1.0f / 60), "Set kinematic target");
    Check(world.GetBodyPose(handle, position, rotation), "Read before target step");
    CheckVector(position, desc.Position, 1e-5);
    Check(world.GetBodyVelocity(handle, linear, angular), "Read target COM velocity");
    CheckVector(linear, { -60, 120, 0 }, 0.01);
    CheckNear(angular[2], 30 * 3.141592653589793, 0.01, "Target world angular velocity");
    Check(world.Step(1.0f / 60), "Target step");
    Check(world.GetBodyPose(handle, position, rotation), "Read reached target");
    CheckVector(position, { 1, 10, 0 }, 1e-4);
    CheckNear(rotation[2], h, 1e-5, "Target rotation");
    Check(world.SetBodyVelocity(handle, { 1, 2, 3 }, { 0, 0, 0.5 }), "Set kinematic velocity");

    // 같은 handle·BodyID·pose·속도를 유지하며 왕복 전환하고 Dynamic은 중력으로 낙하한다.
    Check(world.SetBodyMotion(handle, BodyMotion::Dynamic), "Switch to dynamic");
    Check(world.SetBodyMotion(handle, BodyMotion::Dynamic), "Repeated dynamic preserves state");
    Check(world.GetBodyPose(handle, position, rotation), "Read switched pose");
    CheckVector(position, { 1, 10, 0 }, 1e-4);
    Check(world.GetBodyVelocity(handle, linear, angular), "Read switched velocity");
    CheckVector(linear, { 1, 2, 3 }, 1e-6);
    CheckVector(angular, { 0, 0, 0.5 }, 1e-6);
    CheckNear(reader.Mass(), mass, 1e-5, "Switch preserves mass");
    Check(reader.SortedInertia()[0] > 0, "Kinematic inertia survives conversion");
    Check(world.SetBodyMotion(handle, BodyMotion::Kinematic), "Switch back to kinematic");
    Check(world.SetBodyMotion(handle, BodyMotion::Dynamic), "Switch again to dynamic");
    Check(world.GetBodyVelocity(handle, linear, angular), "Read round-trip velocity");
    CheckVector(linear, { 1, 2, 3 }, 1e-6);
    CheckVector(angular, { 0, 0, 0.5 }, 1e-6);
    Check(world.GetBodyCount() == 1 && PhysicsWorldAccess::GetImpl(world)->SimBodies.at(handle) == reader.Id, "Handle and ID retained");
    Check(world.SetBodyVelocity(handle, {}, {}), "Clear velocity");
    Check(world.Step(1.0f / 60), "Dynamic gravity step");
    Check(world.GetBodyVelocity(handle, linear, angular) && linear[1] < 0, "Dynamic falls");

    // 정지시킨 body의 COM에서 벗어난 +X impulse는 +Z 회전을 만든다.
    Check(world.SetBodyVelocity(handle, {}, {}), "Reset impulse velocity");
    std::array<double, 3> com{};
    Check(world.GetBodyCenterOfMass(handle, com), "Read impulse COM");
    auto point = com;
    point[1] -= 1;
    PhysicsWorldAccess::GetImpl(world)->World.GetBodyInterface().DeactivateBody(reader.Id);
    Check(!world.IsBodyAwake(handle), "Impulse starts asleep");
    Check(world.AddBodyImpulse(handle, { mass, 0, 0 }, point), "Apply off-center impulse");
    Check(world.IsBodyAwake(handle), "Impulse wakes body");
    Check(world.GetBodyVelocity(handle, linear, angular), "Read impulse result");
    CheckVector(linear, { 1, 0, 0 }, 1e-5);
    Check(angular[2] > 0 && std::abs(angular[0]) < 1e-5, "Off-center impulse rotates");
    CheckNear(angular[2], 6.0, 0.3, "Box impulse angular response");

    // 무효 입력과 비지원 상태를 거절하고 속도와 자원 수를 보존한다.
    const auto savedLinear = linear;
    const auto savedAngular = angular;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Check(!world.MoveKinematic(handle, {}, { 0, 0, 0, 1 }, 1), "Dynamic target rejected");
    Check(!world.SetBodyMotion(handle, BodyMotion::Static), "Static conversion rejected");
    Check(!world.SetBodyMotion(handle, static_cast<BodyMotion>(99)), "Invalid motion rejected");
    Check(!world.AddBodyImpulse(handle, { nan, 0, 0 }, com), "NaN impulse rejected");
    Check(!world.AddBodyImpulse(handle, { 1e300, 0, 0 }, com), "Huge impulse rejected");
    Check(!world.SetBodyVelocity(handle, {}, { nan, 0, 0 }), "NaN velocity rejected");
    Check(!world.AddBodyImpulse(handle, { 1, 0, 0 }, { nan, 0, 0 }), "NaN point rejected");
    Check(!world.AddBodyImpulse(handle, { double(FLT_MAX), 0, 0 }, point), "Overflow response rejected");
    Check(world.GetBodyVelocity(handle, linear, angular), "Read failure survivor");
    CheckVector(linear, savedLinear, 0);
    CheckVector(angular, savedAngular, 0);
    Check(world.SetBodyMotion(handle, BodyMotion::Kinematic), "Kinematic failures setup");
    Check(!world.AddBodyImpulse(handle, { 1, 0, 0 }, com), "Kinematic impulse rejected");
    Check(!world.MoveKinematic(handle, {}, { 0, 0, 0, 1 }, 0), "Zero dt rejected");
    Check(!world.MoveKinematic(handle, {}, { 0, 0, 0, 1 }, float(nan)), "NaN dt rejected");
    Check(!world.MoveKinematic(handle, { nan, 0, 0 }, { 0, 0, 0, 1 }, 1), "NaN target rejected");
    Check(!world.MoveKinematic(handle, {}, {}, 1), "Zero rotation rejected");
    Check(!world.MoveKinematic(handle, {}, { 0, 0, 0, 1 }, std::numeric_limits<float>::denorm_min()), "Overflow target velocity rejected");
    Check(!world.MoveKinematic(handle, {}, { 0, 0, 0, 1 }, -1), "Negative dt rejected");
    Check(!world.MoveKinematic(handle, {}, { 0, 0, 0, 1 }, std::numeric_limits<float>::infinity()), "Infinite dt rejected");
    Check(world.GetBodyVelocity(handle, linear, angular), "Read rejected target velocity");
    CheckVector(linear, savedLinear, 0);
    CheckVector(angular, savedAngular, 0);
    Check(world.SetBodyShape(handle, geom, { 1, 1, 1 }, 2), "Replace kinematic shape and mass");
    Check(world.SetBodyMotion(handle, BodyMotion::Dynamic), "Convert replaced kinematic");
    CheckNear(reader.Mass(), 2 * mass, 1e-5, "Replacement mass retained");
    Check(world.RemoveBodyFromWorld(handle), "Remove transition body");
    Check(!world.SetBodyMotion(handle, BodyMotion::Dynamic), "Removed motion rejected");
    Check(!world.MoveKinematic(handle, {}, { 0, 0, 0, 1 }, 1), "Removed target rejected");
    Check(!world.SetBodyVelocity(handle, {}, {}), "Removed velocity rejected");
    Check(world.DestroyBody(handle), "Destroy transition body");
    Check(!world.AddBodyImpulse(handle, {}, {}) && !world.SetBodyMotion(handle, BodyMotion::Dynamic), "Stale handle rejected");
    Check(world.GetBodyCount() == 0 && world.GetReservedCount() == 0, "No transition resource leak");

    // 기존 Static body는 전환을 허용하지 않으며 초기화 전 API도 안전하게 거절한다.
    desc.Motion = BodyMotion::Static;
    Check(world.PrepareBody(desc, prepared), "Prepare static transition guard");
    const auto fixed = world.CommitBody(std::move(prepared), false);
    Check(!world.SetBodyMotion(fixed, BodyMotion::Dynamic), "Static body conversion rejected");
    Check(!world.SetBodyVelocity(fixed, {}, {}) && !world.AddBodyImpulse(fixed, {}, {}), "Static mutations rejected");
    world.Clear();
    PhysicsWorld unopened;
    Check(!unopened.SetBodyMotion(fixed, BodyMotion::Dynamic), "Uninitialized motion rejected");
    Check(!unopened.MoveKinematic(fixed, {}, { 0, 0, 0, 1 }, 1), "Uninitialized target rejected");
    Check(!unopened.SetBodyVelocity(fixed, {}, {}) && !unopened.AddBodyImpulse(fixed, {}, {}), "Uninitialized velocity and impulse rejected");
    Check(world.GetReservedCount() == 0, "Static guard cleaned");
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

        // 준비 COM과 커밋 전 속도 설정·상속·실패 보존을 검사한다.
        TestPreparedMotion();
        TestPreparedInheritance();
        TestPreparedVelocityLimit();
        TestPreparedVelocityFailures();

        // Kinematic 이동과 운동 방식 전환 및 impulse를 검사한다.
        TestKinematicMotion();

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
