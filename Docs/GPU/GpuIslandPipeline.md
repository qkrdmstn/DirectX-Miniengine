상태: G1 완료·G2~G5 보류 — G0 `959b1bbc`, G1a `d6c84d3a`, G1b 이 커밋; 섬 판정 CPU 합 약 0.3 ms라 GPU 이관 보류, 첫 절단 비용 조사로 전환 (2026-11-26)

# 스켈레탈 절단 섬 판정·cap 보정의 GPU 이관 결정

## 목표

- 스켈레탈 절단의 요청→게시 경로에서 CPU 계산과 CPU·GPU 동기 대기를 줄인다.
- 대상은 [MeshIslandMultiBoneCut.md](MeshIslandMultiBoneCut.md)가 범위 밖으로 둔 섬 판정(GPU union-find)과 그 앞뒤 단계다.
- 출력은 현재 CPU(double) 경로와 같아야 한다. 비교 지점은 아래 "완료 조건"에 적는다.

## 현재 경로 요약 (조사 메모 기준)

| 단계 | 위치 | 동기 대기 |
| --- | --- | --- |
| `BuildLatticeTopology` (로드 1회) | CPU | — |
| `SkeletalPatchJob::Submit`/`Poll` 추출 결과 | GPU→CPU | Submit이 전체 capacity 복사 기록, Poll은 fence 후 실제 개수만 해석 |
| `DiagnoseIslands` 정점 재스키닝·부호 | CPU | — |
| `LabelPatchIslands` (셀,side) UF·첫 등장 번호·weight 투표 | CPU | — |
| cutter `LoopReadback` → `RelabelCapLoops`·`RelabelPatchIslands`·`DiagnoseCapLoops` | GPU→CPU→CPU | fence Poll |
| `ClassifyCutBodies` | CPU↔GPU | 합계 4회: side 0의 표 2개 `Create(initialData)`, side별 counter `Finish(true)` 1회씩 |
| `CompactCutBodies`·`PollBodyCompaction` | GPU→CPU | fence Poll |
| `BuildPatchSkinVertices` | CPU↔GPU | 초기 데이터 업로드 여러 개(descriptors·joints·length·boundary·counters) + skin VB 전체 회수 `Finish(true)` |

- 함수별 CPU 시간은 지금 따로 재지 않는다. 어느 단계가 병목인지 근거가 없다.

## 결정

1. **측정을 먼저 한다 (G0).** `-cut-perf-log`에 함수별 CPU 구간과 동기 대기 구간을 추가한다. 같은 이름 구간이 여러 번 나오면 합계와 횟수를 남긴다. runner CSV까지 연결한다.
   - runner에 CesiumMan 위팔, Xbot 다중 허벅지, 척추 장면과 `-island-gpu 0/1` 비교 옵션을 추가한다.
   - 이유: 병목을 모른 채 위험이 큰 UF부터 옮기면 이득이 작을 수 있다.
2. **분류 단계의 동기 대기 4회를 먼저 없앤다 (G1).**
   - 표 2개는 `ReserveUploadMemory`로 같은 command list에 올린다.
   - counter 회수는 비동기로 바꾼다. 분류 counter와 readback 자원은 job이 소유하고, 압축 fence 완료 뒤 `job.Dropped`를 채운 다음 `sum + dropped == input`을 검사한다. invalid body 실패 플래그는 유지한다.
   - 정상 완료·취소·예외 경로에서 job 자원 수명을 보장한다.
3. **CPU(double) 경로를 기준 구현(oracle)으로 남긴다.**
   - `-island-gpu 0|1`(기본 0), `-island-gpu-verify 1`(두 경로 모두 실행, 표 비교, 불일치 로그, 진단 전용)을 둔다.
   - 기본값 1 전환은 완료 조건을 만족한 뒤 사용자가 결정한다.
4. **topology를 GPU에 올린다 (G2a).** 로드 시 section별 `VertexCell`을 SRV로 한 번 업로드한다. CPU vector는 oracle용으로 유지한다.
5. **평면 거리는 추출과 같은 제출에서 구한다 (G2b).**
   - PG(`PreparePoseCS` 출력)는 다음 프레임에 덮어쓰므로 나중에 읽지 않는다. 추출 제출 안에서 정점별 `d0 = dot(plane, world·p_PG)`를 계산해 job 소유 버퍼에 남긴다. PG에는 section frame이 이미 적용돼 있으므로 world만 곱한다.
   - 복사 offset은 Poll에서 정해진다. 그래서 부호는 라벨링 dispatch에서 `d = d0 + plane.x·offset`으로 구하고, `|d| ≤ 1e-5`이면 0으로 둔다.
   - CPU 재스키닝과 부동소수 순서가 다르다. verify에서 부호 불일치 수를 센다. 부호만 다르고 섬·body 표가 같으면 허용한다. 표가 다르면 그 단계는 완료가 아니다.
