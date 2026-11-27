상태: 설계안 (2026-11-27, base `2411cd56`, 미착수) — 단계 순서 사용자 합의, 구현은 ST(PatchStaleFrozenJoints) 세션 종료 후

# 스켈레탈 절단 지연 단축 결정

## 목표
- RTX 5060 Ti에서 스켈레탈 절단 한 번이 요청→게시 약 110 ms, 6프레임(dframe 5)이 걸린다. cutter GPU 자체는 0.12 ms다.
- 지연의 대부분은 CPU 동기 대기, 지연 생성(PSO·버퍼), 프레임 경계를 만드는 검사 순서다. 이 셋을 줄인다.
- 첫 절단과 반복 절단(같은 실행의 second-bone)을 따로 집계해 둘 다 개선한다.
- 절단 결과(기하·스키닝·섬 판정·게시 순서)는 바꾸지 않는다. CPU oracle과 `-island-gpu` 기본값 0도 그대로 둔다.

## 측정 근거 (5060 Ti, 다른 PC)
| 구간 | 값 | 원인 (조사 메모 기준) |
| --- | --- | --- |
| `extract-submitted` CPU | ≈14 ms | `InitializePatch` 등 PSO 최초 생성, 입력 T 기준 최대 용량 버퍼 생성, 초기 업로드 6개마다 `InitializeBuffer→Finish(true)` |
| copy 준비→cut 제출 | +1프레임 | `SkeletalPatchCopy::Update`에서 `m_CutPending` 제출 검사가 추출 `Poll`보다 앞에 있음 |
| 첫 `DiagnoseIslands` | 5.3 ms | quiet에서도 `PatchLog` 인자인 `SafeDiagnosticNodeName`이 먼저 평가되어 첫 호출에 GLB JSON 파싱 (가설) |
| `FinishCutResult` | 28 ms | `BuildPatchSkinVertices` A 9.0·B 2.5 ms(동기 업로드 5개×2, SkinVB 전체 readback) + 미계측 16.7 ms |
| `-island-gpu 1` | 이득 없음 | CPU oracle이 계속 돌고 readback 프레임 수가 같음, 요청 프레임 CPU +7~8 ms |
- 미계측 16.7 ms 후보: `MeshComponent::SetGeometry`(PosedGeometry 생성·mask 정리·collision 재구축), `GeometryRevision::FromGenerated`·`SelectSections`, `m_SkinPatch.reset()`, physics 상태 복사·`QueueBreak`. 코드만으로 확정할 수 없다.
- 프레임 흐름: F0 요청·추출 제출 → F1 추출 회수·copy 준비 → F2 cutter 제출 → F3 cutter 회수·body 분류 제출 → F4 body 회수·skin·`QueueBreak` → F5 break 결과·게시.

## 결정과 이유
1. **계측을 먼저 한다.** 28 ms의 60%가 출처 불명이라, 업로드 묶기 범위를 정하기 전에 `FinishCutResult` 하위 구간에 CPU 타이머를 단다. 동작 변경 없이 측정만 바꾸므로 가장 안전하다.
2. **quiet에서는 섬 상세 로그 루프를 호출자에서 건너뛴다.** `PatchLog` 내부 반환으로는 인자 평가를 막을 수 없다. 결과 생성 계산(`LabelPatchIslands`, 부호·body 판정)은 그대로 두고 이름 조회·대표 joint 탐색만 생략한다.
3. **copy 준비와 cut 제출을 같은 프레임에서 한다.** `m_CutPending` 검사를 추출 회수 뒤로 옮기거나 회수 직후 한 번 더 검사한다. 1프레임(≈16.7 ms)을 줄이는 가장 싼 변경이다. `cutting.IsBusy()` 조건은 유지한다.
4. **PSO·root signature를 시작 시 미리 만든다.** `GpuMeshCutter::Prepare`가 이미 startup에서 하는 방식을 따라, 패치 추출·섬 거리·body 분류/압축·`BuildPatchSkinVertices`용 초기화를 시작 시 호출한다. 첫 절단 비용만 줄인다.
5. **동기 초기 업로드를 Scene 쪽에서 묶는다 (Core 수정 없음).** `SkeletalPatchJob::Submit`과 `BuildPatchSkinVertices`의 작은 업로드를 같은 command context의 `LinearAllocator` 업로드 + `CopyBufferRegion`으로 바꿔, 제출 fence 하나로 합친다. `CommandContext::InitializeBuffer`는 다른 샘플이 쓰므로 건드리지 않는다.
6. **후속(이 문서 범위 밖):** SkinVB bounds GPU 계산으로 전체 readback 제거, 요청별 버퍼 풀, readback 병합, body 회수·break 게시 같은 프레임화. L1 결과를 보고 별도 결정 문서로 다룬다.
- 기각: `-island-gpu 1` 기본화(측정상 이득 없음). Core `InitializeBuffer` 비동기 오버로드(영향 범위가 샘플 전체).

