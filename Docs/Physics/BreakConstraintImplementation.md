상태: 진행 중 — BC-04a `8593a0be`, BC-04b `d31863a7`, BC-05 구현·검증 완료(미커밋), BC-06 이후 미착수 (2026-10-29)

# BreakConstraint 구현 인계

## 1. 시작점과 완료 범위

구현자는 BC-00 한 단계부터 시작한다. 약 300줄의 한 개념 변경 → 검증 → 문서 상태 갱신 → 사용자 검토·커밋을 위한 중단 순서를 지킨다. 전체를 한 번에 구현하지 않는다. Static 공통 기반(body prepare/commit, 고정 step, CutTransaction)은 `5f56b93d`까지 커밋됐다. 코드 작성 세션이 겹치면 시작하지 말고 사용자에게 인계 상태를 알린다.

필요한 문서만 읽는다: [조사 근거](BreakConstraintDesign.md) 2절, [Scene](../Current/Scene.md) 소유권/변환 절, [Static 계획](StaticPiecePhysics.md) F의 공통 물리 선행 단계. GPU 연결을 시작할 때만 [GPU 설계](../GPU/GPUCuttingSystemDesign.md) 12~13절을 추가한다.

첫 완료 목표 M1은 한 owner의 body chain에서 관절 하나를 끊고, 떨어진 subtree만 Jolt가 구동하며 그 결과가 본 pose/palette에 반영되는 것이다. M1의 최종 증거는 fixture가 아니라 Xbot 화면이다: 왼팔 상완–전완–손에 body 3개를 붙이고 팔꿈치(상완–전완) 관절을 끊으면 전완·손이 떨어지고(손목 관절은 유지), 상완과 섞인 팔꿈치 주변 정점이 늘어나는 NDC25 슬라이드 장면이 재현돼야 한다. 늘어남은 결함이 아니라 geometry 처리가 없다는 진단 결과다. M2는 기존 GPU 절단과의 원자적 게시 계약이다. 피부 분리·cap·생성 정점의 지속 스키닝 전체는 M1 완료 조건이 아니며 기존 skeletal 절단 거절을 M1에서 해제하지 않는다.

## 2. 확정 정책

BC-00 자산 정적 확인: `Xbot.glb`의 scene 0을 `WalkGraph`와 같은 DFS 순서로 순회하면 70 nodes이며, NodeId 13/14/15는 `mixamorig:LeftArm`/`LeftForeArm`/`LeftHand`, parent는 12/13/14다. Debug Viewer 런타임에서도 70 nodes, NodeId 13→14→15와 parent 12/13/14를 확인했다. `RunXbotArmChecks`는 런타임에서도 node 수·parent를 출력하고 이 구조와 다르면 실패한다. 이름은 런타임에 없으므로 기대 이름으로만 출력한다.

| 항목 | v1 결정 |
| --- | --- |
| 소유 | GameObject/ModelInstance 1개 유지. 본별 body/constraint는 공유 geometry가 아닌 인스턴스별 |
| 기본 동작 | body를 로드 시 Kinematic으로 생성. body는 존재하며 query에 참여, 중력에 의한 이동은 없음 |
| 분리 | 선택 constraint만 제거. 자식 body subtree를 Dynamic으로 전환. 이미 Dynamic인 body의 pose/속도 유지 |
| pose 제어 | Animation/Physics 두 상태만 제공(weight 0/1). 중간 blend와 full-ragdoll UI는 후속 |
| 관절 종류 | Jolt SwingTwist. 모터는 초기부터 Off, 제한은 유지. 나중에 모터를 추가해도 break 시 두 모터 모두 Off |
| 초기 자산 | 프로그램으로 생성한 3 body fixture와 Xbot 왼팔 3 body rig. Xbot은 BC-00에서 검증한 고정 런타임 NodeId(현재 13→14→15 = `mixamorig:LeftArm`→`LeftForeArm`→`LeftHand`)를 쓴다. 런타임 모델은 본 이름을 보존하지 않으므로 등록 시 parent 관계·node 수로 자산을 확인하고 다르면 거절한다. 파일 포맷·범용 본 이름 로더·자동 PhysicsAsset 생성은 추가하지 않음 |
| 형상/질량 | fixture는 Box half extent와 명시적 양수 MassKg. Jolt가 box 관성 계산. 기존 Static 질량 공식을 임의 재사용하지 않음 |
| 단위/scale | m, kg, s, rad. 물리는 월드 단위를 m로 본다. 본/frame 변환은 uniform scale까지 허용하고 비균일·shear는 거절한다(6절 scale 분리). Xbot은 Armature 배율 0.01과 뷰어 시작 정규화 배율이 곱해져 본 pose에 scale이 들어 있으므로 scale=1 전제로는 등록할 수 없다 |
| topology | 본 ancestry와 일치하는 body tree. body 없는 중간 본 허용. cycle·외부 pin·교차 연결·weld는 거절 |
| 충돌 | floor/타 owner와 접촉. 같은 rig의 직접 인접 body pair는 break 후에도 제외, 비인접 pair는 허용. 현재 `PhysicsLayers`는 레이어 필터만 있으므로 pair 제외 필터는 BC-06에서 구현한다 |
| 편집 | 등록된 rig owner의 transform/scale·Undo·삭제 경로를 통제. 이동 시험은 별도 진단 API로 수행 |
| 처리 위치 | scene 스레드, Jolt Update 밖에서만 body/constraint 변경. Jolt world/step은 Static과 공유 |

인접 pair 영구 제외는 M1의 초기 겹침 방지 정책이며 UE와 동일하다고 주장하지 않는다. 분리 pair 접촉 재활성화는 후속. 캐릭터 이동 캡슐은 이 기능의 필수 요소가 아니다.

## 3. 현재 코드에서 반드시 보존할 사실

- `ModelInstance::Update`가 animation 평가와 palette 생성을 함께 한다. `Scene::Update`는 그 직후 MeshComponent의 GPU 준비까지 한다. 물리 step을 이 사이에 삽입하려면 분리해야 한다.
- `GraphNode`는 직렬화 96바이트 DFS 레이아웃이다. 필드 추가/순서 변경·mini 포맷 변경 금지.
- `matrixIdx`는 결과 node transform 배열의 인덱스다. DFS 위치와 같다고 가정하지 않는다.
- `m_JointIndices`는 skin별 palette를 이어 붙인 배열이다(`ModelConvert`의 skin loop). 같은 NodeId가 여러 PaletteIndex에 등장할 수 있다. NodeId로 body를 식별하고 palette는 모든 해당 entry를 갱신한다.
- 렌더 변환은 `Wc * inverse(SourceFrame) * Nmesh * Gjoint * IBM`이다. joint world를 `owner * Gjoint`로 단정하지 않는다.
- 조회용 ColliderHandle와 실제 BodyHandle는 별개다. Static 작업의 handle·PImpl·world를 재사용하며 새 Jolt world를 추가하지 않는다.
- 현재 프레임 순서는 `ModelViewer`에서 `PhysicsSystem::AdvanceSimulation`(자체 substep 루프) 다음에 `Scene::Update`(animation→palette→GPU 준비)다. 그대로 두면 물리 결과를 animation이 덮어쓴다. 9절 순서로 바꾸는 것이 BC-11이다.
- `PhysicsSystem::m_World`는 private이며 `CutTransaction`은 friend로 접근한다. `SkeletalPhysicsSystem`은 `PhysicsSystem`의 멤버로 두어 같은 world를 쓰고, Scene/ModelViewer에 world를 공개하지 않는다.
- `PhysicsWorld`는 BC-05에서 기존 Static/Dynamic에 Kinematic·MoveKinematic·운동 전환·impulse를 추가했다. constraint는 BC-06에서 추가한다.
- 시작 시 HEAD·diff·선행 단계 완료 여부를 다시 확인한다.

