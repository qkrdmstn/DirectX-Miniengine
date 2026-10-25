상태: 완료 — `673b56b6` Static GPU 절단·접촉/대상 조회 수정; 자동 분리 이동 기본 해제·반복 절단의 부모 보존 검증 추가 (2026-10-25)

# Static GPU 절단 구현

## 현재 범위

같은 GameObject의 MeshComponent A를 자르고, A의 GeometryRevision을 바꾸며 B를 같은 owner에 추가한다. Original Model은 읽기 전용으로 공유한다. 생성 geometry에는 node·skin·source-frame 변환을 다시 적용하지 않는다. Skeletal 절단과 실제 물리는 범위 밖이다. 기존 Xbot 애니메이션·렌더는 유지한다.

입력은 현재 revision의 packed surface VB·IB 및 같은 프레임의 PosedGeometry다. 출력은 생성 surface/depth VB·32-bit IB, canonical RAW40 pose, 재질 참조, parent revision 기준 provenance, AABB다. GPU 생성 위치는 component-local이고 VS는 Wc·VP만 적용한다. 재절단은 생성된 cap을 포함한 최신 revision을 읽는다.

## 파일과 역할

| 파일 | 역할 |
| --- | --- |
| `Model/GeometryBuffers.*`, `GeometryRevision.h` | 생성 GPU 자원·불변 section·원본/parent 수명·독립 cap 재질 |
| `Model/PosedGeometry.*` | 생성 canonical pose를 컴포넌트 cache로 GPU 복사, 변환 중복 방지 |
| `ModelViewer/Scene/MeshComponent.*` | 원본/생성 렌더 및 편집 상태 generation |
| `ModelViewer/Scene/CuttingSystem.*` | 대상 snapshot, 비동기 완료 수집, 입력 변경 검사, A/B 게시 |
| `MeshCutting/GPU/CutTypes.h`, `GpuMeshCutter.*` | GPU 입력 descriptor·CutJob·PSO·dispatch·fence·metadata·timestamp |
| `MeshCutting/Shaders/ClipTrianglesCS.hlsl` | section AABB 한쪽 분류, 삼각형별 A/B clip·속성 보간·선분 |
| `BoundaryKeysCS.hlsl`, `LinkBoundaryCS.hlsl` | 경계 끝점 해시·9셀 거리 매칭·연결·복잡 경계 판정 |
| `RankBoundaryCS.hlsl` | pointer jumping으로 loop 대표와 경계 상태 전파 (선형 rank 배열은 만들지 않음) |
| `CapEarsCS.hlsl` | 한 loop 내부의 귀 검사·안전한 다중 제거; 작은 loop의 그룹 공유 메모리 cache |
| `FinalizeCutCS.hlsl`, `CutBoundsCS.hlsl` | 성공한 cap 양면 출력, 그룹 축소 AABB |
| `MeshCutting/tests/GpuCutValidation.cpp` | 명시적 결과 readback 검사, 정상·벤치 경로에서는 비활성 |

새 C++·HLSL은 해당 vcxproj와 filters에 등록했다. `GpuMeshCutter::Prepare`와 생성 표면/cap의 `Renderer::GetPSO`는 Startup에서 미리 실행해 첫 게시 중 Native PSO 생성 지연을 피한다. FXC가 아닌 기존 DXC 6.2 빌드 설정을 사용한다.

## GPU 순서와 병렬화

