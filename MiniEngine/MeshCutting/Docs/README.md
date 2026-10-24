# MeshCutting 문서 목차

현재 MeshCutting은 유한 평면 기하 API를 제공하며 ModelViewer에는 객체 선택·TRS 편집·깊이 검사 격자 표시가 연결되어 있다. 실제 GPU 절단·교차 미리보기·유한 영역 일부만 걸친 메시의 절단 정책은 미구현이다. 과거 CPU 절단 구현은 제거되었다.

필요한 문서만 연다. 현재 구조는 CodeMap, 기법 판단은 NDC25Reference부터 읽는다. 과거 설계의 완료 표기는 해당 시점의 기록이며 현재 구현 완료를 뜻하지 않는다.

## 현재 현황

| 문서 | 용도 | 상태 |
| --- | --- | --- |
| [CodeMap.md](CodeMap.md) | 현재 Scene·GameObject·선택·평면 기하·렌더 흐름 | 완료 — 현황문서 (`096f8a1c` 기준, 미커밋) |
| [CutPlaneDesign.md](CutPlaneDesign.md) | 유한 평면 기하·선택·편집·표시 계약과 검증 한계 | 완료 — 단계 1·2·3 구현·리뷰 완료 (`096f8a1c` 기준, 미커밋), 수동 시각 검증 미확인 |
| [Scene README](../../ModelViewer/Scene/README.md) | 현재 객체 소유권·TRS·조작·표시 모드 | 현재 Scene 구현 안내 |

## NDC·Unreal·GPU 근거와 후속 설계

| 문서 | 용도 | 상태 / 읽을 때의 주의 |
| --- | --- | --- |
| [NDC25Reference.md](NDC25Reference.md) | 원본 NDC25 기법과 근거 등급 | 근거 문서. 8절의 프로젝트 대응은 과거 CPU 구현 기록 |
| [GPUCuttingSystemDesign.md](GPUCuttingSystemDesign.md) | 원본 GPU 버퍼 기반 절단·cap·스키닝 및 제어 계약 | 설계안 — 초기화 기준 `e5975f9`, 실제 GPU 절단 미구현 |
| [참고자료.md](참고자료.md) | GPU 삼각분할과 CPU 병렬 ear clipping 문헌 비교 | 논문 2편 확인 (2026-10-23) |
| [UnrealSourceComparison.md](UnrealSourceComparison.md) | Slice·cap·BreakConstraint·FMeshPlaneCut 소스 비교 | 진행 중 (커밋 전, 화면 확인 전). 프로젝트 구현 설명은 과거 CPU 경로 |
| [UnrealAlignmentDesign.md](UnrealAlignmentDesign.md) | Unreal 정렬 로드맵과 채택한 U-1~U-5b 이력 | 과거 코드 경로 완료, 실제 물리는 미구현 |

## 과거 NDC 설계·조작 이력

아래 문서는 기법과 결정 배경을 보존한다. 구현·미구현 상태와 커밋은 원문 시점 기준이며 현행 API는 CodeMap과 CutPlaneDesign을 따른다.

| 문서 | 보존 목적 | 원문 상태 |
| --- | --- | --- |
| [PhysicsSkeletalCutDesign.md](PhysicsSkeletalCutDesign.md) | 물리·실제 BreakConstraint·NDC 전체 기능의 구조 설계 | 설계안, 결정 확정 (`9dfcc33` 기준), 구현 전 |
| [RegionCutDesign.md](RegionCutDesign.md) | 여러 joint 영역을 합친 뒤 자르기 | 설계안·보류, 물리 도입 뒤 재설계 |
| [JointCutDesign.md](JointCutDesign.md) | 단일 joint 메시 교차·패치 추출 | J-1~J-3 구현 (`a6b5d86`), 화면 확인 대기 |
| [PerSegmentCutDesign.md](PerSegmentCutDesign.md) | 선분별·인스턴스별 절단 작업 | 구현 (`b606ba4`), 작업 식별은 이후 JointCut 설계로 변경 |
| [PlaneSlicingDesign.md](PlaneSlicingDesign.md) | 교차·루프·cap·조각 스키닝 핵심 알고리즘 | 완료 (`3e0f55f`, `11c4be6`) |
| [PlaneFirstCutDesign.md](PlaneFirstCutDesign.md) | 유한 평면 후보 선택과 패치 절단 흐름 | P-1·P-2 구현, P-3 미구현·폐기 범위 |
| [SceneViewControlsDesign.md](SceneViewControlsDesign.md) | 카메라·기즈모·입력 조작의 설계 배경 | 과거 S-1~S-4 기록 (`0fc39fe`), 현재 키 계약은 Scene README |
