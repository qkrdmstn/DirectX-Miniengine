# Repository Guidelines

## Project Structure & Module Organization
- `MiniEngine/Core/` provides rendering, input, math, and profiling; `MiniEngine/Model/` provides model support.
- `MiniEngine/ModelViewer/` is the main engine demo. Shared build settings live in `MiniEngine/PropertySheets/`, and utilities in `MiniEngine/Tools/`.
- `Samples/Desktop/` contains individual DirectX 12 samples; `TechniqueDemos/` contains larger demonstrations.
- `Libraries/` holds supporting libraries and fallback-layer tests. `Tools/` holds standalone utilities.
- `Assets/` contains repository imagery; runtime textures, models, and HLSL shaders also live beside their consuming projects.
- For any `MiniEngine/MeshCutting/` work (design, implementation, review), consult `MiniEngine/MeshCutting/Docs/NDC25Reference.md`: the NDC25 skeletal-mesh cutting technique this module follows, with each claim marked by its evidence level.

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

## Testing Guidelines
Build affected projects in Debug and Release. Run changed samples with DirectX validation enabled; check rendering, input, and diagnostics. Record GPU, driver, configuration, and reproduction steps.

`Libraries/D3D12RaytracingFallback/src/FallbackLayerUnitTests/` uses Microsoft's C++ Unit Test Framework. Build its project with the required toolset and run through Visual Studio Test Explorer; follow existing `TEST_CLASS`/`TEST_METHOD` patterns. No repository-wide coverage threshold is defined.

## MeshCutting Workflow
- Docs: start from `MiniEngine/MeshCutting/Docs/README.md` (index with status) and open only the documents the task needs; use `CodeMap.md` for structure.
- Every design doc begins with a `상태:` line (설계안 / 진행 중 / 완료, with the commit). Update it, and the README row, when a step lands. Long-lived docs cite function names, not line numbers.
- Keep each implementation step to one concept and about 300 changed lines, then stop so the user can read and commit it before the next step. The user must be able to follow every line.
- After changing `Geometry/`, run the CPU tests in `MiniEngine/MeshCutting/tests/PlaneSlicerTests.cpp` (build command in the file header); keep build output outside the repository.
- When several sessions work at once, design sessions edit only `Docs/`, and only one session writes code at a time.
- At the start of multi-step work, list the steps with a rough time for each, based on measured costs (Codex review about 2-5 minutes, Debug/Release builds, CPU tests), not guesses.

## Commit & Pull Request Guidelines
History uses short, action-oriented subjects such as `Fix reflection hint`; no strict prefix convention is established. Keep commits focused. PRs should explain the change, link relevant issues, list build/runtime checks, and include screenshots for rendering changes. Exclude generated binaries, compiled shaders, package caches, and local IDE settings.
