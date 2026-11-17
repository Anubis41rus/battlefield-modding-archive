// LuaRuntime.cpp — Lua 5.4.2 + окружение VeniceEXT.
//
// Правила модов и require взяты из документации VU
// (content\modding\structure.md, content\vext\guides\code-splitting.md):
//   - запускается только __init__.lua из ext\{client,server,shared};
//   - require(path) ищет относительно ext\<контекст> текущего мода,
//     .lua необязателен, другие расширения не поддерживаются;
//   - shared-файлы только с префиксом __shared/;
//   - путь всегда от корня ext\<контекст>, а не от папки скрипта;
//   - скрипт исполняется один раз, его return возвращается в require.

#include "LuaRuntime.h"
#include "NativeRconGameplay.h"
#include "NativeVisualSettingsBridge.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "Json.h"
#include "EVBotsLog.h"
#include "InlineHook.h"  // сборка 238: CoreFrequencyHz
#include "LuaDebugConsole.h"
#include "LuaEvents.h"
#include "MiddleClass.h"
#include "ModSelection.h"
#include "VextEntityManager.h"
#include "VextClientEntities.h"
#include "VextEntityEvents.h"
#include "NativeSoldierCustomization.h"
#include "NativePlayerEvents.h"
#include "VextGeometry.h"
#include "VextGlobals.h"
#include "VextHooks.h"
#include "VextNet.h"
#include "VextNetEvents.h"
#include "VextNetCodec.h"
#include "EvNetBridge.h"
#include "LuaDll.h"
#include "VextClientPlayers.h"
#include "VextClientRaycast.h"
#include "VextClientInput.h"
#include "VextWebUi.h"
#include "VextSpectator.h"
#include "VextPlayerManager.h"
#include "VextRcon.h"
#include "VextResourceManager.h"
#include "VextSpatial.h"
#include "VextTicketManager.h"
#include "VextSql.h"
#include "VextConfig.h"
#include "VextTeamSquadManager.h"
#include "VextChatManager.h"
#include "VextClient.h"
#include "VextCustomization.h"
#include "VextFbTypes.h"
#include "VextEnums.h"
#include "VextUtils.h"
#include "VextVisualEnvironment.h"
#include "VextClientSoldier.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

constexpr const char* kVeniceExtVersion = "1.12.0";
constexpr wchar_t kSharedPrefixW[] = L"__shared/";

bool ReadFileText(const std::wstring& path, std::string& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        ::CloseHandle(h);
        out.clear();
        return true;
    }

    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()),
                               &read, nullptr);
    ::CloseHandle(h);
    if (!ok) {
        return false;
    }

    out.resize(read);
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                            static_cast<int>(w.size()), nullptr,
                                            0, nullptr, nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                            static_cast<int>(s.size()), nullptr,
                                            0);
    if (need <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          out.data(), need);
    return out;
}

const wchar_t kBackslash = 0x5C;
const wchar_t kSlash = L'/';

void ToBackslashes(std::wstring& s) {
    std::replace(s.begin(), s.end(), kSlash, kBackslash);
}

std::wstring JoinPathW(const std::wstring& a, const std::wstring& b) {
    std::wstring out = a;
    if (!out.empty() && out.back() != kBackslash && out.back() != kSlash) {
        out.push_back(kBackslash);
    }
    out += b;
    return out;
}

bool FileExistsW(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExistsW(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ── json для Lua ───────────────────────────────────────────────────────────

void PushJson(lua_State* L, const Json& j) {
    switch (j.type) {
        case Json::Type::Null: lua_pushnil(L); break;
        case Json::Type::Bool: lua_pushboolean(L, j.boolean ? 1 : 0); break;
        case Json::Type::Number: lua_pushnumber(L, j.number); break;
        case Json::Type::String:
            lua_pushlstring(L, j.str.data(), j.str.size());
            break;
        case Json::Type::Array:
            lua_createtable(L, static_cast<int>(j.arr.size()), 0);
            for (std::size_t k = 0; k < j.arr.size(); ++k) {
                PushJson(L, j.arr[k]);
                lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
            }
            break;
        case Json::Type::Object:
            lua_createtable(L, 0, static_cast<int>(j.obj.size()));
            for (const auto& kv : j.obj) {
                lua_pushlstring(L, kv.first.data(), kv.first.size());
                PushJson(L, kv.second);
                lua_rawset(L, -3);
            }
            break;
    }
}

/// Lua-значение -> Json. Таблица 1..n — массив, иначе объект.
Json FromLua(lua_State* L, int idx, int depth) {
    idx = lua_absindex(L, idx);
    if (depth > 32) {
        return Json();
    }

    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            return Json::MakeBool(lua_toboolean(L, idx) != 0);
        case LUA_TNUMBER:
            return Json::MakeNumber(lua_tonumber(L, idx));
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return Json::MakeString(std::string(s != nullptr ? s : "", len));
        }
        case LUA_TTABLE: {
            const lua_Integer n = luaL_len(L, idx);
            if (n > 0) {
                Json arr = Json::MakeArray();
                for (lua_Integer k = 1; k <= n; ++k) {
                    lua_rawgeti(L, idx, k);
                    arr.arr.push_back(FromLua(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return arr;
            }

            Json obj = Json::MakeObject();
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    std::size_t len = 0;
                    const char* k = lua_tolstring(L, -2, &len);
                    obj.obj.emplace_back(std::string(k != nullptr ? k : "", len),
                                         FromLua(L, -1, depth + 1));
                }
                lua_pop(L, 1);
            }
            return obj;
        }
        default:
            return Json();
    }
}

// Счётчик включает ошибки вложенных Events:Dispatch: внешний pcall их не видит.
// Отсутствующий/сломанный счётчик — ошибка загрузчика, а не ноль ошибок.
bool ReadEventFailureCount(lua_State* L, lua_Integer* out) {
    const int top = lua_gettop(L);
    bool ok = false;
    lua_getglobal(L, "Events");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "_FailureCount");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isinteger(L, -1)) {
                *out = lua_tointeger(L, -1);
                ok = *out >= 0;
            }
        }
    }
    lua_settop(L, top);
    return ok;
}

