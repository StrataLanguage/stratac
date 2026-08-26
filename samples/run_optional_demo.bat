@echo off
REM Build and run the optionals demo (JIT path via the optional_demo target,
REM or AOT: stratac -> clang link).
REM   requires CLANG env var or a working clang on PATH
if "%CLANG%"=="" set "CLANG=clang"

stratac optional_demo.strata -o optional_demo.o || exit /b 1
%CLANG% hosts/optional_demo_host.c optional_demo.o -o optional_demo.exe || exit /b 1
optional_demo.exe
echo exit code: %ERRORLEVEL%
