상태: G4a 구현·CPU 테스트 완료, Debug 빌드·validation 실행 미수행 (런타임 미연결) (미커밋, 2026-11-26); G4b·G4c 후속, G5 보류

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
   - 복사 offset은 Poll 뒤 게시 준비에서 정해진다. 고정 평면이면 `d = d0 + plane.x·offset`이나, 현재 `SkeletalPatchCopy::Update`는 평면도 같이 이동한다. 실제 보정은 `d = d0 + plane.x·offset + (plane.w - submitPlane.w)`이며 두 이동 항은 수학적으로 상쇄된다. `|d| ≤ 1e-5`이면 0으로 둔다.
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
    StructuredBuffer CornerDistance;     // 추출 제출에서 구한 삼각형별 float3 d0
    StructuredBuffer CornerCell;         // 같은 출처의 삼각형별 uint3 셀 id
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
| G3b | 첫 등장 번호 + verify 섬 표 비교 | 완료: Release `TriangleSideIsland`·섬 개수 일치, K=1 번호 생략 |
| G4a | CPU 정수 점수 함수 + LatticeTopologyTests(근접 판정·overflow·동점·빈 패치·없는 side) | 구현·CPU 테스트 완료, Debug 빌드·validation 실행 미수행 (런타임 미연결): 2011 사례 PASS, 비근접 섬 4941개 oracle 일치 |
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

- 사용자 결정(2026-11-26): G2~G4는 진행한다. G5는 보류. G0 측정에서 GPU로 옮기려던 CPU 연산의 합은 요청당 약 0.3 ms다(재스키닝·부호 0.02~0.07, `LabelPatchIslands` 0.06~0.23, `RelabelCapLoops` 0.01~0.04, `DiagnoseCapLoops` 0.01 이하). 같은 장면의 요청→게시 시간은 실행마다 수백 ms씩 흔들린다. 옮겨도 측정으로 구분할 수 없고, GPU dispatch 추가·verify 경로·fallback 유지 비용만 늘어난다.
- G1a·G1b로 분류 동기 대기 4회(요청당 약 1~6 ms)는 0이 됐다. 출력은 세 장면에서 HEAD와 같다.
- 실제 비용은 첫 절단에 몰려 있다: `BuildPatchSkinVertices` 1차 116~128 ms(2차 4~5 ms), `FinishCutResult` 1차 233~259 ms(2차 9~13 ms), `DiagnoseIslands` 1차 Xbot 12 ms 중 재스키닝·부호는 0.05 ms. 첫 사용 초기화(셰이더·PSO·버퍼 생성 등)로 보이며 원인은 미확정이다. 첫 절단 비용의 원인 조사는 나중의 별도 후속 작업으로 다룬다. 이 파이프라인의 다음 단계는 G2b다.
- 기존 결함: Xbot 데모 1 + `-skeletal-multi-cut 1`은 G0 이전 `b74abc49`에서도 `0xC0000409`로 종료한다(CesiumMan 다중 허벅지는 정상). 이 작업과 무관하며 별도로 다룬다.

### G2a 결과

- 리뷰 수정: 업로드 임시 원본을 16바이트 배수로 패딩하고 실제 정점 수는 유지했으며, Map 예외의 원인을 FAIL 로그에 기록하고 해당 section 전체를 불일치로 집계한다. 다음 단계는 G2b로 명확히 했다. Release 재빌드 성공(10.175초), 위 개별 모델 옵션으로 Xbot/CesiumMan 재검증 모두 PASS·불일치 0·종료 0(10.046/8.078초); 근거는 `%TEMP%/g2a-review-release-build.log`, `%TEMP%/g2a-validation/review-{Xbot,CesiumMan}-verify.log`다.

2026-11-26, `master` / HEAD `e2aadbe7` + 미커밋. `Model::m_GpuLatticeTopology`가 section별 `StructuredBuffer`(정점당 uint)와 `CellCount`를 소유한다. `LoadModel`은 skin CPU topology 생성 직후 `UploadLatticeTopology`로 한 번 업로드한다. CPU vector는 oracle로 유지하며 정적 모델·빈 topology는 제외하고 빈 section은 빈 슬롯을 보존한다. `Model::Destroy`에서 GPU 격자를 정리한다. 새 파일은 없다.

