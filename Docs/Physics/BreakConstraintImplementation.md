상태: 진행 중 — BC-12a-2 b4afd069 완료; BC-12b 렌더 팔레트 수치 보완 완료(미커밋), Debug/Release 각2회 PASS, 전용 근접 카메라 적용·화면 확인/캡처는 Claude 후속; BC-13a 미착수 (2026-10-30)

# BreakConstraint 구현 인계

## 1. 시작점과 완료 범위

2026-10-29 사용자가 Claude 호출 없이 Codex만으로 M1(BC-13a) 끝까지 계속 진행하도록 명시했다. 약 300줄의 한 개념 변경 → 검증 → 독립 리뷰 → 문서 기록을 단계별로 유지하되 승인 대기 중단 없이 다음 새 Codex 실행으로 이어간다. 커밋·병합은 하지 않는다. Static 공통 기반(body prepare/commit, 고정 step, CutTransaction)은 `5f56b93d`까지 커밋됐다. 코드 작성 세션이 겹치면 시작하지 말고 사용자에게 인계 상태를 알린다.

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
| 충돌 | floor/타 owner와 접촉. 같은 rig의 직접 인접 body pair는 break 후에도 제외, 비인접 pair는 허용. BC-06의 명시적 pair 필터와 기존 `PhysicsLayers`를 함께 사용한다 |
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
- `PhysicsWorld`는 BC-05에서 기존 Static/Dynamic에 Kinematic·MoveKinematic·운동 전환·impulse를 추가했다. BC-06에서 SwingTwist constraint와 인접 pair 필터를 추가했다.
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
| `MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.h/.cpp` (신규) | owner↔rig↔본↔body 매핑, generation, break 준비/commit, pose writeback. BC-07 등록/해제는 `PhysicsWorld&`와 `ObjectId`만 의존하여 CPU 시험 `Scene/tests/SkeletalBreakTests.cpp`(Physics CMake target)로 검증 |
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

BC-06 API: `SwingTwistDesc`는 Parent/Child runtime handle과 중립 `JointDefinition`을 받는다(Joint 안의 자산 body ID는 사용하지 않는다). anchor는 body 원점 기준이며 생성 시 shape COM을 빼고 joint 회전의 +X/+Y를 twist/plane 축으로 전달한다. 유한 frame·단위 회전·각도 범위·weld·등록 상태를 검사하며 두 endpoint 모두 비정적이어야 한다. `ConstraintHandle`은 단조 증가하고 Clear/Shutdown 후에도 재사용하지 않는다. `GetConstraintState`는 Enabled/MotorsOff를 반환하고 실패 시 출력을 보존한다. 생성 시 Dynamic이 하나라도 있으면 enable하고 두 Kinematic이면 disable한다. 마지막 Dynamic을 Kinematic으로 바꾸면 자동 disable한다. Dynamic 전환 후 재활성화는 등록/break 호출자가 `SetConstraintEnabled`로 명시한다. 생성·enable·제거는 잠든 endpoint를 깨운다.

`ExcludeBodyPair`는 같은 rig인지 판단하는 API가 아니라 등록자가 직접 인접 pair에만 호출하는 공통 기반이다(BC-07에서 적용). Jolt CollisionGroup의 world별 필터가 BodyID index+sequence pair만 제외하므로 비인접 body·다른 owner·Static 바닥은 기존 계층 규칙대로 접촉한다. Step 밖에서만 pair를 바꾸며 기존 접촉 캐시를 무효화한다. 관절 제거로 pair를 되살리지 않는다. `RemoveBodyFromWorld`는 연결 관절을 먼저 제거하되 body와 pair 제외를 보존하고, `DestroyBody`는 연결 관절과 해당 pair를 정리한다. Clear/Shutdown도 관절→body 순서로 정리한다. API/수명 연결과 CPU 검증을 합치면 300줄을 넘으므로 §11의 BC-06a/06b로 나누며 이번 요청의 BC-06 범위 안에서 둘 다 검증한다.
RemoveConstraint는 world 참조를 먼저 제거하고 map/RAII의 ref를 정리한다. owner 삭제는 모든 constraint 제거 → body 제거/파괴 → rig entry 제거 순서다. 등록 실패는 등록된 일부 자원도 역순 정리하고 owner pose/geometry를 변경하지 않는다. query fallback body와 본 body의 중복 후보는 handle→owner/node 매핑으로 구별한다.

BC-07 API: `PhysicsSystem`이 `m_World` 뒤에 `SkeletalPhysicsSystem m_Skeletal`을 두고 `GetSkeletalPhysics()`로 등록자만 공개한다(world는 비공개 유지). `Register(owner, asset, nodeParents, bodyWorldFrames)`는 owner·중복·`ValidateSkeletalPhysicsAsset`·frame 수를 검사한 뒤 body 원점 중심 상자를 모두 Prepare하고(실패는 PreparedBody RAII가 예약 반환), commit → 관절 생성 → 인접 pair 제외 순으로 진행한다. 질량은 공통 Unreal 질량식의 `MassScale = MassKg / CalculateUnrealMass(volume, 1)`로 맞추며 최소 질량 하한에 걸리는 상자는 거절한다. 중간 실패는 관절 역순 `RemoveConstraint` → body 역순 `DestroyBody`(pair 제외도 정리)로 되돌린다. `bodyWorldFrames`는 호출자가 계산한 body 원점 월드 frame이며 `E*Gnode`에서 계산하는 경로는 BC-08a다. RigGeneration은 성공한 등록마다 새 번호이며 Clear로 되돌리지 않는다. `Unregister`·`Clear`·소멸자는 같은 역순 정리를 쓰고, `PhysicsSystem::RemoveTarget(owner, InvalidComponentId)`(owner 삭제)와 `PhysicsSystem::Clear`(world Shutdown 전)에서 호출한다. 시험용 `DiagFailRegistrationStep`(0=꺼짐)으로 n번째 commit/관절/pair 단계 직전 실패를 주입한다.

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

