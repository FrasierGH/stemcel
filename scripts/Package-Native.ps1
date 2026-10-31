<#
.SYNOPSIS
    Package the native (JUCE, no Electron) Stemcel for Windows as a portable zip.

.DESCRIPTION
    Builds StemcelNative in Release (Visual Studio 2022 generator), checks that
    every runtime file the app loads sits beside Stemcel.exe (LAME, MIDI controller
    profiles, the rekordbox reader, the VC++ runtime it links), runs
    the app's smoke test from the staged folder, and writes
    dist/Stemcel-<version>-native-win-x64.zip.

    The rekordbox reader is built by cargo. OpenSSL's vendored build needs a native
    Perl: pass -Perl when the first perl on PATH is MSYS/Git Bash's.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\Package-Native.ps1
    powershell -ExecutionPolicy Bypass -File scripts\Package-Native.ps1 -Perl C:\Strawberry\perl\bin\perl.exe
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$Perl = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'backend\build-native-release'

if (-not $SkipBuild) {
    $configure = @('-S', (Join-Path $repo 'backend'), '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DSTEMCEL_BUILD_TESTS=OFF', '-DSTEMCEL_BUILD_NATIVE_APP=ON')
    if ($Perl -ne '') { $configure += "-DSTEMCEL_PERL=$Perl" }
    & cmake @configure
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    & cmake --build $build --config Release --parallel --target StemcelNative
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
}

$artefacts = Join-Path $build 'StemcelNative_artefacts\Release'
foreach ($required in @('Stemcel.exe', 'lame.exe', 'midi-mappings', 'stemcel_rekordbox.dll', 'vcruntime140.dll')) {
    if (-not (Test-Path (Join-Path $artefacts $required))) {
        throw "Bundling guard: $required is missing beside Stemcel.exe in $artefacts"
    }
}

$version = (Select-String -Path (Join-Path $repo 'backend\CMakeLists.txt') -Pattern '^\s*VERSION\s+([0-9.]+)' |
    Select-Object -First 1).Matches[0].Groups[1].Value
$name = "Stemcel-$version-native-win-x64"
$dist = Join-Path $repo 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# Everything the app needs at runtime; build by-products (.pdb/.lib/.exp/.ilk) stay behind.
Get-ChildItem $artefacts | Where-Object { $_.Extension -notin @('.pdb', '.lib', '.exp', '.ilk') } |
    ForEach-Object { Copy-Item $_.FullName -Destination $stage -Recurse -Force }

# The AGPL travels with the program: its text and the notice of who wrote and changed it.
foreach ($doc in @('LICENSE', 'NOTICE')) {
    $source = Join-Path $repo $doc
    if (-not (Test-Path $source)) { throw "Licence guard: $doc is missing from $repo" }
    Copy-Item $source -Destination (Join-Path $stage "$doc.txt") -Force
}

# Prove the staged app runs end to end.
$smoke = Join-Path ([System.IO.Path]::GetTempPath()) "stemcel-package-smoke"
if (Test-Path $smoke) { Remove-Item -Recurse -Force $smoke }
$process = Start-Process -FilePath (Join-Path $stage 'Stemcel.exe') -ArgumentList '--smoke-test', "`"$smoke`"" -PassThru
if (-not $process.WaitForExit(300000)) {
    $process | Stop-Process -Force
    throw 'Packaged app smoke test timed out'
}
Get-Content (Join-Path $smoke 'smoke-result.txt')
if ($process.ExitCode -ne 0) { throw "Packaged app failed its smoke test (exit $($process.ExitCode))" }

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Packaged $zip"
