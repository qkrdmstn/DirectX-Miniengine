상태: 폐기 (2026-11-24 사용자 결정, 구현하지 않음). 원래 설계안 — 관절 부근 절단 재설계: 뼈대 전용 P 선택, 쪽-그룹 불일치 패치 선택, 쪽 기준 배정(base `2fdfa0e8`, 2026-11-19). 근거 조사는 [JointCutRedesignMemo.md](JointCutRedesignMemo.md).

# 관절 부근 절단 재설계 결정

## 1. 목표와 전제

- 관절 부근이나 넓은 부위를 잘라도 잘리지 않는 경계 띠와 구멍이 남지 않게 한다.
- 단일 절단과 다중 절단이 같은 규칙으로 끊을 관절(P 집합)을 고른다.
- 원본 메시는 break 뒤에도 rig로 skinning된다(메모 4절). 원본에 남은 삼각형은 자기 본 그룹의 몸체를 따라간다. A는 남은 몸체이고, B_i는 P_i와 그 자손으로 Dynamic이 된다.
- 그래서 **원본에 남겨도 되는 삼각형**은 "한 그룹에만 묶이고, 그 그룹이 놓여야 할 평면 쪽에 통째로 있는" 삼각형뿐이다. 나머지는 모두 복사해 잘라야 한다. 띠만 복사하면 P 본 살 중 부모 쪽에 있는 부분이 B_i를 따라 날아가 구멍이 생긴다(Codex 리뷰 blocker).

## 2. 결정 (2026-11-19 사용자 결정 Q1·Q2·Q4·Q5, Q3은 기술 판단)

### D1. P 집합은 뼈대만으로 고른다 (Q1)
- **후보:** 모든 rig owner의 `BuildKinematicSegments` 선분 중 유한 절단 사각형을 가로지르는 것을 모은다.
  - 이 함수는 그대로 쓴다. 포함되는 것: pose·constraint 조회, root 예외, RR-2 조건(선택 node가 Kinematic이고 incoming이 살아 있음), Dynamic 자식의 `AnimationFrame` 끝점.
  - 끝점 허용오차도 지금 값을 그대로 쓴다.
- **owner:** 교차점이 사각형 중심에 가장 가까운 후보의 owner로 정한다(지금 `SelectRigSegment`의 거리 규칙). 동률이면 기존 순서를 따른다. 이후 P는 그 owner 안에서만 고른다.
- **P 집합:** 아래 순서로 정한다.
  1. 그 owner의 교차 후보에서 부모 본 P를 모은다.
  2. 같은 P는 하나로 합친다.
  3. 조상이 집합에 있는 P를 뺀다.
- **거절과 D5:** 판정 순서는 다음과 같다.
  1. 교차가 있는데 모두 사용 불가(이미 끊김)면 지금처럼 `AlreadyBroken`으로 거절한다.
  2. root·1 mm 모호성 거절도 유지한다.
  3. 교차가 하나도 없을 때만 지금의 D5(가까운 Kinematic body 부착 패치)로 간다.
  - 물리 hit는 D5의 body 선택에만 쓴다.
- **스냅샷:** 선택 시 P마다 교차점과 ChildSide(자식 끝점이 있는 평면 쪽)를 job에 넘긴다. `DiagnoseIslands`·`RelabelCapLoops`는 끝점을 다시 계산하지 않고 이 값을 쓴다. 다시 계산하면 AnimationFrame 끝점과 어긋날 수 있다.
- P들의 ChildSide가 서로 다르면 요청을 거절한다(쪽-그룹 규칙 D3·D4가 성립하지 않음).
- 기본 C(Rig 모드)에만 적용한다. Parent·Influence는 비교 기준으로 둔다.

### D2. 다중 P도 유한 사각형 안에서만 고른다 (Q2)
- D1이 P를 1개 고르면 단일이고, 2개 이상이면 기존 다중 경로(union 추출, `QueueBreaks`, mask 1회, n+1 게시)를 쓴다.
- `SelectMultiCut`→`SelectCrossingJoints`(무한 평면)는 D1으로 대체한다.
- D1의 최상위 집합은 비중첩·중복 없음이 보장된다. 그래서 `BuildCutBodyGroups`의 nested·중복 거절에 걸리지 않는다.
- 같은 P의 자식 여러 개가 교차하면 P 하나, B_i 하나다. 그 loop들은 모두 B_i 쪽 loop다(D4).
- 복사본 절단은 지금처럼 무한 평면이다.

