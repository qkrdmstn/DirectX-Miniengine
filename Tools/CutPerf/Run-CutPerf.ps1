<#
.SYNOPSIS
  Single-bone cut performance campaign for ModelViewer (Xbot / CesiumMan thigh cut).

.DESCRIPTION
  Runs the Release ModelViewer.exe repeatedly with the -cut-perf-* options, writes raw logs, per-run CSV,
  a summary CSV and ONE self-contained text file (CutPerfResult.txt) and opens it in Notepad.
  Needs only this repo and a Release build (MiniEngine\Build\x64\Release\Output\ModelViewer\ModelViewer.exe).

.PARAMETER N
  Number of launches per cut configuration and model (default 10).
.PARAMETER RenderRuns
  Number of launches per render before/after configuration and model (default 3, each ~30 s).
.PARAMETER OutDir
  Output folder (default: Tools\CutPerf\out\<timestamp> under the repo).
.PARAMETER ExePath
  Override the ModelViewer.exe path.
.PARAMETER Models
  Xbot, CesiumMan, CesiumMan-UpperArm, Xbot-MultiThigh, CesiumMan-Spine.
.PARAMETER G0
  Quiet GPU timers OFF/ON thigh cuts plus one launch per upper-arm, multi-thigh and spine scene.
.PARAMETER SkipRender
  Skip the render before/after runs.
.PARAMETER NoOpen
  Do not open the result file in Notepad.
