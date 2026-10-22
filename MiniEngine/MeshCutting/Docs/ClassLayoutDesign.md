상태: 설계안 (참고 기록) — 채택한 ProceduralMesh 통합은 ProceduralMeshSimplification.md에서 구현. 아래 6클래스 스케치 전체를 적용했다는 뜻은 아니다 (`d418fcd` 이후 작업 트리).

# 클래스 단위 캡슐화 설계

> 작성: 2026-10-21 · 기준: `0fc39fe` + 작업 트리(Geometry 파일 통합: Patch/Slice/SliceCap/Skinning/CutPlane, Rendering/RenderBake)
>
> 목적: 기능을 클래스 6개로 묶어 가독성과 일관성을 높인다.
>
> 적용 진행: `ProceduralMeshSimplification.md`에서 작은 단계로 진행한다. 아래 통합 클래스 코드는 설계 스케치다.

## 0. 지금 코드의 기능 분포

| 기능 | 지금 위치 | 형태 |
| --- | --- | --- |
| GPU용 메시 그릇 (정점·인덱스·section, 버전, bounds) | `ProceduralMesh.h/.cpp` | class `ProceduralMesh` + struct `ProceduralMeshData` |
| 절단용 메시 (vertex·corner·face·edge tag·cap 기록) | `Geometry/Patch.h` | struct `PatchPiece` + 자유 함수 (Validate, Radius, Area, EdgeUsage) |
| 용접 (corner → vertex) | `Geometry/Patch.cpp` | 자유 함수 `BuildPatchPiece` + `WeldInput`, `WeldStats` |
| 스킨 정점 데이터 | `Geometry/Skinning.h`, `Geometry/Patch.h` | `SkinVertex`(K-2), `PatchOrigin`, `PatchPiece::skin`(K-2), `skinStateId` |
| 스키닝 계산 | `Geometry/Skinning.*` | class `Skinning` (`Apply`, `SkinPoint`, joint 영향 함수 4개) — K-2 |
| 스킨 행렬 (Model 의존) | `BonePatch.*` | `MakeSlotMatrices`, `BuildSkinMatrices`, `ComputePoseJointWorld`, `ComputePaletteSkeleton` |
| 패치 추출 (Model 의존) | `BonePatch.*` | `ExtractBonePatch`, `MakeWeldInput`, `BonePatchSource` |
| 절단 평면 (데이터·변환·포함 검사) | `Geometry/CutPlane.*` | struct `CutPlane`, `CutPlaneFrame` + 자유 함수 |
| 선분 선택 | `Geometry/CutPlane.*` | `PickSegments`, `CutJoints`, `SkeletonPose`, `SegmentHit` |
| 기즈모 | `Debug/PlaneGizmo.*` | 순수 계산 자유 함수 + class `PlaneGizmo` |
| 평면 분할 | `Geometry/Slice.*` | 자유 함수 `SlicePatch` |
| 루프·cap | `Geometry/SliceCap.cpp` | 자유 함수 `ExtractCutLoops`, `BuildCap` |
| 렌더 굽기 | `Rendering/RenderBake.*` | `BuildRenderMesh`, `UpdateRenderPositions` |
| 디버그 튜브 굽기 | `Rendering/RenderBake.*` | `AppendDebugSegment`, `BakeBoundaryDebug`, `BakeCutPlaneDebug`, `BakeLoopDebug`, `EdgeKindColor` |
| 렌더러 | `Rendering/ProceduralMeshRenderer.*` | class |
| 흐름 제어 | `Runtime/CutController.*` | class (PatchState, CandidateState 등) |
| 디버그 표시·카메라 | `Debug/CutDebugView.*`, `Debug/SceneViewCamera.*` | class |

문제: "메시"가 둘(`ProceduralMesh`=GPU 그릇, `PatchPiece`=절단용 위상)로 나뉘어 있고, 스킨 데이터는 메시 안에, 스킨 계산은 Skinning에, 스킨 행렬은 BonePatch에 흩어져 있다. 절단 평면의 데이터·기즈모·선분 선택도 세 곳에 있다.

## 1. 클래스 역할 (확정안)

```
                ┌──────────────┐
                │  PatchPiece  │  절단 단위 객체 = 메시 + 스킨 바인딩 + 출처
                └──┬────────┬──┘
          소유     │        │ 공유(shared_ptr<const>)
        ┌──────────▼───┐ ┌──▼─────────┐
        │ProceduralMesh│ │  Skinning  │  스킨 정점 표 + 포즈 행렬 → 메시 위치 갱신
        └──▲────────▲──┘ └────────────┘
   입력/출력│        │그리기
  ┌────────┴─────┐ ┌┴──────────────────────┐
  │ PlaneSlicer  │ │ ProceduralMeshRenderer│
  └──────▲───────┘ └───────────────────────┘
         │입력
  ┌──────┴───────┐
  │   CutPlane   │  평면 데이터·변환·기즈모·선분 선택
  └──────────────┘
```

### 1.1 ProceduralMesh — 런타임에 바뀌는 메시
지금의 `PatchPiece` 위상 데이터와 `ProceduralMesh`의 GPU 캐시를 합친다.

