// VextResourceManager.cpp — библиотека VeniceEXT ResourceManager.
//
// Глобальная таблица ResourceManager (shared) — по документации VU
// (НЕ по памяти): content\vext\ref\shared\library\resourcemanager.md
//
// В реальном VU ResourceManager — доступ к базе данных ресурсов движка
// (EBX/DataContainer). Здесь он работает по бинарному индексу, который
// строится офлайн из выгрузки Venice-EBX
// (evbots\tools\build_ebx_index.py), поэтому:
//   - SearchForDataContainer(<путь>) ищет именованный инстанс раздела по
//     пути и отдаёт userdata-объект DataContainer с typeInfo.name = класс
//     инстанса (нужно моду fun-bots: EbxEditUtils.lua:19, 42);
//   - SearchForInstanceByGuid(<GUID>) — то же по GUID инстанса;
//   - индекс грузится лениво, один раз, путь задаётся переменной окружения
//     EVBOTS_EBX_INDEX;
//   - RegisterInstanceLoadHandler/RegisterInstanceLoadHandlerOnce/
//     RegisterPartitionLoadHandler хранят обработчики в реестре по (тип-гуид,
//     гуид) и возвращают объект ContainerCallback с Deregister. Реестр
//     пригодится, когда появятся загружаемые уровни/бандлы.
//   - Сборки 161–234 добавили живые ресурсы/разделы/настройки движка.
//   - Сборка 267: MountSuperBundle/AddRegistry — native-вызовы только в своих
//     синхронных фазах загрузки; остальные операции загрузки пока не реализованы.

#include "VextResourceManager.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <new>
#include <string>
#include <vector>
#include <intrin.h>

#include "EngineResources.h"
#include "EVBotsLog.h"
#include "LuaRuntime.h"
#include "ModdingInterface.h"
#include "NativeBlueprint.h"
#include "VextEbxIndex.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// Чанк получает аргументами: функцию логирования и два native-поисковика
// по индексу EBX (nativeSearch / nativeSearchByGuid).
constexpr char kResourceManagerSource[] = R"LUA(
-- ResourceManager VeniceEXT (VU)
-- по content\vext\ref\shared\library\resourcemanager.md

local log, nativeSearch, nativeSearchByGuid, live = ...
EVBotsLiveWrap = live ~= nil and live.wrap or nil  -- сборка 186: данные по адресу (EntityFactory:Create)

local ResourceManager = {}
_G.ResourceManager = ResourceManager

-- ★ Сборка 161: «живые» объекты движка (EngineResources.cpp). live.ready() — слой
-- доступен (сервер, прологи совпали). live.search() — поиск отдаёт живые объекты
-- (EVBOTS_LIVE_SEARCH=1), иначе — офлайн-индекс, как раньше.
local liveReady = live ~= nil and live.ready() or false
local liveSearch = liveReady and live.search() or false

local function guidText(g)
  if g == nil then return nil end
  local s = tostring(g)
  local tail = s:match("([^/]+)$")
  return tail or s
end

-- ── реестр обработчиков: key = partitionGuid .. "|" .. instanceGuid ────────
-- → массив { context, hasContext, callback, alive }
local loadHandlers = {}
local partitionHandlers = {}
local nextCallbackId = 1
local callbacksById = {}

-- Каждый непройденный путь логируем ровно один раз: иначе мод на каждом
-- спавне бота сыпал бы одинаковые WARNING (BotSpawner:_GetCustomization).
local reportedMissing = {}

local function logMissingOnce(kind, key, value)
  if reportedMissing[key] then return end
  reportedMissing[key] = true
  log("WRN ", kind .. "(" .. tostring(value) .. ") — не найдено в индексе EBX")
end

-- ── ContainerCallback (shared\type\containercallback.md) ────────────────────
local ContainerCallback = {}
ContainerCallback.__index = ContainerCallback

function ContainerCallback:Deregister()
  if self._handle then
    -- ★ 161: обработчики теперь вызываются — снятый должен перестать срабатывать
    local rec = callbacksById[self._handle]
    if rec then
      local list = (rec.kind == "instance" and loadHandlers or partitionHandlers)[rec.key]
      for _, e in ipairs(list or {}) do
        if e.handle == self._handle then e.alive = false end
      end
    end
    callbacksById[self._handle] = nil
    self._handle = nil
  end
end

-- Обёртки: xOf(x) показывает полный гуид (для логов).
local function GuidKey(a, b)
  return tostring(a) .. "|" .. tostring(b)
end

-- ── методы с реальным поведением ───────────────────────────────────────────
function ResourceManager:RegisterInstanceLoadHandler(partitionGuid,
                                                     instanceGuid, ...)
  local count = select("#", ...)
  if count < 1 then
    error("ResourceManager:RegisterInstanceLoadHandler: нет обработчика", 2)
  end

  local context, callback
  if count == 1 then
    callback = select(1, ...)
  else
    context, callback = ...
  end

  local function isCallable(v)
    return type(v) == "function" or (type(v) == "table" and v.__call ~= nil)
  end
  if not isCallable(callback) then
    error("ResourceManager:RegisterInstanceLoadHandler: обработчик должен " ..
          "быть функцией", 2)
  end

  local key = GuidKey(partitionGuid, instanceGuid)
  loadHandlers[key] = loadHandlers[key] or {}
  local list = loadHandlers[key]
  local handle = nextCallbackId
  nextCallbackId = nextCallbackId + 1
  list[#list + 1] = { context = context, hasContext = count > 1,
                      callback = callback, alive = true, handle = handle,
                      pg = guidText(partitionGuid), ig = guidText(instanceGuid) }
  callbacksById[handle] = { kind = "instance", key = key }

  log("DBG ", "RegisterInstanceLoadHandler(" .. key .. ") -> cb#" ..
      tostring(handle))

  -- ★ 161: движок сообщит, когда раздел загрузится (перехват onBundlePartition)
  if liveReady then
    live.watch(guidText(partitionGuid), guidText(instanceGuid))
  end

  return setmetatable({ _handle = handle }, ContainerCallback)
end

function ResourceManager:RegisterInstanceLoadHandlerOnce(partitionGuid,
                                                         instanceGuid, ...)
  local cb = self:RegisterInstanceLoadHandler(partitionGuid, instanceGuid, ...)
  local list = loadHandlers[GuidKey(partitionGuid, instanceGuid)]
  if list and list[#list] then list[#list].once = true end
  return cb
end

function ResourceManager:RegisterPartitionLoadHandler(partitionGuid, ...)
  local count = select("#", ...)
  if count < 1 then
    error("ResourceManager:RegisterPartitionLoadHandler: нет обработчика", 2)
  end
  local context, callback
  if count == 1 then
    callback = select(1, ...)
  else
    context, callback = ...
  end

  local key = guidText(partitionGuid)
  partitionHandlers[key] = partitionHandlers[key] or {}
  local list = partitionHandlers[key]
  local handle = nextCallbackId
  nextCallbackId = nextCallbackId + 1
  list[#list + 1] = { context = context, hasContext = count > 1,
                      callback = callback, alive = true, handle = handle }
  callbacksById[handle] = { kind = "partition", key = key }

  log("DBG ", "RegisterPartitionLoadHandler(" .. key .. ") -> cb#" ..
      tostring(handle))
  if liveReady then
    live.watch(key, nil)
  end
  return setmetatable({ _handle = handle }, ContainerCallback)
end

function ResourceManager:RegisterPartitionLoadHandlerOnce(partitionGuid, ...)
  local cb = self:RegisterPartitionLoadHandler(partitionGuid, ...)
  local list = partitionHandlers[guidText(partitionGuid)]
  if list and list[#list] then list[#list].once = true end
  return cb
end

-- ── ★ 161: вызов обработчиков загрузки (как в VU) ─────────────────────────
-- Адрес может повториться после выгрузки. Нативный PostMarshal обновляет
-- поколение раздела, а догоняющий вызов для уже живого инстанса его не меняет.
local firedAt = {}
local fireStats = { fired = 0, failed = 0, missing = 0 }

local function callEntry(e, obj, what)
  if not e.alive then return end
  local ok, err
  if e.hasContext then
    ok, err = pcall(e.callback, e.context, obj)
  else
    ok, err = pcall(e.callback, obj)
  end
  if ok then
    fireStats.fired = fireStats.fired + 1
  else
    fireStats.failed = fireStats.failed + 1
    log("ERR ", "load-хендлер cb#" .. tostring(e.handle) .. " (" .. what .. ") упал: " .. tostring(err))
  end
  if e.once then e.alive = false end
end

local function fireInstance(key, list, obj, how, generation)
  firedAt[key] = { address = live.addr(obj), generation = generation }
  log("INFO", "★ load-хендлер " .. key .. " -> " .. tostring(obj) .. " (" .. how .. ")")
  for _, e in ipairs(list) do callEntry(e, obj, key) end
end

-- Раздел загрузился (зовёт перехват движка; partition — адрес раздела).
function ResourceManager.__OnPartition(pg, partition, generation)
  for key, list in pairs(loadHandlers) do
    local e1 = list[1]
    if e1 and e1.pg == pg then
      local obj = live.inPartition(partition, e1.ig)
      local previous = firedAt[key]
      if obj ~= nil then
        if previous == nil or previous.address ~= live.addr(obj) or
            (generation ~= nil and previous.generation ~= nil and generation ~= previous.generation) then
          fireInstance(key, list, obj, "загрузка раздела", generation)
        elseif previous.generation == nil then
          previous.generation = generation
        end
      end
    end
  end
  local plist = partitionHandlers[pg]
  if plist then
    local p = live.partitionTable(partition)
    log("INFO", "★ partition-хендлер " .. pg .. " (загрузка раздела)")
    for _, e in ipairs(plist) do callEntry(e, p, pg) end
  end
end

-- ★ Сборка 234: событие Partition:Loaded(partition) — каждый загруженный раздел (VU-Docs shared\event\
-- partition_loaded.md). Зовёт кадр движка клиента (очередь из перехвата onBundlePartition, ResourcePartitionEventsDrain).
-- partition.instances — инстансы из хеш-таблицы раздела (live.partitionInstances), лениво — при первом обращении.
-- Отличие от VU (реестр В8): порядок — порядок таблицы движка, не файла EBX.
local instancesMeta = { __index = function(t, k)
  if k ~= "instances" then return nil end
  local v = live.partitionInstances(rawget(t, "_ptr") or 0)
  rawset(t, "instances", v)
  return v
end }
function ResourceManager.__OnAnyPartition(ptr, name)
  local p = live.partitionTable(ptr)
  if p == nil then return end
  p.name = name
  setmetatable(p, instancesMeta)
  if Events ~= nil and Events._DispatchEngine ~= nil then
    local ok, err = pcall(Events._DispatchEngine, Events, "Partition:Loaded", p)
    if not ok then log("WRN ", "Partition:Loaded(" .. tostring(name) .. "): " .. tostring(err)) end
  end
end

-- Эти входы вызывает только native lifecycle hook. Сам вызов функции из Lua
-- не даёт разрешения на Mount/AddRegistry: scope хранится в TLS ресурсного слоя.
function ResourceManager.__OnLoadResources(name, gameMode, dedicated)
  if Events == nil or Events._DispatchEngine == nil then
    error("Level:LoadResources: engine event dispatcher is unavailable")
  end
  Events:_DispatchEngine("Level:LoadResources", name, gameMode, dedicated)
end

function ResourceManager.__OnRegisterResources(ptr)
  local levelData = live.wrap(ptr)
  if levelData == nil then error("Level:RegisterEntityResources: LevelData is unavailable") end
  if Events == nil or Events._DispatchEngine == nil then
    error("Level:RegisterEntityResources: engine event dispatcher is unavailable")
  end
  Events:_DispatchEngine("Level:RegisterEntityResources", levelData)
end

-- Догоняющий вызов: инстанс уже был загружен до регистрации (первый уровень
-- грузится раньше модов). Без него мод не правит данные первого раунда.
function ResourceManager.__FireLate(reason)
  if not liveReady then return end
  local n, miss = 0, 0
  for key, list in pairs(loadHandlers) do
    local e1 = list[1]
    if e1 then
      local obj = live.find(e1.pg, e1.ig)
      local previous = firedAt[key]
      if obj ~= nil and previous ~= nil and previous.address == live.addr(obj) then
        -- тот же инстанс уже обработан (перехватом или прошлым догоняющим)
      elseif obj ~= nil then
        fireInstance(key, list, obj, "догоняющий: " .. tostring(reason))
        n = n + 1
      else
        miss = miss + 1
      end
    end
  end
  fireStats.missing = fireStats.missing + miss
  log("INFO", "load-хендлеры (" .. tostring(reason) .. "): вызвано " .. n .. ", не загружено " .. miss ..
      "; всего вызовов " .. fireStats.fired .. ", ошибок " .. fireStats.failed)
end


-- ── поиск (ресурсов нет) → nil ──────────────────────────────────────────────
local offlineFallback = nil  -- 254: EVBOTS_OFFLINE_SEARCH=1, читается при первом поиске
function ResourceManager:SearchForDataContainer(name)
  if type(name) ~= "string" or name == "" then
    return nil
  end

  -- ★ 161: живой объект движка (как VU), офлайн-индекс — запасной путь
  if liveSearch then
    local s_Live = live.byName(name)
    if s_Live ~= nil then return s_Live end
    -- ★ 252: как VU — ассета нет в загруженных разделах → nil, мод пробует следующий вариант. Прогон 252 (XP4_Parl):
    -- офлайн-индекс отдавал «Visual/MP/Us/…_Ranger» (на XP4 есть только «Visual/MP_XP4/…»), BotSpawner:_FindAppearance
    -- брал его, мост не находил объект — бот без модели. ★ 254: для всех путей (было — только анлоки и киты), как VU:
    -- объект офлайн-индекса в памяти движка не существует, мод получал «найдено» и не пробовал свой следующий вариант.
    -- EVBOTS_OFFLINE_SEARCH=1 — прежний запасной путь (офлайн-индекс) для остального.
    if offlineFallback == nil then
      offlineFallback = NativeBridge ~= nil and NativeBridge.EnvFlag ~= nil and NativeBridge.EnvFlag('EVBOTS_OFFLINE_SEARCH') == true
    end
    if not (offlineFallback and not (name:find("^Persistence/Unlocks/") or name:find("^Gameplay/Kits/"))) then
      logMissingOnce("SearchForDataContainer", "path:" .. name, name)
      return nil
    end
  end

  local s_Instance = nativeSearch(name)

  if s_Instance == nil then
    -- Кэш поиска живёт в моде (BotSpawner:GetResourceDataContainer), нам
    -- повторно логировать нечего.
    logMissingOnce("SearchForDataContainer", "path:" .. name, name)
    return nil
  end

  return s_Instance
end

function ResourceManager:SearchForInstanceByGuid(instanceGuid)
  -- Мод зовёт это с Guid(...), а userdata-типа Guid у нас нет; принимаем
  -- строку с GUID (в т.ч. ссылку вида "путь/GUID" из полей EBX).
  local s_Guid = instanceGuid

  if type(s_Guid) == "userdata" then
    local s_Mt = getmetatable(s_Guid)
    if s_Mt and s_Mt.__tostring then
      s_Guid = tostring(s_Guid)
    else
      return nil
    end
  end

  if type(s_Guid) ~= "string" or s_Guid == "" then
    return nil
  end

  -- Ссылка на инстанс: "Weapons/M67/M67/C569F719-..." -> берём хвост.
  local s_Tail = s_Guid:match("([^/]+)$")
  if s_Tail then
    s_Guid = s_Tail
  end

  if liveSearch then
    local s_Live = live.find(nil, s_Guid)
    if s_Live ~= nil then return s_Live end
  end

  local s_Instance = nativeSearchByGuid(s_Guid)

  if s_Instance == nil then
    logMissingOnce("SearchForInstanceByGuid", "guid:" .. s_Guid, s_Guid)
    return nil
  end

  return s_Instance
end

function ResourceManager:FindInstanceByGuid(partitionGuid, instanceGuid)
  if liveSearch then
    local s_Live = live.find(guidText(partitionGuid), guidText(instanceGuid))
    if s_Live ~= nil then return s_Live end
  end
  return ResourceManager:SearchForInstanceByGuid(instanceGuid)
end

function ResourceManager:FindDatabasePartition(partitionGuid)
  if liveReady then
    return live.partitionByGuid(guidText(partitionGuid))
  end
  return nil
end

function ResourceManager:FindPartitionForInstance(instance)
  if liveReady and live.isLive(instance) then
    return live.partitionOf(instance)
  end
  return nil
end

-- Сборка 267: native операции требуют своей синхронной фазы загрузки. Отказ
-- поднимает Lua-ошибку: пустая функция скрывала отсутствие ресурсов оригинального мода.
function ResourceManager:MountSuperBundle(superbundle, mediaHint, optional)
  return live.mountSuperBundle(superbundle, mediaHint, optional)
end
function ResourceManager:UnmountSuperBundle(...) end
function ResourceManager:GetSettings(settingsType)
  -- ★ 161: настройки-синглтоны движка (только известные адреса, см. EngineResources)
  if liveReady and type(settingsType) == "string" then
    return live.settings(settingsType)
  end
  return nil
end
function ResourceManager:LookupDataContainer(compartment, name)
  -- ★ 161: fb::ResourceManager_lookupDataContainer — раздел по имени в отсеке движка
  if liveReady and type(name) == "string" then
    return live.byName(name, compartment)
  end
  return nil
end
function ResourceManager:AddRegistry(registry, compartment)
  return live.addRegistry(registry, compartment)
end
function ResourceManager:BeginLoadData(...) return 0 end
function ResourceManager:EndLoadData(...) return true end
function ResourceManager:PollBundleOperation(...) return true end
function ResourceManager:CancelBundleOperation(...) end
function ResourceManager:BeginClear(...) return 0 end
function ResourceManager:EndClear(...) end
function ResourceManager:AllocateDynamicCompartment(name, parent, clientOnly)
  log("DBG ", "ResourceManager:AllocateDynamicCompartment(requested) — " ..
      "заглушка, возвращаю таблицу")
  return { name = tostring(name), parent = parent, clientOnly = clientOnly }
end
function ResourceManager:DestroyDynamicCompartment(...) end
function ResourceManager:AlwaysClearGameCompartment(...) end

return ResourceManager
)LUA";

// ═══════════════════════════════════════════════════════════════════════════
// Индекс EBX (реализация EbxIndex — объявление в VextEbxIndex.h)
// ═══════════════════════════════════════════════════════════════════════════

constexpr char kIndexMagic[8] = {'L', 'E', 'B', 'X', 'I', 'X', '0', '2'};
constexpr std::uint32_t kIndexVersion = 2;
constexpr std::size_t kIndexHeaderSize = 64;
constexpr std::size_t kIndexEntrySize = 40;

/// Привести к нижнему регистру ASCII: в путях и GUID регистр не важен, а мод
/// мешает регистр (WEAPONS-RESEARCH.md, 4.2).
std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::uint32_t ReadU32(const char* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::uint64_t ReadU64(const char* p) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

/// Прочитать файл целиком. false — файла нет, он пуст или нечитаем.
bool ReadWholeFile(const std::wstring& path, std::string* out,
                   std::string* error) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = "не удалось открыть файл индекса";
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (1024LL * 1024 * 1024)) {
        ::CloseHandle(h);
        *error = "индекс пуст или подозрительно большой";
        return false;
    }

    out->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ::ReadFile(h, out->data(),
                               static_cast<DWORD>(out->size()), &read,
                               nullptr);
    ::CloseHandle(h);
    if (!ok || read != out->size()) {
        *error = "ошибка чтения файла индекса";
        return false;
    }
    return true;
}

/// Где искать индекс, если EVBOTS_EBX_INDEX не задан.
std::vector<std::wstring> IndexCandidates() {
    std::vector<std::wstring> out;

    // 1. Каталог DLL — сборка лежит рядом с EVBots.dll.
    HMODULE self = nullptr;
    if (::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&IndexCandidates), &self) != 0) {
        wchar_t dir[MAX_PATH] = {};
        const DWORD n = ::GetModuleFileNameW(self, dir, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring full(dir, n);
            const std::size_t slash = full.find_last_of(L'\\');
            if (slash != std::wstring::npos) {
                out.push_back(full.substr(0, slash + 1) + L"ebx_index.bin");
            }
        }
    }

    // 2. Рабочий каталог процесса.
    wchar_t cwd[MAX_PATH] = {};
    if (::GetCurrentDirectoryW(MAX_PATH, cwd) != 0) {
        out.emplace_back(cwd);
        out.back() += L"\\ebx_index.bin";
    }

    return out;
}

}  // namespace

// ── EbxIndex ───────────────────────────────────────────────────────────────

bool EbxIndex::EnsureLoaded() {
    if (loaded_) {
        return true;
    }
    // Пробуем один раз: неудачу запоминаем, чтобы не бить диск на каждом
    // SearchForDataContainer.
    static bool attempted = false;
    if (attempted) {
        return false;
    }
    attempted = true;

    std::vector<std::wstring> candidates;
    wchar_t env[MAX_PATH * 2] = {};
    const DWORD n = ::GetEnvironmentVariableW(
        L"EVBOTS_EBX_INDEX", env, static_cast<DWORD>(std::size(env)));
    if (n > 0 && n < std::size(env)) {
        candidates.emplace_back(env, n);
    }
    for (auto& c : IndexCandidates()) {
        candidates.push_back(std::move(c));
    }

    std::string blob;
    std::string readError;
    for (const std::wstring& c : candidates) {
        std::string tmp;
        if (ReadWholeFile(c, &tmp, &readError)) {
            // Путь индекса для лога — узкая строка, ASCII-пути достаточно.
            path_.clear();
            for (wchar_t ch : c) {
                path_.push_back(ch < 128 ? static_cast<char>(ch) : '?');
            }
            blob = std::move(tmp);
            break;
        }
    }
    if (blob.empty()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "файл индекса не найден (EVBOTS_EBX_INDEX не задан, "
                      "проверено %zu путей)",
                      candidates.size());
        loadError_ = buf;
        return false;
    }

    // Parse() читает blob_ — содержимое прочитанного файла.
    blob_ = std::move(blob);
    return Parse();
}

