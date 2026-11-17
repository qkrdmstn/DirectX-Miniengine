상태: 완료 — MI-4 완료(`e123cad1`), MI-5 시험·기록 완료(`bffcf22f`, 2026-11-17): 위팔·실제 허벅지·척추 평면 및 P=1 옵션 비교 18 PNG 확인·Release 회귀6건 PASS. 몸통 접힘·척추 절단 양팔 틈·작은 파편은 미해결이며 아래 남은 일에 기록. 다중 경계 측정·Applied 이후 복구·기본값 on 결정 후속.

# 메시 아일랜드와 다중 본 합쳐 자르기 (격자 토폴로지 맵)

## 목표

1. 스켈레탈 패치 절단 뒤 생기는 메시 아일랜드(끊긴 연결 성분)를 찾아, 각 아일랜드를 맞는 몸체에 붙인다.
   현재 CesiumMan 위팔 절단에서 허리 삼각형이 B와 함께 움직이는 문제를 없앤다.
2. 평면에 걸친 여러 본을 합쳐 한 번에 복사·절단하고, 끊은 관절마다 별도 몸체로 분리한다.

## 현재 구조와 원인 (조사 근거: 공유 노트 `island-survey.md`)

- `PatchExtractCS.hlsl::Selected`는 선택 joint 하나의 weight가 0보다 크면 정점을 고르고, 꼭짓점 하나라도 선택된 삼각형을 통째로 복사한다.
  weight 하한·연결성 조건이 없다.
- 복사본은 사실상 무한 평면으로 잘리고(`SkeletalPatchCopy::Update`가 사각형 확대), `ClipTrianglesCS`의 반공간 부호만으로 A/B가 정해진다.
- 그래서 P의 미소 weight를 가진 허리 삼각형이 반대편 반공간에 있으면 B로 가 팔을 따라 움직인다.
- 다중 본: `SelectSegmentJoint`/`SelectRigSegment`는 joint 하나만 고르고, `Request`는 `BreakRequest` 하나만 가진다.
  본마다 따로 자르면 먼저 복사한 영역이 첫 패치에 귀속돼 이음새·cap이 맞물리지 않는다.
  NDC25도 "자른 뒤 합치기"를 "합친 뒤 자르기"로 바꿨다고 기록한다([슬라이드 10:12], `NDC25Reference.md` 6절).
- 메시 연결 성분·union-find 코드는 없다. CPU에는 `Model::m_SkinCpuSections`(bind position, joint, weight)가 있고 인덱스는 없다.

## 결정 (사용자 확정 2026-11-13)

| 항목 | 결정 | 이유 |
| --- | --- | --- |
| 격자 공간·시점 | bind 공간, 모델 로드 때 1회 생성 | 포즈와 무관하게 연결성이 고정된다 |
| 해상도 | bind bounds 최장축 64칸, 정육면체 셀 | CesiumMan 셀 약 2.3cm로 팔·허리 틈보다 작다 |
| 셀 노드 | 같은 셀에 든 정점은 한 노드(위치 용접) | UV 이음새 중복 정점과 section 경계를 자동으로 잇는다 |
| 셀 연결 | 한 삼각형의 꼭짓점 셀끼리 연결 | 맞닿기만 한 팔·허리는 연결하지 않는다 |
| 맵 자르기 | 노드를 (셀, 반공간)으로 나누고 같은 반공간끼리만 잇는다 | 평면을 건너는 연결을 끊는 것이 곧 맵 절단이다 |
| 계산 위치 | 맵·아일랜드 라벨은 CPU, 출력 분리는 GPU | 패치 삼각형 수천 개 수준이라 CPU 비용이 작다 |
| 아일랜드→몸체 | 실제 분리 loop에 닿으면 반공간, 아니면 weight 다수결(2026-11-16 수정, 아래 추가 결정) | 평면 반대편에 있어도 몸통 weight가 큰 허리는 A에 남는다 |
| 다중 본 | 무한 평면과 교차하는 본 세그먼트 중 최상위만 | 조상이 이미 끊기면 자손 관절은 끊지 않는다 |

### 그룹과 다수결

- 끊는 관절 집합 `{P_1..P_n}`(서로 조상·자손 아님). 그룹 `B_i` = `P_i`와 그 자손 joint, 그룹 `A` = 나머지 joint.
- 아일랜드의 그룹 점수 = 아일랜드 패치 삼각형 꼭짓점의 bind weight를 joint의 그룹별로 합한 값.
  가장 큰 그룹의 몸체에 붙인다. 동점이면 A.
- 몸체 0 = A(현재 A 부착 규칙: `P`의 직접 부모 node), 몸체 i = `B_i`(부착 `P_i`).

### 반공간 예측

- CPU가 패치 삼각형의 원본 정점을 현재 포즈(`GetPreparedJoints`, 컴포넌트 world)로 스키닝해 평면 부호를 구한다.
  0은 `ClipTrianglesCS`처럼 side0으로 친다.
- GPU 절단 출력 삼각형의 (패치 삼각형, side)에 대응하는 노드가 예측에 없으면 그 side의 다른 꼭짓점 셀 노드를 쓰고,
  그것도 없으면 그 side의 기본 몸체(그 side 삼각형이 가장 많이 간 몸체)를 쓴다. 이 대체 횟수를 진단 로그에 남긴다.

### cap

- cap 삼각형은 그것이 닫는 절단 테두리의 원본 패치 삼각형이 속한 아일랜드의 몸체로 간다.
  cutter 출력에서 테두리 출처를 추적할 수 없으면 그 side의 기본 몸체로 보내고 MI-3 조사 메모에 근거를 남긴다.

## 인터페이스 골격

```cpp
// MiniEngine/Model/LatticeTopology.h — 순수 CPU, 테스트 가능
struct LatticeTopology
{
    float3 Origin; float CellSize; uint32_t Dim[3];
    std::vector<std::vector<uint32_t>> VertexCell;  // section별 정점 → 압축 셀 id
    std::vector<std::vector<uint32_t>> Indices;     // section별 삼각형 인덱스(로컬)
    uint32_t CellCount;
};
LatticeTopology BuildLatticeTopology(const SkinCpuSections&, uint32_t cellsOnLongestAxis = 64);

// 잘린 맵의 아일랜드 라벨: (셀, side) 노드 union-find
struct IslandLabels { std::vector<uint32_t> TriangleSideBody; uint32_t IslandCount; /* 진단 */ };
IslandLabels LabelPatchIslands(const LatticeTopology&, const PatchTriangleSpan&, const SideSigns&,
                               const JointGroups&);
```

- `SkeletalPatchJob::Poll`이 `TriangleSources`를 읽어 `LabelPatchIslands`를 부르고, 결과 표를 다음 `CuttingSystem::Request`와 함께 GPU에 올린다.
- 다중 본: `SelectCrossingJoints(plane) → {P_i}`, `PatchExtractCS`의 선택 마스크를 joint 집합으로, `BreakRequest`를 배열로 확장한다.

## 단계 (각 단계 = 한 개념, 약 300줄 이하, 새 Codex 구현 + 별도 Codex 리뷰)

| 단계 | 내용 | 완료 조건 |
| --- | --- | --- |
| MI-1 | 격자 토폴로지 맵: CPU 인덱스 보관, `BuildLatticeTopology`, 로드 시 생성, union-find | 단위 테스트(분리된 두 삼각형 띠=2 성분, 같은 위치 중복 정점=1 성분, 평면 절단 시 성분 증가) PASS; CesiumMan 로드 로그에 Dim·셀 수·전체 성분 수 |
| MI-2 | 완료(base `432ff0c5`): Poll의 기존 TriangleSources readback 재사용, 제출 포즈 반공간 예측, `LabelPatchIslands`, 로그·표 보관만 | 리뷰 3건 수정 후 CPU 테스트·D/R 빌드·Release 회귀 PASS; 위팔 P7에서 아일랜드 3개, 허리 90 tris → A; GPU 출력 불변 |
| MI-3a | 완료(미커밋, base `b7413004`): 라벨 GPU 업로드, 출력 삼각형 몸체 판정(provenance `TriangleId`→`TriangleSideBody`) | D/R 빌드·Release 회귀 PASS; 양쪽 invalid=0/idMismatch=0, side0 reassigned=104; 출력·게시 불변 |
| MI-3b | 완료(미커밋, base `2a0ab7d2`): body×section count→offset→scatter 압축 셰이더·합성 GPU 검사, P2 surface 우선 배치 수정 | 두-side 정점/depth/provenance/IB 보존·스키닝 주소 일치·합계 6·삼각형/바이트 overflow PASS; D/R 빌드·Release 회귀 PASS |
| MI-3c | 완료(`96214e6f`): 실제 두 side 비동기 압축·실패 격리·완료 후 회수·몸체별 Mesh/삼각형 수 보관(게시 미연결) | 위팔 body0=518/body1=194, 합계 712=입력 712 및 material/depth/IB/bounds PASS; D/R 빌드·Release 회귀 PASS |
| MI-3d | 완료(미커밋, base `96214e6f`): 비동기 완료 후 body 0/1 게시·몸체 snapshot 스키닝, cap은 SideDefault, 부적합 결과는 side 폴백 | 위팔 518/194 fallback=0, 허리 90 tris→A, ON/OFF 캡처·D/R 빌드·Release 회귀 PASS |
| MI-3e | 완료(미커밋, base `e8cd2e59`): segment 출처·leader snapshot → loop별 양쪽 몸체 다수결 진단 | 위팔 팔 keep(B/A)·허리 drop, Xbot 허벅지 drop; conflicts/fallback=0, D/R·Release 회귀 PASS. 귀속은 MI-3f |
| MI-3e2 | 완료(미커밋, base `94f17ca0`): 제출 포즈 교차점·실제 loop 판정, 테두리 아일랜드 재라벨 후 MI-3a/3c | 위팔 팔 keep·허리 drop/A 유지, Xbot 허벅지 keep(1/0)·fallback=0; CPU·D/R·Release 3사례·회귀 PASS |
| MI-3f | 완료(미커밋, base `63c2292b`): leader별 side 몸체/drop 표를 GPU 분류·압축에 연결 | 위팔 cap 54개 제거·팔 cap 28개 유지, body0/1=491/167; publish/after60 PNG 확인; SP7e2 case2/3=0mm(case1 기존 N/A), PS2/3·PS4 ≤0.01mm, Static/PS/AR 회귀 PASS |
| MI-4a | 완료(`7728485a`): `SelectCrossingJoints` 무한 평면 교차 최상위 P 집합, 유효 조상 계산 공유; 런타임 미연결 | 형제 P={1,2}, 조상+자손 P={2}, 부호 반전 Bside={0,0}→{1,1}, 비영향/비팔레트 건너뜀 0→3; CPU 83 checks·Lattice·D/R 빌드·Release 회귀 6실행 PASS |
| MI-4b | 완료(`86522153`): `BuildJointUnionMasks`·추출 P 집합 입력, `PatchExtractCS::Selected` 재사용 | CesiumMan 형제 허벅지 P={13,17}: 단독364/352, 합집합526, 중복 P도526; GPU==CPU·중복0, CPU·D/R·3데모·Release 회귀 PASS |
| MI-4c | 완료(`caa6f681`): `BuildCutBodyGroups`·몸체별 snapshot/attachment 배열 | n=1 기존 Frozen 규칙·세 데모 동등; 형제 n=2 그룹·Frozen 팔레트·LCA 및 CPU/D/R/회귀 PASS |
| MI-4d | 완료: MI-4d1(`58807a5e`), MI-4d2(`188adc01`) | 같은 side B1/B2 cap 분리·빈 몸체 로그 진단 PASS; 몸체별 skin 준비는 MI-4e 이후로 이월 |
| MI-4d1 | 완료(`58807a5e`): 요청 그룹 수 기반 body×section 압축·몸체별 분류 카운터·준비 ID 배열 | 빈 마지막 몸체 보존·로그, 합성 세 몸체/빈 네 번째 몸체 GPU 압축 검증 |
| MI-4d2 | 완료(`188adc01`): P_i 교차점 snapshot·아일랜드 재라벨·loop별 side 몸체·`-skeletal-union-checks` n=2 진단(몸체별 skin 준비는 미연결) | P13/P17 실제 합집합 절단·분류·압축 진단, B1/B2 cap 분리·빈 몸체 로그; break·mask·n+1 게시 미연결 |
| MI-4e | 완료(`1d8c8ce2`): `PrepareConstraintBreak`/`CommitConstraintBreak`·`PrepareBreak`/`CommitBreak` 분리, 기존 진입점은 얇은 래퍼 | 단일 break 회귀·거절 무변경(CPU 959 checks), 세 데모·회귀 PASS |
| MI-4f | 완료(`61dd8e91`): `BreakConstraints` 요청 전체 사전 검증·겹침 거절·핸들 생존 점검 후 순차 commit, 공개 `CommitBreak` 오래된 snapshot Stale | 두 번째 실패 시 첫 constraint도 보존, 같은 rig 독립 관절 batch 세대 +2·충돌 batch 무변경(CPU 996 checks), 세 데모·회귀 PASS |
| MI-4g | 완료(`f72f28c2`): `QueueBreaks`·`TryGetBreakResults`, 단일 API는 래퍼 | pause·Stale·AlreadyBroken·Reset 무변경, 같은 rig 성공 +2, CPU·D/R·회귀 PASS |
| MI-4h | 완료: h1(`0cd2efba`) 준비와 h2(`e123cad1`) batch 게시로 분할 | 몸체별 skin·mask 1회·n+1 게시·거절 무변경·허벅지/몸통 시점 PNG·회귀 PASS |
| MI-4h1 | 완료(`0cd2efba`, 리뷰 포함): 명시 다중 옵션·n+1 숨긴 component/skin 준비 | 300/187/175 skin PASS·원본/mask/constraint 무변경·실패 정리·회귀 PASS |
| MI-4h2 | 완료(`e123cad1`): 모든 P incoming 고정·`QueueBreaks` 전부 Applied 후 mask 1회·빈 몸체 제외 n+1 게시 | Applied2·constraint18→16·세대+2·patch id1·300/187/175·Stale 정리·캡처·회귀 PASS |
| MI-5 | 완료(미커밋, 2026-11-17): CesiumMan 시험·기록 | 위팔·허벅지·실제 척추 평면 게시/+60 및 위팔+120/+180 PNG, P=1 옵션 비교·Release 회귀6건 기록. 시각 결함은 남은 일에 보존 |

