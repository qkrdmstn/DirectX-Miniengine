상태: 진행 중 — `feb35f23` 기준; 과거 CPU 이력·현행 GPU 접촉 비교 보존, UE5.3.2 절단 물리·Actor/컴포넌트·Skeletal 충돌 조사 추가 (2026-10-25)

> 1~5절은 과거 CPU 절단 경로의 설계·기록이며 현재 구현 상태로 해석하지 않는다. 6절은 현행 GPU 비교, 7절은 새 물리 조사다. 현행 흐름은 [CodeMap.md](../Current/CodeMap.md), 평면 계약은 [CutPlaneDesign.md](../Current/CutPlaneDesign.md)를 따른다.

# 언리얼 소스와의 비교 (SliceProceduralMesh, ProceduralMesh, BreakConstraint)

근거: 아래 1~5절은 과거 Unreal Engine `release` 조사 기록이다. 현행 GPU 수정의 직접 확인 버전과 차이는 6절에 구분한다. 함수 이름으로 적는다.

| 파일 | 본 함수 |
| --- | --- |
| `Plugins/Runtime/ProceduralMeshComponent/.../KismetProceduralMeshLibrary.cpp` | `SliceProceduralMesh`, `TriangulatePoly`, `CopyProceduralMeshFromStaticMeshComponent`, `BoxPlaneCompare` |
| `Source/Runtime/Engine/Private/GeomTools.cpp` | `FGeomTools::Buid2DPolysFromEdges`, `FindNextEdge`, `FixPolyWinding` |
| `Source/Runtime/Engine/Private/SkeletalMeshComponentPhysics.cpp` | `USkeletalMeshComponent::BreakConstraint`, `UpdateMeshForBrokenConstraints` |
| `Source/Runtime/Engine/Private/PhysicsEngine/PhysAnim.cpp` | `PerformBlendPhysicsBones`, `ShouldBlendPhysicsBones` |
| `Plugins/Runtime/GeometryProcessing/.../MeshPlaneCut.cpp`, `PlanarHoleFiller.cpp` | `FMeshPlaneCut::SplitCrossingEdges`, `CollapseDegenerateEdges`, `HoleFill` |

## 1. SliceProceduralMesh와 우리 절단

| 단계 | 언리얼 | 우리 |
| --- | --- | --- |
| 평면 | world 평면을 컴포넌트 로컬로 옮긴다 | world 그대로, 유한 사각형 검사(`ContainsLoops`) 추가 |
| 빠른 분류 | section AABB가 한쪽이면 section째 보낸다 (`BoxPlaneCompare`) | 없음 |
| 정점 분류 | `PlaneDot > 0` 2상태, 허용오차 없음 | +, 0, − 3상태와 크기 비례 ε, 평면과 겹친 면은 거절 |
| 분할 | 삼각형마다 교점을 새로 만든다 (공유 없음) | 에지 캐시로 교점 공유 (수밀) |
| 루프 | 교차 선분을 거리 0.01로 잇는다 | 정점 공유로 잇는다 |
| cap | 닫힌 다각형마다 따로 ear clipping. 방향은 `FixPolyWinding`으로 고친다 | 바깥 루프와 구멍을 묶어 ear clipping, 루프 정점 공유 |
| cap을 빼는 경우 | 닫히지 않은 사슬, 점 3개 미만, 귀를 못 찾은 다각형 | 아래 2절 |
| 실패 | 없음 (반환값 없음) | 스켈레탈 패치 경계에서 끝나는 절단만 `OpenLoop`로 거절 |
| 반대쪽 | 새 컴포넌트 생성, 같은 cap을 뒤집어 붙인다, convex 충돌체도 자른다 | `ProceduralMesh` 반환, 충돌체 없음 |

## 2. 이번에 맞춘 cap 정책 (되는 만큼 자르기)

`SliceGeometry`는 cap을 채울 수 없는 경계를 거절하지 않고 `GeometrySliceResult::uncapped`로 돌린다.

