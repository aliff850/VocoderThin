param(
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

$Gpp = "C:\msys64\ucrt64\bin\g++.exe"
$Env:PATH = "C:\msys64\ucrt64\bin;$Env:PATH"

$RootDir = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RootDir "build"
$TestDir = Join-Path $BuildDir "tests"

if (!(Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
if (!(Test-Path $TestDir)) { New-Item -ItemType Directory -Path $TestDir | Out-Null }

$CommonFlags = @(
    "-std=c++17",
    "-O3",
    "-Wall",
    "-Wextra",
    "-static-libgcc",
    "-static-libstdc++",
    "-static"
)

# Collect DSP and VST sources as they are created
$AudioSources = @()
if (Test-Path (Join-Path $RootDir "src\audio")) {
    $AudioSources = Get-ChildItem -Path (Join-Path $RootDir "src\audio") -Filter "*.cpp" | ForEach-Object { $_.FullName }
}

$VstSources = @()
if (Test-Path (Join-Path $RootDir "src\vst")) {
    $VstSources = Get-ChildItem -Path (Join-Path $RootDir "src\vst") -Filter "*.cpp" | ForEach-Object { $_.FullName }
}

$UiSources = @()
if (Test-Path (Join-Path $RootDir "src\ui")) {
    $UiSources = Get-ChildItem -Path (Join-Path $RootDir "src\ui") -Filter "*.cpp" | ForEach-Object { $_.FullName }
}

# 1. Build VST2 Plugin DLL if VST sources exist
if ($VstSources.Count -gt 0) {
    $DllOut = Join-Path $BuildDir "VocoderThin.dll"
    Write-Host "[BUILD] Compiling VST2 Plugin -> $DllOut" -ForegroundColor Cyan
    $DllArgs = $CommonFlags + @("-shared") + $AudioSources + $VstSources + $UiSources + @(
        "-o", $DllOut,
        "-lwinmm", "-lgdi32", "-luser32", "-lmsimg32"
    )
    & $Gpp @DllArgs
    if ($LASTEXITCODE -ne 0) { throw "VST2 DLL build failed." }
}

# 2. Build Standalone App if StandaloneMain.cpp exists
$StandaloneSrc = Join-Path $RootDir "src\standalone\StandaloneMain.cpp"
if (Test-Path $StandaloneSrc) {
    $ExeOut = Join-Path $BuildDir "VocoderThinStandalone.exe"
    Write-Host "[BUILD] Compiling Standalone App -> $ExeOut" -ForegroundColor Cyan
    $ExeArgs = $CommonFlags + @("-mwindows") + $AudioSources + $VstSources + $UiSources + @($StandaloneSrc) + @(
        "-o", $ExeOut,
        "-lwinmm", "-lgdi32", "-luser32", "-lmsimg32"
    )
    & $Gpp @ExeArgs
    if ($LASTEXITCODE -ne 0) { throw "Standalone EXE build failed." }
}

# 3. Build Automated Test Suite
$TestSrc = Join-Path $RootDir "tests\test_dsp_and_vst.cpp"
$TestOut = Join-Path $TestDir "vocoder_tests.exe"
Write-Host "[BUILD] Compiling Test Suite -> $TestOut" -ForegroundColor Cyan
$TestArgs = $CommonFlags + $AudioSources + @($TestSrc) + @(
    "-o", $TestOut,
    "-lwinmm", "-lgdi32", "-luser32", "-lmsimg32"
)
& $Gpp @TestArgs
if ($LASTEXITCODE -ne 0) { throw "Test suite build failed." }

if (-not $SkipTests) {
    Write-Host "[TEST] Running $TestOut" -ForegroundColor Green
    & $TestOut
    if ($LASTEXITCODE -ne 0) { throw "Unit tests failed." }
}