6. **연결 성분은 GPU hook + pointer jumping으로 구한다 (G3a).**
   - 노드는 `2*cell+side`다. 패치 삼각형 t의 **존재하는 side**마다 그 side 꼭짓점(0 정점 포함) 셀의 현재 root를 대상으로 `InterlockedMin` hook을 한다. 패스 사이에는 UAV barrier를 둔다.
   - side 존재 규칙은 CPU와 같다. 양수만 있으면 side 0, 음수만 있으면 side 1, 양·음이 섞이면 양쪽, 전부 0이면 side 0만 존재한다. 없는 side는 `UINT32_MAX`로 둔다.
   - hook·jump를 상한 K번 이어 붙인다. 마지막에 모든 연결의 root가 같은지 검증한다. 실패하거나 K를 넘으면 그 요청은 게시하지 않고 CPU 경로로 다시 계산한다.
7. **첫 등장 번호는 결정적으로 만든다 (G3b).** 존재하는 `(t, side)`만 대상으로 성분마다 최소 `t*2+side`를 구한다. t는 원본 triangle ID가 아니라 패치 배열 인덱스다. 이 키 순서로 scan해 번호를 매기면 CPU의 `map.emplace` 순서와 같다. CPU UF의 대표 노드 값은 재현하지 않는다.
8. **투표는 GPU 정수 점수 + 근접 표차 fallback으로 한다 (G4).**
   - CPU 규칙: 존재하는 (t, side)마다 세 정점 모두(반대 부호 정점 포함, 공유 정점은 삼각형마다 중복) 비영 weight마다 `double(w)/Σw`를 그룹에 더한다. CPU oracle은 바꾸지 않는다.
   - GPU 규칙: 정점 weight를 `floor(w·4096/Σw)`(정수 나눗셈)로 바꿔 uint32 `InterlockedAdd` 한다. (t, side)당 최대 3·4096이므로 그룹 합은 존재하는 (t, side) 34만 개 미만에서 넘치지 않는다. 그보다 크면 CPU 경로로 처리한다.
   - 정수화 오차는 정점·weight당 1 미만이다. 1위와 2위 차가 `12 × 그 섬의 (t, side) 수` 이하이면 그 섬을 "근접"으로 표시하고 CPU double로 다시 계산한다. 그래서 출력은 oracle과 같다.
   - 동점이면 A, `SideDefault` 동점이면 작은 body인 규칙은 유지한다.
9. **섬 표 소비 경계.**
   - G3까지: GPU는 섬 번호만 만든다. 투표·cap 보정은 CPU에서 한다.
   - G4b부터: 섬·body 표를 GPU에 둔다. G5 전에는 cap 보정이 CPU에 있으므로, 섬 표·`SideDefault`를 cutter fence와 같은 시점에 작은 비동기 readback으로 CPU mirror에 받는다. 보정 결과만 작은 요청 표로 다시 올린다.
   - G4c: `ClassifyCutBodies`가 CPU 표 대신 GPU 결과를 읽는다.
10. **cap loop 보정·cap 표(G5)는 G0~G4 측정 후 결정한다 (조건부).** 최소 면적 loop 선택, 경계 포함(double), 다중 P 충돌 예외를 모두 옮겨야 해서 위험이 크다. 진행하면 `LoopSources`를 재사용하고, 충돌·실패는 GPU 오류 플래그 → CPU 재계산으로 처리한다.
11. **CPU에 남기는 것:** 로드 시 `BuildLatticeTopology`, `BuildCutBodyGroups`, `Crossings` snapshot, 물리·게시·revision 검증.
12. **후속 후보 (범위 밖, G0 결과로 별도 결정):** `BuildPatchSkinVertices`의 업로드 대기·BindBounds 회수 축소, 추출 readback을 count 크기로 축소.

## 인터페이스 골격

```cpp
// 로드 시 VertexCell을 GPU에 올린 버퍼 묶음
struct GpuLatticeTopology
{
    std::vector<StructuredBuffer> SectionVertexCell; // section별 uint 셀 id
    uint32_t CellCount = 0;
};

// 요청 하나의 GPU 섬 판정 상태(job 소유)
struct GpuIslandJob
{
    StructuredBuffer VertexDistance;     // 추출 제출에서 구한 d0
    StructuredBuffer Parent;             // 2*CellCount UF
    StructuredBuffer TriangleSideIsland; // 패치 삼각형당 uint2
    StructuredBuffer TriangleSideBody;   // G4b부터, 패치 삼각형당 uint2
    ByteAddressBuffer Status;            // 수렴·섬 수·근접·오류 플래그
};
```

- 새 셰이더: `IslandDistanceCS`(G2b), `IslandHookCS`·`IslandJumpCS`·`IslandVerifyCS`(G3a), `IslandNumberCS`(G3b), `IslandVoteCS`(G4b).

