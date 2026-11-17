// VextNetEvents.cpp — серверная библиотека VeniceEXT NetEvents для EVBots.
//
// Реализована глобальная таблица NetEvents (сервер) — строго по документации
// VU (НЕ по памяти):
//   content\vext\ref\server\library\netevents.md
//
// Что в VU действительно делает NetEvents:
//   - Subscribe(eventName[, context], callback) — зарегистрировать обработчик
//     на сетевое событие, приходящее с клиента; возвращает NetEvent;
//   - SendTo / SendToLocal / Broadcast / BroadcastLocal и их unreliable и
//     unreliable-ordered варианты — отправить событие игрокам.
//
// ★ Сборка 219 (К4): Send*/Broadcast* уходят клиентам через канал ev-core.dll (как Harmony у VU), входящие с клиента —
// NetEvents._OnNet на тике, обработчик получает (player, ...args) — по guides\netevents.md. Флаг Local передаётся, но
// при приёме не фильтрует по моду (мод у нас один; реестр В9). Перехват моста (_Intercept, эмуляция лучей Б1) — первым.
// Сигнатуры и количество аргументов — ровно по доке, ничего не выдумано.

#include "VextNetEvents.h"

#include <string>

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// Чанк получает первым аргументом функцию логирования (как LuaEvents).
constexpr char kNetEventsSource[] = R"LUA(
-- Серверные NetEvents VeniceEXT (VU) — по
-- content\vext\ref\server\library\netevents.md

local log = ...

local NetEvents = {}
_G.NetEvents = NetEvents

-- подписки: имя -> массив записей { owner, userData, hasUserData, callback, alive }
local subs = {}

local function isCallable(v)
  return type(v) == "function" or (type(v) == "table" and v.__call ~= nil)
end

local function makeNetEvent(eventName, entry)
  local e = {}
  function e:Unsubscribe()
    if entry.alive then
      entry.alive = false
    end
  end
  return e
end