- 원본 구멍에서 끝나는 열린 사슬: 강체(스태틱) 복사는 열린 경계를 `OpenSurface`로 표시하고, 거기서 끝나는 사슬은 cap 없이 둔다. 스켈레탈 패치의 `Replacement`·`UnweldedSeam`에서 끝나면 지금처럼 `OpenLoop`다.
- 꼬이거나 서로 겹친 루프, 면적 0, 두 단계 넘는 중첩, 방향이 틀린 루프: 그 루프만 cap에서 뺀다. 바깥 루프가 빠지면 그 구멍도 빠지고, 구멍만 빠지면 바깥 루프가 구멍 자리까지 채운다.
- non-manifold 정점이 있으면 루프를 만들지 않고 모든 절단 에지를 cap 없이 둔다.
- 삼각분할: 전부 한 번에 시도하고, 안 되면 바깥 루프 묶음마다 시도해 되는 묶음만 cap을 만든다 (`Slicer::CapWhatFits`).
- cap이 없는 절단 에지는 `Cut`에서 `OpenSurface`로 바꿔, 같은 조각을 다시 잘라도 구멍으로 다룬다.
- 근접 허용오차가 조각 크기에 비례하므로 두 조각의 cap 판정이 다를 수 있다. 두 조각은 절단 에지 수만 같으면 된다.

언리얼과 남은 차이: 언리얼은 방향이 틀린 루프를 뒤집어 채우고, 겹친 루프도 귀를 찾으면 채우며, 구멍 단면은 구멍까지 덮는다. 우리는 cap이 표면과 정점·에지를 공유하므로 겹친 cap을 만들 수 없어서 그 루프의 cap을 뺀다.

## 3. ProceduralMeshComponent

| 항목 | 언리얼 | 우리 |
| --- | --- | --- |
| 데이터 | section별 정점·인덱스 배열, 위상 없음 | 용접된 Vertex / Corner / Face와 경계 태그 |
| 좌표 | 컴포넌트 로컬 + 트랜스폼 | 스태틱 조각은 절단 순간 world에 고정 + `CutJob::pieceWorld` |
| 갱신 | `UpdateMeshSection` (위상 고정) | `UpdatePose` |
| 복사 | `CopyProceduralMeshFromStaticMeshComponent`: LOD section을 로컬로, convex 충돌체·재질 복사, 원본 숨김은 호출자 몫 | `CopyProceduralMeshFromStaticMesh`: world로 복사, 충돌체 없음, 숨김은 컨트롤러 |

## 4. BreakConstraint (스켈레탈 관점)

언리얼 순서: `FindConstraintIndex` → 자식 바디 물리 켜기 → `TermConstraint` → `UpdateMeshForBrokenConstraints`(자식 본과 모든 자손 바디를 물리로, 자손 관절 drive 끔) → `AddImpulseAtLocation`.
`PerformBlendPhysicsBones`는 시뮬레이션 중인 바디의 본만 물리 값으로 바꾸므로 나머지 몸은 애니메이션을 유지한다. 메시는 손대지 않으므로 늘어남은 NDC의 마스킹이 따로 처리한다.

이번 구현(`ModelInstance::BreakConstraint`): 끊은 노드와 모든 자손이 끊는 순간 world에 고정되어 offset만큼 옮겨지고 애니메이션을 멈춘다. 나머지 노드는 계속 애니메이션한다. 이미 고정된 자손을 다시 끊으면 그 서브트리만 더 옮긴다. 물리(중력·충돌·Impulse)는 없다.

## 5. 메시-평면 처리에서 참고할 것 (미구현 후보)

`FMeshPlaneCut`이 우리 구조와 가장 가깝다.

1. 평면과 겹친 삼각형을 지우고 진행한다 (`bDeleteTrisOnPlane`). 우리는 `CoplanarFace`로 거절한다.
2. 절단 뒤 아주 짧은 새 에지를 접는다 (`CollapseDegenerateEdges`, seam 정점은 피함). 우리는 가는 조각을 `DegenerateInput`으로 거절한다. 접을 때 스킨 출처와 seam을 함께 맞춰야 한다.
3. AABB로 미리 거르기 (`SliceProceduralMesh`의 `BoxPlaneCompare`).
4. 부호 거리 병렬 계산 (`ParallelFor`). 측정 후 판단.
5. 기존 cap을 지나는 재절단: 두 언리얼 방식 모두 cap 삼각형을 일반 삼각형처럼 자른다. 우리는 `CrossesCap`으로 거절한다.

## 6. 현행 GPU 접촉·반복 절단 수정 (2026-10-25)

UE 5.3.2 [GitHub 공개 미러](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Plugins/Runtime/ProceduralMeshComponent/Source/ProceduralMeshComponent/Private/KismetProceduralMeshLibrary.cpp)의 Epic 작성 소스를 직접 확인했다. 버전은 같은 브랜치의 [Build.version](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Build/Build.version)으로 확인했다. 최신 Epic 비공개 저장소의 구현을 확인했다고 주장하지 않는다.

