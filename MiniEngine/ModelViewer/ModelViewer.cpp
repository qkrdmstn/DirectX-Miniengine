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
#include "Utility.h"
#include "../MeshCutting/Debug/CutDebugView.h"
#include "../MeshCutting/Rendering/ProceduralMeshRenderer.h"
#include "../MeshCutting/Runtime/CutController.h"

#define LEGACY_RENDERER

using namespace GameCore;
using namespace Math;
using namespace Graphics;
using namespace std;

using Renderer::MeshSorter;

class ModelViewer : public GameCore::IGameApp
{
public:

    ModelViewer(void) {}

    virtual void Startup(void) override;
    virtual void Cleanup(void) override;

    virtual void Update(float deltaT) override;
    virtual void RenderScene(void) override;
    virtual void RenderUI(GraphicsContext& context) override;

private:

    Camera m_Camera;
    unique_ptr<CameraController> m_CameraController;

    D3D12_VIEWPORT m_MainViewport;
    D3D12_RECT m_MainScissor;

    ModelInstance m_ModelInst;
    struct BoneSelection { uint32_t paletteId = 0; uint32_t jointIndex = 0; } m_BoneSelection;
    bool m_BoneDebugEnabled = false;
    bool m_WeightDebugEnabled = false;
    bool m_WireframeEnabled = false;
    bool m_AnimationsPaused = false;
    vector<AnimationState::eMode> m_ResumeModes;
    MeshCutting::ProceduralMeshRenderer m_ProceduralRenderer;
    MeshCutting::CutController m_Cut;           // patch, plane, cut pieces and their per-frame skinning
    MeshCutting::CutDebugView m_CutView;        // debug meshes, display toggles and status lines
    ShadowCamera m_SunShadowCamera;

    void RenderProceduralMeshes(GraphicsContext& gfxContext, const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissor, uint32_t frameIndex);
};

CREATE_APPLICATION(ModelViewer)

static std::wstring BoneName(const Model& model, uint32_t nodeIndex, uint32_t jointIndex)
{
    if (nodeIndex < model.m_NodeNames.size() && !model.m_NodeNames[nodeIndex].empty())
        return Utility::UTF8ToWideString(model.m_NodeNames[nodeIndex]);
    return L"Joint_" + std::to_wstring(jointIndex);
}

static std::wstring ParentName(const Model& model, uint32_t nodeIndex)
{
    if (nodeIndex >= model.m_NodeNames.size())
        return L"None";
    if (!model.m_NodeNames[nodeIndex].empty())
        return Utility::UTF8ToWideString(model.m_NodeNames[nodeIndex]);
    return L"Node_" + std::to_wstring(nodeIndex);
}

ExpVar g_SunLightIntensity("Viewer/Lighting/Sun Light Intensity", 4.0f, 0.0f, 16.0f, 0.1f);
NumVar g_SunOrientation("Viewer/Lighting/Sun Orientation", -0.5f, -100.0f, 100.0f, 0.1f);
NumVar g_SunInclination("Viewer/Lighting/Sun Inclination", 0.75f, 0.0f, 1.0f, 0.01f);

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
    } while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);

    Utility::Printf("Found %u IBL environment map sets\n", g_IBLTextures.size());

    if (g_IBLTextures.size() > 0)
        g_IBLSet.Increment();
}

