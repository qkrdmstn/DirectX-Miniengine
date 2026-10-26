상태: 완료 — `39db69f3`에서 구현(초기화 실패 fallback·검증 계약 보강 포함); Jolt v5.6.0 연결·Static 절단 후보 조회, 낙하/접촉 반응은 후속 (2026-10-26)

# Jolt 연결과 평면 절단 후보 조회

## 완료 범위

사용자의 우선 요청은 Jolt 연결과 평면-GameObject 검사에서 collider 접근을 단순하게 만드는 것이다. 절단 결과는 같은 GameObject의 A/B MeshComponent로 유지한다. 이번 구현은 **조회 전용 보수적 Box**를 Jolt에 등록하고 절단 후보를 반환한다. 동적 body simulation, 실제 조각 convex 분할·질량·낙하는 이번 완료 범위에 포함하지 않는다. 조회 Box를 실제 메시의 정밀 충돌체나 질량 계산용 proxy로 해석하지 않는다.

기존 `CuttingSystem::RequestAll`의 Scene/mesh 순회·경계 구 검사 대신 `PhysicsSystem::QueryCutCandidates`를 한 번 호출한다. SDK 포인터나 BodyID를 절단 코드로 전달하지 않는다. Jolt broadphase는 후보 조회이며 실제 surface split·접촉/공면·유한 사각형 전체 경계 검사는 기존 GPU 절단이 결정한다.

```text
Scene.Update → GraphicsContext.Finish
    → PhysicsSystem.SyncColliders
        Static 메시의 보수적 월드 구 → 조회 Box → Jolt body
        (ObjectId, MeshId, GeometryRevision, StateVersion, Wc) snapshot

C / RequestAll(plane)
    → PhysicsSystem.QueryCutCandidates
        사각형 월드 AABB + epsilon → Jolt QueryAabb
        → 현재 body 경계 → 기존 평면/구 조건 → ID·revision·변환 검사
        → 메시별 snapshot 정렬·중복 제거
    → 기존 SubmitPending / GPU 절단 / A/B 게시
    → 다음 Scene.Update 뒤 A/B 조회 collider 갱신
```

## 파일과 책임

