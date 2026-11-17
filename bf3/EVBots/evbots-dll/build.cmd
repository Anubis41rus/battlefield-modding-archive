@echo off
chcp 65001 >nul
rem build.cmd — сборка EVBots.dll и тест-хоста (x86).
rem
rem Без CMake: собираем cl.exe напрямую. Зависимости — MSVC с C++ и
rem исходники Lua 5.4.2 в thirdparty.
rem
rem Требуется Windows SDK (winget install Microsoft.WindowsSDK.10.0.18362).

setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
  echo [ОШИБКА] не найден vcvarsall.bat
  exit /b 1
)

call "%VCVARS%" x86 >nul
if errorlevel 1 (
  echo [ОШИБКА] не удалось настроить окружение MSVC x86
  exit /b 1
)

if not exist build mkdir build
if not exist obj mkdir obj
if not exist objdump mkdir objdump

rem Сборка всегда полная, поэтому чистим объекты: иначе на компоновку
rem попадают файлы от прошлых попыток, и линкер падает на дубликатах.
del /q obj\*.obj 2>nul
del /q objdump\*.obj 2>nul

rem ── Lua 5.4.2: все .c кроме инструментов командной строки ────────────────
set "LUA_SRC="
for %%f in (thirdparty\lua-5.4.2\src\*.c) do (
  set "NAME=%%~nxf"
  if not "!NAME!"=="lua.c" (
    if not "!NAME!"=="luac.c" (
      if not "!NAME!"=="onelua.c" set "LUA_SRC=!LUA_SRC! %%f"
    )
  )
)

echo === сборка EVBots.dll (x86) ===

rem ★ Сборка 220: Lua 5.4.2 — отдельная build\lua.dll (как vubin\lua.dll у VU), EVBots.dll грузит её отложенно
rem (/DELAYLOAD:lua.dll, крючок в src\LuaDll.cpp — из папки EVBots.dll). Объекты Lua — в objlua\, не в obj\.
if not exist objlua mkdir objlua
del /q objlua\*.obj 2>nul
echo === lua.dll (Lua 5.4.2, x86) ===
cl /nologo /c /O2 /MT /W3 /wd4701 /wd4702 /wd4005 /DLUA_BUILD_AS_DLL ^
   /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /Ithirdparty\lua-5.4.2\src /Foobjlua\ ^
   !LUA_SRC!
if errorlevel 1 (
  echo [ОШИБКА] компиляция Lua провалилась
  exit /b 1
)
link /nologo /DLL /OUT:build\lua.dll /IMPLIB:build\lua.lib /MACHINE:X86 /OPT:REF /OPT:ICF /DEBUG objlua\*.obj
if errorlevel 1 (
  echo [ОШИБКА] компоновка lua.dll провалилась
  exit /b 1
)

rem наш код - с максимальными предупреждениями; LUA_BUILD_AS_DLL — API Lua импортируется из lua.dll
cl /nologo /c /O2 /MT /W4 /WX /std:c++17 /DUNICODE /D_UNICODE /DLUA_BUILD_AS_DLL ^
   /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /DXXH_NAMESPACE=ZSTD_ /Ithirdparty\zstd /Ithirdparty\webview2\include ^
   /Isrc /Ithirdparty\lua-5.4.2\src /Ithirdparty\sqlite\sqlite-amalgamation-3540000 /Foobj\ ^
   src\VextWebUi.cpp src\WebUiPresent.cpp src\Vuic.cpp src\VextClientInput.cpp src\VextSettings.cpp src\VextSpectator.cpp ^
   src\NativeBlueprint.cpp src\VextClientEntities.cpp src\NativeCollision.cpp src\NativeSpatial.cpp ^
   src\NativeEntityEvents.cpp src\VextEntityEvents.cpp ^
   src\NativeSoldierCustomization.cpp src\NativePlayerEvents.cpp ^
   src\TeamCapacity.cpp src\RoundPlayerCounts.cpp src\NativeRconGameplay.cpp src\NativeVisualSettings.cpp src\NativeVisualSettingsBridge.cpp src\NativeVisualEnvironment.cpp src\VextVisualEnvironment.cpp ^
   src\VextClientSoldier.cpp ^
   src\ModdingInterface.cpp src\EngineHooks.cpp src\InlineHook.cpp ^
   src\SpmProbe.cpp src\EngineResources.cpp ^
   src\EVBotsLog.cpp src\EVBots.TestApi.cpp ^
   src\LuaRuntime.cpp src\Json.cpp src\MiddleClass.cpp ^
   src\LuaEvents.cpp src\LuaDebugConsole.cpp src\VextEnums.cpp src\VextUtils.cpp src\VextGeometry.cpp src\VextHooks.cpp src\VextNet.cpp src\VextNetEvents.cpp src\VextPlayerManager.cpp src\VextResourceManager.cpp src\VextTicketManager.cpp src\VextSql.cpp src\VextRcon.cpp src\VextEntityManager.cpp src\VextGlobals.cpp src\VextSpatial.cpp src\VextConfig.cpp src\VextTeamSquadManager.cpp src\VextChatManager.cpp src\VextCustomization.cpp src\VextClient.cpp src\VextFbTypes.cpp src\VextNetCodec.cpp src\EvNetBridge.cpp src\LuaDll.cpp src\VextClientPlayers.cpp src\VextClientRaycast.cpp