1. `GpuMeshCutter::Submit`이 current section/draw 범위를 펼치고 출력 최대량을 계산한다. section-local 구의 보수적 AABB가 평면 한쪽이면 해당 section은 교차 계산 없이 그쪽으로 보낸다.
2. `ClipTrianglesCS`는 삼각형마다 A/B 표면과 enter→exit 방향의 교차 선분을 만든다. 표면의 UV·normal·tangent·재질을 유지한다. 원본 tangent/UV1 PSO 조합을 유지하고 UV0가 없으면 0인 UV0를 합성해 공통 생성 renderer 계약을 맞춘다. UV1-only 입력 offset도 구별한다. 공유 edge는 위치 사전순으로 산술 방향을 맞춰 반대 방향 계산의 오차를 줄인다.
3. 평면 위 모서리는 양/음 표면의 후보를 중점 해시에 넣고 두 끝점의 실제 거리로 같은 모서리인지 비교한다. 양쪽 비공면 표면이 하나씩 붙은 모서리만 양의 표면 방향의 경계 하나로 유지한다. 같은 쪽 접촉·중복·비다양체 후보는 경계에서 제외한다. 해시를 초기화한 뒤 활성 경계 끝점을 주변 9셀과 실제 거리로 연결한다. 렌더 UV seam 정점을 합치거나 원본 전체 연결 표를 만들지 않는다.
4. 앞뒤 연결 자체가 경계 순서다. 별도의 선형 정렬 배열 없이 double-buffer pointer jumping으로 대표·열린 상태를 전파한다.
5. loop마다 한 threadgroup을 배정한다. 경계 안에서 convex/reflex 판정·귀 내부 검사·서로 세 점을 공유하지 않는 제거 집합·출력을 병렬 수행한다. 매 라운드의 읽기/쓰기 단계 사이에 barrier가 있다. ID 순서가 단조로울 때 하나씩만 제거되는 현상을 줄이기 위해 라운드별 우선순위를 해시한다.
6. 확실한 단순 loop는 reflex/collinear 점만 귀 내부 검사에 사용한다. 허용오차 안의 근접 접촉은 실제 자기 교차로 단정하지 않고 모든 점을 검사하는 보수적 경로를 쓴다. 확실한 crossing·잘못된 endpoint 연결은 그 cap만 생략한다.
7. 성공 표시가 된 loop의 cap topology를 한 번 사용해 A/B에 반대 winding·법선·handedness로 출력한다. cap은 독립 주황색 재질과 평면 UV를 사용한다.
8. 결과 위치 AABB는 그룹별 축소 후 합산한다. 결과 descriptor는 section별 실제 count와 side 전체의 보수적 bounds를 사용한다.

2018년 논문의 한 다각형 안의 ear 병렬 제거 전략에서 착안한 자체 HLSL 구현이며 논문의 CPU/OpenMP 코드를 그대로 GPU로 옮긴 것이 아니다. 2013년 CUDA 자료는 단일 polygon 순차 기준 경로의 참고다. 원래 제안한 BitonicSort 대신 경계 전용 GPU hash를 사용한다.

## 열린 버니와 유한 평면

`SliceProceduralMesh` 계열의 best-effort cap 정책을 따른다. 열린 chain·퇴화·삼각분할 실패는 cap만 생략하고 A/B 표면을 유지한다. `UncappedSegments`는 chain 수가 아닌 cap을 만들지 못한 경계 선분 수다. 닫힌 독립 loop는 각각 채우고 outer/hole hierarchy는 만들지 않는다. 따라서 속 빈 고리의 구멍까지 독립 cap으로 덮을 수 있다.

UE 5.3.2 [GitHub 공개 미러의 Epic 소스](https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Plugins/Runtime/ProceduralMeshComponent/Source/ProceduralMeshComponent/Private/KismetProceduralMeshLibrary.cpp)를 직접 확인했다. 최신 Epic 비공개 저장소를 확인한 것은 아니다. section AABB→삼각형 분할→교차선 cap의 순서를 참고하지만, 접촉·단면만 있는 조각의 게시 방지는 우리 GPU 경로에 추가한 검사다. 세부 비교는 [Unreal 소스 조사 기록](../Unreal/UnrealSourceComparison.md)의 현행 GPU 절을 따른다.

유한 사각형의 범위를 벗어난 교차 선분이 있으면 전체 후보를 게시하지 않고 원래 revision을 유지한다. 사각형 내부만 부분 절개해 별도 seam을 연결하는 기능은 없다. 열린 경계의 cap 생략과 유한 범위 초과의 전체 거절은 다른 정책이다.

## 수명·게시·제한

`CutJob`이 입력 revision과 GPU 후보·scratch·query·readback을 fence 완료까지 보관한다. 컴포넌트 pose 자원의 수명은 현재 컴포넌트 삭제/교체 시 GPU 대기로 보장한다. GPU 자원 지연 해제 큐는 아직 없으며 전역 대기 비용이 남는다.

