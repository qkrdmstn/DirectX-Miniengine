상태: 진행 중 — master `6f328257`(단계 1·1b) 기준 S1 구현·MX450 지정 검증 완료(미커밋); S2·S2b·S3 미착수 (2026-10-30)

# Cap 선택을 누적합 기반 교대 표시로 바꾸기

상위 계획은 [CapParallelOptimization.md](CapParallelOptimization.md)다. 이 문서는 그 문서의 "단계 3(라운드 감소)"를 확정한다.

## 1. 목표

- cap ear clipping의 라운드 수를 줄여 cap 시간을 줄인다. 라운드 하나의 시간은 ear 판정 루프가 정하므로(정점당 스레드 하나일 때) 라운드 수가 곧 성능이다.
- 해시 우선순위 선택(라운드당 ear의 약 1/5 제거, 370정점에서 약 22라운드 추정)을, 경계 순서상 **하나 건너 하나** 선택(라운드당 볼록 후보의 최대 1/2, 약 8~9라운드 추정)으로 바꾼다. 추정은 모든 점이 ear에 가깝다는 가정이며 미측정이다.
- 선택은 GPU 병렬로 한다. MC 논문(Eder·Held·Palfrader 2018 §2.3)은 표시를 스레드 하나가 직렬로 하고, 우리 브랜치 `eb74c0bb` 구현도 그래서 느렸다.

## 2. 핵심 아이디어

- 후보(볼록·공선, 활성)에 1, 나머지에 0을 붙이고 **경계 순서로 exclusive scan**하면 그 값이 경계 전체의 후보 번호다.
- 번호가 라운드 parity와 같은 후보만 표시한다. 표시된 후보끼리는 사이에 후보가 하나 이상 있으므로 경계에서 붙지 않는다(논문 §2.3의 관찰).
- scan은 CUDA의 CUB `BlockScan`과 같은 2단계 구성이다: warp 안은 `WavePrefixCountBits`, warp 사이는 warp 합계의 누적합. 결과는 그룹 크기와 무관하다.
- 해시가 없으므로 결과가 실행·그룹 크기와 무관하게 결정적이다.

## 3. 결정과 이유

