# Scene 뷰 조작 설계: 마우스 카메라와 평면 기즈모

> 이력 문서: 아래는 과거 조작 설계다. 현재 키·마우스·기즈모 계약은 [Scene README](../../ModelViewer/Scene/README.md), 평면 구현은 [CutPlaneDesign.md](CutPlaneDesign.md)를 따른다.

> 작성: 2026-10-21 · S-1(커서와 마우스 좌표), S-2(Scene 뷰 카메라), S-3(평면 기즈모), S-4(키와 화면 정리)
>
> 목적: 절단 평면을 키보드로 움직이던 디버그 조작을 Unity Scene 뷰 방식으로 바꾼다. 카메라는 마우스와 WASD, 평면은 마우스 기즈모, 커서는 항상 보인다.

## 1. 구성

| 파일 | 역할 |
| --- | --- |
| `Core/Input.cpp`, `Core/GameInput.h` | 마우스 cooperative level 선택, 창 클라이언트 기준 커서 좌표, 클라이언트 크기 |
| `MeshCutting/Debug/SceneViewCamera.*` | Scene 뷰 카메라 (`CameraController` 상속). Core 입력을 읽는다 |
| `MeshCutting/Geometry/CutPlane.h`, `CutPlaneGizmo.cpp` | 기즈모 상태(도구, 호버, 드래그)는 `CutPlane`이 가진다. 광선, 피킹, 드래그 계산, 메시 굽기는 **Model과 Core를 모르는 순수 계산**이라 `tests/PlaneSlicerTests.cpp`가 GPU 없이 테스트한다 |
| `MeshCutting/Debug/CutDebugView.*` | 렌더링 모드, 기즈모 그리기, 화면 텍스트 |
| `MeshCutting/Rendering/ProceduralMeshRenderer.*` | `DrawOptions::depthTest` (기즈모는 깊이 버퍼 없이 그린다) |
| `ModelViewer/ModelViewer.cpp` | 입력을 읽어 카메라, 기즈모, 컨트롤러에 전달 |

프레임 순서 (`ModelViewer::Update`): 키(T, V, Ctrl+Z, Enter, 도구 키) → 카메라 입력 허용 여부(기즈모 드래그 중이면 끔) → 카메라 `Update` → 기즈모 `Update`(이번 프레임 카메라 기준, 평면이 바뀌면 `CutController::SetPlane`) → `ModelInstance::Update` → `CutController::Update`.

## 2. Core 입력 변경 (S-1)

- `GameInput::SetMouseExclusive(bool)`: 마우스 cooperative level. **기본값은 exclusive**(DirectInput이 커서를 숨기고 상대 이동량만 준다)라서 다른 앱의 동작은 그대로다. `ModelViewer::Startup`만 `false`로 바꾼다. 상대 이동량(`kAnalogMouseX/Y`), 버튼, 휠은 두 모드에서 같다.
- `GameInput::GetCursorClientPos(x, y, nx, ny)`: 창 클라이언트 픽셀 좌표와 0~1 정규화 좌표 (렌더 해상도가 아니다). 이 창이 포그라운드가 아니거나 커서가 클라이언트 영역 밖이면 `false`이고 네 값은 -1이다.
- `GameInput::GetClientSize(w, h)`: 위 좌표의 기준이 되는 창 클라이언트 크기. 최소화되어 면적이 없으면 `false`.
- 휠(`kAnalogMouseScroll`)은 원래부터 -1/0/+1 값만 준다. 바꾸지 않았다.

## 3. Scene 뷰 카메라 (S-2)

`SceneViewCamera`는 **피벗, 거리, heading, pitch**로 위치를 정한다. 피벗은 카메라 앞 `distance`만큼 떨어진 점이다.

| 조작 | 동작 |
| --- | --- |
| 오른쪽 버튼 누름 | 마우스 = 둘러보기(눈 위치 고정, 피벗은 새 방향 앞으로 다시 잡는다). W/A/S/D = 앞·왼·뒤·오른쪽, Q/E = 아래·위(world up), Shift = 3배, 휠 = 이동 속도 배율(0.05~40) |
| 가운데 버튼 드래그 | pan (피벗을 화면 평면에서 옮긴다. 한 픽셀의 world 길이는 피벗 거리에 비례) |
| Alt + 왼쪽 드래그 | 피벗 중심 궤도 회전 |
| Alt + 오른쪽 드래그 | 피벗 방향 줌 (오른쪽이나 위로 끌면 다가간다) |
| 휠 (오른쪽 버튼 없이) | 피벗 방향 줌. 거리에 0.85를 곱하므로 피벗까지의 거리에 비례해서 움직인다 |
| F | 디버그 모드에서는 평면(사각형이 다 보이는 거리), 아니면 모델에 맞춰 피벗과 거리를 정한다 |

