상태: 진행 중 — SP-7e 게시·부착 구현; 연속 캡처 시연 옵션·리뷰 수정(미커밋, base `03b5c8a1`, 2026-11-06), 리뷰 수정은 정적 검토만 수행

# Skeletal 패치 절단 + BreakConstraint (SP-7)

## 연속 캡처 시연 옵션 (2026-11-06, base `03b5c8a1`, 미커밋)

- `ModelViewer::Update`는 `-skeletal-break-demo 1|2`에서 기본 장면·애니메이션·물리 실행을 사용하고 60프레임 이후 정상 `SkeletalPatchCopy::Request`를 한 번 호출한다. 고정 스텝은 `PhysicsStepClock::FixedSeconds`다.
- `BuildSkeletalBreakPlane`을 `RunBreakChecks`와 공유한다. 1은 Xbot P60→61(JointId 43), 2는 CesiumMan P8→9(JointId 5)이며 현재 본 중점·본 방향 수직의 0.3×0.3 m 평면이다.
- `FocusBreakCamera`는 요청 시 평면 중심을 향해 `2.5 * normalize(0.5, 0.2, 0.8660254)` m 오프셋으로 한 번 배치한다.
- `SkeletalPatchCopy::Update`의 Applied·A/B 게시 완료를 `TakeCaptureRequest`로 전달한다. `-capture <prefix>`이면 해당 프레임부터 +0,+20,…,+140에 `<prefix>-demo-000`부터 `007`까지 `-hdr.raw`·`-post.raw`를 저장한다. `-physics-query-exit` 프레임 종료는 게시 성공 후 캡처가 없거나 8장을 모두 저장했을 때 허용한다. 종료 옵션이 없으면 성공 시 창을 유지한다.
- `-skeletal-break-demo-unmask 1`은 `SetTriangleMask`를 생략하고 break·A/B 부착을 유지한다. 제거 상태·patch id도 게시되지 않으므로 첫 C 게시 이후 수동 C를 포함한 모든 후속 C 요청을 로그와 함께 거절한다. D6 연속 절단을 지원하는 모드가 아니며 재절단에는 재시작이 필요하다. 기본값 0이며 시연 외에는 적용하지 않는다.
- 시연 요청 이후 pending 작업이 끝났는데 Applied·A/B 게시 신호가 없으면 요청 거절 또는 게시 실패로 판단한다. 오류 로그를 남기고 `IsDone`으로 정상 정리 후 종료하며 캡처나 `-physics-query-exit` 프레임을 기다리지 않는다. 사용자 일시정지 중 실제 step을 기다리는 요청은 실패로 판정하지 않는다.
- `Startup`은 자동 검사·벤치·자동 절단 옵션과의 동시 지정을 값이 0이어도 `std::invalid_argument`로 거절한다. `skeletal-{patch-break,patch,mask,auto-rig,break}-checks`, `skeletal-break-xbot`, `xbot-arm-checks`, `edit-{target,apply,delete}-checks`, `retired-resource-checks`, `component-checks`, `physics-{sim,query,cut}-checks`, `physics-query-self-test`, `physics-cut-scenario`, `cut`, `cut-benchmark`, `cut-benchmark-out`, `cut-scene`, `cut-transform-checks`, `cut-validate`, `cut-expect-loops`, `cut-expect-publications`, `demo-tower`가 대상이다. `capture`, `physics-query-exit`, 표시·카메라 진단 옵션은 함께 사용할 수 있다.
- 매 시연 캡처 로그는 카메라 위치, 전방 2.5 m 시선 대상 위치, 물리 활성·실행 상태와 누적 simulation step을 200자 이하 한 줄로 기록한다.
- 호출자 제공 Release 실행 관찰: 두 모델의 break Applied를 확인했고 CesiumMan은 원본 숨김/비숨김 차이를 확인했다.
- 같은 관찰에서 Xbot 다리 낙하는 보지 못했으며 일부 실행은 +140f에 카메라가 튀었다. 원인은 미확인이다.
- 이번 리뷰 수정은 정적 diff와 호출 흐름만 검토했다. 빌드·실행은 수행하지 않았으며 위 관찰은 수정 후 검증 결과가 아니다.

