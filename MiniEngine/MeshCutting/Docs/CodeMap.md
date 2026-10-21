# 코드 지도: 실행 흐름과 상태의 주인

> 목적: 코드를 처음 열었을 때 "누가 무엇을 부르고, 상태는 어디에 있는가"를 빨리 찾기 위한 한 장 요약이다.
> 줄 번호는 적지 않는다(자주 바뀐다). 함수 이름으로 찾는다. 알고리즘 설명은 `PlaneSlicingDesign.md`, `PlaneFirstCutDesign.md`, `CutDebugDesign.md`.

## 1. 계층

```
ModelViewer.cpp        입력을 읽어 컨트롤러에 전달하고, 프레임 갱신과 그리기를 호출한다
   │
   ├─ Runtime/CutController      후보 패치, Enter 확정 절단, 스키닝을 조율한다 (그리기 없음)
   │     ├─ BonePatch            Model에서 패치/뼈대를 읽는다 (Model 의존은 여기까지)
   │     └─ Geometry/*           Model, D3D12를 모르는 순수 계산
   │
   └─ Debug/CutDebugView         컨트롤러의 결과를 읽어 ProceduralMesh로 굽고 그린다 (상태를 바꾸지 않는다)
         └─ Rendering/ProceduralMeshRenderer   GPU 버퍼와 PSO
```

의존은 위에서 아래로만 간다. `Geometry/`는 `Model`, `Core`를 include하지 않아서 `tests/PlaneSlicerTests.cpp`가 GPU 없이 단독 빌드된다.

## 2. 프레임 흐름

`ModelViewer::Update` → `ModelViewer::RenderScene` → `ModelViewer::RenderUI` 순서다.

| 순서 | 위치 | 하는 일 |
| --- | --- | --- |
| 1 | `ModelViewer::Update` (키 처리) | T, Enter, Ctrl+Z와 마우스 기즈모 입력을 `CutController`에 전달한다. 컨트롤러는 키를 모른다 |
| 2 | `ModelInstance::Update` | 이번 프레임 포즈 계산 |
| 3 | `CutController::Update` | 아래 3절 |
| 4 | `ModelViewer::RenderProceduralMeshes` → `CutDebugView::Draw` | `Sync`로 메시를 갱신하고 그린다 |
| 5 | `ModelViewer::RenderUI` → `CutDebugView::DrawUI` | 상태 줄 출력 |
| 종료 | `ModelViewer::Cleanup` → `CutDebugView::Release` | GPU 버퍼 반납 |

## 3. CutController::Update (포즈 갱신 뒤 한 번)

```
UpdateTargets: 모델 교체 확인, 편집 중 현재 뼈대 읽기와 최초 원본 캐시 준비
절단 모드(T)이면 UpdateCandidates
    RefreshPick: 자세 변경 시 전체 캐시 정점 스키닝, 자세/평면 변경 시 CutPick::PickJoints
    PickSegments는 뼈 선분 tube 표시용
    ApplyDiff: 새 후보 joint마다 ExtractJob(ExtractBonePatch → MakeWeldInput → BuildPatchPiece)
    사라진 후보는 패치 해제, 확정 결과가 없으면 작업 삭제
편집 모드가 아니면 모든 후보 해제 (확정 결과는 유지)
SkinJobs: 후보 패치와 확정 조각을 BuildSkinMatrices → Skinning::Apply로 이번 포즈에 맞춤
Enter 요청이 있으면 ConfirmAll
    이전 확정 결과를 모두 지운 뒤 후보마다 SliceWithPlane을 한 번 수행
    SliceWithPlane = SlicePatch(무한 평면, 단면 생성) + CutPlane::ContainsLoops(사각형 검사)
    성공한 조각만 저장, 실패한 작업은 status/detail로 표시하고 자동 재시도하지 않음
```

자동 시험 절단과 예상 고리 표시는 없다. 후보가 사라져도 확정 조각은 남고, Ctrl+Z나 다음 Enter에서 지운다.

## 4. 상태의 주인 (`Runtime/CutJob.h`, CutController가 관리)

| 구조체 | 담는 것 | 쓰는 곳 | 읽는 곳 |
| --- | --- | --- | --- |
| `CutTarget` | 등록 인스턴스, 팔레트, 현재 뼈대, 표시용 선분 교차 | `UpdateTargets`, `UpdateCandidates` | 대상 선택, 선분 표시 |
| `CutTargetSource` | 전체 메시 원본 캐시, 현재 정점 위치, 마지막 선택 결과 | `BuildSource`, `RefreshPick` | 삼각형 교차와 joint 선택 |
| `CutJob` | joint별 후보 패치, 확정 조각 둘, 마지막 절단 상태와 시간 | `ApplyDiff`, `SkinJobs`, `ConfirmAll`, `DiscardCut` | 슬라이스 입력, 결과 표시, UI |

그 밖에 평면과 편집 여부, Enter 요청, 원본 캐시의 평면 변경 감지 상태는 컨트롤러가 보관한다.

뷰는 `confirmedGeneration` 또는 표시 구역 분리 방식이 바뀌면 메시를 다시 굽고(`CutDebugView::Sync`), 그 밖의 프레임에는 정점 위치를 갱신한다.

## 5. Geometry 파이프라인 (순수 계산)

```
Geometry/Patch      패치 조각 자료구조(PatchPiece 등)와 검증, 정점 -> 노드/코너 용접 (BuildPatchPiece)
Geometry/Slice      평면으로 자르기 (SlicePatch). 내부에서 SliceCap.cpp의 루프 추출·검증과 cap 생성을 부른다
Geometry/Skinning   스킨 정점 표(Skinning)와 piece 스키닝(Apply), joint 집합 선택 규칙. piece들이 shared_ptr<const Skinning>으로 공유한다
Geometry/CutPlane   world 유한 사각 평면, 조작, 루프 사각형 검사, 평면이 지나는 뼈 선분 찾기
Geometry/CutPick    평면과 교차하는 삼각형을 찾아 삼각형마다 절단 joint 하나를 고른다 (CutPick::PickJoints)
Rendering/RenderBake  piece -> ProceduralMesh 변환(BuildRenderMesh, UpdateRenderPositions)과 디버그 튜브
```

파일은 기능 단위로 합쳐져 있다 (Patch = PatchGeometry + PatchWeld, Slice/SliceCap = PlaneSlicer + CutLoops + CapBuilder, Skinning = PieceSkinning + JointSet, CutPlane = CutPlane + SegmentPick). RenderBake는 Rendering/으로 옮겼지만 Core에 의존하지 않는다.

`PatchNode`(위치의 주인)와 `PatchCorner`(노드 + UV/법선/색)를 나눈 것은 UV 이음매를 용접하면서도 렌더 정점은 따로 두기 위해서다. 절단선의 노드는 두 조각이 공유한다.

## 6. 진입점 찾기

| 알고 싶은 것 | 시작할 함수 |
| --- | --- |
| 키가 어떤 동작이 되는가 | `ModelViewer::Update`의 `m_Cut.*` 호출 |
| 절단 모드에서 패치가 어떻게 정해지는가 | `CutController::UpdateCandidates`, `ExtractJob` |
| Enter에서 절단하는 과정 | `CutController::ConfirmAll` → `SliceWithPlane` |
| 화면에 무엇이 그려지는가 | `CutDebugView::Sync`, `Draw` |
| 절단 알고리즘 | `SlicePatch` (`Geometry/Slice.cpp`) |
| 프레임마다 piece가 움직이는 방식 | `CutController::Update`, `Skinning::Apply` |