## 단계 (각 한 개념, 약 300줄 이내)

| 단계 | 내용 | 확인 |
| --- | --- | --- |
| G0 | 함수별 CPU·대기 구간 계측, runner 장면·옵션 추가, 기준값과 빌드 시간 기록 | 결과표를 이 문서에 기록 |
| G1a | 분류 표 업로드를 `ReserveUploadMemory`로 | 출력 동일, 업로드 대기 0 |
| G1b | 분류 counter 비동기 회수·job 소유·`Dropped` 검증 이동 | 출력 동일, 대기 0, 취소·예외 시 자원 해제 |
| G2a | `VertexCell` GPU 업로드 | 업로드 내용 CPU와 일치 |
| G2b | `IslandDistanceCS` + 라벨링 부호 + verify 부호 비교 | 부호 불일치 수 기록 |
| G3a | GPU UF hook·jump·root 검증·K 초과 fallback | 강제 K=1 시험에서 fallback |
| G3b | 첫 등장 번호 + verify 섬 표 비교 | `TriangleSideIsland` 일치 |
| G4a | CPU 정수 점수 함수 + LatticeTopologyTests(근접 판정·overflow·동점·빈 패치·없는 side) | oracle과 대조 |
| G4b | `IslandVoteCS` + 근접 fallback + CPU mirror readback | `TriangleSideBody`·`SideDefault` 일치 |
| G4c | `ClassifyCutBodies`가 GPU 표를 직접 사용 | body별 삼각형 수 일치 |
| G5a~c | (조건부) loop 선택 / 섬 재라벨 / cap 투표 | cap 표 일치 |

- 각 단계: Codex 구현 → 구현과 다른 새 Codex 리뷰(이 문서 완료 조건 기준) → CPU 테스트 → Debug·Release 빌드와 실행(validation) → 로컬 커밋 → 사용자가 커밋 단위로 읽는다.
- 공간이 부족하므로 Debug와 Release를 차례로 하나씩 빌드하고, 빌드 사이에 산출물을 정리한다.
- 예상 시간: Codex 별도 리뷰 2~5분(기존 기록). G0 실측: Release 전체 빌드 24.171초(VS 환경 초기화 포함, MSBuild 20.69초), runner `-G0 -N 3` 198.427초(15회, 1회 실패), validation 13.141초, 다중 허벅지 재실행 8.025초. 별도 리뷰는 이번 실행에 포함하지 않았다.

## G0 측정

2026-11-26, `master` / HEAD `d5e94361` + 미커밋 계측. **계측 구현·일부 기준값 측정 완료, G0 전체 완료는 보류**다. 다중 허벅지 실행 실패와 DXGI validation 오류가 남았다.

### 계측 정의

- 기존 `-cut-perf-log <경로>`에서만 기록하며 기본은 꺼짐이다. `CutPerf::Cpu::End`는 자원·변수 수명을 바꾸지 않고 구간을 한 번 닫는다.
- `DiagnoseIslands`: 함수 전체; `DiagnoseIslands-reskin-sign`: section 프레임 표 준비·중복 제거·CPU 재스키닝·평면 부호 계산. 재스키닝과 부호는 한 구간이다.
- `LabelPatchIslands`, `RelabelPatchIslands`: `SkeletalPatchCopy.cpp` 호출부에서 해당 호출만 측정한다. `LatticeTopology.cpp`에 엔진 의존성을 추가하지 않았다.
- `RelabelCapLoops`: loop 선택·재라벨 전체(내부 `RelabelPatchIslands` 포함). `DiagnoseCapLoops`: cap loop 투표·GPU 귀속 표 생성 전체.
- `ClassifyCutBodies-upload-wait`: side 0의 초기 데이터 `Create` 두 번; `ClassifyCutBodies-counter-wait`: side별 `Finish(true)` 두 번.
- `BuildPatchSkinVertices`: 조각별 함수 전체. `BuildPatchSkinVertices-upload-wait`: descriptors·joints·length·boundary·counters의 초기 데이터 `Create` 다섯 번/호출. `BuildPatchSkinVertices-final-wait`: 최종 VB·counter readback의 `Finish(true)` 한 번/호출.
- upload-wait는 순수 fence sleep이 아니라 할당·복사 명령 제출·내부 완료 대기를 포함한 벽시계다. 부모 CPU 구간은 자식 구간을 포함하므로 서로 더하지 않는다.
- runner는 요청별 `cpu_ms_sum_*`, `cpu_count_*`, `cpu_ms_mean_*`을 CSV·Summary·텍스트에 출력한다. mean은 합계/호출 횟수이며 Summary는 실행 간 통계다.
- `gpu_sum_ms`: cutter GPU ms + `*-whole` 패스들의 합. 중첩 `Extract-mode*`와 프레임 렌더는 제외한다. GPU 타이머 OFF에서는 이 합을 출력하지 않는다(cutter 자체 값은 유지).

