상태: 설계안 — 결정 확정·Sonnet 리뷰 반영(2026-11-23), 구현 전. 기준 master `06edbd76`, 브랜치 `patch-anim-follow`

# 몸통 쪽 패치가 끊은 본의 애니메이션을 따르기

## 목표

몸통에 남는 A 조각이 끊은 본 P(와 자손)의 키네마틱 애니메이션을 따르게 한다. 절단면(rim)과 뚜껑(cap)도 그루터기와 함께 움직이게 한다.

- 현재(`PatchSkinning.md` D1·D2): A는 P와 자손의 기여를 부착 node 기준 절단 시점 값으로 고정한다(`D·JointsAtCut`). 그래서 그루터기가 절단 순간 각도로 굳는다.
- 원인: 공유 스켈레톤의 P는 래그돌 물리가 덮어쓴다. 그래서 A는 현재 행렬을 쓸 수 없다.
- 근거: 애니메이션 포즈는 절단 뒤에도 프레임마다 `ModelInstance::EvaluateAnimation`에서 계산된다. 다만 `FinalizePose`가 같은 배열을 물리 결과로 덮어쓴다.

조사 메모: Sonnet 조사 두 건과 리뷰 한 건(2026-11-23, 코드 읽기만). 요점은 아래 결정에 반영했다.

## 결정

D1. 애니메이션 포즈 보존
- `ModelInstance::EvaluateAnimation` 안에서 순수 애니메이션 전역 노드를 별도 버퍼로 복사한다. EvaluateAnimation은 프레임당 한 번이고 FinalizePose는 여러 번 불릴 수 있으므로, 복사는 EvaluateAnimation에서 한다.
- 같은 자리에서 owner당 한 번 애니메이션 Joint 배열(`G_anim·IBM`, 법선 행렬 포함)을 만들어 노출한다. 조각마다 다시 계산하지 않는다.
- `MeshComponent`에는 기존 `GetPreparedNodes`/Joints 옆에 애니메이션용 getter를 두고, `Update`에서 연결한다.

D2. A 조각 팔레트
- `PatchSkinSnapshot`에 `AnimFollow`를 추가한다. 켜는 조건은 둘 다다.
  - 스켈레톤 루트를 포함한 몸체 그룹이다(`BuildCutBodyGroups`의 group 0 = 모든 Pi의 비자손).
  - boneCut 경로(Frozen 전부 0, Side "D5")가 아니다.
  - 팔 조각처럼 Frozen에 몸통 joint가 든 몸체는 켜지 않는다.
- 고정 joint k의 팔레트(옵션 1, AnimFollow): `C(t)·A_k(t)·O_k`
  - `C(t) = G_att(t)·inv(G_att_anim(t))`. 부착 node가 키네마틱이면 I다. 부착 node까지 물리에 덮인 몸에서도 부착 node를 따르게 하는 보정이다. 다중 본에서 이 보정은 부착 node 하나로 전체를 옮기는 근사다.
  - `A_k(t)` = 애니메이션 Joint k(posXform).
  - `O_k = inv(C(t_cut)·A_k(t_cut))·JointsAtCut[k]`. 절단 시점에 팔레트가 원래 값과 정확히 같아지게 하는 joint별 오프셋이고, snapshot에 저장한다. 부착 node와 k 사이에 물리 override가 있어도 게시 순간 튀지 않는다. 이후에는 애니메이션의 상대 움직임을 따른다.
- 슬롯 K는 지금처럼 `D`다. 끝점으로 폴백하는 정점과 cap 법선에 쓴다.
- B 조각(래그돌 쪽)은 지금처럼 고정한다.

