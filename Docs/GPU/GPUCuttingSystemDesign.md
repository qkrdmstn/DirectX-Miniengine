상태: 진행 중 — 기준 `4bc5cdf9` 이후 Static GPU 절단·단면·재절단 구현/검증 완료(미커밋); Skeletal 절단·물리 미구현

# GPU 기반 메시 절단 시스템 설계

## 1. 기준과 적용 범위

기존 CPU 절단 구현을 초기화한 뒤 GPU 절단을 새로 구성한다. 현재 MeshCutting 정적 라이브러리와 평면 기하, ModelViewer의 GameObject/TRS·선택·기즈모·Undo, 유한 평면 편집·표시, 표시 모드는 보존한다. 이 문서는 구현 완료 보고가 아니라 새 backend의 입력·출력과 미결정 사항을 기록한 설계안이다.

- 현재 구현 기준: `4bc5cdf9`.
- 초기화 시점 기록: `e5975f9b0744fc5096dd593ed72bf8f5c004164f`.
- 초기화 전 작업 보관: `2b4871f5d63cb369bfd51f340dce239b507e31be`.
- 보관 브랜치: `codex/meshcutting-before-gpu-reset-20261023`.
- 초기화 전 CPU 절단 함수는 보관 스냅샷의 구현이다. 현재 평면 기하·Scene 구현과 구분하며 GPU 절단 기능으로 해석하지 않는다.
- 11·14절은 pose/컴포넌트 기반 구현 이력이며 최신 Static 절단의 실제 API·정책·검증은 [StaticCutImplementation.md](StaticCutImplementation.md)를 따른다. 나머지 Skeletal 절단 등의 이름은 설계 구분이며 API 구현을 뜻하지 않는다.

## 2. 확정된 방향

1. 원본 Model GPU 버퍼를 읽기 전용 입력으로 사용한다.
2. 렌더 정점·삼각형 인덱스를 기준으로 절단한다. 전체 입력의 용접이나 명시적인 전역 인접성 구축은 필수 경로에 넣지 않는다.
3. CPU는 입력·평면 조작·애니메이션 본 행렬·작업 제어를 담당한다. 정점 스키닝, patch 선택, 절단, 단면 생성, 결과 렌더링은 GPU 경로를 목표로 한다.
4. static과 skeletal은 입력 위치를 준비하는 단계만 다르고 이후 절단 경로는 공유한다.
5. 원본 모델을 그리는 버퍼를 절단 출력으로 덮어쓰지 않는다. 새 교차점·cap·인덱스에는 결과 저장 공간이 필요하다.
6. 렌더링을 위해 결과 정점 전체를 매번 CPU로 읽어오는 경로는 만들지 않는다. 제어·오류·진단 정보의 제한된 비동기 읽기는 필요에 따라 허용한다.

Unreal의 CPU ProceduralMesh Slice는 삼각형별 절단과 절단선 연결의 참고 방식이다. 여기서 설계하는 GPU backend는 별도 구현이며, Unreal이나 NDC의 실제 GPU 구현을 재현했다고 주장하지 않는다.

## 3. 엔진 입력 계약

현재 엔진의 Model GPU `m_DataBuffer`에는 원본 정점·인덱스 데이터가 들어 있다. GPU 절단은 이것을 입력으로 읽는다. CPU에 geometry 전체를 풀어 복사하는 중간 소유자는 필수로 두지 않는다.

CPU는 mesh별로 다음 메타데이터를 GPU에 제공해야 한다.

| 정보 | 목적 |
|---|---|
| 정점·인덱스 시작 위치와 개수, stride, index 형식 | 올바른 범위와 16/32-bit 인덱스 해석 |
| 정점 속성 형식과 존재 여부 | 압축 위치·법선·탄젠트·UV·joint/weight 해석 |
| draw/primitive/material 대응, 원본 삼각형 ID | 결과 재질과 원본 마스킹 연결 |
| mesh/node 변환과 좌표 공간 | static 위치 준비 및 평면과 공간 일치 |
| joint palette 대응과 현재 본 행렬 | skeletal 스키닝 및 선택 본 검사 |

동일한 버퍼 종류라고 모든 mesh의 정점 레이아웃이 같다고 가정하지 않는다. 실제 입력 형식·SRV 생성·리소스 상태는 첫 구현 단계에서 검증한다. 모델 하나에 static/skeletal mesh가 섞이면 mesh별로 경로를 선택한다.

본 계층과 애니메이션 행렬 생성은 CPU에 유지한다. GPU는 전달받은 행렬로 다수의 정점을 변형한다. GPU skin cache와 본 행렬의 GPU 생성은 서로 다른 기능이다.

### 3.1 현재 입력에서 확인한 사실

- `ModelLoader`는 변환된 geometry를 `Model::m_DataBuffer`에 올린다. `ByteAddressBuffer::CreateDerivedViews`는 RAW SRV를 이미 생성한다. Compute 바인딩과 읽기 경로의 구현·검증은 11절에 기록한다.
- `Mesh`의 VB/IB byte offset, size, stride, index format과 `Mesh::Draw`의 `startIndex`, `baseVertex`, `primCount`를 함께 해석해야 한다. `primCount`는 인덱스 수다.
- 원본 glTF accessor를 직접 읽는 형식이 아니다. `Renderer::GetPSO`의 입력 레이아웃과 변환기의 packed/interleaved VB 형식을 따른다. depth VB와 surface VB를 혼동하지 않는다.
- `Scene::CreateObject`는 객체별 `ModelInstance`를 만들지만 `shared_ptr<const Model>`은 공유한다. 본 팔레트·애니메이션은 ModelInstance별이며 mesh 상수는 MeshComponent별이다.
- `ModelInstance::Update`와 `DefaultVS`/`DepthOnlyVS`의 실제 곱셈 순서를 기준으로 Compute 위치 준비를 맞춘다. 결과를 원본 모델 버퍼에 기록하면 다른 객체까지 오염된다.

## 4. 좌표 공간과 프레임 계약

- 절단 평면과 검사할 정점은 반드시 같은 공간에 있어야 한다.
- static도 mesh/node 변환이 필요하다. 스키닝 생략은 모든 변환 생략을 뜻하지 않는다.
- skeletal의 절단·표시·선택은 같은 프레임의 본 행렬을 사용해야 한다.
- 절단 평면의 중심·법선·평면 축·가로/세로 반크기를 한 상태로 관리한다. gizmo 표시와 GPU 절단은 같은 상태를 받는다.
- 유한 사각형이 실제 절단 범위인지, 무한 평면의 표시인지 명시한다. 크기 제한을 GPU 전환 과정에서 암묵적으로 없애지 않는다.
- 비균일 scale이 있으면 점·방향·법선을 같은 방식으로 변환하지 않는다. 평면 변환과 normal의 inverse-transpose 처리를 검증한다.

현재 위치 준비 구현은 스키닝·node 변환 뒤 source 프레임 역변환을 포함한 component-local 좌표를 저장한다. 후속 절단 평면도 대상 Wc를 사용해 같은 공간으로 옮긴다. 렌더러는 Wc를 한 번 적용한다(13·14절).

## 5. GPU 처리 흐름

```text
원본 GPU 버퍼 + mesh 메타데이터 + CPU 본 행렬/평면/선택 본
  → patch 삼각형 선택(필요한 경우)
  → 위치 준비: static rigid transform / skeletal GPU skinning
  → 삼각형별 clip
       ├→ 남는 표면의 정점·인덱스·출처
       └→ 절단 평면 위 선분
  → 선분 끝점 연결 → 폐곡선/영역 구성 → cap 삼각분할
  → 결과 버퍼·draw 정보 → GPU 렌더링
```

처리 순서는 데이터 의존성에 맞춰 조정할 수 있다. 원본 weight에 기반한 patch 소속은 pose 계산 전에 판정할 수 있다. 반면 화면 선택이나 실제 교차검사는 현재 pose가 필요하다. 모든 mesh를 먼저 스키닝하는 경로와 선택 patch만 스키닝하는 경로의 비용은 측정 후 결정한다.

### 5.1 선택 본의 patch

현재 논의의 기본 조건은 삼각형의 세 정점 중 하나라도 선택 본의 원본 weight가 0보다 크면 삼각형 전체를 유지하는 것이다. 경계에서 weight가 0인 나머지 정점도 함께 유지한다.

선택 본의 전역 ID와 mesh별 joint palette 인덱스를 명시적으로 연결한다. threshold 변경 여부는 별도 정책으로 기록하며 조용히 조건을 바꾸지 않는다.

patch는 원본 정점을 모두 복사하지 않고 원본 삼각형/인덱스 목록으로 표현할 수 있다. 압축·정점 remap은 후속 최적화이며 전역 용접의 선행 조건이 아니다. 마스킹과 재질을 위해 원본 mesh/draw/triangle ID를 유지한다.

