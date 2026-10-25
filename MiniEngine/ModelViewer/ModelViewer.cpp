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

#include "GameCore.h"
#include "CameraController.h"
#include "BufferManager.h"
#include "Camera.h"
#include "CommandContext.h"
#include "TemporalEffects.h"
#include "MotionBlur.h"
#include "DepthOfField.h"
#include "PostEffects.h"
#include "SSAO.h"
#include "FXAA.h"
#include "SystemTime.h"
#include "TextRenderer.h"
#include "ParticleEffectManager.h"
#include "GameInput.h"
#include "SponzaRenderer.h"
#include "glTF.h"
#include "Renderer.h"
#include "Model.h"
#include "ModelLoader.h"
#include "ShadowCamera.h"
#include "Display.h"
#include "Scene/ObjectSelection.h"
#include "Scene/TransformGizmo.h"
#include "Scene/CutPlaneSystem.h"
#include "Scene/CutPlaneRenderer.h"
#include "Scene/CuttingSystem.h"
#include <algorithm>
#include "Scene/tests/MeshComponentChecks.h"
#include <cmath>

using namespace GameCore;
using namespace Math;
using namespace Graphics;
using namespace std;

using Renderer::MeshSorter;

class ModelViewer : public GameCore::IGameApp
{
public:

    ModelViewer( void ) {}

    virtual void Startup( void ) override;
    virtual void Cleanup( void ) override;

    virtual void Update( float deltaT ) override;
    virtual void RenderScene( void ) override;
    virtual void RenderUI(GraphicsContext& context) override;
    virtual bool IsDone() override { return !m_EscapeConsumed && IGameApp::IsDone(); }

private:

    Camera m_Camera;
    unique_ptr<CameraController> m_CameraController;

    D3D12_VIEWPORT m_MainViewport;
    D3D12_RECT m_MainScissor;

    Viewer::Scene m_Scene;
    Viewer::CuttingSystem m_Cutting{m_Scene};
    Viewer::ObjectId m_CutOwner = Viewer::InvalidObjectId, m_ActiveCutPlane = Viewer::InvalidObjectId;
    Viewer::ComponentId m_CutMesh = Viewer::InvalidComponentId;
    uint32_t m_AutoCuts = 0, m_BenchmarkRemaining = 0, m_BenchmarkWarmup = 3, m_AutoCutIndex = 0;
    float m_AutoCutY = 0, m_AutoCutWidth = 1000;
    uint32_t m_CapMode = 0;
    uint32_t m_RepeatCutPlane = 0;
    uint32_t m_CutScene = 0, m_CutOther = 0;
    float m_CutAngle = 0, m_CutStepY = 0;
    float m_CutGap = 0.0f;
    uint32_t m_ExpectedPublications = ~0u, m_Publications = 0;
    std::vector<double> m_CutGpuSamples, m_CutReadySamples;
    Math::Vector3 m_LastCutNormal{Math::kYUnitVector};
    bool m_CaptureComplete = false;
    Viewer::CutPlaneSystem m_CutPlanes{m_Scene};
    Viewer::ObjectId m_SelectedObject = Viewer::InvalidObjectId;
    Viewer::TransformGizmo m_Gizmo;
    bool m_EscapeConsumed = false;
    Renderer::RenderMode m_RenderMode = Renderer::RenderMode::Solid;
    bool m_ViewDiagnosticPending = false;
    std::wstring m_CapturePath;
    uint32_t m_CaptureFrame = 0;
    uint32_t m_CaptureTargetFrame = 60;
    uint32_t m_AnimationStepMs = 0;
    bool m_CapturePostPending = false;
    bool m_ComponentChecksPending = false;
    ShadowCamera m_SunShadowCamera;
};

CREATE_APPLICATION( ModelViewer )

ExpVar g_SunLightIntensity("Viewer/Lighting/Sun Light Intensity", 4.0f, 0.0f, 16.0f, 0.1f);
NumVar g_SunOrientation("Viewer/Lighting/Sun Orientation", -0.5f, -100.0f, 100.0f, 0.1f );
NumVar g_SunInclination("Viewer/Lighting/Sun Inclination", 0.75f, 0.0f, 1.0f, 0.01f );

