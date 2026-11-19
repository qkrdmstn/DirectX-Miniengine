상태: 진행 중 — RM-1 커밋(RM-2 대기). 패치 추출의 진단 전용 계산(Mode 5/8 인접 집계, 기본 경로 평면 교차 수) 제거(base `78c4bfe9`, 2026-11-19)

# 패치 추출 진단 계산 제거 결정

## 1. 목표

`PatchExtractCS`와 `SkeletalPatchJob`에서 결과에 영향을 주지 않는 진단 전용 계산 두 가지를 걷어낸다.
- **(B) 인접 집계:** Mode 5와, AD-1에서 이를 O(N)으로 만든 Mode 8 정점 표시를 없앤다.
- **(A) 평면 교차 수:** 기본 경로(Rig/Parent)에서 삼각형마다 하던 평면 교차 판정과, 교차 수·미포함 교차 수 집계를 없앤다.

패치 선택·복사·출처 순서·게시 여부, 그리고 출력 데이터(VB, depth VB, IB, 출처, AABB·world X bounds)는 바뀌지 않아야 한다. 예외는 RM-2 뒤 `Meta`의 byte 4~11이다. 교차 수가 들어 있던 이 자리는 0으로 고정된다.

## 2. 근거

- 2026-11-17 사용자 결정이다. 교차 판정은 "지우자"(착수는 나중에), Mode 5는 함께 제거하라는 권고에 "기록만 해둬"라고 답했다.
- 2026-11-19 AD-1(`c018aea0`)이 Mode 5를 제거하지 않고 최적화했다. 사용자는 이를 되돌리지 않고 이번 제거에서 함께 걷어내기로 했다.
- 조사 결과(Codex, HEAD `78c4bfe9`):
  - 게시 생략 조건은 `SkeletalPatchCopy::Update`의 `!result.PatchTriangles`뿐이다. 교차 수는 생략 사유 로그 문자열을 고르는 데만 쓴다.
  - 무한 절단 사각형은 패치 bounds·world scale·margin으로 계산하며, 교차 수를 읽지 않는다.
  - 인접 수(`m_Adjacent`)는 병합·게시·mask 변경에 연결되어 있지 않고, `Poll` 로그와 `Validate` 비교에만 쓰인다.
  - Influence 경로(Mode 3·4)는 Mode 0의 교차 결과나 `Meta` 교차 수를 읽지 않는다.
- 따라서 두 제거 모두 동작을 바꾸지 않고, 로그와 진단 검사만 줄어든다.

## 3. 결정과 이유

### D1. (B)를 먼저(RM-1), (A)를 다음(RM-2)에 한다
- (B)는 가장 최근에 넣은 코드이고 범위가 독립적이다. 먼저 걷어내면 (A) 단계의 `Submit`과 셰이더가 단순해진다.
- 두 단계 모두 약 190~270줄(추정)로 단계당 300줄 이내다.

### D2. RM-1: Mode 5·8과 그 자원을 통째로 없앤다
- 셰이더: Mode 5·8 블록, `Adjacent`(u6)·`VertexMarks`(u7) 선언을 없앤다. `DrawInput.VertexBase`는 `Reserved1`로, `VertexMarkWords`는 `Padding1`로 되돌린다. C++와 HLSL 양쪽을 함께 바꾸므로 구조체 크기는 그대로다.
- `InitializePatch`: `PatchRoot`를 14개에서 12개로 줄인다.
- `SkeletalPatchJob::Submit`: VB 쌍 구간 배정, 64비트 정점 합 검사, `VertexCount` 최댓값 덮어쓰기, adjacent·vertexMarks 생성·바인딩, Mode 8→barrier→5, adjacent readback을 없앤다.
- `Poll`의 `adjacentPatch` 로그와 `Validate`의 인접 CPU 집계·`memcmp`·SP7d adjacent 블록을 없앤다. 헤더의 `m_Adjacent`, `m_AdjacentReadback`, `m_VertexMarks`도 없앤다.
- 유지: `HasState`, State SRV(t4), `m_EmptyState`, `m_State`. Mode 0의 `available`이 제거된 삼각형을 빼는 데 계속 필요하다. `DrawInput.VertexCount`도 Mode 3 범위 검사에 쓰이므로 남긴다.
- 사라지는 것: 정점 합이 `UINT32_MAX`를 넘을 때의 제출 거절. 이 검사는 Mode 8 비트 번호용이라 함께 없어져도 된다.

### D3. RM-2: 기본 경로 교차 수를 없애되 `Meta` 바이트 배치는 유지한다
- Mode 0은 `crossing`을 계산하지 않고 Scan에 선택 플래그만 쓴다. Scan .y에는 0을 쓴다.
- Mode 1·6의 `ScanTotals`를 세 성분(선택·교차·미포함)에서 선택 하나로 줄이고, `Counts[64]`를 없앤다.
- Mode 6은 `Meta` 0~11바이트에 `uint3(선택 수, 0, 0)`을 쓴다. byte 4~11은 "예전 교차 수 자리, 항상 0"인 예약 영역으로 남긴다.
  - 이유: AABB(12~32)·world X(36~40)·NodeId readback(44) 배치를 그대로 두기 위해서다. `CheckMaskedPatch`는 `LocalBounds` 44바이트 전체를 비교하므로, 0으로 고정하면 그 비교도 그대로 통과한다. 배치 압축은 별도 작업이다.
