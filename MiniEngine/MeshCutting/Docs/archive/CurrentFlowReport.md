상태: 완료 — 현재 코드 흐름 분석 보고서 (기준 커밋 1287cb50fc9bba2d20d48de73b3aec033639ca7e, 2026-10-21). 구현 완료 선언이 아니라 해당 시점 소스의 분석 완료를 뜻한다.

# MeshCutting: 처음부터 읽는 데이터 흐름과 코드 역할 보고서

## 0. 이 보고서의 기준과 핵심 결론

이 문서는 현재 소스를 직접 읽어 작성했다. 예전 대화의 구조나 설계 문서의 예정 사항을 구현된 것으로 취급하지 않는다. 분석 범위는 MeshCutting 전체와 ModelViewer의 연결 부분이다. 이번 작업에서는 C++·HLSL·프로젝트 설정을 수정하지 않았고, 빌드·CPU 테스트·GPU 실행·성능 측정을 수행하지 않았다. 개선점의 성능 효과는 추정이며, 코드로 확인되는 복사·할당·반복 호출과 구분해서 썼다.

현재 프로그램은 **월드 평면 하나로 여러 등록 대상의 본 선분을 검사하고, 교차 선분마다 독립적인 패치를 추출·절단하여 복사본을 표시하는 CPU 절단 데모**다.

- 이전의 joint 집합 → 통합 패치 하나 경로는 현재 컨트롤러의 주 경로가 아니다.
- CutJob은 이제 컨트롤러와 뷰에 실제 연결되어 있다.
- 같은 부모 joint를 가진 형제 선분은 작업은 별도지만, 추출·용접을 다시 하지 않고 기존 형제 패치를 복사해 사용한다.
- 원본 스키닝 표와 원본 출처는 shared_ptr로 공유한다. 기하 점·corner·face 배열까지 모두 공유하는 것은 아니다.
- 원본 → ProceduralMeshData + BonePatchSource → WeldInput → PatchPiece 경로는 아직 유지한다.
- 성공한 작업마다 양수/음수 PatchPiece 한 쌍이 생긴다. 단면 삼각형은 각 조각 안에 포함된다.
- 원본 캐릭터 메시 제거·교체, 뼈 계층/물리 분리, 독립 스키닝, 임의 스태틱 메시 입력 경로는 아직 없다.
- Ctrl+Z는 전체 확정 결과 제거다. 여러 단계의 실행 취소 이력을 복원하는 기능이 아니다.
- Enter는 각 작업의 미절단 patch를 자른다. 직전 positive/negative를 연속으로 재절단하는 UI가 아니다.
- 컨트롤러는 여러 인스턴스와 모든 유효 스킨 팔레트를 다룰 수 있지만, 현재 ModelViewer 연결은 인스턴스 하나를 등록하고 갱신·렌더링한다.

NDC와의 관계는 [NDC25Reference.md](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Docs/NDC25Reference.md)를 참고했다. 이 보고서는 원본 발표를 새로 검증한 문서가 아니다. 현재 구현은 그 문서의 '영향 범위 복사 → 복사본 절단 → 수동 스키닝'에 해당한다. 원본 숨김·물리 관절 분리·분리 후 기여 교체는 이후 단계다.

## 1. 읽기 전에 필요한 용어

| 용어 | 현재 코드에서 뜻하는 것 |
| --- | --- |
| Model | 원본 메시·압축 정점 버퍼·팔레트·스켈레톤 연결을 가진 모델 자산 |
| ModelInstance | 그 모델을 장면에 둔 인스턴스. 현재 자세·변환을 제공 |
| joint | 스키닝 변환 기준점. 정점에 번호와 가중치로 연결 |
| segment / 본 선분 | 부모 joint와 자식 joint를 잇는 선택·표시용 선분 |
| patch | 선택 joint의 영향을 받는 삼각형들을 복사한 부분 메시 |
| CutTarget | 컨트롤러에 등록한 모델 인스턴스 |
| CutJob | 교차 선분 하나에 대응하는 작업 상태 |
| node | 위치를 공유하는 기하 점 |
| corner | node를 참조하고 normal·UV·색을 따로 갖는 렌더 표현 |
| face | corner 번호 세 개를 연결한 삼각형 |
| cap | 절단 구멍을 채우는 단면 삼각형들 |
| topology | 어떤 점과 면이 연결되어 있는지 |
| bake | 계산 자료를 그릴 수 있는 정점·인덱스 배열로 펼치는 작업 |

joint 번호, node 번호, corner 번호, 원본 스킨 정점 번호는 서로 다른 배열의 인덱스다. 우연히 초기 번호가 같더라도 같은 개념으로 취급하면 안 된다.

## 2. 파일 위치 지도

그림의 파일 이름은 아래 실제 파일을 뜻한다. 구조체/클래스 선언·멤버 배치는 헤더에, 일반적인 함수 동작은 CPP에 있다. 그림의 파일 표기는 정의 또는 구현 위치이며, 데이터가 그 파일의 전역 변수라는 뜻은 아니다.

| 구분 | 헤더 / 정의 | 구현 |
| --- | --- | --- |
| 외부 원본 모델 | [Model.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/Model/Model.h) | MiniEngine Model 모듈, 이 보고서에서는 공개 입력 역할만 다룸 |
| 진입점 | — | [ModelViewer.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/ModelViewer/ModelViewer.cpp) |
| 작업 자료 | [CutJob.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Runtime/CutJob.h) | inline 비교·Diff·색 함수는 같은 헤더 |
| 실행 제어 | [CutController.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Runtime/CutController.h) | [CutController.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Runtime/CutController.cpp) |
| 원본 추출 | [BonePatch.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/BonePatch.h) | [BonePatch.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/BonePatch.cpp) |
| 절단용 기하 자료·용접 | [Patch.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/Patch.h) | [Patch.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/Patch.cpp) |
| 스키닝 | [Skinning.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/Skinning.h) | [Skinning.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/Skinning.cpp) |
| 평면·본 선택 | [CutPlane.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/CutPlane.h) | [CutPlane.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/CutPlane.cpp) |
| 평면 기즈모 | CutPlane.h | [CutPlaneGizmo.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/CutPlaneGizmo.cpp) |
| 절단 | [Slice.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/Slice.h) | [Slice.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/Slice.cpp) |
| 루프·캡 | Slice.h | [SliceCap.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/SliceCap.cpp) |
| 공통 타입 | [SliceTypes.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Geometry/SliceTypes.h) | inline 도구도 같은 헤더 |
| 렌더 배열 변환 | [RenderBake.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Rendering/RenderBake.h) | [RenderBake.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Rendering/RenderBake.cpp) |
| CPU 렌더 메시 | [ProceduralMesh.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/ProceduralMesh.h) | [ProceduralMesh.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/ProceduralMesh.cpp) |
| GPU 렌더러 | [ProceduralMeshRenderer.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Rendering/ProceduralMeshRenderer.h) | [ProceduralMeshRenderer.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Rendering/ProceduralMeshRenderer.cpp) |
| 표시·UI | [CutDebugView.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Debug/CutDebugView.h) | [CutDebugView.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Debug/CutDebugView.cpp) |
| 카메라 | [SceneViewCamera.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Debug/SceneViewCamera.h) | [SceneViewCamera.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Debug/SceneViewCamera.cpp) |

## 3. 데이터 흐름 다이어그램

범례:
- 둥근 상자 **[함수]**: 계산·변환하는 동작.
- 사각형 **[구조체 데이터]**: 구조체 인스턴스 또는 배열.
- 사각형 **[클래스 객체]**: 상태와 동작을 가진 인스턴스.
- 실선: 입력·출력 또는 처리 전달.
- 점선: 소유·참조 관계. 그림마다 화살표에 의미를 적었다.
- '정의: X.h'와 '구현: X.cpp'를 구분한다.

### 3.1 원본에서 교차 작업까지

~~~mermaid
flowchart TD
    M["[클래스 객체] ModelInstance / Model<br/>정의: Model.h"]
    REG("[멤버 함수] CutController::RegisterInstance<br/>구현: CutController.cpp")
    T["[구조체 데이터] CutTarget<br/>대상 ID·팔레트 목록·현재 뼈대<br/>정의: CutJob.h"]
    POSE("[함수] ComputePaletteSkeleton<br/>구현: BonePatch.cpp")
    SP["[구조체 데이터] SkeletonPose<br/>현재 joint 위치·부모 목록<br/>정의: CutPlane.h"]
    PL["[클래스 객체] CutController.m_Plane: CutPlane<br/>정의: CutController.h / CutPlane.h"]
    PICK("[멤버 함수] CutPlane::PickSegments<br/>구현: CutPlane.cpp")
    HIT["[구조체 데이터] SegmentHit 목록<br/>부모·자식·교차점<br/>정의: CutPlane.h"]
    KEYS("[함수] MakeSegmentKey / DiffJobs<br/>정의·구현: CutJob.h")
    JOB["[구조체 데이터] CutJob 목록<br/>target·palette·parent·child별 작업<br/>정의: CutJob.h"]
    M --> REG --> T
    T --> POSE --> SP --> PICK
    PL --> PICK
    PICK --> HIT --> KEYS --> JOB
