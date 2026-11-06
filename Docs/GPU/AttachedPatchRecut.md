상태: 진행 중 — AR-1 `a4d337b1`, AR-2 `1a017f5a`, AR-3 반대쪽 조각 KDOP 충돌·body 등록 경로 완료(D/R 빌드, break checks·cube 회귀 동일; 실제 낙하는 AR-4 검사로 확인 예정), AR-4 미착수 (2026-11-06)

# 부착 패치 조각의 Static 재절단

## 1. 목표

스켈레탈 절단(SP-7) 뒤 node에 부착된 패치 조각 A/B는 같은 자리를 다시 자를 수 없다.
- 원본 삼각형은 마스크로 숨겨져 있다.
- 패치는 `PhysicsSystem::IsEligible`이 Attachment를 이유로 Static 후보에서 뺀다.

이 문서는 부착 패치를 기존 Static 절단기로 자르는 방법을 정한다.
- node가 있는 쪽 결과만 부착을 유지한다.
- 반대쪽은 Attachment를 풀어 독립 Static 조각(rigid body)으로 떨어뜨린다. 이것은 사용자 결정 (b)이다.

범위 밖이다.
- ragdoll 부위 기즈모는 [RagdollPartGizmo.md](../Physics/RagdollPartGizmo.md)에서 다룬다.
- 원본 skinned 메시의 스켈레탈 절단 규칙은 바꾸지 않는다.

## 2. 현재 구조 (조사·검토 요약, `f247a8aa`)

- 최종 A/B는 generated revision이다. `MeshComponent::Update`의 generated 경로가 Data와 identity section frame을 준비하므로 `CuttingSystem::Submit` 입력은 이미 있다.
- 절단 결과는 입력 component-local 위치를 그대로 쓴다. `ReadVertex`가 읽은 위치를 `WriteVertex`가 쓴다. `CutJob::BuildResults`의 center는 bounds용이며 중심 재배치는 없다.
- 후보와 작업 타입은 `CutCandidate`, `ActiveCut`, `CutJob`이다.
  - `SubmitPending`은 저장된 월드 평면과 현재 transform으로 `Submit`을 호출한다.
  - `UpdateOne`은 `BuildResults` 뒤 게시한다. A는 `CommitGeometryReplacement`로 Attachment를 유지한 채, B는 `CommitMeshComponent`로 Attachment 없이 게시한다.
- `Scene::PrepareMeshes`는 `MeshComponent::SetTransform`으로 부착 패치를 매 프레임 움직인다. 이때 EditGeneration이 증가하므로 `SubmitPending`과 `UpdateOne`의 세대 검사에서 취소된다.
- 부착 패치의 collision은 비어 있다. `SetInheritedCollision({}, ..., "node attached")`가 비워 둔다.
- collision 자료형은 `MiniCollision::AggregateGeom`이다.
  - `CookShape`의 Jolt hull은 CPU `IndexData`를 채우지 않는다.
  - `GetVolume`과 `PickObject`는 `IndexData`를 사용한다.
  - 기존 `GenerateKDop18(points, out)`은 점 집합에서 이 자료형을 만든다.
- `PhysicsSystem::SyncBodies`가 body를 등록하려면 다음을 모두 만족해야 한다. 시작 pose는 등록 시점 `GetWorldTransform`, 초기 속도는 0이다.
  - simulation 활성
  - 게시 가시성
  - Attachment 없음
  - 비어 있지 않은 collision
  - 유효 TRS
  - cook·용량 성공
- `Scene::PrepareMeshes`의 계산식은 다음과 같다.
  - `Wnode(t) = Wsource(t)·inverse(SourceFrame)·Nodes[meshCBV]·Nodes[NodeId]`
  - `Wpiece(t) = Wnode(t)·inverse(NodeWorldAtCut)·PieceWorldAtCut`
- 결과 측 규약은 고정이다. side 0은 평면의 양의 반공간, side 1은 음의 반공간이다.

## 3. 결정과 이유

