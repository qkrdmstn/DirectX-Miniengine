상태: 설계안 — `5f56b93d` 기준 재대조, UE5.3.2 근거 재확인, Xbot 데모 경로는 구현 인계 v2 참조 (2026-10-28), Skeletal 구현 전

# GPU 절단과 BreakConstraint

구현자는 [BreakConstraintImplementation.md](BreakConstraintImplementation.md)의 단계·확정 계약을 따른다. 이 문서의 제안/후보와 충돌하면 구현 인계 문서를 우선한다. Unreal 조사 근거는 이 문서 2절을 따른다.

## 1. 결론과 범위

Unreal의 `USkeletalMeshComponent::BreakConstraint`는 새 Actor나 SkeletalMeshComponent를 만들지 않는다. 기존 component의 본별 물리 body 사이 관절을 종료하고 분리된 쪽을 물리로 구동한다. ReferenceSkeleton의 부모 관계·inverse bind·skin weight·렌더 삼각형은 이 함수에서 수정하지 않는다.

우리도 한 GameObject와 한 ModelInstance를 유지한다. 관절 분리는 CPU Jolt, 본 pose의 물리 반영은 CPU, 표면/단면 절단과 다수 정점 변형은 GPU가 담당한다. 조각 A/B는 같은 owner의 MeshComponent로 표현한다. 독립 GameObject로 옮기는 기능은 별도 수명/게임 로직 기능이다.

이번 문서는 설계만 추가한다. 다른 세션의 Static 물리 구현 파일은 변경하지 않는다. Static의 낙하·sleep 절단 조건은 [StaticPiecePhysics.md](StaticPiecePhysics.md)의 범위이며 움직이는 Skeletal에 자동 적용하지 않는다.

## 2. 직접 확인한 Unreal GitHub 근거

Epic 공식 GitHub 저장소는 이번 웹 접근에서 본문을 읽을 수 없었다. 아래는 공개 미러에 있는 Epic 작성 소스를 직접 읽은 결과다. 최신 UE 또는 Unreal 런타임 실행 결과로 해석하지 않는다.

| 근거 | 확인 대상 |
| --- | --- |
| [Build.version](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Build/Build.version) | Major=5, Minor=3, Patch=2 |
| [SkeletalMeshComponentPhysics.cpp](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Engine/Private/SkeletalMeshComponentPhysics.cpp) | `BreakConstraint`, `UpdateMeshForBrokenConstraints`, `FindConstraintIndex`, `GetBodyInstance` |
| [ConstraintInstance.cpp](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Engine/Private/PhysicsEngine/ConstraintInstance.cpp) | `TermConstraint`, `IsTerminated` |
| [PhysAnim.cpp](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Engine/Private/PhysicsEngine/PhysAnim.cpp) | `PerformBlendPhysicsBones`, `ShouldBlendPhysicsBones` |
| [Jolt 5.6.0 PhysicsSystem.h](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/PhysicsSystem.h) | `RemoveConstraint` |
| [Jolt 5.6.0 Constraint.h](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Constraints/Constraint.h) | `SetEnabled`, `EConstraintSpace::LocalToBodyCOM` |

### 2.1 함수 호출 순서

`BreakConstraint(Impulse, HitLocation, InBoneName)`는 다음 순서로 처리한다.

1. `FindConstraintIndex(InBoneName)`로 관절을 찾는다. 내부 조회는 PhysicsAsset의 constraint 이름 조회다. 모든 skin bone에 같은 이름의 관절이 있다는 보장은 없다.
2. 인덱스가 없거나 이미 `IsTerminated()`이면 반환한다. 반복 호출로 impulse를 재적용하지 않는다.
3. 관절의 `GetChildBoneName()`에 대응하는 `FBodyInstance`를 찾고, 비시뮬레이션 상태이면 `SetInstanceSimulatePhysics(true)`를 호출한다.
4. `TermConstraint()`로 solver의 관절을 해제한다.
5. `UpdateMeshForBrokenConstraints()`로 끊긴 관절의 자식 본과 자손에 대응하는 body의 simulation을 켜고 관련 linear/angular drive를 끈다.
6. `AddImpulseAtLocation(Impulse, HitLocation, InBoneName)`를 호출한다.