- `m_Scan`의 원소 크기(8바이트)와 블록 영역 간격(16바이트)은 이번에 줄이지 않는다. 메모리 최적화는 범위 밖이다.
- `SkeletalPatchResult`에서 `CrossingTriangles`·`UncoveredCrossing`을 없앤다. `PatchTriangles`가 offset 0이라 `Poll`의 `memcpy(&result, metadata, sizeof(result))`는 그대로 맞다.
- 로그와 진단 정리:
  - GPU/CPU `crossing=… uncovered=…` 로그, SP6 로그(`LogCutResult`의 서식 지정자와 인자를 함께 줄임)의 교차 항목, publish-skipped 사유 문자열과 `"zero crossing created a copy"` 메시지를 없앤다.
  - `Validate` 끝의 crossing·uncovered·infiniteCrossing 전체 순회와 비교를 없앤다.
  - `CheckMaskedPatch`(`SkeletalMaskChecks.cpp`)의 교차 존재 검사와 전체 제거 교차 0 검사를 없앤다.
- 유지: 셰이더의 `Crossing`·`Overlaps` 함수(Mode 3이 사용), `Selected`, `ExtractPatchReference`, `SelectInfluenceJointReference`, `CrossesRectangle`과 CPU 테스트, `patchInfinite`/`patchFinite`/`outsideTriangles` 단면 진단.

### D4. 관절 부근 절단 재설계(보류)와의 관계
- 그 재설계안은 패치 선택에 "평면 사각형이 지나가는 삼각형"을 다시 쓴다.
- 이번 제거 후에도 셰이더의 `Crossing`·`Overlaps` 함수는 Mode 3용으로 남는다. 따라서 재설계에서 Mode 0에 다시 붙이기 쉽다.
- 재설계가 쓰는 것은 "선택 조건"이고, 이번에 없애는 것은 "진단용 개수"다. 그래서 충돌이 크지 않다고 판단했다.

### D5. 다른 작업의 미커밋 파일은 건드리지 않는다
- 조사 결과 `ModelViewer.cpp`, `Scene/tests/SkeletalPatchTests.cpp`, `SkeletalPatchReference.h`는 수정할 필요가 없다.

## 4. 단계와 예상 시간

| 단계 | 내용 | 예상 시간 |
|---|---|---|
| RM-1 | (B) 제거 + `PatchAdjacentMarks.md` 상태를 "폐기"로, `SkeletalPatchBreak.md` SP-7d·CodeMap·README 정리 (약 190~245줄, 추정) | Codex 구현 15~25분, D/R 빌드·검사 15~20분, 리뷰 2~5분 |
| RM-2 | (A) 제거 + `SkeletalPatchCopyCut.md`(D7/D7a/D12, 완료 조건, 이미 틀린 "`CrossingTriangles==0`이면 게시 생략" 설명 포함)·`PatchPrefixScan.md` D5·CodeMap·README 정리 (약 190~270줄, 추정) | 같음 |

- 빌드는 한 번에 하나씩, 기존 `MiniEngine/Build`에서 한다.
- 단계마다 새 세션에서 Codex 구현 → 빌드·검사 → 새 Codex 리뷰 → 로컬 커밋 순으로 진행한다. push는 하지 않는다.

## 5. 완료 조건 (두 단계 공통, 단계별로 확인)

1. Debug·Release 빌드 오류 0, D3D12 debug layer ERROR/CORRUPTION 0.
2. `-skeletal-mask-checks 1` PASS. 패치 삼각형 수는 934, 135, 0, 934, 934 그대로이고, SP7c/SP7d의 출처·출력 바이트 비교도 PASS다.
   - 이 비교는 같은 실행 안에서 무마스크·0마스크·해제 결과끼리 맞춰 보는 것이지, 변경 전후 비교가 아니다.
   - 출력 데이터 불변(1절)은 3·4항의 `Validate` GPU==CPU 비교로 확인한다. 이 비교는 독립 CPU 참조(`ExtractPatchReference`)와 VB·출처·순서를 대조한다. 따라서 변경 전후 자료를 따로 보관하지 않는다.
3. `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1`의 20개 요청(Parent·Influence 각 Xbot/CesiumMan) GPU==CPU PASS. 선택 수는 기존 로그와 같다(예: X/P/0 1556, X/I/2 2736, C/P/0 412).
4. `-skeletal-union-checks 1 -skeletal-break-demo 2` PASS(364/352→526, 중복 0, n=1 출처 순서).
5. `-skeletal-break-demo 1`·`2` 게시 PASS, 패치 2118·271.
6. 제거 대상 이름이 패치 추출 파일에서 검색되지 않는다. 검색 대상은 `PatchExtractCS.hlsl`, `SkeletalPatchCopy.cpp/.h`, `SkeletalMaskChecks.cpp`다. 다른 파일의 같은 이름(예: `PhysicsWorldInternal.h`의 `AdjacentPairFilter`, 샘플의 `VertexBase`)과 문서의 역사 기록은 예외다.
   - RM-1: `Adjacent`, `VertexMarks`, `VertexBase`, `VertexMarkWords`, `m_Adjacent`
   - RM-2: `CrossingTriangles`, `UncoveredCrossing`, `Counts`, `infiniteCrossing`
   - 300줄 제한은 실제 diff로 확인하고, 구현 추정 시간과 실측 빌드·검사 시간을 구분해 기록한다.
7. 유지 대상(`Crossing`, `Overlaps`, `HasState`, State SRV, `VertexCount`, Influence 경로)이 남아 있고, Influence 요청의 선택 수가 그대로다.
