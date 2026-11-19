상태: 완료 — PF-1 구현·검증(2aaef989), PF-2 재측정(2026-11-19)에서 7절 7항 기준 통과(Xbot Mode 1+6+7 평균 36.8 µs ≤ 200 µs); 7절 9항의 T=1·63·64·65·B<64·65535그룹 경계는 코드 리뷰로만 확인

# 패치 추출 prefix 병렬화 결정

## 1. 목표

- `PatchExtractCS`의 Mode 1은 thread 0 하나가 모든 삼각형을 순서대로 돌며 선택 수 누적값을 `Scan`에 쓴다.
- 이 순차 루프를 병렬 scan으로 바꿔 절단 요청당 추출 시간을 줄인다.
- 결과 바이트는 바뀌지 않아야 한다. `Scan`의 삼각형별 inclusive prefix와 `Meta`의 선택·교차·미포함 교차 수가 지금과 같아야 한다.
- Mode 0·2·3·4·5와 CPU 쪽 readback·검증 코드는 그대로 둔다.

## 2. 근거 (측정, MX450, Release, 임시 GPU timestamp)

| 대상 | Mode 0 | Mode 1 | Mode 2 | 전체 |
|---|---:|---:|---:|---:|
| Xbot 데모 T=49112 (3회 평균) | 85 µs | 4411 µs | 34 µs | 4713 µs |
| CesiumMan 데모 T=4672 (3회 평균) | 22 µs | 246 µs | 11 µs | 299 µs |

- Mode 1이 Xbot 추출 시간의 94%를 차지한다. 검사 실행에서는 같은 T에서 13~27 ms까지 나왔다.
- 정점 중복 판정이 있는 Mode 0은 2% 안팎이라 이번 범위에서 제외한다.
- Mode 5(제거 상태 인접 집계, 부분 제거 12.6 ms)는 별도 결정으로 미룬다.
- 측정 원자료: `%TEMP%\vertex-dedup-measure\result-timing.md`(임시, 저장소 밖).

### PF-1 이후 재측정 (MX450, Release, 같은 임시 timestamp 방식, 데모당 3회 평균, 단위 µs)

| 대상 | Mode 0 | Mode 1 | Mode 6 | Mode 7 | Mode 2 | Mode 1+6+7 | 전체 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Xbot 데모 T=49112 | 85.7 | 19.3 | 8.7 | 8.8 | 33.7 | 36.8 | 341.1 |
| CesiumMan 데모 T=4672 | 22.6 | 6.2 | 6.1 | 5.5 | 12.0 | 17.8 | 71.4 |

- Xbot Mode 1 4411 → 19.3 µs, 전체 4713 → 341 µs. CesiumMan 전체 299 → 71 µs. Meta는 기존과 같다(2118/47/0, 271/16/0).
- 회차별 Mode 1+6+7 합은 Xbot 36.6·34.8·39.1 µs로 모두 기준 200 µs 이하다.
- 전체 구간에는 모드 시간 외에 barrier·상태 전환·복사가 포함된다. 그 내역은 따로 측정하지 않았다.
- `-skeletal-mask-checks 1` 실행에서도 PASS했고 Meta는 934/373/373, 135/373/373, 0/0/0, 934/373/373, 934/373/373이다. 진단 readback이 섞여 있어 이 실행의 시간은 평균에 넣지 않았다.
- 어댑터는 NVIDIA GeForce MX450이고 `Influence`·`HasState` 경로(Mode 3/4/5)는 이번 데모에서 쓰이지 않아 재측정하지 않았다. 원자료: `%TEMP%\vertex-dedup-measure\result-pf2.md`(임시).

## 3. 결정과 이유

### D1. 3단계 block scan을 쓴다
- Mode 1: 64개 삼각형 블록마다 그룹 안에서 scan하고, 블록 합계를 기록한다.
- Mode 6(신규): 한 그룹이 블록 합계를 exclusive scan하고 `Meta`에 총계를 쓴다.
- Mode 7(신규): 각 블록의 삼각형 prefix에 앞 블록들의 합을 더한다.
- 이유: 단일 패스 decoupled look-back은 그룹 실행 순서 보장이 없는 GPU에서 멈출 수 있고 코드가 길다. 3단계는 dispatch 두 번이 늘지만 각 단계가 단순하고 결과가 결정적이다.

### D2. 그룹 안 scan은 groupshared로 하고 Wave 함수는 쓰지 않는다
- 셰이더는 SM 6.0이라 `WavePrefixSum`을 쓸 수 있다.
- 그러나 wave 크기가 Intel(8~32)·NVIDIA(32)로 달라 64개 블록을 여러 wave로 나누는 분기가 생긴다.
- `numthreads(64)`와 groupshared Hillis-Steele scan(6단계)은 wave 크기와 무관하게 같은 코드로 동작한다.

