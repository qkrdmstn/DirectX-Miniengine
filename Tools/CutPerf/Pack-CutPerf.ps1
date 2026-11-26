<#
.SYNOPSIS
  Packages the Release cut-perf runner and its runtime assets for another Windows PC.
.PARAMETER OutZip
  Destination zip (default: %TEMP%\CutPerfPackage-<timestamp>.zip).
#>
param(
    [string]$OutZip = (Join-Path $env:TEMP "CutPerfPackage-$(Get-Date -Format 'yyyyMMdd-HHmmss').zip")
)

# 빌드 출력과 작업 에셋 경로를 확인한다.
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$output = Join-Path $repo 'MiniEngine\Build\x64\Release\Output\ModelViewer'
$assets = Join-Path $repo 'MiniEngine\ModelViewer'
$OutZip = [IO.Path]::GetFullPath($OutZip)
$stage = [IO.Path]::GetFullPath((Join-Path $env:TEMP "CutPerfStage-$([guid]::NewGuid().ToString('N'))"))
$repoPrefix = $repo.TrimEnd('\') + '\'
foreach ($path in $OutZip, $stage) {
    if ($path -eq $repo -or $path.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package destination and staging directory must be outside the repository: $path"
    }
}

# 필수 실행 파일과 런타임의 누락을 모두 모아 압축 전에 보고한다.
$missing = @('ModelViewer.exe', 'WinPixEventRuntime.dll' | ForEach-Object {
    $source = Join-Path $output $_
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        $source
    }
})
if ($missing.Count) {
    throw "Required build files missing: $($missing -join ', ')"
}
if (Test-Path -LiteralPath $OutZip) {
    throw "Destination already exists: $OutZip"
}
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    # 실행 파일과 출력 폴더 바로 아래 DLL만 포함한다.
    $bin = Join-Path $stage 'bin'
    $work = Join-Path $stage 'work'
    $models = Join-Path $work 'Models'
    New-Item -ItemType Directory -Path $bin, $models | Out-Null
    Copy-Item -LiteralPath (Join-Path $output 'ModelViewer.exe') -Destination $bin
    foreach ($file in (Get-ChildItem -LiteralPath $output -File | Where-Object Extension -EQ '.dll')) {
        Copy-Item -LiteralPath $file.FullName -Destination $bin
    }

    # 선택적 D3D12 폴더도 바로 아래 DLL만 상대 경로를 보존한다.
    $d3d12 = Join-Path $output 'D3D12'
    if (Test-Path -LiteralPath $d3d12 -PathType Container) {
        foreach ($file in (Get-ChildItem -LiteralPath $d3d12 -File | Where-Object Extension -EQ '.dll')) {
            $target = Join-Path $bin 'D3D12'
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target
        }
    }

    # Startup의 기본 장면 다섯 모델과 원본 GLB 진단 입력을 함께 담는다.
    foreach ($name in 'cube', 'bunny', 'dragon', 'Xbot', 'CesiumMan') {
        foreach ($extension in '.glb', '.mini') {
            $source = Join-Path $assets "Models\$name$extension"
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                throw "Required scene asset missing: $source"
            }
            Copy-Item -LiteralPath $source -Destination $models
        }
    }

    # LoadIBLTextures가 시작 시 열거하는 환경 맵을 동일하게 제공한다.
    $textures = Join-Path $assets 'Textures'
    $imageExtensions = @('.dds', '.hdr', '.png', '.jpg', '.jpeg', '.ktx', '.ktx2', '.bmp', '.tga', '.tif', '.tiff', '.gif', '.exr')
    foreach ($file in (Get-ChildItem -LiteralPath $textures -File -Recurse)) {
        if ($file.Extension -notin $imageExtensions) {
            Write-Host "Skipped texture: $($file.FullName)"
            continue
        }
        $target = Join-Path $work ('Textures\' + $file.FullName.Substring($textures.Length + 1))
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Run-CutPerf.ps1') -Destination $stage
    $readme = @'
CutPerf 다른 PC 측정 안내

요구 사항: Windows 10/11 x64, DirectX 12 지원 GPU와 최신 제조사 드라이버,
Microsoft Visual C++ 2015-2022 Redistributable (x64).
개발 도구와 git은 필요 없습니다. 압축을 모두 풀고 로컬 쓰기 가능한 폴더에서 실행하세요.
노트북은 전원을 연결하고 다른 GPU 작업을 종료하세요.

압축을 푼 폴더에서 PowerShell을 열어 실행:
powershell -ExecutionPolicy Bypass -File .\Run-CutPerf.ps1 -G0 -N 3 -NoOpen
powershell -ExecutionPolicy Bypass -File .\Run-CutPerf.ps1 -G0 -N 3 -NoOpen -ExtraArgs "-island-gpu 1"

기본 실행 파일은 bin\ModelViewer.exe, 작업 폴더는 work입니다.
실행 파일은 Windows 시스템 D3D12 런타임을 사용합니다.
bin\D3D12 폴더는 있을 때만 동봉되며 실행에 필요하지 않습니다.
-ExePath와 -WorkDir로 경로를 바꿀 수 있고 -ExtraArgs는 모든 실행 끝에 추가됩니다.
-OutDir C:\CutPerfResults\baseline 처럼 결과 경로를 지정할 수 있습니다.
기본 결과는 이 폴더 아래 out\<실행시각>에 저장됩니다.
측정 중 창이 숨겨지며 여러 번 실행되므로 완료까지 기다리세요.
실패하면 CutPerfResult.txt의 FAILED/INCOMPLETE와 runs.txt를 확인하세요.

돌려보낼 파일: OutDir 폴더 전체를 zip으로 압축해 보내주세요.
CutPerfResult.txt, machine.txt, commands.txt, runs.txt, CSV 및 raw 로그를 모두 포함하세요.
machine.txt에는 CPU/RAM/GPU/드라이버/OS, 실행 파일 정보와 실제 명령행이 기록됩니다.
'@
    [IO.File]::WriteAllText((Join-Path $stage 'README-CutPerf.txt'), $readme, [Text.UTF8Encoding]::new($true))

    # 저장소 밖 기본 목적지에 압축하고 최종 크기를 표시한다.
    New-Item -ItemType Directory -Force -Path (Split-Path $OutZip) | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $OutZip)
    $zip = Get-Item -LiteralPath $OutZip
    Write-Host ("Package: {0} ({1:N2} MiB, {2} bytes)" -f $zip.FullName, ($zip.Length / 1MB), $zip.Length)
} finally {
    # 직접 생성한 임시 폴더가 TEMP 내부인지 확인한 뒤 정리한다.
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    $stagePath = [IO.Path]::GetFullPath($stage)
    if (-not $stagePath.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected staging path: $stagePath"
    }
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
