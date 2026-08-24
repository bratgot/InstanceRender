<#
.SYNOPSIS
  Build the distributable zip: dist\InstanceRender-<version>-Nuke14.1-17.1-win64.zip

.DESCRIPTION
  Stages what a USER needs and nothing else: the plugin built for each Nuke
  minor version, the loader scripts, icons, licences and the documentation in
  packaging\.

  The runtime libraries (Embree, oneTBB, the CUDA runtime) are staged ONCE, in
  runtime\, rather than copied into all six build folders - they are identical
  across builds and one of them is 36 MB, so six copies would quadruple the
  download for nothing. install.ps1 copies them beside each build it installs,
  which is where the plugin looks for them.

  .pdb files are deliberately NOT packaged. They travel with a developer
  install, are worth more than the DLL to anyone reverse engineering it, and
  are of no use to someone who just wants to render.

.EXAMPLE
  .\build.ps1 -All -Install     # build everything first
  .\package.ps1                 # -> dist\InstanceRender-0.20.0-Nuke14.1-17.1-win64.zip

.EXAMPLE
  .\package.ps1 -Version 0.21.0 -Versions 17.0,17.1
#>
param(
    [string]$Version  = "0.20.0",
    [string[]]$Versions = @("14.1", "15.2", "16.0", "16.1", "17.0", "17.1"),
    [string]$EmbreeRoot = "",
    [string]$CudaRoot   = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
# Name the zip after what is actually in it. Hard-coding the full range
# would mislabel a trimmed release as covering every version.
$span  = if ($Versions.Count -eq 1) { "Nuke$($Versions[0])" } else { "Nuke$($Versions[0])-$($Versions[-1])" }
$name  = "InstanceRender-$Version-$span-win64"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
$plug  = Join-Path $stage "InstanceRender"

function FileSha($path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try { return (($sha.ComputeHash($fs) | ForEach-Object { $_.ToString("x2") }) -join "") }
    finally { $fs.Dispose(); $sha.Dispose() }
}

function Need($path, $what) {
    if (-not (Test-Path $path)) { throw "$what not found: $path" }
    return $path
}

# ---- where did this machine's build get Embree and CUDA from? ---------------
# Read it out of a CMakeCache rather than hard-coding a path that is only true
# on one machine.
function CacheValue($cache, $key) {
    if (-not (Test-Path $cache)) { return "" }
    $line = Select-String -Path $cache -Pattern "^$key(:[A-Z]+)?=" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($line) { return ($line.Line -split "=", 2)[1] }
    return ""
}
$firstCache = ""
foreach ($v in $Versions) {
    $c = Join-Path $root "build$v\CMakeCache.txt"
    if (Test-Path $c) { $firstCache = $c; break }
}
if (-not $EmbreeRoot) { $EmbreeRoot = CacheValue $firstCache "EMBREE_ROOT" }
if (-not $CudaRoot)   { $CudaRoot   = CacheValue $firstCache "CUDA_TOOLKIT_ROOT" }

# ---- check every build is there before deleting anything -------------------
foreach ($v in $Versions) {
    Need (Join-Path $root "build$v\Release\InstanceRender.dll") "the Nuke $v build (run .\build.ps1 -All first)" | Out-Null
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $plug | Out-Null

# ---- the plugin, one folder per Nuke minor version -------------------------
foreach ($v in $Versions) {
    $rel = Join-Path $root "build$v\Release"
    $vd  = Join-Path $plug "nuke$v"
    New-Item -ItemType Directory -Force $vd | Out-Null
    Copy-Item (Join-Path $rel "InstanceRender.dll") $vd

    # The Hydra delegate, where this Nuke has one (17.0+). Its layout is fixed
    # by plugInfo.json, which says LibraryPath ../hdInstanceRender.dll relative
    # to Root ".." - so the DLL sits in hydra\ and the json two levels down in
    # hydra\hdInstanceRender\resources\. Flatten it and USD stops finding it.
    $hd = Join-Path $rel "hdInstanceRender.dll"
    if (Test-Path $hd) {
        $hy = Join-Path $vd "hydra"
        New-Item -ItemType Directory -Force (Join-Path $hy "hdInstanceRender\resources") | Out-Null
        Copy-Item $hd $hy
        Copy-Item (Need (Join-Path $root "src\hydra\plugInfo.json") "plugInfo.json") (Join-Path $hy "hdInstanceRender\resources")
    }
}

# ---- the loader scripts, icons and licence that sit beside the builds -------
foreach ($f in @("nuke\init.py", "nuke\menu.py", "nuke\hydra_launch.bat", "nuke\hydra_debug.bat",
                 "LICENSE", "THIRD_PARTY_NOTICES.md")) {
    Copy-Item (Need (Join-Path $root $f) $f) $plug
}
New-Item -ItemType Directory -Force (Join-Path $plug "icons") | Out-Null
# _contact_sheet.png is a proof sheet used while drawing the icons - it is
# not a node icon and has no business in a release.
Get-ChildItem (Join-Path $root "nuke\icons") -Filter *.png |
    Where-Object { $_.Name -notlike "_*" } |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $plug "icons") }

