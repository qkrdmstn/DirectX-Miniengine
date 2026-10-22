상태: 완료 — ProceduralMesh 통합 구현 및 CPU·Debug/Release 빌드 검증 완료 (`cb1c12a`).

# 코드 지도: 하나의 메시 객체를 따라가기

## 1. 먼저 읽을 큰 흐름

원본 모델의 필요한 범위를 ProceduralMesh로 생성한다. 같은 객체를 현재 자세로 옮겨 절단하고,
성공한 두 ProceduralMesh를 저장한다. 이후 프레임은 각 메시의 자세를 갱신하고 그대로 그린다.
WeldInput과 PatchPiece라는 별도 작업 메시 타입은 없다.

| 순서 | 호출 위치 / 함수 | 입출력과 역할 |
| --- | --- | --- |
| 등록 | ModelViewer::Startup, AddStaticInstance → CutController::RegisterInstance | 여러 ModelInstance를 등록 |
| 프레임 | ModelViewer::Update → ModelInstance::Update → CutController::Update | 원본 포즈 계산 후 절단 시스템 갱신 |
| 선택 | CutController::UpdateCandidates → CutPick::PickJoints / PickStaticMeshes | 실제 메시와 유한 사각 평면의 교차로 작업 key 생성. 뼈 선분은 표시용 |
| 생성 (Enter) | ConfirmJob → ExtractJob → ExtractBonePatch / CopyProceduralMeshFromStaticMesh | Model + instance → ProceduralMesh + MeshSource |
| 생성 내부 | ProceduralMesh::Build → BuildMeshGeometry → SetTopology | 원본 입력을 한 번 용접하여 메시의 기하와 렌더 캐시 구성 |
| 현재 자세 | SkinPatch → BuildJobMatrices → ProceduralMesh::UpdatePose | 스킨 또는 강체 행렬로 현재 위치 갱신 |
| 절단 | SliceWithPlane → SliceProceduralMesh | ProceduralMesh 하나 → 같은 타입의 양·음쪽 메시 |
| 범위 승인 | CutPlane::ContainsLoops | 내부 평면 분할 후 루프 전체가 사각형 안인지 검사. 밖이면 확정하지 않음 |
| 확정 | ConfirmJob → UpdateSkinMask, UpdateBrokenJoints | 원본 숨김·디버그 분리, 성공한 두 메시만 저장 |
| 후속 프레임 | SkinJobs → BuildPieceMatrices → ProceduralMesh::UpdatePose | 양쪽에 맞는 행렬로 기하·렌더 위치·bounds 함께 갱신 |
| 표시 | RenderScene → CutDebugView::Draw → ProceduralMeshRenderer::Draw | 뷰는 확정 메시를 그대로 그리며 별도 표면 메시를 만들지 않음 |

다음 Enter는 이전 확정을 되돌린 뒤 원본에서 다시 생성한다. 누적 재절단 기능을 추가한 작업은 아니다.
이미 분리된 joint가 있으면 한 프레임은 복원만 하고, 다음 Update에서 원래 자세로 다시 생성한다.

## 2. 데이터의 주인

| 파일 | 클래스 / 자료형 | 보관하는 것 |
| --- | --- | --- |
| ProceduralMesh.h | ProceduralMesh | 기하, 스킨 표 연결, 렌더 캐시, bounds, 변경 버전 |
| ProceduralMesh.h | MeshGeometry | 메시 내부의 실제 기하 저장 레코드. 별도 런타임 메시나 변환 단계가 아님 |
| ProceduralMesh.h | MeshVertex / MeshVertexOrigin | 위치와 원본 스킨 정점 조합 |
| ProceduralMesh.h | MeshCorner / MeshFace | 면별 속성과 Corner 세 개의 연결 |
| ProceduralMesh.h | MeshEdgeTag / MeshCutRecord | 두 Vertex 사이 경계 태그와 단면 기록 |
| ProceduralMesh.h | MeshRenderVertex / ProceduralMeshData | GPU에 보낼 렌더 형식. 원본 import에서도 임시 입력으로 사용 |
| ProceduralMesh.h | MeshVertexBinding | import 시 바인드/로컬 위치·meshSlot·joint/weight. 생성 뒤 const Skinning 표로 통합 |
| Geometry/Skinning.h | Skinning / SkinVertex | 잘린 양쪽이 공유하는 원본 스킨 정점 표 |
| BonePatch.h | MeshSource / SourceMesh·Vertex·Triangle | 원본 메시·정점·삼각형 추적, 마스크 판정, 분리 순간 기준. 위치·법선·스킨 정점 배열을 복제 보관하지 않음 |
| Runtime/CutJob.h | CutJob | 후보 key, 원본 출처, 확정 메시 두 개, 분리 스냅샷, 상태·계측. 미절단 메시 필드 없음 |
| Runtime/CutJob.h | CutTargetSource / CutStaticMesh | 삼각형 선택용 캐시. 작업의 절단 메시와 목적이 다름 |

두 조각의 Vertex/Corner/Face 배열은 각자 소유하고 const Skinning 표만 공유한다.
뷰는 확정 메시와 같은 shared_ptr를 보관하여 GPU 등록 해제까지 주소와 수명을 유지한다.

## 3. 파일 역할

| 파일 묶음 | 하는 일 |
| --- | --- |
| BonePatch | 스킨 원본의 영향 범위 import, 스킨 행렬, 뼈대 읽기 |
| StaticMeshCopy | 스킨 없는 원본 메시 import, 메시 world 행렬 |
| ProceduralMesh | 공통 메시 API와 기하·렌더 캐시 소유. SetTopology / UpdatePose / SetSectionSplit |
| Geometry/MeshGeometry | 기하 검증·용접·면적·에지 사용 계산. 자료구조 정의는 ProceduralMesh.h |
| Geometry/Slice, SliceCap | 공개 SliceProceduralMesh와 내부 SliceGeometry, 루프 검사·단면 생성 |
| Geometry/Skinning | 내부 기하의 위치 계산과 joint 영향 규칙. 외부 프레임 갱신은 ProceduralMesh::UpdatePose |
| Geometry/CutPlane, CutPlaneGizmo | world 사각 평면과 편집 기즈모. ContainsLoops, PickSegments |
| Geometry/CutPick | 실제 삼각형 교차와 joint 선택, 스태틱 구 조기 판정 |
| Runtime/CutController, CutJob | 입력 요청·대상·작업·확정·복원 조율 |
| Rendering/RenderBake | 내부 렌더 캐시 굽기와 디버그 튜브의 순수 계산 |
| Rendering/ProceduralMeshRenderer, VS/PS | D3D12 업로드·그리기·fence 수명 관리와 색 표시 |
| Debug/CutDebugView, SceneViewCamera | 확정 메시 표시, 경계·평면·기즈모·상태 UI, 카메라 |

Geometry 계산은 Model/Core/D3D12에 의존하지 않는다. ReadBonePatch와 ReadStaticMesh는 선택 캐시용 raw 읽기다.
공개 절단 파이프라인의 중간 메시가 아니다. 새 교점 생성과 GPU용 배열 갱신, 실패 원자성을 위한 임시 배열은 필요하다.
원본 숨김과 분리는 구현됐지만 실제 충돌·중력·래그돌 물리는 없다. 조각 재질은 아직 디버그 색 표시 중심이다.
