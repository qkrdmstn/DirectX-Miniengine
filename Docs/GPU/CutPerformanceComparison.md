상태: 완료 — 기존 측정값 정리(새 측정 없음), CPU `2b4871f5` 재현 하네스 / GPU `673b56b6` 계열 / Mark-and-Cut `eb74c0bb`(master 미병합) 기준 (2026-10-30)

# 메시 절단 성능 비교: 병렬화 전(CPU) / 병렬화 후(GPU)

이 문서는 저장소 문서와 저장소 밖 원본 로그에 이미 남은 값만 모은다. 새로 측정하거나 추정한 값은 없다. 조건이 다른 값은 같은 행에 두지 않았다.

## 1. 용어와 근거

- **병렬화 전(CPU)**: GPU 리셋 전 CPU 절단 구현. 리셋 직전 commit `2b4871f5`(브랜치 `codex/meshcutting-before-gpu-reset-20261023`)를 저장소 밖에 추출해 만든 독립 하네스로 측정했다. 출처는 [StaticCutImplementation.md](StaticCutImplementation.md)의 "이전 CPU 구현의 재현" 절이다.
- **병렬화 후(GPU)**: `ClipTrianglesCS`·경계 연결·`CapEarsCS`·bounds를 GPU 컴퓨트로 실행하는 Static 절단이다. `-cut-benchmark 20`의 `Static cut benchmark: medianGpuMs/p95GpuMs/medianReadyMs` 출력이다.
- **MC = Mark-and-Cut**: 커밋 `eb74c0bb` "Use mark-and-cut as the only cap triangulation path"(브랜치 `codex/mark-and-cut-only`)와 그 커밋의 StaticCutImplementation.md 절 "경계 선표시 mark-and-cut 단일 경로"가 근거다. Eder/Held/Palfrader (2018)의 선표시 원리를 `CapEarsCS::MarkCandidates`에 적용했고, 측정 당시 옵션 `-cap-mode 3`이다. `-cap-mode 0`은 그 전 기본 경로(병렬 ear 검사 후 해시 우선순위 선택)다.
- 이 커밋은 2026-10-30 현재 master에 병합되지 않았다. master의 `CapEarsCS.hlsl`에는 `MarkCandidates`가 없다.

## 2. 측정 환경

| 구분 | 환경 | 날짜 |
| --- | --- | --- |
| CPU | Intel Core i5-1135G7(8 logical), Windows 10.0.19045, VS2022 MSVC 19.44.35229 x64 `/O2 /DNDEBUG`, 3 warm-up + 20 sample, 모델마다 독립 process 2회 | 2026-10-25 |
| GPU (mode0) | NVIDIA GeForce MX450, driver 32.0.15.9186, 1920×1080, Release `-debug 0`, 3 warm-up + 20 sample | 2026-10-25 |
| GPU (MC 비교) | 같은 GPU·driver, Release `-debug 0 -model Models/bunny.glb -cut-benchmark 20 -cap-mode 0\|3 -cut-expect-publications 1`, 순차 실행. 물리 연결이 추가된 로컬 기준 코드 | 2026-10-26 |

공통 입력: GLB 원본 cube(12 triangles)·bunny(30,338 triangles), 모델 기준 y=0 평면. bunny는 경계 370, loop 1, uncapped 0이다.

## 3. 비교표 — 같은 조건 (y=0 평면, 준비된 입력의 절단 계산 구간)

CPU 구간: 준비된 입력의 clipping + cap + 출력 할당 + 출력 validation. 입력 변환·용접·반복 입력 검증 제외.
GPU 구간: 준비된 pose부터 scratch clear + clip + 경계 + cap + bounds까지의 GPU timestamp. 자원 생성·readback·게시·렌더 제외.

| 모델 | CPU 중앙값 ms (run1 / run2) | GPU 중앙값 ms (P95) | 배율 CPU÷GPU | GPU 경로 | 출처 |
| --- | --- | --- | --- | --- | --- |
| Bunny | 88.5693 / 94.5612 (P95 109.98 / 122.43) | 5.627808 (6.781920) | 15.7× / 16.8× | mode0 해시 선택 | StaticCutImplementation.md "최종 GPU 실행 결과" |
| Bunny | 88.5693 / 94.5612 | 5.383952 (cap P95 10.08) | 16.5× / 17.6× | mode0, MC 비교 세션 | `eb74c0bb` 문서의 mode0/3 표 |
| **Bunny** | 88.5693 / 94.5612 | **6.808912** (cap P95 7.08) | **13.0× / 13.9×** | **MC (mode3)** | `eb74c0bb` 문서의 mode0/3 표 |
| Cube | 0.1423 / 0.0754 (P95 0.2378 / 0.1006) | 0.100192 (0.700416) | 1.4× / 0.75× | mode0 | StaticCutImplementation.md |
| Cube | 0.1423 / 0.0754 | (미측정) | — | MC | — |

읽을 때 주의:

