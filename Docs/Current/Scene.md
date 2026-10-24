# ModelViewer GameObject와 Gizmo

`MiniEngine/ModelViewer`를 작업 디렉터리로 실행하면 Cube와 Bunny가 떨어져 배치된다.
원점의 4×4 청록색 격자는 모델 없는 Cut Plane 객체이며 시작 시 선택된 객체는 없다.
평면 내부를 클릭하면 선택되고 테두리가 노란색으로 바뀐다. 이동·회전·TRS Undo는 모델과 같다.
평면의 R 크기 모드는 X/Z만 표시하며 중앙 핸들도 XZ만 변경하고 Y는 보존한다.
평면 선은 장면 깊이에 가려지고 깊이에는 쓰지 않으며, 교차 미리보기나 실제 절단은 수행하지 않는다.
기존 `-model <경로>` 인수는 단일 모델을 열고 모든 애니메이션을 반복 재생한다.
진단용 `-view 0|1|2`는 시작 표시 모드를 Solid/Wireframe/Vertices로 선택한다.
생략하거나 범위 밖이면 기본 Solid이며, 실행 중 `V` 순환은 그대로 유지된다.
예: `../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -view 2`.
유효한 `-view` 실행만 시작 모드·PSO 준비·첫 장면 GPU 작업 완료를 debugger에 한 번씩 기록한다.
마지막 표시는 첫 장면 명령 제출 후 fence 완료를 기다린 결과이며 화면의 시각적 정확성 검증은 아니다.

## 조작

| 입력 | 동작 |
|---|---|
| 왼쪽 클릭 | 객체 선택, 빈 공간에서는 선택 해제 |
| `Q` | View: Gizmo 숨김·조작 해제, 클릭 선택 유지 |
| `W` | 이동: RGB 축 또는 XY/YZ/ZX 사각형 드래그 |
| `E` | 회전: RGB 링 드래그 |
| `R` | Local 크기: X/Y/Z 축 방향으로 드래그하면 해당 성분만 변경; 중앙 사각형은 위/아래로 세 성분 배율 변경 |
| `L` | 이동·회전의 World / Local 축 전환 (크기는 항상 Local) |
| `V` | Solid → Wireframe → Vertices → Solid 순환 (현재 모드 overlay 표시) |
| 드래그 중 `Esc` | 시작 Transform으로 복구 |
| `Ctrl+Z` | 마지막 완료 드래그 한 번 취소; 최근 128개 보관 |
| 오른쪽 버튼 유지 + 마우스 | 카메라 회전 |
| 오른쪽 버튼 유지 + `WASD` / `QE` | 카메라 이동 / 상승·하강 |

드래그 중에는 카메라와 Gizmo 도구 모드 전환을 차단한다. V 표시 모드는 전환할 수 있다. 핸들이 객체 선택보다 우선한다.
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
- `GetTransform()` / `SetTransform()`은 `SceneTransform`의 위치·Quaternion·양수 XYZ 크기를 다룬다. 각 성분은 `0.0001`~`1e8`이며 범위를 벗어난 값과 NaN/Inf를 거부하고 회전을 정규화한다.
- 원본 `shared_ptr<const Model>`은 공유하고 객체별 TRS를 `R*S` 기저의 `AffineTransform`으로 변환하여 `ModelInstance` locator에 반영한다. 매 프레임 행렬 분해나 편집 TRS의 전단은 없다.
- `ModelInstance::Update`는 모델 공간 pose와 joint palette를 계산한 뒤 렌더·컬링 행렬에 locator를 한 번 적용한다.

`Scene::CreateObject`, `FindObject`, `DeleteObject`, `Clear`로 객체를 관리한다.
매 프레임 `Scene::Update` 후 기본·그림자 pass에서 각각 `Scene::Render`를 호출한다.
`TransformGizmo`는 viewport 하나와 Scene 하나를 대상으로 입력·overlay를 처리한다.

## 현재 범위

표시 모드의 입력·PSO·깊이·알파·검증 계약은
[RenderModes.md](RenderModes.md)를 따른다.

선택은 모델의 CPU 경계 구와 가장 가까운 교차점을 이용한다. 삼각형 단위 선택이나
애니메이션에 맞춘 경계 재계산은 없으므로 구가 겹치거나 빈 공간이 많은 모델에서는 근사 선택이다.
Gizmo는 깊이 검사 없는 overlay이며 축과 카메라가 거의 나란하면 해당 축/회전 링 조작을 제한한다.
크기는 각 축 `0.0001`부터 `1e8`까지의 양수다. 음수·영 크기, 계층 편집, Redo,
장면 저장, 물리, GPU 절단은 구현하지 않는다.

