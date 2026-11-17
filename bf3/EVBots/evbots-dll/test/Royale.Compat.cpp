// Изолированные проверки SettingsManager и отказа при ошибке init, без BF3.
#include <Windows.h>
#include <cstdio>
#include <string>

using Start = int(__cdecl*)(const wchar_t*);
using Stop = void(__cdecl*)();
using Eval = int(__cdecl*)(const char*);

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { puts("Usage: Royale.Compat.exe settings-dir fixtures-dir [royale-mods-dir]"); return 2; }
    SetEnvironmentVariableW(L"EVBOTS_SETTINGS_DIR", argv[1]);
    SetEnvironmentVariableW(L"EVBOTS_WEBUI", L"0");
    SetEnvironmentVariableW(L"EVBOTS_TESTS", L"0");
    SetEnvironmentVariableW(L"EVBOTS_LOG_VERBOSE", L"1");
    HMODULE dll = LoadLibraryW(L"EVBots.dll");
    if (!dll) { printf("DLL load failed: %lu\n", GetLastError()); return 2; }
    auto start = reinterpret_cast<Start>(GetProcAddress(dll, "EVBotsTestClientMode"));
    auto stop = reinterpret_cast<Stop>(GetProcAddress(dll, "EVBotsTestStopClientMode"));
    auto eval = reinterpret_cast<Eval>(GetProcAddress(dll, "EVBotsTestLuaTrue"));
    if (!start || !stop || !eval) { puts("Test API missing"); return 2; }
    int failed = 0, passed = 0;
    auto check = [&](bool ok, const char* label) {
        printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        if (ok) ++passed; else ++failed;
    };
    check(start(nullptr) == 1, "client startup without mods");
    check(eval(R"LUA((function()
      class('RoyaleGlobalClass')
      function RoyaleGlobalClass:__init(value) self.value = value end
      local Child = class('RoyaleGlobalChild', RoyaleGlobalClass)
      local instance = Child(42)
      return middleclass == class and RoyaleGlobalChild == Child and instance.value == 42
        and instance:isInstanceOf(RoyaleGlobalClass)
    end)())LUA") == 1, "VU class(name) publishes global and aliases middleclass correctly");
    check(eval("ShaderParameterType.ShaderParameterType_TextureSlice == 7 and UIHudIcon.UIHudIcon_Revive == 119 and InputDeviceMouseButtons.IDB_Button_7 == 7") == 1,
          "documented enum values");
    check(eval("WeaponSwitchingEnum.wsSlot9 == 9 and WeaponSwitchingEnum.wsMeleeWeapon == 7 "
               "and WeaponState.Reload == 11 and WeaponState.ReloadDelay == 10 "
               "and SpatialQueryFlags.AllGrids == 15 and VoipEmitterType.Local == 0 "
               "and UIAction.SpawnCustomization == -216857053 and UIAction.UnSpawnCustomization == 787240248") == 1,
          "Royale resource callbacks and HUD use documented enum values");
    check(eval(R"LUA((function()
      SettingsManager:DeleteSetting('TestFlag')
      SettingsManager:DeleteSetting('TestOption')
      SettingsManager:DeleteSetting('TestNumber')
      SettingsManager:DeleteSetting('TestKeys')
      SettingsManager:DeleteSetting('TestKey')
      SettingsManager:DeleteSetting('TestString')
      SettingsManager:DeleteSetting('TestDefaultOnly')
      SettingsManager:DeleteSetting('TestNul')
      SettingsManager:DeleteSetting('TestNul\0Name')
      TestFlag = SettingsManager:DeclareBool('TestFlag', false, SettingOptions())
      TestEvents = 0
      Events:Subscribe('SettingsManager:SettingsChanged', function() TestEvents = TestEvents + 1 end)
      TestFlag.value = true; TestFlag.value = true
      return TestFlag.name == 'TestFlag' and TestFlag.value == true and TestEvents == 1
        and SettingsManager:GetSetting('TestFlag') == TestFlag
        and not pcall(function() TestFlag.name = 'Changed' end)
        and not pcall(function() TestFlag.value = 1 end)
    end)())LUA") == 1, "boolean identity, event and validation");
    check(eval(R"LUA((function()
      local s = SettingsManager:DeclareNumber('TestNumber', 2, 0, 10, {})
      s.value = 3
      return s.value == 3 and not pcall(function() s.value = 11 end)
        and not pcall(function() s.value = 0/0 end)
    end)())LUA") == 1, "number bounds and NaN rejection");
    check(eval(R"LUA((function()
      local s = SettingsManager:DeclareMultiKeybind('TestKeys', {InputDeviceKeys.IDK_Q}, {})
      local keys = s.value; keys[1] = InputDeviceKeys.IDK_E
      if s.value[1] ~= InputDeviceKeys.IDK_Q then return false end
      s.value = {InputDeviceKeys.IDK_E, InputDeviceKeys.IDK_LeftCtrl}
      local k = SettingsManager:DeclareKeybind('TestKey', InputDeviceKeys.IDK_Q, {})
      k.value = InputDeviceKeys.IDK_E
      return not pcall(function() s.value = {123456} end)
        and k.value == InputDeviceKeys.IDK_E
    end)())LUA") == 1, "keybind validation and array copy");
    check(eval(R"LUA((function()
      local s = SettingsManager:DeclareString('TestString', 'ok', 1, 4, {})
      s.value = 'test'
      return not pcall(function() s.value = 'longer' end)
    end)())LUA") == 1, "string bounds");
    check(eval(R"LUA((function()
      local s = SettingsManager:DeclareOption('TestOption', 'first', {'first', 'second'}, true, {})
      s.value = nil
      return s.value == nil and not pcall(function() s.value = 'third' end)
    end)())LUA") == 1, "nullable option");
    check(eval(R"LUA((function()
      SettingsManager:DeclareBool('TestDefaultOnly', false, {})
      local prefix = SettingsManager:DeclareString('TestNul', 'prefix', 0, 20, {})
      local s = SettingsManager:DeclareString('TestNul\0Name', '', 0, 20, {})
      local value = utf8.char(0x0416, 0x1F600)..'\0end'
      s.value = value
      return s.value == value and prefix.value == 'prefix' and s.name == 'TestNul\0Name'
    end)())LUA") == 1, "Unicode and embedded NUL in setting name and value");
    stop();
    check(start(nullptr) == 1, "client restart");
    check(eval(R"LUA((function()
      -- Оригинальный шаблон Royale: GetSetting, затем Declare + default только
      -- при отсутствии сохранённой настройки. Get должен работать до Declare.
      local b = SettingsManager:GetSetting('TestFlag')
      if b == nil then
        b = SettingsManager:DeclareBool('TestFlag', false, {})
        b.value = false
      end
      local o = SettingsManager:GetSetting('TestOption')
      if o == nil then
        o = SettingsManager:DeclareOption('TestOption', 'first', {'first','second'}, true, {})
        o.value = 'first'
      end
      local k = SettingsManager:GetSetting('TestKey')
      if k == nil then
        k = SettingsManager:DeclareKeybind('TestKey', InputDeviceKeys.IDK_Q, {})
        k.value = InputDeviceKeys.IDK_Q
      end
      return b.value == true and o.value == nil and k.value == InputDeviceKeys.IDK_E
        and SettingsManager:DeclareBool('TestFlag', false, {}) == b
    end)())LUA") == 1, "Royale GetSetting-before-Declare preserves values after restart");
    check(eval(R"LUA((function()
      local n = SettingsManager:GetSetting('TestNumber')
      local o = SettingsManager:GetSetting('TestOption')
      local keys = SettingsManager:GetSetting('TestKeys')
      local k = SettingsManager:GetSetting('TestKey')
      local s = SettingsManager:GetSetting('TestString')
      return n ~= nil and keys ~= nil and s ~= nil and n.value == 3 and s.value == 'test'
        and keys.value[1] == InputDeviceKeys.IDK_E and #keys.value == 2
        and not pcall(function() n.value = 11 end)
        and not pcall(function() o.value = 'third' end)
        and not pcall(function() keys.value = {123456} end)
        and not pcall(function() k.value = 123456 end)
        and not pcall(function() s.value = 'longer' end)
    end)())LUA") == 1, "GetSetting restores type and validation metadata");
    check(eval(R"LUA((function()
      local untouched = SettingsManager:GetSetting('TestDefaultOnly')
      local prefix = SettingsManager:GetSetting('TestNul')
      local s = SettingsManager:GetSetting('TestNul\0Name')
      return untouched ~= nil and untouched.value == false
        and prefix ~= nil and prefix.value == 'prefix' and s ~= nil
        and s.name == 'TestNul\0Name' and s.value == utf8.char(0x0416, 0x1F600)..'\0end'
    end)())LUA") == 1, "unchanged default, Unicode and NUL survive restart");
    check(eval(R"LUA((function()
      local b = SettingsManager:GetSetting('TestFlag')
      SettingsManager:DeleteSetting('TestFlag')
      SettingsManager:DeleteSetting('TestOption')
      return SettingsManager:GetSetting('TestFlag') == nil
        and not pcall(function() b.value = false end)
    end)())LUA") == 1, "delete invalidates old handle");
    stop();
    check(start(nullptr) == 1, "restart after delete");
    check(eval("SettingsManager:DeclareBool('TestFlag', false, {}).value == false and SettingsManager:DeclareOption('TestOption', 'first', {'first'}, true, {}).value == 'first'") == 1,
          "delete persists");
    stop();
    const std::wstring valid = std::wstring(argv[2]) + L"\\valid";
    check(start(valid.c_str()) == 1, "two fixture mods start");
    check(eval("SettingsFixtureA == true and SettingsFixtureB == false") == 1,
          "same setting and display Name remain isolated by mod folder");
    check(eval(R"LUA((function()
      for _, call in ipairs({function() Hooks._Fire('SettingsScopeProbe') end,
          function() Hooks._FireCall('SettingsScopeProbe', function() end) end,
          function() Hooks._FireEx('SettingsScopeProbe') end}) do
        HookFixtureA, HookFixtureB, HookRequireA, HookRequireB = nil, nil, nil, nil
        call()
        if HookFixtureA ~= true or HookFixtureB ~= false or HookRequireA ~= 'A' or HookRequireB ~= 'B' then return false end
      end
      Hooks._Fire('SettingsScopeFailure')
      return SettingsManager:GetSetting('SameName') == nil
    end)())LUA") == 1, "all hook dispatchers keep mod scope and restore it after failure");
    stop();
    const std::wstring invalid = std::wstring(argv[2]) + L"\\invalid";
    check(start(invalid.c_str()) == 0, "bad shared init fails startup");
    check(eval("true") == -2, "failed mod leaves no partial Lua runtime");
    stop();
    for (const wchar_t* fixture : {L"invalid-event-init", L"invalid-engine-init", L"invalid-extension-loaded"}) {
        const std::wstring path = std::wstring(argv[2]) + L"\\" + fixture;
        check(start(path.c_str()) == 0, "callback failure rejects startup");
        check(eval("true") == -2, "callback failure closes Lua runtime");
        stop();
        check(start(nullptr) == 1, "clean startup succeeds after callback failure");
        stop();
    }
    if (argc > 3) {
        check(start(argv[3]) == 1, "original Royale client init");
        check(eval(R"LUA((function()
          local probe = {m_Presets = {a = {logic = {visibility = 0.25}}, b = {logic = {visibility = 0.75}}}}
          local calls = {}
          function probe:Lerp(id, value, seconds) calls[id] = {value, seconds} end
          VEManagerClient.Crossfade(probe, 'a', 'b', 2)
          return calls.a[1] == 0.75 and calls.b[1] == 0.25 and calls.a[2] == 2 and calls.b[2] == 2
            and not pcall(function() VEManagerClient.Crossfade(probe, 'a', 'missing', 2) end)
        end)())LUA") == 1, "VEManager crossfade uses existing visibilities and validates both presets");
        check(eval("Voip:IsAvailable() == false and Voip:GetChannel('absent') == nil and #Voip:GetChannels() == 0") == 1,
              "local runtime reports unavailable voice transport");
        check(eval(R"LUA((function()
          local before = Events:_FailureCount()
          Events:Dispatch('Extension:Unloading')
          return Events:_FailureCount() == before and SpectatorManager:GetSpectating() == false
        end)())LUA") == 1, "original Royale unloading callbacks complete without errors");
        stop();
    }
    printf("RESULT pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
