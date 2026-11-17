@echo off
rem build.cmd - builds the EVBots loader (build\evbots-loader.exe, x86) and the test marker DLLs for test.cmd.
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
if not exist build\test mkdir build\test
if not exist obj mkdir obj
del /q obj\*.obj 2>nul

echo === evbots-loader.exe (x86) ===
cl /nologo /O2 /MT /W4 /WX /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /Foobj\ /Febuild\evbots-loader.exe src\EVBotsLoader.cpp ^
   /link /MACHINE:X86 /SUBSYSTEM:CONSOLE advapi32.lib
if errorlevel 1 (
  echo [ERROR] loader build failed
  exit /b 1
)

echo === test marker DLLs ===
cl /nologo /O2 /MT /W4 /WX /LD /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /Foobj\ /Febuild\test\marker1.dll test\marker.cpp /link /MACHINE:X86
if errorlevel 1 (
  echo [ERROR] marker build failed
  exit /b 1
)
copy /y build\test\marker1.dll build\test\marker2.dll >nul

echo === done: build\evbots-loader.exe ===
exit /b 0
