상태: 진행 중 — 6a~6e·7a 완료, 7b 이후 미구현 (2026-10-28)

# Static 절단 조각 물리 (요구사항 v4)

## A. 목적과 범위

Static 메시를 절단한 뒤 조각마다 rigid body 물리를 적용하고, 바닥 위에서 낙하·정지시킨다.

- 물리: Jolt 5.6.0 CPU rigid body. 렌더 절단은 기존 GPU 경로를 유지하며 매 프레임 정점 readback은 하지 않는다.
- 단위: 1 unit = 1 m. Unreal은 cm이므로 Unreal의 길이 기준값은 ×0.01로 환산한다(면적은 ×1e-4, 거리 제곱은 ×1e-4).
- 범위 밖은 R13, 향후 호환은 R12에 둔다.

## B. 사용자 결정

| 항목 | 결정 |
| --- | --- |
| 소유 | 한 GameObject를 유지하고 컴포넌트(MeshComponent) 단위로 편집한다 |
| 시작 | 모든 Static 물체가 시작부터 낙하한다 |
| 절단 대상 | 정지(sleep) 물체만 자른다 |
| 충돌 형상 | 출처·생성 시점·기즈모 동작은 Unreal과 동일하게 맞춘다 |

## C. 구조 개요

표현을 두 개로 나눈다.

| 표현 | 저장 위치 | 규모 | 절단 | 사용처 |
| --- | --- | --- | --- | --- |
| 렌더 메시 | GPU 버퍼 | 삼각형 수천~수만 | GPU | 화면 출력 |
| 충돌 convex | CPU 메모리 | 정점 수십 | CPU | Jolt 충돌·질량 |

Unreal 대응은 BodySetup.AggGeom.ConvexElems(CPU)와 Chaos(CPU)다.

```text
로드: CPU 정점 → 18-DOP → Model에 보관
MeshComponent 생성: Model hull 복사 (× SourceFrame⁻¹)
Body 생성: scale을 정점에 반영 → Jolt ConvexHullShape → Dynamic body
매 프레임: Step → body 원점 T/R → 컴포넌트 Transform 기록 → VS의 Wc
절단: 같은 로컬 평면으로 GPU 렌더 메시와 CPU convex를 각각 자르고
      transaction으로 함께 교체
```

의존 방향: `Model → MiniCollision`(Jolt 무관), `PhysicsWorld → MiniCollision + Jolt`.

## D. Unreal 근거

기준은 UE 5.3 GitHub 미러(`https://raw.githubusercontent.com/chenyong2github/UnrealEngine/5.3/`)다. 아래는 소스에서 확인한 사항이며, 미확인은 명시한다.

| 항목 | 확인된 내용 |
| --- | --- |
| import 기본 충돌 | FBX는 사용자 지정 충돌이 없고(`!bCustomizedCollision`) `bAutoGenerateCollision`이 켜져 있으며 BodySetup이 존재하는 LOD0에서 `KDopDir18`로 `GenerateKDopAsSimpleCollision`을 호출한다(FbxStaticMeshImport.cpp). Interchange는 `bImportCollision` 기본 true이고 사용자 지정 충돌이 없으면 `GenerateKDopCollision`(KCount 18)을 호출한다(InterchangeStaticMeshFactory.cpp). 생성기는 정점을 18방향에 투영한 최댓값에 MinSize 0.1 cm를 더한 평면들로 convex를 만들고, `CreateFromModel` → `AddConvexPrim` → `HullFromPlanes`로 ConvexElem을 만든다(GeomFitUtils.cpp; BSP 세부는 미확인). — 1차 조사에서 확인, 교차 검증 미완료. |
| 복사 | `CopyProceduralMeshFromStaticMeshComponent`는 ConvexElems의 VertexData만 변환 없이 복사한다(Box/Sphere 제외) |
| 절단 | `SliceProceduralMesh`는 평면을 컴포넌트 로컬로 변환한다. ElemBox로 `BoxPlaneCompare`를 호출해 한쪽이면 통째로 넘기고, 걸치면 `SliceConvexElem`(GetPlanes ≥4 + 뒤집은 절단 평면 → `HullFromPlanes`)을 쓴다. 정점 ≥4만 채택한다. B는 같은 Outer의 새 컴포넌트이며 SimulatePhysics·속도는 설정하지 않는다 |
| HullFromPlanes | `KAggregateGeom.cpp`. 평면별 큰 다각형을 다른 평면들로 clip, snap(거리 제곱 비교), `MIN_HULL_VERT_DISTANCE` 0.1 cm 병합. 실패 조건은 면적 <0.001, 정점 <4, 공선, 공면 (1차 조사에서 확인, 교차 검증 미완료) |
| 질량 | `BodyUtils.cpp`. Density 1 g/cm³, RaiseMassToPower 0.75, MassScale 1, 최소 0.001 kg. 조각 질량의 합은 부모와 같지 않다. 마찰 0.7, 반발 0.3, 중력 −980 cm/s² |
| 시뮬레이션 | `bSimulatePhysics` 기본 false, Movable 필요. 단일 body를 simulate하면 KeepWorldTransform으로 detach |
| 이동 | `SetBodyTransform`이 동적 body를 `SetGlobalPose`로 순간이동하고 속도는 명시적으로 리셋하지 않는다. Chaos 내부 속도 처리는 미확인 |
| 소유 선례 | Slice(같은 Outer 새 컴포넌트), BreakConstraint(같은 SkeletalMeshComponent 안 분리 body), LeaderPose(한 Actor 안 pose 공유), Chaos GeometryCollection(한 컴포넌트 다수 입자). 선례일 뿐 필수 구조의 증명은 아니다 |

우리 정책(Unreal에 없음): convex의 빈 쪽은 body 없는 조각, 실패 시 transaction rollback, 시작 시 전체 simulate, sleep 대상만 절단.

## E. 요구사항 v4

### R0 소유

- 절단 후에도 GameObject는 1개다(A는 교체, B는 컴포넌트로 추가).
- 결과는 (ObjectId, MeshId) + revision + status 목록으로 소비한다(`DrainResults` 형태). LastOther는 진단용이다.
- 좌표 기준은 `MeshComponent::GetWorldTransform`이다.

### R1 바닥

- 별도 GameObject, Static, `cube.glb`를 확대 표시, Jolt Static Box.
- 시작 정규화·배치에서 제외하고, 모델 bounds로 크기와 높이를 정한다(기본 평면 y=0, 4×4가 멈춘 물체를 지나도록).
- NotCuttable/NotEditable를 `IsEligible`·직접 Request·기즈모 pick·Undo에서 검사한다.

### R2 충돌 자산

