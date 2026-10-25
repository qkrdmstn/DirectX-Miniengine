#pragma once

#include "../Core/GpuBuffer.h"
#include "../Core/VectorMath.h"
#include <vector>

class GraphicsContext;
class Model;
struct Joint;
struct GeometryBuffers;

// 객체 Transform 이전의 위치(float3), 법선(float3), 탄젠트(float4)를 GPU에 보관한다.
class PosedGeometry
{
public:
    // 모델의 정점 순서를 보존하는 객체별 출력 공간을 할당한다.
    explicit PosedGeometry(const Model& model);
    // 생성 결과의 canonical GPU pose를 컴포넌트 전용 cache로 복사할 공간을 만든다.
    explicit PosedGeometry(const GeometryBuffers& geometry);
    // 이미 component-local인 생성 결과를 한 번 복사하고 node/skin 변환은 생략한다.
    void UpdateBaked(GraphicsContext& context, const GeometryBuffers& geometry);
    // GPU 출력 자원의 암묵적 공유 복사를 금지한다.
    PosedGeometry(const PosedGeometry&) = delete;
    // GPU 출력 자원의 암묵적 복사 대입을 금지한다.
    PosedGeometry& operator=(const PosedGeometry&) = delete;
    // 제출된 GPU 작업이 끝난 뒤 객체별 버퍼를 해제한다.
    ~PosedGeometry();
    // 현재 node pose와 본 팔레트로 정점을 갱신하며 변하지 않은 static pose는 재사용한다.
    void Update(GraphicsContext& context, const Model& model,
        const Math::Matrix4* nodeTransforms, const Joint* joints);
    // 렌더링과 후속 절단이 공유할 읽기 전용 GPU 버퍼를 반환한다.
    const ByteAddressBuffer& GetBuffer() const { return m_Buffer; }
    // 원본 mesh의 정점 번호에 대응하는 결과 구간의 바이트 시작점을 반환한다.
    uint32_t GetOffset(uint32_t meshIndex) const { return m_Offsets.at(meshIndex); }
    // Compute가 유효한 결과를 기록했는지 확인한다.
    bool IsReady() const { return m_Valid; }

    static constexpr uint32_t VertexStride = 40;

private:
    ByteAddressBuffer m_Buffer;
    std::vector<uint32_t> m_Offsets;
    std::vector<Math::Matrix4> m_LastNodeTransforms;
    bool m_Valid = false;
};
