상태: 진행 중 — SP-1~SP-5a 커밋, SP-5b 독립 리뷰·수정·검증 완료(미커밋, base `70a754d7`): Xbot·CesiumMan 각 3/3 A/B, GPU==CPU, B 0.1 m, 물리 ON 캡처·Static 회귀 PASS; SP-6·SP-7 미구현 (2026-11-03)

# Skeletal 패치 복사 절단 (첫 단계)

## 1. 목표

현재 평면을 둔 상태에서 C를 누르면 skin 메시에 대해 다음을 수행한다.

1. 평면이 지나는 본(joint)을 고른다.
2. 그 본의 영향 삼각형을 현재 pose 그대로 GPU에서 복사해 원본 옆에 정적 메시로 띄운다.
3. 복사본을 같은 평면(같은 오프셋만큼 이동)으로 기존 Static GPU 절단기로 자른다.

NDC25 방식(영향 영역 복사→복사본 절단→원본 숨김)의 앞 두 단계를 눈으로 확인하는 진단 마일스톤이다. 원본 숨김·본 추종·BreakConstraint·물리는 하지 않는다.

## 2. 결정과 이유

| # | 결정 | 이유 |
| --- | --- | --- |
| D1 | 검증 자산은 CesiumMan. `ModelViewer::Startup`에서 기존 Cube·Bunny·Dragon·Xbot의 배치를 지금 값으로 고정하고 CesiumMan만 별도 위치에 둔다. 기존 진단·벤치마크 실행에서는 CesiumMan을 로드하지 않는다. | 연속 피부라 혼합 weight 경계가 있다. 전체 객체 수 기반 배치가 바뀌면 기존 화면·측정이 달라진다. |
| D2 | C 입력의 Static 경로(`CuttingSystem::RequestAll`)는 그대로 둔다. `PhysicsSystem::IsEligible`, `CuttingSystem::Submit`, `GpuMeshCutter`의 skin 거절도 유지한다. 원본 skin 메시는 Static 절단기에 넣지 않고, 정적 복사본만 넣는다. | 기존 검증 경로를 건드리지 않는다. C는 원래 절단 키라 새 키를 만들지 않는다. |
| D3 | 대상: `QueryCutCandidates`는 skin을 빼므로 쓰지 않는다. Scene의 skin MeshComponent를 직접 순회해 world bounds가 유한 평면 사각형과 겹치는 것을 고르고, 여럿이면 bounds 중심이 평면 중심에 가장 가까운 하나만 쓴다. | C 한 번에 복사본 하나. 규칙이 결정적이다. |
| D4 | 애니메이션은 멈추지 않는다. `ModelViewer::Update`에서 그 프레임의 `PhysicsSystem::UpdateFrame`·`Finish` 뒤에 Job을 제출해 그 프레임의 `PosedGeometry` RAW40을 입력으로 쓴다. Compute Pose가 꺼져 있으면(`-compute-pose 0`) 거절하고 로그만 남긴다. | 복사본은 원본과 독립된 정적 메시라 원본이 계속 움직여도 된다. RAW40이 없으면 입력이 없다. |
| D5 | 정점 데이터를 다루는 복사는 전부 GPU Compute에서 한다. 본 선택은 CPU에 이미 있는 본 행렬로 한다(D6). 입력은 원본 `Model::m_DataBuffer`(IB R16/R32·draw `startIndex/baseVertex`·VB 끝 16바이트 BLENDINDICES uint16×4·BLENDWEIGHT unorm16×4·UV half2)와 `PosedGeometry::GetOffset(SourceMesh)`의 RAW40, section별 palette→NodeId 표(작은 업로드). weight는 `PreparePoseCS::main`처럼 합으로 정규화한다. CPU로는 개수 몇 바이트만 fence 뒤 비동기로 읽는다. | 프로젝트 방향이 GPU 절단이다. 데이터가 만들어지는 곳에서 계산한다: 정점은 GPU, 본 행렬은 CPU(`ModelInstance::FinalizePose`). 결과 버퍼를 그대로 절단기 입력으로 넘긴다. decode 규칙을 기존 pose 셰이더와 맞춘다. |
| D6 | 본 선택(CPU, segment 부모). segment는 저장된 데이터가 아니라 C를 누를 때 만든다: 부모 관계는 `ModelInstance::m_PoseParents`(생성 시 `BuildPoseParents`), 위치는 그 프레임 `GetNodeTransforms()`(model space)에 원본 world를 곱한 joint 원점. skin joint(`m_JointIndices`에 있는 NodeId)마다 가장 가까운 skin joint 조상을 부모로 삼아 segment(부모 → joint)를 만든다. 양 끝 부호 거리가 다르고 교차점이 유한 평면 사각형 안이면 후보다. 후보가 여럿이면 교차점이 평면 중심에 가장 가까운 하나만 쓰고 여러 segment 처리는 하지 않는다. 선택 본 = 그 segment의 부모 joint. 후보가 없으면 말단 처리: 자식 skin joint가 없는 joint 중 world 위치가 평면 중심에 가장 가까운 joint를 고르고 같은 패치 방식으로 복사·절단한다. 선택 NodeId의 palette 번호 마스크(`m_JointIndices[mesh.startJoint + j] == NodeId`)를 CPU에서 만들어 추출 패스에 넘긴다. 로그: 선택 본, segment(또는 말단), 교차점. 모델 로드 시 원본 정점을 한 번 순회해 양수 정규화 weight가 있는 influencing joint를 캐시하며, segment의 양 끝·가장 가까운 skin 조상과 influencing 후손이 없는 말단 후보·진단 말단 선택은 이 집합만 사용한다. | **[슬라이드]** 「BreakConstraint()를 사용할 본 선택」·"절단할 위치에서 Bone을 선택" — 고르는 본이 끊을 본이다. **[추론]** Unreal의 physics body는 bone→자식 segment를 감싸므로 평면이 지나는 body = segment 부모 joint. 평면이 B→C 위에 있을 때 C를 고르면 패치(C)가 단면을 덮지 못해 엉뚱한 관절에서 떨어진다. joint 수만큼의 계산이라 GPU 패스보다 싸고 결과가 즉시 나온다. |
| D7 | 패치 추출(GPU). 선택 NodeId에 양수 weight가 있는 꼭짓점이 하나라도 있는 삼각형을 고른다. 선택 플래그 누적합 순서로 압축해 정적 생성 형식에 쓴다: stride 28 packed VB(`WriteVertex`와 같은 순서·flags), RAW40 Pose, depth VB, R32 IB(`baseVertex=0`, draw 1개). 출처 `(section, draw, triangle)`도 쓰고 local bounds를 계산한다. 출력 용량은 대상 삼각형 수 합(상한)으로 미리 잡는다. 자손 본은 포함하지 않는다. 같은 패스에서 평면 교차 삼각형 수와 그중 선택 본 weight가 없는 "미포함 교차" 수를 센다(교차 = 꼭짓점 부호 거리 `|d|<=eps`는 0, `min d < -eps`이고 `max d > eps`, 교차 선분이 유한 사각형과 겹침 — `ClipCoplanarSegmentToCutPlaneRectangle`과 같은 판정). 미포함 교차가 있어도 복사·절단은 그대로 수행하고 로그만 남긴다. | 선택 joint만, 자손 제외라는 기존 결정과 같다. `Mesh::Draw`가 여럿일 수 있어 draw까지 출처에 넣는다. 상한 확보로 overflow가 없다. 미포함 교차(weight 경계와 joint 위치 어긋남·사선 절단)가 얼마나 생기는지 측정해, 원본 숨김 단계에서 NDC식 각도 보정·영역 병합 중 무엇이 필요한지 정한다. |
| D7a | 추출 규칙의 기준 구현은 CPU 순수 함수(`ExtractPatchReference`)로 테스트 코드에만 둔다. segment 선택은 런타임 CPU 함수 자체를 테스트한다. 진단 옵션에서 GPU 결과(교차 수·미포함 교차 수·패치 수·출처 목록)를 같은 입력의 CPU 기준과 비교한다. 기본 실행 경로에는 CPU 계산이 없다. | GPU 결과의 정답을 사람이 읽을 수 있는 코드로 고정한다. |
| D8 | 복사본 게시. `Scene::CreateObject(name, sourceModel)`로 owner를 만든다. 자동 생성된 컴포넌트에 D7 버퍼를 `GeometryRevision::FromGenerated`로 감싸 `MeshComponent::SetGeometry`로 넣는다(skin 없음, `meshCBV=0`, 원본 material, static PSO). world = 원본 world + X 오프셋(원본 bounds 폭 + 0.5 m). 삼각형 수는 비동기 readback으로 받은 뒤 게시한다(`CutJob::Poll` 방식). | 원본과 겹치지 않게 비교한다. 기존 생성 geometry 렌더 경로(`UpdateBaked`)를 재사용한다. 렌더(`Mesh::Draw`)와 절단기 dispatch가 CPU의 index 수를 쓰므로 개수만 읽는다(기존 Static `CutJob::Poll`과 같다). readback을 없애는 indirect draw·dispatch는 후속이다. 별도 GameObject는 오프셋 진단용 예외다. 원본 숨김 단계에서는 원칙(13.1절)대로 패치 A/B를 원본 GameObject의 MeshComponent로 옮긴다. |
| D9 | 절단 요청. 게시 후 `MeshComponent::Update`/`PosedGeometry::UpdateBaked`로 bounds·pose를 명시적으로 준비한다. Static batch가 끝날 때까지 요청을 pending으로 둔다(`Request` busy 거절 회피). 그다음 오프셋 평면으로 `CuttingSystem::Request(copyObject, copyComponent, plane)`를 부른다. 오프셋 평면은 평면 변환을 Δ만큼 옮겨 `TryBuildCutPlaneData`로 다시 만든다(`WorldPlane`·`WorldToPlane`·`WorldCorners`가 함께 맞도록). | 새 절단 코드를 만들지 않고 검증된 절단기를 쓴다. 결과 A/B는 기존 규칙(같은 owner, A 교체 + B 추가)을 따른다. |
| D10 | (SP-5b에서 변경) 복사본 절단은 무한 평면으로 한다. 유한 사각형은 대상 선택(D3)과 본 선택(D6)에만 쓴다. 복사본 절단용 오프셋 평면은 같은 중심·법선·회전에 사각형 크기만 복사본 world bounds를 덮도록 키워 `TryBuildCutPlaneData`로 만든다. 절단기 규칙은 바꾸지 않는다. 패치는 열린 경계라 cap 실패는 기존 정책대로 surface만 게시한다. 그래도 거절되면 복사본을 그대로 두고 이유를 로그로 남긴다. `LastCaps/LastUncapped/LastError`를 기록하고 `ExpectedMinimumLoops`는 쓰지 않는다. | SP-5a에서 위팔·허벅지가 "교차선 끝점이 사각형 밖"으로 거절됐다. 원인은 선택 본 weight가 조금 있는 몸통·반대쪽 다리 삼각형이 사각형 밖에서 교차한 것이다(아래 SP-5a 근거). Unreal `SliceProceduralMesh`도 무한 평면으로 자른다. 사용자 요구는 "평면을 옮겨 C를 누르면 잘린 결과가 옆에 보인다"이다. |
| D11 | 수명과 격리. 새 C는 진행 중인 SkeletalPatchJob과 pending 요청을 무효화하고, `RequestAll` 전에 이전 복사본 owner를 `DeleteEditTarget`의 owner 삭제 경로로 지운다(`CancelTarget`·`PhysicsSystem::RemoveTarget` 포함). 대상이 없어도 이전 복사본은 지운다. 복사본 owner는 Static publication 수·벤치마크·자동 재절단 집계에서 뺀다. | 복사본은 정적이라 다음 C의 Static 후보가 될 수 있다. 진단 화면이 쌓이지 않게 하고, 기존 측정을 바꾸지 않는다. |
| D12 | (SP-6) 본 선택 모드 두 가지. Parent는 D6 그대로다. Influence는 유한 사각형 교차 삼각형(D7과 같은 판정)의 꼭짓점 정규화 weight를 NodeId별로 합해 최대 joint를 고른다(동점은 작은 NodeId). Influence는 GPU 합산·선택 패스가 고른 결과를 GPU 버퍼로 추출 패스에 바로 넘기고, CPU는 로그용 NodeId 4바이트만 기존 readback에 붙여 읽는다. 교차 삼각형이 없으면 기존처럼 거절 로그를 남긴다. 실행 옵션 `-skeletal-joint-mode parent\|influence`(기본 parent)와 빈 키 하나로 전환하고 전환 시 로그를 남긴다. `-skeletal-patch-checks`는 부위마다 두 모드를 차례로 돌려 joint·패치·교차·미포함·절단 결과를 나란히 로그로 남긴다. Influence 선택은 CPU 기준 함수(테스트 전용)와 비교한다. | 사용자가 두 기준의 결과를 화면과 수치로 비교한 뒤 BreakConstraint 단계의 기준을 정한다. SP-2a 투표 셰이더(`8af35400`)를 고쳐 재사용한다. |
| D13 | (SP-5b) 절단 결과를 보이게 한다. 복사본 절단 A/B가 게시되면 B 컴포넌트를 평면 법선 방향으로 A에서 멀어지게 0.1 m(world) 옮겨 렌더링한다. | A/B가 붙어 있으면 잘렸는지 보이지 않는다(사용자 결정). |
| D14 | (SP-5b) 스켈레탈 메시 일반화. 선택·추출·게시·절단은 모델 이름·노드 이름·좌표 상수 없이 동작한다. 한 Model 안의 여러 내부 Mesh(예: Xbot의 Surface·Joints)는 각 `mesh.startJoint`로 palette→NodeId를 풀고, 선택 본 영향 삼각형을 모든 Mesh에서 함께 복사한다. `-skeletal-patch-checks`는 CesiumMan과 Xbot을 모두 올리고, 각 모델의 골격 구조에서 결정적으로 고른 segment 2개 이상과 말단 1개로 평면을 만든다(중심 = segment 중점, 법선 = segment 방향, 하드코딩 이름 없음). | CesiumMan 전용 구현이 되지 않게 한다(사용자 요구). |

