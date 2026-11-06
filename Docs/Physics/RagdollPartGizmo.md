상태: 설계안 — 사용자 결정 반영, Codex 설계 검토 반영(2026-11-06, base `f247a8aa`), 구현 전

# 떨어진 ragdoll 부위와 kinematic 몸의 개별 기즈모

## 1. 목표

BreakConstraint 뒤 rig는 두 부분으로 나뉜다.
- 끊긴 joint 아래의 Dynamic 연결 성분(예: 떨어진 팔)
- 애니메이션을 따르는 kinematic 나머지

지금은 어디를 클릭해도 owner 하나가 선택된다. 이 문서는 두 부분이 각자 기즈모를 갖게 한다.
- 떨어진 부위를 클릭하면 그 성분이 선택되고, 기즈모가 그 부위에 생긴다.
- 몸을 클릭하면 지금처럼 owner가 선택된다.
- owner를 옮겨도 떨어진 부위는 따라가지 않는다. running과 paused 모두 해당한다(사용자 결정).
- 패치와는 무관하다. 부착 패치를 클릭하면 그 node가 속한 부분으로 해석할 뿐이다.

## 2. 현재 구조 (조사·검토 요약, `f247a8aa`)

- 편집 대상과 선택
  - `EditTargetKind`는 Owner와 Mesh뿐이다.
  - `PickHit`에는 Id, WorldT, MeshId만 있다.
  - `ResolveEditTarget`은 skeletal source와 부착 패치를 Owner로 해석한다.
  - `DeleteEditTarget`은 `IsMesh()==false`면 owner 전체를 삭제한다.
- 기즈모와 기록
  - `TransformGizmo::Update`는 드래그 시작 TRS 기준의 절대 목표를 매 프레임 다시 적용한다.
  - `Cancel`(Esc)은 시작 TRS로 복원한다.
  - `TransformHistory`에는 Undo만 있다. `Undo`가 기록을 pop하며 Redo 스택은 없다.
- rig와 body
  - rig body 형상은 `BuildBoxBody`의 box다.
  - `PhysicsWorld::Impl::SimBodies`가 handle→BodyID 매핑을 가진다.
  - `QueryAabb`는 query collider 경로라 rig body에는 쓰지 않는다.
- 관절 상태 조회
  - `FindIncomingJoint`, `GetConstraintHandle`, `ReadDiagnosticState`로 joint Broken과 body Motion을 읽는다.
  - rig 식별은 Object와 `GetRigGeneration`이다. topology generation은 `Register` 때 1로 시작하는 별도 값이다. `RigId` 타입은 없다.
- body 조작
  - `PhysicsWorld::SetBodyTransform`과 `SetBodyVelocity`가 있다.
- pose writeback
  - writeback은 `PhysicsSystem::FinalizeSimulationPoses`가 한다(`ReadSkeletalPose`→`CollectPoseOverrides`→`FinalizeObjectPose`).
  - 이 경로는 실제 substep에서만 호출된다. 그래서 paused 상태에서 owner를 편집하면 렌더와 Attachment가 owner를 따라갈 수 있다.
- 배율 제약
  - `UpdateKinematicTargets`와 `CollectPoseOverrides`는 등록 당시 `FrameScale`과 같은 배율을 요구한다.

## 3. 결정과 이유

D1. **새 편집 대상 `RagdollPart`.**
- `EditTarget`에 다음을 담는다.
  - ObjectId
  - RigGeneration
  - TopologyGeneration
  - RootBody
- 성분은 매번 계산하고 저장하지 않는다.
- `IsEditTargetValid`와 `EditTarget::operator==`는 네 값을 모두 비교한다. 다르면 선택을 해제한다.
- 이유: break가 추가되거나 rig가 재등록되면 성분이 바뀐다.

D2. **성분 탐색 규칙.**
- hit body에서 시작해 살아 있는(끊기지 않은) incoming joint를 거슬러 올라간다.
- incoming joint가 끊긴 body가 root다.
- 끊긴 joint를 만나지 않고 rig root에 닿으면 kinematic 쪽이다. 이 경우 Owner로 해석한다.
- root부터 끊기지 않은 자손 joint를 따라 body를 수집한다.
- 모든 body의 Motion이 Dynamic인지 확인하고, 아니면 Owner로 해석한다.
- 부착 패치 hit는 `Attachment.NodeId`의 body에서 시작한다. 그 node에 body가 없으면 NodeParents를 따라 가장 가까운 조상 body를 찾는 함수를 추가한다.

D3. **선택은 rig body box에 대한 ray cast로 한다.**
- 기존 `PickObject`가 skeletal owner를 고르고, 그 rig에 끊긴 joint가 있을 때만 판정한다.
  - 같은 ray를 rig body들에 cast한다.
  - 가장 가까운 hit body에 D2를 적용한다.
- 신규 `PhysicsWorld::CastRayBodies(handles, origin, dir, maxDist, hitHandle, hitT)`를 둔다.
  - `SimBodies` 매핑으로 지정 body만 검사한다.
  - `maxDist`는 기존 pick ray 길이를 쓴다.
  - 반환값은 가장 작은 `hitT`의 handle 하나다.
- 이유: CPU에는 posed 삼각형과 index가 없다. 떨어진 부위 구분에는 box 근사로 충분하다.

