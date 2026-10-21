# MeshCutting 프로젝트 흐름 보고서

> 2026-10-21 갱신: Geometry 파일을 합쳤고(PatchGeometry+PatchWeld -> Patch, PlaneSlicer -> Slice, CutLoops+CapBuilder -> SliceCap, PieceSkinning+JointSet -> Skinning, SegmentPick -> CutPlane, RenderBake -> Rendering/) 구조체 이름도 바꿨다(GeomNode -> PatchNode, SkinNode -> PatchSkin, SurfaceOrigin -> PatchOrigin, RenderCorner -> PatchCorner, CapRecord -> PatchCap, Face -> PatchFace, NodeKind/FaceKind/EdgeKind -> PatchNodeKind/PatchFaceKind/PatchEdgeKind, EdgeUsage -> PatchEdgeUsage, PlaneBasis -> SlicePlaneBasis, CutLoop -> SliceLoop). 파일 경로는 새 이름으로 고쳤지만 `파일:줄` 인용의 줄 번호는 합치기 전 기준이라 새 파일에서는 어긋난다. 함수 이름으로 찾는다.

기준: 2026-10-20에 읽은 현재 작업 트리. 실행 결과가 아니라 정적 코드 추적 보고서다.
이 보고서 작성 중 빌드·테스트·앱 실행은 하지 않았다. 실제 GPU·드라이버·성능 수치는 확인 못 함.
`[미커밋]`은 현재 HEAD에 없는 구현 또는 수정된 연결 지점을 뜻한다.
`Runtime/`, `Debug/` 전체와 렌더러 와이어프레임 확장, ModelViewer의 새 위임 경로에 이 표시를 붙인다.
이 표시는 기능의 구현 여부와 구별한다. 설계 문서에만 있는 기능은 구현된 것으로 서술하지 않는다.

근거 경로 규칙: `Geometry/...`, `Runtime/...`, `Debug/...`, `Rendering/...`, `Docs/...`, `tests/...`와
`BonePatch.*`, `ProceduralMesh.*`, `MeshCutting.*`는 모두 `MiniEngine/MeshCutting/` 기준 상대 경로다.
그 밖의 파일은 `MiniEngine/`부터 적는다. `파일:줄`은 해당 파일의 실제 줄 번호다.
한 문장 끝의 근거는 그 문장에, 표의 근거 열은 해당 행 전체에 적용한다.
디버거의 관찰 안내는 권장 확인 항목이며, 그 값이 실행에서 관측되었다는 뜻은 아니다.

## 1. 한 장 요약

1. 스킨 모델의 CPU VB/IB 사본에서 선택 본의 영향이 있는 삼각형을 현재 포즈의 world 패치로 복사한다. (`MiniEngine/Model/ModelLoader.cpp:230`, `BonePatch.cpp:336`, `BonePatch.cpp:257`)
2. 렌더 정점은 유지하면서 동일한 bind 위치·스킨·meshSlot을 기하 노드로 용접한다. (`Geometry/Patch.cpp:169`, `Geometry/Patch.cpp:202`)
3. world 평면으로 노드를 분류하고 표면을 나눈 뒤, 절단 경계의 루프를 검증하고 선택적으로 cap을 만든다. (`Geometry/Slice.cpp:453`, `Geometry/Slice.cpp:517`, `Geometry/Slice.cpp:541`)
4. [미커밋] 패치와 성공한 두 조각은 매 프레임 CPU 스키닝을 따르며, 조각 교체는 평면 조작으로 dirty가 된 때만 시도한다. (`Runtime/CutController.cpp:130`, `Runtime/CutController.cpp:141`, `Runtime/CutController.cpp:148`)
5. [미커밋] 표시용 메시를 베이크·갱신하고 버전이 바뀐 GPU 버퍼를 교체해 그리며, 이전 버퍼는 제출 펜스 완료 뒤 재사용한다. (`Debug/CutDebugView.cpp:71`, `Rendering/ProceduralMeshRenderer.cpp:186`, `Rendering/ProceduralMeshRenderer.cpp:119`)

### 1.1 모듈 지도

아래의 `owns`는 C++ 멤버 소유, `->`는 호출 또는 입력 의존, `copy`는 값 복사다.
지도 근거: `MiniEngine/ModelViewer/ModelViewer.cpp:78`, `Runtime/CutController.h:80`,
`Debug/CutDebugView.h:45`, `Rendering/ProceduralMeshRenderer.h:84`, `Geometry/Slice.cpp:3`.

```text
ModelViewer
  | owns ModelInstance -> Model (shared_ptr<const Model>)
  |                       owns m_GeometryCPU, Mesh, palettes, IBMs
  | owns CutController [미커밋]
  |       | owns ExtractedMesh + BonePatchSource + PatchPiece + +/- pieces
  |       +-> BonePatch -> Model / ModelInstance
  |       +-> PatchWeld -> PatchGeometry
  |       +-> PieceSkinning -> PatchGeometry
  |       +-> PlaneSlicer -> CutLoops -> PatchGeometry
  |                    +-> CapBuilder -> PatchGeometry
  | owns CutDebugView [미커밋] <- reads CutController
  |       +-> RenderBake -> ProceduralMeshData -> ProceduralMesh (CPU)
  | owns ProceduralMeshRenderer
          +-> Core GraphicsContext / ByteAddressBuffer / fence
          +-> ProceduralMeshVS / ProceduralMeshPS
          owns map<ProceduralMesh address, MeshGpu>, retired/free/pending buffers
tests -> Geometry + ProceduralMesh (별도 main, D3D12 렌더 경로 밖)
```

ModelInstance의 Model 공유 소유와 포즈 배열은 `MiniEngine/Model/Model.h:184`, `MiniEngine/Model/Model.h:188`, `MiniEngine/Model/Model.h:193`에서 확인한다.
Geometry 작업 자료는 Model/Core 타입을 받지 않고, BonePatch가 `WeldInput`으로 변환한다. (`Geometry/Patch.h:3`, `BonePatch.cpp:391`)
단, `RenderBake`는 출력용 `ProceduralMeshData`를 만든다. Geometry의 계산 자료와 렌더 자료의 연결점이다. (`Rendering/RenderBake.cpp:92`)

### 1.2 먼저 구별할 세 가지 복사본

| 복사본 | 역할과 생명주기 | 근거 |
| --- | --- | --- |
| `Model::m_GeometryCPU` | 원본 압축 정점 형식과 인덱스를 담은 모델 소유 바이트 배열 | `MiniEngine/Model/Model.h:121`, `BonePatch.cpp:234` |
| `m_ExtractedMesh` / `m_Source` | C에서 추출한 world 정점과 원본 스킨·삼각형 출처; [미커밋] Controller 소유 | `Runtime/CutController.cpp:18`, `Runtime/CutController.cpp:21` |
| `m_PatchPiece` / 두 sliced piece | 용접된 계산용 위상과 현재 포즈 위치; [미커밋] Controller가 스키닝·교체 | `Runtime/CutController.h:84`, `Runtime/CutController.cpp:130`, `Runtime/CutController.cpp:282` |

표시용 `m_PatchMesh`와 `m_PiecePositive/Negative`는 다시 별도 사본이며 [미커밋] View가 소유한다. (`Debug/CutDebugView.h:45`)
원본 모델은 계속 `m_ModelInst.Render(sorter)`로 제출된다. 패치 표시가 원본 삼각형을 숨기는 경로는 이 호출에 없다. (`MiniEngine/ModelViewer/ModelViewer.cpp:478`, `MiniEngine/Model/Model.cpp:44`)
현재 두 조각을 벌리는 처리는 표시 행렬의 평행이동이다. 물리 시뮬레이션 결과로 해석하지 않는다. (`Debug/CutDebugView.cpp:169` [미커밋])

### 1.3 파일 전체를 읽을 때의 경계

화면 입력용 `InputStroke`/`ReceiveStroke`(`MeshCutting.h/.cpp`)는 호출도 동작도 없는 자리표시라 삭제했다.
키보드 평면 절단의 진입 함수는 `CutController::SetPatch/ToggleEditPlane/Update`다. 흐름은 `Docs/CodeMap.md` 참고.
프로젝트는 BonePatch, ProceduralMesh, Geometry, Rendering과 [미커밋] Runtime/Debug 소스를 등록한다. (`MeshCutting.vcxproj:162`, `MeshCutting.vcxproj:165`, `MeshCutting.vcxproj:168`, `MeshCutting.vcxproj:171`)
셰이더는 `FxCompile`, PlaneSlicerTests는 `None` 항목이다. 일반 라이브러리 빌드와 테스트 main을 구별한다. (`MeshCutting.vcxproj:203`, `MeshCutting.vcxproj:207`)
ModelViewer는 MeshCutting 프로젝트를 참조한다. (`MiniEngine/ModelViewer/ModelViewer.vcxproj:80`)

## 2. 핵심 데이터 타입

### 2.1 모델과 추출 자료

`BonePatch`라는 단일 구조체는 여기서 찾지 못했다. 실제 API는 `ExtractBonePatch`이고 출력 묶음은 `ProceduralMeshData`와 `BonePatchSource`다. (`BonePatch.h:46`, `BonePatch.h:69`)
`PatchGeometry`도 구조체 이름이 아니라 자료·검증 함수가 모인 파일 이름이다. 실제 작업 단위는 `PatchPiece`다. (`Geometry/Patch.h:94`, `Geometry/Patch.h:186`)

| 타입/필드 | 무엇을 담는가 | 생성자와 소유자 | 좌표계/인덱스 의미 | 근거 |
| --- | --- | --- | --- | --- |
| `Model::m_GeometryCPU` | `.mini`의 geometry 바이트 | LoadModel 생성, Model 소유 | 위치는 원본 mesh의 pre-skin 데이터 | `MiniEngine/Model/ModelLoader.cpp:232`, `BonePatch.cpp:252` |
| `SkinPalette` | `startJoint`, `numJoints` | 로더가 읽고 Model 소유 | flat joint 배열의 부분 범위 | `MiniEngine/Model/Model.h:101`, `MiniEngine/Model/ModelLoader.cpp:314` |
| `Joint` | `posXform`, `nrmXform` | ModelInstance::Update 작성, instance 소유 | node pose와 IBM을 합친 스킨 변환 | `MiniEngine/Model/Model.h:95`, `MiniEngine/Model/Model.cpp:242` |
| `BonePatchMesh` | 원본 mesh 번호, mesh world 번호, palette 시작 | ExtractBonePatch 생성, Source vector 소유 | `meshSlot`이 참조하는 원본 mesh 설명 | `BonePatch.h:18`, `BonePatch.cpp:377` |
| `BonePatchVertex` | 원본 정점 ID·위치·법선·4 joints/weights | 추출 중 생성, Source 소유 | 위치/법선은 pre-skin mesh; joint는 palette-local | `BonePatch.h:27`, `BonePatch.cpp:277` |
| `BonePatchTriangle` | meshSlot, drawIndex, draw 안 primitive | 추출 중 생성, Source 소유 | 원본 삼각형 식별자 | `BonePatch.h:39`, `BonePatch.cpp:371` |
| `BonePatchSource` | 추출 선택, 원본 배열, pose snapshot, 혼합 진단 수 | ExtractBonePatch 생성; [미커밋] Controller 소유 | bind와 world를 함께 보관하는 메타데이터 | `BonePatch.h:46`, `Runtime/CutController.h:82` |
| `poseJointWorld` | 추출 시 palette별 joint 행렬 | 추출기가 채움, Source 소유 | 첫 기여 mesh world × joint node pose; 패치와 같은 world | `BonePatch.cpp:352`, `BonePatch.cpp:360` |
| `mixedOutsideTriangles` | 미선택 삼각형 중 subtree 안/밖 weight가 공존하는 개수 | 추출기 누적 | 좌표 아님; 추출 제외 영역의 진단 | `BonePatch.cpp:338`, `BonePatch.h:57` |

`source.vertices[i]`와 추출 정점 `vertices[i]`는 나란히 쌓인다. `source.triangles[i]`는 패치 삼각형 i의 출처다. (`BonePatch.cpp:285`, `BonePatch.cpp:370`)
이 대응이 깨지면 `MakeWeldInput`은 개수 검사에서 실패한다. (`BonePatch.cpp:394`)
스킨 원본은 world 스냅샷을 역변환한 값이 아니라 원본 VB에서 읽어둔 값이다. (`BonePatch.cpp:252`, `BonePatch.cpp:280`)

### 2.2 용접과 계산 위상