- `-island-gpu-verify 1`(기본 0)은 로드당 한 번 section별 readback을 동기 완료하고 모든 정점 값을 CPU와 비교한다. Map 실패도 FAIL로 계산하며 section별·전체 정점/바이트 수와 CellCount, 불일치 수를 로그에 남긴다. 검증 후 버퍼는 compute SRV 상태다. 아직 실제 소비자는 없으며 `-island-gpu 0|1` 플러밍은 G2b로 미룬다.
- Release 개별 모델 실행: `-debug 0 -model Models/<Xbot|CesiumMan>.glb -island-gpu-verify 1 -physics-query-exit 180`. 각각 종료 0, 7.750초, 불일치 0/PASS다.

| 모델 | section별 정점 수 | section별 버퍼 bytes | 합계 bytes | CellCount |
| --- | --- | --- | --- | --- |
| Xbot | 12,473 / 15,901 | 49,892 / 63,604 | 113,496 | 3,350 |
| CesiumMan | 3,273 | 13,092 | 13,092 | 1,360 |

- Release validation: `-debug 1 -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:RightUpLeg -skeletal-break-demo-delay 2 -physics-query-exit 480 -island-gpu-verify 1`. 오른쪽 허벅지 절단 1회 A/B 게시·caps=1·uncapped=0·error=0, MI3a invalid/idMismatch=0, 종료 0(14.906초). D3D12 ERROR 0, 기존 DXGI GetContainingOutput ERROR #80 1건, 기존 WARNING #1008 480행이다. 전체 validation 무오류로 판정하지 않는다. NVIDIA GeForce MX450, 드라이버 `32.0.15.9186`(레지스트리 확인).
- VS2022 Developer 환경은 vswhere의 설치 경로와 DevShell DLL로 초기화했다. Release 빌드 성공(116.069초); `Model.h` 변경으로 관련 소스를 재컴파일했다. 기존 C4819/C4267/C4756 등 경고와 post-build `pwsh.exe` 부재 메시지는 남는다.
- Debug 빌드 성공(126.595초), 이어서 Debug 구성 MSBuild `Clean` 성공(3.386초). Release exe는 보존됐다. 파일 헤더 옵션으로 TEMP에 빌드한 `LatticeTopologyTests` PASS(빌드·실행 2.580초). 모든 빌드·Clean·테스트 종료 0, `git diff --check` 통과.
- 근거: `%TEMP%/g2a-validation/{Xbot-verify,CesiumMan-verify,Xbot-validation}.log`, `%TEMP%/g2a-{release-build,debug-build,debug-clean,lattice-tests}.log`. 기존 PowerShell 로그 수집기의 Add-Type 임시 DLL 권한 오류를 피하려고 TEMP의 네이티브 DBWIN 수집기를 빌드해 사용했고 모든 실행의 정상 종료를 기다렸다. 별도 새 세션 리뷰·Git 쓰기·커밋·푸시는 수행하지 않았다.

### G2b 결과

2026-11-26, `master` / HEAD `760d1412` + 미커밋. `SkeletalPatchJob::Submit`은 `-island-gpu 1` 또는 `-island-gpu-verify 1`에서만 추출 직후 UAV barrier를 두고 `IslandDistanceCS`를 같은 DIRECT 제출에 기록한다. 두 옵션 기본값은 0이며 CPU 경로가 계속 출력을 결정한다.

- 리뷰 수정: `InitializeDistance`는 활성 `SkeletalPatchJob::Submit`에서만 거리 root signature·PSO를 만들며 월드 평면도 활성 분기에서만 계산한다. `DiagnoseIslands`는 `Utility::Printf`로 quiet 억제 없이 `Island GPU verify PASS/FAIL`을 출력한다. 부호·셀 불일치 또는 Map 예외는 FAIL이며 예외 내용도 출력한다. 두 Map은 범위 기반 Unmap으로 보호한다.
- 리뷰 수정 검증(2026-11-26): Release 빌드 성공(148.29초, 오류 0; 최종 증분 확인 3.88초). `Run-CutPerf.ps1`의 TEMP 실행 사본에서 Git 조회를 제외하고 단일 절단·quiet=1·`-island-gpu-verify 1 -debug 1`로 Xbot-Thigh/CesiumMan-Thigh/CesiumMan-UpperArm을 실행했다. 세 장면 모두 거리 verify PASS, 부호·셀 불일치 0, D3D12 ERROR/CORRUPTION 0, 종료 0(7.859/8.281/8.531초); 기존 DXGI #80은 각각 1건이다. NVIDIA GeForce MX450 / driver 591.86. 명령·성능 로그·디버그 출력은 `%TEMP%/g2b-review-verification/final-results/raw/`, 빌드 로그는 `%TEMP%/g2b-review-{release-build,final-release-build}.log`에 보존했다.

