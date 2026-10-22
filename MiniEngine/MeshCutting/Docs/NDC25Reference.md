# NDC25 「저 메시의 목을 쳐라!」 참고 정리

> 이 프로젝트(MiniEngine/MeshCutting)가 따르는 원본 기법의 정리다. 코드 리뷰와 설계 판단의 기준으로 쓴다.
>
> 정리: 2026-10-21. 근거 수준을 항목마다 표시한다.
> - **[슬라이드]** 사용자가 공유한 발표 영상 캡처로 직접 확인
> - **[기사]** 게임조선 기사로 확인
> - **[계획서]** 사용자 연구 계획서에 적힌 시간대 메모 (영상으로 다시 확인하지 않음)
> - **[추론]** 위 자료와 리그 원리로 추론. 사실로 단정하지 않는다

## 1. 출처

| 자료 | 내용 |
| --- | --- |
| 발표 | NDC25, 넥슨 팀 컴플루언스 전수현, 「저 메시의 목을 쳐라! - 실시간 스켈레탈 메시 절단」, 28:19 |
| 영상 | https://www.youtube.com/watch?v=I5bSUJ9-9ns |
| 기사 | 게임조선 「[NDC 25] 저 메시의 목을 쳐라! 넥슨 팀 컴플루언스 전수현 프로그래머의 절단신공」 https://www.gamechosun.co.kr/webzine/article/view.php?no=214816 |
| 엔진 | Unreal Engine (ProceduralMeshComponent, `SliceProceduralMesh`, `BreakConstraint`, Physics Asset) |

## 2. 용어 대응 (중요)

| NDC / Unreal | 이 프로젝트 | 설명 |
| --- | --- | --- |
| **Bone** | **joint** | transform 하나(이름, 부모, 로컬 변환). 웨이트가 붙는 대상. 선분이 아니다 **[슬라이드: Bone1/Bone2가 상자(transform)로 그려짐]** |
| 에디터의 뼈 그림(부모→자식 선) | **segment** | 표시용일 뿐 데이터가 아니다. 부모 joint의 transform을 따라 움직인다 |
| `BreakConstraint(BoneName)` | (미구현) 본 분리 | 그 Bone과 **부모 Bone 사이의 물리 관절**을 끊는다. 표면 분할이 아니다 |
| ProceduralMesh | `ProceduralMesh` + `PatchPiece` | 런타임에 바꾸는 메시 |
| 버텍스 컬러 마스킹 | (미구현) 원본 숨김 | 원본에서 늘어난 범위를 지운다 |

## 3. 늘어나는 이유 **[슬라이드 5:54 부근 「메시가 늘어나는 이유 / 스키닝 애니메이션」]**

- 정점 하나가 Bone1과 Bone2에 함께 맵핑돼 있다(혼합 웨이트). 자막: "각 Bone을 쫓아서 이 점이 두 개로 맵핑이 됩니다".
- `BreakConstraint`로 두 Bone이 떨어지면, 그 정점은 두 위치를 섞은 허공에 그려진다. 자막: "자연스럽게 늘어진 모습으로 그려지게 됩니다".
- **떨어진 두 쪽 모두에 웨이트가 있는 정점만 늘어난다.** 한쪽에만 웨이트가 있는 정점은 그쪽을 따라 깔끔하게 움직인다. **[추론, 스키닝 원리]**

## 4. 절단 과정 **[슬라이드 6:17~6:51 「구현 상세 / 해결 방안」]**

| 순서 | 슬라이드 문구 | 내용 |
| --- | --- | --- |
| 1 | 「BreakConstraint()를 사용할 본 선택」 | 자막 "우선은 절단할 위치에서 Bone을 선택합니다". 메시에서 선택한 본에 영향받는 범위를 탐색 → 해당 범위를 프로시저럴 메시로 복사 → 복사한 메시를 절단 |
| 2 | 「BreakConstraint()로 절단 표현」 | 원본 스켈레탈 메시의 관절을 끊는다. 원본은 어깨에서 손까지 길게 늘어난다(이미지) |
| 3 | 「늘어진 메시를 감춤」 | "늘어진 범위는 곧 본에 영향 받는 범위, 즉 1에서 복사한 범위와 동일" → 버텍스 컬러로 마스킹. 결과 이미지는 늘어난 팔이 사라지고 바닥에 손만 남음 |

