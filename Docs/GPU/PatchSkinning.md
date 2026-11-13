상태: 진행 중 — PS-0 `42b3c9ab`, PS-1 `0bdb795c`, PS-2a `62e60db3`, PS-2b `cbe54cda`, PS-3 `cfffd37f`, PS-4a `cfd3e682`, PS-4b 완료(Debug/Release 빌드·회귀, 2026-11-13). PS-4c 이후 미착수. 기준 master `f2b4d2ec`

# 스켈레탈 패치 조각 스키닝 (NDC25 "프로시저럴 메시 스키닝")

## 목표

스켈레탈 절단 패치 조각 A/B를 node 강체 부착 대신 스키닝으로 움직여, 원본과 맞닿은 경계가 늘어나거나 벌어지지 않게 한다.

- 원본에서 복사한 정점: `V = Σ w_k · J_k(t) · V_bind`. 끊어진 쪽 joint 항만 절단 시점 행렬로 고정하고, 나머지는 현재 행렬을 쓴다.
- 절단으로 새로 생긴 정점(edge split 테두리, cap): 부착 node에 강체로 붙인다.
- 현재 문제: 강체 부착은 node 변화량만 곱하므로 다른 뼈 weight가 있는 경계가 원본과 어긋난다(SP7e2: CesiumMan 팔 게시 31.2 mm, 60프레임 뒤 277 mm).

슬라이드 해석: [../NDC/NDC25CutPatchSkinning.md](../NDC/NDC25CutPatchSkinning.md)(그 문서 5절의 미확정 세부를 이 문서가 결정한다).
관련 문서: [SkeletalPatchCopyCut.md](SkeletalPatchCopyCut.md), [SkeletalPatchBreak.md](SkeletalPatchBreak.md), [AttachedPatchRecut.md](AttachedPatchRecut.md), [../Physics/RagdollRigPreload.md](../Physics/RagdollRigPreload.md).

## 공간 정의

- `G(t)`: 모델 공간 node 행렬(`ModelInstance::GetNodeTransforms`). 래그돌 writeback도 `inverse(E)`로 같은 공간에 되돌린다.
- `J_k(t) = G[NodeId_k] · IBM_k`: 모델 공간 팔레트(`ModelInstance::FinalizePose`).
- `N(t) = inverse(SourceFrame) · G_mesh(t)`: `PreparePoseCS`가 스키닝 뒤 곱하는 NodeMatrix(`MeshComponent::Update`). 원본 PG 위치 = `N(t) · Σ w J b`, 렌더 = source world `W(t)` · PG.
- 결과 Data 위치 = 추출한 source PG 위치 = t_cut의 source component-local.

## 결정과 이유

D1. 고정 공간 = 부착 node 상대.
- 고정 joint k: `J_k'(t) = D(t) · J_k(t_cut)`, `D(t) = G_att(t) · inverse(G_att(t_cut))`.
- 조각은 source와 같은 `W(t)`, `N(t)`로 그린다. 부착 node의 출력 공간 행렬이 `N·G_att`이므로 `N(t)·D(t)·J_k(t_cut)`는 그 node를 정확히 따라간다.
- normal 팔레트는 최종 position 항의 inverse-transpose로 만든다(원본 `FinalizePose`와 같은 규칙).
- 조건: 패치가 걸친 모든 section의 `N`이 같아야 한다. 다르면 스키닝하지 않고 현행 강체 부착으로 게시한다(진단 로그).
- 이유: 컴포넌트 공간에 고정하면 부모 뼈가 움직일 때 고정 부분이 제자리에 남아 늘어난다.

D2. 고정 joint 집합과 부착 node.
- A(남는 쪽): 부착 node = P의 직접 부모(현행 A attachment). 고정 = P와 그 skeleton 자손 전체.
- B(떨어지는 쪽): 부착 node = P. 고정 = P 자손이 아닌 모든 joint(대칭).
- D5(break 없음): 부착 node = N, 고정 집합 없음. 결과는 원본 스키닝과 같다.
- 경계 정확성: 원본에 남은 삼각형은 P weight가 0이다(`PatchExtractCS::Selected`). 그러나 P 자손(A 쪽)이나 비자손(B 쪽) weight는 0이라는 보장이 없다. 그런 경계 정점은 고정 항만큼 원본과 달라질 수 있다.
- 따라서 경계 오차는 증명이 아니라 SP7e2 측정으로 확인한다. PS-1 진단에서 경계 정점의 고정 항 weight 합 최댓값을 로그로 남긴다.

D3. t_cut = `SkeletalPatchCopy::Request` 프레임.
- 같은 요청의 `SkeletalPatchJob::Submit`이 그 프레임 PG를 추출하므로 패치 위치와 팔레트 snapshot이 일치한다.
- snapshot에 `N(t_cut)`과 source frame도 저장하고, 추출한 PG의 프레임과 같은지 검증한다.