void ChangeIBLSet(EngineVar::ActionType);
void ChangeIBLBias(EngineVar::ActionType);

DynamicEnumVar g_IBLSet("Viewer/Lighting/Environment", ChangeIBLSet);
std::vector<std::pair<TextureRef, TextureRef>> g_IBLTextures;
NumVar g_IBLBias("Viewer/Lighting/Gloss Reduction", 2.0f, 0.0f, 10.0f, 1.0f, ChangeIBLBias);

void ChangeIBLSet(EngineVar::ActionType)
{
    int setIdx = g_IBLSet - 1;
    if (setIdx < 0)
    {
        Renderer::SetIBLTextures(nullptr, nullptr);
    }
    else
    {
        auto texturePair = g_IBLTextures[setIdx];
        Renderer::SetIBLTextures(texturePair.first, texturePair.second);
    }
}

void ChangeIBLBias(EngineVar::ActionType)
{
    Renderer::SetIBLBias(g_IBLBias);
}

#include <direct.h> // for _getcwd() to check data root path

void LoadIBLTextures()
{
    char CWD[256];
    _getcwd(CWD, 256);

    Utility::Printf("Loading IBL environment maps\n");

    WIN32_FIND_DATA ffd;
    HANDLE hFind = FindFirstFile(L"Textures/*_diffuseIBL.dds", &ffd);

    g_IBLSet.AddEnum(L"None");

    if (hFind != INVALID_HANDLE_VALUE) do
    {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

       std::wstring diffuseFile = ffd.cFileName;
       std::wstring baseFile = diffuseFile; 
       baseFile.resize(baseFile.rfind(L"_diffuseIBL.dds"));
       std::wstring specularFile = baseFile + L"_specularIBL.dds";

       TextureRef diffuseTex = TextureManager::LoadDDSFromFile(L"Textures/" + diffuseFile);
       if (diffuseTex.IsValid())
       {
           TextureRef specularTex = TextureManager::LoadDDSFromFile(L"Textures/" + specularFile);
           if (specularTex.IsValid())
           {
               g_IBLSet.AddEnum(baseFile);
               g_IBLTextures.push_back(std::make_pair(diffuseTex, specularTex));
           }
       }
    }
    while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);

    Utility::Printf("Found %u IBL environment map sets\n", g_IBLTextures.size());

    if (g_IBLTextures.size() > 0)
        g_IBLSet.Increment();
}

