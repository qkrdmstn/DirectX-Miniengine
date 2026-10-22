상태: 완료 (채택한 U-1~U-5b 코드 경로) — 현재 공통 메시 API는 ProceduralMeshSimplification.md 기준. 아래는 과거 설계 이력이며 실제 물리는 미구현 (`d418fcd` 이후 작업 트리).

# Unreal 방식 정렬 로드맵

>
> 목적: 절단 기능을 Unreal의 `ProceduralMeshComponent`, `UKismetProceduralMeshLibrary::SliceProceduralMesh`, `BreakConstraint` 흐름에 맞춘다. BVH는 쓰지 않는다. 근거: `NDC25Reference.md` 7절, 측정은 `PickPerformanceDesign.md`.

## 1. Unreal 흐름과 지금 흐름의 차이

Unreal에서 절단은 **맞은 순간 한 번** 일어난다 **[확인: 공개 API 형태]**.
1. (스태틱) `CopyProceduralMeshFromStaticMeshComponent`로 ProceduralMesh를 만든다. 스켈레탈(NDC)은 자를 Bone의 영향 범위를 ProceduralMesh로 복사한다 **[슬라이드]**.
2. `SliceProceduralMesh(ProcMesh, 평면 위치, 법선, bCreateOtherHalf, OutOtherHalf, CapOption, CapMaterial)`로 자른다.
3. 원본을 숨기고(스태틱: 컴포넌트 통째, 스켈레탈: 버텍스 컬러 마스크), 스켈레탈은 `BreakConstraint`로 본을 끊는다.

지금 코드(`cd9a09e`)는 평면을 편집하는 동안 **후보마다 패치를 미리 추출·용접하고 매 프레임 스키닝**한다(`ApplyDiff` → `ExtractJob`, `SkinJobs`). 측정에서 가장 큰 비용이 이 미리 추출이다(Xbot 걷기 중 추출 1회 약 6 ms, 초당 약 35~40회, `PickPerformanceDesign.md` 2절). Unreal에는 이런 "자르기 전 패치"가 없다.

| 단계 | Unreal | 지금 | 정렬 후 |
| --- | --- | --- | --- |
| 무엇을 자를지 | 충돌 결과(Bone/컴포넌트) | 교차 삼각형 → joint (`CutPick::PickJoints`) | 그대로 (가볍다) |
| ProceduralMesh 생성 | 맞은 순간 | 후보가 생길 때마다 미리 | **Enter 순간** |
| 생성 전 스키닝 | 없음 | 후보 패치 매 프레임 | **없음** |
| 절단 | `SliceProceduralMesh` | `SlicePatch` (Enter) | 같은 시점, Unreal 모양의 진입점 |
| 원본 처리 | 숨김 | 없음 | U-4 |
| 분리 | `BreakConstraint` / 반쪽 물리 | 없음 | U-5 |

## 2. 단계 (한 단계 = 개념 하나, 약 300줄, 단계마다 멈춤)