| 타입 | 무엇을 담는가 | 누가 만들고 누가 소유하는가 | 공간 또는 참조 대상 | 근거 |
| --- | --- | --- | --- | --- |
| `WeldInputVertex` | posed/bind 위치, 스킨, 렌더 속성 | MakeWeldInput 생성, 임시 WeldInput 소유 | bind는 mesh, position은 world | `Geometry/Patch.h:8`, `BonePatch.cpp:406` |
| `WeldInput` | 정점 vector와 triangle-list indices | MakeWeldInput 출력; BuildPatchPiece 입력 | 추출 정점 번호 | `Geometry/Patch.h:20`, `BonePatch.cpp:418` |
| `WeldStats` | 용접 전후 개수, seam/경계/비다양체 수 | BuildPatchPiece 작성; [미커밋] Controller 보관 | 좌표 없음 | `Geometry/Patch.h:26`, `Runtime/CutController.h:85` |
| `PatchSkin` | bindPosition, meshSlot, 정렬된 스킨 쌍 | BuildPatchPiece 생성, piece.originNodes 소유 | pre-skin mesh 위치, palette-local joints | `Geometry/Patch.h:21`, `Geometry/Patch.cpp:164` |
| `PatchOrigin` | 원본 노드 최대 3개와 bary | 최초 용접 또는 MergeOrigin 생성, PatchNode 값 멤버 | `originNodes` 번호; 현 piece.nodes 번호가 아님 | `Geometry/Patch.h:30`, `Geometry/Slice.cpp:80` |
| `PatchNode` | 현재 위치, origin, kind, 생성 cutId | 용접/슬라이서 생성, piece.nodes 소유 | 데모에서는 world; Geometry는 입력 공간 그대로 사용 | `Geometry/Patch.h:41`, `Geometry/Slice.cpp:191` |
| `PatchCorner` | node 번호 + normal/uv/color | 용접·분할·cap 생성, piece.corners 소유 | node는 piece-local; normal은 추출/절단 당시 속성 | `Geometry/Patch.h:50`, `Geometry/SliceCap.cpp:289` |
| `PatchFace` | corner 세 개, PatchFaceKind, sourceTriangle, cutId | 용접·분할·cap 생성, piece.faces 소유 | corner는 piece-local, sourceTriangle은 Source.triangles | `Geometry/Patch.h:60`, `Geometry/Patch.cpp:214` |
| `PatchPiece` | originNodes/nodes/corners/faces/boundary/caps/skinStateId | 용접기·슬라이서 생성; [미커밋] Controller 보관 | 위치는 world, 인덱스는 배열별 범위 | `Geometry/Patch.h:94`, `Runtime/CutController.h:109` |

`originNodes`의 주석에는 shared라고 쓰여 있지만 실제 필드는 `std::vector<PatchSkin>`다. (`Geometry/Patch.h:96`)
출력 piece를 만들 때 원본 vector를 값 복사한다. 공유 포인터 소유 관계로 읽으면 안 된다. (`Geometry/Slice.cpp:374`)
노드 하나에 corner 여러 개가 달릴 수 있으므로 노드 수와 정점 수를 동일시하지 않는다. (`Geometry/Patch.cpp:181`, `Geometry/Patch.cpp:202`)
UV seam의 서로 다른 corner가 한 node를 가리키는 사례는 테스트에 있다. (`tests/PlaneSlicerTests.cpp:222`)

### 2.3 절단, cap, 렌더 자료

| 타입 | 무엇을 담는가 | 생성·소유 | 좌표계/의미 | 근거 |
| --- | --- | --- | --- | --- |
| `SlicePlane` | point, normal | [미커밋] CurrentPlane 생성, SlicePatch가 읽음 | 현재 posed patch와 같은 world | `Geometry/Patch.h:129`, `Runtime/CutController.cpp:239` |
| `SlicePlaneBasis` | origin, 단위 n, u, v | MakePlaneBasis 생성, 계산 함수의 지역 값 | world 평면에 대한 2D 투영 기준, u×v=n | `Geometry/Patch.h:138`, `Geometry/Patch.cpp:143` |
| `SliceOptions` | cap 모드, epsilonScale, capUvScale | 호출자 지역 값 | epsilon은 piece radius 비율 | `Geometry/Slice.h:13`, `Geometry/Slice.cpp:466` |
| `SliceResult` | status, 두 piece, loops/depth/area, detail | SlicePatch 출력; [미커밋] UpdateSlice 지역 값 | loops는 positive piece의 node 번호 | `Geometry/Slice.h:20`, `Runtime/CutController.cpp:271` |
| `SliceLoop` | node 순서, signedArea, depth, parent | ExtractCutLoops 생성, Run의 지역 배열 | node는 각 piece-local, area는 plane 2D | `Geometry/Slice.h:7`, `Geometry/Slice.cpp:513` |
| `PatchCap` | cutId, plane, rimNodes/loopSizes | BuildCap 생성, piece.caps 소유 | plane은 절단 당시 world, rim은 해당 piece-local | `Geometry/Patch.h:79`, `Geometry/SliceCap.cpp:336` |
| `ProceduralVertex` | position/normal/uv/color | 추출기 또는 RenderBake 생성 | 데모 position은 world; shader World는 표시 offset | `ProceduralMesh.h:10`, `Debug/CutDebugView.cpp:135` [미커밋] |
| `MeshSection` | firstIndex/indexCount/materialSlot | 추출기/베이크 생성, mesh data 소유 | index 배열의 범위 | `ProceduralMesh.h:18`, `Rendering/RenderBake.cpp:116` |
| `ProceduralMeshData` | 렌더 정점·uint32 indices·sections | CPU 생성자들 출력, ProceduralMesh가 소유 | GPU 스킨 속성은 들어 있지 않음 | `ProceduralMesh.h:25`, `ProceduralMesh.h:10` |
| `ProceduralMesh` | CPU data/bounds와 두 version | [미커밋] Controller 또는 View의 멤버 | GPU 리소스 소유 객체와 분리 | `ProceduralMesh.h:41`, `ProceduralMesh.h:55`, `Rendering/ProceduralMeshRenderer.h:63` |
| `SkinStats` | 원본 LBS 평가 수·Surface 기록 수·Cap skip 수 | SkinPatchPiece의 선택 출력 | 시간 값이 아닌 개수 | `Geometry/Skinning.h:16`, `Geometry/Skinning.cpp:112` |

### 2.4 에지와 상태를 읽는 표

| 값 | 현재 의미 | 근거 |
| --- | --- | --- |
| `Interior` | 일반 내부 에지는 boundary map 항목을 만들지 않는 규약 | `Geometry/Patch.h:68` |
| `Replacement` | 추출 패치의 열린 경계 | `Geometry/Patch.h:72`, `Geometry/Patch.cpp:270` |
| `UnweldedSeam` | 동일 위치라도 용접되지 않은 seam 경계 | `Geometry/Patch.h:73`, `Geometry/Patch.cpp:268` |
| `Cut` | cap 전 이번 절단으로 생긴 열린 경계 | `Geometry/Slice.cpp:422` |
| `CapSeam` | 표면과 cap이 공유하는 내부 에지 태그 | `Geometry/SliceCap.cpp:324`, `Geometry/SliceCap.cpp:326` |
| `skinStateId == 0` | 현재 SkinPatchPiece가 허용하는 원본 스키닝 상태 | `Geometry/Skinning.cpp:44` |
| `SliceStatus::Ok` | 두 piece가 있고 뒤 단계까지 성공; loops가 꼭 있어야 하는 것은 아님 | `Geometry/Slice.h:35`, `Geometry/Slice.cpp:557` |
| `SliceStatus::NoCut` | 처리 후 어느 한쪽 face 배열이 비어 있음 | `Geometry/Slice.cpp:483` |

boundary 키는 node 번호의 작은 값을 상위 32비트, 큰 값을 하위 32비트에 넣는다. (`Geometry/Patch.h:152`)
따라서 map 키 자체에는 방향이 없고, 방향은 face 순회에서 따로 센다. (`Geometry/Patch.cpp:165`, `Geometry/Patch.cpp:176`)
`Empty()`는 faces.empty()만 검사한다. 디버거에서 nodes가 남았는지만 보고 유효 piece로 판단하지 않는다. (`Geometry/Patch.h:106`)

## 3. 흐름 따라가기

### 3a. 앱 시작 → 모델 로드 → CPU 지오메트리 보존

1. `ModelViewer::Startup` → `Renderer::Initialize` → `m_ProceduralRenderer.Initialize`.
   렌더러 공통 초기화 다음 procedural root signature/PSO 설정을 준비한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:188`, `Rendering/ProceduralMeshRenderer.cpp:41`)
2. `Startup` → `Renderer::LoadModel(gltfFileName, forceRebuild)`.
   기본 파일은 `Models/CesiumMan.glb`이고 rebuild 인자를 읽어 전달한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:193`, `MiniEngine/ModelViewer/ModelViewer.cpp:197`, `MiniEngine/ModelViewer/ModelViewer.cpp:215`)
3. `Renderer::LoadModel` → `.mini` 존재·시간·버전 검사.
   재생성 필요 여부가 결정되며 양쪽 파일이 모두 없으면 nullptr로 끝난다. (`MiniEngine/Model/ModelLoader.cpp:139`, `MiniEngine/Model/ModelLoader.cpp:153`, `MiniEngine/Model/ModelLoader.cpp:162`, `MiniEngine/Model/ModelLoader.cpp:170`)
4. 필요 시 `LoadModel` → `glTF::Asset` → `BuildModel` → `SaveModel`.
   변환 결과 `.mini`를 다시 열어 이후 공통 로딩 경로로 들어온다. (`MiniEngine/Model/ModelLoader.cpp:192`, `MiniEngine/Model/ModelLoader.cpp:209`, `MiniEngine/Model/ModelLoader.cpp:212`)
5. `LoadModel` → `new Model` → geometry read.
   geometrySize 바이트를 `m_GeometryCPU`에 읽고 upload 사본으로 `m_DataBuffer`를 만든다. (`MiniEngine/Model/ModelLoader.cpp:223`, `MiniEngine/Model/ModelLoader.cpp:232`, `MiniEngine/Model/ModelLoader.cpp:238`)
6. `LoadModel` → scene graph/mesh metadata read → skinned mesh scan.
   `kHasSkin`이 하나도 없으면 CPU geometry vector를 비우고, 하나라도 있으면 보존한다. (`MiniEngine/Model/ModelLoader.cpp:241`, `MiniEngine/Model/ModelLoader.cpp:247`, `MiniEngine/Model/ModelLoader.cpp:253`)
7. `LoadModel` → joint/IBM/palette/parent/name read.
   추출과 본 방향 계산에 쓸 flat joint 배열, palette 범위, 부모 관계를 Model에 채운다. (`MiniEngine/Model/ModelLoader.cpp:304`, `MiniEngine/Model/ModelLoader.cpp:314`, `MiniEngine/Model/ModelLoader.cpp:320`)
8. 로드된 Model → `ModelInstance::operator=(shared_ptr<const Model>)`.
   instance가 node world, skeleton과 애니메이션 graph/state를 할당한다. (`MiniEngine/Model/Model.cpp:123`, `MiniEngine/Model/Model.cpp:142`, `MiniEngine/Model/Model.cpp:146`)
9. `Startup` → `LoopAllAnimations` → `Resize(10.0f)` → palette 선택 초기화.
   애니메이션 재생을 요청하고 표시 크기를 정하며, joint가 있는 palette를 bone debug 대상으로 잡는다. (`MiniEngine/ModelViewer/ModelViewer.cpp:216`, `MiniEngine/ModelViewer/ModelViewer.cpp:222`)
10. 다음 `ModelViewer::Update` → `ModelInstance::Update` → node world/skeleton 작성.
    실제 포즈 배열은 이 갱신에서 채워지므로 C 추적 전 완료된 Update가 있는지 확인한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:389`, `MiniEngine/Model/Model.cpp:208`, `MiniEngine/Model/Model.cpp:243`)

이 시나리오의 종료 관찰값은 CPU geometry 크기, 선택 palette 범위, node world/skeleton 포인터다. (`MiniEngine/Model/Model.h:121`, `MiniEngine/Model/Model.h:170`)
런타임에 원본 glb를 직접 잘라 쓰는 것이 아니라, 로더가 유지한 packed geometry를 Decode한다. (`MiniEngine/Model/ModelLoader.cpp:233`, `BonePatch.cpp:59`, `BonePatch.cpp:198`)

### 3b. 본 선택 → C → 패치 복사와 용접

1. `ModelViewer::Update` → bone debug 입력 분기 → J/K/I 처리.
   J/K는 palette-local jointIndex를 순환하고 I는 다음 유효 palette와 joint 0을 고른다. (`MiniEngine/ModelViewer/ModelViewer.cpp:277`, `MiniEngine/ModelViewer/ModelViewer.cpp:288`, `MiniEngine/ModelViewer/ModelViewer.cpp:293`)
2. [미커밋] C 입력 → `CutController::SetPatch` → `ExtractBonePatch(..., 0.0f, ...)`.
   현재 선택과 threshold 0으로 직전 instance Update 포즈의 복사를 시작한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:306`, `Runtime/CutController.cpp:18`)
