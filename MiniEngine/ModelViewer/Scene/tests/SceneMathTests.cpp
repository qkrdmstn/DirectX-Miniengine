// Run from a VS Developer PowerShell at the repository root (outputs outside the repository):
// cl /nologo /std:c++17 /EHsc /W4 /I MiniEngine/Core /I MiniEngine/Model
//    /Fo:<external-directory>/SceneMathTests.obj /Fe:<external-directory>/SceneMathTests.exe
//    MiniEngine/ModelViewer/Scene/tests/SceneMathTests.cpp

#include "../GizmoMath.h"
#include "../TransformHistory.h"
#include "ModelPose.h"
#include <cstdio>
#include <stdexcept>

using namespace Math;
using namespace Viewer;
using namespace DirectX;

namespace
{
    int Checks = 0;
    void Check(bool condition, const char* description)
    {
        ++Checks;
        if (!condition)
            throw std::runtime_error(description);
    }
    void Near(Vector3 actual, Vector3 expected, const char* description)
    {
        Check(float(Length(actual - expected)) < 0.0002f, description);
    }
    Vector3 Point(const Matrix4& matrix, Vector3 position)
    {
        return MakeHomogeneous(matrix * position);
    }

    void ProjectionTests()
    {
        const Vector3 eye(8.0f, 6.0f, 12.0f), target(kZero);
        const Matrix4 view(XMMatrixLookAtRH(eye, target, Vector3(kYUnitVector)));
        const Vector3 forward = Normalize(target - eye), right = Normalize(Cross(forward, Vector3(kYUnitVector)));
        const Vector3 up = Cross(right, forward);
        for (const float aspect : { 16.0f / 9.0f, 1.0f })
        {
            for (int convention = 0; convention < 3; ++convention)
            {
                Matrix4 projection = Matrix4(XMMatrixPerspectiveFovRH(XM_PIDIV4, aspect,
                    convention == 0 ? 0.1f : 1000.0f, convention == 0 ? 1000.0f : 0.1f));
                if (convention == 2)
                {
                    const float y = 1.0f / std::tan(XM_PIDIV4 * 0.5f);
                    projection = Matrix4(Vector4(y / aspect, 0, 0, 0), Vector4(0, y, 0, 0),
                        Vector4(0, 0, 0, -1), Vector4(0, 0, 0.1f, 0));
                }
                const Matrix4 vp = projection * view;
                const float width = 1080.0f * aspect, height = 1080.0f;
                for (const float side : { -1.0f, 0.0f, 1.0f })
                {
                    const Vector3 world = eye + forward * 10.0f + right * (side * 1.5f) + up * 0.75f;
                    GizmoMath::Point screen;
                    Check(GizmoMath::Project(world, vp, width, height, screen), "front point projects");
                    const Vector3 direction = GizmoMath::RayDirection(vp, eye, screen.X / width, screen.Y / height);
                    Check(float(Dot(direction, Normalize(world - eye))) > 0.99999f,
                        "project/ray roundtrip: standard, reverse and infinite reverse Z");
                }
                GizmoMath::Point screen;
                Check(!GizmoMath::Project(eye - forward, vp, width, height, screen), "behind-camera point rejected");
            }
        }
    }