### MI-3·MI-4 추가 결정 (Claude 판단, 2026-11-13; 근거 Codex 계획 메모 `%TEMP%/MI3-plan.md`)

- 재배치는 절단 후 body×section 압축(새 Scene compute pass)으로 한다. 절단 전 패치에서 빼는 방식은 교차 삼각형·B_i↔B_j 분배·cap을 풀지 못해 쓰지 않는다.
- 출력 삼각형 몸체 = provenance `TriangleId`(패치 삼각형)와 출력 side로 `TriangleSideBody` 조회. cap은 loop 단위 몸체(MI-3e까지는 SideDefault).
- cap loop 규칙: loop의 side0 테두리 아일랜드 몸체와 side1 테두리 아일랜드 몸체가 같으면 그 loop는 실제 분리면이 아니므로 양쪽 cap을 모두 버린다(위팔 MI-3d 캡처의 허리 cap). 다르면 각 side cap은 그 side 테두리 아일랜드의 몸체로 간다. 한 loop 안 테두리 라벨이 섞이면 다수결 후 충돌 수를 로그한다.
- 아일랜드→몸체 규칙 수정(사용자 확정 2026-11-16): weight 다수결만으로는 관절 근처 절단에서 양쪽이 모두 B가 된다(MI-3e Xbot 허벅지 `side0Body=1 side1Body=1`).
  끊는 세그먼트 `P_i→자식`과 평면의 교차점을 평면 위 2D로 둘러싼 loop를 "실제 분리 loop"로 본다. 그 loop에 닿은 아일랜드는 반공간으로 정한다(자식 쪽=B_i, 반대=A).
  중첩되면 교차점을 포함하는 유효 폐곡선 중 최소 면적 loop 하나만 실제로 선택하며, 끝점은 |d|<=1e-5에서 교차로 처리하고 B는 부모→자식 방향으로 정한다.
  선택기의 ChildNodeId를 제출 snapshot에 보존해 사용하며, 보존값이 없으면 B 루트 P_i의 직접 자식 중 평면 교차점이 평면 중심에 가장 가까운 선분을 사용한다.
  어느 실제 분리 loop에도 닿지 않은 아일랜드만 weight 다수결로 정한다. 실제가 아닌 loop의 cap은 버린다. 서로 다른 몸체를 요구하는 실제 loop 둘에 닿은 아일랜드는 다수결로 정하고 충돌로 로그한다.
- 원본 mask는 합집합 패치 전체를 한 번 숨기고 patch id 하나를 유지한다.
- 다중 본 A의 강체 부착 node = 모든 `P_i` 부모의 최소 공통 조상(n=1이면 현재와 같다).
- 다중 break는 전부 성공 아니면 전부 거절(사전 검증 후 순차 commit). 부분 성공은 허용하지 않는다.

- 단계마다 Debug·Release 빌드, 회귀(`-skeletal-patch-break-checks 1`, `-skeletal-patch-checks 1`, `-attached-recut-checks 1`, 기존 Static 절단), 로컬 커밋.
- 범위 밖: 이미 절단된 조각(generated)을 다음 절단 입력에 합치는 병합, 재절단 결과의 스키닝 승계, GPU union-find.

## 시험 결과

MI-4h2(완료, 미커밋, base `0cd2efba`, 2026-11-17):
- `SkeletalPatchCopy::Request`는 명시 옵션과 P≥2에서만 모든 incoming·Rig/Topology 세대를 고정한다. 대표 incoming·반공간 확인도 P 집합 기준이며 하나라도 incoming이 없으면 기존 로그로 전체를 거절한다.
- `FinishCutResult`는 `PrepareMultiBodies`와 `DiscardStaleCutResult` 뒤 `QueueBreaks`를 한 번 호출한다. 기존 단일은 `m_Break`·`m_RequestId`와 `QueueBreak`/`TryGetBreakResult` 래퍼를 유지한다. 다중도 batch당 ID 하나를 사용하므로 pause·Reset의 `CancelBreak`는 MI-4g 계약을 따른다.
- `PrepareMultiPublication`은 `QueueBreaks` 전에 비어 있지 않은 전체 게시 수만큼 owner 용량을 첫 준비에서 예약하고, component 생성·Attachment 복사·변환·skin 활성화를 끝낸다. 미게시 component는 Request/Reset/공통 폐기에서 해제한다. 기존 단일 경로의 2칸 예약은 유지한다.
- Pending 복사·patch id 기록은 기존 `Update`의 `m_Removal` 구성에서 제출 전에 끝난다. `ConsumeMultiBreak`는 전부 Applied일 때 mask 1회와 준비된 component commit을 수행한다.
- 이동 불가: `SetTriangleMask`는 원본 변경과 GPU mask 생성·Pending 대입을 함께 수행하므로 거절 무변경을 지키려면 게시 때 남겨야 한다. GPU 갱신(`PrepareMeshes`)과 rig·mask 진단도 게시 결과에 의존하며 할당·예외 가능성이 남는다. 이를 분리하려면 범위 밖 MeshComponent API 계약 변경이 필요하다.
- 후속 과제: 제출 뒤 source revision 변경 및 Applied 이후 예외의 rig·mask·component 복구 계약은 기존 단일 경로와 함께 별도 설계한다.
- 남은 일: PairBoundary·MeasureBoundaryAt·PS4의 n+1 확장은 후속이며 다중 게시 로그는 `MI4h2 boundary/PS4 measure N/A (multi)`를 명시한다.
- CesiumMan P13/P17: Applied2, 실제 runtime constraint handle18→16, TopologyGeneration+2, maskCalls1·고유 patchIds1, A/B1/B2=300/187/175 tris, attach2/13/17·Frozen8/15/15·skin PASS.
- 기본 off인 `-skeletal-multi-reject 1`은 마지막 기대 세대만 변경한다. BatchRejected/Stale로 publish0·mask0·constraintDelta0, `ReadOriginalState` 무변경·`DiscardPreparedBodies` 정리 PASS. `-skeletal-multi-stale 1`의 제출 전 revision 거절도 무변경 PASS.
- CPU: 지정 cpu.ps1/build.ps1, SkeletalBreakTests Debug/Release 각996/996(11/17초), SkeletalPatchTests 각118, LatticeTopologyTests PASS. 외부 props 최종 Debug52초·Release54초, msbuild 각각 exit0, 기존 경고 유지.
- Release 기본 데모 (a)491/167 (b)1475/869 (c)161/170, 모두 fallback0·exit0; SP7b/SP7e/MI3d 로그는 h1과 동일. 옵션 on/P1개도491/167·fallback0. union-only300/187/175·same-side B1/B2 cap split PASS.
- 최종 Release 회귀6실행 exit0(51.31초), BC-12a/b·AR4·Rig·MI4g PASS, SP6 누적1110 PASS/0 FAIL. SP7e2 case2/3 publish/after60=0mm(case1 기존 N/A), PS2 최대0.000203mm·PS3 최대0.000146mm·PS4 간격 변화0mm.
- PNG: `%TEMP%/MI4h2-Thigh-demo-000-post.png`(publish f64), `001-post.png`(+60 f124); `%TEMP%/MI4h2-Torso-demo-000-post.png`, `001-post.png`도 같은 시점이다. 네 장 육안 확인. 몸통은 동일 P13/P17 절단의 몸통 중심 시점이며 별도 몸통 평면 절단 검증은 아니다. 다중 경계 수치는 별도 측정하지 않았다.
- 재현: `-debug 1 -skeletal-break-demo 2 -skeletal-union-checks 1 -skeletal-multi-cut 1 -physics-query-exit 180 -capture <TEMP>/MI4h2-Thigh -capture-count 2 -capture-step 60`; 허벅지 카메라는 `-skeletal-break-demo-bone leg_joint_R_1`, 몸통은 `Skeleton_torso_joint_1` 추가. 기존 raw2png.ps1로 원본 해상도 변환.
- GPU NVIDIA MX450·Release, 드라이버 이전 기록591.86(이번 재조회 불가). 직접 실행·회귀 DBWIN의 D3D12 ERROR/CORRUPTION 모두0, 기존 DXGI #80 유지. 근거: `%TEMP%/MI4h2-{cpu,tests,final-build}.log`, `MI4h2-*-{patch,debug,run}.log`, `MI4h2-regress.log`, `MI4h2-d3d12.json`, `MI4h2-report.md`.
- 리뷰 재검증: Debug/Release 다중 성공 포함 직접 각7건 모두 종료0·assert/abort 없음; Applied2·constraint18→16·patch id1·300/187/175와 두 Stale 거절 무변경 확인. CPU 전체 PASS, 지정 외부 props 빌드 Debug49초/Release63초 종료0, 지정 Release 회귀6건 종료0(51.62초), 직접14건·회귀6건 D3D12 ERROR/CORRUPTION 0. 근거: `%TEMP%/MI4h2-report.md` 리뷰 수정 절 및 `MI4h2-fix-*` 로그.
- 코드 SkeletalPatchCopy.cpp·.h 약231줄, CRLF/BOM 보존. ModelViewer.cpp 해시 불변, 저장소 Build 변경·stash/reset·브랜치 변경·커밋 없음. 기본값 on·다른 모델·재절단은 범위 밖, MI-5 후속.


MI-4h1(완료 `0cd2efba`, MI-4g `f72f28c2` 기반, 2026-11-17):
- 리뷰 수정: `SelectMultiCut`은 평면 모서리도 component-local로 변환한다. 이동·회전 진단은 `CheckJointUnion`의 루트·자식·side와 대조한다.
- 리뷰 재검증: CPU·Debug/Release 빌드·3데모·union·다중 준비·강제 skin 실패 정리·Stale 격리·Release 회귀 6실행 PASS, D3D12 ERROR/CORRUPTION 0. 근거: `%TEMP%/MI4h-report.md`의 리뷰 수정 절.
- `DiscardPreparedBodies`는 owner·압축 자원·준비 상태를 완료 기록보다 먼저 폐기한다. `-skeletal-multi-fail 1`은 두 번째 몸체의 skin을 거절해 부분 생성물 정리를 검사한다.
- 분할: 전체 MI-4h가 약300줄을 크게 넘을 것으로 판단해 h1(준비)/h2(batch 게시)로 나누고 h1만 구현했다.
- 시작 master HEAD=61dd8e91(MI-4f), 이전=1d8c8ce2/188adc01. MI-4g는 미커밋 작업 트리에 있어 그 위에서 작업했다. MI-4g 완료 커밋 해시는 없다.
- 설계: 기본 off인 -skeletal-multi-cut 1에서 최상위 교차 P≥2일 때 합집합·n+1 snapshot/attachment/Frozen·숨긴 component skin을 준비한다. A=P_i 부모 LCA, B_i=P_i다.
- 옵션 off 또는 P 1개는 기존 단일 경로를 사용한다. -skeletal-union-checks 1 병용으로 기존 P13/P17 평면을 재현한다.
- 변경: SkeletalPatchCopy.cpp +169/-7, SkeletalPatchCopy.h +3/-0. 함수는 SelectMultiCut, Request, PrepareMultiBodies, FinishCutResult다.
- 문서: MeshIslandMultiBoneCut.md(상태·단계·결과), README.md, CodeMap.md 동기화. 기존 MI-4g 미커밋 표기를 보존했다.
- CPU: SkeletalBreakTests Debug/Release 각996/996 PASS(13/20초), SkeletalPatchTests 각118 PASS, LatticeTopologyTests PASS.
- 빌드: 지정 외부 출력 props, Debug48초/Release63초, 둘 다 exit0. 저장소 Build 폴더 변경 없음.
- 기본 Release 데모: (a)491/167, (b)1475/869, (c)161/170. 모두 fallback0·exit0(9.74/9.19/10.57초).
- 기존 union-only: 300/187/175, same-side B1/B2 cap split PASS·exit0.
- h1 준비: A/B1/B2 tris=300/187/175, attach=2/13/17, Frozen=8/15/15, skin 모두 PASS·visible0.
- 준비 전후 원본 mask revision/내용·attachment·topology·body motion·broken 관절 동일 PASS; publish/break/mask=0.
- h1 거절: -skeletal-multi-stale 1로 저장 mask revision만 어긋나게 하여 기존 정리 경로·원본/mask/constraint 불변 PASS·exit0. batch 원자적 거절 검증은 h2 범위다.
- 옵션 on/P 1개 위팔:491/167·fallback0·exit0. 기본 단일 경로 수치 유지 확인.
- Release 회귀6실행 exit0(약60초): BC-12a/b·AR4·Rig·SP6·MI4g PASS, SP6 누적1032 PASS/0 FAIL.
- SP7e2 case2/3=0mm(case1 기존 N/A), PS2/PS3/PS4 최대0.000234/0.000189/0mm.
- D3D12 ERROR/CORRUPTION: 직접7실행과 회귀 DBWIN 수집 모두0. 기존 DXGI #80 유지. GPU NVIDIA MX450.
- 캡처: 없음(h1은 게시 전 준비 진단). 허벅지/몸통 PNG는 h2에서 남긴다.
- 보존: ModelViewer.cpp SHA256=1C29C322F6CBCBF8181114194BF750B1F8AA0B9030AFC2703EA766D13C855509(기존 기록과 동일), 타 세션 변경 보존. C++/헤더 BOM·CRLF 보존.
- 금지 작업 미수행: stash/reset/브랜치 변경/커밋/저장소 Build 수정 없음.
- 남은 일(h2): 전체 P incoming 검증, QueueBreaks·모든 Applied 확인, mask1회(patch id1개), 빈 몸체 제외 n+1 게시, constraint n개 감소, batch 거절 무변경, 캡처·회귀.
- 근거: %TEMP%/MI4h1-{cpu,tests,build}.log, MI4h1-{a,b,c,union,multi,stale,single-option}-{patch,debug,run}.log, MI4h1-runtime-*-{patch,physics}.log, MI4h1-regress{,-debug}.log.