### 5.2 위치 준비

GPU compute는 skeletal 정점의 현재 pose를 버퍼에 기록한다. 렌더와 절단이 이 버퍼를 공유하려면 원본 렌더러의 입력과 shader 경로도 맞춰야 한다. 이미 변형된 위치에 VS 스키닝을 다시 적용하지 않는다.

static은 bone 연산을 생략하고 rigid 변환으로 같은 위치 계약을 만족시킨다. 중복 렌더 정점마다 스키닝하는 방식도 올바르다. 위치 계산 공유는 선택 최적화이며 bind 위치·공간·joint/weight가 동일한 경우에만 허용한다. UV/법선 seam은 유지한다.

### 5.3 삼각형 절단

인덱스 세 개가 삼각형과 세 변을 정의하므로 교차검사는 삼각형별로 독립 처리할 수 있다. 정점의 평면 부호와 변의 교차점을 계산하고, 속성을 보간해 표면 삼각형과 절단 선분을 출력한다.

출력 개수는 입력 삼각형마다 달라진다. 다음 중 하나를 선택한다.

- 개수 계산 → 누적 offset 계산 → 결과 기록.
- append/atomic counter로 공간 확보 후 기록.

어느 방식도 현재 구현된 것으로 간주하지 않는다. 최대 출력량·용량 초과 시 동작·재시도 정책을 먼저 정한다. 평면 위 정점, 거의 평행한 변, 0면적 삼각형의 허용오차도 정의한다.

### 5.4 단면 선분 연결

절단 선분을 평면의 2D 좌표로 옮기고 끝점 검색·매칭으로 폐곡선을 구성한다. 후보 수단은 정렬 또는 양자화/hash이며 아직 선택하지 않았다.

이 단계는 **절단 경계에 한정된 연결 복원**이다. 원본 mesh 전체의 용접이나 인접 삼각형 표를 다시 요구하지 않는다. 대신 허용오차, 중복 선분, 열린 경계, 분기점, 접촉하는 loop를 처리하는 규칙이 필요하다. Unreal 참고 구현의 수치를 우리 좌표 scale에 검증 없이 그대로 적용하지 않는다.

### 5.5 cap과 영역

cap은 평면 법선·별도 UV·재질을 가진 렌더 정점을 생성한다. 표면 UV/법선을 cap에 그대로 사용하지 않는다. loop 연결, hole 분류, 삼각분할은 서로 다른 단계다.

외피·내피를 어떤 물체로 해석하는지가 먼저 정해져야 한다.

- 속 빈 관: outer에서 inner 영역을 뺀 고리 단면.
- 피부와 내부 살: 피부 고리와 내부 살의 채워진 단면을 별도 재질로 표현할 수 있음.
- 독립된 중첩 solid: 각 물체의 cap을 별도로 생성하는 정책이 필요할 수 있음.

중첩 loop를 무조건 hole로 처리하지 않는다. 같은 위치의 층을 병합하거나 cap을 겹쳐 z-fighting을 만드는 것도 피해야 한다. 원본 mesh/draw/material 구분으로 충분한지 확인한 뒤 부족한 경우에만 영역 식별자를 추가한다.

## 6. 새 정점의 속성과 지속 애니메이션

표면 교차점은 위치뿐 아니라 UV·normal·tangent·color 등 필요한 속성을 보간한다. normal/tangent 정규화와 handedness 처리는 정점 형식에 맞춰 정의한다. 원본 surface material/section 정보는 결과까지 유지한다.

현재 pose에서 위치를 만든 것과 다음 프레임에도 올바르게 움직이는 것은 별개다.

- 고정된 분리 조각: 현재 pose 위치를 bake하고 조각 변환으로 움직일 수 있다.
- 계속 애니메이션하는 조각: 새 정점이 원본 변형 결과를 어떻게 재구성하는지 출처를 보존해야 한다.

joint/weight를 단순 보간한 뒤 LBS하는 결과가 원본 정점들의 변형 위치를 보간한 결과와 항상 같다고 가정하지 않는다. 원본 정점 ID와 출처 계수 등 표현을 결정해야 한다. cap 재절단에서는 출처가 여러 원본 삼각형에 걸칠 수 있으므로 항상 원본 정점 세 개만으로 충분하다고 가정하지 않는다.

cap의 위치, normal/tangent, 내부층의 독립 변형도 각각 계약이 필요하다. 이 항목을 결정하기 전에는 지속 skeletal 절단을 완료했다고 표시하지 않는다.

## 7. 결과·마스킹·수명

- 결과는 새 정점/인덱스 버퍼와 material별 draw 정보를 가진다. 원본의 변하지 않은 정점 참조는 가능하지만 실제 binding 방식은 별도 결정한다.
- GPU 출력에는 UAV 접근과 필요한 barrier/state transition을 적용한다. 동시에 원본을 그리는 입력 버퍼는 덮어쓰지 않는다.
- 출력 개수로 draw를 실행하는 방법은 CPU 제어 또는 indirect draw 중 선택한다. indirect draw가 이미 제공된다고 가정하지 않는다.
- 원본 숨김은 source triangle ID와 연결한다. 기존 CPU 색상 override 구현을 그대로 재사용한다고 가정하지 않는다.
- 작업 ID, 취소, Undo, 본 분리와 GPU 완료 상태를 연결한다. 늦게 끝난 작업이 취소된 결과를 다시 활성화하면 안 된다.
- fence 이전에 입력·출력·임시 버퍼를 해제하거나 재사용하지 않는다. 실패·overflow 시 원본 가시성과 이전 결과의 일관성을 보장한다.
- CPU 동기 읽기는 필수 경로로 두지 않는다. 필요한 count/status와 진단 결과는 작게, 가능하면 비동기로 전달한다.

### 7.1 제안: 한 소유 객체, 여러 메시 컴포넌트

현재 확정하려는 재설계는 13절이다. GameObject A/B를 매번 만드는 초기안 대신 같은 GameObject 안에 조각별 MeshComponent를 둔다. 결과 A/B는 메시 조각의 구분이며 GameObject 두 개를 뜻하지 않는다. 영역 밖 표면도 해당 조각에 배정하고, 각 조각은 여러 section·연결 성분·draw를 가질 수 있다.

| 소유자 | 데이터 | 수명 / 공유 |
|---|---|---|
| `Model` | 원본 GPU 버퍼, mesh/draw/layout, material, 본 계층·clip·joint 대응 | 읽기 전용 공유 자산 |
| `GameObject`의 `ModelInstance` 한 개 | 원본 Model 참조, 애니메이션·node/bone pose, 본 분리 상태 | 조각들이 같은 pose 평가를 공유 |
| `GameObject`의 여러 `MeshComponent` | ComponentId, 변환·deform binding, CurrentGeometry, PosedGeometry | 조각별 상태·GPU 출력 |
| `GeometryRevision` | 원본 참조/생성 section, VB/IB, material, 출처, 로컬 bounds | 게시 후 불변. 활성 컴포넌트·Undo가 참조 |
| `GameObject`의 여러 `ColliderComponent` (후속) | 대상 MeshComponentId, 엔진 독립 shape 설명·충돌 필터·물리엔진 등록 참조 | 메시 경계와 구분하며 실제 body/constraint는 PhysicsWorld 소유 |
| `CuttingSystem` / `CutJob` | source ObjectId+ComponentId+revision, snapshot, GPU 후보 결과 A/B, fence | 결과 게시·실패·GPU 완료까지 유지 |

Scene은 GameObject를, GameObject는 모델 상태와 컴포넌트들을 소유한다. Viewer는 현재 MeshComponent들을 렌더링하며 backend는 Scene 포인터를 장기 보관하지 않는다. 초기 단일 ModelInstance 소유 PosedGeometry는 구현 이력이며, 14절에서 컴포넌트별 소유로 이전했다. 별도 CutGeometryInstance는 추가하지 않는다.

### 7.2 어느 데이터를 복사하는가

