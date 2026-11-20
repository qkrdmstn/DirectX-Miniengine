상태: 진행 중 — CP-1 측정, CP-2 7367a00a, CP-3 415fa819, CP-4a ad5c74cd, CP-4b (커밋 대기)

# 절단 성능 개선 결정 (1차)

## 1. 목표

- 2026-11-18 발표 피드백 두 가지에 답한다.
  - 피드백 1: IB를 직접 마스킹할 때의 오버헤드.
  - 피드백 2: thread가 삼각형 하나만 맡는 구조.
- PerfBaseline에서 확인된 큰 비용을 줄인다. 같은 조건으로 다시 재서 개선 전후를 기록한다.

## 2. 근거 (PerfBaseline, MX450 Release)

| 항목 | Xbot | 비고 |
|---|---:|---|
| `TriangleMaskCS` 마스크 생성 | 770 µs | 추출 전체(350 µs)의 2배 |
| ReplacedIB 상주 메모리 | 1.38 MB | 실제 IB 합 0.29 MB. section 사이 VB 간격까지 덮는다 |
| 렌더: 숨긴 삼각형 | IA·clipper 100%, VS 약 2/3 잔존 | GPU 시간 차이는 사이클 잡음 범위 |
| 추출 단계 사이 gap | 199 µs (57%) | Mode 합은 151 µs |
| CPU `SkeletalPatchCopy::Update` (Poll 뒤) | 약 4.9 s | debug-output 수집기 포함. 원인 미확정 |

정정: 현행 Mode 0에는 정점 중복 판정이 없다.
- 선택 삼각형마다 정점 3개를 복제한다. Mode 2는 identity IB(R32)를 쓴다.
- 그래서 "정점 단위 추출"의 이득은 중복 판정 제거가 아니다. 이득은 추출 출력 메모리(Xbot Data 7.07 MB, 입력 전체 T에 대해 3·T·48 B 할당)와 복사량 감소다.

## 3. 결정과 이유

### D1. 원본 렌더는 압축 IB로 바꾸고, 압축은 CPU가 만든다
- `TriangleMask::Pending`에 이미 CPU State가 있다. draw별 남은 인덱스 수도 CPU가 알아야 `DrawIndexed`에 넘길 수 있다. 그래서 GPU compaction과 readback 대신 CPU에서 만든다.
- 원본 인덱스는 이미 CPU에 있는 것을 재사용한다.
  - skin section: `BuildInfluencingJoints`가 보존한 `m_SkinCpuSections[].Indices`를 쓴다. 이 값에는 baseVertex가 이미 더해져 있다.
  - 압축 IB는 baseVertex를 뺀 draw 상대값으로 쓰고, `DrawIndexed`의 baseVertex는 원본 그대로 둔다.
  - 비skin section: 지금 마스크 대상이 skinned 원본뿐이라 범위 밖이다. 비skin section에 마스크가 요청되면 거절 로그를 남긴다.
- 압축 IB는 R32, 컴포넌트 소유다. 같은 Model을 쓰는 다른 인스턴스와 공유 `Mesh.draw`는 바꾸지 않는다.
- 렌더 override는 **section별 IB view(주소·크기·형식) + draw별 범위(StartIndex, IndexCount)**다.
  - 이것을 `MeshSorter::AddMesh`에 넘긴다.
  - `RenderMeshes`의 공통 루프(shadow·depth·opaque·transparent)가 원본 `ibOffset/ibSize/ibFormat`과 `mesh.draw` 대신 이 값을 쓴다.
- 경로는 두 갈래로 나눠 처리한다.
  - **설정:** `SetTriangleMask`. 일반 `Update`, `ConsumeMultiBreak`, `PublishRigBonePatch`가 여기를 지난다. 이때 압축 IB와 override를 만든다.
  - **해제:** `ClearTriangleMask`. `SetGeometry`, `CommitGeometryReplacement`가 여기를 지난다. 이때 압축 IB와 override를 없애고 원본 view로 돌아간다. GPU 사용 중 자원의 수명은 지금 ReplacedIB와 같은 방식으로 지킨다.
