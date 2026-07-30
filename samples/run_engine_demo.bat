@echo off
REM ==========================================================================
REM  run_engine_demo.bat -- run the engine demo in both AOT and JIT modes.
REM
REM  AOT:  stratac pre-compiles engine_demo.strata to an object, clang links
REM        it with the host (no Strata library needed at runtime -- ship this).
REM  JIT:  the CMake-built engine_demo.exe loads the .strata at runtime via
REM        the Strata JIT API (develop/iterate with this).
REM
REM  Both run the SAME script logic. Build the JIT target first:
REM    cmake --build --preset default --target engine_demo
REM  Then:
REM    samples\run_engine_demo.bat
REM ==========================================================================
setlocal

set "ROOT=%~dp0.."
set "STRATAC=%ROOT%\build\default\bin\stratac.exe"
set "CLANG=clang"
set "SCRIPT=%ROOT%\samples\engine_demo.strata"
set "HOST=%ROOT%\samples\hosts\engine_demo_host.c"
set "OBJ=%TEMP%\engine_demo.o"
set "AOT=%TEMP%\engine_demo_aot.exe"
set "JIT=%ROOT%\build\default\bin\engine_demo.exe"

echo ==================== AOT mode (pre-compiled, linked) ====================
echo [1] stratac --emit obj
"%STRATAC%" --emit obj "%SCRIPT%" -o "%OBJ%" >nul 2>&1
if errorlevel 1 ( echo stratac failed & exit /b 1 )
echo [2] clang link host + object
"%CLANG%" "%HOST%" "%OBJ%" -o "%AOT%" >nul 2>&1
if errorlevel 1 ( echo link failed & del "%OBJ%" >nul 2>&1 & exit /b 1 )
echo [3] run
"%AOT%"
del "%OBJ%" "%AOT%" >nul 2>&1

echo.
echo ==================== JIT mode (loaded at runtime) =======================
if not exist "%JIT%" (
  echo JIT exe not found: "%JIT%"
  echo Build it first: cmake --build --preset default --target engine_demo
  exit /b 1
)
"%JIT%"