- section마다 capacity 전체를 dispatch하고 GPU patch count 밖 및 다른 section 슬롯을 건너뛴다. 기존 draw descriptor를 재사용해 `TriangleSources(section, draw, triangle)` → R16/R32 index + baseVertex → PG를 읽는다. G2a section별 SRV를 그대로 바인딩하기 위해 section별 dispatch를 택했으며 슬롯별 쓰기는 겹치지 않는다.
- PG에는 이미 section frame이 적용되어 `world`만 곱한다. 제출 component-local 평면을 `transpose(inverse(world))`로 월드 평면으로 되돌려 `d0`를 계산한다. job 소유 `CornerDistance(float3)`·`CornerCell(uint3)`는 다음 프레임 PG 갱신과 무관하다. readback은 verify에서만 추출 fence에 포함한다.
- `Poll` 완료 후 `DiagnoseIslands`에서 CPU가 쓰는 실제 `plane.WorldPlane`·`worldPosition + (offset,0,0)`와 비교한다. 평면도 X 이동하므로 `w` 변화까지 보정한다. 지정 데모는 모두 offset=0이었다. 부호 기준은 CPU와 같은 `1e-5`이며 CPU 부호·거리·VertexCell을 코너마다 비교한다. G3의 GPU 라벨링 부호 계산은 후속이다.

| Release verify 장면 | corners | signMismatch | maxDistanceDiff | cellMismatch | 실행 초 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Xbot 허벅지 | 6288 | 0 | 4.76837158e-7 | 0 | 10.359 |
| CesiumMan 허벅지 | 1056 | 0 | 4.17232513e-7 | 0 | 13.188 |
| CesiumMan 위팔 | 1620 | 0 | 8.34465027e-7 | 0 | 10.750 |
| CesiumMan 척추 | 1179 | 0 | 7.15255737e-7 | 0 | 10.047 |
| CesiumMan 다중 허벅지 | 1578 | 0 | 3.57627869e-7 | 0 | 12.000 |

- 모두 `-skeletal-break-demo-delay 2 -physics-query-exit 180 -island-gpu-verify 1`, 다중은 `-skeletal-multi-cut 1 -skeletal-union-checks 1` 추가, 종료 0. 개별 명령·로그는 `%TEMP%/g2b-validation/{장면}-command.txt`, `{장면}.log`. 척추는 기존 CPU 출력 `caps=1 uncapped=17 error=0`, 다중은 union 비교 PASS·5개 컴포넌트 준비를 기록했다.
- flags-off 동등성 캡처: 기존 `%TEMP%/g1a-equivalence-20261126/working-<name>/command.txt` 인자를 그대로 사용했다. Xbot-Thigh/CesiumMan-Thigh/CesiumMan-UpperArm 모두 종료 0(10.047/10.203/10.812초). 요청된 native `capture.exe`로 `%TEMP%/g2b-equivalence/<name>/debug.log`와 사용 명령을 저장했으며 diff 판정은 사용자에게 남긴다.
- Release `-debug 1 -island-gpu 1` Xbot 허벅지: 종료 0, 10.203초, **D3D12 ERROR 0**, 기존 DXGI GetContainingOutput ERROR #80 1건, WARNING #1008 180행. GPU NVIDIA GeForce MX450, driver 591.86. 근거 `%TEMP%/g2b-validation/Xbot-validation.log`.
- Release 빌드 성공(52.284초, 오류 0, 기존 경고 51). 근거 `%TEMP%/g2b-release-build.log`. Debug 빌드 성공(152.781초, 오류 0, 기존 경고 264), 이어서 MSBuild Debug Clean 성공(3.796초). TEMP에 빌드한 LatticeTopologyTests PASS(2.669초). 근거 `%TEMP%/g2b-{debug-build,debug-clean,lattice-tests}.log`. `git diff --check` 통과. Git 쓰기 명령과 커밋은 수행하지 않았다.