`TermConstraint`는 유효한 `ConstraintHandle`에 대해 physics write 경로에서 `FPhysicsInterface::ReleaseConstraint`를 호출한다. `IsTerminated`는 handle의 무효 여부를 검사한다. Constraint 인스턴스의 이름·설정과 런타임 solver joint의 생존은 서로 다른 상태다.

`UpdateMeshForBrokenConstraints`는 자손 관절을 전부 종료하지 않는다. 팔꿈치에서 자르면 팔꿈치 연결은 해제하되 손목 관절의 연결/제한은 남고, 애니메이션을 따라가게 하던 drive를 해제할 수 있다. 이때 별도 연결 경로가 있으면 한 관절 제거만으로 완전 분리가 되지 않을 수 있다.

### 2.2 실제 변경되는 데이터

| 데이터 | Break 이후 |
| --- | --- |
| Actor 소유권 / component 수 | 유지 |
| component의 본별 Bodies | 기존 body를 유지하고 대상 subtree의 simulation 상태 변경 |
| Constraints의 런타임 handle | 선택 관절의 solver joint 해제 |
| 자손 관절 drive | 위치/속도 및 각도 drive 해제 경로 실행 |
| 물리 body 속도 | impulse와 중력/접촉 solver에 의해 변경 |
| 본의 local/component pose | 후속 physics blend에서 물리 결과로 갱신 |
| ReferenceSkeleton / skin weight / VB·IB | 관절 분리 함수에서는 유지 |

소스의 `SetOrientationDriveTwistAndSwing` 등은 해당 버전의 구체적 경로다. 우리 Jolt 구현에서는 사용 중인 drive 종류 전체를 명시적으로 끄며 UE 호출 이름만 복사하지 않는다.

### 2.3 부모 본을 유지하면서 떨어지는 원리

`PerformBlendPhysicsBones`는 body의 world transform을 가져와 부모 본 world transform에 대한 상대 transform으로 바꾼다. 이를 `InOutBoneSpaceTransforms`에 물리 blend weight로 혼합하고 `InOutComponentSpaceTransforms`를 다시 만든다. body가 없는 자손도 부모의 갱신된 pose를 상속할 수 있다.

아래는 열벡터·강체 변환(scale 없음)으로 단순화한 설명식이다. scale을 가진 본의 분리 규칙은 구현 인계 6절을 따른다. Unreal의 FTransform 표기 순서를 그대로 옮긴 식이 아니다.

```text
분리된 본의 물리 world pose = P
부모 본의 최종 world pose   = H
분리된 본 local pose       = inverse(H) * P
계층을 다시 곱한 world     = H * inverse(H) * P = P
```

부모가 이동해도 local pose를 매 프레임 역산하기 때문에 결과는 독립적인 물리 위치 P다. skeleton 계층을 잘라서 새 자산을 만들 필요가 없다.

simulation 활성과 화면 pose 반영은 별도다. 해당 소스는 `bBlendPhysics`, body별 `PhysicsBlendWeight`, collision/physics 상태로 blend 실행을 결정한다. 우리 설계는 분리 body에 physics weight=1을 명시하고 본 pose에 반영되는지 별도로 검사한다.

## 3. 현재 코드와 부족한 기반

- `ModelInstance::Update`는 애니메이션 → `EvaluateModelPose` → `m_ModelSpaceTransforms` → `m_Skeleton` 순서다. 물리 override 단계와 bone/body 매핑은 없다.
- `m_Skeleton[i].posXform = nodePose[jointNode] * inverseBind[i]`다. 이 팔레트를 생성하기 전에 최종 본 pose를 확정해야 한다.
- `EvaluateModelPose`는 직렬화된 DFS `GraphNode`를 사용한다. parent 인덱스 배열이 이미 있다고 가정하지 않는다. 별도 runtime parent 매핑을 만들고 `skeletonRoot`의 공간 초기화 의미를 보존한다.
- `MeshComponent::Update`는 `inverse(SourceFrame) * node * skin(p)`를 만들고 렌더에서 Wc를 적용한다. 물리 world pose를 곧바로 joint palette에 넣으면 공간 또는 node 변환이 중복될 수 있다.
- `PhysicsWorld`는 query box API와 Static/Dynamic body의 prepare/commit/destroy·Step·pose/속도 조회를 가진다. Kinematic motion·impulse·관절 API는 없다. query용 AABB를 랙돌 shape로 사용하지 않는다.
- Xbot.glb는 Armature 배율 0.01을 가지고 뷰어가 시작 시 다시 정규화 배율을 곱한다. 본 pose에 uniform scale이 들어 있으므로 물리 frame은 scale을 분리해야 한다(구현 인계 6절).
- `CuttingSystem::Submit`은 skin section을 명시적으로 거절한다. `FollowModel`과 `Frozen`만 있으며 조각별 본 기여 정책은 아직 없다.