[SkeletalPatchCopyCut.md](SkeletalPatchCopyCut.md)의 SP-6까지 C 입력은 원본 옆 정적 복사본만 자른다. SP-7은 같은 C 입력으로 원본을 실제로 끊는다. 순서는 NDC25 해석([NDC25Reference.md](../NDC/NDC25Reference.md) 5절)의 1~5를 따른다.

## 1. 목표

C를 누르면 평면이 지나는 skin GameObject에 대해 다음을 처리한다.

1. Parent joint P를 고르고 패치를 추출한다(SP-6 그대로).
2. 패치를 평면으로 잘라 조각 A/B를 만든다.
3. P의 incoming constraint를 끊는다.
4. 원본에서 패치 범위를 숨긴다.
5. A는 남는 쪽 node에, B는 떨어지는 쪽 node에 붙인다.

대상은 아래 D2 제약을 만족하는 모든 skeletal mesh다(사용자 결정 2026-11-03). 검증 모델은 Xbot 전신과 CesiumMan이다.

현재 C 키는 Rig 모드가 기본이며, 경계 구 + `SelectSegmentJoint`는 rig 미등록 owner fallback·Parent 진단 모드로 남는다([RagdollRigPreload.md](../Physics/RagdollRigPreload.md)).

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
    - 원점: dominant 정점의 body frame bounds 중심. 정점이 없으면 기존 선분 중점 fallback
    - 축: P와 가장 먼 영향 자식의 선분 방향을 유지한다. 정점이 있는 말단은 기존 본 X축을 유지한다
    - 크기: 그 joint가 dominant인 정점만의 bounds half size(body frame 기준), 각 축 최소 0.005 m
    - 말단: 본 원점을 강제로 포함하지 않고 dominant 정점 bounds만으로 중심·크기를 정한다.
    - 사용자 결정(2026-11-03, SP-7a2): Model이 skin section별 CPU 위치·joint·weight를 보존한다. `BuildInfluencingJoints`가 GPU 업로드 전 mesh-local 위치 float 3개와 uint16 joint/weight 각 4개(정점당 28 B)를 저장한다. Static 정점 복사와 index/draw 복사는 없다.
    - `BuildSkeletalPhysicsAsset`는 GPU와 같은 양수 합 정규화 weight로 `E * Gjoint * inverseBind`를 합성한다. SP-7a3 결정은 Unreal Physics Asset 생성의 Dominant Weight 방식에 맞춰 정점마다 최대 정규화 weight의 NodeId 하나에만 배정한다. 정규화 분모가 같으므로 원래 uint16 weight를 비교하며 동률은 낮은 NodeId가 이긴다. 기존 회전에서 bounds 중심을 body 원점으로 옮기고 `BoneFromBody`를 다시 구한다. anchor는 자식 본 원점을 양쪽 body frame으로 표현하므로 그대로 일치한다.
    - dominant 정점이 없는 본만 기존 segment 근사로 대체하고 NodeId와 이유를 stderr에 기록한다. `RunSkeletalAutoRigChecks`는 모델별 fallback 수와 half extent 최솟값·최댓값·최대 joint 이름을 기록하며 `%TEMP%/SP7a3/BodyExtents.csv`에 모든 body 크기를 쓴다.
    - 변경 이유: 모든 양수 weight를 포함한 SP-7a2는 CesiumMan `leg_joint_L_2`에 1.1% weight 정점 때문에 half extent 1.06 m, Xbot `LeftUpLeg`에 0.597 m의 거대 box를 만들었다. 5% 임계값만 적용해도 CesiumMan은 0.74 m가 남아 dominant 배정과 중심 보정을 함께 선택했다.
    - CPU 보관 비용 실측: Xbot 28,374정점, Debug 794,536 B / Release 794,520 B(약 776 KiB); CesiumMan 3,273정점, 91,676 / 91,668 B(약 90 KiB). `m_SkinCpuSections`의 정점·section capacity 기준이며 allocator와 Model 내부의 빈 vector 객체 비용은 제외한다. inverse bind는 기존 Model 데이터를 재사용한다.
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
- A/B가 준비되면 `QueueBreak(request, true)`를 호출해 RequestId를 받는다. `AdvanceSimulation`은 결과 보관을 지정한 요청만 `BreakStatus`를 남기며 `TryGetBreakResult`로 한 번 소비하거나 `Clear`로 해제한다. 기존 결과 미조회 호출은 보관하지 않는다.
- 다음 실제 substep에서 Applied가 나오면 같은 프레임 `SkeletalPatchCopy::Update`에서 다음을 게시한다. `ModelViewer::Update`는 물리 갱신 뒤 이 경로를 실행하므로 늘어난 원본이 보이는 프레임이 없다.
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

