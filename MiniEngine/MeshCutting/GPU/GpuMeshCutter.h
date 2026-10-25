#pragma once
#include "CutTypes.h"
#include "GeometryRevision.h"
#include "ReadbackBuffer.h"
#include "../Geometry/CutPlane.h"
#include <wrl/client.h>
#include <array>
#include <chrono>

namespace MeshCutting
{
    // 작업이 GPU 입력/후보 결과를 소유한다. Scene 포인터는 보관하지 않는다.
    class CutJob
    {
    public:
        std::shared_ptr<const GeometryRevision> Input;
        std::array<std::shared_ptr<GeometryBuffers>, 2> Output;
        std::vector<CutSection> Sections;
        std::vector<Mesh> Templates;
        ByteAddressBuffer Metadata, Scratch;
        StructuredBuffer Descriptors;
        ReadbackBuffer StatusReadback, TimeReadback;
        ReadbackBuffer DiagnosticInputPose, DiagnosticInputData;
        bool HasDiagnosticInput = false;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> Queries;
        uint64_t Fence = 0;
        CutConstants Constants;
        std::vector<uint32_t> Status;
        std::array<double, 4> StageMs{};
        double GpuMilliseconds = 0, ReadyMilliseconds = 0;
        std::chrono::steady_clock::time_point Started;
        bool Collected = false;
        // 이미 제출한 작업의 완료 후 scratch와 query 자원을 해제한다.
        ~CutJob();
        // 기다리지 않고 완료된 작은 count/status만 수집한다.
        bool Poll();
        // 공면·접촉만 있는 작업은 새 단면이 있어도 분할로 인정하지 않는다.
        bool HasSplit() const { return Collected && Status[0] == 0 && Status[1] != 0 && (Status[5] & 7) == 7; }
        // 명시적 테스트에서만 전체 GPU 결과의 인덱스·출처·평면·단면 면적을 검증한다.
        void ValidateDiagnostics();
        // 결과 버퍼가 완성되고 오류가 없는 경우에만 불변 revision을 만든다.
        std::array<std::shared_ptr<const GeometryRevision>, 2> BuildResults();
    };

    class GpuMeshCutter
    {
    public:
        // 첫 절단 중 shader/PSO 생성으로 멈추지 않도록 장면 시작에서 파이프라인을 준비한다.
        static void Prepare();
        // 현재 posed GPU 정점을 입력으로 Static A/B 표면·경계·cap을 같은 queue에서 계산한다.
        static std::unique_ptr<CutJob> Submit(std::shared_ptr<const GeometryRevision> input,
            const PosedGeometry& posed, const Math::AffineTransform& world,
            const CutPlaneData& plane, float worldEpsilon = 1.0e-5f, uint32_t capMode = 0,
            const std::vector<Math::BoundingSphere>& bounds = {}, bool snapshotDiagnostics = false);
    };
}
