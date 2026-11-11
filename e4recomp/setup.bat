@echo off
rem One-time build of E4 on Windows, from your own copy of the game:
rem   1. builds the (modified) ReXGlue SDK,
rem   2. recompiles the game's code from ..\game\default.xex,
rem   3. builds e4.exe and e4_launcher.exe.
rem Needs: Git, CMake 3.25+, Ninja, LLVM/clang 20+ and the Visual Studio 2022 Build Tools
rem (C++ workload, for the Windows SDK) - see README.md.
rem Usage: setup.bat [relwithdebinfo|release]
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=relwithdebinfo
if /i "%CONFIG%"=="relwithdebinfo" (set CMAKE_CONFIG=RelWithDebInfo) else if /i "%CONFIG%"=="release" (set CMAKE_CONFIG=Release) else (
  echo Unknown config "%CONFIG%" - use relwithdebinfo or release.
  exit /b 1
)
set ROOT=%~dp0
set SDK=%ROOT%..\tools\rexglue-sdk-src
set GAME=%ROOT%..\game
set SDK_INSTALL=%SDK%\out\install\win-amd64

for %%t in (git cmake ninja clang++) do (
  where %%t >nul 2>nul || (echo Missing %%t - see README.md. & exit /b 1)
)

rem --- Your game files -------------------------------------------------------
if not exist "%GAME%\default.xex" (
  echo Put the game files in E4\game first - see E4\game\README.md.
  exit /b 1
)
rem The code recompilation was made for this exact default.xex (E4, XBLA, title 58410872).
set EXPECTED=c4157358ace8664504edc95348e20b4588c294a8fe32821ef4b2f2a9461a3fd9
set HASH=
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%GAME%\default.xex" SHA256') do if not defined HASH set "HASH=%%h"
set "HASH=%HASH: =%"
if /i not "%HASH%"=="%EXPECTED%" (
  echo WARNING: your default.xex is a different version than this project supports
  echo   expected %EXPECTED%
  echo   found    %HASH%
  echo The build may fail or the game may crash. Press Ctrl+C to stop, or
  pause
)

rem --- Third-party sources (git submodules) -----------------------------------
if not exist "%SDK%\thirdparty\fmt\CMakeLists.txt" (
  echo === Fetching third-party sources ^(git submodules^) ===
  git -C "%ROOT%.." submodule update --init --recursive || exit /b 1
)

rem --- 1/3 SDK ----------------------------------------------------------------
echo === 1/3  ReXGlue SDK ===
cmake --preset win-amd64 -S "%SDK%" -DREXGLUE_BUILD_TESTS=OFF || exit /b 1
for %%c in (Release %CMAKE_CONFIG%) do (
  cmake --build "%SDK%\out\build\win-amd64" --config %%c || exit /b 1
  cmake --install "%SDK%\out\build\win-amd64" --config %%c || exit /b 1
)

rem --- 2/3 Recompile the game's code ------------------------------------------
if not exist "%ROOT%generated\default\e4_init.h" (
  echo === 2/3  Generating code from default.xex ^(takes a few minutes^) ===
  pushd "%ROOT%"
  "%SDK_INSTALL%\bin\rexglue.exe" codegen e4_manifest.toml || (popd & exit /b 1)
  popd
)

rem --- 3/3 Game + launcher ----------------------------------------------------
echo === 3/3  E4 ===
pushd "%ROOT%"
cmake --preset win-amd64-%CONFIG% -Drexglue_DIR="%SDK_INSTALL%\lib\cmake\rexglue" || (popd & exit /b 1)
cmake --build --preset win-amd64-%CONFIG% || (popd & exit /b 1)
popd
copy /y "%SDK_INSTALL%\bin\*.dll" "%ROOT%out\build\win-amd64-%CONFIG%\" >nul

echo.
echo Built: %ROOT%out\build\win-amd64-%CONFIG%\e4_launcher.exe
echo Start the launcher, enter a name and press PLAY.