~~~

RegisterInstance는 등록 시 실행하고, ComputePaletteSkeleton과 선분 검사는 편집 프레임에 실행한다. 실제 호출 연결은 CutController::UpdateTargets → UpdateCandidates → ApplyDiff다. CutJoints로 부모 집합을 합치는 경로는 이 컨트롤러에서 쓰지 않는다.

작업 키의 의미:
- target: 인스턴스 등록 ID.
- kind: 현재는 SkeletalSegment 경로.
- a: 팔레트 번호.
- b: 부모 joint.
- c: 자식 joint.

같은 Model을 공유하는 캐릭터도 target ID가 다르면 서로 다른 작업이다.

### 3.2 새 작업 하나의 추출과 용접

~~~mermaid
flowchart TD
    M["[클래스 객체] 원본 Model + ModelInstance<br/>정의: Model.h"]
    J["[구조체 데이터] CutJob<br/>key.a 팔레트·key.b 추출할 부모 joint<br/>정의: CutJob.h"]
    E("[함수] ExtractBonePatch<br/>구현: BonePatch.cpp")
    R["[구조체 데이터] ProceduralMeshData<br/>현재 위치·normal·UV·색·인덱스<br/>정의: ProceduralMesh.h"]
    S["[구조체 데이터] BonePatchSource<br/>바인드 정보·joint·가중치·출처<br/>정의: BonePatch.h"]
    W("[함수] MakeWeldInput<br/>구현: BonePatch.cpp")
    WI["[구조체 데이터] WeldInput<br/>현재 위치 + 바인드·렌더·스키닝 정보<br/>정의: Patch.h"]
    B("[함수] BuildPatchPiece<br/>구현: Patch.cpp")
    P["[구조체 데이터] job.patch: PatchPiece<br/>nodes·corners·faces·boundary<br/>정의: Patch.h / CutJob.h"]
    SK["[클래스 객체] Skinning<br/>용접된 원본 SkinVertex 배열<br/>정의: Skinning.h"]
    M --> E
    J --> E
    E --> R
    E --> S
    R --> W
    S --> W
    W --> WI --> B --> P
    B --> SK
    P -.->|"skin 멤버로 공유 참조"| SK
    J -.->|"해당 job.source.bone이 공유 보관"| S
~~~

실제 호출자는 CutController::ExtractJob이다. 현재 이 함수의 patch: ProceduralMeshData와 weldInput은 지역 변수라 용접이 끝나면 사라진다. 이전 컨트롤러처럼 추출 결과를 별도 ProceduralMesh 객체에 장기 보관하지 않는다. BonePatchSource는 shared_ptr로 작업에 남는다.

형제 작업 재사용:
- 같은 target·kind·palette·parent인 후보 작업을 찾는다.
- 있으면 ExtractBonePatch/MakeWeldInput/BuildPatchPiece를 다시 실행하지 않는다.
- job.patch = sibling.patch로 기하 배열을 복사한다.
- patch.skin과 source.bone은 공유한다.
- preview, confirmed 결과와 오류는 작업별로 관리한다.

### 3.3 CPU 스키닝에서 절단 결과까지

~~~mermaid
flowchart TD
    SRC["[구조체 데이터] job.source.bone<br/>메시 슬롯·팔레트<br/>정의: BonePatch.h"]
    INS["[클래스 객체] 현재 ModelInstance<br/>정의: Model.h"]
    BM("[함수] BuildSkinMatrices<br/>구현: BonePatch.cpp")
    MAT["[데이터 배열] job.skinMatrices<br/>메시 슬롯별 joint 행렬<br/>멤버 정의: CutJob.h"]
    P["[구조체 데이터] job.patch: PatchPiece<br/>정의: Patch.h"]
    SK["[클래스 객체] patch.skin: Skinning<br/>정의: Skinning.h"]
    APPLY("[멤버 함수] Skinning::Apply<br/>구현: Skinning.cpp")
    PL["[구조체 데이터] SlicePlane<br/>CutPlane::ToSlicePlane으로 생성<br/>정의: Patch.h"]
    SL("[함수] SlicePatch<br/>구현: Slice.cpp")
    LOOP("[함수] ExtractCutLoops / BuildCap<br/>구현: SliceCap.cpp")
    RES["[구조체 데이터] SliceResult<br/>positive·negative·loops·status<br/>정의: Slice.h"]
    CHECK("[멤버 함수] CutPlane::ContainsLoops<br/>구현: CutPlane.cpp")
    JOB["[구조체 데이터] job.preview 또는 job.positive / negative<br/>저장 위치: CutJob.h"]
    SRC --> BM
    INS --> BM
    BM --> MAT --> APPLY
    P --> APPLY
    SK --> APPLY
    APPLY -->|"patch.nodes 위치 갱신"| SL
    PL --> SL
    SL --> LOOP
    LOOP --> RES
    RES --> CHECK --> JOB
~~~

SlicePatch 내부에서 루프·캡 함수를 부르는 것이며, 외부에서 SlicePatch 이후 BuildCap을 한 번 더 부르는 흐름은 아니다. ContainsLoops는 성공한 결과의 루프가 유한 사각형 안에 들어오는지 확인한다.

RunPreview는 결과를 job.preview에 저장한다. ConfirmAll은 작업마다 별도 SliceWithPlane을 실행하고 성공한 작업만 job.positive/negative를 교체한다. 미리보기 결과를 그대로 확정 결과로 넘기는 캐시는 현재 없다.

### 3.4 조각에서 GPU까지

~~~mermaid
flowchart TD
    P["[구조체 데이터] 작업별 positive / negative: PatchPiece<br/>정의: Patch.h / CutJob.h"]
    B("[함수] BuildRenderMesh<br/>구현: RenderBake.cpp")
    D["[구조체 데이터] ProceduralMeshData<br/>정의: ProceduralMesh.h"]
    SET("[멤버 함수] ProceduralMesh::SetMesh<br/>구현: ProceduralMesh.cpp")
    PM["[클래스 객체] JobMeshes.positive / negative<br/>타입: ProceduralMesh<br/>소유: CutDebugView.h"]
    DRAW("[멤버 함수] ProceduralMeshRenderer::Draw<br/>구현: ProceduralMeshRenderer.cpp")
    GPU["[GPU 리소스] 정점·인덱스 버퍼<br/>관리: ProceduralMeshRenderer"]
    SH["[GPU 함수] VS main / PS main<br/>ProceduralMeshVS.hlsl / ProceduralMeshPS.hlsl"]
    P --> B --> D --> SET --> PM --> DRAW --> GPU --> SH
~~~

BuildRenderMesh와 SetMesh를 호출하는 곳은 CutDebugView::Sync다. Renderer::Draw를 호출하는 곳은 CutDebugView::Draw다. 작업마다 표시 메시를 std::map<CutJobKey, JobMeshes>에 보관한다.

후속 프레임에는 topology를 다시 만들지 않고 UpdateRenderPositions → ProceduralMesh::UpdateVertices → 변경된 정점 버퍼 업로드를 실행한다. normal/UV/color는 유지한다.

### 3.5 소유와 공유: 무엇이 복사되는가

~~~mermaid
flowchart TD
    CC["[클래스 객체] CutController<br/>CutController.h"] -->|"소유"| PL["[클래스 객체] CutPlane 하나<br/>CutPlane.h"]
    CC -->|"소유"| TS["[구조체 데이터] CutTarget 목록<br/>CutJob.h"]
    CC -->|"소유"| JS["[구조체 데이터] CutJob map<br/>CutJob.h"]
    JS -->|"작업별 소유"| P["[구조체 데이터] patch / preview / positive / negative<br/>각 PatchPiece의 기하 배열은 별도"]
    P -.->|"shared_ptr const"| S["[클래스 객체] 공유 Skinning<br/>Skinning.h"]
    JS -.->|"source.bone으로 공유"| BS["[구조체 데이터] BonePatchSource<br/>BonePatch.h"]
    V["[클래스 객체] CutDebugView<br/>CutDebugView.h"] -->|"소유"| JM["[구조체 데이터] JobMeshes map<br/>표시용 ProceduralMesh들"]
    V -.->|"읽기만 함"| JS
    R["[클래스 객체] ProceduralMeshRenderer<br/>ProceduralMeshRenderer.h"] -->|"소유·풀 관리"| G["[GPU 리소스] 버퍼"]
    R -.->|"주소를 key로 등록"| JM