    void DragMathTests()
    {
        const Vector3 eye(0, 2, 5), pivot(kZero), axis(kXUnitVector);
        const Vector3 first = Normalize(pivot - eye);
        Vector3 normal, start, end;
        Check(GizmoMath::AxisPlaneNormal(axis, first, normal), "side-view axis has drag plane");
        Check(GizmoMath::IntersectPlane(eye, first, pivot, normal, start), "axis drag starts");
        Check(GizmoMath::IntersectPlane(eye, Normalize(Vector3(3, 0, 0) - eye), pivot, normal, end), "axis drag continues");
        Near(axis * Dot(end - start, axis), Vector3(3, 0, 0), "axis drag moves exactly three world units");
        Check(!GizmoMath::AxisPlaneNormal(axis, axis, normal), "end-on axis has no stable drag plane");
        Check(!GizmoMath::IntersectPlane(Vector3(0, 0, 5), axis, pivot, Vector3(kZUnitVector), end), "parallel ray rejected");
        Check(!GizmoMath::IntersectPlane(Vector3(0, 0, 5), Vector3(kZUnitVector), pivot,
            Vector3(kZUnitVector), end), "intersection behind ray rejected");
        const Vector3 planeEye(0, 0, 10);
        Check(GizmoMath::IntersectPlane(planeEye, Normalize(Vector3(1, 2, 0) - planeEye), pivot,
            Vector3(kZUnitVector), start), "plane drag starts");
        Check(GizmoMath::IntersectPlane(planeEye, Normalize(Vector3(-2, 4, 0) - planeEye), pivot,
            Vector3(kZUnitVector), end), "plane drag continues");
        Near(end - start, Vector3(-3, 2, 0), "XY drag preserves both in-plane components");
        const Quaternion localRotation(Vector3(kZUnitVector), XM_PIDIV2);
        Near(localRotation * axis, Vector3(kYUnitVector), "rotated object's local X points along world Y");
        const float a = 179.0f * XM_PI / 180.0f, b = -179.0f * XM_PI / 180.0f;
        const float step = GizmoMath::SignedAngle(Vector3(std::cos(a), std::sin(a), 0),
            Vector3(std::cos(b), std::sin(b), 0), Vector3(kZUnitVector));
        Check(std::abs(step - 2.0f * XM_PI / 180.0f) < 0.00001f, "rotation crosses pi without a full-turn jump");
        Check(std::abs(GizmoMath::SignedAngle(axis, Vector3(kYUnitVector), Vector3(kZUnitVector)) - XM_PIDIV2)
            < 0.00001f, "positive quarter turn uses right-hand rule");
        Check(std::abs(GizmoMath::SignedAngle(Vector3(kYUnitVector), axis, Vector3(kZUnitVector)) + XM_PIDIV2)
            < 0.00001f, "negative quarter turn preserves sign");
        Near((Quaternion(Vector3(kYUnitVector), XM_PIDIV2) * localRotation) * axis,
            Quaternion(Vector3(kYUnitVector), XM_PIDIV2) * (localRotation * axis), "world rotation composes outside local pose");
        Check(std::abs(GizmoMath::UniformScale(2.0f, 100.0f) - 2.0f * std::exp(1.0f)) < 0.00001f,
            "100 upward pixels multiply scale by e");
        Check(GizmoMath::UniformScale(1.0f, -100000.0f) > 0.0f, "extreme downward drag stays positive");
        Check(std::isfinite(GizmoMath::UniformScale(1.0f, 100000.0f)), "extreme upward drag stays finite");
        GizmoMath::Point head[3];
        Check(GizmoMath::ArrowHead({ 10, 20 }, { 110, 20 }, head), "translation arrow has a head");
        Check(std::abs(head[0].X - 110.0f) < 0.001f && std::abs(head[1].X - 98.0f) < 0.001f &&
            std::abs(head[1].Y - head[2].Y - 10.0f) < 0.001f, "arrow head is twelve by ten screen pixels");
        Check(GizmoMath::SegmentDistance(head[1], { 25, 20 }, { 110, 20 }) < 9.0f &&
            GizmoMath::SegmentDistance(head[2], { 25, 20 }, { 110, 20 }) < 9.0f,
            "both arrow wings remain inside the existing axis hit tolerance");
        Check(GizmoMath::ArrowHead({ 0, 0 }, { 0, -8 }, head) && head[1].Y < 0.0f && head[1].Y > -8.0f,
            "foreshortened arrow stays inside its projected shaft");
        Check(!GizmoMath::ArrowHead({ 1, 1 }, { 1, 1 }, head), "degenerate arrow rejected without division");
    }

    GraphNode Node(uint32_t index, Matrix4 transform, bool children = false, bool sibling = false, bool skeleton = false)
    {
        GraphNode node{};
        node.xform = transform;
        node.rotation = Quaternion(kIdentity);
        node.scale = XMFLOAT3(1, 1, 1);
        node.matrixIdx = index;
        node.hasChildren = children;
        node.hasSibling = sibling;
        node.skeletonRoot = skeleton;
        return node;
    }

