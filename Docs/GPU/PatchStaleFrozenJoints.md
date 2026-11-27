상태: 진행 중 — ST-1·ST-2 완료(`007c9306`), ST-3 구현·부분 검증 완료(기존 패치가 있는 다중 batch 미검증: Xbot·CesiumMan 연속 다중은 게시 전 MI4d2 loop 실패, 기존 결함 판정), ST-4 대기, 기준 master `007c9306` (2026-11-27)

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
| ST-2 | 완료(`007c9306`): `Request`의 동일 t_s 입력을 성공한 단일 `Update` 게시에서 이력에 추가하고 기존 패치 신규 슬롯만 고정. publication 복사 후 교체·VB 공유. 만료 owner/모델 이력 회수, CPU skin의 정점별 위치 경계 진단 완료 | 리뷰 보완 추가 약 120줄 이내 |
| ST-3 | 구현·부분 검증 완료(미커밋): 다중 root별 기록·RS 자식 전체 이력·새 A/B 이전 이력 적용(D2·D5). 정순/역순·AF 0·LB 1·CesiumMan batch·RS/AR4 PASS. Xbot 다중 게시 및 기존 패치가 있는 batch 검증은 loop 실패로 미완료 | 코드 약 110줄 변경 |
| ST-4 | 검증, 문서 상태 줄·README·CodeMap 갱신 | 문서 |

## 완료 조건

1. 재현 인수에서 2차 게시 `SKINSTALE`의 모든 몸통 패치가 0건이고 `cpu=unavailable`이 없다.
2. 역순(LeftUpLeg→RightUpLeg)과 3회 절단(두 허벅지 뒤 위팔)에서도 1과 같다.
3. AF 0/1, LB 0/1 각각에서 1이 성립하고 갱신 slot 팔레트가 유한하다. 경계 오차는 Release에서 1 mm 이하다.
4. RS 재절단 전후, 다른 owner(CesiumMan)의 패치는 바뀌지 않는다. 거절·취소된 요청 뒤 기존 패치 snapshot이 그대로다.
5. 1회 절단 장면(Xbot·CesiumMan 허벅지·위팔·몸통)의 MI 로그가 기준(`%TEMP%\g1a-equivalence-20261126`)과 같다. 연속 재절단 회귀(RS 계열, AR-4)가 기존처럼 PASS한다.
6. 재현 장면을 육안으로 봤을 때 늘어난 삼각형이 없다(사용자 확인).
7. Release 빌드 성공, D3D12 ERROR 0.

## ST-2 검증 (2026-11-27, 미커밋)

