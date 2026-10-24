# 디버그 흐름 변경 설계: 평면 우선 절단

> 이력 문서: 아래 상태·구현 설명은 과거 CPU 절단 경로의 설계·기록이다. 현재 구현 상태로 해석하지 않는다. 현행 흐름은 [CodeMap.md](../../Current/CodeMap.md), 평면 계약은 [CutPlaneDesign.md](../../Current/CutPlaneDesign.md)를 따른다.

> 작성: 2026-10-20 · 대상 커밋: `6311352` (CutController/CutDebugView, D-0~D-3 반영 후)
>
> v2: 절단 평면을 **world에 정의된 유한 평면(이동·회전·크기 조절)**으로 바꾸고, 한 번에 여러 선분을 자를 수 있게 했다.
>
> 상태: P-1(CutPlane)과 P-2(SegmentPick, joint 집합 추출, 후보 패치)는 구현됨. S-1~S-4(마우스, Scene 뷰 카메라, 기즈모, 키 정리)로 조작 체계가 바뀌었다. P-3(OpenLoop 확장, 표시 변환 분리의 나머지)은 아직 없다. 구현하면서 달라진 점은 `CPU 절단 디버그 설계(삭제된 과거 기록)` 8~9절.

## 0. 왜 바꾸는가

현재 흐름은 **joint 우선**이다.

1. J/K로 joint 선택 → C로 그 joint의 영향 삼각형을 패치로 복사
2. 평면은 그 joint 위치에서 자식 방향으로 초기화되고, **joint 로컬 프레임**에 묶여 offset/tilt로 움직인다
3. Enter로 절단

평면이 실제로 어느 선분을 지나는지는 확인하지 않는다. 평면이 자식 joint를 넘어가면 다른 선분을 자르는 상황인데 패치는 처음 joint 그대로라서, 절단선이 교체 경계에 닿아 `OpenLoop`가 된다. 또 평면이 관절을 따라 움직이므로, 월드의 정해진 위치를 자르는 시나리오(칼이 지나가는 자리)를 표현할 수 없다.

NDC 흐름은 반대다. 절단 위치(평면)가 먼저 정해지고, 그 위치가 걸친 선분의 **부모 joint**를 고른 뒤 그 joint의 영향 범위를 복사해 자른다(NDC 6:17~6:51).

## 1. 새 흐름

```
[절단 모드 T]
 CutPlane(world, 이동·회전·크기) ──▶ SegmentPick: 평면 사각형 안에서 교차하는 모든 선분
                                     → 선분별 부모 joint (중복 제거) = 절단 joint 집합
     ├─ 원본 메시: 절단 joint들의 웨이트 표시
     ├─ 교차 선분 튜브 + 교차점
     └─ 후보 패치 = Extract(절단 joint 집합 ∪ 확장 joint) → dry-run 슬라이스 → 상태 색·예상 루프
 Enter ──▶ 후보 패치를 확정 절단 → 옆(표시 오프셋)에 두 piece
```

## 2. 절단 평면 (CutPlane)

### 2.1 정의
```cpp
// World 공간에 정의된 유한 사각 평면. 어떤 joint에도 묶이지 않는다. (K-1에서 클래스로 캡슐화했다.)
class CutPlane
{
public:
    struct Frame { /* center, axisU, axisV, normal, halfExtents: 정규 직교, u × v = n */ };

    Frame GetFrame() const;                  // 이전 MakeFrame
    SlicePlane ToSlicePlane() const;         // 슬라이서에는 무한 평면으로 넘긴다
    bool Contains(const DirectX::XMFLOAT3& p, float margin) const;       // 이전 ContainsInPlane
    bool ContainsLoops(const PatchPiece& positive, const std::vector<std::vector<uint32_t>>& loops, float margin,
        std::string* error = nullptr) const;                              // 이전 LoopsInsidePlane
    void Translate(...); void RotateAroundLocalAxis(...); void ScaleUniform(...); void ScaleU(...); void ClampExtents();
    // private: m_Shape(center, rotation 쿼터니언 로컬 +Z = 법선, halfExtents), m_MinExtent/m_MaxExtent, 기즈모 상태
};
```
- `Geometry/CutPlane.h`, `CutPlane.cpp`, `CutPlaneGizmo.cpp`에 둔다(Model 의존 없음). 기즈모는 `SceneViewControlsDesign.md` 3절.
- 패치 위치가 world 스냅샷이므로 같은 공간이다. 캐릭터 locator가 움직여도 평면은 world에 그대로 있다.
- 애니메이션 재생 중에는 팔다리가 평면을 지나가며 교차 선분이 바뀐다. 이는 의도된 동작이다(칼이 놓인 자리를 캐릭터가 지나가는 상황).

### 2.2 크기의 의미
크기는 **어떤 선분을 자를지**와 **루프 검증**에만 쓴다. 삼각형 분할 자체는 지금처럼 무한 평면으로 한다.

