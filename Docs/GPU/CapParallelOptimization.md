상태: 진행 중 — 단계 1·1b `6f328257`; 단계 3은 [CapScanMarking.md](CapScanMarking.md) S1 `3a17454f`·S2 `1a7d77ed`로 5060 Ti medianGpuMs 1.70 ms(목표 ≤ 2 ms 달성); 1c·S2b·S3 미착수 (2026-10-30)

# Cap 삼각분할 병렬화 개선

## 1. 목표

- Bunny(30,338 삼각형, y=0, 경계 370, loop 1)의 `medianGpuMs`를 RTX 5060 Ti에서 3.889 ms → **2 ms 이하**로 줄인다.
- 기준은 GPU timestamp 합(`CutJob::Poll`의 `GpuMilliseconds`)이다. `medianReadyMs`(요청→준비)는 이번 목표가 아니다.
- 이번 작업은 cap 단계(`StageMs[2]`, `CapEarsCS` 1회)만 바꾼다. clip·boundary·finalize는 건드리지 않는다.
- 이전 MX450 측정에서 cap은 GPU 전체의 약 75%였다(5.38 중 4.06 ms). 같은 비율이면 2 ms에는 cap 약 2.84배 가속이 필요하다. **이 비율은 가설이다.** 5060 Ti의 단계별 기준값(`Static cut stages ms`)을 먼저 받아 확정한다.
- 스레드당 후보 수 감소를 그대로 시간 단축 배율로 보지 않는다. tid 0 직렬 구간·라운드 의존·barrier·목록 압축 비용이 남는다.

## 2. 현재 구조 (조사 메모 요약)

- `CapEarsCS::main`은 `[numthreads(64,1,1)]`, 유효 loop 하나에 그룹 하나다. loop가 1개면 SM 하나만 일한다.
- 한 라운드: reflex mask → ear 판정 → 해시 우선순위 선택 → 제거·연결 갱신. 라운드당 barrier 7회, 상한 `min(count,256)`.
- ear 판정은 후보마다 `k=0..count-1` 전체 슬롯을 직렬로 돌며 활성 reflex/공선 점만 `InTriangle` 검사한다(`AllPoints`면 모든 활성 점).
- 370슬롯을 64스레드가 나눠 스레드당 5~6 후보를 맡는다. 후보 하나당 내부 순회가 370회이므로 ear 판정이 라운드의 대부분으로 추정한다(미계측).
- Mark-and-Cut(`eb74c0bb`)은 master에 병합되지 않았고, MX450에서 해시 선택보다 cap이 약 35% 느렸다. **이번 개선 대상은 master의 해시 선택 경로다.**

## 3. 결정과 이유

| 결정 | 이유 |
| --- | --- |
| 먼저 **결과가 바뀌지 않는 최적화**만 한다 | 결과 불변 = 같은 입력에서 라운드별 ear 선택과 Ring 슬롯별 cap 코너가 같고, 최종 출력은 winding·속성·provenance를 포함한 **삼각형 다중집합**이 같다. 출력 버퍼 순서는 `EmitTriangle`의 원자 예약에 의존하므로 비교하지 않는다 |
| 결과 불변은 cap 삼각형 checksum으로 확인 | 기존 cap 면적 검사는 허용오차가 있고 대각선 차이를 잡지 못한다. `-cut-validate`에서 cap 삼각형을 순서 무관 checksum으로 출력해 64/256/512 실행 로그를 비교한다 |
| 단계 1: 그룹 크기 64 → 상수 `CAP_THREADS`(256 기본, 512 비교) | 후보 판정 직렬 부담을 스레드당 5~6개 → 1~2개로 줄인다. 큰 GPU일수록 한 SM 안의 warp 수가 늘어 지연을 숨긴다. 공유 메모리(28,696 B)는 그대로이고 C++ dispatch 변경이 없다. cache·reflex·ear·선택·제거의 **다섯 stride를 모두** 바꿔야 중복 처리가 없다. `SerialMode==1`의 stride 1과 `min(count,256)` 상한은 유지한다 |
| 단계 2: 라운드마다 검사 대상 점 목록을 groupshared에 압축하고 ear 판정은 그 목록만 순회 | 대상은 기존 마스크와 **정확히 같게** `Active(k)!=0 && turn<=0`(공선 포함), `AllPoints!=0`이면 모든 활성 점이다. `k!=a/i/c`와 `InTriangle`의 경계 포함 비교도 그대로 둔다. 존재 여부 검사라 순서는 결과에 영향이 없다. **`Cached!=0` 경로에만** 적용하고 캐시 미사용(count>1024)은 기존 전체 순회를 유지한다 |
| 라운드 수를 줄이는 선택 규칙 변경(Mark-and-Cut, 독립 집합 변경)은 **보류** | 결과 삼각분할이 바뀌어 안전성 검증 범위가 커진다. 단계 1·2 측정 뒤 cap이 여전히 목표를 막을 때만 별도 결정한다 |
| wave intrinsic은 단계 2의 목록 압축에만 선택적으로 사용 | SM6.2로 사용 가능하다. 다만 wave 사이 의존은 barrier가 필요하므로 groupshared 원자 카운터가 더 단순하면 그것을 쓴다 |
| 여러 loop 압축 목록·간접 dispatch는 하지 않음 | Bunny 목표에 영향이 없고 C++ 준비 단계가 추가된다 |

