상태: 진행 중 — SP-7a 진단 구현·검증(미커밋, base `2e8dcdff`), D2 정점 bounds 미충족; SP-7b~e 미구현 (2026-11-03)

# Skeletal 패치 절단 + BreakConstraint (SP-7)

[SkeletalPatchCopyCut.md](SkeletalPatchCopyCut.md)의 SP-6까지 C 입력은 원본 옆 정적 복사본만 자른다. SP-7은 같은 C 입력으로 원본을 실제로 끊는다. 순서는 NDC25 해석([NDC25Reference.md](../NDC/NDC25Reference.md) 5절)의 1~5를 따른다.

## 1. 목표

C를 누르면 평면이 지나는 skin GameObject에 대해 다음을 처리한다.

1. Parent joint P를 고르고 패치를 추출한다(SP-6 그대로).
2. 패치를 평면으로 잘라 조각 A/B를 만든다.
3. P의 incoming constraint를 끊는다.
4. 원본에서 패치 범위를 숨긴다.
5. A는 남는 쪽 node에, B는 떨어지는 쪽 node에 붙인다.

대상은 아래 D2 제약을 만족하는 모든 skeletal mesh다(사용자 결정 2026-11-03). 검증 모델은 Xbot 전신과 CesiumMan이다.

## 2. 결정과 이유

### D1. 끊을 constraint: P의 incoming constraint
- 대상은 `JointDefinition.ChildBody == body(P)`인 constraint다. 연결 상대는 P의 직접 부모가 아니라 **가장 가까운 body 조상**일 수 있다. 이것은 body 없는 중간 node를 허용하는 `ValidateSkeletalPhysicsAsset` 규칙 때문이다.
- 이유: 패치는 P에 weight가 있는 삼각형이고 P→자식 선분 전체를 덮는다. 끊기는 관절은 P와 그 위 사이이고, 절단 위치는 선분 위다. NDC `BreakConstraint(BoneName)`과 같다.
- 신규 `SkeletalPhysicsSystem::FindIncomingJoint`는 JointId, ParentBody, ParentNode 또는 거절 사유를 돌려준다. 거절 사유는 body 없음, rig root, 이미 끊김이다.
- 거절되면 SP-7 동작 전체를 생략하고 사유만 로그로 남긴다. 가까운 본으로 대체하는 fallback은 두지 않는다(BreakConstraintDesign 원칙).
- 같은 P를 다시 자르면 AlreadyBroken으로 거절된다. 연속 절단은 인접 joint로 이어진다(D6).

### D2. 자동 rig: box body, C 시점 지연 등록
- 지금 등록되는 rig는 진단 fixture와 Xbot 왼팔뿐이다. 그래서 skeleton에서 asset을 자동으로 만든다.
- 형상은 기존 `BodyDefinition::HalfExtentMeters` / `BuildBoxBody` 계약 그대로 **box**로 한다. capsule은 asset·검증·생성 변경이 필요하므로 범위 밖이다.
- 신규 `BuildSkeletalPhysicsAsset(model, pose)` 규칙
  - **body:** 모든 skin section의 palette를 NodeId로 합쳐 중복을 없앤다. 그 영향 joint마다 body 하나를 둔다. `SelectSegmentJoint`가 후보로 삼는 집합과 같다.
  - **body frame:** `TryBuildBodyFrame`과 같은 `E*Gnode` 공간에서 정한다.
    - 원점: P와 영향 있는 자식(여럿이면 가장 먼 것) 사이 선분의 중점
    - 축: 그 선분 방향
    - 크기: 그 joint에 weight가 있는 정점 bounds(body frame 기준)
    - 말단: P와 weight 정점 bounds만으로 정한다.
    - SP-7a 구현 편차: CPU 정점 미보존으로 크기는 segment 기반 근사이며 leaf 축·길이는 incoming segment를 쓴다. 아래 검증 기록의 fallback은 진단용이고 위 정점 bounds·leaf 규칙의 완료 근거가 아니다. SP-7b 등록 전에 bounds 구현 또는 사용자와의 설계 결정을 확정해야 한다.
    - 질량: 고정 밀도 × box 부피
    - 저장: 미터 단위 `BoneFromBody`로 둔다.
  - **joint:** 각 body를 가장 가까운 body 조상과 SwingTwist로 잇는다.
    - anchor: 자식 body node의 원점을 등록 pose에서 양쪽 body frame으로 표현한 값이다. 그래서 등록 순간 오차는 0이다.
    - limit: 넉넉한 고정값이다.
