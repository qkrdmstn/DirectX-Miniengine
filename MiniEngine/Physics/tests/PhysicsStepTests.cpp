#include "CollisionTestUtils.h"
#include "PhysicsWorld.h"
#include "Collision/AggregateGeom.h"
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
        const double x = c.x + ((i & 1) ? half.x : -half.x);
        const double y = c.y + ((i & 2) ? half.y : -half.y);
        const double z = c.z + ((i & 4) ? half.z : -half.z);
        corners.push_back({ x, y, z });
    }

    // 18-DOP hull 하나가 만들어졌는지 확인하고 반환한다.
    AggregateGeom geom;
    Check(GenerateKDop18(corners, geom) && geom.ConvexElems.size() == 1, "Box geometry builds");
    return geom;
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

// 쿼터니언(xyzw)으로 벡터를 회전한다.
static std::array<double, 3> Rotate(const std::array<double, 4>& q, std::array<double, 3> v)
{
    // t = 2 (u x v)를 구한 뒤 v' = v + w t + u x t 공식을 쓴다.
    const double ux = q[0];
    const double uy = q[1];
    const double uz = q[2];
    const double w = q[3];

    // 외적 항 t를 구한다.
    const double tx = 2 * (uy * v[2] - uz * v[1]);
    const double ty = 2 * (uz * v[0] - ux * v[2]);
    const double tz = 2 * (ux * v[1] - uy * v[0]);

    // 회전된 벡터를 반환한다.
    return { v[0] + w * tx + (uy * tz - uz * ty), v[1] + w * ty + (uz * tx - ux * tz), v[2] + w * tz + (ux * ty - uy * tx) };
}