## 4. 인터페이스 골격

```hlsl
// CapEarsCS.hlsl
#define CAP_THREADS 256          // 단계 1: 기존 리터럴 64의 numthreads·stride를 모두 이 상수로
[numthreads(CAP_THREADS,1,1)]
void main(...);

// 단계 2
// 새 1024 배열을 추가하면 32,796 B로 32 KiB를 28 B 넘는다.
// ear 판정 단계에서 CachedSelected 저장소를 목록으로 재사용하고, 선택 단계 전에 다시 마스크로 쓴다.
// 순서: 카운터 초기화 → barrier → 원자 예약·목록 기록 → barrier → 판정. 비활성 lane도 barrier에 참여한다.
groupshared uint TestCount;
```

- `SerialMode==1`의 stride 1 경로는 그대로 둔다.
- C++(`GpuMeshCutter::Submit`)의 cap `Dispatch` 인자는 그룹 수이므로 바꾸지 않는다. 다른 셰이더의 `Dispatch1D(...,64)`는 건드리지 않는다.
- groupshared 합계는 32 KiB 이하를 유지하고 컴파일된 사용량으로 확인한다.
- UAV 경로의 `AllMemoryBarrierWithGroupSync`는 유지한다.

## 5. 단계

| 단계 | 내용 | 변경 파일 | 예상 시간 |
| --- | --- | --- | --- |
| 1 | `CAP_THREADS` 상수화, 256 기본. 512는 상수만 바꿔 MX450 비교 측정. `-cut-validate`의 cap 삼각형 checksum 출력 추가 | `CapEarsCS.hlsl`, `tests/GpuCutValidation.cpp` | Codex 구현 5~10분 + 리뷰 2~5분 + 빌드 약 2분 + MX450 실행 수 분 |
| 1b | cap 선택 결정성: `Priority`와 잔여 5개 이하 단일 선택의 키를 슬롯 ID 대신 점 위치 비트로 바꾼다(아래 8절) | `CapEarsCS.hlsl` | 단계 1과 같음 + 반복 실행 검증 |
| 1c | 경계 크기별 adaptive 그룹 크기: loop 정점 수로 분류해 크기별 셰이더 변형을 간접 dispatch(아래 9절). 착수 전 Codex 조사 | `CapEarsCS*.hlsl`, 분류 셰이더, `GpuMeshCutter.*`, vcxproj | 조사 후 단계 분할 |
| 2 | 라운드별 reflex 목록 압축, ear 판정이 목록만 순회 | `CapEarsCS.hlsl` | 위와 같음 |
| 3 | 라운드 감소: 해시 선택을 누적합 기반 교대 표시로 교체(S1 배치·S2 선택·S3 목록 압축, 단계 2를 흡수) | [CapScanMarking.md](CapScanMarking.md) | 그 문서 5절 |

각 단계 후 멈추고 사용자가 읽고 커밋한 다음 진행한다. 5060 Ti 측정은 사용자가 한다(이 작업 PC는 MX450).

