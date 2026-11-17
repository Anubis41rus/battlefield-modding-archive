// LuaDebugConsole.cpp — глобалы VeniceEXT: Console и перехват print.
//
// Источники (документация VU, НЕ по памяти):
//   - Console: content\vext\ref\client\library\console.md (библиотека
//     есть только в клиентском API — в shared/server её нет);
//   - ConsoleCommand: content\vext\ref\client\type\consolecommand.md
//     (свойства name/description/groupName + метод Deregister()).
//
// Почему нет глобальной Debug: в VU библиотеки Debug не существует
// (в ref\shared, ref\client, ref\server её нет; live-доки тоже). Мод
// fun-bots сам объявляет глобал Debug в ext\Shared\Debug.lua и грузит
// его require('__shared/Debug') — нам добавлять нечего.
//
// Реализация: логика без доступа к игре написана на Lua и встроена
// строкой (приём MiddleClass.cpp). Методы Console тикают в игру только в
// будущем — сейчас это обёртки, которые логируют и возвращают нейтральное
// значение (пометка "TODO: связать с движком"). print модов уходит в лог
// EVBots (LOG_INFO).

#include "LuaDebugConsole.h"

#include <cstddef>
#include <string>

#include "EVBotsLog.h"
#include "LuaRuntime.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// ── сериализация значений Lua для лога ─────────────────────────────────────

void AppendLuaValue(lua_State* L, int idx, std::string& out, int depth);

// Таблица -> "ключ = значение, ..." с ограничением глубины и числа полей.
void AppendTable(lua_State* L, int idx, std::string& out, int depth) {
    idx = lua_absindex(L, idx);
    constexpr int kMaxDepth = 3;
    constexpr int kMaxItems = 32;
    if (depth >= kMaxDepth) {
        out += "{ ... }";
        return;
    }

    out += '{';
    int count = 0;
    bool first = true;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        ++count;
        if (count > kMaxItems) {
            out += "...";
            lua_pop(L, 2);  // снять значение и ключ перед прерыванием обхода
            break;
        }
        if (!first) {
            out += ", ";
        }
        first = false;

        AppendLuaValue(L, -2, out, depth + 1);  // ключ
        out += " = ";
        AppendLuaValue(L, -1, out, depth + 1);  // значение
        lua_pop(L, 1);  // значение снято, ключ остаётся для lua_next
    }
    out += '}';
}

void AppendLuaValue(lua_State* L, int idx, std::string& out, int depth) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            out += "nil";
            break;
        case LUA_TBOOLEAN:
            out += (lua_toboolean(L, idx) != 0) ? "true" : "false";
            break;
        case LUA_TNUMBER:
        case LUA_TSTRING: {
            // lua_tolstring changes numeric stack slots to strings. Never
            // convert the original key retained by lua_next.
            lua_pushvalue(L, idx);
            std::size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            if (s != nullptr) {
                out.append(s, len);
            }
            lua_pop(L, 1);
            break;
        }
        case LUA_TTABLE:
            AppendTable(L, idx, out, depth);
            break;
        default: {
            // luaL_tolstring кладёт результат (учитывая __tostring) на стек
            std::size_t len = 0;
            const char* s = luaL_tolstring(L, idx, &len);
            if (s != nullptr) {
                out.append(s, len);
            }
            lua_pop(L, 1);
            break;
        }
    }
}

/// C-функция evbotslog(level, ...): склеивает значения в одну строку и
/// пишет в лог EVBots. level — "INFO" / "WARN" / "ERR ".
///
/// Формат — как в VeniceEXT: '[VeniceEXT] [<мод>] текст' (см. доки VU-Docs
/// your-first-mod.md — именно так выглядит вывод мода в консоли сервера VU).
int l_log_message(lua_State* L) {
    const char* level = luaL_checkstring(L, 1);

    std::string msg;
    const int n = lua_gettop(L);
    for (int i = 2; i <= n; ++i) {
        if (i > 2) {
            msg += '\t';
        }
        AppendLuaValue(L, i, msg, 0);
    }
    if (msg.empty()) {
        msg = "(nothing)";
    }

    std::string modName;
    const std::wstring& modFolder = LuaRuntime::Instance().currentModFolder();
    if (!modFolder.empty()) {
        const std::size_t slash = modFolder.find_last_of(L"\\/");
        const std::wstring name =
            slash == std::wstring::npos ? modFolder : modFolder.substr(slash + 1);
        int size = ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0,
                                         nullptr, nullptr);
        if (size > 0) {
            std::string nameUtf8(static_cast<std::size_t>(size - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, &nameUtf8[0],
                                  size, nullptr, nullptr);
            modName = nameUtf8;
        }
    }

    // ★ Сборка 206: вывод мода — в logs\<мод>.log (fun-bots.log), не в EVBots.log
    LogModWrite(modName.c_str(), level != nullptr ? level : "INFO", msg.c_str());
    // ★ Сборка 217: и в консоль окна сервера, как у VU («[info] [fun-bots] …»). Вне __init__ мод не известен —
    // при одном моде берём его имя.
    if (modName.empty() && LuaRuntime::Instance().mods().size() == 1) modName = LuaRuntime::Instance().mods()[0].name;
    const char* lvl = (level != nullptr && level[0] == 'E') ? "error" : (level != nullptr && level[0] == 'W') ? "warning" : "info";
    ConsoleWrite(lvl, modName.empty() ? "VeniceEXT" : modName.c_str(), msg.c_str(), false);
    return 0;
}