~~~

절단 양쪽 조각은 node 객체를 메모리로 공유하지 않는다. 각자 별도 node 배열에 같은 교차 위치와 같은 origin을 가지며, 공유 Skinning으로 매 프레임 일치하는 위치를 재구성한다. 같은 조각 안의 표면/단면 corner들은 실제로 같은 rim node 번호를 참조한다.

## 4. 한 프레임과 한 번의 Enter를 따라가기

1. ModelViewer::Startup이 로드한 인스턴스를 RegisterInstance로 등록한다.
2. 사용자가 T를 누르면 ToggleEditPlane이 편집을 켠다. 최초에는 첫 대상 bounds 중심에 수평 평면을 놓는다.
3. ModelViewer::Update가 카메라와 기즈모 입력을 처리한다.
4. ModelInstance::Update가 현재 프레임의 모델 자세를 계산한다.
5. CutController::Update를 한 번 호출한다.
6. UpdateTargets가 대상 모델 교체를 확인하고, 편집 중 각 팔레트의 SkeletonPose를 읽는다.
7. UpdateCandidates가 모든 대상·팔레트에서 교차 선분을 모으고 DiffJobs로 새로 생긴/유지된/사라진 작업을 구분한다.
8. ApplyDiff가 새 작업을 만들고 ExtractJob을 호출한다. 유지된 작업은 hit만 갱신한다.
9. SkinJobs가 현재 행렬을 준비하고 후보 patch와 확정 조각의 현재 위치를 계산한다.
10. Enter 요청이 있으면 ConfirmAll, 아니면 dirty한 유효 후보마다 RunPreview를 실행한다.
11. 렌더 단계에서 CutDebugView::Sync가 결과 세대 번호를 비교한다. 새 결과는 전체 bake, 평소에는 위치 갱신이다.
12. Draw가 대상 옆에 조각을 배치하고, 원본 위치에는 평면·본 선분·미리보기·기즈모를 그린다.
13. ModelViewer가 명령을 제출한 fence를 ProceduralMeshRenderer::NotifySubmitted에 알려 준다.
14. UI는 작업별 대상·팔레트·부모/자식·삼각형 수·실패 이유·시간을 표시한다.

### 작업 수명

| 사건 | 현재 동작 |
| --- | --- |
| 새 교차 선분 | 새 작업 또는 비후보 작업을 후보로 복귀시키고 추출 |
| 교차가 계속 유지 | 재추출하지 않고 현재 hit와 자세 갱신 |
| 후보에서 사라짐 | patch·preview를 해제. 확정 결과가 없으면 작업 삭제 |
| 편집 모드 종료 | 후보를 모두 잃은 것으로 처리. 확정 결과는 계속 스키닝·표시 |
| Enter 일부 실패 | 성공한 작업만 교체. 실패 작업은 이전 확정 결과 유지 |
| Ctrl+Z | 모든 확정 결과 제거. 후보는 preview 재계산, 비후보 작업 삭제 |
| 인스턴스 해제 | 그 대상 작업 전부 제거 |
| 등록 인스턴스의 Model 포인터 변경 | 기존 작업 제거 후 팔레트 목록 다시 읽기 |

### 결과가 몇 개인가

후보 작업 N개가 모두 성공하면 확정 PatchPiece는 최대 2N개다. 이 밖에 미리보기 결과에도 임시 조각 쌍이 들어갈 수 있다. 여러 덩어리가 한쪽에 있어도 그쪽 PatchPiece 하나에 함께 저장하며, 연결 성분별 독립 물체로 분리하지 않는다. 형제 작업이 같은 패치를 쓰면 결과가 겹칠 수 있다.

## 5. 핵심 자료형을 예제로 이해하기

렌더 정점 여섯 개가 두 삼각형 ABC와 ACD를 표현한다고 하자. A와 C는 UV 경계 때문에 두 번 저장되어 있고, 같은 위치의 복제 정점은 스키닝도 같다고 가정한다.

| 입력 정점 | 위치 | 용접 후 node |
| --- | --- | --- |
| 0 | A | 0 |
| 1 | B | 1 |
| 2 | C | 2 |
| 3 | A | 0 |
| 4 | C | 2 |
| 5 | D | 3 |

결과는 node 4개, corner 6개, face 2개, 원본 SkinVertex 4개다. face는 corner 번호를, corner는 node 번호를 참조한다. 위치는 공유하지만 UV/normal은 보존한다.

### PatchPiece 관련 구조체

| 형식 | 보관 내용 | 만든 이유 |
| --- | --- | --- |
| SkinVertex | meshSlot, bindPosition, jointCount, joints, weights | 원본 점의 현재 위치를 재계산 |
| Skinning | SkinVertex 배열, Apply 등 계산 | 원본 표 공유와 스키닝 책임 묶기 |
| PatchOrigin | skinVertex 최대 3개와 bary | 생성 점을 원본 점들의 조합으로 표현 |
| PatchNode | kind, origin, cutId, position | 절단에서 위치·연결의 기준 |
| PatchCorner | node, normal, uv, color | UV seam·hard normal 보존 |
| PatchFace | corner[3], kind, sourceTriangle, cutId | 삼각형과 생성·분할 이력 |
| PatchCap | cutId, planePoint, planeNormal, rimNodes, loopSizes | 단면 기록. 실제 단면 삼각형은 faces |
| PatchPiece | skin, nodes, corners, faces, boundary, caps, skinStateId | 절단 계산용 조각 전체 |
| WeldInputVertex / WeldInput | 원본과 현재 위치, 렌더·스키닝 정보, indices | Model에 의존하지 않는 용접 입력 |
| WeldStats | corner/node 수, skin 분리 위치, 각 경계 수 등 | 용접 결과 진단 |

예를 들어 새 교차점 P가 A-B 사이 25%라면 origin에 A 0.75, B 0.25를 기록한다. 기존 A는 A 1.0만 기록한다.
- joint weights: bindPosition → 원본 점의 현재 위치.
- origin bary: 원본 점들의 현재 위치 → P의 현재 위치.
- 번호는 Skinning::GetVertices()를 참조한다.
- 현재 cap은 별도 Cap-kind 기하 점을 추가하지 않고 Surface rim node로 삼각형을 만든다. PatchFaceKind::Cap과 PatchNodeKind::Cap은 같은 판정이 아니다.

### 간선 종류

| 종류 | 뜻 |
| --- | --- |
| Interior | 일반 내부 간선. 보통 boundary map에 기록 없음 |
| Replacement | 추출한 부분 패치의 기존 열린 경계 |
| UnweldedSeam | 같은 위치를 덮는 경계지만 용접되지 않음 |
| Cut | 절단으로 새로 생긴 열린 경계 |
| CapSeam | 단면과 표면이 연결된 내부 접합선. boundary map에 있지만 열린 경계는 아님 |

용접은 거리 epsilon 방식이 아니다. meshSlot·바인드 위치 비트·정리된 joint/weight가 일치할 때만 합친다. -0과 +0은 통일한다. UV/normal이 달라도 node는 합치고 corner를 유지한다. 스키닝이 다르면 나중에 움직임이 다르므로 node를 나눈다.

## 6. 각 파일·클래스·함수의 역할

### 6.1 ModelViewer.cpp — 장면과 모듈을 연결하는 입구

큰 역할: 입력, 원본 애니메이션, 절단 모듈, 기존 렌더링의 실행 순서를 연결한다.

| 함수 | 역할 |
| --- | --- |
| Startup | 모델 로드·애니메이션 초기화, 지원 경로에서 인스턴스 등록, 카메라 준비 |
| Cleanup | 인스턴스 해제 → 뷰의 GPU 등록 해제 → 렌더러 종료 → 모델 해제 |
| Update | T/V/Enter/Ctrl+Z/WERQ/Space/F 입력, 카메라·기즈모, 원본 자세 갱신 후 CutController::Update |
| RenderScene | 원본 모델 및 프로시저럴 메시 렌더링, 제출 fence 통지 |
| RenderProceduralMeshes | CutDebugView::Draw 호출, 필요한 선형 깊이 갱신 |
| SetWeightView | 팔레트별 joint→작업 색 표를 기존 모델 렌더러에 전달 |
| RenderUI | 대상/팔레트/joint 번호를 이름으로 바꾸는 callback과 함께 DrawUI 호출 |
| BoneName 등 이름 보조 | joint 표시용 이름 제공 |

컨트롤러는 여러 대상 지원이지만, 이 데모의 Startup/Update/RenderScene은 m_ModelInst 하나를 연결한다. 다중 인스턴스 데모를 만들려면 호스트의 로드·갱신·원본 렌더·UI도 확장해야 한다.

### 6.2 Runtime/CutJob.h — 작업 자료와 식별 규칙

큰 역할: 작업 하나가 무엇인지 정의한다. 절단 계산은 수행하지 않는다.