// 요청 번호를 돌려주며 keepResult=true인 결과만 번호로 한 번 조회한다.
uint64_t PhysicsSystem::QueueBreak(const BreakRequest& request, bool keepResult = false);
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
| SP-7a2 | Model의 최소 skin CPU 복사본과 등록 pose 정점 bounds, leaf bounds, fallback·메모리·extent 진단 | D/R CPU 43 checks, 두 모델 fallback 0, 메모리·bounds 실측, Static·SP-6 회귀 | D/R 빌드 실측 75/77초 + 실행 진단 |
| SP-7a3 | dominant 배정·낮은 NodeId 동률·bounds 중심 box·이름별 CSV | D/R AutoRig·SkeletalPatch CPU, D/R 빌드·두 모델 크기/anchor·D3D12 진단 | 직전 빌드 실측 75/77초 + R 2~5분 |
| SP-7b | C 시점 지연 등록 + `FindIncomingJoint` + RequestId `QueueBreak`/결과 표. 이 단계만 숨김·부착 없이 원본에 바로 break(늘어남 정상), 오프셋 복사본 유지 | Xbot 팔·다리, CesiumMan 팔: 기대 JointId·subtree 낙하, root·AlreadyBroken·physics off 거절, pause 대기 | 구현 + R + B + V |
| SP-7c | D3 상태 버퍼·퇴화 치환 IB·`RenderMeshes` IB 분리 + 진단 옵션(지정 joint 범위 숨김 fixture) | D/R IB·color/depth/shadow readback·독립 컴포넌트 보존·복원 PASS, D3D12 ERROR 0; Wire/Vertices 픽셀 미검증 | 구현 + R + B + V |
| SP-7d | D3 추출 제거 조건(GPU·CPU) + D6 `adjacentPatch` 로그 | 완료(미커밋): Xbot P=6→Q=12, 934−799=135, 인접 id=1 64개, GPU==CPU·D/R 검증 PASS | 구현 + R + B + T + V |
| SP-7e | D4 부착·물리 제외 + D5 break 성공 뒤 게시로 7b 경로 교체 + 오프셋 복사본 진단 옵션화 | 게시·부착 구현(미커밋), 확장 진단 후속: 두 모델에서 게시 프레임 경계 오차 ≤ 1 mm, 이후 프레임 오차 보고, 강제 Stale 시 원본 불변, 캡처 | 구현 + R + B + V |

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