BC-10 구현(2026-10-30, 24da5640 기준): SkeletalPhysicsSystem::BreakConstraint는 owner/joint → AlreadyBroken → rig/topology 세대 → 유한 입력 순으로 검사한다. broken 경계를 제외한 tree를 순회하고 저장 Kinematic 목표·COM 속도를 PhysicsWorld::ApplyConstraintBreak에 전달한다. world는 모든 body·관절·외부 pin·anchor≤1e-4m·Jolt cone/twist 제한·impulse 결과 범위를 변경 없이 검사한다. 이미 Dynamic이면 실제 pose/속도를 사용한다. 성공 때만 선택 관절 제거·Kinematic 목표 동기화와 Dynamic 전환·저장 속도 복원·내부 관절 enable/모터 Off·wake·ChildBody impulse 1회를 수행한다. 이후 rig joint handle을 invalid로 표시하고 topology를 +1한다. Physics 제어권은 기존 CollectPoseOverrides의 Dynamic 판정으로 표현하며 중복 상태는 추가하지 않는다. scene 스레드/Step 밖 호출 전제는 유지한다.

약 400줄을 world API/등록(BC-10a 약205줄)과 rig 적용/검사(BC-10b 약195줄)로 나눴다. 동기 M1 계약의 의미는 변경하지 않았다. 기존 SkeletalBreakTests target과 BuildJolt.ps1 testTargets를 재사용한다. PhysicsWorldBreak.cpp를 CMake·Physics.vcxproj/.filters에 등록했다. 기존 헤더의 BC-10 선언은 선행 커밋에 있다.

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

BC-11 구현(2026-10-30, HEAD `e7a608dc`, 미커밋): `PhysicsSystem::AdvanceSimulation`이 기존 `PhysicsStepClock`(1/60초, 최대4회 catch-up) 안에서 EvaluateAnimations → section E/목표 검증·UpdateKinematicTargets → QueueBreak 요청을 BreakConstraint로 동기 처리 → StepSimulation 1회 → Static ApplyBodyPoses → Dynamic CollectPoseOverrides/FinalizeObjectPose 순서로 진행한다. owner마다 substep당 한 번만 확정한다. ModelViewer와 통합 검사는 같은 PhysicsSystem::UpdateFrame을 호출한다. 이 함수가 AdvanceSimulation 뒤 physics on에서 PrepareMeshes를 렌더 프레임당 한 번 호출하고, off에서는 기존 Scene::Update와 animation-step-ms 동작을 유지한다. 물리 on에서는 공통 fixed dt만 쓴다.

zero-step/pause는 이전 pose·팔레트·대기 요청을 보존한다. 신규 모델은 Scene::InitializePoses가 Update(0)으로 준비한다. 삭제 owner rig는 step 전에 해제하고 Clear는 대기 요청도 버린다. QueueBreak는 ID·세대·값만 저장하며 결과를 owner/status로 로그에 기록한다. world는 private이고 PhysicsSystem만 friend로 rig 메타데이터를 읽는다. E는 실제 Wc/SourceFrame/Nmesh로 구하며 section 사이 상대행렬 오차1e-4, uniform/shear는 기존 helper 기준이다. Nmesh가 rig body node 또는 자손이면 거절한다. 스킨 section 없는 모델 fixture는 owner world를 E로 쓴다. 무효 target/writeback frame은 예외로 중단하며 조용히 근사하거나 animation으로 대체하지 않는다.

검증: SkeletalBreak482·PhysicsBody362·PhysicsConstraint286·PhysicsStep427·SkeletalFrame706·SceneMath495·SkeletonPose32건을 Debug/Release 각각 모두 PASS(구성당2790건, 합5580건)했다. 직접 cl·산출물은 시험 머리말과 `$env:TEMP/BreakConstraintTests/BC11-root`의 verify.ps1/pose-tests.ps1을 따른다. 전체 msbuild는 /restore 없이 최종 증분23.59/12.79초·종료0·오류0·기존 C4819경고6/0이다. 최초 빌드67.70/46.93초도 성공했다. 신규 C4244 한 건은 NodeId 명시변환으로 없앴다.

Debug 물리OFF `-component-checks 1 -physics-query-exit 6`은11.56초·종료0·PASS GPU preparation, 초기 BC-11 추가39검사 PASS였으며 독립 리뷰 수정 후 공통 프레임/선형 animation 검사는44건 PASS다. 별도 Scene에서 최초/반복 zero-step, step0/1/4/0, world 총횟수, wrapper와 전체 node·position/normal palette 동등, substep GPU0·프레임 GPU1, 실제 정규화 Xbot rig의 queued break·Dynamic2개 writeback·zero-step 보존·자원0을 검사한다. 물리OFF `-xbot-arm-checks 1`도18.37초·종료0·BC-00/03a PASS다. 물리OFF 기본 실행9.15초와 기존 `-physics-sim 1 -physics-sim-checks 1` 실행10.14초도 종료0이다(후자는 PASS 문자열 별도 캡처 없음). 모두 -debug 1과 종료옵션을 사용했다. .mini3개·Report 해시는 전후 동일했다. 독립 리뷰 수정 재검증은 Progress의 BC-11 기록에 있다. 공통 UpdateFrame에서 GPU 준비를 측정하고 선형 animation의 예상 목표·상속 속도·step 후 pose를 독립 계산한다. break 선소비·target 생략 변형은 새 검사에서 실패하며 복구 후 Debug 재빌드·component 703 PASS 행(BC-11 44건 포함)·D3D12 ERROR/CORRUPTION0을 확인했다. GPU 기반 검증·driver 재조회·CTest·Release viewer·BC-12 시각 데모는 미수행이다.

