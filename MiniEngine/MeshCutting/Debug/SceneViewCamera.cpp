#include "../pch.h"
#include "SceneViewCamera.h"
#include "GameInput.h"
#include "Display.h"

using namespace Math;

namespace MeshCutting
{
    namespace
    {
        constexpr float kLookSensitivity = 1.0f;        // GameInput은 마우스 한 칸을 0.0018로 바꿔 준다
        constexpr float kOrbitSensitivity = 1.0f;
        constexpr float kMoveSpeed = 1.5f;              // 초당 대상 반지름의 배수
        constexpr float kFastFactor = 3.0f;             // Shift
        constexpr float kZoomPerWheel = 0.85f;          // 휠 한 칸에 피벗까지의 거리가 곱해지는 값
        constexpr float kZoomDragSpeed = 2.0f;
        constexpr float kMinSpeedScale = 0.05f, kMaxSpeedScale = 40.0f;
        constexpr float kMouseCount = 0.0018f;
    }

    SceneViewCamera::SceneViewCamera(Camera& camera, const BoundingSphere& focus, Vector3 worldUp)
        : CameraController(camera), m_WorldUp(Normalize(worldUp))
    {
        m_FocusRadius = XMMax(float(focus.GetRadius()), 0.001f);
        m_MinDistance = XMMax(m_FocusRadius * 0.02f, 0.001f);
        m_MaxDistance = m_FocusRadius * 200.0f;

        UpdateAspectRatio();
        m_TargetCamera.SetZRange(XMMax(m_FocusRadius * 0.001f, 0.0001f),
            XMMax(m_TargetCamera.GetFarClip(), m_FocusRadius * 201.0f));
        Frame(Vector3(focus.GetCenter()), m_FocusRadius);
    }

    Matrix3 SceneViewCamera::Orientation() const
    {
        return Matrix3::MakeYRotation(m_Heading) * Matrix3::MakeXRotation(m_Pitch);
    }

    void SceneViewCamera::ApplyTransform()
    {
        const Matrix3 orientation = Orientation();
        const Vector3 position = m_Pivot + orientation.GetZ() * m_Distance;
        m_TargetCamera.SetTransform(AffineTransform(orientation, position));
        m_TargetCamera.Update();
    }

    void SceneViewCamera::UpdateAspectRatio()
    {
        // 창 크기가 바뀌면 화면비를 맞춘다 (OrbitCamera와 같다).
        if (Graphics::g_DisplayWidth > 0 && Graphics::g_DisplayHeight > 0)
        {
            const float aspectRatio = float(Graphics::g_DisplayHeight) / float(Graphics::g_DisplayWidth);
            if (aspectRatio != m_AspectRatio)
            {
                m_AspectRatio = aspectRatio;
                m_TargetCamera.SetAspectRatio(aspectRatio);
            }
        }
    }

    void SceneViewCamera::Frame(const Vector3& center, float radius)
    {
        radius = XMMax(radius, 0.001f);
        const float halfVerticalFov = m_TargetCamera.GetFOV() * 0.5f;
        const float halfHorizontalFov = atanf(tanf(halfVerticalFov) / m_AspectRatio);
        const float limitingHalfFov = XMMin(halfVerticalFov, halfHorizontalFov);
        m_Pivot = center;
        m_Distance = Clamp(radius * 1.1f / sinf(limitingHalfFov), m_MinDistance, m_MaxDistance);
        ApplyTransform();
    }