현재 velocity 셰이더는 카메라 이동만 반영하고 객체 Transform·스킨 애니메이션의 이전 위치는 반영하지 않는다.
따라서 ModelViewer는 TAA를 끄고 FXAA를 사용하며 모션 블러도 끈다. TAA를 다시 켜면 움직이는 객체에
이전 프레임 잔상이 재발할 수 있다. 객체별 이전 Transform·스킨 pose를 사용하는 velocity pass는 아직 없다.

후속 GPU 절단은 `GameObject::GetTransform()`을 객체 좌표와 월드 좌표 사이의 계약으로 사용해야 한다.
절단 평면과 입력 정점은 같은 공간으로 변환한 뒤 교차 계산해야 한다. 공유 원본
`Model::m_DataBuffer`는 수정하지 않고, 절단 결과 GPU 버퍼는 객체별로 소유하도록 연결한다.
현재 구현에는 절단 결과 버퍼 API가 없다.

## CPU 검증

`MiniEngine/ModelViewer/Scene/tests/SceneMathTests.cpp`는 실제 `GizmoMath`, `TransformHistory`, `EvaluateModelPose`를 호출한다.
표준/역방향/무한 역방향 Z의 투영·레이 왕복, 축/평면 드래그, 회전 부호·π 경계,
양수 크기, Undo와 삭제된 대상, DFS 부모 복원 및 skeletonRoot 유/무의 locator 단일 적용을 검사한다.
VS Developer PowerShell에서 파일 머리말의 명령을 사용하고 `/Fo`, `/Fe`는 저장소 밖으로 지정한다.
GPU 렌더링과 실제 마우스 조작은 별도 런타임 확인이 필요하다.

## 비균일 변환과 회전 검토

크기 핸들은 항상 객체 로컬 축이다. World 축으로 비균일 신장을 적용하면 전단이 필요할 수 있어
L 전환은 이동·회전에만 적용한다. overlay도 크기 모드에서는 Local을 표시한다.
축 드래그는 시작 시 고정한 축을 포함하는 ray-plane 교차의 축 방향 이동량을 사용한다.
중앙 핸들은 기존 위/아래 픽셀 이동으로 모든 성분에 같은 배율을 적용하고 각 성분을 개별 제한한다.
Esc는 시작 TRS 전체를 복구하고 Undo도 XYZ 크기를 함께 저장한다.

표시·선택·드래그의 축은 공통 `GizmoMath::Basis`에서 구한다. World는 고정 XYZ,
Local은 객체 Quaternion만 사용한다. 드래그 중에는 시작 기저를 표시에도 유지하여
로컬 회전 도중 표시 링과 실제 드래그 평면이 어긋나는 중복 계산을 제거했다.
카메라로 축을 정의하는 경로는 확인하지 못했다. 카메라 이동에 따라 링의 원근 투영과
보이는 타원 모양은 달라진다. 회전 느낌에 대한 실제 마우스·GPU 재현은 미확인이며,
링을 화면에 고정하지 않는다. 거의 옆으로 보이는 링은 기존 조작 제한을 유지한다.

선택 구 중심은 전체 affine으로 변환한다. 반지름은 `AffineRadiusScale`의 신장 상한을 쓴다.
이는 TRS에서 최대 축 크기이고, locator와 내부 회전 합성으로 생기는 전단에서도 보수적이다.
메시 컬링·정렬에도 같은 도우미를 써서 극단적인 비균일 크기에서 구를 과소평가하지 않는다.
전단에서는 구가 커져 선택·컬링이 느슨해질 수 있다. 원본 경계가 애니메이션을 감싸는지는 기존 한계다.
`Resize`는 위치와 축 비율을 유지하며 경계 반지름을 맞춘다. 상자는 기존 affine×OrientedBox를 사용한다.
정적·스킨 메시 모두 기존 WorldIT 역전치와 모델 공간 joint 법선 역전치를 유지하며,
locator는 mesh World에 한 번만 곱한다. Solid/Wireframe/Vertices 표시 정책도 동일하다.

CPU 회귀 검사는 공통 TRS·축 기저·축별 배율·히스토리·경계 도우미를 호출한다.
비균일 점/법선 변환, 합성 전단 구, XYZ 독립/중앙 배율, 양수 상하한·무효값,
비균일 Undo, 카메라 위치·회전 변화의 기저 불변성과 투영 차이,
두 skeletonRoot 경우의 비균일 locator 단일 적용을 포함한다.
