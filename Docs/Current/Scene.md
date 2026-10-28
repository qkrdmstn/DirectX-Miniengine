상태: 진행 중 — 5a~5c 낙하·바닥·pause, 6b 물리 절단 transaction, 6d 충돌 overlay, 7a EditTarget 선택 완료; collider 유효성 검색 순서 최적화 완료 (`5f56b93d` 기반); 조각 드래그·Undo(7b·7c)와 관절 미구현 (2026-10-28)

# ModelViewer GameObject와 Gizmo

`MiniEngine/ModelViewer`를 작업 디렉터리로 실행하면 Cube·Bunny·Xbot이 각각 X=-5/0/5에 배치된다.
Xbot은 첫 번째 애니메이션 클립을 반복 재생한다. `GameObject::PlayAnimation`으로 클립 하나를 지정한다.
원점의 4×4 청록색 격자는 모델 없는 Cut Plane 객체이며 시작 시 선택된 객체는 없다.
평면 내부를 클릭하면 선택되고 테두리가 노란색으로 바뀐다. 이동·회전·TRS Undo는 모델과 같다.
평면의 R 크기 모드는 X/Z만 표시하며 중앙 핸들도 XZ만 변경하고 Y는 보존한다.
평면 선은 장면 깊이에 가려지고 깊이에는 쓰지 않는다. `C`는 현재 평면 영역의 Static 메시를 조회해 GPU 절단한다. 교차 미리보기는 없다.
기존 `-model <경로>` 인수는 단일 모델을 열고 모든 애니메이션을 반복 재생한다.
진단용 `-view 0|1|2`는 시작 표시 모드를 Solid/Wireframe/Vertices로 선택한다.
생략하거나 범위 밖이면 기본 Solid이며, 실행 중 `V` 순환은 그대로 유지된다.
예: `../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -view 2`.
유효한 `-view` 실행만 시작 모드·PSO 준비·첫 장면 GPU 작업 완료를 debugger에 한 번씩 기록한다.
이 debugger 로그 자체는 첫 장면 명령 제출 후 fence 완료를 확인하며 화면 정확성을 입증하지 않는다. 별도 GPU 캡처의 시각 확인·픽셀 비교는 GPU 설계 14.1절에 기록했다.

## 조작

| 입력 | 동작 |
|---|---|
| 왼쪽 클릭 | 편집 대상 선택(월드 독립 조각은 Mesh 대상, Attached는 owner 대상), 빈 공간에서는 선택 해제 |
| `Q` | View: Gizmo 숨김·조작 해제, 클릭 선택 유지 |
| `W` | 이동: RGB 축 또는 XY/YZ/ZX 사각형 드래그 |
| `E` | 회전: RGB 링 드래그 |
| `R` | Local 크기: X/Y/Z 축 방향으로 드래그하면 해당 성분만 변경; 중앙 사각형은 위/아래로 세 성분 배율 변경 |
| `L` | 이동·회전의 World / Local 축 전환 (크기는 항상 Local) |
| `V` | Solid → Wireframe → Vertices → Solid 순환 (현재 모드 overlay 표시) |
| `C` | 현재 유한 평면 영역의 Static 컴포넌트 후보를 조회하고 실제 분할만 게시. 처리 중 재입력은 Busy 안내 |
| `P` | 물리 step 실행/일시정지. 디버그 UI `Viewer/Physics/Running`과 동일 상태 |
| `H` | 조각별 충돌 convex hull 모서리와 물리 바닥 상자 표시 켜기/끄기(기본 꺼짐). 디버그 UI `Viewer/Physics/ShowCollision`과 동일 상태. 깨어 있음 초록·잠듦 파랑·body 없음 노랑·바닥 회색 |
| 드래그 중 `Esc` | 시작 Transform으로 복구 |
| `Ctrl+Z` | 마지막 완료 드래그 한 번 취소; 최근 128개 보관 |
| 오른쪽 버튼 유지 + 마우스 | 카메라 회전 |
| 오른쪽 버튼 유지 + `WASD` / `QE` | 카메라 이동 / 상승·하강 |

