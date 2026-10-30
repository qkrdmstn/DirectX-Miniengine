상태: 진행 중 — BreakConstraint M1(BC-00~BC-13a) 완료, BC-13a 미커밋 (2026-10-30)

# 현재 Scene과 유한 평면 코드 지도

현재 구현은 객체 선택·TRS 편집과 유한 평면의 기하·격자 표시까지다. 실제 GPU 절단, 교차 미리보기, 유한 영역에 일부만 걸친 메시의 절단 정책은 미구현이다. 과거 CPU 패치 추출·슬라이스·cap 경로는 현행 코드에 없다.

## 실행 흐름

| 순서 | 함수 | 역할 |
| --- | --- | --- |
| 초기화 | `ModelViewer::Startup`, `CutPlaneSystem::CreatePlane` | Scene에 모델 객체와 원점의 4×4 XZ 평면을 만든다. 시작 선택은 없다. |
| 갱신 | `Scene::Update`, `CutPlaneSystem::PruneStaleObjects` | 모델 자세를 갱신하고 일반 Scene 삭제로 남은 평면 설명을 정리한다. |
| 선택 | `PickObject`, `CutPlaneSystem::Pick`, `NearestPickHit` | 생성 조각은 경계 구 뒤 충돌 hull 교점을 검사하고 원본 구·유한 직사각형과 월드 거리로 비교한다. |
| 편집 | `TransformGizmo`, `GetEditTargetPivot`, `GizmoMath::KeepPivot` | 생성 Mesh의 경계 중심으로 기즈모를 표시·편집하고 노란 hull 모서리로 선택을 표시한다. 평면 크기 모드는 로컬 X/Z만 바꾼다. |
| 스냅샷 | `CutPlaneSystem::BuildData`, `TryBuildCutPlaneData` | 평면 설명과 현재 GameObject TRS로 기하 데이터를 계산한다. |
| 표시 | `Scene::Render`, `RenderCutPlanes` | 모델을 렌더하고 메인 투명 메시 뒤·후처리 전에 평면당 테두리·격자 22개 선을 그린다. 장면 깊이를 검사하며 깊이를 쓰지 않는다. |

## 소유권과 공간 계약

아래 모듈 경로는 저장소의 `MiniEngine/` 기준이다. 문서는 최상위 `Docs/`에 모아 둔다.

| 파일 / 자료형 | 소유 데이터와 책임 |
| --- | --- |
| `ModelViewer/Scene/Scene.*`의 `Scene`, `GameObject` | Scene이 객체를 소유한다. GameObject는 TRS의 원본이며 모델 없이도 존재한다. 모델이 있으면 ModelInstance를 가진다. |
| `ModelViewer/Scene/SkeletalPhysicsSystem.*` | 공통 PhysicsSystem의 world에서 owner별 rig를 등록·정리하고 Kinematic 목표, 동기 BreakConstraint, 배율을 복원한 Dynamic 본 override, 실제 상태 조회를 제공한다. |
| `ModelViewer/Scene/tests/SkeletalPhysicsChecks.h`의 `SkeletalPhysicsChecks` | fixture/Xbot을 실제 공통 fixed-step 경로로 구동하고 본·COM·관절 anchor overlay와 낙하·팔레트 수치 PASS/FAIL을 기록한다. |
| `-skeletal-break-checks 1` / `-skeletal-break-xbot 1` | 각각 3-body fixture / 기존 Xbot 왼팔을 60 tick 뒤 분리하고 180 tick에 판정 후 정상 종료한다. |
| `ModelViewer/Scene/tests/MeshComponentChecks.h`의 `SkeletalLifetimeChecks` | `-component-checks 1`에서 Scene clear·owner 삭제·지연 break 거절과 body·관절·pair·예약 0을 검사한다. |
| `ModelViewer/Scene/ObjectSelection.*`, `PickHit.h` | 원본 경계 구·생성 조각 hull 선택, 조각 중심 피벗, 객체/조각 ID와 월드 거리 비교를 제공한다. |
| `ModelViewer/Scene/TransformGizmo.*`, `SceneTransform.h` | 객체 TRS 편집과 입력 처리를 담당한다. 조작·Undo 계약은 [Scene.md](Scene.md)를 따른다. |
| `ModelViewer/Scene/CutPlaneSystem.*` | Scene을 참조하고 ObjectId별 `CutPlaneDesc`를 소유한다. 별도 평면 TRS를 보관하지 않는다. `Find`는 삭제된 객체를 거절한다. |
| `MeshCutting/Geometry/CutPlane.*`의 `CutPlaneDesc` | 로컬 XZ, y=0 직사각형의 `Width`와 `Height`를 정의한다. |
| `CutPlaneData` | `WorldPlane`은 정규화된 `Vector4(n, d)`이며 식은 n·x+d=0이다. `WorldToPlane`은 affine 역변환, `HalfWidth`·`HalfHeight`는 로컬 반크기, `WorldCorners`는 네 월드 모서리다. |
| `ModelViewer/Scene/CutPlaneRenderer.*`, `Shaders/CutPlaneVS.hlsl`, `CutPlanePS.hlsl` | 스냅샷으로 선을 만들고 전용 line PSO로 표시한다. 절단 결과 버퍼는 생성하지 않는다. |

