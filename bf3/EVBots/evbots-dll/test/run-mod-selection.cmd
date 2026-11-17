@echo off
setlocal
if "%~1"=="" (
  echo Usage: run-mod-selection.cmd absolute-test-output-directory
  exit /b 2
)
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" x86 >nul
if errorlevel 1 exit /b 2
if not exist "%~1" mkdir "%~1"
pushd "%~1"
cl /nologo /EHsc /MT /W4 /WX /std:c++17 /utf-8 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
  /I"%~dp0..\src" "%~dp0ModSelection.Test.cpp" "%~dp0..\src\Json.cpp" /FeModSelection.Test.exe
if errorlevel 1 (
  popd
  exit /b 1
)
ModSelection.Test.exe
set "TEST_RESULT=%ERRORLEVEL%"
popd
exit /b %TEST_RESULT%