## 6. 완료 조건

- 단계마다 Debug/Release 빌드 exit 0, `git diff --check` 통과.
- MX450 Debug `-model Models/bunny.glb -debug 1 -cut 1 -cut-y 0 -cut-validate 1 -cut-expect-loops 1 -cut-expect-publications 1` 통과, 새 D3D12 ERROR/CORRUPTION 없음. Cube 같은 명령(`Models/cube.glb`)도 통과.
- 결과 불변 확인: 아래 입력에서 64(변경 전)/256/512의 cap checksum, `segments/loops/uncappedSegments`가 같다.
  - Cube y=0, Cube `-cut-angle 45`, Bunny y=0, Bunny y=-240(열린 경계), Bunny y=-230(loop2), Bunny `-cut 2` 재절단(loop2), Debug와 Release 모두.
  - 총 슬롯 1023/1024/1025 경계, `SerialMode` 1, `Remaining` 3~6 직접 유도 입력은 현재 만들 수단이 없어 검증 공백으로 기록한다.
- 성능: MX450 Release `-model Models/bunny.glb -debug 0 -cut-benchmark 20 -cut-y 0 -cut-expect-publications 1`을 변경 전/후 각 3회 실행해 `medianGpuMs`와 cap 단계 값의 중앙값을 기록한다. 3회 범위가 겹치면 개선으로 판정하지 않는다.
- 최종 목표 판정은 사용자의 5060 Ti 같은 명령 결과로 한다. cap 단계만으로 2 ms에 못 미치면 그 값과 남은 단계 비중을 기록하고 3단계 여부를 다시 정한다.

## 7. 단계 1 실행 기록 (2026-10-30, 미커밋)

- `CapEarsCS::main`의 그룹 크기와 다섯 stride만 `CAP_THREADS`로 바꿨다. 기본 256, `#ifndef` 재정의 가능이다. 64로 치환하면 주석·상수 선언을 제외한 실행 토큰은 기준 커밋과 같고, SerialMode·라운드 상한·barrier·C++ Dispatch는 유지한다. 컴파일된 공유 메모리 선언 합계는 28,696 B다.
- `CutJob::ValidateDiagnostics`에서 `PrintCapChecksum`을 호출한다. 위치의 32비트 원시 값으로 winding을 유지하는 순환 회전을 정규화하고, 삼각형별 FNV-1a 64비트 해시를 정렬 후 결합한다. 기존 검사는 유지한다. 추출한 실제 C++ 함수의 순서·회전 불변과 winding·1비트·부호 있는 0·중복·빈 cap 구분 검사도 통과했다.
- MX450 / 드라이버 591.86. 64·256·512 각각 Debug/Release의 여섯 입력, 총 36회에서 기존 기하·게시 검사 통과, D3D12 ERROR/CORRUPTION 없음. 옵션 조정은 없고 `cut-expect-loops`는 최소값이다. 완료 게시 marker 뒤 helper가 실행한 프로세스를 종료했으므로 정상 종료 검증은 아니다.
- Cube 두 입력의 checksum은 모든 설정에서 같다. Bunny 네 입력은 Debug/Release 각각에서도 설정 간 다르다. 같은 64/Debug 바이너리의 y=0 반복 세 번도 cap 366/364/365개와 서로 다른 checksum을 냈다. **결과 불변 완료 조건은 미충족**이다. 경계 슬롯의 원자 배정(`ClipTrianglesCS::main`)과 슬롯 ID 기반 `Priority`가 원인 후보이며 원인은 확정하지 않았다. 선택 규칙 변경이나 재현성 수정은 이번 단계에 넣지 않았다.
- 일반 입력의 segments/loops/uncappedSegments는 Cube 8/1/0, Bunny y=0 370/1/0, y=-240 606/1/207, y=-230 502/2/49로 같다. 재절단 두 번째 segments는 64/256/512 순으로 Debug 181/185/187, Release 188/180/185이며 모두 loops=2, uncappedSegments=0이다.

Release `-debug 0` 성능이다. 각 칸은 **medianGpuMs / cap 중앙값(ms)**이며, cap은 warmup 3개를 제외한 측정 20개의 산술 중앙값이다.