MI-4g(완료 `f72f28c2`, base `61dd8e91`, 2026-11-17):
- API: `PhysicsSystem::QueueBreaks(requests, keepResult)`가 batch당 RequestId 하나를 발급하고 입력·기대 세대를 복사한다. `TryGetBreakResults`는 요청 순서의 상태 배열을 한 번 소비한다. `QueueBreak`는 원소 하나 제출, `TryGetBreakResult`는 배열의 첫 결과를 소비하는 래퍼다. 빈 batch도 번호를 발급하고 처리 뒤 빈 결과를 돌려주며 단일 조회는 이를 소비하지 않는다.
- 처리·계수: `AdvanceSimulation`의 기존 fixed step 경계에서 `ProcessQueuedBreaks`가 batch당 `BreakConstraints`를 정확히 한 번 호출한다. `Processed`와 `NotFound`는 요청당 누적하고 취소·pause는 누적하지 않는다. 단일 경로의 번호·소비 시점·로그·계수는 유지하며 SkeletalPatchCopy 호출부는 그대로다. batch 성공 로그의 body/sensor 수는 batch 적용 완료 상태다.
- 무변경 검사: `QueuedBreakChecks::Run`이 MI-4f와 같은 분기 fixture로 pause(실제 `AdvanceSimulation` 포함), Stale, 제출 후 끼어든 break, AlreadyBroken, Reset의 `CancelBreak` 전체 취소를 검사한다. 전후 Rig/Topology 세대·관절 수/handle/enable·body 수/운동/전체 pose/속도/sensor·Pairs 수 snapshot이 정확히 같고, 문제 요청은 자기 사유·나머지는 BatchRejected다. 독립 관절 둘 성공은 Applied 두 개·세대 +2이며 소비는 한 번뿐이다.
- 실행 위치: CPU 타깃은 `PhysicsSystem`의 Scene·모델·렌더 프레임 의존성을 링크하지 않으므로 기존 `-skeletal-break-checks`와 `-skeletal-break-xbot` 런타임 검사에서 독립 PhysicsSystem 큐를 만든다. 거절 검사는 공통 큐 처리부를 직접 호출해 world step에 따른 운동 변화와 구분한다. 두 실행 모두 진단 파일에 MI4g PASS를 기록했다.
- 검증: CPU SkeletalBreakTests Debug·Release 각 996/996 PASS(14/18초), SkeletalPatchTests 각 118 PASS, LatticeTopologyTests PASS. 최종 Debug/Release 빌드 각각 39초·exit 0(최초 46/55초). 세 데모 각 8초·exit 0, 491/167·1475/869·161/170·fallback 0이며 SP7b 로그가 MI-4f와 일치한다.
- 회귀 6실행 exit 0: BC-12a/b·AR4·Rig·SP6 PASS, SP7e2 case2/3 0mm(case1 기존 N/A), PS2 ≤0.000234mm·PS3 ≤0.000189mm·PS4 0mm. 모든 실행 로그 D3D12 ERROR 0. 근거: `%TEMP%/MI4g-*.log`, `MI4g-validation.json`, `MI4g-report.md`. 다중 P 실제 호출·mask 1회·n+1 게시는 MI-4h다.

MI-4f(완료 `61dd8e91`, 2026-11-16):
- 세대 계약: 성공한 break마다 `TopologyGeneration`을 1 올리므로 n개 batch가 전부 성공하면 +n이다. batch 요청은 호출 시점 세대로 Prepare하고 순차 commit은 세대 일치를 다시 요구하지 않는다. 공개 `CommitBreak`는 `prepared.TopologyGeneration`이 현재와 다르면 Stale로 무변경 거절한다(MI-4e (b)(c) 해결). `BreakConstraint`는 Prepare 직후 Commit이라 동작이 같다.
- `BreakConstraints(requests, statuses)`: (가) 모든 요청 PrepareBreak, (나) 통과한 요청 쌍의 겹침 검사, (다) 거절이 하나라도 있으면 아무것도 commit하지 않음, (라) 통과하면 batch 전체 handle 생존 점검 후 순차 commit.
- 겹침 규칙: Remove 관절 중복, 한 요청의 Remove가 다른 요청의 Enable에 포함, Enable 중복, 바꿀 body 공유 중 하나면 양쪽 요청을 InvalidInput으로 거절한다. 순서로 살리지 않는 보수적 거절이다(같은 rig의 부모 관절과 자손 관절 요청은 항상 겹침이다).
- 반환 형태: `bool`(전부 Applied일 때만 true)과 요청 순서의 `std::vector<BreakStatus>`. 거절된 요청은 자기 사유, 통과했지만 batch가 거절된 요청은 신규 `BreakStatus::BatchRejected`다. 빈 batch는 false·빈 statuses다.
- (a) handle 생존: `PhysicsWorld::CanCommitConstraintBreak`가 Remove/Enable 관절과 snapshot body·분리 관절 반대 endpoint 생존을 확인하고 `CommitConstraintBreak` 첫 단락이 이를 쓴다(단일 경로 포함). `SkeletalPhysicsSystem::CheckCommittable`이 rig 검사와 합쳐 batch 사전 점검에도 쓰이고, `CommitPrepared(…, requireTopology)`가 내부 순차 commit 경로다.
- CPU `SkeletalBreakTests` Debug·Release 각 996/996 PASS(MI-4e 959에서 +37): 두 번째 요청 실패 시 첫 rig 상태 보존(세대·관절 handle/enable·body 운동·pose·속도·sensor·Pairs·TemporaryPairs), 중복 JointId·Enable/Remove 충돌·세 요청 중 충돌 쌍만 InvalidInput, 빈 batch 거절, 독립 두 rig batch는 둘 다 제거·세대 각 +1·말단 body Dynamic, 분기 fixture(body 0의 자식 1과 3)의 같은 rig 독립 관절 둘 batch는 둘 다 Applied·세대 정확히 +2·constraint 수 -2·두 말단 body Dynamic(세대 검사 생략 순차 commit 경로 실행), 끼어든 break 뒤 오래된 snapshot Stale, 준비 뒤 Enable 대상 관절이 사라진 commit NotReady 무변경. 기존 단일 break 검사는 그대로 PASS. SkeletalPatchTests·LatticeTopologyTests도 PASS.
- 순차 commit 중 불변식 위반으로 실패하면 첫 실패에서 멈추고 남은 요청은 NotReady로 표시한다(방어 코드, 테스트로 도달 불가).
- `SensorOverlapCount`는 commit마다 초기화되므로 batch 뒤에는 마지막 commit의 값만 보인다.
- 빌드 exit0(Debug 68초, Release 80초), 신규 경고 없음. 지정 세 데모 491/167, 1475/869, 161/170·fallback0·exit0, SP7b/SP7e 로그가 MI-4e와 동일. 회귀 6실행 exit0, SP6 902 PASS/0 FAIL, SP7e2 case2/3 0mm, PS2 ≤0.000234mm, BC-12a/12b·AR4·Rig PASS. D3D12 ERROR 0. 근거: `%TEMP%/MI4f-*.log`, `MI4f-report.md`.
- 남은 일: `QueueBreak` batch·결과 수집은 MI-4g, mask·n+1 게시는 MI-4h.

MI-4e(완료 `1d8c8ce2`, 2026-11-16):
- 설계: `PhysicsWorld::PrepareConstraintBreak`(const, world 불변)가 기존 거절 사유를 그대로 검사하고 통과하면 `ConstraintBreakStatus::Prepared`와 body snapshot·impulse를 담은 `PreparedConstraintBreak`(pimpl, world 자원 없음)를 준다. `CommitConstraintBreak`는 관절 제거·Dynamic 전환·impulse를 게시하며 빈 snapshot이나 사라진 관절만 NotReady로 거절한다. `ApplyConstraintBreak`는 둘을 잇는 래퍼다. 겹침 쿼리와 `TemporaryPairs` 추가는 commit에 둔다(거절 시 변경 없음 유지).
- `SkeletalPhysicsSystem::PrepareBreak`(const)는 기존 검사 순서 그대로 `BreakStatus::Prepared`와 `PreparedBreak`(Owner·JointId·준비 시점 Rig/Topology 세대·world snapshot)를 준다. `CommitBreak`는 rig 존재·Rig 세대·관절 생존만 재확인하고 성공마다 `TopologyGeneration`을 올린다. Topology 세대 일치는 commit에서 요구하지 않는다: 같은 rig의 batch 순차 commit 계약은 MI-4f에서 정한다. `BreakConstraint`는 두 함수를 잇는 래퍼다.
- CPU `SkeletalBreakTests` Debug·Release 각 959/959 PASS: 요청 거절 6건(NotFound 2·Stale 2·InvalidInput 2)과 world 거절 UnsupportedPose·UnsupportedTopology 각 1건, 통과한 검증·반복 검증이 세대·관절 handle/enable·body 운동/pose/속도·pair·TemporaryPairs·sensor 상태를 바꾸지 않음, commit 1회만 세대 +1, 같은 관절 두 번째 commit AlreadyBroken, unregister 뒤 NotFound, 오래된 Rig 세대 Stale, JointId와 준비 관절이 어긋난 commit NotReady(무변경).
- 빌드 exit0(Debug 67초, Release 146초), 신규 경고 없음. 지정 세 데모 491/167, 1475/869, 161/170·fallback0·exit0, SP7b 로그가 MI-4d2와 동일(BreakStatus=1). 회귀 6실행 exit0, SP6 870 rows PASS/0 FAIL(리뷰 수정 후 재검증), SP7e2 case2/3 0mm, PS2 ≤0.000234mm, BC-12a/12b·AR4·Rig PASS. D3D12 ERROR 0. 근거: `%TEMP%/MI4e-*.log`, `MI4e-report.md`.
- MI-4f에서 처리할 것(아래 MI-4f 절에서 해결): (a) 같은 rig 순차 commit에서 앞선 commit이 지운 Remove/Enable handle 때문에 뒤 commit이 일부만 바뀌거나 예외가 나는 문제이므로 Commit 전에 모든 handle 생존을 확인한다(단일 래퍼는 Prepare 직후 Commit이라 해당 없음). (b) `PreparedBreak::TopologyGeneration`은 저장만 하고 쓰지 않는다. (c) Prepare와 Commit을 분리 호출하면 Prepare 시점의 pose/속도 snapshot이 그대로 게시된다.
- 테스트 범위: fixture body는 모두 sensor라 겹침 쿼리는 실행되지만 외부 Dynamic body와의 겹침이 없어 `TemporaryPairs`·`OverlapAdded` 증가 경로는 CPU 검사로 검증되지 않았다.
- 남은 일: batch 사전 검증과 세대 계약은 MI-4f, `QueueBreak` batch는 MI-4g, mask·n+1 게시는 MI-4h.

MI-4d2(완료 `188adc01`, 2026-11-16):
- `SkeletalPatchBuffers::Crossings`가 P_i별 Root/Child/ChildSide/Point/Valid를 보관하고 `RelabelCapLoops`는 교차점마다 최소 면적 유효 loop를 고른다. loop 공유·아일랜드 요구 충돌은 다중일 때 진단 실패 예외로 격리한다. `DiagnoseCapLoops(bodyCount)`는 side별 몸체 유지 loop 수와 같은 side B1/B2 cap 분리 로그를 남긴다.
- `CheckJointUnion`이 검증한 형제 P 쌍을 반환하고 `-skeletal-union-checks 1`일 때만 `Request`가 그 평면으로 3몸체(A/B1/B2) 절단을 제출한다. 기본 n=1 경로는 불변이다. `FinishCutResult`는 몸체가 둘이 아니면 압축 결과만 확인하고 게시·break·mask 없이 끝낸다.
- 컴파일 오류 없음. 리뷰 수정: 단일 제출은 Poll의 GPU NodeId로 P_0 확정(Influence), 다중 몸체 조기 종료 정리 순서·Submit 실패 시 몸체 side 복원. SkeletalPatchTests Debug·/O2 각 118 checks, LatticeTopologyTests PASS, Debug 40초·Release 42초 빌드 exit0.
- 지정 세 데모는 491/167, 1475/869, 161/170·fallback0·exit0. n=2(P=13,17 child=14,18): loop0→B1·loop6→B2 선택, keep 2/drop0·conflicts/fallback 0, side1 cap 몸체별 B1=1/B2=1, `same-side B1/B2 cap split=PASS`, 분류 body1=169+18/body2=161+14, 압축 body0/1/2=300/187/175 tris(합계 662=입력, PASS), `publish=0 break=0 mask=0`·exit0.
- 지정 bash Release 회귀 6실행 exit0(31초), SP6 792 PASS/0 FAIL, SP7e2 case2/3 0mm(case1 기존 N/A), PS2 ≤0.000234mm, 기존 PASS 유지. 모든 실행 D3D12 ERROR 0. 근거: `%TEMP%/MI4d2-*.log`, `MI4d2-report.md`.
- 남은 일: 몸체별 skin 준비는 미연결(MI-4e 이후), break·mask·n+1 게시는 MI-4e~4h.

