상태: 진행 중 — MI-3b까지 완료(`34d5199e`); MI-3c 실제 출력 압축·비동기 회수·실패 격리 완료(미커밋, base `34d5199e`), D/R 빌드·Release 위팔·회귀 PASS; 게시 연결은 MI-3d 후속 (2026-11-16).

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
| 아일랜드→몸체 | weight 다수결 | 평면 반대편에 있어도 몸통 weight가 큰 허리는 A에 남는다 |
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
| MI-3c | 완료(미커밋, base `34d5199e`): 실제 두 side 비동기 압축·실패 격리·완료 후 회수·몸체별 Mesh/삼각형 수 보관(게시 미연결) | 위팔 body0=518/body1=194, 합계 712=입력 712 및 material/depth/IB/bounds PASS; D/R 빌드·Release 회귀 PASS |
| MI-3d | 단일 관절 재배치 게시 연결(`OnCutResult`·`BuildPieceSkin(body)`), cap은 SideDefault | 위팔 허리 90 tris가 A에 남음, A/B 각 1회 게시, 캡처 |
| MI-3e | `ClipTrianglesCS` segment 출처 → cap loop 몸체 보조 전달 | 다중 loop cap 귀속·충돌/fallback 진단 |
| MI-3f | cap loop 귀속 연결 | 위팔 publish/after60 캡처, SP7e2 ≤1mm, PS2/3·PS4 ≤0.01mm, Static/PS/AR 회귀 |
| MI-4a | 무한 평면 교차 세그먼트 집합·조상 제거 함수 | 형제 2개/조상+자손/부호 반전 fixture 로그 |
| MI-4b | joint 집합 팔레트 합집합 선택(`PatchExtractCS::Selected` 재사용) | 중복 삼각형 0, GPU==CPU |
| MI-4c | 그룹 일반화·몸체별 snapshot/attachment 배열 | n=1 동등, 형제 n=2 Frozen 팔레트 검증 |
| MI-4d | 압축·게시 준비물 n+1 배열화(break 미연결) | 같은 side B1/B2 cap 분리, 빈 몸체 로그 |
| MI-4e | `ApplyConstraintBreak` 검증/commit 분리 | 단일 break 회귀·거절 무변경 |
| MI-4f | `SkeletalPhysicsSystem` batch 사전 검증·세대 계약 | 두 번째 실패 시 첫 constraint도 보존(CPU 검사) |
| MI-4g | `QueueBreak` batch·결과 수집 | pause·Stale·AlreadyBroken·Reset 전체 무변경 |
| MI-4h | batch 성공 후 mask 1회 + n+1 게시 | 허벅지/몸통 캡처, constraint n개 감소, 회귀 |

### MI-3·MI-4 추가 결정 (Claude 판단, 2026-11-13; 근거 Codex 계획 메모 `%TEMP%/MI3-plan.md`)

- 재배치는 절단 후 body×section 압축(새 Scene compute pass)으로 한다. 절단 전 패치에서 빼는 방식은 교차 삼각형·B_i↔B_j 분배·cap을 풀지 못해 쓰지 않는다.
- 출력 삼각형 몸체 = provenance `TriangleId`(패치 삼각형)와 출력 side로 `TriangleSideBody` 조회. cap은 loop 단위 몸체(MI-3e까지는 SideDefault).
- 원본 mask는 합집합 패치 전체를 한 번 숨기고 patch id 하나를 유지한다.
- 다중 본 A의 강체 부착 node = 모든 `P_i` 부모의 최소 공통 조상(n=1이면 현재와 같다).
- 다중 break는 전부 성공 아니면 전부 거절(사전 검증 후 순차 commit). 부분 성공은 허용하지 않는다.
| MI-5 | CesiumMan 시험·기록 | 위팔·허벅지·몸통 평면 캡처, 결과를 아래 "시험 결과"에 기록 |

- 단계마다 Debug·Release 빌드, 회귀(`-skeletal-patch-break-checks 1`, `-skeletal-patch-checks 1`, `-attached-recut-checks 1`, 기존 Static 절단), 로컬 커밋.
- 범위 밖: 이미 절단된 조각(generated)을 다음 절단 입력에 합치는 병합, 재절단 결과의 스키닝 승계, GPU union-find.

## 시험 결과

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

(MI-5에서 추가 기록)
