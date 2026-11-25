상태: 측정 기록(이 PC) + 다른 PC 측정 도구 추가 — 기준 HEAD `09dca0ed` + 미커밋 계측(`-cut-perf-log`), 2026-11-25

# 단일 본 절단 최종 성능 측정 (Xbot·CesiumMan 오른쪽 허벅지)

발표용 수치다. 각 수치가 무엇을 재는지, 표본 수, 재지 않은 것을 함께 적는다. 해석이 필요한 곳은 "읽는 법"에 따로 적었다.

## 1. 환경과 조건

| 항목 | 값 |
|---|---|
| CPU | 11th Gen Intel Core i5-1135G7 (Win32_Processor) |
| GPU | NVIDIA GeForce MX450, 전용 VRAM 1906 MiB (계측 로그 `ENV` 행). Intel Iris Xe도 있으나 엔진은 MX450을 선택 |
| OS | Windows 10 Home 10.0.19045 |
| 빌드 | Release / Windows→x64, VS2022 MSBuild. HEAD `09dca0ed`에 계측만 얹은 미커밋 상태 |
| 실행 옵션 | 모든 실행 `-debug 0`(D3D12 디버그 레이어 꺼짐). V-Sync는 기본값(켜짐, 프레임 간격 16.66 ms 고정). 디버거 미연결 |
| 패치 옵션 | 기본값: 따르기 1, 길이 가중치 0, 길이 혼합 0 (옵션을 주지 않음) |
| 백그라운드 | Steam, Chrome(약 10개 프로세스), Codex, 유휴 MSBuild 노드 1개(내가 띄운 것 아님)가 떠 있었다. 측정 중 MSBuild·ModelViewer 빌드/다른 실행 없음. 전원·온도·GPU 클럭은 통제하지 않았다 |
| 장면 | 데모 기본 장면: Cube, Bunny, Dragon, Xbot, CesiumMan이 모두 로드됨 (`-skeletal-break-demo`가 항상 기본 장면을 만든다) |

## 2. 모델 통계

출처: `MiniEngine/ModelViewer/Models/*.glb`의 JSON 청크와 accessor/bufferView 직접 파싱(PowerShell). 영향 joint는 JOINTS_0·WEIGHTS_0 바이너리를 읽어 가중치가 0보다 큰 서로 다른 joint 번호를 센 값이다. rig joint/body는 엔진의 RigPreload 로그(`RIGPRELOAD` 행).

| 항목 | Xbot | CesiumMan |
|---|---:|---:|
| 파일 크기 (B) | 2,930,032 | 438,044 |
| node 수 | 70 | 22 |
| mesh 수 / primitive(=draw) 수 | 2 / 2 | 1 / 1 |
| skin 수 / skin joint 수(glb) | 1 / 67 | 1 / 19 |
| 가중치 > 0인 영향 joint 수 | 52 | 19 |
| RigPreload joint / body | 51 / 52 | 18 / 19 |
| 정점 수 | 28,374 (12,473 + 15,901) | 3,273 |
| 인덱스 수 / 삼각형 수 | 147,336 / 49,112 | 14,016 / 4,672 |
| 애니메이션 | 7개 (데모 기본 clip 0 `agree`, 1.833 s) | 1개 (2.0 s) |
| 절단 본 | `mixamorig:RightUpLeg` → 자식 node 사이의 중점 | `leg_joint_R_1` → 자식 node 사이의 중점 |
| 추출된 패치 삼각형 수 | 2,096 (원본의 4.3%) | 352 (원본의 7.5%) |

패치 삼각형 수는 `%TEMP%\SP6-SkeletalPatchChecks.log`(과거 기록이 누적된 파일, 사본 `perf\SP6-SkeletalPatchChecks-cumulative.log`)의 `Skeletal patch published` 행이다. Xbot은 이번 실행들에서 2096이 대부분이었고(24회), 다른 값은 과거 진단 실행의 것이다.

## 3. 측정 방법

### 3.1 추가한 계측 (옵션 `-cut-perf-log <파일 경로>`, 기본 꺼짐)

로그 파일 경로를 값으로 받는다. 옵션이 없으면 모든 계측 호출은 즉시 반환한다(`CutPerf::Enabled()`가 false). 요청한 `-cut-perf-log 1` 대신 경로를 받은 이유는 디버그 출력 수집기(디버거 연결)가 CPU 시간을 크게 늘리기 때문이다. 이전 기준 측정(PerfBaseline)에서 같은 코드 경로가 수집기 아래 수 초가 걸렸다.