```cpp
class ProceduralMesh
{
public:
    // ---- 위상과 기하 (지금 PatchPiece의 필드) ----
    struct MeshVertex { /* 지금 PatchNode: position, kind, origin(PatchOrigin), cutId */ };
    struct MeshCorner { /* 지금 PatchCorner: vertex, normal, uv, color */ };
    struct MeshFace   { /* 지금 PatchFace: corner[3], kind, sourceTriangle, cutId */ };
    enum class MeshEdgeTag { /* 지금 PatchEdgeKind */ };
    struct MeshCutRecord { /* 지금 PatchCap: cutId, planePoint, planeNormal, rimVertices, loopSizes */ };

    const std::vector<MeshVertex>& GetVertices() const;
    const std::vector<MeshCorner>& GetCorners() const;
    const std::vector<MeshFace>& GetFaces() const;
    const MeshEdgeTagMap& GetEdgeTags() const;
    const std::vector<MeshCutRecord>& GetCutRecords() const;

    // 위상 전체 교체 (용접·절단 결과). 검증 실패 시 기존 상태 유지. topology 버전 증가
    bool SetTopology(std::vector<Vertex>, std::vector<Corner>, std::vector<Face>,
                     EdgeTagMap, std::vector<CutRecord>, float epsilonScale, std::string* detail);
    // 위치만 갱신 (스키닝). vertex 버전 증가
    void SetPositions(const std::vector<DirectX::XMFLOAT3>& positions);

    // ---- 질의 (지금 자유 함수) ----
    bool Empty() const;
    double Radius() const;                       // ComputePieceRadius
    double FaceArea(uint32_t face) const;
    double TotalArea(FaceKind kind) const;
    void CollectEdgeUsage(EdgeUsageMap& usage) const;
    SliceStatus Validate(float epsilonScale, std::string* detail) const;   // ValidatePatchPiece

    // ---- GPU 캐시 (지금 ProceduralMeshData + RenderBake) ----
    const ProceduralMeshData& RenderData() const;   // corner마다 정점 하나, section 0 표면 / 1 cap
    void SetBakeOptions(const RenderBakeOptions& options);   // splitByOrigin 등
    const ProceduralMeshBounds& Bounds() const;
    uint64_t VertexVersion() const;
    uint64_t TopologyVersion() const;

    // ---- 위상 없는 디버그 메시 (튜브, 기즈모) ----
    bool SetRenderDataOnly(ProceduralMeshData data, std::string* error);   // 지금 SetMesh

private:
    // 위상, GPU 캐시, 버전, bounds. 위상/위치를 바꾸는 길은 위 두 함수뿐이라
    // "고쳤는데 버전을 안 올림" 같은 실수가 생기지 않는다.
};
```

- **위상과 GPU 캐시를 한 객체가 가진다.** 위치가 바뀌면 GPU 캐시의 위치만, 위상이 바뀌면 전체를 다시 굽는다. 지금 `CutDebugView`가 하던 "PatchPiece → BuildRenderMesh → ProceduralMesh::SetMesh" 연결이 사라진다.
- **디버그 메시도 같은 클래스를 쓴다.** 튜브·평면·기즈모처럼 위상이 필요 없는 메시는 `SetRenderDataOnly`로 GPU 데이터만 넣는다. 이 경우 위상 질의는 빈 값을 돌려준다.
- **origin(스킨 출처)은 정점에 남긴다.** 절단할 때 새 정점의 origin을 섞어야 하므로, 슬라이서가 메시만 보고도 갱신할 수 있어야 한다. origin이 가리키는 스킨 정점 표는 Skinning이 가진다.
- 필드는 private이다. 지금 슬라이서·용접기가 piece의 vector를 직접 채우던 부분은 "로컬에서 만든 뒤 `SetTopology`로 한 번에 넘기는" 방식이 된다(트랜잭션 계약과도 맞는다).

### 1.2 Skinning — 스킨 정보와 계산
```cpp
class Skinning
{
public:
    struct SkinVertex { /* 지금 PatchSkin: meshSlot, bindPosition, jointCount, joints, weights */ };

    explicit Skinning(std::vector<SkinVertex> vertices);
    const std::vector<SkinVertex>& GetVertices() const;

    // K-2 구현: SetPose는 두지 않는다. Skinning은 여러 piece가 shared_ptr<const>로 공유하므로 상태를 바꿀 수 없고,
    // 포즈 행렬(meshSlot별 메시 world × joint skin)은 Apply의 인자로 받는다.
    // K-3 전까지 Apply는 PatchPiece를 받는다. K-3 뒤에는 ProceduralMesh를 받아 SetPositions로 반영한다.
    bool Apply(PatchPiece& piece, const std::vector<std::vector<DirectX::XMFLOAT4X4>>& slotMatrices,
        SkinStats* stats, std::string* error) const;                                 // SkinPatchPiece
    static DirectX::XMVECTOR SkinPoint(...);                                         // SkinPoint

    // joint 영향 (지금 JointSet 함수들)
    static float Influence(const uint16_t joints[4], const float weights[4], uint32_t joint);
    static float SetInfluence(...);
    static bool InJointSet(...);
    static bool IsValidJointSet(...);

    // ---- Model 연결 (Skinning_Model.cpp: Model/Core 의존, 단독 테스트 빌드에서 제외) ----
    static void MakeSlotMatrices(...);
    static bool BuildSlotMatrices(const Model&, const ModelInstance&, const PatchSource&, ...);   // BuildSkinMatrices
    static bool ComputePoseJointWorld(...);
    static bool ComputePaletteSkeleton(..., SkeletonPose& out, ...);
};
```
- 한 패치에서 잘려 나온 piece들은 같은 스킨 정점 표를 쓴다. 지금은 piece마다 `originNodes`를 복사하는데, `std::shared_ptr<const Skinning>`로 공유한다.
- `skinStateId`(4주차 분리 상태)는 PatchPiece가 갖는다. 분리 상태는 "어느 piece인가"에 따라 다르기 때문이다.

