# 디버그 기능 설계: 패치 와이어프레임 · 수동 절단

> 작성: 2026-10-20 · 대상 커밋: `11c4be6` (Procedural Mesh 스키닝 애니메이션)
>
> 상태: **설계안**. 아래 코드는 아직 없다.

## 0. 현재 동작과 문제

| 항목 | 현재 (`ModelViewer.cpp`) | 문제 |
| --- | --- | --- |
| 패치 표시 | `ProceduralMeshRenderer` PSO 2개(양면 / 후면 컬링), 무조명 단색 | 분할된 삼각형과 cap 삼각분할이 보이지 않는다. 원본 모델의 V 와이어프레임은 `MeshSorter` 경로라 패치에 적용되지 않는다 |
| 절단 시점 | T로 슬라이스 모드를 켜면, `[ ] , . ; '`로 평면을 움직이는 동안 매 프레임 `m_SliceDirty` → `UpdateSlice` | 평면을 맞추는 중간 상태가 계속 잘리고, 결과가 계속 바뀐다. 원하는 위치에서 확정하는 동작이 없다 |
| 상태 | 슬라이스 관련 멤버 약 25개가 `ModelViewer`에 직접 있다 | 두 기능을 더하면 플래그가 더 섞인다 |

## 1. 구조 개요

```
ModelViewer (키 입력 → 호출, 렌더 호출)
  ├─ CutController          # 상태와 절단 수행 (MeshCutting/Runtime/)
  │    ├─ 패치 원본 PatchPiece
  │    ├─ 평면 편집 상태 (world 공간의 유한 사각 평면. `PlaneFirstCutDesign.md` 2절)
  │    ├─ 미리보기 결과 (dry-run, 적용하지 않음)
  │    └─ 확정 결과 (두 piece)
  └─ CutDebugView           # 표시 옵션과 디버그 메시 (MeshCutting/Debug/)
       ├─ 와이어프레임 모드, section tint, 경계, overlay
       └─ 미리보기 루프 튜브, 평면 표시
ProceduralMeshRenderer     # FillMode 추가, 와이어 색
RenderBake                 # 분할 면 section 분리(디버그), 루프 튜브
PlaneSlicer                # 분할된 표면 면에 cutId 기록
```

`ModelViewer`의 슬라이스 멤버와 `UpdatePatchPose`/`UpdateSlice`/`ComputeBasePlane`/`UpdateBasePlane`/`CurrentPlane`/`ClearSlice`를 `CutController`로 옮긴다. `ModelViewer`에는 키 매핑과 `Draw` 호출만 남긴다. 계획서 8절의 `Runtime/CutController`에 해당한다. 동작은 바꾸지 않는 순수 이동을 **먼저 별도 단계로** 하고, 기능은 그 위에 얹는다(3절 단계 D-0).

## 2. 기능 A: 패치 와이어프레임

### 2.1 렌더러
```cpp
enum class FillMode { Solid, Wireframe };

struct DrawOptions
{
    CullMode cull = CullMode::TwoSided;
    FillMode fill = FillMode::Solid;
    bool sectionTint = false;
    DirectX::XMFLOAT4 wireColor = { 0.05f, 0.05f, 0.05f, 1.0f };
};
```
- PSO는 `(cull 2) × (fill 2)` = 4개로 만든다. 기존 2개를 복사해 `FillMode = WIREFRAME`만 바꾼다.
- 와이어 PSO의 깊이:
  - `DepthStateReadOnly`를 쓴다. MiniEngine은 reversed-Z(`GREATER_EQUAL`)다.
  - 같은 면을 solid 위에 겹쳐 그릴 때 z-fighting을 막으려면 **양의 `DepthBias`/`SlopeScaledDepthBias`**를 준다. reversed-Z에서는 양수가 카메라 쪽이다. 값은 화면에서 맞추고 문서에 기록한다.
  - **구현 값 (D-1)**: `DepthBias = 200`, `SlopeScaledDepthBias = 1.5`, `DepthBiasClamp = 0`(MiniEngine 기본, 제한 없음). 깊이 포맷은 D32_FLOAT다. 이 값으로 Solid + Wire에서 선이 끊김 없이 보이는 것까지만 스크린샷으로 확인했다. 카메라 거리와 각도를 바꿔 가며 z-fighting이 없는지는 **사용자 확인 필요**이고, 필요하면 `ProceduralMeshRenderer.cpp`의 `kWireDepthBias`, `kWireSlopeScaledDepthBias`만 바꾸면 된다.
  - **루트 상수**: 6 dword(`float4 color`, `uint mode`, `uint section`). `mode` 0 = 정점 색, 1 = solid section 색, 2 = `wireColor`, 3 = section별 와이어 팔레트(PS 안의 상수 4개: 원본 표면 · 분할된 표면 · cap · 여분).