| 파일 | 변경 |
|---|---|
| `Scene/CutPerfLog.h` (신규, 헤더 전용) | 로그 파일 열기, 구간 시각 `Mark`, CPU 범위 `Cpu`, GPU timestamp 범위 `Gpu`(펜스 완료 후 readback), EngineProfiling 블록 래퍼 `Block`, 프레임별 로그 `LogPreviousFrame`, DXGI 비디오 메모리 조회, 버퍼 크기 `Memory` |
| `ModelViewer.cpp` | `Update` 시작에서 직전 프레임 행 기록·GPU 쿼리 회수, Update·RenderScene의 CPU 시간 측정, 블록 5개(`CP Scene Update`, `CP Render`, `CP Depth`, `CP Shadow`, `CP Color`)를 기존 ScopedTimer 옆에 추가, `DEMOSTART` 행, `-cut-perf-only-target 1`(대상 외 오브젝트의 `PublishVisible=false`로 그리기만 생략) |
| `Scene/SkeletalPatchCopy.cpp` | `Request` 시작(`BeginRequest`)·선택 완료·추출 제출 후, 추출 폴링·복사 준비·절단 제출·결과 수신·`FinishCutResult` 시작·break 결과·게시 완료 `Mark`, OnCutResult 분류·압축 제출 CPU, FinishCutResult CPU, 절단기가 이미 재던 GPU 단계 값(`CUTTER` 행), 추출 GPU(전체+Mode 0/1/2/6), 분류·압축 GPU, 조각 스키닝 타이머 이름 지정, 게시 직후 상주 버퍼 크기, RigPreload 시간 |
| `Scene/PatchSkinning.cpp` | 조각 skin VB 생성 GPU 구간 |
| `Scene/MeshComponent.cpp` | `SetTriangleMask` CPU 구간, 원본 마스크 압축 IB 생성 GPU 구간, 조각 스키닝 블록 타이머를 `-cut-perf-log`에서도 켜는 조건 |

기존 GPU 타이머를 우선 썼다: 절단기의 `StageMs`(clip·boundary·cap·finalize)는 기존 `CutResultEntry` 값을 로그에 옮겼고, 프레임별 GPU 시간은 기존 `EngineProfiling` 블록 방식(타임스탬프 쌍)을 Release에서도 켜서 얻었다. 그 외 추출·분류·압축·스키닝 빌드는 새 timestamp 쌍이다.

### 3.2 실행 프로토콜

실행 파일: `MiniEngine\Build\x64\Release\Output\ModelViewer\ModelViewer.exe`, cwd `MiniEngine\ModelViewer`. 실행마다 새 프로세스다.

- **A. 절단 비용** (모델당 10회, 별도 실행): `-debug 0 -skeletal-break-demo N -skeletal-break-demo-bone <본> -skeletal-break-demo-delay 3 -physics-query-exit 700 -cut-perf-log ...`. 절단 요청은 데모 기본값대로 시작 후 59프레임째다.
- **B. 렌더 전후** (모델당 완전 장면 3회 + 대상만 3회): `-skeletal-break-demo-delay 7 -skeletal-break-demo-cut-time 8.3`(Xbot) 또는 `8.98`(CesiumMan) `-physics-query-exit 1550`. `cut-time`은 절단 전 애니메이션 프레임을 300개 이상 얻기 위해 쓴다. 값은 "기본 요청 시점의 재생 위상(약 0.98 s) + 클립 길이×4"로 맞췄다(Xbot 1.833×4, CesiumMan 2.0×4). 위상 일치는 가정이며 실행 로그로 검증하지 않았다. 대상만 실행에는 `-cut-perf-only-target 1`을 추가했다.
- 모든 실행에서 `exit=0`이고 게시(`published`)가 확인됐다(`perf\runs.txt`).

### 3.3 구간 정의 (절단 단계)

모든 시각은 `SkeletalPatchCopy::Request` 진입(`REQUEST` 행)을 0으로 하는 `steady_clock` 벽시계(ms)다. "CPU"는 해당 함수 안의 벽시계라 GPU 완료 대기·파일 I/O(기존 `PatchLog`의 `fopen` 추가 기록)를 포함한다. "대기"는 다음 프레임 경계나 GPU·스레드 완료를 기다린 시간이며, 같은 프레임의 렌더·물리 등 다른 작업도 그 안에 들어 있다.

| 단계 | 정의 | 종류 |
|---|---|---|
| 본 선택 | Request 시작 → `select-done` (평면·본 선택, skin snapshot·팔레트 마스크 준비) | CPU |
| 추출 제출 | `select-done` → `extract-submitted` (`SkeletalPatchJob::Submit`: 버퍼 생성, 명령 기록, 제출) | CPU |
| 추출 GPU 대기 | `extract-submitted` → `extract-polled` (fence 완료 후 readback 처리 포함) | 대기 |
| 복사 준비 | `extract-polled` → `copy-prepared` (카운터 처리, 복사 owner 생성, pose·bounds 준비) | CPU |
| 절단 제출 대기 | `copy-prepared` → `cut-submitted` (다음 프레임에서 Static 절단 요청) | 대기 |
| 절단기 지연 | `cut-submitted` → `cut-result-received` (clip·boundary·cap·finalize GPU와 readback) | 대기 |
| 몸체 분류·압축 제출 | `OnCutResult` 안에서 RelabelCapLoops, 분류, 압축 제출까지 | CPU |
| 몸체 GPU 대기 | OnCutResult 끝 → `finish-start` | 대기 |
| FinishCutResult | skin VB 빌드(GPU 완료 대기 포함, 조각 두 개), 준비물 ID, break 제출 | CPU |
| break(물리) 대기 | FinishCutResult 끝 → `break-result` (물리 substep이 BreakConstraint를 적용) | 대기 |
| 게시 | `break-result` → `published` (조각 2개 commit, `SetTriangleMask`, `PrepareMeshes`) | CPU |

## 4. 결과 1 — 절단 비용 (요청 → 조각 게시) [이 PC(MX450), 첫 절단·진단 로그 포함, GPU 타이머 켬, 전체 장면]