- 로더가 geometry 바이트를 보관한 채 node/mesh metadata를 읽고, `EvaluateModelPose`·matrixIdx로 메시별 model-local 18-DOP를 CPU에서 1회 계산한 뒤 업로드한다. Model에 공유 보관한다.
- 충돌 hull과 허용오차(KDopMinSize·병합 0.001·snap)는 메시 로컬(asset) 단위로 적용된다. 컴포넌트 scale은 body 생성 시 정점에 반영되므로 월드 기준 오차는 scale에 비례한다. Unreal도 asset 단위(cm)로 충돌을 만들고 scale을 나중에 적용하므로 같은 성질이다. 문서의 m 표기는 1 asset unit = 1 m 가정의 수치다.
- 초기 hull snap: `snapDistSq = 0.000015·R_m`(m², 거리 제곱 비교; Unreal 0.0015·R_cm cm² 환산).
- 18개 반공간 교집합과의 동일성은 정점 병합 전 다면체에만 적용한다. 최종 hull은 병합·snap이 반영된 근사이며 형상·부피 허용오차를 명시한다.
- 정점 병합(0.001 m) 때문에 얇은 형상의 최종 hull 부피는 병합 전보다 줄 수 있다(두께 0.012 m 상자에서 약 10%, Unreal 0.1 cm 병합과 같은 성질). 일반 형상의 허용오차는 1%.
- 노드 애니메이션 메시 판별은 메시별로 한다(모델에 애니메이션이 있다는 것만으로 단정하지 않는다). 스킨·노드 애니메이션 메시는 충돌 없음·비시뮬레이션이다.
- 컴포넌트 충돌:
  - 원본 revision: 선택된 source section의 hull × SourceFrame⁻¹(항등이면 그대로 복사).
  - 생성 revision: 부모의 잘린 결과를 계승한다(원본 Model hull로 재생성 금지).
  - SourceFrame 변경 시 현재 조각 hull을 재기준화한다.
  - render revision ↔ collision 대응을 검증한다.

### R3 convex 절단

- GPU 평면(`transpose(world)·WorldPlane`, 비정규화)은 그대로 둔다. CPU는 같은 로컬 평면 사본의 법선·상수항을 함께 정규화한다.
- 비균일 scale에서 Unreal `InverseTransformVectorNoScale`과 다르다. GPU 일치를 우선한다(문서화된 차이).
- `BoxPlaneCompare`/`SliceConvexElem`/`HullFromPlanes`를 이식한다.
- 아래 허용오차도 메시 로컬(asset) 단위로 적용되며 1 asset unit = 1 m 가정의 수치다(R2 단위 계약 참조).
- 단위 환산:

| 값 | Unreal(cm) | 이 프로젝트(m) |
| --- | --- | --- |
| 기본 snap | 1e-8 cm² | 1e-12 m² |
| 정점 병합 | 0.1 cm | 0.001 m |
| 최소 면적 | 0.001 | 1e-7 m² |
| LOCAL_EPS | 단일 값 | 무차원(각도) 1e-8과 거리 1e-10 m로 분리 |

- 빈 쪽도 게시하되 body는 없다(우리 정책). Unreal은 section 삼각형 충돌을 만들 수 있으므로 차이로 기록한다.
- 유한 사각형 규칙은 유지한다.

### R4 body·질량·transaction

- 충돌이 있는 MeshComponent마다 Jolt body 1개(convex가 여러 개면 compound).
- 질량은 Unreal 공식을 쓰고, 관성은 shape에서 계산한 뒤 질량에 맞춰 스케일한다.
- 마찰 0.7, 반발 0.3, 중력 −9.8 m/s².
- pose는 COM이 아닌 body 원점(`GetPosition`/`GetRotation`)을 쓴다. scale은 정점에 반영하며 렌더는 T·R·S를 유지한다.
- 게시 transaction:
  1. 준비: GPU A/B, CPU convex A/B, 미등록 Jolt body.
  2. 검증.
  3. step/query와 섞이지 않는 경계에서 교체: 부모 body 제거, A/B 추가, 메시 교체.
  4. 실패(용량·cook·stale·예산) 시 준비 자원을 회수하고 부모를 유지한다.

### R5 시작

- 적격 Static 메시는 전부 simulate한다.
- simulate 컴포넌트는 KeepWorldTransform으로 WorldIndependent로 전환한다. TRS로 표현할 수 없으면(전단) body 생성 전에 거절한다.
- TransformMode와 simulate 여부는 독립 상태다.

### R6 세대

- 두 세대를 분리한다.
  - `EditGeneration`: geometry·scale·source frame·명시적 편집·teleport에서 증가. owner 편집은 EditGeneration을 올리지 않으며, Attached(비시뮬레이션) 대상은 기존 world 행렬 비교로 검출한다.
  - `PhysicsPoseGeneration`: T/R 변경에서 증가. 같은 pose를 다시 기록하면 증가하지 않는다.
- `CuttingSystem::Update`·`SubmitPending`·`PhysicsSystem::IsCurrent`는 EditGeneration만 비교한다.
- simulate 대상은 제출 시 (body, PhysicsPoseGeneration, sleeping)을 기록하고 게시 직전에 재확인한다. 깨어났거나 pose가 바뀌었으면 취소하고 부모를 유지한다.
- 비simulate 대상은 기존 world 행렬 엄격 비교를 유지한다.

### R7 절단 자격

- sim on: simulate 메시는 sleep일 때만 자른다. body 없는 조각·비대상 메시는 기존 GPU 경로를 쓴다.
- sim off(`-physics-sim 0` 또는 초기화 실패): 전부 기존 경로.
- 거절 문구는 "Wait - object moving". 속도 전달은 없다.

### R8 편집

- 대상은 `Owner | Mesh(ObjectId, MeshId)`다.
- WorldIndependent 컴포넌트(simulate 여부 무관)를 pick하면 Mesh, Attached는 Owner다. Owner 기즈모는 Attached만 이동한다.
- simulate 컴포넌트의 Mesh 기즈모는 Unreal `SetBodyTransform`과 같다. 드래그 갱신마다 body를 순간이동하고(속도 명시 리셋 없음), scale 편집은 shape·질량을 재생성한다(실패하면 렌더 scale도 바꾸지 않는다).
- transform·body·세대를 함께 바꾸는 공통 적용 경로를 하나만 둔다.
- History는 대상 종류·ObjectId·MeshId·TransformMode·이전/이후 world TRS를 저장한다. Esc·Undo는 이전 TRS를 복원하고 순간이동한다.
- sim on에서는 `-cut-gap`을 거절한다.

### R9 수명

- 조각 삭제와 owner 삭제를 분리한다.
- 다음 step/query 전에 body·query 매핑을 제거한다.
- 제거된 대상을 가리키는 대기 작업은 취소한다.

### R10 시뮬레이션