## 3. 인터페이스 골격 (이름은 구현 시 주변 코드에 맞춘다)

```cpp
// Scene/SkeletalPatchCopy.h
struct SkeletalPatchResult          // fence 후 비동기로 읽는 몇 바이트
{
    uint32_t PatchTriangles;
    uint32_t CrossingTriangles;
    uint32_t UncoveredCrossing;     // 교차하지만 선택 본 weight가 없는 삼각형
};

class SkeletalPatchJob              // CutJob처럼 제출 → Poll
{
public:
    // GPU: 패치 추출 1패스를 기록한다 (선택 본의 palette 마스크는 CPU가 넘긴다)
    bool Submit(const MeshComponent& source, const CutPlaneData& plane, const PaletteMask& selected);
    // fence 완료 시 결과와 패치 GPU 버퍼(GeometryBuffers)를 넘긴다
    bool Poll(SkeletalPatchResult& result, std::shared_ptr<GeometryBuffers>& patch);
};

class SkeletalPatchCopy
{
public:
    void Request(Scene& scene, const CutPlaneData& plane); // C 입력: 이전 복사본 정리, 대상 선택, segment 본 선택, Job 제출
    void Update(Scene& scene, CuttingSystem& cutting);     // 완료 시 복사본 게시 → batch 종료 후 오프셋 평면 절단 요청
};
```