Xbot은 `-skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:RightUpLeg`, CesiumMan은 `-skeletal-break-demo 2 -skeletal-break-demo-bone leg_joint_R_1`, 각 10회 별도 실행.
표의 열: **1회차** = 배치의 첫 실행, **평균·최소·최대** = 2~10회차 9개 표본.

### 4.1 CPU·대기 구간 (ms)

Xbot

| 단계 | 1회차 | 평균 | 최소 | 최대 |
|---|---:|---:|---:|---:|
| 본 선택 (CPU) | 2.72 | 3.11 | 2.21 | 4.84 |
| 추출 제출 (CPU) | 25.36 | 24.52 | 17.42 | 37.03 |
| 추출 GPU 대기 | 5.24 | 4.32 | 1.57 | 7.66 |
| 복사 준비 (CPU) | 21.90 | 24.68 | 17.46 | 37.33 |
| 절단 제출 대기 | 14.97 | 12.12 | 8.81 | 15.68 |
| 절단기 지연 | 4.14 | 6.65 | 4.85 | 8.06 |
| 몸체 분류·압축 제출 (CPU) | 25.96 | 25.29 | 17.78 | 31.87 |
| 몸체 GPU 대기 | 3.02 | 4.61 | 3.43 | 6.41 |
| FinishCutResult (CPU) | 28.21 | 43.22 | 37.94 | 50.76 |
| break(물리) 대기 | 3.31 | 4.65 | 3.25 | 7.01 |
| 게시 (CPU) | 4.09 | 7.40 | 4.58 | 10.95 |
| 　└ 그중 `SetTriangleMask` | 1.47 | 2.57 | 1.52 | 4.45 |
| **요청→게시 합계** | **138.99** | **160.72** | **129.38** | **211.88** |
| 걸린 프레임 수 (Graphics 프레임 차) | 4 | 4.33 | 4 | 5 |

CesiumMan

| 단계 | 1회차 | 평균 | 최소 | 최대 |
|---|---:|---:|---:|---:|
| 본 선택 (CPU) | 2.03 | 2.97 | 1.62 | 8.41 |
| 추출 제출 (CPU) | 18.84 | 25.88 | 21.02 | 47.58 |
| 추출 GPU 대기 | 3.62 | 5.33 | 2.54 | 9.01 |
| 복사 준비 (CPU) | 9.32 | 7.60 | 5.04 | 12.15 |
| 절단 제출 대기 | 9.35 | 13.80 | 8.53 | 19.32 |
| 절단기 지연 | 12.62 | 7.96 | 4.67 | 13.04 |
| 몸체 분류·압축 제출 (CPU) | 26.71 | 29.52 | 26.80 | 32.52 |
| 몸체 GPU 대기 | 8.83 | 4.80 | 2.71 | 8.58 |
| FinishCutResult (CPU) | 64.98 | 43.56 | 37.20 | 53.11 |
| break(물리) 대기 | 6.23 | 4.89 | 3.62 | 7.51 |
| 게시 (CPU) | 8.50 | 6.84 | 4.07 | 9.36 |
| 　└ 그중 `SetTriangleMask` | 2.39 | 2.41 | 1.28 | 3.68 |
| **요청→게시 합계** | **171.15** | **153.31** | **136.41** | **182.41** |
| 걸린 프레임 수 | 5 | 4.22 | 3 | 5 |

읽는 법
- 합계는 약 130~210 ms, 4~5프레임이다. 구간 대부분이 CPU 구간이다(CPU 구간 합 평균: Xbot 약 128 ms, CesiumMan 약 116 ms). 나머지는 "대기" 행의 합(Xbot 약 32 ms, CesiumMan 약 37 ms)이며, 이는 GPU 완료와 다음 프레임 경계를 기다린 시간이고 같은 프레임의 렌더·물리 CPU 작업도 들어 있다.
- CesiumMan은 정점이 Xbot의 1/9인데 CPU 합이 비슷하다. 큰 구간(추출 제출, 분류·압축 제출, FinishCutResult)이 모델 크기와 거의 무관한 고정 비용(버퍼·PSO·readback·동기 Finish)에 가깝다는 뜻이다. 구간 안의 세부 원인은 이번에 나누지 않았다.
- 1회차가 평균보다 반드시 큰 것은 아니다(Xbot 139 vs 161 ms). 이 "cold"는 진짜 cold가 아니다. 배치 직전에 빌드와 시험 실행이 있었기 때문에 OS 파일 캐시·드라이버 셰이더 캐시가 이미 데워져 있었다. 완전한 cold 시작은 재지 못했다.
- 각 실행은 프로세스의 첫 절단이다. 절단 경로에서 지연 생성되는 PSO·static 초기화도 모두 포함한다.

### 4.2 GPU 구간 (µs, 해당 명령 기록 구간의 timestamp 차)

각 값은 "구간 전체"(구간 안의 clear·copy·barrier 포함, 구간 시작 직전 barrier는 flush해 제외). 같은 이름이 여러 번 나오면 실행당 합계다.

Xbot