D1. **부착 추종과 사용자 편집 세대 분리.**
- 부착 추종 월드 갱신은 `MeshComponent::SetAttachedWorld`로 따로 둔다. 이 함수는 EditGeneration을 올리지 않는다.
- 사용자 기즈모, Undo, 삭제는 지금처럼 세대를 올린다.
- 이유: 매 프레임 추종 때문에 모든 절단이 취소되는 문제를 막고, 실제 편집 감지는 유지한다.

D2. **요청 시점 좌표 고정.**
- 부착 입력의 `CutCandidate`는 후보 생성 시점의 다음 값을 저장한다.
  - local 평면과 유한 사각형
  - node side(D4)
  - Attachment snapshot(NodeId, NodeWorldAtCut, PieceWorldAtCut)
- `SubmitPending`은 부착 입력에 한해 저장한 local 평면으로 제출하고, 월드 불일치로 취소하지 않는다.
- `UpdateOne`은 게시 직전에 Attachment snapshot이 입력의 현재 Attachment와 같은지 확인한다. 다르면 취소하고 로그를 남긴다.
- 이유: C를 누른 순간 조각 위의 위치가 그대로 잘리고, 대기 중 이동에도 결과가 같다.

D3. **후보 허용.**
- `IsEligible`에서 Attachment 제외 조건을 지운다. skin PSO, 미게시, pose 미준비, 빈 section 제외는 유지한다.
- query box는 기존 `SyncColliders`가 `Scene::PrepareMeshes` 뒤 현재 bounds로 갱신한다.

D4. **node 쪽 판정.**
- 후보 생성 시점 `Wnode`의 translation과 월드 평면의 signed distance `d`로 정한다.
  - `d > 0`이면 side 0이 node 쪽이다.
  - `d < 0`이면 side 1이 node 쪽이다.
  - `|d| ≤ 1 mm`이면 그 조각의 후보를 거절하고 `ambiguous node side`를 기록한다.
- 이유: SP-7의 `ambiguous side within 1mm` 거절과 같은 허용 오차와 정책이다.
- 구현: `Scene::PrepareMeshes`의 Wnode 계산을 `Scene::TryGetAttachmentNodeWorld`로 꺼내 공유한다.

D5. **Attachment 승계.**
- 결과 local이 입력 local과 같으므로, node 쪽 결과는 입력의 Attachment snapshot을 그대로 받는다.
- `CommitGeometryReplacement` 쪽(side 0)이 node 반대편이면 그 쪽의 Attachment를 해제한다. 새 조각 쪽이 node 쪽이면 snapshot을 설정한다.

D6. **반대쪽 조각의 낙하.**
- Attachment를 해제하고, 게시 시점 최신 부착 월드를 `SetTransform(WorldIndependent)`로 고정한다.
- 시작 속도는 0이다. node 속도는 상속하지 않는다(첫 범위).
- collision을 새로 만든다.
  - `BuildResults`의 실제 section `vbOffset/vbSize/vbStride`로 결과 Data의 위치만 한 번 readback한다(`CutJob::ValidateDiagnostics`의 복사·동기화 방식).
  - `GenerateKDop18(points, out)`으로 `AggregateGeom`을 만든다. `IndexData`를 포함한다.
  - 점이 4개 미만이거나 평탄해서 부피가 0이면, bounds box에 양의 최소 두께 1 mm를 보장한다(`BuildShapeAndMass`는 양의 부피를 요구한다).
- body는 기존 `SyncBodies`가 다음 프레임에 등록한다. 등록에 실패하면 `Detached piece body failed: ...`를 기록한다.
- node 쪽 결과는 `node attached` 빈 collision을 유지한다.

D7. **반복과 동시 요청.**
- node 쪽 결과는 Attachment를 유지하므로 다시 자를 수 있다.
- 반대쪽 결과는 일반 Static 조각이 된다. 재절단과 Mesh 기즈모 대상이 된다.
- C는 계속 `RequestAll`과 `SkeletalPatchCopy::Request`를 둘 다 호출한다. 최종 A/B는 skin이 없어 스켈레탈 후보가 아니다.
- 다른 joint의 스켈레탈 절단이 같은 owner의 원본 mask를 바꾸는 경우는 회귀 항목으로 확인한다(§6 7). 기존 A/B와 재절단 결과가 남아 있어야 한다.