### 기준표

단위는 ms. CPU 함수 행은 요청별 sum/count의 실행 간 평균이며, 대기 행은 요청당 합계/횟수의 실행 간 평균이다. 1·2차는 같은 프로세스의 양쪽 허벅지이며 작업량은 같지 않다. `실패`는 0이 아니며, 타이머 OFF의 GPU 합은 측정하지 않는다.

#### GPU 타이머 ON — 허벅지 (각 N=3)

| CPU 함수/구간 mean 또는 지표 | Xbot 1차 | Xbot 2차 | CesiumMan 1차 | CesiumMan 2차 |
| --- | --- | --- | --- | --- |
| DiagnoseIslands / reskin-sign | 12.478 / 0.054 | 1.040 / 0.068 | 1.956 / 0.020 | 0.871 / 0.014 |
| LabelPatchIslands | 0.181 | 0.227 | 0.084 | 0.058 |
| RelabelCapLoops / RelabelPatchIslands | 0.039 / 0.010 | 0.042 / 0.011 | 0.024 / 0.003 | 0.012 / 0.002 |
| DiagnoseCapLoops | 0.004 | 0.003 | 0.003 | 0.001 |
| BuildPatchSkinVertices | 116.563 | 5.490 | 123.219 | 4.104 |
| FinishCutResult / OnCutResult-classify-compact-submit | 235.237 / 62.496 | 13.280 / 8.502 | 248.440 / 69.214 | 9.395 / 3.862 |
| 분류 upload wait (ms sum/count) | 0.919 / 2.000 | 2.798 / 2.000 | 4.587 / 2.000 | 0.697 / 2.000 |
| 분류 counter wait (ms sum/count) | 0.481 / 2.000 | 0.435 / 2.000 | 0.531 / 2.000 | 0.217 / 2.000 |
| skin upload wait (ms sum/count) | 7.722 / 10.000 | 5.594 / 10.000 | 7.334 / 10.000 | 4.255 / 10.000 |
| skin final wait (ms sum/count) | 1.341 / 2.000 | 0.911 / 2.000 | 0.675 / 2.000 | 0.323 / 2.000 |
| 요청→게시 ms / frames | 457.863 / 4.000 | 65.683 / 4.000 | 476.060 / 4.000 | 61.572 / 3.667 |
| GPU sum ms | 1.579 | 1.514 | 0.429 | 0.350 |

#### GPU 타이머 ON — 추가 장면 (각 N=1)

| CPU 함수/구간 mean 또는 지표 | CesiumMan 위팔 | Xbot 다중 허벅지 | CesiumMan 척추 |
| --- | --- | --- | --- |
| DiagnoseIslands / reskin-sign | 2.368 / 0.035 | 실패 | 2.534 / 0.022 |
| LabelPatchIslands | 0.130 | 실패 | 0.144 |
| RelabelCapLoops / RelabelPatchIslands | 0.035 / 0.011 | 실패 | 0.033 / 0.004 |
| DiagnoseCapLoops | 0.004 | 실패 | 0.006 |
| BuildPatchSkinVertices | 128.298 | 실패 | 115.373 |
| FinishCutResult / OnCutResult-classify-compact-submit | 258.678 / 70.405 | 실패 | 232.884 / 67.514 |
| 분류 upload wait (ms sum/count) | 5.146 / 2.000 | 실패 | 5.060 / 2.000 |
| 분류 counter wait (ms sum/count) | 0.626 / 2.000 | 실패 | 0.554 / 2.000 |
| skin upload wait (ms sum/count) | 6.718 / 10.000 | 실패 | 6.543 / 10.000 |
| skin final wait (ms sum/count) | 0.898 / 2.000 | 실패 | 0.724 / 2.000 |
| 요청→게시 ms / frames | 476.720 / 4.000 | 실패 | 463.299 / 4.000 |
| GPU sum ms | 0.551 | 실패 | 0.503 |

#### GPU 타이머 OFF — 허벅지 (각 N=3)