| 구간 | 1회차 | 평균 | 최소 | 최대 |
|---|---:|---:|---:|---:|
| 추출 전체 (Submit 명령 목록 전체) | 351.9 | 362.8 | 348.1 | 454.3 |
| 　└ Mode 0+1+6+2 dispatch 합 | 119.9 | 117.5 | 109.3 | 125.1 |
| 절단기 GPU 합 (clip+boundary+cap+finalize) | 260 | 296 | 262 | 344 |
| 　└ clip / boundary / cap / finalize 평균 | 61 / 70 / 85 / 44 | 73 / 77 / 98 / 48 | | |
| 몸체 분류 (A·B 두 번 합) | 36.0 | 38.8 | 34.0 | 48.4 |
| 몸체 압축 | 114.3 | 125.0 | 113.8 | 142.0 |
| 조각 skin VB 빌드 (조각 2개 합) | 638.4 | 637.4 | 631.6 | 640.4 |
| 원본 마스크 압축 IB 생성 | 130.9 | 127.2 | 111.0 | 172.0 |
| **GPU 합 (위 6개 구간, 평균)** | | **약 1,590** | | |

CesiumMan

| 구간 | 1회차 | 평균 | 최소 | 최대 |
|---|---:|---:|---:|---:|
| 추출 전체 | 110.0 | 99.8 | 90.4 | 108.6 |
| 　└ Mode 0+1+6+2 dispatch 합 | 55.9 | 48.0 | 42.5 | 57.4 |
| 절단기 GPU 합 | 295 | 159 | 135 | 250 |
| 몸체 분류 (두 번 합) | 41.7 | 39.6 | 36.9 | 42.5 |
| 몸체 압축 | 64.7 | 57.5 | 50.3 | 61.9 |
| 조각 skin VB 빌드 | 122.4 | 122.9 | 121.2 | 126.3 |
| 원본 마스크 압축 IB 생성 | 20.5 | 17.2 | 14.7 | 25.4 |
| **GPU 합 (평균)** | | **약 500** | | |

읽는 법: 절단 전체의 GPU 비용은 Xbot 약 1.6 ms, CesiumMan 약 0.5 ms로, 요청→게시 130~210 ms에 비하면 1% 안팎이다. 지연은 GPU가 아니라 CPU·동기·프레임 경계에서 나온다. Xbot의 skin VB 빌드(0.64 ms)가 GPU 구간 중 가장 크다. 절단기 GPU 합의 CesiumMan 1회차 295 µs는 이후 평균 159 µs보다 크다(첫 실행 효과로 보이나 표본 1개).

### 4.3 절단이 걸친 프레임의 끊김 (Xbot·CesiumMan 각 10회)

요청 프레임부터 게시 프레임까지(포함, 5~6프레임)의 `ModelViewer::Update` CPU 시간과 프레임 간격(wall)이다. 기준 프레임 간격은 요청 직전 60프레임 평균(16.66 ms)이다.

| 항목 | Xbot 평균 (최소~최대) | CesiumMan 평균 (최소~최대) |
|---|---:|---:|
| 구간 프레임 수 | 5.3 (5~6) | 5.3 (4~6) |
| 가장 긴 한 프레임의 Update CPU (ms) | 68.0 (43.7~91.4) | 71.7 (45.9~88.3) |
| 구간의 Update CPU 합 (ms) | 178.8 (151.9~235.9) | 151.8 (134.8~181.5) |
| 기준 대비 늘어난 프레임 시간 합 (ms) | 99.9 (63.4~162.3) | 75.2 (49.4~105.3) |

읽는 법: 절단 한 번에 60 Hz 프레임 약 4~6개 분량의 지연이 생기고, 가장 긴 프레임 하나가 45~90 ms다(예: Xbot 5회차는 70 ms, 12 ms, 24 ms, 45 ms로 이어짐). 한 프레임에 몰리는 구간은 FinishCutResult(약 43 ms)와 분류·압축 제출(약 25 ms)·추출 제출(약 25 ms)이다.

### 4.4 물리·래그돌 준비

- 래그돌 rig는 절단 전에 이미 등록되어 있다(RigPreload). 시작 시 한 번의 비용이며 절단 요청 비용에 들어 있지 않다. Xbot 10회 실행 로그의 `create_register_ms`: Xbot 19.5~42.1 ms, CesiumMan 5.6~98.0 ms(실행 간 편차 큼).
- 절단 시점의 물리 쪽 비용은 break 제출(FinishCutResult 안에 포함)과 break 결과 대기(위 표의 "break(물리) 대기", 평균 4.7~4.9 ms, 다음 프레임 경계 포함)다. 물리 substep 내부의 BreakConstraint 처리 시간 자체는 따로 재지 않았다.

## 5. 결과 2 — 렌더 비용 절단 전·후 [이 PC(MX450), 이전 계측 빌드, 기본 카메라]

### 5.1 정의