- 고정 1/60 s, 프레임당 최대 4 step, 남은 시간은 버린다.
- 실행 중 사용자가 시뮬레이션을 켜고 끌 수 있다(단축키 P와 디버그 UI 토글). 끄면 step을 돌리지 않아 모든 body가 그 자리에 멈추고 속도·sleep 상태는 보존되며, 다시 켜면 이어서 진행한다. `-physics-sim 0`(world 미생성)과는 다르다. 꺼진 동안에는 깨어 있는 body도 정지 상태로 보고 절단을 허용하며, 조각은 속도 0으로 시작한다.
- `-physics-sim 0/1`(기본 1)은 `-physics-query`와 독립이다. 초기화 실패 시 경고하고 sim off.
- 레이어 Query/Static/Moving. `QueryAabb`는 Query 레이어만 본다. query handle과 simulation body handle을 분리한다. query box는 크기가 같으면 위치만 갱신하고 sleep은 생략한다.
- 프레임 순서: 게시 → 입력 → 오래된 body 정리/`SyncBodies` → step → pose 반영 → `Scene.Update` → Finish → `SyncColliders` → 제출/요청.
- Startup에서 배치·정규화 후 첫 step 전에 모든 body와 바닥을 준비한다. simulation 등록은 GPU pose 준비 조건(`IsEligible`)에 의존하지 않는다.

### R11 예산

- owner당 컴포넌트 64개, 전역 body 용량(query·simulation·준비 중 body 포함 예약).
- 초과하면 절단을 거절하고 부모를 유지한다.

### R12 향후 호환 (이번 미구현)

- Skeletal: 공유 animation/physics pose → 조각별 DeformBinding → 생성 표면 평가. 본 물리 변환과 Wc의 중복 적용 금지.
- cap·내부층은 같은 조각의 section으로 둔다.
- ExtractToGameObject는 baked Static geometry 대상 재생성 transaction으로 후속 처리한다.

### R13 범위 밖

움직이는 조각 재절단, 속도 전달, Skeletal, ExtractToGameObject, V-HACD, UCX, 다른 Model의 내부층.

### R14 검증

CPU:

- 18-DOP(병합 전 반공간 교집합; cube·얇은 box·원점 밖).
- `HullFromPlanes` 실패 조건(m 단위 기준), cm/m snap 판정 일치.
- `SliceConvexElem` 부피 A+B=부모(허용오차), `BoxPlaneCompare` 통째 전달.
- scale에서 정규화 평면 위치.
- Jolt cook 허용오차·thin hull·반복 절단·cook 실패·Query 레이어 격리·오프센터 회전·scale 교체.

런타임:

- 시작 낙하 후 바닥 위 sleep(가장 낮은 점 ≥ 바닥 윗면 − 0.01 m).
- sleep Cube 절단 → 결과 2개, Unreal 질량, 게시 직후 점프 없음(≤1e-5), 조각 sleep.
- GPU 작업 중 깨어남 → 취소. sim off 절단. 빈 쪽 body 없음.
- 기즈모 순간이동·Esc·Undo, 조각/owner 삭제, 예산 거절, 강제 실패 rollback.
- Debug/Release D3D12 ERROR 0.
- `TransformHistory`를 바꾸면 `SceneMathTests`를 갱신한다.

### R15 절단 후 convex 재맞춤

절단 후 각 쪽 조각의 convex를 그 쪽 출력 정점의 18-DOP(GPU 측정)와 잘린 부모 convex의 교집합으로 다시 맞춘다. 포함이 검증되지 않거나 겹침이 불확실하면 잘린 convex를 그대로 쓴다. 섬 분리는 하지 않는다(Unreal SliceProceduralMesh와 같음, Skeletal 단계로 연기). Unreal 슬라이스는 재맞춤을 하지 않으며 이는 우리 정책이다.

## F. 구현 계획

단계마다 구현 → 독립 리뷰 → Debug/Release 빌드·테스트 → 사용자 검토·커밋 순서로 진행하며, 한 단계는 한 개념·약 300줄이다. 해당 문서와 README 상태를 단계마다 갱신한다. 2026-10-26 사용자는 5a~5c의 자동 진행·병렬 작업과 단계별 필수 리뷰를 승인했다.

| 단계 | 내용 | 주요 파일 | 대략 비용 |
| --- | --- | --- | --- |
| 0 | 설계 문서·기존 문서 정리 | `Docs/` | 리뷰 2~5분 |
| 1a | MiniCollision target(Jolt 무관 별도 CMake target) + ConvexElem(VertexData, ElemBox, GetPlanes, HullFromPlanes 이식, 부피) + `CollisionUnits.h` + ConvexElemTests. `BuildJolt.ps1`이 선택한 테스트만 실행 | `MiniEngine/Physics/Collision/`, `Physics/tests/`, `Tools/BuildJolt.ps1` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초) |
| 1b | `AggregateGeom::GenerateKDop18` + 테스트 | `Physics/Collision/AggregateGeom.*` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초) |
| 1c | `BoxPlaneCompare`·`SliceConvexElem`·`SliceAggregateGeom` + 질량 공식(`BodyMass.h`) + 테스트 | `Physics/Collision/` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초) |
| 2a | 로더: geometry 바이트 보관 후 metadata를 읽고 업로드, `EvaluateModelPose`로 메시별 hull, 메시별 노드 애니메이션 판별, Model 보관, 의존 방향 정리(Model.vcxproj·ModelConverter) | `Model/ModelLoader.cpp`, `Model.h`, `Model.vcxproj` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 2b | 컴포넌트 충돌 상태: 원본 section hull × SourceFrame⁻¹, 생성 revision 계승, SourceFrame 재기준화, revision↔collision 검증 | `ModelViewer/Scene/MeshComponent.*` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 3 | 세대 분리(증가 규칙 포함), 동작 변화 없음 | `MeshComponent.*`, `CuttingSystem.cpp`, `PhysicsSystem.cpp` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 4a | `PhysicsWorldInternal.h`(Jolt 비공개 PImpl 공유 경계), 레이어 3개, Query 필터, BodyHandle 분리 | `Physics/PhysicsWorld*.*` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초) |
| 4b | body 수명: 생성·등록·해제·파괴 분리, 미게시 body 자동 정리 소유 객체, 용량 예약, ConvexHullShape cook·compound·허용오차, 실패 테스트 | `Physics/PhysicsWorldBodies.cpp` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초) |
| 4c | 고정 Step·pose/sleep 읽기, 바닥 위 Box 낙하 CPU 테스트 | `Physics/` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초) |
| 4d | `SetBodyTransform` 순간이동·원자적 shape 교체 + 테스트 | `Physics/` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초) |
| 5a | Attached→WorldIndependent TRS 표현 가능성 검사 + CPU 검증 | `Scene/MeshComponent.*`, `SceneMathTests` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초) |
| 5b | `SyncBodies`·바닥 생성·Startup 순서·pose 반영 | `Scene/PhysicsSimulation.cpp`, `ModelViewer.cpp` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 5c | `-physics-sim`·시뮬레이션 켜기/끄기(P·UI)·누적기·프레임 순서(step 전 정리), 임시 제한(sim-on 절단·simulate 메시의 owner 기즈모/Undo·cut-gap 차단), 낙하·sleep 런타임 검증(`PhysicsSimChecks.h`) | `ModelViewer.cpp`, `Scene/tests/` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 6a | 물리 무관 게시 준비·교체: `PrepareGeometryReplacement`, 준비된 B 컴포넌트, 할당 없는 commit, `DrainResults` 결과 소비, 기존 소비자 이전 | `Scene/CuttingSystem.*`, `Scene.*` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 6b | 물리 절단 transaction(준비→검증→교체→rollback)·예산, 절단 임시 제한 해제 | `Scene/CutTransaction.*` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 6c | 실패 경로 런타임 검증(작업 중 깨어남·강제 cook 실패·빈 쪽) | `Scene/tests/` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 6d | 충돌 convex 디버그 표시(H·UI 토글), 조각별 색, 바닥 상자 | `Scene/CollisionDebugMath.h`, `Scene/CollisionDebugDraw.*`, `Scene/PhysicsSystem.*`, `Scene/GizmoDraw.h`, `ModelViewer.cpp`, `Scene/tests/CollisionDebugTests.cpp` | 리뷰 2~5분 + 빌드 약 90초 (+CPU 테스트 약 5초, 런타임 약 8초/회) |
| 6e-1 | GPU 조각별 18-DOP 측정·검증 | `MeshCutting/Shaders/CutBoundsCS.hlsl`, `GPU/CutTypes.h`, `GPU/GpuMeshCutter.*`, `tests/GpuCutValidation.cpp` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 6e-2 | CPU convex 재맞춤 함수·테스트 | `Physics/Collision/AggregateGeom.*`, `Physics/tests/HullRefitTests.cpp` | CPU 5621 checks/구성 |
| 6e-3 | 절단 경로 연결·포함 검증·문서 | `Scene/CuttingSystem.cpp`, `GPU/GpuMeshCutter.h`, `tests/GpuCutValidation.cpp` | Debug 증분 빌드 약 7초, Release 약 135초; 런타임 아래 기록 |
| 7a | EditTarget(Owner/Mesh) 선택 | `Scene/EditTarget.h`, `ObjectSelection.*`, `ModelViewer.cpp` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 7b | 공통 편집 적용 경로(transform·body·세대), scale cook 실패 원자성 | `Scene/` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 7c | History·Esc·Undo(대상 종류 저장), `SceneMathTests` 갱신, 기즈모 임시 제한 해제 | `TransformGizmo.*`, `TransformHistory.h` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 7d | 조각 삭제 vs owner 삭제·body 정리 | `Scene.*`, `PhysicsSystem` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 8 | 전체 검증·문서 상태 최종 확인 | `Docs/` | 빌드 약 90초 + 전체 테스트·런타임 |

