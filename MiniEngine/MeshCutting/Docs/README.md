# MeshCutting 문서 목차

필요한 문서만 연다. 구조를 모르면 `CodeMap.md`부터, 기법 판단은 `NDC25Reference.md`를 본다.
상태가 바뀌면 이 표와 해당 문서 머리의 `상태:` 줄을 함께 고친다.

## 기준 문서 (항상 최신으로 유지)

| 문서 | 용도 | 상태 | 언제 읽나 |
| --- | --- | --- | --- |
| `NDC25Reference.md` | NDC25 근거와 프로젝트 대응 | 8절은 통합 구현 `cb1c12a` 기준 | 설계·구현 판단 전 |
| `UnrealAlignmentDesign.md` | Unreal 정렬 로드맵의 이력 | U-1~U-5b 코드 구현. 분리는 디버그 방식이며 물리는 미구현 | 과거 설계 배경 확인 |
| `CodeMap.md` | 현재 실행·데이터 흐름과 파일별 역할 | ProceduralMesh 통합·CPU·Debug/Release 빌드 검증 완료 (`cb1c12a`) | 코드를 처음 열 때 |

## 진행·예정 설계

| 문서 | 용도 | 상태 | 언제 읽나 |
| --- | --- | --- | --- |
| `ClassLayoutDesign.md` | 이전 클래스 배치·이름 스케치 | 참고 설계. 현재 통합은 아래 문서 기준 | 과거 결정 확인 |
| `ProceduralMeshSimplification.md` | 공통 메시 통합, 중간 변환·중복 제거 | 구현·CPU·Debug/Release 빌드 검증 완료 (`cb1c12a`) | 현재 구조와 검증 확인 |
| `RegionCutDesign.md` | 영역 작업, 걸친 joint 합친 뒤 자르기 (NDC 한계 돌파) | 설계안, 보류 (JointCutDesign 다음 단계) | 한계 돌파 작업 전 |
| `PickPerformanceDesign.md` | 절단 후보 선택(pick·추출) 성능 측정과 BVH 없는 개선 방향 | 안 A는 U-1로 구현 (`ccf0349`), 안 B 설계안. U-1 이후 실측 포함 | 편집 중·Enter 프레임 비용을 줄일 때 |

## 완료 기록 (코드 주석이 참조하므로 자리를 옮기지 않는다)

| 문서 | 용도 | 상태 | 언제 읽나 |
| --- | --- | --- | --- |
| `JointCutDesign.md` | 메시 교차 기반 단일 joint 작업 (NDC 초기 단계, 한계 체험) | 구현 (J-1~J-3, `a6b5d86`). 화면 확인 대기 | 무엇을 자를지(작업·패치 joint) 바꿀 때 |
| `PerSegmentCutDesign.md` | 선분별·인스턴스별 절단 작업 | 구현 (`b606ba4`). 작업 식별은 JointCutDesign으로 대체됨 (선분 key는 모양만 남김) | 작업 목록·등록·Enter/Ctrl+Z·표시를 고칠 때 |
| `PlaneFirstCutDesign.md` | world 유한 평면, 후보 패치 (P-1, P-2) | P-1·P-2 완료, P-3 폐기 | 평면 코드를 고칠 때 |
| `SceneViewControlsDesign.md` | 마우스 카메라, 평면 기즈모, 키 (S-1~S-4) | 완료 (`0fc39fe`), 현재 조작의 기준 | 입력·기즈모를 고칠 때 |
| `CutDebugDesign.md` | 디버그 와이어프레임, Enter 확정 절단 (D-0~D-3 이력 포함) | 완료 (`d33b1a7`), 자동 미리보기 제거 `cd9a09e` | 디버그 표시를 고칠 때 |
| `PlaneSlicingDesign.md` | 평면 슬라이스, 루프, cap, 조각 스키닝 알고리즘 | 완료 (`3e0f55f`, `11c4be6`) | Slice/SliceCap/Skinning 알고리즘을 볼 때 |

## 보관 (`archive/`, 과거 구조 확인용)

| 문서 | 용도 | 상태 |
| --- | --- | --- |
| `archive/CurrentFlowReport.md` | 선분별 작업 시점의 흐름 보고서 (U-1 이전, 편집 중 패치 추출) | 보관, `1287cb5` 기준. 현재 흐름은 `CodeMap.md` |
| `archive/ProjectFlowReport.md` | 2026-10-20 시점 흐름 보고서 (Codex 작성) | 보관, 줄 번호 낡음 |