bool EbxIndex::Parse() {
    loadError_.clear();

    if (blob_.size() < kIndexHeaderSize ||
        std::memcmp(blob_.data(), kIndexMagic, sizeof(kIndexMagic)) != 0) {
        loadError_ = "неверная магия индекса (пересобери build_ebx_index.py)";
        return false;
    }

    const char* h = blob_.data();
    const std::uint32_t version = ReadU32(h + 8);
    if (version != kIndexVersion) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "версия индекса %u, ожидалась %u", version,
                      kIndexVersion);
        loadError_ = buf;
        return false;
    }

    const std::uint32_t count = ReadU32(h + 12);
    const std::uint64_t entriesOff = ReadU64(h + 16);
    const std::uint64_t stringsOff = ReadU64(h + 24);
    const std::uint64_t stringsSize = ReadU64(h + 32);

    if (count == 0) {
        loadError_ = "индекс пуст (0 записей)";
        return false;
    }
    if (stringsOff > blob_.size() ||
        stringsOff + stringsSize > blob_.size() ||
        entriesOff + static_cast<std::uint64_t>(count) * kIndexEntrySize >
            blob_.size()) {
        loadError_ = "индекс обрезан (заголовок не сходится с размером файла)";
        return false;
    }

    const char* strs = blob_.data() + stringsOff;
    const char* strsEnd = strs + stringsSize;
    auto strAt = [strs, strsEnd](std::uint32_t off) -> const char* {
        const char* p = strs + off;
        return (p < strsEnd) ? p : "";
    };

    instances_.clear();
    instances_.reserve(count);
    byPath_.clear();
    byPathLower_.clear();
    byGuid_.clear();
    byGuid_.reserve(count * 2);

    const char* e = blob_.data() + entriesOff;
    for (std::uint32_t i = 0; i < count; ++i, e += kIndexEntrySize) {
        EbxInstance inst;
        inst.path = strAt(ReadU32(e + 4));
        inst.pathLower = ToLowerAscii(inst.path);
        inst.typeName = strAt(ReadU32(e + 8));
        inst.nameField = strAt(ReadU32(e + 12));
        inst.instanceGuid = strAt(ReadU32(e + 16));
        inst.fileRel = strAt(ReadU32(e + 20));
        inst.dataOff = ReadU64(e + 24);
        inst.dataLen = ReadU64(e + 32);
        if (inst.typeName.empty()) {
            continue;  // битая запись — пропускаем
        }

        // Записи бывают двух видов: именованные (непустой path — их видно
        // через SearchForDataContainer) и обычные (path пустой, но есть GUID —
        // на них ссылаются поля, см. EbxInstance::named()).
        const std::size_t idx = instances_.size();
        if (inst.named()) {
            byPath_.emplace(inst.path, idx);
            byPathLower_.emplace(inst.pathLower, idx);
        }
        if (!inst.instanceGuid.empty()) {
            byGuid_.emplace(ToLowerAscii(inst.instanceGuid), idx);
        }
        instances_.push_back(std::move(inst));
    }

    if (instances_.empty()) {
        loadError_ = "в индексе нет пригодных записей";
        return false;
    }

    loaded_ = true;
    LOG_INFO("EVBots: индекс EBX загружен: %zu инстансов "
             "(%zu именованных, доступны по пути) из %s",
             instances_.size(), namedCount(), path_.c_str());
    return true;
}

const EbxInstance* EbxIndex::FindByPath(const std::string& resourcePath) const {
    if (resourcePath.empty()) {
        return nullptr;
    }
    // 1. Точное совпадение — как в данных.
    auto exact = byPath_.find(resourcePath);
    if (exact != byPath_.end()) {
        return &instances_[exact->second];
    }
    // 2. Запасной вариант без учёта регистра: мод шлёт перки как
    //    "persistence/.../sprintboostl2", в данных "Persistence/.../SprintBoostL2".
    auto lower = byPathLower_.find(ToLowerAscii(resourcePath));
    if (lower != byPathLower_.end()) {
        return &instances_[lower->second];
    }
    return nullptr;
}

const EbxInstance* EbxIndex::FindByAlias(
    const std::string& resourcePath) const {
    // Известное расхождение имён в моде: BotSpawner.lua:2199 конкатенирует
    // 'Engi' (типы USEngi/USEngi_GM/USEngi_XP4/...), а в данных комплект
    // инженера называется USEngineer*. Проверено по выгрузке: "Engi" в имени
    // файла Gameplay/Kits не встречается ни разу.
    static const char kPrefix[] = "Gameplay/Kits/";
    static const char kFrom[] = "Engi";
    static const char kTo[] = "Engineer";

    const std::string prefix(kPrefix);
    if (resourcePath.compare(0, prefix.size(), prefix) != 0) {
        return nullptr;
    }
    const std::string tail = resourcePath.substr(prefix.size());

    // Имя комплекта: <команда><тип>, где команда — двухбуквенный код (US/RU),
    // а тип инженера мод пишет как "Engi": USEngi, USEngi_GM, USEngi_XP4...
    // Значит "Engi" начинается не с начала, а после кода команды.
    constexpr std::size_t kTeamLen = 2;
    if (tail.size() < kTeamLen + 4 || tail.compare(kTeamLen, 4, kFrom) != 0) {
        return nullptr;
    }
    // USEngi_GM -> USEngineer_GM
    const std::string fixed =
        prefix + tail.substr(0, kTeamLen) + kTo + tail.substr(kTeamLen + 4);

    const EbxInstance* inst = FindByPath(fixed);
    if (inst == nullptr) {
        return nullptr;
    }
    LOG_WARN("EVBots: путь '%s' не найден, использован алиас '%s' (%s)",
             resourcePath.c_str(), fixed.c_str(), inst->typeName.c_str());
    return inst;
}

const EbxInstance* EbxIndex::FindByGuid(const std::string& guid) const {
    if (guid.empty()) {
        return nullptr;
    }
    const auto it = byGuid_.find(ToLowerAscii(guid));
    return (it == byGuid_.end()) ? nullptr : &instances_[it->second];
}

std::size_t EbxIndex::namedCount() const {
    return byPath_.size();
}

std::string EbxIndex::Describe(const EbxInstance& inst) {
    return inst.typeName + " '" + inst.path + "'";
}

namespace {

// ═══════════════════════════════════════════════════════════════════════════
// DataContainer: userdata-представление EBX-инстанса
// ═══════════════════════════════════════════════════════════════════════════
//
// Требования выведены из кода мода fun-bots:
//   EbxEditUtils.lua:18  type(x) == 'userdata' and x.typeInfo ~= nil
//   EbxEditUtils.lua:19  _G[x.typeInfo.name](x)   — «каст» к типу
//   EbxEditUtils.lua:22  x:MakeWritable()
//   EbxEditUtils.lua:66  x['Поле']
// Имена свойств и методов — по VU-Docs content\vext\ref\shared\type\
// datacontainer.md и typeinformation.md, не по памяти.

constexpr const char kDcMeta[] = "vext.DataContainer";

/// Что лежит в поле инстанса. Вид определяется разбором блока .txt:
///   * скаляр — строка/число/булево/null;
///   * ссылка  — "путь/GUID" или голый GUID: отдаём DataContainer другого
///     инстанса (именно это нужно моду для Weapon.Object.WeaponFiring...,
///     WeaponClass.lua:51-65);
///   * структура "Имя::Тип" — вложенный блок, тоже отдаём DataContainer
///     (например Shot::ShotConfigData -> .ProjectileData);
///   * массив "Имя::array" — элементы member(N), доступны по целому индексу.
enum class DcKind { Scalar, Reference, Struct, Array };

/// Одно поле инстанса.
struct DcField {
    std::string name;    ///< имя поля как в данных ("Weapon", "Name", "0")
    std::string value;   ///< значение как в данных (скаляр или ссылка)
    DcKind kind = DcKind::Scalar;

    bool isNumber = false;
    bool isBool = false;
    bool isNull = false;  ///< *nullGuid*, *nullArray* и подобные
    double number = 0.0;

    std::string typeName;       ///< класс вложенной структуры ("ShotConfigData")
    std::uint64_t blockOff = 0; ///< смещение вложенного блока внутри .txt
    std::uint64_t blockLen = 0; ///< длина вложенного блока
};

/// Распарсенный (лениво) объект: инстанс, вложенная структура или массив.
struct EbxDc {
    const EbxInstance* inst = nullptr;  ///< nullptr у вложенных объектов
    std::string typeName;               ///< класс для typeInfo.name
    std::string fileRel;                ///< где лежит блок
    std::uint64_t off = 0;
    std::uint64_t len = 0;
    bool isArray = false;  ///< доступ по целочисленным индексам
    bool writable = false;
    std::vector<DcField> fields;
    bool fieldsParsed = false;
};

/// Прочитать кусок файла выгрузки EBX (лениво, только по запросу поля).
///
/// off/len — абсолютные смещение и длина внутри .txt. Для инстанса это
/// dataOff/dataLen из индекса, для вложенной структуры или элемента массива —
/// вычисленные при разборе блока. Так переиспользуется ОДИН и тот же код
/// чтения для всех трёх уровней.
std::string ReadBlock(const std::string& fileRel, std::uint64_t off,
                      std::uint64_t len) {
    if (fileRel.empty() || off == 0 || len == 0) {
        return {};
    }

    std::wstring root;
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetEnvironmentVariableW(
        L"EVBOTS_EBX_ROOT", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        root.assign(buf, n);
    } else {
        root = L"C:\\Games\\EVBots-dev\\Venice-EBX";
    }

    // Пути в индексе ASCII (гарантия утилиты сборки).
    std::string full;
    full.reserve(root.size() + fileRel.size() + 2);
    for (wchar_t c : root) {
        full.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    full.push_back('\\');
    full += fileRel;

    HANDLE h = ::CreateFileA(full.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        off >= static_cast<std::uint64_t>(size.QuadPart)) {
        ::CloseHandle(h);
        return {};
    }

    const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(
        len, 4u * 1024 * 1024));
    if (want == 0) {
        ::CloseHandle(h);
        return {};
    }

    std::string out(want, '\0');
    DWORD done = 0;
    BOOL ok = TRUE;
    // Блок может не влезть в один ReadFile — читаем по частям.
    while (done < want) {
        OVERLAPPED ov{};
        const std::uint64_t at = off + done;
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        ok = ::ReadFile(h, out.data() + done, want - done, &got, &ov);
        if (!ok || got == 0) {
            break;
        }
        done += got;
    }
    ::CloseHandle(h);

    out.resize(done);
    return done > 0 ? out : std::string();
}

/// Разобрать значение поля. Форматы (проверено на Weapons\M416\U_M416.txt):
///   UnlockScore 0                        — целое
///   AutoAvailable False                  — булево
///   NextLevelUnlockAsset *nullGuid*      — null
///   Identifier 3781439326 (...) #unsigned — число с комментарием и флагом
///   Name Weapons/M416/U_M416              — строка
void ClassifyValue(const std::string& raw, DcField* field) {
    std::string v = raw;

    const std::size_t hash = v.find('#');
    if (hash != std::string::npos) {
        v = v.substr(0, hash);  // хвостовой флаг "#unsigned"
    }
    const std::size_t paren = v.find(" (");
    if (paren != std::string::npos) {
        v = v.substr(0, paren);  // комментарий "3781439326 (путь)"
    }

    // Выгрузка EBX — CRLF, поэтому \r надо снять ДО проверки на число,
    // иначе "0\r" не распознаётся и поле уходит в строки.
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' ||
                          v.back() == ' ' || v.back() == '\t')) {
        v.pop_back();
    }
    const std::size_t begin = v.find_first_not_of(" \t\r\n");
    v = (begin == std::string::npos) ? std::string() : v.substr(begin);
    field->value = v;

    if (v.empty() || v == "*nullGuid*" || v == "*nullArray*" ||
        v == "*nullString*" || v == "*null*") {
        field->isNull = true;
        return;
    }
    if (v == "True" || v == "False") {
        field->isBool = true;
        field->number = (v == "True") ? 1.0 : 0.0;
        return;
    }

    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end != nullptr && *end == '\0' && !v.empty()) {
        field->isNumber = true;
        field->number = d;
    }
}

/// Похоже ли значение на GUID Frostbite (8-4-4-4-12 в hex).
bool LooksLikeGuid(const std::string& s) {
    if (s.size() != 36) {
        return false;
    }
    static const int kDashAt[4] = {8, 13, 18, 23};
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        bool dash = false;
        for (int d : kDashAt) {
            if (static_cast<std::size_t>(d) == i) {
                dash = true;
                break;
            }
        }
        if (dash) {
            if (c != '-') {
                return false;
            }
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

/// Значение-ссылка на другой инстанс?
///
/// В данных встречаются три формы (проверено на Weapons\M416\M416.txt,
/// Weapons\M416\U_M416.txt):
///     Object 793FC225-DC6A-974F-CBCA-E431F432756E              — тот же раздел;
///     Weapon Weapons/M416/M416/8775DC85-7E41-5821-C9F8-D4A144884C5E  — «путь/GUID»;
///     AIData AI/Weapons/Handheld_rif_AI_Weapon/D0941902-...      — другой раздел.
/// Во всех трёх хвост — GUID, по которому инстанс ищется в индексе.
bool AsReference(const std::string& value, std::string* guid) {
    if (LooksLikeGuid(value)) {
        *guid = value;
        return true;
    }
    if (value.size() > 37 && value[value.size() - 37] == '/') {
        const std::string tail = value.substr(value.size() - 36);
        if (LooksLikeGuid(tail)) {
            *guid = tail;
            return true;
        }
    }
    return false;
}

/// Разобрать блок в список свойств.
///
/// Формат блока (проверено на Weapons\M416\M416.txt, Weapons\M416\U_M416.txt):
///
///     SoldierWeaponUnlockAsset 3A6B6A16-... #primary instance
///         $::UnlockAssetBase        <- цепочка базовых классов
///             $::Asset
///                 $::DataContainer
///                 Name Weapons/M416/U_M416     <- свойство базового класса
///             DebugUnlockId U_M416             <- тоже доступно с инстанса
///         Weapon Weapons/M416/M416/8775DC85-...  <- ссылка на другой инстанс
///         Shot::ShotConfigData                 <- вложенная структура
///             ProjectileData Weapons/Common/Bullets/.../6C055C66-...
///         UnlockParts::array                    <- массив
///             member(0) A35669D3-...
///
/// Правила:
///   * «$::Тип» — цепочка наследования, полем не является;
///   * «Имя::Тип» — вложенная структура: запоминаем её блок (смещение и
///     длину), содержимое пропускаем — к нему полезим по требованию;
///   * «Имя::array» — массив, элементы читаются по целочисленному индексу;
///   * «member(N)» — элемент массива, имя поля = "N";
///   * «Имя значение» — скаляр либо ссылка, если значение похоже на GUID.
///
/// blockOff — смещение блока В ФАЙЛЕ, нужно вложенным структурам, чтобы
/// добраться до их содержимого тем же кодом чтения.
void ParseFields(const std::string& block, std::uint64_t blockOff,
                 bool arrayItems, std::vector<DcField>* out) {
    std::size_t pos = 0;
    // Абсолютное смещение, до которого пропускаем содержимое вложенного
    // контейнера; 0 — пропускаем ничего.
    std::uint64_t skipUntil = 0;

    while (pos < block.size()) {
        const std::size_t lineStart = pos;
        std::size_t eol = block.find('\n', pos);
        if (eol == std::string::npos) {
            eol = block.size();
        }
        const std::string line = block.substr(pos, eol - pos);
        pos = eol + 1;

        if (skipUntil != 0 && blockOff + lineStart < skipUntil) {
            continue;  // содержимое вложенного контейнера
        }
        skipUntil = 0;

        // Отступ = число ведущих табов.
        int indent = 0;
        while (indent < static_cast<int>(line.size()) && line[indent] == '\t') {
            ++indent;
        }
        if (indent >= static_cast<int>(line.size()) || indent == 0) {
            continue;  // пустая строка или заголовок инстанса
        }

        std::string body = line.substr(indent);
        // Выгрузка CRLF: без этого «Shot::ShotConfigData\r» дал бы имя класса
        // с хвостовым CR, и сравнение с 'ShotConfigData' не прошло бы.
        while (!body.empty() && (body.back() == '\r' || body.back() == '\n' ||
                                 body.back() == ' ' || body.back() == '\t')) {
            body.pop_back();
        }
        if (body.compare(0, 3, "$::") == 0) {
            continue;  // цепочка базовых классов
        }

        const std::size_t sp = body.find_first_of(" \t");
        std::string name = (sp == std::string::npos) ? body : body.substr(0, sp);
        if (name.empty()) {
            continue;
        }

        // Элемент массива: member(N) значение или member(N)::Тип.
        bool isMember = false;
        if (name.compare(0, 7, "member(") == 0) {
            const std::size_t close = name.find(')');
            if (close == std::string::npos) {
                continue;
            }
            name = name.substr(7, close - 7);  // "N"
            isMember = true;
        }
        if (!isMember && !arrayItems) {
            // Вне массива member(N) не встречается; если встретился — пропуск.
            if (name.compare(0, 7, "member(") == 0) {
                continue;
            }
        }

        const std::string::size_type dcolon = name.find("::");
        if (dcolon != std::string::npos) {
            // "Имя::Тип" / "Имя::array" — контейнер. Ищем конец его блока:
            // первая следующая строка с отступом <= текущего.
            const std::uint64_t childStart = blockOff + pos;
            std::size_t scan = pos;
            std::uint64_t childEnd = blockOff + block.size();
            while (scan < block.size()) {
                std::size_t e2 = block.find('\n', scan);
                if (e2 == std::string::npos) {
                    e2 = block.size();
                }
                int ind2 = 0;
                while (ind2 < static_cast<int>(e2 - scan) &&
                       block[scan + ind2] == '\t') {
                    ++ind2;
                }
                if (ind2 <= indent && e2 > scan) {
                    childEnd = blockOff + scan;
                    break;
                }
                scan = e2 + 1;
            }

            const std::string type = name.substr(dcolon + 2);
            DcField field;
            field.name = name.substr(0, dcolon);
            field.typeName = (type == "array") ? std::string() : type;
            field.kind =
                (type == "array") ? DcKind::Array : DcKind::Struct;
            field.blockOff = childStart;
            field.blockLen = (childEnd > childStart) ? childEnd - childStart : 0;
            if (!field.name.empty()) {
                out->push_back(std::move(field));
            }
            skipUntil = childEnd;
            continue;
        }

        DcField field;
        field.name = name;
        const std::string raw = (sp == std::string::npos)
                                    ? std::string()
                                    : body.substr(sp + 1);
        ClassifyValue(raw, &field);
        if (!field.isNull) {
            std::string guid;
            if (AsReference(field.value, &guid)) {
                field.kind = DcKind::Reference;
                field.value = guid;
            }
        }
        if (!field.name.empty()) {
            out->push_back(std::move(field));
        }
    }
}

/// Лениво разобрать поля объекта (один раз на инстанс/структуру/массив).
/// Работает для всех трёх уровней: там, где inst != nullptr, берём блок из
/// индекса, иначе — из fileRel/off/len (вложенная структура или массив).
void EnsureFields(EbxDc* dc) {
    if (dc->fieldsParsed) {
        return;
    }
    dc->fieldsParsed = true;

    std::uint64_t off = dc->off;
    std::uint64_t len = dc->len;
    std::string file = dc->fileRel;
    if (dc->inst != nullptr) {
        off = dc->inst->dataOff;
        len = dc->inst->dataLen;
        file = dc->inst->fileRel;
    }
    if (len == 0) {
        return;
    }

    const std::string block = ReadBlock(file, off, len);
    if (!block.empty()) {
        ParseFields(block, off, dc->isArray, &dc->fields);
    }
}

EbxDc* CheckDc(lua_State* L, int idx) {
    return static_cast<EbxDc*>(luaL_checkudata(L, idx, kDcMeta));
}

/// typeInfo кэшируем в реестре Lua по имени класса: мод читает obj.typeInfo
/// многократно. Таблица должна быть СВОЯ у каждого класса, иначе второй
/// класс перезапишет name у первого.
constexpr const char kTypeInfoRegistry[] = "vext.DataContainer.typeInfo";

void PushTypeInfo(lua_State* L, const char* className) {
    lua_getfield(L, LUA_REGISTRYINDEX, kTypeInfoRegistry);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kTypeInfoRegistry);
    }

    lua_getfield(L, -1, className);
    if (lua_istable(L, -1)) {
        lua_remove(L, -2);  // убрать кэш, оставить typeInfo
        return;
    }
    lua_pop(L, 1);

    lua_createtable(L, 0, 6);
    lua_pushstring(L, className);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, "TypeInformation");
    lua_setfield(L, -2, "typeName");
    lua_newtable(L);
    lua_setfield(L, -2, "fields");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "array");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "enum");
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, className);  // запомнить в кэше
    lua_remove(L, -2);               // убрать кэш-таблицу
}

bool LiveRead(unsigned long addr, void* out, unsigned n);

// Реальные TypeInfo кэшируются по адресу, отдельно от офлайн-имён. Таблица
// попадает в кэш до рекурсии: поле может ссылаться на свой класс или его массив.
void PushNativeTypeInfo(lua_State* L, unsigned long type, unsigned depth = 0) {
    luaL_checkstack(L, 16, "TypeInformation recursion");
    eng::TypeDesc td;
    if (type == 0 || depth > 64 || !eng::Describe(type, &td)) {
        lua_pushnil(L);
        return;
    }
    constexpr const char* registry = "vext.NativeTypeInfo";
    lua_getfield(L, LUA_REGISTRYINDEX, registry);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, registry);
    }
    const int cache = lua_gettop(L);
    lua_rawgeti(L, cache, type);
    if (lua_istable(L, -1)) { lua_remove(L, cache); return; }
    lua_pop(L, 1);
    lua_createtable(L, 0, 7);
    const int info = lua_gettop(L);
    lua_pushvalue(L, info);
    lua_rawseti(L, cache, type);
    lua_pushstring(L, td.name);
    lua_setfield(L, info, "name");
    lua_pushstring(L, "TypeInformation");
    lua_setfield(L, info, "typeName");
    lua_pushboolean(L, td.kind == eng::kArray || td.kind == eng::kFixedArray);
    lua_setfield(L, info, "array");
    lua_pushboolean(L, td.kind == eng::kEnum);
    lua_setfield(L, info, "enum");
    PushNativeTypeInfo(L, td.super, depth + 1);
    lua_setfield(L, info, "super");
    PushNativeTypeInfo(L, td.element, depth + 1);
    lua_setfield(L, info, "elementType");
    lua_createtable(L, static_cast<int>(td.fieldCount), 0);
    const int fields = lua_gettop(L);
    if (td.kind == eng::kClass || td.kind == eng::kValueType || td.kind == eng::kEnum) {
        for (unsigned i = 0; td.fields && i < td.fieldCount; ++i) {
            // FieldInfoData/EnumFieldInfoData имеют размер 12; enum value по +8.
            unsigned long record[3]{};
            if (!LiveRead(td.fields + i * 12, record, sizeof(record)) || !record[0]) continue;
            char name[256]{};
            unsigned n = 0;
            bool readable = true;
            for (; n < sizeof(name) - 1; ++n) {
                if (!LiveRead(record[0] + n, name + n, 1)) { readable = false; break; }
                if (name[n] == 0) break;
            }
            if (!readable || n == 0 || n == sizeof(name) - 1 || name[n] != 0) continue;
            lua_createtable(L, 0, 3);
            lua_pushlstring(L, name, n);
            lua_setfield(L, -2, "name");
            lua_pushinteger(L, td.kind == eng::kEnum ? static_cast<int>(record[2]) : 0);
            lua_setfield(L, -2, "value");
            if (td.kind != eng::kEnum) {
                PushNativeTypeInfo(L, record[2], depth + 1);
                lua_setfield(L, -2, "typeInfo");
            }
            lua_rawseti(L, fields, i + 1);
        }
    }
    lua_setfield(L, info, "fields");
    lua_remove(L, cache);
}

