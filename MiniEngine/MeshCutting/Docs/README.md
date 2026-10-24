# MeshCutting 문서 목차

> 초기화 안내: 기존 CPU 구현과 프로젝트 연결은 제거했으며 문서만 보존했습니다. 아래 기존 구현 상태·코드 설명은 초기화 전 스냅샷 `2b4871f5`의 기록입니다. 현재 방향은 [GPU 기반 시스템 설계](GPUCuttingSystemDesign.md)이며, 아직 구현 전입니다.

필요한 문서만 연다. 구조를 모르면 `CodeMap.md`부터, 기법 판단은 `NDC25Reference.md`를 본다.
상태가 바뀌면 이 표와 해당 문서 머리의 `상태:` 줄을 함께 고친다.

## 기준 문서 (항상 최신으로 유지)

| 문서 | 용도 | 상태 | 언제 읽나 |
| --- | --- | --- | --- |
| [GPUCuttingSystemDesign.md](GPUCuttingSystemDesign.md) | 원본 GPU 버퍼 기반 절단·cap·스키닝 및 제어 계약 | 설계안 — 초기화 기준 `e5975f9`, GPU 미구현 | 새 GPU 시스템을 설계·구현할 때 먼저 읽기 |
| `참고자료.md` | 평면 절단·GPU 삼각분할 문헌, CUDA 다각형별 병렬화와 CPU 병렬 ear clipping의 차이 | 논문 2편 확인 (2026-10-23), 자유곡선 기반 접근 제외 | GPU 절단·단면 생성 알고리즘을 검토할 때 |
| `NDC25Reference.md` | NDC25 근거와 프로젝트 대응 | 8절은 통합 구현 `cb1c12a` 기준 | 설계·구현 판단 전 |
| `UnrealAlignmentDesign.md` | Unreal 정렬 로드맵의 이력 | U-1~U-5b 코드 구현. 분리는 디버그 방식이며 물리는 미구현 | 과거 설계 배경 확인 |
| `UnrealSourceComparison.md` | 언리얼 소스(Slice, ProceduralMesh, BreakConstraint, FMeshPlaneCut)와의 비교, 맞춘 cap 정책 | 진행 중 (커밋 전) | 절단·cap·관절 분리를 언리얼과 맞출 때 |
| `CodeMap.md` | 현재 실행·데이터 흐름과 파일별 역할 | ProceduralMesh 통합·CPU·Debug/Release 빌드 검증 완료 (`cb1c12a`) | 코드를 처음 열 때 |

## 진행·예정 설계

| 문서 | 용도 | 상태 | 언제 읽나 |
| --- | --- | --- | --- |
| `SkeletalPerformanceReview.md` | 실시간 스켈레탈 절단의 성능 근거·미측정 항목·개선 우선순위 | 설계안 (`9dfcc33` + 2026-10-23 작업 트리 조사), 신규 실행 측정 없음 | 성능 개선 순서·측정 범위를 정할 때 |
| `PhysicsSkeletalCutDesign.md` | 물리 엔진·실제 BreakConstraint 도입과 NDC 전체 기능을 위한 구조 개편: 포즈 공간 계약, 프레임 순서, CutController 분리, 절단 이벤트 누적, 단계 R/P/N | 설계안 (`9dfcc33`, Codex 검토 12건 반영), 결정 확정 (PhysX 5, 다음 주 R-0~R-5·P-0~P-2), 구현 전 | 물리·분리·절단 구조를 바꾸기 전 |
| `TargetMeshCacheDesign.md` | 스태틱/스켈레탈 대상의 공통 용접 `ProceduralMesh` 원본 캐시와 생성 시점 | 진행 중 (공통 캐시 구축, Debug/Release 빌드 확인 완료·커밋 전). 스켈레탈 Enter 패치 추출은 다음 단계 | 대상 원본 캐시를 바꿀 때 |
| `ClassLayoutDesign.md` | 이전 클래스 배치·이름 스케치 | 참고 설계. 현재 통합은 아래 문서 기준 | 과거 결정 확인 |
| `ProceduralMeshSimplification.md` | 공통 메시 통합, 중간 변환·중복 제거 | 구현·CPU·Debug/Release 빌드 검증 완료 (`cb1c12a`) | 현재 구조와 검증 확인 |
| `RegionCutDesign.md` | 영역 작업, 걸친 joint 합친 뒤 자르기 (NDC 한계 돌파) | 설계안, 보류 (JointCutDesign 다음 단계). `PhysicsSkeletalCutDesign.md` N-3 단계에서 조각 재절단 계약과 함께 다시 설계 | 한계 돌파 작업 전 |
| `CutFrameDropReport.md` | 절단 확정 후 매 프레임 드랍 측정, 원인(조각 재계산·재업로드)과 해결안 | 안 1 구현 `57587ba`, 안 2 구현 (측정·확인 실험 `4f4bfb0`). 안 3 미구현, 구현 후 재측정 전 | 확정 조각 갱신·업로드 비용을 줄일 때 |
| `CutEnterCostReport.md` | 스태틱 bunny Enter 1회 비용의 단계별 실측(약 186 ms), 중복 검증·추출 반복과 줄일 수 있는 양 | 측정 완료 (`fc1f739` Release, 임시 계측 되돌림), 개선 미구현 | Enter 프레임 비용을 줄일 때 |
| `EnterCostReductionDesign.md` | Enter 비용 줄이기 단계 설계: C 스태틱 SkinPatch 생략 → B 중복 검증 제거 → A 용접 결과 캐시 | 단계 C·B 구현·검증 완료 (커밋 전, Enter 약 186 → 148 ms), A 설계안 | Enter 비용 개선을 구현할 때 |
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
