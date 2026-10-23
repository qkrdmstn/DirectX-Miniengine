상태: 측정 완료 — `fc1f739` Release, 임시 계측(커밋하지 않고 되돌림). 개선은 미구현, 단계 설계는 `EnterCostReductionDesign.md`

# 스태틱 메시 Enter 1회 비용: 단계별 실측

## 1. 목적

스태틱 메시(bunny) 하나를 Enter로 자를 때 한 프레임이 오래 걸리는 원인을 단계별로 나눠 잰다.
확정 뒤 매 프레임 비용(`CutFrameDropReport.md`)과는 다른 문제이며, 여기서는 Enter 프레임 하나만 본다.

## 2. 측정 환경과 방법

| 항목 | 값 |
| --- | --- |
| CPU / GPU | Intel Core i5-1135G7 / NVIDIA GeForce MX450 (드라이버 591.86), 노트북 |
| 빌드 | Release, `fc1f739` + 임시 계측 패치 |
| 장면 | 시작 장면 그대로: cube(기준) + bunny. Xbot 없음 |
| 평면 | bunny 중심, 법선 +Y에서 로컬 U축으로 30° 기울임, 반 길이 = bunny 반지름 × 1.2 (cube는 후보가 아님) |
| 입력 메시 | bunny: corner 91,014, 용접 뒤 정점 15,258, 삼각형 30,338 (glb가 정점을 공유하지 않는다) |
| 결과 | 12회 모두 `Ok`, 루프 1개. + 조각 정점 7,336 / 면 14,668 / 렌더 정점 42,772, − 조각 8,748 / 17,318 / 50,720 |
| 반복 | 90프레임부터 40프레임마다 `RequestCut`을 자동 호출, 12회 |
| 계측 | 이름 붙인 구간(`steady_clock`)을 경로별로 합산. Enter를 요청한 프레임의 Update부터 다음 프레임의 Update 직전까지만 켰다 |
| 프레임 시간 | Update 시작부터 다음 Update 시작까지의 벽시계 시간. 엔진 deltaT는 16.667 ms로 고정 표시되어 쓰지 않았다 |
| 원본 | `Deliverables/측정/enter_probe.log`(12회 전체), `enter_probe_median.txt`(구간별 중앙값·최소·최대), `enter_probe.patch`(계측 패치, 재측정용) |

표의 값은 12회의 **중앙값**이다. 10·11회차는 모든 구간이 약 2배로 느려 프레임이 약 314 ms였다. 구간 비율은 다른 회차와 같아서
노트북의 클럭 변화나 다른 프로세스 탓으로 보이지만, 원인은 확인하지 않았다.

## 3. 결과

### 3.1 프레임 전체

| 항목 | 중앙값 | 범위 |
| --- | --- | --- |
| Enter 프레임 (Update → 다음 Update) | **약 186 ms** | 175 ~ 314 ms |
| `CutController::ConfirmAll` | 179.9 ms | 168.7 ~ 305.9 ms |
| 그 프레임의 `RenderScene` (조각 첫 업로드 포함) | 5.2 ms | 4.7 ~ 7.9 ms |
| 다음 프레임 | 6 ~ 17 ms | 정상 수준으로 돌아온다 |

CPU 비용의 거의 전부가 `ConfirmAll` 하나(작업 1개)에 있다. `DiscardCut`, 스킨 마스크, `SkinJobs`는 0.1 ms 미만이다.

### 3.2 `ConfirmJob` 단계별

| 단계 | 구간 | ms |
| --- | --- | --- |
| 1. 추출 `ExtractJob` | | **79.3** |
| | `ReadStaticMesh` (원본 버퍼 읽기, corner마다 `unordered_map` 조회) | 24.9 |
| | `BuildMeshGeometry` — 입력 검사 | 2.8 |
| | — 용접 (`nodeOf`, `positionVertexCount` 해시) | 7.6 |
| | — corner·face 배열 | 3.8 |
| | — 경계 에지 태그 (`CollectEdgeUsage` + 해시 4개) | 9.8 |
| | — `ValidateMeshGeometry` ① | 10.5 |
| | — 구간 밖(해시맵 해제 등) | 약 4.6 |
| | `SetTopology` — `ValidateMeshGeometry` ② (방금 ①에서 검증한 메시) | 9.4 |
| | — `BuildRenderMesh` + `SetRenderData` | 5.5 |
| 2. `SkinPatch` → `UpdatePose` | 이미 world 좌표인 스태틱 정점을 다시 변환·복사 | **6.7** |
| 3. `TotalArea` | | 0.4 |
| 4. 절단 `SliceProceduralMesh` | | **89.8** |
| | `Slicer::Run` — 입력 `ValidateMeshGeometry` ③ | 9.2 |
| | — 정점 분류 + 면 처리 (`ProcessFace`, corner 해시) | 13.7 |
| | — `BuildPiece` ×2 (`CollectEdgeUsage` 포함) | 11.8 |
| | — 출력 `ValidateMeshGeometry` ④⑤ | 8.9 |
| | — `ExtractCutLoops` ×2 | 6.0 |
| | — `BuildCap` ×2: 삼각분할 0.7, 조각 복사 1.5, cap 면·에지 검사 6.7, `ValidateMeshGeometry` ⑥⑦ 9.6 | 20.1 |
| | `SetTopology` ×2 — `ValidateMeshGeometry` ⑧⑨ | 9.6 |
| | — `BuildRenderMesh` + `SetRenderData` | 5.6 |
| | 구간 밖 | 약 3.4 |
| 5. 숨김·관절·면적·정보 | | 0.4 |

### 3.3 종류별로 묶으면