3. `ExtractBonePatch` → model/palette/joint/CPU geometry/pose 검사.
   소속이 다르거나 geometry가 없으면 outputs를 교체하기 전에 실패한다. (`BonePatch.cpp:161`, `BonePatch.cpp:168`, `BonePatch.cpp:170`)
4. `ExtractBonePatch` → selected subtree 표시.
   부모를 위로 걸으며 선택 본과 자손 여부를 표시하지만, 이 배열은 혼합 진단에 사용된다. (`BonePatch.cpp:175`, `BonePatch.cpp:331`, `BonePatch.cpp:340`)
5. `ExtractBonePatch` → mesh 순회 → `ComputeLayout`.
   같은 palette의 skinned mesh만 대상으로 잡고 stride, VB/IB 범위, index format을 검사한다. (`BonePatch.cpp:208`, `BonePatch.cpp:213`, `BonePatch.cpp:217`, `BonePatch.cpp:228`)
6. `ExtractBonePatch` → `MakeSlotMatrices`.
   mesh world와 palette skeleton의 posXform을 곱해 해당 meshSlot의 CPU LBS 행렬을 만든다. (`BonePatch.cpp:237`, `BonePatch.cpp:240`, `BonePatch.cpp:103`)
7. draw/triangle 순회 → `Decode` → `Influence`.
   R16/R32 index와 baseVertex로 원본 정점을 찾고, 어느 한 정점이라도 선택 본 weight가 threshold보다 크면 삼각형 전체를 선택한다. (`BonePatch.cpp:299`, `BonePatch.cpp:314`, `BonePatch.cpp:324`, `BonePatch.cpp:336`)
8. 미선택 triangle → subtree 혼합 진단.
   subtree 안/밖 weight가 함께 있으면 mixedOutsideTriangles만 증가하고 indices에는 넣지 않는다. (`BonePatch.cpp:338`)
9. 첫 기여 mesh → `poseJointWorld` snapshot; 후속 기여 mesh → mesh world 일치 검사.
   첫 mesh의 `world * nodeWorld[jointNode]`를 저장하고 서로 다른 world 행렬은 memcmp 검사에서 거절한다. (`BonePatch.cpp:352`, `BonePatch.cpp:360`, `BonePatch.cpp:365`)
10. 선택 triangle → `addVertex` → `Decode` → `SkinPoint`.
    mesh별 sourceVertex remap으로 중복 정점을 재사용하고, 새 정점은 posed world 위치와 원본 스킨을 나란히 저장한다. (`BonePatch.cpp:246`, `BonePatch.cpp:257`, `BonePatch.cpp:285`)
11. `addVertex` → 법선 blend/색 결정 → triangle 출처 기록.
    법선은 joint nrmXform과 worldIT를 적용하고, 색은 선택 본 influence로 정하며, draw/primitive ID를 보관한다. (`BonePatch.cpp:264`, `BonePatch.cpp:267`, `BonePatch.cpp:275`, `BonePatch.cpp:371`)
12. `ExtractBonePatch` → outputs move → [미커밋] `m_ExtractedMesh.SetMesh`.
    삼각형이 있으면 section 하나를 만들고 결과를 넘기며, SetMesh 검증 성공 후 Controller가 Source를 교체한다. (`BonePatch.cpp:380`, `BonePatch.cpp:383`, `Runtime/CutController.cpp:19`, `Runtime/CutController.cpp:21`)
13. [미커밋] `SetPatch` → `RebuildPatchPiece` → `MakeWeldInput`.
    posed 렌더 속성과 pre-skin 스킨 원본을 plain 입력 구조체로 합친다. (`Runtime/CutController.cpp:25`, `Runtime/CutController.cpp:92`, `BonePatch.cpp:403`)
14. `RebuildPatchPiece` → `BuildPatchPiece` → `CanonicalSkin`.
    양수 weight만 남기고 같은 joint를 합친 뒤 정렬하여 비트 기반 용접 키를 만든다. (`Runtime/CutController.cpp:97` [미커밋], `Geometry/Patch.cpp:91`, `Geometry/Patch.cpp:169`)
15. `BuildPatchPiece` → node/corner/face 생성 → `CollectEdgeUsage`.
    원본 렌더 정점당 corner 하나를 보존하고 열린 에지를 Replacement 또는 UnweldedSeam으로 분류한다. (`Geometry/Patch.cpp:193`, `Geometry/Patch.cpp:202`, `Geometry/Patch.cpp:233`, `Geometry/Patch.cpp:270`)
16. `BuildPatchPiece` → `ValidatePatchPiece` → [미커밋] radius 저장.
    성공한 용접 piece만 출력하고 Controller는 최초 복사 때의 반지름을 표시 기준으로 보관한다. (`Geometry/Patch.cpp:280`, `Geometry/Patch.cpp:284`, `Runtime/CutController.cpp:100`)
17. [미커밋] `SetPatch` → `ClearSlice` → `ComputeBasePlane` → `UpdateBasePlane`.
    이전 조각을 지우고 offset/tilt를 0으로 한 뒤 선택 본 local 평면을 만들며 patchVersion을 증가시킨다. (`Runtime/CutController.cpp:26`, `Runtime/CutController.cpp:28`, `Runtime/CutController.cpp:29`, `Runtime/CutController.cpp:31`)
18. [미커밋] 이후 `ModelInstance::Update` → `CutController::Update` → `CutDebugView::Sync`.
    새 패치는 이번 포즈로 스키닝되고 View가 patchVersion 변화를 보고 추출 사본을 표시 메시로 복사한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:389`, `Runtime/CutController.cpp:130`, `Debug/CutDebugView.cpp:38`)

C 성공과 용접 성공은 별도 관찰 대상이다. SetPatch는 RebuildPatchPiece 호출 뒤 piece status와 무관하게 true를 반환한다. (`Runtime/CutController.cpp:25`, `Runtime/CutController.cpp:32` [미커밋])
따라서 C 후 화면 상태는 `PatchStatusText()`와 `PatchStatus()/PatchDetail()`을 함께 읽는다. (`Runtime/CutController.h:45`, `Debug/CutDebugView.cpp:206` [미커밋])
C 추출 실패는 이전 패치를 남기고 오류 문구만 바꾼다. (`Runtime/CutController.cpp:18`, `Runtime/CutController.cpp:34` [미커밋])

### 3c. 평면 조정 → 슬라이스 → 루프 → cap → 렌더 베이크

용접은 이 시나리오의 매 절단마다 실행되지 않는다. 앞의 C 단계에서 `m_PatchPiece`를 만든 뒤 그 piece를 사용한다. (`Runtime/CutController.cpp:97`, `Runtime/CutController.cpp:272` [미커밋])

1. [미커밋] `ModelViewer::Update` → `ToggleSlice` 또는 `NudgePlane/ResetPlane/ToggleCap`.
   T는 enabled를 바꾸고, 조작 함수는 dirty를 켠다. 키 처리 자체는 SlicePatch를 부르지 않는다. (`MiniEngine/ModelViewer/ModelViewer.cpp:320`, `Runtime/CutController.cpp:50`, `Runtime/CutController.cpp:68`)
2. `ModelViewer::Update` → `ModelInstance::Update` → [미커밋] `CutController::Update`.
   이번 프레임 포즈 계산 후 패치의 노드 위치를 먼저 현재 포즈로 갱신한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:389`, `MiniEngine/ModelViewer/ModelViewer.cpp:395`, `Runtime/CutController.cpp:130`)
3. [미커밋] `CutController::Update` → `UpdateBasePlane` → 조건부 `UpdateSlice`.
   enabled·dirty·baseValid가 모두 참일 때만 실제 절단하고 이 시도의 dirty를 소비한다. (`Runtime/CutController.cpp:138`, `Runtime/CutController.cpp:141`, `Runtime/CutController.cpp:146`)
4. [미커밋] `UpdateSlice` → `CurrentPlane` → `SlicePatch(m_PatchPiece, plane, 1, options, result)`.
   cap 옵션을 Create/None으로 선택하고 cutId는 매번 1을 넘긴다. (`Runtime/CutController.cpp:267`, `Runtime/CutController.cpp:270`, `Runtime/CutController.cpp:272`)
5. `SlicePatch` → `Slicer::Run` → `ValidatePlane` → `ValidatePatchPiece`.
   out을 초기화하고 평면·위상·origin·퇴화·에지 사용을 검사한다. (`Geometry/Slice.cpp:574`, `Geometry/Slice.cpp:456`, `Geometry/Patch.cpp:233`)
6. `Slicer::Run` → `MakePlaneBasis` → `ComputePieceRadius` → `ClassifyNodes`.
   단위 평면 법선과 epsilon을 만든 뒤 각 node의 signed distance를 +1/0/-1로 저장한다. (`Geometry/Slice.cpp:465`, `Geometry/Slice.cpp:153`, `Geometry/Slice.cpp:157`)
7. `Slicer::Run` → 각 `ProcessFace`.
   세 node의 부호를 세고 모두 0이면 CoplanarFace, 한쪽뿐이면 그쪽에 원래 face를 옮긴다. (`Geometry/Slice.cpp:469`, `Geometry/Slice.cpp:279`, `Geometry/Slice.cpp:289`, `Geometry/Slice.cpp:296`)
8. `ProcessFace` → 필요 시 `CutNodeOf` → `MergeOrigin`.
   엄격한 반대 부호 에지에 `t=dl/(dl-dh)` 교차 노드를 하나 만들고 origin을 섞으며, 기하 에지 캐시에 재사용한다. (`Geometry/Slice.cpp:164`, `Geometry/Slice.cpp:178`, `Geometry/Slice.cpp:183`, `Geometry/Slice.cpp:198`)
9. `ProcessFace` → `CutCorner` → `AddFace`.
   node와 별도로 corner 쌍 캐시를 사용해 UV·색·법선을 보간하고, 0/+/-는 양쪽 삼각형 하나씩 만든다. (`Geometry/Slice.cpp:216`, `Geometry/Slice.cpp:231`, `Geometry/Slice.cpp:309`, `Geometry/Slice.cpp:321`)
10. `ProcessFace` → lone vertex 분기 → `AddFace` 세 번.
    한쪽 1정점은 삼각형 하나, 다른 쪽 2정점은 잘린 사각형의 삼각형 둘이 된다. (`Geometry/Slice.cpp:328`, `Geometry/Slice.cpp:339`)
11. `Slicer::Run` → cap sides 검사 → NoCut 검사.
    기존 cap이 양쪽으로 갈라지면 CrossesCap, 어느 side에도 face가 없으면 NoCut이다. (`Geometry/Slice.cpp:476`, `Geometry/Slice.cpp:483`)
12. `Slicer::Run` → 양쪽 `BuildPiece` → `InheritedKind` → `CollectEdgeUsage`.
    사용하는 global node만 compact하고 corner.node를 remap하며, 현재 adjacency에서 경계를 다시 태깅한다. (`Geometry/Slice.cpp:370`, `Geometry/Slice.cpp:383`, `Geometry/Slice.cpp:399`, `Geometry/Slice.cpp:411`)
13. `BuildPiece` → cap record remap → `ValidatePatchPiece`.
    보존된 cap의 rim 번호를 바꾸고 양쪽 출력이 다음 절단의 입력으로도 유효한지 검사한다. (`Geometry/Slice.cpp:429`, `Geometry/Slice.cpp:442`, `Geometry/Slice.cpp:503`)
14. `Slicer::Run` → 각 side의 `ExtractCutLoops`.
    Cut 태그이며 두 끝이 onPlane인 에지에서 face 방향의 next/in/out 그래프를 만든다. (`Geometry/Slice.cpp:517`, `Geometry/SliceCap.cpp:78`, `Geometry/SliceCap.cpp:88`)
15. `ExtractCutLoops` → 차수 검사 → cycle 추출.
    외부 경계에서 끝나는 chain은 OpenLoop, 나머지 잘못된 차수는 NonManifoldLoop로 실패한다. (`Geometry/SliceCap.cpp:119`, `Geometry/SliceCap.cpp:136`, `Geometry/SliceCap.cpp:145`)
16. `ExtractCutLoops` → 2D 투영 → 교차/포함/방향 검사.
    자기·상호 접촉, 깊이 2 이상, side에 맞지 않는 winding을 InvalidLoop로 거절한다. (`Geometry/SliceCap.cpp:165`, `Geometry/SliceCap.cpp:189`, `Geometry/SliceCap.cpp:232`, `Geometry/SliceCap.cpp:241`)
