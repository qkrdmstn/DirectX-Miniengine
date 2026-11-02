param(
    [int]$Runs = 3,
    [string]$Model = 'Models/bunny.glb',
    [float]$CutY = 0,
    [int]$Cuts = 20,
    [switch]$SkipBuild,
    [string]$OutDir = (Join-Path $env:TEMP 'cut-benchmark'),
    [int]$TimeoutSeconds = 300
)
# Static GPU 절단 벤치마크를 Release로 빌드하고 같은 인수로 여러 번 실행해 결과를 모은다.
# 사용: Tools/RunCutBenchmark.ps1 [-Runs 3] [-SkipBuild] [-Model Models/bunny.glb] [-CutY 0]
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repoRoot 'MiniEngine/Build/x64/Release/Output/ModelViewer/ModelViewer.exe'
$workDir = Join-Path $repoRoot 'MiniEngine/ModelViewer'

# vswhere로 최신 Visual Studio의 MSBuild 경로를 찾는다.
function Get-MSBuildPath {
    # 설치 관리자 폴더의 vswhere가 없으면 PATH의 msbuild를 쓴다.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return 'msbuild' }

    # MSBuild 구성 요소가 있는 최신 설치를 고른다.
    $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if ($path) { return $path }
    return 'msbuild'
}

# 출력 폴더를 만들고 앱 작업 폴더와 무관하도록 절대 경로로 바꾼 뒤 현재 커밋을 기록한다.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$commit = (& git -C $repoRoot rev-parse --short HEAD).Trim()
$dirty = if (& git -C $repoRoot status --porcelain) { ' (uncommitted changes)' } else { '' }

# 지정하지 않으면 ModelViewer 솔루션을 Release로 빌드한다.
if (-not $SkipBuild) {
    $msbuild = Get-MSBuildPath
    $solution = Join-Path $repoRoot 'MiniEngine/ModelViewer/ModelViewer.slnx'
    & $msbuild $solution /restore /m /nologo /v:minimal /p:Configuration=Release /p:Platform=Windows
    if ($LASTEXITCODE -ne 0) { throw "Release build failed (exit $LASTEXITCODE)" }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "ModelViewer.exe not found: $exe" }

# 같은 인수로 벤치마크를 순서대로 실행하고 각 결과 파일을 확인한다.
$results = @()
for ($run = 1; $run -le $Runs; $run++) {
    $outFile = Join-Path $OutDir "run$run.txt"
    Remove-Item -LiteralPath $outFile -ErrorAction SilentlyContinue
    $argList = @('-debug', '0', '-model', "`"$Model`"", '-cut-benchmark', "$Cuts", '-cut-y', "$CutY",
        '-cut-expect-publications', '1', '-cut-benchmark-out', "`"$outFile`"")
    $process = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $workDir -PassThru
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        throw "Run $run timed out after $TimeoutSeconds s"
    }
    if ($process.ExitCode -ne 0) { throw "Run $run exited with code $($process.ExitCode)" }
    if (-not (Test-Path -LiteralPath $outFile)) { throw "Run $run wrote no result" }

    # 결과 파일에서 전체 중앙값과 cap 단계 중앙값을 읽고 형식과 error=0을 확인한다.
    $text = Get-Content -LiteralPath $outFile -Raw
    $gpu = [regex]::Match($text, 'medianGpuMs=([\d.]+) p95GpuMs=([\d.]+) medianReadyMs=[\d.]+ error=(\d+)')
    $cap = [regex]::Match($text, 'cap=([\d.]+)')
    if (-not $gpu.Success -or -not $cap.Success) { throw "Run $run result has unexpected format: $outFile" }
    if ($gpu.Groups[3].Value -ne '0') { throw "Run $run reported error=$($gpu.Groups[3].Value)" }
    $results += [pscustomobject]@{
        Run = $run; MedianGpuMs = $gpu.Groups[1].Value; P95GpuMs = $gpu.Groups[2].Value; CapMs = $cap.Groups[1].Value
    }
    Write-Output "Run $run done"
}

# 커밋·인수·각 실행의 원문을 요약 파일 하나로 묶고 표를 출력한다.
$summary = Join-Path $OutDir 'summary.txt'
$lines = @("Commit: $commit$dirty", "Args: -model $Model -cut-benchmark $Cuts -cut-y $CutY (Release, -debug 0)", '')
for ($run = 1; $run -le $Runs; $run++) {
    $lines += "[run $run]"
    $lines += Get-Content -LiteralPath (Join-Path $OutDir "run$run.txt")
}
$lines += ''
$lines += ($results | Format-Table -AutoSize | Out-String).TrimEnd()
Set-Content -LiteralPath $summary -Value $lines -Encoding UTF8
Get-Content -LiteralPath $summary
Write-Output "`nSaved: $summary"
