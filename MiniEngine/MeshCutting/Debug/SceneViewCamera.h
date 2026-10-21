#pragma once

// Unity Scene 뷰처럼 움직이는 카메라. 설계: Docs/SceneViewControlsDesign.md 2절.
//   오른쪽 버튼을 누른 채: 마우스 = 둘러보기, W/A/S/D = 앞/왼/뒤/오른쪽, Q/E = 아래/위, Shift = 빠르게, 휠 = 이동 속도
//   가운데 버튼 드래그 = pan,  Alt + 왼쪽 드래그 = 피벗 중심 궤도 회전,  Alt + 오른쪽 드래그 = 피벗 방향 줌
//   휠(오른쪽 버튼 없이) = 피벗 방향 줌,  Frame()은 F 키가 부른다.
// 피벗은 카메라 앞 distance만큼 떨어진 점이다. 줌과 pan, 이동으로 함께 갱신된다.

#include "CameraController.h"
#include "Camera.h"

namespace MeshCutting
{
    class SceneViewCamera : public CameraController
    {
    public:
        // focus: 처음 화면에 맞출 대상. 이동 속도와 줌 범위는 이 반지름에 비례한다.
        SceneViewCamera(Math::Camera& camera, const Math::BoundingSphere& focus,
            Math::Vector3 worldUp = Math::Vector3(Math::kYUnitVector));

        virtual void Update(float dt) override;

        // 기즈모를 드래그하는 동안은 끈다. 꺼져 있으면 새 카메라 조작을 시작하지 않는다.
        void SetInputEnabled(bool enabled) { m_InputEnabled = enabled; }

        // 오른쪽 버튼을 누르고 있다 (W/A/S/D/Q/E가 이동). 이때는 도구 전환 키(W/E/R/Q)를 도구로 받으면 안 된다.
        bool IsLooking() const { return m_Mode == Mode::Look; }
        // 어떤 카메라 조작이든 진행 중이다.
        bool IsNavigating() const { return m_Mode != Mode::None; }

        // 대상(center, radius)이 화면에 맞도록 피벗과 거리를 정한다. 보는 방향은 그대로다.
        void Frame(const Math::Vector3& center, float radius);

        float Distance() const { return m_Distance; }
        Math::Vector3 Pivot() const { return m_Pivot; }

    private:
        enum class Mode { None, Look, Pan, Orbit, ZoomDrag };

        Math::Matrix3 Orientation() const;
        void ApplyTransform();
        void UpdateAspectRatio();

        Math::Vector3 m_WorldUp;
        Math::Vector3 m_Pivot;
        float m_Distance = 1.0f;
        float m_Heading = 0.0f;
        float m_Pitch = 0.0f;
        float m_FocusRadius = 1.0f;     // 처음 대상의 반지름: 이동 속도와 거리 범위의 기준
        float m_MinDistance = 0.001f;
        float m_MaxDistance = 1000.0f;
        float m_SpeedScale = 1.0f;      // 휠로 조절하는 이동 속도 배율
        float m_AspectRatio = 9.0f / 16.0f;
        Mode m_Mode = Mode::None;
        bool m_InputEnabled = true;
    };
}
