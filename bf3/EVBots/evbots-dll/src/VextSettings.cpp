// Настройки клиентских модов: VU-Docs/ref/client/library/settingsmanager.md,
// type/modsetting.md и type/settingoptions.md. Данные хранятся рядом с нашей DLL,
// отдельно от исходников модов. Интерфейс настроек VU этим файлом не реализуется.
#include "VextSettings.h"

#include <Windows.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include "EVBotsLog.h"
#include "LuaRuntime.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace evbots {
namespace {

constexpr DWORD kMaxSettingsBytes = 1024 * 1024;

std::wstring SettingsPath(const char* owner, size_t len) {
    wchar_t root[MAX_PATH]{};
    DWORD size = GetEnvironmentVariableW(L"EVBOTS_SETTINGS_DIR", root, MAX_PATH);
    if (size >= MAX_PATH) return {};
    std::wstring directory;
    if (size != 0) {
        directory.assign(root, size);
    } else {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&SettingsPath), &self)) return {};
        size = GetModuleFileNameW(self, root, MAX_PATH);
        if (size == 0 || size >= MAX_PATH) return {};
        directory.assign(root, size);
        const size_t slash = directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        directory.resize(slash);
        directory += L"\\mod-settings";
    }
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    // Стабильный ключ FNV-1a от идентификатора мода: он не становится частью пути.
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<unsigned char>(owner[i]);
        hash *= UINT64_C(1099511628211);
    }
    wchar_t filename[32]{};
    swprintf_s(filename, L"\\%016llx.json", static_cast<unsigned long long>(hash));
    return directory + filename;
}

int Scope(lua_State* L) {
    auto& runtime = LuaRuntime::Instance();
    for (const auto& mod : runtime.mods()) {
        if (mod.folder == runtime.currentModFolder()) {
            // VU: имя папки уникально; Name в mod.json — только отображаемый текст.
            const size_t slash = mod.folder.find_last_of(L"\\/");
            const std::wstring folder = mod.folder.substr(slash == std::wstring::npos ? 0 : slash + 1);
            const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                folder.data(), static_cast<int>(folder.size()), nullptr, 0, nullptr, nullptr);
            if (length <= 0) return luaL_error(L, "SettingsManager: invalid mod folder");
            std::string owner = "mod:";
            owner.resize(4 + length);
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                folder.data(), static_cast<int>(folder.size()), owner.data() + 4, length,
                nullptr, nullptr) != length) return luaL_error(L, "SettingsManager: invalid mod folder");
            lua_pushlstring(L, owner.data(), owner.size());
            return 1;
        }
    }
    lua_pushliteral(L, "EVBots:builtin");
    return 1;
}

int Read(lua_State* L) {
    size_t ownerLen = 0;
    const char* owner = luaL_checklstring(L, 1, &ownerLen);
    const std::wstring path = SettingsPath(owner, ownerLen);
    if (path.empty()) { lua_pushnil(L); lua_pushliteral(L, "settings directory unavailable"); return 2; }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) { lua_pushliteral(L, "{}"); return 1; }
        lua_pushnil(L); lua_pushliteral(L, "settings file unreadable"); return 2;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > kMaxSettingsBytes) {
        CloseHandle(file); lua_pushnil(L); lua_pushliteral(L, "invalid settings file size"); return 2;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                    read == bytes.size();
    CloseHandle(file);
    if (!ok) { lua_pushnil(L); lua_pushliteral(L, "settings read failed"); return 2; }
    lua_pushlstring(L, bytes.data(), bytes.size());
    return 1;
}

int Write(lua_State* L) {
    size_t ownerLen = 0, dataLen = 0;
    const char* owner = luaL_checklstring(L, 1, &ownerLen);
    const char* data = luaL_checklstring(L, 2, &dataLen);
    if (dataLen > kMaxSettingsBytes) { lua_pushboolean(L, 0); return 1; }
    const std::wstring path = SettingsPath(owner, ownerLen);
    if (path.empty()) { lua_pushboolean(L, 0); return 1; }
    const std::wstring temporary = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { lua_pushboolean(L, 0); return 1; }
    DWORD written = 0;
    bool ok = WriteFile(file, data, static_cast<DWORD>(dataLen), &written, nullptr) && written == dataLen;
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) LOG_WARN("SettingsManager: настройка не сохранена, Win32=%lu", GetLastError());
    lua_pushboolean(L, ok);
    return 1;
}