1. **요청 고정:** `Scene::Update` 이후 동일 프레임의 본 팔레트, mesh/node 변환, GameObject TRS, `CutPlaneData`를 고정한다. GPU 실행 중 다음 프레임 자료로 덮어쓰지 않는다. 본 팔레트 등 작은 제어 데이터 업로드와 geometry 전체 복사는 구분한다.
2. **patch 선택:** 원본 joint/weight를 읽어 선택 joint의 weight가 세 정점 중 하나라도 0보다 큰 삼각형을 고른다. 선택 대상은 명시된 joint 집합이며 자손 포함은 자동 가정하지 않는다. 전역 joint ID를 mesh palette로 대응시킨다.
3. **삼각형 목록 작성:** `(meshId, drawId, triangleId)`와 원본 정점 참조를 GPU에 기록한다. 영향이 0인 나머지 두 정점도 삼각형 전체를 위해 포함한다. 원본 VB나 렌더링용 posed cache 전체를 절단 입력용으로 다시 복제하지 않는다.
4. **선택 영역 위치 참조:** 현재 구현하는 렌더링 공용 cache에서 선택 삼각형의 posed local 위치를 읽는다. 이 cache는 원본 surface VB의 **모든 joint 영향**으로 스키닝한 뒤 node 변환을 반영한다. 선택 joint만으로 스키닝하면 현재 화면의 위치와 달라진다. static은 스키닝만 생략한다. 선택 영역만 계산하는 전용 경로는 공용 전체 cache와 다른 후속 최적화다.
5. **절단 출력 생성:** 월드 평면을 객체 로컬로 변환하여 posed local 위치와 비교하고, 양쪽 표면의 위치·UV·normal/tangent·출처 및 인덱스를 별도 UAV에 쓴다. GPU 연산으로 추출·생성하는 과정이며 단순 `CopyBufferRegion`으로 구현할 수 없다.

첫 경로는 선택 삼각형별 3개 코너를 scratch에 기록할 수 있다. 중복 정점 계산을 허용해 remap·전역 용접 없이 검증한다. seam 정점을 위치만으로 병합하지 않는다. 선택 영역이 커서 비용이 확인되면 원본 vertex ID 기반 선택 영역 remap을 추가한다. 동일 위치라도 UV·normal·skin이 다른 렌더 정점은 구분한다.

반복 사용되는 원본 patch의 삼각형 ID 목록은 자산+선택 joint 집합 기준으로 캐시할 수 있다. 현재 pose 위치·출력 버퍼·원본 마스크는 인스턴스별이다. 이미 절단된 revision의 재절단은 원본 patch 캐시만으로 처리하지 않는다.

### 7.3 최종 위치로 자르고 객체 로컬 위치로 저장한다

판정할 정점은 **현재 렌더와 같은 pose**를 사용한다. 저장 좌표는 **객체의 posed local 공간**으로 통일한다. 이는 bind local과 다르며, 이미 스키닝·내부 node 변환이 반영된 좌표다. 후속 교차 계산은 월드 평면을 해당 객체 로컬로 옮겨 이 버퍼를 그대로 읽는 안을 채택한다. 투영 좌표는 저장하지 않는다.

`O`는 초기 구현의 GameObject 월드 행렬, `N`은 node pose, `S(p)`는 기존 스키닝이다. 컴포넌트 재설계에서는 절단 평면 변환에 `O` 대신 대상 컴포넌트의 실제 월드 행렬 `Wc`를 사용하고 정점은 13절의 component-local 출력으로 통일한다. 아래 식은 컴포넌트 변환과 SourceFrame이 identity인 경우다:

```text
static:   pCutLocal = N * pBind
skeletal: pCutLocal = N * S(pBind)
평면:     PlaneInObject = transpose(O_atCut) * WorldPlane
판정:     dot(PlaneInObject, float4(pCutLocal, 1))
유한 범위: (WorldToPlane * O_atCut)로 평면 로컬에 옮겨 XZ 반크기 검사
결과 draw: pWorldNow = PieceTransformNow * pCutLocal
```

`WorldPlane`이 정규화되어 있으면 위 dot 값은 원래 월드 부호 거리와 같다. 비균일 scale에서도 이 성질을 유지하려면 변환된 평면식을 다시 정규화하지 않는다. 허용오차는 월드 거리 단위로 정하고, 표시·선택과 같은 평면 로컬 Width/Height를 사용한다. 월드 정점과 월드 평면의 비교는 동일성 검증용으로 사용할 수 있다.

위 식은 기존 `ModelInstance::Update`와 VS의 연산 순서를 재현하는 계약이다. 스켈레톤 자산 해석을 바꾸거나 `N`을 임의로 제거하는 변경은 별도 검증한다. 같은 source-local 프레임으로 출력하면 두 결과 컴포넌트의 초기 월드 행렬은 모두 요청 시점 대상 `Wc`다. 프레임을 바꿔 저장하면 정점과 행렬을 함께 재기준화해 게시 직후 위치를 유지한다. 초기 pivot도 원본 객체 pivot을 유지하고, 조각 중심으로 옮길 때는 정점과 TRS를 함께 재기준화한다.

결과 draw는 posed geometry 전용 경로를 사용해 기존 mesh node 변환과 VS 스키닝을 재적용하지 않는다. normal은 실제 렌더의 skin normal·WorldIT 연산을 재현한다. `L`이 `O_atCut`의 선형 부분이면 저장 법선은 `nCutLocal = normalize(transpose(L) * nWorld)`이며, 결과 draw는 `inverseTranspose(PieceTransformNow.linear)`로 월드 법선을 복원한다. 비균일 scale에서도 위치와 법선에 같은 행렬을 곱하지 않는다. tangent와 handedness도 별도 검증한다.

### 7.4 두 결과의 실제 저장 형태

```text
공유 Model 원본 (읽기 전용)
  ├─ 선택 영역 밖 삼각형 → 소속에 따라 A/B 원본 참조 section
  └─ 선택 patch → posed 위치 → 절단 → A 표면 ──┤ 몸체 A
                                  └→ B 표면 ──┤ 분리 조각 B
                          cap A / cap B → 각각의 결과
```

- **몸체 A:** 원본 VB를 참조하는 A 소속 삼각형의 필터된 IB + 새 A 표면 VB/IB + 필요 시 cap A. 원본과 생성 section은 별도 draw이므로 하나의 공통 VB에 억지로 합치지 않는다.
- **분리 조각 B:** 원본 VB를 참조하는 B 소속 삼각형의 필터된 IB + 새 B 표면 VB/IB + 필요 시 cap B. 원본 material/texture는 참조로 공유한다. 새 cap은 별도 section/material을 사용한다.
- **공통 출처:** 생성 표면에 원본 mesh/draw/triangle 및 보간 계수를 연결한다. 경계 정점은 A/B가 독립 소유하고, 두 cap은 서로 반대 winding/normal을 갖는다.
- **원본 제거:** patch에서 뽑은 삼각형은 모두 기존 draw에서 제외하고 A/B 출력으로 대체한다. 실제로 교차하지 않은 선택 삼각형도 해당 쪽에 온전히 출력한다. 교차 삼각형만 숨기면 중복 표면이 생긴다.

영역 밖의 정점 payload는 복사하지 않지만, 기존 draw가 삼각형 제외 기능을 제공하지 않으므로 남은 인덱스 목록을 만드는 GPU scan/compaction은 필요할 수 있다. 따라서 선택 영역만 생성한다는 말이 전체 입력 검사나 원본 크기에 비례한 인덱스 저장까지 없앤다는 뜻은 아니다. 모든 mesh를 재구성하지 않고 영향받는 draw부터 필터링한다. 원본 경로와 생성 경로가 맞닿는 경계는 같은 프레임·같은 변형 규칙을 사용해야 한다.

필터된 원본 IB는 draw별로 원래 인덱스 값을 유지한다. 원본 mesh의 VB offset/stride와 해당 draw의 `baseVertex`를 그대로 사용하고, 새 draw의 `startIndex`는 필터된 IB 내 시작점으로 바꾼다. 인덱스 형식도 해당 원본 draw와 동일하게 유지한다. 제외 조건은 `(meshId, drawId, triangleId)`의 patch 포함 여부다. 생성 A/B IB는 별도 출력 VB 기준 0-based `uint32`로 통일하며 원본 `baseVertex`를 재적용하지 않는다.

논리적으로 결과는 두 메시지만 각각 단일 VB/IB일 필요는 없다. 단일 독립 asset으로 내보내야 할 때만 별도 flatten 작업으로 원본 참조 부분까지 복사한다. 이때에도 UV seam·material section을 보존한다.

### 7.5 절단 후 애니메이션과 부분 절개

현재 pose를 복사한 것만으로 지속 애니메이션은 해결되지 않는다. B를 고정 pose로 bake하는 초기 제안은 선택 모드로 남기고, 현재 재사용 구조는 하나의 ModelInstance가 평가한 pose를 공유하고 각 MeshComponent가 자체 recipe와 PosedGeometry를 평가하도록 설계한다. 애니메이션 유지와 실제 랙돌 물리는 별도 기능이다.

몸체가 계속 움직일 경우 생성 정점은 보간된 joint/weight만으로 표현하지 않는다. 예를 들어 교차점은 매 프레임 `lerp(PosedOriginal(v0), PosedOriginal(v1), t_atCut)`으로 재평가할 출처를 유지한다. UV 등 절단 당시 속성과 현재 위치의 재평가를 구분한다. 원본 section과 생성 section은 동일 pose revision을 사용한다. cap의 변형·법선 재계산과 재절단 출처 표현은 별도 설계가 필요하며, 전용 출처가 정의되기 전에는 지속 skeletal 절단을 지원 완료로 표시하지 않는다.

