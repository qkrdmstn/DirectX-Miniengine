#pragma once
#include "Model.h"
#include <vector>

class GeometryRevision;

// 생성된 component-local geometry의 GPU 저장 공간. material/texture는 원본을 강하게 참조한다.
struct GeometryBuffers
{
    ByteAddressBuffer Data;
    ByteAddressBuffer Pose;
    ByteAddressBuffer Provenance;
    ByteAddressBuffer CapMaterial;
    std::shared_ptr<const GeometryRevision> Parent;
    std::vector<Mesh> Meshes;
    std::vector<uint32_t> PoseOffsets;
    std::shared_ptr<const Model> Source;
    uint32_t UncappedSegments = 0;
    uint32_t Generation = 1;
    uint64_t LineageBytes = 0;
    // 이미 제출한 GPU 명령이 사용하는 geometry를 완료 전에 해제하지 않는다.
    ~GeometryBuffers();
    // 미게시 출력의 마지막 GPU 사용이 완료된 뒤 내부 GPU buffer를 즉시 해제한다.
    void ReleaseGpuResourcesAfterCompletion() noexcept;
};
