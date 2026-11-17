// VextUtils.cpp — глобальные библиотеки VeniceEXT (VU) для EVBots.
//
// Реализованы две shared-библиотеки документации VU (НЕ по памяти):
//   content\vext\ref\shared\library\mathutils.md
//   content\vext\ref\shared\library\sharedutils.md
//
// Подход тот же, что в MiddleClass.cpp / LuaEvents.cpp: логика пишется на
// Lua и встраивается в DLL строкой (рядом с сервером никаких .lua лежать
// не должно), из C++ в чанк передаются только «источники»:
//   rand()      — равномерное число [0,1) (xorshift64, C);
//   getTimeMs() — миллисекунды GetTickCount64 (C);
//   getTimeNs() — наносекунды QueryPerformanceCounter (C);
//   isServer    — флаг контекста (1 = сервер, 0 = клиент), проброшен из
//                 RegisterVextUtils; от него зависят IsServerModule/IsClientModule
//                 (в нашем рантайме одно состояние Lua на окружение, поэтому
//                 сервер и клиент не сосуществуют в одном состоянии).
//
// Что нельзя сделать без живой игры (возврат Vec3/LinearTransform,
// имя уровня, игровой режим, состав контент-пакетов, память мода) —
// помечено комментарием "TODO: связать с движком" и возвращает значение
// из документа по умолчанию (обычно nil), без выдуманного поведения.

#include "VextUtils.h"

#include <Windows.h>
#include <objbase.h>

#include <cstdint>
#include <cstdio>

#include "EVBotsLog.h"
#include "EngineResources.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// ── случайность (xorshift64*) ─────────────────────────────────────────────
// Один генератор на процесс; ресет на регистрации, если не посеяно.
std::uint64_t gRandState = 0;

std::uint64_t NextRandom() {
    std::uint64_t x = gRandState;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    gRandState = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/// Число [0, 1) из xorshift64: делим результат на 2^64.
int l_vext_rand(lua_State* L) {
    const std::uint64_t r = NextRandom();
    lua_pushnumber(L, static_cast<double>(r) / 18446744073709551616.0);
    return 1;
}

// Обычный вызов без self/аргументов: строка для существующего конструктора Guid.
int l_vext_random_guid(lua_State* L) {
    GUID guid{};
    const HRESULT result = ::CoCreateGuid(&guid);
    if (FAILED(result)) {
        return luaL_error(L, "MathUtils:RandomGuid: CoCreateGuid failed (%d)",
                          static_cast<int>(result));
    }
    char text[37]{};
    std::snprintf(text, sizeof(text),
                  "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  guid.Data1, static_cast<unsigned>(guid.Data2),
                  static_cast<unsigned>(guid.Data3),
                  static_cast<unsigned>(guid.Data4[0]), static_cast<unsigned>(guid.Data4[1]),
                  static_cast<unsigned>(guid.Data4[2]), static_cast<unsigned>(guid.Data4[3]),
                  static_cast<unsigned>(guid.Data4[4]), static_cast<unsigned>(guid.Data4[5]),
                  static_cast<unsigned>(guid.Data4[6]), static_cast<unsigned>(guid.Data4[7]));
    lua_pushlstring(L, text, 36);
    return 1;
}

/// Миллисекунды с момента загрузки ОС (GetTickCount64).
int l_vext_gettime_ms(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(::GetTickCount64()));
    return 1;
}

/// Наносекунды через QueryPerformanceCounter (точный монотонный таймер).
int l_vext_gettime_ns(lua_State* L) {
    static const double kFreq = [] {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart != 0 ? static_cast<double>(f.QuadPart) : 1.0;
    }();
    LARGE_INTEGER c{};
    ::QueryPerformanceCounter(&c);
    const double ns = static_cast<double>(c.QuadPart) * 1e9 / kFreq;
    lua_pushinteger(L, static_cast<lua_Integer>(ns));
    return 1;
}

int l_vext_native_level_info(lua_State* L) {
    char name[512]={},mode[128]={};
    bool authoritative=false;
    const bool ok=eng::NativeLevelIdentity(name,sizeof(name),mode,sizeof(mode),&authoritative);
    if (ok) { lua_pushstring(L,name); lua_pushstring(L,mode); }
    else { lua_pushnil(L); lua_pushnil(L); }
    lua_pushboolean(L,authoritative);
    return 3;
}

// ── Lua: MathUtils и SharedUtils (встроены строкой) ───────────────────────
// Аргументы чанка: rand, getTimeMs, getTimeNs, isServer, nativeLevelInfo, randomGuid.
constexpr char kUtilsSource[] = R"LUA(
-- Аргументы, переданные из C++ при регистрации.
local rand, getTimeMs, getTimeNs, isServer, nativeLevelInfo, randomGuid = ...

-------------------------------------------------------------------------------
-- MathUtils — content\vext\ref\shared\library\mathutils.md
-------------------------------------------------------------------------------
local MathUtils = {}

-- случайное число [0, 1) из генератора C++
local function randomUnit()
  return rand()