| 종류 | ms | Enter의 비율 |
| --- | --- | --- |
| `ValidateMeshGeometry` 9회 (회차별 합의 중앙값) | **약 57.6** | 32% |
| 절단 본작업 (분류·면 처리, `BuildPiece`, 루프, 검증을 뺀 cap) | 약 42 | 23% |
| 용접·경계 태그 (`BuildMeshGeometry`에서 검증을 뺀 부분) | 약 28.6 | 16% |
| `ReadStaticMesh` | 24.9 | 14% |
| 렌더 데이터 생성 3회 (`BuildRenderMesh` + `SetRenderData`) | 11.1 | 6% |
| 스태틱 `SkinPatch` | 6.7 | 4% |
| 구간 밖·기타 | 약 8 | 4% |

- `CollectEdgeUsage`(에지 약 4.5만 개를 `unordered_map`에 넣는다)는 한 번에 약 4.5 ~ 5.5 ms이고, Enter 한 번에 **14번** 불려 합계 약 44.8 ms다.
  9번은 검증 안에서, 5번은 용접 태그·`BuildPiece`·`BuildCap`에서 부른다.
- 삼각분할(`EarClip`)과 `ContainsLoops`는 1 ms 미만이다. 알고리즘의 차수가 아니라 **메시 전체를 해시맵으로 여러 번 훑는 것**이 비용이다.
- GPU 쪽 첫 업로드(`ProceduralMeshRenderer::Upload` 6회)는 CPU 3.6 ms다. Enter 비용에서 작은 부분이다.

## 4. 해석

언리얼 `SliceProceduralMesh`는 섹션 배열을 한 번 훑으면서 결과 배열을 만들고 검증하지 않는다(`UnrealSourceComparison.md` 1절).
우리 Enter는 용접, 위상 구성, 검증을 매번 처음부터 하며, 그중 상당 부분이 같은 메시를 거듭 다룬다.

| 반복 | 근거 | ms |
| --- | --- | --- |
| ② `SetTopology` 검증 | ①과 같은 메시 | 9.4 |
| ③ `Slicer::Run` 입력 검증 | 입력은 ②에서 검증해 `ProceduralMesh`에 들어간 `m_Geometry` | 9.2 |
| ⑧⑨ `SetTopology` 검증 | `BuildCap`(⑥⑦)이 검증한 조각을 그대로 받는다 | 9.6 |
| 추출 전체 | 스태틱 메시는 로드 뒤 바뀌지 않는데 Enter마다 다시 읽고 용접한다 (`UpdateStaticMeshCache`도 로드 때 같은 메시를 한 번 읽는다) | 79.3 |
| `SkinPatch` | 스태틱 정점은 `ReadStaticMesh`가 이미 world로 옮겼다 | 6.7 |

④⑤(`Slicer::Run` 출력 검증)는 단순 반복이 아니다. 잘린 면 중 축퇴 기준 아래의 가는 조각을 거절하는 역할이 있어(`Slicer::Run` 주석),
빼려면 새로 생긴 면만 보는 좁은 검사로 바꿔야 한다. ⑥⑦도 cap을 붙인 결과의 유효성을 보장한다.

## 5. 줄일 수 있는 양 (측정값으로 계산한 추정, 구현 전)

| 안 | 내용 | 줄어드는 Enter 비용 | 대가 |
| --- | --- | --- | --- |
| A | 스태틱 메시의 용접 결과(`ProceduralMesh`)를 로드 때 한 번 만들고 Enter 때 복사해 쓴다 | 약 79 ms | 로드 때 메시당 약 80 ms, 메모리 |
| B | 단순 반복 검증 ②③⑧⑨를 뺀다 (Debug에서는 유지 가능) | 약 28 ms (A를 하면 ③⑧⑨ 약 19 ms) | 검증 지점을 계약으로 문서화해야 한다 |
| C | 스태틱 작업에서는 `SkinPatch`를 건너뛴다 | 약 6.7 ms | 없음 (정점이 이미 world) |
| D | `CollectEdgeUsage`·corner 매핑의 `unordered_map`을 정렬 배열·인덱스 배열로 바꾼다 | 측정 필요 (해시 합계 약 45 ms 중 일부) | 변경 범위가 넓다 |

A + B + C를 하면 Enter는 약 180 → 75 ms로 줄 것으로 추정한다. 그래도 60 Hz 기준 4 ~ 5프레임이므로, 끊김을 없애려면
D나 절단을 다른 스레드로 옮기는 일이 이어서 필요하다. 이 수치는 구간 중앙값을 빼서 얻은 추정이며 구현 후 같은 패치로 다시 잰다.

## 6. 재측정 방법

1. `git apply Deliverables/측정/enter_probe.patch` (`MeshCutting/TempProbe.h`를 만들고 구간을 넣는다. `ModelViewer::Update`의 드라이버가 로그 경로를 쓰므로 경로를 맞춘다)
2. Release 빌드 후 `MiniEngine/ModelViewer`에서 실행. 약 10초 뒤 로그에 `DONE`이 찍히면 종료한다
3. 측정이 끝나면 `git checkout -- MiniEngine`으로 되돌리고 `TempProbe.h`를 지운다

## 7. 측정하지 못한 것

- 스켈레탈 작업(Xbot)의 Enter 단계별 비용. 이번 장면에는 Xbot이 없다.
- 10·11회차가 2배 느렸던 원인.
- GPU 시간. 업로드는 CPU 쪽 호출 시간만 쟀다.
- 이전 측정(`4f4bfb0`, Xbot 장면, 추출 약 110 ms·절단 약 120 ms)과의 차이 원인. 장면과 cap 정책(`fc1f739`)이 달라 직접 비교하지 않는다.