- 거절하는 모델: 영향 joint의 공통 조상이 둘 이상인 경우, section마다 E가 다른 경우, 비균일 scale. 이들은 `ValidateSkeletalPhysicsAsset`과 `ReadSkeletalPose` 제약과 같다. 거절된 모델은 등록하지 않고 C 동작을 SP-6 복사본까지만 한다.
- 등록 시점은 **그 owner에서 처음 C를 누른 때**다. 그때의 최종 pose로 `RegisterFromPose`를 호출한다.
  - 이유: 생성 직후 등록하면 초기 pose와 배치가 확정되기 전이다. 또 모든 skin owner가 편집 잠금에 걸린다.
  - 진단용 `StartXbot`처럼 이미 등록된 owner는 건너뛴다. `Register`가 중복 등록을 거절하기 때문이다.
- 내부 관절의 anchor 오차는 `SupportsJointPose`가 0.1 mm로 검사하므로 limit만 넓혀서는 해결되지 않는다. 7a에서 애니메이션 표본 프레임의 최대 anchor 오차를 로그로 남긴다. 실제로 break할 때 실패하면 UnsupportedPose로 처리한다(D5).

### D3. 원본 숨김: MeshComponent 소유 상태 버퍼 + 퇴화 치환 IB (D안)
- "원본"은 절단 대상 skin GameObject의 MeshComponent를 말한다.
  - `Model::m_DataBuffer`의 VB·weight·원래 IB는 공유 데이터라 수정하지 않는다. 같은 Model의 다른 인스턴스를 보호하기 위해서다.
  - 컴포넌트마다 두 가지를 둔다.
    - **상태 버퍼:** 삼각형마다 제거 여부와 소유 패치 id를 둔다.
    - **치환 IB:** 원래 IB를 복사한 것이다.
- 삼각형 번호는 `SkeletalPatchJob::Submit`이 (section, draw)마다 누적하는 `FirstTriangle` + draw 내부 번호다. GeometryRevision이 같은 동안 고정된다. 상태 버퍼는 이 번호로 색인한다.
- 숨길 때는 GPU compute로 제거된 삼각형의 세 인덱스를 그 삼각형의 첫 인덱스로 바꿔 **퇴화**시킨다. 개수는 줄이지 않는다.
  - 이유: `RenderMeshes`가 CPU의 `mesh.draw[]`로 `DrawIndexed`하기 때문이다. 개수가 같으면 draw 범위·`baseVertex`·format을 그대로 쓸 수 있다. readback, indirect draw, CPU 범위 갱신이 모두 필요 없다.
  - 면적 0인 삼각형은 래스터화되지 않는다. 그래서 color/depth/shadow/Wireframe에서 정확히 사라진다.
  - Vertices(POINTLIST) 모드에서는 퇴화된 첫 정점이 점으로 남는다. 진단 표시의 한계로 허용한다.
- `MeshSorter::AddMesh` / `RenderMeshes`는 지금 `bufferPtr` 하나로 VB와 IB 주소를 함께 받는다. 이를 나눠서, 치환 IB가 있으면 그 주소와 같은 `ibOffset` 규칙을 쓴다. 치환 IB는 컴포넌트가 소유하고 수명도 컴포넌트를 따른다. 갱신은 같은 command list에서 그리기 전에 끝낸다.
- `PatchExtractCS`의 선택 판정은 원래 IB와 정점 weight를 읽는다. 여기에 "제거되지 않음" 조건을 더한다. 그러면 prefix·출력·카운트가 모두 따라간다. CPU 기준 함수에도 같은 조건을 넣는다. NDC 슬라이드 10:12 "이미 한번 복사된 범위는 다시 복사하지 않음"에 해당한다.
- 버텍스 컬러(B안)를 버린 이유
  - 정점 단위 마스크라서 삼각형 단위 기준을 정확히 맞추기 어렵다.
  - 다섯 패스 모두에 discard를 넣어야 한다.
  - 합칠 대상을 고를 소유 정보가 없다.
- 소유는 패치 id로 둔다. 평면에 갈린 원본 삼각형은 A와 B 양쪽에 파생 삼각형을 남기기 때문이다. 조각별 출처는 패치 쪽에서 관리한다.

