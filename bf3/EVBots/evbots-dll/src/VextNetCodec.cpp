// VextNetCodec.cpp — упаковка NetEvents для канала клиент↔сервер (К4, сборка 219). Общая для сервера и клиента.
//
// Какие данные можно слать — по VU-Docs (guides\netevents.md, «NetEvent data»): числа, строки, bool, nil, Vec2, Vec3,
// Vec4, LinearTransform, Guid, AxisAlignedBox, QuatTransform, Quat и таблицы из них (ключи и значения тех же типов).
// Остальное (функции, userdata, объекты движка) — ошибка, как у VU.
//
// Кадр (байты Lua-строки, string.pack, little-endian):
//   "EVN" B(версия=1) B(вид)
//   вид 1 — приветствие клиента: s2 имя игрока
//   вид 2 — событие: B флаги (1 Local, 2 Unreliable, 4 Ordered) s2 имя события B число аргументов, затем значения
// Значение: B метка, затем данные:
//   0 nil, 1 false, 2 true, 3 целое (j), 4 дробное (n), 5 строка (s4), 10 Vec2 (2n), 11 Vec3 (3n), 12 Vec4 (4n),
//   13 Quat (4n: x y z w), 14 LinearTransform (12n: left up forward trans), 15 QuatTransform (Quat + Vec4),
//   16 Guid (s1, строка D), 17 AxisAlignedBox (min Vec3, max Vec3), 20 таблица (I4 число пар, затем ключ, значение).

#include "VextNetCodec.h"