- 프레임별 한 줄(`FRAME`)은 직전 프레임의 값이다. CPU는 `Update`·`RenderScene` 함수의 벽시계 시간(ms), GPU는 EngineProfiling 블록의 timestamp 쌍이다. GPU 블록 값은 읽기 지연 때문에 같은 프레임 번호의 실제 GPU 작업보다 1~2프레임 어긋날 수 있다. 윈도우가 길어 평균에는 영향이 없다.
- `GPU 합` = `CP Scene Update` GPU(pose 준비·조각 스키닝) + `CP Render` GPU(깊이 선행+그림자+색+SSAO·후처리 일부). `Render UI`·`PostEffects`·`Present`는 포함하지 않는다.
- `CPU 합` = `Update` 함수 + `RenderScene` 함수의 벽시계. 프레임 간격(wall)은 V-Sync 때문에 항상 16.66 ms라서 비교에 쓸 수 없다(모든 조합이 16.659~16.671).
- 윈도우: `정지` = 시연 지연 구간(포즈 고정, 물리 0 step), `전` = 애니메이션 시작+15프레임 ~ 요청 직전, `후` = 게시 +150프레임 ~ 실행 끝(절단 후 안정). 프레임 수는 실행당 `정지` 약 378, `전` Xbot 481·CesiumMan 521, `후` Xbot 약 474·CesiumMan 약 435. 모두 300 이상이다.
- 표본: 조합당 3회 실행. 표는 3회 실행 평균의 평균이고 괄호는 3회 실행 평균의 최소~최대다.
- `전`과 `후`는 모두 애니메이션 중이므로 직접 비교한다. `정지`는 포즈 고정이라 그림자 pass 값이 다르다(참고용).

### 5.2 완전 장면 (Cube·Bunny·Dragon·Xbot·CesiumMan 모두 그림)

| 대상·구간 | GPU 합 (ms) | Scene Update GPU | Render GPU | 깊이 | 그림자 | 색 | CPU 합 (ms) | 조각 스키닝 GPU / CPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Xbot 정지 | 7.663 (7.568~7.807) | 0.107 | 7.556 | 1.555 | 1.732 | 3.058 | 1.183 | — |
| Xbot 전 | 8.753 (8.460~8.916) | 0.123 | 8.630 | 1.546 | 2.430 | 3.305 | 2.564 (2.440~2.661) | — |
| Xbot 후 | 9.011 (8.722~9.265) | 0.175 | 8.836 | 1.560 | 2.565 | 3.231 | 2.760 (2.709~2.837) | 0.043 / 0.067 |
| CesiumMan 정지 | 7.631 (7.538~7.685) | 0.108 | 7.523 | 1.555 | 1.617 | 3.095 | 1.091 | — |
| CesiumMan 전 | 8.937 (8.853~9.036) | 0.127 | 8.811 | 1.548 | 2.472 | 3.337 | 2.670 | — |
| CesiumMan 후 | 9.096 (9.035~9.129) | 0.165 | 8.931 | 1.542 | 2.554 | 3.293 | 2.812 | 0.029 / 0.053 |

읽는 법: 완전 장면에서는 Dragon 등 다른 오브젝트가 GPU 시간의 대부분(약 7~8.5 ms)을 차지한다. 전→후 GPU 합 증가는 Xbot +0.26 ms(+2.9%), CesiumMan +0.16 ms(+1.8%)지만, 같은 조건 3회 실행 간 범위(전 8.46~8.92)가 증가분보다 넓다. 따라서 이 장면에서 절단 전후 GPU 차이는 잡음과 구별되지 않는다. 그림자 pass는 정지/애니메이션 상태에서 1.5 ms와 2.7 ms 두 값 사이를 오가는 변동이 있다.

### 5.3 대상 캐릭터만 그림 (`-cut-perf-only-target 1`)

대상 외 오브젝트는 그리기만 생략했고 pose 준비·갱신은 그대로다(그래서 `Scene Update GPU`·CPU 합에 다른 모델의 갱신이 남아 있다). Render GPU의 약 1.3~1.4 ms는 하늘·SSAO·후처리 등 캐릭터와 무관한 고정 비용이다.

| 대상·구간 | GPU 합 (ms) | Scene Update GPU | Render GPU | 깊이 | 그림자 | 색 | CPU 합 (ms) | 조각 스키닝 GPU / CPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Xbot 정지 | 1.485 | 0.105 | 1.380 | 0.0389 | 0.0821 | 0.0551 | 1.002 | — |
| Xbot 전 | 1.556 (1.540~1.568) | 0.121 | 1.435 | 0.0381 | 0.0827 | 0.0567 | 2.365 (2.282~2.454) | — |
| Xbot 후 | 1.667 (1.648~1.677) | 0.176 | 1.491 | 0.0506 | 0.1007 | 0.0706 | 2.775 (2.645~3.002) | 0.043 / 0.066 |
| CesiumMan 전 | 1.440 (1.410~1.460) | 0.122 | 1.318 | 0.0000 | 0.0385 | 0.0023 | 2.345 | — |
| CesiumMan 후 | 1.551 (1.500~1.591) | 0.169 | 1.382 | 0.0001 | 0.0426 | 0.0036 | 2.703 | 0.030 / 0.053 |

