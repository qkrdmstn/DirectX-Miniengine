상태: 진행 중 — AL-1 `74f4940d` (base `7fe9aa0b`), AL-0·AL-1 구현·검증 기록 (2026-11-02)

# cap 내부 점 검사 대상 목록 압축

상위 문서: [CapScanMarking.md](CapScanMarking.md)(현재 cap 선택 계약), [CapParallelOptimization.md](CapParallelOptimization.md)(단계 2 "검사 집합을 정확히 같게").

## 1. 목표

- `CapEarsCS`의 `SelectCached`가 ear 후보마다 도는 내부 점 검사(InTriangle) 루프의 비용을 줄인다.
- cap 결과는 바꾸지 않는다. 같은 구성에서 변경 전과 cap checksum·rounds/marked/removed가 같아야 한다.
- 수치 목표는 두지 않는다. 변경 전후 교차 측정으로 개선 여부만 판정한다.

## 2. 근거 측정 (MX450, Release, `Tools/RunCutBenchmark.ps1` 기본 Bunny y=0, 2026-11-02)

임시 셰이더 수정(커밋하지 않음)으로 cap 단계(1 loop, 370점, 22라운드, 약 1.40 ms)를 나눴다. 각 3회.

| 구간 | 방법 | 추정 비용 |
| --- | --- | --- |
| 빈 dispatch 바닥 | 셰이더 시작/leader 확인 직후 return | 0.18~0.22 ms |
| leader 순회 | 순회 직후 return − leader 확인 직후 return | 약 3 µs |
| 초기화·면적 | 초기화 직후 return − 순회 직후 return | 수 µs |
| 라운드 22회 | 전체 − 초기화 직후 return | 약 1.2 ms |
| InTriangle 루프 | 루프 3회 반복 시 증가분 / 2 | 약 1.1 ms(반복 삽입 영향으로 과대 가능) |
| 오목 점 표시 pass | pass 3회 반복 시 증가분 / 2 | 약 0.19 ms |
| scan 1회 / barrier 1개 | 추가 삽입 증가분 | 1 µs 미만 / 약 0.3 µs |

5060 Ti에서는 나누어 재지 않았다. 순서(InTriangle이 지배적)는 같다고 가정하고 AL-1 후 실측으로 확인한다.

## 3. 현재 구조 (Codex 조사, `ea6ea51f`)

- `SelectCached`: 각 스레드가 연속 k=ceil(LoopLength/CAP_THREADS)개 위치를 소유한다(Bunny k=2).
- 표시된 Kind=1 후보마다 소유 스레드가 `test=0..LoopLength-1`을 직렬로 돈다. `LoopLength`는 제거 후에도 원래 길이(370)다.
- 칸마다 `test!=a/v/c` → `Active(test)` → `AllPoints || bit4(reflex)` → `InTriangle` 순으로 검사하고, 첫 포함점에서 탈락·break한다.
- 포함점이 없는 진짜 ear는 370칸을 끝까지 돌고, 같은 wave의 다른 lane이 기다린다.
- 검사 집합 = `Active && (AllPoints || turn<=0)`, a/v/c 제외. 공선(turn=0)을 포함한다.
- `AllPoints`는 leader `Ring +52`의 bit1(`LinkBoundaryCS`의 근접·평행 위험)로 정해지며 라운드 중 바뀌지 않는다. Bunny y=0은 badBoundaryMask=2라 AllPoints=1이다. 즉 Bunny의 검사 집합은 활성 점 전부다.
- groupshared는 256에서 24,888 B, 한도 32 KiB.

## 4. 결정과 이유

1. **라운드마다 검사 집합을 groupshared 목록으로 압축하고, InTriangle 루프는 그 목록만 돈다.**
   - 이유: 제거된 점과 집합 밖 점을 매 칸 확인하는 낭비를 없앤다. 라운드가 진행될수록 목록이 짧아진다.
   - "오목 점만"(S3 원안)이 아니라 현재 검사 집합 그대로를 담는다. Bunny는 AllPoints=1이라 오목 점만 모으면 결과가 바뀌기 때문이다.
