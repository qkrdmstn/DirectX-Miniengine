# 2단계 설계: 프로시저럴 패치 평면 슬라이싱 (v2)

> 작성: 2026-10-19 · 대상 커밋: `7e921f9` (본 영향 삼각형 → 프로시저럴 패치 복사)
>
> v2: Codex 설계 리뷰(높음 7, 중간 4)를 반영했다. 대응표는 9절에 있다.
>
> 상태: **설계안**. 아래 코드는 아직 없고, 동작·성능은 검증되지 않았다.

## 0. 출발점 (현재 코드)

| 구성 | 위치 | 이 단계와의 관계 |
| --- | --- | --- |
| `ProceduralMeshData` / `ProceduralMesh` | `ProceduralMesh.h` | **렌더 출력 전용**으로 쓴다. 슬라이스 계산은 별도 자료(2절)에서 한다 |
| `ProceduralMeshRenderer` | `Rendering/` | 양면 PSO 하나, section은 그리기 범위로만 쓰고 재질은 읽지 않는다 → 3.7에서 확장 |
| `ExtractBonePatch` | `BonePatch.*` | 직전 `ModelInstance::Update`의 포즈로 world 스냅샷을 만든다. ModelViewer의 C 키는 Update 앞에서 실행되므로 화면에 보이는 포즈와 같다 |
| `BonePatchVertex` | `BonePatch.h` | 렌더 정점 단위의 기준 자세 위치·법선·joint·weight·원본 정점 ID |

렌더러 기본 규약: `RasterizerDefault`는 `CULL_BACK`, `FrontCounterClockwise = TRUE`다. 패치는 원본 winding을 그대로 쓴다.

## 1. 핵심 결정

### 1.1 절단은 포즈된 패치 공간(현재 world)에서 한다
패치 위치가 world 스냅샷이므로 평면도 같은 공간에서 받는다. 기준 자세 공간으로 되돌려 자르지 않는다. LBS는 기준 자세의 평면을 포즈 공간의 평면으로 보내지 않기 때문이다. **평면을 만드는 본 행렬은 패치를 추출할 때와 같은 스냅샷에서 가져온다**(2.1의 `poseJointWorld`). 디버그 표시 오프셋은 패치와 평면에 똑같이 적용한다.

### 1.2 위치는 기하 노드가 소유하고, 렌더 정점은 노드를 참조한다
리뷰 2·3을 반영했다. UV seam이나 hard normal 때문에 같은 점에 렌더 정점이 여럿 있어도 **위치는 기하 노드 하나에만 있다**. 렌더 정점은 노드 인덱스와 자기 속성(UV·법선·색)만 가진다.

- 슬라이스 거리 `d`, 교차 위치, 매 프레임 스키닝 결과는 모두 노드 단위로 계산한 뒤 렌더 정점에 복사한다.
- seam 양쪽, 양쪽 반쪽, cap은 같은 노드 값을 복사하므로 비트 단위로 같다.

### 1.3 노드 용접은 "위치 + 스킨 동등성 + 메시 슬롯"으로 한다
리뷰 2를 반영했다. 용접 키는 `(meshSlot, 기준 자세 위치 비트, (joint, weight) 쌍을 joint 순으로 정렬한 값)`이다.

- 위치는 같은데 스킨이 다른 정점은 **용접하지 않는다**. 그 에지는 `UnweldedSeam` 경계가 되고, 진단 개수를 기록한다.
- epsilon 용접은 쓰지 않는다. 맞닿은 별도 shell까지 합칠 수 있기 때문이다.
- 서로 다른 `meshSlot`에 걸친 seam도 용접하지 않는다(같은 처리).
- CesiumMan에서 실제 개수는 2-1에서 측정한다. 0이 아니면 해당 seam을 지나는 절단을 거절한다(3.4).

