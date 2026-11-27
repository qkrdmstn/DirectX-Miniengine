상태: 진행 중 — ST-1 진단(경계 거리 제외) 구현·Release 재현 확인, 기준 master `d00194ea` (2026-11-27)

# 연속 절단 뒤 기존 패치의 고정 joint 갱신

## 목표

같은 스켈레톤을 두 번 이상 자를 때, 앞 절단이 만든 패치가 뒤 절단으로 분리된 본을 따라 늘어나지 않게 한다.

## 현상과 원인 (확인됨)

- 재현: Xbot 데모 1, `-skeletal-break-demo-bone mixamorig:RightUpLeg -cut-perf-second-bone mixamorig:LeftUpLeg -cut-perf-log <경로>`. 2차 절단 뒤 왼쪽 허벅지 그루터기와 떨어진 왼쪽 다리 사이에 가는 삼각형이 늘어난다. `-island-gpu` 0/1 모두 같다.
- 진단(`SKINSTALE`, 게시 때 같은 owner의 모든 활성 패치 검사):
  - 1차 게시: 몸통 패치 `patch=2` 0건.
  - 2차 게시: 같은 `patch=2`에서 188개 정점이 분리된 LeftUpLeg(node 60)에 live 가중치를 가진다. 2차 몸통 패치 `patch=6`은 0건.
  - 대조(2차 절단 옵션 없이 실행): `patch=2` 0건.
- 원인:
  - `SkeletalPatchCopy.cpp:Request`가 생성 때 `JointsAtCut`·`Frozen`을, `FillAnimFollow`가 `AnimOffset`을 한 번 채운다. 고정 범위는 `SegmentJointSelection.h:BuildCutBodyGroups`의 이번 `cutRoots`뿐이다.
  - `Update`·`ConsumeMultiBreak`에는 이미 게시된 패치를 갱신하는 경로가 없다.
  - `PatchSkinning.cpp:BuildPatchPalette`는 Frozen이 아닌 joint에 현재(물리 포함) 팔레트를 쓰므로, 그 정점이 래그돌 다리를 따라간다.
  - RS 재절단 자식은 생성 대신 부모 snapshot을 복사해 시작한다(`BuildRecutPatchSkinVertices`). 따라서 같은 누락을 물려받는다.
- 중복 복사는 원인이 아니다. 2차 추출은 원본 제거 마스크(`State & 0x80000000`)로 1차 삼각형을 제외한다(`PatchExtractCS` Mode 0).
- "새 패치는 맞다"는 이 재현 장면의 결과일 뿐이다. 역순 절단 등에서는 새 패치도 이전 분리 subtree를 live로 참조할 수 있다(D5).

## 결정

D1. owner별 **분리 이력**을 둔다. 성공한 break 게시마다 항목 하나를 추가한다.
- 항목: 분리 root NodeId, 이 절단 요청 시점 t_s의 물리 포함 source 팔레트 `J_s`, 같은 시점 node 전역 행렬(AF용).
- t_s는 이번 요청이 새 A 패치를 만들 때 쓴 시점과 같다. 그래서 새 A 패치와 기존 패치가 같은 joint에 같은 시점 값을 쓴다.
- 거절·취소된 요청은 이력도 기존 패치도 바꾸지 않는다.

D2. **모든 게시 경계**에서 같은 규칙을 적용한다: 그 owner의 활성 skinned 패치 각각에 대해, 이력의 분리 subtree에 속하고 아직 Frozen이 아닌 palette slot을 고정한다.
- 적용 시점: 새 절단 게시(`Update`), 다중 절단 게시(`ConsumeMultiBreak`), RS 자식 게시. 새로 게시되는 A 패치 자신도 대상이다. 이 규칙이 D5의 역순 절단을 덮는다.
- 판정: slot k의 NodeId는 `model.m_JointIndices[k]`로 얻는다. 같은 NodeId의 모든 slot을 갱신한다. 행렬은 slot별로 따로 계산한다(section별 inverse bind가 다를 수 있다).
- 제외: 패치 snapshot의 `AttachNode`가 그 이력 subtree 안이면, 그 subtree를 자기 몸으로 따르는 쪽 조각이므로 건너뛴다. 이미 Frozen인 slot과 마지막 강체 슬롯 K는 바꾸지 않는다.
- 비활성(Stale로 skin이 꺼진) 패치와 `SourceFrozen` 패치는 건너뛰고, 건수만 진단에 남긴다.

