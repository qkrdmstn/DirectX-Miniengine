상태: 완료 — BreakConstraint M1(BC-00~BC-13a)과 master `913031f3` 병합 `32cc650a`; `f2138029` 다중 Static CutJob·`1532fc60` cap 단계 1 기록 유지, cap 결과 불변은 미확인 (2026-10-30); SP-5b 진단 복사본 절단 미커밋 동기화(2026-11-03); 전체 지원 loop MC 통일 `0553410b`(D/R 검증 완료); SP-6 Parent/Influence 독립 리뷰·blend-zone 포함 20회 진단 완료(미커밋, base `329aec7b`); SP-7a `731c7005` + SP-7a2 CPU 정점 보존·D2 bounds 구현·D/R 진단 검증(미커밋, base `731c7005`); SP-7b 지연 등록·incoming·요청별 break 결과 D/R 검증 완료(미커밋, base `18848ec0`); SP-7c 컴포넌트 마스크·렌더 연결 D/R 검증 완료(미커밋, base `d10077bd`); SP-7d D3 제거 제외·D6 인접 진단 D/R 검증 완료(미커밋, base `f83ecf1e`); SP-7e 게시·부착 구현(미커밋, base `9400691f`), 확장 진단 후속; 연속 캡처 시연·unmask 후속 C 거절·실패 종료·진단 옵션 격리(미커밋, base `03b5c8a1`, 2026-11-06); AR-4 `bb766e1b` 완료, Debug/Release 실행 PASS(2026-11-12); MI-3c 비동기 압축·실패 격리·완료 후 회수·몸체별 메타데이터 D/R 빌드·Release 위팔·회귀 PASS(미커밋, base `34d5199e`, 2026-11-16); MI-3d 몸체별 게시·부분집합 PG 연결(미커밋, base `96214e6f`, 2026-11-16); MI-3e2 실제 loop 판정·아일랜드 재라벨 및 리뷰 3건 수정 CPU/D/R/Release 회귀 PASS(미커밋, base `94f17ca0`, 2026-11-16); MI-3f cap loop 귀속·drop 제거 CPU/D/R/3데모/캡처/회귀 PASS(미커밋, base `63c2292b`, 2026-11-16); MI-4b 완료(`86522153`); MI-4c 완료(`caa6f681`); MI-4d1 요청 몸체 수 기반 압축·카운터·준비 ID 배열(미커밋, base `caa6f681`, 2026-11-16), MI-4d2 완료(`188adc01`), MI-4e 완료(`1d8c8ce2`), MI-4f `BreakConstraints` 완료(`61dd8e91`); MI-4g 완료(`f72f28c2`); MI-4h1 완료(`0cd2efba`, 리뷰 포함); MI-4h2 batch·mask1회·n+1 게시·거절 정리·캡처·CPU/D/R/회귀 PASS(완료 `e123cad1`, 2026-11-17); MI-5 시험·기록 완료(미커밋, base `e123cad1`, 2026-11-17): 실제 척추 평면·P=1 옵션 비교 포함18 PNG·Release 회귀6건 PASS, 몸통 접힘·양팔 틈·작은 파편 미해결; CP-4b 옛 마스크 경로 제거·CPU 압축/R16·Release 회귀/HDR 일치 완료(`3e16d25a`, 2026-11-20); NDC 클립 전환 녹화 재현 완료(기준 `049c3579` 위 본 문서 포함 커밋, 2026-11-25); 진행 중 — CP-1 측정, CP-2 7367a00a, CP-3 415fa819, CP-4a ad5c74cd, CP-4b 3e16d25a, CP-5 (커밋 대기); 2026-11-25(base `547587a9`, 미커밋): 두 데모의 절단 시 자동 카메라 초점 제거, 자동 검사 프레이밍 유지; CesiumMan tilt 녹화 옵션 추가(기준 `1ec89f9f`, 미커밋, 2026-11-25); LW 비교 녹화 본 위치 옵션(기준 `5672bb44`, 미커밋, 2026-11-25); G0 기준값 일부 측정·다중 허벅지 실패/DXGI 오류 후속(미커밋, 2026-11-26); G1a 분류 표 업로드 동기 대기 제거(미커밋, base `959b1bbc`, 2026-11-26); G2~G4 최종 검증(base `2c89992d`, 2026-11-26): 통합 Debug 빌드·LatticeTopologyTests 2011 PASS, 척추 off/on 동등·classify PASS; Xbot 다중 P 기존 충돌·Debug off/on 정렬 assertion으로 전체 완료 미충족, 성능 비교 기록·기본 GPU 0·CPU oracle 매 요청 유지(절감 없음)·G5 보류

