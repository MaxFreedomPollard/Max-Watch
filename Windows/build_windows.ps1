$ErrorActionPreference = "Stop"

$WinRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRoot = Join-Path $WinRoot "build"
$DistDir = Join-Path $BuildRoot "dist"
$Version = "1.3.0"

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

# Auto-detect installed Visual Studio generator (VS 2026 = 18, VS 2022 = 17, VS 2019 = 16; fallback to CMake default VS generator)
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsGenArgs = @()
if (Test-Path $VsWhere) {
    $VsVer = (& $VsWhere -latest -products * -property installationVersion | Select-Object -First 1)
    if ($VsVer -match "^18\.") {
        $VsGenArgs = @("-G", "Visual Studio 18 2026")
    } elseif ($VsVer -match "^17\.") {
        $VsGenArgs = @("-G", "Visual Studio 17 2022")
    } elseif ($VsVer -match "^16\.") {
        $VsGenArgs = @("-G", "Visual Studio 16 2019")
    }
    Write-Host "Detected Visual Studio version: $VsVer ($($VsGenArgs -join ' '))"
}

Write-Host "=== [1/5] Building Max Watch Native x64 (Windows 10/11) ==="
& cmake -S $WinRoot -B (Join-Path $BuildRoot "x64") @VsGenArgs -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake x64 configure failed with exit code $LASTEXITCODE" }
& cmake --build (Join-Path $BuildRoot "x64") --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake x64 build failed with exit code $LASTEXITCODE" }

Write-Host "=== [2/5] Building Max Watch Native ARM64 (Windows 10/11 Snapdragon / Surface) ==="
& cmake -S $WinRoot -B (Join-Path $BuildRoot "arm64") @VsGenArgs -A ARM64
if ($LASTEXITCODE -ne 0) { throw "CMake ARM64 configure failed with exit code $LASTEXITCODE" }
& cmake --build (Join-Path $BuildRoot "arm64") --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake ARM64 build failed with exit code $LASTEXITCODE" }

$ExeX64 = Join-Path $BuildRoot "x64\Release\MaxWatch.exe"
$ExeArm64 = Join-Path $BuildRoot "arm64\Release\MaxWatch.exe"

if (!(Test-Path $ExeX64)) { throw "Missing x64 executable at $ExeX64" }
if (!(Test-Path $ExeArm64)) { throw "Missing ARM64 executable at $ExeArm64" }

$HostExe = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { $ExeArm64 } else { $ExeX64 }

Write-Host "=== [3/5] Running 14-Stage Automated Cryptographic, Power-Cut & Playback Self-Test ==="
$SelfTestTmp = Join-Path $BuildRoot "selftest_tmp"
& $HostExe --selftest $SelfTestTmp
if ($LASTEXITCODE -ne 0) {
    throw "MaxWatch.exe --selftest failed with exit code $LASTEXITCODE"
}

Write-Host "=== [4/5] Running Direct2D Studio UI Smoke Test ==="
$SmokeDir = Join-Path $DistDir "screenshots"
& $HostExe --ui-smoke-test $SmokeDir
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