### 1.4 출처는 "기하 노드에 대한 무게중심 좌표"로 기록한다
```cpp
struct PatchOrigin
{
    uint32_t node[3];   // 원본 패치의 기하 노드 (스킨 대표 정점)
    float    bary[3];   // 합 1. node는 오름차순, 쓰지 않는 칸은 weight 0
};
```
- 매 프레임 위치는 `Σ bary_i · LBS(node_i)`이고, **합산 순서는 node 오름차순으로 고정**한다. 노드 단위로 계산하므로 seam 양쪽 결과도 같다.
- 본 ID와 웨이트는 보간하지 않는다.
- **표면에서만 유효하다.** 원본 패치 표면을 반복해 잘라도 모든 하위 삼각형은 원래 삼각형 하나 안에 있으므로 노드는 3개 이하다. 3개를 넘으면 assert하지 않고 `InvalidOrigin`으로 거절한다.
- cap 노드는 3.5의 별도 타입을 쓴다(리뷰 4).

### 1.5 분류는 3상태(+, 0, −)이고, 0 노드는 그 자체가 루프 점이다
리뷰 1을 반영했다. tie-to-positive 규칙은 폐기한다.

- `|d| <= ε`이면 0이다. ε은 패치 bounds 반지름의 1e-5배이고 옵션으로 바꿀 수 있다.
- 분류는 노드 단위이므로 이웃 삼각형의 판정이 항상 같다.
- **새 교차점은 양 끝이 엄격히 반대 부호인 에지에서만 생긴다.** 따라서 `t ∈ (0, 1)`이고, 에지 끝점으로 붕괴하는 교차점이나 길이 0인 선분이 생기지 않는다.

| 삼각형 부호 | 처리 |
| --- | --- |
| 모두 +/0 (0 아닌 것이 하나 이상) | + 반쪽에 그대로 |
| 모두 −/0 (0 아닌 것이 하나 이상) | − 반쪽에 그대로 |
| (0, +, −) | +−에지만 분할 → 삼각형 2개, 양쪽에 하나씩 |
| (+, +, −) 류 | 에지 2개 분할 → 1개 + 사각형(2개) |
| (0, 0, 0) 평면과 겹치는 면 | **`CoplanarFace`로 거절**. 소유 규칙은 확장 단계에서 정한다 |

### 1.6 절단 루프는 "각 반쪽의 경계"에서 뽑는다
삼각형별로 선분을 따로 기록하지 않는다. 분할이 끝난 뒤 반쪽마다 경계 에지를 다시 계산하고, **양 끝이 모두 평면 위 노드(새 교차점 또는 0 노드)인 경계 에지**를 절단 에지로 본다.

- 평면에 닿기만 하고 넘지 않는 경우(에지가 평면에 있고 양쪽 면이 같은 반쪽)에는 그 에지가 경계가 아니므로 루프가 생기지 않는다. 접촉과 절단이 자동으로 구분된다.
- 방향은 반쪽의 면 winding에서 얻는다. 노드마다 in-degree와 out-degree가 1이어야 한다.

### 1.7 경계는 종류를 가지고 계승된다
리뷰 5를 반영했다. 최초 패치의 경계 집합을 고정해서 쓰지 않는다. piece마다 현재 adjacency에서 다시 계산하고, 에지 종류를 함께 들고 다닌다.

```cpp
enum class PatchEdgeKind : uint8_t
{
    Interior,
    Replacement,   // 패치 추출 때의 열린 교체 경계
    UnweldedSeam,  // 1.3에서 용접하지 않은 seam
    Cut,           // 이번 절단으로 생긴 경계 (cap 전)
    CapSeam        // cap과 표면이 만나는 에지 (cap 후, Interior와 같은 위상)
};
```
에지 AB를 P에서 나누면 AP와 PB는 AB의 종류를 물려받는다. 0 노드가 `Replacement`/`UnweldedSeam` 경계 위에 있거나 절단 에지가 그런 경계에 닿으면, 루프의 그 점은 차수 1이 된다. 이때는 `OpenLoop`로 거절하고, 원인 에지 종류를 진단에 남긴다. 차수 이상의 나머지 경우는 `NonManifoldLoop`다.

### 1.8 cap은 루프 노드를 공유하고, 구동 방식은 선택한다
리뷰 4·6을 반영했다. earcut은 Steiner 점을 추가하지 않으므로, **cap 삼각형은 루프 노드만 사용한다**. cap 정점은 렌더 속성(법선 ±n, 평면 UV)만 따로 가지고, 위치는 표면과 같은 노드를 참조한다.