# 현재 Scene과 유한 평면 코드 지도

ST-1~ST-4 구현·검증 완료 (ST-1 89e1ff81, ST-2 007c9306, ST-3 2411cd56, ST-4 3575718b), 기능 전체는 진행 중: `SkeletalPatchCopy::Request`가 요청 시점 분리 입력을 보관하고 `PublishDetachRecord`가 성공한 단일·다중 게시에만 root별 owner 이력을 추가한다. `ApplyDetachRecord`는 기존·새 패치의 아직 Frozen이 아닌 슬롯만 고정하며 `ApplyRecutDetachHistory`는 RS 자식에 전체 이력을 적용한다. `PruneDetachHistory`는 삭제 owner/만료 모델만 회수하고 `Reset`은 살아 있는 owner의 게시 이력을 보존한다. `ReadDetachCheckBytes`는 Joint 법선 행렬의 미사용 SIMD lane과 패딩을 제외한 값 필드만 비교한다. `-detach-history-checks 1`은 `RunBreakChecks`·`VerifyStale`에서 snapshot/이력 비교와 추가 Request 제출·Reset 취소를 수행하는 선택적 검증 인자다. 인자 생략 시 추가 요청은 없고 quiet=1이면 직렬화·할당·비교와 추가 요청도 건너뛴다. 미완료: 전체 모델·본 단일 절단 회귀(완료 조건 5), 사용자 육안 확인(완료 조건 6). 3회 절단·정역순·AF off·LB on·RS/AR4·D3D12 ERROR 0 PASS, 기존 다중 batch loop 실패와 검증 한계는 [결정·결과](../GPU/PatchStaleFrozenJoints.md)를 따른다.

CutPerf 배포(2026-11-26, base `ba8299ce`, 미커밋): `Tools/CutPerf/Pack-CutPerf.ps1`은 Release 실행 파일·런타임과 기본 장면 에셋을 다른 Windows PC용 zip으로 묶는다.
기본 출력은 `%TEMP%\CutPerfPackage-<시각>.zip`이며 `-OutZip`으로 바꾼다. 패키지의 한국어 README에 실행과 결과 회수 방법이 있다.
`Run-CutPerf.ps1 -ExtraArgs "-island-gpu 1"`은 모든 실행에 옵션을 추가하고, `-WorkDir`은 에셋 폴더를 지정한다(배포본 기본값 `work`).
기존 결과 형식을 유지하며 `machine.txt`와 `commands.txt`에 장비·실행 파일·실제 명령행을 남긴다. git 없는 PC에서는 저장소 정보를 `n/a`로 기록한다.

