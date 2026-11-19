상태: 완료 — AD-1 `c018aea0`, AD-2 재측정으로 잠정 목표(Mode 8+5 ≤ 300 µs) 통과(2026-11-19)

# 패치 추출 인접 집계 정점 표시 결정

## 1. 목표

- `PatchExtractCS`의 Mode 5는 제거된 삼각형마다 원본 정점을 공유하는 선택 삼각형이 있는지 검색하고, 있으면 그 제거 삼각형의 patch id에 `Adjacent`를 1 더한다.
- 지금은 제거 삼각형 하나가 같은 VB의 모든 삼각형을 훑으므로 최악 O(N²)이다.
- 선택 삼각형의 정점을 먼저 비트로 표시하고, 제거 삼각형은 자기 정점 3개의 비트만 읽도록 바꿔 O(N)으로 만든다.
- `Adjacent` 결과 바이트는 바뀌지 않아야 한다. CPU 쪽 readback과 `Validate` 비교는 그대로 둔다.

## 2. 근거 (MX450, Release, 임시 GPU timestamp, `-skeletal-mask-checks 1`)

| 요청 | Meta 선택/교차/미포함 | Mode 5 | 추출 전체 |
|---|---|---:|---:|
| 부분 제거 (PF-1 전) | 135/373/373 | 12,613 µs | 38,730 µs |
| 부분 제거 (PF-1 후) | 135/373/373 | 34,513 µs | 50,693 µs |
| 전체 제거 (PF-1 후) | 0/0/0 | 132 µs | 15,479 µs |
| 0 마스크 (PF-1 후) | 934/373/373 | 150 µs | 15,993 µs |

- 부분 제거 상태에서 Mode 5가 추출 시간의 대부분이다. 두 측정값의 차이(12.6 ms와 34.5 ms)는 실행마다 GPU 상태가 다르기 때문으로 추정하며, 원인은 확인하지 않았다.
- 전체 제거나 0 마스크에서는 내부 조기 종료로 짧다.
- 마스크 검사는 진단 readback을 포함하므로 추출 전체 값은 데모와 직접 비교하지 않는다.
- 원자료: `%TEMP%\vertex-dedup-measure\result-timing.md`, `result-pf2.md`(임시, 저장소 밖).

## 3. 현재 판정 규칙 (보존 대상)

제거 삼각형 t(`State[t]` bit31)가 다음을 만족하면 `Adjacent[State[t]&65535]`를 한 번 증가시킨다.
- `Meta[0]`(선택 수)이 0이 아니다.
- 같은 `VbOffset`과 `Stride`를 가진 draw 안에, prefix가 증가한(선택된) 삼각형 j가 있다.
- t와 j가 `Index()` 값(BaseVertex 적용 후 정점 번호)을 하나 이상 공유한다.

위치만 같은 seam 분할 정점은 지금도 인접으로 보지 않는다. 이 규칙은 그대로 둔다.

## 4. 결정과 이유

### D1. 2단계로 나눈다: 표시(Mode 8, 신규)와 조회(Mode 5, 교체)
- Mode 8: 선택된 삼각형마다 자기 정점 3개의 비트를 `InterlockedOr`로 켠다.
- Mode 5: 제거 삼각형이 자기 정점 3개의 비트를 읽고, 하나라도 켜져 있으면 지금처럼 `Adjacent`를 1 더한다.
- 이유: 두 패스 모두 삼각형당 상수 작업이라 O(N)이고, 판정 규칙이 3절과 그대로 대응한다.

### D2. 정점 비트 번호는 VB별 시작 번호 + `Index()` 값으로 정한다
- CPU(`SkeletalPatchJob::Submit`)가 `(VbOffset, Stride)` 쌍마다 연속된 정점 번호 구간을 배정하고, 그 시작 번호를 `DrawInput`의 `Reserved1` 자리에 `VertexBase`라는 이름으로 넣는다.
- 같은 쌍을 쓰는 draw는 같은 `VertexBase`를 받는다. 구간 길이는 그 쌍에 속한 draw들의 `VertexCount` 중 최댓값이다.
- 구간 합은 CPU에서 64비트로 더한다. `UINT32_MAX`를 넘으면 기존 제출 상한 검사처럼 Submit을 거절한다.
- 비트 번호 = `VertexBase + Index(s, ...)`. 같은 쌍과 같은 정점 번호일 때만 같은 비트가 된다. 따라서 3절의 "같은 VbOffset·Stride 안에서 번호 공유" 조건과 정확히 같다.
- 전제: `Index()` 값은 그 쌍의 구간 길이보다 작다. 이 전제는 새로 생기는 것이 아니다. Mode 0·2도 `Source`를 `VbOffset + i*Stride`에서 읽으므로 이미 같은 전제에 기대고 있다.
- 방어: Mode 8과 Mode 5는 범위 밖 인덱스면 비트를 쓰거나 읽지 않는다. 다른 VB 구간의 비트를 오염시키거나 할당 밖에 접근하지 않게 하려는 것이다. 이 경우는 잘못된 입력이며, 기존 판정과 같다고 주장하지 않는다.
- 이유: 기존 구조체의 빈 칸을 쓰므로 구조체 크기와 stride가 바뀌지 않는다. 바이트 주소를 키로 쓰는 안은 다른 VB 구간이 겹칠 때 판정이 달라질 수 있어 택하지 않았다.

