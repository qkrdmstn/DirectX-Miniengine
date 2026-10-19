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

## Testing Guidelines
Build affected projects in Debug and Release. Run changed samples with DirectX validation enabled; check rendering, input, and diagnostics. Record GPU, driver, configuration, and reproduction steps.

`Libraries/D3D12RaytracingFallback/src/FallbackLayerUnitTests/` uses Microsoft's C++ Unit Test Framework. Build its project with the required toolset and run through Visual Studio Test Explorer; follow existing `TEST_CLASS`/`TEST_METHOD` patterns. No repository-wide coverage threshold is defined.

## Commit & Pull Request Guidelines
History uses short, action-oriented subjects such as `Fix reflection hint`; no strict prefix convention is established. Keep commits focused. PRs should explain the change, link relevant issues, list build/runtime checks, and include screenshots for rendering changes. Exclude generated binaries, compiled shaders, package caches, and local IDE settings.