## 4. 제안하는 소유 구조와 데이터

아래 타입명은 제안이며 현재 존재하는 API가 아니다. PhysicsAsset에 해당하는 저해상도 skeletal 물리 자산을 먼저 준비해야 한다. glTF skin만으로 body 질량·관절 제한을 추론하지 않는다.

| 소유 위치 | 필요한 데이터 |
| --- | --- |
| 공유 skeletal 물리 자산 | BodyDefinition(Bone/NodeId, shape, bone↔body 고정 offset, 질량), ConstraintDefinition(Id, ParentBody, ChildBody, anchor/frame, limits, collision pair 정책) |
| ModelInstance | animation pose, 최종 pose, runtime parent/node/joint 매핑, pose generation. 물리 상태는 두지 않고 FinalizePose override로만 받는다 |
| Scene의 SkeletalPhysics 연결 계층 | ObjectId, 자산 참조, body/constraint handle 매핑, 본별 Animation/Physics 제어권, broken 상태, 분리 그룹, topology generation |
| PhysicsWorld | Jolt body/constraint 실제 자원, step 경계 변경 큐 |
| MeshComponent | GeometryRevision, DeformBinding, 조각별 joint 기여/attachment 정책과 bounds |
| 절단 transaction | 대상 revision/generation, snapshot pose, geometry A/B 후보, 물리 변경 계획, 취소 상태 |

한 본 body를 조각 MeshComponent마다 중복 생성하지 않는다. 여러 메시 조각이 같은 최종 pose의 서로 다른 영역을 그릴 수 있다. 기존 Static용 ColliderComponent의 mesh↔body 관계를 본별 매핑의 영구 제약으로 사용하지 않는다.

## 5. 프레임 흐름과 좌표 계약

```text
Animation pose 평가
 → 비시뮬레이션 body의 kinematic target 갱신
 → 준비가 끝난 절단 transaction을 step 경계에서 commit
 → Jolt step
 → body world pose를 bone pose로 역반영
 → 최종 node pose / joint palette 확정
 → 컴포넌트별 GPU 변형·bounds 준비
 → 렌더 / 다음 절단 snapshot 생성
```

제안식에서 Wm은 physics skeleton의 model→world 기준, Gj는 최종 model-local bone pose, Xj는 body-local→bone-local 고정 변환, Bj는 body 원점의 world transform이다. scale은 shape cooking/자산 정책에서 별도로 처리한다.

```text
animation → body: Bj = Wm * Gj_anim * Xj
body → bone:     Gj_phys = inverse(Wm) * Bj * inverse(Xj)
bone local:      Lj_phys = inverse(Gparent_final) * Gj_phys
skin palette:    Sj = Gj_final * inverseBindj
```

Jolt COM transform과 body 원점 transform을 혼용하지 않는다. COM-local joint frame에는 shape COM 보정을 적용한다. 수식은 먼저 단위/회전/offset 테스트로 검증하며 기존 node*skin 계약의 mesh node 보정까지 실제 포맷에 맞춰 확인한다.

초기 skeletal 물리 실험은 uniform scale로 제한하고, 비균일 scale·shear가 생기는 자산은 명시적으로 거절한다. Static의 scale 지원이 곧 skeletal frame 지원을 뜻하지 않는다.