// 모델·평면과 GPU 파이프라인을 준비하고 명시적 검증 옵션을 읽는다.
void ModelViewer::Startup( void )
{
    CommandLineArgs::GetString(L"capture", m_CapturePath);
    uint32_t computePose;
    if (CommandLineArgs::GetInteger(L"compute-pose", computePose))
        Renderer::UseComputePose = computePose != 0;
    if (!m_CapturePath.empty())
    {
        uint32_t targetFrame;
        if (CommandLineArgs::GetInteger(L"capture-frame", targetFrame) && targetFrame > 0)
            m_CaptureTargetFrame = targetFrame;
        CommandLineArgs::GetInteger(L"animation-step-ms", m_AnimationStepMs);
        Utility::Printf("ModelViewer capture: computePose=%u, frame=%u, animationStepMs=%u\n",
            Renderer::UseComputePose ? 1u : 0u, m_CaptureTargetFrame, m_AnimationStepMs);
    }
    uint32_t viewMode;
    if (CommandLineArgs::GetInteger(L"view", viewMode) && viewMode <= 2)
    {
        m_RenderMode = static_cast<Renderer::RenderMode>(viewMode);
        m_ViewDiagnosticPending = true;
        Utility::Printf("ModelViewer diagnostic: initial view=%u\n", viewMode);
    }

    if (!GameInput::SetMouseExclusive(false))
        Utility::Printf("Unable to enable the ModelViewer free cursor.\n");
    MotionBlur::Enable = false;
    // Camera-only velocity cannot reproject independently moving objects or skins.
    TemporalEffects::EnableTAA = false;
    FXAA::Enable = true;
    PostEffects::EnableHDR = true;
    PostEffects::EnableAdaptation = true;
    SSAO::Enable = true;

    Renderer::Initialize();

    LoadIBLTextures();

    std::wstring gltfFileName;

    bool forceRebuild = false;
    uint32_t rebuildValue;
    if (CommandLineArgs::GetInteger(L"rebuild", rebuildValue))
        forceRebuild = rebuildValue != 0;

    if (CommandLineArgs::GetString(L"model", gltfFileName) == false)
    {
        m_Scene.CreateObject("Cube", Renderer::LoadModel(L"Models/cube.glb", forceRebuild));
        m_Scene.CreateObject("Bunny", Renderer::LoadModel(L"Models/bunny.glb", forceRebuild));
        m_Scene.CreateObject("Xbot", Renderer::LoadModel(L"Models/Xbot.glb", forceRebuild)).PlayAnimation(0);
    }
    else
    {
        m_Scene.CreateObject("Model", Renderer::LoadModel(gltfFileName, forceRebuild)).LoopAllAnimations();
    }

    // 생성 표면/cap의 렌더 PSO도 첫 결과 게시가 아닌 로딩 단계에 준비한다.
    MeshCutting::GpuMeshCutter::Prepare();
    Renderer::GetPSO(31);
    for (const auto& object : m_Scene.GetObjects())
        for (const auto& component : object->GetMeshComponents())
            for (size_t section=0;section<component->GetGeometry()->GetSections().size();++section)
            {
                const uint16_t flags=component->GetGeometry()->GetMesh(section).psoFlags;
                if (!(flags&PSOFlags::kHasSkin)) Renderer::GetPSO(uint16_t(flags|PSOFlags::kHasUV0));
            }
    size_t index = 0;
    for (const auto& object : m_Scene.GetObjects())
    {
        const ModelInstance* model = object->GetModelInstance();
        if (!model || float(model->GetModel()->m_BoundingSphere.GetRadius()) <= 0.0f)
            continue;
        const float scale = 2.0f / float(model->GetModel()->m_BoundingSphere.GetRadius());
        const float x = (float(index++) - 0.5f * float(m_Scene.GetObjects().size() - 1)) * 5.0f;
        object->SetTransform(UniformTransform(Quaternion(kIdentity), scale,
            Vector3(x, 0.0f, 0.0f) - model->GetModel()->m_BoundingSphere.GetCenter() * scale));
    }
    uint32_t componentChecks = 0;
    if (CommandLineArgs::GetInteger(L"component-checks", componentChecks) && componentChecks == 1)
    {
        Viewer::PrepareMeshComponentChecks(m_Scene);
        m_ComponentChecksPending = true;
    }
    m_ActiveCutPlane = m_CutPlanes.CreatePlane({4.0f, 4.0f}, Viewer::SceneTransform(kIdentity));
    CommandLineArgs::GetInteger(L"cut", m_AutoCuts);
    CommandLineArgs::GetInteger(L"cut-benchmark", m_BenchmarkRemaining);
    CommandLineArgs::GetFloat(L"cut-y", m_AutoCutY);
    CommandLineArgs::GetFloat(L"cut-width",m_AutoCutWidth);
    CommandLineArgs::GetInteger(L"cut-repeat-plane",m_RepeatCutPlane);
    CommandLineArgs::GetInteger(L"cut-scene",m_CutScene);
    CommandLineArgs::GetInteger(L"cut-other",m_CutOther);
    CommandLineArgs::GetFloat(L"cut-angle",m_CutAngle);
    CommandLineArgs::GetFloat(L"cut-step-y",m_CutStepY);
    CommandLineArgs::GetFloat(L"cut-gap",m_CutGap);
    if (!std::isfinite(m_CutGap) || m_CutGap < 0)
        throw std::invalid_argument("cut-gap must be finite and nonnegative");
    CommandLineArgs::GetInteger(L"cut-expect-publications",m_ExpectedPublications);
    uint32_t transformChecks = 0;
    if (CommandLineArgs::GetInteger(L"cut-transform-checks",transformChecks) && transformChecks)
    {
        for (const auto& object : m_Scene.GetObjects())
        {
            if (object->GetMeshComponents().empty()) continue;
            auto owner = object->GetTransform();
            owner.SetRotation(Quaternion(Vector3(kYUnitVector),0.4f)*Quaternion(Vector3(kXUnitVector),0.2f));
            owner.SetScale(owner.GetScale()*Vector3(0.8f,1.4f,0.6f));
            object->SetTransform(owner);
            Viewer::DeformBinding binding;
            binding.SourceFrame = Viewer::SceneTransform(Quaternion(Vector3(kZUnitVector),0.3f),Vector3(1.2f,0.9f,1.1f),Vector3(0.1f,0.05f,-0.1f));
            object->GetMeshComponents().front()->SetPlacement(binding.SourceFrame,Viewer::MeshTransformMode::Attached,binding);
        }
    }
    uint32_t validateCut = 0;
    CommandLineArgs::GetInteger(L"cut-validate", validateCut);
    m_Cutting.ValidateResults = validateCut != 0;
    CommandLineArgs::GetInteger(L"cut-expect-loops",m_Cutting.ExpectedMinimumLoops);
    CommandLineArgs::GetInteger(L"cap-mode", m_CapMode);
    m_CapMode = std::min(m_CapMode, 2u);
    if (m_CutScene && m_BenchmarkRemaining)
        throw std::invalid_argument("cut-scene and cut-benchmark cannot be combined");
    if (m_BenchmarkRemaining) m_BenchmarkRemaining += m_BenchmarkWarmup;
    for (const auto& object : m_Scene.GetObjects())
        if (!object->GetMeshComponents().empty())
        { m_CutOwner = object->GetId(); m_CutMesh = object->GetMeshComponents().front()->GetId(); break; }
    m_Camera.SetEyeAtUp(Vector3(10.0f, 7.0f, 15.0f), Vector3(kZero), Vector3(kYUnitVector));
    m_Camera.SetZRange(0.1f, 1000.0f);
    auto controller = std::make_unique<FlyingFPSCamera>(m_Camera, Vector3(kYUnitVector));
    controller->EnableMomentum(false);
    controller->SetMoveSpeed(5.0f);
    const Vector3 forward = m_Camera.GetForwardVec();
    controller->SetHeadingPitchAndPosition(std::atan2(-float(forward.GetX()), -float(forward.GetZ())),
        std::asin(float(forward.GetY())), m_Camera.GetPosition());
    m_CameraController = std::move(controller);
    m_Camera.Update();
    if (m_ViewDiagnosticPending)
        Utility::Printf("ModelViewer diagnostic: startup PSOs ready, view=%u\n", static_cast<unsigned>(m_RenderMode));
}