### D3. 패치 = 사각형에서 시작한 불안정 삼각형의 연결 성분 (Q3, 기술 판단)
- **추출 정책 플래그:** job에 추출 정책을 명시한다. `Band`는 Rig 단일·다중 절단에 쓰고, `Mask`는 D5·Parent·Influence에 쓴다. D5는 절단 없이 부착하므로 지금의 P weight 마스크 추출을 그대로 쓴다.
- **그룹 G(t):** 세 정점의 양수 weight 슬롯이 가리키는 본의 그룹 합집합이다. 그룹은 `NodeGroups`의 A, B_1, B_2, …를 **따로** 유지한다. B_1·B_2가 섞인 삼각형은 |G|=2다(2차 리뷰).
  - 비skin 삼각형은 `Band`에서 아예 뽑지 않는다(지금 `Selected`와 같다). 그 뒤 경로가 skin section만 받기 때문이다.
- **쪽:** 평면 부호다. ChildSide 쪽을 B 쪽, 반대를 A 쪽이라 한다. 판정은 기존 `Crossing`과 같은 eps 투영을 쓴다. 쪽 판정에서만 모든 B_i를 B 쪽으로 묶는다.
- **불안정(원본에 남기면 안 됨):** `Crossing || |G|>=2 || 불일치`다.
  - 불일치는 통째로 한쪽에 있는데 G가 그 쪽 그룹이 아닌 경우다. A 쪽인데 B_i만 있거나, B 쪽인데 A만 있는 경우다.
  - `Crossing`은 지금의 strict 판정이다. 평면에 닿기만 하는 삼각형은 불일치 판정으로 걸러진다.
- **패치 = 연결 성분:** 사각형과 겹치는(`Overlaps`) 불안정 삼각형을 씨앗으로 잡는다. 공유 변으로 이어진 불안정 삼각형으로 범위 제한 없이 넓힌다.
  - 그러면 패치와 원본이 맞닿는 변의 원본 쪽은 항상 안정(한 그룹, 제 쪽, 비교차)하다. 1차 blocker가 사각형 밖으로 새던 반례(A 쪽 B 전용 T–U 사슬)가 여기서 닫힌다.
  - 씨앗과 이어지지 않은 불안정 삼각형(예: 한쪽 다리 절단 때 무한 평면이 지나는 다른 다리)은 끌어오지 않는다.
- **계산 위치:** GPU Mode 0은 owner의 available skin 삼각형 중 불안정 후보와 씨앗 표시만 뽑는다(`available && skinned && 불안정`).
  - 연결 성분 필터는 readback 뒤 CPU에서 한다. 공유 변은 요청 snapshot의 IB와 위치 용접(같은 eps 양자화)으로 정한다. `BuildLatticeTopology::Edges`는 셀 쌍이라 쓰지 않는다.
  - **두 번 추출:** 첫 추출(후보)은 readback만 한다. CPU 필터 결과는 삼각형별 Keep 비트 버퍼(새 SRV t6)로 올린다. 둘째 추출은 Mode 0이 Keep 비트만 보고 기존 scan·Mode 2로 VB·IB·TriangleSources·개수를 다시 만든다.
    - `Poll`은 둘째 추출이 끝난 뒤에야 결과를 돌려준다(한 프레임 이상 지연). 그래서 절단 입력, 스키닝 출처, 원본 mask가 모두 같은 ID 목록을 쓴다(3차 리뷰).
    - 첫 추출의 VB·IB 출력은 버린다.
  - 성분이 owner available 삼각형의 50%를 넘으면 로그를 남기고 거절한다(평면이 몸을 크게 가로지르는 경우의 안전장치).
- **입력:** 그룹 표는 새 SRV **t5**로 넘긴다. 내용은 section 팔레트 순서의 본→{A, B_1, …, 무효}다.
  - t3는 Influence에서 팔레트→NodeId, 기본 경로에서 0/1로 이미 두 가지 의미로 쓰인다. 그래서 섞지 않는다.
  - root signature는 12→14 param(index 12=t5, 13=t6 Keep)이고, 24→28 DWORD다. `Submit`의 생성·전이·바인딩과 `Cancel` 정리를 함께 바꾼다.
  - 평면·ChildSide·사각형은 기존 Inputs/State 경로로 넘긴다.