## SP-7a2 구현·검증 (2026-11-03, base `731c7005`, 미커밋)
- `BuildInfluencingJoints`가 skin section CPU 정점을 Model에 보존하고 `BuildSkeletalPhysicsAsset`가 등록 pose의 정점 bounds를 사용한다. leaf는 본 원점과 정점만으로 정하며, 양수 weight 정점이 없는 본만 segment fallback을 기록한다. GPU 버퍼·셰이더·Static·C 등록 경로는 변경하지 않았다.
- `SkeletalAutoRigTests` Debug/Release 각 43 checks PASS. 혼합·비정규화 weight, inverse bind, 회전·이동·균일 배율, segment/leaf bounds, section 간 NodeId 병합, zero weight 제외, fallback 수와 잘못된 joint/inverse bind 거절을 확인했다. 기존 `SkeletalPatchTests`도 각 59 checks PASS.
- VS 개발 환경 v143/SDK 10.0.26100.0, `ModelViewer.slnx` Debug/Release `Windows` 빌드 exit 0(최종 75.25/76.77초). 출력은 `%TEMP%/SP7a2`, 변경 없는 MiniPhysics/Jolt는 기존 prebuilt를 재사용했다. 신규 `SkinCpuData.h`는 Model 프로젝트와 filters에 등록했다.
- `-debug 1 -skeletal-auto-rig-checks 1`: D/R exit 0(23/22초), 각 모델 5표본, 등록 0. Xbot 52 body/51 joint, CesiumMan 19/18, Validate 모두 PASS, fallback 모두 0. Xbot half extent 0.01332747936~0.5971588492 m, CesiumMan 0.1145477295~1.059964776 m(D/R 동일).
- 최대 anchor 오차: Xbot D/R 3.414378853e-6 m, CesiumMan Debug 5.728101314e-6 / Release 4.770792202e-6 m. CPU 복사본 메모리 실측은 D2에 기록했다. 로그는 `AutoRig-Debug.log`, `AutoRig-Release.log`다.
- `-debug 1 -skeletal-patch-checks 1`: D/R exit 0(106/46초), 각 20/20 GPU==CPU PASS. 이번 실행의 `SP6 comparison` 20행과 기존 `%TEMP%/SP6-review-comparison-exact.log` 비교 차이는 각 0행이다. Debug 누적 로그는 마지막 20행을 비교했고 Release는 로그를 비운 뒤 실행했다.
- Static 재현 옵션: `-debug 1 -model Models/cube.glb -cut 1 -cut-angle 30 -cut-validate 1 -cut-expect-loops 1 -cut-expect-publications 1 -physics-query-exit 90`. D/R exit 0(24/21초), 게시 1건, 양쪽 16삼각형·cap 2삼각형, 면적 약 6.158399·부피 각 약 4, 부모 보존·DOP·hull containment PASS. 기존 Static 진단 계약을 유지한다.
- 자동 rig·SP-6·Static 총 6실행 모두 D3D12 ERROR/CORRUPTION 0. 활성 GPU NVIDIA GeForce MX450, 드라이버 32.0.15.9186(레지스트리 확인). 기존 Live-resource WARNING·DXGI 하이브리드 출력 ERROR #80·ShadowPlay 87은 별개로 남는다. 런타임 로그는 `%TEMP%/SP7a2/Runtime-{Debug,Release}-{AutoRig,SP6,Static}.log`다.
- 편차: 기존 최소 half extent 0.005 m 유지, index/draw CPU 복사 불필요. 수동 C 입력·실제 물리 등록/break와 별도 세션 독립 리뷰는 이번 단계에서 검증하지 않았다. SP-7b~e는 미구현이며 branch 변경·commit·push는 하지 않았다.
## SP-7a3 구현·검증 (2026-11-03, base `7090421e`, 미커밋)
- `BuildSkeletalPhysicsAsset`는 최대 정규화 weight의 본 하나에 정점을 배정하고 동률은 낮은 NodeId로 정한다. segment 회전과 leaf의 기존 회전을 유지한 채 순수 dominant bounds 중심·half size를 사용한다. `BoneFromBody`와 양쪽 joint frame을 새 중심에서 구하며, dominant 정점이 없으면 기존 segment fallback을 기록한다.
- `SkeletalAutoRigTests` Debug/Release 각 55 checks PASS. 역순 동률·비정규화 혼합 weight·inverse bind·회전/균일 배율·segment/leaf 중심·anchor의 자식 본 원점 일치·section 병합·zero weight 제외·fallback·단일 정점의 0.005 m 하한·잘못된 입력 거절을 확인했다. `SkeletalPatchTests`도 각 59 checks PASS.
- VS 2022 개발 환경 v143, SDK 10.0.26100.0, `ModelViewer.slnx` Debug/Release `Windows` 빌드 exit 0. TEMP 신규 출력의 첫 빌드는 156.40/178.57초, 최종 헤더 반영 확인은 38.07/30.30초였다. 변경 없는 MiniPhysics/Jolt는 기존 prebuilt를 재사용했다. 출력과 로그는 `%TEMP%/SP7a3`에 보관한다.
- `-debug 1 -skeletal-auto-rig-checks 1`: Debug/Release exit 0(26/25초), 모델별 5표본·실제 등록 0. Xbot 52 body/51 joint, CesiumMan 19/18, Validate 모두 PASS, fallback 각각 0. 양쪽 D3D12 ERROR/CORRUPTION 0. NVIDIA GeForce MX450, 드라이버 32.0.15.9186; 기존 DXGI 하이브리드 출력·ShadowPlay 진단은 별도로 남는다.
- 두 구성의 최소/최대 half extent는 Xbot 0.01332747936/0.4745310843 m(`mixamorig:RightUpLeg`, NodeId 65), CesiumMan 0.0873504281/0.4399961829 m(`Skeleton_torso_joint_1`, NodeId 2)다. 문제였던 Xbot `mixamorig:LeftUpLeg`는 0.4725240469 m, CesiumMan `leg_joint_L_2`는 0.3736785650 m로 줄었다.
- 비율 해석: `ModelViewer::Startup`의 `2 / boundingSphere.radius` 배율과 MINI 헤더 bounds로 구한 장면 높이는 Xbot 약 3.208 m, CesiumMan 약 3.900 m다. Xbot 허벅지 box 전체 길이 약 0.949 m(높이의 29.6%), CesiumMan 몸통 약 0.880 m(22.6%)는 대략적인 인체 box로 타당해 보인다. 시각적 밀착도나 충돌 품질을 검증한 판단은 아니다. 계산 근거는 `ModelBounds.txt`다.
- 등록 pose(sample 0)의 최대 anchor 오차는 Xbot 3.159522748e-6 m, CesiumMan 3.144542234e-6 m다. 5표본 최대는 각각 3.372932952e-6/3.966760687e-6 m이며 D/R 동일하다. body 중심을 옮겨도 자식 본 원점 anchor 계약이 유지된다.
- `RunSkeletalAutoRigChecks`와 `ReadDiagnosticNodeNames`는 원본 GLB의 기본 scene을 `ModelConvert::WalkGraph`와 같은 깊이 우선 순서로 읽어 이름을 붙인다. `%TEMP%/SP7a3/BodyExtents.csv`는 71 body의 세 축 half extent(m)를 담는다. 구성별 보존본은 `BodyExtents-Debug.csv`, `BodyExtents-Release.csv`, 진단 요약은 `AutoRig-{Debug,Release}.log`, D3D12 로그는 `Runtime-{Debug,Release}-AutoRig.log`다.
- 미검증: 수동 C 입력·실제 물리 등록/break·충돌 box overlay 시각 확인·별도 새 세션 독립 리뷰. SP-7b~e는 미구현이다. branch 변경·commit·push 없이 이 단계에서 멈춘다.- 독립 리뷰: Codex 리뷰는 용량 한도로 중간 종료됐다. 중간 결론은 dominant 배정·동률·미터 offset/anchor 일관, 결함 없음이다. 남은 CSV 판단은 Opus가 확인했다: 71 body 중 0.005 m 클램프 0개, 좌우 대칭 본의 크기 차이 1% 이내, 최대 box는 허벅지·몸통·머리다. 본-box 공간 겹침 계측은 미검증이다.