각 코드 단계의 확인된 검토·빌드 기준 비용은 리뷰 2~5분 + Debug/Release 합계 약 90초로 약 3분 30초~6분 30초다. CPU 테스트는 기존 SDK 테스트 약 5초, 런타임은 기존 실행 약 8초를 참고한다. 구현·신규 검증 시간은 미측정이며 단계별 실측 후 갱신한다.

## G. 이전 계획과의 관계

[PhysicsIntegrationPlan.md](PhysicsIntegrationPlan.md)의 정지 kinematic 부모 우선, 움직이는 조각 절단, 속도 전달 로드맵은 이 문서로 대체한다. 움직이는 조각 재절단·속도 전달은 범위 밖(R13)으로 옮긴다. SDK 고정·조회 구현 기록은 [JoltQueryIntegration.md](JoltQueryIntegration.md)를 따른다.

## H. 4단계 검증과 5단계 실행 단위 (2026-10-26)

4a~4c는 `f5d60a6b`, `6d1a88b5`, `cccf6a3b`에 커밋되어 있다. 4d의 기존 스테이징 변경은 `SetBodyTransform`과 `SetBodyShape`, 공유 cook/질량 계산, PhysicsTransformTests다. 인계 리뷰에서 회전된 오프센터 원점/COM·선/각속도 보존, 제거된 body의 shape 교체·예약 유지·재등록 검증을 보강했다. Debug/Release CPU 8개 target 통과, PhysicsTransformTests 각각 544 checks 통과, ModelViewer 두 구성 빌드 성공. Debug 기존 조회·Cube 4회 절단은 D3D12 validation을 켜고 정상 종료했다. 기존 렌더/종료 경고는 남아 있다. 로그는 `%TEMP%/jolt-stage4-*.log`다.

4d의 확대 접촉 검사는 Jolt 기본 관통 허용치로 가장 낮은 점 -0.0196 m를 측정했다. 이는 R14 최종 런타임 기준(-0.01 m)을 충족했다는 뜻이 아니다. Scene 연결 시 최종 낙하 검사는 별도로 수행한다.

사용자는 5a~5c를 순서대로 자동 진행하되 각 단계 리뷰를 요구했다. 아래 단위마다 코드 diff 리뷰·검증·문서 갱신을 하고 6단계로 넘어가지 않는다. 기존 미커밋 변경과 스테이징은 보존하며 커밋은 만들지 않는다.

| 단위 | 구현 / 리뷰 판정 | 검증 및 기존 측정 비용 |
| --- | --- | --- |
| 5a | 월드 affine→양수 TRS 검증과 KeepWorld 전환. 전단·반사·무효값은 변경 전에 거절 | SceneMathTests + Viewer Debug/Release. 리뷰 2~5분, 기존 빌드 합계 약90초 |
| 5b-1 | 공통 PhysicsWorld에 Scene body 매핑, GPU 준비와 무관한 등록·stale 제거·pose 반영. 미완성 절단/owner 편집은 임시 차단 | Debug/Release 빌드 및 등록/해제 실행. 리뷰 2~5분 + 빌드 기준 |
| 5b-2 | 정규화 이후 bounds 기반 Static 바닥 생성, NotCuttable/NotEditable 공통 검사, startup 등록 순서 | Debug/Release 빌드, 바닥/초기 배치 확인. 리뷰 2~5분 + 빌드 기준 |
| 5c | 독립 sim/query 옵션, fixed 1/60·최대4 step·누적기 폐기, P/UI pause, 프레임 경계, 낙하/sleep·pause·삭제 회귀 | Debug/Release CPU/실행, 기존 실행 약8초/회(이번 debugger 초기화 포함 약40초). 리뷰 2~5분 + 빌드 기준 |

신규 구현 시간은 미측정이다. 5단계 동안 body가 연결된 메시의 절단은 pause 중에도 거절하며, body 없는 비대상 메시의 기존 절단은 유지한다. pause 중 절단 허용은 6단계 transaction이 준비된 뒤 해제한다.

### 5a~5c 구현과 리뷰 결과