- `TriangleMaskCS`와 `ReplacedIB`는 제거한다.
  - `tests/TriangleMaskShaderTests`는 같은 단계에서 삭제하거나 CPU 압축 함수 테스트로 바꾼다.
  - GPU State 버퍼는 추출 입력(제거 비트)으로 계속 쓰므로 유지한다.
- 이유: 매 프레임 낭비(IA, clipper, VS), 마스크 생성 770 µs, 1.38 MB를 한 번에 없앤다.
- 택하지 않은 안:
  - 추출 Pass에서 Keep IB를 GPU로 만드는 안: 게시 전 대기 결과 관리와 R16 scatter 경쟁이 생긴다.
  - 정점 단위 `SV_CullDistance` 마스크: VS 낭비가 오히려 늘고 경계가 근사가 된다.

### D2. CPU `Update` 구간은 하위 계측으로 원인을 먼저 확정한 뒤 고친다
- 조사 후보:
  - `SafeDiagnosticNodeName`/`DiagnosticNodeName`이 아일랜드마다 GLB를 다시 열어 JSON을 파싱한다(캐시 없음).
  - `PatchLog`가 호출마다 `OutputDebugString`과 파일 열기/닫기를 한다.
  - `Scene::CreateObject`, `SetGeometry`에서 PosedGeometry 할당이 중복된다.
  - `DiagnoseIslands`의 CPU skinning.
- 수집기를 붙인 실행과 붙이지 않은 실행 각각에서 함수별 시간과 로그 호출 수를 잰다.
- 수정은 측정으로 확인된 항목만 한다(예: 이름 캐시, 로그를 기본 실행에서 줄이기). `LabelPatchIslands`의 기능 결과는 바꾸지 않는다.

### D3. 추출 단계 합치기와 coarsening은 측정으로 고른다
- **Mode 7 합치기:** Mode 7을 없애고 Mode 2가 블록 offset을 직접 더한다.
  - Mode 2는 현재 삼각형과 이전 삼각형의 prefix를 비교한다. 두 prefix에 **각자 블록의 offset**을 더한다(t=64처럼 블록 경계에서는 서로 다르다). t=0은 이전 값을 0으로 따로 처리한다.
  - Scan 자체는 블록 내부 prefix로 남는다. Submit/Poll/Validate는 Scan을 readback하지 않으므로 영향이 없다.
- **coarsening:** 기반 단계와 반영 단계를 나눈다.
  - 기반 단계(CP-6a)는 블록 크기를 `64·K`로 일반화한다. 대상은 Mode 0/1/2와 Mode 6의 블록·lane 매핑, Scan 할당, Submit의 `blocks/groups` 계산이다. Influence Mode 3은 지금 처리 범위를 그대로 지킨다(별도 dispatch 크기).
  - K=1/2/4/8을 비교하고, Xbot과 CesiumMan 중 어느 쪽도 나빠지지 않는 K 중 가장 빠른 값을 고른다. 차이가 사이클 범위 안이면 K=1을 유지한다.
  - 반영 단계(CP-6b)는 고른 K만 상수로 남기고 실험 분기는 지운다.
- A→B처럼 전역 prefix가 필요한 경계는 dispatch를 나눠야 하므로 합치지 않는다.

### D4. 추출 출력의 정점 공유(정점 단위 복사)는 1차 범위에서 뺀다
- 바뀌는 소비자가 많다: `PatchSkinBuildCS::SourceAddress`(vertex/3, vertex%3), `Validate`, Poll 크기 계산, D5 출처, 그리고 JC-2b~d와 같은 지점.
- 측정된 것은 Mode 2 시간뿐이다. 첫 절단 35 µs, 재절단 두 번째 62 µs다. 할당·후속 소비자까지 포함한 순이득은 측정하지 않았다.
- 1차 결과를 재측정한 뒤, JC-2와 함께 다시 결정한다. 메모리 7 MB(임시)는 그 결정의 근거로 남긴다.

