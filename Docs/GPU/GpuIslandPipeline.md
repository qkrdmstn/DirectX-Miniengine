상태: 설계안 — 기준 HEAD `b74abc49`, Codex 리뷰 반영 (2026-11-26)

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
- 예상 시간: Codex 리뷰 2~5분(이번 세션 측정). 빌드·회귀 실행 시간은 G0에서 재고 이 표에 적는다.

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