void ModelViewer::Startup(void)
{
    // MotionBlur::Enable = true;      // off: motion blur (the engine default is already false)
    TemporalEffects::EnableTAA = true;
    FXAA::Enable = false;
    PostEffects::EnableHDR = true;
    // PostEffects::EnableAdaptation = true;
    PostEffects::EnableAdaptation = false;  // adaptive exposure made the screen brighten and darken slightly
    SSAO::Enable = true;

    Renderer::Initialize();
    m_ProceduralRenderer.Initialize();

    LoadIBLTextures();

    std::wstring gltfFileName = L"Models/CesiumMan.glb";

    bool forceRebuild = false;
    uint32_t rebuildValue;
    if (CommandLineArgs::GetInteger(L"rebuild", rebuildValue))
        forceRebuild = rebuildValue != 0;

    if (gltfFileName.empty())
    {
#ifdef LEGACY_RENDERER
        Sponza::Startup(m_Camera);
#else
        m_ModelInst = Renderer::LoadModel(L"Sponza/PBR/sponza2.gltf", forceRebuild);
        m_ModelInst.Resize(100.0f * m_ModelInst.GetRadius());
        OrientedBox obb = m_ModelInst.GetBoundingBox();
        float modelRadius = Length(obb.GetDimensions()) * 0.5f;
        const Vector3 eye = obb.GetCenter() + Vector3(modelRadius * 0.5f, 0.0f, 0.0f);
        m_Camera.SetEyeAtUp(eye, Vector3(kZero), Vector3(kYUnitVector));
#endif
    }
    else
    {
        m_ModelInst = Renderer::LoadModel(gltfFileName, forceRebuild);
        m_ModelInst.LoopAllAnimations();
        m_ModelInst.Resize(10.0f);

        const Model* model = m_ModelInst.GetModel();
        if (model != nullptr)
        {
            for (uint32_t paletteId = 0; paletteId < model->m_SkinPalettes.size(); ++paletteId)
            {
                const SkinPalette& palette = model->m_SkinPalettes[paletteId];
                if (palette.numJoints > 0 && !m_BoneDebugEnabled)
                    m_BoneSelection.paletteId = paletteId;
                m_BoneDebugEnabled |= palette.numJoints > 0;
                for (uint32_t jointIndex = 0; jointIndex < palette.numJoints; ++jointIndex)
                {
                    const uint32_t nodeIndex = model->m_JointIndices[palette.startJoint + jointIndex];
                    const uint32_t parent = nodeIndex < model->m_NodeParents.size()
                        ? model->m_NodeParents[nodeIndex] : UINT32_MAX;
                    const std::wstring name = BoneName(*model, nodeIndex, jointIndex);
                    const std::wstring parentName = ParentName(*model, parent);
                    Utility::Printf(L"Skin %u Joint %u Node %u %ls Parent %u %ls\n",
                        paletteId, jointIndex, nodeIndex, name.c_str(), parent, parentName.c_str());
                }
            }
        }

        MotionBlur::Enable = false;
    }

    m_Camera.SetZRange(1.0f, 10000.0f);
    if (gltfFileName.size() == 0)
        m_CameraController.reset(new FlyingFPSCamera(m_Camera, Vector3(kYUnitVector)));
    else
        m_CameraController.reset(new OrbitCamera(m_Camera, m_ModelInst.GetBoundingSphere(), Vector3(kYUnitVector)));
}

void ModelViewer::Cleanup(void)
{
    m_CutView.Release(m_ProceduralRenderer);
    m_ProceduralRenderer.Shutdown();
    m_ModelInst = nullptr;

    g_IBLTextures.clear();

#ifdef LEGACY_RENDERER
    Sponza::Cleanup();
#endif

    Renderer::Shutdown();
}

namespace Graphics
{
    extern EnumVar DebugZoom;
}