-- Subscribe(eventName, callback) либо Subscribe(eventName, context, callback)
function NetEvents:Subscribe(eventName, ...)
  if type(eventName) ~= "string" then
    error("NetEvents:Subscribe: имя события должно быть строкой", 2)
  end

  local count = select("#", ...)
  if count < 1 then
    error("NetEvents:Subscribe: не передан обработчик", 2)
  end

  local userData, callback
  if count == 1 then
    callback = (select(1, ...))
  else
    userData, callback = ...
  end

  if not isCallable(callback) then
    error("NetEvents:Subscribe: обработчик должен быть функцией", 2)
  end

  local entry = {
    owner = "",           -- мод-владелец; мост будет заполнять при реальной сети
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

  return makeNetEvent(eventName, entry)
end

-- ★ Сборка 219 (К4): отправка и приём через канал ev-core.dll (как Harmony у VU). Упаковка — EVNetCodec.
-- Соединения: conn -> { name = имя игрока из приветствия, addr, pending = { {flags, name, n, args}, … } }.
local conns = {}
local warned = {}
local function warnOnce(key, msg)
  if warned[key] then return end
  warned[key] = true
  log("INFO ", msg)
end

local function isHuman(p) return p ~= nil and p.onlineId ~= nil and p.onlineId ~= 0 end

-- Игрок соединения: по имени из приветствия; нет имени — единственный человек при единственном соединении (реестр В14).
local function playerOf(c)
  local pm = rawget(_G, "PlayerManager")
  if pm == nil then return nil end
  if c.name ~= nil and c.name ~= "" then
    local ok, p = pcall(pm.GetPlayerByName, pm, c.name)
    if ok and p ~= nil then return p end
  end
  local nconn = 0
  for _ in pairs(conns) do nconn = nconn + 1 end
  if nconn ~= 1 then return nil end
  local ok, list = pcall(pm.GetPlayers, pm)
  if not ok or type(list) ~= "table" then return nil end
  local human = nil
  for i = 1, #list do
    if isHuman(list[i]) then
      if human ~= nil then return nil end
      human = list[i]
    end
  end
  if human ~= nil and (c.name == nil or c.name == "") then
    c.name = human.name
    log("INFO ", "канал: соединение без имени привязано к единственному человеку " .. tostring(human.name))
  end
  return human
end

local function connOf(player)
  if player == nil then return nil end
  local name = player.name
  for conn, c in pairs(conns) do
    if c.name ~= nil and c.name == name then return conn end
  end
  for conn, c in pairs(conns) do
    local p = playerOf(c)
    if p ~= nil and p.name == name then return conn end
  end
  return nil
end

-- ★ Сборка 227: диагностика «бот видит игрока» — сколько Botmanager:RaycastResults пришло от клиентов и с какими Mode
-- (прогон 226: боты не стреляют, пока по ним не выстрелишь). Сводка — на каждые 300 событий, первая непустая — целиком.
local rr = { n = 0, empty = 0, modes = {}, firstFull = false }
local function countRaycastResults(c, ev)
  rr.n = rr.n + 1
  local list = ev.args and ev.args[1]
  if type(list) ~= "table" or #list == 0 then
    rr.empty = rr.empty + 1
  else
    for i = 1, #list do
      local e = list[i]
      local m = type(e) == "table" and tostring(e.Mode) or "?"
      rr.modes[m] = (rr.modes[m] or 0) + 1
      if not rr.firstFull and type(e) == "table" then
        rr.firstFull = true
        log("INFO ", "канал: ★ первая непустая RaycastResults от " .. tostring(c.name) .. ": Mode " .. tostring(e.Mode) ..
          ", Bot1 " .. tostring(e.Bot1) .. ", Bot2 " .. tostring(e.Bot2) .. ", IgnoreYaw " .. tostring(e.IgnoreYaw))
      end
      -- ★ Сборка 229: номер бота от клиента -> имя на сервере (сверить с «★ клиент: RaycastResults … = имя»)
      local b = type(e) == "table" and e.Bot1 or nil
      rr.ids = rr.ids or {}
      if type(b) == "number" and rr.ids[b] == nil and (rr.idsN or 0) < 12 then
        rr.ids[b] = true
        rr.idsN = (rr.idsN or 0) + 1
        local pm = rawget(_G, "PlayerManager")
        local ok, p = pcall(function() return pm and pm:GetPlayerById(b) end)
        log("INFO ", "канал: ★ RaycastResults Mode " .. m .. ": Bot1 " .. b .. " = " .. tostring(ok and p and p.name))
      end
    end
  end
  if rr.n % 300 == 0 then
    local o = {}
    for k, v in pairs(rr.modes) do o[#o + 1] = k .. "=" .. v end
    table.sort(o)
    log("INFO ", "канал: RaycastResults принято " .. rr.n .. " (пустых " .. rr.empty .. "), записей по Mode: " ..
      (#o > 0 and table.concat(o, ", ") or "—"))
  end
end

local nBotEditor = 0
local function deliverFrom(c, ev)
  if ev.name == "Botmanager:RaycastResults" then
    countRaycastResults(c, ev)
    -- ★ 247: клиент называет ботов движковыми номерами; мод — своими (id из CreatePlayer, не меняется, как VU) → переводим
    local pm = rawget(_G, "PlayerManager")
    local list = ev.args and ev.args[1]
    if pm ~= nil and pm._LiveId ~= nil and type(list) == "table" then
      for i = 1, #list do
        local e = list[i]
        if type(e) == "table" then
          if type(e.Bot1) == "number" and e.Bot1 >= 0 then e.Bot1 = pm._LiveId(e.Bot1) end
          if type(e.Bot2) == "number" and e.Bot2 > 0 then e.Bot2 = pm._LiveId(e.Bot2) end
        end
      end
    end
  end
  -- ★ 238-3: события от клиентского Lua мода (служебные «__ev_*» не в счёт) — у игрока работает клиент мода (_HasModClient)
  if type(ev.name) == "string" and ev.name:sub(1, 5) ~= "__ev_" then
    if (c.modEvents or 0) == 0 then
      log("INFO ", "канал: ★ клиент мода у " .. tostring(c.name) .. " работает (первое событие «" .. ev.name .. "»)")
    end
    c.modEvents = (c.modEvents or 0) + 1
  end
  local p = playerOf(c)
  if p == nil then
    if #c.pending < 256 then c.pending[#c.pending + 1] = ev
    else warnOnce("pend:" .. tostring(c.name), "канал: у соединения " .. tostring(c.name) .. " нет игрока, очередь полна — событие отброшено") end
    return
  end
  -- ★ 247 (друг: боты не вылезают из техники по Q): клиент шлёт «BotEditor», ошибок нет, а «ВЫХОД из техники» не бывает —
  -- мод (BotManager:ExitVehicle) молча выходит раньше. Строка: что пришло, есть ли солдат у человека, ближайший бот своей
  -- команды в технике (мод берёт только ближе Registry.COMMON.COMMAND_DISTANCE = 20 м). Только диагностика, до 40 строк.
  if ev.name == "BotEditor" then
    nBotEditor = (nBotEditor or 0) + 1
    if nBotEditor <= 40 then pcall(function()
      local a = tostring(ev.args and ev.args[1]):sub(1, 80)
      local hp = p.soldier ~= nil and p.soldier.worldTransform.trans or nil
      local best, nVeh, bestFoot, near = nil, 0, nil, 0
      local pm = rawget(_G, "PlayerManager")
      for _, b in ipairs(pm and pm:GetPlayersByTeam(p.teamId) or {}) do
        if b.onlineId == 0 and b.soldier ~= nil then
          local cc = b.inVehicle and (b.controlledControllable or b.attachedControllable) or nil
          local bp = cc ~= nil and cc.transform.trans or b.soldier.worldTransform.trans
          if hp ~= nil and bp ~= nil then
            local d = math.sqrt((bp.x - hp.x) ^ 2 + (bp.y - hp.y) ^ 2 + (bp.z - hp.z) ^ 2)
            if d < 20 then near = near + 1 end
            if b.inVehicle then
              if best == nil or d < best then best = d end
            elseif bestFoot == nil or d < bestFoot then bestFoot = d end
          end
          if b.inVehicle then nVeh = nVeh + 1 end
        end
      end
      -- 251: и пешие — «защищать/за мной/садитесь» мод отдаёт ботам ближе 20 м (BotManager:Attack/EnterVehicle)
      log("INFO ", string.format("канал: BotEditor от %s: %s | солдат у человека %s, ботов своей команды в технике %d, ближайший %s м; пеший ближайший %s м; ближе 20 м %d",
        tostring(p.name), a, tostring(hp ~= nil), nVeh, best and string.format("%.1f", best) or "-",
        bestFoot and string.format("%.1f", bestFoot) or "-", near))
    end) end
  end
  local ok, err = pcall(NetEvents._Deliver, NetEvents, ev.name, p, table.unpack(ev.args, 1, ev.n))
  if ok and ev.name == "Client:RequestSettings" then
    p.clientSettingsSent = true
    log("INFO ", "канал: Client:RequestSettings доставлен для " .. tostring(p.name))
  end
  if not ok then log("ERR ", "обработчик NetEvent «" .. tostring(ev.name) .. "» упал: " .. tostring(err)) end
end

-- Приём из сети (EvNetBridge на тике): kind 1 — соединение открыто (data = адрес), 2 — кадр, 3 — закрыто.
function NetEvents._OnNet(conn, kind, data)
  if kind == 1 then
    conns[conn] = { addr = data, pending = {} }
    log("INFO ", "канал: ★ клиент подключился, соединение " .. tostring(conn) .. " (" .. tostring(data) .. ")")
    return
  elseif kind == 3 then
    local c = conns[conn]
    conns[conn] = nil
    log("INFO ", "канал: ★ соединение " .. tostring(conn) .. " закрыто" .. (c and c.name and (" (" .. c.name .. ")") or ""))
    return
  end
  local c = conns[conn]
  if c == nil then
    c = { addr = "?", pending = {} }
    conns[conn] = c
  end
  local ok, k, a, b, n, args = pcall(EVNetCodec.Decode, data)
  if not ok then
    warnOnce("bad:" .. tostring(conn), "канал: кадр от соединения " .. tostring(conn) .. " не разобран: " .. tostring(k))
    return
  end
  if k == EVNetCodec.KIND_HELLO then
    c.name = a
    log("INFO ", "канал: ★ соединение " .. tostring(conn) .. " — игрок «" .. tostring(a) .. "»")
    -- ★ 239: игрок вошёл заново (клиент упал или завис, старое соединение не закрыто) — прежние соединения с тем же
    -- именем забываем: иначе connOf (pairs, порядок случайный) мог слать события мода в мёртвое соединение, а
    -- _HasModClient видел события старого клиента. Сокет закроет ev-core, когда его закроет система (kind 3).
    if a ~= nil and a ~= "" then
      for other, oc in pairs(conns) do
        if other ~= conn and oc.name == a then
          conns[other] = nil
          log("INFO ", "канал: ★ прежнее соединение " .. tostring(other) .. " игрока «" .. tostring(a) .. "» забыто — вошёл заново")
        end
      end
    end
  elseif k == EVNetCodec.KIND_EVENT then
    deliverFrom(c, { flags = a, name = b, n = n, args = args })
  end
end

-- ★ Сборка 221: есть ли у игрока живой канал (клиент EVBots подключён и представился его именем). Мост по нему решает,
-- нужна ли эмуляция клиента мода (Б1): настоящий клиент сам шлёт Client:RequestSettings.
function NetEvents._HasChannel(player)
  if player == nil or player.name == nil then return false end
  for _, c in pairs(conns) do
    if c.name == player.name then return true end
  end
  return false
end

-- ★ 238-3: у игрока работает клиент мода — от его соединения пришло хотя бы одно событие мода. Канал без событий (ev-core
-- подключился, а клиентский Lua мода не загрузился — нет файлов мода у гостя) не в счёт: тогда мост подменяет клиента (Б1).
function NetEvents._HasModClient(player)
  if player == nil or player.name == nil then return false end
  for _, c in pairs(conns) do
    if c.name == player.name and (c.modEvents or 0) > 0 then return true end
  end
  return false
end

-- Дослать отложенное, когда у соединения появился игрок (человек попадает в список мода позже, чем подключается канал).
-- ★ Сборка 224: список ботов клиентам (служебное «__ev_bots», модам не доставляется). На VU onlineId бота в движке — 0,
-- и клиент fun-bots отличает ботов по onlineId == 0 (ClientBotManager.lua: проверка врагов). У нас в движке у бота свой
-- onlineId (сборка 171, по нему бэкенд отдаёт статистику), клиент видел его ненулевым и не считал лучи (прогон 223).
-- Клиент по этому списку отдаёт моду onlineId = 0 (реестр В15). Проверка раз в 30 тиков (~1 с), шлём при изменении
-- и новым соединениям.
local botsTick, botsSig = 0, nil
local function botNames()
  local pm = rawget(_G, "PlayerManager")
  if pm == nil then return {} end
  local ok, list = pcall(pm.GetPlayers, pm)
  if not ok or type(list) ~= "table" then return {} end
  local names = {}
  for i = 1, #list do
    local p = list[i]
    if p ~= nil and p.onlineId == 0 and type(p.name) == "string" then names[#names + 1] = p.name end
  end
  table.sort(names)
  return names
end

function NetEvents._Pump()
  for _, c in pairs(conns) do
    if #c.pending > 0 and playerOf(c) ~= nil then
      local list = c.pending
      c.pending = {}
      for i = 1, #list do deliverFrom(c, list[i]) end
    end
  end
  botsTick = botsTick + 1
  if botsTick < 30 or next(conns) == nil then return end
  botsTick = 0
  local names = botNames()
  local sig = table.concat(names, "\n")
  local changed = sig ~= botsSig
  botsSig = sig
  local bytes = nil
  for conn, c in pairs(conns) do
    if changed or not c.botsSent then
      bytes = bytes or EVNetCodec.EncodeEvent(0, "__ev_bots", names)
      if __evnet_send(conn, bytes) then c.botsSent = true end
    end
  end
end

local function encode(flags, eventName, ...)
  local ok, bytes = pcall(EVNetCodec.EncodeEvent, flags, eventName, ...)
  if not ok then error(bytes, 4) end
  return bytes
end

local function sendTo(kind, flags, eventName, player, ...)
  if type(eventName) ~= "string" then
    error("NetEvents:" .. kind .. ": имя события должно быть строкой", 3)
  end
  local bytes = encode(flags, eventName, ...)
  if not __evnet_available() then
    warnOnce("noev", "NetEvents: канала нет (ev-core.dll не загружен) — " .. kind .. "(\"" .. eventName .. "\") не отправлено")
    return
  end
  local conn = connOf(player)
  if conn == nil then
    warnOnce(eventName .. "/" .. tostring(player and player.name), "NetEvents:" .. kind .. "(\"" .. eventName ..
      "\"): у игрока " .. tostring(player and player.name) .. " нет канала (бот или клиент без EVBots) — не отправлено")
    return
  end
  __evnet_send(conn, bytes)
end

local function broadcast(kind, flags, eventName, ...)
  if type(eventName) ~= "string" then
    error("NetEvents:" .. kind .. ": имя события должно быть строкой", 3)
  end
  local bytes = encode(flags, eventName, ...)
  if not __evnet_available() then
    warnOnce("noev", "NetEvents: канала нет (ev-core.dll не загружен) — " .. kind .. "(\"" .. eventName .. "\") не отправлено")
    return
  end
  __evnet_broadcast(bytes)
end

local L, U, O = 1, 2, 4

-- Сборка 121: перехват отправки и доставка подписчикам на сервере (мост сам выполняет логику клиента).
NetEvents._Intercept = {}
function NetEvents:_Deliver(eventName, player, ...)
  local list = subs[eventName]
  if list == nil then return end
  local copy = {}
  for i = 1, #list do copy[i] = list[i] end
  for i = 1, #copy do
    local entry = copy[i]
    if entry.alive then
      if entry.hasUserData then entry.callback(entry.userData, player, ...)
      else entry.callback(player, ...) end
    end
  end
end

function NetEvents:SendTo(eventName, player, ...) sendTo("SendTo", 0, eventName, player, ...) end
function NetEvents:SendToLocal(eventName, player, ...) sendTo("SendToLocal", L, eventName, player, ...) end
function NetEvents:Broadcast(eventName, ...) broadcast("Broadcast", 0, eventName, ...) end
function NetEvents:BroadcastLocal(eventName, ...) broadcast("BroadcastLocal", L, eventName, ...) end
function NetEvents:SendUnreliableTo(eventName, player, ...) sendTo("SendUnreliableTo", U, eventName, player, ...) end
function NetEvents:SendUnreliableToLocal(eventName, player, ...)
  if type(eventName) ~= "string" then
    error("NetEvents:SendUnreliableToLocal: имя события должно быть строкой", 2)
  end
  local hook = NetEvents._Intercept[eventName]
  -- ★ 225: перехват вернул false — у игрока свой клиент, отправляем по каналу
  if hook ~= nil and hook(player, ...) ~= false then return end
  sendTo("SendUnreliableToLocal", U + L, eventName, player, ...)
end
function NetEvents:BroadcastUnreliable(eventName, ...) broadcast("BroadcastUnreliable", U, eventName, ...) end
function NetEvents:BroadcastUnreliableLocal(eventName, ...) broadcast("BroadcastUnreliableLocal", U + L, eventName, ...) end
function NetEvents:SendUnreliableOrderedTo(eventName, player, ...) sendTo("SendUnreliableOrderedTo", U + O, eventName, player, ...) end
function NetEvents:SendUnreliableOrderedToLocal(eventName, player, ...)
  sendTo("SendUnreliableOrderedToLocal", U + O + L, eventName, player, ...)
end
function NetEvents:BroadcastUnreliableOrdered(eventName, ...) broadcast("BroadcastUnreliableOrdered", U + O, eventName, ...) end
function NetEvents:BroadcastUnreliableOrderedLocal(eventName, ...)
  broadcast("BroadcastUnreliableOrderedLocal", U + O + L, eventName, ...)
end

-- Unsubscribe() — снять все; Unsubscribe(eventName) — снять по имени
function NetEvents:Unsubscribe(eventName)
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
    error("NetEvents:Unsubscribe: имя события должно быть строкой", 2)
  end

  local list = subs[eventName]
  if list ~= nil then
    for i = 1, #list do
      list[i].alive = false
    end
    subs[eventName] = nil
  end
end

return NetEvents
)LUA";

/// Писать в лог EVBots (передаётся в Lua первым аргументом чанка).
int l_ne_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: NetEvents: %s", text);
    return 0;
}

}  // namespace

bool RegisterVextNetEvents(lua_State* L) {
    if (luaL_loadbufferx(L, kNetEventsSource,
                         sizeof(kNetEventsSource) - 1, "=[vext-netevents]",
                         "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: NetEvents не собрались: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    // аргумент чанка — функция логирования (после chunk, чтобы стек был
    // [chunk, arg]: функция на дне, аргументы сверху)
    lua_pushcfunction(L, l_ne_log);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        // собрать traceback для точного места ошибки
        luaL_traceback(L, L, lua_tostring(L, -1), 0);
        const char* tb = lua_tostring(L, -1);
        LOG_ERR("EVBots: NetEvents не выполнились: %s",
                tb != nullptr ? tb : "?");
        lua_pop(L, 2);
        return false;
    }
    LOG_INFO("EVBots: NetEvents подключены (сервер; канал к клиентам — ev-core.dll, К4)");
    return true;
}

}  // namespace evbots