/// ★ Сборка 217: evbotsconsole(level, source, text) — строка в консоль окна сервера из Lua-кода EVBots (мост: вход и
/// выход игроков). Не для модов (у VU такого API нет — моды пишут через print).
int l_console_line(lua_State* L) {
    ConsoleWrite(luaL_optstring(L, 1, "info"), luaL_optstring(L, 2, "EVBots"), luaL_optstring(L, 3, ""), true);
    return 0;
}

// ── Console и print на Lua (встроены строкой) ──────────────────────────────
//
// Логика без доступа к игре — на Lua. Методы Console в будущем будут
// тикать в реальную консоль игры, сейчас это обёртки: логируем и
// возвращаем нейтральное значение, поведение не выдумываем.

constexpr char kConsoleModuleSource[] = R"LUA(
-- Console — по ref\client\library\console.md и типу ConsoleCommand.
local commands = {}  -- name -> команда (ConsoleCommand)

local function makeCommand(name, description)
  return {
    name        = name,
    description = description,
    groupName   = '',  -- VU не документирует дефолт — оставляем пустым
    Deregister  = function(self)
      commands[self.name] = nil
    end
  }
end

Console = {}

-- Register(name, description, callback)
-- Register(name, description, context, callback)
function Console:Register(name, description, context, callback)
  -- TODO: связать с движком — зарегистрировать в реальной консоли игры.
  if callback == nil then
    callback = context
    context = nil
  end
  if commands[name] then
    evbotslog("WARN", "[Console] команда «" .. tostring(name) ..
                      "» переопределена")
  end
  local cmd = makeCommand(name, description)
  commands[name] = cmd
  evbotslog("INFO", "[Console] зарегистрирована команда «" .. tostring(name) ..
                    "»: " .. tostring(description))
  return cmd
end

-- Deregister(name) / Deregister(command) -> bool
function Console:Deregister(name)
  -- TODO: связать с движком — снять команду с реальной консоли игры.
  local key = type(name) == 'table' and name.name or name
  if commands[key] then
    commands[key] = nil
    evbotslog("INFO", "[Console] снята команда «" .. tostring(key) .. "»")
    return true
  end
  return false
end

function Console:DeregisterAll()
  -- TODO: связать с движком — снять все команды с реальной консоли.
  commands = {}
  evbotslog("INFO", "[Console] all commands removed")
end

-- Сообщения модов и print -> в лог EVBots (LOG_INFO).
_G.print = function(...) evbotslog("INFO", ...) end
)LUA";

}  // namespace

bool RegisterDebugConsole(lua_State* L) {
    if (L == nullptr) {
        return false;
    }

    // evbotslog нужен C-коду Lua-модуля (print и Console)
    lua_pushcfunction(L, l_log_message);
    lua_setglobal(L, "evbotslog");
    lua_pushcfunction(L, l_console_line);
    lua_setglobal(L, "evbotsconsole");

    static const int kSourceLen =
        static_cast<int>(sizeof(kConsoleModuleSource) - 1);
    if (luaL_loadbufferx(L, kConsoleModuleSource, kSourceLen,
                         "=[Console+print]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LogPrintf("ERR ", "EVBots: Console failed to compile: %s",
                  err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LogPrintf("ERR ", "EVBots: Console failed to run: %s",
                  err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: Console (Register/Deregister/DeregisterAll) "
             "и print -> лог");
    return true;
}

}  // namespace evbots