MI-4d1(`58807a5e`, 2026-11-16):
- MI-4d를 두 부분으로 분할했다. 기존 `DiagnoseIslands`·`RelabelCapLoops`는 단일 P/교차점/ChildSide를 사용한다. P_i별 snapshot, 실제 loop 대응·충돌 처리, n=2 cutter 진단까지 함께 바꾸면 약 300줄을 크게 넘으므로 이번은 압축 입력의 요청 몸체 수 계약만 구현한다.
- `ClassifyCutBodies`·`PatchBodyClassifyCS`는 요청 BodyCount를 받아 기존 10개 진단 뒤 몸체별 표면/cap 카운터를 둔다. leader×side 표 형식은 이미 임의 몸체 ID를 담을 수 있어 유지하며 실제 다중 loop 표 생성은 MI-4d2다.
- `CompactCutBodies`는 표면 라벨의 최댓값으로 몸체 수를 추정하지 않는다. 요청 그룹 수를 사용해 마지막 빈 몸체도 body×section bin과 `PatchBodyResult`의 빈 Sections/0 Triangles로 보존한다. 범위 밖 라벨은 거절하고 `PollBodyCompaction`은 빈 몸체를 bounds 없이 명시적으로 로그한다.
- `m_PreparedIds`는 벡터로 바꾸되 `FinishCutResult`의 실제 준비/게시 경로는 기존 두 몸체만 허용한다. `BuildPieceSkin`의 임의 body 인덱스 입력은 유지한다. 다중 몸체별 staging 생성·skin 준비는 MI-4d2, 실제 break·mask·n+1 게시는 MI-4e~4h다.
- 보수적 선택: 기본 요청 P 하나·n=1 loop/아일랜드 규칙·두-side 폴백을 유지한다. `CheckBodyCompaction`은 union 옵션에서도 실행하며 합성 body0/1/2=2/3/1, 같은 side body1 표면/body2 cap, 빈 body3 및 기존 drop/바이트·삼각형 overflow/정점·depth·출처·IB 보존을 검사한다. 합성 압축은 실제 loop↔P_i 대응 검증이 아니다.


- 검증: SkeletalPatchTests Debug·/O2 각각118 checks, LatticeTopologyTests PASS. 지정 외부 출력 props로 최종 Debug/Release 빌드 exit0(32.63/38.30초), 오류0·기존 경고 각9. 외부 출력은 `%TEMP%/MI3e2-fix-build/`; 기존 pwsh 미설치 후처리 메시지는 남아 있다.
- 새 Release의 지정 세 데모는 491/167, 1475/869, 161/170·fallback0·exit0(8.70/8.60/8.68초). union 진단9.66초·exit0, 기존 P13/P17 추출526/중복0·Frozen 팔레트 PASS; 새 합성 압축 body0/1/2=2/3/1·빈 body3=0 PASS. 실제 P13/P17 합집합 절단·분류·압축과 실제 cap loop별 B_i 검증은 MI-4d2에 남긴다.
- 지정 bash Release 회귀·기존 BASH_ENV 래퍼: 6실행 exit0(전체53.49초), SP6 누적766 PASS/0 FAIL, Static/AR/Rig/BC PASS. SP7e2 case2/3 publish/after60=0mm, case1 기존 N/A. PS2/PS3/PS4 최대 오차=0.000234/0.000189/0mm, 각7/6/12건 모두 PASS.
- MX450/32.0.15.9186(레지스트리 재확인), 네 데모·회귀 D3D12 ERROR/CORRUPTION 각각0, 기존 DXGI #80 유지. 초기 Release 링크는 실행 파일 잠금으로 실패했으며 해당 검증 종료 후 다시 빌드하고 전체 검증을 재실행한 최종 로그만 근거로 사용한다.
- ModelViewer.cpp SHA256 전후 동일(1C29C322F6CBCBF8181114194BF750B1F8AA0B9030AFC2703EA766D13C855509). Geometry 변경·커밋·stash/reset·브랜치 변경 없음. 근거: `%TEMP%/MI4d-cpu.log`, `MI4d-{Debug,Release}-build.log`, `MI4d-build-summary.log`, `MI4d-{UpperArm,Thigh,DefaultArm,Union}-{patch,debug,run}.log`, `MI4d-Release-regress{,-debug}.log`, `MI4d-report.md`.
MI-4c(완료 `caa6f681`, 2026-11-16):
- `BuildCutBodyGroups`는 원본 계층의 노드→그룹(0=A, i=B_i), A의 모든 P_i 부모 LCA·B_i의 P_i 부착, 팔레트 순서를 보존한 Frozen=(group!=body)를 만든다. 중복·중첩 P, 잘못된 계층·팔레트는 거절한다. 공통 조상이 없는 forest의 A 부착은 UINT32_MAX이며 이번 런타임 단일 P는 기존 부모 유효성 검사를 유지한다.
- `Request`의 attachment·skin snapshot·팔레트 오차·piece ID를 벡터로 바꾸고 별도 snapshot count를 제거했다. 몸체별 반공간 배열이 기존 `m_PositiveA`를 대체한다. `DiagnoseIslands`도 공통 그룹 분류기를 쓴다. 기본 런타임은 기존 선택 P 하나만 전달하며 D5는 기존 단일 전체 추종 snapshot이다. 압축·준비 ID 및 두-side 게시 자원과 단일 break는 MI-4d 이후 범위로 유지한다.
- CPU `SkeletalPatchTests` Debug·/O2 각각118 checks PASS(2.17/5.27초), `LatticeTopologyTests` PASS(1.46초). 형제 그룹·팔레트 중복/순서·n=1 기존 자손 규칙·부모가 다른 LCA·오류 거절을 검사한다.
- `CheckBodyPalettes`는 기존 `-skeletal-union-checks 1`에서 복사한 후속 포즈로 실제 `BuildPatchPalette`를 검증한다. CesiumMan P13/P17: attach A/B1/B2=2/13/17, Frozen=8/15/15(전체19), 최대 위치 오차=1.45024e-6/9.53674e-7/4.76837e-7 PASS. 다중 절단·break·게시에는 연결하지 않는다. MI-4b GPU==CPU·중복0·n=1 출처 동등도 PASS.
- 지정 MSBuild /restore /m Debug·Release /p:Platform=Windows 및 `%TEMP%/MI3e2-fix-output.props`: 최종29.94/22.72초, 오류0·기존 경고9/4. 출력은 `%TEMP%/MI3e2-fix-build/`에 보존한다.
- Release를 ModelViewer 작업 디렉터리에서 지정 옵션으로 실행: (a) 위팔491/167(8.96초), (b) Xbot허벅지1475/869(8.95초), (c) 기본팔161/170(9.67초), union 진단9.07초. 모두 종료0·게시fallback0. MX450/32.0.15.9186(레지스트리 확인).
- 지정 bash Release 회귀·기존 BASH_ENV 래퍼: 6실행 종료0(54.06초), Static/PS/AR·BC12a/b·Rig PASS, SP6 누적688 PASS/0 FAIL. SP7e2 case2/3 publish/after60=0mm, case1 기존 N/A. 네 데모·회귀 D3D12 ERROR/CORRUPTION 각각0; 기존 DXGI #80·D3D12 경고 유지.
- `ModelViewer.cpp` SHA256 작업 전후 동일(1C29C322F6CBCBF8181114194BF750B1F8AA0B9030AFC2703EA766D13C855509). Geometry/ 변경·커밋·stash/reset·브랜치 변경 없음. 근거: `%TEMP%/MI4c-{lattice,skeletal-Debug,skeletal-Release}.log`, `MI4c-{Debug,Release}-build.log`, `MI4c-{UpperArm,Thigh,DefaultArm,Union}-{patch,debug,run}.log`, `MI4c-Release-regress{,-debug}.log`, `MI4c-report.md`.

MI-4b(완료 `86522153`, 2026-11-16):
- `BuildJointUnionMasks`는 각 P_i 본 자체만 선택하는 기존 단일 선택 마스크를 OR하여 section별 0/1 팔레트 마스크 하나로 합친다. 자손은 자동 포함하지 않는다. 추출 규칙은 P_i node palette union이다. 중복 P·중복 팔레트는 멱등이며 잘못된 P·계층 순환은 거절한다.
- `SkeletalPatchJob::Submit`의 선택적 unionRoots가 이 마스크를 사용한다. 생략하면 기존 단일 본 마스크/Influence 입력을 유지한다. `PatchExtractCS::Selected`와 원본 삼각형당 한 번인 prefix 추출은 수정 없이 재사용한다. 하나의 TriangleSources/패치 결과만 생성하며 원본 mask·patch id·다중 절단/break는 이번 단계에서 변경하지 않는다.
- 새 옵션 `-skeletal-union-checks 1`을 기존 `-debug 1 -skeletal-break-demo 2 -physics-query-exit 180`에 추가한다. `CheckJointUnion`은 제출 prepared nodes에 mesh node·SourceFrame 보정을 적용하고 MI-4a의 무한 평면 선택 결과에서 동일 부모의 형제 P 둘을 찾는다. 진단 작업은 게시하지 않는다.
- CesiumMan 양 허벅지: P13=leg_joint_L_1, P17=leg_joint_R_1, 부모2, component-local planeY=0.470281. 단독 P13=364/P17=352, 합집합=526(공유190), 중복 P={13,17,13}도526. 모든 경우 GPU/CPU 출처 순서·삼각형 집합 일치, 중복0; 별도 P 추출 집합의 합집합과도 일치한다. 자손이 있는 P13의 기존 마스크 경로와 단일 unionRoots 경로를 각각 GPU에 제출하여 출처 목록과 순서가 일치했다(legacy=364/union=364, sourcesEquivalent=PASS). 진단 종료0.
- CPU: SkeletalPatchTests Debug·/O2 각각95 checks PASS(2.39/6.13초), LatticeTopologyTests PASS(2.09초). 합집합·자손 제외·중복 팔레트·빈 집합·오류 계층·제거 상태 및 자손이 있는 계층에서 기존 단일 선택기와 n=1 마스크 동등성을 검사했다.
- 지정 MSBuild /restore /m Debug·Release /p:Platform=Windows 및 `%TEMP%/MI3e2-fix-output.props`: 39.32/65.37초, 오류0·경고각44. 산출물은 `%TEMP%/MI3e2-fix-build/`에 둔다.
- Release 세 데모는 기존 MI-3f와 동일: 위팔491/167, Xbot허벅지1475/869, 기본CesiumMan팔161/170; 모두 종료0·게시fallback0. 지정 bash 회귀+기존 외부 exe 래퍼의 6실행도 종료0, Static/PS/AR·BC12a/b·Rig PASS. SP7e2 case2/3 publish/after60=0mm, case1 기존 N/A.
- MX450/32.0.15.9186(591.86, 레지스트리 재확인). D3D12 ERROR/CORRUPTION0; 기존 DXGI #80·D3D12 경고는 남아 있다. ModelViewer.cpp SHA256은 작업 전후 동일(1C29C322F6CBCBF8181114194BF750B1F8AA0B9030AFC2703EA766D13C855509). Geometry/·신규 소스/셰이더·커밋·stash/reset·브랜치 변경 없음.
- 초기 진단은 mesh node 보정 누락으로 형제 쌍을 못 찾아 종료했으며 보정 후 전체 검증을 재실행했다. 근거: `%TEMP%/MI4b-{lattice,skeletal-Debug,skeletal-Release}.log`, `MI4b-{Debug,Release}-build.log`, `MI4b-{UpperArm,Thigh,DefaultArm,Union}-{patch,debug,run}.log`, `MI4b-Release-regress{,-debug}.log`, `MI4b-report.md`. P2/P3 수정의 최종 근거는 `%TEMP%/MI4b-P23-*` 로그와 `%TEMP%/MI4b-fix.md`다.