| 자료형 / 함수 | 역할 |
| --- | --- |
| CutTargetId | 등록된 인스턴스 ID |
| CutJobKind | SkeletalSegment / StaticMesh 구분. StaticMesh는 예약 구조 |
| CutJobKey, operator<, operator== | target/kind/a/b/c로 작업 비교, map key |
| MakeSegmentKey | 대상·팔레트·부모·자식을 key로 |
| MakeStaticMeshKey | 대상·메시 번호를 key로. 생성 경로는 현재 미연결 |
| CutJobSource | 공유 BonePatchSource, 추출 joint와 종류 |
| CutTarget | 비소유 instance/model 포인터, 이름, 팔레트, skeletons, 오류, 반지름 |
| CutJob | 후보 patch, 원본 출처, 현재/직전 행렬, preview, 확정 조각, 세대·오류·시간 |
| CutJobDiff | added / lost / kept |
| DiffJobs | 이번 key 목록과 이전 후보를 비교. 입력 중복 key는 첫 항목만 처리 |
| GetNoJobColor / GetJobColor | 비선택 회색 / 작업별 8색 순환 |

CutJob은 데이터 모음이다. 추출·스키닝·절단 실행은 CutController의 멤버 함수다. candidate와 haveConfirmed는 독립 상태라, 후보이면서 확정 결과도 가질 수 있다.

### 6.3 Runtime/CutController — 평면과 여러 작업의 실행 제어

큰 역할: 원본 접근 함수와 순수 기하 함수들을 작업별로 조율한다. GPU 메시를 소유하지 않는다.

| 함수 | 내부 역할 |
| --- | --- |
| RegisterInstance | ID 발급, 비소유 포인터 저장, 팔레트 읽기 |
| UnregisterInstance | 해당 작업 제거 후 대상 제거 |
| ReloadPalettes | joint가 있는 팔레트 전부 수집, skeleton 배열·반지름 준비 |
| RemoveJobsOf | 대상 ID의 모든 작업 삭제 |
| FindTarget / FindSkeleton | 대상 또는 작업의 팔레트 자세 조회 |
| GetFrameStats | 작업 수·후보 수·확정 수·추출 횟수·각 시간 합계 |
| GetJointColors | 후보 부모 joint에 작업 색 배정. 형제는 생성 순서가 빠른 작업의 색 |
| ToggleEditPlane | 편집 전환, 최초 평면 배치, preview dirty |
| UpdateGizmo | 평면 조작 후 모든 작업 preview dirty |
| SetGizmoTool / CancelGizmo | 평면의 도구·조작 상태에 전달 |
| RequestCut | 편집·평면·후보 patch 조건을 만족하면 다음 Update의 요청 저장 |
| DiscardCut | 전체 확정 결과 제거, 비후보 작업 삭제, 후보 preview dirty |
| Update | UpdateTargets → 후보 diff → SkinJobs → ConfirmAll 또는 dirty preview |
| UpdateTargets | 모델 교체 감지·작업 제거, 현재 SkeletonPose 갱신 |
| UpdateCandidates | 대상·팔레트별 PickSegments, MakeSegmentKey, DiffJobs |
| ApplyDiff | added는 생성/복귀·추출, kept는 hit 갱신, lost는 후보 제거 |
| ExtractJob | 형제 입력 재사용 또는 부모 joint 단일 추출 → WeldInput → PatchPiece |
| DropCandidate | patch·preview 해제. 확정 결과와 출처는 유지 |
| SkinJobs | 현재 행렬 생성 → 후보/확정 조각 Apply → 행렬 변화로 preview dirty |
| SliceWithPlane | SlicePatch(Create cap) → 유한 사각형 범위 확인 |
| RunPreview | 미절단 patch 시험 절단, 결과·상태·세대 보관 |
| ConfirmAll | 후보별 독립 절단, 성공 결과만 교체, 표면 면적 차이 진단 |
| PlacePlane | 모델 중심, normal +Y, 반 크기 반지름/2, 도구 선택 유지 |
| Smooth / ElapsedMs | 시간 지수 평균 / 경과 ms |
| SameMatrices | 슬롯·joint 행렬 배열의 크기와 바이트 비교 |
| 내부 ClearPreview | preview 결과·존재·상태·세대 초기화 |
| 나머지 Get 함수 | 상태 읽기 전용 제공 |

등록한 인스턴스는 UnregisterInstance 전까지 살아 있어야 한다. raw pointer는 수명을 연장하지 않는다. m_Generation은 전 작업의 preview/confirmed 세대를 유일하게 발급해 같은 key가 재생성되어도 뷰가 변경을 감지하게 한다.

### 6.4 BonePatch — 원본 모델 해독과 패치 추출

큰 역할: Model의 packed CPU 정점과 현재 자세를 읽어, 필요한 삼각형의 현재 외형과 원본 출처를 생성한다. 실제 면 분할은 하지 않는다.

| 공개 함수 | 역할 |
| --- | --- |
| ExtractBonePatch 단일 joint | 하나짜리 집합으로 집합 버전 호출. 현재 ExtractJob이 사용하는 입구 |
| ExtractBonePatch joint 집합 | 입력 검사, 메시/draw/삼각형 순회, 영향 판정, 정점·출처 복사, 성공 시 결과 교체 |
| MakeSlotMatrices | meshWorld와 joint.posXform을 결합해 슬롯 행렬 생성 |
| BuildSkinMatrices | source.meshes를 따라 현재 슬롯별 행렬 생성 |
| ComputePaletteSkeleton | 팔레트의 현재 joint 위치와 가까운 상위 joint 연결 생성 |
| ComputePoseJointWorld | 패치 기준 joint 자세 행렬 생성. 현재 주 실행 경로에서 호출 없음 |
| MakeWeldInput | 현재 렌더 속성과 원본 바인드·스키닝 정보를 묶고 인덱스 복사 |

| 출처 자료형 | 내용 |
| --- | --- |
| BonePatchMesh | 모델 전체 meshIndex, worldMatrixIndex, startJoint |
| BonePatchVertex | 패치 내부 meshSlot, sourceVertex, 바인드 position/normal, joints/weights |
| BonePatchTriangle | meshSlot, drawIndex, primitive |
| BonePatchSource | 팔레트·선택 집합·기준값·메시/정점/삼각형 출처·추출 자세·mixedOutsideTriangles |

meshIndex는 모델 전체 번호, meshSlot은 추출 출처 목록 안의 번호다. source.vertices[i]와 렌더 vertices[i]는 같은 정점의 서로 다른 정보를 담는다.

추출 기준: 정점 세 개 중 하나라도 선택 joint 영향이 minWeight를 초과하면 삼각형 전체를 가져온다. 자손만의 영향으로 선택하지는 않는다. subtree 표는 패치 밖 혼합 영향 진단에도 쓰인다. mixedOutsideTriangles는 자동 실패 조건이 아니라 경고용 집계다.

| 내부 자료형·함수 | 이유 |
| --- | --- |
| VertexLayout / ComputeLayout | tangent·UV 플래그에 따른 바이트 offset/stride |
| DecodedVertex / Decode | packed normal, half UV, 16bit joint/weight를 복원·weight 정규화 |
| SourceMeshView | 검증된 vb/ib, 개수, layout, 인덱스 형식, joint 수 |
| TriangleClass | selected와 subtree 안팎 영향 |
| PoseSnapshot | 첫 기여 메시의 world 기준 |
| MeshCopyState 생성자 | worldIT, 슬롯 행렬, 원본→패치 remap 준비 |
| ValidateExtractInput | 모델·인스턴스·팔레트·집합·CPU/pose 데이터 검사 |
| MarkJointSubtree | 선택 joint 자신과 자손 표시 |
| PrepareSourceMesh | 버퍼 범위·stride·행렬·joint·인덱스 형식 검사 |
| ReadTriangle | startIndex와 baseVertex를 적용해 원본 정점 번호 세 개 |
| ClassifyTriangle | 삼각형 정점 스키닝 읽기, 선택·혼합 진단 |
| CapturePoseSnapshot | 기준 자세 행렬 보관, 다른 world 섞임 거부 |
| CopyVertex | remap으로 중복 방지, 현재 위치/normal 계산·영향 색, 원본 바인드 정보 저장 |
| Fail | 오류 문자열과 false |

현재 같은 팔레트를 쓰는 메시들의 world 변환이 다르면 skeleton 계산이 거부할 수 있다. '여러 모델 지원'과 '아무 팔레트 공간 조합도 지원'은 다르다.

### 6.5 Geometry/Patch — 절단 자료형과 용접·검증

큰 역할: 렌더 정점 목록에서 기하 점 공유 관계를 복원하고 절단 가능한 입력을 만든다.

