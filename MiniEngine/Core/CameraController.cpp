//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author:  James Stanard 
//

#include "pch.h"
#include "CameraController.h"
#include "Camera.h"
#include "GameInput.h"

using namespace Math;
using namespace GameCore;

// 월드 기준축과 자유 이동 카메라의 입력·관성 상태를 초기화한다.
FlyingFPSCamera::FlyingFPSCamera( Camera& camera, Vector3 worldUp ) : CameraController( camera )
{
    // 월드 위쪽 방향으로부터 카메라 이동의 기준축을 구성한다.
    m_WorldUp = Normalize(worldUp);
    m_WorldNorth = Normalize(Cross(m_WorldUp, Vector3(kXUnitVector)));
    m_WorldEast = Cross(m_WorldNorth, m_WorldUp);

    // 회전 감도와 기본 이동 속도를 설정한다.
    m_HorizontalLookSensitivity = 2.0f;
    m_VerticalLookSensitivity = 2.0f;
    m_MoveSpeed = 1000.0f;
    m_StrafeSpeed = 1000.0f;
    m_MouseSensitivityX = 1.0f;
    m_MouseSensitivityY = 1.0f;

    // 현재 카메라 자세에서 초기 피치와 수평 회전각을 구한다.
    m_CurrentPitch = Sin(Dot(camera.GetForwardVec(), m_WorldUp));
    Vector3 forward = Normalize(Cross(m_WorldUp, camera.GetRightVec()));
    m_CurrentHeading = ATan2(-Dot(forward, m_WorldEast), Dot(forward, m_WorldNorth));

    // 회전 감속과 관성 적용 여부를 초기화한다.
    m_FineRotation = false;
    m_Momentum = true;

    // 이전 프레임의 회전과 이동 관성을 초기화한다.
    m_LastYaw = 0.0f;
    m_LastPitch = 0.0f;
    m_LastForward = 0.0f;
    m_LastStrafe = 0.0f;
    m_LastAscent = 0.0f;
}

namespace Graphics
{
    extern EnumVar DebugZoom;
}

// 입력과 관성을 카메라에 반영하고 Shift를 누르는 동안 이동 속도를 높인다.
void FlyingFPSCamera::Update( float deltaTime )
{
    // 디버그 확대 상태에 맞춰 입력 시간 배율을 정한다.
    float timeScale = Graphics::DebugZoom == 0 ? 1.0f : Graphics::DebugZoom == 1 ? 0.5f : 0.25f;

    // 회전 감속 토글을 유지하고 양쪽 Shift의 현재 눌림 상태를 확인한다.
    if (GameInput::IsFirstPressed(GameInput::kRThumbClick))
        m_FineRotation = !m_FineRotation;

    const bool fastMovement = GameInput::IsPressed(GameInput::kKey_lshift) || GameInput::IsPressed(GameInput::kKey_rshift);
    float speedScale = (fastMovement ? 4.0f : 1.0f) * timeScale;
    float panScale = (m_FineRotation ? 0.5f : 1.0f) * timeScale;

    // 회전 입력과 전후·좌우·상하 이동량을 계산한다.
    float yaw = GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogRightStickX ) * m_HorizontalLookSensitivity * panScale;
    float pitch = GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogRightStickY ) * m_VerticalLookSensitivity * panScale;
    float forward =	m_MoveSpeed * speedScale * (
        GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogLeftStickY ) +
        (GameInput::IsPressed( GameInput::kKey_w ) ? deltaTime : 0.0f) +
        (GameInput::IsPressed( GameInput::kKey_s ) ? -deltaTime : 0.0f)
        );
    float strafe = m_StrafeSpeed * speedScale * (
        GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogLeftStickX  ) +
        (GameInput::IsPressed( GameInput::kKey_d ) ? deltaTime : 0.0f) +
        (GameInput::IsPressed( GameInput::kKey_a ) ? -deltaTime : 0.0f)
        );
    float ascent = m_StrafeSpeed * speedScale * (
        GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogRightTrigger ) -
        GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogLeftTrigger ) +
        (GameInput::IsPressed( GameInput::kKey_e ) ? deltaTime : 0.0f) +
        (GameInput::IsPressed( GameInput::kKey_q ) ? -deltaTime : 0.0f)
        );

    // 활성화된 관성으로 회전과 이동 입력을 부드럽게 연결한다.
    if (m_Momentum)
    {
        ApplyMomentum(m_LastYaw, yaw, deltaTime);
        ApplyMomentum(m_LastPitch, pitch, deltaTime);
        ApplyMomentum(m_LastForward, forward, deltaTime);
        ApplyMomentum(m_LastStrafe, strafe, deltaTime);
        ApplyMomentum(m_LastAscent, ascent, deltaTime);
    }

    // 마우스 회전 입력은 관성을 적용하지 않고 더한다.
    yaw += GameInput::GetAnalogInput(GameInput::kAnalogMouseX) * m_MouseSensitivityX;
    pitch += GameInput::GetAnalogInput(GameInput::kAnalogMouseY) * m_MouseSensitivityY;

    // 수직 회전각을 위아래 보기 범위로 제한한다.
    m_CurrentPitch += pitch;
    m_CurrentPitch = XMMin( XM_PIDIV2, m_CurrentPitch);
    m_CurrentPitch = XMMax(-XM_PIDIV2, m_CurrentPitch);

    // 수평 회전각을 한 바퀴 범위로 정규화한다.
    m_CurrentHeading -= yaw;
    if (m_CurrentHeading > XM_PI)
        m_CurrentHeading -= XM_2PI;
    else if (m_CurrentHeading <= -XM_PI)
        m_CurrentHeading += XM_2PI; 

    // 계산한 자세와 이동량으로 카메라 변환을 갱신한다.
    Matrix3 orientation = Matrix3(m_WorldEast, m_WorldUp, -m_WorldNorth) * Matrix3::MakeYRotation( m_CurrentHeading ) * Matrix3::MakeXRotation( m_CurrentPitch );
    Vector3 position = orientation * Vector3( strafe, ascent, -forward ) + m_TargetCamera.GetPosition();
    m_TargetCamera.SetTransform( AffineTransform( orientation, position ) );
    m_TargetCamera.Update();
}