// 장면을 지운 뒤 평면 설명을 정리하고 렌더러를 종료한다.
void ModelViewer::Cleanup( void )
{
    m_Cutting.Clear();
    m_Scene.Clear();
    m_CutPlanes.PruneStaleObjects();

    g_IBLTextures.clear();

    Renderer::Shutdown();
}

namespace Graphics
{
    extern EnumVar DebugZoom;
}

// 완료된 절단을 먼저 게시하고 최신 pose 준비 뒤 현재 평면의 후보를 제출한다.
void ModelViewer::Update( float deltaT )
{
    ScopedTimer _prof(L"Update State");
    if (m_Cutting.Update())
    {
        if (m_BenchmarkRemaining)
        {
            if (m_BenchmarkWarmup) --m_BenchmarkWarmup;
            else { m_CutGpuSamples.push_back(m_Cutting.LastGpuMs); m_CutReadySamples.push_back(m_Cutting.LastReadyMs); }
            --m_BenchmarkRemaining;
            if (!m_BenchmarkRemaining && !m_CutGpuSamples.empty())
            {
                std::sort(m_CutGpuSamples.begin(), m_CutGpuSamples.end());
                std::sort(m_CutReadySamples.begin(),m_CutReadySamples.end());
                Utility::Printf("Static cut benchmark: samples=%zu medianGpuMs=%.6f p95GpuMs=%.6f medianReadyMs=%.3f error=%u\n",
                    m_CutGpuSamples.size(), m_CutGpuSamples[m_CutGpuSamples.size()/2],
                    m_CutGpuSamples[std::min(m_CutGpuSamples.size()-1, size_t(m_CutGpuSamples.size()*0.95))],m_CutReadySamples[m_CutReadySamples.size()/2],m_Cutting.LastError);
            }
        }
        else if (m_AutoCuts && !m_Cutting.IsBusy()) { --m_AutoCuts; ++m_AutoCutIndex; }
        // 기본은 위치 보존이며, 명시한 진단 간격만 새 B의 실제 Transform에 적용한다.
        if (m_Cutting.LastOther)
        {
            ++m_Publications;
            auto* owner = m_Scene.FindObject(m_Cutting.LastOwner);
            auto* piece = owner ? owner->FindMeshComponent(m_Cutting.LastOther) : nullptr;
            if (piece)
            {
                auto transform = piece->GetTransform();
                Vector3 delta = -m_LastCutNormal * m_CutGap;
                if (piece->GetTransformMode() == Viewer::MeshTransformMode::Attached)
                    delta = Vector3(Invert(Matrix4(owner->GetTransform().ToAffine())) * Vector4(delta, 0));
                if (m_CutGap > 0)
                {
                    transform.SetTranslation(transform.GetTranslation() + delta);
                    piece->SetTransform(transform, piece->GetTransformMode());
                }
                Utility::Printf("Static cut separation: owner=%llu mesh=%llu gap=%.6f delta=(%.6f,%.6f,%.6f) deltaSpace=%s\n",
                    static_cast<unsigned long long>(m_Cutting.LastOwner),static_cast<unsigned long long>(m_Cutting.LastOther),
                    m_CutGap,float(delta.GetX()),float(delta.GetY()),float(delta.GetZ()),
                    piece->GetTransformMode() == Viewer::MeshTransformMode::Attached ? "owner-local" : "world");
                if (m_CutOther) { m_CutOwner = m_Cutting.LastOwner; m_CutMesh = m_Cutting.LastOther; }
            }
        }
    }
    const bool cutKey = GameInput::IsFirstPressed(GameInput::kKey_c);

    if (GameInput::IsFirstPressed(GameInput::kKey_v))
        m_RenderMode = static_cast<Renderer::RenderMode>((static_cast<unsigned>(m_RenderMode) + 1) % 3);

    if (GameInput::IsFirstPressed(GameInput::kLShoulder))
        DebugZoom.Decrement();
    else if (GameInput::IsFirstPressed(GameInput::kRShoulder))
        DebugZoom.Increment();

    m_Camera.SetAspectRatio(float(g_SceneColorBuffer.GetHeight()) / float(g_SceneColorBuffer.GetWidth()));
    m_Camera.Update();
    m_CutPlanes.PruneStaleObjects();
    m_EscapeConsumed = m_Gizmo.IsDragging() && GameInput::IsFirstPressed(GameInput::kKey_escape);
    const bool gizmoInput = m_Gizmo.Update(m_Scene, m_SelectedObject, m_Camera,
        float(g_OverlayBuffer.GetWidth()), float(g_OverlayBuffer.GetHeight()), m_CutPlanes.Find(m_SelectedObject) ? 5u : 7u);
    // Hold the right button to navigate; the free cursor selects objects with the left button.
    if (!gizmoInput && !m_Gizmo.IsDragging() && GameInput::IsPressed(GameInput::kMouse1))
        m_CameraController->Update(deltaT);

    float mouseX, mouseY;
    if (!gizmoInput && GameInput::IsFirstPressed(GameInput::kMouse0) && !GameInput::IsPressed(GameInput::kMouse1) &&
        GameInput::GetMousePosition(mouseX, mouseY))
    {
        const auto ray = Viewer::MakePickingRay(m_Camera, mouseX, mouseY);
        const Viewer::PickHit hit = Viewer::NearestPickHit(Viewer::PickObject(m_Scene, ray), m_CutPlanes.Pick(ray));
        m_SelectedObject = hit.Id;
        if (hit.MeshId) { m_CutOwner = hit.Id; m_CutMesh = hit.MeshId; }
        if (m_CutPlanes.Find(hit.Id)) m_ActiveCutPlane = hit.Id;
    }

    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");

    m_Scene.Update(gfxContext, m_AnimationStepMs > 0 ? m_AnimationStepMs * 0.001f : deltaT);
    if (m_ComponentChecksPending)
    {
        Viewer::CheckPreparedMeshComponents(m_Scene, gfxContext);
        m_ComponentChecksPending = false;
    }

    gfxContext.Finish();
    m_Cutting.SubmitPending();
    if (cutKey)
    {
        MeshCutting::CutPlaneData plane;
        if (m_CutPlanes.BuildData(m_ActiveCutPlane,plane))
        {
            if (m_Cutting.RequestAll(plane,m_CapMode)) m_LastCutNormal = Vector3(plane.WorldPlane);
        }
    }
    if (!m_Cutting.IsBusy() && (m_AutoCuts || m_BenchmarkRemaining))
    {
        MeshCutting::CutPlaneData plane;
        bool valid = false;
        float epsilon = 1.0e-5f;
        if (m_AutoCuts || m_BenchmarkRemaining)
        {
            auto* owner = m_Scene.FindObject(m_CutOwner);
            auto* mesh = owner ? owner->FindMeshComponent(m_CutMesh) : nullptr;
            if (mesh)
            {
                const AffineTransform world = mesh->GetWorldTransform(owner->GetTransform());
                const float angle = m_CutAngle * XM_PI / 180.0f + (!m_RepeatCutPlane && m_AutoCutIndex % 2 ? XM_PIDIV2 : 0.0f);
                const Matrix3 rotation = Matrix3(Quaternion(Vector3(kZUnitVector),angle));
                valid = MeshCutting::TryBuildCutPlaneData({m_AutoCutWidth,1000.0f},
                    world * AffineTransform(rotation, Vector3(0,m_AutoCutY+m_CutStepY*m_AutoCutIndex,0)), plane);
                epsilon = float(Length(world.GetY())) * 1.0e-5f;
            }
        }
        else valid = m_CutPlanes.BuildData(m_ActiveCutPlane, plane);
        if (!valid) { m_AutoCuts=m_BenchmarkRemaining=0; Utility::Printf("Static cut plane rejected\n"); }
        const bool publish = !m_BenchmarkRemaining || m_BenchmarkRemaining == 1;
        if (valid)
        {
            m_LastCutNormal = Vector3(plane.WorldPlane);
            const bool accepted = m_CutScene ? m_Cutting.RequestAll(plane,m_CapMode) :
                m_Cutting.Request(m_CutOwner, m_CutMesh, plane, publish, epsilon, m_CapMode);
            if (!accepted)
            {
                Utility::Printf("Static cut request rejected: %s\n",m_Cutting.GetStatusText());
                m_AutoCuts = m_BenchmarkRemaining = 0;
            }
        }
    }

    // GPU 작업이 없었던 후보 없음·제출 거절도 최종 게시 수 검증을 거친다.
    if (!m_Cutting.IsBusy() && !m_AutoCuts && !m_BenchmarkRemaining && m_ExpectedPublications != ~0u)
    {
        if (m_Publications != m_ExpectedPublications)
            throw std::runtime_error("Unexpected number of cut publications");
        Utility::Printf("Static cut publication validation passed: count=%u\n",m_Publications);
        m_ExpectedPublications = ~0u;
    }

    // We use viewport offsets to jitter sample positions from frame to frame (for TAA.)
    // D3D has a design quirk with fractional offsets such that the implicit scissor
    // region of a viewport is floor(TopLeftXY) and floor(TopLeftXY + WidthHeight), so
    // having a negative fractional top left, e.g. (-0.25, -0.25) would also shift the
    // BottomRight corner up by a whole integer.  One solution is to pad your viewport
    // dimensions with an extra pixel.  My solution is to only use positive fractional offsets,
    // but that means that the average sample position is +0.5, which I use when I disable
    // temporal AA.
    TemporalEffects::GetJitterOffset(m_MainViewport.TopLeftX, m_MainViewport.TopLeftY);

    m_MainViewport.Width = (float)g_SceneColorBuffer.GetWidth();
    m_MainViewport.Height = (float)g_SceneColorBuffer.GetHeight();
    m_MainViewport.MinDepth = 0.0f;
    m_MainViewport.MaxDepth = 1.0f;

    m_MainScissor.left = 0;
    m_MainScissor.top = 0;
    m_MainScissor.right = (LONG)g_SceneColorBuffer.GetWidth();
    m_MainScissor.bottom = (LONG)g_SceneColorBuffer.GetHeight();
}