드래그 중에는 카메라와 Gizmo 도구 모드 전환을 차단한다. V 표시 모드는 전환할 수 있다. 핸들이 객체 선택보다 우선한다.
오른쪽 버튼으로 카메라를 조작하는 동안에는 Q/W/E/R로 도구를 바꾸지 않는다.
ModelViewer는 비독점 마우스 입력을 사용해 선택과 카메라 조작 중 시스템 커서를 표시한다.
이동 축 끝은 화살표, 크기 축 끝은 사각형으로 구분한다.
`Q` View는 Gizmo를 숨기는 도구이며 왼쪽 드래그 카메라 pan은 제공하지 않는다.
포커스를 잃거나 커서가 클라이언트 영역을 벗어나거나 대상이 삭제되면 드래그를 취소한다.
취소와 변환 없는 클릭은 Undo에 넣지 않는다. 삭제된 대상의 Undo는 건너뛴다.
드래그 밖에서 `Esc`를 누르면 기존 ModelViewer 동작대로 종료한다.

## 소유권과 변환

- `Viewer::Scene`이 `unique_ptr<GameObject>`를 소유한다. 객체 주소는 삭제 전까지 유지된다.
- `ObjectId`는 Scene 안에서 증가하는 `uint64_t`이고 `0`은 invalid다. 삭제·`Clear()` 후 재사용하지 않는다.
- `GameObject`는 모델 없이도 존재하며, 모델이 있으면 별도 `ModelInstance`를 소유한다.
- `GetTransform()` / `SetTransform()`은 `SceneTransform`의 위치·Quaternion·양수 XYZ 크기를 다룬다. 각 성분은 `0.0001`~`1e8`이며 범위를 벗어난 값과 NaN/Inf를 거부하고 회전을 정규화한다.
- `ModelInstance` 하나는 공유 원본과 CPU 애니메이션·node·joint pose만 소유한다. 원본 모델은 최초 `MeshComponent` 하나로 표현한다.
- `Scene::Update`는 owner마다 `ModelInstance::Update`를 한 번 호출하고 모든 메시 컴포넌트에 같은 읽기 전용 pose를 전달한다.
- 각 `MeshComponent`는 자체 Transform·GeometryRevision·PosedGeometry·상수 버퍼를 소유한다. `Attached`는 `Wc=owner*component`, `WorldIndependent`는 component 월드 TRS를 사용한다.
- 변형은 `Vc=inverse(SourceFrame)*node*skin(p)`를 Compute에 저장하고 VS에서 `Wc`를 한 번 적용한다. source 프레임과 target 이동은 독립이며, 기존 화면을 보존하려면 Attached target을 SourceFrame으로 맞춘다. `SetPlacement`는 둘을 함께 검증·게시한다.
- 기본 정책 `FollowModel`은 공유 pose를 따라가고 `Frozen`은 다음 유효 Update의 node/joint snapshot을 고정한다. 선택 본 분리·기여 보정 정책은 아직 없다.
- 렌더는 각 컴포넌트의 현재 revision section만 등록한다. Static node bound와 Skeletal 전체 본 변환 구의 합으로 컴포넌트 경계를 만들며, 스킨 경계는 보수적이지만 느슨할 수 있다. 첫 유효 pose 전·기준 변경 후·pose 실패 시 선택을 보류한다.
- 컴포넌트 GPU 버퍼 삭제는 현재 전역 GPU 완료를 기다린다. 단계별 검증은 [GPU 설계 14절](../GPU/GPUCuttingSystemDesign.md)을 따른다.

`Scene::CreateObject`, `FindObject`, `DeleteObject`, `Clear`로 객체를 관리한다.
매 프레임 `Scene::Update` 후 기본·그림자 pass에서 각각 `Scene::Render`를 호출한다.
`TransformGizmo`는 viewport 하나와 Scene 하나를 대상으로 입력·overlay를 처리한다.

## 현재 범위

표시 모드의 입력·PSO·깊이·알파·검증 계약은
[RenderModes.md](RenderModes.md)를 따른다.

선택은 각 메시 컴포넌트의 CPU 경계 구와 가장 가까운 교차점을 이용하고 `PickHit`에 ObjectId와 MeshId를 함께 반환한다. 구가 겹치거나 빈 공간이 많은 모델에서는 근사 선택이다.