- 와이어 색: 기존 `SectionTint` cbuffer를 쓴다. `TintMode` 2 = `wireColor` 단색, 3 = section별 색(와이어용 밝은 팔레트). PS 분기만 추가하면 된다.

### 2.2 표시 모드 (키 하나로 순환)
| 모드 | 그리기 |
| --- | --- |
| Solid | 현재와 같음 |
| Solid + Wire | solid → 같은 메시를 와이어 PSO로 한 번 더 (bias) |
| Wire only | 와이어만. 뒤의 원본 캐릭터가 보여 overlay(N)와 함께 쓰면 원본 대비 분할 위치가 보인다 |

패치(분리 전), 두 piece, 미리보기에 모두 같은 모드를 적용한다. 원본 모델의 V 와이어프레임과는 독립이다.

### 2.3 "잘린 영역"을 구분해 보이기
와이어만으로는 어디가 절단으로 생긴 삼각형인지 구분하기 어렵다. 그래서 면에 출처를 기록하고 색으로 나눈다.

1. **PlaneSlicer:** 표면 면을 분할해 만든 하위 면에 `Face::cutId = 이번 cutId`를 기록한다. 분할되지 않고 통째로 옮겨진 면은 기존 값을 유지한다(원본은 0). 현재 `Face::cutId`는 cap 전용으로 문서화돼 있으므로 주석과 설계 문서 2절을 함께 고친다. 0 노드에서 갈라진 (0, +, −) 면도 분할이므로 기록 대상이다.
2. **RenderBake:** `BuildRenderMesh(piece, out, const RenderBakeOptions& = {})`에 `splitByOrigin` 옵션을 추가한다. 켜면 section을 `[원본 표면][절단으로 분할된 표면][cap]` 순서로 만든다. 빈 section은 만들지 않는 규칙을 유지한다. `materialSlot`은 표면 0, cap 1 그대로 둔다(재질 의미 불변). 기본값은 꺼짐이고, 현재 출력과 같다.
3. **표시:** section tint(Z)를 켜면 세 구역이 다른 색으로 보인다. 와이어 모드 3(section별 색)이면 선 색으로도 구분된다.

이 옵션은 index 순서만 바꾼다. 정점과 위상, `UpdateRenderPositions`(corner → node 복사)는 영향을 받지 않는다.

**구현 메모 (D-2)**
- `CutDebugView`는 Z가 켜져 있을 때 `splitByOrigin`으로 bake한다. Z를 토글하면 확정 piece가 한 번 다시 bake된다(index 순서만 다시 만든다). 매 프레임 갱신 경로는 바뀌지 않는다.
- 빈 section은 만들지 않으므로 색은 "몇 번째 section인가"로 정해진다. 절단으로 분할된 면이 없거나 원본 그대로인 면이 없으면 색 순서가 한 칸씩 당겨진다(예: 원본 면이 전부 분할되면 분할 면이 section 0 색).
- 재절단에서는 이전 절단이 분할한 면이 이번 절단에서 통째로 옮겨지면 이전 `cutId`를 유지하고, 이번 절단이 다시 나누면 이번 `cutId`로 덮는다. 즉 `cutId != 0`은 "어느 절단에서든 분할된 적 있는 면"이다.

## 3. 기능 B: 평면 편집과 수동 절단

