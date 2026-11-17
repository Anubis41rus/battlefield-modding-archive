// VextPlayerManager.cpp — серверная библиотека VeniceEXT PlayerManager.
//
// Глобальная таблица PlayerManager (server) и типы Player, SoldierEntity,
// EntryInput — по документации VU (НЕ по памяти):
//   content\vext\ref\server\library\playermanager.md
//   content\vext\ref\server\type\player.md
//   content\vext\ref\shared\type\entryinput.md
//
// Что здесь настоящее, а что заглушка:
//   - Реестр игроков (CreatePlayer/DeletePlayer/GetPlayers и все Get*)
//     РАБОТАЕТ по-настоящему: игроки-боты хранятся в таблице и живут,
//     пока их не удалят. Это база, на которой fun-bots считает слоты,
//     защиту ботов и т.п.
//   - Движок BF3 — через NativeBridge (мост, очереди в хуке тика): SpawnSoldierAt (createSoldier+spawnSoldier движка,
//     как VU), EnterVehicle/ExitVehicle (178), Kick (RCON, 186). С 199 SelectWeapon, AttachSoldier, EnableInput,
//     Ban* — настоящие (RCON banList.add/save). Заглушка осталась одна: Fade (эффекта экрана клиента нет без клиентского
//     VEXT); CreateSoldier отдаёт Lua-объект (NativeBridge.CreateEngineSoldier → 0), тело создаёт SpawnSoldierAt.
//     В тест-хосте движка нет — всё это не падает и не ломает логику.
//
// Бот в fun-bots отличается от человека тем, что у него onlineId == 0
// (именно так BotManager:DestroyAllOldBotPlayers отличает ботов). Поэтому
// CreatePlayer для ботов даёт onlineId = 0, как на настоящем VU-сервере.

#include "VextPlayerManager.h"
#include "EngineHooks.h"
#include "InlineHook.h"
#include "SpawnPolicy.h"
#include "VextEntityManager.h"
#include "LevelPolicy.h"
#include "NativeSoldierCustomization.h"
#include "EngineResources.h"
#include "NativeBlueprint.h"
#include "TeamCapacity.h"

#include <intrin.h>

#include <cmath>
#include <string>

#include "EVBotsLog.h"
#include "VextResourceManager.h"
#include "LuaRuntime.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {

static bool EngineLevelInfoRaw(char* name, size_t nameCap, char* mode, size_t modeCap);

// ★ Сборка 192: нечисловые значения мода (NaN/inf) движку не отдаём. Прогон 191, 01:01:04: мод посчитал упреждение
// через math.sqrt отрицательного (VehicleAiming.lua, _GetTimeToTravel) — у пилота Ми-28 ввод «рыск nan», стрелок вылез,
// и сервер упал на FatalAssert «Active camera transform is invalid when leaving entry Vehicles/Mi28/Mi28»
// (ServerPlayerEntryComponent.cpp:102). У VU сервер — клиентский bf3.exe, где этот assert сервер не роняет; наш exe —
// с FatalAssert. Значение не пишется (в движке остаётся прежнее), первые 30 случаев — в лог.
static bool NonFiniteGuard(double v, const char* what, long idx) {
    if (std::isfinite(v)) return true;
    static volatile LONG s_n = 0;
    const LONG n = InterlockedIncrement(&s_n);
    if (n <= 30 || (n % 1000) == 0) {
        LOG_WARN("Bridge: %s[%ld] = не число (%s) — в движок не пишем (всего %ld)", what, idx,
                 std::isnan(v) ? "NaN" : "inf", n);
    }
    return false;
}

// Поставщики данных движка (определены в SpmProbe.cpp). Объявлены
// ВНЕ анонимного namespace, иначе у них внутреннее связывание и мост
// не свяжется с хуками (MSVC C5046).
void* EVBotsEnginePlayerManager();
bool EVBotsVerifyTeamPropertyEntry(unsigned long address,const unsigned char* expected,unsigned size);
unsigned long EVBotsEngineSpawnPoint();
unsigned long EVBotsEngineSpawnPointBest();
void EVBotsBridgeRetryPending();
void EVBotsRconInstallDispatchHookOnce();  // сборка 209, VextRcon.cpp
void EVBotsRconNoteServer(void* server);    // ★ 239 (А2), VextRcon.cpp
void EVBotsRconDrainReplies();             // сборка 209, VextRcon.cpp
void* EVBotsEngineOnCreateMsg();  // сообщение о создании игрока
unsigned long EVBotsEngineSpawnPointForTeam(unsigned int team);  // точка по команде
unsigned long EVBotsEngineSpawnPointBestForTeam(unsigned int team);  // сборка 195: запасная, только своей команды
bool EVBotsEngineSpawnScanDone();  // скан точек закончен
void EVBotsEngineStartSpawnScan();  // запуск скана без живого спавна
unsigned int EVBotsEngineHumanTeam();  // команда живого игрока (1/2)
unsigned long EVBotsEnginePointSystem();  // PointSystem (снят перехватчиком)
unsigned long EVBotsEngineSpawnKit();  // кит живого игрока (a1 spawnSoldier)
void EVBotsEngineRefFields(unsigned long* f318, unsigned long* f31c,
                           unsigned long* f480);  // эталон полей
// ★ Эталон полей В МОМЕНТ СПАВНА настоящего игрока: у него +0x318 = 0
// (а НЕ 1, как мы ошибочно писали) и +0x480 = указатель (а НЕ 0).
void EVBotsEngineSpawnRefFields(unsigned long* f318, unsigned long* f31c,
                                unsigned long* f480);
void EVBotsEngineOurSpawnBegin();
void EVBotsEngineOurSpawnEnd();
// ★ Настоящие аргументы createPlayer, снятые перехватчиком при реальном
// входе клиента (12.10.2026): a2=FFFFFFFF, a3=FA000000.
// Раньше мы передавали a3 = 0x40603000 — это было НЕВЕРНО.
void EVBotsEngineCreateRefArgs(long* a1, long* a2, long* a3);
unsigned long EVBotsEngineRealPlayer();
unsigned long EVBotsEngineHumanByName(const char* want);  // сборка 201: человек по имени
int EVBotsEngineHumans(unsigned long* out, int cap);  // сборка 202: все люди сервера (список игроков движка)
int EVBotsEngineHumansChecked(unsigned long* out, int cap);  // сборка 214: < 0 — список не уверен (n = −1 − r)
bool EVBotsEngineIsHuman(unsigned long ptr);  // сборка 202: игрок движка — один из людей
bool EVBotsEngineRealPlayerName(char* out, size_t cap);  // сборка 139: имя настоящего игрока
int EVBotsEngineHumanSquad();  // отряд человека по перехвату addToSquad (-1 — неизвестно)
bool EVBotsEngineAdoptHumanRef(unsigned long human, unsigned long fallback480);  // сборки 123–124: эталон до спавна
unsigned long EVBotsEngineSquadManager();
// ★ Точка спавна номер n — каждому боту своя (иначе боты слипаются
// в геометрию и гибнут в зоне поражения через ~10 с).
unsigned long EVBotsEngineSpawnPointNth(unsigned long n, unsigned int team);
// ★ Настоящие аргументы addToTeam (регистрация в команде/отряде).
// Возвращает false, пока перехватчик не снял их у живого игрока.
bool EVBotsEngineSquadAddTeamArgs(unsigned long* p2, unsigned long* p3,
                                  unsigned long* p4);
// ★ Точка спавна с запасным вариантом: настоящая точка спавна живого игрока,
// а если её нет — любая пригодная из списка конструкторов точек.
unsigned long EVBotsEngineSpawnPointBest();
void RetryPendingSpawns();
static void PushDmg(unsigned long victim, unsigned long giver, float dmg, float hpAfter);  // сборка 153, определение — у перехвата handleCollision
static void HumanHpTick();  // сборка 153
int l_bridge_poll_damage(lua_State* L);  // сборка 153
int l_bridge_poll_chat(lua_State* L);  // сборка 159: входящий чат (EVBOTS_CHAT_EVENTS)
bool EVBotsChatSendPop(int* kind, int* team, int* squad, char* text, int cap);  // сборка 159, определение — VextChatManager.cpp
int EVBotsListEntitiesByName(const char* name, unsigned long* out, int cap);  // сборка 159, определение — VextEntityManager.cpp
int l_bridge_damage_stats(lua_State* L);  // сборка 153
int l_bridge_level_reload(lua_State* L);  // сборка 167: смена уровня (Level:Destroy → Level:Loaded), как VU
int l_bridge_level_reset(lua_State* L);
int l_bridge_level_info(lua_State* L);    // сборка 185: имя уровня и режим из движка
int l_bridge_level_signals(lua_State* L); // сборка 186: конец раунда (RCON onMessage) и смена уровня, как VU
int l_bridge_set_level_gate(lua_State* L); // сборка 186: спавн закрыт, пока уровень меняется
int l_bridge_level_changing(lua_State* L); // сборка 214: уровень меняется (для ухода человека)
int l_bridge_poll_stat_events(lua_State* L); // сборка 186: ScoringSystem:StatEvent
int l_bridge_poll_orders(lua_State* L);       // сборка 193: приказы колеса BF3
int l_bridge_gun_sway(lua_State* L);  // сборка 186: weaponFiring.gunSway (компенсация отдачи)
int l_bridge_corpse(lua_State* L);        // сборка 185: player.corpse (реанимация медиками), как VU
int l_bridge_poll_combat_area(lua_State* L);  // сборка 185: CombatArea:PlayerDeserting/Returning
void EVBotsEngineResetSpawnRef();  // сборка 167, SpmProbe.cpp
void EVBotsInstallEntityFactoryHookOnce();  // сборка 186, VextEntityManager.cpp
void EVBotsEngineResetSpawnPoints();  // сборка 173, SpmProbe.cpp: точки спавна старого уровня

int l_bridge_bullet_call_original(lua_State* L);  // сборка 163: HookContext:Call() хука BulletEntity:Collision (вне анонимного блока — определение ниже)
namespace {

static bool LevelGateClosed();

// Чанк получает первым аргументом функцию логирования (как LuaEvents).
constexpr char kPlayerManagerSource[] = R"LUA(
-- Серверный PlayerManager VeniceEXT (VU)
-- по content\vext\ref\server\library\playermanager.md
-- и player.md / entryinput.md

local log = ...

local PlayerManager = {}
_G.PlayerManager = PlayerManager
local nativeFadeAll = EVBotsNativeFadeAll
local nativePlayerFade = EVBotsNativePlayerFade
function PlayerManager:FadeOutAll(fadeTime)
  return nativeFadeAll(fadeTime, true)
end
function PlayerManager:FadeInAll(fadeTime)
  return nativeFadeAll(fadeTime, false)
end

-- ── реестр игроков ────────────────────────────────────────────────────────
local playersById = {}          -- id -> player
local players = {}              -- упорядоченный список (для GetPlayers)
-- ★ 231: id до привязки к движку — временный, с 1000 (у движка 0..63, не пересекаются). Привязали — Player.id = id движка,
-- как VU (Player.id = ServerPlayer+0x3B8): один номер у сервера и клиента (прогон 230: не совпал ни один из 12, реестр В19).
local nextId = 1000

-- Временный id остаётся псевдонимом (мод мог его запомнить, пока игрок ждал движок в очереди).
-- ★ 247: keep = id уже отдан моду (привязка после возврата из CreatePlayer — очередь «ждёт точку спавна»). На VU Player.id
-- после создания не меняется; мод держит его в Bot.m_Id и строит _BotsByPlayerId по m_Player.id (RefreshTables) — смена id
-- рвала GetBotById(m_Id) → пары bot-bot не собирались (прогон 247, 07:54–08:02: запросов 0), выход из техники терял бота.
-- Тогда id для мода остаётся прежним, движковый — псевдоним (playersById[eid], p._engineId); на границе с клиентом переводим.
function PlayerManager._AdoptEngineId(p, ptr, keep)
  if p == nil or ptr == nil or ptr == 0 or NativeBridge == nil or NativeBridge.EnginePlayerId == nil then return end
  local ok, eid = pcall(NativeBridge.EnginePlayerId, ptr)
  if not ok or type(eid) ~= 'number' or eid < 0 or eid == p.id then return end
  local other = playersById[eid]
  if other ~= nil and other ~= p and (other.id == eid or rawget(other, '_engineId') == eid) then
    log("WARN ", "мост: id движка " .. tostring(eid) .. " для " .. tostring(p.name) .. " занят " .. tostring(other.name) .. " — id не меняем")
    return
  end
  if keep then
    log("INFO ", "мост: " .. tostring(p.name) .. " id для мода " .. tostring(p.id) .. ", движка " .. tostring(eid)
        .. " (псевдоним; id уже у мода — не меняем, как VU)")
    rawset(p, '_engineId', eid)
    playersById[eid] = p
    return
  end
  log("INFO ", "мост: " .. tostring(p.name) .. " id " .. tostring(p.id) .. " -> " .. tostring(eid) .. " (id движка, как VU)")
  if p._tmpId == nil then p._tmpId = p.id end
  p.id = eid
  playersById[eid] = p
end

-- ★ 246: живой id по любому известному номеру (временный 1000+ до привязки, движковый после). CreatePlayer отдаёт
-- временный id, мод запоминает его в Bot.m_Id (Bot.lua:34) насовсем; если движковый игрок создался позже (очередь
-- «ждёт точку спавна», сборка 178), клиент знает бота только под движковым номером — на границе переписываем.
-- Серверный playersById хранит и устаревшую запись по временному id (мост: BOT_Josiah id 1001 -> 1, сессия 15:10).
function PlayerManager._LiveId(id)
  if type(id) ~= 'number' then return id end
  local p = playersById[id]
  if p ~= nil and p.id ~= nil and p.id ~= id then return p.id end
  return id
end
-- ★ 247: id движка по id мода (сервер -> клиент: клиент знает игроков только по движковым номерам)
function PlayerManager._EngineId(id)
  if type(id) ~= 'number' then return id end
  local p = playersById[id]
  if p ~= nil and rawget(p, '_engineId') ~= nil then return rawget(p, '_engineId') end
  return id
end

-- ── Player ────────────────────────────────────────────────────────────────
-- Поля — из server\type\player.md. Значения "как новый игрок на сервере".

local Player = {}
-- EVBots players currently use tables. Royale's shared name helper takes the
-- table branch (GetName) instead of VU's userdata branch (.name).
function Player:GetName() return self.name end
-- ★ Сборка 171: score/kills/deaths как в VU — живые, из PointSystem движка (NativeBridge.PlayerScore); были нули в таблице.
local scoreField = { score = 1, kills = 2, deaths = 3 }
-- Сборка 178: указатель игрока в движке (у человека nativePtr не задан). ★ Сборка 202: у каждого человека свой (EVBotsHumanPtr,
-- по имени), а не первый человек движка (RealPlayer) — техника, труп, очки второго человека были чужими.
local function playerPtr(t)
  local ptr = rawget(t, 'nativePtr')
  if (ptr == nil or ptr == 0) and (rawget(t, 'onlineId') or 0) ~= 0 and EVBotsHumanPtr ~= nil then
    ptr = EVBotsHumanPtr(t)
  end
  if ptr == nil or ptr == 0 then return nil end
  return ptr
end
-- Техника (178): controlledControllable / attachedControllable / controlledEntryId живые, из полей игрока движка
-- (+0x3E8 / +0x3D8 / +0x3D4). Пешком: controlledControllable / attachedControllable = nil, entryId = 0 (как было).
-- Сборка 181: inVehicle (VU Player.inVehicle — в машине ли игрок; клиент мода берёт по нему позицию для лучей).
local vehicleField = { controlledControllable = 1, attachedControllable = 2, controlledEntryId = 3, inVehicle = 4 }
local function vehicleFieldValue(t, vk)
  -- ★ Сборка 245: быстрый путь, как C++-геттер VU: бот пешком и посадка не заказана — одним вызовом C (NativeBridge.PlayerOnFoot,
  -- без pcall и обёртки техники). Мод читает controlledControllable/attachedControllable ~190 раз за тик (профиль 240).
  if rawget(t, '_vehPending') == nil then
    local np = rawget(t, 'nativePtr')
    local onFoot = NativeBridge ~= nil and NativeBridge.PlayerOnFoot or nil
    if onFoot ~= nil and np ~= nil and np ~= 0 and onFoot(np) then
      if vk == 3 then return 0 end
      if vk == 4 then return false end
      if vk == 1 then return rawget(t, 'soldier') end
      return nil
    end
  end
  local ptr = playerPtr(t)
  local veh, entry = nil, 0
  if ptr ~= nil and NativeBridge ~= nil and NativeBridge.PlayerVehicle ~= nil
      and EntityManager ~= nil and EntityManager._VehicleByPtr ~= nil then
    -- ★ Сборка 240: PlayerVehicleV — не техника (у пешего controlled — сам солдат) уже 0, без обёртки и emIsVeh на каждое чтение
    local ok, att, ent, ctl = pcall(NativeBridge.PlayerVehicleV or NativeBridge.PlayerVehicle, ptr)
    if ok and att ~= nil then
      if att ~= 0 then veh = EntityManager._VehicleByPtr(att) end
      if veh == nil and ctl ~= 0 then veh = EntityManager._VehicleByPtr(ctl) end
      if veh ~= nil then entry = ent end
    end
  end
  -- ★ Как в VU посадка/выход «сразу»: мод зовёт UpdateVehicleMovableId сразу после EnterVehicle (Bot.lua:586) и ждёт, что
  -- controlledControllable уже машина; движок садит бота в тике моста (очередь), и без этого бот оставался в пехотном
  -- состоянии (прогон 178: водитель жал прыжок/бег, машина стояла). До подтверждения движком (или 3 с) отдаём заказанное.
  local pend = rawget(t, '_vehPending')
  if pend ~= nil then
    -- сборка 250: заказ до привязки (later) живёт до привязки игрока движка — она ждёт точку спавна, бывает дольше 6 с
    if os.clock() - pend.t > (pend.later and 30.0 or 6.0) then
      rawset(t, '_vehPending', nil)
    elseif pend.kind == 1 then
      -- Сборка 235: мост снял запрос (машина исчезла, нет места) и бот не сел — как отказ VU, заказанное не показываем.
      local gone = false
      local pe = type(pend.ent) == "table" and rawget(pend.ent, "_ptr") or nil
      if not pend.engineSeats and not pend.later and pe ~= nil and NativeBridge ~= nil and NativeBridge.VehSeatQueued ~= nil then
        local okq, qp = pcall(NativeBridge.VehSeatQueued, pe, pend.seat)
        gone = okq and qp == 0
      end
      if veh == pend.ent and entry == pend.seat then rawset(t, '_vehPending', nil)
      elseif gone then rawset(t, '_vehPending', nil)
      else veh, entry = pend.ent, pend.seat end
    else
      if veh == nil then rawset(t, '_vehPending', nil) else veh, entry = nil, 0 end
    end
  end
  if veh ~= nil then
    if vk == 3 then return entry end
    if vk == 4 then return true end
    return veh
  end
  if vk == 3 then return 0 end
  if vk == 4 then return false end
  -- Пешком (как в VU): управляемая сущность — сам солдат, прикреплённой нет.
  if vk == 1 then return rawget(t, 'soldier') end
  return nil
end
-- ★ Сборка 185: player.corpse как в VU — труп из движка ([[игрок+0x3CC]] − 4, NativeBridge.Corpse), живой на каждое чтение.
-- Мод по нему ищет, кого реанимировать (BotManager:_CheckForBotBotRevive, BotActions.lua:20), целится и подходит к телу.
-- До 185 corpse был всегда nil: медики-боты не реанимировали никого.
local CorpseMt = { __index = function(c, k)
  if k == "transform" then return rawget(c, "worldTransform") end
  if k == "typeInfo" then return { name = "ServerSoldierEntity" } end
  return nil
end }
local function corpseIs(_, typeName)
  return typeName == "ServerSoldierEntity" or typeName == "SoldierEntity" or typeName == "Entity"
end
local function corpseValue(t)
  local ptr = playerPtr(t)
  if ptr == nil or NativeBridge == nil or NativeBridge.Corpse == nil then return nil end
  local ok, sp, x, y, z, dead, down, idown = pcall(NativeBridge.Corpse, ptr)
  if not ok or sp == nil then
    rawset(t, '_corpse', nil)
    return nil
  end
  local c = rawget(t, '_corpse')
  if c == nil or rawget(c, '_ptr') ~= sp then
    c = setmetatable({ _ptr = sp, player = t, isAlive = false, Is = corpseIs }, CorpseMt)
    rawset(t, '_corpse', c)
  end
  local pos = Vec3._make(x, y, z)
  c.worldTransform = LinearTransform(Vec3._make(1, 0, 0), Vec3._make(0, 1, 0), Vec3._make(0, 0, 1), pos)
  c.physicsEntityBase = { position = Vec3._make(x, y, z), partCount = 0 }
  c.isDead = dead == true
  c.isManDown = down == true
  c.isInteractiveManDown = idown == true
  return c
end
-- Keep these properties absent from the public table: Lua otherwise bypasses
-- __newindex after the first assignment. Cache only unbound players and events.
local function playerTeamValue(t, key)
  local ptr = playerPtr(t)
  if ptr ~= nil and NativeBridge ~= nil and NativeBridge.PlayerTeamProperty ~= nil then
    local team, squad = NativeBridge.PlayerTeamProperty(ptr, rawget(t, 'name'))
    return key == 'teamId' and team or squad
  end
  return rawget(t, key == 'teamId' and '_teamId' or '_squadId') or 0
end
local function playerTeamAssign(t, key, value)
  if type(value) ~= 'number' or value % 1 ~= 0 or value < 0 or value > (key == 'teamId' and 127 or 32) then
    error('Player.' .. key .. ': invalid enum value', 3)
  end
  local ptr = playerPtr(t)
  if ptr ~= nil and NativeBridge ~= nil and NativeBridge.PlayerTeamProperty ~= nil then
    local team, squad = NativeBridge.PlayerTeamProperty(ptr, rawget(t, 'name'), key == 'teamId' and 1 or 2, value)
    rawset(t, '_teamId', team)
    rawset(t, '_squadId', squad)
    rawset(t, '_observedTeamId', team)
  else
    rawset(t, key == 'teamId' and '_teamId' or '_squadId', value)
    rawset(t, key == 'teamId' and '_pendingTeamId' or '_pendingSquadId', value)
  end
end
Player.__index = function(t, k)
  if k == 'teamId' or k == 'squadId' then return playerTeamValue(t, k) end
  if k == 'corpse' then return corpseValue(t) end
  -- ★ Сборка 197: isAllowedToSpawnOn как VU — байт движка (сервер: [игрок+[0x0285BAD8]+0xFB], сеттер 0x01171490 зовут события
  -- AutoTeam Enable/DisableAllowedToSpawnOn; у VU на клиентском exe — [игрок+0x420+0x52]). Мод по нему сажает ботов на
  -- товарища (GameDirector.lua:1198…). Не прочитать — true, как было до 197.
  if k == 'isAllowedToSpawnOn' then
    local ptr = playerPtr(t)
    if ptr ~= nil and NativeBridge ~= nil and NativeBridge.PlayerSpawnOn ~= nil then
      local ok, v = pcall(NativeBridge.PlayerSpawnOn, ptr)
      if ok and v ~= nil then return v end
    end
    return true
  end
  local vk = vehicleField[k]
  if vk ~= nil then return vehicleFieldValue(t, vk) end
  local i = scoreField[k]
  if i ~= nil then
    local ptr = playerPtr(t)  -- сборка 202: у человека — свой указатель
    if ptr ~= nil and ptr ~= 0 and NativeBridge ~= nil and NativeBridge.PlayerScore ~= nil then
      local v = { pcall(NativeBridge.PlayerScore, ptr) }
      if v[1] and v[2] ~= nil then return v[i + 1] end
    end
    return 0
  end
  return Player[k]
end
-- ★ Сборка 199: хук VU «Soldier:Damage» ДО урона (soldier_damage.md) — зовёт C++ из ServerSoldierEntity::onDamage 0x00E08000 в
-- потоке движка (под локом Lua, короткое ожидание). Мод меняет info.damage: множители урона ботов по людям (DamageFactor*,
-- BotManager.lua:340–343) и запрет самоубийства (BotCanKillHimself=false, 330–332). Возврат — урон для движка.
-- Настенное время, с (os.clock — процессорное время процесса, для окон «N секунд» не годится)
function EVBotsNow()
  if SharedUtils ~= nil and SharedUtils.GetTimeNS ~= nil then
    local ok, t = pcall(SharedUtils.GetTimeNS, SharedUtils)
    if ok and type(t) == 'number' then return t / 1e9 end
  end
  return os.clock()
end
EVBotsDamage = EVBotsDamage or { calls = 0, changed = 0 }
function EVBotsDamage.Pre(victimPtr, giverPtr, damage, isBullet, isClient, healthBefore)
  EVBotsDamage.calls = EVBotsDamage.calls + 1
  if Hooks == nil or Hooks._Fire == nil or type(damage) ~= 'number' then return damage end
  local victim, giver = nil, nil
  local okp, list = pcall(function() return PlayerManager:GetPlayers() end)
  if okp and type(list) == 'table' then
    for _, p in ipairs(list) do
      local ptr = rawget(p, 'nativePtr')
      -- ★ Сборка 202: у каждого человека свой указатель (раньше всем людям — первый человек: стрелок-гость не находился,
      -- бот не отвечал на его огонь)
      if (ptr == nil or ptr == 0) and (rawget(p, 'onlineId') or 0) ~= 0 then ptr = EVBotsHumanPtr(p) end
      if ptr ~= nil and ptr ~= 0 then
        if ptr == victimPtr then victim = p end
        if giverPtr ~= nil and giverPtr ~= 0 and ptr == giverPtr then giver = p end
      end
    end
  end
  local sol = victim ~= nil and rawget(victim, 'soldier') or nil
  if sol == nil then return damage end
  local info = { damage = damage, isBulletDamage = isBullet == true, isClientDamage = isClient == true,
                 isExplosionDamage = false, boneIndex = 0 }
  if type(healthBefore) == 'number' and healthBefore >= 0 and healthBefore < math.huge then
    info.evbotsHealthBefore = healthBefore
  end
  local giverInfo = (giver ~= nil) and { giver = giver } or nil
  rawset(victim, '_preDmgAt', EVBotsNow())
  if giver ~= nil then rawset(victim, '_lastDmg', { giver = giver, at = EVBotsNow() }) end
  Hooks._Fire('Soldier:Damage', sol, info, giverInfo)
  local d = tonumber(info.damage)
  if d == nil or d ~= d or d == math.huge or d == -math.huge then return damage end
  if d ~= damage then EVBotsDamage.changed = EVBotsDamage.changed + 1 end
  return d
end

-- ★ Мод ЗАМЕНЯЕТ player.input своим объектом (BotManager: s_BotPlayer.input =
-- s_BotInput). Без этого привязка input._native к движку (поставленная при
-- создании) терялась бы и SetLevel уходил в никуда. __newindex штампует
-- _native на каждый новый input (и наоборот: при поздней установке nativePtr
-- привязывает текущий input). Остальные поля — обычный rawset.
Player.__newindex = function(self, key, value)
  if key == 'teamId' or key == 'squadId' then return playerTeamAssign(self, key, value) end
  rawset(self, key, value)
  if key == 'input' and type(value) == 'table' then
    local ptr = rawget(self, 'nativePtr')
    if ptr ~= nil then value._native = ptr end
  elseif key == 'nativePtr' and value ~= nil then
    if value ~= 0 then
      local team, squad = rawget(self, '_pendingTeamId'), rawget(self, '_pendingSquadId')
      if team ~= nil then playerTeamAssign(self, 'teamId', team); rawset(self, '_pendingTeamId', nil) end
      if squad ~= nil then playerTeamAssign(self, 'squadId', squad); rawset(self, '_pendingSquadId', nil) end
    end
    local inp = rawget(self, 'input')
    if type(inp) == 'table' then inp._native = value end
    -- ★ Сборка 244: посадка в технику, заказанная модом до привязки игрока движка (Player:EnterVehicle)
    local el = rawget(self, '_enterLater')
    if el ~= nil and value ~= 0 then
      rawset(self, '_enterLater', nil)
      local ep = EVBotsRawType(el.entity) == 'table' and rawget(el.entity, '_ptr') or nil
      if ep ~= nil and EVBotsSeatLater ~= nil and EVBotsSeatLater[ep] ~= nil then EVBotsSeatLater[ep][el.seat] = nil end
      -- сборка 250: заказ до привязки снимаем — EnterVehicle ниже ставит настоящий, если мост принял посадку
      local pv = rawget(self, '_vehPending')
      if pv ~= nil and pv.later then rawset(self, '_vehPending', nil) end
      local ok, err = pcall(self.EnterVehicle, self, el.entity, el.seat)
      log("INFO ", "Player:EnterVehicle(" .. tostring(rawget(self, 'name')) .. "): посадка после привязки — "
        .. (ok and "заказана" or ("ошибка " .. tostring(err))))
    end
  end
end

-- ── SoldierEntity (server\type\soldierentity.md) ────────────────────────────
-- Объект солдата. Настоящего солдата не создаём (движка в тест-хосте нет),
-- но fun-bots требует непустого результата от Player:CreateSoldier, иначе
-- "CreateSoldier failed" и бот не спавнится. Возвращаем Lua-объект по
-- документации: вои-поля, которые мод реально трогает (health/maxHealth,
-- worldTransform, pose, weaponsComponent, методы Kill/SetPose/SetTransform/
-- ApplyCustomization/SingleStepEntry), — рабочие заглушки.
local SoldierEntity = {}
-- ★ Сборка 171: скорость солдата ближе к движковой (VU PhysicsEntity.velocity). Таймер Lua не совпадает с тиком движка: на
-- части тиков позиция ещё не обновлена (смещение РОВНО 0) — раньше это давало скорость 0, и fun-bots считал бота застрявшим
-- (BotMovement.lua:291 «velocity < 0.3» → обход препятствий, через 2 попытки — телепорт: «скользит без анимации», 12–19
-- телепортов за 20 с в прогоне 170). Теперь: позиция не изменилась и прошло < 0,25 с — скорость прежняя; иначе — смещение за
-- реальный интервал с прошлого обновления. EVBOTS_VEL_HOLD=0 — по-старому (смещение за один тик).
-- ★ Сборка 201: указатель движка человека — по имени (NativeBridge.HumanPtr), кэш на объекте; один человек на сервере — как
-- раньше (RealPlayer). nil — человека в списке движка нет.
function EVBotsHumanPtr(p)
  if p == nil or NativeBridge == nil then return nil end
  local c = rawget(p, "_humanPtr")
  if c ~= nil and c ~= 0 and NativeBridge.ReadU32 ~= nil then
    local ok, v = pcall(NativeBridge.ReadU32, c, 0)
    if ok and v ~= nil and v ~= 0 then return c end
  end
  local ptr = 0
  if NativeBridge.HumanPtr ~= nil and p.name ~= nil then
    local ok, v = pcall(NativeBridge.HumanPtr, tostring(p.name))
    if ok and v ~= nil then ptr = v end
  end
  -- ★ Сборка 202: запасной путь «первый человек» — только без натива по имени; иначе ушедший человек получил бы чужой указатель.
  if (ptr == nil or ptr == 0) and NativeBridge.HumanPtr == nil and NativeBridge.RealPlayer ~= nil then
    local ok, v = pcall(NativeBridge.RealPlayer)
    if ok and v ~= nil then ptr = v end
  end
  if ptr == nil or ptr == 0 then return nil end
  rawset(p, "_humanPtr", ptr)
  return ptr
end
-- Движковый адрес может оставаться читаемым после выгрузки; это ещё не тот же человек на новом уровне.
function PlayerManager._LevelReset()
  for _, p in ipairs(players) do
    p._basePathPending = nil
    if p.onlineId ~= 0 then
      p.clientSettingsSent = nil
      rawset(p, '_humanPtr', nil)
      rawset(p, '_respBody', nil)
      rawset(p, '_modWaitSince', nil)
      rawset(p, '_modWaitSaid', nil)
      if p.hostMirror then
        p.soldier, p.hostMirror = nil, nil
        p.hasSoldier, p.alive = false, false
      end
    end
  end
end
-- ★ Сборка 202: игрок моста по указателю движка — бот по nativePtr, человек по своему указателю (любой из людей, не только
-- первый). Для урона, чата, пуль: раньше указатель второго человека не находился («не игрок моста»).
function EVBotsPlayerByPtr(ptr)
  if ptr == nil or ptr == 0 then return nil end
  for i = 1, #players do
    if rawget(players[i], 'nativePtr') == ptr then return players[i] end
  end
  for i = 1, #players do
    local p = players[i]
    if (rawget(p, 'onlineId') or 0) ~= 0 and EVBotsHumanPtr(p) == ptr then return p end
  end
  return nil
end
-- Не выдаём неудачную установку ввода за успех: позднее тело/свободный слот могут появиться на следующем тике.
function EVBotsEnsureOwnInput(p)
  if p.ownInputOn == p.nativePtr or NativeBridge.BotOwnInput == nil then return end
  local ok, _, installed = pcall(NativeBridge.BotOwnInput, p.nativePtr, 1)
  if ok and installed then
    p.ownInputOn, p._ownInputFails = p.nativePtr, nil
  else
    p._ownInputFails = (p._ownInputFails or 0) + 1
    if p._ownInputFails == 30 or p._ownInputFails % 300 == 0 then
      log("WRN  ", "ввод: " .. tostring(p.name) .. " — свой объект ввода не встал за " .. p._ownInputFails .. " попыток; повтор продолжается")
    end
  end
end
-- Только после перенаправленного пехотного спавна TankSuperiority0: путь начинается у фактического тела на HQ.
-- Используется тот же поиск мода, что у BotSpawner для обычного движкового спавна.
function EVBotsAlignBaseSpawn(p, position)
  local gd, bm = rawget(_G, 'g_GameDirector'), rawget(_G, 'g_BotManager')
  if gd == nil or bm == nil or gd.FindClosestPath == nil or bm.GetBotByName == nil then return false end
  local b = bm:GetBotByName(p.name)
  if b == nil or b.SetVarsWay == nil then return false end
  local node = gd:FindClosestPath(position, false, true, nil, 1)
  if node == nil then return false end
  b:SetVarsWay(nil, true, node.PathIndex, node.PointIndex, false)
  log("INFO ", string.format("TankSuperiority0: %s — путь от HQ %s:%s (%.0f, %.0f, %.0f)",
    tostring(p.name), tostring(node.PathIndex), tostring(node.PointIndex), position.x, position.y, position.z))
  return true
end
-- skipVel (сборка 245): скорость возьмут из движка — производную не создаём (лишний Vec3 на бота за тик), только история.
function EVBotsVelocityStep(s, x, y, z, dt, skipVel)
  local prev = s.bridgePrev
  if prev == nil then
    s.bridgePrev = { x, y, z }
    s.velAcc = 0
    return
  end
  local dx, dy, dz = x - prev[1], y - prev[2], z - prev[3]
  local acc = (s.velAcc or 0) + (dt or 0)
  if dx * dx + dy * dy + dz * dz > 400.0 then
    if not skipVel then s.bridgeVelocity = Vec3._make(0, 0, 0) end  -- посадка/телепорт: скорость на этом тике не считаем
  elseif EVBOTS_VEL_HOLD ~= false and dx == 0 and dy == 0 and dz == 0 and acc < 0.25 then
    s.velAcc = acc  -- движок ещё не обновил позицию: скорость прежняя
    return
  elseif acc > 0 and not skipVel then
    local inv = (EVBOTS_VEL_HOLD ~= false) and acc or (dt or acc)
    s.bridgeVelocity = Vec3._make(dx / inv, dy / inv, dz / inv)
  end
  s.velAcc = 0
  prev[1], prev[2], prev[3] = x, y, z
end
-- Сборка 124: soldier.velocity (мод целится с упреждением, BotAiming.lua:214) = скорость, посчитанная мостом.
SoldierEntity.__index = function(t, k)
  if k == "velocity" then
    local v = rawget(t, "bridgeVelocity")
    if v ~= nil then return v end
    -- Сборка 134: скорость ещё не измерена (до старта раунда движок не даёт позицию солдата). Ноль мод читает как
    -- «стоит» и запускает обход препятствий (шаги, огонь, телепорт). Пока не измерено — отдаём «идёт».
    if EVBOTS_VEL_STUB ~= false then return Vec3._make(1.0, 0, 0) end
    return Vec3._make(0, 0, 0)
  end
  -- Сборка 178: как в VU у SpatialEntity — transform = позиция тела (мод берёт controlledControllable.transform у пехоты).
  if k == "transform" then return rawget(t, "worldTransform") or LinearTransform() end
  if k == "typeInfo" then return { name = "ServerSoldierEntity" } end  -- Bot:_CheckForVehicleActions сверяет имя типа
  -- Сборка 203: ControllableEntity.teamId у солдата — команда его игрока
  if k == "teamId" or k == "defaultTeamId" then
    local p = rawget(t, "player")
    return p ~= nil and p.teamId or 0
  end
  -- Страховка (178): мод в «машинном» состоянии читает controlledControllable.physicsEntityBase; если движок бота в машину не
  -- посадил (нет тела / отказ), здесь солдат — не даём упасть всему UpdateManager:Update (прогон 178: 12 000 падений).
  if k == "physicsEntityBase" then
    local ph = rawget(t, "_physStub")
    if ph == nil then
      ph = setmetatable({ partCount = 0 }, { __index = function(_, kk)
        if kk == "position" then return t.transform.trans end
        if kk == "velocity" then return t.velocity end
        if kk == "internalHealth" then return 100.0 end
        return nil
      end })
      function ph:GetPart(j) return nil end
      function ph:GetPartTransform(j)
        local ok, q = pcall(function() return t.transform:ToQuatTransform(false) end)
        if ok then return q end
        return nil
      end
      rawset(t, "_physStub", ph)
    end
    return ph
  end
  -- Сборка 132: поза живая, из движка (мод целится и приседает по ней). Пока солдата нет — последняя заказанная.
  if k == "pose" then
    local p = rawget(t, "player")
    if EVBOTS_POSE_REAL == true and p ~= nil and p.nativePtr ~= nil and p.nativePtr ~= 0
        and NativeBridge.GetBotPose ~= nil then
      local okp, v = pcall(NativeBridge.GetBotPose, p.nativePtr)
      if okp and v ~= nil then return v end
    end
    return rawget(t, "poseLua") or CharacterPoseType.CharacterPoseType_Stand
  end
  return SoldierEntity[k]
end

-- Сборка 178: Is как у ControllableEntity VU (мод различает солдата и технику: controlledControllable:Is("ServerSoldierEntity")).
function SoldierEntity:Is(typeName)
  return typeName == "ServerSoldierEntity" or typeName == "SoldierEntity" or typeName == "Entity"
      or typeName == "ControllableEntity" or typeName == "PhysicsEntity" or typeName == "SpatialEntity"
      or typeName == "GameEntity"
end

-- ★ Сборка 203: SoldierEntity наследует ControllableEntity (VU-Docs server/type/soldierentity.md) — у солдата есть
-- GetPlayerInEntry/entryCount/teamId. Мод зовёт их у controlledControllable бота, которого считает водителем, а движок уже
-- высадил (прогон 202, 20:19:49: Vehicles.lua:78 «attempt to call a nil value (method 'GetPlayerInEntry')» из
-- GameDirector:TrySpawnInVehicle → обработчик Engine:Update мода падал). У VU солдат — своё единственное место 0 с игроком;
-- мод тогда получает «0 свободных мест» (Vehicles:GetVehicleName → солдат → nil). entryCount = 1 — одно место солдата (не сверено с движком).
function SoldierEntity:GetPlayerInEntry(entryId)
  if entryId == 0 then return rawget(self, "player") end
  return nil
end
function SoldierEntity:GetDataForEntry(entryId) return nil end
SoldierEntity.entryCount = 1

function SoldierEntity:SetPosition(position)
  if self.worldTransform and self.worldTransform.trans then
    self.worldTransform.trans = position
  end
end
-- Kill/ForceDead: у бота с игроком движка — настоящее убийство через мост
-- (ServerPlayerInstantSuicideMessage, см. NativeKillPlayer). Смерть затем
-- приходит обычным путём: наблюдение +0x3C4 -> soldier=nil -> Player:Killed.
local function KillNative(self)
  local p = self.player
  if p ~= nil and p.nativePtr ~= nil and NativeBridge ~= nil and
     NativeBridge.KillEnginePlayer ~= nil then
    local ok, r = pcall(NativeBridge.KillEnginePlayer, p.nativePtr)
    return ok and r == 1
  end
  return false
end
-- ★ Сборка 170: в VU Kill синхронный (Lua в потоке движка): после soldier:Kill() сразу player.soldier = nil и
-- Player:Killed уже разослан. fun-bots на этом строит уход ботов без людей: KillAll и в том же кадре
-- DestroyDisabledBots (удаляет только ботов с soldier = nil). У нас убийство ждало тика движка — в тот кадр боты
-- были «живы», не удалялись, и 11 игроков движка оставались без тел (прогон 169, 10:36: «11 из 64» в браузере).
-- Тело добивает тик моста (очередь убийств идёт раньше очереди удалений); второй Player:Killed не шлём (luaKilled).
local function KillNowLikeVu(self)
  local bp = self.player
  if not KillNative(self) or bp == nil or bp.soldier ~= self then return end
  bp.soldier = nil
  bp.hasSoldier = false
  bp.alive = false
  bp.luaKilled = true
  local okd, errd = pcall(Events._DispatchEngine, Events, 'Player:Killed', bp, nil, nil, nil, false, false, false, nil)
  if not okd then log("ERR ", "Player:Killed не разослан: " .. tostring(errd)) end
end
function SoldierEntity:Kill()
  -- Сборка 138: у солдата без тела в движке убивать нечего (таймер «неверный путь» GameDirector.lua:217 до старта раунда)
  if rawget(self, "noBody") then
    -- Сборка 139: бот без тела (спавн не прошёл) не умирал и не уходил — мод убирает лишних ботов через Kill и ждёт
    -- смерть (BotManager:KillAll → Bot:Kill → soldier:Kill, затем DestroyDisabledBots → DeletePlayer). Имитируем смерть:
    -- солдат снимается, уходит Player:Killed, мод делает бота неактивным и удаляет его.
    EVBotsBodylessKills = (EVBotsBodylessKills or 0) + 1
    self.hasDied = true
    local bp = self.player
    if bp ~= nil then
      bp.pendingSoldier = nil
      bp.soldier = nil
      bp.hasSoldier = false
      bp.alive = false
      -- Сборка 142: удаление игрока, который ещё стоит в отряде движка, роняло сервер (AV в цикле по отряду, запуск 06:41).
      -- Сборка 187: удаляем по DeletePlayer мода, но мост сначала выводит бота из отряда (leaveSquad движка).
      bp.nobodyRemoved = true
      bp.luaKilled = true  -- сборка 153: смерть уже разослана здесь — второй Player:Killed от движка не нужен
      if NativeBridge ~= nil and NativeBridge.TestLog ~= nil then
        pcall(NativeBridge.TestLog, "бот без тела убран модом (смерть без движка): " .. tostring(bp.name))
      end
      -- Сборка 145: мост забывает бота сразу (отложенный спавн/отряд не выполнятся — тела-призрака не будет).
      if bp.nativePtr ~= nil and bp.nativePtr ~= 0 and NativeBridge ~= nil and NativeBridge.ForgetEngineBot ~= nil then
        pcall(NativeBridge.ForgetEngineBot, bp.nativePtr)
      end
      pcall(Events._DispatchEngine, Events, 'Player:Killed', bp, nil, nil, nil, false, false, false, nil)
    end
    return
  end
  self.hasDied = true
  -- Сборка 91: кто убивает бота (застревание, баланс, прочее): строка в файле мода, который вызвал Kill
  local info = debug.getinfo(2, "Sl")
  local where = info and (info.short_src .. ":" .. tostring(info.currentline)) or "?"
  local name = self.player and self.player.name or "?"
  log("INFO ", "бот " .. tostring(name) .. " убит модом: " .. where)
  -- ★ Сборка 213, диагностика (прогон 212, XP5_002: 7 ботов с HQ США убиты GameDirector.lua:217 «inactivity on wrong paths»
  -- через 51–62 с жизни, все появились в одной точке). Где бот был: путь/точка мода, состояние, позиция тела сейчас.
  if where:find("GameDirector.lua:217", 1, true) and g_BotManager ~= nil then
    pcall(function()
      local b = g_BotManager:GetBotByName(name)
      if b == nil then return end
      local st = '?'
      if g_BotStates ~= nil then
        for k, v in pairs(g_BotStates.States) do if v == b.m_ActiveState then st = k end end
      end
      local t = self.worldTransform and self.worldTransform.trans
      -- ★ Сборка 241: почему мод счёл путь неверным (GameDirector.lua:197–209, только чтение): цели первой точки пути,
      -- активна ли каждая, база ли; сколько целей у мода всего; скорость тела (стоял ли бот).
      local pathInfo = '?'
      local gd, nc = g_GameDirector, g_NodeCollection
      if gd ~= nil and nc ~= nil then
        local okf, first = pcall(nc.GetFirst, nc, b._PathIndex)
        local objs = okf and EVBotsRawType(first) == 'table' and first.Data and first.Data.Objectives or nil
        local parts = {}
        if objs ~= nil then
          for _, on in ipairs(objs) do
            local oko, o = pcall(gd._GetObjectiveObject, gd, on)
            o = oko and o or nil
            parts[#parts + 1] = string.format('%s[%s%s]', tostring(on), o == nil and 'нет' or (o.active and 'акт' or 'неакт'),
                (o ~= nil and o.isBase) and ',база' or '')
          end
        end
        local oks, stat = pcall(gd.GetEnableStateOfPath, gd, objs or {})
        local nAll = EVBotsRawType(gd.m_AllObjectives) == 'table' and #gd.m_AllObjectives or -1
        pathInfo = string.format('цели пути {%s} статус %s, целей у мода %d, техника-путь %s', table.concat(parts, ' '),
            tostring(oks and stat or '?'), nAll, tostring(okf and EVBotsRawType(first) == 'table' and first.Data
              and first.Data.Vehicles ~= nil))
        -- ★ Сборка 242: развилки пути (PathSwitcher.lua:59 — сворачивает только в точке с Data.Links): сколько точек, какие
        -- пути по ссылкам, их цели и статус; цель бота (_Objective). Прогон 241: бот идёт (6 м/с) по «base us» и не сворачивает.
        local links, nPts = {}, 0
        for pi = 1, 400 do
          local okp, pt = pcall(nc.Get, nc, pi, b._PathIndex)
          if not okp or EVBotsRawType(pt) ~= 'table' then break end
          nPts = pi
          local ls = pt.Data and pt.Data.Links
          if EVBotsRawType(ls) == 'table' then
            for _, lid in ipairs(ls) do
              local okn, np = pcall(nc.Get, nc, lid)
              if okn and EVBotsRawType(np) == 'table' and #links < 8 then
                local okq, f2 = pcall(nc.GetFirst, nc, np.PathIndex)
                local o2 = okq and EVBotsRawType(f2) == 'table' and f2.Data and f2.Data.Objectives or {}
                local oks2, st2 = pcall(gd.GetEnableStateOfPath, gd, o2)
                local okb2, base2 = pcall(gd.IsBasePath, gd, o2)
                links[#links + 1] = string.format('т%d→путь %s {%s} ст %s%s', pi, tostring(np.PathIndex),
                    table.concat(o2, ','), tostring(oks2 and st2 or '?'), (okb2 and base2) and ' база' or '')
              end
            end
          end
        end
        pathInfo = pathInfo .. string.format(' | цель бота "%s", точек %d, развилки: %s', tostring(b._Objective), nPts,
            #links > 0 and table.concat(links, '; ') or 'нет')
      end
      local v = rawget(self, 'bridgeVelocity')
      log("INFO ", string.format("ДИАГ неверный путь: %s путь %s точка %s, состояние %s, поз (%.0f, %.0f, %.0f), цель-игрок %s | %s | скорость %.1f",
          tostring(name), tostring(b._PathIndex), tostring(b._CurrentWayPoint), st, t and t.x or 0, t and t.y or 0,
          t and t.z or 0, tostring(b._FollowTargetPlayer ~= nil), pathInfo,
          v ~= nil and v.x ~= nil and math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z) or -1))
    end)
  end
  -- Сборка 136: счёт убийств модом по месту вызова (тест vu-suicide: GameDirector.lua:217 — «неверный путь»)
  EVBotsModKills = EVBotsModKills or {}
  local key = (info and info.short_src or "?"):match("([^\\/]+)$") or "?"
  key = key .. ":" .. tostring(info and info.currentline or "?")
  EVBotsModKills[key] = (EVBotsModKills[key] or 0) + 1
  -- Сборка 149: причина смерти для журнала «ЖИЗНЬ» (смерть, пришедшая в ближайшие секунды, — от мода)
  if self.player ~= nil then self.player.lifeModKill = { where = key, at = EVBotsLifeWall or 0 } end
  -- ★ Сборка 153: тела в движке уже нет, а солдат в Lua остался (прогон 152, 19:17–19:21: BOT_Jayden — мост «забыл» его
  -- в 145-правке, мод снова посадил, после настоящей смерти смерть до мода не дошла; мод при переходе человека в другую
  -- команду 92 раза звал Kill по нему, лишний бот не уходил). Убивать в движке нечего — завершаем смерть как обычную:
  -- солдат снимается, Player:Killed (как в VU: у мёртвого soldier = nil), мод убирает лишнего.
  local bp = self.player
  if bp ~= nil and bp.soldier == self and bp.nativePtr ~= nil and bp.nativePtr ~= 0 and NativeBridge ~= nil
     and NativeBridge.ReadU32 ~= nil and (NativeBridge.ReadU32(bp.nativePtr, 0x3C4) or 0) == 0 then
    bp.soldier = nil
    bp.hasSoldier = false
    bp.alive = false
    log("INFO ", "бот " .. tostring(bp.name) .. ": Kill по солдату без тела в движке — смерть завершена в Lua (Player:Killed)")
    local okd, errd = pcall(Events._DispatchEngine, Events, 'Player:Killed', bp, nil, nil, nil, false, false, false, nil)
    if not okd then log("ERR ", "Player:Killed не разослан: " .. tostring(errd)) end
    return
  end
  KillNowLikeVu(self)
end
function SoldierEntity:ApplyCustomization(customizationData)
  -- Full native DCs retain VU's flags, visual groups and weapon removals.
  -- Keep the managed fun-bots staging path below for its legacy constructors.
  if type(customizationData) == 'userdata' then
    local p = self.player
    local ptr = p and playerPtr(p)
    local body = rawget(self, 'nativePtr')
    if p == nil or p.soldier ~= self or ptr == nil or body == nil or body == 0 then
      error('ApplyCustomization: this SoldierEntity has no current native body', 2)
    end
    if NativeBridge == nil or NativeBridge.ApplyHumanSoldierCustomization == nil then
      error('ApplyCustomization: native soldier customization bridge is unavailable', 2)
    end
    local target = rawget(self, '_nativeCustomizationTarget')
    if target == nil then
      local why
      target, why = NativeBridge.CaptureHumanSoldier(ptr, body)
      if target == nil then error('ApplyCustomization: ' .. tostring(why), 2) end
      rawset(self, '_nativeCustomizationTarget', target)
    end
    local ok, why, queued = NativeBridge.ApplyHumanSoldierCustomization(ptr, body, target, customizationData)
    if not ok then error('ApplyCustomization: ' .. tostring(why), 2) end
    -- Queued acceptance is not proof the native operation already completed.
    if not queued then self.customization = customizationData end
    return
  end
  if self.player ~= nil and (rawget(self.player, 'onlineId') or 0) ~= 0 and playerPtr(self.player) ~= nil then
    error('ApplyCustomization: a native human soldier requires native CustomizeSoldierData', 2)
  end
  self.customization = customizationData
  -- Сборка 98: оружие мода (слот -> имя ассета) в мост; в движок пишется при спавне
  local p = self.player
  if customizationData == nil or p == nil or NativeBridge == nil or NativeBridge.SetBotWeapons == nil then return end
  local w = customizationData.weapons
  local slots, count = {}, 0
  -- Сборка 152: навесы оружия (UnlockWeaponAndSlot.unlockAssets, как в VU) — слот -> {имена}; раньше отбрасывались
  local atts, attCount = {}, 0
  local n = (w ~= nil) and #w or 0
  for i = 1, n do
    local it = w[i]
    if it ~= nil and it.weapon ~= nil then
      local ok, nm = pcall(function() return it.weapon.name end)
      local sl = tonumber(it.slot)
      if ok and nm ~= nil and sl ~= nil then
        slots[sl] = tostring(nm)
        count = count + 1
        local list = {}
        pcall(function()
          local ua = it.unlockAssets
          for j = 1, (ua ~= nil and #ua or 0) do
            local a = ua[j]
            local okn, an = pcall(function() return a.name end)
            if okn and an ~= nil then list[#list + 1] = tostring(an) end
          end
        end)
        if #list > 0 then atts[sl] = list; attCount = attCount + #list end
      end
    end
  end
  p.selectedWeapons = slots
  p.selectedAttachments = atts
  if count > 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 then
    local ok, found, missing, attFound, attMissing = pcall(NativeBridge.SetBotWeapons, p.nativePtr, slots, atts)
    log('INFO ', 'ДИАГ оружие: ' .. tostring(p.name) .. ' слотов мода=' .. count .. ' найдено=' .. tostring(found)
        .. ' нет в памяти=' .. tostring(missing) .. ' слот0=' .. tostring(slots[0]) .. ' навесов мода=' .. attCount
        .. ' найдено=' .. tostring(attFound) .. ' нет=' .. tostring(attMissing)
        .. (atts[0] ~= nil and (' (слот0: ' .. table.concat(atts[0], ', ') .. ')') or ''))
  end
end
function SoldierEntity:RequestParachute()
  local player = self.player
  local ptr, body = player and playerPtr(player), rawget(self, 'nativePtr')
  if player == nil or player.soldier ~= self or ptr == nil or body == nil or body == 0 then
    return false, 'this SoldierEntity has no current native body'
  end
  if NativeBridge == nil or NativeBridge.RequestHumanParachute == nil then
    return false, 'native human parachute bridge is unavailable'
  end
  local target = rawget(self, '_nativeCustomizationTarget')
  if target == nil then
    local why
    target, why = NativeBridge.CaptureHumanSoldier(ptr, body)
    if target == nil then return false, why end
    rawset(self, '_nativeCustomizationTarget', target)
  end
  return NativeBridge.RequestHumanParachute(ptr, body, target)
end
function SoldierEntity:ForceDead() self.hasDied = true; KillNowLikeVu(self) end
function SoldierEntity:SingleStepEntry(entryId) self.controlledEntryId = entryId end
-- Сборка 132: настоящая поза. Запрос уходит в очередь моста (движок меняет позу в хуке тика); soldier.pose читается
-- из движка. immediately / overridePending мод всегда передаёт true, мост ставит позу сразу.
function SoldierEntity:SetPose(pose, immediately, overridePending)
  self.poseLua = pose
  local p = self.player
  if EVBOTS_POSE_REAL == true and p ~= nil and p.nativePtr ~= nil and p.nativePtr ~= 0
      and NativeBridge.SetBotPose ~= nil and pose ~= nil then
    pcall(NativeBridge.SetBotPose, p.nativePtr, pose)
  end
end
-- Entity:Destroy() — Bot:Destroy зовёт его перед PlayerManager:DeletePlayer. ★ Сборка 234: солдата уничтожает сам
-- движок при удалении игрока («покинул игру» с isAI = 0 — тот же менеджер уничтожения, что у VU Entity:Destroy).
-- Отличие от VU (реестр Б15): Destroy без последующего DeletePlayer солдата в движке не уничтожает.
function SoldierEntity:Destroy() self.destroyed = true end
function SoldierEntity:SetTransform(transform)
  local p = self.player
  local target = rawget(self, '_nativeCustomizationTarget')
  if target ~= nil or (p ~= nil and (rawget(p, 'onlineId') or 0) ~= 0) then
    local ptr, body = p and playerPtr(p), rawget(self, 'nativePtr')
    -- Потерянная human-привязка — отказ, в том числе в тест-хосте без движка.
    if p == nil or p.soldier ~= self or ptr == nil or body == nil or body == 0 then
      error('SetTransform: this SoldierEntity has no current native body', 2)
    end
    if EVBOTS_TP_REAL == false then error('SetTransform: EVBOTS_TP_REAL is disabled', 2) end
    if NativeBridge == nil or NativeBridge.SetHumanSoldierTransform == nil then
      error('SetTransform: native human soldier transform bridge is unavailable', 2)
    end
    if target == nil then
      local why
      target, why = NativeBridge.CaptureHumanSoldier(ptr, body)
      if target == nil then error('SetTransform: ' .. tostring(why), 2) end
      rawset(self, '_nativeCustomizationTarget', target)
    end
    local ok, why, queued = NativeBridge.SetHumanSoldierTransform(ptr, body, target, transform)
    if not ok then error('SetTransform: ' .. tostring(why), 2) end
    if not queued then
      self.worldTransform = transform
      self.bridgePrev = nil
      self.bridgeVelocity = Vec3._make(0, 0, 0)
    end
    return
  end
  self.worldTransform = transform
  -- Сборка 128: настоящий телепорт (VU: TeleportIfStuck переносит бота к цели). Раньше здесь был только Lua-объект, и
  -- мост на следующем тике возвращал бота по позиции движка: застрявший бот «телепортировался» 172 раза за 150 с и стоял.
  if EVBOTS_TP_REAL == true and p ~= nil and p.nativePtr ~= nil and p.nativePtr ~= 0
      and NativeBridge.TeleportBot ~= nil and transform ~= nil and transform.trans ~= nil then
    local tr = transform.trans
    pcall(NativeBridge.TeleportBot, p.nativePtr, tr.x, tr.y, tr.z)
    self.bridgePrev = nil
    self.bridgeVelocity = Vec3._make(0, 0, 0)
  end
end

-- Сборка 136: weaponsComponent как в VU (SoldierWeaponsComponent). Слот и патроны читаются из движка на лету
-- (NativeBridge.WeaponCurSlot / WeaponAmmo), запись primaryAmmo / secondaryAmmo уходит в движок (SetWeaponAmmo).
-- weapons[i] — слот i-1 (как в VU: weapons[1] = основное, [2] = пистолет, [7] = гранаты); nil, если оружия нет.
-- name — имя ассета, выбранного модом для слота (selectedWeapons). weaponFiring/gunSway пока нет (компенсация отдачи = 0).
local function bridgeOk(name) return NativeBridge ~= nil and NativeBridge[name] ~= nil end
local function soldierPtr(soldier)
  local p = rawget(soldier, "player")
  if p == nil or p.soldier ~= soldier then return nil, p end
  return playerPtr(p), p
end
local function NewWeaponProxy(soldier, slot)
  return setmetatable({}, {
    __index = function(_, k)
      local ptr, p = soldierPtr(soldier)
      if k == "primaryAmmo" or k == "secondaryAmmo" then
        if ptr == nil or not bridgeOk("WeaponAmmo") then return 0 end
        local ok, mag, res = pcall(NativeBridge.WeaponAmmo, ptr, slot)
        if not ok or mag == nil then return 0 end
        if k == "primaryAmmo" then return mag end
        return res
      elseif k == "name" then
        local sw = p and rawget(p, "selectedWeapons")
        return (sw and sw[slot]) or ""
      elseif k == "weaponFiring" then
        -- Сборка 186: GunSway из движка (компенсация отдачи мода); нет — nil, как у оружия без стрельбы
        if ptr == nil or not bridgeOk("GunSway") then return nil end
        local function dev(o)
          local ok, rp, ry, rr, rt, dp, dy, dr, dt = pcall(NativeBridge.GunSway, ptr, slot)
          -- Сборка 193: между проверкой weaponFiring и чтением отклонения оружие сменилось/солдат погиб — у VU объект
          -- отклонения всегда есть; nil ронял мод (прогон 192: BotAiming.lua:135, index a nil value s_CurrentRecoilDeviation).
          if not ok or rp == nil then return { pitch = 0, yaw = 0, roll = 0, transY = 0 } end
          EVBotsSwayN = (EVBotsSwayN or 0) + 1
          if (rp ~= 0 or ry ~= 0 or dp ~= 0) and (EVBotsSwayLogged or 0) < 5 then
            EVBotsSwayLogged = (EVBotsSwayLogged or 0) + 1
            log("INFO ", string.format("ДИАГ отдача: recoil %.4f/%.4f dispersion %.4f/%.4f (обращений %d)", rp, ry, dp, dy, EVBotsSwayN))
          end
          if o == 0 then return { pitch = rp, yaw = ry, roll = rr, transY = rt } end
          return { pitch = dp, yaw = dy, roll = dr, transY = dt }
        end
        local okc, rp0 = pcall(NativeBridge.GunSway, ptr, slot)
        if not okc or rp0 == nil then return nil end
        local sway = setmetatable({}, { __index = function(_, f)
          if f == "currentRecoilDeviation" then return dev(0) end
          if f == "currentDispersionDeviation" then return dev(1) end
          if f == "currentLagDeviation" then return { pitch = 0, yaw = 0, roll = 0, transY = 0 } end
          return nil
        end })
        return { gunSway = sway }
      end
      return nil
    end,
    __newindex = function(_, k, v)
      local ptr = soldierPtr(soldier)
      if ptr == nil or type(v) ~= "number" or not bridgeOk("SetWeaponAmmo") then return end
      if k == "primaryAmmo" then
        pcall(NativeBridge.SetWeaponAmmo, ptr, slot, math.floor(v), nil)
      elseif k == "secondaryAmmo" then
        pcall(NativeBridge.SetWeaponAmmo, ptr, slot, nil, math.floor(v))
      end
    end,
  })
end
local function NewWeaponsComponent(soldier)
  -- ★ Сборка 162: слоты, которые у этого солдата уже были. На смерти движок очищает слоты сразу, а мост
  -- узнаёт о потере солдата ~0,8 с спустя (прогон 161: смерть 04:50:02.198 → soldier=nil 02.991). В этом окне
  -- мод видел живого солдата без weapons[7] и падал (BotAttacking.lua:172) — весь UpdateManager:Update ботов
  -- срывался на каждой смерти. Бывший слот отдаём как оружие с 0 патронов: мод сам снимает GrenadeActive.
  local seen = {}
  local weapons = setmetatable({}, { __index = function(_, i)
    if type(i) ~= "number" then return nil end
    local ptr = soldierPtr(soldier)
    if ptr == nil or not bridgeOk("WeaponAmmo") then return nil end
    local ok, mag = pcall(NativeBridge.WeaponAmmo, ptr, i - 1)
    if not ok or mag == nil then
      if seen[i] then return NewWeaponProxy(soldier, i - 1) end
      return nil
    end
    seen[i] = true
    return NewWeaponProxy(soldier, i - 1)
  end })
  return setmetatable({ weapons = weapons }, { __index = function(t, k)
    if k == "currentWeaponSlot" then
      local ptr = soldierPtr(soldier)
      if ptr == nil or not bridgeOk("WeaponCurSlot") then return nil end
      local ok, s = pcall(NativeBridge.WeaponCurSlot, ptr)
      if ok then return s end
      return nil
    elseif k == "currentWeapon" then
      local s = t.currentWeaponSlot
      if s == nil then return nil end
      return t.weapons[s + 1]
    end
    return nil
  end })
end

-- Создать объект солдата (вызывается из Player:CreateSoldier).
local function NewSoldierEntity(transform)
  -- lineartransform, или простая таблица: мод трогает .trans/.forward.
  local w = transform or LinearTransform()
  local s = setmetatable({
    worldTransform = w,
    player = nil,
    detailedCollisionEnabled = false,
    physicsEnabled = false,
    forceInvisible = false,
    aimingEnabled = false,
    maxHealth = 100.0,
    waterLevel = 0.0,
    isManDown = false,
    isAlive = true,
    isDead = false,
    isDying = false,
    isInteractiveManDown = false,
    isFiring = false,
    isReloading = false,
    health = 100.0,
    poseLua = CharacterPoseType.CharacterPoseType_Stand,
    pendingPose = CharacterPoseType.CharacterPoseType_Stand,
    controlledEntryId = 0,
    customization = nil,
    hasDied = false,
  }, SoldierEntity)
  s.weaponsComponent = NewWeaponsComponent(s)
  return s
end

function Player.__eq(a, b)
  return a.id == b.id
end

-- Действия, требующие движка BF3: заглушки (движка в тест-хосте нет).
-- ★ Сборка 186: Player:Kick(причина) — через RCON сервера (admin.kickPlayer), как VU (мод выгоняет игрока с ником бота).
function Player:Kick(reason)
  local nm = rawget(self, 'name') or self.name
  if EVBotsRconSend == nil or nm == nil then log("WRN  ", "Player:Kick: RCON нет") return end
  local ok, resp = pcall(EVBotsRconSend, { 'admin.kickPlayer', tostring(nm), tostring(reason or '') }, 1500)
  log("INFO ", string.format("Player:Kick(%s, %q) -> %s", tostring(nm), tostring(reason or ''),
    (ok and type(resp) == 'table') and table.concat(resp, ' ') or tostring(resp)))
end
-- ★ Сборка 199: Ban* как VU (player.md 235–339) — RCON сервера banList.add <name|ip|guid> <id> <perm|seconds N> <причина>,
-- затем admin.kickPlayer (бан у BF3 не выгоняет сам). Guid у нас — EA-guid игрока, если известен; нет — бан по имени (журнал).
local function evbotsBan(self, kind, seconds, reason)
  local nm = rawget(self, 'name')
  local id = nm
  if kind == 'guid' then
    id = rawget(self, 'guid')
    if id == nil or tostring(id) == '' then kind, id = 'name', nm end
  elseif kind == 'ip' then
    id = rawget(self, 'ip')
    if id == nil or id == '' then
      log("WRN  ", "Player:BanByIp(" .. tostring(nm) .. "): IP не известен — бан по имени")
      kind, id = 'name', nm
    end
  end
  if EVBotsRconSend == nil or id == nil then log("WRN  ", "Player:Ban: RCON нет") return end
  local words = { 'banList.add', kind, tostring(id) }
  if seconds ~= nil then words[#words + 1] = 'seconds'; words[#words + 1] = tostring(math.max(1, math.floor(tonumber(seconds) or 1)))
  else words[#words + 1] = 'perm' end
  if reason ~= nil then words[#words + 1] = tostring(reason) end
  local ok, resp = pcall(EVBotsRconSend, words, 1500)
  log("INFO ", "Player:Ban (" .. table.concat(words, ' ') .. ") -> " .. ((ok and type(resp) == 'table') and table.concat(resp, ' ') or tostring(resp)))
  pcall(EVBotsRconSend, { 'banList.save' }, 1500)
  pcall(function() self:Kick(reason) end)
end
function Player:Ban(reason) evbotsBan(self, 'guid', nil, reason) end
function Player:BanTemporarily(seconds, reason) evbotsBan(self, 'guid', seconds, reason) end
function Player:BanByName(reason) evbotsBan(self, 'name', nil, reason) end
function Player:BanByNameTemporarily(seconds, reason) evbotsBan(self, 'name', seconds, reason) end
function Player:BanByIp(reason) evbotsBan(self, 'ip', nil, reason) end
function Player:BanByIpTemporarily(seconds, reason) evbotsBan(self, 'ip', seconds, reason) end
-- ★ Сборка 199: Spawn(spawnPoint, checkSpawnArea) как VU (player.md:340) — тело движка на этой точке спавна (тот же путь, что
-- событие «Spawn» точки: NativeBridge.SpawnEnginePlayer с 7-м аргументом). Человека (нет nativePtr) движок сажает сам — false.
function Player:Spawn(spawnPoint, checkSpawnArea)
  local ptr = rawget(self, 'nativePtr')
  local sp = type(spawnPoint) == 'table' and rawget(spawnPoint, '_ptr') or nil
  if ptr == nil or ptr == 0 or sp == nil or sp == 0 or NativeBridge == nil or NativeBridge.SpawnEnginePlayer == nil then
    log("WRN  ", "Player:Spawn(" .. tostring(self.name) .. "): нет игрока движка или точки спавна — false")
    return false
  end
  local ok, r = pcall(NativeBridge.SpawnEnginePlayer, ptr, nil, nil, nil, tonumber(self.squadId) or 0, 0, sp)
  if ok and r == 1 then self.hasSoldier = true; return true end
  return false
end
-- ★ Сборка 199: SelectWeapon(slot, weaponUnlockAsset, unlockAssets) как VU (player.md:357) — выбор оружия слота на следующий
-- спавн: тот же путь, что ApplyCustomization (NativeBridge.SetBotWeapons; в движок пишется при спавне).
function Player:SelectWeapon(slot, weaponUnlockAsset, unlockAssets)
  local sl = tonumber(slot)
  local okn, nm = pcall(function() return weaponUnlockAsset.name end)
  if sl == nil or not okn or nm == nil then log("WRN  ", "Player:SelectWeapon: нет слота или ассета") return end
  local slots = rawget(self, 'selectedWeapons') or {}
  local atts = rawget(self, 'selectedAttachments') or {}
  slots[sl] = tostring(nm)
  local list = {}
  for j = 1, (type(unlockAssets) == 'table' and #unlockAssets or 0) do
    local oka, an = pcall(function() return unlockAssets[j].name end)
    if oka and an ~= nil then list[#list + 1] = tostring(an) end
  end
  atts[sl] = (#list > 0) and list or nil
  self.selectedWeapons = slots
  self.selectedAttachments = atts
  local ptr = rawget(self, 'nativePtr')
  if ptr ~= nil and ptr ~= 0 and NativeBridge ~= nil and NativeBridge.SetBotWeapons ~= nil then
    pcall(NativeBridge.SetBotWeapons, ptr, slots, atts)
  end
end
-- Выбор класса: fun-bots зовёт SelectUnlockAssets(кит, {внешний вид}, перки). Класс берём из имени кита
-- ("Gameplay/Kits/USAssault"), мост ставит движку кит этого класса и ФРАКЦИИ КОМАНДЫ В ДВИЖКЕ (KIT-RESEARCH.md).
-- Внешний вид (сборка 97) и перки (сборка 107) мост пишет в движок сам — см. SetBotAppearance / SetBotPerks.
-- Имена элементов списка (внешний вид, перки) для диагностики: у объекта берём .name, иначе tostring
local function namesOf(list)
  if type(list) ~= 'table' then return tostring(list) end
  local out = {}
  for i = 1, #list do
    local ok, v = pcall(function() return list[i].name end)
    out[#out + 1] = (ok and v ~= nil) and tostring(v) or tostring(list[i])
  end
  return '[' .. table.concat(out, ', ') .. ']'
end
function Player:SelectUnlockAssets(kit, appearances, unlocks)
  if (rawget(self, 'onlineId') or 0) ~= 0 then
    local ptr = playerPtr(self)
    if ptr == nil or NativeBridge == nil or NativeBridge.SelectHumanUnlockAssets == nil then
      error('Player:SelectUnlockAssets: current native human player is unavailable', 2)
    end
    NativeBridge.SelectHumanUnlockAssets(ptr, rawget(self, 'name'), kit, appearances or {}, unlocks or {})
    self.selectedKit, self.selectedKitName = kit, kit.name
    self.selectedAppearanceName = appearances and appearances[1] and appearances[1].name or nil
    self.selectedUnlocks = unlocks or {}
    log('INFO ', 'Player:SelectUnlockAssets native human ' .. tostring(self.name) .. ' kit=' .. tostring(kit.name)
      .. ' appearance=' .. namesOf(appearances))
    return
  end
  local nm = nil
  if kit ~= nil then
    local ok, v = pcall(function() return kit.name end)
    if ok and v ~= nil then nm = tostring(v) end
  end
  self.selectedKitName = nm
  -- ★ Сборка 159: выбор мода (класс, внешность, перки) — мосту ПО ИМЕНИ сразу, до привязки nativePtr. В 158 перки и внешность
  -- уходили только после привязки, а тела садились по таймауту 30 с без выбора (одинаковые модели). Пустой список перков — тоже выбор.
  if NativeBridge ~= nil and NativeBridge.SetChoiceByName ~= nil and self.name ~= nil then
    local okap, appName = pcall(function()
      if type(appearances) == 'table' and appearances[1] ~= nil then return appearances[1].name end
      return nil
    end)
    local pl = {}
    if type(unlocks) == 'table' then
      for i = 1, #unlocks do
        local oku2, un2 = pcall(function() return unlocks[i].name end)
        if oku2 and un2 ~= nil then pl[#pl + 1] = tostring(un2) end
      end
    end
    pcall(NativeBridge.SetChoiceByName, tostring(self.name), nm, (okap and appName ~= nil) and tostring(appName) or nil, table.concat(pl, "\n"))
  end
  -- Сборка 93: что выбрал мод. Мост внешний вид и перки пока НЕ передаёт (см. комментарий выше):
  -- если в логе у разных ботов разный «внешний вид», а модели одинаковые — причина в этом.
  log('INFO ', 'ДИАГ внешность: ' .. tostring(self.name) .. ' кит=' .. tostring(nm)
      .. ' внешний вид=' .. namesOf(appearances) .. ' перки=' .. namesOf(unlocks))
  -- Сборка 97: внешность по выбору мода (первый элемент списка) — мост пишет объект в держатель бота
  if type(appearances) == 'table' and appearances[1] ~= nil then
    local oka, an = pcall(function() return appearances[1].name end)
    if oka and an ~= nil then
      self.selectedAppearanceName = tostring(an)
      if self.nativePtr ~= nil and self.nativePtr ~= 0 and NativeBridge ~= nil and NativeBridge.SetBotAppearance ~= nil then
        pcall(NativeBridge.SetBotAppearance, self.nativePtr, self.selectedAppearanceName)
      end
    end
  end
  -- Сборка 107: перки (отрядный и техники). selectedUnlocks — как в VU: мод читает у соседей по отряду
  -- l_PlayerUnlock.partition.name (в VU имя раздела в нижнем регистре), чтобы не брать одинаковый перк.
  -- Раньше список был пуст, и почти все боты получали SprintBoostL2. Сами перки мост пишет в движок на спавне.
  if type(unlocks) == 'table' then
    local sel, names = {}, {}
    for i = 1, #unlocks do
      local u = unlocks[i]
      local oku, un = pcall(function() return u.name end)
      if oku and un ~= nil then
        un = tostring(un)
        names[#names + 1] = un
        sel[#sel + 1] = setmetatable({ partition = { name = string.lower(un) }, name = un, _unlock = u },
            { __index = function(_, k) local okk, v = pcall(function() return u[k] end); if okk then return v end end })
      end
    end
    self.selectedUnlocks = sel
    self.selectedPerkNames = names
    if #names > 0 and self.nativePtr ~= nil and self.nativePtr ~= 0 and NativeBridge ~= nil
        and NativeBridge.SetBotPerks ~= nil then
      pcall(NativeBridge.SetBotPerks, self.nativePtr, names)
    end
  end
  if nm == nil then return end
  if self.nativePtr ~= nil and self.nativePtr ~= 0 and NativeBridge ~= nil and NativeBridge.SetBotKit ~= nil then
    pcall(NativeBridge.SetBotKit, self.nativePtr, nm)
  else
    self.pendingKit = nm  -- движкового игрока ещё нет: применим при привязке
  end
end
function Player:CreateSoldier(selectedKit, transform)
  -- Вызывается fun-bots в BotManager:SpawnBot.
  -- ★ Раньше здесь создавался объект В LUA, и движок такого игрока не
  -- мог посадить на карту. Теперь просим МОСТ создать настоящего
  -- SoldierEntity из того же blueprint (selectedKit), который мод сам
  -- достал через ResourceManager.
  self.selectedKit = selectedKit
  if self.nativePtr ~= nil and NativeBridge ~= nil then
    local ok, sol = pcall(NativeBridge.CreateEngineSoldier, self.nativePtr)
    if ok and sol ~= nil and sol ~= 0 then
      local s_Soldier = NewSoldierEntity(transform)
      s_Soldier.player = self
      s_Soldier.nativePtr = sol
      self.soldier = s_Soldier
      self.hasSoldier = true
      log("DBG ", "Player:CreateSoldier -> настоящий SoldierEntity движка ptr=" ..
          tostring(sol))
      return s_Soldier
    end
    log("DBG ", "Player:CreateSoldier -> движок не дал SoldierEntity")
  end
  -- Фолбэк: объект в Lua (как раньше), чтобы мод не падал.
  local s_Soldier = NewSoldierEntity(transform)
  s_Soldier.player = self
  self.soldier = s_Soldier
  self.hasSoldier = true
  log("DBG ", "Player:CreateSoldier -> SoldierEntity создан в Lua (фолбэк)")
  return s_Soldier
end
-- Сборка 185: 4-й аргумент (внутренний, не VU) — указатель ServerCharacterSpawnEntity: посадка через спавн самой точки
-- (Entity:FireEvent("Spawn"), VextEntityManager.cpp), как событие VU — движок сам сажает в привязанную технику.
function Player:SpawnSoldierAt(soldier, transform, pose, spawnPointPtr)
  -- Сборка 153: новая жизнь по заказу мода — флаг «смерть уже разослана» (убран без тела) больше не действует
  self.luaKilled = nil
  self._basePathPending = nil
  -- В движке это ставит солдата на карту. Сначала пробуем МОСТ: если у
  -- игрока есть нативный указатель, просим движок посадить солдата.
  if self.nativePtr ~= nil and NativeBridge ~= nil then
    -- Позиция из трансформа мода (точка узла карты): мост ставит солдата туда после спавна (стакание у HQ).
    -- Нулевой трансформ (LinearTransform по умолчанию) не передаём, иначе бот уйдёт в начало координат.
    local tr = transform and transform.trans or nil
    -- Отряд выбирает мод (как в VU): fun-bots пишет squadId перед спавном (_GetSquadToJoin). Мост ставит туда.
    local sq = tonumber(self.squadId) or 0
    local ok, spawned
    if spawnPointPtr ~= nil then
      ok, spawned = pcall(NativeBridge.SpawnEnginePlayer, self.nativePtr, nil, nil, nil, sq, tonumber(pose) or 0, spawnPointPtr)
    elseif tr ~= nil and (tr.x ~= 0 or tr.z ~= 0) then
      ok, spawned = pcall(NativeBridge.SpawnEnginePlayer, self.nativePtr, tr.x, tr.y, tr.z, sq, tonumber(pose) or 0)
    else
      ok, spawned = pcall(NativeBridge.SpawnEnginePlayer, self.nativePtr, nil, nil, nil, sq, tonumber(pose) or 0)
    end
    if ok and spawned == 1 then
      self.hasSoldier = true
      log("DBG ", "Player:SpawnSoldierAt -> движок посадил солдата для " ..
          tostring(self.name))
    else
      log("DBG ", "Player:SpawnSoldierAt -> движок отказал, остаюсь на Lua")
    end
  end
  -- Движковый игрок ещё не создан (мост в очереди): запоминаем позицию, мост передаст её при появлении ptr.
  -- Нулевой трансформ не запоминаем (как и при передаче в мост).
  local tr0 = transform and transform.trans or nil
  if (self.nativePtr == nil or NativeBridge == nil) and tr0 ~= nil and (tr0.x ~= 0 or tr0.z ~= 0) then
    self.spawnPos = { x = tr0.x, y = tr0.y, z = tr0.z }
  end
  if self.nativePtr == nil or NativeBridge == nil then
    self.spawnSquad = tonumber(self.squadId) or 0
    -- Сборка 148 (как VU): мод попросил тело до привязки к движку — мост повторит SpawnSoldierAt при привязке,
    -- после кита/внешности/оружия/перков (тело раньше выбора мода — одна иконка и нет ника, прогоны 145–147).
    self.spawnRequested = true
  end
  -- Фолбэк: фиксируем transform/pose на Lua-объекте (как раньше).
  -- ВАЖНО: hasSoldier ставим в ЛЮБОМ случае. Если оставить его nil при
  -- неудачном спавне, мод уходит в ветку "игрок без солдата" и доходит
  -- до нативного кода с некорректным состоянием -> 0xC0000005.
  if soldier then
    soldier.worldTransform = transform or soldier.worldTransform
    if pose then soldier.poseLua = pose end
    soldier.player = self
    -- Сборка 135: как в VU. В veniceext.dll player.soldier = [ServerPlayer+0x3C4] (0 -> nil), hasSoldier = (+0x3C4 != 0),
    -- alive = движковый isAlive (разбор re\vu\, VU-AUDIT.md). Пока движок не дал тело (до старта раунда), у бота нет
    -- солдата: мод его не ведёт (нет обхода препятствий, огня в пустоту, самоубийства на базе GameDirector.lua:217).
    -- Солдат отдаётся моду на тике, когда тело появилось (см. Engine:Update). EVBOTS_SOLDIER_VU=0 — по-старому.
    -- Сборка 136: мод зовёт SpawnSoldierAt раньше, чем мост привязал бота к движку (nativePtr ещё nil, прогон 135) —
    -- такой бот тоже без тела, солдат откладывается.
    local noBody = (self.nativePtr == nil or self.nativePtr == 0)
    if not noBody and NativeBridge ~= nil and NativeBridge.ReadU32 ~= nil then
      noBody = (NativeBridge.ReadU32(self.nativePtr, 0x3C4) or 0) == 0
    end
    if EVBOTS_SOLDIER_VU ~= false and self.onlineId == 0 and NativeBridge ~= nil and noBody then
      -- Сборка 138: объект солдата мод получает СРАЗУ (в VU спавн синхронный: BotSpawner.lua:1552 при soldier == nil
      -- выходит и не вызывает ApplyCustomization -> бот без оружия до первого респавна, прогон 137). Но тела ещё нет:
      -- hasSoldier/alive = false, noBody = true (Kill такого солдата — пусто), пока движок не даст тело (Engine:Update).
      soldier.noBody = true
      self.pendingSoldier = soldier
      self.soldier = soldier
      self.hasSoldier = false
      self.alive = false
    else
      self.pendingSoldier = nil
      self.soldier = soldier
      self.hasSoldier = true
      self.alive = true
    end
  end
end
function Player:SetSquadLeader(leader, squadPrivate)
  self.isSquadLeader = leader and true or false
  self.isSquadPrivate = squadPrivate and true or false
  -- ★ Сборка 186: лидер отряда — в движок через RCON сервера (squad.leader <команда> <отряд> <имя>,
  -- fb::BFServerAdministration::squadLeaderCommand, re\method_index.txt), как VU: значок лидера, спавн на лидере.
  self._leaderSent = nil
  self._leaderTries = 0
  if leader then EVBotsSendSquadLeader(self) end
end
-- Отправка лидера в движок; бот ещё не создан в движке (InvalidPlayerName) — повтор из тика моста (EVBotsSquadLeaderTick).
function EVBotsSendSquadLeader(p)
  if EVBotsRconSend == nil or rawget(p, '_leaderSent') then return end
  local t, sq, nm = rawget(p, 'teamId') or p.teamId, rawget(p, 'squadId') or p.squadId, rawget(p, 'name') or p.name
  if t == nil or sq == nil or nm == nil or t <= 0 or sq <= 0 then return end
  if rawget(p, 'onlineId') == 0 and (p.nativePtr == nil or p.nativePtr == 0) then return end  -- бота в движке ещё нет
  local tries = (rawget(p, '_leaderTries') or 0) + 1
  if tries > 5 then return end
  p._leaderTries = tries
  local ok, resp = pcall(EVBotsRconSend, { 'squad.leader', tostring(t), tostring(sq), tostring(nm) }, 1500)
  local okResp = ok and type(resp) == 'table' and resp[1] == 'OK'
  if okResp then p._leaderSent = true end
  EVBotsSquadLeaderN = (EVBotsSquadLeaderN or 0) + 1
  if EVBotsSquadLeaderN <= 12 then
    log("INFO ", string.format("Player:SetSquadLeader(%s, команда %d, отряд %d) -> %s", tostring(nm), t, sq,
      (ok and type(resp) == 'table') and table.concat(resp, ' ') or tostring(resp)))
  end
end
-- ★ Сборка 199: AttachSoldier(soldier) — у нас тело создаёт движок при посадке (CreateSoldier отдаёт Lua-объект): посадка по
-- трансформу этого солдата, как SpawnSoldierAt.
function Player:AttachSoldier(soldier)
  if soldier == nil then return end
  local tr = rawget(soldier, 'worldTransform') or LinearTransform()
  return self:SpawnSoldierAt(soldier, tr, rawget(soldier, 'poseLua') or 0)
end
-- ★ Сборка 199: EnableInput(action, enable) как VU (player.md:442) — у ботов действие блокируется в вводе моста (SetLevel пишет 0).
-- У человека ввод приходит от клиента — здесь не меняется (нужен клиентский VEXT), пишем в журнал.
EVBotsInputOff = EVBotsInputOff or {}
function Player:EnableInput(action, enable)
  local ptr = rawget(self, 'nativePtr')
  local a = math.tointeger(tonumber(action))
  if a == nil then return end
  if ptr == nil or ptr == 0 then
    log("INFO ", "Player:EnableInput(" .. tostring(self.name) .. ", " .. a .. ", " .. tostring(enable) .. "): у человека не поддержано")
    return
  end
  EVBotsInputOff[ptr] = EVBotsInputOff[ptr] or {}
  EVBotsInputOff[ptr][a] = (enable == false) or nil
  if enable == false and NativeBridge ~= nil and NativeBridge.SetInputLevel ~= nil then pcall(NativeBridge.SetInputLevel, ptr, a, 0) end
end
-- Send the stock fade message only to this player's active connection.
function Player:Fade(fadeTime, fadeToBlack)
  return nativePlayerFade(playerPtr(self) or 0, fadeTime, fadeToBlack)
end
-- Сборка 178: посадка/выход в очереди моста (движок меняет состояние в тике), как VU: EnterVehicle(entity, entryId),
-- ExitVehicle(флаг, флаг). Объект машины — из EntityManager (ServerVehicleEntity, поле _ptr).
function Player:EnterVehicle(entity, entryId)
  local ptr = playerPtr(self)
  local ent = type(entity) == "table" and rawget(entity, "_ptr") or nil
  -- ★ Сборка 244 (прогон 243: 30 отказов «игрок nil, машина есть»): мод сажает бота в технику сразу, как получил солдата
  -- (BotSpawner.lua:179–188), а игрок движка ещё не привязан к Lua-игроку (очередь создания). У VU игрок уже есть. Посадку
  -- запоминаем и повторяем при привязке nativePtr (Player.__newindex) — раньше она терялась, мод считал бота в машине, путь 0,
  -- бот стоял и через 50 с убивался (GameDirector.lua:217).
  if ptr == nil and ent ~= nil and (rawget(self, 'onlineId') or 0) == 0 then
    rawset(self, '_enterLater', { entity = entity, seat = entryId or 0 })
    -- место занято сразу, как у VU (Vehicle:GetPlayerInEntry, VextEntityManager.cpp): прогон 243 — 16 ботов подряд «садились»
    -- на место 0 одной и той же машины и стояли
    EVBotsSeatLater = EVBotsSeatLater or {}
    EVBotsSeatLater[ent] = EVBotsSeatLater[ent] or {}
    EVBotsSeatLater[ent][entryId or 0] = { p = self, t = os.clock() }
    -- ★ Сборка 250 (прогон 249, 10:38: пилоты Mi28/AH1Z сидели 50 с на месте 0 и мод их убил, GameDirector.lua:217, состояние Moving):
    -- мод сразу после EnterVehicle зовёт UpdateVehicleMovableId (BotActions.lua:333) и по controlledControllable ставит состояние
    -- «пилот/водитель» (Bot.lua:582–594). У VU посадка синхронна; у нас без заказа здесь controlledControllable был солдат, и бот
    -- оставался пешим. Заказ виден сразу, как в обычной ветке ниже (later — место держит EVBotsSeatLater, не очередь моста).
    rawset(self, '_vehPending', { kind = 1, ent = entity, seat = entryId or 0, t = os.clock(), later = true })
    EVBotsEnterLater = (EVBotsEnterLater or 0) + 1
    if EVBotsEnterLater <= 30 then
      log("INFO ", string.format("Player:EnterVehicle(%s, место %s): игрок движка ещё не привязан — посадка при привязке",
        tostring(rawget(self, 'name')), tostring(entryId)))
    end
    return
  end
  if ptr == nil or ent == nil or NativeBridge == nil or NativeBridge.EnterVehicle == nil then
    -- ★ Сборка 243: прогон 242 — 30 отказов, мод при этом считает бота в машине (BotActions.lua:328 не проверяет итог), путь 0,
    -- бот стоит и мод убивает его через 50 с. Что именно пусто — в журнал (WRN виден и в тихом режиме).
    EVBotsEnterFail = (EVBotsEnterFail or 0) + 1
    if EVBotsEnterFail <= 30 then
      local keys = {}
      if type(entity) == "table" then
        for k in pairs(entity) do if #keys < 8 then keys[#keys + 1] = tostring(k) end end
      end
      log("WRN  ", string.format("Player:EnterVehicle(%s, место %s): отказ — игрок %s, машина %s (тип %s, _kind %s, имя %s, поля [%s])",
        tostring(rawget(self, 'name')), tostring(entryId), tostring(ptr), tostring(ent), type(entity),
        type(entity) == "table" and tostring(rawget(entity, '_kind')) or '-', type(entity) == "table" and tostring(rawget(entity, 'name')) or '-',
        table.concat(keys, ',')))
    end
    return
  end
  -- Сборка 235: мост отказывает, как VU до вызова движка (не машина, не заспавнена, нет места) — тогда посадка не ждёт.
  local ok, accepted = pcall(NativeBridge.EnterVehicle, ptr, ent, entryId or 0)
  if ok and accepted then
    rawset(self, '_vehPending', { kind = 1, ent = entity, seat = entryId or 0, t = os.clock() })
  end
end
function Player:ExitVehicle(a, b)
  local ptr = playerPtr(self)
  if ptr == nil or NativeBridge == nil or NativeBridge.ExitVehicle == nil then return end
  -- сборка 186: в лог — кто и откуда вылезает (вопрос пользователя «вылезают ли из подбитой техники»), до 200 строк.
  -- Сборка 187: читать до _vehPending — с ним controlledControllable уже солдат (здоровье было -1).
  EVBotsExitLogN = (EVBotsExitLogN or 0) + 1
  if EVBotsExitLogN <= 200 then
    local hp = '?'
    pcall(function()
      local cc = self.controlledControllable
      if cc ~= nil then hp = string.format('%.0f', PhysicsEntity(cc).internalHealth or -1) end
    end)
    log("INFO ", string.format("ВЫХОД из техники: %s, здоровье машины %s, место %s", tostring(self.name), hp,
      tostring(self.controlledEntryId)))
  end
  rawset(self, '_vehPending', { kind = 2, t = os.clock() })
  pcall(NativeBridge.ExitVehicle, ptr, a == true, b == true)
end

-- Создать объект Player по полям документации.
-- ★ Фабрика объекта ввода объявлена ЗДЕСЬ, до NewPlayer. Раньше NewPlayer
-- стоял выше блока EntryInput, и ``EntryInputMt`` внутри него резолвился
-- как ГЛОБАЛЬНОЕ имя (nil) — Lua-ошибка при создании каждого игрока:
--     attempt to index a nil value (global 'EntryInputMt')
-- Поэтому forward declaration, а ссылка на EntryInputMt — уже ниже.
local EntryInputFactory = nil

local function NewPlayer(id, name, team, squad)
  local p = {
    name = name,
    id = id,
    onlineId = 0,           -- 0 => бот (как в fun-bots)
    guid = nil,
    accountGuid = nil,
    ip = "",
    ping = 0,
    alive = true,
    isSquadLeader = false,
    isSquadPrivate = false,
    hasSoldier = false,
    soldier = nil,
    -- corpse — живое поле через Player.__index (сборка 185), в таблицу не кладём
    _teamId = team or 0,
    _squadId = squad or 0,
    _observedTeamId = team or 0,
    _pendingTeamId = team or 1,
    _pendingSquadId = squad or 0,
    isAllowedToSpawn = true,
    -- isAllowedToSpawnOn — живое поле через Player.__index (сборка 197, из движка), в таблицу не кладём
    selectedKit = nil,
    -- (Сборка 171: score/kills/deaths — живые, через Player.__index.)
    -- ★ ВНИМАНИЕ: было nil. Из-за этого fun-bots не мог управлять ботом:
    -- он командует ``p_Bot.m_Player.input:SetLevel(EIAForward, 1)``, а при
    -- nil это ошибка Lua и бот просто стоит.
    -- Через EntryInputFactory (объявлен выше NewPlayer): прямая ссылка на
    -- EntryInputMt здесь не работает — этот local объявлен НИЖЕ.
    input = (function()
      -- pcall: если создание объекта ввода почему-то не выйдет, игрок всё
      -- равно должен создаться. Ошибка ввода не должна ломать матч.
      if EntryInputFactory == nil then return {} end
      local ok, obj = pcall(EntryInputFactory)
      if ok and obj ~= nil then return obj end
      return {}
    end)(),
    -- (Сборка 178: controlledEntryId / attachedControllable / controlledControllable — живые, через Player.__index.)
    customization = nil,
    selectedUnlocks = {},
    visualUnlocks = {},
    weapons = {},
    weaponUnlocks = {},
  }
  -- Сборка 199: чей ввод — для EntryInput:GetLevel у людей (ввод человека читается из движка, у VU — настоящий ввод)
  if type(p.input) == 'table' then rawset(p.input, '_owner', p) end
  return setmetatable(p, Player)
end

-- ── EntryInput (shared\type\entryinput.md) ──────────────────────────────────
-- Конструктор, вызывается как EntryInput().
-- С точки зрения Lua — таблица-объект с метатаблицей, у которой есть __call,
-- поэтому EntryInput() создаёт итог (см. VectUtils/PropertyBuilder по стилю).
-- Сборка 115: сколько раз мод ставил ненулевой / нулевой уровень ввода (по номеру действия)
local setLevelNonZero, setLevelZero, setLevelByAction = 0, 0, {}
local aimVariant = 0  -- сборка 118: 0 — не передавать прицел; 1 — только флаг; 2 — yaw@1F0 pitch@1F4; 3 — yaw@1F4 pitch@1F0
local EntryInputMt = {}
EntryInputMt.__index = function(self, key)
  if key == 'GetLevel' then
    -- ★ Сборка 199: как VU (entryinput.md — уровень ввода) — у человека читаем движок (объект ввода [игрок+0x3C0]: действия
    -- 0..9 — float, 10+ — биты маски), иначе мод не видел ввод человека: «следуй за мной» (BotMovement.lua:740–743),
    -- mimic/mirror (1157–1167), запись путей (NodeEditor.lua:1323–1349). У ботов — записанное модом (его же мы пишем в движок).
    return function(_, action)
      local o = rawget(self, '_owner')
      if o ~= nil and (rawget(o, 'onlineId') or 0) ~= 0 and NativeBridge ~= nil and NativeBridge.InputLevelOf ~= nil
          and EVBotsHumanPtr ~= nil and type(action) == 'number' then
        local okp, hp = pcall(EVBotsHumanPtr, o)  -- сборка 202: ввод своего человека, не первого
        if okp and hp ~= nil and hp ~= 0 then
          local ok, v = pcall(NativeBridge.InputLevelOf, hp, math.tointeger(action) or 0)
          if NativeBridge.InputAimOf ~= nil then
            local oka, y, pt = pcall(NativeBridge.InputAimOf, hp)
            if oka and y ~= nil then
              rawset(self, 'authoritativeAimingYaw', y)
              rawset(self, 'authoritativeAimingPitch', pt)
            end
          end
          if ok and v ~= nil then return v end
        end
      end
      return self._levels[action] or 0.0
    end
  elseif key == 'SetLevel' then
    -- ★ Команда ввода уходит в МОСТ, а не остаётся в таблице Lua.
    -- fun-bots управляет ботом именно так:
    --     p_Bot.m_Player.input:SetLevel(EntryInputActionEnum.EIAForward, 1)
    -- Пока адрес компонента ввода движка не разобран, мост ЛОГИРУЕТ вызовы
    -- (чтобы увидеть реальные значения и номера действий), а значение
    -- сохраняется в Lua — чтобы мод не падал.
    return function(_, action, level)
      -- Сборка 199: Player:EnableInput(action, false) у бота — действие заблокировано
      local nat = rawget(self, '_native')
      if nat ~= nil and EVBotsInputOff ~= nil and EVBotsInputOff[nat] ~= nil and EVBotsInputOff[nat][action] then level = 0 end
      self._levels[action] = level
      -- Сборка 178: знак руля в технике (EVBOTS_VEH_YAW_SIGN=-1 инвертирует EIAYaw водителя); в Lua-уровне остаётся значение мода.
      if action == 4 and level ~= nil and EVBOTS_VEH_YAW_SIGN == -1 and self._native ~= nil and NativeBridge ~= nil
          and NativeBridge.PlayerVehicle ~= nil then
        local okv, att = pcall(NativeBridge.PlayerVehicle, self._native)
        if okv and att ~= nil and att ~= 0 then level = -level end
      end
      if level ~= nil and level ~= 0 then
        setLevelNonZero = setLevelNonZero + 1
        setLevelByAction[action] = (setLevelByAction[action] or 0) + 1
      else
        setLevelZero = setLevelZero + 1
      end
      if self._native ~= nil and NativeBridge ~= nil then
        pcall(NativeBridge.SetInputLevel, self._native, action, level)
      end
    end
  end
  return nil
end

EntryInputMt.__call = function()
  return setmetatable({
    deltaTime = 0.0,
    authoritativeAimingYaw = 0.0,
    authoritativeAimingPitch = 0.0,
    authoritativeMovementPenalty = 0.0,
    authoritativeCameraPosition = nil,
    authoritativeMovementPosition = nil,
    authoritativeMovementVelocity = nil,
    flags = 0,
    zoomLevel = 0,
    _levels = {},
  }, EntryInputMt)
end

local EntryInput = {}
_G.EntryInput = setmetatable(EntryInput, EntryInputMt)

-- Заполняем forward declaration из NewPlayer (см. комментарий там).
EntryInputFactory = function()
  return EntryInputMt.__call()
end

function EntryInput:Create()
  return EntryInputMt.__call()
end

-- ── PlayerManager методы ────────────────────────────────────────────────────
function PlayerManager:GetPlayers()
  return players
end

function PlayerManager:GetPlayerById(id)
  return playersById[id]
end

function PlayerManager:GetPlayerByOnlineId(onlineId)
  for i = 1, #players do
    if players[i].onlineId == onlineId then
      return players[i]
    end
  end
  return nil
end

function PlayerManager:GetPlayerByGuid(guid)
  if guid == nil then
    return nil
  end
  for i = 1, #players do
    local p = players[i]
    if p.guid ~= nil and tostring(p.guid) == tostring(guid) then
      return p
    end
  end
  return nil
end

function PlayerManager:GetPlayerByName(name)
  for i = 1, #players do
    if players[i].name == name then
      return players[i]
    end
  end
  return nil
end

function PlayerManager:GetPlayersByName(name)
  local out = {}
  for i = 1, #players do
    if players[i].name == name then
      out[#out + 1] = players[i]
    end
  end
  return out
end

function PlayerManager:GetPlayerCount()
  return #players
end

function PlayerManager:GetPlayersByTeam(team)
  local out = {}
  for i = 1, #players do
    if players[i].teamId == team then
      out[#out + 1] = players[i]
    end
  end
  return out
end

function PlayerManager:GetPlayersBySquad(team, squad)
  local out = {}
  for i = 1, #players do
    local p = players[i]
    if p.teamId == team and p.squadId == squad then
      out[#out + 1] = p
    end
  end
  return out
end

function PlayerManager:GetSquadLeader(team, squad)
  for i = 1, #players do
    local p = players[i]
    if p.teamId == team and p.squadId == squad and p.isSquadLeader then
      return p
    end
  end
  return nil
end

function PlayerManager:CreatePlayer(name, team, squad)
  if type(name) ~= "string" then
    error("PlayerManager:CreatePlayer: имя должно быть строкой", 2)
  end
  local id = nextId
  nextId = nextId + 1
  local p = NewPlayer(id, name, team, squad)
  playersById[id] = p
  players[#players + 1] = p
  -- МОСТ в движок: создаём настоящего ServerPlayer. Без этого игрок
  -- существует только в Lua и в игре не виден.
  -- Сборка 139: настоящий человек уже есть в движке — второго игрока не создаём (он в списке мода сам).
  local isHumanName = false
  if NativeBridge ~= nil and NativeBridge.RealPlayerName ~= nil then
    local okn, rn = pcall(NativeBridge.RealPlayerName)
    isHumanName = okn and rn ~= nil and rn == name
  end
  if NativeBridge ~= nil and not isHumanName then
    local ok, ptr = pcall(NativeBridge.CreateEnginePlayer, name, team or 1, squad or 0)
    if ok and ptr ~= nil and ptr ~= 0 then
      p.nativePtr = ptr
      PlayerManager._AdoptEngineId(p, ptr)
      if rawget(p, '_enterLater') ~= nil then Player.__newindex(p, 'nativePtr', ptr) end  -- 244: посадка после привязки
      -- ★ Привязываем объект ввода к нативному игроку: без этого
      -- ``input:SetLevel`` от модa уходил бы в никуда.
      if p.input ~= nil then p.input._native = ptr end
    else
      -- ВНИМАНИЕ: это сырой C++-строка R"LUA(...)", поэтому экранировать
      -- кавычки НЕЛЬЗЯ (двойной слэш закрывает строку Lua раньше времени
      -- и весь чанк перестаёт парситься). Поэтому без кавычек в тексте.
      -- Сборка 178: не ошибка — пока в мире нет точек спавна (человек ещё грузится), движковый игрок ждёт в очереди моста.
      log("INFO ", "PlayerManager:CreatePlayer: движковый игрок для " .. name .. " ещё не создан (ждёт точку спавна, в очереди)")
    end
  end
  log("DBG ", "PlayerManager:CreatePlayer(\"" .. name .. "\", team=" ..
      tostring(team) .. ") -> id=" .. tostring(id) .. " (бот)")
  return p
end

function PlayerManager:DeletePlayer(player)
  if player == nil or player.id == nil then
    return
  end
  if player.onlineId == 0 then  -- сборка 149: журнал «ЖИЗНЬ»
    local lf = player.life
    log("INFO ", "ЖИЗНЬ " .. tostring(player.name) .. ": удалён модом (тел " .. tostring(lf and lf.bodies or 0)
        .. ", смертей " .. tostring(lf and lf.deaths or 0) .. ", без тела в движке=" .. tostring(player.nobodyRemoved == true) .. ")")
  end
  local id = player.id
  playersById[id] = nil
  if player._tmpId ~= nil and playersById[player._tmpId] == player then playersById[player._tmpId] = nil end  -- ★ 231
  local eidDel = rawget(player, '_engineId')  -- ★ 247: псевдоним id движка
  if eidDel ~= nil and playersById[eidDel] == player then playersById[eidDel] = nil end
  for i = 1, #players do
    if players[i].id == id then
      table.remove(players, i)
      break
    end
  end
  -- Бот с игроком движка: убрать его и в движке (иначе «призрак» в табе, мод создаёт нового).
  -- Сборка 187: бот без тела тоже удаляется из движка, как VU DeletePlayer (до 187 оставался призраком в счёте:
  -- SQDM → CQ, FatalAssert «Invalid team 4»). Мост перед deletePlayer выводит его из отряда движка (leaveSquad).
  if player.nobodyRemoved and NativeBridge ~= nil and NativeBridge.TestLog ~= nil then
    pcall(NativeBridge.TestLog, "бот без тела: удаляем из движка (leaveSquad + deletePlayer): " .. tostring(player.name))
  end
  if player.nativePtr ~= nil and player.nativePtr ~= 0 and NativeBridge ~= nil
      and NativeBridge.DeleteEnginePlayer ~= nil then
    pcall(NativeBridge.DeleteEnginePlayer, player.nativePtr)
    player.nativePtr = nil
  end
  log("DBG ", "PlayerManager:DeletePlayer(id=" .. tostring(id) .. ")")
end

-- Смерть ботов (13.10): мост следит за солдатом (+0x3C4) и отдаёт очередь умерших.
-- Обнуляем Player.soldier (иначе StateIdle мода не уходит в респавн) и шлём
-- Player:Killed, на который подписан fun-bots.
if Events ~= nil then
  local firstTick = true
  local teamSyncClock = 0
  local syncTicks = 0
  -- ★ Сборка 164: профиль блоков тика моста (раз в 20 с — топ в лог). Отметка закрывает предыдущий блок.
  EVBotsBPS = EVBotsBPS or { at = os.clock(), sum = {}, max = {}, cur = nil, t = 0 }
  function EVBotsBP(name)
    local S = EVBotsBPS
    local now = os.clock()
    if S.cur ~= nil then
      local dt = now - S.t
      S.sum[S.cur] = (S.sum[S.cur] or 0) + dt
      if dt > (S.max[S.cur] or 0) then S.max[S.cur] = dt end
    end
    S.cur, S.t = name, now
    if name == nil and now - S.at >= 20 then
      local rows = {}
      for k, v in pairs(S.sum) do rows[#rows + 1] = { k = k, v = v } end
      table.sort(rows, function(a, b) return a.v > b.v end)
      local parts = {}
      for i = 1, math.min(8, #rows) do
        parts[#parts + 1] = string.format('%s %.0f мс (макс %.0f)', rows[i].k, rows[i].v * 1000, (S.max[rows[i].k] or 0) * 1000)
      end
      -- Сборка 191: память Lua (паузы тика могут быть от сборщика мусора)
      log('INFO ', string.format('ДИАГ тик моста %.0f с: %s | память Lua %.0f МБ', now - S.at, table.concat(parts, '; '),
          collectgarbage('count') / 1024))
      S.at, S.sum, S.max = now, {}, {}
    end
  end
  Events:Subscribe('Engine:Update', function(deltaTime)
    if NativeBridge == nil or NativeBridge.PollDeadBots == nil then
      return
    end
    if firstTick then
      firstTick = false
      log("INFO ", "мост: подписка Engine:Update работает (смерть/привязка ботов)")
    end
    -- ★ Сборка 90: мод судит о движении по позиции и скорости солдата (BotMovement: стоит ли бот, не застрял ли).
    -- Раньше позиция солдата оставалась запланированной точкой, а скорость — заглушкой 0, и мод считал каждого бота
    -- стоящим и убивал его как «got stuck». Теперь каждый тик берём позицию из движка, скорость — по смещению за тик.
    EVBotsBP('позиции')
    if NativeBridge.SoldierPos ~= nil and deltaTime ~= nil and deltaTime > 0 then
      for i = 1, #players do
        local p = players[i]
        -- Сборка 135 (как в VU): отложенный солдат отдаётся моду, как только движок дал тело ([игрок+0x3C4] != 0).
        local bodyReady = rawget(p, 'pendingSoldier') ~= nil and p.nativePtr ~= nil and p.nativePtr ~= 0  -- 245: без __index
            and (NativeBridge.ReadU32(p.nativePtr, 0x3C4) or 0) ~= 0
        -- ★ Сборка 201: спавн на точке самолёта (FireEvent Spawn, у точки дочерний ServerVehicleSpawnEntity). У VU игрок после Spawn
        -- уже в машине, и мод сразу берёт controlledControllable как самолёт (BotSpawner.lua:185 → Vehicles.lua:51). Движок у нас
        -- создаёт самолёт и сажает в нём же спавне, но позже, чем появляется тело: солдата отдаём, когда движок посадил бота.
        -- Запас: посадки нет 3 с — отдаём как раньше (WRN; прогон 16:12 XP5_002: 108 ошибок мода без этого).
        if bodyReady and EVBotsSpawnVehPoint ~= nil and EVBotsSpawnVehPoint[p.nativePtr] then
          local okv, att, _, ctl = pcall(NativeBridge.PlayerVehicle, p.nativePtr)
          -- Готово, когда есть обёртка машины (её и отдаст controlledControllable): прогон 16:22 — движок уже посадил (att ≠ 0),
          -- а обёртки свежего самолёта ещё не было, и мод снова получал солдата (65 ошибок Vehicles.lua:51).
          -- ★ Сборка 212: у пешего [игрок+0x3E8] (ctl) — сам солдат (vtable 0x025C3948; прогон 23.10: «движок дал машину
          -- F6E80DD0 (vtable 025C3948)» у BOT_Dylan), машиной он не считается.
          local solp = NativeBridge.ReadU32(p.nativePtr, 0x3C4) or 0
          if okv and ctl ~= nil and ctl ~= 0 and ctl == solp then ctl = 0 end
          local vptr = (okv and att ~= nil and att ~= 0) and att or ((okv and ctl ~= nil and ctl ~= 0) and ctl or nil)
          local vobj = nil
          if vptr ~= nil and EntityManager ~= nil and EntityManager._VehicleByPtr ~= nil then
            local okw, w = pcall(EntityManager._VehicleByPtr, vptr)
            if okw then vobj = w end
          end
          if vptr ~= nil and vobj == nil then
            EVBotsVehWrapMiss = (EVBotsVehWrapMiss or 0) + 1
            if EVBotsVehWrapMiss <= 10 then
              local okt, vt = pcall(NativeBridge.ReadU32, vptr, 0)
              log("INFO ", string.format("ДИАГ посадка в самолёт: %s — движок дал машину %08X (vtable %08X), обёртки нет — ждём",
                tostring(p.name), vptr, (okt and vt) or 0))
            end
          end
          if vobj ~= nil then
            EVBotsSpawnVehPoint[p.nativePtr] = nil
            rawset(p, '_bodyAt', nil)
          else
            local now = EVBotsNow()
            local since = rawget(p, '_bodyAt')
            if since == nil then rawset(p, '_bodyAt', now) since = now end
            if now - since < 3.0 then
              bodyReady = false
            else
              log("WRN  ", "мост: " .. tostring(p.name) .. " — спавн на точке самолёта, но посадки нет 3 с; солдат отдан моду пешим (запас)")
              EVBotsSpawnVehPoint[p.nativePtr] = nil
              rawset(p, '_bodyAt', nil)
            end
          end
        end
        if bodyReady then
          p.soldier = p.pendingSoldier
          p.soldier.noBody = nil
          p.pendingSoldier = nil
          p._engineDead = nil  -- сборка 185: тело по спавну мода, не реанимация
          p.hasSoldier = true
          p.alive = true
          log("INFO ", "мост: тело в движке появилось — солдат отдан моду: " .. tostring(p.name))
        end
        -- ★ Сборка 185: реанимация (как VU). Медик поднял бота — движок вернул тело тому же игроку, мод спавн не просил
        -- (солдата у мода нет, отложенного тоже). В VU player.soldier живой из движка, а сервер шлёт Soldier:HealthAction
        -- (OnRevive): мод ждёт 50–300 мс и принимает реанимацию (EIACycleRadioChannel, StateIdle.lua:47). Раньше тело
        -- появлялось, а мод считал бота мёртвым и спавнил заново.
        if p.onlineId == 0 and rawget(p, '_engineDead') and not rawget(p, 'luaKilled') and p.soldier == nil and rawget(p, 'pendingSoldier') == nil
            and p.nativePtr ~= nil and p.nativePtr ~= 0 then
          local sp = NativeBridge.ReadU32(p.nativePtr, 0x3C4) or 0
          if sp == 0 then
            local cs, cdead = nil, true
            if NativeBridge.Corpse ~= nil then
              local okc, c0, _, _, _, d0 = pcall(NativeBridge.Corpse, p.nativePtr)
              if okc then cs, cdead = c0, d0 end
            end
            if cs ~= nil and cs ~= p._corpseSeen then
              EVBotsCorpseSeen = (EVBotsCorpseSeen or 0) + 1  -- диагностика (тест revive)
              p._corpseLiveCounted = nil
            end
            -- первое чтение — «мёртв», пока тик не посчитал состояние: считаем труп «живым», если хоть раз было isDead=false
            if cs ~= nil and cdead == false and not p._corpseLiveCounted then
              p._corpseLiveCounted = true
              EVBotsCorpseLive = (EVBotsCorpseLive or 0) + 1
            end
            p._corpseSeen = cs
          else
            local x, y, z = 0, 0, 0
            local okp, x1, y1, z1 = pcall(NativeBridge.SoldierPos, p.nativePtr)
            if okp and x1 ~= nil then x, y, z = x1, y1, z1 end
            local rs = NewSoldierEntity(LinearTransform(Vec3._make(1, 0, 0), Vec3._make(0, 1, 0), Vec3._make(0, 0, 1),
                                                        Vec3._make(x, y, z)))
            rs.player = p
            p.soldier = rs
            p.hasSoldier = true
            p.alive = true
            log("INFO ", string.format("РЕАНИМАЦИЯ: %s — движок вернул тело %08X (труп был %s); Soldier:HealthAction OnRevive",
              tostring(p.name), sp, p._corpseSeen and string.format('%08X', p._corpseSeen) or 'не виден'))
            EVBotsRevives = (EVBotsRevives or 0) + 1
            p._corpseSeen = nil
            p._engineDead = nil
            local okh, eh = pcall(Events._DispatchEngine, Events, 'Soldier:HealthAction', rs, HealthStateAction.OnRevive)
            if not okh then log("WRN  ", "Soldier:HealthAction: " .. tostring(eh)) end
          end
        end
        local s = p.soldier
        if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 and s ~= nil then
          -- Сборка 117: физика персонажа бота (CPE+0xFE = 0) и ввод мода в движок (свой объект ввода + флаг ИИ)
          local phys = false
          if NativeBridge.BotPhysOn ~= nil then
            -- (Сборка 168: опыт EVBOTS_ICON_AB 153 — ввод после тела у половины ботов — удалён, ответ получен.)
            local inputNow = true
            if inputNow and p.ownInputOn ~= p.nativePtr and NativeBridge.BotOwnInput ~= nil then
              -- ★ 255: отметка — только если ввод встал (прогон 254: таблица полна, отметка ставилась, повтора не было)
              EVBotsEnsureOwnInput(p)
            end
            phys = true  -- сборка 240: снимает BotTick ниже
          end
          -- ★ Сборка 172: объект ввода мода должен быть привязан к движку. fun-bots сразу после CreatePlayer подставляет
          -- свой EntryInput (BotManager.lua:879: s_BotPlayer.input = s_BotInput); ключ input в таблице игрока уже есть,
          -- поэтому __newindex не срабатывает. У позднего бота nativePtr ставится ещё в CreatePlayer, и новый input
          -- оставался без _native: SetLevel мода (газ, спринт) уходил в никуда, работал только прицел (InputAim) —
          -- бот поворачивался и стоял/«скользил» телепортами мода (прогон 171: 13 из 15 поздних ботов, скорость 0).
          local inp = rawget(p, 'input')
          if type(inp) == 'table' and rawget(inp, '_native') ~= p.nativePtr then
            inp._native = p.nativePtr
            EVBotsInputRebind = (EVBotsInputRebind or 0) + 1
            log("INFO ", "ввод мода привязан к движку: " .. tostring(p.name) .. " (был без привязки)")
          end
          -- Сборка 118: прицел мода в объект ввода движка (опыт: раскладка полей по окнам теста)
          -- Сборка 119: прицел мода (VU: flags = AuthoritativeAiming, authoritativeAimingYaw/Pitch) в объект ввода движка.
          -- Раскладка (re/cpe2): +0x1E8 флаги (bit0 AuthoritativeAiming), +0x1F4 yaw [0, 2pi], +0x1F8 pitch [-pi/2, pi/2].
          local aimYaw, aimPitch = nil, nil
          if NativeBridge.InputWrite ~= nil and p.input ~= nil and p.ownInputOn == p.nativePtr then
            local yaw = tonumber(rawget(p.input, "authoritativeAimingYaw"))
            local pitch = tonumber(rawget(p.input, "authoritativeAimingPitch")) or 0.0
            if yaw ~= nil then
              local twoPi = 2 * math.pi
              yaw = yaw % twoPi
              if pitch > math.pi / 2 then pitch = math.pi / 2 elseif pitch < -math.pi / 2 then pitch = -math.pi / 2 end
              aimYaw, aimPitch = yaw, pitch  -- сборка 168: один вызов вместо трёх InputWrite; 240 — в BotTick
            end
          end
          -- ★ Сборка 240: с 239 Lua — в кадре движка, раздел «позиции» был 86 % моста. Физика, прицел, позиция, скорость и машина
          -- бота — одним вызовом C (раньше пять pcall на бота за тик). Без BotTick — прежний путь.
          local okp, x, y, z, ex, ey, ez, eatt, ectl
          if NativeBridge.BotTick ~= nil then
            okp, x, y, z, ex, ey, ez, eatt, ectl = pcall(NativeBridge.BotTick, p.nativePtr, phys, aimYaw, aimPitch)
          else
            if phys then pcall(NativeBridge.BotPhysOn, p.nativePtr) end
            if aimYaw ~= nil then pcall(NativeBridge.InputAim, p.nativePtr, aimYaw, aimPitch) end
            okp, x, y, z = pcall(NativeBridge.SoldierPos, p.nativePtr)
          end
          if okp and x ~= nil and math.abs(x) < 4000 and math.abs(z) < 4000 then
            if s.bridgePrev ~= nil and EVBotsMoveDiag ~= nil then
              EVBotsMoveDiag.ticks = EVBotsMoveDiag.ticks + 1
              local pv = s.bridgePrev
              if x == pv[1] and y == pv[2] and z == pv[3] then EVBotsMoveDiag.same = (EVBotsMoveDiag.same or 0) + 1 end
            end
            -- ★ Сборка 200b: скорость из движка, как VU (PhysicsEntity.velocity); производная — запас (в технике позиция игрока =
            -- позиция машины, поле солдата там не скорость машины). ★ 245: машина читается до шага скорости; пешему со скоростью
            -- движка производную не считаем (был лишний Vec3 на бота за тик); поле derivVelocity нигде не читалось — убрано.
            -- ★ Сборка 212 (оптимизация): машину игрока читаем один раз за тик — раньше до трёх раз (каждый раз pcall с новым
            -- замыканием, NativeBridge.PlayerVehicle и поиск обёртки) на каждого бота, 30 раз в секунду.
            -- ★ Сборка 240: BotTick уже прочитал машину (не техника — 0): пешему без заказанной посадки обёртка не нужна —
            -- vehicleFieldValue вернул бы nil.
            local okA, attached = true, nil
            if eatt == nil or eatt ~= 0 or ectl ~= 0 or rawget(p, '_vehPending') ~= nil then
              okA, attached = pcall(vehicleFieldValue, p, 2)
              if not okA then attached = nil end
            end
            EVBotsVelocityStep(s, x, y, z, deltaTime, okA and attached == nil and ex ~= nil)
            if okA and attached == nil then
              if ex ~= nil then
                s.bridgeVelocity = Vec3._make(ex, ey, ez)
              elseif NativeBridge.BotTick == nil and NativeBridge.SoldierVel ~= nil then
                local okE, vx, vy, vz = pcall(NativeBridge.SoldierVel, p.nativePtr)
                if okE and vx ~= nil then s.bridgeVelocity = Vec3._make(vx, vy, vz) end
              end
            end
            -- ★ Сборка 193: падение и парашют (вопрос пользователя 19.10). Мод жмёт EIAToggleParachute (36) при
            -- PhysicsEntity(soldier).velocity.y < −25 (BotMovement.lua:610/817). Строка на каждое падение быстрее 15 м/с:
            -- скорость, высота, состояние мода, нажата ли кнопка парашюта (уровень ввода мода 36).
            local bv = s.bridgeVelocity
            -- Сборка 195: игрок в технике — не падение (прогон 194: все «ПАДЕНИЕ» — пилоты самолётов, BOT_Lucas 96–115 м/с).
            local okV, inVeh = okA, attached ~= nil
            -- ★ Сборка 200, диагностика: где у движка скорость солдата/машины (VU берёт velocity из физики). Ровный бег/езда
            -- (|v| 3..40 м/с) — раз в 2 с на бота: смещения с тем же Vec3 в солдате, его CharacterPhysicsEntity [+0x1D4],
            -- машине и её physicsEntityBase [+0x144]. Итог раз в 30 с: «ДИАГ скорость движка».
            -- ★ Сборка 212: только при EVBOTS_VELSCAN=1. Скорость солдата уже найдена (200b), а скан машины — до 3 обходов по
            -- 0x400 указателей × 0x400 байт на водителя раз в 2 с — давал пики блока «позиции» до 90–109 мс (прогон 23.10).
            if EVBOTS_VELSCAN and bv ~= nil and bv.x ~= nil and NativeBridge.VelScan ~= nil and NativeBridge.ReadU32 ~= nil then
              local m = math.sqrt(bv.x * bv.x + bv.y * bv.y + bv.z * bv.z)
              local now = EVBotsNow()
              if m > 3 and m < 40 and now - (rawget(s, '_vsAt') or 0) > 2 then
                rawset(s, '_vsAt', now)
                EVBotsVelScanDiag = EVBotsVelScanDiag or { n = { sol = 0, cpe = 0, veh = 0, peb = 0, vdeep = 0, pdeep = 0 }, hit = { sol = {}, cpe = {}, veh = {}, peb = {}, vdeep = {}, pdeep = {} }, at = now }
                local VS = EVBotsVelScanDiag
                local function scan(kind, base, span)
                  if base == nil or base == 0 then return end
                  local okS, offs = pcall(NativeBridge.VelScan, base, bv.x, bv.y, bv.z, span)
                  if okS and type(offs) == 'table' then
                    VS.n[kind] = VS.n[kind] + 1
                    for _, o in ipairs(offs) do VS.hit[kind][o] = (VS.hit[kind][o] or 0) + 1 end
                  end
                end
                local okv, veh = okA, attached
                if okv and veh ~= nil and rawget(veh, '_ptr') ~= nil then
                  local vp = rawget(veh, '_ptr')
                  scan('veh', vp, 0x800)
                  local okb, peb = pcall(NativeBridge.ReadU32, vp, 0x144)
                  if okb then scan('peb', peb, 0x400) end
                  if NativeBridge.VelScanDeep ~= nil then
                    local function deep(kind, base, span)
                      if base == nil or base == 0 then return end
                      local okD, hits = pcall(NativeBridge.VelScanDeep, base, bv.x, bv.y, bv.z, span)
                      if okD and type(hits) == 'table' then
                        VS.n[kind] = VS.n[kind] + 1
                        for _, o in ipairs(hits) do VS.hit[kind][o] = (VS.hit[kind][o] or 0) + 1 end
                      end
                    end
                    -- ★ 200c: по классу машины (vtable [veh+0] у каждого типа своя) — свои замеры и кандидаты.
                    local okt, vt = pcall(NativeBridge.ReadU32, vp, 0)
                    if okt and vt ~= nil then
                      VS.byVt = VS.byVt or {}
                      local e = VS.byVt[vt]
                      if e == nil then e = { n = 0, hit = {} }; VS.byVt[vt] = e end
                      e.n = e.n + 1
                      local okD, hits = pcall(NativeBridge.VelScanDeep, vp, bv.x, bv.y, bv.z, 0x400)
                      if okD and type(hits) == 'table' then
                        for _, o in ipairs(hits) do e.hit[o] = (e.hit[o] or 0) + 1 end
                      end
                      local okp2, pb2 = pcall(NativeBridge.ReadU32, vp, 0x144)
                      if okp2 and pb2 ~= nil and pb2 ~= 0 then
                        local okD2, hits2 = pcall(NativeBridge.VelScanDeep, pb2, bv.x, bv.y, bv.z, 0x200)
                        if okD2 and type(hits2) == 'table' then
                          for _, o in ipairs(hits2) do e.hit['peb/' .. o] = (e.hit['peb/' .. o] or 0) + 1 end
                        end
                      end
                    end
                  end
                else
                  local oks, sp = pcall(NativeBridge.ReadU32, p.nativePtr, 0x3C4)
                  if oks and sp ~= nil and sp ~= 0 then
                    scan('sol', sp, 0x800)
                    local okc, cpe = pcall(NativeBridge.ReadU32, sp, 0x1D4)
                    if okc then scan('cpe', cpe, 0x400) end
                  end
                end
                if now - VS.at > 30 then
                  VS.at = now
                  local parts = {}
                  for _, kind in ipairs({ 'sol', 'cpe', 'veh', 'peb', 'vdeep', 'pdeep' }) do
                    local arr = {}
                    for o, c in pairs(VS.hit[kind]) do arr[#arr + 1] = { o, c } end
                    table.sort(arr, function(a, b) return a[2] > b[2] end)
                    local top = {}
                    for i = 1, math.min(5, #arr) do top[#top + 1] = (type(arr[i][1]) == 'string') and string.format('%s:%d', arr[i][1], arr[i][2]) or string.format('+0x%X:%d', arr[i][1], arr[i][2]) end
                    parts[#parts + 1] = string.format('%s замеров %d [%s]', kind, VS.n[kind], table.concat(top, ' '))
                  end
                  log("INFO ", "ДИАГ скорость движка (совпадение Vec3 с производной позиции): " .. table.concat(parts, ' | '))
                  for vt, e in pairs(VS.byVt or {}) do
                    local arr = {}
                    for o, c in pairs(e.hit) do arr[#arr + 1] = { o, c } end
                    table.sort(arr, function(x, y) return x[2] > y[2] end)
                    local top = {}
                    for i = 1, math.min(3, #arr) do top[#top + 1] = string.format('%s:%d', arr[i][1], arr[i][2]) end
                    log("INFO ", string.format("ДИАГ скорость машины класс vtbl %08X: замеров %d [%s]", vt, e.n, table.concat(top, ' ')))
                  end
                end
              end
            end
            if bv ~= nil and bv.y ~= nil and bv.y < -15.0 and not (okV and inVeh) then
              if not rawget(p, '_fallLogged') then
                rawset(p, '_fallLogged', true)
                EVBotsFallLogN = (EVBotsFallLogN or 0) + 1
                if EVBotsFallLogN <= 100 then
                  local st = '?'
                  if g_BotStates ~= nil and BotManager ~= nil and BotManager.GetBotByName ~= nil then
                    pcall(function()
                      local b = BotManager:GetBotByName(p.name)
                      if b ~= nil then
                        for k, v in pairs(g_BotStates.States) do if v == b.m_ActiveState then st = k end end
                      end
                    end)
                  end
                  local lv = p.input ~= nil and rawget(p.input, '_levels') or nil
                  log("INFO ", string.format("ПАДЕНИЕ %s: vy=%.1f м/с, y=%.0f, состояние мода %s, мод задал уровень 36 (парашют)=%s",
                      tostring(p.name), bv.y, y, st, tostring(lv ~= nil and lv[36] or nil)))
                end
              end
              if bv.y < -25.0 then
                local lv = p.input ~= nil and rawget(p.input, '_levels') or nil
                if lv ~= nil and (tonumber(lv[36]) or 0) > 0 and not rawget(p, '_chuteLogged') then
                  rawset(p, '_chuteLogged', true)
                  log("INFO ", string.format("ПАРАШЮТ %s: мод нажал EIAToggleParachute при vy=%.1f, y=%.0f", tostring(p.name), bv.y, y))
                end
              end
            elseif rawget(p, '_fallLogged') and bv ~= nil and bv.y ~= nil and bv.y > -3.0 then
              rawset(p, '_fallLogged', nil)
              rawset(p, '_chuteLogged', nil)
            end
            if EVBotsMoveDiag ~= nil and s.bridgeVelocity ~= nil and s.bridgeVelocity.magnitude ~= nil
                and s.bridgeVelocity.magnitude < 0.3 then
              EVBotsMoveDiag.still = EVBotsMoveDiag.still + 1
            end
            local wt = s.worldTransform
            if wt ~= nil and wt.left ~= nil and wt.trans ~= nil then
              local t = wt.trans
              if math.abs(t.x - x) > 0.01 or math.abs(t.y - y) > 0.01 or math.abs(t.z - z) > 0.01 then
                -- новый объект с теми же базисными векторами: общий трансформ мода не меняем
                s.worldTransform = LinearTransform(wt.left, wt.up, wt.forward, Vec3._make(x, y, z))
              end
            end
            if NativeBridge.TakeBaseSpawn ~= nil and NativeBridge.TakeBaseSpawn(p.nativePtr) then
              p._basePathPending = true
            end
            if rawget(p, '_basePathPending') then
              local okb, aligned = pcall(EVBotsAlignBaseSpawn, p, Vec3._make(x, y, z))
              if okb and aligned then p._basePathPending = nil end
            end
          end
        end
      end
    end
    -- Боты, созданные очередью: проставляем nativePtr по имени (13.10, прогон 60:
    -- без этого CreateSoldier/SpawnSoldierAt/ввод мода не доходили до движка).
    EVBotsBP('созданные боты')
    if NativeBridge.PollCreatedBots ~= nil then
      local okc, created = pcall(NativeBridge.PollCreatedBots)
      if okc and created ~= nil then
        for name, ptr in pairs(created) do
          for i = 1, #players do
            local p = players[i]
            if p.name == name and (p.nativePtr == nil or p.nativePtr == 0) then
              p.nativePtr = ptr
              log("INFO ", "мост: " .. tostring(name) .. " привязан к движку ptr=" .. tostring(ptr))
              PlayerManager._AdoptEngineId(p, ptr, true)  -- ★ 247: id уже у мода — не меняем
              if rawget(p, '_enterLater') ~= nil then Player.__newindex(p, 'nativePtr', ptr) end  -- 244: посадка после привязки
              local reqPos, reqSquad = p.spawnPos, p.spawnSquad  -- сборка 148: для повтора SpawnSoldierAt
              -- позиция, которую мод передал до появления движкового игрока (SpawnSoldierAt): отдаём мосту
              if p.spawnPos ~= nil and NativeBridge.SetBotPos ~= nil then
                pcall(NativeBridge.SetBotPos, ptr, p.spawnPos.x, p.spawnPos.y, p.spawnPos.z)
                p.spawnPos = nil
              end
              -- отряд, который мод выбрал до появления движкового игрока
              if p.spawnSquad ~= nil and NativeBridge.SetBotSquad ~= nil then
                pcall(NativeBridge.SetBotSquad, ptr, p.spawnSquad)
                p.spawnSquad = nil
              end
              -- класс (кит), выбранный до привязки
              if p.pendingKit ~= nil and NativeBridge.SetBotKit ~= nil then
                pcall(NativeBridge.SetBotKit, ptr, p.pendingKit)
                p.pendingKit = nil
                if p.selectedAppearanceName ~= nil and NativeBridge.SetBotAppearance ~= nil then
                  pcall(NativeBridge.SetBotAppearance, ptr, p.selectedAppearanceName)
                end
              end
              -- Сборка 99: оружие, выбранное модом до привязки (ApplyCustomization без указателя в движке)
              if p.selectedWeapons ~= nil and next(p.selectedWeapons) ~= nil and NativeBridge.SetBotWeapons ~= nil then
                local okw, fw, mw = pcall(NativeBridge.SetBotWeapons, ptr, p.selectedWeapons, p.selectedAttachments or {})
                log('INFO ', 'ДИАГ оружие при привязке: ' .. tostring(p.name) .. ' найдено=' .. tostring(fw)
                    .. ' нет в памяти=' .. tostring(mw) .. ' слот0=' .. tostring(p.selectedWeapons[0]))
              end
              -- Сборка 107: перки, выбранные модом до привязки
              if p.selectedPerkNames ~= nil and #p.selectedPerkNames > 0 and NativeBridge.SetBotPerks ~= nil then
                pcall(NativeBridge.SetBotPerks, ptr, p.selectedPerkNames)
              end
              -- Сборка 148 (как VU): внешность и без pendingKit (выше она уходила только вместе с китом)
              if p.pendingKit == nil and p.selectedAppearanceName ~= nil and NativeBridge.SetBotAppearance ~= nil then
                pcall(NativeBridge.SetBotAppearance, ptr, p.selectedAppearanceName)
              end
              -- Сборка 148 (как VU): тело — повтор SpawnSoldierAt мода, уже после выбора мода (выше)
              if p.spawnRequested and NativeBridge.VuSpawnMode ~= nil and NativeBridge.VuSpawnMode()
                  and NativeBridge.SpawnEnginePlayer ~= nil then
                p.spawnRequested = nil
                local okS, spawned
                if reqPos ~= nil then
                  okS, spawned = pcall(NativeBridge.SpawnEnginePlayer, ptr, reqPos.x, reqPos.y, reqPos.z, reqSquad or 0)
                else
                  okS, spawned = pcall(NativeBridge.SpawnEnginePlayer, ptr, nil, nil, nil, reqSquad or 0)
                end
                log("INFO ", "мост: " .. tostring(name) .. " — SpawnSoldierAt мода повторён после выбора (как VU): "
                    .. tostring(okS and spawned or spawned))
              end
              break
            end
          end
        end
      end
    end
    -- Команда бота — из движка, как в VU (Player.teamId отражает движок). Прогон 78: на старте раунда
    -- (onNewRound) движок поменял команды ботов местами, а Lua хранил старые — мод ставил ботов на базу
    -- противника, и защита базы их убивала. Раз в 0,5 с сверяем +0x31C и шлём Player:TeamChange.
    -- Сборка 122: человек для мода — служебный Host получает Lua-солдата с позицией настоящего человека из движка
    -- (цель для ботов), и один раз эмулированный клиент мода шлёт Client:RequestSettings (мод регистрирует активного
    -- игрока и начинает раздавать проверки bot-bot). Затем серверная эмуляция лучей (ServerRayTick).
    EVBotsBP('человек/поз')
    if NativeBridge.RealPlayer ~= nil and NativeBridge.SoldierPos ~= nil then
      for i = 1, #players do
        local p = players[i]
        -- ★ Сборка 201: у каждого человека свой указатель движка (по имени). Раньше всем людям бралась позиция первого
        -- (RealPlayer) — для мода все люди стояли там, где хост, и боты били только его (друг, 19.10 17:12).
        local okh, human, hx, hy, hz = false, nil, nil, nil, nil
        if p.onlineId ~= 0 then
          human = EVBotsHumanPtr(p)
          okh = human ~= nil
          if okh then hx, hy, hz = NativeBridge.SoldierPos(human) end
        end
        if p.onlineId ~= 0 then
          -- ★ Сборка 201: Client:RequestSettings — когда клиент человека вошёл в уровень (+0x49C/+0x49D = 1/1), а не после дислокации.
          -- У VU клиент мода шлёт его по ClientStateEnteredIngameMessage (ClientBotManager.lua:104), до «Дислокации»; без этого мод не
          -- считает человека активным игроком и не раздаёт проверки бот-бот — боты на новом уровне не воевали, пока человек не
          -- задеплоился (пользователь, 19.10 ≈17:00, TDM после AirSuperiority).
          -- ★ Сборка 221 (Б1): у игрока живой канал К4 — его клиент EVBots сам шлёт Client:RequestSettings (прогон 23:17:
          -- настоящий + эмуляция = дубль). Эмуляция — только для клиентов без EVBots.dll.
          -- ★ 238-3: клиент мода игрока уже прислал события (в т.ч. сам Client:RequestSettings) — эмуляция не нужна.
          -- Факт событий клиента за прошлый уровень не подтверждает RequestSettings на этом уровне.
          -- Настоящий запрос отмечается при доставке в VextNetEvents; иначе после ожидания повторяем его.
          -- Эмуляция за клиента (EVBOTS_SERVER_CLIENT_EMU, по умолчанию — по каждому игроку, 238-3). Есть канал, но событий мода
          -- ещё нет — ждём до 5 с с входа в уровень (настоящий клиент шлёт RequestSettings тогда же, иначе был бы дубль, 221):
          -- это срок «клиента мода нет», а не задержка события.
          local chanWait = false
          if EVBotsLevelReady ~= false and not p.clientSettingsSent and NetEvents._HasChannel ~= nil and NetEvents._HasChannel(p) then
            local since = rawget(p, '_modWaitSince')
            if since == nil then rawset(p, '_modWaitSince', EVBotsNow()); chanWait = true
            elseif EVBotsNow() - since < 5.0 then chanWait = true
            elseif not rawget(p, '_modWaitSaid') then
              rawset(p, '_modWaitSaid', true)
              log("WARN ", "мост: у " .. tostring(p.name) .. " канал есть, а клиент мода за 5 с ничего не прислал — подменяю его клиента")
            end
          end
          if EVBotsLevelReady ~= false and not p.clientSettingsSent and not chanWait and EVBOTS_SERVER_CLIENT_EMU == true and okh and human ~= nil and human ~= 0
              and NativeBridge.ReadU32 ~= nil then
            local okf, fl = pcall(NativeBridge.ReadU32, human, 0x49C)
            if (okf and fl ~= nil and (fl & 0xFF) == 1 and ((fl >> 8) & 0xFF) == 1) or hx ~= nil then
              local okd, errd = pcall(NetEvents._Deliver, NetEvents, "Client:RequestSettings", p)
              if okd then p.clientSettingsSent = true end
              log("INFO ", "мост: эмуляция клиента мода — Client:RequestSettings для " .. tostring(p.name)
                  .. (hx ~= nil and " (в мире)" or " (клиент в уровне, до дислокации — как VU)")
                  .. (okd and "" or (" (ошибка: " .. tostring(errd) .. ")")))
            end
          end
          if hx ~= nil then
            local body = NativeBridge.ReadU32 and NativeBridge.ReadU32(human, 0x3C4) or nil
            if p.soldier == nil or (p.hostMirror and body ~= nil and rawget(p.soldier, 'nativePtr') ~= body) then
              p.soldier = NewSoldierEntity(LinearTransform(Vec3._make(1, 0, 0), Vec3._make(0, 1, 0), Vec3._make(0, 0, 1),
                  Vec3._make(hx, hy, hz)))
              p.soldier.player = p
              p.soldier.nativePtr = body
              if body ~= nil and body ~= 0 and NativeBridge.CaptureHumanSoldier ~= nil then
                local target, why = NativeBridge.CaptureHumanSoldier(human, body)
                rawset(p.soldier, '_nativeCustomizationTarget', target)
                if target == nil then log('WARN ', 'human soldier weak capture: ' .. tostring(why)) end
              end
              p.hasSoldier = true
              p.alive = true
              p.hostMirror = true
              log("INFO ", "мост: человек (" .. tostring(p.name) .. ") в мире — Lua-солдат для мода")
            elseif p.hostMirror then
              local wt = p.soldier.worldTransform
              if wt ~= nil and wt.left ~= nil then
                EVBotsVelocityStep(p.soldier, hx, hy, hz, deltaTime)  -- сборка 171: как у ботов
                p.soldier.worldTransform = LinearTransform(wt.left, wt.up, wt.forward, Vec3._make(hx, hy, hz))
              end
            end
          elseif p.hostMirror and p.soldier ~= nil then
            p.soldier = nil
            p.hasSoldier = false
            p.alive = false
          end
        end
      end
    end
    -- Сборка 128 (диагностика прицела): у ботов, которым мод велел стрелять (_ShootPlayer), раз в 1 с сравниваем
    -- нужный угол на цель с углом мода (input) и с тем, что лежит в объекте ввода движка (+0x1F4/+0x1F8), и уровень огня (+0x2C).
    EVBotsAimT = (EVBotsAimT or 0) + (deltaTime or 0)
    EVBotsAimLogT = (EVBotsAimLogT or 0) + (deltaTime or 0)
    EVBotsBP('прицел')
    if EVBotsAimT >= 1.0 and g_BotManager ~= nil and NativeBridge.ReadU32 ~= nil then
      EVBotsAimT = 0
      local A = EVBotsAim or { n = 0, dm = 0, de = 0, pm = 0, pe = 0, fire = 0, dist = 0, sample = nil }
      EVBotsAim = A
      local function f32(u) if u == nil then return nil end return (string.unpack("<f", string.pack("<I4", u & 0xFFFFFFFF))) end
      local function nrm(v) while v > math.pi do v = v - 2 * math.pi end while v < -math.pi do v = v + 2 * math.pi end return v end
      pcall(function()
        for _, b in pairs(g_BotManager:GetBots()) do
          local sp = b._ShootPlayer
          local pl = b.m_Player
          if sp ~= nil and sp.soldier ~= nil and pl ~= nil and pl.soldier ~= nil and pl.nativePtr ~= nil and pl.nativePtr ~= 0 then
            local a = pl.soldier.worldTransform.trans
            local t = sp.soldier.worldTransform.trans
            local dx, dy, dz = t.x - a.x, (t.y + 1.2) - (a.y + 1.5), t.z - a.z
            local hz = math.sqrt(dx * dx + dz * dz)
            local wy = math.atan(dz, dx) - math.pi / 2
            local wp = math.atan(dy, hz)
            local my, mp = pl.input.authoritativeAimingYaw or 0, pl.input.authoritativeAimingPitch or 0
            local inp = NativeBridge.ReadU32(pl.nativePtr, 0x3C0)
            local ey, ep, fl = nil, nil, nil
            if inp ~= nil and inp ~= 0 then
              ey = f32(NativeBridge.ReadU32(inp, 0x1F4)); ep = f32(NativeBridge.ReadU32(inp, 0x1F8)); fl = f32(NativeBridge.ReadU32(inp, 0x2C))
            end
            A.n = A.n + 1
            A.dist = A.dist + hz
            A.dm = A.dm + math.abs(nrm(my - wy)); A.pm = A.pm + math.abs(nrm(mp - wp))
            if ey ~= nil then A.de = A.de + math.abs(nrm(ey - wy)); A.pe = A.pe + math.abs(nrm(ep - wp)) end
            if fl ~= nil and fl > 0.5 then A.fire = A.fire + 1 end
            if A.sample == nil then
              A.sample = string.format("%s->%s: нужно yaw %.2f pitch %.2f | мод %.2f %.2f | движок %s %s | огонь %s | %.0f м",
                tostring(pl.name), tostring(sp.name), wy, wp, my, mp, tostring(ey and string.format("%.2f", ey)),
                tostring(ep and string.format("%.2f", ep)), tostring(fl), hz)
            end
          end
        end
      end)
    end
    if EVBotsAimLogT >= 20.0 then
      EVBotsAimLogT = 0
      local A = EVBotsAim
      if A ~= nil and A.n > 0 then
        log("INFO ", string.format("ДИАГ прицел: выборок %d, ср. дистанция %.0f м; ошибка yaw мод %.0f° движок %.0f°; pitch мод %.0f° движок %.0f°; огонь>0.5 в %d; пример: %s",
          A.n, A.dist / A.n, math.deg(A.dm / A.n), math.deg(A.de / A.n), math.deg(A.pm / A.n), math.deg(A.pe / A.n), A.fire, tostring(A.sample)))
      else
        log("INFO ", "ДИАГ прицел: ботов с целью нет")
      end
      EVBotsAim = nil
    end
    -- Сборка 127 (диагностика движения): застревание мода — как часто запускаются «препятствия», при какой скорости, в каком
    -- режиме скорости, сколько телепортов (SoldierEntity:SetTransform из мода). Поведение не меняет (оригиналы вызываются).
    EVBotsBP('движение')
    if EVBOTS_NOTELEPORT == true and Config ~= nil and Config.TeleportIfStuck ~= nil and not EVBotsNoTeleportSet then
      EVBotsNoTeleportSet = true
      Config.TeleportIfStuck = false
      log("INFO ", "мост: телепорт застрявших ботов ВЫКЛЮЧЕН (Config.TeleportIfStuck = false; EVBOTS_NOTELEPORT=0 вернёт)")
    end
    if EVBotsMoveDiag == nil and rawget(_G, "Bot") ~= nil and type(Bot._ObstacleHandling) == "function" then
      EVBotsMoveDiag = { ob = 0, lo = 0, mid = 0, hi = 0, walk = 0, sprint = 0, other = 0, tele = 0, ticks = 0, still = 0, t = 0.0 }
      local origOb = Bot._ObstacleHandling
      Bot._ObstacleHandling = function(self, v, d, h, dt, pos)
        local d2 = EVBotsMoveDiag
        local m = (v ~= nil and v.magnitude) or 0
        d2.ob = d2.ob + 1
        if m < 0.3 then d2.lo = d2.lo + 1 elseif m < 3.5 then d2.mid = d2.mid + 1 else d2.hi = d2.hi + 1 end
        local sp = self.m_ActiveSpeedValue
        if sp == BotMoveSpeeds.Sprint then d2.sprint = d2.sprint + 1
        elseif sp == BotMoveSpeeds.Normal then d2.walk = d2.walk + 1
        else d2.other = d2.other + 1 end
        return origOb(self, v, d, h, dt, pos)
      end
      if SoldierEntity ~= nil and type(SoldierEntity.SetTransform) == "function" then
        local origST = SoldierEntity.SetTransform
        SoldierEntity.SetTransform = function(self, t)
          EVBotsMoveDiag.tele = EVBotsMoveDiag.tele + 1
          return origST(self, t)
        end
      end
      log("INFO ", "мост: диагностика движения включена (препятствия, скорости, телепорты)")
    end
    if EVBotsMoveDiag ~= nil then
      EVBotsMoveDiag.t = EVBotsMoveDiag.t + (deltaTime or 0)
      if EVBotsMoveDiag.t >= 20.0 then
        local q = EVBotsMoveDiag
        log("INFO ", string.format("ДИАГ движение: препятствий %d (скорость<0.3: %d, 0.3..3.5: %d, >3.5: %d); режим: ходьба %d, спринт %d, иной %d; телепортов %d; тиков без движения %d из %d; позиция не обновилась %d (скорость удержана, VEL_HOLD=%s)",
            q.ob, q.lo, q.mid, q.hi, q.walk, q.sprint, q.other, q.tele, q.still, q.ticks, q.same or 0, tostring(EVBOTS_VEL_HOLD ~= false)))
        q.ob, q.lo, q.mid, q.hi, q.walk, q.sprint, q.other, q.tele, q.ticks, q.still, q.t, q.same = 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0, 0
      end
    end
    -- Сборка 133: скучивание ботов (диагностика). Раз в 20 с: сколько пар ботов стоят ближе 1 м друг к другу.
    EVBotsCrowdT = (EVBotsCrowdT or 0.0) + (deltaTime or 0)
    if EVBotsCrowdT >= 20.0 then
      EVBotsCrowdT = 0.0
      local pts = {}
      for i = 1, #players do
        local p = players[i]
        local s = p.soldier
        if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 and s ~= nil and s.bridgePrev ~= nil then
          pts[#pts + 1] = s.bridgePrev
        end
      end
      local near, minD = 0, -1.0
      for a = 1, #pts do
        for b = a + 1, #pts do
          local dx = pts[a][1] - pts[b][1]
          local dy = pts[a][2] - pts[b][2]
          local dz = pts[a][3] - pts[b][3]
          local d = math.sqrt(dx * dx + dy * dy + dz * dz)
          if minD < 0 or d < minD then minD = d end
          if d < 1.0 then near = near + 1 end
        end
      end
      log("INFO ", string.format("ДИАГ скучивание: ботов с позицией %d, пар ближе 1 м: %d, ближайшая пара %.2f м", #pts, near, minD))
    end
    -- Сборка 126: подробные логи мода на окно 150 с (EVBOTS_MODLOG=1). Debug.Logger читает PRINTALL при каждом Write.
    if EVBOTS_MODLOG == true and EVBotsModLogT == nil and rawget(_G, "Debug") ~= nil and Debug.Logger ~= nil then
      EVBotsModLogT = 0.0
      Debug.Logger.PRINTALL = true
      log("INFO ", "мост: подробные логи мода включены на 150 с (EVBOTS_MODLOG=1)")
    end
    if EVBotsModLogT ~= nil and EVBotsModLogT >= 0 then
      EVBotsModLogT = EVBotsModLogT + (deltaTime or 0)
      if EVBotsModLogT >= 150.0 then
        EVBotsModLogT = -1.0
        Debug.Logger.PRINTALL = false
        log("INFO ", "мост: подробные логи мода выключены")
      end
    end
    -- Сборка 125 (ред. 3): ДИАГНОСТИКА стрельбы. Прогон 125: боты врагов стоят вплотную и не стреляют, хотя лучи
    -- доставляются. Обёртка над Bot:ShootAt мода: поведение то же (вызов оригинала), считаем вызовы/успехи, ошибки
    -- и при отказе — какое условие не прошло. Сводка раз в 20 с.
    EVBotsBP('стрельба')
    if not shootDiag and rawget(_G, "Bot") ~= nil and type(Bot.ShootAt) == "function" then
      shootDiag = { calls = 0, ok = 0, err = 0, why = {}, lastErr = nil, t = 0.0, samples = 0 }
      local orig = Bot.ShootAt
      Bot.ShootAt = function(self, target, ignoreYaw)
        local d = shootDiag
        d.calls = d.calls + 1
        local okc, res = pcall(orig, self, target, ignoreYaw)
        if not okc then
          d.err = d.err + 1
          d.lastErr = tostring(res)
          return false
        end
        if res then
          d.ok = d.ok + 1
        else
          local why = "иное (ready/угол)"
          local okw = pcall(function()
            local cfg = rawget(_G, "Config")
            local tb = nil
            pcall(function() tb = g_BotManager:GetBotByName(target.name) end)
            if target == nil then why = "цель nil"
            elseif self.m_Player.teamId == target.teamId then why = "своя команда"
            elseif target.soldier == nil then why = "у цели нет солдата"
            elseif self.m_Player.soldier == nil then why = "у бота нет солдата"
            elseif self._Shoot == false then why = "_Shoot=false (путь фиксированный)"
            elseif self._DontAttackPlayers then why = "_DontAttackPlayers"
            elseif self._ActiveAction ~= nil and self._ActiveAction ~= 0 then why = "действие " .. tostring(self._ActiveAction)
            elseif self._ShootPlayerId ~= nil and self._ShootPlayerId ~= -1 and self._ShootPlayerId ~= target.id
              and (self._DoneShootDuration or 0) <= 0.7 then why = "занят другой целью"
            else
              local a = target.soldier.worldTransform.trans
              local b = self.m_Player.soldier.worldTransform.trans
              local dx, dy, dz = a.x - b.x, a.y - b.y, a.z - b.z
              local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
              local okd, maxd = pcall(self.GetAttackDistance, self, ignoreYaw, nil)
              local soundOverride = tb ~= nil and tb._SoundTimer ~= nil and tb._SoundTimer <= 5.0 and dist < 50
              if okd and maxd ~= nil and dist > maxd then
                why = string.format("далеко %.0f > %.0f м", dist, maxd)
              elseif not ignoreYaw and not soundOverride and cfg ~= nil then
                local pitch = math.atan(dy, math.sqrt(dx * dx + dz * dz))
                local yaw = math.atan(dz, dx) - math.pi / 2
                local function nrm(v)
                  while v > math.pi do v = v - 2 * math.pi end
                  while v < -math.pi do v = v + 2 * math.pi end
                  return v
                end
                local ry = math.abs(nrm(yaw - self.m_Player.input.authoritativeAimingYaw))
                local rp = math.abs(nrm(pitch - self.m_Player.input.authoritativeAimingPitch))
                local hh = math.rad((cfg.FovForShooting or 160) / 2)
                local hv = math.rad((cfg.FovVerticleForShooting or 60) / 2)
                if ry >= hh or rp >= hv then
                  why = string.format("вне угла обзора (гор %.0f°, верт %.0f°)", math.deg(ry), math.deg(rp))
                else
                  why = "угол ок, отказ позже (промах у края/таймеры)"
                end
              else
                why = "условия ок, отказ позже (таймеры/промах)"
              end
            end
          end)
          if not okw then why = "разбор упал" end
          if ignoreYaw then why = "[в ответ] " .. why end  -- сборка 167: ответный огонь (Soldier:Damage → ShootAt(…, true))
          d.why[why] = (d.why[why] or 0) + 1
        end
        if ignoreYaw then
          d.iy = (d.iy or 0) + 1
          if res then d.iyOk = (d.iyOk or 0) + 1 end
          if target ~= nil and target.onlineId ~= nil and target.onlineId ~= 0 then
            d.iyHuman = (d.iyHuman or 0) + 1
            if res then d.iyHumanOk = (d.iyHumanOk or 0) + 1 end
          end
        end
        return res
      end
      -- входы в обработчики результатов мода: доходят ли лучи до стрельбы
      local BM = rawget(_G, "BotManager")
      if BM ~= nil and type(BM.OnBotShootAtBot) == "function" and type(BM.OnShootAt) == "function" then
        local origBB, origES = BM.OnBotShootAtBot, BM.OnShootAt
        BM.OnBotShootAtBot = function(self, a, b)
          shootDiag.bb = (shootDiag.bb or 0) + 1
          return origBB(self, a, b)
        end
        BM.OnShootAt = function(self, p, id, ign)
          shootDiag.es = (shootDiag.es or 0) + 1
          return origES(self, p, id, ign)
        end
      end
      log("INFO ", "мост: диагностика Bot:ShootAt включена")
    end
    if shootDiag ~= nil then
      shootDiag.t = shootDiag.t + (deltaTime or 0)
      if shootDiag.t >= 20.0 then
        local parts = {}
        for k, v in pairs(shootDiag.why) do parts[#parts + 1] = k .. "=" .. v end
        log("INFO ", string.format("ДИАГ стрельба: входов bb %d, es %d; ShootAt вызовов %d, успех %d, ошибок %d; в ответ на попадание %d (успех %d; по человеку %d, успех %d); отказы: %s%s",
            shootDiag.bb or 0, shootDiag.es or 0, shootDiag.calls, shootDiag.ok, shootDiag.err,
            shootDiag.iy or 0, shootDiag.iyOk or 0, shootDiag.iyHuman or 0, shootDiag.iyHumanOk or 0, table.concat(parts, ", "),
            shootDiag.lastErr and (" | ошибка: " .. shootDiag.lastErr) or ""))
        shootDiag.calls, shootDiag.ok, shootDiag.err, shootDiag.why, shootDiag.lastErr, shootDiag.t = 0, 0, 0, {}, nil, 0.0
        shootDiag.bb, shootDiag.es = 0, 0
        shootDiag.iy, shootDiag.iyOk, shootDiag.iyHuman, shootDiag.iyHumanOk = 0, 0, 0, 0
      end
    end
    -- Сборка 125: опрос флагов движка — события CapturePoint:Captured/Lost для мода (VextEntityManager.cpp)
    EVBotsBP('флаги/лучи')
    if EntityManager ~= nil and EntityManager._Poll ~= nil then
      local okp, errp = pcall(EntityManager._Poll, deltaTime)
      if not okp and not emPollErrLogged then
        emPollErrLogged = true
        log("ERR ", "EntityManager._Poll: " .. tostring(errp))
      end
    end
    if ServerRayTick ~= nil then
      local okr, errr = pcall(ServerRayTick, deltaTime)
      if not okr and not rayErrLogged then
        rayErrLogged = true
        log("ERR ", "ServerRayTick: " .. tostring(errr))
      end
    end
    -- Сборка 139: состав набора и отряд каждого бота — в тестовый лог (EVBots-test.log), раз в 20 с.
    -- Нужно, чтобы сопоставить иконки ботов с тем, что у них в наборе.
    iconDiagClock = (iconDiagClock or 0) + (deltaTime or 0)
    EVBotsBP('ИКОНКИ')
    -- ★ 238-5: только с EVBOTS_DIAG_ICONS=1 (иконки и перки ботов давно сходятся; раз в 20 с — строки по всем ботам, пик тика)
    if EVBOTS_DIAG_ICONS == true and iconDiagClock >= 20.0 and NativeBridge ~= nil and NativeBridge.TestLog ~= nil
        and NativeBridge.BotKitItems ~= nil then
      iconDiagClock = 0
      for i = 1, #players do
        local p = players[i]
        if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 then
          local okk, items = pcall(NativeBridge.BotKitItems, p.nativePtr)
          local perks = (type(p.selectedPerkNames) == 'table') and #p.selectedPerkNames or 0
          -- Сборка 149: + путь создания и поля игрока (что отличает бота с двумя иконками и ником)
          local okf, flds = false, nil
          if NativeBridge.BotLifeFields ~= nil then okf, flds = pcall(NativeBridge.BotLifeFields, p.nativePtr) end
          pcall(NativeBridge.TestLog, 'ИКОНКИ ' .. tostring(p.name) .. ' команда=' .. tostring(p.teamId)
              .. ' отряд=' .. tostring(p.squadId) .. ' лидер=' .. tostring(p.isSquadLeader == true)
              .. ' кит=' .. tostring(p.selectedKit) .. ' внешность=' .. tostring(p.selectedAppearanceName)
              .. ' перков=' .. tostring(perks) .. ' набор=' .. (okk and tostring(items) or 'ошибка')
              .. ' | ' .. (okf and tostring(flds) or '-'))
          -- Сборка 139: полный список перков бота (то, что мост ставит в набор), без обрезки строки.
          local pnames = {}
          if type(p.selectedPerkNames) == 'table' then
            for _, pn in ipairs(p.selectedPerkNames) do
              pnames[#pnames + 1] = (tostring(pn):gsub('Persistence/Unlocks/', ''))
            end
          end
          pcall(NativeBridge.TestLog, 'ПЕРКИ ' .. tostring(p.name) .. ' (' .. #pnames .. '): ' .. table.concat(pnames, ', '))
        end
      end
    end
    -- Сборка 149: журнал «ЖИЗНЬ» по каждому боту — привязка (путь, выбор мода), тело (номер, позиция, отряд, через
    -- сколько после смерти), смерть (номер, сколько прожил, причина: мод с местом вызова или движок/урон, последнее hp),
    -- изменения полей игрока (то, что отличает бота с двумя иконками и ником). Раз в 0,5 с.
    EVBotsLifeWall = (EVBotsLifeWall or 0) + (deltaTime or 0)
    lifeClock = (lifeClock or 0) + (deltaTime or 0)
    -- Сборка 152 (из 136L): + кто последним атаковал (цель мода _ShootPlayer), оружие движка в слотах на каждом теле,
    -- «БОЙ 10с» (состояния мода, доля с целью, дистанция, смерти), «ЧЕЛОВЕК» (его слоты), «ПОЯВЛЕНИЕ» (время человека и ботов).
    EVBotsFight = EVBotsFight or { clock = 0, hclock = 0, dmg = 0, mod = 0, attackers = {},
                                   w = { samples = 0, target = 0, dist = 0, distN = 0, states = {}, dmg = 0, mod = 0, bodies = 0 } }
    local LF = EVBotsFight
    LF.clock = LF.clock + (deltaTime or 0)
    LF.pilotT = (LF.pilotT or 0) + (deltaTime or 0)
    if LF.pilotT >= 2.0 then LF.pilotT = 0; pcall(EVBotsPilotDiag, false) end  -- сборка 185
    LF.hclock = LF.hclock + (deltaTime or 0)
    EVBotsBP('ЖИЗНЬ')
    if lifeClock >= 0.5 and NativeBridge.BotLifeFields ~= nil and NativeBridge.ReadU32 ~= nil then
      lifeClock = 0
      local now = EVBotsLifeWall
      local W = LF.w
      local stateName, modBots = {}, {}
      pcall(function() for k, v in pairs(g_BotStates.States) do stateName[v] = k end end)
      pcall(function()
        for _, b in pairs(g_BotManager:GetBots()) do
          local pl = b.m_Player
          if pl ~= nil then modBots[tostring(pl.name)] = b end
          local st = stateName[b.m_ActiveState] or '?'
          W.states[st] = (W.states[st] or 0) + 1
          W.samples = W.samples + 1
          local sp = b._ShootPlayer
          if sp ~= nil and pl ~= nil then
            W.target = W.target + 1
            local d = nil
            pcall(function()
              local a = pl.soldier.worldTransform.trans
              local t = sp.soldier.worldTransform.trans
              d = math.sqrt((t.x - a.x) ^ 2 + (t.y - a.y) ^ 2 + (t.z - a.z) ^ 2)
            end)
            if d ~= nil then W.dist = W.dist + d; W.distN = W.distN + 1 end
            local wn = nil
            pcall(function() wn = b.m_ActiveWeapon and b.m_ActiveWeapon.name end)
            LF.attackers[tostring(sp.name)] = { by = tostring(pl.name), at = now, w = tostring(wn), d = d }
          end
        end
      end)
      -- ★ Сборка 153: «БЛИЗКО» — раз в 2 с враги-боты ближе 30 м к человеку: состояние мода, цель, луч «человек видит
      -- бота» (es), угол взгляда бота относительно направления на человека, оружие мода (жалоба 19:22: «стою рядом — не стреляет»).
      LF.nearClock = (LF.nearClock or 0) + 0.5
      if LF.nearClock >= 2.0 and NativeBridge.RealPlayer ~= nil and NativeBridge.SoldierPos ~= nil then
        LF.nearClock = 0
        local okh, hptr = pcall(NativeBridge.RealPlayer)
        if okh and hptr ~= nil and hptr ~= 0 then
          local hx, hy, hz = NativeBridge.SoldierPos(hptr)
          local hteam = NativeBridge.ReadU32(hptr, 0x31C)
          if hx ~= nil then
            local shown = 0
            for name, b in pairs(modBots) do
              local pl = b.m_Player
              if shown < 5 and pl ~= nil and pl.teamId ~= hteam and pl.nativePtr ~= nil and pl.nativePtr ~= 0 then
                local bx, by, bz = NativeBridge.SoldierPos(pl.nativePtr)
                if bx ~= nil then
                  local d = math.sqrt((bx - hx) ^ 2 + (by - hy) ^ 2 + (bz - hz) ^ 2)
                  if d < 30 then
                    shown = shown + 1
                    local st, tgt, wn, yawErr = '?', 'нет', '?', '?'
                    pcall(function() st = stateName[b.m_ActiveState] or '?' end)
                    pcall(function() if b._ShootPlayer ~= nil then tgt = tostring(b._ShootPlayer.name) end end)
                    pcall(function() wn = tostring(b.m_ActiveWeapon and b.m_ActiveWeapon.name) end)
                    pcall(function()
                      local want = math.atan(hz - bz, hx - bx) - math.pi / 2
                      local v = (pl.input.authoritativeAimingYaw or 0) - want
                      while v > math.pi do v = v - 2 * math.pi end
                      while v < -math.pi do v = v + 2 * math.pi end
                      yawErr = string.format('%.0f°', math.deg(math.abs(v)))
                    end)
                    local es = EVBotsEsSeen and EVBotsEsSeen[pl.id]
                    local esTxt = es and string.format('%s %.1f с назад', es.clear and 'ЧИСТО' or 'закрыт', now - es.at) or 'не было'
                    log("INFO ", string.format("БЛИЗКО %s: %.0f м, состояние=%s, цель=%s, луч человек->бот: %s, взгляд мимо человека на %s, оружие мода=%s",
                      name, d, st, tgt, esTxt, yawErr, wn))
                  end
                end
              end
            end
          end
        end
      end
      -- ПОЯВЛЕНИЕ: первое тело человека и первое тело бота (сверка времени)
      if NativeBridge.RealPlayer ~= nil and LF.humanBodyAt == nil then
        local okh, hptr = pcall(NativeBridge.RealPlayer)
        if okh and hptr ~= nil and hptr ~= 0 and (NativeBridge.ReadU32(hptr, 0x3C4) or 0) ~= 0 then
          LF.humanBodyAt = now
          log("INFO ", string.format("ПОЯВЛЕНИЕ: первое тело человека на %.1f с (первое тело бота — %s)", now,
            LF.botBodyAt and string.format('%.1f с', LF.botBodyAt) or 'ещё нет'))
        end
      end
      for i = 1, #players do
        local p = players[i]
        if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 then
          local okf, f = pcall(NativeBridge.BotLifeFields, p.nativePtr)
          f = (okf and type(f) == 'string') and f or '-'
          local lf = p.life
          if lf == nil then
            lf = { bodies = 0, deaths = 0, body = false, fields = nil, fieldLogs = 0 }
            p.life = lf
            log("INFO ", "ЖИЗНЬ " .. tostring(p.name) .. ": привязан ptr=" .. string.format('%08X', p.nativePtr)
                .. " команда=" .. tostring(p.teamId) .. " кит=" .. tostring(p.selectedKit or p.pendingKit)
                .. " внешность=" .. tostring(p.selectedAppearanceName) .. " перков="
                .. tostring(type(p.selectedPerkNames) == 'table' and #p.selectedPerkNames or 0) .. " | " .. f)
          end
          local body = (NativeBridge.ReadU32(p.nativePtr, 0x3C4) or 0) ~= 0
          if body and not lf.body then
            lf.bodies = lf.bodies + 1
            -- ★ Сборка 153: событие VU Player:Respawn(player) на каждое тело (мод: NodeEditor, клиентская часть)
            pcall(Events._DispatchEngine, Events, 'Player:Respawn', p)
            -- Сборка 153: бот, убранный модом «без тела» (139/142/145), снова с телом — обычный бот (удаляется из движка)
            if p.nobodyRemoved then
              p.nobodyRemoved = nil
              log("INFO ", "ЖИЗНЬ " .. tostring(p.name) .. ": снова с телом после «забыть» — пометка «без тела» снята")
            end
            local pos = '?'
            if NativeBridge.SoldierPos ~= nil then
              local okp, x, y, z = pcall(NativeBridge.SoldierPos, p.nativePtr)
              if okp and x ~= nil then pos = string.format('(%.0f, %.0f, %.0f)', x, y, z) end
            end
            local after = lf.deathAt and string.format(' через %.1f с после смерти', now - lf.deathAt) or ''
            local okw, wv = false, nil
            if NativeBridge.BotWeaponNames ~= nil then okw, wv = pcall(NativeBridge.BotWeaponNames, p.nativePtr) end
            local mb, mw, mk = modBots[tostring(p.name)], nil, nil
            if mb ~= nil then
              pcall(function() mw = mb.m_ActiveWeapon and mb.m_ActiveWeapon.name end)
              pcall(function() mk = mb.m_Kit end)
            end
            log("INFO ", "ЖИЗНЬ " .. tostring(p.name) .. ": тело #" .. lf.bodies .. after .. " поз=" .. pos
                .. " отряд=" .. tostring(p.squadId) .. " | оружие движка: " .. (okw and tostring(wv) or '?')
                .. " | мод: оружие=" .. tostring(mw) .. " кит=" .. tostring(mk) .. " | " .. f)
            lf.bodyAt = now
            LF.w.bodies = LF.w.bodies + 1
            if LF.botBodyAt == nil then
              LF.botBodyAt = now
              log("INFO ", string.format("ПОЯВЛЕНИЕ: первое тело бота (%s) на %.1f с (первое тело человека — %s)",
                tostring(p.name), now, LF.humanBodyAt and string.format('%.1f с', LF.humanBodyAt) or 'ещё нет'))
            end
          elseif not body and lf.body then
            lf.deaths = lf.deaths + 1
            local lived = lf.bodyAt and string.format('%.0f', now - lf.bodyAt) or '?'
            local cause = 'движок/урон'
            if p.lifeModKill ~= nil and now - (p.lifeModKill.at or -100) < 3 then
              cause = 'мод ' .. tostring(p.lifeModKill.where)
              LF.mod = LF.mod + 1; LF.w.mod = LF.w.mod + 1
            else
              LF.dmg = LF.dmg + 1; LF.w.dmg = LF.w.dmg + 1
              local at = LF.attackers[tostring(p.name)]
              if at ~= nil and now - at.at < 3 then
                cause = cause .. string.format(', последний атаковал %s (%s, %s м)', at.by, at.w, at.d and string.format('%.0f', at.d) or '?')
                -- Сборка 177: что в руках у убийцы по движку (текущий слот и оружие в нём) — к «убил с пистолета при автомате»
                local killer = nil
                for ki = 1, #players do
                  if tostring(players[ki].name) == tostring(at.by) then killer = players[ki]; break end
                end
                if killer ~= nil and killer.nativePtr ~= nil and killer.nativePtr ~= 0 and NativeBridge.WeaponCurSlot ~= nil then
                  local oks, ks = pcall(NativeBridge.WeaponCurSlot, killer.nativePtr)
                  local okn, kn = false, nil
                  if NativeBridge.BotWeaponNames ~= nil then okn, kn = pcall(NativeBridge.BotWeaponNames, killer.nativePtr) end
                  local wname = (oks and ks ~= nil and okn and type(kn) == 'string') and kn:match(' ' .. tostring(ks) .. '=([^%s%[]+)') or nil
                  cause = cause .. string.format(' | в руках у убийцы (движок): слот %s %s', tostring(oks and ks or '?'), tostring(wname))
                end
              else
                cause = cause .. ', атакующий не известен моду (человек?)'
              end
              if LF.firstDmgAt == nil then
                LF.firstDmgAt = now
                log("INFO ", string.format("ПЕРВАЯ СМЕРТЬ ОТ УРОНА: %s на %.1f с", tostring(p.name), now))
              end
            end
            log("INFO ", "ЖИЗНЬ " .. tostring(p.name) .. ": смерть #" .. lf.deaths .. ", прожил " .. lived
                .. " с, причина=" .. cause .. ", последнее hp=" .. tostring(lf.lastHp))
            lf.deathAt = now
          end
          lf.body = body
          if body then lf.lastHp = f:match('hp=(%-?%d+)') end
          -- изменения полей (без hp, тела и кита — они меняются постоянно); не больше 30 строк на бота
          local key = f:gsub(' hp=%S+', ''):gsub(' тело=%S+', ''):gsub(' кит480=%S+', '')
          if lf.fields ~= nil and key ~= lf.fields and lf.fieldLogs < 30 then
            lf.fieldLogs = lf.fieldLogs + 1
            log("INFO ", "ЖИЗНЬ " .. tostring(p.name) .. ": поля изменились: " .. lf.fields .. " -> " .. key)
          end
          lf.fields = key
        end
      end
    end
    -- ★ Сборка 153: хук VU «Soldier:Damage» (мод: BotManager:OnSoldierDamage — ответный огонь Config.ShootBackIfHit,
    -- множители урона ботов по людям). События — из моста (NativeBridge.PollDamage: жертва, стрелок, урон, hp после).
    -- Урон уже нанесён движком: подмена damage обработчиком на него не влияет (пишем в лог, если мод её сделал).
    EVBotsBP('урон/чат')
    if NativeBridge.PollDamage ~= nil and Hooks ~= nil and Hooks._Fire ~= nil then
      local okd, evs = pcall(NativeBridge.PollDamage)
      if okd and type(evs) == 'table' and #evs > 0 then
        -- ★ Сборка 202: стрелок и жертва — любой из людей (EVBotsPlayerByPtr), не только первый
        local byPtr = EVBotsPlayerByPtr
        EVBotsDmgHook = EVBotsDmgHook or { ev = 0, giver = 0, fired = 0, noSoldier = 0, changed = 0, botVictimGiver = 0 }
        local DH = EVBotsDmgHook
        for _, e in ipairs(evs) do
          DH.ev = DH.ev + 1
          local victim = byPtr(e[1])
          local giver = byPtr(e[2])
          local giverHuman = giver ~= nil and giver.onlineId ~= 0
          if giver ~= nil then DH.giver = DH.giver + 1 end
          if giverHuman then DH.human = (DH.human or 0) + 1 end  -- сборка 167: стрелок — человек
          local sol = victim and victim.soldier
          if sol == nil then
            DH.noSoldier = DH.noSoldier + 1
          else
            if giver ~= nil and victim.onlineId == 0 then DH.botVictimGiver = DH.botVictimGiver + 1 end
            local info = { damage = e[3], isBulletDamage = (e[2] ~= 0), isClientDamage = giverHuman,
                           isExplosionDamage = false, boneIndex = 0 }
            if type(e[4]) == 'number' and e[4] >= 0 and e[4] + e[3] < math.huge then
              info.evbotsHealthBefore = e[4] + e[3]
            end
            local giverInfo = (giver ~= nil) and { giver = giver } or nil
            -- Сборка 199: хук уже отработал ДО урона (EVBotsDamage.Pre из ServerSoldierEntity::onDamage) — второй раз не шлём
            -- (и стрелка из Pre не затираем: здесь стрелок клиентского урона не известен, если людей несколько — сборка 202)
            local pre = rawget(victim, '_preDmgAt')
            if pre ~= nil and EVBotsNow() - pre < 0.5 then
              DH.pre = (DH.pre or 0) + 1
            else
              -- Сборка 199: последний урон по движку — для Player:Killed (inflictor, info), как VU
              if giver ~= nil then rawset(victim, '_lastDmg', { giver = giver, at = EVBotsNow() }) end
              local n = Hooks._Fire('Soldier:Damage', sol, info, giverInfo)
              DH.fired = DH.fired + (n or 0)
              if info.damage ~= e[3] then DH.changed = DH.changed + 1 end
            end
          end
        end
      end
      -- ★ Сборка 159: входящий чат (EVBOTS_CHAT_EVENTS=1) → Player:Chat(игрок|nil, маска, текст). Из моста — NativeBridge.PollChat.
      -- Отправитель — объект моста по указателю (человек — по NativeBridge.RealPlayer); S у admin.say = 0 → nil.
      if NativeBridge.PollChat ~= nil and Events ~= nil and Events._DispatchEngine ~= nil then
        local okc, chs = pcall(NativeBridge.PollChat)
        if okc and type(chs) == 'table' and #chs > 0 then
          EVBotsChatLog = EVBotsChatLog or { n = 0 }
          for _, c in ipairs(chs) do
            -- ★ Сборка 202: отправитель — любой из людей (раньше чат второго человека приходил моду как «не игрок»)
            local sender = (c[1] ~= nil and c[1] ~= 0) and EVBotsPlayerByPtr(c[1]) or nil
            EVBotsChatLog.n = EVBotsChatLog.n + 1
            if EVBotsChatLog.n <= 30 then
              log("INFO ", string.format("ЧАТ: отправитель %s (%s), маска lo=%s hi=%s, тип %s: %s",
                tostring(c[1]), (sender ~= nil) and tostring(sender.name) or "не игрок моста",
                tostring(c[2]), tostring(c[3]), tostring(c[4]), tostring(c[5])))
            end
            local okd2, errd2 = pcall(Events._DispatchEngine, Events, 'Player:Chat', sender, c[2], c[5])
            if not okd2 then log("WRN  ", "ЧАТ: обработчик Player:Chat: " .. tostring(errd2)) end
          end
        end
      end
      EVBotsDmgHookT = (EVBotsDmgHookT or 0) + (deltaTime or 0)
      if EVBotsDmgHookT >= 20.0 and EVBotsDmgHook ~= nil then
        EVBotsDmgHookT = 0
        local DH = EVBotsDmgHook
        local wg, ng, lost = 0, 0, 0
        if NativeBridge.DamageStats ~= nil then wg, ng, lost = NativeBridge.DamageStats() end
        log("INFO ", string.format("ДИАГ хук Soldier:Damage: стрелок-человек %d; событий %d (со стрелком %d; мост: со стрелком %s, без %s, потеряно %s), вызовов обработчиков %d, бот-жертва со стрелком (ответный огонь) %d, без солдата %d, мод менял урон %d",
          DH.human or 0, DH.ev, DH.giver, tostring(wg), tostring(ng), tostring(lost), DH.fired, DH.botVictimGiver, DH.noSoldier, DH.changed))
        EVBotsDmgHook = nil
      end
    end
    -- ★ Сборка 153: Player:Respawn человека (каждое новое тело) и конец/начало раунда по тикетам движка:
    -- Server:RoundOver(roundTime, winningTeam) — тикеты команды дошли до 0; Server:RoundReset — тикеты снова > 0 у обеих.
    EVBotsRound = EVBotsRound or { t = 0, over = false, tick = 0, humanBody = false }
    local LR = EVBotsRound
    LR.t = LR.t + (deltaTime or 0)
    LR.tick = LR.tick + (deltaTime or 0)
    EVBotsBP('раунд')
    if LR.tick >= 0.5 then
      LR.tick = 0
      -- ★ Сборка 202: у каждого человека — своё тело и свой Player:Respawn (раньше — только первый человек)
      if NativeBridge.ReadU32 ~= nil then
        for i = 1, #players do
          local hpl = players[i]
          if hpl.onlineId ~= 0 then
            local hptr = EVBotsHumanPtr(hpl)
            local okb, sv = false, nil
            if hptr ~= nil and hptr ~= 0 then okb, sv = pcall(NativeBridge.ReadU32, hptr, 0x3C4) end
            local hb = okb and sv ~= nil and sv ~= 0
            if hb and not rawget(hpl, '_respBody') then
              pcall(Events._DispatchEngine, Events, 'Player:Respawn', hpl)
            end
            rawset(hpl, '_respBody', hb)
          end
        end
      end
      -- ★ Сборка 186: конец раунда и смена уровня — по сигналам движка, как VU, на любом режиме:
      --   Server:RoundOver(время, победитель) — ServerRoundOverMessage (RCON onMessage), запас — тикеты;
      --   Level:Destroy — уровень ушёл (указатель ServerLevel сменился/обнулился или имя не читается 1 с);
      --   Level:Loaded(уровень, режим, раунд, раундов) → Server:RoundReset — новый уровень читается и его сущности
      --   (точки спавна + флаги) есть и не меняются 1 с, как первый Level:Loaded при старте.
      -- Пока уровень меняется, ворота спавна закрыты (NativeBridge.SetLevelGate). Прогон TDM 18.10: тикетов на TDM нет —
      -- RoundOver не уходил, боты играли поверх таблицы счёта, на новом раунде мост сажал на мёртвые точки (814 исключений).
      local function roundOver(win, why)
        if LR.over then return end
        LR.over = true
        log("INFO ", string.format("РАУНД ОКОНЧЕН (%s): победила команда %d, время раунда %.0f с — Server:RoundOver", why, win or 0, LR.t))
        if NativeBridge.SetLevelGate ~= nil then pcall(NativeBridge.SetLevelGate, true) end
        -- ★ Сборка 265: движок уничтожает игроков-ботов уже после выгрузки контейнеров уровня — деструктор отпускает
        -- кит +0x480 и падает на уже разрушенном объекте (друг 07.11, EIP 0x468892, выгрузка XP5_003). Отпускаем ссылку
        -- сейчас, пока кит жив; на новом уровне спавн восстановит его из эталона/базы движка.
        if NativeBridge.ClearBotKits ~= nil then
          local okk, nk = pcall(NativeBridge.ClearBotKits)
          if not okk then log("WRN  ", "ClearBotKits: " .. tostring(nk)) end
        end
        local ok, err = pcall(Events._DispatchEngine, Events, 'Server:RoundOver', LR.t, win or 0)
        if not ok then log("WRN  ", "Server:RoundOver: " .. tostring(err)) end
      end
      local function levelDestroy(why)
        if LR.destroyed then return end
        roundOver(0, why)
        LR.destroyed = true
        EVBotsLevelReady = false
        LR.loadStable, LR.loadLast, LR.loadWait = 0, -1, 0
        log("INFO ", string.format("УРОВЕНЬ ВЫГРУЖЕН (%s) — Level:Destroy", why))
        if NativeBridge.SetLevelGate ~= nil then pcall(NativeBridge.SetLevelGate, true) end
        if NativeBridge.ResetLevelState ~= nil then pcall(NativeBridge.ResetLevelState) end
        local okd, errd = pcall(Events._DispatchEngine, Events, 'Level:Destroy')
        if not okd then log("WRN  ", "Level:Destroy: " .. tostring(errd)) end
        if EntityManager ~= nil and EntityManager._LevelReset ~= nil then pcall(EntityManager._LevelReset) end  -- сборка 186
        -- ★ Сборка 175: клиент VU на новом уровне снова входит в игру и шлёт Client:RequestSettings
        -- (ClientBotManager.lua:104, ClientStateEnteredIngameMessage); мод на Level:Destroy очищает _ActivePlayers.
        PlayerManager._LevelReset()
      end
      local sig = NativeBridge.LevelSignals ~= nil and { pcall(NativeBridge.LevelSignals, LR.destroyed == true) } or { false }
      if sig[1] then
        local overSeq, win, loadedSeq, lptr, ready, ents, hook = sig[2], sig[3], sig[4], sig[5], sig[6], sig[7], sig[8]
        if LR.hookLogged ~= hook then
          LR.hookLogged = hook
          log("INFO ", string.format("РАУНД: перехват конца раунда в движке = %s", hook == 1 and "есть" or (hook == -1 and "нет (тикеты)" or "ставится")))
        end
        local unloadSeq, startSeq = sig[10], sig[11]
        if LR.overSeq == nil then LR.overSeq, LR.loadedSeq = overSeq, loadedSeq end
        if LR.unloadSeq == nil then LR.unloadSeq = unloadSeq end
        -- сборка 186 (прогон 20:25): перехват onMessage теперь ставится до загрузки первой карты, и её server.onLevelLoaded
        -- приходил ПОСЛЕ того, как Lua запомнил номер. В конце раунда это сочли «новый уровень загружен»: Level:Loaded ушёл
        -- для старой MP_007, а настоящую загрузку Rush не заметили (тел 0). Пока раунд идёт — номер всегда свежий.
        -- ★ Сборка 211: но только до сигнала, с которым ушёл стартовый Level:Loaded (startSeq). Сигнал после него — новый уровень
        -- без конца раунда (mapList.runNextRound / restartRound: ServerRoundOverMessage движок не шлёт). Раньше такой сигнал
        -- проглатывался, и если тикеты не дали «0/0» (Rush → SQDM, прогон 20.10 09:24), Level:Destroy/Loaded не уходили вовсе.
        local reloaded = not LR.over and not LR.destroyed and startSeq ~= nil and startSeq >= 1
          and LR.loadedSeq ~= nil and LR.loadedSeq >= startSeq and loadedSeq ~= LR.loadedSeq
        if not LR.over and not LR.destroyed and not reloaded then LR.loadedSeq = loadedSeq end
        if overSeq ~= LR.overSeq then
          LR.overSeq = overSeq
          roundOver(win, 'движок')
        end
        -- ★ Сборка 211: движок выгружает уровень (Server+0x74 = 0, ловится в каждом тике) — Level:Destroy сразу, как VU.
        if unloadSeq ~= nil and unloadSeq ~= LR.unloadSeq then
          LR.unloadSeq = unloadSeq
          if not LR.destroyed then levelDestroy('движок: уровень выгружается') end
        end
        if reloaded and not LR.destroyed then levelDestroy('движок: новый уровень без конца раунда') end
        if not LR.destroyed then
          if ready and lptr ~= 0 then
            if LR.levelPtr == nil then LR.levelPtr = lptr end
          end
          local gone = LR.levelPtr ~= nil and (lptr ~= LR.levelPtr or not ready)
          LR.goneN = gone and (LR.goneN or 0) + 1 or 0
          if LR.goneN >= 2 then
            levelDestroy(lptr == 0 and 'уровня нет' or (ready and 'новый уровень' or 'имя уровня не читается'))
          elseif loadedSeq ~= LR.loadedSeq and LR.over then
            levelDestroy('движок: уровень загружен заново')
          end
        else
          LR.loadWait = (LR.loadWait or 0) + 0.5
          if ready and ents > 0 and ents == LR.loadLast then LR.loadStable = LR.loadStable + 1 else LR.loadStable = 0 end
          LR.loadLast = ents
          -- сборка 186: главный признак — сигнал движка server.onLevelLoaded (loadedSeq сменился) + сущности не меняются 1 с;
          -- без сигнала — 3 с; крайний срок 90 с (20 с мало для MP_007 — Level:Loaded при 0 сущностей, у мода 0 флагов)
          local engineLoaded = loadedSeq ~= LR.loadedSeq
          if (engineLoaded and LR.loadStable >= 2) or LR.loadStable >= 6 or (ready and LR.loadWait >= 90) then
            LR.destroyed, LR.over = false, false
            LR.levelPtr = lptr
            LR.loadedSeq = loadedSeq
            LR.seenTickets = false
            LR.round = (LR.round or 1) + 1
            local lvl, mode = 'Levels/MP_001/MP_001', 'ConquestSmall0'  -- запас; LevelReload берёт из движка (185)
            if NativeBridge.LevelReload ~= nil then
              local okr, l1, m1 = pcall(NativeBridge.LevelReload)
              if okr then lvl, mode = l1 or lvl, m1 or mode else log("WRN  ", "LevelReload: " .. tostring(l1)) end
            end
            log("INFO ", string.format("НОВЫЙ РАУНД %d: %s %s, сущностей %d, ждали %.1f с — Level:Loaded → Server:RoundReset", LR.round, lvl, mode, ents, LR.loadWait))
            -- ★ Сборка 266: как VU — очки/убийства/смерти считаются за раунд и на новой карте начинаются с нуля
            -- (движок сам их не сбрасывает: и боты, и человек переживают смену уровня теми же ServerPlayer).
            if NativeBridge.ResetRoundStats ~= nil then
              local okc, nc = pcall(NativeBridge.ResetRoundStats)
              if not okc then log("WRN  ", "ResetRoundStats: " .. tostring(nc)) end
            end
            LR.t = 0
            if NativeBridge.SetLevelGate ~= nil then pcall(NativeBridge.SetLevelGate, false) end
            -- ★ Сборка 198: round / roundsPerMap — у сервера (mapList.getRounds), как VU; LR.round — только счётчик для лога.
            -- Было LR.round и 2: fun-bots (SwitchForRoundTwo, p_Round ~= _LastRound) менял ботам команды на каждой загрузке.
            local rRound, rPerMap = LR.lastRound or 0, LR.lastPerMap or 1
            if EVBotsRconSend ~= nil then
              local okR, rr = pcall(EVBotsRconSend, { 'mapList.getRounds' }, 1500)
              if okR and type(rr) == 'table' and rr[1] == 'OK' and tonumber(rr[2]) ~= nil and tonumber(rr[3]) ~= nil then
                rRound, rPerMap = math.tointeger(tonumber(rr[2])) or 0, math.tointeger(tonumber(rr[3])) or 1
              else
                log("WRN  ", "Level:Loaded: mapList.getRounds не ответил — раунд " .. tostring(rRound) .. " из " .. tostring(rPerMap))
              end
            end
            LR.lastRound, LR.lastPerMap = rRound, rPerMap
            log("INFO ", string.format("Level:Loaded: раунд %d из %d (mapList.getRounds)", rRound, rPerMap))
            local okl, errl = pcall(Events._DispatchEngine, Events, 'Level:Loaded', lvl, mode, rRound, rPerMap)
            if not okl then log("WRN  ", "Level:Loaded: " .. tostring(errl)) end
            pcall(Events._DispatchEngine, Events, 'Server:RoundReset')
            EVBotsLevelReady = true
          end
        end
      end
      -- Сборка 186: лидеры отрядов, которых движок ещё не принял (бот создан позже назначения)
      for i = 1, #players do
        local p = players[i]
        if rawget(p, 'isSquadLeader') and not rawget(p, '_leaderSent') then pcall(EVBotsSendSquadLeader, p) end
      end
      -- ★ Сборка 186: ScoringSystem:StatEvent(player, objectPlayer, statEvent, paramX, paramY, value) — из перехвата движка
      if NativeBridge.PollStatEvents ~= nil then
        local okS, evs = pcall(NativeBridge.PollStatEvents)
        if okS and type(evs) == 'table' then
          for _, se in ipairs(evs) do
            local pl = EVBotsPlayerByPtr(se[1])
            EVBotsStatEvN = (EVBotsStatEvN or 0) + 1
            -- Сборка 187: убийства/смерти/самоубийства (VU-Docs fb/statevent: 0 Kill, 3 KillWith_X_As_Y, 7 TeamKill,
            -- 17 Death, 101 Suicide) — тоже всегда: разбор самоубийств гранатой (PLAN 1б). x — имя, если это указатель игрока.
            local se2 = se[2]
            local killEv = se2 == 0 or se2 == 3 or se2 == 7 or se2 == 17 or se2 == 101
            -- Сборка 187 (прогон 23:54: 3 смерти, событий 0/17 нет): каждый код — первые 5 раз, чтобы найти, под каким идут убийства
            EVBotsStatEvSeen = EVBotsStatEvSeen or {}
            local seenN = (EVBotsStatEvSeen[se2] or 0) + 1
            EVBotsStatEvSeen[se2] = seenN
            if EVBotsStatEvN <= 30 or seenN <= 5 or se2 == 86 or se2 == 87 or ((killEv or (se2 >= 53 and se2 <= 100)) and EVBotsStatEvN <= 600) then
              local xs = tostring(se[3])
              if killEv then
                local xu = se[3] % 4294967296
                for i = 1, #players do
                  if playerPtr(players[i]) == xu then xs = xs .. "=" .. tostring(players[i].name) break end
                end
              end
              log("INFO ", string.format("ScoringSystem:StatEvent %d (%s) x=%s y=%d value=%.1f", se2, pl and pl.name or string.format('%08X', se[1]), xs, se[4], se[5]))
            end
            if pl ~= nil then
              local okD, errD = pcall(Events._DispatchEngine, Events, 'ScoringSystem:StatEvent', pl, nil, se[2], se[3], se[4], se[5])
              if not okD then log("WRN  ", "ScoringSystem:StatEvent: " .. tostring(errD)) end
            end
          end
        end
      end
      -- ★ Сборка 193: приказы колеса BF3 человека → команды меню мода (BotEditor), как у VU от WebUI мода (UIServer.lua:40–100).
      -- OrderType (VU-Docs fb/ordertype.md): 1 RequestHeal, 3 RequestAmmo, 6 RequestRepair, 9 FollowMe, 15 GetIn, 16 GetOut.
      if NativeBridge.PollOrders ~= nil then
        local okO, ords = pcall(NativeBridge.PollOrders)
        if okO and type(ords) == 'table' and #ords > 0 then
          local orderAction = { [1] = 'drop_medkit', [3] = 'drop_ammo', [6] = 'repair_vehicle', [9] = 'follow_me',
                                [15] = 'enter_vehicle', [16] = 'exit_vehicle' }
          -- ★ Сборка 202: заказчик — любой из людей (у каждого свои игрок/солдат/машина); раньше — только первый человек движка.
          local humansO = {}
          for i = 1, #players do
            local pl = players[i]
            if pl.onlineId ~= 0 then
              local hptr = EVBotsHumanPtr(pl)
              if hptr ~= nil and hptr ~= 0 then
                local function rd(off)
                  local okR, v = pcall(NativeBridge.ReadU32, hptr, off)
                  return (okR and tonumber(v)) or 0
                end
                humansO[#humansO + 1] = { p = pl, ptr = hptr, s = rd(0x3C4), v = rd(0x3D8), c = rd(0x3E8) }
              end
            end
          end
          -- Сборка 194, факт прогона 193 (01:35:08, приказ «нужны патроны»): A = ServerPlayer заказчика (KOTuk1 F2DEA7C0),
          -- B = игрок-адресат (бот F1B95780); одно сообщение приходит в компонент каждого игрока (десятки строк в один тик) —
          -- повторы с тем же (A, B, тип) в пределах 1 с отбрасываем. Заказчик-человек — A == указатель одного из людей (202).
          EVBotsOrderSeen = EVBotsOrderSeen or {}
          local nowO = os.clock()
          for _, o in ipairs(ords) do
            local comp, a, b, ot = o[1], o[2], o[3], o[4]
            local key = string.format('%08X:%08X:%d', a, b, ot)
            local seenAt = EVBotsOrderSeen[key]
            if seenAt ~= nil and nowO - seenAt < 1.0 then goto nextOrder end
            EVBotsOrderSeen[key] = nowO
            local hp, hs, hv, hc = nil, 0, 0, 0
            for _, h in ipairs(humansO) do
              if a ~= 0 and (a == h.ptr or a == h.s or a == h.v or a == h.c) then
                hp, hs, hv, hc = h.p, h.s, h.v, h.c
                break
              end
            end
            local byHuman = hp ~= nil
            local act = orderAction[ot]
            EVBotsOrderN = (EVBotsOrderN or 0) + 1
            if EVBotsOrderN <= 300 then
              -- Сборка 195: имена OrderType BF3 (строки движка по порядку: тип 3 = RequestAmmo — факт 193, тип 7 = RequestRide —
              -- «подобрать меня», прогон 194). У мода «подобрать» нет (enter_vehicle — боты садятся в машину человека).
              local orderName = ({ [0] = 'None', 'RequestHeal', 'LetMeHeal', 'RequestAmmo', 'LetMeGiveAmmo', 'LetMeRepair',
                'RequestRepair', 'RequestRide', 'GoGoGo', 'FollowMe', 'Negative', 'Sorry', 'ThankYou', 'RogerThat', 'NeedBackup',
                'GetIn', 'GetOut', 'Attack' })[ot] or '?'
              log("INFO ", string.format("ПРИКАЗ колеса: тип %d (%s), A=%08X B=%08X компонент=%08X; человек: солдат %08X машина %08X/%08X → %s",
                  ot, orderName, a, b, comp, hs, hv, hc, (byHuman and act ~= nil) and ("моду: " .. act) or "не наш/без действия"))
            end
            if byHuman and act ~= nil and NetEvents ~= nil and NetEvents._Deliver ~= nil then
              local okE, errE = pcall(NetEvents._Deliver, NetEvents, 'BotEditor', hp, '{"action":"' .. act .. '"}')
              if not okE then log("WRN  ", "BotEditor (приказ колеса): " .. tostring(errE)) end
            end
            ::nextOrder::
          end
        end
      end
      -- запас (сборка 153): тикеты команды дошли до 0 — RoundOver; снова > 0 у обеих после конца — смена уровня
      if TicketManager ~= nil and TicketManager.GetTicketCount ~= nil and not LR.destroyed then
        local okt, t1 = pcall(TicketManager.GetTicketCount, TicketManager, 1)
        local okt2, t2 = pcall(TicketManager.GetTicketCount, TicketManager, 2)
        if okt and okt2 and type(t1) == 'number' and type(t2) == 'number' then
          if not LR.over and LR.seenTickets and (t1 <= 0 or t2 <= 0) then
            roundOver((t1 <= 0 and t2 > 0) and 2 or ((t2 <= 0 and t1 > 0) and 1 or 0), string.format('тикеты %d/%d', t1, t2))
          elseif LR.over and LR.seenTickets == false and t1 > 0 and t2 > 0 then
            levelDestroy(string.format('тикеты снова %d/%d', t1, t2))
          end
          if t1 > 0 and t2 > 0 and not LR.over then LR.seenTickets = true end
          if LR.over and (t1 <= 0 or t2 <= 0) then LR.seenTickets = false end
        end
      end
    end
    -- Сборка 152: «БОЙ 10с» и «ЧЕЛОВЕК» (раз в 20 с)
    if LF.clock >= 10.0 then
      LF.clock = 0
      LF.revDiag = not LF.revDiag
      if LF.revDiag then pcall(EVBotsReviveDiag) else pcall(EVBotsAirDiag); pcall(EVBotsObjDiag); pcall(EVBotsGadgetDiag) end  -- сборка 185: раз в 20 с каждая; 186: цели
      local W = LF.w
      local sts = {}
      for k, v in pairs(W.states) do sts[#sts + 1] = k .. '=' .. string.format('%.0f%%', W.samples > 0 and 100 * v / W.samples or 0) end
      table.sort(sts)
      log("INFO ", string.format("БОЙ 10с: состояния %s; с целью %.0f%%; ср. дистанция до цели %s м; тел %d; смертей урон %d, мод %d; всего урон %d, мод %d%s",
        table.concat(sts, ' '), W.samples > 0 and 100 * W.target / W.samples or 0,
        W.distN > 0 and string.format('%.0f', W.dist / W.distN) or '-', W.bodies, W.dmg, W.mod, LF.dmg, LF.mod,
        LF.firstDmgAt and string.format('; первая смерть от урона на %.0f с', LF.firstDmgAt) or ''))
      LF.w = { samples = 0, target = 0, dist = 0, distN = 0, states = {}, dmg = 0, mod = 0, bodies = 0 }
    end
    if LF.hclock >= 20.0 then
      LF.hclock = 0
      if NativeBridge.RealPlayer ~= nil and NativeBridge.BotWeaponNames ~= nil then
        local okh, hptr = pcall(NativeBridge.RealPlayer)
        if okh and hptr ~= nil and hptr ~= 0 then
          local okw, wv = pcall(NativeBridge.BotWeaponNames, hptr)
          local hb = (NativeBridge.ReadU32(hptr, 0x3C4) or 0) ~= 0
          local hpk = NativeBridge.ReadU32(hptr, 0xA4)
          log("INFO ", "ЧЕЛОВЕК: тело=" .. tostring(hb) .. " перковA4=" .. tostring(hpk) .. " | оружие движка: "
              .. (okw and tostring(wv) or '?'))
        end
      end
    end
    teamSyncClock = teamSyncClock + (deltaTime or 0)
    EVBotsBP('команды/синхр')
    if teamSyncClock >= 0.5 and NativeBridge.ReadU32 ~= nil then
      teamSyncClock = 0
      syncTicks = syncTicks + 1
      -- Человек в списке мода (как в VU: PlayerManager:GetPlayers() содержит людей). Сейчас в списке вместо
      -- настоящего человека служебный «Host» (onlineId ~= 0, команда 1 навсегда), и fun-bots в balanced_teams
      -- считает людей не в той команде -> лишние/недостающие боты (прогон 80: 16 ботов вместо 15). Подтягиваем
      -- команду и отряд Host-а к настоящему человеку из движка.
      -- Сборка 138: «человек в списке мода» появляется, когда настоящий человек зашёл на сервер (как в VU; до этого
      -- fun-bots не спавнит ботов: BotSpawner.lua:480 «если людей 0 — выход»). Раньше Host создавался на старте
      -- сервера: 15 ботов спавнились и крутили повторные спавны до входа клиента.
      -- Сборка 139: записи Host больше нет. В список мода попадает сам настоящий человек (как в VU): имя и команда
      -- из движка, onlineId 777 (не бот). Второго игрока в движке не создаём (CreateEnginePlayer пропускает это имя).
      -- Ушёл человек — через ~20 с (40 тиков по 0,5 с) его запись убираем: без людей мод ботов не спавнит (как у ВУ).
      -- ★ Сборка 202: в списке мода — КАЖДЫЙ человек сервера, как в VU. До 202 — только первый из движка (RealPlayer): второй
      -- человек для fun-bots не существовал — боты не видели его лучами и не отвечали на его огонь (друг, 19.10 вечером: «стоят
      -- в упор и не стреляют», «по ботам стреляешь — не реагируют»), а команда всех людей подтягивалась к команде первого.
      -- У каждого человека: свой указатель движка (_humanPtr), своя команда и отряд, свой onlineId — persona движка
      -- ([игрок+0x20], как VU player.onlineId); мод хранит активных игроков по onlineId (BotManager.lua:554/559), с общим 777
      -- второй человек активным не стал бы. Ушёл — запись убираем через ~20 с (40 проходов по 0,5 с), как раньше.
      if NativeBridge.Humans ~= nil then
        local okHl, hl, hlSure = pcall(NativeBridge.Humans)
        if okHl and type(hl) == 'table' then
          EVBotsHumanTeamWait = EVBotsHumanTeamWait or {}
          local seen = {}
          for _, e in ipairs(hl) do
            local hp, hname, hoid = e[1], e[2], e[3]
            if hp ~= nil and hp ~= 0 and type(hname) == 'string' and hname ~= '' then
              seen[hname] = true
              local rec = nil
              for i = 1, #players do
                if players[i].onlineId ~= 0 and players[i].name == hname then rec = players[i] break end
              end
              local okt0, ht0 = pcall(NativeBridge.ReadU32, hp, 0x31C)
              local team0 = (okt0 and ht0 ~= nil and ht0 >= 1 and ht0 <= 4) and ht0 or nil  -- сборка 186: SQDM — 4 команды
              if rec == nil then
                -- Сборка 145: пока движок не назначил человеку команду (+0x31C = 0), в список мода его не ставим.
                -- Раньше ставили с командой 1 «по умолчанию»; движок сам назначал 2 (прогон 144: 0 -> 2 через 19 с,
                -- пользователь РФ не выбирал), balanced_teams мода пересчитывал и убирал лишнего бота без тела.
                if team0 == nil then
                  if not EVBotsHumanTeamWait[hname] then
                    EVBotsHumanTeamWait[hname] = true
                    log("INFO ", "мост: человек " .. hname .. " на сервере, команда в движке ещё 0 — ждём назначения")
                  end
                else
                  EVBotsHumanTeamWait[hname] = nil
                  -- Сборка 155: VU-событие Player:Joining(name, guid, ip, accountGuid) — до записи, как в VU.
                  -- Guid/IP/аккаунта в мосте нет: передаём nil (мод читает только имя).
                  if EVBOTS_NOHUMANEVENTS ~= true then
                    pcall(Events._DispatchEngine, Events, 'Player:Joining', hname, nil, nil, nil)
                  end
                  -- Сборка 217: в консоль окна сервера, как у VU («Player 'X' is requesting to join the server.»)
                  if evbotsconsole ~= nil then pcall(evbotsconsole, "info", "EVBots", "Player '" .. tostring(hname) .. "' is requesting to join the server.") end
                  local okc, hp2 = pcall(PlayerManager.CreatePlayer, PlayerManager, hname, team0, 0)
                  if okc and hp2 ~= nil then
                    local used = {}
                    for i = 1, #players do
                      if players[i] ~= hp2 and players[i].onlineId ~= 0 then used[players[i].onlineId] = true end
                    end
                    local oid = (type(hoid) == 'number' and hoid ~= 0 and not used[hoid]) and hoid or nil
                    if oid == nil then
                      oid = 777
                      while used[oid] do oid = oid + 1 end
                    end
                    hp2.onlineId = oid
                    -- Joining is not spawning: the native body mirror sets
                    -- alive after the human presses Ready and spawns.
                    hp2.alive = false
                    rawset(hp2, '_humanPtr', hp)
                    PlayerManager._AdoptEngineId(hp2, hp)  -- ★ 231: id человека = id движка, как VU
                    local nh = 0
                    for i = 1, #players do if players[i].onlineId ~= 0 then nh = nh + 1 end end
                    log("INFO ", string.format("мост: человек зашёл — он сам в списке мода: %s (команда %s, onlineId %s, игрок %08X; людей в списке %d)",
                        hname, tostring(team0), tostring(oid), hp, nh))
                    -- Созданный человек уже имеет native id/onlineId и alive=false.
                    -- Royale создаёт BRPlayer по Player:Created до выбора внешности.
                    if EVBOTS_NOHUMANEVENTS ~= true then
                      local okp, errp = pcall(Events._DispatchEngine, Events, 'Player:Created', hp2)
                      if not okp then log("ERR ", "Player:Created не разослан: " .. tostring(errp)) end
                      local oka, erra = pcall(Events._DispatchEngine, Events, 'Player:Authenticated', hp2)
                      if not oka then log("ERR ", "Player:Authenticated не разослан: " .. tostring(erra)) end
                    end
                    if evbotsconsole ~= nil then pcall(evbotsconsole, "info", "EVBots", "Player '" .. tostring(hname) .. "' has established a connection to the server.") end
                  else
                    log("ERR ", "мост: человек не добавлен в список мода: " .. tostring(hp2))
                  end
                end
              else
                rawset(rec, '_humanMissing', nil)
                rawset(rec, '_humanPtr', hp)
                if team0 ~= nil then
                  local oks, hs = pcall(NativeBridge.EngineSquad, hp)
                  if rawget(rec, '_observedTeamId') ~= team0 then
                    local oldTeam = rawget(rec, '_observedTeamId') or 0
                    rawset(rec, '_observedTeamId', team0)
                    rawset(rec, '_teamId', team0)
                    if oks and hs ~= nil and hs >= 0 then rawset(rec, '_squadId', hs) end
                    log("INFO ", "команда человека (" .. tostring(rec.name) .. ") в движке: " ..
                        tostring(oldTeam) .. " -> " .. tostring(team0))
                    pcall(Events._DispatchEngine, Events, 'Player:TeamChange', rec, team0, rec.squadId, oldTeam)
                  elseif oks and hs ~= nil and hs >= 0 and rec.squadId ~= hs then
                    rawset(rec, '_squadId', hs)
                  end
                end
              end
            end
          end
          for i = #players, 1, -1 do
            local p = players[i]
            if p.onlineId ~= 0 and not seen[p.name] then
              rawset(p, '_humanPtr', nil)  -- указатель ушедшего игрока движка больше не трогаем
              local miss = (rawget(p, '_humanMissing') or 0) + 1
              rawset(p, '_humanMissing', miss)
              -- ★ Сборка 214 (прогон 213: человек ушёл из движка 04:22:48, Player:Left мод получил в 04:23:07): у VU Player:Left —
              -- событие движка, сразу. Уровень стоит, а человека нет в списке игроков движка — ушёл: убираем в этом же проходе.
              -- ~20 с (40 проходов) ждём только во время смены уровня или если список движка прочитан не уверенно.
              local changing = true
              if NativeBridge.LevelChanging ~= nil then
                local okc, c = pcall(NativeBridge.LevelChanging)
                if okc then changing = c and true or false end
              end
              if miss >= 40 or (not changing and hlSure == true) then
                log("INFO ", "мост: человек ушёл — запись убрана из списка мода: " .. tostring(p.name))
                -- Сборка 155: Player:Left(player) — пока запись ещё есть. Мод чистит по нему ссылки ботов на игрока
                -- и список активных игроков (BotManager:OnPlayerLeft): без события список не пустел, и бот-бот бой шёл без людей.
                if EVBOTS_NOHUMANEVENTS ~= true then
                  pcall(Events._DispatchEngine, Events, 'Player:Left', p)
                end
                if evbotsconsole ~= nil then pcall(evbotsconsole, "info", "EVBots", "Player '" .. tostring(p.name) .. "' left the server.") end
                pcall(PlayerManager.DeletePlayer, PlayerManager, p)
                if EVBOTS_NOHUMANEVENTS ~= true then
                  pcall(Events._DispatchEngine, Events, 'Player:Destroyed', p)
                end
              end
            end
          end
        end
      end
      for i = 1, #players do
        local p = players[i]
        if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 then
          local okt, et = pcall(NativeBridge.ReadU32, p.nativePtr, 0x31C)
          if okt and et ~= nil and et >= 1 and et <= 127 and et ~= rawget(p, '_observedTeamId') then
            local oldTeam = rawget(p, '_observedTeamId') or 0
            rawset(p, '_observedTeamId', et)
            rawset(p, '_teamId', et)
            -- отряд старой команды не годится: выбираем, как fun-bots (_GetSquadToJoin) — первый, где < 4
            p.squadId = 0
            for sq = 1, 31 do
              if TeamSquadManager:GetSquadPlayerCount(et, sq) < 4 then
                p.squadId = sq
                break
              end
            end
            pcall(NativeBridge.SetBotSquad, p.nativePtr, p.squadId)
            -- позиция, выбранная модом для старой команды (база противника), больше не годится
            pcall(NativeBridge.ClearBotPos, p.nativePtr)
            log("INFO ", "команда бота " .. tostring(p.name) .. " в движке сменилась: " ..
                tostring(oldTeam) .. " -> " .. tostring(et))
            local okd, errd = pcall(Events._DispatchEngine, Events, 'Player:TeamChange', p, et,
                                    p.squadId, oldTeam)
            if not okd then
              log("ERR ", "Player:TeamChange не разослан: " .. tostring(errd))
            end
          end
          -- Отряд в движке должен совпадать с p.squadId (выбор мода). Движок сбрасывает отряд при переносе бота
          -- в другую команду и при ряде событий; мост ставил отряд только на спавне, поэтому у живых ботов
          -- он «слетал» (прогон 81: Bravo 2/4, Charlie 2/4, «не в отряде» 1). Подводим раз в ~2 с на бота.
          if p.squadId ~= nil and p.squadId >= 1 and p.squadId <= 31 and NativeBridge.JoinSquad ~= nil then
            local oke, es = pcall(NativeBridge.EngineSquad, p.nativePtr)
            if oke and es ~= nil and es >= 0 and es ~= p.squadId
                and (p._sqAt == nil or syncTicks - p._sqAt >= 4) then
              p._sqAt = syncTicks
              local okj, rj = pcall(NativeBridge.JoinSquad, p.nativePtr, p.squadId)
              if not (okj and rj == 1) then
                -- отряд полон (движок: 4 человека) — берём первый со свободным местом
                for sq = 1, 31 do
                  if sq ~= p.squadId and TeamSquadManager:GetSquadPlayerCount(p.teamId, sq) < 4 then
                    local ok2, r2 = pcall(NativeBridge.JoinSquad, p.nativePtr, sq)
                    if ok2 and r2 == 1 then p.squadId = sq end
                    break
                  end
                end
              end
            end
          end
        end
      end
    end
    EVBotsBP('мёртвые')
    local ok, dead = pcall(NativeBridge.PollDeadBots)
    if not ok or dead == nil then
      EVBotsBP(nil)
      return
    end
    for _, ptr in ipairs(dead) do
      for i = 1, #players do
        local p = players[i]
        if p.nativePtr == ptr then
          -- Сборка 199: Player:Killed как VU (player_killed.md: player, inflictor, position, weapon, isRoadKill, isHeadShot,
          -- wasVictimInReviveState, info) — позиция тела (последняя, что видел мост) и убийца из последнего урона движка (≤5 с).
          local deathPos, inflictor = nil, nil
          pcall(function()
            local s0 = rawget(p, 'soldier')
            local bp0 = s0 ~= nil and rawget(s0, 'bridgePrev') or nil
            if bp0 ~= nil then deathPos = Vec3._make(bp0[1], bp0[2], bp0[3]) end
          end)
          local ld = rawget(p, '_lastDmg')
          if ld ~= nil and EVBotsNow() - (ld.at or -100) <= 5 then inflictor = ld.giver end
          rawset(p, '_lastDmg', nil)
          p._killArgs = { pos = deathPos, by = inflictor }
          p.soldier = nil
          p.hasSoldier = false
          p._engineDead = true  -- сборка 185: тело умерло в движке — если вернётся без спавна мода, это реанимация
          if EVBOTS_SOLDIER_VU ~= false then p.alive = false end  -- сборка 135: как в VU (alive = движковый isAlive)
          log("DBG ", "бот умер, soldier=nil: " .. tostring(p.name))
          if p.luaKilled then
            -- Сборка 153: мод уже получил Player:Killed («убран без тела»), тело убил мост — второй раз не шлём
            p.luaKilled = nil
            log("DBG ", "бот " .. tostring(p.name) .. ": тело умерло после Kill мода — Player:Killed уже был, повтор не шлём")
            break
          end
          -- Событие движка: аргумент — объект Player, поэтому не Events:Dispatch (он пропускает только
          -- скаляры и таблицы из них и молча отказывал — прогон 77: Player:Killed не получал никто).
          local ka = rawget(p, '_killArgs') or {}
          local kinfo = ka.by ~= nil and { giver = ka.by } or nil
          local okd, errd = pcall(Events._DispatchEngine, Events, 'Player:Killed', p, ka.by, ka.pos, nil,
                                  false, false, false, kinfo)
          if not okd then
            log("ERR ", "Player:Killed не разослан: " .. tostring(errd))
          end
          break
        end
      end
    end
    EVBotsBP(nil)  -- ★ 164: закрыть последний блок тика (иначе в него попадёт простой между тиками)
  end)
end

-- ── RaycastManager (shared\library\raycastmanager.md) ───────────────────────
-- ★ Сборка 185: до 185 глобала не было — мод зовёт RaycastManager:CollisionRaycast перед реанимацией (BotManager.lua:1572:
-- пусто = тело видно → Revive). Лучи — очередь движка (LosRequest/LosResult, GameWorld::rayCast, тип collision,
-- флаги 0x34: без воды, персонажей и регдоллов). В VU вызов синхронный; Lua моста — в своём потоке, поэтому ответ берём из
-- кэша: первый запрос отрезка — «закрыто» (одно попадание), готовый результат живёт 2 с. Попадание — точка «to»
-- (поле position; мод смотрит только число попаданий). DetailedRaycast (редактор путей) — то же.
-- ★ Сборка 185: ДИАГ авиации (только чтение): вертолёты и самолёты по имени данных машины, пилот-бот, высота над точкой,
-- где машину увидели впервые, и скорость по смещению. «В воздухе» — выше на 8 м или быстрее 15 м/с.
EVBotsAir = EVBotsAir or { seen = {}, maxUp = { heli = 0, jet = 0 }, maxV = { heli = 0, jet = 0 }, flew = { heli = 0, jet = 0 },
                           flewIds = {}, t = os.clock() }
-- ★ Сборка 186: цели мода (GameDirector.m_AllObjectives) и цели ботов по командам — раз в 20 с. Жалоба 18.10: боты США
-- в основном стоят на точке A и не идут захватывать; во 2-м раунде не садятся в технику (цели isEnterVehiclePath).
-- ★ Сборка 186: гаджеты штурмовиков и пулемётчиков (вопрос пользователя «раскладывают ли аптечки и патроны»): сколько ботов
-- мод отправил раскладывать (_WeaponToUse = Gadget1, Bot:DeployIfPossible), сколько в движке держат аптечку/ящик (текущий
-- слот), какие слоты в руках. Итог раскладки — счётчик SupplySphere в «ДИАГ список техники».
function EVBotsGadgetDiag()
  local bm = rawget(_G, 'g_BotManager')
  if type(bm) ~= 'table' or NativeBridge.WeaponCurSlot == nil or NativeBridge.BotWeaponNames == nil then return end
  local bots = rawget(bm, '_Bots') or {}
  local order, holding, kits, slots = 0, 0, 0, {}
  for i = 1, #bots do
    local b = bots[i]
    local p = rawget(b, 'm_Player')
    local ptr = p and p.nativePtr
    local kit = rawget(b, 'm_Kit')
    if ptr ~= nil and ptr ~= 0 and BotKits ~= nil and (kit == BotKits.Assault or kit == BotKits.Support) and p.soldier ~= nil then
      kits = kits + 1
      if BotWeapons ~= nil and rawget(b, '_WeaponToUse') == BotWeapons.Gadget1 then order = order + 1 end
      local okS, sl = pcall(NativeBridge.WeaponCurSlot, ptr)
      if okS and sl ~= nil then
        slots[sl] = (slots[sl] or 0) + 1
        local okN, names = pcall(NativeBridge.BotWeaponNames, ptr)
        if okN and type(names) == 'string' then
          local nm = names:match('%f[%d]' .. tostring(sl) .. '=(U_[%w_]+)')
          if nm ~= nil and (nm:find('Medkit') or nm:find('Ammobag')) then holding = holding + 1 end
        end
      end
    end
  end
  local sl = {}
  for k, v in pairs(slots) do sl[#sl + 1] = tostring(k) .. '=' .. v end
  table.sort(sl)
  log("INFO ", string.format("ДИАГ гаджеты: штурмовиков и пулемётчиков с телом %d; мод велит раскладывать %d; в руках аптечка/ящик %d; слоты в руках [%s]",
    kits, order, holding, table.concat(sl, ' ')))
end

function EVBotsObjDiag()
  local gd = rawget(_G, 'g_GameDirector')
  local bm = rawget(_G, 'g_BotManager')
  if type(gd) ~= 'table' or type(bm) ~= 'table' then return end
  local objs = rawget(gd, 'm_AllObjectives') or {}
  local parts, veh = {}, 0
  for i = 1, #objs do
    local o = objs[i]
    if o.isEnterVehiclePath then
      if o.active then veh = veh + 1 end
    elseif not o.subObjective then
      local as = o.assigned or {}
      parts[#parts + 1] = string.format('%s т%s%s%s%s [%s/%s]', tostring(o.name), tostring(o.team), o.active and '' or ' выкл',
        o.isBase and ' база' or '', o.destroyed and ' уничтожена' or '', tostring(as[1] or 0), tostring(as[2] or 0))
    end
  end
  local per = { {}, {} }
  local bots = rawget(bm, '_Bots') or {}
  for i = 1, #bots do
    local b = bots[i]
    local okT, t = pcall(function() return b.m_Player.teamId end)
    local ob = rawget(b, '_Objective') or ''
    local md = rawget(b, '_ObjectiveMode')
    if okT and (t == 1 or t == 2) then
      local k = (ob == '' and '-' or ob) .. (md ~= nil and (':' .. tostring(md)) or '')
      per[t][k] = (per[t][k] or 0) + 1
    end
  end
  local function fmt(m)
    local a = {}
    for k, v in pairs(m) do a[#a + 1] = k .. '=' .. v end
    table.sort(a)
    return table.concat(a, ' ')
  end
  local rush = (Globals and Globals.IsRush) and ('; Rush этап=' .. tostring(gd.m_RushStageCounter)
    .. ' база=' .. tostring(gd.m_RushAttackingBase)) or ''
  log("INFO ", string.format("ДИАГ цели: загружено=%s, целей %d, путей в технику активно %d%s | %s || т1: %s || т2: %s",
    tostring(rawget(gd, 'm_MapCompletelyLoaded')), #objs, veh, rush, table.concat(parts, '; '), fmt(per[1]), fmt(per[2])))
end

local function airClass(name)
  local n = string.lower(tostring(name or ''))
  -- ★ Сборка 211: поиск по простому тексту. С шаблоном Lua '-' — квантификатор: 'su-25' ловил «2s25» (2S25 «Спрут»), 'z-11' —
  -- «m1128» (Stryker); прогон 22.10 на XP3_Shield (самолётов нет) писал «самолёты не взлетают выше 9 м» про наземную технику.
  local str = n
  n = { find = function(_, s) return string.find(str, s, 1, true) end }
  if n:find('ah1z') or n:find('mi28') or n:find('ah6') or n:find('z11') or n:find('z-11') or n:find('venom') or n:find('ka60')
      or n:find('ka-60') or n:find('uh1') or n:find('littlebird') or n:find('heli') or n:find('chopper') or n:find('mi-28')
      or n:find('kv46') or n:find('ah-1') then return 'heli' end
  if n:find('f35') or n:find('f-35') or n:find('f18') or n:find('fa18') or n:find('su35') or n:find('su-35') or n:find('su37')
      or n:find('su-37') or n:find('a10') or n:find('a-10') or n:find('su25') or n:find('su-25') or n:find('jet')
      or n:find('ac130') then return 'jet' end
  return nil
end
function EVBotsAirDiag()
  if EntityManager == nil or EntityManager.GetIterator == nil then return end
  local A = EVBotsAir
  local now = os.clock()
  local cnt = { heli = 0, jet = 0 }
  local piloted = { heli = 0, jet = 0 }
  local inAir = { heli = 0, jet = 0 }
  local names = {}
  local okI, it = pcall(EntityManager.GetIterator, EntityManager, 'ServerVehicleEntity')
  if not okI or it == nil then return end
  local e = it:Next()
  while e ~= nil do
    local okn, nm = pcall(function() return VehicleEntityData(e.data).controllableType end)
    if not okn then nm = nil end
    local cls = airClass(nm)
    if cls ~= nil then
      cnt[cls] = cnt[cls] + 1
      local okp, tr = pcall(function() return e.transform.trans end)
      local okd, drv = pcall(function() return e:GetPlayerInEntry(0) end)
      local id = rawget(e, '_ptr') or tostring(e)
      if okp and tr ~= nil then
        local st = A.seen[id]
        if st == nil then st = { y0 = tr.y, x = tr.x, y = tr.y, z = tr.z, t = now }; A.seen[id] = st end
        local dt = now - st.t
        local v = 0
        local okv, vm = pcall(function() return e.velocity.magnitude end)
        if okv and type(vm) == 'number' then v = vm
        elseif dt > 0.5 then v = math.sqrt((tr.x - st.x) ^ 2 + (tr.y - st.y) ^ 2 + (tr.z - st.z) ^ 2) / dt end
        st.x, st.y, st.z, st.t = tr.x, tr.y, tr.z, now
        local up = tr.y - st.y0
        if okd and drv ~= nil and drv.onlineId == 0 then
          piloted[cls] = piloted[cls] + 1
          if up > A.maxUp[cls] then A.maxUp[cls] = up end
          if v > A.maxV[cls] and v < 400 then A.maxV[cls] = v end
          if up > 8 or v > 15 then
            inAir[cls] = inAir[cls] + 1
            if not A.flewIds[id] then A.flewIds[id] = true; A.flew[cls] = A.flew[cls] + 1 end
          end
          if #names < 4 then names[#names + 1] = string.format('%s:%s +%.0f м %.0f м/с', tostring(nm):match('[^/]+$') or '?', tostring(drv.name), up, v) end
        end
      end
    end
    e = it:Next()
  end
  log("INFO ", string.format("ДИАГ авиация: вертолётов %d (пилот-бот %d, в воздухе %d), самолётов %d (пилот-бот %d, в воздухе %d); за прогон взлетало верт %d / сам %d, макс высота верт %.0f / сам %.0f м, макс скорость верт %.0f / сам %.0f м/с [%s]",
    cnt.heli, piloted.heli, inAir.heli, cnt.jet, piloted.jet, inAir.jet, A.flew.heli, A.flew.jet, A.maxUp.heli, A.maxUp.jet,
    A.maxV.heli, A.maxV.jet, table.concat(names, '; ')))
  A.cnt = cnt
  EVBotsPilotDiag(true)
end
-- Сборка 185: строки пилотов авиации (из EVBotsAirDiag раз в 20 с и раз в 2 с, пока есть пилоты вертолётов — до 80 строк).
function EVBotsPilotDiag(full)
  local bm = rawget(_G, 'g_BotManager')
  local S = (g_BotStates ~= nil) and g_BotStates.States or nil
  if type(bm) == 'table' and S ~= nil then
    local rows = {}
    for _, b in ipairs(rawget(bm, '_Bots') or {}) do
      if #rows >= 4 then break end
      local st = b.m_ActiveState
      local nm = (st == S.InVehicleChopperControl and 'верт') or (st == S.InVehicleJetControl and 'сам') or nil
      if nm ~= nil then
        local ok, txt = pcall(function()
          local cc = b.m_Player.controlledControllable
          local vy = cc and cc.transform.trans.y or -1
          local tp = b._TargetPoint
          local ty = (tp ~= nil and tp.Position ~= nil) and tp.Position.y or nil
          local inp = b.m_Player.input
          local thr = (type(inp) == 'table' and type(inp._levels) == 'table') and (inp._levels[EntryInputActionEnum.EIAThrottle] or 0) or -1
          local eu = cc and cc.transform:ToQuatTransform(false).rotation:ToEuler() or nil
          local T = cc and cc.transform or nil
          local lv = (type(inp) == 'table' and type(inp._levels) == 'table') and inp._levels or {}
          -- ★ Сборка 211 (друг: «самолёты просто летают, иногда стреляют»): бой по полям мода — цель (_ShootPlayer, у самолёта
          -- только воздушная, AirTargets), готов стрелять (нос на цели < 0,2 рад, VehicleJetControl.lua:191), огонь, слот оружия.
          local sp = rawget(b, '_ShootPlayer')
          local at = rawget(_G, 'g_AirTargets')
          local nAir = (type(at) == 'table' and type(rawget(at, '_Targets')) == 'table') and #at._Targets or -1
          local fight = string.format(' | бой: цель %s (тип %s) готов=%s огонь=%.2f слот=%s, воздушных целей у мода %d',
            sp ~= nil and tostring(sp.name) or 'нет', tostring(rawget(b, '_ShootPlayerVehicleType')),
            tostring(rawget(b, '_VehicleReadyToShoot')), lv[EntryInputActionEnum.EIAFire] or 0,
            tostring(rawget(b, '_ActiveVehicleWeaponSlot')), nAir)
          return string.format('%s %s: машина y=%.0f цель y=%s ждать=%.1f газ=%.2f место=%s эйлер (%.2f, %.2f, %.2f) up (%.2f, %.2f, %.2f) fwd (%.2f, %.2f, %.2f) ввод рыск %.2f тан %.2f крен %.2f%s',
            nm, tostring(b.m_Player.name), vy, ty and string.format('%.0f', ty) or 'нет', tonumber(b._VehicleWaitTimer) or -1, thr,
            tostring(b.m_Player.controlledEntryId), eu and eu.x or 0, eu and eu.y or 0, eu and eu.z or 0,
            T and T.up.x or 0, T and T.up.y or 0, T and T.up.z or 0, T and T.forward.x or 0, T and T.forward.y or 0, T and T.forward.z or 0,
            lv[EntryInputActionEnum.EIAYaw] or 0, lv[EntryInputActionEnum.EIAPitch] or 0, lv[EntryInputActionEnum.EIARoll] or 0, fight)
        end)
        rows[#rows + 1] = ok and txt or ('ошибка ' .. tostring(txt))
      end
    end
    if #rows > 0 and not full then
      local heli = false
      for _, r in ipairs(rows) do if r:find('^верт') then heli = true end end
      if not heli or (EVBotsPilotFast or 0) >= 80 then rows = {} else EVBotsPilotFast = (EVBotsPilotFast or 0) + 1 end
    end
    if #rows > 0 then log("INFO ", "ДИАГ авиация пилоты: " .. table.concat(rows, ' | ')) end
    if not full then return end
    -- цель авиации у мода — позиция флага (GameDirector:GetActiveTargetPointPosition) + 110 м
    local gd = rawget(_G, 'g_GameDirector')
    if type(gd) == 'table' and #rows > 0 then
      local ok, txt = pcall(function()
        local cps = rawget(gd, '_AllCapturePoints') or {}
        local first = cps[1]
        local ft = first and first.transform and first.transform.trans
        local t1 = gd:GetActiveTargetPointPosition(1)
        local t2 = gd:GetActiveTargetPointPosition(2)
        return string.format('флагов у мода %d, первый %s (%s); цель команды 1 (%.0f, %.0f, %.0f), команды 2 (%.0f, %.0f, %.0f); таймер %.1f, конквест %s',
          #cps, ft and string.format('(%.0f, %.0f, %.0f)', ft.x, ft.y, ft.z) or 'нет', first and tostring(first.name) or '-',
          t1.x, t1.y, t1.z, t2.x, t2.y, t2.z, tonumber(rawget(gd, 'm_UpdateTimer')) or -9, tostring(Globals and Globals.IsConquest))
      end)
      log("INFO ", "ДИАГ авиация цель: " .. (ok and txt or ('ошибка ' .. tostring(txt))))
    end
  end
end

-- ★ Сборка 185: ДИАГ реанимации (только чтение) — те же условия, что BotManager:_CheckForBotBotRevive (BotManager.lua:1480).
function EVBotsReviveDiag()
  local bm = rawget(_G, 'g_BotManager')
  if type(bm) ~= 'table' then return end
  local bots = rawget(bm, '_Bots') or {}
  local act = rawget(bm, '_ActivePlayers') or {}
  local idle = (g_BotStates ~= nil and g_BotStates.States ~= nil) and g_BotStates.States.Idle or nil
  local medics, noDefib, deadIdle, deadOther, corpses, live, dontRevive = 0, 0, 0, 0, 0, 0, 0
  local w6 = {}
  local medPos, corpsePos, reviving = {}, {}, 0
  for _, b in ipairs(bots) do
    if BotActionFlags ~= nil and b._ActiveAction == BotActionFlags.ReviveActive then reviving = reviving + 1 end
    local p = b.m_Player
    if p ~= nil then
      if p.soldier == nil then
        if idle ~= nil and b.m_ActiveState == idle then deadIdle = deadIdle + 1 else deadOther = deadOther + 1 end
        if b.m_DontRevive then dontRevive = dontRevive + 1 end
        local c = p.corpse
        if c ~= nil then
          corpses = corpses + 1
          if not c.isDead then
            live = live + 1
            local okc, cp = pcall(function() return c.physicsEntityBase.position end)
            if okc and cp ~= nil then corpsePos[#corpsePos + 1] = { p = cp, t = p.teamId } end
          end
        end
      elseif BotKits ~= nil and b.m_Kit == BotKits.Assault then
        local ok, nm = pcall(function()
          local w = p.soldier.weaponsComponent.weapons[6]
          return w ~= nil and w.name or nil
        end)
        if ok and nm ~= nil and string.find(nm, 'Defibrillator') then
          medics = medics + 1
          local okm, mp = pcall(function() return p.soldier.worldTransform.trans end)
          if okm and mp ~= nil then medPos[#medPos + 1] = { p = mp, t = p.teamId } end
        else noDefib = noDefib + 1; if #w6 < 4 then w6[#w6 + 1] = tostring(ok and nm or 'ошибка') end end
      end
    end
  end
  local dists = {}
  for _, cp in ipairs(corpsePos) do
    local best = nil
    for _, mp in ipairs(medPos) do
      if mp.t == cp.t then
        local d = math.sqrt((mp.p.x - cp.p.x) ^ 2 + (mp.p.y - cp.p.y) ^ 2 + (mp.p.z - cp.p.z) ^ 2)
        if best == nil or d < best then best = d end
      end
    end
    dists[#dists + 1] = best ~= nil and string.format('%.0f', best) or '-'
  end
  local rs = EVBotsRcStat or { clear = 0, blocked = 0, pending = 0 }
  log("INFO ", string.format("ДИАГ реанимация+: ближайший медик своей команды до трупов, м [%s]; медиков в режиме реанимации %d; лучи: чисто %d, закрыто %d, новых запросов %d",
    table.concat(dists, ','), reviving, rs.clear, rs.blocked, rs.pending))
  log("INFO ", string.format("ДИАГ реанимация: людей у мода %d; медиков с дефибриллятором %d, без %d [%s]; мёртвых ботов в Idle %d, в другом состоянии %d, «не поднимать» %d; трупов %d, из них можно поднять %d; реанимаций всего %d",
    #act, medics, noDefib, table.concat(w6, ','), deadIdle, deadOther, dontRevive, corpses, live, EVBotsRevives or 0))
  -- Сборка 197: isAllowedToSpawnOn — из движка (Player.__index → NativeBridge.PlayerSpawnOn). Сверка раз в 20 с: отряд из
  -- поля [ext+0xF4] против геттера движка (vtable+0x20) — подтверждает раскладку подобъекта отряда; сколько ботов с 1/0.
  if NativeBridge ~= nil and NativeBridge.PlayerSpawnOn ~= nil and NativeBridge.EngineSquad ~= nil then
    local on, off, offN, human, same, diff = 0, 0, {}, '-', 0, 0
    local okP, pls = pcall(function() return PlayerManager:GetPlayers() end)
    if okP and type(pls) == 'table' then
      for _, p in ipairs(pls) do
        local ptr = playerPtr(p)
        if ptr ~= nil and ptr ~= 0 then
          local okR, v, sqField = pcall(NativeBridge.PlayerSpawnOn, ptr)
          local okS, sq = pcall(NativeBridge.EngineSquad, ptr)
          if okR and v ~= nil then
            if okS and sq == sqField then same = same + 1 else diff = diff + 1 end
            if (rawget(p, 'onlineId') or 0) ~= 0 then human = tostring(v) end
            if v then on = on + 1 else off = off + 1; if #offN < 6 then offN[#offN + 1] = tostring(p.name) end end
          end
        end
      end
    end
    log("INFO ", string.format("ДИАГ isAllowedToSpawnOn движка ([ext+0xFB]): true у %d, false у %d [%s]; человек %s | " ..
      "отряд [ext+0xF4] = геттеру движка у %d, не равно у %d", on, off, table.concat(offN, ','), human, same, diff))
  end
end

if rawget(_G, 'RaycastManager') == nil then
  local RM = {}
  local cache, cacheN = {}, 0
  Events:Subscribe('Level:Destroy', function()
    cache, cacheN = {}, 0
    EVBotsRcStat = nil
  end)
  local function hitList(to)
    local pos = (to ~= nil and to.Clone ~= nil) and to:Clone() or to
    return { { position = pos, normal = Vec3._make(0, 1, 0), rigidBody = nil, material = nil } }
  end
  -- Ключ — цель (с точностью 0,5 м) и отправная точка: результат годится, пока стрелок/медик сдвинулся < 3 м.
  -- (Прогон 06:30: ключ по обеим точкам с точностью 0,1 м — медик за 0,5 с между проверками уходил на 2 м, каждый раз
  -- новый запрос и ответ «ещё нет» = «закрыто»; за прогон один «чисто» и 0 реанимаций.)
  local function cast(from, to)
    if from == nil or to == nil or NativeBridge == nil or NativeBridge.LosRequest == nil then return {} end
    if EVBotsLevelReady == false then return hitList(to) end
    local key = string.format('%.0f,%.0f,%.0f', to.x * 2, to.y * 2, to.z * 2)
    local now = os.clock()
    local list = cache[key]
    if list == nil then list = {}; cache[key] = list; cacheN = cacheN + 1 end
    local e = nil
    for i = #list, 1, -1 do
      local c = list[i]
      if c.id ~= nil and c.res == nil then
        local okr, r = pcall(NativeBridge.LosResult, c.id)
        if okr and r ~= nil then c.res, c.id, c.t = r, nil, now end
      end
      if (c.res ~= nil and now - c.t > 2.0) or (c.res == nil and c.id == nil) or now - c.t > 6.0 then
        table.remove(list, i)
      elseif e == nil and (c.fx - from.x) ^ 2 + (c.fy - from.y) ^ 2 + (c.fz - from.z) ^ 2 < 9.0 then
        e = c
      end
    end
    EVBotsRcStat = EVBotsRcStat or { clear = 0, blocked = 0, pending = 0 }
    if e == nil then
      if cacheN > 256 then cache, cacheN = {}, 1; list = {}; cache[key] = list end
      local okq, id = pcall(NativeBridge.LosRequest, from.x, from.y, from.z, to.x, to.y, to.z)
      list[#list + 1] = { id = okq and id or nil, t = now, fx = from.x, fy = from.y, fz = from.z }
      EVBotsRcStat.pending = EVBotsRcStat.pending + 1
      return hitList(to)
    end
    if e.res == nil then EVBotsRcStat.pending = EVBotsRcStat.pending + 1; return hitList(to) end
    if e.res == true then EVBotsRcStat.clear = EVBotsRcStat.clear + 1; return {} end
    EVBotsRcStat.blocked = EVBotsRcStat.blocked + 1
    return hitList(to)
  end
  -- Серверный набор VU (server\library\raycastmanager.md); latency (компенсация задержки) не учитываем.
  function RM:CollisionRaycast(from, to, maxHits, materialFlags, rayFlags) return cast(from, to) end
  function RM:DetailedRaycast(from, to, maxHits, materialFlags, rayFlags) return cast(from, to) end
  function RM:CollisionLatencyRaycast(latency, from, to, maxHits, materialFlags, rayFlags) return cast(from, to) end
  function RM:DetailedLatencyRaycast(latency, from, to, maxHits, materialFlags, rayFlags) return cast(from, to) end
  _G.RaycastManager = RM
end

-- ── Прогон проверок (EVBOTS_TESTS=1) ───────────────────────────────────
-- Один заход в игру — много проверок. Шаги идут по времени от деплоя человека
-- (тела посажены). Итог: строки «TEST <имя> PASS|FAIL|SKIP …» и «TESTS-DONE» в EVBots.log.
if Events ~= nil then
  local testsChecked, testsOn = false, false
  -- ★ Сборка 201: прогон без человека (EVBOTS_TESTS_NOHUMAN=1 — старт, как только у ботов есть тела) и по всем режимам MapList
  -- за один заход (EVBOTS_TESTS_CYCLE=1 — после TESTS-DONE мост шлёт RCON mapList.runNextRound, на новом уровне проверки
  -- начинаются заново; в конце — TESTS-ALL-DONE). Скрипт — stand/run/cycle-modes.cmd.
  local testsNoHuman, testsCycle = false, false
  local clock = 0.0
  local t0 = nil
  local victim = nil
  local victim2 = nil
  local deathSeen = false
  local probe = nil
  -- Сборка 121: серверная эмуляция клиентских проверок видимости мода. Лучи: NativeBridge.LosRequest/LosResult.
  -- Bot-bot — перехват CheckBotBotAttack; enemy — люди видят ботов противоположной команды (ClientBotManager).
  local rayJobs = {}
  local rayStats = { req = 0, clear = 0, block = 0, bb = 0, es = 0, deliver = 0, t = 0.0 }
  local function rayEye(p)
    local ptr = p.nativePtr
    if (ptr == nil or ptr == 0) and p.onlineId ~= 0 then ptr = EVBotsHumanPtr(p) end  -- сборка 201: свой у каждого человека
    if ptr == nil or ptr == 0 then return nil end
    local x, y, z = NativeBridge.SoldierPos(ptr)
    if x == nil then return nil end
    return x, y + 1.4, z
  end
  -- ★ Сборка 181: седок техники — как клиент мода (ClientBotManager.lua:233/307/327): глаз = позиция машины + 1,4 м,
  -- луч начинается в 3,2 м от машины к цели, а до цели в машине не доходит 3,2 м (DoRaycast без столкновений с персонажами,
  -- иначе луч упирается в корпус своей/чужой машины). Раньше брали позицию солдата — в машине она не та, лучи «закрыты».
  local rayAsk
  local function vehEye(p)
    local okV, veh = pcall(function() return p.attachedControllable end)
    if not okV or veh == nil then return nil end
    local okT, tr = pcall(function() return veh.transform.trans end)
    if okT and tr ~= nil and tr.x ~= nil then return tr.x, tr.y + 1.4, tr.z end
    return nil
  end
  local function eyeOf(p, inVeh)
    if inVeh ~= false then
      local x, y, z = vehEye(p)
      if x ~= nil then return x, y, z, true end
    end
    local x, y, z = rayEye(p)
    return x, y, z, false
  end
  local function rayAskV(ax, ay, az, aIn, bx, by, bz, bIn, job)
    local dx, dy, dz = bx - ax, by - ay, bz - az
    local d = math.sqrt(dx * dx + dy * dy + dz * dz)
    if d < 0.01 then return false end
    dx, dy, dz = dx / d, dy / d, dz / d
    if aIn then ax, ay, az = ax + dx * 3.2, ay + dy * 3.2, az + dz * 3.2 end
    if bIn then bx, by, bz = bx - dx * 3.2, by - dy * 3.2, bz - dz * 3.2 end
    if (aIn or bIn) and d <= 6.4 then
      rayStats.close = (rayStats.close or 0) + 1
    end
    if aIn or bIn then rayStats.veh = (rayStats.veh or 0) + 1 end
    return rayAsk(ax, ay, az, bx, by, bz, job)
  end
  rayAsk = function(ax, ay, az, bx, by, bz, job)
    local id = NativeBridge.LosRequest(ax, ay, az, bx, by, bz)
    if id == nil then return false end
    job.id = id
    rayJobs[#rayJobs + 1] = job
    rayStats.req = rayStats.req + 1
    return true
  end
  -- ★ Сборка 225: у человека с нашим клиентом (канал К4) лучи считает его клиент мода, как на VU, — серверная эмуляция
  -- для него выключена (bot-bot, «человек видит бота», реанимация). Без клиента — эмуляция как раньше (реестр Б1).
  local realSaid = {}
  -- ★ Сборка 237 (просьба пользователя «перенести то, что делали на сервере, пока клиента не было»): у каждого игрока теперь
  -- наш клиент — как у VU, лучи (bot-bot, «человек видит бота», реанимация) считает только клиент мода. Серверная эмуляция
  -- — только с EVBOTS_SERVER_CLIENT_EMU=1 (гость без клиента EVBots). Без флага: «у игрока свой клиент» всегда.
  -- ★ 238-3: «свой клиент» = от соединения игрока уже пришли события клиента мода (NetEvents._HasModClient), а не просто
  -- канал: у гостя ev-core может подключиться, а клиентский Lua мода — не загрузиться (нет файлов мода на его ПК).
  local function realClient(p)
    if EVBOTS_SERVER_CLIENT_EMU ~= true then return p ~= nil end
    if p == nil or NetEvents._HasModClient == nil or not NetEvents._HasModClient(p) then return false end
    if not realSaid[p.name] then
      realSaid[p.name] = true
      log("INFO", "лучи: у " .. tostring(p.name) .. " свой клиент мода — серверная эмуляция лучей для него выключена")
    end
    return true
  end
  -- ★ 246/247: в парах — id мода (Bot.m_Id). Клиенту — движковые (_EngineId): клиент знает игроков только по ним,
  -- его ответы переводятся назад в NetEvents (deliverFrom, RaycastResults → _LiveId). Серверная эмуляция — id мода как есть.
  local function toEngineIds(list)
    if type(list) ~= "table" then return end
    for i = 1, #list do
      local e = list[i]
      if type(e) == "table" then
        local b1, b2 = PlayerManager._EngineId(e.Bot1), PlayerManager._EngineId(e.Bot2)
        if b1 ~= e.Bot1 or b2 ~= e.Bot2 then
          rayStats.idfix = (rayStats.idfix or 0) + 1
          if rayStats.idfix <= 5 then
            log("INFO ", "мост: CheckBotBotAttack id " .. tostring(e.Bot1) .. "/" .. tostring(e.Bot2)
                .. " -> " .. tostring(b1) .. "/" .. tostring(b2) .. " (движковые для клиента, 247)")
          end
        end
        e.Bot1, e.Bot2 = b1, b2
      end
    end
  end
  NetEvents._Intercept["CheckBotBotAttack"] = function(player, list)
    if EVBotsLevelReady == false then return end
    -- ★ 247 (прогон 246: «боты не стреляют друг в друга»): у клиента нет тел ботов (CreateEngineSoldier → Lua-фолбэк),
    -- его лучи bot-bot все пустые (336/336) → bb=0. Пары bot-bot считает сервер и при живом клиенте мода, как до 237
    -- (вечер 27.10: bb 120–693/окно). «Человек видит бота» и реанимация — по-прежнему клиент. EVBOTS_BB_ON_CLIENT=1 — как VU
    -- (реестр расхождений, Б1-bb). EVBOTS_SERVER_CLIENT_EMU=0 — как раньше, всё клиенту.
    if EVBOTS_SERVER_CLIENT_EMU ~= true then toEngineIds(list); return false end
    if EVBOTS_BB_ON_CLIENT == true and realClient(player) then toEngineIds(list); return false end   -- уходит настоящему клиенту
    if not realSaid["bb:" .. tostring(player and player.name)] then
      realSaid["bb:" .. tostring(player and player.name)] = true
      log("INFO", "лучи: bot-bot для " .. tostring(player and player.name) .. " считает сервер (247: у клиента нет тел ботов)")
    end
    if type(list) ~= "table" then return end
    for i = 1, #list do
      local e = list[i]
      local b1 = PlayerManager:GetPlayerById(e.Bot1)
      local b2 = PlayerManager:GetPlayerById(e.Bot2)
      if b1 and b2 and b1.nativePtr and b2.nativePtr and b1.nativePtr ~= 0 and b2.nativePtr ~= 0 then
        -- Сборка 181: флаги мода Bot1InVehicle/Bot2InVehicle (BotManager.lua:1445) — глаз от машины, как клиент мода
        local ax, ay, az, aIn = eyeOf(b1, e.Bot1InVehicle == true)
        local bx, by, bz, bIn = eyeOf(b2, e.Bot2InVehicle == true)
        if ax ~= nil and bx ~= nil then
          rayAskV(ax, ay, az, aIn, bx, by, bz, bIn, { kind = "bb", player = player, bot1 = e.Bot1, bot2 = e.Bot2 })
        end
      end
    end
  end
  -- ★ Сборка 188: реанимация человека ботом, как клиент мода (ClientBotManager.lua:368–410): человек мёртв, труп можно
  -- поднять (corpse.isDead == false) — по кругу боты его команды (с телом, не в технике), первый ближе
  -- Registry.CLIENT.REVIVE_DISTANCE (30 м) — один луч от трупа (+0,4 м) к глазам бота; чисто — RaycastResults Mode 2
  -- (RaycastResultModes.RevivePlayer) → BotManager:OnRevivePlayer → Bot:Revive. До 188 режима 2 не было: боты-медики не
  -- поднимали человека никогда. Один луч за раз (у клиента — «only one raycast per cycle»).
  local reviveIdx, reviveBusy = 0, false
  Events:Subscribe('Level:Destroy', function()
    -- Результаты старого мира нельзя доставлять на новой карте. Слоты забираем обычным LosResult,
    -- а не бросаем очередь: незабранный нативный запрос иначе продолжал бы занимать слот.
    for _, job in ipairs(rayJobs) do job.cancelled = true end
    reviveIdx, reviveBusy = 0, false
    EVBotsEsSeen = nil
    for _, p in ipairs(players) do
      rawset(p, '_esAlive', nil)
      rawset(p, '_esT', nil)
      rawset(p, '_esIdx', nil)
      rawset(p, '_rvHadBody', nil)
      rawset(p, '_rvDiag', nil)
    end
  end)
  local function reviveTick()
    if reviveBusy then return end
    local reg = rawget(_G, "Registry")
    local maxD = (type(reg) == "table" and type(reg.CLIENT) == "table" and tonumber(reg.CLIENT.REVIVE_DISTANCE)) or 30.0
    for _, h in ipairs(players) do
      if h.onlineId ~= 0 and h.soldier ~= nil then rawset(h, '_rvDiag', nil); rawset(h, '_rvHadBody', true) end
      -- Сборка 194: до первого тела (вход на сервер) — не «смерть» (прогон 193, 01:34:50: ложная строка при входе)
      if h.onlineId ~= 0 and h.soldier == nil and not rawget(h, '_rvHadBody') then rawset(h, '_rvDiag', true) end
      if h.onlineId ~= 0 and h.soldier == nil and not realClient(h) then
        local okC, c = pcall(function() return h.corpse end)
        -- Сборка 193: строка на каждую смерть человека (прогон 192: «реанимация человека 0/0» за весь прогон — не видно почему)
        if not rawget(h, '_rvDiag') then
          rawset(h, '_rvDiag', true)
          local near, medics = 0, 0
          if okC and c ~= nil and c.worldTransform ~= nil then
            local cp = c.worldTransform.trans
            for _, b in ipairs(PlayerManager:GetPlayersByTeam(h.teamId)) do
              if b.onlineId == 0 and b.soldier ~= nil then
                local bx, by, bz = rayEye(b)
                if bx ~= nil and math.sqrt((bx - cp.x) ^ 2 + (bz - cp.z) ^ 2) < maxD then
                  near = near + 1
                  local okW, w6 = pcall(function() return b.soldier.weaponsComponent.weapons[6].name end)
                  if okW and type(w6) == 'string' and w6:find('Defibrillator') then medics = medics + 1 end
                end
              end
            end
          end
          log("INFO ", string.format("ДИАГ человек мёртв: %s; труп %s, isDead=%s; ботов своей команды ближе %.0f м: %d, с дефибриллятором: %d",
              tostring(h.name), (okC and c ~= nil) and "есть" or "НЕТ", tostring(okC and c ~= nil and c.isDead), maxD, near, medics))
        end
        if okC and c ~= nil and not c.isDead and c.worldTransform ~= nil then
          local cp = c.worldTransform.trans
          local tm = PlayerManager:GetPlayersByTeam(h.teamId)
          local n = #tm
          if reviveIdx >= n then reviveIdx = 0 end
          for i = 0, n - 1 do
            local idx = (reviveIdx + i) % n + 1
            local b = tm[idx]
            if b ~= nil and b.onlineId == 0 and b.soldier ~= nil and not b.inVehicle then
              local bx, by, bz = rayEye(b)
              if bx ~= nil then
                local d = math.sqrt((bx - cp.x) ^ 2 + (by - 1.4 - cp.y) ^ 2 + (bz - cp.z) ^ 2)
                if d < maxD then
                  reviveIdx = idx
                  if rayAsk(cp.x, cp.y + 0.4, cp.z, bx, by, bz, { kind = "rv", player = h, bot = b.id }) then
                    reviveBusy = true
                    rayStats.rvReq = (rayStats.rvReq or 0) + 1
                  end
                  return
                end
              end
            end
          end
        end
      end
    end
  end
  function ServerRayTick(dt)
    if NativeBridge.LosRequest == nil then return end
    if EVBotsLevelReady ~= false then pcall(reviveTick) end
    rayStats.t = rayStats.t + dt
    -- ★ Сборка 202: «человек видит бота» — как клиент мода у КАЖДОГО человека (ClientBotManager.lua:201–365): свой таймер
    -- (Registry.GAME_RAYCASTING.RAYCAST_INTERVAL_ENEMY_CHECK, 0,03 с), свой круговой индекс по врагам (m_LastIndex), один луч за
    -- цикл, не больше Registry.CLIENT.MAX_CHECKS_PER_CYCLE (10) проверок дальности за цикл; дальность — как у мода: пехота
    -- max(MaxShootDistanceMissileAir, MaxShootDistanceSniper) = 350 м, техника max(MaxShootDistanceVehicles, Gunship) = 1000 м;
    -- защита после спавна Registry.CLIENT.SPAWN_PROTECTION (1,5 с; мёртв с трупом для реанимации — 0,5 с, иначе 0); человек
    -- в команде 0 (зритель) ботов не видит; Config.BotsAttackPlayers = false — проверок нет. Глаз человека — позиция + высота
    -- по позе (у клиента — камера), цель — позиция бота + getCameraPos (1,6 / 1,0 / 0,3 м). До 202: раз в 0,5 с ОБЩИЕ 16 лучей
    -- на всех людей (первый человек забирал все), дальность 80 м — боты «в упор» не видели второго человека (друг, 19.10).
    local cfg = rawget(_G, "Config")
    local reg = rawget(_G, "Registry")
    local function cnum(t, k, d) return (type(t) == "table" and tonumber(t[k])) or d end
    local regC = type(reg) == "table" and reg.CLIENT or nil
    local regR = type(reg) == "table" and reg.GAME_RAYCASTING or nil
    local interval = cnum(regR, "RAYCAST_INTERVAL_ENEMY_CHECK", 0.03)
    local maxChecks = cnum(regC, "MAX_CHECKS_PER_CYCLE", 10)
    local spawnProt = cnum(regC, "SPAWN_PROTECTION", 1.5)
    local maxVeh = math.max(cnum(cfg, "MaxShootDistanceVehicles", 375.0), cnum(cfg, "MaxShootDistanceGunship", 1000.0))
    local maxPl = math.max(cnum(cfg, "MaxShootDistanceMissileAir", 350.0), cnum(cfg, "MaxShootDistanceSniper", 150.0))
    local direct = cnum(cfg, "DistanceForDirectAttack", 6.0)
    local attackPlayers = not (type(cfg) == "table" and cfg.BotsAttackPlayers == false)
    local function poseH(p)
      local okP, pose = pcall(function() return p.soldier.pose end)
      local cpt = rawget(_G, "CharacterPoseType")
      if okP and pose ~= nil and type(cpt) == "table" then
        if pose == cpt.CharacterPoseType_Prone then return 0.3 end
        if pose == cpt.CharacterPoseType_Crouch then return 1.0 end
      end
      return 1.6
    end
    local function esEye(p)
      if p.inVehicle then
        local x, y, z = vehEye(p)
        if x ~= nil then return x, y, z, true end
      end
      local ptr = p.nativePtr
      if (ptr == nil or ptr == 0) and p.onlineId ~= 0 then ptr = EVBotsHumanPtr(p) end
      if ptr == nil or ptr == 0 then return nil end
      local x, y, z = NativeBridge.SoldierPos(ptr)
      if x == nil then return nil end
      return x, y + poseH(p), z, false
    end
    local nb, nh = 0, 0
    for i = 1, #players do
      local p = players[i]
      if p.soldier ~= nil then
        if p.onlineId == 0 then nb = nb + 1 else nh = nh + 1 end
      end
    end
    rayStats.nb, rayStats.nh = nb, nh
    local pendingEs = {}  -- лучей «человек видит бота» в очереди у каждого человека
    for _, job in ipairs(rayJobs) do
      if job.kind == "es" and job.player ~= nil then pendingEs[job.player] = (pendingEs[job.player] or 0) + 1 end
    end
    local whoS = {}
    for i = 1, #players do
      local h = players[i]
      if EVBotsLevelReady ~= false and h.onlineId ~= 0 and not realClient(h) then
        if h.soldier == nil then
          -- мёртв: у клиента мода m_AliveTimer = 0,5, если труп можно поднять, иначе 0 (ClientBotManager.lua:369/420)
          local okC, c = pcall(function() return h.corpse end)
          rawset(h, '_esAlive', (okC and c ~= nil and not c.isDead) and 0.5 or 0.0)
        elseif attackPlayers and h.teamId ~= 0 then
          local t = (rawget(h, '_esT') or 0.0) + dt
          rawset(h, '_esT', t)
          local alive = rawget(h, '_esAlive') or 0.0
          if t >= interval then
            rawset(h, '_esT', 0.0)
            if alive < spawnProt then
              rawset(h, '_esAlive', alive + dt)
            elseif (pendingEs[h] or 0) < 4 then
              local hx, hy, hz, hIn = esEye(h)
              if hx ~= nil then
                local enemies = {}
                for j = 1, #players do
                  if players[j].teamId ~= h.teamId then enemies[#enemies + 1] = players[j] end
                end
                local ne = #enemies
                local last = rawget(h, '_esIdx') or 0
                if last >= ne then last = 0 end
                local checks = 0
                for k = 0, ne - 1 do
                  local idx = (last + k) % ne + 1
                  local b = enemies[idx]
                  if b ~= nil and b.onlineId == 0 and b.soldier ~= nil and b.nativePtr ~= nil and b.nativePtr ~= 0 then
                    local bx, by, bz, bIn = esEye(b)
                    if bx ~= nil then
                      local d = math.sqrt((bx - hx) ^ 2 + (by - hy) ^ 2 + (bz - hz) ^ 2)
                      checks = checks + 1
                      if d < (bIn and maxVeh or maxPl) then
                        rawset(h, '_esIdx', idx)
                        rayAskV(hx, hy, hz, hIn, bx, by, bz, bIn, { kind = "es", player = h, bot = b.id, near = d < direct })
                        break  -- один луч за цикл, как у клиента мода
                      end
                      if checks >= maxChecks then
                        rawset(h, '_esIdx', idx)
                        break
                      end
                    end
                  end
                end
              end
            end
          end
        end
        if #whoS < 4 then
          whoS[#whoS + 1] = tostring(h.name) .. "/т" .. tostring(h.teamId) .. (h.soldier ~= nil and "" or "/мёртв")
        end
      end
    end
    rayStats.who = (#whoS > 0) and table.concat(whoS, ", ") or "нет"
    local out, keep = {}, {}
    for _, job in ipairs(rayJobs) do
      local r = NativeBridge.LosResult(job.id)
      -- Сборка 153: последний луч «человек видит бота» по каждому боту — для строки «БЛИЗКО»
      if r ~= nil and not job.cancelled and job.kind == "es" then
        EVBotsEsSeen = EVBotsEsSeen or {}
        EVBotsEsSeen[job.bot] = { clear = (r == true), at = EVBotsLifeWall or 0 }
      end
      if r ~= nil and not job.cancelled and job.kind == "rv" then reviveBusy = false end
      if r == nil then
        keep[#keep + 1] = job
      elseif job.cancelled then
        -- Результат забран и слот освобождён, событие относится к уничтоженному уровню.
      elseif r then
        rayStats.clear = rayStats.clear + 1
        local entry = nil
        if job.kind == "rv" then
          rayStats.rv = (rayStats.rv or 0) + 1
          entry = { Mode = 2, Bot1 = job.bot, Bot2 = 0, IgnoreYaw = false }
          log("INFO ", "мост: эмуляция клиента — человек " .. tostring(job.player.name) .. " ждёт реанимации, бот id="
              .. tostring(job.bot) .. " его видит (RevivePlayer)")
        elseif job.kind == "bb" then
          rayStats.bb = rayStats.bb + 1
          entry = { Mode = 0, Bot1 = job.bot1, Bot2 = job.bot2, IgnoreYaw = false }
        elseif job.kind == "es" then
          rayStats.es = rayStats.es + 1
          entry = { Mode = 1, Bot1 = job.bot, Bot2 = 0, IgnoreYaw = job.near == true }
        end
        if entry ~= nil and job.player ~= nil then
          local key = job.player.id
          out[key] = out[key] or { player = job.player, list = {} }
          out[key].list[#out[key].list + 1] = entry
        end
      else
        rayStats.block = rayStats.block + 1
      end
    end
    rayJobs = keep
    for _, o in pairs(out) do
      rayStats.deliver = rayStats.deliver + 1
      NetEvents:_Deliver("Botmanager:RaycastResults", o.player, o.list)
    end
    if rayStats.t >= 20.0 then
      log("INFO ", string.format("ДИАГ луч: запросов %d, чисто %d, закрыто %d, bot-bot %d, enemy %d, реанимация человека %d/%d, доставок %d, ждут %d; ботов %s, людей %s (%s)",
          rayStats.req, rayStats.clear, rayStats.block, rayStats.bb, rayStats.es, rayStats.rv or 0, rayStats.rvReq or 0,
          rayStats.deliver, #rayJobs, tostring(rayStats.nb), tostring(rayStats.nh), tostring(rayStats.who)))
      rayStats = { req = 0, clear = 0, block = 0, bb = 0, es = 0, deliver = 0, t = 0.0 }
    end
  end
  local vehTest = nil  -- сборка 178: бот и машина теста посадки
  local vehDrive = nil  -- сборка 179: машины с водителем-ботом на старте теста езды
  local startSpots = nil  -- сборка 113: позиции всех ботов в начале окна движения
  local keepOwnInput = true  -- сборка 114: шаг move не выключает свой ввод (варианты работают с ним)
  local moveAct = nil     -- сборка 90: действие «вперёд», которое тест держит на пробном боте 62–68 с
  local probeStart = nil  -- позиция пробного бота из движка в начале теста движения
  local walkStart = nil  -- сборка 108: позиции ботов при включении своего ввода всем
  local count = { PASS = 0, FAIL = 0, SKIP = 0 }

  -- Сборка 201: в Air Superiority вся пехота — в самолётах, и проверки пехоты/наземной техники там неприменимы (прогон 15:29,
  -- XP5_002: walk-all, veh-drive, respawn, vu-weapons, kit давали FAIL от самой постановки, а не от ошибки). SKIP с пометкой,
  -- не PASS: остальное (ERR, VEH, Vehicles.lua) по-прежнему видно.
  local AIR_SKIP = { ['walk-all'] = true, ['veh-drive'] = true, ['respawn'] = true, ['vu-weapons'] = true, kit = true }
  local function report(name, verdict, detail)
    if verdict == 'FAIL' and AIR_SKIP[name] then
      local okM, md = pcall(function() return SharedUtils:GetCurrentGameMode() end)
      if okM and type(md) == 'string' and md:match('AirSuperiority') then
        verdict = 'SKIP'
        detail = '(Air Superiority: нет пехоты) ' .. tostring(detail or '')
      end
    end
    count[verdict] = (count[verdict] or 0) + 1
    log(verdict == 'FAIL' and 'WRN ' or 'INFO', 'TEST ' .. name .. ' ' .. verdict .. ' ' .. tostring(detail or ''))
  end

  -- Слабая ссылка бота (+0x4AC) не должна совпадать с человеческой (иначе смерть бота = смерть человека).
  -- Ненулевой блок у бота и тот же у человека — FAIL; пустой слот (0) — не проверяем.
  -- первый бот команды team с живым солдатом ДВИЖКА (кроме skip)
  local function firstBot(team, skip)
    for i = 1, #players do
      local p = players[i]
      if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 and p.teamId == team
          and p ~= skip and NativeBridge.HasSoldier(p.nativePtr) == 1 then
        return p
      end
    end
    return nil
  end

  -- Смерти ботов после старта теста: человек убивает в любой момент, поэтому каждая смерть
  -- получает свою проверку респауна (checkRespawns), а не одно окно по времени.
  local humanKills = {}
  Events:Subscribe('Player:Killed', function(p)
    if t0 ~= nil and p ~= nil and p.onlineId == 0 then
      -- Сборка 178: новая смерть того же бота — значит, после прошлой он возродился (прогон 177: 3 ложных FAIL — бот
      -- погиб через 3 с после возрождения, проверка «через 30 с» застала его между жизнями).
      for i = 1, #humanKills do
        local k = humanKills[i]
        if not k.checked and k.p == p then
          k.checked = true
          report('respawn', 'PASS', tostring(p.name) .. ' возродился и снова погиб через ' .. string.format('%.0f', clock - k.at) .. ' с')
        end
      end
      humanKills[#humanKills + 1] = { p = p, at = clock, checked = false,
        bodies = (type(p.life) == 'table' and p.life.bodies) or 0 }  -- сборка 185: тел на момент смерти
      report('death', 'PASS', tostring(p.name) .. ' смерть на ' .. string.format('%.0f', clock - t0) .. ' с теста (мод или игрок — тест не различает)')
    end
  end)

  -- Через 60 с после смерти бот должен иметь солдата движка и кит (+0x480). atEnd — конец прогона.
  -- Сборка 184: было 30 с — мод возрождает и через 37 с (прогон 183: ложный FAIL), медиана по логу 14 с.
  local function checkRespawns(atEnd)
    -- Сборка 175: на экране конца раунда возрождений нет ни у кого (прогон 174: 20 ложных FAIL) — отсчёт 30 с заново
    -- с нового раунда.
    local roundOver = EVBotsRound ~= nil and EVBotsRound.over
    for i = 1, #humanKills do
      local k = humanKills[i]
      if roundOver and not k.checked then k.at = clock end
      if not k.checked and not roundOver and (atEnd or clock - k.at >= 60) then
        k.checked = true
        if clock - k.at < 60 then
          report('respawn', 'SKIP', tostring(k.p.name) .. ': конец прогона раньше 60 с после смерти')
        elseif k.p.nativePtr == nil or k.p.nativePtr == 0 then
          -- бот уже удалён из движка (баланс/мод): возрождения проверять не на чем (сборка 91)
          report('respawn', 'SKIP', tostring(k.p.name) .. ': удалён из движка, возрождения нет')
        else
          local has = NativeBridge.HasSoldier(k.p.nativePtr)
          local kit = NativeBridge.ReadU32(k.p.nativePtr, 0x480)
          -- Сборка 185: тело после смерти было (счётчик тел вырос), а в миг проверки бот снова погиб — это возрождение
          -- (ночной прогон 09:28: BOT_Fuseered2 возродился через 18,6 с и погиб ровно на 60-й с — ложный FAIL).
          local bodiesNow = (type(k.p.life) == 'table' and k.p.life.bodies) or 0
          local reborn = bodiesNow > (k.bodies or 0)
          report('respawn', ((has == 1 and k.p.soldier ~= nil) or reborn) and 'PASS' or 'FAIL',
              tostring(k.p.name) .. ' через ' .. string.format('%.0f', clock - k.at) .. ' с: солдат=' .. tostring(has)
              .. (reborn and has ~= 1 and ' (тело было, снова погиб)' or ''))
          report('kit', (kit ~= nil and kit ~= 0) and 'PASS' or 'FAIL',
              tostring(k.p.name) .. ' +0x480=' .. tostring(kit))
        end
      end
    end
  end

  -- Сборка 91: диагностика для проверок (только чтение, в игру не вмешивается)
  local function hex(v)
    if v == nil then return 'нет' end
    return string.format('%08X', v)
  end
  local function diagPlayer(tag, ptr)
    if ptr == nil or ptr == 0 then
      log('INFO ', 'ДИАГ ' .. tag .. ': указателя нет')
      return
    end
    local soldier = NativeBridge.ReadU32(ptr, 0x3C4) or 0
    local inputOfSoldier = nil
    if soldier ~= 0 then inputOfSoldier = NativeBridge.ReadU32(soldier, 0x3C0) end
    log('INFO ', 'ДИАГ ' .. tag .. ': игрок=' .. hex(ptr)
        .. ' +0x318=' .. hex(NativeBridge.ReadU32(ptr, 0x318))
        .. ' +0x31C=' .. hex(NativeBridge.ReadU32(ptr, 0x31C))
        .. ' +0x90=' .. hex(NativeBridge.ReadU32(ptr, 0x90))
        .. ' +0x3C0(игрок)=' .. hex(NativeBridge.ReadU32(ptr, 0x3C0))
        .. ' солдат=' .. hex(soldier)
        .. ' +0x3C0(солдат)=' .. hex(inputOfSoldier))
    -- Сборка 93: иконка (готовность по 0xE191A0: +0x3C8 ~= 0 и == +0x3E8), имя кита (+0x90 → +0x08),
    -- имя блюпринта (+0x480 → +0x08), флаги солдата (+0x38B и +0x594 — байты, +0x74 — состояние, +0x160 — контроллер)
    local rdy1 = NativeBridge.ReadU32(ptr, 0x3C8) or 0
    local rdy2 = NativeBridge.ReadU32(ptr, 0x3E8) or 0
    local kitObj = NativeBridge.ReadU32(ptr, 0x90) or 0
    local bpObj = NativeBridge.ReadU32(ptr, 0x480) or 0
    local kitName = nil
    if kitObj ~= 0 then kitName = NativeBridge.ReadCStr(kitObj, 0x08) end
    local bpName = nil
    if bpObj ~= 0 then bpName = NativeBridge.ReadCStr(bpObj, 0x08) end
    local soldierFlags = 'нет солдата'
    if soldier ~= 0 then
      soldierFlags = '+0x38B=' .. tostring((NativeBridge.ReadU32(soldier, 0x38B) or 0) & 0xFF)
        .. ' +0x594=' .. tostring((NativeBridge.ReadU32(soldier, 0x594) or 0) & 0xFF)
        .. ' +0x74=' .. hex(NativeBridge.ReadU32(soldier, 0x74))
        .. ' +0x160=' .. hex(NativeBridge.ReadU32(soldier, 0x160))
    end
    log('INFO ', 'ДИАГ ' .. tag .. ': иконка готова=' .. tostring(rdy1 ~= 0 and rdy1 == rdy2)
        .. ' +0x3C8=' .. hex(rdy1) .. ' +0x3E8=' .. hex(rdy2)
        .. ' кит=' .. tostring(kitName) .. ' блюпринт=' .. tostring(bpName)
        .. ' ' .. soldierFlags)
  end
  local function diagSnapshot()
    diagPlayer('человек', NativeBridge.RealPlayer())
    local b1 = firstBot(1, nil)
    if b1 ~= nil then diagPlayer('бот команды 1 ' .. tostring(b1.name), b1.nativePtr) end
    local b2 = firstBot(2, nil)
    if b2 ~= nil then diagPlayer('бот команды 2 ' .. tostring(b2.name), b2.nativePtr) end
  end
  local function diagState()
    local luaBots, engineBots = 0, 0
    for i = 1, #players do
      local p = players[i]
      if p.onlineId == 0 then
        luaBots = luaBots + 1
        if p.nativePtr ~= nil and p.nativePtr ~= 0 then engineBots = engineBots + 1 end
      end
    end
    -- Сборка 93: экземпляр мода (BotManager) из моста не виден, поэтому счёт мода не читаем (была ошибка «_Bots»)
    local modPart = 'экземпляр мода недоступен из моста'
    log('INFO ', 'ДИАГ состояние: ботов в Lua=' .. luaBots .. ', с указателем в движке=' .. engineBots
        .. '; мод: ' .. modPart)
  end


  -- Сборка 110: позиции всех ботов из движка (горизонталь) — для поиска наложений и свободного места
  local function botSpots()
    local out = {}
    for i = 1, #players do
      local p = players[i]
      if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 then
        local x, _, z = NativeBridge.SoldierPos(p.nativePtr)
        if x ~= nil then out[#out + 1] = { name = p.name, ptr = p.nativePtr, x = x, z = z } end
      end
    end
    return out
  end
  local steps = {
    -- Сборка 136: тесты input/mstart/move убраны — они вмешивались в игру (переставляли пробного бота, жали ему газ,
    -- включали/выключали свой ввод). Ходьба давно работает; ниже только пассивные проверки и сверка «как в VU».
    { at = 62, name = 'walk-start', run = function()
        walkStart = {}
        for i = 1, #players do
          local p = players[i]
          if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 then
            local bx, _, bz = NativeBridge.SoldierPos(p.nativePtr)
            if bx ~= nil then walkStart[p.nativePtr] = { bx, bz, p.name } end
          end
        end
      end },
    { at = 66, name = 'vu-soldier', run = function()
        -- Как в VU: hasSoldier/alive есть тогда и только тогда, когда у игрока есть тело в движке ([+0x3C4] != 0).
        local ok, bad, total = 0, {}, 0
        for i = 1, #players do
          local p = players[i]
          if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 then
            total = total + 1
            local body = (NativeBridge.ReadU32(p.nativePtr, 0x3C4) or 0) ~= 0
            if body == (p.hasSoldier == true) and body == (p.alive == true) then ok = ok + 1
            else bad[#bad + 1] = tostring(p.name) .. (body and ':тело без hasSoldier' or ':hasSoldier без тела') end
          end
        end
        report('vu-soldier', (total > 0 and #bad == 0) and 'PASS' or 'FAIL',
            'совпало ' .. ok .. ' из ' .. total .. (#bad > 0 and ('; ' .. table.concat(bad, ', ')) or ''))
      end },
    { at = 70, name = 'vu-weapons', run = function()
        -- Как в VU: у бота с солдатом мод видит текущий слот и патроны основного оружия (weaponsComponent).
        local ok, total, sample, bad = 0, 0, nil, nil
        for i = 1, #players do
          local p = players[i]
          if p.onlineId == 0 and p.soldier ~= nil then
            total = total + 1
            local wc = p.soldier.weaponsComponent
            local slot = wc and wc.currentWeaponSlot
            local w1 = wc and wc.weapons[1]
            -- Сборка 211: имя бота без оружия — понять, только что ли он получил тело (гонка) или оружия нет совсем.
            if (slot == nil or w1 == nil) and bad == nil then
              bad = tostring(p.name) .. ' wc=' .. tostring(wc ~= nil) .. ' слот=' .. tostring(slot) .. ' w1=' .. tostring(w1 ~= nil)
            end
            if slot ~= nil and w1 ~= nil then
              ok = ok + 1
              if sample == nil then
                local have = {}
                for i = 1, 10 do if wc.weapons[i] ~= nil then have[#have + 1] = tostring(i) end end
                sample = tostring(p.name) .. ' слот=' .. tostring(slot) .. ' маг=' .. tostring(w1.primaryAmmo)
                    .. ' запас=' .. tostring(w1.secondaryAmmo) .. ' ' .. tostring(w1.name)
                    .. ' weapons[]=' .. table.concat(have, ',')
              end
            end
          end
        end
        report('vu-weapons', (total > 0 and ok == total) and 'PASS' or 'FAIL',
            'видно оружие у ' .. ok .. ' из ' .. total .. (sample and ('; пример ' .. sample) or '')
            .. (bad and ('; без оружия ' .. bad) or ''))
      end },
    -- Сборка 95: пачка проверок за один вход в игру (человек не меняет команду — поздний бот создаётся тестом).
    { at = 72, name = 'count', run = function()
        -- Сборка 110: пары ботов, стоящих ближе 2 м друг к другу (стакание от спавна мода)
        local spots = botSpots()
        local pairsFound = {}
        for i = 1, #spots do
          for j = i + 1, #spots do
            local a, b = spots[i], spots[j]
            local d = math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2)
            if d < 2.0 then pairsFound[#pairsFound + 1] = string.format('%s-%s %.2f м', a.name, b.name, d) end
          end
        end
        log('INFO ', 'ДИАГ наложение (ботов ближе 2 м): ' .. (#pairsFound > 0 and table.concat(pairsFound, '; ') or 'нет'))
        -- счёт ботов в движке: должно быть ровно 15 (служебный Host в движок не попадает)
        local bots, withSoldier, host = 0, 0, 'нет'
        for i = 1, #players do
          local p = players[i]
          do
            if p.onlineId ~= 0 then
              host = (p.nativePtr ~= nil and p.nativePtr ~= 0) and 'ЕСТЬ в движке' or 'только в Lua'
            elseif p.nativePtr ~= nil and p.nativePtr ~= 0 then
              bots = bots + 1
              if NativeBridge.HasSoldier(p.nativePtr) == 1 then withSoldier = withSoldier + 1 end
            end
          end
        end
        -- Сборка 185: «нужно» — сколько ботов ведёт сам мод (balanced_teams в TDM — InitNumberOfBots на команду, 29–30),
        -- а не постоянные 15. Нет списка мода — по-старому 15.
        local need = 15
        local gbm = rawget(_G, 'g_BotManager')
        if type(gbm) == 'table' and type(rawget(gbm, '_Bots')) == 'table' then need = #gbm._Bots end
        report('count', bots == need and 'PASS' or 'FAIL',
            'ботов в движке=' .. bots .. ' (у мода ' .. need .. '), с солдатом=' .. withSoldier .. ', человек: ' .. host)
      end },
    -- Сборка 178: техника — итератор, данные, посадка и выход бота (Player:EnterVehicle/ExitVehicle через мост).
    { at = 74, name = 'veh-list', run = function()
        local it = EntityManager:GetIterator('ServerVehicleEntity')
        local n, withType, sample = 0, 0, nil
        local e = it:Next()
        while e ~= nil do
          n = n + 1
          local okT, vt = pcall(function() return VehicleEntityData(e.data).controllableType end)
          if okT and type(vt) == 'string' and vt ~= '' then
            withType = withType + 1
            if sample == nil then
              local tr = e.transform.trans
              sample = string.format('%s мест=%s (%.0f, %.0f, %.0f) vel=%.2f', vt, tostring(e.entryCount), tr.x, tr.y, tr.z,
                  e.velocity.magnitude)
            end
          end
          e = it:Next()
        end
        if n == 0 then report('veh-list', 'SKIP', 'техники в мире нет')
        else report('veh-list', withType == n and 'PASS' or 'FAIL', 'машин=' .. n .. ', с controllableType=' .. withType
            .. (sample and ('; пример ' .. sample) or '')) end
      end },
    { at = 78, name = 'veh-enter', run = function()
        vehTest = nil
        local it = EntityManager:GetIterator('ServerVehicleEntity')
        local veh = it:Next()
        while veh ~= nil and veh:GetPlayerInEntry(0) ~= nil do veh = it:Next() end
        -- Сборка 180: бот пешком (прогон 179: тест пересадил водителя мода BOT_Santiago из его машины)
        local bot = nil
        for i = 1, #players do
          local p = players[i]
          if p.onlineId == 0 and p.nativePtr ~= nil and p.nativePtr ~= 0 and NativeBridge.HasSoldier(p.nativePtr) == 1 then
            local okv, att = pcall(NativeBridge.PlayerVehicle, p.nativePtr)
            if okv and (att == nil or att == 0) then bot = p break end
          end
        end
        if veh == nil or bot == nil then
          report('veh-enter', 'SKIP', 'нет свободной машины (' .. tostring(veh ~= nil) .. ') или бота с солдатом (' .. tostring(bot ~= nil) .. ')')
          return
        end
        vehTest = { veh = veh, bot = bot }
        bot:EnterVehicle(veh, 0)
      end },
    -- Сборка 201: проверка через 2 с (было 5): в прогоне 200b бот сел за 0,5 с и сам вышел через 3,6 с (мод забирает ботов) — FAIL был ложным.
    { at = 80, name = 'veh-enter-check', run = function()
        if vehTest == nil then report('veh-enter-check', 'SKIP', 'посадка не запрошена') return end
        local bot, veh = vehTest.bot, vehTest.veh
        local inSeat = veh:GetPlayerInEntry(0)
        local ctl = bot.controlledControllable
        report('veh-enter-check', (inSeat == bot and ctl == veh and bot.controlledEntryId == 0) and 'PASS' or 'FAIL',
            tostring(bot.name) .. ': место0=' .. tostring(inSeat and inSeat.name) .. ' controlled=' .. tostring(ctl ~= nil)
            .. ' entryId=' .. tostring(bot.controlledEntryId))
        bot:ExitVehicle(true, false)
      end },
    { at = 88, name = 'veh-exit-check', run = function()
        if vehTest == nil then report('veh-exit-check', 'SKIP', 'посадки не было') return end
        local bot, veh = vehTest.bot, vehTest.veh
        local ctl = bot.controlledControllable
        -- Сборка 185: бот вышел — место 0 уже не его (туда мог сразу сесть другой: прогон 09:36, BOT_Paul) и он не управляет
        -- машиной (может быть уже убит — controlled=nil).
        local seat0 = veh:GetPlayerInEntry(0)
        report('veh-exit-check', (seat0 ~= bot and ctl ~= veh) and 'PASS' or 'FAIL',
            tostring(bot.name) .. ': место0=' .. tostring(veh:GetPlayerInEntry(0) and veh:GetPlayerInEntry(0).name)
            .. ' controlled=' .. (ctl == nil and 'nil' or (ctl:Is('ServerSoldierEntity') and 'солдат' or 'машина')))
        vehTest = nil
      end },
    -- Сборка 179: езда ботов мода — сколько проехали машины с водителем-ботом за 30 с и сходится ли курс (ошибка < 0,3 рад).
    { at = 95, name = 'veh-drive-start', run = function()
        vehDrive = {}
        EVBotsVehDrive = { n = 0, good = 0 }
        local it = EntityManager:GetIterator('ServerVehicleEntity')
        local e = it:Next()
        while e ~= nil do
          local d = e:GetPlayerInEntry(0)
          if d ~= nil and d.onlineId == 0 then
            local tr = e.transform.trans
            vehDrive[#vehDrive + 1] = { veh = e, name = d.name, x = tr.x, z = tr.z }
          end
          e = it:Next()
        end
      end },
    { at = 125, name = 'veh-drive', run = function()
        if vehDrive == nil or #vehDrive == 0 then report('veh-drive', 'SKIP', 'в технике нет водителей-ботов') return end
        local moved, far, parts = 0, 0.0, {}
        for _, r in ipairs(vehDrive) do
          local tr = r.veh.transform.trans
          local d = math.sqrt((tr.x - r.x) ^ 2 + (tr.z - r.z) ^ 2)
          if d > 25.0 then moved = moved + 1 end
          if d > far then far = d end
          parts[#parts + 1] = string.format('%s %.0f м', tostring(r.name), d)
        end
        local hd = EVBotsVehDrive or { n = 0, good = 0 }
        local share = hd.n > 0 and (100 * hd.good / hd.n) or 0
        report('veh-drive', (moved >= 1 and share >= 50) and 'PASS' or 'FAIL', string.format(
            'водителей %d, проехали >25 м: %d, макс %.0f м; курс сошёлся (<0,3 рад) в %.0f%% из %d замеров; телепортов-заглушек %d; урон технике %d (со стрелявшим %d); %s',
            #vehDrive, moved, far, share, hd.n, EVBotsVehTeleportStub or 0, EVBotsVehDamage or 0, EVBotsVehDamageGiver or 0,
            table.concat(parts, ', ')))
      end },
    -- Сборка 99: проверки, которых ещё не было (пройденные tickets/weakref/weapons/squad/stats/внешность убраны).
    { at = 92, name = 'walk-all', run = function()
        -- Сборка 108: после включения своего ввода всем ботам — сколько из них сдвинулось за ≈24 с (мод ведёт сам)
        if walkStart == nil then
          report('walk-all', 'SKIP', 'пробный бот не пошёл — всем не включали')
          return
        end
        local moved, still, gone, far = 0, 0, 0, 0
        for ptr, st in pairs(walkStart) do
          local x, _, z = NativeBridge.SoldierPos(ptr)
          if x == nil then
            gone = gone + 1
          else
            local d = math.sqrt((x - st[1]) ^ 2 + (z - st[2]) ^ 2)
            if d > 3.0 then moved = moved + 1 else still = still + 1 end
            if d > far then far = d end
          end
        end
        report('walk-all', (moved >= 5) and 'PASS' or 'FAIL', 'сдвинулись=' .. moved .. ' стоят=' .. still
            .. ' без солдата=' .. gone .. ' макс=' .. string.format('%.1f', far) .. ' м')
      end },
    { at = 100, name = 'diag', run = function() diagState() end },
    { at = 130, name = 'done', run = function()
        checkRespawns(true)
        if #humanKills == 0 then report('death', 'SKIP', 'смертей ботов за прогон не было') end
        -- Сборка 136: как в VU боты не убивают себя «за неверный путь» до/на старте раунда (GameDirector.lua:217)
        local gd = (EVBotsModKills or {})['GameDirector.lua:217'] or 0
        local parts = {}
        for k, v in pairs(EVBotsModKills or {}) do parts[#parts + 1] = k .. '=' .. v end
        -- Сборка 185: мод сам убивает бота, долго стоящего на неактивном пути (GameDirector.lua:217) — как в VU (MP_007: 11 за
        -- прогон). Ошибка 136 была массовой (все боты до старта раунда): FAIL — больше половины числа ботов.
        local nb = 0
        local gbm2 = rawget(_G, 'g_BotManager')
        if type(gbm2) == 'table' and type(rawget(gbm2, '_Bots')) == 'table' then nb = #gbm2._Bots end
        report('vu-suicide', (gd == 0 or (nb > 0 and gd <= nb / 2)) and 'PASS' or 'FAIL',
            'GameDirector.lua:217 (неверный путь) = ' .. gd .. '; все убийства модом: ' .. (#parts > 0 and table.concat(parts, ', ') or 'нет'))
        -- Сборка 185: карта и режим для мода — из движка (как VU); реанимации медиками
        local lv, md = SharedUtils:GetLevelName(), SharedUtils:GetCurrentGameMode()
        do  -- сборка 185: авиация (SKIP, если на карте её нет)
          pcall(EVBotsAirDiag)
          local A = EVBotsAir or { cnt = { heli = 0, jet = 0 }, flew = { heli = 0, jet = 0 }, maxUp = { heli = 0, jet = 0 } }
          local c = A.cnt or { heli = 0, jet = 0 }
          if c.heli + c.jet == 0 then
            report('air', 'SKIP', 'вертолётов и самолётов на карте нет')
          else
            report('air', (A.flew.heli + A.flew.jet > 0) and 'PASS' or 'FAIL', string.format(
              'вертолётов %d, самолётов %d; взлетало с ботом-пилотом: верт %d, сам %d; макс высота верт %.0f, сам %.0f м',
              c.heli, c.jet, A.flew.heli, A.flew.jet, A.maxUp.heli, A.maxUp.jet))
          end
        end
        report('level', (lv ~= nil and md ~= nil) and 'PASS' or 'FAIL',
            'GetLevelName=' .. tostring(lv) .. ' GetCurrentGameMode=' .. tostring(md))
        report('revive', (EVBotsRevives or 0) > 0 and 'PASS' or 'SKIP',
            string.format('реанимаций %d; трупов видно %d (isDead=false %d)', EVBotsRevives or 0,
              EVBotsCorpseSeen or 0, EVBotsCorpseLive or 0))
        log('INFO ', 'TESTS-DONE PASS=' .. count.PASS .. ' FAIL=' .. count.FAIL .. ' SKIP=' .. count.SKIP)
        if EVBotsTestsNextLevel ~= nil then pcall(EVBotsTestsNextLevel, lv, md) end
      end },
  }

  -- Сборка 111: состояние входа человека и пробного бота каждые 3 с (от 66 до 120 с после старта теста)
  local function stateDump()
    local function fmt(v) if v == nil then return "-" end return string.format("%X", v) end
    local hp = NativeBridge.RealPlayer()
    local hs = "нет человека"
    if hp ~= nil and hp ~= 0 then
      local x, _, z = NativeBridge.SoldierPos(hp)
      local inp = NativeBridge.ReadU32(hp, 0x3C0)
      local fl = (inp ~= nil and inp ~= 0) and NativeBridge.ReadU32(inp, 0x2F8) or nil
      hs = string.format("человек (%s, %s) вход 2F8=%s газ=%s", tostring(x), tostring(z), fmt(fl),
          tostring(NativeBridge.InputLevelOf(hp, 0)))
    end
    local ps = "нет пробы"
    if probe ~= nil and probe.nativePtr ~= nil and probe.nativePtr ~= 0 then
      local x, _, z = NativeBridge.SoldierPos(probe.nativePtr)
      local inp = NativeBridge.ReadU32(probe.nativePtr, 0x3C0)
      local fl = (inp ~= nil and inp ~= 0) and NativeBridge.ReadU32(inp, 0x2F8) or nil
      ps = string.format("проба (%s, %s) вход 2F8=%s газ=%s", tostring(x), tostring(z), fmt(fl),
          tostring(NativeBridge.InputLevelOf(probe.nativePtr, 0)))
    end
    log("INFO ", string.format("ДИАГ вход t=%d: %s || %s", math.floor(clock - t0), hs, ps))
  end
  for at = 66, 120, 3 do
    steps[#steps + 1] = { at = at, name = "state", run = stateDump }
  end
  -- Сборка 113: двигаются ли боты вообще за окно теста (65–110 с), независимо от пробного бота
  steps[#steps + 1] = { at = 65, name = "bstart", run = function() startSpots = botSpots() end }
  steps[#steps + 1] = { at = 110, name = "bmove", run = function()
    if startSpots == nil then return end
    local moved, still = 0, 0
    local now = botSpots()
    for _, s0 in ipairs(startSpots) do
      local found = nil
      for _, s1 in ipairs(now) do if s1.ptr == s0.ptr then found = s1 end end
      if found ~= nil then
        local d = math.sqrt((found.x - s0.x) ^ 2 + (found.z - s0.z) ^ 2)
        if d > 1.0 then moved = moved + 1 else still = still + 1 end
      end
    end
    log("INFO ", string.format("ДИАГ движение ботов за окно 65–110 с: сдвинулось >1 м: %d, стоят: %d", moved, still))
  end }
  -- Сборка 115: состояние мода — Globals и каждый бот (состояние, режим движения, путь, защита, цель, ввод)
  local function modDump(tag)
    local function g(f) local ok, v = pcall(f); if ok then return tostring(v) end return "err" end
    log("INFO ", "ДИАГ мод " .. tag .. ": IsInputAllowed=" .. g(function() return Globals.IsInputAllowed end)
        .. " IsInputRestrictionDisabled=" .. g(function() return Globals.IsInputRestrictionDisabled end)
        .. " GameMode=" .. g(function() return Globals.GameMode end)
        .. " Level=" .. g(function() return Globals.LevelName end)
        .. " SetLevel ненулевых=" .. setLevelNonZero .. " нулевых=" .. setLevelZero)
    local acts = {}
    for k, v in pairs(setLevelByAction) do acts[#acts + 1] = tostring(k) .. ":" .. tostring(v) end
    log("INFO ", "ДИАГ мод " .. tag .. ": SetLevel по действиям " .. table.concat(acts, " "))
    local ok, bots = pcall(function() return g_BotManager:GetBots() end)
    if not ok or type(bots) ~= "table" then
      log("INFO ", "ДИАГ мод " .. tag .. ": GetBots недоступен (" .. tostring(bots) .. ")")
      return
    end
    local shown = 0
    for _, b in pairs(bots) do
      if shown >= 16 then break end
      shown = shown + 1
      local sname = "?"
      pcall(function()
        for k, v in pairs(g_BotStates.States) do if v == b.m_ActiveState then sname = tostring(k) end end
      end)
      log("INFO ", "ДИАГ мод " .. tag .. " бот " .. g(function() return b.m_Player.name end)
          .. ": состояние=" .. sname
          .. " движение=" .. g(function() return b.m_ActiveMoveMode end)
          .. " путь=" .. g(function() return b._PathIndex end)
          .. " точка=" .. g(function() return b._CurrentWayPoint end)
          .. " защита=" .. g(function() return b._SpawnProtectionTimer end)
          .. " цель=" .. g(function() return b._TargetPlayer and b._TargetPlayer.name or nil end)
          .. " солдат=" .. g(function() return b.m_Player.soldier ~= nil end)
          .. " вход0=" .. g(function() return b.m_ActiveInputs[0] and b.m_ActiveInputs[0].value end)
          .. " ввод=" .. g(function() return b.m_Player.input ~= nil end))
    end
  end
  steps[#steps + 1] = { at = 40, name = "mod40", run = function() modDump("t40") end }
  steps[#steps + 1] = { at = 100, name = "mod100", run = function() modDump("t100") end }
  -- Сборка 120: курс ботов против yaw мода и точки пути
  local function headDump(tag)
    local ok, bots = pcall(function() return g_BotManager:GetBots() end)
    if not ok or type(bots) ~= "table" then return end
    local function norm(a) while a > math.pi do a = a - 2 * math.pi end while a < -math.pi do a = a + 2 * math.pi end return a end
    local sum, n = 0, 0
    for _, b in pairs(bots) do
      pcall(function()
        local sol = b.m_Player.soldier
        local v = sol.bridgeVelocity
        local pos = sol.worldTransform.trans
        local tp = b._TargetPoint and b._TargetPoint.Position
        local yaw = b.m_Player.input.authoritativeAimingYaw
        if v ~= nil and tp ~= nil and yaw ~= nil then
          local speed = math.sqrt(v.x * v.x + v.z * v.z)
          local mh = math.atan(v.z, v.x)
          local th = math.atan(tp.z - pos.z, tp.x - pos.x)
          local dMove = norm(mh - (yaw + math.pi / 2))
          local dTarget = norm(mh - th)
          if speed > 0.5 then sum = sum + math.abs(dMove); n = n + 1 end
          log("INFO ", string.format("КУРС %s %s: скорость %.1f, yaw %.2f, курс %.2f, на точку %.2f, курс-(yaw+pi/2) %.2f, курс-точка %.2f",
              tag, tostring(b.m_Player.name), speed, yaw, mh, th, dMove, dTarget))
        end
      end)
    end
    if n > 0 then log("INFO ", string.format("КУРС %s итог: средн |курс-(yaw+pi/2)| = %.2f рад по %d ботам", tag, sum / n, n)) end
  end
  steps[#steps + 1] = { at = 75, name = "head75", run = function() headDump("t75") end }
  steps[#steps + 1] = { at = 95, name = "head95", run = function() headDump("t95") end }
  -- Сборка 118: окна гипотез прицела; сдвиг всех ботов за окно
  local function cpeDump(tag)
    local hp = NativeBridge.RealPlayer()
    if hp ~= nil and hp ~= 0 then log("INFO ", "ДИАГ CPE " .. tag .. " человек: " .. tostring(NativeBridge.CpeState(hp))) end
    local b = probe or firstBot(2, nil)
    if b ~= nil and b.nativePtr ~= nil and b.nativePtr ~= 0 then
      log("INFO ", "ДИАГ CPE " .. tag .. " бот " .. tostring(b.name) .. ": " .. tostring(NativeBridge.CpeState(b.nativePtr)))
    end
  end
  steps[#steps + 1] = { at = 30, name = "cpe30", run = function() cpeDump("t30") end }
  steps[#steps + 1] = { at = 90, name = "cpe90", run = function() cpeDump("t90") end }
  -- Сборка 116: тест гравитации пробного бота
  local gravBase = {}
  local function gravLift(tag)
    if probe == nil or probe.nativePtr == nil or probe.nativePtr == 0 then return end
    local x, y, z = NativeBridge.SoldierPos(probe.nativePtr)
    if x == nil then return end
    gravBase[tag] = y
    NativeBridge.TeleportBot(probe.nativePtr, x, y + 3.0, z)
    log("INFO ", string.format("ГРАВИТАЦИЯ %s: подняли пробного бота с y=%.2f на %.2f", tag, y, y + 3.0))
  end
  local function gravRead(tag, dt)
    if probe == nil or probe.nativePtr == nil or probe.nativePtr == 0 then return end
    local _, y = NativeBridge.SoldierPos(probe.nativePtr)
    log("INFO ", string.format("ГРАВИТАЦИЯ %s через %.1f с: y=%s (база %s)", tag, dt, tostring(y), tostring(gravBase[tag])))
  end
  steps[#steps + 1] = { at = 66, name = "grav1", run = function() gravLift("ai1") end }
  steps[#steps + 1] = { at = 66.5, name = "grav1a", run = function() gravRead("ai1", 0.5) end }
  steps[#steps + 1] = { at = 68, name = "grav1b", run = function() gravRead("ai1", 2) end }
  steps[#steps + 1] = { at = 72, name = "grav1c", run = function() gravRead("ai1", 6) end }
  steps[#steps + 1] = { at = 76, name = "grav2", run = function() gravLift("ai0") end }
  steps[#steps + 1] = { at = 76.5, name = "grav2a", run = function() gravRead("ai0", 0.5) end }
  steps[#steps + 1] = { at = 78, name = "grav2b", run = function() gravRead("ai0", 2) end }
  steps[#steps + 1] = { at = 82, name = "grav2c", run = function() gravRead("ai0", 6) end }
  table.sort(steps, function(a, b) return a.at < b.at end)

  local nextStep = 1
  Events:Subscribe('Engine:Update', function(deltaTime)
    -- NativeBridge может появиться позже этого чанка: проверяем в колбэке, один раз
    if not testsChecked then
      testsChecked = true
      testsOn = NativeBridge ~= nil and NativeBridge.TestsEnabled ~= nil and NativeBridge.TestsEnabled()
      if NativeBridge ~= nil and NativeBridge.EnvFlag ~= nil then
        testsNoHuman = NativeBridge.EnvFlag('EVBOTS_TESTS_NOHUMAN') == true
        testsCycle = NativeBridge.EnvFlag('EVBOTS_TESTS_CYCLE') == true
      end
    end
    if not testsOn then return end
    if t0 == nil then
      if firstBot(1, nil) == nil and firstBot(2, nil) == nil then return end
      -- старт, когда человек уже в мире (задеплоился): окна для ручных проверок открыты
      local human = NativeBridge.RealPlayer()
      local humanIn = human ~= nil and human ~= 0 and NativeBridge.HasSoldier(human) == 1
      if not humanIn and not testsNoHuman then return end
      t0 = clock
      log('INFO ', humanIn and 'TEST старт: человек в мире, тела посажены, проверки начинаются'
          or 'TEST старт: без человека (EVBOTS_TESTS_NOHUMAN=1), тела ботов посажены, проверки начинаются')
      diagSnapshot()  -- сборка 91: состояние игрока и солдат в движке (объект ввода на игроке и на солдате)
    end
    clock = clock + deltaTime
    -- сборка 90: пока тест движения активен, держим «вперёд» на пробном боте (SetInputLevel пишет в движок)
    if moveAct ~= nil and probe ~= nil and probe.nativePtr ~= nil and probe.nativePtr ~= 0 then
      pcall(NativeBridge.SetInputLevel, probe.nativePtr, moveAct, 1.0)
    end
    -- (Сборка 168: опыты «зеркало человека» 109 и «варианты» 114 удалены — флаг/таблица были пусты.)
    while nextStep <= #steps and clock - t0 >= steps[nextStep].at do
      local s = steps[nextStep]
      nextStep = nextStep + 1
      local ok, err = pcall(s.run)
      if not ok then report(s.name, 'FAIL', 'ошибка Lua: ' .. tostring(err)) end
    end
    checkRespawns(false)
  end)
  -- Сборка 201: новый уровень — проверки заново (каждый режим MapList проверяется своим набором).
  Events:Subscribe('Level:Loaded', function()
    if not testsOn or t0 == nil then return end
    if nextStep <= #steps then log('INFO ', 'TEST уровень сменился до конца проверок — заново на новом уровне') end
    t0, nextStep = nil, 1
    count.PASS, count.FAIL, count.SKIP = 0, 0, 0
    EVBotsModKills = {}  -- vu-suicide — по уровню (раньше копился за весь прогон)
  end)
  local levelsDone = 0
  function EVBotsTestsNextLevel(lv, md)
    if not testsCycle then return end
    levelsDone = levelsDone + 1
    local total = 1
    local okL, resp = pcall(function() return RCON:SendCommand('mapList.list') end)
    if okL and type(resp) == 'table' and resp[1] == 'OK' then total = tonumber(resp[2]) or 1 end
    log('INFO ', string.format('TESTS-LEVEL %d/%d: %s %s', levelsDone, total, tostring(lv), tostring(md)))
    if levelsDone >= total then
      log('INFO ', 'TESTS-ALL-DONE уровней ' .. levelsDone)
      return
    end
    local okN, r2 = pcall(function() return RCON:SendCommand('mapList.runNextRound') end)
    log('INFO ', 'TEST смена уровня: mapList.runNextRound -> ' .. ((okN and type(r2) == 'table') and table.concat(r2, ' ') or tostring(r2)))
  end
end

-- ★ Сборка 163: хук VU «BulletEntity:Collision» (мод: FunBotServer:OnBulletEntityCollision). Зовёт мост из перехвата
-- handleCollision (главный поток движка) с указателем стрелка. Обработчик мода сам открывает окно урона:
-- GetSettings("SyncedGameSettings").allowClientSideDamageArbitration = false → p_HookCtx:Call() → true.
-- Возвращает число обработчиков (0 — хука нет, мост ведёт себя как раньше).
local bulletStats = { calls = 0, fired = 0, at = 0 }
function PlayerManager.__BulletCollision(giverPtr)
  if Hooks == nil or Hooks._FireCall == nil or NativeBridge == nil or NativeBridge.BulletCallOriginal == nil then
    return 0
  end
  local giver = (giverPtr ~= nil and giverPtr ~= 0) and EVBotsPlayerByPtr(giverPtr) or nil  -- сборка 202: любой из людей
  local giverInfo = { giver = giver, assist = nil }
  local n = Hooks._FireCall('BulletEntity:Collision', NativeBridge.BulletCallOriginal, nil, {}, giverInfo)
  bulletStats.calls = bulletStats.calls + 1
  bulletStats.fired = bulletStats.fired + (n or 0)
  local now = os.clock()
  if now - bulletStats.at >= 20 then
    bulletStats.at = now
    log('INFO ', 'ДИАГ хук BulletEntity:Collision: пуль ' .. bulletStats.calls .. ', обработчиков вызвано ' ..
        bulletStats.fired)
  end
  return n
end

return PlayerManager
)LUA";

int l_playermanager_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: PlayerManager: %s", text);
    return 0;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════
// МОСТ Lua -> движок BF3.
//
// Мод (fun-bots, BotManager.lua:868,913) работает ровно через два вызова:
//   PlayerManager:CreatePlayer(name, teamId, squadId)
//   Player:SpawnSoldierAt(soldier, transform, pose)
// Обе функции реализованы выше, но чисто на Lua — они создают таблицу и
// ведут реестр, а движок не трогают. Поэтому бота не видно в игре.
//
// Этот блок добавляет недостающее: нативные функции, которые делают то же
// самое, что уже доказано рабочим в пробнике (SpmProbe.cpp):
//   createPlayer (RVA 0x00A644B0) + addToArray (RVA 0x00A6CA00)
//   и вызов спавна FUN_00ddbbd0 (RVA 0x009DBBD0).
//
// ГРАБЛИ (проверено на живых прогонах, см. ext\SPAWN-RESEARCH.md):
//  1) движок __thiscall => в C++ объявлять __fastcall С ФИКТИВНЫМ
//     параметром, иначе едет стек и RET 0xN рвёт кадр;
//  2) адреса из Ghidra — это VA (ImageBase 0x400000), вычитать
//     перед сложением с базой модуля;
//  3) слот обязан быть уникальным, иначе бот вышибает живого игрока;
//  4) вызовы движка на неизвестных объектах роняют сервер — оборачивать
//     в __try/__except.
// ═══════════════════════════════════════════════════════════════════════

namespace {

constexpr unsigned long kNativeCreatePlayerRva = 0x00A644B0UL;
constexpr unsigned long kNativeAddToArrayRva = 0x00A6CA00UL;
constexpr unsigned long kNativeSpawnRva = 0x009DBBD0UL;

// Смещения в ServerPlayerManager (подтверждены пробником).
constexpr int kOffArr1Begin = 0x80;
constexpr int kOffArr1End = 0x84;
// Поле «есть солдат» у ServerPlayer.
constexpr int kOffPlayerSoldier = 0x3C4;
// Поля игрока, отвечающие за команду (подтверждено разбором FUN_00e59340 —
// шаблона спавна: он читает у игрока +0x318 флаг AI, +0x31C команда).
// Без записи команды игрок создан и посажен, но НЕ числится в составе
// команды: его нет в табе и счётчиках захвата.
constexpr int kOffPlayerIsAi = 0x318;
constexpr int kOffPlayerTeam = 0x31C;
constexpr int kOffPlayerKit = 0x480;
// ★ Blaze-идентификатор игрока (personaId / onlineId). Найден дифференциальным
// дампом 12.10.2026: у настоящего игрока +0x20 = 0x0DF9C16C, у бота 0.
constexpr int kOffPlayerOnlineId = 0x20;

unsigned char* NativeModuleBase() {
    return reinterpret_cast<unsigned char*>(
        ::GetModuleHandleA("bf3_server_Final.exe"));
}

void* NativeManager() {
    // Менеджер берём у Lua-моста: его сообщает хук
    // ServerPlayerManager::update, который стоит постоянно.
    return ::evbots::EVBotsEnginePlayerManager();
}

bool Readable(const void* p) {
    if (p == nullptr) return false;
    __try {
        volatile unsigned char c = *(const unsigned char*)p;
        (void)c;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ★ Номер соединения бота (13.10, разбор 0xE644B0/0xE350B0/0xDDBBD0).
// createPlayer(a1, a2, a3): a1 — байт «номер сетевого соединения», движок пишет
// его в player+0x3BC (getConnectionId 0xD47100, сеттер 0xE141D0); a2 — id игрока
// (-1 = выдать свободный); a3 — зритель. Человеку a1 = номер его соединения (1).
// Свой AI-игрок движка создаётся с a1 = 0 (0xDDBE0E: push 0) — соединения нет.
// По +0x3BC сервер решает «это игрок данного клиента» (ghostWrite 0xE5D180,
// 0xE0AE3D, очки 0x130D600) и ищет соединение игрока (0xD4C7EF). Раньше мы давали
// ботам 2..16, 17..31, а после входа человека — его a1 = 1 (EVBotsEngineCreateRefArgs).
// Гипотеза 13.10: вид игрока без соединения (+0x3BC = 0) клиентам не уходит, поэтому нет ника и класса.
// Сборка 168: опыты EVBOTS_BOT_CONN_SLOT / BOT_CONN_AB удалены (гипотеза отключена, MODLOG 6219) — у бота всегда 0.
long BotConnectionId(unsigned long /*slot*/) { return 0L; }

// ★ Сборка 171: onlineId бота — из диапазона persona человека, младшие биты свои. С 171 по нему идёт запрос статистики
// к бэкенду (VU-создание, см. VuFinishCreate), поэтому совпадение с persona человека (слот 108: 0x100+0x6C == 0x16C)
// недопустимо — такой бот получил бы статистику человека. Номер слота растёт с каждым созданным ботом.
unsigned long BotOnlineId(unsigned long refId, unsigned long slot) {
    if (refId == 0) refId = 0x0DF9C000UL;
    unsigned long id = (refId & 0xFFFFF000UL) | ((0x100UL + slot) & 0xFFFUL);
    if (id == refId) id ^= 0x800UL;
    return id;
}
void VuFinishCreate(unsigned long player, const char* name);

// ★ Сборка 176 (опыт): строка onlineId бота. Базовый Player (ctor 0x008CCCA0) держит три OnlineId {u64 id; char m_id[]}
// по +0x20/+0x40/+0x60; у человека +0x20 = persona, +0x28 = «KOTuk1» (имя persona), у ботов id пишем мы, строка пустая.
// Гипотеза (не проверено): клиент рисует ник «не ИИ» игрока из onlineId.m_id — при isAI 0 у ботов ников нет (123, 174–175).
// EVBOTS_OID_NAME: ab (по умолчанию в 176 — чётные созданные), 1 — всем, 0 — никому. Обрезка до 16 символов: если ник
// появится обрезанным, клиент читает именно это поле.
constexpr int kOffPlayerOnlineIdStr = 0x28;
constexpr unsigned kOnlineIdStrCap = 17;  // m_id[17]; следующий OnlineId — +0x40
static void SetBotOnlineIdName(unsigned char* p, const char* name) {
    static const int mode = [] {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_OID_NAME", v, (DWORD)sizeof(v));
        if (n == 1 && v[0] == '0') return 0;
        if (n == 2 && (v[0] == 'a' || v[0] == 'A')) return 2;
        return 1;  // ★ сборка 177: всем (прогон 176: у группы А ники есть у всех, у Б — не у всех)
    }();
    static LONG s_n = 0;
    const LONG idx = s_n++;
    if (mode == 0 || name == nullptr || (mode == 2 && (idx % 2) != 0)) {
        LOG_INFO("Bridge: \"%s\" строка onlineId не пишется (опыт OID_NAME, группа Б)", name ? name : "?");
        return;
    }
    char buf[kOnlineIdStrCap] = {};
    ::strncpy(buf, name, kOnlineIdStrCap - 1);
    ::memcpy(p + kOffPlayerOnlineIdStr, buf, kOnlineIdStrCap);
    LOG_INFO("Bridge: \"%s\" строка onlineId = «%s» (опыт OID_NAME, группа А)", name, buf);
}

// ★ player+0x480 — указатель на объект СО СЧЁТЧИКОМ ССЫЛОК (счётчик в obj+4).
// Сеттер движка 0xE207F0 (thiscall, 1 стековый, RET 4): release(старый),
// запись, lock xadd [obj+4], 1. Сброс игрока (0xE37061) делает release.
// Раньше мы писали боту указатель человека НАПРЯМУЮ, без addref: 15 ботов
// отпускали ссылку, которой не владели. Теперь — только через сеттер.
constexpr unsigned long kPlayerSet480Rva = 0x00A207F0UL;  // VA 0xE207F0

// obj == 0 — очистка (release старого, запись нуля), нужна перед выгрузкой уровня.
bool SetPlayerRef480(unsigned long player, unsigned long obj) {
    unsigned char* base = reinterpret_cast<unsigned char*>(
        ::GetModuleHandleA("bf3_server_Final.exe"));
    if (base == nullptr || player == 0) return false;
    unsigned long before = 0, after = 0;
    using SetFn = void(__fastcall*)(void*, void*, void*);
    __try {
        if (obj != 0) before = *(const unsigned long*)((const unsigned char*)obj + 4);
        ((SetFn)((ULONGLONG)base + kPlayerSet480Rva))((void*)(ULONGLONG)player, nullptr,
                                                       (void*)(ULONGLONG)obj);
        if (obj != 0) after = *(const unsigned long*)((const unsigned char*)obj + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: сеттер +0x480 (игрок %p, объект %08lX) — исключение", (void*)player, obj);
        return false;
    }
    if (obj == 0) {
        LOG_INFO("Bridge: игрок %p +0x480 очищен через сеттер", (void*)player);
    } else {
        LOG_INFO("Bridge: игрок %p +0x480 <- %08lX через сеттер (ссылок %lu -> %lu)",
                 (void*)player, obj, before, after);
    }
    return true;
}

// Сколько сейчас игроков в менеджере — чтобы выдать боту свободный слот.
unsigned NativePlayerCount(void* mgr) {
    if (!Readable(mgr)) return 0;
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    __try {
        begin = (unsigned char**)*(void**)((unsigned char*)mgr + kOffArr1Begin);
        end = (unsigned char**)*(void**)((unsigned char*)mgr + kOffArr1End);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (begin == nullptr || end == nullptr || end <= begin) return 0;
    return (unsigned)(end - begin);
}

/// NativeBridge.CreateEnginePlayer(name, teamId, squadId) -> ptr или nil
/// Создаёт настоящего ServerPlayer движка и регистрирует его в менеджере.
// ★ Пока нет точки спавна, нативного игрока НЕ создаём.
// Проверено 11.10.2026: 15 игроков движка БЕЗ солдат ломают вход клиента —
// он зависает на загрузке карты (не доходит до выбора дислокации), тогда как
// на сервере без моста вход проходит нормально.
// Поэтому ждём точку (g_spawnSelf) и создаём игрока только тогда.
// Имя и команду запоминаем, чтобы создать позже — повтор делает RetryPending.
struct PendingPlayer {
    char name[32];
    long team;
};
static PendingPlayer g_pendingNative[32];
static volatile LONG g_pendingNativeCount = 0;
// ★ Сборка 99: очередь трогают два потока (Lua дописывает, хук разбирает ~2 с на бота). Раньше хук в конце прохода
// записывал счётчик «оставшихся старых» и стирал то, что Lua добавил за проход (прогон 98: George и Jaxson пропали, 13 из 15).
static SRWLOCK g_pendingLock = SRWLOCK_INIT;
// Игроки, созданные до живого спавна: тела откладываются (см. NativeCreateOne).
static unsigned long g_deferredSpawn[32];
static ULONGLONG g_deferredSince[32];  // сборка 174: когда бот попал в очередь (ожидание выбора мода)
static LONG g_deferredSpawnCount = 0;
// Наблюдение за смертью ботов (диагностика 13.10): указатель игрока и был ли у него
// солдат на прошлом тике. Переход «был -> нет» пишется в лог — это смерть бота.
static unsigned long g_watchBot[64];
static unsigned char g_watchHadSoldier[64];
static LONG g_watchCount = 0;
// ★ Сборка 174: история солдат ботов (для журнала падений: чей солдат — живой или труп). Прогон 173: assert движка
// DataBus «containsListener» у солдата без игрока (труп?) с Glock18 → падение; по логу не понять, чей это солдат.
struct SoldierHist { unsigned long soldier; unsigned long player; ULONGLONG bornAt; ULONGLONG diedAt; };
static SoldierHist g_solHist[128];
static LONG g_solHistNext = 0;  // кольцо
static unsigned long g_deadQueue[64];  // боты, потерявшие солдата (для PollDeadBots)
static LONG g_deadCount = 0;
static ULONGLONG g_bodiesAt = 0;  // когда посажены первые отложенные тела (GetTickCount64)
// ★ 13.10 (прогон 71): спавн мёртвого бота из Lua-колбэка падает внутри движка: TlsGetValue (контекст
// потока движка) возвращает NULL, потом [NULL+0x2C]. Первый спавн проходил из хука тика, где контекст есть.
// Поэтому респауны из Lua ставим в очередь и спавним внутри EVBotsBridgeRetryPending (хук тика).
volatile LONG g_inHook = 0;  // 1 — выполняется код хука тика
static bool OnEngineThreadCtx();  // определение — у NativeSpawnOne (сборка 139)
// ★ Сборка 185: «можно звать движок прямо сейчас» = идёт хук тика И это поток движка. Один g_inHook этого не значит: флаг
// глобальный, а Lua моста работает в своём потоке — вызовы движка из него шли бы параллельно тику (так падал спавн в 139).
// Было: создание позднего бота, kill, перки, кит (SetBotKit) и выбор проверяли только g_inHook.
static bool InEngineTick() { return InterlockedCompareExchange(&g_inHook, 0, 0) != 0 && OnEngineThreadCtx(); }
constexpr LONG kRespawnCapacity = 64;
static volatile LONG g_lastRespawnPoint[5] = {};
unsigned long g_respawnQueue[kRespawnCapacity];
LONG g_respawnCount = 0;
ULONGLONG g_respawnSince[kRespawnCapacity] = {};  // с какого момента спавн ждёт точку
ULONGLONG g_respawnTry[kRespawnCapacity] = {};    // последняя попытка
struct HookScope {
    HookScope() { InterlockedExchange(&g_inHook, 1); }
    ~HookScope() { InterlockedExchange(&g_inHook, 0); }
};
// Боты, созданные из очереди: у их Lua-объектов nativePtr == nil (в момент
// CreatePlayer движок ещё не создал игрока). Отдаём имя -> указатель в Lua.
struct CreatedBot {
    char name[32];
    unsigned long ptr;
};
static CreatedBot g_createdBots[64];
static LONG g_createdCount = 0;
static volatile LONG g_botSlotCounter = 0;  // следующий слот бота (17, 18, ...)

static void RememberPending(const char* name, long team) {
    // «Host» — служебный игрок моста: в движок его не создаём, иначе он попадает в счёт сервера (нужны боты и люди).
    if (::strcmp(name, "Host") == 0) return;
    ::AcquireSRWLockExclusive(&g_pendingLock);
    const LONG i = g_pendingNativeCount;
    if (i >= 0 && i < 32) {
        ::strncpy(g_pendingNative[i].name, name, 31);
        g_pendingNative[i].name[31] = '\0';
        g_pendingNative[i].team = team;
        InterlockedExchange(&g_pendingNativeCount, i + 1);
    }
    ::ReleaseSRWLockExclusive(&g_pendingLock);
    if (i < 0 || i >= 32) {
        LOG_ERR("Bridge: очередь полна — игрок \"%s\" не поставлен", name);
        return;
    }
    LOG_INFO("Bridge: игрок \"%s\" в очереди (точки спавна пока нет)", name);
}

unsigned long NativeSpawnOne(unsigned long /*ptr*/,
                             unsigned long /*spawnPoint*/);

// Создаёт нативного игрока движка и сразу сажает его на точку спавна.
// Вынесено отдельно, чтобы пользоваться и из CreateEnginePlayer, и из
// повторной обработки очереди.
// ★ Сборка 150: игроков-ботов создаём в движке, только когда клиент человека «в уровне» ([человек+0x49C..+0x49E] = 1/1/1:
// уровень загружен, расширения, вошёл в уровень). Наблюдения 15.10: в 136 боты созданы ДО подключения клиента — у всех
// обе иконки; в 139–148 — пока клиент грузится (подключён, но не в уровне) — одна иконка, нет ника; созданные после
// первого спавна человека (поздний путь) — обе иконки и ник. У ботов 136 флаги 00 00 00 01, у человека 01 01 01 01
// (живое чтение) — флаги самого бота иконки не дают; важен момент создания относительно клиента.
// Сборка 151: по умолчанию ВЫКЛ (пользователь: в 150 боты появились только после дислокации и долго — хуже всех);
// включить EVBOTS_WAIT_INLEVEL=1.
static bool HumanInLevel() {
    static const bool on = [] {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_WAIT_INLEVEL", v, (DWORD)sizeof(v));
        return n == 1 && v[0] == '1';
    }();
    if (!on) return true;
    static bool s_inLevel = false;
    if (s_inLevel) return true;
    const unsigned long h = ::evbots::EVBotsEngineRealPlayer();
    if (h == 0) return false;
    unsigned char f[3] = {};
    unsigned long soldier = 0;
    __try {
        const unsigned char* p = (const unsigned char*)(ULONGLONG)h;
        f[0] = p[0x49C]; f[1] = p[0x49D]; f[2] = p[0x49E];
        soldier = *(const unsigned long*)(p + kOffPlayerSoldier);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    static unsigned char s_last[3] = {0xFF, 0xFF, 0xFF};
    if (f[0] != s_last[0] || f[1] != s_last[1] || f[2] != s_last[2]) {
        s_last[0] = f[0]; s_last[1] = f[1]; s_last[2] = f[2];
        LOG_INFO("Bridge: клиент человека: в уровне +0x49C/D/E = %u/%u/%u, солдат=%s", (unsigned)f[0], (unsigned)f[1],
                 (unsigned)f[2], soldier != 0 ? "есть" : "нет");
    }
    if (f[2] != 0 || soldier != 0) {
        s_inLevel = true;
        LOG_INFO("Bridge: клиент человека в уровне — создаём ботов в движке (сборка 150)");
    }
    return s_inLevel;
}

// Сборка 148: EVBOTS_VU_SPAWN=0 — по-старому (ранние боты получают тело от моста сразу после создания).
// Сборка 151: по умолчанию ВЫКЛ — как 136 (тело сразу от моста); иконки не дал (148). Включить EVBOTS_VU_SPAWN=1.
static bool VuSpawnMode() {
    static const bool on = [] {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VU_SPAWN", v, (DWORD)sizeof(v));
        return n == 1 && v[0] == '1';
    }();
    return on;
}

// ★ Сборка 149: журнал жизни бота (строки «ЖИЗНЬ»). Путь создания — ранний (очередь, NativeCreateOne) или поздний
// (LateCreateEngineImpl): по прогонам 145–148 две иконки и ник есть только у позднего.
struct BotPathRec { unsigned long ptr; char path; };  // 'E' — ранний, 'L' — поздний
static BotPathRec g_botPath[128];
static LONG g_botPathCount = 0;
static void RememberBotPath(unsigned long ptr, char path) {
    for (LONG i = 0; i < g_botPathCount; ++i) {
        if (g_botPath[i].ptr == ptr) { g_botPath[i].path = path; return; }
    }
    if (g_botPathCount < 128) { g_botPath[g_botPathCount].ptr = ptr; g_botPath[g_botPathCount].path = path; ++g_botPathCount; }
}

/// NativeBridge.BotLifeFields(ptr) -> строка: путь создания и поля игрока, которые различались у раннего и позднего бота
/// (живой дифф 148): +0x318 (ИИ), +0x31C (команда), +0x3C4 (солдат), +0x49C..+0x49E (в уровне), +0x4B0, +0x2D4, +0x2E0,
/// +0x308, число ненулевых слов в блоке +0x260..+0x2CC, здоровье солдата [солдат+0x34].
static char BotWeaponGroup(unsigned long ptr);  // сборка 152, определение — у ApplyBotWeaponsVector
int l_bridge_bot_life_fields(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    char path = '?';
    for (LONG i = 0; i < g_botPathCount; ++i) {
        if (g_botPath[i].ptr == ptr) { path = g_botPath[i].path; break; }
    }
    unsigned long v318 = 0, v31c = 0, v3c4 = 0, v4b0 = 0, v2d4 = 0, v2e0 = 0, v308 = 0;
    unsigned char b49c = 0, b49d = 0, b49e = 0;
    int blk = 0;
    float hp = -1.0f;
    bool ok = true;
    __try {
        const unsigned char* p = (const unsigned char*)(ULONGLONG)ptr;
        v318 = *(const unsigned long*)(p + 0x318);
        v31c = *(const unsigned long*)(p + 0x31C);
        v3c4 = *(const unsigned long*)(p + 0x3C4);
        v4b0 = *(const unsigned long*)(p + 0x4B0);
        v2d4 = *(const unsigned long*)(p + 0x2D4);
        v2e0 = *(const unsigned long*)(p + 0x2E0);
        v308 = *(const unsigned long*)(p + 0x308);
        b49c = p[0x49C]; b49d = p[0x49D]; b49e = p[0x49E];
        for (int off = 0x260; off <= 0x2CC; off += 4) {
            if (*(const unsigned long*)(p + off) != 0) ++blk;
        }
        if (v3c4 != 0) hp = *(const float*)((const unsigned char*)(ULONGLONG)v3c4 + 0x34);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) { lua_pushstring(L, "поля не читаются"); return 1; }
    // Сборка 152: + число перков (+0xA4), кит (+0x480), группа оружия (опыт А/Б)
    unsigned long va4 = 0, v480 = 0;
    __try {
        va4 = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0xA4);
        v480 = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x480);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    const char grp = BotWeaponGroup(ptr);
    char buf[320];
    ::snprintf(buf, sizeof(buf),
               "путь=%s оружие=%s ИИ318=%lu команда=%lu тело=%s hp=%.0f в_уровне=%u/%u/%u перковA4=%lu кит480=%08lX 4B0=%lu 2D4=%lX 2E0=%lX 308=%lu блок260=%d/28",
               path == 'L' ? "поздний" : (path == 'E' ? "ранний" : "?"),
               grp == 'A' ? "А(человека)" : "Б(мода)", v318, v31c, v3c4 != 0 ? "есть" : "нет",
               (double)hp, (unsigned)b49c, (unsigned)b49d, (unsigned)b49e, va4, v480, v4b0, v2d4, v2e0, v308, blk);
    lua_pushstring(L, buf);
    return 1;
}

/// NativeBridge.VuSpawnMode() -> true: тело только по SpawnSoldierAt мода (Lua повторяет его при привязке)
int l_bridge_vu_spawn_mode(lua_State* L) {
    lua_pushboolean(L, VuSpawnMode() ? 1 : 0);
    return 1;
}

static void RememberPtrName(unsigned long ptr, const char* name);  // сборка 159, определение — у выбора мода по имени
static bool EngineSpawnOn();  // сборка 186, определение ниже
static unsigned long NativeCreateOne(const char* name, long team) {
    unsigned char* base = NativeModuleBase();
    void* mgr = NativeManager();
    if (base == nullptr || mgr == nullptr) return 0;
    // ★ Точка по команде бота, по кругу, БЕЗ точки живого игрока. Запасного
    // пути на g_spawnSelf нет: раньше именно он сажал всех ботов на игрока
    // (прогон 39: 15 из 15 на F691EC80). Точек ещё нет — не создаём игрока,
    // очередь подождёт (см. EVBotsBridgeRetryPending).
    // Игрок не создаётся, пока нет точки: иначе останется «полу-бот» без тела.
    // Точки ещё не собраны (скан не закончен) — не создаём игрока вовсе.
    if (!::evbots::EVBotsEngineSpawnScanDone()) return 0;
    unsigned long spawnPoint =
        ::evbots::EVBotsEngineSpawnPointForTeam((unsigned int)team);
    // ВРЕМЕННО (13.10, прогон 42): у остальных точек скана ref битый, пригодна
    // только точка живого игрока. Без неё боты не получают тело вовсе — берём её.
    // Это возвращает старое поведение «спавн на игроке», пока не разобрана цепочка ref.
    if (spawnPoint == 0) spawnPoint = ::evbots::EVBotsEngineSpawnPointBestForTeam((unsigned int)team);  // 195: не чужая
    // Сборка 186: посадка как VU (createSoldier+spawnSoldier на позицию мода) точку не требует. Прогон 19:44 (Rush):
    // у всех 11 точек +0x19C пуст («НЕЛЬЗЯ») — боты стояли в очереди до дислокации человека.
    if (spawnPoint == 0 && !EngineSpawnOn()) return 0;

    const unsigned count = NativePlayerCount(mgr);
    // ★ Слоты ботов с 17: низкие слоты 1..16 — людям (бэкенд выдаёт человеку слот 1).
    // Раньше боты, созданные до входа клиента, занимали слоты 1..15 -> клиент
    // застревал на чёрном экране (прогон 56, KOTuk1 индекс 15 в списке игроков).
    (void)count;
    const unsigned long slot =
        2UL + (unsigned long)(InterlockedIncrement(&g_botSlotCounter) - 1);
    // ★ 13.10, прогон 62: со слотами 17+ клиент приписывал смерть бота себе
    // («Самоубийство», клиент «мёртв», сервер «жив»). Со слотами 2..16 (как в прогоне 52,
    // где убийства работали) человек сохраняет слот 1, который выдаёт бэкенд.
    using CreateFn = int(__fastcall*)(void*, void*, void*, void*, void*);
    using AddFn = void(__fastcall*)(void*, void*, void*, void*);

    int player = 0;
    __try {
        // ★ Аргументы берём У ДВИЖКА (сняты при реальном входе клиента),
        // а не выдумываем. Раньше здесь стояло a3 = 0x40603000 — движок
        // такого значения не передаёт, отсюда неполные игроки.
        long r1 = (long)slot, r2 = -1L, r3 = 0xFA000000L;
        ::evbots::EVBotsEngineCreateRefArgs(&r1, &r2, &r3);
        r1 = BotConnectionId(slot);  // a1 = номер соединения (+0x3BC), у бота 0
        r2 = -1L;                    // a2 = id игрока: -1 — движок выдаёт сам
        CreateFn createFn = (CreateFn)((ULONGLONG)base + kNativeCreatePlayerRva);
        player = createFn(mgr, nullptr, (void*)(ULONGLONG)r1,
                          (void*)(ULONGLONG)r2,
                          (void*)(ULONGLONG)r3);
        if (player != 0) {
            AddFn addFn = (AddFn)((ULONGLONG)base + kNativeAddToArrayRva);
            addFn(mgr, nullptr, (void*)(ULONGLONG)player, (void*)name);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (player == 0) return 0;

    // Прописываем минимально безопасное.
    // ПОЛЯ НЕ ПОМОГАЮТ (проверено 12.10.2026, три попытки):
    //   1) свои значения (1/команда/0)   -> движок ОТКАЗЫВАЕТ;
    //   2) копия трёх полей игрока       -> ИСКЛЮЧЕНИЕ;
    //   3) копия только +0x480 (комплект) -> ИСКЛЮЧЕНИЕ.
    // Значит поля — следствие, а не причина: настоящий игрок получает
    // корректный набор полноценным путём создания.
    // ★ ДИФФЕРЕНЦИАЛЬНЫЙ ДАМП: настоящий игрок против бота, поле в поле.
    // Сервер жалуется «Unknown player: BOT_* (onlineId: 0)» — у бота не
    // заполнено поле личности Blaze. Ищем его: печатаем только те смещения,
    // где у НАСТОЯЩЕГО игрока значение есть, а у бота — ноль. Один раз.
    if (name != nullptr && ::strncmp(name, "BOT_", 4) == 0) {
        static volatile LONG g_diffDone = 0;
        const unsigned long ref = ::evbots::EVBotsEngineRealPlayer();
        if (ref != 0 && ref != (unsigned long)player &&
            InterlockedCompareExchange(&g_diffDone, 0, 0) == 0) {
            InterlockedExchange(&g_diffDone, 1);
            int shown = 0;
            for (int off = 0; off < 0x2000 && shown < 90; off += 4) {
                unsigned int a = 0, b = 0;
                __try {
                    a = *(const unsigned int*)((const unsigned char*)ref + off);
                    b = *(const unsigned int*)(
                        (const unsigned char*)(ULONGLONG)player + off);
                } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (a == b || b != 0) continue;
                ++shown;
                LOG_INFO("DIFF +0x%04X: настоящий=%08X бот=%08X", off, a, b);
            }
            LOG_INFO("DIFF: показано %d смещений (у настоящего есть, у бота 0)",
                     shown);
        }
    }

    __try {
        unsigned char* p = (unsigned char*)(ULONGLONG)player;
        // ★★ ЛИЧНОСТЬ BLAZE. Дифференциальный дамп «настоящий игрок vs бот»
        // показал: у настоящего игрока +0x20 = 0x0DF9C16C (это personaId
        // из лога Blaze — тот же, что в «endpoint(234471788)»), у бота 0.
        // Из-за этого сервер писал:
        //     ServerTransactionMessageListener: Unknown player: BOT_* (onlineId: 0)
        // то есть слой, который РАЗДАЁТ ИГРОКОВ КЛИЕНТАМ, наших ботов не знал.
        // Даём каждому боту уникальный идентификатор в том же диапазоне.
        {
            unsigned long refId = 0;
            const unsigned long refPlayer = ::evbots::EVBotsEngineRealPlayer();
            if (refPlayer != 0) {
                __try {
                    refId = *(const unsigned long*)(
                        (const unsigned char*)refPlayer + kOffPlayerOnlineId);
                } __except (EXCEPTION_EXECUTE_HANDLER) { refId = 0; }
            }
            // Младшие биты делаем своими: идентификатор должен быть УНИКАЛЬНЫМ (сборка 171: и не равным persona человека).
            const unsigned long myId = BotOnlineId(refId, (unsigned long)slot);
            *reinterpret_cast<unsigned long*>(p + kOffPlayerOnlineId) = myId;
            SetBotOnlineIdName(p, name);  // сборка 176 (опыт ников)
            LOG_INFO("Bridge: \"%s\" onlineId=%08X (эталон %08X)",
                     name, myId, refId);
        }

        // ★ Эталон СПАВНА: то, что движок видит у настоящего игрока в момент
        // посадки. Прежние попытки копировали эталон, снятый ПОСЛЕ спавна
        // (у игрока уже был солдат) — там указатели на объекты солдата,
        // отсюда были исключения.
        // В момент спавна (проверено 12.10.2026 по логу):
        //   +0x318 = 0         <- мы ошибочно писали 1, движок это видел
        //   +0x31C = команда
        //   +0x480 = УКАЗАТЕЛЬ <- у бота был 0, у настоящего заполнен
        unsigned long s318 = 0, s31c = 0, s480 = 0;
        ::evbots::EVBotsEngineSpawnRefFields(&s318, &s31c, &s480);
        // +0x31C — команда бота (своя, из аргумента).
        // Сборка 186: команды 1..4 (SquadDeathMatch — 4 команды; было только 1/2 — петля TeamChange → Kill мода)
        *reinterpret_cast<int*>(p + kOffPlayerTeam) =
            (int)((team >= 1 && team <= 4) ? team : 1);
        // ★★ ПРИЗНАК AI-ИГРОКА. В движке тип игрока задаётся ДАННЫМИ:
        //     mpsoldier -> PlayerSpawnType_HumanPlayer
        //     aisoldier -> PlayerSpawnType_AiPlayer
        // и проверяется как  isAIPlayer() != (getData().getPlayerType()
        // == PlayerSpawnType_HumanPlayer).
        // AI-игроку подключение НЕ НУЖНО — он штатно реплицируется, у него
        // есть ServerAILocoComponent (перемещение) и он виден клиенту.
        // Поскольку мы создаём игрока по бланке MpSoldier (HumanPlayer),
        // движок ждёт сетевого подключения, которого нет, и не рисует бота.
        // (Сборка 168: опыт EVBOTS_AI_PLAYER — признак AI при создании — удалён, «не помог», MODLOG 5860.)
        *reinterpret_cast<unsigned int*>(p + kOffPlayerIsAi) = (unsigned)s318;
        if (s480 != 0) {
            SetPlayerRef480((unsigned long)player, s480);  // с addref, не напрямую
        }
        LOG_INFO("Bridge: \"%s\" эталон СПАВНА 318=%08X 31C(эт)=%08X "
                 "480=%08X -> команда=%ld", name, s318, s31c, s480, team);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: не удалось прописать поля игроку %p", (void*)player);
    }
    LOG_INFO("Bridge: \"%s\" ptr=%08X команда=%ld", name, player, team);

    // ★ КЛЮЧЕВОЕ ПОЛЕ +0x480 — «свой комплект». Шаблон спавна FUN_00e59340
    // читает у игрока три поля: +0x318 (флаг AI), +0x31C (команда) и
    // +0x480. Первые два мы заполнили, третье — кандидат на причину
    // отказа движка сажать бота. Снимаем для сравнения с живым игроком.
    __try {
        const unsigned char* pp = (const unsigned char*)(ULONGLONG)player;
        unsigned int v480 = 0, v318 = 0, v31c = 0, v3c4 = 0;
        __try {
            v480 = *(const unsigned int*)(pp + 0x480);
            v318 = *(const unsigned int*)(pp + 0x318);
            v31c = *(const unsigned int*)(pp + 0x31C);
            v3c4 = *(const unsigned int*)(pp + 0x3C4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        LOG_INFO("Bridge: \"%s\" поля: +0x318=%08X +0x31C=%08X +0x480=%08X "
                 "+0x3C4(солдат)=%08X", name, v318, v31c, v480, v3c4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }

    // (Сборка 168: опыт EVBOTS_BRIDGE_ONCREATE — свой onCreatePlayerMessage, ронял сервер 80000003 — удалён.)

    // ★ Регистрируем бота в МЕНЕДЖЕРЕ ОТРЯДОВ. Без этого бот есть в
    // ServerPlayerManager, но его нет в ServerPlayerSquadManager — отсюда
    // «Player count for Team2: 7» при 16 игроках, пустой таб, ноль очков
    // и отсутствие респавна.
    // Аргументы берём у НАСТОЯЩЕГО игрока (снял перехватчик addToTeam).
    {
        unsigned long a2 = 0, a3 = 0, a4 = 0;
        const unsigned long squadMgr = ::evbots::EVBotsEngineSquadManager();
        if (::evbots::EVBotsEngineSquadAddTeamArgs(&a2, &a3, &a4) &&
            squadMgr != 0) {
            // (Сборка 168: опыт EVBOTS_DUMP_INPUT 13.10 — дамп полей ввода бота — удалён.)

    // ★ ПОРЯДОК АРГУМЕНТОВ (13.10, разбор по логу перехватчика):
            // функция СТАТИЧЕСКАЯ, это НЕ метод с this:
            //     f(игрок, команда, отряд, менеджерОтрядов)
            // Первые две мои попытки упали: я подставлял бота вместо
            // менеджера, потом объявил неверное число аргументов.
            using AddTeamFn = int(__cdecl*)(void*, void*, void*, void*);
            const unsigned long teamId = (team >= 1 && team <= 4) ? (unsigned long)team : 1UL;  // сборка 186: 1..4
            __try {
                AddTeamFn fn = (AddTeamFn)((ULONGLONG)base + 0x00D8EF80UL);
                const int r = fn((void*)(ULONGLONG)player, (void*)teamId,
                                 (void*)(ULONGLONG)a3, (void*)squadMgr);
                LOG_INFO("Bridge: \"%s\" addToTeam(игрок=%08X команда=%lu "
                         "отряд=%lu менеджер=%08X) -> %d",
                         name, player, teamId, a3, squadMgr, r);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LOG_ERR("Bridge: сбой addToTeam для \"%s\"", name);
            }
        } else {
            LOG_INFO("Bridge: \"%s\" addToTeam пропущен — эталон/менеджер "
                     "ещё не сняты (mgr=%08X)", name, squadMgr);
        }
    }
    VuFinishCreate((unsigned long)player, name);  // сборка 171: как VU CreatePlayer (сообщение создания, «в уровне»)

    if (g_watchCount < 64) {
        g_watchBot[g_watchCount] = (unsigned long)player;
        g_watchHadSoldier[g_watchCount] = 0;
        ++g_watchCount;
    }
    // ★ Сборка 148: КАК В VU — тело только по SpawnSoldierAt мода (разбор VU, re\vu\SEQUENCE-REPORT.md: CreatePlayer
    // тела не даёт, к SpawnSoldierAt выбор мода уже в игроке). Раньше здесь мост сажал тело сам, до выбора мода: у таких
    // ботов одна иконка и нет ника (прогоны 145–147; поздний путь LateCreateEngineImpl тела не сажал — у его ботов всё есть).
    // Lua повторяет отложенный SpawnSoldierAt мода при привязке, после кита/внешности/оружия/перков.
    RememberBotPath((unsigned long)player, 'E');  // сборка 149
    if (VuSpawnMode()) {
        LOG_INFO("Bridge: \"%s\" создан без тела — тело по SpawnSoldierAt мода (как VU)", name);
        return (unsigned long)player;
    }
    // ★ Тела — только после живого спавна человека (13.10, прогон 54): с ботами,
    // посаженными ДО входа клиента, клиент завис на загрузке (чёрный экран).
    // Игрок в движке уже есть (счётчик 15/64), посадка откладывается и делается
    // в EVBotsBridgeRetryPending, как только появится g_spawnSelf.
    // ★ Сборка 174: тело — всегда через отложенную очередь: она ждёт выбор мода (оружие), см. EVBotsBridgeRetryPending.
    // Прогон 173: с тиком моста каждый кадр (172) тело садилось в том же кадре, что и создание, до привязки мода —
    // 6 из 15 первых тел без оружия в руках (оружие движка n=0).
    (void)spawnPoint;
    if (g_deferredSpawnCount < 32) {
        g_deferredSince[g_deferredSpawnCount] = ::GetTickCount64();
        g_deferredSpawn[g_deferredSpawnCount++] = (unsigned long)player;
    }
    return (unsigned long)player;
}

// Определение EVBotsBridgeRetryPending вынесено ПОСЛЕ анонимного
// namespace (иначе внутреннее связывание и SpmProbe не увидит символ,
// LNK2019). Там же оно и лежит.

// ★ Поля бота, созданного ПОСЛЕ входа человека (сборка 80, прогон 79). Этот путь (l_bridge_create_player)
// создавал игрока в движке и не писал ему команду, онлайн-id и кит — их писал только NativeCreateOne
// (очередь). Бот получал +0x31C=0, движок сам сажал его в команду 2, а fun-bots считал, что в команде 1
// людей мало, и создавал новых ботов: за 15 минут набежало 37 вместо 15. Те же значения, что в NativeCreateOne.
static void InitLateBotFields(const char* name, long team, unsigned long slot, unsigned long player) {
    __try {
        unsigned char* p = (unsigned char*)(ULONGLONG)player;
        unsigned long refId = 0;
        const unsigned long refPlayer = ::evbots::EVBotsEngineRealPlayer();
        if (refPlayer != 0) {
            __try {
                refId = *(const unsigned long*)((const unsigned char*)refPlayer + kOffPlayerOnlineId);
            } __except (EXCEPTION_EXECUTE_HANDLER) { refId = 0; }
        }
        *reinterpret_cast<unsigned long*>(p + kOffPlayerOnlineId) = BotOnlineId(refId, slot);
        SetBotOnlineIdName(p, name);  // сборка 176 (опыт ников)
        unsigned long s318 = 0, s31c = 0, s480 = 0;
        ::evbots::EVBotsEngineSpawnRefFields(&s318, &s31c, &s480);
        *reinterpret_cast<int*>(p + kOffPlayerTeam) = (int)((team >= 1 && team <= 4) ? team : 1);  // сборка 186: 1..4
        *reinterpret_cast<unsigned int*>(p + kOffPlayerIsAi) = (unsigned)s318;
        if (s480 != 0) SetPlayerRef480((unsigned long)player, s480);
        LOG_INFO("Bridge: \"%s\" поля позднего бота: команда=%ld 318=%08lX 480=%08lX", name, team, s318, s480);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: не удалось прописать поля позднему игроку %p", (void*)(ULONGLONG)player);
    }
}

// Повторная обработка очереди: как только появилась точка спавна (значит
// на сервер зашёл живой игрок), создаём нативных игроков. Так ведут себя
// боты и в VU — до прихода игрока их в матче нет.
// Поздние боты: движок трогаем только в хуке тика (потоке движка). Прогон 93: вызов из таймера Lua завис внутри движка
// (после смены команды, 05:19:32): таймер встал, Lua и heartbeat тоже. Lua ставит запрос и ждёт результат до 3 с.
static volatile LONG g_lateReqId = 0;    // номер запроса от Lua (0 — запросов нет)
static volatile LONG g_lateDoneId = 0;   // номер последнего выполненного запроса
static LONG g_lateSeq = 0;
static char g_lateName[64] = {};
static long g_lateTeam = 0;
static unsigned long g_lateResult = 0;
static volatile LONG g_hookAliveSec = 0;  // секунда, когда хук тика последний раз работал (0 — не работал)

/// Создание позднего бота (вызывается в хуке): движок, отряд, поля, список наблюдения. Возвращает указатель или 0.
static unsigned long LateCreateEngineImpl(const char* name, long team) {

    unsigned char* base = NativeModuleBase();
    void* mgr = NativeManager();

    // Диагностический переключатель: EVBOTS_BRIDGE_ENGINE=0 — не трогать
    // движок.
    // ВАЖНО (баг, из-за которого прогоны противоречили друг другу):
    // GetEnvironmentVariableA(..., nullptr, 0) возвращает 0, когда переменная
    // НЕ ЗАДАНА, а не когда она равна "0". Поэтому сравниваем ЗНАЧЕНИЕ,
    // иначе «убрали переменную» = «выключили мост».
    bool engineCallsEnabled = true;
    char toggle[8] = {};
    const DWORD tn = ::GetEnvironmentVariableA("EVBOTS_BRIDGE_ENGINE",
                                               toggle,
                                               (DWORD)sizeof(toggle));
    if (tn > 0) {
        engineCallsEnabled = !(tn == 1 && toggle[0] == '0');
    }
    if (!engineCallsEnabled) {
        LOG_INFO("Bridge: EVBOTS_BRIDGE_ENGINE=0 — вызов движка отключён");
        return 0;
    }

    if (base == nullptr || mgr == nullptr) {
        LOG_INFO("Bridge: нет движка (base=%p mgr=%p) — игрок только в Lua",
                 (void*)base, mgr);
        return 0;
    }

    // ★ Нет точки спавна -> не создаём нативного игрока (см. комментарий
    // выше про зависание клиента). Запоминаем и ждём.
    if (::evbots::EVBotsEngineSpawnPointBest() == 0 && !EngineSpawnOn()) {  // сборка 186: VU-посадке точка не нужна
        return 0;  // в очередь ставит вызывающий (одна запись на создание, иначе дубли ботов)
    }

    // Слот уникален: первый свободный после последнего игрока.
    const unsigned count = NativePlayerCount(mgr);
    // ★ Слоты ботов с 17: низкие слоты 1..16 — людям (бэкенд выдаёт человеку слот 1).
    // Раньше боты, созданные до входа клиента, занимали слоты 1..15 -> клиент
    // застревал на чёрном экране (прогон 56, KOTuk1 индекс 15 в списке игроков).
    (void)count;
    const unsigned long slot =
        2UL + (unsigned long)(InterlockedIncrement(&g_botSlotCounter) - 1);
    // ★ 13.10, прогон 62: со слотами 17+ клиент приписывал смерть бота себе
    // («Самоубийство», клиент «мёртв», сервер «жив»). Со слотами 2..16 (как в прогоне 52,
    // где убийства работали) человек сохраняет слот 1, который выдаёт бэкенд.

    using CreateFn = int(__fastcall*)(void*, void*, void*, void*, void*);
    using AddFn = void(__fastcall*)(void*, void*, void*, void*);

    const ULONGLONG b = (ULONGLONG)base;
    // ★ Сборка 93 (прогон 92): был int. Адрес ≥ 0x80000000 становился отрицательным, Lua получал отрицательный
    // nativePtr, и PollDeadBots (беззнаковый) не находил позднего бота: мод не узнавал о его смерти и убивал снова.
    unsigned long player = 0;
    __try {
        CreateFn createFn = (CreateFn)(b + kNativeCreatePlayerRva);
        // ★ Аргументы — те, что движок реально передаёт при входе клиента
        // (сняты перехватчиком createPlayer, 12.10.2026). Раньше стояло
        // a3 = 0x40603000 — это выдуманное значение, из-за которого игроки
        // создавались неполными.
        long r1 = (long)slot, r2 = -1L, r3 = 0xFA000000L;
        ::evbots::EVBotsEngineCreateRefArgs(&r1, &r2, &r3);
        r1 = BotConnectionId(slot);  // a1 = номер соединения (+0x3BC), у бота 0
        r2 = -1L;                    // a2 = id игрока: -1 — движок выдаёт сам
        player = (unsigned long)createFn(mgr, nullptr, (void*)(ULONGLONG)r1,
                          (void*)(ULONGLONG)r2,
                          (void*)(ULONGLONG)r3);
        if (player != 0) {
            AddFn addFn = (AddFn)(b + kNativeAddToArrayRva);
            addFn(mgr, nullptr, (void*)(ULONGLONG)player, (void*)name);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: исключение при создании игрока \"%s\"", name);
        return 0;
    }

    if (player == 0) {
        LOG_ERR("Bridge: движок не создал игрока \"%s\" (слот %u)", name,
                (unsigned)slot);
        return 0;
    }

    InitLateBotFields(name, team, slot, player);
    VuFinishCreate(player, name);  // сборка 171: как VU CreatePlayer (сообщение создания, «в уровне»)
    RememberBotPath(player, 'L');  // сборка 149
    // ★ Сборка 92 (прогон 91): поздний бот не попадал в список наблюдения. Из-за этого его смерть не доходила до Lua
    // (мод считал бота живым и убивал его каждые ~2 с), а удаление в движке отказывало (l_bridge_delete_player
    // требует «tracked»), и бот оставался в игре. Теперь он под наблюдением, как ранние.
    if (g_watchCount < 64) {
        g_watchBot[g_watchCount] = (unsigned long)player;
        g_watchHadSoldier[g_watchCount] = 0;
        ++g_watchCount;
    } else {
        LOG_ERR("Bridge: список наблюдения полон — поздний бот \"%s\" без отслеживания смерти", name);
    }
    LOG_INFO("Bridge: игрок \"%s\" создан в движке ptr=%p слот=%u "
            "(всего было %u, команда %ld)",
            name, (void*)(ULONGLONG)player, (unsigned)slot, count, team);
    RememberPtrName(player, name);  // сборка 159: имя ↔ указатель (выбор мода по имени)
    return player;
}

/// Запрос из Lua: если уже выполняется хук (поток движка) — создаём сразу. Если хук не работает (до входа игрока в мир) —
/// возвращаем 0 без ожидания: вызывающий ставит бота в очередь. Иначе ставим запрос и ждём хук (до 3 с).
static unsigned long RunLateCreateInHook(const char* name, long team) {
    if (InEngineTick()) return LateCreateEngineImpl(name, team);
    // ★ 238-3 (А1): тик Lua из кадра движка (EVBOTS_SERVER_TICK=frame) — ждать хук тика нельзя ни из какого потока: из потока
    // кадра (гипотеза: хук ServerPlayerManager::update идёт в нём же — подтвердить журналом) ждали бы сами себя, а из чужого
    // потока с локом Lua (Level:Loaded) стоял бы кадр на том же локе (проверка 26.10). Сразу 0: бот в очередь, хук создаст его.
    if (::evbots::EVBotsServerFrameTickActive()) return 0;
    const LONG nowSec = (LONG)(::GetTickCount64() / 1000ULL);
    if (nowSec - InterlockedCompareExchange(&g_hookAliveSec, 0, 0) > 2) return 0;
    const LONG id = InterlockedIncrement(&g_lateSeq);
    ::strncpy(g_lateName, name, sizeof(g_lateName) - 1);
    g_lateName[sizeof(g_lateName) - 1] = '\0';
    g_lateTeam = team;
    InterlockedExchange(&g_lateReqId, id);
    const ULONGLONG deadline = ::GetTickCount64() + 3000ULL;
    while (InterlockedCompareExchange(&g_lateDoneId, 0, 0) != id) {
        if (::GetTickCount64() > deadline) {
            // отменяем запрос, если хук его ещё не взял: тогда бот уйдёт в очередь ровно один раз
            if (InterlockedCompareExchange(&g_lateReqId, 0, id) == id) {
                LOG_ERR("Bridge: создание бота \"%s\" не выполнено хуком за 3 с — в очередь", name);
                return 0;
            }
            // хук уже взял запрос: ждём его результат (ещё до 10 с), создание состоится один раз
            const ULONGLONG hardDeadline = ::GetTickCount64() + 10000ULL;
            while (InterlockedCompareExchange(&g_lateDoneId, 0, 0) != id) {
                if (::GetTickCount64() > hardDeadline) {
                    LOG_ERR("Bridge: создание бота \"%s\" — хук не ответил за 13 с", name);
                    return 0;
                }
                ::Sleep(1);
            }
            return g_lateResult;
        }
        ::Sleep(1);
    }
    return g_lateResult;
}

/// NativeBridge.RealPlayerName() -> имя настоящего игрока-человека из движка (сборка 139) или nil.
int l_bridge_real_player_name(lua_State* L) {
    char nm[64] = {};
    if (::evbots::EVBotsEngineRealPlayerName(nm, sizeof(nm))) {
        lua_pushstring(L, nm);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// NativeBridge.CreateEnginePlayer(name, team, squad) из Lua: движок не трогаем, ждём создание от хука тика.
int l_bridge_create_player(lua_State* L) {
    // Первая же строка: если её нет в логе — падение происходит ДО входа в функцию (в вызывающем Lua-коде мода).
    LOG_INFO("Bridge: CreateEnginePlayer ВЫЗВАНА");
    const char* name = luaL_checkstring(L, 1);
    const long team = (long)luaL_optinteger(L, 2, 1);
    (void)luaL_optinteger(L, 3, 0);  // squadId — движку сейчас не нужен
    // «Host» и настоящий игрок-человек (сборка 139) в движок не создаём: человек уже в движке.
    // ★ Сборка 202: любой человек с этим именем (раньше — только первый: второму человеку мост создал бы тёзку-бота).
    const bool isHuman = ::evbots::EVBotsEngineHumanByName(name) != 0;
    if (::strcmp(name, "Host") == 0 || isHuman) {
        lua_pushnil(L);
        return 1;
    }
    // Сборка 150: пока клиент человека не «в уровне» — бот только в очередь (см. HumanInLevel).
    // ★ Сборка 157 (трек 7, прогон 155): нет пригодной точки спавна — поздний путь всё равно вернёт 0 (LateCreateEngineImpl),
    // поэтому сразу в очередь, без ожидания хука (до 2 с на бота: проход моста раз в 2000 мс, одна заявка за проход).
    // Очередь создаёт всех ожидающих за один проход — боты выходят пачкой.
    const bool noSpawnPoint = ::evbots::EVBotsEngineSpawnPointBest() == 0 && !EngineSpawnOn();  // сборка 186
    const unsigned long player = (HumanInLevel() && !noSpawnPoint) ? RunLateCreateInHook(name, team) : 0;
    if (player == 0) {
        // Хук ещё не работает (до входа в мир) или не ответил: ставим в очередь, создадим, когда появится точка спавна.
        RememberPending(name, team);
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)(ULONGLONG)player);
    return 1;
}

// Очередь ботов, которым спавн отложен: мод создаёт их при загрузке,
// когда точка спавна ещё неизвестна (перехватчик ещё не сработал).
constexpr unsigned kMaxPending = 128;
unsigned long g_pending[kMaxPending];
volatile LONG g_pendingCount = 0;

// ★ applyCustomization (13.10, разбор 0xE322F0 по дизассемблеру): cdecl, 3 стековых
// аргумента, RET 0. a1=[ebp+8] — объект комплекта (поле +0x4B8), a2=[ebp+0xC] —
// игрок (поле +0x54C — его солдат), a3=[ebp+0x10] — addToPool. Вызываем после посадки
// бота: без неё у солдата нет визуальных частей (заглушка API, STUB-RESEARCH §7).
constexpr unsigned long kApplyCustomRva = 0x00A322F0UL;  // 0xE322F0 - ImageBase 0x400000

void ApplyBotKit(unsigned long ptr) {
    // ★ 13.10 (прогон 50): настоящие вызовы движка для ботов — applyCustomization(
    // a1=ИГРОК, a2=ЕГО СОЛДАТ, a3=0). Кит движок берёт из +0x480 игрока сам.
    // Прежние варианты с «китом» в a1 были неверными. Три варианта по кругу:
    //   V0: (игрок, солдат, 0)  — как у движка
    //   V1: (игрок, солдат, 1)  — addToPool
    //   V2: (игрок, игрок, 0)   — проверка: сработает ли, если a2 не солдат
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0) return;
    // Прогон 60: варианты с addToPool=1 и «игрок, игрок» падали внутри движка
    // (SEH ловил, но состояние движка могло остаться сломанным: у пользователя не
    // работали самоубийство в технике, респавн и смена команды). Только V0.
    int variant = 0;  // не const: иначе C4127 (условие-константа) при /WX
    unsigned long soldier = 0;
    __try {
        soldier = *(const unsigned long*)((const unsigned char*)ptr + kOffPlayerSoldier);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        soldier = 0;
    }
    unsigned long a2 = (variant == 2) ? ptr : soldier;
    const int addToPool = (variant == 1) ? 1 : 0;
    if (a2 == 0) {
        LOG_INFO("Bridge: applyCustomization V%d пропущен — у бота нет солдата", variant);
        return;
    }
    using ApplyFn = void(__cdecl*)(void*, void*, int);
    __try {
        ApplyFn fn = (ApplyFn)((ULONGLONG)base + kApplyCustomRva);
        fn((void*)(ULONGLONG)ptr, (void*)(ULONGLONG)a2, addToPool);
        LOG_INFO("Bridge: applyCustomization V%d(игрок=%p, a2=%08lX, addToPool=%d) — вернулась",
                 variant, (void*)ptr, a2, addToPool);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: applyCustomization V%d(игрок=%p, a2=%08lX) — исключение в движке",
                variant, (void*)ptr, a2);
    }
}

// ★ addToTeam (13.10, дизассемблер 0x118EF80): cdecl, 4 стековых аргумента:
// (игрок, индекс команды [сравнивается с размером глобального вектора 0x285BB38],
// отряд [byte], флаг [byte]), возврат al. Менеджера в аргументах НЕТ — прежние
// падения были у перехватчика с неверными аргументами. Три варианта по кругу:
//   V0 (игрок, команда, 0, 1)   V1 (игрок, команда, 1, 1)   V2 (игрок, команда, 0, 0)
constexpr unsigned long kAddToTeamRvaBridge = 0x00D8EF80UL;

static void XformCheckStep(unsigned long player, const char* step, bool after);  // 238-6, определение — у SoldierXformStackBad

void RegisterBotTeam(unsigned long ptr) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0) return;
    // Проверка гипотезы (13.10, прогоны 61–63): через ~30 с после высадки движок сам
    // делает switchTeams(человек) — возможно, выравнивает команды с учётом наших ботов.
    // EVBOTS_BOT_TEAM=0 — не регистрировать ботов в командах (по умолчанию регистрируем).
    {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_BOT_TEAM", v, (DWORD)sizeof(v));
        if (n == 1 && v[0] == '0') return;
    }
    // Аргументы как у движка для человека (консоль сервера: addToTeam(KOTuk1, Team1, 1, 0)).
    int variant = 0;  // не const: иначе C4127 (условие-константа) при /WX
    unsigned int team = 0;
    __try {
        team = *(const unsigned int*)((const unsigned char*)ptr + kOffPlayerTeam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const unsigned int squad = 1u;
    const unsigned int flag = 0u;
    using AddFn = unsigned char(__cdecl*)(void*, unsigned int, unsigned int, unsigned int);
    unsigned int r = 0xFFFFFFFFu;
    XformCheckStep(ptr, "addToTeam", false);  // 238-6
    __try {
        AddFn fn = (AddFn)((ULONGLONG)base + kAddToTeamRvaBridge);
        r = fn((void*)(ULONGLONG)ptr, team, squad, flag);
        LOG_INFO("Bridge: addToTeam V%d(игрок=%p, команда=%u, отряд=%u, флаг=%u) -> %u",
                 variant, (void*)ptr, team, squad, flag, r & 0xFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: addToTeam V%d(игрок=%p) — исключение в движке", variant, (void*)ptr);
    }
    XformCheckStep(ptr, "addToTeam", true);
}

// ★ Набор бота (13.10, по строкам botlayer + дифф игрока): у игрока набор — вектор
// указателей на unlock-ассеты: [p+0x4B8]=begin, [p+0x4BC]=end, [p+0x4C0]=конец
// ёмкости, встроенный буфер на p+0x4CC (16 элементов). У бота буфер пуст — отсюда
// нет класса/модели (метка «снайпер»). Копируем вектор человека (a1 spawnSoldier)
// в бота; потом applyCustomization(игрок, солдат, 0).
// ★★ +0x4AC НЕ КОПИРОВАТЬ (13.10, разбор 0xD73AF0/0x4F4660): это СЛАБАЯ ССЫЛКА
// игрока на себя — блок {указатель на владельца, счётчик}, его выдаёт 0xD73AF0,
// а PointSystem берёт через неё «кто это» при убийстве (0x131B807, 0x131BA28).
// С копией (прогоны 60–64) слабая ссылка каждого бота вела на ЧЕЛОВЕКА: убийство
// бота засчитывалось как смерть человека от себя -> «Самоубийство», клиент «мёртв».
constexpr int kOffKitBegin = 0x4B8;
constexpr int kOffKitEnd = 0x4BC;
constexpr int kOffKitCap = 0x4C0;
constexpr int kOffKitInline = 0x4CC;

// ★ Оружие игрока (13.10, разбор 0xE33040 и applyCustomization 0xE322F0):
// вектор слотов [p+0x50C] begin / [p+0x510] end, элемент 0x24 байта:
// +0 unlock-ассет оружия, +4.. навесы, +0x20 число навесов. Движок делает
// resize(10) через 0xE293A0 (thiscall на p+0x50C, 1 стековый, RET 4) и пишет
// слот; applyCustomization(игрок, солдат, 0) создаёт оружие из этого вектора.
// У бота вектор пуст — отсюда «нет оружия в руках». Копируем слоты человека.
// EVBOTS_BOT_WEAPONS=0 — не копировать.
constexpr int kOffWpnBegin = 0x50C;
constexpr int kOffWpnEnd = 0x510;
constexpr unsigned long kWpnEntrySize = 0x24;
constexpr unsigned long kWpnSlots = 10;
constexpr unsigned long kWpnResizeRva = 0x00A293A0UL;  // VA 0xE293A0
static unsigned char g_wpnTpl[kWpnSlots * kWpnEntrySize];
static unsigned long g_wpnTplN = 0;
// ★ Сборка 186: после смены уровня слоты человека (+0x50C) до его дислокации держат указатели старого уровня — копия
// давала боту мусор (прогон 16:03, MP_003: имя ассета — мусор, запись по освобождённой памяти, 3 VEH). Пока у человека
// нет тела на новом уровне — шаблон не берём (как на старте сервера: «у человека слотов нет»).
static volatile LONG g_wpnTplStale = 0;

// Шаблоны набора по командам: снимаются с живого игрока при каждой посадке бота
// (команда человека — g_humanTeam). Бот команды T берёт шаблон T; если его ещё нет
// (человек не играл за T) — набор человека как есть (модель будет, но «чужая»).
static unsigned long g_kitTpl[3][16];
static unsigned long g_kitTplN[3] = {0, 0, 0};
// ★ Сборка 252: шаблон набора тоже устаревает со сменой уровня — как слоты оружия (186). Друг 29.10 (251): XP4_Parl, XP4_Quake,
// XP4_FD — бот садился до дислокации человека, набор +0x4B8 держал ассеты прошлого уровня → applyCustomization (0xE322F0)
// читал освобождённую память (EIP 0xE32387, [e+0x28] = 0) → 16–31 исключение и падение сервера через 2 с – 2 мин.
static volatile LONG g_kitTplStale = 0;
void EVBotsMarkWeaponTemplateStale() {
    InterlockedExchange(&g_wpnTplStale, 1);
    g_wpnTplN = 0;
    InterlockedExchange(&g_kitTplStale, 1);
    g_kitTplN[0] = g_kitTplN[1] = g_kitTplN[2] = 0;
}

bool BotWeaponsEnabled() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_BOT_WEAPONS", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;
    }
    return s_on == 1;
}

// Снимает слоты оружия человека в шаблон и кладёт их боту (вектор бота
// расширяется движковым resize, память — движковая).
void PrepareBotWeapons(unsigned long human, unsigned long bot) {
    if (!BotWeaponsEnabled() || human == 0 || bot == 0) return;
    if (InterlockedCompareExchange(&g_wpnTplStale, 0, 0) != 0) {
        unsigned long hs = 0;
        __try {
            hs = *(const unsigned long*)((const unsigned char*)human + kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hs = 0;
        }
        if (hs == 0) {
            static volatile LONG s_n = 0;
            if (InterlockedIncrement(&s_n) % 50 == 1)
                LOG_INFO("Bridge: оружие: слоты человека старого уровня — шаблон не берём до его дислокации (бот %p)", (void*)bot);
            return;
        }
        InterlockedExchange(&g_wpnTplStale, 0);
        LOG_INFO("Bridge: оружие: человек в мире на новом уровне — шаблон слотов снова берём");
    }
    unsigned long hBegin = 0, hEnd = 0;
    __try {
        hBegin = *(const unsigned long*)((const unsigned char*)human + kOffWpnBegin);
        hEnd = *(const unsigned long*)((const unsigned char*)human + kOffWpnEnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (hBegin != 0 && hEnd > hBegin && (hEnd - hBegin) % kWpnEntrySize == 0 &&
        (hEnd - hBegin) / kWpnEntrySize <= kWpnSlots) {
        const unsigned long hn = (hEnd - hBegin) / kWpnEntrySize;
        __try {
            ::memcpy(g_wpnTpl, (const void*)(ULONGLONG)hBegin, hn * kWpnEntrySize);
            g_wpnTplN = hn;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const unsigned long n = g_wpnTplN;
    if (n == 0) {
        LOG_INFO("Bridge: оружие: у человека слотов нет (begin=%08lX end=%08lX)", hBegin, hEnd);
        return;
    }
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return;
    unsigned long bBegin = 0, bEnd = 0;
    using ResizeFn = void(__fastcall*)(void*, void*, unsigned long);
    __try {
        ((ResizeFn)((ULONGLONG)base + kWpnResizeRva))(
            (void*)(ULONGLONG)(bot + kOffWpnBegin), nullptr, n);
        bBegin = *(const unsigned long*)((const unsigned char*)bot + kOffWpnBegin);
        bEnd = *(const unsigned long*)((const unsigned char*)bot + kOffWpnEnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: оружие: resize вектора бота %p — исключение", (void*)bot);
        return;
    }
    if (bBegin == 0 || bEnd < bBegin || (bEnd - bBegin) < n * kWpnEntrySize) {
        LOG_ERR("Bridge: оружие: вектор бота %p не вырос (begin=%08lX end=%08lX)",
                (void*)bot, bBegin, bEnd);
        return;
    }
    __try {
        ::memcpy((void*)(ULONGLONG)bBegin, g_wpnTpl, n * kWpnEntrySize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: оружие: запись в бота %p не удалась", (void*)bot);
        return;
    }
    LOG_INFO("Bridge: оружие: бот %p <- %lu слотов человека (слот0=%08lX)", (void*)bot, n,
             *(const unsigned long*)g_wpnTpl);
}

void PrepareBotKit(unsigned long bot) {
    const unsigned long human = ::evbots::EVBotsEngineSpawnKit();
    if (bot == 0 || human == 0 || human == bot) return;
    PrepareBotWeapons(human, bot);
    unsigned long hBegin = 0, hEnd = 0;
    unsigned long bBegin = 0, bCap = 0;
    unsigned int botTeam = 0;
    __try {
        hBegin = *(const unsigned long*)((const unsigned char*)human + kOffKitBegin);
        hEnd = *(const unsigned long*)((const unsigned char*)human + kOffKitEnd);
        bBegin = *(const unsigned long*)((const unsigned char*)bot + kOffKitBegin);
        bCap = *(const unsigned long*)((const unsigned char*)bot + kOffKitCap);
        botTeam = *(const unsigned int*)((const unsigned char*)bot + kOffPlayerTeam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: набор: чтение полей не удалось (бот %p)", (void*)bot);
        return;
    }
    // 1) Снимаем шаблон с человека для его текущей команды.
    // Сборка 186: SQDM — команды 1..4; шаблон набора — по фракции (1/3 → 1 US, 2/4 → 2 RU)
    const unsigned int humanTeamRaw = ::evbots::EVBotsEngineHumanTeam();
    const unsigned int humanTeam = (humanTeamRaw >= 1 && humanTeamRaw <= 4) ? ((humanTeamRaw % 2 == 0) ? 2u : 1u) : 0u;
    const unsigned int botFaction = (botTeam >= 1 && botTeam <= 4) ? ((botTeam % 2 == 0) ? 2u : 1u) : 0u;
    // Сборка 252: после смены уровня набор человека до его дислокации — ассеты прошлого уровня, шаблон не берём.
    bool humanKitFresh = true;
    if (InterlockedCompareExchange(&g_kitTplStale, 0, 0) != 0) {
        unsigned long hs = 0;
        __try {
            hs = *(const unsigned long*)((const unsigned char*)human + kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hs = 0;
        }
        if (hs != 0) {
            InterlockedExchange(&g_kitTplStale, 0);
            LOG_INFO("Bridge: набор: человек в мире на новом уровне — шаблон набора снова берём");
        } else {
            humanKitFresh = false;
        }
    }
    if (humanKitFresh && hEnd >= hBegin && (hEnd - hBegin) <= 0x40 && (humanTeam == 1 || humanTeam == 2)) {
        const unsigned long hn = (hEnd - hBegin) / 4;
        if (hn > 0) {
            __try {
                for (unsigned long i = 0; i < hn; ++i) {
                    g_kitTpl[humanTeam][i] = *(const unsigned long*)((const unsigned char*)hBegin + i * 4);
                }
                g_kitTplN[humanTeam] = hn;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    // 2) Источник для бота: шаблон его команды, иначе шаблон человека.
    unsigned int src = (botFaction == 1 || botFaction == 2) && g_kitTplN[botFaction] > 0 ? botFaction : humanTeam;
    if (src > 2 || g_kitTplN[src] == 0) {
        LOG_INFO("Bridge: набор: шаблона нет (бот %p команда %u)", (void*)bot, botTeam);
        return;
    }
    // ★ Сборка 201: шаблон ЧУЖОЙ фракции не подставляем. Факт 19.10 16:13:42 (XP5_002 AirSuperiority): бот команды 2 получил набор
    // команды 1 → AV в spawnSoldier (EIP 0x004904F0, чтение 0x5F345058 = ASCII «XP4_» как указатель); та же строка «ЧУЖОЙ» — за 2 с
    // до падения сервера 15:41:08 (битый призрак в StreamManagerGhost). Свой набор бота остаётся как есть (выбор мода/движка).
    if (src != botFaction) {
        static volatile LONG s_foreignLog = 0;
        if (InterlockedIncrement(&s_foreignLog) <= 20) {
            LOG_INFO("Bridge: набор: бот %p команда %u — шаблона своей фракции нет, чужой (%u) не подставляю", (void*)bot, botTeam, src);
        }
        return;
    }
    const unsigned long n = g_kitTplN[src];
    const unsigned long botInline = bot + (unsigned long)kOffKitInline;
    if (bBegin != botInline || n == 0 || n > 16) {
        LOG_INFO("Bridge: набор: пропуск (n=%lu, бот begin=%08lX ожидался %08lX)", n, bBegin, botInline);
        return;
    }
    __try {
        for (unsigned long i = 0; i < n; ++i) {
            *(unsigned long*)((unsigned char*)botInline + i * 4) = g_kitTpl[src][i];
        }
        *(unsigned long*)((unsigned char*)bot + kOffKitEnd) = botInline + n * 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: набор: запись в бота %p не удалась", (void*)bot);
        return;
    }
    LOG_INFO("Bridge: набор: бот %p команда %u <- шаблон команды %u (элементов=%lu, cap=%08lX)%s",
             (void*)bot, botTeam, src, n, bCap, src == botFaction ? "" : " ЧУЖОЙ");
}

// ★ Регистрация бота в PointSystem (13.10): движок делает это человеку на
// ServerPlayerCreateMessage (0x131F010, thiscall this=PointSystem, 1 стековый).
// Без неё смерть бота даёт «No player info found for victim», клиент пишет человеку
// «Самоубийство», а таб/ники/строка убийства не работают. Один раз на бота.
constexpr unsigned long kPointsRegRvaBridge = 0x00F1F010UL;
static unsigned long g_pointsDone[64];
static LONG g_pointsDoneCount = 0;
static int EnginePlayerId(unsigned long player);

// ★ Сборка 196: запись PlayerPointsInfo игрока уже есть в движке ([ps+8][id] != 0). registerPlayer 0x0131F010 на занятом
// слоте пишет «PlayerPointsInfo already created» и ЗАТИРАЕТ запись новой — без объекта статистики [info+4] (декомпиляция 19.10).
// Движок регистрирует игрока один раз (ServerPlayerCreateMessage), снимает — на ServerPlayerDestroyMessage (0x0131AE80).
static bool EnginePointsInfoExists(unsigned long ps, unsigned long player) {
    const int id = EnginePlayerId(player);
    if (ps == 0 || id < 0) return false;
    __try {
        const unsigned long beg = *(const unsigned long*)(ULONGLONG)(ps + 8);
        const unsigned long end = *(const unsigned long*)(ULONGLONG)(ps + 0xC);
        if (beg == 0 || end <= beg || (unsigned long)id >= (end - beg) / 4) return false;
        return *(const unsigned long*)(ULONGLONG)(beg + (unsigned long)id * 4) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RegisterBotPoints(unsigned long bot) {
    unsigned char* base = NativeModuleBase();
    const unsigned long ps = ::evbots::EVBotsEnginePointSystem();
    if (base == nullptr || bot == 0) return;
    for (LONG i = 0; i < g_pointsDoneCount; ++i) {
        if (g_pointsDone[i] == bot) return;
    }
    if (ps == 0) {
        LOG_INFO("Bridge: PointSystem ещё не известен — бот %p не зарегистрирован в очках", (void*)bot);
        return;
    }
    // Прогон 195: после «забыть» (смена режима) бот снова регистрировался — 9 ботов SQDM и 19 на MP_007 потеряли статистику
    // (0/0/0 в таблице). Запись в движке есть — регистрировать не надо, как у VU.
    if (EnginePointsInfoExists(ps, bot)) {
        if (g_pointsDoneCount < 64) g_pointsDone[g_pointsDoneCount++] = bot;
        LOG_INFO("Bridge: бот %p уже есть в PointSystem %08lX — повторная регистрация пропущена (статистика сохранена)",
                 (void*)bot, ps);
        return;
    }
    using RegFn = void(__fastcall*)(void*, void*, void*);
    __try {
        ((RegFn)((ULONGLONG)base + kPointsRegRvaBridge))((void*)(ULONGLONG)ps, nullptr,
                                                          (void*)(ULONGLONG)bot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: PointSystem::registerPlayer(бот %p) — исключение в движке", (void*)bot);
        return;
    }
    if (g_pointsDoneCount < 64) g_pointsDone[g_pointsDoneCount++] = bot;
    LOG_INFO("Bridge: бот %p зарегистрирован в PointSystem %08lX", (void*)bot, ps);
}

// ★ Убить игрока движком (13.10, разбор 0x1186DF4 и обработчика 0xDE1F94):
// ServerPlayerInstantSuicideMessage (хеш 0x83EE48C7). Движок делает ровно так
// при смене команды: конструктор 0x22FA340 (thiscall, this=буфер), +0x28 = игрок,
// +0x30 = команда, рассылка 0x549A20 (thiscall на [0x2B3EE2C], 1 стековый, RET 4),
// деструктор 0x5306A0. Обработчик убивает солдата игрока (нужно +0x318 == 0).
// Это настоящий SoldierEntity:Kill() для VU-API вместо заглушки.
constexpr unsigned long kSuicideMsgCtorRva = 0x01EFA340UL;  // VA 0x22FA340
constexpr unsigned long kMsgDispatchRva = 0x00149A20UL;     // VA 0x549A20
constexpr unsigned long kMsgDtorRva = 0x001306A0UL;         // VA 0x5306A0
constexpr unsigned long kMsgManagerRva = 0x0273EE2CUL;      // VA 0x2B3EE2C

// ★ Настоящее убийство (сборка 87, прогон 85). InstantSuicide ботов не убивает, поэтому лишний бот, которого мод
// «убивал» при перебалансировке команд, жил дальше («нас 17»). RCON `admin.killPlayer` (обработчик 0x0113A640)
// делает так: солдат = [игрок+0x3C4]; ServerSoldierEntity::kill(1) — VA 0x00E01660, thiscall на солдата, 1 арг.,
// ret 4; внутри: компонент здоровья [солдат+0x538], урон = текущее здоровье. Это и есть VU SoldierEntity:Kill().
// Вызов — в хуке тика (как спавн): из Lua-контекста кладём в очередь.
constexpr unsigned long kSoldierKillRva = 0x00A01660UL;  // VA 0x00E01660
constexpr LONG kKillQueueCap = 64;  // сборка 170: было 32 — KillAll при 64 игроках терял лишних молча
static unsigned long g_killQueue[kKillQueueCap];
static LONG g_killCount = 0;

// ★ Сборка 236: падение 00E0B030 (234-в, 234-г, 235) — обход списка источников трансформа солдата [солдат+0x394]
// («bubblestack», соседняя 0x00E0D7B0: «max depth on bubblestack reached»). Голова-страж — +0x394+0xC (первый узел),
// +0x10 (последний); узел: [0] следующий, [1] предыдущий, +0xC ключ, +0x10 приоритет (декомпиляция 0x00E0B030/0x00E0D7B0).
// В 235 у двух солдат узел со ссылкой 0 (EAX=0, VEH #1/#2), в 234-в — 1.0f вместо указателя. Кто портит — не найдено.
// Проверяем целостность кольца: 0 — цело, иначе номер узла, где сломано. Kill по испорченному солдату не зовём —
// движок упал бы при высадке (kill → exitControllable → 0x00E1EA20 → 0x00E0BE20 → 0x00E0B030).
constexpr unsigned long kOffSoldierXformStack = 0x394;
static int SoldierXformStackBad(unsigned long soldier) {
    if (soldier == 0) return 0;
    __try {
        const unsigned long head = soldier + kOffSoldierXformStack + 0xC;
        unsigned long prev = head;
        unsigned long n = *(const unsigned long*)(ULONGLONG)head;
        for (int i = 1; i <= 64; ++i) {
            if (n == 0) return i;
            if (*(const unsigned long*)(ULONGLONG)(n + 4) != prev) return i;
            if (n == head) return 0;
            prev = n;
            n = *(const unsigned long*)(ULONGLONG)n;
        }
        return 65;  // длиннее 64 — кольцо, скорее всего, разомкнуто
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 66;
    }
}
static LONG g_xformBadLogged = 0;
static unsigned long g_xformBadSeen[64];
static LONG g_xformBadSeenCount = 0;

// ★ 238-6: какой шаг портит список трансформа солдата (падение 00E0B030 в 234-г, 235, 236, 238-5). Проверка раз в 2 с
// (XformStackTick) шага не показывала: в прогоне 238-5 22:59 у BOT_Isaac за 2 с были спавн, addToTeam, посадка и выход.
// Зовётся до и после каждого нашего вызова движка с солдатом бота (спавн, addToTeam, setTransform, посадка, выход):
// «до» — порча случилась между нашими вызовами (движок или мод), «после» — её дал этот шаг. Первая порча солдата — ERR.
// Поведение не меняет.
static void XformCheckStep(unsigned long player, const char* step, bool after) {
    unsigned long sol = 0;
    __try {
        sol = *(const unsigned long*)((const unsigned char*)(ULONG_PTR)player + kOffPlayerSoldier);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const int bad = SoldierXformStackBad(sol);
    if (bad == 0) return;
    for (LONG k = 0; k < g_xformBadSeenCount; ++k) {
        if (g_xformBadSeen[k] == sol) return;
    }
    if (g_xformBadSeenCount < 64) g_xformBadSeen[g_xformBadSeenCount++] = sol;
    else g_xformBadSeen[0] = sol;
    LOG_ERR("Bridge: ★ список трансформа солдата %08lX (бот %p) испорчен %s шага «%s» (код %d)", sol,
            (void*)(ULONG_PTR)player, after ? "сразу после" : "уже до", step, bad);
}

bool KillSoldierNow(unsigned long player) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || player == 0) return false;
    unsigned long soldier = 0, health = 0;
    __try {
        soldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)player + kOffPlayerSoldier);
        if (soldier != 0) health = *(const unsigned long*)((const unsigned char*)(ULONGLONG)soldier + 0x538);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (soldier == 0 || health == 0) return false;  // уже мёртв / нет компонента здоровья
    if (const int bad = SoldierXformStackBad(soldier)) {  // сборка 236
        if (g_xformBadLogged < 200) {
            ++g_xformBadLogged;
            unsigned long att = 0;
            __try { att = *(const unsigned long*)((const unsigned char*)(ULONGLONG)player + 0x3D8); } __except (EXCEPTION_EXECUTE_HANDLER) { att = 0; }
            LOG_ERR("Bridge: kill(бот %p, солдат %08lX) НЕ вызван — список трансформа солдата испорчен (код %d), техника %08lX",
                    (void*)(ULONGLONG)player, soldier, bad, att);
        }
        return false;
    }
    using KillFn = void(__fastcall*)(void*, void*, unsigned int);
    __try {
        ((KillFn)(base + kSoldierKillRva))((void*)(ULONGLONG)soldier, nullptr, 1u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: kill(солдат бота %p) — исключение в движке", (void*)(ULONGLONG)player);
        return false;
    }
    LOG_INFO("Bridge: бот %p убит (ServerSoldierEntity::kill)", (void*)(ULONGLONG)player);
    return true;
}

bool NativeKillPlayer(unsigned long player) {
    if (player == 0) return false;
    if (InEngineTick()) return KillSoldierNow(player);
    for (LONG i = 0; i < g_killCount; ++i) {
        if (g_killQueue[i] == player) return true;
    }
    if (g_killCount >= kKillQueueCap) return false;
    g_killQueue[g_killCount++] = player;
    return true;
}

// ═══ Сборка 171: создание бота «как VU CreatePlayer» (re\vu\SEQUENCE-REPORT.md п. 2 и 4; разбор очков — MODLOG 171) ═══
// VU после createPlayer рассылает ServerPlayerCreateMessage — на неё подписаны PointSystem (регистрация очков, 0x0131F010)
// и персистентность (запрос статистики игрока у Blaze, 0x01188D90 → StatisticsEvent → подготовка PlayerPointsInfo,
// 0x01322540), затем LevelLoaded (+0x49C, сообщение) и инициализация расширений игрока (+0x49D); EnteredLevel (+0x49E,
// на неё подписаны PointSystem и точки спавна) движок шлёт сам из ServerPlayer::update.
// Факт (живое чтение PointSystem, прогон 170, 11:18): у ботов запись PlayerPointsInfo не подготовлена (+4 = 0, нет объекта
// статистики): kills (+0x128) и deaths (+0x12C) растут, score (+0x134) = 0 — очки складываются из категорий статистики.
// Подписчики создания пропускают ИИ-игроков (+0x318 != 0): на время рассылки ставим 0, потом возвращаем.
// EVBOTS_VU_CREATE: 1 (по умолчанию) — всем, 0 — выкл., ab — только чётным созданным (опыт А/Б в одном прогоне).
constexpr unsigned long kCreateMsgCtorRva = 0x01F07610UL;  // VA 0x02307610 (push 0x3051E83C; +0x28 = игрок; 0x2C байт)
constexpr unsigned long kLevelLoadedRva = 0x00A19380UL;    // VA 0x00E19380: thiscall(игрок), без аргументов
constexpr unsigned long kExtentsInitRva = 0x00A208E0UL;    // VA 0x00E208E0: thiscall(игрок), без аргументов

static int VuCreateMode() {
    static const int m = [] {
        char v[8] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VU_CREATE", v, (DWORD)sizeof(v));
        if (n == 1 && v[0] == '0') return 0;
        if (n == 2 && (v[0] == 'a' || v[0] == 'A')) return 2;
        return 1;
    }();
    return m;
}

struct VuGroupRec { unsigned long ptr; char group; };
static VuGroupRec g_vuGroup[128];
static LONG g_vuGroupCount = 0;
static LONG g_vuCreateSeq = 0;

static void SetVuGroup(unsigned long ptr, char group) {
    for (LONG i = 0; i < g_vuGroupCount; ++i) {
        if (g_vuGroup[i].ptr == ptr) { g_vuGroup[i].group = group; return; }
    }
    if (g_vuGroupCount < 128) g_vuGroup[g_vuGroupCount++] = {ptr, group};
}

static char VuGroupOf(unsigned long ptr) {
    for (LONG i = 0; i < g_vuGroupCount; ++i) {
        if (g_vuGroup[i].ptr == ptr) return g_vuGroup[i].group;
    }
    return '-';
}

void VuFinishCreate(unsigned long player, const char* name) {
    const int mode = VuCreateMode();
    const LONG seq = InterlockedIncrement(&g_vuCreateSeq);
    const bool on = mode == 1 || (mode == 2 && (seq % 2) == 0);
    SetVuGroup(player, on ? 'A' : 'B');
    if (!on) {
        if (mode == 2) LOG_INFO("Bridge: VU-создание: \"%s\" — группа Б (без сообщения создания и «в уровне», опыт А/Б)", name);
        return;
    }
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || player == 0) return;
    unsigned char* p = (unsigned char*)(ULONGLONG)player;
    unsigned int ai = 0;
    __try {
        ai = *(const unsigned int*)(p + kOffPlayerIsAi);
        *(unsigned int*)(p + kOffPlayerIsAi) = 0u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    bool msgOk = false, lvlOk = false, extOk = false;
    __try {
        alignas(16) unsigned char msg[0x80] = {};
        using CtorFn = void*(__fastcall*)(void*, void*);
        using DispFn = void(__fastcall*)(void*, void*, void*);
        using DtorFn = void(__fastcall*)(void*, void*);
        ((CtorFn)(base + kCreateMsgCtorRva))(msg, nullptr);
        *(unsigned long*)(msg + 0x28) = player;
        void* mm = *(void* const*)(base + kMsgManagerRva);
        if (mm != nullptr) {
            ((DispFn)(base + kMsgDispatchRva))(mm, nullptr, msg);
            msgOk = true;
        }
        ((DtorFn)(base + kMsgDtorRva))(msg, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: VU-создание: ServerPlayerCreateMessage для \"%s\" — исключение в движке", name);
    }
    using ThisFn = void(__fastcall*)(void*, void*);
    // ★ Сборка 185: лицензии — как VU CreatePlayer (0x10B991E7–0x10B9957D): 70 × лицензия(extent, имя), затем лицензии
    // из конфига, и только потом LevelLoaded/расширения. Extent = игрок + [0x0285BAD8]; лицензия 0x011764E0 (thiscall,
    // ret 4: бит по индексу в [extent+0xDC]); из конфига 0x01176520 (thiscall: [extent+0xEC] = 1 и флаг репликации 0x20).
    // Сверка начала обеих функций с exe; EVBOTS_VU_LICENSES=0 — без лицензий (как до 185).
    int licOk = -1;
    {
        static const char* const kVuLicenses[] = {
            "le", "b2k", "xp2", "xp3", "xp4", "xp5", "pwp", "speca", "pepper1", "pepper2", "pepper3", "pepper4",
            "pepper5", "pepper6", "dogtags", "alienware", "razer", "tournament1", "tournament2", "tournament3", "valor",
            "ea_nfs12", "veteran", "ea_bf3m", "ea_1942", "ea_bf2", "ea_2142", "ea_bfv", "ea_bc", "ea_bc2", "ea_bfp4f",
            "dice", "pistolpack", "b2kpack", "pdwpack", "shotgunpack", "assaultpack", "supportpack", "engineerpack",
            "reconpack", "cooppack", "specpack", "camopack", "jetpack", "attackhelipack", "scouthelipack", "tankpack",
            "ifvpack", "aapack", "welcomepack", "premium", "xp2_ks", "dicecm", "pbpack", "xp3_ks", "xp3promo1",
            "xp3promo2", "xp3promo3", "ea_1943", "xp4_ks", "xp4promo1", "xp4promo2", "xp4promo3", "xp4promo4",
            "xp4promo5", "premium_xp4", "xp5_ks", "xp5_enlisted", "xp5_iwasthere", "xp5_pvz"};
        static const unsigned char kLicFn[10] = {0x56, 0x8B, 0xF1, 0xE8, 0x48, 0x6C, 0x07, 0x00, 0x85, 0xC0};
        static const unsigned char kLicCfgFn[10] = {0xA1, 0xF0, 0xD7, 0xB5, 0x02, 0x81, 0xEC, 0x88, 0x02, 0x00};
        char lv[4] = {};
        const DWORD ln = ::GetEnvironmentVariableA("EVBOTS_VU_LICENSES", lv, (DWORD)sizeof(lv));
        if (!(ln == 1 && lv[0] == '0')) {
            using LicFn = void(__fastcall*)(void*, void*, const char*);
            __try {
                const unsigned long off = *(const unsigned long*)(base + 0x0245BAD8UL);
                unsigned char* ext = p + off;
                if (std::memcmp(base + 0x00D764E0UL, kLicFn, sizeof(kLicFn)) == 0 &&
                    std::memcmp(base + 0x00D76520UL, kLicCfgFn, sizeof(kLicCfgFn)) == 0 && off != 0 &&
                    *(const unsigned long*)(ext + 0xDC) != 0) {
                    licOk = 0;
                    for (const char* lic : kVuLicenses) {
                        ((LicFn)(base + 0x00D764E0UL))(ext, nullptr, lic);
                        ++licOk;
                    }
                    ((ThisFn)(base + 0x00D76520UL))(ext, nullptr);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LOG_ERR("Bridge: VU-создание: лицензии для \"%s\" — исключение в движке (выдано %d)", name, licOk);
            }
        }
    }
    __try {
        ((ThisFn)(base + kLevelLoadedRva))(p, nullptr);
        lvlOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: VU-создание: LevelLoaded для \"%s\" — исключение в движке", name);
    }
    __try {
        ((ThisFn)(base + kExtentsInitRva))(p, nullptr);
        extOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: VU-создание: расширения игрока для \"%s\" — исключение в движке", name);
    }
    unsigned b49c = 0, b49d = 0;
    __try {
        *(unsigned int*)(p + kOffPlayerIsAi) = ai;
        b49c = p[0x49C];
        b49d = p[0x49D];
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    // PointSystem зарегистрировал бота сам (на время рассылки +0x318 = 0) — свой registerPlayer не нужен.
    if (msgOk && g_pointsDoneCount < 64) {
        bool known = false;
        for (LONG i = 0; i < g_pointsDoneCount; ++i) {
            if (g_pointsDone[i] == player) { known = true; break; }
        }
        if (!known) g_pointsDone[g_pointsDoneCount++] = player;
    }
    LOG_INFO("Bridge: VU-создание: \"%s\"%s — сообщение создания %s, лицензий %d, LevelLoaded %s, расширения %s "
             "(+0x49C/D = %u/%u, ИИ %u)",
             name, mode == 2 ? " (группа А)" : "", msgOk ? "да" : "НЕТ", licOk, lvlOk ? "да" : "НЕТ", extOk ? "да" : "НЕТ",
             b49c, b49d, ai);
}

// ★ Сборка 171: счёт игрока как VU Player.score/kills/deaths (veniceext 0x1098C5F0/0x1098C410): PointSystem [ps+8][id]
// → PlayerPointsInfo, [info+0x1C] → объект счёта: +0x134 очки, +0x128 убийства, +0x12C смерти, +0x124 звание; [info+4] —
// объект статистики (без него очки не копятся). id — vfunc +0x10 игрока (так читает и PointSystem).
static int EnginePlayerId(unsigned long player) {
    if (player == 0) return -1;
    __try {
        void** vt = *(void***)(ULONGLONG)player;
        using IdFn = int(__fastcall*)(void*, void*);
        return ((IdFn)vt[4])((void*)(ULONGLONG)player, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static bool ReadPlayerScore(unsigned long player, int out[5]) {
    const unsigned long ps = ::evbots::EVBotsEnginePointSystem();
    const int id = EnginePlayerId(player);
    if (ps == 0 || id < 0) return false;
    __try {
        const unsigned long beg = *(const unsigned long*)(ULONGLONG)(ps + 8);
        const unsigned long end = *(const unsigned long*)(ULONGLONG)(ps + 0xC);
        if (beg == 0 || end <= beg || (unsigned long)id >= (end - beg) / 4) return false;
        const unsigned long info = *(const unsigned long*)(ULONGLONG)(beg + (unsigned long)id * 4);
        if (info == 0) return false;
        const unsigned long obj = *(const unsigned long*)(ULONGLONG)(info + 0x1C);
        if (obj == 0) return false;
        out[0] = *(const int*)(ULONGLONG)(obj + 0x134);
        out[1] = *(const int*)(ULONGLONG)(obj + 0x128);
        out[2] = *(const int*)(ULONGLONG)(obj + 0x12C);
        out[3] = *(const int*)(ULONGLONG)(obj + 0x124);
        out[4] = *(const unsigned long*)(ULONGLONG)(info + 4) != 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

/// NativeBridge.PlayerScore(playerPtr) -> score, kills, deaths, rank, primed | nil
int l_bridge_player_score(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    int v[5] = {};
    if (!ReadPlayerScore(ptr, v)) {
        lua_pushnil(L);
        return 1;
    }
    for (int i = 0; i < 4; ++i) lua_pushinteger(L, v[i]);
    lua_pushboolean(L, v[4]);
    return 5;
}

// ★ Сборка 266: как VU/BF3, очки/убийства/смерти — статистика РАУНДА. Движок этот сброс сам не делает для наших
// игроков (боты — долгоживущие ServerPlayer через смену уровня, человек — тоже), и на новой карте доска показывала
// прошлые числа. Чистим +0x128/+0x12C/+0x134 у ботов и человека на Level:Loaded; ранг +0x124 — личный, не трогаем.
static bool RoundStatObject(unsigned long player, unsigned long* out) {
    const unsigned long ps = ::evbots::EVBotsEnginePointSystem();
    const int id = EnginePlayerId(player);
    if (ps == 0 || id < 0) return false;
    __try {
        const unsigned long beg = *(const unsigned long*)(ULONGLONG)(ps + 8);
        const unsigned long end = *(const unsigned long*)(ULONGLONG)(ps + 0xC);
        if (beg == 0 || end <= beg || (unsigned long)id >= (end - beg) / 4) return false;
        const unsigned long info = *(const unsigned long*)(ULONGLONG)(beg + (unsigned long)id * 4);
        if (info == 0) return false;
        const unsigned long obj = *(const unsigned long*)(ULONGLONG)(info + 0x1C);
        if (obj == 0) return false;
        *out = obj;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ResetPlayerRoundStats(unsigned long player) {
    unsigned long obj = 0;
    if (!RoundStatObject(player, &obj)) return false;
    __try {
        *(int*)(ULONGLONG)(obj + 0x128) = 0;  // убийства
        *(int*)(ULONGLONG)(obj + 0x12C) = 0;  // смерти
        *(int*)(ULONGLONG)(obj + 0x134) = 0;  // очки
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

/// ★ 231: NativeBridge.EnginePlayerId(playerPtr) -> id движка | nil. Как VU Player.id (ServerPlayer+0x3B8, vfunc +0x10 —
/// так же читает PointSystem, сборка 171): один номер у сервера и клиента (реестр В19).
int l_bridge_engine_player_id(lua_State* L) {
    const int id = EnginePlayerId((unsigned long)luaL_checkinteger(L, 1));
    if (id < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, id);
    }
    return 1;
}

// Сводка «ДИАГ счёт» раз в ~30 с (в тике моста): боты по группам VU-создания, подготовлена ли статистика, очки/убийства.
static void ScoreDiagTick(const unsigned long* bots, LONG count) {
    static ULONGLONG s_next = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now < s_next) return;
    s_next = now + 30000ULL;
    int n[2] = {}, primed[2] = {}, score[2] = {}, kills[2] = {}, deaths[2] = {}, unread = 0;
    for (LONG i = 0; i < count && i < 64; ++i) {
        int v[5] = {};
        if (!ReadPlayerScore(bots[i], v)) { ++unread; continue; }
        const int g = VuGroupOf(bots[i]) == 'B' ? 1 : 0;
        ++n[g]; primed[g] += v[4]; score[g] += v[0]; kills[g] += v[1]; deaths[g] += v[2];
    }
    int h[5] = {-1, -1, -1, -1, -1};
    ReadPlayerScore(::evbots::EVBotsEngineRealPlayer(), h);
    LOG_INFO("Bridge: ДИАГ счёт: боты А (VU-создание) %d: статистика %d, очки %d, убийств %d, смертей %d | Б %d: статистика %d, "
             "очки %d, убийств %d, смертей %d | не прочитано %d | человек: очки %d, убийств %d, смертей %d, звание %d, статистика %d",
             n[0], primed[0], score[0], kills[0], deaths[0], n[1], primed[1], score[1], kills[1], deaths[1], unread,
             h[0], h[1], h[2], h[3], h[4]);
}

// Имя бота по указателю игрока (сборка 159; объявление поднято сюда в 175 — нужно ScoreChangeTick).
struct PtrName { unsigned long ptr; char name[32]; };
static PtrName g_ptrName[64];
static LONG g_ptrNameCount = 0;
// ★ Сборка 175: «СЧЁТ» — каждое изменение очков/убийств/смертей бота (раз в ~0,5 с). Пользователь (173): очки ботам идут,
// но не за убийства; сводка раз в 30 с этого не показывает. Видно, сколько очков пришло вместе с убийством.
static void ScoreChangeTick(const unsigned long* bots, LONG count) {
    static ULONGLONG s_next = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now < s_next) return;
    s_next = now + 500ULL;
    struct Last { unsigned long ptr; int v[3]; };
    static Last s_last[64];
    static LONG s_n = 0;
    static volatile LONG s_logged = 0;
    for (LONG i = 0; i < count && i < 64; ++i) {
        int v[5] = {};
        if (!ReadPlayerScore(bots[i], v)) continue;
        Last* l = nullptr;
        for (LONG k = 0; k < s_n; ++k) {
            if (s_last[k].ptr == bots[i]) { l = &s_last[k]; break; }
        }
        if (l == nullptr) {
            if (s_n >= 64) continue;
            l = &s_last[s_n++];
            l->ptr = bots[i];
            l->v[0] = v[0]; l->v[1] = v[1]; l->v[2] = v[2];
            continue;
        }
        if (l->v[0] == v[0] && l->v[1] == v[1] && l->v[2] == v[2]) continue;
        if (InterlockedIncrement(&s_logged) <= 400) {
            const char* nm = "?";
            for (LONG k = 0; k < g_ptrNameCount && k < 64; ++k) {
                if (g_ptrName[k].ptr == bots[i]) { nm = g_ptrName[k].name; break; }
            }
            LOG_INFO("Bridge: СЧЁТ %s (группа %c): очки %d -> %d (%+d), убийства %d -> %d, смерти %d -> %d", nm,
                     VuGroupOf(bots[i]), l->v[0], v[0], v[0] - l->v[0], l->v[1], v[1], l->v[2], v[2]);
        }
        l->v[0] = v[0]; l->v[1] = v[1]; l->v[2] = v[2];
    }
}

// Момент «человек в уровне» (+0x49C/D/E) и его команда — к задаче «~22 с до ботов» (раз в тик, лог только на изменении).
// ★ Сборка 211 (друг 22.10: «у некоторых бесконечное подключение, случайно»): вход людей глазами движка. Вектор игроков
// [[[0x02B3EE2C]+0x30]+0x80..+0x84] (как в IsUnregisteredForTelemetry); человек — с соединением [p+0x3BC] != 0; в уровне —
// [p+0x49C]/[p+0x49D]. Строка «ДИАГ вход» — только при изменении (пишется и в тихом журнале): сколько игроков в движке, сколько
// людей, у кого уровень не загружен и сколько секунд. Причину зависания по ней искать в журнале друга.
static void HumanJoinDiagTick() {
    static ULONGLONG s_next = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now < s_next) return;
    s_next = now + 1000ULL;
    struct Seen { unsigned long p; unsigned lvl; ULONGLONG since; };
    static Seen s_h[32];
    static LONG s_hn = 0;
    static LONG s_lastTotal = -1, s_lastHumans = -1;
    LONG total = 0, humans = 0;
    unsigned long hp[32] = {};
    unsigned hl[32] = {};
    __try {
        const unsigned long gs = *(const unsigned long*)(ULONGLONG)0x02B3EE2CUL;
        if (gs == 0) return;
        const unsigned long pm = *(const unsigned long*)((const unsigned char*)(ULONGLONG)gs + 0x30);
        if (pm == 0) return;
        const unsigned long* it = *(unsigned long* const*)((const unsigned char*)(ULONGLONG)pm + 0x80);
        const unsigned long* end = *(unsigned long* const*)((const unsigned char*)(ULONGLONG)pm + 0x84);
        for (int n = 0; it != nullptr && it < end && n < 256; ++it, ++n) {
            const unsigned char* p = (const unsigned char*)(ULONGLONG)*it;
            if (p == nullptr) continue;
            ++total;
            if (*(const unsigned char*)(p + 0x3BC) == 0) continue;
            if (humans < 32) {
                hp[humans] = (unsigned long)(ULONGLONG)p;
                hl[humans] = (unsigned)p[0x49C] | ((unsigned)p[0x49D] << 8);
            }
            ++humans;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (total != s_lastTotal && (humans != s_lastHumans || total >= 60)) {
        LOG_INFO("Bridge: ДИАГ вход: игроков в движке %ld (людей с соединением %ld, остальные — боты)", (long)total, (long)humans);
    }
    s_lastTotal = total;
    s_lastHumans = humans;
    Seen nh[32] = {};
    LONG nn = 0;
    for (LONG i = 0; i < humans && i < 32; ++i) {
        const Seen* old = nullptr;
        for (LONG k = 0; k < s_hn; ++k) if (s_h[k].p == hp[i]) { old = &s_h[k]; break; }
        Seen s = { hp[i], hl[i], old != nullptr ? old->since : now };
        if (old == nullptr || old->lvl != hl[i]) {
            s.since = now;
            LOG_INFO("Bridge: ДИАГ вход: человек %08lX — уровень загружен %u/%u (игроков в движке %ld)%s", hp[i], hl[i] & 0xFF,
                     hl[i] >> 8, (long)total, old == nullptr ? " — новый" : "");
        } else if ((hl[i] & 0xFF) == 0 && now - s.since >= 60000ULL && ((now - s.since) / 1000ULL) % 60ULL == 0ULL) {
            LOG_INFO("Bridge: ДИАГ вход: человек %08lX уровень не загружен уже %llu с (игроков в движке %ld)", hp[i],
                     (now - s.since) / 1000ULL, (long)total);
        }
        nh[nn++] = s;
    }
    for (LONG k = 0; k < s_hn; ++k) {
        bool still = false;
        for (LONG i = 0; i < nn; ++i) still = still || nh[i].p == s_h[k].p;
        if (!still) LOG_INFO("Bridge: ДИАГ вход: человек %08lX ушёл (игроков в движке %ld)", s_h[k].p, (long)total);
    }
    for (LONG i = 0; i < nn; ++i) s_h[i] = nh[i];
    s_hn = nn;
}

static void HumanLevelDiagTick() {
    static unsigned long s_human = 0;
    static unsigned s_last = 0xFFFFFFFFu;
    const unsigned long human = ::evbots::EVBotsEngineRealPlayer();
    if (human == 0) return;
    unsigned cur = 0;
    int team = 0;
    __try {
        const unsigned char* p = (const unsigned char*)(ULONGLONG)human;
        cur = (unsigned)p[0x49C] | ((unsigned)p[0x49D] << 8) | ((unsigned)p[0x49E] << 16);
        team = *(const int*)(p + kOffPlayerTeam);
        cur |= ((unsigned)team & 0xFFu) << 24;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (human != s_human || cur != s_last) {
        LOG_INFO("Bridge: ЧЕЛОВЕК %p: в уровне +0x49C/D/E = %u/%u/%u, команда %d", (void*)(ULONGLONG)human, cur & 0xFF,
                 (cur >> 8) & 0xFF, (cur >> 16) & 0xFF, team);
        s_human = human;
        s_last = cur;
    }
}

// Бот входит в отряд при каждом спавне — в тот, что выбрал мод (AssignBotSquad ниже).
// addToSquad(ServerPlayer* p, SquadId sq, byte fA, byte fB) — cdecl, 4 аргумента (проверено дизассемблером).
constexpr unsigned long kAddToSquadRvaAuto = 0x00D8D8D0UL;  // VA 0x0118D8D0
// ★ Счётчики прогона (проверки в харнессе): вход в отряд при спавне, позиции, спавны.
volatile LONG g_stSquadAuto = 0;
volatile LONG g_stPosApplied = 0;
volatile LONG g_stPosSkipped = 0;
volatile LONG g_stSpawnOk = 0;
volatile LONG g_stSpawnFail = 0;
volatile LONG g_stPosNoRecord = 0;  // бот спавнился без записи позиции (мод не передал координаты)
volatile LONG g_posNotReady = 0;    // ApplyBotPosition: солдат движка ещё не готов (+0x74 == 0), повторить
// ★ Отряд игрока в движке (14.10, сборка 78). Так читает его сам addToSquad (VA 0x0118D9F4):
//   mov eax,[0x0285BAD8] ; lea esi,[eax+player] ; mov edx,[esi] ; call [edx+0x20]  (thiscall, без аргументов).
// -1 — не прочитать.
constexpr unsigned long kSquadSubOffsetRva = 0x0245BAD8UL;  // VA 0x0285BAD8: смещение подобъекта отряда в игроке
int EngineSquadOf(unsigned long player) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || player == 0) return -1;
    int sq = -1;
    __try {
        const unsigned long off = *(const unsigned long*)(base + kSquadSubOffsetRva);
        void* sub = (void*)(ULONGLONG)(player + off);
        const unsigned long vt = *(const unsigned long*)sub;
        using GetSquadFn = unsigned int(__fastcall*)(void*, void*);
        const unsigned long fn = *(const unsigned long*)(ULONGLONG)(vt + 0x20);
        sq = (int)((GetSquadFn)(ULONGLONG)fn)(sub, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sq = -1;
    }
    return sq;
}

// Отряд, который выбрал мод (fun-bots пишет Player.squadId перед SpawnSoldierAt). -1 — мод не передал.
struct BotSquadRec { unsigned long ptr; int squad; };
BotSquadRec g_botSquad[64];
LONG g_botSquadCount = 0;
void RememberBotSquad(unsigned long ptr, int squad) {
    if (ptr == 0) return;
    for (LONG i = 0; i < g_botSquadCount; ++i) {
        if (g_botSquad[i].ptr == ptr) { g_botSquad[i].squad = squad; return; }
    }
    if (g_botSquadCount < 64) {
        g_botSquad[g_botSquadCount].ptr = ptr;
        g_botSquad[g_botSquadCount].squad = squad;
        ++g_botSquadCount;
    }
}
int WantedBotSquad(unsigned long ptr) {
    for (LONG i = 0; i < g_botSquadCount; ++i) {
        if (g_botSquad[i].ptr == ptr) return g_botSquad[i].squad;
    }
    return -1;
}

// ★ Как в VU (сборка 78): бот встаёт в отряд, который выбрал мод, на каждом спавне. Раньше мост выбирал
// сам (сначала отряд человека, потом 1..4) — боты шли за человеком, а мод видел другие отряды.
// SquadNone (0) — без отряда, как решил мод. Отряд уже тот — не трогаем (addToSquad «Already member»).
static bool EngineTeamInList(unsigned long player, unsigned int* teamOut, unsigned int* teamsOut);

void AssignBotSquad(unsigned long ptr) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0) return;
    const int want = WantedBotSquad(ptr);
    if (want < 1 || want > 32) {
        LOG_INFO("Bridge: бот %p без отряда (мод выбрал %d)", (void*)ptr, want);
        return;
    }
    unsigned int team = 0, teams = 0;
    if (!EngineTeamInList(ptr, &team, &teams)) {  // сборка 196: как «Invalid team» в joinSquad 0x01189D60
        LOG_WARN("Bridge: бот %p — отряд %d не ставлю: команда %u вне списка (команд %u)", (void*)ptr, want, team, teams);
        return;
    }
    if (EngineSquadOf(ptr) == want) {
        InterlockedIncrement(&g_stSquadAuto);
        return;
    }
    using SquadFn = unsigned char(__cdecl*)(void*, unsigned int, unsigned int, unsigned int);
    unsigned int r = 0;
    __try {
        r = (unsigned int)((SquadFn)(base + kAddToSquadRvaAuto))((void*)(ULONGLONG)ptr, (unsigned int)want, 0u, 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: addToSquad(бот %p, отряд %d) — исключение в движке", (void*)ptr, want);
        return;
    }
    if ((r & 0xFFu) == 1u) {
        InterlockedIncrement(&g_stSquadAuto);
        LOG_INFO("Bridge: бот %p вошёл в отряд %d (выбор мода)", (void*)ptr, want);
    } else {
        LOG_INFO("Bridge: бот %p не вошёл в отряд %d (движок ответил %u)", (void*)ptr, want, r & 0xFFu);
    }
}

// ★ Позиция бота из трансформа мода (13.10). Все боты садились в одну точку HQ и вставали друг на друга.
// Сборка 78 (14.10): setTransform солдата (VA 0x00E0CEA0) — thiscall, ECX = ServerSoldierEntity (player+0x3C4),
// аргументы: LinearTransform* (4 строки по 16 байт, позиция в +0x30) и флаг (движок в 0x00E1F36C передаёт 1), ret 8.
// Внутри: трансформ сущности (0xE0C0B0), затем 0x008A2310 с ECX = солдат+0x160 (контроллер персонажа).
// Прогон 76 падал, потому что мы звали 0x008A2310 сами с ECX = солдат: проверка [ECX+0x74] читала чужое поле.
// Готовность: [солдат+0x1D4] (= +0x160+0x74) != 0, иначе 0x008A2310 падает в assert.
constexpr unsigned long kSoldierSetTransformRva = 0x00A0CEA0UL;  // VA 0x00E0CEA0
constexpr unsigned long kOffSoldierCharCtrl = 0x1D4;             // [солдат+0x160+0x74]
constexpr unsigned long kOffSoldierNoMove = 0x38B;              // байт != 0 — setTransform ничего не делает
struct BotPosRec { unsigned long ptr; float x; float y; float z; bool baseSpawned; };
BotPosRec g_botPos[64];
LONG g_botPosCount = 0;
void RememberBotPos(unsigned long ptr, float x, float y, float z) {
    if (ptr == 0) return;
    for (LONG i = 0; i < g_botPosCount; ++i) {
        if (g_botPos[i].ptr == ptr) {
            g_botPos[i].x = x; g_botPos[i].y = y; g_botPos[i].z = z;
            g_botPos[i].baseSpawned = false;
            return;
        }
    }
    if (g_botPosCount < 64) {
        g_botPos[g_botPosCount].ptr = ptr;
        g_botPos[g_botPosCount].x = x;
        g_botPos[g_botPosCount].y = y;
        g_botPos[g_botPosCount].z = z;
        g_botPos[g_botPosCount].baseSpawned = false;
        ++g_botPosCount;
    }
}
// Метка только успешного перенаправленного спавна; Lua забирает её после чтения живой позиции тела.
int l_bridge_take_base_spawn(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    bool spawned = false;
    for (LONG i = 0; i < g_botPosCount; ++i) {
        if (g_botPos[i].ptr != ptr) continue;
        spawned = g_botPos[i].baseSpawned;
        g_botPos[i].baseSpawned = false;
        break;
    }
    lua_pushboolean(L, spawned ? 1 : 0);
    return 1;
}
// ★ Сборка 81 (прогон 80). Две беды, которые показал прогон:
//  1) стакание: у первой пачки позиций не было вовсе (ClearBotPos при смене команды), и все 15 сели в одну точку HQ;
//     мод тоже иногда даёт одной и той же точки нескольким ботам;
//  2) смерть через 10,2 с у бота, посаженного в 25–50 м от HQ (таймер «вернитесь в зону боя»; выжившие стояли
//     в пределах ~10 м от HQ). Мод берёт узлы путей, которые для Conquest Small вне зоны боя.
// Поэтому: база — текущая позиция тела (там его поставил движок, это гарантированно внутри зоны),
// позиция мода принимается, только если она не дальше kMaxSpawnDrift от неё, и всегда разводится спиралью.
constexpr float kMaxSpawnDrift = 18.0f;  // метров по горизонтали от точки HQ
struct PlacedRec { float x, z; ULONGLONG t; };
PlacedRec g_placed[96];
LONG g_placedN = 0;
volatile LONG g_stPosClamped = 0;  // позиция мода отвергнута как далёкая от HQ
// ★ Сборка 147: проверка спавна на флагах (пользователь 146: «на захваченных точках не появляются»). Мод выбирает
// позицию (позже в раунде 140–295 м от базы), мост ставит туда setTransform-ом. Через 2 с читаем, где бот на самом деле:
// строка «СПАВН проверка» — просили (x, z), стоит (x, z), отклонение. Координаты флагов — в строке «ФЛАГИ» (147).
struct SpawnCheck { unsigned long ptr; float x, z; ULONGLONG t; };
static SpawnCheck g_spawnCheck[64];
static LONG g_spawnCheckCount = 0;

static bool ReadSoldierXZ(unsigned long ptr, float* x, float* z) {
    __try {
        const unsigned long soldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSoldier);
        if (soldier == 0) return false;
        const unsigned char* s = (const unsigned char*)(ULONGLONG)soldier;
        const unsigned char* tr = (const unsigned char*)(ULONGLONG)*(const unsigned long*)(s + 0x1C);
        if (((*(const unsigned long*)(s + 8) >> 18) & 1u) != 0) tr = tr + 0x10 + (unsigned long)tr[6] * 0x50;
        *x = *(const float*)(tr + 0x30);
        *z = *(const float*)(tr + 0x38);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SpawnCheckTick() {
    const ULONGLONG now = ::GetTickCount64();
    for (LONG i = 0; i < g_spawnCheckCount;) {
        if (now - g_spawnCheck[i].t < 2000ULL) { ++i; continue; }
        const SpawnCheck c = g_spawnCheck[i];
        g_spawnCheck[i] = g_spawnCheck[g_spawnCheckCount - 1];
        --g_spawnCheckCount;
        float ax = 0.0f, az = 0.0f;
        if (!ReadSoldierXZ(c.ptr, &ax, &az)) {
            LOG_INFO("Bridge: СПАВН проверка бот %p: через 2 с солдата нет (просили %.0f, %.0f)", (void*)(ULONGLONG)c.ptr,
                     (double)c.x, (double)c.z);
            continue;
        }
        const float dx = ax - c.x, dz = az - c.z;
        LOG_INFO("Bridge: СПАВН проверка бот %p: просили (%.0f, %.0f), через 2 с стоит (%.0f, %.0f), отклонение %.0f м",
                 (void*)(ULONGLONG)c.ptr, (double)c.x, (double)c.z, (double)ax, (double)az,
                 (double)::sqrtf(dx * dx + dz * dz));
    }
}

// ★ Сборка 236 (прогон 236): все 4 испорченных списка трансформа солдата [+0x394] — посадка в технику после нашего
// setTransform 0x00E0CEA0 (в том же тике спавна или после телепорта мода); посадки без него (14) и через ≥ 1 с (115) целы.
// Механизм не доказан (гипотеза). Отметка «когда мост последний раз ставил позицию солдату» — для посадки (VehicleTick
// не сажает в том же тике) и для журнала посадок.
struct XformMark { unsigned long ptr; ULONGLONG at; LONG tick; };
static XformMark g_xformMark[64];
static LONG g_xformMarkCount = 0;
static volatile LONG g_bridgeTickNo = 0;  // номер тика моста (основной хук тика)
static void MarkBotXform(unsigned long ptr) {
    LONG idx = -1;
    for (LONG i = 0; i < g_xformMarkCount; ++i) {
        if (g_xformMark[i].ptr == ptr) { idx = i; break; }
    }
    if (idx < 0) {
        if (g_xformMarkCount < 64) idx = g_xformMarkCount++;
        else idx = (LONG)(::GetTickCount64() % 64ULL);  // полон — затираем любую (только диагностика и отсрочка на тик)
    }
    g_xformMark[idx].ptr = ptr;
    g_xformMark[idx].at = ::GetTickCount64();
    g_xformMark[idx].tick = InterlockedCompareExchange(&g_bridgeTickNo, 0, 0);
}
// true — мост ставил позицию этому игроку; *agoMs — сколько мс назад, *sameTick — в этом же тике.
static bool BotXformMark(unsigned long ptr, ULONGLONG* agoMs, bool* sameTick) {
    for (LONG i = 0; i < g_xformMarkCount; ++i) {
        if (g_xformMark[i].ptr != ptr) continue;
        *agoMs = ::GetTickCount64() - g_xformMark[i].at;
        *sameTick = g_xformMark[i].tick == InterlockedCompareExchange(&g_bridgeTickNo, 0, 0);
        return true;
    }
    return false;
}
static bool HasQueuedEnter(unsigned long ptr);  // сборка 236, определение — у очереди техники
static bool PlayerInVehicle(unsigned long ptr) {
    __try {
        return *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3D8) != 0;  // attached (re\vehicle\NOTES.md)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyBotPosition(unsigned long ptr) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0) return;
    LONG rec = -1;
    for (LONG i = 0; i < g_botPosCount; ++i) {
        if (g_botPos[i].ptr == ptr) { rec = i; break; }
    }
    unsigned long soldier = 0, ctrl = 0;
    unsigned char noMove = 0;
    alignas(16) float t[16] = {};
    __try {
        soldier = *(const unsigned long*)((const unsigned char*)ptr + kOffPlayerSoldier);
        if (soldier != 0) {
            const unsigned char* s = (const unsigned char*)(ULONGLONG)soldier;
            ctrl = *(const unsigned long*)(s + kOffSoldierCharCtrl);
            noMove = *(s + kOffSoldierNoMove);
            // Трансформ сущности — как его читает сам движок (0x00E0C29C..0x00E0C2F6):
            // eax = [s+0x1C]; если бит 18 в [s+8] — eax += 0x10 + byte[eax+6]*0x50; 4 строки по 16 байт.
            const unsigned char* tr = (const unsigned char*)(ULONGLONG)*(const unsigned long*)(s + 0x1C);
            if (((*(const unsigned long*)(s + 8) >> 18) & 1u) != 0) {
                tr = tr + 0x10 + (unsigned long)tr[6] * 0x50;
            }
            ::memcpy(t, tr, 0x40);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (soldier == 0 || ctrl == 0 || noMove != 0) {
        // Солдат ещё не готов (контроллер персонажа не создан): не пропускаем, а повторим на тиках хука.
        g_posNotReady = 1;
        return;
    }
    // t[12..14] сейчас — позиция, где движок посадил тело (точка базы его команды).
    const float hqX = t[12], hqZ = t[14];
    float x = hqX, y = t[13], z = hqZ;
    bool fromMod = false;
    // Сборка 88: КАК В VU — SpawnSoldierAt(transform) ставит ровно в точку мода. Лимит 18 м и разведение
    // (сборки 81–86) были моей выдумкой против смертей через 10 с; причиной оказалась смена сторон на новом
    // раунде (подавлена в 85). Расстояние до точки движка только пишем в лог — для проверки по фактам.
    if (rec >= 0) {
        x = g_botPos[rec].x; y = g_botPos[rec].y; z = g_botPos[rec].z;
        fromMod = true;
        const float dx = x - hqX, dz = z - hqZ;
        if (dx * dx + dz * dz > kMaxSpawnDrift * kMaxSpawnDrift) InterlockedIncrement(&g_stPosClamped);
    } else {
        InterlockedIncrement(&g_stPosNoRecord);
    }
    // Сборка 86 (прогон 85): движок САМ разводит ботов по точкам своей базы (у каждого своя позиция, до 50 м
    // друг от друга, места свободные). Кольцо 84 (сдвиг на ~4 м от такой точки) загоняло ботов в стены.
    // Поэтому без годной позиции мода бота не трогаем вовсе — он стоит там, где его посадил движок.
    if (!fromMod) {
        LOG_INFO("Bridge: бот %p оставлен где посадил движок (%.1f, %.1f, %.1f) [%s]", (void*)ptr,
                 (double)hqX, (double)t[13], (double)hqZ, rec >= 0 ? "мод далеко от HQ" : "без записи");
        return;
    }
    // Позиция мода — ставим ровно туда (без разведения: в VU его нет; боты расходятся сами, когда ходят).
    t[12] = x; t[13] = y; t[14] = z; t[15] = 1.0f;
    using SetTransformFn = void(__fastcall*)(void*, void*, void*, unsigned int);
    XformCheckStep(ptr, "setTransform моста", false);  // 238-6
    __try {
        ((SetTransformFn)(base + kSoldierSetTransformRva))((void*)(ULONGLONG)soldier, nullptr, (void*)t, 1u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: позиция бота %p — исключение в движке", (void*)ptr);
        XformCheckStep(ptr, "setTransform моста (исключение движка)", true);
        return;
    }
    XformCheckStep(ptr, "setTransform моста", true);
    MarkBotXform(ptr);  // сборка 236
    InterlockedIncrement(&g_stPosApplied);
    if (g_spawnCheckCount < 64) {  // сборка 147
        g_spawnCheck[g_spawnCheckCount].ptr = ptr;
        g_spawnCheck[g_spawnCheckCount].x = x;
        g_spawnCheck[g_spawnCheckCount].z = z;
        g_spawnCheck[g_spawnCheckCount].t = ::GetTickCount64();
        ++g_spawnCheckCount;
    }
    LOG_INFO("Bridge: бот %p поставлен в (%.1f, %.1f, %.1f) [%s, HQ %.1f %.1f]", (void*)ptr,
             (double)x, (double)y, (double)z,
             fromMod ? "позиция мода" : "без записи",
             (double)hqX, (double)hqZ);
}

// (Сборка 168: MoveBot / ApplyBotMoves — ходьба скольжением, опыт 101–102, отвергнут — удалены.)

// addToSquad(бот, отряд, 0, 0) одним вызовом. Возврат: 1 — вошёл, 0 — полон, -1 — ошибка.
int CallAddToSquad(unsigned long ptr, unsigned int sq) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0) return -1;
    using SquadFn = unsigned char(__cdecl*)(void*, unsigned int, unsigned int, unsigned int);
    unsigned int r = 0;
    __try {
        r = (unsigned int)((SquadFn)(base + kAddToSquadRvaAuto))((void*)(ULONGLONG)ptr, sq, 0u, 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return (int)(r & 0xFFu);
}

// ═══ Диагностический пакет 13.10: одна проверка — несколько гипотез сразу ═══
// DIFF: поля, у которых в эталоне есть значение, а у проверяемого объекта 0. Печатаем до maxShown штук.
void DumpDiffWords(const char* tag, unsigned long refObj, unsigned long badObj,
                   unsigned long span, int maxShown) {
    if (refObj == 0 || badObj == 0) return;
    int shown = 0;
    for (unsigned long off = 0; off < span && shown < maxShown; off += 4) {
        unsigned long a = 0, b = 0;
        __try {
            a = *(const unsigned long*)((const unsigned char*)(ULONGLONG)refObj + off);
            b = *(const unsigned long*)((const unsigned char*)(ULONGLONG)badObj + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (a != 0 && b == 0) {
            ++shown;
            LOG_INFO("DIFF[%s] +0x%04lX: эталон=%08lX, у проверяемого 0", tag, off, a);
        }
    }
    LOG_INFO("DIFF[%s]: показано %d полей", tag, shown);
}

// LINK: ссылается ли солдат на игрока (имя над головой может браться через эту связь).
void ScanSoldierLink(const char* tag, unsigned long playerPtr, unsigned long soldierPtr) {
    if (playerPtr == 0 || soldierPtr == 0) return;
    int found = 0;
    for (unsigned long off = 0; off < 0x800; off += 4) {
        unsigned long v = 0;
        __try {
            v = *(const unsigned long*)((const unsigned char*)(ULONGLONG)soldierPtr + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v == playerPtr) {
            ++found;
            LOG_INFO("LINK[%s] солдат+0x%04lX указывает на игрока", tag, off);
        }
    }
    LOG_INFO("LINK[%s]: ссылок солдата на игрока=%d", tag, found);
}

// Диагностика падения spawnSoldier (13.10): фильтр исключения запоминает адрес инструкции движка
// и адрес доступа. VEH-лог печатает только первые 12 исключений, поэтому здесь свой фильтр.
unsigned long g_spawnFaultEip = 0;
unsigned long g_spawnFaultAddr = 0;
unsigned long g_spawnFaultCode = 0;
int RecordSpawnFault(EXCEPTION_POINTERS* ep) {
    g_spawnFaultEip = (unsigned long)ep->ContextRecord->Eip;
    g_spawnFaultCode = (unsigned long)ep->ExceptionRecord->ExceptionCode;
    g_spawnFaultAddr = (ep->ExceptionRecord->NumberParameters >= 2)
        ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0UL;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Постановка позиций. Прогон 76 падал (вызов 0x008A2310 с неверным ECX, см. ApplyBotPosition), в 77 была выключена.
// Сборка 78: через setTransform солдата — ВКЛЮЧЕНА по умолчанию, выключить: EVBOTS_BOT_POS=0.
bool BotPosEnabled() {
    static const bool enabled = [] {
        char buf[8] = {};
        return !(GetEnvironmentVariableA("EVBOTS_BOT_POS", buf, sizeof buf) > 0 && buf[0] == '0');
    }();
    return enabled;
}

// Позиции, которые не удалось поставить сразу (солдат ещё не готов): повторяем на тиках хука до 5 с.
struct PendingPosRec { unsigned long ptr; ULONGLONG deadline; };
PendingPosRec g_posPending[64];
LONG g_posPendingCount = 0;

void QueueBotPosition(unsigned long ptr) {
    for (LONG i = 0; i < g_posPendingCount; ++i) {
        if (g_posPending[i].ptr == ptr) return;
    }
    if (g_posPendingCount < 64) {
        g_posPending[g_posPendingCount].ptr = ptr;
        g_posPending[g_posPendingCount].deadline = GetTickCount64() + 5000ULL;
        ++g_posPendingCount;
    }
}

// Ставит позицию, если запись есть; если солдат не готов — в очередь повтора.
void ApplyBotPositionOrQueue(unsigned long ptr) {
    if (!BotPosEnabled()) return;
    // Записи может не быть (мод ещё не звал SpawnSoldierAt или команда сменилась): ApplyBotPosition тогда
    // разводит бота вокруг точки, где движок посадил тело, — стакания у HQ не будет.
    g_posNotReady = 0;
    ApplyBotPosition(ptr);
    if (g_posNotReady != 0) QueueBotPosition(ptr);
}

// Вызывается из хука тика (контекст движка есть): повтор постановки позиций из очереди.
void RetryBotPositions() {
    if (g_posPendingCount == 0 || !BotPosEnabled()) return;
    if (LevelGateClosed()) return;
    const ULONGLONG now = GetTickCount64();
    LONG w = 0;
    for (LONG i = 0; i < g_posPendingCount; ++i) {
        const unsigned long ptr = g_posPending[i].ptr;
        // ★ Сборка 236: телепорт (SoldierEntity:SetTransform мода) боту в технике или с посадкой в очереди не ставим —
        // setTransform перед посадкой портил список трансформа солдата (прогон 236, гипотеза). Запрос снимаем.
        if (PlayerInVehicle(ptr) || HasQueuedEnter(ptr)) {
            static volatile LONG s_skipLog = 0;
            if (InterlockedIncrement(&s_skipLog) <= 60) {
                LOG_INFO("Bridge: позиция бота %p не ставится — %s (setTransform перед посадкой, сборка 236)", (void*)ptr,
                         PlayerInVehicle(ptr) ? "бот в технике" : "посадка в технику в очереди");
            }
            continue;
        }
        g_posNotReady = 0;
        ApplyBotPosition(ptr);
        if (g_posNotReady == 0) continue;  // поставлена, или нет записи, или отказ движка: из очереди
        if (now < g_posPending[i].deadline) {
            g_posPending[w++] = g_posPending[i];
            continue;
        }
        InterlockedIncrement(&g_stPosSkipped);
        LOG_INFO("Bridge: позиция бота %p не применена за 5 с: солдат не готов", (void*)ptr);
    }
    g_posPendingCount = w;
}

// ★ Класс бота (сборка 83). У ботов `+0x90` (выбранный кит игрока) был 0 — отсюда «не понятно, за кого играют»
// и нет иконки класса. Факты (живая память сервера 14.10, re\selectunlock_decomp.txt):
//   * `+0x90` у человека — указатель на ассет `Gameplay/Kits/<US|RU><Assault|Engineer|Support|Recon>`;
//   * ассеты всех 8 китов живые в памяти: у объекта `+0x08` указатель на строку имени (объект = ссылка − 8);
//   * записывает кит `ServerPlayer::setSelectedCustomization` VA 0x008C2660, thiscall, 1 арг., ret 4:
//     старый release, `[this+0x90] = new`, `lock xadd [new+4], 1` (правильный счётчик ссылок).
// Класс берём у мода (имя кита в `Player:SelectUnlockAssets`), фракцию — по КОМАНДЕ ДВИЖКА (1 = США, 2 = РФ).
constexpr unsigned long kSetSelectedKitRva = 0x004C2660UL;  // VA 0x008C2660
constexpr int kOffPlayerSelectedKit = 0x90;
static unsigned long g_kitAsset[2][4];  // [0 = US, 1 = RU][Assault, Engineer, Support, Recon]
static bool g_kitScanned = false;
struct BotClassRec { unsigned long ptr; int cls; };
static BotClassRec g_botClass[64];
static LONG g_botClassCount = 0;

static int ClassFromKitName(const char* s) {
    if (s == nullptr) return -1;
    if (::strstr(s, "Assault") != nullptr) return 0;
    if (::strstr(s, "Engineer") != nullptr || ::strstr(s, "Engi") != nullptr) return 1;
    if (::strstr(s, "Support") != nullptr) return 2;
    if (::strstr(s, "Recon") != nullptr) return 3;
    return -1;
}

// Один раз найти все 8 китов: строки имён в памяти -> указатель на строку в окне перед ней (+0x08 объекта).
static void ScanKitsOnce() {
    if (g_kitScanned) return;
    g_kitScanned = true;
    static const char* const kNames[2][4] = {
        {"Gameplay/Kits/USAssault", "Gameplay/Kits/USEngineer", "Gameplay/Kits/USSupport", "Gameplay/Kits/USRecon"},
        {"Gameplay/Kits/RUAssault", "Gameplay/Kits/RUEngineer", "Gameplay/Kits/RUSupport", "Gameplay/Kits/RURecon"}};
    // ★ Сборка 164: киты — из базы данных движка (как в VU). Сборка 169: запасной скан всего адресного пространства
    // (281–593 мс на главном потоке, 163b) удалён — база сверена 8/8 во всех прогонах 164–168.
    // ★ Сборка 252: на картах XP4 ванильных китов в памяти нет — там Gameplay/Kits/<кит>_XP4 (и _GM, _GM_XP4, _XP4_SCV).
    // Порядок суффиксов — как у мода (BotSpawner:_FindKit): первый найденный. Друг 29.10 (251): «кит класса N … не найден»
    // перед каждым падением на XP4_Parl/XP4_Quake/XP4_FD.
    static const char* const kSuffix[] = {"", "_GM", "_GM_XP4", "_XP4", "_XP4_SCV"};
    int nat = 0;
    for (int f = 0; f < 2; ++f) {
        for (int c = 0; c < 4; ++c) {
            g_kitAsset[f][c] = 0;
            for (size_t s = 0; s < sizeof(kSuffix) / sizeof(kSuffix[0]) && g_kitAsset[f][c] == 0; ++s) {
                char nm[64];
                ::_snprintf_s(nm, sizeof(nm), _TRUNCATE, "%s%s", kNames[f][c], kSuffix[s]);
                g_kitAsset[f][c] = evbots::LiveFindAssetByName(nm);
            }
            if (g_kitAsset[f][c] != 0) ++nat;
        }
    }
    LOG_INFO("Bridge: киты из базы движка: %d из 8 (US A/E/S/R=%08lX %08lX %08lX %08lX, RU=%08lX %08lX %08lX %08lX)", nat,
             g_kitAsset[0][0], g_kitAsset[0][1], g_kitAsset[0][2], g_kitAsset[0][3], g_kitAsset[1][0], g_kitAsset[1][1],
             g_kitAsset[1][2], g_kitAsset[1][3]);
}

static void RememberBotClass(unsigned long ptr, int cls) {
    if (ptr == 0 || cls < 0) return;
    for (LONG i = 0; i < g_botClassCount; ++i) {
        if (g_botClass[i].ptr == ptr) { g_botClass[i].cls = cls; return; }
    }
    if (g_botClassCount < 64) {
        g_botClass[g_botClassCount].ptr = ptr;
        g_botClass[g_botClassCount].cls = cls;
        ++g_botClassCount;
    }
}

// ★ Сборка 159: ВЫБОР МОДА ПО ИМЕНИ (EVBOTS_CHOICE_BY_NAME; по умолчанию вкл., =0 — как до 159).
// Причина в логе прогона 158: класс, внешность и перки мост получал только у привязанных ботов (nativePtr), а привязка
// шла позже тел. Тела садились по таймауту 30 с без выбора: одинаковые модели и без перков до первой смерти.
// Теперь Lua передаёт выбор по имени сразу (SetChoiceByName), мост связывает имя с указателем при создании (RememberPtrName).
// (PtrName / g_ptrName — объявлены выше, у ScoreChangeTick, сборка 175)
struct ChoiceName {
    char name[32];
    int cls;           // класс кита (ClassFromKitName), -1 — нет
    char app[120];     // имя объекта внешности, "" — нет
    int perksKnown;    // список перков получен (пустой тоже — перков нет)
    char perks[4096];  // имена перков через '\n' (сборка 197: было 1024 — 26 перков техники ≈ 1200 байт обрезались)
};
// ★ Сборка 197: имена перков из ChoiceName. Раньше каждый разбор копировал в buf[1024] и шагал `p += strlen(p) + 1` без
// проверки конца: при списке ≥ 1023 байт (прогон 196: 26 перков у каждого бота) последнее имя обрезалось
// («Persistence/Unlocks/Vehicles/Mb»), а шаг уходил за буфер — мусор со стека шёл в поиск ассетов («АССЕТ «@�w…»»).
static int SplitPerkNames(const ChoiceName* cn, char* buf, size_t cap, const char** names, int maxNames) {
    if (cn == nullptr || cap == 0) return 0;
    const size_t n = ::strnlen(cn->perks, sizeof(cn->perks));
    const size_t len = n < cap - 1 ? n : cap - 1;
    ::memcpy(buf, cn->perks, len);
    buf[len] = '\0';
    int k = 0;
    char* p = buf;
    char* const end = buf + len;
    while (p < end && k < maxNames) {
        char* nl = (char*)::memchr(p, '\n', (size_t)(end - p));
        if (nl != nullptr) *nl = '\0';
        if (*p != '\0') names[k++] = p;
        if (nl == nullptr) break;
        p = nl + 1;
    }
    return k;
}
static ChoiceName g_choiceName[64];
static LONG g_choiceNameCount = 0;

static void RememberPtrName(unsigned long ptr, const char* name) {
    if (ptr == 0 || name == nullptr) return;
    for (LONG i = 0; i < g_ptrNameCount; ++i) {
        if (g_ptrName[i].ptr == ptr) return;
    }
    if (g_ptrNameCount >= 64) {  // сборка 236: предел молчал — в 235 с него начались спавны без выбора мода
        static LONG s_warn = 0;
        if (s_warn < 20) { ++s_warn; LOG_WARN("Bridge: таблица имён ботов полна (64) — %s без выбора мода по имени", name); }
        return;
    }
    g_ptrName[g_ptrNameCount].ptr = ptr;
    ::strncpy(g_ptrName[g_ptrNameCount].name, name, sizeof(g_ptrName[0].name) - 1);
    g_ptrName[g_ptrNameCount].name[sizeof(g_ptrName[0].name) - 1] = '\0';
    ++g_ptrNameCount;
}

// Выбор мода бота по его указателю: через имя (g_ptrName) → запись g_choiceName. nullptr — по имени выбора нет.
static const ChoiceName* ChoiceOfPtr(unsigned long ptr) {
    if (ptr == 0) return nullptr;
    const char* nm = nullptr;
    for (LONG i = 0; i < g_ptrNameCount; ++i) {
        if (g_ptrName[i].ptr == ptr) { nm = g_ptrName[i].name; break; }
    }
    if (nm == nullptr) return nullptr;
    for (LONG j = 0; j < g_choiceNameCount; ++j) {
        if (::strcmp(g_choiceName[j].name, nm) == 0) return &g_choiceName[j];
    }
    return nullptr;
}

// ★ Сборка 159: флаг репликации +0x374 — тот же вызов, что у перков (0x01A35BA0, маска 0x20). Опыт значка класса:
// EVBOTS_KIT_FLAG=1 — после записи кита ставим маску 0x40 (class-badge.md: движок ставит 0x40 в ветке кита 0xE331B0).
// По умолчанию выкл. Что это даст на стенде — гипотеза, проверяется прогоном.
constexpr unsigned long kOffPlayerReplDirty = 0x374;
constexpr unsigned long kReplDirtyRva = 0x01635BA0UL;  // VA 0x01A35BA0
using ReplDirtyFn = void(__fastcall*)(void*, void*, unsigned long);
// ★ Сборка 185: по умолчанию ВКЛ — движок ставит 0x40 сразу после записи кита всегда (обработчик выбора 0x00E331B0, ветка
// кита 0xE33237–0xE33253), VU идёт через этот обработчик. Без флага клиент получал кит бота только «попутно» — значка класса
// не было с 138, иконки то были, то нет. EVBOTS_KIT_FLAG=0 — как до 185.
static bool KitFlagOn() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_KIT_FLAG", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;
    }
    return s_on != 0;
}

// Ставит боту кит его класса и ФРАКЦИИ ЕГО КОМАНДЫ В ДВИЖКЕ. Вызывается на каждом спавне: команда могла смениться.
static bool ReadObjName(unsigned long obj, char* out, size_t cap);  // сборка 164: проверка устаревших ассетов
// Сборка 185: write=false — только вычислить кит (для обработчика выбора движка), вернуть объект или 0.
static unsigned long ApplyBotKitClass(unsigned long ptr, bool write = true) {
    int cls = -1;
    for (LONG i = 0; i < g_botClassCount; ++i) {
        if (g_botClass[i].ptr == ptr) { cls = g_botClass[i].cls; break; }
    }
    if (cls < 0) {  // сборка 159: класс по имени (SetChoiceByName), до привязки
        const ChoiceName* cn = ChoiceOfPtr(ptr);
        if (cn != nullptr) cls = cn->cls;
    }
    if (cls < 0 || ptr == 0) return 0;
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return 0;
    ScanKitsOnce();
    unsigned int team = 0;
    unsigned long cur = 0;
    __try {
        team = *(const unsigned int*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerTeam);
        cur = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSelectedKit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (team < 1 || team > 4) return 0;  // сборка 186: SQDM — команды 1..4, фракция по чётности (1/3 US, 2/4 RU)
    // ★ Сборка 164: кит из кэша мог устареть со сменой уровня (163b: 6 из 8 адресов — мусор) — проверка имени.
    {
        char nm[120];
        const unsigned long k0 = g_kitAsset[(team % 2 == 0) ? 1 : 0][cls];
        if (k0 != 0 && (!ReadObjName(k0, nm, sizeof(nm)) || ::strncmp(nm, "Gameplay/Kits/", 14) != 0)) {
            LOG_WARN("Bridge: кит класса %d (%s) по %08lX устарел — ищем заново", cls, team % 2 == 0 ? "RU" : "US", k0);
            g_kitScanned = false;
            ScanKitsOnce();
        }
    }
    const unsigned long asset = g_kitAsset[(team % 2 == 0) ? 1 : 0][cls];
    if (asset == 0) {
        static volatile LONG s_warned = 0;
        if (InterlockedIncrement(&s_warned) <= 8) {  // сборка 252: было один раз за запуск — на других картах промах не виден
            LOG_ERR("Bridge: кит класса %d фракции %s в памяти не найден", cls, team % 2 == 0 ? "RU" : "US");
        }
        return 0;
    }
    if (!write || cur == asset) return asset;
    using SetKitFn = void(__fastcall*)(void*, void*, void*);
    __try {
        ((SetKitFn)(base + kSetSelectedKitRva))((void*)(ULONGLONG)ptr, nullptr, (void*)(ULONGLONG)asset);
        if (KitFlagOn()) ((ReplDirtyFn)(base + kReplDirtyRva))((unsigned char*)(ULONGLONG)ptr + kOffPlayerReplDirty, nullptr, 0x40UL);  // сборка 159: опыт значка класса
        static volatile LONG s_logged = 0;
        if (InterlockedIncrement(&s_logged) <= 40) {
            LOG_INFO("Bridge: бот %p — кит класса %d, команда %u (%s), было %08lX -> %08lX", (void*)(ULONGLONG)ptr,
                     cls, team, team % 2 == 0 ? "RU" : "US", cur, asset);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: setSelectedCustomization(бот %p) — исключение в движке", (void*)(ULONGLONG)ptr);
    }
    return asset;
}

// ★ Сборка 97: внешность бота = выбор мода (SelectUnlockAssets). У всех ботов в держателе (+0x0B0 игрока -> +0x1C4)
// лежал объект внешности человека (MP_RU_Engi_Appearance01), поэтому модели одинаковые. Объект — из базы ресурсов
// движка по имени (сборка 168; до того — среди указателей кита), пишем боту в держатель.
struct BotAppearanceReq { unsigned long ptr; char name[120]; };
static BotAppearanceReq g_botAppearance[64];
static LONG g_botAppearanceCount = 0;
constexpr int kOffHolderAppearance = 0x1C4;  // в держателе игрока (+0x0B0)

static bool ReadObjName(unsigned long obj, char* out, size_t cap) {
    out[0] = '\0';
    if (obj == 0 || !Readable((const void*)(ULONGLONG)obj)) return false;
    __try {
        const char* s = *(const char* const*)((const unsigned char*)(ULONGLONG)obj + 0x08);
        if (s == nullptr) return false;
        size_t i = 0;
        while (i + 1 < cap && s[i] != '\0') { out[i] = s[i]; ++i; }
        out[i] = '\0';
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

// ★ Сборка 139: тестовый лог (EVBots-test.log) и состав набора бота — для сопоставления иконок.
int l_bridge_test_log(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    ::evbots::LogTestWrite(s);
    return 0;
}

// NativeBridge.BotKitItems(ptr) → «n=N имя|имя…»: элементы набора бота (+0x4B8..+0x4BC), до 8 штук.
int l_bridge_bot_kit_items(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long begin = 0, end = 0;
    __try {
        begin = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffKitBegin);
        end = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffKitEnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        begin = end = 0;
    }
    char out[480];
    out[0] = '\0';
    size_t used = 0;
    int n = 0;
    if (begin != 0 && end > begin && (end - begin) <= 0x40) {
        const unsigned long cnt = (end - begin) / 4;
        for (unsigned long i = 0; i < cnt && i < 8; ++i) {
            unsigned long el = 0;
            __try {
                el = *(const unsigned long*)((const unsigned char*)(ULONGLONG)begin + i * 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                el = 0;
            }
            char nm[120] = {};
            if (el != 0) ReadObjName(el, nm, sizeof(nm));
            ++n;
            const int w = snprintf(out + used, sizeof(out) - used, "%s%s", (n > 1 ? "|" : ""), nm[0] ? nm : "?");
            if (w < 0 || used + (size_t)w >= sizeof(out) - 1) break;
            used += (size_t)w;
        }
    }
    char res[560];
    snprintf(res, sizeof(res), "n=%d %s", n, out);
    lua_pushstring(L, res);
    return 1;
}

static unsigned long FindAssetByName(unsigned long kit, const char* name);  // сборка 168, определение ниже
static void RememberBotAppearance(unsigned long ptr, const char* name) {
    if (ptr == 0 || name == nullptr || name[0] == '\0') return;
    for (LONG i = 0; i < g_botAppearanceCount; ++i) {
        if (g_botAppearance[i].ptr == ptr) {
            ::strncpy(g_botAppearance[i].name, name, 119);
            g_botAppearance[i].name[119] = '\0';
            return;
        }
    }
    if (g_botAppearanceCount < 64) {
        g_botAppearance[g_botAppearanceCount].ptr = ptr;
        ::strncpy(g_botAppearance[g_botAppearanceCount].name, name, 119);
        g_botAppearance[g_botAppearanceCount].name[119] = '\0';
        ++g_botAppearanceCount;
    }
}

static void ApplyBotAppearance(unsigned long ptr) {
    const char* name = nullptr;
    for (LONG i = 0; i < g_botAppearanceCount; ++i) {
        if (g_botAppearance[i].ptr == ptr) { name = g_botAppearance[i].name; break; }
    }
    if (name == nullptr) {  // сборка 159: внешность по имени (SetChoiceByName), до привязки
        const ChoiceName* cn = ChoiceOfPtr(ptr);
        if (cn != nullptr && cn->app[0] != '\0') name = cn->app;
    }
    if (name == nullptr || ptr == 0) return;
    unsigned long kit = 0, holder = 0, cur = 0;
    __try {
        kit = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSelectedKit);
        holder = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0xB0);
        if (holder != 0) cur = *(const unsigned long*)((const unsigned char*)(ULONGLONG)holder + kOffHolderAppearance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    static volatile LONG s_logged = 0;
    if (kit == 0 || holder == 0) return;
    // ★ Сборка 168: база движка (FindAssetByName), а не обход кита на каждом спавне (тот же путь, что стоил 4–6 с в 165).
    const unsigned long obj = FindAssetByName(kit, name);
    if (obj == 0) {
        if (InterlockedIncrement(&s_logged) <= 40) {
            LOG_INFO("Bridge: ВНЕШНОСТЬ бота %p: объект «%s» не найден (кит %08lX)",
                     (void*)(ULONGLONG)ptr, name, kit);
        }
        return;
    }
    // Сборка 130: набор +0x4B8 пуст (тело посажено до того, как человек выбрал кит, — шаблона нет; прогон 129: модели
    // пропали). В наборе один элемент — объект внешности (прогон 99), кладём туда свой объект бота.
    __try {
        unsigned char* pp = (unsigned char*)(ULONGLONG)ptr;
        const unsigned long kb = *(const unsigned long*)(pp + kOffKitBegin);
        const unsigned long ke = *(const unsigned long*)(pp + kOffKitEnd);
        const unsigned long inl = ptr + (unsigned long)kOffKitInline;
        if (kb == inl && ke == kb) {
            *(unsigned long*)(pp + kOffKitInline) = obj;
            *(unsigned long*)(pp + kOffKitEnd) = inl + 4;
            if (InterlockedIncrement(&s_logged) <= 40) {
                LOG_INFO("Bridge: набор бота %p был пуст — положен свой объект внешности %08lX", (void*)(ULONGLONG)ptr, obj);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (cur == obj) return;
    __try {
        *(unsigned long*)((unsigned char*)(ULONGLONG)holder + kOffHolderAppearance) = obj;
        if (InterlockedIncrement(&s_logged) <= 40) {
            LOG_INFO("Bridge: ВНЕШНОСТЬ бота %p: %08lX -> %08lX «%s»", (void*)(ULONGLONG)ptr, cur, obj, name);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: ВНЕШНОСТЬ бота %p: запись в держатель не удалась", (void*)(ULONGLONG)ptr);
    }
}

// ★ Сборка 185: объект внешности по выбору мода — без записи (для обработчика выбора движка).
static unsigned long ResolveBotAppearance(unsigned long ptr, unsigned long kit) {
    const char* name = nullptr;
    for (LONG i = 0; i < g_botAppearanceCount; ++i) {
        if (g_botAppearance[i].ptr == ptr) { name = g_botAppearance[i].name; break; }
    }
    if (name == nullptr) {
        const ChoiceName* cn = ChoiceOfPtr(ptr);
        if (cn != nullptr && cn->app[0] != '\0') name = cn->app;
    }
    if (name == nullptr || ptr == 0 || kit == 0) return 0;
    return FindAssetByName(kit, name);
}

static void ApplyBotWeaponsVector(unsigned long ptr);  // сборка 98, определение — ниже
static char BotWeaponGroup(unsigned long ptr);  // сборка 152, определение — у ApplyBotWeaponsVector
static int SelectViaEngine(unsigned long ptr);  // сборка 185: выбор через обработчик движка: 0 нет, 1 всё, 2 без перков
static void ApplyBotPerks(unsigned long ptr);  // сборка 107, определение — после SetBotWeapons
static unsigned char* OwnInputOf(unsigned long ptr);  // сборка 125, определение — у MakeOwnInput
static unsigned int BotAiFlag();                      // сборка 125, определение — у BotOwnInput
static unsigned int BotAiFlagFor(unsigned long ptr);  // сборка 173: флаг ИИ по боту (опыт А/Б EVBOTS_BOT_AI=ab)

// ★ Сборка 139: поток движка — тот, где TLS-слот движка (адрес 0x027BCCD4 в образе) даёт непустой контекст. Флаг «идёт хук»
// глобальный: хук может идти на другом потоке, и спавн с потока Lua тогда проходил бы мимо защиты (прогон 06:18: 5 падений).
static bool OnEngineThreadCtx() {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return false;
    DWORD tlsIdx = 0;
    __try {
        tlsIdx = *(const DWORD*)(base + 0x027BCCD4UL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ::TlsGetValue(tlsIdx) != nullptr;
}

// ★ Сборка 146: тело — только когда выбор мода для бота известен (кит класса, внешность, перки). Прогон 145: ранние
// боты (BOT_Horst, BOT_AdaiKava, BOT_Mazutwerty…, 11:34:13–15) сели раньше, чем мод передал выбор (Horst привязан к Lua
// в 11:34:24): у Horst пустой набор (нет модели) и нет иконок, у Mazutwerty нет перков (только звезда лидера). Ждём
// не дольше kChoiceWaitMs, потом сажаем как есть. Респауны не ждут: выбор уже есть.
static bool BotPerksKnown(unsigned long ptr);  // определение — у g_botPerks
struct ChoiceWait { unsigned long ptr; ULONGLONG since; };
static ChoiceWait g_choiceWait[64];
static LONG g_choiceWaitCount = 0;
constexpr ULONGLONG kChoiceWaitMs = 30000ULL;  // сборка 147: было 15 с, ранние боты ждали до 14,2 с (прогон 146)

static bool BotChoiceKnown(unsigned long ptr) {
    bool cls = false, app = false;
    for (LONG i = 0; i < g_botClassCount; ++i) {
        if (g_botClass[i].ptr == ptr) { cls = true; break; }
    }
    for (LONG i = 0; i < g_botAppearanceCount; ++i) {
        if (g_botAppearance[i].ptr == ptr) { app = true; break; }
    }
    if (!cls || !app) {  // сборка 159: выбор по имени (SetChoiceByName) тоже считается выбором
        const ChoiceName* cn = ChoiceOfPtr(ptr);
        if (cn != nullptr) {
            if (cn->cls >= 0) cls = true;
            if (cn->app[0] != '\0') app = true;
        }
    }
    return cls && app && BotPerksKnown(ptr);
}

static unsigned long g_choiceBypass = 0;  // бот, которого ChoiceWaitTick сажает сейчас (гейт пропускает)


// true — сажать сейчас; false — бот поставлен ждать выбора мода (посадку сделает ChoiceWaitTick).
static bool ChoiceGate(unsigned long ptr) {
    if (ptr == g_choiceBypass) return true;
    if (BotChoiceKnown(ptr)) return true;
    for (LONG i = 0; i < g_choiceWaitCount; ++i) {
        if (g_choiceWait[i].ptr == ptr) return false;
    }
    if (g_choiceWaitCount >= 64) return true;
    g_choiceWait[g_choiceWaitCount].ptr = ptr;
    g_choiceWait[g_choiceWaitCount].since = ::GetTickCount64();
    ++g_choiceWaitCount;
    LOG_INFO("Bridge: бот %p — ждём выбор мода (кит/внешность/перки) перед первым телом", (void*)(ULONGLONG)ptr);
    return false;
}

unsigned long NativeSpawnOne(unsigned long ptr, unsigned long spawnPoint);

// Каждый тик (в хуке): выбор пришёл или время вышло — сажаем.
static void ChoiceWaitTick() {
    const ULONGLONG now = ::GetTickCount64();
    for (LONG i = 0; i < g_choiceWaitCount;) {
        const unsigned long p = g_choiceWait[i].ptr;
        const bool known = BotChoiceKnown(p);
        const ULONGLONG waited = now - g_choiceWait[i].since;
        if (!known && waited < kChoiceWaitMs) { ++i; continue; }
        g_choiceWait[i] = g_choiceWait[g_choiceWaitCount - 1];
        --g_choiceWaitCount;
        LOG_INFO("Bridge: бот %p — %s через %llu мс, сажаем", (void*)(ULONGLONG)p,
                 known ? "выбор мода пришёл" : "выбора мода нет", waited);
        unsigned int team = 0;
        __try {
            team = *(const unsigned int*)((const unsigned char*)(ULONGLONG)p + kOffPlayerTeam);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        unsigned long sp = ::evbots::EVBotsEngineSpawnPointForTeam(team);
        if (sp == 0) sp = ::evbots::EVBotsEngineSpawnPointBestForTeam(team);  // 195: не чужая HQ
        if (sp == 0 && !EngineSpawnOn()) {
            LOG_ERR("Bridge: бот %p — точки спавна нет, тело не посажено", (void*)(ULONGLONG)p);
            continue;
        }
        g_choiceBypass = p;
        NativeSpawnOne(p, sp);
        g_choiceBypass = 0;
    }
}

static bool InForgetQueue(unsigned long ptr);  // сборка 153, определение — у очереди «забыть»

// ═══ Сборка 185: посадка тела КАК VU Player:CreateSoldier + Player:SpawnSoldierAt (re\vu\SEQUENCE-REPORT.md, табл. 2) ═══
// VU: CreateSoldier → 0x81FC60 (у нас 0x011821F0: cdecl-подобный thiscall без this, args (blueprint, игрок, transform*),
// ret 0xC — создаёт сущность солдата из blueprint по команде [игрок+0x31C]); SpawnSoldierAt → 0x816810 (у нас 0x011760E0:
// thiscall(extent = игрок + [0x0285BAD8], солдат, transform*, поза), ret 0xC — убрать старое тело, setPose 0x00DFCC50,
// setTransform vf+0x6C, setSoldier 0x00E37180 (+0x3C4), applyCustomization(игрок, солдат, 0) 0x00E322F0, инициализация
// тела 0x00E18980); затем ServerPlayerRespawnMessage (ctor 0x02307700, хэш 0xC935A574, +0x28 игрок) → MessageManager.
// До 185 тело садилось на точку спавна команды (0x00DDBBD0, путь живого игрока) и переносилось телепортом на позицию мода.
// Точка спавна в пути VU не нужна. Сверка начала трёх функций с exe; EVBOTS_ENGINE_SPAWN=0 — по-старому (точка спавна).
constexpr unsigned long kCreateSoldierRva = 0x00D821F0UL;  // VA 0x011821F0
constexpr unsigned long kSpawnSoldierRva = 0x00D760E0UL;   // VA 0x011760E0
constexpr unsigned long kRespawnMsgCtorRva = 0x01F07700UL; // VA 0x02307700
struct BotPoseRec { unsigned long ptr; int pose; };
static BotPoseRec g_botPose[64];
static LONG g_botPoseCount = 0;
static void RememberBotPose(unsigned long ptr, int pose) {
    for (LONG i = 0; i < g_botPoseCount; ++i) {
        if (g_botPose[i].ptr == ptr) { g_botPose[i].pose = pose; return; }
    }
    if (g_botPoseCount < 64) g_botPose[g_botPoseCount++] = {ptr, pose};
}
static int BotPoseOf(unsigned long ptr) {
    for (LONG i = 0; i < g_botPoseCount; ++i) {
        if (g_botPose[i].ptr == ptr) return g_botPose[i].pose;
    }
    return 0;  // CharacterPoseType_Stand
}
// Сборка 185: точка спавна, выбранная модом событием FireEvent("Spawn") — посадка через спавн этой точки (до успеха).
struct ForcedPointRec { unsigned long ptr; unsigned long point; };
static ForcedPointRec g_forcedPoint[64];
static LONG g_forcedPointCount = 0;
static void SpawnEvForget(unsigned long ptr);  // сборка 201: для новой точки событие Spawn ещё не отправлено
static void SetForcedPoint(unsigned long ptr, unsigned long point) {
    SpawnEvForget(ptr);
    for (LONG i = 0; i < g_forcedPointCount; ++i) {
        if (g_forcedPoint[i].ptr == ptr) { g_forcedPoint[i].point = point; return; }
    }
    if (g_forcedPointCount < 64) g_forcedPoint[g_forcedPointCount++] = {ptr, point};
}
static unsigned long ForcedPointOf(unsigned long ptr) {
    for (LONG i = 0; i < g_forcedPointCount; ++i) {
        if (g_forcedPoint[i].ptr == ptr) return g_forcedPoint[i].point;
    }
    return 0;
}
static bool QueuedEnterPos(unsigned long ptr, float* x, float* y, float* z);  // сборка 195, у очереди техники
static bool BotPosOf(unsigned long ptr, float* x, float* y, float* z) {
    for (LONG i = 0; i < g_botPosCount; ++i) {
        if (g_botPos[i].ptr == ptr) {
            *x = g_botPos[i].x; *y = g_botPos[i].y; *z = g_botPos[i].z;
            return *x != 0.0f || *z != 0.0f;
        }
    }
    return false;
}
static bool EngineSpawnOn() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_ENGINE_SPAWN", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;
    }
    return s_on != 0;
}
static bool EngineSpawnFnsOk(unsigned char* base) {
    static volatile LONG s_ok = 0;
    const LONG cur = InterlockedCompareExchange(&s_ok, 0, 0);
    if (cur != 0) return cur > 0;
    static const unsigned char kCreate[12] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xE8, 0x04, 0x00, 0x00};
    static const unsigned char kSpawn[12] = {0x83, 0x3D, 0xD8, 0xBA, 0x85, 0x02, 0x00, 0x56, 0x57, 0x8B, 0xF1, 0x75};
    static const unsigned char kResp[12] = {0x56, 0x68, 0x74, 0xA5, 0x35, 0xC9, 0x68, 0xD3, 0x3D, 0xC9, 0xF3, 0x8B};
    bool ok = false;
    __try {
        ok = std::memcmp(base + kCreateSoldierRva, kCreate, sizeof(kCreate)) == 0 &&
             std::memcmp(base + kSpawnSoldierRva, kSpawn, sizeof(kSpawn)) == 0 &&
             std::memcmp(base + kRespawnMsgCtorRva, kResp, sizeof(kResp)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    InterlockedExchange(&s_ok, ok ? 1 : -1);
    if (ok) LOG_INFO("Bridge: createSoldier 0x011821F0 / spawnSoldier 0x011760E0 / RespawnMessage сверены — посадка как VU");
    else LOG_ERR("Bridge: createSoldier/spawnSoldier/RespawnMessage не совпали с exe — посадка по-старому (точка спавна)");
    return ok;
}
// true — тело посажено. Вызывать только в тике движка, с +0x318 = 0 (как посадка по точке).
static bool VuSpawnSoldier(unsigned long ptr, float x, float y, float z, int pose, unsigned long* soldierOut) {
    *soldierOut = 0;
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0) return false;
    unsigned long blueprint = 0, off = 0;
    __try {
        blueprint = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerKit);
        off = *(const unsigned long*)(base + 0x0245BAD8UL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (blueprint == 0 || off == 0) return false;
    alignas(16) float tr[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, x, y, z, 1.f};
    using CreateFn = unsigned long(__fastcall*)(void*, void*, void*, void*, void*);
    using SpawnFn = void(__fastcall*)(void*, void*, void*, void*, int);
    unsigned long soldier = 0;
    __try {
        soldier = ((CreateFn)(base + kCreateSoldierRva))(nullptr, nullptr, (void*)(ULONGLONG)blueprint,
                                                         (void*)(ULONGLONG)ptr, tr);
        if (soldier == 0) return false;
        ((SpawnFn)(base + kSpawnSoldierRva))((unsigned char*)(ULONGLONG)ptr + off, nullptr, (void*)(ULONGLONG)soldier, tr,
                                              pose);
        *soldierOut = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSoldier);
    } __except (RecordSpawnFault(GetExceptionInformation())) {
        LOG_ERR("Bridge: посадка как VU %p: исключение код=%08lX EIP=%08lX адрес=%08lX (солдат %08lX)",
                (void*)(ULONGLONG)ptr, g_spawnFaultCode, g_spawnFaultEip, g_spawnFaultAddr, soldier);
        return false;
    }
    // ServerPlayerRespawnMessage — как VU после spawnSoldier.
    __try {
        alignas(16) unsigned char msg[0x80] = {};
        using CtorFn = void*(__fastcall*)(void*, void*);
        using DispFn = void(__fastcall*)(void*, void*, void*);
        using DtorFn = void(__fastcall*)(void*, void*);
        ((CtorFn)(base + kRespawnMsgCtorRva))(msg, nullptr);
        *(unsigned long*)(msg + 0x28) = ptr;
        void* mm = *(void* const*)(base + kMsgManagerRva);
        if (mm != nullptr) ((DispFn)(base + kMsgDispatchRva))(mm, nullptr, msg);
        ((DtorFn)(base + kMsgDtorRva))(msg, nullptr);
    } __except (RecordSpawnFault(GetExceptionInformation())) {
        // Сборка 201: с кодом и местом — прогон 16:25 (выход из AirSuperiority): 2 таких исключения, через 18 с зависание сервера.
        LOG_ERR("Bridge: посадка как VU %p: ServerPlayerRespawnMessage — исключение в движке код=%08lX EIP=%08lX адрес=%08lX",
                (void*)(ULONGLONG)ptr, g_spawnFaultCode, g_spawnFaultEip, g_spawnFaultAddr);
    }
    return *soldierOut != 0;
}

// ★ Сборка 186: ворота спавна на смену уровня. Закрывает Lua (Server:RoundOver / Level:Destroy), открывает после Level:Loaded;
// кроме того, без читаемого уровня (Server+0x74/+0x7C → имя «Levels/…») спавн не идёт никогда.
static volatile LONG g_levelGate = 0;
// ★ Сборка 211: текущий уровень движка — только Server+0x74. Живое чтение 22.10 (стенд, mapList.runNextRound XP3_Shield →
// MP_001, шаг 50 мс): +0x74 = 0 на всё время выгрузки/загрузки (1,8 с), старый уровень лежит в +0x7C, новый — в +0x78, потом
// встаёт в +0x74 ПО ТОМУ ЖЕ АДРЕСУ. EngineLevelReady/EngineLevelPtr берут +0x7C, когда +0x74 пуст, — для них уровень «пропадал»
// на 0,3 с, и Lua (опрос 0,5 с, два подряд) смену не видел: прогон 20.10 09:24 (Rush → SQDM) — ворота не закрылись, 825
// исключений «посадка как VU», 339 исключений спавна, боты без тел. Переход +0x74 «был → 0» ловим в каждом тике движка.
static volatile LONG g_levelUnloadSeq = 0;
static volatile LONG g_levelCurSeen = 0;  // +0x74 уже был ненулевым (при старте первый уровень грузится через +0x7C)
static unsigned long EngineCurrentLevelRaw() {
    __try {
        const unsigned long mainp = *(const unsigned long*)(ULONGLONG)0x02877210UL;
        if (mainp == 0) return 0;
        const unsigned long server = *(const unsigned long*)(ULONGLONG)(mainp + 0xD0);
        if (server == 0) return 0;
        return *(const unsigned long*)(ULONGLONG)(server + 0x74);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
/// Тик движка: текущий уровень ушёл (+0x74 стал 0) — номер выгрузки +1 (Lua шлёт Level:Destroy, спавн закрыт сразу).
static void LevelUnloadTick() {
    static unsigned long s_last = 0;
    const unsigned long cur = EngineCurrentLevelRaw();
    if (cur != 0) InterlockedExchange(&g_levelCurSeen, 1);
    if (s_last != 0 && cur == 0) {
        InterlockedExchange(&g_levelGate, 1);
        const LONG n = InterlockedIncrement(&g_levelUnloadSeq);
        LOG_INFO("Bridge: движок выгружает уровень %08lX (Server+0x74 = 0) — выгрузка #%ld, спавн закрыт", s_last, (long)n);
    }
    s_last = cur;
}
// (Сборка 208: ворота по перебору ServerFriendZoneEntity — typeinfo 0x02C23138 — пробовали: итератор зон не находит («зон 0»,
// прогон 06:30), зона — компонент; защита — перехваты самой зоны 0x012E54A0 и 0x012E65F0, см. InstallFriendZoneGuardOnce.)
static bool LevelGateClosed() {
    const bool unloading = InterlockedCompareExchange(&g_levelCurSeen, 0, 0) != 0 && EngineCurrentLevelRaw() == 0;
    if (InterlockedCompareExchange(&g_levelGate, 0, 0) != 0 || unloading || !EngineLevelReady()) {
        static volatile LONG s_n = 0;
        if (InterlockedIncrement(&s_n) % 200 == 1) {
            LOG_INFO("Bridge: спавн закрыт — уровень меняется (ворота=%ld), ждём Level:Loaded", (long)g_levelGate);
        }
        return true;
    }
    return false;
}

// ★ Сборка 201: FireEvent("Spawn") как VU — событие движка ServerPlayerEvent в onEvent точки спавна, а не наш вызов спавна точки.
// Разбор (exe, 19.10): onEvent ServerCharacterSpawnEntity 0x00DE03C0 (слот +0x10 vtable 0x025BBBC0) на id 0x0DC320DE («Spawn»,
// djb2-xor) проверяет класс события (TypeInfo ServerPlayerEvent 0x02C167EC), берёт игрока из слабой ссылки [ev+0x18] и ставит его в
// очередь спавна самой точки ([точка+0xD20], 0x00DDE6C0) — спавн и выбор машины (самолёт XP5) движок делает в обновлении точки.
// Конструктор 0x00DD8660: thiscall(this, игрок, id, b1..b6, команда), ret 0x24 (мод: ServerPlayerEvent("Spawn", p, true, false×5,
// team)); объект 0x1C байт; деструктор — слот +0x10 vtable 0x025BABEC = 0x00DD8720 (flags 0 — без освобождения памяти).
// Прогон 16:38 (XP5_002 AirSuperiority): наш прямой спавн точки — 25 посадок на 1072 отказа движка («свободные самолёты есть,
// боты не садятся» — пользователь). Сверка байтов — всё или ничего (правило 18); EVBOTS_SPAWN_EVENT=0 — по-старому.
constexpr unsigned long kSpEvCtorVa = 0x00DD8660UL;
constexpr unsigned long kSpEvDtorVa = 0x00DD8720UL;
constexpr unsigned long kSpEvVtblVa = 0x025BABECUL;
constexpr unsigned long kSpEvGetTypeVa = 0x00DC9DB0UL;
constexpr unsigned long kCharSpawnVtblVa = 0x025BBBC0UL;
constexpr unsigned long kCharSpawnOnEventVa = 0x00DE03C0UL;
constexpr unsigned long kSpawnEventId = 0x0DC320DEUL;
static int g_spawnEvState = -1;  // -1 — не проверено, 0 — выкл/не совпало, 1 — можно
static bool SpawnEventOn(unsigned char* base) {
    if (g_spawnEvState >= 0) return g_spawnEvState == 1;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_SPAWN_EVENT", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') {
        g_spawnEvState = 0;
        LOG_INFO("Bridge: FireEvent Spawn — событием движка ВЫКЛ (EVBOTS_SPAWN_EVENT=0), спавн точки мостом");
        return false;
    }
    static const unsigned char kCtor[] = {0x8B, 0x44, 0x24, 0x08, 0x56, 0x50, 0x8B, 0xF1, 0xE8};
    static const unsigned char kDtor[] = {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x18};
    static const unsigned char kOnEv[] = {0x81, 0xEC, 0x34, 0x01, 0x00, 0x00};
    bool ok = false;
    __try {
        const unsigned char* c = base + (kSpEvCtorVa - 0x400000UL);
        const unsigned char* d = base + (kSpEvDtorVa - 0x400000UL);
        const unsigned char* o = base + (kCharSpawnOnEventVa - 0x400000UL);
        const unsigned long* vt = (const unsigned long*)(base + (kSpEvVtblVa - 0x400000UL));
        ok = ::memcmp(c, kCtor, sizeof(kCtor)) == 0 && ::memcmp(d, kDtor, sizeof(kDtor)) == 0 &&
             ::memcmp(o, kOnEv, sizeof(kOnEv)) == 0 &&
             vt[0] == (unsigned long)(ULONG_PTR)(base + (kSpEvGetTypeVa - 0x400000UL)) &&
             vt[4] == (unsigned long)(ULONG_PTR)(base + (kSpEvDtorVa - 0x400000UL));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    g_spawnEvState = ok ? 1 : 0;
    if (ok) LOG_INFO("Bridge: ★ FireEvent Spawn — событием движка ServerPlayerEvent (конструктор/деструктор/onEvent сверены), как VU");
    else LOG_ERR("Bridge: FireEvent Spawn — байты конструктора/деструктора/onEvent не совпали с exe, спавн точки мостом (по-старому)");
    return ok;
}
// Точка — ServerCharacterSpawnEntity (vtable совпадает).
// ★ Сборка 211: спавнер техники точки (самолёт в воздухе XP5 и т.п.) — функция движка 0x00DD7140 (thiscall(точка), ret, 0 — нет).
// Её же зовут canSpawnInVehicle 0x00DD8F90 и tryVehicleSpawn 0x00DD9000 («Vehicle is destroyed, cant spawn into it.»). Зовём
// только из хука тика (поток движка), под SEH.
static unsigned long CharSpawnVehicleSpawner(unsigned char* base, unsigned long point) {
    if (base == nullptr || point == 0) return 0;
    using GetVsFn = unsigned long(__fastcall*)(void*, void*);
    __try {
        return ((GetVsFn)(base + (0x00DD7140UL - 0x00400000UL)))((void*)(ULONGLONG)point, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool IsCharSpawnPoint(unsigned char* base, unsigned long point) {
    __try {
        return point != 0 && *(const unsigned long*)(ULONG_PTR)point == (unsigned long)(ULONG_PTR)(base + (kCharSpawnVtblVa - 0x400000UL));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct SpawnEvRec { unsigned long ptr; unsigned long point; };
static SpawnEvRec g_spawnEvFired[64];
static LONG g_spawnEvFiredCount = 0;
static bool SpawnEvFiredFor(unsigned long ptr, unsigned long point) {
    for (LONG i = 0; i < g_spawnEvFiredCount; ++i) if (g_spawnEvFired[i].ptr == ptr) return g_spawnEvFired[i].point == point;
    return false;
}
static void SpawnEvForget(unsigned long ptr) {
    for (LONG i = 0; i < g_spawnEvFiredCount; ++i) {
        if (g_spawnEvFired[i].ptr == ptr) { g_spawnEvFired[i] = g_spawnEvFired[--g_spawnEvFiredCount]; return; }
    }
}
static void SpawnEvMark(unsigned long ptr, unsigned long point) {
    SpawnEvForget(ptr);
    if (g_spawnEvFiredCount < 64) g_spawnEvFired[g_spawnEvFiredCount++] = {ptr, point};
}
// Только в тике движка (главный поток), как и спавн точки.
static bool FireEngineSpawnEvent(unsigned char* base, unsigned long point, unsigned long player) {
    unsigned long team = 0;
    __try {
        team = *(const unsigned long*)((const unsigned char*)(ULONG_PTR)player + kOffPlayerTeam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    alignas(16) unsigned char ev[0x40] = {};
    using CtorFn = void*(__fastcall*)(void*, void*, void*, unsigned long, int, int, int, int, int, int, unsigned long);
    using OnEvFn = void(__fastcall*)(void*, void*, void*);
    using DtorFn = void*(__fastcall*)(void*, void*, int);
    bool built = false, delivered = false;
    __try {
        ((CtorFn)(base + (kSpEvCtorVa - 0x400000UL)))(ev, nullptr, (void*)(ULONG_PTR)player, kSpawnEventId, 1, 0, 0, 0, 0, 0, team);
        built = true;
        const unsigned long vt = *(const unsigned long*)(ULONG_PTR)point;
        const unsigned long fn = *(const unsigned long*)(ULONG_PTR)(vt + 0x10);  // onEvent (или наш перехват RegisterEventCallback)
        ((OnEvFn)(ULONG_PTR)fn)((void*)(ULONG_PTR)point, nullptr, ev);
        delivered = true;
    } __except (RecordSpawnFault(GetExceptionInformation())) {
        LOG_ERR("Bridge: событие Spawn игрока %p точке %p — исключение код=%08lX EIP=%08lX адрес=%08lX",
                (void*)(ULONG_PTR)player, (void*)(ULONG_PTR)point, g_spawnFaultCode, g_spawnFaultEip, g_spawnFaultAddr);
    }
    if (built) {
        __try {
            ((DtorFn)(base + (kSpEvDtorVa - 0x400000UL)))(ev, nullptr, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return delivered;
}

// ★ Сборка 252: набор (+0x4B8) и слоты оружия (+0x50C) бота с прошлого уровня. Бот переживает смену карты, а вектора — нет:
// в них ассеты выгруженного уровня. Раньше их перезаписывал кит/шаблон человека; на XP4 киты не находились (имена *_XP4) и
// шаблон устарел — applyCustomization читал освобождённую память (друг 29.10, 3 падения сервера из 4). Перед первой посадкой
// на новом уровне набор очищаем (его заполнят выбор мода через обработчик движка или свежий шаблон), слоты оружия — если
// хоть один ассет не читается по имени. Как VU: SelectUnlockAssets/SelectWeapon мода — с ассетами текущего уровня.
struct BotLevelRec { unsigned long ptr; LONG seq; };
static BotLevelRec g_botLevelSeq[64];
static LONG g_botLevelSeqCount = 0;
static void DropBotVectorsOfOldLevel(unsigned long ptr) {
    const LONG seq = InterlockedCompareExchange(&g_levelUnloadSeq, 0, 0);
    LONG slot = -1;
    for (LONG i = 0; i < g_botLevelSeqCount; ++i) {
        if (g_botLevelSeq[i].ptr == ptr) { slot = i; break; }
    }
    if (slot >= 0 && g_botLevelSeq[slot].seq == seq) return;
    if (slot < 0) {
        slot = g_botLevelSeqCount < 64 ? g_botLevelSeqCount++ : (LONG)(ptr / 16 % 64);
        g_botLevelSeq[slot].ptr = ptr;
    }
    g_botLevelSeq[slot].seq = seq;
    unsigned long kb = 0, ke = 0, wb = 0, we = 0;
    __try {
        kb = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffKitBegin);
        ke = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffKitEnd);
        wb = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffWpnBegin);
        we = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffWpnEnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const unsigned long kn = (ke > kb) ? (ke - kb) / 4 : 0;
    bool wpnBad = false;
    unsigned long wn = 0;
    if (we > wb && (we - wb) <= kWpnSlots * kWpnEntrySize * 4) {
        wn = (we - wb) / kWpnEntrySize;
        char nm[120];
        for (unsigned long i = 0; i < wn && !wpnBad; ++i) {
            unsigned long a = 0;
            __try {
                a = *(const unsigned long*)(ULONGLONG)(wb + i * kWpnEntrySize);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                wpnBad = true;
                break;
            }
            if (a != 0 && !ReadObjName(a, nm, sizeof(nm))) wpnBad = true;
        }
    }
    __try {
        if (kn > 0) *(unsigned long*)((unsigned char*)(ULONGLONG)ptr + kOffKitEnd) = kb;
        if (wpnBad) *(unsigned long*)((unsigned char*)(ULONGLONG)ptr + kOffWpnEnd) = wb;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: набор/оружие бота %p прошлого уровня — очистка не удалась", (void*)(ULONGLONG)ptr);
        return;
    }
    if (kn > 0 || wpnBad) {
        static volatile LONG s_log = 0;
        if (InterlockedIncrement(&s_log) <= 64) {
            LOG_INFO("Bridge: бот %p, уровень #%ld: набор прошлого уровня очищен (элементов %lu), слоты оружия %s (%lu)",
                     (void*)(ULONGLONG)ptr, (long)seq, kn, wpnBad ? "с мусором — очищены" : "целы", wn);
        }
    }
}

unsigned long NativeSpawnOne(unsigned long ptr, unsigned long spawnPoint) {
    // ★ Сборка 139: спавн вне хука тика падает внутри движка (TlsGetValue = NULL). В прогоне 05:49 так упали BOT_Romeo и
    // BOT_Erwin (поток фонового таймера, «в хуке=0»): у них нет тела и мод не может их убрать. Ставим в очередь хука тика —
    // там спавн работает (g_respawnQueue, см. EVBotsBridgeRetryPending).
    if (ptr != 0 && (InterlockedCompareExchange(&g_inHook, 0, 0) == 0 || !OnEngineThreadCtx())) {
        bool present = false;
        for (LONG i = 0; i < g_respawnCount; ++i) {
            if (g_respawnQueue[i] == ptr) { present = true; break; }
        }
        if (!present && g_respawnCount < kRespawnCapacity) { g_respawnSince[g_respawnCount] = 0; g_respawnQueue[g_respawnCount++] = ptr; }
        else if (!present) LOG_ERR("Bridge: очередь спавна полна, игрок %p не поставлен", (void*)(ULONGLONG)ptr);
        ::evbots::LogTestPrintf("Bridge: спавн %p вне хука тика — в очередь хука (поток=%lu, в очереди=%ld)", (void*)(ULONGLONG)ptr,
                 (unsigned long)GetCurrentThreadId(), (long)g_respawnCount);
        return 0;
    }
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0) return 0;
    // ★ Сборка 186: уровень меняется (конец раунда → выгрузка → загрузка) — движок не сажает, точки старого уровня мёртвые.
    // Прогон TDM 18.10 (14:51): мост сажал ботов во время перезагрузки — 814 исключений C0000005 (EIP 0x009B0468), во 2-м
    // раунде ни одного тела. В VU спавн во время смены уровня не идёт; очередь повторит после Level:Loaded.
    if (LevelGateClosed()) return 0;
    // ★ Сборка 252: у игрока уже есть солдат — второй не сажаем. Друг 29.10 20:03 (XP5_002): kill бота в технике не вызван
    // (список трансформа испорчен), солдат остался; через 16 с мод посадил бота снова → assert движка «Respawning a player who
    // already has a soldier attached!» → AV, падение сервера. Очередь спавна считает такого бота посаженным и снимает его.
    {
        unsigned long have = 0;
        __try {
            have = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            have = 0;
        }
        if (have != 0) {
            static volatile LONG s_haveLog = 0;
            if (InterlockedIncrement(&s_haveLog) <= 40) {
                LOG_WARN("Bridge: спавн %p пропущен — у игрока уже есть солдат %08lX (второй движок не примет)",
                         (void*)(ULONGLONG)ptr, have);
            }
            return 0;
        }
    }
    // ★ Сборка 185: посадка как VU (createSoldier + spawnSoldier на позиции мода) — точка спавна не нужна.
    float vuX = 0.f, vuY = 0.f, vuZ = 0.f;
    // Сборка 185: мод выбрал точку событием «Spawn» — сажаем через неё (как событие VU), а не на позицию.
    const unsigned long forced = ForcedPointOf(ptr);
    if (forced != 0) spawnPoint = forced;
    bool havePos = BotPosOf(ptr, &vuX, &vuY, &vuZ);
    char levelName[128] = {}, levelMode[64] = {};
    const bool baseOnly = EngineLevelInfoRaw(levelName, sizeof(levelName), levelMode, sizeof(levelMode)) &&
        RequireBaseSpawn(levelMode, havePos, forced != 0, HasQueuedEnter(ptr));
    const bool exactRush = RequireExactRushSpawn(levelMode, havePos, forced != 0, HasQueuedEnter(ptr));
    if (baseOnly) {
        // Позиция fun-bots у захваченного флага обходит правила режима. Берём существующий путь HQ своей команды.
        // Это явное ограничение нашего слоя для TankSuperiority0; API VU позволяет произвольный SpawnSoldierAt.
        havePos = false;
        unsigned int team = 0;
        __try { team = *(const unsigned int*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerTeam); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        spawnPoint = ::evbots::EVBotsFindTeamBaseSpawn(team);
        static volatile LONG s_baseLog = 0;
        if (InterlockedIncrement(&s_baseLog) <= 20) {
            LOG_INFO("Bridge: TankSuperiority0: пехотный спавн %p через HQ команды %u (%08lX), флаг не даёт точку появления",
                     (void*)(ULONGLONG)ptr, team, spawnPoint);
        }
    }
    // Сборка 195: позиции мода нет (спавн в технику — пустой трансформ), но посадка в машину ждёт — тело у машины.
    const bool atVehicle = !havePos && forced == 0 && QueuedEnterPos(ptr, &vuX, &vuY, &vuZ);
    if (atVehicle) {
        havePos = true;
        static volatile LONG s_atVehLog = 0;
        if (InterlockedIncrement(&s_atVehLog) <= 30) {
            LOG_INFO("Bridge: спавн %p в технику (мод: пустой трансформ + EnterVehicle) — тело у машины (%.0f, %.0f, %.0f)",
                     (void*)(ULONGLONG)ptr, (double)vuX, (double)vuY, (double)vuZ);
        }
    }
    const bool vuSpawn = forced == 0 && EngineSpawnOn() && EngineSpawnFnsOk(base) && havePos;
    if (exactRush && !vuSpawn) {
        static volatile LONG s_rushUnavailable = 0;
        if (InterlockedIncrement(&s_rushUnavailable) <= 10)
            LOG_WARN("Bridge: Rush: позиционный спавн %p пока недоступен — выбранную базу другим спавнером не заменяю", (void*)ptr);
        return 0;
    }
    if (!vuSpawn && spawnPoint == 0) {
        static unsigned long s_logged[32] = {};
        static LONG s_n = 0;
        bool seen = false;
        for (LONG i = 0; i < s_n; ++i) seen = seen || s_logged[i] == ptr;
        if (!seen && s_n < 32) {
            s_logged[s_n++] = ptr;
            LOG_INFO("Bridge: спавн %p отложен: позиции мода нет и точки спавна нет — повтор на тиках", (void*)(ULONGLONG)ptr);
        }
        return 0;
    }
    // ★ Сборка 173: точка с пустой ссылкой (+0x14) — объект старого уровня или не готовый: движок читает [ref+0x180] и
    // падает (прогон 172, раунд 2: 15 из 15 «исключение … адрес=00000180»). Не зовём движок, спавн ждёт новую точку.
    if (!vuSpawn) {
        unsigned long ref = 0;
        __try {
            ref = *(const unsigned long*)((const unsigned char*)(ULONGLONG)spawnPoint + 0x14);
            if (ref != 0) (void)*(volatile const unsigned long*)((const unsigned char*)(ULONGLONG)ref + 0x180);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ref = 0;
        }
        if (ref == 0) {
            static volatile LONG s_badLogged = 0;
            if (InterlockedIncrement(&s_badLogged) <= 10) {
                LOG_INFO("Bridge: спавн %p: точка %08lX без ссылки (+0x14) — пропуск, ждём точки нового уровня",
                         (void*)(ULONGLONG)ptr, spawnPoint);
            }
            return 0;
        }
    }
    if (!ChoiceGate(ptr)) return 0;  // сборка 146: первое тело — после выбора мода
    if (InForgetQueue(ptr)) {  // сборка 153: мод уже убрал бота («забыть» в очереди) — тело не даём (гонка 19:49:29)
        LOG_INFO("Bridge: спавн %p пропущен — бот в очереди «забыть» (мод его уже убрал)", (void*)ptr);
        return 0;
    }
    DropBotVectorsOfOldLevel(ptr);  // сборка 252
    PrepareBotKit(ptr);
    // ★ Сборка 185: как VU — кит, внешность и перки одним вызовом обработчика выбора движка (флаги репликации 0x40/0x20 ставит
    // он сам). Не всё разблокировано (vars.unlockMode) или флаг EVBOTS_SELECT_ENGINE=0 — прежняя запись полей.
    const int viaEngine = SelectViaEngine(ptr);
    if (viaEngine == 0) ApplyBotKitClass(ptr);
    // сборка 97: внешность по выбору мода — после кита. И после обработчика движка: он заполняет вектор +0x4B8, но не держатель
    // [игрок+0xB0]+0x1C4 (оттуда в 97 у всех ботов была одна модель человека).
    ApplyBotAppearance(ptr);
    // сборка 98: оружие по выбору мода — поверх скопированных слотов человека; 152: группа А (EVBOTS_BOT_WEAPONS) — как 136
    if (BotWeaponGroup(ptr) != 'A') ApplyBotWeaponsVector(ptr);
    if (viaEngine != 1) ApplyBotPerks(ptr);  // сборка 107: перки (специализации) по выбору мода — игрок +0x94

    // ★ 13.10 (прогон 69): после смерти движок очищает кит игрока (+0x480, с release). Респаун без кита
    // падает внутри spawnSoldier (исключение, «отказ» у всех ботов). Если кита нет, возвращаем эталон
    // спавна человека. Состояние пишем в лог, чтобы в следующем прогоне видеть, что было.
    {
        unsigned long kitNow = 0, p318 = 0, p31c = 0, p3c4 = 0;
        __try {
            kitNow = *(const unsigned long*)((const unsigned char*)ptr + kOffPlayerKit);
            p318 = *(const unsigned long*)((const unsigned char*)ptr + kOffPlayerIsAi);
            p31c = *(const unsigned long*)((const unsigned char*)ptr + kOffPlayerTeam);
            p3c4 = *(const unsigned long*)((const unsigned char*)ptr + kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        LOG_INFO("Bridge: спавн %p: до спавна кит=%08lX 318=%08lX 31C=%08lX 3C4=%08lX",
                 (void*)ptr, kitNow, p318, p31c, p3c4);
        {
            // Диагностика контекста: TLS движка (спавн падал при TlsGetValue = 0) и хук тика.
            const DWORD tlsIdx = *(const DWORD*)(base + 0x027BCCD4UL);
            LOG_INFO("Bridge: спавн %p: поток=%lu TLS[%lu]=%08lX в хуке=%ld",
                     (void*)ptr, (unsigned long)GetCurrentThreadId(), (unsigned long)tlsIdx,
                     (unsigned long)(ULONG_PTR)TlsGetValue(tlsIdx),
                     (long)InterlockedCompareExchange(&g_inHook, 0, 0));
        }
        if (kitNow == 0) {
            unsigned long s318 = 0, s31c = 0, s480 = 0;
            ::evbots::EVBotsEngineSpawnRefFields(&s318, &s31c, &s480);
            // ★ Сборка 186: после смены уровня эталон сброшен (LevelReload), а путь «ДО ДИСЛОКАЦИИ» уже отработал —
            // боты ждали, пока человек войдёт в мир (прогон 15:29: 0 тел за 35 с, 768 отказов «кит=0»). Как на старте:
            // блюпринт Characters/Soldiers/MpSoldier из базы движка.
            if (s480 == 0) s480 = ::evbots::LiveFindAssetByName("Characters/Soldiers/MpSoldier");
            if (s480 != 0) SetPlayerRef480(ptr, s480);  // с addref, не напрямую
            __try {
                *(unsigned int*)((unsigned char*)ptr + kOffPlayerIsAi) = (unsigned)s318;
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
            LOG_INFO("Bridge: спавн %p: кит был пуст, восстановлен эталон %08lX (318=%08lX)",
                     (void*)ptr, s480, s318);
        }
    }

    using SpawnFn = int(__fastcall*)(void*, void*, void*, void*, void*, void*);
    int r = 0;
    unsigned long soldier = 0;
    // Помечаем, что это НАШ вызов: перехватчик spawnSoldier не примет его за
    // спавн настоящего игрока и не перезапишет эталон значениями бота.
    // Сборка 125: посадка всегда с isAI = 0 (как все рабочие посадки до 117); флаг вернём после.
    __try {
        *(unsigned int*)((unsigned char*)ptr + kOffPlayerIsAi) = 0u;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    // (Сборка 168: опыт EVBOTS_ICON_AB2 154 — пробная посадка с пустым китом — удалён, ответ получен 15.10.)
    ::evbots::EVBotsEngineOurSpawnBegin();
    bool vuDone = false;
    if (vuSpawn) {
        vuDone = VuSpawnSoldier(ptr, vuX, vuY, vuZ, BotPoseOf(ptr), &soldier);
        r = vuDone ? 1 : 0;
        static volatile LONG s_vuLog = 0;
        if (InterlockedIncrement(&s_vuLog) <= 60) {
            LOG_INFO("Bridge: спавн %p как VU (createSoldier+spawnSoldier) в (%.0f, %.0f, %.0f) поза %d -> солдат=%08lX%s",
                     (void*)ptr, (double)vuX, (double)vuY, (double)vuZ, BotPoseOf(ptr), soldier,
                     vuDone ? "" : (spawnPoint != 0 ? " — не вышло, по точке спавна" : " — не вышло, точки нет"));
        }
        if (!vuDone && (spawnPoint == 0 || exactRush)) {
            if (exactRush) {
                static volatile LONG s_rushFailed = 0;
                if (InterlockedIncrement(&s_rushFailed) <= 10)
                    LOG_WARN("Bridge: Rush: спавн %p на выбранной позиции не выполнен — запасную точку A/B не использую", (void*)ptr);
            }
            ::evbots::EVBotsEngineOurSpawnEnd();
            return 0;
        }
        // ★ Сборка 252: исключение внутри spawnSoldier — солдат мог остаться прикреплённым наполовину. Спавн по точке поверх
        // него давал «Attaching soldier» дважды и assert «Respawning a player who already has a soldier» (друг 29.10).
        if (!vuDone) {
            unsigned long half = 0;
            __try {
                half = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSoldier);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                half = 0;
            }
            if (half != 0) {
                LOG_WARN("Bridge: спавн %p: после неудачной посадки как VU у игрока солдат %08lX — по точке не сажаю",
                         (void*)(ULONGLONG)ptr, half);
                ::evbots::EVBotsEngineOurSpawnEnd();
                return 0;
            }
        }
    }
    // ★ Сборка 201: точка выбрана модом событием FireEvent("Spawn") — отдаём событие движку (как VU) один раз и ждём тело: движок
    // сажает в обновлении точки (самолёт XP5 — сам). Повторы очереди моста событие не дублируют; новая FireEvent мода — заново.
    if (!vuDone && forced != 0 && spawnPoint == forced && SpawnEventOn(base) && IsCharSpawnPoint(base, spawnPoint)) {
        unsigned long have = 0;
        __try {
            have = *(const unsigned long*)((const unsigned char*)ptr + kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) { have = 0; }
        if (SpawnEvFiredFor(ptr, spawnPoint)) {
            SpawnEvForget(ptr);
            if (have != 0) {
                soldier = have;
                r = 1;
                vuDone = true;  // дальше — обычная обработка успешной посадки
                static volatile LONG s_evOkLog = 0;
                if (InterlockedIncrement(&s_evOkLog) <= 40) {
                    LOG_INFO("Bridge: спавн %p по событию Spawn точки %p — движок посадил, солдат=%08lX", (void*)ptr, (void*)spawnPoint, have);
                }
            } else {
                // ★ Сборка 210: за тик движок по событию не посадил — сажаем мостом на ту же точку (как до 201). Прогон 209 (TDM
                // после смены уровня): 37 событий Spawn, ни одного тела, 29 ботов сняты из очереди через 30 с; во всех логах
                // с 19.10 строки «движок посадил» нет ни разу. Очередь точки [+0xD20] движок для ботов не разбирает (почему — не разобрано).
                // ★ Сборка 211 (друг 22.10: на XP5_002/XP5_004 боты «постоянно гибнут, не в начале»): у точки с техникой (самолёт
                // в воздухе) отказ движка законный — самолёт уничтожен/занят (tryVehicleSpawn 0x00DD9000). Мост сажал бота на эту
                // точку без самолёта; мод (BotSpawner.lua:185, «SpawnInJet») получал пешего и убивал его (_EnterVehicleEntity ~= 0 →
                // Kill), и так по кругу. У VU такого запасного пути нет: тела нет — мод посадит бота заново.
                // В Rush отказ явной точки тоже нельзя обходить: движок может отклонить спавнер другого/закрытого сектора.
                if (IsRushMode(levelMode)) {
                    static volatile LONG s_rushPointRefused = 0;
                    if (InterlockedIncrement(&s_rushPointRefused) <= 10)
                        LOG_WARN("Bridge: Rush: точка %p не дала тело %p — отказ не обхожу ручной посадкой", (void*)spawnPoint, (void*)ptr);
                    ::evbots::EVBotsEngineOurSpawnEnd();
                    return 0;
                }
                const unsigned long vsp = CharSpawnVehicleSpawner(base, spawnPoint);
                if (vsp != 0) {
                    static volatile LONG s_evVehLog = 0;
                    if (InterlockedIncrement(&s_evVehLog) <= 20) {
                        LOG_INFO("Bridge: спавн %p — точка %p с техникой (спавнер %08lX): движок тело не дал, мостом не сажаю (как VU)",
                                 (void*)ptr, (void*)spawnPoint, vsp);
                    }
                    ::evbots::EVBotsEngineOurSpawnEnd();
                    return 0;
                }
                static volatile LONG s_evMissLog = 0;
                if (InterlockedIncrement(&s_evMissLog) <= 20) {
                    LOG_INFO("Bridge: спавн %p — по событию Spawn точки %p тела нет за тик, сажаю мостом на эту точку", (void*)ptr, (void*)spawnPoint);
                }
            }
        } else if (FireEngineSpawnEvent(base, spawnPoint, ptr)) {
            SpawnEvMark(ptr, spawnPoint);
            static volatile LONG s_evLog = 0;
            if (InterlockedIncrement(&s_evLog) <= 40) {
                LOG_INFO("Bridge: спавн %p — событие Spawn отдано точке %p (движок сажает сам, как VU)", (void*)ptr, (void*)spawnPoint);
            }
            ::evbots::EVBotsEngineOurSpawnEnd();
            return 0;
        }
    }
    if (!vuDone) __try {
        SpawnFn spawnFn = (SpawnFn)((ULONGLONG)base + kNativeSpawnRva);
        r = spawnFn((void*)spawnPoint, nullptr, (void*)ptr,
                    (void*)(ULONGLONG)-1, (void*)(ULONGLONG)1,
                    (void*)(ULONGLONG)0);
        soldier = *(const unsigned long*)((const unsigned char*)ptr +
                                          kOffPlayerSoldier);
    } __except (RecordSpawnFault(GetExceptionInformation())) {
        ::evbots::EVBotsEngineOurSpawnEnd();
        const unsigned long rva = g_spawnFaultEip - (unsigned long)(ULONGLONG)base;
        LOG_ERR("Bridge: исключение при спавне игрока %p: код=%08lX EIP=%08lX (RVA %08lX) адрес=%08lX",
                (void*)ptr, g_spawnFaultCode, g_spawnFaultEip, rva, g_spawnFaultAddr);
        // Первый провал респауна: что у мёртвого бота обнулено по сравнению с живым ботом и человеком.
        static volatile LONG s_failDiffDone = 0;
        if (InterlockedCompareExchange(&s_failDiffDone, 1, 0) == 0) {
            unsigned long alive = 0;
            for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
                if (g_watchHadSoldier[i] && g_watchBot[i] != ptr) {
                    alive = g_watchBot[i];
                    break;
                }
            }
            DumpDiffWords("респаун: живой бот", alive, ptr, 0x800, 80);
            DumpDiffWords("респаун: человек", ::evbots::EVBotsEngineRealPlayer(), ptr, 0x800, 80);
        }
        return 0;
    }
    ::evbots::EVBotsEngineOurSpawnEnd();
    const bool ok = (r != 0) || (soldier != 0);
    if (ok) XformCheckStep(ptr, vuDone ? "спавн как VU" : "спавн на точке", true);  // 238-6
    // Сборка 125: свой ввод уже стоит (BotOwnInput до тела) — вернуть флаг ИИ, иначе движок не берёт ввод мода.
    if (ok && OwnInputOf(ptr) != nullptr) {
        const unsigned int ai = BotAiFlagFor(ptr);
        __try {
            *(unsigned int*)((unsigned char*)ptr + kOffPlayerIsAi) = ai;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        static volatile LONG s_aiLog = 0;
        if (InterlockedIncrement(&s_aiLog) <= 20) {
            LOG_INFO("Bridge: спавн %p: флаг ИИ после посадки = %u (свой ввод есть)", (void*)ptr, ai);
        }
    }
    LOG_INFO("Bridge: спавн %p на точке %p -> %d, солдат=%08X (%s)",
            (void*)ptr, (void*)spawnPoint, r, soldier,
            ok ? "УСПЕХ" : "отказ движка");
    if (ok) {
        // ★ Сборка 153: бот, которого мост «забыл» (145: убран модом как «без тела»), но мод снова посадил, — вернуть
        // в наблюдение. Иначе его смерть не доходит до мода (прогон 152: BOT_Jayden после смерти навсегда «жив» в Lua,
        // мод 92 раза звал Kill при переходе человека в другую команду).
        {
            bool watched = false;
            for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
                if (g_watchBot[i] == ptr) { watched = true; break; }
            }
            if (!watched && g_watchCount < 64) {
                g_watchBot[g_watchCount] = ptr;
                g_watchHadSoldier[g_watchCount] = 0;
                ++g_watchCount;
                LOG_INFO("Bridge: бот %p снова посажен модом после «забыть» — возвращён в наблюдение (смерть/урон)", (void*)ptr);
            }
        }
        // Сборка 185: при посадке как VU applyCustomization уже выполнил spawnSoldier движка (0x011760E0) — второй раз не
        // зовём (ночной прогон 08:42: двойное применение — вероятная причина assert «Magazine with capacity 7 contains 8»).
        if (!vuDone) ApplyBotKit(ptr);
        RegisterBotTeam(ptr);
        RegisterBotPoints(ptr);
        AssignBotSquad(ptr);
        if (baseOnly) {
            for (LONG i = 0; i < g_botPosCount; ++i) {
                if (g_botPos[i].ptr == ptr) { g_botPos[i].baseSpawned = true; break; }
            }
        }
        // Сборка 185: посадка через точку события «Spawn» — бот остаётся на точке, как в VU (позицию мода не применяем).
        // ★ Сборка 236: как VU — при посадке «как VU» позиция уже задана в spawnSoldier (0x011760E0, трансформ до появления
        // солдата), второй setTransform 0x00E0CEA0 не зовём (прогон 236: он же в том же тике с посадкой в технику —
        // список трансформа испорчен). На старом пути (точка спавна) позицию ставим, кроме спавна в технику (посадка в очереди).
        if (forced == 0 && !vuDone && !HasQueuedEnter(ptr) && !baseOnly) {
            ApplyBotPositionOrQueue(ptr);
        } else if (forced == 0 && vuDone && g_spawnCheckCount < 64) {  // проверка спавна, как после ApplyBotPosition (147)
            g_spawnCheck[g_spawnCheckCount].ptr = ptr;
            g_spawnCheck[g_spawnCheckCount].x = vuX;
            g_spawnCheck[g_spawnCheckCount].z = vuZ;
            g_spawnCheck[g_spawnCheckCount].t = ::GetTickCount64();
            ++g_spawnCheckCount;
        }
    }
    // Диагностика 13.10: связь солдата с игроком (имя над головой), один раз у бота и у человека.
    {
        static volatile LONG s_linkDone = 0;
        if (InterlockedCompareExchange(&s_linkDone, 1, 0) == 0) {
            ScanSoldierLink("бот", ptr, soldier);
            const unsigned long human = ::evbots::EVBotsEngineRealPlayer();
            unsigned long humanSoldier = 0;
            if (human != 0) {
                __try {
                    humanSoldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)human + kOffPlayerSoldier);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    humanSoldier = 0;
                }
            }
            ScanSoldierLink("человек", human, humanSoldier);
        }
    }
    // Диагностика 13.10 (сборка 65): у бота должны быть СВОИ соединение (0),
    // слабая ссылка (+0x4AC, не как у человека) и +0x480 со счётчиком.
    {
        const unsigned long human = ::evbots::EVBotsEngineSpawnKit();
        unsigned long bConn = 0, bWeak = 0, b480 = 0, hConn = 0, hWeak = 0, wpn = 0;
        __try {
            bConn = *(const unsigned char*)((const unsigned char*)ptr + 0x3BC);
            bWeak = *(const unsigned long*)((const unsigned char*)ptr + 0x4AC);
            b480 = *(const unsigned long*)((const unsigned char*)ptr + kOffPlayerKit);
            wpn = (*(const unsigned long*)((const unsigned char*)ptr + 0x510) -
                   *(const unsigned long*)((const unsigned char*)ptr + 0x50C)) / 0x24;
            if (human != 0) {
                hConn = *(const unsigned char*)((const unsigned char*)human + 0x3BC);
                hWeak = *(const unsigned long*)((const unsigned char*)human + 0x4AC);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        LOG_INFO("Bridge: бот %p conn=%lu weak=%08lX 480=%08lX слотов оружия=%lu | "
                 "человек conn=%lu weak=%08lX%s",
                 (void*)ptr, bConn, bWeak, b480, wpn, hConn, hWeak,
                 (bWeak != 0 && bWeak == hWeak) ? "  !!! ОБЩАЯ СЛАБАЯ ССЫЛКА" : "");
    }
    if (ok) InterlockedIncrement(&g_stSpawnOk);
    else InterlockedIncrement(&g_stSpawnFail);
    return ok ? 1UL : 0UL;
}



/// NativeBridge.SpawnEnginePlayer(ptr) -> 1 (успех) / 0 (отказ или отложен)
// ВНИМАНИЕ: очередь отложенных спавнов (g_pending + RetryPendingSpawns)
// была добавлена после успешного прогона 19:33 и привела к падению сервера
// 0xC0000005 при загрузке мода (19:51). Bisect не завершён, очередь убрана —
// сначала возвращаем рабочее поведение. Если понадобится, откладывать спавн
// надо без глобального состояния: например, повтором из того же хука Lua.
int l_bridge_spawn_player(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    // Позиция из трансформа мода (x, y, z), если передана: запоминаем для бота (см. ApplyBotPosition).
    if (lua_gettop(L) >= 4 && lua_isnumber(L, 2) && lua_isnumber(L, 3) && lua_isnumber(L, 4)) {
        RememberBotPos(ptr, (float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4));
    } else {
        // ★ Сборка 197: позиции в этом вызове нет (спавн в технику — пустой LinearTransform мода, или точка спавна) — позицию
        // прошлого SpawnSoldierAt не берём: у VU SpawnSoldierAt ставит тело только по своему трансформу. (Случая, где это
        // мешало, в логах нет — правка по смыслу VU.)
        for (LONG i = 0; i < g_botPosCount; ++i) {
            if (g_botPos[i].ptr == ptr) {
                g_botPos[i] = g_botPos[g_botPosCount - 1];
                --g_botPosCount;
                break;
            }
        }
    }
    // Отряд, который выбрал мод (Player.squadId): мост ставит бота туда после спавна (AssignBotSquad).
    if (lua_gettop(L) >= 5 && lua_isnumber(L, 5)) {
        RememberBotSquad(ptr, (int)lua_tointeger(L, 5));
    }
    // Сборка 185: поза мода (CharacterPoseType) — для spawnSoldier движка, как VU SpawnSoldierAt(soldier, transform, pose).
    if (lua_gettop(L) >= 6 && lua_isnumber(L, 6)) {
        RememberBotPose(ptr, (int)lua_tointeger(L, 6));
    }
    // Сборка 185: 7-й — точка спавна из Entity:FireEvent("Spawn"); без него — обычная посадка (позиция мода).
    SetForcedPoint(ptr, (lua_gettop(L) >= 7 && lua_isnumber(L, 7)) ? (unsigned long)lua_tointeger(L, 7) : 0UL);
    // Вне хука тика спавн не делаем: ставим в очередь, солдат появится на ближайшем тике хука.
    if (ptr != 0 && (InterlockedCompareExchange(&g_inHook, 0, 0) == 0 || !OnEngineThreadCtx())) {
        bool queued = false;
        for (LONG i = 0; i < g_respawnCount; ++i) {
            if (g_respawnQueue[i] == ptr) { queued = true; break; }
        }
        if (!queued && g_respawnCount < kRespawnCapacity) {
            g_respawnSince[g_respawnCount] = 0;
            g_respawnQueue[g_respawnCount++] = ptr;
            queued = true;
        }
        if (!queued) LOG_ERR("Bridge: очередь спавна полна, игрок %p не поставлен", (void*)ptr);
        LOG_INFO("Bridge: спавн %p поставлен в очередь хука тика (поток=%lu, в очереди=%ld)",
                 (void*)ptr, (unsigned long)GetCurrentThreadId(), (long)g_respawnCount);
        lua_pushinteger(L, queued ? 1 : 0);
        return 1;
    }
    if (ptr == 0) {
        lua_pushinteger(L, 0);
        return 1;
    }
    // Раньше здесь была ТОЛЬКО настоящая точка спавна (this у spawnSoldier).
    // Она появляется лишь после первого входа живого игрока, поэтому все
    // боты получали отказ: "движок отказал, остаюсь на Lua".
    // Теперь берём любую пригодную точку из списка конструкторов.
    // ★ Точка только по команде бота (+0x31C). Запасной путь на точку живого
    // игрока (g_spawnSelf) УБРАН: именно он сажал всех ботов на игрока, когда
    // скан точек ещё не закончился. Нет подходящей точки — отказ, мод повторит.
    unsigned int botTeam = 0;
    __try {
        botTeam = *(const unsigned int*)((const unsigned char*)ptr + kOffPlayerTeam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        botTeam = 0;
    }
    const unsigned long spawnPoint = ::evbots::EVBotsEngineSpawnPointForTeam(botTeam);
    // Сборка 185: точки ещё нет — NativeSpawnOne посадит как VU (на позиции мода) или откажет; мод попробует снова.
    lua_pushinteger(L, NativeSpawnOne(ptr, spawnPoint) ? 1 : 0);
    return 1;
}

/// NativeBridge.SetInputLevel(playerPtr, action, level)
/// Команда ввода игрока. fun-bots водит ботов ТОЛЬКО так:
///     p_Bot.m_Player.input:SetLevel(EntryInputActionEnum.EIAForward, 1)
///
/// Пока адрес компонента ввода движка НЕ РАЗОБРАН, функция честно пишет
/// в лог, что команда дошла, и показывает номер действия. Это нужно,
/// чтобы увидеть настоящие значения — и по ним уже искать компонент.
/// Смещение берётся из переменной окружения, чтобы можно было проверить
/// гипотезы без пересборки.
// ★ Сборка 104: свой объект ввода бота — как VU `player.input = EntryInput()`. Разбор (re\entryinput_ctor_*.txt):
// конструктор EntryInputState FUN_008c0040 (thiscall, 1 арг. -> +0xF0): vtable 0x024F0E88, +0x08 = 0;
// копия FUN_008c93e0 (~0x300 байт) берёт ссылки по +0x230 и массиву +0x240 (4×0x20) — у объекта ввода солдата бота
// там мусор, отсюда падение 102. Здесь: память моста (обнулена, движок её не освобождает), конструктор движка,
// игрок+0x3C0 = объект, +0x318 = 1 после спавна (prePhysicsUpdate 0x00E34280 берёт ввод игрока для AI-игрока).
constexpr unsigned long kEntryInputVtableRva = 0x020F1354UL;  // VA 0x024F1354 — vtable полного EntryInputState (сборка 108)
constexpr unsigned long kOffInputDeltaTime = 0x1EC;
constexpr unsigned long kEntryInputCtorFullRva = 0x004C6440UL;  // VA 0x008C6440 — конструктор (сборка 119)
constexpr unsigned long kOffInputPenalty = 0x1FC;              // authoritativeMovementPenalty, по умолчанию 1.0
constexpr unsigned long kOwnInputSize = 0x400;
struct OwnInput { unsigned long ptr; unsigned char* buf; ULONGLONG since; };  // since — сборка 159 (BODY_LEAD_MS)
static OwnInput g_ownInput[64];
static LONG g_ownInputCount = 0;

static unsigned char* OwnInputOf(unsigned long ptr) {
    for (LONG i = 0; i < g_ownInputCount; ++i) {
        if (g_ownInput[i].ptr == ptr) return g_ownInput[i].buf;
    }
    return nullptr;
}

// ★ Сборка 255 (прогон 254): запись снимаем, когда бот удалён в движке (deletePlayer). Раньше таблица только росла:
// после F12 kill all и заказа ботов заново выдано ровно 64 объекта ввода, 13 новых ботов остались без ввода (+0x3C0 = 0) —
// стояли толпой на базе, мод телепортировал их как застрявших (TeleportIfStuck); у всех 8 ботов с испорченным списком
// трансформа (253 — 7, 254 — 1) ввода тоже не было, через минуту — падение 0xE0B053. Буфер не освобождаем (4 КБ на бота):
// на него мог ссылаться удалённый игрок, а повторное использование адреса чужим ботом хуже утечки.
static void ForgetOwnInput(unsigned long ptr) {
    for (LONG i = 0; i < g_ownInputCount; ++i) {
        if (g_ownInput[i].ptr != ptr) continue;
        g_ownInput[i] = g_ownInput[g_ownInputCount - 1];
        InterlockedDecrement(&g_ownInputCount);
        return;
    }
}

static unsigned char* MakeOwnInput(unsigned long ptr, unsigned long owner) {
    unsigned char* buf = OwnInputOf(ptr);
    if (buf != nullptr) return buf;
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return nullptr;
    if (g_ownInputCount >= 64) {
        static volatile LONG s_fullLog = 0;
        if (InterlockedIncrement(&s_fullLog) <= 20)
            LOG_ERR("Bridge: ВВОД: таблица своих объектов ввода полна (64) — бот %p без ввода (стоит, не стреляет)", (void*)(ULONGLONG)ptr);
        return nullptr;
    }
    buf = (unsigned char*)::VirtualAlloc(nullptr, kOwnInputSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buf == nullptr) return nullptr;  // память новая и уже нулевая
    // Сборка 108: полный класс EntryInputState (vtable 0x024F1354, 0x2FC байт) — его движок копирует из игрок+0x3C0
    // функцией 0x008C6030. «Конструктор» 0x00DA2C50 только ставит vtable и обнуляет поля — память уже нулевая.
    // Прежний 0x008C0040 строил ДРУГОЙ класс (024F0E88) и писал владельца в +0xF0 — зона уровней ввода полного класса.
    // deltaTime +0x1EC = 1/30 (у человека ровно 0.03333; VU fun-bots: deltaTime = 1 / тикрейт).
    (void)owner;
    // Сборка 119: конструктор движка EntryInputState 0x008C6440 (thiscall, без аргументов, ret): vtable 0x024F1354 и
    // reset 0x008C16D0 — +0x1FC authoritativeMovementPenalty = 1.0, +0x2D0 = -1, +0x2F8 = 1. Без него +0x1FC = 0, и
    // 0x008B69A0 пишет в солдата газ*0 и стрейф*0 — бот стоит (прогоны 104–118). Память нулевая — ссылочные поля пусты.
    using EisCtorFn = void*(__fastcall*)(void*, void*);
    __try {
        ((EisCtorFn)(base + kEntryInputCtorFullRva))(buf, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: ВВОД: конструктор EntryInputState 0x008C6440 — исключение, ставлю поля вручную");
        *(unsigned long*)buf = (unsigned long)(ULONGLONG)(base + kEntryInputVtableRva);
        *(float*)(buf + kOffInputPenalty) = 1.0f;
    }
    *(float*)(buf + kOffInputDeltaTime) = 1.0f / 30.0f;
    g_ownInput[g_ownInputCount].ptr = ptr;
    g_ownInput[g_ownInputCount].buf = buf;
    g_ownInput[g_ownInputCount].since = ::GetTickCount64();  // сборка 159: момент выдачи ввода (BODY_LEAD_MS)
    ++g_ownInputCount;
    LOG_INFO("Bridge: ВВОД: свой объект ввода бота %p = %p (vtable %08lX, +0x1FC %.2f, +0x2F8 %08lX)", (void*)(ULONGLONG)ptr,
             (void*)buf, *(const unsigned long*)buf, (double)*(const float*)(buf + kOffInputPenalty),
             *(const unsigned long*)(buf + 0x2F8));
    return buf;
}

// ★ Сборка 112: флаг ИИ игрока (+0x318) при своём вводе бота. Разбор: виртуальная функция объекта движения
// +0x20 (0x00D48AF0) возвращает именно этот флаг; условие выбора ввода в prePhysicsUpdate и ИИ-ветка движения
// зависят от него. По умолчанию 1 (как раньше); EVBOTS_BOT_AI=0 — как у человека (опыт сборки 112).
// ★ Сборка 123: перехват vf13C солдата. Разбор (re\death\p1.txt): ServerSoldierEntity vf13C 0x00E07C80
// (thiscall bool, без аргументов) = 1 только при !isAI && солдат+0x249 && PhysicsSettings+0x22; prePhysicsUpdate
// 0x00E34280 берёт ввод игрока (+0x3C0), если isAI != 0 ИЛИ vf13C == 0. Для наших ботов отвечаем 0 — ввод игрока
// применяется при isAI = 0, и движок не удаляет бота при смерти (onDying 0xE114C1) и шлёт «killed X» (0x0131B677).
constexpr unsigned long kSoldierVtVf13CSlotRva = 0x021C3A84UL;  // VA 0x025C3A84 = vtable 0x025C3948 + 0x13C
constexpr unsigned long kSoldierVf13COrigRva = 0x00A07C80UL;    // VA 0x00E07C80
constexpr int kOffSoldierPlayer = 0x144;                         // солдат -> ServerPlayer (onDying)
static volatile LONG g_vf13cOrig = 0;
static volatile LONG g_vf13cInstalled = 0;  // 1 — стоит, -1 — не удалось
static volatile LONG g_vf13cBotHits = 0;

static bool __fastcall SoldierVf13CHook(void* soldier, void* /*edx*/) {
    unsigned long player = 0;
    __try {
        player = *(const unsigned long*)((const unsigned char*)soldier + kOffSoldierPlayer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        player = 0;
    }
    if (player != 0) {
        const LONG n = InterlockedCompareExchange(&g_ownInputCount, 0, 0);
        for (LONG i = 0; i < n && i < 64; ++i) {
            if (g_ownInput[i].ptr == player) {
                InterlockedIncrement(&g_vf13cBotHits);
                return false;
            }
        }
    }
    using Fn = bool(__fastcall*)(void*, void*);
    return ((Fn)(ULONGLONG)InterlockedCompareExchange(&g_vf13cOrig, 0, 0))(soldier, nullptr);
}

static bool InstallSoldierVf13CHook() {
    const LONG st = InterlockedCompareExchange(&g_vf13cInstalled, 0, 0);
    if (st != 0) return st > 0;
    char e[4] = {};
    const DWORD en = ::GetEnvironmentVariableA("EVBOTS_VF13C_HOOK", e, (DWORD)sizeof(e));
    unsigned char* base = NativeModuleBase();
    if ((en == 1 && e[0] == '0') || base == nullptr) {
        InterlockedExchange(&g_vf13cInstalled, -1);
        LOG_INFO("Bridge: vf13C солдата — перехват выключен (EVBOTS_VF13C_HOOK=0 или нет модуля)");
        return false;
    }
    unsigned long* slot = (unsigned long*)(base + kSoldierVtVf13CSlotRva);
    const unsigned long expect = (unsigned long)(ULONGLONG)(base + kSoldierVf13COrigRva);
    unsigned long cur = 0;
    __try {
        cur = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cur = 0;
    }
    if (cur != expect) {
        InterlockedExchange(&g_vf13cInstalled, -1);
        LOG_ERR("Bridge: vf13C солдата: в слоте %08lX, ждали %08lX — перехват НЕ ставлю", cur, expect);
        return false;
    }
    InterlockedExchange(&g_vf13cOrig, (LONG)cur);
    // сборка 237: замена слота — ядром (ev-core), со сверкой прежнего значения
    if (!PatchPointer((void*)slot, (void*)&SoldierVf13CHook, (void*)(ULONGLONG)cur, nullptr, "vf13C солдата")) {
        InterlockedExchange(&g_vf13cInstalled, -1);
        LOG_ERR("Bridge: vf13C солдата: ядро не заменило слот");
        return false;
    }
    InterlockedExchange(&g_vf13cInstalled, 1);
    LOG_INFO("Bridge: ★ vf13C солдата перехвачен (слот %p: %08lX -> %p): ботам isAI = 0, ввод — свой",
             (void*)slot, cur, (void*)&SoldierVf13CHook);
    return true;
}

// ★ Сборка 124: ИИ-ветки смерти сервера выключены патчами (разбор re\death, сверено дизассемблером exe).
// С isAI = 1 у ботов есть ники над головой (прогон 123: при 0 ники пропали), а смерть идёт как у человека:
// игрок не удаляется, «killed X» и очки — как у человека. EVBOTS_AI_DEATH_PATCH=0 — без патчей.
struct CodePatch { unsigned long rva; unsigned char len; unsigned char from[6]; unsigned char to[6]; const char* what; };
static const CodePatch kAiDeathPatches[] = {
    {0x00A114C8UL, 2, {0x74, 0x15}, {0xEB, 0x15}, "onDying солдата: не удалять ИИ-игрока"},
    {0x0095146EUL, 2, {0x74, 0x1A}, {0xEB, 0x1A}, "onDying персонажа: не удалять ИИ-игрока"},
    {0x00A0EEF0UL, 2, {0x74, 0x15}, {0xEB, 0x15}, "снятие солдата: не удалять ИИ-игрока"},
    {0x009D46B6UL, 2, {0x74, 0x15}, {0xEB, 0x15}, "спавн-сущность: не удалять ИИ-игрока"},
    {0x00F1B67EUL, 6, {0x0F, 0x85, 0x37, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, "PointSystem: «killed X» жертве-ИИ"},
    {0x00F1AFC3UL, 2, {0x75, 0x4E}, {0x90, 0x90}, "PointSystem: очки убийце-ИИ"},
};
static volatile LONG g_aiDeathPatched = 0;  // 1 — стоят, -1 — выключены/не удалось

// ★ 12.11.2026 (Royale/локальный стенд): обход проверки контрольной суммы уровня на сервере.
// Разбор bf3_server_Final.exe (D:\Games-cache\royale\server-runtime-v3): VA 0x00E4838E (RVA 0x00A4838E),
// сравнение 4×dword checksum клиента/сервера; при несовпадении — «Level Checksum Failed!» + кик
// (SecureReason_MismatchingContent). -skipChecksum движком не учитывается. Патч по умолчанию выключен:
// EVBOTS_SKIP_LEVEL_CHECKSUM=1 — заменить je→jmp на success-путь. Всё или ничего, сверка байт перед записью.
static const CodePatch kLevelChecksumPatches[] = {
    {0x00A4838EUL, 6, {0x0F, 0x84, 0xDC, 0x00, 0x00, 0x00}, {0xE9, 0xDC, 0x00, 0x00, 0x00, 0x90},
     "level checksum: не кикать при несовпадении (je→jmp success)"},
};
static volatile LONG g_levelChecksumPatched = 0;  // 1 — стоят, -1 — выключены/не удалось

static void ApplyAiDeathPatches() {
    if (InterlockedCompareExchange(&g_aiDeathPatched, 0, 0) != 0) return;
    char e[4] = {};
    const DWORD en = ::GetEnvironmentVariableA("EVBOTS_AI_DEATH_PATCH", e, (DWORD)sizeof(e));
    unsigned char* base = NativeModuleBase();
    if ((en == 1 && e[0] == '0') || base == nullptr) {
        InterlockedExchange(&g_aiDeathPatched, -1);
        LOG_INFO("Bridge: патчи смерти ИИ выключены (EVBOTS_AI_DEATH_PATCH=0 или нет модуля)");
        return;
    }
    // Сначала проверяем ВСЕ места: если хоть одно не совпало — не пишем ничего.
    for (const CodePatch& cp : kAiDeathPatches) {
        const unsigned char* at = base + cp.rva;
        bool same = true, done = true;
        __try {
            for (unsigned i = 0; i < cp.len; ++i) {
                if (at[i] != cp.from[i]) same = false;
                if (at[i] != cp.to[i]) done = false;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            same = done = false;
        }
        if (!same && !done) {
            InterlockedExchange(&g_aiDeathPatched, -1);
            LOG_ERR("Bridge: патч смерти ИИ: байты по VA %08lX не те (%s) — НИЧЕГО не пишу",
                    (unsigned long)(ULONGLONG)at, cp.what);
            return;
        }
    }
    int n = 0;
    for (const CodePatch& cp : kAiDeathPatches) {
        unsigned char* at = base + cp.rva;
        // сборка 237: запись — ядром (ev-core); сверка «всё или ничего» — выше
        if (!PatchBytes(at, nullptr, cp.to, cp.len, cp.what)) {
            LOG_ERR("Bridge: патч смерти ИИ: ядро не записало VA %08lX", (unsigned long)(ULONGLONG)at);
            continue;
        }
        ++n;
        LOG_INFO("Bridge: патч смерти ИИ: VA %08lX — %s", (unsigned long)(ULONGLONG)at, cp.what);
    }
    InterlockedExchange(&g_aiDeathPatched, n == (int)(sizeof(kAiDeathPatches) / sizeof(kAiDeathPatches[0])) ? 1 : -1);
    LOG_INFO("Bridge: ★ патчи смерти ИИ: %d из %u", n, (unsigned)(sizeof(kAiDeathPatches) / sizeof(kAiDeathPatches[0])));
}

static void ApplyLevelChecksumBypassOnce() {
    if (InterlockedCompareExchange(&g_levelChecksumPatched, 0, 0) != 0) return;
    char e[4] = {};
    const DWORD en = ::GetEnvironmentVariableA("EVBOTS_SKIP_LEVEL_CHECKSUM", e, (DWORD)sizeof(e));
    unsigned char* base = NativeModuleBase();
    // Только выделенный сервер (bf3_server_Final.exe). На клиенте NativeModuleBase() == nullptr — тихий no-op.
    if (!(en == 1 && e[0] == '1') || base == nullptr) {
        if (en == 1 && e[0] == '1' && base == nullptr) {
            LOG_INFO("Bridge: патч checksum уровня пропущен (не серверный процесс)");
        }
        InterlockedExchange(&g_levelChecksumPatched, -1);
        return;  // по умолчанию выкл (локальный стенд/Royale: EVBOTS_SKIP_LEVEL_CHECKSUM=1)
    }
    for (const CodePatch& cp : kLevelChecksumPatches) {
        const unsigned char* at = base + cp.rva;
        bool same = true, done = true;
        __try {
            for (unsigned i = 0; i < cp.len; ++i) {
                if (at[i] != cp.from[i]) same = false;
                if (at[i] != cp.to[i]) done = false;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            same = done = false;
        }
        if (!same && !done) {
            InterlockedExchange(&g_levelChecksumPatched, -1);
            LOG_ERR("Bridge: патч checksum уровня: байты по VA %08lX не те (%s) — НИЧЕГО не пишу",
                    (unsigned long)(ULONGLONG)at, cp.what);
            return;
        }
    }
    int n = 0;
    for (const CodePatch& cp : kLevelChecksumPatches) {
        unsigned char* at = base + cp.rva;
        if (!PatchBytes(at, nullptr, cp.to, cp.len, cp.what)) {
            LOG_ERR("Bridge: патч checksum уровня: ядро не записало VA %08lX", (unsigned long)(ULONGLONG)at);
            continue;
        }
        ++n;
        LOG_INFO("Bridge: ★ патч checksum уровня: VA %08lX — %s", (unsigned long)(ULONGLONG)at, cp.what);
    }
    InterlockedExchange(&g_levelChecksumPatched,
                        n == (int)(sizeof(kLevelChecksumPatches) / sizeof(kLevelChecksumPatches[0])) ? 1 : -1);
    LOG_INFO("Bridge: патчи checksum уровня: %d из %u (EVBOTS_SKIP_LEVEL_CHECKSUM=1)",
             n, (unsigned)(sizeof(kLevelChecksumPatches) / sizeof(kLevelChecksumPatches[0])));
}

// ★ Сборка 175: по умолчанию isAI = 0 (+ перехват vf13C), как бот VU — обычный игрок: очки, звание, иконки (прогон 174:
// всё это только у группы А). Цена: ника над головой у таких ботов пока нет (123, 174). EVBOTS_BOT_AI=1 — isAI = 1
// (ники есть, очков нет, смерть — через патчи выше); =ab — опыт А/Б по группам VU-создания.
static int BotAiEnv() {
    static const int env = [] {
        char b[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_BOT_AI", b, (DWORD)sizeof(b));
        if (n == 1 && b[0] == '1') return 1;
        if (n == 2 && (b[0] == 'a' || b[0] == 'A')) return 2;
        return 0;
    }();
    return env;
}

static unsigned int BotAiFlag() {
    if (BotAiEnv() == 0) return InstallSoldierVf13CHook() ? 0u : 1u;
    return 1u;
}

// ★ Сборка 173: как у VU — бот обычный игрок (+0x318 = 0). Факт (Ghidra, MODLOG 173): очки отсекаются по +0x318 в двух
// местах — ScoringSystem 0x0120ADE0 (убийца-ИИ: событие выброшено) и PointSystem::addPoints 0x0131CF90; у VU-ботов
// (CreatePlayer(0,-1,0), полное «в уровне») флага нет. Ники при 0 пропадали в сборке 123 — тогда у бота не было
// сообщения создания и «в уровне». EVBOTS_BOT_AI=ab — 0 только группе А VU-создания (опыт в одном прогоне).
static unsigned int BotAiFlagFor(unsigned long ptr) {
    if (BotAiEnv() == 2) return (VuGroupOf(ptr) == 'A' && InstallSoldierVf13CHook()) ? 0u : 1u;
    return BotAiFlag();
}

/// NativeBridge.BotOwnInput(ptr, on): on=1 — свой объект ввода в игрок+0x3C0 и +0x318=1; on=0 — вернуть 0/0.
/// Ответ: true, если игрок+0x3E8 == солдат (третье условие prePhysicsUpdate).
int l_bridge_bot_own_input(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const int on = (int)luaL_optinteger(L, 2, 1);
    unsigned long soldier = 0, ctl = 0, cur = 0;
    __try {
        soldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSoldier);
        ctl = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3E8);
        cur = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3C0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushboolean(L, 0);
        return 1;
    }
    unsigned char* buf = (on != 0) ? MakeOwnInput(ptr, soldier) : OwnInputOf(ptr);
    __try {
        unsigned char* p = (unsigned char*)(ULONGLONG)ptr;
        // Сборка 105: ServerSoldierEntity::prePhysicsUpdateEntry 0x00E0A7C0 — если байт солдата +0x249 == 0, персонаж
        // каждый кадр ставится в позицию сущности (FUN_008a2310): физика его не двигает. +0x249 = 1 — «отцепить».
        unsigned char* sol = (unsigned char*)(ULONGLONG)soldier;
        if (on != 0 && buf != nullptr) {
            if (cur == 0) *(unsigned long*)(p + 0x3C0) = (unsigned long)(ULONGLONG)buf;
            *(unsigned int*)(p + kOffPlayerIsAi) = BotAiFlagFor(ptr);
            if (sol != nullptr) {
                LOG_INFO("Bridge: ОПЫТ ввод VU: солдат %08lX +0x249 = %u (не трогаем: прогон 105 — уже 1)", soldier,
                         (unsigned)sol[0x249]);
            }
        } else {
            if (buf != nullptr && cur == (unsigned long)(ULONGLONG)buf) *(unsigned long*)(p + 0x3C0) = 0;
            *(unsigned int*)(p + kOffPlayerIsAi) = 0u;
        }
        cur = *(const unsigned long*)(p + 0x3C0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: BotOwnInput(%p) — исключение", (void*)(ULONGLONG)ptr);
    }
    LOG_INFO("Bridge: ОПЫТ ввод VU: игрок %p on=%d +0x3C0=%08lX (свой %p) +0x3E8=%08lX %s солдат %08lX", (void*)(ULONGLONG)ptr,
             on, cur, (void*)buf, ctl, ctl == soldier ? "==" : "!=", soldier);
    lua_pushboolean(L, ctl == soldier ? 1 : 0);
    // сборка 255: второй ответ — свой ввод стоит в +0x3C0 (Lua повторяет вызов, пока не встанет)
    lua_pushboolean(L, (on != 0 && buf != nullptr && cur == (unsigned long)(ULONGLONG)buf) ? 1 : 0);
    return 2;
}

// Сборка 126 (диагностика): сколько раз мод нажал огонь (EIAFire = 7) и сколько урона засчитал движок.
static volatile LONG g_fireWrites = 0;

int l_bridge_set_input(lua_State* L) {
    static volatile LONG g_shown = 0;
    const unsigned long player = (unsigned long)luaL_checkinteger(L, 1);
    const long action = (long)luaL_checkinteger(L, 2);
    const double level = luaL_checknumber(L, 3);
    if (player == 0) return 0;
    if (!NonFiniteGuard(level, "SetInputLevel", action)) return 0;
    if (action == 7 && level > 0.5) InterlockedIncrement(&g_fireWrites);

    // ★ НАСТОЯЩИЙ путь к входу бота (найден 12.10 по отчёту
    // `re\input_component.txt`, подтверждено декомпиляцией prePhysicsUpdate):
    //   ServerPlayer -> [+0x3C4] ServerSoldier (наши kOffPlayerSoldier)
    //   ServerSoldier/Controllable -> [+0x3C0] EntryInputState
    //   EntryInputState::setLevel = 0x008BEF00 (__thiscall: this, int action,
    //     float level). Действия 0..9 пишутся в this+0x10+action*4 (float),
    //     10+ — бит в this+0x1E0/0x1E4.
    // Если путь не подтвердится — log ASCII-диагностикой.
    static volatile LONG g_ok = 0;
    unsigned char* base = NativeModuleBase();
    unsigned long soldier = 0, input = 0;
    if (base != nullptr) {
        __try {
            if (Readable((const void*)player)) {
                soldier = *(const unsigned long*)((const unsigned char*)player +
                                                  kOffPlayerSoldier);
            }
            if (soldier != 0 && Readable((const void*)soldier)) {
                input = *(const unsigned long*)((const unsigned char*)soldier +
                                                0x3C0);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    // Сборка 104: если у бота свой объект ввода (висит в игрок+0x3C0) — пишем в него, его читает prePhysicsUpdate
    {
        unsigned char* own = OwnInputOf(player);
        if (own != nullptr) {
            unsigned long cur = 0;
            __try {
                cur = *(const unsigned long*)((const unsigned char*)(ULONGLONG)player + 0x3C0);
            } __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
            if (cur == (unsigned long)(ULONGLONG)own) input = cur;
        }
    }

    // ★ ВЫКЛЮЧЕНО по умолчанию (13.10, прогон 61: краш сервера в Lua VM через 1 с
    // после того, как у привязанных ботов появились солдаты). Три ошибки: 0x008BEF00 —
    // это VA, а не RVA (base + VA = чужая функция); __thiscall(this, action, level)
    // объявлен __fastcall (action уходит в EDX, стек ломается); путь [солдат+0x3C0]
    // не подтверждён. Включить для проверки: EVBOTS_INPUT_NATIVE=1.
    static int s_inputNative = -1;
    if (s_inputNative < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_INPUT_NATIVE", v, (DWORD)sizeof(v));
        s_inputNative = (n == 1 && v[0] == '0') ? 0 : 1;  // сборка 136: по умолчанию ВКЛ (без него боты не ходят), =0 — выкл
    }
    if (input != 0 && s_inputNative == 1) {
        __try {
            // Сборка 89: адрес — RVA (VA 0x8BEF00 − 0x400000), соглашение __thiscall(this, action, level), ret 8.
            using InputFn = void(__thiscall*)(void*, long, float);
            InputFn setLevel =
                (InputFn)((ULONGLONG)base + 0x004BEF00UL);
            setLevel((void*)input, action, (float)level);
            InterlockedIncrement(&g_ok);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }

    if (InterlockedIncrement(&g_shown) <= 12) {
        LOG_INFO("INPUT player=%08X soldier=%08X input=%08X action=%ld level=%.3f (ok=%ld)",
                 player, soldier, input, action, level, (long)g_ok);
    }
    return 0;
}

/// NativeBridge.KillEnginePlayer(playerPtr) -> 1 (сообщение разослано) / 0
/// Настоящий SoldierEntity:Kill() — см. NativeKillPlayer.
int l_bridge_kill_player(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    lua_pushinteger(L, NativeKillPlayer(ptr) ? 1 : 0);
    return 1;
}

/// NativeBridge.TestsEnabled() -> bool: прогон проверок включён (EVBOTS_TESTS=1).
int l_bridge_tests_enabled(lua_State* L) {
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_TESTS", v, (DWORD)sizeof(v));
    lua_pushboolean(L, (n == 1 && v[0] == '1') ? 1 : 0);
    return 1;
}

// Запись уровня ввода движка EntryInputState::setLevel. Адрес 0x008BEF00 — это VA,
// RVA = VA - 0x400000. __thiscall(this, int action, float level) объявлено как
// __fastcall(this, фиктивный, action, level): стековые аргументы совпадают.
// Разбор агентом 13.10: RVA 0x4BEF00, __thiscall; объект ввода игрока — [player+0x3C0].
constexpr unsigned long kSetLevelRvaProbe = 0x004BEF00UL;

/// NativeBridge.ReadCStr(addr, off) -> строка по указателю, лежащему по addr+off (до 80 символов) | nil.
/// Только для проверок прогона: имена китов и блюпринтов (указатель на имя — по +0x08 объекта).
int l_bridge_read_cstr(lua_State* L) {
    const unsigned long addr = (unsigned long)luaL_checkinteger(L, 1);
    const long off = (long)luaL_checkinteger(L, 2);
    if (addr == 0 || off < 0 || off > 0x1000) {
        lua_pushnil(L);
        return 1;
    }
    char buf[81] = {};
    bool ok = false;
    __try {
        const char* s = *(const char* const*)((const unsigned char*)(ULONGLONG)addr + off);
        if (s != nullptr) {
            size_t i = 0;
            while (i < 80 && s[i] != '\0') {
                buf[i] = s[i];
                ++i;
            }
            buf[i] = '\0';
            ok = (i > 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, buf);
    return 1;
}

/// NativeBridge.ReadU32(ptr, off) -> число | nil. Только для проверок прогона (off <= 0x1000).
int l_bridge_read_u32(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const long off = (long)luaL_checkinteger(L, 2);
    if (ptr == 0 || off < 0 || off > 0x1000) {
        lua_pushnil(L);
        return 1;
    }
    unsigned long v = 0;
    __try {
        v = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

/// NativeBridge.HumanPtr(name) -> ptr человека с этим именем | 0 (сборка 201: несколько людей — у каждого свой указатель).
int l_bridge_human_ptr(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)::evbots::EVBotsEngineHumanByName(luaL_checkstring(L, 1)));
    return 1;
}

/// NativeBridge.Humans() -> { {ptr, name, onlineId}, ... } — все люди сервера из списка игроков движка (сборка 202).
/// onlineId — поле движка [игрок+0x20] (persona из Blaze, как VU player.onlineId); 0 — не прочитано.
int l_bridge_humans(lua_State* L) {
    unsigned long hs[64];
    // Сборка 214: второе значение — список прочитан уверенно (false — нет менеджера/массива или запись без имени).
    const int r = ::evbots::EVBotsEngineHumansChecked(hs, 64);
    const bool sure = r >= 0;
    const int n = sure ? r : -1 - r;
    lua_createtable(L, n, 0);
    int k = 0;
    for (int i = 0; i < n; ++i) {
        char nm[64] = {};
        unsigned long oid = 0;
        __try {
            const unsigned char* p = (const unsigned char*)(ULONGLONG)hs[i];
            oid = *(const unsigned long*)(p + kOffPlayerOnlineId);
            const char* s = *(const char* const*)(p + 0x10);
            if (s != nullptr) {
                for (int j = 0; j < 63 && s[j] != '\0'; ++j) nm[j] = s[j];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (nm[0] == '\0') continue;
        lua_createtable(L, 3, 0);
        lua_pushinteger(L, (lua_Integer)hs[i]); lua_rawseti(L, -2, 1);
        lua_pushstring(L, nm); lua_rawseti(L, -2, 2);
        lua_pushinteger(L, (lua_Integer)oid); lua_rawseti(L, -2, 3);
        lua_rawseti(L, -2, ++k);
    }
    lua_pushboolean(L, sure ? 1 : 0);
    return 2;
}

/// NativeBridge.RealPlayer() -> ptr игрока-человека | 0 (для проверок прогона)
int l_bridge_real_player(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)::evbots::EVBotsEngineRealPlayer());
    return 1;
}

// addToSquad(ServerPlayer* p, SquadId sq, byte fA, byte fB) — cdecl, 4 стековых аргумента, RET без
// операнда. Разбор агентом 13.10 и проверка прологa дизассемблером: sq = arg2, fA = arg3, fB = arg4.
// VA 0x0118D8D0 -> RVA 0x00D8D8D0. Только из игрового потока; sq 0..32; полный отряд — вернёт 0.
constexpr unsigned long kAddToSquadRvaBridge = 0x00D8D8D0UL;

// BEGIN PlayerTeamProperty native adapter (also compiled by the isolated fixture).
namespace playerteam {
using Ptr=unsigned long;
volatile LONG verified=0,faulted=0;
bool Fail(const char** error,const char* text) {if(error)*error=text;return false;}
bool Read(Ptr at,void* out,unsigned size) {
    if(!at)return false;
    __try {std::memcpy(out,reinterpret_cast<const void*>(at),size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool Verify(const char** error) {
    if(verified==1)return true;
    if(verified<0)return Fail(error,"player team/squad signature batch failed");
    if(reinterpret_cast<Ptr>(NativeModuleBase())!=0x400000)return Fail(error,"player team/squad requires the verified dedicated server");
    const unsigned char team[]={0xa1,0xd8,0xd7,0xb5,2,0x81,0xec,0x38,1,0,0,0x80,0x38,0};
    const unsigned char squad[]={0xa1,0xd8,0xd7,0xb5,2,0x81,0xec,0xb8,1,0,0,0x80,0x38,0};
    struct Signature {Ptr address;unsigned char bytes[16];unsigned size;};
    const Signature signatures[]={
        {0x011714C0,{0x8b,0x81,0xf4,0,0,0,0xc3},7},
        {0x0083D5C0,{0xa1,0x74,0xaa,0xb6,2,0x64,0x8b,0x0d,0x2c,0,0,0,0x8b,0x14,0x81,0x8b},16},
        {0x0083D5E0,{0xa1,0x74,0xaa,0xb6,2,0x64,0x8b,0x0d,0x2c,0,0,0,0x8b,0x14,0x81,0x8b},16}
    };
    bool ok=EVBotsVerifyTeamPropertyEntry(0x0118EF80,team,sizeof(team));
    LOG_INFO("PlayerTeam: signature118EF80 %s",ok?"PASS":"FAIL");
    const bool squadOk=EVBotsVerifyTeamPropertyEntry(0x0118D8D0,squad,sizeof(squad));ok=ok&&squadOk;
    LOG_INFO("PlayerTeam: signature118D8D0 %s",squadOk?"PASS":"FAIL");
    for(const auto& s:signatures) {
        unsigned char bytes[16]={};const bool one=Read(s.address,bytes,s.size) && !std::memcmp(bytes,s.bytes,s.size);
        ok=ok&&one;LOG_INFO("PlayerTeam: signature%08lX %s",s.address,one?"PASS":"FAIL");
    }
    InterlockedExchange(&verified,ok?1:-1);
    return ok || Fail(error,"player team/squad signatures differ; no native mutation");
}
bool Access(Ptr player,const char* name,unsigned field,int value,unsigned* team,unsigned* squad,const char** error) {
    if(error)*error=nullptr;
    if(!team || !squad || !player || !name || !*name)return Fail(error,"player team/squad requires a live named player");
    if(field>2 || (field && (value<0 || value>(field==1?127:32))))return Fail(error,"invalid team/squad enum");
    if(faulted)return Fail(error,"native player team/squad fault; further access disabled");
    if(!Verify(error))return false;
    if(!EVBotsServerFrameTickActive() || !EVBotsOnServerFrameThread())return Fail(error,"player team/squad requires the actual native server frame");
    Ptr index=0,tls=0,physics=0,context=0,manager=0,begin=0,end=0,extent=0;
    const Ptr vector=__readfsdword(0x2C);
    if(!Read(0x02B6AA74,&index,4) || index>4096 || !vector || !Read(vector+index*4,&tls,4) ||
        !tls || !Read(tls+0x66C,&physics,4) || !physics)return Fail(error,"player team/squad engine TLS is unavailable");
    if(!Read(0x02B3EE2C,&context,4) || !context || !Read(context+0x30,&manager,4) || !manager ||
        !Read(manager+0x80,&begin,4) || !Read(manager+0x84,&end,4) || !begin || end<begin ||
        (end-begin)%4 || (end-begin)/4>128)return Fail(error,"player team/squad native player list is unavailable");
    bool member=false;
    for(Ptr item=begin;item<end;item+=4) {Ptr p=0;if(!Read(item,&p,4))return Fail(error,"unreadable native player list");if(p==player){member=true;break;}}
    if(!member)return Fail(error,"player disconnected or is not in the native player list");
    if(!Read(0x0285BAD8,&extent,4) || extent<4 || extent>0x2000)return Fail(error,"native squad component offset is unavailable");
    Ptr previous=0;bool switched=false,ok=false;
    __try {
        const char* nativeName=*reinterpret_cast<const char* const*>(player+0x10);
        if(!nativeName || std::strncmp(nativeName,name,128)!=0)return Fail(error,"player identity changed");
        *team=*reinterpret_cast<const unsigned*>(player+0x31C);
        *squad=*reinterpret_cast<const unsigned*>(player+extent+0xF4);
        if(!field)return true;
        if(InterlockedCompareExchange(&g_levelGate,0,0)!=0)return Fail(error,"player team/squad cannot change during level transition");
        const Ptr first=*reinterpret_cast<const Ptr*>(0x0285BB38),last=*reinterpret_cast<const Ptr*>(0x0285BB3C);
        const unsigned limit=TeamCapacityInstalled()?128u:17u;
        if(!first || last<first || (last-first)%8 || (last-first)/8>limit ||
            *team>=(last-first)/8 || (field==1 && static_cast<unsigned>(value)>=(last-first)/8))
            return Fail(error,"team is outside the current native TeamSize table");
        if((field==1?*team:*squad)==static_cast<unsigned>(value))return true;
        previous=reinterpret_cast<Ptr(__cdecl*)()>(0x0083D5E0)();
        reinterpret_cast<void(__cdecl*)(Ptr)>(0x0083D5C0)(context);switched=true;
        using Change=unsigned char(__cdecl*)(Ptr,unsigned,unsigned,unsigned);
        // Native force/private flag is arg3 for squad. Team arg3=0 removes the
        // previous squad; arg4=0 bypasses automatic team-capacity balancing.
        const auto change=reinterpret_cast<Change>(field==1?0x0118EF80:0x0118D8D0);
        const unsigned char accepted=change(player,static_cast<unsigned>(value),field==2?1u:0u,0u);
        *team=*reinterpret_cast<const unsigned*>(player+0x31C);
        *squad=*reinterpret_cast<const unsigned*>(player+extent+0xF4);
        ok=accepted && (field==1?*team:*squad)==static_cast<unsigned>(value);
        if(!ok)Fail(error,"native team/squad manager rejected the requested assignment");
    } __except(EXCEPTION_EXECUTE_HANDLER) {InterlockedExchange(&faulted,1);Fail(error,"native player team/squad access failed");}
    if(switched) __try {reinterpret_cast<void(__cdecl*)(Ptr)>(0x0083D5C0)(previous);}
    __except(EXCEPTION_EXECUTE_HANDLER) {ok=false;InterlockedExchange(&faulted,1);Fail(error,"restoring player team/squad context failed");}
    if(ok)LOG_INFO("PlayerTeam: player=%08lX field=%u requested=%d nativeTeam=%u nativeSquad=%u",player,field,value,*team,*squad);
    return ok;
}
}
// END PlayerTeamProperty native adapter.
int l_bridge_player_team_property(lua_State* L) {
    const auto pointer=luaL_checkinteger(L,1);
    if(pointer<=0 || static_cast<unsigned long long>(pointer)>0xFFFFFFFFull)return luaL_error(L,"Player.teamId/squadId: invalid native pointer");
    const auto player=static_cast<unsigned long>(pointer);
    const char* name=luaL_checkstring(L,2);
    const auto requestedField=luaL_optinteger(L,3,0);
    if(requestedField<0 || requestedField>2)return luaL_error(L,"Player.teamId/squadId: invalid field");
    const auto field=static_cast<unsigned>(requestedField);
    const auto requestedValue=field?luaL_checkinteger(L,4):0;
    if(requestedValue<0 || requestedValue>(field==1?127:32))return luaL_error(L,"Player.teamId/squadId: invalid enum value");
    const int value=static_cast<int>(requestedValue);
    unsigned team=0,squad=0;const char* error=nullptr;
    if(!playerteam::Access(player,name,field,value,&team,&squad,&error))return luaL_error(L,"Player.teamId/squadId: %s",error?error:"native access failed");
    lua_pushinteger(L,team);lua_pushinteger(L,squad);return 2;
}

/// NativeBridge.JoinSquad(playerPtr, squadId) -> байт результата | -1 (нет базы/ошибка) | -2 (исключение)
///   | -3 (уровень меняется или команда игрока вне списка команд движка — не зовём)
int l_bridge_join_squad(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const unsigned int squad = (unsigned int)luaL_checkinteger(L, 2);
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0 || squad > 32) {
        lua_pushinteger(L, -1);
        return 1;
    }
    // ★ Сборка 196 (прогон 195, 02:46:02): при смене SQDM → CQL мост подводил отряды ботам команд 3/4, когда в движке
    // уже было 3 команды, — 40 исключений внутри addToSquad (у 18.10 21:36:37 рядом был FatalAssert «Invalid team 4»).
    // Движок сам так не делает: joinSquad 0x01189D60 проверяет команду по массиву команд («Invalid team») и до
    // переноса игроков в команды нового уровня отряды не трогает. Пока ворота уровня закрыты — тоже не зовём.
    unsigned int team = 0, teams = 0;
    if (InterlockedCompareExchange(&g_levelGate, 0, 0) != 0 || !EngineTeamInList(ptr, &team, &teams)) {
        static LONG s_logged = 0;
        if (InterlockedIncrement(&s_logged) <= 20) {
            LOG_INFO("Bridge: addToSquad(игрок %p, отряд %u) не зову: %s (команда %u, команд в движке %u)", (void*)ptr,
                     squad, g_levelGate != 0 ? "уровень меняется" : "команда вне списка", team, teams);
        }
        lua_pushinteger(L, -3);
        return 1;
    }
    using SquadFn = unsigned char(__cdecl*)(void*, unsigned int, unsigned int, unsigned int);
    unsigned int r = 0;
    __try {
        r = (unsigned int)((SquadFn)(base + kAddToSquadRvaBridge))((void*)(ULONGLONG)ptr, squad, 0u, 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: addToSquad(игрок %p, отряд %u) — исключение в движке", (void*)ptr, squad);
        lua_pushinteger(L, -2);
        return 1;
    }
    LOG_INFO("Bridge: addToSquad(игрок %p, отряд %u, 0, 0) -> %u", (void*)ptr, squad, r & 0xFFu);
    lua_pushinteger(L, (lua_Integer)(r & 0xFFu));
    return 1;
}

// ── Сборка 132: настоящая поза солдата (SoldierEntity:SetPose -> движок) ─────────────────────────────────
// Мод выбирает позу (стоя / присед / лёжа) и по ней считает высоту прицела (Utilities:getTargetHeight), а при
// стрельбе приседает бота (UpdateShootMovement). Раньше SetPose был заглушкой: поза жила только в Lua, в движке
// бот стоял, и прицел шёл на высоту, которой у бота нет, — промахи (HANDOFF п. 5.1).
//   SetPose -> очередь моста -> движок (хук тика); soldier.pose -> живая поза из движка (см. __index в Lua).
// Вызов движка: fb::CharacterPhysicsEntity::setPose, VA 0x00623CF0, thiscall: ECX = объект позы
// ([солдат+0x1D4]), 4 стековых аргумента (ret 0x10): поза, immediately, override, checkState.
// Сверка: дизассемблер exe (push esi; mov esi,ecx; ...; ret 0x10), разбор re\pose\decomp-state.txt и
// xrefs-setpose.txt (вызовы из forceValidPose и setPoseConstraints). Текущая поза — [объект+0x7C], следующая — +0x80.
// EVBOTS_POSE_REAL=0 — выключить (поза только в Lua, как до 132).
constexpr unsigned long kPoseSetRva = 0x00223CF0UL;  // VA 0x00623CF0
constexpr int kOffPoseCur = 0x7C;                    // текущая поза: 0 стоя, 1 присед, 2 лёжа
constexpr int kOffPoseNext = 0x80;                   // следующая (целевая) поза
struct PoseReq { unsigned long ptr; int pose; int tries; };
static PoseReq g_poseQueue[64];
static LONG g_poseCount = 0;
static LONG g_poseReqs = 0, g_poseOk = 0, g_poseRefused = 0, g_poseNoObj = 0, g_poseLogged = 0;
static LONG g_poseInVeh = 0;  // 238-6: запросы позы, снятые потому, что бот в технике

/// Объект позы солдата игрока: [игрок+0x3C4] -> [солдат+0x1D4]. 0, если солдата нет или память не читается.
static unsigned long PoseObjectOf(unsigned long player) {
    if (player == 0) return 0;
    unsigned long soldier = 0;
    unsigned long obj = 0;
    __try {
        soldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)player + kOffPlayerSoldier);
        if (soldier != 0) {
            obj = *(const unsigned long*)((const unsigned char*)(ULONGLONG)soldier + kOffSoldierCharCtrl);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return obj;
}

/// Текущая и следующая поза объекта. false — объекта нет или память не читается.
static bool PoseReadObj(unsigned long obj, int* cur, int* next) {
    if (obj == 0) return false;
    unsigned long c = 0;
    unsigned long n = 0;
    __try {
        c = *(const unsigned long*)((const unsigned char*)(ULONGLONG)obj + kOffPoseCur);
        n = *(const unsigned long*)((const unsigned char*)(ULONGLONG)obj + kOffPoseNext);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *cur = (c <= 2) ? (int)c : -1;
    *next = (n <= 2) ? (int)n : -1;
    return true;
}

/// Вызов движка: поставить позу (immediately=1, override=1, checkState=0). 1 — принято, 0 — отказ, <0 — ошибка.
static int CallEnginePose(unsigned long obj, int pose) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || obj == 0) return -1;
    using PoseFn = unsigned char(__fastcall*)(void*, void*, int, int, int, int);
    unsigned char ok = 0;
    __try {
        ok = ((PoseFn)(base + kPoseSetRva))((void*)(ULONGLONG)obj, nullptr, pose, 1, 1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
    return ok ? 1 : 0;
}

/// NativeBridge.SetBotPose(ptr, pose) -> true: поза в очереди моста; движок меняет её в хуке тика.
int l_bridge_set_bot_pose(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const int pose = (int)luaL_checkinteger(L, 2);
    if (ptr == 0 || pose < 0 || pose > 2) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ++g_poseReqs;
    for (LONG i = 0; i < g_poseCount; ++i) {
        if (g_poseQueue[i].ptr == ptr) {
            g_poseQueue[i].pose = pose;
            g_poseQueue[i].tries = 0;
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    if (g_poseCount < 64) {
        g_poseQueue[g_poseCount].ptr = ptr;
        g_poseQueue[g_poseCount].pose = pose;
        g_poseQueue[g_poseCount].tries = 0;
        ++g_poseCount;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/// NativeBridge.GetBotPose(ptr) -> поза движка 0/1/2 | nil (солдата нет). Это «живая» поза для soldier.pose.
int l_bridge_get_bot_pose(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    int cur = -1;
    int next = -1;
    if (PoseReadObj(PoseObjectOf(ptr), &cur, &next) && cur >= 0) {
        lua_pushinteger(L, (lua_Integer)cur);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// Очередь позы (хук тика). Вызов движка — только если поза ещё не стоит. Запрос снимается, когда движок принял
/// позу (или поза уже такая); если солдата нет — до 300 тиков; если движок отказал — 10 раз.
static void PoseTick() {
    LONG i = 0;
    while (i < g_poseCount) {
        const unsigned long obj = PoseObjectOf(g_poseQueue[i].ptr);
        const int want = g_poseQueue[i].pose;
        int cur = -1;
        int next = -1;
        bool done = false;
        if (!PoseReadObj(obj, &cur, &next)) {
            if (++g_poseQueue[i].tries > 300) {
                ++g_poseNoObj;
                done = true;
            }
        } else if (cur == want && next == want) {
            done = true;
        } else if (PlayerInVehicle(g_poseQueue[i].ptr)) {
            // ★ 238-6: в технике позу ведёт место машины. Наш вызов (override=1, checkState=0 — без проверки состояния) на
            // солдате открытого места (мотоцикл KLR650, место 2 Vodnik) — кандидат порчи списка трансформа (прогон 238-6
            // 23:19:43: порча через 230 мс после посадки, без наших вызовов посадки/перестановки в этот промежуток).
            // Запрос снимаем: мод при высадке попросит позу снова.
            ++g_poseInVeh;
            done = true;
        } else {
            XformCheckStep(g_poseQueue[i].ptr, "поза", false);  // 238-6
            const int res = CallEnginePose(obj, want);
            XformCheckStep(g_poseQueue[i].ptr, "поза", true);
            if (res == 1) {
                ++g_poseOk;
                done = true;
            } else if (++g_poseQueue[i].tries >= 10) {
                ++g_poseRefused;
                done = true;
            }
            if (g_poseLogged < 30) {
                ++g_poseLogged;
                int c2 = -1;
                int n2 = -1;
                PoseReadObj(obj, &c2, &n2);
                LOG_INFO("ДИАГ поза: бот %p просили %d, было cur=%d next=%d, движок вернул %d, теперь cur=%d next=%d",
                         (void*)(ULONGLONG)g_poseQueue[i].ptr, want, cur, next, res, c2, n2);
            }
        }
        if (done) {
            g_poseQueue[i] = g_poseQueue[g_poseCount - 1];
            --g_poseCount;
        } else {
            ++i;
        }
    }
}

// ── Сборка 178: техника — посадка и выход (Player:EnterVehicle / ExitVehicle) ────────────────────────────────
// Разбор re\vehicle\NOTES.md (сверено по коду VU 0x00AB4EA0/0x00AB4060 и дизассемблеру exe, правило 18):
//   enterEntry 0x00E37950 — thiscall: ecx = ServerPlayer, [esp+4] = управляемая сущность (ServerVehicleEntity),
//   [esp+8] = номер места, ret 8; exitEntry 0x00E365F0 — thiscall: ecx = игрок, 2 аргумента-флага, ret 8.
// Поля игрока: [+0x3D8] — сущность, в которой сидит (attached), [+0x3D4] — номер места, [+0x3E8] — управляемая (controlled).
// Lua зовёт из потока-таймера, движок меняет состояние только в хуке тика → очередь (как поза). EVBOTS_VEH_ENTER=0 — выкл.
constexpr unsigned long kEnterEntryRva = 0x00E37950UL - 0x00400000UL;
constexpr unsigned long kExitEntryRva = 0x00E365F0UL - 0x00400000UL;
constexpr int kOffPlayerControlled = 0x3E8;
constexpr int kOffPlayerAttached = 0x3D8;
constexpr int kOffPlayerEntryId = 0x3D4;
struct VehReq { unsigned long player; unsigned long entity; int seat; int flagA; int flagB; int kind; int tries; };  // kind 1 вход, 2 выход
static VehReq g_vehQueue[32];
static LONG g_vehCount = 0;
// ★ Сборка 195: позиция машины, в которую для бота ждёт посадка (запрос входа в очереди). fun-bots при спавне в технику
// зовёт SpawnSoldierAt с пустым LinearTransform() и сразу EnterVehicle (у VU оба синхронны, бот в машине в том же кадре).
// У нас тело садится в хуке, посадка — следующим тиком: тело ставим у машины, а не на HQ (прогон 194: 11 таких спавнов
// сняты за 30 с без тела — точек ещё не было). Трансформ — как его читает движок (0x00E0C29C, см. ApplyBotPosition).
static LONG g_vehEnters = 0, g_vehExits = 0, g_vehFails = 0, g_vehLogged = 0;

// ★ Сборка 211: кто сидит на месте машины — как читает движок в enterEntry (0x00E379E9): места [ent+0xFC..+0x100),
// игрок — [место+0x4C]. false — память не читается или номера места нет.
static bool SeatOccupant(unsigned long ent, int seat, unsigned long* occ) {
    *occ = 0;
    if (ent == 0 || seat < 0) return false;
    __try {
        const unsigned char* e = (const unsigned char*)(ULONGLONG)ent;
        const unsigned long b = *(const unsigned long*)(e + 0xFC);
        const unsigned long en = *(const unsigned long*)(e + 0x100);
        if (b == 0 || en < b || (unsigned long)seat >= (en - b) / 4UL) return false;
        const unsigned long s = *(const unsigned long*)(ULONGLONG)(b + 4UL * (unsigned long)seat);
        *occ = (s != 0) ? *(const unsigned long*)(ULONGLONG)(s + 0x4C) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ★ Сборка 235: падение сервера 00E0B030 (прогоны 234-в, 234-г). enterEntry 0x00E37950 (дизассемблер) проверяет
// «заспавнена» — бит [ent+0x0A] & 1 — и номер места < ([ent+0x100]-[ent+0xFC])/4, но в релизе оба assert только пишут в
// RuntimeLog и идут дальше: место берётся за концом массива, запись в чужую память (234-г, 16:52:28: «Trying to enter
// non-spawned vehicle» и «getEntry invoked with invalid index (1)», через 5 с — падение в высадке). VU (EnterVehicle
// 0x1098B770, re\vehicle\NOTES.md) до вызова движка проверяет тип ServerVehicleEntity и номер места; у нас посадка ещё и
// ждёт тика — машина может исчезнуть. Проверяем то же, что VU, плюс бит движка, и при входе в очередь, и перед вызовом.
static const char* EnterTargetBad(unsigned long ent, int seat) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ent == 0) return "нет сущности";
    if (seat < 0) return "номер места < 0";
    __try {
        const unsigned char* e = (const unsigned char*)(ULONGLONG)ent;
        if (*(const unsigned long*)e != (unsigned long)(ULONG_PTR)(base + (0x025BF8E8UL - 0x00400000UL)))
            return "не ServerVehicleEntity";
        if ((e[0x0A] & 1u) == 0) return "не заспавнена ([+0x0A] & 1 = 0)";
        const unsigned long b = *(const unsigned long*)(e + 0xFC);
        const unsigned long en = *(const unsigned long*)(e + 0x100);
        if (b == 0 || en < b || (unsigned long)seat >= (en - b) / 4UL) return "нет такого места";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "память не читается";
    }
    return nullptr;
}
static LONG g_vehBadLogged = 0;

static bool HasQueuedEnter(unsigned long ptr) {
    for (LONG i = 0; i < g_vehCount; ++i) {
        if (g_vehQueue[i].kind == 1 && g_vehQueue[i].player == ptr) return true;
    }
    return false;
}

static void VehQueueRemoveAt(LONG idx) {
    for (LONG i = idx + 1; i < g_vehCount; ++i) g_vehQueue[i - 1] = g_vehQueue[i];
    --g_vehCount;
}

static bool QueuedEnterPos(unsigned long ptr, float* x, float* y, float* z) {
    unsigned long ent = 0;
    for (LONG i = 0; i < g_vehCount; ++i) {
        if (g_vehQueue[i].kind == 1 && g_vehQueue[i].player == ptr) {
            // ★ Сборка 211 (жалоба друга 22.10: «игрок в самолёте — бот появляется прямо на нём, жизни отнимаются»). Мод
            // выбрал свободное место, но спавн у нас ждёт очереди, и место успел занять человек. Движок в enterEntry на занятом
            // месте не сажает (0x00E37A20: «Failed to remove other player from entry», выход 0x00E37CF4), а тело уже стояло бы
            // в точке машины — внутри самолёта. У VU спавн и посадка синхронны, место было бы свободно. Посадку снимаем, тело —
            // на обычную точку; мод сам переводит пешего бота из «техники» в Moving (StateInVehicle*Control.lua).
            unsigned long occ = 0;
            if (SeatOccupant(g_vehQueue[i].entity, g_vehQueue[i].seat, &occ) && occ != 0 && occ != ptr) {
                LOG_INFO("ДИАГ техника: спавн %08lX в машину %08lX место %d — место уже занял игрок %08lX, посадка снята, тело на обычную точку",
                         ptr, g_vehQueue[i].entity, g_vehQueue[i].seat, occ);
                ++g_vehFails;
                VehQueueRemoveAt(i);
                return false;
            }
            ent = g_vehQueue[i].entity;
            break;
        }
    }
    if (ent == 0) return false;
    float px = 0.f, py = 0.f, pz = 0.f;
    __try {
        const unsigned char* e = (const unsigned char*)(ULONGLONG)ent;
        const unsigned char* tr = (const unsigned char*)(ULONGLONG)*(const unsigned long*)(e + 0x1C);
        if (tr == nullptr) return false;
        if (((*(const unsigned long*)(e + 8) >> 18) & 1u) != 0) tr = tr + 0x10 + (unsigned long)tr[6] * 0x50;
        px = *(const float*)(tr + 0x30); py = *(const float*)(tr + 0x34); pz = *(const float*)(tr + 0x38);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!(px == px) || !(pz == pz) || (px == 0.f && pz == 0.f)) return false;
    *x = px; *y = py; *z = pz;
    return true;
}

static bool VehEnabled() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VEH_ENTER", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;
    }
    return s_on != 0;
}

/// Поля игрока: attached, номер места, controlled. false — память не читается.
static bool ReadPlayerVehicle(unsigned long player, unsigned long* attached, unsigned long* entry, unsigned long* controlled) {
    if (player == 0) return false;
    __try {
        const unsigned char* p = (const unsigned char*)(ULONGLONG)player;
        *attached = *(const unsigned long*)(p + kOffPlayerAttached);
        *entry = *(const unsigned long*)(p + kOffPlayerEntryId);
        *controlled = *(const unsigned long*)(p + kOffPlayerControlled);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

/// NativeBridge.PlayerVehicle(ptr) -> attached, entryId, controlled (указатели сущностей движка, 0 — нет) | nil
int l_bridge_player_vehicle(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long a = 0, e = 0, c = 0;
    if (!ReadPlayerVehicle(ptr, &a, &e, &c)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)a);
    lua_pushinteger(L, (lua_Integer)e);
    lua_pushinteger(L, (lua_Integer)c);
    return 3;
}

// Сборка 240: та же проверка, что IsVehicleEntity в VextEntityManager.cpp (emIsVeh): vtable ServerVehicleEntity 0x025BF8E8.
static bool IsVehicleEntity(unsigned long ptr) {
    static unsigned char* const base = NativeModuleBase();  // exe сервера не выгружается; на клиенте — nullptr, всегда false
    if (base == nullptr || ptr == 0) return false;
    __try {
        return *(const unsigned long*)(ULONG_PTR)ptr == (unsigned long)(ULONG_PTR)(base + (0x025BF8E8UL - 0x00400000UL));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// ★ Сборка 240: NativeBridge.PlayerVehicleV(ptr) -> attached, entryId, controlled | nil — как PlayerVehicle, но указатель, который
/// не ServerVehicleEntity, отдаётся 0 (та же проверка, что emIsVeh в обёртке техники). У пешего [игрок+0x3E8] — сам солдат:
/// раньше каждое чтение controlledControllable/attachedControllable (мод — много раз за тик) шло в Lua-обёртку техники с промахом
/// кэша и pcall emIsVeh(солдат). Результат для Lua тот же.
int l_bridge_player_vehicle_v(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long a = 0, e = 0, c = 0;
    if (!ReadPlayerVehicle(ptr, &a, &e, &c)) {
        lua_pushnil(L);
        return 1;
    }
    if (a != 0 && !IsVehicleEntity(a)) a = 0;
    if (c != 0 && !IsVehicleEntity(c)) c = 0;
    lua_pushinteger(L, (lua_Integer)a);
    lua_pushinteger(L, (lua_Integer)e);
    lua_pushinteger(L, (lua_Integer)c);
    return 3;
}

/// ★ Сборка 245: NativeBridge.PlayerOnFoot(ptr) -> true, если игрок не в технике ([+0x3D8] и [+0x3E8] — не ServerVehicleEntity);
/// false — в технике или чтение не удалось (тогда Lua идёт обычным путём vehicleFieldValue). Быстрый путь геттеров техники.
int l_bridge_player_on_foot(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long a = 0, e = 0, c = 0;
    const bool ok = ReadPlayerVehicle(ptr, &a, &e, &c);
    lua_pushboolean(L, ok && (a == 0 || !IsVehicleEntity(a)) && (c == 0 || !IsVehicleEntity(c)) ? 1 : 0);
    return 1;
}

static int VehQueuePush(unsigned long player, unsigned long entity, int seat, int a, int b, int kind) {
    if (!VehEnabled() || player == 0) return 0;
    for (LONG i = 0; i < g_vehCount; ++i) {  // мод зовёт каждый кадр, пока не сел: последний запрос игрока заменяет прежний
        if (g_vehQueue[i].player == player) {
            g_vehQueue[i].entity = entity;
            g_vehQueue[i].seat = seat;
            g_vehQueue[i].flagA = a;
            g_vehQueue[i].flagB = b;
            g_vehQueue[i].kind = kind;
            return 1;  // tries не сбрасываем: мод зовёт каждый кадр, счётчик ожидания тела общий
        }
    }
    if (g_vehCount >= 32) return 0;
    g_vehQueue[g_vehCount].tries = 0;
    g_vehQueue[g_vehCount].player = player;
    g_vehQueue[g_vehCount].entity = entity;
    g_vehQueue[g_vehCount].seat = seat;
    g_vehQueue[g_vehCount].flagA = a;
    g_vehQueue[g_vehCount].flagB = b;
    g_vehQueue[g_vehCount].kind = kind;
    ++g_vehCount;
    return 1;
}

/// NativeBridge.EnterVehicle(player, entity, seat) -> true: посадка в очереди моста.
int l_bridge_enter_vehicle(lua_State* L) {
    const unsigned long pl = (unsigned long)luaL_checkinteger(L, 1);
    const unsigned long ent = (unsigned long)luaL_checkinteger(L, 2);
    const int seat = (int)luaL_checkinteger(L, 3);
    const char* bad = EnterTargetBad(ent, seat);  // сборка 235: как проверки VU EnterVehicle до вызова движка
    if (bad != nullptr) {
        ++g_vehFails;
        if (g_vehBadLogged < 200) {
            ++g_vehBadLogged;
            LOG_INFO("ДИАГ техника: EnterVehicle игрок %08lX сущность %08lX место %d — отказ: %s", pl, ent, seat, bad);
        }
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, VehQueuePush(pl, ent, seat, 0, 0, 1) ? 1 : 0);
    return 1;
}

/// NativeBridge.ExitVehicle(player, flagA, flagB) -> true: выход в очереди моста.
int l_bridge_exit_vehicle(lua_State* L) {
    const unsigned long pl = (unsigned long)luaL_checkinteger(L, 1);
    const int a = lua_toboolean(L, 2) ? 1 : 0;
    const int b = lua_toboolean(L, 3) ? 1 : 0;
    lua_pushboolean(L, VehQueuePush(pl, 0, 0, a, b, 2) ? 1 : 0);
    return 1;
}

/// Сборка 186: NativeBridge.VehSeatQueued(entity, seat) -> игрок (ptr) или 0 — посадка на это место ждёт тика моста.
/// У VU EnterVehicle синхронный, место занято сразу; у нас — пока запрос в очереди (без таймеров).
int l_bridge_veh_seat_queued(lua_State* L) {
    const unsigned long ent = (unsigned long)luaL_checkinteger(L, 1);
    const int seat = (int)luaL_checkinteger(L, 2);
    for (LONG i = 0; i < g_vehCount; ++i) {
        if (g_vehQueue[i].kind == 1 && g_vehQueue[i].entity == ent && g_vehQueue[i].seat == seat) {
            lua_pushinteger(L, (lua_Integer)g_vehQueue[i].player);
            return 1;
        }
    }
    lua_pushinteger(L, 0);
    return 1;
}

// ★ 238-6: солдат удалён (память не читается) или его список трансформа испорчен — движок не зовём, как VU отказывает
// на уничтоженной сущности («tried accessing an invalid or destroyed EntityBusPeer», EnterVehicle 0x1098B770). Прогон 238-6:
// BOT_Nicholas — солдат удалён через 0,9 с после посадки на место 2 Vodnik, наш exitEntry упал внутри движка (-2), движок
// остался на полпути, через 2,5 мин сервер упал на этом игроке (VEH 23:21:38, EBX = игрок, ESI = машина).
static LONG g_vehDeadSoldier = 0;
static bool SoldierUnsafeForVehicle(unsigned long player, const char* what) {
    unsigned long sol = 0;
    __try {
        sol = *(const unsigned long*)((const unsigned char*)(ULONG_PTR)player + kOffPlayerSoldier);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    if (sol == 0) return false;  // без солдата движок откажет сам (как раньше)
    const int bad = SoldierXformStackBad(sol);
    if (bad == 0) return false;
    if (InterlockedIncrement(&g_vehDeadSoldier) <= 40) {
        LOG_WARN("Bridge: %s игрока %p не вызван — солдат %08lX %s (код %d), как VU (уничтоженная сущность)", what,
                 (void*)(ULONG_PTR)player, sol, bad == 66 ? "удалён" : "с испорченным списком трансформа", bad);
    }
    return true;
}

static int CallEnginePlayerEnter(unsigned long player, unsigned long entity, int seat) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return -1;
    if (SoldierUnsafeForVehicle(player, "посадка")) return -3;
    using EnterFn = void(__fastcall*)(void*, void*, void*, int);
    XformCheckStep(player, "посадка", false);  // 238-6
    __try {
        ((EnterFn)(base + kEnterEntryRva))((void*)(ULONGLONG)player, nullptr, (void*)(ULONGLONG)entity, seat);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        XformCheckStep(player, "посадка (исключение движка)", true);
        return -2;
    }
    XformCheckStep(player, "посадка", true);
    return 1;
}

static int CallEnginePlayerExit(unsigned long player, int a, int b) {
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return -1;
    if (SoldierUnsafeForVehicle(player, "выход")) return -3;  // 238-6
    using ExitFn = void(__fastcall*)(void*, void*, int, int);
    XformCheckStep(player, "выход", false);  // 238-6
    __try {
        ((ExitFn)(base + kExitEntryRva))((void*)(ULONGLONG)player, nullptr, a, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        XformCheckStep(player, "выход (исключение движка)", true);
        return -2;
    }
    XformCheckStep(player, "выход", true);
    return 1;
}

/// Очередь посадки/выхода (хук тика). Каждый запрос — один вызов движка; результат читаем из полей игрока.
/// onlyPlayers != nullptr (сборка 196) — только запросы этих игроков (боты, получившие тело в этом же тике), остальные
/// остаются в очереди как были.
// Сборка 254: 1 — солдат «заспавнен» движком ([sol+0x0A] & 1, как у машины), 0 — ещё нет, -1 — гейт выключен/не сверен.
// Сверка смещения: у солдата человека (живого, давно в мире) бит должен стоять; нет — гейт не применяем (не блокируем посадки).
static int SoldierSpawnedState(unsigned long sol) {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VEH_SPAWNED_GATE", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;
    }
    if (s_on == 0 || sol == 0) return -1;
    static volatile LONG s_verified = 0;  // 0 — не сверено, 1 — смещение верное, -1 — неверное
    LONG ver = InterlockedCompareExchange(&s_verified, 0, 0);
    if (ver == 0) {
        const unsigned long human = ::evbots::EVBotsEngineRealPlayer();
        unsigned long hs = 0;
        unsigned char hb = 0;
        bool read = false;
        __try {
            if (human != 0) hs = *(const unsigned long*)((const unsigned char*)(ULONG_PTR)human + kOffPlayerSoldier);
            if (hs != 0) { hb = ((const unsigned char*)(ULONG_PTR)hs)[0x0A]; read = true; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            read = false;
        }
        if (!read) return -1;  // солдата человека нет — сверим позже, пока не мешаем
        ver = (hb & 1u) ? 1 : -1;
        InterlockedExchange(&s_verified, ver);
        LOG_INFO("Bridge: бит «заспавнен» солдата [+0x0A]&1 у человека (солдат %08lX) = %u — гейт посадки %s", hs, (unsigned)(hb & 1u),
                 ver > 0 ? "ВКЛ" : "ВЫКЛ (смещение не подтвердилось)");
    }
    if (ver < 0) return -1;
    __try {
        return (((const unsigned char*)(ULONG_PTR)sol)[0x0A] & 1u) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void VehicleTick(const unsigned long* onlyPlayers = nullptr, LONG onlyCount = 0) {
    if (LevelGateClosed()) return;
    LONG todo = g_vehCount;
    LONG idx = 0;
    while (todo-- > 0 && g_vehCount > idx) {
        if (onlyPlayers != nullptr) {
            bool mine = false;
            for (LONG k = 0; k < onlyCount; ++k) mine = mine || g_vehQueue[idx].player == onlyPlayers[k];
            if (!mine || g_vehQueue[idx].kind != 1) { ++idx; continue; }
        }
        VehReq r = g_vehQueue[idx];
        for (LONG i = idx + 1; i < g_vehCount; ++i) g_vehQueue[i - 1] = g_vehQueue[i];
        --g_vehCount;
        unsigned long a0 = 0, e0 = 0, c0 = 0;
        ReadPlayerVehicle(r.player, &a0, &e0, &c0);
        if (r.kind == 1 && a0 == r.entity && (int)e0 == r.seat) continue;  // уже сидит на этом месте
        // ★ Сборка 235: машина могла исчезнуть, пока запрос ждал тика — без проверки движок пишет за концом массива мест.
        if (r.kind == 1) {
            const char* bad = EnterTargetBad(r.entity, r.seat);
            if (bad != nullptr) {
                ++g_vehFails;
                if (g_vehBadLogged < 200) {
                    ++g_vehBadLogged;
                    LOG_INFO("ДИАГ техника: посадка игрок %08lX сущность %08lX место %d снята перед вызовом движка: %s",
                             r.player, r.entity, r.seat, bad);
                }
                continue;
            }
        }
        // ★ Сборка 211: место заняли, пока запрос ждал тика (человек сел раньше). Движок такую посадку не выполнит
        // (enterEntry 0x00E37A20) — запрос снимаем сразу, без вызова, как отказ VU EnterVehicle.
        if (r.kind == 1) {
            unsigned long occ = 0;
            if (SeatOccupant(r.entity, r.seat, &occ) && occ != 0 && occ != r.player) {
                ++g_vehFails;
                if (g_vehLogged < 60) {
                    ++g_vehLogged;
                    LOG_INFO("ДИАГ техника: посадка игрок %08lX сущность %08lX место %d — место занято игроком %08lX, посадка снята",
                             r.player, r.entity, r.seat, occ);
                }
                continue;
            }
        }
        // Мод зовёт EnterVehicle сразу при создании бота, пока тела в движке ещё нет (прогон 178: controlled=0): ждём тело
        // до ~150 тиков, потом отказ. Вход без тела движок не выполняет.
        if (r.kind == 1 && c0 == 0) {
            // ★ Сборка 195: у VU SpawnSoldierAt и EnterVehicle синхронны — fun-bots (BotSpawner, «SpawnAtVehicle»/«SpawnInAa»/
            // «SpawnInGunship») зовёт их подряд. У нас спавн ждёт в очереди хука (до 30 с), а посадка сгорала за 150 тиков:
            // прогон 194, BOT_Lew — «посадка не выполнена — тела нет» (01:48:59), тело через 13 с на HQ, без машины.
            // Пока спавн бота в очереди, попытки посадки не тратим: сядет в первом тике с телом.
            bool spawnQueued = false;
            for (LONG q = 0; q < g_respawnCount; ++q) spawnQueued = spawnQueued || g_respawnQueue[q] == r.player;
            if (!spawnQueued) ++r.tries;
            if (r.tries < 150 && g_vehCount < 32) g_vehQueue[g_vehCount++] = r;
            else { ++g_vehFails; if (g_vehLogged < 60) { ++g_vehLogged; LOG_INFO("ДИАГ техника: посадка игрок %08lX не выполнена — тела нет", r.player); } }
            continue;
        }
        // ★ Сборка 236: не сажаем в том же тике, где мост ставил солдату позицию (setTransform) — посадка следующим тиком.
        ULONGLONG xfAgo = 0;
        bool xfSame = false;
        const bool xfHad = (r.kind == 1) && BotXformMark(r.player, &xfAgo, &xfSame);
        if (xfHad && xfSame) {
            if (g_vehCount < 32) g_vehQueue[g_vehCount++] = r;
            static volatile LONG s_deferLog = 0;
            if (InterlockedIncrement(&s_deferLog) <= 60) {
                LOG_INFO("ДИАГ техника: посадка игрок %08lX отложена на тик — в этом тике мост ставил позицию (setTransform)", r.player);
            }
            continue;
        }
        // ★ Сборка 254: посадка солдата, которого движок ещё не довёл до «заспавнен». Прогон 253 (XP4_FD, после kill all):
        // у 7 ботов, севших в технику через 0,2–2 с после тела (посадка в тике спавна, 196), испортился список трансформа
        // [+0x394]; 35 посадок через >3 с после тела — целы; через 6 с падение 0xE0B050 (узел = 1.0f). Пошаговая проверка
        // (238-6) после наших вызовов порчи не видела — портит обновление движка после посадки «сырого» солдата. Ждём бит
        // движка, как у машины (EnterTargetBad: [ent+0x0A] & 1), — событие, не таймер. Смещение у солдата сверяем на живом
        // солдате человека: у него бит не стоит — гейт выключен (лог). EVBOTS_VEH_SPAWNED_GATE=0 — выкл.
        // ★ Сборка 255: бот без своего объекта ввода (+0x3C0 = 0) в технику не садится. Факт: у всех 8 ботов с испорченным
        // списком трансформа (253 — 7, 254 — 1) ввода не было (таблица ввода полна после kill all); механизм порчи в движке —
        // не разобран (гипотеза: ветка без ввода). Ждём ввод до 300 тиков, потом посадка снимается, как отказ VU.
        if (r.kind == 1 && c0 != 0) {
            bool ourBot = false;
            for (LONG k = 0; k < g_watchCount && k < 64; ++k) ourBot = ourBot || g_watchBot[k] == r.player;
            unsigned long in3c0 = 1;
            __try {
                in3c0 = *(const unsigned long*)((const unsigned char*)(ULONG_PTR)r.player + 0x3C0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                in3c0 = 1;
            }
            if (ourBot && in3c0 == 0) {
                ++r.tries;
                if (r.tries < 300 && g_vehCount < 32) {
                    g_vehQueue[g_vehCount++] = r;
                    static volatile LONG s_noInLog = 0;
                    if (r.tries == 1 && InterlockedIncrement(&s_noInLog) <= 60)
                        LOG_INFO("ДИАГ техника: посадка бота %08lX отложена — нет своего объекта ввода (+0x3C0 = 0)", r.player);
                } else {
                    ++g_vehFails;
                    LOG_WARN("ДИАГ техника: посадка бота %08lX снята — без своего объекта ввода %d тиков", r.player, r.tries);
                }
                continue;
            }
        }
        if (r.kind == 1 && c0 != 0) {
            const int sp = SoldierSpawnedState(c0);
            if (sp == 0) {
                ++r.tries;
                if (r.tries < 300 && g_vehCount < 32) {
                    g_vehQueue[g_vehCount++] = r;
                    static volatile LONG s_rawLog = 0;
                    if (r.tries == 1 && InterlockedIncrement(&s_rawLog) <= 60) {
                        LOG_INFO("ДИАГ техника: посадка игрок %08lX — солдат %08lX ещё не «заспавнен» движком, ждём", r.player, c0);
                    }
                } else {
                    ++g_vehFails;
                    LOG_WARN("ДИАГ техника: посадка игрок %08lX снята — солдат %08lX так и не «заспавнен» за %d тиков", r.player, c0, r.tries);
                }
                continue;
            }
        }
        const int res = (r.kind == 1) ? CallEnginePlayerEnter(r.player, r.entity, r.seat)
                                      : CallEnginePlayerExit(r.player, r.flagA, r.flagB);
        unsigned long a1 = 0, e1 = 0, c1 = 0;
        ReadPlayerVehicle(r.player, &a1, &e1, &c1);
        if (res == 1) {
            if (r.kind == 1) ++g_vehEnters; else ++g_vehExits;
        } else {
            ++g_vehFails;
        }
        // сборка 235: было 60 строк на весь сервер — к падению 234-г (через 11 мин) журнал посадок уже молчал
        if (g_vehLogged < 400) {
            ++g_vehLogged;
            // сборка 236: когда мост последний раз ставил позицию. ★ 237: было char[48] + sprintf_s — с ≥ 1000 мс строка
            // (кириллица — 2 байта) не влезала, sprintf_s завершал процесс мгновенно (fail-fast CRT, без VEH и дампа):
            // беззвучные падения сервера 236-2 (12:35:13) и 237 (12:51:10) — спавн бота с посадкой в технику.
            char xf[128] = "";
            if (xfHad) ::_snprintf_s(xf, sizeof(xf), _TRUNCATE, " | setTransform моста %llu мс назад", xfAgo);
            LOG_INFO("ДИАГ техника: %s игрок %08lX сущность %08lX место %d -> %d | было attached=%08lX место=%lu controlled=%08lX | стало attached=%08lX место=%lu controlled=%08lX%s",
                     r.kind == 1 ? "посадка" : "выход", r.player, r.entity, r.seat, res, a0, e0, c0, a1, e1, c1, xf);
        }
    }
}

// ── Сборка 133: попадания по солдатам (диагностика, поведение не меняет) ─────────────────────────────────
// Раньше «урон» мерили счётчиком движка (ClientDamageListener, см. MODLOG 131), а «раненые» — снимком. Теперь каждый тик
// читаем HP бота ([солдат+0x34], float) и считаем падения: падение = попадание, HP до 0 = смерть. Жертва с позой (cur
// на момент попадания) — чтобы видеть, попадают ли в присевших и лежащих.
struct HpSeen { unsigned long pl; float hp; };
static HpSeen g_hpSeen[64];
static LONG g_hitEvents = 0, g_hitKills = 0, g_hitStand = 0, g_hitCrouch = 0, g_hitProne = 0;
static float g_hitDmg = 0.0f;

/// Тик: падение HP бота из g_watchBot — попадание; HP ≤ 0 — смерть. Новый бот в слоте — только запоминаем HP.
static void HitTick() {
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
        const unsigned long pl = g_watchBot[i];
        float hp = -1.0f;
        int cur = -1;
        int next = -1;
        __try {
            const unsigned char* sol = *(const unsigned char* const*)((const unsigned char*)(ULONG_PTR)pl + kOffPlayerSoldier);
            if (sol != nullptr) hp = *(const float*)(sol + 0x34);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hp = -1.0f;
        }
        HpSeen& s = g_hpSeen[i];
        if (s.pl != pl) {
            s.pl = pl;
            s.hp = hp;
            continue;
        }
        if (hp < 0.0f) {
            s.hp = -1.0f;
            continue;
        }
        if (s.hp > 0.0f && hp < s.hp - 0.01f) {
            PushDmg(pl, 0, s.hp - (hp > 0.0f ? hp : 0.0f), hp);  // сборка 153: урон вне перехватов — стрелок неизвестен
            ++g_hitEvents;
            g_hitDmg += s.hp - (hp > 0.0f ? hp : 0.0f);
            if (hp <= 0.0f) ++g_hitKills;
            PoseReadObj(PoseObjectOf(pl), &cur, &next);
            if (cur == 0) ++g_hitStand;
            else if (cur == 1) ++g_hitCrouch;
            else if (cur == 2) ++g_hitProne;
        }
        s.hp = hp;
    }
}

// Сборка 236: раз в 2 с — целостность списка трансформа у солдат ботов. Первое обнаружение по боту — в журнал (когда
// испортилось: после спавна, посадки, телепорта), чтобы найти, кто портит. Поведение не меняет.
static void XformStackTick() {
    static ULONGLONG s_at = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now - s_at < 2000ULL) return;
    s_at = now;
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
        const unsigned long pl = g_watchBot[i];
        unsigned long sol = 0, att = 0, seat = 0;
        __try {
            const unsigned char* p = (const unsigned char*)(ULONG_PTR)pl;
            sol = *(const unsigned long*)(p + kOffPlayerSoldier);
            att = *(const unsigned long*)(p + kOffPlayerAttached);
            seat = *(const unsigned long*)(p + kOffPlayerEntryId);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        const int bad = SoldierXformStackBad(sol);
        if (bad == 0) continue;
        bool seen = false;
        for (LONG k = 0; k < g_xformBadSeenCount; ++k) seen = seen || g_xformBadSeen[k] == sol;
        if (seen) continue;
        if (g_xformBadSeenCount < 64) g_xformBadSeen[g_xformBadSeenCount++] = sol;
        else { g_xformBadSeen[0] = sol; }
        // код: 1..64 — номер узла со сломанной ссылкой, 65 — кольцо длиннее 64 узлов, 66 — память не читается
        LOG_ERR("Bridge: ДИАГ список трансформа испорчен: бот %p солдат %08lX код %d | техника %08lX место %lu",
                (void*)(ULONG_PTR)pl, sol, bad, att, seat);
    }
}

/// Диагностика позы (раз в 20 с, поведение не меняет): сколько ботов стоят / сидят / лежат по движку.
static void PoseDiagTick() {
    static ULONGLONG s_at = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (s_at == 0) {
        s_at = now;
        return;
    }
    if (now - s_at < 20000) return;
    LONG cnt[3] = {0, 0, 0};
    LONG none = 0;
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
        int cur = -1;
        int next = -1;
        if (PoseReadObj(PoseObjectOf(g_watchBot[i]), &cur, &next) && cur >= 0) {
            ++cnt[cur];
        } else {
            ++none;
        }
    }
    LOG_INFO("ДИАГ поза: стоя %ld, присед %ld, лёжа %ld, без солдата %ld; запросов %ld, принято движком %ld, отказов %ld, без объекта %ld, в очереди %ld",
             (long)cnt[0], (long)cnt[1], (long)cnt[2], (long)none, (long)g_poseReqs, (long)g_poseOk,
             (long)g_poseRefused, (long)g_poseNoObj, (long)g_poseCount);
    LOG_INFO("ДИАГ попадания по солдатам (по HP ботов): попаданий %ld (в стоящих %ld, присевших %ld, лежащих %ld), урон %.0f HP, смертей %ld",
             (long)g_hitEvents, (long)g_hitStand, (long)g_hitCrouch, (long)g_hitProne, (double)g_hitDmg, (long)g_hitKills);
    g_hitEvents = g_hitKills = g_hitStand = g_hitCrouch = g_hitProne = 0;
    g_hitDmg = 0.0f;
    s_at = now;
}

// ── Сборка 136: weaponsComponent как в VU (SoldierWeaponsComponent: currentWeaponSlot, weapons[i].primaryAmmo/secondaryAmmo)
// Мод выбирает оружие по currentWeaponSlot (BotWeaponHandling.lua:53–125): у нас поле было nil, и мод каждый тик жал
// «выбрать оружие» и сбрасывал таймер выстрела (_ShotTimer = -0.05) — боты почти не стреляли. Мод сам держит запас
// полным (secondaryAmmo = primaryAmmo + 3) и решает перезарядку по primaryAmmo — нужна запись.
// Пути движка (re\aim\report.md): комп = [солдат+0x53C]; текущий слот = [[комп+0x68]+4]; оружие = [[комп+0x30]+слот*4];
// WF = [оружие+0x128]; магазин int [WF+0xF4]; запас int [WF+0xF8]. Слоты = WeaponSlot VU (0..9).
static unsigned long WeaponCompOf(unsigned long player) {
    if (player == 0) return 0;
    unsigned long comp = 0;
    __try {
        const unsigned long soldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)player + kOffPlayerSoldier);
        if (soldier != 0) comp = *(const unsigned long*)((const unsigned char*)(ULONGLONG)soldier + 0x53C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return comp;
}

/// WeaponFiring слота (0, если оружия в слоте нет).
static unsigned long WeaponFiringOf(unsigned long comp, int slot) {
    if (comp == 0 || slot < 0 || slot > 9) return 0;
    unsigned long wf = 0;
    __try {
        const unsigned long arr = *(const unsigned long*)((const unsigned char*)(ULONGLONG)comp + 0x30);
        if (arr != 0) {
            const unsigned long w = *(const unsigned long*)((const unsigned char*)(ULONGLONG)arr + slot * 4);
            if (w != 0) wf = *(const unsigned long*)((const unsigned char*)(ULONGLONG)w + 0x128);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return wf;
}

// ★ Сборка 186: GunSway оружия, как VU (soldier.weaponsComponent.currentWeapon.weaponFiring.gunSway) — мод компенсирует
// отдачу по currentRecoilDeviation + currentDispersionDeviation (BotAiming.lua:112–140), у нас было 0. Разбор exe (18.10):
// sway = [WF+0xA0] (vtable 0x02649830, ctor 0x011423B0); Deviation = 4 float (pitch, yaw, roll, transY);
// recoil +0xAC, dispersion +0x12C. Сервер обновляет (WeaponFiring::update 0x008B7970 → [[WF+0xA0]+0x5C]).
/// NativeBridge.GunSway(ptr, slot) -> recoil pitch, yaw, roll, transY, dispersion pitch, yaw, roll, transY | nil
int l_bridge_gun_sway(lua_State* L) {
    const unsigned long comp = WeaponCompOf((unsigned long)luaL_checkinteger(L, 1));
    const unsigned long wf = WeaponFiringOf(comp, (int)luaL_checkinteger(L, 2));
    if (wf == 0) return 0;
    float v[8] = {};
    __try {
        const unsigned long sway = *(const unsigned long*)((const unsigned char*)(ULONGLONG)wf + 0xA0);
        if (sway == 0 || *(const unsigned long*)(ULONGLONG)sway != 0x02649830UL) return 0;
        for (int i = 0; i < 4; ++i) v[i] = *(const float*)((const unsigned char*)(ULONGLONG)sway + 0xAC + i * 4);
        for (int i = 0; i < 4; ++i) v[4 + i] = *(const float*)((const unsigned char*)(ULONGLONG)sway + 0x12C + i * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    for (int i = 0; i < 8; ++i) lua_pushnumber(L, (lua_Number)v[i]);
    return 8;
}

/// NativeBridge.WeaponCurSlot(ptr) -> слот 0..9 | nil
int l_bridge_weapon_cur_slot(lua_State* L) {
    const unsigned long comp = WeaponCompOf((unsigned long)luaL_checkinteger(L, 1));
    unsigned long slot = 0xFFFFFFFFUL;
    if (comp != 0) {
        __try {
            const unsigned long s = *(const unsigned long*)((const unsigned char*)(ULONGLONG)comp + 0x68);
            if (s != 0) slot = *(const unsigned long*)((const unsigned char*)(ULONGLONG)s + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            slot = 0xFFFFFFFFUL;
        }
    }
    if (slot <= 9) lua_pushinteger(L, (lua_Integer)slot); else lua_pushnil(L);
    return 1;
}

/// Объект оружия в слоте (0, если слот пуст).
static unsigned long WeaponObjOf(unsigned long comp, int slot) {
    if (comp == 0 || slot < 0 || slot > 9) return 0;
    unsigned long w = 0;
    __try {
        const unsigned long arr = *(const unsigned long*)((const unsigned char*)(ULONGLONG)comp + 0x30);
        if (arr != 0) w = *(const unsigned long*)((const unsigned char*)(ULONGLONG)arr + slot * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return w;
}

/// NativeBridge.WeaponAmmo(ptr, slot) -> магазин, запас | nil (слот пуст). Оружие без WeaponFiring (гранаты/нож
/// по этому пути, сборка 136-фикс: мод падал на weapons[7] == nil, BotAttacking.lua:172) — есть, патроны 0.
int l_bridge_weapon_ammo(lua_State* L) {
    const unsigned long comp = WeaponCompOf((unsigned long)luaL_checkinteger(L, 1));
    const int slot = (int)luaL_checkinteger(L, 2);
    if (WeaponObjOf(comp, slot) == 0) { lua_pushnil(L); return 1; }
    const unsigned long wf = WeaponFiringOf(comp, slot);
    if (wf == 0) { lua_pushinteger(L, 0); lua_pushinteger(L, 0); return 2; }
    int mag = 0, res = 0;
    __try {
        mag = *(const int*)((const unsigned char*)(ULONGLONG)wf + 0xF4);
        res = *(const int*)((const unsigned char*)(ULONGLONG)wf + 0xF8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, mag);
    lua_pushinteger(L, res);
    return 2;
}

/// NativeBridge.SetWeaponAmmo(ptr, slot, магазин|nil, запас|nil) -> true, если записано
// ★ Сборка 185: ёмкость магазина — как primaryAmmoMagCapacity движка (0x0089F1E0): [wf+0x114], при −1 — [[[wf+0x12C]+0xD0]+8],
// иначе [[wf+0x98]]. Ночной прогон 18.10: assert движка «Magazine with capacity 7 contains 8 projectiles» (WeaponFiring.cpp:1207)
// и «Mags 1, -1» (:1448) — мод пишет primaryAmmo больше ёмкости, мы писали как есть. В VU движок такого не принимает.
static int WeaponMagCapacity(unsigned long wf) {
    __try {
        int cap = *(const int*)((const unsigned char*)(ULONGLONG)wf + 0x114);
        if (cap != -1) return cap;
        const unsigned long a = *(const unsigned long*)((const unsigned char*)(ULONGLONG)wf + 0x12C);
        if (a != 0) {
            const unsigned long b = *(const unsigned long*)((const unsigned char*)(ULONGLONG)a + 0xD0);
            if (b != 0) return *(const int*)((const unsigned char*)(ULONGLONG)b + 8);
        }
        const unsigned long c = *(const unsigned long*)((const unsigned char*)(ULONGLONG)wf + 0x98);
        return c != 0 ? *(const int*)(ULONGLONG)c : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int l_bridge_set_weapon_ammo(lua_State* L) {
    const unsigned long wf = WeaponFiringOf(WeaponCompOf((unsigned long)luaL_checkinteger(L, 1)),
                                            (int)luaL_checkinteger(L, 2));
    const bool hasMag = !lua_isnoneornil(L, 3);
    const bool hasRes = !lua_isnoneornil(L, 4);
    int mag = hasMag ? (int)luaL_checkinteger(L, 3) : 0;
    if (hasMag && wf != 0) {
        const int cap = WeaponMagCapacity(wf);
        if (cap > 0 && cap < 1000 && mag > cap) mag = cap;
    }
    const int res = hasRes ? (int)luaL_checkinteger(L, 4) : 0;
    bool ok = false;
    if (wf != 0 && mag >= 0 && res >= 0 && mag < 1000 && res < 100000) {
        __try {
            if (hasMag) *(int*)((unsigned char*)(ULONGLONG)wf + 0xF4) = mag;
            if (hasRes) *(int*)((unsigned char*)(ULONGLONG)wf + 0xF8) = res;
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/// NativeBridge.TeleportBot(ptr, x, y, z) -> true: переносит живого бота в точку (сборка 110, опыт). Запись и очередь:
/// сам вызов движка выполняет хук тика (RetryBotPositions), из потока Lua его нельзя.
int l_bridge_teleport_bot(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    if (!NonFiniteGuard(luaL_checknumber(L, 2) + luaL_checknumber(L, 3) + luaL_checknumber(L, 4), "TeleportBot", 0)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    RememberBotPos(ptr, (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    QueueBotPosition(ptr);
    lua_pushboolean(L, 1);
    return 1;
}

/// NativeBridge.SetBotPos(ptr, x, y, z) -> nil: позиция бота, которую мод передал до появления движкового игрока
int l_bridge_set_bot_pos(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    if (!NonFiniteGuard(luaL_checknumber(L, 2) + luaL_checknumber(L, 3) + luaL_checknumber(L, 4), "SetBotPos", 0)) return 0;
    RememberBotPos(ptr, (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    return 0;
}

// ★ Удаление бота (сборка 82). Мод удаляет лишних ботов (Bot:Destroy -> PlayerManager:DeletePlayer), а у нас
// удаление было только в Lua: игрок оставался в движке «призраком», мод создавал нового -> в игре на одного больше
// (прогон 81: «нас 17 вместо 16»). Настоящий вызов: fb::ServerPlayerManager::deletePlayer (VA 0x00E67460, thiscall,
// 1 стековый, ret 4; разбор re\deleteplayer_decomp.txt). Вызываем из хука тика (RetryPending), до update менеджера.
constexpr unsigned long kNativeDeletePlayerRva = 0x00A67460UL;
constexpr unsigned long kPlayerLeftGameRva = 0x00A378E0UL;  // сборка 175: VA 0x00E378E0 ServerPlayer «покинул игру» (thiscall)
constexpr int kOffPlayerInGame = 0x3EC;                     // m_isIngame (байт): 1 — 0x00E351AD, 0 — 0x00E3790D
constexpr LONG kDeleteQueueCap = 64;  // сборка 170: было 32 — при 64 игроках мод удаляет больше за кадр
static unsigned long g_deleteQueue[kDeleteQueueCap];
static LONG g_deleteCount = 0;
// ★ Сборка 187: выход из отряда движка перед удалением — fb leaveSquad(ServerPlayer*, team) VA 0x0118D590, cdecl
// (ret без операнда; team 0 — текущая команда +0x31C). Так её зовёт сам движок: addToTeam перед сменой команды
// (0x0118F1D9) и обработчик 0x013263F0 (`push 0; push игрок; call`). Внутри: отряд игрока (0x011754A0) → снять игрока
// с отряда (0x01176220), сброс подобъекта отряда. Без этого запись отряда держала удалённого игрока: AV 0x00D489CB
// (`[запись+4]` → vtbl[3], сборка 141/142; что запись — отряда, гипотеза разбора 145). Разбор: re\squad_decomp.txt,
// MODLOG «Сборка 187».
constexpr unsigned long kLeaveSquadRva = 0x00D8D590UL;
constexpr unsigned long kTeamsBeginRva = 0x0245BB38UL;  // VA 0x0285BB38: начало массива команд (8 байт на команду)
constexpr unsigned long kTeamsEndRva = 0x0245BB3CUL;    // VA 0x0285BB3C: конец (так считает addToTeam 0x0118EF80)

// Команда игрока (+0x31C) есть в массиве команд движка (1..N−1), как проверка «Invalid team» в joinSquad 0x01189D60.
static bool EngineTeamInList(unsigned long player, unsigned int* teamOut, unsigned int* teamsOut) {
    unsigned char* b = NativeModuleBase();
    unsigned int team = 0, teams = 0;
    if (b != nullptr && player != 0) {
        __try {
            team = *(const unsigned int*)((const unsigned char*)(ULONGLONG)player + kOffPlayerTeam);
            teams = (*(const unsigned long*)(b + kTeamsEndRva) - *(const unsigned long*)(b + kTeamsBeginRva)) >> 3;
        } __except (EXCEPTION_EXECUTE_HANDLER) { team = 0; teams = 0; }
    }
    if (teamOut != nullptr) *teamOut = team;
    if (teamsOut != nullptr) *teamsOut = teams;
    return team != 0 && team < teams;
}
// Боты без тела, которых мост «забыл» (сборка 145): из наблюдения сняты, но в движке живы — мод их ещё удалит
// (DestroyDisabledBots → DeletePlayer). VU удаляет игрока полностью; до 187 они оставались призраками в счёте и
// при смене SQDM → CQ, по разбору, давали FatalAssert «Invalid team 4. Num of teams: 3» (RuntimeLog 21:36:37, связь
// с призраками — гипотеза).
static unsigned long g_forgottenBots[64];
static LONG g_forgottenCount = 0;
static bool IsForgottenEngineBot(unsigned long ptr) {
    for (LONG i = 0; i < g_forgottenCount && i < 64; ++i) {
        if (g_forgottenBots[i] == ptr) return true;
    }
    return false;
}
static void DropForgottenEngineBot(unsigned long ptr) {
    for (LONG i = 0; i < g_forgottenCount;) {
        if (g_forgottenBots[i] == ptr) { g_forgottenBots[i] = g_forgottenBots[g_forgottenCount - 1]; --g_forgottenCount; } else { ++i; }
    }
}
bool BotDeleteEnabled() {
    static const bool on = [] {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_BOT_DELETE", v, (DWORD)sizeof(v));
        return !(n == 1 && v[0] == '0');
    }();
    return on;
}

/// NativeBridge.DeleteEnginePlayer(ptr) -> true, если бот поставлен в очередь на удаление в движке
int l_bridge_delete_player(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    bool tracked = false;
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
        if (g_watchBot[i] == ptr) { tracked = true; break; }
    }
    if (!tracked) tracked = IsForgottenEngineBot(ptr);  // сборка 187: бот без тела, забытый мостом, — тоже удаляем
    if (!BotDeleteEnabled() || ptr == 0 || !tracked) {
        lua_pushboolean(L, 0);
        return 1;
    }
    for (LONG i = 0; i < g_deleteCount; ++i) {
        if (g_deleteQueue[i] == ptr) { lua_pushboolean(L, 1); return 1; }
    }
    if (g_deleteCount < kDeleteQueueCap) g_deleteQueue[g_deleteCount++] = ptr;
    LOG_INFO("Bridge: бот %p поставлен в очередь на удаление в движке", (void*)ptr);
    lua_pushboolean(L, 1);
    return 1;
}

/// NativeBridge.ClearBotKits() -> сколько игроков-ботов отпустили кит (+0x480)
/// Сборка 265: перед выгрузкой уровня движок уничтожает игроков-ботов уже после того, как контейнеры уровня
/// освобождены; деструктор игрока отпускает +0x480 и падает на нулевом vtable (друг, 07.11, EIP 0x468892).
/// Отпускаем ссылку заранее, пока кит жив; на новом уровне спавн восстановит его из эталона/базы движка.
int l_bridge_clear_bot_kits(lua_State* L) {
    static const bool enabled = [] {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_CLEAR_KITS", v, (DWORD)sizeof(v));
        const bool on = !(n == 1 && v[0] == '0');
        if (!on) LOG_INFO("Bridge: отпускание кита +0x480 перед выгрузкой уровня ВЫКЛЮЧЕНО (EVBOTS_CLEAR_KITS=0)");
        return on;
    }();
    if (!enabled) { lua_pushinteger(L, 0); return 1; }
    unsigned long seen[128];
    int seenN = 0, cleared = 0, dead = 0, zero = 0;
    auto visit = [&](unsigned long pl) {
        if (pl == 0) return;
        for (int i = 0; i < seenN; ++i) if (seen[i] == pl) return;
        if (seenN < 128) seen[seenN++] = pl;
        unsigned long obj = 0;
        __try {
            obj = *(const unsigned long*)((const unsigned char*)(ULONGLONG)pl + kOffPlayerKit);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        if (obj == 0) { ++zero; return; }
        bool alive = false;
        __try {
            alive = *(const void**)(ULONGLONG)obj != nullptr;  // vtable живого объекта
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            alive = false;
        }
        if (!alive) { ++dead; return; }  // контейнер уже разрушен — release его добьёт
        if (SetPlayerRef480(pl, 0)) ++cleared;
    };
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) visit(g_watchBot[i]);
    for (LONG i = 0; i < g_forgottenCount && i < 64; ++i) visit(g_forgottenBots[i]);
    LOG_INFO("Bridge: кит +0x480 отпущен у ботов: %d (без кита %d, контейнер уже мёртв %d; игроков %d)",
             cleared, zero, dead, seenN);
    lua_pushinteger(L, cleared);
    return 1;
}

// ★ Сборка 145: бот без тела, которого убрал мод (SoldierEntity:Kill для noBody). Из движка не удаляем (падение 06:45,
// сборка 141), но мост обязан его забыть: иначе отложенный спавн выполнялся уже после удаления из Lua — бот получал
// тело и addToTeam (прогон 144: BOT_Christian, 11:21:40 — «17-й игрок», солдат без управления).
static unsigned long g_forgetQueue[32];
static LONG g_forgetCount = 0;
static bool InForgetQueue(unsigned long ptr) {  // сборка 153: бот уже убран модом, «забыть» ещё не обработано
    for (LONG i = 0; i < g_forgetCount && i < 32; ++i) {
        if (g_forgetQueue[i] == ptr) return true;
    }
    return false;
}

/// NativeBridge.ResetRoundStats() -> сколько игроков обнулили (боты из наблюдения + человек)
int l_bridge_reset_round_stats(lua_State* L) {
    static const bool enabled = [] {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_RESET_STATS", v, (DWORD)sizeof(v));
        const bool on = !(n == 1 && v[0] == '0');
        if (!on) LOG_INFO("Bridge: сброс раундовой статистики ВЫКЛЮЧЕН (EVBOTS_RESET_STATS=0)");
        return on;
    }();
    if (!enabled) { lua_pushinteger(L, 0); return 1; }
    unsigned long seen[160];
    int seenN = 0, bots = 0, human = 0;
    auto visit = [&](unsigned long pl, int& counter) {
        if (pl == 0) return;
        for (int i = 0; i < seenN; ++i) if (seen[i] == pl) return;
        if (seenN < 160) seen[seenN++] = pl;
        if (ResetPlayerRoundStats(pl)) ++counter;
    };
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) visit(g_watchBot[i], bots);
    for (LONG i = 0; i < g_forgottenCount && i < 64; ++i) visit(g_forgottenBots[i], bots);
    visit(::evbots::EVBotsEngineRealPlayer(), human);
    LOG_INFO("Bridge: статистика раунда обнулена: ботов %d, человек %d (игроков %d)", bots, human, seenN);
    lua_pushinteger(L, bots + human);
    return 1;
}

/// NativeBridge.ForgetEngineBot(ptr) -> true, если бот поставлен в очередь «забыть» (без deletePlayer)
int l_bridge_forget_bot(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    if (ptr == 0) { lua_pushboolean(L, 0); return 1; }
    for (LONG i = 0; i < g_forgetCount; ++i) {
        if (g_forgetQueue[i] == ptr) { lua_pushboolean(L, 1); return 1; }
    }
    if (g_forgetCount < 32) g_forgetQueue[g_forgetCount++] = ptr;
    LOG_INFO("Bridge: бот без тела %p поставлен в очередь «забыть»", (void*)ptr);
    lua_pushboolean(L, 1);
    return 1;
}

/// NativeBridge.ClearBotPos(ptr): забыть позицию бота (движок сменил ему команду — точка была для старой стороны)
int l_bridge_clear_bot_pos(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    for (LONG i = 0; i < g_botPosCount; ++i) {
        if (g_botPos[i].ptr == ptr) {
            g_botPos[i] = g_botPos[g_botPosCount - 1];
            --g_botPosCount;
            break;
        }
    }
    return 0;
}

// ★ Сборка 98: оружие бота = выбор мода. Мод зовёт soldier:ApplyCustomization(CustomizeSoldierData) сразу после
// постановки спавна в очередь, поэтому мост успевает: запоминает объекты оружия по слотам и пишет их в вектор слотов
// бота (+0x50C, элемент 0x24: +0 ассет, +4..+0x1F навесы, +0x20 их число) при спавне, поверх копии слотов человека.
// Движок на спавне сам создаёт оружие из этого вектора (applyCustomization).
static unsigned long long QpcNow();   // сборка 161: замер нативного поиска ассетов
static unsigned long long QpcFreq();
// Сборка 152: + навесы (до 7 на слот, как в элементе вектора; живое чтение 136L: +4.. объекты UnlockAsset
// vtable 0x02504B1C, например U_M4A1_RX01 / NoPrimaryAccessory / NoSecondaryAccessory / NoCamo, +0x20 = их число).
constexpr unsigned long kWpnMaxAtt = 7;
constexpr unsigned long kUnlockAssetVtbl = 0x02504B1CUL;  // UnlockAsset (навес), сервер без ASLR
struct BotWeaponsReq {
    unsigned long ptr;
    unsigned long obj[kWpnSlots];
    unsigned long att[kWpnSlots][kWpnMaxAtt];
    unsigned long attN[kWpnSlots];
};
static BotWeaponsReq g_botWeapons[64];
static LONG g_botWeaponsCount = 0;

// ★ Сборка 152: источник оружия бота. EVBOTS_BOT_WEAPONS: не задан — оружие мода с навесами (как VU);
// «human» — копия слотов человека, оружие мода не пишется (как 136); «ab» — опыт: боты по очереди создания
// группа А (как 136) / Б (мод с навесами) — проверка, от чего пропадает иконка перка и появляется ник.
struct BotWpnGroup { unsigned long ptr; char group; };
static BotWpnGroup g_wpnGroup[128];
static LONG g_wpnGroupCount = 0;
static char BotWeaponGroup(unsigned long ptr) {
    static int s_mode = -1;  // 0 мод, 1 человек, 2 опыт А/Б
    if (s_mode < 0) {
        char v[16] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_BOT_WEAPONS", v, sizeof(v));
        s_mode = (n > 0 && ::_stricmp(v, "human") == 0) ? 1 : ((n > 0 && ::_stricmp(v, "ab") == 0) ? 2 : 0);
        LOG_INFO("Bridge: оружие ботов — %s (EVBOTS_BOT_WEAPONS)",
                 s_mode == 1 ? "копия слотов человека, как 136" : (s_mode == 2 ? "ОПЫТ А/Б: А — как 136, Б — мод с навесами" : "оружие мода с навесами"));
    }
    if (s_mode == 0) return 'B';
    if (s_mode == 1) return 'A';
    for (LONG i = 0; i < g_wpnGroupCount; ++i) {
        if (g_wpnGroup[i].ptr == ptr) return g_wpnGroup[i].group;
    }
    const char g = (g_wpnGroupCount % 2 == 0) ? 'A' : 'B';
    if (g_wpnGroupCount < 128) { g_wpnGroup[g_wpnGroupCount].ptr = ptr; g_wpnGroup[g_wpnGroupCount].group = g; ++g_wpnGroupCount; }
    LOG_INFO("Bridge: ОПЫТ А/Б: бот %p — группа %c (%s)", (void*)(ULONGLONG)ptr, g,
             g == 'A' ? "оружие человека, как 136" : "оружие мода с навесами");
    return g;
}

static unsigned long FindAssetByName(unsigned long kit, const char* name) {
    (void)kit;
    // ★ Сборка 166–169: только база ресурсов движка, как VU (хеш раздела, микросекунды). Промах — ассета нет в
    // загруженных разделах. Обход кита (стоил 4–6 с в 165) и скан памяти (до 200 мс) удалены в 169.
    const unsigned long long t0 = QpcNow();
    const unsigned long nat = evbots::LiveFindAssetByName(name);
    const double us = (double)(QpcNow() - t0) * 1e6 / (double)QpcFreq();
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 60 || nat == 0) {
        LOG_INFO("Bridge: АССЕТ «%s» — база движка: %p (%.1f мкс)", name, (void*)(ULONG_PTR)nat, us);
    }
    return nat;
}

static void ApplyBotWeaponsVector(unsigned long ptr) {
    const BotWeaponsReq* req = nullptr;
    for (LONG i = 0; i < g_botWeaponsCount; ++i) {
        if (g_botWeapons[i].ptr == ptr) { req = &g_botWeapons[i]; break; }
    }
    if (req == nullptr || ptr == 0) return;
    unsigned long begin = 0, end = 0;
    __try {
        begin = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffWpnBegin);
        end = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffWpnEnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    // Сборка 125: вектор пуст (тело до дислокации человека — шаблона слотов нет): растим до 10 слотов движковым
    // resize и обнуляем; пустой слот у человека = указатель 0 (память сервера, прогон 124).
    if (begin == 0 || end <= begin) {
        unsigned char* base = NativeModuleBase();
        if (base == nullptr) return;
        using ResizeFn = void(__fastcall*)(void*, void*, unsigned long);
        __try {
            ((ResizeFn)((ULONGLONG)base + kWpnResizeRva))((void*)(ULONGLONG)(ptr + kOffWpnBegin), nullptr, kWpnSlots);
            begin = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffWpnBegin);
            end = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffWpnEnd);
            if (begin == 0 || end < begin + kWpnSlots * kWpnEntrySize) {
                LOG_ERR("Bridge: ОРУЖИЕ бота %p: вектор не вырос (begin=%08lX end=%08lX)", (void*)(ULONGLONG)ptr, begin, end);
                return;
            }
            ::memset((void*)(ULONGLONG)begin, 0, kWpnSlots * kWpnEntrySize);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERR("Bridge: ОРУЖИЕ бота %p: resize — исключение", (void*)(ULONGLONG)ptr);
            return;
        }
        LOG_INFO("Bridge: ОРУЖИЕ бота %p: слотов не было — создано %lu пустых", (void*)(ULONGLONG)ptr, kWpnSlots);
    }
    const unsigned long n = (end - begin) / kWpnEntrySize;
    int written = 0, attWritten = 0;
    int stale = 0;
    for (unsigned long s = 0; s < kWpnSlots && s < n; ++s) {
        if (req->obj[s] == 0) continue;
        {
            // ★ Сборка 164: объект оружия мог устареть со сменой уровня — мусор в слот не пишем
            char nm[120];
            if (!ReadObjName(req->obj[s], nm, sizeof(nm)) || ::strncmp(nm, "Weapons/", 8) != 0) {
                ++stale;
                continue;
            }
        }
        unsigned char* e = (unsigned char*)(ULONGLONG)(begin + s * kWpnEntrySize);
        __try {
            *(unsigned long*)e = req->obj[s];
            ::memset(e + 4, 0, 0x1C);                     // навесы человека к чужому оружию не подходят
            // Сборка 152: навесы мода (как VU: unlockAssets оружия), только проверенные объекты UnlockAsset
            unsigned long na = 0;
            for (unsigned long j = 0; j < req->attN[s] && j < kWpnMaxAtt; ++j) {
                if (req->att[s][j] == 0) continue;
                char an[120];
                if (!ReadObjName(req->att[s][j], an, sizeof(an)) || ::strncmp(an, "Weapons/", 8) != 0) {
                    ++stale;  // ★ 164: устаревший навес
                    continue;
                }
                *(unsigned long*)(e + 4 + na * 4) = req->att[s][j];
                ++na;
            }
            *(unsigned long*)(e + 0x20) = na;
            attWritten += (int)na;
            ++written;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    static volatile LONG s_logged = 0;
    if (written > 0 && InterlockedIncrement(&s_logged) <= 40) {
        LOG_INFO("Bridge: ОРУЖИЕ бота %p: записано слотов мода=%d, навесов=%d (слот0=%08lX слот1=%08lX)", (void*)(ULONGLONG)ptr,
                 written, attWritten, req->obj[0], req->obj[1]);
    }
    if (stale > 0) {
        static volatile LONG s_staleLogged = 0;
        if (InterlockedIncrement(&s_staleLogged) <= 40) {
            LOG_WARN("Bridge: ОРУЖИЕ бота %p: %d устаревших объектов не записано (смена уровня?) — ждём новый выбор мода",
                     (void*)(ULONGLONG)ptr, stale);
        }
    }
}

// ★ Сборка 174: мод уже передал мосту выбор оружия для этого игрока (SetBotWeapons при привязке) — тело можно сажать.
static bool ModWeaponsReady(unsigned long ptr) {
    for (LONG i = 0; i < g_botWeaponsCount; ++i) {
        if (g_botWeapons[i].ptr == ptr) return true;
    }
    return false;
}

// Private native weak-target userdata used by the real human customization path.
constexpr char kHumanSoldierTargetMeta[] = "evbots.HumanSoldierTarget";
struct CustomizationLiveDc {
    unsigned long ptr, type;
    unsigned char shape, writable, owned, reserved;
};
static_assert(sizeof(CustomizationLiveDc)==12,"LiveDataContainer x86 ABI");
static unsigned long CustomizationPointer(lua_State* L,int index) {
    const lua_Integer value=luaL_checkinteger(L,index);
    if(value<=0 || static_cast<unsigned long long>(value)>0xffffffffULL)
        luaL_argerror(L,index,"expected a nonzero native x86 pointer");
    return static_cast<unsigned long>(value);
}
int l_bridge_capture_human_soldier(lua_State* L) {
    const auto player=CustomizationPointer(L,1),body=CustomizationPointer(L,2);
    soldiercustom::Target target;const char* error=nullptr;
    if(!soldiercustom::CaptureHuman(player,body,&target,&error)) {
        lua_pushnil(L);lua_pushstring(L,error?error:"native soldier capture failed");return 2;
    }
    void* storage=lua_newuserdatauv(L,sizeof(target),0);
    std::memcpy(storage,&target,sizeof(target));
    luaL_getmetatable(L,kHumanSoldierTargetMeta);lua_setmetatable(L,-2);return 1;
}
int l_bridge_apply_human_soldier_customization(lua_State* L) {
    const auto player=CustomizationPointer(L,1),body=CustomizationPointer(L,2);
    const auto* target=static_cast<soldiercustom::Target*>(luaL_testudata(L,3,kHumanSoldierTargetMeta));
    const auto* data=static_cast<CustomizationLiveDc*>(luaL_testudata(L,4,"vext.LiveDataContainer"));
    const char* error=nullptr;bool queued=false,ok=false;
    if(!target || lua_rawlen(L,3)!=sizeof(*target) || target->player!=player || target->soldier.pointer!=body)
        error="native target does not belong to this player and SoldierEntity";
    else if(!data || lua_rawlen(L,4)!=sizeof(*data) || data->shape!=0 || !data->ptr ||
        !eng::IsKindOfName(data->type,"CustomizeSoldierData"))
        error="expected a live native CustomizeSoldierData instance";
    else ok=soldiercustom::Submit(*target,data->ptr,data->owned!=0,&queued,&error);
    lua_pushboolean(L,ok);if(error)lua_pushstring(L,error);else lua_pushnil(L);
    lua_pushboolean(L,queued);return 3;
}
static bool ReadHumanSoldierTransform(lua_State* L,int index,float matrix[16]) {
    index=lua_absindex(L,index);
    if(!lua_istable(L,index))return false;
    const char* rows[]={"left","up","forward","trans"};
    const char* components[]={"x","y","z"};
    for(unsigned row=0;row<4;++row) {
        lua_getfield(L,index,rows[row]);
        if(!lua_istable(L,-1)) {lua_pop(L,1);return false;}
        for(unsigned col=0;col<3;++col) {
            lua_getfield(L,-1,components[col]);int valid=0;
            const double value=lua_tonumberx(L,-1,&valid);lua_pop(L,1);
            if(!valid || !std::isfinite(value) || !std::isfinite(static_cast<float>(value))) {lua_pop(L,1);return false;}
            matrix[row*4+col]=static_cast<float>(value);
        }
        matrix[row*4+3]=0;lua_pop(L,1);
    }
    return true;
}
int l_bridge_set_human_soldier_transform(lua_State* L) {
    const auto player=CustomizationPointer(L,1),body=CustomizationPointer(L,2);
    const auto* target=static_cast<soldiercustom::Target*>(luaL_testudata(L,3,kHumanSoldierTargetMeta));
    const char* error=nullptr;bool queued=false,ok=false;float matrix[16]={};
    if(!target || lua_rawlen(L,3)!=sizeof(*target) || target->player!=player || target->soldier.pointer!=body)
        error="native target does not belong to this player and SoldierEntity";
    else if(!ReadHumanSoldierTransform(L,4,matrix))error="expected a complete finite LinearTransform";
    else ok=soldiercustom::SubmitTransform(*target,matrix,&queued,&error);
    lua_pushboolean(L,ok);if(error)lua_pushstring(L,error);else lua_pushnil(L);
    lua_pushboolean(L,queued);return 3;
}
int l_bridge_request_human_parachute(lua_State* L) {
    const auto player=CustomizationPointer(L,1),body=CustomizationPointer(L,2);
    const auto* target=static_cast<soldiercustom::Target*>(luaL_testudata(L,3,kHumanSoldierTargetMeta));
    const char* error=nullptr;
    bool ok=false;
    if(!target || lua_rawlen(L,3)!=sizeof(*target) || target->player!=player || target->soldier.pointer!=body)
        error="native target does not belong to this player and SoldierEntity";
    else ok=soldiercustom::RequestParachute(*target,&error);
    lua_pushboolean(L,ok);
    if(error)lua_pushstring(L,error);else lua_pushnil(L);
    return 2;
}
int l_bridge_read_human_state(lua_State* L) {
    const auto player=EVBotsEngineHumanByName(luaL_checkstring(L,1));
    soldiercustom::HumanState state;const char* error=nullptr;
    if(!soldiercustom::ReadHumanState(player,&state,&error)) {
        lua_pushnil(L);lua_pushstring(L,error?error:"native human state readback failed");return 2;
    }
    lua_createtable(L,0,5);
    lua_pushinteger(L,static_cast<lua_Integer>(state.target.player));lua_setfield(L,-2,"playerPtr");
    lua_pushinteger(L,static_cast<lua_Integer>(state.target.soldier.pointer));lua_setfield(L,-2,"bodyPtr");
    lua_pushboolean(L,state.target.soldier.pointer!=0);lua_setfield(L,-2,"hasSoldier");
    lua_pushinteger(L,static_cast<lua_Integer>(state.target.soldier.generation));lua_setfield(L,-2,"generation");
    if(state.target.soldier.pointer) {
        const char* rows[]={"left","up","forward","trans"};
        const char* components[]={"x","y","z"};
        lua_createtable(L,0,4);
        for(unsigned row=0;row<4;++row) {
            lua_createtable(L,0,3);
            for(unsigned col=0;col<3;++col) {
                lua_pushnumber(L,state.matrix[row*4+col]);lua_setfield(L,-2,components[col]);
            }
            lua_setfield(L,-2,rows[row]);
        }
        lua_setfield(L,-2,"transform");
    }
    return 1;
}
/// NativeBridge.SetBotWeapons(ptr, {[слот]=имя ассета}): находит объекты оружия и запоминает; пишутся при спавне.
int l_bridge_set_bot_weapons(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    unsigned long kit = 0;
    __try {
        kit = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSelectedKit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        kit = 0;
    }
    BotWeaponsReq req = {};
    req.ptr = ptr;
    int found = 0, missing = 0;
    for (unsigned long s = 0; s < kWpnSlots; ++s) {
        lua_geti(L, 2, (lua_Integer)s);
        const char* nm = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
        if (nm != nullptr && nm[0] != '\0') {
            req.obj[s] = FindAssetByName(kit, nm);
            if (req.obj[s] != 0) {
                ++found;
            } else {
                ++missing;
                static volatile LONG s_miss = 0;
                if (InterlockedIncrement(&s_miss) <= 30) {
                    LOG_INFO("Bridge: ОРУЖИЕ: объект «%s» (слот %lu) в памяти не найден", nm, s);
                }
            }
        }
        lua_pop(L, 1);
    }
    // Сборка 152: третий аргумент — {[слот] = {имена навесов}}; объект берём, только если это UnlockAsset (vtable)
    int attFound = 0, attMissing = 0;
    if (lua_istable(L, 3)) {
        for (unsigned long s = 0; s < kWpnSlots; ++s) {
            if (req.obj[s] == 0) continue;
            lua_geti(L, 3, (lua_Integer)s);
            if (lua_istable(L, -1)) {
                const lua_Integer cnt = luaL_len(L, -1);
                for (lua_Integer j = 1; j <= cnt && req.attN[s] < kWpnMaxAtt; ++j) {
                    lua_geti(L, -1, j);
                    const char* an = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
                    if (an != nullptr && an[0] != '\0') {
                        const unsigned long ao = FindAssetByName(kit, an);
                        unsigned long vt = 0;
                        if (ao != 0 && Readable((const void*)(ULONGLONG)ao)) {
                            __try { vt = *(const unsigned long*)(ULONGLONG)ao; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = 0; }
                        }
                        if (ao != 0 && vt == kUnlockAssetVtbl) {
                            req.att[s][req.attN[s]++] = ao;
                            ++attFound;
                        } else {
                            ++attMissing;
                            static volatile LONG s_amiss = 0;
                            if (InterlockedIncrement(&s_amiss) <= 30) {
                                LOG_INFO("Bridge: НАВЕС «%s» (слот %lu): %s (объект %08lX, vtable %08lX)", an, s,
                                         ao == 0 ? "в памяти не найден" : "не UnlockAsset — пропущен", ao, vt);
                            }
                        }
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
    LONG idx = -1;
    for (LONG i = 0; i < g_botWeaponsCount; ++i) {
        if (g_botWeapons[i].ptr == ptr) { idx = i; break; }
    }
    if (idx < 0 && g_botWeaponsCount < 64) idx = g_botWeaponsCount++;
    if (idx >= 0) g_botWeapons[idx] = req;
    else {
        static volatile LONG s_fullLog = 0;  // сборка 256: раньше выбор молча пропадал
        if (InterlockedIncrement(&s_fullLog) <= 20)
            LOG_ERR("Bridge: ОРУЖИЕ: таблица выбора мода полна (64) — бот %p сядет без оружия мода", (void*)(ULONGLONG)ptr);
    }
    lua_pushinteger(L, found);
    lua_pushinteger(L, missing);
    lua_pushinteger(L, attFound);
    lua_pushinteger(L, attMissing);
    return 4;
}

// ★ Сборка 107: перки бота = выбор мода. Разбор обработчика выбора анлоков FUN_00E331B0 (re\selectunlock_decomp.txt):
// третий список сообщения (+0x40, «Too many specializations… MaxSpecializationUnlockAssets») движок кладёт в игрока
// +0x94 — eastl::fixed_set<UnlockAsset*, 37> (+0x04 якорь, +0x0C корень|флаг, +0x10 размер, +0x14 свободные узлы
// своего пула; узел 0x10: +0/+4 дети, +8 родитель, +0xC значение). Запись: присвоить пустое (0x00E2C1D0 = очистка),
// вставить каждый (0x00E299A0, thiscall, ret 0xC), затем флаг репликации 0x20 (0x01A35BA0 на игрок+0x374).
// Узлы берутся из пула самого множества — куча не трогается. Пишем только в хуке тика (на спавне).
constexpr unsigned long kOffPlayerSpecSet = 0x94;
constexpr unsigned long kSpecSetAssignRva = 0x00A2C1D0UL;  // VA 0x00E2C1D0
constexpr unsigned long kSpecSetInsertRva = 0x00A299A0UL;  // VA 0x00E299A0
constexpr int kMaxBotPerks = 37;  // предел движка (fixed_set на 37)
struct BotPerksReq { unsigned long ptr; LONG count; unsigned long obj[kMaxBotPerks]; };
static BotPerksReq g_botPerks[64];
static LONG g_botPerksCount = 0;

static bool BotPerksKnown(unsigned long ptr) {
    for (LONG i = 0; i < g_botPerksCount; ++i) {
        if (g_botPerks[i].ptr == ptr) return true;
    }
    const ChoiceName* cn = ChoiceOfPtr(ptr);  // сборка 159: список перков по имени (и пустой тоже)
    return cn != nullptr && cn->perksKnown != 0;
}

static void ApplyBotPerks(unsigned long ptr) {
    const BotPerksReq* req = nullptr;
    for (LONG i = 0; i < g_botPerksCount; ++i) {
        if (g_botPerks[i].ptr == ptr) { req = &g_botPerks[i]; break; }
    }
    // ★ Сборка 159: перки по имени (SetChoiceByName). Объекты ищем по киту бота — кит уже стоит (PrepareBotKit/ApplyBotKitClass).
    if (req == nullptr && ptr != 0 && g_botPerksCount < 64) {
        const ChoiceName* cn = ChoiceOfPtr(ptr);
        unsigned long kit = 0;
        if (cn != nullptr && cn->perksKnown != 0) {
            __try {
                kit = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSelectedKit);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                kit = 0;
            }
        }
        if (kit != 0) {
            char buf[sizeof(cn->perks)];
            const char* names[64];
            const int nn = SplitPerkNames(cn, buf, sizeof(buf), names, 64);
            BotPerksReq r = {};
            r.ptr = ptr;
            for (int k = 0; k < nn && r.count < kMaxBotPerks; ++k) {
                const unsigned long obj = FindAssetByName(kit, names[k]);
                if (obj != 0) r.obj[r.count++] = obj;
            }
            g_botPerks[g_botPerksCount] = r;
            req = &g_botPerks[g_botPerksCount];
            ++g_botPerksCount;
        }
    }
    unsigned char* base = NativeModuleBase();
    if (req == nullptr || ptr == 0 || base == nullptr || !InEngineTick()) return;
    // ★ Сборка 164: объекты перков запомнены при первом выборе и вставляются на каждом респавне. После смены уровня
    // адреса устаревают (163b: киты — мусор) — проверяем имя каждого; устаревшие переразрешаем по именам выбора мода,
    // мусор в движок не пишем.
    {
        int bad = 0;
        char nm[120];
        for (LONG i = 0; i < req->count; ++i) {
            if (!ReadObjName(req->obj[i], nm, sizeof(nm)) || ::strncmp(nm, "Persistence/", 12) != 0) ++bad;
        }
        if (bad > 0) {
            BotPerksReq* w = const_cast<BotPerksReq*>(req);
            const ChoiceName* cn = ChoiceOfPtr(ptr);
            unsigned long kit = 0;
            __try {
                kit = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSelectedKit);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                kit = 0;
            }
            LONG keep = 0;
            if (cn != nullptr && cn->perksKnown != 0) {
                char buf[sizeof(cn->perks)];
                const char* names[64];
                const int nn = SplitPerkNames(cn, buf, sizeof(buf), names, 64);
                for (int k = 0; k < nn && keep < kMaxBotPerks; ++k) {
                    const unsigned long obj = FindAssetByName(kit, names[k]);
                    if (obj != 0) w->obj[keep++] = obj;
                }
            } else {
                for (LONG i = 0; i < req->count; ++i) {  // имён нет — оставляем только живые объекты
                    if (ReadObjName(req->obj[i], nm, sizeof(nm)) && ::strncmp(nm, "Persistence/", 12) == 0) {
                        w->obj[keep++] = req->obj[i];
                    }
                }
            }
            LOG_WARN("Bridge: ПЕРКИ бота %p: %d объектов устарели (смена уровня?) — %s, теперь %ld", (void*)(ULONGLONG)ptr,
                     bad, (cn != nullptr && cn->perksKnown != 0) ? "переразрешены по именам" : "убраны", (long)keep);
            w->count = keep;
        }
    }
    using AssignFn = void(__fastcall*)(void*, void*, const void*);
    using InsertFn = void(__fastcall*)(void*, void*, void*, const unsigned long*, unsigned long);
    using DirtyFn = void(__fastcall*)(void*, void*, unsigned long);
    static unsigned char s_emptySet[0x40] = {};  // источник присваивания: корень +0x0C == 0 → только очистка
    unsigned char* set = (unsigned char*)(ULONGLONG)(ptr + kOffPlayerSpecSet);
    int inserted = 0;
    long size = -1;
    __try {
        ((AssignFn)(base + kSpecSetAssignRva))(set, nullptr, s_emptySet);
        for (LONG i = 0; i < req->count && i < 37; ++i) {
            unsigned long result[2] = {0, 0};
            const unsigned long v = req->obj[i];
            ((InsertFn)(base + kSpecSetInsertRva))(set, nullptr, result, &v, 0);
            ++inserted;
        }
        ((DirtyFn)(base + kReplDirtyRva))((unsigned char*)(ULONGLONG)ptr + kOffPlayerReplDirty, nullptr, 0x20);
        size = *(const long*)(set + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: ПЕРКИ бота %p: исключение при записи (вставлено %d)", (void*)(ULONGLONG)ptr, inserted);
        return;
    }
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 80) {
        // ★ Сборка 163: полный список коротких имён (хвост пути) — видно, есть ли специализация солдата
        // (Specializations/…) и какие объекты повторились (размер множества меньше вставленного).
        char list[1400];
        size_t len = 0;
        list[0] = '\0';
        const char* spec = "НЕТ";
        static char s_spec[64];
        for (LONG i = 0; i < req->count && i < 37; ++i) {
            char nm[120];
            ReadObjName(req->obj[i], nm, sizeof(nm));
            const char* tail = ::strrchr(nm, '/');
            tail = tail != nullptr ? tail + 1 : nm;
            bool dup = false;
            for (LONG j = 0; j < i; ++j) dup = dup || req->obj[j] == req->obj[i];
            if (::strstr(nm, "Specializations") != nullptr) {
                ::strncpy(s_spec, tail, sizeof(s_spec) - 1);
                s_spec[sizeof(s_spec) - 1] = '\0';
                spec = s_spec;
            }
            const int w = ::_snprintf(list + len, sizeof(list) - len, "%s%s%s", i ? "," : "", tail, dup ? "(ПОВТОР)" : "");
            if (w < 0) break;
            len += (size_t)w;
            if (len >= sizeof(list) - 1) break;
        }
        LOG_INFO("Bridge: ПЕРКИ бота %p: вставлено=%d размер множества=%ld специализация=%s [%s]",
                 (void*)(ULONGLONG)ptr, inserted, size, spec, list);
    }
}

// ★ Сборка 185: Player:SelectUnlockAssets КАК VU. VU (0x1098EB70) собирает NetworkPlayerSelectedUnlockAssetsMessage
// (+0x38 кит, +0x3C массив внешности, +0x40 массив анлоков) и синхронно зовёт обработчик сервера — у нас 0x00E331B0
// (thiscall(игрок, сообщение), ret 4; re\selectunlock_decomp.txt). Обработчик: кит → setSelectedCustomization + флаг 0x40;
// внешность → очистка вектора +0x4B8 и добавление прошедших проверку игрока vtbl+0x18 (0x00E145A0: бит «разблокировано» в
// [игрок+0x3A0]); анлоки (≤37) → набор +0x94 + флаг 0x20; затем слоты оружия и applyCustomization, если тело уже есть.
// Обработчик читает только поля +0x38..+0x45, поэтому сообщение — наш нулевой буфер, массивы fb::Array (число в [p−4])
// — наши статические (обработчик их копирует). Вызов — только в тике движка. Проверку «разблокировано» делаем заранее:
// обработчик сначала ОЧИЩАЕТ внешность — без разблокировок бот остался бы без модели. Не всё разблокировано
// (у VU «всё открыто всем», у нас — vars.unlockMode) — false, мост пишет поля по-старому (кит с флагом 0x40).
static bool SelectEngineOn() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_SELECT_ENGINE", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;
    }
    return s_on != 0;
}
static bool SelectHandlerOk(unsigned char* base) {
    static volatile LONG s_ok = 0;
    const LONG cur = InterlockedCompareExchange(&s_ok, 0, 0);
    if (cur != 0) return cur > 0;
    static const unsigned char kHandler[12] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0xB4, 0x04, 0x00, 0x00};
    static const unsigned char kUnlocked[13] = {0x81, 0xEC, 0x30, 0x01, 0x00, 0x00, 0x56, 0x8B, 0xB4, 0x24, 0x38, 0x01, 0x00};
    bool ok = false;
    __try {
        ok = std::memcmp(base + 0x00A331B0UL, kHandler, sizeof(kHandler)) == 0 &&
             std::memcmp(base + 0x00A145A0UL, kUnlocked, sizeof(kUnlocked)) == 0 &&
             *(const unsigned long*)(base + 0x021C778CUL + 0x18) == 0x00E145A0UL;  // vtable ServerPlayer 0x025C778C, слот +0x18
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    InterlockedExchange(&s_ok, ok ? 1 : -1);
    if (ok) LOG_INFO("Bridge: обработчик выбора движка 0x00E331B0 и проверка разблокировок сверены — выбор ботов как VU");
    else LOG_ERR("Bridge: обработчик выбора 0x00E331B0 / проверка 0x00E145A0 не совпали с exe — выбор по-старому (поля)");
    return ok;
}
static int IsUnlockedFor(unsigned long ptr, unsigned long asset) {
    using UnlockedFn = bool(__fastcall*)(void*, void*, void*);
    __try {
        const unsigned long vt = *(const unsigned long*)(ULONGLONG)ptr;
        const unsigned long fn = *(const unsigned long*)(ULONGLONG)(vt + 0x18);
        return ((UnlockedFn)(ULONGLONG)fn)((void*)(ULONGLONG)ptr, nullptr, (void*)(ULONGLONG)asset) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
static LONG ResolveBotPerks(unsigned long ptr, unsigned long kit, unsigned long* out, LONG cap, bool* known) {
    *known = false;
    LONG n = 0;
    const ChoiceName* cn = ChoiceOfPtr(ptr);
    if (cn != nullptr && cn->perksKnown != 0) {
        *known = true;
        char buf[sizeof(cn->perks)];
        const char* names[64];
        const int nn = SplitPerkNames(cn, buf, sizeof(buf), names, 64);
        for (int k = 0; k < nn && n < cap; ++k) {
            const unsigned long obj = FindAssetByName(kit, names[k]);
            if (obj != 0) out[n++] = obj;
        }
        return n;
    }
    for (LONG i = 0; i < g_botPerksCount; ++i) {
        if (g_botPerks[i].ptr != ptr) continue;
        *known = true;
        char nm[120];
        for (LONG j = 0; j < g_botPerks[i].count && n < cap; ++j) {
            if (ReadObjName(g_botPerks[i].obj[j], nm, sizeof(nm)) && ::strncmp(nm, "Persistence/", 12) == 0) {
                out[n++] = g_botPerks[i].obj[j];
            }
        }
        break;
    }
    return n;
}
static int SelectViaEngine(unsigned long ptr) {
    unsigned char* base = NativeModuleBase();
    if (!SelectEngineOn() || base == nullptr || ptr == 0 || !InEngineTick()) return 0;
    if (!SelectHandlerOk(base)) return 0;
    const unsigned long kit = ApplyBotKitClass(ptr, false);
    if (kit == 0) return 0;
    const unsigned long app = ResolveBotAppearance(ptr, kit);
    unsigned long perks[kMaxBotPerks];
    bool perksKnown = false;
    const LONG np = ResolveBotPerks(ptr, kit, perks, kMaxBotPerks, &perksKnown);
    // Проверка «разблокировано» — та же, что сделает обработчик.
    int appOk = app != 0 ? IsUnlockedFor(ptr, app) : 0;
    LONG perksOk = 0;
    for (LONG i = 0; i < np; ++i) {
        if (IsUnlockedFor(ptr, perks[i]) == 1) ++perksOk;
    }
    static volatile LONG s_logged = 0;
    if (app == 0 || appOk != 1) {
        if (InterlockedIncrement(&s_logged) <= 40) {
            LOG_INFO("Bridge: ВЫБОР бота %p: внешность %s — запись полей по-старому", (void*)(ULONGLONG)ptr,
                     app == 0 ? "нет объекта" : "не разблокирована");
        }
        return 0;
    }
    // Перки не все разблокированы (у ботов нет статистики) — обработчику только кит и внешность (пустой список анлоков
    // он не трогает), перки пишет мост полями (ApplyBotPerks, флаг 0x20).
    const bool withPerks = perksOk == np;
    struct FbArray { unsigned long count; unsigned long items[kMaxBotPerks]; };
    static FbArray s_app;    // только тик движка — вызовы последовательные
    static FbArray s_perks;
    s_app.count = 1;
    s_app.items[0] = app;
    s_perks.count = withPerks ? (unsigned long)np : 0UL;
    for (LONG i = 0; i < np; ++i) s_perks.items[i] = perks[i];
    alignas(8) unsigned char msg[0x48] = {};
    *(unsigned long*)(msg + 0x38) = kit;
    *(unsigned long*)(msg + 0x3C) = (unsigned long)(ULONGLONG)&s_app.items[0];
    *(unsigned long*)(msg + 0x40) = (unsigned long)(ULONGLONG)&s_perks.items[0];
    using HandlerFn = void(__fastcall*)(void*, void*, void*);
    unsigned long kitAfter = 0;
    long setSize = -1;
    __try {
        ((HandlerFn)(base + 0x00A331B0UL))((void*)(ULONGLONG)ptr, nullptr, msg);
        kitAfter = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSelectedKit);
        setSize = *(const long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSpecSet + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("Bridge: ВЫБОР бота %p: исключение в обработчике движка — запись полей по-старому", (void*)(ULONGLONG)ptr);
        return 0;
    }
    if (InterlockedIncrement(&s_logged) <= 40) {
        char an[120] = {};
        ReadObjName(app, an, sizeof(an));
        LOG_INFO("Bridge: ВЫБОР бота %p через движок (как VU): кит %08lX (стал %08lX), внешность «%s», перков %ld "
                 "(набор движка %ld)%s%s", (void*)(ULONGLONG)ptr, kit, kitAfter, an, (long)np, setSize,
                 perksKnown ? "" : ", список перков мода ещё не пришёл",
                 withPerks ? "" : " — перки не разблокированы, их пишет мост полями");
    }
    if (kitAfter != kit) return 0;
    return withPerks ? 1 : 2;
}

// BEGIN HumanSelectUnlockAssets native adapter.
namespace humanselect {
constexpr unsigned kVisualLimit=64,kMiscLimit=37;
struct Array { unsigned long count=0;unsigned long items[kVisualLimit]={}; };
static_assert(offsetof(Array,items)==4,"fb::Array count precedes its item pointer");
struct Request {unsigned long kit=0;Array visual,misc;};
volatile LONG faulted=0;
bool CurrentAsset(const CustomizationLiveDc* data,const char* type,const char** error) {
    if(!data || data->shape || !data->ptr || eng::TypeOf(data->ptr)!=data->type || !eng::IsKindOfName(data->type,type))
        return playerteam::Fail(error,"expected a live native customization/unlock asset of the correct type");
    unsigned flags=0;
    if(!playerteam::Read(data->ptr+4,&flags,4) || (flags&0x08000000))return playerteam::Fail(error,"unlock asset is destroyed");
    if(data->owned)return (flags&0xFFFF)!=0 || playerteam::Fail(error,"owned unlock asset has no native reference");
    eng::Guid16 guid={};char text[40]={};
    const auto partition=eng::PartitionOfInstance(data->ptr);
    if(!partition || !eng::InstanceGuid(data->ptr,&guid))return playerteam::Fail(error,"borrowed unlock asset has no current partition/GUID");
    eng::FormatGuid(guid,text);
    return eng::FindInstanceInPartition(partition,text)==data->ptr || playerteam::Fail(error,"unlock asset no longer belongs to its current partition/GUID");
}
bool Apply(unsigned long player,const char* name,const Request& request,const char** error) {
    if(error)*error=nullptr;
    if(faulted)return playerteam::Fail(error,"native human unlock selection fault; retry disabled");
    if(!request.kit || request.visual.count>kVisualLimit || request.misc.count>kMiscLimit)
        return playerteam::Fail(error,"invalid native unlock request");
    unsigned team=0,squad=0;
    if(!playerteam::Access(player,name,0,0,&team,&squad,error))return false;
    if(!EVBotsEngineIsHuman(player))return playerteam::Fail(error,"native unlock target is not a current human player");
    if(g_levelGate!=0)return playerteam::Fail(error,"human unlock selection cannot run during level transition");
    if(!SelectEngineOn())return playerteam::Fail(error,"EVBOTS_SELECT_ENGINE=0");
    if(!SelectHandlerOk(NativeModuleBase()))return playerteam::Fail(error,"native unlock handler signature batch failed");
    unsigned long context=0,previous=0;bool switched=false,ok=false;
    if(!playerteam::Read(0x02B3EE2C,&context,4) || !context)return playerteam::Fail(error,"native unlock server context is unavailable");
    __try {
        // Every preflight precedes the handler: it clears the previous visual
        // selection before testing unlocks, so a partial request is not applied.
        const auto vtable=*reinterpret_cast<const unsigned long*>(player);
        if(*reinterpret_cast<const unsigned long*>(vtable+0x18)!=0x00E145A0)
            return playerteam::Fail(error,"native human unlock permission slot differs");
        previous=reinterpret_cast<unsigned long(__cdecl*)()>(0x0083D5E0)();
        reinterpret_cast<void(__cdecl*)(unsigned long)>(0x0083D5C0)(context);switched=true;
        bool unlocked=true;
        for(unsigned i=0;i<request.visual.count;++i)unlocked=IsUnlockedFor(player,request.visual.items[i])==1 && unlocked;
        for(unsigned i=0;i<request.misc.count;++i)unlocked=IsUnlockedFor(player,request.misc.items[i])==1 && unlocked;
        if(!unlocked)playerteam::Fail(error,"requested appearance/unlock is not unlocked for this human player");
        else {
            // E331B0 consumes only +38/+3C/+40, copies both arrays synchronously,
            // retains the kit and marks replication dirty. It does not retain
            // this message. VU1098F68E likewise destroys its message on return.
            alignas(8) unsigned char message[0x48]={};
            *reinterpret_cast<unsigned long*>(message+0x38)=request.kit;
            *reinterpret_cast<const unsigned long**>(message+0x3C)=request.visual.items;
            *reinterpret_cast<const unsigned long**>(message+0x40)=request.misc.items;
            using Handler=void(__thiscall*)(void*,const void*);
            reinterpret_cast<Handler>(0x00E331B0)(reinterpret_cast<void*>(player),message);
            ok=*reinterpret_cast<const unsigned long*>(player+0x90)==request.kit;
            if(ok && request.visual.count) {
                const auto begin=*reinterpret_cast<const unsigned long* const*>(player+0x4B8);
                const auto end=*reinterpret_cast<const unsigned long* const*>(player+0x4BC);
                ok=begin && end>=begin && static_cast<unsigned>(end-begin)==request.visual.count;
                if(ok)for(unsigned i=0;i<request.visual.count;++i)if(begin[i]!=request.visual.items[i]){ok=false;break;}
            }
            if(!ok)playerteam::Fail(error,"native human unlock handler did not retain the requested kit/appearance");
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {InterlockedExchange(&faulted,1);playerteam::Fail(error,"native human unlock selection failed");}
    if(switched) __try {reinterpret_cast<void(__cdecl*)(unsigned long)>(0x0083D5C0)(previous);}
    __except(EXCEPTION_EXECUTE_HANDLER) {ok=false;InterlockedExchange(&faulted,1);playerteam::Fail(error,"restoring human unlock context failed");}
    if(ok)LOG_INFO("PlayerSelect: HUMAN player=%08lX kit=%08lX visual=%u first=%08lX misc=%u native readback PASS",player,request.kit,request.visual.count,request.visual.count?request.visual.items[0]:0,request.misc.count);
    return ok;
}
}
// END HumanSelectUnlockAssets native adapter.
static unsigned long HumanUnlockAsset(lua_State* L,int index,const char* type) {
    const auto* data=static_cast<const CustomizationLiveDc*>(luaL_testudata(L,index,"vext.LiveDataContainer"));
    const char* error=nullptr;
    if(!data || lua_rawlen(L,index)!=sizeof(*data) || !humanselect::CurrentAsset(data,type,&error))
        luaL_error(L,"Player:SelectUnlockAssets: %s",error?error:"expected native DataContainer userdata");
    return data->ptr;
}
static void HumanUnlockList(lua_State* L,int index,humanselect::Array* array,unsigned limit) {
    luaL_checktype(L,index,LUA_TTABLE);
    const auto count=lua_rawlen(L,index);
    if(count>limit)luaL_error(L,"Player:SelectUnlockAssets: too many native unlock assets");
    array->count=static_cast<unsigned long>(count);
    for(size_t i=0;i<count;++i) {
        lua_rawgeti(L,index,static_cast<lua_Integer>(i+1));
        array->items[i]=HumanUnlockAsset(L,-1,"UnlockAssetBase");lua_pop(L,1);
    }
}
int l_bridge_select_human_unlock_assets(lua_State* L) {
    const auto player=CustomizationPointer(L,1);const char* name=luaL_checkstring(L,2);
    unsigned team=0,squad=0;const char* error=nullptr;
    // Validate the actual frame before consulting any borrowed DataContainer.
    if(!playerteam::Access(player,name,0,0,&team,&squad,&error))return luaL_error(L,"Player:SelectUnlockAssets: %s",error?error:"native player unavailable");
    humanselect::Request request;
    request.kit=HumanUnlockAsset(L,3,"CharacterCustomizationAsset");
    HumanUnlockList(L,4,&request.visual,humanselect::kVisualLimit);
    HumanUnlockList(L,5,&request.misc,humanselect::kMiscLimit);
    if(!humanselect::Apply(player,name,request,&error))return luaL_error(L,"Player:SelectUnlockAssets: %s",error?error:"native selection failed");
    return 0;
}

/// NativeBridge.SetBotPerks(ptr, {имя, ...}): находит объекты перков (отрядный и техники) и запоминает; пишутся на спавне.
int l_bridge_set_bot_perks(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    unsigned long kit = 0;
    __try {
        kit = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSelectedKit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        kit = 0;
    }
    BotPerksReq req = {};
    req.ptr = ptr;
    int missing = 0;
    const lua_Integer n = (lua_Integer)luaL_len(L, 2);
    for (lua_Integer i = 1; i <= n && req.count < kMaxBotPerks; ++i) {
        lua_geti(L, 2, i);
        const char* nm = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
        if (nm != nullptr && nm[0] != '\0') {
            const unsigned long obj = FindAssetByName(kit, nm);
            if (obj != 0) {
                req.obj[req.count++] = obj;
            } else {
                ++missing;
                static volatile LONG s_miss = 0;
                if (InterlockedIncrement(&s_miss) <= 30) LOG_INFO("Bridge: ПЕРКИ: объект «%s» в памяти не найден", nm);
            }
        }
        lua_pop(L, 1);
    }
    LONG idx = -1;
    for (LONG i = 0; i < g_botPerksCount; ++i) {
        if (g_botPerks[i].ptr == ptr) { idx = i; break; }
    }
    if (idx < 0 && g_botPerksCount < 64) idx = g_botPerksCount++;
    if (idx >= 0) g_botPerks[idx] = req;
    lua_pushinteger(L, req.count);
    lua_pushinteger(L, missing);
    return 2;
}

// ★ Сборка 236 (прогон 235, слова пользователя: «лишние боты, оружие без моделек, тупят»). Таблицы выбора мода на 64 бота
// (g_botClass, g_botAppearance, g_botPerks, g_ptrName, g_choiceName) при удалении бота не чистились: за прогон 92 бота, с 11:31
// новые не записывались — ChoiceGate ждал 30 с и сажал без выбора мода (36 раз, «выбора мода нет»). А бот на памяти
// удалённого получал его старые класс/внешность/имя. Чистим при удалении — вместе с остальными списками моста.
static bool PtrNameInUse(const char* name, unsigned long except) {
    for (LONG i = 0; i < g_ptrNameCount; ++i) {
        if (g_ptrName[i].ptr != except && ::strcmp(g_ptrName[i].name, name) == 0) return true;
    }
    // и бот с этим именем ещё в очереди отложенного создания (выбор мод мог прислать до тика)
    bool pending = false;
    ::AcquireSRWLockShared(&g_pendingLock);
    for (LONG i = 0; i < g_pendingNativeCount && i < 32; ++i) {
        if (::strcmp(g_pendingNative[i].name, name) == 0) { pending = true; break; }
    }
    ::ReleaseSRWLockShared(&g_pendingLock);
    return pending;
}

static void ForgetBotChoice(unsigned long dp) {
    for (LONG i = 0; i < g_botClassCount;) {
        if (g_botClass[i].ptr == dp) { g_botClass[i] = g_botClass[g_botClassCount - 1]; --g_botClassCount; } else { ++i; }
    }
    for (LONG i = 0; i < g_botAppearanceCount;) {
        if (g_botAppearance[i].ptr == dp) { g_botAppearance[i] = g_botAppearance[g_botAppearanceCount - 1]; --g_botAppearanceCount; } else { ++i; }
    }
    for (LONG i = 0; i < g_botPerksCount;) {
        if (g_botPerks[i].ptr == dp) { g_botPerks[i] = g_botPerks[g_botPerksCount - 1]; --g_botPerksCount; } else { ++i; }
    }
    for (LONG i = 0; i < g_ptrNameCount;) {
        if (g_ptrName[i].ptr != dp) { ++i; continue; }
        // выбор по имени — только если это имя больше ни у кого (мод мог уже пересоздать бота с тем же именем)
        if (!PtrNameInUse(g_ptrName[i].name, dp)) {
            for (LONG j = 0; j < g_choiceNameCount;) {
                if (::strcmp(g_choiceName[j].name, g_ptrName[i].name) == 0) {
                    g_choiceName[j] = g_choiceName[g_choiceNameCount - 1];
                    --g_choiceNameCount;
                } else { ++j; }
            }
        }
        g_ptrName[i] = g_ptrName[g_ptrNameCount - 1];
        --g_ptrNameCount;
    }
}

// ★ Сборка 256 (прогон 255): бот удалён в движке (deletePlayer) — снять его записи из таблиц, которые только росли. Прогон 255:
// после kill all таблица выбора оружия мода (64) переполнилась, SetBotWeapons новых ботов молча отбрасывался, таймер
// «выбор мода не пришёл за 3000 мс» (13 раз) сажал их с оружием человека — мод вёл их как снайперов (M40A5, арбалет),
// а в руках у солдата было другое: «оружия в руках нет, ходят странно» (BOT_Erwin, BOT_Christina). Остальные таблицы —
// та же болезнь (поза, точка спавна, событие спавна, группа VU, путь создания).
template <class T>
static void DropPtrRecs(T* arr, LONG* cnt, unsigned long dp) {
    for (LONG i = 0; i < *cnt;) {
        if (arr[i].ptr == dp) { arr[i] = arr[*cnt - 1]; --*cnt; } else { ++i; }
    }
}
static void ForgetDeletedBot(unsigned long dp) {
    DropPtrRecs(g_botWeapons, &g_botWeaponsCount, dp);
    DropPtrRecs(g_botPose, &g_botPoseCount, dp);
    DropPtrRecs(g_forcedPoint, &g_forcedPointCount, dp);
    DropPtrRecs(g_spawnEvFired, &g_spawnEvFiredCount, dp);
    DropPtrRecs(g_vuGroup, &g_vuGroupCount, dp);
    DropPtrRecs(g_botPath, &g_botPathCount, dp);
    DropPtrRecs(g_botLevelSeq, &g_botLevelSeqCount, dp);
}

/// NativeBridge.SetChoiceByName(имя, кит, внешность, "перк\nперк") — выбор мода по имени бота (сборка 159).
/// Lua шлёт его при SelectUnlockAssets в любой момент: до привязки и до создания игрока. Пустая строка перков — список получен, перков нет.
int l_bridge_set_choice_by_name(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* kit = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    const char* app = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr;
    const char* perks = lua_isstring(L, 4) ? lua_tostring(L, 4) : nullptr;
    if (name[0] == '\0') {
        lua_pushboolean(L, 0);
        return 1;
    }
    ChoiceName* e = nullptr;
    for (LONG j = 0; j < g_choiceNameCount; ++j) {
        if (::strcmp(g_choiceName[j].name, name) == 0) { e = &g_choiceName[j]; break; }
    }
    if (e == nullptr) {
        if (g_choiceNameCount >= 64) {
            static LONG s_warn = 0;  // сборка 236
            if (s_warn < 20) { ++s_warn; LOG_WARN("Bridge: таблица выбора по имени полна (64) — выбор %s не записан", name); }
            lua_pushboolean(L, 0);
            return 1;
        }
        e = &g_choiceName[g_choiceNameCount++];
        ::memset(e, 0, sizeof(*e));
        e->cls = -1;
        ::strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
    }
    if (kit != nullptr) {
        const int cls = ClassFromKitName(kit);
        if (cls >= 0) e->cls = cls;
    }
    if (app != nullptr && app[0] != '\0') {
        ::strncpy(e->app, app, sizeof(e->app) - 1);
        e->app[sizeof(e->app) - 1] = '\0';
    }
    if (perks != nullptr) {
        ::strncpy(e->perks, perks, sizeof(e->perks) - 1);
        e->perks[sizeof(e->perks) - 1] = '\0';
        e->perksKnown = 1;
    }
    static volatile LONG s_choiceLog = 0;
    if (InterlockedIncrement(&s_choiceLog) <= 40) {
        int pc = (e->perks[0] != '\0') ? 1 : 0;
        for (const char* q = e->perks; *q != '\0'; ++q) {
            if (*q == '\n') ++pc;
        }
        LOG_INFO("Bridge: ВЫБОР по имени «%s»: кит класса %d, внешность «%s», перков %d%s", name, e->cls, e->app, pc,
                 e->perksKnown ? "" : " (список не получен)");
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ★ Сборка 109: диагностика движения и опыт «зеркало человека». Компонент солдата (+0x538, vtable 0x026542A0):
// его vfunc +0x0C (0x01189200) берёт текущее состояние (+0x18) и опирается на поля движения, которые у человека
// приходят от клиента по сети (разбор re\comp_move_decomp*.txt). Байтовые смещения: 0x4CC..0x4F0 (значения и
// индекс состояния 0x4F0 & 0x1F), 0x4E0 — тип состояния, 0x4E8 — флаги, 0x660 — индекс-копия, 0x664 — таймер.
constexpr unsigned long kOffSoldierComponent = 0x538;
constexpr unsigned long kOffCompState = 0x18;      // текущее состояние компонента
constexpr unsigned long kOffCompMoveBegin = 0x4CC;  // dword 0x133
constexpr int kCompMoveDwords = 10;                 // 0x133..0x13C

/// NativeBridge.InputAimOf(playerPtr) -> authoritativeAimingYaw, authoritativeAimingPitch | nil (сборка 199; [+0x3C0]: yaw +0x1F4,
/// pitch +0x1F8 — те же поля, что пишет NativeBridge.InputAim ботам).
/// ★ Сборка 200, диагностика: NativeBridge.VelScan(base, vx, vy, vz, span) -> { смещение, ... } — смещения в [base, base+span),
/// где лежат три float, совпадающие с (vx, vy, vz) с точностью 15 % (не меньше 0,4 м/с). Ищем поле скорости физики движка
/// (VU: PhysicsEntity.velocity — из физики; у нас пока производная позиции). base — объект физики (солдат [+0x1D4], машина [+0x144]).
int l_bridge_vel_scan(lua_State* L) {
    const unsigned long base = (unsigned long)luaL_checkinteger(L, 1);
    const float vx = (float)luaL_checknumber(L, 2), vy = (float)luaL_checknumber(L, 3), vz = (float)luaL_checknumber(L, 4);
    const unsigned long span = (unsigned long)luaL_optinteger(L, 5, 0x400);
    lua_newtable(L);
    if (base == 0 || span > 0x2000) return 1;
    const float mag = ::sqrtf(vx * vx + vy * vy + vz * vz);
    const float tol = mag * 0.15f > 0.4f ? mag * 0.15f : 0.4f;
    int k = 0;
    __try {
        for (unsigned long off = 0; off + 12 <= span; off += 4) {
            const float* f = (const float*)(ULONGLONG)(base + off);
            const float dx = f[0] - vx, dy = f[1] - vy, dz = f[2] - vz;
            if (dx * dx + dy * dy + dz * dz <= tol * tol) {
                lua_pushinteger(L, (lua_Integer)off);
                lua_rawseti(L, -2, ++k);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 1;
}

/// NativeBridge.SoldierVel(playerPtr) -> vx, vy, vz | nil (сборка 200b): скорость солдата из движка, как VU (PhysicsEntity.velocity
/// из физики). Поле — Vec3 [[игрок+0x3C4]+0x2B0] (факт прогона 200: 263 из 329 замеров совпали с производной позиции).
/// Не конечное или |v| ≥ 200 — nil (мост берёт производную). EVBOTS_VEL_ENGINE=0 — всегда nil.
static bool SoldierVelEnabled() {
    static const bool s_on = []() {
        char b[8] = {0};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VEL_ENGINE", b, (DWORD)sizeof(b));
        return !(n > 0 && n < sizeof(b) && b[0] == '0');
    }();
    return s_on;
}

/// Сборка 240: общее для SoldierVel и BotTick — Vec3 [[игрок+0x3C4]+0x2B0], false — нет солдата, не конечное или |v| ≥ 200.
static bool ReadSoldierVel(unsigned long ptr, float v[3]) {
    bool ok = false;
    if (SoldierVelEnabled() && ptr != 0) {
        __try {
            const unsigned long sol = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3C4);
            if (sol != 0) {
                const float* f = (const float*)((const unsigned char*)(ULONGLONG)sol + 0x2B0);
                v[0] = f[0]; v[1] = f[1]; v[2] = f[2];
                const float m2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
                ok = m2 == m2 && m2 < 200.0f * 200.0f;  // NaN даёт false; inf отсекает предел
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    return ok;
}

int l_bridge_soldier_vel(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    float v[3] = {0.0f, 0.0f, 0.0f};
    if (!ReadSoldierVel(ptr, v)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (lua_Number)v[0]);
    lua_pushnumber(L, (lua_Number)v[1]);
    lua_pushnumber(L, (lua_Number)v[2]);
    return 3;
}

/// ★ Сборка 200b, диагностика: NativeBridge.VelScanDeep(base, vx, vy, vz, span) -> { "p<смещение указателя>:<смещение поля>", ... }.
/// Как VelScan, но ещё идёт по указателям из [base, base+span) (первый уровень, 0x400 байт за каждым): скорость машины лежит
/// в теле Havok за цепочкой указателей. Указатель отсекается по диапазону (0x10000..0x7FFE0000) и SEH.
int l_bridge_vel_scan_deep(lua_State* L) {
    const unsigned long base = (unsigned long)luaL_checkinteger(L, 1);
    const float vx = (float)luaL_checknumber(L, 2), vy = (float)luaL_checknumber(L, 3), vz = (float)luaL_checknumber(L, 4);
    const unsigned long span = (unsigned long)luaL_optinteger(L, 5, 0x400);
    lua_newtable(L);
    if (base == 0 || span > 0x1000) return 1;
    const float mag = ::sqrtf(vx * vx + vy * vy + vz * vz);
    const float tol = mag * 0.15f > 0.4f ? mag * 0.15f : 0.4f;
    int k = 0;
    for (unsigned long po = 0; po + 4 <= span && k < 40; po += 4) {
        unsigned long ptr = 0;
        __try {
            ptr = *(const unsigned long*)(ULONGLONG)(base + po);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        // Сервер — large-address-aware: объекты движка выше 2 ГБ (F748C440…); верхний предел 0x7FFE0000 их отсекал (прогон 201).
        if (ptr < 0x10000UL || ptr > 0xFFFF0000UL || (ptr & 3) != 0) continue;
        __try {
            for (unsigned long off = 0; off + 12 <= 0x400 && k < 40; off += 4) {
                const float* f = (const float*)(ULONGLONG)(ptr + off);
                const float dx = f[0] - vx, dy = f[1] - vy, dz = f[2] - vz;
                if (dx * dx + dy * dy + dz * dz <= tol * tol) {
                    char buf[40];
                    ::sprintf_s(buf, "p%lX:%lX", po, off);
                    lua_pushstring(L, buf);
                    lua_rawseti(L, -2, ++k);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 1;
}

int l_bridge_input_aim_of(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    float yaw = 0.0f, pitch = 0.0f;
    bool ok = false;
    __try {
        const unsigned long inp = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3C0);
        if (inp != 0) {
            yaw = *(const float*)((const unsigned char*)(ULONGLONG)inp + 0x1F4);
            pitch = *(const float*)((const unsigned char*)(ULONGLONG)inp + 0x1F8);
            ok = yaw == yaw && pitch == pitch;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (lua_Number)yaw);
    lua_pushnumber(L, (lua_Number)pitch);
    return 2;
}

/// NativeBridge.InputLevelOf(playerPtr, action) -> уровень действия из объекта ввода игрока (+0x3C0, +0x10 + action*4) или nil.
int l_bridge_input_level_of(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const long action = (long)luaL_checkinteger(L, 2);
    float v = 0.0f;
    bool ok = false;
    __try {
        const unsigned long inp = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3C0);
        if (inp != 0 && action >= 0 && action < 10) {
            v = *(const float*)((const unsigned char*)(ULONGLONG)inp + 0x10 + action * 4);
            ok = true;
        } else if (inp != 0 && action >= 10 && action < 74) {
            // ★ Сборка 199: цифровые действия — 64-битная маска [+0x1E0..+0x1E4], бит (action − 10): так их пишет движок
            // (EntryInputState::setLevel 0x008BEF00 → 0x008B9AD0: `and [esi+0x1E0], ~mask` / `[esi+0x1E4]`, сдвиг 0x01564740).
            const unsigned long lo = *(const unsigned long*)((const unsigned char*)(ULONGLONG)inp + 0x1E0);
            const unsigned long hi = *(const unsigned long*)((const unsigned char*)(ULONGLONG)inp + 0x1E4);
            const long bit = action - 10;
            const unsigned long w = bit < 32 ? lo : hi;
            v = ((w >> (bit & 31)) & 1UL) != 0 ? 1.0f : 0.0f;
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok || !(v > -10.0f && v < 10.0f)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

/// NativeBridge.EnvFlag(name) -> true, если переменная окружения равна "1" (опыты на время прогона).
int l_bridge_env_flag(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    char v[4] = {};
    const DWORD n = ::evbots::EnvGetCached(name, v, (DWORD)sizeof(v));  // сборка 168: кэш
    bool on = n == 1 && v[0] == '1';
    // ★ 239: или файл-флаг рядом с DLL — activation\flags\<ИМЯ> (диагностика без перезапуска лаунчера: переменные окружения
    // сервер берёт у лаунчера). Только имена EVBOTS_*.
    if (!on && std::strncmp(name, "EVBOTS_", 7) == 0 && std::strlen(name) < 64 && gModule != nullptr) {
        char path[MAX_PATH] = {};
        const DWORD len = ::GetModuleFileNameA(gModule, path, MAX_PATH);
        char* slash = len > 0 && len < MAX_PATH ? std::strrchr(path, '\\') : nullptr;
        if (slash != nullptr && (size_t)(slash - path) + 8 + std::strlen(name) < MAX_PATH) {
            std::strcpy(slash + 1, "flags\\");
            std::strcat(path, name);
            on = ::GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        }
    }
    lua_pushboolean(L, on ? 1 : 0);
    return 1;
}

/// NativeBridge.SetBotAi(playerPtr, v) -> true: флаг ИИ игрока (+0x318).
int l_bridge_set_bot_ai(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const unsigned int v = (unsigned int)luaL_checkinteger(L, 2);
    __try {
        *(unsigned int*)((unsigned char*)(ULONGLONG)ptr + kOffPlayerIsAi) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ★ Сборка 117: физика персонажа бота. CPE (CharacterPhysicsEntity) = [солдат+0x1D4]. Поля (разбор re\cpe\):
// +0x78 флаги (бит0 в мире Havok, бит1 physics-controlled, бит2 прокси), +0xE0 прокси-тело, +0xFC isPushable, +0xFD isAI,
// +0xFE «шагать только по сетевым мувам клиента» (= PhysicsSettings.SingleStepCharacter при создании солдата).
// У бота мувов нет, поэтому updateCharacter (0x00624460) не вызывается никогда — бот висит. Глобальный цикл 0x00676880
// шагает CPE при +0xFE == 0. PhysicsSettings — по указателю 0x02B1D6F4 (+0x22 SingleStepCharacter).
constexpr unsigned long kOffSoldierCpe = 0x1D4;
constexpr unsigned long kPhysicsSettingsPtrVa = 0x02B1D6F4UL;
static bool BotPhysEnabled() {
    static const bool on = [] {
        char b[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_BOT_PHYS", b, (DWORD)sizeof(b));
        return !(n == 1 && b[0] == '0');
    }();
    return on;
}

/// NativeBridge.BotPhysOn(playerPtr) -> прежнее значение CPE+0xFE (nil — нет солдата/CPE). Ставит +0xFE = 0.
/// Сборка 240: общее для BotPhysOn и BotTick — прежнее значение CPE+0xFE или -1 (нет солдата/CPE, ошибка чтения).
static int BotPhysOnRaw(unsigned long ptr) {
    unsigned long soldier = 0, cpe = 0;
    int old = -1;
    __try {
        soldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSoldier);
        if (soldier != 0) cpe = *(const unsigned long*)((const unsigned char*)(ULONGLONG)soldier + kOffSoldierCpe);
        if (cpe != 0) {
            unsigned char* c = (unsigned char*)(ULONGLONG)cpe;
            old = c[0xFE];
            if (old != 0) c[0xFE] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        old = -1;
    }
    static volatile LONG s_logged = 0;
    if (old > 0 && InterlockedIncrement(&s_logged) <= 40) {
        LOG_INFO("Bridge: ФИЗИКА бота %p: CPE %08lX +0xFE %d -> 0", (void*)(ULONGLONG)ptr, cpe, old);
    }
    return old;
}

int l_bridge_bot_phys_on(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    if (!BotPhysEnabled()) {
        lua_pushnil(L);
        return 1;
    }
    const int old = BotPhysOnRaw(ptr);
    if (old < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, old);
    return 1;
}

/// NativeBridge.CpeState(playerPtr) -> строка: поля CPE и солдата, PhysicsSettings.SingleStepCharacter.
int l_bridge_cpe_state(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    char out[384];
    unsigned long soldier = 0, cpe = 0, f78 = 0, e0 = 0, ps = 0;
    unsigned long physicsManager = 0, terrain = 0;
    float heightAccumulator = 0.0f;
    unsigned char fd = 0, fe = 0, fc = 0, s249 = 0, s24a = 0, ss = 0;
    __try {
        soldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSoldier);
        if (soldier != 0) {
            const unsigned char* so = (const unsigned char*)(ULONGLONG)soldier;
            cpe = *(const unsigned long*)(so + kOffSoldierCpe);
            s249 = so[0x249];
            s24a = so[0x24A];
        }
        if (cpe != 0) {
            const unsigned char* c = (const unsigned char*)(ULONGLONG)cpe;
            f78 = *(const unsigned long*)(c + 0x78);
            e0 = *(const unsigned long*)(c + 0xE0);
            fc = c[0xFC];
            fd = c[0xFD];
            fe = c[0xFE];
            // Сверено статически: 0062565B берёт CPE+38 для groundHeightClamped;
            // 00679790 читает manager+60; 006256A1 сохраняет накопленную высоту в CPE+214.
            physicsManager = *(const unsigned long*)(c + 0x38);
            if (physicsManager != 0)
                terrain = *(const unsigned long*)((const unsigned char*)(ULONGLONG)physicsManager + 0x60);
            heightAccumulator = *(const float*)(c + 0x214);
        }
        ps = *(const unsigned long*)(ULONGLONG)kPhysicsSettingsPtrVa;
        if (ps != 0) ss = *((const unsigned char*)(ULONGLONG)ps + 0x22);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushstring(L, "ошибка чтения CPE");
        return 1;
    }
    ::snprintf(out, sizeof(out), "солдат=%08lX cpe=%08lX +78=%08lX +E0=%08lX +FC=%u +FD=%u +FE=%u с249=%u с24A=%u SingleStep=%u physics=%08lX terrain=%08lX heightAcc=%.3f",
               soldier, cpe, f78, e0, fc, fd, fe, s249, s24a, ss, physicsManager, terrain, heightAccumulator);
    lua_pushstring(L, out);
    return 1;
}

/// ★ Сборка 168: NativeBridge.InputAim(playerPtr, yaw, pitch) -> true. Прицел мода в объект ввода бота одним вызовом
/// (+0x1F4 yaw, +0x1F8 pitch, +0x1E8 = 1 — AuthoritativeAiming); раньше — три InputWrite на бота за тик.
int l_bridge_input_aim(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const float yaw = (float)luaL_checknumber(L, 2);
    const float pitch = (float)luaL_checknumber(L, 3);
    if (!NonFiniteGuard(yaw, "InputAim yaw", 0) || !NonFiniteGuard(pitch, "InputAim pitch", 0)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    bool ok = false;
    __try {
        const unsigned long inp = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3C0);
        if (inp != 0) {
            unsigned char* d = (unsigned char*)(ULONGLONG)inp;
            *(float*)(d + 0x1F4) = yaw;
            *(float*)(d + 0x1F8) = pitch;
            *(unsigned long*)(d + 0x1E8) = 1UL;
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ★ Сборка 118: запись поля в свой объект ввода бота ([игрок+0x3C0]) — для прицела и флагов мода.
/// NativeBridge.InputWrite(playerPtr, off, value, isFloat) -> true.
int l_bridge_input_write(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const unsigned long off = (unsigned long)luaL_checkinteger(L, 2);
    const int isFloat = (int)luaL_optinteger(L, 4, 1);
    bool ok = false;
    __try {
        const unsigned long inp = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3C0);
        if (inp != 0 && off < 0x300) {
            unsigned char* d = (unsigned char*)(ULONGLONG)inp + off;
            if (isFloat != 0) {
                const double fv = luaL_checknumber(L, 3);
                if (!NonFiniteGuard(fv, "InputWrite", (long)off)) __leave;
                *(float*)d = (float)fv;
            }
            else *(unsigned long*)d = (unsigned long)luaL_checkinteger(L, 3);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ★ Сборка 121: серверная линия прямой видимости — GameWorld::rayCast (VA 0x0085C130, thiscall, ret 0x1C; разбор
// re\ray\server_raycast.h). Вызов только из хука тика: Lua ставит запрос (LosRequest), хук выполняет (RunLosQueue),
// Lua забирает результат (LosResult). Тип 1 (collision), флаги 0x34 = DontCheckWater|DontCheckRagdoll|DontCheckCharacter.
namespace losray {
constexpr unsigned long kRayCastRva = 0x0045C130UL;    // VA 0x0085C130
constexpr unsigned long kGameWorldRva = 0x0273EE30UL;  // VA 0x02B3EE30 (g_serverGameWorld)
constexpr int kSlots = 64;
constexpr unsigned long kFlags = 0x34u;
static const char kIdent[] = "EVBots::losRayCast";
struct alignas(16) Vec4f { float x, y, z, w; };
struct alignas(16) HitRec {
    Vec4f position;
    Vec4f normal;
    void* rigidBody;
    uint32_t material;
    int32_t part;
    int32_t bone;
    float lambda;
    uint8_t pad[0x0C];
};
static_assert(sizeof(HitRec) == 0x40, "HitRec size");
struct ExcludeRec { void* const* begin; void* const* end; };
typedef bool(__thiscall* RayCastFn)(void* world, const char* ident, int type, const Vec4f* from, const Vec4f* to,
                                    HitRec* hit, uint32_t flags, const ExcludeRec* ex);
// state: 0 свободен, 1 ждёт выполнения, 2 чисто, 3 закрыто, 4 ошибка, 5 заполняется
// ID относится к запросу, а не к слоту: просроченный потребитель не должен забирать чужой результат.
struct Slot { volatile LONG state; lua_Integer id; float a[3]; float b[3]; ULONGLONG doneAt; };
static Slot g_slots[kSlots];
static lua_Integer g_nextId = 0;  // меняется только из Lua, как и LosResult
}  // namespace losray

/// NativeBridge.LosRequest(ax,ay,az,bx,by,bz) -> уникальный id | nil (очередь полна).
int l_bridge_los_request(lua_State* L) {
    float v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = (float)luaL_checknumber(L, i + 1);
        if (!std::isfinite(v[i])) {
            lua_pushnil(L);  // NaN/Inf нельзя передавать в физику движка
            return 1;
        }
    }
    if (losray::g_nextId == LUA_MAXINTEGER) {
        lua_pushnil(L);  // не переиспользовать ID даже при исчерпании счётчика
        return 1;
    }
    for (int i = 0; i < losray::kSlots; ++i) {
        losray::Slot& s = losray::g_slots[i];
        if (InterlockedCompareExchange(&s.state, 5, 0) != 0) continue;
        for (int k = 0; k < 3; ++k) { s.a[k] = v[k]; s.b[k] = v[3 + k]; }
        s.id = ++losray::g_nextId;
        InterlockedExchange(&s.state, 1);
        lua_pushinteger(L, s.id);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/// NativeBridge.LosResult(id) -> nil (ещё ждёт) | true (чисто) | false (закрыто, ошибка, просрочено). Слот освобождается.
int l_bridge_los_result(lua_State* L) {
    const lua_Integer id = luaL_checkinteger(L, 1);
    for (int i = 0; id > 0 && i < losray::kSlots; ++i) {
        losray::Slot& s = losray::g_slots[i];
        const LONG st = InterlockedCompareExchange(&s.state, 0, 0);
        if (st == 0 || st == 5 || s.id != id) continue;
        if (st == 1) {
            lua_pushnil(L);
            return 1;
        }
        InterlockedExchange(&s.state, 0);
        lua_pushboolean(L, st == 2 ? 1 : 0);
        return 1;
    }
    lua_pushboolean(L, 0);  // просрочен или уже забран; чужой слот не трогаем
    return 1;
}

/// Вызывается из хука тика (контекст движка): выполняет запросы LOS на серверном мире.
void RunLosQueue() {
    using namespace losray;
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return;
    void* world = nullptr;
    __try {
        world = *(void* const*)(base + kGameWorldRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        world = nullptr;
    }
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        const LONG st = InterlockedCompareExchange(&s.state, 0, 0);
        if (st == 1) {
            LONG outc = 4;
            if (world != nullptr && InterlockedCompareExchange(&g_levelGate, 0, 0) == 0) {
                Vec4f f = { s.a[0], s.a[1], s.a[2], 0.0f };
                Vec4f t = { s.b[0], s.b[1], s.b[2], 0.0f };
                if (f.x == t.x && f.y == t.y && f.z == t.z) {
                    outc = 2;
                } else {
                    HitRec h = {};
                    RayCastFn fn = (RayCastFn)(base + kRayCastRva);
                    __try {
                        const bool hit = fn(world, kIdent, 1, &f, &t, &h, kFlags, nullptr);
                        outc = hit ? 3 : 2;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        outc = 4;
                    }
                }
            }
            s.doneAt = now;
            InterlockedExchange(&s.state, outc);
        } else if ((st == 2 || st == 3 || st == 4) && now - s.doneAt > 5000) {
            InterlockedExchange(&s.state, 0);
        }
    }
}

// 262: updateCharacter queried terrain even on mesh-only levels (MP_011).
// At this single call site ESI is the current CPE. With no terrain, use the
// existing world collision query beneath that character instead of a zero height.
// All other callers and the terrain-present path retain the engine implementation.
static float __fastcall CharacterGroundHeight(void* manager, void* cpe, float x, float z) {
    using GroundFn = float(__thiscall*)(void*, float, float);
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return 0.0f;
    __try {
        const auto* pm = static_cast<const unsigned char*>(manager);
        const auto* character = static_cast<const unsigned char*>(cpe);
        if (manager != nullptr && cpe != nullptr &&
            *(void* const*)(pm + 0x60) == nullptr &&
            *(void* const*)(character + 0x38) == manager &&
            InterlockedCompareExchange(&g_levelGate, 0, 0) == 0 &&
            std::isfinite(x) && std::isfinite(z)) {
            const auto* body = *(const unsigned char* const*)(character + 0x6C);
            void* world = *(void* const*)(base + losray::kGameWorldRva);
            if (body != nullptr && world != nullptr) {
                const float y = *(const float*)(body + 0xE4);
                if (std::isfinite(y)) {
                    losray::Vec4f from = {x, y + 0.25f, z, 0.0f};
                    losray::Vec4f to = {x, y - 4096.0f, z, 0.0f};
                    losray::HitRec hit = {};
                    const bool found = ((losray::RayCastFn)(base + losray::kRayCastRva))(
                        world, "EVBots::characterGround", 1, &from, &to, &hit, losray::kFlags, nullptr);
                    if (found && std::isfinite(hit.position.y) &&
                        hit.position.y <= from.y && hit.position.y >= to.y) return hit.position.y;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // No valid world/body/hit: preserve the original behavior and its warning.
    }
    return ((GroundFn)(base + 0x00279790UL))(manager, x, z);
}

static __declspec(naked) void CharacterGroundThunk() {
    __asm {
        mov edx, esi
        jmp CharacterGroundHeight
    }
}

static void InstallCharacterGroundOnce() {
    static LONG attempted = 0;
    if (attempted != 0) return;
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return;
    InterlockedExchange(&attempted, 1);
    char setting[4] = {};
    if (::GetEnvironmentVariableA("EVBOTS_GROUND_FALLBACK", setting, sizeof(setting)) == 1 && setting[0] == '0') {
        LOG_INFO("Bridge: character ground fallback disabled (EVBOTS_GROUND_FALLBACK=0)");
        return;
    }
    // Capstone + saved Ghidra decompilation: CPE in ESI, manager in ECX,
    // X/Z on the stack. Verify the entire preparation, including the original CALL.
    static const unsigned char context[] = {
        0x8B,0x46,0x6C, 0xD9,0x80,0xE4,0x00,0x00,0x00, 0x8B,0x4E,0x38,
        0x05,0xB0,0x00,0x00,0x00, 0xDD,0x5C,0x24,0x30, 0xD9,0x40,0x38,
        0x83,0xEC,0x08, 0xD9,0x5C,0x24,0x04, 0xD9,0x40,0x30,
        0xD9,0x1C,0x24, 0xE8,0x14,0x41,0x05,0x00
    };
    bool same = false;
    __try { same = std::memcmp(base + 0x00225652UL, context, sizeof(context)) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
    if (!same) {
        LOG_ERR("Bridge: character ground 00625652: context mismatch, no patch written");
        return;
    }
    unsigned char* site = base + 0x00225674UL;
    static const unsigned char expected[8] = {0xD9,0x1C,0x24,0xE8,0x14,0x41,0x05,0x00};
    unsigned char replacement[8];
    std::memcpy(replacement, expected, sizeof(replacement));
    const uint32_t relative = (uint32_t)(uintptr_t)&CharacterGroundThunk - (uint32_t)(uintptr_t)(site + 8);
    std::memcpy(replacement + 4, &relative, sizeof(relative));
    // One atomic eight-byte patch through ev-core: same FSTP, redirected CALL.
    if (!PatchBytes(site, expected, replacement, sizeof(replacement), "character ground fallback")) {
        LOG_ERR("Bridge: character ground 00625674: patch not installed");
        return;
    }
    LOG_INFO("Bridge: character ground 00625674: collision height fallback installed (262)");
}

/// NativeBridge.SetBotAppearance(ptr, name): запоминает выбор внешности мода и сразу применяет; на спавне — ещё раз.
int l_bridge_set_bot_appearance(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const char* name = luaL_optstring(L, 2, "");
    RememberBotAppearance(ptr, name);
    ApplyBotAppearance(ptr);
    lua_pushboolean(L, 1);
    return 1;
}

/// NativeBridge.SetBotKit(ptr, kitName): класс бота по имени кита мода («Gameplay/Kits/USAssault» -> Assault).
/// Фракция берётся по команде в движке при спавне (ApplyBotKitClass).
int l_bridge_set_bot_kit(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const char* name = luaL_optstring(L, 2, "");
    const int cls = ClassFromKitName(name);
    if (cls < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    RememberBotClass(ptr, cls);
    // сразу, если у бота уже есть команда в движке; на спавне применится ещё раз
    if (InEngineTick()) ApplyBotKitClass(ptr);
    lua_pushboolean(L, 1);
    return 1;
}

// Счётчик QPC — замеры времени поиска ассетов (сборка 160; сам скан памяти удалён в 169).
static unsigned long long QpcNow() {
    LARGE_INTEGER v;
    ::QueryPerformanceCounter(&v);
    return (unsigned long long)v.QuadPart;
}
static unsigned long long QpcFreq() {
    static unsigned long long s_f = 0;
    if (s_f == 0) {
        LARGE_INTEGER v;
        ::QueryPerformanceFrequency(&v);
        s_f = (unsigned long long)v.QuadPart;
        if (s_f == 0) s_f = 1000000ULL;
    }
    return s_f;
}

// Короткое имя объекта по [obj+8]: часть после последнего «/» (или адрес).
static void ShortObjName(unsigned long obj, char* out, size_t cap) {
    char nm[160];
    if (ReadObjName(obj, nm, sizeof(nm))) {
        const char* slash = ::strrchr(nm, '/');
        ::snprintf(out, cap, "%s", (slash != nullptr && slash[1] != '\0') ? slash + 1 : nm);
    } else {
        ::snprintf(out, cap, "%08lX", obj);
    }
}

/// ★ Сборка 152 (из 136L): NativeBridge.BotWeaponNames(ptr) -> строка: что лежит в векторе слотов оружия игрока
/// (+0x50C) — из этого движок создаёт оружие на спавне: «n=10 0=U_M4A1[RX01,NoCamo] 1=U_M9 …».
int l_bridge_bot_weapon_names(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long begin = 0, end = 0, n = 0;
    unsigned long obj[kWpnSlots] = {};
    unsigned long att[kWpnSlots][kWpnMaxAtt] = {};
    unsigned long attN[kWpnSlots] = {};
    __try {
        begin = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffWpnBegin);
        end = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffWpnEnd);
        if (begin != 0 && end > begin) n = (end - begin) / kWpnEntrySize;
        for (unsigned long s = 0; s < n && s < kWpnSlots; ++s) {
            const unsigned char* e = (const unsigned char*)(ULONGLONG)(begin + s * kWpnEntrySize);
            obj[s] = *(const unsigned long*)e;
            attN[s] = *(const unsigned long*)(e + 0x20);
            for (unsigned long j = 0; j < attN[s] && j < kWpnMaxAtt; ++j) att[s][j] = *(const unsigned long*)(e + 4 + j * 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushstring(L, "слоты не читаются");
        return 1;
    }
    char buf[900];
    int len = ::snprintf(buf, sizeof(buf), "n=%lu", n);
    for (unsigned long s = 0; s < n && s < kWpnSlots && len > 0 && len < (int)sizeof(buf) - 96; ++s) {
        if (obj[s] == 0) continue;
        char sn[96];
        ShortObjName(obj[s], sn, sizeof(sn));
        len += ::snprintf(buf + len, sizeof(buf) - len, " %lu=%s", s, sn);
        if (attN[s] > 0 && len < (int)sizeof(buf) - 96) {
            len += ::snprintf(buf + len, sizeof(buf) - len, "[");
            for (unsigned long j = 0; j < attN[s] && j < kWpnMaxAtt && len < (int)sizeof(buf) - 64; ++j) {
                char an[96];
                ShortObjName(att[s][j], an, sizeof(an));
                len += ::snprintf(buf + len, sizeof(buf) - len, "%s%s", j ? "," : "", an);
            }
            len += ::snprintf(buf + len, sizeof(buf) - len, "]");
        }
    }
    lua_pushstring(L, buf);
    return 1;
}

/// NativeBridge.SoldierPos(playerPtr) -> x, y, z солдата из движка (nil — солдата нет).
/// Позиция — translation матрицы трансформа солдата, то же место, что ставит setTransform (см. ApplyBotPosition).
/// Сборка 90: по ней мост обновляет Lua-позицию и считает скорость, чтобы мод видел реальное движение.
static bool ReadSoldierPos(unsigned long ptr, float* px, float* py, float* pz);

int l_bridge_soldier_pos(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!ReadSoldierPos(ptr, &x, &y, &z)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    lua_pushnumber(L, (lua_Number)z);
    return 3;
}

/// ★ Сборка 240: NativeBridge.BotTick(playerPtr, phys, yaw, pitch) -> x, y, z, vx, vy, vz, attached, controlled
/// Тик моста на бота одним вызовом вместо пяти pcall (BotPhysOn, InputAim, SoldierPos, SoldierVel, PlayerVehicle): с 239 Lua — в
/// кадре движка, и раздел «позиции» был 86 % моста (профиль 26.10: 6–8 вызовов C через pcall на бота за тик).
/// phys — снять физику персонажа (как BotPhysOn, при EVBOTS_BOT_PHYS); yaw/pitch (nil — не писать) — как InputAim.
/// Позиция — nil, nil, nil, если солдата нет/вне карты (как SoldierPos); скорость — nil ×3 (как SoldierVel); машина — как
/// PlayerVehicleV (не техника — 0), nil ×2 при ошибке чтения.
int l_bridge_bot_tick(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const bool phys = lua_toboolean(L, 2) != 0;
    if (phys && BotPhysEnabled()) BotPhysOnRaw(ptr);
    if (lua_type(L, 3) == LUA_TNUMBER) {
        const float yaw = (float)lua_tonumber(L, 3);
        const float pitch = (float)luaL_optnumber(L, 4, 0.0);
        if (NonFiniteGuard(yaw, "InputAim yaw", 0) && NonFiniteGuard(pitch, "InputAim pitch", 0)) {
            __try {
                const unsigned long inp = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + 0x3C0);
                if (inp != 0) {
                    unsigned char* d = (unsigned char*)(ULONGLONG)inp;
                    *(float*)(d + 0x1F4) = yaw;
                    *(float*)(d + 0x1F8) = pitch;
                    *(unsigned long*)(d + 0x1E8) = 1UL;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (ReadSoldierPos(ptr, &x, &y, &z)) {
        lua_pushnumber(L, (lua_Number)x);
        lua_pushnumber(L, (lua_Number)y);
        lua_pushnumber(L, (lua_Number)z);
    } else {
        lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
    }
    float v[3] = {0.0f, 0.0f, 0.0f};
    if (ReadSoldierVel(ptr, v)) {
        lua_pushnumber(L, (lua_Number)v[0]);
        lua_pushnumber(L, (lua_Number)v[1]);
        lua_pushnumber(L, (lua_Number)v[2]);
    } else {
        lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
    }
    unsigned long a = 0, e = 0, c = 0;
    if (ReadPlayerVehicle(ptr, &a, &e, &c)) {
        if (a != 0 && !IsVehicleEntity(a)) a = 0;
        if (c != 0 && !IsVehicleEntity(c)) c = 0;
        lua_pushinteger(L, (lua_Integer)a);
        lua_pushinteger(L, (lua_Integer)c);
    } else {
        lua_pushnil(L); lua_pushnil(L);
    }
    return 8;
}

static bool ReadSoldierPos(unsigned long ptr, float* px, float* py, float* pz) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool ok = false;
    __try {
        unsigned long soldier = 0;
        if (ptr != 0) soldier = *(const unsigned long*)((const unsigned char*)(ULONGLONG)ptr + kOffPlayerSoldier);
        if (soldier != 0) {
            const unsigned char* s = (const unsigned char*)(ULONGLONG)soldier;
            const unsigned long trp = *(const unsigned long*)(s + 0x1C);
            if (trp != 0) {
                const unsigned char* tr = (const unsigned char*)(ULONGLONG)trp;
                if (((*(const unsigned long*)(s + 8) >> 18) & 1u) != 0) {
                    tr = tr + 0x10 + (unsigned long)tr[6] * 0x50;
                }
                x = *(const float*)(tr + 0x30);
                y = *(const float*)(tr + 0x34);
                z = *(const float*)(tr + 0x38);
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    // NaN и значения вне карты отсекаем (ok=false не пропускает NaN: сравнения с NaN всегда ложны)
    if (!ok || !(x > -5000.0f && x < 5000.0f) || !(z > -5000.0f && z < 5000.0f) || !(y > -1000.0f && y < 5000.0f)) {
        return false;
    }
    *px = x;
    *py = y;
    *pz = z;
    return true;
}

/// NativeBridge.SetBotSquad(ptr, squadId): отряд, который мод выбрал до появления движкового игрока
int l_bridge_set_bot_squad(lua_State* L) {
    RememberBotSquad((unsigned long)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2));
    return 0;
}

/// NativeBridge.EngineSquad(ptr) -> отряд игрока в движке (-1 — не прочитать)
int l_bridge_engine_squad(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)EngineSquadOf((unsigned long)luaL_checkinteger(L, 1)));
    return 1;
}

/// NativeBridge.SquadExtentOffset() -> смещение подобъекта отряда в игроке ([0x0285BAD8]) | nil.
/// Сборка 196, диагностика: у VU (клиентский bf3.exe) подобъект на +0x420: +0x4C отряд, +0x50 лидер, +0x51 закрыт,
/// +0x52 isAllowedToSpawnOn (veniceext 0x107cce20/0x107ccea4). Сверяем, та ли раскладка у нашего серверного exe.
int l_bridge_squad_extent_offset(lua_State* L) {
    unsigned char* base = NativeModuleBase();
    unsigned long off = 0;
    bool ok = false;
    if (base != nullptr) {
        __try {
            off = *(const unsigned long*)(base + kSquadSubOffsetRva);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    }
    if (!ok) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)off);
    return 1;
}

/// NativeBridge.PlayerSpawnOn(ptr) -> isAllowedToSpawnOn (bool), отряд из поля [ext+0xF4] | nil.
/// Сборка 197: подобъект отряда сервера ext = игрок + [0x0285BAD8] (прогон 196: 0x690). Сверено по коду exe: сеттер
/// isAllowedToSpawnOn 0x01171490 (`cmp/mov [esi+0xFB], bl`, событие AutoTeam DisableAllowedToSpawnOn → 0x012EF640(игрок, 0)),
/// геттеры 0x011714C0 отряд [+0xF4], 0x011714D0 лидер [+0xF8], 0x011714E0 закрытый отряд [+0xF9].
int l_bridge_player_spawn_on(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    unsigned char* base = NativeModuleBase();
    if (base == nullptr || ptr == 0) { lua_pushnil(L); return 1; }
    unsigned char v = 0;
    unsigned long sq = 0;
    __try {
        const unsigned long off = *(const unsigned long*)(base + kSquadSubOffsetRva);
        if (off == 0 || off > 0x2000) { lua_pushnil(L); return 1; }
        const unsigned char* ext = (const unsigned char*)(ULONGLONG)(ptr + off);
        v = ext[0xFB];
        sq = *(const unsigned long*)(ext + 0xF4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, v != 0 ? 1 : 0);
    lua_pushinteger(L, (lua_Integer)sq);
    return 2;
}

/// NativeBridge.CreateEngineSoldier(playerPtr) -> ptr солдата или 0
/// Создание НАСТОЯЩЕГО SoldierEntity движка. Пока адрес функции создания
/// солдата не разобран — возвращаем 0, и Lua-обёртка уходит в фолбэк.
int l_bridge_create_soldier(lua_State* L) {
    (void)luaL_checkinteger(L, 1);
    LOG_INFO("Bridge: CreateEngineSoldier ВЫЗВАНА (адрес создания солдата "
             "ещё не разобран — возвращаю 0)");
    lua_pushinteger(L, 0);
    return 1;
}

/// NativeBridge.HasSoldier(ptr) -> 0/1
/// NativeBridge.PollDeadBots() -> {ptr, ptr, ...}: боты, у которых с прошлого опроса
/// пропал солдат (смерть). Очередь заполняет наблюдение в EVBotsBridgeRetryPending.
int l_bridge_poll_dead(lua_State* L) {
    lua_newtable(L);
    const LONG n = g_deadCount;
    for (LONG i = 0; i < n && i < 64; ++i) {
        lua_pushinteger(L, (lua_Integer)g_deadQueue[i]);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    g_deadCount = 0;
    return 1;
}

/// NativeBridge.PollCreatedBots() -> { [имя] = ptr, ... }: боты, созданные очередью
/// с прошлого опроса. Lua проставляет им nativePtr.
int l_bridge_poll_created(lua_State* L) {
    lua_newtable(L);
    const LONG n = g_createdCount;
    for (LONG i = 0; i < n && i < 64; ++i) {
        lua_pushinteger(L, (lua_Integer)g_createdBots[i].ptr);
        lua_setfield(L, -2, g_createdBots[i].name);
    }
    g_createdCount = 0;
    return 1;
}

int l_bridge_has_soldier(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    if (ptr == 0 || !Readable((void*)ptr)) {
        lua_pushinteger(L, 0);
        return 1;
    }
    unsigned long soldier = 0;
    __try {
        soldier = *(const unsigned long*)((const unsigned char*)ptr +
                                          kOffPlayerSoldier);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        soldier = 0;
    }
    lua_pushinteger(L, soldier != 0 ? 1 : 0);
    return 1;
}

// Регистрирует таблицу NativeBridge в состоянии Lua.
void RegisterBridge(lua_State* L) {
    lua_newtable(L);

    lua_pushcfunction(L, l_bridge_create_player);
    lua_setfield(L, -2, "CreateEnginePlayer");

    lua_pushcfunction(L, l_bridge_spawn_player);
    lua_setfield(L, -2, "SpawnEnginePlayer");

    lua_pushcfunction(L, l_bridge_set_input);
    lua_setfield(L, -2, "SetInputLevel");

    lua_pushcfunction(L, l_bridge_create_soldier);
    lua_setfield(L, -2, "CreateEngineSoldier");

    lua_pushcfunction(L, l_bridge_kill_player);
    lua_setfield(L, -2, "KillEnginePlayer");

    lua_pushcfunction(L, l_bridge_player_score);  // сборка 171: Player.score/kills/deaths как VU
    lua_setfield(L, -2, "PlayerScore");

    lua_pushcfunction(L, l_bridge_reset_round_stats);  // сборка 266: очки/убийства/смерти — статистика раунда, как VU
    lua_setfield(L, -2, "ResetRoundStats");

    lua_pushcfunction(L, l_bridge_tests_enabled);
    lua_setfield(L, -2, "TestsEnabled");



    lua_pushcfunction(L, l_bridge_read_u32);
    lua_setfield(L, -2, "ReadU32");

    lua_pushcfunction(L, l_bridge_engine_player_id);  // ★ 231: Player.id = id движка, как VU
    lua_setfield(L, -2, "EnginePlayerId");

    lua_pushcfunction(L, l_bridge_read_cstr);
    lua_setfield(L, -2, "ReadCStr");


    lua_pushcfunction(L, l_bridge_real_player);
    lua_setfield(L, -2, "RealPlayer");

    lua_pushcfunction(L, l_bridge_human_ptr);
    lua_setfield(L, -2, "HumanPtr");
    lua_pushcfunction(L, l_bridge_humans);  // сборка 202: все люди сервера
    lua_setfield(L, -2, "Humans");
    lua_pushcfunction(L, l_bridge_real_player_name);
    lua_setfield(L, -2, "RealPlayerName");
    lua_pushcfunction(L, l_bridge_test_log);
    lua_setfield(L, -2, "TestLog");
    lua_pushcfunction(L, l_bridge_bot_kit_items);
    lua_setfield(L, -2, "BotKitItems");

    lua_pushcfunction(L, l_bridge_join_squad);
    lua_setfield(L, -2, "JoinSquad");



    lua_pushcfunction(L, l_bridge_set_bot_pos);
    lua_setfield(L, -2, "SetBotPos");

    lua_pushcfunction(L, l_bridge_take_base_spawn);
    lua_setfield(L, -2, "TakeBaseSpawn");

    lua_pushcfunction(L, l_bridge_set_bot_squad);
    lua_setfield(L, -2, "SetBotSquad");

    lua_pushcfunction(L, l_bridge_clear_bot_pos);
    lua_setfield(L, -2, "ClearBotPos");

    lua_pushcfunction(L, l_bridge_delete_player);
    lua_setfield(L, -2, "DeleteEnginePlayer");

    lua_pushcfunction(L, l_bridge_clear_bot_kits);  // сборка 265: отпустить кит +0x480 перед выгрузкой уровня
    lua_setfield(L, -2, "ClearBotKits");

    lua_pushcfunction(L, l_bridge_forget_bot);
    lua_setfield(L, -2, "ForgetEngineBot");

    lua_pushcfunction(L, l_bridge_vu_spawn_mode);
    lua_setfield(L, -2, "VuSpawnMode");

    lua_pushcfunction(L, l_bridge_bot_life_fields);
    lua_setfield(L, -2, "BotLifeFields");

    lua_pushcfunction(L, l_bridge_set_bot_kit);
    lua_setfield(L, -2, "SetBotKit");

    lua_pushcfunction(L, l_bridge_set_bot_appearance);
    lua_setfield(L, -2, "SetBotAppearance");

    lua_pushcfunction(L, l_bridge_set_bot_weapons);
    lua_setfield(L, -2, "SetBotWeapons");

    lua_pushcfunction(L, l_bridge_capture_human_soldier);
    lua_setfield(L, -2, "CaptureHumanSoldier");
    lua_pushcfunction(L, l_bridge_apply_human_soldier_customization);
    lua_setfield(L, -2, "ApplyHumanSoldierCustomization");
    lua_pushcfunction(L, l_bridge_set_human_soldier_transform);
    lua_setfield(L, -2, "SetHumanSoldierTransform");
    lua_pushcfunction(L, l_bridge_request_human_parachute);
    lua_setfield(L, -2, "RequestHumanParachute");
    lua_pushcfunction(L, l_bridge_read_human_state);
    lua_setfield(L, -2, "ReadHumanState");

    lua_pushcfunction(L, l_bridge_set_bot_perks);
    lua_setfield(L, -2, "SetBotPerks");
    lua_pushcfunction(L, l_bridge_set_choice_by_name);  // сборка 159: выбор мода по имени (до привязки)
    lua_setfield(L, -2, "SetChoiceByName");





    lua_pushcfunction(L, l_bridge_bot_own_input);
    lua_setfield(L, -2, "BotOwnInput");



    lua_pushcfunction(L, l_bridge_input_level_of);
    lua_setfield(L, -2, "InputLevelOf");

    lua_pushcfunction(L, l_bridge_input_aim_of);
    lua_setfield(L, -2, "InputAimOf");

    lua_pushcfunction(L, l_bridge_vel_scan);
    lua_setfield(L, -2, "VelScan");

    lua_pushcfunction(L, l_bridge_vel_scan_deep);
    lua_setfield(L, -2, "VelScanDeep");

    lua_pushcfunction(L, l_bridge_soldier_vel);
    lua_setfield(L, -2, "SoldierVel");

    lua_pushcfunction(L, l_bridge_bot_tick);
    lua_setfield(L, -2, "BotTick");

    lua_pushcfunction(L, l_bridge_player_vehicle_v);
    lua_setfield(L, -2, "PlayerVehicleV");

    lua_pushcfunction(L, l_bridge_player_on_foot);
    lua_setfield(L, -2, "PlayerOnFoot");

    lua_pushcfunction(L, l_bridge_env_flag);
    lua_setfield(L, -2, "EnvFlag");

    lua_pushcfunction(L, l_bridge_los_request);
    lua_setfield(L, -2, "LosRequest");

    lua_pushcfunction(L, l_bridge_los_result);
    lua_setfield(L, -2, "LosResult");

    lua_pushcfunction(L, l_bridge_teleport_bot);
    lua_setfield(L, -2, "TeleportBot");

    lua_pushcfunction(L, l_bridge_set_bot_pose);
    lua_setfield(L, -2, "SetBotPose");

    lua_pushcfunction(L, l_bridge_get_bot_pose);
    lua_setfield(L, -2, "GetBotPose");

    lua_pushcfunction(L, l_bridge_player_vehicle);
    lua_setfield(L, -2, "PlayerVehicle");

    lua_pushcfunction(L, l_bridge_enter_vehicle);
    lua_setfield(L, -2, "EnterVehicle");

    lua_pushcfunction(L, l_bridge_veh_seat_queued);
    lua_setfield(L, -2, "VehSeatQueued");

    lua_pushcfunction(L, l_bridge_exit_vehicle);
    lua_setfield(L, -2, "ExitVehicle");

    lua_pushcfunction(L, l_bridge_weapon_cur_slot);
    lua_setfield(L, -2, "WeaponCurSlot");

    lua_pushcfunction(L, l_bridge_weapon_ammo);
    lua_setfield(L, -2, "WeaponAmmo");

    lua_pushcfunction(L, l_bridge_set_weapon_ammo);
    lua_setfield(L, -2, "SetWeaponAmmo");


    lua_pushcfunction(L, l_bridge_set_bot_ai);
    lua_setfield(L, -2, "SetBotAi");



    lua_pushcfunction(L, l_bridge_bot_phys_on);
    lua_setfield(L, -2, "BotPhysOn");

    lua_pushcfunction(L, l_bridge_cpe_state);
    lua_setfield(L, -2, "CpeState");

    lua_pushcfunction(L, l_bridge_input_aim);  // сборка 168
    lua_setfield(L, -2, "InputAim");
    lua_pushcfunction(L, l_bridge_input_write);
    lua_setfield(L, -2, "InputWrite");


    lua_pushcfunction(L, l_bridge_bot_weapon_names);  // сборка 152: оружие в слотах движка
    lua_setfield(L, -2, "BotWeaponNames");
    lua_pushcfunction(L, l_bridge_poll_damage);  // сборка 153: события урона для хука Soldier:Damage
    lua_setfield(L, -2, "PollDamage");

    lua_pushcfunction(L, l_bridge_bullet_call_original);  // сборка 163
    lua_setfield(L, -2, "BulletCallOriginal");
    lua_pushcfunction(L, l_bridge_poll_chat);  // сборка 159: входящий чат (EVBOTS_CHAT_EVENTS)
    lua_setfield(L, -2, "PollChat");
    lua_pushcfunction(L, l_bridge_damage_stats);
    lua_setfield(L, -2, "DamageStats");
    lua_pushcfunction(L, l_bridge_level_reload);  // сборка 167
    lua_setfield(L, -2, "LevelReload");
    lua_pushcfunction(L, l_bridge_level_reset);
    lua_setfield(L, -2, "ResetLevelState");
    lua_pushcfunction(L, l_bridge_level_info);  // сборка 185: уровень и режим из движка
    lua_setfield(L, -2, "LevelInfo");
    lua_pushcfunction(L, l_bridge_level_signals);  // сборка 186
    lua_setfield(L, -2, "LevelSignals");
    lua_pushcfunction(L, l_bridge_set_level_gate);  // сборка 186
    lua_setfield(L, -2, "SetLevelGate");
    lua_pushcfunction(L, l_bridge_level_changing);  // сборка 214
    lua_setfield(L, -2, "LevelChanging");
    lua_pushcfunction(L, l_bridge_poll_stat_events);  // сборка 186
    lua_setfield(L, -2, "PollStatEvents");
    lua_pushcfunction(L, l_bridge_poll_orders);  // сборка 193: приказы колеса BF3
    lua_setfield(L, -2, "PollOrders");
    lua_pushcfunction(L, l_bridge_gun_sway);  // сборка 186
    lua_setfield(L, -2, "GunSway");
    lua_pushcfunction(L, l_bridge_corpse);  // сборка 185: труп игрока
    lua_setfield(L, -2, "Corpse");
    lua_pushcfunction(L, l_bridge_poll_combat_area);  // сборка 185: выход за границу зоны боя
    lua_setfield(L, -2, "PollCombatArea");

    lua_pushcfunction(L, l_bridge_soldier_pos);
    lua_setfield(L, -2, "SoldierPos");


    lua_pushcfunction(L, l_bridge_engine_squad);
    lua_setfield(L, -2, "EngineSquad");

    lua_pushcfunction(L, l_bridge_squad_extent_offset);
    lua_setfield(L, -2, "SquadExtentOffset");

    lua_pushcfunction(L, l_bridge_player_spawn_on);
    lua_setfield(L, -2, "PlayerSpawnOn");
    lua_pushcfunction(L, l_bridge_player_team_property);
    lua_setfield(L, -2, "PlayerTeamProperty");
    lua_pushcfunction(L, l_bridge_select_human_unlock_assets);
    lua_setfield(L, -2, "SelectHumanUnlockAssets");

    lua_pushcfunction(L, l_bridge_poll_dead);
    lua_setfield(L, -2, "PollDeadBots");

    lua_pushcfunction(L, l_bridge_poll_created);
    lua_setfield(L, -2, "PollCreatedBots");

    lua_pushcfunction(L, l_bridge_has_soldier);
    lua_setfield(L, -2, "HasSoldier");

    lua_pushcfunction(L, l_bridge_poll_dead);
    lua_setfield(L, -2, "PollDeadBots");

    lua_pushcfunction(L, l_bridge_poll_created);
    lua_setfield(L, -2, "PollCreatedBots");

    lua_pushcfunction(L, l_bridge_has_soldier);
    lua_setfield(L, -2, "HasSoldier");

    lua_setglobal(L, "NativeBridge");

    LOG_INFO("EVBots: NativeBridge (Lua -> движок BF3) зарегистрирован");
}

}  // namespace

// ★ Сборка 160: сброс кэшей ассетов при смене уровня — зовёт NativeBridge.LevelReload (сборка 167). Вне анонимного
// namespace (иначе внутреннее связывание и LNK2019). Сборка 169: кэши скана памяти удалены вместе со сканом.
void ResetAssetCaches() {
    g_kitScanned = false;  // ★ 164: киты нового уровня — заново (раньше не сбрасывались никогда)
}

// ★ TeamSquadManager (сборка 78): настоящие счётчики вместо 0. fun-bots выбирает отряд бота
// по GetSquadPlayerCount < 4 (BotSpawner:_GetSquadToJoin) — без счётчиков все боты шли в отряд 1.
// Ботов считает Lua по Player.squadId (в VU запись squadId действует сразу, а у нас бот входит в отряд
// движка только после спавна в хуке тика). Здесь — люди: живой игрок из движка (команда +0x31C,
// отряд — геттер движка EngineSquadOf). Вне анонимного namespace: их зовёт VextTeamSquadManager.cpp.
namespace {
// ★ Сборка 202: все люди сервера (раньше — только первый, RealPlayer: отряд второго человека для мода был пустым).
template <typename Fn>
void ForEachEngineHuman(Fn fn) {
    if (NativeModuleBase() == nullptr) return;
    unsigned long hs[64];
    const int nh = EVBotsEngineHumans(hs, 64);
    for (int k = 0; k < nh; ++k) {
        const unsigned long human = hs[k];
        bool bot = false;
        for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
            if (g_watchBot[i] == human) { bot = true; break; }  // это бот
        }
        if (!bot) fn(human);
    }
}
unsigned int EngineTeamOf(unsigned long player) {
    unsigned int team = 0;
    __try {
        team = *(const unsigned int*)((const unsigned char*)(ULONGLONG)player + kOffPlayerTeam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        team = 0;
    }
    return team;
}
}  // namespace

int EVBotsCountSquadHumans(unsigned int team, unsigned int squad) {
    int n = 0;
    if (team == 0 || squad == 0) return 0;
    ForEachEngineHuman([&](unsigned long p) {
        if (EngineTeamOf(p) == team && EngineSquadOf(p) == (int)squad) ++n;
    });
    return n;
}

int EVBotsCountTeamHumans(unsigned int team) {
    int n = 0;
    if (team == 0) return 0;
    ForEachEngineHuman([&](unsigned long p) {
        if (EngineTeamOf(p) == team) ++n;
    });
    return n;
}

// ★ Сборка 124: блюпринт солдата Characters/Soldiers/MpSoldier (= +0x480 человека после дислокации; тот же объект,
// которым fun-bots создаёт солдат в VU). Объект с vtable 0x02500E38 и строкой имени по +0x08. Скан памяти — один раз
// успешный, повтор не чаще раза в 5 с. Только чтение.
constexpr unsigned long kSoldierBlueprintVtRva = 0x02100E38UL;  // VA 0x02500E38
static unsigned long FindMpSoldierBlueprint() {
    // ★ Сборка 167: как VU — из базы ресурсов движка (всегда объект ТЕКУЩЕГО уровня). Сборка 169: скан памяти удалён.
    return ::evbots::LiveFindAssetByName("Characters/Soldiers/MpSoldier");
}

// ★ Сборка 123: тела ботов до дислокации человека (как в VU: боты бегают, пока человек на экране выбора).
// Человек в списке менеджера появляется уже при подключении (до загрузки карты; прогон 54 — чёрный экран, если
// посадить тела слишком рано). Поэтому ждём ДАННЫЕ, которые клиент шлёт с экрана дислокации: команда 1/2,
// +0x480 и кит (+0x4B8..+0x4BC) у человека, плюс скан точек и ещё EVBOTS_EARLY_DELAY_MS (3000) после этого.
static bool EarlyBodiesReady() {
    if (::evbots::EVBotsEngineSpawnPoint() != 0) return true;
    static int s_on = -1;
    static ULONGLONG s_delay = 0;  // сборка 129: без выдержки (прогон 128: ждали кит 25 с + 3 с)
    if (s_on < 0) {
        char b[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_EARLY_BODIES", b, (DWORD)sizeof(b));
        s_on = (n == 1 && b[0] == '0') ? 0 : 1;
        char d[16] = {};
        const DWORD dn = ::GetEnvironmentVariableA("EVBOTS_EARLY_DELAY_MS", d, (DWORD)sizeof(d));
        if (dn > 0 && dn < sizeof(d)) s_delay = (ULONGLONG)::strtoul(d, nullptr, 10);
    }
    if (s_on == 0) return false;
    static ULONGLONG s_condAt = 0, s_logAt = 0;
    static bool s_done = false;
    const ULONGLONG now = ::GetTickCount64();
    const unsigned long human = ::evbots::EVBotsEngineRealPlayer();
    unsigned int team = 0;
    unsigned long k480 = 0, kb = 0, ke = 0, wb = 0, we = 0, sol = 0;
    if (human != 0) {
        __try {
            const unsigned char* hp = (const unsigned char*)(ULONGLONG)human;
            team = *(const unsigned int*)(hp + kOffPlayerTeam);
            k480 = *(const unsigned long*)(hp + kOffPlayerKit);
            kb = *(const unsigned long*)(hp + kOffKitBegin);
            ke = *(const unsigned long*)(hp + kOffKitEnd);
            wb = *(const unsigned long*)(hp + 0x50C);
            we = *(const unsigned long*)(hp + 0x510);
            sol = *(const unsigned long*)(hp + kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    const unsigned long kitN = (ke >= kb && ke - kb <= 0x40) ? (ke - kb) / 4 : 0;
    const unsigned long wpnN = (we >= wb && we - wb <= 0x24 * 16) ? (we - wb) / 0x24 : 0;
    const bool scan = ::evbots::EVBotsEngineSpawnScanDone();
    if (!scan && human != 0) ::evbots::EVBotsEngineStartSpawnScan();
    // Сборка 124: +0x480 у человека до дислокации пуст — берём блюпринт MpSoldier из памяти.
    unsigned long bp = k480;
    if (bp == 0 && human != 0 && team >= 1 && team <= 4) bp = FindMpSoldierBlueprint();  // сборка 129: кит человека не нужен
    // ★ Сборка 167: команду человека не ждём (движок даёт её в onNewRound через ~20 с после подключения; в VU боты
    // появляются в момент подключения). EVBOTS_EARLY_NOTEAM=0 — ждать команду, как до 167.
    static const bool s_noTeam = [] {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_EARLY_NOTEAM", v, (DWORD)sizeof(v));
        return !(n == 1 && v[0] == '0');
    }();
    if (bp == 0 && human != 0 && s_noTeam) bp = FindMpSoldierBlueprint();
    const bool teamOk = (team >= 1 && team <= 4) || s_noTeam;
    const bool cond = human != 0 && scan && teamOk && bp != 0;  // сборка 129: сразу после выбора команды
    if (!cond) s_condAt = 0;
    else if (s_condAt == 0) s_condAt = now;
    const bool ready = cond && now - s_condAt >= s_delay;
    if (!s_done && (now - s_logAt > 3000 || ready)) {
        s_logAt = now;
        LOG_INFO("Bridge: ДО ДИСЛОКАЦИИ: человек %08lX команда %u 480=%08lX (блюпринт %08lX) кит=%lu оружия=%lu солдат=%08lX скан=%d -> %s",
                 human, team, k480, bp, kitN, wpnN, sol, scan ? 1 : 0, ready ? "САЖАЕМ" : (cond ? "выдержка" : "ждём"));
    }
    if (!ready) return false;
    if (!::evbots::EVBotsEngineAdoptHumanRef(human, bp)) return false;
    if (!s_done) {
        s_done = true;
        LOG_INFO("Bridge: ★ ДО ДИСЛОКАЦИИ: тела ботов сажаем сейчас (человек %08lX ещё без солдата)", human);
    }
    return true;
}

// Повторная обработка очереди моста. ОБЯЗАТЕЛЬНО вне анонимного namespace:
// иначе внутреннее связывание и SpmProbe.cpp не найдёт символ (LNK2019).
// Как только появилась точка спавна (значит на сервер зашёл живой игрок),
// создаём нативных игроков, которые ждали. Так ведут себя боты и в VU —
// до прихода игрока их в матче нет.
// Сборка 131: счётчики перехвата handleCollision (детур ниже, лог — в AmmoTick).
static volatile LONG g_hcCalls = 0, g_hcBot = 0, g_hcFlip = 0;

// ★ Сборка 129: патроны ботов (разбор агента, re\aim\report.md). Путь: солдат = [игрок+0x3C4]; компонент оружия =
// [солдат+0x53C]; слот = [[комп+0x68]+4]; оружие = [[комп+0x30]+слот*4]; WF = [оружие+0x128]; магазин int [WF+0xF4],
// запас [WF+0xF8], у пустых [WF+0xD0] = 0x80000000 (перезарядка не завершается: без клиента её не двигает анимация).
// Ёмкость — как геттер 0x0089F1E0: [WF+0x114], если != -1, иначе [[[WF+0x12C]+0xD0]+8].
// EVBOTS_AMMO_FIX=0 — только диагностика (по умолчанию магазин пополняется из запаса после 4 с пустоты).
// EVBOTS_POSEUPDATE=0 — не включать cvar ant.DetailedCollision.forceServerPoseUpdate ([[0x2B47088]], как botlayer).
static bool EnvOn(const char* name, bool def) {
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') return false;
    if (n == 1 && v[0] == '1') return true;
    return def;
}
struct CapCache { unsigned long data; int maxMag; };  // сборка 136: ёмкость магазина по типу оружия
static CapCache g_capCache[64];
static LONG g_capCacheCount = 0;
struct AmmoSeen { unsigned long wf; int emptyTicks; int maxMag; };  // maxMag: прогон 129 — геттер ёмкости дал -1, берём максимум, увиденный у оружия
static AmmoSeen g_ammoSeen[64];
static void AmmoTick() {
    static ULONGLONG s_at = 0, s_logAt = 0;
    static bool s_fix = EnvOn("EVBOTS_AMMO_FIX", true);
    static long s_refills = 0, s_failCap = 0;
    static volatile LONG s_pose = 0;
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return;
    if (InterlockedCompareExchange(&s_pose, 1, 0) == 0) {
        if (EnvOn("EVBOTS_POSEUPDATE", true)) {
            __try {
                unsigned char* cv = *(unsigned char**)(base + (0x02B47088UL - 0x00400000UL));
                if (cv != nullptr) {
                    const unsigned char before = *cv;
                    *cv = 1;
                    LOG_INFO("Bridge: ★ forceServerPoseUpdate %u -> 1 (поза хитбоксов на сервере, как botlayer)", (unsigned)before);
                } else {
                    LOG_INFO("Bridge: forceServerPoseUpdate: cvar ещё не создан");
                    InterlockedExchange(&s_pose, 0);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LOG_ERR("Bridge: forceServerPoseUpdate: исключение");
            }
        }
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now - s_at < 2000) return;
    s_at = now;
    int total = 0, empty = 0, d0set = 0, hurt = 0;
    char sample[160] = {};
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
        const unsigned long pl = g_watchBot[i];
        unsigned long wf = 0;
        int mag = 0, res = 0, cap = -1;
        unsigned long d0 = 0;
        unsigned long wdata = 0;
        __try {
            const unsigned char* sol = *(const unsigned char* const*)((const unsigned char*)(ULONG_PTR)pl + kOffPlayerSoldier);
            if (sol == nullptr) { g_ammoSeen[i].emptyTicks = 0; continue; }
            {
                const float hp = *(const float*)(sol + 0x34);
                if (hp > 0.0f && hp < 99.9f) ++hurt;
            }
            const unsigned char* comp = *(const unsigned char* const*)(sol + 0x53C);
            if (comp == nullptr) continue;
            const unsigned long slot = *(const unsigned long*)(*(const unsigned char* const*)(comp + 0x68) + 4);
            const unsigned char* arr = *(const unsigned char* const*)(comp + 0x30);
            if (arr == nullptr || slot > 15) continue;
            const unsigned char* w = *(const unsigned char* const*)(arr + slot * 4);
            if (w == nullptr) continue;
            wf = *(const unsigned long*)(w + 0x128);
            if (wf == 0) continue;
            const unsigned char* f = (const unsigned char*)(ULONG_PTR)wf;
            mag = *(const int*)(f + 0xF4);
            res = *(const int*)(f + 0xF8);
            d0 = *(const unsigned long*)(f + 0xD0);
            cap = *(const int*)(f + 0x114);
            wdata = *(const unsigned long*)(f + 0x12C);
            if (cap == -1) {
                const unsigned char* d = *(const unsigned char* const*)(f + 0x12C);
                const unsigned char* dd = d ? *(const unsigned char* const*)(d + 0xD0) : nullptr;
                cap = dd ? *(const int*)(dd + 8) : -1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        {
            const int c2 = WeaponMagCapacity(wf);  // сборка 185: полностью как движок (с запасным [[wf+0x98]])
            if (c2 >= 1 && c2 <= 500) cap = c2;
        }
        ++total;
        if (d0 != 0) ++d0set;
        if (g_ammoSeen[i].wf != wf) { g_ammoSeen[i].wf = wf; g_ammoSeen[i].emptyTicks = 0; g_ammoSeen[i].maxMag = 0; }
        if (mag > g_ammoSeen[i].maxMag) g_ammoSeen[i].maxMag = mag;
        // ★ Сборка 177: ёмкость движка (данные оружия) — главная; кэш «максимум виденного» её не превышает. Прогон 174:
        // assert WeaponFiring.cpp(1207) «Magazine with capacity 7 contains 8» — кэш помнил 8 (7 + патрон в патроннике).
        const bool engineCap = cap >= 1 && cap <= 500;
        if (!engineCap) cap = g_ammoSeen[i].maxMag;
        // Сборка 136: ёмкость по типу оружия — максимум, виденный у любого бота с тем же [WF+0x12C]; иначе 30.
        // Раньше бот, посаженный с пустым магазином, оставался «без ёмкости» навсегда и не стрелял (прогон 135: 81).
        {
            LONG k = 0;
            for (; k < g_capCacheCount; ++k) if (g_capCache[k].data == wdata) break;
            if (k == g_capCacheCount && g_capCacheCount < 64 && wdata != 0) {
                g_capCache[k].data = wdata; g_capCache[k].maxMag = 0; ++g_capCacheCount;
            }
            if (k < g_capCacheCount) {
                if (mag > g_capCache[k].maxMag && mag <= 500) g_capCache[k].maxMag = mag;
                if (!engineCap && (cap < 1 || cap > 500)) cap = g_capCache[k].maxMag;
            }
            if (cap < 1 || cap > 500) cap = 30;
        }
        // ★ Сборка 207: диагностика ассерта движка «Magazine with capacity 7 contains 8» (WeaponFiring.cpp:1207; 20.10 — бот с
        // 870, мост в эти секунды патроны не писал). Пишем первые случаи, когда магазин больше ёмкости движка: кто и в каком
        // состоянии перезарядки ([WF+0xD0]). Ничего не меняем — сначала причина.
        if (engineCap && mag > cap) {
            static volatile LONG s_over = 0;
            if (InterlockedIncrement(&s_over) <= 8)
                LOG_WARN("Bridge: ДИАГ магазин больше ёмкости: игрок %08lX WF %08lX данные %08lX маг %d ёмк %d запас %d D0 %08lX",
                         pl, wf, wdata, mag, cap, res, d0);
        }
        // Сборка 177: при isAI 0 бот перезаряжается сам, вводом, как человек (у VU пополнения нет) — мост ждёт дольше
        // (4 тика по 2 с), чтобы не перебивать обычную перезарядку; при isAI 1 — как раньше (2 тика).
        const int waitTicks = (BotAiEnv() == 0) ? 4 : 2;
        if (mag <= 0 && res > 0) {
            ++empty;
            if (sample[0] == 0) ::_snprintf(sample, sizeof(sample) - 1, "%08lX маг %d запас %d D0 %08lX ёмк %d", pl, mag, res, d0, cap);
            if (++g_ammoSeen[i].emptyTicks >= waitTicks && s_fix) {
                if (cap < 1 || cap > 500) { ++s_failCap; continue; }
                const int put = cap < res ? cap : res;
                __try {
                    unsigned char* f = (unsigned char*)(ULONG_PTR)wf;
                    *(int*)(f + 0xF4) = put;
                    *(int*)(f + 0xF8) = res - put;
                    *(unsigned long*)(f + 0xD0) = 0;
                    ++s_refills;
                    g_ammoSeen[i].emptyTicks = 0;
                } __except (EXCEPTION_EXECUTE_HANDLER) { }
            }
        } else {
            g_ammoSeen[i].emptyTicks = 0;
        }
    }
    if (now - s_logAt >= 20000) {
        s_logAt = now;
        LOG_INFO("ДИАГ патроны: оружий %d, пустой магазин при запасе %d, флаг D0 у %d; пополнено мостом %ld (без ёмкости %ld)%s%s",
                 total, empty, d0set, s_refills, s_failCap, sample[0] ? "; пример " : "", sample);
        LOG_INFO("ДИАГ попадания: handleCollision %ld, пуль ботов %ld, окон арбитража %ld; раненых ботов сейчас %d",
                 (long)InterlockedExchange(&g_hcCalls, 0), (long)InterlockedExchange(&g_hcBot, 0),
                 (long)InterlockedExchange(&g_hcFlip, 0), hurt);
    }
}

// ★ Сборка 131: УРОН ОТ ПУЛЬ БОТОВ (рецепт рабочего botlayer, разбор агента 14.10 — MODLOG «Сборка 131»).
// Сервер по умолчанию доверяет урон клиенту стрелка (SyncedGameSettings.AllowClientSideDamageArbitration =
// byte [[0x02B2AD70]+0x1E] = 1); у бота клиента нет — его попадания отбрасываются. botlayer на время вызова
// handleCollision 0x00E72040 (thiscall: this = пуля, 4 аргумента, ret 0x10) для пули своего бота ставил байт в 0 и сразу
// возвращал. Стрелок пули: tok = [пуля+0x84], игрок = [tok] − 8. Вне окна байт не трогаем (его читают 0x01183061 и
// 0x0132E560). Пролог сверяется перед установкой (правило 18). EVBOTS_BOTDMG=0 — без перехвата.
// ★ Сборка 153: события урона для хука VU «Soldier:Damage» (мод: ответный огонь Config.ShootBackIfHit, множители урона
// ботов). Урон движок наносит ВНУТРИ двух вызовов, которые мы уже перехватываем: handleCollision пули (стрелок —
// [[пуля+0x84]]−8) и ClientDamageListener::onMessage 0x01183000 (урон от клиента человека). После оригинала сверяем HP
// ботов и человека: у кого упало — жертва, стрелок известен. Остальные падения ловит HitTick (стрелок неизвестен).
// Lua забирает события (NativeBridge.PollDamage) и вызывает обработчики хука (Hooks._Fire). Только чтение памяти.
struct DmgEvt { unsigned long victim; unsigned long giver; float dmg; float hpAfter; };
static SRWLOCK g_dmgLock = SRWLOCK_INIT;
static DmgEvt g_dmgEvt[128];
static LONG g_dmgEvtN = 0, g_dmgDropped = 0, g_dmgWithGiver = 0, g_dmgNoGiver = 0;
static unsigned long g_humanHpPl = 0;
static float g_humanHp = -1.0f;

static float ReadPlayerHp(unsigned long pl) {
    float hp = -1.0f;
    if (pl == 0) return hp;
    __try {
        const unsigned char* sol = *(const unsigned char* const*)((const unsigned char*)(ULONG_PTR)pl + kOffPlayerSoldier);
        if (sol != nullptr) hp = *(const float*)(sol + 0x34);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hp = -1.0f;
    }
    return hp;
}

static void PushDmg(unsigned long victim, unsigned long giver, float dmg, float hpAfter) {
    AcquireSRWLockExclusive(&g_dmgLock);
    if (g_dmgEvtN < 128) {
        g_dmgEvt[g_dmgEvtN].victim = victim;
        g_dmgEvt[g_dmgEvtN].giver = giver;
        g_dmgEvt[g_dmgEvtN].dmg = dmg;
        g_dmgEvt[g_dmgEvtN].hpAfter = hpAfter;
        ++g_dmgEvtN;
    } else {
        ++g_dmgDropped;
    }
    ReleaseSRWLockExclusive(&g_dmgLock);
    if (giver != 0) ++g_dmgWithGiver; else ++g_dmgNoGiver;
}

/// Сверка HP после вызова, в котором движок мог нанести урон (giver — стрелок, 0 — неизвестен). Обновляет g_hpSeen,
/// чтобы HitTick не посчитал то же падение второй раз.
static void ScanHpDrops(unsigned long giver) {
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
        const unsigned long pl = g_watchBot[i];
        HpSeen& s = g_hpSeen[i];
        if (s.pl != pl) continue;
        const float hp = ReadPlayerHp(pl);
        if (hp >= 0.0f && s.hp > 0.0f && hp < s.hp - 0.01f) {
            const float d = s.hp - (hp > 0.0f ? hp : 0.0f);
            PushDmg(pl, giver, d, hp);
            ++g_hitEvents;
            g_hitDmg += d;
            if (hp <= 0.0f) ++g_hitKills;
            s.hp = hp;
        }
    }
    const unsigned long human = ::evbots::EVBotsEngineRealPlayer();
    if (human != 0) {
        const float hp = ReadPlayerHp(human);
        if (g_humanHpPl == human && hp >= 0.0f && g_humanHp > 0.0f && hp < g_humanHp - 0.01f) {
            PushDmg(human, giver, g_humanHp - (hp > 0.0f ? hp : 0.0f), hp);
        }
        g_humanHpPl = human;
        g_humanHp = hp;
    }
}

/// Тик: HP человека (без событий — только чтобы сверка после урона шла от свежего значения).
static void HumanHpTick() {
    const unsigned long human = ::evbots::EVBotsEngineRealPlayer();
    const float hp = ReadPlayerHp(human);
    if (human != 0 && g_humanHpPl == human && hp >= 0.0f && g_humanHp > 0.0f && hp < g_humanHp - 0.01f) {
        PushDmg(human, 0, g_humanHp - (hp > 0.0f ? hp : 0.0f), hp);  // урон вне наших перехватов (взрыв, падение)
    }
    g_humanHpPl = human;
    g_humanHp = hp;
}

// ★ Сборка 159: ВХОДЯЩИЙ ЧАТ → Player:Chat (EVBOTS_CHAT_EVENTS=1). Детур handleMessage (DetourChatMessage, EngineHooks.cpp)
// читает M = arg1: текст UTF-16 от [M+0] & ~3 до [M+4], маска [M+0x110]/[M+0x114], тип канала [M+0x118]; S = arg2 —
// отправитель (0 у admin.say). Разбор — re\stubs-155\chat.md, разделы 2.1–2.4. Здесь только запись в очередь;
// Lua забирает её через NativeBridge.PollChat(). Наша собственная отправка (ChatSendDrain) событие не порождает.
struct ChatEvt { unsigned long sender; long lo; long hi; unsigned int type; char text[400]; };
static SRWLOCK g_chatLock = SRWLOCK_INIT;
static ChatEvt g_chatEvt[32];
static LONG g_chatEvtN = 0, g_chatDropped = 0;
static volatile LONG g_chatSending = 0;  // 1 — идёт наша отправка (ChatSendDrain)

static bool ChatEventsOn() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_CHAT_EVENTS", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;  // сборка 186: по умолчанию ВКЛ (команды чата мода), выключатель =0
    }
    return s_on != 0;
}

// UTF-16 → UTF-8 своей функцией (штатный 0x49C460 отбрасывает всё, что старше 0x7F). Суррогатные пары — 4 байта.
static int Utf16ToUtf8(const wchar_t* src, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n; ++i) {
        unsigned long cp = (unsigned long)src[i];
        if (cp >= 0xD800UL && cp <= 0xDBFFUL && i + 1 < n && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
            cp = 0x10000UL + ((cp - 0xD800UL) << 10) + ((unsigned long)src[i + 1] - 0xDC00UL);
            ++i;
        }
        const int need = cp < 0x80UL ? 1 : cp < 0x800UL ? 2 : cp < 0x10000UL ? 3 : 4;
        if (o + need > cap - 1) break;
        if (need == 1) {
            out[o++] = (char)cp;
        } else if (need == 2) {
            out[o++] = (char)(0xC0UL | (cp >> 6));
            out[o++] = (char)(0x80UL | (cp & 0x3FUL));
        } else if (need == 3) {
            out[o++] = (char)(0xE0UL | (cp >> 12));
            out[o++] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
            out[o++] = (char)(0x80UL | (cp & 0x3FUL));
        } else {
            out[o++] = (char)(0xF0UL | (cp >> 18));
            out[o++] = (char)(0x80UL | ((cp >> 12) & 0x3FUL));
            out[o++] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
            out[o++] = (char)(0x80UL | (cp & 0x3FUL));
        }
    }
    out[o] = '\0';
    return o;
}

// Вызывается детуром handleMessage ДО оригинала: msg — M, sender — S. Только чтение памяти; сбой чтения — запись не кладём.
void BridgeOnChatMessage(void* msg, void* sender) {
    if (msg == nullptr || !ChatEventsOn()) return;
    if (InterlockedCompareExchange(&g_chatSending, 0, 0) != 0) return;
    ChatEvt e;
    ::memset(&e, 0, sizeof(e));
    e.sender = (unsigned long)(ULONG_PTR)sender;
    wchar_t buf[128] = {};
    int n = 0;
    __try {
        const unsigned char* m = (const unsigned char*)msg;
        const unsigned long begin = *(const unsigned long*)m & ~3UL;
        const unsigned long end = *(const unsigned long*)(m + 4);
        if (begin != 0 && end > begin) {
            n = (int)((end - begin) / sizeof(wchar_t));
            if (n > 127) n = 127;
            ::memcpy(buf, (const void*)(ULONG_PTR)begin, (size_t)n * sizeof(wchar_t));
        }
        e.lo = *(const long*)(m + 0x110);
        e.hi = *(const long*)(m + 0x114);
        e.type = *(const unsigned int*)(m + 0x118);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    Utf16ToUtf8(buf, n, e.text, (int)sizeof(e.text));
    AcquireSRWLockExclusive(&g_chatLock);
    if (g_chatEvtN < 32) {
        g_chatEvt[g_chatEvtN++] = e;
    } else {
        ++g_chatDropped;
    }
    ReleaseSRWLockExclusive(&g_chatLock);
}

/// NativeBridge.PollChat() -> { {sender, lo, hi, type, text}, ... } — входящие сообщения с прошлого опроса.
int l_bridge_poll_chat(lua_State* L) {
    ChatEvt copy[32];
    LONG n = 0;
    AcquireSRWLockExclusive(&g_chatLock);
    n = g_chatEvtN;
    if (n > 0) ::memcpy(copy, g_chatEvt, (size_t)n * sizeof(ChatEvt));
    g_chatEvtN = 0;
    ReleaseSRWLockExclusive(&g_chatLock);
    lua_createtable(L, (int)n, 0);
    for (LONG i = 0; i < n; ++i) {
        lua_createtable(L, 5, 0);
        lua_pushinteger(L, (lua_Integer)copy[i].sender); lua_rawseti(L, -2, 1);
        lua_pushinteger(L, (lua_Integer)copy[i].lo); lua_rawseti(L, -2, 2);
        lua_pushinteger(L, (lua_Integer)copy[i].hi); lua_rawseti(L, -2, 3);
        lua_pushinteger(L, (lua_Integer)copy[i].type); lua_rawseti(L, -2, 4);
        lua_pushstring(L, copy[i].text); lua_rawseti(L, -2, 5);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    return 1;
}

/// NativeBridge.PollDamage() -> { {victim, giver, dmg, hp}, ... } — события урона с прошлого опроса.
int l_bridge_poll_damage(lua_State* L) {
    DmgEvt copy[128];
    LONG n = 0;
    AcquireSRWLockExclusive(&g_dmgLock);
    n = g_dmgEvtN;
    if (n > 0) ::memcpy(copy, g_dmgEvt, (size_t)n * sizeof(DmgEvt));
    g_dmgEvtN = 0;
    ReleaseSRWLockExclusive(&g_dmgLock);
    lua_createtable(L, (int)n, 0);
    for (LONG i = 0; i < n; ++i) {
        lua_createtable(L, 4, 0);
        lua_pushinteger(L, (lua_Integer)copy[i].victim); lua_rawseti(L, -2, 1);
        lua_pushinteger(L, (lua_Integer)copy[i].giver); lua_rawseti(L, -2, 2);
        lua_pushnumber(L, (lua_Number)copy[i].dmg); lua_rawseti(L, -2, 3);
        lua_pushnumber(L, (lua_Number)copy[i].hpAfter); lua_rawseti(L, -2, 4);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    return 1;
}

/// NativeBridge.DamageStats() -> со стрелком, без стрелка, потеряно (переполнение очереди)
int l_bridge_damage_stats(lua_State* L) {
    lua_pushinteger(L, g_dmgWithGiver);
    lua_pushinteger(L, g_dmgNoGiver);
    lua_pushinteger(L, g_dmgDropped);
    return 3;
}

// ★ Сборка 185: имя уровня и режим — из движка, как SharedUtils:GetLevelName/GetCurrentGameMode и Level:Loaded у VU.
// Прогоны 17.10 (184): движок грузил MP_012 TeamDeathMatch0 / MP_003 (MapList.txt от лаунчера), а мост всегда слал
// моду «MP_001 ConquestSmall0» — пути и точки Гранд-Базара на чужой карте: тела гибли за 10 с и не возрождались.
// Разбор exe (18.10, capstone): fb::Main — статический указатель 0x02877210 (конструктор 0x00423570:
// `mov [0x2877210], esi`); fb::Server = [Main+0xD0] (0x0040B080: `mov [esi+0x40], eax`, esi = Main+0x90);
// ServerLevel = [Server+0x7C] (0x00D6714E); LevelSetup = ServerLevel+0x6C (ServerLevel::load 0x00D654F4 копирует его):
// +0 имя (fb::String = char*), +4 массив опций {ключ, значение} (пары char*, число — [массив−4]; «GameMode=…» печатает
// 0x00D6561B). Только чтение, под SEH.
static bool EngineLevelInfoRaw(char* name, size_t nameCap, char* mode, size_t modeCap) {
    name[0] = '\0';
    mode[0] = '\0';
    if (NativeModuleBase() == nullptr) return false;
    __try {
        const unsigned long mainp = *(const unsigned long*)(ULONGLONG)0x02877210UL;
        if (mainp == 0) return false;
        const unsigned long server = *(const unsigned long*)(ULONGLONG)(mainp + 0xD0);
        if (server == 0) return false;
        // Живое чтение 18.10 (прогон 185, MP_012): после загрузки уровень лежит в Server+0x74; +0x7C — место на время
        // serverLoadLevel (0x00D6714E), потом 0. Берём +0x74, иначе +0x7C.
        unsigned long level = *(const unsigned long*)(ULONGLONG)(server + 0x74);
        if (level == 0) level = *(const unsigned long*)(ULONGLONG)(server + 0x7C);
        if (level == 0) return false;
        const char* s = *(const char* const*)(ULONGLONG)(level + 0x6C);
        if (s == nullptr) return false;
        size_t i = 0;
        while (i + 1 < nameCap && s[i] != '\0') { name[i] = s[i]; ++i; }
        name[i] = '\0';
        if (std::strncmp(name, "Levels/", 7) != 0) { name[0] = '\0'; return false; }
        const unsigned long opts = *(const unsigned long*)(ULONGLONG)(level + 0x70);
        if (opts != 0) {
            const unsigned long n = *(const unsigned long*)(ULONGLONG)(opts - 4);
            for (unsigned long k = 0; k < n && k < 64; ++k) {
                const char* key = *(const char* const*)(ULONGLONG)(opts + k * 8);
                const char* val = *(const char* const*)(ULONGLONG)(opts + k * 8 + 4);
                if (key == nullptr || val == nullptr || std::strcmp(key, "GameMode") != 0) continue;
                size_t j = 0;
                while (j + 1 < modeCap && val[j] != '\0') { mode[j] = val[j]; ++j; }
                mode[j] = '\0';
                break;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        name[0] = '\0';
        mode[0] = '\0';
        return false;
    }
}

// ★ Сборка 185: сущности уровня в движке (точки спавна + флаги). Ночной прогон MP_007 (12:04): Level:Loaded ушёл, когда имя
// уровня уже читалось, но сущностей ещё не было — мод при FinishedLoading (GameDirector:_InitFlagTeams) получил 0 флагов,
// цель авиации стала (0, 0, 0) + 110 м и вертолёты не взлетали; без флагов GameDirector не знает их команды и для пехоты.
int EVBotsEngineLevelEntityCount() {
    unsigned long buf[256];
    // Сборка 186: и цели/счётчики жизней — в Rush флагов нет, точки спавна другого класса (прогон 19:23: «сущности (0)»,
    // Level:Loaded ушёл по таймауту через 90 с, боты появились только к дислокации человека).
    return EVBotsListEntitiesByName("ServerCharacterSpawnEntity", buf, 256) +
           EVBotsListEntitiesByName("ServerCapturePointEntity", buf, 256) +
           EVBotsListEntitiesByName("ServerObjectiveEntity", buf, 256) +
           EVBotsListEntitiesByName("ServerLifeCounterEntity", buf, 256);
}

bool EngineLevelReady() {
    char n[128];
    char m[64];
    return EngineLevelInfoRaw(n, sizeof(n), m, sizeof(m)) && HasCompleteLevelInfo(n, m);
}

void EngineLevelNameAndMode(char* name, size_t nameCap, char* mode, size_t modeCap) {
    static volatile LONG s_logged = 0;
    char eName[128];
    char eMode[64];
    const bool ok = EngineLevelInfoRaw(eName, sizeof(eName), eMode, sizeof(eMode));
    // Флаги — только явная подмена (опыт); по умолчанию — движок, как VU.
    const char* envLvl = std::getenv("EVBOTS_LEVEL_NAME");
    const char* envMode = std::getenv("EVBOTS_GAMEMODE");
    const char* n = envLvl != nullptr && envLvl[0] != '\0' ? envLvl
                    : ok                                    ? eName
                                                            : "Levels/MP_001/MP_001";
    const char* m = envMode != nullptr && envMode[0] != '\0' ? envMode
                    : ok && eMode[0] != '\0'                  ? eMode
                                                              : "ConquestSmall0";
    std::snprintf(name, nameCap, "%s", n);
    std::snprintf(mode, modeCap, "%s", m);
    if (::InterlockedIncrement(&s_logged) <= 8) {
        LOG_INFO("EVBots: уровень для модов: %s %s (движок: %s%s%s)", name, mode, ok ? eName : "не прочитан",
                 ok ? " " : "", ok ? eMode : "");
    }
}

/// ★ Сборка 185: NativeBridge.LevelInfo() -> имя уровня, режим (из движка; nil — не прочитан). Для SharedUtils.
int l_bridge_level_info(lua_State* L) {
    char name[128];
    char mode[64];
    if (!EngineLevelInfoRaw(name, sizeof(name), mode, sizeof(mode))) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    lua_pushstring(L, name);
    if (mode[0] != '\0') lua_pushstring(L, mode); else lua_pushnil(L);
    return 2;
}

/// ★ Сборка 167: NativeBridge.LevelReload() -> имя уровня, режим. Зовёт Lua моста при перезагрузке уровня (новый
/// раунд, тикеты снова > 0): сброс кэшей ассетов и эталонов прошлого уровня, догоняющий вызов load-хендлеров.
/// Прогон 166: этого не было вовсе (onNewRound движка до мода не доходил) — ботам шли объекты прошлого уровня.
/// Сборка 185: имя и режим — из движка (EngineLevelNameAndMode), как VU.
int l_bridge_level_reset(lua_State* L) {
    (void)L;
    LOG_INFO("Bridge: смена уровня: отменены старые заявки спавна %ld, позиций %ld, техники %ld",
             (long)g_respawnCount, (long)g_posPendingCount, (long)g_vehCount);
    g_respawnCount = 0;
    for (int i = 0; i < kRespawnCapacity; ++i) g_respawnSince[i] = g_respawnTry[i] = 0;
    g_botPosCount = g_posPendingCount = g_spawnCheckCount = 0;
    g_forcedPointCount = g_spawnEvFiredCount = 0;
    g_vehCount = 0;
    g_placedN = 0;
    g_capCacheCount = 0;
    for (int i = 0; i < 5; ++i) InterlockedExchange(&g_lastRespawnPoint[i], 0);
    return 0;
}

int l_bridge_level_reload(lua_State* L) {
    ::evbots::ResetAssetCaches();
    EVBotsMarkWeaponTemplateStale();  // сборка 186; 252 — и шаблон набора
    g_kitScanned = false;  // сборка 252: киты нового уровня (XP4 — *_XP4) ищем заново, старые адреса не держим
    for (int f = 0; f < 2; ++f) {
        for (int c = 0; c < 4; ++c) g_kitAsset[f][c] = 0;
    }
    ::evbots::EVBotsEngineResetSpawnRef();
    ::evbots::EVBotsEngineResetSpawnPoints();  // сборка 173
    for (int i = 0; i < 5; ++i) InterlockedExchange(&g_lastRespawnPoint[i], 0);
    ResourceLoadHandlersNewRound();
    char name[128];
    char mode[64];
    EngineLevelNameAndMode(name, sizeof(name), mode, sizeof(mode));
    lua_pushstring(L, name);
    lua_pushstring(L, mode);
    return 2;
}

// ★ Сборка 186: конец раунда и загрузка уровня — из движка, как VU (Server:RoundOver на любом режиме). Раньше — только по
// тикетам: на TDM счётчика нет («TicketManager: счётчик тикетов … не найден»), RoundOver не уходил, боты играли поверх
// таблицы счёта, а Level:Destroy/Level:Loaded нового раунда мод не получал. Разбор exe (18.10, capstone):
// RconServer::onMessage(msg) = 0x01152A00 (thiscall, ret 4; слот vtable 0x02649BF0) — [msg+4] категория, [msg+8] тип:
// 0x16BABB47 ServerRoundOverMessage (→ 0x0114B810 «server.onRoundOver», победитель [msg+0x28]),
// 0x3845EA0B ServerMapSequencerLevelLoadedMessage («server.onLevelLoaded»). Перехват — заменой слота vtable после сверки.
static volatile LONG g_rconInstalled = 0;  // 0 — нет, 1 — стоит, -1 — отказ
static volatile LONG g_rconOrig = 0;
static volatile LONG g_rconCalls = 0;
static volatile LONG g_roundOverSeq = 0, g_roundOverWinner = 0, g_levelLoadedSeq = 0;
using RconOnMessageFn = void(__fastcall*)(void*, void*, void*);
static void __fastcall RconOnMessageHook(void* self, void* edx, void* msg) {
    InterlockedIncrement(&g_rconCalls);
    EVBotsRconNoteServer(self);  // ★ 239 (А2): RconServer для RCON:SendCommand в процессе
    __try {
        if (msg != nullptr) {
            const unsigned long type = *(const unsigned long*)((const unsigned char*)msg + 8);
            if (type == 0x16BABB47UL) {
                InterlockedExchange(&g_roundOverWinner, (LONG)*(const unsigned long*)((const unsigned char*)msg + 0x28));
                InterlockedIncrement(&g_roundOverSeq);
            } else if (type == 0x3845EA0BUL) {
                InterlockedIncrement(&g_levelLoadedSeq);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ((RconOnMessageFn)(ULONGLONG)(unsigned long)g_rconOrig)(self, edx, msg);
}
static void InstallRconHookOnce() {
    if (InterlockedCompareExchange(&g_rconInstalled, 0, 0) != 0) return;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_ROUND_HOOK", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') {
        InterlockedExchange(&g_rconInstalled, -1);
        LOG_INFO("Bridge: RCON onMessage — перехват выключен (EVBOTS_ROUND_HOOK=0), конец раунда — по тикетам");
        return;
    }
    if (NativeModuleBase() == nullptr) return;
    volatile unsigned long* slot = (volatile unsigned long*)(ULONGLONG)0x02649BF0UL;
    const unsigned long expect = 0x01152A00UL;
    // пролог onMessage: mov eax, 0x6ACC; call __chkstk — сверено дизассемблером
    static const unsigned char kPro[5] = {0xB8, 0xCC, 0x6A, 0x00, 0x00};
    unsigned long cur = 0;
    bool pro = false;
    __try {
        cur = *slot;
        pro = ::memcmp((const void*)(ULONGLONG)expect, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cur = 0;
    }
    if (cur != expect || !pro) {
        InterlockedExchange(&g_rconInstalled, -1);
        LOG_ERR("Bridge: RCON onMessage: в слоте %08lX, ждали %08lX (пролог %s) — перехват НЕ ставлю", cur, expect,
                pro ? "совпал" : "не совпал");
        return;
    }
    InterlockedExchange(&g_rconOrig, (LONG)cur);
    // сборка 237: замена слота — ядром (ev-core), со сверкой прежнего значения
    if (!PatchPointer((void*)slot, (void*)&RconOnMessageHook, (void*)(ULONGLONG)cur, nullptr, "RCON onMessage")) {
        InterlockedExchange(&g_rconInstalled, -1);
        LOG_ERR("Bridge: RCON onMessage: ядро не заменило слот");
        return;
    }
    InterlockedExchange(&g_rconInstalled, 1);
    LOG_INFO("Bridge: ★ RCON onMessage перехвачен (слот 02649BF0: %08lX -> %p) — Server:RoundOver из движка, как VU", cur,
             (void*)&RconOnMessageHook);
}
// Сборка 186 (прогон 20:01): Level:Loaded при старте — по сигналу движка server.onLevelLoaded, а не по таймеру. Перехват —
// только запись слота vtable (движок не вызываем), поэтому его можно ставить из потока ожидания до загрузки уровня.
void EVBotsRconHookEarly() { InstallRconHookOnce(); }
long EVBotsLevelLoadedSeq() { return InterlockedCompareExchange(&g_levelLoadedSeq, 0, 0); }
// ★ Сборка 211: номер server.onLevelLoaded, по которому ушёл стартовый Level:Loaded (-1 — ушёл без сигнала движка).
// Всё, что придёт после него, — загрузка НОВОГО уровня, даже без конца раунда (mapList.runNextRound, restartRound).
static volatile LONG g_startupLoadedSeq = -1;
void EVBotsSetStartupLevelSeq(long seq) { InterlockedExchange(&g_startupLoadedSeq, (LONG)seq); }
bool EVBotsRconHookOn() { return InterlockedCompareExchange(&g_rconInstalled, 0, 0) == 1; }

// ★ Сборка 186: ScoringSystem:StatEvent(player, objectPlayer, statEvent, paramX, paramY, value), как VU. Все стат-события
// сервера собираются конструктором сообщения 0x011749C0 (thiscall(msg, игрок, событие, paramX, paramY, float value),
// ret 0x14; поля msg: +0x28 игрок, +0x30 событие, +0x34/+0x38 параметры, +0x3C значение; разбор 18.10, 21 вызов).
// Пролог `push esi; mov esi, ecx; call 0x023B7290` — относительный call, общий InlineHook его не переносит, поэтому
// трамплин собран здесь: те же 3 инструкции с пересчитанным call и jmp назад на 0x011749C8.
struct StatEv {
    unsigned long player;
    int ev;
    int px;
    int py;
    float value;
};
static StatEv g_statRing[1024] = {};
static volatile LONG g_statWrite = 0;
static LONG g_statRead = 0;
static void* g_statTramp = nullptr;
static volatile LONG g_statInstalled = 0;
using StatCtorFn = void*(__fastcall*)(void*, void*, unsigned long, int, int, int, float);
static void* __fastcall StatCtorDetour(void* msg, void* edx, unsigned long player, int ev, int px, int py, float value) {
    void* r = ((StatCtorFn)g_statTramp)(msg, edx, player, ev, px, py, value);
    const LONG w = InterlockedIncrement(&g_statWrite) - 1;
    StatEv& e = g_statRing[w & 1023];
    e.player = player;
    e.ev = ev;
    e.px = px;
    e.py = py;
    e.value = value;
    return r;
}
static void InstallStatEventHookOnce() {
    if (InterlockedCompareExchange(&g_statInstalled, 0, 0) != 0) return;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_STAT_EVENTS", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') { InterlockedExchange(&g_statInstalled, -1); return; }
    if (NativeModuleBase() == nullptr) return;
    unsigned char* t = (unsigned char*)(ULONGLONG)0x011749C0UL;
    static const unsigned char kPro[8] = {0x56, 0x8B, 0xF1, 0xE8, 0xC8, 0x28, 0x24, 0x01};
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        InterlockedExchange(&g_statInstalled, -1);
        LOG_ERR("Bridge: StatEvent 0x011749C0 — пролог не совпал, перехват НЕ ставлю");
        return;
    }
    unsigned char* tr = (unsigned char*)::VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (tr == nullptr) { InterlockedExchange(&g_statInstalled, -1); return; }
    const unsigned long callee = 0x011749C8UL + 0x012428C8UL;  // цель call из пролога = 0x023B7290
    tr[0] = 0x56; tr[1] = 0x8B; tr[2] = 0xF1;
    tr[3] = 0xE8;
    const LONG relCall = (LONG)(callee - ((unsigned long)(ULONG_PTR)tr + 8));
    ::memcpy(tr + 4, &relCall, 4);
    tr[8] = 0xE9;
    const LONG relBack = (LONG)(0x011749C8UL - ((unsigned long)(ULONG_PTR)tr + 13));
    ::memcpy(tr + 9, &relBack, 4);
    g_statTramp = tr;
    unsigned char patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const LONG relDet = (LONG)((unsigned long)(ULONG_PTR)&StatCtorDetour - (0x011749C0UL + 5));
    ::memcpy(patch + 1, &relDet, 4);
    // сборка 237: запись 8 байт — ядром (ev-core), атомарно
    if (!PatchBytes(t, nullptr, patch, 8, "StatEvent (ScoringSystem)")) { InterlockedExchange(&g_statInstalled, -1); return; }
    InterlockedExchange(&g_statInstalled, 1);
    LOG_INFO("Bridge: ★ StatEvent перехвачен (0x011749C0, трамплин %p) — ScoringSystem:StatEvent, как VU", tr);
}

// ★ Сборка 186: второй источник стат-событий — триггер карты ServerStatEventTriggerEntity::sendEvent (0x012BC7C0,
// thiscall(this, ServerPlayer*), ret 4; событие — [[this+0x14]+0x60]). Через него, по разбору субагента, идут события
// уровня (МКОМ «заряжен/разряжен» в Rush — StatEvent_CrateArmed/Disarmed у VU), которых нет среди 21 вызова 0x011749C0.
static InlineHook g_hookStatTrigger;
static void* g_trampStatTrigger = nullptr;
using StatTriggerFn = void(__fastcall*)(void*, void*, void*);
static void __fastcall StatTriggerDetour(void* self, void* edx, void* player) {
    __try {
        const unsigned long data = *(const unsigned long*)((const unsigned char*)self + 0x14);
        const int ev = data != 0 ? *(const int*)((const unsigned char*)(ULONGLONG)data + 0x60) : -1;
        if (ev >= 0) {
            const LONG w = InterlockedIncrement(&g_statWrite) - 1;
            StatEv& e = g_statRing[w & 1023];
            e.player = (unsigned long)(ULONG_PTR)player;
            e.ev = ev;
            e.px = 0;
            e.py = 0;
            e.value = 1.0f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ((StatTriggerFn)g_trampStatTrigger)(self, edx, player);
}
static void InstallStatTriggerHookOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    unsigned char* t = (unsigned char*)(ULONGLONG)0x012BC7C0UL;
    static const unsigned char kPro[13] = {0x81, 0xEC, 0x68, 0x01, 0x00, 0x00, 0xF6, 0x05, 0x58, 0x36, 0x84, 0x02, 0x04};
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: StatEventTrigger 0x012BC7C0 — пролог не совпал, перехват НЕ ставлю");
        return;
    }
    const HookResult r = g_hookStatTrigger.Install(t, (void*)&StatTriggerDetour, "VextPlayerManager:g_hookStatTrigger");
    if (r != HookResult::Ok) {
        LOG_ERR("Bridge: StatEventTrigger — установка не удалась: %s", ToString(r));
        return;
    }
    g_trampStatTrigger = g_hookStatTrigger.Trampoline();
    LOG_INFO("Bridge: ★ StatEventTrigger перехвачен (0x012BC7C0) — стат-события карты (МКОМ) в ScoringSystem:StatEvent");
}

// ★ Сборка 186: ГЛАВНЫЙ источник — fb::ScoringDispatcher::executeEvent 0x013164F0 (имя — строка в коде функции,
// re\method_index.txt): thiscall(this, игрок, statEvent, paramX, paramY, float value), ret 0x14; statEvent < 0x90.
// Через неё идут все стат-события (лечение, пополнение, реанимация, МКОМ, убийства). При paramX != 0 она зовёт себя
// с paramX = 0 — пишем только внешний вызов (глубина на поток). Прежние источники (0x011749C0, триггер карты) при этом
// не ставятся (повторы); EVBOTS_STAT_OLD=1 — вернуть их.
static InlineHook g_hookScoring;
static void* g_trampScoring = nullptr;
static volatile LONG g_scoringOn = 0;
using ScoringFn = void(__fastcall*)(void*, void*, void*, int, int, int, float);
static __declspec(thread) int t_scoringDepth = 0;
static void __fastcall ScoringDetour(void* self, void* edx, void* player, int ev, int px, int py, float value) {
    if (t_scoringDepth == 0 && ev != 55 && ev != 56) {  // 55/56 — выстрел/попадание на каждую пулю, моду не нужны
        const LONG w = InterlockedIncrement(&g_statWrite) - 1;
        StatEv& e = g_statRing[w & 1023];
        e.player = (unsigned long)(ULONG_PTR)player;
        e.ev = ev;
        e.px = px;
        e.py = py;
        e.value = value;
    }
    ++t_scoringDepth;
    ((ScoringFn)g_trampScoring)(self, edx, player, ev, px, py, value);
    --t_scoringDepth;
}
static void InstallScoringHookOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    unsigned char* t = (unsigned char*)(ULONGLONG)0x013164F0UL;
    static const unsigned char kPro[12] = {0x81, 0xEC, 0x60, 0x01, 0x00, 0x00, 0x53, 0x55, 0x56, 0x8B, 0xB4, 0x24};
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: ScoringDispatcher::executeEvent 0x013164F0 — пролог не совпал, перехват НЕ ставлю");
        return;
    }
    if (g_hookScoring.Install(t, (void*)&ScoringDetour, "VextPlayerManager:g_hookScoring") != HookResult::Ok) {
        LOG_ERR("Bridge: ScoringDispatcher::executeEvent — установка не удалась");
        return;
    }
    g_trampScoring = g_hookScoring.Trampoline();
    InterlockedExchange(&g_scoringOn, 1);
    LOG_INFO("Bridge: ★ ScoringDispatcher::executeEvent перехвачен (0x013164F0) — все стат-события, ScoringSystem:StatEvent как VU");
}
// ★ Сборка 207: ассерт движка «ServerTransactionMessageListener.cpp(715) Got DownloadProcessed for locally unregistered
// player» (1 раз за запуск, на первом боте). Разбор exe 20.10 (capstone): fb::ServerTransactionMessageListener::onStatisticsMessage
// 0x011D3600 (thiscall(this, msg), ret 4) при [msg+0x28] == 1 (DownloadProcessed) ищет игрока по onlineId [msg+0x38..0x3F]
// в своей таблице; не нашёл — ТОЛЬКО ассерт и выход. В таблицу onPlayerAdd 0x011D6DD0 кладёт лишь людей с соединением
// ([p+0x318] == 0 и [p+0x3BC] != 0), боты — «Not adding player». Статистику ботам отдаёт бэкенд (звания), поэтому сообщение
// приходит. Фильтр: такое сообщение для игрока, которого onPlayerAdd не добавил бы (тот же признак), в функцию не передаём —
// результат тот же, без ассерта. Поиск игрока — как 0x00E5F3B0: вектор [[[0x02B3EE2C]+0x30]+0x80 .. +0x84], [p+0x20..0x24].
// EVBOTS_DLPROC_FILTER=0 — выключить.
static InlineHook g_hookStatMsg;
static void* g_trampStatMsg = nullptr;
static volatile LONG g_dlSkipped = 0;
using StatMsgFn = void(__fastcall*)(void*, void*, void*);
static bool IsUnregisteredForTelemetry(unsigned long long onlineId) {
    __try {
        const unsigned long gs = *(const unsigned long*)(ULONGLONG)0x02B3EE2CUL;
        if (gs == 0) return false;
        const unsigned long pm = *(const unsigned long*)((const unsigned char*)(ULONGLONG)gs + 0x30);
        if (pm == 0) return false;
        const unsigned long* it = *(unsigned long* const*)((const unsigned char*)(ULONGLONG)pm + 0x80);
        const unsigned long* end = *(unsigned long* const*)((const unsigned char*)(ULONGLONG)pm + 0x84);
        for (int n = 0; it != nullptr && it < end && n < 256; ++it, ++n) {
            const unsigned char* p = (const unsigned char*)(ULONGLONG)*it;
            if (p == nullptr || *(const unsigned long long*)(p + 0x20) != onlineId) continue;
            return *(const unsigned char*)(p + 0x318) != 0 || *(const unsigned char*)(p + 0x3BC) == 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;  // не нашли — пусть решает движок (у него своё сообщение «missing from PlayerManager»)
}
static void __fastcall StatMsgDetour(void* self, void* edx, void* msg) {
    bool skip = false;
    unsigned long long id = 0;
    __try {
        const unsigned char* m = (const unsigned char*)msg;
        if (m != nullptr && *(const int*)(m + 0x28) == 1) {
            id = *(const unsigned long long*)(m + 0x38);
            skip = IsUnregisteredForTelemetry(id);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skip = false;
    }
    if (skip) {
        if (InterlockedIncrement(&g_dlSkipped) == 1)
            LOG_INFO("Bridge: телеметрия: DownloadProcessed игрока без соединения (onlineId %llu) не передаю — как onPlayerAdd", id);
        return;
    }
    ((StatMsgFn)g_trampStatMsg)(self, edx, msg);
}
static void InstallDownloadProcessedFilterOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    if (!EnvOn("EVBOTS_DLPROC_FILTER", true)) {
        LOG_INFO("Bridge: фильтр DownloadProcessed выключен (EVBOTS_DLPROC_FILTER=0)");
        return;
    }
    unsigned char* t = (unsigned char*)(ULONGLONG)0x011D3600UL;
    // sub esp,10 / push ebx / push esi / mov esi,[esp+1C] / push 024E0AE8 («StatisticsEventMessageBase»)
    static const unsigned char kPro[14] = {0x83, 0xEC, 0x10, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x1C, 0x68, 0xE8, 0x0A, 0x4E, 0x02};
    // и вызов поиска игрока там же, где у оригинала (0x011D369D: call 0x00E5F3B0) — признак, что раскладка та же
    static const unsigned char kFind[5] = {0xE8, 0x0E, 0xBD, 0xC8, 0xFF};
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0 &&
               ::memcmp((const unsigned char*)(ULONGLONG)0x011D369DUL, kFind, sizeof(kFind)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: onStatisticsMessage 0x011D3600 — пролог не совпал, фильтр DownloadProcessed НЕ ставлю");
        return;
    }
    if (g_hookStatMsg.Install(t, (void*)&StatMsgDetour, "VextPlayerManager:g_hookStatMsg") != HookResult::Ok) {
        LOG_ERR("Bridge: onStatisticsMessage — установка не удалась");
        return;
    }
    g_trampStatMsg = g_hookStatMsg.Trampoline();
    LOG_INFO("Bridge: ★ onStatisticsMessage перехвачен (0x011D3600) — DownloadProcessed ботов не доходит до телеметрии");
}

// ★ Сборка 208: ПАДЕНИЕ СЕРВЕРА НА СМЕНЕ УРОВНЯ (повторяемо: cycle-quick 20.10 06:13, MP_001 CQ → TDM; раньше Rush → SQDM,
// выход из AirSuperiority). Факт (лог + capstone): через 5 с после Level:Loaded посадка бота шлёт ServerPlayerRespawnMessage,
// обработчик fb::ServerFriendZoneEntity (0x012E65F0 → 0x012E54A0, строка «m_playerInsideShapeIndex») берёт байт по индексу игрока
// из таблицы [зона+0x48], а у новой зоны она ещё NULL (performPrecalculations не прошёл) — EIP 0x012E5511, адрес = индекс игрока
// (1, 4). Исключение внутри рассылки оставляет блокировку, через 18 с сторожевой таймер роняет сервер. У людей BF3 так рано не
// возрождаются, поэтому у движка проверки нет. Защита: 0x012E54A0 (thiscall(зона, индекс), ret 4; вызывающие 0x012E65D8,
// 0x012E666E, 0x012E7748 результат не читают) при [зона+0x48] == 0 не вызываем — убирать игрока из пустой таблицы нечего.
// EVBOTS_FRIENDZONE_GUARD=0 — выключить.
static InlineHook g_hookFriendZone;
static void* g_trampFriendZone = nullptr;
static volatile LONG g_fzSkipped = 0;
using FriendZoneFn = void(__fastcall*)(void*, void*, int);
static void __fastcall FriendZoneDetour(void* self, void* edx, int index) {
    bool empty = false;
    __try {
        empty = self != nullptr && *(const unsigned long*)((const unsigned char*)self + 0x48) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        empty = true;
    }
    if (empty) {
        const LONG n = InterlockedIncrement(&g_fzSkipped);
        if (n <= 3 || (n % 200) == 0)
            LOG_INFO("Bridge: FriendZone %p ещё не рассчитана ([+0x48]=0) — игрока %d из неё не убираю (было бы падение), раз %ld",
                     self, index, n);
        return;
    }
    ((FriendZoneFn)g_trampFriendZone)(self, edx, index);
}
// ★ Сборка 208 (2): тот же источник — второе место (прогон 06:34): обработчик сообщений зоны 0x012E65F0 (thiscall(слушатель,
// msg), ret 4, в таблице vtable, результат не читается) пишет счётчик игрока в [слушатель+0x30] = NULL (EIP 0x012E6937,
// запись по 0x14). Таблицы зоны — [слушатель+0x30] и [слушатель+0x34] (= [зона+0x48], зона = слушатель−0x14). Пока их нет,
// сообщения зоны не передаём (обновлять нечего); тип сообщения — в журнал (первые 12 разных).
static InlineHook g_hookFzMsg;
static void* g_trampFzMsg = nullptr;
using FzMsgFn = void(__fastcall*)(void*, void*, void*);
static void __fastcall FzMsgDetour(void* self, void* edx, void* msg) {
    bool empty = false;
    unsigned long cat = 0, typ = 0;
    __try {
        const unsigned char* s = (const unsigned char*)self;
        empty = s != nullptr && (*(const unsigned long*)(s + 0x30) == 0 || *(const unsigned long*)(s + 0x34) == 0);
        if (empty && msg != nullptr) {
            cat = *(const unsigned long*)((const unsigned char*)msg + 4);
            typ = *(const unsigned long*)((const unsigned char*)msg + 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        empty = true;
    }
    if (empty) {
        static unsigned long s_seen[12] = {};
        static volatile LONG s_n = 0;
        bool known = false;
        for (LONG i = 0; i < s_n && i < 12; ++i) known = known || s_seen[i] == (cat ^ typ);
        if (!known && s_n < 12) {
            s_seen[s_n] = cat ^ typ;
            InterlockedIncrement(&s_n);
            LOG_INFO("Bridge: FriendZone %p без таблиц — сообщение %08lX/%08lX не передаю (было бы падение)", self, cat, typ);
        }
        return;
    }
    ((FzMsgFn)g_trampFzMsg)(self, edx, msg);
}
static void InstallFzMsgGuard() {
    unsigned char* t = (unsigned char*)(ULONGLONG)0x012E65F0UL;
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,0xAF4
    static const unsigned char kPro[12] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xF4, 0x0A, 0x00, 0x00};
    // и место записи счётчика: mov edx,[esi+0x30] / add eax,ecx / mov byte [edx+eax*4],0 (0x012E6932)
    static const unsigned char kWr[9] = {0x8B, 0x56, 0x30, 0x03, 0xC1, 0xC6, 0x04, 0x82, 0x00};
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0 &&
               ::memcmp((const unsigned char*)(ULONGLONG)0x012E6932UL, kWr, sizeof(kWr)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: FriendZone onMessage 0x012E65F0 — пролог не совпал, защиту НЕ ставлю");
        return;
    }
    if (g_hookFzMsg.Install(t, (void*)&FzMsgDetour, "VextPlayerManager:g_hookFzMsg") != HookResult::Ok) {
        LOG_ERR("Bridge: FriendZone onMessage — установка не удалась");
        return;
    }
    g_trampFzMsg = g_hookFzMsg.Trampoline();
    LOG_INFO("Bridge: ★ FriendZone onMessage 0x012E65F0 под защитой — сообщения зоны без таблиц не роняют сервер");
}
static void InstallFriendZoneGuardOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    if (EnvOn("EVBOTS_FRIENDZONE_GUARD", true)) InstallFzMsgGuard();
    if (!EnvOn("EVBOTS_FRIENDZONE_GUARD", true)) {
        LOG_INFO("Bridge: защита FriendZone выключена (EVBOTS_FRIENDZONE_GUARD=0)");
        return;
    }
    unsigned char* t = (unsigned char*)(ULONGLONG)0x012E54A0UL;
    // push ebp / mov ebp,esp / and esp,-16 / mov eax,[0x02B3EE2C]
    static const unsigned char kPro[11] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xA1, 0x2C, 0xEE, 0xB3, 0x02};
    // и само место чтения таблицы: mov ecx,[edi+0x48] / mov [esp+1C],eax / mov al,[eax+ecx] (0x012E550A)
    static const unsigned char kRead[10] = {0x8B, 0x4F, 0x48, 0x89, 0x44, 0x24, 0x1C, 0x8A, 0x04, 0x08};
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0 &&
               ::memcmp((const unsigned char*)(ULONGLONG)0x012E550AUL, kRead, sizeof(kRead)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: FriendZone 0x012E54A0 — пролог не совпал, защиту НЕ ставлю");
        return;
    }
    if (g_hookFriendZone.Install(t, (void*)&FriendZoneDetour, "VextPlayerManager:g_hookFriendZone") != HookResult::Ok) {
        LOG_ERR("Bridge: FriendZone — установка не удалась");
        return;
    }
    g_trampFriendZone = g_hookFriendZone.Trampoline();
    LOG_INFO("Bridge: ★ FriendZone 0x012E54A0 под защитой — нерассчитанная зона не роняет сервер на смене уровня");
}

// ★ Сборка 193: приказы колеса BF3 (Q: «нужен медик», «нужны патроны», «нужен ремонт», «садись», «за мной»…) — замена меню
// клиентской части мода (у VU — WebUI мода, Alt → NetEvent BotEditor). Клиент шлёт VeniceNetworkOrderMessage (тип 0xB85845AC,
// строка 0x02645D54); серверный компонент разбирает его в 0x012C4B40 (категория 0x802AE469) и зовёт 0x012C2EA0 —
// thiscall(компонент, сообщение), ret 4, один вызов (0x012C4B93). В сообщении: [+0x3C] и [+0x44] — слабые ссылки на сущности
// (getter 0x023B7CE0 / 0x023B7D20: [[+off]] − 8), [+0x48] — OrderType (0x012C22F0 кладёт его в [компонент+0x38]).
// Кто из A/B — заказчик, а кто цель, не подтверждено: в лог пишем всё. Выключатель — EVBOTS_ORDER_HOOK=0.
struct OrderEv { unsigned long comp, a, b; int type; };
static OrderEv g_orderRing[64];
static volatile LONG g_orderWrite = 0;
static LONG g_orderRead = 0;
static InlineHook g_hookOrder;
static void* g_trampOrder = nullptr;
using OrderFn = void(__fastcall*)(void*, void*, void*);
static unsigned long OrderWeakEntity(const unsigned char* msg, int off) {
    const unsigned long p = *(const unsigned long*)(msg + off);
    if (p == 0) return 0;
    const unsigned long e = *(const unsigned long*)(ULONGLONG)p;
    return e != 0 ? e - 8 : 0;
}
static void __fastcall OrderDetour(void* self, void* edx, void* msg) {
    __try {
        const unsigned char* m = (const unsigned char*)msg;
        OrderEv e;
        e.comp = (unsigned long)(ULONG_PTR)self;
        e.a = OrderWeakEntity(m, 0x3C);
        e.b = OrderWeakEntity(m, 0x44);
        e.type = *(const int*)(m + 0x48);
        // Сборка 194 (проверяющий): слот публикуется после записи. Писатель один — поток движка (диспетчер сообщений).
        const LONG w = InterlockedCompareExchange(&g_orderWrite, 0, 0);
        g_orderRing[w & 63] = e;
        InterlockedIncrement(&g_orderWrite);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ((OrderFn)g_trampOrder)(self, edx, msg);
}
static void InstallOrderHookOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_ORDER_HOOK", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') return;
    unsigned char* t = (unsigned char*)(ULONGLONG)0x012C2EA0UL;
    static const unsigned char kPro[9] = {0x83, 0xEC, 0x28, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x34};
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: приказы колеса 0x012C2EA0 — пролог не совпал, перехват НЕ ставлю");
        return;
    }
    if (g_hookOrder.Install(t, (void*)&OrderDetour, "VextPlayerManager:g_hookOrder") != HookResult::Ok) {
        LOG_ERR("Bridge: приказы колеса — установка перехвата не удалась");
        return;
    }
    g_trampOrder = g_hookOrder.Trampoline();
    LOG_INFO("Bridge: ★ приказы колеса BF3 перехвачены (VeniceNetworkOrderMessage, 0x012C2EA0) — команды меню мода");
}
// ★ Сборка 197, диагностика (поведение не меняет): «Pure function call have been made» в журнале движка (WinMain.cpp(46)) —
// прогон 196 ×5, прогон 194 ×1, стек там пустой («main»). Обработчик движка 0x00406E80 (_purecall → он): пишет журнал и зовёт
// exit(1) (импорт MSVCR90 [0x02BDDBE0]), но сервер жил дальше — значит, поток завис в exit (гипотеза). Пишем в наш лог поток
// (главный — тот, где идёт хук тика, `g_engineMainTid`) и адреса возврата со стека (в .text, после call) — кто зовёт.
static volatile LONG g_engineMainTid = 0;
static InlineHook g_hookPureCall;
static void* g_trampPureCall = nullptr;
static bool AfterCall(unsigned long ra) {
    if (ra < 0x00401010UL || ra >= 0x022AB63CUL) return false;  // .text 0x00401000 + 0x01EAA63C
    __try {
        const unsigned char* p = (const unsigned char*)(ULONGLONG)ra;
        if (p[-5] == 0xE8) return true;                                      // call rel32
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;            // call reg / [reg]
        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;            // call [reg+disp8]
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;            // call [disp32] / [reg+disp32]
        if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;            // call [reg+reg*s+disp32]
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}
// ★ 23.10 (211): скан шёл с `&marker` — через свои же неинициализированные `ras/buf/who`, где лежит мёртвый стек прошлых
// вызовов (в прогонах 197 и 211 первыми шли 004CEC0F ← 0085B816 ← 0085C325 — луч SpatialQueryManager с замером CpuTiming:
// такой же связный след оставляет любой прошлый луч). Перехват — JMP (InlineHook), поэтому на входе [esp] — точный адрес
// возврата в функцию, сделавшую виртуальный вызов, а ECX — её `this` (thiscall). Пишем их, vtable объекта и байты вызова;
// скан — только выше своего кадра. Флаг [0x028C8168] (0x004AB220) решает, что сделает обработчик: int3 или exit(1).
static void __fastcall PureCallDetour(unsigned long ecxThis, unsigned long /*edx*/) {
    const unsigned long callerRa = (unsigned long)(ULONGLONG)_ReturnAddress();
    const unsigned long* sp = (const unsigned long*)_AddressOfReturnAddress();
    unsigned long ras[24] = {};
    int n = 0;
    __try {
        for (int i = 1; i < 1024 && n < 24; ++i) {
            const unsigned long v = sp[i];
            if (AfterCall(v)) ras[n++] = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    char buf[24 * 10 + 8] = {};
    int pos = 0;
    for (int i = 0; i < n && pos < (int)sizeof(buf) - 10; ++i) {
        pos += ::sprintf_s(buf + pos, sizeof(buf) - pos, " %08lX", ras[i]);
    }
    // ★ Сборка 212 (прогон 23.10): обработчик зовёт не vtable, а `_purecall` MSVCR90 (`call eax` → возврат 713D19E9), ECX там
    // уже не `this` (было 004026D5). У `_purecall` до `call eax` стек сбалансирован (push/call DecodePointer/pop ecx), поэтому
    // [esp+4] (= sp[1]) — точный адрес возврата из виртуального вызова. Он может быть вне exe — пишем модуль и смещение.
    (void)ecxThis;
    unsigned long siteRa = 0, callBytes[2] = {}, raw[6] = {};
    int breakFlag = -1;
    __try {
        siteRa = sp[1];
        for (int i = 0; i < 6; ++i) raw[i] = sp[1 + i];
        callBytes[0] = *(const unsigned long*)(ULONGLONG)(siteRa - 8);
        callBytes[1] = *(const unsigned long*)(ULONGLONG)(siteRa - 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        breakFlag = *(const unsigned char*)(ULONGLONG)0x028C8168UL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    char modPath[MAX_PATH] = "?";
    unsigned long modOff = 0;
    HMODULE mod = nullptr;
    if (siteRa != 0 && ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            (LPCSTR)(ULONGLONG)siteRa, &mod) && mod != nullptr) {
        ::GetModuleFileNameA(mod, modPath, MAX_PATH);
        modOff = (unsigned long)(siteRa - (unsigned long)(ULONGLONG)mod);
    }
    const char* modName = ::strrchr(modPath, '\\');
    modName = (modName != nullptr) ? modName + 1 : modPath;
    const DWORD tid = ::GetCurrentThreadId();
    const LONG mainTid = InterlockedCompareExchange(&g_engineMainTid, 0, 0);
    LOG_ERR("Bridge: ★ PURE CALL движка (0x00406E80): поток %lu (%s), обработчик из %08lX (_purecall CRT); виртуальный вызов — "
            "возврат %08lX (%s+0x%lX), байты до: %08lX %08lX; [esp+4..]: %08lX %08lX %08lX %08lX %08lX %08lX; флаг int3 "
            "[0x028C8168]=%d", (unsigned long)tid, (LONG)tid == mainTid ? "главный, хук тика" : "НЕ главный", callerRa, siteRa,
            modName, modOff, callBytes[0], callBytes[1], raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], breakFlag);
    LOG_ERR("Bridge: PURE CALL — адреса возврата выше кадра:%s", buf);
    // ★ Сборка 198: прогон 197 — 3 из 4 в ServerSoldierEntity::prePhysicsUpdate (0x00E0A7C0, слот vtable 0x025C3A8C), в деструкторе
    // локального объекта (0x0044E840 → база 0x00494190). Чей солдат: ищем на стеке указатели солдат ботов и человека.
    char who[256] = {};
    int wpos = 0;
    __try {
        for (LONG b = -1; b < g_watchCount && b < 64 && wpos < (int)sizeof(who) - 48; ++b) {
            const unsigned long pl = b < 0 ? ::evbots::EVBotsEngineRealPlayer() : g_watchBot[b];
            if (pl == 0) continue;
            const unsigned long sol = *(const unsigned long*)(ULONGLONG)(pl + kOffPlayerSoldier);
            if (sol == 0) continue;
            for (int i = 0; i < 1024; ++i) {
                if (sp[i] != sol) continue;
                const char* nm = "?";
                for (LONG k = 0; k < g_ptrNameCount; ++k) {
                    if (g_ptrName[k].ptr == pl) { nm = g_ptrName[k].name; break; }
                }
                // сборка 237: с обрезкой — sprintf_s при нехватке места завершает процесс (fail-fast CRT)
                const int wn = ::_snprintf_s(who + wpos, sizeof(who) - wpos, _TRUNCATE, " %s:%s солдат=%08lX [+%d]",
                                             b < 0 ? "ЧЕЛОВЕК" : "бот", nm, sol, i * 4);
                wpos = wn < 0 ? (int)sizeof(who) - 1 : wpos + wn;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    LOG_ERR("Bridge: PURE CALL — солдаты на стеке:%s", wpos > 0 ? who : " нет");
    ((void(__cdecl*)())g_trampPureCall)();
}
static void InstallPureCallHookOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    unsigned char* t = (unsigned char*)(ULONGLONG)0x00406E80UL;
    static const unsigned char kPro[8] = {0x81, 0xEC, 0x30, 0x01, 0x00, 0x00, 0xF6, 0x05};  // sub esp,130h; test byte [..]
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: обработчик pure call 0x00406E80 — пролог не совпал, перехват НЕ ставлю");
        return;
    }
    // Пролог: 6 байт sub esp + test byte ptr [abs32], imm8 (7 байт, адрес абсолютный — перенос в трамплин безопасен).
    if (g_hookPureCall.Install(t, (void*)&PureCallDetour, "VextPlayerManager:g_hookPureCall") != HookResult::Ok) {
        LOG_ERR("Bridge: обработчик pure call — установка перехвата не удалась");
        return;
    }
    g_trampPureCall = g_hookPureCall.Trampoline();
    LOG_INFO("Bridge: ★ обработчик pure call движка перехвачен (0x00406E80) — поток и стек в лог (диагностика)");
}

// ★ Сборка 198, диагностика для Soldier:Damage «как VU» (хук ДО урона, мод меняет info.damage: множители урона ботов, запрет
// самоубийства — BotManager.lua:308–345). Место у сервера: ServerSoldierEntity::onDamage 0x00E08000 (слот vtable солдата
// 0x025C3A58, строка профилировщика «ServerSoldierEntity_onDamage»), thiscall(солдат, DamageInfo*, DamageGiverInfo*), ret 8;
// внутри — модуль здоровья [солдат+0x538] → vf0(info, giver), если [солдат+0x51C] == 0. Смещение поля damage не известно:
// пишем байты DamageInfo/DamageGiverInfo и падение HP ([солдат+0x34]) для первых 12 попаданий. Поведение не меняет.
// EVBOTS_DMG_PROBE=0 — не ставить.
static InlineHook g_hookSoldierDmg;
static void* g_trampSoldierDmg = nullptr;
static volatile LONG g_soldierDmgLogged = 0;
// ★ Сборка 199: ТРИ аргумента на стеке — функция кончается `ret 0xC` (0x00E08094, 0x00E08129); в 198 перехват был на два (ret 8),
// стек вызывающего сдвигался — падение сервера на первом попадании (прогон 10:11:53: AV 0x012BE90E, «vtable» = DamageInfo[0]).
// Аргумент 2 — по значению (у оригинала `lea ecx, [esp+0x14]`), аргумент 3 уходит дальше.
static unsigned long g_curBulletShooter = 0;  // сборка 199: стрелок пули, пока идёт handleCollision (см. DetourHandleCollision)
static volatile LONG g_dmgPreCalls = 0, g_dmgPreBusy = 0, g_dmgPreChanged = 0, g_dmgPreLogged = 0;
static bool DmgPreOn() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_DMG_PRE", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;
    }
    return s_on != 0;
}
/// Игрок движка, чей солдат — soldier (боты под наблюдением и человек); 0 — не нашли.
static unsigned long PlayerOfSoldier(unsigned long soldier) {
    if (soldier == 0) return 0;
    unsigned long hs[64];
    const int nh = ::evbots::EVBotsEngineHumans(hs, 64);  // ★ сборка 202: все люди, не только первый
    __try {
        for (int k = 0; k < nh; ++k) {
            const unsigned long h = hs[k];
            if (h != 0 && *(const unsigned long*)(ULONGLONG)(h + kOffPlayerSoldier) == soldier) return h;
        }
        for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
            const unsigned long pl = g_watchBot[i];
            if (pl != 0 && *(const unsigned long*)(ULONGLONG)(pl + kOffPlayerSoldier) == soldier) return pl;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}
// ★ Сборка 199: Soldier:Damage ДО урона, как VU. Урон — float [DamageInfo+0x40] (модуль здоровья 0x01180190: `0.0 < [info+0x40]`;
// прогон 199: −0,4 при лечении на полном HP; 10:35:50 — HP 100 → 66 при [info+0x40] = 0x42080000 = 34,0 — факт). Только урон > 0 (лечение мод пропускает сам: BotManager.lua:316). Стрелок — пуля
// (g_curBulletShooter), иначе неизвестен (взрыв, падение — как у нас и было). Lua занята дольше 3 мс — урон без изменений, событие
// уйдёт обычным путём после урона. EVBOTS_DMG_PRE=0 — выкл.
/// Стрелок из 2-го аргумента onDamage: [arg2+0] — слабая ссылка игрока, та же, что [игрок+0x4AC] (прогон 199, 10:35:50: урон 34 по
/// двум ботам, [arg2] = F2C93A38 = weak бота F1EEB180). Ищем среди ботов под наблюдением и человека; 0 — не нашли.
static unsigned long GiverFromArg2(void* arg2) {
    if (arg2 == nullptr) return 0;
    __try {
        const unsigned long w = *(const unsigned long*)arg2;
        if (w == 0) return 0;
        // ★ Сборка 202: стрелок — любой из людей (раньше только первый: выстрелы второго человека мод не видел — «не реагируют»)
        unsigned long hs[64];
        const int nh = ::evbots::EVBotsEngineHumans(hs, 64);
        for (int k = 0; k < nh; ++k) {
            const unsigned long h = hs[k];
            if (h != 0 && *(const unsigned long*)(ULONGLONG)(h + 0x4AC) == w) return h;
        }
        for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
            const unsigned long pl = g_watchBot[i];
            if (pl != 0 && *(const unsigned long*)(ULONGLONG)(pl + 0x4AC) == w) return pl;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}
static void DamagePreHook(void* self, void* info, void* arg2, float healthBefore) {
    if (!DmgPreOn() || info == nullptr) return;
    float dmg = 0.0f;
    __try { dmg = *(const float*)((const unsigned char*)info + 0x40); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!(dmg > 0.0f && dmg < 100000.0f)) return;
    const unsigned long victim = PlayerOfSoldier((unsigned long)(ULONGLONG)self);
    if (victim == 0) return;
    unsigned long shooter = GiverFromArg2(arg2);
    if (shooter == 0) shooter = g_curBulletShooter;
    const bool byHuman = shooter != 0 && ::evbots::EVBotsEngineIsHuman(shooter);  // сборка 202: любой человек — урон клиента
    InterlockedIncrement(&g_dmgPreCalls);
    double out = (double)dmg;
    const bool ok = LuaRuntime::Instance().CallTableFunction(
        "EVBotsDamage", "Pre",
        {LuaRuntime::EngineEventArg::Num((double)victim), LuaRuntime::EngineEventArg::Num((double)shooter),
         LuaRuntime::EngineEventArg::Num((double)dmg), LuaRuntime::EngineEventArg::Bool(shooter != 0),
         LuaRuntime::EngineEventArg::Bool(byHuman), LuaRuntime::EngineEventArg::Num(healthBefore)},
        3, &out);
    if (!ok) { InterlockedIncrement(&g_dmgPreBusy); return; }
    const float nd = (float)out;
    if (!NonFiniteGuard(out, "Soldier:Damage", 0) || nd == dmg) return;
    const float clamped = nd < 0.0f ? 0.0f : nd;
    __try { *(float*)((unsigned char*)info + 0x40) = clamped; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    InterlockedIncrement(&g_dmgPreChanged);
    if (InterlockedIncrement(&g_dmgPreLogged) <= 30) {
        LOG_INFO("Bridge: Soldier:Damage ДО урона (как VU): жертва %08lX, стрелок %08lX, урон %.2f -> %.2f", victim, shooter,
                 (double)dmg, (double)clamped);
    }
}

static void __fastcall SoldierDmgDetour(void* self, void* edx, void* info, void* arg2, void* giver) {
    float hp0 = -1.0f;
    __try { hp0 = *(const float*)((const unsigned char*)self + 0x34); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    DamagePreHook(self, info, arg2, hp0);
    ((void(__fastcall*)(void*, void*, void*, void*, void*))g_trampSoldierDmg)(self, edx, info, arg2, giver);
    if (InterlockedCompareExchange(&g_soldierDmgLogged, 0, 0) >= 12) return;
    float hp1 = -1.0f;
    __try { hp1 = *(const float*)((const unsigned char*)self + 0x34); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    // Сборка 199: прогон 10:26 — все 12 записей ушли на лечение ([info+0x40] = −0,4 при HP 100). Пишем только падения HP.
    if (!(hp0 - hp1 > 0.01f)) return;
    char fi[0x60 / 4 * 10 + 8] = {};
    char g2[0x30 / 4 * 10 + 8] = {};
    char gi[0x30 / 4 * 10 + 8] = {};
    int fp = 0, p2 = 0, gp = 0;
    __try {
        const unsigned long* a = (const unsigned long*)info;
        for (int i = 0; i < 0x60 / 4; ++i) fp += ::sprintf_s(fi + fp, sizeof(fi) - fp, " %08lX", a[i]);
        if (arg2 != nullptr) {
            const unsigned long* g = (const unsigned long*)arg2;
            for (int i = 0; i < 0x30 / 4; ++i) p2 += ::sprintf_s(g2 + p2, sizeof(g2) - p2, " %08lX", g[i]);
        }
        if (giver != nullptr) {
            const unsigned long* g = (const unsigned long*)giver;
            for (int i = 0; i < 0x30 / 4; ++i) gp += ::sprintf_s(gi + gp, sizeof(gi) - gp, " %08lX", g[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (InterlockedIncrement(&g_soldierDmgLogged) > 12) return;
    LOG_INFO("ДИАГ onDamage солдата %p: HP %.2f -> %.2f (упало %.2f) | DamageInfo[0..0x60]:%s | arg2=%p:%s | arg3=%p:%s",
             self, (double)hp0, (double)hp1, (double)(hp0 - hp1), fi, arg2, p2 > 0 ? g2 : " nil", giver, gp > 0 ? gi : " nil");
}
static void InstallSoldierDmgProbeOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_DMG_PROBE", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') return;
    unsigned char* t = (unsigned char*)(ULONGLONG)0x00E08000UL;
    static const unsigned char kPro[7] = {0x53, 0x55, 0x33, 0xDB, 0x56, 0x8B, 0xF1};  // push ebx; push ebp; xor ebx,ebx; push esi; mov esi,ecx
    bool same = false;
    __try {
        // сборка 199: и оба выхода `ret 0xC` (C2 0C 00) — три аргумента на стеке, как у перехватчика
        static const unsigned char kRet[3] = {0xC2, 0x0C, 0x00};
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0 && *(const unsigned long*)(ULONGLONG)0x025C3A58UL == 0x00E08000UL &&
               ::memcmp((const void*)(ULONGLONG)0x00E08094UL, kRet, 3) == 0 &&
               ::memcmp((const void*)(ULONGLONG)0x00E08129UL, kRet, 3) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: ServerSoldierEntity::onDamage 0x00E08000 — пролог или слот vtable не совпал, диагностику НЕ ставлю");
        return;
    }
    if (g_hookSoldierDmg.Install(t, (void*)&SoldierDmgDetour, "VextPlayerManager:g_hookSoldierDmg") != HookResult::Ok) {
        LOG_ERR("Bridge: ServerSoldierEntity::onDamage — установка перехвата не удалась");
        return;
    }
    g_trampSoldierDmg = g_hookSoldierDmg.Trampoline();
    LOG_INFO("Bridge: ★ ServerSoldierEntity::onDamage перехвачен (0x00E08000) — диагностика DamageInfo для Soldier:Damage как VU");
}

/// NativeBridge.PollOrders() -> { {компонент, сущностьA, сущностьB, OrderType}, ... }
int l_bridge_poll_orders(lua_State* L) {
    lua_newtable(L);
    const LONG w = InterlockedCompareExchange(&g_orderWrite, 0, 0);
    if (w - g_orderRead > 64) g_orderRead = w - 64;
    int k = 0;
    while (g_orderRead < w) {
        const OrderEv e = g_orderRing[g_orderRead & 63];
        ++g_orderRead;
        lua_createtable(L, 4, 0);
        lua_pushinteger(L, (lua_Integer)e.comp); lua_rawseti(L, -2, 1);
        lua_pushinteger(L, (lua_Integer)e.a); lua_rawseti(L, -2, 2);
        lua_pushinteger(L, (lua_Integer)e.b); lua_rawseti(L, -2, 3);
        lua_pushinteger(L, (lua_Integer)e.type); lua_rawseti(L, -2, 4);
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

static bool StatOldOn() {
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_STAT_OLD", v, (DWORD)sizeof(v));
    return n == 1 && v[0] == '1';
}

/// NativeBridge.PollStatEvents() -> { {player, statEvent, paramX, paramY, value}, ... }
int l_bridge_poll_stat_events(lua_State* L) {
    lua_newtable(L);
    const LONG w = InterlockedCompareExchange(&g_statWrite, 0, 0);
    if (w - g_statRead > 1024) g_statRead = w - 1024;
    int k = 0;
    while (g_statRead < w) {
        const StatEv e = g_statRing[g_statRead & 1023];
        ++g_statRead;
        lua_createtable(L, 5, 0);
        lua_pushinteger(L, (lua_Integer)e.player);
        lua_rawseti(L, -2, 1);
        lua_pushinteger(L, e.ev);
        lua_rawseti(L, -2, 2);
        lua_pushinteger(L, e.px);
        lua_rawseti(L, -2, 3);
        lua_pushinteger(L, e.py);
        lua_rawseti(L, -2, 4);
        lua_pushnumber(L, e.value);
        lua_rawseti(L, -2, 5);
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

static unsigned long EngineLevelPtr() {
    __try {
        const unsigned long mainp = *(const unsigned long*)(ULONGLONG)0x02877210UL;
        if (mainp == 0) return 0;
        const unsigned long server = *(const unsigned long*)(ULONGLONG)(mainp + 0xD0);
        if (server == 0) return 0;
        unsigned long level = *(const unsigned long*)(ULONGLONG)(server + 0x74);
        if (level == 0) level = *(const unsigned long*)(ULONGLONG)(server + 0x7C);
        return level;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/// NativeBridge.LevelSignals(wantEntities) -> roundOverSeq, winner, levelLoadedSeq, levelPtr, ready, entities, hook, rconCalls,
/// unloadSeq, startupLoadedSeq (сборка 211)
int l_bridge_level_signals(lua_State* L) {
    const bool want = lua_toboolean(L, 1) != 0;
    lua_pushinteger(L, (lua_Integer)InterlockedCompareExchange(&g_roundOverSeq, 0, 0));
    lua_pushinteger(L, (lua_Integer)InterlockedCompareExchange(&g_roundOverWinner, 0, 0));
    lua_pushinteger(L, (lua_Integer)InterlockedCompareExchange(&g_levelLoadedSeq, 0, 0));
    lua_pushinteger(L, (lua_Integer)EngineLevelPtr());
    const bool ready = EngineLevelReady();
    lua_pushboolean(L, ready ? 1 : 0);
    lua_pushinteger(L, (lua_Integer)((want && ready) ? EVBotsEngineLevelEntityCount() : 0));
    lua_pushinteger(L, (lua_Integer)InterlockedCompareExchange(&g_rconInstalled, 0, 0));
    lua_pushinteger(L, (lua_Integer)InterlockedCompareExchange(&g_rconCalls, 0, 0));
    lua_pushinteger(L, (lua_Integer)InterlockedCompareExchange(&g_levelUnloadSeq, 0, 0));
    lua_pushinteger(L, (lua_Integer)InterlockedCompareExchange(&g_startupLoadedSeq, 0, 0));
    return 10;
}

/// NativeBridge.SetLevelGate(closed)
int l_bridge_set_level_gate(lua_State* L) {
    const bool closed = lua_toboolean(L, 1) != 0;
    const LONG prev = InterlockedExchange(&g_levelGate, closed ? 1 : 0);
    if ((prev != 0) != closed) LOG_INFO("Bridge: ворота спавна %s", closed ? "ЗАКРЫТЫ (смена уровня)" : "открыты (Level:Loaded)");
    return 0;
}
/// ★ Сборка 214: NativeBridge.LevelChanging() -> bool — уровень меняется (те же условия, что LevelGateClosed, без журнала).
int l_bridge_level_changing(lua_State* L) {
    const bool unloading = InterlockedCompareExchange(&g_levelCurSeen, 0, 0) != 0 && EngineCurrentLevelRaw() == 0;
    lua_pushboolean(L, (InterlockedCompareExchange(&g_levelGate, 0, 0) != 0 || unloading || !EngineLevelReady()) ? 1 : 0);
    return 1;
}

static InlineHook g_hookHandleCollision;
static void* g_trampHandleCollision = nullptr;
using HandleCollisionFn = int(__fastcall*)(void*, void*, void*, void*, void*, void*);

static int CallHandleCollisionWindow(void* bullet, void* a1, void* a2, void* a3, void* a4, unsigned char* b,
                                     unsigned char saved) {
    int r = 0;
    __try {
        r = ((HandleCollisionFn)g_trampHandleCollision)(bullet, nullptr, a1, a2, a3, a4);
    } __finally {
        if (b != nullptr) b[0x1E] = saved;
    }
    return r;
}

// ★ Сборка 163: хук VU «BulletEntity:Collision». Аргументы текущей пули — для HookContext:Call() (только главный поток).
struct BulletCallState {
    void* bullet;
    void* a1;
    void* a2;
    void* a3;
    void* a4;
    int result;
    bool called;
    bool active;
};
static BulletCallState g_bc = {};
static volatile LONG g_vuBulletLua = 0, g_vuBulletNoCall = 0, g_vuBulletBusy = 0;

int l_bridge_bullet_call_original(lua_State* L) {
    if (g_bc.active && !g_bc.called && g_trampHandleCollision != nullptr) {
        g_bc.called = true;
        g_bc.result = ((HandleCollisionFn)g_trampHandleCollision)(g_bc.bullet, nullptr, g_bc.a1, g_bc.a2, g_bc.a3,
                                                                  g_bc.a4);
    }
    lua_pushinteger(L, g_bc.result);
    return 1;
}

/// Хук VU включён: EVBOTS_VU_BULLET != 0 и GetSettings(«SyncedGameSettings») совпал с глобалом движка (поле +0x1E).
/// Иначе — прежнее нативное окно урона (сборка 131), чтобы урон ботов не пропал.
static bool VuBulletHookEnabled() {
    static int s_on = -1;
    if (s_on >= 0) return s_on == 1;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VU_BULLET", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') {
        s_on = 0;
        LOG_INFO("Bridge: хук VU BulletEntity:Collision выключен (EVBOTS_VU_BULLET=0) — нативное окно урона");
        return false;
    }
    unsigned long b = 0;
    __try {
        unsigned char* base = NativeModuleBase();
        b = base ? *(const unsigned long*)(base + (0x02B2AD70UL - 0x00400000UL)) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        b = 0;
    }
    s_on = (b != 0 && evbots::LiveSyncedSettingsCheck(b, 0x1E)) ? 1 : 0;
    LOG_INFO("Bridge: ★ урон пуль: %s", s_on ? "хук VU BulletEntity:Collision (мод открывает окно сам через GetSettings)"
                                             : "нативное окно (сверка GetSettings не прошла)");
    return s_on == 1;
}

// ★ Сборка 199: стрелок пули на время handleCollision — для Soldier:Damage ДО урона (ServerSoldierEntity::onDamage вызывается
// внутри handleCollision синхронно, в главном потоке движка). Стрелок — тот же, что в сборке 131: [[пуля+0x84]] − 8.
// (g_curBulletShooter объявлен у SoldierDmgDetour.)
static int __fastcall DetourHandleCollisionImpl(void* bullet, void* edx, void* a1, void* a2, void* a3, void* a4);
static int __fastcall DetourHandleCollision(void* bullet, void* edx, void* a1, void* a2, void* a3, void* a4) {
    unsigned long pl = 0;
    __try {
        const unsigned long tok = *(const unsigned long*)((const unsigned char*)bullet + 0x84);
        if (tok != 0) {
            const unsigned long v = *(const unsigned long*)(ULONG_PTR)tok;
            if (v != 0) pl = v - 8;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pl = 0;
    }
    const unsigned long prev = g_curBulletShooter;
    g_curBulletShooter = pl;
    const int r = DetourHandleCollisionImpl(bullet, edx, a1, a2, a3, a4);
    g_curBulletShooter = prev;
    return r;
}
static int __fastcall DetourHandleCollisionImpl(void* bullet, void* /*edx*/, void* a1, void* a2, void* a3, void* a4) {
    InterlockedIncrement(&g_hcCalls);
    unsigned long pl = 0;
    __try {
        const unsigned long tok = *(const unsigned long*)((const unsigned char*)bullet + 0x84);
        if (tok != 0) {
            const unsigned long v = *(const unsigned long*)(ULONG_PTR)tok;
            if (v != 0) pl = v - 8;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pl = 0;
    }
    // ★ Сборка 163: как в VU — сначала хук мода (BulletEntity:Collision). Лок Lua ждём не дольше 3 мс (164: точное ожидание по QPC): это главный
    // поток движка (сторож GameStall); не взяли — пуля идёт прежним нативным путём ниже.
    if (!g_bc.active && VuBulletHookEnabled()) {
        g_bc.bullet = bullet;
        g_bc.a1 = a1;
        g_bc.a2 = a2;
        g_bc.a3 = a3;
        g_bc.a4 = a4;
        g_bc.result = 0;
        g_bc.called = false;
        g_bc.active = true;
        const bool ok = LuaRuntime::Instance().CallTableFunction(
            "PlayerManager", "__BulletCollision", {LuaRuntime::EngineEventArg::Num((double)pl)}, 3);
        g_bc.active = false;
        if (ok || g_bc.called) {
            InterlockedIncrement(&g_vuBulletLua);
            int r = g_bc.result;
            if (!g_bc.called) {
                InterlockedIncrement(&g_vuBulletNoCall);  // обработчик не позвал Call() — оригинал после цепочки (Pass)
                r = ((HandleCollisionFn)g_trampHandleCollision)(bullet, nullptr, a1, a2, a3, a4);
            }
            if (pl != 0) ScanHpDrops(pl);
            return r;
        }
        InterlockedIncrement(&g_vuBulletBusy);
    }
    if (pl != 0 && OwnInputOf(pl) != nullptr) {
        InterlockedIncrement(&g_hcBot);
        unsigned char* b = nullptr;
        unsigned char saved = 0xFF;
        __try {
            unsigned char* base = NativeModuleBase();
            b = base ? *(unsigned char**)(base + (0x02B2AD70UL - 0x00400000UL)) : nullptr;
            if (b != nullptr) {
                saved = b[0x1E];
                if (saved <= 1) {
                    b[0x1E] = 0;
                    InterlockedIncrement(&g_hcFlip);
                } else {
                    b = nullptr;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            b = nullptr;
        }
        if (b != nullptr) {
            const int rw = CallHandleCollisionWindow(bullet, a1, a2, a3, a4, b, saved);
            ScanHpDrops(pl);  // сборка 153: Soldier:Damage — жертвы этой пули, стрелок известен
            return rw;
        }
    }
    const int r = ((HandleCollisionFn)g_trampHandleCollision)(bullet, nullptr, a1, a2, a3, a4);
    if (pl != 0) ScanHpDrops(pl);  // сборка 153
    return r;
}

/// ★ Сборка 153: вызывается из EngineHooks после ClientDamageListener::onMessage (урон от клиента человека).
void BridgeAfterClientDamage() {
    // ★ Сборка 202: людей несколько — чей клиент прислал урон, здесь не видно; стрелок неизвестен (0), а не «первый человек».
    // Стрелка даёт хук ДО урона (GiverFromArg2) — он и шлёт моду Soldier:Damage.
    unsigned long hs[2];
    const int nh = ::evbots::EVBotsEngineHumans(hs, 2);
    ScanHpDrops(nh == 1 ? hs[0] : 0);
}

static void InstallBotDamageHookOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_BOTDMG", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') {
        LOG_INFO("Bridge: урон от пуль ботов — перехват handleCollision выключен (EVBOTS_BOTDMG=0)");
        return;
    }
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return;
    unsigned char* target = base + (0x00E72040UL - 0x00400000UL);
    static const unsigned char kPro[12] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x34, 0x01, 0x00, 0x00};
    bool same = false;
    __try {
        same = ::memcmp(target, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: handleCollision 0x00E72040 — пролог не совпал, перехват НЕ ставлю");
        return;
    }
    const HookResult r = g_hookHandleCollision.Install(target, (void*)&DetourHandleCollision, "VextPlayerManager:g_hookHandleCollision");
    if (r != HookResult::Ok) {
        LOG_ERR("Bridge: handleCollision — установка не удалась: %s", ToString(r));
        return;
    }
    g_trampHandleCollision = g_hookHandleCollision.Trampoline();
    LOG_INFO("Bridge: ★ handleCollision 0x00E72040 перехвачен (украдено %u байт): урон от пуль ботов как в botlayer",
             (unsigned)g_hookHandleCollision.StolenBytes());
}

// ★ Сборка 185: события VU CombatArea:PlayerDeserting / CombatArea:PlayerReturning (entity, player). Мод разворачивает
// бота атакующей команды, ушедшего за границу (GameDirector:ToggleDirectionCombatZone). Разбор exe (18.10):
// ServerCombatAreaTriggerEntity::updateDeserters 0x00E55480 (this = сущность зоны) зовёт «начал дезертировать» 0x00E4FD20
// и «вернулся» 0x00E4FE10 — thiscall(сущность, игрок), ret 4; только они создают ServerGameplayDeserterMessage
// (ctor 0x02309FE0, хэш 0x387C1E27) и …DeserterReturnMessage (0x0230A030, 0x8D77BB8D), игрок — в +0x28. Перехват входов:
// запись (сущность, игрок, вид) в кольцо и вызов оригинала; Lua (EntityManager._Poll) шлёт события. EVBOTS_COMBATAREA=0 — выкл.
static InlineHook g_hookDesert;
static InlineHook g_hookReturn;
static void* g_trampDesert = nullptr;
static void* g_trampReturn = nullptr;
struct CombatAreaEv { unsigned long entity; unsigned long player; LONG returning; };
static CombatAreaEv g_caRing[64];
static volatile LONG g_caWrite = 0;
static LONG g_caRead = 0;  // читает только Lua (под Gate)
static volatile LONG g_caCount = 0;
static void CombatAreaPush(void* self, void* player, LONG returning) {
    const LONG i = InterlockedIncrement(&g_caWrite) - 1;
    CombatAreaEv& e = g_caRing[i & 63];
    e.entity = (unsigned long)(ULONGLONG)self;
    e.player = (unsigned long)(ULONGLONG)player;
    e.returning = returning;
    InterlockedIncrement(&g_caCount);
}
using CombatAreaFn = void(__fastcall*)(void*, void*, void*);
static void __fastcall DetourDesert(void* self, void* edx, void* player) {
    if (player != nullptr) CombatAreaPush(self, player, 0);
    ((CombatAreaFn)g_trampDesert)(self, edx, player);
}
static void __fastcall DetourReturn(void* self, void* edx, void* player) {
    if (player != nullptr) CombatAreaPush(self, player, 1);
    ((CombatAreaFn)g_trampReturn)(self, edx, player);
}
static void InstallCombatAreaHooksOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_COMBATAREA", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') {
        LOG_INFO("Bridge: CombatArea — перехват выключен (EVBOTS_COMBATAREA=0)");
        return;
    }
    unsigned char* base = NativeModuleBase();
    if (base == nullptr) return;
    unsigned char* tDesert = base + (0x00E4FD20UL - 0x00400000UL);
    unsigned char* tReturn = base + (0x00E4FE10UL - 0x00400000UL);
    // sub esp, 0x8C; push esi; push edi; mov edi, [esp+0x98]; mov esi, ecx — у обеих одинаково (сверено дизассемблером)
    static const unsigned char kPro[17] = {0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00, 0x56, 0x57, 0x8B, 0xBC, 0x24, 0x98, 0x00,
                                           0x00, 0x00, 0x8B, 0xF1};
    bool same = false;
    __try {
        same = ::memcmp(tDesert, kPro, sizeof(kPro)) == 0 && ::memcmp(tReturn, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("Bridge: CombatArea 0x00E4FD20/0x00E4FE10 — пролог не совпал, перехват НЕ ставлю");
        return;
    }
    const HookResult r1 = g_hookDesert.Install(tDesert, (void*)&DetourDesert, "VextPlayerManager:g_hookDesert");
    if (r1 != HookResult::Ok) {
        LOG_ERR("Bridge: CombatArea дезертир — установка не удалась: %s", ToString(r1));
        return;
    }
    g_trampDesert = g_hookDesert.Trampoline();
    const HookResult r2 = g_hookReturn.Install(tReturn, (void*)&DetourReturn, "VextPlayerManager:g_hookReturn");
    if (r2 != HookResult::Ok) {
        LOG_ERR("Bridge: CombatArea возврат — установка не удалась: %s (дезертир стоит)", ToString(r2));
        return;
    }
    g_trampReturn = g_hookReturn.Trampoline();
    LOG_INFO("Bridge: ★ CombatArea перехвачен (0x00E4FD20 дезертир, 0x00E4FE10 возврат) — события как VU");
}

/// NativeBridge.PollCombatArea() -> { {entity, player, returning}, ... } — накопленное с прошлого опроса (≤ 64).
int l_bridge_poll_combat_area(lua_State* L) {
    lua_newtable(L);
    const LONG w = InterlockedCompareExchange(&g_caWrite, 0, 0);
    if (w - g_caRead > 64) g_caRead = w - 64;  // переполнение — старые потеряны
    int k = 0;
    while (g_caRead < w) {
        const CombatAreaEv e = g_caRing[g_caRead & 63];
        ++g_caRead;
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)e.entity);
        lua_setfield(L, -2, "entity");
        lua_pushinteger(L, (lua_Integer)e.player);
        lua_setfield(L, -2, "player");
        lua_pushboolean(L, e.returning != 0);
        lua_setfield(L, -2, "returning");
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

// Сборка 126 (диагностика, поведение не меняет): раз в 20 с — нажатия огня и урон движка по солдатам.
// Огонь без урона — пули мимо (прицел/линия); урона нет и огня нет — мод не стреляет.
static void FireDamageDiagTick() {
    static ULONGLONG s_at = 0;
    static unsigned long s_dmg = 0;
    const ULONGLONG now = ::GetTickCount64();
    const unsigned long dmg = evbots::Counters().onDamage;
    if (s_at == 0) {
        s_at = now;
        s_dmg = dmg;
        return;
    }
    if (now - s_at < 20000) return;
    LOG_INFO("ДИАГ огонь: нажатий огня %ld, урон движка по солдатам %lu за %llu с | пули: хук VU %ld (без Call %ld), "
             "Lua занята → нативно %ld",
             (long)InterlockedExchange(&g_fireWrites, 0), dmg - s_dmg, (unsigned long long)((now - s_at) / 1000),
             (long)InterlockedExchange(&g_vuBulletLua, 0), (long)InterlockedExchange(&g_vuBulletNoCall, 0),
             (long)InterlockedExchange(&g_vuBulletBusy, 0));
    s_dmg = dmg;
    s_at = now;
}

// ★ Сборка 159: UTF-8 → UTF-16 для отправки в движок (исходящий текст — до 127 символов, как у VU).
static int Utf8ToUtf16(const char* s, wchar_t* out, int cap) {
    int o = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p != 0 && o < cap - 1) {
        const unsigned char c = *p++;
        unsigned long cp = c;
        int extra = 0;
        if (c >= 0xF0) {
            cp = c & 0x07UL;
            extra = 3;
        } else if (c >= 0xE0) {
            cp = c & 0x0FUL;
            extra = 2;
        } else if (c >= 0xC0) {
            cp = c & 0x1FUL;
            extra = 1;
        } else if (c >= 0x80) {
            cp = 0xFFFDUL;  // хвост без начала
        }
        for (int k = 0; k < extra; ++k) {
            if ((*p & 0xC0) != 0x80) {
                cp = 0xFFFDUL;
                break;
            }
            cp = (cp << 6) | (*p & 0x3FUL);
            ++p;
        }
        if (cp >= 0x10000UL) {
            if (o + 2 > cap - 1) break;
            cp -= 0x10000UL;
            out[o++] = (wchar_t)(0xD800UL + (cp >> 10));
            out[o++] = (wchar_t)(0xDC00UL + (cp & 0x3FFUL));
        } else {
            out[o++] = (wchar_t)cp;
        }
    }
    out[o] = L'\0';
    return o;
}

// ★ Сборка 159: ИСХОДЯЩИЙ ЧАТ (EVBOTS_CHAT_SEND=1). ChatManager:SendMessage кладёт запрос в очередь (VextChatManager.cpp);
// здесь — в хуке тика, то есть в потоке движка — повторяем admin.say (0x0113A2D8–0x0113A370, разбор в MODLOG «Сборка 159»):
// ChatMgr = [[0x02B3EE2C]+0x34]+0x3034; M = конструктор 0x011346B0(M, тип, lo, hi, текст, 0, 0) (thiscall, RET 0x18);
// handleMessage(ChatMgr, M, 0) (0x00E4DAF0, thiscall, RET 8). Событие Player:Chat от своей отправки не строим (g_chatSending).
// Маски (chat.md, 2.4): всем — тип 0, lo=hi=-1; команде — тип 1, lo=TeamId, hi=0; отряду — тип 2, lo=0x11+0x21*team+squad, hi=0.
constexpr unsigned long kChatMgrGlobalRva = 0x0273EE2CUL;  // VA 0x02B3EE2C
constexpr unsigned long kChatMgrPtrOffset = 0x34;
constexpr unsigned long kChatMgrObjOffset = 0x3034;
constexpr unsigned long kChatCtorRva = 0x00D346B0UL;       // VA 0x011346B0
constexpr unsigned long kHandleMessageRva = 0x00A4DAF0UL;  // VA 0x00E4DAF0

static void ChatSendDrain() {
    int kind = 0, team = 0, squad = 0;
    char text[256];
    unsigned char* base = nullptr;
    while (EVBotsChatSendPop(&kind, &team, &squad, text, (int)sizeof(text))) {
        if (base == nullptr) base = NativeModuleBase();
        if (base == nullptr) return;
        unsigned long chatMgr = 0;
        __try {
            const unsigned long p1 = *(const unsigned long*)(base + kChatMgrGlobalRva);
            const unsigned long p2 = (p1 != 0) ? *(const unsigned long*)(ULONG_PTR)(p1 + kChatMgrPtrOffset) : 0UL;
            chatMgr = (p2 != 0) ? *(const unsigned long*)(ULONG_PTR)(p2 + kChatMgrObjOffset) : 0UL;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            chatMgr = 0;
        }
        if (chatMgr == 0) {
            LOG_ERR("Bridge: ЧАТ: ChatManager не найден — «%s» не отправлено", text);
            continue;
        }
        wchar_t w[128] = {};
        Utf8ToUtf16(text, w, 128);
        unsigned int type = 0;
        unsigned long lo = 0xFFFFFFFFUL, hi = 0xFFFFFFFFUL;
        if (kind == 1) {
            type = 1;
            lo = (unsigned long)team;
            hi = 0;
        } else if (kind == 2) {
            type = 2;
            lo = 0x11UL + 0x21UL * (unsigned long)team + (unsigned long)squad;
            hi = 0;
        }
        alignas(8) unsigned char msg[0x130];
        using ChatCtorFn = void*(__fastcall*)(void*, void*, unsigned int, unsigned long, unsigned long, const wchar_t*, unsigned long, unsigned long);
        using ChatHandleFn = int(__fastcall*)(void*, void*, void*, void*);
        InterlockedExchange(&g_chatSending, 1);
        __try {
            ((ChatCtorFn)(base + kChatCtorRva))(msg, nullptr, type, lo, hi, w, 0UL, 0UL);
            ((ChatHandleFn)(base + kHandleMessageRva))((void*)(ULONG_PTR)chatMgr, nullptr, msg, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERR("Bridge: ЧАТ: исключение при отправке «%s»", text);
        }
        InterlockedExchange(&g_chatSending, 0);
        static volatile LONG s_logged = 0;
        if (InterlockedIncrement(&s_logged) <= 40) {
            LOG_INFO("Bridge: ЧАТ отправлен: тип %u, lo=%08lX hi=%08lX: «%s»", type, lo, hi, text);
        }
    }
}

// ★ Сборка 159: ПРОБНИК БРОШЕННОЙ ТЕХНИКИ (EVBOTS_VEH_PROBE=1; только чтение памяти). Раз в 2 с — строка «VEH probe» на
// каждую технику. Разбор — re\stubs-155\vehicles-enum.md, 2.3–2.4 и 5.3: таймер [v+0x250], флаг [v+0x2F4], компонент
// [v+0x10C] → d = [comp+0x14]: ApplyDamage 0x1ED, TUA 0x1C0, TUAID 0x1C4, RespawnRange 0x1D8, KeepAliveRadius 0x1E4;
// bl = [comp+0xD18]. Места — вектор [v+0xFC]..[v+0x100] (по 4 байта), место занято, если [[e+0x4C]+0x3C4] != 0.
// Рядом — наши боты (g_watchBot) и человек: солдаты в радиусе KeepAliveRadius (гипотеза H1) по XZ, как ReadSoldierXZ.
static bool ReadEntityXYZ(unsigned long ptr, float* x, float* y, float* z) {
    __try {
        const unsigned char* s = (const unsigned char*)(ULONGLONG)ptr;
        const unsigned char* tr = (const unsigned char*)(ULONGLONG)*(const unsigned long*)(s + 0x1C);
        if (((*(const unsigned long*)(s + 8) >> 18) & 1u) != 0) tr = tr + 0x10 + (unsigned long)tr[6] * 0x50;
        *x = *(const float*)(tr + 0x30);
        *y = *(const float*)(tr + 0x34);
        *z = *(const float*)(tr + 0x38);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ★ Сборка 185: player.corpse как в VU (веник: геттер 0x1098C900 = [[игрок+0x3CC]] − 4). Движок: ServerPlayer::getCorpse
// 0x00E209C0 — то же; слабая ссылка ставится, когда солдат перестаёт быть живым (0x00E36AEB). Состояние трупа — функции
// движка ServerSoldierEntity (имена — из записей assert 0x02C56C24…0x02C56C60): isManDown 0x00E012B0 ([с+0x3D8] → vf+0x20),
// isInteractiveManDown 0x00E012F0 (vf+0x24), isDead 0x00E01370 ([с+0x3F8] → vf+0x18). Компонент проверяем сами, чтобы
// не попасть в assert (int3) внутри функции. Позиция — как у читателей трупа в движке (0x00DCDB4E): ReadEntityXYZ.
static unsigned long ReadCorpsePtr(unsigned long player) {
    __try {
        const unsigned long weak = *(const unsigned long*)(ULONGLONG)(player + 0x3CC);
        if (weak == 0) return 0;
        const unsigned long obj = *(const unsigned long*)(ULONGLONG)weak;
        return obj != 0 ? obj - 4 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
typedef bool(__thiscall* SoldierBoolFn)(void* soldier);
// Сверка начала функций с exe (push esi; mov esi, ecx; cmp dword [esi+комп], 0) — один раз; не совпало — не зовём.
static bool SoldierStateFnsOk() {
    static volatile LONG s_ok = 0;  // 0 — не проверено, 1 — совпало, -1 — нет
    const LONG cur = InterlockedCompareExchange(&s_ok, 0, 0);
    if (cur != 0) return cur > 0;
    static const struct { unsigned long va; unsigned char comp; } kFns[] = {
        {0x00E01370UL, 0xF8}, {0x00E012B0UL, 0xD8}, {0x00E012F0UL, 0xD8}};
    bool ok = NativeModuleBase() != nullptr;
    for (const auto& f : kFns) {
        const unsigned char want[10] = {0x56, 0x8B, 0xF1, 0x83, 0xBE, f.comp, 0x03, 0x00, 0x00, 0x00};
        __try {
            if (ok && std::memcmp((const void*)(ULONGLONG)f.va, want, sizeof(want)) != 0) ok = false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    InterlockedExchange(&s_ok, ok ? 1 : -1);
    if (ok) LOG_INFO("Bridge: состояние трупа — функции движка isDead/isManDown сверены");
    else LOG_ERR("Bridge: функции движка isDead/isManDown не совпали с exe — corpse.isDead = true (реанимации не будет)");
    return ok;
}
// ★ Сборка 213 (прогон 212, 23.10 04:06:26 — смена уровня XP5_002 → XP5_004): PURE CALL с адресом возврата EVBots.dll+0x106C02 —
// это CallSoldierBool. Функции движка заканчиваются хвостовым `jmp [vtable компонента + слот]` (0x00E013A9: +0x18, 0x00E012E9:
// +0x20, 0x00E01329: +0x24); у трупа, который движок уже разрушает, vtable компонента — базовая, слот = `_purecall` MSVCR90.
// Слот проверяем сами: `_purecall` (или переход `jmp [импорт _purecall]`) — не зовём, состояние «не прочитано» (−1).
static unsigned long CrtPureCallAddr() {
    static volatile LONG s_addr = -1;
    LONG a = InterlockedCompareExchange(&s_addr, 0, 0);
    if (a == -1) {
        HMODULE crt = ::GetModuleHandleA("MSVCR90.dll");
        a = crt != nullptr ? (LONG)(ULONGLONG)::GetProcAddress(crt, "_purecall") : 0;
        InterlockedExchange(&s_addr, a);
    }
    return (unsigned long)a;
}
static bool IsPureCallTarget(unsigned long fn) {
    const unsigned long s_pure = CrtPureCallAddr();
    if (fn == 0) return true;
    if (s_pure != 0 && fn == s_pure) return true;
    __try {
        const unsigned char* p = (const unsigned char*)(ULONGLONG)fn;
        if (p[0] == 0xFF && p[1] == 0x25) {  // jmp dword ptr [imm32] — переходник на импорт
            const unsigned long iat = *(const unsigned long*)(p + 2);
            if (s_pure != 0 && *(const unsigned long*)(ULONGLONG)iat == s_pure) return true;
        }
        if (p[0] == 0xE9) {  // jmp rel32 — ILT
            const unsigned long dst = fn + 5 + *(const unsigned long*)(p + 1);
            if (s_pure != 0 && dst == s_pure) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return false;
}
static int CallSoldierBool(unsigned long soldier, unsigned long fnVa, unsigned long compOff, unsigned long slot) {
    if (!SoldierStateFnsOk()) return -1;
    __try {
        const unsigned long comp = *(const unsigned long*)(ULONGLONG)(soldier + compOff);
        if (comp == 0) return -1;
        const unsigned long vt = *(const unsigned long*)(ULONGLONG)comp;
        if (vt == 0 || IsPureCallTarget(*(const unsigned long*)(ULONGLONG)(vt + slot))) {
            static volatile LONG s_logged = 0;
            if (InterlockedIncrement(&s_logged) <= 10) {
                LOG_INFO("Bridge: ТРУП: компонент %08lX (vtable %08lX) уже разрушается — функцию движка %08lX не зову", comp, vt,
                         fnVa);
            }
            return -1;
        }
        return ((SoldierBoolFn)(ULONGLONG)fnVa)((void*)(ULONGLONG)soldier) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Функции движка зовём только в тике (поток движка): Lua моста — в своём потоке. Lua читает кэш состояния по указателю
// трупа; нет записи или старше 0,5 с — ставит запрос, тик его выполняет. Пока не посчитано — «мёртв» (не реанимировать).
struct CorpseStateRec { volatile LONG corpse; LONG dead, down, idown; ULONGLONG at; };
static CorpseStateRec g_corpseState[64];
static volatile LONG g_corpseReq[64];
static void CorpseStateCompute(CorpseStateRec& r, unsigned long c) {
    // ★ Живая проверка 18.10 (прогон 05:46): функции — методы подобъекта солдат+0x140 (vtable 0x025C38B0: +0xC isAlive
    // 0x00E01230, +0x10 isDead 0x00E01370, +0x14 isDying 0x00E01270; имена — записи assert 0x02C56BFC…0x02C56C60).
    // Движок зовёт их так же: `lea ecx, [солдат+0x140]` (0x00E36AC5). С this = солдат компонентов не было (−1).
    const unsigned long st = c + 0x140;
    r.dead = CallSoldierBool(st, 0x00E01370UL, 0x3F8, 0x18);
    r.down = CallSoldierBool(st, 0x00E012B0UL, 0x3D8, 0x20);
    r.idown = CallSoldierBool(st, 0x00E012F0UL, 0x3D8, 0x24);
    r.at = ::GetTickCount64();
    const bool isNew = (unsigned long)r.corpse != c;
    InterlockedExchange(&r.corpse, (LONG)c);
    static volatile LONG s_logged = 0;
    if (isNew && InterlockedIncrement(&s_logged) <= 60) {
        LOG_INFO("Bridge: ТРУП %08lX: isDead=%ld isManDown=%ld isInteractiveManDown=%ld (−1 — не прочитано)", c, r.dead,
                 r.down, r.idown);
    }
}
// ★ Сборка 185: перенос машины, как VU `vehicle.transform = LinearTransform` (мод: TeleportIfStuck, VehicleMovement.lua:173;
// до 185 — заглушка). Движок: с 190 — слот vtable +0x5C 0x00843570 (разбор в VehicleTeleportTick; в 185–189 ошибочно +0x6C
// 0x00D9BB40 — спавн сущности). Только в тике; перед вызовом сверка слотов. Lua ставит запрос (emVehTeleport), тик выполняет.
struct VehTpReq { volatile LONG state; unsigned long veh; float m[16]; };  // state: 0 свободен, 1 записывается, 2 ждёт
static VehTpReq g_vehTp[16];
bool EVBotsQueueVehicleTeleport(unsigned long veh, const float* m12) {
    if (veh == 0) return false;
    for (int i = 0; i < 12; ++i) {  // сборка 192: трансформ с NaN/inf движку не отдаём
        if (!NonFiniteGuard(m12[i], "перенос машины", i)) return false;
    }
    // ★ Сборка 186: перенос выключен по умолчанию. Прогон 16:03 (MP_001): setTransform машины (0x00D9BB40) дал исключение в
    // движке, затем FatalAssert «Attempting to add ghost of type VehicleEntityData twice» и Havok «!isAddedToWorld» —
    // сервер упал через 1 с. Машина добавлялась в мир второй раз. Без безопасного пути переноса — как до 185 (машина стоит).
    // ★ Сборка 190: снова включён по умолчанию — через настоящий setter (слот +0x5C, см. VehicleTeleportTick); =0 — выключить.
    static const bool s_on = [] {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VEH_TELEPORT", v, (DWORD)sizeof(v));
        return !(n == 1 && v[0] == '0');
    }();
    if (!s_on) {
        static volatile LONG s_logged = 0;
        if (InterlockedIncrement(&s_logged) <= 3) {
            LOG_INFO("Bridge: перенос машины %08lX пропущен (выключен, EVBOTS_VEH_TELEPORT=0)", veh);
        }
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        if (InterlockedCompareExchange(&g_vehTp[i].state, 1, 0) != 0) continue;
        g_vehTp[i].veh = veh;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 3; ++c) g_vehTp[i].m[r * 4 + c] = m12[r * 3 + c];
            g_vehTp[i].m[r * 4 + 3] = (r == 3) ? 1.0f : 0.0f;
        }
        InterlockedExchange(&g_vehTp[i].state, 2);
        return true;
    }
    return false;
}
// ★ Сборка 197: строки поворота (left/up/forward) текущего трансформа машины — там же, где позиция (ReadEntityXYZ, +0x30).
static bool ReadEntityBasis(unsigned long ptr, float out[12]) {
    __try {
        const unsigned char* s = (const unsigned char*)(ULONGLONG)ptr;
        const unsigned char* tr = (const unsigned char*)(ULONGLONG)*(const unsigned long*)(s + 0x1C);
        if (((*(const unsigned long*)(s + 8) >> 18) & 1u) != 0) tr = tr + 0x10 + (unsigned long)tr[6] * 0x50;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) out[r * 4 + c] = *(const float*)(tr + r * 0x10 + c * 4);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Оси поворота единичной длины (допуск 10 %): нулевые/вырожденные оси физика движка нормализует в NaN.
static bool BasisUnit(const float* m) {
    for (int r = 0; r < 3; ++r) {
        const float x = m[r * 4], y = m[r * 4 + 1], z = m[r * 4 + 2];
        const float l2 = x * x + y * y + z * z;
        if (!(l2 > 0.81f && l2 < 1.21f)) return false;
    }
    return true;
}

static void VehicleTeleportTick() {
    for (int i = 0; i < 16; ++i) {
        if (InterlockedCompareExchange(&g_vehTp[i].state, 0, 0) != 2) continue;
        const unsigned long veh = g_vehTp[i].veh;
        alignas(16) float m[16];
        for (int k = 0; k < 16; ++k) m[k] = g_vehTp[i].m[k];
        InterlockedExchange(&g_vehTp[i].state, 0);
        float bx = 0, by = 0, bz = 0, ax = 0, ay = 0, az = 0;
        ReadEntityXYZ(veh, &bx, &by, &bz);
        // ★ Сборка 197 (падение прогона 196, 06:05:04): мод переносит застрявшую машину с поворотом
        // `LookAtTransform(цель, следующая точка)` (VehicleMovement.lua:172). Точки совпали — наш Vec3:Normalize оставляет ноль,
        // все три оси нулевые; проверку «конечное число» они проходят, а физика (0x00665ED0 нормализует оси) делает NaN —
        // через 5 мс FatalAssert ServerWeaponComponent.cpp(313) «Unreasonable distance … 1.#QNAN». Поворот не единичный —
        // оставляем текущий поворот машины, переносим только позицию; не прочитать текущий — не переносим.
        if (!BasisUnit(m)) {
            float cur[12] = {};
            const bool okCur = ReadEntityBasis(veh, cur) && BasisUnit(cur);
            static volatile LONG s_badLogged = 0;
            if (InterlockedIncrement(&s_badLogged) <= 30) {
                LOG_WARN("Bridge: перенос машины %08lX: оси поворота мода не единичные (|left|²=%.3f |up|²=%.3f |fwd|²=%.3f) — %s",
                         veh, (double)(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), (double)(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]),
                         (double)(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]),
                         okCur ? "поворот оставлен текущий, переносится только позиция" : "текущий не прочитан — не переносим");
            }
            if (!okCur) continue;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) m[r * 4 + c] = cur[r * 4 + c];
                m[r * 4 + 3] = 0.0f;
            }
        }
        int res = 0;  // 1 — перенесена, -1 — слот не тот, -2 — исключение
        // ★ Сборка 190: настоящий setter, как VU `SpatialEntity.transform = …`. VU (veniceext, регистрация 0x10A8D0F0–0x10A8D1FC):
        // пара transform = getter 0x10A8CE60 (vtbl+0x50) / setter 0x10A8D260 (vtbl+0x54, один аргумент — трансформ). У серверного
        // класса машины (vtable 0x025BF8E8) тот же порядок со сдвигом +8: getter +0x58 0x00843530 (копия 64 байт из [+0x1C]),
        // setter +0x5C 0x00843570 → 0x0083CF80(t, 1) (имя «SpatialEntity::setTransform» — гипотеза): пишет хранилище [+0x1C],
        // затем vtbl+0xAC (у машины 0x00DEC890: физика [машина+0x144] → vtbl+0x5C; потом 0x0084D190 и 0x00D99DB0 — не разобраны).
        // До 190 звали +0x6C 0x00D9BB40 — это спавн сущности (второе добавление в мир, падение 186). Сверяются все звенья.
        // Физика наземной машины (живая память 19.10 00:33, M1A2/TOW2): vtable 0x02490A60, +0x5C 0x006A8C30 → 0x00665ED0
        // (нормализует оси хранилища; movaps — по хранилищу движка, не по m) → части [+0xC4] vtbl+0x18 (ворота 0x0065A220).
        // У другого класса физики (0x02490C38, +0x5C 0x006A4BF0 — assert тождественной матрицы) не переносим.
        // Риск (проверяющий 19.10): FatalAssert движка __try не ловит; при исключении хранилище уже новое, физика — старая.
        using SetTrFn = void(__fastcall*)(void*, void*, const float*);
        __try {
            const unsigned long vt = *(const unsigned long*)(ULONGLONG)veh;
            const unsigned long fn = *(const unsigned long*)(ULONGLONG)(vt + 0x5C);
            const unsigned long onTr = *(const unsigned long*)(ULONGLONG)(vt + 0xAC);
            const unsigned long phys = *(const unsigned long*)(ULONGLONG)(veh + 0x144);
            const unsigned long pvt = phys != 0 ? *(const unsigned long*)(ULONGLONG)phys : 0;
            const unsigned long pfn = pvt != 0 ? *(const unsigned long*)(ULONGLONG)(pvt + 0x5C) : 0;
            if (vt != 0x025BF8E8UL || fn != 0x00843570UL || onTr != 0x00DEC890UL || pvt != 0x02490A60UL || pfn != 0x006A8C30UL) {
                res = -1;
            } else {
                ((SetTrFn)(ULONGLONG)fn)((void*)(ULONGLONG)veh, nullptr, m);
                res = 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            res = -2;
        }
        ReadEntityXYZ(veh, &ax, &ay, &az);
        static volatile LONG s_logged = 0;
        if (InterlockedIncrement(&s_logged) <= 30) {
            LOG_INFO("Bridge: ПЕРЕНОС машины %08lX (как VU, setTransform): %s; было (%.0f, %.0f, %.0f) → цель (%.0f, %.0f, %.0f), стало (%.0f, %.0f, %.0f)",
                     veh, res == 1 ? "вызван (vtbl+0x5C)" : (res == -1 ? "класс машины/физики не тот (сверка vtable) — не зовём"
                                                                     : "исключение в движке (хранилище уже новое, физика — нет)"),
                     (double)bx, (double)by, (double)bz, (double)m[12], (double)m[13], (double)m[14], (double)ax, (double)ay, (double)az);
        }
    }
}

static void CorpseStateTick() {
    for (int i = 0; i < 64; ++i) {
        const unsigned long c = (unsigned long)InterlockedExchange(&g_corpseReq[i], 0);
        if (c == 0) continue;
        int slot = -1, oldest = 0;
        for (int k = 0; k < 64; ++k) {
            if ((unsigned long)g_corpseState[k].corpse == c) { slot = k; break; }
            if (g_corpseState[k].at < g_corpseState[oldest].at) oldest = k;
        }
        CorpseStateCompute(g_corpseState[slot >= 0 ? slot : oldest], c);
    }
}
static void CorpseStateGet(unsigned long c, int* dead, int* down, int* idown) {
    *dead = -1;
    *down = *idown = 0;
    const ULONGLONG now = ::GetTickCount64();
    bool fresh = false;
    for (int k = 0; k < 64; ++k) {
        if ((unsigned long)g_corpseState[k].corpse != c) continue;
        *dead = g_corpseState[k].dead;
        *down = g_corpseState[k].down;
        *idown = g_corpseState[k].idown;
        fresh = now - g_corpseState[k].at < 500ULL;
        break;
    }
    if (fresh) return;
    for (int i = 0; i < 64; ++i) {
        if ((unsigned long)g_corpseReq[i] == c) return;
    }
    for (int i = 0; i < 64; ++i) {
        if (InterlockedCompareExchange(&g_corpseReq[i], (LONG)c, 0) == 0) return;
    }
}

/// NativeBridge.Corpse(playerPtr) -> nil | soldierPtr, x, y, z, isDead, isManDown, isInteractiveManDown.
int l_bridge_corpse(lua_State* L) {
    const unsigned long player = (unsigned long)luaL_checkinteger(L, 1);
    if (player == 0 || NativeModuleBase() == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    const unsigned long c = ReadCorpsePtr(player);
    float x = 0.f, y = 0.f, z = 0.f;
    if (c == 0 || !ReadEntityXYZ(c, &x, &y, &z)) {
        lua_pushnil(L);
        return 1;
    }
    int dead = -1, down = 0, idown = 0;
    CorpseStateGet(c, &dead, &down, &idown);
    lua_pushinteger(L, (lua_Integer)c);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, z);
    // Не прочитано (-1) — считаем «мёртв»: мод не станет реанимировать то, чего движок не подтвердил.
    lua_pushboolean(L, dead != 0);
    lua_pushboolean(L, down == 1);
    lua_pushboolean(L, idown == 1);
    return 7;
}

static void VehicleProbeTick() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VEH_PROBE", v, (DWORD)sizeof(v));
        s_on = (n == 1 && v[0] == '1') ? 1 : 0;
    }
    if (s_on == 0) return;
    static ULONGLONG s_next = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now < s_next) return;
    s_next = now + 2000ULL;
    unsigned long veh[32];
    const int nv = ::evbots::EVBotsListEntitiesByName("ServerVehicleEntity", veh, 32);
    const unsigned long human = ::evbots::EVBotsEngineRealPlayer();
    LOG_INFO("VEH probe: техник %d, человек %08lX, ботов в наблюдении %ld", nv, human, (long)g_watchCount);
    for (int i = 0; i < nv; ++i) {
        const unsigned long v0 = veh[i];
        unsigned long flags = 0, comp = 0, d = 0, begin = 0, end = 0;
        float timer = 0.0f, tua = 0.0f, tuaId = 0.0f, respawn = 0.0f, keep = 0.0f;
        unsigned int abandon = 0, bl = 0, applyDmg = 0;
        __try {
            const unsigned char* s = (const unsigned char*)(ULONGLONG)v0;
            flags = *(const unsigned long*)(s + 8);
            timer = *(const float*)(s + 0x250);
            abandon = *(const unsigned char*)(s + 0x2F4);
            begin = *(const unsigned long*)(s + 0xFC);
            end = *(const unsigned long*)(s + 0x100);
            comp = *(const unsigned long*)(s + 0x10C);
            if (comp != 0) {
                const unsigned char* c = (const unsigned char*)(ULONGLONG)comp;
                bl = *(const unsigned char*)(c + 0xD18);
                d = *(const unsigned long*)(c + 0x14);
            }
            if (d != 0) {
                const unsigned char* dd = (const unsigned char*)(ULONGLONG)d;
                applyDmg = *(const unsigned char*)(dd + 0x1ED);
                tua = *(const float*)(dd + 0x1C0);
                tuaId = *(const float*)(dd + 0x1C4);
                respawn = *(const float*)(dd + 0x1D8);
                keep = *(const float*)(dd + 0x1E4);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERR("VEH probe: %08lX — поля не прочитаны", v0);
        }
        int seats = 0, occupied = 0;
        if (end >= begin && end - begin <= 4UL * 16UL) {
            seats = (int)((end - begin) / 4UL);
            for (int k = 0; k < seats; ++k) {
                unsigned long pl = 0;
                __try {
                    const unsigned long e = *(const unsigned long*)(ULONGLONG)(begin + 4UL * (unsigned long)k);
                    if (e != 0) pl = *(const unsigned long*)(ULONGLONG)(e + 0x4C);
                    if (pl != 0 && *(const unsigned long*)(ULONGLONG)(pl + kOffPlayerSoldier) != 0) ++occupied;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    pl = 0;
                }
            }
        }
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        ReadEntityXYZ(v0, &vx, &vy, &vz);
        int nearCount = 0;
        float nearest2 = -1.0f;
        const float keep2 = keep * keep;
        for (LONG b = 0; b <= g_watchCount && b < 64; ++b) {
            const unsigned long pl = (b < g_watchCount) ? g_watchBot[b] : human;
            if (pl == 0) continue;
            float x = 0.0f, z = 0.0f;
            if (!ReadSoldierXZ(pl, &x, &z)) continue;
            const float dx = x - vx, dz = z - vz;
            const float d2 = dx * dx + dz * dz;
            if (nearest2 < 0.0f || d2 < nearest2) nearest2 = d2;
            if (keep > 0.0f && d2 <= keep2) ++nearCount;
        }
        LOG_INFO("VEH probe: %08lX флаги=%08lX таймер=%.1f abandon=%u | ApplyDamage=%u TUA=%.1f TUAID=%.1f Respawn=%.1f Keep=%.1f | bl=%u | pos=(%.1f, %.1f, %.1f) | мест=%d занято=%d | солдат в радиусе=%d ближ2=%.0f",
                 v0, flags, timer, abandon, applyDmg, tua, tuaId, respawn, keep, bl, vx, vy, vz, seats, occupied, nearCount, nearest2);
    }
}

// ★ Сборка 174: для журнала падений (VEH): чей это указатель — игрок бота или его солдат (текущий или прежний = труп),
// с именем и группой VU-создания (А = ИИ 0 при EVBOTS_BOT_AI=ab). Только наши массивы, память движка не читаем (вызов из VEH).
// Сборка 201: указатель — бот моста (создан мостом или «забыт» им). Для перехватчика spawnSoldier: спавн бота по событию Spawn идёт
// из обновления точки (не «наш вызов»), и без этой проверки он принял бы бота за человека и перезаписал эталон спавна.
bool EVBotsIsBridgeBot(unsigned long p) {
    if (p == 0) return false;
    for (LONG i = 0; i < g_createdCount && i < 64; ++i) if (g_createdBots[i].ptr == p) return true;
    return IsForgottenEngineBot(p);
}

bool EVBotsBridgeDescribePtr(unsigned long v, char* out, unsigned cap) {
    if (v == 0 || out == nullptr || cap < 16) return false;
    auto nameOf = [](unsigned long p) -> const char* {
        for (LONG i = 0; i < g_ptrNameCount && i < 64; ++i) {
            if (g_ptrName[i].ptr == p) return g_ptrName[i].name;
        }
        return "?";
    };
    const ULONGLONG now = ::GetTickCount64();
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
        if (g_watchBot[i] == v) {
            ::_snprintf_s(out, cap, _TRUNCATE, "игрок бота %s (группа %c)", nameOf(v), VuGroupOf(v));
            return true;
        }
    }
    for (LONG k = 0; k < 128; ++k) {
        const SoldierHist& h = g_solHist[k];
        if (h.soldier != v || h.soldier == 0) continue;
        if (h.diedAt == 0) {
            ::_snprintf_s(out, cap, _TRUNCATE, "солдат бота %s (живой, %llu с; группа %c)", nameOf(h.player),
                          (unsigned long long)((now - h.bornAt) / 1000), VuGroupOf(h.player));
        } else {
            ::_snprintf_s(out, cap, _TRUNCATE, "солдат бота %s (ТРУП: смерть %llu мс назад; группа %c)", nameOf(h.player),
                          (unsigned long long)(now - h.diedAt), VuGroupOf(h.player));
        }
        return true;
    }
    return false;
}

void EVBotsBridgeRetryPending() {
    HookScope hookScope;  // контекст хука тика: спавн здесь работает (см. g_respawnQueue)
    LevelUnloadTick();    // сборка 211: выгрузка уровня (Server+0x74 = 0) — раньше любого спавна в этом тике
    ::evbots::EVBotsRconDrainReplies();  // сборка 209: ответы на RCON-команды модов (главный поток)
    ApplyAiDeathPatches();  // сборка 124: один раз (дальше — возврат по флагу)
    ApplyLevelChecksumBypassOnce();  // 12.11.2026: локальный Royale/стенд, EVBOTS_SKIP_LEVEL_CHECKSUM=1
    RunLosQueue();        // сборка 121: лучи видимости для целей мода
    CorpseStateTick();    // сборка 185: состояние трупов для player.corpse (функции движка — только здесь)
    VehicleTeleportTick();  // сборка 185: перенос машин (vehicle.transform = …), как VU
    FireDamageDiagTick(); // сборка 126: диагностика огня и урона
    InstallBotDamageHookOnce();  // сборка 131: урон от пуль ботов
    InstallCombatAreaHooksOnce();  // сборка 185: CombatArea:PlayerDeserting/Returning
    InstallRconHookOnce();         // сборка 186: Server:RoundOver из движка (RCON onMessage)
    InstallScoringHookOnce();      // сборка 186: ScoringSystem:StatEvent — все события (executeEvent)
    InstallDownloadProcessedFilterOnce();  // сборка 207: телеметрия — DownloadProcessed ботов (ассерт движка)
    InstallFriendZoneGuardOnce();  // сборка 208: смена уровня — FriendZone без таблиц (падение сервера)
    InstallCharacterGroundOnce();  // 262: no terrain -> collision height at the current character
    ::evbots::EVBotsRconInstallDispatchHookOnce();  // сборка 209: команды модов через RCON сервера, как VU
    InstallOrderHookOnce();        // сборка 193: приказы колеса BF3 → команды меню мода
    InterlockedExchange(&g_engineMainTid, (LONG)::GetCurrentThreadId());  // сборка 197: для диагностики pure call
    InstallPureCallHookOnce();     // сборка 197: «Pure function call» движка — поток и стек в лог
    InstallSoldierDmgProbeOnce();  // сборка 198: ServerSoldierEntity::onDamage — разметка DamageInfo (диагностика)
    if (StatOldOn() || InterlockedCompareExchange(&g_scoringOn, 0, 0) == 0) {
        InstallStatEventHookOnce();    // запасные источники, если executeEvent не встал
        InstallStatTriggerHookOnce();
    }
    ::evbots::EVBotsInstallEntityFactoryHookOnce();  // сборка 186: EntityFactory:Create (ракеты)
    AmmoTick();           // сборка 129: патроны ботов (пополнение магазина) и поза хитбоксов
    PoseTick();           // сборка 132: поза солдата — вызов движка CharacterPhysicsEntity::setPose (очередь из Lua)
    PoseDiagTick();       // сборка 132: диагностика позы раз в 20 с
    XformStackTick();     // сборка 236: целостность списка трансформа солдат (падение 00E0B030)
    // Сборка 236: номер тика. Отсрочка посадки срабатывает в VehicleTick(bodiedNow) ниже (после RetryBotPositions этого же
    // тика); общий VehicleTick() идёт до постановок позиций, его посадки после телепорта прошлого тика не откладываются.
    InterlockedIncrement(&g_bridgeTickNo);
    VehicleTick();        // сборка 178: посадка/выход из техники (очередь из Lua)
    HitTick();            // сборка 133: попадания по солдатам (падение HP ботов), диагностика
    HumanHpTick();        // сборка 153: HP человека для событий Soldier:Damage
    HumanLevelDiagTick(); // сборка 171: момент «человек в уровне» и его команда (задача «~22 с»)
    HumanJoinDiagTick();  // сборка 211: вход людей глазами движка (друг: бесконечное подключение у некоторых)
    ScoreDiagTick(g_watchBot, g_watchCount);  // сборка 171: счёт ботов по группам VU-создания
    ScoreChangeTick(g_watchBot, g_watchCount);  // сборка 175: каждое изменение счёта бота
    // Убийства, которые мод заказал из Lua (soldier:Kill), — здесь, в контексте движка.
    // ★ Сборка 170: раньше очереди удалений. Kill в Lua теперь синхронный, как в VU (soldier = nil сразу), и мод в том же
    // кадре удаляет убитого (KillAll → DestroyDisabledBots); удаление первым оставило бы в очереди убийств указатель на
    // удалённого игрока. Удаление мёртвого — проверенный путь (прогон 169: 4 бота).
    if (g_killCount > 0) {
        const LONG kn = g_killCount < kKillQueueCap ? g_killCount : kKillQueueCap;
        g_killCount = 0;
        for (LONG i = 0; i < kn; ++i) KillSoldierNow(g_killQueue[i]);
    }
    // Удаление ботов, которых мод убрал (Bot:Destroy). Сначала забываем указатель во всех наших списках — иначе
    // чтение освобождённой памяти и повторный спавн; потом настоящий deletePlayer движка.
    // Сборка 145: та же очистка списков и для «забытых» (бот без тела, убранный модом) — но без deletePlayer.
    if (g_deleteCount > 0 || g_forgetCount > 0) {
        unsigned long items[kDeleteQueueCap + 32];
        bool doDelete[kDeleteQueueCap + 32];
        LONG n = 0;
        for (LONG i = 0; i < g_deleteCount && i < kDeleteQueueCap; ++i) { items[n] = g_deleteQueue[i]; doDelete[n++] = true; }
        for (LONG i = 0; i < g_forgetCount && i < 32; ++i) {
            // Сборка 187: «забыть» и удаление в одном тике — только удаление (иначе «забыть» читало бы удалённого игрока).
            bool alsoDeleted = false;
            for (LONG k = 0; k < n; ++k) {
                if (doDelete[k] && items[k] == g_forgetQueue[i]) { alsoDeleted = true; break; }
            }
            if (alsoDeleted) continue;
            items[n] = g_forgetQueue[i];
            doDelete[n++] = false;
        }
        g_deleteCount = 0;
        g_forgetCount = 0;
        unsigned char* dbase = NativeModuleBase();
        void* dmgr = NativeManager();
        for (LONG q = 0; q < n; ++q) {
            const unsigned long dp = items[q];
            // ★ Сборка 153 (прогон 153, 19:49:29: Jason/Theodore): «забыть» пришло из Lua посреди тика, а посадка в том же
            // тике уже дала тело. Мод считает бота мёртвым и сажает снова — второе тело поверх первого («призрак»: стоит,
            // урона не получает), а «забыть» сняло бота с учёта очков (в табе 0/0). Если тело уже есть — не забываем,
            // а убиваем по-настоящему: движок и мод снова согласны (мёртв), бот остаётся под наблюдением.
            if (!doDelete[q]) {
                unsigned long bodyNow = 0;
                __try {
                    bodyNow = *(const unsigned long*)((const unsigned char*)(ULONGLONG)dp + kOffPlayerSoldier);
                } __except (EXCEPTION_EXECUTE_HANDLER) { bodyNow = 0; }
                if (bodyNow != 0) {
                    const bool killed = KillSoldierNow(dp);
                    LOG_INFO("Bridge: «забыть» бота %p отменено — тело уже есть (%08lX), убит в движке: %s (мод уже считает его мёртвым)",
                             (void*)(ULONGLONG)dp, bodyNow, killed ? "да" : "НЕТ");
                    continue;
                }
            }
            auto drop = [dp](unsigned long* arr, LONG* cnt) {
                for (LONG i = 0; i < *cnt;) {
                    if (arr[i] == dp) { arr[i] = arr[*cnt - 1]; --*cnt; } else { ++i; }
                }
            };
            for (LONG i = 0; i < g_respawnCount;) {  // сборка 173: вместе с временем ожидания
                if (g_respawnQueue[i] == dp) {
                    g_respawnQueue[i] = g_respawnQueue[g_respawnCount - 1];
                    g_respawnSince[i] = g_respawnSince[g_respawnCount - 1];
                    g_respawnTry[i] = g_respawnTry[g_respawnCount - 1];
                    g_respawnSince[g_respawnCount - 1] = 0;
                    --g_respawnCount;
                } else { ++i; }
            }
            for (LONG i = 0; i < g_deferredSpawnCount;) {  // сборка 174: вместе с временем постановки
                if (g_deferredSpawn[i] == dp) {
                    g_deferredSpawn[i] = g_deferredSpawn[g_deferredSpawnCount - 1];
                    g_deferredSince[i] = g_deferredSince[g_deferredSpawnCount - 1];
                    --g_deferredSpawnCount;
                } else { ++i; }
            }
            drop(g_deadQueue, &g_deadCount);
            drop(g_pointsDone, &g_pointsDoneCount);
            for (LONG i = 0; i < g_watchCount;) {
                if (g_watchBot[i] == dp) {
                    g_watchBot[i] = g_watchBot[g_watchCount - 1];
                    g_watchHadSoldier[i] = g_watchHadSoldier[g_watchCount - 1];
                    --g_watchCount;
                } else { ++i; }
            }
            for (LONG i = 0; i < g_botPosCount;) {
                if (g_botPos[i].ptr == dp) { g_botPos[i] = g_botPos[g_botPosCount - 1]; --g_botPosCount; } else { ++i; }
            }
            for (LONG i = 0; i < g_botSquadCount;) {
                if (g_botSquad[i].ptr == dp) { g_botSquad[i] = g_botSquad[g_botSquadCount - 1]; --g_botSquadCount; } else { ++i; }
            }
            for (LONG i = 0; i < g_posPendingCount;) {
                if (g_posPending[i].ptr == dp) { g_posPending[i] = g_posPending[g_posPendingCount - 1]; --g_posPendingCount; } else { ++i; }
            }
            for (LONG i = 0; i < g_createdCount;) {
                if (g_createdBots[i].ptr == dp) { g_createdBots[i] = g_createdBots[g_createdCount - 1]; --g_createdCount; } else { ++i; }
            }
            for (LONG i = 0; i < g_spawnCheckCount;) {  // сборка 147
                if (g_spawnCheck[i].ptr == dp) { g_spawnCheck[i] = g_spawnCheck[g_spawnCheckCount - 1]; --g_spawnCheckCount; } else { ++i; }
            }
            for (LONG i = 0; i < g_choiceWaitCount;) {  // сборка 146
                if (g_choiceWait[i].ptr == dp) { g_choiceWait[i] = g_choiceWait[g_choiceWaitCount - 1]; --g_choiceWaitCount; } else { ++i; }
            }
            ForgetBotChoice(dp);  // сборка 236
            if (!doDelete[q]) {
                if (!IsForgottenEngineBot(dp)) {
                    if (g_forgottenCount < 64) g_forgottenBots[g_forgottenCount++] = dp;
                    else LOG_WARN("Bridge: список забытых ботов полон (64) — %p из движка по DeletePlayer не удалится", (void*)(ULONGLONG)dp);
                }
                LOG_INFO("Bridge: бот без тела %p забыт мостом (спавн/отряд/позиция сняты), удаление из движка — по DeletePlayer мода", (void*)(ULONGLONG)dp);
                continue;
            }
            DropForgottenEngineBot(dp);
            if (dbase == nullptr || dmgr == nullptr) continue;
            using DelFn = void(__fastcall*)(void*, void*, void*);
            using LeftGameFn = void(__fastcall*)(void*, void*);
            using LeaveSquadFn = void(__cdecl*)(void*, unsigned int);
            // ★ Сборка 234: порядок как у движка при уходе человека (ServerConnection 0x00E480B0: сначала «покинул игру»
            // 0x00E378E0, потом остальное). Факт (дизассемблер, хвост 0x00E36DE0): «покинул игру» высаживает игрока из
            // техники — 0x00E36FE1 `call 0x00E35AC0` («ServerPlayer_exitControllable», по [игрок+0x3D8]; только если
            // есть запись входа — 0x00E36F95 `call 0x00E190E0`), — и потом (0x00E36FED, при isAI == 0) уничтожает
            // солдата 0x5E1C60 (как VU Entity:Destroy). Раньше мост снимал бота с отряда ДО этого — живым. Связь с
            // падением сервера 234-в НЕ подтверждена (leaveSquad 0x118D590 в путь падения 0x0118E5C0 не ведёт — наоборот,
            // 0x0118E5C0 зовёт его; падение — через 3 мин после последнего удаления). Теперь: isAI = 0 → «покинул игру»
            // → leaveSquad → deletePlayer. «Покинул игру» команду/отряд не меняет (только читает +0x31C).
            // EVBOTS_BOT_DESTROY=0 — прежний порядок (leaveSquad, «покинул игру» с isAI 1, deletePlayer).
            static int s_aiFirst = -1;
            if (s_aiFirst < 0) {
                const char* off = std::getenv("EVBOTS_BOT_DESTROY");
                s_aiFirst = (off != nullptr && off[0] == '0') ? 0 : 1;
                LOG_INFO("Bridge: удаление бота как уход человека — isAI=0, «покинул игру» (высадка и уничтожение солдата "
                         "движком), потом отряд и deletePlayer: %s",
                         s_aiFirst == 1 ? "включено" : "ВЫКЛЮЧЕНО (EVBOTS_BOT_DESTROY=0), прежний порядок");
            }
            const int sqBefore = EngineSquadOf(dp);
            if (s_aiFirst == 1) {
                bool leftOk = true;
                __try {
                    const unsigned char* pb = (const unsigned char*)(ULONGLONG)dp;
                    const unsigned long soldierRef = *(const unsigned long*)(pb + 0x3C8);
                    const unsigned long attached = *(const unsigned long*)(pb + kOffPlayerAttached);
                    if (*(pb + kOffPlayerInGame) != 0) {
                        *(unsigned int*)((unsigned char*)(ULONGLONG)dp + kOffPlayerIsAi) = 0u;
                        ((LeftGameFn)(dbase + kPlayerLeftGameRva))((void*)(ULONGLONG)dp, nullptr);
                        LOG_INFO("Bridge: бот %p покинул игру (0xE378E0) перед удалением; техника [+0x3D8] была %p, теперь %p; "
                                 "солдат [+0x3C8] был %p, теперь %p", (void*)(ULONGLONG)dp, (void*)(ULONGLONG)attached,
                                 (void*)(ULONGLONG)(*(const unsigned long*)(pb + kOffPlayerAttached)),
                                 (void*)(ULONGLONG)soldierRef, (void*)(ULONGLONG)(*(const unsigned long*)(pb + 0x3C8)));
                    } else if (soldierRef != 0) {
                        LOG_WARN("Bridge: бот %p не «в игре», но солдат [+0x3C8] = %p — «покинул игру» не зову, солдат не уничтожен",
                                 (void*)(ULONGLONG)dp, (void*)(ULONGLONG)soldierRef);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    leftOk = false;
                }
                if (!leftOk) {
                    LOG_ERR("Bridge: «покинул игру»(бот %p) — исключение в движке, удаление отменено: бот ОСТАЛСЯ в движке",
                            (void*)(ULONGLONG)dp);
                    continue;
                }
            }
            // Проверка команды: leaveSquad берёт команду из +0x31C; число команд — как в addToTeam движка
            // ((PTR 0x0285BB3C − PTR 0x0285BB38) >> 3). Команда вне списка (призрак SQDM на карте CQ) — FatalAssert
            // «Invalid team 4. Num of teams: 3»; тогда отряда этой команды в движке уже нет — удаляем без leaveSquad.
            unsigned int teamNow = 0, teamsNum = 0;
            __try {
                teamNow = *(const unsigned int*)((const unsigned char*)(ULONGLONG)dp + kOffPlayerTeam);
                teamsNum = (*(const unsigned long*)(dbase + kTeamsEndRva) - *(const unsigned long*)(dbase + kTeamsBeginRva)) >> 3;
            } __except (EXCEPTION_EXECUTE_HANDLER) { teamNow = 0; teamsNum = 0; }
            if (sqBefore > 0 && (teamNow == 0 || teamNow >= teamsNum)) {
                LOG_WARN("Bridge: бот %p в отряде %d, но команда %u вне списка (команд %u) — leaveSquad не зову, только удаление",
                         (void*)(ULONGLONG)dp, sqBefore, teamNow, teamsNum);
            } else if (sqBefore > 0) {
                __try {
                    ((LeaveSquadFn)(dbase + kLeaveSquadRva))((void*)(ULONGLONG)dp, 0u);
                    LOG_INFO("Bridge: бот %p вышел из отряда %d (leaveSquad 0x118D590) перед удалением, отряд теперь %d",
                             (void*)(ULONGLONG)dp, sqBefore, EngineSquadOf(dp));
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    LOG_ERR("Bridge: leaveSquad(бот %p) — исключение в движке, удаление отменено: бот ОСТАЛСЯ в движке (призрак)",
                            (void*)(ULONGLONG)dp);
                    continue;
                }
            }
            __try {
                // ★ Сборка 175: как при уходе человека (ServerConnection 0x00E480CD) и смерти ИИ (0x00E114CC) — сначала
                // ServerPlayer «покинул игру» 0x00E378E0 (thiscall: проверка и сброс +0x3EC, хвост 0x00E36DE0), потом
                // deletePlayer. Прогон 174: assert «Deleting ServerPlayer who is still inGame» (ServerPlayer.cpp:209,
                // проверка 0x00E34BAA: +0x3EC != 0 и +0x318 == 0) — бот с isAI 0 удалялся «в игре».
                // Прежний порядок (EVBOTS_BOT_DESTROY=0): «покинул игру» после отряда, isAI = 0 после него.
                if (s_aiFirst != 1 &&
                    *(const unsigned char*)((const unsigned char*)(ULONGLONG)dp + kOffPlayerInGame) != 0) {
                    ((LeftGameFn)(dbase + kPlayerLeftGameRva))((void*)(ULONGLONG)dp, nullptr);
                    LOG_INFO("Bridge: бот %p покинул игру (0xE378E0) перед удалением", (void*)(ULONGLONG)dp);
                }
                // Сборка 124: перед удалением isAI = 0, иначе PointSystem не уберёт запись бота (разбор re\death).
                *(unsigned int*)((unsigned char*)(ULONGLONG)dp + kOffPlayerIsAi) = 0u;
                ((DelFn)(dbase + kNativeDeletePlayerRva))(dmgr, nullptr, (void*)(ULONGLONG)dp);
                ForgetOwnInput(dp);  // сборка 255: место в таблице ввода — следующему боту
                ForgetDeletedBot(dp);  // сборка 256: оружие мода, поза, точки, группы
                LOG_INFO("Bridge: бот %p удалён в движке (deletePlayer)", (void*)(ULONGLONG)dp);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LOG_ERR("Bridge: deletePlayer(бот %p) — исключение в движке", (void*)(ULONGLONG)dp);
            }
        }
    }
    // Поздние боты: создаём здесь, в потоке движка, по запросу из Lua (см. RunLateCreateInHook).
    InterlockedExchange(&g_hookAliveSec, (LONG)(::GetTickCount64() / 1000ULL));
    {
        const LONG reqId = InterlockedCompareExchange(&g_lateReqId, 0, 0);
        if (reqId != 0 && InterlockedCompareExchange(&g_lateReqId, 0, reqId) == reqId) {  // хук забирает запрос атомарно
            g_lateResult = LateCreateEngineImpl(g_lateName, g_lateTeam);
            InterlockedExchange(&g_lateDoneId, reqId);
        }
    }
    ChoiceWaitTick();  // сборка 146: боты, ждавшие выбора мода перед первым телом
    ChatSendDrain();  // сборка 159: исходящий чат (EVBOTS_CHAT_SEND), в потоке движка
    VehicleProbeTick();  // сборка 159: пробник брошенной техники (EVBOTS_VEH_PROBE=1)
    SpawnCheckTick();  // сборка 147: где бот стоит через 2 с после постановки
    // Очередь респаунов из Lua: спавним внутри хука тика.
    unsigned long keptQ[kRespawnCapacity];
    ULONGLONG keptT[kRespawnCapacity], keptTry[kRespawnCapacity];
    LONG keptRespawn = 0;
    unsigned long bodiedNow[kRespawnCapacity];  // получили тело в этом тике
    LONG bodiedCount = 0;
    const ULONGLONG nowR = ::GetTickCount64();
    for (LONG i = 0; i < g_respawnCount && i < kRespawnCapacity; ++i) {
        const unsigned long rp = g_respawnQueue[i];
        unsigned int rteam = 0;
        __try {
            rteam = *(const unsigned int*)((const unsigned char*)(ULONGLONG)rp + kOffPlayerTeam);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rteam = 0;
        }
        if (g_respawnSince[i] == 0 || nowR - g_respawnTry[i] >= 1000ULL) {
            g_respawnTry[i] = nowR;
            unsigned long rsp = ::evbots::EVBotsEngineSpawnPointForTeam(rteam);
            // ★ Сборка 195: запасная точка — только своей команды (было EVBotsEngineSpawnPointBest — HQ человека любому боту:
            // прогон 194, BOT_Lew т2 на HQ США, смерть за границей зоны).
            if (rsp == 0) rsp = ::evbots::EVBotsEngineSpawnPointBestForTeam(rteam);
            // Сборка 185: точки сейчас не нашлось — последняя удачная точка этой команды (ночной прогон MP_007, 12:57:
            // BOT_Cooper без позиции мода и без точки 51 с стоял в очереди без тела, без единой строки в логе).
            if (rteam <= 4 && InterlockedCompareExchange(&g_levelGate, 0, 0) == 0) {
                if (rsp != 0) InterlockedExchange(&g_lastRespawnPoint[rteam], static_cast<LONG>(rsp));
                else rsp = static_cast<unsigned long>(InterlockedCompareExchange(&g_lastRespawnPoint[rteam], 0, 0));
            }
            NativeSpawnOne(rp, rsp);  // сборка 185: без точки — посадка как VU (на позиции мода), если позиция есть
        }
        // ★ Сборка 173: точек ещё нет (скан нового уровня) или точка негодная — спавн остаётся в очереди, повтор на
        // следующих тиках (до 30 с), а не теряется: раньше мод считал бота посаженным, а тела не было.
        unsigned long solNow = 0;
        __try {
            solNow = *(const unsigned long*)((const unsigned char*)(ULONGLONG)rp + kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) { solNow = 0; }
        if (solNow != 0 && bodiedCount < kRespawnCapacity) bodiedNow[bodiedCount++] = rp;
        if (solNow == 0 && keptRespawn < kRespawnCapacity) {
            if (g_respawnSince[i] == 0) g_respawnSince[i] = nowR;
            // ★ Сборка 211: пока ворота закрыты (конец раунда → загрузка следующей карты), 30 с не идут: спавн ждёт Level:Loaded.
            // Прогон 22.10 15:23–15:24: 32 «не удался за 30 с» — все в окне между RoundOver TDM и загрузкой XP3_Shield.
            if (InterlockedCompareExchange(&g_levelGate, 0, 0) != 0) g_respawnSince[i] = nowR;
            if (nowR - g_respawnSince[i] < 30000ULL) {
                keptQ[keptRespawn] = rp;
                keptTry[keptRespawn] = g_respawnTry[i];
                keptT[keptRespawn++] = g_respawnSince[i];
            } else {
                // Сборка 252: друг 29.10 21:02 (XP4_FD, все флаги у врага): 10 таких строк без причины — пишем, что было у бота.
                float dx = 0.f, dy = 0.f, dz = 0.f;
                const bool dPos = BotPosOf(rp, &dx, &dy, &dz);
                const unsigned long dForced = ForcedPointOf(rp);
                unsigned char* dBase = NativeModuleBase();
                LOG_ERR("Bridge: спавн %p не удался за 30 с (нет годной точки) — снят; позиция мода %s (%.0f, %.0f, %.0f), точка "
                        "мода %08lX (точка спавна %s, техника %08lX), запасная точка команды %u = %08lX",
                        (void*)(ULONGLONG)rp, dPos ? "есть" : "нет", (double)dx, (double)dy, (double)dz, dForced,
                        (dForced != 0 && dBase != nullptr && IsCharSpawnPoint(dBase, dForced)) ? "да" : "нет",
                        (dForced != 0 && dBase != nullptr) ? CharSpawnVehicleSpawner(dBase, dForced) : 0UL, rteam,
                        ::evbots::EVBotsEngineSpawnPointForTeam(rteam));
            }
        }
    }
    RetryBotPositions();
    for (LONG i = 0; i < keptRespawn; ++i) { g_respawnQueue[i] = keptQ[i]; g_respawnSince[i] = keptT[i]; g_respawnTry[i] = keptTry[i]; }
    for (LONG i = keptRespawn; i < kRespawnCapacity; ++i) { g_respawnSince[i] = 0; g_respawnTry[i] = 0; }
    g_respawnCount = keptRespawn;
    // ★ Сборка 196: у VU SpawnSoldierAt и EnterVehicle синхронны — fun-bots зовёт их подряд, бот сразу в машине. Раньше
    // VehicleTick (выше) шёл до очереди спавна, и тело кадр стояло у машины («телепорт в тачку», пользователь 19.10).
    // Теперь посадка тех, кто получил тело в этом тике, — сразу после спавна, в том же тике.
    if (bodiedCount > 0 && g_vehCount > 0) VehicleTick(bodiedNow, bodiedCount);
    // (Сборка 168: перевод ботов в отряд человека, EVBOTS_SQUAD_FOLLOW — выключен с 78, не как VU, — удалён.)
    // Наблюдение за смертью ботов: переходы наличия солдата (+0x3C4) пишем в лог.
    // Сборка 93: сводка «МОСТ» раз в ~20 с — сколько ботов под наблюдением и сколько с солдатом (вместо счёта SpmProbe).
    static LONG s_watchTicks = 0;
    unsigned int wAlive = 0, wDead = 0;
    for (LONG i = 0; i < g_watchCount && i < 64; ++i) {
        unsigned long sol = 0;
        __try {
            sol = *(const unsigned long*)((const unsigned char*)g_watchBot[i] + kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        const unsigned char has = (sol != 0) ? 1 : 0;
        if (has != g_watchHadSoldier[i]) {
            LOG_INFO("Bridge: бот %p %s (солдат=%08lX)", (void*)g_watchBot[i],
                     has ? "получил солдата" : "ПОТЕРЯЛ солдата (смерть?)", sol);
            g_watchHadSoldier[i] = has;
            const ULONGLONG nowH = ::GetTickCount64();
            if (has) {  // сборка 174: история солдат (см. EVBotsBridgeDescribePtr)
                SoldierHist& h = g_solHist[g_solHistNext++ % 128];
                h.soldier = sol;
                h.player = g_watchBot[i];
                h.bornAt = nowH;
                h.diedAt = 0;
            } else {
                for (LONG k = 0; k < 128; ++k) {
                    if (g_solHist[k].player == g_watchBot[i] && g_solHist[k].diedAt == 0 && g_solHist[k].soldier != 0) {
                        g_solHist[k].diedAt = nowH;
                    }
                }
            }
            if (!has && g_deadCount < 64) {
                g_deadQueue[g_deadCount++] = g_watchBot[i];  // в очередь для Lua
            }
        }
        if (has) ++wAlive; else ++wDead;
    }
    if ((++s_watchTicks % 600) == 0) {
        LOG_INFO("Bridge: МОСТ — под наблюдением=%ld, с солдатом=%u, без солдата=%u", g_watchCount, wAlive, wDead);
    }
    // Тест респавна без человека у экрана (13.10): EVBOTS_TEST_KILLBOT=<секунд> —
    // через N с после первых тел убить одного живого бота движком (один раз).
    {
        static int s_killAfter = -1;
        static bool s_killDone = false;
        if (s_killAfter < 0) {
            char v[8] = {};
            const DWORD n = ::GetEnvironmentVariableA("EVBOTS_TEST_KILLBOT", v, (DWORD)sizeof(v));
            s_killAfter = (n > 0 && n < sizeof(v)) ? ::atoi(v) : 0;
        }
        if (s_killAfter > 0 && !s_killDone && g_bodiesAt != 0 &&
            ::GetTickCount64() - g_bodiesAt > (ULONGLONG)s_killAfter * 1000ULL) {
            for (LONG i = 0; i < g_watchCount; ++i) {
                if (g_watchHadSoldier[i] && NativeKillPlayer(g_watchBot[i])) {
                    LOG_INFO("Bridge: ТЕСТ — бот %p убит для проверки респавна", (void*)g_watchBot[i]);
                    s_killDone = true;
                    break;
                }
            }
        }
    }
    // Сборка 123: счётчик перехвата vf13C (наши боты), раз в ~20 с.
    {
        static ULONGLONG s_vfLogAt = 0;
        const ULONGLONG nowVf = ::GetTickCount64();
        if (InterlockedCompareExchange(&g_vf13cInstalled, 0, 0) > 0 && nowVf - s_vfLogAt > 20000) {
            s_vfLogAt = nowVf;
            LOG_INFO("Bridge: vf13C: ботам отвечено 0 раз=%ld", (long)InterlockedCompareExchange(&g_vf13cBotHits, 0, 0));
        }
    }
    // Отложенные тела: как только человек заспавнился (g_spawnSelf известен) или (сборка 123) человек уже на
    // сервере с командой и китом — до дислокации, как в VU.
    if (g_deferredSpawnCount > 0 && EarlyBodiesReady()) {
        // ★ Сборка 174: бота без выбора мода (оружие) держим до EVBOTS_MOD_SEL_WAIT_MS (3000; 0 — не ждать), как VU:
        // к посадке выбор мода уже в игроке. Очередь трогает только этот поток (NativeCreateOne зовётся отсюда же).
        static const ULONGLONG s_selWaitMs = [] {
            char v[16] = {};
            const DWORD n = ::GetEnvironmentVariableA("EVBOTS_MOD_SEL_WAIT_MS", v, (DWORD)sizeof(v));
            return (n > 0 && n < sizeof(v)) ? (ULONGLONG)::strtoul(v, nullptr, 10) : 3000ULL;
        }();
        const ULONGLONG nowD = ::GetTickCount64();
        const LONG n = g_deferredSpawnCount;
        unsigned long waitP[32];
        ULONGLONG waitSince[32];
        LONG waitN = 0;
        g_deferredSpawnCount = 0;
        for (LONG i = 0; i < n && i < 32; ++i) {
            const unsigned long p = g_deferredSpawn[i];
            if (p == 0) continue;
            const ULONGLONG since = g_deferredSince[i];
            if (!ModWeaponsReady(p)) {
                if (s_selWaitMs > 0 && nowD - since < s_selWaitMs) {
                    waitP[waitN] = p;
                    waitSince[waitN++] = since;
                    continue;
                }
                LOG_WARN("Bridge: отложенный бот %p: выбор мода не пришёл за %llu мс — сажаем без оружия мода",
                         (void*)(ULONGLONG)p, (unsigned long long)(nowD - since));
            } else if (nowD - since > 50) {
                LOG_INFO("Bridge: отложенный бот %p: выбор мода пришёл через %llu мс — сажаем",
                         (void*)(ULONGLONG)p, (unsigned long long)(nowD - since));
            }
            if (g_bodiesAt == 0) g_bodiesAt = nowD;
            unsigned int team = 0;
            __try {
                team = *(const unsigned int*)((const unsigned char*)p + kOffPlayerTeam);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            // ★ Сборка 243 (прогон 242): точка — только своей команды. Было `EVBotsEngineSpawnPointBest()` — точка живого
            // игрока (HQ США) для любой команды: боты РФ появлялись на базе США. Без точки и без позиции мода бот раньше молча
            // выпадал из очереди и ждал повтора спавна модом (тела — только после дислокации человека); теперь ждёт в очереди.
            unsigned long sp = ::evbots::EVBotsEngineSpawnPointForTeam(team);
            if (sp == 0) sp = ::evbots::EVBotsEngineSpawnPointBestForTeam(team);
            float qx = 0.f, qy = 0.f, qz = 0.f;
            if (sp == 0 && !(EngineSpawnOn() && BotPosOf(p, &qx, &qy, &qz))) {
                if (waitN < 32) {
                    waitP[waitN] = p;
                    waitSince[waitN++] = since;
                }
                static volatile LONG s_noPtLog = 0;
                if (InterlockedIncrement(&s_noPtLog) <= 20)
                    LOG_INFO("Bridge: отложенный бот %p (команда %u): точки своей команды и позиции мода нет — ждёт в очереди",
                             (void*)(ULONGLONG)p, team);
                continue;
            }
            // ★ Бот создан ДО живого спавна, эталонных полей тогда не было (+0x480=0,
            // прогон 57: все 15 посадок — «отказ движка»). Теперь эталон есть — пишем.
            {
                unsigned long s318 = 0, s31c = 0, s480 = 0;
                ::evbots::EVBotsEngineSpawnRefFields(&s318, &s31c, &s480);
                // Сборка 158 (трек 9, H: AI_GUARD): EVBOTS_AI_GUARD=1 — если свой ввод уже выдан, флаг ИИ не сбрасываем
                // в эталон (0), а пишем BotAiFlag(). По умолчанию ВЫКЛ (как до 158).
                static const bool s_aiGuard = [] {
                    char v[4] = {};
                    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_AI_GUARD", v, (DWORD)sizeof(v));
                    return n == 1 && v[0] == '1';
                }();
                const unsigned aiVal = (s_aiGuard && OwnInputOf(p) != nullptr) ? BotAiFlagFor(p) : (unsigned)s318;
                __try {
                    *(unsigned int*)((unsigned char*)p + kOffPlayerIsAi) = aiVal;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    LOG_ERR("Bridge: не удалось записать эталон отложенному боту %p", (void*)p);
                    continue;
                }
                if (s480 != 0) SetPlayerRef480(p, s480);  // с addref, не напрямую
                LOG_INFO("Bridge: отложенный бот %p: эталон 318=%08lX 480=%08lX", (void*)p, s318, s480);
            }
            NativeSpawnOne(p, sp);  // 243: sp == 0 — посадка как VU по позиции мода
        }
        for (LONG i = 0; i < waitN && g_deferredSpawnCount < 32; ++i) {
            g_deferredSince[g_deferredSpawnCount] = waitSince[i];
            g_deferredSpawn[g_deferredSpawnCount++] = waitP[i];
        }
    }
    const LONG total = InterlockedCompareExchange(&g_pendingNativeCount, 0, 0);
    if (total <= 0) return;
    // Ждём, пока скан даст точки хотя бы одной команде (или нейтральные).
    // Ждём конец скана: иначе боты создаются раньше, чем известны точки, и
    // садятся на точку живого игрока (прогон 43: разрыв 250 мс).
    if (!::evbots::EVBotsEngineSpawnScanDone()) {
        ::evbots::EVBotsEngineStartSpawnScan();  // не ждём живого спавна
        return;
    }
    // Снимок очереди под замком (очередь сразу пустая для Lua), разбор без замка, неудачных — обратно в начало.
    PendingPlayer snap[32];
    LONG snapN = 0;
    ::AcquireSRWLockExclusive(&g_pendingLock);
    snapN = g_pendingNativeCount < 32 ? g_pendingNativeCount : 32;
    for (LONG i = 0; i < snapN; ++i) snap[i] = g_pendingNative[i];
    InterlockedExchange(&g_pendingNativeCount, 0);
    ::ReleaseSRWLockExclusive(&g_pendingLock);
    (void)total;
    PendingPlayer failed[32];
    LONG kept = 0;
    for (LONG i = 0; i < snapN; ++i) {
        if (!HumanInLevel()) {  // сборка 150: создаём, когда клиент человека «в уровне»
            failed[kept++] = snap[i];
            continue;
        }
        const unsigned long ptr = NativeCreateOne(snap[i].name, snap[i].team);
        if (ptr != 0) {
            LOG_INFO("Bridge: очередь — \"%s\" создан в движке ptr=%08X",
                     snap[i].name, ptr);
            RememberPtrName(ptr, snap[i].name);  // сборка 159: имя ↔ указатель (выбор мода по имени)
            if (g_createdCount < 64) {
                ::strncpy(g_createdBots[g_createdCount].name, snap[i].name, 31);
                g_createdBots[g_createdCount].name[31] = '\0';
                g_createdBots[g_createdCount].ptr = ptr;
                ++g_createdCount;
            }
        } else {
            failed[kept++] = snap[i];
        }
    }
    if (kept > 0) {
        // неудачные — в начало, перед тем, что Lua успел добавить за проход
        ::AcquireSRWLockExclusive(&g_pendingLock);
        const LONG added = g_pendingNativeCount;
        const LONG room = 32 - kept;
        const LONG move = added < room ? added : room;
        for (LONG j = move - 1; j >= 0; --j) g_pendingNative[kept + j] = g_pendingNative[j];
        for (LONG j = 0; j < kept; ++j) g_pendingNative[j] = failed[j];
        InterlockedExchange(&g_pendingNativeCount, kept + move);
        ::ReleaseSRWLockExclusive(&g_pendingLock);
    }
}

// Повтор отложенных спавнов. Вызывается из хука ServerPlayerManager::update,
// когда точка спавна уже известна (после первого входа живого игрока).
// Файл УЖЕ внутри namespace evbots — лишняя обёртка дала бы evbots::evbots::
// и символ не нашёлся бы (LNK2019).
void RetryPendingSpawns() {
    const unsigned long sp = EVBotsEngineSpawnPoint();
    if (sp == 0) return;
    const LONG n = InterlockedCompareExchange(&g_pendingCount, 0, 0);
    if (n <= 0) return;
    LOG_INFO("Bridge: повтор спавна очереди, игроков %ld, точка %p", (long)n,
             (void*)sp);
    unsigned long done = 0;
    for (LONG i = 0; i < n && i < (LONG)kMaxPending; ++i) {
        const unsigned long ptr = g_pending[i];
        if (ptr == 0 || !Readable((void*)ptr)) continue;
        // Уже есть солдат — ничего не делаем.
        unsigned long soldier = 0;
        __try {
            soldier = *(const unsigned long*)((const unsigned char*)ptr +
                                              kOffPlayerSoldier);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (soldier != 0) continue;
        if (NativeSpawnOne(ptr, sp) != 0) ++done;
    }
    if (done > 0) {
        LOG_INFO("Bridge: ★ из очереди удалось посадить %lu солдат(ов)", done);
    }
}

bool EVBotsOnServerFrameThread();
// VU FadeOutAll/FadeInAll (10B99780/10B996E0) construct the same
// NetworkScreenFadeMessage and broadcast it through ServerGameContext+34.
// Verified dedicated counterparts: ctor 022FF0D0, send D6E9E0, base dtor 1A0D980.
// VU Player:Fade 1098B910 uses FindConnection A426A0 and Send A31930.
// Dedicated counterparts D6E1C0/E4DE80 verified against the stock broadcast path.
static bool NativeScreenFade(float seconds, bool toBlack, bool broadcast, uintptr_t player) {
    if (!EVBotsOnServerFrameThread() || !std::isfinite(seconds) || seconds < 0) return false;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleA("bf3_server_Final.exe")) != 0x400000) return false;
    char disabled[4] = {};
    if (GetEnvironmentVariableA("EVBOTS_NATIVE_FADE", disabled, sizeof(disabled)) == 1 && disabled[0] == '0') return false;
    struct Signature { uintptr_t at; unsigned char bytes[16]; };
    static const Signature signatures[] = {
        {0x022FF0D0,{0x56,0x68,0x3A,0x8C,0xE6,0x25,0x68,0x7B,0x27,0x7D,0xC6,0x8B,0xF1,0xE8,0x6E,0xE8}},
        {0x00D6E9E0,{0x56,0x8B,0xF1,0x8B,0x8E,0x4C,0x30,0,0,0x57,0x8B,0x7C,0x24,0x0C,0x85,0xC9}},
        {0x01A0D980,{0x56,0x8B,0xF1,0x8B,0x46,0x30,0xC7,0x06,0xD8,0xD7,0x75,0x02,0x85,0xC0,0x74,0x07}},
        {0x00D6E1C0,{0x53,0x55,0x8B,0x6C,0x24,0x0C,0x8B,0xD9,0x85,0xED,0x75,0x07,0x5D,0x33,0xC0,0x5B}},
        {0x00E4DE80,{0x81,0xEC,0x34,0x01,0x00,0x00,0x56,0x57,0x8B,0xBC,0x24,0x40,0x01,0x00,0x00,0x8B}}
    };
    static LONG verified = 0;
    alignas(16) unsigned char message[0x40] = {};
    bool constructed = false, sent = false;
    using Ctor = void*(__fastcall*)(void*, void*);
    using Send = void(__fastcall*)(void*, void*, void*);
    using Dtor = void*(__fastcall*)(void*, void*, unsigned);
    __try {
        if (verified == 0) {
            bool all = true;
            for (const auto& sig : signatures) {
                const bool match = std::memcmp(reinterpret_cast<void*>(sig.at), sig.bytes, sizeof(sig.bytes)) == 0;
                LOG_INFO("ScreenFade: signature %08X %s", static_cast<unsigned>(sig.at), match ? "PASS" : "FAIL");
                all = all && match;
            }
            verified = all ? 1 : -1;
        }
        if (verified < 0) return false;
        const uintptr_t context = *reinterpret_cast<const uintptr_t*>(0x02B3EE2C);
        const uintptr_t server = context ? *reinterpret_cast<const uintptr_t*>(context + 0x34) : 0;
        if (!server) return false;
        uintptr_t recipient = server;
        if (!broadcast) {
            if (!player) return false;
            using FindConnection = uintptr_t(__fastcall*)(void*, void*, uintptr_t);
            // The lookup compares against active connections without dereferencing the supplied pointer.
            recipient = reinterpret_cast<FindConnection>(0x00D6E1C0)(reinterpret_cast<void*>(server), nullptr, player);
            if (!recipient) return false;
        }
        reinterpret_cast<Ctor>(0x022FF0D0)(message, nullptr);
        constructed = true;
        std::memcpy(message + 0x38, &seconds, sizeof(seconds));
        message[0x3C] = toBlack ? 1 : 0;
        reinterpret_cast<Send>(broadcast ? 0x00D6E9E0 : 0x00E4DE80)(reinterpret_cast<void*>(recipient), nullptr, message);
        sent = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        verified = -1;
        LOG_ERR("ScreenFade: native call failed; disabled for this process");
    }
    if (constructed) {
        __try { reinterpret_cast<Dtor>(0x01A0D980)(message, nullptr, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { verified = -1; sent = false; }
    }
    if (sent) LOG_INFO("ScreenFade: %s player=%08X %s %.3fs", broadcast ? "broadcast" : "targeted",
                       static_cast<unsigned>(player), toBlack ? "out" : "in", static_cast<double>(seconds));
    return sent;
}
static int l_native_fade_all(lua_State* L) {
    const double seconds = luaL_checknumber(L, 1);
    luaL_argcheck(L, std::isfinite(seconds) && seconds >= 0 && std::isfinite(static_cast<float>(seconds)), 1,
                  "fadeTime must be a finite non-negative float");
    lua_pushboolean(L, NativeScreenFade(static_cast<float>(seconds), lua_toboolean(L, 2) != 0, true, 0));
    return 1;
}
static int l_native_player_fade(lua_State* L) {
    const lua_Integer player = luaL_checkinteger(L, 1);
    luaL_argcheck(L, player >= 0 && static_cast<unsigned long long>(player) <= 0xffffffffULL, 1, "invalid native player");
    const double seconds = luaL_checknumber(L, 2);
    luaL_argcheck(L, std::isfinite(seconds) && seconds >= 0 && std::isfinite(static_cast<float>(seconds)), 2,
                  "fadeTime must be a finite non-negative float");
    lua_pushboolean(L, NativeScreenFade(static_cast<float>(seconds), lua_toboolean(L, 3) != 0, false,
                                      static_cast<uintptr_t>(player)));
    return 1;
}

bool RegisterVextPlayerManager(lua_State* L) {
    lua_pushcfunction(L, l_native_fade_all);
    lua_setglobal(L, "EVBotsNativeFadeAll");
    lua_pushcfunction(L, l_native_player_fade);
    lua_setglobal(L, "EVBotsNativePlayerFade");
    if(luaL_newmetatable(L,kHumanSoldierTargetMeta)) {
        lua_pushliteral(L,"native human soldier target");lua_setfield(L,-2,"__metatable");
    }
    lua_pop(L,1);
    soldiercustom::Start();
    // Мост Lua -> движок ВКЛЮЧАЕТСЯ ЯВНО: EVBOTS_BRIDGE=1.
    // По умолчанию выключен, чтобы DLL вела себя ровно как до его появления
    // (регресс 19:51 ловился только с мостом включённым).
    // ВАЖНО: сравниваем ЗНАЧЕНИЕ переменной, а не факт её существования:
    // GetEnvironmentVariableA(name, nullptr, 0) == 0, когда переменная НЕ
    // задана (из-за этого полчаса прогоны сравнивались не с теми настройками).
    bool bridgeOn = false;
    char b[8] = {};
    const DWORD bn = ::GetEnvironmentVariableA("EVBOTS_BRIDGE", b,
                                               (DWORD)sizeof(b));
    if (bn == 1 && b[0] == '1') bridgeOn = true;
    // Сборка 126: подробные логи МОДА на окно 150 с (Debug.Logger.PRINTALL в тике моста). Только при EVBOTS_MODLOG=1.
    {
        char m[4] = {};
        const DWORD mn = ::GetEnvironmentVariableA("EVBOTS_MODLOG", m, (DWORD)sizeof(m));
        lua_pushboolean(L, (mn == 1 && m[0] == '1') ? 1 : 0);
        lua_setglobal(L, "EVBOTS_MODLOG");
    }
    // Сборка 127: мод не телепортирует застрявших ботов (Config.TeleportIfStuck = false). Сборка 138 (просьба пользователя:
    // «телепортацию ботов вырубить»): по умолчанию ВЫКЛЮЧЕНА; EVBOTS_NOTELEPORT=0 — как в моде (телепорт включён).
    // ★ Сборка 154 (просьба пользователя 15.10 ≈20:15: «телепорт включи, как у VU — он включён по умолчанию»): по умолчанию
    // ВКЛЮЧЁН (настройка мода Config.TeleportIfStuck не трогается); EVBOTS_NOTELEPORT=1 — выключить.
    {
        char nt[4] = {};
        const DWORD nn = ::GetEnvironmentVariableA("EVBOTS_NOTELEPORT", nt, (DWORD)sizeof(nt));
        lua_pushboolean(L, (nn == 1 && nt[0] == '1') ? 1 : 0);
        lua_setglobal(L, "EVBOTS_NOTELEPORT");
    }
    // Сборка 155: VU-события жизни человека (Player:Joining/Authenticated/Left/Destroyed) шлются по умолчанию;
    // EVBOTS_NOHUMANEVENTS=1 — выключить (поведение до 155: мод событий не получал).
    {
        char nh[4] = {};
        const DWORD nhn = ::GetEnvironmentVariableA("EVBOTS_NOHUMANEVENTS", nh, (DWORD)sizeof(nh));
        lua_pushboolean(L, (nhn == 1 && nh[0] == '1') ? 1 : 0);
        lua_setglobal(L, "EVBOTS_NOHUMANEVENTS");
    }
    // Сборка 128: настоящий телепорт застрявших ботов (SoldierEntity:SetTransform -> NativeBridge.TeleportBot). По умолчанию ВКЛ.
    {
        char tr[4] = {};
        const DWORD tn = ::GetEnvironmentVariableA("EVBOTS_TP_REAL", tr, (DWORD)sizeof(tr));
        lua_pushboolean(L, (tn == 1 && tr[0] == '0') ? 0 : 1);
        lua_setglobal(L, "EVBOTS_TP_REAL");
    }
    // Сборка 237: серверная эмуляция клиента мода (Client:RequestSettings, лучи, реанимация) была ВЫКЛ для всех.
    // ★ 238-3 (просьба пользователя: боты должны бить всех людей, а не только хоста): по умолчанию — по каждому игроку: от его
    // соединения пришли события клиента мода — считает его клиент, как у VU; нет (гость без клиентской части EVBots или без
    // файлов мода) — сервер подменяет клиента для него. EVBOTS_SERVER_CLIENT_EMU=0 — как 237 (у всех свой клиент).
    {
        char ce[4] = {};
        const DWORD cn = ::GetEnvironmentVariableA("EVBOTS_SERVER_CLIENT_EMU", ce, (DWORD)sizeof(ce));
        lua_pushboolean(L, (cn == 1 && ce[0] == '0') ? 0 : 1);
        lua_setglobal(L, "EVBOTS_SERVER_CLIENT_EMU");
        // ★ 247: EVBOTS_BB_ON_CLIENT=1 — пары bot-bot считает клиент мода (как VU); по умолчанию — сервер (у клиента нет тел ботов)
        char bbc[4] = {};
        const DWORD bbn = ::GetEnvironmentVariableA("EVBOTS_BB_ON_CLIENT", bbc, (DWORD)sizeof(bbc));
        lua_pushboolean(L, (bbn == 1 && bbc[0] == '1') ? 1 : 0);
        lua_setglobal(L, "EVBOTS_BB_ON_CLIENT");
    }
    // ★ 238-5: диагностика ИКОНКИ/ПЕРКИ ботов раз в 20 с — только по флагу (по умолчанию выкл.)
    {
        char di[4] = {};
        const DWORD dn = ::GetEnvironmentVariableA("EVBOTS_DIAG_ICONS", di, (DWORD)sizeof(di));
        lua_pushboolean(L, (dn == 1 && di[0] == '1') ? 1 : 0);
        lua_setglobal(L, "EVBOTS_DIAG_ICONS");
    }
    // Сборка 171: удержание скорости, когда движок ещё не обновил позицию (EVBotsVelocityStep). По умолчанию ВКЛ.
    {
        char vh[4] = {};
        const DWORD vn = ::GetEnvironmentVariableA("EVBOTS_VEL_HOLD", vh, (DWORD)sizeof(vh));
        lua_pushboolean(L, (vn == 1 && vh[0] == '0') ? 0 : 1);
        lua_setglobal(L, "EVBOTS_VEL_HOLD");
    }
    // Сборка 178: знак руля водителя техники (EIAYaw=4 в машине). 1 — как у мода, -1 — инвертировать (опыт, если «руль против»).
    {
        char ys[4] = {};
        const DWORD yn = ::GetEnvironmentVariableA("EVBOTS_VEH_YAW_SIGN", ys, (DWORD)sizeof(ys));
        lua_pushinteger(L, (yn == 2 && ys[0] == '-' && ys[1] == '1') ? -1 : 1);
        lua_setglobal(L, "EVBOTS_VEH_YAW_SIGN");
    }
    // Сборка 132: настоящая поза солдата (SetPose -> движок, soldier.pose — из движка). По умолчанию ВКЛ.
    {
        char pr[4] = {};
        const DWORD pn = ::GetEnvironmentVariableA("EVBOTS_POSE_REAL", pr, (DWORD)sizeof(pr));
        lua_pushboolean(L, (pn == 1 && pr[0] == '0') ? 0 : 1);
        lua_setglobal(L, "EVBOTS_POSE_REAL");
    }
    // Сборка 134: скорость не измеренного солдата — «идёт», а не «стоит» (до старта раунда мод не должен запускать обход). По умолчанию ВКЛ.
    {
        char vs[4] = {};
        const DWORD vn = ::GetEnvironmentVariableA("EVBOTS_VEL_STUB", vs, (DWORD)sizeof(vs));
        lua_pushboolean(L, (vn == 1 && vs[0] == '0') ? 0 : 1);
        lua_setglobal(L, "EVBOTS_VEL_STUB");
    }
    // Сборка 135: player.soldier / hasSoldier / alive как в VU (по телу в движке [игрок+0x3C4]). По умолчанию ВКЛ.
    {
        char sv[4] = {};
        const DWORD sn = ::GetEnvironmentVariableA("EVBOTS_SOLDIER_VU", sv, (DWORD)sizeof(sv));
        lua_pushboolean(L, (sn == 1 && sv[0] == '0') ? 0 : 1);
        lua_setglobal(L, "EVBOTS_SOLDIER_VU");
    }
    // Сборка 212: «ДИАГ скорость движка» (скан памяти солдата/машины, сборка 200) — только при EVBOTS_VELSCAN=1. По умолчанию ВЫКЛ.
    {
        char sv[4] = {};
        const DWORD sn = ::GetEnvironmentVariableA("EVBOTS_VELSCAN", sv, (DWORD)sizeof(sv));
        lua_pushboolean(L, (sn == 1 && sv[0] == '1') ? 1 : 0);
        lua_setglobal(L, "EVBOTS_VELSCAN");
    }

    if (bridgeOn) {
        RegisterBridge(L);
    } else {
        LOG_INFO("EVBots: NativeBridge ВЫКЛЮЧЕН (задай EVBOTS_BRIDGE=1)");
    }

    if (luaL_loadbufferx(L, kPlayerManagerSource,
                         sizeof(kPlayerManagerSource) - 1,
                         "=[vext-playermanager]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: PlayerManager не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_playermanager_log);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: PlayerManager не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: PlayerManager подключен (server), реестр игроков");
    return true;
}

}  // namespace evbots
