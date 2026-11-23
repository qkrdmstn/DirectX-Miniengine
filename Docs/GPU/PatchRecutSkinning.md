상태: 완료 — RS-1(부모 skin·N·D 보관, `-patch-recut-skin`)·RS-2a(PatchSkinRecutCS 생성, readback 약 1.1~1.6 ms)·RS-2b(독립 스키닝·kFrame 진단 PASS)·RS-3(node 쪽 skin 승계, 경계 게시·60프레임 0 local)·RS-4(FollowError를 K 정점 추종으로 교체: AR4 #2 마지막 0.0011 mm·최대 local 1.37e-08, RigBoneCut 최대 0.000002 m·local 1.15e-08; `-attached-recut-checks 2` 연속 재절단 skinned=1·경계 게시·60프레임 PASS) 완료, RS-5(AR4 장면 옵션 0/1 캡처: 틈 없음, 육안 차이 작음, 로그로 옵션 0 11.2 mm 어긋남 확인) 완료. Release 회귀 6종 옵션 1·0 모두 PASS(2026-11-23). 기준 master `a4b9ea23`, 브랜치 `patch-recut-keep-skin`

# 스키닝 패치 재절단 시 스키닝 유지

## 목표

부착된 스키닝 패치 조각을 다시 자를 때, 몸에 남는 쪽 조각이 계속 스키닝되어 원본 메시와 함께 변형되게 한다.

- 현재(PS-6): 후보 선정 때 `BakePatchSkin`으로 현재 포즈를 정적 정점으로 굽고 스키닝을 영구 해제한 뒤 정적 경로로 자른다. node 쪽 결과는 부착 node 하나를 강체로 따라간다.
- 문제: 원본 몸통 경계 정점은 여러 joint weight를 섞어 변형되는데, 굳은 조각은 node만 따라간다. 몸이 움직이면 경계에 틈이 생길 수 있다.
- 근거: 부착 패치 재절단은 본을 끊지 않는다(`AttachedPatchRecut.md`). 그래서 새로 고정할 joint가 없고, `PatchSkinning.md` D5(break 없음)와 같은 경우로 본다.

조사 메모: Sonnet 조사(2026-11-23, 코드 읽기만, 실행 미확인). 요점은 아래 결정에 반영했다.

## 결정

D1. 범위
- node 쪽 결과(부착을 승계하는 조각)만 스키닝을 유지한다.
- 반대쪽 독립 조각은 지금처럼 요청 시점 포즈로 구운 정적 조각으로 둔다. 원본과 맞닿은 경계가 없으므로 정적이어도 된다.

D2. 팔레트 승계
- node 쪽 결과는 부모 조각의 `PatchSkinSnapshot`을 값 그대로 복사한다(AttachNode, MeshNode, SourceRevision, SourceFrame, SourceFrozen, AttachAtCut, MeshNodeAtCut, JointsAtCut, Frozen).
- 새 고정 joint는 없고, 슬롯 K 번호(`JointsAtCut.size()`)도 같다.
- 이유: 재절단은 본을 끊지 않으므로 부모 조각의 변형 규칙이 그대로 맞다.

D3. 결과 정점의 skin 레코드
- 복사 정점(Provenance `z == w`): 부모 skin VB의 레코드(Stride 바이트, ext 포함)를 통째로 복사한다. 원래 joint·weight·내부 교점 레코드가 보존되므로, 원본과 맞닿은 경계 정점은 부모와 똑같이 변형된다.
- split 정점과 cap 정점: 모두 이번 절단 평면 위의 새 절단면이다. 슬롯 K에 weight 1로 붙인다.
  - 위치 = `inv(N(t_req)·D(t_req))·p`. p는 요청 시점에 구운 위치, N은 mesh node 프레임, D는 부착 node 변화량(직전 팔레트의 K 슬롯)이다.
  - 방향은 같은 행렬의 cofactor로 되돌린다(첫 절단의 RotateFrame과 같다).
  - uv0/uv1과 법선·탄젠트(w 유지)도 결과 Data에서 읽고, 방향은 같은 cofactor로 되돌린다.
- 복사 판정은 `Origin.x != 0xffffffff && Origin.z == Origin.w`다. cap은 Origin.x가 0xffffffff다.
- 끝점 보간(StoreEndpoint)은 쓰지 않는다. split은 전부 rim이라 끝점 보간이 필요 없고, 두 겹 보간도 피한다. 복사 정점이 이미 내부 교점 레코드(internal=1)이면 통째 복사로 보존한다(끝점·t·joint가 모두 부모 bind 공간이고 snapshot이 같으므로 유효).
- 부모 skin VB 주소 = `PatchSkin->Sections[SourceSection].FirstVertex + Origin.z`. 리뷰에서 확인한 근거: SourceSection은 입력 revision의 부분집합 section 인덱스이고 PatchSkin->Sections도 같은 인덱스를 쓴다. bake는 정점 번호와 section 순서를 1:1로 보존한다. RS-2a 진단으로 실행 검증한다.
- 자식 skin의 section FirstVertex/VertexCount와 BindBounds는 새로 계산한다(K 정점 때문에 부모 값 불가). BindBounds는 `BuildPatchSkinVertices`처럼 readback으로 계산하고 비용을 로그한다.
- 자식 publication의 RimCapPairs는 비운다. 부모 Data 번호 기준이라 무효이며, 이를 읽는 진단은 재절단 조각을 건너뛴다.

D4. 굽기는 현행 유지, 게시 때 스키닝 재활성화 (리뷰 반영으로 변경)
- 후보 선정 때 `BakePatchSkin`은 지금(PS-6)처럼 실행한다. 절단 도중 조각은 정적이고, cutter 입력·bounds·attachment snapshot·동일성 검사는 모두 기존 그대로다.
- 다만 bake 직전에 부모 skin(`PatchSkin` shared_ptr)과 요청 시점의 `N·D`를 후보와 ActiveCut에 보관한다.
  - N은 PG를 만든 같은 Update의 값이어야 하므로, `MeshComponent::Update`가 마지막 N(sourceToComponent 포함)을 멤버에 저장한다.
  - D는 `m_PatchPalette`의 K 슬롯이다.
- 게시 때 node 쪽 결과에 D3의 자식 skin을 붙여 스키닝을 다시 켠다. 절단 중 몇 프레임은 강체였다가 게시 때 스키닝으로 돌아오며, 그 사이 위치 차이는 게시 시 경계 오차로 측정한다.
- 절단이 실패하거나 무분할이면 PS-6처럼 정적으로 남는다(완료 조건 범위 밖).
- 버린 대안: bake 비파괴화(원래 조각을 게시까지 스키닝 유지). 리뷰 결과, attachment snapshot memcmp, 입력 revision 동일성 검사 5곳, live section bounds, ValidateQueries 식이 모두 바뀌어야 한다. 위험과 변경량이 커서 이번 범위에서 제외했다.
- 반대쪽 결과는 지금처럼 구운 Data와 기존 게시 월드식을 쓴다. 충돌 상태도 바뀌지 않는다(node 쪽 빈 충돌, 반대쪽 `BuildDetachedCollision`).

D5. 연속 재절단
- 스키닝을 승계한 조각을 다시 자르면 같은 규칙(D2~D4)을 적용한다. 부모 skin VB는 승계된 것이다. 고정 집합이 쌓이지 않으므로 횟수 제한은 두지 않는다.

D6. 비교 옵션
- `-patch-recut-skin 0`이면 기존 PS-6 동작(굽고 정적)으로 돌아간다. 기본값은 1이다. 시각 비교와 회귀 대조용이다.

D7. 진단 판정 갱신
- AR4 #2와 RigBoneCut의 FollowError는 `GetWorldTransform`을 node 기대값과 비교한다. 스키닝 조각은 이 값이 source W(t)이므로 그대로는 맞지 않는다.
- 스키닝 조각은 이 판정 대신 다음으로 확인한다.
  - (a) K 정점의 PG 위치와, node 기대 월드로 옮긴 구운 위치의 차이
  - (b) 원본과 맞닿은 경계 정점의 PG와 원본 PG의 차이(SkeletalBoundaryChecks 방식)
- `AttachedRecutChecks::Plane`은 스키닝 조각이면 Data 대신 PG를 읽는다.

## 인터페이스 골격

```cpp
// PhysicsSystem: 후보에 스키닝 승계 정보를 싣는다.
struct CutCandidate {
    ...
    std::shared_ptr<const PatchSkinPublication> ParentSkin; // null이면 기존 정적 경로
    Math::Matrix4 RecutKFrame;                              // N(t_req)·D(t_req)
};

// PatchSkinning: 재절단 결과의 skin VB를 만든다(PatchSkinRecutCS).
bool BuildRecutPatchSkinVertices(const PatchSkinPublication& parent,
    const GeometryBuffers& cutResult, const Math::Matrix4& kFrame,
    PatchSkinVertices& out);

// MeshComponent: 준비 단계에서 skin을 붙인 교체를 noexcept로 게시한다.
struct PreparedGeometryReplacement { ...; std::shared_ptr<PatchSkinPublication> PatchSkin; };
```

- `ActivatePatchSkin`은 멤버를 직접 바꾸고 예외를 던질 수 있어 준비 단계에 쓸 수 없다. section 매핑, skinned PosedGeometry, 팔레트 생성은 (geometry, skin)을 받는 정적 helper로 분리해 준비 단계에서 수행한다. Commit은 move만 하고, 이전 PatchSkin은 `m_RetiredPatchSkin`으로 넘긴다.
- 후보가 보관하는 부모 skin과 구운 revision은 후보가 소비될 때까지 GPU 버퍼를 잡는다. RequestAll의 메모리 예산 계산에 포함되는지 RS-1에서 확인한다.

## 단계

| 단계 | 내용 | 완료 확인 |
| --- | --- | --- |
| RS-1 | D4 보관: Update의 마지막 N 저장, bake 직전 부모 skin·`N·D`를 후보와 ActiveCut에 보관, D6 옵션. 동작 변화 없음 | 옵션 0/1 모두 AR4 다섯 PASS, 보관 값 로그, 메모리 예산 포함 여부 확인 |
| RS-2a | D3 `PatchSkinRecutCS`·`BuildRecutPatchSkinVertices`(section 재계산, BindBounds readback), 호출은 UpdateOne 게시 전 생성만 | 빌드·회귀 PASS, 생성 로그(정점 수, 복사/K 수, readback 비용) |
| RS-2b | RS-2a 결과 진단 | 복사 레코드 부모와 바이트 동일, K 정점 왕복 위치와 복사 정점 독립 스키닝 위치가 구운 값과 local ≤ 1e-6, 법선 ≤ 0.5도, kFrame = N·palette[K]. 완료: mismatch 0/2246, K 1.86e-9, 복사 스키닝 1.62e-7 local(월드 0.029 mm, float 잡음), kframe diff 0 |
| RS-3 | D1·D2 node 쪽 결과에 skin 승계·활성화(helper 분리, A는 PreparedGeometryReplacement, B는 준비 단계), 이전 skin 지연 해제, D7 (a)(b) 측정과 `AttachedRecutChecks::Plane`의 PG 읽기 | node 쪽 결과 HasSkinnedPatch, 경계 오차 게시·60프레임 뒤 local ≤ 1e-6(월드 mm는 참고) |
| RS-4 | D7 판정 교체(AR4 #2·RigBoneCut FollowError), D5 연속 재절단 확인, 문서 동기화(상태 줄·README·CodeMap·PatchSkinning PS-6 주석) | 완료: 스키닝 조각은 `SkinnedFollowError`(K 정점 PG 월드 vs node 기대 월드의 구운 위치, 1 mm)로 판정. 연속 재절단(`-attached-recut-checks 2`) 두 번째 node 쪽 skinned=1, RS2b 4줄·경계 게시·60프레임 0 local PASS |
| RS-5 | CesiumMan 시각 확인: 부착 조각 재절단을 옵션 0/1로 캡처 비교 | 몸에 남는 조각이 원본과 함께 변형, 틈 없음. 결과: AR4 장면에 `-capture`·`-capture-count`·`-capture-step` 허용(첫 재절단 게시부터 `-attached-recut-N`). 옵션 0/1 게시·+30·+60 캡처에서 틈·늘어남 없음, 픽셀 차이 0.05~0.07%로 육안 차이는 작다(node 이동 17.4 mm뿐). 로그: 옵션 0은 조각이 7.0 mm만 따라가 11.2 mm 어긋남, 옵션 1은 skin 승계·경계 0 local. 큰 동작 장면의 육안 확인은 남음 |

각 단계는 약 300줄 이하로, 새 구현 실행 → 다른 실행 리뷰 → Release 빌드 → 회귀 → 로컬 커밋 순서로 진행한다.

## 완료 조건

기준 단위(RS-2b에서 변경): AR4 장면은 모델을 `2 / BoundingSphere.radius`(CesiumMan 약 177배)로 키운다. 그래서 float 반올림(local 1e-7대)이 월드 0.03 mm로 보인다. 위치 판정은 local 1e-6 이하로 하고 월드 mm는 참고로 남긴다.

1. 기본값(옵션 1)에서 부착 스키닝 패치 재절단의 node 쪽 결과가 스키닝을 유지하고, 원본과의 경계 오차가 게시 프레임과 60프레임 뒤 모두 local 1e-6 이하다.
2. 반대쪽 결과는 기존처럼 독립 정적 조각이고, AR4의 나머지 판정이 PASS다.
3. 게시 프레임에 node 쪽 결과가 튀지 않는다(게시 경계 오차가 1과 같은 기준).
4. 연속 2회 재절단 뒤에도 1이 성립한다.
5. `-patch-recut-skin 0`은 PS-6 결과를 재현한다(AR4 다섯 PASS).
6. Release 회귀 6종 PASS, D3D12 ERROR 0.
7. CesiumMan 캡처로 옵션 0/1 차이를 확인한다.

## 범위 밖

- 재절단이 본을 끊는 경우(현재 경로에 없음). 생기면 고정 집합 중첩 설계가 따로 필요하다.
- 반대쪽 독립 조각의 스키닝.
- 절단 실패·무분할 때 스키닝 복원(D4의 버린 대안).
- 첫 절단 쪽 변경(JointCutRedesign JC-1~4 범위).
