param(
    [string]$Cases = 'bunny:0,dragon:0,dragon:0.04,dragon:0.08',
    [int]$Runs = 3,
    [int]$Cuts = 20,
    [switch]$SkipBuild,
    [string]$OutDir = (Join-Path $env:TEMP 'cap-benchmark')
)
# 사용: Tools/RunCapBenchmark.ps1 -SkipBuild -Runs 3 -Cuts 20
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$culture = [System.Globalization.CultureInfo]::InvariantCulture

# 실행별 cap 시간을 정렬해 짝수 개일 때도 중앙값을 구한다.
function Get-Median
{
    param([double[]]$Values)

    # 가운데 두 값의 평균으로 홀수와 짝수 표본을 함께 처리한다.
    $sorted = @($Values | Sort-Object)
    if (-not $sorted.Count) { throw 'No cap samples' }
    return ($sorted[[int][Math]::Floor(($sorted.Count - 1) / 2)] + $sorted[[int][Math]::Floor($sorted.Count / 2)]) / 2
}

# 인수와 모든 사례를 먼저 검사하고 출력 폴더와 커밋 정보를 준비한다.
if ($Runs -lt 1 -or $Cuts -lt 1) { throw 'Runs and Cuts must be positive' }
$caseList = @($Cases.Split(','))
foreach ($case in $caseList)
{
    if ($case -notmatch '^[\w-]+:-?\d+(?:\.\d+)?$') { throw "Invalid case: $case" }
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$commit = & git -C $repoRoot rev-parse --short HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot read commit' }
$status = & git -C $repoRoot status --porcelain
if ($LASTEXITCODE -ne 0) { throw 'Cannot read worktree status' }
$dirty = if ($status) { ' (dirty)' } else { '' }

# 첫 사례만 빌드하고 각 사례의 원본 결과 파일을 모은다.
$rows = @()
$caseStats = @()
for ($index = 0; $index -lt $caseList.Count; $index++)
{
    $case = $caseList[$index]
    $name, $y = $case.Split(':')
    $caseDir = Join-Path $OutDir "${name}_y$y"
    & (Join-Path $PSScriptRoot 'RunCutBenchmark.ps1') -Runs $Runs -Cuts $Cuts -Model "Models/$name.glb" -CutY ([float]::Parse($y, $culture)) -OutDir $caseDir -SkipBuild:($SkipBuild -or $index -gt 0)

    # 필수 줄을 모두 검사하고 숫자는 문화권과 무관하게 읽는다.
    $caseRows = @()
    for ($run = 1; $run -le $Runs; $run++)
    {
        $text = Get-Content -LiteralPath (Join-Path $caseDir "run$run.txt") -Raw
        $gpu = [regex]::Match($text, '(?m)^Cut benchmark: samples=\d+ medianGpuMs=([\d.]+) p95GpuMs=([\d.]+) medianReadyMs=[\d.]+ error=0\r?$')
        $stages = [regex]::Match($text, '(?m)^Cut stages median ms: clip=([\d.]+) boundary=([\d.]+) cap=([\d.]+) finalize=([\d.]+)\r?$')
        $cap = [regex]::Match($text, '(?m)^Cut cap: medianMs=([\d.]+) p95Ms=([\d.]+) minMs=([\d.]+) loops=(\d+) maxLoop=(\d+) mappedLoops=(\d+) rounds=(\d+)\r?$')
        $gpuName = [regex]::Match($text, '(?m)^GPU: (.+?)\r?$')
        if (-not ($gpu.Success -and $stages.Success -and $cap.Success -and $gpuName.Success)) { throw "Invalid result: $case run $run" }
        $caseRows += [pscustomobject]@{
            Case = $case; Run = $run
            MedianGpuMs = [double]::Parse($gpu.Groups[1].Value, $culture); P95GpuMs = [double]::Parse($gpu.Groups[2].Value, $culture)
            Clip = [double]::Parse($stages.Groups[1].Value, $culture); Boundary = [double]::Parse($stages.Groups[2].Value, $culture)
            Cap = [double]::Parse($stages.Groups[3].Value, $culture); Finalize = [double]::Parse($stages.Groups[4].Value, $culture)
            MedianMs = [double]::Parse($cap.Groups[1].Value, $culture); P95Ms = [double]::Parse($cap.Groups[2].Value, $culture)
            MinMs = [double]::Parse($cap.Groups[3].Value, $culture); Loops = [uint32]$cap.Groups[4].Value
            MaxLoop = [uint32]$cap.Groups[5].Value; MappedLoops = [uint32]$cap.Groups[6].Value
            Rounds = [uint32]$cap.Groups[7].Value; GPU = $gpuName.Groups[1].Value
        }
    }

    # 사례별 cap 중앙값과 Mapped 경로 사용 여부를 보관한다.
    $rows += $caseRows
    $caseStats += [pscustomobject]@{
        Case = $case; Model = $name; Cap = Get-Median $caseRows.Cap
        Mapped = @($caseRows | Where-Object { $_.MappedLoops -gt 0 }).Count -gt 0
    }
}

# 커밋·GPU·인수와 모든 실행을 넓은 표로 출력하고 같은 내용을 저장한다.
$gpuNames = ($rows.GPU | Select-Object -Unique) -join ', '
$lines = @("Commit: $commit$dirty | GPU: $gpuNames | Args: Cases=$Cases Runs=$Runs Cuts=$Cuts SkipBuild=$SkipBuild OutDir=$OutDir", '')
$columns = 'Case', 'Run', 'MedianGpuMs', 'P95GpuMs', 'Clip', 'Boundary', 'Cap', 'P95Ms', 'MinMs', 'Finalize', 'Loops', 'MaxLoop', 'MappedLoops', 'Rounds'
$lines += ($rows | Format-Table -Property $columns -AutoSize | Out-String -Width 4096).TrimEnd()
foreach ($stat in $caseStats)
{
    $lines += ('Case {0}: cap median={1:F4} ms' -f $stat.Case, $stat.Cap)
}

# 같은 모델의 Mapped 없는 사례 중 가장 작은 cap 중앙값과 차이를 비교한다.
foreach ($stat in @($caseStats | Where-Object { $_.Mapped }))
{
    $baseline = @($caseStats | Where-Object { $_.Model -eq $stat.Model -and -not $_.Mapped } | Sort-Object Cap)
    $lines += if ($baseline.Count)
    {
        'Mapped {0}: cap - non-mapped min ({1}) = {2:F4} ms' -f $stat.Case, $baseline[0].Case, ($stat.Cap - $baseline[0].Cap)
    }
    else { "Mapped $($stat.Case): no non-mapped case for model $($stat.Model)" }
}
Set-Content -LiteralPath (Join-Path $OutDir 'cap-summary.txt') -Value $lines -Encoding UTF8
$lines
