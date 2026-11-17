// VextSql.cpp — серверная библиотека VeniceEXT SQL для EVBots.
//
// Реализована глобальная таблица SQL (сервер) — строго по документации VU
// (НЕ по памяти):
//   content\vext\ref\server\library\sql.md
//
// Методы (из доки):
//   Open() -> bool
//   Close() -> void
//   Query(query: string, ...args: any) -> table | nil
//   Escape(text: string) -> string
//   Blob(data: string) -> SQLBlob
//   Error() -> string
//   AffectedRows() -> int
//   LastInsertId() -> int
//
// Под капотом — SQLite (sqlite3.c, amalgamation 3.54.0), как в VU.
// Способ подключения — как у VextUtils: тут лежат «сырые» C-функции
// (аргументы без self), а обёртку с  self для методик SQL строит маленький
// Lua-чанк внизу. База: mod.db мода (как в VU), фолбэк — evbots.db рядом
// с процессом.

#include "VextSql.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "EVBotsLog.h"
#include "LuaRuntime.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "sqlite3.h"
}

namespace evbots {
namespace {

sqlite3* g_db = nullptr;
std::string g_lastError;

// В VU база для SQL лежит в папке МОДА (у fun-bots это mod.db рядом с mod.json),
// а не «рядом с сервером». Ищем mod.db в подпапках Admin\Mods (EVBOTS_MODS),
// фолбэк — evbots.db рядом с исполняемым файлом.
std::string DatabasePath() {
    wchar_t env[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"EVBOTS_MODS", env, MAX_PATH) != 0 &&
        env[0] != L'\0') {
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = std::wstring(env) + L"\\*";
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    std::wstring db = std::wstring(env) + L"\\" + fd.cFileName +
                                      L"\\mod.db";
                    const DWORD attrs = ::GetFileAttributesW(db.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES) {
                        ::FindClose(h);
                        std::string out;
                        const int size = ::WideCharToMultiByte(
                            CP_UTF8, 0, db.c_str(), -1, nullptr, 0, nullptr, nullptr);
                        if (size > 0) {
                            out.resize(size - 1);
                            ::WideCharToMultiByte(CP_UTF8, 0, db.c_str(), -1, &out[0],
                                                   size, nullptr, nullptr);
                        }
                        return out.empty() ? "mod.db" : out;
                    }
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
    }

    // тот же корень, по которому LuaRuntime нашёл моды (Admin\Mods)
    std::wstring roots[] = {LuaRuntime::DiscoverModsRoot(), L""};
    const wchar_t* kFixed[] = {
        L"C:\\Games\\BF-Portable\\Battlefield_3_Server\\Instance\\Admin\\Mods",
        nullptr,
    };
    for (const wchar_t** f = kFixed; *f != nullptr; ++f) {
        if (::GetFileAttributesW(*f) != INVALID_FILE_ATTRIBUTES) {
            roots[1] = std::wstring(*f);
            break;
        }
    }