### 1.3 CutPlane — 절단 평면과 기즈모
```cpp
class CutPlane
{
public:
    struct Frame { /* 지금 CutPlaneFrame: center, axisU, axisV, normal, halfExtents */
        SlicePlane ToInfinite() const;                 // ToSlicePlane
        bool Contains(p, margin) const;                // ContainsInPlane
    };

    // 데이터와 변환 (지금 CutPlane 필드 + 자유 함수)
    Frame GetFrame() const;                            // MakeFrame
    void Translate(...); void RotateAroundLocalAxis(...);
    void ScaleUniform(...); void ScaleU(...); void ClampExtents(...);

    // 선분 선택 (지금 PickSegments, CutJoints)
    void PickSegments(const SkeletonPose& pose, std::vector<SegmentHit>& hits) const;
    static std::vector<uint32_t> CutJoints(const std::vector<SegmentHit>& hits);

    // 기즈모 (지금 PlaneGizmo 클래스와 순수 계산 함수)
    void SetGizmoTool(GizmoTool tool);
    bool UpdateGizmo(const GizmoView& view, const GizmoInput& input);   // 드래그 중이면 true, 평면을 직접 바꾼다
    void BakeGizmo(const GizmoView& view, ProceduralMeshData& out) const;

private:
    DirectX::XMFLOAT3 m_Center; DirectX::XMFLOAT4 m_Rotation; DirectX::XMFLOAT2 m_HalfExtents;
    float m_MinExtent, m_MaxExtent;      // 지금 CutController가 매번 넘기던 제한
    GizmoState m_Gizmo;                  // 지금 PlaneGizmo의 tool, hover, drag
};
```
- 기즈모의 순수 계산(광선, 투영, 피킹, 드래그 수식)은 `CutPlaneGizmo.cpp`에 두고 CutPlane의 private 정적 함수로 만든다. 기존 기즈모 테스트는 `CutPlane` 공개 API(`UpdateGizmo`)와, 테스트용으로 남길 계산 함수(`MakeRay`, `PickHandle` 등)로 옮긴다.
- 평면 크기 제한(min/max extent)을 CutPlane이 갖는다. 지금은 CutController가 `ModelRadius × 0.01~3`을 매번 계산해 넘긴다.

### 1.4 PlaneSlicer — 절단과 단면 생성
```cpp
class PlaneSlicer
{
public:
    struct Options { /* 지금 SliceOptions */ };
    struct Result
    {
        SliceStatus status;
        ProceduralMesh positive, negative;    // 성공할 때만
        std::vector<Loop> loops;              // 지금 loops, loopDepth, loopArea를 하나로
        std::string detail;
    };

    // 무한 평면으로 자르고, 루프가 사각형(CutPlane 크기) 밖으로 나가면 LoopOutsidePlane.
    // 지금 CutController가 하던 LoopsInsidePlane 검사를 슬라이서 안으로 옮긴다.
    static SliceStatus Slice(const ProceduralMesh& in, const CutPlane& plane, uint32_t cutId,
                             const Options& options, Result& out);
    // PatchPiece용: 메시를 자르고 스킨 공유·출처·분리 상태를 두 piece에 넘긴다
    static SliceStatus Slice(const PatchPiece& in, const CutPlane& plane, uint32_t cutId,
                             const Options& options, PatchPiece& positive, PatchPiece& negative,
                             std::vector<Loop>& loops, std::string* detail);

private:
    // 단면 (지금 SliceCap.cpp의 ExtractCutLoops, BuildCap)
    static SliceStatus ExtractLoops(...);
    static SliceStatus BuildCap(...);
};
```
- 사용자 의도대로 입력은 **CutPlane + ProceduralMesh**다. 평면 크기 검사가 슬라이서 안으로 들어오면서 `LoopOutsidePlane`이 "CutController만 쓰는 상태"에서 슬라이서의 정식 결과가 된다.
- 테스트가 `ExtractLoops`/`BuildCap`을 직접 부르면 private으로 둘 수 없다. 그 경우 `PlaneSlicer::Detail` 같은 테스트용 공개 구역을 두거나 public으로 남긴다(구현 때 사용처 확인).

### 1.5 PatchPiece — 절단 단위 객체 (정의)
"??"였던 PatchPiece는 **"캐릭터에서 떼어 낸, 자를 수 있고 움직이는 조각 하나"**로 정의한다. 메시만으로는 부족하고 아래 셋이 함께 있어야 의미가 있기 때문이다.