**6:29 프레임** **[슬라이드]**: 자막 "이렇게 절단된 메시는 잠시 옆으로 치워두고요". 복사한 패치를 자른 두 조각을 옆으로 옮겨 둔 모습이다.
- 패치는 천 재질(재킷 소매 메시)이고, 어깨 쪽 입구부터 소맷부리까지 이어진다. 한 조각에 검은 단면(cap)이 있다.
- 복사는 메시 단위로 일어난다. 분홍색 손은 다른 메시(장갑·몸)로 보인다. **[추론]**
- 어느 Bone을 선택했는지는 슬라이드에 이름이 없다. 확정하지 않는다.

## 5. 이 프로젝트가 채택한 해석 (사용자와 합의, 2026-10-20)

1. 절단 위치가 놓인 **segment(부모 joint → 자식 joint)를 찾고, 그 부모 joint**에 영향받는 모든 삼각형을 패치로 복사한다.
2. 패치를 평면으로 자른다.
3. 원본 스켈레톤에서 그 부모 joint를 `BreakConstraint`처럼 분리한다(부모 joint와 그 부모 사이가 끊긴다). **절단 위치와 끊기는 관절 위치는 다르다.**
4. 원본에서 1의 복사 범위를 버텍스 컬러(마스크)로 숨긴다.
5. 잘라 둔 두 조각을 각각 남는 쪽과 떨어지는 쪽에 붙인다.

이 해석에서 나오는 성질 **[추론]**:
- 패치에는 "정점 하나라도 선택 joint 웨이트가 있는 삼각형"이 들어가므로, **패치 경계 정점은 선택 joint 웨이트가 0**이다. 그래서 끊은 뒤에도 원본과 패치의 경계 이음매가 벌어지지 않는다.
- 선분 A→B의 양 끝 혼합 구간(A+A의 부모, A+B)에 모두 A 웨이트가 있다. 그래서 joint A 하나의 범위가 선분 전체와 양 끝을 덮는다.
- **5번이 어렵다.** 떨어지는 쪽 조각은 그대로 따라가면 된다. 하지만 남는 쪽 조각은 떨어진 joint의 웨이트를 가지고 있어서 단순 스키닝이면 다시 늘어난다. 떨어진 joint의 기여를 절단 시점 위치로 바꿔야 한다. **[계획서: NDC 약 7:27~8:14 "떨어져 나간 본에 대응하는 정점 기여를 절단 당시 위치로 교체"]**
- 예외: 선택 joint를 거치지 않고 자손 joint와 바깥 joint에 함께 걸친 정점은 패치 밖에 남는데도 늘어난다. 코드에서는 `BonePatchSource::mixedOutsideTriangles`로 센다.

## 6. 발표의 나머지 내용 **[기사]** (일부는 **[계획서]**)

| 주제 | 내용 |
| --- | --- |
| 수동 스키닝 | ProceduralMesh는 스키닝을 지원하지 않아 매 틱 정점을 직접 갱신했다 |
| cap | 단면을 본에 부착 **[계획서 8:22~8:46. 기사로는 확인 안 됨]** |
| 반복·다중 본 절단 | 처음엔 "자른 뒤 합치기"였는데 조각이 맞물리지 않았다 → **"합친 뒤 자르기"**로 바꿨다. 여러 본이 겹친 영역을 세로로 자를 때 문제였다 |
| 메시 아일랜드 | 의도한 절단선과 실제 결과의 차이로 떨어진 조각이 생긴다. 워헤이븐식 사지 마킹은 연속 절단에 맞지 않았다. **Lattice Deformer**: 메시를 3D 격자에 매핑하고 셀 연결 그래프(토폴로지 맵)를 같이 잘라 아일랜드를 찾는다 |
| 각도 보정 | 절단면이 경계를 건드리면 실패한다. 양쪽 경계를 컨벡스 볼륨으로 감싸 충돌을 검사하고, 겹치면 가장 가까운 안전한 평면으로 옮긴다 |
| 울퉁불퉁한 단면 | `FDynamicMesh3` 불리언 연산 |
| 상처 표현 | Left 4 Dead 방식: 레퍼런스 포즈 공간의 데칼과 디폼(절단·애니메이션과 독립). 날카로운 절단과 둔기 피해를 구분 |
| 내부 표현 | Dead Island 2(외피 아래 내부 메시, 데칼 블렌딩)를 참고 사례로 언급. 이 시스템이 이중 메시를 쓴다는 뜻은 아니다 |
| 향후 | 부위별 프로시저럴 메시 대신 스켈레탈 메시 전체를 다이나믹 메시 하나로 관리, 필요하면 추가 본이나 스켈레탈 메시를 스폰. 아직 개발 중 |