| 단계 | 내용 | 줄이는 것 / 얻는 것 |
| --- | --- | --- |
| **U-1 절단 시점 생성 (구현, `ccf0349`)** | 편집 중에는 joint 선택(`PickJoints`)과 작업 목록만 갱신한다. 패치 추출·용접·스키닝은 Enter 때 작업마다 한 번(현재 자세로) 하고 바로 절단한다 | 추출 반복과 후보 패치 스키닝 제거 (측정상 최대 비용). Unreal 호출 시점과 같아짐 |
| **U-2 Unreal 모양의 진입점 (구현, `ccf0349`)** | `SliceProceduralMesh(inOutMesh, planePosition, planeNormal, createOtherHalf, outOtherHalf, capOption, capMaterialSlot, ...)`를 `SlicePatch` 위에 얇게 둔다. CapOption은 NoCap / CreateNewSectionForCap / UseLastSectionForCap. 세부는 4절 | 이름·인자를 Unreal과 맞춤 (알고리즘 변경 없음) |
| **U-3a 스태틱 메시 복사 기반 (구현, `ccf0349`)** | CPU 접근(`m_GeometryCPU` 보존, Unreal `bAllowCPUAccess`), 메시 하나를 절단 입력으로 복사(`CopyProceduralMeshFromStaticMesh`, `CopyProceduralMeshFromStaticMeshComponent` 대응), world 행렬 하나로 위치 갱신(`BuildRigidMatrices`, 강체 정점). 세부는 5절 | 스태틱 절단의 입력과 위치 갱신 |
| **U-3b 스태틱 메시 연결 (구현, `ccf0349`)** | 컨트롤러의 후보 찾기·Enter 경로에 U-3a를 연결한다. 세부는 5.1절 | 스태틱 절단 지원 |
| **U-3c 정적 모델 표시 (구현, `762538f`)** | ModelViewer가 `Models/cube.glb`, `bunny.glb`를 캐릭터 +X 쪽에 띄우고(크기 = 캐릭터 반지름 0.6배, 중심 간격 4배·8배) 절단 대상으로 등록한다. `ModelInstance::SetPosition` 추가. 파일이 없으면 건너뛴다 | 화면 확인 |
| U-4 원본 숨김 | 스켈레탈: 복사 범위 마스크(color/depth/shadow). 스태틱: 그 메시 그리기 끄기 | NDC 3단계 |
| U-5 분리 | `BreakConstraint` 대응 디버그 분리(joint 서브트리를 따로 움직임), 남는 쪽 기여 교체 | NDC 2·5단계 |
| (측정 후) 섹션 컬링 | `PickPerformanceDesign.md` 안 B. pick이 다시 병목일 때만 | pick 비용 |

## 3. U-1 세부

- **편집 중(매 프레임):** `UpdateTargets`, `RefreshPick`(캐시 스키닝 + `PickJoints`), `DiffJobs`/`ApplyDiff`로 작업 key 목록만 갱신. `ApplyDiff`의 added에서 `ExtractJob`을 부르지 않는다. 후보 작업은 key, joint, 교차 삼각형 수, 색만 가진다.
- **Enter (`ConfirmAll`):** 후보 작업마다 `ExtractJob`(추출 + 용접) → `BuildSkinMatrices` + `Skinning::Apply`로 현재 자세 → `SliceWithPlane` → 확정 조각. 작업이 끝나면 미절단 패치는 버린다(확정 조각만 남김).
- **확정 조각:** 지금처럼 매 프레임 스키닝(`SkinJobs`)한다. 스킨 행렬 출처는 작업의 `source`.
- **표시에서 후보 패치를 쓰던 곳:** `CutDebugView`의 `patchRadius`(tube 반경, 조각 간격·배치 폭)는 대상 반지름(`CutTarget::radius`) 기반으로 바꾼다. "patch n tris"는 확정 후에만 보인다.
- **웨이트 뷰·작업 색:** key와 joint만 쓰므로 그대로.
- **Enter 프레임 비용:** 작업마다 추출(Xbot 약 6 ms) + 절단(약 3 ms)이 Enter 한 프레임에 몰린다. Unreal도 맞은 순간에 복사·절단하므로 같은 성격이다. 계측 줄로 Enter 프레임 값을 확인한다. 분산(프레임 나눠 처리)은 측정 후 필요할 때만.
- **추출 반복 원인 로그(key가 경계에서 오가는지)**는 U-1 뒤에는 비용이 없어지므로 필수가 아니다. 작업 목록이 깜빡이는 표시 문제가 보이면 그때 본다.

## 4. U-2 세부

