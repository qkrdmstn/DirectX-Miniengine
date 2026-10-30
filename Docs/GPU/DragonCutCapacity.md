상태: 진행 중 — `76c7baf3` 기반 대형 Static 입력 용량 사전 계산 구현·독립 검토, 실행 검증 중, 미커밋 (2026-10-29)

# Dragon 절단 출력 용량

## 현황과 결정

원본 Dragon은 871,414개 삼각형과 9개 draw를 가진다. 기존 경로는 draw별 표면 `6T`, cap `3T`를 합친 `9T` 정점을 **양쪽 각각** 확보한다. 정점당 Data 48B, Pose 40B, Provenance 32B이므로 출력만 `9T × 240B`다. Dragon에서는 1,882,254,240B이며 scratch·원본·렌더 자원은 포함하지 않은 계산값이다. 기존 side당 1,000,000 정점 제한을 단순히 올리면 MX450의 1906MB 전용 메모리를 출력만으로 대부분 사용한다.

`GpuMeshCutter::PlanCapacity`는 `9T > 1,000,000`인 큰 입력에만 동일한 clip shader의 count 모드를 먼저 실행한다. 기존 작은 입력은 기존 최악 용량 계산과 비동기 제출 경로를 유지한다. count는 큰 표면이나 scratch 없이 작은 metadata·descriptor·readback을 사용하고, draw 수는 4096개로 제한한다. 이 단계에는 CPU의 fence 대기와 작은 개수 배열 readback이 있다.

`ClipTrianglesCS`와 `EmitTriangle`은 실제 출력과 같은 교차·퇴화 판정으로 draw별 A/B 표면 정점 수와 경계 후보 수를 센다. `CutCapacityPlan`은 draw별 `max(A, B)`와 전체 경계 후보 수를 저장한다. 실제 제출은 같은 frame의 입력 pose·평면을 사용한다. 출력은 양쪽 공통 layout을 유지하되 다음 용량을 사용한다.

```text
S = max(실제 경계 후보 수, 1)
V = Σdraw max(표면 정점 A, 표면 정점 B) + 3S
양쪽 출력 논리 byte = 240V
scratch byte = 136S + 4 × nextPowerOfTwo(4S)
```

각 GPU buffer의 64KiB 올림, upload allocator 2MiB page 올림, metadata·query·진단 readback은 `EstimateWorkingBytes`에 더한다. 이는 작업 예약량 계산이며 실측 VRAM 사용량이 아니다. 원본 geometry·부모 lineage·posed cache·렌더 자원은 별도로 남는다. count 사전 단계는 동시에 한 번만 실행하고, 별도의 8MiB allocator 여유를 로그에 표시한다.

cap shader의 4096 제한은 전체 입력 후보 수가 아니라 loop별 처리 한계다. 여러 loop를 허용하므로 cap 저장 용량을 전체 4096으로 줄이지 않는다. 기존 cap 실패 정책과 부분 cap 폐기 정책은 유지한다.

## 실패와 제한

- 입력 삼각형 1,000,000개, generation 32, 부모 lineage와 양쪽 예상 출력 512MiB 제한은 유지한다.
- 출력 byte offset의 uint32 범위, 계획 배열 길이와 경계 수를 검사한다. 실제 emit에서도 capacity overflow를 검사하며 오류 결과는 게시하지 않는다.
- 작업별·합산 예약은 `-cut-memory-mb` 범위 안에서 제한한다. 개별 초과는 거절하고 합산 초과는 기존 작업 수집 후 다시 제출한다.
- count 비용은 `CutJob::Started` 및 GPU timestamp보다 앞에 있다. 큰 입력의 `ReadyMs`와 GPU 작업 시간은 count 비용을 포함하지 않으며 batch 경과 시간에는 포함된다.
- `GpuBuffer::Create`, `ReadbackBuffer::Create/Map`과 절단 query heap은 할당/매핑 실패를 예외로 전달한다. 큰 진단 snapshot은 command list 기록 전에 확보해 할당 실패 시 원본을 유지한다.
- 실제 VRAM 고갈을 강제로 발생시키는 검증은 하지 않는다. 메모리 예산 거절 검증과 실패 경로 코드 리뷰를 구분한다.

## 실행 검증

최종 실행 결과와 재현 인수는 검증 완료 후 이 절에 기록한다. 다중 작업·MC 비교 결과는 [ConcurrentCutJobs.md](ConcurrentCutJobs.md)를 따른다.