### 3.1 상태
```cpp
class CutController
{
public:
    enum class Mode { Idle, EditPlane };   // Idle: 평면 숨김, 키 무시

    void SetPatch(...);                 // C: 새 패치 → 확정 결과·미리보기 초기화, 평면 프레임 재계산
    void ClearPatch();                  // X
    void SetMode(Mode mode);            // T: 평면 편집 켜기/끄기 (결과는 유지)
    void NudgePlane(float offset, float tiltA, float tiltB);   // [ ] , . ; '
    void ResetPlane();                  // R
    void SetCapEnabled(bool);           // M: 다음 절단과 미리보기에 적용
    void RequestCut();                  // Enter: 다음 Update에서 확정 절단
    void DiscardCut();                  // Backspace: 확정 결과 버리고 절단 전 패치 표시
    void Update(const Model&, const ModelInstance&);   // ModelInstance::Update 직후 1회

    // 읽기 전용 출력 (CutDebugView가 굽는 데 사용)
    const PatchPiece& Patch() const;
    bool HasCut() const;  const PatchPiece& Positive() const;  const PatchPiece& Negative() const;
    const SliceResult* Preview() const;      // 마지막 dry-run (없으면 nullptr)
    SliceStatus LastCutStatus() const;  const std::string& LastCutDetail() const;
    SlicePlane CurrentPlane() const;
};
```

### 3.2 플래그 분리
| 플래그 | 켜는 것 | 소비 |
| --- | --- | --- |
| `m_PreviewDirty` | 평면 변경, 패치 변경, cap 토글, 포즈 변경(애니메이션 재생 중) | `Update`에서 dry-run |
| `m_CutRequested` | Enter | `Update`에서 확정 절단 1회 |

현재의 `m_SliceDirty`(변경 = 즉시 적용)는 없앤다. **평면을 움직여도 확정 결과는 바뀌지 않는다.**

### 3.3 Update 순서 (같은 프레임 포즈)
1. 스킨 행렬과 `poseJointWorld` 계산 → 패치 스키닝. 평면은 world에 고정이라 포즈에 따라 움직이지 않는다 (`PlaneFirstCutDesign.md`)
2. `m_CutRequested`이면: `SlicePatch(Patch(), CurrentPlane())`
   - `Ok` → 확정 결과 교체, `HasCut = true`, 미리보기 지움
   - 실패 → 확정 결과 유지, 상태와 detail 기록
   - 어느 쪽이든 요청을 소비한다(다음 프레임에 재시도하지 않음)
3. 그렇지 않고 `EditPlane`이고 `m_PreviewDirty`이면: dry-run. 결과를 `m_Preview`에 보관만 하고 화면 piece는 바꾸지 않는다
4. 확정 piece가 있으면 매 프레임 스키닝 (현재와 같음)
5. 소요 시간: 미리보기와 확정 절단을 따로 계측해 UI에 표시

**확정 절단 대상은 항상 절단 전 패치(`Patch()`)**다. 두 번째 Enter는 기존 결과를 새 평면 결과로 교체한다. 확정 piece를 다시 자르는 재절단은 확장 B 범위로 남긴다.

### 3.4 미리보기 표시
- 평면 링과 법선 화살표(현재 `BakePlaneDebug`). 색은 미리보기 상태로 정한다: `Ok` 초록, `NoCut` 회색, 그 밖의 실패는 빨강.
- **예상 절단 루프:** `BakeLoopDebug(piece, loops, radius, color)`를 RenderBake에 추가한다. `SliceResult::loops`(+쪽 piece의 노드 순서)를 튜브로 그린다. 확정 전에 어디가 잘리고 `OpenLoop`인지 바로 보인다.
- 미리보기 piece 자체는 그리지 않는다. 화면에는 절단 전 패치(또는 이전 확정 piece)와 루프만 보인다.

### 3.5 애니메이션 중 미리보기 비용
재생 중에는 포즈가 매 프레임 바뀌므로 dry-run도 매 프레임 실행된다. 비용은 계측한 다음 판단한다. 무거우면 "재생 중에는 미리보기 갱신을 멈추고 상태를 '이전 포즈 기준'으로 표시"하는 옵션을 추가한다. 이 옵션은 측정 전에는 만들지 않는다.

### 3.6 패치 교체와 키 변경
- **C(재복사):** 확정 결과와 미리보기를 지우고, 평면 프레임을 새 본으로 다시 계산한다. 현재의 "새 패치를 곧바로 다시 자르기"는 없앤다(자동 절단 제거와 일관).
- **M(cap):** 확정 결과는 그대로 두고, 미리보기와 다음 절단에만 적용한다. UI에 "다음 절단에 적용"으로 표시한다.

## 4. 키 배치

