상태: 진행 중 (2026-11-25) — LB-1 완료(미커밋, 기준 `60c36279`), Release·회귀 LB 0/1 PASS. LB-2~4 미구현. 기본값 0, 렌더 연결 전.

# 잘린 본 팔레트를 남은 길이 비율로 따르기·고정 혼합 (개선 방안 4)

## 목표

몸통 쪽 A 조각에서 잘린 본 P의 스키닝 항을 남은 길이 비율 r로 둘로 나눈다. 가중치는 바꾸지 않는다.

v = Σ_k w_k·[ r_k·J_k(t) + (1−r_k)·F_k(t) ]·bind

- P는 r_k = r_P, P의 자손은 r_k = 0(F), 나머지 본은 r_k = 1(J)이다.
- 현재 애니메이션 따르기는 자손도 J로 따른다. 이 방식은 자손을 F로 바꾸므로 실제 동작이 달라진다(사용자 제안 그대로).

- J_P(t): 애니메이션 따르기 팔레트 C(t)·A_P(t)·O_P (`PatchAnimFollow.md`).
- F_P(t) = D(t)·JointsAtCut[P]: 부착 node 기준 절단 시점 고정 팔레트(`-patch-anim-follow 0`의 항). 월드 고정이 아니다.
- r = r_P (`PatchLengthWeights.md` D1과 같은 정의). P의 자손은 r = 0.
- r = 1이면 따르기 그대로, r = 0이면 NDC 고정 그대로다.

조사 메모: Codex 읽기 전용 조사 1건(2026-11-25). 요점은 아래 결정에 반영했다.

## 결정

D1. 본마다 팔레트 한 칸을 혼합한다
- r은 정점이 아니라 본마다 하나다. 그래서 M_k = r_k·J_k + (1−r_k)·F_k를 본마다 한 번 만들어 기존 팔레트 칸에 넣는다.
- 위치는 제안식과 정확히 같다(행렬에 대해 선형).
- 정점 influence 4칸, 가중치, 인덱스, bind, VB 형식을 바꾸지 않는다. 팔레트 크기(jointCount+1)와 K 번호도 그대로다.
- 슬롯을 하나 더 쓰는 방식은 쓰지 않는다. CesiumMan 정점 약 12.8%가 이미 4칸을 다 쓴다.

D2. 법선 행렬은 따로 가중합한다
- NrmMatrix_k = r_k·IT(J_k) + (1−r_k)·IT(F_k). 셰이더가 행렬을 먼저 가중합한 뒤 한 번 정규화하므로, 두 슬롯에 w_k·r_k와 w_k·(1−r_k)를 주는 LBS와 같은 결과다.
- 혼합 위치 행렬 M_k의 역행렬은 쓰지 않는다. M_k는 강체가 아니고 특이할 수 있다.
- cap 법선(CutTimeNormal)은 현재 팔레트의 NrmMatrix로 방향을 스키닝하므로 그대로 따라온다. 절단 순간에는 D = I, J = F = JointsAtCut이라 위치·법선 행렬 모두 기존 값과 같고(특이 폴백·10비트 압축 오차 범위 안), 기존 cap 기록을 그대로 쓴다.
- 법선 가중합이 영벡터나 비유한 값이 되는지 진단에서 검사한다.

D3. 옵션
- `-patch-length-blend 0|1`, 기본 0.
  - 0: 현재 동작 그대로.
  - 1: 자손 r = 0(F), P는 r_P 혼합, 나머지 r = 1.
- 기존 따르기 판정(전역 옵션, `snapshot.AnimFollow`, 애니메이션 입력, offset 유효성)이 참일 때만 적용한다. 아니면 기존 고정 경로를 쓴다. B 조각·boneCut·Frozen 처리 규칙은 그대로다.
- `-patch-length-weights`가 0이 아니면서 함께 쓰면 오류로 끝낸다. 두 방식은 비교용으로 따로 둔다.

D4. 재사용과 수명
- LW의 r_P 계산(월드 위치 길이비, 교점 재계산, 자손 분류, NodeId→팔레트 매핑)만 가져온다. 가중치 재분배·bind 역산·재양자화·끝점 재분배는 쓰지 않는다.
- LW는 표 생성이 `PatchLengthWeightsMode()!=0`에, P의 r_P가 모드 2에 묶여 있다. 비율 계산을 이 조건에서 분리해 LB 옵션만으로도 r 표(P = r_P)가 만들어지게 한다.
- r 배열은 매 프레임 필요하므로 PatchSkinSnapshot에 저장한다. 재절단의 복사 레코드는 snapshot 값 복사로 그대로 승계한다. 새 재절단 cap은 끝점 보정 방향을 0으로 기록해 K 법선을 유지하고, internal 끝점·다중 section cap에는 폴백이 있다(기존 한계). LB-3에서 이 경로를 따로 검증한다.
- 물리가 부착 node를 덮는 경우(C≠I)도 J와 F를 각각 기존 식으로 만든 뒤 섞는다. C나 D를 다시 곱하지 않는다.

D5. 알려진 한계와 확인할 것
- 짧은 그루터기(r 작음)는 절단 순간 각도에 가깝게 굳는다. NDC 문제가 (1−r)만큼 남는다.
- 혼합 행렬은 두 회전 차이가 클 때(180°에 가까움) 부피가 줄 수 있다. 진단에서 행렬식 최솟값을 기록한다.
- 원본 메시와 붙는 경계 정점에도 P·자손 가중치가 있으면 경계가 벌어질 수 있다. SP7e2로 먼저 측정하고, 실패하면 경계 보완을 별도 단계로 나눈다.

