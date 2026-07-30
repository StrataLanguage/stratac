@echo off
REM ==========================================================================
REM  run.bat -- compile a Strata program to a native executable and run it.
REM
REM  Usage:
REM     run.bat  ^<file.strata^>                   standalone (file needs `int main()`)
REM     run.bat  ^<file.strata^> ^<host.c^>          link a host driver (provides main + externs)
REM
REM  The host driver supplies `extern` functions and `int main`; the Strata file
REM  then does not need its own `main`. Tools can be overridden with the STRATAC
REM  and CLANG environment variables.
REM ==========================================================================
setlocal enabledelayedexpansion

if "%~1"=="" goto usage

set "SRC=%~1"
set "SRC=%SRC:/=\%"
if not exist "%SRC%" (
  echo error: input file not found: "%SRC%"
  exit /b 2
)

set "HOST="
if not "%~2"=="" (
  set "HOST=%~2"
  set "HOST=!HOST:/=\!"
  if not exist "!HOST!" (
    echo error: host file not found: "!HOST!"
    exit /b 2
  )
)

REM --- locate tools (env override, else defaults) -------------------------
if "%STRATAC%"=="" set "STRATAC=%~dp0build\default\bin\stratac.exe"
if "%CLANG%"==""   set "CLANG=clang"

if not exist "%STRATAC%" (
  echo error: stratac not found: "%STRATAC%"  ^(set STRATAC to override^)
  exit /b 1
)

set "BASE=%~n1"
set "OBJ=%TEMP%\strata_%BASE%.o"
set "EXE=%TEMP%\strata_%BASE%.exe"
set "LOG=%TEMP%\strata_%BASE%.log"

echo [1/3] compile  %SRC% -^> %OBJ%
"%STRATAC%" "%SRC%" -o "%OBJ%" > "%LOG%" 2>&1
if errorlevel 1 goto fail_compile

if defined HOST (
  echo [2/3] link     !HOST! + %OBJ% -^> %EXE%
  "%CLANG%" "!HOST!" "%OBJ%" -o "%EXE%" > "%LOG%" 2>&1
) else (
  echo [2/3] link     %OBJ% -^> %EXE%
  "%CLANG%" "%OBJ%" -o "%EXE%" > "%LOG%" 2>&1
)
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
if not defined HOST (
  echo link failed. Pass a host driver as the second argument, e.g.
  echo    run.bat %SRC% hosts\driver.c
  echo or make sure "%SRC%" defines int main.
)
type "%LOG%"
del "%OBJ%" "%EXE%" "%LOG%" >nul 2>&1
exit /b 1

:usage
echo Usage: run.bat ^<file.strata^> [host.c]
echo.
echo Standalone:  run.bat hello.strata            ^(.strata defines int main()^)
echo With host:   run.bat engine_api.strata hosts\engine_api_host.c
echo               ^(host provides main + extern functions^)
exit /b 2