D4. **pivot, 축, 핸들.**
- pivot은 RootBody의 현재 월드 위치다. Local 축은 RootBody의 회전이다.
- `RagdollPart`에서는 Scale 핸들을 비활성화한다.
- 등록 rig가 있는 owner에서도 Scale 편집과 Scale 기록의 Undo를 거절한다. 거절 로그를 남긴다.
  - 이유: `FrameScale` 불일치로 kinematic 갱신이 실패한다. rig 재생성은 범위 밖이다.

D5. **드래그 적용과 취소.**
- 드래그를 시작할 때 성분 body들의 월드 변환 snapshot을 저장한다.
- 매 프레임 기즈모의 시작→현재 강체 변환 `Δ`를 구해 각 body에 `Δ·Wbody_start`를 적용하고 속도를 0으로 만든다.
  - 이 방식은 `TransformGizmo::Update`의 절대 목표 재적용과 맞는다.
- running이면 step 전에 적용한다. 놓으면 정지 상태에서 다시 떨어진다.
- Esc는 snapshot 전체로 복원한다.
- 이유: kinematic 전환 없이 기존 API만으로 관절 자세를 유지할 수 있다.

D6. **writeback 분리.**
- `FinalizeSimulationPoses`의 writeback 부분을 `PhysicsSystem::RefreshSkeletalPoses()`로 공개한다.
- 다음 경우에 step과 무관하게 호출한다. 모두 `Scene::PrepareMeshes` 전이며, paused와 zero-step에서도 호출한다.
  - owner 편집 뒤
  - `RagdollPart` 편집 뒤
  - Esc 복원 뒤
  - Undo 뒤
- 이 함수가 Dynamic body 월드 pose로 node를 다시 계산하므로, 떨어진 부위가 owner를 따라가지 않는다.

D7. **Undo (Redo는 범위 밖).**
- `TransformHistory::Record`와 `ApplyHistoryCommand`에 body 목록 payload를 추가한다. payload는 body index와 Before 월드 변환이다.
- ObjectId, RigGeneration, TopologyGeneration이 다르면 그 기록을 건너뛴다.
- 복원 뒤 속도를 0으로 만들고 D6을 호출한다.

D8. **삭제.**
- `RagdollPart`에서 Delete는 거절하고 로그를 남긴다. owner 전체 삭제로 넘어가지 않는다.

## 4. 인터페이스 골격 (모두 신규)

```cpp
enum class EditTargetKind { Owner, Mesh, RagdollPart };

// PhysicsWorld: 지정 body 집합에만 ray를 쏘아 가장 가까운 hit를 돌려준다.
bool CastRayBodies(std::span<const BodyHandle> bodies, Vector3 origin, Vector3 dir, float maxDist,
    BodyHandle& hit, float& hitT) const;
// SkeletalPhysicsSystem: hit body가 속한 Dynamic 성분의 root와 body 목록을 찾는다(kinematic이면 false).
bool FindDynamicComponent(ObjectId owner, uint32_t body, uint32_t& root, std::vector<uint32_t>& bodies) const;
// PhysicsSystem: step 없이 현재 body pose로 skeletal node pose를 다시 확정한다.
void RefreshSkeletalPoses();
```

## 5. 단계 (단계당 한 개념, 약 300줄 이하)

시간은 이번 세션 측정값이다. Codex 구현 6~13분, Codex 리뷰 3~6분, 빌드와 실행은 단계마다 기록한다. AttachedPatchRecut의 AR 단계를 먼저 마친다.

| 단계 | 내용 |
| --- | --- |
| RG-1 | `CastRayBodies`, `FindDynamicComponent`, 조상 body 탐색. UI 변화는 없고, 진단 로그로 확인한다. |
| RG-2 | `RagdollPart` 대상: 해석, 유효성, `operator==`, Delete 거절, 기즈모 표시와 pivot. 적용은 아직 없다. |
| RG-3 | `RefreshSkeletalPoses` 분리와 호출 지점, paused 상태 owner 편집 비추종, Scale 거절. |
| RG-4 | 드래그 적용, snapshot, Esc 복원. |
| RG-5 | Undo payload와 세대 검사. |
| RG-6 | 진단 옵션 `-ragdoll-gizmo-checks 1`(break 후 성분 이동·owner 이동·Undo 자동 검사), Scene.md/CodeMap/README 동기화. |

## 6. 완료 조건

1. break 뒤 떨어진 팔을 클릭하면 기즈모가 그 RootBody 위치에 생긴다. 몸을 클릭하면 owner 기즈모가 생긴다.
2. 팔 기즈모로 이동·회전하면 성분 전체가 관절 자세를 유지한 채 움직인다. Esc는 시작 상태로 되돌린다. running이면 놓은 뒤 정지 상태에서 다시 떨어진다.
3. owner를 이동해도 떨어진 팔의 월드 위치가 1 mm 넘게 변하지 않는다. running과 paused 모두 해당한다.
4. Undo가 팔 이동을 되돌린다. rig나 topology가 바뀐 뒤의 기록은 건너뛴다.
5. `RagdollPart` Delete는 아무것도 지우지 않는다. 등록 rig owner의 Scale 편집은 거절 로그와 함께 무시된다.
6. `-ragdoll-gizmo-checks 1`이 Debug/Release 모두 PASS한다. D3D12 ERROR 0이다.
7. break가 없는 skeletal owner와 Static 조각의 선택·기즈모 동작은 바뀌지 않는다.
