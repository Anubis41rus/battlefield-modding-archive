// LuaEvents.cpp — библиотека Events VeniceEXT: подписки и рассылка событий.
//
// Реализовано строго по документации VU, без добавлений:
//   content\vext\ref\shared\library\events.md — Subscribe/Dispatch/
//       DispatchLocal/Unsubscribe;
//   content\vext\ref\shared\type\event.md — объект Event и его Unsubscribe;
//   content\vext\guides\events.md — user data, локальные события, допустимые
//       типы данных и то, что имена событий регистрозависимы.
//
// Вся логика — на Lua и встроена в DLL строкой (как MiddleClass.cpp): рядом
// с сервером никаких .lua лежать не должно. Из C++ в чанк передаются ровно
// два обратных вызова:
//   log(level, text)  — писать в лог EVBots;
//   currentMod()      — папка мода, чей скрипт исполняется прямо сейчас.
//
// currentMod() нужен для DispatchLocal: мод видит только свои подписки, а не
// чужие. Он читается из LuaRuntime::Instance().currentModFolder(), который
// LuaRuntime выставляет на время выполнения __init__.lua мода. Пока
// выполняется обработчик события, «текущим модом» считается владелец
// подписки — поэтому подписка, сделанная из обработчика, тоже попадает
// в DispatchLocal своего мода.
//
// События движка (Player:Joining, Engine:Init и т.п.) здесь не рассылаются:
// их вызывают хуки, это делает EngineHooks.cpp. Здесь только механизм.

#include "LuaEvents.h"

#include <Windows.h>

#include <string>

#include "EVBotsLog.h"
#include "LuaRuntime.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace evbots {
namespace {

// Исходник Events, встроенный в DLL. Сначала идут аргументы чанка:
// логгер, «кто сейчас исполняется», и обёртки scope (push/pop текущего мода).
constexpr char kEventsSource[] = R"LUA(
local log, currentMod, pushScope, popScope = ...

local Events = {}
local failedCallbacks = 0
-- Внутренний счётчик загрузчика; публичный API Dispatch не меняется.
function Events:_FailureCount() return failedCallbacks end

-- подписки: имя события -> список записей. Имя регистрозависимо
-- (guides\events.md), поэтому сравниваем строки как есть.
local subs = {}

-- мод, чей обработчик выполняется прямо сейчас: обработчик вправе
-- подписаться сам, и его подписка должна принадлежать ему же.
local runningMod = nil
-- Сборка 201: обработчик ошибок xpcall — стек в момент ошибки (после pcall он уже развёрнут).
local function evErrHandler(e)
  return { msg = tostring(e), tb = debug.traceback(tostring(e), 2) }
end

-- мод, в контексте которого выполняется код
local function scope()
  if runningMod ~= nil then
    return runningMod
  end
  local mod = currentMod()
  if mod == nil then
    return ""
  end
  return mod
end

-- обработчик — это «callable»: функция или объект с __call
local function isCallable(value)
  if type(value) == "function" then
    return true
  end
  if type(value) == "table" or type(value) == "userdata" then
    local mt = getmetatable(value)
    return type(mt) == "table" and mt.__call ~= nil
  end
  return false
end

-- Проверка данных события: guides\events.md разрешает только скаляры Lua
-- и таблицы из них. Возвращает текст ошибки или nil.
local function badValue(value, depth)
  local t = type(value)
  if value == nil or t == "boolean" or t == "number" or t == "string" then
    return nil
  end
  if t == "table" then
    if depth >= 8 then
      return "слишком глубокая вложенность таблицы"
    end
    for key, item in pairs(value) do
      local bad = badValue(key, depth + 1) or badValue(item, depth + 1)
      if bad ~= nil then
        return bad
      end
    end
    return nil
  end
  return "неподдерживаемый тип данных: " .. t
end

-- снять одну подписку; alive держит для рассылки, которая уже началась
local function removeEntry(eventName, entry)
  local list = subs[eventName]
  if list ~= nil then
    for i = 1, #list do
      if list[i] == entry then
        table.remove(list, i)
        break
      end
    end
    if #list == 0 then
      subs[eventName] = nil
    end
  end
  entry.alive = false
end

-- Объект типа Event (ref\shared\type\event.md): у него один метод —
-- Unsubscribe. Повторный вызов безвреден.
local function makeEvent(eventName, entry)
  local event = {}
  function event:Unsubscribe()
    if entry.alive then
      removeEntry(eventName, entry)
    end
  end
  return event
end

-- Subscribe(eventName, callback) либо Subscribe(eventName, context, callback)
function Events:Subscribe(eventName, ...)
  if type(eventName) ~= "string" then
    error("Events:Subscribe: имя события должно быть строкой", 2)
  end