// ── методы DataContainer (datacontainer.md) ────────────────────────────────

/// Доступ к индексу из кода разыменовки. Индекс один на процесс и
/// thread-safe под локом LuaRuntime::Gate() (единственный писатель lua_State).
EbxIndex& Ebx() {
    static EbxIndex index;
    return index;
}

/// Создать userdata для инстанса индекса.
bool PushDataContainer(lua_State* L, const EbxInstance* inst);

/// Создать userdata для вложенной структуры или массива (определение ниже).
void PushNestedDc(lua_State* L, const std::string& fileRel, std::uint64_t off,
                  std::uint64_t len, const std::string& typeName,
                  bool isArray);

/// Деструктор userdata (определение ниже).
int Dc_Gc(lua_State* L);

/// Создать userdata DataContainer.
///
/// ВНИМАНИЕ: у EbxDc есть std::string и std::vector, поэтому память
/// userdata нужно не только выделить, но и КОНСТРУИРОВАТЬ (placement new) —
/// иначе присваивание в неинициализированные строки портит кучу. Освобождает
/// память __gc (Dc_Gc).
EbxDc* AllocDc(lua_State* L) {
    void* mem = lua_newuserdatauv(L, sizeof(EbxDc), 0);
    EbxDc* dc = new (mem) EbxDc();
    luaL_setmetatable(L, kDcMeta);
    return dc;
}

/// Ссылка не разрешилась: в индексе нет инстанса с таким GUID.
/// Каждый GUID логируем один раз, иначе обход 125 стволов заспамит лог.
void ReportUnresolvedRef(const std::string& guid) {
    static std::unordered_map<std::string, bool> reported;
    if (reported.emplace(guid, true).second) {
        LOG_WARN("EVBots: ссылка на инстанс %s не разрешилась "
                 "(нет такого GUID в индексе EBX)", guid.c_str());
    }
}

int Dc_MakeWritable(lua_State* L) {
    CheckDc(L, 1)->writable = true;
    return 0;  // void
}

int Dc_Is(lua_State* L) {
    EbxDc* dc = CheckDc(L, 1);
    const char* typeName = luaL_checkstring(L, 2);
    lua_pushboolean(L, std::strcmp(dc->typeName.c_str(), typeName) == 0);
    return 1;
}

int Dc_Eq(lua_State* L) {
    EbxDc* a = CheckDc(L, 1);
    void* bp = luaL_testudata(L, 2, kDcMeta);
    const EbxDc* b = static_cast<const EbxDc*>(bp);
    // Ссылочное равенство: у верхнего уровня это тот же инстанс индекса;
    // у вложенной структуры — тот же блок в том же файле.
    bool same = false;
    if (b != nullptr) {
        if (a->inst != nullptr && b->inst != nullptr) {
            same = (a->inst == b->inst);
        } else {
            same = (a->off == b->off) && (a->len == b->len) &&
                   (a->fileRel == b->fileRel);
        }
    }
    lua_pushboolean(L, same);
    return 1;
}

int Dc_AsDC(lua_State* L) {
    lua_pushvalue(L, 1);  // upcast не меняет сам userdata
    return 1;
}

int Dc_Clone(lua_State* L) {
    EbxDc* src = CheckDc(L, 1);
    EbxDc* copy = AllocDc(L);
    copy->inst = src->inst;
    copy->typeName = src->typeName;
    copy->fileRel = src->fileRel;
    copy->off = src->off;
    copy->len = src->len;
    copy->isArray = src->isArray;
    copy->fieldsParsed = false;
    copy->writable = true;
    return 1;
}

int Dc_Nop(lua_State* L) {
    (void)L;
    return 0;  // ReplaceReferences: ссылок вне раздела у нас нет
}

int Dc_Nil(lua_State* L) {
    (void)L;
    lua_pushnil(L);  // RegisterLoadHandler: инстанс уже загружен
    return 1;
}

/// _G[ИмяКласса](inst) — «каст» к типу. Мод использует его только чтобы
/// получить типизированный объект (EbxEditUtils.lua:19, 42, 70); набор полей
/// не меняется, поэтому возвращаем сам инстанс.
constexpr const char kLiveMeta[] = "vext.LiveDataContainer";  // ★ 161: объект в памяти движка
constexpr const char kLiveArrMeta[] = "vext.LiveArray";

int Dc_Cast(lua_State* L) {
    if (luaL_testudata(L, 1, kLiveMeta) != nullptr) {
        lua_pushvalue(L, 1);  // живой объект: тип и поля берутся из рефлексии движка
        return 1;
    }
    if (luaL_testudata(L, 1, kDcMeta) == nullptr) {
        return luaL_error(L, "cast: ожидался DataContainer, получено %s",
                          luaL_typename(L, 1));
    }
    lua_pushvalue(L, 1);
    return 1;
}

/// Зарегистрировать конструктор-«каст» для класса в _G. Мод делает
/// _G[typeInfo.name](x) (EbxEditUtils.lua:19). Если имя уже занято (например
/// VextCustomization задаёт SoldierWeaponUnlockAsset/UnlockAsset) — не
/// перебиваем чужое определение.
void RegisterCastGlobal(lua_State* L, const char* className) {
    if (className == nullptr || *className == '\0') {
        return;
    }
    lua_getglobal(L, className);
    const bool exists = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (exists) {
        return;
    }
    lua_pushcfunction(L, Dc_Cast);
    lua_setglobal(L, className);
}

/// Краткий комментарий-разделитель: ниже идёт выдача значения поля.

void PushFieldValue(lua_State* L, const DcField& f) {
    if (f.isNull || f.value.empty()) {
        lua_pushnil(L);
    } else if (f.isBool) {
        lua_pushboolean(L, f.number != 0.0);
    } else if (f.isNumber) {
        lua_pushnumber(L, f.number);
    } else {
        lua_pushlstring(L, f.value.data(), f.value.size());
    }
}

/// Найти поле по имени.
///
/// Сначала точное совпадение, затем — без учёта регистра. Это не прихоть:
/// мод через EbxEditUtils:FormatMemberName приводит имена к виду
/// «weaponFiring», «shot», «projectileData», а в данных они «WeaponFiring»,
/// «Shot», «ProjectileData» (EbxEditUtils.lua:217-253). Без второго прохода
/// learnStatsValues не проходит ни одного шага.
const DcField* FindField(const std::vector<DcField>& fields,
                         const char* key) {
    for (const DcField& f : fields) {
        if (f.name == key) {
            return &f;
        }
    }
    const std::string lowerKey = ToLowerAscii(key);
    for (const DcField& f : fields) {
        if (ToLowerAscii(f.name) == lowerKey) {
            return &f;
        }
    }
    return nullptr;
}

/// В каком .txt лежит блок объекта. У верхнего уровня путь берётся из
/// записи индекса, у вложенной структуры — из её собственного поля.
const std::string& DcFileRel(const EbxDc* dc) {
    return (dc->inst != nullptr) ? dc->inst->fileRel : dc->fileRel;
}

