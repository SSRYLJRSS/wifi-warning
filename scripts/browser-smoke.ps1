$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$sourceBin = Join-Path $root "build\mingw\bin"
$portableBin = $root
$isPortableLayout = (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) -and -not (Test-Path -LiteralPath (Join-Path $root "CMakeLists.txt"))
$bin = if (Test-Path -LiteralPath (Join-Path $sourceBin "wifi-warning.exe")) { $sourceBin } elseif (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) { $portableBin } else { $sourceBin }
$runtime = if ($isPortableLayout) { Join-Path ([System.IO.Path]::GetTempPath()) "WiFiWarningPortableBrowserSmoke" } else { Join-Path $root "build\browser-smoke" }
$appData = Join-Path $runtime "AppData"
$configDir = Join-Path $appData "WiFiWarning"
$configPath = Join-Path $configDir "config.json"
$fakeAppPath = Join-Path $runtime "Runtime App.exe"
$runtimeShortcutPath = Join-Path $runtime "Runtime App.lnk"
$crudShortcutPath = Join-Path $runtime "CRUD App.lnk"
$ssidPath = Join-Path $runtime "current-ssid.txt"
$readyPath = Join-Path $runtime "ready.txt"
$screenshotDir = Join-Path $runtime "screenshots"
$autoStartRunKey = "Software\WiFiWarningSmoke\BrowserSmoke-$PID"
$port = 18769
$baseUrl = "http://127.0.0.1:$port"
$nodeRoot = "C:\Users\ylj\.cache\codex-runtimes\codex-primary-runtime\dependencies\node"
$nodeExe = Join-Path $nodeRoot "bin\node.exe"
$nodeModules = Join-Path $nodeRoot "node_modules"
$pnpmModules = Join-Path $nodeModules ".pnpm\node_modules"
$edgePath = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

if (-not (Test-Path -LiteralPath $nodeExe)) { throw "bundled node was not found: $nodeExe" }
if (-not (Test-Path -LiteralPath $edgePath)) { throw "Microsoft Edge was not found: $edgePath" }
if (-not (Test-Path -LiteralPath (Join-Path $pnpmModules "playwright-core\package.json"))) {
  throw "bundled playwright dependencies were not found"
}

