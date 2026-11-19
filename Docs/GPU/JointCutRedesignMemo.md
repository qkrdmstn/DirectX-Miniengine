상태: 조사 메모 — 관절 부근 절단 재설계 착수 전 현황(HEAD `d91fdc9e`, 2026-11-19). 결정 문서가 아니다.

# 관절 부근 절단 재설계 조사 메모

## 1. 문제

- 지금 패치 선택은 선택 본 P 자체에 weight>0인 정점이 하나라도 있는 삼각형이다(`PatchExtractCS::Selected`, 팔레트 마스크).
- 그래서 관절 부근이나 넓은 부위를 자르면 잘리지 않는 경계 띠가 남는다.
- 2026-11-17 재설계안(미결정):
  - break 관절: 뼈대만으로 정한다. 유한 사각형 안 교차 선분의 최상위 P를 쓰고, 교차가 없으면 D5로 간다.
  - 패치 삼각형: 평면 사각형이 지나가거나(`Crossing`·`Overlaps` 재사용), 정점의 본이 2개 이상 그룹에 걸친 삼각형이다.
  - 조각 배정: `LabelPatchIslands` 뒤 평면 쪽으로 후보 그룹을 좁히고, 같은 쪽에 여러 그룹이 있을 때만 가중치 투표한다.
  - 다중 P는 항상 합쳐서 자른다.

## 2. 현재 선택 경로

| 경로 | P와 break 관절 | 추출 본 |
|---|---|---|
| Rig `SelectRigSegment` | 두께 2 mm 사각형 OBB로 물리 body를 조회한다. 후보 선분의 유한 사각형 교차점 중 중심에 가장 가까운 부모 P를 고르고, break는 `FindIncomingJoint(P)`다(RR-2 후보 조건 적용). | P 자체 |
| Parent `SelectSegmentJoint` | 팔레트 최근접 조상으로 선분을 구성한다. 유한 사각형 교차 중 중심 최근접 P를 고르고, 없으면 leaf다. | P 자체 |
| 다중 `SelectMultiCut`→`SelectCrossingJoints` | **무한 평면** 교차 후보에서 조상이 있는 P를 제외하고, 각 P의 incoming을 batch로 break한다. | P들 자체를 OR(`BuildJointUnionMasks`). 자손은 포함하지 않는다. |

- `Request` 우선순위: Rig+물리 활성이면 Rig 결과가 우선이다. 다중 옵션은 roots가 2개 이상일 때만 단일 선택을 대체한다.
- 지금 코드는 "뼈대만으로 유한 사각형 교차 최상위 P 전부"를 고르지 않는다.

## 3. 재설계안 부품의 현재 위치

| 부품 | 현재 |
|---|---|
| CPU 유한 사각형 선분 교차 | `SelectSegmentJoint`, `SelectRigSegment`. 다중 `SelectCrossingJoints`에는 사각형 범위 검사가 없다. |
| 최상위 P 필터 | `SelectCrossingJoints` |
| 본→그룹 표 | `BuildCutBodyGroups`(`NodeGroups`, `AttachNodes`, `FrozenPalettes`). A=나머지, B_i=P_i와 자손. |
| 아일랜드·투표 | `LabelPatchIslands`: (cell, side) 연결 성분마다 세 정점 전체 weight를 그룹별로 합산해 최대 그룹을 고른다. 동점이면 A다. |
| side로 후보 축소 | 같은 구현은 없다. `RelabelCapLoops`가 실제 loop에 닿은 아일랜드를 child-side=B_i, 반대=A로 사후 재라벨한다. |
| GPU 사각형 교차 | `PatchExtractCS::Crossing`→`Overlaps`. 지금은 Mode 3(Influence)만 호출한다. |
| D5 | `SelectRigSegment` fallback → `PublishRigBonePatch`. 절단·break 없이 node 부착 패치로 게시한다. |

## 4. 미확인 전제 — 확인됨

- **원본은 break 뒤에도 rig로 skinning된다.** `FinalizeSimulationPoses`가 `CollectPoseOverrides`로 Dynamic 본 pose를 모으고, `ModelInstance::FinalizePose`가 팔레트를 재생성한다. `SetTriangleMask`는 IB만 바꾼다.
- **`FrozenPalettes`는 실제 사용 경로다.** `Request` → `PatchSkinSnapshot::Frozen` → 매 프레임 `BuildPatchPalette`로 이어진다. 자기 그룹 본은 live로 따라가고, 다른 그룹 본은 부착 기준으로 고정된다. 경계 교점과 cap은 강체 슬롯 K를 쓴다. D5는 Frozen 전부 0이다.

## 5. 패치 선택을 바꿀 때의 접점

- Mode 0 선택식이 `available && (Crossing(...) || 그룹 수>=2)`로 바뀐다. 그룹 수는 세 정점의 양수 weight 슬롯 전체에서 센다.
- 입력: `NodeGroups`를 section 팔레트 순서로 투영해 t3(`m_Masks`, `MaskOffset`)에 올린다. 주의할 점이 두 가지다.
  - 그룹 0(A)도 유효하므로 `Mask!=0` 판정을 쓸 수 없다.
  - t3는 Influence에서 팔레트→NodeId, 기본 경로에서 0/1이라는 다른 의미로 쓰인다. 셋째 의미를 섞지 않도록 분리해야 한다.
- CPU 참조 `ExtractPatchReference`와 `Validate`도 같은 OR 조건으로 바꿔야 한다. 출처 순서(section→draw→triangle, prefix)는 선택식과 분리되어 있어 유지된다.
- `Crossing`은 양쪽 면적이 생기는 부호 교차만 잡는다. 평면에 닿기만 하는 삼각형을 포함할지는 정해야 한다.

## 6. 재현·측정 자료

- MI-5 CesiumMan 기록 명령(`-skeletal-break-demo 2 -skeletal-break-demo-bone …`)이 있다.
  - 위팔: 491/167
  - 척추: 182/337, uncapped 17
  - 다중 허벅지: 300/187/175
- `-skeletal-recut-repro`(RR-1)는 두 번 연속 요청, 모드 지정, 캡처에 재사용할 수 있다. PASS 기준은 게시 여부뿐이다.
- **"띠가 없어짐"을 판정할 정량 기준이 코드에 없다.** RM-2로 교차·미포함 개수 집계도 없어졌다.

## 7. 다음 주 결정이 필요한 질문

1. break 관절을 정할 때 Rig(물리 hit) 결과와 뼈대 전용 선택 중 무엇을 기준으로 할지, D5로 넘어가는 조건은 무엇인지.
2. 다중 P 선택을 유한 사각형으로 제한할지(지금은 무한 평면).
3. 패치 선택식의 그룹 표를 어떤 버퍼로 넘길지(t3 의미 분리), 평면에 닿기만 하는 삼각형을 포함할지.
4. side로 후보를 좁히는 규칙과 기존 `RelabelCapLoops`의 관계, 넓어진 패치에서 loop가 충돌할 때의 처리.
5. 경계 띠 정량 기준: 예를 들어 "사각형 안 교차 삼각형 중 패치에 포함되지 않은 수 = 0"을 진단으로 다시 넣을지.

## 8. 단계 분할 후보(근거만)

- A: 선택기·`Request` 우선순위·D5 계약
- B: 그룹 표 입력·Mode 0·CPU 참조·`Validate`
- C: side 후보 제한·투표·loop 재라벨·cap 귀속
- D: 새 경계·Frozen·재절단 측정과 문서 동기화

주요 위험: 넓어진 경계의 live/Frozen 봉합, loop 충돌, 효과 측정 공백.
