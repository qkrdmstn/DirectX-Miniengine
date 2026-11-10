상태: 진행 중 — CL-1 `983a8c9d`, Release 검증 완료, 5060 Ti Dragon y=0.08 cap 18.48→1.89 ms·전체 20.98→4.06 ms; Debug 빌드 미실시 (2026-11-10)

# cap 캐시 상한 확대 (1024 → 1536)

상위 문서: [CapActiveTestList.md](CapActiveTestList.md)(현재 Cached 경로와 `TestList`), [CapScanMarking.md](CapScanMarking.md)(선택 계약).

## 1. 목표

- `CapEarsCS`에서 groupshared 캐시로 처리하는 루프 상한을 1024에서 **1536**으로 올린다.
- 그러면 Dragon y=0.08(1070)과 y=0.03(1053)의 큰 루프가 느린 `Mapped` 경로 대신 `Cached` 경로를 탄다.
- 1024 이하 루프의 cap 결과는 바꾸지 않는다. 바꾸는 것은 저장 방식뿐이다.

## 2. 근거 측정 (Release, `-cut-benchmark 20`)

| 사례 | GPU | cap ms | 최대 루프 | Mapped 루프 |
| --- | --- | ---: | ---: | ---: |
| Dragon y=0.08 | RTX 5060 Ti | 18.48 | 1070 | 1 |
| Dragon y=0 | RTX 5060 Ti | 0.57 | 459 | 0 |
| Dragon y=0.08 | MX450 | 21.70 | 1070 | 1 |
| Dragon y=0.08, Mapped 그룹만 임시 생략 | MX450 | 0.58 | 1070 | 1 |

- Mapped 루프 하나가 5060 Ti 전체 GPU 20.98 ms 가운데 약 17.9 ms를 차지한다.
- MX450에서 5060 Ti로 바꿔도 1.17배만 빨라진다. 그룹 하나가 SM 하나에서 의존적인 전역 읽기를 반복하기 때문으로 본다.

## 3. 결정

### 3.1 Next·Prev·Flags를 `uint` 하나로 묶는다

현재 groupshared는 정점당 28 B(좌표 8, Next/Prev/Flags/Slot 각 4, TestList 4)이고, 1024칸 기준 약 28.3 KiB다. 그룹당 한도는 32 KiB다.

`CachedFlags`를 `CachedLink`로 바꾸고 비트를 아래처럼 배치한다.

| 비트 | 내용 |
| --- | --- |
| 0 | active (기존과 같음) |
| 1–2 | kind (기존과 같음) |
| 3 | selected (기존과 같음) |
| 4 | reflex (기존과 같음) |
| 5–15 | Next 위치 (11비트) |
| 16–26 | Prev 위치 (11비트) |

- 플래그 비트 위치를 그대로 두므로 `Active`·`Kind`·`Selected`·`Reflex`와 플래그 갱신 식은 바뀌지 않는다.
- `CachedNext`·`CachedPrev` 배열은 없앤다. `Nxt`·`Prv`는 `CachedLink`에서 비트를 꺼내 읽는다.
- 정점당 20 B가 된다. 1536칸이면 30 KiB이고, `WarpTotal`(CAP_THREADS 512일 때 512 B)과 스칼라를 더해도 32 KiB 안에 든다.
- 11비트로 담을 수 있는 위치는 2047까지다. `CAP_CACHE`가 2048을 넘으면 `#error`로 막는다.

### 3.2 연결 갱신은 groupshared 원자 연산으로 한다

제거 단계에서는 스레드 v가 이웃 a의 Next와 이웃 c의 Prev를 고친다. 한 word에 여러 필드가 있으므로, 다른 스레드가 같은 word의 다른 필드를 동시에 고칠 수 있다(a의 Next와 a의 Prev가 서로 다른 선택 정점에서 바뀌는 경우).

- 그래서 필드를 바꿀 때는 `InterlockedAnd`로 그 필드를 지우고 `InterlockedOr`로 새 값을 넣는다. 한 필드의 작성자는 하나뿐이라 두 연산 사이에 끼어드는 작성자는 없다.
- 자기 active 비트를 내리는 것도 `InterlockedAnd`로 한다.
- XOR 한 번으로 바꾸는 방법도 있지만, "a의 Next는 지금 v"라는 불변식에 기대야 해서 쓰지 않는다.
- 다른 단계(후보 판정·reflex·선택 해제)는 지금처럼 각 위치의 소유 스레드만 플래그를 쓴다. 그 단계에서는 Next/Prev를 쓰지 않으므로 일반 대입을 유지한다.

### 3.3 상한을 상수 하나로 모은다

- `#define CAP_CACHE 1536`을 두고, 배열 크기와 `Cached`/`Mapped` 판정, 순회 중 `CachedSlot[p]` 기록 조건(`p < CAP_CACHE`)이 모두 이 상수를 쓴다.
- 회전 임시 저장은 지금 `CachedNext`에 슬롯 번호(32비트)를 잠깐 넣는다. 이제는 `CachedLink`(uint)에 넣고, 초기화 단계에서 Slot으로 옮긴 뒤 Link 값을 채운다.
- `TestList`는 uint 그대로 둔다. 16비트로 줄이면 경계 칸에서 두 스레드가 한 word를 쓰는 경쟁이 생긴다.

### 3.4 하지 않는 것

- `SelectMapped`와 Mapped 경로는 바꾸지 않는다. 1536을 넘는 루프는 지금처럼 Mapped로 간다.
- 좌표를 half로 줄이지 않는다(ear 판정 Epsilon 비교가 흔들린다).
- Mapped 경로에 TestList를 넣는 개선은 후속 후보로 남긴다.

