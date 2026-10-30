상태: 진행 중 — `f2138029` 기준 1단계 검증 완료, GPU 자원 지연 해제·작업 메모리 예산 후속 구현 `1532fc60` (2026-10-29)

# 여러 Static CutJob 진행

## 요청과 작업 경계

여러 조각 절단의 작업 사이 대기를 줄인다. 사용자가 다중 작업 제출과 성능 비교 진행을 승인했다.
BreakConstraint M1은 본·물리 pose를 처리하며 GPU 절단 연결은 제외한다.
이 단계는 `CuttingSystem`의 작업 관리와 `GpuMeshCutter::Submit`의 초기 업로드를 변경한다.
본·물리 엔진·프레임 순서·GPU shader는 유지한다.
구현은 Luna 서브 에이전트, 검증과 문서 동기화는 부모 Codex, 리뷰는 별도 실행으로 수행한다.

## 현황 조사

- `CutJob`은 출력 A/B, scratch, metadata, readback, timestamp query와 fence를 작업별로 소유한다.
- `GpuMeshCutter::Submit`은 `GraphicsContext::Finish`로 같은 graphics queue에 제출한다.
- `CuttingSystem`의 단일 `m_Job`과 공통 대상 snapshot이 여러 작업 진행을 막는다.
- `SubmitPending`은 기존 작업 완료를 기다린 뒤 다음 후보를 제출한다.
- `Update`는 입력 revision·편집 세대·배치를 재검사하고 A/B를 함께 게시한다.
- `CutTransaction`의 `CutBudget`은 프레임당 새 body 수를 제한한다.
- `ModelViewer::Update`는 완료 수집 → 물리·pose 갱신 → 다음 후보 제출 순서를 제공한다.
- `DrainResults`는 완료 결과를 목록으로 전달하므로 기존 소비 API를 유지할 수 있다.

## 이번 단계

1. 작업과 owner·component·편집 세대·배치·게시 여부를 한 snapshot으로 묶는다.
2. 대기 후보를 빈 slot만큼 제출한다. 기본 상한은 2개다.
3. `-cut-inflight`로 1~4개를 선택하며 범위 밖 값은 경계로 제한한다.
4. 완료 결과는 제출 순서대로 프레임당 하나씩 게시한다.
5. 삭제·편집된 대상은 기존 stale 검사로 취소하며 GPU 자원은 완료까지 유지한다.
6. batch 완료 로그에 경과 시간과 최대 진행 작업 수를 남긴다.

버퍼 `Create(initialData)`는 내부 `InitializeBuffer`의 `Finish(true)`로 앞 작업을 기다린다.
이를 제거하려고 cap material·metadata·section descriptor를 빈 버퍼로 생성한 뒤
같은 작업 context의 upload 영역에 정확한 byte 수를 복사하고 `CopyBufferRegion`으로 기록한다.
업로드 page는 `CommandContext::Finish`가 반환한 fence까지 allocator에서 보관한다.
잘못된 옵션 문자열·정수 범위 초과는 기본 2를 사용하며 음수는 1로 제한한다.

단일 대상 `Request`의 busy 계약과 물리 게시 예산을 보존한다.
GPU 동시 실행·async compute·CPU worker 제출은 추가하지 않는다.
상한은 작업 개수 기준이며 총 GPU 메모리의 byte 예산은 후속 작업이다.
큰 조각은 각 작업의 출력 최대량을 따로 확보하므로 동시 진행 상한을 낮춰 비교한다.
기존 결과 게시·pose 교체·geometry 해제의 `IdleGPU`는 유지한다.
첫 결과 게시가 나머지 GPU 작업 완료를 기다릴 수 있으므로 지속적인 CPU/GPU 겹침은 보장하지 않는다.
`peakInFlight`는 완료 수집 전 보관 중인 작업 수이며 물리적으로 동시 실행된 GPU 작업 수가 아니다.

## 완료 조건과 검증

- 상한 1·2·4에서 같은 입력의 게시 수와 geometry 검증 통과 여부가 일치한다.
- 여러 후보에서 실제 최대 진행 작업 수가 2 이상임을 로그로 확인한다.
- Debug/Release 빌드와 DirectX debug layer 실행을 확인한다.
- 단일 절단·재절단·삭제·물리 transaction 회귀를 확인한다.
- 구현과 별도 실행의 리뷰 지적을 처리한다.
- 전체 완료 시간은 batch 경과 시간으로 비교한다. GPU 계산 시간의 감소와 혼동하지 않는다.
- 최초 단계는 사용자 검토를 위해 중단했다. 이후 사용자가 "끝까지 진행해"라고 지시하여 단계별 검증 후 후속 작업을 계속한다.

## 후속 범위와 조사

`CommitGeometryReplacement`에서 이전 `PosedGeometry`의 파괴자가 전역 GPU 완료를 기다린다.
파괴 시 이미 제출된 graphics·compute·copy fence를 고정하고 COM 참조를 보관하면 CPU를 기다리게 하지 않고 해당 사용이 끝난 뒤 해제할 수 있다.
이 방식은 미제출 command list를 보호하지 않는다. 자원을 파괴하기 전에 그 자원을 사용하는 기록 중인 명령을 제출해야 하는 기존 호출자 계약을 유지한다.
회수는 매 `CuttingSystem::Update`에서 완료 여부만 확인하고, 종료 시 큐 파괴 전에 남은 자원을 drain한다.
미게시 `CutJob` 출력은 자기 fence 완료와 단독 소유를 확인한 뒤 버퍼를 해제하여 다른 절단까지 기다리지 않는다.
공개된 geometry와 일반 컴포넌트 삭제·실패 rollback의 보수적인 전역 대기는 별개 경로로 유지한다.
후속 단계에서는 작업 개수 외에 새 출력·scratch의 byte 예산을 제한하고, 메모리와 프레임 처리 비용을 로그로 남긴다.
본·관절·물리 엔진·pose 평가 순서는 변경하지 않으며 BreakConstraint worktree도 수정하지 않는다.