- 기본(`CapDrive::FollowRim`): 경계 노드가 표면 스키닝을 따르므로 cap과 표면이 위상적으로 수밀하다. 경계에 틈이 생기지 않는다. 대신 크게 굽히면 cap이 평면성을 잃을 수 있다.
- NDC 방식(`CapDrive::RigidToJoint`, 3~4주차): 절단 시점의 부착 본 world 행렬과 노드의 본 로컬 위치를 저장해 두고 강체로 움직인다. 경계 틈이 생길 수 있으며, 두 방식을 비교하는 것 자체가 실험 항목이다.
- 두 방식에 필요한 데이터(경계 노드 ↔ 표면 노드 대응, cutId)는 2-4에서 저장한다. 부착 본과 절단 시점 행렬은 쓰는 곳이 없어 `PatchCap`에서 뺐다(필요해지면 그때 다시 넣는다).

earcut은 공선점을 걸러낼 수 있다. 그래서 cap 생성 뒤 **모든 절단 에지가 cap 삼각형의 에지로 쓰였는지** 검사한다. 빠진 노드 b가 있으면 에지 (a, c)를 가진 cap 삼각형을 (a, b, x)와 (b, c, x)로 나눈다. 이렇게 해서 T-junction을 막는다.

## 2. 자료 구조

```cpp
namespace MeshCutting
{
    // ---- 패치 추출 결과 확장 (BonePatch.h) ----
    struct BonePatchTriangle            // 원본 교체·숨김(3주차)·재절단 병합용 (리뷰 7)
    {
        uint32_t meshSlot;
        uint32_t drawIndex;             // Mesh::draw[]
        uint32_t primitive;             // draw 안의 삼각형 번호
    };
    // BonePatchSource에 추가:
    //   std::vector<BonePatchTriangle> triangles;    // 패치 삼각형과 1:1
    //   std::vector<Math::Matrix4> poseJointWorld;   // 추출 시점 팔레트 본 world (평면·cap 부착 기준)

    // ---- 슬라이스 작업 자료 (Geometry/Patch.h) ----
    enum class PatchNodeKind : uint8_t { Surface, Cap };
    struct PatchNode
    {
        PatchNodeKind kind;
        PatchOrigin origin;           // Surface일 때
        uint32_t cutId = 0;             // 이 노드를 만든 절단 (원본은 0)
        DirectX::XMFLOAT3 position;     // 현재 포즈 위치. 모든 렌더 정점이 복사해 간다
    };

    struct PatchCorner                 // 렌더 정점 = 노드 + 속성
    {
        uint32_t node;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv;
        DirectX::XMFLOAT4 color;
    };

    enum class PatchFaceKind : uint8_t { Surface, Cap };
    struct Face
    {
        uint32_t corner[3];
        PatchFaceKind kind;
        uint32_t sourceTriangle;        // BonePatchSource::triangles 인덱스 (Cap이면 UINT32_MAX)
        uint32_t cutId;                 // Cap이면 만든 절단. Surface이면 그 면을 분할한 절단 (분할된 적 없으면 0)
    };

    struct PatchCap                    // 절단 하나 = cap 하나 (양쪽 piece에 각각)
    {
        uint32_t cutId;
        DirectX::XMFLOAT3 planePoint, planeNormal;   // piece 바깥 방향 법선
        std::vector<uint32_t> rimNodes;              // 루프 순서
    };

    struct PatchPiece
    {
        std::vector<PatchNode> nodes;
        std::vector<PatchCorner> corners;
        std::vector<Face> faces;
        std::unordered_map<uint64_t, PatchEdgeKind> boundary;   // 정렬된 (nodeA, nodeB) → 종류
        std::vector<PatchCap> caps;
        uint32_t skinStateId = 0;       // piece별 분리 스키닝 상태 (4주차). 0 = 원본과 동일
    };

    // ProceduralMeshData로 굽기: section 0 표면, section 1 cap (cap 면이 있을 때만)
    void BuildRenderMesh(const PatchPiece& piece, ProceduralMeshData& out);
}
```

