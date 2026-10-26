상태: 진행 중 — `feb35f23` 기반 미커밋 Jolt v5.6.0 연결·Static 평면 후보 조회 완료; 동적 조각·Skeletal 물리는 설계 (2026-10-25)

# 절단 조각 물리 도입 제안

## 추천과 범위

사용자와의 후속 논의에 따라 Jolt의 CPU rigid body 경로를 우선 선정한다. 최초 PhysX 추천은 자동 관절 파손 기능에 비중을 높였으나, 현재 목표는 선택한 관절을 절단 시스템이 명시적으로 분리하는 방식이다. Jolt의 body/constraint/ragdoll과 기존 렌더·충돌 분리 구조로 진행한다. 실제 속도·메모리 우열을 측정한 결과는 아니며 CPU 물리 연동과 관리의 단순함을 우선한 판단이다. 현재 Jolt v5.6.0을 v143·DLL CRT·SSE2로 연결했고, 후속 요청에 따라 평면 후보 조회를 먼저 구현했다. 실제 범위·API·검증은 [JoltQueryIntegration.md](JoltQueryIntegration.md)를 따른다. 아래 낙하·convex 분할·본 물리는 장기 계획이다.

| 후보 | 현재 프로젝트에서의 판단 |
| --- | --- |
| PhysX 5 | 표준 관절의 break force/torque 및 파손 이벤트, D6 관절을 이용한 후속 본 body 구성에 적합. CPU부터 도입하고 CUDA는 별도 단계로 둔다. |
| Jolt | 현재 선정안. CPU rigid body/constraint/ragdoll로 조각 낙하·충돌부터 시작한다. 겉/내부 렌더 메시나 고해상도 렌더/저해상도 충돌 표현의 분리는 물리 SDK와 독립적인 엔진 책임이다. |
| Bullet | rigid body/constraint를 제공하는 대안. 현재 저장소에는 Bullet 전용 자산이나 연동이 없으므로 우선 선택할 특별한 결합점은 없다. |

