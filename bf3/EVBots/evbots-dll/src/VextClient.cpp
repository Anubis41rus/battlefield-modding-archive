// VextClient.cpp — клиентские библиотеки VeniceEXT для клиентского режима EVBots.dll (в bf3.exe).
//
// По документации VU (НЕ по памяти), content\vext\ref\client\library\:
//   netevents.md, playermanager.md, inputmanager.md, clientutils.md, raycastmanager.md, debugrenderer.md, webui.md;
//   shared\library\resourcemanager.md, entitymanager.md (у VU общие; наши серверные — по адресам сервера, сборка 215).
// Сигнатуры, перегрузки и типы возврата — оттуда.
//
// К3 (сборка 215, 23.10.2026): каркас клиентского режима. Движковых вызовов пока нет:
//   - адреса клиентских функций движка размечены (ext\CLIENT-SLOTS-2026-10-23.md), но не сверены в Ghidra — К5;
//   - канала клиент↔сервер нет — К4 (NetEvents:Send* пишут в лог, входящие придут через NetEvents:_Deliver).
// Поэтому методы — заглушки с безопасным значением ровно документированного типа: где VU допускает nil
// (GetLocalPlayer, Raycast, GetCameraTransform, WorldToScreen …) — nil; где тип обязателен — false / 0 / пустая
// таблица / Vec2(0, 0). Каждая заглушка один раз пишет в лог, что она заглушка, — чтобы по EVBots.log было видно,
// какой API клиенту нужен первым. Мод fun-bots не меняем: он должен работать на этом API как на VU.

#include "VextClient.h"

#include <windows.h>

#include <string>

#include "EVBotsLog.h"
#include "LuaRuntime.h"
#include "VextSettings.h"
#include "VextSpectator.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// Чанк получает первым аргументом функцию логирования (как VextNetEvents).
constexpr char kClientSource[] = R"LUA(
-- Клиентские библиотеки VeniceEXT (VU) — по content\vext\ref\client\library\*.md

local log, modNow, modScope = ...  -- ★ 228: modNow() — папка текущего мода, modScope(папка) -> прежняя (NetEvents)

local function isCallable(v)
  return type(v) == "function" or (type(v) == "table" and getmetatable(v) ~= nil and getmetatable(v).__call ~= nil)
end

-- одна строка в лог на каждый метод-заглушку (не на каждый вызов)
local stubSeen = {}
local function stub(lib, method, step)
  local key = lib .. ":" .. method
  if stubSeen[key] then return end
  stubSeen[key] = true
  log("INFO", "★ клиент: " .. key .. " — пока заглушка (" .. step .. ")")
end

local function vec2zero()
  local V = rawget(_G, "Vec2")
  if V ~= nil then return V(0, 0) end
  return nil
end

