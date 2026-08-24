# Nuke ships pythonXY.dll but no import library, and the USD libraries reference
# python symbols, so the link needs one.  This makes it from the DLL's exports:
#     .\mkpythonlib.ps1 -NukeRoot "C:/Program Files/Nuke15.2v9"
param([string]$NukeRoot = "C:/Program Files/Nuke17.0v4")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$msvc = Get-ChildItem "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName "bin/Hostx64/x64"
# Nuke ships pythonXY.dll and (15/16) foundryboost_python*.dll with no import
# libraries; the USD headers reference symbols from both.
$dlls = @()
$dlls += Get-ChildItem (Join-Path $NukeRoot "python3*.dll") | Where-Object { $_.BaseName -match "^python3\d+$" }
$dlls += Get-ChildItem (Join-Path $NukeRoot "foundryboost_python*.dll") -ErrorAction SilentlyContinue
foreach ($dll in $dlls) {
    $name = $dll.BaseName
    $def = Join-Path $here "$name.def"
    "LIBRARY $name" | Set-Content $def
    "EXPORTS" | Add-Content $def
    & (Join-Path $bin "dumpbin.exe") /EXPORTS $dll.FullName |
        Select-Object -Skip 19 | ForEach-Object { $f = $_ -split "\s+" | Where-Object { $_ }; if ($f.Count -eq 4) { $f[3] } } |
        Add-Content $def
    & (Join-Path $bin "lib.exe") /DEF:$def /MACHINE:X64 /OUT:(Join-Path $here "$name.lib")
    Write-Host "wrote $name.lib"
}
