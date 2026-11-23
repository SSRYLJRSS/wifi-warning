param(
  [string]$Version = "2.0.0"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build"
$distRoot = Join-Path $root "dist"
$packageDir = Join-Path $distRoot "WiFiWarning"
$zipPath = Join-Path $distRoot ("wifi-warning-v{0}-win64.zip" -f $Version)

foreach ($exe in @("wifi-warning.exe", "ww-launch.exe", "ww-admin.exe")) {
  if (-not (Test-Path -LiteralPath (Join-Path $bin $exe))) { throw "required binary missing: $exe (run scripts\build-msvc.bat first)" }
}

if (Test-Path $packageDir) { Remove-Item -LiteralPath $packageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null

foreach ($exe in @("wifi-warning.exe", "ww-launch.exe", "ww-admin.exe")) {
  Copy-Item -LiteralPath (Join-Path $bin $exe) -Destination $packageDir
}
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination $packageDir
if (Test-Path (Join-Path $root "restore-interception.cmd")) { Copy-Item -LiteralPath (Join-Path $root "restore-interception.cmd") -Destination $packageDir }
Copy-Item -Recurse -Force -LiteralPath (Join-Path $root "frontend") -Destination $packageDir
if (Test-Path (Join-Path $root "docs\screens")) { Copy-Item -Recurse -Force -LiteralPath (Join-Path $root "docs\screens") -Destination (Join-Path $packageDir "docs") }
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir "frontend\assets\icons") | Out-Null
$sourceIcon = Join-Path $root "src\resources\icon.ico"
if (Test-Path -LiteralPath $sourceIcon) { Copy-Item -Force -LiteralPath $sourceIcon -Destination (Join-Path $packageDir "frontend\assets\icons\favicon.ico") }

@'
@echo off
setlocal
cd /d "%~dp0"
start "" wifi-warning.exe --minimized
'@ | Set-Content -LiteralPath (Join-Path $packageDir "start-minimized.cmd") -Encoding ASCII

if (Test-Path $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zipPath -Force
Write-Host "Packaged:"
Write-Host "  $packageDir"
Write-Host "  $zipPath"
