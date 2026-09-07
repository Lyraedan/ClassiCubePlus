@echo off
setlocal
cd /d "%~dp0"

REM ------------------------------------------------------------------
REM Builds ClassiCube (Release x64) into build\bin and moves the
REM runnable exe (+ any DLLs) up into build\, so build root stays tidy.
REM Requires Visual Studio with the "Desktop development with C++"
REM workload (MSBuild + MSVC toolchain).
REM ------------------------------------------------------------------

REM --- Locate MSBuild via vswhere (VS 2019+) ---
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio Installer not found. Install Visual Studio with the C++ workload.
    exit /b 1
)
"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" > "%TEMP%\cc_msbuild.txt" 2>nul
set /p MSBUILD= < "%TEMP%\cc_msbuild.txt"
del /q "%TEMP%\cc_msbuild.txt" >nul 2>&1
if not defined MSBUILD (
    echo [ERROR] MSBuild not found. Install Visual Studio with the C++ workload.
    exit /b 1
)

REM --- Build into build\bin ---
if not exist "build\bin" mkdir "build\bin"
echo Building into build\bin ...
"%MSBUILD%" src\ClassiCube.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 "/p:OutDir=../build/bin/" "/p:IntDir=../build/bin/" /m /v:minimal /nologo
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

REM --- Move the runnable exe (+ any DLLs) up into build\ ---
if not exist "build\bin\ClassiCube.exe" (
    echo [ERROR] build\bin\ClassiCube.exe was not produced by the build.
    exit /b 1
)
if exist "build\bin\*.dll" copy /y "build\bin\*.dll" "build\" >nul
copy /y "build\bin\ClassiCube.exe" "build\ClassiCube.exe" >nul
if errorlevel 1 (
    echo [ERROR] Could not overwrite build\ClassiCube.exe. Close ClassiCube if it is running, then rerun this script.
    exit /b 1
)
del /q "build\bin\ClassiCube.exe" >nul 2>&1

REM --- Tidy build root: keep only bin\, the exe, and asset folders ---
del /q "build\*.obj" "build\*.pdb" "build\*.lib" "build\*.exp" "build\*.res" "build\*.tlog" "build\*.log" >nul 2>&1

REM --- Ensure the runtime data folders ClassiCube expects exist ---
if not exist "build\audio"        mkdir "build\audio"  >nul
if not exist "build\dbg"          mkdir "build\dbg"    >nul
if not exist "build\maps"         mkdir "build\maps"   >nul
if not exist "build\plugins"      mkdir "build\plugins" >nul
if not exist "build\texpacks"     mkdir "build\texpacks" >nul
if not exist "build\texturecache" mkdir "build\texturecache" >nul

echo.
echo Done. Executable: build\ClassiCube.exe
endlocal
exit /b 0