- `PublishDetachRecord`: owner·모델별 요청 입력을 보관하고 성공한 단일 게시에서만 이력에 추가한다. 이번 A/B는 제외한다. 기존 Frozen·K·AnimFollow 자격·기존 offset/비율을 보존한다.
- `PruneDetachHistory`: 매 `PublishDetachRecord`·`Update`·`Reset`에서 Scene에 없는 owner 또는 `SourceModel` weak 참조가 만료된 기록을 제거한다. `Scene::DeleteObject`·`Scene::Clear`는 SkeletalPatchCopy에 통지하지 않는다. Reset의 요청 취소는 살아 있는 owner의 게시 이력을 지우지 않는다.
- 물리 팔레트·node와 애니메이션 팔레트·node는 A snapshot과 같은 `Request`에서 복사한다. `JointsAtCut`과 `AnimOffset`은 기존 패치의 부착 기준으로 계산한다.
- `PatchSkinPublication` 복사는 snapshot/CPU 배열을 값 복사하고 `ByteAddressBuffer`의 ComPtr·descriptor를 공유한다. 버퍼 생성이나 skin VB 재구축은 없다. RS가 가진 이전 publication은 변경하지 않는다.
- Release/Windows 최종 빌드 성공(exit 0, 약 46.2초, 경고 16·오류 0). 기존 경고와 후처리의 `pwsh.exe` 부재 메시지가 있다. 리뷰 보완 빌드 로그: `%TEMP%/st2-review-release.log`.
- NVIDIA GeForce MX450(드라이버 버전 조회는 권한 거부로 미확인), 요청한 480프레임 재현: AF 1/0 모두 exit 0(각 약 16.5/15.8초), `patch=2 cut=2 vertsWithDetachedLiveWeight=0`, 활성 패치 `cpu=exact paletteFinite=1`.
- 정점 경계 지표: quiet=0의 기존 패치 `DiagnosticSkin`에서 신규 고정 슬롯에 유효 가중치가 있는 정점만 집계한다. `BuildPatchPalette`가 게시 프레임 입력으로 만든 위치 팔레트(회전·스케일 포함)로 skin한 위치와, 신규 고정 슬롯만 새 A 팔레트로 대체한 위치를 비교한다. 다른 슬롯은 기존 팔레트 그대로이며 확장 정점은 두 끝점을 skin한 뒤 보간한다. section 예약 공간은 제외하고 CPU 사본이나 새 A 팔레트가 없으면 `boundary=unavailable`이다.
- `patch=2 cut=2`: AF 1 `boundaryMaxMm=0`, AF 0 `boundaryMaxMm=0.000243629`, 각각 `verticesCompared=188 newFrozen=5`. 이 값은 같은 기존 정점에 두 팔레트를 적용한 차이이며 원본 정점 ID로 패치 간 대응을 찾은 지표는 아니다. AF 0은 `-patch-anim-follow 0`으로 실행한다.
- 단일 절단도 정상 종료(exit 0, 약 15.5초), `patch=2 cut=1` 0건·`paletteFinite=1`. 기존 `%TEMP%/SKINSTALE-control-patch.log`와 MI 행 차이 0건. AF 0 실행은 약 16.6초에 정상 종료했다.
- 리뷰 보완의 요청한 480프레임 AF 1/0 로그는 `%TEMP%/st2-review-{af1,af0}-patch.log` 및 대응 `-perf.log`에 보관한다. 이전 단일 절단 로그는 `%TEMP%/st2-single-{patch,perf}.log`이다.
- debug 출력 수집기는 임시 DLL 생성 권한 오류로 사용할 수 없었다. 요청 인수 `-debug 0`에서 D3D12 validation ERROR 0은 확정하지 않는다.
- 당시 남은 ST-3 구현·검증은 아래에 기록한다. ST-4 전체 회귀와 사용자 육안 확인은 남아 있다.

## ST-3 구현·검증 (2026-11-27, 미커밋)

- `Request`는 다중 절단에도 A와 동일한 요청 시점 입력·전체 root 목록·남은 길이 비율을 보관한다. `PublishDetachRecord`는 성공한 단일/batch 게시에서 새 A/B에 이전 이력을 적용한 뒤 root마다 기록 하나를 추가한다. `ConsumeMultiBreak`의 거절·stale 반환 경로에서는 호출하지 않는다.
- `ApplyDetachRecord`는 ST-2 계산·경계 진단을 재사용한다. owner/모델·활성 skin·SourceFrozen·AttachNode subtree를 검사하고 기존 Frozen·K·AF 자격·기존 offset/비율을 보존한다. publication만 복사 후 교체하며 geometry·skin VB는 공유한다.
- `CuttingSystem::UpdateOne`은 RS 자식 설치 때 `ApplyRecutDetachHistory`로 그 owner의 전체 이력을 적용한다. quiet=0에서는 팔레트를 준비한 뒤 `SKINSTALE boundary=RS child=...`와 기존 CPU/유한성 진단을 남긴다. 요청 당시 부모 publication은 수정하지 않는다.
- Release/Windows: `%TEMP%/st3-release.log` exit 0, 48.44초, 경고 62·오류 0. 최종 주석/빈 줄 정리 빌드 `%TEMP%/st3-release-final.log`도 exit 0, 49.84초, 경고 16·오류 0이다. GPU는 실행 ENV의 NVIDIA GeForce MX450; 드라이버는 이번에 조회하지 않았다.
- 공통 실행: 작업 디렉터리 `MiniEngine/ModelViewer`, `-debug 0 -skeletal-break-demo 1 -skeletal-break-demo-delay 2 -physics-query-exit 480 -cut-perf-log <TEMP 절대 경로> -cut-perf-quiet 0 -cut-perf-gpu 0`. 각 실행 전 `SP6-SkeletalPatchChecks.log`의 바이트 offset을 기록해 이후 부분만 수집했다. 로그·인수·offset·종료 코드는 `%TEMP%/st3-<사례>-{patch,perf}.log`, `st3-<사례>-result.json`에 보관한다.