```cpp
class PatchPiece
{
public:
    // 생성: 원본 캐릭터에서 추출해 용접한다 (지금 ExtractBonePatch + MakeWeldInput + BuildPatchPiece)
    // PatchPiece_Model.cpp: Model/Core 의존
    static bool Extract(const Model&, const ModelInstance&, uint32_t paletteId,
                        const std::vector<uint32_t>& joints, float minWeight,
                        PatchPiece& out, WeldStats& stats, std::string* detail);
    // 테스트·순수 경로: 이미 평평하게 만든 입력에서 용접 (지금 BuildPatchPiece)
    static SliceStatus Weld(const WeldInput& in, float epsilonScale, PatchPiece& out,
                            WeldStats& stats, std::string* detail);

    ProceduralMesh& Mesh();  const ProceduralMesh& Mesh() const;
    const Skinning& Skin() const;
    const PatchSource& Source() const;     // 지금 BonePatchSource: 팔레트, joint 집합, 원본 메시·삼각형, poseJointWorld, mixedOutsideTriangles
    uint32_t SkinStateId() const;          // 4주차 분리 상태

    // 매 프레임: 포즈 행렬을 받아 스키닝하고 메시 위치를 갱신
    bool UpdatePose(const Model&, const ModelInstance&, std::string* detail);   // PatchPiece_Model.cpp

private:
    ProceduralMesh m_Mesh;
    std::shared_ptr<const Skinning> m_Skin;   // 같은 패치에서 나온 piece끼리 공유
    std::shared_ptr<const PatchSource> m_Source;
    uint32_t m_SkinStateId = 0;
};
```
- **지금 `BonePatch.*`를 흡수한다.** 추출(`ExtractBonePatch`), 출처 데이터(`BonePatchSource` → `PatchSource`), 용접 입력 변환(`MakeWeldInput`)이 PatchPiece의 생성 과정이 된다.
- **4주차 분리**가 들어갈 자리가 생긴다. 분리 상태(`SkinStateId`)와 "이 조각은 떨어진 쪽인가"는 메시가 아니라 조각의 속성이다.

### 1.6 ProceduralMeshRenderer — 그리기
지금 클래스 그대로 두고, 입력을 `ProceduralMesh`의 `RenderData()`/버전/bounds로 바꾼다. 하는 일은 같다(버전이 바뀐 버퍼만 업로드, fence로 버퍼 반납, section별 그리기, 컬링·와이어·tint 옵션).

## 2. 6개 클래스 밖에 남는 것

| 대상 | 처리 | 이유 |
| --- | --- | --- |
| `Runtime/CutController` | **남긴다** | 앱 흐름 제어(편집 모드, 후보 갱신, 미리보기, 확정, 되돌리기). 6개 클래스를 조합하는 쪽이다. 크기는 PatchPiece/CutPlane/PlaneSlicer로 일이 옮겨 가며 줄어든다 |
| `Debug/CutDebugView` | **남긴다** | 화면에 무엇을 그릴지 고르는 쪽. "PatchPiece → 렌더 메시 굽기"는 ProceduralMesh 안으로 들어가고, 대신 **디버그 튜브 굽기 전부**(튜브 한 줄, 경계 태그, 평면 테두리, 예상 루프, `EdgeTagColor`)를 맡는다. 6개 클래스의 헤더에 디버그 표시 함수가 섞이지 않게 하기 위해서다 |
| `Debug/SceneViewCamera` | **남긴다** | 카메라는 절단 기능과 무관 |
| `Debug/PlaneGizmo` | **CutPlane으로 합친다** | 사용자가 정한 "CutPlane = 평면 + 기즈모" |
| `BonePatch` | **PatchPiece로 합친다** | 1.5 |
| `Geometry/Patch` | **ProceduralMesh(위상)와 PatchPiece(용접)로 나눈다** | 1.1, 1.5 |
| `Rendering/RenderBake` | **렌더 굽기(`BuildRenderMesh`, `UpdateRenderPositions`)는 ProceduralMesh 내부(private)로, 디버그 굽기는 CutDebugView로** | 클래스 헤더를 짧게 유지한다 (6절) |
| `SliceStatus`, `SlicePlane`, `SkeletonPose`, `SegmentHit`, `GizmoView` 등 작은 타입 | 가장 가까운 클래스의 헤더에 둔다(필요하면 중첩 타입) | |

## 3. 파일 배치

```
MeshCutting/
  ProceduralMesh.h/.cpp          # 위상 + GPU 캐시 + 굽기·튜브
  Skinning.h/.cpp                # 스킨 정점, LBS, joint 영향
  Skinning_Model.cpp             # Model 연결 (행렬, 포즈 joint, 팔레트 스켈레톤)
  CutPlane.h/.cpp                # 데이터·변환·선분 선택·평면 표시
  CutPlaneGizmo.cpp              # 기즈모 계산·상호작용·굽기
  PlaneSlicer.h/.cpp             # 분류·분할·압축
  PlaneSlicerCap.cpp             # 루프·cap
  PatchPiece.h/.cpp              # 조각, 용접
  PatchPiece_Model.cpp           # 추출, 매 프레임 포즈 (Model 의존)
  Rendering/ProceduralMeshRenderer.h/.cpp, *.hlsl
  Runtime/CutController.h/.cpp
  Debug/CutDebugView.h/.cpp, SceneViewCamera.h/.cpp
  tests/...
```
- `Geometry/` 폴더는 없앤다. 대신 **"_Model.cpp로 끝나는 파일만 Model/Core에 의존한다"**는 규칙으로 바꾼다. 단독 테스트 빌드는 `_Model.cpp`를 빼고 컴파일한다.
- 헤더는 Model/Core 타입을 전방 선언만 한다(`class Model; class ModelInstance;`). `Math::Matrix4`가 필요한 `PatchSource::poseJointWorld`는 `XMFLOAT4X4`로 바꾸거나 PatchPiece_Model 쪽 데이터로 옮긴다.