선택 결과는 `Scene/EditTarget.h`의 `EditTarget{Kind(Owner|Mesh), Object, Mesh}`다. `ResolveEditTarget`이 pick을 R8 규칙으로 바꾼다. `WorldIndependent` 컴포넌트(simulate 여부 무관)를 맞히면 Mesh 대상, `Attached` 컴포넌트나 모델 없는 객체(절단 평면)는 Owner 대상이다. 순수 규칙은 `EditTarget::FromPick`(CPU 테스트 가능)이고, `IsEditTargetValid`는 owner·컴포넌트 존재와 월드 독립 여부를 확인하며 ModelViewer가 매 프레임 무효 대상을 해제한다. `TryGetEditTargetTransform`은 기즈모 위치·축으로 쓰는 월드 TRS를 주는데 Owner는 `GameObject::GetTransform`, Mesh는 `MeshComponent::GetWorldTransform`을 TRS로 분해한 값이다. `GameObject::IsEditable`은 바닥 같은 명시적 편집 잠금만 보며, 잠긴 객체는 pick에서 제외한다. owner 기즈모는 `MeshComponent::GetWorldTransform`이 WorldIndependent에서 owner를 곱하지 않으므로 Attached 컴포넌트만 움직인다.

임시 제한(7b·7c 전): Mesh 대상은 선택되고 기즈모가 조각 위치에 보이지만 드래그와 `Ctrl+Z`는 거절하며 화면에 "Piece editing not ready"를 약 2초 표시한다. Owner 대상의 드래그·Undo는 그대로다. History는 아직 owner만 기록한다.
Gizmo는 깊이 검사 없는 overlay이며 축과 카메라가 거의 나란하면 해당 축/회전 링 조작을 제한한다.
크기는 각 축 `0.0001`부터 `1e8`까지의 양수다. 음수·영 크기, 계층 편집, Redo,
장면 저장은 구현하지 않는다. 충돌 hull이 있는 Static 메시의 낙하·바닥 접촉·pause는 [물리 5단계](../Physics/StaticPiecePhysics.md)의 범위다. simulate 조각은 Mesh 대상으로 선택되며 편집(드래그·Undo)은 7b·7c 전까지 거절한다. `-physics-sim 0`은 기존 비물리 동작을 유지한다. Static 절단은 [구현 기록](../GPU/StaticCutImplementation.md)을 따른다.

현재 velocity 셰이더는 카메라 이동만 반영하고 객체 Transform·스킨 애니메이션의 이전 위치는 반영하지 않는다.
따라서 ModelViewer는 TAA를 끄고 FXAA를 사용하며 모션 블러도 끈다. TAA를 다시 켜면 움직이는 객체에
이전 프레임 잔상이 재발할 수 있다. 객체별 이전 Transform·스킨 pose를 사용하는 velocity pass는 아직 없다.

후속 GPU 절단은 대상 `MeshComponent::GetWorldTransform`(component 월드)을 로컬 좌표와 월드 좌표 사이의 계약으로 사용해야 한다.
절단 평면과 입력 정점은 같은 공간으로 변환한 뒤 교차 계산해야 한다. 공유 원본
`Model::m_DataBuffer`는 수정하지 않고, 절단 결과 GPU 버퍼는 대상 MeshComponent의 geometry로 소유하도록 연결한다.
현재 `GeometryRevision`은 공유 원본 section과 생성 GPU geometry를 모두 지원한다. Static 절단·cap·재절단의 최신 흐름과 진단은 [StaticCutImplementation.md](../GPU/StaticCutImplementation.md)를 따른다.

## CPU 검증

`MiniEngine/ModelViewer/Scene/tests/SceneMathTests.cpp`는 실제 `GizmoMath`, `TransformHistory`, `EvaluateModelPose`를 호출한다.
표준/역방향/무한 역방향 Z의 투영·레이 왕복, 축/평면 드래그, 회전 부호·π 경계,
양수 크기, Undo와 삭제된 대상, DFS 부모 복원 및 skeletonRoot 유/무의 locator 단일 적용을 검사한다.
VS Developer PowerShell에서 파일 머리말의 명령을 사용하고 `/Fo`, `/Fe`는 저장소 밖으로 지정한다.
GPU 렌더링과 실제 마우스 조작은 별도 런타임 확인이 필요하다.

## 비균일 변환과 회전 검토