void ModelViewer::Update(float deltaT)
{
    ScopedTimer _prof(L"Update State");


    const Model* model = m_ModelInst.GetModel();
    if (model != nullptr && m_BoneDebugEnabled && m_BoneSelection.paletteId < model->m_SkinPalettes.size())
    {
        const SkinPalette& palette = model->m_SkinPalettes[m_BoneSelection.paletteId];
        if (GameInput::IsFirstPressed(GameInput::kKey_b))
            m_BoneDebugEnabled = false;
        if (GameInput::IsFirstPressed(GameInput::kKey_h))
            m_WeightDebugEnabled = !m_WeightDebugEnabled;
        if (GameInput::IsFirstPressed(GameInput::kKey_v))
            m_WireframeEnabled = !m_WireframeEnabled;
        if (palette.numJoints > 0)
        {
            if (GameInput::IsFirstPressed(GameInput::kKey_j))
                m_BoneSelection.jointIndex = (m_BoneSelection.jointIndex + palette.numJoints - 1) % palette.numJoints;
            if (GameInput::IsFirstPressed(GameInput::kKey_k))
                m_BoneSelection.jointIndex = (m_BoneSelection.jointIndex + 1) % palette.numJoints;
        }
        if (GameInput::IsFirstPressed(GameInput::kKey_i))
        {
            for (uint32_t step = 1; step <= model->m_SkinPalettes.size(); ++step)
            {
                uint32_t next = (m_BoneSelection.paletteId + step) % (uint32_t)model->m_SkinPalettes.size();
                if (model->m_SkinPalettes[next].numJoints > 0)
                {
                    m_BoneSelection.paletteId = next;
                    m_BoneSelection.jointIndex = 0;
                    break;
                }
            }
        }
        if (GameInput::IsFirstPressed(GameInput::kKey_c))
            m_Cut.SetPatch(*model, m_ModelInst, m_BoneSelection.paletteId, m_BoneSelection.jointIndex);
        if (GameInput::IsFirstPressed(GameInput::kKey_x))
            m_Cut.ClearPatch();
        if (GameInput::IsFirstPressed(GameInput::kKey_g))
            m_CutView.ToggleBoundary();
        if (GameInput::IsFirstPressed(GameInput::kKey_n))
            m_CutView.ToggleOverlay();
        if (GameInput::IsFirstPressed(GameInput::kKey_u))
            m_CutView.CycleWireMode();

        {
            if (GameInput::IsFirstPressed(GameInput::kKey_t))
                m_Cut.ToggleEditPlane(m_ModelInst, m_BoneSelection.paletteId);
            if (GameInput::IsFirstPressed(GameInput::kKey_m))
                m_Cut.ToggleCap();
            if (GameInput::IsFirstPressed(GameInput::kKey_l))
                m_CutView.ToggleBackCull();
            if (GameInput::IsFirstPressed(GameInput::kKey_z))
                m_CutView.ToggleSectionTint();
            if (GameInput::IsFirstPressed(GameInput::kKey_back))
                m_Cut.DiscardCut();
            // The plane keys and Enter only act while the plane is being edited.
            if (m_Cut.PlaneEditing())
            {
                if (GameInput::IsFirstPressed(GameInput::kKey_r))
                    m_Cut.ResetPlane(m_ModelInst);
                if (GameInput::IsFirstPressed(GameInput::kKey_return))
                    m_Cut.RequestCut();
                if (GameInput::IsFirstPressed(GameInput::kKey_tab))
                    m_Cut.CycleWeightJoint();

                // Move: model radius per second. Turn: radians per second. Scale: e-fold per second.
                const float move = m_Cut.ModelRadius() * deltaT, turn = 1.0f * deltaT;
                const bool shift = GameInput::IsPressed(GameInput::kKey_lshift) || GameInput::IsPressed(GameInput::kKey_rshift);
                const Vector3 up = m_Camera.GetUpVec(), right = m_Camera.GetRightVec();
                if (GameInput::IsPressed(GameInput::kKey_home))
                    m_Cut.TranslatePlane(up * move);
                if (GameInput::IsPressed(GameInput::kKey_end))
                    m_Cut.TranslatePlane(up * -move);
                if (GameInput::IsPressed(GameInput::kKey_delete))
                    m_Cut.TranslatePlane(right * -move);
                if (GameInput::IsPressed(GameInput::kKey_pgdn))
                    m_Cut.TranslatePlane(right * move);
                if (GameInput::IsPressed(GameInput::kKey_lbracket))
                    m_Cut.MovePlaneAlongNormal(-move);
                if (GameInput::IsPressed(GameInput::kKey_rbracket))
                    m_Cut.MovePlaneAlongNormal(move);
                if (GameInput::IsPressed(GameInput::kKey_comma))
                    m_Cut.RotatePlane(0, -turn);
                if (GameInput::IsPressed(GameInput::kKey_period))
                    m_Cut.RotatePlane(0, turn);
                if (GameInput::IsPressed(GameInput::kKey_semicolon))
                    m_Cut.RotatePlane(1, -turn);
                if (GameInput::IsPressed(GameInput::kKey_apostrophe))
                    m_Cut.RotatePlane(1, turn);
                if (GameInput::IsPressed(GameInput::kKey_insert))
                    m_Cut.RotatePlane(2, -turn);
                if (GameInput::IsPressed(GameInput::kKey_pgup))
                    m_Cut.RotatePlane(2, turn);
                if (GameInput::IsPressed(GameInput::kKey_minus))
                    m_Cut.ScalePlane(expf(-deltaT), shift);
                if (GameInput::IsPressed(GameInput::kKey_equals))
                    m_Cut.ScalePlane(expf(deltaT), shift);
            }
            // The preview and the cut itself run in CutController::Update, after this frame's pose is known.
        }
    }
    else if (model != nullptr && m_BoneSelection.paletteId < model->m_SkinPalettes.size()
        && model->m_SkinPalettes[m_BoneSelection.paletteId].numJoints > 0
        && GameInput::IsFirstPressed(GameInput::kKey_b))
        m_BoneDebugEnabled = true;

    if (GameInput::IsFirstPressed(GameInput::kKey_space) && m_ModelInst.GetNumAnimations() > 0)
    {
        if (!m_AnimationsPaused)
        {
            m_ResumeModes.resize(m_ModelInst.GetNumAnimations(), AnimationState::kStopped);
            bool anyPlaying = false;
            for (uint32_t i = 0; i < m_ResumeModes.size(); ++i)
            {
                m_ResumeModes[i] = m_ModelInst.GetAnimationMode(i);
                if (m_ResumeModes[i] != AnimationState::kStopped)
                {
                    m_ModelInst.PauseAnimation(i);
                    anyPlaying = true;
                }
            }
            m_AnimationsPaused = anyPlaying;
        }
        else
        {
            for (uint32_t i = 0; i < m_ResumeModes.size(); ++i)
            {
                if (m_ResumeModes[i] != AnimationState::kStopped)
                    m_ModelInst.PlayAnimation(i, m_ResumeModes[i] == AnimationState::kLooping);
            }
            m_ResumeModes.clear();
            m_AnimationsPaused = false;
        }
    }

    if (GameInput::IsFirstPressed(GameInput::kLShoulder))
        DebugZoom.Decrement();
    else if (GameInput::IsFirstPressed(GameInput::kRShoulder))
        DebugZoom.Increment();

    m_CameraController->Update(deltaT);

    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");

    m_ModelInst.Update(gfxContext, deltaT);

    gfxContext.Finish();

    // The CPU pose of this frame is final now; bring the patch and the cut pieces to it.
    if (const Model* poseModel = m_ModelInst.GetModel())
        m_Cut.Update(*poseModel, m_ModelInst);


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

void ModelViewer::RenderScene(void)
{
    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");
    const Model* model = m_ModelInst.GetModel();

    uint32_t FrameIndex = TemporalEffects::GetFrameIndexMod2();
    const D3D12_VIEWPORT& viewport = m_MainViewport;
    const D3D12_RECT& scissor = m_MainScissor;

    ParticleEffectManager::Update(gfxContext.GetComputeContext(), Graphics::GetFrameTime());

    if (m_ModelInst.IsNull())
    {
#ifdef LEGACY_RENDERER
        Sponza::RenderScene(gfxContext, m_Camera, viewport, scissor);
#endif
        if (!SSAO::DebugDraw)
            RenderProceduralMeshes(gfxContext, viewport, scissor, FrameIndex);
    }
    else
    {
        // Update global constants
        float costheta = cosf(g_SunOrientation);
        float sintheta = sinf(g_SunOrientation);
        float cosphi = cosf(g_SunInclination * 3.14159f * 0.5f);
        float sinphi = sinf(g_SunInclination * 3.14159f * 0.5f);

        Vector3 SunDirection = Normalize(Vector3(costheta * cosphi, sinphi, sintheta * cosphi));
        Vector3 ShadowBounds = Vector3(m_ModelInst.GetRadius());
        //m_SunShadowCamera.UpdateMatrix(-SunDirection, m_ModelInst.GetCenter(), ShadowBounds,
        m_SunShadowCamera.UpdateMatrix(-SunDirection, Vector3(0, -500.0f, 0), Vector3(5000, 3000, 3000),
            (uint32_t)g_ShadowBuffer.GetWidth(), (uint32_t)g_ShadowBuffer.GetHeight(), 16);

        GlobalConstants globals = {};
        globals.ViewProjMatrix = m_Camera.GetViewProjMatrix();
        globals.SunShadowMatrix = m_SunShadowCamera.GetShadowMatrix();
        globals.CameraPos = m_Camera.GetPosition();
        globals.SunDirection = SunDirection;
        globals.SunIntensity = Vector3(Scalar(g_SunLightIntensity));
        globals.DebugJointIndex = UINT32_MAX;

        // Begin rendering depth
        gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
        gfxContext.ClearDepth(g_SceneDepthBuffer);

        MeshSorter sorter(MeshSorter::kDefault);
        sorter.SetCamera(m_Camera);
        sorter.SetViewport(viewport);
        sorter.SetScissor(scissor);
        if (m_BoneDebugEnabled && model != nullptr
            && m_BoneSelection.paletteId < model->m_SkinPalettes.size())
        {
            // In cut mode the weight view shows a joint the plane cuts (Tab picks which); otherwise J/K's joint.
            uint32_t weightPalette = m_BoneSelection.paletteId, weightJoint = m_BoneSelection.jointIndex;
            const uint32_t cutJoint = m_Cut.WeightJoint();
            if (cutJoint != MeshCutting::kNoNode && m_Cut.EditPalette() < model->m_SkinPalettes.size())
            {
                weightPalette = m_Cut.EditPalette();
                weightJoint = cutJoint;
            }
            const SkinPalette& palette = model->m_SkinPalettes[weightPalette];
            sorter.SetBoneDebug(m_WeightDebugEnabled, palette.startJoint, weightJoint);
            sorter.SetWireframe(m_WireframeEnabled);
        }
        sorter.SetDepthStencilTarget(g_SceneDepthBuffer);
        sorter.AddRenderTarget(g_SceneColorBuffer);

        m_ModelInst.Render(sorter);

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

                m_ModelInst.Render(shadowSorter);

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

            //Renderer::DrawSkybox(gfxContext, m_Camera, viewport, scissor);

            // Procedural meshes are opaque, so they must land before transparent blending.
            RenderProceduralMeshes(gfxContext, viewport, scissor, FrameIndex);

            sorter.RenderMeshes(MeshSorter::kTransparent, gfxContext, globals);
        }
    }

    // Some systems generate a per-pixel velocity buffer to better track dynamic and skinned meshes.  Everything
    // is static in our scene, so we generate velocity from camera motion and the depth buffer.  A velocity buffer
    // is necessary for all temporal effects (and motion blur).
    MotionBlur::GenerateCameraVelocityBuffer(gfxContext, m_Camera, true);

    TemporalEffects::ResolveImage(gfxContext);

    ParticleEffectManager::Render(gfxContext, m_Camera, g_SceneColorBuffer, g_SceneDepthBuffer, g_LinearDepth[FrameIndex]);

    // Until I work out how to couple these two, it's "either-or".
    if (DepthOfField::Enable)
        DepthOfField::Render(gfxContext, m_Camera.GetNearClip(), m_Camera.GetFarClip());
    else
        MotionBlur::RenderObjectBlur(gfxContext, g_VelocityBuffer);

    m_ProceduralRenderer.NotifySubmitted(gfxContext.Finish());
}