| 함수·내부 형식 | 역할 |
| --- | --- |
| BuildPatchPiece | 입력검사 → canonical skin → node 병합 → Skinning 공유 객체 → corner/face → 경계 → 최종검사 |
| CanonicalSkin | 양수 weight만, 같은 joint 합산, joint 오름차순. 별도 재정규화는 아님 |
| PositionKey | meshSlot + 바인드 위치 비트 |
| WeldKey | PositionKey + 정리된 joint/weight 비트 |
| operator== / Hash / Mix | 정확한 동일성 비교·검색 |
| Bits | -0/+0 통일 후 비트 표현 |
| MakeEdgeKey / EdgeKeyLow / EdgeKeyHigh | 양 끝 node를 작은/큰 순서로 uint64 포장·복원 |
| CollectEdgeUsage | face→corner→node 경로로 count/forward/backward 집계 |
| ValidateOrigin | 원본 번호 범위·정렬·빈 슬롯·bary 범위/합 검사 |
| ValidatePatchPiece | 위치·원본 참조·corner/face 범위, 중복 node·작은 면적·간선 방향/면 수·경계 태그 |
| ValidatePlane | finite 값과 충분한 normal 길이 |
| MakePlaneBasis | 정규화 normal과 안정적인 U,V 직교 축 |
| ComputePieceRadius | node bounds 중심 기준 감싸는 구 반지름 |
| FaceArea / TotalArea | 삼각형 면적 / 특정 kind 합 |
| SliceStatusName / EdgeKindName | 오류·UI용 이름 |
| PatchPiece::Empty / Clear | face가 비었는지 / 전체 초기화 |

epsilon=반지름×epsilonScale이며 면적 임계값은 epsilon²다. 용접의 위치 일치 기준과 별개다. BuildPatchPiece 실패 시 out은 빈 조각이며 stats는 실패 시점까지 채워질 수 있다.

### 6.6 Geometry/Skinning — 공유 원본 스킨 표와 CPU 자세 갱신

큰 역할: 원본 표는 공유하고 현재 node 위치는 조각마다 계산한다.

| 함수 | 역할 |
| --- | --- |
| 생성자 | SkinVertex 배열 이동받아 소유 |
| GetVertices | 원본 표 읽기 |
| Apply | 필요한 원본 표시 → 현재 원본 위치 계산 → origin bary로 node 위치 계산 → 성공 시 한꺼번에 반영 |
| SkinPoint | bindPosition에 joint 행렬 적용 후 weights로 합 |
| Influence | 정점의 특정 joint 영향 합 |
| SetInfluence | 집합의 영향 합. 표시 등에 사용 |
| InJointSet | 집합 중 한 joint의 Influence가 minWeight 초과인지 |
| IsValidJointSet | 비어 있지 않고 정렬·중복 없음·범위 내인지 |
| 내부 Fail | 이유와 false |

Apply의 needed/blend/positions는 각각 사용 원본 표시/원본 현재 위치/출력 임시 위치다. 실패 시 조각을 부분 갱신하지 않는다. skinStateId!=0은 독립 스키닝 미지원으로 거부한다. normal은 갱신하지 않는다. Cap-kind node는 건너뛰지만 현재 cap은 Surface rim node를 공유하므로 함께 움직인다. 변형 후 단면의 완전한 평면성은 보장하지 않는다.

### 6.7 Geometry/CutPlane — 월드 절단 도구와 본 선택

큰 역할: 컨트롤러가 소유한 사각형 평면의 모양·조작·교차 계산을 제공한다. CutPlane가 모든 작업을 관리하는 것은 아니다.

| 형식 | 역할 |
| --- | --- |
| CutPlaneShape | center, quaternion rotation, halfExtents |
| CutPlane::Frame | 회전을 풀어낸 center,U,V,normal,halfExtents |
| SkeletonPose | jointPositions와 팔레트 내부 parents |
| SegmentHit | parentJoint,childJoint,t,point |
| SlicePlane | 중심과 normal만 가진 수학 평면. Patch.h에 정의 |

| 함수 | 역할 |
| --- | --- |
| 생성자 / GetCenter·GetRotation·GetHalfExtents | 모양 초기화·조회 |
| MakeInfinite | point/normal에 맞춘 매우 큰 유한 사각형. 실제 무한 크기는 아님 |
| MakeHorizontal | +Y normal 초기 평면과 모델 비례 크기 제한 |
| GetFrame | 정규 quaternion에서 U,V,normal 추출 |
| ToSlicePlane | center와 normal만 전달 |
| Contains / ContainsLoops | U,V 투영의 사각형 포함 여부 / 모든 루프 점 검사 |
| SetExtentLimits / ClampExtents | 제한 설정 / 실제 범위 적용 |
| Translate / RotateAroundLocalAxis | world 이동 / 로컬 U,V,N 회전 |
| ScaleUniform / ScaleU | 비율 유지 크기 변경 / U만 변경 |
| PickSegments | 부모·자식 signed distance 부호가 다르면 교차점, 사각형 내부 hit |
| CutJoints | hit의 부모 집합 생성. 현재 컨트롤러 경로에서는 미사용, 테스트는 사용 |
| NormalizedRotation / FrameContains | 안전한 회전 정리 / 투영 포함 검사 |

0 거리는 양수 쪽으로 취급한다. 평면이 joint를 정확히 지나갈 때 사용하는 판정 규칙이지, 어떤 분기 구조에서도 반드시 hit 하나만 나온다는 보장은 아니다.

절단은 무한 평면으로 먼저 수행하고, 결과 루프가 사각형 안인지 검사한다. 사각형 테두리를 따라 부분적으로 도려내는 유한 칼날 알고리즘은 아니다.

### 6.8 Geometry/CutPlaneGizmo.cpp — CutPlane 멤버 구현의 나머지

큰 역할: 핸들 도형 생성, 화면 피킹, 드래그 계산이다. 별도 PlaneGizmo 클래스는 없다.

| 자료형 | 역할 |
| --- | --- |
| GizmoTool / GizmoHandle | 도구 종류 / 개별 축·링·크기 핸들 |
| GizmoView / GizmoRay | 카메라·화면·역행렬 / 월드 광선 |
| GizmoPointer / GizmoInput | 좌표·광선 / 버튼·커서·blocked |
| GizmoPrimitive / GizmoShape | handle,형태,points,color |
| GizmoDrag | 시작 모양·축·교차점·기준 좌표·누적 각도 |
| ScreenPoint / PickHit | 투영 결과 / 픽셀 거리와 카메라 거리 |

| 함수 | 역할 |
| --- | --- |
| GizmoView::Make | 역행렬·최소 화면 크기 준비 |
| MakeRay | 픽셀을 역투영해 near→중간 depth 광선 생성 |
| ProjectToScreen | 월드 점을 픽셀로, 카메라 뒤 거부 |
| GetGizmoUnit | 화면에서 약 100픽셀로 보일 world 길이 |
| BuildGizmoPrimitives | 이동축/사각형, 회전 링, 크기 점 |
| PickFromPrimitives / PickHandle | 거리 임계값 안에서 카메라 가까운 핸들 우선 |
| ClosestParamOnAxis / RayPlaneHit | 광선과 축 최근접 좌표 / 광선-평면 앞쪽 교차 |
| BeginDrag | 시작 기준 저장 |
| UpdateDrag | 시작 기준에서 현재 모양 계산. 이동 world XYZ, 회전 local U/V/N |
| SetGizmoTool / CancelGizmo | 도구 변경 시 조작 해제 / hover·drag 초기화 |
| UpdateGizmo | hover·press·drag·release 상태 진행 |
| BakeGizmo | 같은 primitive를 튜브로 변환. hover/active 강조 |
| DistanceToSegment2D / Project / PrimitiveHit / DistanceFromEye | 화면상 피킹 거리 |
| Offset / MakeSegment / MakeQuad / MakeRing / MakePoint | 핸들 형상 |
| Perpendiculars | 화살촉 보조 축 |
| SignedAngle / WrapAngle | 부호 각도와 한 바퀴 넘어가는 연속 회전 |

기즈모의 실제 그리기는 뷰가 담당한다. 이 파일은 RenderBake의 튜브 생성 함수를 사용하지만 D3D12 장치는 사용하지 않는다. blocked는 주로 새 hover/drag 시작을 막는 조건이며, 진행 중 drag는 별도 분기로 처리한다.

### 6.9 Geometry/Slice — 삼각형을 실제로 나누기

큰 역할: 현재 위치의 PatchPiece와 수학 평면을 받아 양쪽 PatchPiece를 만든다.

