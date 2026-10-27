@echo off
rem One-click build for ZelRaccy Commission Manager.
rem Output: CommissionManager.exe next to this file.  "build.bat tests" also builds build\core_tests.exe.
setlocal EnableExtensions
cd /d "%~dp0"
set "OBJ=%~dp0build\obj"
if not exist "%OBJ%" mkdir "%OBJ%"
set "LIBS=kernel32.lib user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib windowscodecs.lib uxtheme.lib msimg32.lib advapi32.lib"

rem ---- 1) MSVC: already in a Developer Command Prompt, or found via vswhere ----
where cl.exe >nul 2>nul && goto :msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if defined VSDIR if exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
    goto :msvc
)

rem ---- 2) MinGW-w64 g++ (MSYS2) ----
where g++.exe >nul 2>nul && goto :mingw
if exist "C:\msys64\ucrt64\bin\g++.exe" (set "PATH=C:\msys64\ucrt64\bin;%PATH%" & goto :mingw)
if exist "C:\msys64\mingw64\bin\g++.exe" (set "PATH=C:\msys64\mingw64\bin;%PATH%" & goto :mingw)

echo.
echo No C++ compiler was found.
echo Install "Build Tools for Visual Studio" with the "Desktop development with C++" workload:
echo   https://visualstudio.microsoft.com/visual-cpp-build-tools/
goto :fail

:msvc
echo Building with MSVC...
rc.exe /nologo /fo "%OBJ%\app.res" src\app.rc || goto :fail
cl.exe /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /MT /DUNICODE /D_UNICODE /Fo"%OBJ%\\" ^
    src\main.cpp src\core.cpp "%OBJ%\app.res" /Fe"%~dp0CommissionManager.exe" ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:NO %LIBS% || goto :fail
if /i "%~1"=="tests" (
    if not exist "%OBJ%\tests" mkdir "%OBJ%\tests"
    cl.exe /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /MT /DUNICODE /D_UNICODE /Fo"%OBJ%\tests\\" ^
        tests\core_tests.cpp src\core.cpp /Fe"%~dp0build\core_tests.exe" /link /SUBSYSTEM:CONSOLE %LIBS% || goto :fail
)
goto :ok

:mingw
echo Building with MinGW-w64 g++...
windres -O coff -i src\app.rc -o "%OBJ%\app_res.o" || goto :fail
g++ -std=c++17 -O2 -Wall -municode -mwindows -DUNICODE -D_UNICODE -static -s ^
    src\main.cpp src\core.cpp "%OBJ%\app_res.o" -o "%~dp0CommissionManager.exe" ^
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -loleaut32 -luuid -lwindowscodecs -luxtheme -lmsimg32 || goto :fail
if /i "%~1"=="tests" (
    g++ -std=c++17 -O2 -Wall -municode -DUNICODE -D_UNICODE -static tests\core_tests.cpp src\core.cpp ^
        -o "%~dp0build\core_tests.exe" -lole32 -loleaut32 -luuid -lwindowscodecs -lgdi32 || goto :fail
)
goto :ok

:ok
echo.
echo Built: %~dp0CommissionManager.exe
endlocal & exit /b 0

:fail
echo.
echo BUILD FAILED.
endlocal & exit /b 1