### D3. 블록 크기는 기존 `numthreads(64,1,1)`을 유지한다
- 셰이더 하나와 root signature를 그대로 쓰기 위해서다.
- T=49112이면 블록 768개다. Mode 6의 64개 thread가 각자 12개 블록을 순차 합산한 뒤 그룹 scan한다.
- Mode 6은 lane마다 **연속 블록 구간** `[lane*K, min((lane+1)*K, B))`(K=ceil(B/64))을 맡는다. 순서는 다음과 같다.
  1. 각 lane이 자기 구간의 선택 합계를 더한다.
  2. lane 합계를 그룹 exclusive scan해 구간 시작 offset을 얻는다.
  3. 각 lane이 구간을 앞에서부터 돌며 누적값을 블록 offset으로 덮어쓴다.
- lane에 블록을 건너뛰며(stride) 배분하면 전역 순서와 맞지 않으므로 쓰지 않는다.
- 제출 상한 T≈2980만(기존 `<=UINT32_MAX/(3*48)`)에서는 블록 약 46.6만 개로, thread당 순차 7300개가 된다. 현재 모델 규모에서는 문제없고, 상한 근처 성능은 보장하지 않는다.

### D4. 블록 합계는 `m_Scan` 뒤쪽에 붙인다
- 새 버퍼와 새 UAV 슬롯을 만들지 않는다. 그러면 `InitializePatch`의 root signature(13개 파라미터)를 바꾸지 않아도 된다.
- `m_Scan`을 8바이트 원소 `T + 2*B`개로 만든다(B=ceil(T/64)).
- 블록 b의 영역은 바이트 `T*8 + b*16`부터 16바이트다. 선택 합계, 교차 합계, 미포함 교차 합계를 쓰고 4바이트는 비운다.
- 삼각형 영역 `[0, T*8)`의 배치는 지금과 같다. Mode 2·5는 계속 `Scan.Load(t*8)`로 최종 inclusive prefix를 읽는다.

### D5. 교차·미포함 교차 총계도 블록 합계로 구한다
- atomic 없이 Mode 6이 블록 합계를 더해 `Meta.Store3(0, uint3(sum, crossing, uncovered))`를 한 번 쓴다.
- 그래서 `Meta` 0~11바이트를 미리 0으로 지울 필요가 없고, 결과 순서도 결정적이다.

## 4. 인터페이스 골격

```hlsl
groupshared uint Prefix[64];

// Mode 1: 블록 안 inclusive prefix와 블록 합계(선택·교차·미포함)를 기록한다.
// Mode 6: 블록 선택 합계를 exclusive offset으로 바꾸고 Meta 총계를 쓴다(Dispatch(1)).
// Mode 7: 블록 offset을 삼각형 prefix에 더한다(블록 0은 건너뛴다).
```

```cpp
// SkeletalPatchJob::Submit: Mode 0 → 1 → 6 → 7 → 2, 각 단계 사이 m_Scan UAV barrier.
// Mode 1·7은 기존 2D 분할 Dispatch(min(B,65535), ceil(B/65535)), Mode 6은 Dispatch(1).
m_Scan.Create(L"Patch inclusive prefix", uint32_t(triangles + 2 * blocks), 8);
```

- 그룹 번호는 기존 `t=id.x+id.y*(65535*64)`에서 `b=t/64`로 얻는다.
- 2D 분할에서는 B보다 많은 그룹이 실행될 수 있다(B=65536이면 65535×2개). 그래서 쓰기를 따로 막는다. Mode 1의 블록 영역 쓰기는 `b<B`일 때만, Mode 1·7의 삼각형 쓰기는 `t<T`일 때만 한다. 범위 밖 입력을 0으로 두는 것만으로는 부족하다.
- Mode 1·6·7은 범위 밖 thread도 groupshared 동기화에 참여해야 하므로 `t>=Triangles` 조기 return보다 앞에서 처리하고, 범위 밖은 0을 넣는다.
- `Mode`는 상수 버퍼 값이라 모드 분기는 그룹 전체가 같은 쪽으로 간다. 다른 모드의 조기 return은 문제가 아니다.
- scan 단계마다 "이전 값 읽기 → barrier → 덮어쓰기 → barrier" 순서로 동기화한다. `GroupMemoryBarrierWithGroupSync`는 `lane>=offset` 같은 lane별 조건 **밖**에서 모든 thread가 호출한다(분기 안 호출은 미정의 동작).
- `Meta` 의존성: Mode 5의 `Meta.Load(0)`은 Mode 6이 쓴 총계를 읽는다. 지금 Mode 2 뒤에 있는 `m_Output->LocalBounds` UAV barrier가 이 순서를 보장하므로 그대로 둔다(`InsertUAVBarrier`는 지정한 리소스에만 걸린다). `Meta` 12바이트 이후의 bounds 초기화도 유지한다.
- Mode 1의 기존 위치(`if(t!=0) return;` 블록)를 교체한다. `Mode==1`을 다른 곳에서 특별 취급하는 C++ 코드(`c.Mode == 1 ? 1 : ...`)는 새 dispatch 크기로 바꾼다.