## SP-7b 구현·검증 (2026-11-03, base `18848ec0`, 미커밋)
- `SkeletalPatchCopy::Request`는 미등록 Parent owner만 최종 pose로 등록하고 incoming·세대를 저장한다. 지원 거절은 이유를 기록하고 SP-6을 유지하며 A/B 준비 뒤 무충격 `QueueBreak`를 제출한다. 숨김·부착은 후속 단계다.
- `FindIncomingJoint`는 NoBody·RigRoot·AlreadyBroken·NotRegistered를 구분한다. `AdvanceSimulation`은 RequestId별 BreakStatus를 보관하고 `TryGetBreakResult`는 한 번 소비한다.
- VS 2022/v143/SDK 10.0.26100.0, Windows(x64) Debug/Release 빌드 exit 0. CPU AutoRig 55·SkeletalPatch 59·SkeletalBreak 914 checks씩 PASS.
- `-skeletal-patch-break-checks 1` D/R PASS: Xbot 전완 JointId 기대/실제 7/7, subtree={14~18,20~22,24~26,28~30,32~34}; 허벅지 43/43, 추가={60~63}(누적 21); CesiumMan 팔 5/5, subtree={8,9}. Dynamic 전환 17/4/2, 모두 Applied.
- RigRoot 조회·같은 C의 AlreadyBroken·physics off 거절·pause 대기 PASS. 기존 break fixture/Xbot·Static 게시 1건/면적/부피/hull·SP-6 20회 GPU==CPU도 D/R PASS. 10회 실행 D3D12 ERROR/CORRUPTION 0, NVIDIA MX450/32.0.15.9186.
- 산출물·로그는 `%TEMP%/SP7b`를 재사용한다. offset 복사본·0.1 m 간격은 유지한다. 수동 C·지원 거절 모델의 통합 fallback은 미검증이다.

