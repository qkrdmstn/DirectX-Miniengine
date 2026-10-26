#include "CollisionTestUtils.h"
#include "PhysicsWorldInternal.h"
#include "Collision/AggregateGeom.h"
#include "Collision/BodyMass.h"
#include <algorithm>
#include <cstdio>
#include <limits>

using namespace MiniPhysics;

constexpr float StepSeconds = 1.0f / 60.0f;

// 중심과 반크기로 8개 모서리의 18-DOP hull 요소 하나를 가진 지오메트리를 만든다.
static AggregateGeom MakeBoxGeom(Vec3d c, Vec3d half)
{
    // 모서리 8개를 모아 hull을 만든다.
    std::vector<Vec3d> corners;
    for (int i = 0; i < 8; ++i)
    {
        corners.push_back({ c.x + ((i & 1) ? half.x : -half.x), c.y + ((i & 2) ? half.y : -half.y), c.z + ((i & 4) ? half.z : -half.z) });
    }

    // 18-DOP hull 하나가 만들어졌는지 확인하고 반환한다.
    AggregateGeom geom;
    Check(GenerateKDop18(corners, geom) && geom.ConvexElems.size() == 1, "Box geometry builds");
    return geom;
}

// 지오메트리 요소 부피의 합을 구한다.
static double Volume(const AggregateGeom& geom)
{
    // 요소별 부피를 더한다.
    double sum = 0.0;
    for (const auto& elem : geom.ConvexElems)
    {
        sum += elem.GetVolume();
    }
    return sum;
}

// 지오메트리로 body를 준비·게시하고 handle을 반환한다.
static BodyHandle AddBody(PhysicsWorld& world, const AggregateGeom& geom, std::array<double, 3> position, BodyMotion motion)
{
    // 요청을 채운다.
    BodyDesc desc;
    desc.Geometry = &geom;
    desc.Position = position;
    desc.Motion = motion;

    // 준비한 뒤 활성 상태로 게시한다.
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Prepare body");
    const BodyHandle handle = world.CommitBody(std::move(prepared), true);
    Check(handle != InvalidBody, "Commit body");
    return handle;
}

// body의 질량·로컬 경계·관성을 Jolt에서 직접 읽는다.
struct BodyReader
{
    PhysicsWorld& World;
    JPH::BodyID Id;

    // handle을 내부 BodyID로 바꾼다.
    BodyReader(PhysicsWorld& world, BodyHandle handle) : World(world), Id(PhysicsWorldAccess::GetImpl(world)->SimBodies.at(handle)) {}

    // Jolt body 잠금 인터페이스를 반환한다.
    const JPH::BodyLockInterface& Locks() const { return PhysicsWorldAccess::GetImpl(World)->World.GetBodyLockInterface(); }

    // 질량을 역질량에서 읽는다. 정적 body는 0이다.
    double Mass() const
    {
        // body를 잠그고 정적이면 0을 돌려준다.
        const JPH::BodyLockRead lock(Locks(), Id);
        if (!lock.GetBody().IsDynamic())
        {
            return 0.0;
        }

        // 역질량의 역수를 질량으로 돌려준다.
        const float inverse = lock.GetBody().GetMotionProperties()->GetInverseMass();
        return inverse > 0.0f ? 1.0 / inverse : 0.0;
    }

    // shape의 body 로컬 경계 상자를 읽는다.
    JPH::AABox LocalBounds() const
    {
        // body를 잠그고 shape의 로컬 경계를 돌려준다.
        const JPH::BodyLockRead lock(Locks(), Id);
        return lock.GetBody().GetShape()->GetLocalBounds();
    }

    // 관성 대각 성분 y를 읽는다.
    double InertiaY() const
    {
        // body를 잠그고 역관성 y 성분의 역수를 돌려준다.
        const JPH::BodyLockRead lock(Locks(), Id);
        return 1.0 / lock.GetBody().GetMotionProperties()->GetInverseInertiaDiagonal().GetY();
    }

