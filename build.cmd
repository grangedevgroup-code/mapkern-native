@echo off
setlocal
if not defined VCINSTALLDIR (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
)
if not defined VCINSTALLDIR call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake -S "%~dp0." -B "%~dp0build\windows" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo %* || exit /b 1
cmake --build "%~dp0build\windows" || exit /b 1