| 키 | 기능 | 비고 |
| --- | --- | --- |
| T | 평면 편집 모드 켜기/끄기 | 의미 변경: 기존 "슬라이스 켜기/끄기" |
| 평면 이동·회전·크기 키 | | P-1에서 `PlaneFirstCutDesign.md` 2.3절의 키로 바뀌었다. 미리보기만 갱신 |
| R | 평면 초기화 | 기존과 같음 |
| **Enter** | 확정 절단 | 신규 (`kKey_return`) |
| **Backspace** | 확정 결과 버리기 | 신규 (`kKey_back`) |
| **U** | 패치 와이어 모드 순환 Solid → Solid+Wire → Wire | 신규 (`kKey_u`). V(원본 와이어)와 별개 |
| M, L, Z, G, N | cap, 후면 컬링, section tint, 경계, overlay | 기존과 같음 |

현재 쓰지 않는 키에서 골랐다(사용 중: B C G H I J K L M N R T V X Z, `[ ] , . ; '`, Space, P, F). UI 도움말 줄을 함께 고친다.

## 5. 구현 단계

| 단계 | 내용 | 확인 |
| --- | --- | --- |
| D-0 | `CutController`/`CutDebugView`로 상태 이동. **동작 변화 없음** | 기존 키 동작과 슬라이스 결과가 같다 (사용자 확인 필요), 테스트 그대로 통과 |
| D-1 | 렌더러 `FillMode` + PSO 4개 + 와이어 색, U 키 | 세 모드가 보이고 Solid+Wire에서 z-fighting이 없다 (사용자 확인 필요) |
| D-2 | `Face::cutId` 기록 + `splitByOrigin` bake | 단위 테스트: 분할 면만 cutId != 0, 두 piece의 section 면 수 합 = 면 수, 기본 bake 출력 불변 |
| D-3 | 미리보기/확정 분리, Enter/Backspace, `BakeLoopDebug` | 평면을 움직여도 확정 piece 불변, Enter 1회 = 절단 1회, 실패 시 이전 piece 유지 (사용자 확인 필요) |

D-1과 D-2는 서로 독립이다. D-3은 D-0 뒤에 한다.

## 6. 하지 않는 것
- 확정 piece의 재절단(확장 B), 실행 취소 스택(직전 결과 하나만 버릴 수 있다)
- 마우스로 평면 조작(기즈모). 키 조작으로 충분한지 써 본 뒤 판단한다
- 조명과 법선 갱신(설계 문서 12절의 제한 그대로)

## 7. 구현 메모 (D-3)
- `m_SliceEnabled`/`m_SliceDirty`는 `m_PlaneEditing`/`m_PreviewDirty`/`m_CutRequested`로 바뀌었다. T는 평면 편집 켜기/끄기이고, 확정 piece는 편집 모드와 무관하게 남는다. R, 평면 이동 키, Enter는 편집 모드에서만 동작하고 Backspace는 항상 동작한다.
- 미리보기는 포즈가 바뀌었을 때(`poseJointWorld`가 이전 프레임과 다를 때)와 평면, cap, 패치가 바뀌었을 때 다시 계산한다. 정지 중에는 계산하지 않는다. 애니메이션 재생 중에는 매 프레임이다.
- 미리보기 시간(`PreviewMs`, 지수 평균)과 확정 절단 시간(`CutMs`, 마지막 1회)을 따로 표시한다. 속도 제한은 두지 않았다. 값은 실행해서 읽어야 하며 이 문서에는 적지 않는다.
- `SlicePatch`는 실패하면 status와 detail만 채우므로, 실패한 미리보기에서는 루프 튜브가 없고 상태 줄만 보인다(`OpenLoop`이어도 루프는 그려지지 않는다).
- **키 충돌:** `Core/EngineTuning.cpp`는 Backspace로 튜닝 메뉴를 열고 닫으며, 메뉴가 열려 있으면 Enter로 선택한 변수에 Bang을 보낸다. `Core/EngineProfiling.cpp`는 Enter로 프로파일러 항목을 토글한다. 그래서 Backspace를 누르면 확정 결과 버리기와 튜닝 메뉴 토글이 함께 일어난다. 다른 키로 바꾸려면 `ModelViewer.cpp`의 `kKey_back` 한 줄만 고치면 된다.
- `CutController`는 `Model`/`ModelInstance` 없이 만들 수 없는 `SetPatch`/`Update`를 거쳐야 하므로 단위 테스트로 플래그 동작을 검사하지 않았다. 순수 함수인 `BakeLoopDebug`만 테스트했다.

