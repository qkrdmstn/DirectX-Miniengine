상태: 완료 — BreakConstraint M1(BC-00~BC-13a)과 master `913031f3` 병합 `32cc650a`; `f2138029` 다중 Static CutJob·`1532fc60` cap 단계 1 기록 유지, cap 결과 불변은 미확인 (2026-10-30); SP-5b 진단 복사본 절단 미커밋 동기화(2026-11-03); 전체 지원 loop MC 통일 `0553410b`(D/R 검증 완료); SP-6 Parent/Influence 독립 리뷰·blend-zone 포함 20회 진단 완료(미커밋, base `329aec7b`); SP-7a `731c7005` + SP-7a2 CPU 정점 보존·D2 bounds 구현·D/R 진단 검증(미커밋, base `731c7005`); SP-7b 지연 등록·incoming·요청별 break 결과 D/R 검증 완료(미커밋, base `18848ec0`); SP-7c 컴포넌트 마스크·렌더 연결 D/R 검증 완료(미커밋, base `d10077bd`); SP-7d D3 제거 제외·D6 인접 진단 D/R 검증 완료(미커밋, base `f83ecf1e`); SP-7e 게시·부착 구현(미커밋, base `9400691f`), 확장 진단 후속; 연속 캡처 시연·unmask 후속 C 거절·실패 종료·진단 옵션 격리(미커밋, base `03b5c8a1`, 2026-11-06); AR-4 `bb766e1b` 완료, Debug/Release 실행 PASS(2026-11-12); MI-3c 비동기 압축·실패 격리·완료 후 회수·몸체별 메타데이터 D/R 빌드·Release 위팔·회귀 PASS(미커밋, base `34d5199e`, 2026-11-16); MI-3d 몸체별 게시·부분집합 PG 연결(미커밋, base `96214e6f`, 2026-11-16); MI-3e2 실제 loop 판정·아일랜드 재라벨 및 리뷰 3건 수정 CPU/D/R/Release 회귀 PASS(미커밋, base `94f17ca0`, 2026-11-16); MI-3f cap loop 귀속·drop 제거 CPU/D/R/3데모/캡처/회귀 PASS(미커밋, base `63c2292b`, 2026-11-16); MI-4b 완료(`86522153`); MI-4c 완료(`caa6f681`); MI-4d1 요청 몸체 수 기반 압축·카운터·준비 ID 배열(미커밋, base `caa6f681`, 2026-11-16), MI-4d2 완료(`188adc01`), MI-4e 완료(`1d8c8ce2`), MI-4f `BreakConstraints` 완료(`61dd8e91`); MI-4g 완료(`f72f28c2`); MI-4h1 완료(`0cd2efba`, 리뷰 포함); MI-4h2 batch·mask1회·n+1 게시·거절 정리·캡처·CPU/D/R/회귀 PASS(완료 `e123cad1`, 2026-11-17); MI-5 시험·기록 완료(미커밋, base `e123cad1`, 2026-11-17): 실제 척추 평면·P=1 옵션 비교 포함18 PNG·Release 회귀6건 PASS, 몸통 접힘·양팔 틈·작은 파편 미해결

# 현재 Scene과 유한 평면 코드 지도

CS-1+CS-2 시퀀스 시연(완료, base `0112e7ba`, 2026-11-18): `ReleaseStageGate`·`OnCutResult`·`FinishCutResult`는 `ValidateStaging`으로 삭제된 준비물을 폐기한다. `SetStageDisplay`·`MeshComponent::Update`는 A 주황/B 보라와 명암을 표시하며, `Startup`은 시퀀스 캡처 디렉터리를 검사하고 `FocusBreakCamera`는 시퀀스 시연만 높인다. Release 실행에서 단계 1→4·캡처 4장·기존 시연 유지를 확인했다. [결정·진행 상태](../GPU/CutStageSequence.md).

