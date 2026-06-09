param(
  [string]$SafeWifiSsid = "",
  [string]$SafeWifiPassword = "",
  [switch]$RunWifiSwitch,
  [switch]$RunRealShortcutScan,
  [switch]$RunRealTraySmoke,
  [switch]$RunRealAutoStartSmoke
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$machinePath = [System.Environment]::GetEnvironmentVariable("Path", "Machine")
$userPath = [System.Environment]::GetEnvironmentVariable("Path", "User")
$env:Path = @($machinePath, $userPath, $env:Path) -join ";"
$sourceBin = Join-Path $root "build\mingw\bin"
$portableBin = $root
$isPortableLayout = (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) -and -not (Test-Path -LiteralPath (Join-Path $root "CMakeLists.txt"))
if (Test-Path -LiteralPath (Join-Path $sourceBin "wifi-warning.exe")) {
  $bin = $sourceBin
} elseif (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) {
  $bin = $portableBin
} else {
  $bin = $sourceBin
}
$runtime = if ($isPortableLayout) { Join-Path ([System.IO.Path]::GetTempPath()) "WiFiWarningPortableExternalAcceptance" } else { Join-Path $root "build\external-acceptance" }
$appData = Join-Path $runtime "AppData"
$configDir = Join-Path $appData "WiFiWarning"
$configPath = Join-Path $configDir "config.json"
$readyPath = Join-Path $runtime "ready.txt"
$port = 18770

function New-Result($Name, $Status, $Detail = "", $Evidence = $null) {
  [pscustomobject]@{
    name = $Name
    status = $Status
    detail = $Detail
    evidence = $Evidence
  }
}

function Invoke-CheckedCommand($Command, $Arguments) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Command
  $psi.Arguments = $Arguments
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $process = [System.Diagnostics.Process]::Start($psi)
  $stdout = $process.StandardOutput.ReadToEnd()
  $stderr = $process.StandardError.ReadToEnd()
  $process.WaitForExit()
  [pscustomobject]@{
    exit_code = $process.ExitCode
    stdout = $stdout.Trim()
    stderr = $stderr.Trim()
  }
}

function Find-CommandPath($CommandName, $Fallbacks = @()) {
  $candidate = Get-Command $CommandName -ErrorAction SilentlyContinue
  if ($candidate) { return $candidate.Source }
  foreach ($path in $Fallbacks) {
    if (Test-Path -LiteralPath $path) { return $path }
  }
  return $null
}

function Get-RegistryValue($Path, $Name) {
  $item = Get-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction SilentlyContinue
  if ($item -and ($item.PSObject.Properties.Name -contains $Name)) {
    return [string]$item.$Name
  }
  return ""
}

function New-WifiPrerequisites($IsAdmin, $Interfaces, $Networks, $SafeSsid) {
  $locationMachine = Get-RegistryValue "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\location" "Value"
  $locationUser = Get-RegistryValue "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\location" "Value"
  $radioSoftwareOff = $Interfaces.stdout -match "Software\s+Off"
  $scanNeedsLocation = $Networks.stdout -match "location permission" -or $Networks.stderr -match "location permission"
  $scanNeedsAdmin = $Networks.stdout -match "requires elevation|requires admin" -or $Networks.stderr -match "requires elevation|requires admin"
  $ready = $IsAdmin -and -not $radioSoftwareOff -and -not $scanNeedsLocation -and -not $scanNeedsAdmin -and -not [string]::IsNullOrWhiteSpace($SafeSsid)
  [pscustomobject]@{
    ready_for_real_wifi_switch = $ready
    administrator = $IsAdmin
    hklm_location = $locationMachine
    hkcu_location = $locationUser
    wlan_software_radio_off = $radioSoftwareOff
    scan_requires_location = $scanNeedsLocation
    scan_requires_admin = $scanNeedsAdmin
    safe_ssid_supplied = -not [string]::IsNullOrWhiteSpace($SafeSsid)
  }
}