| 결정 | 이유 |
| --- | --- |
| 공유 메모리 캐시를 **이 loop의 경계 순서**로 배치한다(위치 p = 0..m-1) | 연속한 칸이 경계에서도 연속해야 scan 번호가 경계 순서가 된다. 지금은 전체 슬롯(`count`)을 슬롯 번호 순으로 담는다 |
| 위치 → 슬롯 대응 `CachedSlot[p]`를 둔다 | 삼각형은 지금처럼 Ring의 슬롯 +64에 기록하고 `FinalizeCutCS`는 바꾸지 않는다 |
| Active/Kind/Selected 세 배열을 플래그 비트 하나(`CachedFlags`)로 합친다 | `CachedSlot`(4 KiB)을 더해도 32 KiB 안에 들어가게 한다(28,672 B → 약 24,576 B + 소량) |
| 경계 순서는 cap 시작 때 tid 0이 leader부터 `Next`(+40)를 한 번 따라가 `CachedSlot[p]`를 채워 만든다. 순회 상한 `min(count,4096)`, leader 복귀·슬롯 범위·leader 일치 검사, 실패 시 cap 생략 | 조사 결과 `RankBoundaryCS`는 rank를 남기지 않는다(+48 leader, +56 점프 포인터뿐). 순회는 loop당 한 번이다. 그룹 pointer jumping은 거리 배열과 log m회 동기화가 필요해 더 복잡하다 |
| 캐시 조건을 전체 segment 수가 아니라 **loop 길이 m ≤ 1024**로 바꾼다. m > 1024는 기존 슬롯 순서 경로 | 다중 loop에서도 작은 loop가 캐시를 쓴다. 두 경로의 선택 규칙이 같으므로 결과도 같아야 하며 검증한다 |
| 플래그 압축 word는 **그 위치를 맡은 스레드만 쓴다**. 다른 스레드는 그 단계에서 바뀌지 않는 비트만 읽는다 | 같은 word를 두 스레드가 쓰는 경쟁을 막는다. Next/Prev는 제거 시 서로 다른 스레드가 같은 점의 Prev와 Next를 쓰므로 압축하지 않는다 |
| wave 순서는 tid로 가정하지 않는다. 시작 때 wave마다 `WaveIsFirstLane`이 groupshared 카운터로 wave 번호를 받고, 위치 = wave 번호 × `WaveGetLaneCount()` + `WaveGetLaneIndex()` | tid→lane 대응은 하드웨어가 보장하지 않는다. scan 순서가 lane 순서와 일치해야 한다 |
| `WarpTotal`은 `CAP_THREADS/4`칸. wave 수 ≤ wave 크기면 wave 0이 `WavePrefixSum` 한 번, 아니면 tid 0이 순회 | wave 크기 4~128을 모두 받는다 |
| S1은 배치 변경만 한다: 동률 비교는 `CachedSlot[p]`, 마지막 3점은 최소 원래 슬롯에 기록, 라운드 상한 `min(count,256)` 유지, 삼각형·`+76`·`+84`·`Meta+12` 계약 유지 | 단계 1b와 checksum이 같아야 배치 회귀를 판정할 수 있다. 방향 면적 합산 순서가 경계 순서로 바뀌는 부동소수 차이는 checksum으로 확인한다 |
| 표시 규칙: 라운드 r의 parity q=r&1, 후보 번호 k가 `k%2==q`이면 표시. q=0이고 후보 수 c가 홀수면 마지막 후보(k=c-1)는 제외 | 순환 경계에서 첫 후보와 마지막 후보가 붙는 경우를 막는다. parity를 번갈아 써서 한쪽 parity의 후보가 모두 ear가 아닐 때도 진행한다 |
| 표시된 후보만 ear 판정, 통과하면 제거(공선은 삼각형 없이 제거) | 판정 대상이 절반으로 준다. 판정 대상 점 집합(`turn<=0` 활성 점, `AllPoints`면 활성 전체)과 `InTriangle`은 그대로다 |
| 잔여 보호: 남은 정점 m에서 이번 라운드 제거는 최대 m-3개, 경계 순서상 앞의 것부터 | 기존 "잔여 5개 이하면 하나만"보다 일반적이고, 순서가 결정적이다 |
| 두 라운드 연속 제거 0이면 그 라운드는 모든 후보를 판정하고 경계 순서상 첫 ear 하나만 제거 | 단순 다각형은 ear가 최소 2개(Two-ear 정리)지만 표시 제외 규칙과 수치 오차에 대비한 진행 보장이다 |
| 라운드 상한 `min(m, 4096)`, 미완료 loop는 기존처럼 cap 생략 | 무한 루프·watchdog 방지 |
| loop 정점 수 m > 1024(캐시 불가)는 이번 범위에서 기존 해시 경로를 유지한다 | Bunny 목표에 영향이 없다. 글로벌 메모리 순서 배치는 별도 결정으로 한다 |
| 진단용 라운드 수를 Meta에 기록해 로그로 출력한다 | 1/2 제거 추정을 실측으로 확인한다 |
| S2는 단일 스레드 마무리 없이 3점까지 병렬로 간다. 마무리 기준은 S2b에서 0/8/16/32를 측정해 정한다 | 논문의 20은 CPU 8스레드의 재시작 비용 기준이라 근거가 없다. 갈림점은 barrier 비용과 ear 판정 비용의 비율이고 GPU마다 다르다. 마무리 경로는 스레드 0이 barrier 없이 끝까지 처리하고 나머지는 마지막 barrier 하나에서만 기다린다(브랜치 MC처럼 하나 제거마다 그룹 barrier를 돌지 않는다). 두 GPU에서 0보다 범위가 겹치지 않게 빠를 때만 채택한다 |

## 4. 인터페이스 골격

```hlsl
// CapEarsCS.hlsl
groupshared float2 CachedPoints[1024];        // 경계 순서 위치 p
groupshared uint   CachedNext[1024], CachedPrev[1024];
groupshared uint   CachedFlags[1024];         // bit0 Active, bit1-2 Kind, bit3 Selected, bit4 Reflex
groupshared uint   CachedSlot[1024];          // p -> Ring 슬롯
groupshared uint   WarpTotal[32];             // 블록 scan의 warp 합계 (1024/32)

// 그룹 전체 exclusive scan. 모든 lane이 호출해야 한다(barrier 포함).
// 각 스레드는 연속 위치 [tid*k, tid*k+k)를 맡는다(k = ceil(m / CAP_THREADS)).
uint BlockExclusiveSum(uint value, uint waveSlot, out uint total);  // value = 내 k칸의 합
```

