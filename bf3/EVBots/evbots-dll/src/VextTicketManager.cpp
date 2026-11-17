// VextTicketManager.cpp — библиотека VeniceEXT TicketManager.
//
// Глобальная таблица TicketManager (server), по VU-Docs
// content\vext\ref\server\library\ticketmanager.md:
//   TicketManager:GetTicketCount(team) -> float — НАСТОЯЩИЕ тикеты движка;
//   TicketManager:GetCurrentRound() / GetRoundCount() — пока заглушки (0).
//
// ★ 13.10.2026: GetTicketCount раньше всегда возвращал 0. fun-bots в Conquest
// (BotSpawner:TriggerRespawnBot, DONT_SPAWN_BOTS_ON_LAST_CONQUEST_TICKET) не
// возрождает ботов при тикетах < 2 — поэтому убитые боты не возвращались.
// Тикеты движка (разбор телеметрии 0x11DBCF0, re\tickets_decomp.txt):
//   перебор сущностей ServerTicketCounterEntity:
//     world = [0x2B3EE30]; 0x5CF340(this=world, typeinfo 0x2C230C0, &iter, 1)
//     (thiscall, RET 0xC); 0x5CF2F0(this=&iter) -> сущность или 0;
//   команда счётчика: [[entity+0x14]+0x6C] (геттер 0x12CE180);
//   тикеты: float [entity+0xA0] (телеметрия делает fld dword [eax+0xA0]).

#include "VextTicketManager.h"
#include "TeamCapacity.h"

#include <windows.h>

#include <string>
#include <cstring>
#include <climits>

#include "EVBotsLog.h"
#include "InlineHook.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

constexpr unsigned long kImageBase = 0x00400000UL;
constexpr unsigned long kEntityWorldVa = 0x02B3EE30UL;   // [VA] -> мир сущностей
constexpr unsigned long kTicketTypeVa = 0x02C230C0UL;    // typeinfo ServerTicketCounterEntity
constexpr unsigned long kIterBeginVa = 0x005CF340UL;     // TypedEntityIterator: начало
constexpr unsigned long kIterNextVa = 0x005CF2F0UL;      // TypedEntityIterator: следующий
constexpr int kOffEntityData = 0x14;
constexpr int kOffDataTeam = 0x6C;
constexpr int kOffTickets = 0xA0;