MI-5(시험·기록 완료, 미커밋, MI-4 완료 `e123cad1`): 기존 외부 Release exe로 위팔491/167, 기본 c 팔161/170, 양 허벅지300/187/175·constraint18→16·mask1회, 실제 척추182/337을 확인했다. `Skeleton_torso_joint_2`의 P3→4는 단일 게시이며 옵션 on도 P=1로 결과가 같다. 기존 Torso 몸통 카메라 기록을 실제 척추 절단으로 대체했다. 위팔·기본 c·척추 옵션 비교 및 추가 실제 허벅지 단일243/169까지18 PNG를 직접 확인하고 회귀6건 PASS·D3D12 ERROR0을 기록했다. 몸통 복부 접힘, 척추 절단 양팔 틈(uncapped17·loop fallback34), 기본 c 작은 보라 파편을 수정 없이 남겼다. 다중 경계 측정·Applied 이후 복구·기본값 on 결정은 후속이다. 단일 constraint 감소1·patch ID 값1은 추론이며 타 세션 ModelViewer.cpp 변경은 보존했다([시험 표·재현·PNG·남은 일](../GPU/MeshIslandMultiBoneCut.md)).

MI-4h2(완료 `e123cad1`, base `0cd2efba`): `SkeletalPatchCopy::Request`는 P≥2인 명시 다중 경로에서 모든 incoming·세대를 고정한다. `FinishCutResult`가 숨긴 몸체 준비·`PrepareMultiPublication`의 전체 게시 용량/component 준비 및 `DiscardStaleCutResult` 검사 후 `PhysicsSystem::QueueBreaks`를 제출하고 `ConsumeMultiBreak`가 `TryGetBreakResults` 전부 Applied일 때 mask1회와 몸체별 Attachment·skin·`PublishVisible`을 게시한다. 빈 준비 ID는 건너뛰고 거절은 `DiscardPreparedBodies`로 폐기한다. 단일은 기존 `QueueBreak`/`TryGetBreakResult`와 값을 유지한다. 기본 off인 `-skeletal-multi-reject 1`은 마지막 기대 세대의 Stale 진단이다. Applied2·constraint18→16·세대+2·patch id1·300/187/175 게시·거절 무변경·허벅지/몸통 시점 PNG·CPU/D/R/회귀 PASS([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).


MI-4h1(완료 `0cd2efba`, MI-4g `f72f28c2` 기반): `SkeletalPatchCopy::Request`는 기본 off인 `-skeletal-multi-cut 1`에서 `SelectMultiCut`/`SelectCrossingJoints`의 P가 2개 이상이면 합집합을 선택한다. `BuildCutBodyGroups`가 전체 P의 attachment·snapshot·Frozen을 구성하고 `PrepareMultiBodies`가 몸체별 압축 section의 숨긴 component와 `BuildPieceSkin(body)`를 준비한다. P13/P17 몸체300/187/175·skin PASS, A attach=LCA 2·B attach=13/17이다. `ReadOriginalState`로 준비 전후 원본/mask/constraint 불변을 확인하고 `-skeletal-multi-stale 1`은 저장 revision 불일치의 기존 `DiscardStaleCutResult` 정리만 검사한다. h1 시점에는 준비 후 모두 폐기했으며 h2에서 batch 게시를 연결했다. `SelectMultiCut`은 모서리까지 component-local로 변환하며, `DiscardPreparedBodies`가 성공·실패·Stale 정리 후 완료 기록을 보장한다. `-skeletal-multi-fail 1`은 두 번째 몸체 skin 거절 뒤 부분 생성물 정리를 검사한다. 실제 batch Applied 확인·mask 1회·빈 몸체 제외 n+1 게시·batch 거절·캡처는 MI-4h2다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).


MI-4g(완료 `f72f28c2`, base `61dd8e91`, 2026-11-17): `PhysicsSystem::QueueBreaks`가 기대 세대를 포함한 요청 배열을 복사하고 batch당 RequestId를 발급한다. `ProcessQueuedBreaks`는 fixed step에서 batch당 `SkeletalPhysicsSystem::BreakConstraints`를 한 번 호출하고 요청당 Processed/NotFound를 누적한다. `TryGetBreakResults`는 순서대로 결과를 한 번 소비하며 기존 `QueueBreak`·`TryGetBreakResult`는 단일 래퍼다. pause는 대기하고 Reset의 `CancelBreak`는 batch 전체를 폐기한다. `QueuedBreakChecks::Run`은 기존 skeletal 런타임 검사에서 무변경 snapshot·같은 rig 성공 세대 +2를 검사한다. 실제 다중 P 호출·mask 1회·n+1 게시는 MI-4h다.