| 형식·함수 | 역할 |
| --- | --- |
| CapMode | None / Create. None이어도 루프 검사 수행 |
| SliceOptions | cap,epsilonScale,capUvScale |
| SliceResult | status,positive,negative,loops,loopDepth,loopArea,detail |
| SliceLoop | nodes,signedArea,depth,parent |
| 내부 CutNodeInfo | 교차점의 원래 간선 low/high와 t |
| 내부 SideBuild | 한쪽 corner/face 임시 배열과 재사용 map |
| 내부 Slicer | 절단 한 번의 중간 상태 |
| SlicePatch | 출력 초기화 → Run → 실패 시 status/detail 외 제거 |
| Slicer 생성자 / Run | 입력 보관 / 검증·분류·분할·조각 구성·루프·캡 전체 순서 |
| ClassifyNodes | dot(p-planePoint,unitNormal)를 epsilon과 비교 |
| ProcessFace | 전부 한쪽이면 복사, 교차하면 분할, coplanar·cap 관통 거부 |
| CutNodeOf | 기하 간선별 교차 node 캐시, 위치·origin 생성 |
| WholeCorner | 기존 corner를 한쪽 결과로 복사·재사용 |
| CutCorner | 렌더 간선별 UV·색·normal 보간. node는 공유 기준 |
| AddFace | 삼각형 추가·cutId와 기존 cap 분포 기록 |
| BuildPiece | 사용 node 압축·번호 재지정, skin 공유, 경계·cap 기록 재구성 |
| InheritedKind / IsCutNode | 기존 경계 계승 / 신규 node 판정 |
| MergeOrigin | 두 끝점 원본 번호·bary 결합·정렬. 3개 초과 시 거부 |
| Normalize3 / Fail | normal 안전 정리 / 상태·원인 |

교차점은 P=A+t(B-A), t=dA/(dA-dB)다. 한 점 대 두 점이면 한쪽 삼각형 하나와 다른 쪽 사각형을 나눈 삼각형 두 개가 된다. 원래 winding을 유지한다.

공유 기하 간선의 교차 node는 한 번 생성한다. UV seam처럼 렌더 속성이 다르면 CutCorner는 별개일 수 있다. 결과 양쪽은 별도 배열을 가진다. 작은 출력 sliver를 임의로 버리지 않고 실패로 반환한다. 기존 cap을 가로지르는 재절단, 통째 coplanar face 등은 현재 제한에 걸린다. 두 껍질이 원래 양쪽에 떨어져 있으면 루프 없이 Ok가 가능하다.

### 6.10 Geometry/SliceCap — 절단 고리와 단면

큰 역할: Cut 경계를 닫힌 고리로 만든 뒤 2D 삼각분할한다.

| 함수·형식 | 역할 |
| --- | --- |
| Point2 | 평면 위 double 2D 점 |
| Cross | 방향·볼록성 |
| PointSegmentDistance / SegmentDistance | 교차·근접 검사 |
| PointInPolygon | 포함 관계. Point2/RingVertex 버전 |
| ExtractCutLoops | 이번 평면의 Cut 간선 → in/out 차수 → 순환 → 2D 투영 → 교차·포함·방향 검사 |
| RingVertex | 원래 node 번호 + 2D 점 |
| SignedArea | 링 방향과 면적 |
| BridgeHole | 구멍 오른쪽 점에서 보이는 가까운 링 정점에 다리 연결 |
| EarClip | 충분히 볼록하고 다른 점을 품지 않는 귀를 순차 제거 |
| BuildCap | 링 방향 정리·구멍 연결·삼각분할·corner/face 추가·접합/면적 검증 |
| cornerFor lambda | rim node당 캡 corner 재사용, 평면 normal/UV/색 |
| 내부 Fail | CapTriangulationFailed와 이유 |

loop의 각 점은 들어오는 Cut 간선 하나와 나가는 간선 하나가 필요하다. 기존 경계에 끝나면 OpenLoop, 분기면 NonManifoldLoop 등으로 실패한다. 루프 자기/상호 교차·접촉, 면적 0, outer+hole보다 깊은 중첩을 거부한다.

BuildCap은 rim node만 사용한다. 중앙점이나 독립 Cap node는 만들지 않는다. 단면 normal/UV가 표면과 다르므로 corner는 새로 만든다. 양수 cap normal=-n, 음수 cap normal=+n. 표면과 반대 방향으로 한 번씩 간선을 공유하는지 확인 후 CapSeam으로 기록한다. copy인 capped를 검증하고 성공 시 교체하므로 실패 시 입력 조각을 유지한다.

### 6.11 Geometry/SliceTypes.h — 공통 상태·수학

큰 역할: 여러 파일이 같은 상태·sentinel·벡터 계산을 쓰도록 한다.

| 항목 | 역할 |
| --- | --- |
| SliceStatus | Ok/NoCut, 입력·origin·연결·퇴화·coplanar·cap·loop·삼각분할·사각 범위 오류 |
| kNoNode | 0xffffffff. 0은 유효 인덱스라 없음 표시로 쓰지 않음 |
| Fail | detail 저장 후 지정 상태 반환 |
| Finite | float2/3/4 유한값 |
| Num | 오류용 숫자 문자열 |
| Clamp | 범위 제한 |
| V3 / ToV3 / ToFloat3 | double 벡터와 변환 |
| +,-,*,Dot,Cross,Length,Normalize | 기하·ray·drag 연산 |

NoCut은 잘못된 입력과 다른 정상적인 비절단 결과다. LoopOutsidePlane은 SlicePatch가 아니라 컨트롤러가 설정한다.

### 6.12 Rendering/RenderBake — 절단 자료를 그릴 배열로

큰 역할: PatchPiece의 연결·속성을 ProceduralMeshData로 펼친다. GPU 리소스를 만들지는 않는다.

| 함수·옵션 | 역할 |
| --- | --- |
| RenderBakeOptions::splitByOrigin | 미분할 Surface와 분할 Surface를 디버그 구역으로 분리 |
| BuildRenderMesh | corner i→vertex i, node 위치+corner 속성, face.corner→indices, section 생성 |
| UpdateRenderPositions | 개수와 참조 범위 검증 후 position만 복사 |
| EdgeKindColor | 경계 태그 색 |
| AppendDebugSegment | 선분을 3면 튜브 옆면 메시로 추가 |
| BakeBoundaryDebug | key 순서대로 경계 튜브. CapSeam은 열린 경계가 아니라 제외 |
| BakeLoopDebug | 마지막-처음까지 이어 닫힌 튜브 고리 |

렌더 구역은 표면, 필요 시 분할 표면, cap 순이다. 빈 구역은 만들지 않는다. materialSlot은 표면0/cap1이지만 현재 렌더러의 section 색은 구역 배열 번호로 결정한다. materialSlot은 실제 재질 적용 경로가 아직 없다.

### 6.13 ProceduralMesh — CPU 렌더 데이터 관리

큰 역할: 렌더 데이터 검증, bounds, 변경 버전. 절단과 스키닝 계산은 하지 않는다.

| 자료형·함수 | 역할 |
| --- | --- |
| ProceduralVertex | position,normal,uv,color |
| MeshSection | firstIndex,indexCount,materialSlot |
| ProceduralMeshData | vertices,indices,sections |
| ProceduralMeshBounds | min,max,center,radius,valid |
| SetMesh | 정점·연결 검증 후 교체, 두 버전 갱신 |
| UpdateVertices | 같은 개수의 정점 검증 후 교체, 정점 버전만 갱신 |
| Clear | 데이터·bounds 초기화, 두 버전 갱신 |
| GetData / GetBounds / 버전 조회 | 읽기 |
| ValidateVertices | 크기·finite 검사, AABB 중심·감싸는 구 |
| ValidateTopology | 인덱스 범위·3의 배수·section 범위/겹침, 필요 시 전체 section |
| NextVersion | 모든 객체에서 유일한 atomic 증가 번호 |
| Finite / Fail | 검사·오류 |

UpdateVertices는 API상 모든 정점 속성을 바꿀 수 있다. 현재 호출부가 주로 position만 바꾼다. 실패 시 이전 데이터 보존. atomic 버전이 클래스 전체의 동시 접근 안전성을 뜻하지 않는다.

### 6.14 Rendering/ProceduralMeshRenderer와 셰이더 — GPU 실행

큰 역할: 버전에 맞는 버퍼 업로드, PSO와 상수 설정, DrawIndexed 기록, GPU 완료 뒤 버퍼 재사용.

| 자료형·함수 | 역할 |
| --- | --- |
| DrawOptions | cull,fill,wireColor,sectionTint,depthTest |
| Buffer | resource,capacityBytes,fence,pending |
| MeshGpu | vertices/indices와 업로드한 버전 |
| 생성자 / 소멸자 | PSO 이름 / Shutdown |
| Initialize | root signature, 48byte 정점 layout, solid/cull/wire/overlay PSO |
| Draw | 대상 format·버전 검사, 업로드, 상태·행렬·버퍼 설정, 구역별 그리기, pending 등록 |
| Acquire | 16byte 정렬, free 버퍼 재사용 또는 생성 |
| Upload | 정렬 임시 데이터로 WriteBuffer |
| Retire | 이전 버퍼를 완료 대기 목록으로 |
| Collect | 제출 대기가 아니고 fence 완료된 버퍼를 free로 |
| NotifySubmitted | Finish의 fence를 pending 버퍼에 연결 |
| ReleaseMesh | CPU 메시 주소 매핑 해제·버퍼 retire |
| Shutdown | GPU idle 후 매핑·풀 제거 |

