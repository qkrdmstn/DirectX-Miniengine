상태: 설계안 — 사용자 결정(2026-11-12) 반영, Codex 리뷰 반영, 구현 전 (2026-11-12)

# 래그돌 rig 사전 생성과 Jolt 기반 뼈 선택

## 1. 목표

스켈레탈 메시를 로드할 때 래그돌 rig를 미리 만들고, C 키 평면 절단의 대상·뼈 선택을 그 rig의 Jolt body 조회로 바꾼다.
- 몸에 붙은 kinematic body는 충돌 응답 없이 겹침만 보고한다(sensor).
- 평면이 관절 segment를 지나면 지금처럼 관절을 끊는다(SP-7 break).
- 평면이 뼈 영역만 지나면(머리 중간, 옆구리) 관절을 끊지 않고 그 뼈 영역만 Static처럼 자른다.

## 2. 현재 구조 (Codex 조사 요약, `bb766e1b`)

- rig는 첫 C 입력 때 `SkeletalPatchCopy::Request` → `BuildSkeletalPhysicsAsset` → `SkeletalPhysicsSystem::RegisterFromPose`로 owner마다 한 번 생성된다. `IsRegistered(owner)`로 재사용하고, `Register`는 이미 등록된 owner를 거절한다.
- AutoRig 입력(부모 배열, CPU node pose, section E, CPU 정점·palette·inverse bind)은 로드 직후 CPU에 모두 있다. GPU 스키닝을 기다릴 필요가 없다.
- `ModelViewer::Startup`은 객체 생성 뒤 크기·배치를 바꾸고 `m_Physics.Initialize`를 다시 부른다. 사전 생성은 그 뒤여야 한다.
- rig body는 Kinematic, ObjectLayer `Moving`, `CollisionGroup={owner,0}`로 `PhysicsWorld::PrepareBody`가 만든다. sensor·contact listener는 엔진에 없다.
- break는 `SkeletalPhysicsSystem::BreakConstraint` → `PhysicsWorld::ApplyConstraintBreak`(검증 뒤 실패 분기 없음)가 subtree를 Dynamic으로 바꾼다.
- 조회는 `PhysicsWorld::QueryAabb`(Query layer 전용)뿐이고 rig body용 shape 조회는 없다.
- 뼈 선택은 node 트리 기반 `SelectSegmentJoint`다. segment가 없으면 가장 가까운 영향 Leaf로 fallback한다. 정상 AutoRig의 constraint ParentBody.Node→ChildBody.Node 쌍은 이 segment 쌍과 같다. `FindIncomingJoint(N)`은 N으로 들어오는 관절이다(교차한 N→자식 관절이 아니다).
- 게시: `SkeletalPatchCopy::Request`는 offset 진단이 아니면 break 준비가 없을 때 거절하고, `Update`는 `BreakStatus::Applied`일 때만 Attachment·원본 마스크를 게시한다. 절단 제출 실패 시 복사 owner를 삭제한다.
- AttachedPatchRecut(AR): `CuttingSystem::Request` → `PhysicsSystem::QueryCutCandidates`가 `IsEligible`(게시·pose bounds·준비된 geometry)과 `SyncColliders`로 등록된 collider를 요구한다. node 원점 쪽(1 mm 초과)만 부착을 유지하고 반대쪽은 `BuildDetachedCollision`(KDOP18) body로 떨어뜨린다. 같은 owner의 rig `{owner,0}`와 독립 조각 `{owner,1}`은 `AdjacentPairFilter::CanCollide`가 제외한다.
- `-skeletal-patch-checks`는 진단 `Request`에 physics를 넘기지 않으므로 `-skeletal-patch-offset-copy 1`과 함께 돌려야 끝까지 진행한다.
- AR-4 `-attached-recut-checks 1`은 이 세션에서 Debug/Release 빌드 후 실행해 다섯 항목 PASS를 확인했다(2026-11-12).

## 3. 결정과 이유

D1. 로드 시 rig 생성
- `ModelViewer::Startup`에서 배치·`m_Physics.Initialize`·`Scene::InitializePoses`가 끝난 뒤 skin owner마다 rig를 등록한다.
- 생성 코드는 `SkeletalPatchCopy::Request` 안의 snapshot 구성·`BuildSkeletalPhysicsAsset`·`RegisterFromPose`를 함수 하나(`RegisterAutoRig`)로 옮겨 두 곳이 같이 쓴다. `Request`의 "미등록이면 생성"은 남긴다.
- 자체 rig를 등록하는 진단(`-skeletal-break-checks`, `-skeletal-break-xbot`)과 물리 꺼짐에서는 사전 등록을 하지 않는다. BC-12b가 Xbot owner에 3-body asset을 등록하기 때문이다.
- 생성 실패는 로그만 남긴다. 그 owner는 D6의 현행 경로 대상이 된다.
- 이유: 첫 C 입력의 지연을 없애고, C 입력 전부터 Jolt 조회 대상이 있어야 D4가 가능하다.

