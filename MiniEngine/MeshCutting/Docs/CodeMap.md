# 코드 지도: 실행 흐름과 상태의 주인

> 목적: 코드를 처음 열었을 때 "누가 무엇을 부르고, 상태는 어디에 있는가"를 빨리 찾기 위한 한 장 요약이다.
> 줄 번호는 적지 않는다(자주 바뀐다). 함수 이름으로 찾는다. 알고리즘 설명은 `PlaneSlicingDesign.md`, `PlaneFirstCutDesign.md`, `CutDebugDesign.md`.

## 1. 계층

```
ModelViewer.cpp        입력을 읽어 컨트롤러에 전달하고, 프레임 갱신과 그리기를 호출한다
   │
   ├─ Runtime/CutController      후보 패치, 미리보기, 확정 절단, 스키닝을 조율한다 (그리기 없음)
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
| 1 | `ModelViewer::Update` (키 처리) | T, M, R, Enter, Backspace, 이동·회전·크기 키를 `CutController`의 함수로 바꿔 부른다. 컨트롤러는 키를 모른다 |
| 2 | `ModelInstance::Update` | 이번 프레임 포즈 계산 |
| 3 | `CutController::Update` | 아래 3절 |
| 4 | `ModelViewer::RenderProceduralMeshes` → `CutDebugView::Draw` | `Sync`로 메시를 갱신하고 그린다 |
| 5 | `ModelViewer::RenderUI` → `CutDebugView::DrawUI` | 상태 줄 출력 |
| 종료 | `ModelViewer::Cleanup` → `CutDebugView::Release` | GPU 버퍼 반납 |

## 3. CutController::Update (포즈 갱신 뒤 한 번)

```
절단 모드(T)이면 UpdateCandidate
    CutPick::PickJoints(평면과 교차하는 삼각형이 고른 절단 joint, 등록 후 첫 편집 프레임에 만든 원본 캐시 사용). PickSegments는 뼈 선분 tube 표시용
    집합이 직전 시도와 다르면 ExtractBonePatch(집합) → RebuildPatchPiece(BuildPatchPiece)
패치가 있으면
    BuildSkinMatrices → Skinning::Apply(패치를 이번 포즈로)
    Enter 요청이 있으면 ConfirmCut, 아니면 포즈/평면이 바뀌었을 때 RunPreview
        둘 다 SliceWithPlane = SlicePatch(무한 평면) + CutPlane::ContainsLoops(사각형 검사)
확정 piece가 있고 이번 프레임에 새로 자르지 않았으면
    확정 piece의 스킨 슬롯(자른 시점 패치의 메시)으로 Skinning::Apply
```

실패한 절단은 이전 확정 piece를 바꾸지 않는다. 후보 패치가 바뀌거나 사라져도 확정 piece는 남는다.

## 4. 상태의 주인 (CutController 안의 작은 구조체)

| 구조체 | 담는 것 | 쓰는 곳 | 읽는 곳 |
| --- | --- | --- | --- |
| `PatchState` | 패치 메시, 원본 정보, 용접한 `PatchPiece`, 상태, `version` | `SetPatch`(C), `UpdateCandidate`, `DropPatch` | 슬라이스 입력, 뷰의 패치 표시 |
| `CandidateState` | 평면이 지나는 선분과 joint 집합, 추출 시도 캐시, 추출 시간 | `UpdateCandidate` | UI, 뷰의 선분 표시, 웨이트 뷰 |
| `PreviewState` | 마지막 dry-run 결과와 `generation`, 갱신 필요 표시 | `RunPreview`, `ClearPreview` | 평면 색, 예상 루프 |
| `ConfirmedCut` | 확정 piece 둘, 자른 시점 스킨 슬롯, 마지막 시도의 보고, `generation` | `ConfirmCut`, `ClearSlice` | 뷰의 piece 표시, UI |

그 밖에 평면(`m_Plane`, 편집 중 여부, 크기 기준 반지름)과 스키닝 계측(`m_SkinMs` 등)이 컨트롤러에 직접 있다.

뷰는 `version`/`generation`이 바뀔 때만 메시를 다시 굽고(`CutDebugView::Sync`), 그 밖의 프레임에는 정점 위치만 갱신한다.

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
| 절단 모드에서 패치가 어떻게 정해지는가 | `CutController::UpdateCandidate` |
| 미리보기와 확정이 어떻게 다른가 | `CutController::RunPreview`, `ConfirmCut` (둘 다 `SliceWithPlane`) |
| 화면에 무엇이 그려지는가 | `CutDebugView::Sync`, `Draw` |
| 절단 알고리즘 | `SlicePatch` (`Geometry/Slice.cpp`) |
| 프레임마다 piece가 움직이는 방식 | `CutController::Update`, `Skinning::Apply` |
