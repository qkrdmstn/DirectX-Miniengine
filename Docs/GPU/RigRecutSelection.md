상태: 완료 — RR-1 `d82176fc`(재현 옵션), RR-2 로컬 커밋(해시는 `git log`). 자식 관절이 이미 끊긴 뼈를 Rig 모드에서 다시 자를 수 있게 한 선택 조건 수정(base `b649eaf7`, 2026-11-19)

# Rig 모드 재절단 선택 결정

## 1. 문제

- 기본 C(Rig 모드)로 종아리를 자른 뒤 같은 다리 허벅지 중간을 자르면 요청이 거절된다.
- 같은 요청을 Parent 모드로 보내면 엉덩이 관절을 끊고 정상 게시된다. 이때 평면 위쪽 살은 엉덩이에 남고, 아래쪽은 허벅지 body와 함께 떨어질 것으로 예상하지만 화면으로는 아직 확인하지 않았다.
- 목표: Rig 모드도 이 경우 Parent 모드처럼 허벅지 본의 incoming 관절을 끊는다. 같은 부위 재요청 거절과 root 선분 처리는 지금과 같게 둔다.

## 2. 근거 (임시 재현, Xbot, MX450 Release, 2026-11-19)

| 순서 | Rig 모드 | Parent 모드 |
|---|---|---|
| 1차 종아리 평면 | 선분 61→62 선택, 관절 44(60→61) 끊김, 1556 게시 | 같음 |
| 2차 허벅지 평면 | `RigSelect result=AlreadyBroken reason=legacy_segment_unavailable` → `SP7b rejected: incoming=3` | 관절 43(3→60) 끊김, 2118 게시, nodes 3/60 |

