상태: 진행 중 — 단계 C·B 구현·검증 (커밋 전, 3절·4절 "결과"). A는 설계안. 근거는 `CutEnterCostReport.md`(`fc1f739` Release 실측), Codex 설계 검토 4건 반영 (7절)

# 스태틱 메시 Enter 비용 줄이기: 단계별 설계 (C → B → A)

## 1. 목표와 범위

`CutEnterCostReport.md`에서 bunny 하나의 Enter 프레임이 약 186 ms였다. 같은 결과를 내면서 중복 작업만 빼서,
`ConfirmAll`을 약 180 ms에서 약 80 ms로 줄인다.

- 절단 결과(조각의 정점·면·cap·에지 태그)는 지금과 비트 단위로 같아야 한다.
- 절단 알고리즘(`Slicer`), cap 정책, 스켈레탈 경로는 바꾸지 않는다. 스켈레탈 작업은 지금처럼 동작해야 한다.
- 해시맵을 배열로 바꾸는 일(보고서 안 D)과 절단을 다른 스레드로 옮기는 일은 이 문서의 범위가 아니다.

순서는 작고 독립적인 것부터 C → B → A다. 각 단계가 끝나면 사용자가 읽고 커밋한 뒤 다음 단계로 간다.

## 2. 단계 요약

| 단계 | 개념 | 바뀌는 곳 | 변경 규모 | Enter 절감 (추정) |
| --- | --- | --- | --- | --- |
| C | 스태틱 작업은 방금 world로 복사한 정점을 다시 변환하지 않는다 | `CutController::ConfirmJob` | 약 20줄 | 약 6.7 ms |
| B | 이미 검증된 기하를 다시 검증하지 않는다 (②③⑧⑨) | `ProceduralMesh`, `SliceProceduralMesh`, `SliceGeometry` 내부 경로, 테스트 | 약 150줄 (이동 함수·표시 테스트 포함) | 약 28 ms (A 뒤에는 ②가 로드로 옮겨 약 19 ms) |
| A | 스태틱 메시의 용접 결과를 로드 때 한 번 만들고, world가 그때와 같을 때만 Enter에서 복사한다 | `CutStaticMesh`, `UpdateStaticMeshCache`, `ExtractJob` | 약 120줄 | 약 79 ms − 복사 비용 (미측정, 수 ms로 추정) |

검증 번호 ①~⑨는 보고서 3.2절과 같다. 세 단계를 마치면 `ConfirmAll`은 약 180 − 6.7 − 18.8 − 79 + 복사 ≈ 80 ms로 추정한다.

### 단계별 예상 시간

| 일 | 시간 | 근거 |
| --- | --- | --- |
| 구현 | C 약 10분, B 약 40분, A 약 40분 | 변경 줄 수 기준 추정 |
| CPU 테스트 (`PlaneSlicerTests.cpp`) | 단계마다 수 분 | B·A만 해당. 이번 세션에서 재지 않았다 |
| Release 빌드 | 단계마다 수 분 | 이번 측정에서 증분 빌드가 몇 분 안에 끝났다 (정확히 재지 않음) |
| Debug 빌드 | 단계마다 수 분 | AGENTS.md 규칙 |
| Codex 리뷰 | 단계마다 2~5분 | AGENTS.md |
| 재측정 (`enter_probe.patch`) | 단계마다 약 5분 | 실행 약 15초 + 집계. 패치가 바뀐 함수와 겹치면 손으로 다시 넣어야 한다 |

## 3. 단계 C: 스태틱 `SkinPatch` 생략

### 현재

`ConfirmJob`은 `ExtractJob` 뒤에 항상 `SkinPatch` → `ProceduralMesh::UpdatePose`를 부른다.
스태틱 작업에서 `ReadStaticMesh`는 정점을 **지금 world 행렬**로 `Skinning::RigidPoint`를 써서 옮긴다.
`UpdatePose`는 같은 행렬로 같은 `RigidPoint`를 다시 계산하므로 결과가 비트 단위로 같다
(`ReadStaticMesh`의 주석 "첫 갱신과 비트 단위로 맞는다"). 정점 배열 복사 2번과 `ValidateVertices`만 추가로 든다.