### D4. A/B는 원본 GameObject 아래, node에 강체로 붙임
- 부착: `Wpiece(t) = Wnode(t) · inverse(Wnode(C)) · Wpiece(C)`
  - `Wnode`는 `JointFrameToWorld`와 같은 `Wc·E·Gnode`다.
  - (C)는 추출에 쓴 snapshot이다.
  - 갱신은 `FinalizeObjectPose` 뒤, 그리기 전에 한다.
- A/B 쪽 판정
  - **Segment:** P와 자식의 부호가 반대다(`SelectSegmentJoint` 보장). P가 있는 쪽이 A, 자식 쪽이 B다.
  - **Leaf:** P와 영향 있는 가장 가까운 조상(선분 시작점)의 부호로 판정한다. 조상이 없거나 둘 다 평면에서 ε(1 mm) 이내이면 거절한다.
  - A나 B가 비면 거절한다.
- 붙는 node: A는 P의 **직접 부모 node**(애니메이션을 그대로 따름), B는 P node다. D1의 ParentNode와는 다를 수 있다.
- 조각 컴포넌트에는 node-attached 표시를 둔다.
  - `PhysicsSystem::IsEligible`에서 제외하고 hull·body를 만들지 않는다. 그래야 `SyncBodies` / `ApplyBodyPoses`와 경쟁하지 않는다.
  - picking은 owner 단위로 기존처럼 한다.
- 강체 근사의 한계: 혼합 정점이 이후에 어떻게 변형되는지는 재현하지 않는다(NDC "남는 쪽 기여 교체"는 범위 밖, 후속). 오차는 게시 프레임과 이후 프레임에서 A/B 경계 정점과 원본 경계 정점 사이 최대 거리로 측정해 보고한다. 게시 프레임 허용치는 1 mm다.
- 오프셋 복사본은 진단 옵션 `-skeletal-patch-offset-copy 1`일 때만 기존처럼 띄운다.

### D5. 게시는 break 성공 뒤 (되돌리기 없음)
- C에서 등록과 `FindIncomingJoint`를 하고 generation을 저장한다. 이어서 추출과 A/B 절단을 진행한다. 이 동안 원본은 바뀌지 않는다.
- A/B가 준비되면 `QueueBreak`를 호출한다. `QueueBreak`는 신규 RequestId를 돌려주도록 확장한다. `AdvanceSimulation`은 요청별 `BreakStatus`를 결과 표에 남긴다(지금은 로그 후 버림).
- 다음 실제 substep에서 Success가 나오면 같은 프레임 `SkeletalPatchCopy::Update`에서 다음을 게시한다. `ModelViewer::Update`는 물리 갱신 뒤 이 경로를 실행하므로 늘어난 원본이 보이는 프레임이 없다.
  - 상태 버퍼·치환 IB 갱신
  - A/B 부착
- 실패(Stale, UnsupportedPose 등)하면 A/B를 버리고 사유만 로그로 남긴다. 원본은 처음부터 바뀌지 않았으므로 되돌릴 것이 없다.
- pause 중에는 결과가 나올 때까지 대기하고 "대기" 로그를 남긴다. physics off이면 C 시점에 거절한다.
- BC-09/BC-14의 원자 commit은 쓰지 않는다. 게시 직전에 owner의 GeometryRevision과 mask revision이 C 시점과 같은지 확인하고, 다르면 버린다.

### D6. 연속 절단 범위
- SP-7에 들어가는 것은 제거 삼각형 건너뛰기(D3)뿐이다.
- 기존 패치 소유 삼각형과 맞닿으면 새 패치를 따로 만들고 `adjacentPatch` 로그를 남긴다.
- NDC "인접 조각에 합친 뒤 절단"은 SP-8 결정에서 다룬다. 소유 패치 id와 조각 연결성으로 합칠 대상을 고르고, 기존 절단 경계는 유지한다.

## 3. 신규 타입·함수 골격 (모두 신규)