2. **목록은 main의 기존 오목 점 표시 pass에서 만든다.** 같은 pass가 bit4를 쓰므로 집합 판정에 필요한 값이 그 자리에 모두 있다. 위치 수 합산은 기존 `BlockExclusiveSum`을 쓴다.
3. **목록 순서는 결과에 영향이 없다.** 검사는 "하나라도 안에 있으면 탈락"인 OR이고, InTriangle 산술·parity·홀수 끝 후보 제외·무진행 재검사·Remaining 한도는 그대로 둔다. break 위치만 달라질 수 있다.
4. **Cached 경로(m ≤ 1024)만 바꾼다.** `SelectMapped`(m > 1024)는 전역 scratch·UAV 동기화가 필요해 범위 밖이다.
5. **한 단계에 한 개념.** Kind 판정과 오목 점 표시를 한 pass로 합치는 것, AllPoints=1일 때 오목 판정 생략, 여러 lane이 한 후보를 나눠 검사하는 협력 검사는 AL-1 측정 뒤 별도로 결정한다.

## 5. 인터페이스 골격 (`CapEarsCS.hlsl`)

```hlsl
groupshared uint TestList[1024];   // 이번 라운드 검사 집합의 캐시 위치
groupshared uint TestCount;        // 목록 길이

// main 라운드 루프의 오목 점 표시 pass (Cached일 때)
//   각 스레드: 소유 위치마다 bit4 갱신, 집합 포함 여부(Active && (AllPoints || reflex)) 계산·개수 합산
//   uint total; offset = BlockExclusiveSum(myCount, waveSlot, total)  // 모든 스레드가 균일하게 호출, 소유 점 없으면 0
//   한 스레드(waveSlot==ZeroSlot)만 TestCount = total 기록      // out을 groupshared로 직접 받지 않는다
//   소유 위치 중 포함된 것을 TestList[offset++]에 기록
//   기존 AllMemoryBarrierWithGroupSync로 목록과 TestCount를 게시

// SelectCached의 Kind=1 검사
//   for (t = 0; t < TestCount; t++) { test = TestList[t]; if (test != a/v/c && InTriangle(...)) { 탈락; break; } }
```

- 추가 groupshared 4,100 B → 256에서 약 28,988 B(한도 내). 64/512도 컴파일 결과로 확인한다.
- Mapped 경로에서는 목록을 만들지 않고 기존 루프를 그대로 쓴다. `BlockExclusiveSum` 호출은 그룹 균일 조건(`Cached`)으로만 분기한다.
- 함수 위 1~2줄, 단락별 한 줄 한글 주석(AGENTS.md)을 지킨다.

## 6. 단계

| 단계 | 내용 | 예상 |
| --- | --- | --- |
| AL-0 | 변경 전 기준선 기록: Debug/Release × 6사례 × 3회 checksum·rounds/marked/removed, CAP_THREADS 64/512 Release(Bunny y=0·재절단), Release 벤치 3회 | 빌드 약 2분×구성 + 실행 약 10분 |
| AL-1 | 5절 구현(Cached 경로만) | Codex 구현 5~10분 + 새 실행 리뷰 2~5분 + 빌드·검증 약 20분 + 벤치 교차 3+3회 약 5분 |
| 이후 | AL-1 결과로 Kind/오목 pass 통합, 협력 검사 여부를 새 결정으로 | — |

AL-1은 변경 약 300줄 이내로 하고, 끝나면 이 문서 상태 줄과 README 행을 갱신한 뒤 멈춰 사용자가 읽고 커밋한다. 5060 Ti 측정은 사용자가 `Tools/RunCutBenchmark.ps1`로 한다.

## 7. 완료 조건 (AL-1)

- Debug/Release 빌드 exit 0, `git diff --check` 통과, 새 D3D12 ERROR/CORRUPTION 없음.
- 사례: Cube y=0·45°, Bunny y=0·-240·-230·재절단(`-cut 2`), `-cut-validate 1 -cut-expect-loops 1`. 기존 기하 검증 통과.
- 같은 구성(Debug는 Debug, Release는 Release)의 AL-0 기준선과 `Static cut cap checksum`(side별 삼각형 수·hash), segments/loops/uncappedSegments, rounds/marked/removed가 모든 사례·3회 반복에서 같다.
- CAP_THREADS 64/256/512 Release에서 Bunny y=0·재절단 checksum이 서로 같다. 각 변형의 groupshared가 32 KiB 이하다.
- 임시 대조 빌드(커밋하지 않음): 같은 후보에 목록 검사와 기존 370칸 검사를 함께 돌려 판정 불일치 수, `attempt=1` 진입 수, loop별 AllPoints 값을 Meta에 기록한다. 모든 사례에서 불일치 0이어야 한다. `attempt=1`과 AllPoints=0 경로가 실제로 실행된 사례를 기록하고, 실행된 사례가 없으면 그 경로를 "미검증"으로 명시한다.
- 1024 초과 경로 회귀: Dragon y=0.08 1회 Release에서 기존 검증 통과, 변경 전과 checksum 동일.
- 성능: MX450 Release 벤치를 변경 전/후 교차 순서로 각 3회. cap 중앙값 범위가 겹치지 않을 때만 개선으로 기록하고, 겹치면 "개선 미확인"으로 적는다.
- 리뷰는 구현과 다른 새 Codex 실행이 이 절을 기준으로 한다.