MI-4a(완료 `7728485a`, 2026-11-16):
- `BuildSegmentJointParents`로 기존 유한 선택기와 영향·팔레트 교집합의 최근접 조상 계산을 공유한다. `SelectCrossingJoints`는 유한 사각형 제한과 말단 fallback 없이 무한 평면 교차만 반환한다.
- `IntersectLatticeSegment`의 |d|<=1e-5 끝점·공면 제외·부모→자식 B side 규칙을 재사용한다. 동일 P의 여러 선분은 평면 중심에 가장 가까운 교점 하나를 보존하며 동점은 작은 ChildNodeId다. 원본 부모 계층에 교차 P 조상이 있으면 자손 P를 제거하고 NodeId 순서를 보존한다.
- 반환값은 P_i, ChildNodeId snapshot, 교점, B_i 반공간이다. `IsSelectedSegmentChild`로 비직접 자식 snapshot 재사용을 검증했다. 팔레트 합집합·그룹/LCA 부착·break·런타임 호출은 후속 단계다.
- 최종 CPU: SkeletalPatchTests Debug 2.03초·/O2 7.25초, 각각 83 checks PASS; LatticeTopologyTests 1.70초 PASS. 형제 P={1,2}/child={3,4}, 조상+자손 P={2}/child=0, 부호 반전 Bside={0,0}→{1,1}, 비영향·비팔레트 중간 노드 건너뜀 P=0/child=3 로그를 남겼다.
- 지정 msbuild /restore /m Debug·Release /p:Platform=Windows 및 기존 외부 출력 props: 최종 65.01/32.33초, 오류 0(기존 경고 42/4). 출력은 `%TEMP%/MI3e2-fix-build/`다.
- 지정 bash Release 회귀와 기존 exe 경로 래퍼를 1회 실행: 6실행 모두 종료 0, 92.31초. SP6 누적506 PASS/0 FAIL, MI3b/3c·BC-12a/b·AR4(Static 재절단)·Rig PASS; PS2/3/4 각각 7/6/12건 PASS, SP7e2 case2/3 publish/after60 0mm, case1 기존 N/A. MX450, D3D12 ERROR/CORRUPTION 0; 기존 DXGI #80 관찰. 드라이버는 이번에 재조회하지 못했다(기존 기록 591.86).
- `ModelViewer.cpp` SHA256은 작업 전후 동일하며 Geometry/ 변경·커밋·stash/reset·브랜치 변경은 없다. 근거: `%TEMP%/MI4a-final-{lattice,skeletal-Debug,skeletal-Release}.log`, `MI4a-final-{Debug,Release}-build.log`, `MI4a-Release-regress{,-debug}.log`, `MI4a-report.md`.

MI-1: `BuildInfluencingJoints`가 R16/R32 draw의 startIndex/baseVertex를 반영한 section 로컬 인덱스를 보존한다.
`BuildLatticeTopology`는 bind 최장축 64칸의 점유 셀과 중복 없는 삼각형 간선을 만들며 `Model::m_LatticeTopology`가 소유한다.
격자용 위치는 section 좌표계 차이를 없애려고 rest node 변환·IBM의 정규화 가중합(공통 모델 bind 공간)으로 구하고, 스키닝용 원본 position은 바꾸지 않는다.
`LatticeUnionFind`는 정수 노드 id를 받아 다음 단계의 (셀, side)에도 재사용한다. `CountLatticeComponents`는 전체 성분을 센다.
`LatticeTopologyTests`는 분리된 띠=2, section 이음새=1, 평면 분리=2와 빈 입력·점 bounds를 검증한다.
2026-11-13: CPU 테스트 PASS(컴파일·실행 약 4.1초); Debug 271.1초·Release 325.6초 빌드 성공(각 오류 0, 기존 경고 129). 테스트 산출물은 `%TEMP%/MI1-LatticeTests/`, 빌드 로그는 `%TEMP%/MI1-{Debug,Release}-build.log`.
Release를 ModelViewer 작업 디렉터리에서 `-debug 1 -skeletal-break-demo 2 -physics-query-exit 180`으로 실행: MX450/591.86, 약 19.7초·종료 코드 0, A/B 게시 완료. CesiumMan: `LatticeTopology: Dim=14x49x64 cells=1360 edges=4075 components=1`.
실행 로그 `%TEMP%/MI1-CesiumMan-debug.log`에는 DXGI 출력 어댑터 오류 #80과 D3D12 리소스 전환 경고도 있다. 이 단계에서는 절단 회귀 전체를 실행하지 않았다.

MI-2(base `432ff0c5`, 리뷰 수정 포함, 2026-11-13): `SkeletalPatchJob::Submit`은 `GetPreparedJoints`·`GetPreparedNodes`·SourceFrame·컴포넌트 world와 부모 계층을 고정한다.
`Poll`은 기존 TriangleSources readback의 section/draw/triangle을 CPU skin section 연속 삼각형 번호로 바꾼다.
`SkeletalPatchCopy::Update`의 실제 복사본 평면 확대 뒤 `DiagnoseIslands`가 패치 삼각형이 참조한 정점만 중복 없이 CPU 스키닝하고 dot 부호(허용오차 1e-5)를 계산한다.
미계산 부호는 2로 표시하고, 팔레트 joint→A/B1 표는 요청당 한 번 만들어 투표 때 부모 계층을 다시 탐색하지 않는다.
`LabelPatchIslands`의 연결 판정과 점수 누적을 분리했다. 유효한 (패치 삼각형, side)는 원본 세 꼭짓점 전체의 정규화 bind weight를 그룹별로 누적하며 반대 부호 꼭짓점도 포함한다.
`ClipTrianglesCS`의 완전 공면 side0 특례, 엄격한 부호 교차, 다각형 꼭짓점 수 분기를 확인했다. `{0,-1,-1}`의 side0은 점, `{0,0,-1}`의 side0은 선분만 남으므로 두 경우 모두 side1에만 소속한다.
없는 side는 삼각형 소속·투표·SideDefault에서 제외하고 `TriangleSideBody`를 `UINT32_MAX`로 유지한다. 평면 위 정점은 유효한 양쪽 side 연결에 계속 참여한다.
동점은 A, side 기본 몸체는 패치 삼각형 수 다수결이다. 결과는 `SkeletalPatchBuffers::Islands`와 job에 다음 요청까지 보관하며 GPU 절단·게시·마스크에는 적용하지 않는다.
CPU 테스트 PASS(컴파일·실행 6.3초): 기존 MI-1/MI-2 조건과 전체 꼭짓점 투표(A=1/B=2 양쪽 동일), 점·선분 접촉의 side0 제외·side1 투표·기본 몸체, 양쪽 동점=A를 검증했다.
Debug 24.3초·Release 29.5초 증분 빌드 성공(각 오류 0, 기존 경고 6). 로그: `%TEMP%/MI2-fix-{Debug,Release}-build.log`, CPU 산출물: `%TEMP%/MI2-fix-LatticeTopologyTests.exe`, `%TEMP%/MI2-fix-unit.log`.
Release를 ModelViewer 작업 디렉터리에서 `-debug 1 -skeletal-break-demo 2 -skeletal-break-demo-bone Skeleton_arm_joint_L__4_ -physics-query-exit 180`으로 실행했다. 최종 실행 11.5초, 종료 0; A/B components=2, caps=2, uncapped=0, error=0.
위팔 결과: `MI2 islands=3 reassigned=1 side0Default=1 side1Default=0`.
허리: `MI2 island side=0 tris=90 scoreA=252.476424 scoreB=17.523576 body=A representative=2 name=Skeleton_torso_joint_1 weight=175.691257`.
나머지는 side1 몸통 350 tris(A=849.309642/B=200.690358, A), side0 팔 145 tris(A=4.831554/B=430.168446, B1). 전체 꼭짓점 투표로 점수가 바뀌어도 허리 A는 유지된다.
실행 로그: `%TEMP%/MI2-fix-UpperArm-patch.log`, `%TEMP%/MI2-fix-UpperArm-exit.log`.
지정 `regress.sh Release`의 6개 실행 모두 종료 0(12/13/14/10/8/9초, 총 66.0초). SP6 비교 로그 누적 46행 PASS·FAIL 0, BC-12a/b·AR4·Rig PASS.
SP7e2 case=1 publish/after60은 기존 N/A(noCoincidentKeptBoundary), case=2/3은 PASS. 회귀 로그: `%TEMP%/MI2-fix-Release-regress.log`.

MI-3a(미커밋, base `b7413004`, 2026-11-16): 중단된 부분 구현을 검토하고 `PatchBodyClassifyCS`의 예약어 변수명을 수정했다.
ModelViewer 프로젝트와 filters에 compute 셰이더를 등록해 기존 `CompiledShaders` 헤더 생성·PSO 로딩 경로를 사용한다.
`SkeletalPatchCopy::ClassifyCutBodies`가 `Islands.TriangleSideBody`를 GPU에 올리고 `OnCutResult`에서 skin 복원 전 실제 section 출력 범위만 판정한다.
side0=`result.Mesh`, side1=`result.Other`이며 물리 A/B 순서와 독립이다. provenance는 정점당 32B, `TriangleId`는 +20B다.
세 정점 ID 불일치·범위 밖 ID·출력 없는 표 항목(`UINT32_MAX`)을 진단하고 invalid 몸체는 sentinel 그대로 남긴다.
cap(`Origin.x==UINT_MAX`)은 별도로 세며 SideDefault를 기록한다. `body0/body1/reassigned`는 cap을 제외한 표면 수다.
삼각형별 몸체 버퍼는 side별 globalVertex/3 인덱스로 유지하고 section 사이 빈 capacity는 소비 대상에서 제외한다. 후속 MI-3b용이며 현재 출력·게시에는 사용하지 않는다.
readback은 32B 카운터만 동기로 읽고 임시 자원은 fence 완료까지 유지한다.
Debug 41.93초·Release 49.57초 빌드 성공(각 오류 0, 경고 39). 최초 Debug 시도는 예약어 충돌로 실패했으며 수정 후 두 구성 모두 통과했다.
Release 실행: ModelViewer 작업 디렉터리, `-debug 1 -skeletal-break-demo 2 -skeletal-break-demo-bone Skeleton_arm_joint_L__4_ -physics-query-exit 180`.
MX450/591.86, 13.25초·종료 0, A/B components=2/caps=2/uncapped=0/error=0. D3D12 ERROR 0; 기존 DXGI #80 및 D3D12 경고 #974/#1008 관찰.
```text
MI3a side=0 tris=298 cap=41 body0=104 body1=153 invalid=0 idMismatch=0 reassigned=104
MI3a side=1 tris=414 cap=41 body0=373 body1=0 invalid=0 idMismatch=0 reassigned=0
```
MI-2의 side0 허리 원본 90 tris(A) 라벨에 대응하는 절단 후 표면 104 tris가 reassigned로 잡힌다. 원본 수와 출력 수는 clip 분할로 다르다.
지정 `regress.sh Release` 6개 실행 모두 종료 0(11/14/11/12/11/11초, 합계 70초), BC-12a/b·AR4·Rig PASS; SP6 누적 PASS 72행/FAIL 0.
SP7e2 case=1 publish/after60은 기존 N/A(noCoincidentKeptBoundary), case=2/3 PASS.
근거 로그: `%TEMP%/MI3a-{Debug,Release}-build.log`, `%TEMP%/MI3a-UpperArm-{patch,debug,exit}.log`, `%TEMP%/MI3a-Release-regress.log`.

MI-3b(미커밋, base `2a0ab7d2`, 2026-11-16): `PatchBodyCompactCS::main`은 Mode 0=count, 1=단일 스레드 prefix(`BuildOffsets`), 2=scatter로 호출한다.
몸체 수는 `BodyCount`이며 두 side의 대응 section을 같은 body×section bin에 누적한다. 표는 bin당 24B(count, provenance firstTriangle, surface/depth/IB byte offset, cursor)다.
기존 cutter처럼 삼각형별 독립 정점을 복사하고 section 로컬 R32 IB를 쓴다. surface/depth stride는 DWORD 정렬이며 두 side의 대응 section 레이아웃이 같아야 한다.
입력 `FirstTriangle`은 MI-3a의 capacity 틈을 포함한 globalVertex/3이고, IB는 section 로컬이다. 32B provenance는 비트 그대로 복사하며 cap도 MI-3a가 기록한 SideDefault 몸체를 사용한다.
용량은 Data 바이트·provenance 삼각형 수를 각각 제한한다. Status bit0=overflow, bit1=invalid body이며 오류가 있으면 scatter를 막는다. 호출자는 패스 사이 UAV barrier와 입력/출력 자원 수명을 보장한다.
`CheckBodyCompaction`은 `-skeletal-patch-break-checks 1`에서 한 번 실행된다. capacity 틈, 재정렬 IB, 양 side의 혼합 몸체, cap, depth 있음/없음, 빈 body/section을 합성한다.
정점·depth·32B 출처·IB 보존, 몸체별 3+3=입력 6 tris, 삼각형/바이트 부족 각각 overflow 및 출력 억제 PASS. 실제 cutter 출력 압축 job·메타데이터 게시 연결은 MI-3c/3d에 남긴다.
Debug/Release MSBuild 성공(종료 0); 최초 HLSL 예약어 변수명 오류는 수정 후 통과했다. 기존 C++ 경고 및 post-build `pwsh.exe` 미설치 메시지가 있으며 빌드 종료 코드는 0이다.
Release `-debug 1 -skeletal-patch-break-checks 1`: MX450/591.86, 12.22초, 종료 0, MI3b PASS, D3D12 ERROR 0. 기존 DXGI #80·D3D12 #974/#1008 경고 관찰.
지정 `regress.sh Release`: 6개 모두 종료 0(9/11/8/9/8/9초, 합계 54초), BC-12a/b·AR4·Rig PASS; SP6 누적 104 PASS/0 FAIL.
SP7e2 case=1 publish/after60은 기존 N/A(noCoincidentKeptBoundary), case=2/3 PASS. Geometry 변경이 없어 CPU slicer 검사는 해당하지 않는다.
근거: `%TEMP%/MI3b-{Debug,Release}-build.log`, `MI3b-synthetic-{patch,debug}.log`, `MI3b-Release-regress.log`, `MI3b-report.md`.

