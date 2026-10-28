// Run from a VS Developer PowerShell at the repository root (outputs outside the repository):
// cl /nologo /std:c++17 /EHsc /W4 /I MiniEngine/Core /I MiniEngine/Model
//    /Fo:<external-directory>/SceneMathTests.obj /Fe:<external-directory>/SceneMathTests.exe
//    MiniEngine/ModelViewer/Scene/tests/SceneMathTests.cpp

#include "../GizmoMath.h"
#include "../TransformHistory.h"
#include "../PickHit.h"
#include "../EditTarget.h"
#include "ModelPose.h"
#include "TransformBounds.h"
#include <cstdio>
#include <stdexcept>
#include <limits>

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

    // 합성 월드 행렬의 TRS 보존과 전단·반사·무효값 거절을 검사한다.
    void WorldTransformTests()
    {
        // 양수 비균일 TRS와 균일 owner 합성은 점과 축을 보존한다.
        const Quaternion rotation(Vector3(kYUnitVector), 0.7f);
        const SceneTransform local(rotation, Vector3(2, 3, 4), Vector3(1, -2, 3));
        const SceneTransform owner(Quaternion(Vector3(kZUnitVector), 0.3f), Vector3(2, 2, 2), Vector3(-5, 2, 4));
        const AffineTransform world = owner.ToAffine() * local.ToAffine();
        SceneTransform result;
        Check(SceneTransform::TryFromAffine(world, result), "Uniform owner can detach without shear");
        for (const Vector3 point : { Vector3(kZero), Vector3(1, 2, 3), Vector3(-2, 1, 4) })
            Near(result.ToAffine() * point, world * point, "Detached TRS preserves world points");
        Near(result.GetScale(), Vector3(4, 6, 8), "World scale composed");

        // 비균일 owner와 회전한 component의 합성 전단은 출력까지 보존하며 거절한다.
        const SceneTransform stretched(Quaternion(kIdentity), Vector3(2, 1, 3), Vector3(kZero));
        const Matrix4 before(result.ToAffine());
        Check(!SceneTransform::TryFromAffine(stretched.ToAffine() * local.ToAffine(), result), "Shear rejected");
        Near(result.GetTranslation(), Vector3(before.GetW()), "Rejected conversion preserves output");
        Check(!SceneTransform::TryFromAffine(AffineTransform(Matrix3::MakeScale(Vector3(-1, 1, 1))), result), "Reflection rejected");
        Check(!SceneTransform::TryFromAffine(AffineTransform(Matrix3::MakeScale(Vector3(0, 1, 1))), result), "Zero axis rejected");

        // 축과 위치의 NaN/Inf는 TRS로 게시하지 않는다.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        Check(!SceneTransform::TryFromAffine(AffineTransform(Matrix3::MakeScale(Vector3(nan, 1, 1))), result), "NaN axis rejected");
        Check(!SceneTransform::TryFromAffine(AffineTransform(Matrix3(kIdentity), Vector3(inf, 0, 0)), result), "Infinite position rejected");
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

    // 모델 공간 포즈와 비균일 locator 단일 적용을 검사한다.
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
        const Matrix4 locator(SceneTransform(Quaternion(Vector3(kYUnitVector), XM_PIDIV2),
            Vector3(2, 3, 5), Vector3(4, 5, 6)).ToAffine());
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

    // 평면 크기 축 제한과 공통 월드 거리 선택을 검사한다.
    void PlaneEditingTests()
    {
        const Vector3 initial(2, 3, 4);
        Near(GizmoMath::ScaleComponents(initial, -1, 100, 5), Vector3(2 * std::exp(1.0f), 3, 4 * std::exp(1.0f)),
            "plane center changes XZ and preserves Y");
        Near(GizmoMath::ScaleComponents(initial, 1, 100, 5), initial, "plane Y scaling disabled");
        const PickHit model{1, 5}, plane{2, 3};
        Check(NearestPickHit(model, plane).Id == 2, "closer plane wins");
        Check(NearestPickHit(plane, model).Id == 2, "farther model loses");
        Check(NearestPickHit(model, {2, -1}).Id == 1, "behind candidate ignored");
        Check(NearestPickHit(model, {0, 1}).Id == 1, "invalid object ignored");
        Check(NearestPickHit(model, {2, 5}).Id == 1, "equal distance stable");
    }

    // pick 결과가 R8 규칙으로 Owner 또는 Mesh 대상이 되는지와 대상 동등 비교를 검사한다.
    void EditTargetTests()
    {
        // 월드 독립 컴포넌트는 Mesh 대상이다.
        const EditTarget mesh = EditTarget::FromPick({ 7, 1.0f, 3 }, true);
        Check(mesh.IsMesh() && mesh.Object == 7 && mesh.Mesh == 3, "world independent pick is a Mesh target");

        // Attached 컴포넌트는 컴포넌트 ID를 버리고 Owner 대상이 된다.
        const EditTarget owner = EditTarget::FromPick({ 7, 1.0f, 3 }, false);
        Check(!owner.IsMesh() && owner.Kind == EditTargetKind::Owner && owner.Object == 7 && owner.Mesh == InvalidComponentId,
            "attached pick is an Owner target");

        // 컴포넌트가 없는 객체(절단 평면)와 빈 pick을 처리한다.
        Check(EditTarget::FromPick({ 9, 1.0f, InvalidComponentId }, true) == EditTarget::MakeOwner(9), "model-less pick is Owner");
        Check(EditTarget::FromPick(PickHit(), true).IsNone(), "empty pick is no target");
        Check(EditTarget().IsNone() && !EditTarget().IsMesh(), "default target is none");

        // 같은 owner의 다른 조각과 owner 전체는 서로 다른 대상이다.
        Check(EditTarget::MakeMesh(7, 3) == mesh, "same mesh target compares equal");
        Check(EditTarget::MakeMesh(7, 4) != mesh, "different piece is a different target");
        Check(EditTarget::MakeOwner(7) != mesh, "owner differs from its piece");
    }

    // 런타임 TRS·크기·경계·법선 도우미의 비균일 변환 계약을 검사한다.
    void NonuniformTests()
    {
        const Quaternion rotation(Vector3(kZUnitVector), 0.7f);
        SceneTransform start(rotation, Vector3(2, 3, 5), Vector3(7, -4, 2));
        Check(start.Normalize(), "valid anisotropic TRS accepted");
        const AffineTransform affine = start.ToAffine();
        Near(affine * Vector3(1, 2, 3), rotation * Vector3(2, 6, 15) + Vector3(7, -4, 2),
            "TRS applies independent scale before rotation and translation");
        const Matrix3 normal = InverseTranspose(affine.GetBasis());
        const Vector3 tangent = affine.GetBasis() * Vector3(1, 1, 0);
        const Vector3 transformedNormal = normal * Vector3(1, -1, 0);
        Check(std::abs(float(Dot(tangent, transformedNormal))) < 0.0001f,
            "inverse transpose preserves tangent-normal orthogonality");
        const BoundingSphere sphere(Vector3(1, -2, 3), 4);
        const BoundingSphere world = TransformBoundingSphere(affine, sphere);
        Near(world.GetCenter(), affine * sphere.GetCenter(), "off-origin sphere center uses complete TRS");
        Check(std::abs(float(world.GetRadius()) - 20.0f) < 0.0001f, "TRS radius uses maximum scale");
        const AffineTransform composed = affine * AffineTransform(Quaternion(Vector3(kYUnitVector), 0.8f));
        const BoundingSphere conservative = TransformBoundingSphere(composed, sphere);
        for (int i = 0; i < 180; ++i)
        {
            const float angle = float(i) * XM_2PI / 180.0f;
            const Vector3 direction = Normalize(Vector3(std::cos(angle), std::sin(angle), std::cos(3 * angle)));
            Check(float(Length(composed * (sphere.GetCenter() + direction * sphere.GetRadius()) -
                conservative.GetCenter())) <= float(conservative.GetRadius()) + 0.0001f,
                "composed shear boundary remains inside culling sphere");
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            const Vector3 scaled = GizmoMath::ScaleComponents(start.GetScale(), axis, 100);
            XMFLOAT3 original, changed;
            XMStoreFloat3(&original, start.GetScale());
            XMStoreFloat3(&changed, scaled);
            Check(std::abs(changed.x - original.x * (axis == 0 ? std::exp(1.0f) : 1.0f)) < 0.0001f &&
                std::abs(changed.y - original.y * (axis == 1 ? std::exp(1.0f) : 1.0f)) < 0.0001f &&
                std::abs(changed.z - original.z * (axis == 2 ? std::exp(1.0f) : 1.0f)) < 0.0001f,
                "selected axis changes exclusively");
        }
        Near(GizmoMath::ScaleComponents(start.GetScale(), -1, 100), start.GetScale() * std::exp(1.0f),
            "center scales every component by same factor");
        SceneTransform extreme = start;
        extreme.SetScale(GizmoMath::ScaleComponents(Vector3(1.0e8f, 0.0001f, 5), -1, 100000));
        Check(extreme.Normalize(), "maximum drag remains valid TRS");
        Check(float(extreme.GetScale().GetX()) == 1.0e8f, "upper scale bound enforced");
        const Vector3 minimum = GizmoMath::ScaleComponents(Vector3(0.0001f, 0.0001f, 0.0001f), -1, -100000);
        Near(minimum, Vector3(0.0001f, 0.0001f, 0.0001f), "minimum positive clamp enforced");
        TransformHistory history;
        Check(history.Record(42, start, extreme), "anisotropic history records extreme drag");
        // 실제 히스토리를 통해 세 축과 회전·위치를 복구한다.
        // 기록된 TRS를 검사하거나 복구한다.
        Check(history.Undo([&](uint64_t id, const SceneTransform& before)
        {
            extreme = before;
            return id == 42;
        }), "anisotropic history undoes extreme drag");
        Near(extreme.GetScale(), start.GetScale(), "undo restores all scale components");
        Near(extreme.GetTranslation(), start.GetTranslation(), "undo restores translation");
        for (const Vector3 invalid : { Vector3(0, 1, 1), Vector3(-1, 1, 1), Vector3(1.0e9f, 1, 1),
            Vector3(INFINITY, 1, 1), Vector3(NAN, 1, 1) })
        {
            extreme.SetScale(invalid);
            Check(!extreme.Normalize(), "out-of-policy scale rejected");
        }
    }

    // 카메라 위치·방향·투영이 바뀌어도 월드/로컬 기저는 유지되는지 검사한다.
    void BasisTests()
    {
        const Quaternion rotation(Vector3(kZUnitVector), XM_PIDIV2);
        const Matrix4 projection(XMMatrixPerspectiveFovRH(XM_PIDIV4, 1.5f, 0.1f, 100));
        for (const bool local : { false, true })
        {
            Vector3 reference[3];
            GizmoMath::Basis(rotation, local, reference);
            Near(reference[0], local ? Vector3(kYUnitVector) : Vector3(kXUnitVector), "explicit coordinate mode");
            GizmoMath::Point previous{};
            int count = 0;
            for (const Vector3 eye : { Vector3(5, 3, 8), Vector3(-6, 5, 7), Vector3(3, -4, 9) })
            {
                const Matrix4 view(XMMatrixLookAtRH(eye, Vector3(kZero), Vector3(kYUnitVector)));
                Vector3 axes[3];
                GizmoMath::Basis(rotation, local, axes);
                for (int i = 0; i < 3; ++i)
                    Near(axes[i], reference[i], "world/local axis independent of camera position and orientation");
                GizmoMath::Point projected;
                Check(GizmoMath::Project(axes[0], projection * view, 900, 600, projected), "ring point projects");
                if (count++)
                    Check(std::abs(projected.X - previous.X) + std::abs(projected.Y - previous.Y) > 1,
                        "camera changes projection while 3D axes stay fixed");
                previous = projected;
            }
        }
    }
    // TRS 히스토리의 복구와 삭제 대상 및 용량 정책을 검사한다.
    void HistoryTests()
    {
        TransformHistory history;
        const SceneTransform start(kIdentity);
        SceneTransform moved = start;
        moved.SetTranslation(Vector3(3, 2, 1));
        Check(!history.Record(1, start, start), "click without movement creates no command");
        SceneTransform signedQuaternion = start;
        signedQuaternion.SetRotation(-start.GetRotation());
        Check(!history.Record(1, start, signedQuaternion), "equivalent quaternion sign creates no command");
        int calls = 0;
        // 기록된 TRS를 검사하거나 복구한다.
        Check(!history.Undo([&](uint64_t, const SceneTransform&) { ++calls; return true; }) && calls == 0,
            "empty/canceled transaction cannot undo anything");
        Check(history.Record(1, start, moved), "completed drag records one command");
        SceneTransform current = moved;
        // 기록된 TRS를 검사하거나 복구한다.
        Check(history.Undo([&](uint64_t id, const SceneTransform& before)
        {
            Check(id == 1, "undo targets stable object ID");
            current = before;
            return true;
        }), "completed drag can undo");
        Near(current.GetTranslation(), Vector3(kZero), "one undo restores original transform");
        // 기록된 TRS를 검사하거나 복구한다.
        Check(!history.Undo([](uint64_t, const SceneTransform&) { return true; }), "one drag produces only one command");
        history.Record(1, start, moved);
        history.Record(2, start, moved);
        calls = 0;
        // 기록된 TRS를 검사하거나 복구한다.
        Check(history.Undo([&](uint64_t id, const SceneTransform&)
        {
            ++calls;
            return id == 1; // Object 2 was deleted; object 1 is still live.
        }) && calls == 2, "deleted target skipped in favor of next live command");
        for (uint64_t id = 1; id <= 129; ++id)
            history.Record(id, start, moved);
        calls = 0;
        // 기록된 TRS를 검사하거나 복구한다.
        while (history.Undo([&](uint64_t id, const SceneTransform&)
        {
            Check(id >= 2, "history drops oldest command beyond capacity");
            ++calls;
            return true;
        })) {}
        Check(calls == 128, "history retains 128 commands");
    }
}

// 공통 런타임 도우미의 CPU 회귀 검사를 실행한다.
int main()
{
    try
    {
        ProjectionTests();
        DragMathTests();
        PoseTests();
        HistoryTests();
        NonuniformTests();
        BasisTests();
        PlaneEditingTests();
        EditTargetTests();
        WorldTransformTests();
        std::printf("SceneMathTests: %d checks passed\n", Checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "SceneMathTests failed: %s\n", error.what());
        return 1;
    }
}