// solid 깊이 뒤 배경과 선택한 표시 모드를 그리고 평면·후처리를 수행한다.
// opt-in 진단은 장면 GPU 완료와 톤매핑 전 HDR 픽셀을 기록한다.
void ModelViewer::RenderScene( void )
{
    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

    uint32_t FrameIndex = TemporalEffects::GetFrameIndexMod2();
    const D3D12_VIEWPORT& viewport = m_MainViewport;
    const D3D12_RECT& scissor = m_MainScissor;

    ParticleEffectManager::Update(gfxContext.GetComputeContext(), Graphics::GetFrameTime());

    {
        // Update global constants
        float costheta = cosf(g_SunOrientation);
        float sintheta = sinf(g_SunOrientation);
        float cosphi = cosf(g_SunInclination * 3.14159f * 0.5f);
        float sinphi = sinf(g_SunInclination * 3.14159f * 0.5f);

        Vector3 SunDirection = Normalize(Vector3( costheta * cosphi, sinphi, sintheta * cosphi ));
        m_SunShadowCamera.UpdateMatrix(-SunDirection, Vector3(kZero), Vector3(50.0f),
            (uint32_t)g_ShadowBuffer.GetWidth(), (uint32_t)g_ShadowBuffer.GetHeight(), 16);

        GlobalConstants globals;
        globals.ViewProjMatrix = m_Camera.GetViewProjMatrix();
        globals.SunShadowMatrix = m_SunShadowCamera.GetShadowMatrix();
        globals.CameraPos = m_Camera.GetPosition();
        globals.SunDirection = SunDirection;
        globals.SunIntensity = Vector3(Scalar(g_SunLightIntensity));

        // Begin rendering depth
        gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
        gfxContext.ClearDepth(g_SceneDepthBuffer);

        MeshSorter sorter(MeshSorter::kDefault, m_RenderMode);
		sorter.SetCamera(m_Camera);
		sorter.SetViewport(viewport);
		sorter.SetScissor(scissor);
		sorter.SetDepthStencilTarget(g_SceneDepthBuffer);
		sorter.AddRenderTarget(g_SceneColorBuffer);

        m_Scene.Render(sorter);

        sorter.Sort();

        {
            ScopedTimer _prof(L"Depth Pre-Pass", gfxContext);
            sorter.RenderMeshes(MeshSorter::kZPass, gfxContext, globals);
        }

        SSAO::Render(gfxContext, m_Camera);

        if (!SSAO::DebugDraw)
        {
            ScopedTimer _outerprof(L"Main Render", gfxContext);

            {
                ScopedTimer _prof(L"Sun Shadow Map", gfxContext);

                MeshSorter shadowSorter(MeshSorter::kShadows);
				shadowSorter.SetCamera(m_SunShadowCamera);
				shadowSorter.SetDepthStencilTarget(g_ShadowBuffer);

                m_Scene.Render(shadowSorter);

                shadowSorter.Sort();
                shadowSorter.RenderMeshes(MeshSorter::kZPass, gfxContext, globals);
            }

            gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
            gfxContext.ClearColor(g_SceneColorBuffer);

            // 깊이를 쓰지 않는 선·점의 실루엣 가장자리를 배경이 덮지 않도록 먼저 그린다.
            Renderer::DrawSkybox(gfxContext, m_Camera, viewport, scissor);

            {
                ScopedTimer _prof(L"Render Color", gfxContext);

                gfxContext.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
                gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV(), g_SceneDepthBuffer.GetDSV_DepthReadOnly());
                gfxContext.SetViewportAndScissor(viewport, scissor);

                sorter.RenderMeshes(MeshSorter::kOpaque, gfxContext, globals);
            }

            sorter.RenderMeshes(MeshSorter::kTransparent, gfxContext, globals);
            const unsigned planeCount = Viewer::RenderCutPlanes(gfxContext, m_CutPlanes, m_SelectedObject,
                m_Camera, viewport, scissor);
            if (m_ViewDiagnosticPending)
                Utility::Printf("ModelViewer diagnostic: plane draw queued, planes=%u, lines=%u\n", planeCount, planeCount * 22);
        }
    }

    // Some systems generate a per-pixel velocity buffer to better track dynamic and skinned meshes.  Everything
    // is static in our scene, so we generate velocity from camera motion and the depth buffer.  A velocity buffer
    // is necessary for all temporal effects (and motion blur).
    MotionBlur::GenerateCameraVelocityBuffer(gfxContext, m_Camera, true);

    TemporalEffects::ResolveImage(gfxContext);

    ParticleEffectManager::Render(gfxContext, m_Camera, g_SceneColorBuffer, g_SceneDepthBuffer,  g_LinearDepth[FrameIndex]);

    // Until I work out how to couple these two, it's "either-or".
    if (DepthOfField::Enable)
        DepthOfField::Render(gfxContext, m_Camera.GetNearClip(), m_Camera.GetFarClip());
    else
        MotionBlur::RenderObjectBlur(gfxContext, g_VelocityBuffer);

    const bool capture = !m_CapturePath.empty() && !m_CaptureComplete && ++m_CaptureFrame >= m_CaptureTargetFrame
        && !m_AutoCuts && !m_BenchmarkRemaining && !m_Cutting.IsBusy();
    gfxContext.Finish(m_ViewDiagnosticPending || capture);
    if (capture)
    {
        m_CaptureComplete = true;
        g_SceneColorBuffer.ExportToFile(m_CapturePath + L"-hdr.raw");
        auto& restore = GraphicsContext::Begin(L"Restore capture state");
        restore.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        restore.Finish(true);
        m_CapturePostPending = true;
    }
    if (m_ViewDiagnosticPending)
    {
        Utility::Printf("ModelViewer diagnostic: scene GPU work completed, view=%u\n", static_cast<unsigned>(m_RenderMode));
        m_ViewDiagnosticPending = false;
    }
}

