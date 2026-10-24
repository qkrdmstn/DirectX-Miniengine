# ModelViewer GameObject와 Gizmo

`MiniEngine/ModelViewer`를 작업 디렉터리로 실행하면 Cube와 Bunny가 떨어져 배치된다.
기존 `-model <경로>` 인수는 단일 모델을 열고 모든 애니메이션을 반복 재생한다.

## 조작

| 입력 | 동작 |
|---|---|
| 왼쪽 클릭 | 객체 선택, 빈 공간에서는 선택 해제 |
| `Q` | View: Gizmo 숨김·조작 해제, 클릭 선택 유지 |
| `W` | 이동: RGB 축 또는 XY/YZ/ZX 사각형 드래그 |
| `E` | 회전: RGB 링 드래그 |
| `R` | 균일 크기: 중앙 사각형 또는 축을 잡고 위/아래로 드래그 |
| `L` | World / Local 축 전환 |
| 드래그 중 `Esc` | 시작 Transform으로 복구 |
| `Ctrl+Z` | 마지막 완료 드래그 한 번 취소; 최근 128개 보관 |
| 오른쪽 버튼 유지 + 마우스 | 카메라 회전 |
| 오른쪽 버튼 유지 + `WASD` / `QE` | 카메라 이동 / 상승·하강 |

드래그 중에는 카메라와 모드 전환을 차단한다. 핸들이 객체 선택보다 우선한다.
오른쪽 버튼으로 카메라를 조작하는 동안에는 Q/W/E/R로 도구를 바꾸지 않는다.
ModelViewer는 비독점 마우스 입력을 사용해 선택과 카메라 조작 중 시스템 커서를 표시한다.
이동 축 끝은 화살표, 크기 축 끝은 사각형으로 구분한다.
`Q` View는 Gizmo를 숨기는 도구이며 왼쪽 드래그 카메라 pan은 제공하지 않는다.
포커스를 잃거나 커서가 클라이언트 영역을 벗어나거나 대상이 삭제되면 드래그를 취소한다.
취소와 변환 없는 클릭은 Undo에 넣지 않는다. 삭제된 대상의 Undo는 건너뛴다.
드래그 밖에서 `Esc`를 누르면 기존 ModelViewer 동작대로 종료한다.

## 소유권과 변환

- `Viewer::Scene`이 `unique_ptr<GameObject>`를 소유한다. 객체 주소는 삭제 전까지 유지된다.
- `ObjectId`는 Scene 안에서 증가하는 `uint64_t`이고 `0`은 invalid다. 삭제·`Clear()` 후 재사용하지 않는다.
- `GameObject`는 모델 없이도 존재하며, 모델이 있으면 별도 `ModelInstance`를 소유한다.
- `GetTransform()` / `SetTransform()`은 위치·회전·양수 균일 크기를 다룬다. 잘못된 값은 거부하고 회전을 정규화한다.
- 원본 `shared_ptr<const Model>`은 공유하고 객체별 Transform은 `ModelInstance` locator에 반영한다.
- `ModelInstance::Update`는 모델 공간 pose와 joint palette를 계산한 뒤 렌더·컬링 행렬에 locator를 한 번 적용한다.

`Scene::CreateObject`, `FindObject`, `DeleteObject`, `Clear`로 객체를 관리한다.
매 프레임 `Scene::Update` 후 기본·그림자 pass에서 각각 `Scene::Render`를 호출한다.
`TransformGizmo`는 viewport 하나와 Scene 하나를 대상으로 입력·overlay를 처리한다.

## 현재 범위

선택은 모델의 CPU 경계 구와 가장 가까운 교차점을 이용한다. 삼각형 단위 선택이나
애니메이션에 맞춘 경계 재계산은 없으므로 구가 겹치거나 빈 공간이 많은 모델에서는 근사 선택이다.
Gizmo는 깊이 검사 없는 overlay이며 축과 카메라가 거의 나란하면 해당 축/회전 링 조작을 제한한다.
크기는 `0.0001`부터 `1e8`까지의 양수 균일 크기다. 계층 편집, 비균일 크기, Redo,
장면 저장, 물리, GPU 절단은 구현하지 않는다.

후속 GPU 절단은 `GameObject::GetTransform()`을 객체 좌표와 월드 좌표 사이의 계약으로 사용해야 한다.
절단 평면과 입력 정점은 같은 공간으로 변환한 뒤 교차 계산해야 한다. 공유 원본
`Model::m_DataBuffer`는 수정하지 않고, 절단 결과 GPU 버퍼는 객체별로 소유하도록 연결한다.
현재 구현에는 절단 결과 버퍼 API가 없다.

## CPU 검증

`tests/SceneMathTests.cpp`는 실제 `GizmoMath`, `TransformHistory`, `EvaluateModelPose`를 호출한다.
표준/역방향/무한 역방향 Z의 투영·레이 왕복, 축/평면 드래그, 회전 부호·π 경계,
양수 크기, Undo와 삭제된 대상, DFS 부모 복원 및 skeletonRoot 유/무의 locator 단일 적용을 검사한다.
VS Developer PowerShell에서 파일 머리말의 명령을 사용하고 `/Fo`, `/Fe`는 저장소 밖으로 지정한다.
GPU 렌더링과 실제 마우스 조작은 별도 런타임 확인이 필요하다.