- **5a 완료**: `SceneTransform::TryFromAffine`가 양수 TRS·오른손 직교 기저·재현 오차를 검사한다. `MeshComponent::MakeWorldIndependent`는 성공할 때만 화면 배치를 보존하며 분리한다. 전단·반사·퇴화·NaN/Inf 거절을 CPU 검사에 추가했다. 독립 리뷰에서 수정 필요 결함 없음.
- **5b-1 완료**: `PhysicsSystem::SyncBodies`는 GPU 준비 전에 CPU 충돌로 body를 준비하고, cook/용량 예약 성공 후 컴포넌트를 분리·게시한다. 삭제·geometry/편집 변경 body를 다음 step 전에 정리하고 `ApplyBodyPoses`는 COM이 아닌 원점 T/R만 반영한다. 리뷰에서 용량 부족 실패의 영구 캐시를 발견해 예약 수 변화 시 재시도하도록 수정했고, 재리뷰와 용량 1 회귀를 통과했다.
- **5b-2 완료**: `CreateFloor`는 CPU 충돌 경계에서 XZ 여유와 높이를 계산하고 cube asset bounds를 실제 Jolt Static Box와 맞춘다. 가장 작은 물체 높이의 절반 아래에 바닥을 두고 초기 clearance 0.5 m를 확보한다. 바닥은 정규화 뒤 생성되며 절단 후보·직접 요청·pick·owner 편집/Undo에서 제외된다. 바닥 메시만 제거되어도 다음 step 전에 body를 회수한다. 독립 리뷰 뒤 수명 처리를 보강했다.
- **5c 완료**: `PhysicsStepClock`와 `AdvanceSimulation`은 1/60 s·최대4 step·초과 잔량 폐기를 적용한다. P와 `Viewer/Physics/Running` BoolVar는 동일한 pause 상태를 사용하고 토글 시 시계만 초기화한다. pause/zero-step에서도 stale body를 정리하며 body pose/속도/sleep은 보존한다. `-physics-sim` 기본1과 `-physics-query`는 독립이고, simulation 초기화/바닥 준비 실패는 경고 후 비활성화한다. 독립 리뷰에서 추가 결함 없음.
- **6a 완료 (2026-10-28)**: 물리와 무관하게 절단 게시를 준비/commit으로 나눴다. `PrepareMeshComponent`·`PrepareGeometryReplacement`가 B 컴포넌트(ID·삽입 자리 예약)와 A 교체 자원을 만들고, `CommitGeometryReplacement`·`CommitMeshComponent`가 할당 없이 교체한다(세대 증가는 기존과 동일). 결과는 `CuttingSystem::DrainResults`(Owner/Mesh/Other/RevisionId/Status, 최대 256개·초과 시 오래된 것 폐기)로 소비하며 `ModelViewer`가 이전했고 `LastOther`는 진단용이다. 동작 변화 없음: Debug/Release 빌드, CPU 8 target·SceneMathTests 468·PhysicsStepClockTests 21 통과, 위 3개 실행 exit 0이며 sim-off 게시 4회와 D3D12 ERROR/CORRUPTION 0건을 OutputDebugString 수집으로 확인했다.
- **6b 완료 (2026-10-28)**: simulate 메시 절단을 body 교체 transaction(`Scene/CutTransaction.*`)으로 게시한다.
  - 흐름: 제출 시 `CutTransaction::CanCut`(실행 중 깨어 있는 body는 "Wait - object moving"으로 거절, sleep이거나 pause면 허용) → 게시 시 6a 준비 뒤 `Prepare`가 재확인(부모 body의 geometry·EditGeneration·PhysicsPoseGeneration, 깨어남 여부, 예산, A/B `PrepareBody`, B 매핑 칸 예약) → `CommitGeometryReplacement`·`CommitMeshComponent`·`CutTransaction::Commit`(부모 body 파괴 → A/B `CommitBody(activate)` → 매핑을 새 revision·편집 세대로 갱신). 조각은 속도 0으로 시작하며 중력·접촉으로 정착하도록 활성화한다. 충돌이 빈 쪽은 body 없는 렌더 조각이다.
  - 실패(용량·cook·stale·깨어남·pose 변경·예산)는 파괴자 RAII로 준비물과 B 칸을 회수하고 부모를 유지하며, 결과는 `Failed`, `Error=0x200`, `CutResultEntry::Reason`에 이유를 남기고 한 번 로그한다. 기존 generic stale 검사(Input geometry·EditGeneration·월드 행렬)는 이전처럼 Cancelled다. 부모 body가 예약 한 칸을 쥔 채 A/B를 준비하므로 일시적으로 여유 두 칸이 필요하다.
  - 예산: `CutBudget::MaxNewBodiesPerFrame = 4`. 시도 시점에 차감하고 실패해도 환불하지 않으며 프레임마다 초기화한다. 현재 `Update` 한 번에 GPU 작업이 하나만 끝나 평소에는 닿지 않는 안전 상한이다.
  - `PhysicsWorld::PrepareBody`가 BodyHandle을 예약하므로 handle 고갈은 Prepare 실패이고 유효한 `PreparedBody`의 `CommitBody`는 실패하지 않는다(PhysicsBodyTests 보강).
  - 해제: sleep이거나 pause인 simulate 메시의 절단. 유지: sim on의 `-cut-gap`(B를 바닥 아래로 옮겨 stale 재생성 경로로 관통하므로 7b의 `SetBodyTransform` 공통 경로 뒤에 해제), simulate 메시 owner의 기즈모·Undo(7단계).
  - 진단: 시뮬레이션 중 자동 절단은 모든 body가 sleep할 때까지 기다린다(`-physics-cut-checks 1`일 때만 3000프레임 초과를 오류로 보고). 신규 옵션 `-physics-cut-checks 1`은 절단 뒤 전 조각 sleep 시 body 수·정착을 검사한다.

    ```powershell
    ../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -model Models/cube.glb -physics-sim-checks 1 -physics-cut-checks 1 -cut 1 -cut-angle 30 -cut-expect-publications 1 -physics-query-exit 1200
    ```

  - 측정(Debug/Release 빌드 오류 0, CPU 8 target·SceneMathTests 468·PhysicsStepClockTests 21 통과, 위 3개 실행과 신규 실행 exit 0, D3D12 ERROR/CORRUPTION 0): Cube 1개가 게시 1회로 body 2개(bodyless 0)가 되고 전 조각이 sleep, 최저점 −1.155189(Debug)/−1.155187(Release)로 바닥 −1.155179의 ±0.01 m 안이었다. `-physics-query-capacity 3` 용량 부족 실행은 Failed·원본 유지·게시 0이었다.