`TryBuildCutPlaneData`는 로컬 +Y의 역전치 법선을 사용한다. 기하 API는 가역 affine의 반사·비균일 스케일·전단을 지원하지만 Scene 편집은 양수 크기의 TRS 계약을 사용한다. 특이 변환과 비유한 크기는 거절한다.

`IntersectCutPlaneRectangle`은 정규화된 월드 ray의 거리 t를 보존한다. `ClipCoplanarSegmentToCutPlaneRectangle`은 공면 선분의 내부 구간을 원본 선분의 [0,1]로 반환한다. 둘 다 실패 시 출력 인자를 보존한다.

구현·검증 범위는 [CutPlaneDesign.md](CutPlaneDesign.md), 후속 GPU 방향은 [GPUCuttingSystemDesign.md](../GPU/GPUCuttingSystemDesign.md), NDC 근거 등급은 [NDC25Reference.md](../NDC/NDC25Reference.md)를 본다.

BC-03a(3bee0b20 이후): ModelInstance::EvaluateAnimation은 애니메이션 node를 평가하고 FinalizePose는 물리 override 후 전체 팔레트를 확정한다. 기존 Update는 두 함수를 연속 호출한다. 이 단계에서 Scene의 GPU 준비 순서는 유지한다.

BC-03b: Scene::EvaluateAnimations가 모든 owner의 CPU 애니메이션을 평가하고 FinalizePoses가 override 없는 최종 pose를 만든다. 물리 owner는 이후 FinalizeObjectPose로 해당 pose를 다시 확정한다. PrepareMeshes는 시간 진행 없이 GPU 준비만 하며, 기존 Update는 세 단계를 연속 호출한다. 비물리 회귀는 -physics-sim 0 -component-checks 1 -physics-query-exit 6으로 Debug/Release PASS를 확인했다.


BC-08a-1: ModelViewer/Scene/SkeletalPhysicsFrames.h의 TryBuildBodyFrame은 배율을 제거한 E*Gnode에 월드 m 단위 body offset을 적용한다. TrySplitUniformFrame은 양수 uniform scale·직교성·affine·finite를 검사하고 TryRestoreNodePose는 저장 배율을 복원하는 순수 좌표 왕복을 제공한다. SkeletalFrameTests는 Jolt/GPU 없이 검사한다. rig 저장·Kinematic 구동은 BC-08a-2, Scene 연결은 BC-11에서 완료했다.

BC-08a-2: SkeletalPhysicsSystem::RegisterFromPose는 자산·부모·등록 s와 E 배율을 저장하고 UpdateKinematicTargets는 전체 목표를 선검증하여 Kinematic만 MoveKinematic으로 구동한다. GetBodyTarget은 body 원점 목표와 COM 선속도·월드 각속도를 읽는다. PhysicsWorld::CanMoveKinematic과 GetBodyMotion으로 SDK 타입 없이 읽기 전용 검증을 수행한다.

BC-08b: SkeletalPhysicsSystem::CollectPoseOverrides는 Dynamic body만 현재 E에서 model node pose로 복원하고 성공 시 override 배열을 교체한다. 등록 s·body offset을 복원하며 Kinematic 본은 기존 애니메이션을 유지한다. 결과는 기존 ModelInstance::FinalizePose 또는 Scene::FinalizeObjectPose에 전달할 자료이며 실제 fixed-step Scene 연결은 BC-11이다.

BC-10: SkeletalPhysicsSystem::BreakConstraint는 broken 경계를 제외한 subtree와 세대를 검증하고 PhysicsWorld::ApplyConstraintBreak로 일괄 적용한다. PhysicsWorldBreak.cpp가 anchor·Jolt 제한·외부 pin·전환 목표·impulse를 선검증하고 선택 관절만 제거한다. 성공 시 Dynamic body를 기존 CollectPoseOverrides가 수집하며 topology가 증가한다. 같은 관절 재요청은 AlreadyBroken이고 추가 impulse가 없다. BC-11의 PhysicsSystem::AdvanceSimulation이 animation 평가→Kinematic 목표→queued break→공통 world step→Static/본 writeback→owner별 pose 확정을 연결한다. ModelViewer는 물리 on에서 렌더 프레임당 PrepareMeshes만 한 번 호출하며 off는 기존 Update wrapper를 쓴다. zero-step은 기존 pose/대기 break를 유지하고 Scene::InitializePoses가 신규 모델만 dt=0으로 준비한다. section E와 Nmesh ancestry를 검증하며 world는 비공개다.