본 선택: `SelectSegmentJoint`(CPU, D6, 런타임 코드이자 CPU 테스트 대상). Shader: `Scene/Shaders/PatchExtractCS.hlsl`(D7). 추출 CPU 기준 함수는 `Scene/tests/`에만 둔다. SP-2a의 `PatchJointVoteCS`·`PatchJointSelectCS`와 `SelectJointReference`는 SP-2b에서 제거한다.

## 4. 단계 (각 약 300줄 이하, 단계마다 새 Codex 실행 + 다른 Codex 리뷰)

| 단계 | 내용 | 완료 증거 | 예상 비용 |
| --- | --- | --- | --- |
| SP-1 | CesiumMan 배치·로드 분리(D1). CPU 기준 함수 + CPU 테스트(합성 입력: R16/R32·draw offset, palette→NodeId, 중복 palette 합산, eps 접촉·공면, 유한 사각형 교차, 양수 weight 포함, 동점, 출력 순서) | CPU 테스트 PASS, Debug/Release 빌드, 기존 네 모델 위치·Static 진단 불변 | 구현 + R 2~5분 + B 약 2분 + T |
| SP-2a | (완료 `8af35400`, 규칙 변경으로 셰이더는 대체) 투표 셰이더 2개 + `SkeletalPatchJob` 제출/Poll 골격 | Debug/Release 빌드 | — |
| SP-2b | 투표 셰이더·`SelectJointReference` 제거, CPU `SelectSegmentJoint`(D6: segment·말단·palette 마스크) + CPU 테스트 + C 입력 연결(D3, D4) + 로그(선택 본·segment/말단·교차점). Job 제출은 SP-3에서. 화면 변화 없음 | CesiumMan 위팔·허벅지·손 절단 시 기대 joint 로그, CPU 테스트 PASS | 구현 + R + B + V |
| SP-3 | 패치 추출 Compute(D7): 누적합 압축, 정적 생성 형식·출처·bounds, 용량 상한, 교차·미포함 교차 카운트. 로그(패치 수·교차 수·미포함 교차 수) | 진단 옵션에서 패치 수·출처 목록·카운트가 CPU 기준과 일치, D3D12 ERROR 0 | 구현 + R + B + V |
| SP-4 | 복사본 owner 게시(D8) + 이전 복사본 정리·격리(D11) | 완료 `9bf4ec77`: 캡처·위팔 재요청 포함 복사본 1개, 원본 posed 월드 X 폭 + 0.5 m, GPU packed VB와 원본 pose의 월드 위치 대조 PASS | 구현 + R + B + V |
| SP-5a | pose 준비·batch 대기·오프셋 평면 절단 요청(D9) + 결과/실패 로그(당시 D10) | 완료 `17cdbdbe`: 위팔·허벅지 유한 사각형 거절, 손 A/B 캡처, 교차 0 게시 생략, Static 회귀·집계 격리, D3D12 ERROR 0 | — |
| SP-5b | 복사본 무한 평면 절단(D10) + B 분리 0.1 m(D13) + 일반화·Xbot 진단(D14) | 독립 리뷰·수정 완료(미커밋): 영향 joint 캐시·비균일 평면 scale 보정, 두 모델 각 3 A/B·GPU==CPU·B 0.1 m, 물리 ON 캡처와 Static 격리/회귀·D/R·CPU PASS, D3D12 ERROR 0. 아래 근거 참조 | 구현 + R + B + V |
| SP-6 | 본 선택 모드 Parent/Influence 전환·비교 진단(D12) | 두 모델·부위별 두 모드 비교 로그와 캡처, Influence GPU==CPU, D3D12 ERROR 0. 끝나면 멈추고 사용자에게 비교 결과를 보고한다 | 구현 + R + B + V |
| SP-7 | (사용자가 SP-6 결과를 본 뒤 설계) 선택 joint에 BreakConstraint 적용 | — | — |