## 5. 범위 밖

- Mode 0 정점 중복 판정 개선: 측정상 이득이 작아 하지 않는다.
- Mode 5 O(N²) 인접 집계: 별도 결정 문서로 다룬다.
- `SkeletalPatchCopy.cpp`의 몸체 압축 경로(`Body compact` 셰이더의 Mode 1 `Dispatch(1)`)도 순차일 가능성이 있다. 측정 후 따로 판단한다.
- Influence 투표 Mode 4(`Dispatch(1)`, Xbot 40 µs)는 그대로 둔다.

## 6. 단계와 예상 시간

| 단계 | 내용 | 예상 시간 |
|---|---|---|
| PF-1 | 셰이더 Mode 1 교체·Mode 6/7 추가, `m_Scan` 크기, `Submit` dispatch 순서 (구현만 약 100~150줄, 추정) | Codex 구현 15~25분 |
| PF-1 검증 | Release 빌드(증분 18초·전체 2.6분 실측) → 검사 실행 → Debug 빌드 → 검사 실행 | 10~20분(Debug 소요는 미실측, 추정) |
| PF-1 리뷰 | 구현과 다른 새 Codex 실행, 기준은 7절 | 2~5분 |
| PF-2 | 임시 timestamp로 재측정(커밋하지 않음), 이 문서 상태 줄·README·CodeMap 동기화 | 약 15분 |

- 디스크가 부족하므로 빌드는 한 번에 하나씩 하고, 산출물은 기존 `MiniEngine/Build`를 쓴다.
- `Geometry/`를 바꾸지 않으므로 `PlaneSlicerTests`는 필요 없다.

## 7. 완료 조건

1. Debug·Release 빌드 오류 0.
2. `-skeletal-mask-checks 1` PASS. 기존 5개 요청(기준·부분 제거·전체 제거·0 마스크·해제)의 `Meta` 선택/교차/미포함 수가 측정 표와 같다(934/373/373, 135/373/373, 0/0/0, 934/373/373, 934/373/373).
3. `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1`의 20개 요청이 `SkeletalPatchJob::Validate`의 GPU==CPU를 통과한다(출력 순서와 출처가 prefix에 의존하므로 prefix 오류를 잡는다).
4. `-skeletal-break-demo 1`(Xbot P60)과 `2`(CesiumMan)가 기존처럼 게시되고, `Meta`가 2118/47/0, 271/16/0이다.
5. `-skeletal-union-checks 1 -skeletal-break-demo 2`(단독으로는 실행되지 않음)의 `CheckJointUnion`이 PASS한다. 단독·합집합·중복 P 추출(364/352→526, 공유190, 중복0), n=1 unionRoots 출처 목록·순서 비교가 기존과 같아야 한다.
6. D3D12 debug layer ERROR/CORRUPTION 0.
7. 재측정: 기존과 같은 임시 timestamp 방식(모드별 timestamp 전에 barrier를 flush하고, barrier는 구간 밖에 둠)으로 `-skeletal-break-demo 1`을 3회 실행한다. 회차마다 Mode 1+6+7 합을 구하고, 그 평균이 200 µs 이하여야 한다(현재 Mode 1만 4411 µs).
8. Mode 0·2·5, `m_Scan`의 삼각형 영역 배치, root signature, CPU readback 코드는 바뀌지 않는다.
9. 경계 조건:
   - 실행으로 확인하는 경우: B%64≠0(CesiumMan B=73), B가 64의 배수(Xbot B=768), 선택 없음(마스크 검사의 전체 제거, 0/0/0).
   - 코드 리뷰로만 확인하는 경우: T=1·63·64·65, B<64, 65535그룹 경계(T가 약 419만 이상이어야 발생)는 현재 모델로 재현할 수 없다. 리뷰에서 4절의 쓰기 제한(`b<B`, `t<T`)과 Mode 6 구간 배분을 확인하고, 실행으로는 검증하지 않았다고 기록한다.
   - 전체 선택은 현재 진단 경로가 없어 확인하지 않은 항목으로 남긴다.
