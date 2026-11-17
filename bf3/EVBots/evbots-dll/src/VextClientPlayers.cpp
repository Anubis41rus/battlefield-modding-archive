// VextClientPlayers.cpp — игроки клиента из движка bf3.exe (К5 шаг 4, сборка 222): основа PlayerManager клиента.
//
// Всё — как у VU, по дизассемблеру veniceext (ext\CLIENT-SLOTS-2026-10-23.md, раздел ★★★):
//   менеджер игроков клиента = [[[0x022E3790]]+0xB4]+0x1C (0x170D5180): +0x08 число слотов, +0xC0 игроки по id,
//   +0x9C..+0xA0 вектор игроков, +0xBC локальный игрок (GetLocalPlayer 10B1B810);
//   игрок: name char* [+0x10], onlineId u64 +0x20, teamId +0x31C, id +0x3EC, squadId +0x46C, isSquadLeader байт +0x470,
//   inVehicle — vfunc +0x0C игрока, soldier = [[+0x3C0]]-4, corpse = [[+0x3C4]]-4, alive — o=[[+0x3C8]]-4 и vfunc +0x0C у
//   o+0xB4, attachedControllable [+0x3D0], controlledControllable [+0x3D8].
// Трансформ сущности — как у сервера (l_bridge_soldier_pos): [e+0x1C] — блок, при бите 18 в [e+8] — +0x10+tr[6]*0x50,
// строки left/up/forward/trans по +0x00/+0x10/+0x20/+0x30 (гипотеза для клиента — общий код движка; проверить в прогоне).
// Читаем под SEH; виртуальные методы зовём только из потока движка (Lua клиента работает в хуке кадра).

