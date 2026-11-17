// VextCustomization.cpp — fb-типы кастомизации солдата VenusEXT (VU).
//
// Native constructors use the verified ResourceManager allocator in game.
// The established zero-argument fun-bots path remains managed by owner folder;
// offline test hosts retain the same managed arrays and defaults.
// Все три типа — по документации VU (НЕ по памяти):
//   content\vext\ref\fb\customizesoldierdata.md
//   content\vext\ref\fb\unlockweaponandslot.md
//   content\vext\ref\fb\unlockasset.md
//   content\vext\ref\fb\soldierweaponunlockasset.md
//
// В тест-хосте движка нет, поэтому возвращаются Lua-структуры (таблицы) с
// полями доки и нужными методами:
//   - CustomizeSoldierData().weapons:add(...) — массив + add;
//   - UnlockWeaponAndSlot().unlockAssets:add(...) — массив + add;
//   - SoldierWeaponUnlockAsset(...) / UnlockAsset(...) — простые обёртки.

#include "VextCustomization.h"

#include <string>

#include "EVBotsLog.h"
#include "LuaRuntime.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

constexpr char kCustomizationSource[] = R"LUA(
-- fb-типы кастомизации. Регистрируются после VextEnums (нужен WeaponSlot).

local log, constructorContext = ...

local function IsDataContainer(v)
  if type(v) ~= 'userdata' then return false end
  local mt = getmetatable(v)
  return mt ~= nil and mt.__name == 'vext.datacontainer'
end

local function IsGuid(v)
  return type(v) == 'table' and v._b ~= nil and type(v.ToString) == 'function'
end

local function NativeConstructor(name, guid)
  local _, inGame = constructorContext()
  local native = rawget(_G, 'EVBotsCreateDataContainer')
  if type(native) == 'function' then
    local result, available = native(name, guid)
    if available then
      if result == nil then error(name .. ': native constructor returned nil', 3) end
      return result, true
    end
  end
  if inGame then error(name .. ': native constructor is unavailable', 3) end
  return nil, false
end

local function ConstructorArgument(name, count, value)
  if count > 1 then error(name .. ': expected zero arguments, a Guid or an instance', 3) end
  if count == 1 and value == nil then error(name .. ': explicit nil is not an instance or Guid', 3) end
  if value ~= nil and not IsGuid(value) then error(name .. ': expected a native instance or Guid', 3) end
end