`CuttingSystem::Update`는 ObjectId+MeshId, revision, component state version, Wc가 요청 시점과 같은지 검사한다. B를 먼저 준비하고 A 교체가 실패하면 B를 제거한다. 정상 경로는 전체 geometry를 CPU로 가져오지 않고 count/status/bounds 및 timestamp만 비동기로 읽는다. 현재 physics collider는 설명·참조만 있고 shape/cooking은 구현하지 않는다. 기존 collider revision은 geometry 변경 후 stale 판정을 받는다.

출력은 triangle-corner 방식이며 정점을 강제 용접하지 않는다. 정점 provenance는 parent revision의 현재 section/draw/vertex endpoint·보간 계수·현재 triangle을 기록한다. cap은 생성 cap marker와 loop 정보를 기록한다. Static 위치는 canonical geometry가 authoritative하며 이 출처를 skeletal skin 보간식으로 쓰지는 않는다.

출력 vertex capacity는 side당 최대 1,000,000개, retained parent와 양쪽 출력 예상량은 branch당 512 MiB, generation은 32로 제한한다. 이 수치는 Scene 전체 메모리 예산이 아니다. 교차/평면 위 모서리 후보 4096개 초과 시 cap을 생략하며 표면은 유지한다. 기본 병렬 ear의 라운드는 최대 256회이고, 완료되지 못한 loop는 cap을 버린다. 작은 공유 메모리 cache는 전체 후보 1024개까지다. 로그의 segments는 비활성 접촉·음의 모서리 지지 후보도 포함하고 uncappedSegments는 활성 경계 중 cap을 만들지 못한 수다.

surface overflow·잘못된 index·좌표 hash 범위 초과는 작업 전체 오류다. cap 실패는 부분 출력된 cap triangle까지 모두 버리며 surface-only 결과로 게시한다. 결과는 새 GPU 버퍼이므로 원본 공유 자산은 바뀌지 않는다.

## 조작과 진단

- 평면을 편집한 뒤 `C`를 누르면 현재 유한 평면의 보수적 범위와 겹치는 Static MeshComponent를 찾는다. 기억된 클릭 대상은 수동 절단에 사용하지 않는다. 후보마다 section 분류·GPU 교차 검사 후 실제 분할만 게시한다. 후보 목록과 평면은 입력 순간에 고정하며, Scene.Update 뒤 하나씩 제출한다. 생성된 B는 같은 요청의 후보 목록에 추가하지 않는다.
- 대기열은 ID·revision·stateVersion·Wc를 보관하고 제출 직전 및 완료 시 변경·삭제를 검사한다. 작업 중 다시 누른 입력은 새 작업을 만들지 않고 Busy 안내를 표시한다. 평면을 편집해도 이미 수락한 요청은 입력 순간의 평면으로 진행한다. 후보 없음·접촉/비교차·평면 크기 부족·입력 변경·완료는 HUD와 로그로 구별한다.
- 기본 절단은 A/B Transform을 모두 유지한다. `-cut-gap 0.6`을 명시하면 과거처럼 새 B만 월드 법선 반대 방향으로 0.6 이동시킨다. 이것은 렌더 전용 offset이 아닌 실제 컴포넌트 이동이며 이후 절단 후보/평면 변환에도 반영된다. Attached 로그의 delta는 owner-local, gap은 world 단위다. 기존 기즈모/Undo UI는 GameObject 전체 TRS를 편집한다. 절단 Undo·컴포넌트 편집 모드는 추가하지 않았다.
- `-model Models/cube.glb -cut 1` / `-model Models/bunny.glb -cut 2`: 자동 절단, 두 번째는 직교 평면으로 A를 재절단한다.
- `-cut 3 -cut-repeat-plane 1 -cut-expect-publications 1`: 같은 평면 반복 시 첫 분할만 게시되는지 검증한다. `-cut-y -1 -cut-expect-publications 0`은 큐브 접촉, `-cut-y -2`는 비교차다.
- `-cut-angle 45`: 모델 Y 평면을 Z축 45도로 돌려 큐브의 기존 모서리를 지나는 절단을 검증한다. `-cut 2 -cut-other 1 -cut-repeat-plane 1 -cut-step-y -0.5`는 B 재절단이다.
- `-cut-scene 1`은 자동 평면으로 수동 C와 같은 전체 후보 조회 경로를 검증한다. 전체 후보 요청과 단일 입력 benchmark는 서로 다른 경로이므로 함께 사용하지 않는다.
- `-cut-transform-checks 1`: owner 회전·비균일 scale과 component source-frame 회전·비균일 scale의 합성 검증이다.
- `-cut-width 0.25`: 유한 영역 초과를 거절하고 원본을 유지하는 진단에 사용한다.
- `-cut-y -240`: 자동 절단 평면의 모델 기준 높이. 자동 평면은 모델 공간 1000×1000이고 기본 y=0이다.
- `-cut-validate 1 -cut-expect-loops 1`: 실제 GPU Data/Pose/Provenance를 읽어 count·index·유한 값·법선·출처 범위·평면·cap winding/면적 및 최소 loop 수를 검사한다. 일반 실행에서는 이 전체 readback을 하지 않는다.
- `-cap-mode 0|1|2`: 병렬 다중 귀 / GPU 순차 후보 검사·단일 제거 / 후보 병렬·단일 제거의 비교 경로다. 기본은 0이다.
- `-cut-benchmark 20`: 3회 warmup 후 20회 같은 원본을 자르고 마지막 결과만 게시한다. `-debug 0` Release 실행으로 성능을 측정하고 검증 실행은 별도로 한다.