## 4. 파일과 의존 방향

| 파일 | 역할 / 의존 |
| --- | --- |
| `MiniEngine/Model/ModelPose.h` | 기존 pose 평가 보존, 새 helper 호출점 |
| `MiniEngine/Model/SkeletonPose.h` (신규) | NodeId, runtime parent map, node override 평가. Math/STL만 사용, header-only로 CPU 검사 가능 |
| `MiniEngine/Model/Model.h/.cpp` | animation 평가/최종 pose 확정 API 분리, palette 갱신. Jolt 의존 금지 |
| `MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.h/.cpp` (신규) | 엔진 중립 rig 정의/검증. NodeId는 uint32 값이며 Model 헤더 의존 금지 |
| `MiniEngine/Physics/PhysicsWorldConstraints.cpp` (신규) | PImpl 내부 Jolt constraint 생성·제거·모터·상태. Static의 `PhysicsWorldInternal.h` 사용 |
| `MiniEngine/Physics/PhysicsWorldBodies.cpp` | 공통 body API에 Kinematic target·motion 전환·속도/impulse 확장 |
| `MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.h/.cpp` (신규) | owner↔rig↔본↔body 매핑, generation, break 준비/commit, pose writeback |
| `MiniEngine/ModelViewer/Scene/Scene.h/.cpp` | CPU pose 단계/GPU 준비 단계 분리. PhysicsWorld 소유하지 않음 |
| `MiniEngine/ModelViewer/ModelViewer.cpp` | 고정 step 프레임 조율·진단 옵션. world step의 소유자는 공통 PhysicsSystem 하나 |
| `MiniEngine/ModelViewer/Scene/tests/SkeletalPhysicsChecks.h` (신규) | CPU fixture와 대응하는 viewer 진단/overlay |

신규 cpp/h는 해당 vcxproj/.filters, Physics의 CMake에 등록한다. 구현 함수 위 1~2줄·내부 기능 단락 위 한 줄 한글 주석과 빈 줄을 작성한다.

## 5. CPU pose 계약 (BC-01~03)

`SkeletonPose.h`의 제안 API는 다음과 같다. 기존 C++17이므로 span을 도입하지 않는다. 이름은 이 문서대로 시작하되 기존 충돌이 있으면 이유를 문서화한다.

```cpp
using NodeId = uint32_t; // invalid = UINT32_MAX
struct NodePhysicsPose { NodeId Node; Math::Matrix4 ModelPose; };
bool BuildPoseParents(const GraphNode* graph, uint32_t count,
    std::vector<NodeId>& effectiveParents);
bool EvaluatePoseOverrides(const GraphNode* graph, uint32_t count,
    const std::vector<NodeId>& effectiveParents,
    const NodePhysicsPose* overrides, uint32_t overrideCount,
    Math::Matrix4* finalModelPose);
```

effectiveParents는 matrixIdx 기준이다. `skeletonRoot`와 실제 최상위 node의 parent는 invalid로 표시한다. DFS는 기존 evaluator의 sibling stack 규칙을 그대로 보존한다. 중복 matrixIdx·범위 초과·비정상 종료·depth 초과·중복 override를 거절하며 실패 시 호출자 출력은 변경하지 않는다(임시 vector에서 검증 후 swap/copy).

parent-first 순회에서 override node는 지정한 model pose를 그대로 출력한다. override 없는 node는 `Gfinal[parent] * Lanim[node]`, effective root는 `Lanim[node]`다. 따라서 물리 본 아래 body 없는 자손은 local animation을 유지하며 따라간다. shared GraphNode와 animation local pose를 물리 값으로 덮어쓰지 않는다.

ModelInstance에는 `EvaluateAnimation(dt)`, `FinalizePose(overrides,count)`를 추가하고 기존 `Update(dt)`는 두 함수를 override 없이 호출하는 호환 wrapper로 둔다. Finalize 후에만 `HasValidPose=true`. 오류면 false이고 GPU 준비를 생략한다. `FinalizePose`에서 모든 palette entry에 `Gfinal[m_JointIndices[i]] * IBM[i]`와 normal transform을 생성한다.

Scene에는 `EvaluateAnimations(dt)`, `PrepareMeshes(context)`를 추가하고 기존 Update wrapper를 유지한다. SkeletalPhysicsSystem의 writeback은 `Scene::FinalizeObjectPose(ObjectId, overrides,count)` 같은 좁은 전달 API를 사용한다. mutable ModelInstance를 외부에 공개하지 않는다.

## 6. rig 데이터와 물리 frame (BC-04~08)

BC-00 측정 경로: `-xbot-arm-checks 1`은 기본 장면의 정규화된 Xbot 배치를 사용한다(`-model` 생략). 각 skinned section의 실제 `meshCBV` node를 Nmesh로, `GetWorldTransform`을 Wc로, `GetDeformBinding().SourceFrame`을 SourceFrame으로 사용하므로 대체 frame은 없다. section 간 E 일치를 매 tick 검사한다. 장면을 변경하지 않는 별도 `ModelInstance`에서 animation 0을 dt=1/60초, 3,600 ticks(시작 포함 3,601 표본) 재생하며 클립 길이가 60초를 넘으면 실패한다.

열 길이 3개와 shear=`max(abs(dot(normalized columns)))`의 시작값·전체 min/max, shoulder→elbow 및 elbow→wrist 길이(m)를 출력한다. Debug 실측에서 세 본의 uniform s≈0.0177327(Armature 0.01 × 뷰어 정규화 배율), shear≤1.1e-6이다. 두 skinned section은 공통 E를 공유한다. 상완(shoulder→elbow)은 약 0.4937m, 전완(elbow→wrist)은 약 0.5023m이며 scale=1로 가정하지 않는다.

`SkeletalPhysicsAsset`는 vector<BodyDefinition>, vector<JointDefinition>을 가진다. body ID와 joint ID는 자산 내 uint32 인덱스, invalid=UINT32_MAX. runtime handle은 단조 증가 uint64, 0 invalid이고 해제 후 재사용하지 않는다.

BodyDefinition 필드: `Node`, `BoneFromBody`(body 원점→bone의 강체 offset), `HalfExtentMeters[3]`, `MassKg`. JointDefinition 필드: `ParentBody`, `ChildBody`, `ParentBodyFromJoint`, `ChildBodyFromJoint`(joint→각 body 원점 frame), `SwingHalfAngles[2]`, `TwistMin/Max`(rad). 양수 extent/mass·유한 입력·정규화 회전·유효 angle 범위·ID·tree·node ancestry를 검증한다.

BC-04a API: `MiniPhysics::SkeletalRigidFrame`은 double 위치(m)·xyzw 회전만 저장하며 Model/Jolt에 의존하지 않는다. `ValidateSkeletalPhysicsAsset(asset, nodeParents)`는 NodeId별 부모 배열(invalid=`UINT32_MAX`)을 읽어 bool을 반환하고 입력을 변경하지 않는다. 회전 제곱 길이 오차 ≤1e-4, swing [0,π], twist -π≤min≤max≤π를 허용하며 모든 회전 자유도를 잠근 weld 설정은 거절한다. 비어 있지 않은 단일 body tree, 본당 body 하나, 각 child의 가장 가까운 body 조상과의 관절 하나를 요구한다. 따라서 body 없는 중간 본은 허용하고 cycle·외부 pin·교차 연결·중간 body를 건너뛴 연결은 거절한다. 부모 배열 전체의 ID·cycle도 검사한다. 실제 fixture/Xbot 생성 값은 BC-04b에서 추가한다.