여기서 `PosedOriginal(v,t) = N_mesh(t) * S_mesh,t(pBind_v)`는 객체 locator를 제외한 posed local 위치다. 생성 표면은 절단 당시 고정한 출처 계수로 이 위치들을 보간해 매 프레임 출력 VB를 갱신하고, draw에서 현재 객체 TRS를 한 번 적용한다. 분리 조각 B의 bake 모드는 같은 출처가 있어도 위치를 갱신하지 않는다. 실시간 A와 frozen B의 모드는 revision의 명시적 설정으로 보관한다.

첫 검증 모드는 원본 참조 section을 포함한 **객체 전체 pose 동결**로 정합성을 확인할 수 있다. 원본 section만 애니메이션하고 생성 section은 절단 pose에 고정하는 혼합은 경계가 벌어지므로 허용하지 않는다. 이는 임시 검증 모드이며 최종 애니메이션 정책 확정을 대신하지 않는다.

유한 평면이 patch를 부분적으로만 통과하면 열린 절개가 될 수 있다. 이 경우에도 닫힌 두 조각과 cap이 생긴다고 가정하지 않는다. 두 메시 분리 모드는 patch 경계에서 A/B가 영역 밖 몸체와 어떻게 연결되는지, 절단선이 닫혔는지 검증하고 분리가 성립하는 요청만 게시하는 안을 제안한다. 미지원 부분 절개는 명시적 상태를 반환하고 원본을 유지한다. 유한 범위 밖까지 무한 평면으로 잘라 성공을 꾸미지 않는다.

### 7.6 게시·메모리·Undo 계약

- 출력은 A/B 별도 논리 자원을 기본으로 하고 scratch만 pool에서 재사용한다. 한 allocation의 subrange 최적화는 삭제·Undo 수명 관리가 검증된 뒤 고려한다.
- count pass로 필요한 출력량을 구하거나 제한 용량과 overflow 상태를 둔다. cap 용량은 선택 삼각형 수의 단순 상수배라고 보장하지 않는다. 상한 초과 시 부분 결과를 활성화하지 않는다.
- 원본은 GPU 후보 결과를 만드는 동안 계속 표시한다. 렌더 queue 의존성과 완료 상태를 확인한 뒤 A 컴포넌트 revision 교체·B 컴포넌트 생성·원본 patch 제외를 한 Scene 연산으로 게시한다. 기본·그림자·표시 모드가 같은 revision을 사용한다.
- job은 source ObjectId와 ComponentId, geometry revision, pose version, TRS version, plane version을 가진다. 이 version은 현재 API가 아니라 새 계약이며 Viewer 연결부가 각 상태 변경 시 발급한다. 첫 경로는 이 값이 달라지면 게시를 취소한다. 다음 프레임 애니메이션으로 매번 무효화되는 문제는 명시적인 요청 pose 동결로 처리하며, 실시간 연속 pose 게시는 후속 계약이다.
- Undo는 이전 A 컴포넌트 revision과 B 컴포넌트 생성 정보를 보존하고 A 복원·B 제거를 하나의 소유 객체 연산으로 처리한다. 자산 전체를 매번 복사하지 않는다. 재절단은 활성 revision을 입력으로 받아 원본 참조와 생성 section을 모두 다룬다. 첫 구현은 재절단을 거절할 수 있으며 원본 bind mesh로 조용히 되돌아가지 않는다.
- 객체 삭제/Undo 후에도 GPU가 사용하는 자원은 fence까지 강한 참조로 유지한다. pooled scratch와 출력의 재사용 시점을 분리한다. Undo가 잡은 revision도 메모리를 차지하므로 이력 수와 GPU 메모리 예산을 함께 제한한다.
- 작은 비동기 count/status/bounds 읽기는 허용한다. geometry 전체 readback을 필수로 하지 않는다. 결과 bounds는 GPU reduction 등으로 갱신하고 준비 전에는 보수적인 원본 bounds를 사용해 잘못 컬링하지 않는다.

이 계약의 검증 항목은 같은 Model을 공유하는 두 객체 중 하나만 절단, 다른 TRS·다른 pose, 16/32-bit index와 baseVertex, multi-material, UV seam, patch 경계의 무영향 정점, 비균일 scale, A/B 게시 실패·취소·Undo·삭제다. 실제 구현은 입력 decode → 선택 patch 위치 준비 → cap 없는 A/B 생성 → 원본 제외와 게시 → cap/지속 애니메이션으로 나눠 각 단계 약 300줄에서 멈춘다. 현재 9절의 static 최소 검증은 유지하고 skeletal 선택 경로를 추가 검증한다.

## 8. 문헌이 말하는 범위

자세한 기록은 [참고자료.md](참고자료.md)를 따른다.

| 문헌 | 확인된 내용 | 적용 한계 |
|---|---|---|
| Alawneh/Peters, *2D Triangulation of Polygons on CUDA* (2013), DOI `10.1109/HPCSim.2013.6641459` | polygon마다 GPU thread 하나, 각 polygon 내부 ear clipping은 직렬 | 단일 작은 cap 내부의 병렬 가속 근거가 아님. hole 지원은 확인되지 않음 |
| Eder/Held/Palfrader, *Parallelized ear clipping for triangulation and constrained Delaunay triangulation of polygons* (2018), DOI `10.1016/j.comgeo.2018.01.004` | CPU/OpenMP에서 ear 제거 병렬화, hole bridge 전처리 | GPU 구현 논문이 아님. HLSL 동기화·메모리·수치 처리는 별도 설계 |