별도 diff 자기 리뷰에서 진단 복제 owner의 정규화 scale 누락1건을 고치고 최종 빌드·진단으로 재검증했다. 초기 zero-step 평가 누락은 구현 중 수정했다. 독립 리뷰는 사용자 지시대로 Claude 담당이다. 새 파일/target은 없으며 코드·검사225줄 변경으로 §11 분할은 필요하지 않다. 의미상 계약 차이는 없다. 커밋·git add/stash·메인 체크아웃 수동 수정·junction 변경·BC-12a 이후 구현은 하지 않았다.

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
| BC-05 | 공통 body Kinematic·Dynamic·impulse API/시험 | 완료 `84ecea1f`: CPU 362/362 두 구성, Debug/Release 빌드·비물리 컴포넌트 PASS / 04b | B=40.82+136.18초, CPU=7.63+10.26초 |
| BC-06a | constraint 수명/enable/motor API + 명시적 인접 pair 필터·body 정리 연결 (BC-06 API 부분) | 완료 `f6a14195`: 3 body·2 joints 생성/삭제, Kinematic disable / 05 | API 약 280줄 |
| BC-06b | PhysicsConstraintTests·기존 body 회귀·빌드·자기 리뷰 (BC-06 검증 부분) | 완료 `f6a14195`: CPU 관절200/200·body362/362 두 구성, Debug/Release 빌드·비물리 진단 PASS / 06a | B=129.26+138.98초 |
| BC-07 | SkeletalPhysicsSystem 등록/해제(PhysicsSystem 멤버) | 완료 `8dc58b08`: CPU 64/64 두 구성, 주입 실패 7단계·용량 실패 역정리·해제/Clear/소멸 뒤 body·관절·pair·예약 0, Debug/Release 빌드·비물리 진단 PASS / 04~06 | B=52.13+43.36초, CPU=17.76+19.18초 |
| BC-08a-1 | 순수 rig frame·uniform scale 변환 helper·CPU 시험 | 완료(미커밋): CPU 두 구성 706/706, Debug/Release 빌드 PASS / 07 | R+B+CPU |
| BC-08a-2 | 등록 scale 저장·frame 검증 적용·Kinematic target/COM 속도 | 완료(미커밋): CPU 두 구성 103/103·전체 Debug/Release PASS, COM offset 원점 도달·Dynamic 보존·등록 s 변경/누적드리프트 거절 / 08a-1 | R+B+T |
| BC-08b | physics pose→node override 수집·기존 palette 경로 연결 준비 | CPU 두 구성227/227 PASS, parent 이동/회전 상쇄·tip local 상속·등록 s 복원·실패 출력 보존, 전체 Debug/Release 빌드 PASS / 03b,08a-2 | R+B+T |
| BC-10a | 공통 world 동기 break 선검증·commit API/등록 | 완료(미커밋): anchor·limit·외부 pin·impulse 선검증, commit 뒤 fallible cook/할당 없음 / 08b | 약205줄 |
| BC-10b | rig subtree·세대 적용 + CPU 회귀(BC-10 완료) | 완료(미커밋): 리뷰 보강 후 두 구성 CPU482/482·anchor 일치 twist/swing 초과 거절·제한 안 성공·body3·관절2→1·손목120step·중복무변경, 기존 전체 빌드·비물리 진단 PASS / 10a | CPU15.00+27.80초, 기존 B5.19+6.01초 |
| BC-11 | 공통 fixed step 연결(9절 순서로 프레임 재배치) | 완료(미커밋): CPU 7종 두 구성2790/2790·전체 빌드·Debug 물리 OFF PASS, step 0/1/4/0·GPU 준비1회·실제 Xbot/선형 animation break/writeback 44검사 PASS / 10 | 리뷰 후 B54.24+68.79초, component15.07초 |
| BC-12a-1 | 실제 body/관절 진단 조회 API·독립 off-center impulse CPU 시험 | 완료: 실제 pose/COM/속도·broken/enabled·월드 anchor, 실패 출력 보존 / 11 | R+B+CPU |
| BC-12a-2 | fixture 진단 옵션·GizmoDraw overlay·자동 수치 판정 | 완료(미커밋): 60/180 ticks·Debug/Release 각2회 PASS(접촉 전≤0.01m·이후≤0.02m·마지막≤0.01m), 정상 종료·D3D12 ERROR 0; 화면 캡처 확인(Claude), 사람 확인 대기 없음 / 12a-1 | R+B+V |
| BC-12b | Xbot 왼팔 rig 등록·팔꿈치 break 데모 | 수치 완료(미커밋): Debug/Release 각2회 PASS, 독립 animation 상완 오차0·전완 하강·손목 구간별 허용값·관절 수·finite 및 최종 node/렌더 팔레트-실제 body 오차≤1e-3m·독립 animation 거리≥0.1m 확인, 전용 근접 카메라 적용·화면 확인/캡처는 Claude 후속 / 12a | CPU92.42초, 최종 B11.18+25.53초, Xbot V49.39/35.04/25.85/35.40초 |
| BC-13a | 기본 수명 회귀(M1) | 등록 실패·씬 clear·삭제 직후 지연 명령·double break·리소스0 / 12b | R+B+T+V |
| BC-09 | (M2) PrepareBreak 일회용 plan·snapshot key 분리 | prepare 무변경·Stale / 13a | R+B+T |
| BC-14 | (M2) GPU transaction 연결 계약 구현 | 미지원 skin 유지, 준비 실패 시 break 안 함 / 09 | R+B+T+V |

M1은 BC-00~BC-13a다. BC-09·BC-14는 M2이며 이번 데모에 필요 없다.

## 12. 검증 명령과 판정

