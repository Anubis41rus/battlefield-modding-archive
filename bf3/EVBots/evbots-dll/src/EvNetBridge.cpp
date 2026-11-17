// EvNetBridge.cpp — NetEvents через канал ev-core.dll (К4, сборка 219), обе стороны.
//
// У VU канал (Harmony) живёт в vu-core, а veniceext только упаковывает NetEvents и раздаёт их обработчикам мода. У нас так
// же: транспорт — ev-core.dll (EvNet*), здесь — связь с Lua:
//   * Lua -> сеть: __evnet_send(conn, bytes) / __evnet_broadcast(bytes) (упаковка — EVNetCodec на Lua);
//   * сеть -> Lua: обработчик ev-core зовётся из его потоков — только кладём в очередь; EvNetDrain() на тике движка
//     (сервер — тик Lua, клиент — хук кадра) отдаёт очередь в NetEvents._OnNet(conn, kind, bytes) под TickGate.
// Нет ev-core.dll (тест-хост, старый комплект) — функции честно отвечают «канала нет», NetEvents пишут об этом в журнал.

#include "EvNetBridge.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "EVBotsLog.h"
#include "LuaRuntime.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace evbots {
namespace {

using Handler = void(__cdecl*)(int role, uint32_t conn, int kind, const char* data, uint32_t len);
using SetHandlerFn = void(__cdecl*)(Handler);
using ClientConnectFn = int(__cdecl*)(const char*, int);
using SendFn = int(__cdecl*)(uint32_t, const char*, uint32_t);
using BroadcastFn = int(__cdecl*)(const char*, uint32_t);

struct Core {
    SetHandlerFn setHandler = nullptr;
    ClientConnectFn clientConnect = nullptr;
    SendFn send = nullptr;
    BroadcastFn broadcast = nullptr;
    bool ok = false;
};

const Core& GetCore() {
    static const Core core = [] {
        Core c;
        HMODULE m = ::GetModuleHandleA("ev-core.dll");
        if (m == nullptr) return c;
        c.setHandler = reinterpret_cast<SetHandlerFn>(::GetProcAddress(m, "EvNetSetHandler"));
        c.clientConnect = reinterpret_cast<ClientConnectFn>(::GetProcAddress(m, "EvNetClientConnect"));
        c.send = reinterpret_cast<SendFn>(::GetProcAddress(m, "EvNetSend"));
        c.broadcast = reinterpret_cast<BroadcastFn>(::GetProcAddress(m, "EvNetBroadcast"));
        c.ok = c.setHandler && c.clientConnect && c.send && c.broadcast;
        return c;
    }();
    return core;
}

struct Incoming {
    uint32_t conn;
    int kind;
    std::string data;
};

std::mutex g_queueGate;
std::vector<Incoming> g_queue;
constexpr size_t kMaxQueue = 20000;  // защита памяти, если Lua долго не забирает

void __cdecl OnNet(int role, uint32_t conn, int kind, const char* data, uint32_t len) {
    (void)role;
    std::lock_guard<std::mutex> lock(g_queueGate);
    if (g_queue.size() >= kMaxQueue) return;
    g_queue.push_back({conn, kind, std::string(data != nullptr ? data : "", len)});
}

int l_evnet_send(lua_State* L) {
    const lua_Integer conn = luaL_checkinteger(L, 1);
    size_t n = 0;
    const char* p = luaL_checklstring(L, 2, &n);
    const Core& c = GetCore();
    lua_pushboolean(L, c.ok && conn >= 0 && c.send(static_cast<uint32_t>(conn), p, static_cast<uint32_t>(n)) == 1);
    return 1;
}

int l_evnet_broadcast(lua_State* L) {
    size_t n = 0;
    const char* p = luaL_checklstring(L, 1, &n);
    const Core& c = GetCore();
    lua_pushinteger(L, c.ok ? c.broadcast(p, static_cast<uint32_t>(n)) : 0);
    return 1;
}

int l_evnet_available(lua_State* L) {
    lua_pushboolean(L, GetCore().ok);
    return 1;
}

/// Имя игрока этого клиента для приветствия: EVBOTS_PLAYER_NAME (ставят join.ps1 / лаунчер). У VU сервер знает игрока по
/// самому соединению (вход через Zeus) — у нас до E3 (клиент EV) так (реестр В14).
int l_evnet_playername(lua_State* L) {
    char v[128]{};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_PLAYER_NAME", v, sizeof(v));
    if (n > 0 && n < sizeof(v)) lua_pushstring(L, v); else lua_pushnil(L);
    return 1;
}

}  // namespace

bool RegisterEvNetBridge(lua_State* L) {
    lua_pushcfunction(L, l_evnet_send);
    lua_setglobal(L, "__evnet_send");
    lua_pushcfunction(L, l_evnet_broadcast);
    lua_setglobal(L, "__evnet_broadcast");
    lua_pushcfunction(L, l_evnet_available);
    lua_setglobal(L, "__evnet_available");
    lua_pushcfunction(L, l_evnet_playername);
    lua_setglobal(L, "__evnet_playername");
    return true;
}

bool EvNetBridgeStart(bool client) {
    const Core& c = GetCore();
    if (!c.ok) {
        LOG_WARN("EVBots: NetEvents: ev-core.dll нет — канала клиент↔сервер не будет (только журнал)");
        return false;
    }
    c.setHandler(&OnNet);
    if (client) {
        const int r = c.clientConnect(nullptr, 0);
        LOG_INFO("EVBots: NetEvents: ★ клиент подключается к серверу через ev-core (Harmony), код %d", r);
    } else {
        LOG_INFO("EVBots: NetEvents: ★ сервер принимает клиентов через ev-core (Harmony)");
    }
    return true;
}

void EvNetDrain() {
    std::vector<Incoming> items;
    {
        std::lock_guard<std::mutex> lock(g_queueGate);
        if (!g_queue.empty()) items.swap(g_queue);
    }
    auto& lua = LuaRuntime::Instance();
    if (!lua.ready()) return;
    for (const Incoming& it : items) {
        lua.CallTableFunction("NetEvents", "_OnNet",
                              {LuaRuntime::EngineEventArg::Num(static_cast<double>(it.conn)),
                               LuaRuntime::EngineEventArg::Num(it.kind), LuaRuntime::EngineEventArg::Str(it.data)});
    }
    lua.CallTableFunction("NetEvents", "_Pump", {});
}

}  // namespace evbots
