param(
  [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sourceBin = Join-Path $root "build\mingw\bin"
$portableBin = $root
$isPortableLayout = (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) -and -not (Test-Path -LiteralPath (Join-Path $root "CMakeLists.txt"))
$bin = if ($isPortableLayout) { $portableBin } else { $sourceBin }
$distRoot = if ($isPortableLayout) { Join-Path $root "dist" } else { Join-Path $root "build\dist" }
$packageDir = Join-Path $distRoot "WiFiWarning"
$zipPath = Join-Path $distRoot "WiFiWarning-portable.zip"

if (-not $isPortableLayout -and ((-not (Test-Path (Join-Path $bin "wifi-warning.exe"))) -or (-not (Test-Path (Join-Path $bin "ww-launch.exe"))) -or (-not (Test-Path (Join-Path $bin "ww-smoke.exe"))))) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "scripts\build-mingw.ps1") -Configuration $Configuration
  if ($LASTEXITCODE -ne 0) {
    throw "build failed"
  }
}

foreach ($requiredExe in @("wifi-warning.exe", "ww-launch.exe", "ww-smoke.exe")) {
  $requiredPath = Join-Path $bin $requiredExe
  if (-not (Test-Path -LiteralPath $requiredPath)) {
    throw "required binary was not found: $requiredPath"
  }
}

if (Test-Path $packageDir) {
  Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir "scripts") | Out-Null

Copy-Item -LiteralPath (Join-Path $bin "wifi-warning.exe") -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $bin "ww-launch.exe") -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $bin "ww-smoke.exe") -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $root "config.json") -Destination $packageDir
Copy-Item -Recurse -Force -LiteralPath (Join-Path $root "frontend") -Destination $packageDir
if (Test-Path -LiteralPath (Join-Path $root "docs")) {
  Copy-Item -Recurse -Force -LiteralPath (Join-Path $root "docs") -Destination $packageDir
}
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir "frontend\assets\icons") | Out-Null
$sourceIcon = Join-Path $root "src\resources\icon.ico"
if (Test-Path -LiteralPath $sourceIcon) {
  Copy-Item -Force -LiteralPath $sourceIcon -Destination (Join-Path $packageDir "frontend\assets\icons\favicon.ico")
}

foreach ($scriptName in @(
  "browser-smoke.ps1",
  "browser-smoke-runner.js",
  "runtime-smoke.ps1",
  "tray-smoke.ps1",
  "real-shortcut-scan-smoke.ps1",
  "local-acceptance.ps1",
  "package-portable.ps1"
)) {
  Copy-Item -Force -LiteralPath (Join-Path $root "scripts\$scriptName") -Destination (Join-Path $packageDir "scripts\$scriptName")
}

@'
@echo off
setlocal
cd /d "%~dp0"
wifi-warning.exe --uninstall-restore
echo.
echo If WiFi Warning is still running, exit it from the tray menu before deleting this folder.
pause
'@ | Set-Content -LiteralPath (Join-Path $packageDir "restore-shortcuts.cmd") -Encoding ASCII

@'
@echo off
setlocal
cd /d "%~dp0"
start "" wifi-warning.exe --minimized
'@ | Set-Content -LiteralPath (Join-Path $packageDir "start-minimized.cmd") -Encoding ASCII

if (Test-Path $zipPath) {
  Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zipPath -Force

Write-Host "Packaged:"
Write-Host "  $packageDir"
Write-Host "  $zipPath"