분리 subtree의 body는 physics weight=1, 나머지는 animation 제어를 기본으로 한다. body 없는 bone은 물리로 갱신된 부모 아래 local animation을 유지하는 초기 정책을 쓴다. 기존 dynamic body의 pose/속도를 초기화하지 않고 kinematic→dynamic 전환 body만 현재 pose로 동기화한다. 초기 속도는 Kinematic body가 `MoveKinematic`으로 얻은 속도를 전환 후 그대로 쓴다(구현 인계 6·8절 우선). 첫 target 갱신 전이면 0이며 진단에 표시한다.

## 6. BreakConstraint 명령의 계약

제안 진입점은 `RequestBreakConstraint(ObjectId, ConstraintId, WorldImpulse, WorldHitPoint, ExpectedTopologyGeneration)`이다. BoneId→ConstraintId 매핑은 별도 조회다. 없는 관절을 가까운 본으로 조용히 대체하지 않는다.

1. owner·자산·세대와 body/constraint 유효성을 검사한다. `NotFound`, `AlreadyBroken`, `Stale`, `UnsupportedTopology`를 구분한다.
2. constraint의 자식 subtree와 남아 있는 연결을 계산한다. 초기 범위는 tree형 관절 그래프이며 교차 연결이나 외부 pin은 거절한다. root에 parent 관절이 없으면 일반 break 요청은 실패다.
3. Jolt Update 밖의 step 경계에서 선택 constraint를 `RemoveConstraint`하고 broken 상태를 기록한다. 이후 임의 초기화로 다시 연결하지 않는다.
4. 분리 그룹 body를 dynamic으로 전환하고 해당 그룹의 animation drive를 해제한다. 내부 관절은 유지하고 그룹 body를 깨운다.
5. 타격 body에 world impulse를 한 번 적용한다. hit point와 COM 차이에 따른 회전도 물리엔진에 맡긴다.
6. topology generation을 증가시키고 결과를 게시한다. 동일 요청 재처리와 `AlreadyBroken`에서는 impulse를 더하지 않는다.

관절 제거는 충돌 필터 변경과 별도다. 연결 시 충돌 제외했던 부모/자식 pair를 계속 제외할지 다시 접촉시킬지 기록해야 한다. 초기 겹침과 폭발적 반발 검증 후 pair별 정책을 확정한다. owner가 같다는 이유로 모든 분리 body끼리 충돌을 영구 제외하지 않는다.

단독 BreakConstraint는 물리 시험 API로 제공할 수 있다. GPU 절단 요청 안에서는 아래 transaction의 일부로만 실행한다.

## 7. GPU 표면 절단과 함께 게시

관절 해제는 삼각형 연결과 혼합 weight를 제거하지 않는다. 예를 들어 몸통 40% + 팔 60% 정점은 관절이 끊겨도 두 본의 중간으로 끌려간다. 표면을 A/B로 복사하는 것만으로 이 영향도 사라지지 않는다.

[GPUCuttingSystemDesign.md](../GPU/GPUCuttingSystemDesign.md)의 12.4~12.5를 다음 계약으로 연결한다.

1. 분리 전 최종 pose·Wc·평면·revision·topology generation을 고정한다. patch P와 분리 subtree D는 다른 집합이다.
2. GPU에서 P를 clip하고 원본 나머지 A/B membership·생성 surface·cap 후보를 준비한다. patch 밖에 D와 잔존 본의 혼합 삼각형이 있으면 patch 확대 또는 거절한다.
3. 결과 pose를 지속 평가할 출처/조각별 기여 정책과 물리 계획이 모두 유효할 때만 commit한다. 실패·overflow·삭제·세대 불일치면 이전 geometry와 관절 상태를 유지한다.
4. GPU 결과 fence와 CPU 준비가 완료된 frame/step 경계에서 Geometry A/B·원본 교체 목록·본 제어권·관절 제거·bounds를 함께 공개한다. commit 도중 실패하지 않도록 자원/메타데이터를 사전 확보한다.
5. 비동기 작업 중 대상 그룹이 움직이면 첫 구현은 pose generation 불일치로 요청을 취소한다. 정지된 animation/physics snapshot에서 먼저 검증하며 움직이는 대상의 지연 절단은 후속 정책이다.

조각별 기여 보정 후보는 다음과 같다. Fj(t)는 기존 node·joint·inverse bind 순서를 모두 포함한 원본 bind 정점→공통 posed frame 변환이다. As는 조각 s의 attachment를 같은 frame에서 나타낸 강체 변환이다.

