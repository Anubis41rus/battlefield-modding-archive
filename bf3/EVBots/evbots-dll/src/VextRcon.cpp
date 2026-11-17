// VextRcon.cpp — серверная библиотека VeniceEXT RCON для EVBots.
//
// Глобальная таблица RCON (server) — по документации VU (НЕ по памяти):
//   content\vext\ref\server\library\rcon.md
//
// Методы (из доки):
//   SendCommand(command: string[, args: string{}]) -> string{}
//   RegisterCommand(command: string, flags, callback: callable) -> int
//   RegisterCommand(command: string, flags, context: any, callback: callable) -> int
//   DeregisterCommand(handle: int) -> void
//   GetServerGuid() -> Guid | nil
//   TriggerEvent(event: string[, args: string{}]) -> void
//
// В реальном VU RCON — это мост к консоли сервера BF3 (и к игрокам с
// правами RCON). В нашем тест-хосте движка и RCON-подключения нет, поэтому:
//   - RegisterCommand хранит команды по-настоящему (понадобится, когда
//     появится консоль/таблица команд EVBots) и возвращает числовой handle;
//   - SendCommand возвращает пустую таблицу (нет движка — нет ответа). Скрипты
//     fun-bots обрабатывают это через fallback'и (tonumber(nil) -> nil);
//   - DeregisterCommand/TriggerEvent — заглушки; GetServerGuid -> nil.
// Сигнатуры — ровно по доке.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

#include "VextRcon.h"

#include <cctype>
#include <deque>
#include <set>

#include "EVBotsLog.h"
#include "InlineHook.h"
#include "EngineResources.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// Build 267: actual ServerSettings, shared by the engine and these commands.
// Static evidence: D:/Games-cache/royale/reports/rcon-settings-native-2026-11-12.md
// Dedicated SHA256 f39f7623063cfde9c449a0bdc9a57448d2d137d6def00b17ea04800eda955470.
// VU core 101E7AD2 writes IsDesertingAllowed +AB directly, with no message send.
// Stock 00E554C4 gates updateDeserters on +AB; 00D76A81 and other destruction
// producers gate native destruction on IsDestructionEnabled +9D.
struct RconSettingDesc { const char* field; unsigned offset; };
constexpr RconSettingDesc kRconSettings[] = {
    {"IsDesertingAllowed", 0xAB}, {"IsDestructionEnabled", 0x9D}
};