void FlyingFPSCamera::SetHeadingPitchAndPosition(float heading, float pitch, const Vector3& position)
{
    m_CurrentHeading = heading;
    if (m_CurrentHeading > XM_PI)
        m_CurrentHeading -= XM_2PI;
    else if (m_CurrentHeading <= -XM_PI)
        m_CurrentHeading += XM_2PI; 

    m_CurrentPitch = pitch;
    m_CurrentPitch = XMMin( XM_PIDIV2, m_CurrentPitch);
    m_CurrentPitch = XMMax(-XM_PIDIV2, m_CurrentPitch);

    Matrix3 orientation =
        Matrix3(m_WorldEast, m_WorldUp, -m_WorldNorth) * 
        Matrix3::MakeYRotation( m_CurrentHeading ) *
        Matrix3::MakeXRotation( m_CurrentPitch );

    m_TargetCamera.SetTransform( AffineTransform( orientation, position ) );
    m_TargetCamera.Update();
}


void CameraController::ApplyMomentum( float& oldValue, float& newValue, float deltaTime )
{
    float blendedValue;
    if (Abs(newValue) > Abs(oldValue))
        blendedValue = Lerp(newValue, oldValue, Pow(0.6f, deltaTime * 60.0f));
    else
        blendedValue = Lerp(newValue, oldValue, Pow(0.8f, deltaTime * 60.0f));
    oldValue = blendedValue;
    newValue = blendedValue;
}

OrbitCamera::OrbitCamera( Camera& camera, Math::BoundingSphere focus, Vector3 worldUp ) : CameraController( camera )
{
    m_ModelBounds = focus;
    m_WorldUp = Normalize(worldUp);

    m_JoystickSensitivityX = 2.0f;
    m_JoystickSensitivityY = 2.0f;

    m_MouseSensitivityX = 1.0f;
    m_MouseSensitivityY = 1.0f;

    m_CurrentPitch = 0.0f;
    m_CurrentHeading = 0.0f;
    m_CurrentCloseness = 0.5f;

    m_Momentum = true;

    m_LastYaw = 0.0f;
    m_LastPitch = 0.0f;
}

void OrbitCamera::Update( float deltaTime )
{
    (deltaTime);

    float timeScale = Graphics::DebugZoom == 0 ? 1.0f : Graphics::DebugZoom == 1 ? 0.5f : 0.25f;

    float yaw = GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogLeftStickX ) * timeScale * m_JoystickSensitivityX;
    float pitch = GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogLeftStickY ) * timeScale * m_JoystickSensitivityY;
    float closeness = GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogRightStickY ) * timeScale;

    if (m_Momentum)
    {
        ApplyMomentum(m_LastYaw, yaw, deltaTime);
        ApplyMomentum(m_LastPitch, pitch, deltaTime);
    }

    // don't apply momentum to mouse inputs
    yaw += GameInput::GetAnalogInput(GameInput::kAnalogMouseX) * m_MouseSensitivityX;
    pitch += GameInput::GetAnalogInput(GameInput::kAnalogMouseY) * m_MouseSensitivityY;
    closeness += GameInput::GetAnalogInput(GameInput::kAnalogMouseScroll) * 0.1f;

    m_CurrentPitch += pitch;
    m_CurrentPitch = XMMin( XM_PIDIV2, m_CurrentPitch);
    m_CurrentPitch = XMMax(-XM_PIDIV2, m_CurrentPitch);

    m_CurrentHeading -= yaw;
    if (m_CurrentHeading > XM_PI)
        m_CurrentHeading -= XM_2PI;
    else if (m_CurrentHeading <= -XM_PI)
        m_CurrentHeading += XM_2PI; 

    m_CurrentCloseness += closeness;
    m_CurrentCloseness = Clamp(m_CurrentCloseness, 0.0f, 1.0f);

    Matrix3 orientation = Matrix3::MakeYRotation( m_CurrentHeading ) * Matrix3::MakeXRotation( m_CurrentPitch );
    Vector3 position = orientation.GetZ() * (m_ModelBounds.GetRadius() * Lerp(3.0f, 1.0f, m_CurrentCloseness) + m_TargetCamera.GetNearClip());
    m_TargetCamera.SetTransform(AffineTransform(orientation, position + m_ModelBounds.GetCenter()));
    m_TargetCamera.Update();
}