end

-- GetRandom(from, to): float — равномерно в [from, to]
function MathUtils:GetRandom(from, to)
  return from + randomUnit() * (to - from)
end

-- GetRandomInt(from, to): int — включительно обе границы, как кличет
-- fun-bots (GetRandomInt(1, #list), GetRandomInt(0, 1) и т.п.)
function MathUtils:GetRandomInt(from, to)
  return math.floor(from + randomUnit() * (to - from + 1))
end

-- GetYPRFromULF(up, left, forward): Vec3 | nil
-- TODO: связать с движком — нужны userdata-тип Vec3 и математика сцены;
-- без зарегистрированного типа Vec3 вернуть настоящий объект нельзя,
-- а выдумывать таблицу-«Vec3» не будем. Док позволяет nil.
function MathUtils:GetYPRFromULF(up, left, forward)
  return nil
end

-- GetTransformFromYPR(yaw, pitch, roll): LinearTransform
-- TODO: связать с движком — возвращает userdata-тип LinearTransform,
-- который в EVBots ещё не реализован; нейтральное значение nil.
function MathUtils:GetTransformFromYPR(yaw, pitch, roll)
  return nil
end

-- Approximately(a, b): bool — сравнение с малым эпсилоном
function MathUtils:Approximately(a, b)
  return math.abs(a - b) < 0.00001
end

-- Clamp(value, min, max): float
function MathUtils:Clamp(value, min, max)
  if value < min then return min end
  if value > max then return max end
  return value
end

-- Round(value): float — округление до ближайшего целого (от половины вверх)
function MathUtils:Round(value)
  return math.floor(value + 0.5)
end

-- FNVHash(text): int — 32-битный FNV-1a (канонический, регистрозависимый):
--   offset basis = 2166136261 (0x811C9DC5), prime = 16777619 (0x01000193),
--   XOR байта string.byte затем умножение, всё по модулю 2^32, по одному
--   байту строки (для UTF-8 — по байтам, как у движка). Реализовано на
--   целочисленной арифметике Lua (64-бит), без плавающих операций.
-- Результат приводится к знаковому int32: движок хранит такие хэши в
-- 32-битных знаковых полях (ср. отрицательные значения в MessageType, см.
-- VextEnums.cpp), и сравнение с ними удобнее в знаковом виде.
function MathUtils:FNVHash(text)
  -- ★ Сборка 186: хеш ДВИЖКА (как у VU: eventId событий сущностей сравнивают с FNVHash("SetTrue") и т. п.). В exe это
  -- djb2-xor: h = 5381; h = h*33 xor байт (mod 2^32). Сверено: «SetTrue» = 0xA4253151, «Activate» = 0xDEAA09DC (cmp в
  -- onEvent 0x00D9C440 / 0x00DE2AB0), «ServerRoundOverMessage» = 0x16BABB47 (как в VextEnums). FNV-1a здесь был неверен.
  local h = 5381
  for i = 1, #text do
    h = ((h * 33) % 4294967296) ~ string.byte(text, i)
  end
  -- движок хранит такие хэши знаковыми int32 -> приводим к знаковому виду
  if h >= 2147483648 then
    h = h - 4294967296
  end
  return h
end

-- Lerp(from, to, t): float — линейная интерполяция без зажатия t
function MathUtils:Lerp(from, to, t)
  return from + (to - from) * t
end

-- RandomGuid(): Guid
-- Генератор ОС и общий тип Guid из VextGeometry; живой движок не требуется.
function MathUtils:RandomGuid()
  return Guid(randomGuid())
end

_G.MathUtils = MathUtils

-------------------------------------------------------------------------------
-- SharedUtils — content\vext\ref\shared\library\sharedutils.md
-------------------------------------------------------------------------------
local SharedUtils = {}

-- Print(data) — вывод в лог EVBots (глобальный print уже перехвачен
-- и пишет в лог, см. LuaDebugConsole.cpp)
function SharedUtils:Print(data)
  print(data)
end

-- GetTime(): int — реальные секунды (GetTickCount64, точность ~1 мс)
function SharedUtils:GetTime()
  return math.floor(getTimeMs() / 1000)
end

-- GetTimeMS(): int — реальные миллисекунды (GetTickCount64)
function SharedUtils:GetTimeMS()
  return getTimeMs()
end

-- GetTimeNS(): int — реальные наносекунды (QueryPerformanceCounter)
function SharedUtils:GetTimeNS()
  return getTimeNs()
end

-- IsClientModule(): bool — наш рантайм работает в одном режиме на процесс,
-- флаг проброшен из C++ (RegisterVextUtils)
function SharedUtils:IsClientModule()
  return not isServer
end

-- IsServerModule(): bool
function SharedUtils:IsServerModule()
  return isServer
end

-- SerializeTable(table): string — рекурсивная сериализация таблицы.
-- Покрывает числа/булевы/строки/вложенные таблицы, защищена от циклов
-- и переполнения глубины. Формат: "ключ=значение," рекурсивно в "{}".
function SharedUtils:SerializeTable(tbl)
  if type(tbl) ~= "table" then
    return tostring(tbl)
  end

  local seen = {}
  local sb = {}

  local function walk(v, depth)
    local t = type(v)
    if t == "number" or t == "boolean" or t == "nil" then
      sb[#sb + 1] = tostring(v)
    elseif t == "string" then
      sb[#sb + 1] = '"' .. v .. '"'
    elseif t == "table" then
      if seen[v] or depth > 10 then
        sb[#sb + 1] = "{...}"
        return
      end
      seen[v] = true
      sb[#sb + 1] = "{"
      for k, val in pairs(v) do
        sb[#sb + 1] = tostring(k)
        sb[#sb + 1] = "="
        walk(val, depth + 1)
        sb[#sb + 1] = ","
      end
      sb[#sb + 1] = "}"
      seen[v] = nil
    else
      -- функция/userdata/thread -> имя типа
      sb[#sb + 1] = "<" .. t .. ">"
    end
  end

  walk(tbl, 0)
  return table.concat(sb, "")
end

-- GetContentPackages(): string{}
-- TODO: связать с движком — вернуть список загруженных контент-пакетов.
function SharedUtils:GetContentPackages()
  return {}
end

-- GetLevelName(): string | nil — «Levels/MP_012/MP_012», как VU.
-- ★ Сборка 185: из движка (NativeBridge.LevelInfo: ServerLevel → LevelSetup). Без моста — nil.
local function engineLevelInfo()
  if nativeLevelInfo then
    local name, mode, authoritative = nativeLevelInfo()
    if authoritative or not isServer then return name, mode end
  end
  local nb = rawget(_G, "NativeBridge")
  if nb == nil or nb.LevelInfo == nil then return nil, nil end
  local ok, name, mode = pcall(nb.LevelInfo)
  if not ok then return nil, nil end
  return name, mode
end

function SharedUtils:GetLevelName()
  local name = engineLevelInfo()
  return name
end

-- GetCurrentGameMode(): string | nil — «TeamDeathMatch0», опция GameMode уровня (сборка 185).
function SharedUtils:GetCurrentGameMode()
  local _, mode = engineLevelInfo()
  return mode
end

-- GetTickrate(): float — частота движка: 30, а с -high60 / -high120 (EVBOTS_FREQUENCY) — 60 / 120, как у VU.
-- Сборка 238: значение даёт ядро ev-core (EVBOTS_TICKRATE), без ядра — 30.
function SharedUtils:GetTickrate()
  return EVBOTS_TICKRATE or 30
end

-- GetMemoryUsage(): int — память мода в байтах
-- TODO: связать с движком — учёт памяти конкретного мода ведёт движок;
-- без него вернуть осмысленное значение нельзя, поэтому 0.
function SharedUtils:GetMemoryUsage()
  return 0
end

_G.SharedUtils = SharedUtils

-- ServerUtils — content\vext\ref\server\library\serverutils.md
if isServer then
  local ServerUtils = {}
  local customMapName = nil
  local customGameModeName = nil

  function ServerUtils:SetCustomMapName(name) customMapName = name end
  function ServerUtils:GetCustomMapName() return customMapName end
  function ServerUtils:ClearCustomMapName() customMapName = nil end
  function ServerUtils:SetCustomGameModeName(name) customGameModeName = name end
  function ServerUtils:GetCustomGameModeName() return customGameModeName end
  function ServerUtils:ClearCustomGameModeName() customGameModeName = nil end
  _G.ServerUtils = ServerUtils
end
)LUA";

}  // namespace

bool RegisterVextUtils(lua_State* L, int serverContext) {
    static const int kSourceLen = static_cast<int>(sizeof(kUtilsSource) - 1);
    if (luaL_loadbufferx(L, kUtilsSource, kSourceLen, "=[vext-utils]", "t") !=
        LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: MathUtils/SharedUtils не собрались: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    // посеять генератор случайных чисел, если ещё не посеян
    if (gRandState == 0) {
        LARGE_INTEGER seed{};
        ::QueryPerformanceCounter(&seed);
        gRandState = static_cast<std::uint64_t>(seed.QuadPart) ^
                     0x9E3779B97F4A7C15ULL;
        if (gRandState == 0) {
            gRandState = 0x123456789ABCDEFULL;
        }
    }

    lua_pushcfunction(L, l_vext_rand);
    lua_pushcfunction(L, l_vext_gettime_ms);
    lua_pushcfunction(L, l_vext_gettime_ns);
    lua_pushboolean(L, serverContext != 0);
    lua_pushcfunction(L, l_vext_native_level_info);
    lua_pushcfunction(L, l_vext_random_guid);
    if (lua_pcall(L, 6, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: MathUtils/SharedUtils не выполнились: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: библиотеки MathUtils и SharedUtils подключены "
             "(контекст %s)",
             serverContext != 0 ? "server" : "client");
    return true;
}

}  // namespace evbots
