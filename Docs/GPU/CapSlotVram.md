상태: 진행 중 — SV-1 구현, dxc 256/512/1024 컴파일·정적 리뷰 통과; Release 빌드·기하 검증·측정 미실시(디스크 부족) (2026-11-10)

# cap 캐시 상한 확대 2 (1536 → 1976): CachedSlot을 VRAM으로

상위 문서: [CapCacheLimit.md](CapCacheLimit.md)(정점당 20 B 배치, `CachedLink`), [CapActiveTestList.md](CapActiveTestList.md)(`TestList`).

## 1. 목표

- `CapEarsCS`의 groupshared 캐시 상한 `CAP_CACHE`를 1536에서 **1976**으로 올린다.
- 그러면 Armadillo y=60의 루프(1953 정점)가 `Mapped` 경로 대신 `Cached` 경로를 탄다.
- 상한 아래 루프의 cap 결과(삼각형·checksum)는 바꾸지 않는다. 바꾸는 것은 슬롯 번호를 두는 위치뿐이다.

## 2. 근거 측정 (RTX 5060 Ti, Release, 512 threads, 사용자 측정, 2026-11-10)

| 사례 | 삼각형 | maxLoop | mappedLoops | cap ms | 전체 GPU ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Armadillo y=60 (`Models/armadillo.glb`) | 345,944 | 1953 | 1 | 66.31 | 68.90 |

- 같은 GPU의 Dragon y=0.08(1070, Cached)은 cap 1.37 ms다(CutPerformanceComparison.md 8절).

## 3. 결정과 이유

| 결정 | 이유 |
| --- | --- |
| `CachedSlot[CAP_CACHE]`(정점당 4 B)를 groupshared에서 뺀다 | groupshared 배열 중 가장 적게 읽힌다. 매 라운드 읽는 것은 `CachedPoints`(InTriangle), `CachedLink`(연결·플래그), `TestList`(InTriangle 순회)이고, `CachedSlot`은 귀 삼각형을 Ring +64/+76에 기록할 때와 마지막 삼각형 선택에서만 `Slot()`으로 읽는다(InTriangle 순회에는 없다) |
| Cached 루프도 Mapped와 같은 VRAM 순서 테이블(`HashLinksOffset + MaxSegments*8 + (MapBase+p)*4`)을 쓴다 | 테이블과 `Slot()`의 Mapped 분기가 이미 있다. scratch는 `HashLinksOffset + MaxSegments*12`로 테이블에 `MaxSegments`칸이 있고, 모든 루프 길이 합은 선분 수(≤ `MaxSegments`)를 넘지 않는다 |
| `MapBase` 예약(Meta `63+2*Sections`)을 `LoopLength != 0`인 모든 루프에서 한다. Mapped 루프 수 카운터(Meta `62+2*Sections`)는 지금처럼 Mapped일 때만 올린다 | 테이블 구간 충돌을 막는다. `mappedLoops` 진단 의미는 유지한다 |
| tid 0 순회 중 슬롯 번호를 임시로 `TestList[p]`에 둔다(지금 `CachedSlot[p]` 자리) | `TestList`는 라운드 전까지 비어 있다. 두 번째 직렬 순회 없이, 예약 뒤 전 스레드가 `table[MapBase+cache] = TestList[(cache+Rotation)%size]`로 병렬 기록한다 |
| Cached의 Rotation 임시 버퍼로 `CachedLink`를 쓰던 단계는 위 병렬 기록으로 대체한다 | 같은 회전 결과를 테이블에서 바로 읽어 `CachedPoints`를 채운다 |
| `Slot()`은 Cached·Mapped 모두 테이블을 읽는다. Ring +112(역방향 순위)는 지금처럼 Mapped만 쓴다 | Cached의 `Nxt`/`Prv`는 `CachedLink`라 역방향 순위가 필요 없다 |
| `CAP_CACHE`는 1976 고정 | 정점당 16 B. CAP_THREADS 1024의 `WarpTotal`(1 KiB) + 스칼라를 더해도 32 KiB 이하(약 32,700 B). 2048 미만이라 `CachedLink`의 11비트 Next/Prev를 그대로 쓴다 |
| `TestList`·`CachedPoints`·`CachedLink`는 groupshared에 그대로 둔다 | 매 라운드 접근하는 배열이다. 이번 단계는 상한 아래 성능을 바꾸지 않는 변경만 한다 |

