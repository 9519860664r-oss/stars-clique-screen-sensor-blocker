@echo off
rem Dev-only: builds preview_ru.exe / preview_en.exe (2x scale, for screenshots).
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%PATH%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -products * -latest -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
cl /nologo /utf-8 /O1 /MT /DPREVIEW_SCALE=2.0f preview.c /Fo:preview.obj /Fe:preview_ru.exe /link /SUBSYSTEM:CONSOLE >nul || exit /b 1
cl /nologo /utf-8 /O1 /MT /DPREVIEW_SCALE=2.0f /DLANG_EN preview.c /Fo:preview.obj /Fe:preview_en.exe /link /SUBSYSTEM:CONSOLE >nul || exit /b 1
del preview.obj 2>nul
echo PREVIEW OK
