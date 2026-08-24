@echo off
REM Launch Nuke with the InstanceRender Hydra delegate AND its trace switched on.
REM
REM Same as hydra_launch.bat, but it also points IR_HYDRA_LOG at a file, so the
REM delegate writes down what it is doing: the material network it was handed,
REM every texture it read, and every one it could not - which is the only way to
REM see why a texture goes missing in an interactive session, where nothing is
REM printed anywhere else.
REM
REM   hydra_debug.bat "C:\Program Files\Nuke17.1v1\Nuke17.1.exe" [args]
REM
REM The log is overwritten each launch and lands next to this script unless
REM IR_HYDRA_LOG is already set. Lines worth searching for when a texture is
REM missing:
REM
REM   material texture <path>: <reason>       the image did not load
REM   diffuseColor is connected but no image  ...and the surface fell back
REM   keeping the last good texture(s)        a read failed, the old one was used
REM   primvars on <prim>                      what st the mesh actually has
setlocal EnableDelayedExpansion

if "%~1"=="" (
  echo usage: hydra_debug.bat "path\to\Nuke17.1.exe" [nuke arguments]
  exit /b 1
)

if not defined IR_HYDRA_LOG set "IR_HYDRA_LOG=%~dp0delegate.log"
if exist "%IR_HYDRA_LOG%" del "%IR_HYDRA_LOG%"
echo InstanceRender: delegate trace -^> %IR_HYDRA_LOG%

call "%~dp0hydra_launch.bat" %*
endlocal