    // 로컬 공간 역관성 행렬의 (행, 열) 성분을 읽는다.
    float InverseInertia(int row, int column) const
    {
        // body를 잠그고 3x3 역관성 행렬에서 한 성분을 꺼낸다.
        const JPH::BodyLockRead lock(Locks(), Id);
        return lock.GetBody().GetMotionProperties()->GetLocalSpaceInverseInertia()(row, column);
    }

    // 원점에서 질량 중심까지의 월드 오프셋을 읽는다.
    JPH::Vec3 CenterOffset() const
    {
        // body를 잠그고 질량 중심과 원점의 차를 돌려준다.
        const JPH::BodyLockRead lock(Locks(), Id);
        return JPH::Vec3(lock.GetBody().GetCenterOfMassPosition() - lock.GetBody().GetPosition());
    }
};

// 처음 잠들 때까지 step하고 걸린 step 수를 반환한다. 600 step 안에 잠들지 않으면 실패한다.
static int StepUntilAsleep(PhysicsWorld& world, BodyHandle body)
{
    // 잠들 때까지 한 step씩 진행한다.
    for (int i = 1; i <= 600; ++i)
    {
        Check(world.Step(StepSeconds), "Step succeeds");
        if (!world.IsBodyAwake(body))
        {
            return i;
        }
    }
    Check(false, "Body falls asleep within 10 s");
    return -1;
}

// 잠든 동적 body를 순간이동하면 즉시 깨어나 다시 낙하하고 정착하는지 검사한다.
static void TestTeleportWakesSleeper()
{
    // 바닥 위에 떨어뜨린 정육면체가 잠들 때까지 기다린다.
    PhysicsWorld world;
    world.Initialize(16);
    const AggregateGeom floorGeom = MakeBoxGeom({ 0, -0.5, 0 }, { 10, 0.5, 10 });
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    AddBody(world, floorGeom, { 0, 0, 0 }, BodyMotion::Static);
    const BodyHandle cube = AddBody(world, cubeGeom, { 0, 2, 0 }, BodyMotion::Dynamic);
    const int firstSleep = StepUntilAsleep(world, cube);

    // 공중으로 순간이동하면 즉시 깨어나고 pose가 정확히 요청값이다.
    Check(world.SetBodyTransform(cube, { 2, 3, -1 }, { 0, 0, 0, 1 }), "Teleport sleeping body");
    Check(world.IsBodyAwake(cube), "Teleport wakes body");
    std::array<double, 3> p;
    std::array<double, 4> q;
    Check(world.GetBodyPose(cube, p, q), "Pose readable");
    CheckNear(p[0], 2.0, 1e-6, "Teleport x");
    CheckNear(p[1], 3.0, 1e-6, "Teleport y");
    CheckNear(p[2], -1.0, 1e-6, "Teleport z");

    // 다시 낙하해 바닥 위(중심 y = 0.5)에서 잠든다.
    const int secondSleep = StepUntilAsleep(world, cube);
    Check(world.GetBodyPose(cube, p, q), "Pose readable after settle");
    CheckNear(p[1], 0.5, 0.02, "Cube settles back on floor");
    std::printf("teleport wake: first sleep %d steps, after teleport sleep %d steps, final y=%.4f\n", firstSleep, secondSleep, p[1]);
}

// 순간이동이 속도를 리셋하지 않고 이후 step에서도 이어지는지 검사한다.
static void TestVelocityPreserved()
{
    // 높은 곳에서 0.5초 자유낙하시켜 속도를 만든다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    const BodyHandle cube = AddBody(world, cubeGeom, { 0, 50, 0 }, BodyMotion::Dynamic);
    for (int i = 0; i < 30; ++i)
    {
        Check(world.Step(StepSeconds), "Step succeeds");
    }
    std::array<double, 3> vBefore;
    std::array<double, 3> w;
    Check(world.GetBodyVelocity(cube, vBefore, w) && vBefore[1] < -4.0, "Cube is falling");

    // 순간이동 직후 속도가 그대로다.
    Check(world.SetBodyTransform(cube, { 5, 40, 0 }, { 0, 0, 0, 1 }), "Teleport falling body");
    std::array<double, 3> vAfter;
    Check(world.GetBodyVelocity(cube, vAfter, w), "Velocity readable");
    for (int i = 0; i < 3; ++i)
    {
        CheckNear(vAfter[i], vBefore[i], 1e-9, "Velocity unchanged by teleport");
    }

    // 한 step 뒤에도 계속 가속하며 낙하한다.
    Check(world.Step(StepSeconds), "Step succeeds");
    std::array<double, 3> vNext;
    Check(world.GetBodyVelocity(cube, vNext, w) && vNext[1] < vBefore[1], "Fall continues after teleport");
    std::printf("velocity: before %.4f, right after teleport %.4f, next step %.4f m/s\n", vBefore[1], vAfter[1], vNext[1]);
}