첫 논문은 [저자 PDF](https://www.engr.mun.ca/~dpeters/papers/AlawnehPeters13a_hpcs2013.pdf), 둘째는 [출판사 페이지](https://www.sciencedirect.com/science/article/pii/S092577211830004X)에서 확인할 수 있다. 두 논문이 GPU 절단선 연결부터 hole 구성까지 해결한다고 해석하지 않는다.

NDC 관련 주장은 [NDC25Reference.md](../NDC/NDC25Reference.md)의 근거 등급을 따른다. 이중메시의 실제 채택이나 GPU 처리 여부가 확인되지 않은 부분은 확정 구현 사실로 쓰지 않는다.

## 9. 구현 전 결정과 진행 순서

우선 결정할 항목은 좌표/pose 공유, 입력 decode, 출력 용량/할당, cap 영역·재질, 지속 애니메이션 출처, 마스킹 및 비동기 수명 계약이다. GPU cap의 안정성과 성능은 아직 검증되지 않았다.

1. 원본 GPU 입력 decode 및 static/skeletal Compute pose·렌더 공유 검증 (현재 구현은 11절).
2. static 삼각형 절단과 결과 draw 검증. 이 단계는 cap 없는 제한 기능으로 표시.
3. 절단선 연결과 simple cap 검증.
4. 기존 skeletal GPU pose에 선택 본 patch 및 절단 통합.
5. hole/영역/재질과 지속 애니메이션 요구를 각각 검증.
6. 원본 마스킹, Undo, 본 분리, overflow와 GPU 자원 수명 통합.

각 항목도 한 개념·약 300줄의 변경 단위로 나눠 사용자가 읽고 commit한 뒤 다음 단계로 진행한다. 전체 GPU 시스템을 한 번에 작성하지 않는다.

각 단계 시작 시 범위와 예상 시간을 다시 제시한다. 알려진 참고값은 이전 작업의 증분 Debug 빌드 약 20.6초, Release 약 26.84초와 Codex 검토 약 2–5분뿐이다. 이는 새 GPU 알고리즘 구현 시간이나 전체 clean build 예측이 아니다. 새 경로의 실제 측정으로 갱신한다.

## 10. 검증 기준

- 단일 삼각형의 부호·교차점·출력 winding/면적을 확인한다.
- 닫힌 단면의 연결, 열린 입력의 실패 정책, seam·중복점·퇴화 입력을 확인한다.
- 관의 hole과 내부가 채워진 중첩 solid를 구별하는 테스트를 둔다.
- static/skeletal의 동일 공간, 동일 프레임 pose와 재질 보존을 확인한다.
- 지속 애니메이션, cap 재절단, 원본 마스킹, Undo 및 취소 후 지연 완료를 확인한다.
- GPU validation, overflow 및 fence 수명을 검사하고 Debug/Release를 빌드한다. GPU·driver·재현 조건을 기록한다.

보관 CPU 구현은 비교 자료로 사용할 수 있지만 모든 입력의 정답으로 취급하지 않는다. endpoint 허용오차와 알고리즘이 다르므로 bit 단위 동일성 대신 면적·경계 연속성·의도된 영역과 허용오차를 검증한다. GPU 전환의 성능 향상은 측정 전 약속하지 않는다.

## 11. Compute pose와 렌더 공유: 초기 검증과 현재 경로 (2026-10-24/25)

기준 `4bc5cdf9` 이후 미커밋 변경이다. 아래 1·2단계는 각각 Luna 독립 리뷰→수정→재검토를 수행했다. 별도 독립 리뷰에서도 전체 데이터 흐름의 중복 변환·소유권·실행 순서를 확인했다. 실제 삼각형 선택·절단·cap은 이번 구현 범위에 없다.

### 11.1 두 구현 단계

1. **컴포넌트별 결과 버퍼와 Compute:** 최초에는 ModelInstance가 소유했으며 14절 구현에서 `MeshComponent`가 `unique_ptr<PosedGeometry>`를 소유하도록 이전했다. `PosedGeometry::Update`는 `PreparePoseCS`로 원본 surface VB를 읽어 위치(float3)·법선(float3)·탄젠트(float4)를 정점당 40바이트로 저장한다. mesh별 구간과 원본 정점 번호를 유지한다. static은 node 변환, skeletal은 기존 VS와 같은 전체 joint 영향 스키닝 후 node 변환을 수행한다. 현재는 그 앞에 source 프레임 역행렬을 합성해 component-local 출력을 만든다. CPU의 애니메이션 평가·본 행렬 생성은 유지한다.
2. **버텍스 셰이더의 입력 전환:** `MeshComponent::Render`와 `MeshSorter`가 컴포넌트별 결과 SRV를 전달한다. 초기 Model::Render 경로는 제거했다. `DefaultVS`·`DepthOnlyVS`는 `UsePosedVertices`가 켜지면 Compute 결과를 읽고 컴포넌트 Wc와 투영만 적용한다. 기존 UV·인덱스·재질·input layout·PSO 분류는 유지하며 color/depth/shadow/Wireframe/Vertices가 같은 위치를 읽는다.

```text
공유 Model::m_DataBuffer (읽기 전용)
  + CPU 현재 node/bone 행렬
    → PreparePoseCS: inverse(F)*N*p / inverse(F)*N*S(p)
    → MeshComponent별 PosedGeometry RAW40
    → VS: 컴포넌트 Wc → ViewProjection → 렌더

후속 절단: 동일 RAW40 + 컴포넌트 로컬로 변환한 유한 평면 → 교차 검사
```

`MeshConstants::ObjectWorld`/`ObjectWorldIT`는 컴포넌트 Wc와 그 역전치를 담는다. 원본 경로의 `World`/`WorldIT`는 비교용으로 보존한다. HLSL packoffset과 C++ offsetof/sizeof assert로 상수 위치와 256바이트 stride를 맞춘다. `SV_VertexID`에는 draw의 `baseVertex`가 포함되지 않으므로 `MeshSorter::RenderMeshes`가 draw마다 RAW SRV 시작점을 `meshOutputOffset + baseVertex * 40`으로 바인딩한다.

### 11.2 갱신과 수명

- 원본 geometry 전체를 결과 버퍼에 먼저 복사하지 않는다. Compute가 packed 원본을 읽어 결과를 생성한다. 이 단계의 결과는 **렌더링을 위한 전체 정점 pose cache**이며, 선택 joint patch만 저장하는 절단 결과와는 별도다.
- target Wc는 저장 좌표에 포함하지 않으므로 target TRS만 바뀌어도 cache를 다시 계산하지 않는다. SourceFrame 변경은 local node 행렬에 반영된다. joint가 없는 모델은 node pose가 같으면 재사용한다. skeletal은 매 프레임 갱신한다. mixed 모델의 static mesh까지 재계산하는 비용은 후속 최적화 대상이다.
- 같은 graphics queue에서 Compute 기록→UAV에서 NON_PIXEL_SHADER_RESOURCE로 전이→렌더 순서를 지킨다. 아직 async compute나 별도 queue 공유는 없다.
- 버퍼는 생성 시 한 번 할당하고 매 프레임 재사용한다. 컴포넌트 삭제/geometry 교체 때 `PosedGeometry` 소멸자가 `IdleGPU`를 기다린 뒤 해제한다. 첫 구현의 보수적인 전역 대기이며 지연 해제 큐 최적화는 미구현이다.
- empty 모델은 출력 할당·dispatch를 생략한다. 지원하지 않는 packed layout/범위는 예외로 거절하며 임의로 decode하지 않는다. 큰 정점 수는 2D dispatch로 차원 제한을 지킨다.

### 11.3 재현과 검증

작업 디렉터리는 `MiniEngine/ModelViewer`다. 기본은 Compute 경로이며 `-compute-pose 0`은 비교용 기존 VS 경로다. `-capture <외부 prefix> -capture-frame 60 -animation-step-ms 16`은 16ms씩 Scene 애니메이션을 갱신한 60번째 렌더 프레임의 HDR/post를 저장한다. 캡처 옵션이 없으면 고정 시간 갱신을 사용하지 않는다.

- 단계 1: Compute를 실행하면서 기존 VS로 그리는 static 기본 장면·CesiumMan에서 실제 HDR/post 캡처와 GPU 완료 확인, 새 D3D12 오류/assert 없음.
- 단계 2: Debug 빌드 57.19초, 최종 Release 증분 빌드 16.73초, 모두 exit 0. 기존 SceneMathTests 457개 검사 통과.
- GPU NVIDIA GeForce MX450, 드라이버 `32.0.15.9186`, `-debug 1`, 1920×1080. 동일 프레임 기존 VS/Compute HDR 비교: static 8픽셀, CesiumMan 1픽셀, Xbot 3픽셀 차이/총 2,073,600픽셀. 최대 선형 채널 차이는 각각 0.0078125 / 0.0001220703125 / 0.0234375이며 전체 평균 차이는 5.81e-9 / 1.96e-11 / 1.04e-8이다. 형상과 조명은 실측 캡처에서 기존 출력과 일치하는 수준이며 bit-identical을 주장하지 않는다.
- CesiumMan의 고정 16ms step, frame 30/90에서 14,789픽셀이 바뀌어 애니메이션 자세 갱신을 확인했다. 캡처를 직접 보아 static와 skeletal 형상을 확인했다. 기존 경로에 이미 있는 재질/텍스처 문제의 수정이나 물리 동작 검증은 이번 범위가 아니다.
- 원본 cube를 복사해 저장소 밖에 만든 multi-draw fixture는 `baseVertex` 0/8의 두 draw를 사용한다. 기존/Compute HDR 차이는 4픽셀, 최대 채널 차이 0.00390625로 시작점 보정을 검증했다. static·CesiumMan의 Wireframe/Vertices도 실제 캡처를 완료했다. Release static·CesiumMan HDR은 같은 시점 Debug HDR과 byte-identical이다.
- 새로운 D3D12 ERROR/CORRUPTION·device removal·assert·처리되지 않은 예외는 없었다. Release에는 기존 RenderModes 검증 이력에도 있는 DXGI #80(하이브리드 GPU 출력 조회), D3D12 경고 #974/#1008이 남아 있다. 기존 진단을 모두 해결했다고 주장하지 않는다.
- 픽셀 검증은 실제 GPU backbuffer 이전의 HDR/post resource readback을 사용했다. 자동화 도구에 실행 창이 노출되지 않아 키·마우스 조작 및 정상 종료 경로는 미검증이다. 캡처 helper는 GPU 완료·캡처 marker 뒤 자신이 실행한 프로세스를 종료하므로 이 결과를 정상 종료의 증거로 쓰지 않는다. 자원 해제 순서는 코드 리뷰로 확인했다.
- 로그·RAW·PNG·CPU 검사와 임시 multi-draw 모델은 저장소 밖 `%TEMP%/pose-validation/`에 둔다. Debug/Release 빌드 로그는 `%TEMP%/pose-*-build.log`다. 실행 생존만으로 성공 판정하지 않고 캡처 완료와 실제 픽셀을 확인한다.

선택 joint patch, 실제 평면 교차, 두 결과 메시 게시, cap, 지속 절단 애니메이션은 후속 작업이다. 전체 애니메이션 평가의 GPU 이전·GPU 성능 향상도 이번 완료 항목이 아니다.


## 12. Static/Skeletal 공통 Geometry와 NDC 방식 적용 설계

### 12.1 근거와 범위

[NDC 참고 정리](../NDC/NDC25Reference.md) 4절의 확인 내용은 선택 본 영향 영역 복사→복사본 절단→BreakConstraint·원본 영역 숨김이며, 6절에는 프로시저럴 메시의 수동 스키닝과 여러 본 영역의 병합 후 절단이 기록돼 있다. 정확한 정점/삼각형 포함 조건, 본 선택법, 기여 교체의 좌표식·cap 부착은 확인되지 않았다. 아래 구조·GPU 구현·분리 정책은 우리 프로젝트의 설계안이며 NDC의 동일 구현으로 주장하지 않는다.

범위는 관절 주변의 국소 절단부터 시작한다. 하나의 joint 영역만으로 몸통 세로 절단·다중 본 절단·아일랜드 분리를 일반 해결한다고 가정하지 않는다. CurrentGeometry의 원본 section revision은 14절에 구현했다. 관절 분리, 생성 절단 결과 및 지속 애니메이션 출처는 아직 미구현이다.

### 12.2 객체 데이터

13절의 한 GameObject·여러 MeshComponent 구조를 사용한다. 원본 Model과 애니메이션·본 pose는 ModelInstance 하나에 두고, CurrentGeometry와 PosedGeometry는 각각의 MeshComponent가 소유한다. ColliderComponent는 대상 MeshComponent를 ID로 참조한다. 소유·렌더 기반은 14절에 구현했다. 생성 절단 geometry와 선택 본별 변형 정책은 설계 상태다.

### 12.3 Static 절단 시 이동

1. ObjectId+ComponentId로 대상을 찾고 현재 Geometry·PosedGeometry·실제 월드 변환·평면을 같은 시점에 고정한다. 첫 절단의 현재 Geometry는 공유 원본 section 참조다.
2. 평면을 posed local 공간으로 옮긴다. section AABB로 한쪽 section은 그쪽 결과에 참조로 배정하고, 걸친 section만 삼각형별로 검사한다. AABB는 교차 검사를 줄이며 한쪽 표면을 결과에서 삭제하는 조건이 아니다.
3. 공통 절단기가 현재 위치·렌더 속성을 읽어 A/B 표면과 교차 정점·출처·인덱스를 생성한다. 지원되는 닫힌 경계는 cap을 생성한다.
4. Geometry A/B를 만들고 기존 MeshComponent에 A를, 같은 GameObject의 새 MeshComponent에 B를 연결한다. 두 컴포넌트는 같은 ModelInstance·원본 Model을 참조하며 CurrentGeometry·PosedGeometry는 따로 갖는다. 초기 배치는 source 프레임을 보존하거나 정점과 변환을 함께 재기준화한다.
5. 다시 자를 때 현재 Geometry A 또는 B를 읽는다. 원본 참조와 생성 section을 한 논리 입력으로 처리하며 원본 Model 전체로 되돌아가지 않는다.

### 12.4 Skeletal 절단 시 이동: 원본 A/B + 절단 패치 A/B

사용자의 세 단계는 원본의 관절 분리·패치 영역 교체→선택 영역 복사본 절단→양쪽 원본/패치 결합이다. 실제 게시 전까지는 다음처럼 후보 결과로 보관한다.

1. **분리 전 pose 보관:** 현재 원본 Geometry와 PosedGeometry, 본·node 행렬, 애니메이션 시간, TRS, 평면을 고정한다. 선택 patch의 현재 위치는 이 snapshot에서 복사한다. BreakConstraint 후 늘어진 위치를 새 절단 입력으로 쓰지 않는다.
2. **영역과 소속 선정:** 선택 joint 영향 patch P, 분리할 본 subtree D, D를 제외한 전체 나머지 본 K를 구분한다. P는 절단 주변의 교체 영역이고 D는 원본의 어느 부분이 분리 조각을 따라가는지 결정하는 범위다. 둘을 동일 집합으로 취급하지 않는다. 선택 joint의 자손 포함 규칙과 palette/global joint 대응을 명시한다.
3. **원본 A/B 참조 구성:** patch 밖에서 삼각형 전체의 양수 weight 영향이 K에만 있으면 A, D에만 있으면 B에 배정한다. K와 D 양쪽 영향이 있는 patch 밖 삼각형은 patch 확대 또는 요청 거절로 처리한다. 손처럼 patch 밖의 D 영역도 B에 포함한다. 본 분리만으로 이 membership 목록이 자동 생성되지는 않는다.
4. **P만 추출·절단:** 원본 joint/weight는 범위 판단에 쓰고, 현재 위치는 snapshot PosedGeometry에서 읽는다. 필요한 위치·UV·normal/tangent·원본 출처만 작업 공간에 추출한다. 전체 원본 geometry를 복사하지 않는다. 공통 static 절단기가 패치 A/B와 필요 시 cap을 생성한다. AABB는 P의 section을 빠르게 분류하는 데 사용하며 P의 한쪽 표면도 결과에 유지한다.
5. **결합:** Geometry A = 원본 A 참조 + 패치 A + cap A, Geometry B = 원본 B 참조 + 패치 B + cap B. 이것은 먼저 section을 같은 논리 Geometry로 묶는 작업이다. 위치만 같은 UV seam 정점의 전역 용접이나 VB 전체 병합을 필수로 하지 않는다. 접합 경계의 현재 위치·후속 변형은 별도 정합성 검사를 한다.
6. **함께 게시:** 결과가 유효하고 GPU 의존성이 준비된 뒤 본 분리 상태, 원본 patch 제외 목록, Geometry A/B, MeshComponent B 및 대응 collider 생성, bounds를 함께 적용한다. 그 전에는 이전 상태를 렌더링한다. BreakConstraint 직후 먼저 원본을 숨겨 중간 프레임에 구멍을 보이지 않는다. 기본적으로 GameObject는 추가하지 않는다. 독립 Actor식 수명·게임 로직이 필요할 때 조각을 별도 객체로 이전하는 것은 후속 기능이다.

원본을 숨기는 목록은 추출한 원본 삼각형 목록과 일치해야 한다. 재절단에서는 현재 `(revision, section, triangle)` 식별자로 교체 대상을 관리하고, 원본 삼각형 출처 ID만으로 모든 자손 표면을 지우지 않는다.

### 12.5 애니메이션·재절단에 필요한 추가 계약

- BreakConstraint는 Unreal에서 물리 관절 분리이며 MiniEngine에 같은 기능이 현재 존재하지 않는다. 우리 엔진의 본 분리·subtree 이동과 실제 랙돌 물리를 구분한다. GameObject를 둘로 만드는 것만으로 혼합 weight의 늘어짐이 해결되지 않는다.
- A/B 각각의 본 기여 정책이 필요하다. A는 남은 본의 현재 pose를 사용하고 분리 본의 영향은 절단 시점 기준으로 남은 쪽 attachment를 따라가게 하는 안, B는 분리 subtree 기준으로 pose를 평가하고 반대쪽 기여를 분리 조각 기준 절단 snapshot에 고정하는 안을 제안한다. 절대 월드 좌표 동결과 상대 attachment 동결은 다르다. 정확한 행렬 공간·inverse bind·locator 단일 적용 식은 구현 전 독립 수학 검증으로 확정하며 NDC 확인식으로 쓰지 않는다.
- 생성 교차점은 단순한 새 joint/weight 보간 대신 `Blend(sourceA, sourceB, t_atCut)` 출처로 보관한다. 각 side의 보정된 pose에서 source 위치를 재평가한 뒤 보간한다. 기존 Compute의 원본 decode·스키닝과 렌더 RAW40 형식은 재사용하되, pose 정책과 생성 정점 평가 dispatch는 추가해야 한다.
- 재절단은 기존 생성 표면을 포함한 CurrentGeometry를 입력으로 하고 새 교차점이 기존 출처를 참조한다. 과거 절단의 보간 계수·pose 고정 정책을 덮어쓰지 않고 revision별 불변 출처/정책 의존성을 유지한다. GPU에서는 재귀 대신 의존 순서에 따른 dispatch가 필요하며 깊이·메모리 상한을 둔다.
- 원본 출처의 joint support는 양수 weight의 joint 집합, Blend의 support는 양수 계수로 실제 기여하는 부모 support 합집합으로 정의하는 안이다. 새 표면의 다음 patch 선택에 이 정보를 사용한다. threshold와 cap의 eligibility는 조용히 추정하지 않는다.
- cap의 위치 출처·본 영향·변형과 normal/tangent 갱신 정책을 확정하기 전에는 지속 애니메이션 cap 및 cap 재절단을 명시적으로 거절한다. section 목록에 cap이 있다는 것만으로 재절단 지원을 주장하지 않는다. 여러 기존 patch가 걸치는 요청은 한 논리 입력으로 함께 잘라야 한다.
- 유한 평면이 patch의 원본 접합 경계에 걸리거나 열린 절개가 되면 닫힌 A/B를 보장하지 않는다. 첫 범위는 경계가 안전한 국소 절단으로 제한하고 실패 시 이전 geometry·pose를 유지한다.

공통 backend는 현재 geometry의 posed 삼각형 절단과 결과 section 생성이다. Skeletal adapter가 joint 영향 영역, 원본 A/B 소속, 본 분리·기여 정책을 담당한다. 따라서 Static을 먼저 검증한 절단기를 재사용하되 스켈레탈의 모든 특수 처리를 그 절단기 안에 넣지는 않는다.


## 13. 재설계: 같은 GameObject 아래 여러 MeshComponent

### 13.1 기준과 컴포넌트 수

[Unreal Slice Procedural Mesh 공식 문서](https://dev.epicgames.com/documentation/unreal-engine/BlueprintAPI/Components/ProceduralMesh/SliceProceduralMesh)는 반대쪽 ProceduralMeshComponent의 owner가 원래와 같다고 명시한다. 여기서 따르는 것은 소유 구조이며 MiniEngine에 Unreal 컴포넌트/Chaos가 이미 있다는 뜻이 아니다. Static/Skeletal 모두 기본 절단은 GameObject 수를 늘리지 않고 MeshComponent 수를 늘린다.

```text
GameObject (ObjectId, 소유 객체 Transform)
  ModelInstance 한 개
    원본 Model 참조, 애니메이션·node/bone pose, 본 분리 상태
  MeshComponent A (ComponentId)
    Local/World 변환, 변환 모드, DeformBinding
    CurrentGeometry -> 불변 GeometryRevision
    PosedGeometry -> 이 컴포넌트의 RAW40 GPU 출력
  ColliderComponent A -> 대상 MeshComponent A의 ID
  MeshComponent B (ComponentId)
    독립 변환·DeformBinding·CurrentGeometry·PosedGeometry
  ColliderComponent B -> 대상 MeshComponent B의 ID
```

반복 절단으로 A/B/C 등 컴포넌트가 늘어난다. section은 한 컴포넌트 내부의 재질·draw 구분이며 자체 조각 Transform이나 ComponentId를 갖지 않는다. 절단 패치와 cap은 보통 조각 MeshComponent 내부의 section으로 저장하고 각각을 별도 GameObject로 만들지 않는다. 범용 ECS나 임의 타입 컴포넌트 registry는 이번 설계의 필수 조건이 아니다.

### 13.2 공유·개별 소유와 기존 코드 이전

- ModelInstance 하나가 원본 Model과 애니메이션 재생 시간·원본 node/joint pose를 관리한다. Static에는 본 평가를 생략한다. 조각마다 전체 애니메이터나 원본 VB/IB를 복사하지 않는다.
- MeshComponent마다 현재 section 목록·geometry revision, 정점 출처, 변형 정책, 로컬 경계, PosedGeometry를 둔다. 원본 A+패치 A+cap A는 A의 section 목록, 원본 B+패치 B+cap B는 B의 section 목록이다.
- 같은 본 pose를 읽더라도 반대편 본 기여를 고정하는 정책은 조각별이므로, MeshComponent별 DeformBinding/recipe 평가와 필요한 보정 팔레트가 있어야 한다. 공유 본 배열을 A용으로 덮어써 B까지 바꾸지 않는다.
- 14절 구현에서 ModelInstance의 m_PosedGeometry와 원본 전체 Render 책임을 MeshComponent로 이전했다. 기존 원본 모델은 최초 MeshComponent 하나로 표현한다. ModelInstance의 모호한 '현재 geometry/pose 하나' 접근자를 유지하지 않고 대상 ComponentId를 명시한다.
- ColliderComponent는 후속 물리엔진 연결을 위한 shape 설명·충돌 필터·대상 메시 ID를 보관한다. 실제 Body/Constraint는 PhysicsWorld가 소유하고 컴포넌트는 엔진 등록용 참조만 갖는다. Static의 조각별 rigid body, Skeletal의 bone별 body/constraint 매핑을 구분하며 같은 bone body를 중복 생성하지 않는다. ModelInstance는 본 pose/분리 제어 상태만 관리하고 물리 solver나 body 자원을 소유하지 않는다. 실제 물리 시스템은 아직 미구현이다.

### 13.3 소유와 Transform 부모를 구분한다

같은 owner라는 이유로 분리 조각이 소유 객체 이동에 항상 끌려가면 안 된다. 컴포넌트의 변환 모드를 구분한다.

- Attached: owner 행렬 O와 컴포넌트 local 행렬 C로 `Wc = O * C`를 계산한다.
- WorldIndependent/Simulated: 조각 제어기나 physics body의 월드 Wc를 사용한다. O가 바뀌어도 Wc를 고정/시뮬레이션할 수 있다. local C가 필요하면 `inverse(O) * Wc`로 갱신하며 O를 추가 적용하지 않는다.
- Renderer, Collider, section AABB/picking, 절단 평면 변환은 같은 GetWorldTransform의 Wc를 읽는다. GameObject 컬링/후보 경계는 현재 컴포넌트들의 월드 경계를 합친다. animating bounds가 늦으면 보수적 경계로 누락을 방지한다.

원본 source posed 위치 Vm은 현재 코드와 같은 model/object-local `N*S(p)`다. DeformBinding이 source frame F를 정하고 Compute가 `Vc = inverse(F) * Vm`으로 component-local 출력을 만든다. source pose를 그대로 따라갈 때 `C=F`이면 `O*C*Vc = O*Vm`으로 기존 화면이 재현된다. Detached target Wc는 source F와 독립 제어하므로 사용자 이동/물리 이동이 상쇄되지 않는다. 임의로 편집한 현재 C의 역행렬로 매번 source를 되돌려 이동 효과를 지우지 않는다.

법선은 source-to-component 변환에 맞춰 `nC = transpose(F.linear) * nM`으로 옮기고 VS에서 Wc 역전치를 적용한다. 정규화/탄젠트 정책은 기존 표현과 맞춰 검증한다. F는 유한·가역이어야 한다. bone/node 변환은 Compute에서, target Wc는 VS에서 한 번만 적용한다. collider/body가 이미 사용한 본 변환을 별도로 다시 곱하지 않는다. Static의 최초 source frame은 identity로 시작할 수 있다.

### 13.4 절단과 재절단

1. Collider hit는 ObjectId와 대상 MeshComponentId를 반환한다. ObjectId만으로 절단 조각을 고르지 않는다.
2. 해당 컴포넌트의 CurrentGeometry·PosedGeometry·Wc·pose 정책을 snapshot한다. 평면은 Wc를 사용해 component-local로 변환한다.
3. Static은 현재 geometry를, Skeletal은 그 geometry에서 선택한 영향 영역을 공통 GPU 절단기에 넘긴다. 결과 A/B의 section·출처·pose 정책·bounds/collider 후보를 준비한다.
4. GPU 의존성과 결과 검증을 통과한 뒤 기존 ComponentId A의 geometry를 교체하고 새 ComponentId B를 같은 GameObject에 추가한다. source 프레임을 유지하면 양쪽의 초기 Wc는 요청 당시 대상 Wc다. Skeletal 조각별 프레임으로 바꾸면 정점도 함께 재기준화한다.
5. 재절단은 해당 ComponentId의 최신 geometry를 대상으로 한다. 다른 조각이나 최초 원본 전체로 돌아가지 않는다. 선택 P가 기존 여러 patch에 걸치면 한 논리 입력으로 모아 처리한다.

ComponentId는 owner 안에서 증가하고 제거 후 재사용하지 않는다. 결과 게시/Undo에는 A의 이전 revision·변환·정책, B의 생성/제거, collider 대응과 본 분리 상태를 포함한다. 삭제한 조각의 지연 GPU 결과를 다시 게시하지 않는다. GPU와 Undo가 참조하는 geometry는 각 수명 조건을 만족할 때 해제한다.

Scene::Update/Render, picking, gizmo, history, culling과 현재 단일 GetModelInstance 경로를 컴포넌트 단위로 확장해야 한다. 첫 구현에서는 GameObject 변환 편집과 조각 변환 편집을 명시적으로 구분한다. 현재의 pose buffer 구현만으로 이 소유 구조가 완료된 것은 아니다. 컴포넌트 이동과 Static 절단을 작은 단계로 검증한 뒤, 12.5절의 skeletal 기여 보정/지속 cap 요구를 별도 검증한다.


### 13.5 향후 물리엔진과의 경계

- 지금 절단 후보 탐색·컬링에 쓰는 Bounds/AABB와 실제 충돌 shape를 구분한다. AABB는 빠른 후보 검사이며 향후 물리엔진용 ColliderComponent를 완성한 것으로 취급하지 않는다.
- ColliderComponent는 MeshComponentId, source geometry revision, primitive/convex 등 shape 설명·필터를 보관하는 엔진 독립 컴포넌트로 설계한다. 특정 물리엔진 타입을 geometry GPU 버퍼나 렌더러에 직접 넣지 않는다.
- PhysicsWorld/연결 계층이 shape cooking·Body/Constraint 생성·등록·해제와 실제 simulation을 담당한다. 참조 유효성은 ObjectId+ComponentId+revision으로 검사한다. 실제 타입과 물리엔진 선택은 후속 작업이다.
- Static 동적 조각은 엔진이 지원하는 primitive/convex 등 충돌 표현을 선택하고, 렌더 삼각형 전체를 매 프레임 CPU로 복사하는 경로를 요구하지 않는다. 필요 데이터의 절단 시점 비동기 readback/cooking은 별도 정책이다. 렌더 mesh와 충돌 mesh의 해상도가 같을 필요는 없다.
- Skeletal은 PhysicsWorld의 본 body 결과를 CPU pose 입력에 반영한다. 조각 Collider가 같은 body를 다시 생성하거나 렌더에 본 변환을 두 번 적용하지 않는다.
- simulation 중 조각은 물리 월드 Wc가 변환의 기준이고 renderer·절단·질의 bounds가 그 상태를 읽는다. owner 변경/teleport와 일반 이동을 구분할 계약을 둔다.
- 절단 후 GPU geometry 갱신과 collider 재생성은 같은 geometry revision에 대응해야 한다. 비동기 cooking 중 이전/취소된 결과를 활성화하지 않으며 교체·Undo·삭제 시 물리 자원과 GPU 자원의 완료 조건을 각각 지킨다.
- 물리엔진이 없는 첫 절단 구현에서는 Bounds 기반 후보 탐색과 조각 관리까지만 제공한다. 자체 충돌 반응·중력·관절 solver를 임시 구현하지 않는다.


## 14. 다중 MeshComponent 기반 구현 (2026-10-25)

상태: 진행 중 — 기준 `4bc5cdf9` 이후 미커밋. 13절의 소유·렌더 기반을 구현했으며 실제 절단 backend는 별도다. 11절의 기존 객체별 pose 검증 이후 소유권이 아래와 같이 바뀌었다.

```text
GameObject (ObjectId, owner TRS)
  ModelInstance 1개: 공유 Model, CPU animation/node/joint pose
  MeshComponent N개: ComponentId, target TRS/Attached 또는 WorldIndependent
    DeformBinding: SourceFrame, FollowModel 또는 Frozen
    GeometryRevision: 공유 원본 section/VB/IB/재질/SourceMesh 참조
    PosedGeometry + mesh 상수: 컴포넌트 전용 GPU 자원
  ColliderComponent N개: ComponentId, 대상 MeshId, GeometryRevisionId
```

`ModelInstance::Update`에서 애니메이션과 node/joint pose를 owner당 한 번 평가한다. `MeshComponent::Update`는 같은 pose를 읽고 `inverse(SourceFrame)*node*skin(p)`를 컴포넌트 로컬 GPU 버퍼에 기록한다. VS는 그 버퍼에 Wc와 VP를 적용한다. source 프레임이 target 이동을 상쇄하지 않는다. 비교용 legacy VS 경로도 같은 프레임 보정 행렬과 공유/고정 본 팔레트를 쓴다.

`GeometryRevision::FromModel`과 `SelectSections`는 강한 원본 참조와 불변 section 목록을 만든다. VB/IB·재질은 원본의 구간을 참조하므로 GPU 원본 전체를 복제하지 않는다. `SetGeometry`로 현재 section revision을 교체하고 이전 snapshot을 복구할 수 있다. ModelInstance는 geometry·PosedGeometry·렌더 상수·locator·Render를 소유하지 않는다.

선택과 컬링은 컴포넌트 단위다. 현재 node와 모든 skin joint로 원본 mesh sphere를 변환·합산한 component-local bound를 Wc로 옮긴다. 정규화된 비음수 LBS는 이 합산 구 안에 들어가며 zero-weight Compute 결과 원점도 포함한다. 전체 팔레트를 사용하므로 경계가 느슨할 수 있다. 잘못된 pose 뒤에는 `HasPoseBounds`가 false다. PickHit는 ObjectId와 MeshId를 반환하지만 기존 기즈모·Undo UI는 owner TRS를 편집한다.

`AddColliderComponent`는 현재 revision과 대상 메시의 ID 대응만 기록한다. geometry 교체 뒤 오래된 대응은 `IsColliderCurrent`에서 거부하고 메시 제거 시 연결된 collider를 제거한다. Bounds와 collider shape를 혼동하지 않으며 실제 PhysicsWorld·shape·body·constraint는 후속 작업이다.

프레임 입력에서 변경한 뒤 Update/Render/명령 제출이 끝날 때까지 컴포넌트를 유지한다. 미제출 명령이 있는 Update/Render 사이의 CRUD는 지원하지 않는다. 이미 제출된 GPU 사용은 현재 전역 IdleGPU로 보호한다. GPU 지연 해제 큐·절단 게시/Undo transaction은 후속 backend에서 추가한다.

이번 구현의 GeometryRevision은 **원본 section subset**까지다. 새 절단 VB/IB·cap·Blend 출처·선택 joint 기여 보정·반복 절단 알고리즘은 아직 없다. 구조 기반을 구현한 것과 절단 결과 생성 지원을 구분한다. Frozen은 첫 유효 Update의 자세를 고정하는 단순 정책이며 NDC 본 분리 정책을 구현한 것이 아니다.

단계 리뷰: 1) 불변 geometry/ID, 2) pose와 GPU 소유 분리, 3) owner CRUD·선택·collider 대응을 각각 독립 리뷰했다. 발견한 임의 section offset 조회, ID wrap, stale pose bounds를 수정했다. `SceneMathTests` 457개 검사를 통과했다. 최종 검증 결과는 다음과 같다.


