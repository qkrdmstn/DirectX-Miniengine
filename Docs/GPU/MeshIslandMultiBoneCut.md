상태: 진행 중 — MI-1~MI-3 완료(MI-3e2 `63c2292b`, MI-3f `ba2b976f`); MI-4a 완료(`7728485a`); MI-4b 완료(`86522153`); MI-4c 완료(`caa6f681`); MI-4d 진행 중(MI-4d1 미커밋, base `caa6f681`, 2026-11-16): 요청 몸체 수 기반 압축·카운터·준비 ID 배열. 실제 다중 loop 귀속·n=2 절단 진단은 MI-4d2.

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
| MI-4d | 진행 중: MI-4d1 완료(미커밋, base `caa6f681`), MI-4d2 대기 | 전체 완료 조건은 같은 side B1/B2 cap 분리, 빈 몸체 로그; 실제 다중 loop 귀속은 아직 미완료 |
| MI-4d1 | 요청 그룹 수 기반 body×section 압축·몸체별 분류 카운터·준비 ID 배열 | 빈 마지막 몸체 보존·로그, 합성 세 몸체/빈 네 번째 몸체 GPU 압축 검증 |
| MI-4d2 | P_i 교차점 snapshot·아일랜드 재라벨·loop leader→side별 몸체·몸체별 skin 준비 | P13/P17 실제 합집합 절단·분류·압축 진단, B1/B2 cap 분리·빈 몸체 로그; break·mask·n+1 게시 미연결 |
| MI-4e | `ApplyConstraintBreak` 검증/commit 분리 | 단일 break 회귀·거절 무변경 |
| MI-4f | `SkeletalPhysicsSystem` batch 사전 검증·세대 계약 | 두 번째 실패 시 첫 constraint도 보존(CPU 검사) |
| MI-4g | `QueueBreak` batch·결과 수집 | pause·Stale·AlreadyBroken·Reset 전체 무변경 |
| MI-4h | batch 성공 후 mask 1회 + n+1 게시 | 허벅지/몸통 캡처, constraint n개 감소, 회귀 |

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
| MI-5 | CesiumMan 시험·기록 | 위팔·허벅지·몸통 평면 캡처, 결과를 아래 "시험 결과"에 기록 |

- 단계마다 Debug·Release 빌드, 회귀(`-skeletal-patch-break-checks 1`, `-skeletal-patch-checks 1`, `-attached-recut-checks 1`, 기존 Static 절단), 로컬 커밋.
- 범위 밖: 이미 절단된 조각(generated)을 다음 절단 입력에 합치는 병합, 재절단 결과의 스키닝 승계, GPU union-find.

## 시험 결과

MI-4d1(미커밋, base `caa6f681`, 2026-11-16):
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

(MI-5에서 추가 기록)

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