if errorlevel 1 (
  echo [ОШИБКА] компиляция нашего кода провалилась
  exit /b 1
)

rem SQLite amalgamation — отдельным объектником, без /WX (свои предупреждения)
cl /nologo /c /O2 /MT /W3 ^
   /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /Ithirdparty\sqlite\sqlite-amalgamation-3540000 ^
   /Foobj\sqlite3.obj ^
   thirdparty\sqlite\sqlite-amalgamation-3540000\sqlite3.c
if errorlevel 1 (
  echo [ОШИБКА] компиляция sqlite3.c провалилась
  exit /b 1
)

rem ★ Сборка 228: декодер zstd 1.5.7 (только распаковка) для ui.vuic — отдельные объектники в objzstd\, без /WX
if not exist objzstd mkdir objzstd
del /q objzstd\*.obj 2>nul
cl /nologo /c /O2 /MT /W3 /DZSTD_DISABLE_ASM /DXXH_NAMESPACE=ZSTD_ /Ithirdparty\zstd /Foobjzstd\ ^
   thirdparty\zstd\common\entropy_common.c thirdparty\zstd\common\error_private.c ^
   thirdparty\zstd\common\fse_decompress.c thirdparty\zstd\common\xxhash.c thirdparty\zstd\common\zstd_common.c ^
   thirdparty\zstd\common\debug.c thirdparty\zstd\decompress\huf_decompress.c thirdparty\zstd\decompress\zstd_ddict.c ^
   thirdparty\zstd\decompress\zstd_decompress.c thirdparty\zstd\decompress\zstd_decompress_block.c
if errorlevel 1 (
  echo [ОШИБКА] компиляция zstd провалилась
  exit /b 1
)

rem WebView2LoaderStatic.lib (SDK 1.0.4258.31) — загрузчик WebView2 внутри EVBots.dll, отдельной DLL в комплекте нет
link /nologo /DLL /OUT:build\EVBots.dll ^
     /MACHINE:X86 /OPT:REF /OPT:ICF /DEBUG ^
     obj\*.obj objzstd\*.obj build\lua.lib delayimp.lib /DELAYLOAD:lua.dll ^
     thirdparty\webview2\x86\WebView2LoaderStatic.lib ole32.lib oleaut32.lib shlwapi.lib advapi32.lib user32.lib windowscodecs.lib
if errorlevel 1 (
  echo [ОШИБКА] компоновка DLL провалилась
  exit /b 1
)

echo === сборка тест-хоста ===

cl /nologo /c /O2 /MT /W4 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /Foobj\ test\EVBots.Test.cpp
if errorlevel 1 (
  echo [ОШИБКА] компиляция теста провалилась
  exit /b 1
)

link /nologo /OUT:build\EVBots.Test.exe ^
     /MACHINE:X86 /OPT:REF /OPT:ICF /DEBUG ^
     obj\EVBots.Test.obj
if errorlevel 1 (
  echo [ОШИБКА] компоновка теста провалилась
  exit /b 1
)

rem Сборка 237: перехваты и правка памяти движка — в ядре ev-core.dll (как vu-core у VU), EVBots.dll ставит их через
rem него. Тест-хост и client-harness грузят EVBots.dll из build\ — ядро кладём рядом. Декодер X86Length и его сверка
rem (X86Length.Dump) — в ..\ev-core (build\test\X86Length.Dump.exe).
if not exist ..\ev-core\build\ev-core.dll (
  echo [ОШИБКА] нет ..\ev-core\build\ev-core.dll — сначала ..\ev-core\build.cmd
  exit /b 1
)
copy /y ..\ev-core\build\ev-core.dll build\ev-core.dll >nul
if exist build\X86Length.Dump.exe del /q build\X86Length.Dump.exe

echo.
echo Готово:
dir /b build
echo.
rem 238-3: строки вывода — ASCII (кириллица в echo ломалась в cmd: «'…ка' is not recognized»)
echo Hook check   : build\EVBots.Test.exe [needs build\ev-core.dll]
echo Decoder check: python C:\Games\re\verify_decoder.py
exit /b 0
