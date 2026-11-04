상태: 진행 중 — CI-1a `62b5228c`, CI-1b `7947cfb1`, CI-2 `6dd6fe35`, CI-3 조각 PG Data 디코드 D/R 빌드·실행 검증 완료(base `6dd6fe35`); CI-4·5는 SP-7 미커밋 파일 커밋 뒤 진행; 결정 B 강체 부착 유지 (2026-11-04)

# 절단 입력을 revision 데이터 버퍼로: 애니메이션과 절단 분리

## 1. 목표

- Static 절단기(`GpuMeshCutter`)가 PosedGeometry(PG)를 읽지 않게 한다. 대신 revision이 가리키는 데이터 버퍼를 직접 읽는다.
- 애니메이션(PG 매 프레임 갱신)과 절단을 서로 기다리지 않는 별개 사이클로 둔다.
- 스키닝 메시 패치 조각의 운동 방식(결정 B)을 확정한다.

근거는 Codex 현황 조사 메모(2026-11-04, 공유 노트 `codex-survey-static-input.md`)다. 아래 사실은 그 메모에서 가져왔다.

## 2. 현재 구조에서 확인한 사실

- `GpuMeshCutter::{Submit,PlanCapacity}`는 평면을 `transpose(Wc)*WorldPlane`으로 component-local로 옮긴다. 정점은 `CutCommon.hlsli::ReadVertex`가 t1(PG, 40 B)에서 P/N/T를, t0(revision Data)에서 UV·IB를 읽는다.
- 원본 revision의 Data(`Model::m_DataBuffer`)는 **mesh-local**이다. PG는 `MeshComponent::Update`의 `localNodes[i] = inverse(SourceFrame) * nodes[i]`를 `PreparePoseCS`에서 mesh별(`meshCBV`)로 적용한 값이다. section마다 행렬이 다를 수 있다.
- 생성 revision의 Data(28 B packed)는 이미 **component-local**이다. 패치 GB의 Data도 PG에서 복사한 값이라 component-local이다. 여기에 node 행렬을 다시 적용하면 안 된다.
- N/T 포맷은 R10G10B10A2_UNORM이다. 디코드(`*2-1`)는 `PreparePoseCS::DecodeFrame`와 `CutCommon::Frame`에 이미 있다. P는 Data와 Pose 모두 float3라서 비트가 같다.
- `GeometryBuffers::Pose`(40 B)의 생산자는 cutter의 `WriteVertex`와 `PatchExtractCS`다. 독자는 `CutBoundsCS`, `PosedGeometry::UpdateBaked`(호출처는 `MeshComponent::Update`의 Generated 분기 하나), `GeometryRevision::FromGenerated`의 범위 검사, `GpuCutValidation.cpp`, `SkeletalBoundaryChecks.cpp::PairBoundary`다.
- 렌더는 Generated에 `UsePosedVertices=1`로 PG(t21)를 묶고, `DefaultVS`/`DepthOnlyVS`가 P/N/T를 PG에서 읽는다.

## 3. 결정 A (사용자 확정): Static 절단은 PG를 입력으로 쓰지 않는다

### A1. 입력 = revision Data + section별 source→component 행렬
- cutter는 t0의 revision Data만 읽는다. t1(입력 PG)은 없앤다.
- section마다 `SectionToComponent`(= `localNodes[meshCBV]`)와 그 3×3 역전치 `NormalToComponent`를 상수로 받는다. 생성 revision의 section은 항상 단위 행렬이다.
- `ReadVertex`는 Data를 디코드한 뒤 이 행렬을 적용한다. N/T는 `PreparePoseCS::main`처럼 **정규화하지 않은** 변환값을 돌려준다. 정규화는 지금처럼 보간 뒤 `WriteVertex`에서 한다. 비균일 배율에서 미리 정규화하면 보간 결과가 달라지기 때문이다. 그래서 결과 P/N/T는 지금 PG에서 읽던 값과 같은 component-local 값이다.
- **평면은 지금처럼 component-local 하나로 둔다.** 평면을 section(mesh) 로컬로 옮기지 않는 이유:
  - 출력 Data·cap·bounds는 공통 component-local이어야 한다(Generated 계약). 정점 변환은 어차피 필요하다.
  - 경계 연결과 cap은 여러 section에 걸친 loop를 한 공간에서 다룬다. section별 평면을 쓰면 경계 끝점을 다시 같은 공간으로 맞춰야 한다.
  - section 분류는 지금도 `MeshComponent::Update`가 component-local로 옮긴 section BoundingSphere를 쓴다. 이 bounds를 그대로 재사용하고 행렬을 다시 적용하지 않는다.