한 단계가 300줄을 넘으면 shader와 C++ 연결을 나눈다.

SP-3 리뷰 근거: `skeletonRoot`에서 부모 공간이 끊긴 본 원점에 `JointFrameToWorld`가 렌더와 같은 `component world × inverse(SourceFrame) × mesh node`를 적용한다. 진단 평면도 같은 좌표로 만들며, 추출 RAW40·packed VB는 component-local을 유지하고 `RequestSkeletalPatch`에서 cutter와 같은 방식으로 평면을 변환한다. 보정 전 무한 평면 교차 198/0/187은 보정 후 47/108/69이며, 위팔의 미포함 교차 5는 거절 조건으로 쓰지 않는다. 비단위 평면의 eps 투영과 현재 패치 bounds도 보정했다. Debug/Release 빌드·CPU 테스트(SkeletalPatchTests 45개)·정적 cube 회귀가 통과했고, 회전·비균일 scale·SourceFrame 추가 진단도 3지역 GPU/CPU PASS다.

SP-4 리뷰 근거: 기존 약 6.874 m 오프셋은 `MeshComponent::GetBoundingSphere`의 느슨한 구 지름 + 0.5 m였다. `SkeletalPatchJob::Submit`의 추출 패스에서 선택과 무관하게 원본 전체 draw의 posed 정점을 component world로 옮겨 X 최소·최대를 집계하도록 수정했다. 일반 경로는 두 스칼라만 추가 readback하며 정점 readback은 진단 전용이다. `SkeletalPatchJob::Validate`는 원본 pose와 실제 GPU packed VB를 게시 TRS로 변환하여 오프셋 제거 후 비교한다. 진단 입력의 X 경계는 [11.696115, 13.003948] m, 폭 1.307833 m, 오프셋 1.807833 m다. 손 영역의 원본/복사본(오프셋 제거) AABB는 모두 min=(11.696115, -0.327247, -1.023413), max=(12.165718, 0.520666, -0.291195) m이고 정점별 최대 오차는 0 m다. 이는 제출 pose의 비교이며 이후 원본 애니메이션을 따라가는 복사본은 아니다. 리뷰 빌드·실행 로그는 `%TEMP%/step7-SP4-review-final-{Debug,Release}.log`, `%TEMP%/step7-Release-SP4-review-patch-final.log`, `%TEMP%/step7-Release-SP4-review-static.log`, CPU 산출물은 `%TEMP%/sp4-review-tests/`, 캡처는 `%TEMP%/SP4-review-copy.png`다. SP-5의 복사본 절단과 A/B 결과 완료 조건은 아직 적용 전이다.