#include "VextClientPlayers.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace evbots {
namespace {

constexpr uintptr_t kMainVarVa = 0x022E3790;
constexpr uint32_t kOffClientOfMain = 0xB4;
constexpr uint32_t kOffPmOfClient = 0x1C;
constexpr uint32_t kPmSlots = 0x08, kPmVecBegin = 0x9C, kPmVecEnd = 0xA0, kPmLocal = 0xBC, kPmById = 0xC0;
constexpr uint32_t kPlName = 0x10, kPlOnlineId = 0x20, kPlTeam = 0x31C, kPlId = 0x3EC, kPlSquad = 0x46C,
                   kPlSquadLeader = 0x470, kPlSoldier = 0x3C0, kPlCorpse = 0x3C4, kPlAliveRef = 0x3C8,
                   kPlAttached = 0x3D0, kPlControlled = 0x3D8;

uintptr_t U32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

uintptr_t PlayerManagerPtr() {
    __try {
        const uintptr_t main = U32(kMainVarVa);
        if (main == 0) return 0;
        const uintptr_t client = U32(main + kOffClientOfMain);
        if (client == 0) return 0;
        return U32(client + kOffPmOfClient);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/// Игрок по id (как 107A0270 у VU): массив по id, иначе поиск по вектору. 0 — нет.
uintptr_t PlayerById(uintptr_t pm, uint32_t id) {
    __try {
        if (pm == 0 || id >= U32(pm + kPmSlots)) return 0;
        const uintptr_t byId = U32(pm + kPmById);
        const uintptr_t p = byId != 0 ? U32(byId + id * 4) : 0;
        if (p != 0) return p;
        for (uintptr_t it = U32(pm + kPmVecBegin), end = U32(pm + kPmVecEnd); it != 0 && it < end; it += 4) {
            const uintptr_t q = U32(it);
            if (q != 0 && U32(q + kPlId) == id) return q;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/// Слабая ссылка движка: [[p+off]] - 4 (как soldier/corpse у VU).
uintptr_t WeakRef(uintptr_t p, uint32_t off) {
    __try {
        const uintptr_t cell = U32(p + off);
        if (cell == 0) return 0;
        const uintptr_t v = U32(cell);
        return v != 0 ? v - 4 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

using BoolMethod = bool(__fastcall*)(uintptr_t self, void* edx);

int CallBoolVfunc(uintptr_t self, uint32_t slotOff) {  // -1 — не вышло
    __try {
        if (self == 0) return -1;
        const uintptr_t vt = U32(self);
        if (vt == 0) return -1;
        const auto fn = reinterpret_cast<BoolMethod>(U32(vt + slotOff));
        return fn(self, nullptr) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool ReadTransform(uintptr_t e, float out[12]) {
    __try {
        if (e == 0) return false;
        uintptr_t tr = U32(e + 0x1C);
        if (tr == 0) return false;
        if (((U32(e + 8) >> 18) & 1u) != 0) tr = tr + 0x10 + static_cast<uintptr_t>(*reinterpret_cast<const uint8_t*>(tr + 6)) * 0x50;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) out[r * 3 + c] = *reinterpret_cast<const float*>(tr + r * 0x10 + c * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Finite(float v) { return v == v && v > -1.0e7f && v < 1.0e7f; }

bool CopyName(uintptr_t p, char* buf, size_t cap) {
    __try {
        const char* s = reinterpret_cast<const char*>(U32(p + kPlName));
        if (s == nullptr) return false;
        size_t i = 0;
        for (; i + 1 < cap && s[i] != '\0'; ++i) buf[i] = s[i];
        buf[i] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct PlayerInfo {
    uint32_t id = 0, team = 0, squad = 0, onlineLo = 0, onlineHi = 0;
    bool leader = false;
    uintptr_t soldier = 0, corpse = 0, attached = 0, controlled = 0;
};

bool ReadInfo(uintptr_t p, PlayerInfo* o) {
    __try {
        o->id = static_cast<uint32_t>(U32(p + kPlId));
        o->team = static_cast<uint32_t>(U32(p + kPlTeam));
        o->squad = static_cast<uint32_t>(U32(p + kPlSquad));
        o->onlineLo = static_cast<uint32_t>(U32(p + kPlOnlineId));
        o->onlineHi = static_cast<uint32_t>(U32(p + kPlOnlineId + 4));
        o->leader = *reinterpret_cast<const uint8_t*>(p + kPlSquadLeader) != 0;
        o->attached = U32(p + kPlAttached);
        o->controlled = U32(p + kPlControlled);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Lua ──────────────────────────────────────────────────────────────────────────────────────────────────────────

/// __clp_local() -> id локального игрока или nil (как GetLocalPlayer у VU: годен, только если он же в массиве по id).
int l_clp_local(lua_State* L) {
    const uintptr_t pm = PlayerManagerPtr();
    uintptr_t p = 0;
    uint32_t id = 0;
    __try {
        if (pm != 0) {
            p = U32(pm + kPmLocal);
            if (p != 0) id = static_cast<uint32_t>(U32(p + kPlId));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p = 0;
    }
    if (p == 0 || PlayerById(pm, id) != p) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, id);
    return 1;
}

/// __clp_ids() -> массив id всех игроков (вектор менеджера, как GetPlayers у VU).
int l_clp_ids(lua_State* L) {
    lua_newtable(L);
    const uintptr_t pm = PlayerManagerPtr();
    if (pm == 0) return 1;
    uint32_t ids[256];
    int n = 0;
    __try {
        for (uintptr_t it = U32(pm + kPmVecBegin), end = U32(pm + kPmVecEnd); it != 0 && it < end && n < 256; it += 4) {
            const uintptr_t q = U32(it);
            if (q != 0) ids[n++] = static_cast<uint32_t>(U32(q + kPlId));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    for (int i = 0; i < n; ++i) {
        lua_pushinteger(L, ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/// __clp_info(id) -> таблица полей игрока или nil (игрок ушёл — у VU «destroyed player object»).
int l_clp_info(lua_State* L) {
    const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const uintptr_t p = PlayerById(PlayerManagerPtr(), id);
    PlayerInfo info;
    if (p == 0 || !ReadInfo(p, &info)) {
        lua_pushnil(L);
        return 1;
    }
    char name[96] = "";
    CopyName(p, name, sizeof(name));
    info.soldier = WeakRef(p, kPlSoldier);
    info.corpse = WeakRef(p, kPlCorpse);
    const uintptr_t aliveObj = WeakRef(p, kPlAliveRef);
    const int alive = aliveObj != 0 ? CallBoolVfunc(aliveObj + 0xB4, 0x0C) : 0;
    const int inVehicle = CallBoolVfunc(p, 0x0C);

    lua_createtable(L, 0, 14);
    lua_pushinteger(L, info.id); lua_setfield(L, -2, "id");
    lua_pushstring(L, name); lua_setfield(L, -2, "name");
    lua_pushinteger(L, info.team); lua_setfield(L, -2, "teamId");
    lua_pushinteger(L, info.squad); lua_setfield(L, -2, "squadId");
    lua_pushinteger(L, static_cast<lua_Integer>((static_cast<uint64_t>(info.onlineHi) << 32) | info.onlineLo));
    lua_setfield(L, -2, "onlineId");
    lua_pushboolean(L, info.leader); lua_setfield(L, -2, "isSquadLeader");
    lua_pushboolean(L, alive == 1); lua_setfield(L, -2, "alive");
    lua_pushboolean(L, inVehicle == 1); lua_setfield(L, -2, "inVehicle");
    lua_pushinteger(L, static_cast<lua_Integer>(info.soldier)); lua_setfield(L, -2, "soldier");
    lua_pushinteger(L, static_cast<lua_Integer>(info.corpse)); lua_setfield(L, -2, "corpse");
    lua_pushinteger(L, static_cast<lua_Integer>(info.attached)); lua_setfield(L, -2, "attached");
    lua_pushinteger(L, static_cast<lua_Integer>(info.controlled)); lua_setfield(L, -2, "controlled");
    return 1;
}

/// ★ Сборка 238-5: __clp_field(id, ключ) -> одно поле игрока (те же ключи, что у __clp_info) или nil. Мод трогает поля
/// игрока каждый кадр (ClientBotManager: teamId/onlineId/soldier/inVehicle у 30 игроков) — раньше на каждое обращение
/// __clp_info строил таблицу из 12 полей со строкой имени и двумя вызовами движка: время кадра и мусор для сборщика
/// (прогон 238-4: UpdateManager:Update мода до 28 мс). Поле по-прежнему читается из движка при каждом обращении, как у VU.
int l_clp_field(lua_State* L) {
    const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    size_t klen = 0;
    const char* k = luaL_checklstring(L, 2, &klen);
    const uintptr_t p = PlayerById(PlayerManagerPtr(), id);
    if (p == 0) {
        lua_pushnil(L);
        return 1;
    }
    auto is = [&](const char* s) { return std::strcmp(k, s) == 0; };
    if (is("name")) {
        char name[96] = "";
        if (!CopyName(p, name, sizeof(name))) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushstring(L, name);
        return 1;
    }
    if (is("alive")) {
        const uintptr_t aliveObj = WeakRef(p, kPlAliveRef);
        lua_pushboolean(L, aliveObj != 0 && CallBoolVfunc(aliveObj + 0xB4, 0x0C) == 1);
        return 1;
    }
    if (is("inVehicle")) {
        lua_pushboolean(L, CallBoolVfunc(p, 0x0C) == 1);
        return 1;
    }
    if (is("soldier")) {
        lua_pushinteger(L, static_cast<lua_Integer>(WeakRef(p, kPlSoldier)));
        return 1;
    }
    if (is("corpse")) {
        lua_pushinteger(L, static_cast<lua_Integer>(WeakRef(p, kPlCorpse)));
        return 1;
    }
    PlayerInfo info;
    if (!ReadInfo(p, &info)) {
        lua_pushnil(L);
        return 1;
    }
    if (is("id")) lua_pushinteger(L, info.id);
    else if (is("teamId")) lua_pushinteger(L, info.team);
    else if (is("squadId")) lua_pushinteger(L, info.squad);
    else if (is("onlineId")) lua_pushinteger(L, static_cast<lua_Integer>((static_cast<uint64_t>(info.onlineHi) << 32) | info.onlineLo));
    else if (is("isSquadLeader")) lua_pushboolean(L, info.leader);
    else if (is("attached")) lua_pushinteger(L, static_cast<lua_Integer>(info.attached));
    else if (is("controlled")) lua_pushinteger(L, static_cast<lua_Integer>(info.controlled));
    else lua_pushnil(L);  // как i[k] у таблицы __clp_info: неизвестное поле — nil
    return 1;
}

/// __clp_transform(entity) -> 12 чисел (left, up, forward, trans) или nil.
int l_clp_transform(lua_State* L) {
    const uintptr_t e = static_cast<uintptr_t>(luaL_checkinteger(L, 1));
    float m[12];
    if (!ReadTransform(e, m)) {
        lua_pushnil(L);
        return 1;
    }
    for (float v : m) {
        if (!Finite(v)) {  // NaN/мусор в Lua не отдаём
            lua_pushnil(L);
            return 1;
        }
    }
    for (float v : m) lua_pushnumber(L, v);
    return 12;
}

}  // namespace

bool RegisterClientPlayers(lua_State* L) {
    lua_pushcfunction(L, l_clp_local);
    lua_setglobal(L, "__clp_local");
    lua_pushcfunction(L, l_clp_ids);
    lua_setglobal(L, "__clp_ids");
    lua_pushcfunction(L, l_clp_info);
    lua_setglobal(L, "__clp_info");
    lua_pushcfunction(L, l_clp_field);
    lua_setglobal(L, "__clp_field");
    lua_pushcfunction(L, l_clp_transform);
    lua_setglobal(L, "__clp_transform");
    return true;
}

}  // namespace evbots
