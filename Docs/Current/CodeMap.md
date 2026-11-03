상태: 완료 — BreakConstraint M1(BC-00~BC-13a)과 master `913031f3` 병합 `32cc650a`; `f2138029` 다중 Static CutJob·`1532fc60` cap 단계 1 기록 유지, cap 결과 불변은 미확인 (2026-10-30); SP-5b 진단 복사본 절단 미커밋 동기화(2026-11-03); 전체 지원 loop MC 통일 `0553410b`(D/R 검증 완료); SP-6 Parent/Influence 독립 리뷰·blend-zone 포함 20회 진단 완료(미커밋, base `329aec7b`); SP-7a `731c7005` + SP-7a2 CPU 정점 보존·D2 bounds 구현·D/R 진단 검증(미커밋, base `731c7005`); SP-7b 지연 등록·incoming·요청별 break 결과 D/R 검증 완료(미커밋, base `18848ec0`); SP-7c 컴포넌트 마스크·렌더 연결 D/R 검증 완료(미커밋, base `d10077bd`); SP-7d D3 제거 제외·D6 인접 진단 D/R 검증 완료(미커밋, base `f83ecf1e`)

# 현재 Scene과 유한 평면 코드 지도

현재 구현은 객체 선택·TRS 편집, 유한 평면의 기하·격자 표시와 Static GPU 절단·cap·재절단을 포함한다. 유한 영역을 넘는 교차는 후보 전체를 거절한다. 원본 Skeletal 절단과 교차 미리보기는 미구현이다. SP-5b(미커밋)는 영향 패치의 정적 복사본만 무한 평면으로 절단하고 B를 월드 0.1 m 분리한다([설계·검증](../GPU/SkeletalPatchCopyCut.md)). 과거 CPU 패치 추출·슬라이스·cap 경로는 현행 코드에 없다.

## 실행 흐름

| 순서 | 함수 | 역할 |
| --- | --- | --- |
| 초기화 | `ModelViewer::Startup`, `CutPlaneSystem::CreatePlane` | Scene에 모델 객체와 원점의 4×4 XZ 평면을 만든다. 시작 선택은 없다. |
| 갱신 | `Scene::Update`, `CutPlaneSystem::PruneStaleObjects` | 모델 자세를 갱신하고 일반 Scene 삭제로 남은 평면 설명을 정리한다. |
| 선택 | `PickObject`, `CutPlaneSystem::Pick`, `NearestPickHit` | 생성 조각은 경계 구 뒤 충돌 hull 교점을 검사하고 원본 구·유한 직사각형과 월드 거리로 비교한다. |
| 편집 | `TransformGizmo`, `GetEditTargetPivot`, `GizmoMath::KeepPivot` | 생성 Mesh의 경계 중심으로 기즈모를 표시·편집하고 노란 hull 모서리로 선택을 표시한다. 평면 크기 모드는 로컬 X/Z만 바꾼다. |
| 스냅샷 | `CutPlaneSystem::BuildData`, `TryBuildCutPlaneData` | 평면 설명과 현재 GameObject TRS로 기하 데이터를 계산한다. |
| cap·진단 | `CapEarsCS::main`, `CutJob::ValidateDiagnostics`, `PrintCapChecksum` | `CAP_THREADS` 기본 256으로 모든 지원 loop에 MC 교대 선택을 적용한다. 1024 이하 공유 캐시와 초과 전역 map은 저장 위치만 다르며 해시 귀 선택은 제거했다. 진단에서 위치 원시 비트와 winding을 보존하는 순서 무관 cap checksum을 출력한다. 현재 구현과 검증은 [MC 통일 기록](../GPU/CapScanMarking.md)을 따른다. |
| 표시 | `Scene::Render`, `RenderCutPlanes` | 모델을 렌더하고 메인 투명 메시 뒤·후처리 전에 평면당 테두리·격자 22개 선을 그린다. 장면 깊이를 검사하며 깊이를 쓰지 않는다. |

## 소유권과 공간 계약

아래 모듈 경로는 저장소의 `MiniEngine/` 기준이다. 문서는 최상위 `Docs/`에 모아 둔다.