**piece별 스키닝 상태(리뷰 7)**: 두 반쪽은 같은 원본 노드를 출처로 쓰지만, 분리 후에는 서로 다른 본 기여를 가져야 한다. 그래서 `LBS(node)`를 전역 캐시로 두지 않고 piece마다 평가한다. 2-5에서는 모든 piece가 상태 0(원본과 동일)이다. 4주차에 `SkinState { 분리 본 집합, 절단 시점 본 행렬, 고정 기여의 좌표 공간 }`을 채운다. 좌표 공간은 계획 5.4의 검증 순서에 따라 정한다.

**추출 본 집합과 분리 본 집합**: 분리할 때 떨어져 나가는 쪽은 선택 본과 그 자손 전체다. 그런데 현재 `ExtractBonePatch`는 선택 본 하나의 영향만 본다. 2-0에서 추출 조건을 "선택 본 + 자손 중 하나라도 weight > minWeight"로 바꿀지 결정한다. 바꾸지 않는다면 자손 영향이 패치 밖에 남는지 시각화로 확인한다.

## 3. SlicePatch 계약과 흐름

```cpp
enum class SliceStatus
{
    Ok,                    // 두 piece 모두 비어 있지 않음. loops는 0개 이상
    NoCut,                 // 모든 면이 한쪽(접촉 포함). piece를 만들지 않는다
    InvalidInput,          // 비유한 값, 비정규 평면, 범위 밖 인덱스
    InvalidOrigin,         // bary 합/범위 위반, 노드 3개 초과
    NonManifoldInput,      // 에지를 쓰는 면이 3개 이상이거나 방향이 맞지 않음
    DegenerateInput,       // 면적 0인 입력 면
    CoplanarFace,
    CrossesCap,            // 기존 cap 면을 지나는 재절단 (지원 전)
    OpenLoop,              // Replacement/UnweldedSeam 경계에 닿음
    NonManifoldLoop,
    InvalidLoop,           // 자기 교차, 루프 간 교차, 지원하지 않는 중첩
    CapTriangulationFailed
};

struct SliceOptions
{
    CapMode cap = CapMode::Create;     // None: 진단용. Cut 경계를 연 채로 Ok
    float epsilonScale = 1e-5f;        // × piece bounds 반지름
    float capUvScale = 1.0f;
};

struct SliceResult
{
    SliceStatus status = SliceStatus::InvalidInput;
    PatchPiece positive, negative;     // status == Ok일 때만 유효
    std::vector<std::vector<uint32_t>> loops;  // +쪽 piece 기준 노드 순서
    std::string detail;                // 실패 위치(면/에지/노드 번호)
};

// in은 const. 시작할 때 out을 비우고, 실패하면 status와 detail만 채운다.
SliceStatus SlicePatch(const PatchPiece& in, const SlicePlane& plane,
    uint32_t cutId, const SliceOptions& options, SliceResult& out);
```

호출자는 `Ok`일 때만 기존 piece를 결과로 바꾼다. `NoCut`을 포함한 모든 실패에서 화면의 piece는 그대로 유지된다. 평면 양쪽에 떨어진 shell이 있어 교차 없이 나뉘는 경우는 `Ok`이고 loops는 0개다.

### 3.1 입력 검증 (리뷰 9)
- 평면: 유한한 값이고 법선 길이가 1e-6보다 커야 한다.
- 노드: 위치가 유한하고, origin의 bary가 [0, 1] 범위이며 합이 1±1e-4여야 한다. 노드 수는 3 이하이고 인덱스가 범위 안이어야 한다.
- 면: corner와 node 인덱스가 범위 안이어야 하고, 같은 노드가 반복되면 안 된다. 면적이 ε² 이하이면 `DegenerateInput`이다.
- 에지 incidence가 2를 넘거나, 내부 에지를 두 면이 같은 방향으로 쓰면 `NonManifoldInput`이다.
- 이 검증은 2-1의 `BuildPatchPiece`(BonePatch → PatchPiece 변환)에서도 그대로 실행한다.

