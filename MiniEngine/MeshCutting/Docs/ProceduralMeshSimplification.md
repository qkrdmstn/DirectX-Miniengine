상태: 완료 — 통합 구현 및 CPU·Debug/Release 빌드 검증 완료 (`cb1c12a`).

# ProceduralMesh 중심 통합

사용자의 이번 지시에 따라 약 300줄마다 멈추던 제한을 해제하고 남은 통합을 한 번에 적용했다.
범위는 원본 생성·절단·스키닝·렌더링의 메시 객체 통일이다. 절단 정책이나 물리 기능을 바꾸는 작업은 아니다.

## 최종 흐름

1. ExtractBonePatch 또는 CopyProceduralMeshFromStaticMesh가 ProceduralMesh를 생성한다.
2. 같은 메시를 UpdatePose로 현재 자세에 맞춘다.
3. SliceProceduralMesh가 메시를 받아 양쪽 ProceduralMesh를 만든다.
4. 사각 평면 범위까지 통과한 두 메시를 CutJob에 저장한다.
5. 후속 프레임은 두 메시의 UpdatePose를 호출하고 뷰가 같은 객체를 그대로 그린다.

## 없앤 것과 남긴 것

- WeldInput / WeldInputVertex / MakeWeldInput을 삭제했다.
- PatchPiece를 삭제했다. 기하 저장 레코드는 ProceduralMesh.h의 MeshGeometry이며 메시 내부에 직접 소유한다.
- 기존 PatchNode/Corner/Face/Origin/Cap은 MeshVertex/Corner/Face/VertexOrigin/CutRecord로 통일했다.
- Patch.h/.cpp는 자료구조 모음에서 Geometry/MeshGeometry.h/.cpp 계산 코드로 정리했다.
- CutJob의 임시 patch와 CutJobSource 래퍼를 없앴다.
- CutDebugView의 별도 표면 메시와 프레임별 표면 복사·재굽기 코드를 없앴다.
- BonePatchSource는 스태틱에도 쓰이는 공통 MeshSource로 이름을 맞췄다.
- SourceVertex에는 원본 정점 번호·슬롯·마스크 선택 여부만 남겼다. 위치·법선·joint/weight 중복은 삭제했다.
- 원본 파일 읽기 형식(ProceduralMeshData)은 한 번의 import용 입력과 렌더 캐시 형식으로 남긴다.
- 용접은 최초 생성 시, 렌더 굽기는 위상 변경 시 메시 내부에서 한다. 위치·범위 갱신은 UpdatePose가 책임진다.

공개 API가 기존 변환을 감추기만 한 것은 아니다. 작업 메시의 왕복 복사, 임시 WeldInput,
별도 뷰 표면, 원본 출처의 변형 배열을 실제로 없앴다. 내부 계산의 입력/출력 기하 레코드나
GPU용 정점 배열, 실패 시 기존 상태를 보존하는 임시 배열까지 없앴다는 뜻은 아니다.

## 보존한 계약

- 위상과 위치는 메시의 API로만 갱신한다. GetGeometry/GetVertices/GetCorners/GetFaces는 읽기 전용이다.
- 두 결과가 성공적으로 구성된 뒤에만 SliceProceduralMesh가 입력·반대쪽 출력을 바꾼다. 별칭 출력은 거절한다.
- 잘린 양쪽은 const Skinning 표를 공유하고 기하 배열은 따로 가진다.
- 스태틱은 joint를 만들지 않고 메시 world 하나로 움직인다.
- 원본 마스크의 selected 플래그는 기존 InJointSet/minWeight 판정을 import 때 기록한 값이다.
- 뷰가 GPU 등록을 해제하기 전에는 확정 메시를 파괴하지 않도록 shared_ptr로 수명을 유지한다.
- 유한 사각 평면 정책은 내부 분할 후 루프 전체 범위를 승인하는 기존 방식이다.
- jobSkin 계측에는 렌더 위치·bounds 갱신도 포함된다. 이전 계측과 비교할 때 범위를 구분한다.
- 다음 Enter는 기존 결과를 되돌려 재생성한다. 누적 재절단·패치 병합·물리는 추가하지 않았다.

## 읽을 순서

ProceduralMesh.h → BonePatch / StaticMeshCopy의 생성 API → CutController::ConfirmJob →
Geometry/Slice → SliceCap → Skinning → ProceduralMesh::UpdatePose → CutDebugView / Renderer.
함수·파일별 현재 흐름은 CodeMap.md를 기준으로 읽는다. ClassLayoutDesign.md는 이전 설계 스케치다.

## 검증

이전 단계는 CPU 9,453 checks 및 Debug/Release 빌드를 통과했다.
최종 검증은 공개 Build → UpdatePose → SliceProceduralMesh → 렌더 데이터 전체 흐름과
스킨 표 공유, 면적 보존, cap 옵션, 실패 원자성, 별칭 출력 거절까지 포함한다.
CPU 출력과 빌드 로그는 저장소 밖에 보관한다.

- 최종 CPU Debug / Release: 각각 9,470 checks, 0 failed.
- ProceduralMeshSmoke: Debug / Release 모두 통과.
- ModelViewer: Debug / Release 빌드 모두 성공.
- DirectX 실행 화면·GPU validation 확인은 수행하지 않았다. 렌더링과 조작의 실기 확인은 남아 있다.