- CPU는 출력 validation을 포함하고 GPU는 포함하지 않는다. attribute(CPU는 고정 normal/UV/color)와 정점 배치·순회 순서도 다르다. 원 문서도 이 비교를 참고값으로 두고 전체 게임 프레임 속도비로 해석하지 않는다.
- CPU 2회 run과 GPU 1회 값의 조합이라 배율은 두 값으로 적었다.
- MC 행의 GPU 값은 다른 코드 기준(물리 연결 추가)과 다른 날의 세션이다. 같은 세션의 mode0 값과 비교하면 MC cap 중앙값이 약 35% 느렸다(4.064080 → 5.481024 ms). 원 문서는 MC의 성능 향상을 주장하지 않는다.
- Cube는 일이 작아 GPU clock·dispatch 비용이 지배한다. GPU P95(0.70 ms)가 CPU보다 크며 이득을 보장하지 않는다.
- mode0 행은 "접촉·반복 단면 회귀 수정" 이전 코드의 측정이다(원 문서 명시).

## 4. 같은 행에 둘 수 없는 참고값 (범위·조건이 다름)

| 항목 | 값 | 비교 불가 이유 | 출처 |
| --- | --- | --- | --- |
| CPU Bunny Build+weld+prevalidate+slice 전체 (10 sample) | 141.3835 / 145.0804 ms | GPU `ReadyMs`와 측정 범위가 다르다 | StaticCutImplementation.md |
| GPU Bunny 요청→metadata 준비 (`medianReadyMs`, mode0) | 28.428 ms (이전 run 29.351 / 29.441) | 자원 생성~GPU metadata 읽기 가능, CPU clock. 출력 게시 제외 | 같은 문서, `%TEMP%/static-cut-validation/release-benchmark/` |
| GPU Cube `medianReadyMs` (mode0) | 12.034 ms (이전 12.871 / 12.900) | 위와 같음 | 같은 문서 |
| GPU 마지막 컴포넌트 게시까지 | Cube 11.487 / Bunny 45.465 ms | 1회 값, 중앙값 아님 | 같은 문서 |
| CPU 런타임 Bunny Enter 프레임 (`fc1f739`) | 약 186 ms (`ConfirmAll` 179.9 ms) | 30° 기울인 평면, 추출·렌더 데이터 생성 포함, 드라이버 표기 591.86 | 브랜치 `codex/meshcutting-before-gpu-reset-20261023`의 `MiniEngine/MeshCutting/Docs/CutEnterCostReport.md` |
| 같은 측정의 CPU cap 삼각분할만 | 0.7 ms (cap 전체 `BuildCap`×2 20.1 ms) | 위와 같은 조건. GPU cap 시간(4~5.5 ms)과 평면·범위가 다르다 | 같은 보고서 3.2절 |
| CPU Enter 개선 후 (`ValidateMeshGeometry` 감소) | `ConfirmAll` 약 140 ms, Enter 프레임 약 148 ms | 위 런타임 조건, 단계 C 적용 | 같은 브랜치의 `EnterCostReductionDesign.md` |
| GPU Debug Bunny cap (mode2 / 다중 제거) | 320.640 / 47.506 ms | Debug 실행 | StaticCutImplementation.md 검증 목록 |

CPU cap 삼각분할 자체(0.7 ms)는 GPU cap 구간보다 짧았다. 조건이 달라 직접 비교는 아니다. 그래도 발표에서 "GPU 이득은 cap 병렬화가 아니라 표면 절단·전체 파이프라인에서 나왔다"는 해석의 여지는 표시해 둔다. 이 해석 자체는 측정으로 확인하지 않았다.

## 5. 원본 로그 위치 (저장소 밖)

- CPU: `%TEMP%/cpu-slice-history-bench/release-final/{cube,bunny}-run{1,2}.log` (`prevalidated_absolute_epsilon_slice_and_caps`, `end_to_end_build_validate_once_plus_absolute_epsilon_slice` 줄)
- GPU mode0: `%TEMP%/static-cut-validation/release-benchmark/{cube,bunny}-run{1,2}.txt`
- MC 비교: `%TEMP%/mark-cut-validation/benchmark-summary.json`

## 6. 비어 있는 칸과 채우는 방법

| 비어 있는 값 | 필요한 측정 |
| --- | --- |
| MC Cube GPU ms | `codex/mark-and-cut-only` Release 빌드에서 `-model Models/cube.glb -debug 0 -cut-benchmark 20 -cut-expect-publications 1` |
| MC Bunny `medianReadyMs` | 같은 빌드에서 `-model Models/bunny.glb -debug 0 -cut-benchmark 20 -cut-expect-publications 1`의 로그 줄 |
| 현재 코드 기준 GPU 재측정 | 회귀 수정 이후 코드에서 위 두 명령. MC가 master에 병합된 뒤라면 master에서 |
| 같은 범위의 요청→완료 비교 | CPU 하네스에 GPU `ReadyMs`와 같은 구간 정의가 없다. 범위를 맞추려면 별도 계측 설계가 필요하다 |

실행 위치는 `MiniEngine/ModelViewer`이고, 실행 파일은 `../Build/x64/Release/Output/ModelViewer/ModelViewer.exe`다. 다른 브랜치는 별도 worktree에서 빌드해야 한다.
