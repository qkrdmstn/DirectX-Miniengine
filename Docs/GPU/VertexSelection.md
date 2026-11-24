상태: 완료 — VS-1 b9d9a999 (base `65ffecaf`, 2026-11-24); 추출 전체 시간은 HEAD와 같음, 비트 배열은 VC 마스크 입력용

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
### VS-1 (base `65ffecaf`, 커밋 대기, 2026-11-24)
- 변경: `PatchExtractCS`에 `Mode 8`(정점 1개당 스레드 1개, groupshared `InterlockedOr` 뒤 word당 1회 store)과 `SelectedBit`(Mode 0이 비트 3개만 읽음)을 추가했다. `DrawInput.Reserved1`은 `WordOffset`으로 이름을 바꿨고 `Parameters.Padding1`은 `BitDraw`(Mode 8이 맡은 section의 첫 draw 번호)로 쓴다. `SkeletalPatchJob::Submit`이 section별 word offset·`bitSections`·비트 버퍼(`m_Bits`, 모자랄 때만 `RetireResource` 뒤 재생성)를 만들고, skin section마다 Mode 8을 따로 dispatch한 뒤(Mode 3/4 뒤, Mode 0 앞) UAV barrier를 한 번 둔다. skin 없는 section은 dispatch하지 않으며 `SelectedBit`이 비트를 읽기 전에 false를 반환하므로 비트 버퍼 초기화는 필요 없다. `Selected`는 `Chosen`을 한 번만 읽도록 바꿨고 판정식은 같다. `PatchRoot`에 UAV u6(root 12)을 추가했다. 변경 3파일 +83/-12줄.
- 호출 지점: PatchExtractCS를 dispatch하는 곳은 `SkeletalPatchJob::Submit` 하나뿐이다. 원본 절단·재절단·검사가 모두 이 함수를 거친다.
- Release/Windows(x64) 빌드 오류 0, NVIDIA GeForce MX450(1906 MB). 회귀 모두 exit=0, FAIL 0: `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1`(20요청 GPU==CPU), `-skeletal-patch-break-checks 1` Parent/Rig(boundary pass=4/fail=0/n/a=2), `-skeletal-mask-checks 1`, `-skeletal-recut-repro 1`. 계측 제거 뒤 최종 바이너리로도 같은 결과다.
- 바이트 비교(CP-5 방식, `-capture-frame 3 -animation-step-ms 16`, HEAD 계측 빌드 대비): surface VB·depth VB·IB 유효 구간·TriangleSources·Meta 44바이트가 데모 1/2 각 1요청, 재절단 2요청, offset-copy 20요청 합계 **24요청·72파일 전체 일치**. 재절단 두 번째 요청은 비동기 시점 차이로 frame이 109~110으로 어긋나는 실행이 많아 기준과 같은 frame=111인 재실행으로 비교했다.
- `-debug 1`: Xbot 데모, patch-checks+offset-copy, 재절단 모두 exit=0, D3D12 ERROR/CORRUPTION 0(기존 WARNING만).
- 추출 GPU 시간 1: Mode별 timestamp(각 Mode 직전 barrier flush, `-debug 0`, 새 프로세스 3회 평균 µs, 재절단 6표본). 첫 구현 v1은 정점마다 draw를 이진 탐색하는 단일 dispatch, 현재 v2는 section별 dispatch다.

| 조건 | HEAD Mode 0 | v1 Mode 8 + Mode 0 | v2 Mode 8 + Mode 0 |
| --- | ---: | ---: | ---: |
| Xbot | 70.4 | 32.3 + 45.6 = 77.9 | 42.0 + 45.5 = 87.5 |
| CesiumMan | 23.2 | 11.4 + 10.8 = 22.2 | 11.2 + 11.0 = 22.2 |
| 재절단(중앙값) | 75.2 | 32.8 + 49.2 = 82.0 | 43.3 + 48.9 = 92.2 |

- 추출 GPU 시간 2: dispatch별 flush·timestamp가 section마다 Mode 8의 겹침을 막아 v2를 부풀리므로, 시작·끝 timestamp만 둔 추출 전체 시간도 따로 쟀다(새 프로세스 3회, 재절단 6표본, µs).

| 추출 전체 | HEAD | v1 | v2 |
| --- | ---: | ---: | ---: |
| Xbot 평균 | 312.5 | 313.1 | 314.1 |
| CesiumMan 평균 | 54.7 | 52.3 | 52.4 |
| 재절단 중앙값 | 310.3 | 313.3 | 317.3 |

- 해석: Mode 0은 Xbot에서 70.4→45.5 µs로 줄지만 Mode 8이 그만큼을 되가져가 전체 시간은 HEAD와 사실상 같다(Xbot +0.6~1.6 µs, CesiumMan -2.3 µs, 재절단 +3~7 µs, 모두 잡음 범위 근처). 이진 탐색 제거와 section별 dispatch는 이득이 없었다. dispatch마다 고정 비용이 있고 Mode 8의 시간이 정점 수보다 지연에 좌우되는 것으로 보이며(미검증), v1이 v2보다 약간 낫거나 같다. 재절단의 v2 평균 342.3 µs는 6표본 중 439 µs 이상치 1개 때문이다. 유지 여부는 **사용자가 결정**한다. 이 결과만으로는 VS-1이 성능을 개선하지 못하며, 이득은 이 비트 배열을 나중에 PS discard 마스크(MC5·MC6)가 쓸 때 생긴다.
- 제한: 단일 GPU·드라이버 미통제, 한 세션 3회 표본. 근거·로그는 `%TEMP%/VS1/`에 보관한다(커밋하지 않음). 임시 계측(timestamp·덤프)은 모두 제거했고 최종 실행 파일에 계측 문자열이 없음을 확인했다.
