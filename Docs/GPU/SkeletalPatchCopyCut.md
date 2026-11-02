상태: 진행 중 — `ea6ea51f` 기준, SP-1(CesiumMan 배치·CPU 기준 함수·테스트) 완료, SP-2~SP-5 구현 전 (2026-11-02)

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
| D5 | 선택·복사는 전부 GPU Compute에서 한다. 입력은 원본 `Model::m_DataBuffer`(IB R16/R32·draw `startIndex/baseVertex`·VB 끝 16바이트 BLENDINDICES uint16×4·BLENDWEIGHT unorm16×4·UV half2)와 `PosedGeometry::GetOffset(SourceMesh)`의 RAW40, section별 palette→NodeId 표(작은 업로드). weight는 `PreparePoseCS::main`처럼 합으로 정규화한다. CPU로는 NodeId·개수 몇 바이트만 fence 뒤 비동기로 읽는다. | 프로젝트 방향이 GPU 절단이다. 결과 버퍼를 그대로 절단기 입력으로 넘긴다. decode 규칙을 기존 pose 셰이더와 맞춘다. |
| D6 | 본 선택(GPU 2패스). 평면은 component-local로 변환해 넘긴다. 꼭짓점 부호 거리 `|d|<=eps`는 0으로 본다. 삼각형은 `min d < -eps`이고 `max d > eps`일 때 교차다. 교차 선분이 유한 사각형과 겹칠 때만 센다(`ClipCoplanarSegmentToCutPlaneRectangle`과 같은 판정). ① 교차 삼각형의 꼭짓점 weight를 NodeId별 누적 버퍼에 고정소수 정수로 `InterlockedAdd`한다. palette 번호 j는 `m_JointIndices[mesh.startJoint + j]`로 NodeId로 바꾼다. ② 그룹 하나가 최댓값 NodeId(동점이면 작은 NodeId)와 교차 삼각형 수를 쓴다. | palette 번호와 NodeId를 혼용하지 않고 같은 NodeId의 여러 palette 항목을 합친다. 정수 누적이라 실행 순서와 상관없이 결과가 같다. |
| D7 | 패치 추출(GPU). 선택 NodeId에 양수 weight가 있는 꼭짓점이 하나라도 있는 삼각형을 고른다. 선택 플래그 누적합 순서로 압축해 정적 생성 형식에 쓴다: stride 28 packed VB(`WriteVertex`와 같은 순서·flags), RAW40 Pose, depth VB, R32 IB(`baseVertex=0`, draw 1개). 출처 `(section, draw, triangle)`도 쓰고 local bounds를 계산한다. 출력 용량은 대상 삼각형 수 합(상한)으로 미리 잡는다. 자손 본은 포함하지 않는다. | 선택 joint만, 자손 제외라는 기존 결정과 같다. `Mesh::Draw`가 여럿일 수 있어 draw까지 출처에 넣는다. 상한 확보로 overflow가 없다. |
| D7a | 규칙의 기준 구현은 CPU 순수 함수(`SelectJointReference`, `ExtractPatchReference`)로 테스트 코드에만 둔다. 진단 옵션에서 GPU 결과(NodeId·교차 수·패치 수·출처 목록)를 같은 입력의 CPU 기준과 비교한다. 기본 실행 경로에는 CPU 계산이 없다. | GPU 결과의 정답을 사람이 읽을 수 있는 코드로 고정한다. |
| D8 | 복사본 게시. `Scene::CreateObject(name, sourceModel)`로 owner를 만든다. 자동 생성된 컴포넌트에 D7 버퍼를 `GeometryRevision::FromGenerated`로 감싸 `MeshComponent::SetGeometry`로 넣는다(skin 없음, `meshCBV=0`, 원본 material, static PSO). world = 원본 world + X 오프셋(원본 bounds 폭 + 0.5 m). 삼각형 수는 비동기 readback으로 받은 뒤 게시한다(`CutJob::Poll` 방식). | 원본과 겹치지 않게 비교한다. 기존 생성 geometry 렌더 경로(`UpdateBaked`)를 재사용한다. 렌더(`Mesh::Draw`)와 절단기 dispatch가 CPU의 index 수를 쓰므로 개수만 읽는다(기존 Static `CutJob::Poll`과 같다). readback을 없애는 indirect draw·dispatch는 후속이다. 별도 GameObject는 오프셋 진단용 예외다. 원본 숨김 단계에서는 원칙(13.1절)대로 패치 A/B를 원본 GameObject의 MeshComponent로 옮긴다. |
| D9 | 절단 요청. 게시 후 `MeshComponent::Update`/`PosedGeometry::UpdateBaked`로 bounds·pose를 명시적으로 준비한다. Static batch가 끝날 때까지 요청을 pending으로 둔다(`Request` busy 거절 회피). 그다음 오프셋 평면으로 `CuttingSystem::Request(copyObject, copyComponent, plane)`를 부른다. 오프셋 평면은 평면 변환을 Δ만큼 옮겨 `TryBuildCutPlaneData`로 다시 만든다(`WorldPlane`·`WorldToPlane`·`WorldCorners`가 함께 맞도록). | 새 절단 코드를 만들지 않고 검증된 절단기를 쓴다. 결과 A/B는 기존 규칙(같은 owner, A 교체 + B 추가)을 따른다. |
| D10 | 패치는 열린 경계를 가진다. cap 실패는 기존 정책대로 surface만 게시한다. 교차선 끝점이 유한 사각형 밖이라 절단기가 fatal로 거절하면 복사본은 자르지 않은 채 두고 이유를 로그로 남긴다. `LastCaps/LastUncapped/LastError`를 기록하고, `ExpectedMinimumLoops`는 쓰지 않는다. | 본을 골랐다고 절단 성공이 보장되지는 않는다. 실패를 명시적 결과로 허용한다. |
| D11 | 수명과 격리. 새 C는 진행 중인 SkeletalPatchJob과 pending 요청을 무효화하고, `RequestAll` 전에 이전 복사본 owner를 `DeleteEditTarget`의 owner 삭제 경로로 지운다(`CancelTarget`·`PhysicsSystem::RemoveTarget` 포함). 대상이 없어도 이전 복사본은 지운다. 복사본 owner는 Static publication 수·벤치마크·자동 재절단 집계에서 뺀다. | 복사본은 정적이라 다음 C의 Static 후보가 될 수 있다. 진단 화면이 쌓이지 않게 하고, 기존 측정을 바꾸지 않는다. |