int l_json_encode(lua_State* L) {
    const std::string out = FromLua(L, 1, 0).Dump();
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int l_json_decode(lua_State* L) {
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);

    // Пустую строку ('' из SQLite) VU-шный json.decode трактует как пустой
    // объект — возвращаем {} вместо ошибки, иначе fun-bots падает на
    // NodeCollection.lua:1332 при data='' у вейпоинта.
    bool ws = true;
    for (std::size_t i = 0; i < len; ++i) {
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') {
            ws = false;
            break;
        }
    }
    if (ws) {
        lua_newtable(L);
        return 1;
    }

    std::string err;
    const Json j = Json::Parse(std::string(text, len), &err);
    if (!err.empty()) {
        return luaL_error(L, "json.decode: %s", err.c_str());
    }

    PushJson(L, j);
    return 1;
}

// ── string.starts / string.ends (расширения VU) ────────────────────────────

int l_string_starts(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    const char* p = luaL_checkstring(L, 2);
    const std::size_t ls = std::strlen(s);
    const std::size_t lp = std::strlen(p);
    lua_pushboolean(L, ls >= lp && std::memcmp(s, p, lp) == 0);
    return 1;
}

int l_string_ends(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    const char* p = luaL_checkstring(L, 2);
    const std::size_t ls = std::strlen(s);
    const std::size_t lp = std::strlen(p);
    lua_pushboolean(L, ls >= lp && std::memcmp(s + (ls - lp), p, lp) == 0);
    return 1;
}

}  // namespace

// ── require по правилам VU ────────────────────────────────────────────────

int LuaRuntime_require(lua_State* L) {
    LuaRuntime* rt = &LuaRuntime::Instance();
    const char* raw = luaL_checkstring(L, 1);

    std::string path(raw != nullptr ? raw : "");
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }

    if (path.empty()) {
        return luaL_error(L, "require: пустой путь");
    }

    // .lua необязателен; другие расширения не поддерживаются (docs VU)
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || dot < slash) {
        path += ".lua";
    } else if (path.compare(dot, 4, ".lua") != 0) {
        return luaL_error(L, "require: поддерживаются только .lua (%s)", raw);
    }

    const std::wstring mod = rt->currentModFolder();
    if (mod.empty()) {
        return luaL_error(L, "require: %s — require работает только внутри мода",
                          path.c_str());
    }

    std::wstring rel = WidenUtf8(path);
    ToBackslashes(rel);

    std::wstring full;
    const std::string sharedPrefix = "__shared/";
    if (path.rfind(sharedPrefix, 0) == 0) {
        std::wstring shared = WidenUtf8(path.substr(sharedPrefix.size()));
        ToBackslashes(shared);
        full = JoinPathW(JoinPathW(JoinPathW(mod, L"ext"), L"shared"), shared);
    } else {
        full = rt->ResolveModPath(rel);
    }

    // выполненные скрипты помним (docs VU: один раз, тот же return)
    lua_getfield(L, LUA_REGISTRYINDEX, "evbots.loaded");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "evbots.loaded");
    }

    const std::string key = NarrowUtf8(full);
    lua_pushlstring(L, key.data(), key.size());
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);

    std::string source;
    if (!ReadFileText(full, source)) {
        lua_pop(L, 1);
        return luaL_error(L, "require: не найден %s", key.c_str());
    }

    const std::string chunkName = "@" + key;
    if (luaL_loadbufferx(L, source.data(), source.size(), chunkName.c_str(),
                         "t") != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        const std::string err = msg != nullptr ? msg : "?";
        lua_pop(L, 2);
        return luaL_error(L, "require: синтаксис %s: %s", path.c_str(), err.c_str());
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        const std::string err = msg != nullptr ? msg : "?";
        lua_pop(L, 2);
        return luaL_error(L, "require: ошибка в %s: %s", path.c_str(), err.c_str());
    }

    // результат кладём в кэш (evbots.loaded) и он же возвращается из require:
    // стек после pcall — [path, cache, result]; делаем [path, cache, result, key, result]
    lua_pushlstring(L, key.data(), key.size());
    lua_pushvalue(L, -2);          // копия result под key
    lua_rawset(L, -4);             // cache[key] = result -> [path, cache, result]
    lua_remove(L, -2);             // убрать кэш-таблицу -> [path, result]
    return 1;                      // вернуть result (значение модуля)
}

// ── LuaRuntime ────────────────────────────────────────────────────────────

LuaRuntime& LuaRuntime::Instance() {
    static LuaRuntime rt;
    return rt;
}