캡처는 자동 절단/벤치가 끝날 때까지 기다린다. `-capture <외부 prefix> -capture-frame 30 -animation-step-ms 16`을 기존 GPU 캡처 도구와 함께 사용할 수 있다.

## 검증·성능 기록

### 반복 절단과 자동 분리 이동 확인

이전 기본 경로는 새 B를 매 절단마다 월드 0.6 이동시켰다. A는 제자리이고 B의 자식은 이미 이동한 위치를 물려받는다. 법선이 바뀌면 다른 방향의 이동이 섞여 조각이 겹치거나 원래 윤곽에서 멀어 보일 수 있다. 단순히 화면만 벌리는 효과가 아니라 다음 절단도 이동한 Wc를 기준으로 진행한다. 기본을 `-cut-gap 0`으로 바꾸고, 기존 동작은 명시적 진단 옵션으로 남겼다. 위치를 유지하면 물리 없는 상태에서는 잘린 조각들이 같은 자리에 붙어 보인다.

기존 검증은 output range/plane/cap 면적을 검사하고 signed volume은 출력만 했으므로, 이 조사에서 `-cut-validate 1`의 부모 검사를 추가했다. GPU 작업과 같은 queue에서 당시 입력 Pose/Data를 readback에 고정해 이후 pose 갱신과 섞이지 않게 한다. 진단을 끄면 이 추가 snapshot/readback은 없다.

- 각 출력 surface의 provenance endpoint가 실제 부모 삼각형 인덱스에 속하는지 검사한다.
- 부모 pose의 두 endpoint와 보간값으로 위치를 재구성해 출력 위치와 비교한다. 기존 cap을 다시 자를 때도 현재 parent를 사용한다.
- 입력 삼각형마다 A/B surface의 면적 합이 원래 면적을 보존하는지 검사한다. 새 cap은 이 면적 합에서 제외한다.
- 전체 방향 있는 삼각형 적분의 부모 값과 A+B 합을 double로 비교한다. 열린 Bunny에서는 물리적 부피나 수밀성 증명이라고 해석하지 않는다.

Debug 이동0/0.6 비교 8회: Cube A 평행4회, Cube B 연속3회, Cube 직교2회, Bunny 직교2회를 각각 두 간격으로 실행했다. 모든 단계의 부모 보간/삼각형 출처/면적/방향 적분·기존 cap 검증과 게시 수를 통과했다. Cube A 체인은 로컬 signed integral이 A=4/3/2/1, B=4/1/1/1로 두 이동 설정에서 같았다. Cube 직교의 두 번째는 A/B 각각 약2였으며 두 설정에서 같았다. Bunny도 보존 검사와 cap 면적이 허용오차 내 일치했다. GPU ear 선택/삼각분할 순서가 달라질 수 있으므로 비트 단위로 같은 VB/IB라고 주장하지 않는다.