## 4. 진행 순서

전부 동작이 같아야 한다(구조만 바뀜). 각 단계 끝에 빌드와 기존 테스트 통과.

| 단계 | 내용 | 위험 |
| --- | --- | --- |
| K-0 | 지금 작업 트리(Geometry 통합)를 먼저 커밋 | 없음 |
| K-1 | **CutPlane**: 평면 자유 함수·선분 선택을 멤버로, PlaneGizmo 합치기, 평면 표시 굽기 이동, min/max extent 소유 | 낮음 (독립적) |
| K-2 | **Skinning**: 스킨 정점 표 분리(`shared_ptr` 공유), SkinPatchPiece·JointSet·BonePatch 행렬 함수 이동 | 중간 (piece 복사 경로 변경) |
| K-3 | **ProceduralMesh**: PatchPiece 위상 필드 + RenderBake 굽기를 흡수, 필드 private, `SetTopology`/`SetPositions` | **높음** (사용처 가장 많음: 슬라이서, 용접기, 디버그 뷰, 렌더러, 테스트) |
| K-4 | **PlaneSlicer**: `Slice(ProceduralMesh, CutPlane)`, 루프 사각형 검사 이동, 루프·cap private | 중간 |
| K-5 | **PatchPiece**: BonePatch 흡수, 메시·스킨·출처·분리 상태 보유, `Extract`/`Weld`/`UpdatePose` | 중간 |
| K-6 | CutController·CutDebugView 정리, 문서(CodeMap 등) 갱신 | 낮음 |

K-3이 가장 크다. 테스트(2,463줄)가 PatchPiece 필드를 직접 만들어 입력으로 쓰는 곳이 많아서, 테스트용 생성 보조 함수(`MakeTestMesh(...)` 등)를 먼저 만들어 두면 수정 범위가 줄어든다.

## 6. 헤더 형식: 지금 `ProceduralMesh.h`를 기준으로

지금 `ProceduralMesh.h`(61줄)가 잘 읽히는 이유는 **"이 클래스가 쓰는 구조체 정의 → 클래스 선언(공개 함수 몇 개 → private 데이터)"** 순서로, 그 파일 하나만 보면 클래스를 다 이해할 수 있기 때문이다. 6개 클래스 헤더를 모두 이 형식으로 맞춘다.

1. **한 헤더 = 클래스 하나.** 그 클래스가 쓰는 구조체·enum만 위에 둔다. 다른 클래스의 타입은 include하거나 전방 선언한다.
2. **순서:** include → namespace → 구조체/enum(쓰이는 순서) → class(public 함수 → private 데이터).
3. **public 함수는 한 줄 선언 + 필요하면 한 줄 주석.** 긴 설명, 알고리즘 설명, 설계 근거는 .cpp나 Docs로 보낸다.
4. **자유 함수를 두지 않는다.** 모든 기능은 클래스의 멤버나 static 멤버다.
5. **디버그 표시용 함수를 두지 않는다.** 디버그 굽기는 CutDebugView가 맡는다.
6. **private 보조 함수는 헤더에 꼭 필요한 것만.** 구현 세부(작업용 캐시, 람다로 쓰던 보조 함수)는 .cpp의 익명 namespace에 둔다.
7. **분량 목표:** 헤더 60~150줄. 넘으면 그 클래스가 너무 많은 일을 하는 신호로 보고 다시 나눈다.
8. **주석은 모두 한국어, 한 줄로.** 각 K 단계에서 손대는 파일(헤더와 .cpp)은 기존 영어 주석도 한국어로 다시 쓴다.
   - 함수·변수·구조체마다 **한 줄**로, 그것이 **무슨 역할인지만** 직관적으로 쓴다. 예: `// 평면을 world 방향으로 이동한다`, `// 절단으로 새로 생긴 정점이면 0이 아니다`
   - 알고리즘 설명, 설계 근거, 이력, 리뷰 대응, 예외 상황 나열은 주석에 쓰지 않는다. 꼭 남겨야 하는 내용은 Docs로 옮긴다.
   - 이름만으로 역할이 분명한 것(`GetVertices()`, `m_Center`)에는 주석을 달지 않아도 된다.
   - 여러 줄짜리 기존 주석은 핵심 한 줄로 줄인다. 줄이면서 빠지는 정보가 동작 계약(실패 시 상태 유지 등)이면 그 한 줄에 담는다.
   - 7.2절 용어집의 단어를 쓴다. 코드 식별자, 상태 이름(`OpenLoop` 등), 파일 경로는 영어 그대로 둔다.
   - Model/Core 등 이 프로젝트 밖 MiniEngine 파일의 주석은 건드리지 않는다.

예: 합쳐진 ProceduralMesh 헤더의 모양