```text
자기 조각에 속한 본: Fsj(t) = Fj_final(t)
반대쪽 본의 잔여 영향: Fsj(t) = As(t) * inverse(As(t0)) * Fj(t0)
```

t0에서 원래 위치를 유지하면서 반대쪽 영향도 자기 조각 attachment를 따라가게 하는 자체 제안이다. Unreal BreakConstraint 자체의 동작이 아니다. 이 보정은 component-local로 옮기기 전에 수행하고 SourceFrame 역변환과 Wc는 각각 한 번만 적용한다. 잔존 쪽 attachment와 분리 쪽 attachment를 별도로 선택한다.

생성 교점은 side별로 보정된 원본 정점을 평가한 뒤 `lerp(sourceA, sourceB, t_cut)`로 재구성한다. 새 weight 단순 보간과 동일하다고 가정하지 않는다. attachment 선택, 경계 일치, normal/tangent, cap 변형은 독립 수학/시각 검증을 통과해야 한다. cap 출처와 재절단 정책이 확정되기 전에는 지속 변형 cap·skeletal 재절단을 지원으로 표시하지 않는다.

## 8. 구현 단계와 검증 기준

아래는 개략 단계다. 실제 시작 순서와 Static 선행 조건은 구현 인계 문서의 BC-00~BC-14를 따른다. pose 기반 작업은 지금 시작할 수 있고 Jolt 연동은 Static 공통 기반 완료 뒤 진행한다. 아래 비용은 기존 Static 계획의 리뷰 2~5분/빌드 약90초 기준이고 skeletal 런타임 비용은 아직 미측정이다.

| 단계 | 완료 기준 | 검증 비용 기준 |
| --- | --- | --- |
| B1 | 런타임 parent/node/joint 및 body/constraint 자산 매핑, 잘못된 ID 거절 | 리뷰 2~5분 + CPU 검사 실측 |
| B2 | 본별 body 생성과 animation→kinematic 갱신, 고정 offset 왕복 | 리뷰 2~5분 + Debug/Release 약90초 + 런타임 실측 |
| B3 | physics→bone override 후 palette 확정, 팔만 물리·몸통 animation | 동일 + 본 없는 자손/owner 이동 검증 |
| B4 | 명시적 constraint 제거·drive 해제·중복 impulse 방지 | 동일 + 내부 손목 관절 유지/삭제·종료 검증 |
| B5 | GPU 패치·A/B membership·side별 기여/출처 계약을 각각 구현 | 하위 변경마다 동일 + CPU 수학/화면 회귀 실측 |
| B6 | GPU/physics 일괄 commit·취소·stale와 실패 원복 | 동일 + 지연 결과/삭제/overflow 검증 |

최소 데모는 Xbot 왼팔 상완→전완→손 3 body다(구현 인계 BC-12b). 팔꿈치 관절을 끊어도 owner 수는 1이고 body 수는 유지되며 끊기지 않은 constraint 수는 2→1, solver enabled 수는 0→1(Kinematic 쌍은 disabled였다가 손목이 Dynamic 쪽을 얻어 enabled)이어야 한다. 손목은 연결된 채 전완/손이 중력·충돌·impulse에 반응하고 상완과 몸통은 animation을 계속해야 한다.

관절 단독 시험에서 피부 늘어짐이 보이는 것은 geometry 처리가 없음을 보여주는 분리 진단이다. 최종 GPU 통합 완료 판정에서는 혼합 weight 경계 늘어짐·중복 표면·프레임 구멍·변환 이중 적용이 없어야 한다. Undo는 관절 하나만 재생성하는 방식으로 처리하지 않으며 pose·속도·drive·geometry를 포함한 transaction 복구가 준비되기 전에는 해당 물리 편집을 제한한다.

## 9. 이번 검증

UE5.3.2 GitHub 소스의 함수 본문과 현재 ModelInstance/MeshComponent/PhysicsWorld/CuttingSystem 계약을 대조했다. 코드·Geometry 변경이 없어 빌드 및 CPU 절단 테스트는 실행하지 않았다. Unreal 및 Jolt에서 분리 동작을 실행 재현한 결과는 아직 없다.
