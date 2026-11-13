상태: 진행 중 — MI-1·MI-2 아일랜드 라벨 진단 및 MI-2 리뷰 3건 수정 완료, CPU 테스트·Debug/Release 빌드·Release 회귀 PASS(base `432ff0c5`); MI-3~5 미구현 (2026-11-13).

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
| MI-3 | GPU 분리: 절단 출력 삼각형을 몸체 표로 A/B에 재배치(cap 포함), 단일 관절 유지 | 위팔 캡처에서 허리 삼각형이 원본에 남음; SP7e2·PS 회귀 PASS |
| MI-4 | 다중 본: 교차 최상위 joint 집합, 합집합 선택, 몸체 n+1 게시, `BreakConstraint` n회 | 몸통/허벅지 평면에서 끊은 관절 수만큼 B 몸체; AlreadyBroken·Stale 거절 유지; 회귀 PASS |
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

(MI-5에서 추가 기록)