- 독립 리뷰: 미조회 break 결과의 누적을 막도록 결과 보관을 명시적 선택으로 바꿨다. JointId 7/43/5와 subtree 17/4/2개는 원본 GLB weight·DFS로 재확인했다. D/R CPU 55/59/915 checks, 결과 분리·한 번 소비·Clear 통합 검증 PASS. 로그는 `%TEMP%/SP7b/*Review*.log`에 보관한다.

## SP-7c 셰이더 단계 (2026-11-03, base `94f2bf24`, 미커밋)
- 약 300줄 제한에 따라 셰이더·프로젝트 등록·독립 GPU 테스트에서 분할한다. 컴포넌트 소유권·렌더 IB 분리·모델 진단 연결은 다음 단계다.
- `TriangleMaskCS::main`은 DWORD 단위로 원본 IB를 재구성한다. `Replacement`는 누적 `FirstTriangle`의 bit31만 판정하고 원본 첫 인덱스를 반환하며 baseVertex는 바꾸지 않는다.
- 입력 draw 배치는 `PatchExtractCS`와 같다. root는 b0의 `FirstWord/WordCount/Draws/Padding`, t0 원본, t1 draw, t2 State, u0 별도 출력이다. 원본과 출력 바이트 오프셋을 맞춘다.
- `TriangleMaskShaderTests::main` D/R 각각 13표본·1,664 DWORD 비교, 불일치 0·D3D12 ERROR 0(MX450). R16 공유 DWORD·홀수 StartIndex·R32·빈 영역·소유권만 설정·개별/전체 제거·해제 복원을 검사한다.
- ModelViewer D/R 빌드 PASS. CPU AutoRig 55·SkeletalPatch 59·SkeletalBreak 915 checks씩 PASS. Static·SP-6·기존 break 진단 D/R 10실행 ERROR 0, SP-6 각 20행 기준 로그 차이 0. 출력·로그는 `%TEMP%/SP7b`만 사용한다.
- 미구현·미검증: `TriangleMask` 자원/Revision/수명, `MeshSorter::AddMesh`·`RenderMeshes` 연결, `-skeletal-mask-checks`, Xbot/CesiumMan 패스별 숨김·다른 인스턴스 보존·복원. C 입력은 변경하지 않았다.
- 독립 리뷰: 기존 초과 dispatch는 자원 밖 쓰기를 놓칠 수 있어 `TriangleMaskShaderTests::main`에 원본과 다른 양끝 sentinel을 추가했다. `FirstWord=1`, `WordCount=126`, 192스레드로 시작 오프셋·종료 경계와 이전 출력 복원을 검사한다. D/R 각각 13표본·1,664 DWORD(경계 sentinel 26개 포함), 불일치 0·D3D12 ERROR 0(MX450).
- 리뷰 재검증: ModelViewer D/R 증분 빌드 exit 0(37.3/34.6초, 기존 MiniPhysics/Jolt 의존성 재사용), `CutPlaneTests` D/R PASS, `git diff --check` PASS. 요청된 `PlaneSlicerTests.cpp`는 현재 저장소에 없어 현행 `MeshCutting/tests/CutPlaneTests.cpp`의 헤더 명령으로 실행했다. 대형 IB의 2차원 dispatch·GPU 드라이버 버전·기존 모델 진단은 이번 리뷰에서 재검증하지 않았다. 로그는 `%TEMP%/SP7b/{Build-*,MaskTests-*,CutPlaneReview-*}`에 둔다.

