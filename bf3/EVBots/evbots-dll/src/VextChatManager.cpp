// VextChatManager.cpp — серверная библиотека VeniceEXT ChatManager для
// EVBots.
//
// Строго по документации VU (НЕ по памяти):
//   content\vext\ref\server\library\chatmanager.md
//
// Методы:
//   SendMessage(message [, toTeam/toSquad/toPlayer/toPlayers]) -> bool
//   Yell(message, duration [, toTeam/toSquad/toPlayer/toPlayers]) -> bool
//
// С 159/186: всем, команде, отряду — очередь моста и отправка в потоке движка (как admin.say). С 198: игроку и списку
// игроков — RCON admin.say … player <ник>. С 199 Yell — RCON admin.yell.

#include "VextChatManager.h"

#include "EVBotsLog.h"

#include <windows.h>  // сборка 159: SRWLOCK для очереди исходящего чата

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// ★ Сборка 159: исходящий чат (EVBOTS_CHAT_SEND=1). Запросы из Lua ставим в очередь здесь; отправляет их дренаж
// в хуке тика — в потоке движка (ChatSendDrain, VextPlayerManager.cpp). Без флага — прежнее поведение (лог, true).
// Аргументы как у VU: (message) — всем; (message, teamId) — команде; (message, teamId, squadId) — отряду.
struct ChatSendReq { int kind; int team; int squad; char text[256]; };  // kind: 0 — всем, 1 — команде, 2 — отряду
SRWLOCK g_sendLock = SRWLOCK_INIT;
ChatSendReq g_sendQ[16];
int g_sendN = 0, g_sendDropped = 0;

bool ChatSendOn() {
    static int s_on = -1;
    if (s_on < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_CHAT_SEND", v, static_cast<DWORD>(sizeof(v)));
        s_on = (n == 1 && v[0] == '0') ? 0 : 1;  // сборка 186: по умолчанию ВКЛ, выключатель =0
    }
    return s_on != 0;
}

int ChatSendEnqueue(lua_State* L, const char* msg) {
    ChatSendReq r = {};
    const int t2 = lua_type(L, 2);
    if (t2 == LUA_TNUMBER) {
        r.kind = 1;
        r.team = static_cast<int>(lua_tointeger(L, 2));
        if (lua_type(L, 3) == LUA_TNUMBER) {
            r.kind = 2;
            r.squad = static_cast<int>(lua_tointeger(L, 3));
        }
    } else if (t2 != LUA_TNIL && t2 != LUA_TNONE) {
        LOG_ERR("EVBots: ChatManager:SendMessage: личные сообщения (Player или список) пока не поддержаны — не отправлено");
        lua_pushboolean(L, 0);
        return 1;
    }
    size_t i = 0;
    for (; msg != nullptr && msg[i] != '\0' && i < sizeof(r.text) - 1; ++i) r.text[i] = msg[i];
    r.text[i] = '\0';
    AcquireSRWLockExclusive(&g_sendLock);
    if (g_sendN < 16) {
        g_sendQ[g_sendN++] = r;
    } else {
        ++g_sendDropped;
    }
    ReleaseSRWLockExclusive(&g_sendLock);
    LOG_INFO("EVBots: ChatManager:SendMessage в очередь (вид %d, команда %d, отряд %d): %s", r.kind, r.team, r.squad,
             msg != nullptr ? msg : "");
    lua_pushboolean(L, 1);
    return 1;
}

constexpr char kChatManagerSource[] = R"LUA(
local chat_send_message, chat_yell, chat_log = ...

-- ChatManager — чат-сообщения и Yell-уведомления.
-- Всем / команде / отряду — очередь моста, отправка в потоке движка (как admin.say). Игроку / списку игроков —
-- RCON сервера admin.say <текст> player <ник> (сборка 198). Yell — RCON admin.yell (199).
local ChatManager = {}

local function playerName(p)
    local tp = type(p)
    if tp ~= 'table' and tp ~= 'userdata' then return nil end
    local ok, n = pcall(function() return p.name end)
    if ok and type(n) == 'string' and n ~= '' then return n end
    return nil
end