if (Test-Path $runtime) {
  Remove-Item -LiteralPath $runtime -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $configDir, $screenshotDir | Out-Null
[System.IO.File]::WriteAllText($fakeAppPath, "fake app", [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($ssidPath, "Office-WiFi", [System.Text.Encoding]::ASCII)

function New-SmokeShortcut {
  param(
    [string]$Path,
    [string]$Target,
    [string]$Description
  )
  $shell = New-Object -ComObject WScript.Shell
  $shortcut = $shell.CreateShortcut($Path)
  $shortcut.TargetPath = $Target
  $shortcut.WorkingDirectory = Split-Path -Parent $Target
  $shortcut.IconLocation = "$Target,0"
  $shortcut.Description = $Description
  $shortcut.Save()
}

New-SmokeShortcut -Path $runtimeShortcutPath -Target $fakeAppPath -Description "Runtime smoke shortcut"
New-SmokeShortcut -Path $crudShortcutPath -Target $fakeAppPath -Description "CRUD smoke shortcut"

$config = @{
  version = "1.0.0"
  settings = @{
    auto_start = $false
    protection_enabled = $true
    dark_mode = $true
    bypass_password = ""
    bypass_timeout_minutes = 0
    language = "zh-CN"
    http_port = $port
    bypass_until_epoch = 0
  }
  app_groups = @(
    @{
      id = "runtime_group"
      name = "Browser Smoke Group"
      apps = @(
        @{
          name = "Runtime App"
          original_path = $fakeAppPath
          icon_path = $fakeAppPath
          shortcut_paths = @($runtimeShortcutPath)
          replaced_shortcuts = @()
        },
        @{
          name = "Missing App"
          original_path = (Join-Path $runtime "Missing App.exe")
          icon_path = (Join-Path $runtime "Missing App.exe")
          shortcut_paths = @()
          replaced_shortcuts = @()
        }
      )
    }
  )
  rules = @(
    @{
      id = "runtime_rule"
      ssid = "Office-WiFi"
      app_group_id = "runtime_group"
      safe_wifi_ssid = "Home-WiFi"
      safe_wifi_password = "safe-secret"
      description = "浏览器 smoke"
      blocked_apps = @(
        @{
          name = "Runtime App"
          original_path = $fakeAppPath
          icon_path = $fakeAppPath
          shortcut_paths = @($runtimeShortcutPath)
          replaced_shortcuts = @()
        },
        @{
          name = "Missing App"
          original_path = (Join-Path $runtime "Missing App.exe")
          icon_path = (Join-Path $runtime "Missing App.exe")
          shortcut_paths = @()
          replaced_shortcuts = @()
        }
      )
    }
  )
}
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 8), $utf8NoBom)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = Join-Path $bin "wifi-warning.exe"
$psi.Arguments = "--self-test-server 45000 --ready-file `"$readyPath`" --no-autostart-sync --minimized"
$psi.WorkingDirectory = $bin
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
if ($null -ne $psi.Environment) {
  $psi.Environment["APPDATA"] = $appData
  $psi.Environment["WW_TEST_CURRENT_SSID_FILE"] = $ssidPath
  $psi.Environment["WW_TEST_AVAILABLE_WIFI_JSON"] = '[{"ssid":"Office-WiFi","signal_quality":80,"connected":true,"secure":true,"auth":"WPA2-PSK"},{"ssid":"Home-WiFi","signal_quality":93,"connected":false,"secure":true,"auth":"WPA2-PSK"}]'
  $psi.Environment["WW_TEST_CONNECT_WIFI"] = "password:Home-WiFi:safe-secret;Home-WiFi"
  $psi.Environment["WW_TEST_AUTOSTART_RUN_KEY"] = $autoStartRunKey
  $psi.Environment["WW_TEST_PICK_SHORTCUTS_JSON"] = ConvertTo-Json @($crudShortcutPath) -Compress
} else {
  $psi.EnvironmentVariables["APPDATA"] = $appData
  $psi.EnvironmentVariables["WW_TEST_CURRENT_SSID_FILE"] = $ssidPath
  $psi.EnvironmentVariables["WW_TEST_AVAILABLE_WIFI_JSON"] = '[{"ssid":"Office-WiFi","signal_quality":80,"connected":true,"secure":true,"auth":"WPA2-PSK"},{"ssid":"Home-WiFi","signal_quality":93,"connected":false,"secure":true,"auth":"WPA2-PSK"}]'
  $psi.EnvironmentVariables["WW_TEST_CONNECT_WIFI"] = "password:Home-WiFi:safe-secret;Home-WiFi"
  $psi.EnvironmentVariables["WW_TEST_AUTOSTART_RUN_KEY"] = $autoStartRunKey
  $psi.EnvironmentVariables["WW_TEST_PICK_SHORTCUTS_JSON"] = ConvertTo-Json @($crudShortcutPath) -Compress
}

$process = [System.Diagnostics.Process]::Start($psi)
try {
  $env:NODE_PATH = "$pnpmModules;$nodeModules"
  $env:WW_BROWSER_SMOKE_BASE_URL = $baseUrl
  $env:WW_BROWSER_SMOKE_OUT_DIR = $screenshotDir
  $env:WW_BROWSER_SMOKE_EDGE = $edgePath
  $env:WW_BROWSER_SMOKE_APP_PATH = $fakeAppPath
  $env:WW_BROWSER_SMOKE_SHORTCUT_PATH = $crudShortcutPath
  $env:WW_BROWSER_SMOKE_SSID_FILE = $ssidPath
  $env:WW_BROWSER_SMOKE_SERVICE_PID = [string]$process.Id
  & $nodeExe (Join-Path $PSScriptRoot "browser-smoke-runner.js")
  if ($LASTEXITCODE -ne 0) { throw "browser smoke runner failed with exit code $LASTEXITCODE" }

  if (-not $process.WaitForExit(1000)) {
    Stop-Process -Id $process.Id -Force
    $process.WaitForExit()
  }

  $screenshots = Get-ChildItem -LiteralPath $screenshotDir -Filter *.png | Select-Object -ExpandProperty FullName
  if (($screenshots | Measure-Object).Count -lt 5) {
    throw "browser smoke did not create expected screenshots"
  }

  [pscustomobject]@{
    ok = $true
    port = $port
    screenshots = $screenshots
    config_path = $configPath
  } | ConvertTo-Json -Depth 4
} finally {
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
  }
  Remove-Item -LiteralPath "HKCU:\$autoStartRunKey" -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item Env:\WW_BROWSER_SMOKE_BASE_URL -ErrorAction SilentlyContinue
  Remove-Item Env:\WW_BROWSER_SMOKE_OUT_DIR -ErrorAction SilentlyContinue
  Remove-Item Env:\WW_BROWSER_SMOKE_EDGE -ErrorAction SilentlyContinue
  Remove-Item Env:\WW_BROWSER_SMOKE_APP_PATH -ErrorAction SilentlyContinue
  Remove-Item Env:\WW_BROWSER_SMOKE_SHORTCUT_PATH -ErrorAction SilentlyContinue
  Remove-Item Env:\WW_BROWSER_SMOKE_SSID_FILE -ErrorAction SilentlyContinue
  Remove-Item Env:\WW_BROWSER_SMOKE_SERVICE_PID -ErrorAction SilentlyContinue
}