SP-5a 독립 리뷰 근거(2026-11-02, 당시 D10 기준):

- `SkeletalPatchCopy::Update`는 `CrossingTriangles==0`이면 owner 생성과 cut 요청을 모두 생략한다. GPU 진단의 1mm 사각형은 patch=328/crossing=0이며 생략 로그와 owner 부재 검사를 통과했다.
- C 시점 평면을 복사본과 같은 X=1.807833m 옮겨 `TryBuildCutPlaneData`로 재구축했다. 모서리/역변환/평면 식 최대 오차는 각각 0.000001909/0.000002865/0.000001431이다. 원본 pose와 복사 정점의 이동 제거 후 최대 오차는 0m이다.
- 위팔 2회: 패치 무한 평면 교차 16개, 유한 사각형 교차 15개, 외부 끝점을 가진 삼각형 3개(882/884/885). 0.3×0.3m 사각형에 대해 XZ 범위는 [-0.125115,0.129723]×[-0.090842,0.155269]m. +Z 가장자리 초과 1.118–5.269mm이며 torso(3/4)·오른팔(10/11) 혼합 weight의 삼각형이다.
- 허벅지: 무한 20개/유한 12개/외부 10개. XZ 범위 [-0.140879,0.424734]×[-0.465143,0.137251]m(폭 0.565613×0.602394m). 왼다리(13/14) 지배·오른 허벅지(17) 미소 weight 삼각형 3023/3062/3142/3144는 +X 최대 0.274734m, -Z 최대 0.315143m 초과. 골반(2)·오른다리(17/18) 삼각형 3206/3290–3294도 -Z 최대 0.071477m 초과한다. 선택 본의 양수 weight를 보존한 정상 D10 결과이며 평면 크기나 cutter 규칙을 바꾸지 않았다.
- 손: 패치 무한/유한 17개, 외부 0개, XZ 범위 [-0.117207,0.108221]×[-0.057519,0.146248]m. 최종 components/caps/uncapped/error는 위팔 각 1/1/0/1, 허벅지 1/1/4/1, 손 2/1/0/0이다.
- caps는 성공한 GPU loop 수, uncapped는 cap 실패 경계 segment 수다. `CuttingSystem::UpdateOne`이 매번 현재 `job.Status`로 덮어쓰고 `GpuMeshCutter::Submit`이 metadata를 0으로 초기화한다. `BoundaryKeysCS`의 fatal bit와 별개로 `CapEarsCS`·`FinalizeCutCS`가 집계하므로 거절 결과에도 1/4가 남는다. 이전 작업의 stale 값이 아니다.
- 새 C는 `SkeletalPatchCopy::Reset`→`DeleteEditTarget`→`CancelTarget`/`RemoveTarget`→owner 삭제 순서다. active GPU 작업은 자원을 보존한 채 완료 후 대상 재검색으로 게시를 취소하며, retired owner 결과도 Static 소비 전에 버린다. A/B는 같은 NotCuttable owner에 남는다. `IsEligible`·skin 거절·Static cutter는 변경하지 않았다.
- `-cut-expect-publications`의 조기 검사를 수정해 패치 결과 소비까지 기다린다. 손 A/B 이후에도 기대 Static 게시 수 0 검사가 통과하며 `-cut-expect-loops 2`는 패치에 적용되지 않는다.
- Debug/Release 빌드와 SkeletalPatchTests(각 45 checks)·CutPlaneTests 모두 exit 0. Release 진단과 지정 cube 회귀도 exit 0, D3D12 ERROR/CORRUPTION 0. GPU NVIDIA GeForce MX450, 드라이버 32.0.15.9186; 기존 C4819·D3D12 경고는 남는다.
- 로그: `%TEMP%/step7-SP5-review-final-{Debug,Release}.log`, `%TEMP%/SP5-review-tests/{Debug,Release}/`, `%TEMP%/step7-Release-SP5-review-final-{patch,static}.log`. 캡처: `%TEMP%/SP5-review-hand.png`, 같은 접두사의 `-hdr.raw`·`-post.raw`.