MI-3b P2 수정(미커밋, base `2a0ab7d2`, 2026-11-16): bin마다 surface→depth→IB를 배치하여 다음 surface 주소와 provenance 정점 번호가 어긋나는 문제를 수정했다.
`CutJob::BuildResults`는 surface 주소를 `VertexStart*28`로 게시한다. `BuildPatchSkinVertices`는 28B 정렬을 검사하고 `vbOffset/28`을 넘기며, `PatchSkinBuildCS::main`은 이 번호로 Data와 provenance를 함께 읽는다.
`PatchBodyCompactCS::BuildOffsets`는 전체 용량을 검사하며 모든 bin의 surface VB를 먼저 연속 배치하고, 그 뒤에 bin별 depth·IB를 배치한다. 28B surface에서 provenance의 `firstTriangle*3`과 surface 정점 번호가 일치한다.
`CheckBodyCompaction`에 빈 bin을 포함한 `vbOffset%28==0 && vbOffset/28==firstTriangle*3` 검사를 추가했다. 합성 입력의 두 번째 bin surface 주소는 288B에서 168B(정점 6)로 바뀐다.
Debug/Release 지정 MSBuild 명령 모두 종료 0(18.54초/29.83초, 각 오류 0·기존 경고 6). 두 구성 모두 `-debug 1 -skeletal-patch-break-checks 1`에서 `skinAddress=matched ... PASS`, 종료 0(24.69초/14.53초).
MX450/591.86에서 두 합성 진단과 아래 회귀의 debugger 출력을 수집했으며 D3D12 ERROR/CORRUPTION 0. 기존 경고는 남아 있다.
지정 `regress.sh Release` 6개 실행 모두 종료 0(17/13/11/11/11/11초, 합계 74초); SP6 누적 142 PASS/0 FAIL, BC-12a/b·AR4·Rig PASS. SP7e2 case=1은 기존 N/A, case=2/3 PASS.
`ModelViewer.cpp`는 수정하지 않았으며 Geometry 변경도 없다. 근거: `%TEMP%/MI3b-fix-{Debug,Release}-build.log`, `MI3b-fix-synthetic-{Debug,Release}-{patch,debug,exit}.log`, `MI3b-fix-Release-regress{,-debug}.log`, `MI3b-fix.md`.

MI-3c(미커밋, base `34d5199e`, 2026-11-16): `SkeletalPatchCopy::OnCutResult`가 실제 두 side의 공유 geometry와 MI-3a 몸체 버퍼를 `CompactCutBodies`에 넘긴다. 빈 section으로 양쪽 목록 순서가 달라져도 기존 VB 시작 주소로 대응시킨다.
`CompactCutBodies`는 실제 삼각형 수로 Data/provenance 용량을 64비트 계산·검사하고 count→GPU prefix→scatter를 비동기 제출한다. 멤버 `BodyCompactJob`이 입력 공유 참조·몸체 판정 resource·출력·scratch·readback을 fence 완료까지 유지한다. `PollBodyCompaction`은 후속 `Update`에서 완료된 작은 표만 수집한다.
`PatchBodyCompactCS::BuildOffsets`·`main`은 실제 job에서만 bin 뒤의 AABB 표를 초기화·정수 atomic 축약한다. CPU에는 bin당 48B(count/offset/cursor/bounds)와 status 4B만 읽으며 전체 정점 readback은 추가하지 않았다. 합성 검사는 기존 모드 그대로다.
`PatchBodyResult`는 공유 `GeometryBuffers`, 몸체별 section 인덱스 목록·삼각형 수를 보관한다. `Mesh`에 원본 material/PSO, surface/depth/IB 범위·draw·bounds를 기록하고 cap material·provenance 부모·세대를 보존한다. MI-3d는 이 목록으로 몸체별 revision을 선택하고 `BuildPieceSkin(body)`에 연결할 수 있다. 현재 게시·skin 생성은 기존 A/B 경로 그대로다.
최종 Debug 25.58초·Release 32.40초 빌드 성공(각 오류 0, 기존 경고 6). 최초 Debug에서 Vector3 인덱싱 컴파일 오류를 수정했다.
Release를 ModelViewer 작업 디렉터리에서 `-debug 1 -skeletal-break-demo 2 -skeletal-break-demo-bone Skeleton_arm_joint_L__4_ -physics-query-exit 180`으로 실행: MX450/591.86, 9.90초·종료 0, D3D12 ERROR/CORRUPTION 0. 기존 DXGI #80·D3D12 #974/#1008은 남아 있다.
```text
MI3c body=0 tris=518 sections=2 bounds=(-0.152837,0.591892,-0.116663)-(0.114113,1.18967,0.134356)
MI3c body=1 tris=194 sections=2 bounds=(-0.141221,0.716555,-0.282988)-(0.155783,1.00571,0.102364)
MI3c sum body=712 input=712 PASS material/depth/IB/bounds=PASS status=0
```
body0=MI-3a side0 104+side1 373+SideDefault cap 41=518이다. `regress.sh Release` 6개 모두 종료 0(9/11/9/8/8/8초, 수집 총 54.87초), 합성 MI-3b·실제 MI-3c·BC-12a/b·AR4·Rig PASS; SP6 누적 168 PASS/0 FAIL. SP7e2 case=1은 기존 N/A, case=2/3 PASS. 회귀 debugger 수집에서도 D3D12 ERROR/CORRUPTION 0.
근거: `%TEMP%/MI3c-{Debug,Release}-build.log`, `MI3c-UpperArm-{patch,debug,exit}.log`, `MI3c-Release-regress{,-debug}.log`, `MI3c-report.md`. Geometry 변경 없음. `ModelViewer.cpp`는 수정하지 않았다.

MI-3c 리뷰 수정(미커밋, base `34d5199e`, 2026-11-16):
- `OnCutResult`의 분류·압축 예외 경계는 용량·할당·검증 실패를 압축만의 실패로 기록하고 기존 hide→`BuildPieceSkin`→`QueueBreak` 경로를 계속한다. 로그 예외도 기존 안전 로그 함수로 격리한다.
- layouts 초기 데이터는 `ReserveUploadMemory`와 같은 명령 목록의 `CopyBufferRegion`으로 올린다. 제출 직후 fence 대기와 초기 데이터 `Create`의 동기 업로드를 제거했다.
- `GetBodyState`는 None/InProgress/Done/Failed를 구분하고 `GetBodyResult`는 Done에서만 결과를 노출한다. MI-3d의 몸체별 게시는 Done까지 기다려야 하며 기존 A/B 게시 연결은 유지한다.
- `DiscardBodyCompaction`은 완료된 미게시 결과의 `ReleaseGpuResourcesAfterCompletion`을 먼저 호출한다. Request/Reset으로 취소된 미완료 job은 별도 목록에서 fence 완료 후 회수한다. job 소멸자의 대기는 소유자 종료 시 남은 미완료 제출 보호에만 사용한다.
- 지정 MSBuild Debug/Release 성공(최종 18.77초/72.57초, 오류 0, 기존 경고 6/39). Release 위팔은 위와 같은 명령·작업 디렉터리에서 MX450/591.86(32.0.15.9186), 16.96초·종료 0이다. A/B 게시 이후 후속 프레임에서 위의 body0=518/body1=194·합계 712 로그가 동일하게 PASS였다.
- 지정 `regress.sh Release` 6개 모두 종료 0(16/18/12/13/14/14초, 수집 총 87.65초). MI-3b/MI-3c·BC-12a/b·AR4·Rig PASS, SP6 누적 194 PASS/0 FAIL. SP7e2 case=1은 기존 N/A, case=2/3 PASS. 위팔·회귀 D3D12 ERROR/CORRUPTION 0이며 기존 DXGI #80과 D3D12 경고는 남아 있다.
- 근거: `%TEMP%/MI3c-fix-{Debug,Release}-build.log`, `MI3c-fix-UpperArm-{patch,debug}.log`, `MI3c-fix-Release-regress{,-debug}.log`, `MI3c-fix.md`. 이번 수정은 Geometry와 `ModelViewer.cpp`를 편집하지 않았다.

MI-3d(미커밋, base `96214e6f`, 2026-11-16):
- `OnCutResult`는 양쪽 side를 분류·압축 제출하고 결과를 숨긴 채 보관한다. `Update`는 `GetBodyState`의 InProgress 동안 대기하고, Done/Failed/None에서 `FinishCutResult`로 준비를 이어간다. Request/Reset은 대기 결과도 취소한다.
- `FinishCutResult`는 공유 압축 geometry에서 body 0=A, 1=B의 section을 선택한다. `BuildPieceSkin(body)`는 side와 독립된 몸체 snapshot을 사용하며, QueueBreak→Applied 확인→원본 mask 1회→A/B 각 1회 게시 순서를 유지한다. offset 복사 진단은 기존 side 배치를 유지한다.
- 압축 실패·빈 몸체는 기존 side 결과로 폴백한다. 추가 게시 검증: 양쪽 SideDefault가 같으면 상보적인 A/B cap을 만들지 못하므로 side 폴백한다. Xbot 허벅지 case=2는 두 기본 몸체가 B이고 압축 A는 반대 다리 20 tris뿐이다. 그대로 게시한 첫 회귀에서 SP7e2 9.764/1253.511mm·A cap 없음이 확인되어 이 검증을 추가했다. 라벨·고정 joint 정책 변경은 이 단계 범위 밖이다.
- cap은 MI-3a/3c 그대로 같은 side의 SideDefault에 귀속한다. 위팔 body0=104+373+41 cap=518, body1=153+41 cap=194이며 허리 원본 90 tris의 clip 출력 104 tris가 A에 남는다.
- `PollBodyCompaction`의 `vbOffset % 28 == 0`, `vbOffset/28 == firstTriangle*3` 검증과 MI-3b 합성 skinAddress 검사를 유지한다. `PatchSkinBuildCS`는 같은 정점 번호로 Data(28B)와 provenance(32B)를 읽고 PS2/3 실제 복원도 PASS다.
- `ActivatePatchSkin`은 부분집합의 SourceMesh 번호로 PG offset을 보존한다. 빈 section의 끝 주소 UAV 오류는 `UpdateSkinnedGenerated`의 빈 section 바인딩 생략으로 해결했다. PS4는 같은 SourceMesh offset을 읽고 `BakePatchSkin`은 몸체 section 부분집합을 유지한다. `DiscardBodyCompaction`은 게시 중인 공유 버퍼를 직접 해제하지 않는다.
- 지정 MSBuild Debug/Release 성공(21.57/25.90초, 각각 오류 0·기존 경고 5). Geometry/ 변경 없음. `ModelViewer.cpp`는 편집하지 않았다.
- Release 위팔은 ModelViewer 작업 디렉터리에서 `-debug 1 -skeletal-break-demo 2 -skeletal-break-demo-bone Skeleton_arm_joint_L__4_ -capture %TEMP%/MI3d-arm-{on,off} -physics-query-exit 400 -patch-skinning {1,0}` 실행. MX450/32.0.15.9186, 두 실행 종료 0(ON 33.48초/OFF 34.80초)·A/B 각 1회 게시·HDR/post RAW 각 8쌍·D3D12 ERROR/CORRUPTION 0.
- 양쪽 실행 로그: `MI3d publish body0 tris=518 body1 tris=194 fallback=0`. 지정 raw2png.ps1로 첫/마지막 post(000/007)를 PNG 변환·확인했다. ON에서 허리 표면은 몸통에 남고, OFF는 기존 강체 경계 벌어짐이 보인다. SideDefault cap의 허리 단면은 MI-3e/3f 후속 범위다.
- 지정 `regress.sh Release` 6개 모두 종료 0(11/13/11/11/10/10초, 총 67.71초). SP7e2 case=1 publish/after60은 기존 N/A, case=2/3은 모두 0.000000mm PASS. case=2는 side 폴백(1475/869, fallback=1), case=3은 압축 게시(161/170, fallback=0). PS2 7건·PS3 6건·PS4 12건 PASS, FAIL 0; PS4 간격 변화 0mm. MI-3b/3c·BC-12a/b·AR4·Rig PASS, SP6 누적 246 PASS/0 FAIL. 회귀 D3D12 ERROR/CORRUPTION 0, 기존 DXGI #80 및 D3D12 경고는 남는다.
- 근거: `%TEMP%/MI3d-{Debug,Release}-build.log`, `MI3d-arm-{on,off}-{patch,debug,exit}.log`, `MI3d-Release-regress{,-debug}.log`, `MI3d-arm-{on,off}-demo-{000,007}-post.png`, `MI3d-report.md`.
- MI-3d P1 수정(2026-11-16): `Update`의 압축 대기 중과 `FinishCutResult`의 `QueueBreak` 직전에 `DiscardStaleCutResult`로 Applied 후와 동일한 geometry/mask revision을 검사하고, 불일치 시 break 없이 대기 결과·숨긴 staging owner·압축 상태를 정리하고 로그를 남긴다(Applied 후 검사 유지). Debug/Release 빌드 PASS(38.9/49.7초, 오류 0), MX450/591.86에서 지정 Release 위팔 180프레임 실행 PASS(11.84초, body0=518/body1=194 fallback=0), 지정 `regress.sh Release` 6개 종료 0·PASS(73.74초, SP6 누적 272 PASS/0 FAIL; SP7e2 case=1 기존 N/A), 두 실행 D3D12 ERROR/CORRUPTION 0; 근거 `%TEMP%/MI3d-fix-{Debug,Release}-build.log`, `MI3d-fix-UpperArm-{patch,debug}.log`, `MI3d-fix-Release-regress{,-debug}.log`, `MI3d-fix.md`.

