param(
  [string]$Configuration = "Release",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sourceBin = Join-Path $root "build\mingw\bin"
$portableBin = $root
$isPortableLayout = (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) -and -not (Test-Path -LiteralPath (Join-Path $root "CMakeLists.txt"))
$bin = if (Test-Path -LiteralPath (Join-Path $sourceBin "wifi-warning.exe")) { $sourceBin } elseif (Test-Path -LiteralPath (Join-Path $portableBin "wifi-warning.exe")) { $portableBin } else { $sourceBin }
$distZip = if ($isPortableLayout) { "" } else { Join-Path $root "build\dist\WiFiWarning-portable.zip" }
$workRoot = if ($isPortableLayout) { Join-Path ([System.IO.Path]::GetTempPath()) ("WiFiWarningPortableAcceptance-" + [System.Diagnostics.Process]::GetCurrentProcess().Id) } else { Join-Path $root "build" }

function Invoke-Step($Name, $Command, $Arguments, $WorkingDirectory = $root, $Environment = @{}) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Command
  $psi.Arguments = $Arguments
  $psi.WorkingDirectory = $WorkingDirectory
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  foreach ($key in $Environment.Keys) {
    if ($null -ne $psi.Environment) {
      $psi.Environment[$key] = [string]$Environment[$key]
    } else {
      $psi.EnvironmentVariables[$key] = [string]$Environment[$key]
    }
  }
  $started = Get-Date
  $process = [System.Diagnostics.Process]::Start($psi)
  $stdout = $process.StandardOutput.ReadToEnd()
  $stderr = $process.StandardError.ReadToEnd()
  $process.WaitForExit()
  $elapsed = [Math]::Round(((Get-Date) - $started).TotalSeconds, 2)
  [pscustomobject]@{
    name = $Name
    ok = $process.ExitCode -eq 0
    exit_code = $process.ExitCode
    elapsed_seconds = $elapsed
    stdout_tail = (($stdout -split "`r?`n") | Where-Object { $_ } | Select-Object -Last 20) -join "`n"
    stderr_tail = (($stderr -split "`r?`n") | Where-Object { $_ } | Select-Object -Last 20) -join "`n"
  }
}

function Join-ProcessArguments($Arguments) {
  $items = @($Arguments)
  ($items | ForEach-Object {
    $value = [string]$_
    if ($value -match '[\s"]') {
      '"' + ($value -replace '"', '\"') + '"'
    } else {
      $value
    }
  }) -join " "
}

function Invoke-PowerShellStep($Name, $Script, $ExtraArgs = @()) {
  $args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Script) + $ExtraArgs
  Invoke-Step $Name "powershell" (Join-ProcessArguments $args)
}

function New-Artifact($Name, $Path) {
  if (-not (Test-Path -LiteralPath $Path)) {
    return [pscustomobject]@{ name = $Name; path = $Path; exists = $false; bytes = 0 }
  }
  $item = Get-Item -LiteralPath $Path
  [pscustomobject]@{
    name = $Name
    path = $item.FullName
    exists = $true
    bytes = $item.Length
    last_write_time = $item.LastWriteTime.ToString("s")
  }
}

$results = New-Object System.Collections.Generic.List[object]

if (-not $SkipBuild -and -not $isPortableLayout) {
  $results.Add((Invoke-PowerShellStep "build-mingw" (Join-Path $root "scripts\build-mingw.ps1") @("-Configuration", $Configuration)))
}

$smokeEnvironment = @{}
if ($isPortableLayout) {
  $smokeEnvironment["WW_SMOKE_ROOT"] = Join-Path $workRoot "smoke"
}
$results.Add((Invoke-Step "ww-smoke" (Join-Path $bin "ww-smoke.exe") "" $root $smokeEnvironment))
$results.Add((Invoke-PowerShellStep "runtime-smoke" (Join-Path $root "scripts\runtime-smoke.ps1")))
$results.Add((Invoke-PowerShellStep "tray-smoke" (Join-Path $root "scripts\tray-smoke.ps1")))
$results.Add((Invoke-PowerShellStep "browser-smoke" (Join-Path $root "scripts\browser-smoke.ps1")))
if (-not $isPortableLayout) {
  $results.Add((Invoke-PowerShellStep "validate-cmake" (Join-Path $root "scripts\validate-cmake.ps1")))
}

$parserScript = @'
$ErrorActionPreference = "Stop"
$files = Get-ChildItem -LiteralPath ".\scripts" -Filter "*.ps1" | Select-Object -ExpandProperty FullName
foreach ($file in $files) {
  $tokens = $null
  $errors = $null
  [System.Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$errors) | Out-Null
  if ($errors.Count -gt 0) {
    throw "PowerShell parser failed for ${file}: $($errors[0].Message)"
  }
}
Write-Host "PowerShell parser checks passed: $($files.Count)"
'@
$parserPath = Join-Path $workRoot "local-acceptance-ps-parser.ps1"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $parserPath) | Out-Null
Set-Content -LiteralPath $parserPath -Value $parserScript -Encoding UTF8
$results.Add((Invoke-PowerShellStep "powershell-parser" $parserPath))
Remove-Item -LiteralPath $parserPath -Force -ErrorAction SilentlyContinue

foreach ($jsFile in @(
  ".\scripts\browser-smoke-runner.js",
  ".\frontend\js\api.js",
  ".\frontend\js\polling.js",
  ".\frontend\js\settings.js",
  ".\frontend\js\warning.js",
  ".\frontend\js\wifi_picker.js"
)) {
  $results.Add((Invoke-Step "node-check:$jsFile" "node" "--check `"$jsFile`""))
}

if (-not $isPortableLayout) {
  $results.Add((Invoke-PowerShellStep "package-portable" (Join-Path $root "scripts\package-portable.ps1") @("-Configuration", $Configuration)))
}

$artifacts = @(
  (New-Artifact "wifi-warning.exe" (Join-Path $bin "wifi-warning.exe")),
  (New-Artifact "ww-launch.exe" (Join-Path $bin "ww-launch.exe")),
  (New-Artifact "ww-smoke.exe" (Join-Path $bin "ww-smoke.exe"))
)
if ($distZip) {
  $artifacts += (New-Artifact "WiFiWarning-portable.zip" $distZip)
}

$failed = @($results | Where-Object { -not $_.ok })
$summary = [pscustomobject]@{
  ok = $failed.Count -eq 0
  generated_at = (Get-Date).ToString("s")
  configuration = $Configuration
  portable_layout = $isPortableLayout
  results = $results
  artifacts = $artifacts
}

$summary | ConvertTo-Json -Depth 6
if ($failed.Count -gt 0) { exit 1 }