## 7. Unreal `SliceProceduralMesh`

- **[공식 문서]** 입력: ProceduralMeshComponent, world 평면 위치와 법선, `bCreateOtherHalf`, `CapOption`(NoCap / CreateNewSectionForCap / UseLastSectionForCap), CapMaterial. 법선이 향하는 양(+)쪽을 남기고, 옵션에 따라 반대쪽을 새 컴포넌트로 만든다. 단순 볼록 충돌체도 자른다. 구현: `ProceduralMeshComponent/Private/KismetProceduralMeshLibrary.cpp`.
- **[추론, 소스 미확인]** 섹션 바운딩 박스로 평면 한쪽에 있는 섹션은 통째로 보내고, 걸친 섹션만 삼각형 단위로 분할한다. 잘린 에지를 2D로 투영해 다각형으로 이은 뒤 삼각분할해서 cap을 만든다.
- **뼈·웨이트 개념이 없다.** "어느 Bone을 자를지"는 발표자가 직접 만든 로직이다. 언리얼에서는 보통 충돌 결과(`FHitResult.BoneName`)로 Bone을 얻는데, NDC가 그렇게 했는지는 확인되지 않았다.

## 8. 현재 프로젝트 대응 (`d418fcd` 이후 통합 작업 트리)

| NDC 단계 | 현재 코드 | 상태 |
| --- | --- | --- |
| Bone 선택 | CutPick::PickJoints. 스태틱은 SphereMayCross → CountCrossedTriangles | 실제 메시 교차 기반 구현 |
| 영향 범위 복사 | ExtractBonePatch / CopyProceduralMeshFromStaticMesh → ProceduralMesh::Build | 공통 ProceduralMesh 생성 구현 |
| 복사본 절단 + cap | SliceProceduralMesh(ProceduralMesh) → 내부 SliceGeometry / ExtractCutLoops / BuildCap | 구현. ContainsLoops로 유한 사각형 범위 승인 |
| 수동 스키닝 | BuildPieceMatrices → ProceduralMesh::UpdatePose → Skinning::Apply | 스킨/강체 갱신 구현 |
| BreakConstraint | CutController::UpdateBrokenJoints → ModelInstance::BreakConstraint | 디버그 world 고정·이동 구현. 실제 물리 아님 |
| 원본 숨김 | SetMeshVisibility / UpdateSkinMask → SetVertexColorOverride → color/depth 셰이더 discard | 구현 |
| 남는 쪽 기여 교체 | SetBreakOf / BuildPieceMatrices의 절단 순간 행렬과 자손 집합 | 구현 |
| cap 본 부착 | rim Vertex를 공유하여 표면을 따라감 | 독립 cap 본 부착은 미구현 |
| 병합 후 절단 / 아일랜드 / 각도 보정 | RegionCutDesign 등 | 미구현·보류 범위 |

이 표의 구현은 코드 확인 기준이다. 이번 구조 정리의 GPU 화면 검증을 뜻하지 않는다.

## 9. CesiumMan 리그에서 확인한 사실 (CesiumMan.glb, bind 자세, Z가 위)

- 팔 첫 joint(`Skeleton_arm_joint_L__4_`=5, `Skeleton_arm_joint_R`=6)가 가슴 안쪽(|y| 0.09, z 1.07)에 있다. 첫 팔 segment가 몸통 안에서 시작한다.
- 손 끝 joint(9, 10)가 말단이라 손 부분(|y| 0.45~0.57)에는 segment가 없다. 평면이 손만 지나면 segment 선택에 잡히지 않는다.
- `arm_joint_R`(6)의 웨이트는 가슴 옆부터 손목 근처까지 넓게 퍼져 있다.
- 목 joint 3, 4의 웨이트(> 0.05)는 정점 822개, 2,177개로 머리와 가슴 위쪽 대부분을 덮는다. 목을 자르면 패치가 매우 커진다.
- 팔·다리 joint의 `mixedOutsideTriangles`는 0, `torso_joint_3`은 40이었다(작업 메모).

## 10. 아직 확인하지 못한 것
- 6:29 패치를 만든 정확한 Bone (위팔인지 아래팔인지)
- 기여 교체의 정확한 좌표계와 식 (7:27~8:14)
- cap이 본에 부착되는 방식 (8:22~8:46)
- Bone 선택 방법 (충돌 결과인지, 다른 방법인지)