bool LuaRuntime::Initialize(const std::wstring& modsRoot, LuaContext context) {
    std::lock_guard<std::recursive_mutex> lock(Gate());
    if (state_ != nullptr) {
        return true;
    }
    // ★ Сборка 220: Lua — в отдельной lua.dll (как у VU). Нет её — не падаем на первом вызове Lua, а честно выходим.
    if (!EnsureLuaDll()) {
        return false;
    }

    modsRoot_ = modsRoot;
    context_ = context;
    lastError_.clear();
    selectionFailed_ = false;

    state_ = luaL_newstate();
    if (state_ == nullptr) {
        LOG_ERR("EVBots: failed to create Lua state");
        return false;
    }

    luaL_openlibs(state_);
    applySandbox();
    registerStringExtensions();
    registerJson();
    registerRequire();
    declareVeniceExt();
    registerMiddleClass();
    registerEvents();
    lua_Integer eventFailures = 0;
    if (!ReadEventFailureCount(state_, &eventFailures)) {
        LOG_ERR("EVBots: Events registration failed; Lua stopped");
        Shutdown();
        return false;
    }
    registerDebugConsole();
    registerVextEnums();

    // ★ Сборка 215 (К3): клиент (bf3.exe). Только общие библиотеки (не трогают движок) и клиентские из VextClient.cpp.
    // Серверные (мост PlayerManager, EntityManager, SQL, RCON, тикеты, отряды, чат, серверные NetEvents) работают по
    // адресам bf3_server_Final.exe — в клиентском процессе их вызов уронил бы игру, поэтому их здесь нет.
    // ★ Сборка 234: исключение — ResourceManager (слой движка с таблицей адресов zbf3, ниже).
    if (context == LuaContext::Client) {
        registerVextGlobals();
        registerVextUtils();
        registerVextGeometry();
        registerVextSpatial();
        registerVextHooks();
        registerVextNet();
        registerVextCustomization();
        registerVextConfig();
        // ★ Сборка 222 (К5 шаг 4): игроки клиента из движка — только в настоящем bf3.exe (в тест-хосте адресов движка нет)
        if (::GetModuleHandleA("bf3.exe") != nullptr) {
            RegisterClientPlayers(state_);
            RegisterClientRaycast(state_);  // ★ Сборка 223: лучи клиента из движка
            // ★ Сборка 234 (В8): ResourceManager как на сервере — слой движка с таблицей адресов zbf3
            // (EngineResources.cpp). VextClient.cpp тогда свои заглушки ResourceManager не ставит.
            registerVextResourceManager();
        }
        // ★ Сборка 228: InputManager (клавиши и мышь кадра) и WebUI модов (оверлей WebView2) — движок им не нужен, но без
        // игры их поднимает только проверка EVBOTS_WEBUI_TEST=1 (webui-probe\client-harness)
        if (::GetModuleHandleA("bf3.exe") != nullptr || WebUiTestMode()) {
            RegisterClientInput(state_);
            RegisterClientWebUi(state_);
        }
        if (!RegisterVextClient(state_)) {
            LOG_ERR("EVBots: client API registration failed; Lua stopped");
            Shutdown();
            return false;
        }
        // ★ Сборка 219 (К4): упаковка NetEvents и связь с каналом ev-core.dll
        RegisterNetCodec(state_);
        RegisterEvNetBridge(state_);
        // fb-классы VU как глобальные типы (Foo(), Foo(guid), Foo(other)) — после библиотек, занятые имена не трогает
        if (RegisterVextFbTypes(state_) < 0 || !RegisterVextBlueprintEntities(state_) ||
            !RegisterVextClientSoldier(state_) ||
            !RegisterVextEntityEvents(state_) || !RegisterVextVisualEnvironment(state_) ||
            (::GetModuleHandleA("bf3.exe") != nullptr &&
                (!InstallClientSpatialRaycast(state_) || !InstallClientPhysicsRaycast(state_)))) {
            LOG_ERR("EVBots: client FB/Blueprint API registration failed");
            Shutdown();
            return false;
        }
        if (!RegisterNativeVisualSettings(state_)) {
            LOG_ERR("EVBots: client visual settings registration failed");
            Shutdown();
            return false;
        }
        LOG_INFO("EVBots: Lua ready (context client), mods: %s", NarrowUtf8(modsRoot_).c_str());
        return true;
    }

    registerVextEntityManager();
    registerVextGlobals();
    registerVextUtils();
    registerVextGeometry();
    registerVextSpatial();
    registerVextHooks();
    registerVextNet();
    registerVextNetEvents();
    RegisterNetCodec(state_);     // ★ Сборка 219 (К4)
    RegisterEvNetBridge(state_);
    registerVextPlayerManager();
    registerVextResourceManager();
    registerVextTicketManager();
    registerVextSql();
    registerVextRcon();
    registerVextTeamSquadManager();
    registerVextChatManager();
    registerVextCustomization();
    registerVextConfig();

    if (RegisterVextFbTypes(state_) < 0 || !RegisterVextBlueprintEntities(state_) ||
        !RegisterVextEntityEvents(state_) || !RegisterNativePlayerEvents(state_)) {
        LOG_ERR("EVBots: server FB/Blueprint API registration failed");
        Shutdown();
        return false;
    }
    if (!RegisterNativeVisualSettings(state_)) {
        LOG_ERR("EVBots: server visual settings registration failed");
        Shutdown();
        return false;
    }
    LOG_INFO("EVBots: Lua ready (context %s), mods: %s",
             (context == LuaContext::Server) ? "server" : "client",
             NarrowUtf8(modsRoot_).c_str());
    return true;
}

void LuaRuntime::applySandbox() {
    static const char* kGlobals[] = {"dofile", "loadfile", "load", nullptr};
    for (const char** g = kGlobals; *g != nullptr; ++g) {
        lua_pushnil(state_);
        lua_setglobal(state_, *g);
    }

    lua_getglobal(state_, "string");
    if (lua_istable(state_, -1)) {
        lua_pushnil(state_);
        lua_setfield(state_, -2, "dump");
        lua_pop(state_, 1);
    }

    lua_getglobal(state_, "package");
    if (lua_istable(state_, -1)) {
        static const char* kPackage[] = {"config", "cpath", "loadlib", "path",
                                         "searchpath", nullptr};
        for (const char** p = kPackage; *p != nullptr; ++p) {
            lua_pushnil(state_);
            lua_setfield(state_, -2, *p);
        }
        lua_pop(state_, 1);
    }

    lua_getglobal(state_, "os");
    if (lua_istable(state_, -1)) {
        static const char* kOs[] = {"execute", "exit",    "getenv",  "remove",
                                    "rename",  "setlocale", "tmpname", "getcwd",
                                    "chdir",   nullptr};
        for (const char** p = kOs; *p != nullptr; ++p) {
            lua_pushnil(state_);
            lua_setfield(state_, -2, *p);
        }
        lua_pop(state_, 1);
    }

    LOG_INFO("EVBots: окружение Lua очищено (dofile/load/loadfile, "
             "string.dump, package.*, os.execute/exit/getenv/...)");
}

void LuaRuntime::registerStringExtensions() {
    lua_getglobal(state_, "string");
    if (!lua_istable(state_, -1)) {
        lua_pop(state_, 1);
        return;
    }

    lua_pushcfunction(state_, l_string_starts);
    lua_setfield(state_, -2, "starts");
    lua_pushcfunction(state_, l_string_ends);
    lua_setfield(state_, -2, "ends");
    lua_pop(state_, 1);
    LOG_INFO("EVBots: добавлены string.starts / string.ends");
}

