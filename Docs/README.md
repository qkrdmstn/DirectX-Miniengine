# 프로젝트 문서 목차

현재 Static GPU 절단·단면·재절단과 같은 GameObject의 조각 MeshComponent 관리가 연결되어 있다. Jolt v5.6.0의 조회용 collider를 자동 등록해 평면 후보를 찾는다. 열린 경계는 cap만 생략한다. Skeletal 절단·동적 낙하/접촉 반응은 미구현이며 과거 CPU 경로는 Git 기록에서 비교한다.

작업 범위에 해당하는 문서만 연다. root AGENTS.md의 라우팅 표를 따르고, 이 목차는 문서를 찾을 때 필요한 항목만 확인한다. History는 해당 알고리즘·조작의 역사 추적 시에만 선택하며 일괄 읽지 않는다. 과거 설계의 완료 표기는 해당 시점의 기록이며 현재 구현 완료를 뜻하지 않는다.

## 인수인계

| 문서 | 용도 | 상태 |
| --- | --- | --- |
| [Weekend-2026-10-24-25.md](Handoff/Weekend-2026-10-24-25.md) | Claude용 주말 작업·최종 설계 결정·커밋 구분·검증·다음 작업 주의점 | 완료 — `feb35f23` 및 `39db69f3` 커밋된 Jolt 조회 기준, 2026-10-26 확인 |

## Current — 현재 현황

| 문서 | 용도 | 상태 |
| --- | --- | --- |
| [CodeMap.md](Current/CodeMap.md) | 현재 Scene·GameObject·선택·평면 기하·렌더 흐름 | `3dc5d6ba` 기반 조각 hull 선택·중심 기즈모·선택 모서리 갱신 |
| [CutPlaneDesign.md](Current/CutPlaneDesign.md) | 유한 평면 기하·선택·편집·표시 계약과 검증 한계 | 완료 — 평면 `527ed749` 기반, GPU 연결 `673b56b6`; 수동 입력 검증 범위는 본문 참조 |
| [RenderModes.md](Current/RenderModes.md) | Solid/Wireframe/Vertices·입력·깊이·픽셀 캡처 계약 | 완료 — `673b56b6` Compute pose 연결, 검증은 본문·GPU 설계 참조 |
| [Scene.md](Current/Scene.md) | GameObject·다중 MeshComponent·선택·Gizmo | `3dc5d6ba` 기반 조각 hull 선택·중심 기즈모·노란 선택 모서리 구현, CPU/실행 진단 통과; 수동 화면 미확인 |

## GPU — 후속 절단 설계·문헌

| 문서 | 용도 | 상태 / 읽을 때의 주의 |
| --- | --- | --- |
| [GPUCuttingSystemDesign.md](GPU/GPUCuttingSystemDesign.md) | GPU 절단 설계·다중 MeshComponent 기반 | `673b56b6` Static 구현, Skeletal·물리는 설계 |
| [StaticCutImplementation.md](GPU/StaticCutImplementation.md) | Static GPU 절단·열린 경계·재절단·성능 | `673b56b6` GPU 절단 + `39db69f3` 커밋된 Jolt 후보 조회·반복 절단 회귀 완료 |
| [CapParallelOptimization.md](GPU/CapParallelOptimization.md) | cap 그룹 크기·결정성·MX450 비교 측정 | 진행 중 — `1532fc60` 기준 단계 1·1b 미커밋, 결정성 확인; 다음은 CapScanMarking S1 |
| [CapScanMarking.md](GPU/CapScanMarking.md) | cap 선택을 누적합 기반 교대 표시로 교체 | 진행 중 — `6f328257` 기준 S1 구현·MX450 checksum/그룹 크기 검증 완료(미커밋), 기존 미캡핑 진단 잔존; S2 이후 미착수 |
| [ConcurrentCutJobs.md](GPU/ConcurrentCutJobs.md) | 여러 Static CutJob 제출·작업별 snapshot·완료 게시 | 완료 — `f2138029` 기준 1단계, Debug/Release·20회 실행 검증, 미커밋 |
| [참고자료.md](GPU/참고자료.md) | 단일 cap 내부 병렬화·GPU 이식 후보 및 문헌의 적용 한계 | 논문 2편 분석 기록 (2026-10-25); 실제 GPU 측정은 StaticCutImplementation 참조 |