    void SceneViewCamera::Update(float deltaTime)
    {
        UpdateAspectRatio();

        const bool left = GameInput::IsPressed(GameInput::kMouse0);
        const bool right = GameInput::IsPressed(GameInput::kMouse1);
        const bool middle = GameInput::IsPressed(GameInput::kMouse2);
        const bool alt = GameInput::IsPressed(GameInput::kKey_lalt);
        const bool shift = GameInput::IsPressed(GameInput::kKey_lshift) || GameInput::IsPressed(GameInput::kKey_rshift);

        // 조작은 버튼을 누른 순간 커서가 창 안에 있어야 시작하고, 그 버튼을 놓으면 끝난다.
        float cursorX, cursorY, normalizedX, normalizedY;
        const bool cursorInside = GameInput::GetCursorClientPos(cursorX, cursorY, normalizedX, normalizedY);

        if (!m_InputEnabled)
            m_Mode = Mode::None;
        if ((m_Mode == Mode::Look && !right) || (m_Mode == Mode::ZoomDrag && !right)
            || (m_Mode == Mode::Pan && !middle) || (m_Mode == Mode::Orbit && !left))
            m_Mode = Mode::None;
        if (m_InputEnabled && m_Mode == Mode::None && cursorInside)
        {
            if (GameInput::IsFirstPressed(GameInput::kMouse1))
                m_Mode = alt ? Mode::ZoomDrag : Mode::Look;
            else if (GameInput::IsFirstPressed(GameInput::kMouse2))
                m_Mode = Mode::Pan;
            else if (alt && GameInput::IsFirstPressed(GameInput::kMouse0))
                m_Mode = Mode::Orbit;
        }

        // 마우스 이동량은 이번 프레임의 상대 이동이다 (오른쪽, 위가 양수).
        const float dx = GameInput::GetAnalogInput(GameInput::kAnalogMouseX);
        const float dy = GameInput::GetAnalogInput(GameInput::kAnalogMouseY);
        const float scroll = GameInput::GetAnalogInput(GameInput::kAnalogMouseScroll);

        switch (m_Mode)
        {
        case Mode::Look:
        {
            // 눈 위치는 그대로 두고 방향만 바꾼다. 피벗은 새 방향의 앞으로 다시 잡는다.
            const Vector3 eye = m_Pivot + Orientation().GetZ() * m_Distance;
            m_Heading -= dx * kLookSensitivity;
            m_Pitch = Clamp(m_Pitch + dy * kLookSensitivity, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
            if (m_Heading > XM_PI)
                m_Heading -= XM_2PI;
            else if (m_Heading <= -XM_PI)
                m_Heading += XM_2PI;
            const Matrix3 orientation = Orientation();

            const float speed = m_FocusRadius * kMoveSpeed * m_SpeedScale * (shift ? kFastFactor : 1.0f) * deltaTime;
            const float forward = (GameInput::IsPressed(GameInput::kKey_w) ? 1.0f : 0.0f) - (GameInput::IsPressed(GameInput::kKey_s) ? 1.0f : 0.0f);
            const float strafe = (GameInput::IsPressed(GameInput::kKey_d) ? 1.0f : 0.0f) - (GameInput::IsPressed(GameInput::kKey_a) ? 1.0f : 0.0f);
            const float ascent = (GameInput::IsPressed(GameInput::kKey_e) ? 1.0f : 0.0f) - (GameInput::IsPressed(GameInput::kKey_q) ? 1.0f : 0.0f);
            const Vector3 position = eye + (-orientation.GetZ()) * (forward * speed) + orientation.GetX() * (strafe * speed)
                + m_WorldUp * (ascent * speed);
            m_Pivot = position - orientation.GetZ() * m_Distance;

            if (scroll != 0.0f)
                m_SpeedScale = Clamp(m_SpeedScale * powf(1.25f, scroll), kMinSpeedScale, kMaxSpeedScale);
            break;
        }

        case Mode::Pan:
        {
            // 화면에서 끈 만큼 장면이 따라오도록 피벗을 반대로 옮긴다. 한 픽셀의 world 길이는 피벗 거리에 비례한다.
            const Matrix3 orientation = Orientation();
            const float height = Graphics::g_DisplayHeight > 0 ? float(Graphics::g_DisplayHeight) : 1080.0f;
            const float perCount = 2.0f * m_Distance * tanf(m_TargetCamera.GetFOV() * 0.5f) / height / kMouseCount;
            m_Pivot -= orientation.GetX() * (dx * perCount);
            m_Pivot -= orientation.GetY() * (dy * perCount);
            break;
        }

        case Mode::Orbit:
            // 피벗은 그대로이고 카메라가 그 둘레를 돈다.
            m_Heading -= dx * kOrbitSensitivity;
            m_Pitch = Clamp(m_Pitch + dy * kOrbitSensitivity, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
            if (m_Heading > XM_PI)
                m_Heading -= XM_2PI;
            else if (m_Heading <= -XM_PI)
                m_Heading += XM_2PI;
            break;

        case Mode::ZoomDrag:
            // 오른쪽이나 위로 끌면 피벗에 다가간다.
            m_Distance = Clamp(m_Distance * expf(-(dx + dy) * kZoomDragSpeed), m_MinDistance, m_MaxDistance);
            break;

        case Mode::None:
            if (m_InputEnabled && cursorInside && scroll != 0.0f)
                m_Distance = Clamp(m_Distance * powf(kZoomPerWheel, scroll), m_MinDistance, m_MaxDistance);
            break;
        }

        ApplyTransform();
    }
}