- `ExtractPatchReference`와 `Validate`도 같은 후보 식을 쓴다. 연결 성분 필터는 CPU 한 곳에만 둔다.

### D4. 아일랜드 배정은 쪽으로 후보를 좁힌다 (Q4)
- 그룹→쪽 규칙: A는 A 쪽, 모든 B_i는 B 쪽이다.
- **loop에 닿은 아일랜드:** `RelabelCapLoops`가 B 쪽이면 그 loop를 만든 B_i, A 쪽이면 A로 둔다.
  - 같은 P의 loop가 여러 개면 모두 같은 B_i다. 지금의 "P당 loop 하나"를 "P당 loop 여러 개"로 넓힌다.
  - 서로 다른 P가 같은 loop를 요구하면 지금처럼 거절한다.
- **나머지 아일랜드:** `island.Side`로 후보를 정한다.
  - A 쪽이면 A로 둔다(투표 없음).
  - B 쪽이면 아일랜드 안에 weight가 있는 B_i끼리만 투표한다. 동률이면 본 번호가 작은 쪽이다. B_i 후보가 없으면 가장 가까운 loop의 B_i다.
  - 평면 위(Side 미정)는 Crossing·불일치에서 빠진 경우만 생기며, A로 둔다.

### D5. 봉합 진단은 CPU 검증에만 둔다 (Q5)
- 진단 옵션일 때만 CPU로 센다. 대상은 요청 시점 posed 좌표, available skin 삼각형, 전역 ID다. 공유 변은 D3과 같은 위치 용접으로 정한다. 두 수를 계산 시점이 다른 두 곳에 나눈다(2차 리뷰).
  1. **남은 불안정 수(`Validate`, 추출 검증 때):** 패치와 공유 변으로 맞닿은 원본 삼각형 중 불안정한 수다. 기대값은 0이다. GPU 후보가 CPU와 다를 때 원인을 나누는 데 쓴다.
  2. **봉합 불일치 수(`RelabelCapLoops` 뒤, 최종 재라벨 후):** 패치와 원본이 공유하는 변마다 비교한다. 패치 쪽 최종 아일랜드 그룹이 원본 삼각형의 단일 그룹과 다르면 센다. 기대값은 0이다.
- 0은 "원본과 패치가 같은 몸체를 따라간다"는 뜻이다. 절단면 cap 품질이나 봉합 정점 오차를 증명하지는 않는다. 그것은 SP7e2·PS4 오차로 따로 본다.
- GPU 셰이더에는 진단 계산을 넣지 않는다(RM-2와 같은 이유).

## 3. 범위 밖

- Parent·Influence 선택 규칙 변경
- 게시된 패치 조각을 합쳐 다시 자르기(SP-8)
- 복사본 절단의 유한화

## 4. 단계

| 단계 | 내용 | 예상 변경 | 예상 시간 |
|---|---|---|---|
| JC-1 | D1·D2 선택기: owner·P 집합·거절 순서·D5 조건, P별 ChildSide·교차점 스냅샷, `Request`·`SelectMultiCut` 대체. 추출은 아직 `Mask`. | 200~300줄 | 구현 + D/R 빌드 + 검사 + 리뷰 2~5분 |
| JC-2a | 추출 정책 플래그, t5 그룹 표, root signature, CPU 참조 후보 식 | 150~250줄 | 같음 |
| JC-2b | Mode 0 후보·씨앗 식, `Validate` 동기화, 남은 불안정 진단 | 150~250줄 | 같음 |
| JC-2c | CPU 위치 용접 변 표, 씨앗 연결 성분 필터, 50% 거절 | 150~250줄 | 같음 |
| JC-2d | Keep 비트 t6, 둘째 추출, `Poll` 반환 시점 이동 | 100~200줄 | 같음 |
| JC-3 | D4: 쪽 후보 투표, `RelabelCapLoops` P당 여러 loop, 스냅샷 사용, 봉합 불일치 진단 | 100~200줄 | 같음 |
| JC-4 | 측정·캡처·문서 동기화 | 문서 위주 | 캡처당 수 분 |