SP-5b 독립 리뷰·수정·검증 (2026-11-03, base `70a754d7`, 미커밋):

- `BuildInfluencingJoints`가 Model 로드 시 CPU 원본 VB의 uint16 팔레트와 unorm16 weight를 한 번 순회하여 `Model::m_InfluencingJoints`를 NodeId별로 저장한다. 양수 정규화 weight 여부는 정수 weight의 양수 여부와 같다. `SelectSegmentJoint`와 `BuildSkeletalPatchDiagnosticPlane` 모두 이 캐시로 segment·가장 가까운 영향 skin 조상·영향 후손이 없는 말단을 고른다. C 경로의 정점 CPU 순회/readback이나 모델·본 이름 조건은 추가하지 않았다. 영향 없는 End·중간 본·영향 본 부재 CPU 회귀 3개를 추가했다.
- 무한 평면 재구축은 원래 `WorldToPlane`의 역변환에 복사본과 같은 X 이동만 적용하므로 중심·법선·회전을 보존한다. GPU 패치 경계 구를 월드로 옮기고 `max(0.01 m, radius×5%)` 여유를 더한다. 리뷰에서 발견한 비균일 평면 scale 누락은 역변환의 `AffineRadiusScale`을 반영해 수정했다. `TryBuildCutPlaneData`가 식·역변환·모서리를 함께 만든다.
- A는 양의 반공간이고 B는 음의 반공간이다(`ClipTrianglesCS`). B의 월드 TRS에서 정규화한 `-normal×0.1 m`만 더하고 `SetTransform(WorldIndependent)`·`Update`를 거쳐 렌더·bounds·선택을 갱신한다. 두 모델의 scale 차이와 무관하게 모든 case의 측정 gap은 0.100000 m다.
- 물리 결함은 없었다. 복사본과 A/B는 충돌 hull이 비어 있어 `PhysicsSystem::SyncBodies`의 등록 조건에서 제외된다. 원본 skin은 hull이 없어 `CreateFloor`의 초기 lift 대상에서도 빠지므로 기본 중심 정렬된 CesiumMan 아래쪽을 바닥(top=-1.080540 m)이 가린다. 카메라나 동적 낙하가 원인이 아니다. 진단 모드에서만 두 skin 원본을 보수적인 모델 경계 기준으로 1.019460 m 올렸다. 기본 장면 배치는 유지한다. 매 진단 Update가 복사본의 hull 부재·IsSimulating=false·A/B 위치 유지를 검사하며, 물리를 진행한 60프레임 캡처도 통과했다.
- D3 대상 선택은 Scene skin 컴포넌트의 유한 사각형/경계 구 겹침과 중심 거리로 결정한다. 각 section은 `mesh.startJoint`로 팔레트를 읽고 선택 NodeId의 모든 중복 슬롯을 표시한다. Xbot 두 Mesh의 segment 출처는 각각 CPU/GPU 760/796개로 일치한다. leaf는 0/420개다. 이름은 진단 GLB 이름 복원 로그에만 사용한다.
- Static cutter·IsEligible·skin 거절은 수정하지 않았다. 복사본 결과는 Static publication/벤치마크 소비 전에 격리하고, 다음 C는 공통 삭제 경로로 이전 owner를 정리한다. 물리 ON 진단에 `-cut-expect-publications 0`을 추가해 최종 Static 집계 0도 검증했다.
- AL-1은 `CapEarsCS`의 Cached 내부 검사 집합을 `TestList`로 압축하며 입력·출력 계약을 바꾸지 않는다. merged HEAD의 셰이더를 외부 빌드에서 다시 컴파일했고 아래 6 A/B 및 cube의 cap·면적·부피·hull 검증을 통과했다. 셰이더는 수정하지 않았다.