### D3. 표시 버퍼는 새 UAV로 둔다
- 새 버퍼 `m_VertexMarks`를 만든다. 크기는 `ceil(전체 정점 수 / 32) * 4`바이트다.
- `InitializePatch`의 root signature에 UAV 하나(`u7`)를 추가한다. 이 셰이더는 root descriptor 방식이라 한두 줄로 끝난다.
- PF-1처럼 `m_Scan` 뒤에 붙이지 않는다. prefix 버퍼에 성격이 다른 정점 표시를 섞으면 읽기 어렵기 때문이다.
- `m_Adjacent`처럼 Submit마다 0으로 채워 만든다. `HasState`일 때는 전체 크기로 만든다. 아닐 때는 슬롯만 채우도록 4바이트짜리 작은 UAV를 만든다. `m_EmptyState`는 SRV라 이 용도로 쓸 수 없다.

### D4. 실행 순서와 동기화
- `HasState`일 때 순서는 Mode 0→1→6→7→2→8→5다.
- Mode 8은 최종 prefix(Mode 7 이후)를 읽는다. Mode 8과 Mode 5 사이에는 `m_VertexMarks` UAV barrier를 넣는다.
- Mode 8과 Mode 5 모두 기존과 같은 조기 종료 조건(`Meta.Load(0)==0`)을 지킨다. 기존 `LocalBounds` barrier가 Mode 6의 `Meta` 쓰기 이후 순서를 보장하는 구조는 그대로 둔다.
- dispatch 크기는 기존 삼각형 dispatch(2D 분할)와 같고, `t>=Triangles`이면 쓰지 않는다.

## 5. 인터페이스 골격

```hlsl
struct DrawInput { ... uint DrawId, JointCount, VertexCount, VertexBase; };  // Reserved1 → VertexBase
RWByteAddressBuffer VertexMarks : register(u7);

// Mode 8: 선택 삼각형의 세 정점 비트를 켠다.
// Mode 5: 제거 삼각형의 세 정점 비트 중 하나라도 켜져 있으면 소유 패치의 인접 수를 1 더한다.
```

```cpp
// SkeletalPatchJob::Submit: (VbOffset, Stride)별 VertexBase 배정, 전체 정점 수 합산,
// HasState일 때 m_VertexMarks 0 초기화 생성·바인딩, Mode 2 → 8 → (barrier) → 5.
```

## 6. 범위 밖

- Mode 0 정점 중복 판정: 데모 기준 86 µs로, 이번에도 하지 않는다.
- seam 정점 용접(위치 기준 인접): 현재 규칙(3절)을 바꾸는 일이라 하지 않는다.
- 몸체 압축 경로의 `Dispatch(1)` 단계: 측정 후 따로 판단한다.

## 7. 단계와 예상 시간

| 단계 | 내용 | 예상 시간 |
|---|---|---|
| AD-1 | `DrawInput.VertexBase`, 구간 배정·64비트 합 검사, Mode 8 추가·Mode 5 교체와 범위 방어, `m_VertexMarks`·root signature, `Submit` 순서 (약 90~140줄, 추정, 300줄 이내) | Codex 구현 15~25분 |
| AD-1 검증 | Release(증분 15~20초 실측) → 검사 실행 → Debug 빌드 → 검사 실행 | 10~20분(Debug 소요는 추정) |
| AD-1 리뷰 | 구현과 다른 새 Codex 실행, 기준은 8절 | 2~5분 |
| AD-0 | AD-1 전 기준 측정: 현재 HEAD에 임시 timestamp를 넣고 `-skeletal-mask-checks 1` 3회 실행, 계측 원복(커밋하지 않음) | 약 15분 |
| AD-2 | AD-1 후 같은 방식으로 3회 재측정(커밋하지 않음), 문서 상태 줄·README·CodeMap 동기화 | 약 20분 |

- 빌드는 한 번에 하나씩, 기존 `MiniEngine/Build`에서 한다.
- `Geometry/`를 바꾸지 않으므로 `PlaneSlicerTests`는 필요 없다.

## 8. 완료 조건

