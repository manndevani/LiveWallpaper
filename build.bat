@echo off
rem Builds build\livewall.exe with MSVC. No C runtime, no installer.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto :have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl >nul 2>nul || (echo MSVC not found. Install "Desktop development with C++". & exit /b 1)
:have_cl

if not exist build mkdir build
set SH=src\shaders

rem Shaders are compiled now and embedded, so the app never needs the HLSL compiler
rem (except for user-supplied .hlsl files).
fxc /nologo /T vs_4_0 /E vs_main /O3 /Vn g_vs /Fh build\vs.h %SH%\fullscreen.hlsl >nul || exit /b 1
for %%s in (aurora dunes nightsky ripples glowgrid blit dim) do (
    fxc /nologo /T ps_4_0 /E ps_main /O3 /I %SH% /Vn g_ps_%%s /Fh build\ps_%%s.h %SH%\%%s.hlsl >nul || exit /b 1
)

cl /nologo /utf-8 /O1 /GS- /Gy /Zl /W4 /wd4201 /fp:fast /GR- /EHs-c- /Zc:threadSafeInit- ^
   /Ibuild /Isrc src\*.c src\*.cpp /Fobuild\ /Fe:build\livewall.exe ^
   /link /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
   kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib uuid.lib ^
   dwmapi.lib wtsapi32.lib d3d11.lib dxgi.lib dxguid.lib mfplat.lib mfuuid.lib windowscodecs.lib ^
   d2d1.lib dwrite.lib || exit /b 1

for %%f in (build\livewall.exe) do echo Built %%f (%%~zf bytes)