// 벡터 길이를 반환한다.
static double Length(const std::array<double, 3>& v)
{
    // 제곱합의 제곱근을 구한다.
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// 원점 중심의 1 m 정육면체 body의 가장 낮은 모서리 높이를 반환한다.
static double LowestPoint(PhysicsWorld& world, BodyHandle handle)
{
    // 현재 pose를 읽는다.
    std::array<double, 3> p;
    std::array<double, 4> q;
    Check(world.GetBodyPose(handle, p, q), "Pose readable");

    // 모서리 8개를 회전해 최소 y를 찾는다.
    double lowest = 1e30;
    for (int i = 0; i < 8; ++i)
    {
        const std::array<double, 3> corner = { (i & 1) ? 0.5 : -0.5, (i & 2) ? 0.5 : -0.5, (i & 4) ? 0.5 : -0.5 };
        lowest = std::min(lowest, p[1] + Rotate(q, corner)[1]);
    }
    return lowest;
}

// 바닥 위에 떨어뜨린 정육면체가 바닥 위에서 멈추고 잠드는지 검사한다.
static void TestCubeSettles()
{
    // 윗면이 y = 0인 바닥과 y = 2의 정육면체를 만든다.
    PhysicsWorld world;
    world.Initialize(16);
    const AggregateGeom floorGeom = MakeBoxGeom({ 0, -0.5, 0 }, { 10, 0.5, 10 });
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    const BodyHandle floorBody = AddBody(world, floorGeom, { 0, 0, 0 }, BodyMotion::Static);
    const BodyHandle cube = AddBody(world, cubeGeom, { 0, 2, 0 }, BodyMotion::Dynamic);
    Check(world.IsBodyAwake(cube), "Cube starts awake");

    // 3초 시점 상태와 처음 잠든 시각을 담을 변수를 준비한다.
    double lowest3 = 0;
    double speed3 = 0;
    double spin3 = 0;
    double sleepTime = -1;

    // 5초 동안 step하며 3초 시점의 상태와 처음 잠든 시각을 기록한다.
    for (int i = 1; i <= 300; ++i)
    {
        Check(world.Step(StepSeconds), "Step succeeds");

        // 3초 시점의 높이와 속도를 기록한다.
        if (i == 180)
        {
            std::array<double, 3> v;
            std::array<double, 3> w;
            Check(world.GetBodyVelocity(cube, v, w), "Velocity readable");
            lowest3 = LowestPoint(world, cube);
            speed3 = Length(v);
            spin3 = Length(w);
        }

        // 처음 잠든 시각을 기록한다.
        if (sleepTime < 0 && !world.IsBodyAwake(cube))
            sleepTime = i * double(StepSeconds);
    }

    // 3초 뒤 바닥 윗면(y = 0)에 오차 1 cm 이내로 얹혀 거의 정지해 있어야 한다.
    Check(lowest3 >= -0.01 && lowest3 <= 0.01, "Cube rests on floor top after 3 s");
    Check(speed3 < 0.05 && spin3 < 0.1, "Cube nearly at rest after 3 s");

    // 5초 시점에도 바닥 위에 있고 잠든 채로 유지되어야 한다.
    const double lowestFinal = LowestPoint(world, cube);
    Check(lowestFinal >= -0.01 && lowestFinal <= 0.01, "Cube rests on floor top at final step");
    Check(sleepTime > 0, "Cube asleep within 5 s");
    Check(!world.IsBodyAwake(cube), "Cube stays asleep at final step");
    std::printf("cube: sleep at %.2f s, lowest(3s)=%.4f m, |v|(3s)=%.4f m/s, |w|(3s)=%.4f rad/s, lowest(5s)=%.4f m\n",
        sleepTime, lowest3, speed3, spin3, lowestFinal);

    // 정적 body는 전혀 움직이지 않고 속도 0이며 깨어 있지 않다.
    std::array<double, 3> p;
    std::array<double, 3> v;
    std::array<double, 3> w;
    std::array<double, 4> q;
    Check(world.GetBodyPose(floorBody, p, q) && p[0] == 0 && p[1] == 0 && p[2] == 0, "Static body never moves");
    Check(world.GetBodyVelocity(floorBody, v, w) && Length(v) == 0 && Length(w) == 0, "Static velocity is zero");
    Check(!world.IsBodyAwake(floorBody), "Static body is not awake");
}

// 원점과 질량 중심이 다른 body가 같이 떨어지고 pose는 원점을 돌려주는지 검사한다.
static void TestOriginVersusCenterOfMass()
{
    // 로컬 (0.5, 0.5, 0.5)가 중심인 정육면체를 y = 3에 놓는다.
    PhysicsWorld world;
    world.Initialize(16);
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0.5, 0.5, 0.5 }, { 0.5, 0.5, 0.5 });
    const BodyHandle body = AddBody(world, cubeGeom, { 0, 3, 0 }, BodyMotion::Dynamic);

    // 시작 시 원점은 요청 위치, 질량 중심은 로컬 중심만큼 위에 있다.
    std::array<double, 3> p;
    std::array<double, 3> c;
    std::array<double, 4> q;
    Check(world.GetBodyPose(body, p, q) && world.GetBodyCenterOfMass(body, c), "Pose and COM readable");
    CheckNear(p[1], 3.0, 1e-5, "Origin at request");
    for (int i = 0; i < 3; ++i)
        CheckNear(c[i] - p[i], 0.5, 1e-3, "COM offset from origin");

    // 0.3초 자유낙하 동안 원점과 질량 중심이 같은 거리만큼 내려가고 오프셋이 유지된다.
    for (int i = 0; i < 18; ++i)
        Check(world.Step(StepSeconds), "Step succeeds");

    // 낙하 뒤 pose와 질량 중심을 다시 읽는다.
    std::array<double, 3> p2;
    std::array<double, 3> c2;
    Check(world.GetBodyPose(body, p2, q) && world.GetBodyCenterOfMass(body, c2), "Pose and COM readable after fall");
    CheckNear(p[1] - p2[1], c[1] - c2[1], 1e-4, "Origin and COM fall equally");
    CheckNear(p[1] - p2[1], 0.5 * 9.8 * 0.3 * 0.3, 0.05, "Fall distance matches gravity");
    for (int i = 0; i < 3; ++i)
        CheckNear(c2[i] - p2[i], 0.5, 1e-3, "COM offset kept in free fall");
    std::printf("offset body: fell %.4f m in 0.3 s (expected %.4f)\n", p[1] - p2[1], 0.5 * 9.8 * 0.09);
}