BC-07 검증(2026-10-29, 기준 `f6a14195`, 미커밋): `SkeletalPhysicsSystem`(Scene 폴더, `PhysicsWorld&`·`ObjectId`만 의존)을 추가하고 `PhysicsSystem` 멤버로 연결했다. 새 시험 `MiniEngine/ModelViewer/Scene/tests/SkeletalBreakTests.cpp` 머리말 직접 cl 명령으로 Debug 64/64 PASS(17.76초), Release `/O2 /DNDEBUG` 64/64 PASS(19.18초), 모두 종료0·컴파일 경고0이다. 무효 owner·frame 수·ancestry·Prepare 단계 무효 회전 거절, fixture 3 body·disabled 관절 2·pair 2와 Kinematic frame, 중복 owner 거절, Xbot rig 공존·Dynamic 전환 뒤 자산 질량(상대 1e-4), 한 owner 해제 시 다른 rig 보존, commit/관절/pair 7단계 주입 실패와 용량 부족 실패의 역순 정리, 재등록 새 세대, Unregister·Clear·소멸자 뒤 body·관절·pair·예약 0을 검사한다. Physics CMake에 `SkeletalBreakTests` target, BuildJolt.ps1 testTargets, ModelViewer vcxproj/.filters에 등록했다. CMake/CTest 경로 실행은 `MiniEngine/Build/ThirdParty` 쓰기가 필요하여 수행하지 않았다.

전체 ModelViewer Debug/Release를 `/restore` 없이 빌드하여 52.13/43.36초, 종료0·오류0이다. 기존 C4819 경고 19개(새 cpp가 BOM 없는 기존 `SkeletalPhysicsAsset.h`를 포함하여 1개 증가, 새 파일 자체 경고 없음)와 vswhere 메시지가 남았다. MiniEngine/ModelViewer에서 두 구성 `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`을 Start-Process -Wait로 실행하여 3.31/4.10초, 종료0, 갱신된 MeshComponentChecks.log의 `PASS GPU preparation`을 확인했다. 새 등록 API의 viewer 호출은 아직 없으며(BC-12) owner 삭제·Clear 연결만 런타임 경로에 들어갔다. 산출물·로그는 `$env:TEMP/BreakConstraintTests`의 `SkeletalBreakTestsDebug.exe`, `SkeletalBreakTestsRelease.exe`, `BC07-*-build.log`다. 변경 C++ 약 330줄(구현 193·시험 124·연결 11)로 약 300줄 기준을 조금 넘었으나 등록/해제 한 개념이라 행을 나누지 않았다.

BC-06 검증(2026-10-29, 기준 `84ecea1f`, 미커밋): API/수명 연결(06a) 약 280줄과 CPU 검증/등록(06b) 약 200줄로 분리했다. `PhysicsConstraintTests.cpp` 머리말 직접 cl 명령을 사용하여 `$env:TEMP/BreakConstraintTests`에만 CPU 산출물을 두었다. 최종 Debug/Release 관절 200/200 PASS(16.00/29.99초), 기존 PhysicsBodyTests 362/362 PASS(17.68/88.90초), 모두 종료0·컴파일 경고0이다. 3 Kinematic body·2 disabled joints, COM offset·회전축·각도 한계, 거절/출력 보존, 모터 두 종류 Off, 120 step 손목 anchor 오차 <0.01m, break 후 인접 pair 무접촉, 비인접/타 owner/바닥 접촉, 기존 접촉 캐시 갱신, 직접 제거/파괴·Clear·살아 있는 Shutdown·stale handle을 검사했다. CMake·BuildJolt.ps1 testTargets·vcxproj/.filters에 새 소스와 시험을 등록했다. CTest 전체 실행은 하지 않았다.

자기 리뷰에서 잠든 endpoint 활성화와 실제 solver/접촉/정리 시험 부족 2건을 보완한 뒤 위 CPU 검증을 다시 통과했다. 최종 diff를 별도로 다시 읽은 자기 리뷰의 잔여 지적은 0건이다. 독립 리뷰는 사용자 지시대로 상위 관리자 담당이다. 최초 관절 시험 54/54, 보강 중간 191/191도 통과했으며 최종 완료 증거는 200/200이다. 전체 Debug/Release 최초 빌드는 79.68/79.23초·종료0이다. 코드 보완 후 최종 전체 Debug/Release 빌드도 129.26/138.98초·종료0·오류0·기존 C4819 경고18개로 통과했다. 두 구성의 비물리 component-checks는 종료 대기와 새 로그 갱신을 확인하여 34.50/16.77초·종료0·`PASS GPU preparation`을 확인했다. 첫 Debug GUI 직접 호출은 종료를 기다리지 않아 판정에서 제외하고 Start-Process -WindowStyle Hidden -Wait -PassThru로 다시 실행했다. 실행 인자는 `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`이다. DirectX debug layer 사용, GPU 기반 검증과 adapter/driver 재측정은 하지 않았다.


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

BC-12a 분할(2026-10-30): 실제 world 상태 조회·시험, 자동 fixture 모델/옵션 연결, 화면 투영/overlay 및 자동 종료를 합치면 약 400줄 이상으로 예상하여 §11을 12a-1(진단 조회·독립 impulse 시험)과 12a-2(뷰어 적용)로 나눴다. 12a-1 실행은 사용자 분할 규칙에 따라 조회 API와 CPU 시험만 수행했다(dd219b8e). `SkeletalPhysicsSystem::ReadDiagnosticState`는 step 밖의 scene 스레드에서 실제 body 원점 frame·COM·COM 선속도/월드 각속도·운동 방식·관절 broken/enabled/motorsOff·양쪽 월드 anchor를 임시 묶음으로 읽고 전부 성공했을 때만 출력한다. 미등록 owner·stale body/관절은 출력 전체를 보존한다. world와 Jolt 타입은 공개하지 않는다. CPU는 실제 pose와 목표의 차이, 회전/이동된 anchor, 120 step 손목 오차≤0.01m와 finite 상태·낙하를 검사한다. 별도 단일 body 시험은 회전·COM offset 상자에 접촉/관절/step 없이 impulse를 가해 J/m과 r cross J의 응답, COM impulse의 추가 회전 0을 비교한다. 기존 CMake/BuildJolt testTargets에 등록된 두 시험 파일을 확장하므로 새 등록은 없다. 뷰어 옵션·overlay·60/120 tick 자동 종료 계약은 12a-2에 그대로 남긴다.