void LuaRuntime::registerJson() {
    lua_newtable(state_);
    lua_pushcfunction(state_, l_json_encode);
    lua_setfield(state_, -2, "encode");
    lua_pushcfunction(state_, l_json_decode);
    lua_setfield(state_, -2, "decode");
    lua_setglobal(state_, "json");
    LOG_INFO("EVBots: добавлен json.encode / json.decode");
}

void LuaRuntime::registerRequire() {
    lua_pushcfunction(state_, LuaRuntime_require);
    lua_setglobal(state_, "require");
    lua_newtable(state_);
    lua_setfield(state_, LUA_REGISTRYINDEX, "evbots.loaded");
    LOG_INFO("EVBots: свой require (ext/<контекст>, __shared/, .lua)");
}

void LuaRuntime::declareVeniceExt() {
    // моды проверяют "veniceext": ">=1.12.0" в mod.json — объявляем себя
    lua_newtable(state_);
    lua_pushstring(state_, kVeniceExtVersion);
    lua_setfield(state_, -2, "VERSION");
    lua_pushstring(state_, "EVBots");
    lua_setfield(state_, -2, "IMPL");
    lua_setglobal(state_, "veniceext");
    LOG_INFO("EVBots: объявлен veniceext %s", kVeniceExtVersion);
    // Сборка 238: частота движка (ядро, как VU -high60/-high120) — для SharedUtils:GetTickrate()
    lua_pushinteger(state_, CoreFrequencyHz());
    lua_setglobal(state_, "EVBOTS_TICKRATE");
}

void LuaRuntime::registerMiddleClass() {
    if (RegisterMiddleClass(state_)) {
        LOG_INFO("EVBots: class() подключён (middleclass, конструктор __init)");
    } else {
        LOG_ERR("EVBots: middleclass не загрузился — class() недоступен");
    }
}

void LuaRuntime::registerEvents() {
    if (RegisterLuaEvents(state_)) {
        LOG_INFO("EVBots: Events подключён (Subscribe/Dispatch/DispatchLocal)");
    } else {
        LOG_ERR("EVBots: Events не загрузился — подписки модов не работают");
    }
}

void LuaRuntime::registerDebugConsole() {
    if (RegisterDebugConsole(state_)) {
        LOG_INFO("EVBots: Console подключён, print модов -> лог");
    } else {
        LOG_ERR("EVBots: Console не загрузился");
    }
}

void LuaRuntime::registerVextEnums() {
    if (RegisterVextEnums(state_)) {
        LOG_INFO("EVBots: таблицы-перечисления VU подключены (InputDeviceKeys, TeamId, ...)");
    } else {
        LOG_ERR("EVBots: перечисления VU не загрузились");
    }
}

void LuaRuntime::registerVextUtils() {
    const int server = (context_ == LuaContext::Server) ? 1 : 0;
    if (RegisterVextUtils(state_, server)) {
        LOG_INFO("EVBots: MathUtils и SharedUtils подключены (контекст %s)",
                 server ? "server" : "client");
    } else {
        LOG_ERR("EVBots: MathUtils/SharedUtils не загрузились");
    }
}

void LuaRuntime::registerVextGeometry() {
    if (RegisterVextGeometry(state_)) {
        LOG_INFO("EVBots: типы-конструкторы VU подключены (Vec3, "
                 "LinearTransform, Guid, ...)");
    } else {
        LOG_ERR("EVBots: геометрия VU не загрузилась");
    }
}

void LuaRuntime::registerVextEntityManager() {
    if (RegisterVextEntityManager(state_)) {
        LOG_INFO("EVBots: EntityManager подключен (заглушка)");
    } else {
        LOG_ERR("EVBots: EntityManager не загрузился");
    }
}

void LuaRuntime::registerVextGlobals() {
    if (RegisterVextGlobals(state_)) {
        LOG_INFO("EVBots: Globals подключен (заглушка)");
    } else {
        LOG_ERR("EVBots: Globals не загрузился");
    }
}

void LuaRuntime::registerVextNetEvents() {
    if (RegisterVextNetEvents(state_)) {
        LOG_INFO("EVBots: NetEvents подключены (сервер)");
    } else {
        LOG_ERR("EVBots: NetEvents не загрузились");
    }
}

void LuaRuntime::registerVextSql() {
    if (RegisterVextSql(state_)) {
        LOG_INFO("EVBots: SQL подключён (сервер)");
    } else {
        LOG_ERR("EVBots: SQL не загрузился");
    }
}

void LuaRuntime::registerVextPlayerManager() {
    if (RegisterVextPlayerManager(state_)) {
        LOG_INFO("EVBots: PlayerManager подключен (server)");
    } else {
        LOG_ERR("EVBots: PlayerManager не загрузился");
    }
}

void LuaRuntime::registerVextHooks() {
    if (RegisterVextHooks(state_)) {
        LOG_INFO("EVBots: Hooks подключены, реестр обработчиков");
    } else {
        LOG_ERR("EVBots: Hooks не загрузились");
    }
}

void LuaRuntime::registerVextResourceManager() {
    if (RegisterVextResourceManager(state_)) {
        LOG_INFO("EVBots: ResourceManager подключен");
    } else {
        LOG_ERR("EVBots: ResourceManager не загрузился");
    }
}

void LuaRuntime::registerVextTicketManager() {
    if (RegisterVextTicketManager(state_)) {
        LOG_INFO("EVBots: TicketManager подключен (server)");
    } else {
        LOG_ERR("EVBots: TicketManager не загрузился");
    }
}

void LuaRuntime::registerVextRcon() {
    if (RegisterVextRcon(state_)) {
        if (!RegisterNativeRconGameplay(state_)) {
            LOG_ERR("EVBots: native gameplay RCON commands failed to register");
        }
        LOG_INFO("EVBots: RCON подключен (сервер)");
    } else {
        LOG_ERR("EVBots: RCON не загрузился");
    }
}

void LuaRuntime::registerVextTeamSquadManager() {
    if (RegisterVextTeamSquadManager(state_)) {
        LOG_INFO("EVBots: TeamSquadManager подключен (server)");
    } else {
        LOG_ERR("EVBots: TeamSquadManager не загрузился");
    }
}