1. Debug·Release 빌드 오류 0.
2. `-skeletal-mask-checks 1` PASS. 특히 `adjacentPatch GPU==CPU`가 PASS여야 한다. `Validate`는 `m_AdjacentReadback` 전체(262,144바이트)를 독립 CPU 집계와 `memcmp`로 비교한다. 부분 제거 요청은 지금처럼 `removedNeighbors=64`(0이 아님)여야 한다. 5개 요청의 `Meta`는 934/373/373, 135/373/373, 0/0/0, 934/373/373, 934/373/373 그대로다.
3. `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1`의 20개 요청 GPU==CPU PASS.
4. `-skeletal-union-checks 1 -skeletal-break-demo 2` PASS(364/352→526 등 기존 수치).
5. `-skeletal-break-demo 1`·`2` 게시 PASS, `Meta` 2118/47/0, 271/16/0.
6. D3D12 debug layer ERROR/CORRUPTION 0.
7. 재측정: PF-2와 같은 임시 timestamp 방식(모드별 timestamp 전 barrier flush, barrier는 구간 밖)을 쓴다. AD-1 적용 **전후** 각각 `-skeletal-mask-checks 1`을 3회 실행하고, 회차마다 첫 요청을 포함한 그대로 기록한다.
   - 부분 제거 요청(135/373/373)에서 Mode 8+5 합의 평균이 300 µs 이하여야 한다. 이 값은 측정 근거가 없는 **잠정 목표**다. 넘더라도 변경 전 Mode 5 대비 감소 폭을 기록하고 판정한다.
   - `m_VertexMarks` 생성·초기화 비용은 CPU 쪽이라 GPU 구간에 잡히지 않는다. 버퍼 크기는 따로 기록한다.
   - 데모(`-skeletal-break-demo 1`)의 Mode 0·1·6·7·2는 PF-2 표와 비교만 하고 판정 기준으로 쓰지 않는다(데모는 `HasState`가 아니라 바뀌는 경로가 없다).
8. Mode 0·1·2·6·7, `Adjacent` 배치, CPU readback·`Validate` 코드는 바뀌지 않는다. `DrawInput` 크기(64바이트)도 그대로다.
9. 경로 범위 보고: 구현 중 Xbot·CesiumMan의 draw 수, 서로 다른 `(VbOffset, Stride)` 쌍의 수, 한 쌍을 여러 draw가 공유하는지를 로그로 보고한다.
   - 한 쌍을 여러 draw가 공유하는 경우와 다른 쌍에 같은 인덱스가 있는 경우(구간 분리)가 현재 검사 모델에 있으면, 2항의 GPU==CPU 비교가 곧 그 검증이다.
   - 없으면 합성 검사 코드를 새로 만들지 않는다. "코드 리뷰로만 확인"이라고 기록한다. 이 판단의 근거는 두 가지다. 비트 번호 규칙이 3절 조건과 한 줄로 대응하고, 합성 geometry 검사는 이번 단계 크기를 넘는다.
   - "여러 선택 이웃에 닿아도 한 번만 증가"는 CPU 참조 집계도 같은 규칙이므로 2항 비교로 검증된다.

## 9. 결과 (AD-1 `c018aea0`, AD-2 재측정, 2026-11-19)

- 구현: `DrawInput.Reserved1`을 `VertexBase`로, `Parameters.Padding1`을 `VertexMarkWords`로 바꿨다(구조체 크기 불변). `(VbOffset, Stride)` 쌍마다 구간을 배정하고, 같은 쌍을 쓰는 draw의 `VertexCount`는 쌍 내 최댓값으로 덮어쓴다. 새 UAV `m_VertexMarks`는 `u7`에 바인딩된다.
- 재측정: MX450, Release, `-debug 0 -skeletal-mask-checks 1` 3회(첫 요청 포함, 보정 없음). 아래는 부분 제거(135/373/373) 요청의 산술평균이다. 원자료는 `result-ad0.md`, `result-ad2.md`(임시, 저장소 밖).

| 항목 | AD-0 (AD-1 전) | AD-2 (AD-1 후) | 감소 |
|---|---:|---:|---:|
| Mode 5 | 34,351.691 µs | 109.536 µs | 99.68% |
| Mode 8 | — | 166.795 µs | — |
| Mode 8+5 합 | — | 276.331 µs | 99.20% (AD-0 Mode 5 대비) |
| 추출 전체 | 50,336.427 µs | 16,031.829 µs | 68.15% |

- 판정: Mode 8+5 평균 276.331 µs로 잠정 목표 300 µs 이하를 통과했다. 회차별 합은 265.568 / 282.176 / 281.248 µs다.
- `removedNeighbors=64`(부분 제거), 0(전체 제거·0 마스크), `adjacentPatch GPU==CPU` PASS 3회, 5개 요청 `Meta` 순서 동일.
- `m_VertexMarks` 크기: `HasState`일 때 3,548바이트, 아닐 때 4바이트(CPU 쪽 생성 비용은 GPU 구간에 잡히지 않는다).
- 데모(`-skeletal-break-demo 1`, 1회): Mode 0/1/6/7/2/전체 = 84.672/18.432/9.472/10.144/33.184/340.032 µs로 PF-2 xbot 평균과 최대 1.312 µs(Mode 7) 차이다(비교만).
- 확인하지 못한 항목: 이 재측정에서는 D3D12 debug layer, Influence Mode 3/4, 다른 GPU, 비계측 GPU 시간을 보지 않았다(Debug·검사 전수는 AD-1 검증에서 PASS).
