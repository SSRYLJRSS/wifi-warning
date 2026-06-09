$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sourceBin = Join-Path $root "build\mingw\bin"
$portableBin = $root
$isPortableLayout = (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) -and -not (Test-Path -LiteralPath (Join-Path $root "CMakeLists.txt"))
$bin = if (Test-Path -LiteralPath (Join-Path $sourceBin "wifi-warning.exe")) { $sourceBin } elseif (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) { $portableBin } else { $sourceBin }
$runtime = if ($isPortableLayout) { Join-Path ([System.IO.Path]::GetTempPath()) "WiFiWarningPortableTraySmoke" } else { Join-Path $root "build\tray-runtime" }
$appData = Join-Path $runtime "AppData"
$configDir = Join-Path $appData "WiFiWarning"
$configPath = Join-Path $configDir "config.json"
$readyPath = Join-Path $runtime "tray-ready.txt"
$shortcutPath = Join-Path $runtime "TrayToggle.lnk"
$backupPath = "$shortcutPath.backup"
$port = 18768

if (Test-Path $runtime) {
  Remove-Item -LiteralPath $runtime -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $configDir | Out-Null
Set-Content -LiteralPath $shortcutPath -Encoding ASCII -Value "replacement"
Set-Content -LiteralPath $backupPath -Encoding ASCII -Value "original"

$config = @{
  version = "1.5.0"
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
  rules = @(
    @{
      id = "tray_rule"
      ssid = "Office-WiFi"
      network_type = "wifi"
      network_id = "Office-WiFi"
      network_name = "Office-WiFi"
      safe_wifi_ssid = ""
      safe_wifi_password = ""
      description = "tray smoke"
      blocked_apps = @(
        @{
          name = "Tray App"
          original_path = "C:\Tray\App.exe"
          icon_path = "C:\Tray\App.exe"
          replaced_shortcuts = @(
            @{
              original_lnk = $shortcutPath
              backup_lnk = $backupPath
            }
          )
        }
      )
    }
  )
}
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 8), $utf8NoBom)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = Join-Path $bin "wifi-warning.exe"
$psi.Arguments = "--self-test-tray 2200 --self-test-tray-command toggle --ready-file `"$readyPath`" --no-autostart-sync --minimized"
$psi.WorkingDirectory = $bin
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
if ($null -ne $psi.Environment) {
  $psi.Environment["APPDATA"] = $appData
  $psi.Environment["WW_TEST_CURRENT_SSID"] = "Office-WiFi"
} else {
  $psi.EnvironmentVariables["APPDATA"] = $appData
  $psi.EnvironmentVariables["WW_TEST_CURRENT_SSID"] = "Office-WiFi"
}

$process = [System.Diagnostics.Process]::Start($psi)
try {
  $deadline = (Get-Date).AddSeconds(5)
  while (-not (Test-Path -LiteralPath $readyPath) -and (Get-Date) -lt $deadline) {
    if ($process.HasExited) {
      throw "tray process exited early with code $($process.ExitCode)"
    }
    Start-Sleep -Milliseconds 100
  }
  if (-not (Test-Path -LiteralPath $readyPath)) {
    throw "tray ready file was not written"
  }
  $ready = Get-Content -LiteralPath $readyPath -Raw
  if ($ready -notmatch "tray:window") {
    throw "tray window was not initialized: $ready"
  }

  if (-not $process.WaitForExit(7000)) {
    throw "tray self-test process did not exit"
  }
  if ($process.ExitCode -ne 0) {
    throw "tray self-test exited with code $($process.ExitCode)"
  }
  if ((Get-Content -LiteralPath $shortcutPath -Raw).Trim() -ne "original") {
    throw "tray toggle did not restore the tracked shortcut"
  }
  if (Test-Path -LiteralPath $backupPath) {
    throw "tray toggle did not remove the shortcut backup"
  }
  $saved = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
  if ($saved.settings.protection_enabled) {
    throw "tray toggle did not disable protection"
  }
  if ($saved.rules[0].blocked_apps[0].replaced_shortcuts.Count -ne 0) {
    throw "tray toggle did not clear replacement records"
  }

  [pscustomobject]@{
    ok = $true
    pid = $process.Id
    ready = $ready.Trim()
    restored_shortcut = $true
    protection_enabled = $saved.settings.protection_enabled
    config_path = $configPath
  } | ConvertTo-Json -Depth 4
} finally {
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
  }
}

$noAdapterReady = Join-Path $runtime "tray-no-adapter-ready.txt"
$saved.settings.protection_enabled = $true
$saved.rules[0].blocked_apps[0].replaced_shortcuts = @()
[System.IO.File]::WriteAllText($configPath, ($saved | ConvertTo-Json -Depth 8), $utf8NoBom)

$psi2 = New-Object System.Diagnostics.ProcessStartInfo
$psi2.FileName = Join-Path $bin "wifi-warning.exe"
$psi2.Arguments = "--self-test-tray 900 --ready-file `"$noAdapterReady`" --no-autostart-sync --minimized"
$psi2.WorkingDirectory = $bin
$psi2.UseShellExecute = $false
$psi2.RedirectStandardOutput = $true
$psi2.RedirectStandardError = $true
if ($null -ne $psi2.Environment) {
  $psi2.Environment["APPDATA"] = $appData
  $psi2.Environment["WW_TEST_CURRENT_SSID"] = "<no-adapter>"
} else {
  $psi2.EnvironmentVariables["APPDATA"] = $appData
  $psi2.EnvironmentVariables["WW_TEST_CURRENT_SSID"] = "<no-adapter>"
}

$process2 = [System.Diagnostics.Process]::Start($psi2)
try {
  $deadline = (Get-Date).AddSeconds(5)
  while (-not (Test-Path -LiteralPath $noAdapterReady) -and (Get-Date) -lt $deadline) {
    if ($process2.HasExited) {
      throw "no-adapter tray process exited early with code $($process2.ExitCode)"
    }
    Start-Sleep -Milliseconds 100
  }
  if (-not (Test-Path -LiteralPath $noAdapterReady)) {
    throw "no-adapter tray ready file was not written"
  }
  $noAdapter = Get-Content -LiteralPath $noAdapterReady -Raw
  if ($noAdapter -notmatch "no-adapter") {
    throw "tray no-adapter state was not reported: $noAdapter"
  }
  if (-not $process2.WaitForExit(5000)) {
    throw "no-adapter tray self-test process did not exit"
  }
  if ($process2.ExitCode -ne 0) {
    throw "no-adapter tray self-test exited with code $($process2.ExitCode)"
  }
  $noAdapterSaved = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
  if ($noAdapterSaved.settings.protection_enabled) {
    throw "no-adapter tray did not disable protection"
  }
  [pscustomobject]@{
    ok = $true
    no_adapter_ready = $noAdapter.Trim()
    no_adapter_protection_enabled = $noAdapterSaved.settings.protection_enabled
  } | ConvertTo-Json -Depth 3
} finally {
  if (-not $process2.HasExited) {
    Stop-Process -Id $process2.Id -Force
  }
}