- **6c 완료 (2026-10-28)**: 6b에서 미검증이던 실패·예외 경로를 런타임 시나리오로 검증한다.
  - 옵션: `-physics-cut-scenario <이름>`(기본 꺼짐). 이름은 `awake`(깨어 있는 body 제출 거절), `woken`(작업 중 깨움), `moved`(작업 중 0.001 m 이동), `fail-a`·`fail-b`(cook 실패 주입), `budget`(상한 1), `empty-a`·`empty-b`(빈 쪽 강제), `paused`(낙하 중 정지 뒤 절단), `paused-asleep`, `plain`이다. `-cut 1 -cut-angle 30 -cut-expect-publications <0|1> -physics-query-exit 1500`과 함께 Cube 단독(`-model Models/cube.glb`)으로 실행하며 `-physics-sim-checks`는 쓰지 않는다.
  - 주입 지점은 기본값에서 비활성이다: `CutTransaction::DiagFailSide`, `CuttingSystem::DiagEmptySide`, `CutBudget::SetLimit`. 시나리오가 끝나거나 Cleanup되면 `PhysicsCutScenario`가 되돌리며, 선택한 시나리오가 끝나지 않은 채 종료하면 오류다. 진단 접근자는 `PhysicsSystem::GetReservedBodyCount`·`GetSimulationVelocity`·`NudgeBodyForDiagnostic`이다.
  - 결과(Debug/Release 모두 exit 0, D3D12 ERROR/CORRUPTION 0): `awake`는 "Wait - object moving"으로 제출 거절·작업/게시 없음·부모 불변. `woken`은 Failed 0x200 "Wait - object moving". `fail-a`·`fail-b`·`budget`은 Failed 0x200과 이유, 부모 메시·body 수·예약 수(시작값) 복귀. `empty-a/b`는 빈 쪽이 bodyless이고 다른 쪽만 body를 가지며 SyncBodies가 재등록하지 않는다. `paused`·`paused-asleep`은 정지 중 절단이 허용되고 조각 속도는 0이며 재개 후 정착한다. 게시 프레임의 조각 월드 변환은 부모와 차이 0(허용 1e-5)이다. 정착은 모든 조각이 sleep하고 각 조각이 바닥−0.01 m 이상이며 적어도 하나가 바닥 ±0.01 m 안이다(30° 절단에서 위쪽 조각은 아래쪽 조각 위에 얹힌다, 예: 위쪽 −0.6676, 아래쪽 −1.155189, 바닥 −1.155179).
  - 참고: `moved`는 월드 행렬이 바뀌므로 일반 stale 검사가 먼저 걸려 Cancelled가 된다. `CutTransaction::Prepare`의 pose 세대 검사는 행렬이 같은 값으로 돌아왔지만 세대만 앞선 경우를 막는 방어로 유지하며 현재 시나리오로는 직접 도달하지 못한다.
- **6d 완료 (2026-10-28)**: 조각마다 충돌 convex hull이 절단으로 어떻게 나뉘는지 화면에서 확인하는 디버그 표시다(물리·절단 동작은 변하지 않는다).
  - 토글: `H`와 디버그 UI `Viewer/Physics/ShowCollision`이 같은 `BoolVar`를 쓴다(P와 `Viewer/Physics/Running`처럼). 기본 꺼짐이며 `-collision-draw 1`로 시작부터 켠다. 물리 실행·일시정지·`-physics-sim 0` 어디서나 동작한다. 진단 옵션 `-collision-draw-log 1`은 색별 모서리 수가 바뀔 때와 종료 때 통계·CPU 시간을 출력한다.
  - 데이터(캐시 없음): 매 프레임 충돌 상태가 FromModel/Inherited인 `MeshComponent`의 컴포넌트 로컬 `AggregateGeom`을 `GetWorldTransform(owner)`(비균일 scale 포함 전체 월드 행렬)로 변환한다. Unavailable/None은 그리지 않는다.
  - 모서리: `CollisionDebug::ExtractHullEdges`(`Scene/CollisionDebugMath.h`, GPU 무관)가 `IndexData` 삼각형에서 방향 없는 쌍을 모으고, 배정밀도로 면 법선을 구해 퇴화 삼각형은 법선에서 제외한다. 유효한 인접 면이 정확히 둘이고 법선 내적이 1-1e-9보다 크면 삼각분할 대각선으로 보고 숨긴다. 이 허용치는 약 0.0026도 미만의 꺾임을 숨기며 그보다 큰 꺾임(예: 0.5도)은 남긴다.
  - 그리기: `GizmoDraw`(깊이 검사 없는 화면 overlay 삼각형)에 두께 1.5 px 선으로 낸다. `GizmoDraw::Reserve`로 예상 선 수만큼(선당 192 B) 용량을 먼저 잡는다. 투영 전에 뷰 공간에서 `ClipSegmentToNearPlane`으로 근평면(z ≤ −near)에 맞춰 자르고(걸침·완전 뒤·평면 위 처리) 그 뒤 `GizmoMath::Project`로 투영한다. 호출 위치는 `RenderUI`에서 `AdvanceSimulation`·`Scene::Update` 뒤, 기즈모 그리기 앞이다(같은 스레드).
  - 색: 깨어 있는 body 초록, 잠든 body 파랑, 충돌은 있으나 body가 없는 조각(body 없음·simulation 끔) 노랑, 바닥 회색. 바닥은 모델 hull이 아니라 `CreateFloor`가 저장한 Static Box의 중심·반크기에서 12개 모서리를 그린다. `PhysicsSystem`에 `GetBodyState`(None/Awake/Asleep)·`IsFloorMesh`·`GetFloorBox` 읽기 전용 접근자를 추가했다.
  - 검증: `Scene/tests/CollisionDebugTests.cpp` 24 checks 통과(빌드 명령은 파일 머리말; 실제 `ConvexElem`·`SliceAggregateGeom` 사용). 단위 상자 12개, 0.5도 꺾인 윗면 상자 15개(능선 유지), 상자 평면 절단 두 조각 각 12개, 대각 절단 삼각기둥 각 9개, 퇴화·범위 밖 삼각형, 근평면 자르기(걸침·반대 방향·완전 뒤·평면 위·평면에 걸친 선분·유한 화면 좌표).
  - 런타임(Debug/Release 빌드 오류 0, `-collision-draw 1 -collision-draw-log 1`, 모두 exit 0, D3D12 ERROR/CORRUPTION 0). 모서리 수는 초록/파랑/노랑/회색 순서다. 혼합 장면(Cube+Bunny): 시작 60/0/0/12 → 61프레임 12/48/0/12(Cube만 정착) → 70프레임 0/60/0/12. Cube 절단 `-physics-cut-checks 1`: 0/24/0/12(조각 둘 정착 뒤), 절단 직후 24/0/0/12. `paused` 시나리오 24/0/0/12 → 0/24/0/12. `-physics-sim 0` 4회 절단: 노랑만 12→60, 회색 0(바닥 없음). `empty-a/b`: 빈 쪽은 충돌이 없어 그려지지 않고 남은 hull만 12개(초록 → 파랑). `-physics-query-capacity 2`로 body 등록이 거절된 Bunny는 노랑 48이다. 바닥 12개는 sim-on 모든 실행에서 확인했다.
  - 비용(Release, 프레임당 CPU, 캐시 없음): Cube+Bunny 평균 0.13 ms, 조각 33개(396 모서리, `-physics-sim 0` 연속 절단) 0.28~0.36 ms. 첫 프레임의 PSO 생성과 일시 정지로 최대 수 ms~100 ms 수준의 이상치가 있다. 기존 5·6c 검증 실행(Debug/Release 8회)은 그대로 통과했다.
  - 분량: 제품 코드가 약 450줄(테스트 제외)로 단계당 약 300줄 기준을 넘었다. `-cut 40` 연속 절단 스트레스 실행은 그리기를 꺼도 게시 40회에 도달하지 못해 멈추거나 예외로 끝난다(기존 현상, 원인 미조사. 8단계에서 조사한다).
  - 한계: H 키 입력과 UI 토글은 같은 `BoolVar`를 읽고 쓰는 구조로 보장하며 자동 입력으로 실제 키는 검증하지 못했다(창 포커스 불가). 화면 캡처는 하지 않았다.