MI-3e(미커밋, base `e8cd2e59`, 2026-11-16):
- `ClipTrianglesCS::main`은 스켈레탈 요청에서만 보조 UAV에 표면 provenance와 같은 `TriangleId`를 기록한다. Static·capacity count는 쓰기를 생략한다.
- `RankBoundaryCS::main`의 Commit=2가 pointer jumping 완료 후, CapEars 전에 선분별 16B(TriangleId/leader/active/bad)를 보존한다. `CutJob::Poll`은 fence 완료 후 이 표만 수집하고 `CuttingSystem::UpdateOne`이 작업 해제 전 `CutResultEntry`로 소유권을 넘긴다. 전체 정점 CPU readback은 추가하지 않았다. 선택한 두 GPU 버퍼도 작업 예산에 포함한다.
- `DiagnoseCapLoops`는 active 선분을 leader로 묶어 각 side의 `TriangleSideBody`를 투표한다. 동점은 작은 몸체 번호(A 우선), 유효 표가 없으면 SideDefault다. conflicts는 유효 라벨 중 승자와 다른 (선분, side) 수, fallback은 출처/side 라벨 부재 또는 bad 경계로 투표에서 제외한 (선분, side) 수다. 같은 몸체=drop, 다른 몸체=keep을 로그하며 실제 cap 귀속·압축·게시는 기존 SideDefault 그대로다.
- MX450/591.86(32.0.15.9186), Release를 ModelViewer 작업 디렉터리에서 `-debug 1 -skeletal-break-demo 2 -skeletal-break-demo-bone Skeleton_arm_joint_L__4_ -physics-query-exit 180`으로 실행: 종료 0, 9.91초. 허리 leader=0/segs=29/A-A/drop, 팔 leader=4/segs=16/B-A/keep. loops=2/drop=1/keep=1/conflicts=0/fallback=0. 기존 게시 518/194/fallback=0 유지.
- 같은 디렉터리에서 `-debug 1 -skeletal-break-demo 1 -physics-query-exit 180`: 종료 0, 12.72초. Xbot 허벅지 leader=0/segs=60/B-B/drop, loops=1/drop=1/keep=0/conflicts=0/fallback=0. 기존 MI-3d 게시 1475/869/fallback=1 유지(동일 SideDefault).
- Debug/Release 지정 MSBuild 성공, 오류 0(40.63/61.01초, 기존 경고 55). 주석 정리 후 최종 재빌드도 오류 0(13.28/15.31초, 기존 경고 5). 지정 `regress.sh Release` 6개 모두 종료 0, 63.96초: MI-3b/3c·BC-12a/b·AR4(Static 재절단 포함)·Rig PASS. SP6 누적 298 PASS/0 FAIL, SP7e2 case=1 기존 N/A, case=2/3 publish/after60 모두 0mm PASS. 두 데모·회귀 D3D12 ERROR/CORRUPTION 0, 기존 DXGI #80 관찰.
- Geometry/ 변경·신규 파일 없음. `ModelViewer.cpp`의 다른 세션 미커밋 변경은 보존했다. 근거: `%TEMP%/MI3e-{Debug,Release}{,-final}-build.log`, `MI3e-{UpperArm,Thigh}-{patch,debug}.log`, `MI3e-Release-regress{,-debug}.log`, `MI3e-report.md`.

MI-3e2(미커밋, base `94f17ca0`, 2026-11-16):
- `SkeletalPatchJob::Submit`이 prepared nodes도 고정한다. `DiagnoseIslands`는 실제 incoming B 루트 P의 자식 중 평면을 가로지르는 세그먼트의 교차점과 자식 side를 제출 component world·공통 skin frame으로 계산한다.
- CapEars 전 snapshot을 16B→32B로 늘려 출처·leader·active·bad와 평면 2D 양 끝점을 보존한다. 요청 예산·GPU/readback stride도 함께 갱신했다. 전체 정점 readback은 추가하지 않는다.
- `ContainsLatticeLoop`는 순서와 무관한 선분의 홀짝 교차로 오목 loop와 경계 포함을 판정한다. `TriangleSideIsland`를 보존하고 `RelabelPatchIslands`가 실제 loop에 닿은 아일랜드 전체를 자식 쪽 B/반대쪽 A로 바꾼다. 미접촉은 다수결, 상충 요구는 원래 다수결과 충돌 수를 보존한다. GPU 표와 임시 SideDefault도 갱신한다.
- `OnCutResult`는 이미 snapshot 수집 뒤 호출된다. `RelabelCapLoops`→`DiagnoseCapLoops`→`ClassifyCutBodies`→`CompactCutBodies` 순서로 연결하여 추가 프레임 지연 없이 기존 비동기 압축·원본 revision 검증을 유지한다. MI-3e action은 실제 loop만 keep이다. 실제 cap 귀속·제거는 MI-3f에 남긴다.
- CPU 테스트 PASS(최종 컴파일·실행 4.95초): 오목 polygon 내부/외부/꼭짓점/수평 경계, 아일랜드 전체 전파·정확한 변경 수·미접촉 허리·없는 side·SideDefault 동점, 상충 다수결 보존. Geometry/ 변경 없음.
- 지정 MSBuild Debug/Release 최종 성공(39.92/43.46초, 오류 0, 경고 67/55). 첫 빌드도 오류 0으로 성공했다. 초기 Developer PowerShell 호출은 실행 정책으로 실행되지 않아 process 범위 Bypass 후 CPU 테스트를 실행했다.
- Release, ModelViewer 작업 디렉터리, MX450/591.86: (a) 지정 CesiumMan 위팔 9.67초·종료 0, crossing=(12.688820,0.540722,-0.262048), 팔 realLoop=0 keep(1/0), 허리 loop=6 drop·90 tris A 유지, relabeled=0, body0=518/body1=194 fallback=0. (b) Xbot 허벅지 9.04초·종료 0, realLoop=0, 1 island/1372 triangle-side 재라벨, keep(1/0), body0=1475/body1=869 fallback=0(기존 1). (c) 기본 CesiumMan 팔 9.32초·종료 0, realLoop=0 keep(1/0), body0=161/body1=170 fallback=0. 교차점 상세는 각 patch 로그 참조.
- 지정 `regress.sh Release` 6개 모두 종료 0(8/12/9/9/9/8초, 수집 56.22초). MI3b/3c·BC-12a/b·AR4(Static 재절단 포함)·Rig PASS, SP6 누적 376 PASS/0 FAIL. SP7e2 case=1은 기존 N/A(noCoincidentKeptBoundary), case=2/3 publish/after60 모두 0.000000mm PASS. case=2는 압축 게시 fallback=0이며 PS4 간격 변화도 0mm PASS.
- 세 데모와 회귀의 debugger 수집에서 D3D12 ERROR/CORRUPTION 0. 기존 DXGI #80·D3D12 경고는 남아 있다. `ModelViewer.cpp` SHA256은 작업 전후 동일하다. 커밋·브랜치 변경 없음.
- 근거: `%TEMP%/MI3e2-unit.log`, `MI3e2-{Debug,Release}-final-build.log`, `MI3e2-{UpperArm,Thigh,DefaultArm}-{patch,debug,run}.log`, `MI3e2-Release-regress{,-debug}.log`, `MI3e2-report.md`.

### MI-5 — CesiumMan 시험·기록 (완료, 미커밋, 2026-11-17)

- 완료 조건인 위팔·허벅지·실제 몸통 평면의 게시/+60 캡처와 기록을 수행했다. 아래 결함은 수정하지 않았다. 기존 MI-5 Torso 기록은 허벅지 절단의 몸통 시점이었으며 이번 실제 척추 절단으로 대체한다.
- 환경: HEAD `e123cad1`, master, Release, 작업 디렉터리 `MiniEngine/ModelViewer`, NVIDIA GeForce MX450(1906 MB), `-debug 1`. 드라이버 591.86은 이전 기록이며 이번 재조회 값은 아니다.
- 외부 exe: `%TEMP%/MI3e2-fix-build/Release/Output/ModelViewer/ModelViewer.exe`, SHA256 `7E702D9A5BC97E9C07FB98D2FBC3459E18625266997EF0187C31CCFD5A57E645`로 기존 MI-5 빌드와 일치하여 재빌드하지 않았다. 기존 빌드에는 타 세션의 `ModelViewer.cpp` 캡처 옵션 변경이 포함되어 있다. 순수 HEAD 바이너리와 구분한다.
- 코드·진단 변경 없음. `ModelViewer.cpp` SHA256 `1C29C322F6CBCBF8181114194BF750B1F8AA0B9030AFC2703EA766D13C855509` 보존. 저장소 Build 변경·stash/reset·브랜치 변경·커밋 없음.
- 재현 공통: 위 작업 디렉터리에서 외부 exe에 `-debug 1 -skeletal-break-demo 2 -physics-query-exit 180 -capture C:/Users/Pes/AppData/Local/Temp/MI5-<사례> -capture-count 2 -capture-step 60` 및 아래 추가 옵션을 전달한다. UpperArm만 뒤 프레임 관찰을 위해 exit=300/count=4로 연장했다.
- PNG 공통 절대 경로: `C:/Users/Pes/AppData/Local/Temp/MI5-<사례>-demo-{000,001}-post.png`. 전부 게시 f64/+60 f124이며, UpperArm의 `002/003`은 +120 f184/+180 f244다. 8개 성공 실행·18 PNG를 각각 직접 열어 확인했다.
- RAW 변환은 기존 `scratchpad/raw2png.ps1 -Dir $env:TEMP -Pattern 'MI5-*-post.raw' -Scale 1`을 사용했다(1920×1080). 아래 P 번호는 원본 glTF 배열이 아닌 런타임 DFS NodeId다.

| 사례(캡처 이름) | 추가 옵션 | P 집합·경로 | 몸체별 tris | 아일랜드/loop·cap | constraint·mask | 결과 |
| --- | --- | --- | --- | --- | --- | --- |
| UpperArm | `-skeletal-break-demo-bone Skeleton_arm_joint_L__4_` | {7}, 단일 | 491/167 | 허리90→A; keep/drop=1/1; cap A/B 각14, 총54 drop | Applied1, −1 추론; patch 값1 추론 | 기대 수치 PASS; 몸통 접힘 관찰 |
| UpperArm-Multi | 위 옵션 + `-skeletal-multi-cut 1` | {7}, P=1 단일 게시 | 491/167 | 위와 동일 | 위와 동일 | 수치·PNG 동등 |
| SingleP(요청의 기존 c) | 없음 | {8}, 단일; 팔8→9 | 161/170 | keep/drop=1/0; cap A/B 각14 | Applied1, −1 추론; patch 값1 추론 | 수치 PASS; 허벅지 절단이 아님 |
| SingleP-Multi | `-skeletal-multi-cut 1` | {8}, P=1 단일 게시 | 161/170 | 위와 동일 | 위와 동일 | 수치·PNG 동등 |
| Thigh | `-skeletal-break-demo-bone leg_joint_R_1 -skeletal-union-checks 1 -skeletal-multi-cut 1` | {13,17}, 다중 batch | 300/187/175 | keep/drop=2/0; cap A/B1/B2=32/18/14 | Applied2, 실측18→16, 세대+2, maskCalls1, patchIds1개 | 기대 수치·같은 side B1/B2 cap 분리 PASS |
| Torso | `-skeletal-break-demo-bone Skeleton_torso_joint_2` | {3}, 단일; 척추3→4 | 182/337 | keep/drop=1/4; cap A/B 각22, cap drop0; loop fallback34 | Applied1, −1 추론; patch 값1 추론 | 실제 몸통 절단; 양팔 틈·uncapped17 |
| Torso-Multi | 위 옵션 + `-skeletal-multi-cut 1` | {3}, P=1 단일 게시 | 182/337 | 위와 동일 | 위와 동일 | 수치·PNG 동등, 같은 결함 |
| Thigh-Single(추가 대조) | `-skeletal-break-demo-bone leg_joint_R_1` | {17}, 단일 | 243/169 | keep/drop=1/0; cap A/B 각14 | Applied1, −1 추론; patch 값1 추론 | 실제 오른 허벅지 단일 절단 |

