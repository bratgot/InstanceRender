<#
.SYNOPSIS
  Install InstanceRender into a Nuke plugin folder (~/.nuke by default).

.DESCRIPTION
  Copies the plugin folder, then copies the shared runtime (Embree, oneTBB, the
  CUDA runtime) in beside EACH build. The zip carries one copy of those three to
  stay small; they have to end up next to every InstanceRender.dll because the
  plugin loads them from its own directory by absolute path - it cannot find
  them anywhere else, and Embree in particular must be loaded after the oneTBB
  that ships here rather than the older TBB inside Nuke 14 to 16.

  Finally it adds one idempotent line to <prefix>\init.py so Nuke looks in the
  folder at all.

.EXAMPLE
  .\install.ps1
  Installs every build in the zip to $env:USERPROFILE\.nuke.

.EXAMPLE
  .\install.ps1 -Versions 17.1
  Installs only the Nuke 17.1 build. Each build carries its own copy of the
  runtime, so installing only what you use saves about 38 MB per version.

.EXAMPLE
  .\install.ps1 -Prefix D:\studio\nuke_plugins
  Installs somewhere else. That folder has to be on Nuke's plugin path.
#>
param(
    [string]$Prefix = (Join-Path $env:USERPROFILE ".nuke"),
    [string[]]$Versions = @()
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "InstanceRender"
$rt   = Join-Path $here "runtime"

if (-not (Test-Path $src)) { throw "InstanceRender folder not found next to this script - unpack the whole zip, not just install.ps1" }

# ---- which builds ----------------------------------------------------------
$all = Get-ChildItem $src -Directory | Where-Object { $_.Name -match '^nuke\d+\.\d+$' } | ForEach-Object { $_.Name }
if ($Versions.Count -gt 0) {
    $want = $Versions | ForEach-Object { if ($_ -match '^nuke') { $_ } else { "nuke$_" } }
    $sel = $all | Where-Object { $want -contains $_ }
    $missing = $want | Where-Object { $all -notcontains $_ }
    if ($missing) { throw "no build in this zip for: $($missing -join ', ') (it has: $($all -join ', '))" }
} else {
    $sel = $all
}
if (-not $sel) { throw "no builds found in $src" }

$dest = Join-Path $Prefix "InstanceRender"
Write-Host "Installing InstanceRender to $dest"
Write-Host "  builds: $($sel -join ', ')"

# ---- the parts every build shares ------------------------------------------
New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($f in @("init.py", "menu.py", "hydra_launch.bat", "hydra_debug.bat", "LICENSE", "THIRD_PARTY_NOTICES.md")) {
    $p = Join-Path $src $f
    if (Test-Path $p) { Copy-Item $p $dest -Force }
}
$icons = Join-Path $src "icons"
if (Test-Path $icons) {
    New-Item -ItemType Directory -Force (Join-Path $dest "icons") | Out-Null
    Copy-Item (Join-Path $icons "*") (Join-Path $dest "icons") -Force
}

# ---- each build, plus the runtime beside it --------------------------------
$runtime = @(Get-ChildItem $rt -Filter *.dll -ErrorAction SilentlyContinue)
if (-not $runtime) { throw "runtime folder is empty or missing - the plugin will not load without it" }

foreach ($v in $sel) {
    $vd = Join-Path $dest $v
    New-Item -ItemType Directory -Force $vd | Out-Null
    Copy-Item (Join-Path $src "$v\*") $vd -Recurse -Force
    foreach ($d in $runtime) { Copy-Item $d.FullName $vd -Force }
    # The Hydra delegate is loaded by USD's plugin registry, not by Nuke, and it
    # resolves its own dependencies from ITS folder - so the runtime goes there
    # a second time. plugInfo.json points one level up at hydra\, which is why
    # the .dll sits there and not inside hdInstanceRender\.
    $hy = Join-Path $vd "hydra"
    if (Test-Path $hy) { foreach ($d in $runtime) { Copy-Item $d.FullName $hy -Force } }
    $mb = [math]::Round((Get-ChildItem $vd -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
    Write-Host ("  {0,-10} {1} MB{2}" -f $v, $mb, $(if (Test-Path $hy) { "  (+ Hydra delegate)" } else { "" }))
}

# ---- ToolSets --------------------------------------------------------------
$ts = Join-Path $here "ToolSets"
if (Test-Path $ts) {
    Copy-Item $ts $Prefix -Recurse -Force
    Write-Host "  ToolSets > InstanceRender installed"
}

# ---- register the folder with Nuke -----------------------------------------
# In init.py rather than menu.py so it also applies to terminal (-t) sessions.
$initPy = Join-Path $Prefix "init.py"
$begin  = "# --- InstanceRender (auto-added by install.ps1) ---"
$block  = "$begin`r`nimport nuke`r`nnuke.pluginAddPath('./InstanceRender')`r`n# --- end InstanceRender ---`r`n"
$cur    = if (Test-Path $initPy) { Get-Content $initPy -Raw } else { "" }
if ($cur -match [regex]::Escape("nuke.pluginAddPath('./InstanceRender')")) {
    Write-Host "  $initPy already registers the folder"
} else {
    if ($cur -and -not $cur.EndsWith("`n")) { $cur += "`r`n" }
    # Written without a BOM: Set-Content -Encoding UTF8 adds one on Windows
    # PowerShell, and a user's init.py is nicer left as plain text.
    [System.IO.File]::WriteAllText($initPy, $cur + $block, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "  registered the folder in $initPy"
}

# ---- did we install a build for the Nuke that is actually here? -------------
$found = @()
foreach ($pf in @($env:ProgramFiles, "${env:ProgramFiles(x86)}")) {
    if ($pf -and (Test-Path $pf)) {
        $found += Get-ChildItem $pf -Directory -Filter "Nuke*" -ErrorAction SilentlyContinue |
                  ForEach-Object { if ($_.Name -match '^Nuke(\d+\.\d+)v\d+') { $Matches[1] } }
    }
}
$found = $found | Sort-Object -Unique
Write-Host ""
if ($found) {
    Write-Host "Nuke versions found on this machine: $($found -join ', ')"
    $un = $found | Where-Object { $sel -notcontains "nuke$_" }
    if ($un) {
        Write-Host "  NOTE: nothing was installed for $($un -join ', ')." -ForegroundColor Yellow
        Write-Host "  The build must match the Nuke MINOR version exactly; see COMPATIBILITY.md." -ForegroundColor Yellow
    }
} else {
    Write-Host "No Nuke installation was found in Program Files - that is only a warning;"
    Write-Host "it may simply be installed somewhere else."
}
Write-Host ""
Write-Host "Done. Start Nuke and look for InstanceRender on the 3D toolbar."