- `BoxPlaneCompare`는 section AABB가 완전히 한쪽이면 통째로 유지/이동하고 경계 접촉은 교차 후보로 남긴다.
- `SliceProceduralMesh`는 정점의 부호 거리가 엄격히 양수인 경우와 나머지를 분류한다. 다른 쪽 section의 제거 조건은 정점 또는 인덱스 배열이 비었는지이며, 단면만 있는 결과를 걸러내는 별도 분할 증명은 확인되지 않았다.
- 이는 소스 분석이다. 같은 버그를 Unreal 실행에서 재현한 결과는 아니다. Unreal의 단순 부호 분류를 복사하는 것만으로 우리 접촉 버그가 해결된다고 해석하지 않는다.
- 현행 `CutJob::HasSplit`은 GPU 상태에 양쪽 비공면·비퇴화 표면 출력과 활성 교차 경계가 모두 있어야 참이다. `BuildResults`는 새 cap을 제외한 양쪽 surface count도 검사한다. 실패하면 입력 revision과 컴포넌트 수를 유지한다. 열린 버니를 지원하므로 닫힌 부피 검사는 조건에 넣지 않는다.
- 평면 위 기존 모서리는 GPU 중점 해시와 양쪽 표면 지지로 접촉을 구별한다. 전역 원본 인접성은 구축하지 않는다. 기존 cap은 새 평면이 관통하면 일반 입력 표면처럼 처리한다.
- Unreal 함수는 대상 컴포넌트를 입력으로 받는다. 우리의 `CuttingSystem::RequestAll`은 호출자 책임인 후보 조회를 추가하고 후보별로 `SubmitPending`을 통해 순차 GPU 작업을 시작한다. 실제 물리 collider query는 후속 범위다.

## 7. 절단 이후 물리와 컴포넌트 구성 조사 (2026-10-25)

직접 확인한 코드는 앞 절과 같은 UE5.3.2 공개 GitHub 미러의 Epic 작성 소스다. 최신 Epic 계정 전용 저장소를 조회하거나 Unreal 런타임을 실행한 결과가 아니다. 아래 공식 문서는 조회 시 UE5.8로 표시되므로 소스 버전과 구분한다.

### 메시 자체의 충돌 기능과 별도 컴포넌트

