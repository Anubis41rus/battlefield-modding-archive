// Проверка SearchForDataContainer вне BF3: поднимает голое окружение Lua,
// зовёт RegisterVextResourceManager и повторяет ровно те обращения к
// userdata, которые делает мод fun-bots (EbxEditUtils.lua:18-22, 42, 66).
//
// Собирается вручную (не в build.cmd), лежит рядом с исходниками.
// Запуск: EbxDcCheck.exe

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
bool RegisterVextResourceManager(lua_State* L);

/// Тестовая настройка лога: EVBotsLog пишет файл рядом с DLL и определяет
/// каталог по gModule, который в тесте надо задать вручную (в DLL его ставит
/// DllMain). Иначе причина отказа загрузки индекса не видна.
extern HMODULE gModule;
void LogInit();
void LogClose();
}  // namespace evbots

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    ok ? ++g_pass : ++g_fail;
}

static void CheckStr(const char* got, const char* want, const char* what) {
    const bool ok = (got != nullptr && std::strcmp(got, want) == 0);
    printf("  [%s] %s = %s\n", ok ? " OK " : "FAIL", what,
           got != nullptr ? got : "(nil)");
    if (!ok) {
        printf("         ожидалось: %s\n", want);
    }
    ok ? ++g_pass : ++g_fail;
}

static void Run(lua_State* L, const char* name, const char* script) {
    printf("\n--- %s ---\n", name);
    if (luaL_loadbufferx(L, script, std::strlen(script), "=[check]", "t") !=
            LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        printf("  [FAIL] ошибка Lua: %s\n", err != nullptr ? err : "?");
        ++g_fail;
        return;
    }
}