- 행렬은 `CuttingSystem`이 요청 시점에 Wc와 함께 CPU에서 snapshot한다. `MeshComponent::Update`가 PG에 쓰는 node 선택(Frozen node 포함)과 같은 값을 쓴다. node 애니메이션이 있는 Static도 그 시점 자세로 굳는다. 이는 현행 PG snapshot과 같은 결과다.
- 원본 skin section은 지금도 `CuttingSystem::Submit`·`GpuMeshCutter::{EstimateWorkingBytes,Submit}`에서 거절된다. 이 거절 계약을 유지한다. 그래서 Data를 직접 읽어도 스키닝이 빠지는 경우가 없다. 패치 Generated 입력은 skin 플래그가 없고 단위 행렬을 쓴다.
- `CutSection`에 행렬을 넣는다. `CutSection`은 이미 `sizeof(CutSection)` stride의 StructuredBuffer(t2)로 전달되므로 별도 버퍼가 필요 없다. C++ `CutSection`과 HLSL `Section`의 크기, 행 우선 3×4 배치, `mul` 규약을 같은 단계에서 함께 바꾸고 `static_assert`로 크기를 고정한다.
- 결과: 절단 제출은 이번 프레임에 PG가 갱신됐는지 기다리지 않는다. `CuttingSystem::Submit`의 PG 준비 검사를 없앤다.

### A2. 조각 PG는 Data에서 한 번 디코드해 채운다
CI-3 구현: `PosedGeometry::UpdateGenerated`는 기존 `PreparePoseCS`에 28 B Data·생성 flags·단위 위치/법선 행렬을 바인딩하고 skin 없이 최초 한 번만 디코드한다. Generated 생성자는 `count = mesh.vbSize / mesh.vbStride`, `offset[i] = 40 * sum(count[0..i))`, `size = 40 * sum(count)`로 PG를 연속 할당해 렌더의 `meshOutputOffset + baseVertex * 40` 계약을 유지한다. `MeshComponent::Update`가 새 경로를 호출한다. `-cut-validate 1`에서만 첫 PG를 readback해 기존 GB::Pose의 대응 구간과 비교한다(위치 성분 최대 오차 1e-6, 정규화 N/T 최대 각도 0.5도). 로그는 `Generated PG decode PASS: vertices=.. maxPosError=.. maxNormalDeg=.. maxTangentDeg=..`이며 실패 시 FAIL과 예외를 남긴다. GB::Pose 및 `FromGenerated`의 Pose 검사는 유지했다. 빌드·실행 검증은 호출자에게 남긴다.

CI-3 검증(MX450, D/R 빌드 exit 0, `-debug 1 -cut-validate 1`, D3D12 ERROR 0): Cube, `-cut-transform-checks 1`(비균일), Bunny 3회, B 재절단, CesiumMan 패치, `-component-checks 1`, Release Bunny y=-240·45도·패치가 통과했다. 조각 PG와 GB::Pose는 위치가 비트 동일(maxPosError=0)이고 법선 ≤0.093도, 탄젠트 ≤0.089도다. tangent가 없는 mesh는 렌더가 T를 읽지 않으므로 T 비교에서 제외한다(비균일 배율에서 기본 T가 13.06도 달랐던 원인). 진단 로그는 `Utility::Printf` 256바이트 버퍼에 맞게 두 줄로 나눴다.