### 바꿀 것

- `ConfirmJob`: `job.key.kind == CutJobKind::StaticMesh`이면 `SkinPatch`를 부르지 않는다.
- 대신 `cutWorld`에 쓸 world 행렬은 따로 구한다. 지금은 `SkinPatch`가 채운 `job.skinMatrices[0][0]`을 쓰므로
  스태틱 작업은 `BuildJobMatrices`(행렬만 구하고 정점은 건드리지 않는다)를 직접 불러 `job.skinMatrices`를 채운다.
- 단계 C만 있을 때는 추출이 항상 이번 프레임 world로 하므로 건너뛰어도 항상 같다. 단계 A에서는 캐시 world와 지금 world가 같을 때만
  캐시를 쓰고(5절), 다르면 지금 추출 경로를 쓰므로 이 규칙이 그대로 유지된다.
- `UpdatePose`를 안 부르므로 그 메시의 `m_VertexVersion`은 오르지 않는다. 이 메시는 곧바로 `SliceProceduralMesh`에 들어가고,
  성공하면 조각이 `SetRenderData`에서 새 버전을 받으므로 업로드에는 영향이 없다.

### 확인

- 12회 재측정에서 `SkinPatch` 구간이 0이고, 조각 정점·면 수가 보고서 2절 값과 같다.
- 비트 단위 동일성: 개수만이 아니라 **값**을 비교한다. 같은 프레임에서 `SkinPatch`를 한 경우와 안 한 경우의 절단 결과를 만들어,
  정점 위치·origin, corner 법선·UV, 면, cap 기록, 에지 태그가 모두 같은지 임시 코드로 한 번 대조한다(커밋하지 않음).

### 결과 (2026-10-23, Release, 커밋 전)

- 구현: `CutController::ConfirmJob`에서 스태틱 작업은 `SkinPatch` 대신 `BuildJobMatrices`로 행렬만 채운다(대상·source 없음, 행렬 실패, 빈 행렬은
  `InvalidInput`). 스켈레탈은 그대로 `SkinPatch`. 변경 +26/−2줄. Debug·Release 빌드 성공. Codex 코드 리뷰: 지적 없음.
- 값 대조 (12회, 임시 코드): 같은 Enter에서 추출한 메시를 복사해 `UpdatePose`를 적용한 것과 적용하지 않은 것을 비교했다.
  기하 정점 배열, 렌더 정점 배열(위치·법선·UV·색 전체), bounds 필드가 **12회 모두 비트 단위로 같았다**. 절단 입력이 같으므로 절단 결과도 같다
  (조각 정점·면·렌더 정점 수와 루프 수도 보고서 2절과 같다). bounds를 구조체 통째로 `memcmp`하면 끝의 `bool` 뒤 패딩 때문에 다르게 나오므로 필드별로 비교했다.
- 시간: `SkinPatch` 6.7 ms → 행렬 구하기 0.005 ms. Enter 프레임 전체는 대조 코드(약 10.5 ms)를 빼면 중앙값 약 175 ms로, 이전 약 186 ms(`ConfirmAll` 180 ms)와
  비교해 줄어든 양은 회차 간 흔들림(절단 구간이 ±4 ms) 안에 있다. 단계별 구간 값으로 절감을 판단한다.
- 원본: `Deliverables/측정/enter_probe_stepC.log`(시간), `enter_probe_stepC_verify.log`(필드별 대조).
- `m_Frame.jobVertices`·`jobSkin` 통계에 스태틱 Enter 몫이 더 이상 들어가지 않는다. 실제로 스키닝하지 않으므로 의도한 결과다.
- 화면: 조각이 원래 자리에서 법선 방향으로 벌어진 모습이 이전과 같다.

## 4. 단계 B: 검증 계약 정리

### 지금 검증하는 곳과 판단

