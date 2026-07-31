@echo off
REM ==========================================================================
REM  gen_msvc_lib.bat -- generate an MSVC-compatible import library (.lib)
REM
REM  Run from a "Developer Command Prompt for Visual Studio":
REM    gen_msvc_lib.bat
REM
REM  Produces strata.lib alongside strata.def.
REM ==========================================================================
setlocal

set "DEF=%~dp0strata.def"
set "LIB_OUT=%~dp0strata.lib"

if not exist "%DEF%" (
  echo error: strata.def not found at "%DEF%"
  exit /b 1
)

echo Generating MSVC import library ...
lib /def:"%DEF%" /out:"%LIB_OUT%" /machine:x64
if errorlevel 1 (
  echo error: lib.exe failed. Run from a Visual Studio Developer Command Prompt.
  exit /b 1
)

echo.
echo Done. In your MSVC project:
echo   3. Copy:  strata.lib  strata.h  strata.dll  LLVM-C.dll
echo   2. Link:  strata.lib
echo   3. Include dir with strata.h
echo   4. At runtime: strata.dll + LLVM-C.dll on PATH or next to .exe