// Тикеты команды по счётчикам движка. false — движка нет (тест-хост) или
// счётчик этой команды не найден.
bool EngineTicketCount(int team, float* out, unsigned long* entityOut) {
    unsigned char* base = reinterpret_cast<unsigned char*>(
        ::GetModuleHandleA("bf3_server_Final.exe"));
    if (base == nullptr) return false;
    using BeginFn = void(__fastcall*)(void*, void*, void*, void*, int);
    using NextFn = unsigned char*(__fastcall*)(void*, void*);
    unsigned char iter[64] = {};
    __try {
        void* world = *reinterpret_cast<void**>(base + (kEntityWorldVa - kImageBase));
        if (world == nullptr) return false;
        reinterpret_cast<BeginFn>(base + (kIterBeginVa - kImageBase))(
            world, nullptr, base + (kTicketTypeVa - kImageBase), iter, 1);
        const NextFn next = reinterpret_cast<NextFn>(base + (kIterNextVa - kImageBase));
        for (int guard = 0; guard < 32; ++guard) {
            unsigned char* e = next(iter, nullptr);
            if (e == nullptr) break;
            const unsigned char* data = *reinterpret_cast<unsigned char* const*>(e + kOffEntityData);
            if (data == nullptr) continue;
            if (*reinterpret_cast<const int*>(data + kOffDataTeam) != team) continue;
            *out = *reinterpret_cast<const float*>(e + kOffTickets);
            *entityOut = (unsigned long)(ULONG_PTR)e;
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

/// nativeTickets(team) -> float | nil
int l_native_tickets(lua_State* L) {
    const int team = (int)luaL_optinteger(L, 1, 0);
    float tickets = 0.0f;
    unsigned long entity = 0;
    if (!EngineTicketCount(team, &tickets, &entity)) {
        static volatile LONG s_missLogged = 0;
        if (InterlockedExchange(&s_missLogged, 1) == 0) {
            // ★ Сборка 208: при старте мод спрашивает тикеты до загрузки уровня (счётчиков ещё нет) — моду 0, как без
            // счётчика у VU; после Level:Loaded значения из движка. Это не ошибка — INFO, а не WARN.
            LOG_INFO("EVBots: TicketManager: счётчика тикетов команды %d ещё нет (уровень не загружен) — моду 0", team);
        }
        lua_pushnil(L);
        return 1;
    }
    // Для проверки по логу: первое значение каждой команды.
    static volatile LONG s_teamLogged[8] = {};
    if (team >= 0 && team < 8 && InterlockedExchange(&s_teamLogged[team], 1) == 0) {
        LOG_INFO("EVBots: TicketManager: команда %d — %.1f тикетов (сущность %08lX)", team,
                 (double)tickets, entity);
    }
    lua_pushnumber(L, (lua_Number)tickets);
    return 1;
}

// ServerTicketCounterEntity (Conquest): VU's SetTicketCount calls client
// 008987C0 with a float. Dedicated equivalent 012D54C0, thiscall/ret4:
// clamps negative values, marks replication dirty, updates +A0 and dispatches
// the engine's counter-changed message. Verified from both executables 17.11.
bool EngineSetTicketCount(int team, int count) {
    if (!EVBotsOnServerFrameThread()) return false;
    float oldCount = 0.0f;
    unsigned long entity = 0;
    if (!EngineTicketCount(team, &oldCount, &entity)) return false;
    unsigned char* base = reinterpret_cast<unsigned char*>(::GetModuleHandleA("bf3_server_Final.exe"));
    if (!base) return false;
    const unsigned char expected[16] = {0x83,0xEC,0x30,0x56,0x8B,0xF1,0x83,0xBE,0xAC,0x00,0x00,0x00,0x00,0x75,0x7B,0xF3};
    unsigned char* target = base + (0x012D54C0UL - kImageBase);
    __try {
        const bool match = std::memcmp(target, expected, sizeof(expected)) == 0;
        static volatile LONG logged = 0;
        if (InterlockedExchange(&logged, 1) == 0)
            LOG_INFO("TicketManager: native SetTicketCount 012D54C0 signature %s", match ? "PASS" : "FAIL");
        if (!match) return false;
        using SetFn = void(__fastcall*)(void*, void*, float);
        reinterpret_cast<SetFn>(target)(reinterpret_cast<void*>(static_cast<ULONG_PTR>(entity)), nullptr,
                                       static_cast<float>(count));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("TicketManager: native SetTicketCount failed for team %d", team);
        return false;
    }
}
int l_native_set_tickets(lua_State* L) {
    const lua_Integer team = luaL_checkinteger(L, 1), count = luaL_checkinteger(L, 2);
    if (team < 0 || team > (TeamCapacityInstalled() ? 127 : 16) || count < INT_MIN || count > INT_MAX) {
        lua_pushboolean(L, false); return 1;
    }
    lua_pushboolean(L, EngineSetTicketCount(static_cast<int>(team), static_cast<int>(count)));
    return 1;
}

constexpr char kTicketManagerSource[] = R"LUA(
-- TicketManager VeniceEXT (server)
-- GetTicketCount — тикеты движка; GetCurrentRound, GetRoundCount — заглушки

local log, nativeTickets, nativeSetTickets = ...

local TicketManager = {}
_G.TicketManager = TicketManager

function TicketManager:SetTicketCount(team, ticketCount)
  return nativeSetTickets(team, ticketCount)
end

function TicketManager:GetTicketCount(team)
  if nativeTickets ~= nil then
    local v = nativeTickets(team or 0)
    if v ~= nil then return v end
  end
  -- Движка нет (тест-хост) или счётчика команды нет.
  return 0
end

-- ★ Сборка 198: раунд — у сервера (RCON mapList.getRounds → OK <текущий> <всего>), тот же источник, что Level:Loaded.
-- fun-bots зовёт их при поздней инициализации (__init__.lua:773 → OnLevelLoaded). Нет RCON — 0 (как было).
local function roundsFromServer()
  if EVBotsRconSend == nil then return nil, nil end
  local ok, r = pcall(EVBotsRconSend, { 'mapList.getRounds' }, 1500)
  if ok and type(r) == 'table' and r[1] == 'OK' then
    return math.tointeger(tonumber(r[2])), math.tointeger(tonumber(r[3]))
  end
  return nil, nil
end

function TicketManager:GetCurrentRound()
  local c = roundsFromServer()
  return c or 0
end

function TicketManager:GetRoundCount()
  local _, t = roundsFromServer()
  return t or 0
end

return TicketManager
)LUA";

int l_ticketmanager_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: TicketManager: %s", text);
    return 0;
}

}  // namespace

bool RegisterVextTicketManager(lua_State* L) {
    if (luaL_loadbufferx(L, kTicketManagerSource,
                         sizeof(kTicketManagerSource) - 1,
                         "=[vext-ticketmanager]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: TicketManager не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_ticketmanager_log);
    lua_pushcfunction(L, l_native_tickets);
    lua_pushcfunction(L, l_native_set_tickets);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: TicketManager не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: TicketManager подключен");
    return true;
}

}  // namespace evbots
