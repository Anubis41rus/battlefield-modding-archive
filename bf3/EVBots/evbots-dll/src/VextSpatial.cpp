// VextSpatial.cpp — SpatialEntity (stub) для EVBots.
//
// Реализация SpatialEntity (обёртка Entity → позиция/трансформ) и
// AxisAlignedBox — stub-типы, возвращающие нулевые данные.
// На реальном VU это нативные типы; в EVBots данные недоступны
// без botlayer.dll.
//
// Также добавляет Vec3.__len (метаметод #), чтобы #vec3 работал
// как magnitude (нужен для совместимости с Lua-кодом, который
// использует # вместо .magnitude).
//
// Документация:
//   content\vext\ref\shared\type\spatialentity.md
//   content\vext\ref\shared\type\axisalignedbox.md

#include "VextSpatial.h"

#include <string>

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// ── источник Lua ─────────────────────────────────────────────────────────────
const char kSpatialSource[] = R"LUA(
-- SpatialEntity (stub) и AxisAlignedBox (stub).
-- Зависит от Vec3, LinearTransform — должен регистрироваться после них.

local sqrt = math.sqrt

-- Vec3.__len: длина вектора через метаметод # ───────────────────────────────
-- Добавляем __len к уже зарегистрированному Vec3.
-- В VextGeometry Vec3Mt находится в upvalue (недоступен), но мы можем
-- получить его из глобального Vec3 через getmetatable.
local Vec3global = _G.Vec3
local Vec3Mt = getmetatable(Vec3global)
if Vec3Mt then
  Vec3Mt.__len = function(self)
    return sqrt(self.x * self.x + self.y * self.y + self.z * self.z)
  end
end

-- SpatialTransform ───────────────────────────────────────────────────────────
-- Внутренний тип: table с полями trans (Vec3) и forward (Vec3).
-- Используется как .transform у SpatialEntity.
-- LinearTransform из VextGeometry имеет .trans и .forward, поэтому
-- возвращаем именно его (он уже правильный).
local SpatialTransformMt = {
  __index = function(self, k)
    -- проксируем к LinearTransform
    return Rawget(self, k)
  end
}

local function makeSpatialTransform(trans, forward)
  -- Берём identity LinearTransform и подменяем trans + forward.
  -- LinearTransform: .left/.up/.forward/.trans — нам нужны только trans/.forward.
  local lt = LinearTransform()
  lt.trans = trans or Vec3._make(0, 0, 0)
  lt.forward = forward or Vec3._make(0, 0, 1)
  return lt
end

-- SpatialEntity ───────────────────────────────────────────────────────────────
local SpatialEntityMethods = {}
local SpatialEntityMt = { __index = SpatialEntityMethods }

function SpatialEntityMethods:GetPosition()
  -- NOTE: реальная позиция недоступна (нет данных от botlayer).
  -- Код, использующий этот метод, получит Vec3(0,0,0).
  return Vec3._make(0, 0, 0)
end

function SpatialEntityMethods:GetTranslation()
  return Vec3._make(0, 0, 0)
end

function SpatialEntityMethods:DistanceTo(other)
  -- Заглушка: всегда 0. Реально нужны позиции обеих сущностей.
  return 0
end

-- aabb stub
local AabbMethods = {}
local AabbMt = { __index = AabbMethods }
function AabbMethods:Clone() return self end
function AabbMethods.__eq(a, b) return false end

local function makeAabb()
  return setmetatable({
    min = Vec3._make(0, 0, 0),
    max = Vec3._make(0, 0, 0),
  }, AabbMt)
end

local SpatialEntity = setmetatable({}, {
  __call = function(_, entity)
    -- Живая legacy-обёртка: чтение и запись идут исходной сущности.
    -- Иначе присваивание transform затеняет нативное свойство в пустой прокси.
    -- Проверки записи/readonly остаются в исходном __newindex сущности.
    if type(entity) == "table" and rawget(entity, "_ptr") ~= nil then
      local stub = { aabb = makeAabb(), aabbTransform = makeSpatialTransform() }
      return setmetatable({}, { __index = function(_, k)
        local v = entity[k]
        if v ~= nil then return v end
        local sv = stub[k]
        if sv ~= nil then return sv end
        return SpatialEntityMethods[k]
      end, __newindex = function(_, k, v)
        entity[k] = v
      end })
    end
    -- SpatialEntity(entity) — entity игнорируется (stub),
    -- возвращаем объект-заглушку
    return setmetatable({
      _entity = entity,       -- запоминаем для отладки
      transform = makeSpatialTransform(),
      aabb = makeAabb(),
      aabbTransform = makeSpatialTransform(),
    }, SpatialEntityMt)
  end
})
_G.SpatialEntity = SpatialEntity

-- PhysicsEntity ─────────────────────────────────────────────────────────────
-- Каст солдата/техники к физическому интерфейсу (нужен BotMovement:
-- PhysicsEntity(soldier).velocity для детекта падения/парашюта).
-- Сборка 90: скорость — та, что мост посчитал по позиции солдата из движка за тик (VextPlayerManager:
-- bridgeVelocity). Раньше здесь всегда был ноль: мод считал каждого бота стоящим и убивал его («got stuck»).
-- Вертикальной скорости из физкомпонента всё ещё нет, падение с высоты этим не покрыто.
local PhysicsEntityMt = { __index = function(self, k)
  -- Сборка 178: техника (EntityManager) — скорость и здоровье даёт сам объект машины (живые поля).
  local ent = rawget(self, '_entity')
  if type(ent) == 'table' and rawget(ent, '_kind') == 'ServerVehicleEntity' and (k == 'velocity' or k == 'internalHealth') then
    return ent[k]
  end
  if k == 'velocity' then
    local e = self._entity
    local v = nil
    if type(e) == 'table' then
      v = rawget(e, 'bridgeVelocity')
      -- Сборка 134: солдат без измеренной скорости (до старта раунда позиции недоступны) — «идёт», не «стоит».
      -- Иначе мод запускает обход препятствий для всех ботов (см. SoldierEntity.__index). EVBOTS_VEL_STUB=0 — выкл.
      if v == nil and rawget(e, 'player') ~= nil and EVBOTS_VEL_STUB ~= false then
        return Vec3._make(1.0, 0, 0)
      end
    end
    return v or Vec3._make(0, 0, 0)
  end
  return nil
end }
_G.PhysicsEntity = setmetatable({}, {
  __call = function(_, entity)
    return setmetatable({ _entity = entity }, PhysicsEntityMt)
  end
})

-- AxisAlignedBox ──────────────────────────────────────────────────────────────
local AxisAlignedBox = setmetatable({}, {
  __call = function(_, min, max)
    return setmetatable({
      min = min or Vec3._make(0, 0, 0),
      max = max or Vec3._make(0, 0, 0),
    }, AabbMt)
  end
})
function AxisAlignedBox:Clone()
  return AxisAlignedBox(self.min, self.max)
end
function AxisAlignedBox.__eq(a, b)
  return a.min == b.min and a.max == b.max
end
_G.AxisAlignedBox = AxisAlignedBox

)LUA";

}  // namespace

bool RegisterVextSpatial(lua_State* L) {
    if (luaL_loadbufferx(L, kSpatialSource,
                         sizeof(kSpatialSource) - 1,
                         "=[vext-spatial]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: SpatialEntity не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: SpatialEntity не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: SpatialEntity/AxisAlignedBox подключены "
             "(stub, Vec3.__len добавлен)");
    return true;
}

}  // namespace evbots