Release에서는 Bunny 22.5/112.5도 교대3회(+모델 y step25)를 이동0/0.6 각각 검사하고, Cube 회전·비균일 scale 2회도 검사했다. 모두 부모 보존 검증을 통과했다. Bunny의 후속 삼각형 수가 실행 간 일부 달라도 cap 면적과 부모 표면 분할이 보존됐다. 새 D3D12 ERROR/assert는 없었고 모델 캐시는 백업과 일치했다. Debug/Release 빌드 로그는 `%TEMP%/static-cut-gap-{debug,release}.log`, 실행 로그·HDR/post·비교 PNG는 `%TEMP%/static-cut-gap-validation/{debug,release}/`에 있다. 이미지는 절단 입력을 자동 재생한 GPU 캡처이며 수동 마우스 입력 검증은 아니다.

코드 리뷰와 비교 PNG에서도 gap0은 원래 윤곽을 유지하고 gap0.6은 별도 위치의 조각/단면을 드러냄을 확인했다. 재현한 범위에서는 자동 이동이 외관 차이를 설명하며, 조각 표면 누락/중복을 나타내는 보존 검사 실패는 발견하지 못했다. UV/normal/tangent 보간 전체나 임의 malformed 메시의 완전한 Boolean/수밀성을 증명한 것은 아니다. 후속 물리 도입은 [PhysicsIntegrationPlan.md](../Physics/PhysicsIntegrationPlan.md)의 설계안을 참고한다.

이동 delta의 좌표 공간을 로그에 명시한 최종 Debug/Release 재빌드도 exit0이며 `%TEMP%/static-cut-gap-final-{debug,release}.log`에 남겼다. SceneMathTests 457개·`git diff --check`를 다시 통과했다. 일반 실행의 GPU 알고리즘·버퍼 형식은 이 조사에서 바꾸지 않았고, 기본 분리 이동과 opt-in 진단을 변경했다.

### 접촉·반복 단면 회귀 수정

원인은 평면 위 모서리를 무조건 경계로 만들고, 새 cap까지 포함한 section이 있으면 양쪽 결과로 인정하던 점이었다. 같은 평면에서 반복하면 기존 cap은 A에 남고 B에는 새 cap만 있는 조각을 계속 게시할 수 있었다. 수동 입력은 startup/과거 클릭의 단일 MeshId만 사용해 현재 평면이 만나는 조각을 놓칠 수도 있었다.

`ClipTrianglesCS`는 Meta byte20의 비트 0/1에 실제 비퇴화·비공면 표면 출력 여부를 기록한다. `BoundaryKeysCS`는 byte20의 비트2에 활성 교차 경계 존재를 기록하며, 평면 위 모서리는 양/음 지지가 하나씩 있어야 활성화한다. Ring byte92는 후보 kind, byte108은 활성 여부이며 rank ping-pong byte96/100/104 및 ear 선택 byte88과 겹치지 않는다. `HasSplit`은 오류 없음과 세 비트를 모두 요구한다. `BuildResults`는 마지막 새 cap section을 제외한 양쪽 surface count도 검사한다. 기존 cap을 무조건 제외하는 것이 아니라 새 평면과 공면인 입력을 분할의 증거에서 제외한다.

`FinalizeCutCS`는 실제 분할에만 cap을 출력한다. 진단 경로도 비교차/접촉이면 양쪽 새 cap count=0, 실제 분할이면 양쪽 non-cap surface count>0을 확인한다. `-cut-expect-publications`는 최종 게시 수를 검사하며 GPU 작업이 없었던 후보 없음/제출 거절도 검사한다.

단계별 독립 리뷰: ① 비공면 표면 증명과 게시 gate, ② 평면 위 모서리 양측 지지·해시 재사용, ③ ID snapshot 큐·프레임 순서·대상 조회, ④ 비교차 진단과 최종 게시 수 assertion. 리뷰에서 clipped 출력 면적 검사 전 proof 설정, 공개 Request의 큐 재진입, GPU 완료 없는 요청의 assertion 누락을 발견해 수정했다.

NVIDIA GeForce MX450 / driver 32.0.15.9186 / Debug / `-debug 1 -cut-validate 1` 회귀 실행:

| 케이스 | 결과 |
| --- | --- |
| Cube y=-1 접촉 / y=-2 비교차 | 게시 0, cap loop 0 |
| Cube y=0 정상 | 게시 1, 양쪽 triangles18, cap 면적 약5.3333304 |
| Cube 동일 평면 3회 | 게시 1/0/0, 후속 cap loop 0 |
| Cube 45도 기존 모서리 통과 | 게시 1, 양쪽 triangles10, cap 면적 약7.5424687 |
| Cube B를 y=-0.5에서 재절단 | 두 작업 모두 게시, B 재절단 cap 면적 약5.3333307 |
| Bunny y=0 | 게시 1, loop1, cap 면적 약2.4467710 |
| Bunny y=-240 열린 메시 | 게시 1, loop1, uncapped207, cap 면적 약2.2573260 |
| Cube 회전·비균일 scale/source-frame | 게시 1, cap 면적 약2.9110317 |
| Cube width=0.25 | error1, 게시 0, 원본 유지 |
| 기본 장면 전체 후보 조회 | Cube/Bunny 각각 게시·양쪽 GPU 검증 통과, Skeletal 제외 |

각 작업의 Data/Pose/Provenance·단면 면적 검사를 통과했고 새 D3D12 ERROR/assert는 관찰되지 않았다. 실행 로그·GPU 캡처는 `%TEMP%/static-cut-bug-analysis/debug-suite2/`에 있다. 실제 마우스/C 키를 누른 검증은 아니며 C와 같은 `RequestAll`을 자동 실행으로 검증했다. 이전 CPU/GPU 성능 표는 이 회귀 수정 이전 측정이다.

최종 assertion 실행은 Debug의 접촉(게시0), 동일 평면 3회(게시1), 기본 장면 전체 후보(게시2), 전체 장면 동일 평면 재입력(총게시2), Release의 모서리 통과(게시1), 열린 버니(게시1)를 통과했다. Release의 후보 없는 y=-100 전체 조회도 후보0/게시0 assertion과 캡처를 통과했다. 명시적 no-split 진단에서 새 cap count=0이 확인됐다. 최종 Debug/Release 빌드는 exit0이며 로그는 `%TEMP%/static-cut-fix-final2-{debug,release}.log`, 추가 실행은 `%TEMP%/static-cut-bug-analysis/{final-assertions,publication-check,last-check}/`에 있다. SceneMathTests 457개 및 `git diff --check`도 통과했다. GPU/드라이버는 위와 동일하고 모델 캐시 SHA256은 기존 백업과 일치했다. 캡처 도구는 완료 marker 뒤 자신이 실행한 프로세스를 종료하므로 정상 종료 검증을 뜻하지 않는다.

각 단계 독립 리뷰에서 generated skin/CBV 검증, 2D dispatch 범위, scratch UAV 상태, 원본 ID 순서 병목, 근접 접촉의 과도한 crossing 판정을 보완했다. 최종 수치와 검증 기록은 아래에 추가한다.


### 이전 CPU 구현의 재현

현재 작업 트리를 checkout/reset하지 않고 CPU 초기화 직전 commit `2b4871f5d63cb369bfd51f340dce239b507e31be`를 `%TEMP%/cpu-slice-history-bench/snapshot`에 git archive로 추출했다. GLB blob hash는 현재 cube/bunny와 일치했다. VS2022 MSVC 19.44.35229 x64 `/O2 /DNDEBUG`로 독립 하네스를 빌드했다. source 변경은 snapshot에만 추가한 benchmark용 wrapper뿐이며 `SliceGeometryImpl(..., inputValidated=true)`를 호출해 반복 입력 검증만 생략한다.

CPU 입력은 GLB POSITION/index를 node transform으로 bake한 메시다. y=0 평면, 절대 epsilon 1e-5를 준비 시 한 번 검증한다. cube는 8 vertex/12 triangle, bunny는 91,014 render corner→15,258 용접 geometry vertex/30,338 triangle이다. CPU normals=(0,1,0), UV=(0,0), color=(1,1,1,1)를 사용한다. 그러므로 실제 MiniEngine 런타임 전체나 원본 재질 attribute까지 동일한 benchmark라고 주장하지 않는다.

