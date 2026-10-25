상태: 설계안 — 기준 `4bc5cdf9` 이후 Jolt CPU 물리 도입으로 선정 갱신; 다중 MeshComponent/GPU 절단 연동 계획 (미구현, 2026-10-25)

# 절단 조각 물리 도입 제안

## 추천과 범위

사용자와의 후속 논의에 따라 Jolt의 CPU rigid body 경로를 우선 선정한다. 최초 PhysX 추천은 자동 관절 파손 기능에 비중을 높였으나, 현재 목표는 선택한 관절을 절단 시스템이 명시적으로 분리하는 방식이다. Jolt의 body/constraint/ragdoll과 기존 렌더·충돌 분리 구조로 진행할 수 있다. 실제 속도·메모리 우열을 측정한 결과는 아니며, CPU 물리 연동과 관리의 단순함을 우선한 판단이다. SDK 설치나 코드 연동은 아직 하지 않았다. 도입 때 검증한 SDK tag/commit, MSVC v143, Debug/Release CRT를 고정한다.

| 후보 | 현재 프로젝트에서의 판단 |
| --- | --- |
| PhysX 5 | 표준 관절의 break force/torque 및 파손 이벤트, D6 관절을 이용한 후속 본 body 구성에 적합. CPU부터 도입하고 CUDA는 별도 단계로 둔다. |
| Jolt | 현재 선정안. CPU rigid body/constraint/ragdoll로 조각 낙하·충돌부터 시작한다. 겉/내부 렌더 메시나 고해상도 렌더/저해상도 충돌 표현의 분리는 물리 SDK와 독립적인 엔진 책임이다. |
| Bullet | rigid body/constraint를 제공하는 대안. 현재 저장소에는 Bullet 전용 자산이나 연동이 없으므로 우선 선택할 특별한 결합점은 없다. |

근거: [PhysX 표준 관절과 breakage](https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/Joints.html), [Jolt 구조/shape/COM](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md), [Bullet 공식 저장소](https://github.com/bulletphysics/bullet3). SDK가 메시 절단이나 GameObject 분리를 대신해 주지는 않는다. 향후 본별 body와 Jolt constraint를 구성하고 선택한 관절의 제거/비활성화를 안전한 physics step 경계에서 처리한다. 힘에 의한 자동 파손 정책이 필요해지면 별도 범위로 설계한다.

## 현재 데이터에서 바뀌는 부분

현재 `ColliderComponent`는 `Id`, `MeshId`, `GeometryRevisionId`뿐이며 실제 shape/body가 없다. 이 대응 관계를 유지하고 SDK 포인터를 Scene의 여러 곳에 직접 저장하지 않는다.

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
CollisionGeometry                     닫힌 단순 충돌 형상, CPU 데이터/SDK 캐시
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

## 제안 파일과 책임 (아직 생성하지 않은 코드)

| 파일 | 주요 타입/함수와 역할 |
| --- | --- |
| `MiniEngine/Physics/PhysicsWorld.h/.cpp` | `PhysicsWorld`: SDK 초기화/종료, `Step`, body 생성/삭제, overlap/raycast, handle→owner/component 매핑 |
| `MiniEngine/Physics/PhysicsTypes.h` | `PhysicsBodyHandle`, motion/layer/material 설명. Scene 헤더에 SDK 타입을 노출하지 않음 |
| `MiniEngine/Physics/CollisionGeometry.h/.cpp` | `CollisionGeometry`: component-local CPU convex proxy·revision·SDK shape 캐시 |
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