기본 Release `-skeletal-patch-checks 1 -debug 1` 결과(physics 활성화, 기본 paused pose):

| 모델 / case | 선택 NodeId·이름·종류 | patch/crossing/uncovered | GPU==CPU 개수·출처 | components/caps/uncapped/error | B gap |
| --- | --- | --- | --- | --- | --- |
| Xbot / 0 | 61 mixamorig:LeftLeg segment | 1556/32/0 | PASS | 2/1/0/0 | 0.1 m |
| Xbot / 1 | 66 mixamorig:RightLeg segment | 1556/32/0 | PASS | 2/1/0/0 | 0.1 m |
| Xbot / 2 | 63 mixamorig:LeftToeBase leaf | 420/138/82 | PASS | 2/1/0/0 | 0.1 m |
| CesiumMan / 3 | 18 leg_joint_R_2 segment | 412/16/0 | PASS | 2/1/0/0 | 0.1 m |
| CesiumMan / 4 | 14 leg_joint_L_2 segment | 406/16/0 | PASS | 2/1/0/0 | 0.1 m |
| CesiumMan / 5 | 12 Skeleton_arm_joint_R__3_ leaf | 222/20/0 | PASS | 2/1/0/0 | 0.1 m |

- 물리 진행 캡처 실행도 6/6 GPU==CPU·A/B·gap=0.1 m다. pose가 진행되어 case 4는 406/18/0, components/caps/uncapped/error=2/1/16/0이며 case 5는 222/16/0, 2/1/0/0이다. 나머지 수치는 기본 진단과 같다. case 4의 열린 경계 16개는 D10의 surface 게시 허용 정책이며 fatal error가 아니다. 미포함 교차도 거절 조건으로 쓰지 않는다.
- 최종 `.slnx` Debug/Release 외부 빌드 exit 0(184/186초). `SkeletalPatchTests` Debug/Release 각 50 checks, `CutPlaneTests` 양쪽 모두 build/run exit 0. 외부 출력 재지정은 `%TEMP%/SP5b-review-output.props`를 사용했으며 변경 없는 MiniPhysics/Jolt는 기존 prebuilt 의존성을 재사용했다. 첫 증분 빌드는 기존 저장소 Build 경로에서 실행됐고, 최종 검증용 빌드·실행 파일·CPU 산출물은 TEMP에 있다.
- 최종 Release 기본 진단 exit 0(34초), 물리 진행 캡처 exit 0(51초), 정적 cube 회귀 exit 0(33초). 세 실행 모두 D3D12 ERROR/CORRUPTION 0. 기존 DXGI 하이브리드 출력 ERROR #80과 D3D12 WARNING은 남는다. GPU NVIDIA GeForce MX450, 드라이버 32.0.15.9186(레지스트리 재조회), x64/Windows, v143, SDK 10.0.26100.0.
- 기본 진단: `-skeletal-patch-checks 1 -debug 1`. 캡처: `-skeletal-patch-checks 1 -debug 1 -physics-running 1 -capture <TEMP>/SP5b-review-physics -capture-frame 60 -animation-step-ms 16 -cut-expect-publications 0`. `-physics-sim 0`은 사용하지 않았다. Static: `-debug 1 -model Models/cube.glb -cut 1 -cut-angle 30 -cut-validate 1 -cut-expect-loops 1 -cut-expect-publications 1 -physics-query-exit 90`. 양쪽 cap 2삼각형, 면적·부피·부모 보존·hull 포함·publication=1 검증 통과.
- 로그: `%TEMP%/SP5b-review-external-{Debug,Release}-build.log`, `%TEMP%/SP5b-review-external-build-summary.log`, `%TEMP%/SP5b-review-tests-summary.log`, `%TEMP%/SP5b-review-tests/`, `%TEMP%/step7-Release-SP5b-review-{exact,physics,static}.log`. 실행 helper는 `%TEMP%/SP5b-review-runtime.ps1`이며 최종 외부 실행 파일은 `%TEMP%/SP5b-review-build/Release/Output/ModelViewer/ModelViewer.exe`다. 빌드 완료 전 첫 외부 실행 시도는 CreateProcess error 2였고, 빌드 완료 뒤 같은 이름의 최종 로그로 재실행해 위 exit 0을 확인했다.
- 캡처: `%TEMP%/SP5b-review-Xbot-physics.png`, `%TEMP%/SP5b-review-CesiumMan-physics.png`; 원본은 `%TEMP%/SP5b-review-physics-model{1,2}-{hdr,post}.raw`. PNG를 직접 열어 바닥 위 원본·복사 A/B를 확인했다. 추적 `.mini` 변경은 없어 복원할 파일이 없었다. SP-6·SP-7은 구현하지 않았고 커밋하지 않았다.

