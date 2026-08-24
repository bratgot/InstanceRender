@echo off
REM Launch Nuke with the InstanceRender Hydra delegate available.
REM
REM Nuke builds its renderer list ONCE while it starts up, so the delegate has to
REM be on PXR_PLUGINPATH_NAME before Nuke runs - registering it from init.py or
REM menu.py is too late in a GUI session.
REM
REM   hydra_launch.bat "C:\Program Files\Nuke17.1v1\Nuke17.1.exe" [args]
REM
REM The delegate only does anything in Nuke 17.1 and later: that is where the
REM Viewer's renderer menu and the GeoRender node appear.
setlocal EnableDelayedExpansion

REM %~dp0 MUST be read before any shift.  Batch shifts %0 along with everything
REM else, so after the shift below "the folder this script is in" had quietly
REM become the folder of the first ARGUMENT - Nuke's own - and the delegate was
REM looked for inside the Nuke installation, where it will never be:
REM   InstanceRender: no Hydra delegate for Nuke17.1 in C:\Program Files\Nuke17.1v1\
set "IR_HERE=%~dp0"

set "NUKE_EXE=%~1"
if "%NUKE_EXE%"=="" (
  echo usage: hydra_launch.bat "path\to\Nuke17.1.exe" [nuke arguments]
  exit /b 1
)

REM Everything after the executable, gathered one at a time.  %* is NOT affected
REM by shift, so passing it on would have handed Nuke its own path as an argument
REM to open.
set "NUKE_ARGS="
shift
:collect
if "%~1"=="" goto collected
set "NUKE_ARGS=!NUKE_ARGS! "%~1""
shift
goto collect
:collected

REM this script sits in ~/.nuke/InstanceRender, next to the per-version builds;
REM the build folder is named after the Nuke executable (Nuke17.1.exe -> nuke17.1)
for %%f in ("%NUKE_EXE%") do set "NUKE_NAME=%%~nf"
set "IR_BUILD=%NUKE_NAME:Nuke=nuke%"
set "IR_HYDRA=%IR_HERE%%IR_BUILD%\hydra\hdInstanceRender\resources"

if not exist "%IR_HYDRA%\plugInfo.json" (
  echo InstanceRender: no Hydra delegate for %NUKE_NAME% in %IR_HERE%
) else (
  if defined PXR_PLUGINPATH_NAME (
    set "PXR_PLUGINPATH_NAME=!PXR_PLUGINPATH_NAME!;!IR_HYDRA!"
  ) else (
    set "PXR_PLUGINPATH_NAME=!IR_HYDRA!"
  )
  echo InstanceRender: Hydra delegate at !IR_HYDRA!
)

"%NUKE_EXE%"!NUKE_ARGS!
endlocal