## 4. 인터페이스 골격 (`CapEarsCS.hlsl`)

```hlsl
#define CAP_CACHE 1976
groupshared float2 CachedPoints[CAP_CACHE];
groupshared uint CachedLink[CAP_CACHE];   // CachedSlot 삭제
groupshared uint TestList[CAP_CACHE];     // 준비 단계에서는 슬롯 임시 버퍼

// Cached·Mapped 모두 VRAM 순서 테이블을 읽는다.
uint Slot(uint i)
{
    return (Cached | Mapped) != 0 ? Ring.Load(HashLinksOffset + MaxSegments * 8 + (MapBase + i) * 4) : i;
}
```

- 순회: `if (p < CAP_CACHE) TestList[p] = slot;`
- 예약: `LoopLength != 0`이면 `Meta(63+2*Sections)`에 `LoopLength`를 더해 `MapBase`를 받는다. Mapped면 `Meta(62+2*Sections)`도 1 더한다.
- Cached 준비: 배리어 뒤 `cache = tid..size`에서 `slot = TestList[(cache+Rotation)%size]`를 테이블에 쓰고, `CachedPoints[cache] = Point2(slot)`, `CachedLink[cache] = 1 | next<<5 | prev<<16`.
- 준비가 끝나면 Cached·Mapped 분기 **밖에서** 모든 스레드가 `AllMemoryBarrierWithGroupSync()`를 실행한다. 다른 스레드가 쓴 테이블을 `Slot()`이 읽기 때문이다. 길이 3 루프는 라운드 안의 배리어를 거치지 않고 마지막 삼각형을 기록하므로 이 배리어가 필요하다. `DeviceMemoryBarrier()`만으로는 스레드 간 실행 동기화가 되지 않는다.
- 이 외 함수 이름과 라운드 본문은 바꾸지 않는다.

## 5. 단계

| 단계 | 내용 | 예상 시간 |
| --- | --- | --- |
| SV-1 | 위 변경 (`CapEarsCS.hlsl` 한 파일, 약 40줄) | Codex 구현 5~10분 + 리뷰 2~5분 |
| 검증 | Release 빌드, MX450 회귀·벤치 | 빌드 약 4~5분 + 실행 약 10분 |

## 6. 완료 조건

- dxc `cs_6_2 -HV 2021`로 CAP_THREADS 256/512/1024가 컴파일되고 groupshared가 32 KiB 이하다.
- Release `-debug 0 -cut-validate 1`: Cube y=0·45°, Bunny y=0·-240·-230·재절단(`-cut 2`), Dragon y=0.08이 기하 검증을 통과하고 cap checksum이 변경 전과 같다.
- Armadillo y=60: `maxLoop=1953`, `mappedLoops=0`, 기하 검증 통과.
- Cached·Mapped 혼합과 예약 구간: `CAP_CACHE`를 임시로 1000으로 낮춘 빌드(커밋하지 않음)에서 Dragon y=0.08(1070 Mapped + 작은 Cached 루프)이 기하 검증을 통과하고 checksum이 기본 빌드와 같다.
- 길이 3 루프와 1976/1977 경계는 통제 입력이 없으면 미검증으로 기록한다.
- 성능(MX450, `-cut-benchmark 20`, 변경 전/후 교차 각 1회): Bunny y=0, Dragon y=0.08의 cap이 변경 전 대비 크게 늘지 않는다(범위 기록). Armadillo y=60 cap 전후를 기록한다. 5060 Ti는 사용자가 잰다.

## 7. 위험

- 1976은 Armadillo(1953)보다 23 크다. 더 큰 루프는 여전히 Mapped다. 다음 후보는 `TestList`의 groupshared 풀 공유(크기별 배치)다.
- `MapBase` 예약이 모든 루프로 늘어 Meta atomic이 루프당 1번 는다. 루프 수만큼이라 무시할 수준으로 본다.
- `Slot()`이 VRAM 읽기가 된다. 귀 기록(출력 주소·삼각형 값·완료 플래그)과 마지막 삼각형 선택에서 추가 로드가 생기고, 실제 로드 수는 컴파일러 최적화에 따라 다르다. 성능은 실측으로 판단한다.
