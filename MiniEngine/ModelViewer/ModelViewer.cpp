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
#include "../MeshCutting/ProceduralMesh.h"
#include "../MeshCutting/BonePatch.h"
#include "../MeshCutting/Geometry/PlaneSlicer.h"
#include "../MeshCutting/Geometry/RenderBake.h"
#include "../MeshCutting/Rendering/ProceduralMeshRenderer.h"

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
    MeshCutting::ProceduralMesh m_BonePatch;
    MeshCutting::BonePatchSource m_BonePatchSource;
    MeshCutting::ProceduralMeshRenderer m_ProceduralRenderer;
    std::wstring m_BonePatchStatus;
    // Welded copy of the patch: the slicer input. Boundary edges are drawn as colored tubes.
    MeshCutting::PatchPiece m_PatchPiece;
    MeshCutting::WeldStats m_WeldStats;
    MeshCutting::SliceStatus m_PieceStatus = MeshCutting::SliceStatus::InvalidInput;
    std::string m_PieceDetail;
    MeshCutting::ProceduralMesh m_BoundaryDebug;
    bool m_BoundaryDebugEnabled = true;

    // Slice preview. The base plane sits at the selected joint and points to its child joint;
    // offset and tilt are added on top. The pieces on screen come from the last Ok slice.
    bool m_SliceEnabled = false;
    bool m_SliceDirty = false;
    bool m_CapEnabled = true;               // M: CapMode::Create / None
    bool m_BackCull = false;                // L: back-face culling PSO instead of two-sided
    bool m_SectionTint = false;             // Z: color surface and cap sections differently
    bool m_PlaneBaseValid = false;
    // The plane frame is stored in the selected joint's local space (copy time) and carried to the
    // world with the joint's current matrix every frame, so it follows the bone, twist included.
    Vector3 m_PlaneLocalPoint, m_PlaneLocalNormal, m_PlaneLocalAxisA, m_PlaneLocalAxisB;
    Vector3 m_PlanePoint0, m_PlaneNormal0, m_PlaneAxisA, m_PlaneAxisB;    // world frame of this frame
    float m_PlaneOffset = 0.0f;
    float m_PlaneTiltA = 0.0f;
    float m_PlaneTiltB = 0.0f;
    MeshCutting::SliceStatus m_SliceStatus = MeshCutting::SliceStatus::InvalidInput;
    std::string m_SliceDetail;
    std::wstring m_SliceInfo;
    bool m_HavePieces = false;
    DirectX::XMFLOAT3 m_ShownNormal = { 0.0f, 1.0f, 0.0f };
    MeshCutting::PatchPiece m_SlicedPositive, m_SlicedNegative;     // the pieces on screen, skinned every frame
    MeshCutting::ProceduralMesh m_PiecePositive, m_PieceNegative;
    MeshCutting::ProceduralMesh m_PieceBoundaryPositive, m_PieceBoundaryNegative;
    MeshCutting::ProceduralMesh m_PlaneDebug;

    // Animation follow-up: after ModelInstance::Update the patch and the cut pieces are skinned to the
    // same pose the GPU draws. Only positions change; topology, UVs and normals stay as they were.
    bool m_OverlayEnabled = false;          // N: draw exactly on the character (no display offset, no gap)
    std::vector<std::vector<DirectX::XMFLOAT4X4>> m_SkinMatrices;
    std::vector<Matrix4> m_CurrentPoseJointWorld;
    float m_PatchRadius = 0.0f;             // bounds radius when the patch was copied (keeps debug sizes stable)
    uint32_t m_SkinnedNodes = 0;            // nodes skinned in the last frame (patch + pieces)
    float m_SkinMs = 0.0f;                  // smoothed CPU time of the SkinPatchPiece calls
    std::string m_SkinError;
    ShadowCamera m_SunShadowCamera;

    void RebuildPatchPiece(void);
    void UpdatePatchPose(void);
    void ComputeBasePlane(const Model& model);
    MeshCutting::SlicePlane CurrentPlane(void) const;
    bool UpdateSlice(void);         // true when new pieces replaced the old ones
    void UpdateBasePlane(void);
    void ClearSlice(void);
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
    MotionBlur::Enable = true;
    TemporalEffects::EnableTAA = true;
    FXAA::Enable = false;
    PostEffects::EnableHDR = true;
    PostEffects::EnableAdaptation = true;
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
    m_ProceduralRenderer.ReleaseMesh(m_BonePatch);
    m_ProceduralRenderer.ReleaseMesh(m_BoundaryDebug);
    m_ProceduralRenderer.ReleaseMesh(m_PiecePositive);
    m_ProceduralRenderer.ReleaseMesh(m_PieceNegative);
    m_ProceduralRenderer.ReleaseMesh(m_PieceBoundaryPositive);
    m_ProceduralRenderer.ReleaseMesh(m_PieceBoundaryNegative);
    m_ProceduralRenderer.ReleaseMesh(m_PlaneDebug);
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
        {
            // Copies with the pose of the last ModelInstance::Update, i.e. what is on screen now.
            MeshCutting::ProceduralMeshData patch;
            MeshCutting::BonePatchSource source;
            std::string error;
            if (MeshCutting::ExtractBonePatch(*model, m_ModelInst, m_BoneSelection.paletteId,
                    m_BoneSelection.jointIndex, 0.0f, patch, source, &error)
                && m_BonePatch.SetMesh(std::move(patch), &error))
            {
                m_BonePatchSource = std::move(source);
                m_BonePatchStatus = L"Patch: " + std::to_wstring(m_BonePatch.GetData().indices.size() / 3) + L" tris, "
                    + std::to_wstring(m_BonePatch.GetData().vertices.size()) + L" verts, mixed outside "
                    + std::to_wstring(m_BonePatchSource.mixedOutsideTriangles);
                RebuildPatchPiece();
                ClearSlice();
                m_CurrentPoseJointWorld = m_BonePatchSource.poseJointWorld;
                m_PlaneOffset = m_PlaneTiltA = m_PlaneTiltB = 0.0f;
                ComputeBasePlane(*model);
                m_SliceDirty = m_SliceEnabled;      // a replaced patch is sliced again at once
            }
            else
                m_BonePatchStatus = L"Patch failed: " + Utility::UTF8ToWideString(error);
        }
        if (GameInput::IsFirstPressed(GameInput::kKey_x))
        {
            m_BonePatch.Clear();
            m_BonePatchSource = {};
            m_BonePatchStatus.clear();
            RebuildPatchPiece();
            ClearSlice();
            m_PlaneBaseValid = false;
            m_SliceEnabled = false;
        }
        if (GameInput::IsFirstPressed(GameInput::kKey_g))
            m_BoundaryDebugEnabled = !m_BoundaryDebugEnabled;
        if (GameInput::IsFirstPressed(GameInput::kKey_n))
            m_OverlayEnabled = !m_OverlayEnabled;

        if (m_PlaneBaseValid)
        {
            const float radius = m_PatchRadius;
            if (GameInput::IsFirstPressed(GameInput::kKey_t))
            {
                m_SliceEnabled = !m_SliceEnabled;
                m_SliceDirty = true;
            }
            if (GameInput::IsFirstPressed(GameInput::kKey_m))
            {
                m_CapEnabled = !m_CapEnabled;
                m_SliceDirty = true;
            }
            if (GameInput::IsFirstPressed(GameInput::kKey_l))
                m_BackCull = !m_BackCull;
            if (GameInput::IsFirstPressed(GameInput::kKey_z))
                m_SectionTint = !m_SectionTint;
            if (GameInput::IsFirstPressed(GameInput::kKey_r))
            {
                m_PlaneOffset = m_PlaneTiltA = m_PlaneTiltB = 0.0f;
                m_SliceDirty = true;
            }
            const float move = radius * 0.4f * deltaT, turn = 0.8f * deltaT;
            const auto hold = [&](GameInput::DigitalInput key, float& value, float amount)
            {
                if (GameInput::IsPressed(key))
                {
                    value += amount;
                    m_SliceDirty = true;
                }
            };
            hold(GameInput::kKey_lbracket, m_PlaneOffset, -move);
            hold(GameInput::kKey_rbracket, m_PlaneOffset, move);
            hold(GameInput::kKey_comma, m_PlaneTiltA, -turn);
            hold(GameInput::kKey_period, m_PlaneTiltA, turn);
            hold(GameInput::kKey_semicolon, m_PlaneTiltB, -turn);
            hold(GameInput::kKey_apostrophe, m_PlaneTiltB, turn);
            // The slice itself runs in UpdatePatchPose, after this frame's pose is known.
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
    UpdatePatchPose();


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
            const SkinPalette& palette = model->m_SkinPalettes[m_BoneSelection.paletteId];
            sorter.SetBoneDebug(m_WeightDebugEnabled, palette.startJoint, m_BoneSelection.jointIndex);
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

// Welds the current patch into the slicer input and refreshes the boundary tubes. An empty source
// (after X) just clears everything.
void ModelViewer::RebuildPatchPiece(void)
{
    m_PatchPiece.Clear();
    m_WeldStats = MeshCutting::WeldStats();
    m_PieceDetail.clear();
    m_PieceStatus = MeshCutting::SliceStatus::InvalidInput;
    m_BoundaryDebug.Clear();

    if (m_BonePatch.GetData().indices.empty())
    {
        m_PieceDetail = "no patch";
        return;
    }

    MeshCutting::WeldInput weldInput;
    std::string error;
    if (!MeshCutting::MakeWeldInput(m_BonePatch.GetData(), m_BonePatchSource, weldInput, &error))
    {
        m_PieceDetail = error;
        return;
    }
    m_PieceStatus = MeshCutting::BuildPatchPiece(weldInput, MeshCutting::kDefaultEpsilonScale,
        m_PatchPiece, m_WeldStats, &m_PieceDetail);
    if (m_PieceStatus != MeshCutting::SliceStatus::Ok)
        return;

    m_PatchRadius = float(MeshCutting::ComputePieceRadius(m_PatchPiece));
    MeshCutting::ProceduralMeshData tubes;
    MeshCutting::BakeBoundaryDebug(m_PatchPiece, m_PatchRadius * 0.008f, tubes);
    if (!tubes.indices.empty())
        m_BoundaryDebug.SetMesh(std::move(tubes), &error);
}

// Puts new vertex data into a mesh: positions only when the topology is unchanged (the usual
// per-frame case), a full replacement otherwise. Empty data clears the mesh.
static void ReplaceOrUpdate(MeshCutting::ProceduralMesh& mesh, MeshCutting::ProceduralMeshData&& data)
{
    std::string error;
    if (data.indices.empty())
    {
        if (!mesh.GetData().vertices.empty())
            mesh.Clear();
    }
    else if (mesh.GetData().vertices.size() == data.vertices.size() && mesh.GetData().indices == data.indices)
        mesh.UpdateVertices(data.vertices, &error);
    else
        mesh.SetMesh(std::move(data), &error);
}

// Runs once per frame after ModelInstance::Update. The patch is skinned to the current pose, the
// slice plane (fixed in bone space) is rebuilt from the current joint matrices, and the cut pieces
// are either cut again (when the plane controls changed) or skinned to the new pose. Everything
// that is drawn gets its positions refreshed; normals are not recomputed.
void ModelViewer::UpdatePatchPose(void)
{
    const Model* model = m_ModelInst.GetModel();
    if (model == nullptr || m_PatchPiece.Empty() || m_PieceStatus != MeshCutting::SliceStatus::Ok)
        return;
    ScopedTimer _prof(L"Patch Pose Update");

    std::string error;
    if (!MeshCutting::BuildSkinMatrices(*model, m_ModelInst, m_BonePatchSource, m_SkinMatrices, &error)
        || !MeshCutting::ComputePoseJointWorld(*model, m_ModelInst, m_BonePatchSource, m_CurrentPoseJointWorld, &error))
    {
        m_SkinError = error;
        return;
    }

    int64_t skinTicks = 0;
    const auto skin = [&](MeshCutting::PatchPiece& piece)
    {
        const int64_t start = SystemTime::GetCurrentTick();
        const bool ok = MeshCutting::SkinPatchPiece(piece, m_SkinMatrices, nullptr, &error);
        skinTicks += SystemTime::GetCurrentTick() - start;
        return ok;
    };
    uint32_t skinnedNodes = 0;
    if (!skin(m_PatchPiece))
    {
        m_SkinError = error;
        return;
    }
    skinnedNodes += uint32_t(m_PatchPiece.nodes.size());

    // The plane is fixed in the selected bone's local space: carry it to this frame's joint matrix.
    UpdateBasePlane();

    bool freshlyCut = false;
    if (m_SliceEnabled && m_SliceDirty && m_PlaneBaseValid)
    {
        // Cuts the patch skinned just above, so new pieces start in this pose. If the cut fails the
        // previous pieces stay on screen and keep following the animation below.
        freshlyCut = UpdateSlice();
        m_SliceDirty = false;
    }
    if (m_SliceEnabled && m_HavePieces && !freshlyCut)
    {
        // Cut once, skinned every frame: same topology, new positions.
        MeshCutting::PatchPiece* pieces[2] = { &m_SlicedPositive, &m_SlicedNegative };
        for (MeshCutting::PatchPiece* piece : pieces)
        {
            if (!skin(*piece))
            {
                m_SkinError = error;
                return;
            }
            skinnedNodes += uint32_t(piece->nodes.size());
        }
    }
    if (freshlyCut)
        skinnedNodes += uint32_t(m_SlicedPositive.nodes.size() + m_SlicedNegative.nodes.size());

    // Refresh what is drawn.
    const float tubeRadius = m_PatchRadius * 0.008f;
    const auto refreshTubes = [&](const MeshCutting::PatchPiece& piece, MeshCutting::ProceduralMesh& mesh)
    {
        MeshCutting::ProceduralMeshData tubes;
        MeshCutting::BakeBoundaryDebug(piece, tubeRadius, tubes);
        ReplaceOrUpdate(mesh, std::move(tubes));
    };
    const auto refreshSurface = [&](const MeshCutting::PatchPiece& piece, MeshCutting::ProceduralMesh& mesh)
    {
        std::vector<MeshCutting::ProceduralVertex> vertices = mesh.GetData().vertices;
        if (MeshCutting::UpdateRenderPositions(piece, vertices))
            mesh.UpdateVertices(vertices, &error);
    };
    if (m_SliceEnabled && m_HavePieces)
    {
        if (!freshlyCut)
        {
            refreshSurface(m_SlicedPositive, m_PiecePositive);
            refreshSurface(m_SlicedNegative, m_PieceNegative);
            if (m_BoundaryDebugEnabled)
            {
                refreshTubes(m_SlicedPositive, m_PieceBoundaryPositive);
                refreshTubes(m_SlicedNegative, m_PieceBoundaryNegative);
            }
        }
    }
    else
    {
        refreshSurface(m_PatchPiece, m_BonePatch);
        if (m_BoundaryDebugEnabled)
            refreshTubes(m_PatchPiece, m_BoundaryDebug);
    }
    if (m_SliceEnabled && m_PlaneBaseValid)
    {
        MeshCutting::ProceduralMeshData planeMesh;
        MeshCutting::BakePlaneDebug(CurrentPlane(), m_PatchRadius * 0.7f, tubeRadius, planeMesh);
        ReplaceOrUpdate(m_PlaneDebug, std::move(planeMesh));
    }

    m_SkinError.clear();
    m_SkinnedNodes = skinnedNodes;
    const float ms = float(SystemTime::TimeBetweenTicks(0, skinTicks) * 1000.0);
    m_SkinMs = m_SkinMs == 0.0f ? ms : m_SkinMs + (ms - m_SkinMs) * 0.1f;
}

// The plane starts at the selected joint and points along the bone to its child joint (or away
// from its parent for a leaf joint), measured in the joint matrices of the current pose.
void ModelViewer::ComputeBasePlane(const Model& model)
{
    m_PlaneBaseValid = false;
    const std::vector<Matrix4>& pose = m_CurrentPoseJointWorld;
    if (m_BonePatchSource.paletteId >= model.m_SkinPalettes.size() || m_BonePatchSource.jointIndex >= pose.size())
        return;
    const SkinPalette& palette = model.m_SkinPalettes[m_BonePatchSource.paletteId];
    const uint32_t joint = m_BonePatchSource.jointIndex;
    const uint32_t node = model.m_JointIndices[palette.startJoint + joint];
    const Vector3 origin = Vector3(pose[joint].GetW());

    Vector3 direction(kZero);
    bool found = false;
    for (uint32_t j = 0; j < palette.numJoints && !found; ++j)
    {
        const uint32_t other = model.m_JointIndices[palette.startJoint + j];
        if (other < model.m_NodeParents.size() && model.m_NodeParents[other] == node)
        {
            direction = Vector3(pose[j].GetW()) - origin;
            found = true;
        }
    }
    for (uint32_t j = 0; j < palette.numJoints && !found && node < model.m_NodeParents.size(); ++j)
        if (model.m_JointIndices[palette.startJoint + j] == model.m_NodeParents[node])
        {
            direction = origin - Vector3(pose[j].GetW());
            found = true;
        }
    if (!found || float(Length(direction)) < 1e-5f)
        direction = Vector3(0.0f, 1.0f, 0.0f);
    direction = Normalize(direction);

    Vector3 up(0.0f, 1.0f, 0.0f);
    if (fabsf(float(Dot(direction, up))) > 0.99f)
        up = Vector3(1.0f, 0.0f, 0.0f);
    const Vector3 axisA = Normalize(Cross(direction, up));
    const Vector3 axisB = Cross(direction, axisA);

    // Keep the frame in the joint's local space; UpdateBasePlane carries it to each frame's pose.
    const Matrix4 toLocal = Invert(pose[joint]);
    const Matrix3 toLocal3 = toLocal.Get3x3();
    m_PlaneLocalPoint = Vector3(toLocal * origin);
    // The normal is a covector (it must stay perpendicular to the plane under non-uniform scale):
    // it goes local with the transpose and comes back with the inverse transpose.
    m_PlaneLocalNormal = Normalize(Transpose(pose[joint].Get3x3()) * direction);
    m_PlaneLocalAxisA = Normalize(toLocal3 * axisA);
    m_PlaneLocalAxisB = Normalize(toLocal3 * axisB);
    m_PlaneBaseValid = true;
    UpdateBasePlane();
}

// Carries the stored bone-local plane frame to the current pose of the selected joint.
void ModelViewer::UpdateBasePlane(void)
{
    const uint32_t joint = m_BonePatchSource.jointIndex;
    if (!m_PlaneBaseValid || joint >= m_CurrentPoseJointWorld.size())
        return;
    const Matrix4& world = m_CurrentPoseJointWorld[joint];
    const Matrix3 world3 = world.Get3x3();
    m_PlanePoint0 = Vector3(world * m_PlaneLocalPoint);
    m_PlaneNormal0 = Normalize(InverseTranspose(world3) * m_PlaneLocalNormal);
    m_PlaneAxisA = Normalize(world3 * m_PlaneLocalAxisA);
    m_PlaneAxisB = Normalize(world3 * m_PlaneLocalAxisB);
}

MeshCutting::SlicePlane ModelViewer::CurrentPlane(void) const
{
    const Vector3 normal = Normalize(Quaternion(m_PlaneAxisB, m_PlaneTiltB) * (Quaternion(m_PlaneAxisA, m_PlaneTiltA) * m_PlaneNormal0));
    const Vector3 point = m_PlanePoint0 + m_PlaneNormal0 * m_PlaneOffset;
    MeshCutting::SlicePlane plane;
    plane.point = { float(point.GetX()), float(point.GetY()), float(point.GetZ()) };
    plane.normal = { float(normal.GetX()), float(normal.GetY()), float(normal.GetZ()) };
    return plane;
}

void ModelViewer::ClearSlice(void)
{
    m_HavePieces = false;
    m_SliceDirty = false;
    m_SliceStatus = MeshCutting::SliceStatus::InvalidInput;
    m_SliceDetail.clear();
    m_SliceInfo.clear();
    m_SlicedPositive.Clear();
    m_SlicedNegative.Clear();
    m_PiecePositive.Clear();
    m_PieceNegative.Clear();
    m_PieceBoundaryPositive.Clear();
    m_PieceBoundaryNegative.Clear();
    m_PlaneDebug.Clear();
}

// Slices the welded patch with the current plane. The pieces on screen are replaced only when the
// slice is Ok; otherwise the old pieces stay and the status says why.
bool ModelViewer::UpdateSlice(void)
{
    if (!m_PlaneBaseValid || m_PieceStatus != MeshCutting::SliceStatus::Ok)
        return false;
    const float radius = m_PatchRadius;
    const MeshCutting::SlicePlane plane = CurrentPlane();

    std::string error;
    MeshCutting::ProceduralMeshData planeMesh;
    MeshCutting::BakePlaneDebug(plane, radius * 0.7f, radius * 0.008f, planeMesh);
    ReplaceOrUpdate(m_PlaneDebug, std::move(planeMesh));

    MeshCutting::SliceOptions options;
    options.cap = m_CapEnabled ? MeshCutting::CapMode::Create : MeshCutting::CapMode::None;
    MeshCutting::SliceResult result;
    m_SliceStatus = MeshCutting::SlicePatch(m_PatchPiece, plane, 1, options, result);
    m_SliceDetail = result.detail;
    if (m_SliceStatus != MeshCutting::SliceStatus::Ok)
    {
        m_SliceInfo.clear();
        return false;
    }

    const auto bake =[&](const MeshCutting::PatchPiece& piece, MeshCutting::ProceduralMesh& mesh, MeshCutting::ProceduralMesh& boundary)
    {
        MeshCutting::ProceduralMeshData data, tubes;
        MeshCutting::BuildRenderMesh(piece, data);
        mesh.SetMesh(std::move(data), &error);
        MeshCutting::BakeBoundaryDebug(piece, radius * 0.008f, tubes);
        if (tubes.indices.empty())
            boundary.Clear();
        else
            boundary.SetMesh(std::move(tubes), &error);
    };
    bake(result.positive, m_PiecePositive, m_PieceBoundaryPositive);
    bake(result.negative, m_PieceNegative, m_PieceBoundaryNegative);
    m_ShownNormal = plane.normal;
    m_HavePieces = true;
    m_SlicedPositive = result.positive;
    m_SlicedNegative = result.negative;

    const double original = MeshCutting::TotalArea(m_PatchPiece, MeshCutting::FaceKind::Surface);
    const double positive = MeshCutting::TotalArea(result.positive, MeshCutting::FaceKind::Surface);
    const double negative = MeshCutting::TotalArea(result.negative, MeshCutting::FaceKind::Surface);
    wchar_t info[200];
    swprintf_s(info, L"loops %zu  + %zu faces %zu nodes   - %zu faces %zu nodes   area +%.2f -%.2f original %.2f (diff %.1e)",
        result.loops.size(), result.positive.faces.size(), result.positive.nodes.size(), result.negative.faces.size(),
        result.negative.nodes.size(), positive, negative, original, positive + negative - original);
    m_SliceInfo = info;
    return true;
}

void ModelViewer::RenderProceduralMeshes(GraphicsContext& gfxContext, const D3D12_VIEWPORT& viewport,
    const D3D12_RECT& scissor, uint32_t frameIndex)
{
    // Patch vertices are already in world space; shift the copy beside the original to compare them,
    // or (overlay) draw it exactly on the character to check that it follows the animation.
    const float patchOffset = (m_ModelInst.IsNull() || m_OverlayEnabled) ? 0.0f : float(m_ModelInst.GetRadius());
    const Matrix4 patchWorld(AffineTransform::MakeTranslation(Vector3(patchOffset, 0.0f, 0.0f)));
    const Matrix4 viewProj = m_Camera.GetViewProjMatrix();
    MeshCutting::ProceduralMeshRenderer::DrawOptions pieceOptions;
    pieceOptions.cull = m_BackCull ? MeshCutting::ProceduralMeshRenderer::CullMode::Back
        : MeshCutting::ProceduralMeshRenderer::CullMode::TwoSided;
    pieceOptions.sectionTint = m_SectionTint;
    const auto draw = [&](const MeshCutting::ProceduralMesh& mesh, const Matrix4& world,
        const MeshCutting::ProceduralMeshRenderer::DrawOptions& options = MeshCutting::ProceduralMeshRenderer::DrawOptions())
    {
        m_ProceduralRenderer.Draw(gfxContext, mesh, world, viewProj, g_SceneColorBuffer, g_SceneDepthBuffer, viewport, scissor, options);
    };

    if (m_SliceEnabled && m_HavePieces)
    {
        // The pieces and the plane share the display offset; the pieces are pulled apart along
        // the plane normal so the cut can be seen.
        const float gap = m_OverlayEnabled ? 0.0f : m_PatchRadius * 0.05f;
        const Vector3 shift = Vector3(m_ShownNormal.x, m_ShownNormal.y, m_ShownNormal.z) * gap;
        const Matrix4 positiveWorld(AffineTransform::MakeTranslation(Vector3(patchOffset, 0.0f, 0.0f) + shift));
        const Matrix4 negativeWorld(AffineTransform::MakeTranslation(Vector3(patchOffset, 0.0f, 0.0f) - shift));
        draw(m_PiecePositive, positiveWorld, pieceOptions);
        draw(m_PieceNegative, negativeWorld, pieceOptions);
        if (m_BoundaryDebugEnabled)
        {
            draw(m_PieceBoundaryPositive, positiveWorld);
            draw(m_PieceBoundaryNegative, negativeWorld);
        }
    }
    else
    {
        draw(m_BonePatch, patchWorld);
        if (m_BoundaryDebugEnabled)
            draw(m_BoundaryDebug, patchWorld);
    }
    if (m_SliceEnabled)
        draw(m_PlaneDebug, patchWorld);

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
    text.DrawFormattedString(L"C: copy joint patch  X: clear patch  %ls\n", m_BonePatchStatus.c_str());
    if (m_WeldStats.corners > 0)
    {
        text.DrawFormattedString(L"Weld: corners %u -> nodes %u (position-only %u, skin-split positions %u)\n",
            m_WeldStats.corners, m_WeldStats.nodes, m_WeldStats.positionNodes, m_WeldStats.skinSplitPositions);
        text.DrawFormattedString(L"G: boundary %ls  replacement %u (yellow)  unwelded seam %u (green)  non-manifold %u  %hs\n",
            m_BoundaryDebugEnabled ? L"on" : L"off", m_WeldStats.replacementEdges, m_WeldStats.unweldedSeamEdges,
            m_WeldStats.nonManifoldEdges, MeshCutting::SliceStatusName(m_PieceStatus));
    }
    if (m_PieceStatus != MeshCutting::SliceStatus::Ok && !m_PieceDetail.empty())
        text.DrawFormattedString(L"%ls\n", Utility::UTF8ToWideString(m_PieceDetail.substr(0, 120)).c_str());
    if (m_PlaneBaseValid)
    {
        text.DrawFormattedString(L"T: slice %ls  [ ]: offset %.2f  , .: tilt A %.0f  ; ': tilt B %.0f  R: reset\n",
            m_SliceEnabled ? L"on" : L"off", m_PlaneOffset, m_PlaneTiltA * 57.29578f, m_PlaneTiltB * 57.29578f);
        text.DrawFormattedString(L"M: cap %ls  L: cull %ls  Z: section tint %ls  N: overlay on character %ls\n", m_CapEnabled ? L"on" : L"off",
            m_BackCull ? L"back" : L"two-sided", m_SectionTint ? L"on" : L"off", m_OverlayEnabled ? L"on" : L"off");
        text.DrawFormattedString(L"Follow: %u nodes skinned, %.3f ms/frame  %hs\n", m_SkinnedNodes, m_SkinMs,
            m_SkinError.empty() ? "" : m_SkinError.substr(0, 100).c_str());
        if (m_SliceEnabled)
        {
            text.DrawFormattedString(L"Slice: %hs  %ls\n", MeshCutting::SliceStatusName(m_SliceStatus), m_SliceInfo.c_str());
            if (m_SliceStatus != MeshCutting::SliceStatus::Ok && !m_SliceDetail.empty())
                text.DrawFormattedString(L"%ls\n", Utility::UTF8ToWideString(m_SliceDetail.substr(0, 120)).c_str());
        }
    }
    text.End();
}