CPU는 다음 프레임을 준비하는 동안 GPU가 이전 버퍼를 쓸 수 있어 즉시 덮어쓰지 않는다. renderer는 ProceduralMesh의 주소를 key로 삼으므로 주소 안정성과 ReleaseMesh 호출 순서가 필요하다. JobMeshes를 map에 둔 이유 중 하나다.

셰이더 파일:
- [ProceduralMeshVS.hlsl](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Rendering/ProceduralMeshVS.hlsl): Input position/normal/uv/color, World·ViewProj로 위치 변환, color 전달.
- [ProceduralMeshPS.hlsl](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Rendering/ProceduralMeshPS.hlsl): TintMode 0 정점 색, 1 section tint, 2 wire 색, 3 section wire palette.
- 현재 셰이더는 조명·텍스처·joint 스키닝 계산을 하지 않는다. 조각 스키닝은 CPU에서 완료된다.

### 6.15 Debug/CutDebugView — 작업마다 렌더 메시와 UI

큰 역할: 계산 결과는 읽기만 하고 표시용 메시를 소유한다.

| 자료형·함수 | 역할 |
| --- | --- |
| RenderMode | Solid/SolidAndWire/Wire/Debug |
| JobMeshes | 작업별 positive/negative, boundary 둘, previewLoops, 마지막 세대·구역 모드 |
| Sync | 없어진 작업 ReleaseMesh 후 제거, 새 세대 bake, 평소 위치 갱신, 전역 평면/선분/기즈모 생성 |
| Draw | 작업별 조각을 대상 옆 X offset과 ±normal gap으로 표시, 디버그는 원본 위치 |
| DrawUI | 대상 오류·작업 식별·preview/cut 상태·형제 중복 안내·시간·키 |
| SetGizmoView | 카메라 정보 |
| SetRenderMode/CycleRenderMode/GetRenderMode/RenderModeName | 표시 방식 |
| SectionSplit | Debug의 면 종류 구역 분리 여부 |
| Release | 모든 작업/전역 메시의 GPU 등록 해제 |
| ReplaceOrUpdate | 연결 같으면 UpdateVertices, 다르면 SetMesh, 비면 Clear |
| PlaneColor | 후보 preview 합성 상태. 실패 우선 빨강, 성공 녹색 등 |
| BakePlaneDebug | 사각형·격자·normal 화살표를 튜브로 |
| ToolName | 표시 도구 이름 |

표시용 옆 이동은 렌더 행렬이며 조각의 독립 물리 위치가 아니다. 현재 형제 작업은 같은 결과를 같은 위치에 중복 렌더할 수 있다. Debug는 작업별 wire 색을 사용하며 section 분리는 하지만, 실제 DrawOptions.sectionTint를 켜는 것과 동일하지 않다.

### 6.16 Debug/SceneViewCamera — 관찰용 카메라

큰 역할: 절단 입력과 충돌하지 않게 이동·회전·프레이밍한다.

| 함수 | 역할 |
| --- | --- |
| 생성자 | focus 반지름 기준 속도/거리 범위/클리핑, 초기 Frame |
| Update | Look/Pan/Orbit/ZoomDrag 모드와 키·마우스 계산 |
| Frame | 현재 시야각에서 중심·반지름이 보이는 거리 |
| Orientation | heading/pitch 회전 |
| ApplyTransform | pivot+회전+거리로 Camera 업데이트 |
| UpdateAspectRatio | 엔진 규약인 height/width 반영 |
| SetInputEnabled | gizmo drag 중 camera 조작 억제 |
| IsLooking / IsNavigating | 현재 조작 상태 |
| Distance / Pivot | 값 조회 |

RMB+WASD/QE 비행, Shift 빠르게, RMB+wheel 속도, MMB pan, Alt+LMB orbit, Alt+RMB 거리, wheel 줌. F의 대상 선택은 ModelViewer다.

### 6.17 테스트·빌드·문서 파일

| 파일 | 역할 |
| --- | --- |
| [tests/PlaneSlicerTests.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/tests/PlaneSlicerTests.cpp) | GPU 없는 CPU 기하·스키닝·평면·기즈모·작업 key/diff/색 테스트. main의 Test... 목록을 기능 예제로 읽기 |
| [tests/ProceduralMeshSmoke.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/tests/ProceduralMeshSmoke.cpp) | 작은 삼각형으로 Set/Update/Clear·bounds·버전·실패 보존 검사 |
| [MeshCutting.vcxproj](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/MeshCutting.vcxproj) | 정적 라이브러리와 소스·셰이더 빌드 항목 |
| [MeshCutting.vcxproj.filters](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/MeshCutting.vcxproj.filters) | Visual Studio 표시 폴더 |
| [pch.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/pch.h), [pch.cpp](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/pch.cpp) | 미리 컴파일된 헤더 설정 |
| [framework.h](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/framework.h) | WIN32_LEAN_AND_MEAN 정의 |
| [Docs/README.md](C:/Users/Pes/Desktop/DirectX-Miniengine/MiniEngine/MeshCutting/Docs/README.md) | 문서 목차·상태 |

현재 CPU 테스트는 DiffJobs 등 순수 부분을 검사한다. Model에 의존하는 CutController 전체의 등록·대상 전환·부분 실패·뷰 동기화까지 통합 검증했다고 해석하면 안 된다. 이번 보고서 작업에서 테스트를 실행한 것은 아니다.

## 7. 중복과 개선점: 확인한 사실과 권장 순서

### 7.1 유지하는 편이 좋은 분리

- PatchNode와 PatchCorner: 같은 위치와 서로 다른 UV/normal을 함께 표현하므로 합치지 않는다.
- PatchOrigin과 joint weights: 서로 다른 보간 단계라 하나의 배열로 섞지 않는다.
- PatchPiece와 ProceduralMeshData: 기하 연결·출처와 GPU 입력은 역할이 달라 분리 유지.
- CutController와 CutDebugView: 계산 상태와 표시 리소스를 구분하는 구조는 적합하다.
- Skinning/Source 공유: 이미 적용된 중복 감소다. 모든 정점 자료가 조각마다 복사된다고 설명하면 틀린다.
- BuildCap의 검증용 copy: 실패 시 원본 보존이라는 목적이 있다. 단순히 '복사니까 불필요'로 제거하지 않는다.

### 7.2 우선순위 표

| 순서 | 코드로 확인한 부분 | 조언 | 비용이 생기는 시점 |
| --- | --- | --- | --- |
| 1 | CodeMap과 NDC25Reference 코드 대응 일부가 구 구조·함수명을 사용 | 현재 흐름으로 문서 갱신. 이번 보고서가 기준 스냅샷 | 학습·유지보수 |
| 2 | 뷰에서 정점 vector copy 후 UpdateVertices 안에서 또 copy | 검증 후 이동받기 또는 재사용 버퍼 경로. 실패 보존 계약 유지 | 매 프레임·확정 조각별 |
| 3 | ConfirmAll의 result.positive/negative를 copy 대입 | 면적·로그 계산을 먼저 한 뒤 move로 소유 이전 검토 | Enter 성공 때 |
| 4 | preview가 cap 포함 전체 절단, Enter 때 동일 입력 재계산 가능 | patch/pose/plane/options의 동일성이 보장될 때 preview 재사용 | 편집 중 변화·Enter |
| 5 | 추출 결과 두 배열을 MakeWeldInput에서 새 배열로 복사 | 추출이 WeldInput 또는 단일 추출 결과를 직접 채우는 후속 정리 | 신규·복귀 후보 |
| 6 | Skinning 표는 공유하지만 Apply가 조각마다 원본 blend를 재계산 | 같은 source/pose의 원본 현재 위치를 프레임 캐시로 공유할지 측정 | 매 프레임·조각별 |
| 7 | 형제 작업별 patch 복사·행렬 생성·preview·확정·렌더 반복 | 선분별 상태는 유지하고 동일 입력 계산·표시 재사용 검토 | 같은 부모 형제 수만큼 |
| 8 | 겹친 원본 삼각형의 소유권 미정 | 원본 교체 전에 정책 결정. 작업 독립성만으로 해결되지 않음 | 기능 확장 전 |

### 7.3 매 프레임 정점 중복 복사

