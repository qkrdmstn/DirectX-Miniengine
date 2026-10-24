# 프로젝트 문서 목차

현재 MeshCutting은 유한 평면 기하 API를 제공하며 ModelViewer에는 객체 선택·TRS 편집·깊이 검사 격자 표시가 연결되어 있다. 실제 GPU 절단·교차 미리보기·유한 영역 일부만 걸친 메시의 절단 정책은 미구현이다. 과거 CPU 절단 구현은 제거되었다.

작업 범위에 해당하는 문서만 연다. root AGENTS.md의 라우팅 표를 따르고, 이 목차는 문서를 찾을 때 필요한 항목만 확인한다. History는 해당 알고리즘·조작의 역사 추적 시에만 선택하며 일괄 읽지 않는다. 과거 설계의 완료 표기는 해당 시점의 기록이며 현재 구현 완료를 뜻하지 않는다.

## Current — 현재 현황

| 문서 | 용도 | 상태 |
| --- | --- | --- |
| [CodeMap.md](Current/CodeMap.md) | 현재 Scene·GameObject·선택·평면 기하·렌더 흐름 | 완료 — 현황문서 (`527ed749` 기준 미커밋 문서 정리, 구현 변경 없음) |
| [CutPlaneDesign.md](Current/CutPlaneDesign.md) | 유한 평면 기하·선택·편집·표시 계약과 검증 한계 | 완료 — 단계 1·2·3 구현·리뷰 완료 (`527ed749` 기준 미커밋 문서 정리, 구현 변경 없음), 수동 시각 검증 미확인 |
| [RenderModes.md](Current/RenderModes.md) | Solid/Wireframe/Vertices·입력·깊이·픽셀 캡처 계약 | 완료 — `527ed749` 기준 미커밋 렌더 수정, 검증은 본문 참조 |
| [Scene.md](Current/Scene.md) | 현재 객체 소유권·TRS·조작 | 현재 Scene 구현 안내 |

## GPU — 후속 절단 설계·문헌

| 문서 | 용도 | 상태 / 읽을 때의 주의 |
| --- | --- | --- |
| [GPUCuttingSystemDesign.md](GPU/GPUCuttingSystemDesign.md) | 원본 GPU 버퍼 기반 절단·cap·스키닝 및 제어 계약 | 설계안 — 초기화 기준 `e5975f9`, 실제 GPU 절단 미구현 |
| [참고자료.md](GPU/참고자료.md) | GPU 삼각분할과 CPU 병렬 ear clipping 문헌 비교 | 논문 2편 확인 (2026-10-23) |

## Unreal — 소스 비교·정렬 설계

| 문서 | 용도 | 상태 / 읽을 때의 주의 |
| --- | --- | --- |
| [UnrealSourceComparison.md](Unreal/UnrealSourceComparison.md) | Slice·cap·BreakConstraint·FMeshPlaneCut 소스 비교 | 진행 중 (커밋 전, 화면 확인 전). 프로젝트 구현 설명은 과거 CPU 경로 |
| [UnrealAlignmentDesign.md](Unreal/UnrealAlignmentDesign.md) | Unreal 정렬 로드맵과 채택한 U-1~U-5b 이력 | 과거 코드 경로 완료, 실제 물리는 미구현 |

## NDC — 기법 근거

| 문서 | 용도 | 상태 / 읽을 때의 주의 |
| --- | --- | --- |
| [NDC25Reference.md](NDC/NDC25Reference.md) | 원본 NDC25 기법과 근거 등급 | 근거 문서. 8절의 프로젝트 대응은 과거 CPU 구현 기록 |

## NDC/History — 과거 절단 알고리즘 설계

아래 문서는 기법과 결정 배경을 보존한다. 구현·미구현 상태와 커밋은 원문 시점 기준이며 현행 API는 CodeMap과 CutPlaneDesign을 따른다.

| 문서 | 보존 목적 | 원문 상태 |
| --- | --- | --- |
| [PhysicsSkeletalCutDesign.md](NDC/History/PhysicsSkeletalCutDesign.md) | 물리·실제 BreakConstraint·NDC 전체 기능의 구조 설계 | 설계안, 결정 확정 (`9dfcc33` 기준), 구현 전 |
| [RegionCutDesign.md](NDC/History/RegionCutDesign.md) | 여러 joint 영역을 합친 뒤 자르기 | 설계안·보류, 물리 도입 뒤 재설계 |
| [JointCutDesign.md](NDC/History/JointCutDesign.md) | 단일 joint 메시 교차·패치 추출 | J-1~J-3 구현 (`a6b5d86`), 화면 확인 대기 |
| [PerSegmentCutDesign.md](NDC/History/PerSegmentCutDesign.md) | 선분별·인스턴스별 절단 작업 | 구현 (`b606ba4`), 작업 식별은 이후 JointCut 설계로 변경 |
| [PlaneSlicingDesign.md](NDC/History/PlaneSlicingDesign.md) | 교차·루프·cap·조각 스키닝 핵심 알고리즘 | 완료 (`3e0f55f`, `11c4be6`) |
| [PlaneFirstCutDesign.md](NDC/History/PlaneFirstCutDesign.md) | 유한 평면 후보 선택과 패치 절단 흐름 | P-1·P-2 구현, P-3 미구현·폐기 범위 |

## History — 과거 조작 설계

| 문서 | 보존 목적 | 원문 상태 |
| --- | --- | --- |
| [SceneViewControlsDesign.md](History/SceneViewControlsDesign.md) | 카메라·기즈모·입력 조작의 설계 배경 | 과거 S-1~S-4 기록 (`0fc39fe`), 현재 키 계약은 [Scene.md](Current/Scene.md) |