-- ═══════════════════════════════════════════════════════════════════════════
-- NetEvents (client) — netevents.md
--   Subscribe(eventName, callback) / Subscribe(eventName, context, callback) -> NetEvent
--   Send / SendLocal / SendUnreliable / SendUnreliableLocal / SendUnreliableOrdered / SendUnreliableOrderedLocal
--     (eventName, ...args) -> void      (клиент шлёт только серверу, получателя нет)
--   Unsubscribe() / Unsubscribe(eventName) -> void
-- ═══════════════════════════════════════════════════════════════════════════
local NetEvents = {}
_G.NetEvents = NetEvents
local subs = {}

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
  -- ★ Сборка 228: мод-владелец подписки — обработчик исполняется «от имени» своего мода, как Events (LuaEvents.cpp
  -- pushScope): без этого WebUI:Init() из обработчика NetEvent (fun-bots: WriteClientSettings) не знает свой ui.vuic
  local entry = { userData = userData, hasUserData = count > 1, callback = callback, alive = true, owner = modNow() }
  local list = subs[eventName]
  if list == nil then
    list = {}
    subs[eventName] = list
  end
  list[#list + 1] = entry
  local e = {}
  function e:Unsubscribe() entry.alive = false end
  return e
end

function NetEvents:Unsubscribe(eventName)
  if eventName == nil then
    for _, list in pairs(subs) do
      for i = 1, #list do list[i].alive = false end
    end
    subs = {}
    return
  end
  if type(eventName) ~= "string" then
    error("NetEvents:Unsubscribe: имя события должно быть строкой", 2)
  end
  local list = subs[eventName]
  if list ~= nil then
    for i = 1, #list do list[i].alive = false end
    subs[eventName] = nil
  end
end

-- ★ Сборка 219 (К4): отправка на сервер через канал ev-core.dll (как Harmony у VU), упаковка — EVNetCodec.
-- До подключения канала события копятся (до 512) и уходят сразу после приветствия.
local up = false
local outq = {}
local sendSeen = {}
-- ★ Сборка 227: диагностика «бот видит игрока» (прогон 226: лучи с попаданиями, а боты не стреляют, пока по ним не
-- выстрелишь). last — на чём кончились лучи мода (имя класса последнего тела), sent — Botmanager:RaycastResults по Mode.
-- Сводка — раз в 10 с из блока «игроки — я».
local rayDiag = { last = {}, empty = 0, sends = 0, emptySends = 0, modes = {} }
local function countRaycastResults(list)
  rayDiag.sends = rayDiag.sends + 1
  if type(list) ~= "table" or #list == 0 then rayDiag.emptySends = rayDiag.emptySends + 1 return end
  for i = 1, #list do
    local m = type(list[i]) == "table" and tostring(list[i].Mode) or "?"
    rayDiag.modes[m] = (rayDiag.modes[m] or 0) + 1
    -- ★ Сборка 229: номер бота у клиента -> имя (сверить с сервером: совпадают ли id игроков, прогон 229 — бот рядом молчит)
    local b = type(list[i]) == "table" and list[i].Bot1 or nil
    rayDiag.ids = rayDiag.ids or {}
    if type(b) == "number" and rayDiag.ids[b] == nil and (rayDiag.idsN or 0) < 12 then
      local p = PlayerManager:GetPlayerById(b)
      rayDiag.ids[b] = true
      rayDiag.idsN = (rayDiag.idsN or 0) + 1
      log("INFO", "★ клиент: RaycastResults Mode " .. m .. ": Bot1 " .. b .. " = " .. tostring(p and p.name))
    end
  end
end
local function logOnce(key, msg)
  if sendSeen[key] then return end
  sendSeen[key] = true
  log("INFO", msg)
end
local function send(kind, flags, eventName, ...)
  if type(eventName) ~= "string" then
    error("NetEvents:" .. kind .. ": имя события должно быть строкой", 3)
  end
  local ok, bytes = pcall(EVNetCodec.EncodeEvent, flags, eventName, ...)
  if not ok then error(bytes, 3) end
  if eventName == "Botmanager:RaycastResults" then countRaycastResults((...)) end
  if up and __evnet_send(0, bytes) then
    logOnce("sent " .. kind .. " " .. eventName, "★ клиент: NetEvents:" .. kind .. "(\"" .. eventName .. "\") отправлено на сервер")
    return
  end
  if not __evnet_available() then
    logOnce("noev " .. eventName, "★ клиент: NetEvents:" .. kind .. "(\"" .. eventName .. "\") — ev-core.dll нет, канала нет; не отправлено")
    return
  end
  if #outq < 512 then
    outq[#outq + 1] = bytes
    logOnce("wait " .. eventName, "★ клиент: NetEvents:" .. kind .. "(\"" .. eventName .. "\") ждёт подключения канала")
  end
end
local L, U, O = 1, 2, 4
function NetEvents:Send(eventName, ...) send("Send", 0, eventName, ...) end
function NetEvents:SendLocal(eventName, ...) send("SendLocal", L, eventName, ...) end
function NetEvents:SendUnreliable(eventName, ...) send("SendUnreliable", U, eventName, ...) end
function NetEvents:SendUnreliableLocal(eventName, ...) send("SendUnreliableLocal", U + L, eventName, ...) end
function NetEvents:SendUnreliableOrdered(eventName, ...) send("SendUnreliableOrdered", U + O, eventName, ...) end
function NetEvents:SendUnreliableOrderedLocal(eventName, ...) send("SendUnreliableOrderedLocal", U + O + L, eventName, ...) end

-- ★ Сборка 224: имена ботов от сервера (служебное «__ev_bots», моду не доставляется). Для них PlayerManager клиента
-- отдаёт onlineId = 0, как на VU (в движке у нашего бота свой onlineId ради статистики — реестр В15).
local evBots = {}

-- Приём из канала (EvNetBridge на кадре движка): 1 — подключились (приветствие + отложенное), 2 — кадр, 3 — обрыв.
function NetEvents._OnNet(conn, kind, data)
  if kind == 1 then
    up = true
    local name = __evnet_playername() or ""
    __evnet_send(0, EVNetCodec.EncodeHello(name))
    log("INFO", "★ клиент: канал к серверу открыт (" .. tostring(data) .. "), игрок «" .. name .. "», отложено событий: " .. #outq)
    local q = outq
    outq = {}
    for i = 1, #q do __evnet_send(0, q[i]) end
    return
  elseif kind == 3 then
    up = false
    log("INFO", "★ клиент: канал к серверу закрыт — переподключение")
    return
  end
  local ok, k, flags, name, n, args = pcall(EVNetCodec.Decode, data)
  if not ok then
    logOnce("bad", "★ клиент: кадр с сервера не разобран: " .. tostring(k))
    return
  end
  if k == EVNetCodec.KIND_EVENT and name == "__ev_bots" then
    local set, cnt = {}, 0
    local list = args[1]
    if type(list) == "table" then
      for i = 1, #list do if type(list[i]) == "string" then set[list[i]] = true; cnt = cnt + 1 end end
    end
    evBots = set
    log("INFO", "★ клиент: список ботов от сервера — " .. cnt)
    local ids, info = rawget(_G, "__clp_ids"), rawget(_G, "__clp_info")
    if ids ~= nil and info ~= nil then
      local okI, list2 = pcall(ids)
      if okI and type(list2) == "table" then
        for j = 1, math.min(#list2, 4) do
          local inf = info(list2[j])
          if inf ~= nil then
            logOnce("oid " .. tostring(inf.name), "★ клиент: игрок «" .. tostring(inf.name) .. "» onlineId движка " ..
              tostring(inf.onlineId) .. ", бот по списку: " .. tostring(set[inf.name] == true))
          end
        end
      end
    end
    return
  end
  if k == EVNetCodec.KIND_EVENT then
    logOnce("recv " .. tostring(name), "★ клиент: NetEvent «" .. tostring(name) .. "» с сервера получен")
    local okd, err = pcall(NetEvents._Deliver, NetEvents, name, table.unpack(args, 1, n))
    if not okd then log("ERR ", "клиент: обработчик NetEvent «" .. tostring(name) .. "» упал: " .. tostring(err)) end
  end
end
function NetEvents._Pump() end

-- Входящее событие с сервера (для канала К4): у клиента обработчик получает только аргументы, без игрока.
function NetEvents:_Deliver(eventName, ...)
  local list = subs[eventName]
  if list == nil then return 0 end
  local copy = {}
  for i = 1, #list do copy[i] = list[i] end
  local n = 0
  for i = 1, #copy do
    local entry = copy[i]
    if entry.alive then
      n = n + 1
      local prev = modScope(entry.owner or "")
      local ok, err
      if entry.hasUserData then ok, err = pcall(entry.callback, entry.userData, ...) else ok, err = pcall(entry.callback, ...) end
      modScope(prev)
      if not ok then error(err, 0) end
    end
  end
  return n
end

-- ═══════════════════════════════════════════════════════════════════════════
-- PlayerManager (client) — playermanager.md
-- ═══════════════════════════════════════════════════════════════════════════
local PlayerManager = {}
_G.PlayerManager = PlayerManager
local PM_STEP = "К5: игроки клиента из движка"
-- ★ Сборка 222 (К5 шаг 4): игроки из движка клиента, как у VU — Player = ручка по id (один объект на id, поэтому
-- p == PlayerManager:GetLocalPlayer() работает), поля читаются заново при каждом доступе (__clp_info). Нет движка
-- (тест-хост) — __clp_* не зарегистрированы, остаются заглушки.
local live = rawget(_G, "__clp_info") ~= nil
local playerObjs = {}
-- ★ 238-5: кэш сущностей по виду и адресу без склейки строки-ключа на каждое p.soldier; значения слабые — объект живёт,
-- пока мод его держит (сравнение p.soldier == сохранённому работает), остальное уходит сборщику
local entObjs = { soldier = setmetatable({}, { __mode = "v" }), corpse = setmetatable({}, { __mode = "v" }),
                  vehicle = setmetatable({}, { __mode = "v" }) }

-- ★ 238-5: без промежуточной таблицы { ... } на каждый worldTransform
local function ltFrom(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12)
  if a12 == nil then return nil end
  return LinearTransform(Vec3(a1, a2, a3), Vec3(a4, a5, a6), Vec3(a7, a8, a9), Vec3(a10, a11, a12))
end

-- сущность движка (солдат, труп, техника): worldTransform/transform — живые; один объект на адрес
local function entityObj(ptr, kind)
  if ptr == nil or ptr == 0 then return nil end
  local byPtr = entObjs[kind]
  local e = byPtr[ptr]
  -- Soldier state and team overrides belong to the real native entity. The
  -- generation-checked wrapper also matches SpatialRaycast's entity identity.
  if (kind == "soldier" or kind == "corpse") and rawget(_G, "__cls_wrap") ~= nil then
    if e ~= nil and e.isValid then return e end
    e = __cls_wrap(ptr)
    byPtr[ptr] = e
    return e
  end
  if e ~= nil then return e end
  e = setmetatable({}, { __index = function(_, k)
    if k == "worldTransform" or k == "transform" then return ltFrom(__clp_transform(ptr)) end
    if k == "isDead" then return kind == "corpse" end
    if k == "isAlive" or k == "alive" then return kind == "soldier" end
    if k == "typeInfo" then return { name = kind == "vehicle" and "ClientVehicleEntity" or "ClientSoldierEntity" } end
    return nil
  end, __tostring = function() return kind .. "(" .. string.format("%08X", ptr) .. ")" end })
  byPtr[ptr] = e
  return e
end

-- ★ 238-5: поле — одним вызовом __clp_field (раньше __clp_info строил таблицу всех полей на каждое обращение: мусор и
-- время кадра, рывки на 30/60 Гц). Игрок ушёл — nil на любое поле, как раньше.
local PlayerMt = { __index = function(t, k)
  local id = rawget(t, "_id")
  if k == "id" then return id end
  if type(k) ~= "string" then return nil end
  if k == "soldier" then return entityObj(__clp_field(id, "soldier"), "soldier") end
  if k == "corpse" then return entityObj(__clp_field(id, "corpse"), "corpse") end
  if k == "controlledControllable" then return entityObj(__clp_field(id, "controlled"), "vehicle") end
  if k == "attachedControllable" then return entityObj(__clp_field(id, "attached"), "vehicle") end
  if k == "onlineId" then
    local nm = __clp_field(id, "name")
    if nm == nil then return nil end
    if evBots[nm] then return 0 end
  end
  return __clp_field(id, k)
end, __tostring = function(t) return "Player(" .. tostring(rawget(t, "_id")) .. ")" end }

local function playerObj(id)
  if id == nil then return nil end
  local p = playerObjs[id]
  if p == nil then
    p = setmetatable({ _id = id }, PlayerMt)
    playerObjs[id] = p
  end
  return p
end

local function allPlayers()
  local out = {}
  if not live then return out end
  local ids = __clp_ids()
  for i = 1, #ids do out[#out + 1] = playerObj(ids[i]) end
  return out
end

local function filter(pred)
  local out = {}
  for _, p in ipairs(allPlayers()) do if pred(p) then out[#out + 1] = p end end
  return out
end

if live then
  function PlayerManager:GetPlayers() return allPlayers() end
  function PlayerManager:GetPlayersByTeam(team) return filter(function(p) return p.teamId == team end) end
  function PlayerManager:GetPlayersBySquad(team, squad)
    return filter(function(p) return p.teamId == team and p.squadId == squad end)
  end
  function PlayerManager:GetPlayerById(id)
    if type(id) ~= "number" or __clp_field(id, "id") == nil then return nil end  -- 238-5: без таблицы всех полей
    return playerObj(id)
  end
  function PlayerManager:GetPlayerByOnlineId(onlineId)
    for _, p in ipairs(allPlayers()) do if p.onlineId == onlineId then return p end end
    return nil
  end
  function PlayerManager:GetPlayerByName(name)
    for _, p in ipairs(allPlayers()) do if p.name == name then return p end end
    return nil
  end
  function PlayerManager:GetPlayersByName(name) return filter(function(p) return p.name == name end) end
  function PlayerManager:GetPlayerCount() return #__clp_ids() end
  function PlayerManager:GetLocalPlayer() return playerObj(__clp_local()) end
  function PlayerManager:GetSpectatorCount() stub("PlayerManager", "GetSpectatorCount", PM_STEP) return 0 end
  function PlayerManager:GetSquadLeader(team, squad)
    for _, p in ipairs(allPlayers()) do
      if p.teamId == team and p.squadId == squad and p.isSquadLeader then return p end
    end
    return nil
  end
else
  function PlayerManager:GetPlayers() stub("PlayerManager", "GetPlayers", PM_STEP) return {} end
  function PlayerManager:GetPlayersByTeam(team) stub("PlayerManager", "GetPlayersByTeam", PM_STEP) return {} end
  function PlayerManager:GetPlayersBySquad(team, squad) stub("PlayerManager", "GetPlayersBySquad", PM_STEP) return {} end
  function PlayerManager:GetPlayerById(id) stub("PlayerManager", "GetPlayerById", PM_STEP) return nil end
  function PlayerManager:GetPlayerByOnlineId(onlineId) stub("PlayerManager", "GetPlayerByOnlineId", PM_STEP) return nil end
  function PlayerManager:GetPlayerByName(name) stub("PlayerManager", "GetPlayerByName", PM_STEP) return nil end
  function PlayerManager:GetPlayersByName(name) stub("PlayerManager", "GetPlayersByName", PM_STEP) return {} end
  function PlayerManager:GetPlayerCount() stub("PlayerManager", "GetPlayerCount", PM_STEP) return 0 end
  function PlayerManager:GetLocalPlayer() stub("PlayerManager", "GetLocalPlayer", PM_STEP) return nil end
  function PlayerManager:GetSpectatorCount() stub("PlayerManager", "GetSpectatorCount", PM_STEP) return 0 end
  function PlayerManager:GetSquadLeader(team, squad) stub("PlayerManager", "GetSquadLeader", PM_STEP) return nil end
end

-- ★ Сборка 222: раз в 10 с — локальный игрок и позиция его солдата в журнал клиента (сверить с сервером; гипотеза о
-- трансформе сущности клиента). Зовёт хук кадра через Engine:Update.
if live then
  local acc = 0
  Events:Subscribe("Engine:Update", function(dt)
    acc = acc + (dt or 0)
    if acc < 10 then return end
    acc = 0
    local me = PlayerManager:GetLocalPlayer()
    if me == nil then log("INFO", "★ клиент: игроки — локального нет, всего " .. #__clp_ids()) return end
    local s = me.soldier
    local t = s and s.worldTransform
    log("INFO", string.format("★ клиент: игроки — я %s (id %s, команда %s, alive %s, в технике %s), всего %d; солдат %s",
      tostring(me.name), tostring(me.id), tostring(me.teamId), tostring(me.alive), tostring(me.inVehicle), #__clp_ids(),
      t and string.format("(%.1f, %.1f, %.1f)", t.trans.x, t.trans.y, t.trans.z) or "нет"))
    -- ★ Сборка 226: контрольный луч от солдата (+1 м) вниз на 50 м, DontCheckWater — земля должна попасться;
    -- нет — наш вызов 01199D10 не работает в принципе, есть — дело в лучах мода (MODLOG 226).
    if t and rawget(_G, "__clr_collision") ~= nil then
      local p = t.trans
      local raw = __clr_collision(p.x, p.y + 1.0, p.z, p.x, p.y - 50.0, p.z, 5, 0, 4, true)
      local parts = {}
      for i = 1, #raw do
        local h = raw[i]
        parts[#parts + 1] = string.format("y %.2f %s", h.y, table.concat(__clr_types(h.body), " < "))
      end
      log("INFO", "★ клиент: контрольный луч вниз: попаданий " .. #raw .. (#parts > 0 and (" — " .. table.concat(parts, "; ")) or ""))
    end
    -- ★ Сборка 227: сводка за 10 с — на чём кончаются лучи мода и что ушло в Botmanager:RaycastResults (по Mode).
    local function kv(t)
      local o = {}
      for k, v in pairs(t) do o[#o + 1] = k .. "=" .. v end
      table.sort(o)
      return #o > 0 and table.concat(o, ", ") or "—"
    end
    log("INFO", string.format("★ клиент: лучи мода за 10 с: пусто %d, последнее тело: %s | RaycastResults отправок %d (пустых %d), записей по Mode: %s",
      rayDiag.empty, kv(rayDiag.last), rayDiag.sends, rayDiag.emptySends, kv(rayDiag.modes)))
    rayDiag.last, rayDiag.empty, rayDiag.sends, rayDiag.emptySends, rayDiag.modes = {}, 0, 0, 0, {}
    -- ★ Сборка 227: пробный луч в ближайшего врага-бота (до 80 м) теми же флагами, что у мода (ClientBotManager.lua:135–149):
    -- от камеры к солдату бота +1,5 м. Видно ли тело бота (CharacterPhysicsEntity) нашему лучу — гипотеза, проверка здесь.
    local cam = t and ClientUtils:GetCameraTransform()
    if cam ~= nil and rawget(_G, "__clr_collision") ~= nil then
      local best, bestD, bestT = nil, 80.0, nil
      for _, p in ipairs(allPlayers()) do
        if evBots[p.name] and p.teamId ~= me.teamId then
          local bs = p.soldier
          local bt = bs and bs.worldTransform
          if bt ~= nil then
            local d = cam.trans:Distance(bt.trans)
            if d < bestD then best, bestD, bestT = p, d, bt end
          end
        end
      end
      if best ~= nil then
        local a, b = cam.trans, bestT.trans
        local raw = __clr_collision(a.x, a.y, a.z, b.x, b.y + 1.5, b.z, 5, 0xB000000, 4, true)
        local parts = {}
        for i = 1, #raw do
          local h = raw[i]
          parts[#parts + 1] = string.format("(%.1f %.1f %.1f) %s", h.x, h.y, h.z, __clr_types(h.body)[1] or "?")
        end
        log("INFO", string.format("★ клиент: пробный луч в бота %s (%.1f м, в технике %s): попаданий %d — %s",
          tostring(best.name), bestD, tostring(best.inVehicle), #raw, #parts > 0 and table.concat(parts, "; ") or "нет"))
      end
    end
  end)
end

-- ═══════════════════════════════════════════════════════════════════════════
-- InputManager — inputmanager.md
-- ═══════════════════════════════════════════════════════════════════════════
local InputManager = {}
_G.InputManager = InputManager
local IM_STEP = "К6: ввод клиента"
function InputManager:GetLevel(concept) stub("InputManager", "GetLevel", IM_STEP) return 0.0 end
function InputManager:IsDown(concept) stub("InputManager", "IsDown", IM_STEP) return false end
function InputManager:WentDown(concept) stub("InputManager", "WentDown", IM_STEP) return false end
function InputManager:WentUp(concept) stub("InputManager", "WentUp", IM_STEP) return false end
function InputManager:IsKeyDown(key) stub("InputManager", "IsKeyDown", IM_STEP) return false end
function InputManager:WentKeyDown(key) stub("InputManager", "WentKeyDown", IM_STEP) return false end
function InputManager:WentKeyUp(key) stub("InputManager", "WentKeyUp", IM_STEP) return false end
function InputManager:IsMouseButtonDown(button) stub("InputManager", "IsMouseButtonDown", IM_STEP) return false end
function InputManager:WentMouseButtonDown(button) stub("InputManager", "WentMouseButtonDown", IM_STEP) return false end
function InputManager:WentMouseButtonUp(button) stub("InputManager", "WentMouseButtonUp", IM_STEP) return false end
function InputManager:GetMouseLevel(axes) stub("InputManager", "GetMouseLevel", IM_STEP) return 0.0 end
function InputManager:GetCursorPosition() stub("InputManager", "GetCursorPosition", IM_STEP) return vec2zero() end
function InputManager:SetCursorPosition(x, y) stub("InputManager", "SetCursorPosition", IM_STEP) end
function InputManager:IsMouseActive() stub("InputManager", "IsMouseActive", IM_STEP) return false end
function InputManager:GetMouseSensitivity() stub("InputManager", "GetMouseSensitivity", IM_STEP) return 0.0 end
function InputManager:SetMouseSensitivity(sensitivity) stub("InputManager", "SetMouseSensitivity", IM_STEP) end
-- ★ Сборка 228: клавиши и кнопки мыши кадра (VextClientInput.cpp: опрос раз в кадр, до Client:UpdateInput; видны, только
-- когда окно игры в фокусе — открытое меню WebUI забирает ввод, как у VU). Концепты (GetLevel/WentDown…) — заглушки (В17).
if rawget(_G, "__im_key") ~= nil then
  local function code(k) return type(k) == "number" and math.tointeger(k) or -1 end
  function InputManager:IsKeyDown(key) local d = __im_key(code(key)) return d end
  function InputManager:WentKeyDown(key) local _, w = __im_key(code(key)) return w end
  function InputManager:WentKeyUp(key) local _, _, u = __im_key(code(key)) return u end
  function InputManager:IsMouseButtonDown(b) local d = __im_mouse(code(b)) return d end
  function InputManager:WentMouseButtonDown(b) local _, w = __im_mouse(code(b)) return w end
  function InputManager:WentMouseButtonUp(b) local _, _, u = __im_mouse(code(b)) return u end
  function InputManager:GetCursorPosition() local x, y = __im_cursor() return Vec2(x, y) end
  function InputManager:SetCursorPosition(x, y) __im_set_cursor(x, y) end
end
-- ★ Сборка 234 (В17): концепты — менеджер понятий ввода движка (VextClientInput.cpp). nil от __im_concept — движка нет
-- (тест-хост): тогда прежняя заглушка.
if rawget(_G, "__im_concept") ~= nil then
  local function cid(c) return type(c) == "number" and math.tointeger(c) or -1 end
  local prev = { GetLevel = InputManager.GetLevel, IsDown = InputManager.IsDown, WentDown = InputManager.WentDown,
                 WentUp = InputManager.WentUp }
  for kind, m in ipairs({ "GetLevel", "IsDown", "WentDown", "WentUp" }) do
    InputManager[m] = function(self, concept)
      local v = __im_concept(kind - 1, cid(concept))
      if v == nil then return prev[m](self, concept) end
      return v
    end
  end
end

-- ═══════════════════════════════════════════════════════════════════════════
-- ClientUtils — clientutils.md
-- ═══════════════════════════════════════════════════════════════════════════
-- В локальном EVBots голосового транспорта пока нет. Это capability отказ:
-- моду возвращается документированное отсутствие VoIP, каналы не создаются.
local Voip = {}
_G.Voip = Voip
function Voip:IsAvailable() return false end
function Voip:GetChannel(name)
  if type(name) ~= 'string' then error('Voip:GetChannel: expected channel name', 2) end
  return nil
end
function Voip:GetChannels() return {} end
function Voip:GetEmitters() return {} end
log('INFO', 'EVBots: VoIP transport unavailable in local runtime')

local ClientUtils = {}
_G.ClientUtils = ClientUtils
local CU_STEP = "К5: камера и экран"
-- ★ Сборка 222: у VU — трансформ «вида» камеры (объект от vu-core, у нас ещё не найден, CLIENT-SLOTS «Открыто»).
-- Пока — глаза локального солдата (позиция + 1,6 м вверх по оси up), как запасной путь в коде самого fun-bots
-- (ClientBotManager.lua:310: soldier.worldTransform.trans + getCameraPos). Нет солдата — nil, как у VU без вида.
-- Реестр В4.
function ClientUtils:GetCameraTransform()
  local me = PlayerManager:GetLocalPlayer()
  local s = me and me.soldier
  local t = s and s.worldTransform
  if t == nil then stub("ClientUtils", "GetCameraTransform", CU_STEP) return nil end
  return LinearTransform(t.left, t.up, t.forward, t.trans + t.up * 1.6)
end
function ClientUtils:WorldToScreen(worldPos) stub("ClientUtils", "WorldToScreen", CU_STEP) return nil end
function ClientUtils:ScreenToWorld(mousePos) stub("ClientUtils", "ScreenToWorld", CU_STEP) return nil end
function ClientUtils:GetWindowSize() stub("ClientUtils", "GetWindowSize", CU_STEP) return nil end
-- ★ Сборка 234 (В4): вид камеры VU — рендер игры (VextClientRaycast.cpp, __cu_view: камера 1..16, view·proj 17..32).
-- GetCameraTransform — трансформ камеры вида (VU 10ADB530); WorldToScreen — формула VU 10ADC020: clip = (x,y,z,1)·VP,
-- w > 1e-6, экран = ((x/w + 1)·0,5·ширина, (1 − y/w)·0,5·высота). Размер окна — клиентская область окна игры
-- (__webui_size; у VU WorldToScreen берёт ширину/высоту из окна vu-core [окно+0x20/+0x24] (10ADC1C9), GetWindowSize —
-- GetWindowInfo HWND того же окна (10ADB660)).
-- Plane/showroom/spectator cameras can be arbitrarily far from the soldier.
-- Preserve the engine view instead of replacing it with the soldier's eyes.
if rawget(_G, "__cu_view") ~= nil then
  local function view()
    return __cu_view()
  end
  function ClientUtils:GetCameraTransform()
    local v = view()
    if v == nil then return nil end
    return LinearTransform(Vec3(v[1], v[2], v[3]), Vec3(v[5], v[6], v[7]), Vec3(v[9], v[10], v[11]),
                           Vec3(v[13], v[14], v[15]))
  end
  function ClientUtils:GetWindowSize()
    local w, h = 0, 0
    if rawget(_G, "__webui_size") ~= nil then w, h = __webui_size() end
    if w <= 0 or h <= 0 then return nil end
    return Vec2(w, h)
  end
  function ClientUtils:WorldToScreen(p)
    if p == nil then return nil end
    local v = view()
    if v == nil then stub("ClientUtils", "WorldToScreen", CU_STEP) return nil end
    local size = self:GetWindowSize()
    if size == nil then return nil end
    local x, y, z = p.x, p.y, p.z
    local w = x * v[20] + y * v[24] + z * v[28] + v[32]
    if not (w > 1e-6) then return nil end
    local inv = 1.0 / w
    local cx = x * v[17] + y * v[21] + z * v[25] + v[29]
    local cy = x * v[18] + y * v[22] + z * v[26] + v[30]
    return Vec2((cx * inv + 1.0) * 0.5 * size.x, (1.0 - cy * inv) * 0.5 * size.y)
  end
end
function ClientUtils:PopScreen(graphAsset, screensToPop) stub("ClientUtils", "PopScreen", "К7: интерфейс") return false end
function ClientUtils:ConnectToServer(serverGuid, password) stub("ClientUtils", "ConnectToServer", "вне плана") end
function ClientUtils:SpectateServer(serverGuid, password) stub("ClientUtils", "SpectateServer", "вне плана") end
function ClientUtils:SetSquadAutoJoining(shouldJoinSquad) stub("ClientUtils", "SetSquadAutoJoining", "вне плана") end

-- ═══════════════════════════════════════════════════════════════════════════
-- RaycastManager (client) — raycastmanager.md
-- ═══════════════════════════════════════════════════════════════════════════
local RaycastManager = {}
_G.RaycastManager = RaycastManager
local RM_STEP = "К5: лучи клиента"
function RaycastManager:GetEntityInRaycast(distance) stub("RaycastManager", "GetEntityInRaycast", RM_STEP) return nil end
function RaycastManager:Raycast(from, to, flags) stub("RaycastManager", "Raycast", RM_STEP) return nil end
function RaycastManager:SpatialRaycast(from, to, flags) stub("RaycastManager", "SpatialRaycast", RM_STEP) return {} end
function RaycastManager:DetailedRaycast(from, to, maxHits, materialFlags, flags)
  stub("RaycastManager", "DetailedRaycast", RM_STEP) return {}
end
-- ★ Сборка 223 (К5 шаг 4): CollisionRaycast из движка клиента, как у VU (01199D10, режим 1; VextClientRaycast.cpp).
-- RayCastHit: position, normal (Vec3), rigidBody — сущность физики с Is(имя)/typeInfo по рефлексии Frostbite,
-- material — пока nil (мод его не читает). Нет движка (тест-хост) — пустой массив, как раньше.
local bodyObjs = {}
local function bodyObj(ptr)
  if ptr == nil or ptr == 0 then return nil end
  local b = bodyObjs[ptr]
  if b ~= nil then return b end
  b = setmetatable({}, { __index = function(_, k)
    if k == "Is" then
      return function(_, name)
        local t = __clr_types(ptr)
        for i = 1, #t do if t[i] == name then return true end end
        return false
      end
    end
    if k == "typeInfo" then local t = __clr_types(ptr) return { name = t[1] or "?" } end
    return nil
  end, __tostring = function() return "RigidBody(" .. string.format("%08X", ptr) .. ")" end })
  bodyObjs[ptr] = b
  return b
end
local rayLogged = false
function RaycastManager:CollisionRaycast(from, to, maxHits, materialFlags, flags)
  if rawget(_G, "__clr_collision") == nil or from == nil or to == nil then
    stub("RaycastManager", "CollisionRaycast", RM_STEP) return {}
  end
  local raw = __clr_collision(from.x, from.y, from.z, to.x, to.y, to.z, maxHits or 1, materialFlags or 0, flags or 0)
  local out = {}
  for i = 1, #raw do
    local h = raw[i]
    out[i] = { position = Vec3(h.x, h.y, h.z), normal = Vec3(h.nx, h.ny, h.nz), rigidBody = bodyObj(h.body), material = nil }
  end
  if #raw == 0 then
    rayDiag.empty = rayDiag.empty + 1
  else
    local k = __clr_types(raw[#raw].body)[1] or "?"
    rayDiag.last[k] = (rayDiag.last[k] or 0) + 1
  end
  if not rayLogged and #out > 0 then
    rayLogged = true
    local t = out[#out].rigidBody and __clr_types(raw[#raw].body) or {}
    log("INFO", "★ клиент: первый CollisionRaycast из движка: попаданий " .. #out .. ", последнее — " .. table.concat(t, " < "))
  end
  return out
end

-- ═══════════════════════════════════════════════════════════════════════════
-- DebugRenderer — debugrenderer.md (все методы void)
-- ═══════════════════════════════════════════════════════════════════════════
local DebugRenderer = {}
_G.DebugRenderer = DebugRenderer
local DR_STEP = "К6: отладочная отрисовка"
for _, m in ipairs({ "DrawLine", "DrawTriangle", "DrawSphere", "DrawLine2D", "DrawText2D", "DrawVertices", "DrawOBB" }) do
  DebugRenderer[m] = function() stub("DebugRenderer", m, DR_STEP) end
end
-- ★ Сборка 234 (В6): отладочный рендер движка, как у VU (VextClientRaycast.cpp: __dr_*). Нет рендера — false, тогда
-- заглушка (журнал раз). DrawOBB — 12 линий DrawLine (как устроен DrawOBB у VU 10DB0540 — не разобрано);
-- DrawVertices — заглушка (мод не зовёт).
if rawget(_G, "__dr_text") ~= nil then
  local function c4(c) if c == nil then return 1, 1, 1, 1 end return c.x, c.y, c.z, c.w end
  function DebugRenderer:DrawText2D(x, y, text, color, scale)
    local r, g, b, a = c4(color)
    if not __dr_text(x or 0, y or 0, tostring(text or ""), r, g, b, a, scale or 1.0) then
      stub("DebugRenderer", "DrawText2D", DR_STEP)
    end
  end
  function DebugRenderer:DrawSphere(pos, radius, color, renderLines, smallSizeSegmentDecrease)
    local r, g, b, a = c4(color)
    if not __dr_sphere(pos.x, pos.y, pos.z, radius or 1.0, r, g, b, a, renderLines == true, smallSizeSegmentDecrease == true) then
      stub("DebugRenderer", "DrawSphere", DR_STEP)
    end
  end
  function DebugRenderer:DrawLine2D(from, to, color)
    local r, g, b, a = c4(color)
    if not __dr_line2d(from.x, from.y, to.x, to.y, r, g, b, a) then stub("DebugRenderer", "DrawLine2D", DR_STEP) end
  end
  function DebugRenderer:DrawLine(from, to, colorFrom, colorTo)
    local r1, g1, b1, a1 = c4(colorFrom)
    local r2, g2, b2, a2 = c4(colorTo or colorFrom)
    if not __dr_verts(1, from.x, from.y, from.z, r1, g1, b1, a1, to.x, to.y, to.z, r2, g2, b2, a2) then
      stub("DebugRenderer", "DrawLine", DR_STEP)
    end
  end
  function DebugRenderer:DrawTriangle(p1, p2, p3, c1, c2, c3)
    local r1, g1, b1, a1 = c4(c1)
    local r2, g2, b2, a2 = c4(c2 or c1)
    local r3, g3, b3, a3 = c4(c3 or c1)
    if not __dr_verts(0, p1.x, p1.y, p1.z, r1, g1, b1, a1, p2.x, p2.y, p2.z, r2, g2, b2, a2,
                      p3.x, p3.y, p3.z, r3, g3, b3, a3) then
      stub("DebugRenderer", "DrawTriangle", DR_STEP)
    end
  end
  function DebugRenderer:DrawOBB(aabb, transform, color)
    if aabb == nil or transform == nil then return end
    local mn, mx = aabb.min, aabb.max
    local function pt(x, y, z)
      return transform.trans + transform.left * x + transform.up * y + transform.forward * z
    end
    local c = {}
    for i = 0, 7 do
      c[i] = pt((i & 1) == 0 and mn.x or mx.x, (i & 2) == 0 and mn.y or mx.y, (i & 4) == 0 and mn.z or mx.z)
    end
    for _, e in ipairs({ {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} }) do
      self:DrawLine(c[e[1]], c[e[2]], color, color)
    end
  end
end

-- ═══════════════════════════════════════════════════════════════════════════
-- WebUI — webui.md
-- ═══════════════════════════════════════════════════════════════════════════
local WebUI = {}
_G.WebUI = WebUI
local WU_STEP = "К7: WebUI"
for _, m in ipairs({ "Init", "Show", "Hide", "BringToFront", "SendToBack", "EnableKeyboard", "EnableMouse",
                     "DisableKeyboard", "DisableMouse", "ResetKeyboard", "ResetMouse", "ExecuteJS" }) do
  WebUI[m] = function() stub("WebUI", m, WU_STEP) end
end
function WebUI:GetScreenWidth() stub("WebUI", "GetScreenWidth", WU_STEP) return 0 end
function WebUI:GetScreenHeight() stub("WebUI", "GetScreenHeight", WU_STEP) return 0 end
-- QuoteString(string) -> string: строка в кавычках для вставки в JS (не заглушка — JSON-строка подходит для JS).
function WebUI:QuoteString(s)
  return json.encode(tostring(s))
end
-- ★ Сборка 228: WebUI как у VU (webui.md, vext\guides\webui.md) — фрейм текущего мода в оверлее WebView2 (VextWebUi.cpp).
-- Команды идут в поток WebUI очередью; страница грузится из ui.vuic мода; ExecuteJS до загрузки исполняется после неё.
if rawget(_G, "__webui_init") ~= nil then
  function WebUI:Init()
    if not __webui_init() then log("WARN", "WebUI:Init — WebUI не запущен (нет окна игры или EVBOTS_WEBUI=0)") end
  end
  for _, m in ipairs({ "Show", "Hide", "BringToFront", "SendToBack", "EnableKeyboard", "EnableMouse",
                       "DisableKeyboard", "DisableMouse", "ResetKeyboard", "ResetMouse" }) do
    WebUI[m] = function() __webui_cmd(m) end
  end
  function WebUI:ExecuteJS(script)
    if type(script) ~= "string" then error("WebUI:ExecuteJS: нужна строка", 2) end
    __webui_cmd("ExecuteJS", script)
  end
  function WebUI:GetScreenWidth() local w = __webui_size() return w end
  function WebUI:GetScreenHeight() local _, h = __webui_size() return h end
  -- вызов страницы WebUI.Call('DispatchEvent'|'DispatchEventLocal', имя, данные) -> событие Lua (одно значение данных,
  -- как у VU). Зовёт кадр движка (DrainWebUiCalls), «текущий мод» — мод страницы.
  function _G.__webui_on_call(text)
    local ok, args = pcall(json.decode, text)
    if not ok or type(args) ~= "table" or type(args[2]) ~= "string" then
      log("WARN", "WebUI: непонятный вызов страницы: " .. tostring(text))
      return
    end
    if args[1] == "DispatchEventLocal" then
      Events:DispatchLocal(args[2], args[3])
    else
      Events:Dispatch(args[2], args[3])
    end
  end
end

-- ═══════════════════════════════════════════════════════════════════════════
-- ResourceManager — shared\library\resourcemanager.md. У VU библиотека общая. ★ Сборка 234 (В8): в настоящем bf3.exe
-- ResourceManager уже поднят серверной библиотекой (VextResourceManager.cpp) по адресам zbf3 — тогда заглушки ниже
-- не ставим. Заглушки остаются для тест-хоста (без движка).
-- ═══════════════════════════════════════════════════════════════════════════
if rawget(_G, "ResourceManager") == nil then
local ResourceManager = {}
_G.ResourceManager = ResourceManager
local RES_STEP = "К5: ресурсы клиента"
ResourceManager._handlers = {}

local function containerCallback(entry)
  local cb = {}
  function cb:Deregister() entry.alive = false end
  return cb
end

-- (partitionGuid, instanceGuid | nil, [context,] callback) -> ContainerCallback
local function registerHandler(method, once, partitionGuid, instanceGuid, ...)
  stub("ResourceManager", method, RES_STEP)
  local count = select("#", ...)
  local context, callback
  if count >= 2 then context, callback = ... else callback = (select(1, ...)) end
  if not isCallable(callback) then
    error("ResourceManager:" .. method .. ": обработчик должен быть функцией", 3)
  end
  local entry = { partition = partitionGuid, instance = instanceGuid, context = context, hasContext = count >= 2,
                  callback = callback, once = once, alive = true }
  local h = ResourceManager._handlers
  h[#h + 1] = entry
  return containerCallback(entry)
end

function ResourceManager:RegisterInstanceLoadHandler(partitionGuid, instanceGuid, ...)
  return registerHandler("RegisterInstanceLoadHandler", false, partitionGuid, instanceGuid, ...)
end
function ResourceManager:RegisterInstanceLoadHandlerOnce(partitionGuid, instanceGuid, ...)
  return registerHandler("RegisterInstanceLoadHandlerOnce", true, partitionGuid, instanceGuid, ...)
end
function ResourceManager:RegisterPartitionLoadHandler(partitionGuid, ...)
  return registerHandler("RegisterPartitionLoadHandler", false, partitionGuid, nil, ...)
end
function ResourceManager:RegisterPartitionLoadHandlerOnce(partitionGuid, ...)
  return registerHandler("RegisterPartitionLoadHandlerOnce", true, partitionGuid, nil, ...)
end

function ResourceManager:MountSuperBundle(superbundle, mediaHint, optional) stub("ResourceManager", "MountSuperBundle", RES_STEP) end
function ResourceManager:UnmountSuperBundle(superbundle) stub("ResourceManager", "UnmountSuperBundle", RES_STEP) end
function ResourceManager:GetSettings(settingsType) stub("ResourceManager", "GetSettings", RES_STEP) return nil end
function ResourceManager:LookupDataContainer(compartment, name) stub("ResourceManager", "LookupDataContainer", RES_STEP) return nil end
function ResourceManager:SearchForDataContainer(name) stub("ResourceManager", "SearchForDataContainer", RES_STEP) return nil end
function ResourceManager:FindInstanceByGuid(partitionGuid, instanceGuid)
  stub("ResourceManager", "FindInstanceByGuid", RES_STEP) return nil
end
function ResourceManager:SearchForInstanceByGuid(instanceGuid) stub("ResourceManager", "SearchForInstanceByGuid", RES_STEP) return nil end
function ResourceManager:FindDatabasePartition(partitionGuid) stub("ResourceManager", "FindDatabasePartition", RES_STEP) return nil end
function ResourceManager:FindPartitionForInstance(instance) stub("ResourceManager", "FindPartitionForInstance", RES_STEP) return nil end
function ResourceManager:AddRegistry(registry, compartment) stub("ResourceManager", "AddRegistry", RES_STEP) end
-- Операции с бандлами: ничего не грузится. Handle 0; PollBundleOperation = true («операция закончена», чтобы цикл
-- ожидания мода не зависал), EndLoadData = false («не загружено»).
function ResourceManager:BeginLoadData(compartment, bundles) stub("ResourceManager", "BeginLoadData", RES_STEP) return 0 end
function ResourceManager:EndLoadData(handle) stub("ResourceManager", "EndLoadData", RES_STEP) return false end
function ResourceManager:PollBundleOperation(handle) stub("ResourceManager", "PollBundleOperation", RES_STEP) return true end
function ResourceManager:CancelBundleOperation(handle) stub("ResourceManager", "CancelBundleOperation", RES_STEP) end
function ResourceManager:BeginClear(compartment) stub("ResourceManager", "BeginClear", RES_STEP) return 0 end
function ResourceManager:EndClear(handle) stub("ResourceManager", "EndClear", RES_STEP) end
-- Тип возврата обязателен (ResourceCompartment); отсек не создаётся — отдаём родителя, без выдуманного номера.
function ResourceManager:AllocateDynamicCompartment(name, parent, clientOnly)
  stub("ResourceManager", "AllocateDynamicCompartment", RES_STEP) return parent
end
function ResourceManager:DestroyDynamicCompartment(compartment) stub("ResourceManager", "DestroyDynamicCompartment", RES_STEP) end
function ResourceManager:AlwaysClearGameCompartment(enable) stub("ResourceManager", "AlwaysClearGameCompartment", RES_STEP) end
end  -- заглушки ResourceManager (нет движка)

-- ═══════════════════════════════════════════════════════════════════════════
-- EntityManager — shared\library\entitymanager.md (у VU общая). До К5 — заглушки.
-- ═══════════════════════════════════════════════════════════════════════════
local EntityManager = {}
_G.EntityManager = EntityManager
local ENT_STEP = "К5: сущности клиента"
-- GetIterator(entityType) -> EntityIterator (не nil): пустой итератор, Next() сразу nil.
function EntityManager:GetIterator(entityType)
  stub("EntityManager", "GetIterator", ENT_STEP)
  local it = {}
  function it:Next() return nil end
  return it
end
function EntityManager:CreateEntity(entityData, transformOrParams) stub("EntityManager", "CreateEntity", ENT_STEP) return nil end
function EntityManager:CreateEntitiesFromBlueprint(blueprint, transformOrParams)
  stub("EntityManager", "CreateEntitiesFromBlueprint", ENT_STEP) return nil
end
-- TraverseAllEntities(callback) / (context, callback): сущностей нет — обработчик не вызывается.
function EntityManager:TraverseAllEntities(...)
  stub("EntityManager", "TraverseAllEntities", ENT_STEP)
end

-- ═══════════════════════════════════════════════════════════════════════════
-- Engine:Message — shared\event\engine_message.md: message = Message { category: MessageCategory, type: MessageType }
-- (shared\type\message.md, поля только для чтения). Зовёт слушатель сообщений движка клиента (ModdingInterface.cpp,
-- как MessageManager у VU): category = msg+4, type = msg+8 — djb2-хэши со знаком, те же, что в MessageType/MessageCategory.
-- ═══════════════════════════════════════════════════════════════════════════
local Bridge = {}
_G.EVBotsClientBridge = Bridge

-- Stock ClientPlayerConnectMessage/DeletedMessage, matching VU 10C0F710.
-- Deliver after the engine frame so mod callbacks may use native entity APIs.
-- Preserve message order and suppress duplicate ready notifications until deletion.
local connectionQueue, connectedPlayers = {}, {}
function Bridge.PlayerConnectionMessage(id, connected)
  connectionQueue[#connectionQueue + 1] = {id, connected == 1}
end
Events:Subscribe("Engine:Update", function()
  if #connectionQueue == 0 then return end
  local pending = connectionQueue
  connectionQueue = {}
  for _, item in ipairs(pending) do
    local id, connected = item[1], item[2]
    if connected then
      if connectedPlayers[id] == nil then
        local p = playerObj(id)
        connectedPlayers[id] = p
        Events:_DispatchEngine("Player:Connected", p)
      end
    else
      local p = connectedPlayers[id]
      if p ~= nil then
        Events:_DispatchEngine("Player:Deleted", p)
        connectedPlayers[id] = nil
        playerObjs[id] = nil
      end
    end
  end
end)

-- ★ Сборка 234 (В2): хук Input:PreUpdate(cache: ConceptCache, deltaTime) — раз в кадр движка, до Client:UpdateInput
-- (ModdingInterface.cpp). ConceptCache:GetLevel — уровни концептов из движка (__im_concept, как InputManager:GetLevel).
-- Отличие от VU (реестр В2): у VU — хук обновления ввода движка (место не разобрано), SetLevel/Pass/Return меняют ввод
-- игры; у нас SetLevel меняет только кэш для следующих обработчиков, ввод игры не трогаем.
function Bridge.InputPreUpdate(dt)
  if Hooks == nil or Hooks._Fire == nil then return end
  local over = {}
  local cache = {}
  function cache:GetLevel(concept)
    local v = over[concept]
    if v ~= nil then return v end
    return InputManager:GetLevel(concept)
  end
  function cache:SetLevel(concept, level) over[concept] = tonumber(level) or 0.0 end
  setmetatable(cache, {
    __index = function(self, key)
      if type(key) == "number" then return self:GetLevel(key) end
    end,
    __newindex = function(self, key, value)
      if type(key) == "number" then self:SetLevel(key, value)
      else rawset(self, key, value) end
    end,
  })
  Hooks._Fire("Input:PreUpdate", cache, dt)
end

-- ★ Сборка 234 (В2): хук UI:PushScreen(screen, priority, parentGraph, stateNodeGuid) — из перехвата PushScreen
-- движка (ModdingInterface.cpp). screen/parentGraph — живые DataContainer (ResourceManager, В8), stateNodeGuid — Guid.
-- Вернуть: true — движок открывает экран; false, значение — обработчик сделал Return (экран не открывается).
-- Отличие от VU (реестр В2): Pass(...) с другими аргументами движку не передаётся (только Return/как есть).
function Bridge.UIPushScreen(screenPtr, priority, parentPtr, guidText)
  if Hooks == nil or Hooks._FireEx == nil then return true end
  local wrap = rawget(_G, "EVBotsLiveWrap")
  local screen = wrap ~= nil and wrap(screenPtr) or nil
  local parent = (wrap ~= nil and parentPtr ~= 0) and wrap(parentPtr) or nil
  local G = rawget(_G, "Guid")
  local guid = (guidText ~= nil and G ~= nil) and G(guidText) or nil
  local _, mode, rv = Hooks._FireEx("UI:PushScreen", screen, priority, parent, guid)
  if mode == "return" then return false, rv == true end
  return true, false
end
local MessageMeta = {
  __index = function(t, k)
    if k == "category" then return rawget(t, "_category") end
    if k == "type" then return rawget(t, "_type") end
    return nil
  end,
  __newindex = function() error("Message: поля только для чтения", 2) end,
  __tostring = function(t) return "Message(" .. tostring(rawget(t, "_type")) .. ")" end,
}
function Bridge.EngineMessage(category, msgType)
  Events:Dispatch("Engine:Message", setmetatable({ _category = category, _type = msgType }, MessageMeta))
end

return true
)LUA";

int l_client_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "INFO");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: %s", text);
    return 0;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const char* p, size_t len) {
    if (len == 0) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(len), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(len), w.data(), n);
    return w;
}

/// ★ Сборка 228: папка мода, чей код исполняется сейчас (как currentMod() в LuaEvents.cpp).
int l_client_mod_now(lua_State* L) {
    const std::string s = ToUtf8(LuaRuntime::Instance().currentModFolder());
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

/// ★ Сборка 228: сделать «текущим» мод папки; вернуть прежнюю папку (для обратного вызова).
int l_client_mod_scope(lua_State* L) {
    size_t n = 0;
    const char* p = luaL_optlstring(L, 1, "", &n);
    const std::string prev = ToUtf8(LuaRuntime::Instance().PushCurrentMod(FromUtf8(p, n)));
    lua_pushlstring(L, prev.data(), prev.size());
    return 1;
}

}  // namespace

bool RegisterVextClient(lua_State* L) {
    if (!RegisterVextSettings(L)) return false;
    if (!RegisterVextSpectator(L)) return false;
    if (luaL_loadbufferx(L, kClientSource, sizeof(kClientSource) - 1, "=[vext-client]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: клиентские библиотеки не собрались: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_client_log);
    lua_pushcfunction(L, l_client_mod_now);
    lua_pushcfunction(L, l_client_mod_scope);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        luaL_traceback(L, L, lua_tostring(L, -1), 0);
        const char* tb = lua_tostring(L, -1);
        LOG_ERR("EVBots: клиентские библиотеки не выполнились: %s", tb != nullptr ? tb : "?");
        lua_pop(L, 2);
        return false;
    }
    LOG_INFO("EVBots: клиентские библиотеки VU подключены (NetEvents, PlayerManager, InputManager, ClientUtils, "
             "RaycastManager, DebugRenderer, WebUI, ResourceManager, EntityManager — каркас К3, без движка)");
    return true;
}

}  // namespace evbots