- **6e-1 완료 (2026-10-28)**: 절단 각 쪽 출력 정점의 18-DOP을 GPU에서 측정해 읽어 온다(동작 변화 없음, 게시는 아직 쓰지 않는다).
  - 레이아웃: `Metadata` 끝에 `20 + 2*Sections` 뒤로 면당 19단어(축 9개 최솟값, 최댓값 9개, 기여 정점 수)를 붙인다. 축은 `KDopDir18`의 짝수 번째 방향 9개(음의 방향은 최솟값이 맡음)이며 `GenerateKDop18`과 같은 단위 방향이다. 값은 `Ordered` 인코딩의 원자 min/max이고 최솟값은 0xffffffff, 최댓값·개수는 0으로 시작한다.
  - 구현: `CutBoundsCS`의 `ReduceDop`이 기존 AABB 축소 뒤 같은 순회(구간별 VertexStart·min(count, capacity), cap 포함)로 그룹 축소 후 전역 합산한다. 출력은 컴포넌트 로컬 Pose를 그대로 쓰며 SourceFrame은 적용하지 않는다. 비유한 정점은 제외하고 세지 않는다. `CutJob::Poll`이 `SideDop`/`SideDopValid`로 풀며(C++17이 아니라 optional 대신 배열+플래그) 개수 0인 면은 sentinel을 해석하지 않는다. 새 셰이더·PSO는 없다.
  - 검증: `ValidateDiagnostics`가 분할 여부와 무관하게 `ValidateSideDop`으로 출력 Pose를 읽어 CPU double 기준과 비교한다(허용 `1e-6*extent+1e-7`, 유한 정점이 0이면 GPU도 비어야 함).
  - 결과(Debug/Release, 모두 exit 0, D3D12 ERROR/CORRUPTION 0): sim-off Cube 4회 절단 최대 오차 9.6e-8(허용 약 2.9e-6), 6b 30° 절단·6c `empty-a` 7.2e-8, Bunny 1.8e-5(좌표 약 250, 허용 약 4.4e-4). 샘플(Cube 첫 절단 A, 정점 54): 축0 [-1,1], 축1 [0,1], 축2 [-1,1.000001].
  - 미해결: Bunny 절단 출력 정점 수가 실행마다 다르다(A면 Debug 36576/36564, Release 36564). 구성이 아니라 실행 간 비결정일 수 있으며 원인은 조사하지 않았다.
- **6e-2 완료 (`e1f2b855`, 2026-10-28)**: `MiniCollision::RefitElemToDop`·`RefitAggregateToDop`이 슬라이스 convex와 패딩한 `Dop18Bounds`의 교집합을 만든다. 기본 패딩 `RefitDopPad`는 0.002 asset unit이다.
  - 결과는 `Refit`(교집합 채택), `Disjoint`(확실한 비겹침), `KeptUnrefit`(원본 유지)이며 집계는 `RefitStats`의 Refit·Disjoint·Kept·VolumeBefore·VolumeAfter다. 전부 Disjoint이면 aggregate 전체를 원본 유지한다.
  - guard: 잘못된 입력·유한 다각형 생성 범위의 절반 초과 좌표·hull 생성 실패·정확한 교집합 포함 실패·부피 증가 시 원본을 유지한다. 병합·snap 뒤 후보가 정확한 교집합 정점을 포함해야 한다.
  - `HullRefitTests`: Debug/Release 각각 5621 checks 통과(6e-3에서도 재실행). 상자·귀 모양 슬랩·얇은 조각·재맞춤 반복·비겹침·잘못된 입력·큰 좌표를 검사한다.
- **6e-3 완료 (2026-10-28)**: `CuttingSystem::Update`가 부모 충돌 슬라이스 직후, 진단·`PrepareGeometryReplacement`·`CutTransaction::Prepare` 전에 A/B별 GPU DOP을 double로 변환해 재맞춘다. DOP 무효·정점 수 0·빈 충돌은 슬라이스를 유지한다. 재맞춤 결과가 조각 충돌과 물리 body 준비에 전달된다.
  - `-collision-log 1`의 `CutCollision refit`은 면별 sliced·refit 부피, refit/sliced 비율, Refit·Disjoint·Kept 개수를 출력한다.
  - `CutJob::ContainmentHulls`에 슬라이스, `RefitContainmentHulls`에 재맞춤 결과를 보관한다. `ValidateDiagnostics`가 각 출력 정점의 두 aggregate 합집합 포함을 따로 검사한다. 두 검사 모두 슬라이스 hull 최대 변의 1e-4를 쓰고 기존 얇은 조각 허용오차·빈 충돌 규칙을 유지한다. 실패 주입은 이 검증 뒤에 적용한다.
  - 부피는 컴포넌트 로컬 asset unit³, 비율은 refit/sliced다. Cube 4회 절단에서 모두 비율 1.0(첫 A/B 각각 4.00731771 → 4.00731771). 같은 A의 재절단도 3.00590298 → 3.00590298, 1.00141473 → 1.00141473이며 두 포함 검증을 통과했다.
  - Bunny `-cut-y .5`: A 27063937.5 → 22578457.3(0.834263577), B 36585107.6 → 29057425.6(0.794241907). 작은 귀 쪽을 남기는 `-cut-y 200`: A 2251178.75 → 1147730.03(0.509835140), B 61397866.4 → 59938047.1(0.976223616). 두 집합 포함 검증을 통과했다.
  - 런타임(Debug/Release 각각 9회, `-debug 1 -collision-log 1 -cut-validate 1`): 섹션 H sim-off Cube 4회, Bunny 위 두 높이, 6b `-physics-cut-checks 1`, 6c `plain`·`empty-a`·`fail-a`·`paused`, 같은 Cube A 2회 재절단(`-cut 2 -physics-sim 0 -cut-repeat-plane 1 -cut-step-y .25 -cut-gap 0 -cut-expect-publications 2 -physics-query-exit 12`) 모두 exit 0, D3D12 ERROR 0·CORRUPTION 0이다. 6c는 기존 명령처럼 종료 프레임 1500, 6b는 1200을 사용했다.
  - 6b 조각 최저점 Debug −1.155189/Release −1.155187, 바닥 −1.155179이며 두 조각 sleep을 확인했다. `empty-a`는 body 1·bodyless 1, `fail-a`는 Failed 0x200·원본/예약 복원, `paused`는 게시 위치 오차 0·재개 후 정착을 통과했다. 성공한 body 준비는 `BuildShapeAndMass`의 유한 양수 질량 검사를 통과한다(질량 수치를 별도로 읽지는 않음).
  - 환경: NVIDIA GeForce MX450, 드라이버 DLL 32.0.15.9186. 실행당 약 31~73초. `%TEMP%/step2a/Dbg.cs`를 PowerShell `Add-Type`으로 프로세스 안에서 컴파일해 OutputDebugString을 수집했다. 빌드/실행 로그는 `%TEMP%/step6e3-*`, 재현 하네스는 `%TEMP%/step6e3-runtime.ps1`·`step6e3-small.ps1`이다. 기존 종료 시 LightGrid live-resource WARNING은 남아 있다.