D3. 실제 분리 rim은 끝점 스키닝
- 지금 rim은 슬롯 K다. 이를 내부 교점과 같은 끝점 레코드로 바꾼다. 원래 메시 변의 두 끝점을 조각 팔레트로 각각 스키닝한 뒤 t로 보간하고, 위치·법선·탄젠트 모두 보간한다.
- 어느 본을 따를지 정하지 않는다. E자처럼 본이 여러 조각 나도 각 rim이 자기 변의 weight를 따른다. 제거 쪽 끝점의 joint도 조각 팔레트(전체 joint + K)에 있다.
- `PatchSkinBuildCS`에서 "두 body가 같음" 항을 조각별 플래그로 우회한다. 무효 body 가드는 유지한다. 플래그는 cbuffer 여유 공간(Padding 등)에 snapshot.AnimFollow·옵션으로 채운다. 그래서 B 조각과 옵션 0은 지금처럼 K다.
- ExtInternal 값: 1 = 같은 body 내부 교점, 2 = 실제 분리 rim, 3 = cap. `PreparePoseCS`는 0이 아닌지만 보므로 1과 2는 바뀌는 것이 없다.
- CPU 판독(PatchSkinning.cpp의 RS-2b 통계, SkeletalPatchCopy의 PS4 rim 선별 `!internal`, PS2 기대값)은 1/2/3을 구분하도록 고친다.

D4. cap은 대응 rim과 같은 끝점
- cap 정점은 링 슬롯 시작점(enter)이다. 삼각분할(CapEars)은 슬롯만 쓰고 내부 정점이 없다.
- `ClipTrianglesCS`가 enter를 만드는 세 경로마다 링 슬롯의 미사용 영역(+116~127)에 (Origin.z, Origin.w, Parameter)를 저장한다.
  - 교차 hit: `Blend` 결과
  - 평면 위 끝점: IntersectEdge 결과(t = 0 또는 1)
  - 변 전체가 평면 위(kind 2/4): z = w = 원본 정점 번호, t = 0
- `FinalizeCutCS`는 cap 꼭짓점마다 `Origin = (0xffffffff, leader, e0, e1)`, Parameter = t를 쓴다. b/c 교환 전에 넣는다. cap Origin.z의 의미가 "슬롯 번호"에서 "원본 정점 번호"로 바뀐다. 기존 판독 코드는 z/w를 쓰지 않는다(리뷰 확인).
- `PatchSkinBuildCS`는 cap에 internal = 3으로 끝점 레코드를 쓰고, 끝점 번호 범위를 검사한다(범위 밖이면 K).
- 용접 차이: 링 용접 때문에 cap 위치가 rim과 수십 μm 다를 수 있다. 판정은 "t_cut 대비 rim–cap 간격 변화"로 한다.
- cap 법선: internal = 3이면 `PreparePoseCS`가 위치만 끝점 보간하고, 법선·탄젠트는 주 레코드(K, bind = 평면 법선) 결과를 그대로 쓴다.
  - 한계: 그루터기가 부모에 대해 돌면 cap 법선은 부모만큼만 돈다. AF-6 캡처에서 관찰한다.
- rim(D3)과 cap(D4)은 함께 켠다.

D5. 옵션과 기본값
- `-patch-anim-follow 0|1`로 D2·D3·D4를 함께 켠다. 구현 중 기본은 0이고, AF-6에서 검증 뒤 기본 1로 바꾼다.
- 옵션 0은 렌더·진단 결과가 지금과 같다. 단 AF-3의 링·cap Provenance 바이트는 옵션과 무관하게 바뀐다.

D6. 재절단(`PatchRecutSkinning.md`)
- 재절단에서 스키닝을 승계하는 것은 node 쪽 결과뿐이다(RS-3). 그래서 AnimFollow도 몸에 남는 쪽만 승계한다. 반대쪽은 정적이다.
- 새 split·cap의 두 끝점이 부모의 일반 복사 정점(internal = 0)이면, 부모 주 skin으로 끝점 레코드를 만든다.
- 끝점 중 하나라도 internal ≠ 0이면 K로 폴백한다. 옵션 1에서 그루터기 절단면 근처를 다시 자르면 흔할 수 있으므로, AF-5에서 폴백 비율을 로그로 보고한다.
- 판정 갱신
  - `SkinnedFollowError`: 끝점 정점은 기대 위치를 "요청 시점 팔레트로 두 끝점 스키닝 후 t 보간"으로 계산해 비교한다. K 정점이 없어도 실패하지 않는다.
  - RS-2b: K 수와 끝점 수, 폴백 수를 나눠 보고한다.

## 단계

