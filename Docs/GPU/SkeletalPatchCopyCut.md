상태: 진행 중 — `9f26d066` 기준 미커밋, SP-4 구현·리뷰 완료: GPU 패치 정적 owner 게시·새 C 공통 삭제·Static 집계 격리, posed 월드 X 폭 기반 오프셋 수정·복사 정점 위치 수치 검증, 위팔 2회(328)·허벅지(352)·손(222) 모두 복사본 1개 PASS, Debug/Release·CPU 테스트·Static cube 회귀 통과·D3D12 ERROR 0; SP-5 구현 전 (2026-11-02)

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
| D6 | 본 선택(CPU, segment 부모). segment는 저장된 데이터가 아니라 C를 누를 때 만든다: 부모 관계는 `ModelInstance::m_PoseParents`(생성 시 `BuildPoseParents`), 위치는 그 프레임 `GetNodeTransforms()`(model space)에 원본 world를 곱한 joint 원점. skin joint(`m_JointIndices`에 있는 NodeId)마다 가장 가까운 skin joint 조상을 부모로 삼아 segment(부모 → joint)를 만든다. 양 끝 부호 거리가 다르고 교차점이 유한 평면 사각형 안이면 후보다. 후보가 여럿이면 교차점이 평면 중심에 가장 가까운 하나만 쓰고 여러 segment 처리는 하지 않는다. 선택 본 = 그 segment의 부모 joint. 후보가 없으면 말단 처리: 자식 skin joint가 없는 joint 중 world 위치가 평면 중심에 가장 가까운 joint를 고르고 같은 패치 방식으로 복사·절단한다. 선택 NodeId의 palette 번호 마스크(`m_JointIndices[mesh.startJoint + j] == NodeId`)를 CPU에서 만들어 추출 패스에 넘긴다. 로그: 선택 본, segment(또는 말단), 교차점. | **[슬라이드]** 「BreakConstraint()를 사용할 본 선택」·"절단할 위치에서 Bone을 선택" — 고르는 본이 끊을 본이다. **[추론]** Unreal의 physics body는 bone→자식 segment를 감싸므로 평면이 지나는 body = segment 부모 joint. 평면이 B→C 위에 있을 때 C를 고르면 패치(C)가 단면을 덮지 못해 엉뚱한 관절에서 떨어진다. joint 수만큼의 계산이라 GPU 패스보다 싸고 결과가 즉시 나온다. |
| D7 | 패치 추출(GPU). 선택 NodeId에 양수 weight가 있는 꼭짓점이 하나라도 있는 삼각형을 고른다. 선택 플래그 누적합 순서로 압축해 정적 생성 형식에 쓴다: stride 28 packed VB(`WriteVertex`와 같은 순서·flags), RAW40 Pose, depth VB, R32 IB(`baseVertex=0`, draw 1개). 출처 `(section, draw, triangle)`도 쓰고 local bounds를 계산한다. 출력 용량은 대상 삼각형 수 합(상한)으로 미리 잡는다. 자손 본은 포함하지 않는다. 같은 패스에서 평면 교차 삼각형 수와 그중 선택 본 weight가 없는 "미포함 교차" 수를 센다(교차 = 꼭짓점 부호 거리 `|d|<=eps`는 0, `min d < -eps`이고 `max d > eps`, 교차 선분이 유한 사각형과 겹침 — `ClipCoplanarSegmentToCutPlaneRectangle`과 같은 판정). 미포함 교차가 있어도 복사·절단은 그대로 수행하고 로그만 남긴다. | 선택 joint만, 자손 제외라는 기존 결정과 같다. `Mesh::Draw`가 여럿일 수 있어 draw까지 출처에 넣는다. 상한 확보로 overflow가 없다. 미포함 교차(weight 경계와 joint 위치 어긋남·사선 절단)가 얼마나 생기는지 측정해, 원본 숨김 단계에서 NDC식 각도 보정·영역 병합 중 무엇이 필요한지 정한다. |
| D7a | 추출 규칙의 기준 구현은 CPU 순수 함수(`ExtractPatchReference`)로 테스트 코드에만 둔다. segment 선택은 런타임 CPU 함수 자체를 테스트한다. 진단 옵션에서 GPU 결과(교차 수·미포함 교차 수·패치 수·출처 목록)를 같은 입력의 CPU 기준과 비교한다. 기본 실행 경로에는 CPU 계산이 없다. | GPU 결과의 정답을 사람이 읽을 수 있는 코드로 고정한다. |
| D8 | 복사본 게시. `Scene::CreateObject(name, sourceModel)`로 owner를 만든다. 자동 생성된 컴포넌트에 D7 버퍼를 `GeometryRevision::FromGenerated`로 감싸 `MeshComponent::SetGeometry`로 넣는다(skin 없음, `meshCBV=0`, 원본 material, static PSO). world = 원본 world + X 오프셋(원본 bounds 폭 + 0.5 m). 삼각형 수는 비동기 readback으로 받은 뒤 게시한다(`CutJob::Poll` 방식). | 원본과 겹치지 않게 비교한다. 기존 생성 geometry 렌더 경로(`UpdateBaked`)를 재사용한다. 렌더(`Mesh::Draw`)와 절단기 dispatch가 CPU의 index 수를 쓰므로 개수만 읽는다(기존 Static `CutJob::Poll`과 같다). readback을 없애는 indirect draw·dispatch는 후속이다. 별도 GameObject는 오프셋 진단용 예외다. 원본 숨김 단계에서는 원칙(13.1절)대로 패치 A/B를 원본 GameObject의 MeshComponent로 옮긴다. |
| D9 | 절단 요청. 게시 후 `MeshComponent::Update`/`PosedGeometry::UpdateBaked`로 bounds·pose를 명시적으로 준비한다. Static batch가 끝날 때까지 요청을 pending으로 둔다(`Request` busy 거절 회피). 그다음 오프셋 평면으로 `CuttingSystem::Request(copyObject, copyComponent, plane)`를 부른다. 오프셋 평면은 평면 변환을 Δ만큼 옮겨 `TryBuildCutPlaneData`로 다시 만든다(`WorldPlane`·`WorldToPlane`·`WorldCorners`가 함께 맞도록). | 새 절단 코드를 만들지 않고 검증된 절단기를 쓴다. 결과 A/B는 기존 규칙(같은 owner, A 교체 + B 추가)을 따른다. |
| D10 | 패치는 열린 경계를 가진다. cap 실패는 기존 정책대로 surface만 게시한다. 교차선 끝점이 유한 사각형 밖이라 절단기가 fatal로 거절하면 복사본은 자르지 않은 채 두고 이유를 로그로 남긴다. `LastCaps/LastUncapped/LastError`를 기록하고, `ExpectedMinimumLoops`는 쓰지 않는다. | 본을 골랐다고 절단 성공이 보장되지는 않는다. 실패를 명시적 결과로 허용한다. |
| D11 | 수명과 격리. 새 C는 진행 중인 SkeletalPatchJob과 pending 요청을 무효화하고, `RequestAll` 전에 이전 복사본 owner를 `DeleteEditTarget`의 owner 삭제 경로로 지운다(`CancelTarget`·`PhysicsSystem::RemoveTarget` 포함). 대상이 없어도 이전 복사본은 지운다. 복사본 owner는 Static publication 수·벤치마크·자동 재절단 집계에서 뺀다. | 복사본은 정적이라 다음 C의 Static 후보가 될 수 있다. 진단 화면이 쌓이지 않게 하고, 기존 측정을 바꾸지 않는다. |

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
| SP-4 | 복사본 owner 게시(D8) + 이전 복사본 정리·격리(D11) | 완료(미커밋): 캡처·위팔 재요청 포함 복사본 1개, 원본 posed 월드 X 폭 + 0.5 m, GPU packed VB와 원본 pose의 월드 위치 대조 PASS | 구현 + R + B + V |
| SP-5 | pose 준비·batch 대기·오프셋 평면 절단 요청(D9) + 결과/실패 로그(D10) | 복사본이 A/B로 잘림(캡처) 또는 실패 이유 로그, 기존 Static C 절단 불변, D3D12 ERROR 0 | 구현 + R + B + V |