// 정적 body와 월드에서 빠진 body도 pose만 바뀌고 깨어나지 않는지 검사한다.
static void TestStaticAndRemoved()
{
    // 바닥과 동적 body를 만든다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom floorGeom = MakeBoxGeom({ 0, -0.5, 0 }, { 10, 0.5, 10 });
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    const BodyHandle floorBody = AddBody(world, floorGeom, { 0, 0, 0 }, BodyMotion::Static);
    const BodyHandle cube = AddBody(world, cubeGeom, { 0, 2, 0 }, BodyMotion::Dynamic);

    // 정적 body를 옮기면 pose만 바뀌고 깨어나지 않는다.
    Check(world.SetBodyTransform(floorBody, { 0, -1, 0 }, { 0, 0, 0, 1 }), "Teleport static body");
    std::array<double, 3> p;
    std::array<double, 4> q;
    Check(world.GetBodyPose(floorBody, p, q), "Static pose readable");
    CheckNear(p[1], -1.0, 1e-6, "Static body moved");
    Check(!world.IsBodyAwake(floorBody), "Static body stays not awake");

    // 월드에서 뺀 동적 body는 pose만 바뀌고 깨어나지 않는다.
    Check(world.RemoveBodyFromWorld(cube), "Remove body from world");
    Check(world.SetBodyTransform(cube, { 1, 7, 2 }, { 0, 0, 0, 1 }), "Teleport removed body");
    Check(world.GetBodyPose(cube, p, q), "Removed pose readable");
    CheckNear(p[1], 7.0, 1e-6, "Removed body moved");
    Check(!world.IsBodyAwake(cube), "Removed body not awake");

    // 다시 올리면 새 위치에서 시작한다.
    Check(world.AddBodyToWorld(cube, true), "Add body back");
    Check(world.GetBodyPose(cube, p, q), "Pose readable after re-add");
    CheckNear(p[0], 1.0, 1e-6, "Re-added body keeps new pose");
}

// 무효한 handle과 입력은 false이고 body를 바꾸지 않는지 검사한다.
static void TestTransformInvalid()
{
    // 초기화 전 world는 거절한다.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    PhysicsWorld empty;
    Check(!empty.SetBodyTransform(1, { 0, 0, 0 }, { 0, 0, 0, 1 }), "Uninitialized world rejects teleport");

    // 낙하 전 정지 상태의 body를 준비한다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    const BodyHandle cube = AddBody(world, cubeGeom, { 1, 2, 3 }, BodyMotion::Dynamic);

    // 알 수 없는 handle과 무효 값은 모두 거절된다.
    const std::array<double, 4> identity = { 0, 0, 0, 1 };
    Check(!world.SetBodyTransform(InvalidBody, { 0, 0, 0 }, identity), "Invalid handle rejected");
    Check(!world.SetBodyTransform(cube + 100, { 0, 0, 0 }, identity), "Unknown handle rejected");
    Check(!world.SetBodyTransform(cube, { nan, 0, 0 }, identity), "NaN position rejected");
    Check(!world.SetBodyTransform(cube, { 0, inf, 0 }, identity), "Infinite position rejected");
    Check(!world.SetBodyTransform(cube, { 1e10, 0, 0 }, identity), "Huge position rejected");
    Check(!world.SetBodyTransform(cube, { 0, 0, 0 }, { 0, 0, 0, 0 }), "Zero quaternion rejected");
    Check(!world.SetBodyTransform(cube, { 0, 0, 0 }, { nan, 0, 0, 1 }), "NaN rotation rejected");

    // 거절된 호출은 pose를 바꾸지 않는다.
    std::array<double, 3> p;
    std::array<double, 4> q;
    Check(world.GetBodyPose(cube, p, q), "Pose readable");
    CheckNear(p[0], 1.0, 1e-6, "Pose x unchanged");
    CheckNear(p[1], 2.0, 1e-6, "Pose y unchanged");
    CheckNear(p[2], 3.0, 1e-6, "Pose z unchanged");
    CheckNear(q[3], 1.0, 1e-6, "Rotation unchanged");

    // 파괴한 handle도 거절한다.
    Check(world.DestroyBody(cube), "Destroy body");
    Check(!world.SetBodyTransform(cube, { 0, 0, 0 }, identity), "Destroyed handle rejected");
}