## 5. 완료 조건

- C를 누르면 평면이 지나는 segment의 부모 본(segment가 없으면 가장 가까운 말단 joint)이 선택되고, 그 본의 영향 삼각형만 원본 옆에 같은 pose로 보인다.
- 평면을 옮겨 C를 누르면 복사본이 오프셋 평면(무한)으로 잘려 A/B가 되고, B가 법선 방향으로 벌어져 원본 옆에 보인다. 그래도 절단기가 거절하면 이유가 로그에 남는다. cap 결과를 로그로 보고한다.
- CesiumMan과 Xbot에서 같은 코드로 동작한다(모델별 상수 없음).
- (SP-6) Parent/Influence 모드를 바꿔 같은 평면의 결과를 비교할 수 있다.
- 기존 Static C 절단(Cube·Bunny·Dragon)의 동작·배치·진단·벤치마크 결과가 바뀌지 않는다. 원본 skin 메시는 바뀌지 않는다.
- 평면이 skin 메시와 겹치지 않거나 교차 삼각형이 없거나 Compute Pose가 꺼져 있으면 아무것도 만들지 않고 이유를 로그로 남긴다.
- 기본 실행 경로에서 정점 데이터의 CPU readback·CPU 추출이 없다(결과 몇 바이트만 비동기). 본 선택만 CPU 본 행렬로 한다.
- 미포함 교차 수가 로그에 남고, 0이 아니어도 복사·절단은 수행된다.
- Debug/Release 빌드, CPU 테스트, viewer 실행 D3D12 ERROR 0.

## 6. 하지 않는 것

원본 패치 숨김(다음 단계는 NDC 슬라이드대로 버텍스 컬러 마스크 + discard를 기준으로 설계), 원본 A/B 배정(K/D subtree), 본 추종 애니메이션·교점 Blend 출처, BreakConstraint 연결(SP-7, SP-6 결과 확인 후), 복사본 재절단 보장.
