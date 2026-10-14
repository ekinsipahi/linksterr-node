@echo off
rem Builds dist\linksterr-node.exe with MSVC (Visual Studio 2022+ or Build Tools).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found - install Visual Studio Build Tools with the C++ workload.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
  echo No MSVC C++ toolset found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
if not exist build mkdir build
if not exist dist mkdir dist
rem /MT: static CRT, so the exe runs on any Windows 10/11 without a redistributable.
cl /nologo /std:c++20 /EHsc /O2 /W4 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /Fobuild\ src\util.cpp src\json.cpp src\net.cpp src\ws.cpp src\tunnel.cpp src\main.cpp ^
   /Fe:dist\linksterr-node.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
echo Built dist\linksterr-node.exe