- JC-1은 JC-1a(`SegmentJointSelection.h` 순수 CPU 함수·`SkeletalPatchTests` 합성 사례), JC-1b(`SelectRigSegment`·`Request` 연결·demo 모드 전달), JC-1c(`SkeletalPatchJob`의 P별 world 교차점·ChildSide 저장, JC-3에서만 사용)로 나눈다.
- 단계마다 새 세션에서 진행한다. 순서는 Codex 구현 → D/R 빌드 → 검사 → 새 Codex 리뷰 → 로컬 커밋이다. 300줄이 넘으면 다시 쪼갠다.
- 단계마다 바뀌는 기대값은 다음과 같다.
  - **JC-1:** 바뀌는 것은 P·child·거절·게시 여부뿐이다. 구현 전에 아래 사례마다 예상 P 집합을 문서에 먼저 적는다.
    - MI-5 세 평면
    - 종아리→허벅지
    - 두 owner
    - Dynamic hit
    - root
    - 같은 P 복수 자식
  - **JC-2:** 패치 삼각형 수가 바뀐다. 기대값은 변경 전후 ID 차이와 독립 CPU 계산을 근거로 갱신한다.
  - `CheckJointUnion`의 중복 root 거절은 유지한다.
  - "union = 단독 추출 합집합"은 `Mask` 경로에서만 유지한다.
  - `Band`에서는 이 성질이 성립하지 않는다. 그룹이 요청의 P 집합에 따라 달라지기 때문이다(예: B₁ 전용 삼각형이 P₂ 단독 요청에서는 A다). 그래서 `Band` 다중 요청은 단독 합집합과 비교하지 않는다. 대신 D5 진단 2종 = 0과 P 집합 비중첩으로 검사한다. 숫자만 바꿔 통과시키지 않는다.

## 5. 완료 조건 (기능 전체)

1. D/R 빌드 오류 0, D3D12 ERROR 0.
2. 다음 실행에서 D5 진단 2종이 0이다.
   - MI-5 세 평면: CesiumMan 위팔 `Skeleton_arm_joint_L__4_`, 척추 `Skeleton_torso_joint_2`, 다중 허벅지 `leg_joint_R_1`
   - Xbot 종아리→허벅지 `-skeletal-recut-repro 1`
3. 같은 실행들의 `Validate` GPU==CPU가 PASS다.
4. 위 평면들은 게시 뒤 SP7e2·PS4 오차가 기존 한계 안이다(SP7e2 기존 FAIL은 변화만 기록). 게시 뒤 캡처를 남긴다. 경계 띠·구멍의 육안 확인은 사용자가 하며, 보지 않았으면 미확인으로 적는다.
5. 다음 동작이 유지된다.
   - 같은 부위 재요청 거절(`-skeletal-patch-break-checks 1`, Parent·Rig)
   - root `RigRoot` 거절
   - `AlreadyBroken` 거절
   - D5 경로(`Mask` 추출, 삼각형 수 불변)
6. JC-1의 사례별 예상 P 집합과 실제가 같다. 두 owner, Dynamic hit, ChildSide 불일치 거절 사례를 포함한다.
7. 기존 회귀 5종(`-skeletal-patch-checks`, `-skeletal-patch-break-checks`(Parent·Rig), MI-4h2 다중, MI-5 CesiumMan, `SkeletalPatchTests`)이 PASS다. 수가 바뀐 항목은 4절 규칙으로 갱신한다.
8. Influence 요청의 선택 node·추출 ID·수가 기존과 같다.

## 6. 위험

- **패치 수 증가:** 불일치 성분이 넓으면 B 살 일부가 더 복사된다. JC-4에서 PF-2 방식으로 수, GPU 시간, CPU 성분 필터 시간을 잰다.
- **CPU 필터 지연:** 연결 성분 필터와 용접 표가 readback 뒤 프레임에 CPU 시간을 더한다. 표는 owner별로 rest IB에서 한 번 만들고 재사용한다.
- **위치 용접:** eps가 크면 다른 살이 붙고, 작으면 UV seam에서 성분이 끊긴다. JC-2c에서 MI-5 모델로 확인한다.
- **ChildSide 불일치 거절:** 다중 절단에서 생각보다 자주 걸릴 수 있다. 자주 걸리면 P별 쪽 정의로 확장을 다시 결정한다.
- **live/Frozen 봉합:** 넓어진 경계의 정점 봉합 오차는 4번 조건으로만 본다.

## 7. JC-1 사례별 예상값