### 3.2 분류와 분할
1. 노드별 `d` 계산 → 3상태 분류 (1.5)
2. 평면이 Cap 면을 지나면 `CrossesCap` (리뷰 4)
3. 면 순회와 분할. 교차 노드는 **기하 에지 캐시** `(min node, max node) → 새 노드`에 저장한다. 위치는 캐시에 한 번만 계산하고(낮은 노드에서 높은 노드 방향 `t`), 렌더 corner는 그 노드를 참조한다. UV·법선·색은 렌더 에지별로 보간하되, 에지 방향이 반대이면 `1 - t`를 쓴다. 보간한 법선 길이가 1e-6 이하이면 면 법선으로 대체한다.
4. 새 노드의 origin = 양 끝 origin을 `t`로 섞어 노드별로 합친 값. 노드가 3개를 넘으면 `InvalidOrigin`이다.
5. 경계 종류 계승 (1.7)

### 3.3 루프 구성 (1.6)
반쪽마다 경계를 다시 계산하고 `Cut` 에지만 모은다. 방향 그래프에서 in/out degree를 검사하고 루프를 추출한다.

### 3.4 루프 검증 (리뷰 8)
- 각 루프를 평면 2D(`u`, `v = n × u`)로 투영하고, 자기 교차를 O(n²) 선분 검사로 확인한다. 루프 사이 교차도 검사한다.
- 포함 트리를 만든다. 깊이 0은 외곽, 1은 구멍이다. 깊이 2 이상(구멍 속 섬)은 이번 범위에서 `InvalidLoop`이다.
- 부호 면적의 방향이 깊이 짝수/홀수와 맞지 않으면 `InvalidLoop`이다(winding 불일치를 잡는다).

### 3.5 cap 생성 (1.8)
- 외곽 + 소속 구멍 단위로 earcut을 돌린다.
- T-junction 보정(1.8) 뒤 다음을 검증한다.
  - cap 면적 합 = 루프 부호 면적 합 (상대 오차 1e-4)
  - 모든 `Cut` 에지가 cap 에지와 정확히 한 번씩 짝지어지는지 → 짝지어지면 `CapSeam`으로 바꾼다
- 실패하면 `CapTriangulationFailed`다.
- +쪽 cap은 법선 −n, −쪽 cap은 +n이다. winding은 각 piece의 바깥을 향하게 한다(CCW front).
- cap UV는 `(dot(p−q, u), dot(p−q, v)) · capUvScale`이다. `PatchCap`를 piece마다 추가한다.

### 3.6 출력 수밀성
cap 후 각 piece의 경계 = `Replacement` + `UnweldedSeam`만 남아야 한다. `Cut`이 남으면 버그다. `CapMode::None`이면 `Cut`이 남는 것이 정상이다.

### 3.7 렌더러 확장 (리뷰 10)
`ProceduralMeshRenderer::Draw`에 `DrawOptions { CullMode cull = TwoSided; bool sectionTint = false; }`를 추가한다. PSO는 양면/후면 컬링 두 개를 둔다. `sectionTint`는 section별 고정 색으로 cap을 구분하는 디버그용이다. 재질 시스템은 7주차 범위다. 빈 section은 만들지 않는다(`SetMesh`가 거절한다).

## 4. 매 프레임 갱신 (2-5)
1. piece마다 원본 노드 LBS를 계산한다(상태 0이면 원본과 같은 식).
2. 모든 Surface 노드: `Σ bary · LBS`를 node 오름차순으로 합산한다.
3. Cap 노드: `FollowRim`이면 rim 노드와 같은 노드라 추가 계산이 없다.
4. `BuildRenderMesh`의 위치만 갱신해 `UpdateVertices`로 올린다(위상 불변).

## 5. 파일 배치

```
MeshCutting/
  BonePatch.h/.cpp           # + triangles, poseJointWorld
  Geometry/                  # Model/Core에 의존하지 않는다
    Patch.h/.cpp             # PatchNode, PatchCorner, PatchFace, PatchPiece, 검증, 경계 재계산 + BonePatch → PatchPiece (용접 키 1.3)
    Slice.h/.cpp             # 분류·분할·계승
    SliceCap.cpp             # 루프 추출·2D 검증·포함 트리 + ear clipping, PatchCap (선언은 Slice.h의 내부용 구역)
    Skinning.h/.cpp          # SkinPatchPiece, joint 집합 규칙
    CutPlane.h/.cpp          # 절단 평면, 뼈 선분 찾기
  Rendering/
    RenderBake.h/.cpp        # PatchPiece → ProceduralMeshData (Core에 의존하지 않는다)
  ThirdParty/earcut.hpp      # mapbox/earcut.hpp (ISC). 추가 전 승인, 라이선스 동봉
  tests/PlaneSlicerTests.cpp
```