```cpp
// incoming constraint 조회 결과. Status가 Ok일 때만 나머지가 유효하다.
struct IncomingJointResult { IncomingJointStatus Status; uint32_t JointId, ChildBody, ParentBody, ParentNode; };
IncomingJointResult SkeletalPhysicsSystem::FindIncomingJoint(ObjectId owner, uint32_t nodeId) const;

// 등록 pose의 skeleton·skin weight로 box body 전신 asset을 만든다. 실패 사유는 로그로 남긴다.
bool BuildSkeletalPhysicsAsset(const Model& model, const SkeletalPoseSnapshot& pose, MiniPhysics::SkeletalPhysicsAsset& outAsset);

// QueueBreak 확장: 요청 번호를 돌려주고, 결과는 번호로 조회한다.
uint64_t PhysicsSystem::QueueBreak(const BreakRequest& request);
bool PhysicsSystem::TryGetBreakResult(uint64_t requestId, BreakStatus& outStatus);

// MeshComponent 소유 숨김 상태. State는 삼각형당 uint(bit31 제거, 하위 16bit 패치 id).
struct TriangleMask { GpuBuffer State; GpuBuffer ReplacedIB; uint32_t TriangleCount; uint32_t Revision; };

// node에 강체로 붙는 조각.
struct NodeAttachment { uint32_t NodeId; Math::Matrix4 NodeWorldAtCut; Math::Matrix4 PieceWorldAtCut; };
```

## 4. 단계

| 단계 | 내용 | 완료 증거 | 예상 비용 |
| --- | --- | --- | --- |
| SP-7a | D2 `BuildSkeletalPhysicsAsset` + CPU 테스트(palette 병합, 다중 root·E 불일치 거절, frame·anchor) + 진단 로그(등록 없음) | Xbot·CesiumMan body/joint 수, Validate PASS, 표본 프레임 최대 anchor 오차 로그, CPU 테스트 PASS | 구현 + R 2~5분 + B 약 2분 + T |
| SP-7b | C 시점 지연 등록 + `FindIncomingJoint` + RequestId `QueueBreak`/결과 표. 이 단계만 숨김·부착 없이 원본에 바로 break(늘어남 정상), 오프셋 복사본 유지 | Xbot 팔·다리, CesiumMan 팔: 기대 JointId·subtree 낙하, root·AlreadyBroken·physics off 거절, pause 대기 | 구현 + R + B + V |
| SP-7c | D3 상태 버퍼·퇴화 치환 IB·`RenderMeshes` IB 분리 + 진단 옵션(지정 joint 범위 숨김 fixture) | 지정 범위가 color/depth/shadow/Wire에서 사라짐, 같은 Model의 다른 인스턴스 불변, D3D12 ERROR 0 | 구현 + R + B + V |
| SP-7d | D3 추출 제거 조건(GPU·CPU) + D6 `adjacentPatch` 로그 | P 절단 뒤 인접 자식 Q 절단 시 복사 수 = Q 범위 − 이미 제거된 수, GPU==CPU | 구현 + R + B + T + V |
| SP-7e | D4 부착·물리 제외 + D5 break 성공 뒤 게시로 7b 경로 교체 + 오프셋 복사본 진단 옵션화 | 두 모델에서 게시 프레임 경계 오차 ≤ 1 mm, 이후 프레임 오차 보고, 강제 Stale 시 원본 불변, 캡처 | 구현 + R + B + V |

각 단계는 약 300줄 이내다. 진행은 Codex 구현 → 별도 Codex 리뷰 → 로컬 커밋 순서다. 300줄을 넘으면 셰이더와 C++ 연결을 나눈다.

## 5. 완료 조건 (SP-7 전체)

- Xbot과 CesiumMan의 팔·다리·말단을 C로 자르면 다음이 모두 성립한다.
  - P의 incoming constraint만 끊기고, 기대 subtree만 Dynamic이 된다.
  - 원본 패치 범위가 color/depth/shadow/Wireframe에서 사라진다.
  - A는 P의 부모 node를, B는 P node를 따라간다.
  - 게시 프레임 경계 오차는 1 mm 이하다.
- 거절 상황(root, body 없음, 이미 끊김, physics off, 모델 거절, Stale)에서는 원본·물리·화면이 바뀌지 않고 사유 로그만 남는다.
- 인접 joint를 연속 절단하면 이미 제거된 삼각형은 다시 복사되지 않는다.
- 기존 Static 절단, SP-6 비교 진단, CPU 테스트, Debug/Release 빌드가 PASS이고 D3D12 ERROR는 0이다.

## 6. 남은 위험