| 파일 | 실제 구현 |
| --- | --- |
| `Tools/BuildJolt.ps1` | 고정 archive 다운로드·SHA256 검증, CMake 증분 빌드, Debug/Release/Profile 매핑, 테스트 실행 |
| `MiniEngine/Physics/CMakeLists.txt`, `ThirdParty.md` | v5.6.0 commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`, MIT 고지, 공식 Jolt target과 adapter 연결 |
| `Physics/Physics.vcxproj/.filters`, `PropertySheets/Physics.props` | v143/x64 MSBuild bridge와 최종 정적 lib 링크. Profile은 SDK Release 사용 |
| `Physics/PhysicsWorld.h/.cpp` | SDK 수명·전용 layer·Box 생성/갱신/제거·`QueryAabb`. handle은 단조 증가 uint64이며 Clear/재초기화에서도 재사용하지 않음 |
| `ModelViewer/Scene/PhysicsSystem.h/.cpp` | `SyncColliders`, `QueryCutCandidates`, Scene ID 매핑·stale 검사·fallback·수명 정리 |
| `Scene/ComponentId.h`, `Scene.*` | ColliderComponent의 QueryHandle 참조와 `SetColliderQueryHandle`. SDK 객체 소유는 PhysicsWorld에 한정 |
| `Scene/CuttingSystem.*` | 조회 결과를 기존 후보 큐에 연결. 요청 당시 revision/StateVersion/Wc 검사는 유지 |
| `ModelViewer.cpp` | 시작·종료, pose 준비 후 collider 동기화, 명시적 진단 옵션 |
| `Physics/tests/PhysicsQueryTests.cpp`, `Scene/tests/PhysicsQueryChecks.h` | 실제 SDK CPU 검증과 GPU 준비를 포함한 장면 수명·좌표 통합 검증 |

빌드 생성물과 SDK 소스는 기존 ignore 대상 `MiniEngine/Build/ThirdParty/Jolt/` 아래에만 둔다. archive SHA256은 `1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd`다. Debug `/MDd`, Release·Profile `/MD`, C++17, SSE2, 정적 Jolt.lib이며 Jolt Compute backend는 비활성이다. SDK 헤더와 공개 compile 정의는 CMake adapter 내부에서 일치시킨다. 초기 플랫폼은 x64이며 다른 플랫폼에는 명시적 빌드 오류를 낸다.

## 정합성·실패 정책

- 같은 owner의 A/B는 MeshId가 다르므로 별도 조회 body·후보로 반환한다. GameObject 단위로 합치지 않는다. 생성 B를 진행 중인 batch에 다시 넣지 않는 기존 정책도 유지한다.
- 조회 경계는 렌더의 기존 `GetBoundingSphere`에서 가져온다. node/source frame·현재 revision·Attached/WorldIndependent 배치는 렌더와 같은 계산을 사용한다. 조회 Box에는 월드 AABB를 사용하므로 전단을 rigid T/R로 잘못 분해하지 않는다. GPU 정점 readback은 추가하지 않는다.
- 축 정렬 평면의 두께 0을 포함해 검색 AABB를 world epsilon과 바깥 방향 한 ULP로 확장한다. body Box 생성도 중심·반크기 반올림을 보수적으로 처리한다. 검색에 사용한 두께를 GPU 절단 두께로 전달하지 않는다.
- Jolt broadphase는 이동 이전 영역을 여유 후보로 남길 수 있다. `QueryAabb`는 body read lock 후 현재 월드 경계로 다시 검사하며, Scene 단계에서 기존 평면/구 조건도 적용한다.
- query filter와 simulation pair filter를 분리한다. 조회 body는 서로 접촉 반응을 하지 않지만 query에서는 보인다. 현재 `PhysicsSystem::Update` simulation 호출·job pool·fixed step은 추가하지 않았다.
- 기본 body 예산은 4096이다. 예산 부족·SDK 비활성·지원 범위를 벗어난 조회 경계는 기존 경계 검사 fallback 목록으로 유지한다. 미등록 메시를 조용히 누락하지 않는다. SDK 경계는 유한·정방향·절댓값 1e15 이하를 지원한다.
- `SyncColliders`는 매 프레임 Static 적격 메시를 순회한다. 실제 Box는 경계가 달라질 때만 교체한다. 정상 query는 Jolt hit와 별도 fallback 목록만 읽는다. 대규모 장면 성능 향상은 아직 측정하지 않았으며 전체 프레임 순회가 없어졌다고 주장하지 않는다.
- 빈 geometry·메시 삭제·owner 삭제는 다음 동기화에서 body와 대응 참조를 제거한다. query는 현재 ID/revision/StateVersion/Wc를 재검사한다. 외부 호출자는 최신 Scene.Update/Finish와 SyncColliders 뒤에 query해야 한다.
- 용량이 0 또는 65536(`MiniPhysics::MaxColliderCapacity`) 초과이거나 SDK 초기화가 예외로 실패하면 `PhysicsSystem::Initialize`가 `Physics query disabled: ...` 경고를 출력하고 조회를 끈 채 경계 검사 fallback으로 계속 실행한다. 크래시하지 않는다.
- 호출 계약: `QueryCutCandidates`는 `SyncColliders`가 만든 snapshot을 읽으므로 같은 프레임에서 `SyncColliders` 뒤에 호출해야 한다(현재 `ModelViewer::Update` 순서가 만족). 마지막 Sync 이후 새로 적격이 되었거나 StateVersion이 바뀐 메시는 오래된 데이터로 조회하지 않고 후보에서 누락된다.
- `ValidateQueries`는 Jolt 결과를 같은 `SyncColliders` 캐시의 전수 검사와 대조한다. 실제 Scene이나 이전 `RequestAll` 순회와 비교하지 않으므로 Sync 단계의 누락은 검출하지 못한다.
- geometry가 비어 있는(section 없음) 메시는 후보에서 제외된다. 이전 `RequestAll`과 달라진 동작이다.
- 종료는 Cutting.Clear → Physics.Clear → Scene.Clear → renderer 종료다. Physics.Clear는 대응 참조·body·SDK world를 종료한다. 다시 사용할 때 Initialize 후 SyncColliders를 호출한다.
- Skeletal section은 기존 Static 절단과 같이 후보에서 제외한다. 본별 body/constraint는 후속이며 mesh 1개↔body 1개를 영구 제약으로 만들지 않는다.

## 단계별 리뷰와 실측

사용자가 단계별 자체 리뷰 후 최종 완료까지 계속 진행하도록 지시했다. 각 개념을 나누어 구현·리뷰했으며 커밋하지 않았다.

| 단위 | 리뷰·검증과 보완 |
| --- | --- |
| SDK 재현 빌드 | 공식 고정 소스·SHA·라이선스 확인. 생성 vcxproj에서 DLL CRT와 AVX/Compute 정의 비활성 확인. 첫 Debug SDK 빌드 약58초 |
| SDK 경계·수명 | 실제 Jolt CPU 테스트. 이동 전 broadphase의 과잉 후보를 발견해 현재 body 경계 필터 추가. 오래된 handle·capacity·Clear·여러 world 공유 검증 |
| Scene 등록·조회 | owner+mesh 키, revision 갱신, WorldIndependent, 회전/비균일 scale, 빈 revision·삭제 검증. 정상 query의 fallback 전수 순회를 별도 목록으로 분리 |
| 절단·빌드 연결 | RequestAll을 단일 조회 호출로 변경. MSBuild의 Windows PowerShell에서 Get-FileHash 명령 부재를 발견해 .NET SHA256 계산으로 수정. 추가 소스 인코딩 경고와 infinity 상수 경고 제거 |
| 최종 회귀 | 아래 Debug/Release CPU·장면·절단·정상 종료 검증. Profile 전체 smoke build도 성공 |

Debug adapter 연결을 포함한 Viewer 빌드는 약35초, 첫 Release SDK+Viewer 빌드는 약102초, 첫 Profile 전체 빌드는 약139초였다. 재구성·캐시와 동시 작업의 영향을 받는 이 환경의 실측이며 다른 PC의 예상 시간은 아니다. 최초 Debug 실행 한 번은 60초 제한에 도달해 도구가 종료했고 성공으로 세지 않았다. 이후 정상 종료를 기다리는 설정으로 재실행해 통과했다.

## 실행 검증

환경: NVIDIA GeForce MX450, driver `32.0.15.9186`, MSBuild 17.14.60 / MSVC 14.44.35207 / CMake 3.26.0-rc6. SDK CPU 테스트 seed는 `20261025`다. 동적 simulation은 하지 않으므로 fixed dt·질량 검증 수치는 없다.

| 검증 | 결과 |
| --- | --- |
| SDK CPU Debug / Release | 각 8,530 checks 통과. 생성·이동·resize·경계 접촉·NaN/역전 경계·삭제·stale·capacity·재초기화·무작위 후보 누락 검사 |
| 기존 SceneMath | 457 checks 통과, 산출물은 `%TEMP%/jolt-query-validation/` |
| Viewer Debug / Release | 장면 query self-test + Cube 4회 RequestAll 절단, 게시 4회·GPU 보존 검증 통과 |
| SDK 비활성 / capacity 1 | 각 장면 self-test와 Cube 4회 절단 통과. fallback으로 대상 누락 없음 |
| Bunny | RequestAll 1회 게시·GPU 부모 보존 검사 통과 |
| 유한 사각형 width 0.2 | 후보는 조회되지만 GPU가 전체 경계 미포함을 거절, 게시 0회 통과 |
| Cube·Bunny·Xbot 혼합 장면 | 기존 component-checks와 새 장면 query self-test 통과. Static 메시 4개를 조회에 등록하고 skin section은 제외 |

위 Viewer 실행은 모두 `-debug 1`과 `-physics-query-checks 1`을 사용했고, cleanup에서 registered/fallback 모두 0, process exit 0을 확인했다. GPU 결과와 정상 종료를 확인한 자동 실행이며 실제 마우스/C 키 입력은 수동 검증하지 않았다. Debug Cube의 post 캡처를 확인했고 기본 gap 0에서 외형 위치가 유지된다.

D3D12 ERROR/CORRUPTION과 치명적 예외는 없었다. 렌더 경로의 PS output/RT 채널 불일치 경고, 일부 반복 ResourceBarrier 경고, Debug 종료의 LightGrid 등 live-resource 경고는 남아 있다. 이번 Jolt 조회는 D3D12 자원을 만들지 않으며, 이 경고의 발생 전후 비교나 수정은 수행하지 않았다. 무경고 실행으로 보고하지 않는다.

로그는 `%TEMP%/jolt-viewer-{debug,release,profile}.log`, `%TEMP%/jolt-query-tests-{debug,release}.log`, `%TEMP%/jolt-runtime-{debug,release,fallback,capacity,bunny,finite,mixed}.log`에 있다. 캡처는 `%TEMP%/jolt-query-validation/debug-cube-{hdr,post}.raw`와 `debug-cube-post.png`다.

재현 예:

```powershell
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows
./Tools/BuildJolt.ps1 -Configuration Debug -Target PhysicsQueryTests -RunTests
# ModelViewer 작업 디렉터리에서 실행한다.
../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -model Models/cube.glb -physics-query-self-test 1 -physics-query-checks 1 -physics-query-exit 12 -cut 4 -cut-scene 1 -cut-repeat-plane 1 -cut-step-y .25 -cut-validate 1 -cut-expect-publications 4 -cut-gap 0
```

진단 옵션: `-physics-query 0`은 SDK 조회를 끄고, `-physics-query-capacity 1`은 fallback을 강제한다. `-physics-query-self-test 1`은 임시 객체로 장면 수명/변환 검증을 수행해 정리한다. `-physics-query-exit N`은 N 프레임 이후 모든 자동 절단 완료를 기다린 뒤 정상 종료한다. 옵션을 생략한 일반 실행은 자동 종료하지 않는다.

## 남은 한계

- 용량이 가득 차면 미등록 메시가 매 프레임 `CreateQueryBox`를 재시도한다(할당과 handle 소모).
- NaN 경계는 등록되지 않아 매 프레임 다시 생성을 시도한다.
- `AddColliderComponent`는 revision마다 owner의 ComponentId 카운터를 소비한다.
- self-test는 ObjectId를 하나 소비한다.
- broadphase 최적화(`PhysicsSystem::Update`/`OptimizeBroadPhase`)는 호출하지 않는다.