-- VU: «Limited to 127 characters» — режем по символам UTF-8, не по байтам.
local function cut127(s)
    s = tostring(s or '')
    if utf8 == nil then return string.sub(s, 1, 127) end
    local okL, n = pcall(utf8.len, s)
    if okL and n ~= nil and n > 127 then
        local off = utf8.offset(s, 128)
        if off ~= nil then s = string.sub(s, 1, off - 1) end
    end
    return s
end

-- ★ Сборка 198: SendMessage(message, toPlayer | toPlayers), как VU (chatmanager.md, перегрузки 3 и 4). Раньше — отказ
-- «личные сообщения пока не поддержаны»: fun-bots отвечает так на команды чата и «follow me» (BotManager.lua:1683–1709,
-- Commands/Chat.lua). Путь — штатный RCON сервера (тот же, что у Player:Kick и squad.leader).
local function sendToPlayers(message, list)
    if EVBotsRconSend == nil then
        chat_log('ERR ', 'ChatManager:SendMessage игроку: RCON нет — не отправлено')
        return false
    end
    local text = cut127(message)
    local sent = 0
    for i = 1, #list do
        local nm = playerName(list[i])
        if nm ~= nil then
            local ok, resp = pcall(EVBotsRconSend, { 'admin.say', text, 'player', nm }, 1500)
            if ok and type(resp) == 'table' and resp[1] == 'OK' then
                sent = sent + 1
            else
                chat_log('WRN ', 'ChatManager:SendMessage игроку ' .. nm .. ': RCON ответил ' ..
                    (type(resp) == 'table' and table.concat(resp, ' ') or tostring(resp)))
            end
        end
    end
    chat_log('INFO', string.format('ChatManager:SendMessage игроку (%d из %d): %s', sent, #list, text))
    return sent > 0
end

function ChatManager:SendMessage(message, a, b)
    if playerName(a) ~= nil then return sendToPlayers(message, { a }) end
    if type(a) == 'table' and a[1] ~= nil and playerName(a[1]) ~= nil then return sendToPlayers(message, a) end
    return chat_send_message(message, a, b)
end

-- ★ Сборка 199: Yell как VU (chatmanager.md: message ≤255, duration float; всем / toTeam / toTeam+toSquad / toPlayer / toPlayers)
-- — RCON сервера admin.yell <текст> <длительность> <подмножество>. Длительность в секундах (BF3 RCON; гипотеза — у BFBC2 были
-- мс, проверить на экране). Нет RCON — прежний журнал (chat_yell).
local function yellRcon(words)
    if EVBotsRconSend == nil then return nil end
    local ok, resp = pcall(EVBotsRconSend, words, 1500)
    if ok and type(resp) == 'table' and resp[1] == 'OK' then return true end
    chat_log('WRN ', 'ChatManager:Yell: RCON ответил ' .. (type(resp) == 'table' and table.concat(resp, ' ') or tostring(resp)))
    return false
end

function ChatManager:Yell(message, duration, a, b)
    local text = tostring(message or '')
    if utf8 ~= nil then
        local okL, n = pcall(utf8.len, text)
        if okL and n ~= nil and n > 255 then
            local off = utf8.offset(text, 256)
            if off ~= nil then text = string.sub(text, 1, off - 1) end
        end
    end
    local secs = tostring(math.max(1, math.floor((tonumber(duration) or 5) + 0.5)))
    local r
    if playerName(a) ~= nil then
        r = yellRcon({ 'admin.yell', text, secs, 'player', playerName(a) })
    elseif type(a) == 'table' and a[1] ~= nil and playerName(a[1]) ~= nil then
        local any = false
        for i = 1, #a do
            local nm = playerName(a[i])
            if nm ~= nil and yellRcon({ 'admin.yell', text, secs, 'player', nm }) then any = true end
        end
        r = any
    elseif type(a) == 'number' and type(b) == 'number' then
        r = yellRcon({ 'admin.yell', text, secs, 'squad', tostring(math.tointeger(a) or a), tostring(math.tointeger(b) or b) })
    elseif type(a) == 'number' then
        r = yellRcon({ 'admin.yell', text, secs, 'team', tostring(math.tointeger(a) or a) })
    else
        r = yellRcon({ 'admin.yell', text, secs, 'all' })
    end
    if r == nil then return chat_yell(message, duration, a, b) end
    chat_log('INFO', 'ChatManager:Yell (' .. secs .. ' с, RCON ' .. (r and 'OK' or 'отказ') .. '): ' .. text)
    return r
end

_G.ChatManager = ChatManager
return ChatManager
)LUA";

bool RegisterVextChatManagerImpl(lua_State* L) {
    const int kSourceLen = static_cast<int>(sizeof(kChatManagerSource) - 1);
    if (luaL_loadbufferx(L, kChatManagerSource, kSourceLen,
                         "=[vextchatmanager]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: ChatManager не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    // Стек: СНАЧАЛА функция (chunk), ПОТОМ аргументы — как в VextGlobals.
    // ВНИМАНИЕ (баг 13.10): чанк зовёт chat_send_message/chat_yell ОБЫЧНЫМ
    // вызовом (без self): message в индексе 1, duration в индексе 2.
    // Старый код читал индекс 2 как message и молчал на числах — Yell
    // никогда не попадал в лог, и «нет Yell» ложно читалось как «пути
    // не загрузились». Исправлено: читаем с индекса 1.
    lua_pushcfunction(
        L, [](lua_State* L_) -> int {
            const int type = lua_type(L_, 1);
            if (type == LUA_TSTRING) {
                const char* msg = lua_tostring(L_, 1);
                if (ChatSendOn()) return ChatSendEnqueue(L_, msg);  // сборка 159
                LOG_INFO("EVBots: ChatManager:SendMessage: %s",
                         msg != nullptr ? msg : "");
            } else {
                LOG_INFO("EVBots: ChatManager:SendMessage (не строковый арг)");
            }
            lua_pushboolean(L_, 1);
            return 1;
        });
    lua_pushcfunction(
        L, [](lua_State* L_) -> int {
            const int type = lua_type(L_, 1);
            if (type == LUA_TSTRING) {
                const char* msg = lua_tostring(L_, 1);
                lua_Number dur = luaL_optnumber(L_, 2, 2.0);
                LOG_INFO("EVBots: ChatManager:Yell (%.2f c): %s", dur,
                         msg != nullptr ? msg : "");
            } else {
                LOG_INFO("EVBots: ChatManager:Yell (не строковый арг)");
            }
            lua_pushboolean(L_, 1);
            return 1;
        });
    lua_pushcfunction(
        L, [](lua_State* L_) -> int {  // сборка 198: chat_log(уровень, текст) — журнал из Lua-чанка
            const char* lvl = luaL_optstring(L_, 1, "INFO");
            const char* txt = luaL_optstring(L_, 2, "");
            if (lvl[0] == 'E') {
                LOG_ERR("EVBots: %s", txt);
            } else if (lvl[0] == 'W') {
                LOG_WARN("EVBots: %s", txt);
            } else {
                LOG_INFO("EVBots: %s", txt);
            }
            return 0;
        });
    if (lua_pcall(L, 3, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: ChatManager не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) {
        LOG_ERR("EVBots: ChatManager вернул не таблицу");
        lua_pop(L, 1);
        return false;
    }
    LOG_INFO("EVBots: ChatManager подключен (server)");
    lua_setglobal(L, "ChatManager");
    return true;
}

}  // namespace

// Дренаж (ChatSendDrain, VextPlayerManager.cpp): берёт один запрос из очереди. false — очередь пуста.
bool EVBotsChatSendPop(int* kind, int* team, int* squad, char* text, int cap) {
    AcquireSRWLockExclusive(&g_sendLock);
    if (g_sendN == 0) {
        ReleaseSRWLockExclusive(&g_sendLock);
        return false;
    }
    const ChatSendReq r = g_sendQ[0];
    for (int i = 1; i < g_sendN; ++i) g_sendQ[i - 1] = g_sendQ[i];
    --g_sendN;
    ReleaseSRWLockExclusive(&g_sendLock);
    *kind = r.kind;
    *team = r.team;
    *squad = r.squad;
    int i = 0;
    for (; i < cap - 1 && r.text[i] != '\0'; ++i) text[i] = r.text[i];
    text[i] = '\0';
    return true;
}

bool RegisterVextChatManager(lua_State* L) {
    return RegisterVextChatManagerImpl(L);
}
}  // namespace evbots
