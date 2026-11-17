// VextNet.cpp — библиотека VeniceEXT Net для EVBots.
//
// Глобальная таблица Net — по документации VU (НЕ по памяти):
//   content\vext\ref\shared\library\net.md
//
// В реальном VU Net — HTTP/Socket-клиент движка. В нашем тест-хосте движка
// и исходящей сети уровня VU нет, поэтому (как с RCON.SendCommand):
//   - Async-методы (GetHTTPAsync и пр.) вызывают callback с nil — скрипты
//     fun-bots обрабатывают nil корректно (UpdateCheck печатает "failed");
//   - Sync-методы (GetHTTP/HeadHTTP/...) возвращают nil;
//   - Socket -> nil.
// Сигнатуры всех перегрузок — ровно по доке. Подключу настоящий HTTP
// (WinHTTP) позже, если понадобится (например, для фидбэка в бэкенд).

#include "VextNet.h"

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
constexpr char kNetSource[] = R"LUA(
-- Net VeniceEXT (VU) — по content\vext\ref\shared\library\net.md

local log = ...

local Net = {}
_G.Net = Net

local function isCallable(v)
  return type(v) == "function" or (type(v) == "table" and v.__call ~= nil)
end

-- Находим последний callable-аргумент (callback) и вызываем его с nil:
-- сетевого клиента в тест-хосте нет. Формы с context передают его первым.
local function asyncCall(...)
  local nargs = select("#", ...)
  local args = { ... }
  local callback = nil
  local last = nargs
  while last >= 1 and not isCallable(args[last]) do
    last = last - 1
  end
  callback = args[last]

  if callback == nil then
    log("WARN", "Net: async-вызов без callback, пропускаю")
    return
  end

  log("DBG ", "Net: запроса нет (движок не подключен), callback(nil)")
  local ok, err = pcall(callback)
  if not ok then
    log("ERR ", "Net: callback упал: " .. tostring(err))
  end
end

-- ── GET ────────────────────────────────────────────────────────────────────
function Net:GetHTTP(url, ...)       -- (url[, options]) -> nil
  log("DBG ", "Net:GetHTTP(\"" .. tostring(url) .. "\") — нет сети, nil")
  return nil
end

function Net:GetHTTPAsync(url, ...)
  asyncCall(...)
end

-- ── HEAD ───────────────────────────────────────────────────────────────────
function Net:HeadHTTP(url, ...)
  log("DBG ", "Net:HeadHTTP(\"" .. tostring(url) .. "\") — нет сети, nil")
  return nil
end

function Net:HeadHTTPAsync(url, ...)
  asyncCall(...)
end

-- ── POST ───────────────────────────────────────────────────────────────────
function Net:PostHTTP(url, data, ...)
  log("DBG ", "Net:PostHTTP(\"" .. tostring(url) .. "\") — нет сети, nil")
  return nil
end

function Net:PostHTTPAsync(url, data, ...)
  asyncCall(...)
end

-- ── PUT ────────────────────────────────────────────────────────────────────
function Net:PutHTTP(url, data, ...)
  log("DBG ", "Net:PutHTTP(\"" .. tostring(url) .. "\") — нет сети, nil")
  return nil
end

function Net:PutHTTPAsync(url, data, ...)
  asyncCall(...)
end

-- ── DELETE ─────────────────────────────────────────────────────────────────
function Net:DeleteHTTP(url, ...)
  log("DBG ", "Net:DeleteHTTP(\"" .. tostring(url) .. "\") — нет сети, nil")
  return nil
end

function Net:DeleteHTTPAsync(url, ...)
  asyncCall(...)
end

-- ── OPTIONS ────────────────────────────────────────────────────────────────
function Net:OptionsHTTP(url, ...)
  log("DBG ", "Net:OptionsHTTP(\"" .. tostring(url) .. "\") — нет сети, nil")
  return nil
end

function Net:OptionsHTTPAsync(url, ...)
  asyncCall(...)
end

-- ── PATCH ──────────────────────────────────────────────────────────────────
function Net:PatchHTTP(url, data, ...)
  log("DBG ", "Net:PatchHTTP(\"" .. tostring(url) .. "\") — нет сети, nil")
  return nil
end

function Net:PatchHTTPAsync(url, data, ...)
  asyncCall(...)
end

-- ── SOCKET ─────────────────────────────────────────────────────────────────
function Net:Socket(family, socketType)
  log("DBG ", "Net:Socket(...) — сокеты не поддерживаются, nil")
  return nil
end

return Net
)LUA";

int l_net_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: Net: %s", text);
    return 0;
}

}  // namespace

bool RegisterVextNet(lua_State* L) {
    if (luaL_loadbufferx(L, kNetSource, sizeof(kNetSource) - 1, "=[vext-net]",
                         "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Net не собрался: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_net_log);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Net не выполнился: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: Net подключён");
    return true;
}

}  // namespace evbots