- `UPrimitiveComponent`는 `FBodyInstance BodyInstance`를 가진다. 일반적인 단일 강체 컴포넌트는 여러 shape를 한 body에 묶을 수 있다. [PrimitiveComponent.h](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h)
- `UProceduralMeshComponent`는 `UMeshComponent`를 상속하며 `CollisionConvexElems`, `ProcMeshBodySetup`을 내부에 둔다. 별도 Box/Capsule 컴포넌트를 필수로 붙이는 구조가 아니다. 렌더 정점과 충돌용 convex 데이터는 별개다. [ProceduralMeshComponent.h](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Plugins/Runtime/ProceduralMeshComponent/Source/ProceduralMeshComponent/Public/ProceduralMeshComponent.h)
- 별도 충돌 컴포넌트도 사용한다. `ACharacter` 생성자는 `CapsuleComponent`를 만들고 root로 지정한다. 이는 캐릭터 이동의 대표 충돌이며 SkeletalMesh의 본별 body와 함께 존재할 수 있다. [Character.cpp](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Engine/Private/Character.cpp), [공식 capsule/mesh 충돌 설정 예](https://dev.epicgames.com/documentation/en-us/unreal-engine/applying-a-physics-constraint-profile-in-unreal-engine)

### SliceProceduralMesh가 하는 일과 호출자가 할 일

- 기존 `BodySetup->AggGeom.ConvexElems`를 순회해 평면 양쪽으로 분류하거나 `SliceConvexElem`로 자른다. 렌더 cap에서 충돌체를 새로 추론하는 경로가 아니다. 원래 convex가 없으면 이 경로에서 convex가 자동 생성되지 않는다.
- A에는 `SetCollisionConvexMeshes`를 적용한다. B는 원본과 같은 Outer로 새 ProceduralMeshComponent를 만들고 world transform·section·material·collision profile/enabled·complex-as-simple 설정과 잘린 convex를 전달한 뒤 등록한다. 새 Actor 생성이나 원본에 대한 Attach 호출은 없다.
- 함수 안에는 새 B의 `SetSimulatePhysics`, 부모 선/각속도 전달, impulse 부여가 없다. 원본 body 재생성 시 상태 보존과 새 B에 대한 명시적 상태 복사는 구분한다. 자동 질량 합·운동량 보존까지 보장한다고 해석하지 않는다.

위 근거: [KismetProceduralMeshLibrary.cpp의 SliceProceduralMesh](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Plugins/Runtime/ProceduralMeshComponent/Source/ProceduralMeshComponent/Private/KismetProceduralMeshLibrary.cpp). [공식 Slice 문서](https://dev.epicgames.com/documentation/unreal-engine/BlueprintAPI/Components/ProceduralMesh/SliceProceduralMesh)도 새 컴포넌트의 owner가 원본과 같다고 명시한다. 같은 Actor 소유와 parent attachment는 다른 관계다.

`SetCollisionConvexMeshes` → `UpdateCollision`은 convex를 BodySetup에 넣어 cook하고 physics state를 재생성한다. 비동기 옵션에서는 cook 완료 시 새 BodySetup을 채택한다. [ProceduralMeshComponent.cpp](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Plugins/Runtime/ProceduralMeshComponent/Source/ProceduralMeshComponent/Private/ProceduralMeshComponent.cpp)

동적 조각에는 유효한 simple convex와 simulation 설정이 필요하다. `UseComplexAsSimple`은 그 오브젝트 자체의 simulation을 지원하지 않는다고 [공식 simple/complex 설명](https://dev.epicgames.com/documentation/en-us/unreal-engine/simple-versus-complex-collision-in-unreal-engine)이 명시한다. 단일 body의 simulation 시작은 attachment를 해제하지만 Actor ownership을 바꾸는 작업은 아니다. Skeletal처럼 다중 body인 컴포넌트는 자동 detach하지 않는다. [공식 SetSimulatePhysics](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UPrimitiveComponent/SetSimulatePhysics)

### SkeletalMesh의 보통 경로

Physics Asset은 본별 shape/body 정의와 관절 정의를 저장하는 자산이다. 각 SkeletalMeshComponent는 실행 상태인 `Bodies`·`Constraints` 배열을 가진다. 본마다 별도 UBoxComponent를 생성하는 구조가 아니며 모든 skin bone에 body가 필요하지도 않다. [SkeletalMeshComponent.h](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Engine/Classes/Components/SkeletalMeshComponent.h), [공식 Physics Asset](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-asset-editor-in-unreal-engine), [본과 body 대응](https://dev.epicgames.com/documentation/unreal-engine/constraints-user-guide?application_version=4.27)

`InitArticulated/InstantiatePhysicsAsset`는 자산에서 body·constraint 인스턴스를 만든다. `BreakConstraint`는 관절 종료·자식 body simulation 활성화·하위 drive 해제·impulse를 처리하지만 렌더 삼각형이나 cap은 생성하지 않는다. 같은 SkeletalMeshComponent에 남아 있는 분리 body가 가능하다. 시각적인 피부 분리는 별도 geometry/masking 처리가 필요하다. [SkeletalMeshComponentPhysics.cpp](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Engine/Private/SkeletalMeshComponentPhysics.cpp)

`UpdateKinematicBonesToAnim`은 비시뮬레이션 body를 애니메이션 본에 맞춘다. `PerformBlendPhysicsBones`는 물리 body 결과를 본 pose에 반영한다. 부분 simulation/blend로 일부 신체만 물리에 맡길 수 있다. `bEnablePerPolyCollision` 경로도 있지만 일반적인 ragdoll body/constraint 표현과 구분한다. [PhysAnim.cpp](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Engine/Private/PhysicsEngine/PhysAnim.cpp), [공식 물리/애니메이션 혼합](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-driven-animation-in-unreal-engine)

### 우리 설계에 대한 해석

렌더와 충돌을 별도 데이터로 유지하고 같은 owner 아래 독립 body를 두는 방향은 참고할 수 있다. 다만 Unreal식 외부 API는 메시 컴포넌트에 통합되어 있으므로, 우리 `ColliderComponent`를 별도 타입으로 둔 것은 자체 설계 선택이다. 사용자가 별도 컴포넌트를 매번 수동 부착해야 한다는 요구로 연결하지 않는다. `EnablePhysics(meshId, proxy)` 같은 등록 API가 body/collider 참조를 내부 생성하는 방식도 가능하다.

Skeletal 단계에서는 하나의 MeshComponent에 본별 body 여러 개가 대응할 수 있다. 첫 Static 단계의 mesh 1개↔body 1개 규칙을 전체 엔진의 영구 제약으로 만들지 않는다. GPU 렌더 절단과 CPU convex 분할의 일괄 게시·stale 검사·속도 전달은 우리 비동기 구조에 맞춰 별도 구현한다.