D2. body 쌍 겹침 측정
- 등록 직후 같은 owner의 body 쌍 OBB를 CPU SAT로 검사해 "인접 쌍 겹침 n / 비인접 쌍 겹침 m"과 상위 몇 쌍(Node 이름)을 로그로 남긴다. 생성·등록 시간(ms)도 같이 남긴다.
- 측정 결과와 무관하게 최종 판정은 D4의 segment 판정이다. 겹침은 후보가 늘어난다는 뜻일 뿐이다.

D3. kinematic body = sensor
- rig body만 `PrepareBody`에 sensor 플래그를 넘겨 `BodyCreationSettings::mIsSensor=true`로 만든다(Static 조각·독립 조각은 그대로). `mCollideKinematicVsNonDynamic`은 켜지 않는다: Dynamic과의 겹침만 보고하면 충분하다(Jolt 5.6.0 kinematic sensor는 Dynamic 접촉을 보고한다).
- `PhysicsWorld::Impl`이 Jolt `ContactListener`를 소유하고 `Initialize`의 `World.Init` 뒤 등록, `Shutdown`에서 해제한다. sensor가 낀 접촉 시작 수만 원자 카운터로 센다(진단 로그용, 로직 미사용).
- `ApplyConstraintBreak`는 검증이 끝난 뒤, 쓰기 잠금 아래에서 전환 대상 body마다 `Body::SetIsSensor(false)`를 부르고, 그다음 Dynamic으로 바꾼다.
- 결과: 떨어진 쪽은 몸 body를 통과하고 바닥·Static 조각·독립 조각과는 충돌한다. 같은 owner 필터와 인접 쌍 제외는 그대로 둔다.
- 초기 겹침 무시(사용자 결정 2026-11-12): sensor인 몸 안에는 다른 Dynamic body가 들어와 머물 수 있다. break로 sensor를 끄는 순간 이미 겹친 Dynamic body가 있으면 강한 접촉 보정으로 튄다(R2 첫 구현에서 BC-12a가 tick 61 anchor 오차 0.277 m로 FAIL, 외부 Dynamic body 하나와의 겹침이 원인으로 확인). 그래서 sensor를 끄는 body마다 그 순간 겹친 Dynamic body 쌍을 기록하고, 두 body가 더 이상 겹치지 않을 때까지 그 쌍의 충돌을 끈다. 쌍 필터는 기존 `AdjacentPairFilter`의 제외 쌍 구조를 재사용하고, 분리 확인은 Step 뒤 쌍마다 수행한다.

D4. Jolt 조회 + segment 최종 판정
- `PhysicsWorld::CollideOrientedBox`: 유한 평면 사각형을 반두께 1 mm(convex radius 0)의 `BoxShape`로 만들어 `NarrowPhaseQuery::CollideShape`를 `SpecifiedBroadPhaseLayerFilter(Moving)`·`SpecifiedObjectLayerFilter(Moving)`로 부른다. 결과 BodyID를 simulation BodyHandle로 바꾸고 중복을 없앤다.
- `SkeletalPhysicsSystem`이 BodyHandle → (owner, BodyId, NodeId)를 돌려준다. 이미 Dynamic인 body는 후보에서 뺀다.
- segment 목록은 hit owner의 rig constraint 중 runtime handle이 살아 있고 양 끝 body가 Kinematic인 것이다. 끝점은 각 body node의 현재 world joint 원점이다(body 중심 아님). 부모 body 또는 자식 body가 hit에 든 constraint만 후보로 둔다.
- 선택: 양 끝이 평면의 엄격히 반대 부호이고 교점이 유한 사각형 안인 segment 중, 교점이 사각형 중심에 가장 가까운 것. 동률은 낮은 owner, 낮은 부모 NodeId. owner가 여럿이어도 하나만 고른다(관절은 입력당 하나).
- 선택 결과는 segment의 부모 node P다. 그 뒤는 현행 그대로다: 패치 = P weight 삼각형, `QueueBreak` → `FindIncomingJoint(P)`, A는 P의 부모 node, B는 P 부착. P로 들어오는 관절이 없거나 이미 끊겼으면 현행 거절 로그를 따른다. Leaf fallback은 없다(segment가 없으면 D5).
- 새 모드 `Rig`를 C 키 기본값으로 둔다. `Parent`/`Influence`는 진단용으로 남긴다.