RS-1~RS-4(base `a4b9ea23`, 브랜치 `patch-recut-keep-skin`, 미커밋): 부착 스키닝 패치 재절단에서 node 쪽 결과가 스키닝을 유지한다(`-patch-recut-skin 0`이면 기존 bake·정적). `MeshComponent::Update`가 마지막 N을 보관하고 후보·ActiveCut이 부모 `PatchSkin`과 `N·D`(RecutKFrame)를 들고 있다. `PatchSkinRecutCS`가 부모 skin 레코드를 복사 정점은 통째로, split·cap 정점은 슬롯 K weight 1로 만들고 `BuildRecutPatchSkinVertices`가 section·BindBounds를 재계산한다. `MeshComponent::BuildSkinnedPose`(준비 단계 helper)와 `PreparedGeometryReplacement::PatchSkin`이 게시 때 skin을 noexcept로 승계한다. 진단은 `AttachedRecutChecks::SkinnedFollowError`(K 정점 추종, AR4 #2·RigBoneCut 공용)와 `-attached-recut-checks 2`의 연속 재절단 `RS4` 판정이다([결정·검증](../GPU/PatchRecutSkinning.md)).

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

G0(진행 중, 미커밋, base `d5e94361`, 2026-11-26): `CutPerf::Cpu`가 `DiagnoseIslands`·`LabelPatchIslands`·cap 재라벨·분류/skin 동기 대기를 요청별 sum/count/mean으로 기록한다. `Run-CutPerf.ps1 -G0 -N 3`은 Release 전체 빌드 후 14/15 실행·26요청을 측정했다. 다중 허벅지 런타임 실패로 기준값이 없고 Release validation은 D3D12 ERROR 0/DXGI ERROR 1이다. 계측 설계·엔진 동작은 변경하지 않았다([기준표·근거](../GPU/GpuIslandPipeline.md)).

G1a(진행 중, 미커밋, base `959b1bbc`, 2026-11-26): `ClassifyCutBodies`는 job 소유 `m_IslandTable`·`m_CapLoopTable`을 초기 데이터 없이 생성하고, 분류 context의 `ReserveUploadMemory`·`CopyBufferRegion`으로 복사한 뒤 SRV로 전이한다. 빈 side 0도 복사를 비동기 제출해 side 1 입력을 보존한다. 두 `ClassifyCutBodies-upload-wait` 구간은 남기고 counter 동기 회수는 G1b로 유지한다([측정·검증](../GPU/GpuIslandPipeline.md#g1a-결과)).

G1b(진행 중, 미커밋, base `d6c84d3a`, 2026-11-26): `BodyCompactJob`이 분류 counter/readback과 입력·표·몸체 리소스 참조를 소유한다. `ClassifyCutBodies`는 `Finish(false)`로 제출하며 `PollBodyCompaction`이 같은 DIRECT 큐의 압축 fence 완료 후 로그·Dropped를 회수한다. 취소·예외는 최근 제출 fence까지 retired job으로 보호하고 합계·invalid-body 실패 검증은 유지한다. counter-wait 52개 기록 0.000 ms, D3D12 ERROR 0([측정·검증](../GPU/GpuIslandPipeline.md#g1b-결과)).

G2a(최종 통합 Debug 빌드·CPU 재검증 PASS, 척추 verify PASS·Xbot 다중 게시 미검증; 완료, 미커밋, base `e2aadbe7`, 2026-11-26): `LoadModel`이 CPU `BuildLatticeTopology` 직후 `UploadLatticeTopology`를 호출한다. `Model::m_GpuLatticeTopology`는 skin section별 uint StructuredBuffer와 CellCount를 소유하며 CPU vector는 유지한다. `-island-gpu-verify 1`(기본 0)은 로드당 한 번 GPU 회수·전체 값 비교 후 PASS/FAIL·정점/바이트/셀 수를 기록한다. 빈 section은 슬롯만 유지하며 `Model::Destroy`가 GPU 격자를 해제한다. 당시 실제 소비자와 `-island-gpu`는 G2b로 남겼다([검증](../GPU/GpuIslandPipeline.md#g2a-결과)).

G2b(최종 통합 Debug 빌드·CPU 재검증 PASS, 척추 verify PASS·Xbot 다중 게시 미검증; 완료, 미커밋, base `760d1412`, 2026-11-26): `SkeletalPatchJob::Submit`은 `-island-gpu 1` 또는 `-island-gpu-verify 1`에서 추출 직후 `IslandDistanceCS`를 같은 제출에 기록한다. PG에는 section frame이 적용되어 world만 곱하며 코너별 거리·G2a 셀을 job 버퍼에 보존한다. verify만 같은 fence로 회수하고 `Poll` 뒤 `DiagnoseIslands`가 실제 이동 평면의 w 변화까지 보정해 CPU와 비교한다. CPU가 계속 출력을 결정하며 5장면 부호·셀 불일치 0이다([결과](../GPU/GpuIslandPipeline.md#g2b-결과)).

G3a(최종 통합 Debug 빌드·CPU 재검증 PASS, 척추 verify PASS·Xbot 다중 게시 미검증; 완료, 미커밋, 2026-11-26): `SkeletalPatchJob::Submit`은 GPU 옵션에서만 Parent·Status를 소유하고 거리 뒤 `IslandHookCS`·`IslandJumpCS`를 K번(기본 32) 기록한 뒤 `IslandVerifyCS`로 활성 코너의 최종 root를 검증한다. `Poll`이 같은 추출 fence로 Status를 비동기 회수하며 미수렴은 CPU fallback 로그를 남긴다. `DiagnoseIslands`는 verify에서 GPU root↔CPU island ID의 양방향 대응을 검사한다. CPU 출력은 유지한다([결과](../GPU/GpuIslandPipeline.md#g3a-결과)).

G3b(최종 통합 Debug 빌드·CPU 재검증 PASS, 척추 verify PASS·Xbot 다중 게시 미검증; 완료, 2026-11-26): `SkeletalPatchJob::Submit`은 G3a 뒤 `IslandNumberCS::main`의 5개 패스를 같은 제출에 기록한다. root별 최소 패치 키와 256키 블록 scan으로 CPU 첫 등장 번호를 만들며 미수렴은 번호를 생략한다. job 소유 uint2 표·Status 개수는 기존 fence로 회수하고 `Poll`·`DiagnoseIslands`가 verify에서 정확히 비교한다. CPU 출력은 유지하며 Release 3장면·validation 번호 PASS, K=1 fallback·D3D12 ERROR 0이다([결과](../GPU/GpuIslandPipeline.md#g3b-결과)).

MI-2(base `432ff0c5`, 2026-11-13): `SkeletalPatchJob::Poll`이 기존 TriangleSources readback을 재사용한다. `Submit`의 준비 포즈 snapshot과 실제 확대 평면으로 `DiagnoseIslands`가 패치 참조 정점만 중복 없이 스키닝하고 joint→그룹 표를 요청당 한 번 만든다. `LabelPatchIslands`는 출력 없는 side를 제외하고 (셀, side) 연결과 원본 세 꼭짓점 전체의 bind weight 투표를 분리해 몸체 표를 job/패치에 보관한다. GPU 출력에는 적용하지 않는다. CPU 테스트·D/R 빌드·Release 회귀 PASS; CesiumMan 위팔 P7에서 3개 아일랜드 중 허리 90 tris가 A로 재배치되는 라벨을 확인했다([결정·로그](../GPU/MeshIslandMultiBoneCut.md)).

MI-1(base `2a1620cd`, 2026-11-13): `BuildInfluencingJoints`는 skin section에 draw 로컬 삼각형 인덱스를 보존한다. `BuildLatticeTopology`가 bind 최장축 64칸의 점유 셀·삼각형 간선을 생성해 `Model::m_LatticeTopology`에 보관한다. `LatticeUnionFind`·`CountLatticeComponents`는 순수 CPU 연결 성분 계산이며 절단 출력은 변경하지 않는다([결정·검증](../GPU/MeshIslandMultiBoneCut.md)).

스켈레탈 연속 캡처 시연 옵션 추가(미커밋, base `03b5c8a1`, 2026-11-06): `ModelViewer::Update`가 `BuildSkeletalBreakPlane`을 재사용해 Xbot P60(`-skeletal-break-demo 1`) 또는 CesiumMan P8(`2`)을 정상 C 요청한다. 절단 시 카메라를 유지하고(`-skeletal-recut-repro` 및 두 데모의 `-capture` 포함, 2026-11-25; 자동 검사 프레이밍 유지) Applied 게시부터 20프레임 간격으로 HDR/post 8장을 저장한다. `-skeletal-break-demo-unmask 1`은 `SetTriangleMask`를 생략하므로 첫 게시 후 수동 C를 포함한 후속 요청을 거절한다. 요청 거절·게시 실패는 오류 로그 후 종료하며 자동 검사·벤치 옵션 동시 지정은 Startup에서 거절한다. 호출자 Release 관찰은 [SP-7 문서](../GPU/SkeletalPatchBreak.md)에 기록했고 이번 수정은 빌드·실행하지 않았다.

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
| `Physics/PhysicsWorldStep.cpp`, `ModelViewer/Scene/SkeletalPhysicsSystem.*` | `CollideOrientedBox`로 Moving body를 조회하고, `FindBody`로 owner·body·node를 찾으며, `BuildKinematicSegments`로 선택 body가 Kinematic이고 incoming 관절이 살아 있는 선분의 world 끝점을 만든다(root는 기존 조건, Dynamic 자식 끝점은 `UpdateKinematicTargets`가 덮어쓰기 전에 보관한 `AnimationFrame` 사용, [설계](../GPU/RigRecutSelection.md)). |
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

NDC 녹화 재현: `ModelViewer::ConfigureDemoAnimation`이 `ReadRigDiagnosticAnimationNames`로 클립을 해석하고 임시 pose 60개에서 자동 절단 시각을 고른다. `ModelViewer::Update`는 시작 지연·누적 재생 시간 절단과 `-skeletal-break-demo-tilt`의 모델 +X 투영축 회전을, `RenderScene`은 Applied 후 클립 전환을 조율한다. `ModelInstance::GetAnimationTime`과 `GameObject::PauseAnimation`을 사용하며 next 지정 시 추종 기본값은 0, 생략 시 1이다. CesiumMan은 숫자 절단 시각을 사용한다. `-skeletal-break-demo-bone-fraction`은 명시한 본의 부모 관절에서 DFS 첫 자식 방향 비율(기본 0.5)을 `BuildSkeletalBonePlane`에 전달한다. LW 0/2 비교 씬은 0.35를 사용하며 모드 0의 비율표는 로그에만 사용한다(기준 `5672bb44`, 미커밋, 2026-11-25). tilt는 기준 `1ec89f9f` 위 추가(미커밋)이다. [옵션·명령](../GPU/NdcAnimSwitchRepro.md).

PatchAnimFollow(AF-1~6): ModelInstance::EvaluateAnimation이 물리 override 전 애니메이션 node와 Joint 배열(BuildJointArray, Model.cpp)을 따로 보존한다. SkeletalPatchCopy.cpp의 FillAnimFollow가 snapshot AnimFollow·O_k를 채우고, CheckAnimFollowAtCut(절단 시점)·MeshComponent::CheckAnimFollowFrame(프레임)이 팔레트 재계산을 진단한다. Viewer::PatchAnimFollowEnabled(PatchSkinning.cpp)는 `-patch-anim-follow`(기본 1)를 읽는다. PatchSkinBuildCS의 ExtInternal은 1 = 같은 body 내부 교점, 2 = 실제 분리 rim 끝점, 3 = cap 끝점이다. 설계·결과는 [PatchAnimFollow.md](../GPU/PatchAnimFollow.md).
PatchLengthWeights(LW-1~4, 기본 off `-patch-length-weights 0|1|2`): `PatchLengthWeightsMode`가 옵션을 한 번 읽고, `BuildRemainLengthTable`이 A 조각 팔레트 joint별 남은 길이 비율과 소속 P를 만들며, `BuildPatchBoundaryFlags`가 원본과 공유되는 패치 정점을 표시한다. `PatchSkinBuildCS`의 `Reweight`가 복사 정점·rim·cap 끝점 레코드를 재분배하고 bind 위치·법선·탄젠트를 `RemapFrame` 등으로 다시 계산한다. cap 방향은 `CutTimeNormal`이 실제 저장할 끝점 skin의 절단 시점 법선 행렬로 역변환한다(master cofactor 설계 유지). 재절단은 부모 레코드를 복사하여 승계한다. 자세한 내용은 [PatchLengthWeights.md](../GPU/PatchLengthWeights.md).

PatchLengthBlend(LB-1, 기본 off `-patch-length-blend 0|1`): `PatchLengthBlendEnabled`가 LW와 동시 사용을 거부한다. `BuildRemainLengthTable`의 모드 독립 비율(P=r_P, 자손=0, 나머지=1)을 A의 `PatchSkinSnapshot::LengthBlendRatio`에 저장하고 재절단은 값 복사로 승계한다. LW 모드별 P 처리는 호출부에 유지한다. 렌더에는 아직 연결하지 않는다. 자세한 내용은 [PatchLengthBlend.md](../GPU/PatchLengthBlend.md).


BC-08a-1: ModelViewer/Scene/SkeletalPhysicsFrames.h의 TryBuildBodyFrame은 배율을 제거한 E*Gnode에 월드 m 단위 body offset을 적용한다. TrySplitUniformFrame은 양수 uniform scale·직교성·affine·finite를 검사하고 TryRestoreNodePose는 저장 배율을 복원하는 순수 좌표 왕복을 제공한다. SkeletalFrameTests는 Jolt/GPU 없이 검사한다. rig 저장·Kinematic 구동은 BC-08a-2, Scene 연결은 BC-11에서 완료했다.

BC-08a-2: SkeletalPhysicsSystem::RegisterFromPose는 자산·부모·등록 s와 E 배율을 저장하고 UpdateKinematicTargets는 전체 목표를 선검증하여 Kinematic만 MoveKinematic으로 구동한다. GetBodyTarget은 body 원점 목표와 COM 선속도·월드 각속도를 읽는다. PhysicsWorld::CanMoveKinematic과 GetBodyMotion으로 SDK 타입 없이 읽기 전용 검증을 수행한다.

BC-08b: SkeletalPhysicsSystem::CollectPoseOverrides는 Dynamic body만 현재 E에서 model node pose로 복원하고 성공 시 override 배열을 교체한다. 등록 s·body offset을 복원하며 Kinematic 본은 기존 애니메이션을 유지한다. 결과는 기존 ModelInstance::FinalizePose 또는 Scene::FinalizeObjectPose에 전달할 자료이며 실제 fixed-step Scene 연결은 BC-11이다.

BC-10: SkeletalPhysicsSystem::BreakConstraint는 broken 경계를 제외한 subtree와 세대를 검증하고 PhysicsWorld::ApplyConstraintBreak로 일괄 적용한다. PhysicsWorldBreak.cpp가 anchor·Jolt 제한·외부 pin·전환 목표·impulse를 선검증하고 선택 관절만 제거한다. 성공 시 Dynamic body를 기존 CollectPoseOverrides가 수집하며 topology가 증가한다. 같은 관절 재요청은 AlreadyBroken이고 추가 impulse가 없다. BC-11의 PhysicsSystem::AdvanceSimulation이 animation 평가→Kinematic 목표→queued break→공통 world step→Static/본 writeback→owner별 pose 확정을 연결한다. ModelViewer는 물리 on에서 렌더 프레임당 PrepareMeshes만 한 번 호출하며 off는 기존 Update wrapper를 쓴다. zero-step은 기존 pose/대기 break를 유지하고 Scene::InitializePoses가 신규 모델만 dt=0으로 준비한다. section E와 Nmesh ancestry를 검증하며 world는 비공개다.

SP-7a/7a2: `SkeletalAutoRig.*`의 `BuildSkeletalPhysicsAsset`는 영향 palette 병합·E/배율/root 검증·등록 pose 정점 bounds box·anchor를 생성한다. leaf는 본 원점을 bounds에 포함하고 정점 없는 본만 segment fallback을 사용한다. `RunSkeletalAutoRigChecks`는 두 모델의 독립 애니메이션 5시점 anchor 오차·extent 범위·fallback 수·CPU 보관 비용을 TEMP에 남긴다. [결과](../GPU/SkeletalPatchBreak.md).

SP-7b: `SkeletalPatchCopy::Request`는 미등록 owner를 최종 pose로 지연 등록하고 `FindIncomingJoint`와 세대를 저장한다. A/B 준비 뒤 `QueueBreak`의 RequestId로 `TryGetBreakResult`를 소비한다. 원본 숨김·node 부착은 아래 SP-7e 게시 단계로 이어진다.

SP-7c / CP-4b: `TriangleMaskCompaction.h`의 `CompactTriangleMask`는 CPU 제거 상태와 skin CPU 인덱스를 원본 R16/R32 압축 IB·draw 범위로 바꾼다. `tests/SkeletalPatchTests.cpp`는 합성 원본·다중 section/draw·전체/무제거·거절 보존을 GPU 없이 검사한다. `MeshComponent::SetTriangleMask`는 컴포넌트 전용 업로드 IB와 section override를 게시하고, `UpdateTriangleMask`는 추출 입력 GPU State만 갱신하며, `ClearTriangleMask`는 원본 view로 복원한다. `MeshSorter::AddMesh`·`RenderMeshes`는 section IB view와 원본 draw 순서의 override 범위를 사용한다. `Scene/SkeletalMaskChecks.cpp`의 `RunSkeletalMaskChecks`는 원본 형식 압축 IB·공유 원본 불변·색상/깊이/그림자 ROI 소실·다른 컴포넌트 보존·상태 갱신/해제 복원을 검사한다.

SP-7d: `PatchExtractCS::main`·`ExtractPatchReference`는 전역 State의 제거 비트를 선택 조건에 반영하며 교차·미포함 카운터도 제거 삼각형을 제외한다. 인접 제거 삼각형 집계(`Submit`·`Poll`·`Validate`)는 RM-1에서 제거됐다. `CheckMaskedPatch`는 `RunSkeletalMaskChecks`에서 Xbot 부모 제거 후 자식 추출과 없음/0/해제 출력 바이트 불변·전체 제거의 빈 카운터를 검증한다.

PF / CP-5: `PatchExtractCS::main`의 Mode 1이 64개 삼각형 블록 안 inclusive prefix와 블록 합계를 쓰고, Mode 6이 블록 exclusive offset과 `Meta` 총계를 쓴다. Mode 7은 제거되었으며 Mode 2 scatter가 현재·이전 삼각형 prefix에 각자 소속 블록 offset을 더한다(t=0의 이전 값은 0). `Scan`은 블록 안 prefix로 남고 CPU·Mode 3/4는 이를 읽지 않는다. `SkeletalPatchJob::Submit`은 Mode 0→1→6→2 순서로 dispatch하고 블록 영역 배치와 root signature는 그대로다([설계](../GPU/PatchPrefixScan.md)).

AD: 폐기 — Mode 8 정점 표시·Mode 5 인접 집계와 `Adjacent`(u6)·`VertexMarks`(u7)·`VertexBase`·`VertexMarkWords`는 RM-1에서 제거됐다(`PatchRoot` 12개, `Reserved1`·`Padding1` 복원). 실행 순서는 Mode 0→1→6→2다(CP-5에서 Mode 7 제거)([기록](../GPU/PatchAdjacentMarks.md), [제거 결정](../GPU/PatchDiagnosticRemoval.md)).

RM-2: 기본 경로의 평면 교차 수·미포함 교차 수 집계도 제거됐다. Mode 0은 선택 플래그만 쓰고, Mode 1·6은 선택 합계 하나를 scan하며, `SkeletalPatchResult`는 `PatchTriangles`만 가진다(`Meta` byte 4~11은 항상 0인 예약 영역). 셰이더 `Crossing`·`Overlaps`는 Influence 투표(Mode 3)용으로 남는다([제거 결정](../GPU/PatchDiagnosticRemoval.md)).

SP-7e: `SkeletalPatchCopy::Request`는 C snapshot·side를 보관하고 `OnCutResult`는 숨긴 A/B 준비 뒤 break를 요청한다. `Update`는 Applied·revision 일치 후 원본 mask와 원래 owner의 A/B를 함께 게시한다. `Reset`은 `PhysicsSystem::CancelBreak`로 미실행 요청도 취소한다. `Scene::PrepareMeshes`는 최종 node pose로 부착을 갱신한다. `PhysicsSystem::IsEligible`은 부착 조각도 Static 절단 후보로 허용하고 `SyncBodies`는 부착 조각의 body 등록을 제외한다. `ResolveEditTarget`은 owner 선택을 유지한다. offset·B 간격은 `-skeletal-patch-offset-copy 1` 전용이다. 경계 오차·강제 Stale·캡처 진단은 미완료다.


G4a(구현·CPU 테스트 완료, 최종 통합 Debug 빌드·CPU 재검증 PASS, Debug 실행 assertion, base `2c89992d`, 2026-11-26): `VotePatchIslandsInteger`·`ResolvePatchIslandVotes`는 `Model/LatticeTopology.*`의 D3D 없는 CPU helper다. uint16 원시 weight 정수 투표·근접/overflow oracle 선택·SideDefault 재구성을 LatticeTopologyTests 2011 사례와 ModelViewer Release 빌드로 검증했다. 런타임 연결은 G4b에서 추가됐다([결과](../GPU/GpuIslandPipeline.md#g4a-결과)).

G4b(최종 Debug 빌드·CPU 테스트 PASS, 척추·Xbot 다중 vote PASS, Debug 실행 assertion; 예외 범위·부분 버퍼 해제 수정·Release 빌드 및 Xbot 최종 소비/published error=0 확인, 미커밋, 2026-11-26): `Submit`은 uint64 vote-size·주소·256 MiB 상한 검사와 선택적 섬 GPU 준비 중 던져진 예외(예: 버퍼 생성)에만 gpu-prepare fallback·부분 버퍼 해제를 적용하고, weight 0의 joint 조회를 생략하며 기존 제출 context로 weight를 비동기 업로드한다. `DiagnoseIslands`는 수렴·dispatch-limit 없음·거리 부호/셀·partition·번호/개수·섬 메타데이터 일치·vote mismatch=0·near=0·overflow 없음에서만 GPU 표를 소비한다. 동일 번호의 Scores/JointScores를 보존해 후속 cap 재라벨·분류와 일관성을 유지한다. CPU oracle은 매 요청 실행하므로 CPU 절감은 아직 없고 기본 경로 제거는 별도 결정이다. PSO/RootSignature Finalize·업로드 페이지 할당 실패는 필수 추출과 같은 비예외 `ASSERT_SUCCEEDED` 규약을 따라 catch에서 잡지 못하는 알려진 한계다. near>0/overflow 런타임 미검증; 최종 Debug 빌드·CPU 테스트 PASS, Debug 실행 assertion([검증](../GPU/GpuIslandPipeline.md#g4b-결과)).

G4c(구현·Release 검증 완료, 최종 Debug 실행·다중 P 완료 조건 미충족, base `4fd04f42`, 미커밋, 2026-11-26): `DiagnoseIslands`의 G4b 소비 승인만 `ClassifyCutBodies`에 전달한다. `PatchBodyClassifyCS::main`의 GPU 변형은 job의 투표 몸체 표·삼각형 섬 번호를 SRV로 읽고 `RelabelCapLoops`/`RelabelPatchIslands`가 변경한 섬마다 8B override를 분류 context로 비동기 업로드한다. 비섬 보정·64개 초과는 사유와 함께 CPU 표로 복귀한다. `BodyCompactJob`은 압축 fence까지 원본 COM 참조·보정 표를 보호하고 `PollBodyCompaction`은 verify에서 CPU 표와 몸체별 수·표면 삼각형별 body를 비교한다. 최종 척추 off/on 동등·양 side classify PASS, 통합 Debug 빌드 PASS·실행 assertion, Xbot 다중 P 기존 충돌; 기본 GPU 0·매 요청 CPU oracle 유지(절감 없음)·G5 보류([결과·검증 범위](../GPU/GpuIslandPipeline.md#g4c-결과)).

G2~G4 최종 검증·측정(base `2c89992d`, 2026-11-26): `ComputeContext::SetDynamicConstantBufferView`의 입력 포인터·16바이트 정렬 assertion이 Debug GPU+verify와 flags-off 모두에서 발생했다. D3D12 ERROR는 양쪽 0이나 정상 완료 검증은 실패다. Release 척추는 지정 진단 출력 동등·양 side body 불일치 0, Xbot 다중 P는 flags-off도 기존 충돌로 최종 분류·게시를 검증하지 못했다. 성능 runner는 각 14/15 실행·26요청 성공이며 GPU on의 요청→게시 평균이 모든 비교 행에서 증가했다([최종 표·근거](../GPU/GpuIslandPipeline.md#g2g4-최종-검증측정)).