## SP-7c 컴포넌트 연결·검증 (2026-11-03, base `d10077bd`, 미커밋)
- `MeshComponent::SetTriangleMask`·`UpdateTriangleMask`·`ClearTriangleMask`는 상태·compact IB·Revision을 컴포넌트별로 소유하며 geometry 교체 때 해제한다. 최초 compute 전에는 원본 IB를 유지한다. 소멸·해제는 기존 GPU 대기로 제출된 사용을 보호한다.
- `MeshSorter::AddMesh`·`RenderMeshes`는 VB/IB 주소만 분리하여 모든 패스의 ibOffset·format·draw 범위·baseVertex와 깊이 전용 VB를 유지한다. 마스크 compute는 전달받은 그래픽 목록에서 실행되며 일반 프레임의 Update→Render 목록은 같은 direct queue 제출 순서를 따른다.
- 독립 리뷰에서 `RunSkeletalMaskChecks`를 패치 복사와 의존성이 없는 `Scene/SkeletalMaskChecks.cpp`로 분리했다. padding을 제외한 모델 커버리지 ROI에서 before→after clear 소실을 검사하며 color는 같은 픽셀의 depth 소실도 요구한다. Revision 유지·State 0 갱신·공유 Model 전체 불변·별도 MeshComponent 보존·Clear 완전 복원을 검사한다.
- D/R `-skeletal-mask-checks 1`: Xbot NodeId 14는 2,206삼각형, CesiumMan NodeId 8은 271삼각형 제거. shadow/depth/color 소실 픽셀은 각각 4,623/2,288/1,786과 20,037/9,903/9,863이며 변경 바이트는 9,826/9,887/8,057과 40,273/39,916/39,824다. IB CPU 기대값·원본·peer·갱신/해제 복원 PASS.
- 리뷰 재검증: D/R 빌드 exit 0(41.8/38.7초), D/R 마스크 진단과 Release `-compute-pose 0` exit 0·D3D12 ERROR/CORRUPTION 0(MX450). CPU AutoRig 55/SkeletalPatch 59/SkeletalBreak 915·CutPlaneTests PASS. `TriangleMaskShaderTests::main`은 원래 offset/compact 출력 각각 13상태(총 26표본·3,328 DWORD), 불일치 0·D3D12 ERROR 0. 출력은 `%TEMP%/SP7b`만 사용하며 MiniPhysics/Jolt는 변경 없는 기존 의존성을 재사용한다.
- 미검증: 실제 V/C 입력·Wireframe/Vertices 픽셀·대형 IB 2차원 dispatch·GPU 진행 중 삭제 스트레스. `PlaneSlicerTests.cpp`는 없어 현행 CutPlaneTests를 실행했다. SP-7d~e는 후속이며 C 입력은 이번 범위에서 변경하지 않는다.