#include "EVBotsLog.h"
#include "VextResourceManager.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace evbots {
namespace {

constexpr char kCodecSource[] = R"LUA(
local nativeGeometry = ...
local C = {}
_G.EVNetCodec = C

local VERSION = 1
C.KIND_HELLO = 1
C.KIND_EVENT = 2
C.FLAG_LOCAL = 1
C.FLAG_UNRELIABLE = 2
C.FLAG_ORDERED = 4

local pack, unpack = string.pack, string.unpack
local MAX_DEPTH = 32

-- метатаблицы типов VU берём с образцов (конструкторы регистрируются раньше модов)
local mt
local function types()
  if mt ~= nil then return mt end
  mt = {}
  local function sample(name, ...)
    local ctor = rawget(_G, name)
    if ctor == nil then return end
    local ok, v = pcall(ctor, ...)
    if ok and type(v) == "table" then local m = getmetatable(v) if m ~= nil then mt[m] = name end end
  end
  sample("Vec2", 0, 0)
  sample("Vec3", 0, 0, 0)
  sample("Vec4", 0, 0, 0, 0)
  sample("Quat", 0, 0, 0, 1)
  sample("QuatTransform")
  sample("LinearTransform")
  sample("Guid", "")
  sample("AxisAlignedBox")
  return mt
end

local function vuType(v)
  local m = getmetatable(v)
  if m == nil then return nil end
  return types()[m]
end

local putValue
local function putVec3(out, v) out[#out + 1] = pack("<nnn", v.x, v.y, v.z) end

putValue = function(out, v, depth)
  local t = type(v)
  if v == nil then out[#out + 1] = "\0"
  elseif t == "boolean" then out[#out + 1] = v and "\2" or "\1"
  elseif t == "number" then
    if math.type(v) == "integer" then out[#out + 1] = pack("<Bj", 3, v) else out[#out + 1] = pack("<Bn", 4, v) end
  elseif t == "string" then out[#out + 1] = pack("<Bs4", 5, v)
  elseif t == "table" or t == "userdata" then
    local vt
    if t == "userdata" then vt = nativeGeometry(v) else vt = vuType(v) end
    if vt == "Vec2" then out[#out + 1] = pack("<Bnn", 10, v.x, v.y)
    elseif vt == "Vec3" then out[#out + 1] = "\11" putVec3(out, v)
    elseif vt == "Vec4" then out[#out + 1] = pack("<Bnnnn", 12, v.x, v.y, v.z, v.w)
    elseif vt == "Quat" then out[#out + 1] = pack("<Bnnnn", 13, v.x, v.y, v.z, v.w)
    elseif vt == "LinearTransform" then
      out[#out + 1] = "\14" putVec3(out, v.left) putVec3(out, v.up) putVec3(out, v.forward) putVec3(out, v.trans)
    elseif vt == "QuatTransform" then
      local r, s = v.rotation, v.transAndScale
      out[#out + 1] = pack("<Bnnnnnnnn", 15, r.x, r.y, r.z, r.w, s.x, s.y, s.z, s.w)
    elseif vt == "Guid" then out[#out + 1] = pack("<Bs1", 16, v:ToString())
    elseif vt == "AxisAlignedBox" then out[#out + 1] = "\17" putVec3(out, v.min) putVec3(out, v.max)
    elseif vt ~= nil then error("NetEvents: тип " .. vt .. " нельзя передать", 0)
    elseif t == "userdata" then error("NetEvents: native object is not a supported geometry value", 0)
    else
      if depth >= MAX_DEPTH then error("NetEvents: слишком глубокая таблица", 0) end
      local keys = {}
      for k in pairs(v) do keys[#keys + 1] = k end
      out[#out + 1] = pack("<BI4", 20, #keys)
      for i = 1, #keys do
        local k = keys[i]
        putValue(out, k, depth + 1)
        putValue(out, v[k], depth + 1)
      end
    end
  else
    error("NetEvents: значение типа " .. t .. " нельзя передать (VU: только числа, строки, bool, nil, типы VU и таблицы)", 0)
  end
end

local getValue
local function getVec3(s, p)
  local x, y, z, np = unpack("<nnn", s, p)
  return Vec3(x, y, z), np
end

getValue = function(s, p, depth)
  local tag
  tag, p = unpack("<B", s, p)
  if tag == 0 then return nil, p
  elseif tag == 1 then return false, p
  elseif tag == 2 then return true, p
  elseif tag == 3 then return unpack("<j", s, p)
  elseif tag == 4 then return unpack("<n", s, p)
  elseif tag == 5 then return unpack("<s4", s, p)
  elseif tag == 10 then local x, y, np = unpack("<nn", s, p) return Vec2(x, y), np
  elseif tag == 11 then return getVec3(s, p)
  elseif tag == 12 then local x, y, z, w, np = unpack("<nnnn", s, p) return Vec4(x, y, z, w), np
  elseif tag == 13 then local x, y, z, w, np = unpack("<nnnn", s, p) return Quat(x, y, z, w), np
  elseif tag == 14 then
    local l, u, f, t
    l, p = getVec3(s, p) u, p = getVec3(s, p) f, p = getVec3(s, p) t, p = getVec3(s, p)
    return LinearTransform(l, u, f, t), p
  elseif tag == 15 then
    local rx, ry, rz, rw, sx, sy, sz, sw, np = unpack("<nnnnnnnn", s, p)
    return QuatTransform(Quat(rx, ry, rz, rw), Vec4(sx, sy, sz, sw)), np
  elseif tag == 16 then local g, np = unpack("<s1", s, p) return Guid(g), np
  elseif tag == 17 then
    local a, b
    a, p = getVec3(s, p) b, p = getVec3(s, p)
    return AxisAlignedBox(a, b), p
  elseif tag == 20 then
    if depth >= MAX_DEPTH then error("NetEvents: слишком глубокая таблица", 0) end
    local n
    n, p = unpack("<I4", s, p)
    local t = {}
    for _ = 1, n do
      local k, v
      k, p = getValue(s, p, depth + 1)
      v, p = getValue(s, p, depth + 1)
      if k ~= nil then t[k] = v end
    end
    return t, p
  end
  error("NetEvents: неизвестная метка значения " .. tostring(tag), 0)
end

local function head(kind) return pack("<c3BB", "EVN", VERSION, kind) end

function C.EncodeHello(name)
  return head(C.KIND_HELLO) .. pack("<s2", tostring(name or ""))
end

-- (флаги, имя события, аргументы...) -> строка; ошибка — если аргумент нельзя передать
function C.EncodeEvent(flags, eventName, ...)
  local n = select("#", ...)
  if n > 255 then error("NetEvents: больше 255 аргументов", 0) end
  local out = { head(C.KIND_EVENT), pack("<Bs2B", flags or 0, eventName, n) }
  for i = 1, n do putValue(out, (select(i, ...)), 0) end
  return table.concat(out)
end

-- строка -> вид, затем: для приветствия — имя; для события — флаги, имя, число аргументов, таблица аргументов
function C.Decode(s)
  local magic, ver, kind, p = unpack("<c3BB", s, 1)
  if magic ~= "EVN" then error("NetEvents: чужой кадр", 0) end
  if ver ~= VERSION then error("NetEvents: версия кадра " .. tostring(ver) .. ", нужна " .. VERSION, 0) end
  if kind == C.KIND_HELLO then
    local name = unpack("<s2", s, p)
    return kind, name
  elseif kind == C.KIND_EVENT then
    local flags, name, n
    flags, name, n, p = unpack("<Bs2B", s, p)
    local args = {}
    for i = 1, n do args[i], p = getValue(s, p, 0) end
    return kind, flags, name, n, args
  end
  error("NetEvents: неизвестный вид кадра " .. tostring(kind), 0)
end

return C
)LUA";

int NativeGeometry(lua_State* L) {
    const char* name = ResourceGeometryType(L, 1);
    if (name) lua_pushstring(L, name); else lua_pushnil(L);
    return 1;
}

}  // namespace

bool RegisterNetCodec(lua_State* L) {
    int result = luaL_loadbufferx(L, kCodecSource, sizeof(kCodecSource) - 1, "=[vext-netcodec]", "t");
    if (result == LUA_OK) {
        lua_pushcfunction(L, NativeGeometry);
        result = lua_pcall(L, 1, 0, 0);
    }
    if (result != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: кодек NetEvents не загрузился: %s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

}  // namespace evbots