## 단계 (각 약 300줄 이내)

| 단계 | 범위 | 완료 조건 |
|---|---|---|
| LB-1 | 옵션, LW 비율 계산을 LW 모드에서 분리해 이식, snapshot 저장, LW와 동시 사용 오류. 렌더 연결 전 | 단일·다중·잎·중복 joint의 r 로그, 옵션 0 불변 |
| LB-2 | BuildPatchPalette 위치 혼합(D1)·법선 가중합(D2). 진단 입력(`RecutCheckInput`, getter)에 실제 법선 팔레트 전달, `CheckAnimFollowFrame`이 혼합 기대값을 쓰게 변경 | r = 0/1/중간·절단 순간·C≠I에서 식 일치, 유한값, 행렬식·법선 영벡터 검사 |
| LB-3 | 끝점·cap·재절단 진단을 새 팔레트에 맞춤 | rim/cap 위치, cap 면 법선 독립 검사, snapshot 승계, 새 재절단 cap·폴백 경로 |
| LB-4 | SP7e2 경계, 회귀 6종, 캡처, 문서 | LB 켬/끔 × follow 켬/끔 회귀 PASS, LW 1·2 동시 사용 오류 확인, CesiumMan·Xbot 캡처 |

## LB-1 구현

- `PatchLengthBlendEnabled`가 옵션을 한 번 읽고 LW와 동시 사용을 거부한다. `ModelViewer::Startup`에서 렌더 준비 전에 호출한다.
- `BuildRemainLengthTable`은 LW 모드와 무관하게 P=r_P인 표를 만든다. `SkeletalPatchCopy::Request`의 LW 모드 0·1 처리는 P만 1로 되돌려 기존 표를 보존한다.
- LB가 켜지면 몸통 snapshot의 `LengthBlendRatio`에 원래 표를 값 복사한다. `BuildRecutPatchSkinVertices`의 snapshot 값 복사로 승계한다.
- `LB1` 로그의 `paletteEntries`와 `duplicates`는 같은 P의 section 중복 팔레트 수이며 `match`는 저장된 P 비율의 일치를 검사한다.
- `BuildPatchPalette`, 가중치·bind·GPU 입력은 변경하지 않았다. D1·D2의 실제 혼합과 D5 진단은 후속 단계다.

### LB-1 검증 (2026-11-25)

- Release x64 `ModelViewer.slnx -p:Configuration=Release -p:Platform=Windows -m`: 성공, 84.72초, 오류 0. 로그 `%TEMP%/lb1-release-build.log`.
- 지정 `regress.sh Release`와 `regress.sh Release -patch-length-blend 1`: 각각 6종 PASS, 모든 프로세스 exit=0. SP6 20행 PASS, SP7e2 pass=4 fail=0 n/a=2, BC-12a/b·AR4·Rig PASS. 실행별 6~10초. 로그 `%TEMP%/lb1-regress-default.txt`, `lb1-regress-enabled.txt`와 `lb1-default-patch.log`, `lb1-enabled-patch.log`.
- 요청한 CesiumMan `leg_joint_R_1`, fraction 0.35, cut-time 0.5: exit=0, `LB1 P=17 r_P=0.3500 descendants=3 paletteEntries=1 duplicates=0 match=PASS`. 로그 `%TEMP%/lb1-leg-on-patch.log`.
- 같은 다리 데모에 `-skeletal-multi-cut 1`: exit=0, P=14 r_P=0.4256 자손 2, P=17 r_P=0.3500 자손 3, 둘 다 match=PASS. 조각 3개 게시. 로그 `%TEMP%/lb1-multi-leg.log`.
- 잎·중복 section NodeId는 실제 `BuildRemainLengthTable`과 구조체를 추출한 외부 CPU fixture에서 검증했다(실제 모델 절단 검증과 구분). P=1 r_P=0.3500 자손 1, 잎 P=3 r_P=1.0000 자손 0, 각각 paletteEntries=2 duplicates=1 PASS. 기존 `60c36279` 함수와 LW 모드 0/1/2 표의 비트 비교 303개 PASS. 소스·결과 `%TEMP%/lb1-ratio-tests.cpp`, `lb1-ratio-tests.txt`.
- LB 0/1의 고정 16ms 다리 데모 게시 직후·60프레임 후 post RAW 두 쌍은 SHA-256이 각각 일치했다. `%TEMP%/lb1-leg-{off,on}-demo-{000,001}-post.raw`. 렌더 미연결 상태를 확인했다.
- `-patch-length-blend 1`과 `-patch-length-weights 1` 및 `2`는 각각 시작 시 예외 종료(exit=-1073740791). 오류는 `patch-length-blend 1 cannot be combined with nonzero patch-length-weights`이다.
- 추가 시도한 `-skeletal-patch-break-checks 1 -skeletal-multi-cut 1` 조합은 `-skeletal-union-checks 1` 유무 모두 비정상 종료했다(exit=-1073740791). LB=0에서도 동일하게 재현되어 이번 범위에서 수정하지 않았다. 필수 회귀와 위 다중 절단 데모는 정상 종료했다.