- `UpdateBaked`의 "GB::Pose 복사"를 "GB::Data 디코드(단위 행렬, skin 없음)"로 바꾼다. `PreparePoseCS`의 디코드를 재사용하는 Generated 전용 dispatch로 만든다.
- 렌더 경로(`UsePosedVertices`, t21)는 바꾸지 않는다. 조각을 PG 없이 Data VB로 직접 그리는 안은 렌더러 변경이 커서 이번 범위에서 뺀다.
- PG 할당 크기와 mesh별 offset은 GB::Pose 크기·`PoseOffsets` 대신 Data 정점 수에서 계산한다(Generated 생성자).
- 정밀도: 재절단과 조각 PG의 N/T가 float에서 10 bit로 바뀐다(단계 약 0.002). 기존 0.01 검사는 법선 길이 검사라서 이 변화를 재지 못한다. 그래서 6절에 법선 각도 오차 측정을 따로 둔다.

### A3. GB::Pose를 없앤다
- 결정 B가 강체 유지이므로 생성 정점에 posed 위치 외의 속성은 필요 없다. Pose는 Data의 P/N/T를 float로 한 번 더 담은 중복이다.
- 독자를 먼저 Data로 옮긴 뒤(`CutBoundsCS`, 진단, `PairBoundary`, A2), 생산자와 버퍼를 없앤다(`WriteVertex`, `PatchExtractCS`, `GeometryBuffers::Pose/PoseOffsets`, `FromGenerated`의 Pose 검사).
- 수명과 회계도 같이 고친다: `GeometryBuffers::{~GeometryBuffers,ReleaseGpuResourcesAfterCompletion}`의 Pose 해제, cutter와 `CuttingSystem`의 정점당 120/240 B 계보·예산·로그 계산이다.
- 효과: 논리 용량으로 생성 정점당 40 B와 Pose UAV·barrier가 사라진다. `EstimateWorkingBytes`는 64 KiB 단위로 할당을 계산하므로 실제 할당 감소량은 따로 기록한다. 패치 추출(`SkeletalPatchJob::Submit`)은 지금처럼 PG에서 복사한다. 스키닝 자세를 굳히는 것이 그 단계의 역할이기 때문이다.

### A 인터페이스 골격 (이름은 구현 시 주변 코드에 맞춘다)
```cpp
// MeshCutting/GPU/CutTypes.h — PoseOffset 대신 section 행렬
struct CutSection { /* 기존 필드 */ float SectionToComponent[12]; float NormalToComponent[12]; };

// ModelViewer/Scene/MeshComponent.h — 요청 시점 section 행렬 snapshot (생성 revision은 단위 행렬)
bool BuildCutSectionFrames(std::vector<Math::Matrix4>& outSectionToComponent) const;

// MeshCutting/GPU/GpuMeshCutter.h — const PosedGeometry& 인자를 section 행렬로 교체
bool Submit(..., const GeometryRevision& source, const std::vector<Math::Matrix4>& sectionToComponent, ...);
bool PlanCapacity(..., const std::vector<Math::Matrix4>& sectionToComponent, ...);

// Model/PosedGeometry.h — UpdateBaked 대체: Data를 한 번 디코드
void UpdateGenerated(ComputeContext& context, const GeometryBuffers& buffers);
```
```hlsl
// CutCommon.hlsli — t1 제거, Data 디코드 + section 행렬
Vertex ReadVertex(CutSection section, uint vertex); // P = mul(S, float4(p,1)), N/T = mul(Nm, n) (정규화는 WriteVertex)
```

## 4. 결정 B (사용자 선택 2026-11-04): 패치 조각은 강체 부착을 유지한다

- 지금처럼 `NodeAttachment`로 A는 P의 직접 부모 node, B는 P node를 강체로 따른다(`Scene::PrepareMeshes`, [SkeletalPatchBreak.md](SkeletalPatchBreak.md) D4).
- GB에 bind 위치·joint·weight를 넣지 않는다. 그래서 A3(Pose 제거)가 가능하다.
- 판단 근거와 받아들인 위험:
  - 패치 경계 정점(원본과 공유하는 인덱스)의 선택 joint 가중치는 0이다(`PatchExtractCS::Selected`). 그래도 경계가 여러 본의 블렌드로 움직이면 단일 node 강체 운동과 다르다. 이음새가 벌어질 수 있다.
  - 기존 로그(현재 소스로 재측정 안 함, `%TEMP%/SP7b/Runtime-*-SP7e2Break.log`): Xbot P60은 게시 0.79→60프레임 후 2.07 mm(Debug)다. CesiumMan P8은 35.9→495.5 mm이고 게시 1 mm 검사 FAIL이다.
  - 사용자는 추가 비용 없이 강체를 유지하고, 오차는 측정·보고로 다루기로 했다.