| 번호 | 위치 | 판단 |
| --- | --- | --- |
| ① | `BuildMeshGeometry` 끝 | **유지**. 원본 메시의 유일한 검증 |
| ② | `ProceduralMesh::Build` → `SetTopology` | **뺀다**. ①과 같은 기하 |
| ③ | `Slicer::Run` 입력 | **조건부로 뺀다**. 아래 "검증된 상태" 참고 |
| ④⑤ | `Slicer::Run` 출력 (cap 전) | **유지**. 가는 조각을 거절하는 역할이 있고, `ExtractCutLoops`의 전제다 |
| ⑥⑦ | `BuildCap` 끝 | **유지**. cap을 붙인 결과의 검사 |
| ⑧⑨ | `SliceProceduralMesh` → `SetTopology` ×2 | **뺀다**. `SliceGeometry`가 `Ok`면 조각은 ④⑤(cap이 없을 때) 또는 ⑥⑦(cap이 있을 때)를 통과했다 |

⑧⑨의 근거 확인: ④⑤ 뒤에 `Slicer::Run`이 조각을 바꾸는 곳은 두 군데다.
`CapWhatFits`는 cap을 붙인 결과를 ⑥⑦로 검증한 뒤에만 조각을 바꾸고, 실패하면 조각을 그대로 둔다.
cap 없는 사슬의 태그를 `Cut` → `OpenSurface`로 바꾸는 것은 경계 에지의 종류만 바꾸며, `OpenSurface`는 검증이 허용하는 경계 종류다.
`ExtractCutLoops`는 기하를 바꾸지 않는다. 그 뒤 `SliceProceduralMesh`가 이번 cap 기록에 `materialSlot`·`inLastSection`을 적지만,
`ValidateMeshGeometry`는 이 값을 검사하지 않으므로 재검증 결과가 달라지지 않는다 (Codex 검토로 확인).

### 검증된 상태 (③)

`ProceduralMesh`의 `m_Geometry`가 "마지막 검증 이후 바뀌지 않았다"는 표시를 둔다. 표시는 성공한 변경과 함께만 바뀐다
(지금 구현은 검사가 끝난 뒤에 상태를 한꺼번에 바꾸므로, 실패한 호출은 이전 값을 그대로 둔다).

| 연산 | 표시 |
| --- | --- |
| 기본 생성 | false |
| 성공한 `SetTopology`, 검증된 기하 채택(`AdoptValidatedTopology`) | true |
| 성공한 `UpdatePose` | false. 스키닝은 정점을 움직여 면적이 ε² 아래로 줄 수 있고, `UpdatePose`는 면적·축퇴를 다시 보지 않는다 |
| 성공한 `SetMesh`, `Clear` | false. 기하를 비운다 (`ValidateMeshGeometry`는 면이 없는 기하를 거절하므로 빈 기하가 true일 수 없다) |
| `SetSectionSplit` | 그대로. 렌더 데이터만 다시 만들고 기하는 바꾸지 않는다 |
| 복사 | 기하와 함께 복사 |
| 이동 | 대상이 이어받고, 이동된 원본은 false |

- 지금 `ProceduralMesh`는 복사·이동을 기본 생성 함수에 맡긴다. bool을 그냥 더하면 이동된 원본에 true가 남으므로,
  이동 생성자·이동 대입에서 원본을 false로 만든다(이동만 직접 쓰고 복사는 기본으로 둔다).
- 표시는 **기본 epsilon(`kDefaultEpsilonScale`)으로 검증했다**는 뜻이다. 검증 결과가 epsilon에 따라 달라지기 때문이다.
- `SliceProceduralMesh`는 표시가 켜져 있으면 ③을 건너뛰게 한다. 이 생략은 공개 `SliceOptions`에 넣지 않는다.
  공개 옵션으로 두면 다른 epsilon으로 자르는 호출자도 생략을 켤 수 있어 계약이 깨진다.
  `Slicer`에 내부 전용으로 넘기고(`SliceGeometry`의 내부 오버로드나 `Slicer` 생성 인자), `SliceProceduralMesh`는 자기가 쓰는 epsilon이
  `kDefaultEpsilonScale`일 때만 켠다. 이때도 `ComputePieceRadius`, `MakePlaneBasis` 등 나머지 준비는 그대로 한다.

결과: 단계 C 이후 스태틱 작업은 `UpdatePose`를 거치지 않으므로 ③이 빠진다. 스켈레탈 작업은 지금처럼 ③을 한다.