    void PoseTests()
    {
        const Matrix4 identity(kIdentity);
        GraphNode branch[] = {
            Node(0, Matrix4(AffineTransform(Vector3(10, 0, 0))), true),
            Node(1, Matrix4(AffineTransform(Vector3(0, 20, 0))), true, true),
            Node(2, Matrix4(AffineTransform(Vector3(0, 0, 30)))),
            Node(3, Matrix4(AffineTransform(Vector3(-5, 0, 0)))) };
        Matrix4 poses[4];
        Check(EvaluateModelPose(branch, 4, poses), "depth-first graph evaluates");
        Near(Point(poses[2], Vector3(kZero)), Vector3(10, 20, 30), "nested node inherits its parent");
        Near(Point(poses[3], Vector3(kZero)), Vector3(5, 0, 0), "sibling restores prior parent");
        branch[3].matrixIdx = 99;
        Check(!EvaluateModelPose(branch, 4, poses), "invalid matrix index rejected");
        GraphNode truncated[] = { Node(0, identity, true) };
        Check(!EvaluateModelPose(truncated, 1, poses), "truncated child graph rejected");
        GraphNode deep[34];
        Matrix4 deepPoses[34];
        for (uint32_t i = 0; i < 34; ++i)
            deep[i] = Node(i, identity, true, true);
        Check(!EvaluateModelPose(deep, 34, deepPoses), "parent stack overflow rejected");

        // Cesium-style explicit skeletonRoot and Xbot-style absent skin.skeleton.
        const Matrix4 locator(AffineTransform(UniformTransform(Quaternion(Vector3(kYUnitVector), XM_PIDIV2),
            2.0f, Vector3(4, 5, 6))));
        for (const bool explicitRoot : { false, true })
        {
            const Matrix4 parent = explicitRoot ? Matrix4(AffineTransform(Quaternion(Vector3(kXUnitVector), XM_PIDIV2))) :
                Matrix4(AffineTransform(Matrix3::MakeScale(0.01f)));
            const Matrix4 jointLocal(AffineTransform(Vector3(0, explicitRoot ? 1.0f : 100.0f, 0)));
            GraphNode graph[] = { Node(0, parent, true), Node(1, identity, false, true),
                Node(2, jointLocal, false, false, explicitRoot) };
            Check(EvaluateModelPose(graph, 3, poses), "skinned graph evaluates with optional skeleton root");
            const Matrix4 expectedJoint = explicitRoot ? jointLocal : parent * jointLocal;
            Near(Point(poses[2], Vector3(kZero)), Point(expectedJoint, Vector3(kZero)), "skeleton reset rule preserved");
            const Matrix4 inverseBind = Invert(expectedJoint);
            const Matrix4 palette = poses[2] * inverseBind;
            const Vector3 vertex(100, 20, 30);
            const Vector3 atIdentity = Point(poses[1] * palette, vertex);
            Near(atIdentity, Point(parent, vertex), "identity locator keeps original bind pose");
            const Vector3 rendered = Point((locator * poses[1]) * palette, vertex);
            Near(rendered, Point(locator, atIdentity), "instance locator applied exactly once after skinning");
            Check(float(Length(rendered - Point(locator, Point(locator, atIdentity)))) > 1.0f,
                "fixture detects a double locator transform");
        }
    }

    void HistoryTests()
    {
        TransformHistory history;
        const UniformTransform start(kIdentity);
        UniformTransform moved = start;
        moved.SetTranslation(Vector3(3, 2, 1));
        Check(!history.Record(1, start, start), "click without movement creates no command");
        UniformTransform signedQuaternion = start;
        signedQuaternion.SetRotation(-start.GetRotation());
        Check(!history.Record(1, start, signedQuaternion), "equivalent quaternion sign creates no command");
        int calls = 0;
        Check(!history.Undo([&](uint64_t, const UniformTransform&) { ++calls; return true; }) && calls == 0,
            "empty/canceled transaction cannot undo anything");
        Check(history.Record(1, start, moved), "completed drag records one command");
        UniformTransform current = moved;
        Check(history.Undo([&](uint64_t id, const UniformTransform& before)
        {
            Check(id == 1, "undo targets stable object ID");
            current = before;
            return true;
        }), "completed drag can undo");
        Near(current.GetTranslation(), Vector3(kZero), "one undo restores original transform");
        Check(!history.Undo([](uint64_t, const UniformTransform&) { return true; }), "one drag produces only one command");
        history.Record(1, start, moved);
        history.Record(2, start, moved);
        calls = 0;
        Check(history.Undo([&](uint64_t id, const UniformTransform&)
        {
            ++calls;
            return id == 1; // Object 2 was deleted; object 1 is still live.
        }) && calls == 2, "deleted target skipped in favor of next live command");
        for (uint64_t id = 1; id <= 129; ++id)
            history.Record(id, start, moved);
        calls = 0;
        while (history.Undo([&](uint64_t id, const UniformTransform&)
        {
            Check(id >= 2, "history drops oldest command beyond capacity");
            ++calls;
            return true;
        })) {}
        Check(calls == 128, "history retains 128 commands");
    }
}

int main()
{
    try
    {
        ProjectionTests();
        DragMathTests();
        PoseTests();
        HistoryTests();
        std::printf("SceneMathTests: %d checks passed\n", Checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "SceneMathTests failed: %s\n", error.what());
        return 1;
    }
}