// 교체 성공 시 질량·경계·관성이 새 scale 기준으로 바뀌고 pose와 속도는 유지되는지 검사한다.
static void TestShapeReplace()
{
    // 1 m 정육면체를 회전된 pose로 만들고 낙하 속도를 준다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    const double volume = Volume(cubeGeom);
    BodyDesc desc;
    desc.Geometry = &cubeGeom;
    desc.Position = { 1, 20, 3 };
    desc.Rotation = { 0.3826834, 0, 0, 0.9238795 };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Prepare rotated body");
    const BodyHandle cube = world.CommitBody(std::move(prepared), true);
    for (int i = 0; i < 20; ++i)
    {
        Check(world.Step(StepSeconds), "Step succeeds");
    }

    // 교체 전 상태를 기록한다.
    const BodyReader reader(world, cube);
    std::array<double, 3> pBefore;
    std::array<double, 4> qBefore;
    std::array<double, 3> vBefore;
    std::array<double, 3> w;
    Check(world.GetBodyPose(cube, pBefore, qBefore) && world.GetBodyVelocity(cube, vBefore, w), "State readable");
    const double massBefore = reader.Mass();
    const double sizeBefore = reader.LocalBounds().GetSize().GetX();
    CheckNear(massBefore, MiniCollision::CalculateUnrealMass(volume), massBefore * 1e-4, "Initial mass follows Unreal formula");

    // scale 2로 교체하면 질량은 8배 부피 기준 Unreal 값이고 크기는 2배다.
    std::string error;
    Check(world.SetBodyShape(cube, cubeGeom, { 2, 2, 2 }, 1.0, &error), "Shape replace succeeds");
    const double massAfter = reader.Mass();
    CheckNear(massAfter, MiniCollision::CalculateUnrealMass(volume * 8.0), massAfter * 1e-4, "Mass after scale 2 follows Unreal formula");
    CheckNear(reader.LocalBounds().GetSize().GetX(), sizeBefore * 2.0, 1e-3, "Bounds doubled");

    // 같은 scale로 새로 만든 body와 관성이 같고 pose와 속도는 유지된다.
    BodyDesc fresh = desc;
    fresh.Scale = { 2, 2, 2 };
    PreparedBody freshPrepared;
    Check(world.PrepareBody(fresh, freshPrepared), "Prepare fresh scaled body");
    const BodyHandle freshBody = world.CommitBody(std::move(freshPrepared), false);
    CheckNear(reader.InertiaY(), BodyReader(world, freshBody).InertiaY(), reader.InertiaY() * 1e-3, "Inertia matches fresh scaled body");
    std::array<double, 3> pAfter;
    std::array<double, 4> qAfter;
    std::array<double, 3> vAfter;
    Check(world.GetBodyPose(cube, pAfter, qAfter) && world.GetBodyVelocity(cube, vAfter, w), "State readable after replace");
    for (int i = 0; i < 3; ++i)
    {
        CheckNear(pAfter[i], pBefore[i], 1e-6, "Position kept");
        CheckNear(vAfter[i], vBefore[i], 1e-9, "Velocity kept");
    }
    for (int i = 0; i < 4; ++i)
    {
        CheckNear(qAfter[i], qBefore[i], 1e-6, "Rotation kept");
    }

    // MassScale도 질량에 반영되고 정적 body는 질량 0으로 경계만 바뀐다.
    Check(world.SetBodyShape(cube, cubeGeom, { 1, 1, 1 }, 2.0), "Shape replace with mass scale");
    CheckNear(reader.Mass(), MiniCollision::CalculateUnrealMass(volume, 2.0), reader.Mass() * 1e-4, "Mass scale applied");
    const BodyHandle floorBody = AddBody(world, cubeGeom, { 0, 0, 0 }, BodyMotion::Static);
    Check(world.SetBodyShape(floorBody, cubeGeom, { 4, 1, 4 }, 1.0), "Static shape replace");
    const BodyReader floorReader(world, floorBody);
    CheckNear(floorReader.LocalBounds().GetSize().GetX(), sizeBefore * 4.0, 1e-3, "Static bounds changed");
    CheckNear(floorReader.Mass(), 0.0, 0.0, "Static mass stays zero");
    std::printf("shape replace: mass %.4f -> %.4f kg (expected %.4f), bounds %.3f -> %.3f m\n",
        massBefore, massAfter, MiniCollision::CalculateUnrealMass(volume * 8.0), sizeBefore, sizeBefore * 2.0);
}