| CPU 함수/구간 mean 또는 지표 | Xbot 1차 | Xbot 2차 | CesiumMan 1차 | CesiumMan 2차 |
| --- | --- | --- | --- | --- |
| DiagnoseIslands / reskin-sign | 12.383 / 0.055 | 0.805 / 0.052 | 1.961 / 0.020 | 1.260 / 0.023 |
| LabelPatchIslands | 0.187 | 0.176 | 0.084 | 0.083 |
| RelabelCapLoops / RelabelPatchIslands | 0.034 / 0.009 | 0.028 / 0.009 | 0.023 / 0.003 | 0.019 / 0.003 |
| DiagnoseCapLoops | 0.006 | 0.002 | 0.003 | 0.001 |
| BuildPatchSkinVertices | 118.903 | 5.174 | 127.081 | 4.420 |
| FinishCutResult / OnCutResult-classify-compact-submit | 240.178 / 65.192 | 11.540 / 3.968 | 256.379 / 65.938 | 10.269 / 3.667 |
| 분류 upload wait (ms sum/count) | 0.957 / 2.000 | 1.016 / 2.000 | 5.062 / 2.000 | 0.766 / 2.000 |
| 분류 counter wait (ms sum/count) | 0.726 / 2.000 | 0.251 / 2.000 | 0.839 / 2.000 | 0.216 / 2.000 |
| skin upload wait (ms sum/count) | 6.893 / 10.000 | 5.604 / 10.000 | 6.719 / 10.000 | 4.735 / 10.000 |
| skin final wait (ms sum/count) | 1.274 / 2.000 | 1.029 / 2.000 | 0.829 / 2.000 | 0.419 / 2.000 |
| 요청→게시 ms / frames | 472.447 / 4.000 | 55.119 / 4.000 | 479.724 / 4.000 | 62.679 / 3.667 |
| GPU sum ms | — | — | — | — |

타이머 ON 성공 요청 14개에서 CPU 구간 mean 상위 3개는 `FinishCutResult` 143.616 ms, `BuildPatchSkinVertices` 70.843 ms/호출, `OnCutResult-classify-compact-submit` 40.724 ms다. 앞 두 구간은 포함 관계이므로 합산하지 않는다. 1차의 초기화 비용이 크며 2차와 분리해 해석한다.

타이머 ON 표의 요청당 대기 합계 평균 범위: 분류 upload 0.697~5.146 ms/2회, counter 0.217~0.626 ms/2회, skin upload 4.255~7.722 ms/10회, final 0.323~1.341 ms/2회. upload는 할당·제출을 포함한다.

환경: NVIDIA GeForce MX450, 전용 메모리 1906 MiB(엔진 ENV), NVIDIA 드라이버 591.86(`nvidia-smi`). Release, `-debug 0`, 전체 장면, quiet=1, V-Sync 기본 ON. WMI 조회는 접근 거부되어 CPU·OS 상세는 수집하지 못했다.

### 실행·검증 및 남은 항목