## 8. 구현 메모 (P-1, 평면 우선 절단)
- 3.1의 `NudgePlane`과 joint 로컬 평면(`ComputeBasePlane`/`UpdateBasePlane`)은 없어졌다. 평면은 `Geometry/CutPlane`의 `CutPlane`(world, 쿼터니언, halfExtents) 하나이고, 조작은 `TranslatePlane`, `MovePlaneAlongNormal`, `RotatePlane`, `ScalePlane`이다. 키는 `PlaneFirstCutDesign.md` 2.3절.
- T는 패치가 없어도 절단 모드를 켠다(처음 켤 때, 또는 R로 평면을 놓는다). 패치가 없으면 미리보기를 계산하지 않고 Enter는 무시한다.
- `LoopOutsidePlane`은 `SliceStatus`에 추가했지만 `SlicePatch`가 돌려주는 값이 아니다. 컨트롤러가 슬라이스 뒤 사각형 검사에 실패했을 때만 쓴다. 상태 전달 경로(미리보기 링 색, UI, 확정 절단 실패 시 이전 piece 유지)가 모두 `SliceStatus` 하나로 되어 있어서 별도 타입을 만들지 않았다.
- 평면과 예상 루프는 world 그대로(캐릭터 위)에, 패치와 확정 piece는 기존처럼 표시 오프셋으로 그린다.

## 9. 구현 메모 (P-2, 후보 패치)
- `SegmentPick`(`Geometry/SegmentPick.*`)과 joint 집합 규칙(`Geometry/JointSet.*`)은 Model 없이 테스트한다. `BonePatch`는 `ComputePaletteSkeleton`(패치 없이 팔레트의 joint 위치와 팔레트 안 부모)과 joint 집합 `ExtractBonePatch`를 가진다. 단일 joint 버전은 크기 1 집합 버전을 부른다.
- **집합의 삼각형 선택:** 정점이 집합 안 joint 중 하나라도 `JointInfluence > minWeight`이면 그 정점이 고른 것이고, 삼각형은 정점 하나라도 고르면 복사한다. joint마다 따로 판단하므로 집합의 결과는 각 joint 결과의 합집합이다(`VertexInJointSet`의 테스트). 정점 색의 가중치는 집합 안 joint의 합이다.
- **후보 패치 갱신(`CutController::UpdateCandidate`):** 절단 모드에서 매 프레임 뼈대 위치를 구해 `PickSegments`를 돌린다. 설계의 "평면 또는 포즈가 바뀌었을 때만"과 달리 매 프레임 돌리는데, joint 수십 개의 선분 검사라 값이 작다. 절단 joint 집합이 직전에 시도한 집합과 같으면 추출하지 않고 스키닝만 한다. 실패한 집합도 같은 집합이면 다시 시도하지 않는다. 추출 + 용접 시간은 마지막 값과 횟수를 UI에 표시한다.
- **C/X와의 관계:** 절단 모드에 들어가면 C로 복사한 패치를 버리고, 절단 모드에서 C/X는 동작하지 않는다. 절단 모드를 끄면 후보 패치도 버린다. 확정 piece는 어느 쪽에서도 남는다.
- **확정 piece의 스킨 슬롯:** 확정 piece는 자른 시점 패치의 메시 슬롯으로 스키닝해야 하므로(`m_PieceSource`), 후보 패치가 바뀌어도 piece는 영향을 받지 않는다. 패치가 없어도 piece는 계속 스키닝된다.
- **웨이트 뷰:** 절단 모드에서는 절단 joint가 있으면 H 뷰가 그 joint를 보여주고(Tab으로 순환), 없으면 J/K로 고른 joint를 쓴다. J/K 선택 자체는 바뀌지 않는다. 한 번에 하나만 보인다(셰이더가 joint 하나만 받는다).
- **팔레트:** 절단 모드는 T를 누른 시점에 선택된 팔레트 하나만 다룬다. 여러 팔레트를 합치는 것은 하지 않았다(CesiumMan은 하나).
- **표시:** 교차한 뼈 선분은 노랑 튜브, 교차점은 흰 십자로 캐릭터 위(world)에 그린다. 평면이 어떤 뼈도 지나지 않으면 평면 색은 회색이다.