주 CPU 구간은 준비된 입력의 clipping+cap+출력 할당+출력 validation이며 입력 변환/weld와 반복 입력 validation은 제외한다. GPU 구간은 준비된 pose부터 scratch clear+clip+경계+cap+bounds 계산까지의 timestamp다. GPU에는 일반 경로의 geometry 전체 CPU readback/검증이 없으며 diagnostic 검사는 별도 실행했다. 출력 validation 비용과 vertex layout/triangle iteration order가 다르므로 비교는 참고값이고 전체 게임 프레임 속도비로 해석하지 않는다.

CPU 측정은 3회 warm-up 뒤 20 sample, 모델마다 독립 process 2회다. 최종 측정 중 GPU app/동시 build를 종료했다. Intel Core i5-1135G7(8 logical), Windows 10.0.19045에서 측정했다.

| CPU 준비 완료 입력, ms | 중앙값 run1 / run2 | P95 run1 / run2 |
| --- | --- | --- |
| Cube slice+cap+allocation+output validation | 0.1423 / 0.0754 | 0.2378 / 0.1006 |
| Bunny slice+cap+allocation+output validation | 88.5693 / 94.5612 | 109.9821 / 122.4257 |
| Bunny 입력 validation까지 포함 | 94.5246 / 102.6666 | 117.7608 / 123.3952 |
| Bunny Build+weld+prevalidate+slice 전체 (10 sample) | 141.3835 / 145.0804 | 155.6972 / 179.9843 |

CPU 전체 범위/로그·입력 manifest·임시 하네스는 `%TEMP%/cpu-slice-history-bench/release-final/`과 그 상위 폴더에 있다. 과거 public wrapper의 기본 radius-relative epsilon은 위 exact-epsilon 표와 달라 같은 표에 혼합하지 않았다.


### 최종 GPU 실행 결과

NVIDIA GeForce MX450, driver `32.0.15.9186`, 1920×1080. 성능은 Release `-debug 0`, 검증은 별도 `-debug 1` 실행이다. GPU 전체 작업 timer는 scratch clear부터 표면 절단·경계·cap·bounds까지이며 source pose 준비·GPU 자원 생성·CPU polling·컴포넌트 게시·렌더는 제외한다. `ReadyMs`는 요청의 자원 생성부터 GPU metadata 읽기 가능까지를 CPU clock으로 재며, 출력 geometry 게시 시간과 구별한다.

| GPU 최종 20 sample, 3 warm-up, ms | GPU 중앙값 | GPU P95 | CPU clock Ready 중앙값 |
| --- | --- | --- | --- |
| Cube (12 triangles, boundary 8) | 0.100192 | 0.700416 | 12.034 |
| Bunny (30,338 triangles, boundary 370) | 5.627808 | 6.781920 | 28.428 |

동일 계산 코드를 사용한 그 전 두 독립 20-sample run의 중앙값은 Cube 0.099264/0.101248ms, Bunny 5.786656/5.929952ms다. 큐브는 GPU clock/dispatch 비용에 비해 일이 작아 편차가 크고 CPU 대비 명확한 이득을 보장하지 않는다. 버니는 GPU 계산 구간이 CPU geometry 구간보다 짧았지만, 앞서 설명한 출력 검증·attribute·입력 위상 차이 때문에 전체 게임 프레임 속도비로 단정하지 않는다.

Native PSO 준비를 Startup으로 옮긴 마지막 적용 확인은 3 warm-up+5 sample이다. GPU 중앙값/P95는 Cube 0.680096/0.699328ms, Bunny 6.554080/6.628832ms였다. 짧은 표본에서 큐브의 GPU clock 변동을 확인했다. 마지막 실제 컴포넌트 게시까지 CPU elapsed는 Cube 11.487ms, Bunny 45.465ms였다. 이는 **마지막 적용 1회**의 값이며 게시 시간의 중앙값을 측정한 것은 아니다. 초기 버전의 첫 게시에서 보이던 0.67~1.56초 Native PSO 생성 지연은 시작 준비로 옮겼다. 로딩 비용이 사라진 것이 아니라 게임플레이 전에 실행된다.

모든 benchmark iteration은 loops=1, uncapped=0, error=0이고 마지막에 A/B를 게시해 실제 HDR/post 캡처까지 확인했다. 원본 glb와 `.mini` 사용자 변경은 보존됐고 실행 전후 Xbot/bunny/cube `.mini` SHA256은 백업과 일치했다.

