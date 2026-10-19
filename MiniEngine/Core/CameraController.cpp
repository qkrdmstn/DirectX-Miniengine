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
#include "Display.h"

using namespace Math;
using namespace GameCore;

FlyingFPSCamera::FlyingFPSCamera( Camera& camera, Vector3 worldUp ) : CameraController( camera )
{
    m_WorldUp = Normalize(worldUp);
    m_WorldNorth = Normalize(Cross(m_WorldUp, Vector3(kXUnitVector)));
    m_WorldEast = Cross(m_WorldNorth, m_WorldUp);

    m_HorizontalLookSensitivity = 2.0f;
    m_VerticalLookSensitivity = 2.0f;
    m_MoveSpeed = 1000.0f;
    m_StrafeSpeed = 1000.0f;
    m_MouseSensitivityX = 1.0f;
    m_MouseSensitivityY = 1.0f;

    m_CurrentPitch = Sin(Dot(camera.GetForwardVec(), m_WorldUp));

    Vector3 forward = Normalize(Cross(m_WorldUp, camera.GetRightVec()));
    m_CurrentHeading = ATan2(-Dot(forward, m_WorldEast), Dot(forward, m_WorldNorth));

    m_FineMovement = false;
    m_FineRotation = false;
    m_Momentum = true;

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

void FlyingFPSCamera::Update( float deltaTime )
{
    (deltaTime);

    float timeScale = Graphics::DebugZoom == 0 ? 1.0f : Graphics::DebugZoom == 1 ? 0.5f : 0.25f;

    if (GameInput::IsFirstPressed(GameInput::kLThumbClick) || GameInput::IsFirstPressed(GameInput::kKey_lshift))
        m_FineMovement = !m_FineMovement;

    if (GameInput::IsFirstPressed(GameInput::kRThumbClick))
        m_FineRotation = !m_FineRotation;

    float speedScale = (m_FineMovement ? 0.1f : 1.0f) * timeScale;
    float panScale = (m_FineRotation ? 0.5f : 1.0f) * timeScale;

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

    if (m_Momentum)
    {
        ApplyMomentum(m_LastYaw, yaw, deltaTime);
        ApplyMomentum(m_LastPitch, pitch, deltaTime);
        ApplyMomentum(m_LastForward, forward, deltaTime);
        ApplyMomentum(m_LastStrafe, strafe, deltaTime);
        ApplyMomentum(m_LastAscent, ascent, deltaTime);
    }

    // don't apply momentum to mouse inputs
    yaw += GameInput::GetAnalogInput(GameInput::kAnalogMouseX) * m_MouseSensitivityX;
    pitch += GameInput::GetAnalogInput(GameInput::kAnalogMouseY) * m_MouseSensitivityY;

    m_CurrentPitch += pitch;
    m_CurrentPitch = XMMin( XM_PIDIV2, m_CurrentPitch);
    m_CurrentPitch = XMMax(-XM_PIDIV2, m_CurrentPitch);

    m_CurrentHeading -= yaw;
    if (m_CurrentHeading > XM_PI)
        m_CurrentHeading -= XM_2PI;
    else if (m_CurrentHeading <= -XM_PI)
        m_CurrentHeading += XM_2PI; 

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
    m_FocusCenter = focus.GetCenter();

    m_JoystickSensitivityX = 2.0f;
    m_JoystickSensitivityY = 2.0f;

    m_MouseSensitivityX = 1.0f;
    m_MouseSensitivityY = 1.0f;

    m_CurrentPitch = 0.0f;
    m_CurrentHeading = 0.0f;

    const float radius = XMMax((float)focus.GetRadius(), 0.001f);
    m_MinDistance = XMMax(radius * 0.05f, 0.001f);
    m_MaxDistance = radius * 100.0f;
    m_AspectRatio = Graphics::g_DisplayWidth > 0 && Graphics::g_DisplayHeight > 0
        ? (float)Graphics::g_DisplayHeight / Graphics::g_DisplayWidth : 9.0f / 16.0f;
    m_TargetCamera.SetAspectRatio(m_AspectRatio);
    m_TargetCamera.SetZRange(XMMax(radius * 0.001f, 0.0001f),
        XMMax(m_TargetCamera.GetFarClip(), radius * 101.0f));
    m_Momentum = true;
    m_LastYaw = 0.0f;
    m_LastPitch = 0.0f;
    FrameModel();
}

void OrbitCamera::FrameModel()
{
    m_FocusCenter = m_ModelBounds.GetCenter();

    const float halfVerticalFov = m_TargetCamera.GetFOV() * 0.5f;
    const float halfHorizontalFov = atanf(tanf(halfVerticalFov) / m_AspectRatio);
    const float limitingHalfFov = XMMin(halfVerticalFov, halfHorizontalFov);
    const float radius = XMMax((float)m_ModelBounds.GetRadius(), 0.001f);
    m_Distance = Clamp(radius * 1.1f / sinf(limitingHalfFov), m_MinDistance, m_MaxDistance);
    m_LastYaw = 0.0f;
    m_LastPitch = 0.0f;
}

void OrbitCamera::Update( float deltaTime )
{
    if (Graphics::g_DisplayWidth > 0 && Graphics::g_DisplayHeight > 0)
    {
        const float aspectRatio = (float)Graphics::g_DisplayHeight / Graphics::g_DisplayWidth;
        if (aspectRatio != m_AspectRatio)
        {
            m_AspectRatio = aspectRatio;
            m_TargetCamera.SetAspectRatio(aspectRatio);
        }
    }

    float timeScale = Graphics::DebugZoom == 0 ? 1.0f : Graphics::DebugZoom == 1 ? 0.5f : 0.25f;

    float yaw = GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogLeftStickX ) * timeScale * m_JoystickSensitivityX;
    float pitch = GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogLeftStickY ) * timeScale * m_JoystickSensitivityY;
    float zoom = GameInput::GetTimeCorrectedAnalogInput( GameInput::kAnalogRightStickY ) * timeScale;

    if (m_Momentum)
    {
        ApplyMomentum(m_LastYaw, yaw, deltaTime);
        ApplyMomentum(m_LastPitch, pitch, deltaTime);
    }

    // Mouse analog values are relative movement for this frame, not velocities.
    const bool rotating = GameInput::IsPressed(GameInput::kMouse1)
        && !GameInput::IsFirstPressed(GameInput::kMouse1);
    const bool panning = GameInput::IsPressed(GameInput::kMouse2)
        && !GameInput::IsFirstPressed(GameInput::kMouse2);
    if (rotating)
    {
        yaw += GameInput::GetAnalogInput(GameInput::kAnalogMouseX) * m_MouseSensitivityX;
        pitch += GameInput::GetAnalogInput(GameInput::kAnalogMouseY) * m_MouseSensitivityY;
    }
    zoom += GameInput::GetAnalogInput(GameInput::kAnalogMouseScroll) * 0.1f;

    m_CurrentPitch += pitch;
    m_CurrentPitch = Clamp(m_CurrentPitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);

    m_CurrentHeading -= yaw;
    if (m_CurrentHeading > XM_PI)
        m_CurrentHeading -= XM_2PI;
    else if (m_CurrentHeading <= -XM_PI)
        m_CurrentHeading += XM_2PI; 

    m_Distance = Clamp(m_Distance * Pow(0.2f, zoom), m_MinDistance, m_MaxDistance);

    Matrix3 orientation = Matrix3::MakeYRotation( m_CurrentHeading ) * Matrix3::MakeXRotation( m_CurrentPitch );
    if (panning && !rotating && Graphics::g_DisplayHeight > 0)
    {
        // GameInput scales each mouse count by 0.0018; undo that for pixel-based panning.
        const float unitsPerAnalog = 2.0f * m_Distance * tanf(m_TargetCamera.GetFOV() * 0.5f)
            / (Graphics::g_DisplayHeight * 0.0018f);
        m_FocusCenter -= orientation.GetX() *
            (GameInput::GetAnalogInput(GameInput::kAnalogMouseX) * unitsPerAnalog);
        m_FocusCenter -= orientation.GetY() *
            (GameInput::GetAnalogInput(GameInput::kAnalogMouseY) * unitsPerAnalog);
    }

    if (GameInput::IsFirstPressed(GameInput::kKey_f))
        FrameModel();

    Vector3 position = orientation.GetZ() * m_Distance;
    m_TargetCamera.SetTransform(AffineTransform(orientation, position + m_FocusCenter));
    m_TargetCamera.Update();
}