void LuaRuntime::registerVextChatManager() {
    if (RegisterVextChatManager(state_)) {
        LOG_INFO("EVBots: ChatManager подключен (server)");
    } else {
        LOG_ERR("EVBots: ChatManager не загрузился");
    }
}

void LuaRuntime::registerVextCustomization() {
    if (RegisterVextCustomization(state_)) {
        LOG_INFO("EVBots: Customization fb-типы подключены");
    } else {
        LOG_ERR("EVBots: Customization fb-типы не загрузились");
    }
}

void LuaRuntime::registerVextNet() {
    if (RegisterVextNet(state_)) {
        LOG_INFO("EVBots: Net подключён");
    } else {
        LOG_ERR("EVBots: Net не загрузился");
    }
}

void LuaRuntime::registerVextConfig() {
    if (RegisterVextConfig(state_)) {
        LOG_INFO("EVBots: Config/SpawnMethod/TeamSwitchModes подключены");
    } else {
        LOG_ERR("EVBots: Config/SpawnMethod/TeamSwitchModes не загрузились");
    }
}

void LuaRuntime::registerVextSpatial() {
    if (RegisterVextSpatial(state_)) {
        LOG_INFO("EVBots: SpatialEntity/AxisAlignedBox подключены");
    } else {
        LOG_ERR("EVBots: SpatialEntity/AxisAlignedBox не загрузились");
    }
}

std::wstring LuaRuntime::ResolveModPath(const std::wstring& rel) const {
    std::wstring base = JoinPathW(currentMod_, L"ext");
    base = JoinPathW(base, context_ == LuaContext::Server ? L"server" : L"client");
    return JoinPathW(base, rel);
}

int LuaRuntime::LoadMods() {
    std::lock_guard<std::recursive_mutex> lock(Gate());
    if (initRan_) {
        lastError_ = "набор модов уже инициализирован; перед сменой набора требуется Shutdown";
        LOG_ERR("EVBots: выбор модов: %s", lastError_.c_str());
        return -1;
    }
    mods_.clear();
    lastError_.clear();
    selectionFailed_ = false;
    const auto fail = [&](const std::string& error) {
        lastError_ = error;
        selectionFailed_ = true;
        LOG_ERR("EVBots: выбор модов: %s", lastError_.c_str());
        return -1;
    };
    if (state_ == nullptr) return fail("Lua не инициализирована");

    // Пустое значение Windows обычно удаляет: @none — однозначный пустой набор.
    // Admin/ModList.txt намеренно не читается: старый backend оставляет его пустым.
    ::SetLastError(ERROR_SUCCESS);
    const DWORD needed = ::GetEnvironmentVariableW(L"EVBOTS_ACTIVE_MODS", nullptr, 0);
    const DWORD envError = ::GetLastError();
    const bool defined = needed != 0 || envError != ERROR_ENVVAR_NOT_FOUND;
    std::wstring selection;
    if (needed != 0) {
        std::vector<wchar_t> buffer(needed);
        const DWORD copied = ::GetEnvironmentVariableW(L"EVBOTS_ACTIVE_MODS", buffer.data(), needed);
        if (copied == 0 || copied >= needed) return fail("EVBOTS_ACTIVE_MODS изменился во время чтения; повторите запуск");
        selection.assign(buffer.data(), copied);
    } else if (envError != ERROR_SUCCESS && envError != ERROR_ENVVAR_NOT_FOUND) {
        return fail("не удалось прочитать EVBOTS_ACTIVE_MODS, Win32=" + std::to_string(envError));
    }

    std::vector<modselection::Candidate> candidates;
    // @none не требует даже существования каталога модов.
    if (!modsRoot_.empty() && !(defined && modselection::SameName(modselection::Trim(selection), L"@none"))) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(JoinPathW(modsRoot_, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                return fail("не читается каталог модов '" + NarrowUtf8(modsRoot_) + "', Win32=" + std::to_string(error));
            }
        } else {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                    std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) continue;
                modselection::Candidate candidate;
                candidate.folderName = fd.cFileName;
                const std::wstring path = JoinPathW(JoinPathW(modsRoot_, candidate.folderName), L"mod.json");
                const DWORD attributes = ::GetFileAttributesW(path.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES) {
                    candidate.hasDescriptor = true;
                    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) candidate.readError = "mod.json является каталогом";
                    else if (!ReadFileText(path, candidate.descriptor)) candidate.readError = "не читается mod.json";
                } else {
                    const DWORD error = ::GetLastError();
                    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                        candidate.hasDescriptor = true;
                        candidate.readError = "mod.json недоступен, Win32=" + std::to_string(error);
                    }
                }
                candidates.push_back(std::move(candidate));
            } while (::FindNextFileW(h, &fd));
            const DWORD error = ::GetLastError();
            ::FindClose(h);
            if (error != ERROR_NO_MORE_FILES) return fail("не удалось дочитать каталог модов, Win32=" + std::to_string(error));
        }
    }
    std::vector<modselection::SelectedMod> selected;
    std::string error;
    if (!modselection::Resolve(candidates, defined, selection, kVeniceExtVersion, &selected, &error)) {
        return fail(error + " [каталог: " + NarrowUtf8(modsRoot_) + "]");
    }
    for (auto& choice : selected) {
        LoadedMod mod;
        mod.folder = JoinPathW(modsRoot_, choice.folderName);
        mod.name = std::move(choice.name);
        mod.version = std::move(choice.version);
        mod.hasVeniceExt = choice.hasVeniceExt;
        mod.dependsVeniceExt = std::move(choice.dependsVeniceExt);
        LOG_INFO("EVBots: selected mod folder='%s', name='%s', version=%s", NarrowUtf8(choice.folderName).c_str(),
                 mod.name.c_str(), mod.version.c_str());
        mods_.push_back(std::move(mod));
    }
    LOG_INFO("EVBots: selected mods: %zu (%s)", mods_.size(), defined ? "EVBOTS_ACTIVE_MODS" : "all; compatibility mode");
    return static_cast<int>(mods_.size());
}