### 검증된 기하를 받는 경로 (②⑧⑨)

공개 `SetTopology`는 외부 입력을 받으므로 계속 검증한다. 검증을 건너뛰는 경로는 공개하지 않는다.

- `ProceduralMesh`에 private `AdoptValidatedTopology(MeshGeometry)`를 둔다. 렌더 메시 생성(`BuildRenderMesh`, `SetRenderData`)은 `SetTopology`와 같다.
- 호출자는 `ProceduralMesh::Build`(①을 통과한 기하)와 `SliceProceduralMesh`(`Ok`인 결과)뿐이다. `SliceProceduralMesh`는 자유 함수이므로 `friend`로 연다.
- Debug 빌드에서는 **검증을 실제로 건너뛰는 두 자리**(`AdoptValidatedTopology`, ③ 생략)에서만, 같은 기본 epsilon으로
  `ValidateMeshGeometry`를 돌려 `Ok`인지 assert한다. 계약이 깨지면 Debug에서 바로 드러나고, Release에서만 비용이 빠진다.
  잘못된 입력을 정상적으로 거절해야 하는 경로(공개 `SetTopology`, ①, ④~⑦)는 assert로 바꾸지 않고 지금처럼 실패를 돌려준다.
- 렌더 데이터 검사(`SetRenderData`의 `ValidateVertices`·`ValidateTopology`)는 `AdoptValidatedTopology`에서도 그대로 한다.

### 테스트 (`PlaneSlicerTests.cpp`)

- `SliceProceduralMesh` 결과 두 조각에 `ValidateMeshGeometry`를 직접 불러 `Ok`인지 본다 (cap 있음·없음, 구멍 있는 cap, 재절단 `TestRecut`).
- 검증된 표시: 위 표의 각 연산 뒤 값(기본 생성, `SetTopology`, `UpdatePose`, `SetMesh`, `Clear`, `SetSectionSplit`, 복사, 이동한 원본과 대상,
  실패한 `SetTopology` 뒤 이전 값 유지). 표시를 테스트에서 읽어야 하므로 읽기 함수(`IsGeometryValidated` 같은 이름)를 하나 연다.
- 꺼진 상태(`UpdatePose` 뒤)에서 자른 결과가 지금과 같다.
- 기존 `TestProceduralMeshOwnsGeometry`, `TestPublicProceduralMeshPipeline`은 갱신 동작만 보므로, 위 표시 검사는 새 테스트로 더한다.
- 기존 테스트 전체 통과 (현재 9,470 checks).

### 확인

- 재측정: 양쪽 조각 모두 cap이 한 번에 붙는 경로(보고서의 bunny)에서 `ValidateMeshGeometry`가 Enter당 **9회 → 5회**(①④⑤⑥⑦)로 준다.
  단계 A 뒤에는 ①도 로드로 옮겨 **4회**가 남는다. `CapWhatFits`가 묶음마다 다시 시도하는 경로에서는 `BuildCap`이 더 불려 횟수가 늘어난다.
- Debug 빌드에서 절단 12회 동안 assert가 없다.

### 결과 (2026-10-23, 커밋 전)

- 구현 (Sonnet 서브에이전트): `ProceduralMesh`에 `m_GeometryValidated`·`IsGeometryValidated`, private `AdoptValidatedTopology`(공개 `SetTopology`는
  검증 뒤 이것을 부른다), 이동 생성·이동 대입, `SliceProceduralMesh` friend. `Slice.cpp`는 파일 내부 `SliceGeometryImpl`과 `Slicer`의 `inputValidated` 인자로
  ③을 건너뛴다(공개 `SliceGeometry`·`SliceOptions`는 그대로). Debug 전용 assert는 건너뛰는 두 자리에만 있다.