| 파일 / 자료형 | 소유 데이터와 책임 |
| --- | --- |
| `ModelViewer/Scene/Scene.*`의 `Scene`, `GameObject` | Scene이 객체를 소유한다. GameObject는 TRS의 원본이며 모델 없이도 존재한다. 모델이 있으면 ModelInstance를 가진다. |
| `ModelViewer/Scene/SkeletalPhysicsSystem.*` | 공통 PhysicsSystem의 world에서 owner별 rig를 등록·정리하고 Kinematic 목표, 동기 BreakConstraint, 배율을 복원한 Dynamic 본 override, 실제 상태 조회를 제공한다. |
| `ModelViewer/Scene/tests/SkeletalPhysicsChecks.h`의 `SkeletalPhysicsChecks` | fixture/Xbot을 실제 공통 fixed-step 경로로 구동하고 본·COM·관절 anchor overlay와 낙하·팔레트 수치 PASS/FAIL을 기록한다. |
| `-skeletal-break-checks 1` / `-skeletal-break-xbot 1` | 각각 3-body fixture / 기존 Xbot 왼팔을 60 tick 뒤 분리하고 180 tick에 판정 후 정상 종료한다. |
| `-skeletal-patch-break-checks 1` | `SkeletalPatchCopy::RunBreakChecks`가 실제 C 경로로 Xbot 전완·허벅지와 CesiumMan 팔의 JointId·Dynamic subtree·거절·pause를 검증한다. |
| `ModelViewer/Scene/tests/MeshComponentChecks.h`의 `SkeletalLifetimeChecks` | `-component-checks 1`에서 Scene clear·owner 삭제·지연 break 거절과 body·관절·pair·예약 0을 검사한다. |
| `ModelViewer/Scene/ObjectSelection.*`, `PickHit.h` | 원본 경계 구·생성 조각 hull 선택, 조각 중심 피벗, 객체/조각 ID와 월드 거리 비교를 제공한다. |
| `ModelViewer/Scene/TransformGizmo.*`, `SceneTransform.h` | 객체 TRS 편집과 입력 처리를 담당한다. 조작·Undo 계약은 [Scene.md](Scene.md)를 따른다. |
| `ModelViewer/Scene/CutPlaneSystem.*` | Scene을 참조하고 ObjectId별 `CutPlaneDesc`를 소유한다. 별도 평면 TRS를 보관하지 않는다. `Find`는 삭제된 객체를 거절한다. |
| `Model/ModelLoader.cpp`의 `BuildInfluencingJoints`, `Model::m_InfluencingJoints`, `Model::m_SkinCpuSections`, `Model/SkinCpuData.h` | 로드 시 양수 weight 여부를 NodeId별로 캐시하고 skin section만 위치·joint·weight를 정점당 28 B로 보존한다. Static CPU 정점 복사본은 없다. |
| `ModelViewer/Scene/SegmentJointSelection.h`, `SkeletalPatchCopy.*` | Parent의 영향 skin joint segment 부모·말단 선택과 Influence의 GPU 고정소수 투표·선택, GPU 패치 복사, 정적 owner 게시·격리, 무한 평면 재구축과 B 분리를 담당한다. |
| `ModelViewer/Scene/CuttingSystem.*` | 작업별 대상 snapshot과 `CutJob`을 보관하고 후보를 기본 2개까지 제출한다. 완료는 제출 순서대로 프레임당 하나씩 게시한다. [검증과 제한](../GPU/ConcurrentCutJobs.md) 참조. |
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

SP-7a/7a2: `SkeletalAutoRig.*`의 `BuildSkeletalPhysicsAsset`는 영향 palette 병합·E/배율/root 검증·등록 pose 정점 bounds box·anchor를 생성한다. leaf는 본 원점을 bounds에 포함하고 정점 없는 본만 segment fallback을 사용한다. `RunSkeletalAutoRigChecks`는 두 모델의 독립 애니메이션 5시점 anchor 오차·extent 범위·fallback 수·CPU 보관 비용을 TEMP에 남긴다. [결과](../GPU/SkeletalPatchBreak.md).

SP-7b: `SkeletalPatchCopy::Request`는 미등록 owner를 최종 pose로 지연 등록하고 `FindIncomingJoint`와 세대를 저장한다. A/B 준비 뒤 `QueueBreak`의 RequestId로 `TryGetBreakResult`를 소비하며 offset 복사본을 유지한다. 원본 숨김·node 부착은 아직 없다.

SP-7c: `Scene/Shaders/TriangleMaskCS.hlsl`의 `main`·`Replacement`는 누적 `FirstTriangle` State와 원본 IB에서 R16/R32 퇴화 인덱스를 재구성한다. `tests/TriangleMaskShaderTests.cpp`의 `main`은 두 R16 draw의 공유 DWORD·혼합 R32·마스크 해제를 독립 GPU readback으로 검증한다. `MeshComponent::SetTriangleMask`·`UpdateTriangleMask`·`ClearTriangleMask`는 원본 revision의 상태와 IB 구간을 컴포넌트별로 소유한다. `MeshSorter::AddMesh`·`RenderMeshes`는 VB와 IB 주소를 분리한다. `Scene/SkeletalMaskChecks.cpp`의 `RunSkeletalMaskChecks`는 두 모델의 joint weight 선택·치환 IB·공유 원본 불변·색상/깊이/그림자 ROI 소실·다른 컴포넌트 보존·상태 갱신/해제 복원을 검사한다.

SP-7d: `PatchExtractCS::main`·`ExtractPatchReference`는 전역 State의 제거 비트를 선택 조건에 반영하며 교차·미포함 카운터도 제거 삼각형을 제외한다. `SkeletalPatchJob::Submit`·`Poll`은 공유 정점 번호를 기준으로 인접 제거 삼각형을 소유 패치별로 집계·기록하며 `Validate`가 독립 CPU 집계와 비교한다. `CheckMaskedPatch`는 `RunSkeletalMaskChecks`에서 Xbot 부모 제거 후 자식 추출과 없음/0/해제 출력 바이트 불변·전체 제거의 빈 카운터를 검증한다.
