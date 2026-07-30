@echo off
REM ==========================================================================
REM  run.bat -- compile a Strata program to a native executable and run it.
REM
REM  Usage:
REM     run.bat  ^<file.strata^>
REM
REM  The file must define an entry point the C runtime can call, i.e.
REM  `int main()`. Files that call `extern` host functions need a host driver
REM  instead (see samples\hosts\*.c). Tools can be overridden with the
REM  STRATAC and CLANG environment variables.
REM ==========================================================================
setlocal enabledelayedexpansion

if "%~1"=="" goto usage

set "SRC=%~1"
set "SRC=%SRC:/=\%"

if not exist "%SRC%" (
  echo error: input file not found: "%SRC%"
  exit /b 2
)

REM --- locate tools (env override, else default repo / install paths) -------
if "%STRATAC%"=="" set "STRATAC=%~dp0build\default\bin\stratac.exe"
if "%CLANG%"==""   set "CLANG=C:\Users\andre\llvm-x64\bin\clang.exe"

if not exist "%STRATAC%" (
  echo error: stratac not found: "%STRATAC%"  ^(set STRATAC to override^)
  exit /b 1
)
if not exist "%CLANG%" (
  echo error: clang not found: "%CLANG%"  ^(set CLANG to override^)
  exit /b 1
)

set "BASE=%~n1"
set "OBJ=%TEMP%\strata_%BASE%.o"
set "EXE=%TEMP%\strata_%BASE%.exe"
set "LOG=%TEMP%\strata_%BASE%.log"

echo [1/3] compile  %SRC% -^> %OBJ%
"%STRATAC%" --emit obj "%SRC%" -o "%OBJ%" > "%LOG%" 2>&1
if errorlevel 1 goto fail_compile

echo [2/3] link     %OBJ% -^> %EXE%
"%CLANG%" "%OBJ%" -o "%EXE%" > "%LOG%" 2>&1
if errorlevel 1 goto fail_link

echo [3/3] run      %EXE%
echo.
"%EXE%"
set "RC=%ERRORLEVEL%"
echo.
del "%OBJ%" >nul 2>&1
del "%EXE%" >nul 2>&1
del "%LOG%" >nul 2>&1
echo exit code: %RC%
exit /b %RC%

:fail_compile
echo.
echo compile failed:
type "%LOG%"
del "%OBJ%" "%LOG%" >nul 2>&1
exit /b 1

:fail_link
echo.
echo link failed. Does "%SRC%" define `int main()`^? Calls to `extern` functions
echo need a host driver to link against ^(see samples\hosts\*.c^).
type "%LOG%"
del "%OBJ%" "%EXE%" "%LOG%" >nul 2>&1
exit /b 1

:usage
echo Usage: run.bat ^<file.strata^>
echo.
echo Compiles the program to a native executable with stratac + clang and runs it.
echo The file must define `int main()`; `extern`-using files need a host driver.
exit /b 2