D5. segment 없음 → 뼈 영역 Static 절단 (break 없음)
- hit는 있는데 D4 segment가 없으면, hit body 중 body 중심이 사각형 중심에 가장 가까운 것 하나를 고른다. 그 body의 node를 N이라 한다.
- N의 joint 원점과 평면의 거리가 1 mm 이하이면 거절하고 로그(AR의 모호 판정과 같은 기준). 아무것도 바꾸지 않는다.
- (a) `SkeletalPatchCopy`에 break와 독립된 "부착 패치 게시" 상태를 둔다: N weight 삼각형을 `PatchExtractCS`로 복사하고, 절단 없이 N에 부착한 generated Static 패치로 게시하며 원본 영역을 `TriangleMaskCS`로 숨긴다(보이는 모습은 그대로). 이 상태는 break 준비를 요구하지 않고, 기존 break 경로의 취소·삭제 분기와 섞지 않는다.
- (b) 게시 뒤 `Scene::PrepareMeshes` → `PhysicsSystem::SyncColliders`로 패치가 후보 조건을 갖춘 다음 프레임에, 같은 평면으로 그 패치에 `CuttingSystem::Request`를 보낸다. AR 경로가 node 쪽 부착·반대쪽 KDOP 독립 조각을 처리한다.
- (b)가 실패·무분할·취소되면 (a)의 부착 패치와 마스크를 그대로 둔다(모습은 절단 전과 같다). 로그만 남긴다.
- 이유: AR의 node 쪽 판정·부착 승계·KDOP·owner 필터를 그대로 재사용한다.
- 한계(R4a 리뷰, 2026-11-12): (a)는 추출 시점 정점을 게시 시점에 N의 상대 변환만으로 옮긴다(SP-7 break 게시와 같은 강체 부착). N 외 뼈와 섞인 정점은 대기 프레임 동안의 자세 변화만큼 원본과 어긋날 수 있다. 부착 후 강체 패치가 스키닝과 달라지는 설계 한계와 같은 성질이라 이번 범위에서는 고치지 않는다.

D6. rig가 없는 owner
- Jolt 조회는 등록된 rig만 본다. hit가 하나도 없으면 C 키는 rig가 없는 skin owner만 대상으로 현행 경계 구 + `SelectSegmentJoint` 경로(미등록이면 생성)를 쓴다. 물리 꺼짐이면 모든 owner가 여기에 해당해 현행 결과와 같다.

D7. rig 디버그 뷰 = Jolt 기본 디버그 렌더러 (사용자 결정 2026-11-12)
- 현재 Jolt CMake 캐시는 `DEBUG_RENDERER_IN_DEBUG_AND_RELEASE=OFF`라 `JPH_DEBUG_RENDERER`가 없다. 이 옵션을 켜고 Jolt를 다시 빌드하며, 엔진 쪽 Jolt 포함 프로젝트에도 같은 define을 넣는다(클래스 배치가 바뀌므로 반드시 일치).
- `DebugRendererSimple`을 상속한 클래스가 `DrawLine`을 기존 `CollisionDebugDraw`의 선 overlay(근평면 자르기 포함)로 보내고, `DrawTriangle`은 세 모서리 선으로, `DrawText3D`는 무시한다.
- H 키(`Viewer/Physics/ShowCollision`)가 켜져 있으면 `PhysicsSystem::DrawBodies`(wireframe, 모든 body)와 `DrawConstraints`를 같이 그린다. 기존 hull overlay는 그대로 둔다.
- 이유: rig body·관절·sensor/Dynamic 상태를 Jolt가 보는 그대로 확인하고, R2~R4 검증에 쓴다.

## 4. 인터페이스 골격 (이름은 구현 단계에서 기존 규칙에 맞춰 조정 가능)

```cpp
// SkeletalPhysicsSystem
bool RegisterAutoRig(Scene& scene, ObjectId owner);           // 로드·C 입력 공용 AutoRig 등록
bool FindBody(BodyHandle body, ObjectId& owner, uint32_t& bodyId, uint32_t& nodeId) const;
void BuildKinematicSegments(ObjectId owner, std::vector<RigSegment>& out) const; // Parent/ChildNode, world 끝점

// PhysicsWorld
void CollideOrientedBox(const Math::Vector3& center, const Math::Quaternion& rotation,
    const Math::Vector3& halfExtents, std::vector<BodyHandle>& hits) const;
uint32_t SensorContactCount() const;
// PrepareBody 입력에 bool Sensor 추가(rig만 true)

// SkeletalPatchCopy
struct RigSelection { ObjectId Owner; uint32_t NodeId; bool Segment; /* false = D5 */ };
```

## 5. 단계 (단계당 한 개념, 약 300줄 이하)

측정 근거(2026-11-12): Debug 빌드 77초, Release 94초, 검사 실행 1회 약 5초, Codex 조사 약 7분, Codex 리뷰 2~5분. 구현 Codex는 단계당 15~25분으로 잡는다.

