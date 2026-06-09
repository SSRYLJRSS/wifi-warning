param(
  [string]$ExePath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sourceBin = Join-Path $root "build\mingw\bin"
$portableBin = $root
$isPortableLayout = (Test-Path -LiteralPath (Join-Path $portableBin "ww-smoke.exe")) -and -not (Test-Path -LiteralPath (Join-Path $root "CMakeLists.txt"))
$bin = if (Test-Path -LiteralPath (Join-Path $sourceBin "ww-smoke.exe")) { $sourceBin } elseif (Test-Path -LiteralPath (Join-Path $portableBin "ww-smoke.exe")) { $portableBin } else { $sourceBin }

if (-not $ExePath) {
  $smokeRoot = if ($isPortableLayout) { Join-Path ([System.IO.Path]::GetTempPath()) "WiFiWarningPortableRealShortcutSmoke" } else { Join-Path $root "build\real-shortcut-smoke" }
  $appDir = Join-Path $smokeRoot "App"
  New-Item -ItemType Directory -Force -Path $appDir | Out-Null
  $ExePath = Join-Path $appDir "target.exe"
  if (-not (Test-Path -LiteralPath $ExePath)) {
    Set-Content -LiteralPath $ExePath -Encoding ASCII -Value "placeholder"
  }
}

$desktop = [Environment]::GetFolderPath("DesktopDirectory")
$startMenu = [Environment]::GetFolderPath("StartMenu")
if (-not $desktop -or -not $startMenu) {
  throw "Unable to resolve Desktop or Start Menu shell folder"
}
$taskbar = Join-Path $env:APPDATA "Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar"

$stamp = "WiFiWarningSmoke-$([Guid]::NewGuid().ToString('N'))"
$shortcutTargets = @(
  [pscustomobject]@{ name = "desktop"; path = (Join-Path $desktop "$stamp-desktop.lnk"); required = $true },
  [pscustomobject]@{ name = "start-menu"; path = (Join-Path $startMenu "$stamp-start-menu.lnk"); required = $true }
)
if (Test-Path -LiteralPath $taskbar) {
  $shortcutTargets += [pscustomobject]@{ name = "taskbar"; path = (Join-Path $taskbar "$stamp-taskbar.lnk"); required = $true }
}
$paths = @($shortcutTargets | ForEach-Object { $_.path })

foreach ($path in $paths) {
  $parent = Split-Path -Parent $path
  $resolvedParent = [System.IO.Path]::GetFullPath($parent)
  $allowed = @(
    [System.IO.Path]::GetFullPath($desktop),
    [System.IO.Path]::GetFullPath($startMenu),
    [System.IO.Path]::GetFullPath($taskbar)
  )
  if ($allowed -notcontains $resolvedParent) {
    throw "Refusing to create shortcut outside the expected shell folders: $path"
  }
}

try {
  $shell = New-Object -ComObject WScript.Shell
  foreach ($path in $paths) {
    $shortcut = $shell.CreateShortcut($path)
    $shortcut.TargetPath = $ExePath
    $shortcut.WorkingDirectory = Split-Path -Parent $ExePath
    $shortcut.Description = "WiFi Warning smoke shortcut"
    $shortcut.Save()
  }

  $probe = Join-Path $bin "ww-smoke.exe"
  $outputLines = & $probe --scan $ExePath 2>&1
  $exitCode = $LASTEXITCODE
  $output = ($outputLines -join "`n")
  $scan = $null
  if ($output.Trim()) {
    try {
      $scan = $output | ConvertFrom-Json
    } catch {
      throw "Default shortcut scanner returned non-JSON output with exit ${exitCode}: $output"
    }
  }
  if ($exitCode -ne 0) {
    throw "Default shortcut scanner exited ${exitCode}: $output"
  }
  $foundPaths = @($scan.shortcuts | ForEach-Object { $_.path })
  $missing = @()
  foreach ($target in $shortcutTargets) {
    $expectedLeaf = [System.IO.Path]::GetFileName($target.path)
    if (-not ($foundPaths | Where-Object { [System.IO.Path]::GetFileName($_) -eq $expectedLeaf })) {
      $missing += $target.name
    }
  }
  if ($missing.Count) {
    throw "Default shortcut scanner did not report expected real shell folder shortcuts ($($missing -join ', ')): $output"
  }

  [pscustomobject]@{
    ok = $true
    target = $ExePath
    expected_roots = @($shortcutTargets | ForEach-Object { $_.name })
    scanner_output = $scan
  } | ConvertTo-Json -Depth 4
} finally {
  foreach ($path in $paths) {
    if ((Split-Path -Leaf $path) -like "WiFiWarningSmoke-*.lnk") {
      Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
  }
}