## 8. 위험과 미확인

- checksum은 위치·winding의 순서 무관 해시다. 라운드별 선택 동일성은 rounds/marked/removed 일치로 간접 확인할 뿐 직접 증명하지 않는다.
- 목록 작성과 읽기 사이 게시 barrier 누락, 비균일 분기 안의 `BlockExclusiveSum` 호출, 목록 overflow(m ≤ 1024라 발생하지 않아야 함)가 주요 구현 위험이다.
- 개선 폭은 측정 전에는 말하지 않는다. 라운드별 활성 점 수 분포가 비용을 정한다.

## 9. AL-0·AL-1 실행 기록 (2026-11-02)

- 작업 위치: `.worktrees/cap-active-list`, branch `cap-active-list`, base `7fe9aa0b`; 커밋·push 없음.
- `TestList[1024]`·`TestCount`를 추가하고 Cached만 변경했다. local total·단일 작성자·기존 게시 barrier를 사용하며 `SelectMapped`는 문자 단위로 동일하다.
- AL-0 및 AL-1 각각 41/41회 통과: Debug/Release × 여섯 사례 × 3회, Release 64/512의 Bunny y=0·재절단, Dragon 1회.
- 각 단계 기하 검증 98/98회와 게시 검증 통과. 같은 구성의 checksum·segments/loops/uncappedSegments·rounds/marked/removed가 모두 같다.
- Debug/Release 두 프로젝트 빌드 exit 0, `git diff --check` 통과, D3D12 ERROR/CORRUPTION 0건. 수정 시각 갱신·헤더 재생성을 확인한 빌드/측정만 채택했다.
- Release 64/256/512 checksum 동일. DXC cs_6_2/HV2021 groupshared는 각각 28,796/28,988/29,244 B로 32 KiB 이하이다.
- 임시 대조: 7사례·8게시·11 loop, Cached 후보 1,907회 비교에서 불일치 0건. 계측 코드는 제거했다.
- AllPoints=0 실행: Cube 두 입력, Bunny -240·-230, 재절단 두 번째 게시. Bunny y=0·재절단 첫 게시·Dragon은 AllPoints=1이다.
- attempt=1 진입은 모든 사례에서 0회이므로 해당 경로는 미검증이다.
- Dragon y=0.08: 최대 loop 1070·mapped loop 1, 기하 검증 통과, 양쪽 cap 418개와 checksum 모두 AL-0와 동일하다.
- MX450 / 드라이버 591.86, Release `RunCutBenchmark.ps1 -SkipBuild -Runs 1`, before→after 교차 3쌍(각 20표본).
- cap 중앙값(ms): before 2.4305/1.9067/2.1258, after 1.1023/0.7451/1.2728. 범위 비중첩으로 개선; 세 중앙값의 중앙값은 2.1258→1.1023 ms(약 48.15% 감소).
- 구현 대화를 공유하지 않은 별도 Codex 정적 리뷰 PASS, 보고할 결함 0건. 최종 셰이더 diff는 +26/-6줄이다.
- 로그·DXIL·백업·해시·집계는 `%TEMP%/cap-al-wt/`의 `evidence.json`, `verify-*.json`, `cross-results.json`, `bench-*/`에 있다. 임시 편집마다 복원 해시를 확인했다.
- 미검증: attempt=1, 5060 Ti, 앱 정상 종료(helper는 게시 marker 뒤 종료). 기존 Bunny uncappedSegments 207/49와 badBoundaryMask는 유지된다.