D4. 정점 종류별 skin 속성.
- 분류는 cutter 출처로 한다: 복사 `Origin.z == Origin.w`, edge split = 두 endpoint + `Parameter`, cap `Origin.x == 0xffffffff`(`CutCommon.hlsli`, `FinalizeCutCS.hlsl`).
- 복사 정점: patch 정점 `v` → `TriangleSources[v/3]`, corner `v%3` → `PatchExtractCS::Index`와 같은 원본 IB/baseVertex → 원본 VB의 bind position/normal/tangent, 전역 joint(`startJoint` 더함), weight. source revision을 보존한다.
- edge split·cap 정점: 위치 = `inverse(N(t_cut)) · Data 위치`(모델 공간), joint = 조각 팔레트 끝의 추가 슬롯 `K`, weight = 1. `K = D(t)`.
- 이유: cap은 별도 복사 정점이라 테두리와 다르게 움직이면 틈이 생긴다. 둘 다 `K`로 움직이면 단면이 항상 닫힌다. 복사 정점은 원본 값 복사라 보간·weight 병합이 필요 없다.
- cutter·`CutCommon.hlsli` 정점 형식은 바꾸지 않는다.

D5. 구현 = 조각별 팔레트 + `PreparePoseCS` 재사용.
- 조각 팔레트 = owner 현재 팔레트 전체 복사 → 고정 joint를 `J_k'`로 덮어씀 → 끝에 `K` 추가. CPU에서 매 프레임 만든다. 전역 joint를 쓰므로 `startJoint = 0`, 범위 = joint 수 + 1(u16 한계 확인).
- 조각 skin VB는 원본 section의 레이아웃 flags(tangent, UV0, UV1)를 따른 stride + skin 16 B로 만든다. 섹션 flags가 다르면 합집합 레이아웃을 쓴다.
- 기존 generated PG(28 B, skin 거절, 1회 decode)와 모델 PG(`Update`)는 그대로 쓸 수 없다. `PosedGeometry`에 skinned generated 전용 생성자와 매 프레임 갱신 함수를 추가한다.
- 이유: 새 스키닝 셰이더가 필요 없고 원본 경로와 수식이 같다.

D6. 재절단·분리 = 후보 시점 bake 후 영구 강체.
- 스키닝 조각은 AR 후보 판정(`PhysicsSystem::IsEligible`)에 static 메타데이터로 노출하고, skin 입력은 cutter에 넘기지 않는다.
- 후보로 선택된 프레임에 조각 PG를 28 B Data로 굽고, 같은 프레임의 local 평면·node side와 함께 snapshot한다. 조각은 그때 현행 강체 부착(NodeWorldAtCut = 그 프레임)으로 영구 전환한다.
- 재절단이 실패하거나 무분할이어도 강체 상태를 유지한다(원복 없음). 이전 skin 자원은 다음 프레임 이후 해제한다.
- 재절단 결과 조각의 스키닝 승계는 후속 과제.

D7. bounds.
- 조각 bounds는 owner bounds를 쓰지 않는다. owner의 `MeshComponent::Update`와 같은 방법으로, 조각 팔레트와 조각 bind bounds로 매 프레임 계산한다.
- collision은 현행처럼 "node attached" 빈 형상이다.

D8. SP7e2 판정.
- 경계 정점 0개는 N/A로 기록하고 FAIL·PASS 어느 쪽에도 세지 않는다.
- 짝은 위치(`pair.Local`)가 아니라 조각 정점 식별자(section, vertex)로 저장한다. 같은 프레임의 원본 PG와 조각 PG를 읽어 비교한다.
- publish·after60 모두 >1 mm면 FAIL. NaN·짝 누락도 FAIL.

## 인터페이스 골격

```cpp
// t_cut 팔레트와 고정 규칙 (SkeletalPatchCopy::Request가 채움)
struct PatchSkinSnapshot
{
    uint32_t AttachNode;                  // A: P 부모, B: P, D5: N
    Math::Matrix4 AttachAtCut;            // G_att(t_cut)
    Math::Matrix4 MeshNodeAtCut;          // N(t_cut)
    std::vector<Joint> JointsAtCut;       // owner 팔레트 전체 복사
    std::vector<uint8_t> Frozen;          // joint별 고정 여부
};

// 매 프레임 조각 팔레트: 복사 → 고정 덮어쓰기 → 슬롯 K 추가
void BuildPatchPalette(const PatchSkinSnapshot& snap, const Joint* current,
    const Math::Matrix4* nodes, std::vector<Joint>& out);
```

- GPU `PatchSkinBuildCS`: cutter 결과 정점마다 D4 규칙으로 skin VB 정점을 쓴다.
- `PosedGeometry`: skinned generated 생성자 + 매 프레임 갱신(조각 팔레트 SRV, `N(t)`).
- `MeshComponent`/`Scene::PrepareMeshes`: 스키닝 조각은 source `W`·`N`·조각 팔레트로 갱신, `SetAttachedWorld` 생략.

