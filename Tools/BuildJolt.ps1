param(
    [ValidateSet('Debug', 'Release', 'Profile')][string]$Configuration = 'Debug',
    [string]$Target = 'Jolt',
    [switch]$RunTests,
    [switch]$CleanFirst,
    [switch]$Clean,
    [string]$VsVersion = '',
    [string]$VsInstallDir = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repoRoot 'MiniEngine/Physics'
$cacheRoot = Join-Path $repoRoot 'MiniEngine/Build/ThirdParty/Jolt'
$archivePath = Join-Path $cacheRoot 'source.tar.gz'
$buildRoot = Join-Path $cacheRoot 'build'
$revision = 'e77f175595e64cb44218cc9d9d56fc365ad0e36a'
$expectedHash = '1F32328FB763135DE10A244568D6CCB2ED9B1E6593FAFE6DC6DB5B2719D330BD'
$sdkConfiguration = if ($Configuration -eq 'Profile') { 'Release' } else { $Configuration }
New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null

# MSBuild의 Windows PowerShell에서도 같은 해시를 계산한다.
function Get-ArchiveHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}

# vswhere 실행 파일 경로를 반환하고, 설치되어 있지 않으면 빈 문자열을 반환한다.
function Get-VsWherePath {
    # Program Files (x86) 위치를 확인한다.
    $programFilesX86 = ${env:ProgramFiles(x86)}
    if (-not $programFilesX86) { return '' }

    # 설치 관리자 폴더의 vswhere가 있으면 그 경로를 반환한다.
    $path = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $path) { return $path }
    return ''
}

# Visual Studio 주 버전 번호에 맞는 CMake generator 이름을 반환한다.
function Get-GeneratorName([string]$Version) {
    # 버전 문자열에서 주 버전만 꺼내 generator 이름으로 바꾼다.
    $major = ($Version -split '\.')[0]
    switch ($major) {
        '17' { return 'Visual Studio 17 2022' }
        '18' { return 'Visual Studio 18 2026' }
        default { throw "Unsupported Visual Studio version: $Version" }
    }
}

# 파라미터, 환경 변수, vswhere 순서로 Visual Studio 버전을 정해 generator 이름을 반환한다.
function Resolve-VsGenerator {
    # 호출한 쪽이 알려 준 버전을 먼저 쓰고 없으면 환경 변수를 본다.
    $version = $VsVersion
    if (-not $version) { $version = $env:VisualStudioVersion }

    # 둘 다 없으면 설치된 가장 최신 Visual Studio 버전을 vswhere로 조회한다.
    if (-not $version) {
        $vswhere = Get-VsWherePath
        if ($vswhere) {
            $version = & $vswhere -latest -products * -property installationVersion | Select-Object -First 1
        }
    }
    if (-not $version) {
        throw 'Visual Studio version not found. Pass -VsVersion or run from a Visual Studio build or Developer PowerShell.'
    }

    return Get-GeneratorName $version
}

# 해당 cmake의 --help 목록에 generator 이름이 있는지 확인한다.
function Test-CMakeSupportsGenerator([string]$CMakePath, [string]$Generator) {
    # cmake가 출력하는 도움말 전체를 문자열로 받는다.
    $help = (& $CMakePath --help | Out-String)

    # Generators 목록에 정확한 이름이 있는지 검사한다.
    $pattern = '(?m)^[\s\*]*' + [regex]::Escape($Generator) + '(\s|\[|=)'
    return [bool]($help -match $pattern)
}

# 후보 cmake 경로를 PATH, 지정한 VS 설치, vswhere 순서로 모은다.
function Get-CMakeCandidates {
    $relativePath = 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $candidates = @()

    # PATH의 cmake를 가장 먼저 시도한다.
    $command = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $candidates += $command.Source }

    # 호출한 쪽이 알려 준 Visual Studio 설치의 번들 cmake를 다음에 시도한다.
    if ($VsInstallDir) {
        $installCMake = Join-Path $VsInstallDir $relativePath
        if (Test-Path -LiteralPath $installCMake) { $candidates += $installCMake }
    }

    # 마지막으로 vswhere가 찾은 번들 cmake를 시도한다.
    $vswhere = Get-VsWherePath
    if ($vswhere) {
        $candidates += @(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -find $relativePath)
        $candidates += @(& $vswhere -products * -find $relativePath)
    }

    return @($candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)
}