M1 fixture는 Node 0→1→2→3(body 없는 tip), 0~2에 각 1kg box를 둔다. 반크기 (0.1,0.25,0.1)m, bone은 box 중심에 두고 BoneFromBody=identity. rest 중심 Y=(2,1.5,1)m, joint 위치 Y=(1.75,1.25)m, body-local anchor Y=(-0.25,+0.25)m다. floor는 Y=0. swing half-angle π/3, twist 범위 ±π/4, 정지 rest에서 시작한다.

BC-04b 생성 API는 `CreateThreeBodyFixture()`와 `CreateXbotLeftArmAsset(nodeParents, output)`다. fixture는 자산·부모 배열·4개 본의 월드 rest 강체 frame을 반환하고 tip Y=0.5m다. Xbot 생성은 node 수 70과 parent[13/14/15]=12/13/14를 확인하고 공통 검증을 통과한 경우에만 출력에 게시한다. 실패하면 기존 출력은 보존한다. 이름/메시를 검사하는 범용 자산 식별 API는 아니다.

| Xbot body ID / Node | 반크기 XYZ (m) | 질량 (kg) | BoneFromBody 위치 (m) |
| --- | --- | --- | --- |
| 0 / 13 상완 | (0.2468535, 0.07, 0.07) | 2.0 | (+0.2468535, 0, 0) |
| 1 / 14 전완 | (0.251174, 0.06, 0.06) | 1.5 | (+0.251174, 0, 0) |
| 2 / 15 손 | (0.09, 0.07, 0.035) | 0.5 | (+0.09, 0, 0) |

상완·전완 길이는 BC-00의 월드 길이를 반올림한 0.493707/0.502348m다. GLB의 LeftForeArm/LeftHand 정적 translation도 각 부모 본 +X 방향임을 확인했다. 손 길이 0.18m·폭·두께·질량은 자동 추정이나 실측이 아닌 M1용 명시적 proxy 값이다. 모든 회전 offset은 identity이며 body 중심은 본 +X 반길이에 둔다. 각 관절의 parent anchor는 부모 +X 반길이, child anchor는 자식 -X 반길이다. 따라서 자식 본이 회전해도 child anchor는 자식 본 원점이다. 정적 GLB translation×BC-00 s=0.0177327581과 이 반올림 anchor의 차이는 CPU 시험에서 0.1mm 이내다. animation 전 구간·solver의 실제 anchor 검증은 후속 단계에 남긴다.

관절 0(팔꿈치 0→1)은 양쪽 swing half-angle 150°(5π/6), 관절 1(손목 1→2)은 60°(π/3), 둘 다 twist ±90°(±π/2)다. 팔꿈치 굽힘·손목 굽힘/편위·전완 회전에 여유를 주는 넓은 인체 팔 proxy 범위를 택했다. 한쪽으로만 굽는 해부학적 팔꿈치를 정밀 재현하는 제한은 아니다. BC-00 native 축의 절대 상대 자세를 유지하려고 joint rotation을 identity로 두며 +X가 twist 축이다. fixture와 Xbot은 Model/Jolt 의존 없이 기존 자산 파일 안에 구현하여 새 파일·target은 추가하지 않는다.

frame은 열벡터 기준 `E = Wc * inverse(SourceFrame) * Nmesh`, 일반식은 `B = R(E * Gnode) * BoneFromBody`이고 R은 아래 scale 분리의 강체 부분이다. s=1이면 `B = E * Gnode * BoneFromBody`, writeback `Gnode = inverse(E) * B * inverse(BoneFromBody)`로 줄어든다. 관여하는 skinned section들의 E가 같은지 검사한다. 서로 다른 E·비균일 scale·shear 및 Nmesh 자체가 물리 override의 영향을 받는 구성이면 `UnsupportedFrame`; 자동 근사하지 않는다. E는 매 animation 평가 후 계산하되 분리 body의 world pose는 owner 이동과 무관하게 유지한다. body 없는 fixture는 owner world를 E로 사용한다.

scale 분리: `E * Gnode`는 uniform scale s를 포함할 수 있다(Xbot은 Armature 0.01 × 뷰어 정규화 배율). 등록 시 각 body node의 s를 `E * Gnode`의 열 길이로 구하고, 세 열 길이 차이가 1e-4 상대 오차를 넘거나 shear가 있으면 `UnsupportedFrame`이다. 물리에는 강체 부분 `R(E * Gnode)`만 보내고 `BoneFromBody` offset·half extent·anchor는 월드 m 값으로 저장한다. writeback은 강체 결과에 등록 시 저장한 s를 다시 곱해 `Gnode = inverse(E) * (B * inverse(BoneFromBody)) * S(s)`로 복원한다. 등록 뒤 owner scale 변경은 2절 편집 통제로 막는다. Xbot rig 크기는 BC-00에서 화면 기준 월드 m로 측정해 정한다.

Body 원점 pose는 `GetWorldTransform`/`GetPosition` 의미로 주고받는다. joint frame은 Jolt `LocalToBodyCOM`으로 만들며 body-origin anchor에서 shape COM을 빼고 축은 그대로 전달한다. COM offset을 가진 추가 fixture로 왕복을 검증한다.

기본 body는 Kinematic으로 생성한다(Static이 아님). shape 질량/관성을 미리 준비하고 Dynamic 전환 가능성을 CPU 테스트한다. kinematic target은 `MoveKinematic(..., fixedDt)`로 갱신한다. 일반 프레임마다 teleport하지 않는다. 첫 등록은 초기 pose로 놓고 속도 0, 이후 속도는 COM 기준 선속도와 월드 각속도로 저장한다.

SwingTwist constraint는 미리 생성하되 두 endpoint가 모두 Kinematic이면 `SetEnabled(false)`로 둔다. 한 endpoint라도 Dynamic이고 unbroken이면 enable한다. Broken은 world에서 제거하며 자동 재추가하지 않는다. 논리 unbroken 개수와 solver enabled 개수는 따로 진단한다.

SDK 근거: [BodyInterface](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Body/BodyInterface.h), [BodyCreationSettings](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Body/BodyCreationSettings.h), [SwingTwistConstraint](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Constraints/SwingTwistConstraint.h). Unreal의 관절 종료/자손 전환 근거는 조사 문서 2절 링크를 따른다.

## 7. 공통 PhysicsWorld 확장 계약

BC-05 구현: 기존 BodyHandle·PreparedBody·COM 속도 조회·Moving 계층을 재사용했다. 새 API는 `MoveKinematic`, `SetBodyMotion`, `SetBodyVelocity`, `AddBodyImpulse`이며 모두 world에 올라간 body만 변경한다. 전환은 비정적 body의 Kinematic↔Dynamic만 허용하고 pose·속도·shape·handle을 보존한다. 두 방식이 같은 Moving 계층을 쓰므로 전환 시 레이어 교체는 필요 없다. Static은 기존 생성·동작을 유지한다.

Kinematic 생성과 shape 교체는 기존 공통 질량 계산과 shape 관성을 준비하여 Dynamic 전환에 사용한다. skeletal 자산의 명시적 질량 적용은 후속 등록 경로에서 다룬다. 속도 설정은 기존 준비 속도 제한을 재사용한다. MoveKinematic은 원점 목표에서 Jolt가 COM 목표를 계산하며, float 길이 제곱 오버플로 방지를 위해 목표 선속도 또는 보수적 각속도 상계가 1e18을 넘으면 거절한다. impulse도 결과 선속도·각속도의 길이 제곱이 유한한지 먼저 확인하고 Jolt의 제한·활성화 API에 전달한다.

Static 단계 4a~4d의 BodyHandle·RAII·Create/Register/Remove/Destroy·Step·floor 구현을 먼저 재사용한다. 다음 메서드는 그 API에 추가할 의미 계약이며 같은 기능이 있으면 중복 wrapper를 만들지 않는다.