### 14.1 최종 검증

- Debug/Release 빌드 모두 exit 0. 단계별 독립 리뷰와 전체 데이터 흐름 리뷰에서 수정 후 추가 blocker 없음. `git diff --check` 통과.
- GPU NVIDIA GeForce MX450, driver `32.0.15.9186`, `-debug 1`, 1920×1080. 기본 장면의 구조 변경 전 legacy VS와 변경 후 Compute를 고정 16ms frame 60에서 비교해 HDR 2,073,600픽셀 중 9픽셀 차이, 최대 선형 채널 차이 0.00390625, 평균 4.00e-9. 이전 화면 보존을 확인했다.
- 진단 `-component-checks 1 -compute-pose 1 -capture <외부 prefix> -animation-step-ms 16 -capture-frame 30`과 frame 90을 실행했다. owner 3개(Cube/Bunny/Xbot), 각각 메시 2개의 `structure passed` 및 `GPU preparation passed`를 확인했다. 독립 GPU VA·ID 비재사용·revision 교체/복구·collider stale·source frame 보정·WorldIndependent·invalid pose 준비 해제를 검사했다.
- 두 프레임 HDR에서 상단 Frozen Xbot 영역은 0픽셀 변경, 하단 FollowModel Xbot은 10,330픽셀 변경. PNG를 직접 확인해 Static·Skeletal 형상, 서로 다른 자세의 두 컴포넌트 렌더, 애니메이션 추종과 고정을 확인했다. 기본 실행에는 기존 세 모델만 표시하고 진단 옵션에서만 복사본을 추가한다.
- 동일 진단 frame 30의 legacy/Compute는 360픽셀 차이, 최대 선형 채널 차이 0.046875, 평균 3.36e-7. Debug/Release Compute frame 30 HDR는 byte-identical(0픽셀 차이)이다.
- 새 D3D12 ERROR/CORRUPTION·device removal·assert·처리되지 않은 예외 없음. Release의 기존 DXGI #80 및 D3D12 WARNING #974/#1008은 남아 있다.
- 로그·RAW·PNG·비교 JSON은 저장소 밖 `%TEMP%/mesh-component-validation/`, 빌드 로그는 `%TEMP%/mesh-component-{debug,release}-build.log`, CPU 회귀 로그는 `%TEMP%/pose-validation/stage3/`다. 실행 전후 Xbot/bunny/cube cache SHA256은 보존한 원래 바이트와 일치했다.
- 캡처 helper가 GPU 완료·캡처 marker 이후 자신이 실행한 프로세스를 종료했다. 실제 키·마우스 조작과 정상 종료/삭제의 런타임 검증은 포함하지 않았다. 이미 제출된 GPU 자원 해제 순서는 코드 리뷰로 확인했다. 생성 절단 geometry·반복 절단·관절 기여 보정·물리 검증은 이 결과에 포함하지 않는다.


## 15. Static GPU 절단 구현

2026-10-25 이후 Static 표면 clip·경계 hash/loop·한 loop 내부 병렬 cap·생성 버퍼 렌더·같은 owner의 A/B 게시·재절단을 연결했다. 12~13절의 장기 설계 전체나 Skeletal 절단이 구현됐다는 뜻은 아니다. 열린 경계·유한 평면·용량/수명·검증·CPU 비교의 현재 계약은 [StaticCutImplementation.md](StaticCutImplementation.md)에 기록한다.
