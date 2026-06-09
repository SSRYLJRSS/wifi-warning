param(
  [string]$PackageRoot = "",
  [string]$ResultPath = "",
  [string]$SafeWifiSsid = "",
  [string]$SafeWifiPassword = ""
)

$ErrorActionPreference = "Stop"

function New-Result($Ok, $Stage, $Detail, $Evidence = $null) {
  [pscustomobject]@{
    ok = $Ok
    stage = $Stage
    detail = $Detail
    evidence = $Evidence
    generated_at = (Get-Date).ToString("s")
  }
}

function Save-Result($Result) {
  if (-not $ResultPath) { return }
  $dir = Split-Path -Parent $ResultPath
  if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  $Result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $ResultPath -Encoding UTF8
}

function Invoke-CheckedCommand($Command, $Arguments, $WorkingDirectory = "") {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Command
  $psi.Arguments = $Arguments
  if ($WorkingDirectory) { $psi.WorkingDirectory = $WorkingDirectory }
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

function Test-IsAdmin {
  $principal = [Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-RegistryValue($Path, $Name) {
  $item = Get-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction SilentlyContinue
  if ($item -and ($item.PSObject.Properties.Name -contains $Name)) {
    return $item.$Name
  }
  return $null
}

function Set-LocationPermission {
  $consentPaths = @(
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\location",
    "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\location"
  )
  $before = @{}
  foreach ($path in $consentPaths) {
    $before[$path] = Get-RegistryValue $path "Value"
    New-Item -Path $path -Force | Out-Null
    New-ItemProperty -LiteralPath $path -Name "Value" -Value "Allow" -PropertyType String -Force | Out-Null
  }

  $lfsvcPath = "HKLM:\SYSTEM\CurrentControlSet\Services\lfsvc\Service\Configuration"
  $lfsvcBefore = Get-RegistryValue $lfsvcPath "Status"
  $policyPath = "HKLM:\SOFTWARE\Policies\Microsoft\Windows\LocationAndSensors"
  $disableLocationBefore = Get-RegistryValue $policyPath "DisableLocation"
  if (Test-Path -LiteralPath $policyPath) {
    New-ItemProperty -LiteralPath $policyPath -Name "DisableLocation" -Value 0 -PropertyType DWord -Force | Out-Null
  }

  if (Test-Path -LiteralPath $lfsvcPath) {
    New-ItemProperty -LiteralPath $lfsvcPath -Name "Status" -Value 1 -PropertyType DWord -Force | Out-Null
  }
  Start-Service -Name lfsvc -ErrorAction SilentlyContinue
  Start-Service -Name WlanSvc -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2

  $after = @{}
  foreach ($path in $consentPaths) {
    $after[$path] = Get-RegistryValue $path "Value"
  }
  $after[$lfsvcPath] = Get-RegistryValue $lfsvcPath "Status"
  $after[$policyPath] = Get-RegistryValue $policyPath "DisableLocation"

  [pscustomobject]@{
    before = $before
    lfsvc_status_before = $lfsvcBefore
    disable_location_policy_before = $disableLocationBefore
    after = $after
  }
}

function Get-KnownProfiles {
  $profiles = New-Object System.Collections.Generic.List[string]
  $output = Invoke-CheckedCommand "netsh.exe" "wlan show profiles"
  foreach ($line in ($output.stdout -split "`r?`n")) {
    if ($line -match ":\s*(.+?)\s*$") {
      $name = $Matches[1].Trim()
      if ($name -and $name -ne "<None>") { $profiles.Add($name) }
    }
  }
  [pscustomobject]@{
    command = $output
    profiles = @($profiles)
  }
}

function Get-AvailableNetworks {
  $scan = Invoke-CheckedCommand "netsh.exe" "wlan show networks mode=bssid"
  $items = @()
  $current = $null
  foreach ($line in ($scan.stdout -split "`r?`n")) {
    if ($line -match "^\s*SSID\s+\d+\s*:\s*(.*)\s*$") {
      if ($current -and $current.ssid) { $items += $current }
      $current = [pscustomobject]@{ ssid = $Matches[1].Trim(); signal = 0; authentication = "" }
      continue
    }
    if ($current -and $line -match "^\s*Signal\s*:\s*(\d+)%") {
      $current.signal = [int]$Matches[1]
      continue
    }
    if ($current -and $line -match "^\s*Authentication\s*:\s*(.+?)\s*$") {
      $current.authentication = $Matches[1].Trim()
      continue
    }
  }
  if ($current -and $current.ssid) { $items += $current }
  [pscustomobject]@{
    command = $scan
    networks = $items
  }
}

function Select-TargetSsid($Profiles, $Networks, $RequestedSsid) {
  if ($RequestedSsid) {
    return [pscustomobject]@{
      ssid = $RequestedSsid
      reason = "requested"
      candidates = @()
    }
  }

  $profileSet = @{}
  foreach ($profile in $Profiles) { $profileSet[$profile] = $true }
  $candidates = @($Networks | Where-Object { $_.ssid -and $profileSet.ContainsKey($_.ssid) } | Sort-Object -Property signal -Descending)
  if ($candidates.Count -eq 0) {
    return [pscustomobject]@{
      ssid = ""
      reason = "no scanned network matched a saved profile"
      candidates = @()
    }
  }
  [pscustomobject]@{
    ssid = $candidates[0].ssid
    reason = "strongest scanned saved profile"
    candidates = @($candidates | Select-Object ssid, signal, authentication)
  }
}

function Invoke-RealWifiAcceptance($PackageRoot, $Ssid, $Password) {
  $args = @(
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    (Join-Path $PackageRoot "scripts\external-acceptance.ps1"),
    "-RunWifiSwitch",
    "-SafeWifiSsid",
    $Ssid
  )
  if ($Password) {
    $args += @("-SafeWifiPassword", $Password)
  }
  $output = & powershell @args 2>&1
  $outputText = $output -join "`n"
  [pscustomobject]@{
    ssid = $Ssid
    exit_code = $LASTEXITCODE
    output = $outputText
    passed = ($LASTEXITCODE -eq 0 -and $outputText -match '"name"\s*:\s*"real-wifi-switch"' -and $outputText -match '"status"\s*:\s*"pass"')
  }
}

try {
  if (-not (Test-IsAdmin)) {
    $result = New-Result $false "administrator" "This script must run elevated."
    Save-Result $result
    exit 1
  }
  if (-not $PackageRoot -or -not (Test-Path -LiteralPath (Join-Path $PackageRoot "scripts\external-acceptance.ps1"))) {
    $result = New-Result $false "package-root" "PackageRoot does not contain scripts\external-acceptance.ps1." @{ package_root = $PackageRoot }
    Save-Result $result
    exit 1
  }

  $locationEvidence = Set-LocationPermission
  $interfaces = Invoke-CheckedCommand "netsh.exe" "wlan show interfaces"
  $profiles = Get-KnownProfiles
  $networks = Get-AvailableNetworks
  if ($networks.command.exit_code -ne 0) {
    $result = New-Result $false "wifi-scan" "netsh wlan show networks failed after enabling location." @{
      location = $locationEvidence
      interfaces = $interfaces
      profiles = $profiles
      networks = $networks
    }
    Save-Result $result
    exit 1
  }

  $target = Select-TargetSsid $profiles.profiles $networks.networks $SafeWifiSsid
  if (-not $target.ssid) {
    $result = New-Result $false "target-ssid" "No safe target SSID could be selected from currently scanned saved profiles." @{
      location = $locationEvidence
      interfaces = $interfaces
      profiles = $profiles.profiles
      networks = $networks.networks
      target = $target
    }
    Save-Result $result
    exit 1
  }

  $attempts = @()
  $ssidsToTry = @()
  if ($SafeWifiSsid) {
    $ssidsToTry = @($target.ssid)
  } else {
    $ssidsToTry = @($target.candidates | Select-Object -ExpandProperty ssid -Unique)
    if ($ssidsToTry.Count -eq 0) { $ssidsToTry = @($target.ssid) }
  }
  $success = $null
  foreach ($ssid in $ssidsToTry) {
    $attempt = Invoke-RealWifiAcceptance $PackageRoot $ssid $SafeWifiPassword
    $attempts += $attempt
    if ($attempt.passed) {
      $success = $attempt
      break
    }
  }

  $result = New-Result ($null -ne $success) "real-wifi-acceptance" "Elevated real WiFi acceptance finished." @{
    location = $locationEvidence
    interfaces_before_acceptance = $interfaces
    selected_target = $target
    attempted_ssids = $ssidsToTry
    attempts = $attempts
    passed_attempt = $success
  }
  Save-Result $result
  if (-not $success) { exit 1 }
  exit 0
} catch {
  $result = New-Result $false "exception" $_.Exception.Message @{
    exception_type = $_.Exception.GetType().FullName
    script_stack = $_.ScriptStackTrace
  }
  Save-Result $result
  exit 1
}