int LuaRuntime::runInitScripts() {
    std::lock_guard<std::recursive_mutex> lock(Gate());
    if (selectionFailed_) return -1;
    if (initRan_ || state_ == nullptr) {
        return 0;
    }
    initRan_ = true;

    int ran = 0;
    for (auto& mod : mods_) {
        LOG_INFO("[VeniceEXT] Loading VeniceEXT module '%s'.", mod.name.c_str());
        ConsolePrintf("info", "EVBots", "Loading EVBots module '%s' %s.", mod.name.c_str(), mod.version.c_str());
        std::wstring ctxDir = JoinPathW(JoinPathW(mod.folder, L"ext"),
                                        context_ == LuaContext::Server ? L"server"
                                                                        : L"client");
        // Порядок как у VU: сначала ext\shared\__init__.lua, потому что
        // общие константы (BotWeapons и т.п.) объявляются именно в shared и
        // нужны server/client до выполнения их __init__.lua.
        const std::wstring initShared =
            JoinPathW(JoinPathW(JoinPathW(mod.folder, L"ext"), L"shared"),
                      L"__init__.lua");
        const std::wstring initCtx = JoinPathW(ctxDir, L"__init__.lua");

        for (const std::wstring* p : {&initShared, &initCtx}) {
            if (!FileExistsW(*p)) {
                continue;
            }

            const std::wstring saved = currentMod_;
            currentMod_ = mod.folder;

            std::string err;
            lua_Integer failuresBefore = 0, failuresAfter = 0;
            bool ok = ReadEventFailureCount(state_, &failuresBefore);
            if (!ok) err = "не читается счётчик ошибок Events перед init";
            if (ok) ok = runScriptFile(*p, "@" + NarrowUtf8(*p), &err);
            if (ok && (!ReadEventFailureCount(state_, &failuresAfter) || failuresAfter != failuresBefore)) {
                ok = false;
                err = "ошибка обработчика Events во время init (подробности выше в журнале)";
            }
            if (ok) {
                ++ran;
                mod.ranInit = true;
                LOG_INFO("EVBots: mod '%s': ran %s",
                         mod.name.c_str(), NarrowUtf8(*p).c_str());
                ConsolePrintf("info", mod.name.c_str(), "Loaded %s script '__init__.lua'.",
                              p == &initShared ? "shared" : (context_ == LuaContext::Server ? "server" : "client"));
            } else {
                mod.error = err;
                mod.ranInit = false;
                LOG_ERR("EVBots: мод «%s»: %s", mod.name.c_str(), err.c_str());
                ConsolePrintf("error", mod.name.c_str(), "%s", err.c_str());
            }

            currentMod_ = saved;
            // Ошибка shared init запрещает запуск context init и остальных модов.
            // Вызывающая сторона закроет Lua: частичные подписки не должны работать.
            if (!mod.error.empty()) return -1;
        }
    }

    LOG_INFO("[VeniceEXT] __init__.lua ran: %d", ran);
    return ran;
}

bool LuaRuntime::runScriptFile(const std::wstring& path,
                               const std::string& chunkName,
                               std::string* error) {
    std::string source;
    if (!ReadFileText(path, source)) {
        if (error != nullptr) {
            *error = "не читается " + NarrowUtf8(path);
        }
        return false;
    }

    if (luaL_loadbufferx(state_, source.data(), source.size(),
                         chunkName.c_str(), "t") != LUA_OK) {
        if (error != nullptr) {
            const char* m = lua_tostring(state_, -1);
            *error = std::string("синтаксис: ") + (m != nullptr ? m : "?");
        }
        lua_pop(state_, 1);
        return false;
    }

    if (lua_pcall(state_, 0, 0, 0) != LUA_OK) {
        if (error != nullptr) {
            const char* m = lua_tostring(state_, -1);
            *error = std::string("ошибка: ") + (m != nullptr ? m : "?");
        }
        lua_pop(state_, 1);
        return false;
    }

    return true;
}

std::wstring LuaRuntime::DiscoverModsRoot() {
    // 1) явное указание (удобно для теста и для нестандартного размещения)
    wchar_t env[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"EVBOTS_MODS", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::wstring p(env);
        if (DirExistsW(p)) {
            return p;
        }
    }

    // 2) рабочий каталог процесса и常见的 варианты рядом
    wchar_t cwd[MAX_PATH]{};
    if (::GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
        std::wstring base(cwd);
        const wchar_t* kVariants[] = {L"", L"\\Instance", L"\\Server",
                                      L"\\Battlefield 3\\Server", nullptr};
        for (const wchar_t** v = kVariants; *v != nullptr; ++v) {
            const std::wstring dir = JoinPathW(base, *v);
            const std::wstring mods = JoinPathW(JoinPathW(dir, L"Admin"), L"Mods");
            if (DirExistsW(mods)) {
                return mods;
            }
        }
    }

    // 3) каталог самого процесса (рядом может лежать Instance)
    wchar_t exe[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
        std::wstring dir(exe);
        const std::size_t cut = dir.find_last_of(kBackslash);
        if (cut != std::wstring::npos) {
            dir.resize(cut);
        }

        const wchar_t* kVariants[] = {L"\\Instance", nullptr};
        for (const wchar_t** v = kVariants; *v != nullptr; ++v) {
            const std::wstring mods =
                JoinPathW(JoinPathW(JoinPathW(dir, *v), L"Admin"), L"Mods");
            if (DirExistsW(mods)) {
                return mods;
            }
        }
    }

    return {};
}

LoadedMod* LuaRuntime::findModByFolder(const std::wstring& folder) {
    for (auto& m : mods_) {
        if (::_wcsicmp(m.folder.c_str(), folder.c_str()) == 0) {
            return &m;
        }
    }
    return nullptr;
}

void LuaRuntime::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(Gate());
    if (state_ != nullptr) {
        if (context_ == LuaContext::Server) playerevents::Shutdown();
        soldiercustom::Shutdown();
        ShutdownVextEntityEvents(state_);
        lua_close(state_);
        state_ = nullptr;
        // GC кладёт native refs в очередь. Освобождение допустимо только на кадре
        // движка; при закрытии с worker Poll оставляет их следующему кадру.
        if (context_ == LuaContext::Server) playerevents::Poll();
    }

    mods_.clear();
    currentMod_.clear();
    initRan_ = false;
    selectionFailed_ = false;
    LOG_INFO("EVBots: Lua closed");
}