// 원점에서 떨어진 비대칭 hull을 비균일 scale로 교체해도 원점·속도가 유지되고 질량 중심·관성이 새로 만든 body와 같은지 검사한다.
static void TestShapeReplaceOffCenter()
{
    // 원점 밖에 치우친 상자 body를 만들고 선속도와 각속도를 직접 준다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom geom = MakeBoxGeom({ 0.5, 1.0, 0.25 }, { 0.25, 0.5, 0.5 });
    const BodyHandle body = AddBody(world, geom, { 1, 30, 3 }, BodyMotion::Dynamic);
    const BodyReader reader(world, body);
    PhysicsWorldAccess::GetImpl(world)->World.GetBodyInterface().SetLinearVelocity(reader.Id, JPH::Vec3(1.0f, 2.0f, 3.0f));
    PhysicsWorldAccess::GetImpl(world)->World.GetBodyInterface().SetAngularVelocity(reader.Id, JPH::Vec3(0.5f, -1.0f, 2.0f));

    // scale (1, 2, 0.5)로 교체하고 같은 scale로 새 body도 만든다.
    const std::array<double, 3> scale = { 1, 2, 0.5 };
    Check(world.SetBodyShape(body, geom, scale, 1.0), "Off-center shape replace succeeds");
    BodyDesc desc;
    desc.Geometry = &geom;
    desc.Scale = scale;
    desc.Position = { 1, 30, 3 };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Prepare fresh off-center body");
    const BodyHandle fresh = world.CommitBody(std::move(prepared), false);
    const BodyReader freshReader(world, fresh);

    // 원점은 그대로고 질량 중심은 scale된 로컬 중심(0.5, 2, 0.125)으로 옮겨졌다.
    std::array<double, 3> p;
    std::array<double, 4> q;
    Check(world.GetBodyPose(body, p, q), "Pose readable");
    CheckNear(p[0], 1.0, 1e-6, "Origin x kept");
    CheckNear(p[1], 30.0, 1e-6, "Origin y kept");
    CheckNear(p[2], 3.0, 1e-6, "Origin z kept");
    const JPH::Vec3 offset = reader.CenterOffset();
    CheckNear(offset.GetX(), 0.5, 1e-3, "COM offset x");
    CheckNear(offset.GetY(), 2.0, 1e-3, "COM offset y");
    CheckNear(offset.GetZ(), 0.125, 1e-3, "COM offset z");

    // 3x3 역관성 행렬 전체가 새로 만든 body와 같다.
    float peak = 0.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            peak = std::max(peak, std::abs(freshReader.InverseInertia(r, c)));
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            CheckNear(reader.InverseInertia(r, c), freshReader.InverseInertia(r, c), peak * 1e-3, "Inverse inertia matches fresh body");

    // 선속도와 각속도가 그대로다.
    std::array<double, 3> v;
    std::array<double, 3> w;
    Check(world.GetBodyVelocity(body, v, w), "Velocity readable");
    CheckNear(v[0], 1.0, 1e-6, "Linear x kept");
    CheckNear(v[1], 2.0, 1e-6, "Linear y kept");
    CheckNear(v[2], 3.0, 1e-6, "Linear z kept");
    CheckNear(w[0], 0.5, 1e-6, "Angular x kept");
    CheckNear(w[1], -1.0, 1e-6, "Angular y kept");
    CheckNear(w[2], 2.0, 1e-6, "Angular z kept");
}

