@echo off
rem build.cmd - builds ev-core.dll (x86): EVBots core, analogue of vu-core.dll (see README.md).
rem Same MSVC as evbots-dll\build.cmd. ASCII only: cmd.exe reads .cmd files in the OEM code page.
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
  echo [ERROR] vcvarsall.bat not found
  exit /b 1
)
call "%VCVARS%" x86 >nul
if errorlevel 1 (
  echo [ERROR] MSVC x86 environment failed
  exit /b 1
)

if not exist build mkdir build
if not exist obj mkdir obj
del /q obj\*.obj 2>nul

echo === ev-core.dll (x86) ===
cl /nologo /O2 /MT /W4 /WX /EHa /std:c++17 /utf-8 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /LD /Foobj\ /Febuild\ev-core.dll src\EvCore.cpp src\EvNet.cpp src\EvHook.cpp src\EvWin.cpp src\EvFreq.cpp src\X86Length.cpp ^
   /link /MACHINE:X86 /SAFESEH:NO user32.lib ws2_32.lib
if errorlevel 1 (
  echo [ERROR] ev-core build failed
  exit /b 1
)

echo === ev-core net test (x86) ===
if not exist build\test mkdir build\test
cl /nologo /O2 /MT /W4 /WX /EHsc /std:c++17 /utf-8 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /Foobj\ /Febuild\test\EvNetTest.exe test\EvNetTest.cpp ^
   /link /MACHINE:X86 ws2_32.lib
if errorlevel 1 (
  echo [ERROR] ev-core build failed
  exit /b 1
)

rem Build 237: x86 length decoder moved here from evbots-dll (hooks live in ev-core). Checker: re\verify_decoder.py
echo === X86Length.Dump (decoder check) ===
if not exist objdump mkdir objdump
cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /utf-8 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /Foobjdump\ /Febuild\test\X86Length.Dump.exe test\X86Length.Dump.cpp src\X86Length.cpp
if errorlevel 1 (
  echo [ERROR] decoder dump build failed
  exit /b 1
)
echo OK: build\ev-core.dll
exit /b 0
