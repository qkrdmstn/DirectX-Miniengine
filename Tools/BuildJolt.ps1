param(
    [ValidateSet('Debug', 'Release', 'Profile')][string]$Configuration = 'Debug',
    [string]$Target = 'Jolt',
    [switch]$RunTests,
    [switch]$CleanFirst
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

# CMake가 소스 변경에 따른 재구성을 관리하며 두 구성의 산출물은 분리한다.
if (-not (Test-Path -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt'))) {
    & cmake -S $sourceRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64 -T v143 "-DJOLT_ARCHIVE=$archivePath"
    if ($LASTEXITCODE -ne 0) { throw 'Jolt CMake configuration failed.' }
}
$buildArgs = @('--build', $buildRoot, '--config', $sdkConfiguration, '--target', $Target, '--parallel', '4')
if ($CleanFirst) { $buildArgs += '--clean-first' }
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw 'Jolt build failed.' }
if ($RunTests) {
    # 목록의 테스트 target이면 그 테스트만 실행하고, 그 외 target이면 모든 테스트 target을 빌드한 뒤 전체를 실행한다.
    $testTargets = @('PhysicsQueryTests', 'PhysicsBodyTests', 'PhysicsStepTests', 'ConvexElemTests', 'AggregateGeomTests', 'AggregateSliceTests', 'BodyMassTests')
    $ctestArgs = @('--test-dir', $buildRoot, '-C', $sdkConfiguration, '--output-on-failure')
    if ($testTargets -contains $Target) {
        $ctestArgs += @('-R', ('^' + [regex]::Escape($Target) + '$'))
    }
    else {
        foreach ($testTarget in $testTargets) {
            & cmake --build $buildRoot --config $sdkConfiguration --target $testTarget --parallel 4
            if ($LASTEXITCODE -ne 0) { throw "Test target build failed: $testTarget" }
        }
    }
    & ctest @ctestArgs
    if ($LASTEXITCODE -ne 0) { throw 'Physics tests failed.' }
}
