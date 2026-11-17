// VextGeometry.cpp — типы-конструкторы VeniceEXT (VU) для EVBots.
//
// Реализованы глобальные типы Vec2/Vec3/Vec4, LinearTransform, Quat,
// QuatTransform и Guid — строго по документации VU (НЕ по памяти):
//   content\vext\ref\shared\type\{vec2,vec3,vec4,lineartransform,quat,
//   quattransform,guid}.md
// Логика — на Lua, встроенной в DLL строкой (подход тот же, что в
// MiddleClass.cpp / VextUtils.cpp).

#include "VextGeometry.h"

#include <string>

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// ── часть 1: Vec2 / Vec3 / Vec4 ────────────────────────────────────────────
// Типы реализованы таблицами с метатаблицами: поля x/y/z/w хранятся в самой
// таблице, методы и операторы — в метатаблице, read-only свойства
// (magnitude) — через __index-функцию. Конструктор — вызываемая таблица
// (метаметод __call), поэтому работают и Vec3(1,2,3), и Vec3.zero/up/...
constexpr char kGeoVecSource[] = R"LUA(
-- Векторные типы VeniceEXT (VU). Методы/операторы/статики — по докам
-- content\vext\ref\shared\type\{vec2,vec3,vec4}.md

-- ── общие помощники ──────────────────────────────────────────────────────
local function isNumber(v) return type(v) == "number" end

-- наибольший минимум в [0, 2^53], чтобы не сломать длину строк литерала
local sqrt, abs, min, max = math.sqrt, math.abs, math.min, math.max

-- Vec2 ─────────────────────────────────────────────────────────────────────
local Vec2Methods = {}
local Vec2Mt = { __index = function(self, k)
  if k == "magnitude" then
    return sqrt(self.x * self.x + self.y * self.y)
  end
  return Vec2Methods[k]
end }

function Vec2Methods.__add(a, b) return Vec2(a.x + b.x, a.y + b.y) end
function Vec2Methods.__sub(a, b) return Vec2(a.x - b.x, a.y - b.y) end
function Vec2Methods.__mul(a, b)
  if isNumber(b) then return Vec2(a.x * b, a.y * b) end
  return Vec2(a.x * b.x, a.y * b.y)          -- component-wise (как в движке)
end
function Vec2Methods.__div(a, b)
  if isNumber(b) then return Vec2(a.x / b, a.y / b) end
  return Vec2(a.x / b.x, a.y / b.y)
end
function Vec2Methods.__eq(a, b)
  return a.x == b.x and a.y == b.y
end
function Vec2Methods.__lt(a, b)               -- лексикографически
  if a.x ~= b.x then return a.x < b.x end
  return a.y < b.y
end
function Vec2Methods.__tostring(a)
  return "Vec2(" .. a.x .. ", " .. a.y .. ")"
end
function Vec2Methods:Distance(to)
  local dx, dy = self.x - to.x, self.y - to.y
  return sqrt(dx * dx + dy * dy)
end
function Vec2Methods:Clone()
  return Vec2(self.x, self.y)
end
-- Метаметоды операторов Lua достаёт напрямую из метатаблицы (не через __index):
do
  for k, v in pairs(Vec2Methods) do
    if k:sub(1, 2) == "__" then Vec2Mt[k] = v end
  end
end

local function makeVec2(x, y) return setmetatable({ x = x, y = y }, Vec2Mt) end
local Vec2 = setmetatable({}, { __call = function(_, ...)
  local n = select("#", ...)
  if n == 1 then
    local d = ...
    if type(d) == "table" then
      if d.x then return makeVec2(d.x, d.y or 0) end
      return makeVec2(d[1] or 0, d[2] or 0)
    end
    error("Vec2: ожидается таблица или (x, y)")
  end
  return makeVec2(select(1, ...) or 0, select(2, ...) or 0)
end })
function Vec2._make(x, y) return makeVec2(x, y) end
Vec2.zero = makeVec2(0, 0)
Vec2.one = makeVec2(1, 1)
Vec2.down = makeVec2(0, -1)
Vec2.left = makeVec2(-1, 0)
Vec2.right = makeVec2(1, 0)
Vec2.up = makeVec2(0, 1)
Vec2.negativeInfinity = makeVec2(-math.huge, -math.huge)
Vec2.positiveInfinity = makeVec2(math.huge, math.huge)
_G.Vec2 = Vec2

-- Vec3 ─────────────────────────────────────────────────────────────────────
local Vec3Methods = {}
local Vec3Mt = { __index = function(self, k)
  if k == "magnitude" then
    return sqrt(self.x * self.x + self.y * self.y + self.z * self.z)
  end
  return Vec3Methods[k]
end }

function Vec3Methods.__add(a, b) return Vec3(a.x + b.x, a.y + b.y, a.z + b.z) end
function Vec3Methods.__sub(a, b) return Vec3(a.x - b.x, a.y - b.y, a.z - b.z) end
function Vec3Methods.__mul(a, b)
  if isNumber(b) then return Vec3(a.x * b, a.y * b, a.z * b) end
  return Vec3(a.x * b.x, a.y * b.y, a.z * b.z)
