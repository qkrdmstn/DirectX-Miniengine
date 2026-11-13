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
| [CodeMap.md](Current/CodeMap.md) | 현재 Scene·GameObject·선택·평면 기하·렌더 흐름 | 완료 — BreakConstraint M1과 master `913031f3` 병합 검증; 다중 Static CutJob·cap 단계 1 기록 유지, cap 결과 불변은 미확인 (2026-10-30) |
| [CutPlaneDesign.md](Current/CutPlaneDesign.md) | 유한 평면 기하·선택·편집·표시 계약과 검증 한계 | 완료 — 평면 `527ed749`, GPU 연결 `673b56b6`; 스켈레탈 겹침 클릭 깊이 판정은 `Fix cut plane picking near skinned meshes`에 포함 |
| [RenderModes.md](Current/RenderModes.md) | Solid/Wireframe/Vertices·입력·깊이·픽셀 캡처 계약 | 완료 — 본 부착 패치 선·점 초록색, D/R 빌드·두 모드 캡처 확인 (2026-11-06) |
| [Scene.md](Current/Scene.md) | GameObject·다중 MeshComponent·선택·Gizmo | `3dc5d6ba` 기반 조각 hull 선택·중심 기즈모·노란 선택 모서리 구현, CPU/실행 진단 통과; 수동 화면 미확인 |

## GPU — 후속 절단 설계·문헌

| 문서 | 용도 | 상태 / 읽을 때의 주의 |
| --- | --- | --- |
| [GPUCuttingSystemDesign.md](GPU/GPUCuttingSystemDesign.md) | GPU 절단 설계·다중 MeshComponent 기반 | `673b56b6` Static 구현, Skeletal·물리는 설계 |
| [StaticCutImplementation.md](GPU/StaticCutImplementation.md) | Static GPU 절단·열린 경계·재절단·성능 | `673b56b6` GPU 절단 + `39db69f3` 커밋된 Jolt 후보 조회·반복 절단 회귀 완료 |
| [CapParallelOptimization.md](GPU/CapParallelOptimization.md) | cap 그룹 크기·결정성·MX450 비교 측정 | 진행 중 — 단계 1·1b `6f328257`; 단계 3(CapScanMarking)으로 5060 Ti 1.70 ms, 목표 달성; 1c 미착수 |
| [CapScanMarking.md](GPU/CapScanMarking.md) | cap 선택을 누적합 기반 교대 표시로 교체 | 진행 중 — S2 `1a7d77ed` 이후 전체 지원 loop MC 통일·해시 귀 선택 제거·D/R 검증 완료 `0553410b`; 5060 Ti Bunny 재측정 1.71 ms(회귀 없음); 기존 미캡핑 진단 잔존 |
| [CapActiveTestList.md](GPU/CapActiveTestList.md) | cap 내부 점 검사(InTriangle) 대상을 라운드별 목록으로 압축 | 진행 중 — AL-1 `74f4940d`, AL-0·AL-1 각 41회 회귀 통과·대조 불일치 0; 5060 Ti cap 1.01→0.51 ms; attempt=1 미검증 (2026-11-02) |
| [CapCacheLimit.md](GPU/CapCacheLimit.md) | Next·Prev·Flags 비트 묶음으로 cap 캐시 상한 1024→1536 | 진행 중 — CL-1 `983a8c9d`, Release 회귀 checksum 동일; Dragon y=0.08 cap 5060 Ti 18.48→1.89 ms·MX450 21.7→3.5 ms; Debug 미실시 (2026-11-10) |
| [CapSlotVram.md](GPU/CapSlotVram.md) | CachedSlot을 VRAM 순서 테이블로 옮겨 cap 캐시 상한 1536→1976 | 진행 중 — SV-1 `47d0edfa`; 5060 Ti Armadillo y=60 cap 66.31→3.62 ms, Bunny·Dragon 변화 없음; 기하 검증 미보고 (2026-11-10) |
| [ConcurrentCutJobs.md](GPU/ConcurrentCutJobs.md) | 여러 Static CutJob 제출·작업별 snapshot·완료 게시 | 완료 — `f2138029` 기준 1단계, Debug/Release·20회 실행 검증 `1532fc60` |
| [CutInputFromRevision.md](GPU/CutInputFromRevision.md) | Static 절단 입력을 PG에서 revision Data+section 행렬로 교체, GB::Pose 제거, 패치 조각 강체 유지 결정 | 진행 중 — CI-1a `62b5228c`, CI-1b `7947cfb1`, CI-2 `6dd6fe35`, CI-3 `f7b0bb9c`, CI-4 `6bfa3092`, CI-5 GB::Pose 제거 D/R 검증 완료; 재절단 법선 오차 미측정 (2026-11-04) |
| [SkeletalPatchCopyCut.md](GPU/SkeletalPatchCopyCut.md) | C 입력으로 영향 본 선택→패치 GPU 복사→정적 복사본 무한 절단 | 진행 중 — SP-7e 게시·부착 구현(미커밋, base `9400691f`); 경계 오차·강제 Stale·캡처 확장 진단 미완료 (2026-11-03) |
| [SkeletalPatchBreak.md](GPU/SkeletalPatchBreak.md) | SP-7: 선택 joint BreakConstraint, 자동 rig, 원본 숨김(상태 버퍼+치환 IB), A/B node 부착 | 진행 중 — SP-7e 게시·부착 및 연속 캡처 시연 옵션(미커밋, base `03b5c8a1`, 2026-11-06); 시연 후속 C 거절·실패 종료·진단 옵션 격리; 호출자 Release 관찰 기록, 리뷰 수정 빌드·실행 미수행 |
| [AttachedPatchRecut.md](GPU/AttachedPatchRecut.md) | 부착 패치 조각 Static 재절단: node 쪽 부착 유지, 반대쪽 독립 낙하 | 완료 — AR-4 bb766e1b, Debug/Release 실행 PASS(2026-11-12) |
| [PatchSkinning.md](GPU/PatchSkinning.md) | 스켈레탈 패치 조각 스키닝: 끊은 쪽 joint를 부착 node 상대로 고정, 새 정점은 강체 | 진행 중 — PS-0~PS-3 완료(2026-11-13), PS-4 이후 미착수 |
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
| [BreakConstraintImplementation.md](Physics/BreakConstraintImplementation.md) | Codex 구현용 파일·API·frame·상태·단계·테스트 | 완료 — M1(BC-00~BC-13a) `0c7fcaa7` (2026-10-30). 실제 큐6/6 NotFound 자동 검사, CPU 두 구성·Debug/Release 빌드·Debug 회귀 PASS, 결과 버리기 변형 FAIL·SHA-256 복구. 수명 리소스0·D3D12 ERROR0. BC-09/14 GPU 절단은 후속 (2026-10-30) |
| [RagdollRigPreload.md](Physics/RagdollRigPreload.md) | 로드 시 래그돌 rig 생성, kinematic sensor, Jolt 조회·segment 뼈 선택, segment 없는 뼈 영역 Static 절단 | 완료 — R1a~R4b `b215bdee`~`dafd19ac`, R5 문서 동기화·Debug/Release 검증 반영 (2026-11-12) |
| [RagdollPartGizmo.md](Physics/RagdollPartGizmo.md) | break 후 떨어진 Dynamic 성분과 kinematic 몸의 개별 기즈모, owner 이동 비추종 | 설계안 — 사용자 결정 반영, 구현 전 (2026-11-06) |

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
