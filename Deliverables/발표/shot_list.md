# 스크린샷 목록 (deck.html이 참조하는 파일)

저장 위치: `Deliverables/발표/shots/<이름>.png`. 파일을 넣으면 deck.html의 회색 자리표시가 자동으로 그림으로 바뀐다.
PDF 다시 만들기는 맨 아래 참고.

키와 동작은 2026-10-22 작업 트리의 코드를 읽고 적었다 (`ModelViewer::Update`, `SceneViewCamera::Update`,
`CutDebugView::Draw`/`DrawUI`, `CutController::PlacePlane`/`ConfirmAll`). 다른 세션이 코드를 고치는 중이므로,
촬영 전에 화면 왼쪽 위 도움말 두 줄(`Space: animation  T: debug mode ...`)이 아래 표와 같은지 먼저 확인한다.

## 장면 기본 상태

- 실행: `MiniEngine/ModelViewer`에서 `../Build/x64/Release/Output/ModelViewer/ModelViewer.exe` (Release 권장. 화면 구성은 Debug와 같다)
- 장면: 가운데 Xbot(걷기 애니메이션 재생 중). 캐릭터의 +X 쪽에 `cube.glb`(중심이 캐릭터 반지름 R의 0.9배 거리),
  더 멀리 `bunny.glb`(1.75R). 두 정적 모델 크기는 반지름 0.6R, 중심 높이는 캐릭터 중심과 같다.
- 시작 카메라는 Xbot을 화면에 맞춘 상태라 정적 모델이 잘려 보일 수 있다. 휠로 조금 물러나거나 MMB 드래그로 옮긴다.

### 키 요약

| 키 | 동작 |
| --- | --- |
| T | 디버그 모드 켜기/끄기. 처음 켜면 평면이 **캐릭터 중심 높이에 수평**으로, 반 크기 0.5R로 생긴다 |
| W / E / R / Q | 기즈모: 이동 / 회전 / 크기 / 숨김 (디버그 모드에서만, 오른쪽 버튼을 누르지 않은 상태) |
| Enter | 평면 아래 후보 작업을 **전부** 확정 절단. 다시 누르면 이전 결과를 지우고 새로 자른다 |
| Ctrl+Z | 확정 결과 버리기, 원본 다시 보이기 |
| V | 렌더 모드 순환: Solid → Solid + Wire → Wire → Debug → Solid |
| Space | 애니메이션 일시정지/재개 |
| F | 디버그 모드면 평면, 아니면 Xbot에 카메라 맞추기 |
| Alt+LMB 드래그 | 피벗 둘레 궤도 회전 |
| MMB 드래그 | 이동(pan) |
| 휠 | 줌 |
| RMB + WASD/QE | 비행 (Shift 빠르게, RMB 누른 채 휠로 속도) |

### 주의

- 평면이 Xbot이나 cube에도 걸쳐 있으면 그 대상도 후보가 되고 Enter 때 같이 잘린다. 이번 발표는 스태틱 절단만 보여 주므로
  **후보 목록에 bunny 한 줄만** 남게 평면을 놓는다. 후보 목록은 왼쪽 위 `#1 [Models/bunny.glb] mesh 0  crossed N tris` 형식이다.
- 평면은 유한 사각형이다. 단면 루프 전체가 사각형 안에 있어야 확정된다. 안 되면 목록 줄에 `(not under the plane)`이 붙는다 → R(크기)로 키운다.
- 평면 테두리가 **청록**이면 후보가 있다는 뜻, **회색**이면 교차하는 삼각형이 없다는 뜻이다.
- 확정된 두 조각은 **원본 자리**에 그려지고, positive는 +법선, negative는 −법선으로 대상 반지름의 12%(`kSeparationRatio`)씩 벌어진다(디버그 배치, `4f4bfb0`).
  원본 bunny는 숨겨진다.
- 왼쪽 위 계측 텍스트(ms 값)는 측정 결과로 발표하지 않으므로, 촬영본에서 잘라 내거나 가린다.

## 장면별 목록

### 1. `demo1_plane.png` — 평면 배치 (12쪽 ①)

보여 줄 것: bunny를 비스듬히 지나는 청록 평면과 기즈모. 원본은 아직 그대로.

