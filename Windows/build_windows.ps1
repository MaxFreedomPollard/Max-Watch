$ErrorActionPreference = "Stop"

$WinRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRoot = Join-Path $WinRoot "build"
$DistDir = Join-Path $BuildRoot "dist"
$Version = "1.3.0"

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

Write-Host "=== [1/5] Building Max Watch Native x64 (Windows 10/11) ==="
cmake -S $WinRoot -B (Join-Path $BuildRoot "x64") -G "Visual Studio 17 2022" -A x64
cmake --build (Join-Path $BuildRoot "x64") --config Release --parallel

Write-Host "=== [2/5] Building Max Watch Native ARM64 (Windows 10/11 Snapdragon / Surface) ==="
cmake -S $WinRoot -B (Join-Path $BuildRoot "arm64") -G "Visual Studio 17 2022" -A ARM64
cmake --build (Join-Path $BuildRoot "arm64") --config Release --parallel

$ExeX64 = Join-Path $BuildRoot "x64\Release\MaxWatch.exe"
$ExeArm64 = Join-Path $BuildRoot "arm64\Release\MaxWatch.exe"

if (!(Test-Path $ExeX64)) { throw "Missing x64 executable at $ExeX64" }
if (!(Test-Path $ExeArm64)) { throw "Missing ARM64 executable at $ExeArm64" }

Write-Host "=== [3/5] Running 14-Stage Automated Cryptographic, Power-Cut & Playback Self-Test ==="
$SelfTestTmp = Join-Path $BuildRoot "selftest_tmp"
& $ExeX64 --selftest $SelfTestTmp
if ($LASTEXITCODE -ne 0) {
    throw "MaxWatch.exe --selftest failed with exit code $LASTEXITCODE"
}

Write-Host "=== [4/5] Running Direct2D Studio UI Smoke Test ==="
$SmokeDir = Join-Path $DistDir "screenshots"
& $ExeX64 --ui-smoke-test $SmokeDir
if ($LASTEXITCODE -ne 0) {
    throw "MaxWatch.exe --ui-smoke-test failed with exit code $LASTEXITCODE"
}

Write-Host "=== [5/5] Packaging Portable Editions & Universal (x64 + ARM64) Windows Setup Installer ==="
$PortableX64 = Join-Path $DistDir "Max-Watch-$Version-Windows-Portable-x64.exe"
$PortableArm64 = Join-Path $DistDir "Max-Watch-$Version-Windows-Portable-ARM64.exe"
Copy-Item -Force $ExeX64 $PortableX64
Copy-Item -Force $ExeArm64 $PortableArm64

$IsccCandidates = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
)
$IsccPath = $IsccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $IsccPath) {
    $Cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($Cmd) { $IsccPath = $Cmd.Source }
}

if ($IsccPath) {
    & $IsccPath (Join-Path $WinRoot "installer\MaxWatch.iss")
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed" }
} else {
    Write-Warning "Inno Setup 6 (ISCC.exe) not found in PATH; portable .exe artifacts generated."
}

$Artifacts = Get-ChildItem -Path $DistDir -Filter "*.exe" | Sort-Object Name
$SumLines = @()
foreach ($Art in $Artifacts) {
    $Hash = (Get-FileHash -Path $Art.FullName -Algorithm SHA256).Hash.ToLower()
    $Line = "$Hash  $($Art.Name)"
    $SumLines += $Line
    Write-Host $Line
}
$SumLines | Set-Content -Encoding UTF8 (Join-Path $DistDir "SHA256SUMS-Windows.txt")
Write-Host "Windows Release Artifacts Ready in $DistDir"
