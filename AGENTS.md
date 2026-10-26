# Repository Guidelines

## Project Structure & Module Organization
- `MiniEngine/Core/` provides rendering, input, math, and profiling; `MiniEngine/Model/` provides model support.
- `MiniEngine/ModelViewer/` is the main engine demo. Shared build settings live in `MiniEngine/PropertySheets/`, and utilities in `MiniEngine/Tools/`.
- `Samples/Desktop/` contains individual DirectX 12 samples; `TechniqueDemos/` contains larger demonstrations.
- `Libraries/` holds supporting libraries and fallback-layer tests. `Tools/` holds standalone utilities.
- `Assets/` contains repository imagery; runtime textures, models, and HLSL shaders also live beside their consuming projects.

## Build, Test, and Development Commands
Use Visual Studio Developer PowerShell with C++ tools and the Windows SDK. MiniEngine uses v143; use Visual Studio/MSBuild supporting `.slnx`. Legacy projects may require older toolsets.

From the repository root:

```powershell
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /restore /m /p:Configuration=Debug /p:Platform=Windows
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Release /p:Platform=Windows
```

These build the viewer and dependencies. The solution's `Windows` platform maps to project `x64`. Restore required NuGet packages; `vcpkg.json` declares additional DirectX dependencies.

Open this solution, select ModelViewer as the startup project, and press **F5**. `Profile` enables instrumentation. For Debug command-line execution:

```powershell
Set-Location MiniEngine/ModelViewer
../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe
```

## Coding Style & Naming Conventions
Match surrounding C++ and HLSL: four-space indentation, braces on separate lines, PascalCase types/functions, and existing `m_` member and `g_` global prefixes. Preserve license headers and local spacing. No repository-wide formatter or linter configuration is provided. Register source/shader files in the relevant `.vcxproj` and `.filters` files.
- 함수를 추가하거나 수정할 때는 함수 바로 위에 역할을 설명하는 한글 주석을 1~2줄 작성한다.
- 함수 내부도 기능 단위로 단락을 나눈다. 단락 사이에 빈 줄을 두고 각 단락 위에 그 단락이 하는 일을 한 줄 한글 주석으로 적는다. 한 줄에 여러 문장을 몰아 쓰지 않는다.

## Testing Guidelines
Build affected projects in Debug and Release. Run changed samples with DirectX validation enabled; check rendering, input, and diagnostics. Record GPU, driver, configuration, and reproduction steps.

`Libraries/D3D12RaytracingFallback/src/FallbackLayerUnitTests/` uses Microsoft's C++ Unit Test Framework. Build its project with the required toolset and run through Visual Studio Test Explorer; follow existing `TEST_CLASS`/`TEST_METHOD` patterns. No repository-wide coverage threshold is defined.

## MeshCutting Workflow
- 아래 표에서 실제 변경 기능·작업 범위에 해당하는 문서만 읽는다. 공통 진입 파일인 `MiniEngine/ModelViewer/ModelViewer.cpp`도 파일명만으로 문서를 모두 읽지 않고 변경 기능에 맞는 행을 선택한다.
- 문서 파일 편집은 편집 대상과 직접 참조하는 문서만 읽는다. 문서를 찾을 때만 `Docs/README.md`의 필요한 항목을 확인한다. README·NDC 문서의 일괄 필독이나 History 일괄 읽기는 요구하지 않는다.

| 코드 / 작업 범위 | 필독 문서 (저장소 루트 기준) |
| --- | --- |
| `MiniEngine/MeshCutting/Geometry/CutPlane.*`, `MiniEngine/ModelViewer/Scene/CutPlaneSystem.*`, `CutPlaneRenderer.*`, `Scene/Shaders/CutPlane*`의 평면 기하·편집·표시 | [Current/CutPlaneDesign.md](Docs/Current/CutPlaneDesign.md) |
| `MiniEngine/ModelViewer/Scene/`의 Scene, GameObject, ObjectSelection, PickHit, Transform, Gizmo, History 및 해당 입력·소유권·변환 작업 | [Current/Scene.md](Docs/Current/Scene.md)의 관련 절 |
| `MiniEngine/Model/Renderer*`, ConstantBuffers, Shaders의 Solid/Wireframe/Vertices 표시 모드 작업 | [Current/RenderModes.md](Docs/Current/RenderModes.md) (표시 모드만 변경할 때 Scene.md 추가 필독 없음) |
| GPU 절단 버퍼·스키닝·출력 설계 | [GPU/GPUCuttingSystemDesign.md](Docs/GPU/GPUCuttingSystemDesign.md); GPU 논문·삼각분할 조사일 때만 [GPU/참고자료.md](Docs/GPU/참고자료.md) 추가 |
| NDC 본 패치·스켈레탈 절단 기법 | [NDC/NDC25Reference.md](Docs/NDC/NDC25Reference.md)의 근거 등급 확인; `NDC/History/`는 해당 알고리즘의 역사 추적 시에만 선택 |
| Unreal 소스·동작 비교 | [Unreal/UnrealSourceComparison.md](Docs/Unreal/UnrealSourceComparison.md); 정렬 설계 검토 시에만 [Unreal/UnrealAlignmentDesign.md](Docs/Unreal/UnrealAlignmentDesign.md) 추가 |
| 전체 구조 조사 | [Current/CodeMap.md](Docs/Current/CodeMap.md) |

- Every design doc begins with a `상태:` line (설계안 / 진행 중 / 완료, with the commit). Update it, and the README row, when a step lands. Long-lived docs cite function names, not line numbers.
- Keep each implementation step to one concept and about 300 changed lines, then stop so the user can read and commit it before the next step. The user must be able to follow every line.
- After changing `Geometry/`, run the CPU tests in `MiniEngine/MeshCutting/tests/PlaneSlicerTests.cpp` (build command in the file header); keep build output outside the repository.
- When several sessions work at once, design sessions edit only `Docs/`, and only one session writes code at a time.
- At the start of multi-step work, list the steps with a rough time for each, based on measured costs (Codex review about 2-5 minutes, Debug/Release builds, CPU tests), not guesses.

## Commit & Pull Request Guidelines
History uses short, action-oriented subjects such as `Fix reflection hint`; no strict prefix convention is established. Keep commits focused. PRs should explain the change, link relevant issues, list build/runtime checks, and include screenshots for rendering changes. Exclude generated binaries, compiled shaders, package caches, and local IDE settings.