- 이동 보정 추가 검증: demo는 offset-copy 옵션을 강제로 끄므로 자동 `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1 -island-gpu-verify 1 -physics-query-exit 180 -debug 0`으로 20개 case를 실행했다. offset=1.402922/1.807833, 모든 case 부호·셀 불일치 0, 최대 거리 차이 1.90734863e-6, 종료 0(6.047초). 근거 `%TEMP%/g2b-validation/OffsetCopy-checks.log`, `OffsetCopy-checks-command.txt`.

- 구현 대화를 상속하지 않은 별도 Codex 읽기 전용 리뷰 완료: 추출→거리 의존성, PG 변환, 출처·baseVertex·셀 인덱싱, job 수명, fence 회수, 평면 이동 보정에서 차단 결함 없음. 빌드·실행을 중복하지 않고 위 근거를 사용했다.

### G3a 결과

G3a 구현·Release 검증 완료 (Debug 빌드 미수행, LatticeTopologyTests 해당 없음 — G3a는 CPU 코드 변경 없음).

2026-11-26, 미커밋. `SkeletalPatchJob::Submit`은 GPU 옵션이 켜진 요청에서만 job 소유 `Parent(2*CellCount)`·`Status`를 만들고, `InitializeDistance`에서 UF PSO도 지연 생성한다. `IslandJumpCS` 초기화 모드로 `Parent[i]=i`·Status=0을 기록한 뒤 `IslandDistanceCS`와 같은 제출에서 hook+jump를 `-island-gpu-k N`번(기본 32) 기록한다. 패스 사이 Parent UAV barrier를 둔다.

- `IslandUnion.hlsli`의 `SideNodes`는 CPU `LabelPatchIslands`와 같은 side 존재·영점 코너 규칙과 `1e-5` 임계값을 사용한다. 결정 5의 복사 offset·평면 이동 항이 상쇄되므로 d0를 직접 분류한다.
- `IslandHookCS`는 현재 root에 `InterlockedMin`을 적용하고 `IslandJumpCS`는 원자적으로 부모의 부모로 압축한다. `IslandVerifyCS`는 모든 존재하는 `(t,side)`의 코너 부모가 동일한 최종 root인지 확인한다. 압축 미완료도 보수적으로 미수렴 처리한다.
- `SkeletalPatchJob::Poll`은 기존 추출 fence 완료 뒤 작은 Status를 회수한다. 새 동기 대기는 없으며 `Utility::Printf`로 quiet와 무관하게 수렴 또는 fallback 사유를 출력한다. G3a에서는 성공·실패 모두 기존 CPU 경로가 출력을 결정하며 GPU 라벨은 소비하지 않는다.
- verify에서만 Parent도 회수한다. `DiagnoseIslands`는 존재 여부와 모든 활성 코너의 GPU root↔CPU island ID 양방향 대응을 검사한다. Map은 범위 기반 Unmap으로 보호하며 실패도 로그에 남긴다. 독립 읽기 전용 Codex 리뷰에서 차단 결함 없음.
- Release 빌드 성공(58.01초, 오류 0). 지정 capture 도구·작업 디렉터리로 실행한 아래 5회와 최종 validation 재실행 모두 종료 0. NVIDIA GeForce MX450, 드라이버는 기존 G2b 기록 591.86. 명령·로그·runner·빌드 근거는 `%TEMP%/g3a-verify/`에 보존했다. Debug 빌드·flags-off 비교는 이번 요청 범위에서 제외했다.

| Release 실행 | UF 결과 | partition | 거리·셀 | 실행 초 |
| --- | --- | --- | --- | ---: |
| Xbot-Thigh | converged iterations=32 | PASS mismatch=0 | PASS | 9.153 |
| CesiumMan-Thigh | converged iterations=32 | PASS mismatch=0 | PASS | 9.189 |
| CesiumMan-UpperArm | converged iterations=32 | PASS mismatch=0 | PASS | 10.526 |
| Xbot-Thigh K=1 | fallback reason=not-converged/verify-failed | FAIL mismatch=6118 (미수렴 예상) | PASS | 10.759 |
| Xbot-Thigh debug=1 | converged iterations=32 | PASS mismatch=0 | PASS | 9.884 |

