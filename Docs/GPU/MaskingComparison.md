상태: 완료 — 측정 기록(MC5·MC6·MC7, 코드 미반영), MC7 기준 `aaa8c4a7` (2026-11-24)

# 원본 메시 마스킹 방식 성능 비교

절단 패치가 떨어져 나간 자리의 원본 스켈레탈 메시를 숨기는 네 방식을 같은 장면에서 비교했다. 실험 코드는 저장소에 반영하지 않았고, 재현용 패치와 원본 결과만 `MaskingExperiments/`에 둔다. 현재 엔진 코드의 마스킹은 압축 IB(CP-8)다.

## 1. 비교 방식
| 방식 | 숨기는 위치 | 판정 규칙 |
| --- | --- | --- |
| 퇴화 IB | 제거 삼각형의 인덱스를 (a,a,a)로 바꾼 IB | 삼각형 단위 |
| 압축 IB (현재) | 남는 삼각형만 모은 IB(`TriangleCompactCS`) | 삼각형 단위 |
| VC + ClipDistance | VS가 정점 마스크로 `SV_ClipDistance0` 출력 | 정점 값 보간, 경계 삼각형이 반만 잘림 |
| VC + PS discard | VS가 숨김=1을 보간, PS가 h > 0이면 discard(shadow·depth·color 모두) | 꼭짓점 하나라도 숨김이면 삼각형 전체 제거 |

VC 마스크는 VS-1의 정점 판정 비트(Mode 8)를 입력으로 쓴다([VertexSelection.md](VertexSelection.md)).

## 2. 결과 (MC7, MX450, Release, Xbot 절단 1회)
| 방식 | 마스킹 CPU (게시당) | 마스킹 GPU (게시당) | 추가 인스턴스당 | 렌더 GPU i1 / i10 (ms) | PS 실행 수 | 마스크 메모리 | 화면 차이 (압축 IB 대비) |
| --- | --- | --- | --- | --- | ---: | --- | --- |
| 퇴화 IB | 0.23~0.27 ms | 33 µs | 0.40 ms | 0.372 / 3.11 | 92,013 | 589 KB × 인스턴스 | 0 px |
| 압축 IB | 0.67~0.70 ms | 51~53 µs | 1.0~1.5 ms | 0.371 / 3.19 | 92,013 | 564 KB × 인스턴스 | 기준 |
| VC + ClipDistance | 약 0.002 ms | 사실상 0 | 사실상 0 (공유) | 0.357 / 2.96 | 92,013 | 7 KB (공유) | CesiumMan 926 px |
| VC + PS discard | 약 0.002 ms | 사실상 0 | 사실상 0 (공유) | 0.360 / 2.90 | 230,476 | 7 KB (공유) | 0 px |

- 공통 비용인 정점 판정(Mode 8, 약 32~38 µs)은 표에서 뺐다. 마스킹 값은 두 번째 절단부터의 정상 상태다.
- 렌더 차이는 대부분 측정 잡음 범위 안이다. PS discard의 PS 실행 수가 2.5배인 것은 원래 PS가 없던 shadow·depth 패스에 마스크 PS가 붙기 때문이다.
- VC 마스킹 CPU 비용 변화: MC5 4~8 ms → MC6 0.7~1.7 ms → MC7 약 0.004 ms.

## 3. 단계별 실험
| 실험 | 내용 | 결과 파일 |
| --- | --- | --- |
| MC5 | 네 방식 첫 구현·비교 | [MC5-results.md](MaskingExperiments/MC5-results.md) |
| MC6 | VC 마스크 갱신을 GPU로 이동 | [MC6-results.md](MaskingExperiments/MC6-results.md) |
| MC7 | VC 최적화: Mode 8 비트 재사용, 같은 dispatch 누적(Mode 9), 마스크 2장 ping-pong, 인스턴스 공유, 원본 R16 IB | [MC7-results.md](MaskingExperiments/MC7-results.md) |

재현: 각 `MC*-experiment.patch`를 측정 당시 기준 커밋 위에 `git apply`한다. MC7은 `aaa8c4a7`, MC6은 `06edbd76`이다. MC5는 MC6 이전 master에서 만들었고 기준 커밋을 따로 기록하지 않았다.

## 4. 제한
- 단일 GPU(MX450), 조합당 1회 실행이다.
- baseVertex ≠ 0 모델과 Debug 빌드는 검증하지 않았다.
- MC7의 중복 barrier 경고 4,316건은 HEAD와 대조하지 않았다.
- MC5 결과 파일은 생성 당시 한글 인코딩이 일부 손상됐다(숫자·표는 원본과 같다).