| CAP_THREADS | 1회 | 2회 | 3회 |
| --- | --- | --- | --- |
| 64 | 9.161056 / 6.394864 | 6.881440 / 4.130064 | 5.720576 / 4.116288 |
| 256 | 3.324864 / 1.629984 | 4.145952 / 1.773104 | 6.438944 / 3.375952 |
| 512 | 3.014656 / 1.192816 | 3.042656 / 1.226448 | 3.054016 / 1.216672 |

- 256의 전체 GPU 시간 범위는 64와 겹치므로 개선 확정은 하지 않는다. 512는 이번 측정에서 64보다 낮았으나 결과 불변은 미확인이다. 최종 기본값은 결정대로 256으로 복원한다. 단계 2는 미착수다.
- 지정 솔루션의 Debug/Release 빌드에는 `MSBUILDDISABLENODEREUSE=1`을 적용한다. 초기 Jolt MSB3491 접근 거부가 이 설정으로 해소됐다. 설정별 빌드·실행 원본 로그, 36회 checksum/경계 통계와 성능 표는 저장소 밖 `%TEMP%\cap-parallel-step1\`의 `measurements.md`, `results.json`에 있다.
- 공백: 슬롯 1023/1024/1025, SerialMode 1, Remaining 3~6의 통제된 직접 입력과 5060 Ti 측정은 없다. 위치 checksum으로 라운드별 선택·속성·provenance 동일성까지 입증하지는 못한다.

## 8. 단계 1b 결정: cap 선택 결정성 (2026-10-30)

문제: `ClipTrianglesCS::main`이 경계 슬롯 번호를 `Meta.InterlockedAdd(4,1,at)` 실행 순서로 배정한다. `CapEarsCS`의 `Priority(id,round)`와 잔여 5개 이하의 "가장 작은 슬롯 하나만 남기기"가 이 번호를 쓰므로, 같은 입력에서도 실행마다 다른 ear가 선택된다. 공선 제거(`Kind==2`)는 삼각형을 만들지 않아 선택 순서가 삼각형 개수도 바꾼다. 변경 전 64 설정 반복 실행에서 Bunny cap 삼각형이 366/364/365개였다.

| 결정 | 이유 |
| --- | --- |
| 선택 키를 슬롯 ID 대신 **점의 투영 2D 위치 비트**(`Ring` +24, `Point2`)에서 만든다. `Priority(key,round)`, 동률은 위치 비트 사전순 → 슬롯 ID 순 | 위치는 원본 삼각형 하나에서 계산되어 실행마다 같다. Ring에 새 필드가 필요 없고 32 KiB groupshared 예산도 그대로다 |
| 잔여 5개 이하 단일 선택도 같은 키의 최솟값으로 고른다 | 슬롯 순서 의존이 남으면 마지막 몇 라운드가 여전히 실행마다 달라진다 |
| 슬롯 배정(`InterlockedAdd`)은 바꾸지 않는다 | 결정적 배정은 prefix sum 단계가 추가되는 더 큰 변경이다. 선택 키만 안정하면 cap 결과는 슬롯 위치와 무관하다 |
| 같은 위치를 가진 서로 다른 경계 점(접촉·중복)은 슬롯 ID 동률 처리가 남는다 | 정상 다양체 경계에서는 생기지 않는다. 남는 비결정성으로 문서에 기록한다 |

완료 조건:
- 같은 바이너리 반복 3회에서 Bunny y=0, y=-240, y=-230, 재절단의 cap checksum과 삼각형 수가 같다(Debug, Release 각각).
- 그 뒤 64/256/512 사이 checksum이 같다(같은 빌드 구성 안에서). Debug와 Release 사이 차이는 셰이더 최적화의 부동소수 차이일 수 있어 판정에 쓰지 않는다.
- 기존 검증(`-cut-validate`, 면적·winding·부모 보존) 통과, D3D12 ERROR/CORRUPTION 없음.
- 성능: 64/512 Release 벤치 각 3회, 키 계산 추가로 cap 시간이 단계 1 대비 의미 있게 늘지 않는지 기록.

## 9. 단계 1c 방향: 경계 크기별 adaptive 그룹 크기 (2026-10-30, 설계 전)

요구: 게임에서는 절단마다 경계 크기가 다르다(Cube 8, Bunny 370, 열린 Bunny 606). 고정 그룹 크기 하나는 작은 경계에서 barrier 비용을, 큰 경계에서 직렬 부담을 남긴다. 그룹 크기가 경계 크기를 따라가야 한다.

제약: `numthreads`는 컴파일 상수이고, loop 정점 수는 GPU가 절단 중에 계산하므로 CPU는 dispatch 시점에 모른다. CPU readback은 프레임 지연을 만든다.

방향(세부는 Codex 조사 뒤 결정):
- 같은 `CapEarsCS` 본문을 `CAP_THREADS` 64/256/512/1024로 컴파일한 변형 PSO를 둔다.
- 경계 연결(`RankBoundaryCS`) 뒤 작은 분류 pass가 loop별 정점 수를 세고, 크기 구간별 leader 목록과 간접 dispatch 인자를 GPU에 쓴다. 구간은 "정점 수 이상인 가장 작은 변형"이며 1024 초과는 1024 변형을 쓴다.
- C++은 현재의 `MaxSegments` 그룹 직접 dispatch를 구간별 간접 dispatch로 바꾼다. 빈 구간은 그룹 0이라 거의 비용이 없고, 지금처럼 빈 그룹 수천 개를 띄우지 않는다.
- 결과 불변은 단계 1b의 결정성이 전제다: 어떤 변형을 써도 checksum이 같아야 한다.
- 위험: 1024 변형의 스레드당 레지스터 한도(SM 65,536개 / 1024 = 64개) 초과 시 spill. 컴파일 결과와 실측으로 확인하고, 넘으면 최대 변형을 512로 둔다.

조사 항목: 셰이더 변형 컴파일 방식(빌드 시 헤더 생성 규칙, vcxproj 등록), MiniEngine의 `DispatchIndirect` 사용법, loop 정점 수를 이미 계산하는 곳이 있는지와 Ring의 빈 필드, 여러 loop·1024 초과 경계 처리, 각 변형의 레지스터 사용량 확인 방법.

## 10. 단계 1b 실행 기록 (2026-10-30, 미커밋)

MX450 / 드라이버 591.86. 로그는 `%TEMP%/cap-parallel-step1b/`.

- 결정성: CAP_THREADS 256에서 Bunny y=0·-240·-230·재절단을 Debug·Release 각 3회 반복해 cap checksum과 삼각형 수가 모두 같았다. Bunny y=0은 양쪽 364개.
- 그룹 크기: 64/256/512/1024에서 Cube 2개·Bunny 4개 입력의 checksum이 같은 구성(Debug 또는 Release) 안에서 모두 같았다. 64/512/1024의 반복 실행은 1회씩이다.
- 기존 검증·부모 보존·완료 marker 통과, 새 D3D12 ERROR/CORRUPTION 없음.
- 성능(medianGpuMs / cap 중앙값 ms): 64는 6.93~7.06 / 5.10~5.23, 512는 3.09~7.00 / 1.26~2.37, 1024는 3.08 / 1.26(1회). 512의 변동이 커서 단계 1 대비 키 계산 비용 증가 여부는 판정하지 않는다. 계획한 64/512/1024 교차 3회 중 1회만 완료했다.
- 1024: DXC 컴파일 성공, groupshared 28,696 B. 하드웨어 레지스터 수·spill은 DXIL로 확인할 수 없다.
- 남은 슬롯 순서 의존: 방향 면적(`area`) 누산과 마지막 3점 기록 슬롯. 이번 입력에서는 결과 차이가 없었고, 면적 상쇄가 큰 경계에서의 영향은 미검증이다. checksum은 위치·winding만 보며 provenance 동일성은 별도다.
- 사고: 측정 스크립트가 C: 드라이브 용량 부족(여유 약 2.4 GB)으로 셰이더 원복 쓰기에 실패해 `CapEarsCS.hlsl`이 0바이트가 됐다. 단계 1 백업에 1b 변경을 다시 적용해 복구했고, Release 재빌드 Bunny y=0 checksum이 위 기록과 같았다.