- **7a 완료 (2026-10-28)**: 편집 대상 선택만 구현했다(적용·History는 7b·7c).
  - `Scene/EditTarget.h`: `EditTarget{Kind, Object, Mesh}`와 `FromPick`(월드 독립이면 Mesh, 아니면 Owner)·동등 비교(GPU 무관, `SceneMathTests`에서 검사). `ObjectSelection`의 `ResolveEditTarget`·`IsEditTargetValid`·`TryGetEditTargetTransform`이 Scene과 연결한다. `GameObject::IsEditable`에서 simulate 소유 검사를 제거해 바닥 등 명시적 잠금만 남겼고, simulate 조각 pick 제외를 풀었다.
  - `TransformGizmo::Update/Render`는 `EditTarget`을 받아 대상의 월드 TRS 위치·축에 기즈모를 그린다. Mesh 대상의 핸들 누름과 `Ctrl+Z`는 편집 없이 거절하고 안내를 띄운다(`GetNotice`). ModelViewer는 `m_Target`을 쓰며 `IsEditTargetValid`가 거짓이면 매 프레임 선택을 해제한다.
  - 진단 `-edit-target-checks 1`(`Scene/tests/EditTargetChecks.h`): 절단이 끝나고 모든 pose 경계가 준비되면 메시마다 26방향 ray로 pick해 종류(WorldIndependent→Mesh, Attached→Owner)·기즈모 위치·조각별 고유 Mesh 대상·바닥 미선택·사라진 대상 무효를 검사한다.
  - 검증(Debug/Release 빌드 오류 0, SceneMathTests 476·PhysicsStepClockTests 21·CollisionDebugTests 24 통과, D3D12 ERROR/CORRUPTION 0, 모두 exit 0): 혼합 장면 Mesh 2·Owner 1·바닥 1, Cube 절단 뒤 서로 다른 Mesh 2·바닥 1, `-physics-sim 0` 기본 장면과 sim-off 절단 Owner 3. 섹션 H 3개 실행과 6b·6c(`awake`·`fail-a`·`paused`·`empty-a`) 실행도 그대로 통과했다. 마우스 드래그·Ctrl+Z 거절과 화면 안내는 자동 입력으로 확인하지 못했다.

프레임 순서는 절단 결과 게시 → 입력/P·UI 반영 → `AdvanceSimulation`(stale 정리·등록→step→pose) → `Scene::Update`/Finish → `SyncColliders` → 절단 제출/요청이다. Startup에서는 기존 진단용 query world 재초기화를 simulation 등록 전에 완료한다. Cleanup은 cutting → physics body/참조/소유 바닥 → scene 순서다.

임시 제한: simulate 조각은 pick에서 Mesh 대상이 되어 기즈모가 보이지만 드래그와 Undo는 "Piece editing not ready"로 거절한다(7b·7c에서 해제). Owner 대상은 Attached 컴포넌트가 있을 때만 그것을 맞혀 얻으며 기존 동작을 유지하고 WorldIndependent 조각은 움직이지 않는다. simulate 메시 절단은 6b부터 body가 sleep이거나 pause일 때 허용하고 깨어 있으면 거절한다. `-cut-gap`은 simulation 활성 상태에서 거절하고 `-physics-sim 0`에서만 사용한다(7b 뒤 해제). 7b~7d는 구현하지 않았다.

검증 명령(작업 디렉터리는 저장소 루트, 실행은 `MiniEngine/ModelViewer`):

```powershell
./Tools/BuildJolt.ps1 -Configuration Debug -Target MiniPhysics -RunTests
./Tools/BuildJolt.ps1 -Configuration Release -Target MiniPhysics -RunTests
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Release /p:Platform=Windows
# 기본 혼합 장면: Cube/Bunny 낙하·sleep, Xbot 비시뮬레이션
../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -physics-sim-checks 1 -physics-query-checks 1 -physics-query-exit 4
# 조회와 simulation의 독립성
../Build/x64/Release/Output/ModelViewer/ModelViewer.exe -debug 1 -model Models/cube.glb -physics-query 0 -physics-sim-checks 1 -physics-query-exit 4
# 기존 비물리 GPU 절단 회귀
../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -model Models/cube.glb -physics-sim 0 -physics-sim-checks 1 -physics-query-self-test 1 -physics-query-checks 1 -physics-query-exit 12 -cut 4 -cut-scene 1 -cut-repeat-plane 1 -cut-step-y .25 -cut-validate 1 -cut-expect-publications 4 -cut-gap 0
```

SceneMathTests는 Debug/Release 각각 468 checks, PhysicsStepClockTests는 각각 21 checks다. Static Box 별도 CPU smoke는 각각 48 checks를 통과했으며 TEMP에 소스/로그를 보관했다. 혼합 장면 진단은 360 fixed ticks 뒤 Cube/Bunny sleep, 최저점과 바닥 오차 약 0.000002 m로 R14 관통 제한을 충족했다. pause/resume·최대4 step·등록 전 GPU 비의존·삭제·용량 재등록도 검사한다.

최종 CPU 회귀는 Debug/Release 각각 8 target 전부 통과했다(CTest 약1.07초/0.35초, 빌드 시간 제외). ModelViewer Debug/Release 빌드 성공. 위 3개 실행은 모두 exit 0, D3D12 ERROR/CORRUPTION 0이며 sim-off에서 실제 게시 4회를 확인했다. Release의 query-off에서도 simulation body 1개가 360 ticks 뒤 sleep했다. 마지막 바닥 소유자 검사 수정 뒤 두 구성 빌드도 재확인했다.

GPU는 NVIDIA GeForce MX450, driver DLL version `32.0.15.9186`이다. D3D12 validation을 켠 실행 로그는 `%TEMP%/jolt-stage5-runtime-{debug,release,simoff}.log`, 빌드/CPU 로그는 `%TEMP%/jolt-stage5-*.log`, Box smoke는 `%TEMP%/jolt-stage5-static-box/`다. 기존 렌더 경고와 Debug 종료 LightGrid live-resource 경고는 남아 있다. 자동 진단으로 물리와 기존 GPU 절단 회귀를 검증했으며 실제 P/UI·마우스 조작은 수동 검증하지 않았다.