## SP-7d 구현·검증 (2026-11-03, base `f83ecf1e`, 미커밋)
- `PatchExtractCS::main`·`ExtractPatchReference`는 전역 삼각형 State의 bit31을 선택에서 제외하며 마스크 없음은 0 버퍼로 처리한다. 교차·미포함 카운터에서도 제거 삼각형을 제외한다. Influence 투표 규칙은 유지한다.
- `SkeletalPatchJob::Submit`·`Poll`은 VB 시작·stride와 baseVertex 적용 후 정점 번호가 같은 제거 삼각형을 소유 id별로 한 번씩 집계하고 `adjacentPatch`를 기록한다. draw/section 경계도 같은 조건으로 비교하며 공유 정점 하나면 인접이다(변 공유나 위치 용접은 요구하지 않음). GPU 비용은 제거 수×전체 삼각형 수, 최악 O(N²)이며 대형 입력 성능은 미검증이다. 병합과 C 게시 변경은 없다.
- `CheckMaskedPatch`·`RunSkeletalMaskChecks`: Xbot Spine2(P=6)→LeftShoulder(Q=12), 범위 934−제거 799=추출 135, `adjacentPatch id=1 removedNeighbors=64 separatePatch=1`, GPU==CPU 및 없음/0/해제 출력 바이트 비교 PASS.
- D/R 빌드(53.3/50.7초)·AutoRig 55·SkeletalPatch 63·SkeletalBreak 915·CutPlaneTests·TriangleMask 26표본/3,328 DWORD·마스크·SP-6 20행 기존 로그 일치·patch-break·Static PASS, D3D12 ERROR 0(MX450, 32.0.15.9186, `-debug 1`). 산출물은 `%TEMP%/SP7b`; 대형 입력 인접 집계 성능·실제 C 게시(SP-7e)는 미검증이다.
- 독립 리뷰: 제거 삼각형이 교차·미포함 카운터에 남는 GPU/CPU 누락과 0 버퍼 업로드의 16바이트 올림 범위 초과 읽기를 수정했다. 기존 metadata/selectedNode 정렬·패딩 변경도 `InitializeBuffer`의 SIMD 복사 계약상 필요하다. 실제 교차 평면으로 진단을 보강해 전체 제거 시 교차 373→0, 출력/교차/미포함 0/0/0을 확인했다.
- 재검증: D/R 빌드 39.9/30.8초(기존 MiniPhysics/Jolt 재사용), CPU 55/65/915·CutPlaneTests 및 TriangleMask 각 26표본/3,328 DWORD PASS. 마스크·SP-6·patch-break·Static 8실행 exit 0, SP-6 각 20행 불변, 934−799=135·인접 64·바이트 불변 유지. D3D12 ERROR/CORRUPTION·바인딩 경고 0; 기존 Release PSO 출력 #974·중복 barrier #1008 및 Debug live-resource 경고는 남는다. 근거: `%TEMP%/SP7b/Independent-*`, `Runtime-*-SP7d*.log`.

## SP-7e 게시·부착 단계 (2026-11-03, base `9400691f`, 미커밋)
- `SkeletalPatchCopy::Request`는 C의 geometry·mask revision과 두 node snapshot을 보관하고 Segment/Leaf 쪽을 결정한다.
- `SkeletalPatchCopy::OnCutResult`는 숨긴 준비 owner의 A/B가 갖춰진 뒤 break를 요청한다.
- `SkeletalPatchCopy::Update`는 Applied와 revision 일치 후 원본 mask를 갱신하고 원래 owner에 A/B를 게시하며 실패 준비물은 버린다.
- `Scene::PrepareMeshes`는 최종 pose 뒤 node 부착식을 적용한다. `PhysicsSystem::IsEligible`·`SyncBodies`는 조각을 제외하고 `ResolveEditTarget`은 owner를 고른다.
- `-skeletal-patch-offset-copy 1`에서만 기존 offset·B 간격을 유지한다. `RunBreakChecks`는 제거 범위·원래 owner·hull/body 제외를 확인한다.
- 독립 리뷰 수정: Leaf의 기준 조상이 평면에서 1 mm 이내면 단독으로 거절한다. C snapshot·매 프레임 갱신은 첫 skin section의 공통 E를 사용해 앞쪽 Static section을 제외한다. `Reset`은 `PhysicsSystem::CancelBreak`로 미실행 요청을 취소하고, 절단 제출 거절도 숨긴 준비 owner를 즉시 버린다. 이미 게시된 부착 조각·mask는 D6의 연속 절단을 위해 새 C에서 유지하며 owner 삭제 때 함께 해제한다.
- 약 300줄 단위의 게시·부착 단계로 분리한다. 경계 오차(게시/이후 N 프레임)·강제 Stale·모델별 캡처는 후속이며 §5 미충족으로 SP-7 완료가 아니다.
- 검증: D/R 최종 빌드 50.5/46.4초, CPU 55/65/915·CutPlane 및 TriangleMask 26표본/3,328 DWORD PASS. C·mask·offset SP-6 20행 불변·Static과 최종 C 재검증 모두 PASS, D3D12 ERROR 0. 근거는 `%TEMP%/SP7b/SP7e-Result.json`이며 Wireframe 픽셀·Leaf/거절 통합 검증도 후속이다.
- 리뷰 최종 소스 재검증 범위: D/R 빌드·CPU, Debug 게시·mask·offset SP-6(20행 동일)·Static, Release 게시까지 PASS. Release mask·offset SP-6·Static 재실행은 Codex 사용 한도로 미완료다.