- 하지 않는 것: 연속 스키닝(GB에 bind P/N/T·joint/weight 추가, 교차점 가중치 보간과 4개 축약, 조각별 팔레트에서 끊긴 쪽 본 고정, 조각 PG 매 프레임 스키닝)이다. 비용은 약 5~6단계×300줄로 추정했고, 비교 기록으로만 남긴다.
- 경계 오차 측정과 판정은 [SkeletalPatchBreak.md](SkeletalPatchBreak.md)의 SP-7 진행(미커밋 `SkeletalBoundaryChecks.*`)이 소유한다. 이 문서는 기준을 바꾸지 않는다.

## 5. 단계 (한 단계 = 한 개념, 약 300줄 이하, 단계마다 새 Codex 구현 + 다른 Codex 리뷰)

시간 근거(측정): D/R 빌드 약 50.5/46.4초(SP-7e), Codex 리뷰 2~5분. 구현 시간은 추정이다.

| 단계 | 개념 | 주요 위치 | 예상 시간 |
| --- | --- | --- | --- |
| CI-1a | section 행렬 snapshot·`CutSection` ABI 확장만. 기존 PG 입력 경로는 유지하고, 진단에서 `Data×행렬` 위치가 PG 위치와 같은지 검사한다(약 150~200줄) | `CutTypes.h`·HLSL `Section`, `MeshComponent::BuildCutSectionFrames`, `CuttingSystem::Submit`, `GpuCutValidation.cpp` | 구현 20분 + 빌드 2분 + 검증 10분 + 리뷰 5분 |
| CI-1b | 입력 전환: `ReadVertex`, `Submit`, `PlanCapacity`, `EstimateWorkingBytes`를 함께 Data로 바꾸고 PG 준비 검사를 없앤다. **임시 계약:** `-cut-validate 1`일 때만 `CuttingSystem`이 PG를 진단 전용 snapshot으로 따로 넘긴다(기존 `DiagnosticInputPose` 유지, 약 250줄) | `CutCommon.hlsli`, `GpuMeshCutter.*`, `CuttingSystem.*` | 구현 25분 + 빌드 2분 + 검증 15분 + 리뷰 5분 |
| CI-2 | 진단 입력을 Data+행렬 재구성으로 바꾸고 임시 PG snapshot·`PoseOffset`을 없앤다 | `GpuCutValidation.cpp::ValidateDiagnostics`, `CuttingSystem` | 구현 20분 + 빌드 2분 + 검증 10분 + 리뷰 5분 |
| CI-3 | 조각 PG를 Data 디코드로 채운다(`UpdateGenerated`). PG 크기와 offset은 Data에서 계산한다 | `PosedGeometry.*`(Generated 생성자), `PreparePoseCS` Generated 경로, `MeshComponent::Update` | 구현 20분 + 빌드 2분 + 검증 10분 + 리뷰 5분 |
| CI-4 | Pose 독자를 Data로 옮긴다 | `CutBoundsCS`, 진단 출력 검사·`ValidateSideDop`, `SkeletalBoundaryChecks::PairBoundary` | 구현 20분 + 빌드 2분 + 검증 15분 + 리뷰 5분 |
| CI-5 | Pose 생산자·버퍼·수명·회계를 없앤다 | `CutCommon::WriteVertex`, `PatchExtractCS`, `GeometryBuffers::{Pose,PoseOffsets,~GeometryBuffers,ReleaseGpuResourcesAfterCompletion}`, `FromGenerated`, 120/240 B 예산·계보·로그 | 구현 25분 + 빌드 2분 + 검증 15분 + 리뷰 5분 |