- 기본 c 평면: `BuildSkeletalBreakPlane`은 demo2에서 node8/child9 중점·본 방향 법선을 사용한다. 로그 `target=CesiumMan NodeId=8 ... child=9`, 대표 본 `Skeleton_arm_joint_L__3_`/`Skeleton_arm_joint_L__2_`로 팔 절단임을 확인했다. `demo1`은 ModelViewer의 대상 선택이 Xbot이고 기존 1475/869도 Xbot 허벅지 결과이므로 CesiumMan 몸통 근거가 아니다.
- 몸통 평면: `BuildSkeletalBonePlane`의 `Skeleton_torso_joint_2`(3)→`torso_joint_3`(4) 중점, 월드 교점=(12.423952,0.417974,−0.007242). 두 실행 모두 `MI3d publish` 단일 경로이며 `MI4h2 published` 다중 게시가 아니다. `Skeleton_torso_joint_1` 후보는 `SP7b rejected: incoming=2 (RigRoot)`로 거절됐다(근거 `%TEMP%/MI5-TorsoRoot-{options,patch,debug}.log`, 게시/새 캡처 없음).
- 다중 Thigh는 `CheckJointUnion`이 planeY=0.470281의 P13/P17 평면으로 교체한다. 몸통250 tris→A, 왼다리159→B1, 오른다리153→B2, attach=2/13/17, Frozen=8/15/15, skin 모두 PASS. 합계662=입력662, cap drop0이다.
- 아일랜드 진단: UpperArm islands3/초기 reassigned1, SingleP 2/0, Thigh 3/1, Torso 4/2, Thigh-Single 2/0. 모든 성공 실행에서 실제 loop 재라벨 islands/tris=0/0·conflicts0이며 초기 귀속과 실제 loop 요구가 같았다. Torso의 side1 팔 아일랜드5/13 tris는 B에 남는다.
- fallback 구분: Torso/Torso-Multi의 loop 판정만34, 나머지 사례 loop0. 모든 사례 cap 분류 fallback/invalid0·삼각형 invalid/idMismatch0·게시 fallback0. 다중은 세 몸체 skin/압축 성공과 N/A 경계 로그로 확인했다. Torso cap은 실제 생성된 22개/side만 유지되어 drop loop4가 cap 삼각형 제거로 이어지지는 않았다.
- 단일 constraint 전후 개수와 mask ID 값의 직접 로그는 없다. Applied1 및 `CommitConstraintBreak`의 handle 하나 제거에서 감소1을 추론한다. 다중 `patchIds=1`은 고유 ID 개수이며 값이 아니다. 새 모델 첫 패치와 `SkeletalPatchCopy::Update`의 max(mask low16)+1 규칙에서 값1을 추론하며 실측으로 주장하지 않는다.

직접 연 PNG의 관찰:
- UpperArm 000: 팔의 주황 cap과 절단 틈이 보이고 허리 가짜 cap은 없다. 복부·옆구리에 큰 사선 삼각형 음영/접힘이 이미 보인다. 001은 팔 조각이 시야에서 사라지고 복부 접힘 모양이 변한다. 002는 000과, 003은 001과 비슷하게 반복된다. 링 밖 복부·허리까지 보여 링 주변만의 현상으로 판정할 수 없다. 가슴 전체가 균일하게 찌그러진다는 증거는 없다.
- UpperArm-Multi 000/001도 같은 모양이다. 4장 시간 비교에서 몸통에 남는 주황 cap이나 뚜렷한 새 구멍은 없고, 분리 팔의 화면 밖 이후 궤적은 판정하지 못한다. 허리 옆 작은 보라 돌출은 보이지만 독립 조각인지는 불명이다.
- SingleP/SingleP-Multi 000: 팔 cap 외에 골반 오른쪽 공중에 작은 보라 조각이 보인다. 001에서는 보이지 않으며 남은 팔 단면이 보인다. 기원은 불명이고 완전 무결 PASS로 처리하지 않는다.
- Thigh 000/001: 양 허벅지 틈과 화면 오른쪽 주황 cap이 보이며, +60에서는 팔이 일부 가린다. 뚜렷한 몸통 동반 이탈이나 별도 떠 있는 파편은 관찰되지 않는다. Thigh-Single은 한쪽 허벅지만 절단되고 반대 다리는 이어져 있다.
- Torso/Torso-Multi 000: 가슴 아래 가로 절단선과 양팔 중간의 찢어진 틈, 화면 오른쪽 팔의 배경이 통하는 삼각형 구멍이 보인다. 001: 상체/머리는 오른쪽 아래로 쓰러지고 하체 쪽 몸통 cap은 주황색으로 닫힌다. 잘린 상체의 다른 cap은 가려져 육안 확정 불가다. 로그 `caps=1 uncapped=17 error=0`과 함께 시각 결함으로 기록한다.
- 원인 추정만 기록: `%TEMP%/TorsoWobble-memo.md`의 A 혼합 weight33개/팔 최대59%·Frozen 가설은 국소 링 변형 후보다. 이번 복부 범위를 전부 설명하지 못하며 원본 잔여 메시의 라이브 팔레트·애니메이션/물리 구동도 대조해야 한다. 절단 전 같은 카메라/포즈 대조가 없어 절단이 접힘을 새로 만들었는지는 확정하지 않는다.

회귀·경계·진단:
- 지정 `bash C:/Users/Pes/AppData/Local/Temp/claude/80629d7db37982f2/f78e5679-53b8-4682-ab5d-fe31132b4413/scratchpad/regress.sh Release`에 `BASH_ENV=$TEMP/MI3e2-fix-regress-env.sh` 적용. 6실행 모두 exit0, 수집58.87초, SP6 비교 누적1162 PASS/0 FAIL, BC-12a/b·AR4·Rig PASS. 근거 `%TEMP%/MI5-Release-regress{,-debug,-run}.log`.
- 전체 PS4 보존을 위해 `-debug 1 -skeletal-patch-break-checks 1`도 별도 실행했다(exit0). `%TEMP%/MI5-Boundary-patch.log`: SP7e2 case2/3 publish/after60 모두0mm(pass4/fail0), case1 기존 N/A2건; PS4 전 case A/B publish/after60 12건 간격 변화0mm PASS, unmatched0. PS2/PS3 최대0.000234/0.000189mm.
- 경계 수치 적용 범위: case3은 기본 팔 P8(SingleP)이며 위팔 P7과 다르다. 따라서 UpperArm·Torso·Thigh-Single의 해당 데모 평면 수치는 N/A(기존 옵션에 계측 없음), Thigh 다중은 명시적 `boundary/PS4 measure N/A (multi)`다. 측정 가능한 기존 회귀값을 다른 평면의 PASS로 바꾸지 않는다.
- 이번 사례·별도 경계·회귀의 수집 로그에서 D3D12 ERROR/CORRUPTION=0/0. 기존 DXGI GetContainingOutput ERROR #80 및 D3D12 경고는 남는다. GPU API 오류0이 시각 결함 없음을 의미하지 않는다.

남은 일(수정 없이 후속 판단용):
- 다중 게시 경계 측정: PairBoundary·MeasureBoundaryAt·PS4를 n+1 몸체로 확장하고 위팔 P7·몸통 P3·실제 허벅지 P17 평면도 정량화한다.
- Applied 이후 복구 계약: rig·mask·component의 예외/원본 revision 변경 복구를 단일 경로와 함께 설계한다.
- 기본값 on 결정: 현재 `skeletal-multi-cut` 기본 off 유지. 시각 결함과 측정 공백을 포함해 별도 판단한다.
- 몸통 우글거림: 위 UpperArm 재현 명령과 `%TEMP%/MI5-UpperArm-{patch,debug}.log`, `MI5-UpperArm-demo-{000,001,002,003}-post.png`로 복부·옆구리까지의 반복 접힘을 추적한다. 혼합 weight/Frozen과 원본 팔레트·물리 원인은 가설이며 이번에 수정하지 않았다.
- 몸통 절단 양팔 틈: Torso 재현 명령(옵션 on도 동일), `%TEMP%/MI5-Torso{,-Multi}-{patch,debug}.log`, 동명000/001 PNG. loop fallback34·uncapped17와 팔 구멍/찢어짐을 조사한다.
- 기본 c의 떠 있는 작은 조각: SingleP 재현 명령(옵션 on도 동일), `%TEMP%/MI5-SingleP{,-Multi}-{patch,debug}.log`, 동명000 PNG의 골반 오른쪽 보라 조각을 조사한다.

근거는 위 공통 경로의 사례별 `MI5-<사례>-{options,patch,debug,run}.log`, PNG와 동명 RAW, `%TEMP%/MI5-report.md`다. 시험·기록 완료와 렌더링 결함 해결 완료를 구분한다.

MI-3e2 리뷰 3건 수정(미커밋, base `94f17ca0`, 2026-11-16):
- `SmallestContainingLatticeLoop`로 중첩 시 최소 면적 유효 loop 하나만 고른다. `Submit`은 선택 자식을 보존하고 `DiagnoseIslands`는 그 선분을 우선한다. `IntersectLatticeSegment`는 끝점 허용오차와 선분 진행 방향으로 B side를 정한다.
- CPU PASS; 외부 출력 경로 전체 Debug/Release 빌드 286.49/305.26초, 최종 재빌드 28.81/21.79초, 모두 오류 0.
- Release 3데모 종료 0: 위팔 relabel=0/0, real=0, keep/drop=1/1, body=518/194; Xbot 허벅지 relabel=1/1372, real=0, keep/drop=1/0, body=1475/869; 기본 CesiumMan relabel=0/0, real=0, keep/drop=1/0, body=161/170. conflicts·fallback 모두 0.
- 지정 회귀 6실행 모두 종료 0, 53.83초. BC-12a/b·AR4·Rig PASS; SP7e2 case=2/3 publish/after60 0mm PASS, case=1 기존 N/A. 3데모·회귀 D3D12 ERROR/CORRUPTION 0.
- 근거와 실행 경로: `%TEMP%/MI3e2-fix.md`. 빌드 출력은 `%TEMP%/MI3e2-fix-build/`, `ModelViewer.cpp` 해시는 작업 전후 동일하다.

MI-3f(미커밋, base `63c2292b`, 2026-11-16):
- 리뷰 P2/P3 수정: `FinishCutResult`는 SideDefault 동일 여부와 무관하게 검증된 두 비어 있지 않은 압축 몸체를 게시하고, 압축 결과 없음·몸체 수 오류·빈 몸체만 사유를 기록해 side로 대체한다. 지정 C++/HLSL 조건문은 조건·실행문·중괄호를 별도 줄로 분리했다.
- `DiagnoseCapLoops`는 기존 투표·충돌 진단으로 leader별 uint2(side0/1 몸체) 표를 반환한다. 실제 분리 loop이며 양쪽 몸체가 다르면 keep, 그 외는 양쪽 drop이다. `OnCutResult`의 Relabel→Diagnose→Classify→Compact 순서는 유지한다.
- `ClassifyCutBodies`가 표를 업로드하고 `PatchBodyClassifyCS::main`이 cap Origin.y leader로 조회한다. UINT_MAX는 미해결 SideDefault 대체, UINT_MAX-1은 의도적 drop이며 세 코너 leader 불일치는 오류로 보존한다. cap fallback/invalid/drop 수를 별도 기록한다.
- `PatchBodyCompactCS::main`은 drop을 count·scatter·bounds 전에 제외한다. `BodyCompactJob`이 실제 제거 수를 보존하고 `PollBodyCompaction`은 출력+drop=입력으로 검증한다. 용량은 입력 상한을 유지한다. `CheckBodyCompaction`의 양쪽 cap 2개 제거→출력 4개 GPU 합성 검사 PASS.
- CPU LatticeTopologyTests PASS(1.82초), SkeletalPatchTests Debug/Release 각 69 checks PASS(2.00/3.71초). 지정 MSBuild /restore /m /p:Platform=Windows Debug/Release 최종 41.62/45.10초, 오류 0·기존 경고 각 9. 출력은 `%TEMP%/MI3e2-fix-build/`와 기존 외부 props를 사용했다.
- Release, ModelViewer 작업 디렉터리, MX450(기존 드라이버 기록 591.86): (a) 위팔 10.74초, loop keep/drop=1/1, cap 각 side 41 중 27 drop·14 keep, 입력712−54=출력658, body0/1=491/167. (b) Xbot 허벅지 11.28초, keep/drop=1/0, cap 각53 유지, body=1475/869. (c) 기본 CesiumMan 팔 10.85초, keep/drop=1/0, cap 각14 유지, body=161/170. 모두 종료0, conflicts·분류fallback/invalid·게시fallback=0, 압축 bounds PASS.
- (a)에 `-capture %TEMP%/MI3f-UpperArm -capture-count 2 -capture-step 60` 추가: 게시 f64/+0과 f124/+60 post RAW를 지정 raw2png.ps1로 원본 해상도 PNG 변환했다. 두 프레임에서 허리 주황 cap이 없고 게시 프레임 팔 절단면에는 cap이 보인다. +60은 팔 조각이 화면 밖으로 떨어지고 몸통의 절단 끝은 유지된다. 팔 cap 보존 수는 양쪽 각14다.
- 지정 `regress.sh Release`+기존 외부 exe 래퍼: 6실행 모두 종료0(11/12/10/11/10/10초), 수집64.57초. SP6 누적454 PASS/0 FAIL, Static 재절단/AR4·BC12a/b·Rig PASS. SP7e2 case2/3 publish/after60 모두0mm, case1 기존 N/A(noCoincidentKeptBoundary). PS2 7건 최대0.000234mm, PS3 6건 최대0.000189mm, PS4 12건 간격변화0mm PASS.
- 세 데모·회귀 D3D12 ERROR/CORRUPTION 각0. 기존 DXGI #80·D3D12 경고는 남는다. Geometry/·신규 소스/셰이더·ModelViewer.cpp 편집 없음. 커밋·stash/reset·브랜치 변경 없음.
- 근거: `%TEMP%/MI3f-{Debug,Release}-build.log`, `MI3f-{lattice,skeletal-Debug,skeletal-Release}.log`, `MI3f-{UpperArm,Thigh,DefaultArm}-{patch,debug,run}.log`, `MI3f-Release-regress{,-debug}.log`, `MI3f-UpperArm-demo-{000,001}-post.png`, `MI3f-report.md`.
