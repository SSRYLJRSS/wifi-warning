$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sourceBin = Join-Path $root "build\mingw\bin"
$portableBin = $root
$isPortableLayout = (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) -and -not (Test-Path -LiteralPath (Join-Path $root "CMakeLists.txt"))
$bin = if (Test-Path -LiteralPath (Join-Path $sourceBin "wifi-warning.exe")) { $sourceBin } elseif (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) { $portableBin } else { $sourceBin }
$runtime = if ($isPortableLayout) { Join-Path ([System.IO.Path]::GetTempPath()) "WiFiWarningPortableRuntimeSmoke" } else { Join-Path $root "build\runtime" }
$appData = Join-Path $runtime "AppData"
$configDir = Join-Path $appData "WiFiWarning"
$configPath = Join-Path $configDir "config.json"
$readyPath = Join-Path $runtime "ready.txt"
$port = 18767
$baseUrl = "http://127.0.0.1:$port"

if (Test-Path $runtime) {
  Remove-Item -LiteralPath $runtime -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $configDir | Out-Null

$config = @{
  version = "1.5.0"
  settings = @{
    auto_start = $false
    protection_enabled = $true
    dark_mode = $true
    bypass_password = ""
    language = "zh-CN"
    http_port = $port
  }
  rules = @(
    @{
      id = "runtime_rule"
      ssid = "Office-WiFi"
      network_type = "wifi"
      network_id = "Office-WiFi"
      network_name = "Office-WiFi"
      safe_wifi_ssid = "Home-WiFi"
      safe_wifi_password = ""
      description = "运行时 smoke"
      blocked_apps = @(
        @{
          name = "Runtime App"
          original_path = "C:\Runtime\App.exe"
          icon_path = "C:\Runtime\App.exe"
          replaced_shortcuts = @()
        }
      )
    }
  )
}
$configJson = $config | ConvertTo-Json -Depth 8
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($configPath, $configJson, $utf8NoBom)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = Join-Path $bin "wifi-warning.exe"
$psi.Arguments = "--self-test-server 6500 --ready-file `"$readyPath`" --no-autostart-sync --minimized"
$psi.WorkingDirectory = $bin
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
if ($null -ne $psi.Environment) {
  $psi.Environment["APPDATA"] = $appData
  $psi.Environment["WW_TEST_CURRENT_SSID"] = "Office-WiFi"
  $psi.Environment["WW_TEST_AVAILABLE_WIFI_JSON"] = '[{"ssid":"Office-WiFi","signal_quality":80,"connected":true,"secure":true,"auth":"WPA2-PSK"},{"ssid":"Home-WiFi","signal_quality":93,"connected":false,"secure":true,"auth":"WPA2-PSK"}]'
  $psi.Environment["WW_TEST_WIRED_ADAPTERS_JSON"] = '[{"id":"Ethernet 1","name":"Ethernet 1","connected":true,"enabled":true,"status":"up"}]'
  $psi.Environment["WW_TEST_WIRED_ACTION"] = "success"
  $psi.Environment["WW_TEST_CONNECT_WIFI"] = "dialog"
} else {
  $psi.EnvironmentVariables["APPDATA"] = $appData
  $psi.EnvironmentVariables["WW_TEST_CURRENT_SSID"] = "Office-WiFi"
  $psi.EnvironmentVariables["WW_TEST_AVAILABLE_WIFI_JSON"] = '[{"ssid":"Office-WiFi","signal_quality":80,"connected":true,"secure":true,"auth":"WPA2-PSK"},{"ssid":"Home-WiFi","signal_quality":93,"connected":false,"secure":true,"auth":"WPA2-PSK"}]'
  $psi.EnvironmentVariables["WW_TEST_WIRED_ADAPTERS_JSON"] = '[{"id":"Ethernet 1","name":"Ethernet 1","connected":true,"enabled":true,"status":"up"}]'
  $psi.EnvironmentVariables["WW_TEST_WIRED_ACTION"] = "success"
  $psi.EnvironmentVariables["WW_TEST_CONNECT_WIFI"] = "dialog"
}

$process = [System.Diagnostics.Process]::Start($psi)
try {
  $deadline = (Get-Date).AddSeconds(5)
  $configResponse = $null
  do {
    Start-Sleep -Milliseconds 150
    if ($process.HasExited) {
      throw "runtime process exited early with code $($process.ExitCode)"
    }
    try {
      $configResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/config" -TimeoutSec 1
    } catch {
      $configResponse = $null
    }
  } while ($null -eq $configResponse -and (Get-Date) -lt $deadline)

  if ($null -eq $configResponse) {
    if (Test-Path $readyPath) { Write-Host "READY: $(Get-Content -LiteralPath $readyPath)" }
    else { Write-Host "READY: missing" }
    Write-Host "PROCESS_EXITED: $($process.HasExited)"
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    if ($stdout) { Write-Host "STDOUT: $stdout" }
    if ($stderr) { Write-Host "STDERR: $stderr" }
    throw "runtime server did not respond on port $port"
  }

  $settingsResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/settings" -TimeoutSec 2
  $warningResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/warning?appName=Runtime%20App&app=C%3A%5CRuntime%5CApp.exe&ssid=Office-WiFi&ruleId=runtime_rule" -TimeoutSec 2
  $pickerResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/wifi-picker" -TimeoutSec 2
  $wifiResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/wifi/current" -TimeoutSec 2
  $networkResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/network/current" -TimeoutSec 2
  $wiredResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/network/wired" -TimeoutSec 2
  $wiredToggleResponse = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$baseUrl/api/network/wired/toggle" -ContentType "application/json" -Body '{"id":"Ethernet 1","enabled":false}' -TimeoutSec 2
  $wifiAvailableResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/wifi/available" -TimeoutSec 2
  $wifiSwitchResponse = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$baseUrl/api/wifi/switch" -ContentType "application/json" -Body '{"ssid":"Home-WiFi"}' -TimeoutSec 2
  $statsResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/stats" -TimeoutSec 2
  $appStatusResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/apps/status" -TimeoutSec 2

  Start-Sleep -Milliseconds 500
  $sample = Get-Process -Id $process.Id
  $memoryBytes = [int64]$sample.WorkingSet64
  $memoryMiB = [math]::Round($memoryBytes / 1MB, 2)

  $serviceIconPath = [System.Net.WebUtility]::UrlEncode((Join-Path $bin "wifi-warning.exe"))
  $appIconResponse = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/apps/icon?path=$serviceIconPath" -TimeoutSec 2

  if ($configResponse.StatusCode -ne 200) { throw "config endpoint status $($configResponse.StatusCode)" }
  if ($settingsResponse.StatusCode -ne 200 -or $settingsResponse.Content -notmatch 'settings.js') { throw "settings status $($settingsResponse.StatusCode)" }
  if ($warningResponse.StatusCode -ne 200 -or $warningResponse.Content -notmatch 'warning-card' -or $warningResponse.Content -notmatch 'switchSafe' -or $warningResponse.Content -notmatch 'appIcon') { throw "warning page smoke failed" }
  if ($pickerResponse.StatusCode -ne 200 -or $pickerResponse.Content -notmatch 'networkList' -or $pickerResponse.Content -notmatch 'refreshNetworks') { throw "wifi picker smoke failed" }
  if ($wifiResponse.Content -notmatch '"adapter_available"') { throw "wifi current response missing adapter_available" }
  if ($wifiResponse.Content -notmatch '"ssid": "Office-WiFi"') { throw "wifi current did not use test ssid" }
  if ($networkResponse.Content -notmatch '"type": "wifi"' -or $networkResponse.Content -notmatch 'Office-WiFi') { throw "network current response missing current wifi" }
  if ($wiredResponse.Content -notmatch 'Ethernet 1') { throw "wired adapters response missing test adapter" }
  if ($wiredToggleResponse.Content -notmatch 'disable_requested') { throw "wired toggle response missing disable request" }
  if ($wifiAvailableResponse.Content -notmatch 'Home-WiFi') { throw "wifi available response missing test network" }
  if ($wifiSwitchResponse.Content -notmatch '"ok": true') { throw "wifi switch test failed" }
  if ($wifiSwitchResponse.Content -notmatch '"connect_requested": false') { throw "wifi switch response missing connect_requested false" }
  if ($wifiSwitchResponse.Content -notmatch '"native_dialog_opened": true') { throw "wifi native dialog response did not report native dialog" }
  if ($statsResponse.Content -notmatch '"stats"') { throw "stats response missing stats" }
  if ($appStatusResponse.Content -notmatch '"rules"' -or $appStatusResponse.Content -notmatch '"Runtime App"') { throw "app status response missing configured app" }
  if ($appIconResponse.StatusCode -ne 200 -or $appIconResponse.Headers["Content-Type"] -notmatch 'image/x-icon') { throw "app icon endpoint smoke failed" }

  if (-not $process.WaitForExit(9000)) {
    throw "runtime process did not exit after self-test duration"
  }
  if ($process.ExitCode -ne 0) {
    throw "runtime process exited with code $($process.ExitCode)"
  }

  [pscustomobject]@{
    ok = $true
    pid = $process.Id
    memory_bytes = $memoryBytes
    memory_mib = $memoryMiB
    memory_target_mib = 5
    memory_target_met = ($memoryBytes -lt 5MB)
    port = $port
    config_status = $configResponse.StatusCode
    settings_status = $settingsResponse.StatusCode
    warning_status = $warningResponse.StatusCode
    picker_status = $pickerResponse.StatusCode
    app_icon_status = $appIconResponse.StatusCode
    app_status_status = $appStatusResponse.StatusCode
    wifi_switch_status = $wifiSwitchResponse.StatusCode
    wifi_switch_native_dialog = $true
    config_path = $configPath
  } | ConvertTo-Json -Depth 4
} finally {
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
  }
}