bool RconSettingsCodeMatches(uintptr_t base) {
    static const unsigned char desert[] = {0x80,0xBA,0xAB,0,0,0,0,0x0F,0x85,0x7D,0x02,0,0};
    static const unsigned char destruction[] = {0x80,0xB9,0x9D,0,0,0,0,0x75,0x2F};
    const uintptr_t cache = base + 0x02877500 - 0x00400000;
    __try {
        const auto* desertLoad = reinterpret_cast<const unsigned char*>(base + 0x00E554BE - 0x00400000);
        const auto* destructionLoad = reinterpret_cast<const unsigned char*>(base + 0x00D76A7B - 0x00400000);
        return desertLoad[0] == 0x8B && desertLoad[1] == 0x15 &&
            *reinterpret_cast<const uintptr_t*>(desertLoad + 2) == cache &&
            destructionLoad[0] == 0x8B && destructionLoad[1] == 0x0D &&
            *reinterpret_cast<const uintptr_t*>(destructionLoad + 2) == cache &&
            std::memcmp(reinterpret_cast<const void*>(base + 0x00E554C4 - 0x00400000), desert, sizeof(desert)) == 0 &&
            std::memcmp(reinterpret_cast<const void*>(base + 0x00D76A81 - 0x00400000), destruction, sizeof(destruction)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* RconSettingByte(uintptr_t base, unsigned long object, unsigned offset, int desired, bool* value) {
    __try {
        // The stock getter fills this cache lazily from the same SettingsManager.
        // A nonzero different singleton means that writing the map result would
        // not affect the consumer whose instructions were verified above.
        const auto cached = *reinterpret_cast<const unsigned long*>(base + 0x02877500 - 0x00400000);
        if (cached != 0 && cached != object) return "ServerSettings singleton differs from the native consumer";
        auto* at = reinterpret_cast<volatile unsigned char*>(static_cast<uintptr_t>(object) + offset);
        if (*at > 1) return "ServerSettings boolean contains an invalid native value";
        if (desired >= 0) {
            *at = static_cast<unsigned char>(desired);
            if (*at != desired) return "ServerSettings native write did not persist";
        }
        *value = *at != 0;
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "ServerSettings native memory is unavailable or read-only"; }
}

const char* AccessRconSetting(unsigned index, int desired, bool* value) {
    if (index >= sizeof(kRconSettings) / sizeof(kRconSettings[0]) || desired < -1 || desired > 1)
        return "Invalid native settings request";
    // External RCON is queued by DispatchDetour and pumped by Engine:Update.
    // BR calls directly inside the genuine native Level:LoadResources callback:
    // permit that TLS scope even if it is not the normal server frame thread.
    if (!EVBotsOnServerFrameThread() && !eng::IsInNativeLevelLoadResources())
        return "ServerSettings RCON requires the engine frame or native Level:LoadResources callback";
    const uintptr_t base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(L"bf3_server_Final.exe"));
    if (base == 0 || !eng::Ready()) return "Native dedicated server settings are unavailable";
    if (!RconSettingsCodeMatches(base)) return "ServerSettings consumer signatures do not match this server";
    const auto& setting = kRconSettings[index];
    const unsigned long object = eng::FindSettings("ServerSettings");
    if (object == 0) return "ServerSettings is not present in the native SettingsManager";
    const unsigned long type = eng::TypeOf(object);
    eng::TypeDesc container, fieldType;
    eng::FieldDesc field;
    if (!eng::IsKindOfName(type, "ServerSettings") || !eng::Describe(type, &container) ||
        !eng::FindField(type, setting.field, &field) || !eng::Describe(field.type, &fieldType) ||
        field.offset != setting.offset || field.offset >= container.size ||
        fieldType.kind != eng::kBoolean || fieldType.size != 1)
        return "ServerSettings native field layout/type does not match the verified boolean";
    const char* error = RconSettingByte(base, object, field.offset, desired, value);
    if (error == nullptr && desired >= 0)
        LOG_INFO("EVBots: RCON native ServerSettings.%s @%p+0x%X = %s (readback verified)",
                 setting.field, reinterpret_cast<void*>(static_cast<uintptr_t>(object)), field.offset, *value ? "true" : "false");
    return error;
}

// (field index, nil|bool) -> success, native bool | diagnostic. No fallback data.
int l_rcon_setting(lua_State* L) {
    const lua_Integer index = luaL_checkinteger(L, 1);
    if (!lua_isnoneornil(L, 2)) luaL_checktype(L, 2, LUA_TBOOLEAN);
    const int desired = lua_isnoneornil(L, 2) ? -1 : lua_toboolean(L, 2) ? 1 : 0;
    bool value = false;
    const char* error = index < 0 || index > 1 ? "Invalid native settings request" :
        AccessRconSetting(static_cast<unsigned>(index), desired, &value);
    lua_pushboolean(L, error == nullptr);
    if (error != nullptr) lua_pushstring(L, error); else lua_pushboolean(L, value);
    return 2;
}

// Чанк получает первым аргументом функцию логирования (как LuaEvents).
constexpr char kRconSource[] = R"LUA(
-- Серверный RCON VeniceEXT (VU) — по content\vext\ref\server\library\rcon.md

local log = ...

local RCON = {}
_G.RCON = RCON

-- команды: handle -> { name, context, hasContext, callback, alive }
local commands = {}
local nextHandle = 1

-- RemoteCommandFlag (shared\type\remotecommandflag.md) — значений по доке.
RemoteCommandFlag = {
  None = 0,
  RequiresLogin = 1,
  DisableOnRanked = 2,
  ReadOnlyOnRanked = 4,
  DisableAfterStartup = 8,
  Hidden = 16,
}

local function isCallable(v)
  return type(v) == "function" or (type(v) == "table" and v.__call ~= nil)
end

-- ★ Сборка 209: выполнить команду мода: (команда, {аргументы}, вошёл) -> {слова ответа}. Как VU: обработчик получает
-- (command, args, loggedIn) (с контекстом — первым), возвращает string{}; RequiresLogin без входа — «LogInRequired».
local function findCommand(name)
  local low = string.lower(name)
  for _, c in pairs(commands) do
    if c.alive and string.lower(c.name) == low then return c end
  end
  return nil
end

local function runCommand(c, name, args, loggedIn)
  if (c.flags & RemoteCommandFlag.RequiresLogin) ~= 0 and not loggedIn then
    return { "LogInRequired" }
  end
  local ok, res
  if c.hasContext then
    ok, res = pcall(c.callback, c.context, name, args, loggedIn)
  else
    ok, res = pcall(c.callback, name, args, loggedIn)
  end
  if not ok then
    log("ERR ", "команда мода «" .. tostring(name) .. "»: " .. tostring(res))
    return { "ERROR", tostring(res) }
  end
  if type(res) ~= "table" or #res == 0 then return { "OK" } end
  local out = {}
  for i = 1, #res do out[i] = tostring(res[i]) end
  return out
end

local builtinsShown = false  -- сборка 238: встроенные команды VU видны RCON сервера (EVBotsRconSetCmd может появиться позже)
local function pump()
  if EVBotsRconPoll == nil then return end
  if not builtinsShown and EVBotsRconSetCmd ~= nil then
    builtinsShown = true
    EVBotsRconSetCmd("vu.FrequencyMode", true)
    EVBotsRconSetCmd("vu.DesertingAllowed", true)
    EVBotsRconSetCmd("vu.DestructionEnabled", true)
  end
  for _ = 1, 16 do
    local id, words, loggedIn = EVBotsRconPoll()
    if id == nil then return end
    local name = words[1] or ""
    local args = {}
    for i = 2, #words do args[#args + 1] = words[i] end
    local c = findCommand(name)
    local reply = c and runCommand(c, c.name, args, loggedIn) or { "UnknownCommand" }
    log("INFO ", "RCON команда мода: " .. table.concat(words, " ") .. " -> " .. table.concat(reply, " "))
    EVBotsRconReply(id, reply)
  end
end

local pumpOn = false
local function ensurePump()
  if pumpOn then return end
  local ev = rawget(_G, "Events")
  if ev ~= nil and ev.Subscribe ~= nil then
    ev:Subscribe("Engine:Update", function() pump() end)
    pumpOn = true
  end
end

-- RegisterCommand(eventName, flags, callback) либо (..., context, callback)
function RCON:RegisterCommand(eventName, flags, ...)
  if type(eventName) ~= "string" then
    error("RCON:RegisterCommand: имя команды должно быть строкой", 2)
  end
  if type(flags) ~= "number" then
    error("RCON:RegisterCommand: флаги должны быть числами", 2)
  end

  local count = select("#", ...)
  if count < 1 then
    error("RCON:RegisterCommand: не передан обработчик", 2)
  end

  local context, callback
  if count == 1 then
    callback = (select(1, ...))
  else
    context, callback = ...
  end

  if not isCallable(callback) then
    error("RCON:RegisterCommand: обработчик должен быть функцией", 2)
  end

  local handle = nextHandle
  commands[handle] = {
    name = eventName,
    flags = flags,
    context = context,
    hasContext = count > 1,
    callback = callback,
    alive = true,
  }
  nextHandle = nextHandle + 1
  -- ★ Сборка 209: команда видна RCON сервера (как VU) — запросы приходят в очередь, их разбирает pump() на тике
  if EVBotsRconSetCmd ~= nil then EVBotsRconSetCmd(eventName, true) end
  ensurePump()

  return handle
end

function RCON:DeregisterCommand(handle)
  local c = commands[handle]
  if c ~= nil then
    c.alive = false
    commands[handle] = nil
    local still = false
    for _, o in pairs(commands) do
      if string.lower(o.name) == string.lower(c.name) then still = true break end
    end
    if not still and EVBotsRconSetCmd ~= nil then EVBotsRconSetCmd(c.name, false) end
  end
end

-- Return only a real command response. Transport failure is an explicit
-- error response, including in the standalone host; it is never a setting.
function RCON:SendCommand(command, args)
  if type(command) ~= "string" then
    error("RCON:SendCommand: команда должна быть строкой", 2)
  end
  -- ★ Сборка 209: команда мода — сразу здесь (иначе поток Lua ждал бы сам себя через сеть)
  local mc = findCommand(command)
  if mc ~= nil then
    local a = {}
    if type(args) == "table" then for i = 1, #args do a[i] = tostring(args[i]) end end
    return runCommand(mc, mc.name, a, true)
  end
  -- ★ Сборка 186: настоящий RCON сервера (127.0.0.1:47900, протокол Frostbite RCON, вход login.plainText), как VU:
  -- ответ — слова консоли сервера ({"OK", значение…}).
  if EVBotsRconSend ~= nil then
    local words = { command }
    if type(args) == "table" then
      for i = 1, #args do words[#words + 1] = tostring(args[i]) end
    end
    local ok, resp, why = pcall(EVBotsRconSend, words, 1500)
    if ok and type(resp) == "table" and #resp > 0 then
      log("DBG ", "RCON:SendCommand(\"" .. command .. "\") -> " .. table.concat(resp, " "))
      return resp
    end
    local diagnostic = tostring(ok and (why or "no command response") or resp)
    log("ERR ", "RCON:SendCommand(\"" .. command .. "\"): " .. diagnostic)
    return { "ERROR", diagnostic }
  end
  return { "ERROR", "native RCON is unavailable" }
end

function RCON:GetServerGuid()
  return nil
end

function RCON:TriggerEvent(event, args)
  if type(event) ~= "string" then
    error("RCON:TriggerEvent: имя события должно быть строкой", 2)
  end
  log("DBG ", "RCON:TriggerEvent(\"" .. tostring(event) .. "\") — заглушка")
end

-- Сборка 238: встроенная команда VU (VU-Docs hosting/commands.md) — частота сервера: regular / high60 / high120.
-- Частоту ставит ядро ev-core (EVBOTS_FREQUENCY или -high60/-high120), Lua видит её как EVBOTS_TICKRATE.
pcall(RCON.RegisterCommand, RCON, "vu.FrequencyMode", 0, function()
  local hz = rawget(_G, "EVBOTS_TICKRATE") or 30
  return { "OK", hz == 120 and "high120" or (hz == 60 and "high60" or "regular") }
end)

-- Real native booleans. The setter never reports OK for an absent engine, an
-- unsafe callback thread, a mismatched layout or an unverified write.
local nativeSetting = EVBotsRconSetting
local function registerSetting(name, index)
  RCON:RegisterCommand(name, RemoteCommandFlag.RequiresLogin, function(_, args)
    if #args > 1 then return { "InvalidArguments" } end
    local requested = nil
    if #args == 1 then
      if args[1] ~= "true" and args[1] ~= "false" then return { "InvalidArguments" } end
      requested = args[1] == "true"
    end
    local ok, value = nativeSetting(index, requested)
    if not ok then
      log("ERR ", name .. ": " .. tostring(value))
      return { "ERROR", tostring(value) }
    end
    if #args == 0 then return { "OK", value and "true" or "false" } end
    return { "OK" }
  end)
end
registerSetting("vu.DesertingAllowed", 0)
registerSetting("vu.DestructionEnabled", 1)

RCON:RegisterCommand("evbots.ReadHumanState", RemoteCommandFlag.RequiresLogin | RemoteCommandFlag.Hidden, function(_, args)
  if #args ~= 1 or args[1] == "" or args[1]:find("\0", 1, true) then return { "InvalidArguments" } end
  local bridge = rawget(_G, "NativeBridge")
  if not bridge or not bridge.ReadHumanState then return { "ERROR", "native human state readback is unavailable" } end
  local state, err = bridge.ReadHumanState(args[1])
  if not state then return { "ERROR", err } end
  local out = { "OK", tostring(state.playerPtr), tostring(state.bodyPtr), tostring(state.generation), state.hasSoldier and "true" or "false" }
  if state.hasSoldier then
    for _, row in ipairs({ "left", "up", "forward", "trans" }) do
      for _, component in ipairs({ "x", "y", "z" }) do
        out[#out + 1] = string.format("%.9g", state.transform[row][component])
      end
    end
  end
  return out
end)

return RCON
)LUA";

// ★ Сборка 186: клиент RCON сервера BF3 (Frostbite RCON). Пакет: u32 заголовок (бит 31 — от сервера, бит 30 — ответ,
// 0..29 — номер), u32 размер пакета, u32 число слов, слова: u32 длина, байты, '\0'. Вход: login.plainText <пароль>.
// Порт и пароль — как у сервера стенда (-RemoteAdminPort 127.0.0.1:47900, +admin.password); переопределение —
// EVBOTS_RCON_PORT / EVBOTS_RCON_PASSWORD. Один сокет, под мьютексом; таймаут на каждый вызов.
std::mutex g_rconMu;
SOCKET g_rconSock = INVALID_SOCKET;
uint32_t g_rconSeq = 1;
ULONGLONG g_rconRetryAt = 0;

std::string EnvOr(const char* name, const char* def) {
    char v[128] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, v, (DWORD)sizeof(v));
    return (n > 0 && n < sizeof(v)) ? std::string(v, n) : std::string(def);
}

void RconClose() {
    if (g_rconSock != INVALID_SOCKET) ::closesocket(g_rconSock);
    g_rconSock = INVALID_SOCKET;
}

bool SendAll(SOCKET s, const char* p, int n) {
    while (n > 0) {
        const int k = ::send(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

bool RecvAll(SOCKET s, char* p, int n) {
    while (n > 0) {
        const int k = ::recv(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

// Один обмен: запрос (слова) -> слова ответа. Пакеты сервера (бит 31 — события) пропускаем.
bool RconExchange(const std::vector<std::string>& words, std::vector<std::string>& out, int timeoutMs) {
    DWORD to = (DWORD)timeoutMs;
    ::setsockopt(g_rconSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    ::setsockopt(g_rconSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    const uint32_t seq = (g_rconSeq++) & 0x3FFFFFFFu;
    std::string pkt(12, '\0');
    for (const auto& w : words) {
        const uint32_t len = (uint32_t)w.size();
        pkt.append((const char*)&len, 4);
        pkt.append(w);
        pkt.push_back('\0');
    }
    const uint32_t size = (uint32_t)pkt.size(), cnt = (uint32_t)words.size();
    std::memcpy(&pkt[0], &seq, 4);
    std::memcpy(&pkt[4], &size, 4);
    std::memcpy(&pkt[8], &cnt, 4);
    if (!SendAll(g_rconSock, pkt.data(), (int)pkt.size())) return false;
    for (int guard = 0; guard < 16; ++guard) {
        uint32_t hdr[3] = {};
        if (!RecvAll(g_rconSock, (char*)hdr, 12)) return false;
        if (hdr[1] < 12 || hdr[1] > (1u << 20)) return false;
        std::vector<char> body(hdr[1] - 12);
        if (!body.empty() && !RecvAll(g_rconSock, body.data(), (int)body.size())) return false;
        const bool fromServer = (hdr[0] & 0x80000000u) != 0;
        const bool isResponse = (hdr[0] & 0x40000000u) != 0;
        if (fromServer || !isResponse || (hdr[0] & 0x3FFFFFFFu) != seq) continue;  // событие сервера или чужой ответ
        out.clear();
        size_t p = 0;
        for (uint32_t i = 0; i < hdr[2] && p + 4 <= body.size(); ++i) {
            uint32_t len = 0;
            std::memcpy(&len, &body[p], 4);
            p += 4;
            if (p + len > body.size()) break;
            out.emplace_back(&body[p], len);
            p += len + 1;
        }
        return true;
    }
    return false;
}

bool RconConnect(int timeoutMs) {
    if (g_rconSock != INVALID_SOCKET) return true;
    const ULONGLONG now = ::GetTickCount64();
    if (now < g_rconRetryAt) return false;
    g_rconRetryAt = now + 10000;  // неудача — повтор не чаще раза в 10 с
    static bool s_wsa = [] {
        WSADATA w;
        return ::WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }();
    if (!s_wsa) return false;
    const std::string port = EnvOr("EVBOTS_RCON_PORT", "47900");
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)std::atoi(port.c_str()));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
    ::connect(s, (sockaddr*)&a, sizeof(a));
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (::select(0, nullptr, &wf, nullptr, &tv) != 1) {
        ::closesocket(s);
        return false;
    }
    nb = 0;
    ::ioctlsocket(s, FIONBIO, &nb);
    g_rconSock = s;
    std::vector<std::string> resp;
    const std::string pw = EnvOr("EVBOTS_RCON_PASSWORD", "change-this-rcon-password");
    if (!RconExchange({"login.plainText", pw}, resp, timeoutMs) || resp.empty() || resp[0] != "OK") {
        LogPrintf("WARN", "EVBots: RCON: вход на 127.0.0.1:%s не удался (%s)", port.c_str(),
                  resp.empty() ? "нет ответа" : resp[0].c_str());
        RconClose();
        return false;
    }
    g_rconRetryAt = 0;
    LogPrintf("INFO", "EVBots: RCON: подключен к серверу 127.0.0.1:%s (вход OK) — SendCommand настоящий, как VU", port.c_str());
    return true;
}

/// EVBotsRconSend({слова}, таймаут мс) -> {слова ответа} | nil, ошибка
bool RconInProcess(const std::vector<std::string>& words, std::vector<std::string>& out);  // ★ 239 (А2), ниже

bool RconNativeContext() {
    // Level:LoadResources is a synchronous engine callback on the loading
    // thread, which is distinct from the frame thread. A Lua event name alone
    // does not grant this scope. Stock command handlers execute before return.
    return EVBotsOnServerFrameThread() || eng::IsInNativeLevelLoadResources();
}

int l_rcon_send(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const int timeoutMs = (int)luaL_optinteger(L, 2, 1500);
    std::vector<std::string> words;
    const lua_Integer n = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= n && i <= 64; ++i) {
        lua_geti(L, 1, i);
        size_t len = 0;
        const char* w = lua_tolstring(L, -1, &len);
        words.emplace_back(w != nullptr ? std::string(w, len) : std::string());
        lua_pop(L, 1);
    }
    if (words.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, "пустая команда");
        return 2;
    }
    std::vector<std::string> resp;
    const ULONGLONG t0 = ::GetTickCount64();
    // ★ 239 (А2): в потоке кадра движка — как VU, команда исполняется в процессе (диспетчер RCON сервера напрямую);
    // по TCP к себе из кадра ответа нет: сервер разбирает RCON в этом же потоке.
    const bool onFrame = RconNativeContext();
    if (!onFrame) {
        static std::atomic<int> s_off{0};
        if (s_off.fetch_add(1) < 6)
            LOG_INFO("EVBots: RCON: SendCommand(\"%s\") не из потока кадра — поток %lu (кадр: %s)", words[0].c_str(),
                     ::GetCurrentThreadId(), EVBotsServerFrameTickActive() ? "тик из кадра" : "таймер");
    }
    const bool ok = EVBotsRconCommand(words, resp, timeoutMs);
    // ★ 238-3 (А1): из кадра движка RCON к своему серверу может ждать сам себя (если сервер разбирает RCON в этом потоке) —
    // замер для прогона с EVBOTS_SERVER_TICK=frame: сколько ждали и пришёл ли ответ.
    if (onFrame && !ok) {
        LogPrintf("WARN", "EVBots: RCON: ответ на \"%s\" за %llu мс в потоке кадра — НЕТ", words[0].c_str(),
                  ::GetTickCount64() - t0);
    }
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "RCON недоступен");
        return 2;
    }
    lua_createtable(L, (int)resp.size(), 0);
    for (size_t i = 0; i < resp.size(); ++i) {
        lua_pushlstring(L, resp[i].data(), resp[i].size());
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    return 1;
}

int l_rcon_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: RCON: %s", text);
    return 0;
}

// ── ★ Сборка 209: команды модов через RCON сервера, как VU ────────────────────────────────────────────────────────────
// Разбор exe (capstone, 20.10; ext\AGENT-REPORT-rcon-dispatch-2026-10-20.md + сверка): все команды (сеть, Startup.txt)
// идут через диспетчер 0x00DB7F40 (thiscall(admin, Packet* pkt, int flag), ret 8; «UnknownCommand» — только в нём).
// Слова: i-е = [pkt+0x38] + ((u32*)[pkt+0x3FC8])[i], число = ([pkt+0x3FCC]-[pkt+0x3FC8])>>2; соединение [pkt+0x28]
// (вход: [conn+0x6C]&1). Ответ — 0x00DB7570 (ret 8; this не читает), из исходного пакета берёт только [+0x28] соединение,
// [+0x2C] байт и [+0x30] номер, слова — из объекта: [+0]/[+4] символы (u32 длина, строка, NUL), [+0x3F90]/[+0x3F94]
// смещения строк. Lua у нас в своём потоке — синхронно звать нельзя: перехват кладёт запрос в очередь, Lua на тике
// (Engine:Update) вызывает обработчик мода, ответ отправляет хук тика движка (EVBotsRconDrainReplies).
struct RconReq {
    long id;
    std::vector<std::string> words;
    bool loggedIn;
};
struct RconPending {
    unsigned long conn;
    unsigned char b2c;
    unsigned long seq;
};
struct RconRep {
    RconPending to;
    std::vector<std::string> words;
};
SRWLOCK g_inLock = SRWLOCK_INIT;
std::set<std::string> g_modCmds;      // имена команд модов, в нижнем регистре
std::deque<RconReq> g_inQ;
std::deque<RconRep> g_outQ;
std::vector<std::pair<long, RconPending>> g_waiting;
long g_nextReq = 1;
InlineHook g_hookDispatch;
void* g_trampDispatch = nullptr;

std::string Lower(const char* s) {
    std::string r = s != nullptr ? s : "";
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

struct PktView {
    const char* base;
    const unsigned long* offs;
    long n;
    unsigned long conn;
    unsigned char b2c;
    unsigned long seq;
    bool loggedIn;
    const char* word[64];
};
// Только чтение полей пакета (под SEH, без объектов C++).
bool ReadPkt(const unsigned char* pkt, PktView* v) {
    __try {
        v->offs = *(unsigned long* const*)(pkt + 0x3FC8);
        const unsigned long* e = *(unsigned long* const*)(pkt + 0x3FCC);
        v->base = *(const char* const*)(pkt + 0x38);
        v->n = (v->offs != nullptr && e != nullptr && e > v->offs) ? (long)(e - v->offs) : 0;
        if (v->n <= 0 || v->n >= 64 || v->base == nullptr) return false;
        for (long i = 0; i < v->n; ++i) {
            v->word[i] = v->base + v->offs[i];
            volatile char probe = v->word[i][0];  // строка читается
            (void)probe;
        }
        const unsigned char* conn = *(unsigned char* const*)(pkt + 0x28);
        v->conn = (unsigned long)(ULONG_PTR)conn;
        v->loggedIn = conn == nullptr || (conn[0x6C] & 1) != 0;  // Startup.txt (без соединения) — как вход
        v->b2c = pkt[0x2C];
        v->seq = *(const unsigned long*)(pkt + 0x30);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using DispatchFn = void(__fastcall*)(void*, void*, unsigned char*, int);
void __fastcall DispatchDetour(void* self, void* edx, unsigned char* pkt, int flag) {
    PktView v{};
    bool mine = false;
    if (ReadPkt(pkt, &v)) {
        const std::string name = Lower(v.word[0]);
        AcquireSRWLockShared(&g_inLock);
        mine = g_modCmds.count(name) != 0;
        ReleaseSRWLockShared(&g_inLock);
        if (mine) {
            RconReq r;
            for (long i = 0; i < v.n; ++i) r.words.emplace_back(v.word[i]);
            r.loggedIn = v.loggedIn;
            const RconPending to{v.conn, v.b2c, v.seq};
            AcquireSRWLockExclusive(&g_inLock);
            r.id = g_nextReq++;
            g_waiting.emplace_back(r.id, to);
            if (g_waiting.size() > 512) g_waiting.erase(g_waiting.begin());
            if (g_inQ.size() < 256) g_inQ.push_back(std::move(r));
            ReleaseSRWLockExclusive(&g_inLock);
        }
    }
    if (mine) return;  // команда мода: ответит Lua через очередь
    ((DispatchFn)g_trampDispatch)(self, edx, pkt, flag);
}

using SendFn = void(__fastcall*)(void*, void*, void*, void*);
bool SendWordsSeh(void* orig, void* words) {
    __try {
        ((SendFn)(ULONGLONG)0x00DB7570UL)(nullptr, nullptr, orig, words);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CheckBytesSeh(const void* at, const unsigned char* want, size_t n) {
    __try {
        return ::memcmp(at, want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// EVBotsRconSetCmd(имя, вкл) — команда мода видна RCON сервера
int l_rcon_set_cmd(lua_State* L) {
    const std::string name = Lower(luaL_checkstring(L, 1));
    const bool on = lua_toboolean(L, 2) != 0;
    AcquireSRWLockExclusive(&g_inLock);
    if (on) g_modCmds.insert(name); else g_modCmds.erase(name);
    ReleaseSRWLockExclusive(&g_inLock);
    return 0;
}

/// EVBotsRconPoll() -> id, {слова}, вошёл | nil
int l_rcon_poll(lua_State* L) {
    RconReq r;
    bool have = false;
    AcquireSRWLockExclusive(&g_inLock);
    if (!g_inQ.empty()) { r = std::move(g_inQ.front()); g_inQ.pop_front(); have = true; }
    ReleaseSRWLockExclusive(&g_inLock);
    if (!have) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, r.id);
    lua_createtable(L, (int)r.words.size(), 0);
    for (size_t i = 0; i < r.words.size(); ++i) {
        lua_pushlstring(L, r.words[i].data(), r.words[i].size());
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    lua_pushboolean(L, r.loggedIn ? 1 : 0);
    return 3;
}

/// EVBotsRconReply(id, {слова})
int l_rcon_reply(lua_State* L) {
    const long id = (long)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    RconRep rep{};
    const lua_Integer n = luaL_len(L, 2);
    for (lua_Integer i = 1; i <= n && i <= 64; ++i) {
        lua_geti(L, 2, i);
        size_t len = 0;
        const char* w = luaL_tolstring(L, -1, &len);
        rep.words.emplace_back(w != nullptr ? std::string(w, len) : std::string());
        lua_pop(L, 2);
    }
    if (rep.words.empty()) rep.words.emplace_back("OK");
    bool found = false;
    AcquireSRWLockExclusive(&g_inLock);
    for (size_t i = 0; i < g_waiting.size(); ++i) {
        if (g_waiting[i].first == id) {
            rep.to = g_waiting[i].second;
            g_waiting.erase(g_waiting.begin() + (long)i);
            found = true;
            break;
        }
    }
    if (found && rep.to.conn != 0 && g_outQ.size() < 256) g_outQ.push_back(std::move(rep));
    ReleaseSRWLockExclusive(&g_inLock);
    return 0;
}

// ── ★ 239 (реестр А2): RCON:SendCommand в процессе, как VU ──────────────────────────────────────────────────────────────
// VU исполняет RCON:SendCommand внутри сервера. Нам это нужно для тика Lua из кадра движка (А1): по TCP к себе из кадра
// ответа нет — сервер разбирает RCON в этом же потоке (прогон 238-6). Разбор exe (28.10, capstone): диспетчер 0x00DB7F40
// thiscall(RconServer, Packet*, int) — второй аргумент не читается (слот служит локальной); RconServer — this в
// RconServer::onMessage 0x01152A00 (вызов 0x01152F51: ecx = edi = this). Пакет: [+0x28] соединение, [+0x2C] байт,
// [+0x30] номер, с +0x38 объект слов ([+0] начало символов, [+4] конец, [+0x3F90]/[+0x3F94] смещения; строка — u32
// длина, символы, NUL). Пустое соединение нельзя: 0x00DB8046 читает [conn+0x6C] (вход) и [conn+0x68] (уровень прав) —
// даём своё: вход есть, ограничение наименьшее. Все ответы — 0x00DB7EC0 → 0x00DB7570(пакет, объект слов) синхронно; ответ
// на наш пакет перехватываем там и не отправляем. Команды, запоминающие соединение (события, вход, выход), — не в процессе.
std::atomic<void*> g_rconServer{nullptr};
void* g_trampSendWords = (void*)(ULONGLONG)0x00DB7570UL;  // подменяем вызов, сама функция не тронута
std::atomic<bool> g_sendWordsHookInstalled{false};
struct InprocReply {
    unsigned char* packet;
    bool got = false;
    std::vector<std::string> words;
};
thread_local InprocReply* t_inprocReply = nullptr;

// Только чтение объекта слов ответа (под SEH, без объектов C++): указатели и длины во временные массивы потока.
bool ReadWordsObjSeh(const unsigned char* w, const char** word, unsigned long* len, long* count) {
    __try {
        const char* b = *(const char* const*)(w + 0);
        const unsigned long* o = *(const unsigned long* const*)(w + 0x3F90);
        const unsigned long* e = *(const unsigned long* const*)(w + 0x3F94);
        long n = (o != nullptr && e != nullptr && e >= o) ? (long)(e - o) : -1;
        if (n < 0 || n > 64 || (n > 0 && b == nullptr)) return false;
        for (long i = 0; i < n; ++i) {
            word[i] = b + o[i];
            len[i] = *(const unsigned long*)(b + o[i] - 4);
            if (len[i] > 0x10000 || word[i][len[i]] != '\0') return false;
        }
        *count = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyReplyWordSeh(char* destination, const char* source, size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CaptureReply(const unsigned char* words, std::vector<std::string>& out) {
    const char* word[64];
    unsigned long len[64];
    long count = 0;
    if (!ReadWordsObjSeh(words, word, len, &count)) return false;
    std::vector<std::string> copy;
    for (long i = 0; i < count; ++i) {
        copy.emplace_back(len[i], '\0');
        if (!CopyReplyWordSeh(copy.back().data(), word[i], len[i])) return false;
    }
    out.swap(copy);
    return true;
}

using SendWordsFn = void(__fastcall*)(void*, void*, void*, void*);
void __fastcall SendWordsDetour(void* self, void* edx, void* orig, void* words) {
    if (t_inprocReply != nullptr && orig == t_inprocReply->packet) {
        // Own the response while the handler's local words buffer is alive.
        // Saving its pointers until after dispatcher return is a use-after-free.
        t_inprocReply->got = CaptureReply((const unsigned char*)words, t_inprocReply->words);
        return;  // ответ на команду в процессе — не в сеть
    }
    ((SendWordsFn)g_trampSendWords)(self, edx, orig, words);
}

bool CallDispatchSeh(void* fn, void* server, unsigned char* pkt) {
    __try {
        ((DispatchFn)fn)(server, nullptr, pkt, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SendWordsEntryMatches(const unsigned char* fn) {
    static const unsigned char original[10]={0xB8,0x34,0x6A,0,0,0xE8,0x16,0xCC,0x7A,0};
    if (!g_sendWordsHookInstalled.load()) return CheckBytesSeh(fn,original,sizeof(original));
    unsigned char entry[5]={0xE9};
    *reinterpret_cast<LONG*>(entry+1)=static_cast<LONG>(reinterpret_cast<ULONG_PTR>(&SendWordsDetour)-reinterpret_cast<ULONG_PTR>(fn+5));
    unsigned char trampoline[10]={0xB8,0x34,0x6A,0,0,0xE9};
    *reinterpret_cast<LONG*>(trampoline+6)=static_cast<LONG>(reinterpret_cast<ULONG_PTR>(fn+5)-(reinterpret_cast<ULONG_PTR>(g_trampSendWords)+10));
    return CheckBytesSeh(fn,entry,sizeof(entry)) && CheckBytesSeh(fn+5,original+5,5) &&
           CheckBytesSeh(g_trampSendWords,trampoline,sizeof(trampoline));
}

bool DispatchEntryMatches(const unsigned char* dispatcher,const unsigned char* unknown,const unsigned char* send) {
    static const unsigned char prologue[13]={0x83,0xEC,0x10,0x53,0x55,0x56,0x8B,0x74,0x24,0x20,0x57,0x8B,0xF9};
    static const unsigned char unknownCommand[5]={0x68,0x20,0x59,0x5B,0x02};
    return CheckBytesSeh(dispatcher,prologue,sizeof(prologue)) &&
           CheckBytesSeh(unknown,unknownCommand,sizeof(unknownCommand)) && SendWordsEntryMatches(send);
}

// Отправка ответа 0x00DB7570 — около 75 прямых вызовов (обработчики vars.*, mapList.* и др. зовут её сами, не через
// 0x00DB7EC0 — прогон 28.10 09:17: ответа не было). Перехват на входе: пролог «mov eax,0x6A34 / call (выделение стека)» —
// ядро крадёт 8 байт и задело бы относительный call, поэтому свой 5-байтовый jmp поверх одной целой инструкции mov;
// трамплин — та же mov и jmp на 0x00DB7575. Сверка 10 байт по exe (28.10). Ставится из потока кадра (RCON разбирает он же).
bool InstallSendWordsHookOnce() {
    static volatile LONG s_state = 0;  // 0 — не пробовали, 1 — стоит, -1 — отказ
    const LONG st = InterlockedCompareExchange(&s_state, 0, 0);
    if (st != 0) return st > 0 && SendWordsEntryMatches(reinterpret_cast<const unsigned char*>(0x00DB7570UL));
    unsigned char* fn = (unsigned char*)(ULONGLONG)0x00DB7570UL;
    static const unsigned char kPro[10] = {0xB8, 0x34, 0x6A, 0x00, 0x00, 0xE8, 0x16, 0xCC, 0x7A, 0x00};
    unsigned char* tramp = (unsigned char*)::VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    DWORD old = 0;
    if (tramp == nullptr || !CheckBytesSeh(fn, kPro, sizeof(kPro)) ||
        !::VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
        InterlockedExchange(&s_state, -1);
        LOG_ERR("EVBots: RCON: отправка ответа 0x00DB7570 не совпала с exe — RCON в процессе выключен");
        return false;
    }
    std::memcpy(tramp, kPro, 5);  // mov eax,0x6A34
    tramp[5] = 0xE9;              // jmp 0x00DB7575
    *(LONG*)(tramp + 6) = (LONG)((ULONG_PTR)(fn + 5) - (ULONG_PTR)(tramp + 10));
    ::FlushInstructionCache(::GetCurrentProcess(), tramp, 16);
    g_trampSendWords = tramp;
    unsigned char jmp[5] = {0xE9, 0, 0, 0, 0};
    *(LONG*)(jmp + 1) = (LONG)((ULONG_PTR)&SendWordsDetour - (ULONG_PTR)(fn + 5));
    std::memcpy(fn, jmp, 5);
    ::VirtualProtect(fn, 5, old, &old);
    ::FlushInstructionCache(::GetCurrentProcess(), fn, 5);
    g_sendWordsHookInstalled.store(true);
    InterlockedExchange(&s_state, 1);
    LOG_INFO("EVBots: RCON: ★ RCON:SendCommand в процессе (диспетчер 0x00DB7F40, ответ — перехват 0x00DB7570), как VU");
    return true;
}

// RconServer до первого onMessage (fun-bots зовёт RCON:SendCommand на загрузке уровня раньше — прогон 28.10 09:03:10):
// объект один на сервер (создаёт 0x01153F01 → конструктор 0x01153710, vtable 0x02649BF0 в +0, размер 0x13C), ищем его один
// раз по vtable в своей частной памяти процесса. Найдено не одно — берём первое и пишем число в журнал; onMessage
// потом всё равно подставит настоящий this.
bool ScanRegionSeh(const unsigned char* p, SIZE_T n, void** found, int* count) {
    __try {
        for (SIZE_T i = 0; i + 0x13C <= n; i += 4) {
            if (*(const unsigned long*)(p + i) == 0x02649BF0UL) {
                if (*found == nullptr) *found = (void*)(p + i);
                ++*count;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* FindRconServerOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedExchange(&s_done, 1) != 0) return g_rconServer.load();
    const ULONGLONG t0 = ::GetTickCount64();
    void* found = nullptr;
    int count = 0;
    unsigned char* a = nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    while (::VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) != 0 &&
            (mbi.Protect & PAGE_GUARD) == 0)
            ScanRegionSeh((const unsigned char*)mbi.BaseAddress, mbi.RegionSize, &found, &count);
        unsigned char* next = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a || (ULONG_PTR)next >= 0x7FFF0000UL) break;
        a = next;
    }
    LOG_INFO("EVBots: RCON: RconServer по vtable 0x02649BF0 — %p (совпадений %d, %llu мс)", found, count,
             ::GetTickCount64() - t0);
    if (found != nullptr && g_rconServer.load() == nullptr) g_rconServer.store(found);
    return g_rconServer.load();
}

bool RconInProcess(const std::vector<std::string>& words, std::vector<std::string>& out) {
    if (::GetModuleHandleW(L"bf3_server_Final.exe") == nullptr || !RconNativeContext()) return false;
    void* server = g_rconServer.load();
    if (server == nullptr) server = FindRconServerOnce();
    if (server == nullptr || words.empty() || words.size() > 60) return false;
    const std::string name = Lower(words[0].c_str());
    if (name.rfind("login.", 0) == 0 || name == "logout" || name == "quit" || name == "admin.eventsenabled") return false;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_RCON_INPROC", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') return false;
    if (!InstallSendWordsHookOnce()) return false;
    std::string chars;
    std::vector<unsigned long> offs;
    for (const auto& w : words) {
        const unsigned long len = (unsigned long)w.size();
        chars.append((const char*)&len, 4);
        offs.push_back((unsigned long)chars.size());
        chars.append(w);
        chars.push_back('\0');
    }
    unsigned char pkt[0x4000] = {};
    unsigned char conn[0x400] = {};
    static std::atomic<unsigned long> seq{0x40000000UL};
    conn[0x6C] = 1;                               // вход выполнен
    // ограничение соединения: 0x00DB8072 пускает команду, если её уровень >= [conn+0x68] (jge) — наименьшее значение
    *(long*)(conn + 0x68) = (long)0x80000000UL;
    *(unsigned char**)(pkt + 0x28) = conn;
    *(unsigned long*)(pkt + 0x30) = ++seq;
    unsigned char* w = pkt + 0x38;
    *(const char**)(w + 0) = chars.data();
    *(const char**)(w + 4) = chars.data() + chars.size();
    *(const char**)(w + 8) = chars.data() + chars.size();
    *(const unsigned long**)(w + 0x3F90) = offs.data();
    *(const unsigned long**)(w + 0x3F94) = offs.data() + offs.size();
    *(const unsigned long**)(w + 0x3F98) = offs.data() + offs.size();
    void* fn = g_trampDispatch != nullptr ? g_trampDispatch : (void*)(ULONGLONG)0x00DB7F40UL;
    InprocReply reply{pkt};
    InprocReply* previous = t_inprocReply;
    t_inprocReply = &reply;
    const bool called = CallDispatchSeh(fn, server, pkt);
    t_inprocReply = previous;
    static std::atomic<int> logged{0};
    if (!called || !reply.got) {
        if (logged.fetch_add(1) < 5)
            LOG_WARN("EVBots: RCON: команда \"%s\" в процессе — %s", words[0].c_str(),
                     called ? "ответа не было" : "исключение в движке");
        return false;
    }
    out.swap(reply.words);
    const bool okResp = !out.empty() && out[0] == "OK";
    if (!okResp) {  // отказ движка — всегда в журнал (прогон 28.10: squad.leader без ответа в журнале)
        std::string all;
        for (const auto& word : words) {
            if (!all.empty()) all += ' ';
            all += word;
        }
        LOG_WARN("EVBots: RCON: в процессе \"%s\" -> %s", all.c_str(), out.empty() ? "(пусто)" : out[0].c_str());
    } else if (logged.fetch_add(1) < 5) {
        LOG_INFO("EVBots: RCON: в процессе \"%s\" -> OK%s", words[0].c_str(), out.size() > 1 ? " …" : "");
    }
    return true;
}

}  // namespace

/// ★ 239 (А2): RconServer — из RconServer::onMessage (VextPlayerManager.cpp), для RCON:SendCommand в процессе.
void EVBotsRconNoteServer(void* server) {
    if (server != nullptr) g_rconServer.store(server);
}

/// ★ 239 (А2): тот же обмен для кода C++ в потоке кадра (EVBotsRconCommand).
bool EVBotsRconInProcess(const std::vector<std::string>& words, std::vector<std::string>& out) {
    return RconInProcess(words, out);
}

void EVBotsRconInstallDispatchHookOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_RCON_MODCMDS", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') {
        LOG_INFO("EVBots: RCON: команды модов через RCON сервера выключены (EVBOTS_RCON_MODCMDS=0)");
        return;
    }
    unsigned char* t = (unsigned char*)(ULONGLONG)0x00DB7F40UL;
    // sub esp,10 / push ebx / push ebp / push esi / mov esi,[esp+20] / push edi / mov edi,ecx
    // и «UnknownCommand» внутри: push 0x025B5920 (0x00DB7FFF)
    // и отправка ответа 0x00DB7570: mov eax,0x6A34 / call (выделение стека)
    const bool same = DispatchEntryMatches(t,reinterpret_cast<const unsigned char*>(0x00DB7FFFUL),
                                          reinterpret_cast<const unsigned char*>(0x00DB7570UL));
    if (!same) {
        LOG_ERR("EVBots: RCON: диспетчер 0x00DB7F40 — байты не совпали, команды модов через RCON НЕ включаю");
        return;
    }
    if (g_hookDispatch.Install(t, (void*)&DispatchDetour, "VextRcon:g_hookDispatch") != HookResult::Ok) {
        LOG_ERR("EVBots: RCON: перехват диспетчера не встал");
        return;
    }
    g_trampDispatch = g_hookDispatch.Trampoline();
    LOG_INFO("EVBots: RCON: ★ команды модов (RCON:RegisterCommand) доступны через RCON сервера (диспетчер 0x00DB7F40), как VU");
}

void EVBotsRconDrainReplies() {
    if (g_trampDispatch == nullptr) return;
    for (int k = 0; k < 16; ++k) {
        RconRep rep;
        bool have = false;
        AcquireSRWLockExclusive(&g_inLock);
        if (!g_outQ.empty()) { rep = std::move(g_outQ.front()); g_outQ.pop_front(); have = true; }
        ReleaseSRWLockExclusive(&g_inLock);
        if (!have) return;
        // слова в формате пакета: u32 длина, строка, NUL; смещения — на начало строки
        std::string chars;
        std::vector<unsigned long> offs;
        for (const auto& w : rep.words) {
            const unsigned long len = (unsigned long)w.size();
            chars.append((const char*)&len, 4);
            offs.push_back((unsigned long)chars.size());
            chars.append(w);
            chars.push_back('\0');
        }
        static unsigned char words[0x3FA0];
        std::memset(words, 0, sizeof(words));
        *(const char**)(words + 0) = chars.data();
        *(const char**)(words + 4) = chars.data() + chars.size();
        *(const char**)(words + 8) = chars.data() + chars.size();
        *(const unsigned long**)(words + 0x3F90) = offs.data();
        *(const unsigned long**)(words + 0x3F94) = offs.data() + offs.size();
        *(const unsigned long**)(words + 0x3F98) = offs.data() + offs.size();
        unsigned char orig[0x40] = {};
        *(unsigned long*)(orig + 0x28) = rep.to.conn;
        orig[0x2C] = rep.to.b2c;
        *(unsigned long*)(orig + 0x30) = rep.to.seq;
        if (!SendWordsSeh(orig, words)) LOG_ERR("EVBots: RCON: ответ команды мода — исключение при отправке");
    }
}

// ★ Сборка 198: тот же обмен для кода C++ (первый Level:Loaded — раунд из mapList.getRounds).
bool EVBotsRconCommand(const std::vector<std::string>& words, std::vector<std::string>& out, int timeoutMs) {
    out.clear();
    if (words.empty() || ::GetModuleHandleW(L"bf3_server_Final.exe") == nullptr) return false;
    // Never retry a native failure through TCP to ourselves. The native caller
    // can be holding the engine/loading thread (and the Lua lock); waiting for
    // that same thread cannot complete and may execute a timed-out setter later.
    if (RconNativeContext()) return RconInProcess(words, out);
    std::lock_guard<std::mutex> lk(g_rconMu);
    if (!RconConnect(timeoutMs)) return false;
    if (!RconExchange(words, out, timeoutMs)) {
        RconClose();
        return false;
    }
    return true;
}

bool RegisterVextRcon(lua_State* L) {
    if (luaL_loadbufferx(L, kRconSource, sizeof(kRconSource) - 1, "=[vext-rcon]",
                         "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: RCON не собрался: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_rcon_send);  // сборка 186: настоящий RCON сервера
    lua_setglobal(L, "EVBotsRconSend");
    lua_pushcfunction(L, l_rcon_set_cmd);  // сборка 209: команды модов через RCON сервера
    lua_setglobal(L, "EVBotsRconSetCmd");
    lua_pushcfunction(L, l_rcon_poll);
    lua_setglobal(L, "EVBotsRconPoll");
    lua_pushcfunction(L, l_rcon_reply);
    lua_setglobal(L, "EVBotsRconReply");
    lua_pushcfunction(L, l_rcon_setting);
    lua_setglobal(L, "EVBotsRconSetting");
    lua_pushcfunction(L, l_rcon_log);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: RCON не выполнился: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: RCON подключен (сервер)");
    return true;
}

}  // namespace evbots