17. `Slicer::Run` → 양쪽 loop 길이 목록 비교.
    정렬한 각 loop의 node 수 목록이 다르면 내부 불일치로 InvalidLoop를 반환한다. (`Geometry/Slice.cpp:526`, `Geometry/Slice.cpp:533`)
18. cap Create일 때 `Slicer::Run` → 각 side의 `BuildCap`.
    None이어도 루프 검사는 이미 했고, 여기의 cap 생성만 생략한다. (`Geometry/Slice.cpp:512`, `Geometry/Slice.cpp:541`)
19. `BuildCap` → 2D rings 생성 → 방향 정리 → `BridgeHole`.
    외곽은 CCW, 구멍은 CW로 맞추고 같은 외곽의 구멍을 오른쪽부터 bridge로 연결한다. (`Geometry/SliceCap.cpp:220`, `Geometry/SliceCap.cpp:233`, `Geometry/SliceCap.cpp:254`, `Geometry/SliceCap.cpp:269`)
20. `BridgeHole` → 가시 후보 선택 → `EarClip`.
    bridge가 벽을 넘거나 중점이 재료 밖이면 후보를 버리고, 연결 ring에서 충분한 면적의 convex ear를 잘라낸다. (`Geometry/SliceCap.cpp:102`, `Geometry/SliceCap.cpp:120`, `Geometry/SliceCap.cpp:144`, `Geometry/SliceCap.cpp:163`)
21. `BuildCap` → piece 사본 → cap corner/face 추가.
    새로운 위치 노드를 추가하지 않고 rim node를 참조하는 cap corner와 바깥 방향 face를 만든다. (`Geometry/SliceCap.cpp:278`, `Geometry/SliceCap.cpp:289`, `Geometry/SliceCap.cpp:300`)
22. `BuildCap` → 에지 짝 검사 → 면적 검사 → PatchCap → 최종 검증.
    각 rim 에지가 반대 방향의 두 face에 쓰이면 CapSeam으로 바꾸고, 성공한 사본만 piece로 옮긴다. (`Geometry/SliceCap.cpp:324`, `Geometry/SliceCap.cpp:333`, `Geometry/SliceCap.cpp:336`, `Geometry/SliceCap.cpp:350`, `Geometry/SliceCap.cpp:354`)
23. `Slicer::Run` → `SliceResult` 게시 → [미커밋] `UpdateSlice` 성공 분기.
    양쪽 piece와 positive loops를 반환하며 Controller는 성공한 결과만 기존 두 piece 대신 저장한다. (`Geometry/Slice.cpp:557`, `Geometry/Slice.cpp:559`, `Runtime/CutController.cpp:274`, `Runtime/CutController.cpp:282`)
24. [미커밋] `UpdateSlice` → pieceGeneration 증가 → `CutDebugView::Draw` → `Sync`.
    generation이 달라졌으면 두 piece를 `BuildRenderMesh`와 `BakeBoundaryDebug`로 다시 굽는다. (`Runtime/CutController.cpp:284`, `Debug/CutDebugView.cpp:133`, `Debug/CutDebugView.cpp:56`, `Debug/CutDebugView.cpp:74`)
25. `BuildRenderMesh` → [미커밋] View 메시의 `SetMesh` → renderer `Draw`.
    corner별 정점과 Surface/Cap section을 만들고 CPU 버전을 바꿔 다음 GPU 업로드를 유도한다. (`Rendering/RenderBake.cpp:98`, `Rendering/RenderBake.cpp:107`, `Debug/CutDebugView.cpp:75`, `ProceduralMesh.cpp:155`, `Rendering/ProceduralMeshRenderer.cpp:186`)

### 3c 보조: 실패를 따라가는 갈림길

| 최초 멈춤 지점 | 상태와 확인할 값 | 근거 |
| --- | --- | --- |
| plane/input 검증 | InvalidInput/InvalidOrigin/퇴화/비다양체; detail의 node/face/edge 번호 | `Geometry/Slice.cpp:456`, `Geometry/Patch.cpp:248`, `Geometry/Patch.cpp:277` |
| face가 plane 위 | CoplanarFace; 세 sign이 모두 0인지 | `Geometry/Slice.cpp:289` |
| 기존 cap 교차 | CrossesCap; cap face 또는 cap 집합이 양쪽으로 갔는지 | `Geometry/Slice.cpp:306`, `Geometry/Slice.cpp:476` |
| 한쪽 결과 없음 | NoCut; 양쪽 SideBuild.faces 크기 | `Geometry/Slice.cpp:483` |
| 루프 구성 실패 | OpenLoop/NonManifoldLoop; blocker와 in/out degree | `Geometry/SliceCap.cpp:119` |
| 2D 루프 실패 | InvalidLoop; 교차/깊이/winding detail | `Geometry/SliceCap.cpp:189`, `Geometry/SliceCap.cpp:232` |
| cap 실패 | CapTriangulationFailed; bridge/ear/edge/area detail | `Geometry/SliceCap.cpp:200`, `Geometry/SliceCap.cpp:269`, `Geometry/SliceCap.cpp:324` |

`SlicePatch` 실패 출력에는 status/detail만 남기고 두 piece와 loops를 비운다. (`Geometry/Slice.cpp:577`)
[미커밋] Controller는 이 임시 실패 결과를 화면 piece에 덮어쓰지 않는다. (`Runtime/CutController.cpp:274`)
[미커밋] 실패해도 dirty는 false로 소비되므로 같은 입력 없이 다음 프레임 자동 재시도하지 않는다. (`Runtime/CutController.cpp:145`)

### 3d. 매 프레임 애니메이션 → CPU 스키닝 → 버퍼 → 그리기

1. `ModelViewer::Update` → `ModelInstance::Update` → `UpdateAnimations`.
   재생 상태의 animation time을 전진시키고 keyframe에서 translation/rotation/scale을 graph에 반영한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:389`, `MiniEngine/Model/Model.cpp:176`, `MiniEngine/Model/Animation.cpp:101`, `MiniEngine/Model/Animation.cpp:132`)
2. `ModelInstance::Update` → stale matrix 재생성 → scene graph 순회.
   node transform과 parent transform을 합성하고 node world를 CPU 배열과 mesh constant에 함께 쓴다. (`MiniEngine/Model/Model.cpp:183`, `MiniEngine/Model/Model.cpp:197`, `MiniEngine/Model/Model.cpp:205`)
3. `ModelInstance::Update` → skeleton loop → GPU mesh constants copy.
   joint.posXform=`jointNodeWorld * IBM`, nrmXform은 inverse transpose로 갱신된다. (`MiniEngine/Model/Model.cpp:240`, `MiniEngine/Model/Model.cpp:243`, `MiniEngine/Model/Model.cpp:249`)
4. `ModelViewer::Update` → context Finish → [미커밋] `CutController::Update`.
   CPU 포즈 확정 뒤 실행하며 먼저 FramePoseValid를 false로 하고 빈/실패 patch는 반환한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:391`, `MiniEngine/ModelViewer/ModelViewer.cpp:395`, `Runtime/CutController.cpp:108`)
5. [미커밋] `CutController::Update` → `BuildSkinMatrices` → `MakeSlotMatrices`.
   Source의 meshSlot 순서로 현재 meshWorld × skeleton.posXform 행렬 배열을 다시 만든다. (`Runtime/CutController.cpp:114`, `BonePatch.cpp:122`, `BonePatch.cpp:128`)
6. [미커밋] `CutController::Update` → `ComputePoseJointWorld`.
   평면용 현재 행렬은 첫 source mesh world × palette joint node world로 따로 만든다. (`Runtime/CutController.cpp:115`, `BonePatch.cpp:145`, `BonePatch.cpp:152`)
7. [미커밋] Update의 skin lambda → `SkinPatchPiece(m_PatchPiece)` → `SkinPoint`.
   필요한 origin만 LBS하고 각 PatchNode를 PatchOrigin의 bary 합으로 현재 포즈에 놓는다. (`Runtime/CutController.cpp:125`, `Runtime/CutController.cpp:130`, `Geometry/Skinning.cpp:67`, `Geometry/Skinning.cpp:103`)
8. [미커밋] `UpdateBasePlane` → 필요 시 `UpdateSlice`; 그 외 기존 piece 스키닝.
   새 절단 성공 직후에는 결과가 이미 현재 포즈이므로 두 조각의 중복 스키닝을 건너뛴다. (`Runtime/CutController.cpp:138`, `Runtime/CutController.cpp:145`, `Runtime/CutController.cpp:148`)
9. [미커밋] `CutController::Update` 종료 → `m_FramePoseValid=true`.
   성공 종료한 프레임만 View의 매 프레임 position refresh가 수행된다. (`Runtime/CutController.cpp:169`, `Debug/CutDebugView.cpp:88`)
10. `ModelViewer::RenderScene` → 원본 모델 opaque pass → `RenderProceduralMeshes`.
    procedural 그림은 원본 opaque 뒤, transparent 앞에 삽입된다. (`MiniEngine/ModelViewer/ModelViewer.cpp:517`, `MiniEngine/ModelViewer/ModelViewer.cpp:523`, `MiniEngine/ModelViewer/ModelViewer.cpp:525`)
11. [미커밋] `RenderProceduralMeshes` → `CutDebugView::Draw` → `Sync`.
    새 generation은 full bake하고, 보통 프레임은 `UpdateRenderPositions` → `UpdateVertices`로 위치를 복사한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:550`, `Debug/CutDebugView.cpp:133`, `Debug/CutDebugView.cpp:97`, `Rendering/RenderBake.cpp:129`)
12. [미커밋] `Sync` → 필요 시 `BakeBoundaryDebug/BakePlaneDebug` → `ReplaceOrUpdate`.
    튜브는 다시 계산하되 indices와 정점 수가 같으면 vertex update를 사용한다. (`Debug/CutDebugView.cpp:91`, `Debug/CutDebugView.cpp:125`, `Debug/CutDebugView.cpp:26`)
13. [미커밋] `CutDebugView::Draw` → display world 계산 → renderer `Draw`.
    overlay가 아니면 옆으로 이동하고 두 조각은 자른 순간의 법선을 따라 추가로 벌려 그린다. (`Debug/CutDebugView.cpp:137`, `Debug/CutDebugView.cpp:169`, `Debug/CutDebugView.cpp:174`)
14. `ProceduralMeshRenderer::Draw` → 버전 비교 → `Acquire/Upload/Retire`.
    바뀐 vertex만 새 VB로 교체하고 topology가 같으면 기존 IB를 유지한다. (`Rendering/ProceduralMeshRenderer.cpp:186`, `Rendering/ProceduralMeshRenderer.cpp:194`)
15. `Draw` → GPU binding → `DrawIndexed` → [미커밋] 필요 시 wire Draw.
    각 section을 triangle-list로 그리고 U 모드에 따라 동일 메시를 와이어 PSO로 추가 제출한다. (`Rendering/ProceduralMeshRenderer.cpp:213`, `Rendering/ProceduralMeshRenderer.cpp:249`, `Debug/CutDebugView.cpp:153`)
16. `RenderScene` → `NotifySubmitted(gfxContext.Finish())`.
    이번 draw에 사용된 버퍼들에 실제 제출 펜스를 기록하고 완료된 retired 버퍼를 free로 회수한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:544`, `Rendering/ProceduralMeshRenderer.cpp:270`)

`SkinPatchPiece`는 필요한 origin·joint·slot을 검증하고 임시 positions를 완성한 뒤 해당 piece의 node.position을 한꺼번에 쓴다. (`Geometry/Skinning.cpp:47`, `Geometry/Skinning.cpp:90`, `Geometry/Skinning.cpp:110`)
이 보장은 piece 한 번의 호출 범위이며 Controller의 패치+두 piece 전체를 묶은 transaction은 아니다. (`Runtime/CutController.cpp:130`, `Runtime/CutController.cpp:152` [미커밋])
위치 갱신은 corner normal/UV/color를 재계산하지 않는다. (`Rendering/RenderBake.cpp:129`)
현재 procedural VS/PS 출력은 위치와 색을 쓰므로 보고서에서 조명 갱신 결과를 주장하지 않는다. (`Rendering/ProceduralMeshVS.hlsl:24`, `Rendering/ProceduralMeshPS.hlsl:23`)

### 3e. X·새 C·재절단·종료의 상태 정리