void ModelViewer::RenderProceduralMeshes(GraphicsContext& gfxContext, const D3D12_VIEWPORT& viewport,
    const D3D12_RECT& scissor, uint32_t frameIndex)
{
    m_CutView.Draw(gfxContext, m_ProceduralRenderer, m_Cut, m_Camera.GetViewProjMatrix(),
        m_ModelInst.IsNull() ? 0.0f : float(m_ModelInst.GetRadius()), viewport, scissor);

    // SSAO linearized depth before these draws; refresh it so velocity, TAA and DoF see the new depth.
    if (SSAO::Enable || SSAO::ComputeLinearZ)
        SSAO::LinearizeZ(gfxContext.GetComputeContext(), m_Camera, frameIndex);
}

void ModelViewer::RenderUI(GraphicsContext& context)
{
    const Model* model = m_ModelInst.GetModel();
    if (!m_BoneDebugEnabled || model == nullptr || m_BoneSelection.paletteId >= model->m_SkinPalettes.size())
        return;

    const SkinPalette& palette = model->m_SkinPalettes[m_BoneSelection.paletteId];
    if (m_BoneSelection.jointIndex >= palette.numJoints)
        return;

    const uint32_t nodeIndex = model->m_JointIndices[palette.startJoint + m_BoneSelection.jointIndex];
    const uint32_t parent = nodeIndex < model->m_NodeParents.size()
        ? model->m_NodeParents[nodeIndex] : UINT32_MAX;
    const std::wstring name = BoneName(*model, nodeIndex, m_BoneSelection.jointIndex);
    const std::wstring parentName = ParentName(*model, parent);

    TextContext text(context);
    text.Begin();
    text.ResetCursor(20.0f, 70.0f);
    text.DrawFormattedString(L"Skin %u/%u  Joint %u/%u  Node %u  %ls\n",
        m_BoneSelection.paletteId, (uint32_t)model->m_SkinPalettes.size(),
        m_BoneSelection.jointIndex, palette.numJoints, nodeIndex, name.c_str());
    text.DrawFormattedString(L"Parent %u  %ls\n", parent, parentName.c_str());
    text.DrawFormattedString(L"B: debug  J/K: joint  I: skin  H: weight %ls  V: wire %ls\n",
        m_WeightDebugEnabled ? L"on" : L"off", m_WireframeEnabled ? L"on" : L"off");
    const uint32_t cutPalette = m_Cut.EditPalette();
    m_CutView.DrawUI(text, m_Cut, [model, cutPalette](uint32_t joint) -> std::wstring
        {
            if (cutPalette >= model->m_SkinPalettes.size() || joint >= model->m_SkinPalettes[cutPalette].numJoints)
                return L"?";
            const SkinPalette& cutSkin = model->m_SkinPalettes[cutPalette];
            return BoneName(*model, model->m_JointIndices[cutSkin.startJoint + joint], joint);
        });
    text.End();
}