BC-12a 옵션은 `-skeletal-break-checks 1`로 고정하고 자동 fixture를 사용한다. 기존 모델은 변경하지 않는다. 새 GameObject 하나에 pose fixture를 등록하고 기존 화면의 bone/body 중심·anchor를 view-projection 후 GizmoDraw로 그린다. 60 fixed ticks 후 joint0을 0 impulse로 분리하고 120 ticks 뒤 수치 판정/종료 로그를 출력한다. root Y는 초기값 유지, child COM Y 하강, 살아 있는 손목 anchor 오차는 접촉 전 자유낙하 최대≤0.01m·접촉 이후 최대≤0.02m·마지막 tick≤0.01m, 모든 pose/속도 finite를 검사한다. 별도 off-center impulse CPU 시험은 접촉/관절 없는 단일 body로 수행한다.

BC-12a-2 구현·검증(2026-10-30, dd219b8e 기준, 미커밋): `SkeletalPhysicsChecks`는 기존 모델 정규화·물리 초기화 뒤 메시 없는 4-node GameObject를 하나 추가한다. 별도 world나 step 없이 `PhysicsSystem::UpdateFrame`에 정확히 1/60초를 공급하여 렌더 프레임당 한 tick을 진행한다. 따라서 catch-up으로 60/180 경계를 건너뛰지 않는다. 60회 완료 뒤 zero impulse 요청을 queue하고 61번째 target 갱신 뒤 분리하여 180회 완료 때 판정한다. 이 시간 입력 고정은 진단 옵션에만 적용한다. 기존 모델·geometry·weight와 물리 OFF 경로는 보존한다. 실제 COM/속도/양쪽 anchor는 `ReadDiagnosticState`, 확정 본은 ModelInstance에서 읽는다. `CollisionDebugDraw`와 같은 view 변환→근평면 clipping→projection 뒤 GizmoDraw 선을 낸다. 청록=본, 노랑=COM, 빨강/초록=부모/자식 anchor다. header-only 뷰어 검사이므로 ModelViewer.vcxproj/.filters에만 등록하며 Physics CMake/testTargets 변경은 없다. 접촉을 포함한 오차 판정 변경 근거는 아래 후속 측정 문단에 기록한다.

Debug 뷰어 `-debug 1 -skeletal-break-checks 1`에서 180 ticks·rootDeltaY=0m·childDeltaY=-2.51435m·손목 최대오차=0.00992500037m·finite=1·손목 enabled/motorsOff 유지·BC-12a PASS·종료0을 확인했다(41.81초). 실제 GPU는 NVIDIA GeForce MX450(1906MB)이며 driver는 레지스트리에서 32.0.15.9186으로 확인했다(CIM 조회는 권한 거절). debug layer 사용, GPU 기반 검증은 미사용이다. D3D12 ERROR/CORRUPTION은 0이며 기존 종료 시 Light live-resource 경고는 남는다. 초기에는 overlay 캡처가 없었으나 아래 Claude 외부 캡처로 화면 확인을 완료했다.

관련 CPU 머리말 직접 cl 명령: SkeletalBreakTests Debug/Release 각909/909 PASS(13.11/24.80초), PhysicsBodyTests 각393 PASS(11.72/15.32초), 컴파일 경고0·종료0. 전체 msbuild Debug17.76초/Release167.11초·종료0·오류0(기존 C4819 4/33개, /restore 없음). 최초 Debug 타입 오류 실패(11.02초)는 Matrix4::GetW와 float Color로 수정했다. 물리 OFF 기본 Debug41.03초·종료0, `-component-checks 1 -physics-query-exit 6` Debug30.54초·종료0·PASS GPU preparation(FAIL0), 둘 다 -debug 1 -physics-sim 0과 D3D12 ERROR/CORRUPTION0이다. 독립 codex exec는 home 탐색 오류로 시작 불가하여 별도 읽기 전용 자기 리뷰로 대체했다. 지적1건(정의 없는 .filters Scene 필터)을 Header Files로 수정했고 잔여0건이다. 로그·재현 스크립트는 `$env:TEMP/BreakConstraintTests/BC12a2-root`에 있다. CTest 전체·Release viewer는 미수행이며 BC-12b는 시작하지 않는다.
BC-12a-2 독립 리뷰(Codex) 지적 3건 수정(2026-10-30, 미커밋): Startup fixture/RenderUI overlay 앞 빈 줄과 Draw의 mark·본·COM·anchor 한글 역할 주석/단락을 정리했다. 판정·종료 시점과 기준값·물리 동작은 변경하지 않았다. 전체 Debug/Release 최종 증분 빌드는 27.77/22.21초, 각각 오류0·기존 C4819 4건이다. 관련 CPU는 SkeletalBreak909/909·PhysicsBody393건을 두 구성 모두 PASS했다. Debug fixture34.19초 PASS(손목0.00992500037m, 기준 대비 여유0.75%), Release36.51초/재실행32.76초는 모두 FAIL(손목0.0133718159m, 기준보다33.72% 큼)이며 두 구성 모두 finite/topology=1이다. 종료0을 PASS로 간주하지 않는다. 당시 Release 오차 원인은 미해결이었으며 아래 후속 측정과 승인된 구간별 판정으로 해결했다.

화면 검증: 초기 샌드박스 캡처 실패 후 Claude가 샌드박스 밖에서 성공했다(사용자 전달 확인). `$env:TEMP/BreakConstraintTests/BC12a2-screens/t002.7.png`는 분리 전 본 3개 세로 사슬·COM·anchor, `t005.4.png`는 분리 후 아래 두 body가 바닥에 떨어져 누운 화면이다. 상태: **화면 캡처 확인(Claude), 사람 확인 대기 없음**. 이번 실행에서는 캡처를 재시도하지 않았다.