## 4. 단계와 예상 시간

| 단계 | 내용 | 줄 수(추정) | 시간 |
|---|---|---|---|
| CP-1 | D2 하위 계측(임시, 커밋 안 함). 수집기 유무 각 1회, 원인 보고 | 100~180 | 구현 15~25분 + Release 증분 약 1분 + 실행 수 분 |
| CP-2 | D2 수정. 확인된 원인만 | 측정 후 결정, 300 이내 | 구현 + 빌드 + 리뷰 2~5분 |
| CP-3 | D1 렌더 override 경로(section IB view + draw 범위)만 추가. 동작 불변 | 150~240 | 같음 |
| CP-4a | D1 CPU 압축 IB 생성·해제·렌더 적용, 압축 IB 기준 마스크 검사. 옛 경로는 비활성 보존 | 약 200 | Release 빌드 + HDR·마스크·회귀 검사 |
| CP-4b | `TriangleMaskCS`, `ReplacedIB`, `TriangleMaskShaderTests` 제거와 `SkeletalMaskChecks` 정리 | 300 이내 | Release 빌드 + 회귀 검사 |
| CP-5 | D3 Mode 7 합치기 | 100~150 | 같음 |
| CP-6a/b | D3 블록 크기 일반화와 K 비교 / 고른 K 반영 | 각 100~250 | 같음 + 측정 약 20분 |
| CP-7 | BL·RP와 같은 조건으로 재측정, PerfBaseline에 개선 후 절 추가, 상태 줄·README 동기화 | 문서 | 약 40분 |

- 단계마다: 새 Codex 구현 → Release 빌드(디스크 여유가 부족하면 Debug는 마지막 단계에서 한 번) → 검사 → 새 Codex 리뷰 → 로컬 커밋.
- `Geometry/`는 바꾸지 않으므로 PlaneSlicerTests는 필요 없다.

## 5. 완료 조건

1. D/R 빌드 오류 0, D3D12 ERROR 0(Debug 실행 1회 이상).
2. 압축 IB 적용 뒤:
   - `-skeletal-mask-checks 1` PASS. 기대 IB 비교는 "남은 삼각형 원본 인덱스 순서"로 갱신한다.
   - CPU 압축 함수 단위 검사: R16/R32 원본, 다중 draw, 0이 아닌 startIndex/baseVertex, 전체 제거, 제거 없음.
   - HDR 캡처가 현재(퇴화 IB)와 같다. 자동 노출을 고정하거나 HDR 버퍼로 비교한다.
   - 공유 Model의 다른 인스턴스는 바뀌지 않는다.
   - 연속 절단(D6), multi(`ConsumeMultiBreak`), D5, Clear·`SetGeometry` 뒤 원본 복원이 동작한다.
3. 렌더 재측정:
   - 전체 제거 fixture에서 해당 draw의 IA·VS가 0이다.
   - 부분 제거에서는 draw별 인덱스 수와 IAPrimitives가 남은 삼각형 수와 일치하고, 전체 VS 호출 감소를 기록한다.
   - 마스크 생성 GPU dispatch가 없다. CPU 압축 시간, 업로드 시간, 압축 IB 바이트를 기록한다.
4. Mode 7 합치기와 K 반영 뒤 추출 결과가 같다.
   - 같은 입력 snapshot으로 변경 전후의 유효 Data 영역(surface VB, depth VB, IB), TriangleSources, Meta를 바이트 비교한다. 기록되지 않는 용량 영역은 제외한다.
   - 블록 경계 꼬리를 검사한다(T%(64·K)≠0, 블록 경계 t).
   - K마다 `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1` 20요청 GPU==CPU, 데모 Meta 2118/47/0·271/16/0 유지.
5. CPU `Update` 구간: 원인 항목별 전후 시간을 기록한다. 수집기 없는 실행 기준으로 판정한다.
6. 기존 회귀 PASS: `-skeletal-patch-checks`, `-skeletal-patch-break-checks`(Parent·Rig), MI-4h2 다중, MI-5 CesiumMan, `SkeletalPatchTests`, `-skeletal-recut-repro 1`, Influence 요청의 선택 node.