크기 핸들은 항상 객체 로컬 축이다. World 축으로 비균일 신장을 적용하면 전단이 필요할 수 있어
L 전환은 이동·회전에만 적용한다. overlay도 크기 모드에서는 Local을 표시한다.
축 드래그는 시작 시 고정한 축을 포함하는 ray-plane 교차의 축 방향 이동량을 사용한다.
중앙 핸들은 기존 위/아래 픽셀 이동으로 모든 성분에 같은 배율을 적용하고 각 성분을 개별 제한한다.
Esc는 시작 TRS 전체를 복구하고 Undo도 XYZ 크기를 함께 저장한다.

표시·선택·드래그의 축은 공통 `GizmoMath::Basis`에서 구한다. World는 고정 XYZ,
Local은 객체 Quaternion만 사용한다. 드래그 중에는 시작 기저를 표시에도 유지하여
로컬 회전 도중 표시 링과 실제 드래그 평면이 어긋나는 중복 계산을 제거했다.
카메라로 축을 정의하는 경로는 확인하지 못했다. 카메라 이동에 따라 링의 원근 투영과
보이는 타원 모양은 달라진다. 회전 느낌에 대한 실제 마우스·GPU 재현은 미확인이며,
링을 화면에 고정하지 않는다. 거의 옆으로 보이는 링은 기존 조작 제한을 유지한다.

선택 구 중심은 전체 affine으로 변환한다. 반지름은 `AffineRadiusScale`의 신장 상한을 쓴다.
이는 TRS에서 최대 축 크기이고, locator와 내부 회전 합성으로 생기는 전단에서도 보수적이다.
메시 컬링·정렬에도 같은 도우미를 써서 극단적인 비균일 크기에서 구를 과소평가하지 않는다.
전단에서는 구가 커져 선택·컬링이 느슨해질 수 있다. 원본 경계가 애니메이션을 감싸는지는 기존 한계다.
시작 장면은 공유 원본 모델의 반지름으로 owner TRS를 정규화한다. ModelInstance의 locator·Resize·렌더 API는 제거했다.
정적·스킨 메시 모두 모델 공간 joint 법선 역전치와 source-to-component/node 역전치를 Compute에 적용하고,
컴포넌트 월드 역전치는 VS에서 한 번 적용한다. 비교용 기존 VS 경로는 전체 WorldIT를 유지한다.
Solid/Wireframe/Vertices 표시 정책도 동일하다.

CPU 회귀 검사는 공통 TRS·축 기저·축별 배율·히스토리·경계 도우미를 호출한다.
비균일 점/법선 변환, 합성 전단 구, XYZ 독립/중앙 배율, 양수 상하한·무효값,
비균일 Undo, 카메라 위치·회전 변화의 기저 불변성과 투영 차이,
두 skeletonRoot 경우의 비균일 locator 단일 적용을 포함한다.


## MeshComponent와 Collider 참조 API

`GameObject::AddMeshComponent` / `FindMeshComponent` / `RemoveMeshComponent`는 같은 원본 pose를 사용하는 컴포넌트를 관리한다. ComponentId는 owner 내 mesh·collider 공용 증가 값이며 제거 후 재사용하지 않는다. 서로 다른 owner에서는 같은 ComponentId가 나올 수 있으므로 외부 참조는 항상 ObjectId와 함께 저장한다.

`GeometryRevision::FromModel`은 loader가 만든 유효한 Model metadata를 전제로 원본 GPU VB/IB·재질·draw·SourceMesh를 참조한다. `SelectSections`는 기존 GPU 버퍼를 복사하지 않고 section 부분집합의 새 불변 revision을 만든다. `MeshComponent::SetGeometry`는 같은 원본만 허용하고 pose 준비를 무효화한다. 이 절은 컴포넌트 기반 구현 이력이다. 이후 추가한 `GeometryRevision::FromGenerated`는 Static 절단 VB/IB·parent provenance를 보관한다.