MI-4f(`61dd8e91`): `SkeletalPhysicsSystem::BreakConstraints`는 요청 전체를 `PrepareBreak`로 검증하고 `BreaksConflict`(Remove/Enable 관절·body 공유)가 있거나 하나라도 거절되면 아무것도 commit하지 않으며(나머지는 `BreakStatus::BatchRejected`), 통과하면 `CheckCommittable`(`PhysicsWorld::CanCommitConstraintBreak` 포함)로 전체 handle 생존을 확인한 뒤 `CommitPrepared(…, false)`로 순차 commit한다. 공개 `CommitBreak`는 준비 뒤 `TopologyGeneration`이 바뀌었으면 Stale이다.
MI-4e(`1d8c8ce2`): `SkeletalPhysicsSystem::BreakConstraint`는 `PrepareBreak`(rig·world 변경 없음, 통과 시 `BreakStatus::Prepared`와 `PreparedBreak`)와 `CommitBreak`(rig 세대·관절 생존 재확인, 성공마다 `TopologyGeneration` 증가; MI-4f부터 준비 뒤 topology 세대 변경도 Stale)를 잇는 얇은 래퍼다. `PhysicsWorld::ApplyConstraintBreak`도 `PrepareConstraintBreak`(`PreparedConstraintBreak` snapshot)와 `CommitConstraintBreak`로 나뉘며 외부 동작은 같다. batch 사전 검증·세대 계약은 MI-4f, `QueueBreak` batch·게시 연결은 MI-4g~4h다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-4d2(base `58807a5e`, 미커밋): `DiagnoseIslands`가 `SkeletalPatchBuffers::Crossings`에 P_i별 교차점 snapshot을 만들고 `RelabelCapLoops`가 교차점마다 최소 면적 loop를 골라 loop 공유·아일랜드 충돌은 다중일 때 예외로 격리한다. `DiagnoseCapLoops(bodyCount)`는 side별 몸체 유지 loop 수와 B1/B2 cap 분리를 로그한다. `CheckJointUnion`이 반환한 형제 P 쌍을 `-skeletal-union-checks`일 때만 `Request`가 3몸체로 제출하며 `FinishCutResult`는 압축 확인 뒤 게시·break·mask 없이 끝낸다. 몸체별 skin 준비는 미연결.