## 6. 위험

- 압축 IB 업로드가 게시 프레임에 들어간다. Xbot 0.28 MB다. 업로드 시간을 잰다.
- R16 원본을 R32 압축 IB로 바꾸면 크기가 2배가 된다. R16을 유지하는 안은 CP-4에서 줄 수를 보고 결정한다.
- `m_SkinCpuSections`가 없는 경로(비skin, 로딩 방식 차이)가 마스크 대상이 되면 거절된다. 실제로 요청되는지 회귀에서 확인한다.

## 7. 결과

### CP-1·CP-2: D2 노드 이름 캐시 (base `f2c0501f`, 2026-11-20)

- Release/Windows(x64), NVIDIA GeForce MX450(1906 MB). CP-1과 같은 QPC 계측으로 Poll 성공 직후부터 `SkeletalPatchCopy::Update` 반환까지 U를 측정했다. 일반 실행은 debugger 없이, 수집기는 CP-1의 Win32 debugger 연결 방식이다. 두 조건을 별도로 비교한다.
- cwd `MiniEngine/ModelViewer`, `-debug 0 -skeletal-break-demo 1`(Xbot) 또는 `2`(CesiumMan), `-physics-query-exit 360`. 매번 새 프로세스의 첫 요청이며 최초 이름 표 생성도 U에 포함된다.

| 모델 | 조건 | CP-1 U ms (n=2 평균) | CP-2 U ms (실측) | CP-2 평균 ms |
|---|---|---:|---:|---:|
| Xbot | 일반 | 32.6031 | 15.9008 / 14.6531 | 15.2770 |
| Xbot | 수집기 | 5038.6802 | 1594.9574 (n=1) | 1594.9574 |
| CesiumMan | 일반 | 5.2438 | 3.8415 / 3.7654 | 3.8035 |
| CesiumMan | 수집기 | 75.0722 | 66.1721 (n=1) | 66.1721 |

- CP-1 상위 원인은 `DiagnosticNodeName`의 호출별 GLB 재읽기·JSON 파싱이었다. Xbot: 일반 29.0612 ms(U의 89.14%), 수집기 5004.9137 ms(99.33%); CesiumMan: 일반 2.0756 ms(39.58%), 수집기 68.8850 ms(91.76%). 수집기의 차이는 debugger 상호작용을 포함하며 출력 비용만으로 해석하지 않는다.
- CP-2 `DiagnosticNodeName`: GLB 경로별 최초 조회에서 기존 scene root·자식 전위 DFS 순서로 이름 표를 만든다. 같은 경로는 Model 해제·재생성과 무관하게 재사용하고 파일 크기·수정 시각·파일 ID·변경 시각 변경 또는 삭제 시 폐기한다. 메타데이터 조회 실패 시 이전 캐시를 재사용하지 않는다. Model 포인터 주소 재사용에 의존하지 않는다. 잘못된 헤더·읽기/파싱 실패는 재시도하며 노드별 이름 오류와 자식 순회 오류는 기존 조회 시점에 맞춰 반환·전파한다. `SafeDiagnosticNodeName`의 번호 대체, 로그 문자열과 다른 동작은 유지했다.
- 수정 후 이름 조회 합계: Xbot 일반 12.1835 / 11.3967 ms, 수집기 1555.6760 ms; CesiumMan 일반 1.6454 / 1.3971 ms, 수집기 61.6783 ms. 최초 파싱은 남으므로 U가 0에 가까워지는 변경은 아니다. 표본이 작아 일반화하지 않는다.
- 검증 자료는 `%TEMP%/CP2/`의 실행별 CSV·TEMP 패치 로그·수집 로그, `runs.json`, 빌드 로그와 이름 동등성 검사에 보관했다. CP-1 계측을 재적용해 측정한 뒤 제거했으며 계측 없는 Release도 다시 빌드했다(15.35초, 오류 0). Debug 빌드는 디스크 제약으로 생략했다.
- Xbot `-skeletal-break-demo 1` 수정 전후 TEMP 로그의 `MI2 island` 세 행 전체가 동일하다. 별도 임시 검사도 Xbot/CesiumMan NodeId 0~99 반복 조회와 DFS root/자식 순서·unnamed·이름/자식/JSON 오류·파일 교체/삭제/재시도의 기존 함수 결과 동등성 PASS. `SkeletalPatchTests` Release 131 checks PASS.
- 계측 제거 후 회귀: -skeletal-patch-checks 1 -skeletal-patch-offset-copy 1 20요청 GPU==CPU PASS; -skeletal-patch-break-checks 1 Parent/Rig 각각 6요청 GPU==CPU와 subtree/root/AlreadyBroken PASS, 경계 검사 pass=4/fail=0/n/a=2. 세 프로세스 모두 exit=0, timeout=False, 명시적 FAIL 0. Debug·D3D12 validation과 드라이버 버전은 확인하지 않았다.