/// Выполнить выражение и забрать строку-результат в out.
static void EvalStr(lua_State* L, const char* expr, char* out,
                    std::size_t outSize) {
    out[0] = '\0';
    const char* script = "return " ;
    const std::size_t n = std::strlen(expr) + 16;
    char* buf = new char[n];
    std::snprintf(buf, n, "%s%s", script, expr);
    if (luaL_loadbufferx(L, buf, std::strlen(buf), "=[eval]", "t") !=
            LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::snprintf(out, outSize, "ERR: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        std::snprintf(out, outSize, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        std::snprintf(out, outSize, "<%s>", luaL_typename(L, -1));
        lua_pop(L, 1);
    }
    delete[] buf;
}

/// То же, но без кавычек, которые Lua-строка получила при возврате наружу:
/// так проверять значения полей удобнее, чем через tostring().
static void EvalText(lua_State* L, const char* expr, char* out,
                     std::size_t outSize) {
    EvalStr(L, expr, out, outSize);
    const std::size_t len = std::strlen(out);
    if (len >= 2 && out[0] == '"' && out[len - 1] == '"') {
        memmove(out, out + 1, len - 2);
        out[len - 2] = '\0';
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        printf("[FAIL] не создалось состояние Lua\n");
        return 1;
    }
    luaL_openlibs(L);
    evbots::gModule = GetModuleHandleA(nullptr);  // как делает DllMain в DLL
    evbots::LogInit();

    printf("=== SearchForDataContainer: проверка вне BF3 ===\n");
    printf("индекс: EVBOTS_EBX_INDEX=%s\n",
           getenv("EVBOTS_EBX_INDEX") != nullptr
               ? getenv("EVBOTS_EBX_INDEX")
               : "(по умолчанию)");

    if (!evbots::RegisterVextResourceManager(L)) {
        printf("[FAIL] RegisterVextResourceManager вернул false\n");
        lua_close(L);
        return 1;
    }

    // gModule в тесте не задан (DLL нет — лог пишет только в консоль),
    // поэтому причину отказа загрузки индекса печатаем сами.
    printf("\n--- 0. загрузка индекса ---\n");
    lua_getglobal(L, "ResourceManager");          // [RM]
    lua_getfield(L, -1, "SearchForDataContainer");  // [RM, fn]
    lua_pushvalue(L, -2);                          // [RM, fn, RM]  <- self
    lua_pushstring(L, "Weapons/M416/U_M416");      // [RM, fn, RM, path]
    const int rc = lua_pcall(L, 2, 1, 0);          // [RM, result]
    if (rc == LUA_OK && lua_type(L, -1) == LUA_TUSERDATA) {
        printf("  [ OK ] индекс доступен, первый поиск вернул userdata\n");
        ++g_pass;
    } else {
        const char* detail =
            (rc == LUA_OK) ? luaL_typename(L, -1) : lua_tostring(L, -1);
        printf("  [FAIL] индекс не дал userdata: rc=%d %s\n", rc,
               detail != nullptr ? detail : "?");
        ++g_fail;
    }
    lua_pop(L, 2);

    char buf[512] = {};

    // ── 1. Базовый контракт: userdata с typeInfo.name ──
    printf("\n--- 1. тип результата и typeInfo.name ---\n");
    Run(L, "базовый поиск", R"LUA(
local dc = ResourceManager:SearchForDataContainer('Weapons/M416/U_M416')
assert(dc ~= nil, 'SearchForDataContainer вернул nil')
assert(type(dc) == 'userdata', 'ожидался userdata, получено ' .. type(dc))
assert(dc.typeInfo ~= nil, 'нет typeInfo')
assert(dc.typeInfo.name == 'SoldierWeaponUnlockAsset',
       'Неверный typeInfo.name: ' .. tostring(dc.typeInfo.name))
)LUA");

    // ── 2. Каст _G[typeInfo.name](inst) — EbxEditUtils.lua:19 ──
    Run(L, "каст к типу", R"LUA(
local dc = ResourceManager:SearchForDataContainer('Weapons/M67/U_M67')
assert(dc.typeInfo.name == 'SoldierWeaponUnlockAsset')
local casted = _G[dc.typeInfo.name](dc)
assert(casted ~= nil, 'каст вернул nil')
assert(casted.typeInfo.name == 'SoldierWeaponUnlockAsset')
assert(casted.MakeWritable ~= nil, 'нет метода MakeWritable')
casted:MakeWritable()
assert(casted.isReadOnly == false, 'после MakeWritable должно быть writable')
)LUA");

    // ── 3. Типы инстансов по всем путям, которые ищет мод ──
    printf("\n--- 3. классы инстансов ---\n");
    EvalStr(L, "ResourceManager:SearchForDataContainer('Weapons/M67/U_M67').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "SoldierWeaponUnlockAsset", "Weapons/M67/U_M67");

    EvalStr(L, "ResourceManager:SearchForDataContainer('Weapons/M67/M67_Projectile').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "ProjectileBlueprint", "Weapons/M67/M67_Projectile");

    EvalStr(L, "ResourceManager:SearchForDataContainer('Weapons/M416/U_M416_Kobra').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "UnlockAsset", "Weapons/M416/U_M416_Kobra");

    EvalStr(L, "ResourceManager:SearchForDataContainer('Weapons/Knife/U_Knife').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "SoldierWeaponUnlockAsset", "Weapons/Knife/U_Knife");

    EvalStr(L, "ResourceManager:SearchForDataContainer('Gameplay/Kits/USAssault').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "VeniceSoldierCustomizationAsset", "Gameplay/Kits/USAssault");

    EvalStr(L, "ResourceManager:SearchForDataContainer('Characters/Soldiers/MpSoldier').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "SoldierBlueprint", "Characters/Soldiers/MpSoldier");

    // ── 4. Регистр пути не важен (мод мешает) ──
    printf("\n--- 4. регистр пути ---\n");
    EvalStr(L, "ResourceManager:SearchForDataContainer('persistence/unlocks/soldiers/specializations/sprintboostl2').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "ValueUnlockAsset", "путь в нижнем регистре");

    // ── 5. Поля инстанса ──
    printf("\n--- 5. поля инстанса ---\n");
    EvalText(L, "ResourceManager:SearchForDataContainer('Weapons/M416/U_M416').DebugUnlockId",
             buf, sizeof(buf));
    CheckStr(buf, "U_M416", "DebugUnlockId");

    EvalStr(L, "tostring(ResourceManager:SearchForDataContainer('Weapons/M416/U_M416').AutoAvailable) .. ''",
            buf, sizeof(buf));
    CheckStr(buf, "false", "AutoAvailable — boolean, а не строка 'False'");

    EvalText(L, "ResourceManager:SearchForDataContainer('Weapons/M416/U_M416').Name",
             buf, sizeof(buf));
    CheckStr(buf, "Weapons/M416/U_M416", "Name (поле инстанса)");

    EvalStr(L, "type(ResourceManager:SearchForDataContainer('Weapons/M416/U_M416').UnlockScore) .. ''",
            buf, sizeof(buf));
    CheckStr(buf, "number", "UnlockScore — настоящий Lua number");

    EvalStr(L, "type(ResourceManager:SearchForDataContainer('Weapons/M416/U_M416').AutoAvailable) .. ''",
            buf, sizeof(buf));
    CheckStr(buf, "boolean", "AutoAvailable — настоящий Lua boolean");

    EvalStr(L, "ResourceManager:SearchForDataContainer('Weapons/M416/U_M416').instanceGuid",
            buf, sizeof(buf));
    CheckStr(buf, "3A6B6A16-E5A1-33E0-5B53-56E77833DAF4", "instanceGuid");

    // Поле не существует — nil, а не ошибка (мод на это опирается,
    // EbxEditUtils:CheckInstancePropertyExists).
    EvalStr(L, "tostring(ResourceManager:SearchForDataContainer('Weapons/M416/U_M416').NoSuchField)",
            buf, sizeof(buf));
    CheckStr(buf, "nil", "несуществующее поле -> nil");

    // ── 6. Отсутствующий путь -> nil, без исключения ──
    printf("\n--- 6. отсутствующий путь ---\n");
    EvalStr(L, "tostring(ResourceManager:SearchForDataContainer('Weapons/NoSuch/U_NoSuch')) .. ''",
            buf, sizeof(buf));
    CheckStr(buf, "nil", "несуществующий путь");

    EvalStr(L, "tostring(ResourceManager:SearchForDataContainer('')) .. ''", buf,
            sizeof(buf));
    CheckStr(buf, "nil", "пустая строка");

    // ── 7. Поиск по GUID ──
    printf("\n--- 7. SearchForInstanceByGuid ---\n");
    EvalStr(L, "ResourceManager:SearchForInstanceByGuid('3A6B6A16-E5A1-33E0-5B53-56E77833DAF4').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "SoldierWeaponUnlockAsset", "по GUID ствола M416");

    EvalStr(L, "ResourceManager:SearchForInstanceByGuid('261E43BF-259B-41D2-BF3B-9AE4DDA96AD2').typeInfo.name",
            buf, sizeof(buf));
    CheckStr(buf, "SoldierBlueprint", "SoldierBlueprint по GUID (BotSpawner:935)");

    // ── 8. Мод зовёт ResourceManager:SearchForDataContainer(...) точечно ──
    printf("\n--- 8. вызов через двоеточие ---\n");
    Run(L, "точечный вызов", R"LUA(
local dc = ResourceManager:SearchForDataContainer('Weapons/M416/U_M416')
assert(dc ~= nil)
)LUA");

    // ── 9. read-only: запись без MakeWritable обязана ругаться ──
    printf("\n--- 9. read-only инстанс ---\n");
    EvalStr(L, "tostring((pcall(function() ResourceManager:SearchForDataContainer('Weapons/M416/U_M416').Name = 'x' end))) .. ''",
            buf, sizeof(buf));
    CheckStr(buf, "false", "запись в поле падает (read-only)");

    // ── 10. Сценарий мода целиком: шесть слотов + вложения ──
    // Повторяет BotSpawner:_GetCustomization (BotSpawner.lua:2022-2103) и
    // _SetPrimaryAttachments (:1985-1996) на настоящих путях.
    printf("\n--- 10. сценарий BotSpawner:_GetCustomization ---\n");
    Run(L, "шесть слотов оружия", R"LUA(
local slots = {
  { 'Weapons/M416/U_M416',   'SoldierWeaponUnlockAsset' }, -- слот 0, ствол
  { 'Weapons/M1911/U_M1911', 'SoldierWeaponUnlockAsset' }, -- слот 1, пистолет
  { 'Weapons/FIM92A/U_FIM92', 'SoldierWeaponUnlockAsset' }, -- слот 2, гаджет
  { 'Weapons/SMAW/U_SMAW',   'SoldierWeaponUnlockAsset' }, -- слот 4, гаджет
  { 'Weapons/M67/U_M67',     'SoldierWeaponUnlockAsset' }, -- слот 6, граната
  { 'Weapons/Knife/U_Knife', 'SoldierWeaponUnlockAsset' }, -- слот 7, нож
}
for _, s in ipairs(slots) do
  local res = ResourceManager:SearchForDataContainer(s[1])
  assert(res ~= nil, 'не найдено: ' .. s[1])
  assert(res.typeInfo.name == s[2],
         s[1] .. ': ожидался ' .. s[2] .. ', получено ' .. res.typeInfo.name)
  local w = _G[res.typeInfo.name](res)   -- «каст», как делает мод
  assert(w.typeInfo.name == s[2])
  w:MakeWritable()
end
)LUA");

    Run(L, "вложения (unlockAssets)", R"LUA(
-- BotSpawner:_SetPrimaryAttachments -> unlockAssets:add(UnlockAsset(asset))
local att = ResourceManager:SearchForDataContainer('Weapons/M416/U_M416_Kobra')
assert(att ~= nil, 'прицел не найден')
assert(att.typeInfo.name == 'UnlockAsset', att.typeInfo.name)
local wrapped = UnlockAsset(att)
assert(wrapped.typeInfo ~= nil, 'UnlockAsset() съел typeInfo у DataContainer')
assert(wrapped.typeInfo.name == 'UnlockAsset')
)LUA");

    // ── 11. Разыменовка ссылок: главный сценарий, ради которого всё затевалось ──
    // Повторяет WeaponClass:learnStatsValues (WeaponClass.lua:44-65):
    //   SearchForDataContainer('Weapons/M416/U_M416')
    //     -> .Weapon      (ссылка путь/GUID)  -> SoldierWeaponBlueprint
    //     -> .Object     (ссылка, тот же файл) -> SoldierWeaponData
    //     -> .WeaponFiring (ссылка)          -> WeaponFiringData
    //     -> .PrimaryFire  (ссылка)          -> FiringFunctionData
    //     -> .Shot        (вложенная структура ShotConfigData)
    //     -> .ProjectileData (ссылка)        -> BulletEntityData
    // Плюс .AIData -> AIWeaponData из другого каталога (AI/...).
    printf("\n--- 11. разыменовка ссылок (WeaponClass:learnStatsValues) ---\n");
    Run(L, "полная цепочка Weapon.Object.WeaponFiring.PrimaryFire", R"LUA(
-- Именно те имена, которые мод передаёт в EbxEditUtils:GetWritableContainer.
-- RegisterMemberName в моде приводит их к нижнему регистру первой буквы,
-- поэтому проверяем ИМЕННО ТАКИЕ имена: 'weapon', 'object', 'weaponFiring',
-- 'primaryFire', 'shot', 'projectileData'.
local root = ResourceManager:SearchForDataContainer('Weapons/M416/U_M416')
assert(root ~= nil, 'ствол M416 не найден')
assert(root.typeInfo.name == 'SoldierWeaponUnlockAsset')

-- 1. Поле-ссылка отдаёт DataContainer, а не строку
local weapon = root.weapon
assert(weapon ~= nil, 'поле weapon вернуло nil')
assert(type(weapon) == 'userdata', 'weapon — не userdata, а ' .. type(weapon))
assert(weapon.typeInfo.name == 'SoldierWeaponBlueprint',
       'weapon: ожидался SoldierWeaponBlueprint, получено ' ..
       tostring(weapon.typeInfo.name))

-- 2. Разыменованный объект тоже кастуется (EbxEditUtils.lua:70)
local casted = _G[weapon.typeInfo.name](weapon)
assert(casted ~= nil and casted.typeInfo.name == 'SoldierWeaponBlueprint')
casted:MakeWritable()

-- 3. Ссылка внутри того же файла (голый GUID, поле Object)
local object = weapon.object
assert(object ~= nil, 'поле object вернуло nil')
assert(type(object) == 'userdata', 'object — не userdata')
assert(object.typeInfo.name == 'SoldierWeaponData',
       'object: ' .. tostring(object.typeInfo.name))

-- 4. WeaponFiring -> PrimaryFire
local firing = object.weaponFiring
assert(firing ~= nil, 'поле weaponFiring вернуло nil')
assert(firing.typeInfo.name == 'WeaponFiringData', firing.typeInfo.name)

local primary = firing.primaryFire
assert(primary ~= nil, 'поле primaryFire вернуло nil')
assert(primary.typeInfo.name == 'FiringFunctionData', primary.typeInfo.name)

-- 5. Вложенная структура Shot::ShotConfigData -> ProjectileData (ссылка)
local shot = primary.shot
assert(shot ~= nil, 'поле shot вернуло nil')
assert(type(shot) == 'userdata', 'shot — не userdata')
assert(shot.typeInfo.name == 'ShotConfigData',
       'shot: ' .. tostring(shot.typeInfo.name))
-- каст вложенной структуры тоже обязан работать
assert(_G[shot.typeInfo.name](shot).typeInfo.name == 'ShotConfigData')

local bullet = shot.projectileData
assert(bullet ~= nil, 'поле projectileData вернуло nil')
assert(type(bullet) == 'userdata', 'projectileData — не userdata')
assert(bullet.typeInfo.name == 'BulletEntityData', bullet.typeInfo.name)

-- 6. Ссылка в ДРУГОЙ каталог (AI/...) — тоже резолвится
local ai = object.aiData
assert(ai ~= nil, 'поле aiData вернуло nil')
assert(type(ai) == 'userdata', 'aiData — не userdata')
assert(ai.typeInfo.name == 'AIWeaponData', ai.typeInfo.name)

-- 7. Ссылка из другого раздела (Weapons/Common/Bullets/...) доступна по GUID
assert(bullet.instanceGuid ~= nil, 'у пули нет instanceGuid')
local byGuid = ResourceManager:SearchForInstanceByGuid(bullet.instanceGuid)
assert(byGuid ~= nil, 'SearchForInstanceByGuid не нашёл пулю')
assert(byGuid.typeInfo.name == 'BulletEntityData')
)LUA");

    // ── 12. Алиас USEngi -> USEngineer ──
    printf("\n--- 12. алиас USEngi -> USEngineer ---\n");
    Run(L, "алиас комплекта инженера", R"LUA(
-- BotSpawner.lua:2199 конкатенирует 'Engi', в данных файл USEngineer.txt.
local kit = ResourceManager:SearchForDataContainer('Gameplay/Kits/USEngi')
assert(kit ~= nil, 'алиас USEngi не сработал')
assert(kit.typeInfo.name == 'VeniceSoldierCustomizationAsset', kit.typeInfo.name)
assert(kit.Name == 'Gameplay/Kits/USEngineer',
       'алиас отдал не тот комплект: ' .. tostring(kit.Name))

-- Варианты с суффиксами тоже должны находиться
for _, p in ipairs({ 'Gameplay/Kits/USEngi_GM',
                     'Gameplay/Kits/USEngi_XP4',
                     'Gameplay/Kits/USEngi_GM_XP4',
                     'Gameplay/Kits/USEngi_XP4_SCV' }) do
  local k = ResourceManager:SearchForDataContainer(p)
  assert(k ~= nil, 'алиас не сработал для ' .. p)
end

-- Точное имя продолжает работать как раньше
local direct = ResourceManager:SearchForDataContainer('Gameplay/Kits/USEngineer')
assert(direct ~= nil and direct.typeInfo.name == 'VeniceSoldierCustomizationAsset')

-- Чужие пути алиасом НЕ подменяются
assert(ResourceManager:SearchForDataContainer('Gameplay/Kits/USEngiXXX') == nil,
       'алиас сработал на постороннем пути')
)LUA");

    // ── 13. Регистр пути: точный первый, безрегистровый запасной ──
    printf("\n--- 13. регистр пути ---\n");
    Run(L, "нижний регистр для ~30 перков", R"LUA(
local paths = {
  'persistence/unlocks/soldiers/specializations/sprintboostl2',
  'persistence/unlocks/soldiers/specializations/ammoboostl2',
  'PERSISTENCE/UNLOCKS/VEHICLES/MBTREACTIVEARMOR',
  'persistence/unlocks/soldiers/visual/mp/us/mp_us_assault_appearance01',
}
for _, p in ipairs(paths) do
  local dc = ResourceManager:SearchForDataContainer(p)
  assert(dc ~= nil, 'не найдено (регистр): ' .. p)
  assert(type(dc) == 'userdata', p .. ': не userdata')
  assert(dc.typeInfo.name ~= nil, p .. ': нет typeInfo.name')
end
)LUA");

    printf("\n=== ИТОГ: успешно %d, провалено %d ===\n", g_pass, g_fail);
    lua_close(L);
    evbots::LogClose();
    return g_fail == 0 ? 0 : 1;
}