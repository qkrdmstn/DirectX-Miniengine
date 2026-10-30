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
        // A(0)/B(1)의 포함 검증과 공통 허용오차 기준으로 쓰는 슬라이스 충돌이다.
        std::array<MiniCollision::AggregateGeom, 2> ContainmentHulls;
        // 같은 출력 정점에 대해 별도로 포함을 검사할 재맞춤 충돌이다.
        std::array<MiniCollision::AggregateGeom, 2> RefitContainmentHulls;
        // 부모 충돌 계승이 실행됐는지다. true인데 hull이 빈 쪽은 얇은 조각일 때만 허용한다.
        bool InheritanceActive = false;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> Queries;
        uint64_t Fence = 0;
        CutConstants Constants;
        std::vector<uint32_t> Status;
        // 면(A/B)별 출력 정점의 GPU 18-DOP이며 SideDopValid가 거짓이면 값이 없다. 충돌 재맞춤에 사용한다.
        std::array<KDop18, 2> SideDop{};
        std::array<bool, 2> SideDopValid{};
        std::array<double, 4> StageMs{};
        // 라운드는 전체 경로, 표시·제거 합계는 캐시 경로만 집계한다.
        uint32_t CapRounds = 0, CapMarked = 0, CapRemoved = 0;
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

    // 큰 입력의 표면과 경계 집계로 출력 공간을 제한한다.
    struct CutCapacityPlan
    {
        std::vector<uint32_t> SurfaceVertices;
        uint32_t MaxSegments = 0;
    };

    class GpuMeshCutter
    {
    public:
        // 첫 절단 중 shader/PSO 생성으로 멈추지 않도록 장면 시작에서 파이프라인을 준비한다.
        static void Prepare();
        // 큰 입력만 작은 GPU count pass를 완료해 실제 출력 용량을 계산한다.
        static CutCapacityPlan PlanCapacity(const GeometryRevision& input, const PosedGeometry& posed,
            const Math::AffineTransform& world, const CutPlaneData& plane, float epsilon, bool force = false);
        // 지원 용량 계약 안에서 작업별 임시 GPU 메모리 예약치를 보수적으로 계산한다.
        // 원본 geometry·부모 lineage·posed cache는 이 값에 포함하지 않는다.
        // snapshotDiagnostics가 참이면 진단용 입력 복제와 최대 readback 묶음은 포함한다.
        static uint64_t EstimateWorkingBytes(const GeometryRevision& input, const PosedGeometry& posed,
            bool snapshotDiagnostics = false, const CutCapacityPlan* capacity = nullptr);
        // 현재 posed GPU 정점을 입력으로 Static A/B 표면·경계·cap을 같은 queue에서 계산한다.
        static std::unique_ptr<CutJob> Submit(std::shared_ptr<const GeometryRevision> input,
            const PosedGeometry& posed, const Math::AffineTransform& world,
            const CutPlaneData& plane, float worldEpsilon = 1.0e-5f, uint32_t capMode = 0,
            const std::vector<Math::BoundingSphere>& bounds = {}, bool snapshotDiagnostics = false,
            const CutCapacityPlan* capacity = nullptr, uint64_t retainedLimitBytes = 512ull * 1024 * 1024);
    };
}