- 이동 속도와 거리 범위는 처음 대상의 bounds 반지름에 비례한다 (이동 초당 1.5 반지름, 거리 0.02~200 반지름).
- 조작은 **버튼을 누른 순간 커서가 창 안에 있어야 시작**하고, 그 버튼을 놓으면 끝난다.
- 오른쪽 버튼을 누르고 있는 동안(`IsLooking()`)에는 W/E/R/Q가 도구 전환으로 처리되지 않는다.
- 창 크기가 바뀌면 화면비를 갱신한다 (기존 `OrbitCamera`와 같다). near/far는 `OrbitCamera`와 같은 규칙으로 정한다.
- 이전의 `OrbitCamera`와 `FlyingFPSCamera`는 `ModelViewer`에서 쓰지 않는다 (Core 코드는 그대로 남아 있다).

## 4. 평면 기즈모 (S-3)

### 4.1 도구와 핸들
| 키 | 도구 | 핸들 |
| --- | --- | --- |
| Q | 숨김 | 없음 |
| W | 이동 | **world** X/Y/Z 축 화살표 3개, 축 평면 사각형 3개(YZ, ZX, XY) |
| E | 회전 | 평면 **로컬 축** 링 3개 (U = 로컬 X, V = 로컬 Y, N = 로컬 Z = 법선) |
| R | 크기 | U축 가장자리 핸들, V축 가장자리 핸들, 가운데 균등 크기 핸들. 법선 핸들은 없다 (평면에는 두께가 없다) |

색은 X 빨강, Y 초록, Z 파랑이다 (로컬 축도 U, V, N 순서로 같은 색). 호버나 드래그 중인 핸들은 노랗고 조금 굵다. 기즈모 크기는 `GizmoUnit`으로 정한다: 화면 중앙에서 한 픽셀이 차지하는 각도 × 카메라 거리 × 100이라서 **거리가 달라도 화면에서 약 100픽셀**이다. 기즈모는 깊이 버퍼 없이 그려서 항상 메시 위에 보인다.

### 4.2 광선과 피킹
- 광선: 클라이언트 픽셀 → NDC → `ViewProj` 역행렬. MiniEngine은 reversed-Z(near = 1, far = 0)이다. far가 무한대인 투영에서도 값이 유한하도록 깊이 1(near)과 0.5 두 점으로 방향을 만든다. 행렬 규약: `XMMATRIX(matrix4)`가 DirectXMath의 행 벡터 규약(`p * viewProj`)이다.
- TAA 지터는 뷰포트 오프셋으로 넣기 때문에 카메라의 `ViewProj`에는 지터가 없다. 그래서 피킹에서 빼는 일이 필요 없다.
- 피킹: 핸들을 화면에 투영해 커서와의 픽셀 거리가 **8픽셀 이하**인 것 중 **카메라에 가장 가까운 핸들**을 고른다(깊이가 같으면 커서에 더 가까운 쪽). 사각형 핸들은 안쪽 전체가 핸들(거리 0)이다. 카메라 뒤에 걸친 선분은 건너뛴다(근평면 클리핑은 하지 않는다).

### 4.3 드래그 (모두 시작 평면 기준의 변화량이라 오차가 쌓이지 않는다)
| 핸들 | 계산 |
| --- | --- |
| 축 화살표 | 광선과 축 직선의 **최근접점 매개변수**의 차이만큼 이동. 광선이 축과 거의 평행(사잇각 sin² < 1e-6)이면 갱신하지 않는다 |
| 축 평면 사각형 | 광선과 그 평면의 **교점**의 차이만큼 이동. 평행하거나 교점이 광선 뒤면 갱신하지 않는다 |
| 링 | 축에 수직인 평면과 광선의 교점으로 시작 벡터와 현재 벡터의 부호 있는 각도(오른손 법칙)를 구해 `RotateAroundLocalAxis`. 한 바퀴를 넘겨도 이어지도록 직전 값과의 차이만 누적한다 |
| U/V 핸들 | 로컬 축 위 최근접점의 차이만큼 `halfExtents.x/y`를 바꾼다. [모델 반지름의 0.01, 3배]로 제한 |
| 가운데 핸들 | 커서가 오른쪽 위로 움직이면 커지고 왼쪽 아래로 움직이면 작아진다 (150픽셀에 e배). 두 변의 비율을 지킨 채 같은 범위로 제한 (`ScaleUniform`) |

- 드래그 중에는 카메라 조작을 막는다 (`SceneViewCamera::SetInputEnabled(false)`). 카메라가 움직이는 중이거나 Alt, 가운데·오른쪽 버튼이 눌려 있으면 기즈모는 호버도 드래그 시작도 하지 않는다.
- 커서가 창 밖으로 나가면 평면은 마지막 위치를 유지하고, 버튼을 놓으면 드래그가 끝난다. 도구를 바꾸면 드래그가 취소된다.
- 평면이 바뀔 때마다 `CutController::UpdateGizmo`가 미리보기를 다시 계산하게(`m_Preview.dirty`) 표시한다. 확정 piece는 드래그와 무관하다.