```cpp
#pragma once
#include "SliceTypes.h"     // SliceStatus 등 여러 클래스가 함께 쓰는 작은 타입만 모은 헤더
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace MeshCutting
{
    enum class MeshVertexKind : uint8_t { Surface, Cap };

    // 스킨 출처: Skinning 정점 최대 3개의 무게중심 조합
    struct MeshVertexOrigin { uint32_t skinVertex[3]; float bary[3]; };

    struct MeshVertex { DirectX::XMFLOAT3 position; MeshVertexKind kind; MeshVertexOrigin origin; uint32_t cutId; };
    struct MeshCorner { uint32_t vertex; DirectX::XMFLOAT3 normal; DirectX::XMFLOAT2 uv; DirectX::XMFLOAT4 color; };
    struct MeshFace { uint32_t corner[3]; MeshFaceKind kind; uint32_t sourceTriangle; uint32_t cutId; };
    // ... EdgeTag, CutRecord, 지금의 ProceduralVertex / MeshSection / ProceduralMeshData / ProceduralMeshBounds

    class ProceduralMesh
    {
    public:
        bool SetTopology(...);                  // 용접·절단 결과로 전체 교체
        void SetPositions(...);                 // 스키닝 결과로 위치만 갱신
        bool SetRenderDataOnly(...);            // 위상 없는 디버그 메시
        void Clear();

        const std::vector<MeshVertex>& GetVertices() const { return m_Vertices; }
        // ... GetCorners, GetFaces, GetEdgeTags, GetCutRecords
        double GetRadius() const;
        SliceStatus Validate(float epsilonScale, std::string* error = nullptr) const;

        const MeshRenderData& GetRenderData() const { return m_RenderData; }
        const MeshBounds& GetBounds() const { return m_Bounds; }
        uint64_t GetVertexVersion() const { return m_VertexVersion; }
        uint64_t GetTopologyVersion() const { return m_TopologyVersion; }

    private:
        std::vector<MeshVertex> m_Vertices;
        // ...
        MeshRenderData m_RenderData;
        MeshBounds m_Bounds;
        uint64_t m_VertexVersion = 0;
        uint64_t m_TopologyVersion = 0;
    };
}
```

- 여러 클래스가 함께 쓰는 작은 타입(`SliceStatus`, `SlicePlane`, 에지 키 함수, `kInvalidIndex`)은 `SliceTypes.h` 하나에 모은다. 이 파일만 예외적으로 클래스 없이 타입만 둔다.
- 이름은 7.2절 규칙을 따른다.

## 7. 통합하면서 줄일 중복과 이름 규칙

### 7.1 중복 (지금 코드에서 확인한 것)

| 중복 | 지금 | 통합 |
| --- | --- | --- |
| 오류 문자열 보조 `Fail(error, message)` | 6개 cpp에 각자 정의 (BonePatch, Patch, Skinning, Slice, SliceCap, ProceduralMesh) | `SliceTypes.h`에 하나 |
| 유한값 검사 `Finite(...)` | 같은 이름 6개 정의 | `SliceTypes.h`에 하나 |
| 3D 벡터 보조 (`Dot`, `Cross`, `Length`, `Normalize`, `Normalize3`, double[3] 연산) | PlaneGizmo.cpp, Slice.cpp, Patch.cpp(평면 기저), SliceCap.cpp 각자 | 한 곳(`SliceTypes.h`의 작은 double 벡터 보조)으로. float 계산은 DirectXMath를 쓴다 |
| 2D 점-다각형 포함 `PointInPolygon` | SliceCap.cpp 안에 입력 타입만 다른 두 개 | 하나로 (템플릿 또는 점 배열 입력) |
| bounds·반지름 계산 | `ProceduralMesh.cpp`(GPU 정점 기준)와 `ComputePieceRadius`(vertex 기준), 같은 식 | ProceduralMesh 하나의 계산 (`GetBounds`/`GetRadius`) |
| 렌더 굽기 연결 | CutDebugView가 PatchPiece → `BuildRenderMesh` → `SetMesh`, 매 프레임 `UpdateRenderPositions` → `UpdateVertices` 복사 | ProceduralMesh 내부 (`SetTopology`/`SetPositions`가 GPU 캐시까지 갱신) |
| 평면 표현 3종 | `SlicePlane`(점·법선), `SlicePlaneBasis`(double 기저), `CutPlaneFrame`(float 기저·크기) | 공개는 `CutPlane`과 `CutPlane::Frame` 둘. 무한 평면이 필요한 테스트는 `CutPlane::MakeInfinite(point, normal)`(아주 큰 크기)를 쓴다. double 기저는 PlaneSlicer 내부로 |
| 평면 크기 제한 | CutController가 `ModelRadius × 0.01/3`을 계산해 매번 넘김 | CutPlane이 min/max extent를 가진다 |
| 검증 | `ValidatePlane`, `ValidatePatchPiece`, ProceduralMesh의 입력 검증(유한값, 인덱스 범위)이 따로 | 메시 검증은 ProceduralMesh 하나(`Validate`), 평면 검증은 CutPlane 하나 |
| 추출 진입점 | `ExtractBonePatch` 오버로드 2개(단일 joint / 집합) | `PatchPiece::Extract(joints)` 하나. 단일 joint는 크기 1 집합 |
| 스킨 데이터 복사 | piece마다 `originNodes` 전체 복사 | `shared_ptr<const Skinning>` 공유 |

### 7.2 이름 규칙

**타입 이름: 소속 클래스를 접두사로**