| 동작 | API 의미 |
| --- | --- |
| Kinematic 구동 | `MoveKinematic(handle, originPose, fixedDt)`; Kinematic만 허용, dt>0 |
| 전환 | `SetBodyMotion(handle, Kinematic/Dynamic)`; world/layer/filter 동기화, 기존 body handle 유지 |
| 속도 | `Get/SetBodyVelocity`는 COM 선속도(m/s), 월드 각속도(rad/s) |
| impulse | `AddBodyImpulse(handle, worldJ, worldPoint)`; 유한 입력, Dynamic 및 등록 상태 검사 |
| 관절 | `CreateSwingTwist(desc)`, `RemoveConstraint(handle)`, `SetConstraintEnabled`, `SetConstraintMotorsOff` |
| 진단 | body/constraint 수와 상태 조회. Jolt pointer/BodyID를 Scene으로 노출하지 않음 |

RemoveConstraint는 world 참조를 먼저 제거하고 map/RAII의 ref를 정리한다. owner 삭제는 모든 constraint 제거 → body 제거/파괴 → rig entry 제거 순서다. 등록 실패는 등록된 일부 자원도 역순 정리하고 owner pose/geometry를 변경하지 않는다. query fallback body와 본 body의 중복 후보는 handle→owner/node 매핑으로 구별한다.

## 8. break 준비·commit과 세대 (BC-09~10)

BC-00 위치 지표는 배율을 제거한 전완 강체 frame에서 본 손목 위치(m)의 XYZ min/max와 최초 위치로부터의 거리 min/max다. 이는 anchor 거리의 대용치이며 두 물리 body anchor를 직접 비교한 결과가 아니다. 회전은 `inverse(R(E*G14))*R(E*G15)`를 전완 로컬 +X twist 축으로 분해한 절대 상대 자세다(기준 pose 차감 없음). swing은 비음수 총각, twist는 signed angle(rad)이다.

Debug 실측에서 전완 frame의 손목 drift는 최대 약 1.9e-6m(로그 1.92311586e-6m)로 0.1mm 이하다. animation 0(약 1.83초)의 손목 swing은 [0.105,0.126]rad, 전완 로컬 +X 축 twist는 [-0.594,-0.574]rad다. 이는 애니메이션이 사용하는 pose 범위이며 ragdoll 관절 제한이 아니다. Ragdoll 제한은 이 범위를 포함해야 하며 실제 관절 축과 제한은 BC-04에서 결정한다. PASS의 `animation pose range`는 swing/twist 측정 범위에 0.05rad 여유를 더한 값이다. `anchorTolerance=max(1e-4m,2*driftMax)`는 drift 대용 지표일 뿐 두 body anchor 검증값이 아니다.

BC-04b에서는 6절의 native +X/identity 관절 frame과 손목 swing π/3≈1.0472rad, twist ±π/2≈±1.5708rad를 채택했다. swing 최대 0.126rad와 twist [-0.594,-0.574]rad를 충분히 포함하며 CPU 자산 시험에서 포함 관계를 확인한다. 이는 애니메이션 최대치에 근소한 여유만 더한 제한이 아니라 넓은 M1 팔 proxy 설정이다.

```cpp
struct BreakRequest {
    ObjectId Owner;
    uint32_t JointId;
    uint64_t ExpectedRigGeneration;
    uint64_t ExpectedTopologyGeneration;
    std::array<float, 3> WorldImpulse; // kg*m/s
    std::array<float, 3> WorldPoint;   // m
};
enum class BreakStatus {
    Prepared, Applied, NotFound, AlreadyBroken, Stale,
    UnsupportedTopology, UnsupportedFrame, UnsupportedPose, InvalidInput, NotReady
};
```

M1(BC-10)은 fixed substep 경계에서 검증과 commit을 한 번에 하는 동기 `BreakConstraint(request)`로 시작한다. 검증 항목과 상태값은 아래와 같고, 별도 일회용 plan과 snapshot 세대 키는 GPU 절단과 묶이는 BC-09(M2)에서 분리한다.

아래 plan 문단은 BC-09/M2 전용이다. Scene 계층의 `PrepareBreak(request)`는 변경 없이 일회용 plan을 만든다. plan은 owner/rig/topology/pose 세대·E·joint·대상 body/내부 joint 목록·전환 pose/속도와 사전 확보한 저장 공간을 소유한다. commit 직전 `ValidatePreparedBreak`로 모두 다시 검증한다. plan 복사 금지, consume 후 재사용 거절. 수동 요청은 fixed substep에서 prepare/commit하고, 이전 substep의 plan을 새 pose에 그대로 적용하지 않는다.

RigGeneration은 등록/교체마다 새 번호, TopologyGeneration은 성공한 break마다 +1, PoseGeneration은 최종 pose 확정마다 +1이다. ObjectId와 함께 검사하며 Clear로 카운터를 되돌리지 않는다. Queue에는 raw pointer 대신 ID·요청값만 저장한다.

subtree는 자식 body의 node ancestry로 정하고 이미 끊긴 경계는 연결 탐색에서 제외한다. 이미 Dynamic인 자손의 상태는 유지한다. 동일 joint가 Broken이면 `AlreadyBroken`을 우선 반환하며 impulse를 추가하지 않는다. 다른 요청으로 세대가 바뀌면 `Stale`다. 첫 구현은 모든 기존 break가 ancestry에 일치하는 tree만 지원한다.

Dynamic으로 전환할 내부 관절의 양쪽 anchor 거리 ≤1e-4m, 상대 자세가 설정 limit 안인지 prepare에서 검사한다. Kinematic은 이번 animation target pose, 기존 Dynamic은 현재 body pose로 검사한다(`MoveKinematic`은 즉시 target에 teleport하는 API가 아님). 벗어나면 `UnsupportedPose`로 거절한다. 이 검사는 UE에 없는 자체 제한이다. Xbot 데모는 BC-00에서 재생 animation 전 구간의 손목 상대 자세와 anchor 거리를 측정하고, limit과 거리 허용값(월드 m)을 그 측정값으로 정한다. 측정값이 0.1mm를 넘으면 허용값을 문서에 기록하고 조용히 늘리지 않는다. simulation을 켠 순간 snap으로 애니메이션 오차를 강제로 교정하지 않는다. 전환하는 Kinematic만 target pose로 동기화한 뒤 저장한 COM/각속도를 전환 후 복구한다.

commit 순서: 재검증 → 선택 joint 제거 → 대상 body Dynamic 전환 → 내부 관절 enable/모터 Off → 해당 본 제어권 Physics → body wake → 선택 joint의 ChildBody에 impulse 1회 → generation 증가. 이 구간은 사전검증·예약 이후 할당이나 fallible cook 없이 실행한다. preflight 뒤 실패 가능한 API가 남으면 그 단계에서 commit 가능으로 표시하지 않는다.

## 9. 고정 step 프레임 연결 (BC-11)

고정 dt/누적기/최대 catch-up 수는 Static 공통 step 정책을 그대로 쓴다. Scene별 별도 step 또는 한 프레임 두 번 step하지 않는다. animation도 substep dt로 한 번씩 진행한다.

```text
fixed substep마다:
  삭제/등록 요청 정리
  Scene.EvaluateAnimations(fixedDt)
  rig frame 검증 + Kinematic target 갱신
  대기 중 BreakConstraint 요청 검증 후 즉시 commit (M2에서는 prepared break 재검증/commit)
  공통 PhysicsWorld.Step(fixedDt) 한 번
  Static body→component 반영
  Skeletal body→node override 수집, 모든 owner FinalizePose
render frame마다:
  Scene.PrepareMeshes(context) 한 번 → Render
```

substep이 0회면 이전 최종 pose를 재사용하고 animation을 추가 진행하지 않는다. 최초 준비는 dt=0 animation 평가→Finalize 후 수행한다. 기존 비물리 모드는 기존 Update wrapper로 동일 동작을 유지한다. Physics on에서만 이 고정 step 경로를 사용한다.

