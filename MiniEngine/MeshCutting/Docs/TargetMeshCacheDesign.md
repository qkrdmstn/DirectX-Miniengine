상태: 진행 중 — 스태틱/스켈레탈 공통 원본 캐시 구축 (Debug/Release 빌드 확인 완료, 커밋 전)

# 대상 메시의 공통 용접 캐시

## 목적

대상 등록 후 포즈 행렬을 읽을 수 있는 첫 `CutController::Update`에서, 스태틱 메시와 각 스킨 팔레트의 입력을 같은 `ProceduralMesh::Build` 경로로 용접·검증하고 보관한다. 등록 시점에는 `ModelInstance` 포즈가 준비되지 않을 수 있으므로 실제 생성은 첫 Update에서 한다. 평면 편집 여부는 캐시 생성 조건이 아니다.

## 공통 캐시와 별도 입력

`CutTargetMeshCache`는 용접된 `ProceduralMesh`, 대응 `MeshSource`, `WeldStats`, Build 오류를 한 단위로 소유한다. `CutStaticMesh`와 `CutTargetSource`가 이 캐시를 포함한다.

- 스태틱 메시: `ReadStaticMesh`가 반환한 입력으로 공통 캐시를 만들며, 기존대로 캐시 생성 world 행렬이 현재 world와 비트 단위로 같을 때 Enter에서 메시를 재사용한다. pick용 로컬 위치·인덱스와 오류는 별도 유지한다.
- 스켈레탈 팔레트: `BuildSource`가 모든 팔레트 joint를 읽은 `ProceduralMeshData`로 공통 용접 캐시를 만든다. 같은 입력의 bind 위치·weights·인덱스는 기존 pick 캐시에도 복사한다. 공통 메시 Build가 실패해도 pick의 `MeshSource`와 포즈 입력은 유지한다.

스켈레탈 캐시는 기존 pick 입력과 용접 `ProceduralMesh`를 함께 보관하므로 등록 뒤 첫 Update의 CPU 작업과 메모리 사용이 늘어난다. `ProceduralMesh`가 원본 렌더 데이터와 용접 기하를 모두 소유하고 pick cache도 별도 위치·skin 배열을 유지한다. 현재 캐시 비용이나 메모리는 측정하지 않았다. 스켈레탈 용접 실패는 후보 선택을 막지 않으며, 평면 편집을 껐다가 다시 켤 때 재시도한다.

`BuildMeshGeometry`의 용접 키는 `meshSlot`, bind 위치, canonical joint/weight다. 스태틱의 가중치 없는 열린 경계는 `OpenSurface`, 스켈레탈 패치의 열린 경계는 `Replacement`(또는 감지된 `UnweldedSeam`)으로 판정된다. 같은 `Build` 인터페이스를 쓰되 입력 skin binding에 따라 기존 경계 의미가 유지된다.

## 이 단계의 경계와 다음 단계

이 단계는 공통 캐시의 생성·저장과 기존 스태틱 재사용을 통일한다. 스켈레탈 Enter는 아직 `ExtractBonePatch`로 원본을 다시 읽고 joint 패치를 새로 용접한다. 전체 팔레트 캐시에서 joint별 면을 추출해 Enter의 재용접을 없애는 작업은 별도 단계다. 그 단계는 패치 경계의 `Replacement`/`UnweldedSeam`, `MeshFace::sourceTriangle`, `MeshSource` 매핑과 현재 부분 추출 결과가 같은지 다뤄야 한다.

기준 문서: `NDC25Reference.md` 8절(현재 영향 범위 복사 구현), `PhysicsSkeletalCutDesign.md` 3절 B9 및 4.5절(`IPoseSource`/`CutTargetRegistry` 접점), `CodeMap.md` 생성·현재 자세 흐름.
