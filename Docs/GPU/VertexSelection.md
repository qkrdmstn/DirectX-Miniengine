상태: 설계안 — base `0583e8ff` (2026-11-24)

# 정점 단위 선택 판정 (VS)

## 1. 목표
- 패치 추출 Mode 0이 삼각형마다 꼭짓점 3개의 `Selected(v)`를 다시 계산하는 중복을 없앤다. 공유 정점은 평균 약 6번 판정된다.
- 판정은 정점당 한 번 하고, 결과를 1비트 배열로 남긴다. 이 배열은 나중에 PS discard 마스크(MC5·MC6 실험)의 입력으로도 쓸 수 있다. 이번 범위에서는 마스킹 방식은 바꾸지 않는다(압축 IB 유지).
- 출력(패치 VB·IB, TriangleSources, Meta, State)은 바이트 단위로 바뀌지 않는다.

## 2. 근거
- `PatchExtractCS` Mode 0: `Selected(s,indices.x)||Selected(s,indices.y)||Selected(s,indices.z)`. `Selected`는 정점마다 skin 16바이트를 읽고 본 4개를 `Mask`·`Chosen`과 비교한다.
- 바꿀 수 없는 단계: Mode 3/4(교차 삼각형의 본 투표·선택)는 Mode 0보다 먼저 끝나야 하고, Mode 1/6/2(삼각형 prefix·복사)는 삼각형 단위 출력이라 그대로 둔다.
- Mode 0 단독 시간은 측정된 적이 없다(Xbot 추출 전체 약 330 µs). 이득 크기는 VS-1에서 잰다.

## 3. 결정과 이유
### D1. 새 Mode 8 = 정점 판정 패스
- 위치: `SkeletalPatchCopy`의 추출 기록에서 Mode 3/4(influence일 때) 뒤, Mode 0 앞. 제거된 번호 5·7은 재사용하지 않는다.
- 스레드 하나가 정점 하나를 맡는다. 판정식은 기존 `Selected(DrawInput, i)`를 그대로 호출한다(규칙 복제 금지).
- section마다 정점 범위를 `DrawInput`으로 안다. 같은 section의 draw들은 VB를 공유하므로 section의 첫 draw 입력으로 판정하고, 비트 위치는 section별 word offset으로 정한다.
- skin이 없는 section은 비트를 0으로 둔다(현재 `Selected`가 false를 반환하는 것과 같다).

### D2. 비트 쓰기는 groupshared 누적 후 word당 한 번 store
- 64스레드 그룹 = 2 word. 그룹 안에서 `InterlockedOr`(groupshared)로 모으고, word 담당 스레드가 전역 버퍼에 한 번 쓴다.
- section 시작을 word 경계에 맞춰 두면 그룹이 section 경계를 넘지 않고 전역 원자 연산이 필요 없다.
- wave 크기(32 등)를 가정하지 않는다.

### D3. Mode 0은 비트 3개만 읽는다
- `Selected(s, v)` 호출을 비트 읽기 `(Bits[(s.WordOffset + (v>>5))] >> (v&31)) & 1`로 바꾼다. `v`는 지금과 같은 `Index()` 결과(baseVertex 포함)다.
- 기존 `available`(State bit31) 판정, Scan 기록, 월드 X 폭 집계는 그대로 둔다. X 폭을 정점 패스로 옮기면 IB가 참조하지 않는 정점이 섞여 Meta가 바뀔 수 있으므로 이번 범위에서 뺀다.

### D4. 버퍼와 수명
- 비트 버퍼는 추출 job이 보유하고 재사용한다. 크기는 section별 `ceil(VertexCount/32)` word 합이며, 모자랄 때만 다시 만든다(RetireResource).
- `DrawInput`의 예약 필드(`Reserved1`)를 section word offset으로 쓴다. CPU는 입력 표를 만들 때 이 값을 채운다.

## 4. 단계와 예상 시간
| 단계 | 내용 | 예상 |
| --- | --- | --- |
| VS-1 | Mode 8 추가, Mode 0 비트 읽기 전환, 버퍼·입력 표, 추출을 부르는 모든 지점 반영 | 구현·Release 빌드 약 30분, 회귀 약 10분 |
| 리뷰 | 구현과 다른 실행이 5절 완료 조건으로 검토 | 약 5~10분 |

## 5. 완료 조건
1. Release 빌드 오류 0.
2. 회귀 PASS: `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1`(GPU==CPU), `-skeletal-patch-break-checks 1`(Parent/Rig), `-skeletal-mask-checks 1`, `-skeletal-recut-repro 1`.
3. 출력 바이트 비교: CP-5·CP-6a와 같은 고정 입력 방식으로 HEAD와 surface VB·depth VB·IB 유효 구간, TriangleSources, Meta가 전부 일치.
4. `-debug 1`에서 D3D12 ERROR/CORRUPTION 0.
5. 추출 GPU 시간: Mode 8 + Mode 0 합을 HEAD의 Mode 0과 비교해 기록(Xbot, CesiumMan, 재절단). 느려지면 결과만 기록하고 사용자에게 유지 여부를 묻는다.
6. 변경 약 300줄 이내, 함수·단락 한글 주석 규칙 준수.

## 6. 위험
- 정점 패스는 IB가 참조하지 않는 정점도 판정한다. Mode 0이 참조 정점의 비트만 읽으므로 결과는 같다.
- `Influence`일 때 `Chosen`은 Mode 4 결과다. Mode 8 앞에 Mode 4의 UAV barrier가 있어야 한다.
- 추출을 부르는 경로가 여럿이면(원본, 재절단, 검사용) 한 곳이라도 빠지면 비트가 비어 선택이 0이 된다. 회귀 2·3이 이를 잡는다.

## 7. 결과
(VS-1 후 기록)