한 단계가 300줄을 넘으면 shader와 C++ 연결을 나눈다.

SP-3 리뷰 근거: `skeletonRoot`에서 부모 공간이 끊긴 본 원점에 `JointFrameToWorld`가 렌더와 같은 `component world × inverse(SourceFrame) × mesh node`를 적용한다. 진단 평면도 같은 좌표로 만들며, 추출 RAW40·packed VB는 component-local을 유지하고 `RequestSkeletalPatch`에서 cutter와 같은 방식으로 평면을 변환한다. 보정 전 무한 평면 교차 198/0/187은 보정 후 47/108/69이며, 위팔의 미포함 교차 5는 거절 조건으로 쓰지 않는다. 비단위 평면의 eps 투영과 현재 패치 bounds도 보정했다. Debug/Release 빌드·CPU 테스트(SkeletalPatchTests 45개)·정적 cube 회귀가 통과했고, 회전·비균일 scale·SourceFrame 추가 진단도 3지역 GPU/CPU PASS다.

SP-4 리뷰 근거: 기존 약 6.874 m 오프셋은 `MeshComponent::GetBoundingSphere`의 느슨한 구 지름 + 0.5 m였다. `SkeletalPatchJob::Submit`의 추출 패스에서 선택과 무관하게 원본 전체 draw의 posed 정점을 component world로 옮겨 X 최소·최대를 집계하도록 수정했다. 일반 경로는 두 스칼라만 추가 readback하며 정점 readback은 진단 전용이다. `SkeletalPatchJob::Validate`는 원본 pose와 실제 GPU packed VB를 게시 TRS로 변환하여 오프셋 제거 후 비교한다. 진단 입력의 X 경계는 [11.696115, 13.003948] m, 폭 1.307833 m, 오프셋 1.807833 m다. 손 영역의 원본/복사본(오프셋 제거) AABB는 모두 min=(11.696115, -0.327247, -1.023413), max=(12.165718, 0.520666, -0.291195) m이고 정점별 최대 오차는 0 m다. 이는 제출 pose의 비교이며 이후 원본 애니메이션을 따라가는 복사본은 아니다. 리뷰 빌드·실행 로그는 `%TEMP%/step7-SP4-review-final-{Debug,Release}.log`, `%TEMP%/step7-Release-SP4-review-patch-final.log`, `%TEMP%/step7-Release-SP4-review-static.log`, CPU 산출물은 `%TEMP%/sp4-review-tests/`, 캡처는 `%TEMP%/SP4-review-copy.png`다. SP-5의 복사본 절단과 A/B 결과 완료 조건은 아직 적용 전이다.

