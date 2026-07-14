@echo off
setlocal enabledelayedexpansion

:: Find vcvars64.bat
set VCVARS_PATH=
set PATHS[0]="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATHS[1]="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATHS[2]="C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATHS[3]="C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
set PATHS[4]="C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"

for /l %%i in (0,1,4) do (
    if not defined VCVARS_PATH (
        set PATH_VAR=!PATHS[%%i]!
        set PATH_VAR_NQ=!PATH_VAR:~1,-1!
        if exist "!PATH_VAR_NQ!" (
            set VCVARS_PATH=!PATH_VAR!
        )
    )
)

if not defined VCVARS_PATH (
    echo Error: vcvars64.bat not found in standard Visual Studio locations.
    echo Please make sure Visual Studio or Visual Studio Build Tools are installed.
    exit /b 1
)

echo Found VS environment setup: %VCVARS_PATH%
call %VCVARS_PATH%

cd %~dp0

:: Check if pdcurses is cloned
if not exist pdcurses\wincon\Makefile.vc (
    echo PDCurses source not found, cloning...
    git clone --depth 1 https://github.com/wmcbrine/PDCurses.git pdcurses
    if errorlevel 1 (
        echo Error: failed to clone PDCurses.
        exit /b 1
    )
)

:: Compile PDCurses if pdcurses.lib doesn't exist
if not exist pdcurses\wincon\pdcurses.lib (
    echo Compiling PDCurses library...
    cd pdcurses\wincon
    nmake -f Makefile.vc WIDE=Y UTF8=Y
    if errorlevel 1 (
        echo Error: failed to build PDCurses library.
        exit /b 1
    )
    cd ..\..
)

:: Compile qdux.c
echo Compiling qdux.c ported for Windows...
cl /nologo /Ipdcurses /Ipdcurses/wincon /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /std:c11 /experimental:c11atomics /MT /O2 qdux.c pdcurses\wincon\pdcurses.lib user32.lib advapi32.lib shell32.lib /Fe:qdux.exe

if errorlevel 1 (
    echo Error: failed to compile qdux.exe.
    exit /b 1
)

echo Build successful: win64\qdux.exe