    for (const std::wstring& root : roots) {
        if (root.empty()) {
            continue;
        }
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = root + L"\\*";
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                continue;
            }
            const std::wstring db = root + L"\\" + fd.cFileName + L"\\mod.db";
            if (::GetFileAttributesW(db.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ::FindClose(h);
                std::string out;
                const int size = ::WideCharToMultiByte(
                    CP_UTF8, 0, db.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (size > 0) {
                    out.resize(size - 1);
                    ::WideCharToMultiByte(CP_UTF8, 0, db.c_str(), -1, &out[0],
                                           size, nullptr, nullptr);
                }
                return out.empty() ? "mod.db" : out;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }

    wchar_t module[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring dir(module);
    const std::size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir.resize(slash);
    }
    dir += L"\\evbots.db";
    std::string out;
    const int size =
        ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size > 0) {
        out.resize(size - 1);
        ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, &out[0], size, nullptr, nullptr);
    }
    return out;
}

void SetLastError(const char* text) { g_lastError = text != nullptr ? text : "unknown"; }

// ── сырые C-функции (вызываются из Lua-обёртки без self) ───────────────────

// ★ Сборка 168: соединение держим открытым. fun-bots зовёт SQL:Open/SQL:Close вокруг каждого запроса (прогон 166:
// ~3000 открытий за прогон), а каждое открытие обходило папку модов в поиске mod.db и заново поднимало SQLite.
// Запись SQLite фиксирует сразу (автокоммит), поэтому SQL:Close закрывает базу только для мода (логически).
int l_sql_open(lua_State* L) {
    (void)L;
    if (g_db != nullptr) {
        lua_pushboolean(L, 1);
        return 1;
    }

    static const std::string path = DatabasePath();
    const int rc = sqlite3_open(path.c_str(), &g_db);
    if (rc != SQLITE_OK) {
        if (g_db != nullptr) {
            SetLastError(sqlite3_errmsg(g_db));
            sqlite3_close(g_db);
            g_db = nullptr;
        } else {
            SetLastError("не удалось открыть файл БД");
        }
        LOG_WARN("EVBots: SQL:Open failed (%s): %s", path.c_str(),
                 g_lastError.c_str());
        lua_pushboolean(L, 0);
        return 1;
    }

    LOG_INFO("EVBots: SQL база открыта: %s", path.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

int l_sql_close(lua_State* L) {
    (void)L;
    // сборка 168: соединение остаётся открытым (см. l_sql_open); закрывается вместе с процессом
    return 0;
}

int l_sql_escape(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    std::string out;
    out.reserve(std::char_traits<char>::length(text) * 2 + 1);
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '\'') {
            out += '\'';
        }
        out += *p;
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int l_sql_blob(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    lua_newtable(L);
    lua_pushlstring(L, data, len);
    lua_setfield(L, -2, "value");
    return 1;
}

int l_sql_error(lua_State* L) {
    (void)L;
    lua_pushstring(L, g_lastError.c_str());
    return 1;
}

int l_sql_affectedrows(lua_State* L) {
    (void)L;
    if (g_db != nullptr) {
        lua_pushinteger(L, sqlite3_changes(g_db));
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

int l_sql_lastinsertid(lua_State* L) {
    (void)L;
    if (g_db != nullptr) {
        lua_pushinteger(L, static_cast<lua_Integer>(sqlite3_last_insert_rowid(g_db)));
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

int l_sql_query(lua_State* L) {
    const char* sql = luaL_checkstring(L, 1);

    // ★ Диагностика загрузки путей fun-bots (сборка 32, 13.10.2026).
    // Стейт-машина путей мода (NodeCollection:ProcessAllDataToLoad) делает
    // ~8 запросов: шаг 0 проверяет таблицу '<map>_table', шаг 10 читает
    // страницы по 1024 вейпоинта. Её собственный логгер выключен
    // (Debug.Server.NODECOLLECTION=false в модe), поэтому по логу нельзя
    // было понять, дошёл ли мод до путей (в прогоне 31 — 0 строк INPUT,
    // ни одного Yell 'Loaded N paths').
    // Грабля (наш первый прогон 32): настройки мода делают 25+ запросов
    // к FB_Config_Trace при старте — простой счётчик «первые 25» их съедал,
    // и запросы путей в лог не попадали. Правило: ВСЕ запросы к таблицам
    // карт ('..._table') логируем отдельно (до 25), плюс первые 15 любых
    // для контекста.
    static volatile LONG g_queryLog = 0;
    static volatile LONG g_pathQueryLog = 0;
    const long qn = InterlockedIncrement(&g_queryLog);
    const bool isPathQuery = (std::strstr(sql, "_table") != nullptr);
    const long pqn = isPathQuery ? InterlockedIncrement(&g_pathQueryLog) : 0;
    const bool logQuery = (qn <= 15) || (isPathQuery && pqn <= 25);

    if (g_db == nullptr) {
        l_sql_open(L);
        const bool opened = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (!opened) {
            lua_pushnil(L);
            return 1;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        const char* err = g_db != nullptr ? sqlite3_errmsg(g_db) : "?";
        SetLastError(err);
        LOG_WARN("EVBots: SQL:Query prepare failed: %s", err);
        lua_pushnil(L);
        return 1;
    }

    const int total = lua_gettop(L) - 1;
    for (int i = 0; i < total; ++i) {
        const int idx = i + 2;
        switch (lua_type(L, idx)) {
            case LUA_TNUMBER:
                if (lua_isinteger(L, idx)) {
                    sqlite3_bind_int64(stmt, i + 1, lua_tointeger(L, idx));
                } else {
                    sqlite3_bind_double(stmt, i + 1, lua_tonumber(L, idx));
                }
                break;
            case LUA_TBOOLEAN:
                sqlite3_bind_int(stmt, i + 1, lua_toboolean(L, idx));
                break;
            case LUA_TSTRING: {
                size_t len = 0;
                const char* s = lua_tolstring(L, idx, &len);
                sqlite3_bind_text(stmt, i + 1, s, static_cast<int>(len),
                                  SQLITE_TRANSIENT);
                break;
            }
            case LUA_TNIL:
                sqlite3_bind_null(stmt, i + 1);
                break;
            default:
                break;
        }
    }

    lua_newtable(L);          // -1: итоговая таблица
    int outIndex = 1;
    rc = sqlite3_step(stmt);
    while (rc == SQLITE_ROW) {
        lua_newtable(L);
        const int nCols = sqlite3_column_count(stmt);
        for (int c = 0; c < nCols; ++c) {
            const char* colName = sqlite3_column_name(stmt, c);
            if (colName == nullptr) {
                continue;
            }
            const int type = sqlite3_column_type(stmt, c);
            switch (type) {
                case SQLITE_INTEGER:
                    lua_pushinteger(L, sqlite3_column_int64(stmt, c));
                    break;
                case SQLITE_FLOAT:
                    lua_pushnumber(L, sqlite3_column_double(stmt, c));
                    break;
                case SQLITE_TEXT: {
                    const unsigned char* s = sqlite3_column_text(stmt, c);
                    lua_pushstring(L, reinterpret_cast<const char*>(s));
                    break;
                }
                case SQLITE_BLOB: {
                    const void* b = sqlite3_column_blob(stmt, c);
                    const int blen = sqlite3_column_bytes(stmt, c);
                    lua_pushlstring(L, static_cast<const char*>(b),
                                    static_cast<std::size_t>(blen));
                    break;
                }
                case SQLITE_NULL:
                    lua_pushnil(L);
                    break;
                default:
                    lua_pushnil(L);
                    break;
            }
            lua_setfield(L, -2, colName);
        }
        lua_rawseti(L, -2, outIndex++);
        rc = sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        const char* err = g_db != nullptr ? sqlite3_errmsg(g_db) : "?";
        SetLastError(err);
        LOG_WARN("EVBots: SQL:Query step failed: %s", err);
        if (logQuery) {
            LOG_INFO("SQL:Query #%ld: %.60s -> ОШИБКА", qn, sql);
        }
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }

    if (logQuery) {
        // %.60s — обрезаем длинные запросы страниц; важно видеть имя таблицы
        // (например MP_001_ConquestSmall0_table) и число строк результата.
        LOG_INFO("SQL:Query #%ld%s: %.60s -> %d строк", qn,
                 isPathQuery ? " [ПУТИ]" : "", sql, outIndex - 1);
    }

    return 1;
}

// ── Lua-обёртка: таблица SQL с методами, self-совместимая ─────────────────
constexpr char kSqlSource[] = R"LUA(
local sql_open, sql_close, sql_query, sql_escape, sql_blob, sql_error,
      sql_affectedrows, sql_lastinsertid = ...

local SQL = {}

function SQL:Open()             return sql_open() end
function SQL:Close()            sql_close() end
function SQL:Query(sql, ...)    return sql_query(sql, ...) end
function SQL:Escape(sql)        return sql_escape(sql) end
function SQL:Blob(data)         return sql_blob(data) end
function SQL:Error()            return sql_error() end
function SQL:AffectedRows()     return sql_affectedrows() end
function SQL:LastInsertId()     return sql_lastinsertid() end

_G.SQL = SQL
return SQL
)LUA";

}  // namespace

bool RegisterVextSql(lua_State* L) {
    if (luaL_loadbufferx(L, kSqlSource, sizeof(kSqlSource) - 1, "=[vext-sql]",
                         "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: SQL не собрался: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    lua_pushcfunction(L, l_sql_open);
    lua_pushcfunction(L, l_sql_close);
    lua_pushcfunction(L, l_sql_query);
    lua_pushcfunction(L, l_sql_escape);
    lua_pushcfunction(L, l_sql_blob);
    lua_pushcfunction(L, l_sql_error);
    lua_pushcfunction(L, l_sql_affectedrows);
    lua_pushcfunction(L, l_sql_lastinsertid);
    if (lua_pcall(L, 8, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: SQL не выполнился: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: SQL подключён (server, sqlite3)");
    return true;
}

}  // namespace evbots