- 최종 Map 보호·include 등록 후 Release 재빌드 성공(32.96초, 오류 0). 최종 실행 파일의 `Xbot-validation-final.log`도 종료 0(10.641초), UF converged iterations=32·거리/partition PASS. `Xbot-validation.log`와 최종 로그 모두 D3D12 ERROR 0, CORRUPTION 0. K=1도 기존 CPU 출력으로 게시·정상 종료했으며 강제 성공 처리하지 않았다. Geometry 변경은 없다.

## G3b 결과

- 디스패치 최대 그룹 수가 D3D12 한계(65,535)를 넘으면 해당 요청의 모든 island GPU 패스를 생략하고 Poll에서 `Island GPU UF fallback reason=dispatch-limit`를 quiet와 무관하게 기록하며 CPU 경로를 유지한다.

- `InitializeDistance`는 GPU 옵션에서만 `IslandNumberCS` 파이프라인을 준비한다. `SkeletalPatchJob::Submit`은 job 소유 RootMin·Prefix·TriangleSideIsland를 만들고 G3a 검증 뒤 같은 제출에 초기화 → root별 최소 키 원자적 누적 → head scan → 블록 합계 scan → 번호 기록의 5개 패스를 기록한다. 패스 사이에는 UAV barrier를 둔다.
- `IslandNumberCS::main`은 Status 첫 uint가 실패이면 즉시 종료한다. 따라서 미수렴에서는 번호를 만들지 않고 G3a CPU fallback을 유지한다. Status 둘째 uint는 수렴한 요청의 섬 개수다. `SideRoot`는 기존 `SideNodes`의 side 존재·영점 규칙을 재사용한다.
- 키는 원본 triangle ID가 아닌 패치 배열 인덱스 `t*2+side`다. root마다 최소 키를 구하고 해당 키만 head로 표시한다. 256키 그룹별 공유 메모리 포함 scan과 블록 합계의 단일 스레드 배타적 scan을 합친다. 10만 삼각형에서 블록은 782개여서 상위 scan이 작고, 전체 키를 단일 스레드로 순회하지 않으면서 결정적인 첫 등장 순서를 보장한다. 최종 번호는 head의 포함 prefix + 블록 offset - 1이며 없는 side는 UINT32_MAX다. uint2는 삼각형별 한 스레드가 기록한다.
- `SkeletalPatchJob::Poll`은 기존 추출 fence 완료 후 보호된 Map으로 Status·개수를 읽는다. verify에서만 표를 같은 fence로 회수하고 `DiagnoseIslands`가 보정 전 `LabelPatchIslands`와 양쪽 side·개수를 정확히 비교한다. 번호 표 Map은 RAII로 보호하며 로그는 quiet를 우회한다. 새 동기 대기는 없다. GPU 표는 아직 소비하지 않으며 CPU 출력 경로는 유지한다. 플래그가 모두 꺼지면 추가 버퍼 생성·dispatch·readback·비교가 없다.
- **Release만 검증했다.** 요청된 MSBuild 명령으로 최초 빌드 44.29초(오류 0), 최종 재빌드 25.72초(오류 0, 기존 코드 위치 경고 11개). Debug 빌드는 하지 않았고 아래 debug=1은 Release 실행 파일의 D3D12 validation 옵션이다. CPU oracle·Geometry 변경이 없어 CPU 테스트는 실행하지 않았다. flags-off 출력 동등성은 사용자 검증 범위다.
- NVIDIA GeForce MX450, 드라이버는 기존 G3a 기록 591.86. 지정 capture 도구·ModelViewer 작업 디렉터리 사용. 명령·로그·runner·빌드 근거는 `%TEMP%/g3b-verify/`에 보존했다.

| Release 실행 | UF | partition | number | 실행 초 |
| --- | --- | --- | --- | ---: |
| Xbot-Thigh | converged K=32 | PASS mismatch=0 | PASS mismatch=0 islands=3/3 | 9.916 |
| CesiumMan-Thigh | converged K=32 | PASS mismatch=0 | PASS mismatch=0 islands=2/2 | 9.471 |
| CesiumMan-UpperArm | converged K=32 | PASS mismatch=0 | PASS mismatch=0 islands=3/3 | 8.935 |
| Xbot-Thigh K=1 | fallback not-converged/verify-failed | FAIL mismatch=5925 (예상) | 생략 | 10.050 |
| Xbot-Thigh debug=1 | converged K=32 | PASS mismatch=0 | PASS mismatch=0 islands=3/3 | 10.483 |
| CesiumMan 연속 좌우 허벅지 (runner 설정) | 두 요청 모두 converged K=32 | 두 요청 모두 PASS mismatch=0 | 두 요청 모두 PASS mismatch=0 islands=2/2, 3/3 | 13.520 |