- 아래는 새 Rig 선택의 기대값이다. Parent·Influence 규칙은 그대로다. 게시 «예»는 후속 추출·절단 검사가 성공한다는 전제이며 새 구현 실행 결과가 아니다.
- 모든 등록 owner의 live·unavailable 선분을 본다. strict 반대 부호와 유한 범위 `|x|<=HalfWidth, |z|<=HalfHeight`를 쓴다. 물리 hit는 후보를 제한하지 않는다.
- 교차 0개면 D5의 `PublishRigBonePatch` 경로다. Kinematic hit로 부착 body를 고른다. 교차가 있지만 live가 0개면 `AlreadyBroken`이다.
- live 교차의 중심 거리→owner ID→P tuple로 owner를 고른다. 그 owner의 P를 중복 제거하고 rig 부모 사슬의 조상 P가 있으면 자손 P를 뺀다.
- 대표 child는 P별 중심 최단이다. 동률은 `Asset.Joints` 순서다. P마다 `RigRoot`→incoming `AlreadyBroken`→양 끝 모두 1 mm 이내 모호성을 검사하고, P 간 ChildSide가 다르면 거절한다. P 하나는 단일, 둘 이상은 옵션 없이 기존 union·QueueBreaks·mask 1회·n+1 게시다.
- 실모델 근거: `%TEMP%/JC1-expect/expect.cjs`로 GLB DFS 이름·양수 skin weight·가장 가까운 영향 조상·애니메이션 0 좌표를 계산했다. `.mini` 구의 배율과 mesh frame을 반영했다.
- MI-5는 f60≈1초 pose다. 기존 `%TEMP%/MI5-{UpperArm,Torso,Thigh,Thigh-Single}-patch.log`의 선택·교차점과 대조했다. 작은 사각형은 0.3×0.3m다.
- `-skeletal-recut-repro 1 -skeletal-recut-repro-mode rig`는 `%TEMP%/rr2/Release-x1-rig.log`의 f60/f109·실제 평면과 CPU 애니메이션 근사를 썼다. Dynamic 자식은 AnimationFrame이다. Dynamic 물리 pose 전체는 재현하지 않았다.

| 사례 | owner | 예상 P 집합 → P별 대표 child (이름/DFS ID) | 거절 / 게시 | 현재 대비 Rig 변화 |
|---|---|---|---|---|
| 1. MI-5 위팔 | CesiumMan(5) | {Skeleton_arm_joint_L__4_/7} → Skeleton_arm_joint_L__3_/8 | 없음 / 예, 단일 | 기존 Parent {7}과 같음. JC-1b 뒤 `-skeletal-joint-mode rig`로 실제 Rig 요청. |
| 2. MI-5 척추 | CesiumMan(5) | {Skeleton_torso_joint_2/3} → torso_joint_3/4 | 없음 / 예, 단일 | 기존 Parent {3}과 같음. 사각형 밖 양팔은 제외. |
| 3a. 오른 허벅지, union 교체 없음 | CesiumMan(5) | {leg_joint_R_1/17} → leg_joint_R_2/18 | 없음 / 예, 단일 | 기존 Parent 단일 {17}과 같음. Rig는 multi 옵션 없이도 유한 선택. |
| 3b. 오른 허벅지, union 교체 있음 | CesiumMan(5) | {leg_joint_L_1/13, leg_joint_R_1/17} → 각각 leg_joint_L_2/14, leg_joint_R_2/18 | 없음 / 예, 다중 n+1=3 | local Y=0.470281, 100×100 진단 평면. 기존 Parent+union+multi 경로를 Rig 자동 다중과 대조. |
| 4a. Xbot recut 1차 | Xbot(4) | {mixamorig:LeftLeg/61} → mixamorig:LeftFoot/62 | 없음 / 예, 단일 | 기존 RR-2 Rig와 같음. incoming JointId=44. |
| 4b. Xbot recut 2차 | Xbot(4) | {mixamorig:LeftUpLeg/60} → mixamorig:LeftLeg/61 | 없음 / 예, 단일 | 기존 RR-2 Rig와 같음. Dynamic child 61도 허용. incoming JointId=43. |

