상태: 완료 — 현황문서 (기준 `096f8a1c`, 미커밋)

# 현재 Scene과 유한 평면 코드 지도

현재 구현은 객체 선택·TRS 편집과 유한 평면의 기하·격자 표시까지다. 실제 GPU 절단, 교차 미리보기, 유한 영역에 일부만 걸친 메시의 절단 정책은 미구현이다. 과거 CPU 패치 추출·슬라이스·cap 경로는 현행 코드에 없다.

## 실행 흐름

| 순서 | 함수 | 역할 |
| --- | --- | --- |
| 초기화 | `ModelViewer::Startup`, `CutPlaneSystem::CreatePlane` | Scene에 모델 객체와 원점의 4×4 XZ 평면을 만든다. 시작 선택은 없다. |
| 갱신 | `Scene::Update`, `CutPlaneSystem::PruneStaleObjects` | 모델 자세를 갱신하고 일반 Scene 삭제로 남은 평면 설명을 정리한다. |
| 선택 | `PickObject`, `CutPlaneSystem::Pick`, `NearestPickHit` | 모델 경계 구와 유한 직사각형의 앞쪽 교점을 월드 거리로 비교한다. |
| 편집 | `TransformGizmo`, `GameObject::SetTransform` | 선택 객체의 TRS를 편집한다. 평면 크기 모드는 로컬 X/Z만 바꾼다. |
| 스냅샷 | `CutPlaneSystem::BuildData`, `TryBuildCutPlaneData` | 평면 설명과 현재 GameObject TRS로 기하 데이터를 계산한다. |
| 표시 | `Scene::Render`, `RenderCutPlanes` | 모델을 렌더하고 메인 투명 메시 뒤·후처리 전에 평면당 테두리·격자 22개 선을 그린다. 장면 깊이를 검사하며 깊이를 쓰지 않는다. |

## 소유권과 공간 계약

| 파일 / 자료형 | 소유 데이터와 책임 |
| --- | --- |
| `ModelViewer/Scene/Scene.*`의 `Scene`, `GameObject` | Scene이 객체를 소유한다. GameObject는 TRS의 원본이며 모델 없이도 존재한다. 모델이 있으면 ModelInstance를 가진다. |
| `ModelViewer/Scene/ObjectSelection.*`, `PickHit.h` | 모델 경계 구 선택, 객체 ID와 `WorldT`, 가장 가까운 유효 후보 비교를 제공한다. |
| `ModelViewer/Scene/TransformGizmo.*`, `SceneTransform.h` | 객체 TRS 편집과 입력 처리를 담당한다. 조작·Undo 계약은 [Scene README](../../ModelViewer/Scene/README.md)를 따른다. |
| `ModelViewer/Scene/CutPlaneSystem.*` | Scene을 참조하고 ObjectId별 `CutPlaneDesc`를 소유한다. 별도 평면 TRS를 보관하지 않는다. `Find`는 삭제된 객체를 거절한다. |
| `MeshCutting/Geometry/CutPlane.*`의 `CutPlaneDesc` | 로컬 XZ, y=0 직사각형의 `Width`와 `Height`를 정의한다. |
| `CutPlaneData` | `WorldPlane`은 정규화된 `Vector4(n, d)`이며 식은 n·x+d=0이다. `WorldToPlane`은 affine 역변환, `HalfWidth`·`HalfHeight`는 로컬 반크기, `WorldCorners`는 네 월드 모서리다. |
| `ModelViewer/Scene/CutPlaneRenderer.*`, `Shaders/CutPlaneVS.hlsl`, `CutPlanePS.hlsl` | 스냅샷으로 선을 만들고 전용 line PSO로 표시한다. 절단 결과 버퍼는 생성하지 않는다. |

`TryBuildCutPlaneData`는 로컬 +Y의 역전치 법선을 사용한다. 기하 API는 가역 affine의 반사·비균일 스케일·전단을 지원하지만 Scene 편집은 양수 크기의 TRS 계약을 사용한다. 특이 변환과 비유한 크기는 거절한다.

`IntersectCutPlaneRectangle`은 정규화된 월드 ray의 거리 t를 보존한다. `ClipCoplanarSegmentToCutPlaneRectangle`은 공면 선분의 내부 구간을 원본 선분의 [0,1]로 반환한다. 둘 다 실패 시 출력 인자를 보존한다.

구현·검증 범위는 [CutPlaneDesign.md](CutPlaneDesign.md), 후속 GPU 방향은 [GPUCuttingSystemDesign.md](GPUCuttingSystemDesign.md), NDC 근거 등급은 [NDC25Reference.md](NDC25Reference.md)를 본다.