## 4. 인터페이스 골격 (신규 표시 외에는 기존 타입 확장)

```cpp
// MeshComponent(신규): 부착 추종 월드를 갱신하되 사용자 편집 세대는 올리지 않는다.
void SetAttachedWorld(const SceneTransform& world);
// Scene(신규): 부착 조각이 따라가는 node의 현재 월드 행렬을 계산한다.
bool TryGetAttachmentNodeWorld(const MeshComponent& piece, Math::Matrix4& nodeWorld) const;
// CutCandidate(확장): 부착 입력의 local 평면·사각형, node 쪽 side(-1 비부착), Attachment snapshot.
// CuttingSystem(신규): 결과 Data 위치를 읽어 독립 조각의 AggregateGeom을 만든다.
bool BuildDetachedCollision(CutJob& job, uint32_t side, MiniCollision::AggregateGeom& out);
```

## 5. 단계 (단계당 한 개념, 약 300줄 이하)

시간은 이번 세션 측정값이다. Codex 구현 6~13분, Codex 리뷰 3~6분, Debug/Release 빌드와 실행 확인은 단계마다 기록한다.

| 단계 | 내용 |
| --- | --- |
| AR-1 | D1·D2: `SetAttachedWorld`로 세대 분리, 부착 후보의 local 평면과 snapshot 저장, 게시 직전 재검증. 이 단계에서는 동작 변화가 없어야 한다(부착 조각은 아직 후보가 아님). |
| AR-2 | D3·D4·D5: 후보 허용, node 쪽 판정, Attachment 승계·해제. 반대쪽은 제자리에 남고 body는 없다. |
| AR-3 | D6: 위치 readback, `GenerateKDop18`, 최소 두께, `SyncBodies` 등록과 낙하, 실패 로그. |
| AR-4 | 진단 옵션 `-attached-recut-checks 1`(break 시연 뒤 부착 패치 자동 재절단, §6 판정), CodeMap/README/Scene.md 동기화. |

AR-3 구현(미커밋): `CuttingSystem::UpdateOne`은 `Poll` 완료·`BuildResults` 뒤 반대쪽에만
`BuildDetachedCollision`을 호출한다. 실제 section 범위의 float3 위치를 단일 readback에 모아
`Finish(true)` 후 읽고 `GenerateKDop18`로 CPU 인덱스까지 생성한다. 퇴화 시 최소 두께 1 mm인
bounds box로 대체하며, 예외 시 빈 충돌로 게시한다. `detached kdop` 이유를 보존해
`PhysicsSystem::SyncBodies`가 등록 실패를 200자 이내로 기록한다. 빌드·실행은 사용자 지시로 미수행이다.

각 단계가 끝나면 로컬에서 커밋한다. 회귀 항목은 `-skeletal-patch-break-checks 1`, `-skeletal-break-demo 1/2`, 기존 Static 절단 사례다.

## 6. 완료 조건

1. 부착 패치를 가로지르는 C가 패치를 두 조각으로 자른다. C를 누른 뒤 게시 전에 패치가 움직여도 C 시점 위치가 잘린다.
2. node 쪽 결과는 게시 후 60 프레임 동안 node를 따라간다. 렌더 위치는 같은 시점의 기존 패치와 1 mm 이내다.
3. 반대쪽 결과는 Attachment가 없고, 게시 시점 최신 부착 월드에서 시작한다. simulation이 running이면 body가 등록되어 60 프레임 뒤 아래로 1 cm 이상 이동한다. 등록 실패는 로그로 구별된다.
4. `-attached-recut-checks 1`이 Debug/Release 모두 PASS한다. D3D12 ERROR 0이다.
5. 반대쪽 조각은 Mesh 기즈모로 선택되고, 다시 Static 절단된다.
6. node가 평면에서 1 mm 이내이면 거절 로그가 남고 원래 조각이 그대로 남는다.
7. 다른 joint의 스켈레탈 절단 뒤에도 기존 A/B와 재절단 결과가 남는다. 기존 회귀(break checks, 데모 1/2, Static 절단)에 변화가 없다.