Debug 물리 OFF 기본35.47초·component35.17초 종료0, component703 PASS행·FAIL0·PASS GPU preparation이다. 최종 5회 실행(Debug fixture/기본/component, Release fixture 2회) 모두 -debug 1, D3D12 ERROR/CORRUPTION0이며 기존 Light live-resource 경고는 남는다. Start-Process -WindowStyle Hidden -Wait로 수집기를 기다리고 내부 viewer 제한180초를 적용했다. 로그/재현은 TEMP/BreakConstraintTests/BC12a2-review-fix, 자세한 CPU 시간과 보호 파일 확인은 Progress에 기록했다. 이는 당시 결과이며 현재 완료 판정은 아래 후속 검증을 따른다.
BC-12a-2 후속 원인 확인·판정 정리(2026-10-30, 미커밋): 판정 변경 전 Debug/Release 각1회 측정에서 최초 충돌은 tick98(하위 COM Y≈-0.8627m, 수직속도 -5.9486→+1.822m/s), 최대 손목 오차는 Debug tick131=0.00992500037m(COM Y=-0.88278234m), Release tick139=0.0133718159m(COM Y=-0.886882186m)로 모두 접촉 이후였다. 바닥 상면은 -1.155179m다. 따라서 Claude의 승인된 판정대로 자유낙하 최대≤0.01m·접촉 이후 최대≤0.02m·마지막 tick≤0.01m로 구분한다. 충돌 충격 순간 관절 solver 오차와 Release 최적화(/fp:fast 포함)에 따른 수치 차이로 해석한다. `/fp:fast`는 Build.props의 공통 설정이므로 그 옵션 단독 효과까지 분리 입증한 것은 아니다.
접촉 시작은 이 정지·무충격 fixture에서 `GetFloorBox` 상면+0.30m(상자 반높이0.25m+여유0.05m) 아래인 하위 COM의 수직속도가 이전 tick보다 1m/s 넘게 증가한 최초 tick으로 고정한다. 이후 반발해도 접촉 이후 구간을 유지한다. world pose와 본 pose를 서로 대조한 판정이 아니며 바닥 부재·접촉 미발견도 FAIL이다. `AfterStep`에 tick별 오차/COM/속도, 최대 tick, 구간 최대·마지막 오차 로그만 추가했다. 물리 solver·형상·메시·웨이트는 변경하지 않았다.
최종 재검증: CPU SkeletalBreak909/909·PhysicsBody393건 두 구성 PASS(컴파일 경고0), 전체 Debug17.19초·Release31.38초 PASS(오류0·기존 C4819 각4건, /restore 없음). fixture Debug52.55/31.69초·Release36.21/24.72초 각2회 PASS·종료0·D3D12 ERROR/CORRUPTION0. 두 구성 접촉 전 최대2.384185791e-7m, Debug 접촉 후0.00992500037m/마지막0.009835194796m, Release 접촉 후0.0133718159m/마지막0.0001645183365m, rootDeltaY=0·finite/topology=1이다. 상세 회귀·시간·로그는 Progress의 후속 기록을 따른다.
BC-12b 옵션은 `-skeletal-break-xbot 1`로 고정한다. 기존 시작 장면의 Xbot에 왼팔 rig를 등록하고 animation을 재생한다. 60 fixed ticks 후 팔꿈치 joint를 작은 impulse로 분리하고 120 ticks 뒤 수치 판정 로그를 출력한다. 판정: 상완 본 pose가 animation 값과 일치, 전완 COM Y 하강, 손목 anchor 오차가 BC-00 허용값 이내, unbroken 2→1·enabled 0→1, 모든 pose/속도 finite. 화면에서는 팔꿈치 피부 늘어짐을 확인하고 break 전·후 스크린샷을 남긴다.

BC-12b 구현·수치 검증(2026-10-30, HEAD b4afd069, 미커밋): `SkeletalPhysicsChecks`에 Xbot 모드를 추가하여 기존 tick 제어·상태 조회·오버레이·수치 판정을 재사용한다. 기본 Xbot의 NodeId 13/14/15에 `CreateXbotLeftArmAsset`을 등록하며 별도 owner/메시/웨이트/정점을 만들거나 변경하지 않는다. 스킨 section의 실제 E가 일치함을 확인하여 등록하고 기존 scale 분리 경로를 사용한다. 고정 시작 배치의 E로 본 overlay를 월드로 옮긴다. physics는 기존 프레임 경로가 매 tick E를 검증한다. world 공개·별도 step·새 target은 없다. 두 진단 옵션을 함께 주면 Xbot 모드가 우선한다. 기본 Xbot 없는 `-model` 조합은 FAIL 후 정상 종료한다.

별도 `ModelInstance`에서 animation 0을 dt=0부터 같은 1/60초로 계산하고, 매 tick 상완 model pose의 16성분을 비교한다(절대 오차≤1e-5). 실제 world와 목표 world를 서로 비교하지 않는다. tick60 완료 뒤 요청을 넣고 tick61 target 뒤 팔꿈치를 분리한다. impulse는 월드 +X 0.05kg·m/s, 적용점은 직전 전완 COM이다. tick180에서 전완 COM Y를 tick60과 비교하여 0.01m 넘게 하강해야 한다. 초기 unbroken2/enabled0과 최종 unbroken1/enabled1, 상완 Kinematic·전완/손 Dynamic·손목 motorsOff를 검사한다. 70개 본의 모든 pose 성분과 body pose/COM/선속도/각속도의 finite를 누적 확인한다. 기본 VSync와 60/120 tick을 유지하며 캡처 시도는 하지 않았다. 화면 확인과 분리 전후 캡처는 Claude가 후속 수행한다.