## 6. 구현 순서

| 단계 | 내용 | 완료 확인 |
| --- | --- | --- |
| 2-0 | `BonePatchSource`에 triangles·poseJointWorld 추가. 추출 본 집합 결정 | 기존 C/X 동작 유지, 삼각형 수 일치 |
| 2-1 | PatchGeometry + PatchWeld + 입력 검증 + 경계 종류. 디버그로 경계를 종류별 색으로 표시 | CesiumMan 패치의 용접 전/후 노드 수, `UnweldedSeam` 개수 보고 |
| 2-2 | PlaneSlicer (`CapMode::None`) + RenderBake. ModelViewer에서 평면을 표시(패치와 같은 오프셋)하고 두 piece를 ±n으로 벌려 그림 | 테스트 1~5 통과, 두 piece 면적 합 = 원본 |
| 2-3 | CutLoops + 거절 사유 UI | 테스트 6~9 |
| 2-4 | CapBuilder + 렌더러 컬링/tint 옵션 | 테스트 10~12, 후면 컬링 PSO에서 cap이 보이는지 **사용자 확인 필요** |
| 2-5 | piece별 매 프레임 스키닝 (상태 0) | 절단 후 애니메이션을 재개해도 piece가 원본과 겹치고 rim 틈이 없음 (**사용자 확인 필요**) |

## 7. 테스트 (tests/PlaneSlicerTests.cpp)

입력 메시는 테스트 코드에서 직접 만든다. 기대값은 메시 삼각분할에서 계산한 값이다.

1. **삼각형화된 닫힌 상자, 중앙 수평 절단**: 측면 대각선까지 지나면 루프 1개·노드 8개(세로 에지 4 + 대각선 4). 대각선 방향에 따라 개수를 테스트에 명시한다.
2. **꼭짓점을 지나는 평면** (0 노드): 길이 0 선분과 면적 0 면이 없고, 루프가 닫힌다.
3. **에지 전체가 평면 위, 양쪽 면이 같은 쪽** (접촉): `NoCut`
4. **평면과 겹치는 면**: `CoplanarFace`
5. **빗나간 평면**: `NoCut`, out에 piece 없음
6. **열린 격자**: `OpenLoop`, detail에 `Replacement` 표시
7. **UV seam 원통** (같은 위치·같은 스킨의 렌더 정점 2개): 루프 1개, 양쪽 UV 보존, seam 두 corner의 위치가 비트 단위로 같음
8. **스킨이 다른 seam**: 용접하지 않음 → seam을 지나는 절단은 `OpenLoop`(UnweldedSeam)
9. **구멍 있는 단면** (두꺼운 관): 외곽 1개 + 구멍 1개, cap 면적 = 외곽 − 구멍
10. **cap 수밀성**: cap 후 경계에 `Cut`이 없음. earcut이 공선점을 버리는 입력(루프 위 공선 노드)에서도 T-junction 없음
11. **재절단**: 표면만 지나는 두 번째 절단은 `Ok`이고 모든 origin의 노드가 3개 이하·Σbary=1. 첫 절단의 경계 에지(`Cut`에서 갈라진 `CapSeam` 포함)와 다시 교차할 때의 계승 결과를 확인한다. cap을 지나면 `CrossesCap`
12. **실패 시 out**: 이전 성공 결과가 담긴 out을 재사용해도 실패 후 piece가 비어 있음

## 8. 이 단계에서 하지 않는 것
- 원본 숨김(3주차), `RigidToJoint` 구동과 본 분리·물리(3~4주차). 필요한 식별자는 2-0·2-4에서 저장하고, 부착 본과 행렬은 필요해질 때 추가한다.
- 평면과 겹치는 면의 소유 규칙, 구멍 속 섬, cap을 지나는 재절단, 여러 본 패치 병합.
- 성능 목표. 단계별 시간은 계측만 하고 값은 측정한 다음에 적는다.