bool LuaRuntime::DispatchEngineEvent(const char* name,
                                     const std::vector<EngineEventArg>& args) {
    std::lock_guard<std::recursive_mutex> lock(Gate());
    struct CameraResetOnExit {
        bool active;
        ~CameraResetOnExit() { if (active) StopVextSpectator(); }
    } cameraReset{context_ == LuaContext::Client && name != nullptr && std::strcmp(name, "Level:Destroy") == 0};
    if (state_ == nullptr || name == nullptr || *name == '\0') {
        return false;
    }

    // ★ Сборка 249: стек Lua возвращаем к входному после события. До 249 таблица Events и
    // результаты Dispatch (LUA_MULTRET) оставались на стеке каждый кадр — на клиенте 29.10
    // за 39 мин стек дорос до предела, «stack overflow» в каждом обработчике, игра встала.
    const int top = lua_gettop(state_);
    struct TopGuard { lua_State* L; int t; ~TopGuard() { lua_settop(L, t); } } guard{state_, top};
    if (std::strcmp(name, "Engine:Update") == 0) {
        // Physics notifications are delivered only on this realm's engine frame,
        // with the Lua gate held. Never call Lua on a physics worker thread.
        PollVextBlueprintCollisions(state_);
        PollVextEntityEvents(state_);
        if (context_ == LuaContext::Server) {
            soldiercustom::Poll();
            playerevents::Poll();
        }
    }
    lua_Integer failuresBefore = 0;
    if (!ReadEventFailureCount(state_, &failuresBefore)) {
        LOG_ERR("EVBots: DispatchEngineEvent(%s) - Events failure counter unavailable", name);
        return false;
    }
    {
        // контроль утечки: размер стека раз в минуту и при первом росте выше 64
        static ULONGLONG lastLog = 0;
        static bool warned = false;
        const ULONGLONG now = GetTickCount64();
        if (now - lastLog >= 60000) {
            lastLog = now;
            LOG_INFO("EVBots: стек Lua на входе события: %d", top);
        }
        if (top > 64 && !warned) {
            warned = true;
            LOG_WARN("EVBots: стек Lua на входе события %s уже %d — где-то течёт", name, top);
        }
    }

    // Стек: [Events, Dispatch-Fn]
    lua_getglobal(state_, "Events");
    if (!lua_istable(state_, -1)) {
        lua_pop(state_, 1);
        LOG_WARN("EVBots: DispatchEngineEvent(%s) - Events not found",
                 name);
        return false;
    }

    // ★ Лог редких движковых событий (сборка 32, 13.10.2026).
    // Engine:Update и UpdateManager:Update идут 30 Гц — их НЕ логируем
    // (заспамили бы лог). Всё остальное (Engine:Init, Extension:Loaded,
    // Level:Loaded, NodeCollection:FinishedLoading) — пишем с числом
    // аргументов: нужно видеть порядок и то, кто что шлёт (мы или мод),
    // чтобы разобрать, почему боты не двигаются.
    const bool isTick =
        (std::strcmp(name, "Engine:Update") == 0 ||
         std::strcmp(name, "UpdateManager:Update") == 0 ||
         // сборка 235: события кадра клиента — 18,5 тыс. строк каждое за хвост журнала 234-г
         std::strcmp(name, "UI:DrawHud") == 0 ||
         std::strcmp(name, "Client:UpdateInput") == 0);
    if (!isTick) {
        LOG_INFO("EVBots: -> Events:Dispatch(\"%s\", %d арг.)", name,
                 static_cast<int>(args.size()));
        // ★ Сборка 217: уровень — строкой в консоль окна сервера (как VU пишет загрузку уровня)
        if (context_ == LuaContext::Server && std::strcmp(name, "Level:Loaded") == 0) {
            ConsolePrintf("info", "EVBots", "Level loaded: %s (%s).",
                          args.size() > 0 ? args[0].text.c_str() : "?", args.size() > 1 ? args[1].text.c_str() : "?");
        } else if (context_ == LuaContext::Server && std::strcmp(name, "Level:Destroy") == 0) {
            ConsoleWrite("info", "EVBots", "Level destroyed.");
        }
    }
    lua_getfield(state_, -1, "Dispatch");
    if (!lua_isfunction(state_, -1)) {
        lua_pop(state_, 2);
        LOG_WARN("EVBots: DispatchEngineEvent(%s) - no Dispatch field",
                 name);
        return false;
    }

    // self: Events, аргумент: имя события, затем списочные аргументы
    lua_pushvalue(state_, -2);          // self (Events)
    lua_pushstring(state_, name);       // событие

    for (const EngineEventArg& a : args) {
        switch (a.kind) {
            case EngineEventArg::Kind::None:
                lua_pushnil(state_);
                break;
            case EngineEventArg::Kind::String:
                lua_pushlstring(state_, a.text.data(), a.text.size());
                break;
            case EngineEventArg::Kind::Number:
                lua_pushnumber(state_, a.number);
                break;
            case EngineEventArg::Kind::Boolean:
                lua_pushboolean(state_, a.boolean ? 1 : 0);
                break;
        }
    }

    const int nargs = 2 + static_cast<int>(args.size());  // self + имя + args
    if (lua_pcall(state_, nargs, LUA_MULTRET, 0) != LUA_OK) {
        const char* m = lua_tostring(state_, -1);
        LOG_ERR("EVBots: движковое событие %s упало: %s",
                name, m != nullptr ? m : "?");
        ConsolePrintf("error", "EVBots", "Error in event '%s': %s", name, m != nullptr ? m : "?");
        // Traceback: копируем сообщение, добавляем стек вызовов Lua.
        const char* msg = lua_tostring(state_, -1);
        if (msg != nullptr) {
            luaL_traceback(state_, state_, msg, 1);
            const char* tb = lua_tostring(state_, -1);
            LOG_ERR("EVBots:  ^--- стек вызова:\n%s",
                    tb != nullptr ? tb : "?");
            // сборка 249: до неё шаг цикла стоял на самом '\n' и не двигался — бесконечный
            // поток пустых строк « | » (лог клиента 29.10 вырос до 3,2 ГБ, кадр встал)
            int nLines = 0;
            for (const char* line = tb; line != nullptr && *line != '\0' && nLines < 40; ++nLines) {
                // разбиваем многострочный traceback на строки для читаемого лога
                std::string l;
                const char* nl = strchr(line, '\n');
                if (nl != nullptr) l.assign(line, nl - line); else l = line;
                LOG_ERR("EVBots:  | %s", l.c_str());
                if (nl == nullptr) break;
                line = nl + 1;
            }
            lua_pop(state_, 1);
        }
        lua_pop(state_, 1);
        return false;
    }
    lua_Integer failuresAfter = 0;
    const bool delivered = ReadEventFailureCount(state_, &failuresAfter) && failuresAfter == failuresBefore;
    if (context_ == LuaContext::Server && std::strcmp(name, "Level:Loaded") == 0) {
        if (delivered) LOG_INFO("LEVEL-MODE READY: Level:Loaded callbacks completed");
        else LOG_ERR("LEVEL-MODE FAILED: Level:Loaded callback failed");
    }
    return delivered;
}