- 리뷰 수정: 조상 방문 표시로 순환을 감지하고 반복 구간을 재사용해 기존 요청 NodeId의 이름을 보존한다. 공유 자식·중복 root는 기존대로 재순회한다. 모든 메타데이터가 같은 내용 변경은 진단 이름에 반영되지 않을 수 있다(전체 파일 해시 없음).
- 리뷰 수정 검증: Release 증분 빌드 16.81초, 오류 0. 두 데모 exit=0, 기존 CP-2 island 로그 Xbot 4행·CesiumMan 3행 완전 일치. offset-copy 20요청 GPU==CPU PASS, exit=0, timeout=False, 명시적 FAIL 0. 임시 합성 GLB로 자기/조상 순환·공유 자식·중복 root·순환 중 이름 오류의 NodeId 0~999와 큰 NodeId, 동일 크기·수정 시각의 덮어쓰기/교체 감지 PASS. 자료는 `%TEMP%/CP2/review/`에 보관했다. 위 성능 수치는 리뷰 수정 전 CP-2 측정이며 메타데이터 조회 추가 비용은 재측정하지 않았다. Debug·D3D12 validation은 이번 요청 범위에서 제외했고 드라이버 조회는 접근 거부로 확인하지 못했다.

### CP-3: D1 렌더 override 경로 (base `7367a00a`, commit `415fa819`, 2026-11-20)

- `MeshRenderOverride`는 section의 절대 IB view와 원본 draw 순서의 `StartIndex`·`IndexCount` 배열이다. `MeshSorter::AddMesh`의 선택 인자로 받아 정렬 목록이 `shared_ptr<const MeshRenderOverride>`로 수명을 보존한다. 호출자는 등록 뒤 내용을 바꾸지 않고 GPU 버퍼 자체는 명령 완료까지 보존한다.
- `MeshSorter::RenderMeshes`의 공통 루프에서 override가 있으면 기존 ReplacedIB 주소 대체보다 우선한다. 원본 `Mesh`·`Mesh.draw`, `baseVertex`와 posed SRV 오프셋은 유지한다. `MeshComponent::Render`는 override를 생성하지 않으며 기존 ReplacedIB 전달식을 유지한다.
- Release/Windows(x64), NVIDIA GeForce MX450(1906 MB). 첫 빌드는 125.79초, 오류 0(기존 경고 247개). Debug는 요청대로 빌드하지 않았다.
- 기존 `-debug 0 -skeletal-mask-checks 1`: 두 모델의 IB·공유 원본·peer·reset·restore 및 shadow/depth/HDR 검사 PASS, exit=0(16.57초). `-debug 0 -skeletal-break-demo 1 -physics-query-exit 360`: exit=0, timeout=False(23.45초).
- 변경 전후 `-debug 0 -capture <TEMP 경로> -capture-frame 60 -animation-step-ms 16 -physics-query-exit 100`의 HDR RAW(8,294,416바이트)가 동일하다. SHA-256: `CBD11B9AA57C4A656AC7442E2692B07B20A875A4DE352D9B0FF7B08EEA366ABA`.
- 임시 코드로 한 컴포넌트에 동일 값의 override를 전달하고 Xbot·CesiumMan 각각 원본 IB 및 ReplacedIB 동시 전달 조건에서 shadow/depth/HDR readback을 바이트 비교했다. 네 비교 모두 PASS, exit=0(18.51초). 호출 지역 shared_ptr 소멸 이후 정렬·렌더하는 경로도 포함한다. 임시 코드는 제거했고 해당 두 소스를 재컴파일한 최종 Release 증분 빌드도 exit=0(12.23초)이다. 실행 파일에 임시 검사 문자열이 남지 않은 것도 확인했다.
- 자료는 `%TEMP%/CP3/`에 보관했다. 투명 재질의 override 실행, Debug·D3D12 validation 및 드라이버 버전은 확인하지 않았다. 투명 패스의 적용은 동일 공통 루프의 코드 확인에 한정한다.


