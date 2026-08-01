@echo off
REM Build and run the box demo (AOT path: stratac -> clang link)
REM   requires CLANG env var or a working clang on PATH
if "%CLANG%"=="" set "CLANG=clang"

stratac box_demo.strata -o box_demo.o || exit /b 1
%CLANG% hosts/box_demo_host.c box_demo.o -o box_demo.exe || exit /b 1
box_demo.exe
echo exit code: %ERRORLEVEL%