## Unreal — 소스 비교·정렬 설계

| 문서 | 용도 | 상태 / 읽을 때의 주의 |
| --- | --- | --- |
| [UnrealSourceComparison.md](Unreal/UnrealSourceComparison.md) | Slice·cap·물리·Actor/컴포넌트·Skeletal 충돌 소스 비교 | `feb35f23` 기준 조사 — 과거 CPU 이력 + UE5.3.2 GPU 접촉·절단 물리·Physics Asset 비교 |
| [UnrealAlignmentDesign.md](Unreal/UnrealAlignmentDesign.md) | Unreal 정렬 로드맵과 채택한 U-1~U-5b 이력 | 과거 코드 경로 완료, 실제 물리는 미구현 |

## Physics — 후속 물리 도입

| 문서 | 용도 | 상태 |
| --- | --- | --- |
| [JoltQueryIntegration.md](Physics/JoltQueryIntegration.md) | 현재 SDK 빌드·collider 등록·평면 후보 API·실행 검증 | 완료 — `39db69f3` 커밋된 Jolt v5.6.0 조회 연결·Debug/Release/Profile 빌드·CPU/실행 검증 |
| [PhysicsIntegrationPlan.md](Physics/PhysicsIntegrationPlan.md) | Jolt 조회 이후 동적 조각·proxy·절단 transaction 계획 | 진행 중 — `39db69f3` 조회 구현 완료; 동적 조각 물리는 StaticPiecePhysics로 대체, Skeletal 물리는 후속 |
| [StaticPiecePhysics.md](Physics/StaticPiecePhysics.md) | Static 절단 조각별 rigid body 물리·바닥·요구사항 v4·구현 계획 | 진행 중 — 7a~7d 완료, `26302519` 기반 바닥 최소 1,000×1,000 m 확장; 8단계 전체 검증 미진행 (2026-10-28) |
| [BreakConstraintDesign.md](Physics/BreakConstraintDesign.md) | UE5.3.2 관절 해제·Actor 유지·physics pose 반영 원리 | 설계안 — `5f56b93d` 재대조, 구현 계약은 아래 인계 문서 참조 (2026-10-28) |
| [BreakConstraintImplementation.md](Physics/BreakConstraintImplementation.md) | Codex 구현용 파일·API·frame·상태·단계·테스트·첫 작업 지시 | 설계안 — `5f56b93d` 기준 인계 v2, Xbot 팔 데모 추가, BC-00부터 구현 전 (2026-10-28) |

## NDC — 기법 근거

| 문서 | 용도 | 상태 / 읽을 때의 주의 |
| --- | --- | --- |
| [NDC25Reference.md](NDC/NDC25Reference.md) | 원본 NDC25 기법과 근거 등급 | `3dc5d6ba` 기준 절단 패치 스키닝 해석 연결 (2026-10-28); 8절의 프로젝트 대응은 과거 CPU 구현 기록 |
| [NDC25CutPatchSkinning.md](NDC/NDC25CutPatchSkinning.md) | 절단 패치의 본 재부착 대신 스키닝하는 아이디어·단계별 해석·적용 범위 | 완료 — `3dc5d6ba` 기준 사용자 제공 두 슬라이드·대사 해석 기록, 구현 결정 아님 (2026-10-28) |
| [NDC25WoundAndLayeredMesh.md](NDC/NDC25WoundAndLayeredMesh.md) | 상처 표현(클립·데칼·디폼·레퍼런스 포즈)과 Dead Island 2 이중 메시 분석 | 설계안 — `425491c5` 기준 슬라이드 근거 정리, 구현 전 (2026-10-28) |

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