| Unreal `SliceProceduralMesh` | 이 프로젝트 `SliceProceduralMesh` (`Geometry/Slice.h`) |
| --- | --- |
| `InProcMesh` (제자리 변경, 법선 쪽을 남김) | `PatchPiece& inOutMesh`. 성공하면 양의 쪽 조각으로 바뀐다 |
| `bCreateOtherHalf`, `OutOtherHalfProcMesh` | `createOtherHalf`, `PatchPiece* outOtherHalf`. 거짓이면 `outOtherHalf`는 빈 조각 |
| `PlanePosition`, `PlaneNormal` (world, 내부에서 컴포넌트 로컬로 옮긴다고 추정 **[추론]**) | 같은 이름, world. 패치가 이미 world라 **변환하지 않는다** |
| `CapOption`, `CapMaterial` | `SliceCapOption`, `capMaterialSlot` (재질 대신 재질 번호) |
| (반환 없음) | `bool` 반환, `outStatus`, `error`. 실패하면 `inOutMesh`·`outOtherHalf`·`outLoops`를 바꾸지 않는다 (`SlicePatch` 트랜잭션 계약) |
| — | `outLoops`: 양의 쪽 번호의 절단 루프. 컨트롤러의 유한 평면 검사용 확장 |

- **내부:** `SlicePatch` 호출 하나와 결과 이동뿐이다. 법선 정규화·검증은 `SlicePatch` 규칙 그대로다. `cutId`는 Unreal 인자에 없으므로 `inOutMesh`의 가장 큰 cutId + 1을 쓴다(처음 자르는 패치는 1, 지금 Enter 경로와 같다). NoCap은 `CapMode::None`, 나머지는 `CapMode::Create`.
- **cap 재질:** 이번 절단의 `PatchCap`(양쪽 조각)에 `materialSlot`과 `inLastSection`을 기록하고, `BuildRenderMesh`가 읽는다. 표면 section(재질 0) 뒤에 cap 재질 번호마다 section 하나(처음 나온 순서). `inLastSection` cap의 면은 마지막 표면 section(`splitByOrigin`이면 "나뉜 면" section) 끝에 붙는다. 기본값(슬롯 1, 새 section)의 출력은 이전과 바이트 단위로 같다.
- **Unreal과 다른 점:** (1) 로컬 공간 변환 생략. (2) 유한 평면 사각형 검사(`CutPlane::ContainsLoops` → `LoopOutsidePlane`)는 Unreal에 없는 확장이라 `CutController::SliceWithPlane`에 남긴다. (3) Unreal의 CreateNewSectionForCap은 절단마다 새 section을 만들지만, 여기서는 같은 재질 번호의 cap을 한 section에 모은다(이전 출력 유지). (4) 렌더러(`ProceduralMeshRenderer`)는 아직 재질 번호가 아니라 section 순서로 색을 고른다.
- **Enter 경로:** `SliceWithPlane`이 `SliceProceduralMesh(사본, ..., true, CreateNewSectionForCap, 1)`을 부른다. `job.patch`는 면적 비교에 남기므로 사본을 자른다.

## 5. U-3a 세부

| Unreal | 이 프로젝트 |
| --- | --- |
| `bAllowCPUAccess` | `ModelLoader`가 스킨 없는 모델에서도 `m_GeometryCPU`를 지우지 않는다 |
| `CopyProceduralMeshFromStaticMeshComponent` | `CopyProceduralMeshFromStaticMesh(model, instance, meshIndex, outMesh, outSource)` (`StaticMeshCopy.h`) |
| 컴포넌트 transform | `BuildRigidMatrices`: 슬롯마다 행렬 하나 = 메시의 현재 world 행렬 |