### 지연 해제 단계 결과

`CommandListManager::RetireResource`는 세 queue의 마지막 제출 fence와 resource COM 참조를 보관한다.
`CollectRetiredResources`는 완료된 항목만 제거하며 정상 처리에서 CPU 대기를 추가하지 않는다.
`Shutdown`은 queue 종료 전에 남은 항목을 drain하고, 보관용 메모리 할당 실패 때도 안전한 대기로 대체한다.
`GetRetiredResourceBytes`는 buffer desc의 Width 합이며 실제 VRAM 할당 크기가 아니다.
`CutJob`은 자기 fence 완료 후 단독 소유한 미게시 출력만 직접 해제한다. 공유 출력·일반 MeshComponent 삭제의 안전 대기는 유지한다.
독립 기능 리뷰 0건, Debug 105.51초·Release 184.29초 빌드 성공이다.
Dragon을 포함한 Debug 기본 장면 절단(게시 2)과 컴포넌트 검사(owners 4·GPU preparation)가 모두 exit 0, D3D12 ERROR/CORRUPTION 0이다.
추가 자산 `Models/dragon.glb`는 기본 장면에 `Dragon`으로 생성한다. 원본은 871,414개 삼각형이며 기존 절단 출력 용량을 초과하므로 절단 지원 상한은 확대하지 않았다.
Luna의 일시적인 사용량 제한 후 안내된 재개 시각이 지나 같은 Luna 에이전트로 구현을 재개했다.

## 검증·성능 기록

최종 Debug 빌드 32.75초, Release 38.17초, 모두 exit 0이다. 초기 캐시 권한 오류는 권한 확장 빌드로 해결했고 C++14의 `std::clamp` 컴파일 실패는 `min/max`로 수정했다.
독립 리뷰의 동기 초기 업로드·옵션 파싱 지적을 수정했다. 최종 재리뷰 0건이며 부모 검토에서 불필요한 후보 중복 검사도 제거했다.
Debug 상한 1·2·4에서 Cube+Bunny 각각 게시 2, 단면·DOP·충돌 포함·표면 보존 검사 통과, D3D12 ERROR/CORRUPTION 0이다.
최종 Debug/Release의 Cube 재절단은 같은 owner의 두 조각을 함께 제출해 게시 합계 3을 확인했다. 공중 절단 `midair`의 pose·속도 계승 58 checks와 조각/owner 삭제 검사도 각각 통과했다.
Release의 `-cut-inflight -1`은 상한 1, `abc`는 안내 로그와 기본 2를 확인했다. 검증·성능 총 20회 모두 exit 0, D3D12 ERROR/CORRUPTION 0이다. 진행 중 대상 삭제는 별도 자동 시나리오 없이 수명 경로를 코드 리뷰했다.

성능은 NVIDIA GeForce MX450(1906 MB), 드라이버 32.0.15.9186, Release + D3D12 debug layer + debug 출력 수집 하네스로 측정했다. GPU 기반 validation은 켜지 않았다.
기본 Cube+Bunny+Xbot 장면에서 폭 30의 평면으로 Cube+Bunny를 자르고, skeletal Xbot은 기존대로 거절한다. `-cut-validate`는 성능 측정에서 끈다.
상한 순서 1/2/4/4/2/1/2/1/4로 독립 process를 실행했다. 아래 시간은 로딩을 제외한 batch 요청→마지막 게시 완료이며 동일한 새 업로드 경로끼리의 비교다.

| 상한 | 3회 경과 시간(ms) | 중앙값(ms) | 최대 보관 작업 |
| --- | --- | --- | --- |
| 1 | 223.238 / 187.848 / 267.640 | 223.238 | 1 |
| 2 | 202.982 / 226.143 / 166.500 | 202.982 | 2 |
| 4 | 242.743 / 202.680 / 251.970 | 242.743 | 2 |

기본 2의 중앙값은 1보다 약 9.1% 짧지만 범위가 겹치고 표본이 3개여서 일반적인 성능 향상을 확정하지 않는다. 상한 4도 실제 후보는 2개뿐이다.
GPU 메모리 최고 사용량과 프레임 지연 분포는 계측하지 않았다. 기존 게시 시 전역 대기 제거는 후속 단계다.
재현은 ModelViewer 작업 디렉터리에서 Release exe에 `-debug 1 -physics-sim 0 -cut 1 -cut-scene 1 -cut-width 30 -cut-gap 0 -cut-expect-publications 2 -physics-query-exit 12 -cut-inflight 2`를 전달한다.
검증 로그와 하네스는 `%TEMP%/CutInflightChecks`에 보관했다. CPU Geometry 코드는 변경하지 않았으며 CPU 기하 테스트는 재실행하지 않았다.
