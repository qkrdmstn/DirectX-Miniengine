상태: 진행 중 — 1단계·2단계(2a `4d3d393b`·2b-1 `e23e4945`·2b-2) 구현 완료, 3 이후 미구현 (2026-10-26)

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
  - `EditGeneration`: geometry·scale·source frame·명시적 편집·teleport·Attached에 영향을 주는 owner 편집에서 증가.
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

움직이는 조각 재절단, 속도 전달, Skeletal, ExtractToGameObject, V-HACD, UCX, 다른 Model의 내부층, 일시정지 키.

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

## F. 구현 계획

단계마다 Sonnet 구현 → Codex 리뷰 → Debug/Release 빌드·테스트 → 사용자 검토·커밋 순서로 진행하며, 한 단계는 한 개념·약 300줄이다. 해당 문서와 README 상태를 단계마다 갱신한다.

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
| 5c | `-physics-sim`·누적기·프레임 순서(step 전 정리), 임시 제한(sim-on 절단·simulate 메시의 owner 기즈모/Undo·cut-gap 차단), 낙하·sleep 런타임 검증(`PhysicsSimChecks.h`) | `ModelViewer.cpp`, `Scene/tests/` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 6a | 물리 무관 게시 준비·교체: `PrepareGeometryReplacement`, 준비된 B 컴포넌트, 할당 없는 commit, `DrainResults` 결과 소비, 기존 소비자 이전 | `Scene/CuttingSystem.*`, `Scene.*` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 6b | 물리 절단 transaction(준비→검증→교체→rollback)·예산, 절단 임시 제한 해제 | `Scene/CutTransaction.*` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 6c | 실패 경로 런타임 검증(작업 중 깨어남·강제 cook 실패·빈 쪽) | `Scene/tests/` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 7a | EditTarget(Owner/Mesh) 선택 | `Scene/EditTarget.h`, `ObjectSelection.*`, `ModelViewer.cpp` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 7b | 공통 편집 적용 경로(transform·body·세대), scale cook 실패 원자성 | `Scene/` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 7c | History·Esc·Undo(대상 종류 저장), `SceneMathTests` 갱신, 기즈모 임시 제한 해제 | `TransformGizmo.*`, `TransformHistory.h` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 7d | 조각 삭제 vs owner 삭제·body 정리 | `Scene.*`, `PhysicsSystem` | 리뷰 2~5분 + 빌드 약 90초 (+런타임 약 8초/회) |
| 8 | 전체 검증·문서 상태 최종 확인 | `Docs/` | 빌드 약 90초 + 전체 테스트·런타임 |

각 코드 단계의 확인된 검토·빌드 기준 비용은 리뷰 2~5분 + Debug/Release 합계 약 90초로 약 3분 30초~6분 30초다. CPU 테스트는 기존 SDK 테스트 약 5초, 런타임은 기존 실행 약 8초를 참고한다. 구현·신규 검증 시간은 미측정이며 단계별 실측 후 갱신한다.

## G. 이전 계획과의 관계

[PhysicsIntegrationPlan.md](PhysicsIntegrationPlan.md)의 정지 kinematic 부모 우선, 움직이는 조각 절단, 속도 전달 로드맵은 이 문서로 대체한다. 움직이는 조각 재절단·속도 전달은 범위 밖(R13)으로 옮긴다. SDK 고정·조회 구현 기록은 [JoltQueryIntegration.md](JoltQueryIntegration.md)를 따른다.
