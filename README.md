# DirectX MiniEngine — GPU Mesh Cutting

DirectX 12 MiniEngine 위에서 **메시를 실시간으로 자르는 기능**을 직접 구현한 프로젝트입니다.
정점 데이터는 GPU 메모리에서만 절단하고, 단면(cap)도 GPU 컴퓨트 셰이더에서 채웁니다.
절단 조각에는 Jolt 물리를 붙이고, 스켈레탈 메시는 관절 단위로 떼어 ragdoll로 넘기는 것이 목표입니다.

📑 **발표 자료**

- 3주차: [Canva 슬라이드](https://canva.link/ycxk54ec6bttauh)
- 4주차: [Canva 슬라이드](https://canva.link/s9wl6s8f998jn9c)

## 주요 기능

| 기능 | 상태 | 설명 |
| --- | --- | --- |
| Static 메시 GPU 절단 | 완료 | 유한 평면으로 메시를 자르고, 단면을 채우고, 잘린 조각을 다시 자를 수 있다 |
| 단면(cap) 병렬 삼각분할 | 완료 | 경계를 GPU에서 이어 붙이고, 귀 자르기(ear clipping)를 라운드 단위로 병렬 처리한다 |
| 절단 조각 물리 | 진행 중 | Jolt v5.6.0으로 평면 후보를 조회하고, 조각마다 rigid body를 만든다 |
| 관절 해제 (BreakConstraint) | 완료 (M1) | 스켈레탈 rig의 관절을 끊고 떨어진 부분을 Dynamic body로 전환한다 |
| Skeletal 메시 절단 | 진행 중 | 평면이 지나는 뼈의 joint를 고르고, 영향 범위 패치를 복사·절단해 붙인다 |

## Static 절단 파이프라인

`.glb`의 정점은 삼각형마다 따로 저장되어 있어, 같은 위치라도 렌더링 정보(uv 등)에 따라 다른 정점입니다.
절단에는 미리 만든 인접 정보가 필요 없습니다. 삼각형은 각자 자르고, 생긴 경계 끝점만 위치로 이어 붙입니다.

| 순서 | 셰이더 | 하는 일 |
| --- | --- | --- |
| 1 | `ClipTrianglesCS` | 삼각형마다 정점 3개만 보고 자른다. 교차점 2개가 절단 선분 1개가 된다 |
| 2 | `BoundaryKeysCS` | 선분 끝점을 평면 위 2D 좌표의 공간 해시(셀 크기 = `Epsilon`)에 넣는다 |
| 3 | `LinkBoundaryCS` | 주변 9셀에서 같은 위치의 끝점을 찾아 선분을 연결한다 |
| 4 | `RankBoundaryCS` | 연결된 선분마다 루프의 대표와 순서를 전파한다 (pointer jumping) |
| 5 | `CapEarsCS` | 루프를 귀 자르기로 채운다 (아래 참고) |
| 6 | `FinalizeCutCS` | 양쪽에 실제 표면이 있는 분할에서 성공한 루프만 cap으로 출력한다 |
| 7 | `CutBoundsCS` | 조각 정점의 18-DOP 경계를 계산한다 (선택·충돌용) |

CPU 쪽에서는 섹션 경계 구가 평면 한쪽에만 있으면 교차 검사 없이 그 섹션을 통째로 한쪽에 배정합니다.

셰이더는 [MiniEngine/MeshCutting/Shaders](MiniEngine/MeshCutting/Shaders)에 있습니다.

### 단면 채우기의 병렬화 (`CapEarsCS`)

한 루프 안에서 여러 귀를 동시에 자르려면, 이웃한 귀가 같은 라운드에 함께 잘리면 안 됩니다.

1. **볼록 후보 표시:** 각 경계 정점이 볼록인지 0/1로 표시한다.
2. **Exclusive Scan:** 표시를 누적합해 후보마다 순번을 매긴다.
3. **교대 선택:** 라운드마다 홀수 순번 또는 짝수 순번 후보만 고른다. 이웃한 후보는 함께 선택되지 않는다.
4. **InTriangle 검사:** 후보마다 스레드 하나가 귀 삼각형 안에 다른 경계 점이 있는지 확인한다. 검사할 점은 라운드 시작 때 목록(`TestList`)으로 압축한다.
5. **동시 제거:** 통과한 귀를 한 번에 잘라 내고 다음 라운드로 넘어간다.

## 성능

Bunny 모델(30,338 삼각형), y=0 평면, Release 빌드의 Static 절단 GPU 시간(중앙값)입니다.

| 환경 | 코드 기준 | 전체 GPU | cap 단계 |
| --- | --- | --- | --- |
| RTX 5060 Ti | `master` (`9f5e93c7`) | 1.71 ms | 1.01 ms |
| RTX 5060 Ti | `cap-active-list` (`a2633711`, 미병합) | 1.12 ms | 0.51 ms |

- GPU 병렬화 이전 CPU 구현은 i5-1135G7에서 Bunny 절단에 88.6 / 94.6 ms가 걸렸다. 측정 구간이 GPU와 달라 참고값이다.
- 측정 조건과 원본 기록은 [CutPerformanceComparison.md](Docs/GPU/CutPerformanceComparison.md), [CapScanMarking.md](Docs/GPU/CapScanMarking.md)에 있다. 1.12 ms 측정은 `cap-active-list` 브랜치의 `Docs/GPU/CapActiveTestList.md`에 기록되어 있다.

## 빌드와 실행

Visual Studio Developer PowerShell(C++ 도구, Windows SDK, v143 toolset)에서 저장소 루트 기준으로 실행합니다.

```powershell
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /restore /m /p:Configuration=Debug /p:Platform=Windows
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Release /p:Platform=Windows
```

```powershell
Set-Location MiniEngine/ModelViewer
../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe
```

Visual Studio에서는 `ModelViewer.slnx`를 열고 ModelViewer를 시작 프로젝트로 지정한 뒤 **F5**로 실행합니다.
모델은 `MiniEngine/ModelViewer/Models`의 `cube`, `bunny`, `dragon`, `Xbot`, `CesiumMan`을 `-model Models/bunny.glb`처럼 지정합니다.

절단 벤치마크는 아래 스크립트로 Release 빌드 후 3회 실행하고, 결과를 `%TEMP%/cut-benchmark/summary.txt`에 모읍니다.

```powershell
powershell -ExecutionPolicy Bypass -File Tools/RunCutBenchmark.ps1
```

### 조작

| 입력 | 동작 |
| --- | --- |
| 왼쪽 클릭 | 객체 또는 조각 선택 |
| `Q` / `W` / `E` / `R` | 보기 / 이동 / 회전 / 크기 기즈모 |
| `L` | World / Local 축 전환 |
| `V` | Solid → Wireframe → Vertices 표시 모드 순환 |
| `C` | 현재 절단 평면으로 자르기 |
| `P` | 물리 실행 / 일시정지 |
| `H` | 충돌 hull과 바닥 표시 |
| `Delete` | 선택한 대상 삭제 |
| `Ctrl+Z` | 마지막 드래그 취소 |
| 오른쪽 버튼 + 마우스 / `WASD` `QE` | 카메라 회전 / 이동 |

전체 조작 계약은 [Scene.md](Docs/Current/Scene.md)에 있습니다.

## 저장소 구조

| 경로 | 내용 |
| --- | --- |
| [MiniEngine/MeshCutting](MiniEngine/MeshCutting) | 절단 평면 기하(`Geometry/`), GPU 절단 작업(`GPU/`), 컴퓨트 셰이더(`Shaders/`), CPU 테스트(`tests/`) |
| [MiniEngine/ModelViewer](MiniEngine/ModelViewer) | 데모 앱. Scene·GameObject·기즈모·절단·물리 시스템(`Scene/`) |
| [MiniEngine/Physics](MiniEngine/Physics) | Jolt 연결, 물리 world, 관절 해제 |
| [Docs](Docs) | 설계·구현·측정 문서. 목차는 [Docs/README.md](Docs/README.md), 코드 지도는 [CodeMap.md](Docs/Current/CodeMap.md) |
| [Tools](Tools) | Jolt 빌드, 절단 벤치마크 스크립트 |

## 참고

- NDC 2025 메시 절단 기법 — 근거 정리: [NDC25Reference.md](Docs/NDC/NDC25Reference.md)
- Unreal Engine `ProceduralMeshComponent` · `SliceProceduralMesh` · `BreakConstraint` 소스 비교: [UnrealSourceComparison.md](Docs/Unreal/UnrealSourceComparison.md)
- Eder, Held, Palfrader (2018) — 병렬 귀 자르기 아이디어: [참고자료.md](Docs/GPU/참고자료.md)
- [Jolt Physics](https://github.com/jrouwe/JoltPhysics) v5.6.0

## 기반 프로젝트와 라이선스

이 저장소는 Microsoft [DirectX-Graphics-Samples](https://github.com/microsoft/DirectX-Graphics-Samples)의 MiniEngine을 기반으로 합니다.
원본 샘플(`Samples/`, `TechniqueDemos/`, `Libraries/`)과 라이선스는 그대로 두었습니다. 라이선스는 [LICENSE](LICENSE)를 따릅니다.