  local count = select("#", ...)
  if count < 1 then
    error("Events:Subscribe: не передан обработчик", 2)
  end

  local userData, callback
  if count == 1 then
    callback = (select(1, ...))
  else
    userData, callback = ...
  end

  if not isCallable(callback) then
    error("Events:Subscribe: обработчик должен быть функцией", 2)
  end

  local entry = {
    owner = scope(),
    userData = userData,
    hasUserData = count > 1,
    callback = callback,
    alive = true,
  }

  local list = subs[eventName]
  if list == nil then
    list = {}
    subs[eventName] = list
  end
  list[#list + 1] = entry

  return makeEvent(eventName, entry)
end

-- ★ Сборка 164: профиль обработчиков (кто держит поток Lua). Ключ — событие + файл:строка обработчика.
-- Раз в 20 с (по Engine:Update) — топ по сумме времени в лог. Включён всегда (os.getenv в песочнице нет).
local profOn = true
local prof = { at = os.clock(), per = {}, tick = 0, tickMax = 0, ticks = 0, slow = 0 }
local function profKey(eventName, entry)
  local k = entry._pkey
  if k == nil then
    local ok, info = pcall(debug.getinfo, entry.callback, "S")
    local src = ok and info and (tostring(info.short_src) .. ":" .. tostring(info.linedefined)) or "?"
    k = eventName .. " " .. src
    entry._pkey = k
  end
  return k
end
-- ★ Сборка 165: (1) профиль вызовов API (таблицы рантайма: время каждой функции — если тормозит наш C++);
-- (2) выборочный профиль строк Lua (debug.sethook по счётчику инструкций — если время уходит в сам Lua мода).
-- Ставятся при первом отчёте (моды уже загружены). Прогон 164: обработчик fun-bots Engine:Update держал поток
-- Lua 5–7 с за вызов — нужно знать, где именно.
local apiProf, lineProf, profInstalled = {}, {}, false
local function installProfilers()
  profInstalled = true
  -- Сборка 167: обёртка API и выборка строк — только по EVBOTS_API_PROF=1 (сама стоит заметно; 165–166 своё дала).
  local nb = rawget(_G, "NativeBridge")
  if not (type(nb) == "table" and nb.EnvFlag ~= nil and nb.EnvFlag("EVBOTS_API_PROF")) then return end
  local names = { "NativeBridge", "ResourceManager", "EntityManager", "PlayerManager", "SQL", "RCON", "TeamSquadManager",
                  "TicketManager", "ChatManager", "NetEvents", "SharedUtils", "ServerUtils", "RaycastManager", "Hooks" }
  local wrapped = 0
  for _, tn in ipairs(names) do
    local T = rawget(_G, tn)
    if type(T) == "table" then
      local fns = {}
      for k, v in pairs(T) do
        if type(v) == "function" and type(k) == "string" and k:sub(1, 1) ~= "_" then fns[k] = v end
      end
      for k, v in pairs(fns) do
        local key = tn .. "." .. k
        T[k] = function(...)
          local t0 = os.clock()
          local r = table.pack(v(...))
          local dt = os.clock() - t0
          local p = apiProf[key]
          if p == nil then p = { n = 0, sum = 0, max = 0 }; apiProf[key] = p end
          p.n = p.n + 1
          p.sum = p.sum + dt
          if dt > p.max then p.max = dt end
          return table.unpack(r, 1, r.n)
        end
        wrapped = wrapped + 1
      end
    end
  end
  if debug ~= nil and debug.sethook ~= nil then
    debug.sethook(function()
      local info = debug.getinfo(2, "Sl")
      if info ~= nil then
        local key = tostring(info.short_src) .. ":" .. tostring(info.currentline)
        lineProf[key] = (lineProf[key] or 0) + 1
      end
    end, "", 20000)
  end
  log("INFO", "ДИАГ профиль: обёрнуто функций API " .. wrapped .. ", выборка строк Lua — каждые 20000 инструкций")
end
local function topOf(t, value, fmt, nmax)
  local rows = {}
  for k, v in pairs(t) do rows[#rows + 1] = { k = k, v = v } end
  table.sort(rows, function(a, b) return value(a.v) > value(b.v) end)
  local parts = {}
  for i = 1, math.min(nmax, #rows) do parts[#parts + 1] = fmt(rows[i].k, rows[i].v) end
  return table.concat(parts, " ; ")
end
local function profReport()
  local now = os.clock()
  if now - prof.at < 20 then return end
  local span = now - prof.at
  prof.at = now
  if not profInstalled then
    installProfilers()
  elseif next(apiProf) ~= nil or next(lineProf) ~= nil then
    log("INFO", "ДИАГ API-профиль 20 с: " .. topOf(apiProf, function(p) return p.sum end,
        function(k, p) return string.format("%s %.0f мс (макс %.0f, n=%d)", k, p.sum * 1000, p.max * 1000, p.n) end, 10))
    log("INFO", "ДИАГ строки Lua 20 с (выборки): " .. topOf(lineProf, function(v) return v end,
        function(k, v) return k .. "=" .. v end, 12))
    apiProf, lineProf = {}, {}
  end
  local rows = {}
  for k, p in pairs(prof.per) do rows[#rows + 1] = { k = k, p = p } end
  table.sort(rows, function(a, b) return a.p.sum > b.p.sum end)
  local parts = {}
  for i = 1, math.min(8, #rows) do
    local r = rows[i]
    parts[#parts + 1] = string.format("%s: %.0f мс (макс %.0f, n=%d)", r.k, r.p.sum * 1000, r.p.max * 1000, r.p.n)
  end
  log("INFO", string.format("ДИАГ Lua-профиль %.0f с: тиков Engine:Update %d, тик макс %.0f мс, тиков >33 мс %d | %s",
      span, prof.ticks, prof.tickMax * 1000, prof.slow, table.concat(parts, " ; ")))
  prof.per = {}
  prof.ticks, prof.tickMax, prof.slow = 0, 0, 0
end

-- общая рассылка: localOnly = true -> только подписки текущего мода
local function dispatch(eventName, localOnly, ...)
  local list = subs[eventName]
  if list == nil then
    return
  end

  -- снимок списка: обработчик вправе подписываться и отписываться
  local snapshot = {}
  for i = 1, #list do
    snapshot[i] = list[i]
  end

  local args = table.pack(...)
  local mine = scope()
  local tickT0 = profOn and os.clock() or 0

  for i = 1, #snapshot do
    local entry = snapshot[i]
    -- за время рассылки подписку могли снять
    if entry.alive and (not localOnly or entry.owner == mine) then
      local saved = runningMod
      runningMod = entry.owner
      -- владелец подписки => «текущий мод» для require внутри обработчика
      local prevScope = pushScope(entry.owner)
      local ok, err
      local t0 = profOn and os.clock() or 0
      if entry.hasUserData then
        ok, err = xpcall(entry.callback, evErrHandler, entry.userData,
                         table.unpack(args, 1, args.n))
      else
        ok, err = xpcall(entry.callback, evErrHandler, table.unpack(args, 1, args.n))
      end
      if profOn then
        local dt = os.clock() - t0
        local key = profKey(eventName, entry)
        local p = prof.per[key]
        if p == nil then p = { n = 0, sum = 0, max = 0 }; prof.per[key] = p end
        p.n = p.n + 1
        p.sum = p.sum + dt
        if dt > p.max then p.max = dt end
      end
      popScope(prevScope)
      runningMod = saved
      if not ok then
        failedCallbacks = failedCallbacks + 1
        -- Сборка 201: стек снят в момент ошибки (xpcall), а не после разворота — видны вызывающие функции мода.
        local errMsg = type(err) == "table" and err.msg or tostring(err)
        log("ERR ", "обработчик \"" .. eventName .. "\" упал: " .. errMsg)
        local tb = type(err) == "table" and err.tb or debug.traceback(errMsg, 1)
        for line in tb:gmatch("[^\r\n]+") do
          log("ERR ", "  | " .. line)
        end
      end
    end
  end
  -- ★ 164: тик = Engine:Update + UpdateManager:Update (оба идут подряд из таймера)
  if profOn and (eventName == "Engine:Update" or eventName == "UpdateManager:Update") then
    local dt = os.clock() - tickT0
    prof.tick = prof.tick + dt
    if eventName == "UpdateManager:Update" then
      prof.ticks = prof.ticks + 1
      if prof.tick > prof.tickMax then prof.tickMax = prof.tick end
      if prof.tick > 0.033 then prof.slow = prof.slow + 1 end
      prof.tick = 0
      profReport()
    end
  end
end

-- проверка аргументов рассылки: только скаляры Lua и таблицы из них
local function checkArgs(method, eventName, ...)
  if type(eventName) ~= "string" then
    error("Events:" .. method .. ": имя события должно быть строкой", 3)
  end
  for i = 1, select("#", ...) do
    local bad = badValue((select(i, ...)), 0)
    if bad ~= nil then
      error("Events:" .. method .. ": " .. bad .. " (аргумент " .. i .. ")",
            3)
    end
  end
end

-- событие уходит всем загруженным модам
function Events:Dispatch(eventName, ...)
  checkArgs("Dispatch", eventName, ...)
  return dispatch(eventName, false, ...)
end

-- Событие ДВИЖКА (Player:Killed и т.п.) — не часть API модов. Аргументы — объекты (Player,
-- SoldierEntity): в VU их передаёт движок, а ограничение guides\events.md на типы данных
-- касается только Dispatch из модов. Поэтому checkArgs здесь нет.
function Events:_DispatchEngine(eventName, ...)
  if type(eventName) ~= "string" then
    error("Events:_DispatchEngine: имя события должно быть строкой", 2)
  end
  return dispatch(eventName, false, ...)
end

-- Событие своего мода передаёт исходные Lua-ссылки, без межмодовой сериализации.
-- VU: 10BBEB00 -> 10BB7380 -> 10BBA750/10BBA6C0 используют lua_pushvalue.
-- Поэтому таблицы с методами/циклами, функции и userdata здесь допустимы.
function Events:DispatchLocal(eventName, ...)
  if type(eventName) ~= "string" then
    error("Events:DispatchLocal: имя события должно быть строкой", 2)
  end
  return dispatch(eventName, true, ...)
end

-- Unsubscribe(eventName) -> снять все подписки на имя;
-- Unsubscribe() -> снять вообще всё
function Events:Unsubscribe(eventName)
  if eventName == nil then
    for _, list in pairs(subs) do
      for i = 1, #list do
        list[i].alive = false
      end
    end
    subs = {}
    return
  end

  if type(eventName) ~= "string" then
    error("Events:Unsubscribe: имя события должно быть строкой", 2)
  end

  local list = subs[eventName]
  if list ~= nil then
    for i = 1, #list do
      list[i].alive = false
    end
    subs[eventName] = nil
  end
end

return Events
)LUA";

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                            static_cast<int>(w.size()), nullptr,
                                            0, nullptr, nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                           static_cast<int>(s.size()), nullptr,
                                           0);
    if (need <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          out.data(), need);
    return out;
}

/// Писать в лог EVBots (передаётся в Lua первым аргументом чанка).
int l_events_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: Events: %s", text);
    return 0;
}

/// Папка мода, чей скрипт исполняется прямо сейчас; "" — вне мода.
/// Нужна, чтобы DispatchLocal доставал только подписки своего мода.
int l_events_current_mod(lua_State* L) {
    const std::string key =
        NarrowUtf8(LuaRuntime::Instance().currentModFolder());
    lua_pushlstring(L, key.data(), key.size());
    return 1;
}

/// Установить "текущего мода" = владельцу подписки (для require внутри
/// обработчика). Возвращает прежнее значение — его вернёт popScope.
int l_events_push_scope(lua_State* L) {
    size_t len = 0;
    const char* folder = luaL_optlstring(L, 1, "", &len);
    std::wstring w = WidenUtf8(std::string(folder, len));
    const std::wstring prev = LuaRuntime::Instance().PushCurrentMod(w);
    lua_pushlstring(L, NarrowUtf8(prev).data(), NarrowUtf8(prev).size());
    return 1;
}

/// Вернуть "текущего мода", который был до pushScope.
int l_events_pop_scope(lua_State* L) {
    size_t len = 0;
    const char* folder = luaL_optlstring(L, 1, "", &len);
    LuaRuntime::Instance().PopCurrentMod(WidenUtf8(std::string(folder, len)));
    return 0;
}

}  // namespace

bool RegisterLuaEvents(lua_State* L) {
    static const int kSourceLen = static_cast<int>(sizeof(kEventsSource) - 1);
    if (luaL_loadbufferx(L, kEventsSource, kSourceLen, "=[events]", "t") !=
        LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Events не собрался: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    lua_pushcfunction(L, l_events_log);
    lua_pushcfunction(L, l_events_current_mod);
    lua_pushcfunction(L, l_events_push_scope);
    lua_pushcfunction(L, l_events_pop_scope);
    if (lua_pcall(L, 4, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Events не выполнился: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    if (!lua_istable(L, -1)) {
        LOG_ERR("EVBots: Events вернул не таблицу");
        lua_pop(L, 1);
        return false;
    }

    lua_setglobal(L, "Events");
    LOG_INFO("EVBots: библиотека Events подключена "
             "(Subscribe/Unsubscribe/Dispatch/DispatchLocal)");
    return true;
}

}  // namespace evbots