## 5. 완료 조건

- C를 누르면 평면이 지나는 segment의 부모 본(segment가 없으면 가장 가까운 말단 joint)이 선택되고, 그 본의 영향 삼각형만 원본 옆에 같은 pose로 보인다.
- 복사본이 오프셋 평면으로 잘려 A/B 두 컴포넌트가 되거나, 절단기 거절 이유가 로그에 남는다. cap 결과를 로그로 보고한다.
- 기존 Static C 절단(Cube·Bunny·Dragon)의 동작·배치·진단·벤치마크 결과가 바뀌지 않는다. 원본 skin 메시는 바뀌지 않는다.
- 평면이 skin 메시와 겹치지 않거나 교차 삼각형이 없거나 Compute Pose가 꺼져 있으면 아무것도 만들지 않고 이유를 로그로 남긴다.
- 기본 실행 경로에서 정점 데이터의 CPU readback·CPU 추출이 없다(결과 몇 바이트만 비동기). 본 선택만 CPU 본 행렬로 한다.
- 미포함 교차 수가 로그에 남고, 0이 아니어도 복사·절단은 수행된다.
- Debug/Release 빌드, CPU 테스트, viewer 실행 D3D12 ERROR 0.

## 6. 하지 않는 것

원본 패치 숨김(다음 단계는 NDC 슬라이드대로 버텍스 컬러 마스크 + discard를 기준으로 설계), 원본 A/B 배정(K/D subtree), 본 추종 애니메이션·교점 Blend 출처, BreakConstraint 연결, 복사본 재절단 보장.