- `BlockExclusiveCount` 순서: ① 각 스레드가 자기 k칸의 1을 직렬로 세어 합계 ② `WavePrefixCountBits`(k=1) 또는 `WavePrefixSum`(k>1)으로 wave 안 exclusive scan ③ 각 wave 마지막 lane이 `WarpTotal[wave]`에 기록 → barrier ④ wave 0이 `WarpTotal`을 `WavePrefixSum`으로 scan → barrier ⑤ 각 스레드가 wave offset + wave 안 값으로 자기 k칸 번호를 매긴다. 라운드당 barrier 2회.
- wave 크기는 32로 고정하지 않고 `WaveGetLaneCount()`로 읽는다(NVIDIA 32, AMD 32/64, Intel 8~32). `WarpTotal` 크기는 `CAP_THREADS / 최소 wave 크기`로 잡고, wave 수가 wave 크기를 넘으면(④를 wave 하나로 못 끝내면) 컴파일 시 지원 범위를 문서화한다. 목표 GPU(NVIDIA)는 512/32=16개다.
- 스레드가 연속 위치 묶음을 맡게 하면 그룹 크기 < m(예: 64스레드, 370정점)에서도 같은 결과가 나온다. 단계 2의 오목 점 목록 압축도 같은 함수를 쓴다.
- C++(`GpuMeshCutter::Submit`)의 cap dispatch는 바꾸지 않는다. Meta에 라운드 수 필드를 추가하면 readback 크기와 `CuttingSystem` 로그만 바꾼다.

## 5. 단계

| 단계 | 내용 | 결과 불변 여부 | 예상 |
| --- | --- | --- | --- |
| S1 | 경계 순서 캐시 배치(tid 0 순회)·`CachedSlot`·플래그 압축·캐시 조건 m ≤ 1024. **선택은 단계 1b의 위치 키 해시 그대로.** scan 함수는 쓰이는 S2에서 만든다 | 1b와 checksum 동일해야 함 | Codex 구현 5~10분 + 리뷰 2~5분 + 빌드 약 1분 + MX450 실행 |
| S2 | wave 번호 배정·`BlockExclusiveSum`, 해시 선택을 scan 교대 표시로 교체, 잔여 보호·parity·진행 보장, 라운드 수 진단, `Priority` 제거 | 결과가 바뀐다(새 기준선) | 위와 같음 |
| S2b | 단일 스레드 마무리 기준 0/8/16/32 비교 측정, 채택 시 상수로 고정 | 기준이 바뀌면 결과가 바뀐다(결정적) | 측정 위주 |
| S3 | 오목 점 목록 압축(상위 문서 단계 2)을 같은 scan으로 | S2와 checksum 동일 | 위와 같음 |

S1은 구조만 바꾸고 결과는 그대로 두어 배치 변경의 회귀를 먼저 잡는다. 이를 위해 단계 1b(결정성)가 기준선으로 필요하다. 상위 문서의 1c(adaptive 그룹 크기)는 S2 이후에 한다. 단계마다 멈추고 사용자가 읽고 커밋한다.

## 6. 완료 조건

- 공통: Debug/Release 빌드 exit 0, `git diff --check`, groupshared 32 KiB 이하(컴파일 결과로 확인), 새 D3D12 ERROR/CORRUPTION 없음.
- 공통 검증 입력: Cube y=0·45°, Bunny y=0·-240·-230·재절단, Debug·Release, `-cut-validate 1 -cut-expect-loops 1`.
- S1: 위 입력에서 단계 1b와 cap checksum 동일, 반복 3회 동일.
- S2: 기존 기하 검증 통과, 반복 3회 checksum 동일, CAP_THREADS 64/256/512에서 checksum 동일, 라운드 수 기록. 성능은 MX450 Release Bunny 벤치를 1b와 교차 순서로 각 3회 재고 범위가 겹치면 개선으로 판정하지 않는다.
- S3: S2와 checksum 동일, 성능 기록.
- 최종 목표(5060 Ti `medianGpuMs` ≤ 2 ms) 판정은 사용자 측정으로 한다.

## 7. 위험과 미확인

- 표시된 후보가 ear가 아니면 그 자리는 버려진다. Bunny 단면의 ear 비율은 미측정이다(논문의 98%는 우리 자산 값이 아니다).
- 해시 선택보다 선택이 규칙적이라 가는 삼각형(sliver)이 늘 수 있다. 논문도 Mark-and-cut의 각도 품질이 가장 낮았다. 렌더·물리 hull에 문제가 되는지 캡처로 확인한다.
- `RankBoundaryCS`가 loop 내 순서를 남기는지 미확인이다. 남기지 않으면 tid 0 순회 비용이 추가된다.
- 1024 초과 loop는 해시 경로가 남아 결과 성질이 두 가지가 된다. 별도 단계에서 정리한다.

## 8. S1 실행 기록 (2026-10-30, 미커밋)