## 인터페이스 골격
```cpp
// CutPerfLog.h: 기존 CPU 타이머 이름만 추가 (새 API 없음)
//   FinishCutResult-select-sections, -set-geometry, -piece-skin-A/B, -skin-reset, -queue-break
//   DiagnoseIslands-name-lookup
// ModelViewer.cpp: 측정용 명령줄 -vsync 0 → Display 의 Timing/VSync(s_EnableVSync) 를 끈다.

// SkeletalPatchCopy.cpp
void SkeletalPatchCopy::Update(...)
{
    // 추출 회수·copy 준비 (기존)
    // m_CutPending && !cutting.IsBusy() 이면 같은 프레임에 cut 제출 (L3)
}
static void PrewarmSkeletalCutPipelines();   // 시작 시 1회, 각 Initialize* 호출 (L4)

// 업로드 묶기 (L5): 작은 정적 버퍼는 초기 데이터 없이 Create 후 같은 context에서 복사
void UploadInto(GraphicsContext& ctx, GpuBuffer& dst, const void* data, size_t bytes);
```
- `UploadInto`는 `ctx.ReserveUploadMemory` + `CopyBufferRegion` + 상태 전환만 한다. 업로드 메모리 수명은 기존 `LinearAllocator` fence 재사용에 맡긴다.

## 단계 목록
시간은 측정값 기준: Codex 리뷰 2~5분(측정). Release 빌드·`Run-CutPerf.ps1 -N 10` 실행 시간은 아직 측정값이 없어, L1에서 처음 재서 이 표에 적는다. 각 단계 구현 후 Codex 리뷰 → 사용자 확인·커밋.

| 단계 | 개념 | 변경 범위 (≈300줄 이내) | 기대 효과 | 시간 |
| --- | --- | --- | --- | --- |
| L1 | 계측 | `FinishCutResult` 하위 6구간·`DiagnoseIslands-name-lookup` 타이머, `-vsync 0` 명령줄, runner가 첫/반복 절단을 나눠 집계 | 효과 0, 16.7 ms 정체 확인 | 구현+리뷰 2~5분 + 빌드·측정(L1에서 측정) |
| L2 | quiet 이름 조회 생략 | `DiagnoseIslands` 상세 로그 루프를 `CutPerf::Quiet()`로 감쌈 | 첫 절단 ≈4~5 ms | 리뷰 2~5분 + 빌드·측정 |
| L3 | 같은 프레임 제출 | `SkeletalPatchCopy::Update` 검사 순서 | 1프레임(≈16.7 ms) | 리뷰 2~5분 + 빌드·측정 + Release 회귀 |
| L4 | PSO 미리 만들기 | 각 `Initialize*`를 startup에서 호출 | 첫 절단 extract CPU 일부 | 리뷰 2~5분 + 빌드·측정 |
| L5 | 업로드 묶기 | `SkeletalPatchJob::Submit`, `BuildPatchSkinVertices` 초기 업로드 | 첫/반복 extract·skin 동기 대기 | 리뷰 2~5분 + 빌드·측정 + Release 회귀 |
- L1 결과로 16.7 ms가 L5 밖(예: `SetGeometry`)에 있으면 L5 범위를 사용자와 다시 정한다.
- L3·L5는 `SkeletalPatchCopy.cpp`, L1은 `ModelViewer.cpp`도 건드린다. ST 세션이 같은 파일을 쓰는 동안 시작하지 않는다.

## 완료 조건
- 측정: 이 PC(MX450)에서 단계마다 Release 빌드 후 `Tools/CutPerf/Run-CutPerf.ps1 -N 10`(quiet GPU timer OFF 설정)을 실행하고, `CutPerfSummary.csv`·raw cut-perf log의 `TOTAL_request_to_publish`, `frames_spanned`(=`published` MARK의 `dframe`), `extract-submitted`·`FinishCutResult` CPU를 이전 단계와 비교해 이 문서에 기록한다.
- 5060 Ti: L3·L5 뒤 `Pack-CutPerf.ps1` 패키지로 사용자가 다른 PC에서 측정한다. vsync ON 1회, `-ExtraArgs "-vsync 0"` 1회.
- 목표 (5060 Ti, 반복 절단): vsync ON `frames_spanned ≤ 4`(≈≤70 ms), vsync OFF `TOTAL_request_to_publish ≤ 50 ms`. 첫 절단은 목표 없이 감소량을 기록한다.
- 정확성: 각 단계 후 기존 Release 회귀(SP7e2·RS·AR4·MI4)가 이전과 같은 PASS/FAIL이고, 절단 결과 checksum·삼각형 수가 착수 전(base)과 같다. D3D12 validation ERROR 0 (Debug 1회, L3·L5).
- `-island-gpu 1`·CPU oracle·verify 동작과 기본값이 바뀌지 않는다.
- 문서: 단계마다 이 `상태:` 줄과 `Docs/README.md` 행을 갱신한다.