- 로그와 임시 diff: `%TEMP%\recut-repro\`(저장소 밖).
- CesiumMan은 허벅지 평면이 물리 body에 닿지 않아(`no_hits` → `no skin target`) 원인을 확인하지 못했다.

## 3. 원인

- `SkeletalPhysicsSystem::BuildKinematicSegments`는 rig 관절 i마다 "부모 body의 본 원점 → 자식 body의 본 원점" 선분을 만든다. 후보 조건은 "관절 i가 살아 있고, 부모·자식 body가 모두 Kinematic"이다.
- `SelectRigSegment`는 고른 선분의 부모 node를 `Selected.NodeId`로 넘긴다. 실제로 끊을 관절은 `SkeletalPatchCopy::Request`의 `FindIncomingJoint`가 그 node의 incoming으로 정한다.
- 매핑:
  - 선분 61→62(종아리 본): 선분 자체 관절 45, 선택 node 61, 끊는 관절 44(60→61).
  - 선분 60→61(허벅지 본): 선분 자체 관절 44, 선택 node 60, 끊는 관절 43(3→60).
- 후보 조건은 선분 **자체 관절**과 자식 body 상태를 보는데, 실제로 끊는 것은 **선택 node의 incoming 관절**이다. 1차 절단 뒤 허벅지 선분은 자체 관절 44가 끊기고 자식 61이 Dynamic이라 후보에서 빠진다. 그러나 끊을 대상인 관절 43은 살아 있다.

## 4. 결정과 이유

### D1. 후보 조건을 "끊을 관절" 기준으로 바꾼다 (root만 예외)
- 선택 node(선분 부모)가 incoming 관절을 가진 경우 후보 조건은 다음 두 가지다.
  - 선택 node의 body가 Kinematic이다.
  - 그 incoming 관절이 살아 있다.
- 선분 자체 관절의 생존 여부와 자식 body의 Kinematic 여부는 보지 않는다.
- 선택 node가 rig root라 incoming이 없으면 지금 조건을 그대로 쓴다. 그러면 지금처럼 선택된 뒤 `Request`가 `RigRoot`로 거절한다. 새 조건으로 바꾸면 root 선분이 후보에서 빠져 `AlreadyBroken`이나 D5로 잘못 넘어갈 수 있기 때문이다.
- 말단 node에는 나가는 선분이 없으므로 이번 변경과 무관하다. 기존 D5/legacy Leaf 경로를 그대로 둔다.
- 이유: 3절의 불일치를 없애는 최소 수정이다. Parent 모드가 같은 요청에서 관절 43을 끊으므로 동작 기준이 있다. `PrepareBreak`는 이미 끊긴 자식 관절을 건너뛰고, `CommitPrepared`는 선택 관절만 무효화한다. 따라서 상위 관절 재절단은 물리 쪽에서도 지원된다.
- 같은 부위 재요청은 계속 거절된다. 종아리를 다시 자르면 선택 node 61의 body가 Dynamic이라 후보가 아니다.

### D2. Dynamic 자식의 선분 끝점은 물리 덮어쓰기 전 애니메이션 자세로 계산한다
- `UpdateKinematicTargets`는 애니메이션으로 `Targets[i].Frame`을 계산한다. 그 뒤 Dynamic body는 `GetBodyPose`로 덮어쓴다. 따라서 지금 `Targets`만 읽어서는 애니메이션 자세를 얻을 수 없다.
- 결정: 같은 함수에서 덮어쓰기 **전** 애니메이션 Frame을 별도 필드(예: `AnimationFrame`)에 보관한다. `BuildKinematicSegments`는 자식이 Dynamic일 때만 이 값으로 끝점을 계산한다.
- `Targets[i].Frame`의 기존 의미는 바꾸지 않는다. `PrepareBreak` 등 기존 소비자도 그대로 둔다.
- 부모 기준 rest 상대변환으로 계산하는 안은 현재 애니메이션의 무릎 위치와 다를 수 있어 택하지 않았다.
- 갱신 시점은 렌더 프레임이 아니라 fixed substep이다(`PhysicsSystem::AdvanceSimulation`). 선분은 마지막 substep 기준이며, 지금과 같다.

### D3. 선택 탈락 사유를 정확히 남긴다
- 지금은 후보가 없을 때 사유를 일괄 `AlreadyBroken`으로 남긴다. 실제 incoming 조회 결과가 아니다.
- 탈락 사유를 구분해 로그에 덧붙인다: 선택 node body가 Dynamic, incoming 관절이 끊김, root(incoming 없음), 자세·constraint 조회 실패.
- 상태 코드 값과 기존 `compare=` 비교 필드는 유지한다. 자식이 Dynamic이라는 사실만으로 부적격처럼 보이는 `legacy_endpoint_Dynamic` 설명은 고친다.

### D4. 재현 옵션을 진단 옵션으로 먼저 만든다
- 기본 off인 `-skeletal-recut-repro 1|2`를 만든다(1=Xbot, 2=CesiumMan). 두 번 연속 C 요청을 보낸다.
- 첫째·둘째 본 이름은 `-skeletal-recut-repro-first`·`-second`로 바꿀 수 있게 한다. 기본값은 같은 다리의 종아리와 허벅지다. 이 옵션으로 root 선분과 말단 경우도 실행할 수 있다.
- 모드를 강제한다. `-skeletal-recut-repro-mode rig|parent`가 `m_SkeletalJointMode`보다 우선한다.
- 판정을 추적한다: 2차 요청·게시·실패를 1차와 따로 추적하고, 2차 게시 성공 또는 거절·timeout을 명시적으로 PASS/FAIL 로그로 남긴다. 기존 데모 실패 검사(`!m_DemoAppliedFrame`)에 기대지 않는다.
- 캡처: 2차 게시 뒤 캡처 시점을 따로 센다(`m_DemoCaptured` 재사용 금지).
- Startup에서 다른 자동 검사·데모 옵션과 함께 쓰는 것, 잘못된 모드 값, 종속 옵션만 단독으로 쓰는 것을 거절한다.
- CesiumMan 평면은 본 중점이 아니라 물리 body(`BuildBoxBody`의 convex 상자) 중심을 지나게 한다. 요청 로그에 상자 중심·크기와 평면 거리를 함께 남긴다.

### D5. 범위
- 관절 부근 절단 재설계(보류)의 "break 관절을 뼈대만으로 결정"은 이보다 큰 변경이다. 이번에는 Rig 선택의 후보 조건만 고친다.
- Parent·Influence 선택 코드, `SelectMultiCut`, GPU 추출 코드는 수정하지 않는다. 다중 컷은 Rig 선택의 owner 결정과 거절 분기를 거쳐 들어가므로 간접 영향이 있다. 그래서 Rig 모드 다중 컷을 회귀로 확인한다.

## 5. 단계와 예상 시간

| 단계 | 내용 | 예상 시간 |
|---|---|---|
| RR-1 | D4 재현 옵션(`ModelViewer::Startup/Update/RenderScene/IsDone`). 수정 전 코드에서 Xbot Rig 2차가 **거절**로 판정되는지, Parent가 성공하는지 기록한다(약 120~170줄, 추정) | Codex 구현 15~25분, Release·Debug 빌드 각 20초~2분(실측), 실행 10분, 리뷰 2~5분 |
| RR-2 | D1~D3 선택 수정(`BuildKinematicSegments`, `UpdateKinematicTargets`, `SelectRigSegment`, 사유 로그)과 7절 회귀(약 80~140줄, 추정) | 같음 + 회귀 검사 15~20분 |

- 각 단계 줄 수에 문서 동기화를 포함하고, 300줄은 실제 diff로 확인한다.

## 6. 실행 공통 조건

- 작업 디렉터리 `MiniEngine/ModelViewer`, `-debug 1`(debug layer)을 쓴다.
- 재현 옵션 실행에는 캡처 경로 옵션을 주고, 2차 게시 뒤 120프레임 안에 종료한다. 데모처럼 종료하지 않는 실행은 timeout으로 끝내고 그 사실을 기록한다.
- 1차·2차 요청 시점은 프레임 번호로 고정한다(1차 frame 60, 2차는 1차 게시 뒤 45프레임). 그래야 2118 같은 수치를 다시 낼 수 있다.

## 7. 완료 조건

판정은 PASS, FAIL, 미확인 셋으로 나눈다. 미확인은 PASS로 세지 않는다.

**RR-1**
1. D/R 빌드 오류 0, D3D12 ERROR 0.
2. `-skeletal-recut-repro 1`(Rig): 2차 요청이 FAIL로 판정되고, 거절 로그(`incoming=3`)가 2절과 같다.
3. `-skeletal-recut-repro 1 -skeletal-recut-repro-mode parent`: 2차가 PASS(관절 43, 2118, nodes 3/60).
4. `-skeletal-recut-repro 2`: 상자 중심 평면이 body에 닿는다(`no_hits` 아님). 2차 결과를 기록한다.
5. Startup 충돌 거절이 동작한다.
6. 기존 회귀 5종 PASS: `-skeletal-mask-checks 1`, `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1`, `-skeletal-union-checks 1 -skeletal-break-demo 2`, `-skeletal-break-demo 1`·`2`.

**RR-2**
1. D/R 빌드 오류 0, D3D12 ERROR 0.
2. `-skeletal-recut-repro 1`(Rig): 2차가 PASS(관절 43, 2118, nodes 3/60)로 Parent 결과와 같다. CesiumMan(`2`)도 허벅지 incoming을 끊고 PASS한다. 안 되면 FAIL로 두고 이유를 보고한다.
3. 같은 부위 재요청 거절: `-skeletal-patch-break-checks 1`과 `-skeletal-patch-break-checks 1 -skeletal-joint-mode rig` 둘 다 PASS.
4. root 선분: `-skeletal-recut-repro-first`/`-second`로 root 선분(예: Hips→Spine)을 자르면 지금처럼 `RigRoot`로 거절된다.
5. 말단: 부모→말단 선분과 말단 body 내부 평면이 수정 전과 같은 결과다. 수정 전 결과는 RR-1 옵션으로 기록한다.
6. Rig 모드 다중 컷 회귀: 기존 MI-4h2 허벅지 다중 컷 명령을 Rig 모드로 실행해 결과(constraint 18→16, 300/187/175)가 기존과 같다.
7. 기존 회귀 5종 PASS.
8. 2항 Xbot 실행의 2차 게시 뒤 캡처를 직접 확인한다: 허벅지 위쪽 살이 엉덩이에 남고 아래쪽이 떨어지는지 본다. 보지 못하면 미확인으로 기록한다.

## 8. RR-1 결과와 RR-2 기준 자료 (2026-11-19, Release, MX450, `-debug 1`, 수정 전 선택 로직)

옵션: `-skeletal-recut-repro 1|2`, `-first`, `-second`, `-mode rig|parent`. 로그는 `RecutRepro request/published/PASS/FAIL`. 요청 로그는 256자 `Utility::Printf` 제한 때문에 두 줄이다.

| 실행 | 판정 | 핵심 로그 |
|---|---|---|
| Xbot Rig | FAIL(예상대로) | `SP7b rejected: incoming=3`, 2절과 동일 |
| Xbot Parent | PASS | 관절 43, 2118, nodes 3/60 |
| CesiumMan Rig | FAIL | 상자 중심 평면이 body에 닿음(`no_hits` 아님), 2차 `incoming=3` 거절 — 3절 원인이 CesiumMan에서도 확인됨 |
| CesiumMan Parent | PASS | 관절 10, 154, nodes 2/13 (Debug는 자세 차이로 D5 leaf를 골라 거절, Release를 기준으로 삼는다) |
| Startup 충돌 | 거절 | 데모·검사 병용, 잘못된 모드·모델 값, 종속 옵션 단독 5종 모두 예외 종료 |
| 회귀 5종 | PASS | mask·patch(offset-copy)·union+demo 2(게시 없는 진단 종료)·demo 1·2 |

D3D12 ERROR 0, Release·Debug 빌드 오류 0.

**RR-2 기준(수정 전, Xbot Rig, 1차 종아리 뒤 2차 = 아래)**
- root 선분 `-second mixamorig:Hips`: 선택은 되고 `SP7b rejected: incoming=2`(RigRoot). FAIL(거절)이 기대 결과다.
- 말단 선분 `-second mixamorig:LeftToeBase`(부모→말단): `RigSelect result=no_candidates reason=legacy_leaf`, FAIL(거절).
- 말단 body 평면 `-second mixamorig:LeftToe_End`(DFS 다음 본이 반대쪽 다리라 평면이 맞지 않음, 참고용): `AlreadyBroken`, `incoming=3` 거절.

## 9. RR-2 결과 (2026-11-19, MX450, `-debug 1`, Release·Debug 빌드 오류 0, D3D12 ERROR 0)

변경: `BuildKinematicSegments`(후보 = 선택 body Kinematic + incoming 생존, root는 기존 조건, 자식 끝점은 부모 Kinematic·자식 Dynamic일 때만 `AnimationFrame`), `UpdateKinematicTargets`(덮어쓰기 전 `AnimationFrame` 보관), `SelectRigSegment`·`Request` 로그(`rejected=` 사유, `legacy_endpoint_Dynamic` 설명 교체). 첫 구현은 탈락 선분의 끝점까지 애니메이션 자세로 만들어 `-skeletal-patch-break-checks 1 -skeletal-joint-mode rig`가 예외로 끝났고(수정 전엔 PASS), 부모가 Dynamic이면 기존 물리 자세를 쓰도록 고쳤다.

| 항목 | 판정 | 핵심 |
|---|---|---|
| Xbot Rig(`1`) | PASS | 관절 43, 2118, nodes 3/60 (Parent와 동일, Release·Debug) |
| CesiumMan Rig(`2`) | PASS | 관절 10, 154, nodes 2/13 (Release·Debug) |
| Parent `1`·`2` | RR-1과 동일 | Release PASS·PASS; Debug `2`만 D5 leaf 거절(RR-1과 같음) |
| patch-break-checks 기본·`rig` | PASS | 세 모델 모두 `second C AlreadyBroken PASS` (Release·Debug) |
| root `-second mixamorig:Hips` | 수정 전과 같음 | `incoming=2`(RigRoot) 거절 |
| 말단 `-second mixamorig:LeftToeBase` | 수정 전과 같음 | `no_candidates`, `legacy_leaf` 거절 |
| 말단 body 평면 | 미확인(새 기준 없음) | 옵션이 DFS 다음 본으로 평면을 잡아 말단 본(`LeftToe_End`)은 같은 다리 평면을 만들 수 없음. 참고로 `LeftToe_End` 실행은 수정 전과 같이 `AlreadyBroken`, `incoming=3` |
| MI-4h2 허벅지 다중 컷(Rig) | PASS | constraint 18→16, 300/187/175 (Release·Debug) |
| 회귀 5종 | PASS | mask·patch(offset-copy)·union+demo 2(게시 없는 진단 종료, RR-1과 동일)·demo 1·2(timeout까지 Applied) |
| 캡처(Xbot Rig 2차 뒤) | 대체로 확인 | Parent와 거의 같은 모습: 엉덩이 쪽에 허벅지 윗부분과 절단면이 남고 아래쪽이 떨어짐(단일 시점) |

리뷰(Codex 새 실행): 지적 없음.
