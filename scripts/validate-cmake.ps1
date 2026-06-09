$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cmakePath = Join-Path $root "CMakeLists.txt"
$text = Get-Content -LiteralPath $cmakePath -Raw -Encoding UTF8

$required = @(
  "add_library(ww_core STATIC",
  "src/core/auto_start.cpp",
  "src/core/config_manager.cpp",
  "src/core/http_probe.cpp",
  "src/core/json.cpp",
  "src/core/logger.cpp",
  "src/core/rule_engine.cpp",
  "src/core/shortcut_manager.cpp",
  "src/core/util.cpp",
  "src/core/wifi_detector.cpp",
  "src/core/wifi_switcher.cpp",
  "src/ui/browser_launcher.cpp",
  "gdi32",
  "add_executable(wifi-warning WIN32",
  "src/main.cpp",
  "src/ui/api_handlers.cpp",
  "src/ui/http_server.cpp",
  "src/ui/tray_icon.cpp",
  "src/resources/wifi-warning.rc",
  "add_executable(ww-launch WIN32",
  "src/ww-launch/main.cpp",
  "src/resources/ww-launch.rc",
  "add_executable(ww-smoke",
  "tests/smoke.cpp",
  "install(FILES config.json DESTINATION .)",
  "scripts/runtime-smoke.ps1",
  "scripts/tray-smoke.ps1",
  "scripts/browser-smoke.ps1",
  "scripts/browser-smoke-runner.js",
  "scripts/local-acceptance.ps1",
  "scripts/external-acceptance.ps1",
  "scripts/real-shortcut-scan-smoke.ps1",
  "scripts/elevated-wifi-acceptance.ps1"
)

$missing = @()
foreach ($item in $required) {
  if ($text -notlike "*$item*") {
    $missing += $item
  }
}

if ($missing.Count -gt 0) {
  throw "CMakeLists.txt is missing expected entries: $($missing -join ', ')"
}

[pscustomobject]@{
  ok = $true
  checked = $required.Count
  cmake_path = $cmakePath
} | ConvertTo-Json -Depth 3