## 9. Codex 리뷰 대응표

| # | 지적 | 반영 |
| --- | --- | --- |
| 1 | tie-to-positive의 퇴화 교차, coplanar | 1.5 3상태 분류, 0 노드 직접 사용, `CoplanarFace` 거절, 1.6 경계 기반 루프 |
| 2 | 위치만으로 용접 | 1.3 스킨 동등성 포함, epsilon 용접 금지, `UnweldedSeam` |
| 3 | 교차 위치 비트 동일성 | 1.2 노드가 위치 소유, 3.2 에지 캐시에 최종 위치, 1.4 합산 순서 고정 |
| 4 | cap의 출처 3개 초과 | 1.4 표면 전용 명시, `PatchNodeKind::Cap`, `CrossesCap` 거절, `InvalidOrigin` |
| 5 | 최초 경계 고정 | 1.7 piece별 재계산 + 종류 계승 |
| 6 | cap 강체 부착 시 rim 균열 | 1.8 cap이 rim 노드 공유(`FollowRim` 기본), NDC 방식용 데이터 선저장 |
| 7 | 원본 삼각형 ID, piece별 분리 상태, 자손 본 | 2절 `BonePatchTriangle`, `skinStateId`, 2-0 추출 본 집합 결정 |
| 8 | 루프 유효성 | 3.4 자기/상호 교차, 포함 트리, 방향 일치 |
| 9 | 입력 검증·실패 계약 | 3.1, 3절 계약(out 초기화, `NoCut`, 떨어진 shell은 Ok) |
| 10 | 렌더러 section·PSO | 3.7 `DrawOptions`, 빈 section 금지 |
| 11 | 포즈 시점·오프셋·테스트 기대값 | 1.1 같은 스냅샷·같은 오프셋, 7절 기대값 수정과 케이스 추가 |

## 10. 열린 질문
- CesiumMan의 `UnweldedSeam` 개수(2-1에서 측정). 많다면 seam 처리를 거절 대신 지원으로 올려야 한다.
- 추출 본 집합: 선택 본만 쓸지, 자손까지 포함할지(2-0).
- `FollowRim` cap이 큰 굽힘에서 얼마나 찌그러지는지, `RigidToJoint`의 틈과 비교(4주차 실험).
- `OpenLoop`일 때 거절만 할지, 패치를 넓혀 다시 추출할지(계획 5.6, NDC 9:31~).

## 11. 구현 노트 (2-0 ~ 2-4 구현 후)

설계와 달라졌거나 설계에 없던 결정이다.

- **earcut 대신 직접 구현한 ear clipping** (`Geometry/SliceCap.cpp`): 외부 코드를 내려받지 않기로 했다. 구멍은 외곽에 브리지로 합치고, 직선 위 노드는 귀 꼭짓점이 될 수 없으므로 버려지지 않는다. 그래서 T-junction 보정 단계 대신 "모든 절단 에지가 cap 에지와 정확히 한 번씩, 반대 방향으로 짝지어지는지" 검증한다. 귀 판정은 `cross > 4 ε²`(면적 > 2 ε²)로, 입력 검증의 퇴화 기준을 넘는다.
- **Geometry/ 독립성**: `PatchWeld`는 BonePatch 대신 평면 입력 구조체(`WeldInput`)를 받고, 변환은 `BonePatch.cpp`의 `MakeWeldInput`이 맡는다. `PatchCap`의 행렬은 `XMFLOAT4X4`다.
- **원본 노드 스킨 보관**: `PatchPiece::originNodes`가 `PatchOrigin`이 가리키는 원본 노드의 바인드 위치·스킨 쌍을 가진다(2-5 스키닝용).
- **`PatchPiece::boundary`는 태그가 붙은 모든 에지**를 가진다. Replacement/UnweldedSeam/Cut은 열린 경계, CapSeam은 내부 에지다. 열린 에지에 태그가 없으면 입력 검증이 거절한다.
- **출력 piece도 `ValidatePatchPiece`를 통과해야 성공**이다(퇴화 직전 면이 잘려 문턱 아래로 내려가는 경우 `DegenerateInput`).
- **루프 추출은 `CapMode::None`에서도 실행**한다. None은 cap 면만 만들지 않는다. 루프에 쓰는 "평면 위 노드"는 슬라이서가 분류한 값을 그대로 넘긴다.
- **`SliceResult`에 `loopDepth`, `loopArea` 추가**(양의 쪽 piece 기준).
- **노드 용접**: -0과 +0은 같은 위치로 본다. UnweldedSeam 판정은 같은 슬롯의 바인드 위치 쌍, 또는 다른 슬롯의 포즈 위치 쌍이 겹치는 경계 에지다.
- **패치 추출은 선택 본만**(자손 제외). 분리 단위(본 + 자손)는 4주차. `mixedOutsideTriangles`로 양쪽에 걸친 삼각형 수를 보고한다.
- **ModelViewer 키**: T 슬라이스, [ ] 오프셋, , . 와 ; ' 기울기, R 초기화, G 경계 표시, M cap 켜기/끄기, L 후면 컬링, Z section tint.