// 바닥 위에서 잠든 정육면체를 키우면 깨어나 바닥 밖으로 밀려나고 다시 잠드는지 검사한다.
static void TestShapeReplaceWakesAndResolves()
{
    // 바닥 위에서 잠들 때까지 기다린다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom floorGeom = MakeBoxGeom({ 0, -0.5, 0 }, { 10, 0.5, 10 });
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    AddBody(world, floorGeom, { 0, 0, 0 }, BodyMotion::Static);
    const BodyHandle cube = AddBody(world, cubeGeom, { 0, 2, 0 }, BodyMotion::Dynamic);
    StepUntilAsleep(world, cube);

    // 1.5배로 키우면 바닥에 파고들지만 즉시 깨어난다.
    Check(world.SetBodyShape(cube, cubeGeom, { 1.5, 1.5, 1.5 }, 1.0), "Enlarge sleeping body");
    Check(world.IsBodyAwake(cube), "Shape replace wakes body");

    // 접촉이 풀려 가장 낮은 점이 바닥 윗면(y = 0)에서 Jolt 관통 허용치(0.02 m) 안에 오고 다시 잠든다.
    const int steps = StepUntilAsleep(world, cube);
    std::array<double, 3> p;
    std::array<double, 4> q;
    Check(world.GetBodyPose(cube, p, q), "Pose readable");
    const double lowest = p[1] + BodyReader(world, cube).LocalBounds().mMin.GetY();
    std::printf("enlarge: asleep again after %d steps, lowest=%.4f m\n", steps, lowest);
    Check(lowest >= -0.025 && lowest <= 0.01, "Enlarged cube pushed out to within penetration slop of floor top");
}

// 회전된 오프센터 body를 순간이동해도 원점 pose와 두 속도가 유지되는지 검사한다.
static void TestTeleportOffCenter()
{
    // 로컬 중심이 원점 밖인 body에 선속도와 각속도를 준다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom geom = MakeBoxGeom({ 0.5, 1.0, 0.25 }, { 0.25, 0.5, 0.5 });
    const BodyHandle body = AddBody(world, geom, { 0, 30, 0 }, BodyMotion::Dynamic);
    const BodyReader reader(world, body);
    auto& bodies = PhysicsWorldAccess::GetImpl(world)->World.GetBodyInterface();
    bodies.SetLinearVelocity(reader.Id, JPH::Vec3(1, 2, 3));
    bodies.SetAngularVelocity(reader.Id, JPH::Vec3(0.5f, -1, 2));

    // 정규화되지 않은 큰 쿼터니언으로 Z축 90도 회전과 위치를 지정한다.
    Check(world.SetBodyTransform(body, { 4, 20, -2 }, { 0, 0, 1e30, 1e30 }), "Teleport rotated off-center body");
    std::array<double, 3> p, com, v, w;
    std::array<double, 4> q;
    Check(world.GetBodyPose(body, p, q) && world.GetBodyCenterOfMass(body, com), "Off-center pose readable");
    CheckNear(p[0], 4, 1e-5, "Teleport preserves requested origin x");
    CheckNear(p[1], 20, 1e-5, "Teleport preserves requested origin y");
    CheckNear(p[2], -2, 1e-5, "Teleport preserves requested origin z");
    CheckNear(q[2], std::sqrt(0.5), 1e-6, "Teleport normalizes rotation z");
    CheckNear(q[3], std::sqrt(0.5), 1e-6, "Teleport normalizes rotation w");

    // 질량 중심은 원점에 회전한 로컬 오프셋을 더한 위치다.
    CheckNear(com[0], 3, 1e-3, "Rotated COM x");
    CheckNear(com[1], 20.5, 1e-3, "Rotated COM y");
    CheckNear(com[2], -1.75, 1e-3, "Rotated COM z");
    Check(world.GetBodyVelocity(body, v, w), "Teleport velocities readable");
    CheckNear(v[0], 1, 0, "Teleport keeps linear x");
    CheckNear(v[1], 2, 0, "Teleport keeps linear y");
    CheckNear(v[2], 3, 0, "Teleport keeps linear z");
    CheckNear(w[0], 0.5, 0, "Teleport keeps angular x");
    CheckNear(w[1], -1, 0, "Teleport keeps angular y");
    CheckNear(w[2], 2, 0, "Teleport keeps angular z");
}