### CP-4a: D1 CPU 압축 IB 적용 (base `415fa819`, 커밋 대기, 2026-11-20)

- `SetTriangleMask`는 원본 mesh 번호를 skin CPU section 번호에 연결하고, draw 순서로 보존된 `Indices`에서 bit31 제거 삼각형을 생략한다. 압축 값은 원본 baseVertex를 뺀 draw 상대값이며 공유 Model·Mesh.draw는 수정하지 않는다. 제거 요청 section의 CPU 인덱스가 없으면 로그와 false를 반환하고 이전 상태를 보존한다.
- 컴포넌트 소유 R32 업로드 IB 하나와 section별 `MeshRenderOverride`를 게시한다. 전체 제거 draw도 배열 길이를 유지하며 IndexCount=0이다. 제거 없는 section은 원본 view를 쓰고, 제거가 전혀 없으면 압축 IB를 만들지 않는다.
- 재설정 시 이전 압축 IB는 `CommandListManager::RetireResource`로 마지막 제출 queue fence와 함께 보존하고 완료 후 수집한다. publish 교체의 IdleGPU는 제거했다. `ClearTriangleMask`의 기존 IdleGPU·소유 객체 해제가 새 IB와 override도 정리한다. `SetGeometry`·`CommitGeometryReplacement`의 해제 호출은 유지된다.
- `Render`는 override만 넘기며 ReplacedIB 주소를 넘기지 않는다. `UpdateTriangleMask`는 추출 State 업로드만 수행한다. 옛 셰이더·ReplacedIB 멤버·dispatch 구현·TriangleMaskShaderTests는 CP-4b까지 남기되, 상수 false 분기로 할당·dispatch를 비활성화했다.
- `SkeletalMaskChecks`의 ReplacedIB readback을 압축 IB readback으로 바꿨다. 원본 GPU R16/R32 인덱스로 독립 계산한 남은 삼각형 순서·section view 크기·draw 범위를 비교한다. Xbot/CesiumMan shadow·depth·HDR 및 source/peer/reset/restore PASS, 전체 제거 draw 배열·0개 count와 제거 없는 원본 복귀 PASS.
- Release/Windows(x64), NVIDIA GeForce MX450(1906 MB). 첫 증분 빌드 144.39초, 마지막 구현 빌드 81.94초, 오류 0. HEAD 기준 재빌드도 Release만 사용했다. Debug 빌드·D3D12 validation은 디스크 제약과 이번 요청에 따라 실행하지 않았다.
- HEAD와 구현본의 동일 게시 프레임 HDR RAW는 각각 8,294,416바이트 전체 일치: Xbot frame=65, CesiumMan frame=64, D6 request1=64/request2=112 및 두 번째 게시 age=0. 비동기 게시 프레임이 다른 실행은 비교에서 제외하고 재실행했다. SHA-256·변경 바이트 수·로그는 `%TEMP%/CP4a/hdr-comparison.json`에 보관한다. `-capture-count`가 HDR을 생략하므로 기본 8장 캡처 뒤 첫 게시 프레임만 보존했다.
- 회귀: offset-copy 20요청 GPU==CPU PASS; Parent/Rig break 각각 6요청 및 boundary pass=4/fail=0/n/a=2; SkeletalPatchTests 131 checks PASS. D5 RigBoneCutChecks PASS. Parent 다중 절단은 조각 3개, maskCalls=1, patchIds=1, topologyDelta=2, constraints 18→16으로 게시했다. 일반 Update·ConsumeMultiBreak·PublishRigBonePatch와 Pending 누적 D6 경로를 확인했다.
- 게시 시 단일 실행 계측(수집기 연결): Xbot CPU 압축 1,239.1 µs, 업로드 힙 생성·복사 419.0 µs, IB 563,928 B; CesiumMan 110.5 µs / 205.6 µs / 52,812 B. D6 IB는 570,672→545,256 B, CPU 976.2→655.7 µs였다. 업로드 계측에는 이전 GPU 사용 대기를 포함하지 않는다.
- 미검증: R32 원본·비영 startIndex/baseVertex·CPU 인덱스 누락 거절의 합성 fixture, 별도 새 실행의 코드 리뷰. 근거와 실행 스크립트는 `%TEMP%/CP4a/`에 보관하며 커밋하지 않았다.