- Codex 코드 리뷰: 정확성 지적 1건. 이동 생성자의 `noexcept`가 틀렸다(MSVC `unordered_map` 이동 생성은 버킷을 할당한다, STL 14.44 `xhash`에서 확인).
  이동 생성자는 `noexcept` 없이 "기본 생성 + 이동 대입"으로 바꾸고, 이동 대입은 원본의 표시를 멤버를 옮기기 **전에** 끈다. 이 수정은 Codex 재리뷰를 받지 않았다.
  테스트 보완 제안을 받아 거짓 표시의 복사·이동, 거절된 `UpdateVertices`·실패한 `SetMesh`의 표시 유지, 자세로 축퇴한 기하를 ③이 다시 거절하는 테스트
  (`TestPosedDegenerateInputIsRejected`)를 더했다. `CapWhatFits`의 묶음별 재시도 경로를 직접 시험하는 테스트는 아직 없다.
- CPU 테스트 9,570 checks, 0 failed (`/MDd`, assert 켜짐). Debug·Release 빌드 성공.
- 재측정 (Release, 12회, 단계 C 포함): `ValidateMeshGeometry`가 **Enter당 5회**(①④⑤⑥⑦), 합계 중앙값 약 29.6 ms(이전 57.6 ms).
  `ConfirmAll` 중앙값 **약 140 ms**(측정 보고서 약 180 ms), Enter 프레임 중앙값 약 148 ms(이전 약 186 ms). ③은 0.002 ms, 조각 채택(⑧⑨ 자리)은 렌더 데이터 생성만 남아 6.5 ms.
  조각 정점·면·루프 수는 보고서 2절과 같다.
- Debug 빌드 12회: 모두 `Ok`, assert 없음(앱이 끝까지 실행됐다).
- 원본: `Deliverables/측정/enter_probe_stepB.log`(Release), `enter_probe_stepB_debug.log`(Debug).

## 5. 단계 A: 스태틱 용접 결과 캐시

### 현재

`ExtractJob`은 Enter마다 `CopyProceduralMeshFromStaticMesh`로 원본 버퍼를 읽고(`ReadStaticMesh` 24.9 ms) 용접·검증해
`ProceduralMesh`를 만든다(`Build` 53.6 ms). 스태틱 메시는 로드 뒤 바뀌지 않는다.
`UpdateStaticMeshCache`도 로드 직후 같은 메시를 `ReadStaticMesh`로 한 번 읽고, pick용 로컬 위치와 인덱스만 남기고 나머지는 버린다.

### 바꿀 것

`CutStaticMesh`에 Enter용 캐시를 더한다.

| 필드 | 내용 |
| --- | --- |
| `mesh` | `std::shared_ptr<const ProceduralMesh>`, 용접·검증을 마친 메시 |
| `source` | `std::shared_ptr<const MeshSource>` (지금 `ExtractJob`이 `job.source`에 넣는 것. `CutJob::source`도 이미 const다) |
| `weld` | `WeldStats` |
| `meshWorld` | 캐시를 만든 world 행렬 |
| `meshStatus`, `meshError` | 실패 기록 (실패해도 pick은 그대로 동작) |

- `UpdateStaticMeshCache`: 이미 읽은 `ReadStaticMesh` 결과(`copy`, `source`)로 `ProceduralMesh::Build`까지 한다.
  pick용 `positions`·`indices`는 지금처럼 뽑는다. 원본을 두 번 읽지 않으므로, 로드 때 늘어나는 비용은 `Build` 약 54 ms다(메시당, 첫 Update 프레임).
- `ExtractJob`: 스태틱 작업이고 캐시가 있으며 **지금 world가 `meshWorld`와 비트 단위로 같을 때만** `*cache.mesh`를 조각용
  `ProceduralMesh`로 복사하고, `source`·`weld`·면 수를 캐시에서 가져온다.
  캐시가 없거나, 실패했거나, world가 다르면 지금 경로(`CopyProceduralMeshFromStaticMesh`)로 간다.
- 단계 C와의 연결: 캐시를 쓴 경우 정점은 이미 지금 world에 있으므로 `SkinPatch`를 건너뛴다. 지금 경로로 추출한 경우도 단계 C 그대로다.

### world가 다를 때 `SkinPatch`로 맞추지 않는 이유

처음 설계는 "world가 다르면 캐시를 복사한 뒤 `SkinPatch`로 지금 world에 맞춘다"였다. 이는 지금 결과와 같지 않다 (Codex 검토).