bool LuaRuntime::CallTableFunction(const char* table, const char* fn,
                                   const std::vector<EngineEventArg>& args,
                                   int waitMs, double* outNum) {
    std::recursive_mutex& gate = Gate();
    if (waitMs < 0) {
        gate.lock();
    } else if (!gate.try_lock()) {
        // ★ Сборка 164: ожидание по QPC. Раньше — Sleep(1) + GetTickCount64: у обоих шаг ~15,6 мс, поэтому при
        // waitMs=2 любая встреча с тиком Lua означала провал (163: до половины пуль — «Lua занята → нативно»), хотя
        // средний тик Lua ~2 мс (профиль 164). Короткое ожидание — SwitchToThread, длинное — Sleep(1).
        LARGE_INTEGER f, t0, now;
        ::QueryPerformanceFrequency(&f);
        ::QueryPerformanceCounter(&t0);
        const LONGLONG budget = f.QuadPart * static_cast<LONGLONG>(waitMs) / 1000;
        for (;;) {
            if (waitMs <= 10) {
                ::SwitchToThread();
            } else {
                ::Sleep(1);
            }
            if (gate.try_lock()) break;
            ::QueryPerformanceCounter(&now);
            if (now.QuadPart - t0.QuadPart >= budget) {
                return false;
            }
        }
    }
    std::lock_guard<std::recursive_mutex> lock(gate, std::adopt_lock);
    if (state_ == nullptr || table == nullptr || fn == nullptr) {
        return false;
    }
    const int top = lua_gettop(state_);
    lua_getglobal(state_, table);
    if (!lua_istable(state_, -1)) {
        lua_settop(state_, top);
        return false;
    }
    lua_getfield(state_, -1, fn);
    if (!lua_isfunction(state_, -1)) {
        lua_settop(state_, top);
        return false;
    }
    for (const EngineEventArg& a : args) {
        switch (a.kind) {
            case EngineEventArg::Kind::None:
                lua_pushnil(state_);
                break;
            case EngineEventArg::Kind::String:
                lua_pushlstring(state_, a.text.data(), a.text.size());
                break;
            case EngineEventArg::Kind::Number:
                lua_pushnumber(state_, a.number);
                break;
            case EngineEventArg::Kind::Boolean:
                lua_pushboolean(state_, a.boolean ? 1 : 0);
                break;
        }
    }
    const bool ok = lua_pcall(state_, static_cast<int>(args.size()), outNum != nullptr ? 1 : 0, 0) == LUA_OK;
    if (!ok) {
        const char* m = lua_tostring(state_, -1);
        LOG_ERR("EVBots: %s.%s упала: %s", table, fn, m != nullptr ? m : "?");
    } else if (outNum != nullptr && lua_isnumber(state_, -1)) {
        *outNum = lua_tonumber(state_, -1);
    }
    lua_settop(state_, top);
    return ok;
}

bool LuaRuntime::AddTestHumanPlayer(const std::string& name, int team,
                                    int squad) {
    std::lock_guard<std::recursive_mutex> lock(Gate());
    if (state_ == nullptr || name.empty()) {
        return false;
    }

    // Стек: [PlayerManager] и вызываем CreatePlayer(name, team, squad).
    lua_getglobal(state_, "PlayerManager");
    if (!lua_istable(state_, -1)) {
        lua_pop(state_, 1);
        LOG_WARN("AddTestHumanPlayer: PlayerManager not found");
        return false;
    }
    lua_getfield(state_, -1, "CreatePlayer");
    if (!lua_isfunction(state_, -1)) {
        lua_pop(state_, 2);
        LOG_WARN("AddTestHumanPlayer: CreatePlayer not found");
        return false;
    }

    lua_pushvalue(state_, -2);       // self (PlayerManager)
    lua_pushlstring(state_, name.data(), name.size());
    lua_pushinteger(state_, team);
    lua_pushinteger(state_, squad);

    if (lua_pcall(state_, 4, 1, 0) != LUA_OK) {
        const char* m = lua_tostring(state_, -1);
        LOG_ERR("EVBots: AddTestHumanPlayer: CreatePlayer failed: %s",
                m != nullptr ? m : "?");
        lua_pop(state_, 2);
        return false;
    }

    // Результат — объект Player на вершине. Делаем его «человеком»:
    // onlineId != 0, чтобы Utilities:isBot(onlineId == 0) вернул false.
    lua_pushinteger(state_, 777);
    lua_setfield(state_, -2, "onlineId");
    lua_pop(state_, 1);  // результат CreatePlayer
    lua_pop(state_, 1);  // таблица PlayerManager

    LOG_INFO("EVBots: test live player '%s' team=%d squad=%d",
             name.c_str(), team, squad);
    return true;
}

}  // namespace evbots