- **출력 형식:** `ExtractBonePatch`와 같아서 `MakeWeldInput` → `BuildPatchPiece`를 그대로 쓴다. `outMesh`는 현재 world 위치·법선(world 역전치), UV0, 회색. `outSource.vertices`는 meshSlot 0, 로컬 위치·법선, joints·weights 0. `triangles`는 (0, draw, primitive). `meshes`는 { meshIndex, meshCBV, startJoint = kNoNode }, `paletteId`·`jointIndex`는 kNoNode, `joints`·`poseJointWorld`는 비어 있다.
- **정점 디코드:** 스킨 없는 레이아웃(POSITION, NORMAL R10G10B10A2, [TANGENT], [UV0 half2], [UV1])을 `StaticMeshCopy.cpp`에 따로 둔다(`BonePatch.cpp`의 디코드는 익명 namespace). stride가 `vbStride`와 다르면 실패. 같은 원본 정점은 remap으로 한 번만 복사.
- **강체 정점:** `SkinVertex::jointCount == 0`. `Skinning::Apply`는 이 정점을 `slotMatrices[meshSlot][0]` × 바인드 위치(`Skinning::RigidPoint`)로 옮긴다. jointCount > 0인 정점의 계산은 그대로다. 복사 때 world 위치도 `RigidPoint`로 계산해서 첫 갱신과 비트 단위로 같다.
- **용접:** `BuildPatchPiece`는 가중치가 모두 0인 정점을 거절했다. 이제 **모든 정점**이 가중치 0이면 강체 입력으로 받고, 일부만 0이면 지금처럼 거절한다. 용접 키는 그대로(슬롯, 바인드 위치, 빈 스킨). `ValidatePatchPiece`는 스킨 가중치를 보지 않아 바꾸지 않았다.
- **메모리:** 스킨 없는 모델은 geometry 크기만큼 CPU 메모리를 더 쓴다(지금 ModelViewer는 Xbot(스킨)만 쓰므로 변화 없음. Sponza 경로를 쓰면 `.mini` 파일 크기 9.1 MB 이하).
- **Unreal과 다른 점:** 메시 하나 단위(컴포넌트 전체가 아님), 재질 section은 복사하지 않고 section 하나, 로컬 공간 대신 world 공간 piece.
- **테스트:** Model 없이 가능한 부분(강체 용접, 강체 Apply, 강체 piece 절단, 스킨 Apply 불변)은 `PlaneSlicerTests.cpp`. `CopyProceduralMeshFromStaticMesh`·`BuildRigidMatrices`는 Model이 필요해 빌드와 코드 검토로 대신했다. 화면 확인은 U-3b에서 정적 glb(예: Khronos Box·Duck)를 넣어 한다.

## 5.1 U-3b 세부

| Unreal | 이 프로젝트 |
| --- | --- |
| 섹션 박스 vs 평면 조기 판정 **[추론]** | `CutPick::SphereMayCross`: 로컬 바운딩 구(`Mesh::bounds`)를 world로 옮겨 평면 사각형(구 반지름 + epsilon만큼 넓힘)과 비교 |
| 컴포넌트 로컬로 옮긴 평면으로 정점 검사 **[추론]** | `CutPick::CountCrossedTriangles`: 평면을 메시 로컬로 옮겨 삼각형 부호를 본다 (정점은 world로 옮기지 않는다) |
| `CopyProceduralMeshFromStaticMeshComponent` → `SliceProceduralMesh` | Enter: `CopyProceduralMeshFromStaticMesh` → `MakeWeldInput` → `BuildPatchPiece` → `BuildRigidMatrices` + `Skinning::Apply` → `SliceWithPlane` |