1. [미커밋] X 입력 → `CutController::ClearPatch` → `m_ExtractedMesh.Clear`.
   CPU mesh를 비우고 Source·문구를 초기화한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:308`, `Runtime/CutController.cpp:38`)
2. [미커밋] `ClearPatch` → `RebuildPatchPiece` → empty 조기 반환.
   PatchPiece·WeldStats·detail/status를 초기화한 뒤 `no patch`로 끝난다. (`Runtime/CutController.cpp:43`, `Runtime/CutController.cpp:79`, `Runtime/CutController.cpp:84`)
3. [미커밋] `ClearPatch` → `ClearSlice`.
   havePieces/dirty/status/detail/info를 초기화하고 양쪽 piece를 비우며 generation을 증가시킨다. (`Runtime/CutController.cpp:44`, `Runtime/CutController.cpp:249`)
4. [미커밋] `ClearPatch` → plane invalid / slice disabled / patchVersion 증가.
   다음 Update는 빈 patch로 빠지고 View는 두 버전 변화를 감지한다. (`Runtime/CutController.cpp:45`, `Runtime/CutController.cpp:109`, `Debug/CutDebugView.cpp:38`, `Debug/CutDebugView.cpp:56`)
5. [미커밋] 다음 `CutDebugView::Sync` → 표시 CPU 메시 Clear.
   patch·경계·두 piece·piece 경계·평면을 비운다. Clear는 GPU ReleaseMesh를 직접 호출하지 않는다. (`Debug/CutDebugView.cpp:41`, `Debug/CutDebugView.cpp:60`, `ProceduralMesh.cpp:183`)
6. 그려지는 빈 메시의 renderer `Draw` → buffer `Retire`.
   해당 mesh entry의 VB/IB를 retired로 옮기고 draw 없이 반환한다. (`Rendering/ProceduralMeshRenderer.cpp:175`)
7. [미커밋] 새 C 성공 → `SetPatch` → `ClearSlice` → 새 plane 계산.
   기존 조각을 먼저 없애고, slice enabled가 유지되어 있으면 dirty를 켜 같은 프레임에 새 패치를 자를 수 있다. (`Runtime/CutController.cpp:25`, `Runtime/CutController.cpp:26`, `Runtime/CutController.cpp:30`)
8. [미커밋] 평면/캡 조작 → `UpdateSlice` 성공 → 기존 sliced piece 교체.
   기존 piece를 입력으로 재귀 절단하지 않고 항상 m_PatchPiece를 입력으로 쓴다. (`Runtime/CutController.cpp:272`, `Runtime/CutController.cpp:282`)
9. [미커밋] 재절단 실패 → 상태/detail만 갱신 → 기존 piece 계속 스키닝.
   generation은 증가하지 않고 이전 성공 조각이 계속 표시된다. (`Runtime/CutController.cpp:274`, `Runtime/CutController.cpp:148`, `Runtime/CutController.cpp:284`)
10. [미커밋] T off → `ToggleSlice` → View의 원본 패치 표시 분기.
    두 piece는 Clear되지 않고, enabled 상태일 때만 기존 piece 스키닝/표시 분기에 들어간다. (`Runtime/CutController.cpp:50`, `Runtime/CutController.cpp:148`, `Debug/CutDebugView.cpp:165`, `Debug/CutDebugView.cpp:182`)
11. [미커밋] 앱 종료 `Cleanup` → `CutDebugView::Release` → `ReleaseMesh`.
    View의 일곱 메시 등록을 renderer에서 제거하고 버퍼를 retire한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:253`, `Debug/CutDebugView.cpp:226`, `Rendering/ProceduralMeshRenderer.cpp:282`)
12. `Cleanup` → renderer `Shutdown` → graphics queue idle → 컨테이너 clear.
    GPU 사용 완료를 기다린 뒤 실제 소유 참조를 제거하고 ModelInstance도 nullptr로 바꾼다. (`MiniEngine/ModelViewer/ModelViewer.cpp:254`, `Rendering/ProceduralMeshRenderer.cpp:298`, `MiniEngine/ModelViewer/ModelViewer.cpp:255`)

X를 눌렀다고 모든 숨겨진 메시의 GPU 메모리가 즉시 free로 간다고 읽지 않는다. (`Debug/CutDebugView.cpp:60`, `Rendering/ProceduralMeshRenderer.cpp:175` [미커밋])
그 mesh가 Draw/ReleaseMesh에 도달해야 renderer의 소유 참조가 retire되고, fence 완료 조건도 필요하다. (`Rendering/ProceduralMeshRenderer.cpp:177`, `Rendering/ProceduralMeshRenderer.cpp:287`, `Rendering/ProceduralMeshRenderer.cpp:125`)

## 4. 키 입력 표

아래 본/패치 키는 모델 존재·bone debug enabled·palette 범위 조건 안에서 처리된다. (`MiniEngine/ModelViewer/ModelViewer.cpp:277`)
B로 debug를 끈 다음의 프레임에는 C/X 등의 입력도 이 분기로 들어오지 않는다. B 재활성화는 별도 else-if다. (`MiniEngine/ModelViewer/ModelViewer.cpp:280`, `MiniEngine/ModelViewer/ModelViewer.cpp:346`)
T/M/L/Z/R/평면 이동은 추가로 PlaneBaseValid 조건이 필요하다. (`MiniEngine/ModelViewer/ModelViewer.cpp:317` [미커밋])