// world에서 제거된 body의 shape 교체는 재등록하지 않고 예약과 handle을 보존하는지 검사한다.
static void TestRemovedShapeReplace()
{
    // 잠든 body를 만든 뒤 world에서만 제거한다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom geom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    BodyDesc desc;
    desc.Geometry = &geom;
    desc.Position = { 0, 20, 0 };
    PreparedBody prepared;
    Check(world.PrepareBody(desc, prepared), "Prepare removed shape fixture");
    const BodyHandle body = world.CommitBody(std::move(prepared), false);
    Check(world.RemoveBodyFromWorld(body), "Remove shape fixture from world");
    const size_t reserved = world.GetReservedCount();
    const double originalWidth = BodyReader(world, body).LocalBounds().GetSize().GetX();

    // 비균일 scale 교체 후에도 broadphase에 없고 용량과 handle 수가 같다.
    Check(world.SetBodyShape(body, geom, { 2, 1, 0.5 }, 1), "Replace removed shape");
    const BodyReader reader(world, body);
    auto& bodies = PhysicsWorldAccess::GetImpl(world)->World.GetBodyInterface();
    Check(!bodies.IsAdded(reader.Id) && !world.IsBodyAwake(body), "Shape replace keeps body removed");
    Check(world.GetReservedCount() == reserved && world.GetBodyCount() == 1, "Shape replace preserves reservation and handle");
    CheckNear(reader.LocalBounds().GetSize().GetX(), originalWidth * 2, 1e-3, "Removed shape x scaled");
    CheckNear(reader.Mass(), MiniCollision::CalculateUnrealMass(Volume(geom)), reader.Mass() * 1e-4, "Removed shape mass updated");

    // 제거된 동안에는 step의 영향을 받지 않고 재등록 후 새 shape로 낙하한다.
    Check(world.Step(StepSeconds), "Step without removed body");
    std::array<double, 3> p;
    std::array<double, 4> q;
    Check(world.GetBodyPose(body, p, q), "Removed shape pose readable");
    CheckNear(p[1], 20, 1e-6, "Removed shape does not fall");
    Check(world.AddBodyToWorld(body, true), "Re-add replaced body");
    Check(world.Step(StepSeconds), "Step re-added body");
    Check(world.GetBodyPose(body, p, q) && p[1] < 20, "Re-added shape falls");
}

