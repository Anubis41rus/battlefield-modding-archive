// VextHooks.cpp — библиотека VeniceEXT Hooks для EVBots.
//
// Глобальная таблица Hooks (shared) и типы Hook, HookContext — по
// документации VU (НЕ по памяти):
//   content\vext\ref\shared\library\hooks.md      — Hooks:Install;
//   content\vext\ref\shared\type\hook.md          — объект Hook (priority,
//                                                   Uninstall);
//   content\vext\ref\shared\type\hookcontext.md   — HookContext (Pass/Return/
//                                                   Call) — выдаётся функции
//                                                   обработчика при вызове.
//
// Что настоящее:
//   - Hooks:Install хранит обработчик в реестре по имени хука с приоритетом
//     и возвращает объект Hook (с полем priority, методом Uninstall).
//     Реестр пригодится, когда появятся настоящие движковые хуки: по имени
//     сможем дёрнуть цепочку обработчиков (как VU).
//   - Uninstall снимает обработчик из реестра.
//
// Что заглушка: реальные движковые срабатывания (в тест-хосте движка нет),
// поэтому HookContext здесь — минимальный объект с методами Pass/Return/Call
// из доки: собран на случай, если обработчик хука будет вызван нами.
// pushHookEventCb далее опишу — но на текущем этапе не используется.

#include "VextHooks.h"

#include <string>
#include <Windows.h>

#include "EVBotsLog.h"
#include "LuaRuntime.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// Чанк получает первым аргументом функцию логирования (как LuaEvents).
constexpr char kHooksSource[] = R"LUA(
-- Hooks VeniceEXT (VU) — по content\vext\ref\shared\library\hooks.md

local log, currentMod, pushScope = ...

local function callScoped(h, ctx, ...)
  local previous = pushScope(h.owner)
  local ok, err
  if h.hasContext then
    ok, err = pcall(h.callback, h.context, ctx, ...)
  else
    ok, err = pcall(h.callback, ctx, ...)
  end
  pushScope(previous)
  return ok, err
end

local Hooks = {}
_G.Hooks = Hooks

-- ── реестр хуков: name -> { handle -> { priority, context, hasContext,
--                                          callback, alive } } ──────────────
local hooks = {}
local handlersByName = {}     -- name -> array of entries (по приоритету)
local nextHandle = 1

-- ── Hook ────────────────────────────────────────────────────────────────────
local Hook = {}
Hook.__index = Hook

function Hook:Uninstall()
  local name = self._name
  if handlersByName[name] == nil or self._handle == nil then
    return
  end
  local list = handlersByName[name]
  for i = 1, #list do
    if list[i]._handle == self._handle then
      table.remove(list, i)
      break
    end
  end
  if #list == 0 then
    handlersByName[name] = nil
    hooks[self._handle] = nil
  end
end

-- ── HookContext (shared\type\hookcontext.md) ────────────────────────────────
-- Нужен только если обработчик хука реально вызван (в тест-хосте — нет).
-- Собираем по доке: Pass(...)/Return()/Call().
local HookContext = {}
HookContext.__index = HookContext

function HookContext:Pass(...)
  self._mode = "pass"
  self._args = { ... }
end

function HookContext:Return(returnValue)
  self._mode = "return"
  self._returnValue = returnValue
end

function HookContext:Call()
  -- ★ Сборка 163: как в VU — вызвать оригинал движка (если мост дал функцию), один раз
  if self._callFn ~= nil and not self._called then
    self._called = true
    self._returnValue = self._callFn()
  end
  return self._returnValue
end

local function MakeHookContext()
  return setmetatable({ _mode = "pass", _args = {}, _returnValue = nil },
                      HookContext)
end