`AddColliderComponent`는 MeshId·GeometryRevisionId와 SDK 비노출 QueryHandle을 보관한다. `PhysicsSystem::SyncColliders`가 준비된 Static 메시마다 참조와 조회 전용 Box/body를 자동 등록한다. `SetColliderQueryHandle`은 현재 revision의 참조만 갱신하고, 실제 SDK 수명은 PhysicsWorld가 소유한다. `IsColliderCurrent`는 geometry 교체로 오래된 참조를 거부한다. 메시 제거 시 대응 collider 참조를 제거하고 다음 SyncColliders에서 SDK body도 정리한다. 낙하 body는 같은 PhysicsWorld의 별도 layer/handle/map을 쓰며 `SyncBodies`가 GPU 준비와 무관하게 등록·정리한다.

`IsColliderCurrent`는 collider ID가 일치하는 항목을 찾은 뒤에만 `FindMeshComponent`를 호출한다. 한 owner의 mesh·collider가 각각 N개일 때 유효성 검사 한 번은 O(N), 전체 `SyncColliders`의 해당 검색은 O(N²)이다. ID 비교 전에 모든 collider의 메시까지 검색하던 O(N³) 경로를 제거했으며, revision 판정과 수명 처리는 유지한다.

2026-10-28 진단: 실제로 한 번 절단한 큐브 조각의 geometry·배치·충돌 hull을 같은 owner에 복제하고 물리 step을 정지한 body 378개에서 각 구간을 12회 평균 측정했다. `SyncColliders`는 Debug 52.23→1.75 ms, Release 7.29→0.23 ms였다. 사용자 원래 절단 장면이나 전체 프레임 시간의 계측은 아니다. CPU는 i5-1135G7, 감지 GPU/드라이버는 MX450/32.0.15.9186 및 Iris Xe/27.20.100.9415이며 활성 렌더 어댑터는 별도 기록하지 않았다. Debug·Release에서 `RunPhysicsQueryChecks`의 geometry 교체·복원 및 메시·owner 삭제 검증을 DirectX 검증 활성화(`-debug 1`) 상태로 통과했다. 임시 계측 코드는 제거했으며 재현용 임시 패치는 시스템 임시 폴더의 `modelviewer-cpu-measurement.patch`에 보관했다.

`MakeWorldIndependent`는 양수 TRS로 월드 배치를 재현할 수 있을 때만 분리한다. `AdvanceSimulation`은 입력 후 stale body 정리→고정 step→원점 pose 반영을 완료하고 그 뒤 GPU pose를 준비한다. 바닥은 NotCuttable/NotEditable이며 simulate 조각은 Mesh 대상으로 선택되고 편집(드래그·Undo)은 7b·7c 전까지 거절하며, owner 편집은 Attached 컴포넌트만 움직인다. 물리 세대·조회 옵션과 simulation 옵션·진단/검증 범위는 [StaticPiecePhysics](../Physics/StaticPiecePhysics.md)의 H절을 따른다.

`ModelViewer::Update`는 Scene.Update/Finish 뒤 collider를 동기화하고 새 절단을 제출한다. `CuttingSystem::RequestAll`은 `PhysicsSystem::QueryCutCandidates`로 후보를 받아 기존 snapshot 큐에 넣는다. 같은 GameObject의 여러 MeshId를 독립 후보로 유지하며 SDK 미사용·용량 부족은 기존 경계 검사로 처리한다. 구현·진단·검증은 [JoltQueryIntegration.md](../Physics/JoltQueryIntegration.md)를 따른다. 기즈모·Undo는 여전히 owner 단위다.

생성·삭제·geometry/placement 변경은 프레임 입력 단계, 즉 `Scene::Update`의 GPU 명령 기록 전에 수행한다. Update부터 모든 Render pass의 명령 제출 사이에는 geometry/컴포넌트를 변경하지 않는다. 현재 GPU 대기 정책은 이미 제출된 명령의 수명을 보장하며, 아직 제출하지 않은 명령을 자동으로 제출하거나 보존하지 않는다. 공유 pose 배열은 해당 프레임 렌더 소비까지 유지되며 GameObject는 MeshComponent를 ModelInstance보다 먼저 파괴한다.

진단 `-component-checks 1`은 각 모델에 메시 두 개를 만든다. 하단은 FollowModel, 상단은 source 프레임 보정·Frozen·WorldIndependent다. 구조/ID/revision/collider/TRS 검증과 첫 GPU 준비 검증을 수행하고 debugger에 결과를 기록한다. 기본 실행 장면은 기존 Cube·Bunny·Xbot을 유지한다.