- **법선**: `ReadStaticMesh`는 위치와 함께 corner 법선도 지금 world의 inverse-transpose로 옮긴다.
  `UpdatePose`는 기하·렌더 위치, bounds, 정점 버전만 바꾸고 법선은 그대로 둔다. 회전하거나 비균일 스케일이 바뀐 뒤에는 법선이 달라진다.
- **경계 태그**: `BuildMeshGeometry`는 경계 에지를 `UnweldedSeam`으로 볼지 정할 때 posed(world) 위치의 비트 일치도 본다.
  `UpdatePose`는 태그를 다시 계산하지 않는다. 변환과 float 반올림으로 위치의 일치 관계가 바뀌는 입력에서는 태그도 달라질 수 있다(조건부).

그래서 첫 구현은 "같은 world에서만 재사용, 다르면 지금 추출"로 좁힌다. 이 프로젝트의 스태틱 메시는 로드 뒤 움직이지 않으므로
보통은 캐시를 쓴다. 움직이는 스태틱 메시까지 빠르게 하려면, 로컬 공간에서 자르고 평면을 로컬로 옮기는 언리얼 방식이 따로 필요하다.

### 모델 교체와 캐시 수명

`InitializeTargetModelData`는 `staticMeshes`를 비우지만, 지금 이 함수를 부르는 곳은 `RegisterInstance`뿐이다.
`UpdateTarget`은 인스턴스의 지금 모델을 읽기만 하고 등록 때의 `target.model`과 비교하지 않으며, `InitCutStaticMesh`는
`staticMeshesInitialized`가 켜져 있으면 바로 돌아간다. 따라서 **등록된 인스턴스의 모델이 바뀌어도 캐시는 다시 만들어지지 않는다.**
이는 pick 캐시와 작업에도 이미 있는 제한이다.

이번 단계에서는 계약으로 정한다: **등록된 `ModelInstance`의 모델을 바꾸려면 먼저 `UnregisterInstance`하고 다시 `RegisterInstance`한다.**
`RegisterInstance` 주석과 `CutTarget::model` 주석("바뀌면 작업을 모두 지운다")을 이 계약에 맞게 고친다.
지금 `ModelViewer`(`Startup`, `AddStaticInstance`, `Cleanup`)는 등록 → 사용 → 해제만 하므로 이 계약을 지킨다.
모델 변경을 자동으로 감지하는 일(작업 정리, 캐시 재생성)은 필요해지면 따로 설계한다.

### 주의

- **버전 번호**: `ProceduralMesh`를 복사하면 `m_VertexVersion`·`m_TopologyVersion`도 같은 값으로 복사된다. 렌더러(`ProceduralMeshRenderer::Draw`)는
  **메시 주소별** 항목에서 버전을 비교하므로, 다른 주소에 있는 같은 데이터의 복사본이라는 것만으로는 충돌하지 않는다.
  지금 Enter 흐름에서는 복사본이 곧바로 `SliceProceduralMesh`에 들어가 성공하면 새 버전을 받고, 실패하면 작업에 저장되지 않는다.
  캐시 자체는 그리지 않는다.
- **메모리**: 기하 정점(bunny 15,258개)과 corner·렌더 정점(91,014개)을 나눠 본다. `BuildMeshGeometry`는 용접된 기하 정점을,
  `BuildRenderMesh`는 corner마다 렌더 정점을 만든다. 스킨 표(`Skinning`)는 복사본과 공유되지만, 기하·렌더 배열과 에지 태그 해시맵은 깊게 복사된다.
  구현 뒤 **상주 캐시 크기**와 **Enter 동안 생기는 임시 복사본 크기**를 따로 재서 적는다.
- **복사 비용**: Enter 때 `ProceduralMesh` 전체(기하 + 렌더 데이터 + 에지 태그 해시맵)를 복사한다. 측정에서 조각 절반 복사(`BuildCap`의 `copyPiece`)가
  1.5 ms였으므로 수 ms로 추정한다. 재측정에서 `ExtractJob` 구간으로 확인한다.
  복사를 아예 없애려면 입력을 const로 받고 결과만 새 메시에 쓰는 절단 함수가 필요하다. 이는 Unreal `SliceProceduralMesh`(inOut)와 모양이 달라지므로 이번에는 하지 않는다.