# ---- ToolSets --------------------------------------------------------------
$tsSrc = Join-Path $root "nuke\ToolSets\InstanceRender_AOVs.nk"
if (Test-Path $tsSrc) {
    New-Item -ItemType Directory -Force (Join-Path $stage "ToolSets\InstanceRender") | Out-Null
    Copy-Item $tsSrc (Join-Path $stage "ToolSets\InstanceRender")
}

# ---- the shared runtime, staged once ---------------------------------------
$rt = Join-Path $stage "runtime"
New-Item -ItemType Directory -Force $rt | Out-Null
$firstRel = Join-Path $root "build$($Versions[0])\Release"
foreach ($dll in @("embree4.dll", "tbb12.dll", "cudart64_12.dll")) {
    Copy-Item (Need (Join-Path $firstRel $dll) "runtime library $dll") $rt
}
# Identical across builds is an assumption worth checking, not asserting: if a
# build were linked against a different Embree, shipping one copy would break
# the others in a way nobody would trace back to the packager.
foreach ($v in $Versions) {
    foreach ($dll in @("embree4.dll", "tbb12.dll", "cudart64_12.dll")) {
        $a = FileSha (Join-Path $rt $dll)
        $b = FileSha (Join-Path $root "build$v\Release\$dll")
        if ($a -ne $b) { throw "$dll differs between build$($Versions[0]) and build$v - they cannot share one copy" }
    }
}

# ---- third-party licence texts ---------------------------------------------
$lic = Join-Path $stage "licenses"
New-Item -ItemType Directory -Force $lic | Out-Null
$embreeLic = Join-Path $EmbreeRoot "doc\LICENSE.txt"
$tbbLic    = Join-Path $EmbreeRoot "doc\third-party-programs-TBB.txt"
$cudaLic   = Join-Path $CudaRoot "EULA.txt"
Copy-Item (Need $embreeLic "Embree LICENSE.txt (pass -EmbreeRoot)") (Join-Path $lic "Embree-LICENSE.txt")
if (Test-Path $tbbLic) { Copy-Item $tbbLic (Join-Path $lic "oneTBB-third-party-programs.txt") }
Copy-Item (Need $cudaLic "CUDA EULA.txt (pass -CudaRoot)") (Join-Path $lic "NVIDIA-CUDA-EULA.txt")

# ---- the documentation and the installer -----------------------------------
foreach ($f in @("README.md", "INSTALL.md", "COMPATIBILITY.md", "install.ps1", "install.bat", "uninstall.ps1")) {
    Copy-Item (Need (Join-Path $root "packaging\$f") "packaging\$f") $stage
}
Copy-Item (Join-Path $root "LICENSE") $stage
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") $stage
# WriteAllText, not Set-Content -Encoding UTF8: that writes a BOM on Windows
# PowerShell and VERSION.txt then opens as "ï»¿CopyToPoints 1.0" in anything
# that does not strip one.
[System.IO.File]::WriteAllText((Join-Path $stage "VERSION.txt"), "InstanceRender $Version`r`nbuilt $(Get-Date -Format 'yyyy-MM-dd')`r`nNuke $($Versions -join ', ') - Windows x64`r`n", (New-Object System.Text.UTF8Encoding($false)))

# ---- nothing that should not ship ------------------------------------------
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Name -like "*.pdb" -or $_.Name -like "*.ilk" -or $_.Name -like "*.exp" }
if ($bad) { throw "debug files staged: $($bad.Name -join ', ')" }

# ---- zip -------------------------------------------------------------------
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
$files = (Get-ChildItem $stage -Recurse -File).Count
Write-Host ""
Write-Host "$zip"
Write-Host "  $mb MB, $files files, Nuke $($Versions -join ', ')"