-- ── вспомогательный: структура с методом add ────────────────────────────────
-- Список структур в frostbite ведёт себя как массив с .add(). Сделаем одну
-- фабрику: создаём таблицу с метаметодом add.
local function NewList()
  local t = {}
  local list = { items = t }
  return setmetatable({}, {
    __index = function(self, k)
      if k == "add" then
        return function(_, item)
          t[#t + 1] = item
          -- чтобы #list в Lua считался корректно (массив внутри), не делим:
          rawset(self, #t, item)
        end
      end
      local v = t[k]
      if v ~= nil then return v end
      if k == "items" then return t end
      return nil
    end,
    __newindex = function(self, k, v)
      t[k] = v
    end,
    __len = function() return #t end,
    __pairs = function() return next, t, nil end,
  })
end

-- ── UnlockWeaponAndSlot (fb\unlockweaponandslot.md) ──────────────────────────
-- Свойства: weapon (SoldierWeaponUnlockAsset | nil), slot (WeaponSlot),
-- unlockAssets (UnlockAssetBase[]). Метод Clone().
local UnlockWeaponAndSlot = {}
UnlockWeaponAndSlot.__index = UnlockWeaponAndSlot

function UnlockWeaponAndSlot:Clone()
  local s_RawWeaponAlreadyAddedOrNot = setmetatable({
    weapon = self.weapon,
    slot = self.slot,
    unlockAssets = NewList(),
  }, UnlockWeaponAndSlot)
  if self._unlockItems then
    s_RawWeaponAlreadyAddedOrNot._unlockItems = self._unlockItems
  end
  return s_RawWeaponAlreadyAddedOrNot
end

local function NewUnlockWeaponAndSlot()
  local s = {
    weapon = nil,
    slot = 0,
    _unlockItems = NewList(),
  }
  s.unlockAssets = s._unlockItems
  return setmetatable(s, UnlockWeaponAndSlot)
end

_G.UnlockWeaponAndSlot = setmetatable({}, {
  __call = function(_, ...)
    local data = (...)
    if IsDataContainer(data) then return data end
    local count = select('#', ...)
    ConstructorArgument('UnlockWeaponAndSlot', count, data)
    local legacy = constructorContext()
    if count == 0 and legacy then return NewUnlockWeaponAndSlot() end
    local result, available = NativeConstructor('UnlockWeaponAndSlot', data)
    if available then return result end
    local offline = NewUnlockWeaponAndSlot()
    offline.instanceGuid = data
    return offline
  end
})

-- ── SoldierWeaponUnlockAsset (fb\soldierweaponunlockasset.md) ───────────────
-- В VU это «каст» к типу: _G[ИмяКласса](data) возвращает сам DataContainer,
-- помеченный этим классом. Раньше здесь была таблица-обёртка {weapon = data},
-- которая ломала мод: fun-bots после SearchForDataContainer делает
-- _G[obj.typeInfo.name](obj) (EbxEditUtils.lua:19), и обёртка теряла typeInfo,
-- поля и MakeWritable.
-- Поэтому: если на входе настоящий DataContainer (userdata с метатаблицой
-- vext.datacontainer, см. VextResourceManager.cpp) — отдаём его же,
-- иначе сохраняем прежнее поведение для обычных таблиц.
_G.SoldierWeaponUnlockAsset = setmetatable({}, { __call = function(_, data)
  if IsDataContainer(data) then return data end
  return { weapon = data }
end })

-- ── UnlockAsset (fb\unlockasset.md) ─────────────────────────────────────────
-- Тот же принцип: настоящий DataContainer проходит насквозь.
_G.UnlockAsset = setmetatable({}, { __call = function(_, data)
  if IsDataContainer(data) then return data end
  return { linkedTo = { data } }
end })

-- ── CustomizeSoldierData (fb\customizesoldierdata.md) ───────────────────────
-- Свойства: weapons (UnlockWeaponAndSlot[]), activeSlot (WeaponSlot),
-- removeAllExistingWeapons (bool), disableDeathPickup (bool),
-- deathPickup (PickupEntityAsset | nil).
local CustomizeSoldierData = {}
CustomizeSoldierData.__index = CustomizeSoldierData

local function NewCustomizeSoldierData()
  return setmetatable({
    weapons = NewList(),
    activeSlot = 0,
    removeAllExistingWeapons = false,
    disableDeathPickup = false,
    deathPickup = nil,
  }, CustomizeSoldierData)
end

_G.CustomizeSoldierData = setmetatable({}, {
  __call = function(_, ...)
    local data = (...)
    if IsDataContainer(data) then return data end
    local count = select('#', ...)
    ConstructorArgument('CustomizeSoldierData', count, data)
    local legacy = constructorContext()
    if count == 0 and legacy then return NewCustomizeSoldierData() end
    local result, available = NativeConstructor('CustomizeSoldierData', data)
    if available then return result end
    local offline = NewCustomizeSoldierData()
    offline.instanceGuid = data
    return offline
  end
})
)LUA";

int l_customization_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: Customization: %s", text);
    return 0;
}

int l_customization_context(lua_State* L) {
    // Preserve the established managed zero-argument fun-bots path. Compare the
    // actual owner folder component, never display Name or a Lua supplied flag.
    const std::wstring& folder=LuaRuntime::Instance().currentModFolder();
    const size_t end=folder.find_last_not_of(L"\\/");
    const size_t slash=end==std::wstring::npos?std::wstring::npos:folder.find_last_of(L"\\/",end);
    const size_t start=slash==std::wstring::npos?0:slash+1;
    const bool legacy=end!=std::wstring::npos && end-start+1==8 &&
        CompareStringOrdinal(folder.data()+start,8,L"fun-bots",8,TRUE)==CSTR_EQUAL;
    lua_pushboolean(L,legacy);
    lua_pushboolean(L,GetModuleHandleA("bf3.exe") || GetModuleHandleA("bf3_server_Final.exe") || GetModuleHandleA("zbf3.exe"));
    return 2;
}

}  // namespace

bool RegisterVextCustomization(lua_State* L) {
    if (luaL_loadbufferx(L, kCustomizationSource,
                         sizeof(kCustomizationSource) - 1,
                         "=[vext-customization]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Customization не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_customization_log);
    lua_pushcfunction(L, l_customization_context);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Customization не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: Customization fb-типы подключены");
    return true;
}

}  // namespace evbots