1. Space로 Xbot 일시정지 (화면이 덜 흔들린다)
2. T → 디버그 모드 ON. 평면이 Xbot 허리 높이에 수평으로 생기고 기즈모 도구는 Move
3. W(Move 확인) → 기즈모의 X축 화살표를 끌어 평면 중심을 bunny 중심까지 +X로 옮긴다 (약 1.75R)
4. 후보 목록에 bunny만 남았는지 확인. Xbot이나 cube 줄이 남으면 조금 더 +X로 옮긴다
5. E → 회전 링 하나를 끌어 평면을 약 25~35° 기울인다 (단면이 타원으로 보이게)
6. R → 크기 핸들을 끌어 평면이 bunny 단면을 넉넉히 덮게 키운다. 목록에 `(not under the plane)`이 없어야 한다
7. F → 카메라가 평면에 맞춰진다. Alt+LMB로 위쪽 앞 45° 정도의 3/4 시점, 휠로 bunny가 화면 높이의 60% 정도가 되게
8. 촬영. 렌더 모드는 Solid

### 2. `demo2_solid.png` — 절단 결과, Solid (12쪽 ②)

보여 줄 것: 원본 bunny 대신 그 자리에서 법선 방향으로 벌어진 두 조각과 단면(cap).

1. 1번 상태에서 Enter
2. 원본 bunny가 숨겨지고 같은 자리에 두 조각이 법선 방향으로 벌어져 생긴다. 목록 줄에 `cut Ok`가 보이는지 확인
3. T → 디버그 모드 OFF (평면·기즈모 숨김. 확정 조각은 남는다)
4. MMB 드래그로 조각 쪽으로 옮기고, Alt+LMB로 **단면이 정면에 가깝게** 보이는 각도로 돌린다 (평면 법선 쪽에서 비스듬히)
5. 렌더 모드 Solid (V를 눌렀다면 Solid로 돌아올 때까지 V)
6. 촬영

### 3. `demo3_wire.png` — Solid + Wire (12쪽 ③)

보여 줄 것: 잘린 경계에서 삼각형이 분할된 모양과 cap 삼각분할.

1. 2번 상태에서 V 한 번 → `Render: Solid + Wire`
2. 휠로 단면 가까이 다가가 cap과 그 둘레가 화면 대부분을 차지하게
3. 촬영
4. (선택) V 두 번 더 → `Debug`: 작업 색 와이어, 경계 튜브, cap이 별도 섹션으로 표시. 쓸 만하면 `demo3_debug.png`로 따로 저장 (지금 덱에는 자리 없음)

### 4. `summary_bunny_cut.png` — 이번 주 결과 대표 그림 (8쪽)

보여 줄 것: 한 장으로 "스태틱 메시를 평면으로 잘랐다"가 보이는 그림. 2번과 같은 결과의 더 넓은 시점.

1. 2번 상태(Solid, 디버그 모드 OFF)
2. 휠로 물러나 Xbot 일부(왼쪽), cube, 두 조각이 함께 보이게. 조각이 가운데
3. 촬영. 2번 사진을 그대로 써도 된다

## 요청하면 추가로 찍을 수 있는 것 (덱에는 아직 자리 없음)

- `extra_cube_cut.png`: cube를 대각선으로 자른 결과. 평평한 면이라 분할·cap 모양이 가장 읽기 쉽다.
  1번의 3단계에서 평면 중심을 cube(0.9R)로 옮기고 Xbot이 후보에서 빠지는지 확인한 뒤 같은 순서
- `extra_undo.png`: Ctrl+Z 뒤 원본이 다시 보이는 모습 (원본 대체가 되돌려짐을 보여 줄 때)

## PDF 다시 만들기

스크린샷을 넣은 뒤 저장소 루트에서 (새 패키지 없이 설치된 Edge 사용):

```bash
"/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" --headless=new --disable-gpu --no-pdf-header-footer --virtual-time-budget=4000 --print-to-pdf="C:/Users/Pes/Desktop/DirectX-Miniengine/Deliverables/발표/deck.pdf" "file:///C:/Users/Pes/Desktop/DirectX-Miniengine/Deliverables/발표/deck.html"
```

## 촬영 기록 (2026-10-22, `4f4bfb0` Release)

- 4장 모두 계획대로 bunny 장면으로 촬영. 대체한 장면 없음. 후보 목록에 bunny만 남은 상태에서 Enter (`cut Ok`, loops 1).
- 평면: X축 화살표로 bunny까지 이동 → F → Rotate 초록 링으로 약 30° 기울임. 크기 조정 없이 루프가 사각형 안에 들어왔다.
- 조각 셰이더가 조명을 쓰지 않아 Solid의 조각은 단색 실루엣으로 보인다. 단면 구조는 Solid + Wire 장면(demo3)에서 보인다.
- 모든 그림은 창 영역을 잘라 왼쪽 위 계측 텍스트·작업표시줄·알림 창을 뺐다.