| 소속 | 접두사 | 예 (지금 → 새 이름) |
| --- | --- | --- |
| ProceduralMesh | `Mesh` | `PatchNode` → `MeshVertex`, `PatchCorner` → `MeshCorner`, `PatchFace` → `MeshFace`, `PatchEdgeKind` → `MeshEdgeTag`, `PatchCap` → `MeshCutRecord`, `PatchOrigin` → `MeshVertexOrigin`, `ProceduralVertex` → `MeshRenderVertex`, `ProceduralMeshData` → `MeshRenderData`, `MeshSection` → 그대로, `ProceduralMeshBounds` → `MeshBounds` |
| Skinning | `Skin` | `PatchSkin` → `SkinVertex`, `SkinStats` → 그대로 |
| CutPlane | `CutPlane::` 중첩 또는 `Gizmo` | `CutPlaneFrame` → `CutPlane::Frame`, `GizmoTool`/`GizmoHandle`/`GizmoView`/`GizmoInput` → 그대로, `SegmentHit`/`SkeletonPose` → 그대로 |
| PlaneSlicer | `Slice` | `SliceOptions`, `SliceResult`, `SliceLoop` → 그대로, `CapMode` → `SliceCapMode` |
| PatchPiece | `Patch` | `BonePatchSource` → `PatchSource`, `BonePatchMesh` → `PatchSourceMesh`, `BonePatchVertex` → `PatchSourceVertex`, `BonePatchTriangle` → `PatchSourceTriangle`, `WeldInput*`/`WeldStats` → `PatchWeldInput*`/`PatchWeldStats` |
| 공통 (`SliceTypes.h`) | 없음 | `SliceStatus`, `kNoNode` → `kInvalidIndex` |

**용어집: 한 단어는 한 뜻만 가진다**

이 표가 타입, 멤버, 함수, 지역 변수, 주석, 테스트 이름 모두에 적용된다. 같은 대상을 다른 단어로 부르거나, 한 단어를 두 대상에 쓰지 않는다.

| 단어 | 뜻 (이것만) | 이 뜻으로 쓰면 안 되는 말 | 이 단어를 다른 뜻으로 쓰면 안 되는 예 |
| --- | --- | --- | --- |
| **vertex** | 절단 메시의 기하 점 (`MeshVertex`, 위치를 가짐) | node, point(데이터 구조로서), cut node | 렌더 정점을 vertex라고 부르지 않는다 → corner 또는 render vertex |
| **corner** | 면의 한 꼭짓점에 붙는 렌더 속성 (`MeshCorner`: vertex 번호 + normal·uv·color) | render vertex(CPU 쪽), wedge | 에지나 정점 쌍을 corner라고 부르지 않는다 |
| **render vertex** | GPU 버퍼에 올라가는 정점 (`MeshRenderVertex`, corner 하나 = render vertex 하나) | procedural vertex | |
| **face** | 삼각형 (`MeshFace`: corner 3개) | triangle(데이터 구조로서), primitive | 원본 모델의 삼각형은 source triangle |
| **edge** | 두 **vertex**를 잇는 무방향 선 (`MakeEdgeKey(vertexA, vertexB)`) | geometric edge, line | **corner 두 개의 쌍은 edge가 아니다** → `CornerPair` |
| **corner pair** | 같은 edge 위의 corner 두 개 (UV seam마다 따로 보간하려고 쓰는 키) | render edge, corner edge | |
| **edge tag** | edge의 종류 표시 (Replacement, UnweldedSeam, Cut, CapSeam) | edge kind, boundary kind | |
| **boundary edge** | 면 하나만 쓰는 edge | open edge | |
| **cut vertex** | 절단으로 edge 위에 새로 생긴 vertex | cut node | |
| **loop** | cut edge들이 이은 닫힌 vertex 사슬 | cycle, ring(데이터로서) | |
| **cap** | 단면을 채우는 면들 | cross-section | |
| **skin vertex** | 용접 뒤 원본 정점의 스킨 데이터 (`SkinVertex`: bind position + joint·weight) | skin node, origin node | |
| **origin** | 절단 메시 vertex가 skin vertex 최대 3개의 무게중심으로 어디서 왔는가 (`MeshVertexOrigin`) | source | |
| **source** | 원본 모델 쪽 데이터 (source mesh, source triangle, source vertex 번호) | origin | |
| **joint** | 스켈레톤의 transform 하나 (Unreal의 bone) | bone | |
| **segment** | 부모 joint와 자식 joint를 잇는 뼈 선분 (`SegmentHit`) | bone, edge | edge라고 부르지 않는다 |
| **node** | **Model scene graph 노드만** (`m_NodeParents`, `GetNodeWorldMatrices`, `jointNode`) | — | 절단 메시에서는 쓰지 않는다 |
| **tube** | 디버그용 가는 원통 메시 | segment(디버그 그리기로서), line | |

**지금 코드에서 바꿀 이름 (Model scene graph 이름은 그대로)**