## 단계 (각 한 개념·약 300줄; 새 Codex 구현 → 다른 Codex 리뷰 → Debug/Release 빌드 → 회귀 → 로컬 커밋)

| 단계 | 내용 | 완료 확인 |
| --- | --- | --- |
| PS-0 | SP7e2 경계 0개 N/A 분리, after60 FAIL 반영, NaN·누락 FAIL | 완료: Xbot 전완 publish·after60 N/A, 나머지 4건 FAIL 집계(강체 부착 기준선: 허벅지 1.04/2.56 mm, CesiumMan 36.4/249 mm), 다른 판정 불변 |
| PS-1 | `PatchSkinSnapshot`, 고정 집합, `BuildPatchPalette`(CPU), N 공통성 검사 | 완료: 세 case A/B 팔레트 오차 ≤3.6e-7 PASS, skinningAllowed=1. 경계 고정 weight는 A/B 경계를 나누지 않아 최대 1로 나옴 → PS-5에서 조각별로 재측정 |
| PS-2a | 출처 복원 + `PatchSkinBuildCS` skin VB 생성 셰이더·동기 생성 API(`BuildPatchSkinVertices`) | 완료: 빌드·리뷰(방향은 transpose(N)으로 되돌림), 호출 연결 전이라 실행 검증 없음 |
| PS-2b | A/B·D5 결과 경로에서 skin VB 생성·보관, PS2 진단 | 완료: A/B 세 case·D5 모두 PASS(최대 0.00024 mm, copied/split/cap 분류 확인), 생성 뒤 입력 상태 GENERIC_READ 복원 |
| PS-3 | `PosedGeometry` skinned generated 생성·매 프레임 갱신 API | 완료: GPU 갱신 결과(RAW40) = Data, A/B 세 case 최대 0.00025 mm PASS, 기존 model·generated PG 동작 불변 |
| PS-4a | A/B/D5 게시 전환(W·N·조각 팔레트), 매 프레임 skinned PG 갱신 | 완료: 회귀 6종 유지. 한계(PS-4b·PS-6에서 해결): 조회용 world·bounds가 게시 시점 값, source Frozen/revision 변경 미검사, AR 재절단은 t_cut Data를 자름 |
| PS-4b | 조회용 world = W(t), source가 실제 쓴 팔레트 사용·revision/binding 변경 시 스키닝 해제, D7 조각 bind bounds·매 프레임 bounds | 완료: 회귀 6종 유지. 알려진 한계: 전단 있는 world 폴백은 기존 강체 경로와 같은 제약, weight 합 0 정점은 원본 bounds와 같은 동작 |
| PS-4c | 게시·60프레임 뒤 PS4 bounds·rim/cap 진단, 조각 팔레트 매 프레임 할당 제거 | 미착수: 조각 PG 전체 정점이 bounds 안, 테두리–cap 대응 정점 거리 ≤0.01 mm 확인 예정 |
| PS-5 | SP7e2 정점 식별자 짝·같은 프레임 PG 비교 | Xbot 허벅지·CesiumMan 팔 publish·after60 PASS; SP7e2 FAIL을 진단 종료 코드에 반영(PS-0 리뷰 후속) |
| PS-6 | AR 후보 static 노출, 후보 시점 bake·영구 강체 전환, 스키닝 해제 폴백 시 진행 중 AR 무효화 | AR4 다섯 PASS, `Rig bone cut checks PASS` |

시간 예상: 단계마다 구현 Codex 10~20분, 리뷰 Codex 2~5분, Debug+Release 빌드 2~4분, 회귀 6종 5~10분. 300줄을 넘기면 단계를 나눈다.

## 완료 조건

1. `-skeletal-patch-break-checks 1`: 경계가 있는 case(Xbot 허벅지, CesiumMan 팔)는 SP7e2 publish·after60 모두 ≤1 mm PASS. 경계 0개 case는 N/A로 기록되고 FAIL에 세지 않는다.
2. 기존 회귀 유지: `-skeletal-patch-offset-copy 1 -skeletal-patch-checks 1` 20행 PASS, SP7b/SP7e PASS, BC-12a·BC-12b PASS, AR4 다섯 PASS, `Rig bone cut checks PASS`. Debug/Release 둘 다.
3. 테두리·cap 정점은 `K` 하나로 움직여 단면이 닫혀 있다(PS-4 진단 + 시각 확인).
4. 조각 PG 전체 정점이 조각 bounds 안에 있다(PS-4 진단).
5. cutter 정점 형식(`CutCommon.hlsli::Vertex`)과 Static 절단 경로는 변경 없음.
6. 조각 스키닝 추가 비용(조각 PreparePoseCS) ms를 Release에서 한 번 측정해 이 문서에 기록.

## 범위 밖

- 재절단 결과 조각의 스키닝 승계, 다중 절단 snapshot 수명 관리.
- 스키닝 조각의 정밀 picking(현행 빈 형상 유지).