- CPU compaction unit-check fixture: CP-4b로 이월.
- CP-4a 리뷰 수정: publish 교체만 기존 fence 지연 해제를 재사용한다. 소멸·ClearTriangleMask·geometry 교체 대기는 유지한다. `compactUploadUs`는 CPU 압축+업로드 합계이며 `retireWaitUs`는 관리 비용·할당 실패 안전 대기를 포함한 대기 상한이다.
- MX450 Release 단발 비교(임시 Update 계측, 제거 완료): 두 번째 publish CPU Update 15.797→19.140 ms로 전체 단축은 확인하지 못했다. IB 교체 구간 4,601.2→0.9 µs, 압축+업로드 1,628.1→1,376.2 µs. 게시 frame은 113→112로 비동기 시점 차이가 있다. HDR은 기존 CP-4a 결과와 SHA-256 EB597C73DE8646772702B772D30B5B658C90C4B7631DD9E165EC93DE66133C72로 일치한다. 근거: `%TEMP%/CP4a/review-before-recut.log`, `review-after-recut.log`.
- 최종 Release 증분 빌드 exit=0, 임시 계측 문자열 제거 확인. `-skeletal-mask-checks 1` PASS/exit=0(33.84초), `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1` PASS/exit=0(26.00초). 로그는 `%TEMP%/CP4a/review-final-{mask,patch}.log`; Debug는 실행하지 않았다.
- HDR 일치는 임시 Update 계측이 있는 동일 게시 frame=112 실행에서 확인했다. 계측 제거 후 최종 바이너리 재현도 모두 PASS/exit=0이나 게시 frame=113~115로 달라 기존 frame=112 HDR과 해시는 다르다. 이 실행들은 동일 시점 HDR 비교 근거에서 제외한다(`review-final*-recut.log`).

### CP-4b: 옛 마스크 경로 제거·CPU 압축 단위 검사 (base `ad5c74cd`, 커밋 대기, 2026-11-20)