근거: [PhysX 표준 관절과 breakage](https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/Joints.html), [Jolt 구조/shape/COM](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md), [Bullet 공식 저장소](https://github.com/bulletphysics/bullet3). SDK가 메시 절단이나 GameObject 분리를 대신해 주지는 않는다. 향후 본별 body와 Jolt constraint를 구성하고 선택한 관절의 제거/비활성화를 안전한 physics step 경계에서 처리한다. 힘에 의한 자동 파손 정책이 필요해지면 별도 범위로 설계한다.

## 현재 데이터에서 바뀌는 부분

### 채택한 소유·편집 정책

사용자 결정에 따라 먼저 조사한 Unreal의 컴포넌트 중심 방식에 맞춘다. 절단 때 GameObject를 자동 분리하는 제안은 기본 정책으로 채택하지 않는다. 근거와 버전 범위는 [UnrealSourceComparison의 절단 물리 조사](../Unreal/UnrealSourceComparison.md#7-절단-이후-물리와-컴포넌트-구성-조사-2026-10-25)를 따른다.

- **Static 절단:** 기존 GameObject 안의 A MeshComponent를 교체하고 B MeshComponent를 추가한다. A/B는 각각 Transform과 독립 rigid body를 갖는다. 물리 이동은 GameObject 전체 Transform이 아닌 각 컴포넌트에 반영하며 동적 조각은 WorldIndependent로 배치한다.
- **외부 사용 방식:** 메시의 물리 활성화 API가 내부 body/collider 참조를 생성·관리한다. 사용자가 매번 별도 충돌 컴포넌트를 수동 부착해야 하는 흐름은 기본으로 삼지 않는다. PhysicsWorld의 SDK 수명 소유 경계는 유지한다.
- **Skeletal:** 하나의 MeshComponent도 본별 body 여러 개와 constraints를 가질 수 있다. GPU 렌더 절단으로 A/B 영역을 나눌 때는 같은 GameObject 안에서 여러 MeshComponent로 관리할 수 있다. 이는 우리의 후속 GPU 확장 설계이며 Unreal SliceProceduralMesh가 Skeletal 절단을 제공한다는 뜻이 아니다.
- **pose 연결:** 같은 owner의 ModelInstance를 사용하되, 본별 animation/physics pose 합성과 각 렌더 영역의 binding을 명시한다. 잘린 팔을 단일 강체로 둘지 여러 본 body로 유지할지는 후속 단계에서 구분한다. 공유 애니메이션을 그대로 두는 것만으로 분리 body의 자세가 반영되지는 않는다.
- **개별 편집:** 현재 GameObject 단위 선택·기즈모·Undo는 유지된 구현 범위다. 후속 편집 단계에서 `(ObjectId, MeshId)` 선택과 컴포넌트 Transform history, 드래그 중 body 제어·해제 정책을 추가한다. 물리 데모 초기에는 아래의 owner 편집 제한을 따른다.
- **별도 GameObject 추출:** 독립 수명·게임 로직이 필요한 때 명시적인 기능으로 검토한다. 지금의 기본 절단 동작에는 넣지 않는다. SDK는 선정한 Jolt를 사용하며 Unreal의 API 전체나 Chaos backend를 복제하지 않는다.

현재 `ColliderComponent`는 `Id`, `MeshId`, `GeometryRevisionId`, `QueryHandle`을 보관한다. 조회 전용 Box/body는 PhysicsWorld가 소유하고 PhysicsSystem이 자동 등록한다. 동적 body와 정밀 충돌 proxy는 아직 없다. 아래 구조도는 후속 simulation 단계의 목표이며 SDK 포인터를 Scene의 여러 곳에 직접 저장하지 않는다.

```text
GameObject
 ├─ Transform
 ├─ ModelInstance                     공유 원본/애니메이션 pose
 ├─ MeshComponent A                   현재 렌더 geometry/pose
 ├─ MeshComponent B                   현재 렌더 geometry/pose
 ├─ RigidBodyComponent A              MeshId A, PhysicsBodyHandle
 ├─ RigidBodyComponent B              MeshId B, PhysicsBodyHandle
 └─ ColliderComponent들               BodyId, MeshId, CollisionRevisionId

PhysicsWorld                          실제 SDK scene/actor/shape 수명 소유
CollisionGeometry                     닫힌 단순 충돌 형상·CPU revision
```

첫 범위에서는 독립 조각 MeshComponent 하나에 rigid body 하나를 대응한다. 같은 body의 여러 collider는 compound shape로 묶는다. 두 조각은 같은 GameObject에 있어도 독립 body로 움직인다. GameObject 전체 Transform을 움직여 한 조각의 물리를 표현하지 않는다. 향후 본 body는 별도 bone binding으로 확장한다.

평면의 양쪽 A/B와 연결된 물리 조각 수가 항상 같은 것은 아니다. Bunny 같은 오목한 모델은 한쪽에 여러 독립 섬이 생길 수 있다. 첫 Cube에서는 side당 body 하나로 검증하고, 이후 독립 섬을 따로 움직여야 할 때 MeshComponent/body 단위를 더 나누는 단계를 추가한다. 떨어진 섬들을 한 compound body로 묶으면 함께 움직인다는 점을 구현 범위에 명시한다.

## 렌더 메시와 충돌 형상

- 렌더: 현재 GPU Data/Pose/IB를 계속 사용한다. 물리 도입 때문에 매 프레임 렌더 정점을 CPU로 복사하지 않는다.
- 충돌: Cube는 Box/닫힌 convex polyhedron, Bunny는 단순화한 convex hull 또는 여러 hull의 compound를 별도 데이터로 둔다. 하나의 hull은 오목한 곳을 메우므로 시각 메시와 정확히 같지는 않다.
- 열린 Bunny도 닫힌 충돌 proxy를 쓰면 질량/관성을 정의할 수 있다. 열린 렌더 삼각형 적분을 물리 부피로 해석하지 않는다.
- Jolt의 동적 절단 조각에는 ConvexHullShape/compound를 우선한다. MeshShape는 대부분 정적 geometry 용도로 사용하며 동적으로 사용할 때의 충돌 조합·질량/관성 제약을 별도로 확인해야 한다. 비교 참고로 PhysX에도 SDF가 있는 dynamic triangle mesh 경로가 있으므로 '모든 동적 triangle mesh는 무조건 불가능'이라고 일반화하지 않는다. [Jolt 형상/질량 문서](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md), [PhysX 비교 자료](https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/RigidBodyCollision.html)

권장 정상 경로는 CPU의 작은 충돌 polyhedron도 GPU 렌더 절단과 같은 component-local 평면으로 자르는 것이다. convex를 평면으로 자르면 각 결과도 convex이므로 전체 렌더 메시의 복잡한 단면 연결을 반복할 필요가 없다. compound이면 구성 hull마다 잘라 각 side에 모은다. 최초 충돌 proxy는 원본 자산으로부터 별도로 준비/저장한다. 이 경로는 렌더 절단을 CPU로 되돌리는 설계가 아니다.

렌더 결과에서 충돌체를 새로 만들어야 하는 대안은 GPU 완료 fence → 필요한 위치/인덱스의 비동기 readback → worker에서 hull/decomposition/cook → revision 재검사다. DX12 UAV가 SDK shape로 바로 변환되는 것은 아니다. PhysX GPU 경로는 CUDA이며 DX12 Compute와 자동 공유되는 경로가 아니다. [GPU backend 문서](https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/GPURigidBodies.html)

## 절단 순간의 데이터 이동

1. 현재 body/mesh revision과 평면을 고정한다. 평면은 renderer와 collision proxy의 동일한 component-local 좌표로 옮긴다.
2. GPU는 렌더 A/B를, CPU worker는 작은 collision proxy A/B를 준비한다. 원본 자산과 기존 shape는 준비 중 유지한다.
3. owner/mesh/revision이 유효한지 재검사한다. GPU 렌더와 CPU proxy가 모두 유효한 A/B 분할을 내야 commit한다. 한쪽만 NoSplit/실패하면 둘 다 버리고 부모 render revision/body를 유지한다. proxy에도 동일한 월드 단위 epsilon과 유한 사각형 경계 정책을 적용한다. 렌더보다 큰 proxy의 경계만 사각형 밖으로 나가도 첫 구현에서는 전체를 거절하고 이유를 알린다. 물리 고정 step 경계에서 렌더 A/B와 body/shape A/B를 하나의 변경으로 게시한다.
4. 각 조각의 질량중심·질량·관성은 닫힌 충돌 형상으로 계산한다. 부모 body의 선속도와 각속도를 물려주되 새 질량중심 속도는 `v_child = v_parent + omega_parent × (COM_child - COM_parent)`로 정한다. 일정 density와 proxy 보존을 기준으로 질량 합을 검사한다.
5. 매 step 물리의 위치·회전을 해당 MeshComponent에 반영한다. 현재 0.6 강제 분리 이동은 사용하지 않는다. 분리 연출은 이후 명시적인 impulse로 구현한다.

작은 body/shape를 바꾸는 동안 simulate/fetch/query와 충돌하지 않도록 안전한 step 경계에서 교체한다. 오래된 cooking 완료는 현재 revision과 다르면 버린다. 첫 구현은 조각 하나의 부분 게시가 생기지 않도록 A/B를 함께 준비/교체한다.

### 움직이는 대상을 자를 때의 주의점

현 `CuttingSystem`은 요청 이후 Wc 또는 component StateVersion이 바뀌면 GPU 결과를 거절한다. 물리가 매 프레임 MeshComponent Transform을 바꾸면 이 검사가 거의 항상 취소시킬 수 있다. 그대로 연결하지 않는다.

첫 통합 검증은 정지/kinematic Cube를 대상으로 한다. 움직이는 절단을 추가할 때는 사용자의 geometry/scale 편집 세대와 physics rigid-pose 세대를 구분한다. 순수한 body 이동이면 local 절단 결과를 최신 body pose에 배치하고, source geometry/편집 변경이면 취소한다. 그 전의 임시 프로토타입에서 대상을 고정한다면 고정/속도 복구를 명시적 transaction으로 관리한다. 일괄 후보 큐의 world snapshot도 같은 정책으로 갱신해야 한다.

### 좌표와 질량중심

body의 위치는 mesh 원점과 다를 수 있다. `BodyFromMesh`를 mesh-local에서 body-local로 가는 고정 변환으로 정의하고 `WorldFromMesh = WorldFromBody * BodyFromMesh`로 렌더 배치를 복원한다. 질량중심을 mesh 원점으로 착각해 재절단 때 조각이 튀지 않도록 한다.

여기서 Body는 선택한 Jolt body pose API의 프레임으로 명시하고 질량중심 프레임과 구분한다. body 원점과 center-of-mass 관련 API 및 shape의 자동 recentering을 실제 SDK에서 확인한 뒤 BodyFromMesh를 정의한다. 렌더 변환에는 body↔mesh 대응을, 속도 전달에는 실제 world COM을 사용해 COM 보정을 두 번 적용하지 않는다.

물리 body는 translation/rotation을 시뮬레이션하며 scale은 충돌 형상에 굽는다. 단위는 1 world unit=1m로 정하고 자산의 시연용 정규화 scale과 구분한다. owner 비균일 scale과 component 회전이 만든 전단은 물리 rigid pose로 그대로 표현할 수 없다. 첫 구현에서는 전단을 검출해 물리 활성화를 제한하거나 렌더/충돌 모두 동일한 기준 프레임으로 bake하는 처리가 필요하다. scale만 조용히 버리는 방식은 사용하지 않는다. 동적 조각은 WorldIndependent 배치를 우선해 owner 편집으로 body와 렌더가 어긋나지 않게 한다.

## 후속 simulation 파일과 책임

아래는 최초 장기 설계의 역할 분담이다. PhysicsWorld·PhysicsSystem·빌드 연결은 현재 조회 범위로 구현됐으며, 실제 함수는 JoltQueryIntegration을 따른다. 이 표의 Step·질량·cook·transaction API가 모두 구현됐다는 뜻은 아니다.

| 파일 | 주요 타입/함수와 역할 |
| --- | --- |
| `MiniEngine/Physics/PhysicsWorld.h/.cpp` | `PhysicsWorld`: SDK 초기화/종료, `Step`, body 생성/삭제, overlap/raycast, handle→owner/component 매핑 |
| `MiniEngine/Physics/PhysicsTypes.h` | `PhysicsBodyHandle`, motion/layer/material 설명. Scene 헤더에 SDK 타입을 노출하지 않음 |
| `MiniEngine/Physics/CollisionGeometry.h/.cpp` | `CollisionGeometry`: component-local CPU convex proxy·revision. SDK shape 캐시는 PhysicsWorld 내부 소유 |
| `MiniEngine/Physics/CollisionCooker.h/.cpp` | `CollisionCooker`: proxy 검증·SDK cook·질량 특성, worker 완료 결과 |
| `MiniEngine/Physics/CollisionProxySlicer.h/.cpp` | `SplitConvex`: 같은 로컬 평면으로 작은 convex/compound를 A/B로 분할 |
| `MiniEngine/ModelViewer/Scene/RigidBodyComponent.h` | body handle, 대상 MeshId, body↔mesh frame, motion mode |
| `MiniEngine/ModelViewer/Scene/PhysicsSystem.h/.cpp` | Scene↔PhysicsWorld 동기화, fixed-step accumulator, 완료된 교체 적용 |
| `MiniEngine/ModelViewer/Scene/PhysicsCutTransaction.h/.cpp` | cut request/revision, renderer/collider A/B 준비 상태와 일괄 commit/cancel |
| 기존 `ComponentId.h`, `Scene.*`, `CuttingSystem.*` | collider/body 참조와 절단 transaction 연결. SDK 수명은 PhysicsWorld에 위임 |

## 구현 순서와 완료 기준

1. **SDK/바닥/큐브:** CPU PhysicsWorld + fixed 60Hz + 정적 바닥 + 동적 Box. 낙하·정지·수명·Debug/Release 검증.
2. **컴포넌트 동기화:** 같은 GameObject의 두 MeshComponent가 독립 body로 움직이는지 확인. 질량중심 offset과 scale, 삭제/scene clear 검증.
3. **정지 Cube 절단 통합:** 렌더·convex A/B를 함께 교체하고 질량 합·body count·원위치·속도 전달 검증. 절단 후 재절단도 동일 transaction 사용.
4. **Bunny compound/움직이는 절단:** collision proxy 품질과 cook 지연을 계측하고 physics-pose/편집 세대 분리, 오래된 결과 취소·빠른 조각 CCD를 검증.
5. **Skeletal body/joint:** 본별 body와 Jolt constraint/ragdoll, 애니메이션/물리 pose 혼합, 명시적 관절 분리를 기존 GPU patch 절단과 연결. 메시 절단과 관절 파손은 별도 책임으로 유지.

각 단계는 한 개념·약 300줄 단위로 나누고 독립 리뷰·Debug/Release·실행 검증을 수행한다. 현재 저장소의 측정 비용은 리뷰 약2~5분, 변경 범위에 따라 빌드 약20~60초 이상이며 SDK 첫 빌드 비용은 아직 측정하지 않았다. 장면 내 성능 우열과 런타임 collision cook 지연은 실제 연동 후 측정해야 한다.

## 2026-10-25 코드·공식 소스 대조 결과

이 절은 최초 계획 조사 당시의 기록이다. 이후 사용자 지시로 SDK 연결·후보 조회를 구현했고 커밋은 하지 않았다. 아래 직접 Scene을 순회하던 RequestAll은 현재 QueryCutCandidates 호출로 교체됐다. 과거 CPU 절단 구현을 현재 코드로 간주하지 않는다.

- `ModelViewer::Update` 시작의 `CuttingSystem::Update`가 지금은 결과를 직접 게시한다. 입력 처리 뒤 `Scene::Update` → `GraphicsContext::Finish` → `CuttingSystem::SubmitPending` 순서다. 완료 수집과 실제 게시를 분리할 지점이 여기다.
- `MeshComponent::SetTransform`은 호출마다 `m_StateVersion`을 증가시키고 준비 상태를 무효화한다. `CuttingSystem::Update`는 revision·StateVersion과 Wc의 바이트 동일성을 검사한다. 현재 API 그대로 물리 자세를 매 프레임 넣으면 지연 절단을 취소한다.
- `GameObject::RemoveMeshComponent`는 collider 참조만 지운다. body 제거 경로는 새로 필요하다. 외부 매핑 키에는 owner 범위인 ComponentId만 쓰지 않고 `(ObjectId, ComponentId)`를 사용한다.
- 현재 `SetGeometry`와 B 추가는 자원 할당을 수반한다. 기존 A/B 게시 코드를 단순히 body 생성 뒤 호출하는 것만으로는 실패 시 전체 복구를 보장하지 못한다. 아래 8·9단계에서 게시용 자원을 먼저 준비한다.
- 설치된 MSBuild는 17.14.60, MSVC 디렉터리는 14.44.35207, CMake는 3.26.0-rc6이다. x64 프로젝트의 유효 toolset 설정은 v143이며 `.slnx`의 Windows는 x64로 매핑된다. 공통 CRT는 Debug `/MDd`, Profile·Release `/MD`다. 실제 신규 Jolt 빌드 호환성은 아직 실행 검증 전이다.

## SDK 고정과 빌드 연결 결정

**선정 버전은 Jolt v5.6.0, commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`다.** 조회 시 공식 최신 릴리스로 표시된다. master를 추적하지 않는다. MIT 라이선스와 고정 버전 정보를 함께 보관한다. SDK 결합의 단순함과 요구 충족에 따른 선정이며 다른 SDK 대비 성능 우위를 뜻하지 않는다. [공식 릴리스](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.6.0), [고정 커밋](https://github.com/jrouwe/JoltPhysics/commit/e77f175595e64cb44218cc9d9d56fc365ad0e36a)

의존성은 **고정 소스 + 공식 CMake로 빌드한 정적 Jolt.lib**로 연결했다. 전체 엔진을 CMake로 전환하지 않는다. 실제 `MiniEngine/Physics/CMakeLists.txt`가 해시 검증된 소스를 공식 `Build/` subdirectory로 포함한다. archive SHA256은 실제 다운로드에서 확인해 고정했다. 소스·라이브러리·설정별 생성물은 기존 ignore 대상 `MiniEngine/Build/ThirdParty/Jolt/` 아래에 둔다. 기존 vcpkg baseline은 유지한다.

| 설정 | 최초 도입값 / 확인 기준 |
| --- | --- |
| 생성기·플랫폼 | Visual Studio 17 2022, `-A x64 -T v143`; 설치된 toolchain으로 구성하고 버전 로그 저장 |
| 구성 | Jolt Debug↔엔진 Debug, Jolt Release↔엔진 Release·Profile; Profile 연결도 smoke build, ARM64 물리 활성화는 후속 범위 |
| CRT·언어 | `USE_STATIC_MSVC_RUNTIME_LIBRARY=OFF`, `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`; Physics adapter는 C++17 |
| 라이브러리·최적화 | `JPH_BUILD_SHARED_LIBS=OFF`, `INTERPROCEDURAL_OPTIMIZATION=OFF`; 초기 ABI/링크 확인 뒤 LTO는 별도 측정 |
| 좌표·레이어 | `DOUBLE_PRECISION=OFF`, `OBJECT_LAYER_BITS=16`; float 변환 검사와 Static/Moving 충돌 필터 두 계층부터 시작 |
| CPU 기능 | `USE_SSE4_1`, `USE_SSE4_2`, `USE_AVX`, `USE_AVX2`, `USE_AVX512`, `USE_LZCNT`, `USE_TZCNT`, `USE_F16C`, `USE_FMADD` 모두 OFF로 SSE2 기준. 이후 실제 CPU 지원·성능 측정으로 변경 |
| 선택 기능 | `JPH_USE_DX12/VK/MTL/CPU_COMPUTE=OFF`; rigid body의 CPU 실행과 Compute backend는 별개. SDK sample/viewer는 빌드하지 않음 |
| 진단·공개 정의 | 초기 SDK debug renderer/profiler/object stream·부동소수 예외 옵션은 OFF, Debug 기본 assert는 유지. CMake Jolt target의 공개 정의/옵션을 adapter에도 동일 적용 |

옵션 이름·기본값 근거: [v5.6.0 CMake 설정](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Build/CMakeLists.txt). 언어·공개 compile 정의 근거: [Jolt target](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Jolt.cmake), [빌드 설명](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Build/README.md). adapter는 Jolt target에 CMake로 링크해 이 설정을 상속받고, Scene 쪽에는 Jolt 헤더를 노출하지 않는다.

MSBuild 연결은 작은 `MiniEngine/Physics/Physics.vcxproj`를 Makefile/NMake bridge로 두고 `.slnx`와 ModelViewer의 ProjectReference에 등록한다. 이 bridge는 `Tools/BuildJolt.ps1`을 통해 Jolt와 adapter를 구성별로 빌드한다. ModelViewer용 `PropertySheets/Physics.props`는 구성별 adapter/Jolt lib 경로만 제공한다. bridge의 프로젝트 참조는 빌드 순서를 보장하고 정적 lib의 최종 링크는 props가 명시한다. 공식 source·고정 설정이 바뀔 때만 재구성하며 매 프레임·매 소스 파일 컴파일 때 다운로드하지 않는다. CMake 공개 설정을 수작업으로 Scene 프로젝트에 복제하지 않는다.

## 좌표·수명·프레임 계약 확정안

### body 원점과 COM

최초 구현은 `BodyInterface::GetPositionAndRotation`의 **shape 생성 원점 기준 body pose**를 렌더 복원에 사용한다. `GetCenterOfMassPosition`은 속도 전달과 질량 검증에만 사용한다. Jolt 내부 COM 중심 재배치가 있어도 입력 hull 정점을 별도로 COM만큼 빼지 않는다. [Body 실제 구현](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Body/Body.h), [BodyInterface](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Body/BodyInterface.h)

전단이 없는 `Wmesh = T * R * S`라면 충돌 정점에 S를 굽고 body에는 T/R만 준다. 이때 `BodyFromMesh=S`, `WorldFromMesh=WorldFromBody*BodyFromMesh`다. off-center 조각의 shape COM은 Jolt가 계산하며, body 원점 pose를 읽은 뒤 COM을 다시 빼지 않는다. `BodyFromMesh`는 이 고정 affine 대응을 뜻하며 scale 없는 COM frame과 혼동하지 않는다. engine/Jolt 벡터·quaternion은 성분별로 변환하고 메모리 reinterpret는 하지 않는다.

원본 proxy는 원본 모델의 node/source-frame을 반영한 **현재 component-local canonical 좌표**에 작성한다. Cube의 실제 모델 경계·node를 확인해 8개 꼭짓점을 만들며 임의의 단위 Box를 렌더 Cube와 같다고 가정하지 않는다. 이후 절단 proxy는 이 좌표를 유지하고 shape cook 직전에 고정 scale을 굽는다. 월드 epsilon, 유한 사각형의 꼭짓점/축도 GPU 요청과 같은 변환으로 비교한다.

최초 지원은 양수 scale과 전단 없는 affine이다. owner 비균일 scale×component 회전의 전단은 기저 직교 검사로 거절하고 이유를 표시한다. WorldIndependent 전환도 TRS 분해·재합성 오차 검사 후 한 번만 수행한다. Jolt의 자동 scale 근사로 전단을 조용히 없애지 않는다. [공식 COM·scale 계약](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Docs/Architecture.md)

### 소유권과 프레임 경계

- `PhysicsWorld`는 Jolt runtime 초기화, Factory/RegisterTypes, temp allocator, job pool, PhysicsSystem, shape refs와 body handle table을 소유한다. 내부 handle은 index+generation이며 Scene은 SDK 포인터를 보관하지 않는다. `Initialize/Shutdown`, `CreateBody/DestroyBody`, `GetBodyPose/GetBodyVelocity`, `Step`부터 제공한다. query·contact 이벤트 UI는 필요한 단계에 추가한다. 초기화/제거 순서는 [공식 HelloWorld](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/HelloWorld/HelloWorld.cpp)를 따른다.
- `PhysicsSystem`은 Scene과 PhysicsWorld의 매핑을 조정한다. GameObject의 RigidBodyComponent는 MeshId·handle·고정 frame만, ColliderComponent는 BodyComponentId·CollisionRevisionId 등 참조만 둔다. SDK shape cache는 PhysicsWorld 내부에 두고 CPU `CollisionGeometry`에는 SDK 타입을 넣지 않는다.
- 프레임은 입력/명시적 편집 → 완료 수집 → 변경 transaction 검사·게시 → fixed step → body pose를 메시로 반영 → 기존 Scene.Update/Finish → 새 절단 제출 → 렌더 순서다. 모든 step은 처음에는 메인 스레드에서 완료를 기다리며 worker callback은 Scene을 수정하지 않는다. SDK 내부 병렬 실행은 job pool이 맡는다.
- fixed dt는 1/60초, 최대 4 substep/frame을 초기 정책으로 둔다. 긴 중단의 누적 시간은 clamp하고 버린 시간을 로그로 남긴다. 첫 단계는 최신 완료 pose를 그대로 표시하고 렌더 보간은 후속으로 둔다. `m_AnimationStepMs`를 물리 시간으로 사용하지 않는다.
- 메시/객체 삭제와 Clear는 다음 simulation 전에 stale 매핑을 제거한다. 종료는 pending cut/cook 정리 → body 제거 → Scene.Clear → PhysicsWorld.Shutdown → renderer 종료다. 미완료 작업은 Scene 포인터 대신 ID/revision과 불변 입력만 보유한다.
- 기존 Gizmo/Undo는 owner 편집이므로 dynamic body에 그대로 적용하지 않는다. 첫 물리 데모에서 물리 owner의 편집은 상태 안내와 함께 제한하고, kinematic 편집·순간이동/Undo의 body 반영은 별도 단계로 구현한다. 진단 `cut-gap` 이동도 물리 활성 시 거절한다.

## 리뷰 단위별 구현 계획

아래 각 행은 최초 simulation 로드맵의 독립 변경 단위다. 한 개념·약 300줄의 리뷰 단위를 유지한다. 이번 조회 구현은 사용자가 단계별 자체 리뷰 후 최종 완료까지 진행하도록 지시해 계속 수행했으며 커밋하지 않았다. 새 함수/수정 함수 위에는 한글 역할 주석을 작성하고 source/project/filter 등록도 변경량에 포함한다.

시간 표기 `R+B`는 기존 측정에 근거한 리뷰 2~5분 + Debug/Release 각 빌드 약20~60초 이상, 즉 반복 검토 비용 약3~7분 이상이다. **구현 시간·SDK 첫 빌드·새 CPU 테스트·런타임 시나리오 시간은 미측정**이며 총 작업 시간으로 오해하지 않는다. 각 단계에서 실측을 기록해 다음 예상에 사용한다.

| 순서 / 한 개념 | 새 파일·주요 함수(제안) / 기존 영향 | 리뷰·검증 완료 기준 | 대략 검토 비용 |
| --- | --- | --- | --- |
| 1. SDK 재현 빌드 | `Tools/BuildJolt.ps1`, `Physics/ThirdParty/CMakeLists.txt`, 고정 버전·license 정보. `ConfigureJolt/BuildJolt` | hash 고정, Debug/Release CRT·CPU flags 확인, 재실행 증분 빌드, SDK compile 시간 기록 | R 2~5분 + 첫 SDK 빌드 미측정 |
| 2. 엔진 링크 경계 | `Physics/CMakeLists.txt`, `Physics.vcxproj/.filters`, `PhysicsTypes.h`, `PhysicsWorld.h/.cpp`의 초기화·종료만. `Physics.props`, `.slnx`, `ModelViewer.vcxproj/.filters` | SDK 헤더 비노출, Debug/Release 링크·반복 initialize/shutdown, Profile smoke build | R+B + Profile 빌드 미측정 |
| 3. CPU world·Box | `PhysicsWorld::CreateBody/DestroyBody/Step`, 내부 layer filter, `Physics/tests/PhysicsWorldTests.cpp` | floor+Box를 fixed60Hz로 낙하, 접촉·sleep·삭제·재생성, 잘못된 handle 거절, 초기 body/allocator 예산 기록 | R+B + CPU 실행 미측정 |
| 4. 컴포넌트 독립 동기화 | `Scene/RigidBodyComponent.h`, `Scene/PhysicsSystem.h/.cpp`의 `AttachBody/Advance/SyncTransforms/PruneStaleBodies`, `Scene.*`, `ComponentId.h`, `ModelViewer::Startup/Update/Cleanup` | 같은 owner의 두 Cube가 독립 낙하, 한 조각 삭제가 다른 body에 영향 없음, Clear·정상 종료, physics OFF 회귀 | R+B + 런타임 미측정 |
| 5. frame·scale 계약 | `Physics/PhysicsMath.h/.cpp`, `PhysicsSystem::BuildBodyPlacement`, off-center hull cook의 최소 경로, `MeshComponent.*` | 회전·양수 비균일 scale·COM offset 위치 보존, 전단 거절, WorldIndependent 전환, Gizmo/Undo 제한 확인 | R+B + 좌표/실행 미측정 |
| 6. 최초 닫힌 proxy | `Physics/CollisionGeometry.h/.cpp`, `CollisionCooker.h/.cpp`의 `CookConvex`, Cube factory; `PhysicsSystem` 등록 경로 | 모델 node/source frame 대응, 닫힘·양의 부피·density·질량/COM/양의 관성, stale collision revision 거절 | R+B + cook 측정 |
| 7. CPU convex 분할 | `Physics/CollisionProxySlicer.h/.cpp`의 `SplitConvex`, `Physics/tests/CollisionProxyTests.cpp` | 중앙·사선·접촉·공면·유한 영역 초과·반복 절단; A+B volume/COM/inertia 적분 보존과 winding, thin/zero-volume 거절 | R+B + CPU 실행 미측정 |
| 8. GPU 완료의 준비/게시 분리 | `CuttingSystem::CollectCompleted/PreparePublication/CommitPrepared`(제안), `MeshComponent::PrepareGeometryReplacement/CommitGeometryReplacement`, `GameObject`의 게시 준비 | 기존 비물리 절단 결과 동일; B·geometry·pose·컨테이너 사전 준비 실패에서 부모 유지. 기존 진단은 commit 후만 성공 집계 | R+B + GPU 회귀 미측정 |
| 9. 정지 Cube 절단 transaction | `Scene/PhysicsCutTransaction.h/.cpp`의 `Begin/TryPrepare/Commit/Cancel`, `PhysicsSystem::ApplyReadyCuts`, `CuttingSystem`·`ComponentId.h` 연결 | 정지 kinematic 부모→동적 A/B, 렌더/proxy 둘 다 성공 시 함께 교체; 실패 주입·삭제·stale·한쪽 cook 실패에서 부분 게시 없음 | R+B + transaction 실행 미측정 |
| 10. 움직이는 재절단 계약 | `MeshComponent::ApplyPhysicsPose/GetEditVersion/GetPoseVersion`, `CuttingSystem::Candidate/RequestAll/SubmitPending`, transaction snapshot | physics 이동은 허용, geometry/scale/source-frame/사용자 teleport는 취소. request-local 평면 보존, 최신 pose 게시, 대기열도 동일 정책 | R+B + 지연/반복 실행 미측정 |
| 11. 이동 조각 속도 전달 | transaction의 `BuildChildMotion`, `Physics/tests/PhysicsCutTests.cpp`, 진단 로그 | 최신 parent v/ω로 child COM 속도 계산, mass·COM·선/각운동량 보존 오차, 중력·충돌 전후 분리 측정. 빠른 얇은 조각의 LinearCast CCD 비교 | R+B + 동적 실행 미측정 |

4단계의 화면 검증은 기존 Cube 데모의 정합된 Box만 사용하고, 임의 hull·일반 scale 지원을 5·6단계에서 확장한다. 8단계의 함수명은 새 API 제안이며 현재 구현된 함수로 읽지 않는다. 한 변경이 300줄을 넘으면 다음 개념을 섞지 않고 더 나누므로 위 행 수가 최종 커밋 수의 상한은 아니다.

## 절단 transaction의 실제 데이터 흐름

```text
ObjectId + MeshId + RenderRevision + CollisionRevision + EditVersion
    + request-local plane/finite rectangle + 고정 scale
        ├─ 현재 GPU geometry/pose → CutJob → 렌더 revision A/B
        └─ 현재 CPU closed proxy → SplitConvex → CookConvex → shape A/B
둘 다 Ready → ID/세대 재검사 → 안전한 프레임/step 경계에서 일괄 게시
    부모 body 1개 제거 + A/B body 2개 추가 + mesh/collider 참조 교체
fixed Step → 원점 기준 body T/R → 각 WorldIndependent MeshComponent
```

1. 9단계에서는 정지 kinematic 부모만 허용한다. GPU의 `HasSplit`과 CPU의 두 유효 양의 부피 결과가 모두 필요하다. render cap 실패를 허용하던 기존 비물리 정책과 별개로, Cube 통합 검증은 닫힌 cap도 요구한다. CPU proxy와 GPU 렌더 중 한쪽만 split이면 전체 취소한다. 동일 중력에서는 두 조각이 나란히 떨어질 수 있으므로 회전한 Cube의 사선 절단·바닥 접촉으로 독립 회전을 확인한다. 임의 gap 이동은 사용하지 않는다.
2. `CutJob::BuildResults` 결과와 CPU proxy/shape를 transaction이 강한 참조로 유지한다. cook 완료 뒤 owner/mesh·render/collision revision·편집 세대를 다시 확인한다. 일반 도형의 worker cooking은 immutable 요청을 받아 결과만 큐로 넘긴다. Cube 최초 cook은 작으므로 동기로 시작하고 비용을 먼저 측정한다.
3. 새 body는 world에 Add하기 전에 둘 다 생성하고, 새 B·A 교체용 PosedGeometry·Scene container capacity·ID를 준비한다. 기존 A/body는 준비 실패 시 보존한다. commit 구간에는 할당·검증 실패 가능 작업을 남기지 않고 swap/이동과 body 등록만 수행한다. Jolt body capacity 부족도 사전 생성 단계에서 잡는다. 예약된 ID를 취소 후 재사용하지 않는다.
4. 부모 제거·두 body 추가·Scene 참조 교체 사이에는 query/step/render가 실행되지 않게 한다. 초기값·속도는 등록 전에 준비한다. 이 의미의 일괄 게시이며 SDK가 Scene까지 포함한 transaction API를 제공하는 것은 아니다.
5. 10단계부터는 요청 시점의 local 평면·유한 rectangle·world epsilon 대응을 후보마다 고정한다. 순수 physics T/R 변경은 편집 세대를 바꾸지 않는다. GPU 제출이 늦어져도 요청 당시 물체의 어느 부분을 잘랐는지 유지하도록 frozen local 정보를 사용하며, 필요하면 최신 Wc로 world 입력을 재구성하는 adapter를 둔다. 현재 Wc로 원래 world 평면을 다시 local 변환해 다른 부위를 자르지 않는다.
6. 게시 순간 최신 body T/R·v·ω를 읽어 두 결과를 배치한다. `v_child = v_parent + ω × (COM_child − COM_parent)`를 적용하고 density를 유지한다. 질량 합은 닫힌 proxy에서 계산하며 관성 합 비교는 평행축 정리로 같은 기준점에 맞춘다. 양쪽 side당 body 하나가 첫 범위이며 island 자동 분리는 하지 않는다.
7. 충돌 hull의 convex radius는 최초 보존 검증에서 0으로 고정하고 hull tolerance를 자산 scale에 맞춰 명시한다. 근접 cap을 가진 형제 body의 접촉·초기 solver 반응을 측정한다. 이후 radius 변경은 질량/접촉 회귀를 재검증한다. `ConvexHullShape`의 최대 hull vertex 256과 퇴화·cook 실패를 결과 상태로 반환하며 임의 geometry를 성공으로 근사하지 않는다. [고정 버전 hull API](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Collision/Shape/ConvexHullShape.h)

## Cube 다음의 범위와 검증 기록

- **Bunny compound**: `CollisionCooker::CookCompound`와 별도 proxy asset/loader를 추가한다. 첫 hull은 시각 근사임을 표시하고, 다중 hull은 질량 이중 계산을 피하도록 비중첩 체적 분할 기준을 둔다. 각 hull을 자르고 side별 compound로 모으며, 독립 island는 우선 한 body에 묶인다. 개별 낙하가 필요하면 connected-island 분석과 MeshComponent/body 추가 분리를 다음 변경으로 한다. 원본 render는 열린 메시여도 collision proxy는 닫혀 있어야 한다.
- **Skeletal**: 본→body frame, joint anchor/constraint, animation→kinematic 구동, ragdoll pose blend, 명시적 constraint 제거, GPU skeletal patch 절단의 연결을 각각 약300줄의 별도 변경으로 설계한다. 현재 `CuttingSystem::Submit`이 skin section을 거절하므로 물리 body만 추가해 skeletal 절단이 완성되지는 않는다. 이 단계에 들어갈 때만 NDC 근거 등급과 현행 GPU pose 경로를 추가 조사한다.
- **두 렌더 표현**: 겉피부·내부 section/material과 정밀 렌더 메시를 GeometryRevision에 유지하고, 물리는 단순 CPU proxy를 별도 revision으로 추적한다. 정점별 매 프레임 readback은 추가하지 않는다. GPU 결과 기반 proxy 생성이 필요하면 비동기 readback·worker cook·예산·취소 책임을 별도 측정 단계로 둔다.
- 매 코드 단계는 Debug/Release 빌드와 D3D12 validation을 실행한다. CPU 검증은 GPU 없이 프레임 변환·proxy 보존·수명·stale/실패 경계를 검사하고, 런타임은 floor/낙하/수동 C/마우스/정상 종료를 직접 확인한다. 자동 캡처 종료만으로 입력·정상 종료를 검증했다고 쓰지 않는다.
- 이전 기준은 NVIDIA MX450 / driver 32.0.15.9186이다. 신규 실행에서는 실제 GPU·driver·configuration·모델·fixed dt·seed·CLI·body 수·mass/COM 오차·step/cook/request→commit 시간과 로그 위치를 함께 기록한다. CPU/SDK 첫 빌드와 runtime 비용은 이때 처음 측정한다.
- 기존 절단 회귀는 `-model Models/cube.glb -cut 4 -cut-repeat-plane 1 -cut-step-y .25 -cut-validate 1 -cut-expect-publications 4 -cut-gap 0` 등 현재 옵션으로 실행한다. 물리 진단 옵션은 구현 단계에서 새로 정의하며 지금 존재한다고 가정하지 않는다.
- AGENTS의 `MiniEngine/MeshCutting/tests/PlaneSlicerTests.cpp`는 현재 checkout에 없다. 현재 있는 것은 `CutPlaneTests.cpp`, `GpuCutValidation.cpp`다. `Geometry/`를 수정하게 되면 현행 CutPlaneTests와 관련 proxy 테스트를 외부 출력 디렉터리에서 실행하고 누락된 legacy 테스트 지침을 함께 정리한다. 이번 계획에서는 Geometry 변경이 없다.

후속 사용자 요청에 따라 SDK 고정·재현 빌드와 엔진 링크를 넘어 **Static collider 자동 등록·평면 후보 조회**까지 우선 완료했다. 이 완료 범위와 리뷰·검증은 JoltQueryIntegration에 기록했다. Cube가 잘리고 실제로 낙하하는 simulation 완료점은 위 9단계이며 움직이는 재절단·Bunny compound·본 절단은 그 이후다.

개별 파편 기즈모 편집은 위 물리 구현과 분리된 후속 변경이다. Scene 관련 설계를 다시 확인한 뒤 선택 대상 → 컴포넌트 기즈모/Undo → 물리 body 동기화를 각각 약300줄의 리뷰 단위로 나눈다. body 없이 먼저 A만 이동·회전하고 B가 유지되는지 확인한 다음, 물리 활성 상태의 드래그·취소·Undo를 검증한다. 단계별 비용은 기존 리뷰·빌드 기준을 적용하고 입력 검증 시간은 실측한다.
