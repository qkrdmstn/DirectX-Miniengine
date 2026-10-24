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
    Viewer::CutPlaneSystem m_CutPlanes{m_Scene};
    Viewer::ObjectId m_SelectedObject = Viewer::InvalidObjectId;
    Viewer::TransformGizmo m_Gizmo;
    bool m_EscapeConsumed = false;
    Renderer::RenderMode m_RenderMode = Renderer::RenderMode::Solid;
    bool m_ViewDiagnosticPending = false;
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

// 장면과 렌더러를 초기화하고 진단용 -view 값으로 시작 표시 모드를 선택한다.
void ModelViewer::Startup( void )
{
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
    }
    else
    {
        m_Scene.CreateObject("Model", Renderer::LoadModel(gltfFileName, forceRebuild)).LoopAllAnimations();
    }

    size_t index = 0;
    for (const auto& object : m_Scene.GetObjects())
    {
        const ModelInstance* model = object->GetModelInstance();
        if (!model || float(model->GetRadius()) <= 0.0f)
            continue;
        const float scale = 2.0f / float(model->GetRadius());
        const float x = m_Scene.GetObjects().size() == 1 ? 0.0f : (index++ == 0 ? -3.0f : 3.0f);
        object->SetTransform(UniformTransform(Quaternion(kIdentity), scale,
            Vector3(x, 0.0f, 0.0f) - model->GetCenter() * scale));
    }
    m_CutPlanes.CreatePlane({4.0f, 4.0f}, Viewer::SceneTransform(kIdentity));
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
    m_Scene.Clear();
    m_CutPlanes.PruneStaleObjects();

    g_IBLTextures.clear();

    Renderer::Shutdown();
}

namespace Graphics
{
    extern EnumVar DebugZoom;
}

// 입력·장면을 갱신하고 V 첫 입력마다 메인 컬러 표시 모드를 순환한다.
void ModelViewer::Update( float deltaT )
{
    ScopedTimer _prof(L"Update State");

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
        m_SelectedObject = Viewer::NearestPickHit(Viewer::PickObject(m_Scene, ray), m_CutPlanes.Pick(ray)).Id;
    }

    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");

    m_Scene.Update(gfxContext, deltaT);

    gfxContext.Finish();

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

// 메인 정렬기에 표시 모드를 전달하고 기존 깊이·그림자·후처리 순서를 유지한다.
// 선택한 모드로 장면을 그리고 -view 진단의 첫 프레임만 GPU 완료를 기다려 기록한다.
// 모델 뒤에 평면 선을 깊이 검사로 그린 뒤 기존 후처리를 수행한다.
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

            {
                ScopedTimer _prof(L"Render Color", gfxContext);

                gfxContext.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
                gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV(), g_SceneDepthBuffer.GetDSV_DepthReadOnly());
                gfxContext.SetViewportAndScissor(viewport, scissor);

                sorter.RenderMeshes(MeshSorter::kOpaque, gfxContext, globals);
            }

            Renderer::DrawSkybox(gfxContext, m_Camera, viewport, scissor);

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

    gfxContext.Finish(m_ViewDiagnosticPending);
    if (m_ViewDiagnosticPending)
    {
        Utility::Printf("ModelViewer diagnostic: scene GPU work completed, view=%u\n", static_cast<unsigned>(m_RenderMode));
        m_ViewDiagnosticPending = false;
    }
}

// 기즈모와 조작 안내 및 현재 메시 표시 모드를 overlay에 그린다.
void ModelViewer::RenderUI(GraphicsContext& context)
{
    m_Gizmo.Render(context, m_Scene, m_SelectedObject, m_Camera,
        float(g_OverlayBuffer.GetWidth()), float(g_OverlayBuffer.GetHeight()));
    TextContext text(context);
    text.Begin();
    text.SetTextSize(22.0f);
    text.ResetCursor(20.0f, 80.0f);
    text.DrawString("Left click: select object | Hold right mouse + WASD/QE: camera\n");
    text.DrawString("Q: View/select (hide gizmo) | W: Move | E: Rotate | R: Local axis/center scale\n");
    text.DrawString("Cut Plane: cyan grid, click inside to select | Scale X/Z or center XZ (Y fixed)\n");
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