- **대상의 메시 목록:** `ReloadPalettes`가 `ListStaticMeshes`로 `kHasSkin` 없는 메시를 `CutTarget::staticMeshes`(`CutStaticMesh`)에 모은다: 메시 번호, `meshCBV`, 로컬 바운딩 구. 로컬 위치·인덱스 캐시는 편집을 처음 시작하는 프레임에 `BuildStaticCache`가 `CopyProceduralMeshFromStaticMesh`로 한 번 만든다(실패하면 편집 모드를 다시 켤 때 재시도, 오류는 화면에 표시).
- **후보 찾기:** `PickStaticMeshes`가 평면이나 메시 world 행렬이 바뀐 프레임에만 다시 계산한다. 구 판정에서 건너뛰면 교차 0, 아니면 `CountCrossedTriangles`. 교차 삼각형이 하나 이상이면 `MakeStaticMeshKey` 후보이고, 목록 갱신은 기존 `DiffJobs`/`ApplyDiff`다.
- **PickJoints와 같은 규칙:** 부호(epsilon 안은 0), 평면 양쪽에 걸침 판정, 절단선 선분과 사각형 교차를 `CutPick.cpp`의 공통 함수(`SignOf`, `IsMixed`, `CutLineHitsRectangle`)로 묶었다. `PickJoints` 결과는 그대로이고, 테스트가 두 함수의 교차 수·계측 개수 일치를 확인한다. 로컬 정점의 부호 거리는 `(M n)·p + (t·n − o·n)`이라 법선을 늘이지 않고도 world 거리와 같아서(비균등 크기에서도) epsilon 규칙이 같다. 교차 에지의 끝점 둘만 world로 옮겨 사각형과 비교한다 (`SegmentHitsRectangle`; `CutPlane::Contains`는 점이 아니라 선분 교차 규칙을 PickJoints와 맞추려고 쓰지 않았다).
- **구 반지름:** 3x3의 크기 배율은 sqrt(최대 행 합 × 최대 열 합)(최대 특이값의 상한)이라 구를 키우기만 한다. 반지름이 0이거나 유한하지 않으면 건너뛰지 않는다.
- **Enter:** `ExtractJob`이 `StaticMesh` 작업이면 `CopyProceduralMeshFromStaticMesh`, 아니면 `ExtractBonePatch`를 부른다. `source.kind`는 작업 종류, `extractJoint`는 스태틱이면 kNoNode. `SkinPatch`와 `SkinJobs`의 행렬은 `BuildJobMatrices`가 `StaticMesh`이면 `BuildRigidMatrices`, 아니면 `BuildSkinMatrices`로 만든다. 가짜 joint나 스킨 정보는 없다.
- **표시:** 작업 줄 "[대상] mesh N  crossed n tris", 확정 후 "patch n tris". 스태틱 메시는 joint가 없어서 웨이트 뷰 색표(`GetJointColors`)에 들어가지 않는다. 작업 색은 확정 조각에만 쓴다. 웨이트 디버그의 스킨 없는 메시 색은 `DefaultVS.hlsl`이 `weightColor`를 먼저 중립 회색(0.12)으로 두므로 셰이더 수정이 필요 없다. 계측 줄 "Static: meshes · culled · tris checked"(`CutFrameStats::staticMeshes/staticCulled/staticTriangles`; culled는 그 프레임에 다시 계산한 메시 중 건너뛴 수)가 추가됐다. 정밀 검사한 삼각형의 mixed/crossed는 기존 개수에 합산된다.
- **Unreal과 다른 점:** (1) 메시 하나 단위 (2) 유한 사각형 평면 (3) 평면을 world 법선 그대로가 아니라 로컬 거리 식으로 옮김 (4) 원본은 숨기지 않는다 (U-4).
- **테스트:** `PlaneSlicerTests.cpp`의 `TestCountCrossedMatchesPickJoints`, `TestCountCrossedLocalPlane`(이동·회전·비균등 크기), `TestSphereMayCross`(놓치지 않음, 경계, 크기 배율). `CutController`의 스태틱 경로는 Model이 필요해 단독 테스트가 어려워 빌드(Debug/Release)와 코드 검토로 대신했다. 화면 확인은 U-3c 이후.

## 6. 결정·제한
- Unreal 내부 구현(섹션 박스 조기 판정, 로컬 공간 평면)은 소스 미확인이라 [추론]으로 둔다.
- U-1은 "미리 보기 없이 Enter 때 한 번" 정책이므로 Enter 전에는 단면 모양을 볼 수 없다(자동 미리보기는 다시 넣지 않기로 함).
- 스켈레탈의 joint 선택 규칙(교차 삼각형별 joint, 덩어리별 최대 웨이트 joint)은 `JointCutDesign.md` 결정에 따른다. 이 문서는 생성 시점만 바꾼다.
