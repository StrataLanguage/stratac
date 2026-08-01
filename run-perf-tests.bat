@echo off
REM ==========================================================================
REM  run-perf-tests.bat -- build and run the LLVM versus TinyCC benchmarks.
REM
REM  Usage:
REM    run-perf-tests.bat          full compile + hot-runtime matrix
REM    run-perf-tests.bat runtime  hot-runtime matrix only
REM    run-perf-tests.bat quick    one-sample smoke run
REM
REM  STRATA_PRESET and STRATA_BENCH_EXE may override the default preset and
REM  benchmark executable. CSV reports are written under build\default\perf.
REM ==========================================================================
setlocal

set "ROOT=%~dp0"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=full"
if not "%~2"=="" goto usage

if /I not "%MODE%"=="full" if /I not "%MODE%"=="runtime" if /I not "%MODE%"=="quick" goto usage

if "%STRATA_PRESET%"=="" set "STRATA_PRESET=default"
if "%STRATA_BENCH_EXE%"=="" set "STRATA_BENCH_EXE=%ROOT%build\%STRATA_PRESET%\bin\strata_jit_bench.exe"
if "%STRATA_PERF_DIR%"=="" set "STRATA_PERF_DIR=%ROOT%build\%STRATA_PRESET%\perf"

where cmake >nul 2>&1
if errorlevel 1 (
  echo error: cmake was not found on PATH
  exit /b 1
)

pushd "%ROOT%" >nul

echo [1/3] Configure preset %STRATA_PRESET%
cmake --preset "%STRATA_PRESET%"
if errorlevel 1 goto configure_failed

echo.
echo [2/3] Build stratac and strata_jit_bench
REM Building stratac also stages LLVM-C.dll beside the benchmark on Windows.
cmake --build --preset "%STRATA_PRESET%" --target stratac strata_jit_bench
if errorlevel 1 goto build_failed

if not exist "%STRATA_BENCH_EXE%" (
  echo error: benchmark executable not found: "%STRATA_BENCH_EXE%"
  popd
  exit /b 1
)
if not exist "%STRATA_PERF_DIR%" mkdir "%STRATA_PERF_DIR%"
if errorlevel 1 (
  echo error: could not create output directory: "%STRATA_PERF_DIR%"
  popd
  exit /b 1
)

echo.
echo [3/3] Run %MODE% performance matrix
if /I "%MODE%"=="runtime" goto run_runtime
if /I "%MODE%"=="quick" goto run_quick

"%STRATA_BENCH_EXE%" --sizes 1,5,20 --iterations 7 --csv "%STRATA_PERF_DIR%\jit-performance.csv"
if errorlevel 1 goto benchmark_failed
set "REPORT=%STRATA_PERF_DIR%\jit-performance.csv"
goto success

:run_runtime
"%STRATA_BENCH_EXE%" --runtime-only --sizes 1,5,20 --iterations 11 --csv "%STRATA_PERF_DIR%\jit-runtime.csv"
if errorlevel 1 goto benchmark_failed
set "REPORT=%STRATA_PERF_DIR%\jit-runtime.csv"
goto success

:run_quick
"%STRATA_BENCH_EXE%" --quick --csv "%STRATA_PERF_DIR%\jit-performance-quick.csv"
if errorlevel 1 goto benchmark_failed
set "REPORT=%STRATA_PERF_DIR%\jit-performance-quick.csv"
goto success

:success
echo.
echo Performance tests completed successfully.
echo CSV report: "%REPORT%"
popd
exit /b 0

:configure_failed
echo error: CMake configure failed
popd
exit /b 1

:build_failed
echo error: benchmark build failed
popd
exit /b 1

:benchmark_failed
echo error: performance benchmark failed
popd
exit /b 1

:usage
echo Usage: run-perf-tests.bat [full^|runtime^|quick]
exit /b 2