| 지금 | 바꿀 이름 |
| --- | --- |
| `PatchNode`, `PatchNodeKind`, `nodes`, `node`(절단 메시 vertex 뜻), `nodeCount`, `nodeA/B`, `nodeOf`, `badNode`, `openNode`, `oneNode` | `MeshVertex`, `MeshVertexKind`, `vertices`, `vertex`, `vertexCount`, `vertexA/B`, `vertexOf`, `badVertex`, `openVertex`, `oneVertex` |
| `PatchCorner::node` | `MeshCorner::vertex` |
| `PatchOrigin::node[3]` | `MeshVertexOrigin::skinVertex[3]` |
| `PatchSkin`, `originNodes`, `badOriginNode` | `SkinVertex`, `Skinning`의 `m_Vertices`(`GetVertices()`), `badSkinVertex` |
| `kNoNode` | `kInvalidIndex` |
| `CutNodeInfo`, `m_CutNodes`, `CutNodeOf`, `cutNode`, `IsCutNode`, `CountCutNodes` | `CutVertexInfo`, `m_CutVertices`, `CutVertexOf`, `cutVertex`, `IsCutVertex`, `CountCutVertices` |
| `m_EdgeNode` | `m_EdgeCutVertex` |
| `CutCorner(side, cornerA, cornerB, ...)` 의 캐시 키 | 이름에 corner pair를 드러낸다: 키 타입 `CornerPair`, 캐시 `m_CornerPairCut` |
| `nodeOfVertex`(용접: 입력 렌더 정점 → 기하 점) | `vertexOfCorner` (용접 입력의 렌더 정점은 corner다) |
| `PatchCap::rimNodes`, `SliceLoop::nodes` | `MeshCutRecord::rimVertices`, `SliceLoop::vertices` |
| `ClassifyNodes` | `ClassifyVertices` |
| `SkinStats::originNodesEvaluated`, `surfaceNodesWritten`, `capNodesSkipped` | `skinVerticesEvaluated`, `surfaceVerticesWritten`, `capVerticesSkipped` |
| `WeldStats::nodes`, `positionNodes`, `positionNodeCount` | `vertices`, `positionVertices`, `positionVertexCount` |
| `m_SkinnedNodes`, `skinnedNodes`, `SkinnedNodes()` | `m_SkinnedVertices`, `skinnedVertices`, `GetSkinnedVertexCount()` |
| `PatchEdgeKind`, `EdgeKindName`, `EdgeKindColor`, `boundary`(map) | `MeshEdgeTag`, `EdgeTagName`, `EdgeTagColor`, `edgeTags` |
| `CountOpenEdges`, `TagOpenEdges` | `CountBoundaryEdges`, `TagBoundaryEdges` |
| 테스트 이름의 Node (`TestSkinMatchesSliceNodes`, `TestSharedNodeIsNonManifoldLoop`, `TestPlaneThroughNodes`, `TestOneNodeOnPlane`, `TestBakeSharesNodePositions`) | Node → Vertex |
| 그대로 둠 | `nodeWorld`, `jointNode`, `nodeToJoint`, `m_NumNodes`, `m_NodeParents`, `m_NodeNames`, `GetNodeWorldMatrices`, `nodeIndex`(Model 노드 번호인 곳) — Model scene graph |

이름 바꾸기는 각 K 단계에서 그 클래스로 옮기는 코드에 함께 적용한다. 단계가 끝날 때 해당 파일들에서 위 표의 "지금" 이름이 남아 있지 않은지 grep으로 확인한다.

**함수 이름**

| 규칙 | 예 |
| --- | --- |
| 접근자는 `Get` 접두사 (지금 ProceduralMesh.h, MiniEngine 스타일과 같게) | `GetVertices()`, `GetBounds()`, `GetFrame()` |
| 전체를 새로 만들면 `Build`, 바꿔 끼우면 `Set`, 매 프레임 갱신은 `Update` | `PatchPiece::Build...`, `ProceduralMesh::SetTopology`, `PatchPiece::UpdatePose` |
| 계산해서 돌려주는 값은 `Compute` 대신 `Get` (캐시) 또는 동사 (`Pick`, `Slice`, `Skin`) | `GetRadius()`, `CutPlane::PickSegments`, `PlaneSlicer::Slice` |
| 실패할 수 있는 함수의 마지막 인자는 `std::string* error = nullptr` 하나로 (지금 `error`/`detail` 혼용) | |
| 결과 상태가 여러 가지면 `SliceStatus`, 성공/실패뿐이면 `bool` | |
| 멤버는 `m_`, 전역은 `g_` (AGENTS.md) | |

## 8. 결정 사항 (2026-10-21)

> 1~4절의 코드 스케치에 쓴 이름(예: `RenderData()`, `Vertices()`, `PatchSource`의 필드명)은 설명용이다. 실제 이름은 6절(헤더 형식)과 7.2절(이름 규칙)을 따른다.

1. **ProceduralMesh 필드는 private.** 위상은 `SetTopology`, 위치는 `SetPositions`로만 바꾼다(버전 갱신 누락 방지). 슬라이서와 용접기는 로컬 배열을 만들어 한 번에 넘긴다.
2. **디버그 튜브 메시도 ProceduralMesh 하나로.** 위상 없는 메시는 `SetRenderDataOnly`로 받는다.
3. **스킨 정점 표는 `shared_ptr<const Skinning>`으로 공유.**
4. **Vertex/Edge/Face/Mesh 기반 설계(`MeshDataStructureDesign.md`, `MeshTypes.h`)는 쓰지 않는다.** 문서는 삭제했다. 기본 자료구조는 이 문서의 ProceduralMesh 구조체(`MeshVertex`, `MeshCorner`, `MeshFace`)가 대신한다.