int Dc_Index(lua_State* L) {
    EbxDc* dc = CheckDc(L, 1);
    const char* rawKey = luaL_checkstring(L, 2);

    // ── свойства типа (datacontainer.md) ──
    if (std::strcmp(rawKey, "typeInfo") == 0) {
        PushTypeInfo(L, dc->typeName.c_str());
        return 1;
    }
    if (std::strcmp(rawKey, "isReadOnly") == 0) {
        lua_pushboolean(L, !dc->writable);
        return 1;
    }
    if (std::strcmp(rawKey, "isLazyLoaded") == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // Asset.name (shared\type\asset.md): путь ассета. Мод читает kit.name / weapon.name; поле Name лежит во вложенной
    // базовой структуре `$::Asset`, а базовые структуры в списке полей не разворачиваются — отдаём имя из индекса.
    if (std::strcmp(rawKey, "name") == 0 && dc->inst != nullptr) {
        const std::string& nm =
            !dc->inst->nameField.empty() ? dc->inst->nameField : dc->inst->path;
        if (!nm.empty()) {
            lua_pushlstring(L, nm.data(), nm.size());
            return 1;
        }
    }
    if (std::strcmp(rawKey, "instanceGuid") == 0) {
        if (dc->inst == nullptr || dc->inst->instanceGuid.empty()) {
            lua_pushnil(L);
        } else {
            lua_pushstring(L, dc->inst->instanceGuid.c_str());
        }
        return 1;
    }
    // partitionGuid / partition: GUID раздела в индексе не сохранялся
    // (ссылки его не носят) и тип DatabasePartition в проекте отсутствует —
    // честный nil, а не выдуманное значение.
    if (std::strcmp(rawKey, "partitionGuid") == 0 ||
        std::strcmp(rawKey, "partition") == 0) {
        lua_pushnil(L);
        return 1;
    }

    // ── методы типа ──
    static const struct {
        const char* name;
        lua_CFunction fn;
    } kMethods[] = {
        {"MakeWritable", Dc_MakeWritable},
        {"Is", Dc_Is},
        {"Eq", Dc_Eq},
        {"AsDC", Dc_AsDC},
        {"Clone", Dc_Clone},
        {"ReplaceReferences", Dc_Nop},
        {"RegisterLoadHandler", Dc_Nil},
        {"RegisterLoadHandlerOnce", Dc_Nil},
    };
    for (const auto& m : kMethods) {
        if (std::strcmp(rawKey, m.name) == 0) {
            lua_pushcfunction(L, m.fn);
            return 1;
        }
    }

    // ── поля объекта ──
    EnsureFields(dc);
    const DcField* field = FindField(dc->fields, rawKey);
    if (field == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    switch (field->kind) {
        case DcKind::Struct:
            PushNestedDc(L, DcFileRel(dc), field->blockOff, field->blockLen,
                         field->typeName, false);
            return 1;
        case DcKind::Array:
            PushNestedDc(L, DcFileRel(dc), field->blockOff, field->blockLen,
                         field->typeName, true);
            return 1;
        case DcKind::Reference: {
            // Ссылка «путь/GUID»: отдаём полноценный DataContainer того
            // инстанса. Мод идёт по ним, как по контейнерам
            // (WeaponClass.lua:51-65), поэтому вернуть строку нельзя.
            const EbxInstance* target = Ebx().FindByGuid(field->value);
            if (target == nullptr) {
                ReportUnresolvedRef(field->value);
                lua_pushnil(L);
                return 1;
            }
            PushDataContainer(L, target);
            return 1;
        }
        case DcKind::Scalar:
        default:
            PushFieldValue(L, *field);
            return 1;
    }
}

int Dc_NewIndex(lua_State* L) {
    EbxDc* dc = CheckDc(L, 1);
    const char* key = luaL_checkstring(L, 2);
    const char* where = dc->inst != nullptr ? dc->inst->path.c_str()
                                            : dc->typeName.c_str();
    if (!dc->writable) {
        return luaL_error(L,
                          "DataContainer '%s' read-only: нельзя присвоить '%s' "
                          "(сначала MakeWritable)",
                          where, key);
    }
    return luaL_error(L, "DataContainer: запись в поле '%s' не поддерживается",
                      key);
}

int Dc_ToString(lua_State* L) {
    EbxDc* dc = CheckDc(L, 1);
    if (dc->inst == nullptr) {
        // Вложенная структура или массив — пути у него нет.
        lua_pushfstring(L, "DataContainer(%s @%s+%d)", dc->typeName.c_str(),
                        dc->fileRel.c_str(), static_cast<int>(dc->off));
    } else if (dc->inst->named()) {
        lua_pushfstring(L, "DataContainer(%s '%s')", dc->inst->typeName.c_str(),
                        dc->inst->path.c_str());
    } else {
        lua_pushfstring(L, "DataContainer(%s %s)", dc->inst->typeName.c_str(),
                        dc->inst->instanceGuid.c_str());
    }
    return 1;
}

/// Создать метатаблицу DataContainer и глобальный тип DataContainer.
bool RegisterDataContainer(lua_State* L) {
    // luaL_newmetatable возвращает 1, если создала, и 0, если имя уже
    // занято (повторная регистрация при Initialize). В обоих случаях на
    // вершине стека — сама метатаблица.
    const bool created = luaL_newmetatable(L, kDcMeta) != 0;
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        LOG_ERR("EVBots: метатаблица %s уже занята не таблицей", kDcMeta);
        return false;
    }

    lua_pushcfunction(L, Dc_Index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Dc_NewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Dc_ToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Dc_Gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "vext.datacontainer");
    lua_setfield(L, -2, "__name");

    // Сам тип, как в VU (datacontainer.md, Static members).
    lua_pushvalue(L, -1);
    lua_setglobal(L, "DataContainer");

    lua_pop(L, 1);  // метатаблица

    LOG_INFO("EVBots: DataContainer userdata %s (метатаблица %s)",
             created ? "создан" : "уже был", kDcMeta);
    return true;
}

/// Создать userdata для инстанса индекса.
///
/// Указатель на запись стабилен: индекс живёт весь процесс.
bool PushDataContainer(lua_State* L, const EbxInstance* inst) {
    if (inst == nullptr) {
        return false;
    }
    EbxDc* dc = AllocDc(L);
    dc->inst = inst;
    dc->typeName = inst->typeName;
    dc->writable = false;
    dc->fieldsParsed = false;

    RegisterCastGlobal(L, inst->typeName.c_str());
    return true;
}

/// Создать userdata для вложенной структуры или массива внутри файла.
///
/// Мод обходит их так же, как инстансы (EbxEditUtils.lua:66-70) и делает
/// _G[typeInfo.name](x) на результате, поэтому это тот же тип userdata —
/// с typeInfo.name, полями и MakeWritable. Иначе «каст» упал бы на nil.
void PushNestedDc(lua_State* L, const std::string& fileRel, std::uint64_t off,
                  std::uint64_t len, const std::string& typeName,
                  bool isArray) {
    EbxDc* dc = AllocDc(L);
    dc->inst = nullptr;
    dc->typeName = typeName;
    dc->fileRel = fileRel;
    dc->off = off;
    dc->len = len;
    dc->isArray = isArray;
    dc->writable = false;
    dc->fieldsParsed = false;

    RegisterCastGlobal(L, typeName.c_str());
}

/// Освободить userdata: у него нет внешних владельцев, но нужно вызвать
/// деструкторы EbxDc, иначе строки и поля утекуют.
int Dc_Gc(lua_State* L) {
    EbxDc* dc = static_cast<EbxDc*>(luaL_checkudata(L, 1, kDcMeta));
    dc->~EbxDc();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// ★ Сборка 161: ЖИВОЙ DataContainer — объект в памяти движка, поля через его
// рефлексию (TypeInfo/FieldInfoData, EngineResources.cpp). Так работает VU:
// мод получает настоящий инстанс, MakeWritable + присваивание пишут в движок.
// Офлайн-DataContainer (индекс EBX) остаётся запасным путём поиска.
// ═══════════════════════════════════════════════════════════════════════════

enum LiveShape : unsigned char { kShapeInstance = 0, kShapeStruct = 1, kShapeArray = 2 };

struct LiveDc {  // POD: живёт в userdata без деструктора
    unsigned long ptr;    ///< инстанс | начало структуры | адрес поля-массива
    unsigned long type;   ///< TypeInfo (у массива — тип массива)
    unsigned char shape;
    unsigned char writable;
    unsigned char owned;  // one native reference; existing prefix/sizeof(12) stays ABI-compatible
    unsigned char reserved;
};
static_assert(sizeof(LiveDc) == 12, "x86 live userdata ABI");

bool LiveRead(unsigned long addr, void* out, unsigned n) {
    __try {
        std::memcpy(out, (const void*)(ULONG_PTR)addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LiveWrite(unsigned long addr, const void* in, unsigned n) {
    __try {
        std::memcpy((void*)(ULONG_PTR)addr, in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Stock reflection only. Evidence: D:\Games-cache\royale\network-blueprint-mapping.txt
// and private clone research. No VeniceEXT appended-object layout is used here.
struct DcNativeApi {
    unsigned long create, copy, valueCopy, setGuid, release, addPartition, types, tlsIndex;
    unsigned long havokIndexOffset, codeEnd;
    unsigned char heads[6][16];
};
const DcNativeApi kDcServer = {
    0x004D3EB0,0x004D6010,0x004737F0,0x004C9C60,0x00468830,0x00580970,0x02886EB0,0x02B6AA74,
    0x66C,0x02428000,
    {{0x83,0xEC,0x10,0x56,0x57,0x8B,0xF9,0x8B,0x07,0x83,0x78,0x14,0x00,0x75,0x21,0x68},
     {0x53,0x8B,0x5C,0x24,0x10,0x55,0x56,0x57,0x8B,0xE9,0x85,0xDB,0x74,0x35,0xF6,0x03},
     {0x56,0x8B,0xF1,0x8B,0x06,0x0F,0xB7,0x40,0x04,0x8B,0xC8,0xC1,0xE9,0x04,0x83,0xE1},
     {0x8B,0x44,0x24,0x04,0xB9,0x00,0x01,0x00,0x00,0x56,0x66,0x85,0x48,0x06,0x74,0x42},
     {0x56,0x8B,0xF1,0x8D,0x4E,0x04,0x83,0xC8,0xFF,0xF0,0x0F,0xC1,0x01,0x48,0x25,0xFF},
     {0x83,0xEC,0x10,0x83,0x7C,0x24,0x1C,0x01,0x53,0x56,0x8B,0x74,0x24,0x1C,0x8B,0xD9}}
};
const DcNativeApi kDcClient = {
    0x0048A060,0x0048B2E0,0x00452440,0x00484F80,0x00448320,0x004EC2C0,0x022EA470,0x023979E0,
    0x230,0x0205E000,
    {{0x8B,0x54,0x24,0x10,0x83,0xEC,0x10,0x85,0xD2,0x75,0x05,0xBA,0x30,0x96,0x2E,0x02},
     {0x53,0x8B,0x5C,0x24,0x10,0x55,0x56,0x57,0x8B,0xE9,0x85,0xDB,0x74,0x35,0xF6,0x03},
     {0x8B,0x01,0x0F,0xB7,0x40,0x04,0xC1,0xE8,0x04,0x83,0xE0,0x1F,0x83,0xC0,0xFE,0x83},
     {0x8B,0x44,0x24,0x04,0xB9,0x00,0x01,0x00,0x00,0x66,0x85,0x48,0x06,0x74,0x05,0x83},
     {0x56,0x8B,0xF1,0x8D,0x4E,0x04,0x83,0xC8,0xFF,0xF0,0x0F,0xC1,0x01,0x48,0x25,0xFF},
     {0x83,0xEC,0x10,0x83,0x7C,0x24,0x1C,0x01,0x56,0x8B,0x74,0x24,0x18,0x57,0x8B,0xF9}}
};
const DcNativeApi* g_dcApi = nullptr;
volatile LONG g_dcNativeState = 0;
unsigned long DcVa(unsigned long va) {
    return (unsigned long)(ULONG_PTR)::GetModuleHandleW(nullptr) + va - 0x00400000UL;
}
bool DcGameProcess() {
    const HMODULE main = ::GetModuleHandleW(nullptr);
    return (::GetModuleHandleW(L"bf3_server_Final.exe") == main ||
            ::GetModuleHandleW(L"bf3.exe") == main || ::GetModuleHandleW(L"zbf3.exe") == main);
}
bool DcNativeReady() {
    if (!DcGameProcess() || !eng::Ready()) return false;
    if (g_dcNativeState != 0) return g_dcNativeState == 1;
    char flag[8] = {};
    if (::GetEnvironmentVariableA("EVBOTS_DATACONTAINER_MUTATION", flag, sizeof(flag)) && flag[0] == '0') return false;
    const DcNativeApi* api = ::GetModuleHandleW(L"bf3_server_Final.exe") == ::GetModuleHandleW(nullptr)
        ? &kDcServer : &kDcClient;
    const unsigned long addresses[] = {api->create,api->copy,api->valueCopy,api->setGuid,api->release,api->addPartition};
    bool match = true;
    unsigned char bytes[16];
    for (unsigned i=0; i<6; ++i) {
        const bool ok = LiveRead(DcVa(addresses[i]),bytes,sizeof(bytes)) && std::memcmp(bytes,api->heads[i],sizeof(bytes)) == 0;
        LOG_INFO("DataContainer: native 0x%08lX, 16 bytes: %s",addresses[i],ok?"PASS":"FAIL");
        match = match && ok;
    }
    g_dcApi = api;
    InterlockedExchange(&g_dcNativeState,match?1:-1);
    return match;
}
// Only native resource callbacks and the actual engine frame can enter mutable
// reflection. Static EXE TLS also exists on unrelated Lua/OS threads: it alone
// is not evidence that a caller owns the engine's resource work.
thread_local unsigned g_dcCallbackDepth=0;
class DcCallbackScope {
public:
    DcCallbackScope() { ++g_dcCallbackDepth; }
    ~DcCallbackScope() { --g_dcCallbackDepth; }
    DcCallbackScope(const DcCallbackScope&)=delete;
    DcCallbackScope& operator=(const DcCallbackScope&)=delete;
};
bool DcAllocatorReady();
volatile LONG g_dcThreadLogged=0;
bool DcNativeThread() {
    if (!DcNativeReady()) return false;
    const bool nativeScope=g_dcCallbackDepth!=0 || blueprint::EngineFrameActive();
    unsigned long index=0,tls=0,probe=0,havokIndex=0;
    const unsigned long vector=__readfsdword(0x2C);
    const bool tlsReady=vector && LiveRead(DcVa(g_dcApi->tlsIndex),&index,4) && index<4096 &&
        LiveRead(vector+index*4,&tls,4) && tls && LiveRead(tls,&probe,4);
    if (tlsReady) LiveRead(tls+g_dcApi->havokIndexOffset,&havokIndex,4);
    // +66C (server)/+230 (client) is Havok's thread-info index, NOT a
    // MemoryArena pointer. Zero is valid during early partition post-load.
    // Stock DC factories use the default arena; Array<T> uses fromBlock(this).
    const bool allocatorReady=nativeScope && tlsReady && DcAllocatorReady();
    const bool ok=nativeScope && tlsReady && allocatorReady;
    const LONG bit=!nativeScope?1:(!tlsReady?2:(!allocatorReady?4:8));
    if ((InterlockedOr(&g_dcThreadLogged,bit)&bit)==0) {
        LOG_INFO("DataContainer: native access %s: thread=%lu scope=%u TLS-index=%lu TLS=%08lX Havok-index=%lu arena-ready=%u",
            ok?"PASS":"REFUSED",::GetCurrentThreadId(),nativeScope?1u:0u,index,tls,havokIndex,allocatorReady?1u:0u);
    }
    return ok;
}
bool DcCode(unsigned long p) {
    return g_dcApi && p>=DcVa(0x00401000) && p<DcVa(g_dcApi->codeEnd);
}
void DcNativeFault() {
    InterlockedExchange(&g_dcNativeState,-1);
    LOG_ERR("DataContainer: native fault; mutations disabled, partial engine changes cannot be rolled back");
}
bool DcRetain(unsigned long p) {
    if (!p || (p&3)) return false;
    __try {
        auto* refs=(volatile LONG*)(ULONG_PTR)(p+4);
        LONG value=::InterlockedCompareExchange(refs,0,0);
        for (;;) {
            // Native low 16 bits are the count; high 16 bits contain flags.
            // CAS keeps concurrent retains from carrying into those flags.
            if (((unsigned long)value&0x08000000UL) || ((unsigned long)value&0xFFFF)==0xFFFF) return false;
            const LONG found=::InterlockedCompareExchange(refs,value+1,value);
            if (found==value) return true;
            value=found;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool DcRelease(unsigned long p) {
    __try {
        ((void(__thiscall*)(void*))(ULONG_PTR)DcVa(g_dcApi->release))((void*)(ULONG_PTR)p);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { DcNativeFault(); return false; }
}
SRWLOCK g_dcReleaseLock = SRWLOCK_INIT;
std::vector<unsigned long> g_dcReleases;
struct DcValueRelease { unsigned long header, arrayType; };
std::vector<DcValueRelease> g_dcValueReleases;
bool DcDestroyValue(unsigned long header,unsigned long arrayType);
void DcDrainReleases() {
    if (!DcNativeThread()) return;
    std::vector<unsigned long> pending;
    std::vector<DcValueRelease> values;
    AcquireSRWLockExclusive(&g_dcReleaseLock);
    pending.swap(g_dcReleases);
    values.swap(g_dcValueReleases);
    ReleaseSRWLockExclusive(&g_dcReleaseLock);
    for (unsigned long p:pending) if (g_dcNativeState==1) DcRelease(p);
    for (const DcValueRelease& v:values) if (g_dcNativeState==1) DcDestroyValue(v.header,v.arrayType);
}
unsigned long DcFindType(const char* name) {
    unsigned long p=0;
    if (!DcNativeReady() || !LiveRead(DcVa(g_dcApi->types),&p,4)) return 0;
    for (unsigned i=0;p && i<65536;++i) {
        eng::TypeDesc d;
        if (eng::Describe(p,&d) && std::strcmp(d.name,name)==0) return p;
        unsigned long next=0;
        if (!LiveRead(p+4,&next,4) || next==p) return 0;
        p=next;
    }
    return 0;
}
int DcCopyFault(EXCEPTION_POINTERS* fault,unsigned long type,unsigned long dst,unsigned long src,bool reference) {
    const EXCEPTION_RECORD* r=fault?fault->ExceptionRecord:nullptr;
    const CONTEXT* context=fault?fault->ContextRecord:nullptr;
    unsigned long d=0,s=0,ds=0,ss=0;
    LiveRead(dst,&d,4);LiveRead(src,&s,4);
    if (d) LiveRead(d+4,&ds,4);
    if (s) LiveRead(s+4,&ss,4);
    LOG_ERR("DataContainer: copy fault code=%08lX EIP=%08lX access=%lu address=%08lX type=%08lX dst=%08lX src=%08lX *dst=%08lX(state %08lX) *src=%08lX(state %08lX) ref=%d",
        r?r->ExceptionCode:0UL,context?context->Eip:0UL,
        r&&r->NumberParameters>=2?static_cast<unsigned long>(r->ExceptionInformation[0]):0UL,
        r&&r->NumberParameters>=2?static_cast<unsigned long>(r->ExceptionInformation[1]):0UL,
        type,dst,src,d,ds,s,ss,reference?1:0);
    if (context) {
        unsigned long vtable=0,destructor=0,stack[16]={};
        LiveRead(d,&vtable,4);LiveRead(vtable+4,&destructor,4);
        LiveRead(context->Esp,stack,sizeof(stack));
        LOG_ERR("DataContainer: copy context EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX ESP=%08lX EBP=%08lX dstVtable=%08lX dstDtor=%08lX",
            context->Eax,context->Ebx,context->Ecx,context->Edx,context->Esi,context->Edi,context->Esp,context->Ebp,vtable,destructor);
        for (unsigned i=0;i<16;i+=4)
            LOG_ERR("DataContainer: copy stack +%02X %08lX %08lX %08lX %08lX",i*4,stack[i],stack[i+1],stack[i+2],stack[i+3]);
        const bool server=g_dcApi==&kDcServer;
        const unsigned long pointer=context->Edx;
        const unsigned long page=DcVa(server?0x02888158:0x0230D938)+(pointer>>16)*4;
        unsigned long entry=0,arena=0;
        const bool mapped=LiveRead(page,&entry,4);
        if (mapped) {
            if (!entry || entry==0xFFFFFFFFUL) arena=DcVa(server?0x02880980:0x022E9630);
            else if (entry!=0xFFFFFFFEUL) arena=entry;
        }
        LOG_ERR("DataContainer: EDX allocator probe pointer=%08lX page=%08lX readable=%d entry=%08lX arena=%08lX",
            pointer,page,mapped?1:0,entry,arena);
        for (unsigned depth=0;depth<16 && arena;++depth) {
            unsigned long flags=0,next=0;
            const bool readable=LiveRead(arena+4,&flags,4) && (!server || LiveRead(arena+8,&next,4));
            LOG_ERR("DataContainer: EDX allocator node depth=%u arena=%08lX readable=%d flags=%08lX next=%08lX",
                depth,arena,readable?1:0,flags,next);
            if (!readable || !server || !next) break;
            if (next==arena || depth==15) {
                LOG_ERR("DataContainer: EDX allocator probe incomplete: cycle or depth limit");
                break;
            }
            arena=next;
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
bool DcCopyNative(unsigned long type,unsigned long dst,unsigned long src,bool reference) {
    // StructInfo tags its local arg3; native ArrayInfo passes NULL for value copies (004CFAB4).
    const unsigned long referenceFlags=1;
    __try {
        ((void(__thiscall*)(void*,void*,const void*,const void*))(ULONG_PTR)DcVa(g_dcApi->valueCopy))
            ((void*)(ULONG_PTR)type,(void*)(ULONG_PTR)dst,(const void*)(ULONG_PTR)src,reference?&referenceFlags:nullptr);
        return true;
    } __except(DcCopyFault(GetExceptionInformation(),type,dst,src,reference)) { DcNativeFault(); return false; }
}
unsigned long DcCreateNative(unsigned long type,unsigned long source,const eng::Guid16* guid) {
    unsigned long result=0;
    __try {
        // ClassInfo::createInstance(skipRandomGuid=true,withGuid,memory=null,arena=default).
        // DataContainer::clone itself always allocates GUID storage: VU Clone() does not.
        result=((unsigned long(__thiscall*)(void*,int,int,void*,void*))(ULONG_PTR)DcVa(g_dcApi->create))
            ((void*)(ULONG_PTR)type,1,guid?1:0,nullptr,nullptr);
        if (!result) return 0;
        if (guid) ((void(__stdcall*)(void*,const eng::Guid16*))(ULONG_PTR)DcVa(g_dcApi->setGuid))
            ((void*)(ULONG_PTR)result,guid);
        if (source) ((void(__thiscall*)(void*,void*,const void*,void*))(ULONG_PTR)DcVa(g_dcApi->copy))
            ((void*)(ULONG_PTR)type,(void*)(ULONG_PTR)result,(void*)(ULONG_PTR)source,nullptr);
    } __except(EXCEPTION_EXECUTE_HANDLER) { DcNativeFault(); return 0; }
    return result;
}
bool DcFactoryAvailable(unsigned long type) {
    eng::TypeDesc d;
    unsigned long factory=0;
    return eng::Describe(type,&d) && d.kind==eng::kClass && d.size>=8 && d.size<=65535 &&
        LiveRead(d.info+0x14,&factory,4) && DcCode(factory);
}
bool DcReadGuid(lua_State* L,int idx,eng::Guid16* guid) {
    if (lua_isnoneornil(L,idx)) return false;
    luaL_tolstring(L,idx,nullptr);
    const bool ok=eng::ParseGuid(lua_tostring(L,-1),guid,nullptr);
    lua_pop(L,1);
    if (!ok) luaL_error(L,"expected a Guid");
    return true;
}
void DcKeepParent(lua_State* L,int parent) {
    if (luaL_testudata(L,-1,kLiveMeta) || luaL_testudata(L,-1,kLiveArrMeta)) {
        lua_pushvalue(L,parent);
        lua_setiuservalue(L,-2,1);
    }
}

/// Строка по char* из памяти движка (до 512 байт, под SEH).
bool LiveCString(unsigned long p, char* out, unsigned cap) {
    if (p == 0) return false;
    __try {
        unsigned i = 0;
        for (; i + 1 < cap; ++i) {
            const char c = *(const char*)(ULONG_PTR)(p + i);
            out[i] = c;
            if (c == '\0') break;
        }
        out[cap - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LiveDc* PushLive(lua_State* L, unsigned long ptr, unsigned long type, unsigned char shape, bool writable) {
    LiveDc* d = static_cast<LiveDc*>(lua_newuserdatauv(L, sizeof(LiveDc), 1));
    d->ptr = ptr;
    d->type = type;
    d->shape = shape;
    d->writable = writable ? 1 : 0;
    d->owned = 0;
    d->reserved = 0;
    luaL_setmetatable(L, shape == kShapeArray ? kLiveArrMeta : kLiveMeta);
    return d;
}

/// Инстанс движка → живой DataContainer (nil, если тип не читается).
void PushLiveInstance(lua_State* L, unsigned long obj) {
    const unsigned long t = obj != 0 ? eng::TypeOf(obj) : 0;
    eng::TypeDesc d;
    if (t == 0 || !eng::Describe(t, &d)) {
        lua_pushnil(L);
        return;
    }
    PushLive(L, obj, t, kShapeInstance, false);
    RegisterCastGlobal(L, d.name);  // _G[typeInfo.name](obj) — каст мода
}

/// Guid из 16 байт: объект Guid(...) нашего Lua (VextGeometry), иначе строка.
void PushGuidValue(lua_State* L, const eng::Guid16& g) {
    char text[40];
    eng::FormatGuid(g, text);
    lua_getglobal(L, "Guid");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, text);
        return;
    }
    lua_pushstring(L, text);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        lua_pushstring(L, text);
    }
}

/// Значение по адресу и типу поля → Lua.
void PushLiveValue(lua_State* L, unsigned long addr, unsigned long ftype, bool writable) {
    eng::TypeDesc d;
    if (ftype == 0 || !eng::Describe(ftype, &d)) {
        lua_pushnil(L);
        return;
    }
    unsigned char raw[16] = {};
    switch (d.kind) {
        case eng::kBoolean:
            if (!LiveRead(addr, raw, 1)) break;
            lua_pushboolean(L, raw[0] != 0);
            return;
        case eng::kInt8:
            if (!LiveRead(addr, raw, 1)) break;
            lua_pushinteger(L, (signed char)raw[0]);
            return;
        case eng::kUint8:
            if (!LiveRead(addr, raw, 1)) break;
            lua_pushinteger(L, raw[0]);
            return;
        case eng::kInt16: {
            short v = 0;
            if (!LiveRead(addr, &v, 2)) break;
            lua_pushinteger(L, v);
            return;
        }
        case eng::kUint16: {
            unsigned short v = 0;
            if (!LiveRead(addr, &v, 2)) break;
            lua_pushinteger(L, v);
            return;
        }
        case eng::kInt32:
        case eng::kEnum: {
            int v = 0;
            const unsigned n = (d.kind == eng::kEnum && d.size >= 1 && d.size < 4) ? d.size : 4;
            if (!LiveRead(addr, &v, n)) break;
            lua_pushinteger(L, v);
            return;
        }
        case eng::kUint32: {
            unsigned v = 0;
            if (!LiveRead(addr, &v, 4)) break;
            lua_pushinteger(L, (lua_Integer)v);
            return;
        }
        case eng::kInt64:
        case eng::kUint64: {
            long long v = 0;
            if (!LiveRead(addr, &v, 8)) break;
            lua_pushinteger(L, (lua_Integer)v);
            return;
        }
        case eng::kFloat32: {
            float v = 0;
            if (!LiveRead(addr, &v, 4)) break;
            lua_pushnumber(L, v);
            return;
        }
        case eng::kFloat64: {
            double v = 0;
            if (!LiveRead(addr, &v, 8)) break;
            lua_pushnumber(L, v);
            return;
        }
        case eng::kCString:
        case eng::kFileRef: {
            unsigned long p = 0;
            char buf[512];
            if (!LiveRead(addr, &p, 4)) break;
            if (p == 0) {
                lua_pushstring(L, "");
                return;
            }
            if (!LiveCString(p, buf, sizeof(buf))) break;
            lua_pushstring(L, buf);
            return;
        }
        case eng::kGuid: {
            eng::Guid16 g;
            if (!LiveRead(addr, g.b, 16)) break;
            PushGuidValue(L, g);
            return;
        }
        case eng::kClass: {
            unsigned long p = 0;
            if (!eng::ReadReferenceTarget(addr, &p)) break;
            if (p == 0) {
                lua_pushnil(L);
                return;
            }
            PushLiveInstance(L, p);
            return;
        }
        case eng::kValueType: {
            // Vec2/Vec3/Vec4 — значениями нашего Lua (как VU), остальное — вложенный объект
            const bool v2 = std::strcmp(d.name, "Vec2") == 0;
            const bool v3 = std::strcmp(d.name, "Vec3") == 0;
            const bool v4 = std::strcmp(d.name, "Vec4") == 0;
            if (v2 || v3 || v4) {
                float f[4] = {};
                const int n = v2 ? 2 : (v3 ? 3 : 4);
                if (!LiveRead(addr, f, (unsigned)(n * 4))) break;
                lua_getglobal(L, d.name);
                if (!lua_isnil(L, -1)) {
                    for (int i = 0; i < n; ++i) lua_pushnumber(L, f[i]);
                    if (lua_pcall(L, n, 1, 0) == LUA_OK) return;
                }
                lua_pop(L, 1);
            }
            PushLive(L, addr, ftype, kShapeStruct, writable);
            RegisterCastGlobal(L, d.name);
            return;
        }
        case eng::kArray:
            PushLive(L, addr, ftype, kShapeArray, writable);
            return;
        default: {
            static volatile LONG s_warned = 0;
            if (InterlockedIncrement(&s_warned) <= 8) {
                LOG_WARN("EVBots: живой DataContainer: вид поля %d (%s) не читается — nil", d.kind, d.name);
            }
            break;
        }
    }
    lua_pushnil(L);
}

const char* WriteLiveValue(lua_State* L,int idx,unsigned long addr,unsigned long ftype,
                           const LiveDc* owner=nullptr);

// The native LinearTransform consists of four aligned Vec3 rows. Parse a
// complete Lua value before writing so invalid geometry cannot partially
// update a camera transform or force an array to grow.
bool ReadLinearTransformTable(lua_State* L, int index, const eng::TypeDesc& type, float (&matrix)[16]) {
    if (!lua_istable(L, index) || type.size != sizeof(matrix)) return false;
    index = lua_absindex(L, index);
    static constexpr const char* rows[] = {"right", "up", "forward", "trans"};
    static constexpr const char* cols[] = {"x", "y", "z"};
    for (unsigned row = 0; row < 4; ++row) {
        eng::FieldDesc field;
        eng::TypeDesc vector;
        if (!eng::FindField(type.typeInfo, rows[row], &field) || field.offset != row * 16 ||
            !eng::Describe(field.type, &vector) || std::strcmp(vector.name, "Vec3") != 0) return false;
        lua_getfield(L, index, row == 0 ? "left" : rows[row]);
        if (row == 0 && lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, index, rows[row]);
        }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
        for (unsigned col = 0; col < 3; ++col) {
            lua_getfield(L, -1, cols[col]);
            int numeric = 0;
            const double component = lua_tonumberx(L, -1, &numeric);
            lua_pop(L, 1);
            if (!numeric || !std::isfinite(component) || std::abs(component) > (std::numeric_limits<float>::max)()) {
                lua_pop(L, 1);
                return false;
            }
            matrix[row * 4 + col] = static_cast<float>(component);
        }
        matrix[row * 4 + 3] = 0;
        lua_pop(L, 1);
    }
    return true;
}

const char* LiveFieldKey(const char* typeName,const char* key) {
    return typeName && std::strcmp(typeName,"LinearTransform")==0 && std::strcmp(key,"left")==0 ? "right" : key;
}

bool ResolveNativeField(unsigned long type,const char* typeName,const char* key,eng::FieldDesc* field) {
    // VU's generated Lua API appends Value to reserved Lua identifiers. Keep
    // genuine native *Value fields authoritative; reflection still exposes the
    // original names (VEManager explicitly converts End to endValue itself).
    if (eng::FindField(type,LiveFieldKey(typeName,key),field)) return true;
    static const struct { const char* luaName; const char* nativeName; } aliases[] = {
        {"endValue","End"}, {"inValue","In"}, {"trueValue","True"},
        {"falseValue","False"}, {"repeatValue","Repeat"}
    };
    for (const auto& alias:aliases) {
        if (std::strcmp(key,alias.luaName)==0) return eng::FindField(type,alias.nativeName,field);
    }
    return false;
}

/// Lua-таблица (LinearTransform/Vec3 из VextGeometry) → поля нативной структуры.
const char* WriteLiveStruct(lua_State* L,int idx,unsigned long addr,unsigned long type,unsigned depth) {
    idx=lua_absindex(L,idx);
    if (depth>16) return "слишком глубокая структура";
    eng::TypeDesc d;
    if (!eng::Describe(type,&d) || d.kind!=eng::kValueType) return "ожидалась структура движка";
    if (!d.size || d.size>65535 || (d.fieldCount && !d.fields)) return "раскладка структуры не читается";
    if (d.super && d.super!=type) {
        const char* error=WriteLiveStruct(L,idx,addr,d.super,depth+1);
        if (error) return error;
    }
    const int top=lua_gettop(L);
    for (unsigned i=0;i<d.fieldCount;++i) {
        const unsigned long fa=d.fields+i*12;
        unsigned long namePtr=0,fieldType=0;unsigned short offset=0;char name[128];
        if (!LiveRead(fa,&namePtr,4) || !LiveRead(fa+6,&offset,2) || !LiveRead(fa+8,&fieldType,4))
            return "поле структуры не читается";
        if (!namePtr || !LiveCString(namePtr,name,sizeof(name))) return "имя поля структуры не читается";
        lua_getfield(L,idx,name);
        if (lua_isnil(L,-1)) { lua_pop(L,1); continue; }
        const char* error=WriteLiveValue(L,top+1,addr+offset,fieldType);
        lua_settop(L,top);
        if (error) return error;
    }
    return nullptr;
}

bool HasLiveGuid(unsigned long object, bool partition, const char* text) {
    eng::Guid16 expected{}, actual{};
    return eng::ParseGuid(text, &expected, nullptr) &&
        (partition ? eng::PartitionGuid(object, &actual) : eng::InstanceGuid(object, &actual)) &&
        std::memcmp(actual.b, expected.b, sizeof(actual.b)) == 0;
}

bool SwapReferenceIfUnchanged(unsigned long address, unsigned long expected, unsigned long replacement) {
    __try {
        return static_cast<unsigned long>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(static_cast<ULONG_PTR>(address)),
            static_cast<LONG>(replacement), static_cast<LONG>(expected))) == expected;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// CQ_Shared_Gameplay is packed in the dedicated SubLevel arena. On restart its
// primary WorldPart can reach the destructor, which frees child shape arrays as
// heap blocks. Two dumps show the allocator reading the last Points.z value as
// the header of the borrowed Normals buffer. Cooked import fixup 00586815 writes
// the resolved pointer without a retain; stock 004D6010 would still release it.
// Limit the first transition to the original Conquest reference and source
// partition. Later assignments already replace an owning native reference.
bool WriteRoyaleBorrowedWorldPart(const LiveDc* owner, unsigned long address, unsigned long type,
                                 const LiveDc* replacement, const char** error) {
    if (!owner || !replacement || g_dcApi != &kDcServer || !g_dcCallbackDepth) return false;
    char flag[8] = {};
    if (GetEnvironmentVariableA("EVBOTS_ROYALE_WORLD_PART_OWNERSHIP", flag, sizeof(flag)) == 1 && flag[0] == '0')
        return false;
    if (!HasLiveGuid(owner->ptr, false, "DA506D40-69C7-4670-BB8B-25EDC9F1A526")) return false;

    constexpr unsigned long kFrozen = 0x20000000UL;
    constexpr unsigned long kMaterialized = 0x00800000UL;
    constexpr unsigned long kDestroyed = 0x08000000UL;
    unsigned long ownerState = 0, raw = 0, previous = 0, before = 0;
    if (!LiveRead(owner->ptr + 4, &ownerState, 4) || !(ownerState & kFrozen) || (ownerState & kDestroyed) ||
        !LiveRead(address, &raw, 4) || !raw || !eng::ReadReferenceTarget(address, &previous) ||
        previous == replacement->ptr || !HasLiveGuid(previous, false, "A048FCDD-2F98-432A-A5B7-5CC49F2AB21E") ||
        !LiveRead(previous + 4, &before, 4) || !(before & kFrozen) || (before & (kDestroyed | kMaterialized))) return false;

    const unsigned long ownerPartition = eng::PartitionOfInstance(owner->ptr);
    const unsigned long worldPartition = eng::PartitionOfInstance(previous);
    if (!ownerPartition || !worldPartition || ownerPartition == worldPartition ||
        !HasLiveGuid(ownerPartition, true, "6C0D021C-80D8-4BDE-85F7-CDF6231F95D5") ||
        !HasLiveGuid(worldPartition, true, "B6BD6848-37DF-463A-81C5-33A5B3D6F623") ||
        eng::PrimaryInstance(worldPartition) != previous) return false;

    unsigned long newBefore = 0;
    if ((raw & 3) || raw != previous || !(before & 0xFFFF) || replacement->owned != 1 ||
        !eng::IsKindOfName(replacement->type, "WorldPartData") ||
        !LiveRead(replacement->ptr + 4, &newBefore, 4) || !(newBefore & 0xFFFF) ||
        (newBefore & (kFrozen | kDestroyed))) {
        *error = "Royale WorldPart replacement requires a direct cooked reference and owned native WorldPartData";
        return true;
    }

    unsigned long staged = 0;
    if (!DcCopyNative(type, static_cast<unsigned long>(reinterpret_cast<ULONG_PTR>(&staged)),
                      static_cast<unsigned long>(reinterpret_cast<ULONG_PTR>(&replacement->ptr)), true)) {
        *error = "native Royale WorldPart reference retain failed";
        return true;
    }
    if (staged != replacement->ptr || !SwapReferenceIfUnchanged(address, raw, staged)) {
        if (staged == replacement->ptr) DcRelease(staged);
        *error = "native Royale WorldPart reference changed during replacement";
        return true;
    }

    unsigned long installed = 0, after = 0, newAfter = 0;
    const bool readable = LiveRead(address, &installed, 4) && LiveRead(previous + 4, &after, 4) &&
        LiveRead(staged + 4, &newAfter, 4);
    LOG_INFO("DataContainer: Royale WorldPart ref owner=%08lX ownerState=%08lX slot=%08lX oldRaw=%08lX refs=%u->%u flags=%04X->%04X installed=%08lX newRefs=%u->%u newFlags=%04X->%04X readback=%s",
        owner->ptr, ownerState, address, raw, static_cast<unsigned>(before & 0xFFFF),
        static_cast<unsigned>(after & 0xFFFF), static_cast<unsigned>(before >> 16), static_cast<unsigned>(after >> 16),
        installed, static_cast<unsigned>(newBefore & 0xFFFF), static_cast<unsigned>(newAfter & 0xFFFF),
        static_cast<unsigned>(newBefore >> 16), static_cast<unsigned>(newAfter >> 16),
        readable && installed == staged ? "PASS" : "FAIL");
    return true;
}

/// Lua-значение → поле движка. Возвращает текст ошибки или nullptr.
const char* WriteLiveValue(lua_State* L, int idx, unsigned long addr, unsigned long ftype, const LiveDc* owner) {
    eng::TypeDesc d;
    if (ftype == 0 || !eng::Describe(ftype, &d)) return "тип поля не читается";
    switch (d.kind) {
        case eng::kBoolean: {
            const unsigned char v = lua_toboolean(L, idx) ? 1 : 0;
            return LiveWrite(addr, &v, 1) ? nullptr : "память не пишется";
        }
        case eng::kInt8:
        case eng::kUint8: {
            const unsigned char v = (unsigned char)luaL_checkinteger(L, idx);
            return LiveWrite(addr, &v, 1) ? nullptr : "память не пишется";
        }
        case eng::kInt16:
        case eng::kUint16: {
            const unsigned short v = (unsigned short)luaL_checkinteger(L, idx);
            return LiveWrite(addr, &v, 2) ? nullptr : "память не пишется";
        }
        case eng::kInt32:
        case eng::kUint32:
        case eng::kEnum: {
            const int v = (int)luaL_checkinteger(L, idx);
            const unsigned n = (d.kind == eng::kEnum && d.size >= 1 && d.size < 4) ? d.size : 4;
            return LiveWrite(addr, &v, n) ? nullptr : "память не пишется";
        }
        case eng::kInt64:
        case eng::kUint64: {
            const long long v = (long long)luaL_checkinteger(L, idx);
            return LiveWrite(addr, &v, 8) ? nullptr : "память не пишется";
        }
        case eng::kFloat32: {
            const float v = (float)luaL_checknumber(L, idx);
            return LiveWrite(addr, &v, 4) ? nullptr : "память не пишется";
        }
        case eng::kFloat64: {
            const double v = (double)luaL_checknumber(L, idx);
            return LiveWrite(addr, &v, 8) ? nullptr : "память не пишется";
        }
        case eng::kClass: {
            const LiveDc* source = nullptr;
            unsigned long p = 0;
            if (!lua_isnil(L, idx)) {
                source = static_cast<const LiveDc*>(luaL_testudata(L, idx, kLiveMeta));
                if (source == nullptr || source->shape != kShapeInstance) return "ожидался живой DataContainer или nil";
                if (!eng::IsKindOfName(source->type,d.name)) return "несовместимый тип ссылки DataContainer";
                p = source->ptr;
            }
            if (!DcNativeThread()) return "native reference assignment requires initialized engine TLS";
            const char* error = nullptr;
            if (WriteRoyaleBorrowedWorldPart(owner, addr, ftype, source, &error)) return error;
            // TypeInfo::copy(reference=true) retains new and releases old, including self-assignment.
            return DcCopyNative(ftype,addr,(unsigned long)(ULONG_PTR)&p,true) ? nullptr : "native reference assignment failed";
        }
        case eng::kCString: {
            size_t len=0;
            const char* text=luaL_checklstring(L,idx,&len);
            if (len>1024*1024 || std::memchr(text,0,len)) return "CString contains NUL or exceeds 1 MiB";
            if (!DcNativeThread()) return "native string assignment requires initialized engine TLS";
            // CString TypeInfo::copy -> CString::set; the native string owns its copy.
            return DcCopyNative(ftype,addr,(unsigned long)(ULONG_PTR)&text,false) ? nullptr : "native string assignment failed";
        }
        case eng::kValueType: {
            const LiveDc* value=static_cast<const LiveDc*>(luaL_testudata(L,idx,kLiveMeta));
            if (value && value->shape==kShapeStruct && value->type==ftype) {
                if (!DcNativeThread()) return "native structure assignment requires initialized engine TLS";
                return DcCopyNative(ftype,addr,value->ptr,false) ? nullptr : "native structure assignment failed";
            }
            if (std::strcmp(d.name, "LinearTransform") == 0) {
                float matrix[16];
                if (!ReadLinearTransformTable(L, idx, d, matrix)) return "expected a complete finite LinearTransform";
                return LiveWrite(addr, matrix, sizeof(matrix)) ? nullptr : "native LinearTransform write failed";
            }
            const bool v2 = std::strcmp(d.name, "Vec2") == 0;
            const bool v3 = std::strcmp(d.name, "Vec3") == 0;
            const bool v4 = std::strcmp(d.name, "Vec4") == 0;
            if (!(v2 || v3 || v4)) {
                if (lua_istable(L,idx)) return WriteLiveStruct(L,idx,addr,ftype,0);
                return "присваивание структуры целиком не поддерживается — пишите по полям";
            }
            static const char* kComp[4] = {"x", "y", "z", "w"};
            const int n = v2 ? 2 : (v3 ? 3 : 4);
            float f[4] = {};
            for (int i = 0; i < n; ++i) {
                lua_getfield(L, idx, kComp[i]);
                f[i] = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            return LiveWrite(addr, f, (unsigned)(n * 4)) ? nullptr : "память не пишется";
        }
        default:
            return "запись этого вида поля не поддерживается";
    }
}

LiveDc* CheckLive(lua_State* L, int idx) {
    return static_cast<LiveDc*>(luaL_checkudata(L, idx, kLiveMeta));
}

const char* DcMakeWritable(LiveDc* d);
int Live_MakeWritable(lua_State* L) {
    LiveDc* d=CheckLive(L,1);
    const char* error=DcMakeWritable(d);
    if (error) return luaL_error(L,"MakeWritable: %s",error);
    d->writable = 1;
    return 0;
}

int Live_Is(lua_State* L) {
    const LiveDc* d = CheckLive(L, 1);
    lua_pushboolean(L, eng::IsKindOfName(d->type, luaL_checkstring(L, 2)));
    return 1;
}

int Live_Eq(lua_State* L) {
    const LiveDc* a = CheckLive(L, 1);
    const LiveDc* b = static_cast<const LiveDc*>(luaL_testudata(L, 2, kLiveMeta));
    lua_pushboolean(L, b != nullptr && a->ptr == b->ptr);
    return 1;
}

int Live_Self(lua_State* L) {
    lua_pushvalue(L, 1);
    return 1;
}

int DcPushValue(lua_State* L,unsigned long type,unsigned long source);
int Live_Clone(lua_State* L) {
    const LiveDc* source=CheckLive(L,1);
    eng::Guid16 guid;
    const bool withGuid=DcReadGuid(L,2,&guid);
    if (!DcNativeThread()) return luaL_error(L,"Clone requires verified native reflection and initialized engine TLS");
    if (source->shape==kShapeStruct) {
        if (withGuid) return luaL_error(L,"value-type Clone does not accept a Guid");
        return DcPushValue(L,source->type,source->ptr);
    }
    if (source->shape!=kShapeInstance) return luaL_error(L,"Clone requires a native class or value instance");
    if (!DcFactoryAvailable(source->type) || eng::TypeOf(source->ptr)!=source->type)
        return luaL_error(L,"Clone source/type/factory is unavailable");
    unsigned short flags=0;
    if (!LiveRead(source->ptr+6,&flags,2) || (flags&0x800)) return luaL_error(L,"cannot clone a destroyed instance");
    const unsigned long p=DcCreateNative(source->type,source->ptr,withGuid?&guid:nullptr);
    if (!p || !DcRetain(p)) return luaL_error(L,"native Clone failed");
    if (eng::IsKindOfName(source->type,"EffectBlueprint")) {
        eng::FieldDesc field;
        if (eng::FindField(source->type,"Object",&field)) {
            unsigned long original=0,copy=0;
            LiveRead(source->ptr+field.offset,&original,4);
            LiveRead(p+field.offset,&copy,4);
            LOG_INFO("DataContainer: effect clone source=%08lX copy=%08lX originalObject=%08lX copyObject=%08lX",
                source->ptr,p,original,copy);
        }
    }
    LiveDc* result=PushLive(L,p,source->type,kShapeInstance,true);
    result->owned=1;
    return 1;
}

int Live_Gc(lua_State* L) {
    LiveDc* d=static_cast<LiveDc*>(luaL_testudata(L,1,kLiveMeta));
    if (!d) d=static_cast<LiveDc*>(luaL_testudata(L,1,kLiveArrMeta));
    if (d && d->owned && d->ptr) {
        const unsigned long p=d->ptr;
        const unsigned char owned=d->owned;
        d->owned=0;
        // lua_close / worker Lua GC must never invoke an engine allocator/destructor.
        AcquireSRWLockExclusive(&g_dcReleaseLock);
        if (owned==2) g_dcValueReleases.push_back({p,d->type});
        else g_dcReleases.push_back(p);
        ReleaseSRWLockExclusive(&g_dcReleaseLock);
    }
    return 0;
}

int Lv_CreateType(lua_State* L) {
    const char* name=luaL_checkstring(L,1);
    if (!DcGameProcess()) { lua_pushnil(L); lua_pushboolean(L,0); return 2; }
    eng::Guid16 guid;
    const bool withGuid=DcReadGuid(L,2,&guid);
    if (!DcNativeThread()) return luaL_error(L,"%s() requires verified native reflection and initialized engine TLS",name);
    const unsigned long type=DcFindType(name);
    eng::TypeDesc desc;
    if (type && eng::Describe(type,&desc) && desc.kind==eng::kValueType) {
        if (withGuid) return luaL_error(L,"%s(): value types do not accept a Guid",name);
        DcPushValue(L,type,0);
        lua_pushboolean(L,1);
        return 2;
    }
    if (!type || !DcFactoryAvailable(type)) return luaL_error(L,"%s(): native class factory is unavailable",name);
    const unsigned long p=DcCreateNative(type,0,withGuid?&guid:nullptr);
    if (!p || !DcRetain(p)) return luaL_error(L,"%s(): native constructor failed",name);
    LiveDc* result=PushLive(L,p,type,kShapeInstance,true);
    result->owned=1;
    lua_pushboolean(L,1);
    return 2;
}

bool DcAddPartition(unsigned long partition,unsigned long instance) {
    __try {
        // VU AddInstance sets the registered-instance flag before InternalDatabasePartition::addPartitionInstance.
        *(unsigned short*)(ULONG_PTR)(instance+6)|=0x1000;
        ((void(__thiscall*)(void*,void*,int,int,const char*))(ULONG_PTR)DcVa(g_dcApi->addPartition))
            ((void*)(ULONG_PTR)partition,(void*)(ULONG_PTR)instance,1,0,nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { DcNativeFault(); return false; }
}
int Partition_Add(lua_State* L) {
    luaL_checktype(L,1,LUA_TTABLE);
    lua_getfield(L,1,"_ptr");
    const unsigned long partition=(unsigned long)luaL_checkinteger(L,-1);
    lua_pop(L,1);
    const LiveDc* instance=CheckLive(L,2);
    if (!DcNativeThread()) return luaL_error(L,"AddInstance requires verified native reflection and initialized engine TLS");
    if (instance->shape!=kShapeInstance || !instance->ptr || !partition)
        return luaL_error(L,"AddInstance requires a native partition and instance");
    eng::Guid16 guid,partitionGuid;
    if (!eng::PartitionGuid(partition,&partitionGuid) || !eng::InstanceGuid(instance->ptr,&guid))
        return luaL_error(L,"AddInstance requires a readable partition and an instance GUID");
    char text[40]; eng::FormatGuid(guid,text);
    const unsigned long existing=eng::FindInstanceInPartition(partition,text);
    if (existing==instance->ptr) return 0;
    if (existing) return luaL_error(L,"AddInstance: GUID already belongs to another instance");
    const unsigned long owner=eng::PartitionOfInstance(instance->ptr);
    if (owner && owner!=partition) return luaL_error(L,"AddInstance: instance belongs to another partition");
    if (!DcAddPartition(partition,instance->ptr)) return luaL_error(L,"native AddInstance failed");
    if (eng::FindInstanceInPartition(partition,text)!=instance->ptr)
        return luaL_error(L,"native AddInstance returned without registering the instance GUID");
    return 0;
}

int Lv_PartitionInstances(lua_State* L);
int Partition_Index(lua_State* L) {
    if (lua_type(L, 2) != LUA_TSTRING || std::strcmp(lua_tostring(L, 2), "instances") != 0) {
        lua_pushnil(L);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "_ptr");
    const lua_Integer partition = luaL_checkinteger(L, -1);
    lua_settop(L, 0);
    lua_pushinteger(L, partition);
    return Lv_PartitionInstances(L);
}

int Lv_InPartition(lua_State* L);
int Partition_FindInstance(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "_ptr");
    const unsigned long p = (unsigned long)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (p == 0) { lua_pushnil(L); return 1; }
    if (lua_type(L, 2) != LUA_TSTRING) {
        luaL_tolstring(L, 2, nullptr);
        lua_replace(L, 2);
    }
    const int top = lua_gettop(L);
    if (top > 2) lua_settop(L, 2);
    // Lv_InPartition expects (partitionPtr:number, instanceGuid:string)
    lua_pushinteger(L, (lua_Integer)p);
    lua_replace(L, 1);
    return Lv_InPartition(L);
}

/// DatabasePartition (shared\type\databasepartition.md): native instance list is
/// lazy for every access path, including DataContainer.partition and load handlers.
void PushPartitionTable(lua_State* L, unsigned long p) {
    if (p == 0) {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 4);
    eng::Guid16 g;
    if (eng::PartitionGuidCheck() > 0 && eng::PartitionGuid(p, &g)) {
        PushGuidValue(L, g);
        lua_setfield(L, -2, "guid");
    }
    PushLiveInstance(L, eng::PrimaryInstance(p));
    lua_setfield(L, -2, "primaryInstance");
    lua_pushinteger(L, (lua_Integer)p);
    lua_setfield(L, -2, "_ptr");
    lua_pushcfunction(L,Partition_Add);
    lua_setfield(L,-2,"AddInstance");
    lua_pushcfunction(L,Partition_FindInstance);
    lua_setfield(L,-2,"FindInstance");
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, Partition_Index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
}

bool PushGeometryMethod(lua_State* L, const char* typeName, const char* key) {
    if (std::strcmp(typeName, "LinearTransform") != 0 ||
        (std::strcmp(key, "LookAtTransform") != 0 && std::strcmp(key, "Translate") != 0 &&
         std::strcmp(key, "Inverse") != 0 && std::strcmp(key, "ToQuatTransform") != 0)) return false;
    // Методы работают с self.left/up/forward/trans. Native __index/__newindex
    // обеспечивают чтение и запись живой структуры, включая readonly/TLS.
    lua_getglobal(L, "LinearTransform");
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) { lua_pop(L, 1); return false; }
    lua_getfield(L, -1, key);
    lua_remove(L, -2);
    if (lua_isfunction(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

int Live_Index(lua_State* L) {
    LiveDc* d = CheckLive(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    const char* key = lua_tostring(L, 2);
    eng::TypeDesc td;
    const bool haveType = eng::Describe(d->type, &td);

    if (haveType && d->shape == kShapeStruct && PushGeometryMethod(L, td.name, key)) return 1;

    if (std::strcmp(key, "typeInfo") == 0) {
        PushNativeTypeInfo(L, d->type);
        return 1;
    }
    if (std::strcmp(key, "isReadOnly") == 0) {
        lua_pushboolean(L, d->writable == 0);
        return 1;
    }
    if (std::strcmp(key, "isLazyLoaded") == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (d->shape == kShapeInstance) {
        if (std::strcmp(key, "instanceGuid") == 0) {
            eng::Guid16 g;
            if (eng::InstanceGuid(d->ptr, &g)) PushGuidValue(L, g); else lua_pushnil(L);
            return 1;
        }
        if (std::strcmp(key, "partitionGuid") == 0) {
            eng::Guid16 g;
            const unsigned long p = eng::PartitionOfInstance(d->ptr);
            if (p != 0 && eng::PartitionGuidCheck() > 0 && eng::PartitionGuid(p, &g)) PushGuidValue(L, g);
            else lua_pushnil(L);
            return 1;
        }
        if (std::strcmp(key, "partition") == 0) {
            PushPartitionTable(L, eng::PartitionOfInstance(d->ptr));
            return 1;
        }
    }
    static const struct {
        const char* name;
        lua_CFunction fn;
    } kMethods[] = {
        {"MakeWritable", Live_MakeWritable},
        {"Is", Live_Is},
        {"Eq", Live_Eq},
        {"AsDC", Live_Self},
        {"Clone", Live_Clone},
        {"ReplaceReferences", Dc_Nop},
        {"RegisterLoadHandler", Dc_Nil},
        {"RegisterLoadHandlerOnce", Dc_Nil},
    };
    for (const auto& m : kMethods) {
        if (std::strcmp(key, m.name) == 0) {
            lua_pushcfunction(L, m.fn);
            return 1;
        }
    }
    eng::FieldDesc f;
    if (!haveType || !ResolveNativeField(d->type,td.name,key,&f)) {
        lua_pushnil(L);
        return 1;
    }
    if (std::strcmp(td.name,"EmitterEntityData")==0 && ::_stricmp(key,"emitter")==0) {
        static volatile LONG logged=0;
        if (InterlockedIncrement(&logged)<=8) {
            unsigned long raw=0,words[6]={};
            const bool readable=LiveRead(d->ptr+f.offset,&raw,4);
            if (raw) LiveRead(raw&~3UL,words,sizeof(words));
            LOG_INFO("DataContainer: emitter owner=%08lX offset=%u fieldType=%08lX readable=%u raw=%08lX words=%08lX,%08lX,%08lX,%08lX,%08lX,%08lX",
                d->ptr,f.offset,f.type,readable?1u:0u,raw,words[0],words[1],words[2],words[3],words[4],words[5]);
        }
    }
    PushLiveValue(L, d->ptr + f.offset, f.type, d->writable != 0);
    DcKeepParent(L,1);
    return 1;
}

int Live_NewIndex(lua_State* L) {
    LiveDc* d = CheckLive(L, 1);
    const char* key = luaL_checkstring(L, 2);
    eng::TypeDesc td;
    const bool haveType = eng::Describe(d->type, &td);
    if (d->writable == 0) {
        return luaL_error(L, "DataContainer '%s' read-only: нельзя присвоить '%s' (сначала MakeWritable)",
                          haveType ? td.name : "?", key);
    }
    eng::FieldDesc f;
    if (!haveType || !ResolveNativeField(d->type,td.name,key,&f)) {
        return luaL_error(L, "DataContainer '%s': нет поля '%s'", haveType ? td.name : "?", key);
    }
    const bool worldPartBlueprint = d->shape == kShapeInstance && !std::strcmp(td.name, "WorldPartReferenceObjectData") &&
        !::_stricmp(key, "Blueprint");
    const char* err = WriteLiveValue(L, 3, d->ptr + f.offset, f.type, worldPartBlueprint ? d : nullptr);
    if (err != nullptr) {
        return luaL_error(L, "DataContainer '%s'.%s: %s", td.name, key, err);
    }
    return 0;
}

int Live_ToString(lua_State* L) {
    const LiveDc* d = CheckLive(L, 1);
    eng::TypeDesc td;
    const char* nm = eng::Describe(d->type, &td) ? td.name : "?";
    lua_pushfstring(L, "DataContainer(%s @%p, движок)", nm, (void*)(ULONG_PTR)d->ptr);
    return 1;
}

int Live_Eqmm(lua_State* L) {
    const LiveDc* a = static_cast<const LiveDc*>(luaL_testudata(L, 1, kLiveMeta));
    const LiveDc* b = static_cast<const LiveDc*>(luaL_testudata(L, 2, kLiveMeta));
    lua_pushboolean(L, a != nullptr && b != nullptr && a->ptr == b->ptr && a->type == b->type);
    return 1;
}

// ── массив (Array<T> движка: указатель на первый элемент, число — в [first-4]) ──

struct ArrayView {
    unsigned long first = 0;
    unsigned long count = 0;
    unsigned long elemType = 0;
    unsigned stride = 0;
    int elemKind = 0;
};

bool ViewArray(const LiveDc* d, ArrayView* v) {
    eng::TypeDesc at, et;
    if (!eng::Describe(d->type, &at) || at.element == 0 || !eng::Describe(at.element, &et)) return false;
    unsigned long first = 0, count = 0;
    if (!LiveRead(d->ptr, &first, 4)) return false;
    if (first != 0 && !LiveRead(first - 4, &count, 4)) return false;
    if (count > 100000) return false;  // не массив: защита от мусора
    v->first = first;
    v->count = first != 0 ? count : 0;
    v->elemType = at.element;
    v->elemKind = et.kind;
    v->stride = et.kind == eng::kClass ? 4u : et.size;
    return v->stride != 0;
}

LiveDc* CheckArr(lua_State* L, int idx) {
    return static_cast<LiveDc*>(luaL_checkudata(L, idx, kLiveArrMeta));
}

int Arr_Get(lua_State* L) {
    const LiveDc* d = CheckArr(L, 1);
    const lua_Integer i = luaL_checkinteger(L, 2);
    ArrayView v;
    if (!ViewArray(d, &v) || i < 1 || (unsigned long)i > v.count) {
        lua_pushnil(L);
        return 1;
    }
    PushLiveValue(L, v.first + (unsigned long)(i - 1) * v.stride, v.elemType, d->writable != 0);
    DcKeepParent(L,1);
    return 1;
}

int Arr_Set(lua_State* L) {
    const LiveDc* d = CheckArr(L, 1);
    const lua_Integer i = luaL_checkinteger(L, 2);
    if (d->writable == 0) return luaL_error(L, "массив read-only (сначала MakeWritable у владельца)");
    ArrayView v;
    if (!ViewArray(d, &v) || i < 1 || (unsigned long)i > v.count) {
        return luaL_error(L, "индекс массива %d вне 1..%d", (int)i, (int)v.count);
    }
    const char* err = WriteLiveValue(L, 3, v.first + (unsigned long)(i - 1) * v.stride, v.elemType);
    if (err != nullptr) return luaL_error(L, "массив[%d]: %s", (int)i, err);
    return 0;
}

int Arr_Len(lua_State* L) {
    const LiveDc* d = CheckArr(L, 1);
    ArrayView v;
    lua_pushinteger(L, ViewArray(d, &v) ? (lua_Integer)v.count : 0);
    return 1;
}

int ArrResizeFault(EXCEPTION_POINTERS* fault,const LiveDc* array,unsigned long size,unsigned long resize) {
    const EXCEPTION_RECORD* record=fault?fault->ExceptionRecord:nullptr;
    const CONTEXT* context=fault?fault->ContextRecord:nullptr;
    LOG_ERR("DataContainer: array resize fault code=%08lX EIP=%08lX this=%08lX field=%08lX size=%lu function=%08lX access=%lu address=%08lX ECX=%08lX EDX=%08lX EAX=%08lX EBX=%08lX ESI=%08lX EDI=%08lX ESP=%08lX EBP=%08lX",
        record?record->ExceptionCode:0UL,context?context->Eip:0UL,array->type-4,array->ptr,size,resize,
        record && record->NumberParameters>=2?static_cast<unsigned long>(record->ExceptionInformation[0]):0UL,
        record && record->NumberParameters>=2?static_cast<unsigned long>(record->ExceptionInformation[1]):0UL,
        context?context->Ecx:0UL,context?context->Edx:0UL,
        context?context->Eax:0UL,context?context->Ebx:0UL,
        context?context->Esi:0UL,context?context->Edi:0UL,context?context->Esp:0UL,context?context->Ebp:0UL);
    return EXCEPTION_EXECUTE_HANDLER;
}
bool ArrResizeNative(const LiveDc* array,unsigned long size) {
    unsigned long table=0,resize=0;
    if (!array->type || !LiveRead(array->type-4,&table,4) || !LiveRead(table+4,&resize,4) || !DcCode(resize)) return false;
    static unsigned logged=0;
    if (logged<8) {
        ++logged;
        unsigned long first=0,count=0;
        LiveRead(array->ptr,&first,4);
        if (first>=4) LiveRead(first-4,&count,4);
        LOG_INFO("DataContainer: array resize this=%08lX field=%08lX first=%08lX count=%lu new=%lu vtable=%08lX function=%08lX",
            array->type-4,array->ptr,first,count,size,table,resize);
    }
    __try {
        // Stock ArrayTypeInfoBase::copy uses vtable[1](arrayAddress,newSize), ret8.
        ((void(__thiscall*)(void*,void*,unsigned long))(ULONG_PTR)resize)
            ((void*)(ULONG_PTR)(array->type-4),(void*)(ULONG_PTR)array->ptr,size);
        return true;
    } __except(ArrResizeFault(GetExceptionInformation(),array,size,resize)) { DcNativeFault(); return false; }
}
struct DcValueApi {
    unsigned long allocate,free,fromBlock,arena,empty,initialized;
    unsigned char heads[3][16];
};
const DcValueApi kDcValueServer={
    0x00430700,0x0042B190,0x004ADD50,0x02880980,0x02B17AE0,0x0287F321,
    {{0x8A,0x15,0x21,0xF3,0x87,0x02,0x53,0x55,0x8B,0xD9,0x8A,0x4C,0x24,0x14,0x57,0x90},
     {0x56,0x8B,0xF1,0x8B,0x46,0x08,0x85,0xC0,0x74,0x13,0x8A,0x4C,0x24,0x0C,0x8B,0xFF},
     {0x8B,0x44,0x24,0x04,0xC1,0xE8,0x10,0x8B,0x04,0x85,0x58,0x81,0x88,0x02,0x85,0xC0}}
};
const DcValueApi kDcValueClient={
    0x0041BE10,0x00418290,0x00503D40,0x022E9630,0x0230D264,0,
    {{0x57,0x8B,0x7C,0x24,0x0C,0x83,0xFF,0x04,0x77,0x05,0xBF,0x04,0x00,0x00,0x00,0xF6},
     {0x8B,0x51,0x04,0xF6,0xC2,0x02,0x75,0x45,0x8B,0x44,0x24,0x04,0x85,0xC0,0x74,0x3D},
     {0x8B,0x44,0x24,0x04,0xC1,0xE8,0x10,0x8B,0x04,0x85,0x38,0xD9,0x30,0x02,0x85,0xC0}}
};
const DcValueApi* g_dcValueApi=nullptr;
bool DcAllocatorReady() {
    const DcValueApi* api=g_dcApi==&kDcServer?&kDcValueServer:&kDcValueClient;
    if (!g_dcValueApi) {
        const unsigned long addresses[]={api->allocate,api->free,api->fromBlock};
        unsigned char bytes[16]; bool match=true;
        for (unsigned i=0;i<3;++i) {
            const bool ok=LiveRead(DcVa(addresses[i]),bytes,16) && std::memcmp(bytes,api->heads[i],16)==0;
            LOG_INFO("DataContainer: value allocator 0x%08lX, 16 bytes: %s",addresses[i],ok?"PASS":"FAIL");
            match=match && ok;
        }
        if (!match) { InterlockedExchange(&g_dcNativeState,-1); return false; }
        g_dcValueApi=api;
    }
    unsigned char initialized=0;
    unsigned long table=0,allocate=0;
    // The dedicated allocator itself asserts on this initialized flag. Client
    // 0041BE10 has no such flag; both require the real default arena vtable.
    return (!api->initialized || (LiveRead(DcVa(api->initialized),&initialized,1) && initialized)) &&
        LiveRead(DcVa(api->arena),&table,4) && table &&
        LiveRead(table+(g_dcApi==&kDcServer?8:4),&allocate,4) && DcCode(allocate);
}
bool DcValueReady() {
    if (!DcNativeThread()) return false;
    unsigned long count=1;
    return LiveRead(DcVa(g_dcValueApi->empty)-4,&count,4) && count==0;
}
unsigned long DcValueHeader() {
    __try {
        // The Array<T> field itself must belong to a native arena: resize calls fromBlock(this).
        if (g_dcApi==&kDcServer)
            return ((unsigned long(__thiscall*)(void*,unsigned long,unsigned long,int))(ULONG_PTR)DcVa(g_dcValueApi->allocate))
                ((void*)(ULONG_PTR)DcVa(g_dcValueApi->arena),4,4,0);
        return ((unsigned long(__thiscall*)(void*,unsigned long,unsigned long))(ULONG_PTR)DcVa(g_dcValueApi->allocate))
            ((void*)(ULONG_PTR)DcVa(g_dcValueApi->arena),4,4);
    } __except(EXCEPTION_EXECUTE_HANDLER) { DcNativeFault(); return 0; }
}
bool DcValueFree(unsigned long p) {
    __try {
        const unsigned long arena=((unsigned long(__cdecl*)(void*))(ULONG_PTR)DcVa(g_dcValueApi->fromBlock))((void*)(ULONG_PTR)p);
        if (!arena) return false;
        if (g_dcApi==&kDcServer)
            ((void(__thiscall*)(void*,void*,int))(ULONG_PTR)DcVa(g_dcValueApi->free))((void*)(ULONG_PTR)arena,(void*)(ULONG_PTR)p,0);
        else ((void(__thiscall*)(void*,void*))(ULONG_PTR)DcVa(g_dcValueApi->free))((void*)(ULONG_PTR)arena,(void*)(ULONG_PTR)p);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { DcNativeFault(); return false; }
}
bool DcDestroyValue(unsigned long header,unsigned long arrayType) {
    if (!DcValueReady()) return false;
    LiveDc array={header,arrayType,kShapeArray,0,0,0};
    ArrayView view;
    eng::TypeDesc element;
    if (!ViewArray(&array,&view) || !eng::Describe(view.elemType,&element)) return false;
    const unsigned alignment=element.kind==eng::kClass?4u:(std::max)(4u,element.alignment);
    unsigned long first=0;
    if (!ArrResizeNative(&array,0) || !LiveRead(header,&first,4)) return false;
    // resize(0) can retain storage; native first is arena-user + alignment (0046F196).
    if (first && first!=DcVa(g_dcValueApi->empty) && !DcValueFree(first-alignment)) return false;
    return DcValueFree(header);
}

// Frozen EBX arrays/strings are interior pointers, not individual heap blocks.
// VU 170D61F0/170D62A0 materializes them before making an instance writable.
// Native resize otherwise frees the old interior pointer (observed 004366D6).
struct DcThawField {
    unsigned long address=0,type=0,oldValue=0,header=0,newValue=0;
    unsigned alignment=4;
    bool array=false,initialized=false;
};
bool DcThawWritable(unsigned long address,unsigned size) {
    MEMORY_BASIC_INFORMATION region={};
    if (!address || !size || address+size<address ||
        !::VirtualQuery((const void*)(ULONG_PTR)address,&region,sizeof(region)) ||
        region.State!=MEM_COMMIT || (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    const DWORD protect=region.Protect&0xFF;
    return (protect==PAGE_READWRITE || protect==PAGE_WRITECOPY ||
            protect==PAGE_EXECUTE_READWRITE || protect==PAGE_EXECUTE_WRITECOPY) &&
        (ULONG_PTR)address+size<=(ULONG_PTR)region.BaseAddress+region.RegionSize;
}
thread_local char g_dcThawDiag[192];
thread_local char g_dcThawError[288];
bool DcThawFail(const char* reason,unsigned long type,int kind,unsigned detail) {
    eng::TypeDesc d;
    const char* name=(eng::Describe(type,&d) && d.name)?d.name:"?";
    _snprintf_s(g_dcThawDiag,sizeof(g_dcThawDiag),_TRUNCATE,"%s: type=%s kind=%d detail=%u @%08lX",
        reason,name,kind,detail,type);
    return false;
}
bool DcThawInspect(unsigned long type,unsigned long address,bool collectFields,
                   std::vector<DcThawField>& fields,std::vector<unsigned long>& refs,
                   unsigned depth,unsigned& budget) {
    if (depth>32 || !budget) return false;
    --budget;
    eng::TypeDesc desc;
    if (!eng::Describe(type,&desc)) return false;
    if (desc.kind==eng::kClass && !collectFields) {
        unsigned long p=0,state=0;
        if (!LiveRead(address,&p,4)) return false;
        if (!p) return true;
        // Stock BF3 references are direct. Do not interpret VU tagged lazy refs.
        if ((p&3) || !LiveRead(p+4,&state,4) || !(state&0xFFFF) ||
            (state&0x08000000UL) || (state&0xFFFF)==0xFFFF) return DcThawFail("frozen reference state",type,desc.kind,state);
        refs.push_back(p);
        return true;
    }
    if (desc.kind==eng::kArray) {
        LiveDc value={address,type,kShapeArray,0,0,0};ArrayView view;
        eng::TypeDesc element;
        if (!ViewArray(&value,&view) || !view.first) return DcThawFail("array is not readable",type,desc.kind,0);
        if (!eng::Describe(view.elemType,&element)) return DcThawFail("array element type is not readable",view.elemType,0,0);
        const unsigned alignment=element.kind==eng::kClass?4u:element.alignment;
        if (!alignment || (alignment&(alignment-1)) || view.stride%alignment)
            return DcThawFail("array element alignment",view.elemType,element.kind,alignment);
        if (view.count>budget || view.stride>65535 ||
            (view.count && view.first+view.count*view.stride<view.first))
            return DcThawFail("array count/stride",type,desc.kind,view.count);
        if (collectFields) {
            if (fields.size()>=4096 || !DcThawWritable(address,4)) return false;
            DcThawField field;field.address=address;field.type=type;field.oldValue=view.first;
            field.alignment=(std::max)(4u,alignment);field.array=true;
            fields.push_back(field);
        }
        for (unsigned long i=0;i<view.count;++i)
            if (!DcThawInspect(view.elemType,view.first+i*view.stride,false,fields,refs,depth+1,budget)) return false;
        return true;
    }
    if (desc.kind==eng::kCString) {
        unsigned long p=0;
        if (!LiveRead(address,&p,4)) return false;
        if (p) {
            bool terminated=false;
            for (unsigned i=0;i<1024*1024;++i) {
                char ch=0;if (!LiveRead(p+i,&ch,1)) return false;
                if (!ch) {terminated=true;break;}
            }
            if (!terminated) return false;
        }
        if (collectFields) {
            if (fields.size()>=4096 || !DcThawWritable(address,4)) return false;
            DcThawField field;field.address=address;field.type=type;field.oldValue=p;
            fields.push_back(field);
        }
        return true;
    }
    if (desc.kind==eng::kValueType || (desc.kind==eng::kClass && collectFields)) {
        if (!desc.size || desc.size>65535 || (desc.fieldCount && !desc.fields))
            return DcThawFail("structure layout",type,desc.kind,desc.size);
        if (desc.super && desc.super!=type &&
            !DcThawInspect(desc.super,address,collectFields,fields,refs,depth+1,budget)) return false;
        for (unsigned i=0;i<desc.fieldCount;++i) {
            unsigned short offset=0;unsigned long fieldType=0;eng::TypeDesc fieldDesc;
            if (!LiveRead(desc.fields+i*12+6,&offset,2) ||
                !LiveRead(desc.fields+i*12+8,&fieldType,4) || !eng::Describe(fieldType,&fieldDesc))
                return DcThawFail("field is not readable",type,desc.kind,i);
            const unsigned size=fieldDesc.kind==eng::kClass?4u:fieldDesc.size;
            if (!size || offset>desc.size || size>desc.size-offset)
                return DcThawFail("field size/offset",fieldType,fieldDesc.kind,offset);
            if (collectFields && fieldDesc.kind==eng::kClass) continue;
            if (!DcThawInspect(fieldType,address+offset,collectFields,fields,refs,depth+1,budget)) return false;
        }
        return true;
    }
    // Native type-copy knows these inline values; no owning pointers to transfer.
    if (desc.kind==eng::kBoolean || (desc.kind>=eng::kInt8 && desc.kind<=eng::kSHA1) ||
        desc.kind==eng::kEnum || desc.kind==eng::kResourceRef) {
        unsigned char bytes[64];
        return desc.size && desc.size<=sizeof(bytes) && LiveRead(address,bytes,desc.size);
    }
    return DcThawFail("no proven thaw contract for this kind",type,desc.kind,desc.size);
}
bool DcThawDiscard(DcThawField& field) {
    if (!field.header) return true;
    if (!field.initialized) {
        const bool ok=DcValueFree(field.header);field.header=0;return ok;
    }
    if (field.array) {
        LiveDc value={field.header,field.type,kShapeArray,0,0,0};unsigned long first=0;
        if (!ArrResizeNative(&value,0) || !LiveRead(field.header,&first,4)) return false;
        if (first && first!=DcVa(g_dcValueApi->empty) && !DcValueFree(first-field.alignment)) return false;
    } else {
        unsigned long text=0;
        if (!LiveRead(field.header,&text,4) || (text && !DcValueFree(text))) return false;
    }
    const bool ok=DcValueFree(field.header);field.header=0;return ok;
}
const char* DcMakeWritable(LiveDc* d) {
    if (d->shape!=kShapeInstance) return d->writable?nullptr:"make the owning DataContainer writable first";
    unsigned short flags=0;
    if (!LiveRead(d->ptr+6,&flags,2) || (flags&0x0800)) return "native instance is unreadable or destroyed";
    if (!(flags&0x2000) || (flags&0x0080)) return nullptr;
    if (!DcValueReady()) return "frozen instance requires verified native callback/frame and allocator";
    if (!DcThawWritable(d->ptr+6,2)) return "native instance flags are not writable";
    std::vector<DcThawField> fields;
    std::vector<unsigned long> refs;
    unsigned budget=1000000;
    if (!DcThawInspect(d->type,d->ptr,true,fields,refs,0,budget)) {
        _snprintf_s(g_dcThawError,sizeof(g_dcThawError),_TRUNCATE,
            "unsupported or invalid frozen field layout (%s)",g_dcThawDiag);
        return g_dcThawError;
    }
    std::sort(refs.begin(),refs.end());
    struct ReferenceState {unsigned long ptr,state;size_t count;};
    std::vector<ReferenceState> referenceStates;
    for (size_t first=0;first<refs.size();) {
        size_t next=first+1;while (next<refs.size() && refs[next]==refs[first]) ++next;
        unsigned long state=0;
        if (!LiveRead(refs[first]+4,&state,4) || (state&0x08000000UL) ||
            !(state&0xFFFF) || next-first>0xFFFF-(state&0xFFFF))
            return "frozen reference copy would overflow native refcount";
        referenceStates.push_back({refs[first],state,next-first});
        if ((state&0xFFFF)<=next-first) {
            LOG_INFO("DataContainer: frozen ownership probe owner=%08lX target=%08lX occurrences=%u state=%08lX",
                d->ptr,refs[first],static_cast<unsigned>(next-first),state);
            eng::ProbeInstanceArena(d->ptr);
            eng::ProbeInstanceArena(refs[first]);
        }
        first=next;
    }
    const char* error=nullptr;
    for (auto& field:fields) {
        field.header=DcValueHeader();
        const unsigned long empty=field.array?DcVa(g_dcValueApi->empty):0;
        const char* emptyText="";
        const unsigned long source=(!field.array && !field.oldValue)?
            (unsigned long)(ULONG_PTR)&emptyText:field.address;
        if (!field.header || !LiveWrite(field.header,&empty,4)) {error="native temporary field initialization failed";break;}
        field.initialized=true;
        if (!DcCopyNative(field.type,field.header,source,false) ||
            !LiveRead(field.header,&field.newValue,4)) {error="native frozen field copy failed";break;}
    }
    unsigned short current=0;
    if (!error && (!LiveRead(d->ptr+6,&current,2) || current!=flags)) error="instance changed during materialization";
    for (const auto& field:fields) {
        unsigned long currentValue=0;
        if (!error && (!LiveRead(field.address,&currentValue,4) || currentValue!=field.oldValue ||
            !DcThawWritable(field.address,4))) error="field changed during materialization";
    }
    if (error) {
        // No destination has changed; only owned temporary copies may be destroyed.
        if (g_dcNativeState==1) for (auto& field:fields) if (!DcThawDiscard(field)) {DcNativeFault();break;}
        return error;
    }
    for (const auto& ref:referenceStates) {
        unsigned long copied=0;
        if (LiveRead(ref.ptr+4,&copied,4) &&
            ((ref.state&0xFFFF)<=ref.count || copied!=ref.state+ref.count))
            LOG_INFO("DataContainer: frozen reference owner=%08lX target=%08lX occurrences=%u before=%08lX copied=%08lX",
                d->ptr,ref.ptr,static_cast<unsigned>(ref.count),ref.state,copied);
    }
    for (const auto& field:fields) {
        if (!LiveWrite(field.address,&field.newValue,4)) {DcNativeFault();return "frozen field commit failed";}
    }
    flags|=0x0080; // VU's thaw marker; retain native frozen-object lifetime flag 0x2000.
    if (!LiveWrite(d->ptr+6,&flags,2)) {DcNativeFault();return "frozen instance commit failed";}
    // These are newly owned array references, copied from borrowed frozen EBX
    // storage. EVBots does not run VU's post-marshal retain walk: there are no
    // matching old array retains to release here. Keep native copy's retains
    // until erase/clear/destruction of the new storage. Releasing them here
    // destroyed PlayerFilterEntityData while SoldierBlueprint still used it
    // (4 references, original count 2; 17.11 runtime/dump evidence).
    for (auto& field:fields) {
        // The copied data now belongs to the original DC. Free only the temporary
        // four-byte owner field, never its transferred array/string allocation.
        if (!DcValueFree(field.header)) {DcNativeFault();return "temporary owner release failed";}
        field.header=0;
    }
    LOG_INFO("DataContainer: materialized frozen %s @%08lX fields=%u references=%u",
        "instance",d->ptr,static_cast<unsigned>(fields.size()),static_cast<unsigned>(refs.size()));
    return nullptr;
}
unsigned long DcFindValueArray(unsigned long type) {
    unsigned long p=0;
    if (!LiveRead(DcVa(g_dcApi->types),&p,4)) return 0;
    for (unsigned i=0;p && i<65536;++i) {
        eng::TypeDesc d;
        if (eng::Describe(p,&d) && d.kind==eng::kArray && d.element==type) return p;
        unsigned long next=0;
        if (!LiveRead(p+4,&next,4) || next==p) return 0;
        p=next;
    }
    return 0;
}
int DcPushValue(lua_State* L,unsigned long type,unsigned long source) {
    eng::TypeDesc desc;
    if (!eng::Describe(type,&desc) || desc.kind!=eng::kValueType || !desc.size ||
        desc.size>65535 || !desc.alignment || (desc.alignment&(desc.alignment-1)))
        return luaL_error(L,"native value allocation requires a reflected type with a power-of-two alignment");
    if (!DcValueReady()) return luaL_error(L,"native value allocator is unavailable");
    const unsigned long arrayType=DcFindValueArray(type);
    if (!arrayType) return luaL_error(L,"native value type has no registered Array<T>");
    const unsigned long header=DcValueHeader();
    if (!header) return luaL_error(L,"native value allocation failed");
    const unsigned long empty=DcVa(g_dcValueApi->empty);
    LiveDc array={header,arrayType,kShapeArray,0,0,0}; ArrayView view;
    if (!LiveWrite(header,&empty,4) || !ArrResizeNative(&array,1) || !ViewArray(&array,&view) || view.count!=1) {
        if (g_dcNativeState==1) DcDestroyValue(header,arrayType);
        return luaL_error(L,"native value construction failed");
    }
    if (source && !DcCopyNative(type,view.first,source,false)) return luaL_error(L,"native value Clone failed");
    PushLive(L,view.first,type,kShapeStruct,true);
    LiveDc* owner=PushLive(L,header,arrayType,kShapeArray,false);
    owner->owned=2;
    lua_setiuservalue(L,-2,1);
    return 1;
}
const char* ArrValidate(lua_State* L,int index,const ArrayView& view,float (&matrix)[16],bool* haveMatrix) {
    if (view.elemKind==eng::kClass) {
        if (lua_isnil(L,index)) return nullptr;
        const LiveDc* value=static_cast<const LiveDc*>(luaL_testudata(L,index,kLiveMeta));
        eng::TypeDesc expected;
        if (!value || value->shape!=kShapeInstance || !eng::Describe(view.elemType,&expected) ||
            !eng::IsKindOfName(value->type,expected.name)) return "incompatible native array element";
        return nullptr;
    }
    if (view.elemKind==eng::kValueType) {
        const LiveDc* value=static_cast<const LiveDc*>(luaL_testudata(L,index,kLiveMeta));
        if (value && value->shape==kShapeStruct && value->type==view.elemType) return nullptr;
        eng::TypeDesc element;
        if (!lua_istable(L,index) || !eng::Describe(view.elemType,&element)) return "expected native structure element";
        if (std::strcmp(element.name, "LinearTransform") == 0) {
            *haveMatrix = ReadLinearTransformTable(L, index, element, matrix);
            return *haveMatrix ? nullptr : "expected a complete finite LinearTransform array element";
        }
        const int components=std::strcmp(element.name,"Vec2")==0?2:
            (std::strcmp(element.name,"Vec3")==0?3:(std::strcmp(element.name,"Vec4")==0?4:0));
        if (!components) return "expected native structure element";
        index=lua_absindex(L,index);
        lua_getfield(L,index,"_vt");
        const char* name=lua_tostring(L,-1);
        const bool matches=name && std::strcmp(name,element.name)==0;
        lua_pop(L,1);
        if (!matches) return "incompatible vector array element";
        static const char* fields[]={"x","y","z","w"};
        for (int i=0;i<components;++i) {
            lua_getfield(L,index,fields[i]);
            const bool numeric=lua_isnumber(L,-1)!=0;
            lua_pop(L,1);
            if (!numeric) return "vector array component must be numeric";
        }
        return nullptr;
    }
    if (view.elemKind==eng::kCString) {
        size_t len=0; const char* s=luaL_checklstring(L,index,&len);
        return len>1024*1024 || std::memchr(s,0,len) ? "invalid CString array element" : nullptr;
    }
    if (view.elemKind==eng::kBoolean) return nullptr;
    if (view.elemKind==eng::kFloat32 || view.elemKind==eng::kFloat64) { luaL_checknumber(L,index); return nullptr; }
    if ((view.elemKind>=eng::kInt8 && view.elemKind<=eng::kUint64) || view.elemKind==eng::kEnum) {
        luaL_checkinteger(L,index); return nullptr;
    }
    return "unsupported native array element type";
}
int Arr_Clear(lua_State* L) {
    const LiveDc* array=CheckArr(L,1); ArrayView view;
    if (!array->writable || !ViewArray(array,&view)) return luaL_error(L,"clear requires a writable native array");
    if (!DcNativeThread()) return luaL_error(L,"array mutation requires initialized engine TLS");
    if (!ArrResizeNative(array,0) || !ViewArray(array,&view) || view.count!=0) return luaL_error(L,"native array clear failed");
    return 0;
}
int Arr_Insert(lua_State* L) {
    const LiveDc* array=CheckArr(L,1);
    const bool append=lua_toboolean(L,lua_upvalueindex(1))!=0; ArrayView view;
    if (!array->writable || !ViewArray(array,&view)) return luaL_error(L,"insert requires a writable native array");
    if (!DcNativeThread()) return luaL_error(L,"array mutation requires initialized engine TLS");
    const lua_Integer index=append?(lua_Integer)view.count+1:luaL_checkinteger(L,2);
    const int valueIndex=append?2:3;
    if (index<1 || index>(lua_Integer)view.count+1 || view.count>=100000) return luaL_error(L,"invalid array insertion index/count");
    float matrix[16];
    bool haveMatrix = false;
    const char* error=ArrValidate(L,valueIndex,view,matrix,&haveMatrix);
    if (error) return luaL_error(L,"%s",error);
    const LiveDc* value=static_cast<const LiveDc*>(luaL_testudata(L,valueIndex,kLiveMeta));
    if (value && value->shape==kShapeStruct && value->ptr>=view.first && value->ptr<view.first+view.count*view.stride)
        return luaL_error(L,"insert requires a structure outside the resized array");
    const unsigned long oldCount=view.count;
    if (!ArrResizeNative(array,oldCount+1) || !ViewArray(array,&view) || view.count!=oldCount+1) return luaL_error(L,"native array resize failed");
    for (unsigned long i=oldCount;i>=(unsigned long)index;--i) {
        if (!DcCopyNative(view.elemType,view.first+i*view.stride,view.first+(i-1)*view.stride,view.elemKind==eng::kClass))
            return luaL_error(L,"native array element shift failed");
    }
    const unsigned long address = view.first + ((unsigned long)index - 1) * view.stride;
    if (haveMatrix) {
        if (!LiveWrite(address, matrix, sizeof(matrix))) return luaL_error(L, "native LinearTransform array write failed");
        return 0;
    }
    error=WriteLiveValue(L,valueIndex,address,view.elemType);
    if (error) return luaL_error(L,"%s",error);
    return 0;
}
int Arr_Erase(lua_State* L) {
    const LiveDc* array=CheckArr(L,1); const lua_Integer index=luaL_checkinteger(L,2); ArrayView view;
    if (!array->writable || !ViewArray(array,&view) || index<1 || index>(lua_Integer)view.count)
        return luaL_error(L,"erase requires a writable native array and a valid index");
    if (!DcNativeThread()) return luaL_error(L,"array mutation requires initialized engine TLS");
    for (unsigned long i=(unsigned long)index;i<view.count;++i) {
        if (!DcCopyNative(view.elemType,view.first+(i-1)*view.stride,view.first+i*view.stride,view.elemKind==eng::kClass))
            return luaL_error(L,"native array element shift failed");
    }
    if (!ArrResizeNative(array,view.count-1)) return luaL_error(L,"native array erase failed");
    return 0;
}

int Arr_Index(lua_State* L) {
    if (lua_isinteger(L, 2)) {
        return Arr_Get(L);
    }
    const char* key = luaL_checkstring(L, 2);
    if (std::strcmp(key, "get") == 0) { lua_pushcfunction(L, Arr_Get); return 1; }
    if (std::strcmp(key, "set") == 0) { lua_pushcfunction(L, Arr_Set); return 1; }
    if (std::strcmp(key, "add") == 0 || std::strcmp(key, "insert") == 0) {
        lua_pushboolean(L,std::strcmp(key,"add")==0);
        lua_pushcclosure(L,Arr_Insert,1);
        return 1;
    }
    if (std::strcmp(key,"clear")==0) { lua_pushcfunction(L,Arr_Clear); return 1; }
    if (std::strcmp(key,"erase")==0) { lua_pushcfunction(L,Arr_Erase); return 1; }
    lua_pushnil(L);
    return 1;
}

int Arr_NewIndexDirect(lua_State* L) {
    // __newindex(arr, i, v) == arr:set(i, v)
    return Arr_Set(L);
}

int Arr_ToString(lua_State* L) {
    const LiveDc* d = CheckArr(L, 1);
    ArrayView v;
    const bool ok = ViewArray(d, &v);
    lua_pushfstring(L, "Array(%d, движок)", ok ? (int)v.count : -1);
    return 1;
}

int Arr_PairsNext(lua_State* L) {
    const LiveDc* d=static_cast<const LiveDc*>(luaL_checkudata(L,lua_upvalueindex(1),kLiveArrMeta));
    const lua_Integer i=luaL_checkinteger(L,2)+1;
    ArrayView v;
    if (i<1 || !ViewArray(d,&v) || (unsigned long)i>v.count) { lua_pushnil(L); return 1; }
    lua_pushinteger(L,i);
    PushLiveValue(L,v.first+(unsigned long)(i-1)*v.stride,v.elemType,d->writable!=0);
    DcKeepParent(L,lua_upvalueindex(1));
    return 2;
}

int Arr_Pairs(lua_State* L) {
    luaL_checkudata(L,1,kLiveArrMeta);
    lua_pushvalue(L,1);
    lua_pushcclosure(L,Arr_PairsNext,1);
    lua_pushnil(L);
    lua_pushinteger(L,0);
    return 3;
}

int Lv_TypeInformation(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const unsigned long type = DcGameProcess() ? DcFindType(name) : 0;
    PushNativeTypeInfo(L, type);
    return 1;
}

bool RegisterLiveMetatables(lua_State* L) {
    lua_pushcfunction(L,Lv_CreateType);
    lua_setglobal(L,"EVBotsCreateDataContainer");
    lua_pushcfunction(L,Lv_TypeInformation);
    lua_setglobal(L,"EVBotsGetTypeInformation");
    luaL_newmetatable(L, kLiveMeta);
    lua_pushcfunction(L, Live_Index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Live_NewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Live_ToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Live_Eqmm);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L,Live_Gc);
    lua_setfield(L,-2,"__gc");
    // __name как у офлайн-контейнера: касты VextCustomization (SoldierWeaponUnlockAsset/UnlockAsset)
    // узнают DataContainer по нему и пропускают объект насквозь.
    lua_pushliteral(L, "vext.datacontainer");
    lua_setfield(L, -2, "__name");
    lua_pop(L, 1);

    luaL_newmetatable(L, kLiveArrMeta);
    lua_pushcfunction(L, Arr_Index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Arr_NewIndexDirect);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Arr_Len);
    lua_setfield(L, -2, "__len");
    lua_pushcfunction(L,Arr_Pairs);
    lua_setfield(L,-2,"__pairs");
    lua_pushcfunction(L, Arr_ToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L,Live_Gc);
    lua_setfield(L,-2,"__gc");
    lua_pop(L, 1);
    return true;
}

// ── натив для Lua-чанка: таблица live ───────────────────────────────────────

bool EnvOn(const char* name) {
    char v[8] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, v, (DWORD)sizeof(v));
    return n > 0 && v[0] == '1';
}

int Lv_Ready(lua_State* L) {
    lua_pushboolean(L, eng::Ready());
    return 1;
}

int Lv_Search(lua_State* L) {
    static int s_on = -1;
    if (s_on < 0) {
        // ★ Сборка 234: в клиенте (bf3.exe) офлайн-индекса EBX нет — только живые объекты движка, как VU
        s_on = (EnvOn("EVBOTS_LIVE_SEARCH") || ::GetModuleHandleA("bf3.exe") != nullptr) ? 1 : 0;
        LOG_INFO("EVBots: ResourceManager: поиск отдаёт %s (EVBOTS_LIVE_SEARCH=%d)",
                 s_on ? "ЖИВЫЕ объекты движка, офлайн-индекс — запасной" : "офлайн-индекс EBX (как раньше)", s_on);
    }
    lua_pushboolean(L, s_on == 1);
    return 1;
}

/// live.byName(name[, compartment]) -> DataContainer | nil
int Lv_ByName(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    PushLiveInstance(L, eng::FindAssetByName(name));
    return 1;
}

/// live.find(partitionGuid|nil, instanceGuid) -> DataContainer | nil. Попутно сверяет гипотезу
/// «GUID раздела по +0x0C» (раздел найденного инстанса против partitionGuid мода).
int Lv_Find(lua_State* L) {
    const char* pg = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    const char* ig = luaL_checkstring(L, 2);
    if (pg != nullptr && eng::PartitionGuidCheck() > 0) {
        // GUID раздела подтверждён: раздел ищем сравнением в памяти (дёшево), инстанс — внутри него
        const unsigned long p = eng::FindPartitionByGuid(pg);
        PushLiveInstance(L, p != 0 ? eng::FindInstanceInPartition(p, ig) : 0);
        return 1;
    }
    int layout = 0;
    const unsigned long obj = eng::FindInstanceByGuid(ig, &layout);
    if (obj == 0) {
        lua_pushnil(L);
        return 1;
    }
    static volatile LONG s_layoutLogged = 0;
    if (InterlockedExchange(&s_layoutLogged, 1) == 0) {
        LOG_INFO("EngineRes: инстанс по GUID найден (%s), раскладка GUID: %s", ig,
                 layout == 1 ? "Windows (Data1..3 LE) — как ожидалось" : "порядок текста");
    }
    if (pg != nullptr && eng::PartitionGuidCheck() == 0) {
        eng::Guid16 want, got;
        const unsigned long p = eng::PartitionOfInstance(obj);
        if (p != 0 && eng::ParseGuid(pg, &want, nullptr) && eng::PartitionGuid(p, &got)) {
            eng::ReportPartitionGuidCheck(std::memcmp(want.b, got.b, 16) == 0);
        }
    }
    PushLiveInstance(L, obj);
    return 1;
}

/// live.wrap(ptr) -> DataContainer | nil — объект данных движка по адресу (сборка 186: EntityFactory:Create)
int Lv_Wrap(lua_State* L) {
    PushLiveInstance(L, (unsigned long)luaL_checkinteger(L, 1));
    return 1;
}

/// live.inPartition(partitionPtr, instanceGuid) -> DataContainer | nil
int Lv_InPartition(lua_State* L) {
    const unsigned long p = (unsigned long)luaL_checknumber(L, 1);
    const char* ig = luaL_checkstring(L, 2);
    PushLiveInstance(L, eng::FindInstanceInPartition(p, ig));
    return 1;
}

int Lv_PartitionTable(lua_State* L) {
    PushPartitionTable(L, (unsigned long)luaL_checknumber(L, 1));
    return 1;
}

/// ★ Сборка 234: live.partitionInstances(ptr) -> массив живых DataContainer раздела (eng::PartitionInstances).
int Lv_PartitionInstances(lua_State* L) {
    const unsigned long p = (unsigned long)luaL_checknumber(L, 1);
    constexpr unsigned kCap = 65536;
    static unsigned long s_buf[kCap];
    const unsigned n = eng::PartitionInstances(p, s_buf, kCap);
    lua_createtable(L, static_cast<int>(n), 0);
    int k = 0;
    for (unsigned i = 0; i < n; ++i) {
        PushLiveInstance(L, s_buf[i]);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

int Lv_PartitionByGuid(lua_State* L) {
    PushPartitionTable(L, eng::FindPartitionByGuid(luaL_checkstring(L, 1)));
    return 1;
}

int Lv_PartitionOf(lua_State* L) {
    const LiveDc* d = static_cast<const LiveDc*>(luaL_testudata(L, 1, kLiveMeta));
    PushPartitionTable(L, d != nullptr ? eng::PartitionOfInstance(d->ptr) : 0);
    return 1;
}

int Lv_IsLive(lua_State* L) {
    lua_pushboolean(L, luaL_testudata(L, 1, kLiveMeta) != nullptr);
    return 1;
}

int Lv_Addr(lua_State* L) {
    const LiveDc* d = static_cast<const LiveDc*>(luaL_testudata(L, 1, kLiveMeta));
    lua_pushinteger(L, d != nullptr ? (lua_Integer)d->ptr : 0);
    return 1;
}

int Lv_Watch(lua_State* L) {
    if (lua_isstring(L, 1)) eng::WatchPartition(lua_tostring(L, 1));
    if (lua_isstring(L, 2)) eng::WatchInstance(lua_tostring(L, 2));
    return 0;
}

/// live.settings(typeName) -> DataContainer | nil. ★ Сборка 163: как VU — любой тип настроек из fb::SettingsManager
/// (eng::FindSettings), без частных адресов.
int Lv_Settings(lua_State* L) {
    const char* type = luaL_checkstring(L, 1);
    const unsigned long p = eng::FindSettings(type);
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 20) {
        LOG_INFO("EVBots: GetSettings(«%s») -> %p (SettingsManager движка)", type, (void*)(ULONG_PTR)p);
    }
    PushLiveInstance(L, p);
    // Settings are live runtime containers. VU mods edit GetSettings results
    // directly (e.g. DebugRenderSettings.enable during Extension:Loaded).
    // Use the normal writable transition, never bypass frozen storage checks.
    if (auto* settings=static_cast<LiveDc*>(luaL_testudata(L,-1,kLiveMeta))) {
        const char* error=DcMakeWritable(settings);
        if (error) return luaL_error(L,"GetSettings('%s'): %s",type,error);
        settings->writable=1;
    }
    return 1;
}

int Lv_MountSuperBundle(lua_State* L) {
    size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    if (length == 0 || std::strlen(name) != length)
        return luaL_error(L, "ResourceManager:MountSuperBundle: invalid empty/NUL name");
    const lua_Integer hint = luaL_optinteger(L, 2, 0);
    if (hint != 0 && hint != 1)
        return luaL_error(L, "ResourceManager:MountSuperBundle: MediaHint must be 0 or 1");
    if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TBOOLEAN);
    const bool optional = lua_toboolean(L, 3) != 0;
    const char* error = nullptr;
    if (!eng::MountSuperBundle(name, (int)hint, optional, &error)) {
        LOG_ERR("EngineRes: Lua MountSuperBundle(%s) отклонён: %s", name, error != nullptr ? error : "?");
        return luaL_error(L, "ResourceManager:MountSuperBundle: %s", error != nullptr ? error : "native failure");
    }
    return 0;  // VU: void, а не ложное подтверждение готовности ассетов
}

int Lv_AddRegistry(lua_State* L) {
    const LiveDc* d = static_cast<const LiveDc*>(luaL_testudata(L, 1, kLiveMeta));
    if (d == nullptr) return luaL_error(L, "ResourceManager:AddRegistry: expected a live RegistryContainer");
    const lua_Integer compartment = luaL_checkinteger(L, 2);
    if (compartment < 0 || compartment > 204)
        return luaL_error(L, "ResourceManager:AddRegistry: resource compartment is out of range");
    const char* error = nullptr;
    if (!eng::AddRegistry(d->ptr, (int)compartment, &error)) {
        LOG_ERR("EngineRes: Lua AddRegistry(%p, %d) отклонён: %s", (void*)(ULONG_PTR)d->ptr,
                (int)compartment, error != nullptr ? error : "?");
        return luaL_error(L, "ResourceManager:AddRegistry: %s", error != nullptr ? error : "native failure");
    }
    return 0;
}

void PushLiveTable(lua_State* L) {
    static const luaL_Reg kFns[] = {
        {"ready", Lv_Ready},           {"search", Lv_Search},
        {"byName", Lv_ByName},         {"find", Lv_Find},
        {"inPartition", Lv_InPartition}, {"partitionTable", Lv_PartitionTable},
        {"partitionInstances", Lv_PartitionInstances},
        {"partitionByGuid", Lv_PartitionByGuid}, {"partitionOf", Lv_PartitionOf},
        {"isLive", Lv_IsLive},         {"addr", Lv_Addr},
        {"wrap", Lv_Wrap},
        {"watch", Lv_Watch},           {"settings", Lv_Settings},
        {"mountSuperBundle", Lv_MountSuperBundle}, {"addRegistry", Lv_AddRegistry},
        {nullptr, nullptr},
    };
    lua_createtable(L, 0, 12);
    luaL_setfuncs(L, kFns, 0);
}

// ── загрузка раздела → Lua ───────────────────────────────────────────────────

struct DeferredPartition {
    unsigned long p;
    eng::Guid16 guid;  ///< GUID раздела на момент постановки — сверка при разборе очереди
};
static_assert(sizeof(DeferredPartition)==20,"compact partition queue record");
struct DeferredWatchedPartition {
    DeferredPartition partition;
    unsigned long generation;
};
constexpr int kDeferredMax = 65536;
DeferredWatchedPartition g_deferred[kDeferredMax];
int g_deferredCount = 0;
int g_deferredRead = 0;
bool g_anyLostSinceLoad = false;
CRITICAL_SECTION g_deferredGate;
volatile LONG g_deferredGateInit = 0;

void EnsureDeferredGate() {
    if (InterlockedCompareExchange(&g_deferredGateInit, 1, 0) == 0) {
        ::InitializeCriticalSection(&g_deferredGate);
        InterlockedExchange(&g_deferredGateInit, 2);
    }
    while (g_deferredGateInit != 2) ::Sleep(0);
}

bool DeliverPartition(unsigned long p, unsigned long generation, int waitMs) {
    eng::Guid16 g;
    char text[40] = {};
    if (eng::PartitionGuid(p, &g)) eng::FormatGuid(g, text);
    return LuaRuntime::Instance().CallTableFunction(
        "ResourceManager", "__OnPartition",
        {LuaRuntime::EngineEventArg::Str(text), LuaRuntime::EngineEventArg::Num((double)p),
         LuaRuntime::EngineEventArg::Num((double)generation)}, waitMs);
}

/// Колбэк перехвата (поток загрузки движка). Lua — под локом с ограниченным ожиданием,
/// иначе раздел ждёт в очереди до ближайшего тика (взаимная блокировка невозможна).
void OnPartitionLoaded(unsigned long p, const char* name, unsigned long generation) {
    DcCallbackScope nativeScope;
    if (DeliverPartition(p, generation, 100)) return;
    EnsureDeferredGate();
    ::EnterCriticalSection(&g_deferredGate);
    if (g_deferredCount < kDeferredMax) {
        auto& entry=g_deferred[(g_deferredRead+g_deferredCount)%kDeferredMax];
        entry.partition.p = p;
        entry.generation = generation;
        if (!eng::PartitionGuid(p, &entry.partition.guid)) {
            std::memset(entry.partition.guid.b, 0, 16);
            g_anyLostSinceLoad = true;
        }
        ++g_deferredCount;
    } else g_anyLostSinceLoad = true;
    ::LeaveCriticalSection(&g_deferredGate);
    LOG_WARN("EngineRes: раздел «%s» — Lua занята, load-хендлеры отложены до тика", name != nullptr ? name : "?");
}

// ★ Сборка 234: очередь Partition:Loaded (клиент). Перехват — в потоке загрузки движка, Lua — в кадре движка:
// здесь только адрес и имя, разбор — ResourcePartitionEventsDrain. Раздел мог выгрузиться — сверка GUID, как выше.
constexpr int kAnyMax = 65536;
DeferredPartition g_any[kAnyMax];
int g_anyCount = 0;
int g_anyRead = 0;
// The dedicated partition-name offset has not been independently mapped. Keep
// the real callback names in a bounded pool rather than inventing primary.name.
std::deque<std::string> g_anyNames;
size_t g_anyNameBytes = 0;
constexpr size_t kAnyNameBytesMax = 8*1024*1024;
unsigned long g_anyDropped = 0;
unsigned long g_anyTotal = 0;
bool ReadPartitionLabel(const char* source,char (&text)[4096],size_t* length) {
    if (!source) { text[0]=0;*length=0;return true; }
    __try {
        for (size_t i=0;i<sizeof(text);++i) {
            text[i]=source[i];
            if (!text[i]) { *length=i;return true; }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}

void OnAnyPartitionLoaded(unsigned long p, const char* name) {
    char text[4096];size_t length=0;
    const bool haveName=ReadPartitionLabel(name,text,&length);
    EnsureDeferredGate();
    ::EnterCriticalSection(&g_deferredGate);
    ++g_anyTotal;
    if (haveName && g_anyCount < kAnyMax && length+1 <= kAnyNameBytesMax-g_anyNameBytes) {
        DeferredPartition& d = g_any[(g_anyRead+g_anyCount)%kAnyMax];
        d.p = p;
        if (!eng::PartitionGuid(p, &d.guid)) { std::memset(d.guid.b,0,16);g_anyLostSinceLoad=true; }
        g_anyNames.emplace_back(text,length);
        g_anyNameBytes+=length+1;
        ++g_anyCount;
    } else {
        ++g_anyDropped;
        g_anyLostSinceLoad = true;
    }
    ::LeaveCriticalSection(&g_deferredGate);
}

bool OnLevelLoadResources(const char* name, const char* mode, bool dedicated) {
    DcCallbackScope nativeScope;
    // Startup callbacks need the engine's live settings and native TLS, before
    // the mod selects bundles or consumes this Level:LoadResources event.
    if (!EnsureNativeModStartup()) return false;
    eng::DeliverPreloadedWatchedPartitions();
    // Нельзя перенести в ResourceDrainDeferred: original сразу начинает загрузку.
    EnsureDeferredGate();
    ::EnterCriticalSection(&g_deferredGate);
    g_anyLostSinceLoad = false;
    ::LeaveCriticalSection(&g_deferredGate);
    return LuaRuntime::Instance().CallTableFunction("ResourceManager", "__OnLoadResources",
        {LuaRuntime::EngineEventArg::Str(name), LuaRuntime::EngineEventArg::Str(mode),
         LuaRuntime::EngineEventArg::Bool(dedicated)}, 100);
}

bool OnLevelRegisterResources(unsigned long levelData) {
    DcCallbackScope nativeScope;
    return LuaRuntime::Instance().CallTableFunction("ResourceManager", "__OnRegisterResources",
        {LuaRuntime::EngineEventArg::Num((double)levelData)}, 100);
}

// Вызывается под Lua gate. Сначала проверяем ВСЕ результаты Pass, затем заменяем
// native-owned копию целиком. Отдельная функция допускает проверку без процесса BF3.
bool ApplyResourceLoadBundlesHook(lua_State* L, std::vector<std::string>* bundles, int* compartment) {
    struct StackRestore {
        lua_State* L;
        int top;
        ~StackRestore() { lua_settop(L, top); }
    } stack{L, lua_gettop(L)};
    lua_getglobal(L, "Hooks");
    if (!lua_istable(L, -1)) return false;
    lua_getfield(L, -1, "_FireEx");
    if (!lua_isfunction(L, -1)) return false;
    lua_pushliteral(L, "ResourceManager:LoadBundles");
    lua_createtable(L, (int)bundles->size(), 0);
    for (size_t i = 0; i < bundles->size(); ++i) {
        lua_pushlstring(L, (*bundles)[i].data(), (*bundles)[i].size());
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_pushinteger(L, *compartment);
    if (lua_pcall(L, 3, 4, 0) != LUA_OK) {
        LOG_ERR("EngineRes: LoadBundles hook упал: %s", lua_tostring(L, -1));
        return false;
    }
    const char* mode = lua_tostring(L, -3);
    if (mode == nullptr || std::strcmp(mode, "pass") != 0) {
        LOG_ERR("EngineRes: LoadBundles — режим %s не поддержан; original не отменяется", mode != nullptr ? mode : "nil");
        return false;
    }
    int integer = 0;
    const lua_Integer nextCompartment = lua_tointegerx(L, -1, &integer);
    if (!integer || nextCompartment < 0 || nextCompartment > 204 || !lua_istable(L, -2)) {
        LOG_ERR("EngineRes: LoadBundles Pass — ожидаются string[] и ResourceCompartment 0..204");
        return false;
    }
    const lua_Unsigned rawCount = lua_rawlen(L, -2);
    if (rawCount > 4096) {
        LOG_ERR("EngineRes: LoadBundles Pass — список превышает 4096 элементов");
        return false;
    }
    const size_t count = (size_t)rawCount;
    std::vector<std::string> next;
    next.reserve(count);
    const int list = lua_absindex(L, -2);
    for (size_t i = 0; i < count; ++i) {
        lua_rawgeti(L, list, (lua_Integer)i + 1);
        size_t length = 0;
        const char* name = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &length) : nullptr;
        if (name == nullptr || length == 0 || length >= 4096 || std::memchr(name, '\0', length) != nullptr) {
            LOG_ERR("EngineRes: LoadBundles Pass — элемент %u не является непустой строкой без NUL (до 4095 байт)", (unsigned)i + 1);
            return false;
        }
        next.emplace_back(name, length);
        lua_pop(L, 1);
    }
    if (next != *bundles || nextCompartment != *compartment) {
        LOG_INFO("EngineRes: LoadBundles Lua Pass проверен: %u -> %u bundles, compartment %d -> %d",
                 (unsigned)bundles->size(), (unsigned)next.size(), *compartment, (int)nextCompartment);
    }
    bundles->swap(next);
    *compartment = (int)nextCompartment;
    return true;
}

// LoadBundles возвращает массив строк, поэтому CallTableFunction(outNum) здесь
// недостаточно. Вход в тот же Lua gate; захват ограничен 100 мс, очередь запрещена.
bool OnResourceLoadBundles(std::vector<std::string>* bundles, int* compartment) {
    DcCallbackScope nativeScope;
    auto& runtime = LuaRuntime::Instance();
    std::unique_lock<std::recursive_mutex> lock(runtime.TickGate(), std::defer_lock);
    const ULONGLONG deadline = ::GetTickCount64() + 100;
    while (!lock.try_lock()) {
        if (::GetTickCount64() >= deadline) return false;
        ::Sleep(1);
    }
    lua_State* L = runtime.state();
    return L != nullptr && ApplyResourceLoadBundlesHook(L, bundles, compartment);
}

}  // namespace

void ResourceClientStart() {
    if (!eng::Ready()) {
        LOG_WARN("EngineRes: клиент: слой ресурсов движка не поднялся — ResourceManager без живых объектов");
        return;
    }
    eng::SetAnyPartitionCallback(&OnAnyPartitionLoaded);
    ResourceLoadHandlersStart();
}

void ResourcePartitionEventsDrain() {
    DcDrainReleases();
    if (g_deferredGateInit != 2) return;
    // не больше kPerFrame разделов за кадр (остаток — в следующих кадрах): при загрузке уровня их тысячи
    constexpr int kPerFrame = 64;
    DeferredPartition local[kPerFrame];
    std::string names[kPerFrame];
    int n = 0;
    unsigned long dropped = 0, total = 0;
    ::EnterCriticalSection(&g_deferredGate);
    n = g_anyCount < kPerFrame ? g_anyCount : kPerFrame;
    for (int i = 0; i < n; ++i) {
        local[i] = g_any[g_anyRead];
        g_anyRead=(g_anyRead+1)%kAnyMax;
        g_anyNameBytes-=g_anyNames.front().size()+1;
        names[i]=std::move(g_anyNames.front());g_anyNames.pop_front();
    }
    g_anyCount -= n;
    dropped = g_anyDropped;
    total = g_anyTotal;
    g_anyDropped = 0;
    ::LeaveCriticalSection(&g_deferredGate);
    if (dropped != 0) LOG_WARN("EngineRes: Partition:Loaded — очередь полна, пропущено разделов: %lu", dropped);
    static unsigned long s_logged = 0;
    for (int i = 0; i < n; ++i) {
        eng::Guid16 now;
        if (!eng::PartitionGuid(local[i].p, &now) || std::memcmp(now.b, local[i].guid.b, 16) != 0) {
            ::EnterCriticalSection(&g_deferredGate);
            g_anyLostSinceLoad = true;
            ::LeaveCriticalSection(&g_deferredGate);
            continue;
        }
        if (s_logged < 3) {
            ++s_logged;
            LOG_INFO("EngineRes: ★ Partition:Loaded «%s» (всего разделов из перехвата: %lu)", names[i].c_str(), total);
        }
        const bool delivered = LuaRuntime::Instance().CallTableFunction(
            "ResourceManager", "__OnAnyPartition",
            {LuaRuntime::EngineEventArg::Num((double)local[i].p), LuaRuntime::EngineEventArg::Str(names[i].c_str())});
        if (!delivered) {
            ::EnterCriticalSection(&g_deferredGate);
            g_anyLostSinceLoad = true;
            ::LeaveCriticalSection(&g_deferredGate);
        }
    }
}

bool ResourceFlushPartitionEvents() {
    // Called synchronously by the verified native LoadingInfo hook. Deferred
    // handlers run on this native loading thread even before the first frame.
    DcCallbackScope nativeScope;
    EnsureDeferredGate();
    int pending = 0;
    bool lost = false;
    // At most one complete capacity per queue, including recursive enqueue.
    for (unsigned pass=0; pass<kAnyMax/64; ++pass) {
        ::EnterCriticalSection(&g_deferredGate);
        pending = g_anyCount+g_deferredCount;
        lost = g_anyLostSinceLoad || g_anyDropped != 0;
        ::LeaveCriticalSection(&g_deferredGate);
        if (pending == 0) break;
        ResourceDrainDeferred();
        ResourcePartitionEventsDrain();
    }
    ::EnterCriticalSection(&g_deferredGate);
    pending = g_anyCount+g_deferredCount;
    lost = lost || g_anyLostSinceLoad || g_anyDropped != 0;
    ::LeaveCriticalSection(&g_deferredGate);
    if (pending != 0 || lost) {
        LOG_ERR("EngineRes: synchronous Partition:Loaded flush incomplete: pending=%d, lost=%d",pending,lost?1:0);
        return false;
    }
    return true;
}

void ResourceLoadHandlersStart() {
    LOG_INFO("EngineRes: ResourceLoadHandlersStart: вход (Ready=%d)", eng::Ready() ? 1 : 0);
    if (!eng::Ready()) {
        LOG_WARN("EngineRes: ResourceLoadHandlersStart пропущен — eng::Ready()=false");
        return;
    }
    eng::SetAnyPartitionCallback(&OnAnyPartitionLoaded);
    const bool lifecycleOk = eng::InstallResourceLifecycleHooks(&OnLevelLoadResources, &OnLevelRegisterResources, &OnResourceLoadBundles);
    LOG_INFO("EngineRes: InstallResourceLifecycleHooks=%s", lifecycleOk ? "ok" : "нет");
    eng::InstallPartitionHook(&OnPartitionLoaded);
    const ULONGLONG t0 = ::GetTickCount64();
    LuaRuntime::Instance().CallTableFunction("ResourceManager", "__FireLate",
                                             {LuaRuntime::EngineEventArg::Str("старт сервера")});
    LOG_INFO("EngineRes: догоняющий вызов load-хендлеров (старт): %llu мс",
             (unsigned long long)(::GetTickCount64() - t0));
}

bool ResourceEnsureNativeStartup() {
    DcCallbackScope nativeScope;
    if (!eng::FindSettings("GameModeSettings") || !DcNativeThread()) return false;
    return EnsureNativeModStartup();
}

void ResourceLoadHandlersNewRound() {
    if (!eng::Ready()) return;
    ResourceDrainDeferred();
    LOG_INFO("EngineRes: разделов загружено после перехвата: %lu, из них с load-хендлерами: %lu",
             eng::PartitionHookCalls(), eng::PartitionHookMatches());
    const ULONGLONG t0 = ::GetTickCount64();
    LuaRuntime::Instance().CallTableFunction("ResourceManager", "__FireLate",
                                             {LuaRuntime::EngineEventArg::Str("новый раунд")});
    LOG_INFO("EngineRes: догоняющий вызов load-хендлеров (раунд): %llu мс",
             (unsigned long long)(::GetTickCount64() - t0));
}

void ResourceDrainDeferred() {
    DcDrainReleases();
    if (g_deferredGateInit != 2) return;
    constexpr int kPerFrame=64;
    DeferredWatchedPartition local[kPerFrame];
    int n = 0;
    ::EnterCriticalSection(&g_deferredGate);
    n = (std::min)(g_deferredCount,kPerFrame);
    for (int i = 0; i < n; ++i) {
        local[i] = g_deferred[g_deferredRead];
        g_deferredRead=(g_deferredRead+1)%kDeferredMax;
    }
    g_deferredCount -= n;
    ::LeaveCriticalSection(&g_deferredGate);
    for (int i = 0; i < n; ++i) {
        const unsigned long p = local[i].partition.p;
        const unsigned long generation = local[i].generation;
        // Тот же GUID и адрес ещё не означают тот же раздел после restart.
        eng::Guid16 now;
        const bool valid=eng::IsCurrentPartitionDelivery(p,generation) && eng::PartitionGuid(p,&now) &&
            std::memcmp(now.b,local[i].partition.guid.b,16)==0;
        if (!valid || !DeliverPartition(p,generation,-1)) {
            ::EnterCriticalSection(&g_deferredGate);g_anyLostSinceLoad=true;::LeaveCriticalSection(&g_deferredGate);
        }
    }
}

unsigned long LiveFindAssetByName(const char* name) {
    return eng::FindAssetByName(name);
}

bool LiveEngineReady() {
    return eng::Ready();
}

bool LiveSyncedSettingsCheck(unsigned long expectObj, unsigned expectOffset) {
    const unsigned long p = eng::FindSettings("SyncedGameSettings");
    eng::FieldDesc f;
    const bool fieldOk = p != 0 && eng::FindField(eng::TypeOf(p), "AllowClientSideDamageArbitration", &f);
    const bool ok = p != 0 && p == expectObj && fieldOk && f.offset == expectOffset;
    LOG_INFO("EngineRes: сверка GetSettings(«SyncedGameSettings»): SettingsManager %p, глобал движка %p, поле "
             "AllowClientSideDamageArbitration +0x%X (ожидалось +0x%X) — %s",
             (void*)(ULONG_PTR)p, (void*)(ULONG_PTR)expectObj, fieldOk ? f.offset : 0xFFFFu, expectOffset,
             ok ? "★ СОВПАЛО" : "НЕ совпало");
    return ok;
}

int l_resourcemanager_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "ERR ");
    const char* text = luaL_optstring(L, 2, "");
    LogPrintf(level, "EVBots: ResourceManager: %s", text);
    return 0;
}

// ── native-поиск по индексу EBX ────────────────────────────────────────────

/// nativeSearch(<путь>) -> DataContainer | nil
int l_native_search(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);

    EbxIndex& index = Ebx();
    if (!index.EnsureLoaded()) {
        // Ошибку загрузки пишем один раз — иначе она повторялась бы на
        // каждом SearchForDataContainer.
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ERR("EVBots: индекс EBX недоступен: %s",
                    index.loadError().c_str());
        }
        lua_pushnil(L);
        return 1;
    }

    // Сначала точное/безрегистровое совпадение, затем алиас (USEngi ->
    // USEngineer). Алиас срабатывает ТОЛЬКО на промахе, чтобы не маскировать
    // настоящие ошибки в путях.
    const EbxInstance* inst = index.FindByPath(path);
    if (inst == nullptr) {
        inst = index.FindByAlias(path);
    }
    if (inst == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    if (!PushDataContainer(L, inst)) {
        lua_pushnil(L);
        return 1;
    }
    return 1;
}

/// nativeSearchByGuid(<GUID>) -> DataContainer | nil
int l_native_search_by_guid(lua_State* L) {
    const char* guid = luaL_checkstring(L, 1);

    EbxIndex& index = Ebx();
    if (!index.EnsureLoaded()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ERR("EVBots: индекс EBX недоступен: %s",
                    index.loadError().c_str());
        }
        lua_pushnil(L);
        return 1;
    }

    const EbxInstance* inst = index.FindByGuid(guid);
    if (inst == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    if (!PushDataContainer(L, inst)) {
        lua_pushnil(L);
        return 1;
    }
    return 1;
}

bool RegisterVextResourceManager(lua_State* L) {
    // Метатаблица DataContainer нужна раньше, чем мод начнёт кастить
    // _G[typeInfo.name](inst) (EbxEditUtils.lua:19).
    if (!RegisterDataContainer(L)) {
        return false;
    }
    RegisterLiveMetatables(L);  // ★ 161: живой DataContainer и массив

    if (luaL_loadbufferx(L, kResourceManagerSource,
                         sizeof(kResourceManagerSource) - 1,
                         "=[vext-resourcemanager]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: ResourceManager не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_resourcemanager_log);
    lua_pushcfunction(L, l_native_search);
    lua_pushcfunction(L, l_native_search_by_guid);
    PushLiveTable(L);
    if (lua_pcall(L, 4, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: ResourceManager не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: ResourceManager подключен (поиск по индексу EBX, "
             "реестр обработчиков)");
    return true;
}

// ★ Сборка 178 (техника): живой DataContainer по указателю на инстанс движка (nil, если тип не читается).
// Нужен EntityManager: у ServerVehicleEntity data = [машина+0x14], мод читает VehicleEntityData(e.data).controllableType.
void EVBotsPushLiveInstance(lua_State* L, unsigned long obj) {
    PushLiveInstance(L, obj);
}

const char* ResourceGeometryType(lua_State* L, int index) {
    const auto* value = static_cast<const LiveDc*>(luaL_testudata(L, index, kLiveMeta));
    eng::TypeDesc desc;
    if (!value || value->shape != kShapeStruct || !value->ptr ||
        !eng::Describe(value->type, &desc) || desc.kind != eng::kValueType) return nullptr;
    static const char* names[] = {"Vec2", "Vec3", "Vec4", "Quat", "LinearTransform", "QuatTransform", "AxisAlignedBox"};
    for (const char* name : names) if (std::strcmp(desc.name, name) == 0) return name;
    return nullptr;
}

bool ResourceRetainLive(unsigned long obj) {
    // DcNativeReady checks file-derived signatures; it does not allocate in the
    // engine. DcRetain only changes the proven intrusive refcount at +4.
    return DcNativeReady() && DcRetain(obj);
}
void ResourceQueueRelease(unsigned long obj) {
    if (!obj) return;
    AcquireSRWLockExclusive(&g_dcReleaseLock);
    g_dcReleases.push_back(obj);
    ReleaseSRWLockExclusive(&g_dcReleaseLock);
}
namespace {
int PushTransferredLive(lua_State* L) {
    const unsigned long obj=(unsigned long)(ULONG_PTR)lua_touserdata(L,1);
    const unsigned long type=(unsigned long)lua_tointeger(L,2);
    luaL_getmetatable(L,kLiveMeta);
    const bool registered=lua_istable(L,-1)!=0;
    lua_pop(L,1);
    if (!registered) return luaL_error(L,"live DataContainer metatable is unavailable");
    LiveDc* d=PushLive(L,obj,type,kShapeInstance,false);
    // No throwing Lua operation after ownership is assigned. The caller can
    // queue the reference on protected-call failure without a double release.
    d->owned=1;
    return 1;
}
}
void ResourcePushOwnedLive(lua_State* L,unsigned long obj) {
    if (!obj) { lua_pushnil(L);return; }
    const unsigned long type=eng::TypeOf(obj);
    eng::TypeDesc desc;
    if (!type || !eng::Describe(type,&desc) || !eng::IsKindOfName(type,"DataContainer")) {
        ResourceQueueRelease(obj);lua_pushnil(L);return;
    }
    lua_pushcfunction(L,PushTransferredLive);
    lua_pushlightuserdata(L,(void*)(ULONG_PTR)obj);
    lua_pushinteger(L,type);
    if (lua_pcall(L,2,1,0)!=LUA_OK) {
        LOG_WARN("DataContainer: retained collision material push failed: %s",lua_tostring(L,-1));
        lua_pop(L,1);ResourceQueueRelease(obj);lua_pushnil(L);
    }
}

}  // namespace evbots