계약 명확화: 위 “BC-00 허용값”은 break 사전 anchor 허용값(1e-4m)과 step 후 solver 오차를 혼동할 수 있다. 사용자 지정대로 solver 판정은 BC-12a-2와 동일하게 접촉 전 최대≤0.01m, 접촉 이후 최대≤0.02m, 마지막≤0.01m를 적용한다. 기존 바닥 상면+0.30m 이하·하위 COM 수직속도 1m/s/tick 초과 증가로 최초 접촉 구간을 고정한다. 접촉이 없으면 전 구간에 0.01m를 적용한다. 접촉 전 초과는 기준을 바꾸지 않고 BLOCKED/FAIL을 기록한다. 등록/break 사전 허용값과 solver 설정은 바꾸지 않았다.

Debug 2회 동일: 상완 animationMaxError=0, 전완 deltaY=-1.36082375m, 손목 접촉 전 최대=0.000006665660294m, 접촉 후 최대=0.006326892879m, 마지막=0.00002529360427m. Release 2회 동일: 상완 오차0, deltaY=-1.360850334m, 접촉 전=0.000006643729193m, 접촉 후=0.006328773685m, 마지막=0.00004452272333m. 모두 contact/max tick88, finite/topology=1, unbroken2→1·enabled0→1, BC-12b PASS·정상 종료0·D3D12 ERROR/CORRUPTION0이다. CPU·빌드·회귀·리뷰와 제한은 Progress BC-12b 기록을 따른다. 화면 검증 전이므로 피부 늘어짐의 시각 확인 완료로 기록하지 않는다.

BC-12b 화면 증거 보완(2026-10-30, 미커밋): tick180에서 `CheckRenderPose`가 렌더 준비의 `GetJoints()` 팔레트를 각 사용 section의 E와 함께 읽고 IBM 역변환으로 NodeId14/15 원점을 복원한다. 최종 node 배열도 함께 검사한다. 물리 기대 위치는 `ReadDiagnosticState`의 실제 body 원점에서 `BoneFromBody`를 제거하고 등록 s≈0.01773275를 복원한 frame의 원점이다. Kinematic target/override와 자기 비교하지 않는다. 별도 animation ModelInstance의 mesh node로 독립 E를 계산해 animation 위치를 얻는다. 모든 해당 팔레트에서 두 물리 오차≤1e-3m, 두 animation 거리≥0.1m, finite를 요구하고 팔레트 누락도 FAIL이다.
Debug 각2회 동일: 전완 팔레트/본 물리 오차4.92415154e-7/1.273155732e-7m, animation 거리1.66850841/1.66850853m; 손 오차1.685873912e-7/1.192092896e-7m, 거리1.226705313/1.22670567m. Release 각2회 동일: 전완 오차9.615579302e-7/1.490116119e-8m, 거리1.668364406/1.668364167m; 손 오차4.951133405e-7/1.192092896e-7m, 거리1.227050066/1.227049947m. 두 스킨 section의 모든 사용 entry PASS이며 최종 BC-12b PASS다.
`-skeletal-break-xbot 1`에서만 시작 eye=(5.5,-0.15,2.8), at=(5.5,-0.15,0)m를 쓴다. 기존 시작 Xbot X=5와 바닥 높이에 맞춘 45° 근접 시점이며 다른 카메라는 보존한다. 최종 팔레트 반영은 수치로 확인했지만 메시 피부 늘어짐·프레이밍의 시각 확인은 Claude가 후속 캡처로 수행한다. 이번 실행은 캡처를 시도하지 않았다. 빌드·회귀·시간·리뷰는 Progress의 화면 증거 보완 기록을 따른다.
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

### BC-08a 분할과 frame helper (2026-10-29, `8dc58b08` 기준)

한 단계 약 300줄을 지키기 위해 BC-08a를 순수 좌표 수학(08a-1)과 rig 적용·Kinematic 구동(08a-2)으로 나눈다. 이번 실행은 08a-1만 구현한다. `SkeletalPhysicsFrames.h`의 `TrySplitUniformFrame`은 `E*Gnode`의 양수 uniform scale을 분리하고, `TryBuildBodyFrame`은 배율을 제거한 강체 본에 월드 m 단위 `BoneFromBody`를 곱한다. `TryRestoreNodePose`는 offset을 제거하고 저장 scale을 곱해 model 좌표로 왕복하는 순수 함수다. rig나 node/palette를 직접 변경하지 않는다. 모든 실패는 출력 frame·scale·행렬을 보존한다.

축 길이 최대·최소 차이는 최대 길이의 1e-4 이하, 정규화 축 내적은 1e-5 이하, 오른손 determinant는 0.99999 이상을 요구한다. affine 마지막 행은 역행렬 반올림을 고려해 각 성분 1e-6 오차까지 허용한다. 모든 성분 finite와 양수·유한 축 길이를 검사해 반사·퇴화·전단·비균일·원근을 거절한다. 중립 frame의 회전 제곱 길이 오차는 자산 계약과 같은 1e-4이며 float로 변환할 수 없는 위치는 거절한다.

이 helper는 입력 E를 받는다. skinned section 사이 E 동일성과 Nmesh의 물리 override 영향은 Scene 연결 단계에서 검사한다. 등록 s 저장·등록 후 배율 변경 거절은 BC-08a-2에서 적용했다. COM offset과 속도는 PhysicsWorld/MoveKinematic 책임이며 이번 순수 좌표 시험의 body-origin offset과 구분한다. BC-08b 이후 런타임 연결은 미착수다.

### BC-08a-2 등록 pose·Kinematic 목표 (2026-10-29, `8dc58b08` 기준)