- 3b의 기존 실행은 `-skeletal-union-checks 1 -skeletal-multi-cut 1`인 Parent 진단이다. 새 Rig에서는 교체된 평면에 D1을 적용한다. `-skeletal-multi-cut` 없이도 다중이어야 한다. 1~3의 현재 demo는 Parent 상수이며 JC-1b에서만 모드 전달로 바뀐다. 기본은 Parent다.
- CPU 유한 범위 근거: 위팔의 다른 교차 3→4는 (x,z)=(-0.205,-0.432)m다. 척추의 7→8·10→11은 (0.351,0.285)·(-0.307,-0.287)m다. 오른 허벅지의 14→15는 (0.390,-0.776)m다. 모두 ±0.15m 밖이다.
- 3b는 교차가 13→14·17→18뿐이며 ChildSide=1로 같다. 4a의 반대 종아리는 (-0.169,-0.424)m로 밖이다. 4b의 60→61은 (-0.028,-0.041)m로 안이다.
- 4b의 다른 live 교차 중 가장 경계에 가까운 16→17(왼손 엄지)은 z≈0.15125m로 밖이다. 약 1.25mm 여유라 실제 요청 프레임·pose 차이에 민감하다. 게시 시점 변화 시 재계산한다. MI4d2의 2차 교점은 prepared pose 재계산값이므로 선택 snapshot 근거와 구분한다.

- 5~9는 JC-1a용 합성 입력이다. 평면 y=0, 중심=(0,0,0), x/z 반폭=0.15m다. 기본 owner A/10, R/0=(1,-0.2,0)→P/1=(0,-0.1,0)→C/2=(0,0.1,0)다.
- 기본은 모든 body Kinematic, incoming·pose 유효다. 명시한 선분 외 교차는 없다. hit는 비운다. 게시 기대는 Request 연결 후의 값이며 순수 CPU 테스트에서는 선택·거절만 단언한다.

| 사례·최소 변형 | owner | 예상 P 집합 → 대표 child | 거절 / 게시 | 현재 대비 Rig 변화 |
|---|---|---|---|---|
| 5. A/10은 P·C의 x=0.08, B/20은 x=0.02인 같은 3-node rig | B/20 | {P/1} → C/2 | 없음 / 예, 단일 | hit 없이도 B 선택. 거리 동률 변형은 owner ID가 작은 A/10. owner 간 P는 합치지 않음. |
| 6a. C/2만 Dynamic, AnimationFrame=(0,0.1,0), 물리 원점=(0,-0.3,0), hit=C만 | A/10 | {P/1} → C/2 | 없음 / 예, 단일 | 현재는 Dynamic hit만으로 live 후보를 못 고름. 새 규칙은 AnimationFrame 교차로 P 선택. |
| 6b. P/1·C/2 Dynamic, incoming 끊김, 물리 원점은 기본 좌표, hit=P만 | 없음(출처 A/10) | ∅ → 없음; unavailable=P/1→C/2 | AlreadyBroken / 아니오 | Dynamic 부모는 후보로 승격하지 않음. unavailable 교차가 있어 D5로 가지 않음. 기존 거절 유지. |
| 7. R/0=(0,-0.1,0)→C/1=(0,0.1,0)만 있는 root rig | A/10 | {R/0} → C/1 | RigRoot / 아니오 | hit 없이 root 교차도 수집한 뒤 Request에서 거절. 현재는 hit에 의존. |
| 8. 기본 P 아래 C/2=(-0.04,0.1,0), D/3=(0.04,0.1,0), Asset.Joints는 P→D가 먼저 | A/10 | {P/1} → D/3 | 없음 / 예, 단일 B 하나 | 동거리 자식은 작은 ID 대신 Asset.Joints 첫 항목. 현재도 동률 순서는 같지만 hit 없이 선택하며 여러 child를 한 P로 묶음. |
| 9. 기본 rig에 R→Q/3=(0.06,0.1,0)→D/4=(0.06,-0.1,0) 추가 | A/10 | {P/1,Q/3} → 각각 C/2,D/4 | ChildSide 불일치 / 아니오 | P·Q는 비중첩. R→Q 교차는 x≈0.373m로 밖. 현재는 단일 최단 P 또는 옵션 다중이며 전체 ChildSide 검사가 없음. |

- 정정: 4절의 «CheckJointUnion 중복 root 거절 유지»와 달리 현재 함수는 {first,second,first} 제출의 성공·중복 삼각형 0을 검사한다. 중복 root 거절은 `BuildCutBodyGroups`에 있다. 진단 의미는 유지한다.