### 확인

- 재측정: `ExtractJob` 약 79 ms → 복사 시간만, `ReadStaticMesh`·`BuildMeshGeometry`는 Enter 구간에서 사라진다.
- 로드: 첫 Update 프레임의 `InitCutStaticMesh` 시간을 한 번 재서 문서에 적는다.
- Enter → Ctrl+Z → Enter를 반복해도 결과가 같다 (캐시가 절단으로 바뀌지 않는다: `shared_ptr<const ...>`에서 복사한다).
- 캐시 경로와 지금 경로의 절단 결과를 **값으로** 대조한다: 정점 위치·origin, corner 법선·UV, 면, cap 기록, 에지 태그 (임시 코드, 커밋하지 않음).
- world가 바뀐 경우(임시로 bunny를 회전·비균일 스케일한 뒤 Enter) 캐시를 쓰지 않고 지금 경로로 가며, 결과가 지금과 같다.
- 절단이 실패한 뒤 평면을 옮겨 다시 Enter해도 캐시가 그대로이고 결과가 정상이다.

## 6. 다음에 남는 것

세 단계 뒤 Enter는 약 80 ms로 추정한다. 60 Hz 기준 아직 4~5프레임이다. 남는 큰 항목은 절단 본작업(약 42 ms)과
남은 검증 ④~⑦(약 18.5 ms), 렌더 데이터 생성이다. 다음 후보는 보고서의 안 D(`CollectEdgeUsage`·corner 매핑 해시맵을 배열로)와,
렌더 데이터 생성을 Enter 다음 프레임으로 미루거나 다른 스레드로 옮기는 것이다. 이 둘은 재측정 결과를 본 뒤 정한다.

언리얼 대응: 언리얼은 `CopyProceduralMeshFromStaticMeshComponent`를 호출자가 원하는 시점에 한 번 하고, 이후 `SliceProceduralMesh`는
그 컴포넌트를 바로 자른다. 단계 A는 이 "복사는 한 번, 절단은 여러 번" 구조에 맞추는 것이다.

## 7. Codex 설계 검토 반영 (2026-10-23)

Codex가 이 문서를 코드와 대조해 읽었다(빌드·실행 없이). 지적 4건과 반영한 곳:

| 심각도 | 지적 | 반영 |
| --- | --- | --- |
| 높음 | world가 다를 때 캐시 + `SkinPatch`는 법선(그리고 조건부로 경계 태그)이 지금 결과와 다르다 | 5절: 같은 world에서만 캐시, 다르면 지금 추출 |
| 높음·조건부 | 모델이 바뀌면 캐시가 다시 만들어진다는 근거가 없다 (`InitializeTargetModelData`는 등록 때만 불린다) | 5절: 교체는 등록 해제 후 재등록이라는 계약을 문서·주석에 둔다 |
| 중간 | 검증 표시의 수명 규칙이 빠져 있다 (`SetMesh`·`Clear`·이동, epsilon 전제) | 4절: 연산별 표, 이동한 원본은 false, 생략은 공개 옵션이 아닌 내부 경로이고 기본 epsilon일 때만 |
| 낮음 | 검증 횟수(9 → 6)가 틀렸고, 메모리 설명이 기하 정점과 corner를 섞었다 | 4절: 9 → 5회(A 뒤 4회). 5절: 기하 15,258 / corner 91,014로 나누고, 상주·임시 크기를 따로 잰다 |

Codex가 맞다고 확인한 것: 같은 world에서 C의 비트 동일성과 `cutWorld` 처리, ②의 중복, ③이 표시가 켜졌을 때만 중복이라는 점,
⑧⑨ 생략 근거, private 채택 함수와 `friend`, 생략하는 자리에서만 쓰는 Debug assert, 복사본의 버전 번호가 지금 흐름에서 문제를 만들지 않는다는 점,
`MeshSource` 공유의 안전성, 순서 C → B → A. 비트 동일성은 실제 데이터로 대조하지 않았으므로 3절·5절의 값 대조로 확인한다.