// 조회 전용 Box가 step의 영향을 받지 않고 QueryAabb에 계속 나오는지 검사한다.
static void TestQueryBoxUnaffected()
{
    // 낙하하는 body 옆에 조회 Box를 둔다.
    PhysicsWorld world;
    world.Initialize(16);
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    AddBody(world, cubeGeom, { 0, 2, 0 }, BodyMotion::Dynamic);
    const QueryBounds bounds{ { -1, 1, -1 }, { 1, 3, 1 } };
    const ColliderHandle query = world.CreateQueryBox(bounds);
    Check(query != InvalidCollider, "Query box created");

    // 1초 step 뒤에도 같은 handle만 조회되고 개수가 변하지 않는다.
    for (int i = 0; i < 60; ++i)
        Check(world.Step(StepSeconds), "Step succeeds");
    const auto hits = world.QueryAabb(bounds);
    Check(hits.size() == 1 && hits[0] == query, "Query box still returned");
    Check(world.GetColliderCount() == 1, "Query count unchanged");
}

// 초기화 전·종료 후·잘못된 dt·제거된 handle의 실패 처리를 검사한다.
static void TestInvalidUse()
{
    // 초기화되지 않은 world는 모든 호출이 false다.
    PhysicsWorld world;
    std::array<double, 3> p;
    std::array<double, 3> v;
    std::array<double, 4> q;
    Check(!world.Step(StepSeconds), "Step before Initialize fails");
    Check(!world.GetBodyPose(1, p, q) && !world.IsBodyAwake(1) && !world.GetBodyVelocity(1, p, v), "Reads before Initialize fail");

    // 잘못된 dt는 거절하되 이후 정상 step은 성공한다.
    world.Initialize(8);
    const AggregateGeom cubeGeom = MakeBoxGeom({ 0, 0, 0 }, { 0.5, 0.5, 0.5 });
    const BodyHandle body = AddBody(world, cubeGeom, { 0, 2, 0 }, BodyMotion::Dynamic);
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Check(!world.Step(0.0f) && !world.Step(-StepSeconds) && !world.Step(nan) && !world.Step(inf), "Invalid dt rejected");
    Check(world.Step(StepSeconds), "Valid step after rejection");
    std::array<double, 3> c;
    Check(world.GetBodyCenterOfMass(body, c) && !world.GetBodyCenterOfMass(body + 100, c), "Unknown handle rejected");

    // 파괴한 body의 읽기는 모두 실패한다.
    Check(world.DestroyBody(body), "Destroy body");
    Check(!world.GetBodyPose(body, p, q) && !world.GetBodyCenterOfMass(body, p) && !world.IsBodyAwake(body) && !world.GetBodyVelocity(body, p, v), "Removed body reads fail");

    // 종료한 world의 step은 실패한다.
    world.Shutdown();
    Check(!world.Step(StepSeconds), "Step after Shutdown fails");
}

// 모든 검사를 순서대로 실행하고 예외가 나면 실패로 끝낸다.
int main()
{
    try
    {
        // 낙하·정지, 원점 대 질량 중심, 조회 격리, 잘못된 사용을 검사한다.
        TestCubeSettles();
        TestOriginVersusCenterOfMass();
        TestQueryBoxUnaffected();
        TestInvalidUse();

        // 통과한 검사 수를 출력한다.
        std::printf("PhysicsStepTests: %zu checks passed\n", g_Checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "PhysicsStepTests failed: %s\n", error.what());
        return 1;
    }
}
