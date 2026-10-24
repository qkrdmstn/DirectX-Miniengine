상태: 진행 중 — 언리얼식 cap 정책과 BreakConstraint 서브트리 고정 구현, CPU 테스트·Debug/Release 빌드 통과 (커밋 전, 화면 확인 전)

> 이력 문서: 아래 상태·구현 설명은 과거 CPU 절단 경로의 설계·기록이다. 현재 구현 상태로 해석하지 않는다. 현행 흐름은 [CodeMap.md](../Current/CodeMap.md), 평면 계약은 [CutPlaneDesign.md](../Current/CutPlaneDesign.md)를 따른다.

# 언리얼 소스와의 비교 (SliceProceduralMesh, ProceduralMesh, BreakConstraint)

근거: Unreal Engine `release` 브랜치 소스를 직접 읽었다. 함수 이름으로 적는다.

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