| 단계 | 내용 | 예상 |
| --- | --- | --- |
| R1a | 기존 검사 기준선 기록(아래 공통 명령) + `RegisterAutoRig` 추출(동작 불변). | ≈ 30분 |
| R1b | D1 Startup 사전 등록·진단 제외, D2 겹침·시간 로그. | ≈ 30분 |
| R1c | D7 Jolt 디버그 렌더러: Jolt를 `JPH_DEBUG_RENDERER`로 재빌드, `DebugRendererSimple` 구현을 H 키 선 overlay에 연결. | ≈ 45분 |
| R2 | D3 sensor 생성·ContactListener·break 시 해제. | ≈ 30분 |
| R3a | D4 조회 API: `CollideOrientedBox`, `FindBody`, `BuildKinematicSegments`(호출자 없음 또는 진단 로그만). | ≈ 30분 |
| R3b | D4·D6 선택: `Rig` 모드 기본값화, 진단 비교 로그. | ≈ 35분 |
| R4a | D5 (a): break 없는 부착 패치 게시 상태·마스크·거절. | ≈ 40분 |
| R4b | D5 (b): 후보 준비 뒤 AR `Request` 연결, 실패 보존, 진단 옵션 `-rig-bone-cut-checks 1`. | ≈ 40분 |
| R5 | 전체 검사(Debug/Release), 상태 줄·README·CodeMap·관련 문서 동기화. | ≈ 20분 |

각 단계: 새 Codex 실행으로 구현 → 다른 새 Codex 실행으로 리뷰(기준 = §6) → Debug/Release 빌드·검사 → 로컬 커밋 → 다음 단계.

## 6. 완료 조건

공통 명령(작업 디렉터리 `MiniEngine/ModelViewer`, Debug·Release, 각각 별도 프로세스, `-debug 1`):
- `-skeletal-patch-offset-copy 1 -skeletal-patch-checks 1` → `%TEMP%/SP6-comparison.log` 이번 실행 20행 `GPU==CPU=PASS`
- `-skeletal-patch-break-checks 1` → SP7b·SP7e PASS 줄
- `-skeletal-break-checks 1` → `SkeletalPhysicsChecks.log`의 `BC-12a PASS`; `-skeletal-break-xbot 1` → `BC-12b PASS`
- `-attached-recut-checks 1` → `%TEMP%/SP6-SkeletalPatchChecks.log`의 AR4 다섯 PASS
- R1a에서 기록한 기준선 대비 회귀가 없어야 한다. 종료 코드만으로 판정하지 않고 PASS 줄을 확인한다. D3D12 ERROR 0.

R1: 일반 실행 로드 로그에 owner별 body/joint 수(Xbot 52/51, CesiumMan 19/18), 생성·등록 ms, 인접/비인접 겹침 쌍 수가 한 번씩 찍힌다. C 입력이 등록된 rig를 재사용한다(두 번째 생성 로그 없음). BC-12b 실행에는 사전 등록 로그가 없다.

R1c: Debug/Release 모두 `JPH_DEBUG_RENDERER`로 빌드된다(Jolt 라이브러리와 엔진 define 일치). 일반 실행에서 H 키를 누르면 Xbot·CesiumMan rig body와 관절이 화면에 선으로 보이고, 다시 누르면 꺼진다. 공통 검사 기준선 유지.

R2: 로드 직후 rig body 전부 sensor, break 뒤 Dynamic subtree body 전부 sensor 아님을 진단 로그로 확인한다. sensor 접촉 수는 break 시연 뒤 값을 기록만 한다(0이어도 실패 아님: 같은 owner 필터·배치에 따라 겹침이 없을 수 있다). 공통 검사 기준선 유지.

R3: `-skeletal-patch-break-checks 1`의 각 평면에서 `Rig` 선택과 `SelectSegmentJoint`(kind=segment인 경우)의 owner·부모/자식 NodeId·끊을 JointId를 함께 로그로 남기고 모두 같아야 한다. 다르면 이유(hit 누락, 끊긴 edge, 동률)를 로그로 남기고 보고한다. 물리 꺼짐에서는 D6 경로로 현행 결과와 같다.

R4: `-rig-bone-cut-checks 1`이 Xbot 머리 중간 평면으로 다음을 판정한다. 관절 break 없음(BreakStatus 변화 없음). node 쪽 조각의 node 상대 변환 오차가 60프레임 동안 1 mm 이하(AR-4 추종 판정과 같은 정의). 반대쪽 KDOP body의 world Y가 게시 프레임 대비 60프레임 뒤 1 cm 이상 낮다. joint 원점 1 mm 이내 평면은 거절되고 component 수·마스크가 변하지 않는다. (b) 실패를 강제한 경우 부착 패치와 마스크가 남는다.

공통 코드 규칙: 새 함수 위 한글 주석 1~2줄, 함수 내부 기능 단락별 한글 주석, 단계당 약 300줄.