- 애니메이션에 이동이나 scale 키가 있으면 내부 anchor 오차가 0.1 mm를 넘을 수 있다. 그 경우 break가 UnsupportedPose가 된다. 7a 로그로 먼저 확인한다.
- `mixedOutsideTriangles`(선택 joint 없이 자손과 바깥에 걸친 삼각형)는 숨기지 않으므로 늘어난다.
- 강체 부착 오차가 크면 정점 기여 교체(NDC 7:27~8:14)를 후속 단계로 올린다.

## SP-7a 구현·검증 (2026-11-03, base `2e8dcdff`, 미커밋)
- `BuildSkeletalPhysicsAsset`는 section E·palette와 최종 node pose를 받아 영향 NodeId를 병합한다. CPU geometry는 `ModelLoader`의 업로드 후 소멸하므로 정점 bounds 대신 segment 길이의 15% 반경을 사용한다. leaf는 들어오는 segment 길이, 고립·영길이 본은 0.05 m, 최소 반경/반길이는 0.005 m다. 밀도는 1000 kg/m³, swing 2.5 rad, twist ±3 rad다.
- `SkeletalPoseSnapshot::Sections`는 모델의 skin mesh 순서이며 E는 호출자가 렌더 경로와 동일하게 제공한다. 모델 overload는 실제 palette·영향 플래그를 사용한다. 등록·break·렌더·C 입력 변경은 없다.
- CPU Debug/Release 각 21검사 PASS. ModelViewer Debug/Release 빌드 PASS(151/201초, TEMP 출력, 변경 없는 MiniPhysics/Jolt 재사용). `-skeletal-auto-rig-checks 1`은 0/0.125/0.25/0.375/0.5초의 독립 pose를 검사한다.
- Xbot 52 body/51 joint, CesiumMan 19/18, 양쪽 Validate PASS. Debug 최대 anchor 오차 Xbot 2.88117e-6 m/CesiumMan 5.72810e-6 m, Release 2.88117e-6/4.77079e-6 m. 등록 시 anchor는 같은 위치에서 유도되며 float 왕복 잔차는 약 3e-6 m다.
- Release Static 게시 1회·GPU 검증 PASS, SP-6 20회 GPU==CPU PASS, 모든 실행 D3D12 ERROR/CORRUPTION 0. 실제 C 키 수동 입력은 미검증. 독립 리뷰 결과는 아래에 기록한다. GPU NVIDIA GeForce MX450; 드라이버 버전 조회 불가. 로그·산출물은 `%TEMP%/SP7a`, 원본 자동 rig 로그는 `%TEMP%/SkeletalAutoRigChecks.log`다.
### SP-7a 독립 리뷰·재검증 (2026-11-03)
- 진단에서 frame 변환 실패 후 미초기화 행렬 사용과 대상 모델 누락의 정상 종료를 수정했다. 매 표본 pose 유효성도 확인한다. 가장 먼 영향 자식·leaf 원점과 fallback 길이 검사를 추가했다.
- CPU Debug/Release 각각 AutoRig 28검사와 SkeletalPatch 59검사 PASS. VS 개발 환경 ModelViewer Debug 95초/Release 178초 빌드 PASS. 변경 없는 MiniPhysics/Jolt는 기존 라이브러리를 재사용했다. 로그·빌드 산출물은 `%TEMP%/SP7a-review`다.
- 두 구성 모두 `-debug 1 -skeletal-auto-rig-checks 1` 종료 0, D3D12 ERROR/CORRUPTION 0, 모델별 5표본·등록 0. Xbot 52/51과 CesiumMan 19/18, Validate PASS. 최대 anchor 오차는 Debug 0.002881/0.005728 mm, Release 0.002881/0.004771 mm다. GPU는 NVIDIA GeForce MX450이며 드라이버 버전은 조회하지 못했다.
- E*Gnode 배율 제거·미터 offset·anchor/limit 계약·NodeId 병합·단일 root/E/비균일 scale 거절을 확인했다. C/Static/등록 경로 변경은 없다. 한글 함수·단락 주석과 프로젝트 등록을 확인했다. 신규 C++는 334줄로 약 300줄 권고를 초과한다. D2 bounds·leaf 규칙은 미충족이며 설계 결정 없이 완료로 간주하지 않는다. 실제 C/Static GPU 재실행 및 Jolt `SupportsJointPose`를 통한 break는 이번 리뷰에서 미검증이다.