| 단계 | 내용 | 완료 확인 |
| --- | --- | --- |
| AF-1 | D1 애니메이션 노드·Joint 보존과 getter(ModelInstance·MeshComponent), 단위 검사 | override 없으면 최종 = 애니메이션, 있으면 override 노드만 다름. 회귀 PASS(동작 불변) |
| AF-2a | D2 snapshot `AnimFollow`·`O_k`, `BuildPatchPalette` 식 | 옵션 0 회귀 PASS. 옵션 1에서 t_cut 팔레트 = JointsAtCut ≤ 1e-6 |
| AF-2b | D5 옵션, 호출처 5곳(MeshComponent Update, SkeletalPatchCopy 진단 4곳)에 애니메이션 Joint 연결, 진단 로그 | 옵션 1에서 A 고정 joint 팔레트 = C·A_k·O_k CPU 재계산 ≤ 1e-6. 물리 override가 있는 owner 케이스 로그 |
| AF-3 | D4 데이터: 링 (e0, e1, t) 세 경로, FinalizeCutCS cap Provenance | cap (e0, e1, t)와 같은 위치 rim의 일치·불일치 수 보고. 회귀 PASS, PlaneSlicerTests·GpuCutValidation PASS |
| AF-4 | D3·D4 스키닝: PatchSkinBuildCS 조각별 플래그로 internal 2·3, PreparePoseCS cap 법선 분기, CPU 판독 1/2/3 구분, PS4·PS2 진단 | 옵션 1에서 PS4 rim–cap 간격의 t_cut 대비 변화(게시·60프레임)가 용접 거리 이하, PS2 PASS, SP7e2 PASS |
| AF-5 | D6 재절단 끝점·K 폴백, SkinnedFollowError·RS-2b 갱신 | 옵션 0/1 모두 `-attached-recut-checks 1·2` PASS, 폴백 비율 로그 |
| AF-6 | CesiumMan 걷기 팔·다리 절단, 다중 본(`-skeletal-multi-cut 1`) 옵션 0/1 캡처, 기본값 1 전환, 문서 동기화(상태 줄·README·CodeMap·PatchSkinning·PatchRecutSkinning) | 그루터기가 걷기를 따르고 절단면이 붙어 있음, cap 법선 관찰, 회귀 PASS(기본 1) |

- 각 단계는 약 300줄 이하로, 새 구현 실행 → 다른 실행 리뷰 → Release 빌드 → 회귀 → 로컬 커밋 순서로 진행한다.
- 회귀 6종: `-skeletal-patch-offset-copy 1 -skeletal-patch-checks 1`(SP6), `-skeletal-patch-break-checks 1`(SP7·SP7e2), `-skeletal-break-checks 1`(BC-12a), `-skeletal-break-xbot 1`(BC-12b), `-attached-recut-checks 1`(AR4), `-rig-bone-cut-checks 1`.
- 위치 판정 단위는 `PatchRecutSkinning.md`처럼 local 1e-6이고, 월드 mm는 참고로 남긴다.

## 완료 조건

1. 옵션 1에서 A 조각의 그루터기 정점이 애니메이션 포즈로 스키닝된다. 게시 순간 튐이 없다(t_cut 팔레트 = 원래 팔레트 ≤ 1e-6).
2. 옵션 1에서 rim과 cap이 그루터기와 함께 움직이고 서로 벌어지지 않는다(PS4 간격 변화 ≤ 용접 거리, 게시와 60프레임 뒤).
3. 원본과의 경계(SP7e2)가 PASS다. 경계 정점에 고정 joint weight가 있으면 frozenWeightMax로 보고한다.
4. 다중 본 절단에서도 각 rim이 자기 가지를 따른다(캡처 확인).
5. 재절단이 옵션 0/1 모두 PASS이고, K 폴백 비율을 보고한다.
6. 옵션 0의 렌더·진단 결과가 지금과 같다(회귀 6종 PASS).
7. CesiumMan 캡처로 옵션 0/1 차이를 확인하고 기본값 1로 바꾼다.

## 범위 밖

- B 조각(래그돌 쪽)의 고정 방식.
- cap 법선을 지배 본으로 회전하는 개선.
- 재절단에서 끝점이 두 겹이 되는 경우의 4끝점 레이아웃(K 폴백으로 대체).