## 12. 구현 노트 (2-5: piece별 매 프레임 스키닝, 상태 0)

- **스키닝 (`Geometry/Skinning.*`)**: `SkinPatchPiece`는 `PatchPiece`와 meshSlot별 행렬 배열(`XMFLOAT4X4`, `mesh world * joint.posXform`)만 받는다. 원본 노드는 `Σ weight · M_joint · bindPosition`(joint 오름차순), 잘린 노드는 `Σ bary · LBS(origin node)`(사용 칸만, node 오름차순)로 위치를 갱신한다. 본 ID와 웨이트는 보간하지 않는다. 원본 노드의 LBS는 호출마다 한 번만 계산해 재사용한다. `skinStateId != 0`, 범위 밖 joint·slot·origin은 실패하고 piece는 바뀌지 않는다.
- **cap 노드**: 현재 cap은 rim 노드를 공유하므로 cap 전용 노드는 없다(`SkinStats::capNodesSkipped == 0`을 테스트로 확인). Cap 종류 노드가 생기면 위치를 계산하지 않고 건너뛴다.
- **추출과 같은 공식**: `ExtractBonePatch`의 위치 계산도 `SkinPoint`와 `MakeSlotMatrices`를 쓴다. 그래서 추출 시점 포즈의 패치와 나중에 스키닝한 패치는 같은 식에서 나온다. `PatchSkin::meshSlot`은 `BonePatchSource::meshes`의 인덱스와 같은 값이다(`ExtractBonePatch`가 그 순서로 번호를 매김).
- **렌더 갱신**: 위상은 그대로 두고 `UpdateRenderPositions`가 corner → node 위치만 복사해 `ProceduralMesh::UpdateVertices`로 올린다. **법선은 갱신하지 않는다**(현재 PS는 법선을 쓰지 않음). 조명을 쓰게 되면 piece별 법선 재계산이 필요하다.
- **프레임 순서**: 입력 처리 → `ModelInstance::Update` → `UpdatePatchPose`(패치 스키닝, 필요하면 재슬라이스, piece 스키닝, 메시 갱신) → 렌더. 슬라이스는 이번 프레임 포즈로 스키닝된 패치에서 한다.
- **평면은 world 공간에 고정**: 절단 평면은 어떤 joint에도 묶이지 않는 world의 유한 사각형이다(정의, 조작, 루프 검사는 `PlaneFirstCutDesign.md` 2절). 이전에는 선택 본의 로컬 프레임에 고정하고 매 프레임 joint 행렬로 옮겼다.
- **경계 디버그 튜브**는 매 프레임 같은 위치를 따라가도록 다시 구워 정점만 갱신한다. 평면은 world에 있으므로 따라가지 않는다.
- **한계**: 두 piece를 벌리는 방향은 자른 순간 평면 법선(월드)으로 고정이다. 재슬라이스가 실패하면 직전 성공 piece가 계속 스키닝되어 화면에 남는다.