- 순서 제약: CI-4·CI-5는 `SkeletalPatchCopy.*`·`SkeletalBoundaryChecks.*`·`PatchExtractCS`를 건드린다. 이 파일들을 수정 중인 SP-7 세션이 커밋한 뒤에 시작한다. CI-1a~3은 그 파일들을 건드리지 않는다.
- 단계마다 사용자가 읽고 커밋한 뒤 다음 단계로 간다. 이 문서의 상태 줄과 README 행을 갱신한다.

CI-1b 구현: `ReadVertex`는 t0 Data를 디코드하고 section 행렬을 적용한다. `PlanCapacity`와 `Submit`은 같은 행렬 snapshot과 clip shader를 쓴다. t1 및 해당 root parameter는 제거했다. PG는 `snapshotDiagnostics`에서만 준비 검사·크기 회계·`PoseOffset`·`DiagnosticInputPose` 복사에 사용한다. CI-1a의 section frames 진단은 유지한다.

CI-1b 검증(MX450, D/R 빌드 36/48초 exit 0, `-debug 1`, D3D12 ERROR 0): Debug의 Cube 정상·Bunny 재절단·transform-checks·B 재절단·width 거절·동일 평면 3회·Bunny `-cut-count-first 1`·`-compute-pose 0`+transform-checks(진단 없음, 게시 1)·CesiumMan 패치 절단, Release의 Bunny y=-240·45도가 통과했다. cap 면적·loop·uncapped·게시 수는 CI-1a 실행과 같고 회귀표와 일치한다. section frames 최대 오차는 6e-8이다. 기본 장면 `-cut-scene 1`의 시작 직후 C++ 예외 종료는 기준 `df81e1e5` 빌드에서도 같아 이 작업과 무관한 기존 문제로 남긴다. 로그는 `%TEMP%/CI1a/`에 있다.

CI-1b 조사(코드 추가 없음): `ModelInstance::PauseAnimation`은 시간을 멈추지만 ModelViewer의 키·명령줄에는 연결되어 있지 않다. P 키는 물리만 정지한다. `-animation-step-ms 0`은 실제 deltaT를 쓴다. `-compute-pose 0`은 원본 메시의 `MeshComponent::Update`에서 PG 갱신을 생략하지만 CPU node 평가·section 행렬·bounds는 갱신한다. 이 옵션으로 진단을 끈 Static 절단과 `-cut-count-first 1`을 검증할 수 있다. `PosedGeometry::Update`는 본이 없고 node 행렬이 같으면 dispatch를 생략하며, `UpdateBaked`는 첫 복사 후 재사용한다. `-cut-validate 1`은 임시 PG snapshot이 필요하므로 PG 미준비 상태와 함께 쓸 수 없다.

CI-2 구현: `ValidateDiagnostics`의 부모 위치·보간·면적·부피는 `DiagnosticInputData`의 P를 `ParentPosition`에서 행 우선 3x4 `mul(SectionToComponent, float4(P,1))`로 재구성한다. 입력 법선을 읽는 검사는 없어 packed N 디코드는 추가하지 않았다. 출력 Data/Pose·법선·DOP 검사는 유지한다. 기존 `GPU cut section frames` 로그 형식으로 행렬 유한성, 3x3 여인수/행렬식 기반 역전치 일관성, Generated 및 cap 단위 행렬을 검사한다(허용치 1e-5). 위치·행렬 실패에 section/vertex/axis/actual/expected/허용치를 기록하며 행렬의 vertex는 `0xffffffff`, axis는 행 우선 원소 번호다. 진단 readback은 `ScopedReadback`으로 예외에도 해제한다. `diagnosticPose`·`DiagnosticInputPose`·PG 준비 검사·복사·크기 회계는 제거했다. C++/HLSL의 `PoseOffset`은 읽지 않는 0 예약 DWORD `ReservedPose`로 바꿔 160 B와 행렬 offset 64/112 B를 유지한다. `PlanCapacity`도 같은 예약 자리를 0으로 채운다. 호출자 검증: D/R 빌드·기존 회귀표·transform-checks·Generated 재절단·count-first와 `-compute-pose 0 -cut-validate 1`, D3D12 검증 및 진단 실패 시 매핑 해제 확인. 이 작업에서는 빌드·실행하지 않았다.