// 편집 안내와 절단 처리·거절 사유를 화면에 표시한다.
void ModelViewer::RenderUI(GraphicsContext& context)
{
    if (m_CapturePostPending)
    {
        g_SceneColorBuffer.ExportToFile(m_CapturePath + L"-post.raw");
        auto& restore = GraphicsContext::Begin(L"Restore capture state");
        restore.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        restore.Finish(true);
        m_CapturePostPending = false;
        Utility::Printf("ModelViewer diagnostic: HDR/post pixels captured, view=%u, typedUAV=%u\n", static_cast<unsigned>(m_RenderMode), g_bTypedUAVLoadSupport_R11G11B10_FLOAT);
    }
    m_Gizmo.Render(context, m_Scene, m_SelectedObject, m_Camera,
        float(g_OverlayBuffer.GetWidth()), float(g_OverlayBuffer.GetHeight()));
    TextContext text(context);
    text.Begin();
    text.SetTextSize(22.0f);
    text.ResetCursor(20.0f, 80.0f);
    text.DrawString("Left click: select object | Hold right mouse + WASD/QE: camera\n");
    text.DrawString("Q: View/select (hide gizmo) | W: Move | E: Rotate | R: Local axis/center scale\n");
    text.DrawString("Cut Plane: cyan grid, click inside to select | Scale X/Z or center XZ (Y fixed)\n");
    text.DrawFormattedString("C: cut static meshes in plane area | %s\n",m_Cutting.GetStatusText());
    if (m_CutGap > 0) text.DrawFormattedString("Diagnostic piece separation: %.3f\n",m_CutGap);
    text.DrawFormattedString("L: %s axes | Esc during drag: cancel | Mode: %s\n",
        m_Gizmo.IsLocal() ? "Local" : "World", m_Gizmo.GetMode() == Viewer::TransformGizmo::View ? "View" :
        (m_Gizmo.GetMode() == Viewer::TransformGizmo::Translate ? "Move" :
        (m_Gizmo.GetMode() == Viewer::TransformGizmo::Rotate ? "Rotate" : "Scale")));
    text.DrawFormattedString("V: display = %s (Solid -> Wireframe -> Vertices)\n",
        m_RenderMode == Renderer::RenderMode::Solid ? "Solid" :
        (m_RenderMode == Renderer::RenderMode::Wireframe ? "Wireframe" : "Vertices"));
    text.DrawString("Ctrl+Z: undo last completed drag (128 commands)\n");
    const Viewer::GameObject* selected = m_Scene.FindObject(m_SelectedObject);
    if (selected)
        text.DrawFormattedString("Selected: %s (ID %llu)\n", selected->GetName().c_str(),
            static_cast<unsigned long long>(selected->GetId()));
    else
        text.DrawString("Selected: none\n");
    text.End();
}