end
function Vec3Methods.__div(a, b)
  if isNumber(b) then return Vec3(a.x / b, a.y / b, a.z / b) end
  return Vec3(a.x / b.x, a.y / b.y, a.z / b.z)
end
function Vec3Methods.__eq(a, b)
  return a.x == b.x and a.y == b.y and a.z == b.z
end
function Vec3Methods.__lt(a, b)
  if a.x ~= b.x then return a.x < b.x end
  if a.y ~= b.y then return a.y < b.y end
  return a.z < b.z
end
function Vec3Methods.__tostring(a)
  return "Vec3(" .. a.x .. ", " .. a.y .. ", " .. a.z .. ")"
end
function Vec3Methods:Dot(with)
  return self.x * with.x + self.y * with.y + self.z * with.z
end
function Vec3Methods:Cross(with)
  return Vec3(self.y * with.z - self.z * with.y,
              self.z * with.x - self.x * with.z,
              self.x * with.y - self.y * with.x)
end
function Vec3Methods:Normalize()
  local m = sqrt(self.x * self.x + self.y * self.y + self.z * self.z)
  if m == 0 then return self end
  self.x, self.y, self.z = self.x / m, self.y / m, self.z / m
  return self
end
function Vec3Methods:Distance(to)
  local dx, dy, dz = self.x - to.x, self.y - to.y, self.z - to.z
  return sqrt(dx * dx + dy * dy + dz * dz)
end
function Vec3Methods:MoveTowards(to, maxDistance)
  local dx, dy, dz = to.x - self.x, to.y - self.y, to.z - self.z
  local dist = sqrt(dx * dx + dy * dy + dz * dz)
  if dist <= maxDistance or dist == 0 then
    return to:Clone()
  end
  local t = maxDistance / dist
  return Vec3(self.x + dx * t, self.y + dy * t, self.z + dz * t)
end
function Vec3Methods:Clone()
  return Vec3(self.x, self.y, self.z)
end
-- Метаметоды операторов Lua достаёт напрямую из метатаблицы (не через __index):
do
  for k, v in pairs(Vec3Methods) do
    if k:sub(1, 2) == "__" then Vec3Mt[k] = v end
  end
end

local function makeVec3(x, y, z)
  return setmetatable({ x = x, y = y, z = z, _vt = "Vec3" }, Vec3Mt)
end
local Vec3 = setmetatable({}, { __call = function(_, ...)
  local n = select("#", ...)
  if n == 1 then
    local d = ...
    if type(d) == "table" then
      if d.x then return makeVec3(d.x, d.y or 0, d.z or 0) end
      return makeVec3(d[1] or 0, d[2] or 0, d[3] or 0)
    end
    error("Vec3: ожидается таблица или (x, y, z)")
  end
  local x, y, z = ...  -- сборка 242: без трёх select
  return makeVec3(x or 0, y or 0, z or 0)
end })
function Vec3._make(x, y, z) return makeVec3(x, y, z) end
Vec3.zero = makeVec3(0, 0, 0)
Vec3.one = makeVec3(1, 1, 1)
Vec3.up = makeVec3(0, 1, 0)
Vec3.down = makeVec3(0, -1, 0)
Vec3.left = makeVec3(-1, 0, 0)
Vec3.right = makeVec3(1, 0, 0)
Vec3.forward = makeVec3(0, 0, 1)
Vec3.back = makeVec3(0, 0, -1)
Vec3.negativeInfinity = makeVec3(-math.huge, -math.huge, -math.huge)
Vec3.positiveInfinity = makeVec3(math.huge, math.huge, math.huge)
_G.Vec3 = Vec3

-- ★ Сборка 181: type() как в VU. У VU объекты классов (Vec3, LinearTransform…) — userdata, у нас — таблицы с меткой _vt.
-- Мод различает «одно смещение» и «список по оружию» через type(x) == "table" (Vehicles.lua:282 GetOffsets, :302, :323, :333):
-- одиночный Vec3 у нас считался списком → s_Offset[1] = nil → VehicleAiming.lua:68 падал каждый кадр у стрелка в технике
-- (прогон 180, 22:11: 954 ошибки), весь UpdateManager:Update мода срывался. "userdata" отдаём только коду мода (источник
-- чанка в ...\Mods\...): проверки моста (type(d) == "table" и d._vt) работают как прежде.
do
  local rawtype = type
  local getinfo = debug ~= nil and debug.getinfo or nil
  if getinfo ~= nil and rawget(_G, "EVBotsRawType") == nil then
    _G.EVBotsRawType = rawtype
    local modSrc = {}
    -- ★ Сборка 241: то же в C (EVBotsVuType, без debug.getinfo и таблицы на каждый вызов); Lua-вариант — запас
    _G.type = rawget(_G, "EVBotsVuType") or function(v)
      local t = rawtype(v)
      if t == "table" and rawget(v, "_vt") ~= nil then
        local info = getinfo(2, "S")
        local src = info ~= nil and info.source or ""
        local m = modSrc[src]
        if m == nil then
          m = src:find("[\\/][Mm]ods[\\/]") ~= nil
          modSrc[src] = m
        end
        if m then return "userdata" end
      end
      return t
    end
  end
