#pragma once

// 스킨 없는 메시 하나를 절단 입력으로 복사하는 기능(Unreal CopyProceduralMeshFromStaticMeshComponent 대응)과 그 위치 갱신 행렬.
// 원본을 읽고 용접하여 ProceduralMesh 하나를 생성한다. 설계: Docs/ProceduralMeshSimplification.md.

#include "BonePatch.h"
#include <cstdint>
#include <string>
#include <vector>

class Model;
class ModelInstance;

namespace MeshCutting
{
    // 모델의 스킨 없는(kHasSkin 없음) 메시 하나의 요약: 후보 찾기용 (Unreal 섹션 박스 조기 판정 입력)
    struct StaticMeshInfo
    {
        uint32_t meshIndex = 0;
        uint16_t meshCBV = 0;                                   // world 행렬 번호
        DirectX::XMFLOAT4 bounds = { 0.0f, 0.0f, 0.0f, 0.0f };  // 로컬 바운딩 구 (x, y, z, 반지름)
    };

    // 모델의 스킨 없는 메시를 메시 번호 순서로 모은다.
    void ListStaticMeshes(const Model& model, std::vector<StaticMeshInfo>& out);

    // 스킨 없는(kHasSkin 없음) 메시 meshIndex의 모든 draw, 모든 삼각형을 복사한다.
    // outMesh는 현재 world 위치·법선, outSource는 로컬 위치·법선과 빈 스킨(joints, weights 0)을 담는다. 실패하면 출력을 바꾸지 않는다.
    SliceStatus CopyProceduralMeshFromStaticMesh(const Model& model, const ModelInstance& instance, uint32_t meshIndex,
        ProceduralMesh& outMesh, MeshSource& outSource, WeldStats& stats, std::string* error = nullptr);

    // Raw read for local pick caches.
    bool ReadStaticMesh(const Model& model, const ModelInstance& instance, uint32_t meshIndex,
        ProceduralMeshData& outMesh, MeshSource& outSource, std::string* error = nullptr);

    // 강체 정점용 위치 갱신 행렬: 슬롯마다 행렬 하나 = 그 메시의 현재 world 행렬 (가짜 joint를 만들지 않는다).
    bool BuildRigidMatrices(const Model& model, const ModelInstance& instance, const MeshSource& source,
        std::vector<std::vector<DirectX::XMFLOAT4X4>>& slotMatrices, std::string* error = nullptr);
}