- `TriangleMaskCS.hlsl`, `TriangleMaskShaderTests.cpp`와 프로젝트 등록, `ReplacedIB`·`FirstByte`·`ByteCount`·미사용 `TriangleCount`, 상수 false 분기와 dispatch/root signature를 제거했다. 렌더러의 옛 IB 주소 대체 인자·정렬 필드도 유일한 호출자가 0만 넘기므로 제거했다. `UpdateTriangleMask`의 GPU State 업로드는 추출 입력으로 유지한다.
- `TriangleMaskCompaction.h`의 `CompactTriangleMask`는 GPU 비의존 입력·출력으로 압축하며, 전체 성공 후에만 출력을 교체한다. `SetTriangleMask`도 성공 결과를 받은 뒤 GPU 자원을 만들고 게시하므로 거절 시 이전 Pending·IB·override·revision을 건드리지 않는다.
- `SkeletalPatchTests`의 합성 원본 R16/R32에는 다중 draw·section, 비영 startIndex/baseVertex가 있다. 기대 IB는 압축 함수를 호출하지 않고 리터럴로 지정했다. 전체 제거의 draw 수·0 count, 제거 없음의 override 생략, CPU 인덱스 없음·길이 불일치·비skin 거절과 기존 결과 보존, 혼합 형식 정렬을 검사한다. CPU 입력은 loader가 draw 순서로 펼쳐 baseVertex를 더한 배열이므로 startIndex는 fixture 준비에서 소비된다.
- R16 유지 결정: 형식별 stride·범위 검사·바이트 저장·section DWORD 정렬·view 형식·GPU 검사 변경은 약 20줄이다. 원본 형식을 따르며 R16 section 사이 패딩은 최대 2바이트, R32는 그대로다. CPU 테스트 Release 144 checks PASS. Release·실행·동일 게시 프레임 HDR 검증 결과는 아래에 기록한다.
- 최종 검증: Release/Windows(x64), NVIDIA GeForce MX450(1906 MB), `-debug 0`. 저장소 밖 `%TEMP%/CP4b/Build/`에 기존 Release 중간 파일을 복사하고 `SolutionDir`만 외부 경로로 지정해 빌드했다. 최종 exit=0·error 0, CPU Release 144 checks PASS. 초기 인코딩 오류는 UTF-8 BOM으로 수정했으며 이후 최종 바이너리로 재검증했다. Debug는 요청에 따라 빌드·실행하지 않았고 드라이버 버전은 재조회하지 않았다.
- 최종 실행: `-skeletal-mask-checks 1` PASS(18.20초), `-skeletal-patch-checks 1 -skeletal-patch-offset-copy 1` 20요청 GPU==CPU PASS(20.08초), `-skeletal-patch-break-checks 1` Parent/Rig 각각 6요청 GPU==CPU·boundary pass=4/fail=0/n/a=2 PASS(23.37/23.32초). `-skeletal-recut-repro 1` 요청 1 frame=64·요청 2 frame=112 정상 게시/PASS(25.40초). 모두 exit=0, timeout=False.
- R16 효과: Xbot 동일 절단 IB는 563,928→281,964바이트. `-animation-step-ms 16` HDR RAW 각 8,294,416바이트가 같은 게시 프레임에서 완전히 일치한다: Xbot frame=64, CesiumMan frame=64, 재절단 request1=64/request2=112·두 번째 게시 age=0. Xbot 기준은 이번 HEAD 바이너리 캡처, 나머지는 CP-4a 보관 캡처와 비교했다. 최초 Xbot 기준 frame=65는 제외하고 frame=64로 재실행했다. SHA-256: Xbot `F7991A50EE392D09C2D0D2143CBE604AF8B9F9918C5C4378A133F68D511808E2`, CesiumMan `9E7146F87DB61AEBB313FBD2BC0058AF7D738243B61D959775450130E3C88259`, 재절단 `EB597C73DE8646772702B772D30B5B658C90C4B7631DD9E165EC93DE66133C72`.
- 근거: `%TEMP%/CP4b/`의 빌드·CPU·`final-*`·`after-*` 로그, `hdr-comparison.json`과 RAW. 옵션 전달이 잘못된 초기 실행은 timeout으로 제외하고 스크립트에 옵션을 직접 지정해 재실행했다. 외부 임시 빌드 산출물 삭제는 경로 검증·절대 경로 명시 후에도 자동 승인 정책에 차단되어 `%TEMP%/CP4b/Build/` 및 같은 폴더의 CPU exe/obj가 남아 있다. 상세 거절 사유는 제공되지 않았다. 커밋하지 않았다.