| 사례 | SKINSTALE 행 / 정점 집계 | boundaryMaxMm | exit |
| --- | --- | --- | --- |
| forward: RightUpLeg→LeftUpLeg | 11 / 6개 패치 검사 모두 0 | 0 | 0 |
| reverse: LeftUpLeg→RightUpLeg | 11 / 6개 모두 0 | 0 | 0 |
| af0: forward + `-patch-anim-follow 0` | 11 / 6개 모두 0 | 0.000251367 | 0 |
| lb1: forward + `-patch-length-blend 1` | 11 / 6개 모두 0 | 0.000240736 | 0 |
| multi: forward + `-skeletal-multi-cut 1` | 0 / 게시 전 `MI4d2 missing multi-P loop` | 미측정 | -1073740791 |
| multi-cesium: demo 2, `leg_joint_R_1`, multi/union-checks 1 | 5 / 3개 모두 0, root 13/17 기록 각 1개 | 신규 고정 슬롯 없어 미측정 | 0 |
| multi-sequence: demo 2, `Skeleton_arm_joint_L__4_`→`leg_joint_R_1`, multi 1 | 첫 단일 게시 3 / 2개 모두 0; 두 번째 게시 전 같은 loop 실패 | 미측정 | -1073740791 |
| multi-reject / multi-stale: multi-cesium + 각 주입 옵션 1 | 각 0 / 이력 추가 없음, 원본·마스크·constraint 무변경 PASS | 해당 없음 | 각 0 |
| single: forward에서 second-bone 제외 | 3 / 2개 모두 0; ST-2 control MI 14행 차이 0 | 신규 고정 슬롯 없어 미측정 | 0 |
| rs-final: `-attached-recut-checks 2` 전용 실행 | 9 / 최초·두 RS 설치 합계 6개 검사 모두 0, 자식 id 2 | ST-2 지표 없음; RS3 게시/+60·두 번째 게시/+60 모두 0 mm | 0 |

- 성공 실행의 모든 검사에서 `cpu=exact paletteFinite=1`, `cpu=unavailable` 0건이다. forward/reverse 새 몸통 패치에 이전 root의 5개 슬롯이 고정된 `boundary=history` 로그를 확인했다. 이 로그 자체에는 수치 경계 비교가 없으며 위 ST-2 지표는 기존 패치의 신규 고정 슬롯 비교다.
- RS 전용 검사는 demo·delay·physics-query-exit 병용을 거절한다. 초기 `rs`/`rs-valid` 시도는 옵션 충돌로 각각 exit -1073740791·SKINSTALE 0행이었다. 최종 실행은 이 옵션들을 제외했다. RS2b 전부 PASS, AR4 다섯 판정·RS4 연속 skinned 자식 게시/+60·전체 `Attached recut checks` PASS다. 과거 snapshot을 보관한 RS 도중 다른 break가 게시되는 동시 상황은 기존 옵션으로 재현하지 않았다.
- 기존 옵션은 `cut-perf-second-bone` 한 번만 지원하므로 3회 절단은 실행하지 않았다. 다른 owner의 기존 snapshot 불변·취소·진행 중 RS의 늦은 이력 반영은 코드 경로로만 확인했고 별도 런타임 검증은 하지 않았다. debug 0 실행이므로 D3D12 validation ERROR 0 및 육안 결과는 확정하지 않는다. 완료 조건 2~5 전체 충족을 주장하지 않는다.