end

-- Vec4 ─────────────────────────────────────────────────────────────────────
local Vec4Methods = {}
local Vec4Mt = { __index = function(self, k)
  if k == "magnitude" then
    return sqrt(self.x * self.x + self.y * self.y + self.z * self.z + self.w * self.w)
  end
  return Vec4Methods[k]
end }

function Vec4Methods.__add(a, b)
  return Vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w)
end
function Vec4Methods.__sub(a, b)
  return Vec4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w)
end
function Vec4Methods.__mul(a, b)
  if isNumber(b) then
    return Vec4(a.x * b, a.y * b, a.z * b, a.w * b)
  end
  return Vec4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w)
end
function Vec4Methods.__div(a, b)
  if isNumber(b) then
    return Vec4(a.x / b, a.y / b, a.z / b, a.w / b)
  end
  return Vec4(a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w)
end
function Vec4Methods.__eq(a, b)
  return a.x == b.x and a.y == b.y and a.z == b.z and a.w == b.w
end
function Vec4Methods.__lt(a, b)
  if a.x ~= b.x then return a.x < b.x end
  if a.y ~= b.y then return a.y < b.y end
  if a.z ~= b.z then return a.z < b.z end
  return a.w < b.w
end
function Vec4Methods.__tostring(a)
  return "Vec4(" .. a.x .. ", " .. a.y .. ", " .. a.z .. ", " .. a.w .. ")"
end
function Vec4Methods:Distance(to)
  local dx, dy, dz, dw = self.x - to.x, self.y - to.y, self.z - to.z, self.w - to.w
  return sqrt(dx * dx + dy * dy + dz * dz + dw * dw)
end
function Vec4Methods:Clone()
  return Vec4(self.x, self.y, self.z, self.w)
end
-- Метаметоды операторов Lua достаёт напрямую из метатаблицы (не через __index):
do
  for k, v in pairs(Vec4Methods) do
    if k:sub(1, 2) == "__" then Vec4Mt[k] = v end
  end
end

local function makeVec4(x, y, z, w) return setmetatable({ x = x, y = y, z = z, w = w }, Vec4Mt) end
local Vec4 = setmetatable({}, { __call = function(_, ...)
  local n = select("#", ...)
  if n == 1 then
    local d = ...
    if type(d) == "table" then
      if d.x then return makeVec4(d.x, d.y or 0, d.z or 0, d.w or 0) end
      return makeVec4(d[1] or 0, d[2] or 0, d[3] or 0, d[4] or 0)
    end
    error("Vec4: ожидается таблица или (x, y, z, w)")
  end
  return makeVec4(select(1, ...) or 0, select(2, ...) or 0,
                  select(3, ...) or 0, select(4, ...) or 0)
end })
function Vec4._make(x, y, z, w) return makeVec4(x, y, z, w) end
Vec4.zero = makeVec4(0, 0, 0, 0)
Vec4.one = makeVec4(1, 1, 1, 1)
Vec4.negativeInfinity = makeVec4(-math.huge, -math.huge, -math.huge, -math.huge)
Vec4.positiveInfinity = makeVec4(math.huge, math.huge, math.huge, math.huge)
_G.Vec4 = Vec4
)LUA";

constexpr char kGeoQuatSource[] = R"LUA(
-- Quat / QuatTransform / LinearTransform по докам
-- content\vext\ref\shared\type\{quat,quattransform,lineartransform}.md

-- Quat ────────────────────────────────────────────────────────────────────
local QuatMethods = {}
local QuatMt = { __index = function(self, k)
  if k == "length" then return sqrt(self.x*self.x+self.y*self.y+self.z*self.z+self.w*self.w) end
  if k == "lengthSquared" then return self.x*self.x+self.y*self.y+self.z*self.z+self.w*self.w end
  return QuatMethods[k]
end }

-- умножение кватернионов (композиция поворотов)
function QuatMethods.__mul(a, b)
  return Quat(
    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z)
end
function QuatMethods.__eq(a, b)
  return a.x == b.x and a.y == b.y and a.z == b.z and a.w == b.w
end
function QuatMethods.__tostring(a)
  return "Quat(" .. a.x .. ", " .. a.y .. ", " .. a.z .. ", " .. a.w .. ")"
end
-- из векторов forward/up
function QuatMethods:Normalize()
  local m = sqrt(self.x*self.x + self.y*self.y + self.z*self.z + self.w*self.w)
  if m == 0 then return self end
  self.x, self.y, self.z, self.w = self.x/m, self.y/m, self.z/m, self.w/m
  return self