### 4.4 깊이 없는 PSO 주의
MiniEngine의 `GraphicsPSO::Finalize`는 `DepthEnable`이 DSV 포맷 유무와 반대이면 `ASSERT`로 멈춘다(Debug). 그래서 `DepthStateDisabled`를 쓰는 기즈모 PSO는 **깊이 포맷을 UNKNOWN**으로 만들고, 그릴 때 **깊이 버퍼를 묶지 않는다** (`DrawOptions::depthTest = false`).

## 5. 키

| 키 | 기능 |
| --- | --- |
| Space | 애니메이션 정지/재생 |
| T | 디버그 모드 켜기/끄기 (평면, 기즈모, 후보 표시, 웨이트 뷰). 처음 켤 때 평면은 모델 bounds 중심, 법선 world +Y, 반 크기 = 모델 반지름 × 0.5 |
| V | 렌더링 모드 순환: Solid → Solid + Wire → Wire → Debug |
| Enter | 절단 (디버그 모드에서, 기존 확정 절단) |
| Ctrl + Z | 절단 되돌리기 (기존 Backspace. Core 튜닝 메뉴와의 충돌이 없어진다) |
| W / E / R / Q | 기즈모 도구 (오른쪽 버튼을 누르지 않았을 때) |
| 오른쪽 버튼 + W/A/S/D/Q/E, Shift | 카메라 이동 |
| F, Alt | 카메라 맞추기, 궤도/줌 수정 키 |

Enter는 Core의 튜닝 메뉴(열려 있을 때 Bang)와 프로파일러(항목 토글)도 쓰므로 그 화면이 열려 있을 때는 같이 동작한다.

## 6. 렌더링 모드 (V)

| 모드 | 원본 모델 | 잘린 조각 |
| --- | --- | --- |
| Solid | 솔리드 | 솔리드 |
| Solid + Wire | 솔리드 + 와이어 (둘째 `MeshSorter`로 와이어 패스를 한 번 더) | 솔리드 + 어두운 선 |
| Wire | 와이어 (기존 V) | 밝은 선 |
| Debug | 와이어 | 밝은 선 + 구역(section)별 색 + 경계 튜브 |

- 후면 컬링은 끄고(양면), cap은 항상 만든다.
- 원본 와이어의 깊이 바이어스: 모델의 와이어 PSO(`Model/Renderer.cpp`)에 조각과 같은 값(`DepthBias` 200, `SlopeScaledDepthBias` 1.5)을 넣었다. 솔리드와 선의 깊이가 같아서 선이 깊이 테스트에서 떨어지는 것을 막는다.
- Solid + Wire의 원본 선은 웨이트 뷰 셰이더로 그린다. 선택한 joint가 없으면 `influence`가 초기화되지 않으므로 팔레트의 마지막 joint를 지정해, 그 joint에 가중치가 없는 정점은 어두운 회색이 되게 한다. 선이 얇고 TAA 때문에 옅어 보인다.

## 7. 웨이트 뷰

디버그 모드에서 **첫 번째 절단 joint**(`CutJoints().front()`)를 보여준다. 절단 joint가 없으면 꺼진다. 셰이더가 joint 하나만 받기 때문이다. 여러 joint를 한꺼번에 칠하려면 상수와 셰이더(`Model/`) 변경이 필요해서 하지 않았다. Tab 순환은 없어졌다.

## 8. 없어진 것
- 키: B, J, K, I, H(본 디버그 선택과 웨이트 토글), C, X(패치 단독 복사), Home/End/Delete/PgDn/Insert/PgUp, `[ ] , . ; '`, `- =`, R(평면 초기화), M, U, Z, G, L, N, Tab, Backspace.
- `ModelViewer`: `BoneSelection`, `m_BoneDebugEnabled`, `m_WeightDebugEnabled`, `m_WireframeEnabled`, `m_CameraController`.
- `CutController`: `SetPatch`, `ClearPatch`, `CycleWeightJoint`, `ToggleCap`, `ResetPlane`, `TranslatePlane`, `MovePlaneAlongNormal`, `RotatePlane`, `ScalePlane`, `ExtractedMesh()`, `Weld()`, `PatchVersion()`, `CapEnabled()`, `CurrentPlane()`, 그리고 `m_CapEnabled`, `PatchState::version`, `CandidateState::weightCycle`.
- `CutDebugView`: 패치 메시와 패치 경계 튜브, `ToggleBoundary`, `ToggleBackCull`, `ToggleSectionTint`, `ToggleOverlay`, `CycleWireMode`, `WireMode`, 패치/weld 관련 UI.
- `RenderBake::BakePlaneDebug`는 P-1에서 `BakeCutPlaneDebug`로 바뀌었고, K-1에서 `CutDebugView`의 `BakePlaneDebug`로 옮겼다.