## 3. 인터페이스 골격 (이름은 구현 시 주변 코드에 맞춘다)

```cpp
// Scene/SkeletalPatchCopy.h
struct SkeletalPatchResult          // fence 후 비동기로 읽는 몇 바이트
{
    uint32_t NodeId;                // 선택 본, 없으면 무효 값
    uint32_t CrossingTriangles;
    uint32_t PatchTriangles;
};

class SkeletalPatchJob              // CutJob처럼 제출 → Poll
{
public:
    // GPU: 본 선택 2패스 + 패치 추출 1패스를 기록한다
    bool Submit(const MeshComponent& source, const CutPlaneData& plane);
    // fence 완료 시 결과와 패치 GPU 버퍼(GeometryBuffers)를 넘긴다
    bool Poll(SkeletalPatchResult& result, std::shared_ptr<GeometryBuffers>& patch);
};

class SkeletalPatchCopy
{
public:
    void Request(Scene& scene, const CutPlaneData& plane); // C 입력: 이전 복사본 정리, 대상 skin 컴포넌트에 Job 제출
    void Update(Scene& scene, CuttingSystem& cutting);     // 완료 시 복사본 게시 → batch 종료 후 오프셋 평면 절단 요청
};
```

Shader: `Scene/Shaders/PatchJointVoteCS.hlsl`(D6 ①), `PatchJointSelectCS.hlsl`(D6 ②), `PatchExtractCS.hlsl`(D7). CPU 기준 함수는 `Scene/tests/`에만 둔다.

## 4. 단계 (각 약 300줄 이하, 단계마다 새 Codex 실행 + 다른 Codex 리뷰)

| 단계 | 내용 | 완료 증거 | 예상 비용 |
| --- | --- | --- | --- |
| SP-1 | CesiumMan 배치·로드 분리(D1). CPU 기준 함수 + CPU 테스트(합성 입력: R16/R32·draw offset, palette→NodeId, 중복 palette 합산, eps 접촉·공면, 유한 사각형 교차, 양수 weight 포함, 동점, 출력 순서) | CPU 테스트 PASS, Debug/Release 빌드, 기존 네 모델 위치·Static 진단 불변 | 구현 + R 2~5분 + B 약 2분 + T |
| SP-2 | 본 선택 Compute 2패스(D6) + `SkeletalPatchJob` 제출/Poll 골격 + C 입력 연결(D3, D4) + 로그(NodeId, 교차 수). 화면 변화 없음 | CesiumMan 다리/팔 절단 시 해당 NodeId 로그, 진단 옵션의 CPU 기준과 일치 | 구현 + R + B + V |
| SP-3 | 패치 추출 Compute(D7): 누적합 압축, 정적 생성 형식·출처·bounds, 용량 상한. 로그(패치 수) | 진단 옵션에서 패치 수·출처 목록이 CPU 기준과 일치, D3D12 ERROR 0 | 구현 + R + B + V |
| SP-4 | 복사본 owner 게시(D8) + 이전 복사본 정리·격리(D11) | 원본 옆에 같은 pose의 패치 복사본이 보임(캡처), C 반복 시 하나만 유지 | 구현 + R + B + V |
| SP-5 | pose 준비·batch 대기·오프셋 평면 절단 요청(D9) + 결과/실패 로그(D10) | 복사본이 A/B로 잘림(캡처) 또는 실패 이유 로그, 기존 Static C 절단 불변, D3D12 ERROR 0 | 구현 + R + B + V |

한 단계가 300줄을 넘으면 shader와 C++ 연결을 나눈다.

## 5. 완료 조건

- C를 누르면 평면이 지나는 CesiumMan 본이 선택되고, 그 본의 영향 삼각형만 원본 옆에 같은 pose로 보인다.
- 복사본이 오프셋 평면으로 잘려 A/B 두 컴포넌트가 되거나, 절단기 거절 이유가 로그에 남는다. cap 결과를 로그로 보고한다.
- 기존 Static C 절단(Cube·Bunny·Dragon)의 동작·배치·진단·벤치마크 결과가 바뀌지 않는다. 원본 skin 메시는 바뀌지 않는다.
- 평면이 skin 메시와 겹치지 않거나 교차 삼각형이 없거나 Compute Pose가 꺼져 있으면 아무것도 만들지 않고 이유를 로그로 남긴다.
- 기본 실행 경로에서 정점 데이터의 CPU readback·CPU 선택/추출이 없다(결과 몇 바이트만 비동기).
- Debug/Release 빌드, CPU 테스트, viewer 실행 D3D12 ERROR 0.

## 6. 하지 않는 것

원본 패치 숨김, 원본 A/B 배정(K/D subtree), 본 추종 애니메이션·교점 Blend 출처, BreakConstraint 연결, 복사본 재절단 보장.