// 교체 실패 시 shape·질량·pose·속도가 전혀 바뀌지 않는지 검사한다.
static void TestShapeReplaceFailure()
{
    // 낙하 중인 정육면체를 준비한다.
    PhysicsWorld world;
    world.Initialize(8);
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    const BodyHandle cube = AddBody(world, cubeGeom, { 0, 20, 0 }, BodyMotion::Dynamic);
    for (int i = 0; i < 10; ++i)
    {
        Check(world.Step(StepSeconds), "Step succeeds");
    }

    // 실패 요청 후보를 만든다. 두 번째 요소가 cook에 실패하는 경우도 포함한다.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const AggregateGeom empty;
    AggregateGeom partial = cubeGeom;
    ConvexElem bad;
    bad.VertexData = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    partial.ConvexElems.push_back(bad);

    // 교체 전 상태를 기록한다.
    const BodyReader reader(world, cube);
    std::array<double, 3> pBefore;
    std::array<double, 4> qBefore;
    std::array<double, 3> vBefore;
    std::array<double, 3> w;
    Check(world.GetBodyPose(cube, pBefore, qBefore) && world.GetBodyVelocity(cube, vBefore, w), "State readable");
    const double massBefore = reader.Mass();
    const JPH::AABox boundsBefore = reader.LocalBounds();

    // 모든 실패 요청이 false와 사유 문자열을 돌려준다.
    std::string error;
    Check(!world.SetBodyShape(cube, empty, { 1, 1, 1 }, 1.0, &error) && !error.empty(), "Empty geometry rejected");
    error.clear();
    Check(!world.SetBodyShape(cube, cubeGeom, { 0, 1, 1 }, 1.0, &error) && !error.empty(), "Zero scale rejected");
    Check(!world.SetBodyShape(cube, cubeGeom, { 1, nan, 1 }, 1.0), "NaN scale rejected");
    Check(!world.SetBodyShape(cube, cubeGeom, { -1, 1, 1 }, 1.0), "Negative scale rejected");
    Check(!world.SetBodyShape(cube, cubeGeom, { 1, 1, 1 }, 0.0), "Zero mass scale rejected");
    error.clear();
    Check(!world.SetBodyShape(cube, partial, { 1, 1, 1 }, 1.0, &error) && !error.empty(), "Cook failure rejected");
    Check(!world.SetBodyShape(cube + 100, cubeGeom, { 1, 1, 1 }, 1.0), "Unknown handle rejected");

    // 실패 뒤 shape·질량·pose·속도가 그대로다.
    std::array<double, 3> pAfter;
    std::array<double, 4> qAfter;
    std::array<double, 3> vAfter;
    Check(world.GetBodyPose(cube, pAfter, qAfter) && world.GetBodyVelocity(cube, vAfter, w), "State readable after failures");
    CheckNear(reader.Mass(), massBefore, 0.0, "Mass unchanged");
    Check(reader.LocalBounds() == boundsBefore, "Bounds unchanged");
    for (int i = 0; i < 3; ++i)
    {
        CheckNear(pAfter[i], pBefore[i], 0.0, "Position unchanged");
        CheckNear(vAfter[i], vBefore[i], 0.0, "Velocity unchanged");
    }
    for (int i = 0; i < 4; ++i)
    {
        CheckNear(qAfter[i], qBefore[i], 0.0, "Rotation unchanged");
    }

    // 파괴한 handle과 초기화 전 world도 거절한다.
    Check(world.DestroyBody(cube), "Destroy body");
    Check(!world.SetBodyShape(cube, cubeGeom, { 1, 1, 1 }, 1.0), "Destroyed handle rejected");
    PhysicsWorld uninitialized;
    Check(!uninitialized.SetBodyShape(1, cubeGeom, { 1, 1, 1 }, 1.0), "Uninitialized world rejected");
}

// 모든 검사를 순서대로 실행하고 예외가 나면 실패로 끝낸다.
int main()
{
    try
    {
        // 순간이동과 shape 교체의 성공·실패 경로를 검사한다.
        TestTeleportWakesSleeper();
        TestVelocityPreserved();
        TestStaticAndRemoved();
        TestTransformInvalid();
        TestShapeReplace();
        TestShapeReplaceOffCenter();
        TestShapeReplaceWakesAndResolves();
        TestShapeReplaceFailure();
        TestTeleportOffCenter();
        TestRemovedShapeReplace();

        // 통과한 검사 수를 출력한다.
        std::printf("PhysicsTransformTests: %zu checks passed\n", g_Checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "PhysicsTransformTests failed: %s\n", error.what());
        return 1;
    }
}