CI-2 검증(MX450, D/R 빌드 29/35초 exit 0, `-debug 1`, D3D12 ERROR 0): CI-1b와 같은 11개 시나리오가 같은 cap 면적·loop·게시 수로 통과했다. 새로 `-compute-pose 0 -cut-transform-checks 1 -cut-validate 1`(원본 PG 갱신 없음 + 진단)도 통과했다. section frames 검사(행렬 유한성·역전치 일관성) 최대 오차는 1.7e-8이다.

## 6. 완료 조건

공통(매 단계): Debug/Release 빌드 exit 0, SceneMathTests·CutPlaneTests 통과, `-debug 1`에서 새 D3D12 ERROR/CORRUPTION 0, `git diff --check` 통과.

결정 A 전체:
1. [StaticCutImplementation.md](StaticCutImplementation.md)의 `-cut-validate 1` 회귀표가 같은 결과다. 대상은 Cube 접촉/비교차/정상/동일 평면 3회/45도/B 재절단, Bunny y=0·y=-240, `-cut-transform-checks 1`, width=0.25 거절, 기본 장면 전체 후보다. 게시 수, loop 수, uncapped 수가 같아야 하고 cap 면적은 기존 허용오차 안이어야 한다.
2. `GpuMeshCutter`와 `CutCommon.hlsli`의 절단 계산에 PG 참조와 t1 입력이 없고(CI-2 `-cut-validate` 진단 포함, 충족), `CuttingSystem::Submit`에 PG 준비 검사가 없다(CI-1b 뒤 코드 확인). `-animation-step-ms 0`은 정지가 아니라 실제 deltaT를 쓰므로 쓰지 않는다. 실제 애니메이션 정지 수단과 "PG 갱신을 건너뛴 프레임" 확인 방법은 CI-1b 구현 전 Codex 조사 메모에서 정하고, 그 프레임에서 절단해도 결과가 같은지 확인한다.
3. CI-1a: node 행렬이 단위가 아닌 원본(transform-checks의 SourceFrame 회전·비균일 배율)에서 `Data×행렬` 위치가 PG 위치와 1e-5 이내로 같다. Generated 패치 입력은 단위 행렬로 같은 검사를 통과한다.
4. CI-1b: `-cut-count-first 1`로 preflight(`PlanCapacity`)와 실제 `Submit`이 같은 section 행렬을 쓰고 출력 용량이 일치한다.
5. 정밀도: Bunny 직교 재절단 3세대에서 출력 법선과 CI-1a 전 출력 법선의 최대 각도 오차를 기록한다. 법선 길이 검사(0.01)와 별개다.
6. 메모리: CI-5 뒤 `GeometryBuffers`에 Pose 자원이 없다. 논리 용량 감소(정점당 40 B)와 `EstimateWorkingBytes`의 64 KiB 단위 실제 할당 감소를 나눠 로그로 기록한다.
7. Skeletal: SP-7e의 `RunBreakChecks`·마스크 검사·`-skeletal-patch-offset-copy 1` SP-6 20행 결과가 CI-1a 전과 같다. 경계 오차는 같은 모델·joint·고정 프레임 번호(게시, 게시+60)에서 CI-4 전후로 비교하고 차이 0.01 mm 이내다(Pose→Data 이전이 오차를 바꾸지 않음).

결정 B: 별도 구현 없음. 강체 부착을 유지하고, 경계 오차는 SP-7 진단에서 측정·보고 항목으로만 다룬다. 방식을 바꾸는 것은 사용자가 따로 요청할 때만 새 결정으로 한다.

## 7. 남은 질문과 위험

- CesiumMan의 **게시 프레임** 오차 36 mm는 애니메이션 누적만으로 설명하기 어렵다. C snapshot과 게시 프레임의 시차, 첫 skin section E 선택 같은 별도 원인일 수 있다. SP-7 경계 진단에서 원인을 나눈다. 이 문서는 확정하지 않는다.
- packed N/T 정밀도 저하가 재절단 세대마다 누적되는지는 CI-1 검증에서 bunny 재절단 법선 오차로 확인한다.
- 렌더용 조각 PG는 남는다. Data VB로 직접 렌더하는 안은 후속 렌더러 결정으로 둔다.