- VS2022 Developer 환경에서 `msbuild MiniEngine/ModelViewer/ModelViewer.slnx /restore /m /p:Configuration=Release /p:Platform=Windows` 성공: 오류 0, 경고 0. 벽시계 24.171초(VS 환경 초기화 포함), MSBuild 20.69초. Jolt Release 캐시를 사용했고 재다운로드하지 않았다. 코드·계측 설계 수정은 없다.
- runner: `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CutPerf/Run-CutPerf.ps1 -G0 -N 3 -NoOpen -OutDir C:/Users/Pes/Desktop/DirectX-Miniengine/Tools/CutPerf/out/G0-20261126-measured`. 벽시계 198.427초, 종료 코드 1. 15개 실행 중 14개 정상 종료·26개 요청 게시(허벅지 24 + 위팔·척추 각 1), 다중 허벅지 1개 실패.
- Xbot 다중 허벅지는 `-skeletal-multi-cut 1`에서 종료 코드 `0xC0000409`(-1073740791), 성능 로그 0 B다. 같은 옵션 재실행도 8.025초에 같은 코드로 실패했다. 디버그 출력은 cutter 결과 수신·cap 진단까지 확인되나 이후 원인은 미확정이다. 실패 로그를 0이나 정상 표본으로 대체하지 않았다. 요청 범위가 빌드 오류 수정뿐이므로 런타임 코드는 변경하지 않았다.
- Release Xbot 오른쪽 허벅지 `-debug 1` 1회: 13.141초, 종료 0, 게시 499.087 ms/4 frames. DBWIN 캡처 기준 **D3D12 ERROR 0, DXGI ERROR 1**: `IDXGISwapChain::GetContainingOutput`의 어댑터·출력 불일치(#80). D3D12 경고 #1008(중복 subresource barrier)도 있어 전체 validation 무오류 PASS로 판정하지 않는다. 초기 NVIDIA ShadowPlay 로그 접근 오류 2건은 위 ERROR 집계에서 별도다.
- 기존 `%TEMP%/ndc-debug-capture.ps1`은 Add-Type DLL 접근 거부로 실패하여 임시 native DBWIN 수집기를 사용했다. 엔진 코드는 변경하지 않았다. 임시 수집기 소스·바이너리 삭제는 도구 자동 승인 검토에서 `blocked by policy`로 거부되어 out의 capture-temp에 남았다. Debug 빌드는 이번 요청 범위에서 생략, Geometry 변경이 없어 PlaneSlicer CPU 테스트는 대상이 아니다. 별도 새 세션 리뷰는 미수행이다.
- 재현 근거: `Tools/CutPerf/out/G0-20261126-measured/`의 `CutPerfRuns.csv`, `CutPerfSummary.csv`, `CutPerfResult.txt`, `runs.txt`, `raw/`, `validation-cut.log`, `validation-debug.log`, `multi-retry-debug.log`. runner가 보고한 실패를 그대로 보존했다. 이 디렉터리는 git ignore 대상이다.
- 후속: 다중 허벅지 런타임 실패와 DXGI 오류를 별도 작업에서 해결한 뒤 해당 기준값을 재측정한다. `-island-gpu` 옵션은 아직 엔진에 없으므로 OFF/ON은 GPU 타이머 비교이며 섬 알고리즘 비교가 아니다. G0는 미커밋 진행 중이며 완료 판정은 보류한다.
- Release 빌드 산출물은 보존했다. 이번 TEMP 빌드 상세 로그(`%TEMP%/g0-release-build-20261126.log`)와 임시 수집기 산출물 삭제는 도구 자동 승인 검토에서 `blocked by policy`로 거부되어 남았다. 삭제를 완료했다고 간주하지 않는다. 커밋·푸시는 하지 않았다.

### G1a 결과

2026-11-26, `master` / HEAD `959b1bbc` + 미커밋. `ClassifyCutBodies`의 두 표는 초기 데이터 없이 생성하고, 분류 context의 `ReserveUploadMemory`에 원본과 같은 바이트 수를 복사한다. `COPY_DEST` 복사 뒤 기존 `NON_PIXEL_SHADER_RESOURCE` 전이를 거쳐 `PatchBodyClassifyCS`가 읽는다. 표의 job 소유권·counter 동기 회수는 유지한다. 빈 side 0도 표 복사만 비동기 제출한다. 기존 upload-wait 구간은 대기 없는 두 구간으로 유지한다.

아래는 G0→G1a 평균 비교다. upload는 요청당 합계 ms(각 2회), 게시 시간은 요청→게시 ms, 프레임도 요청→게시 값이다. 허벅지는 각 N=3, 위팔·척추는 각 N=1이며 ON/OFF는 GPU 타이머 설정이다.

| 설정 / 장면 | upload wait ms | 게시 ms | 게시 frames |
| --- | --- | --- | --- |
| ON / Xbot 1차 | 0.919→0.000 | 457.863→504.977 | 4.000→4.000 |
| ON / Xbot 2차 | 2.798→0.000 | 65.683→56.905 | 4.000→4.000 |
| ON / CesiumMan 1차 | 4.587→0.000 | 476.060→624.096 | 4.000→4.000 |
| ON / CesiumMan 2차 | 0.697→0.000 | 61.572→74.339 | 3.667→4.000 |
| ON / CesiumMan 위팔 | 5.146→0.000 | 476.720→519.622 | 4.000→4.000 |
| ON / CesiumMan 척추 | 5.060→0.000 | 463.299→1185.650 | 4.000→4.000 |
| OFF / Xbot 1차 | 0.957→0.000 | 472.447→489.020 | 4.000→4.333 |
| OFF / Xbot 2차 | 1.016→0.000 | 55.119→65.168 | 4.000→4.000 |
| OFF / CesiumMan 1차 | 5.062→0.000 | 479.724→459.838 | 4.000→4.000 |
| OFF / CesiumMan 2차 | 0.766→0.000 | 62.679→66.155 | 3.667→3.667 |

- 최종 runner `-G0 -N 3 -NoOpen -OutDir Tools/CutPerf/out/G1a`: 206.009초, 14/15 실행·26요청 게시, upload-wait 52개 행 모두 0.000 ms. 종료 1은 기존 Xbot-MultiThigh `0xC0000409` 한 건이며 비교에서 제외했다. 업로드 동기 대기는 제거됐으나 전체 게시 시간은 여러 표본에서 증가했으므로 전체 지연 개선·비열화 없음은 입증하지 못했다.
- 출력 동등성: HEAD `959b1bbc` 빌드와 G1a 빌드에서 Xbot 허벅지·CesiumMan 허벅지·CesiumMan 위팔을 `-cut-perf-quiet 0`으로 실행해 `MI3a`·`MI3f`·`MI4d`·`MI3c`·`MI3d`·cut result 15줄씩(시간 제외)을 비교했다. 세 장면 모두 동일하다(Xbot body0/body1 1453/869, invalid·idMismatch·fallback 0). 로그는 `%TEMP%/g1a-equivalence-20261126/`.
- Codex 리뷰(구현과 다른 실행): 사소 1건(아래 알려진 한계) 외 문제 없음.

- 알려진 한계: 업로드 메모리 확보 중 예외(할당 실패) 시 classify context가 제출되지 않는다. 기존 InitializeBuffer 경로와 같은 수준이며 할당 실패는 엔진 계속 실행이 어려운 상황이라 수정하지 않았다.
- Release 최종 빌드 성공(24.585초). 수정 구간 밖 기존 코드의 C4267/C4456/C4100/C4756 경고 및 post-build `pwsh.exe` 부재 메시지는 남는다. LatticeTopologyTests PASS(빌드·실행 2.152초). Release Xbot 오른쪽 허벅지 `-debug 1` 실행 종료 0(14.562초): D3D12 ERROR 0, 기존 DXGI GetContainingOutput ERROR 1, 기존 D3D12 WARNING #1008 480행.
- Debug용 Jolt는 캐시 소스로 빌드 성공(17.520초, 다운로드 없음), Debug 엔진 빌드 성공(156.589초, 수정 범위 밖 C4819 등 경고는 남음). 중간 폴더 재귀 삭제는 자동 승인 검토에서 `blocked by policy`로 거부됐다. 더 제한된 Debug 구성 MSBuild `Clean`은 종료 0으로 완료해 컴파일 산출물을 정리했으며 `Intermediate`에는 추적/로그 9파일 52,023 B가 남았다. Debug 출력도 Clean 대상이며 Release 실행 파일은 보존했다.
- 근거: `Tools/CutPerf/out/G1a/`의 `G0-comparison.csv`, runner CSV·텍스트·`raw/`, `validation-{cut,debug}.log`. 빈 side 경로 보완 전 첫 측정(208.612초)은 `G1a-first/`에 별도 보존했고 위 표에는 포함하지 않았다. 빌드 상세 로그는 `%TEMP%/g1a-*.log`. 별도 새 세션 리뷰·커밋·푸시는 하지 않았다. G1b counter 대기 제거는 후속이다.

### G1b 결과

2026-11-26, `master` / HEAD `d6c84d3a` + 미커밋. `ClassifyCutBodies`의 side별 counter/readback은 `BodyCompactJob`이 소유한다. 분류는 `Finish(false)`로 제출하고, 같은 DIRECT 큐의 후속 압축 fence가 완료된 `PollBodyCompaction`에서 카운터를 읽어 `job.Dropped`를 합산한다. MI3a·MI3f·MI4d 로그 문자열·값과 `sum + dropped == input` 검사, invalid-body status bit 1 실패 처리는 유지한다.

아래는 G1a→G1b 평균(ms)이다. counter는 요청당 합계(각 2회), 게시는 요청→게시 시간이다. 허벅지 N=3, 위팔·척추 N=1이며 ON/OFF는 GPU 타이머 설정이다.

| 설정 / 장면 | counter wait ms (G1a→G1b) | 게시 ms (G1a→G1b) |
| --- | --- | --- |
| OFF / Xbot 1차 | 0.742→0.000 | 489.020→721.400 |
| OFF / Xbot 2차 | 0.310→0.000 | 65.168→61.238 |
| ON / Xbot 1차 | 0.621→0.000 | 504.977→841.023 |
| ON / Xbot 2차 | 0.302→0.000 | 56.905→59.329 |
| OFF / CesiumMan 1차 | 0.560→0.000 | 459.838→750.884 |
| OFF / CesiumMan 2차 | 0.292→0.000 | 66.155→70.716 |
| ON / CesiumMan 1차 | 1.035→0.000 | 624.096→780.630 |
| ON / CesiumMan 2차 | 0.320→0.000 | 74.339→67.445 |
| ON / CesiumMan 위팔 | 0.478→0.000 | 519.622→845.432 |
| ON / CesiumMan 척추 | 0.575→0.000 | 1185.650→759.902 |

- runner: 절대 경로 `OutDir`로 `-G0 -N 3 -NoOpen` 실행, 229.051초, 14/15 실행·26요청 게시. 종료 1은 기존 Xbot-MultiThigh `0xC0000409` 한 건이다. counter-wait 52개 기록 모두 0.000 ms이며, 분류 동기 대기는 요청당 2→0회(G1a까지 포함하면 4→0회)다. 게시 시간은 증가·감소가 섞여 전체 지연 개선·비열화 없음은 입증하지 못했다.
- 동등성 캡처: 지정된 G1a `working-<name>/command.txt` 인수를 그대로 사용해 최종 Release의 Xbot-Thigh·CesiumMan-Thigh·CesiumMan-UpperArm을 실행, 모두 종료 0(각 8.351/9.387/9.404초). `%TEMP%/g1b-equivalence/<name>/debug.log`에 보존했다. G1a 로그와 결과 15줄(시간 제외)을 비교해 세 장면 모두 내용·순서가 같다. 위팔은 input=712, dropped=54, output=658, 합계 PASS/status=0이다.
- Release Xbot 허벅지 `-debug 1` 1회: 종료 0, 10.637초, D3D12 ERROR 0, 기존 DXGI GetContainingOutput ERROR 1, 기존 D3D12 WARNING #1008 180행. 전체 validation 무오류로 판정하지 않는다.
- 빌드·CPU 검증: VS2022 Developer 환경에서 최종 Release 성공(21.826초, 오류 0/기존 경고 11), Debug 성공(151.994초, 오류 0/기존 경고 264, Jolt Debug 캐시 사용). 이후 Debug 구성 MSBuild `Clean` 성공(3.271초), Release exe 보존. 파일 헤더 명령과 같은 옵션으로 TEMP에 빌드한 `LatticeTopologyTests` PASS(빌드·실행 2.020초). 상세 로그는 `%TEMP%/g1b-{release-build,debug-build,debug-clean,lattice-tests}.log`.
- 수명: job이 counter/readback·분류 입력/표/몸체 리소스 참조를 유지한다. 정상 완료는 압축 fence 뒤 해제, 취소·압축 전 예외는 최근 분류 fence까지 retired job으로 유지한다. 분류 기록 중 예외도 부분 명령을 비동기 제출해 fence를 남긴다. 제출 후 빈 G0 계측은 try/catch 밖에 두어 로그 할당 예외가 완료된 context를 재제출하지 않게 했으며, 종료 시 미완료 job 소멸자는 기존 fence 대기를 유지한다. 취소·할당 실패 강제 주입 및 별도 새 세션 리뷰는 미실행이다.
- 근거: `Tools/CutPerf/out/G1b/`의 runner CSV·raw·`G1a-comparison.csv`·`validation-{cut,debug}.log`, `%TEMP%/g1b-*.log`. 최초 상대 `OutDir` 실행은 로그 생성 실패(204.255초)로 제외하고 절대 경로로 재실행했다(`runs.txt`는 이전 실행 기록도 포함하며 마지막 15행이 최종 실행). Git 쓰기 명령·커밋·푸시는 수행하지 않았다.
- Codex 리뷰(구현과 다른 실행): 문제 없음.

### G0·G1 이후 결정 (2026-11-26)

- **G2~G5는 보류한다.** G0 측정에서 GPU로 옮기려던 CPU 연산의 합은 요청당 약 0.3 ms다(재스키닝·부호 0.02~0.07, `LabelPatchIslands` 0.06~0.23, `RelabelCapLoops` 0.01~0.04, `DiagnoseCapLoops` 0.01 이하). 같은 장면의 요청→게시 시간은 실행마다 수백 ms씩 흔들린다. 옮겨도 측정으로 구분할 수 없고, GPU dispatch 추가·verify 경로·fallback 유지 비용만 늘어난다.
- G1a·G1b로 분류 동기 대기 4회(요청당 약 1~6 ms)는 0이 됐다. 출력은 세 장면에서 HEAD와 같다.
- 실제 비용은 첫 절단에 몰려 있다: `BuildPatchSkinVertices` 1차 116~128 ms(2차 4~5 ms), `FinishCutResult` 1차 233~259 ms(2차 9~13 ms), `DiagnoseIslands` 1차 Xbot 12 ms 중 재스키닝·부호는 0.05 ms. 첫 사용 초기화(셰이더·PSO·버퍼 생성 등)로 보이며 원인은 미확정이다. 다음 작업은 이 첫 절단 비용의 원인 조사이며 별도 결정 문서로 다룬다.
- 기존 결함: Xbot 데모 1 + `-skeletal-multi-cut 1`은 G0 이전 `b74abc49`에서도 `0xC0000409`로 종료한다(CesiumMan 다중 허벅지는 정상). 이 작업과 무관하며 별도로 다룬다.

## 완료 조건

1. 비교 지점은 네 곳이다. 장면별 `-island-gpu-verify 1`에서 모두 불일치 0이어야 한다. 장면은 Xbot 허벅지, CesiumMan 위팔, Xbot 다중 허벅지(A/B1/B2), 척추 절단이다.
   - 보정 전 `TriangleSideIsland`·`TriangleSideBody`·`SideDefault`
   - 보정 후 같은 세 표
   - cap leader 표
   - body별 최종 삼각형·cap 수
2. 강제 실패 시험(K 초과, 근접 표차, overflow 한도, 취소, stale)에서 CPU 재계산 또는 기존 A/B 결과 보존 계약이 지켜지고, 그 사실이 로그에 남는다. 미수렴 결과는 게시하지 않는다.
3. `-island-gpu 1`의 요청→게시 시간과 프레임 수가 G0 기준값보다 나쁘지 않다. G1 대상 대기 4회가 0이다.
4. 단계마다 Debug·Release 빌드, D3D12 validation ERROR 0, `LatticeTopologyTests` 통과.
5. 이 문서 상태 줄, `Docs/README.md` 행, `Docs/Current/CodeMap.md` 격자 절을 단계마다 갱신한다.

## 범위 밖

- 생성 조각과 다음 패치를 병합하는 반복 절단에서의 섬 판정, 잘린 조각 재절단 경로의 섬 구분.
- 6/26 이웃 셀 그래프(`BuildLatticeCellGraph`) 방식 채택.
- 로드 시 topology 생성의 GPU화.