constexpr char kSettingsSource[] = R"LUA(
local scope, read, write = ...
local stores, objects = {}, {}
local function copy(v)
  if type(v) ~= 'table' then return v end
  local r = {}; for k, x in pairs(v) do r[k] = x end; return r
end
local function nameRequired(name)
  assert(type(name) == 'string' and #name > 0, 'SettingsManager: setting name required')
end
local function store(owner)
  if not stores[owner] then
    local text, err = read(owner)
    assert(text, 'SettingsManager: '..tostring(err))
    local ok, values = pcall(json.decode, text)
    assert(ok and type(values) == 'table', 'SettingsManager: invalid saved JSON')
    for name, record in pairs(values) do
      nameRequired(name)
      assert(type(record) == 'table' and record.set == true, 'SettingsManager: invalid saved setting')
    end
    stores[owner], objects[owner] = values, {}
  end
  return stores[owner], objects[owner]
end
local function save(owner, name, record)
  local values = store(owner)
  local nextValues = copy(values)
  nextValues[name] = record
  assert(write(owner, json.encode(nextValues)), 'SettingsManager: save failed')
  stores[owner] = nextValues
end
local function finite(v)
  return type(v) == 'number' and v == v and v > -math.huge and v < math.huge
end
local function key(v)
  if not finite(v) or v % 1 ~= 0 then return false end
  for _, n in pairs(InputDeviceKeys) do if n == v then return true end end
  return false
end
local function sequence(v, valid)
  if type(v) ~= 'table' then return false end
  local n = #v
  for k, x in pairs(v) do
    if type(k) ~= 'number' or k % 1 ~= 0 or k < 1 or k > n or not valid(x) then return false end
  end
  for i = 1, n do if not valid(v[i]) then return false end end
  return true
end
local function validator(kind, definition)
  assert(type(definition) == 'table', 'SettingsManager: invalid definition')
  if kind == 'bool' then return function(v) return type(v) == 'boolean' end end
  if kind == 'key' then return key end
  if kind == 'keys' then return function(v) return sequence(v, key) end end
  if kind == 'number' or kind == 'string' then
    local minimum, maximum = definition.minimum, definition.maximum
    assert(finite(minimum) and finite(maximum) and minimum <= maximum, 'SettingsManager: invalid range')
    if kind == 'number' then
      return function(v) return finite(v) and v >= minimum and v <= maximum end
    end
    assert(minimum >= 0 and minimum % 1 == 0 and maximum % 1 == 0, 'SettingsManager: invalid string length range')
    return function(v)
      if type(v) ~= 'string' then return false end
      local n = utf8.len(v); return n ~= nil and n >= minimum and n <= maximum
    end
  end
  if kind == 'option' then
    assert(type(definition.allowEmpty) == 'boolean' and sequence(definition.names, function(v)
      return type(v) == 'string'
    end), 'SettingsManager: invalid options list')
    local allowed = {}
    for _, value in ipairs(definition.names) do allowed[value] = true end
    return function(v)
      return (definition.allowEmpty and v == nil) or (type(v) == 'string' and allowed[v] == true)
    end
  end
  error('SettingsManager: unknown setting type '..tostring(kind))
end
local function settingOptions(options)
  options = options or SettingOptions()
  assert(type(options) == 'table', 'SettingsManager: invalid options')
  assert(options.displayName == nil or type(options.displayName) == 'string', 'SettingsManager: invalid display name')
  assert(options.showInUi == nil or type(options.showInUi) == 'boolean', 'SettingsManager: invalid UI option')
  return {displayName=options.displayName or '', showInUi=options.showInUi == true}
end
local function record(kind, definition, options, value)
  return {set=true, kind=kind, definition=definition, options=options, value=copy(value)}
end
local function makeSetting(owner, name, value, kind, definition, options, valid)
  local _, registry = store(owner)
  local state = {kind=kind, value=copy(value), alive=true}
  local object = setmetatable({}, {
    __index=function(_, field)
      if field == 'name' then return name end
      if field == 'value' then return copy(state.value) end
    end,
    __newindex=function(_, field, newValue)
      assert(field == 'value', 'ModSetting: '..tostring(field)..' is read-only or unknown')
      assert(state.alive, 'ModSetting: setting was deleted')
      assert(valid(newValue), 'ModSetting: invalid value for '..name)
      if json.encode(state.value) == json.encode(newValue) then return end
      save(owner, name, record(kind, definition, options, newValue))
      state.value = copy(newValue)
      Events:Dispatch('SettingsManager:SettingsChanged')
    end
  })
  state.object = object
  registry[name] = state
  return object
end
local function declare(name, default, kind, definition, options)
  nameRequired(name)
  local valid = validator(kind, definition)
  assert(valid(default), 'SettingsManager: invalid default for '..name)
  options = settingOptions(options)
  local owner = scope()
  local values, registry = store(owner)
  if registry[name] then
    assert(registry[name].kind == kind, 'SettingsManager: incompatible redeclaration of '..name)
    return registry[name].object
  end
  local value = copy(default)
  local saved = values[name]
  if type(saved) == 'table' and saved.set == true and valid(saved.value) then value = copy(saved.value) end
  -- Сохраняем и декларацию без изменений: GetSetting должен работать до Declare
  -- в следующем Lua-сеансе (Royale иначе повторно присваивает default).
  save(owner, name, record(kind, definition, options, value))
  return makeSetting(owner, name, value, kind, definition, options, valid)
end
function SettingOptions() return {displayName='', showInUi=false} end
SettingsManager = {}
function SettingsManager:GetSetting(name)
  nameRequired(name)
  local owner = scope()
  local values, registry = store(owner)
  if registry[name] then return registry[name].object end
  local saved = values[name]
  if saved == nil then return nil end
  -- Старый экспериментальный формат имел только set/value. Его значение ещё
  -- читается в Declare, где доступны тип и ограничения для обновления формата.
  if saved.kind == nil then return nil end
  local valid = validator(saved.kind, saved.definition)
  local options = settingOptions(saved.options)
  assert(valid(saved.value), 'SettingsManager: invalid saved value for '..name)
  return makeSetting(owner, name, saved.value, saved.kind, saved.definition, options, valid)
end
function SettingsManager:DeleteSetting(name)
  nameRequired(name)
  local owner = scope()
  local values, registry = store(owner)
  if values[name] ~= nil then save(owner, name, nil) end
  if registry[name] then registry[name].alive = false; registry[name] = nil end
end
function SettingsManager:DeclareBool(name, default, options)
  return declare(name, default, 'bool', {}, options)
end
function SettingsManager:DeclareNumber(name, default, minimum, maximum, options)
  return declare(name, default, 'number', {minimum=minimum, maximum=maximum}, options)
end
function SettingsManager:DeclareString(name, default, minimum, maximum, options)
  return declare(name, default, 'string', {minimum=minimum, maximum=maximum}, options)
end
function SettingsManager:DeclareKeybind(name, default, options)
  return declare(name, default, 'key', {}, options)
end
function SettingsManager:DeclareMultiKeybind(name, default, options)
  return declare(name, default, 'keys', {}, options)
end
function SettingsManager:DeclareOption(name, default, names, allowEmpty, options)
  return declare(name, default, 'option', {names=copy(names), allowEmpty=allowEmpty}, options)
end
)LUA";

} // namespace

bool RegisterVextSettings(lua_State* L) {
    if (luaL_loadbufferx(L, kSettingsSource, sizeof(kSettingsSource) - 1,
                         "=[vext-settings]", "t") == LUA_OK) {
        lua_pushcfunction(L, Scope);
        lua_pushcfunction(L, Read);
        lua_pushcfunction(L, Write);
        if (lua_pcall(L, 3, 0, 0) == LUA_OK) return true;
    }
    LOG_ERR("SettingsManager: %s", lua_tostring(L, -1));
    lua_pop(L, 1);
    return false;
}

} // namespace evbots