-- ── Hooks:Install ───────────────────────────────────────────────────────────
-- Install(name, priority, callback) | Install(name, priority, context, callback)
function Hooks:Install(hookName, priority, ...)
  if type(hookName) ~= "string" then
    error("Hooks:Install: имя хука должно быть строкой", 2)
  end
  if type(priority) ~= "number" then
    error("Hooks:Install: приоритет должен быть числом", 2)
  end

  local count = select("#", ...)
  if count < 1 then
    error("Hooks:Install: не передан обработчик", 2)
  end

  local context, callback
  if count == 1 then
    callback = select(1, ...)
  else
    context, callback = ...
  end

  local function isCallable(v)
    return type(v) == "function" or (type(v) == "table" and v.__call ~= nil)
  end
  if not isCallable(callback) then
    error("Hooks:Install: обработчик должен быть функцией", 2)
  end

  local handle = nextHandle
  nextHandle = nextHandle + 1

  hooks[handle] = {
    name = hookName,
    priority = priority,
    context = context,
    hasContext = count > 1,
    callback = callback,
    owner = currentMod(),
    alive = true,
  }

  handlersByName[hookName] = handlersByName[hookName] or {}
  local list = handlersByName[hookName]
  list[#list + 1] = { _handle = handle, _name = hookName }

  log("DBG ", "Hooks:Install(\"" .. hookName .. "\", priority=" ..
      tostring(priority) .. ") -> handle=" .. tostring(handle))

  local hookObj = setmetatable({ _handle = handle, _name = hookName,
                                 priority = priority }, Hook)
  return hookObj
end

-- ★ Сборка 153: вызов цепочки обработчиков хука из моста (первый — Soldier:Damage). Каждый получает свой
-- HookContext и аргументы (VU: callback(hookCtx, ...) или callback(context, hookCtx, ...)); ошибки обработчика — в лог.
-- Возвращает число вызванных обработчиков и аргументы после Pass (если обработчик их подменил).
function Hooks._Fire(hookName, ...)
  local list = handlersByName[hookName]
  if list == nil then return 0 end
  local args = { ... }
  local called = 0
  for i = 1, #list do
    local h = hooks[list[i]._handle]
    if h ~= nil and h.alive then
      local ctx = MakeHookContext()
      ctx._args = args
      local ok, err = callScoped(h, ctx, table.unpack(args))
      called = called + 1
      if not ok then
        log("ERR ", "хук " .. tostring(hookName) .. ": ошибка обработчика: " .. tostring(err))
      elseif ctx._mode == "return" then
        return called, ctx._returnValue
      elseif ctx._mode == "pass" and #ctx._args > 0 then
        args = ctx._args
      end
    end
  end
  return called, table.unpack(args)
end

-- ★ Сборка 163: хук, у которого есть оригинал движка (BulletEntity:Collision). callFn вызывает оригинал;
-- ctx:Call() у обработчика зовёт его сам (как в VU), иначе мост вызовет оригинал после цепочки.
-- Возвращает число вызванных обработчиков.
function Hooks._FireCall(hookName, callFn, ...)
  local list = handlersByName[hookName]
  if list == nil then return 0 end
  local args = { ... }
  local called = 0
  for i = 1, #list do
    local h = hooks[list[i]._handle]
    if h ~= nil and h.alive then
      local ctx = MakeHookContext()
      ctx._args = args
      ctx._callFn = callFn
      local ok, err = callScoped(h, ctx, table.unpack(args))
      called = called + 1
      if not ok then
        log("ERR ", "хук " .. tostring(hookName) .. ": ошибка обработчика: " .. tostring(err))
      end
      if ctx._called or ctx._mode == "return" then
        return called
      end
    end
  end
  return called
end

-- ★ Сборка 234: как _Fire, но с режимом для моста: "return", значение Return(...) — оригинал движка не звать;
-- "pass", аргументы (после Pass, если подменили) — звать оригинал. Первым — число вызванных обработчиков.
function Hooks._FireEx(hookName, ...)
  local list = handlersByName[hookName]
  if list == nil then return 0, "pass", ... end
  local args = table.pack(...)
  local called = 0
  for i = 1, #list do
    local h = hooks[list[i]._handle]
    if h ~= nil and h.alive then
      local ctx = MakeHookContext()
      ctx._args = {}
      local ok, err = callScoped(h, ctx, table.unpack(args, 1, args.n))
      called = called + 1
      if not ok then
        log("ERR ", "хук " .. tostring(hookName) .. ": ошибка обработчика: " .. tostring(err))
      elseif ctx._mode == "return" then
        return called, "return", ctx._returnValue
      elseif ctx._mode == "pass" and #ctx._args > 0 then
        args = table.pack(table.unpack(ctx._args))
      end
    end
  end
  return called, "pass", table.unpack(args, 1, args.n)
end

return Hooks
)LUA";

int l_hooks_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: Hooks: %s", text);
    return 0;
}

void pushFolder(lua_State* L, const std::wstring& folder) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), nullptr, 0, nullptr, nullptr);
    std::string value(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), value.data(), n, nullptr, nullptr);
    lua_pushlstring(L, value.data(), value.size());
}

int l_hooks_current_mod(lua_State* L) {
    pushFolder(L, LuaRuntime::Instance().currentModFolder());
    return 1;
}

int l_hooks_push_scope(lua_State* L) {
    size_t len = 0;
    const char* value = luaL_checklstring(L, 1, &len);
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(len), nullptr, 0);
    if (len && !n) return luaL_error(L, "invalid hook mod scope");
    std::wstring folder(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(len), folder.data(), n);
    pushFolder(L, LuaRuntime::Instance().PushCurrentMod(folder));
    return 1;
}

}  // namespace

bool RegisterVextHooks(lua_State* L) {
    if (luaL_loadbufferx(L, kHooksSource, sizeof(kHooksSource) - 1,
                         "=[vext-hooks]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Hooks не собрались: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_hooks_log);
    lua_pushcfunction(L, l_hooks_current_mod);
    lua_pushcfunction(L, l_hooks_push_scope);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Hooks не выполнились: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: Hooks подключены, реестр обработчиков");
    return true;
}

}  // namespace evbots