#>
param(
    [int]$N = 10,
    [int]$RenderRuns = 3,
    [string]$OutDir = "",
    [string]$ExePath = "",
    [string[]]$Models = @('Xbot', 'CesiumMan'),
    [switch]$G0,
    [switch]$SkipRender,
    [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $ExePath) { $ExePath = Join-Path $repo 'MiniEngine\Build\x64\Release\Output\ModelViewer\ModelViewer.exe' }
if (-not (Test-Path $ExePath)) { throw "ModelViewer.exe not found: $ExePath (build Release|x64 first)" }
$workDir = Join-Path $repo 'MiniEngine\ModelViewer'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot "out\$stamp" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$rawDir = Join-Path $OutDir 'raw'
New-Item -ItemType Directory -Force -Path $rawDir | Out-Null

# ---------------------------------------------------------------- model definitions
$modelInfo = @{
    'Xbot'      = @{ Demo = 1; Bone = 'mixamorig:RightUpLeg'; Second = 'mixamorig:LeftUpLeg'; CutTime = '8.3' }
    'CesiumMan-UpperArm' = @{ Demo = 2; Bone = 'Skeleton_arm_joint_L__4_'; Second = ''; CutTime = '0'; Asset = 'CesiumMan' }
    'Xbot-MultiThigh' = @{ Demo = 1; Bone = 'mixamorig:RightUpLeg'; Second = ''; CutTime = '0'; Asset = 'Xbot'; Opt = '-skeletal-multi-cut 1' }
    'CesiumMan-Spine' = @{ Demo = 2; Bone = 'Skeleton_torso_joint_2'; Second = ''; CutTime = '0'; Asset = 'CesiumMan' }
    'CesiumMan' = @{ Demo = 2; Bone = 'leg_joint_R_1';        Second = 'leg_joint_L_1';        CutTime = '8.98' }
}

# ---------------------------------------------------------------- configurations
# Cut configurations: the same launch performs a first (cold) cut and a second (warm) cut of the mirrored bone.
$cutConfigs = @(
    @{ Name = 'cut_quiet_gpuOff_full';   Desc = 'quiet=1 gpuTimers=OFF scene=full';        Opt = '-cut-perf-quiet 1 -cut-perf-gpu 0' },
    @{ Name = 'cut_quiet_gpuOn_full';    Desc = 'quiet=1 gpuTimers=ON  scene=full';        Opt = '-cut-perf-quiet 1 -cut-perf-gpu 1' },
    @{ Name = 'cut_loud_gpuOff_full';    Desc = 'quiet=0 (PatchLog file writes ON) gpuTimers=OFF scene=full'; Opt = '-cut-perf-quiet 0 -cut-perf-gpu 0' },
    @{ Name = 'cut_quiet_gpuOff_only';   Desc = 'quiet=1 gpuTimers=OFF scene=only-target (other objects not drawn, physics bodies removed)'; Opt = '-cut-perf-quiet 1 -cut-perf-gpu 0 -cut-perf-only-target 1' }
)
# Render configurations: long runs, cut at a later animation time so >300 animated frames exist before the cut.
$renderConfigs = @(
    @{ Name = 'render_full'; Desc = 'gpuTimers=ON quiet=1 scene=full, camera focused on target';        Opt = '-cut-perf-quiet 1 -cut-perf-gpu 1 -cut-perf-focus 1' },
    @{ Name = 'render_only'; Desc = 'gpuTimers=ON quiet=1 scene=only-target, camera focused on target'; Opt = '-cut-perf-quiet 1 -cut-perf-gpu 1 -cut-perf-focus 1 -cut-perf-only-target 1' }
)

# G0는 허벅지 quiet 두 설정과 추가 장면 세 개를 측정한다.
if ($G0) {
    $cutConfigs = @($cutConfigs | Select-Object -First 2)
    $Models = @(@($Models) + @('CesiumMan-UpperArm', 'Xbot-MultiThigh', 'CesiumMan-Spine') | Select-Object -Unique)
    $SkipRender = $true
}

# ---------------------------------------------------------------- run helper (kills only its own process)
# 선택한 장면을 실행하고 종료 상태와 로그를 확인한다.
function Invoke-Run([string]$Tag, [string]$Model, [string]$ConfigOpt, [bool]$Render) {
    # 장면별 본과 기존 데모 옵션을 명령행으로 구성한다.
    $mi = $modelInfo[$Model]
    $log = Join-Path $rawDir "$Tag.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    $a = @('-debug', '0', '-skeletal-break-demo', "$($mi.Demo)", '-skeletal-break-demo-bone', $mi.Bone)
    if ($Render) {
        $a += @('-skeletal-break-demo-delay', '7', '-skeletal-break-demo-cut-time', $mi.CutTime, '-physics-query-exit', '1550')
    } else {
        $a += @('-skeletal-break-demo-delay', '2', '-physics-query-exit', '480')
        if ($mi.Second) { $a += @('-cut-perf-second-bone', $mi.Second) }
    }
    if ($mi.Opt) { $a += $mi.Opt.Split(' ') }
    $a += $ConfigOpt.Split(' ')
    $a += @('-cut-perf-log', "`"$log`"")

    # 실행 창을 숨기고 이 프로세스의 제한 시간과 종료 코드를 확인한다.
    $p = Start-Process -FilePath $ExePath -ArgumentList $a -WorkingDirectory $workDir -WindowStyle Hidden -PassThru
    $null = $p.Handle
    $limit = if ($Render) { 180 } else { 90 }
    if (-not $p.WaitForExit($limit * 1000)) { try { $p.Kill() } catch {}; "$Tag TIMEOUT" | Add-Content (Join-Path $OutDir 'runs.txt'); return $false }
    "$Tag exit=$($p.ExitCode)" | Add-Content (Join-Path $OutDir 'runs.txt')
    return ($p.ExitCode -eq 0 -and (Test-Path $log))
}

# ---------------------------------------------------------------- parsing
# 요청별 CPU 반복 구간의 합계·횟수와 GPU 합계를 읽는다.
function Read-CutLog([string]$Path) {
    # 프레임·요청·구간 로그를 요청 번호에 연결한다.
    $res = @{ Cuts = @{}; Env = @(); Frames = New-Object System.Collections.ArrayList; Start = $null; Rig = @() }

    # 처음 나타난 요청의 빈 집계 표를 만든다.
    function Get-Cut($id) {
        # 기존 요청은 누적 집계 표를 그대로 반환한다.
        if (-not $res.Cuts.ContainsKey($id)) { $res.Cuts[$id] = @{ M = @{}; C = @{}; G = @{}; Cut = $null; Req = 0 } }
        return $res.Cuts[$id]
    }

    # 로그 행의 종류에 따라 프레임·단계·반복 구간을 누적한다.
    foreach ($l in [IO.File]::ReadLines($Path)) {
        if ($l.StartsWith('FRAME ')) {
            $o = @{}
            foreach ($m in [regex]::Matches($l, '(\w+)=([\-\d\.]+)')) { $o[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
            [void]$res.Frames.Add([pscustomobject]$o)
        }
        elseif ($l -match '^MARK id=(\d+) stage=(\S+) t_ms=([\d\.]+) dframe=(\d+)') {
            $c = Get-Cut ([int]$Matches[1]); $c.M[$Matches[2]] = @{ t = [double]$Matches[3]; f = [int]$Matches[4] }
        }
        elseif ($l -match '^CPU id=(\d+) name=(\S+) ms=([\d\.]+) t_end_ms=([\d\.]+)') {
            $c = Get-Cut ([int]$Matches[1]); $name = $Matches[2]
            if (-not $c.C.ContainsKey($name)) { $c.C[$name] = @{ ms = 0.0; count = 0; end = 0.0 } }
            $c.C[$name].ms += [double]$Matches[3]
            $c.C[$name].count++
            $c.C[$name].end = [double]$Matches[4]
        }
        elseif ($l -match '^GPU id=(\d+) name=(\S+) us=([\d\.]+)') {
            $c = Get-Cut ([int]$Matches[1]); $n = $Matches[2]
            if (-not $c.G.ContainsKey($n)) { $c.G[$n] = 0.0 }
            $c.G[$n] += [double]$Matches[3]
        }
        elseif ($l -match '^CUTTER id=(\d+) gpu_ms=([\d\.]+) ready_ms=([\d\.]+) clip=([\d\.]+) boundary=([\d\.]+) cap=([\d\.]+) finalize=([\d\.]+)') {
            $c = Get-Cut ([int]$Matches[1]); $c.Cut = @{ gpu = [double]$Matches[2]; ready = [double]$Matches[3]; clip = [double]$Matches[4]; boundary = [double]$Matches[5]; cap = [double]$Matches[6]; fin = [double]$Matches[7] }
        }
        elseif ($l -match '^REQUEST id=(\d+) frame=(\d+)') { $c = Get-Cut ([int]$Matches[1]); $c.Req = [int]$Matches[2] }
        elseif ($l -match '^DEMOSTART frame=(\d+)') { $res.Start = [int]$Matches[1] }
        elseif ($l.StartsWith('ENV ') -or $l.StartsWith('OPTIONS ')) { $res.Env += $l }
        elseif ($l.StartsWith('RIGPRELOAD ')) { $res.Rig += $l }
    }
    return $res
}

# 게시된 요청의 단계 시간과 세부 CPU 합계·횟수·평균을 반환한다.
function Get-StageMetrics($c) {
    # 미게시 요청은 성공 표에서 제외한다.
    if (-not $c.M.ContainsKey('published')) { return $null }

    # 기존 요청 단계와 새 함수별 구간을 같은 요청에서 계산한다.
    $m = $c.M; $cp = $c.C
    $r = [ordered]@{}
    $r['select_cpu'] = $m['select-done'].t
    $r['extract_submit_cpu'] = $m['extract-submitted'].t - $m['select-done'].t
    $r['extract_gpu_wait'] = $m['extract-polled'].t - $m['extract-submitted'].t
    $r['copy_prepare_cpu'] = $m['copy-prepared'].t - $m['extract-polled'].t
    $r['cut_submit_wait'] = $m['cut-submitted'].t - $m['copy-prepared'].t
    $r['cutter_latency'] = $m['cut-result-received'].t - $m['cut-submitted'].t
    $r['body_submit_cpu'] = $cp['OnCutResult-classify-compact-submit'].ms
    $r['body_gpu_wait'] = $m['finish-start'].t - $cp['OnCutResult-classify-compact-submit'].end
    $r['finish_cpu'] = $cp['FinishCutResult'].ms
    $r['break_wait'] = $m['break-result'].t - $cp['FinishCutResult'].end
    $r['publish_cpu'] = $m['published'].t - $m['break-result'].t
    $r['setmask_cpu'] = $cp['SetTriangleMask'].ms
    $r['TOTAL_request_to_publish'] = $m['published'].t
    $r['frames_spanned'] = $m['published'].f
    if ($c.Cut) {
        $r['cutter_gpu_ms'] = $c.Cut.gpu; $r['cutter_ready_ms'] = $c.Cut.ready
        $r['cutter_clip_ms'] = $c.Cut.clip; $r['cutter_boundary_ms'] = $c.Cut.boundary; $r['cutter_cap_ms'] = $c.Cut.cap; $r['cutter_final_ms'] = $c.Cut.fin
    }

    # 같은 이름은 마지막 값으로 덮어쓰지 않고 합계와 횟수를 보존한다.
    foreach ($name in $cp.Keys) {
        $r["cpu_ms_sum_$name"] = $cp[$name].ms
        $r["cpu_count_$name"] = $cp[$name].count
        $r["cpu_ms_mean_$name"] = $cp[$name].ms / $cp[$name].count
    }

    # GPU 합은 중첩 Extract-mode를 제외하고 전체 패스와 cutter만 더한다.
    if ($c.G.Count) {
        $gpuSum = 0.0
        foreach ($name in $c.G.Keys) { if ($name.EndsWith('-whole')) { $gpuSum += $c.G[$name] / 1000.0 } }
        if ($c.Cut) { $gpuSum += $c.Cut.gpu }
        $r['gpu_sum_ms'] = $gpuSum
    }
    foreach ($g in 'Extract-whole', 'Extract-mode0', 'Extract-mode1', 'Extract-mode2', 'Extract-mode6', 'BodyClassify-whole', 'BodyCompact-whole', 'PatchSkinBuild-whole', 'SourceMaskCompactIB-whole') {
        if ($c.G.ContainsKey($g)) { $r["gpu_us_$g"] = $c.G[$g] }
    }
    return $r
}

function Get-Stats($values) {
    $v = @($values | Where-Object { $_ -ne $null })
    if ($v.Count -eq 0) { return $null }
    $s = $v | Measure-Object -Average -Minimum -Maximum
    return @{ n = $v.Count; mean = $s.Average; min = $s.Minimum; max = $s.Maximum }
}

function Get-RenderWindows($log) {
    # returns windows hold/pre/post of FRAME rows for the first cut
    $c = $log.Cuts[1]
    if (-not $c -or -not $c.M.ContainsKey('published') -or $log.Start -eq $null) { return $null }
    $req = $c.Req; $pub = $req + $c.M['published'].f
    $fr = $log.Frames; $last = $fr[$fr.Count - 1].f
    return @{
        pre  = @($fr | Where-Object { $_.f -ge ($log.Start + 15) -and $_.f -lt ($req - 2) })
        post = @($fr | Where-Object { $_.f -ge ($pub + 150) -and $_.f -le ($last - 3) })
    }
}

# ---------------------------------------------------------------- campaign
$allRows = New-Object System.Collections.ArrayList     # per-run metric rows (Config, Model, Run, Cut, Metric, Value)
$renderRows = New-Object System.Collections.ArrayList  # (Config, Model, Run, Window, Metric, Value)
$envLines = @{}
$rigLines = New-Object System.Collections.ArrayList
$failed = New-Object System.Collections.ArrayList

foreach ($model in $Models) {
    foreach ($cfg in $cutConfigs) {
        $extraScene = $modelInfo[$model].ContainsKey('Asset')
        if ($extraScene -and $cfg.Name -ne 'cut_quiet_gpuOn_full') { continue }
        $launches = if ($extraScene) { 1 } else { $N }
        for ($i = 1; $i -le $launches; $i++) {
            $tag = "{0}-{1}-{2:D2}" -f $cfg.Name, $model, $i
            Write-Host "[cut] $tag"
            if (-not (Invoke-Run $tag $model $cfg.Opt $false)) { [void]$failed.Add($tag); continue }
            $log = Read-CutLog (Join-Path $rawDir "$tag.log")
            foreach ($e in $log.Env) { $envLines[$e] = $true }
            foreach ($rg in $log.Rig) { [void]$rigLines.Add("$model $tag $rg") }
            $expected = if ($modelInfo[$model].Second) { @(1, 2) } else { @(1) }
            foreach ($id in $expected) {
                if (-not $log.Cuts.ContainsKey($id)) { [void]$failed.Add("$tag(cut$id missing)"); continue }
                $r = Get-StageMetrics $log.Cuts[$id]
                if (-not $r) { [void]$failed.Add("$tag(cut$id no publish)"); continue }
                $label = if ($id -eq 1) { 'cold(1st cut)' } else { 'warm(2nd cut)' }
                foreach ($k in $r.Keys) { [void]$allRows.Add([pscustomobject]@{ Config = $cfg.Name; Model = $model; Run = $i; Cut = $label; Metric = $k; Value = $r[$k] }) }
            }
        }
    }
    if (-not $SkipRender -and -not $modelInfo[$model].ContainsKey('Asset')) {
        foreach ($cfg in $renderConfigs) {
            for ($i = 1; $i -le $RenderRuns; $i++) {
                $tag = "{0}-{1}-{2:D2}" -f $cfg.Name, $model, $i
                Write-Host "[render] $tag"
                if (-not (Invoke-Run $tag $model $cfg.Opt $true)) { [void]$failed.Add($tag); continue }
                $log = Read-CutLog (Join-Path $rawDir "$tag.log")
                foreach ($e in $log.Env) { $envLines[$e] = $true }
                $w = Get-RenderWindows $log
                if (-not $w) { [void]$failed.Add("$tag(no windows)"); continue }
                foreach ($wn in 'pre', 'post') {
                    $set = $w[$wn]
                    if ($set.Count -eq 0) { continue }
                    foreach ($m in 'wall_ms', 'cpuUpdate_ms', 'cpuRender_ms', 'upd_gpu', 'ren_gpu', 'depth_gpu', 'shadow_gpu', 'color_gpu', 'skin_cpu', 'skin_gpu') {
                        $avg = ($set | ForEach-Object { $_.$m } | Measure-Object -Average).Average
                        [void]$renderRows.Add([pscustomobject]@{ Config = $cfg.Name; Model = $model; Run = $i; Window = $wn; Metric = $m; Value = $avg; Frames = $set.Count })
                    }
                    $g = ($set | ForEach-Object { $_.upd_gpu + $_.ren_gpu } | Measure-Object -Average).Average
                    [void]$renderRows.Add([pscustomobject]@{ Config = $cfg.Name; Model = $model; Run = $i; Window = $wn; Metric = 'gpu_total(upd+ren)'; Value = $g; Frames = $set.Count })
                    $c = ($set | ForEach-Object { $_.cpuUpdate_ms + $_.cpuRender_ms } | Measure-Object -Average).Average
                    [void]$renderRows.Add([pscustomobject]@{ Config = $cfg.Name; Model = $model; Run = $i; Window = $wn; Metric = 'cpu_total(upd+ren)'; Value = $c; Frames = $set.Count })
                }
            }
        }
    }
}

# ---------------------------------------------------------------- CSV outputs
$allRows | Export-Csv (Join-Path $OutDir 'CutPerfRuns.csv') -NoTypeInformation
$renderRows | Export-Csv (Join-Path $OutDir 'CutPerfRenderRuns.csv') -NoTypeInformation
$summaryCsv = New-Object System.Collections.ArrayList
foreach ($g in ($allRows | Group-Object Config, Model, Cut, Metric)) {
    $f = $g.Group[0]; $s = Get-Stats ($g.Group | ForEach-Object { $_.Value })
    [void]$summaryCsv.Add([pscustomobject]@{ Config = $f.Config; Model = $f.Model; Cut = $f.Cut; Metric = $f.Metric; N = $s.n; Mean = [math]::Round($s.mean, 4); Min = [math]::Round($s.min, 4); Max = [math]::Round($s.max, 4) })
}
foreach ($g in ($renderRows | Group-Object Config, Model, Window, Metric)) {
    $f = $g.Group[0]; $s = Get-Stats ($g.Group | ForEach-Object { $_.Value })
    [void]$summaryCsv.Add([pscustomobject]@{ Config = $f.Config; Model = $f.Model; Cut = "render-$($f.Window)"; Metric = $f.Metric; N = $s.n; Mean = [math]::Round($s.mean, 4); Min = [math]::Round($s.min, 4); Max = [math]::Round($s.max, 4) })
}
$summaryCsv | Export-Csv (Join-Path $OutDir 'CutPerfSummary.csv') -NoTypeInformation

# ---------------------------------------------------------------- text report
$sb = New-Object System.Text.StringBuilder
function Add-Line([string]$s = '') { [void]$sb.AppendLine($s) }

Add-Line '=== CutPerfResult (single-bone cut performance) ==='
Add-Line "generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')   script: Tools\CutPerf\Run-CutPerf.ps1   N=$N RenderRuns=$RenderRuns"
Add-Line ''
Add-Line '--- PC ---'
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    Add-Line "CPU: $($cpu.Name.Trim())  cores=$($cpu.NumberOfCores) logical=$($cpu.NumberOfLogicalProcessors) maxMHz=$($cpu.MaxClockSpeed)"
    $os = Get-CimInstance Win32_OperatingSystem
    Add-Line "OS: $($os.Caption) $($os.Version) build $($os.BuildNumber)   RAM: $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GiB"
    foreach ($v in (Get-CimInstance Win32_VideoController)) {
        Add-Line "GPU(WMI): $($v.Name)  driver=$($v.DriverVersion)  adapterRAM=$([math]::Round($v.AdapterRAM / 1MB)) MiB(32-bit field)"
    }
    $bat = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
    if ($bat) { Add-Line "Battery present: status=$($bat.BatteryStatus) (1=discharging, 2=AC) charge=$($bat.EstimatedChargeRemaining)%" } else { Add-Line 'Battery: none detected' }
    $plan = (powercfg /getactivescheme) 2>$null
    if ($plan) { Add-Line "Power plan: $plan" }
} catch { Add-Line "PC info error: $($_.Exception.Message)" }
Add-Line 'GPU used by engine (from log ENV):'
foreach ($e in ($envLines.Keys | Sort-Object)) { Add-Line "  $e" }
Add-Line ''
Add-Line '--- Build / repo ---'
try {
    $head = (& git -C $repo rev-parse HEAD 2>$null)
    $dirty = (& git -C $repo status --porcelain 2>$null | Measure-Object).Count
    Add-Line "git HEAD: $head   uncommitted files: $dirty"
} catch { Add-Line 'git: not available' }
Add-Line "exe: $ExePath   ($((Get-Item $ExePath).LastWriteTime))"
Add-Line 'All runs: -debug 0 (D3D12 debug layer off), no debugger, V-Sync default (ON, frame interval pinned to monitor refresh).'
Add-Line ''
Add-Line '--- Configurations (options appended to: -debug 0 -skeletal-break-demo <1|2> -skeletal-break-demo-bone <bone>) ---'
foreach ($m in $Models) { $mi = $modelInfo[$m]; Add-Line "$m : demo=$($mi.Demo) bone=$($mi.Bone) secondBone(warm cut)=$($mi.Second) extraOptions=$($mi.Opt) renderCutTime=$($mi.CutTime)s" }
foreach ($cfg in $cutConfigs) { Add-Line "$($cfg.Name): $($cfg.Desc)`r`n    cut runs: -skeletal-break-demo-delay 2 -physics-query-exit 480 -cut-perf-second-bone <second> $($cfg.Opt)" }
foreach ($cfg in $renderConfigs) { Add-Line "$($cfg.Name): $($cfg.Desc)`r`n    render runs: -skeletal-break-demo-delay 7 -skeletal-break-demo-cut-time <renderCutTime> -physics-query-exit 1550 $($cfg.Opt)" }
Add-Line ''
Add-Line '--- Model stats (from Models\*.glb JSON) ---'
foreach ($m in $Models) {
    try {
        $asset = if ($modelInfo[$m].Asset) { $modelInfo[$m].Asset } else { $m }
        $b = [IO.File]::ReadAllBytes((Join-Path $workDir "Models\$asset.glb"))
        $len = [BitConverter]::ToUInt32($b, 12)
        $j = [Text.Encoding]::UTF8.GetString($b, 20, $len) | ConvertFrom-Json
        $verts = 0; $idx = 0; $prims = 0
        foreach ($mesh in $j.meshes) { foreach ($p in $mesh.primitives) {
            $prims++; $pc = $j.accessors[$p.attributes.POSITION].count; $verts += $pc
            if ($p.indices -ne $null) { $idx += $j.accessors[$p.indices].count } else { $idx += $pc } } }
        $skinJoints = 0; foreach ($s in $j.skins) { $skinJoints += $s.joints.Count }
        Add-Line ("{0}: file={1} B nodes={2} meshes={3} primitives={4} skins={5} skinJoints={6} vertices={7} indices={8} triangles={9} animations={10}" -f $m, $b.Length, $j.nodes.Count, $j.meshes.Count, $prims, $j.skins.Count, $skinJoints, $verts, $idx, ($idx / 3), $j.animations.Count)
    } catch { Add-Line "$m : model stats error $($_.Exception.Message)" }
}
$rigSample = $rigLines | Select-Object -First 4
foreach ($r in $rigSample) { Add-Line "  rig: $r" }
Add-Line ''

Add-Line '--- How to read the CUT tables ---'
Add-Line 'All values in ms unless the metric name says _us (microseconds). Columns: n = number of launches, mean/min/max over the launches.'
Add-Line 'cold(1st cut) = first cut of the process (includes lazy PSO/RootSignature init, first-use buffers). warm(2nd cut) = second cut in the same process on the mirrored leg (source mesh already has a mask).'
Add-Line 'Stage times are deltas of steady_clock marks from SkeletalPatchCopy::Request entry. *_cpu = time inside that function (includes synchronous GPU waits inside it). *_wait = waiting for GPU/next frame; the frame''s other work (render, physics) is inside.'
Add-Line '  select_cpu            Request start -> joint/bone selection and skin snapshot done'
Add-Line '  extract_submit_cpu    SkeletalPatchJob::Submit (buffers, commands, submit)'
Add-Line '  extract_gpu_wait      submit -> fence done and readback processed'
Add-Line '  copy_prepare_cpu      readback -> copy owner created, pose/bounds prepared'
Add-Line '  cut_submit_wait       copy ready -> Static cut request submitted (next frame)'
Add-Line '  cutter_latency        cut submitted -> cutter result received'
Add-Line '  body_submit_cpu       OnCutResult: cap relabel, body classify and compaction submit'
Add-Line '  body_gpu_wait         OnCutResult end -> FinishCutResult start'
Add-Line '  finish_cpu            FinishCutResult (patch skin VB build incl. GPU wait, break queue)'
Add-Line '  break_wait            FinishCutResult end -> physics break result'
Add-Line '  publish_cpu           break result -> pieces committed and PrepareMeshes done (setmask_cpu is part of it)'
Add-Line '  TOTAL_request_to_publish, frames_spanned   whole request and graphics frames spent'
Add-Line '  cutter_*_ms           the cutter''s own GPU timestamps (clip/boundary/cap/finalize), always available'
Add-Line '  gpu_us_*              GPU timestamp pairs of whole passes (only with gpuTimers=ON)'
Add-Line 'Instrumentation overhead = compare cut_quiet_gpuOn_full vs cut_quiet_gpuOff_full (CPU stage rows). Logging overhead = cut_loud_gpuOff_full vs cut_quiet_gpuOff_full.'
Add-Line 'only-target configs also remove the other objects'' physics bodies and draws: compare CPU only within the same configuration.'
Add-Line ''

$cutOrder = @('select_cpu', 'extract_submit_cpu', 'extract_gpu_wait', 'copy_prepare_cpu', 'cut_submit_wait', 'cutter_latency', 'body_submit_cpu', 'body_gpu_wait', 'finish_cpu', 'break_wait', 'publish_cpu', 'setmask_cpu', 'TOTAL_request_to_publish', 'frames_spanned')
foreach ($model in $Models) {
    Add-Line "################ CUT: $model ################"
    foreach ($cfg in $cutConfigs) {
        foreach ($cutLabel in 'cold(1st cut)', 'warm(2nd cut)') {
            $rows = $summaryCsv | Where-Object { $_.Config -eq $cfg.Name -and $_.Model -eq $model -and $_.Cut -eq $cutLabel }
            if (-not $rows) { continue }
            Add-Line "== $model | $($cfg.Name) | $cutLabel | $($cfg.Desc)"
            Add-Line ('{0,-30} {1,3} {2,11} {3,11} {4,11}' -f 'metric', 'n', 'mean', 'min', 'max')
            $names = @($cutOrder | Where-Object { $rows.Metric -contains $_ }) + @($rows.Metric | Where-Object { $cutOrder -notcontains $_ } | Sort-Object)
            foreach ($mn in $names) {
                $r = $rows | Where-Object { $_.Metric -eq $mn }
                Add-Line ('{0,-30} {1,3} {2,11:N3} {3,11:N3} {4,11:N3}' -f $mn, $r.N, $r.Mean, $r.Min, $r.Max)
            }
            Add-Line ''
        }
    }
}

if (-not $SkipRender) {
    Add-Line '--- How to read the RENDER tables ---'
    Add-Line 'Per launch, FRAME rows (previous-frame values) are averaged over windows: pre = animated frames from start+15 to request-3 (>300 expected), post = publish+150 .. end-3.'
    Add-Line 'Then mean/min/max are taken over launches. wall_ms is pinned by V-Sync, so use GPU/CPU rows. gpu_total = Scene Update GPU + Render GPU. cpu_total = Update + RenderScene function time.'
    Add-Line 'depth/shadow/color = GPU time of those mesh passes (the draws affected by the cut). skin_* = per-frame patch skinning of the two pieces (post only). GPU block values lag 1-2 frames (window averages unaffected).'
    Add-Line 'Compare pre vs post within the same model and configuration. Noise: run-to-run spread (min/max); the GPU clock may also drop at low load with V-Sync.'
    Add-Line ''
    foreach ($model in $Models) {
        Add-Line "################ RENDER: $model ################"
        foreach ($cfg in $renderConfigs) {
            foreach ($wn in 'render-pre', 'render-post') {
                $rows = $summaryCsv | Where-Object { $_.Config -eq $cfg.Name -and $_.Model -eq $model -and $_.Cut -eq $wn }
                if (-not $rows) { continue }
                $nfr = ($renderRows | Where-Object { $_.Config -eq $cfg.Name -and $_.Model -eq $model -and $_.Window -eq $wn.Substring(7) } | Select-Object -First 1).Frames
                Add-Line "== $model | $($cfg.Name) | $wn (frames per launch ~$nfr) | $($cfg.Desc)"
                Add-Line ('{0,-24} {1,3} {2,11} {3,11} {4,11}' -f 'metric', 'n', 'mean', 'min', 'max')
                foreach ($r in $rows) { Add-Line ('{0,-24} {1,3} {2,11:N4} {3,11:N4} {4,11:N4}' -f $r.Metric, $r.N, $r.Mean, $r.Min, $r.Max) }
                Add-Line ''
            }
            # pre -> post difference of means
            $pre = $summaryCsv | Where-Object { $_.Config -eq $cfg.Name -and $_.Model -eq $model -and $_.Cut -eq 'render-pre' }
            $post = $summaryCsv | Where-Object { $_.Config -eq $cfg.Name -and $_.Model -eq $model -and $_.Cut -eq 'render-post' }
            if ($pre -and $post) {
                Add-Line "== $model | $($cfg.Name) | post - pre (difference of means)"
                foreach ($p in $pre) { $q = $post | Where-Object { $_.Metric -eq $p.Metric }; if ($q) { Add-Line ('{0,-24} {1,11:N4}' -f $p.Metric, ($q.Mean - $p.Mean)) } }
                Add-Line ''
            }
        }
    }
}

Add-Line '--- Not measured / limitations ---'
Add-Line '* Thigh launches contain cold+warm cuts; additional scenes contain only one cold cut; no truly cold OS/driver cache start is forced. The first launch of a config may be slower for that reason.'
Add-Line '* cpu_ms_sum/count/mean rows aggregate repeated names per request. Upload-wait includes allocation and command submission, not just fence sleep; parent CPU ranges include child ranges.'
Add-Line '* gpu_sum_ms adds cutter and whole passes only (excludes nested Extract-mode rows); absent with GPU timers OFF.'
Add-Line '* The warm cut cuts the mirrored leg of an already cut source mesh (has a triangle mask), so its workload is not identical to the first cut.'
Add-Line '* Physics internals (BreakConstraint), transient extract buffers and process-wide VRAM are not measured. Only resident piece buffers are logged (MEM lines in raw logs).'
Add-Line '* Background load, power plan/AC state and GPU clocks are not controlled; close other applications before running.'
Add-Line ''
if ($failed.Count) { Add-Line "FAILED/INCOMPLETE runs: $($failed -join ', ')" } else { Add-Line 'All runs completed and published.' }
Add-Line "Raw logs: $rawDir"
Add-Line "CSV: $(Join-Path $OutDir 'CutPerfRuns.csv') , $(Join-Path $OutDir 'CutPerfSummary.csv') , $(Join-Path $OutDir 'CutPerfRenderRuns.csv')"

$result = Join-Path $OutDir 'CutPerfResult.txt'
[IO.File]::WriteAllText($result, $sb.ToString(), (New-Object System.Text.UTF8Encoding($true)))
Write-Host "Result: $result"
if (-not $NoOpen) { Start-Process notepad.exe -ArgumentList "`"$result`"" }

# 실패한 캠페인은 자동화 호출자에게도 실패로 알린다.
if ($failed.Count) { throw "CutPerf incomplete: $($failed -join ', ')" }