# generator를 지원하는 첫 번째 cmake를 고르고, 없으면 설치 방법을 안내한다.
function Resolve-CMakePath([string]$Generator) {
    # 후보가 하나도 없으면 설치 방법을 안내한다.
    $candidates = Get-CMakeCandidates
    if ($candidates.Count -eq 0) {
        throw 'cmake not found. Install "C++ CMake tools for Windows" in the Visual Studio Installer, or install CMake and add it to PATH.'
    }

    # generator를 지원하는 첫 후보를 고른다.
    foreach ($candidate in $candidates) {
        if (Test-CMakeSupportsGenerator $candidate $Generator) { return $candidate }
    }

    throw "No cmake supports generator '$Generator'. Checked: $($candidates -join '; '). Update CMake or install `"C++ CMake tools for Windows`" from a matching Visual Studio."
}

# 선택한 cmake와 같은 폴더의 ctest 경로를 반환한다.
function Resolve-CTestPath([string]$CMakePath) {
    # cmake와 같은 폴더의 ctest 경로를 만든다.
    $ctestPath = Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe'

    # 파일이 없으면 명확한 오류로 알린다.
    if (-not (Test-Path -LiteralPath $ctestPath)) {
        throw "ctest not found next to cmake: $ctestPath"
    }
    return $ctestPath
}

# Clean 요청인데 지울 빌드 캐시가 없으면 도구를 찾지 않고 바로 끝낸다.
if ($Clean -and -not (Test-Path -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt'))) {
    Write-Host 'BuildJolt: nothing to clean.'
    exit 0
}

# 사용할 generator와 cmake를 정하고 빌드 로그에 한 줄로 남긴다.
$generator = Resolve-VsGenerator
$cmakePath = Resolve-CMakePath $generator
Write-Host "BuildJolt: cmake=$cmakePath generator=$generator"

# Clean 요청이면 다운로드와 configure 없이 기존 산출물만 지우고 끝낸다.
if ($Clean) {
    & $cmakePath --build $buildRoot --config $sdkConfiguration --target clean
    if ($LASTEXITCODE -ne 0) { throw 'Jolt clean failed.' }
    exit 0
}

# 처음에만 고정 소스를 내려받고 재사용할 때도 해시를 검사한다.
if (-not (Test-Path -LiteralPath $archivePath)) {
    $pendingArchive = Join-Path $cacheRoot 'source.pending.tar.gz'
    $download = New-Object Net.WebClient
    try { $download.DownloadFile("https://codeload.github.com/jrouwe/JoltPhysics/tar.gz/$revision", $pendingArchive) }
    finally { $download.Dispose() }
    if ((Get-ArchiveHash $pendingArchive) -ne $expectedHash) {
        throw 'Jolt archive hash mismatch; the pending download was not accepted.'
    }
    Move-Item -LiteralPath $pendingArchive -Destination $archivePath
}
if ((Get-ArchiveHash $archivePath) -ne $expectedHash) {
    throw 'Jolt cached archive hash mismatch.'
}

# 기존 캐시가 다른 generator로 만들어졌으면 자동 삭제하지 않고 안내한다.
$cachePath = Join-Path $buildRoot 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath) {
    $cachedLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
    if ($cachedLine) {
        $cachedGenerator = $cachedLine.Matches[0].Groups[1].Value.Trim()
        if ($cachedGenerator -ne $generator) {
            throw "Existing CMake cache uses generator '$cachedGenerator' but '$generator' is required. Delete $buildRoot and build again."
        }
    }
}

# CMake가 소스 변경에 따른 재구성을 관리하며 두 구성의 산출물은 분리한다.
if (-not (Test-Path -LiteralPath $cachePath)) {
    & $cmakePath -S $sourceRoot -B $buildRoot -G $generator -A x64 -T v143 "-DJOLT_ARCHIVE=$archivePath"
    if ($LASTEXITCODE -ne 0) { throw 'Jolt CMake configuration failed. -T v143 requires the "MSVC v143" build tools component in the selected Visual Studio.' }
}
$buildArgs = @('--build', $buildRoot, '--config', $sdkConfiguration, '--target', $Target, '--parallel', '4')
if ($CleanFirst) { $buildArgs += '--clean-first' }
& $cmakePath @buildArgs
if ($LASTEXITCODE -ne 0) { throw 'Jolt build failed.' }
if ($RunTests) {
    # 목록의 테스트 target이면 그 테스트만 실행하고, 그 외 target이면 모든 테스트 target을 빌드한 뒤 전체를 실행한다.
    $testTargets = @('PhysicsQueryTests', 'PhysicsBodyTests', 'PhysicsStepTests', 'PhysicsTransformTests', 'ConvexElemTests', 'AggregateGeomTests', 'AggregateSliceTests', 'BodyMassTests')
    $ctestPath = Resolve-CTestPath $cmakePath
    $ctestArgs = @('--test-dir', $buildRoot, '-C', $sdkConfiguration, '--output-on-failure')
    if ($testTargets -contains $Target) {
        $ctestArgs += @('-R', ('^' + [regex]::Escape($Target) + '$'))
    }
    else {
        foreach ($testTarget in $testTargets) {
            & $cmakePath --build $buildRoot --config $sdkConfiguration --target $testTarget --parallel 4
            if ($LASTEXITCODE -ne 0) { throw "Test target build failed: $testTarget" }
        }
    }
    & $ctestPath @ctestArgs
    if ($LASTEXITCODE -ne 0) { throw 'Physics tests failed.' }
}