- 위 6회 모두 종료 0, 거리·셀 verify PASS이며 D3D12 ERROR 0이다. K=1도 기존 CPU 출력으로 정상 종료했다.
- runner에는 CesiumMan 동시 multi-thigh 프리셋은 없고 `CesiumMan`의 `Second=leg_joint_L_1` 연속 절단 설정이 있다. 같은 `-cut-perf-second-bone leg_joint_L_1`·480프레임·성능 로그 활성화로 추가 실행했으며 두 요청 모두 번호·개수 일치, 종료 0·D3D12 ERROR 0이다. 성능 로그 없이 같은 옵션을 실행한 예비 실행은 한 요청만 관측되어 두 요청 검증으로 세지 않았다.

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

## G4a 결과

- G4a: 구현·CPU 테스트 완료, Debug 빌드·validation 실행 미수행 (런타임 미연결).
- `VotePatchIslandsInteger`는 기존 `TriangleSideIsland` 번호를 받아 모든 활성 `(t, side)`의 세 정점 전체를 재투표한다. 런타임 호출은 추가하지 않았다.
- weight 표현은 `SkinCpuVertex::Weights`의 원시 `uint16_t[4]`다. `uint32_t(w) * 4096u / sum`으로 정수 내림 나눗셈하며, 합 65535를 가정하지 않는다. 최대 분자는 268431360, 최대 분모는 262140으로 uint32 범위다. 합 0이면 모든 기여를 건너뛴다.
- 섬·몸체별 uint32 합에서 1·2위 차이 `Margin`을 구하고 `Margin <= 12 * 활성 쌍 수`이면 `Near`다. 공동 1위는 A이며, A가 공동 1위에 없어도 oracle과 같이 A를 반환한다.
- 요청 전체 활성 쌍 수가 340000 이상이면 누적 전에 모든 섬에 `Fallback`을 표시한다. 339999개에서 최대 누적은 4177907712로 uint32 범위다. `Near`는 근접 double 재판정, `Fallback`은 overflow에 의한 요청 전체 CPU 재판정으로 구분한다.
- `ResolvePatchIslandVotes`는 `Near || Fallback`이면 기존 double oracle 몸체, 아니면 정수 몸체를 선택한다. 삼각형 몸체 표·SideDefault를 재구성하며 기본값 동점은 작은 몸체, 없는 side는 UINT32_MAX를 유지한다. oracle 함수·출력은 수정하지 않았다.
- `CheckIntegerVotes`: 2011 사례 PASS / FAIL 0. 고정 seed `0x474461` 무작위 2000 패치, 비근접 섬 4941개의 정수 승자와 최종 섬·삼각형 표·SideDefault가 oracle과 일치했다. 근접 임계값 포함·강제 oracle 선택, `{32767,16383,16385,0}` → `{0,1,1,0}`의 실제 양자화 동점(A)·double 승자(몸체 1) 불일치 해소, A 동점·비A 공동 1위, 양수 SideDefault 동점, 빈 패치·없는 side, overflow 직전·경계를 포함한다.
- 테스트 헤더 명령(`/utf-8 /std:c++17 /EHsc /W4`)으로 TEMP에 빌드·실행 PASS, 컴파일 경고 없음. 기존 LatticeTopologyTests도 PASS. 환경 초기화의 vswhere 탐색 메시지는 있었으나 cl은 정상 실행했다. 빌드·실행 합계 약 5.5초.
- ModelViewer Release `msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Release /p:Platform=Windows` PASS(종료 0, exe 생성). 수정 범위 밖 파일에서 C4819·C4456·C4756·C4100 경고, 환경에서 pwsh.exe 탐색 메시지가 출력됐다. Debug·GPU 런타임/validation·별도 새 세션 리뷰는 미수행. Geometry를 수정하지 않아 PlaneSlicerTests는 미수행. G4b GPU 코드·런타임 연결은 후속이다.