읽는 법
- Xbot만 그리면 절단 전후 GPU 합은 +0.111 ms(+7.1%)로 3회 범위가 겹치지 않는다. 구성: Scene Update +0.055 ms(조각 스키닝 0.043 포함), 깊이+그림자+색 pass 합 +0.044 ms(0.1775→0.2219; 각각 +0.012, +0.018, +0.014), 나머지 Render GPU +0.012 ms. 절단으로 조각 2개가 추가되어 pass마다 draw가 늘어난 효과로 읽을 수 있다.
- CesiumMan은 깊이 블록이 0.0001 ms다. 기본 카메라에서 CesiumMan이 화면 밖이라 그려지는 것이 거의 없다는 뜻이다(추정: 카메라 절두체 판정을 따로 확인하지 않았고 블록 값으로만 판단). 그럼에도 Render GPU가 +0.064 ms 늘었으므로, 이 정도(약 0.06 ms)는 절단과 무관한 비-draw 변화(후처리·타이머 지연·클럭 등)가 섞여 있다는 대조군이다. Xbot의 "나머지 Render GPU +0.012"가 이 변동 안에 있다. 반면 draw 블록 3개의 증가는 CesiumMan에서 0.005 ms 안쪽이므로 Xbot의 +0.044 ms는 그림에 보이는 조각의 draw 증가로 본다.
- Scene Update GPU 증가(+0.047~0.055 ms)는 두 모델에서 비슷하며 조각 스키닝(0.03~0.04 ms)과 조각 PosedGeometry 갱신이다. 절단 전 pose 준비(PreparePose 포함)는 완전 장면·대상만 모두 0.105~0.127 ms다(모든 오브젝트 합이라 모델별 분리 불가).
- CPU: 절단 후 CPU 합이 +0.2~0.4 ms 늘었다(Xbot 대상만 2.365→2.775). 증가는 Update 쪽이다(1.504→2.014 ms). 이 안에는 조각 스키닝의 CPU(팔레트 생성·명령 기록, 0.066 ms)와 새 강체 body의 물리 시뮬레이션이 섞여 있고 둘을 분리하지 못했다. 완전 장면 Xbot에서는 Update 1.765→2.185, Render 0.799→0.575였다.
- 프레임 간격이 V-Sync에 고정이므로 "프레임 시간"은 변하지 않았다. GPU 합 8~9 ms와 CPU 합 2~3 ms 모두 16.66 ms 예산 안에 있어 어느 쪽도 한계가 아니다.

### 5.4 메모리 (절단이 상주시키는 GPU 버퍼)

게시 직후 `GpuBuffer::GetBufferSize`(논리)와 `GetResourceAllocationInfo`(할당)를 읽었다(실행 간 값 동일). 공유 Data/Provenance/CapMaterial은 A·B가 하나를 공유하므로 한 번만 센다.

| 버퍼 | Xbot 논리 / 할당 (B) | CesiumMan 논리 / 할당 (B) |
|---|---:|---:|
| 공유 Data (VB+depth VB+IB) | 306,504 / 327,680 | 54,384 / 65,536 |
| 공유 Provenance | 222,912 / 262,144 | 39,552 / 65,536 |
| 공유 CapMaterial | 256 / 65,536 | 256 / 65,536 |
| SkinVB (조각 A, B 각각) | 835,920 / 851,968 | 148,320 / 196,608 |
| PosedGeometry A / B | 174,360 / 196,608 ; 104,280 / 131,072 | 29,160 ; 20,280 / 각 65,536 |
| 원본 마스크 State | 196,448 / 196,608 | 18,688 / 65,536 |
| 원본 마스크 압축 IB | 564,192 / 589,824 | 51,840 / 65,536 |
| **합계** | **3,240,792 / 3,473,408 (3.31 MiB)** | **510,800 / 851,968 (0.81 MiB)** |

한계: 위 표는 게시 후 상주 버퍼만이다. 추출 작업 임시 버퍼(Xbot은 과거 기준 측정에서 Data 약 7 MB)와 분류·압축 임시 버퍼, 절단기 scratch는 이번에 다시 측정하지 않았다. 프로세스 전체 로컬 VRAM(`QueryVideoMemoryInfo`)을 요청 시점과 안정 후에 읽었으나 Xbot은 506.6→505.3 MiB(−1.3), CesiumMan은 506.5→495.8 MiB(−10.7)로 절단 후 오히려 줄었다(복사 owner·임시 버퍼 해제, 드라이버 할당 단위, 다른 해제가 섞임). 이 값은 절단의 순증가 메모리로 쓰지 않는다.

## 6. 재지 않은 것과 한계

- **cold 시작**: 완전한 cold(OS·드라이버 캐시가 비어 있는 첫 실행)는 재지 못했다. "1회차"는 배치 첫 실행일 뿐이다.
- **같은 프로세스의 반복 절단**: 데모 경로는 프로세스당 한 번만 자른다. 모든 절단 표본이 "프로세스 첫 절단"이며 warm 반복 절단 시간은 없다.
- **구간 안의 세부 원인**: 추출 제출(약 25 ms), 복사 준비, 분류·압축 제출, FinishCutResult(약 43 ms)의 CPU 내부 분해는 하지 않았다. PatchLog의 파일 열기·쓰기 비용, GPU 쿼리 힙 생성(계측 자체, 절단당 약 15개), 로그 flush가 CPU 구간에 포함된다.
- **GPU 구간의 범위**: 구간 전체 timestamp이며 구간 안의 clear·copy·barrier를 포함한다. 추출은 Mode 0/1/2/6 dispatch 합과 구간 전체를 둘 다 보였다(Mode 8, 정점 비트 dispatch는 따로 재지 않았고 전체에는 포함).
- **물리·래그돌**: BreakConstraint 적용의 내부 시간, 절단 후 body 수 증가에 따른 물리 비용을 따로 분리하지 않았다.
- **렌더 비교의 통제**: 조합당 3회 실행뿐이다. V-Sync(60 Hz)·백그라운드 프로그램·GPU 클럭·전원 상태를 통제하지 않았다. 완전 장면의 GPU 변동(런 간 약 ±0.25 ms)이 절단 전후 차이보다 크다. 위상 일치용 `cut-time`(8.3, 8.98)은 기본 요청 시점과 같은 포즈라는 가정이며 검증하지 않았다.
- **CesiumMan 렌더**: 기본 카메라에서 거의 그려지지 않는 것으로 보인다(위). 그래서 CesiumMan의 렌더 전후 비교는 draw 증가 비용을 보여 주지 못하고 Xbot 값을 기준으로 삼아야 한다. 카메라를 대상에 맞춘 측정은 하지 않았다.
- **GPU 타임스탬프 지연**: 프레임별 GPU 블록 값은 1~2프레임 지연이다. 절단 직후 몇 프레임의 개별 값은 해당 프레임과 어긋난다(평균 윈도우는 절단 후 150프레임 이후만 사용).
- **정지 윈도우 비교**: `정지`는 애니메이션과 물리가 멈춘 상태라 `전`과 직접 비교하지 않는다.
- **디버그 레이어·디버거**: 쓰지 않았다(`-debug 0`, 디버거 미연결). 이전 PerfBaseline의 수 초대 CPU 값은 수집기(debugger 연결) 조건이라 이번 값과 비교할 수 없다. GPU 구간은 PerfBaseline의 추출 전체 349 µs(Xbot)와 이번 363 µs로 비슷하다.