근거:
- CutDebugView::Sync의 refreshSurface: mesh.GetData().vertices를 새 vector에 복사한다.
- UpdateRenderPositions는 이 임시 vector의 position을 바꾼다.
- ProceduralMesh::UpdateVertices는 검증 후 replacement = vertices로 다시 복사한다.
- 이후 GPU 업로드도 수행한다.

권장: 우선 검증 뒤 move받는 overload나 용량 재사용 scratch로 불필요한 CPU 복사/할당을 줄인다. 공개 mutable GetData를 열어 버리는 방식은 bounds/version/실패 보존을 우회하므로 권하지 않는다. 속도 향상 크기는 데이터 크기를 측정해야 안다.

### 7.4 확정 결과 copy

근거: ConfirmAll에서 job.positive = result.positive와 negative 대입은 기하 배열을 복사한다. 공유 skin은 포인터만 공유하지만 nodes/corners/faces/boundary/caps는 copy다.

권장: result에서 면적·face 수·loop 수·로그를 먼저 계산하고 move한다. 현재 코드의 뒤쪽 진단이 result 조각을 읽으므로 대입만 기계적으로 move로 바꾸면 안 된다.

### 7.5 미리보기와 확정의 중복

근거: RunPreview와 ConfirmAll은 각각 SliceWithPlane을 호출한다. 둘 다 cap 생성과 사각형 검사까지 수행한다. 애니메이션 행렬 변화가 previewDirty를 만들므로 이동 중에는 프레임마다 재계산할 수 있다.

권장: 작업 patch 버전·포즈 버전·평면 버전·옵션을 구분할 수 있을 때 최신 성공 preview를 확정에 재사용한다. invalid/dirty preview를 재사용하면 현재 자세와 다른 결과가 된다. 루프만 계산하는 별도 preview는 cap 성공 여부까지 미리 검증하던 동작이 달라지므로 별도 설계 선택이다.

### 7.6 추출 재포장과 장기 보관

근거:
- ExtractJob의 local ProceduralMeshData와 WeldInput은 임시지만 실제 vertex/index 복사는 생긴다.
- BonePatchSource::vertices에는 바인드 위치·joint/weight가 남고, Skinning에도 용접된 원본 재료가 남는다. 배열의 인덱스·목적은 달라 곧바로 동일 배열로 합칠 수는 없다.
- 현재 매 프레임 BuildSkinMatrices는 source.paletteId와 source.meshes를 사용한다. source.vertices/triangles는 주로 출처 추적이다.
- poseJointWorld는 추출 때 생성하지만 현재 컨트롤러의 자세 변화 감지는 previousSkinMatrices로 한다. ComputePoseJointWorld는 현재 모듈 주 경로에서 호출되지 않는다.

권장:
1. 먼저 추출이 WeldInput 정점을 직접 작성해 중간 합치기 copy를 없앤다.
2. 런타임 행렬 연결 정보와 디버그 provenance의 보관 필요를 구분한다.
3. 실제 원본 마스킹에 source.triangles가 필요할 수 있으므로 사용이 적다는 이유만으로 삭제하지 않는다.
4. pose snapshot 제거는 CapturePoseSnapshot과 팔레트 world 검증의 의미를 같이 확인한 후 결정한다. ComputePaletteSkeleton에도 같은 공간 제약이 있어 배열 하나 삭제로 다중 공간 지원이 생기지는 않는다.

### 7.7 형제 작업: 독립 처리와 중복 출력

근거:
- ExtractJob은 target/kind/palette/parent가 같으면 sibling.patch를 copy하고 source.bone을 공유한다.
- child는 작업 key에는 포함되지만 추출 범위에는 쓰지 않는다.
- 같은 평면으로 같은 patch를 자르므로 같은 결과가 생길 수 있다.
- CutDebugView::Draw는 형제 결과를 동일한 표시 위치에 각각 그린다.

권장: 선분별 작업 상태를 유지하면서 추출 키(target,palette,parent) 기준의 계산 캐시를 검토할 수 있다. 표시 중복 제거는 병합과 다른 문제다. 각 선분 결과를 따로 보여주는 것이 목적이면 별도 offset을 주고, 같은 결과가 겹쳐 보이는 혼동을 줄일 수 있다. 새 관리 계층을 먼저 만들기보다 실제 형제 수와 비용을 측정한다.

서로 다른 부모의 패치도 혼합 가중치로 원본 삼각형이 겹칠 수 있다. 현재는 복사본 데모라 허용되지만 원본 제거·교체 시에는 각 삼각형을 누가 담당할지 정해야 한다. 이 프로젝트의 선분별 처리 요구와 NDC 참고 문서의 일부 다중 절단 병합 기법은 같은 정책이 아니므로 혼동하면 안 된다.

### 7.8 기타 작지만 구체적인 조언

- CutJobKey의 a/b/c는 팔레트/부모/자식을 매번 해석하게 한다. GetPalette/GetParentJoint/GetChildJoint 같은 이름 있는 접근 함수부터 추가하면 자료형 폭증 없이 읽기 좋아진다.
- CutDebugView는 SetMesh/UpdateVertices 오류 문자열을 받지만 대부분 UI에 전달하지 않는다. 기하 절단은 성공했는데 표시 갱신이 실패한 경우 구분할 수 있게 뷰 오류 보고를 권한다.
- GetFrameStats의 extract/cut은 작업별 마지막 값 합, skin/preview는 지수 평균 합이다. 매 프레임 실제 총 비용으로 오해하지 않게 현재 주석을 UI 명칭에도 반영하면 좋다.
- CutPlane::CutJoints는 현재 런타임 미사용이지만 테스트가 있다. old API와 test 역할을 확인한 후 유지/정리한다.
- source.extractJoint와 key.b, source.kind와 key.kind처럼 중복될 수 있는 메타데이터는 의도를 명시한다. 향후 추출 정책을 별도로 바꿀 자리라면 유지 가치가 있다.
- m_LastSubmittedFence는 현재 저장하지만 renderer 내부의 다른 경로에서 읽히지 않는다. 작은 정리 후보이며 성능 우선 과제는 아니다.
- normal은 CPU 자세 갱신에서 유지되고 현재 shader도 조명에 쓰지 않는다. 나중에 조명 추가 시 표면·단면 normal 갱신 정책을 함께 구현한다.
- StaticMesh enum/key가 있어도 실제 작업 생성·비스키닝 용접·변환 경로가 없으므로 스태틱 절단 지원이라고 표시하지 않는다.
- 무효 raw pointer를 UpdateTargets에서 검사해서 안전하게 복구할 수는 없다. 인스턴스 해제 전 UnregisterInstance 계약을 유지한다.
- CPU tests는 기하와 key/diff를 다루지만 전체 CutController 통합을 보장하지 않는다. 작업 수명·일부 성공/실패·등록 해제·모델 교체·형제 재사용 검증을 다음 실행 확인 항목으로 권한다.

## 8. 처음 읽는 사람에게 권하는 순서

1. 이 문서 3절 다이어그램과 4절 한 프레임을 먼저 읽는다.
2. CutJob.h에서 key/target/job이 무엇을 담는지 본다.
3. ProceduralMesh.h에서 렌더 출력 형식을 확인한다.
4. BonePatch.h → ExtractJob → ExtractBonePatch → MakeWeldInput 순으로 원본 입구를 읽는다.
5. Patch.h의 Face→Corner→Node→Origin→SkinVertex를 예제와 함께 읽는다.
6. BuildPatchPiece를 읽어 이 관계가 어떻게 만들어지는지 본다.
7. Skinning::Apply로 '새 교차점이 왜 다음 자세에서도 움직이는가'를 이해한다.
8. CutController::Update와 CutPlane::PickSegments를 다시 읽어 선택·작업 수명을 연결한다.
9. SlicePatch → Slicer::Run → ProcessFace → CutNodeOf → BuildPiece를 읽는다.
10. ExtractCutLoops → BuildCap → BridgeHole/EarClip으로 단면을 이해한다.
11. RenderBake → CutDebugView → ProceduralMeshRenderer 순으로 표시를 읽는다.
12. 마지막에 기즈모·카메라와 테스트의 작은 상자/튜브 입력을 읽는다.

## 9. 현재 범위에서 기억할 결론

현재 구조의 중심은 **평면 하나 + 등록 대상 목록 + 선분별 CutJob map**이다. 각 작업은 미절단 patch와 preview/confirmed를 가진다. 기하와 렌더 배열의 분리는 필요하며, 원본 스키닝 표는 이미 공유한다. 다음 정리는 모든 타입을 합치는 것보다 **프레임별 중복 복사 → 결과 소유 이전 → preview 재사용 → 추출 재포장 감소** 순서가 이해와 검증에 유리하다.

이 문서는 소스 정적 분석이다. 실제 시간·프레임율·GPU 결과를 검증한 성능 보고서나 테스트 성공 보고서는 아니다.