end
function QuatMethods:Dot(other)
  return self.x*other.x + self.y*other.y + self.z*other.z + self.w*other.w
end
function QuatMethods:Angle(other)  -- в радианах
  local dot = self:Dot(other)
  if dot > 1 then dot = 1 elseif dot < -1 then dot = -1 end
  return 2 * math.acos(math.abs(dot))
end
function QuatMethods:RotateTowards(to, maxDegreesDelta)
  local angle = self:Angle(to)
  if angle == 0 then return self:Clone() end
  local t = maxDegreesDelta / angle
  if t > 1 then t = 1 end
  return self:SlerpUnclamped(to, t)
end
function QuatMethods:Slerp(to, t)
  if t < 0 then t = 0 elseif t > 1 then t = 1 end
  return self:SlerpUnclamped(to, t)
end
function QuatMethods:SlerpUnclamped(to, t)
  local dot = self:Dot(to)
  if dot < 0 then dot = -dot; to = Quat(-to.x, -to.y, -to.z, -to.w) end
  if dot > 0.9995 then  -- почти совпадают — линейная интерполяция
    local r = Quat(self.x + (to.x - self.x)*t,
                   self.y + (to.y - self.y)*t,
                   self.z + (to.z - self.z)*t,
                   self.w + (to.w - self.w)*t)
    return r:Normalize()
  end
  local theta = math.acos(dot)
  local sinTheta = math.sin(theta)
  local wa = math.sin((1 - t) * theta) / sinTheta
  local wb = math.sin(t * theta) / sinTheta
  return Quat(self.x*wa + to.x*wb,
              self.y*wa + to.y*wb,
              self.z*wa + to.z*wb,
              self.w*wa + to.w*wb)
end
function QuatMethods:Clone()
  return Quat(self.x, self.y, self.z, self.w)
end
-- Метаметоды операторов Lua достаёт напрямую из метатаблицы (не через __index):
do
  for k, v in pairs(QuatMethods) do
    if k:sub(1, 2) == "__" then QuatMt[k] = v end
  end
end

local function makeQuat(x, y, z, w) return setmetatable({ x = x, y = y, z = z, w = w }, QuatMt) end

-- из эйлеровых углов (Vec3 yaw,pitch,roll в радианах), порядок применения y→p→r
local function quatFromEuler(e)
  local cy, sy = math.cos(e.x * 0.5), math.sin(e.x * 0.5)
  local cp, sp = math.cos(e.y * 0.5), math.sin(e.y * 0.5)
  local cr, sr = math.cos(e.z * 0.5), math.sin(e.z * 0.5)
  return makeQuat(
    sr*cp*cy - cr*sp*sy,
    cr*sp*cy + sr*cp*sy,
    cr*cp*sy - sr*sp*cy,
    cr*cp*cy + sr*sp*sy)
end