## 10. GPU 통합 지점 (BC-14, M2)

CPU 명시적 break는 GPU와 무관하게 시험한다. 최종 절단에서는 physics plan과 GPU 결과가 모두 준비되기 전까지 관절을 변경하지 않는다. 기존 Static의 PrepareGeometryReplacement/CutTransaction 기반이 완료된 뒤 확장한다.

snapshot key는 ObjectId+MeshId+GeometryRevision+Geometry/EditGeneration+RigGeneration+TopologyGeneration+PoseGeneration+E다. 첫 통합은 animation과 대상 physics 그룹을 명시적으로 정지한 진단 상태에서만 허용한다. 대기 중 pose가 바뀌면 Stale 취소. 실행 중인 캐릭터를 매번 조용히 정지시키지 않는다.

`PrepareSkeletalCut`는 geometry A/B, 제외 목록, 조각별 deform binding, bounds, PreparedBreak를 함께 소유한다. CPU 준비·GPU fence 완료 → 같은 경계에서 모든 key 재검증 → geometry 준비된 commit과 break commit → 새 상태 공개 순서다. 실패하면 이전 geometry/관절을 유지한다.

이 단계는 수명/commit 인터페이스까지 정의한다. 실제 patch 선택·혼합 weight 보정·교점 출처·cap 변형은 별도 GPU 구현 단계로 설계해야 한다. 그 검증 전에는 `CuttingSystem::Submit`의 skin 거절을 유지한다. 모든 M1 완료를 skeletal slicing 완료로 기록하지 않는다.

## 11. 약 300줄 단위 실행 순서

비용은 기존 측정 기준: 리뷰 R=2~5분, Debug/Release 빌드 B≈90초 합계, 기존 CPU 검사 T≈5초, 기존 viewer 실행 V≈8초/회. 구현 시간·신규 테스트 시간은 미측정이며 단계별 기록한다. 300줄을 넘으면 helper/API와 적용/검사를 분리하고 행을 추가한다.

| 단계 | 변경 파일·한 개념 | 완료 증거 / 선행 | 기준 비용 |
| --- | --- | --- | --- |
| BC-00 | Xbot 진단 로그만 추가(동작 변경 없음): 왼팔 NodeId·parent, `E * Gnode` scale/shear, 본 길이(월드 m), 재생 중 손목 anchor·상대 자세 범위 | 로그 값을 이 문서 2·6·8절에 기록 / 지금 가능 | R+B+V |
| BC-01 | SkeletonPose.h parent map + Model/tests/SkeletonPoseTests.cpp | 구현 완료(uncommitted): CPU 16/16 PASS, Debug 재빌드는 Jolt 외부 캐시 권한 차단 / 선행 00 완료 | R+CPU 1.19초 |
| BC-02 | override evaluator + 같은 테스트 | 구현 완료(uncommitted): CPU 32/32 PASS; override=0 동등, 물리 본 고정·자손 local·skeletonRoot·실패 시 출력 보존 / 01 | R+CPU 0.91초 |
| BC-03a | ModelInstance animation/finalize 분리 | override=0 pose/palette 동등, 중복 palette node 갱신 / 02 | R+B+T |
| BC-03b | Scene CPU/GPU 단계 분리·wrapper | 비물리 Debug/Release 실행·애니메이션 회귀 / 03a | R+B+V |
| BC-04a | SkeletalPhysicsAsset 중립 API·검증·CPU 거절 시험 | 완료 `8593a0be`: CPU 42/42·최적화 42/42 PASS, Debug/Release 빌드 PASS / 00,01 | 최종 B=6.89+7.41초 |
| BC-04b | fixture·Xbot 왼팔 rig 값·CPU 자산 시험 | 완료 `d31863a7`: CPU 82/82·최적화82/82, Debug/Release PASS / 04a | B=105.21+119.91초, CPU=1.19+3.37초 |
| BC-05 | 공통 body Kinematic·Dynamic·impulse API/시험 | 구현·검증 완료(미커밋): CPU 362/362 두 구성, Debug/Release 빌드·비물리 컴포넌트 PASS / 04b | B=40.82+136.18초, CPU=7.63+10.26초 |
| BC-06 | constraint 수명/enable/motor API + rig 인접 pair 충돌 제외 필터 | 3 body·2 joints 생성/삭제·kinematic disable·인접 pair 무접촉 / 05 | R+B+T |
| BC-07 | SkeletalPhysicsSystem 등록/해제(PhysicsSystem 멤버) | 부분 실패 역정리·삭제 뒤 자원0 / 04~06 | R+B+T |
| BC-08a | rig frame·scale 분리·Kinematic target/속도 | body offset/COM 왕복, uniform scale 왕복, 비균일/shear 거절 / 07 | R+B+T |
| BC-08b | physics pose→node→palette 반영 | parent 이동 상쇄·tip 상속·scale 복원 / 03b,08a | R+B+T |
| BC-10 | 동기 BreakConstraint(검증+commit) | body수3 유지·unbroken 2→1·손목 유지·impulse 1회·AlreadyBroken 무추가 / 08b | R+B+T |
| BC-11 | 공통 fixed step 연결(9절 순서로 프레임 재배치) | 전체 world step 횟수 일치·GPU 준비1회·비물리 모드 동등 / 10 | R+B+V |
| BC-12a | fixture 진단 옵션·GizmoDraw overlay | 3 body/본/anchor 시각화·정지/분리 재현 / 11 | R+B+V |
| BC-12b | Xbot 왼팔 rig 등록·팔꿈치 break 데모 | 상완 animation 유지·전완/손 낙하·팔꿈치 피부 늘어짐 화면, 스크린샷 / 12a | R+B+V |
| BC-13a | 기본 수명 회귀(M1) | 등록 실패·씬 clear·삭제 직후 지연 명령·double break·리소스0 / 12b | R+B+T+V |
| BC-09 | (M2) PrepareBreak 일회용 plan·snapshot key 분리 | prepare 무변경·Stale / 13a | R+B+T |
| BC-14 | (M2) GPU transaction 연결 계약 구현 | 미지원 skin 유지, 준비 실패 시 break 안 함 / 09 | R+B+T+V |

M1은 BC-00~BC-13a다. BC-09·BC-14는 M2이며 이번 데모에 필요 없다.

## 12. 검증 명령과 판정

BC-05 검증(2026-10-29, 기준 `d31863a7`, 미커밋): `PhysicsBodyTests.cpp` 머리말 직접 cl 명령으로 최종 Debug 362/362(7.63초), Release `/O2 /DNDEBUG` 362/362(10.26초)를 통과했다. 둘 다 컴파일 경고 없이 종료0이며 산출물은 `$env:TEMP/BreakConstraintTests`다. 기존 body 검사와 함께 Kinematic 중력 무시·Moving 계층·오프셋 COM 회전 목표·순간이동 없음·같은 handle/BodyID·전환 pose/속도·준비 질량/관성·shape 교체·낙하·잠든 body의 off-center impulse·무효 입력/상태 거절·자원0을 검사한다. 기존 PhysicsBodyTests target을 확장했으므로 CMake·BuildJolt·vcxproj 등록은 추가하지 않았다. CTest 전체 실행은 하지 않았다.

전체 ModelViewer Debug/Release를 저장소 루트에서 `/restore` 없이 빌드하여 40.82/136.18초, 각각 종료0·오류0이다. 기존 C4819 경고 18/20개와 vswhere/pwsh 탐색 메시지는 남아 있다. MiniEngine/ModelViewer에서 두 구성 모두 `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`으로 실행하여 6.32/7.31초, 종료0 및 `PASS GPU preparation`을 확인했다. DirectX debug layer를 켰으며 adapter/driver는 이번에 재측정하지 않았다. 새 API의 viewer 호출은 아직 없다.