- SegmentPick은 교차점이 사각형 안(`CutPlane::Contains`)인 선분만 고른다.
- 슬라이스 후 **모든 루프 점이 사각형 안(여유 margin 포함)**이어야 한다. 밖으로 나간 루프가 있으면 `LoopOutsidePlane`으로 거절한다. 패치가 넓은 joint(몸통 등)에서 무한 평면이 사각형 밖의 엉뚱한 부위까지 자르는 경우를 막는다.
- 평면 가장자리가 팔다리 안에서 끝나는 경우(부분 절개)는 지원하지 않는다. 그런 선분은 교차점이 사각형 안이면 통째로 자르는 것으로 보고, 루프가 사각형을 벗어나면 위 규칙으로 거절된다.

### 2.3 조작
> **S-4에서 바뀌었다.** 평면은 키보드가 아니라 마우스 기즈모(이동·회전·크기 도구)로 조작한다. 도구, 핸들, 드래그 방식은 `SceneViewControlsDesign.md` 3절. 크기 범위(모델 반지름의 0.01~3배)와 초기값(모델 bounds 중심, 법선 world +Y, 반 크기 = 반지름 × 0.5)은 그대로다. 아래 2.4절의 표시는 사각형 테두리, 십자선, 법선 화살표이고 기즈모가 그 위에 겹쳐진다.

### 2.4 표시
- 사각형 테두리 + 십자 격자 몇 줄 + 법선 화살표를 튜브로 그린다(`CutDebugView`의 `BakePlaneDebug`, 기존 링 대신).
- 반투명 면은 이번에 넣지 않는다(렌더러에 블렌딩 PSO가 없다).
- 색 = dry-run 상태(Ok 초록, NoCut/NoSegment 회색, 실패 빨강).
- UI: center, 회전(오일러 근사 표시), halfExtents, 교차 선분 수.

## 3. SegmentPick (Geometry/CutPlane.h, Model 의존 없음)

```cpp
struct SkeletonPose                 // 한 팔레트, world(패치 공간)
{
    std::vector<DirectX::XMFLOAT3> jointPositions;   // poseJointWorld와 같은 행렬 규약
    std::vector<uint32_t> parents;                   // 팔레트 로컬 부모, 없으면 kNoNode
};

struct SegmentHit
{
    uint32_t parentJoint;           // 절단할 joint (선분의 부모 쪽 끝)
    uint32_t childJoint;
    float t;                        // 부모 0 → 자식 1
    DirectX::XMFLOAT3 point;        // 교차점 (world)
};

// 부모 → 자식 선분마다 평면 양쪽에 있는지 검사한다(d >= 0을 + 쪽으로 판정).
// 교차점이 사각형 안인 것을 모두 돌려준다. 순서: parentJoint, childJoint 오름차순.
void CutPlane::PickSegments(const SkeletonPose& pose, std::vector<SegmentHit>& hits) const;

// 절단 joint 집합: hits의 parentJoint를 중복 없이, 오름차순으로.
static std::vector<uint32_t> CutPlane::CutJoints(const std::vector<SegmentHit>& hits);
```
- **여러 선분 동시 절단:** 맞은 선분을 모두 쓴다. 두 다리를 지나는 큰 평면이면 두 허벅지 선분이 모두 맞고, 절단 joint는 두 개다.
- **팔레트 로컬 부모:** `m_NodeParents`를 따라 위로 올라가다 처음 만나는 같은 팔레트 joint가 부모다(Model 의존 변환은 `BonePatch.*`).
- **자식이 여럿인 joint:** 어느 선분에 맞든 부모 joint는 같다. 표시는 맞은 선분마다 한다.
- **말단 joint 너머:** 선분이 없으므로 맞지 않는다. 가상 선분 연장은 이번 범위가 아니다.
- **joint 위치:** 패치 없이 팔레트 하나의 joint world 위치를 구하는 함수를 `BonePatch.*`에 추가한다(팔레트를 쓰는 메시의 world를 곱하는 기존 규약과 동일).
- **여러 팔레트:** 팔레트마다 검사해 합친다. 패치 추출은 팔레트별로 한다(CesiumMan은 1개).

## 4. 패치와 슬라이스

- **하나의 패치, 한 번의 슬라이스:** 절단 joint 집합(+확장 joint)으로 패치를 하나 추출하고 한 번 자른다. 슬라이서는 이미 루프 여러 개를 지원하므로 두 다리를 동시에 자르면 루프가 두 개 나온다.
- 결과 piece는 섬(연결 성분)을 여러 개 가질 수 있다(예: +쪽 piece = 두 허벅지 위쪽). 이번에는 섬을 나누지 않고 그대로 표시한다. 본 분리(4주차)에서는 섬마다 따로 움직여야 하므로 그때 연결 성분 분리를 추가한다.
- `ExtractBonePatch`는 joint 집합을 받도록 바꾼다. 조건: "정점 하나라도 집합 안 joint에 대한 웨이트가 minWeight 초과"인 삼각형. 크기 1 집합은 기존 결과와 같아야 한다.
- 확정 결과에 `cutJoints`(절단 joint 집합)와 `patchJoints`(확장 포함)를 기록한다. 이후 끊을 joint는 `cutJoints`다.

## 5. OpenLoop 대응: 필요할 때만 이웃 joint를 합친다