## 7. 원자료

`C:\Users\Pes\AppData\Local\Temp\claude\80629d7db37982f2\518c1cc2-650b-4503-b9cf-db7aa22eec9c\scratchpad\perf\`

| 파일 | 내용 |
|---|---|
| `A-xbot-01.log` ~ `A-xbot-10.log`, `A-cesium-01.log` ~ `A-cesium-10.log` | 절단 비용 실행별 원본 로그(`MARK`·`CPU`·`GPU`·`CUTTER`·`MEM`·`FRAME`) |
| `A-xbot-summary.csv/.md`, `A-cesium-summary.csv/.md` | 실행별 구간 값, 표의 집계 |
| `A-hitch.txt` | 절단 프레임의 끊김 집계 |
| `B-xbot-full-{1,2,3}.log`, `B-xbot-only-{1,2,3}.log`, `B-cesium-full-{1,2,3}.log`, `B-cesium-only-{1,2,3}.log` | 렌더 전후 실행별 원본(프레임별 `FRAME` 1,549행) |
| `B-run-means.csv`, `B-aggregate.txt`, `B-first-summaries.txt` | 실행·윈도우별 평균과 3회 집계 |
| `runs.txt`, `env-before.txt`, `env-beforeB.txt`, `build*.log` | 실행 결과, 측정 전 프로세스 목록, 빌드 로그 |
| `trial-xbot.log` | 계측 확인용 시험 실행(집계에 쓰지 않음) |
| `SP6-SkeletalPatchChecks-cumulative.log` | 패치 삼각형 수 등 기존 PatchLog (과거 기록 누적) |

집계 스크립트: scratchpad의 `summarizeA.ps1`, `summarizeB.ps1`, `aggB.ps1`, `hitch.ps1`, 실행 스크립트 `runperf.sh`, `batchA.sh`, `batchB.sh`.

재현: `MiniEngine\ModelViewer`에서 `..\Build\x64\Release\Output\ModelViewer\ModelViewer.exe -debug 0 -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:RightUpLeg -skeletal-break-demo-delay 3 -physics-query-exit 700 -cut-perf-log <파일>`.

## 8. 후속 수정과 다른 PC에서 측정하는 방법

### 8.1 이 PC 결과의 라벨 (검토 반영)

4~5절의 수치는 **"이 PC(MX450), 첫 절단·진단 로그(PatchLog 파일 기록) 포함, GPU timestamp 켬, 전체 장면"** 조건이다. 요청 구간 CPU에는 프로세스 첫 절단의 지연 초기화(PSO·RootSignature), 장면 렌더 뒤에 서는 동기 GPU 대기, PatchLog의 줄마다 파일 열고 닫기, 계측 자체의 비용이 섞여 있다. 따라서 "순수한 절단 CPU 비용"이 아니다. 렌더 5절에서 CesiumMan은 기본 카메라에서 화면 밖(깊이 0.0000 ms)이었으므로 노이즈 대조군으로만 읽고, 절단의 렌더 비용은 Xbot의 블록별 차이(조각 스키닝 약 0.04 ms, 깊이·그림자·색 draw 약 +0.044 ms)로 본다. 나머지 전후 차이(약 +0.06~0.11 ms)는 노이즈다. V-Sync가 켜진 저부하에서는 MX450 클럭이 내려갈 수 있어 GPU 시간이 흔들린다.

검토 후 추가한 옵션(모두 `-cut-perf-log` 가 있을 때만 유효, 기본 꺼짐):

| 옵션 | 의미 |
|---|---|
| `-cut-perf-log <파일>` | 계측 켜기. 로그는 메모리에 모았다가 종료 시(또는 256 KiB마다) 파일에 쓴다. 프레임별 VRAM 조회와 줄마다 flush는 없앴다 |
| `-cut-perf-quiet 1` | `PatchLog` 파일 기록과 로그 문자열 생성을 끈다 |
| `-cut-perf-gpu 0` | GPU timestamp 쿼리·프로파일 블록을 만들지 않는 CPU 전용 모드 |
| `-cut-perf-second-bone <본>` | 첫 게시 120프레임 뒤 같은 프로세스에서 다른 본을 한 번 더 자른다(warm 절단) |
| `-cut-perf-only-target 1` | 대상 외 오브젝트를 그리지 않는다. 그 오브젝트의 물리 body도 빠지므로 CPU는 같은 모드끼리만 비교한다 |
| `-cut-perf-focus 1` | 3프레임째에 카메라를 대상 정면 5.5 m로 맞춰 CesiumMan도 화면 안에 둔다 |

프레임별 계측(`CutPerf::FrameBegin`)은 `Update` CPU 측정 밖에서 호출한다. 이 PC에서의 재측정 캠페인은 하지 않았고(스크립트 스모크 테스트만 수행), clean 표는 다른 PC에서 측정한다.

### 8.2 측정 스크립트 `Tools\CutPerf\Run-CutPerf.ps1`

필요한 것: 이 저장소와 Release 빌드(`MiniEngine\Build\x64\Release\Output\ModelViewer\ModelViewer.exe`). Python은 필요 없다. PowerShell 5.1 이상.

```powershell
# 1. Release|x64 빌드 (이미 있으면 생략)
MSBuild MiniEngine\ModelViewer\ModelViewer.slnx -p:Configuration=Release -p:Platform=Windows -m
# 2. 측정 전: 다른 앱을 닫고, AC 전원 연결, 전원 계획을 고성능으로 둔다
# 3. 전체 측정 (절단 8구성 x N회 + 렌더 4구성 x RenderRuns회, 약 40~50분)
powershell -ExecutionPolicy Bypass -File Tools\CutPerf\Run-CutPerf.ps1
# 옵션: -N 10  -RenderRuns 3  -Models Xbot,CesiumMan  -SkipRender  -OutDir <폴더>  -ExePath <exe>  -NoOpen
# 짧은 확인: -N 1 -RenderRuns 1 (약 4분)
```

끝나면 출력 폴더(기본 `Tools\CutPerf\out\<시각>`)의 `CutPerfResult.txt`가 메모장으로 열린다. 이 파일 하나에 PC 정보(CPU·GPU·드라이버·VRAM·OS·전원), git 커밋, 구성별 옵션, 모델 통계, 모든 집계 표(평균·최소·최대)가 들어 있으니 그대로 복사해 전달하면 된다. 원시 로그(`raw\`), 실행별 값(`CutPerfRuns.csv`, `CutPerfRenderRuns.csv`), 집계(`CutPerfSummary.csv`)도 같은 폴더에 남는다.

구성(모델마다 Xbot `mixamorig:RightUpLeg`/두 번째 `mixamorig:LeftUpLeg`, CesiumMan `leg_joint_R_1`/`leg_joint_L_1`):

| 구성 | 옵션 | 보는 것 |
|---|---|---|
| `cut_quiet_gpuOff_full` | quiet 1, gpu 0 | 기준 CPU 절단 비용. cold(첫 절단)와 warm(두 번째 절단)을 따로 표시 |
| `cut_quiet_gpuOn_full` | quiet 1, gpu 1 | 위와 비교해 GPU 타이머 계측 오버헤드, GPU 구간(`gpu_us_*`)과 절단기 GPU |
| `cut_loud_gpuOff_full` | quiet 0, gpu 0 | 위(quiet)와 비교해 PatchLog 파일 기록 오버헤드 |
| `cut_quiet_gpuOff_only` | quiet 1, gpu 0, only-target | 장면 렌더 부하를 뺀 동기 대기 영향. CPU는 이 구성끼리만 비교 |
| `render_full` / `render_only` | gpu 1, quiet 1, focus 1 (+only-target) | 절단 전(`render-pre`)·후(`render-post`) 프레임 GPU·CPU 평균. 대상이 화면 안에 있다 |

출력 열 의미: `n` = 표본(실행) 수, `mean/min/max` = 실행 간 평균·최소·최대. 절단 표의 지표 정의(select_cpu, extract_submit_cpu, …, TOTAL_request_to_publish, frames_spanned, cutter_*_ms, gpu_us_*)는 `CutPerfResult.txt` 상단의 "How to read" 절에도 적혀 있다(3.3절의 단계 정의와 같다). 렌더 표의 `pre`는 애니메이션 시작 +15프레임부터 요청 3프레임 전, `post`는 게시 +150프레임부터 끝 3프레임 전이며 실행당 300프레임 이상이다. `gpu_total` = Scene Update GPU + Render GPU, `cpu_total` = Update + RenderScene 함수 시간, `wall_ms`는 V-Sync로 고정된다.

스모크 테스트(이 PC, N=1·RenderRuns=1): 12개 실행이 모두 `exit=0`으로 게시까지 끝났고 CSV 3개와 `CutPerfResult.txt`가 생성됐다. 옵션 없이 `-skeletal-break-demo 1 -physics-query-exit 300`만 준 기본 실행도 정상 종료했다. 이 값들은 표본 1개라 수치로 쓰지 않는다. 확인용으로 본 것: Xbot 첫 절단 요청→게시 128 ms(quiet, GPU 타이머 끔), 같은 프로세스의 두 번째 절단 84 ms; CesiumMan 초점 카메라에서 깊이 pass가 0.08 ms로 화면 안에 들어옴.