자기 리뷰에서 왕복 전환 및 잘못된 목표 요청 뒤 속도 보존 시험 부족 1건을 보강하고 두 CPU 구성을 재검증했다. 잔여 지적0건이며 diff --check PASS다. 초기 개발 중 벡터의 함수 선언 해석 오류를 중괄호 초기화로 수정했다(첫 Debug 빌드 93.22초 실패). 시험의 Kinematic 상태에서 Dynamic 전용 질량 조회 오류는 전환 뒤 조회하도록 수정했으며, CPU 재검증 스크립트의 인자 배열 해제 오류(LNK1181)는 명시적 배열로 고쳤다. 최종 시험·빌드·실행은 모두 통과했다. 로그는 `BC05-CPU-*-final.log`, `BC05-*-build-final.log`, `BC05-*-component-checks.log`, `BC05-self-review.txt`다. BC-05에서 중단하며 커밋·병합·후속 구현은 하지 않는다.

BC-04b 검증(2026-10-29, HEAD `8593a0be`): 테스트 머리말의 직접 cl 명령과 `/O2 /DNDEBUG` 구성 모두 82/82 PASS, 종료 코드 0, 컴파일 경고 없음이다(최종 컴파일+실행 1.19/3.37초). 기존 검사 42건에 생성 fixture·Xbot 값·부모 구조 거절·실패 출력 보존·BC-00 손목 범위 포함·회전된 자식 anchor/정적 GLB 반올림 오차 검사를 추가했다. worktree 루트의 전체 ModelViewer Debug/Release를 `/restore` 없이 빌드하여 각각 105.21/119.91초, 종료 코드 0을 확인했다. 기존 SkeletonPose.h·GizmoDraw.h 등의 C4819와 vswhere/pwsh 탐색 메시지는 남아 있으며 새 CPU 파일 컴파일에는 경고가 없다. CTest와 viewer 실행은 미수행이다. 새 생성 API는 tests 외 런타임 호출이 없어 물리를 끈 기존 경로의 실행 코드는 바뀌지 않는다.

별도 diff 읽기 자기 리뷰에서 Xbot anchor 시험이 local 값 대조에만 머문 지적 1건을 보강하고 CPU 두 구성을 재검증했다. 최종 잔여 지적 0건이다. 산출물·전체 빌드 로그·자기 리뷰 메모는 `$env:TEMP/BreakConstraintTests`의 `SkeletalAssetTests*.exe`, `BC04b-*-build.log`, `BC04b-self-review.txt`다. 사용자 지시에 따라 BC-04b에서 중단하며 커밋·병합·파일 삭제·junction 수동 변경 및 BC-05 이후 구현은 하지 않았다. 기존 .mini와 BreakConstraintReport.md는 보존했다.

BC-04a 검증(2026-10-29, HEAD `71e115e0`): `SkeletalAssetTests.cpp` 머리말의 직접 cl 명령으로 TEMP/BreakConstraintTests에만 CPU 산출물을 저장했다. 최종 42/42 PASS, /O2 /DNDEBUG에서도 42/42 PASS, 모두 종료 코드 0·컴파일 경고 없음이다. CMake 테스트 target과 BuildJolt.ps1 testTargets 등록은 완료했으며 CTest 경로 실행은 이번에 수행하지 않았다. 전체 ModelViewer Debug/Release를 /restore 없이 빌드하여 최초 22.51/7.44초, 최종 6.89/7.41초, 모두 종료 코드 0·경고0·오류0을 확인했다. 기존 vswhere 탐색 메시지와 pwsh→Windows PowerShell fallback 메시지는 있었으나 빌드는 성공했다. 기존 실행 경로에서 새 API를 호출하지 않으므로 viewer 실행은 수행하지 않았다.

별도 `codex exec -s read-only --ephemeral`는 `Error finding codex home: Could not find home directory`로 시작하지 못했다. 대체 자기 리뷰에서 한 줄 여러 문장인 시험 매크로와 weld 설정 누락 2건을 수정하고 CPU 검사와 두 구성 빌드를 다시 통과했다. 최종 자기 리뷰 잔여 지적 0건이다. 기존 BC-04a/04b 단계표 분할을 보존했고 git 쓰기 명령·BC-04b 이후 구현은 수행하지 않았다.

BC-01 검증(2026-10-28): branch `break-constraint`, HEAD `d865c5ed` 기반의 미커밋 구현이다. `BuildPoseParents`는 임시 배열에서 유효성과 matrixIdx 유일성을 검증하고 정상 종료 때만 swap한다. `GraphNode`와 `EvaluateModelPose`는 변경하지 않았다. 형제 스택 32칸 경계·33번째 저장 거절·형제 없는 40노드 체인을 검사하여 기존 평가기의 스택 규칙을 보존했다.

아래 명령으로 CPU 테스트 16/16 PASS, 종료 코드 0, 컴파일 경고 없이 빌드·실행 합계 1.19초를 확인했다. 산출물은 `$env:TEMP/BreakConstraintTests`이며 TEMP 쓰기는 차단되지 않았다. 개발자 환경 로드 시 PowerShell 실행 정책 때문에 첫 시도가 컴파일 전에 중단되어 프로세스 범위 Bypass로 재실행했다. 복원 옵션 없는 ModelViewer Debug 재빌드는 Jolt CMake가 `MiniEngine/Build/ThirdParty` junction 대상인 워크트리 밖 캐시에 쓰려다 Permission denied로 실패했다. junction 변경이나 외부 캐시 수정으로 우회하지 않았다. Debug 재빌드는 71.95초 후 종료 코드 1로 끝났으며 `Model.lib` 컴파일은 성공했다. 따라서 전체 Debug 빌드 성공은 미확인이다.

BC-02 검증(2026-10-28): branch `break-constraint`, HEAD `d044de76` 기반의 미커밋 구현이다. `EvaluatePoseOverrides`는 `BuildPoseParents`로 부모 배열과 DFS를 검증한 뒤 임시 pose 배열에서 부모 우선 합성을 수행한다. override 없는 결과는 `EvaluateModelPose`와 모든 행렬 성분이 정확히 일치한다. 부모 애니메이션 변경에도 물리 본 pose가 고정되며 body 없는 자손은 변경된 local 애니메이션을 유지한다. 물리 조상 아래 skeletonRoot 초기화와 skeletonRoot 자체 override를 확인했다. 부모 크기·범위·구조 오류, null 포인터, 빈 그래프, 중복·범위 밖 override, NaN·무한대, 잘못된 DFS를 거절하며 출력 배열 전체를 보존한다. `GraphNode`와 `EvaluateModelPose`는 변경하지 않았다.

테스트 파일 헤더의 명령을 VS Developer PowerShell 환경에서 실행하여 CPU 32/32 PASS, 종료 코드 0, 컴파일 경고 없이 빌드·실행 합계 0.91초를 확인했다. 산출물은 `$env:TEMP/BreakConstraintTests`다. 개발자 환경 로드 중 `vswhere.exe` 경로 탐색 메시지가 있었지만 C++ 도구 초기화와 컴파일·실행은 성공했다. 이번 단계는 CPU 테스트만 실행했으며 전체 viewer Debug/Release 빌드는 수행하지 않았다.

후속 작업: `XbotArmChecks.h`의 인라인 부모 복원을 `BuildPoseParents` 호출로 교체한다. BC-01과 BC-02에서는 이 파일을 수정하지 않았다. BC-03 이후는 구현하지 않는다.

