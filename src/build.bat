@echo off
rem Builds the Russian (bin\ru) and English (bin\en) versions with MSVC.
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%PATH%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -products * -latest -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
set "EXE=Stars Clique Screen Sensor Blocker.exe"
set "CFLAGS=/nologo /utf-8 /O1 /GS- /W4 /MT"
set "LFLAGS=/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF"
if not exist ..\bin\ru mkdir ..\bin\ru
if not exist ..\bin\en mkdir ..\bin\en
rc /nologo /fo app.res app.rc || exit /b 1
cl %CFLAGS% main.c app.res /Fo:main.obj /Fe:"..\bin\ru\%EXE%" /link %LFLAGS% || exit /b 1
cl %CFLAGS% /DLANG_EN main.c app.res /Fo:main.obj /Fe:"..\bin\en\%EXE%" /link %LFLAGS% || exit /b 1
del main.obj app.res 2>nul
echo BUILD OK