| 키 | 호출/변경 지점 | 결과 | 근거 |
| --- | --- | --- | --- |
| B | bone debug flag | debug off 또는 유효 palette에서 다시 on | `MiniEngine/ModelViewer/ModelViewer.cpp:280`, `MiniEngine/ModelViewer/ModelViewer.cpp:346` |
| J / K | jointIndex 계산 | 이전/다음 joint 순환; 복사된 Source 선택은 바꾸지 않음 | `MiniEngine/ModelViewer/ModelViewer.cpp:288`, `Runtime/CutController.cpp:21` [미커밋] |
| I | 다음 palette 탐색 | joint가 있는 palette 선택, jointIndex=0 | `MiniEngine/ModelViewer/ModelViewer.cpp:293` |
| H | weight debug flag → SetBoneDebug | 원본 모델의 선택 본 weight 표시 | `MiniEngine/ModelViewer/ModelViewer.cpp:282`, `MiniEngine/ModelViewer/ModelViewer.cpp:472` |
| V | wireframe flag → SetWireframe | 원본 모델 MeshSorter 경로의 wire 표시 | `MiniEngine/ModelViewer/ModelViewer.cpp:284`, `MiniEngine/ModelViewer/ModelViewer.cpp:473` |
| C | SetPatch | 패치 추출·용접·평면 초기화·이전 조각 정리 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:306`, `Runtime/CutController.cpp:12` |
| X | ClearPatch | 패치/조각 삭제, slice off [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:308`, `Runtime/CutController.cpp:38` |
| T | ToggleSlice | enabled 반전·dirty 설정; on이면 Update에서 절단 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:320`, `Runtime/CutController.cpp:50`, `Runtime/CutController.cpp:141` |
| M | ToggleCap | cap Create/None 반전, dirty로 재시도 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:322`, `Runtime/CutController.cpp:56`, `Runtime/CutController.cpp:270` |
| R | ResetPlane | offset/tilt 모두 0, dirty 설정 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:328`, `Runtime/CutController.cpp:62` |
| `[` / `]` | NudgePlane | offset 감소/증가 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:331`, `Runtime/CutController.cpp:68` |
| `,` / `.` | NudgePlane | tilt A 감소/증가 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:335` |
| `;` / `'` | NudgePlane | tilt B 감소/증가 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:339` |
| G | ToggleBoundary | 경계 튜브 표시 on/off [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:310`, `Debug/CutDebugView.h:20`, `Debug/CutDebugView.cpp:176` |
| N | ToggleOverlay | 옆 복사/원본 위치 overlay 전환; gap도 0 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:312`, `Debug/CutDebugView.cpp:137`, `Debug/CutDebugView.cpp:169` |
| U | CycleWireMode | Solid → SolidAndWire → Wire 순환 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:314`, `Debug/CutDebugView.h:26`, `Debug/CutDebugView.cpp:153` |
| L | ToggleBackCull | 두 조각 draw options의 cull 전환 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:324`, `Debug/CutDebugView.cpp:140`, `Debug/CutDebugView.cpp:174` |
| Z | ToggleSectionTint | 두 조각의 section별 tint 전환 [미커밋] | `MiniEngine/ModelViewer/ModelViewer.cpp:326`, `Debug/CutDebugView.cpp:141`, `Debug/CutDebugView.cpp:174` |
| Space | PauseAnimation / PlayAnimation | 재생 mode를 보관해 일시정지·복원 | `MiniEngine/ModelViewer/ModelViewer.cpp:351`, `MiniEngine/ModelViewer/ModelViewer.cpp:362`, `MiniEngine/ModelViewer/ModelViewer.cpp:373` |

평면 조정 여섯 키는 `IsPressed`라 누르는 동안 반복되고, 이동량은 `radius * 0.4 * deltaT`, 회전량은 `0.8 * deltaT`다. (`MiniEngine/ModelViewer/ModelViewer.cpp:330` [미커밋])
나머지 표의 toggle/선택 키는 `IsFirstPressed` 경로다. (`MiniEngine/ModelViewer/ModelViewer.cpp:280`, `MiniEngine/ModelViewer/ModelViewer.cpp:306`, `MiniEngine/ModelViewer/ModelViewer.cpp:351`)
현재 절단용 Enter/Backspace 매핑은 확인 못 함. 설계의 RequestCut/DiscardCut 대신 ToggleSlice·dirty 흐름을 사용한다. (`Docs/CutDebugDesign.md:89`, `Runtime/CutController.h:32`, `Runtime/CutController.cpp:141` [미커밋])

## 5. 좌표계와 행렬

### 5.1 인덱스 네 종류부터 분리

paletteId는 Model의 `m_SkinPalettes` 번호다. (`BonePatch.cpp:163`)
jointIndex와 VB의 joints는 그 palette 내부의 local joint 번호다. (`MiniEngine/Model/Model.h:104`, `BonePatch.h:33`)
flat joint 번호는 `palette.startJoint + j`이고, joint node 번호는 `m_JointIndices[flat]`다. (`BonePatch.cpp:149`)
meshSlot은 Source.meshes의 순서이며 Model meshIndex나 scene node 번호와 다르다. (`BonePatch.cpp:241`, `BonePatch.cpp:377`)
worldMatrixIndex는 mesh.meshCBV를 저장한 값으로 instance node world 배열을 가리킨다. (`BonePatch.h:21`, `BonePatch.cpp:128`)

### 5.2 bind → posed patch/world

아래 수식은 코드의 `Math::Matrix4` 표기 순서를 그대로 쓴다.
`S_j = nodeWorld[jointNode] * IBM_j`는 skeleton.posXform이다. (`MiniEngine/Model/Model.cpp:243`)
`M_slot,j = meshWorld_slot * S_j`는 SkinPoint에 전달되는 행렬이다. (`BonePatch.cpp:108`)
`p_origin = Σ weight_j · Transform(bindPosition, M_slot,j)`가 LBS 결과다. (`Geometry/Skinning.cpp:25`, `Geometry/Skinning.cpp:32`)
`p_node = Σ bary_i · p_origin[i]`가 절단 후 노드의 현재 위치다. (`Geometry/Skinning.cpp:101`)

| 데이터 | 출발 공간 | 도착/사용 공간 | 주의할 차이 | 근거 |
| --- | --- | --- | --- | --- |
| BonePatchVertex.position / PatchSkin.bindPosition | 원본 mesh pre-skin | LBS 입력 | 이미 world인 PatchNode.position과 다름 | `BonePatch.cpp:280`, `Geometry/Skinning.cpp:83` |
| skeleton.posXform | mesh bind 위치 | joint pose가 반영된 스키닝 좌표 | IBM 포함 | `MiniEngine/Model/Model.cpp:243` |
| slotMatrices | bind 위치 | posed patch/world | meshWorld를 추가 포함 | `BonePatch.cpp:108`, `BonePatch.h:66` |
| poseJointWorld | joint-local 프레임 | posed patch/world | IBM을 곱하지 않음; 평면 부착용 | `BonePatch.cpp:360`, `Runtime/CutController.cpp:213` [미커밋] |
| CurrentPoseJointWorld | 현재 joint-local 프레임 | 현재 patch/world | 매 프레임 첫 source mesh world를 사용 | `BonePatch.cpp:145`, `BonePatch.cpp:152` |
| Render World | 이미 world인 정점 | 표시용 평행이동을 더한 world | 원본 mesh world를 다시 적용하지 않음 | `Debug/CutDebugView.cpp:135`, `Debug/CutDebugView.cpp:138` [미커밋] |

`GetNodeWorldMatrices`라는 이름만 보고 모든 node에 locator가 동일하게 적용된다고 단정하지 않는다.
Update는 `skeletonRoot`일 때 parent 합성을 생략한다. 실제 최종 관계는 위의 두 곱을 기준으로 읽는다. (`MiniEngine/Model/Model.cpp:197`, `BonePatch.cpp:108`, `BonePatch.cpp:152`)
DirectXMath의 `SkinPoint`는 `XMVector4Transform`의 row-vector API를 쓴다. (`Geometry/Skinning.h:7`, `Geometry/Skinning.cpp:32`)
shader는 `mul(ViewProj, mul(World, position))`로 쓴다. 함수 표기 차이를 보고 행렬을 임의로 뒤집지 말고 실제 저장·호출 경로를 확인한다. (`Rendering/ProceduralMeshVS.hlsl:24`, `BonePatch.cpp:108`)

### 5.3 평면 프레임의 왕복 [미커밋]

`ComputeBasePlane`은 선택 본의 translation을 origin으로 삼는다. (`Runtime/CutController.cpp:183`)
같은 palette에서 첫 직계 자식 방향을 먼저 찾고, 없으면 부모에서 본으로 향하는 방향을 찾는다. (`Runtime/CutController.cpp:187`, `Runtime/CutController.cpp:196`)
방향을 못 찾거나 길이가 작으면 world Y를 사용한다. (`Runtime/CutController.cpp:202`)
up과 평행에 가까우면 다른 up을 선택한 뒤 Cross로 기울기 축 A/B를 만든다. (`Runtime/CutController.cpp:206`)

| 저장/복원 값 | 코드에서 수행하는 변환 | 근거 |
| --- | --- | --- |
| local point | inverse(poseJointWorld) × origin | `Runtime/CutController.cpp:213`, `Runtime/CutController.cpp:215` |
| local normal | transpose(poseJointWorld.3x3) × direction 후 Normalize | `Runtime/CutController.cpp:218` |
| local A/B | inverse(poseJointWorld).3x3 × world A/B 후 Normalize | `Runtime/CutController.cpp:219` |
| current world point | currentJointWorld × local point | `Runtime/CutController.cpp:233` |
| current world normal | inverse-transpose(currentJointWorld.3x3) × local normal 후 Normalize | `Runtime/CutController.cpp:234` |
| current world A/B | currentJointWorld.3x3 × local A/B 후 Normalize | `Runtime/CutController.cpp:235` |
| 최종 normal | A축 tiltA 적용 후 B축 tiltB 적용 후 Normalize | `Runtime/CutController.cpp:241` |
| 최종 point | base point + base normal × offset | `Runtime/CutController.cpp:242` |

offset 이동 방향은 기울인 최종 normal이 아니라 base normal이다. (`Runtime/CutController.cpp:241`, `Runtime/CutController.cpp:242` [미커밋])
world에서 만든 프레임을 joint-local로 저장하고 다시 현재 joint pose로 옮기므로 bone twist도 축 변환에 포함된다. (`Runtime/CutController.cpp:219`, `Runtime/CutController.cpp:235` [미커밋])

### 5.4 평면 2D와 cap의 공간

`MakePlaneBasis`는 입력 normal을 정규화하고 가장 덜 정렬된 좌표축으로 u를 만든 뒤 v=n×u를 만든다. (`Geometry/Patch.cpp:130`, `Geometry/Patch.cpp:135`, `Geometry/Patch.cpp:147`)
투영은 `dot(p-origin,u)`, `dot(p-origin,v)`이며 loop와 cap이 같은 basis 생성 함수를 사용한다. (`Geometry/SliceCap.cpp:172`, `Geometry/SliceCap.cpp:228`)
positive piece의 바깥 cap normal은 -n, negative는 +n이다. (`Geometry/SliceCap.cpp:279`)
cap UV는 이 2D 투영에 capUvScale을 곱한 값이다. (`Geometry/SliceCap.cpp:292`)
cap의 부착 예약 필드(attachJoint, attachJointWorldAtCut)는 생산자와 소비자가 없어 `PatchCap`에서 삭제했다. 강체 부착 실행 경로는 없다.
cap 위치는 Surface node를 공유하므로 스키닝 후 위치가 변해도 record의 planePoint/planeNormal을 매 프레임 재작성하지 않는다. (`Geometry/SliceCap.cpp:290`, `Geometry/Skinning.cpp:110`)

## 6. GPU 경계와 버퍼 수명

### 6.1 CPU 변경을 GPU 변경으로 전달하는 표식

`ProceduralMesh`는 GPU 버퍼를 직접 만들지 않는다. CPU data와 bounds, vertex/topology version을 보관한다. (`ProceduralMesh.h:55`)
`SetMesh`는 모든 속성과 topology를 검증한 뒤 두 version을 바꾼다. (`ProceduralMesh.cpp:147`)
`UpdateVertices`는 기존 정점 수를 요구하고 bounds를 다시 계산하며 vertexVersion만 바꾼다. (`ProceduralMesh.cpp:162`)
`Clear`는 CPU arrays/bounds를 비우고 두 version을 바꾼다. (`ProceduralMesh.cpp:183`)
version 번호는 모든 mesh에 공통인 atomic 증가 값이다. (`ProceduralMesh.cpp:15`)

### 6.2 등록과 최초 생성

1. `Draw` → `Initialize`; target format이 새로 정해지면 PSO Finalize.
   Initialize 시점에는 mesh별 VB/IB를 만들지 않는다. (`Rendering/ProceduralMeshRenderer.cpp:41`, `Rendering/ProceduralMeshRenderer.cpp:158`)
2. `Draw` → `m_Meshes[&mesh]`.
   CPU mesh 주소가 등록 키이고 MeshGpu는 VB/IB 포인터와 마지막 업로드 버전을 가진다. (`Rendering/ProceduralMeshRenderer.cpp:174`, `Rendering/ProceduralMeshRenderer.h:63`)
3. 빈 mesh면 `Retire` 후 반환, 비어 있지 않으면 필요한 바이트 수 계산.
   정점 stride는 48바이트, index는 uint32다. (`Rendering/ProceduralMeshRenderer.cpp:175`, `Rendering/ProceduralMeshRenderer.cpp:184`, `Rendering/ProceduralMeshRenderer.cpp:59`)
4. VB/IB가 없거나 버전이 다르면 `Acquire` → `Upload`.
   최초 draw와 full SetMesh 후에는 두 버퍼가 필요하고, UpdateVertices 후에는 VB만 필요하다. (`Rendering/ProceduralMeshRenderer.cpp:186`, `Rendering/ProceduralMeshRenderer.cpp:194`)

### 6.3 Acquire → Upload → Retire

`Acquire`는 먼저 Collect로 완료된 retired 버퍼를 free 목록으로 옮긴다. (`Rendering/ProceduralMeshRenderer.cpp:95`)
요청 크기를 16바이트 단위로 올리고 free에서 용량이 충분한 첫 버퍼를 선택한다. (`Rendering/ProceduralMeshRenderer.cpp:98`, `Rendering/ProceduralMeshRenderer.cpp:102`)
없으면 ByteAddressBuffer를 새로 만든다. 버퍼를 같은 자리에서 매번 덮어쓰는 방식이 아니다. (`Rendering/ProceduralMeshRenderer.cpp:113`, `Rendering/ProceduralMeshRenderer.cpp:188`)
Upload는 16바이트 정렬 임시 메모리에 padding을 0으로 채우고 context.WriteBuffer를 호출한다. (`Rendering/ProceduralMeshRenderer.cpp:143`)
WriteBuffer는 Core의 CPU linear allocator 메모리로 복사한 뒤 CopyBufferRegion을 기록한다. (`MiniEngine/Core/CommandContext.cpp:451`)
그러므로 renderer의 `_aligned_free(staging)` 이후 실제 GPU copy source는 Core 임시 업로드 공간이다. (`Rendering/ProceduralMeshRenderer.cpp:149`, `MiniEngine/Core/CommandContext.cpp:454`)
기존 버퍼는 Retire 목록에 보관하고 새 버퍼로 entry를 교체한다. (`Rendering/ProceduralMeshRenderer.cpp:190`, `Rendering/ProceduralMeshRenderer.cpp:198`)

### 6.4 draw와 제출 펜스

VB/IB를 각각 vertex/index state로 전환하고 scene color/depth target, root signature, PSO, transform CBV를 바인딩한다. (`Rendering/ProceduralMeshRenderer.cpp:203`)
section이 있으면 각 section의 firstIndex/indexCount로 DrawIndexed한다. materialSlot을 재질 로딩에 사용하지 않는다. (`Rendering/ProceduralMeshRenderer.cpp:249`)
[미커밋] tint는 section의 vector 순번을 사용하며 wire에서는 단색 mode 2 또는 팔레트 mode 3을 보낸다. (`Rendering/ProceduralMeshRenderer.cpp:225`, `Rendering/ProceduralMeshRenderer.cpp:253`)
사용한 VB/IB는 pending=true로 설정하고 pending vector에 한 번씩 넣는다. (`Rendering/ProceduralMeshRenderer.cpp:260`)
`gfxContext.Finish()`는 command list를 queue에 제출하고 FenceValue를 반환한다. (`MiniEngine/Core/CommandContext.cpp:145`, `MiniEngine/Core/CommandContext.cpp:159`)
ModelViewer는 이 값을 그대로 `NotifySubmitted`에 전달한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:544`)
NotifySubmitted는 pending 버퍼의 fence를 max(기존 값, 제출 값)로 바꾸고 pending을 해제한다. (`Rendering/ProceduralMeshRenderer.cpp:270`)

### 6.5 해제라는 말의 두 단계

| 동작 | 실제 변화 | 근거 |
| --- | --- | --- |
| Retire | shared_ptr를 retired에 보관; 즉시 파괴 아님 | `Rendering/ProceduralMeshRenderer.cpp:135` |
| Collect | pending이 아니고 fence가 0 또는 완료이면 retired → free | `Rendering/ProceduralMeshRenderer.cpp:125` |
| Acquire 재사용 | free에서 하나를 빼서 새 업로드 대상에 사용 | `Rendering/ProceduralMeshRenderer.cpp:106` |
| ReleaseMesh | 주소 등록 제거와 VB/IB retire, 이후 Collect | `Rendering/ProceduralMeshRenderer.cpp:282` |
| Shutdown | graphics queue idle 뒤 pending/map/retired/free 참조 제거 | `Rendering/ProceduralMeshRenderer.cpp:293` |

Core의 임시 업로드 페이지 역시 Finish에서 FenceValue로 정리 요청을 받는다. (`MiniEngine/Core/CommandContext.cpp:149`)
한 프레임 안 solid와 wire 두 번 그려도 같은 버전이면 추가 업로드 분기는 타지 않는다. (`Rendering/ProceduralMeshRenderer.cpp:186`, `Debug/CutDebugView.cpp:153` [미커밋])
주소를 등록 키로 쓰므로 mesh 생명주기 종료 전에 ReleaseMesh가 필요하다는 계약이 있다. (`Rendering/ProceduralMeshRenderer.h:18`, `ProceduralMesh.h:41`)
[미커밋] 현재 View는 종료 시 자신의 일곱 메시를 ReleaseMesh로 반환한다. (`Debug/CutDebugView.cpp:226`)

### 6.6 렌더 상태와 디버그 표시 [미커밋]

solid는 TwoSided/BackCull 두 PSO, wire도 같은 두 cull 조합으로 총 네 PSO를 가진다. (`Rendering/ProceduralMeshRenderer.h:75`)
wire는 WIREFRAME와 DepthStateReadOnly, DepthBias 200, slope bias 1.5를 설정한다. (`Rendering/ProceduralMeshRenderer.cpp:24`, `Rendering/ProceduralMeshRenderer.cpp:78`)
이 값이 모든 카메라에서 어떤 시각 결과를 내는지는 이 보고서에서 확인 못 함.
U의 solid+wire는 solid 먼저, 같은 world의 wire 다음으로 제출한다. (`Debug/CutDebugView.cpp:153`)
패치 단독 표시 분기는 기본 DrawOptions를 넘기고, 두 조각 분기에만 pieceOptions를 넘긴다. (`Debug/CutDebugView.cpp:174`, `Debug/CutDebugView.cpp:184`)
경계 디버그는 CapSeam을 제외하며 Replacement는 노랑, UnweldedSeam은 초록, Cut은 주황이다. (`Rendering/RenderBake.cpp:13`, `Rendering/RenderBake.cpp:75`)
평면은 48분할 원과 교차선, positive 방향 화살표로 굽는다. (`Rendering/RenderBake.cpp:148`, `Rendering/RenderBake.cpp:156`)
procedural draw 후 필요할 때 linear depth를 다시 계산하고 이후 velocity/TAA 경로로 넘어간다. (`MiniEngine/ModelViewer/ModelViewer.cpp:554`, `MiniEngine/ModelViewer/ModelViewer.cpp:532`)

## 7. 테스트가 검증하는 것과 경계

### 7.1 실행 방식과 이번 보고서의 검증 상태

`tests/PlaneSlicerTests.cpp`는 별도 main을 가진 CPU 테스트다. D3D12 device가 필요 없는 빌드 명령을 파일 머리에 적어두었다. (`tests/PlaneSlicerTests.cpp:1`)
main은 각 테스트를 호출한 후 check/failure 수를 출력하고 실패 수를 반환한다. (`tests/PlaneSlicerTests.cpp:1413`, `tests/PlaneSlicerTests.cpp:1460`)
MeshCutting.vcxproj에서 이 파일은 compile 대상이 아니라 None이다. 일반 프로젝트 빌드만으로 이 main이 실행되었다고 판단하지 않는다. (`MeshCutting.vcxproj:203`)
이번 작업에서는 산출물 생성 금지에 따라 실행하지 않았다. 테스트 통과 여부는 확인 못 함.

### 7.2 사례별 검증 내용

아래는 테스트에 적힌 입력과 CHECK의 의미다. 실제 실행 통과 결과를 뜻하지 않는다.

| 테스트 | 검사하는 계약 | 근거 |
| --- | --- | --- |
| TestWeldBox | 8 nodes/12 faces, 닫힌 경계와 sourceTriangle/origin 대응 | `tests/PlaneSlicerTests.cpp:195` |
| TestWeldUvSeamTube | 18 corners를 16 nodes로 용접, UV 0/1은 보존 | `tests/PlaneSlicerTests.cpp:208` |
| TestWeldSkinSplitSeam | joint가 다른 seam은 18 nodes로 유지, seam 에지 둘 | `tests/PlaneSlicerTests.cpp:226` |
| TestWeldNegativeZero | bind -0/+0 중복을 같은 node로 묶는 통계 | `tests/PlaneSlicerTests.cpp:241` |
| TestInputRejects | NaN, bad index, 0 weight, bad origin/corner/face, 퇴화, 비다양체 거절 | `tests/PlaneSlicerTests.cpp:257` |
| TestUntaggedBoundaryIsRejected | 열린 edge의 태그 누락/CapSeam 오표기 거절 | `tests/PlaneSlicerTests.cpp:340` |
| TestValidatePlane | 기본 평면 허용, zero normal/무한 point 거절 | `tests/PlaneSlicerTests.cpp:351` |
| TestOpenGridBoundaryKinds | 3×3 grid의 Replacement perimeter 12 | `tests/PlaneSlicerTests.cpp:363` |
| TestBoundaryDebugBake | 경계 튜브 정점/인덱스 개수와 SetMesh 수용 | `tests/PlaneSlicerTests.cpp:385` |
| TestBoxCenterCut | 양쪽 node/cut 수, 표면 면적 합, 평면 위 위치, 8-node loop 면적 4 | `tests/PlaneSlicerTests.cpp:456` |
| TestPlaneThroughNodes | 기존 equator 4 nodes로 절단, 새 cut node 없이 loop 생성 | `tests/PlaneSlicerTests.cpp:508` |
| TestOneNodeOnPlane | 0/+/- 분기와 기존 node를 지나는 4-node loop | `tests/PlaneSlicerTests.cpp:527` |
| TestOpenSheetIsOpenLoop | 열린 sheet 절단 실패, Replacement 진단과 빈 출력 | `tests/PlaneSlicerTests.cpp:544` |
| TestTouchingIsNoCut / TestMissIsNoCut | 접촉만 하거나 빗나간 평면이 NoCut | `tests/PlaneSlicerTests.cpp:572`, `tests/PlaneSlicerTests.cpp:600` |
| TestCoplanarFace | 평면에 놓인 face가 CoplanarFace | `tests/PlaneSlicerTests.cpp:586` |
| TestFailureClearsOut | 이전 결과가 있던 out도 실패 시 결과를 비움 | `tests/PlaneSlicerTests.cpp:610` |
| TestBakeSharesNodePositions | 같은 node의 corner 위치 대응과 surface/cap section | `tests/PlaneSlicerTests.cpp:634` |
| TestOriginLimit / TestCapFaceIsNotCrossed | origin 3개 제한과 기존 cap 교차 거절 | `tests/PlaneSlicerTests.cpp:659`, `tests/PlaneSlicerTests.cpp:674` |
| TestTiltedCutKeepsArea / TestSliverOutputIsRejected | 기울어진 절단의 면적 합과 퇴화 출력 거절 | `tests/PlaneSlicerTests.cpp:685`, `tests/PlaneSlicerTests.cpp:699` |
| TestUvSeamTubeCut | seam 위치 연속성과 절단 UV 보간 | `tests/PlaneSlicerTests.cpp:818` |
| TestSkinSplitSeamIsOpenLoop | 스킨이 다른 seam을 넘는 절단의 OpenLoop | `tests/PlaneSlicerTests.cpp:861` |
| TestThickTubeHoleLoops | 외곽/구멍 두 loop의 깊이와 반대 방향 | `tests/PlaneSlicerTests.cpp:874` |
| TestSharedNodeIsNonManifoldLoop / TestInvalidLoops | 공유 node의 분기, 교차·잘못된 중첩/방향 거절 | `tests/PlaneSlicerTests.cpp:895`, `tests/PlaneSlicerTests.cpp:921` |
| TestCapIsWatertight | cap 에지 짝·면적·법선·UV·section | `tests/PlaneSlicerTests.cpp:1008` |
| TestCapWithHole / TestCapOfConcaveLoop | 구멍/오목 루프의 cap과 열린 에지 검사 | `tests/PlaneSlicerTests.cpp:1043`, `tests/PlaneSlicerTests.cpp:1062` |
| TestRecut | 이전 cap을 피해 재절단, cap record/seam 계승과 cap 교차 실패 | `tests/PlaneSlicerTests.cpp:1096` |
| TestSkinIdentity / TestSkinSingleJoint | bind 복원과 회전+이동 기대 좌표 | `tests/PlaneSlicerTests.cpp:1222`, `tests/PlaneSlicerTests.cpp:1236` |
| TestSkinTwoJointBlend | 0.25/0.75 blend의 수계산 결과와 SkinPoint 일치 | `tests/PlaneSlicerTests.cpp:1253` |
| TestSkinMatchesSliceNodes | 동일 포즈 재스키닝이 radius 비례 허용오차 안에서 절단 위치 복원 | `tests/PlaneSlicerTests.cpp:1276` |
| TestSkinKeepsSeamsAndCapsTogether | 포즈 변경 뒤 seam/cap corner가 같은 node 위치 사용 | `tests/PlaneSlicerTests.cpp:1299` |
| TestSkinRefusesSkinState / TestSkinRefusesBadInput | 미지원 skinState/잘못된 joint·slot·origin 거절과 위치 불변 | `tests/PlaneSlicerTests.cpp:1341`, `tests/PlaneSlicerTests.cpp:1394` |
| TestSkinAfterTwoCuts | 두 번째 절단 piece의 재스키닝과 다른 포즈에서 이동 | `tests/PlaneSlicerTests.cpp:1353` |

### 7.3 이 테스트만으로 확인할 수 없는 것

테스트 main은 Geometry 입력 생성과 CPU 함수 호출로 구성된다. 실제 Model 로드·ExtractBonePatch·instance 행렬 연결은 여기서 검증하지 않는다. (`tests/PlaneSlicerTests.cpp:1413`, `BonePatch.cpp:157`)
[미커밋] Controller의 키/dirty/generation 수명과 View Sync는 main 호출 목록에 없다. (`tests/PlaneSlicerTests.cpp:1415`, `Runtime/CutController.cpp:106`, `Debug/CutDebugView.cpp:33`)
GPU VB/IB 업로드·펜스·PSO·와이어 표시 결과도 이 CPU main의 검사 대상이 아니다. (`tests/PlaneSlicerTests.cpp:1`, `Rendering/ProceduralMeshRenderer.cpp:153`)
TestRecut은 API에 이전 piece를 넘기는 검증이며, 데모가 이전 piece를 자르는 기능을 제공한다는 뜻은 아니다. (`tests/PlaneSlicerTests.cpp:1104`, `Runtime/CutController.cpp:272` [미커밋])
TestSkinMatchesSliceNodes는 tolerance 검증이고 비트 동일성 검사가 아니다. (`tests/PlaneSlicerTests.cpp:1290`)
TestSkinKeepsSeamsAndCapsTogether의 SamePosition은 float `==` 비교다. signed zero까지 포함한 memcmp 비트 검사는 아니다. (`tests/PlaneSlicerTests.cpp:1216`, `tests/PlaneSlicerTests.cpp:1331`)
실제 CesiumMan의 seam 개수·잘리는 본·프레임 시간·모든 포즈의 cap 모양은 이 보고서에서 확인 못 함.
물리적 분리 결과는 테스트가 다루지 않고, nonzero skinState도 현재는 거절한다. (`tests/PlaneSlicerTests.cpp:1347`, `Geometry/Skinning.cpp:44`)

### 7.4 별도 smoke 파일

`ProceduralMeshSmoke.cpp`는 CPU mesh의 기본 생성·section 자동 생성·bounds·실패 불변성을 assert한다. (`tests/ProceduralMeshSmoke.cpp:24`, `tests/ProceduralMeshSmoke.cpp:32`)
정점만 바꿀 때 topology version이 유지되고 다른 mesh가 영향받지 않는지도 검사한다. (`tests/ProceduralMeshSmoke.cpp:49`)
Clear 후 비어 있는 상태와 재설정을 검사한다. GPU smoke test로 해석하지 않는다. (`tests/ProceduralMeshSmoke.cpp:58`)
이 파일도 이번 작업에서 실행하지 않았다. 실행 결과는 확인 못 함.

## 8. 추천 읽기 순서와 브레이크포인트

### 8.1 읽기 순서

먼저 `PatchGeometry.h`의 node/corner/face와 `BonePatch.h`의 Source 대응을 읽는다. (`Geometry/Patch.h:19`, `BonePatch.h:25`)
다음 ModelViewer 입력·Update 순서를 읽고 [미커밋] Controller의 SetPatch/Update/UpdateSlice로 이동한다. (`MiniEngine/ModelViewer/ModelViewer.cpp:271`, `Runtime/CutController.cpp:12`, `Runtime/CutController.cpp:106`)
그 뒤 BonePatch → PatchWeld → PlaneSlicer → CutLoops → CapBuilder 순서로 계산을 따라간다. (`BonePatch.cpp:157`, `Geometry/Patch.cpp:124`, `Geometry/Slice.cpp:453`, `Geometry/SliceCap.cpp:64`, `Geometry/SliceCap.cpp:208`)
마지막으로 PieceSkinning → RenderBake → [미커밋] View Sync → renderer Draw/NotifySubmitted를 읽는다. (`Geometry/Skinning.cpp:37`, `Rendering/RenderBake.cpp:92`, `Debug/CutDebugView.cpp:33`, `Rendering/ProceduralMeshRenderer.cpp:153`, `Rendering/ProceduralMeshRenderer.cpp:270`)

### 8.2 브레이크포인트 10곳

| 번호 | 위치 | 멈췄을 때 볼 것 | 다음 연결 |
| --- | --- | --- | --- |
| 1 | `MiniEngine/Model/ModelLoader.cpp:232` | geometrySize, CPU vector, mesh의 kHasSkin 보존 조건 | 같은 함수의 253행까지 step |
| 2 | `MiniEngine/ModelViewer/ModelViewer.cpp:307` [미커밋] | 현재 paletteId/jointIndex, 직전 Update가 끝난 pose | SetPatch → ExtractBonePatch |
| 3 | `BonePatch.cpp:336` | 선택 본 영향값, selected, subtree 진단과 선택의 차이 | addVertex → SkinPoint → outputs |
| 4 | `Geometry/Patch.cpp:181` | key의 slot/bind bits/joints/weights, 기존 node 재사용 여부 | corner/face 생성 → validation |
| 5 | `Runtime/CutController.cpp:141` [미커밋] | enabled/dirty/baseValid, m_PatchPiece.position이 이번 포즈인지 | UpdateSlice → SlicePatch |
| 6 | `Geometry/Slice.cpp:273` | face corner→node, m_Sign, 0/+/- 또는 lone 분기 | CutNodeOf/CutCorner/AddFace |
| 7 | `Geometry/SliceCap.cpp:119` | Cut 에지의 in/out degree, blocker, loops/depth | 투영 검증 → Run → BuildCap |
| 8 | `Geometry/SliceCap.cpp:300` | triangles가 rim node만 쓰는지, 양쪽 winding, CapSeam 검사 | 최종 Validate → result 게시 |
| 9 | `Geometry/Skinning.cpp:103` | node.origin이 가리키는 blend와 bary, 다음 프레임 위치 | UpdateRenderPositions → UpdateVertices |
| 10 | `Rendering/ProceduralMeshRenderer.cpp:186` | vertex/topology version, VB만/IB도 교체 여부, pending/fence | DrawIndexed → NotifySubmitted → Collect |

실제 한 번의 절단을 추적할 때 1~4로 C를 끝내고, 평면 조작 후 5~8로 계산을 끝내며, 다음 프레임 9~10으로 추종과 제출을 확인한다.
이 동선의 코드상 연결은 `Runtime/CutController.cpp:18`, `Runtime/CutController.cpp:97`, `Runtime/CutController.cpp:145`, `Debug/CutDebugView.cpp:74`, `Debug/CutDebugView.cpp:100` [미커밋]에 있다.
이 실행에서 어떤 본과 평면이 Ok인지 보장하는 실측 재현값은 확인 못 함.
실패하면 억지로 다음 단계로 넘기지 말고 result.status/detail과 3c의 실패 갈림길을 대조한다. (`Geometry/Slice.cpp:472`, `Geometry/Slice.cpp:521`, `Geometry/Slice.cpp:550`)

## 9. 스스로 확인하는 질문

### 질문 1. C로 복사한 직후 원본 VB 위치와 패치 position은 같은 공간인가?

<details>
<summary>답과 근거</summary>

아니다. VB에서 읽은 position은 pre-skin mesh 위치이고 패치 ProceduralVertex.position은 SkinPoint의 posed world 결과다.
원본 위치는 BonePatchVertex.position에 별도로 남아 다음 CPU 스키닝의 입력이 된다. (`BonePatch.cpp:252`, `BonePatch.cpp:257`, `BonePatch.cpp:271`, `BonePatch.cpp:280`)

</details>

### 질문 2. 선택 본의 자손 weight만 있는 삼각형도 C에 포함되는가?

<details>
<summary>답과 근거</summary>

선택 본 자체의 Influence가 threshold를 넘는 정점이 있어야 한다.
subtree는 미선택 삼각형의 mixedOutsideTriangles 진단에 쓰인다. (`BonePatch.cpp:336`, `BonePatch.cpp:338`)

</details>

### 질문 3. UV seam 양쪽 정점이 같은 node가 되는 조건은 무엇인가?

<details>
<summary>답과 근거</summary>

meshSlot, bind 위치 비트, canonical joint/weight 쌍이 같아야 한다. UV/normal/color는 용접 키에 없다.
corner는 둘로 남고 node만 공유할 수 있다. (`Geometry/Patch.cpp:169`, `Geometry/Patch.cpp:202`, `tests/PlaneSlicerTests.cpp:222`)

</details>

### 질문 4. 잘린 node의 joints와 weights를 보간하는가?

<details>
<summary>답과 근거</summary>

교차 node는 양 끝의 PatchOrigin을 합쳐 원본 node 최대 3개에 대한 bary를 저장한다.
다음 프레임에는 origin의 LBS 결과를 bary로 합친다. (`Geometry/Slice.cpp:80`, `Geometry/Slice.cpp:183`, `Geometry/Skinning.cpp:103`)

</details>

### 질문 5. cap의 정점은 따로 스키닝하는가?

<details>
<summary>답과 근거</summary>

현재 BuildCap은 새 cap corner가 기존 rim node를 가리키게 한다. cap 전용 PatchNode를 추가하지 않는다.
따라서 그 위치는 공유 Surface node의 스키닝 결과를 따른다. (`Geometry/SliceCap.cpp:289`, `Geometry/SliceCap.cpp:308`, `tests/PlaneSlicerTests.cpp:1310`)

</details>

### 질문 6. [미커밋] 애니메이션이 움직이면 매 프레임 다시 자르는가?

<details>
<summary>답과 근거</summary>

UpdateSlice에는 enabled·dirty·baseValid가 모두 필요하다. 애니메이션 포즈 갱신 자체는 dirty를 켜지 않는다.
보통 프레임에는 기존 두 piece를 SkinPatchPiece로 이동시킨다. (`Runtime/CutController.cpp:138`, `Runtime/CutController.cpp:141`, `Runtime/CutController.cpp:148`)

</details>

### 질문 7. [미커밋] 평면을 옮긴 결과가 OpenLoop면 화면 조각은 어떻게 되는가?

<details>
<summary>답과 근거</summary>

SlicePatch의 임시 출력은 비워지고, Controller는 이전 성공 piece를 유지한다.
dirty는 소비되며 이전 piece는 계속 스키닝된다. (`Geometry/Slice.cpp:577`, `Runtime/CutController.cpp:274`, `Runtime/CutController.cpp:146`, `Runtime/CutController.cpp:148`)

</details>

### 질문 8. CPU 정점이 바뀐 순간 GPU가 쓰던 VB를 즉시 덮어쓰는가?

<details>
<summary>답과 근거</summary>

Draw에서 버전 차이를 보면 replacement 버퍼를 Acquire/Upload하고 예전 버퍼를 Retire한다.
pending 제출과 fence 완료 조건을 통과한 예전 버퍼만 free 목록으로 들어가 재사용된다. (`Rendering/ProceduralMeshRenderer.cpp:186`, `Rendering/ProceduralMeshRenderer.cpp:125`, `Rendering/ProceduralMeshRenderer.cpp:270`)

</details>

## 10. 문서-코드 불일치 / 버그 의심 목록

### 10.1 문서-코드 불일치

아래에서 설계 문서 후반 구현 노트가 앞의 설계를 정정한 경우도 함께 적는다.
이 경우는 미구현이라고 판정하지 않고, 실제 구현을 읽을 때 참고할 차이로 구분한다.

| 문서 서술 | 현재 코드 기준 | 판정과 근거 |
| --- | --- | --- |
| PlaneSlicingDesign 첫머리의 “아래 코드는 아직 없음” | Geometry 파이프라인과 SkinPatchPiece 구현이 존재 | 문서 상태 문구와 구현 노트가 병존: `Docs/PlaneSlicingDesign.md:7`, `Docs/PlaneSlicingDesign.md:338`, `Geometry/Slice.cpp:453` |
| 초기 표의 양면 PSO 하나 | solid 양면/후면 + [미커밋] wire 양면/후면 | 오래된 출발점 서술: `Docs/PlaneSlicingDesign.md:14`, `Rendering/ProceduralMeshRenderer.h:75` |
| mapbox earcut + T-junction 사후 보정 | 자체 BridgeHole/EarClip, rim 에지 짝 검사 | 후반 구현 노트가 정정: `Docs/PlaneSlicingDesign.md:95`, `Docs/PlaneSlicingDesign.md:327`, `Geometry/SliceCap.cpp:144`, `Geometry/SliceCap.cpp:324` |
| CapDrive 선택·부착 데이터 미리 저장 | CapDrive 선택 타입 확인 못 함; 부착 데이터는 PatchCap에서 삭제함 | FollowRim 공유 노드만 확인: `Docs/PlaneSlicingDesign.md:91`, `Docs/PlaneSlicingDesign.md:93`, `Geometry/Patch.h:84`, `Geometry/SliceCap.cpp:336` |
| 구조체 예시의 PatchPiece에 originNodes 없음 | 실제로 PatchSkin vector 보관·복사 | 후반 구현 노트에 반영: `Docs/PlaneSlicingDesign.md:149`, `Docs/PlaneSlicingDesign.md:329`, `Geometry/Patch.h:96`, `Geometry/Slice.cpp:374` |
| 초기 PatchCap 예시의 Math::Matrix4와 단일 rimNodes | 실제 XMFLOAT4X4, loopSizes도 보관 | 구현 노트/코드 확장: `Docs/PlaneSlicingDesign.md:145`, `Docs/PlaneSlicingDesign.md:328`, `Geometry/Patch.h:85`, `Geometry/Patch.h:91` |
| UpdatePatchPose가 스키닝과 메시 갱신을 함께 수행 | [미커밋] Controller::Update와 렌더 시 View::Sync로 분리 | 함수명/실행 위치 변경: `Docs/PlaneSlicingDesign.md:344`, `MiniEngine/ModelViewer/ModelViewer.cpp:395`, `Debug/CutDebugView.cpp:133` |
| CutDebugDesign의 “아래 코드는 아직 없다” | [미커밋] Controller/View 분리와 U 와이어 구현 | D-0/D-1 해당 코드 존재: `Docs/CutDebugDesign.md:5`, `Docs/CutDebugDesign.md:152`, `Runtime/CutController.cpp:12`, `Debug/CutDebugView.cpp:153` |
| 분할 표면 Face.cutId 기록 | AddFace가 source를 복사하고 cutId를 다시 지정하지 않음 | D-2 미반영: `Docs/CutDebugDesign.md:68`, `Geometry/Slice.cpp:262` |
| splitByOrigin 옵션과 세 종류 section | BuildRenderMesh는 Surface/Cap 두 pass | D-2 미반영: `Docs/CutDebugDesign.md:69`, `Rendering/RenderBake.cpp:92`, `Rendering/RenderBake.cpp:107` |
| T 편집 mode, Enter RequestCut, Backspace DiscardCut | [미커밋] ToggleSlice와 dirty 자동 절단 | D-3 미반영: `Docs/CutDebugDesign.md:81`, `Docs/CutDebugDesign.md:89`, `Runtime/CutController.h:32`, `Runtime/CutController.cpp:141` |
| PreviewDirty/CutRequested, dry-run preview | [미커밋] SliceDirty 하나로 실제 piece 교체 시도 | D-3 미반영: `Docs/CutDebugDesign.md:105`, `Runtime/CutController.h:94`, `Runtime/CutController.cpp:145` |
| preview 상태색과 BakeLoopDebug | 현재 BakePlaneDebug는 청록 링/흰 화살표; View는 plane/boundary를 그림 | preview 경로 확인 못 함: `Docs/CutDebugDesign.md:123`, `Rendering/RenderBake.cpp:140`, `Debug/CutDebugView.cpp:122` |
| C는 자동 절단 제거, M은 다음 확정에만 반영 | [미커밋] C는 enabled면 dirty, M도 dirty 설정 | D-3 의미 변경 미반영: `Docs/CutDebugDesign.md:131`, `Docs/CutDebugDesign.md:132`, `Runtime/CutController.cpp:30`, `Runtime/CutController.cpp:56` |
| 와이어 팔레트 세 이름: 원본/분할/cap | 현재 bake에서는 section 0 표면·1 cap이므로 cap은 팔레트 index 1 사용 | D-2 이전 조합: `Docs/CutDebugDesign.md:53`, `Rendering/RenderBake.cpp:116`, `Rendering/ProceduralMeshRenderer.cpp:253`, `Rendering/ProceduralMeshPS.hlsl:28` [미커밋] |

### 10.2 버그 의심 목록

- [미커밋][의심] L/Z가 패치에도 적용되는 것으로 기대하면 단독 패치 표시에서 효과가 없다. pieceOptions를 계산하지만 단독 패치에는 기본 DrawOptions를 넘긴다. 기대 동작과의 차이는 실행 확인 못 함. (`Debug/CutDebugView.cpp:140`, `Debug/CutDebugView.cpp:184`, `Docs/CutDebugDesign.md:63`)
- [미커밋][의심] Controller에서 패치 스키닝 뒤 두 번째 piece 스키닝 등이 실패하면 먼저 갱신된 계산 piece와 이전 렌더 위치가 섞여 남을 여지가 있다. 각 SkinPatchPiece는 개별 호출만 원자적으로 기록하고 View는 FramePoseValid=false면 위치 refresh를 건너뛴다. 실제 재현은 확인 못 함. (`Runtime/CutController.cpp:130`, `Runtime/CutController.cpp:154`, `Geometry/Skinning.cpp:110`, `Debug/CutDebugView.cpp:88`)
- [의심] C가 첫 완료된 ModelInstance::Update보다 먼저 처리될 경우, 배열 할당만 된 pose를 읽을 여지가 있다. 생성/대입은 배열을 할당하고 첫 값 작성은 Update에서 하며 입력은 Update 앞이다. 실제 앱에서 이 순서가 발생하는지는 확인 못 함. (`MiniEngine/Model/Model.cpp:142`, `MiniEngine/Model/Model.cpp:208`, `MiniEngine/ModelViewer/ModelViewer.cpp:306`, `MiniEngine/ModelViewer/ModelViewer.cpp:389`)