BC-00 검증(2026-10-28): branch `break-constraint`에서 BC-00 구현 완료(uncommitted). Debug/Release 빌드 성공, Debug Viewer 실행의 `XbotArmChecks.log`에서 2 sections·3,601 samples와 PASS를 확인했다. 2·6·8절에 런타임 측정값을 반영했다. GPU validation은 확인하지 않았다. Geometry 코드는 변경하지 않아 CPU slicer 검사는 대상이 아니다.

재검증 명령은 worktree 루트에서 `msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows`(복원 옵션 없음), 이어 `MiniEngine/ModelViewer`에서 `../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -xbot-arm-checks 1`이다. `XbotArmChecks.log`는 매 실행 초기화하고 요약 후 자동 종료한다. 이번 리뷰 수정 후 Debug 재빌드와 Release 재빌드 모두 성공했다(Release 16.38초). UTF-8 BOM 적용 후 헤더 인코딩 경고도 해소했다. 최종 Debug 실행은 7.30초, 종료 코드 0이며 새 로그에서 `BC-00 PASS: sections=2 samples=3601; animation pose range ...`를 확인했다. `-model Models/cube.glb`를 추가한 실패 경로도 6.32초 후 정상 종료하며 `BC-00 FAILED: default Xbot object required (omit -model)`를 기록했다(종료 코드 0). 샌드박스로 차단된 빌드·실행은 없었다. 빌드 중 pwsh 탐색 메시지 뒤 Windows PowerShell fallback이 수행되어 최종 빌드는 성공했다.

진단 예외는 `ModelViewer::Startup`에서 실패 상태와 `BC-00 FAILED: 이유`를 기록하고, 초기화를 마친 뒤 `IsDone`으로 종료하여 Cleanup과 Graphics shutdown을 거친다. 현재 GameCore는 정상 종료 시 항상 0을 반환하며 기존 검사 옵션에도 실패 종료 코드를 전달하는 장치가 없다. 따라서 프로세스 종료 코드만으로 성공을 판정하지 않고 새 로그의 PASS/FAILED를 확인해야 한다. 로그 파일 쓰기 실패 이유는 디버거 출력으로도 남긴다. BC-01은 사용자 검토·커밋 후 별도 단계로 진행한다.

CPU pose 테스트는 Jolt 없이 VS Developer PowerShell에서 실행한다. 산출물은 `$env:TEMP/BreakConstraintTests`에 둔다. BC-01에 테스트 파일 머리말에도 아래 명령을 넣는다.

```powershell
$bcTestDir = Join-Path $env:TEMP 'BreakConstraintTests'
New-Item -ItemType Directory -Force -Path $bcTestDir | Out-Null
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Core /I MiniEngine/Model "/Fo:$bcTestDir/SkeletonPoseTests.obj" "/Fe:$bcTestDir/SkeletonPoseTests.exe" MiniEngine/Model/tests/SkeletonPoseTests.cpp
& "$bcTestDir/SkeletonPoseTests.exe"
```

Physics 테스트 target은 `SkeletalAssetTests`, `PhysicsConstraintTests`, `SkeletalBreakTests`를 담당 단계에서 CMake/BuildJolt.ps1의 testTargets에 각각 등록한다. 예: `./Tools/BuildJolt.ps1 -Configuration Debug -Target SkeletalBreakTests -RunTests`, Release도 동일 실행. Scene 연결 변경은 저장소 지침의 ModelViewer.slnx Debug/Release 빌드를 모두 수행한다.

BC-12a 옵션은 `-skeletal-break-checks 1`로 고정하고 자동 fixture를 사용한다. 기존 모델은 변경하지 않는다. 새 GameObject 하나에 pose fixture를 등록하고 기존 화면의 bone/body 중심·anchor를 view-projection 후 GizmoDraw로 그린다. 60 fixed ticks 후 joint0을 0 impulse로 분리하고 120 ticks 뒤 수치 판정/종료 로그를 출력한다. root Y는 초기값 유지, child COM Y 하강, 살아 있는 손목 anchor 오차≤0.01m, 모든 pose/속도 finite를 검사한다. 별도 off-center impulse CPU 시험은 접촉/관절 없는 단일 body로 수행한다.

BC-12b 옵션은 `-skeletal-break-xbot 1`로 고정한다. 기존 시작 장면의 Xbot에 왼팔 rig를 등록하고 animation을 재생한다. 60 fixed ticks 후 팔꿈치 joint를 작은 impulse로 분리하고 120 ticks 뒤 수치 판정 로그를 출력한다. 판정: 상완 본 pose가 animation 값과 일치, 전완 COM Y 하강, 손목 anchor 오차가 BC-00 허용값 이내, unbroken 2→1·enabled 0→1, 모든 pose/속도 finite. 화면에서는 팔꿈치 피부 늘어짐을 확인하고 break 전·후 스크린샷을 남긴다.

추가 필수 회귀: zero-step frame, 반복 break 무추가 impulse, 삭제 직후 지연 명령 거절, 부모 평행이동/회전 중 분리 본 world 불변, COM offset, 중복 palette, 비물리 모드 동등. 런타임 기록에는 GPU/driver·구성·명령·fixed dt·오차·body/joint 수를 남긴다.

## 13. Codex 구현 세션에 그대로 전달할 첫 지시

```text
Docs/Physics/BreakConstraintImplementation.md를 기준으로 BC-00만 구현해줘.
먼저 git status/HEAD와 코드 작성 세션 인계를 확인하고 기존 변경은 보존해.
Xbot 왼팔(LeftArm→LeftForeArm→LeftHand) 진단 로그 옵션만 추가하고 동작은 바꾸지 마.
NodeId·parent, E*Gnode의 열 길이(scale)·shear, 본 길이(월드 m), 재생 중 손목 anchor 거리와 상대 자세 범위를 출력해.
모든 수정 함수와 내부 기능 단락에는 저장소 지침의 한글 주석을 작성해.
약 300줄 이내로 끝내고 Debug/Release 빌드 후 viewer를 실행해 로그를 얻어.
측정값을 이 문서 2·6·8절에 기록해.
실제 바뀐 범위에 맞는 빌드/검사를 기록하고 이 문서와 README 상태를 갱신해.
변경/검증/미해결 사항을 설명한 뒤 다음 단계로 가지 말고 멈춰. 커밋은 만들지 마.
```

## 14. 문서 검증과 제한

v1은 `367301ec` 기준으로 ModelPose/ModelInstance/Scene/MeshComponent/물리 CMake와 Static 단계 의존을 대조했다. v2(2026-10-28)는 `5f56b93d` 기준으로 다시 대조해 Static 선행 완료, 프레임 순서(`AdvanceSimulation`→`Scene::Update`), private world, 레이어 전용 충돌 필터를 반영했다. Xbot.glb의 Armature 배율 0.01과 왼팔 본 이름을 자산에서 직접 확인했고, Xbot 데모(BC-00, BC-12b)와 uniform scale 분리 계약을 추가했다. UE5.3 `BreakConstraint`·`UpdateMeshForBrokenConstraints` 본문을 다시 읽어 정점·weight를 바꾸지 않음을 확인했다. 문서만 변경했으므로 빌드·물리 실행은 하지 않았다. 범용 본 이름 로더, 비균일 scale, GPU 피부 절단은 별도 후속이다.


### BC-02 재검증 중단 (2026-10-28)

현재 사용자 지시는 단계별 중단 없이 M1까지 진행하되 샌드박스 검증 차단 시 중단하는 것이다. CPU SkeletonPoseTests 32/32 PASS, 컴파일·실행 6.60초, 종료 코드 0. 구현과 분리하여 diff와 기존 EvaluateModelPose를 대조한 자기 리뷰에서 유효 지적 0건이며 git diff --check도 통과했다. 전체 Debug 빌드는 Jolt CMake 재구성 중 메인 체크아웃의 MiniEngine/Build/ThirdParty/Jolt/build에 쓰려다 Permission denied와 MSB3073으로 실패했다. Tools/BuildJolt.ps1은 cacheRoot가 고정되어 별도 경로 인자가 없고 Physics.vcxproj/Physics.props도 같은 경로를 사용한다. 메인 파일·junction을 변경하지 않았으며 권한 우회나 빌드 생략으로 성공 처리하지 않았다. Release와 viewer 실행은 미수행이다. 사용자 멈춤 조건에 따라 BC-02 완료 커밋, master 병합 및 BC-03a 이후를 보류했다. 기존 .mini 부산물 3개는 보존하며 커밋하지 않는다.