-- из пары forward/up: строится базис, затем кватернион
local function quatFromFwd(f, u)
  local forward = f:Clone():Normalize()
  local up = u:Clone():Normalize()
  local right = Vec3._make(0,0,0):Cross(forward)
  right = Vec3._make(up.y*forward.z - up.z*forward.y,
                   up.z*forward.x - up.x*forward.z,
                   up.x*forward.y - up.y*forward.x)
  if right.magnitude < 1e-6 then
    -- коллинеарны — берём произвольную ось
    right = Vec3._make(1, 0, 0):Cross(forward)
    if right.magnitude < 1e-6 then right = Vec3._make(0, 1, 0):Cross(forward) end
  end
  right:Normalize()
  local trueUp = Vec3._make(forward.y*right.z - forward.z*right.y,
                            forward.z*right.x - forward.x*right.z,
                            forward.x*right.y - forward.y*right.x)
  -- матрица R = [right | up | forward] (столбцы); кватернион из ортонормального базиса
  local m00, m01, m02 = right.x, trueUp.x, forward.x
  local m10, m11, m12 = right.y, trueUp.y, forward.y
  local m20, m21, m22 = right.z, trueUp.z, forward.z
  local tr = m00 + m11 + m22
  local q
  if tr > 0 then
    local s = math.sqrt(tr + 1) * 2
    q = makeQuat((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s)
  elseif m00 > m11 and m00 > m22 then
    local s = math.sqrt(1 + m00 - m11 - m22) * 2
    q = makeQuat(0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s)
  elseif m11 > m22 then
    local s = math.sqrt(1 + m11 - m00 - m22) * 2
    q = makeQuat((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s)
  else
    local s = math.sqrt(1 + m22 - m00 - m11) * 2
    q = makeQuat((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s)
  end
  return q:Normalize()
end

local Quat = setmetatable({}, { __call = function(_, ...)
  local n = select("#", ...)
  if n == 1 then
    local d = ...
    if type(d) == "table" then
      if d._vt == "Vec3" then
        -- Vec3 из эйлеровых углов
        return quatFromEuler(d)
      end
      -- таблица data (конструктор): поля x/y/z/w или числовой массив
      if d.x ~= nil then return makeQuat(d.x, d.y or 0, d.z or 0, d.w or 1) end
      return makeQuat(d[1] or 0, d[2] or 0, d[3] or 0, d[4] or 1)
    end
    error("Quat: ожидается (x,y,z,w), Vec3 или таблица")
  elseif n == 2 then
    local a, b = ...
    if type(a) == "table" and type(b) == "table" then
      return quatFromFwd(a, b)
    end
  end
  return makeQuat(select(1, ...) or 0, select(2, ...) or 0,
                  select(3, ...) or 0, select(4, ...) or 1)
end })
_G.Quat = Quat

-- QuatTransform ────────────────────────────────────────────────────────────
local QtMethods = {}
-- ToLinearTransform: rotation + transAndScale(w = масштаб) → LinearTransform.
-- Восстанавливаем три ортогональных столбца (left/up/forward) из кватерниона.
-- ★ Сборка 179: столбцы матрицы поворота (как их собирает quatFromFwd: R = [left | up | forward]). До 179 здесь были СТРОКИ
-- (транспонированный базис): для машины с курсом ψ transform:ToQuatTransform():ToEuler().x давал −ψ — мод крутил руль не
-- в ту сторону, машины вставали задом к цели (прогон 179: ошибка курса держалась ≈ ±3,1 рад, «всё время налево»).
local function quatToBasis(q)
  local x, y, z, w = q.x, q.y, q.z, q.w
  local l = Vec3._make(1 - 2*(y*y + z*z), 2*(x*y + z*w), 2*(x*z - y*w))
  local u = Vec3._make(2*(x*y - z*w), 1 - 2*(x*x + z*z), 2*(y*z + x*w))
  local f = Vec3._make(2*(x*z + y*w), 2*(y*z - x*w), 1 - 2*(x*x + y*y))
  return l, u, f
end
-- ★ Сборка 178 (техника): Quat:ToEuler как в VU — x = курс (вокруг Y), y = крен (вокруг оси носа X = left), z = тангаж
-- (вокруг Z); порядок R = Ry(x) * Rx(y) * Rz(z), столбцы R — left/up/forward. Сверка по моду fun-bots (VehicleMovement.lua:
-- «Yaw = -Euler.x», «Roll = Euler.y», «Pitch = -Euler.z / cos(Roll)», курс цели atan(z, x) от оси left). Раньше была
-- формула Z-вверх (yaw вокруг Z), курс машины получался неверным — боты ехали прямо (прогон 178).
-- ★ Сборка 183: нос = forward (как в движке), поэтому наклон ствола — это подъём forward (f.y), а крен — поворот вокруг forward.
-- До 183 .y был −подъём, .z — крен: мод (VehicleMovement.lua:529: Pitch = −Euler.z / cos(Euler.y)) видел ствол всегда
-- горизонтальным и гнал EIAPitch в упор — стрелок бил в небо/землю (прогон 182: «стреляет в тупую»). Теперь
-- −z / cos(y) = asin(f.y) = подъём носа (цель мода: atan(dy, расстояние), VehicleAiming.lua:126 — вверх положительно).
-- ★ Сборка 185 (ночной прогон MP_007, 12:38): знак крена сверен по вертолётам мода. С atan(−l.y, u.y) мод давал EIARoll +0,72
-- на ровном вертолёте, крен уходил в минус (−0,44 → −1,50 → −2,40), мод добавлял ещё +1 — вертолёт переворачивался
-- (VehicleChopperControl.lua:175: ΔRoll = цель − крен → EIARoll). Для мода положительный EIARoll должен увеличивать крен —
-- знак y обратный: atan(l.y, u.y). На земле и у самолётов мод берёт только cos(y) или не берёт ToEuler — их это не меняет.
function QuatMethods:ToEuler()
  local l, u, f = quatToBasis(self)
  local yaw = math.atan(f.x, f.z)
  local roll = math.atan(l.y, u.y)
  local elev = math.asin(max(-1, min(1, f.y)))
  return Vec3._make(yaw, roll, -elev * math.cos(roll))
end
function QtMethods:ToLinearTransform()
  local l, u, f = quatToBasis(self.rotation)
  local s = self.transAndScale.w
  return LinearTransform(l * s, u * s, f * s, Vec3._make(
    self.transAndScale.x, self.transAndScale.y, self.transAndScale.z))
end
function QtMethods:Clone()
  return QuatTransform(self.rotation:Clone(), self.transAndScale:Clone())
end
function QtMethods.__eq(a, b)
  return a.rotation == b.rotation and a.transAndScale == b.transAndScale
end
function QtMethods.__tostring(a)
  return "QuatTransform(rotation=" .. tostring(a.rotation) .. ", transAndScale=" ..
         tostring(a.transAndScale) .. ")"
end

local QuatTransformMt = { __index = QtMethods }
-- Метаметоды операторов Lua достаёт напрямую из метатаблицы (не через __index):
do
  for k, v in pairs(QtMethods) do
    if k:sub(1, 2) == "__" then QuatTransformMt[k] = v end
  end
end
local function makeQuatTransform(r, ts)
  return setmetatable({ rotation = r, transAndScale = ts }, QuatTransformMt)
end
local QuatTransform = setmetatable({}, { __call = function(_, ...)
  local n = select("#", ...)
  if n == 0 then
    return makeQuatTransform(Quat(0, 0, 0, 1), Vec4(0, 0, 0, 1))
  elseif n == 1 then
    local d = ...
    if type(d) == "table" then
      return makeQuatTransform(d.rotation or Quat(0,0,0,1), d.transAndScale or Vec4(0,0,0,1))
    end
    error("QuatTransform: ожидается (rotation, transAndScale) или таблица")
  end
  return makeQuatTransform(select(1, ...), select(2, ...))
end })
_G.QuatTransform = QuatTransform
)LUA";

constexpr char kGeoLTGuidSource[] = R"LUA(
-- LinearTransform и Guid по докам
-- content\vext\ref\shared\type\{lineartransform,guid}.md

-- LinearTransform ──────────────────────────────────────────────────────────
local LTMethods = {}
local LTMt = { __index = LTMethods }

-- Конструктор LinTransform(scale: float) — единичная матрица с масштабом
local function makeScaleLT(s)
  return setmetatable({
    left = Vec3._make(s, 0, 0),
    up = Vec3._make(0, s, 0),
    forward = Vec3._make(0, 0, s),
    trans = Vec3._make(0, 0, 0),
  }, LTMt)
end

local function makeLT(l, u, f, t)
  return setmetatable({ left = l, up = u, forward = f, trans = t, _vt = "LT" }, LTMt)
end

-- применение преобразования к точке (без w)
local function applyToVec3(a, v)
  return Vec3._make(
    a.left.x*v.x + a.up.x*v.y + a.forward.x*v.z + a.trans.x,
    a.left.y*v.x + a.up.y*v.y + a.forward.y*v.z + a.trans.y,
    a.left.z*v.x + a.up.z*v.y + a.forward.z*v.z + a.trans.z)
end
-- применение к вектору (линейная часть)
local function applyToDir(a, v)
  return Vec3._make(
    a.left.x*v.x + a.up.x*v.y + a.forward.x*v.z,
    a.left.y*v.x + a.up.y*v.y + a.forward.y*v.z,
    a.left.z*v.x + a.up.z*v.y + a.forward.z*v.z)
end

-- умножение: линейная часть = A ∘ B (столбец = базисный вектор)
local function mulMatrix(a, b)
  local n_left = Vec3._make(
    a.left.x*b.left.x + a.up.x*b.left.y + a.forward.x*b.left.z,
    a.left.y*b.left.x + a.up.y*b.left.y + a.forward.y*b.left.z,
    a.left.z*b.left.x + a.up.z*b.left.y + a.forward.z*b.left.z)
  local n_up = Vec3._make(
    a.left.x*b.up.x + a.up.x*b.up.y + a.forward.x*b.up.z,
    a.left.y*b.up.x + a.up.y*b.up.y + a.forward.y*b.up.z,
    a.left.z*b.up.x + a.up.z*b.up.y + a.forward.z*b.up.z)
  local n_forward = Vec3._make(
    a.left.x*b.forward.x + a.up.x*b.forward.y + a.forward.x*b.forward.z,
    a.left.y*b.forward.x + a.up.y*b.forward.y + a.forward.y*b.forward.z,
    a.left.z*b.forward.x + a.up.z*b.forward.y + a.forward.z*b.forward.z)
  local n_trans = Vec3._make(
    a.left.x*b.trans.x + a.up.x*b.trans.y + a.forward.x*b.trans.z + a.trans.x,
    a.left.y*b.trans.x + a.up.y*b.trans.y + a.forward.y*b.trans.z + a.trans.y,
    a.left.z*b.trans.x + a.up.z*b.trans.y + a.forward.z*b.trans.z + a.trans.z)
  return makeLT(n_left, n_up, n_forward, n_trans)
end

function LTMethods.__add(a, b)
  return makeLT(a.left + b.left, a.up + b.up, a.forward + b.forward, a.trans + b.trans)
end
function LTMethods.__sub(a, b)
  return makeLT(a.left - b.left, a.up - b.up, a.forward - b.forward, a.trans - b.trans)
end
function LTMethods.__mul(a, b)
  if type(b) == "table" then
    if b._vt == "Vec4" then
      -- превращаем Vec4 → Vec3 точку (w в VU не используется для точки)
      return applyToVec3(a, Vec3._make(b.x, b.y, b.z))
    end
    if b._vt == "Vec3" then
      return applyToVec3(a, b)
    end
    -- LinearTransform * LinearTransform
    return mulMatrix(a, b)
  end
  if isNumber(b) then
    return makeLT(a.left * b, a.up * b, a.forward * b, a.trans * b)
  end
  error("LinearTransform: операция * с недопустимым типом")
end
function LTMethods.__eq(a, b)
  return a.left == b.left and a.up == b.up and a.forward == b.forward and a.trans == b.trans
end
function LTMethods.__tostring(a)
  return "LinearTransform(" .. tostring(a.left) .. ", " .. tostring(a.up) ..
         ", " .. tostring(a.forward) .. ", " .. tostring(a.trans) .. ")"
end

-- LookAtTransform(camera, world): строит базис, смотрящий из camera на world
function LTMethods:LookAtTransform(camera, world)
  local up = Vec3._make(0, 1, 0)
  local fwd = (world - camera):Clone():Normalize()
  local left = up:Cross(fwd)
  if left.magnitude < 1e-6 then
    left = Vec3._make(0, 0, 1):Cross(fwd)
    if left.magnitude < 1e-6 then left = Vec3._make(1, 0, 0):Cross(fwd) end
  end
  left:Normalize()
  local trueUp = fwd:Cross(left)
  self.left = left
  self.up = trueUp
  self.forward = fwd
  self.trans = camera:Clone()
end
-- ToQuatTransform(hasScale): базис+транс → QuatTransform
function LTMethods:ToQuatTransform(hasScale)
  local scale = 1
  if hasScale then
    scale = (self.left.magnitude + self.up.magnitude + self.forward.magnitude) / 3
    if scale == 0 then scale = 1 end
  end
  local q = quatFromFwd(self.forward, self.up)
  return QuatTransform(q, Vec4._make(self.trans.x, self.trans.y, self.trans.z, scale))
end
-- Translate(trans: Vec3): сдвигает транс (left/up/forward не меняются)
function LTMethods:Translate(trans)
  self.trans = self.trans + trans
end
-- Inverse(): обратный LinearTransform (для ортонормированного базиса)
function LTMethods:Inverse()
  -- инверсия поворота = транспонирование базиса, сдвиг пересчитывается
  local t = self.trans
  local n_trans = Vec3._make(
    -(self.left.x*t.x + self.up.x*t.y + self.forward.x*t.z),
    -(self.left.y*t.x + self.up.y*t.y + self.forward.y*t.z),
    -(self.left.z*t.x + self.up.z*t.y + self.forward.z*t.z))
  return makeLT(
    Vec3._make(self.left.x, self.up.x, self.forward.x),
    Vec3._make(self.left.y, self.up.y, self.forward.y),
    Vec3._make(self.left.z, self.up.z, self.forward.z),
    n_trans)
end
function LTMethods:Clone()
  return makeLT(self.left:Clone(), self.up:Clone(),
                self.forward:Clone(), self.trans:Clone())
end
-- Метаметоды операторов Lua достаёт напрямую из метатаблицы (не через __index):
do
  for k, v in pairs(LTMethods) do
    if k:sub(1, 2) == "__" then LTMt[k] = v end
  end
end

local LinearTransform = setmetatable({}, { __call = function(_, ...)
  local n = select("#", ...)
  if n == 0 then
    -- единичный трансформ
    return makeLT(Vec3._make(1,0,0), Vec3._make(0,1,0), Vec3._make(0,0,1), Vec3._make(0,0,0))
  elseif n == 1 then
    local d = ...
    if isNumber(d) then
      -- масштаб
      return makeLT(Vec3._make(d,0,0), Vec3._make(0,d,0), Vec3._make(0,0,d), Vec3._make(0,0,0))
    elseif type(d) == "table" then
      -- таблица data: {left=...,up=...,forward=...,trans=...} или массив [left,up,forward,trans]
      if d.left ~= nil then
        return makeLT(d.left, d.up, d.forward, d.trans)
      end
      return makeLT(d[1], d[2], d[3], d[4])
    end
    error("LinearTransform: scale or table expected")
  end
  local l, u, f, t = ...  -- сборка 242: без четырёх select (горячая строка профиля 240)
  return makeLT(l, u, f, t)
end })
_G.LinearTransform = LinearTransform

-- Guid ─────────────────────────────────────────────────────────────────────
-- Храним sixteen байт; поля a..k (read-only, по доке) — байты GUID'a.
-- Конструктор: Guid(guid: string, format: string = 'D').
local GuidMt = { __index = function(self, k)
  if GuidMethods and GuidMethods[k] then return GuidMethods[k] end
  -- read-only поля a..k: байты представления (индексы 1..11 -> 0..10).
  -- k может быть НЕ строкой (мод лезет полем .x/.5) — сравниваем только строки,
  -- иначе `k >= "a"` валит "attempt to compare number with string" и роняет
  -- весь обработчик события (краш 11.10.2026, Level:Loaded).
  if type(k) == "string" and #k == 1 and k >= "a" and k <= "k" then
    local bi = string.byte(k) - string.byte("a")
    if self._b[bi + 1] then return self._b[bi + 1] end
  end
  return nil
end }
GuidMethods = {}
function GuidMethods:ToString(format)
  format = format or "D"
  local b = self._b
  local hex = {}
  for i = 0, 15 do hex[#hex + 1] = string.format("%02X", b[i + 1] or 0) end
  local h = table.concat(hex)
  if format == "N" then return h end
  local withDashes = string.sub(h, 1, 8) .. "-" .. string.sub(h, 9, 12) .. "-" ..
                     string.sub(h, 13, 16) .. "-" .. string.sub(h, 17, 20) .. "-" ..
                     string.sub(h, 21, 32)
  if format == "D" then return withDashes end
  if format == "B" then return "{" .. withDashes .. "}" end
  if format == "P" then return "(" .. withDashes .. ")" end
  return withDashes
end
function GuidMethods:Clone()
  return Guid(self:ToString())
end
function GuidMethods.__eq(a, b)
  for i = 1, 16 do
    if a._b[i] ~= b._b[i] then return false end
  end
  return true
end
function GuidMethods.__tostring(a)
  return a:ToString()
end
-- Метаметоды операторов Lua достаёт напрямую из метатаблицы (не через __index):
do
  for k, v in pairs(GuidMethods) do
    if k:sub(1, 2) == "__" then GuidMt[k] = v end
  end
end

-- парсим строку GUID (D/N/B/P) в 16 байт
local function parseGuidString(s, format)
  local clean = s
  if format == "B" then clean = s:sub(2, -2) end      -- {..}
  if format == "P" then clean = s:sub(2, -2) end      -- (..)
  if format ~= "N" then clean = clean:gsub("-", "") end
  local b = {}
  for i = 0, 15 do
    local byte = clean:sub(i*2 + 1, i*2 + 2)
    b[i + 1] = tonumber(byte, 16) or 0
  end
  return b
end

local Guid = setmetatable({}, { __call = function(_, g, fmt)
  if g == nil then g = "" end
  if type(g) ~= "string" then
    -- Пытаемся преобразовать — если не строка, выходим нулевым GUID
    g = "00000000-0000-0000-0000-000000000000"
  end
  if #g == 0 then
    g = "00000000-0000-0000-0000-000000000000"
  end
  return setmetatable({ _b = parseGuidString(g, fmt or "D") }, GuidMt)
end })
_G.Guid = Guid
)LUA";

const char* kParts[] = {
    kGeoVecSource,
    kGeoQuatSource,
    kGeoLTGuidSource,
};

}  // namespace

namespace {

// Источник чанка — код мода: путь содержит «\Mods\» или «/mods/» (как шаблон Lua "[\\/][Mm]ods[\\/]").
bool IsModSource(const char* s) {
    for (; *s != '\0'; ++s) {
        if ((s[0] == '\\' || s[0] == '/') && (s[1] == 'M' || s[1] == 'm') && s[2] == 'o' && s[3] == 'd' && s[4] == 's' &&
            (s[5] == '\\' || s[5] == '/'))
            return true;
    }
    return false;
}

// ★ Сборка 241: type() как в VU (сборка 181) — в C. Lua-обёртка на каждый вызов с объектом (_vt) звала debug.getinfo(2, "S"),
// а она создаёт таблицу: мод зовёт type() постоянно (Vec3, трансформы), Lua с 239 в кадре движка. Поведение то же:
// объект класса (таблица с _vt) — "userdata" только коду мода, остальным — как обычный type.
int l_vu_type(lua_State* L) {
    luaL_checkany(L, 1);
    const int t = lua_type(L, 1);
    if (t == LUA_TTABLE) {
        lua_pushliteral(L, "_vt");
        const bool tagged = lua_rawget(L, 1) != LUA_TNIL;
        lua_pop(L, 1);
        if (tagged) {
            lua_Debug ar;
            if (lua_getstack(L, 1, &ar) != 0 && lua_getinfo(L, "S", &ar) != 0 && ar.source != nullptr &&
                IsModSource(ar.source)) {
                lua_pushliteral(L, "userdata");
                return 1;
            }
        }
    }
    lua_pushstring(L, lua_typename(L, t));
    return 1;
}

}  // namespace

bool RegisterVextGeometry(lua_State* L) {
    lua_pushcfunction(L, l_vu_type);
    lua_setglobal(L, "EVBotsVuType");
    std::string src;
    for (const char* part : kParts) {
        src.append(part, std::char_traits<char>::length(part));
    }

    if (luaL_loadbufferx(L, src.data(), src.size(), "=[vext-geometry]", "t") !=
        LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: geometry failed to compile: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: геометрия не выполнилась: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("EVBots: типы-конструкторы VU подключены "
             "(Vec2/Vec3/Vec4/LinearTransform/QuatTransform/Quat/Guid)");
    return true;
}

}  // namespace evbots