`SkeletalPhysicsSystem::RegisterFromPose`는 E·NodeId별 pose에서 `TryBuildBodyFrame`을 호출하고 등록한 E 배율과 body별 s를 저장한다. 기존 `Register`는 초기 body frame 입력과 s=1을 유지하며 자산·부모·목표 메타데이터를 함께 저장한다. `UpdateKinematicTargets`는 전체 frame·배율·운동 방식·handle과 `CanMoveKinematic`의 속도 범위를 선검증한 다음 Kinematic만 구동한다. Dynamic은 pose/속도를 읽을 뿐 변경하지 않는다. 허용오차가 누적되지 않도록 등록한 s는 갱신하지 않는다. `GetBodyTarget`으로 다음 body 원점 pose·COM 선속도·월드 각속도·등록 s를 읽는다.

공통 `PhysicsWorld::CanMoveKinematic`은 기존 MoveKinematic의 거절 조건을 읽기 전용으로 분리하여 여러 목표를 함께 검증한다. MoveKinematic 자체도 같은 검사를 재사용한다. `GetBodyMotion`은 world에 올라간 body의 중립 운동 방식만 반환하며 Jolt 타입은 노출하지 않는다. 이 API들은 같은 scene 스레드·Step 밖에서 호출한다.

직접 CPU 시험은 `SkeletalBreakTests.cpp` 머리말 명령에 Core include를 추가한다. 초기 scale/offset/속도0, offset COM shape 회전 시 원점 목표 도달·COM 속도 차이, 무효 마지막 목표·배율·극소 dt·제거된 마지막 handle의 전체 거절, Dynamic pose/속도 보존과 scale 허용오차 누적 방지를 검사한다. 두 구성 각각 103/103 PASS, CPU 컴파일 경고0. 본 writeback 수집은 BC-08b에서 추가하며 Scene E 동일성·Nmesh 의존 검증은 후속 연결 단계다.

검증: 직접 cl CPU Debug 706/706 PASS(0.89초), Release /O2 /DNDEBUG 706/706 PASS(1.27초), 컴파일 경고0·종료0. 30종 E 회전/배율 확장 시험에서 발견한 affine 마지막 행 exact 비교 문제를 1e-6 오차 검사로 수정했다. ModelViewer Debug/Release 전체 빌드(/restore 없음)는 최초12.61/38.95초·종료0·오류0이다. 최초 Release의 기존 C4819 19개 이후 최종 증분 빌드도4.88/3.70초·각 경고0·오류0·종료0으로 통과했다. 새 helper의 런타임 호출은 없어 viewer/화면·COM 속도 검사는 수행하지 않았다. CMake/BuildJolt 테스트 등록은 완료했고 CTest 경로 실행은 미수행이다. 별도 독립 리뷰 결과는 Progress에 기록한다.

독립 리뷰 반영: float 길이 제곱 계산이 1e30 배율을 overflow로 거절해 불필요한 범위 제한이 생겼다. 축 길이와 정규화를 double로 계산하고 float로 표현 가능한 양수 길이만 저장하도록 수정했다. 1e30·1e-30·denorm_min의 양수 uniform split 허용을 CPU 두 구성에서 확인했다. offset과 월드 body 생성의 별도 물리 한계는 기존 PhysicsWorld가 검사한다.

BC-08a-2 최종 검증: CPU Debug/Release 각각103/103 PASS(19.84/22.59초), 경고0·종료0. 전체 ModelViewer Debug68.01초·Release47.67초, /restore없이 빌드 PASS·오류0·각 C4819 20개·종료0. 기존 SkeletonPose/SkeletalPhysicsAsset 및 새 호출로 포함된 BC-08a-1 helper의 코드페이지 경고이며 CPU /utf-8 컴파일은 경고0이다. 독립 Codex 읽기 전용 리뷰에서 등록 s의 누적 허용오차를 지적했고 등록 s 불변·누적거절 시험으로 수정하여 최종 대조 잔여0건이다. CTest·viewer는 프레임 연결 전이라 미실행. 로그는 TEMP/BreakConstraintTests/BC08a2-{Debug,Release}-{CPU,build}.log다.

### BC-08b 물리 본 override 수집 (2026-10-29, `8dc58b08` 기준)

`SkeletalPhysicsSystem::CollectPoseOverrides(owner, E, output)`는 등록 당시 E 배율과 대조한 뒤 모든 body handle·운동 방식을 검사한다. Dynamic body만 현재 원점 world pose를 읽어 등록 s와 `BoneFromBody`를 복원한 `NodePhysicsPose`로 변환한다. Kinematic 본은 override하지 않으며 전부 Kinematic이면 빈 배열로 성공한다. owner 이동·회전은 허용하고 변경된 scale·무효 frame·stale body는 출력 배열을 보존한 채 거절한다.

출력은 기존 `ModelInstance::FinalizePose`/`Scene::FinalizeObjectPose`에 전달할 수 있다. 부모 우선 `EvaluatePoseOverrides`와 전체 palette 갱신 구현은 변경하지 않는다. 이 단계 CPU는 실제 Jolt Dynamic body 두 개와 4-node graph를 이용해 owner 이동·회전 상쇄, root local 애니메이션 유지, body 없는 tip의 local 애니메이션 상속, Xbot 배율 복원, 마지막 stale body·무효 E의 출력 보존을 검사한다. 중복 palette 실제 ModelInstance 회귀는 BC-03a의 기존 검사와 후속 viewer 진단을 사용한다. fixed-step Scene 호출 연결과 section E/Nmesh 검증은 BC-11이다.

BC-08b 검증: CPU Debug/Release 각각227/227 PASS(29.38/28.24초), 컴파일 경고0·종료0. 전체 ModelViewer Debug67.80초·Release70.70초, /restore없이 두 구성 PASS·오류0·각 C4819 21개·종료0. 기존 헤더의 코드페이지 경고이며 CPU /utf-8 컴파일은 경고0이다. 독립 Codex 읽기 전용 리뷰 지적0건, git diff --check PASS. CTest·viewer는 fixed-step 연결 전이라 미실행. 로그는 TEMP/BreakConstraintTests/BC08b-{Debug,Release}-{CPU,build}.log다.