function Invoke-RealAutoStartSmoke {
  $runPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
  $valueName = "WiFiWarning"
  $exe = Join-Path $bin "wifi-warning.exe"
  $previousExists = $false
  $previousValue = $null
  $before = Get-ItemProperty -LiteralPath $runPath -Name $valueName -ErrorAction SilentlyContinue
  if ($before -and ($before.PSObject.Properties.Name -contains $valueName)) {
    $previousExists = $true
    $previousValue = [string]$before.$valueName
  }
  try {
    & $exe --status | Out-Null
    $set = New-ItemProperty -LiteralPath $runPath -Name $valueName -Value ("`"$exe`" --minimized") -PropertyType String -Force
    $afterSet = Get-ItemProperty -LiteralPath $runPath -Name $valueName -ErrorAction Stop
    $written = [string]$afterSet.$valueName
    if ($written -notmatch [regex]::Escape($exe) -or $written -notmatch "--minimized") {
      throw "Real Run key value mismatch after write: $written"
    }
    Remove-ItemProperty -LiteralPath $runPath -Name $valueName -ErrorAction Stop
    $afterRemove = Get-ItemProperty -LiteralPath $runPath -Name $valueName -ErrorAction SilentlyContinue
    if ($afterRemove -and ($afterRemove.PSObject.Properties.Name -contains $valueName)) {
      throw "Real Run key value still exists after remove."
    }
    [pscustomobject]@{
      previous_exists = $previousExists
      written = $written
      removed = $true
    }
  } finally {
    if ($previousExists) {
      New-ItemProperty -LiteralPath $runPath -Name $valueName -Value $previousValue -PropertyType String -Force | Out-Null
    } else {
      Remove-ItemProperty -LiteralPath $runPath -Name $valueName -ErrorAction SilentlyContinue
    }
  }
}

function Start-TestService {
  if (Test-Path $runtime) {
    Remove-Item -LiteralPath $runtime -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $configDir | Out-Null
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
    rules = @()
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
  } else {
    $psi.EnvironmentVariables["APPDATA"] = $appData
  }
  $process = [System.Diagnostics.Process]::Start($psi)
  $deadline = (Get-Date).AddSeconds(8)
  do {
    if ($process.HasExited) {
      throw "external acceptance service exited early with code $($process.ExitCode)"
    }
    try {
      $response = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$port/api/config" -TimeoutSec 1
      if ($response.StatusCode -eq 200) { return $process }
    } catch {
      Start-Sleep -Milliseconds 150
    }
  } while ((Get-Date) -lt $deadline)
  throw "external acceptance service did not respond on port $port"
}

$results = New-Object System.Collections.Generic.List[object]

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$adminStatus = if ($isAdmin) { "pass" } else { "missing" }
$adminDetail = if ($isAdmin) { "Current PowerShell is elevated." } else { "Real WiFi queries and switching often require elevated PowerShell." }
$results.Add((New-Result "administrator" $adminStatus $adminDetail))

$interfaces = Invoke-CheckedCommand "netsh.exe" "wlan show interfaces"
$hasWifiInterface = $interfaces.exit_code -eq 0 -and $interfaces.stdout -match "(?m)^\s*GUID\s*:"
$interfaceStatus = if ($hasWifiInterface) { "pass" } elseif ($interfaces.stdout -match "requires elevation|requires admin" -or $interfaces.stderr -match "requires elevation|requires admin") { "missing" } else { "fail" }
$results.Add((New-Result "wifi-interface" $interfaceStatus "netsh wlan show interfaces" $interfaces))

$networks = Invoke-CheckedCommand "netsh.exe" "wlan show networks mode=bssid"
$networksAccessible = $networks.exit_code -eq 0
$networkStatus = if ($networksAccessible) { "pass" } elseif ($networks.stdout -match "location permission|requires elevation|requires admin" -or $networks.stderr -match "location permission|requires elevation|requires admin") { "missing" } else { "fail" }
$results.Add((New-Result "wifi-network-scan" $networkStatus "netsh wlan show networks mode=bssid" $networks))
$wifiPrereqs = New-WifiPrerequisites $isAdmin $interfaces $networks $SafeWifiSsid
$wifiPrereqStatus = if ($wifiPrereqs.ready_for_real_wifi_switch) { "pass" } else { "missing" }
$results.Add((New-Result "wifi-prerequisites" $wifiPrereqStatus "Real WiFi switching requires admin, location access, software radio on, and a safe target SSID." $wifiPrereqs))

$cmake = Find-CommandPath "cmake.exe" @(
  "C:\Program Files\CMake\bin\cmake.exe",
  "C:\Program Files (x86)\CMake\bin\cmake.exe"
)
$cmakeStatus = if ($cmake) { "pass" } else { "missing" }
$cmakeDetail = if ($cmake) { $cmake } else { "cmake.exe was not found in PATH; scripts/build-mingw.ps1 is the verified local build path." }
$results.Add((New-Result "cmake" $cmakeStatus $cmakeDetail))

if ($RunRealShortcutScan) {
  try {
    $scan = Invoke-CheckedCommand "powershell" "-NoProfile -ExecutionPolicy Bypass -File `"$((Join-Path $root "scripts\real-shortcut-scan-smoke.ps1"))`""
    if ($scan.exit_code -eq 0) {
      $results.Add((New-Result "real-shell-shortcut-scan" "pass" "Real Desktop/Start Menu temporary shortcut scan passed." $scan))
    } else {
      $results.Add((New-Result "real-shell-shortcut-scan" "fail" "Real shortcut scan script exited $($scan.exit_code)." $scan))
    }
  } catch {
    $results.Add((New-Result "real-shell-shortcut-scan" "fail" $_.Exception.Message))
  }
} else {
  $results.Add((New-Result "real-shell-shortcut-scan" "skipped" "Pass -RunRealShortcutScan to create and remove temporary .lnk files in the real Desktop/Start Menu roots."))
}

if ($RunRealTraySmoke) {
  try {
    $tray = Invoke-CheckedCommand "powershell" "-NoProfile -ExecutionPolicy Bypass -File `"$((Join-Path $root "scripts\tray-smoke.ps1"))`""
    $status = "fail"
    if ($tray.exit_code -eq 0 -and $tray.stdout -match "tray:window:icon:adapter" -and $tray.stdout -match "tray:window:icon:no-adapter") {
      $status = "pass"
    }
    $detail = if ($status -eq "pass") { "Real desktop tray icon smoke passed." } else { "Real desktop tray smoke did not report both tray icon states." }
    $results.Add((New-Result "real-tray-smoke" $status $detail $tray))
  } catch {
    $results.Add((New-Result "real-tray-smoke" "fail" $_.Exception.Message))
  }
} else {
  $results.Add((New-Result "real-tray-smoke" "skipped" "Pass -RunRealTraySmoke to start the tray briefly and confirm Shell_NotifyIcon in the real desktop notification area."))
}

if ($RunRealAutoStartSmoke) {
  try {
    $autoStart = Invoke-RealAutoStartSmoke
    $results.Add((New-Result "real-autostart-smoke" "pass" "Real HKCU Run auto-start write/remove passed; previous value was restored." $autoStart))
  } catch {
    $results.Add((New-Result "real-autostart-smoke" "fail" $_.Exception.Message))
  }
} else {
  $results.Add((New-Result "real-autostart-smoke" "skipped" "Pass -RunRealAutoStartSmoke to write, verify, remove, and restore the real HKCU Run WiFiWarning value."))
}

if ($RunWifiSwitch) {
  if (-not $SafeWifiSsid) {
    $results.Add((New-Result "real-wifi-switch" "fail" "Real WiFi switching requires -SafeWifiSsid."))
  } else {
    $service = $null
    try {
      $service = Start-TestService
      if ($SafeWifiPassword) {
        $body = @{ ssid = $SafeWifiSsid; password = $SafeWifiPassword } | ConvertTo-Json -Compress
      } else {
        $body = @{ ssid = $SafeWifiSsid } | ConvertTo-Json -Compress
      }
      $switch = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "http://127.0.0.1:$port/api/wifi/switch" -ContentType "application/json" -Body $body -TimeoutSec 20
      $current = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$port/api/wifi/current" -TimeoutSec 5
      $ok = $switch.StatusCode -eq 200 -and ($current.Content -match [regex]::Escape($SafeWifiSsid))
      $wifiSwitchStatus = if ($ok) { "pass" } else { "fail" }
      $results.Add((New-Result "real-wifi-switch" $wifiSwitchStatus "POST /api/wifi/switch followed by /api/wifi/current." @{ switch = $switch.Content; current = $current.Content }))
    } catch {
      $results.Add((New-Result "real-wifi-switch" "fail" $_.Exception.Message))
    } finally {
      if ($service -and -not $service.HasExited) {
        Stop-Process -Id $service.Id -Force -ErrorAction SilentlyContinue
      }
    }
  }
} else {
  $results.Add((New-Result "real-wifi-switch" "skipped" "Pass -RunWifiSwitch -SafeWifiSsid SSID to execute real WiFi switching acceptance."))
}

$summary = [pscustomobject]@{
  ok = -not ($results | Where-Object { $_.status -eq "fail" })
  generated_at = (Get-Date).ToString("s")
  results = $results
}

$summary | ConvertTo-Json -Depth 8

if ($results | Where-Object { $_.status -eq "fail" }) {
  exit 1
}
