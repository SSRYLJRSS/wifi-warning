param(
  [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build\mingw"
$obj = Join-Path $build "obj"
$bin = Join-Path $build "bin"

New-Item -ItemType Directory -Force -Path $obj, $bin | Out-Null

$commonFlags = @(
  "-std=c++20",
  "-DUNICODE",
  "-D_UNICODE",
  "-DWIN32_LEAN_AND_MEAN",
  "-D_WIN32_WINNT=0x0600",
  "-municode",
  "-ffunction-sections",
  "-fdata-sections",
  "-Isrc",
  "-Wall",
  "-Wextra"
)

if ($Configuration -eq "Debug") {
  $commonFlags += @("-g", "-O0")
} else {
  $commonFlags += @("-Os", "-s")
}

$serviceCoreSources = @(
  "src/core/auto_start.cpp",
  "src/core/config_manager.cpp",
  "src/core/dyn_bcrypt.cpp",
  "src/core/dyn_version.cpp",
  "src/core/dyn_wlan.cpp",
  "src/core/http_probe.cpp",
  "src/core/json.cpp",
  "src/core/logger.cpp",
  "src/core/network_manager.cpp",
  "src/core/rule_engine.cpp",
  "src/core/shortcut_manager.cpp",
  "src/core/util.cpp",
  "src/core/wifi_detector.cpp",
  "src/core/wifi_switcher.cpp",
  "src/ui/browser_launcher.cpp"
)

$launcherCoreSources = @()

$serviceSources = @(
  "src/main.cpp",
  "src/ui/api_handlers.cpp",
  "src/ui/http_server.cpp",
  "src/ui/tray_icon.cpp"
)

$launcherSources = @("src/ww-launch/main.cpp")
$smokeSources = @(
  "tests/smoke.cpp",
  "src/ui/api_handlers.cpp",
  "src/ui/http_server.cpp"
)
$libs = @("-Wl,--gc-sections", "-ladvapi32", "-lcomdlg32", "-lgdi32", "-liphlpapi", "-lpsapi", "-lole32", "-lshell32", "-luuid", "-lws2_32")

function Compile-Resource($name, $resource) {
  if (-not $resource) {
    return $null
  }
  $out = Join-Path $obj "$name-resource.o"
  $resourcePath = Join-Path $root $resource
  $resourceDir = Split-Path -Parent $resourcePath
  $resourceFile = Split-Path -Leaf $resourcePath
  Push-Location $resourceDir
  try {
    & windres --use-temp-file -i $resourceFile -o $out
  } finally {
    Pop-Location
  }
  if ($LASTEXITCODE -ne 0) {
    throw "windres failed for $name"
  }
  return $out
}

function Compile-App($name, $core, $sources, [bool]$windowsSubsystem = $true, [string]$resource = "") {
  $out = Join-Path $bin "$name.exe"
  $subsystem = if ($windowsSubsystem) { "-mwindows" } else { "-mconsole" }
  $resourceObj = Compile-Resource $name $resource
  $resourceArgs = if ($resourceObj) { @($resourceObj) } else { @() }
  $args = @($commonFlags) + $core + $sources + $resourceArgs + @($subsystem, "-o", $out) + $libs
  & g++ @args
  if ($LASTEXITCODE -ne 0) {
    throw "g++ failed for $name"
  }
}

Compile-App "wifi-warning" $serviceCoreSources $serviceSources $true "src\resources\wifi-warning.rc"
Compile-App "ww-launch" $launcherCoreSources $launcherSources $true "src\resources\ww-launch.rc"
Compile-App "ww-smoke" $serviceCoreSources $smokeSources $false

Copy-Item -Recurse -Force (Join-Path $root "frontend") $bin
New-Item -ItemType Directory -Force -Path (Join-Path $bin "frontend\assets\icons") | Out-Null
Copy-Item -Force (Join-Path $root "src\resources\icon.ico") (Join-Path $bin "frontend\assets\icons\favicon.ico")
Copy-Item -Force (Join-Path $root "config.json") $bin

Write-Host "Built:"
Write-Host "  $bin\wifi-warning.exe"
Write-Host "  $bin\ww-launch.exe"