MI-4d1(`58807a5e`): `ClassifyCutBodies`·`PatchBodyClassifyCS::main`은 요청 BodyCount로 몸체별 표면/cap 카운터를 기록한다. `CompactCutBodies`는 라벨 최댓값 대신 요청 몸체 수로 bin을 할당해 마지막 빈 몸체를 보존하고 `PollBodyCompaction`이 빈 몸체를 명시적으로 로그한다. `m_PreparedIds`는 벡터이며 `FinishCutResult`의 게시 연결은 기존 두 몸체만 허용한다. `CheckBodyCompaction`의 합성 세 몸체/빈 body3 검사는 union 옵션에서도 실행한다. P_i별 교차점·실제 loop 귀속·n=2 절단/분류/압축 진단과 몸체별 staging/skin 준비는 MI-4d2, batch break·mask·n+1 게시는 MI-4e~4h다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-4c(완료 `caa6f681`): `BuildCutBodyGroups`가 노드 그룹·P_i 부모 LCA의 A 부착·P_i의 B_i 부착·Frozen=(group!=body)를 만든다. `Request`는 몸체별 attachment/snapshot/piece ID와 side 매핑 벡터를 사용하고 `DiagnoseIslands`도 그룹 분류를 공유한다. 기본 입력은 기존 단일 P이며 압축·게시 자원 n+1 확장과 break batch는 후속 범위다. CPU118 checks와 `CheckBodyPalettes`의 형제 P13/P17 후속 포즈 팔레트(A/B1/B2 attach=2/13/17, Frozen=8/15/15) PASS; n=1 세 데모·CPU/D/R·Release 회귀 PASS([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-4b(완료 `86522153`): `BuildJointUnionMasks`는 P 집합의 각 본 자체만 선택한 기존 단일 선택 마스크를 OR하여 section별 팔레트 마스크 하나로 합친다. 자손은 자동 포함하지 않는다. `SkeletalPatchJob::Submit`의 선택적 unionRoots가 기존 `PatchExtractCS::Selected`·prefix 경로를 재사용하고 기존 단일 본 호출은 유지한다. `CheckJointUnion`은 `-skeletal-union-checks 1`에서 MI-4a로 고른 CesiumMan 형제 허벅지 P13/17의 단독·합집합·중복 P 추출을 `Validate`의 CPU 출처 및 중복 검사와 대조한다(364/352→526, 공유190, 중복0; 자손이 있는 P13의 기존 마스크 경로와 n=1 unionRoots 출처 목록·순서 비교). 진단은 다중 절단·break·mask 게시에 연결하지 않는다. CPU/D/R/세 데모/Release 회귀 PASS([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-3f(base `63c2292b`, 미커밋): `DiagnoseCapLoops`가 leader별 side 몸체/drop 표를 반환하고 `ClassifyCutBodies`가 GPU에 업로드한다. `PatchBodyClassifyCS::main`은 cap Origin.y를 조회해 keep을 해당 side 테두리 몸체에 보내며 drop은 UINT_MAX-1, 미해결 대체는 UINT_MAX로 구별한다. `PatchBodyCompactCS::main`은 drop을 count/scatter/bounds 전에 제외한다. `BodyCompactJob`의 제거 수로 `PollBodyCompaction`은 출력+drop=입력을 검사한다. 위팔 cap54 제거·body491/167·fallback0, 게시/+60 PNG 확인, CPU/D/R 및 Static/PS/AR 회귀 PASS([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-3e2(base `94f17ca0`, 미커밋): `Submit`은 선택 ChildNodeId를 보존하고 `DiagnoseIslands`는 제출 prepared nodes·component world로 P→그 자식 교차점을 고정한다. 자식 snapshot이 없으면 직접 자식 중 평면 중심에 가장 가까운 교차를 고르며 `IntersectLatticeSegment`가 끝점 허용오차와 방향 기반 B side를 처리한다. `RankBoundaryCS::main`이 CapEars 전 평면 2D 선분까지 32B snapshot으로 보존한다. `OnCutResult`에서 `RelabelCapLoops`가 `SmallestContainingLatticeLoop`로 교차점을 감싼 최소 면적 유효 loop 하나를 고르고 `RelabelPatchIslands`로 접촉 아일랜드 전체와 GPU 표·SideDefault를 갱신한 뒤 MI-3e 진단→MI-3a 분류→MI-3c 압축을 실행한다. 미접촉/충돌은 다수결이며 실제 cap 귀속·제거는 MI-3f에서 연결했다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-3d(base `96214e6f`, 미커밋): `OnCutResult`는 절단 결과를 보관하고 `Update`가 압축 완료를 조회한 뒤 `FinishCutResult`에서 몸체별 section과 `BuildPieceSkin(body)`를 준비한다. body 0=A, 1=B이며 break Applied 뒤 원본 mask와 A/B를 게시한다. 실패·빈 몸체·양쪽 동일 SideDefault는 기존 side 경로로 폴백한다. `ActivatePatchSkin`은 SourceMesh 번호의 PG offset을 유지하고 `UpdateSkinnedGenerated`는 빈 section 바인딩을 건너뛴다. `BakePatchSkin`은 몸체 section 부분집합을 보존한다. 게시 중인 공유 압축 버퍼는 `DiscardBodyCompaction`에서 직접 해제하지 않는다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-3c(base `34d5199e`, 미커밋): `SkeletalPatchCopy::CompactCutBodies`가 `OnCutResult`의 실제 양쪽 geometry·몸체 버퍼로 count→GPU prefix→scatter job을 실행한다. `BodyCompactJob`이 입력·출력·scratch·readback을 유지하고 `PollBodyCompaction`이 후속 `Update`에서 완료된 작은 표를 수집한다. `GetBodyState`/`GetBodyResult`로 MI-3d가 완료를 조회하며 `DiscardBodyCompaction`은 완료된 미게시 자원을 직접 해제하고 미완료 작업은 지연 회수한다. 분류·압축 실패는 기존 A/B 게시와 격리한다. GPU AABB를 검증하고 `PatchBodyResult`에 공유 geometry·몸체별 Mesh section 목록·삼각형 수를 보관한다. body0=518/body1=194·합계 712 및 범위/bounds PASS; D/R 빌드·Release 회귀 PASS. 게시·몸체별 skin은 MI-3d에서 연결했다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-3b(base `2a0ab7d2`, 미커밋): `PatchBodyCompactCS::main`·`BuildOffsets`가 body×section count→offset→scatter를 수행한다. `CheckBodyCompaction`은 기존 break 검사 옵션에서 두-side 합성 정점/depth/provenance/IB·합계·overflow를 검증한다. 실제 job은 MI-3c에서 연결했고 게시는 MI-3d에서 연결했다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

MI-3a(미커밋, base `b7413004`, 2026-11-16): `SkeletalPatchCopy::OnCutResult`가 skin 복원 전 `ClassifyCutBodies`를 cutter side 순서로 호출한다. `PatchBodyClassifyCS::main`은 provenance `TriangleId`와 GPU `TriangleSideBody` 표로 몸체를 기록하고 cap·invalid·ID 불일치·재배치 카운터만 readback한다. side별 삼각형 몸체 버퍼는 MI-3b용으로 보관하며 출력·게시는 불변이다. D/R 빌드·Release 회귀 PASS, 위팔 invalid/idMismatch=0·side0 reassigned=104([결정·로그](../GPU/MeshIslandMultiBoneCut.md)).

MI-2(base `432ff0c5`, 2026-11-13): `SkeletalPatchJob::Poll`이 기존 TriangleSources readback을 재사용한다. `Submit`의 준비 포즈 snapshot과 실제 확대 평면으로 `DiagnoseIslands`가 패치 참조 정점만 중복 없이 스키닝하고 joint→그룹 표를 요청당 한 번 만든다. `LabelPatchIslands`는 출력 없는 side를 제외하고 (셀, side) 연결과 원본 세 꼭짓점 전체의 bind weight 투표를 분리해 몸체 표를 job/패치에 보관한다. GPU 출력에는 적용하지 않는다. CPU 테스트·D/R 빌드·Release 회귀 PASS; CesiumMan 위팔 P7에서 3개 아일랜드 중 허리 90 tris가 A로 재배치되는 라벨을 확인했다([결정·로그](../GPU/MeshIslandMultiBoneCut.md)).

MI-1(base `2a1620cd`, 2026-11-13): `BuildInfluencingJoints`는 skin section에 draw 로컬 삼각형 인덱스를 보존한다. `BuildLatticeTopology`가 bind 최장축 64칸의 점유 셀·삼각형 간선을 생성해 `Model::m_LatticeTopology`에 보관한다. `LatticeUnionFind`·`CountLatticeComponents`는 순수 CPU 연결 성분 계산이며 절단 출력은 변경하지 않는다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

스켈레탈 연속 캡처 시연 옵션 추가(미커밋, base `03b5c8a1`, 2026-11-06): `ModelViewer::Update`가 `BuildSkeletalBreakPlane`을 재사용해 Xbot P60(`-skeletal-break-demo 1`) 또는 CesiumMan P8(`2`)을 정상 C 요청한다. `FocusBreakCamera`로 한 번 근접 배치하고 Applied 게시부터 20프레임 간격으로 HDR/post 8장을 저장한다. `-skeletal-break-demo-unmask 1`은 `SetTriangleMask`를 생략하므로 첫 게시 후 수동 C를 포함한 후속 요청을 거절한다. 요청 거절·게시 실패는 오류 로그 후 종료하며 자동 검사·벤치 옵션 동시 지정은 Startup에서 거절한다. 호출자 Release 관찰은 [SP-7 문서](../GPU/SkeletalPatchBreak.md)에 기록했고 이번 수정은 빌드·실행하지 않았다.

현재 구현은 객체 선택·TRS 편집, 유한 평면의 기하·격자 표시와 Static GPU 절단·cap·재절단을 포함한다. 유한 영역을 넘는 교차는 후보 전체를 거절한다. 원본 Skeletal 절단은 SP-7e 게시·부착 단계이며 경계 확장 진단과 교차 미리보기는 미완료다. SP-5b(미커밋)는 영향 패치의 정적 복사본만 무한 평면으로 절단하고 B를 월드 0.1 m 분리한다([설계·검증](../GPU/SkeletalPatchCopyCut.md)). 과거 CPU 패치 추출·슬라이스·cap 경로는 현행 코드에 없다.

> 메모 (2026-11-04): CI-1a `62b5228c`에 이어 CI-1b는 Static 절단 입력을 revision Data와 section 행렬로 전환했다. `GpuMeshCutter::{PlanCapacity,Submit}`은 같은 clip shader와 행렬 snapshot을 쓰며 t1은 제거했다. CI-2로 `-cut-validate` 진단도 PG 없이 Data와 section 행렬로 부모를 재구성한다. CI-3으로 생성 조각 PG는 GB::Pose 복사 대신 Data를 한 번 디코드해 채운다. CI-4·5로 `GeometryBuffers::Pose`를 없애 생성 조각은 Data(28 B)·Depth·IB만 가진다. D/R 빌드·회귀 실행을 통과했으며 후속 단계는 [CutInputFromRevision.md](../GPU/CutInputFromRevision.md)를 따른다.

## 실행 흐름

| 순서 | 함수 | 역할 |
| --- | --- | --- |
| 초기화 | `ModelViewer::Startup`, `CutPlaneSystem::CreatePlane`, `RegisterAutoRig` | Scene에 모델 객체와 원점의 4×4 XZ 평면을 만든다. 최종 배치·pose 준비 뒤 skin owner의 rig를 사전 등록한다. 시작 선택은 없다. |
| 갱신 | `Scene::Update`, `CutPlaneSystem::PruneStaleObjects` | 모델 자세를 갱신하고 일반 Scene 삭제로 남은 평면 설명을 정리한다. |
| 선택 | `ModelViewer::PickAtCursor`, `PickObject`, `CutPlaneSystem::Pick` | 생성 조각 hull·원본 경계 구·유한 평면을 월드 거리로 비교한다. 스킨 메시 후보와 평면이 겹치면 `ReadCutPlanePickDepth`로 클릭 픽셀의 가림을 확인한다. |
| 편집 | `TransformGizmo`, `GetEditTargetPivot`, `GizmoMath::KeepPivot` | 생성 Mesh의 경계 중심으로 기즈모를 표시·편집하고 노란 hull 모서리로 선택을 표시한다. 평면 크기 모드는 로컬 X/Z만 바꾼다. |
| 스냅샷 | `CutPlaneSystem::BuildData`, `TryBuildCutPlaneData` | 평면 설명과 현재 GameObject TRS로 기하 데이터를 계산한다. |
| C 키 Skeletal 선택 | `SkeletalPatchCopy::Request`, `SelectRigSegment` | 기본 Rig 모드로 Jolt hit의 kinematic segment를 고른다. segment가 없으면 break 없는 부착 패치 게시 뒤 AR 재절단을 요청한다. 경계 구 + `SelectSegmentJoint`는 rig 미등록 owner fallback·Parent 진단 모드로 남는다. |
| H 키 충돌 표시 | `PhysicsWorld::CollectDebugLines`, `CollisionDebugDraw::Draw` | Jolt `DebugRendererSimple`의 body·관절 선을 기존 hull overlay와 함께 표시한다(`JPH_DEBUG_RENDERER`). |
| cap·진단 | `CapEarsCS::main`, `CutJob::ValidateDiagnostics`, `PrintCapChecksum` | `CAP_THREADS` 기본 256으로 모든 지원 loop에 MC 교대 선택을 적용한다. 1024 이하 공유 캐시와 초과 전역 map은 저장 위치만 다르며 해시 귀 선택은 제거했다. 진단에서 위치 원시 비트와 winding을 보존하는 순서 무관 cap checksum을 출력한다. 현재 구현과 검증은 [MC 통일 기록](../GPU/CapScanMarking.md)을 따른다. |
| 표시 | `Scene::Render`, `RenderCutPlanes` | 모델을 렌더하고 메인 투명 메시 뒤·후처리 전에 평면당 테두리·격자 22개 선을 그린다. 장면 깊이를 검사하며 깊이를 쓰지 않는다. |

## 소유권과 공간 계약

아래 모듈 경로는 저장소의 `MiniEngine/` 기준이다. 문서는 최상위 `Docs/`에 모아 둔다.

| 파일 / 자료형 | 소유 데이터와 책임 |
| --- | --- |
| `ModelViewer/Scene/Scene.*`의 `Scene`, `GameObject` | Scene이 객체를 소유한다. GameObject는 TRS의 원본이며 모델 없이도 존재한다. 모델이 있으면 ModelInstance를 가진다. |
| `ModelViewer/Scene/SkeletalPhysicsSystem.*` | 공통 PhysicsSystem의 world에서 owner별 rig를 등록·정리하고 Kinematic 목표, 동기 BreakConstraint, 배율을 복원한 Dynamic 본 override, 실제 상태 조회를 제공한다. |
| `Physics/PhysicsWorldStep.cpp`, `ModelViewer/Scene/SkeletalPhysicsSystem.*` | `CollideOrientedBox`로 Moving body를 조회하고, `FindBody`로 owner·body·node를 찾으며, `BuildKinematicSegments`로 살아 있는 kinematic constraint의 world 끝점을 만든다. |
| `ModelViewer/Scene/SkeletalPatchCopy.*`, `PhysicsSimulation.cpp` | `RegisterAutoRig`는 로드·C 입력 공용 등록이다. `RefreshPreloadedRig`는 배율 변경 시 끊김 없는 rig를 재등록한다. rig kinematic body는 sensor이며 break 시 해제하고, 이미 겹친 Dynamic 쌍은 분리까지 임시 충돌 제외한다. |
| `ModelViewer/Scene/tests/SkeletalPhysicsChecks.h`의 `SkeletalPhysicsChecks` | fixture/Xbot을 실제 공통 fixed-step 경로로 구동하고 본·COM·관절 anchor overlay와 낙하·팔레트 수치 PASS/FAIL을 기록한다. |
| `-skeletal-break-checks 1` / `-skeletal-break-xbot 1` | 각각 3-body fixture / 기존 Xbot 왼팔을 60 tick 뒤 분리하고 180 tick에 판정 후 정상 종료한다. |
| `-attached-recut-checks 1` | `AttachedRecutChecks::Tick`·`OnResult`가 Xbot break 뒤 두 결과, 60프레임 추종·낙하, 모호한 node 측 거절, 독립 조각 추가 Static 절단을 검사하고 종료 코드 0/1을 반환한다(AR-4 `bb766e1b`, Debug/Release 다섯 판정 PASS, 2026-11-12). |
| `-rig-bone-cut-checks 1` | `RigBoneCutChecks`가 break 없는 뼈 영역 절단·60프레임 추종·KDOP 낙하·1 mm 모호 거절·강제 실패 보존을 검사한다. Debug/Release RBC PASS(`dafd19ac`, 2026-11-12). |
| `-skeletal-patch-break-checks 1` | `SkeletalPatchCopy::RunBreakChecks`가 실제 C 경로로 Xbot 전완·허벅지와 CesiumMan 팔의 JointId·Dynamic subtree·거절·pause를 검증한다. |
| `ModelViewer/Scene/tests/MeshComponentChecks.h`의 `SkeletalLifetimeChecks` | `-component-checks 1`에서 Scene clear·owner 삭제·지연 break 거절과 body·관절·pair·예약 0을 검사한다. |
| `ModelViewer/Scene/ObjectSelection.*`, `PickHit.h` | 원본 경계 구·생성 조각 hull 선택, 조각 중심 피벗, 객체/조각 ID와 월드 거리 비교를 제공한다. |
| `ModelViewer/Scene/TransformGizmo.*`, `SceneTransform.h` | 객체 TRS 편집과 입력 처리를 담당한다. 조작·Undo 계약은 [Scene.md](Scene.md)를 따른다. |
| `ModelViewer/Scene/CutPlaneSystem.*` | Scene을 참조하고 ObjectId별 `CutPlaneDesc`를 소유한다. 별도 평면 TRS를 보관하지 않는다. `Find`는 삭제된 객체를 거절한다. |
| `Model/ModelLoader.cpp`의 `BuildInfluencingJoints`, `Model::m_InfluencingJoints`, `Model::m_SkinCpuSections`, `Model/SkinCpuData.h` | 로드 시 양수 weight 여부를 NodeId별로 캐시하고 skin section만 위치·joint·weight를 정점당 28 B로 보존한다. Static CPU 정점 복사본은 없다. |
| `ModelViewer/Scene/SegmentJointSelection.h`, `SkeletalPatchCopy.*` | 기본 Rig의 `SelectRigSegment`, Parent의 영향 skin joint segment 부모·말단 선택과 Influence의 GPU 고정소수 투표·선택, GPU 패치 복사, 숨긴 A/B 준비·성공 후 원래 owner 게시, 본 부착 snapshot과 선택적 offset 진단을 담당한다. `PublishRigBonePatch`는 segment 없는 hit를 break 없이 부착 게시·마스크 처리하고 후보 준비 뒤 AR 재절단에 연결한다([설계·결과](../Physics/RagdollRigPreload.md)). |
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

SP-7b: `SkeletalPatchCopy::Request`는 미등록 owner를 최종 pose로 지연 등록하고 `FindIncomingJoint`와 세대를 저장한다. A/B 준비 뒤 `QueueBreak`의 RequestId로 `TryGetBreakResult`를 소비한다. 원본 숨김·node 부착은 아래 SP-7e 게시 단계로 이어진다.

SP-7c: `Scene/Shaders/TriangleMaskCS.hlsl`의 `main`·`Replacement`는 누적 `FirstTriangle` State와 원본 IB에서 R16/R32 퇴화 인덱스를 재구성한다. `tests/TriangleMaskShaderTests.cpp`의 `main`은 두 R16 draw의 공유 DWORD·혼합 R32·마스크 해제를 독립 GPU readback으로 검증한다. `MeshComponent::SetTriangleMask`·`UpdateTriangleMask`·`ClearTriangleMask`는 원본 revision의 상태와 IB 구간을 컴포넌트별로 소유한다. `MeshSorter::AddMesh`·`RenderMeshes`는 VB와 IB 주소를 분리한다. `Scene/SkeletalMaskChecks.cpp`의 `RunSkeletalMaskChecks`는 두 모델의 joint weight 선택·치환 IB·공유 원본 불변·색상/깊이/그림자 ROI 소실·다른 컴포넌트 보존·상태 갱신/해제 복원을 검사한다.

SP-7d: `PatchExtractCS::main`·`ExtractPatchReference`는 전역 State의 제거 비트를 선택 조건에 반영하며 교차·미포함 카운터도 제거 삼각형을 제외한다. `SkeletalPatchJob::Submit`·`Poll`은 공유 정점 번호를 기준으로 인접 제거 삼각형을 소유 패치별로 집계·기록하며 `Validate`가 독립 CPU 집계와 비교한다. `CheckMaskedPatch`는 `RunSkeletalMaskChecks`에서 Xbot 부모 제거 후 자식 추출과 없음/0/해제 출력 바이트 불변·전체 제거의 빈 카운터를 검증한다.

SP-7e: `SkeletalPatchCopy::Request`는 C snapshot·side를 보관하고 `OnCutResult`는 숨긴 A/B 준비 뒤 break를 요청한다. `Update`는 Applied·revision 일치 후 원본 mask와 원래 owner의 A/B를 함께 게시한다. `Reset`은 `PhysicsSystem::CancelBreak`로 미실행 요청도 취소한다. `Scene::PrepareMeshes`는 최종 node pose로 부착을 갱신한다. `PhysicsSystem::IsEligible`은 부착 조각도 Static 절단 후보로 허용하고 `SyncBodies`는 부착 조각의 body 등록을 제외한다. `ResolveEditTarget`은 owner 선택을 유지한다. offset·B 간격은 `-skeletal-patch-offset-copy 1` 전용이다. 경계 오차·강제 Stale·캡처 진단은 미완료다.