## 4. 인터페이스 골격

```hlsl
#define CAP_CACHE 1536
groupshared float2 CachedPoints[CAP_CACHE];
groupshared uint CachedLink[CAP_CACHE], CachedSlot[CAP_CACHE];
groupshared uint TestList[CAP_CACHE];

// Link 비트 필드를 읽는다.
uint LinkNext(uint word) { return (word >> 5) & 0x7FF; }
uint LinkPrev(uint word) { return (word >> 16) & 0x7FF; }
```

CPU 쪽 변경은 없다. Meta 진단(`capMaxLoop`, `capMappedLoops`) 위치도 그대로다.

## 5. 단계

| 단계 | 내용 | 예상 시간 |
| --- | --- | --- |
| CL-1 | 위 3.1~3.3 구현 (`CapEarsCS.hlsl` 한 파일, 약 60줄) | Codex 구현 5~10분 |
| 검증 | Debug/Release 빌드, 회귀 실행, MX450 벤치 | Debug 빌드 약 30초, Release 약 4~5분, 실행 약 10분 |
| 리뷰 | 구현과 다른 새 Codex 실행, 7절 기준 | 2~5분 |

## 6. 위험

- Link 필드 갱신의 원자 연산 누락이나 시프트 실수는 루프 연결을 조용히 망가뜨린다. checksum 비교로 잡는다.
- groupshared 32 KiB 초과는 셰이더 컴파일 오류로 드러난다. CAP_THREADS 512 변형도 컴파일해 확인한다.
- 1025~1536 루프는 Mapped에서 Cached로 경로가 바뀐다. 그래서 checksum이 이전과 다를 수 있다. 기하 검증 통과로 판정한다.
- 1536을 넘는 루프가 생기는 입력이 없으면 Mapped 경로 회귀는 실행으로 확인하지 못한다. 그때는 "미검증"으로 적는다.

## 7. 완료 조건 (CL-1)

- Debug/Release 빌드 exit 0, `git diff --check` 통과, 새 D3D12 ERROR/CORRUPTION 없음.
- CAP_THREADS 64/256/512 Release 셰이더가 모두 컴파일된다(groupshared 32 KiB 이하).
- 회귀 사례: Cube y=0·45°, Bunny y=0·-240·-230·재절단(`-cut 2`), `-cut-validate 1`. 기존 기하 검증을 통과한다.
- 위 사례의 `Static cut cap checksum`, segments/loops/uncappedSegments, rounds/marked/removed가 변경 전 같은 구성과 같다. 1024 이하 루프는 저장 방식만 바뀌었으므로 같아야 한다.
- Dragon y=0.08·y=0.03 Release `-cut-validate 1`이 기하 검증을 통과하고, `capMappedLoops=0`이다.
- 성능: MX450 Release `Tools/RunCapBenchmark.ps1`(기본 4사례)을 변경 전후로 실행해 cap 중앙값을 기록한다. 5060 Ti 측정은 사용자가 한다.
- 리뷰는 구현과 다른 새 Codex 실행이 이 절을 기준으로 한다.

## 8. CL-1 실행 기록 (2026-11-10)

- 변경: `CapEarsCS.hlsl` 한 파일. groupshared 총량은 CAP_THREADS 64/256/512에서 30,844 / 31,036 / 31,292 B다.
- Release 빌드 exit 0. dxc `cs_6_2 -HV 2021`로 CAP_THREADS 64/256/512가 컴파일된다. `CAP_CACHE=1700`은 groupshared 34,316 B > 32,768 B 오류로 막힌다.
- 회귀(Release `-debug 0 -cut-validate 1`, 엔진 로그의 선택 GPU는 Intel Iris Xe): Cube y=0·45°, Bunny y=0·-240·-230·재절단(`-cut 2`), Dragon y=0.08·y=0.03.
  - 변경 전후 모든 사례의 cap checksum, segments/loops/uncappedSegments, rounds/marked/removed, 기하 검증 통과 줄이 같다.
  - 다른 점은 Dragon 두 사례의 `capMappedLoops`가 1→0이 된 것뿐이다. 1025~1536 루프도 Mapped와 같은 cap을 만든다.
- 성능(MX450 / 591.86, `Tools/RunCapBenchmark.ps1 -Runs 3`, cap 중앙값 ms):

| 사례 | 변경 전 | 변경 후 |
| --- | ---: | ---: |
| Dragon y=0.08 (1070) | 21.70~21.72 | **3.53~3.57** |
| Dragon y=0 | 0.83~0.85 | 0.84~1.14 |
| Dragon y=0.04 | 0.59~0.61 | 0.61~0.84 |
| Bunny y=0 | 0.71~0.86 | 0.74~0.76 |

  - Dragon y=0.08 전체 GPU 중앙값은 약 42 ms에서 약 26 ms로 줄었다.
  - 1070 루프의 Cached cap(3.5 ms)은 459 루프(0.8 ms)보다 크다. 라운드마다 TestList 전체를 훑는 비용이 길이에 따라 커지는 것으로 본다.
  - Mapped가 없는 사례는 실행 간 편차 안에 있다. 이 사례들의 성능 차이는 주장하지 않는다.
- 5060 Ti(사용자 측정, `983a8c9d` Release) Dragon y=0.08: cap 18.48→1.89 ms, 전체 GPU 20.98→4.06 ms, mappedLoops 1→0. 전후 표는 [CutPerformanceComparison.md](CutPerformanceComparison.md) 7절.
- 미실시: Debug 빌드(C: 여유 약 1 GB), 1536 초과 루프의 Mapped 경로 실행(해당 입력 없음).
- 로그는 저장소 밖 `%TEMP%/cap-cl1/`에 있다.