- 실제 시작 HEAD는 `6f328257`이었다. 코드 변경은 `CapEarsCS.hlsl`만이다. `main`에서 leader부터 최대 `min(count,4096)`번 순회해 닫힘·슬롯 범위·leader 일치를 확인하고, 실패 시 cap을 생략한다.
- 길이 m ≤ 1024인 loop를 경계 순서로 캐시하고 `CachedSlot`으로 Ring 기록 슬롯을 복원한다. `CachedFlags`는 bit0 Active, bit1–2 Kind, bit3 Selected(ear 판정 전에는 임시 reflex)이며 위치별 소유 스레드만 쓴다. `SerialMode`에서도 캐시 후보 판정과 플래그 정리는 소유 스레드가 수행하고 최저 후보 선택은 tid 0이 한다.
- 위치 키 해시와 라운드 상한은 유지했다. `Slot`으로 동률의 원래 슬롯과 삼각형 슬롯 ID를 보존하고, 마지막 세 점도 최소 원래 슬롯에 기록한다. m > 1024는 기존 슬롯 인덱스 경로를 사용한다. scan·교대 표시와 C++·FinalizeCutCS·CutCommon 변경은 없다.
- MX450에서 256 스레드 Debug/Release 각각 Cube 2종·Bunny 4종을 1회씩, Bunny y=0은 총 3회씩 실행했다. 모두 exit 0, 완료 marker·publication 검증 통과. 아래는 각 구성의 단계 1b 로그와 비교한 양쪽 cap checksum 결과다(재절단은 두 publication 모두 비교).

| 입력 | side별 삼각형 수 | Debug / Release의 1b 대비 checksum |
| --- | --- | --- |
| Cube y=0 | 5 / 5 | 동일 / 동일 |
| Cube 45° | 2 / 2 | 동일 / 동일 |
| Bunny y=0 | 364 / 364 | 3회 모두 동일 / 3회 모두 동일 |
| Bunny y=-240 | 396 / 396 | 동일 / 동일 |
| Bunny y=-230 | 449 / 449 | 동일 / 동일 |
| Bunny 재절단 | 364 / 364 → 161 / 161 | 두 publication 모두 동일 / 동일 |

- Release 64/512 스레드도 Bunny y=0·재절단을 각 1회 실행해 256 및 1b와 checksum이 모두 같았다. 경계 순서로 바뀐 면적 합산은 이번 입력의 checksum을 바꾸지 않았다.
- DXC `cs_6_2` 출력의 groupshared 합계는 24,608 B(배열 24,576 + 스칼라 32), 32 KiB 이하이다. 256 복원 후 백업과 SHA256 `B74BDC6743E821BF9E9A3222F78D49E11CB932C94EF33D89D47A45A9EB752BFC` 일치, 최종 Debug/Release의 MeshCutting·ModelViewer 빌드 exit 0, `git diff --check` 통과. 기존 C4819 및 `pwsh.exe`를 찾지 못하는 빌드 메시지는 남아 있다.
- Release 256, `-debug 0 -model Models/bunny.glb -cut-benchmark 20 -cut-y 0 -cut-expect-publications 1` 3회의 medianGpuMs / cap 중앙값(ms)은 **3.980224 / 2.180688**, **4.454880 / 2.158352**, **5.709824 / 2.796672**다. cap은 마지막 측정 20개(warmup 제외)의 산술 중앙값이다. 1b와 교차 측정하지 않았으므로 개선으로 판정하지 않는다.
- 새 D3D12 ERROR/CORRUPTION은 없고 failedEarSegments=0이다. 다만 기존과 같은 Bunny y=-240의 uncappedSegments=207, y=-230의 49 및 badBoundaryMask=1이 남는다. y=0의 badBoundaryMask=2, 렌더 타깃 WARNING, DXGI ERROR #80도 1b 로그에 존재한다. checksum 일치는 이 문제들의 해결을 뜻하지 않는다.
- 로그·백업·DXIL·실행 스크립트는 저장소 밖 `%TEMP%/cap-s1/`에 있다. 실행 파일·PDB·모델 복사와 HDR/RAW 저장은 하지 않았다. C: 여유는 시작 2.854 GiB, 최종 빌드 후 약 1.91 GiB였고 검사 시 1.5 GiB 미만은 없었다. `Get-PSDrive C`가 Free를 반환하지 않아 .NET `DriveInfo.AvailableFreeSpace`로 보완했다.
- 통제된 m=1024 경계·1024 초과 loop·잘못된 연결·SerialMode 직접 입력, 면적 상쇄가 큰 경계 및 5060 Ti는 미검증이다. S2 이후 단계와 독립된 새 세션 리뷰는 수행하지 않았다.