D3. 고정 값 (기존 패치 기준으로 다시 계산한다. 새 A 패치 값을 복사하지 않는다).
- `D(t) = G_attach(t) · inverse(AttachAtCut)`. 여기서 `G_attach`와 `AttachAtCut`은 기존 패치의 것이다.
- `JointsAtCut[k] = inverse(D(t_s)) · J_s[k]`.
- AF가 켜진 패치면 `AnimOffset[k] = inverse(C(t_s) · A_k(t_s)) · J_s[k]`. 이 식은 신규 고정 slot에만 적용하고, 기존 `AnimFollow` 자격과 offset은 보존한다. `FillAnimFollow`는 다시 부르지 않는다.
- `LengthBlendRatio[k]`: 이번 이력 root의 표(root=r_P, 자손=0, 나머지=1)를 신규 고정 slot에만 쓰고, 기존 값은 보존한다.
- t_s와 게시 사이(약 6프레임)에 생기는 전환 점프는 허용한다. 새 A 패치도 같은 t_s 값으로 시작하므로 두 패치는 서로 맞는다.

D4. 갱신은 활성 publication의 snapshot 배열만 바꾼다. 다음 `MeshComponent::Update`가 그 snapshot으로 팔레트를 만들어 반영한다. geometry·skin VB는 다시 만들지 않는다.
- 진행 중 RS가 부모 publication을 공유 참조로 들고 있으므로(`KeepParentSkin`), snapshot은 **복사 후 교체**한다. RS는 요청 당시 snapshot과 K를 그대로 쓰고, 게시 때 D2 규칙으로 최신 이력을 적용받는다.

D5. 새 패치의 `cutRoots`에 이전 root를 합치지 않는다. body 분류와 중첩 root 거절이 바뀌기 때문이다. 대신 D2가 새 패치에도 이력을 적용한다.

D6. 절단 Undo는 현재 없다(History는 TRS만 복원). 범위 밖이다. 나중에 지원하면 기존 패치의 변경 전 snapshot도 transaction에 넣어야 한다.

D7. 진단 `SKINSTALE`은 남긴다(quiet=0일 때만 CPU 사본 보관).
- 자기 몸을 따르는 쪽 조각(D2 제외 규칙)은 세지 않는다.
- RS 자식도 CPU 사본을 남긴다. 사본이 없으면 `cpu=unavailable`로 찍고 완료 판정에서 실패로 본다.
- 현재 위치·법선 팔레트의 유한성을 ST-1에서 검사한다. 전환 프레임에 갱신한 slot과 두 패치가 공유하는 원본 정점의 위치 차(경계 오차) 진단은 ST-2에서 추가한다.

## 인터페이스 골격 (이름은 구현 때 확정)

```cpp
// SkeletalPatchCopy: owner별 분리 이력 항목. t_s의 팔레트와 node 전역 행렬을 보관한다.
struct DetachRecord { uint32_t RootNode; std::vector<Joint> Palette; std::vector<Math::Matrix4> Nodes; uint64_t Generation; };

// SkeletalPatchCopy: 게시 경계에서 owner의 활성 패치 전부에 이력을 적용한다(D2·D3).
void ApplyDetachHistory(Model owner, const std::vector<DetachRecord>& history);

// MeshComponent: snapshot을 복사해 slot 고정을 추가한 새 publication으로 교체한다(D4).
void ReplacePatchSnapshot(const PatchSkinSnapshot& updated);
```

## 단계 (리뷰는 단계당 약 2~5분)

| 단계 | 내용 | 규모 |
| --- | --- | --- |
| ST-1 | 진단 구현·Release 재현 확인(미커밋): `LogStaleSkin` 자기 몸 subtree 전체 제외·skip 건수·현재 위치/법선 팔레트 유한성·RS CPU 사본·stride 및 section 범위의 레이아웃 검증. 경계 거리 제외 | 기존 진단 포함 약 200줄 |
| ST-2 | 분리 이력 기록(D1)과 단일 절단 게시에서 기존 패치에 적용(D2~D4), 경계 위치 차 진단(D7) | 약 250줄 |
| ST-3 | 다중 절단·RS 게시와 새 패치 자신에 적용(D2·D5) | 약 200줄 |
| ST-4 | 검증, 문서 상태 줄·README·CodeMap 갱신 | 문서 |

## 완료 조건

1. 재현 인수에서 2차 게시 `SKINSTALE`의 모든 몸통 패치가 0건이고 `cpu=unavailable`이 없다.
2. 역순(LeftUpLeg→RightUpLeg)과 3회 절단(두 허벅지 뒤 위팔)에서도 1과 같다.
3. AF 0/1, LB 0/1 각각에서 1이 성립하고 갱신 slot 팔레트가 유한하다. 경계 오차는 Release에서 1 mm 이하다.
4. RS 재절단 전후, 다른 owner(CesiumMan)의 패치는 바뀌지 않는다. 거절·취소된 요청 뒤 기존 패치 snapshot이 그대로다.
5. 1회 절단 장면(Xbot·CesiumMan 허벅지·위팔·몸통)의 MI 로그가 기준(`%TEMP%\g1a-equivalence-20261126`)과 같다. 연속 재절단 회귀(RS 계열, AR-4)가 기존처럼 PASS한다.
6. 재현 장면을 육안으로 봤을 때 늘어난 삼각형이 없다(사용자 확인).
7. Release 빌드 성공, D3D12 ERROR 0.