### 검증 완료 목록

- Debug/Release 최종 빌드 exit 0; SceneMathTests 457개 통과; `git diff --check` 통과.
- 단계별 독립 리뷰와 최종 데이터 흐름 리뷰 완료. 생성 skin/CBV0/layout 검증, 큰 dispatch, ClearUAV 전 상태 전이, 근접 경계와 실제 교차 구별, UV0 없는 입력의 dummy UV 합성·tangent/UV1 보존, 결과 factory 예외 시 원본 보존을 수정했다.
- Cube/Bunny 첫 절단과 A 재절단을 실행했다. 큐브 재절단은 loop1, 버니 재절단은 loop2였다. 두 결과의 actual GPU Data/Pose/Provenance readback 검사에서 count/capacity/index, finite position/unit normal, packed/posed 일치, parent section/draw/vertex/t 범위, plane side, cap winding·면적이 통과했다.
- 버니 중앙: 경계370, 닫힌 loop1, uncapped0. 각 cap의 월드 투영 면적 2.446770986이 원래 경계 면적과 일치했다. 재절단 cap 면적 0.355953314/0.355953315도 일치했다.
- 버니 y=-240: 경계606, cap loop1, uncapped207, 표면 양쪽 유지, cap 면적 2.257325956 일치. y=-230의 cap loop2 및 열린 선분49도 확인했다. 원본의 다른 구멍을 자동으로 메우지는 않는다.
- 회전·비균일 owner scale과 source-frame 회전·비균일 scale을 합성한 Cube는 양면 GPU 검증 및 cap 면적 약 2.9110317 일치, 실제 결과 게시·캡처를 통과했다.
- 유한 plane width=0.25는 error1/other0/published0, 원본 유지·캡처를 확인했다.
- 단일 귀 GPU 기준 mode1/2는 Cube에서 면적·winding이 일치했다. Bunny mode2도 mode0과 같은 cap 면적을 얻었으며 이 Debug 실행에서 후보 병렬·단일 제거 cap 320.640ms, 다중 제거 cap 47.506ms였다. 이 값은 Release CPU 비교 표와 혼합하지 않는다. 큰 Bunny의 mode1 직렬 진단은 watchdog 위험을 피하려 실행하지 않았다.
- Cube 절단 Wireframe/Vertices, 기본 Xbot 장면의 Wireframe/Vertices 실행·캡처를 확인했다. `-debug 1`에서 새 D3D12 ERROR/CORRUPTION·device loss·assert가 없었다. 기존 DXGI #80과 D3D12 warning #974/#1008은 남아 있다.
- 검증 중 발견된 최초 scratch 상태 오류 #527과 근접 경계 오분류, UV0 게시 예외는 최종 빌드에서 재현되지 않았다. 실패 로그도 비교용으로 보존했다.
- 캡처 helper는 GPU 완료/캡처 marker 뒤 자신이 실행한 프로세스를 종료했다. 실제 C 키·마우스 조작 및 정상 종료의 자동 검증을 주장하지 않는다. 프레임 경계의 lifetime/게시 순서는 코드 리뷰로 확인했다.

GPU 로그·RAW·PNG는 `%TEMP%/static-cut-validation/`의 `boundary-fix`, `release-benchmark`, `final-check`, `prewarm-final`에 있다. 빌드 로그는 `%TEMP%/static-cut-final-{debug,release}-build.log`다. CPU archive/하네스/결과는 `%TEMP%/cpu-slice-history-bench/`다. 테스트 자료와 과거 CPU source snapshot은 저장소 외부에 유지했다.

### 남은 범위

Skeletal joint patch·지속 애니메이션 절단, 실제 collider/body, cut Undo, 컴포넌트별 gizmo UI, solid/hole 계층 해석은 구현하지 않았다. 매우 얕은 실제 자기 교차나 공선 중첩 등 malformed polygon을 epsilon 근접으로 해석할 수 있으므로 CAD 수준의 exact Boolean kernel을 보장하지 않는다. GPU 자원 pooling·지연 해제·더 타이트한 section AABB와 장면 전체 메모리 예산은 후속 최적화다.
