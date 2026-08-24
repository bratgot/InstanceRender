param(
    [string]$NukeRoot = "C:/Program Files/Nuke17.0v4",
    [string]$Generator = "Visual Studio 17 2022",
    [switch]$Clean,
    [switch]$Install,
    [switch]$Gpu,
    [string]$InstallPrefix = (Join-Path $env:USERPROFILE ".nuke")
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
# one build directory per Nuke MINOR version: 16.0 and 16.1 are not the same NDK
$mm = [regex]::Match($NukeRoot, "Nuke(\d+)\.(\d+)")
if (-not $mm.Success) { throw "cannot read a Nuke version out of $NukeRoot" }
$ver = $mm.Groups[1].Value + "." + $mm.Groups[2].Value
$build = Join-Path $root "build$ver"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
$optix = if ($Gpu) { "ON" } else { "OFF" }
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    & cmake -G $Generator -A x64 -DNUKE_ROOT="$NukeRoot" -DIR_WITH_OPTIX:BOOL=$optix -S $root -B $build
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
} else {
    & cmake -DIR_WITH_OPTIX:BOOL=$optix -S $root -B $build | Out-Null
}
& cmake --build $build --config Release 2>&1 | ForEach-Object { if ($_ -match "error|warning C4\d+: .*unre|\.dll|PTX") { $_ } }
if ($LASTEXITCODE -ne 0) { throw "build failed" }
if ($Install) { & cmake --install $build --config Release --prefix "$InstallPrefix"; if ($LASTEXITCODE -ne 0) { throw "install failed" } }