한 joint의 패치는 선분 P→C와 양 끝 혼합 구간까지만 덮는다. 평면이 혼합 구간 너머까지 걸치면 절단선이 교체 경계에 닿는다. NDC의 "합친 뒤 자르기"를 최소한으로 적용한다.

1. 패치 joint 집합 = 절단 joint 집합으로 추출 → dry-run
2. `OpenLoop`이면 **맞은 선분마다** 교차 위치 쪽 이웃을 추가한다
   - `t < 0.5` → P의 팔레트 부모(부모 끝 너머의 영역)
   - `t ≥ 0.5` → 맞은 선분의 자식 C(자식 끝 너머의 영역)
3. 다시 추출 → dry-run. 여전히 `OpenLoop`이면 반대쪽 이웃도 추가해 한 번 더 시도한다(최대 2회 확장)
4. 그래도 실패하면 거절하고, 시도한 joint 집합을 detail에 남긴다

- 확장은 패치 기하만 넓힌다. 끊을 joint는 여전히 절단 joint 집합이다.
- 이웃 선택 규칙(`t` → 추가할 joint)은 순수 함수로 두고 테스트한다.
- NDC식 각도 보정(경계에 닿는 평면을 안전한 평면으로 옮기기)은 이번 범위가 아니다.

## 6. 캐시와 갱신 시점

| 무엇 | 언제 다시 계산 |
| --- | --- |
| 팔레트 joint 위치 | 매 프레임 (`ModelInstance::Update` 직후) |
| SegmentPick | 평면 또는 포즈가 바뀌었을 때 |
| 후보 패치 추출 + 용접 | **패치 joint 집합이 바뀌었을 때만**. 같으면 기존 패치를 스키닝만 한다 |
| dry-run 슬라이스 | 평면, cap, 후보 패치, 포즈가 바뀌었을 때 (기존 미리보기 규칙) |
| 확정 piece 스키닝 | 매 프레임 (기존) |

평면 크기만 바뀐 경우에도 SegmentPick과 루프 검증은 다시 한다. 재생 중 재추출 빈도와 추출 시간은 계측해 UI에 표시한다. 제한 옵션은 측정 전에 만들지 않는다.

## 7. 표시

| 대상 | 위치 | 비고 |
| --- | --- | --- |
| 절단 평면 사각형·법선 | 캐릭터 위 (world 그대로) | 색 = dry-run 상태 |
| 교차 선분 튜브·교차점 | 캐릭터 위 | 확장 joint의 선분은 다른 색 |
| 원본 웨이트 뷰 | 캐릭터 | 절단 모드에서는 H 뷰를 절단 joint들에 자동 연결 (현재 셰이더가 joint 하나만 받으면, 집합 지원을 추가하거나 Tab으로 하나씩 순환) |
| 예상 루프 | 캐릭터 위 | 기존 `BakeLoopDebug` |
| 확정 piece | **옆** (기존 표시 오프셋 + 법선 방향 벌림) | 와이어·tint·경계 옵션 기존과 같음. N(overlay)은 캐릭터 위에 겹침 |

## 8. 그 밖의 키
> **S-4에서 바뀌었다.** 남은 키는 Space(애니메이션), T(디버그 모드), V(렌더링 모드), Enter(절단), Ctrl+Z(되돌리기)와 Scene 뷰 카메라·기즈모 키뿐이다 (`SceneViewControlsDesign.md` 5절). Tab(웨이트 뷰 순환), M, U, Z, G, N, L, C, X, R은 없어졌다. 웨이트 뷰는 디버그 모드에서 첫 절단 joint를 따른다.

## 9. 구현 단계

| 단계 | 내용 | 확인 |
| --- | --- | --- |
| P-1 | `CutPlane`(world, 이동·회전·크기) + 사각형 표시 + 조작 키. 기존 joint 로컬 평면 제거. 절단 대상은 아직 C로 복사한 패치(기존 흐름)이며 루프 사각형 검증(`LoopOutsidePlane`)을 추가 | 단위 테스트(프레임 직교성, Contains, 조작 후 정규화), 평면이 world에 고정되는지 (사용자 확인 필요) |
| P-2 | 팔레트 joint 위치·부모 + `SegmentPick`(여러 선분) + 교차 표시 + `ExtractBonePatch` joint 집합화 + 후보 패치 자동 추출·캐시 + 웨이트 뷰 연결 | 단위 테스트, 큰 평면으로 두 다리가 동시에 후보가 되는지 (사용자 확인 필요) |
| P-3 | OpenLoop 확장(5절) + `cutJoints`/`patchJoints` 기록 + 표시 변환 분리 | 확장 규칙 테스트, 이전 OpenLoop 위치가 Ok가 되는지 (사용자 확인 필요) |

## 10. 하지 않는 것
- 본 분리, 원본 숨김, 물리, 분리 기여 교체, 섬(연결 성분) 분리
- 부분 절개(평면 가장자리가 메시 안에서 끝나는 절단), 말단 joint 가상 선분, NDC식 평면 각도 보정, 확정 piece 재절단
- 마우스 기즈모, 반투명 평면 면