### BC-02 재개 검증 (2026-10-29)

사용자 지시에 따라 단계별 승인 중단 없이 M1까지 진행한다. ThirdParty 실제 복사본의 이전 build 폴더는 worktree 내부 build.main-copy-20261029로 보존했다. Tools/BuildJolt.ps1로 로컬 캐시를 재구성하고 Debug Jolt 빌드에 성공했다. CPU SkeletonPoseTests 32/32 PASS(3.59초), 전체 Debug/Release MSBuild /v:minimal 빌드 모두 종료 코드 0이다. pwsh 탐색 메시지는 있었으나 빌드 최종 종료는 성공이다. 연결 단절 전 빌드는 완료 근거로 사용하지 않았다. 별도 codex exec 읽기 전용 리뷰는 home directory 탐색 실패로 실행되지 않아, diff와 기존 EvaluateModelPose를 다시 대조한 자기 리뷰를 수행했고 유효 지적은 0건이다. BC-02는 화면 경로를 변경하지 않아 이번 단계 viewer 실행은 하지 않았다. .mini 부산물은 커밋에서 제외한다.
커밋 단계에서 worktree 외부 Git 관리 폴더의 index.lock 생성이 Permission denied로 차단됐다. git add와 git commit 모두 실패했으며 master 병합·후속 단계는 미수행이다. 최종 보고는 BreakConstraintReport.md에 저장했다.

### Static 병합 후 전제 대조 (2026-10-29, 3bee0b20)

사용자 변경 지시: 이 세션은 git commit/merge를 실행하지 않고 M1까지 단계별 구현·검증·별도 리뷰·문서 갱신을 계속한다. 단계별 바뀐 파일 목록을 Progress에 남긴다. 각 단계 구현과 리뷰는 별도 Codex 실행으로 수행한다.

PhysicsWorld의 handle·PreparedBody·COM 속도·private PImpl·Static/Dynamic만 있는 motion은 기존 계약과 일치한다. Kinematic/impulse/constraint 중복 API는 없다. PhysicsSystem::AdvanceSimulation은 PhysicsStepClock의 최대 4회 step과 ApplyBodyPoses를 소유하며 BC-11에서 이 기존 루프에 skeletal 단계를 삽입한다. 새 step 시계를 만들지 않는다.

편집·삭제의 실제 연결 지점은 PhysicsSystem::ApplyMeshEdit와 RemoveTarget, 공통 DeleteEditTarget이다. GameObject::IsEditable은 명시적 m_NotEditable만 검사하며 기존 Static sim 조각 owner도 편집할 수 있다. 따라서 skeletal 등록 시 owner를 명시적으로 잠그고 해제 시 이전 잠금 값을 복원해야 한다. Scene::DeleteObject/Clear 직접 호출은 PhysicsSystem을 통하지 않으므로 SkeletalPhysicsSystem은 SyncBodies 이전 정리에서 사라진 ID도 검사한다. Clear에서는 skeletal 관절→body를 world Clear보다 먼저 정리한다. 이 차이는 기존 Static 편집 정책을 보존하기 위해 문서에 반영했다.

병합 충돌은 사용자가 ModelViewer.cpp의 양쪽 include·IsDone 조건·멤버를 보존하여 해결했고 Debug 빌드 성공을 알렸다. 본 세션은 병합을 다시 실행하지 않는다.
### BC-03a 완료 (2026-10-29)

ModelInstance::EvaluateAnimation과 FinalizePose를 분리하고 Update는 override 없는 wrapper로 유지했다. 인스턴스별 유효 부모를 생성 시 저장하며 애니메이션 단계는 node pose를 공개하되 HasValidPose는 false다. FinalizePose는 모든 팔레트 참조를 검사하고 override 평가 성공 후 각 palette의 IBM·normal을 계산한다.

SkeletonPose 32/32, SceneMath 495 checks와 Physics Debug 9/9 회귀 PASS. 최종 Debug 빌드 10.94초, Release 14.75초, 각 종료 코드 0. 실제 Debug Viewer -debug 1 -xbot-arm-checks 1은 6.32초/종료 코드 0이며 BC-03a PASS와 BC-00 PASS를 새 로그에서 확인했다. Xbot 실제 팔레트는 67개·중복0개여서 별도 CPU Model의 같은 NodeId 두 entry와 서로 다른 IBM으로 중복 갱신을 확인했다(syntheticDuplicateEntries=2). 4개 dt의 wrapper/split node·position·normal 동등, override, 실패 출력 보존을 검사했다. 별도 새 Codex 읽기 전용 리뷰 지적 0건. GPU 기반 검증 옵션은 켜지 않았고 DirectX debug layer를 켰다. 드라이버 레지스트리에는 Intel Iris Xe 27.20.100.9415가 있으며 실제 선택 adapter는 별도 계측하지 않았다. 기존 미수정 헤더 C4819와 pwsh fallback 메시지가 있으나 빌드 성공이다.
### BC-03b 완료 및 Static 기본값 보완 (2026-10-29)

리뷰 수정: 같은 초기 상태의 ModelInstance::Update 기준과 Scene 분리 경로를 4개 dt에서 전체 node·position·normal 팔레트로 비교하고 PrepareMeshes 전후 snapshot 일치를 검사한다. 임시 Scene 해제 전 GPU 명령 완료를 기다리며, FinalizePoses 주석을 문장별 두 줄로 나누고 Scene 소유권 본문을 현재 단계 순서로 맞췄다.

Scene::EvaluateAnimations→FinalizePoses→PrepareMeshes로 분리했다. FinalizeObjectPose는 ID로 지정한 한 owner만 확정한다. 전체 FinalizePoses는 개별 물리 override보다 먼저 호출해야 한다. 무효 pose도 MeshComponent::Update에 전달하여 기존 readiness 해제를 보존한다. 기존 Update wrapper는 세 단계를 이어 부른다.

병합된 master의 ModelViewer::m_UsePhysicsSim 기본값은 1이다. 기존 계약의 비물리 회귀 명령은 반드시 -physics-sim 0을 명시한다. default 값을 변경하지 않는다. 처음 -component-checks 실행은 이를 빠뜨려 준비 이후 추가된 바닥의 메시 수가 기존 Two components per owner 조건과 달라 실패했다. 영속 MeshComponentChecks.log를 추가해 원인을 확인했다. 물리를 명시적으로 끈 재실행은 Debug 4.67초와 Release 4.66초/종료 코드0, PASS GPU preparation이다. Scene 분리 단계·owner override·무효 pose readiness·삭제 ID 검사를 포함한다.

Debug 전체 빌드 PASS(첫136.80초, 로그 추가 후 최종 빌드도 종료0), Release 재실행 PASS(177.00초). 최초 Release 로그 경합은 명령이 실행되지 않았으므로 성공으로 계산하지 않았다. CPU SkeletonPose32/32·SceneMath495 checks 재실행 PASS. 별도 새 Codex 리뷰는 Scene·추가 진단 모두 지적0건. 진단의 반복 GPU 준비와 상세 flush는 옵션 실행에만 적용되며 프레임 성능 측정에는 사용하지 않는다. UI 도구로 찾을 오류 창은 없었으며 Windows ACL이나 앱 설정은 변경하지 않았다.
