// MiddleClass.cpp — встроенная система классов VeniceEXT.
//
// Основа — middleclass (MIT, kikito); VU fork использует __init и публикует
// каждый класс в _G[name]. Последнее сверено по готовому re/vu/veniceext.strings.txt,
// Lua по адресу 0x177C1F48: class.class(e,m) присваивает _G[e]=x перед return.
//
// Исходник встроен в DLL строкой, чтобы мод-система была автономной: на
// диске рядом с сервером никаких .lua лежать не должно.

#include "MiddleClass.h"

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

// Адаптация middleclass v4.1.1: initialize -> __init + алиас initialize.
constexpr char kMiddleClassSource[] = R"LUA(
local middleclass = {
  _VERSION     = 'middleclass v4.1.1 (EVBots)',
  _DESCRIPTION = 'Object Orientation for Lua',
  _URL         = 'https://github.com/kikito/middleclass',
  _LICENSE     = [[
    MIT LICENSE

    Copyright (c) 2011 Enrique Garcia Cota

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
  ]]
}

local function _createIndexWrapper(aClass, f)
  if f == nil then
    return aClass.__instanceDict
  elseif type(f) == "function" then
    return function(self, name)
      local value = aClass.__instanceDict[name]
      if value ~= nil then
        return value
      else
        return (f(self, name))
      end
    end
  else
    return function(self, name)
      local value = aClass.__instanceDict[name]
      if value ~= nil then
        return value
      else
        return f[name]
      end
    end
  end
end

local function _propagateInstanceMethod(aClass, name, f)
  f = name == "__index" and _createIndexWrapper(aClass, f) or f
  aClass.__instanceDict[name] = f
  for subclass in pairs(aClass.subclasses) do
    if rawget(subclass.__declaredMethods, name) == nil then
      _propagateInstanceMethod(subclass, name, f)
    end
  end
end

local function _declareInstanceMethod(aClass, name, f)
  aClass.__declaredMethods[name] = f
  if f == nil and aClass.super then
    f = aClass.super.__instanceDict[name]
  end
  _propagateInstanceMethod(aClass, name, f)
end

local function _tostring(self) return "class " .. self.name end
local function _call(self, ...) return self:new(...) end

local function _createClass(name, super)
  local dict = {}
  dict.__index = dict

  local aClass = { name = name, super = super, static = {},
                   __instanceDict = dict, __declaredMethods = {},
                   subclasses = setmetatable({}, {__mode='k'})  }

  if super then
    setmetatable(aClass.static, {
      __index = function(_,k)
        local result = rawget(dict,k)
        if result == nil then
          return super.static[k]
        end
        return result
      end
    })
  else
    setmetatable(aClass.static, { __index = function(_,k) return rawget(dict,k) end })
  end

  setmetatable(aClass, { __index = aClass.static, __tostring = _tostring,
                         __call = _call, __newindex = _declareInstanceMethod })
  return aClass
end

local function _includeMixin(aClass, mixin)
  assert(type(mixin) == 'table', "mixin must be a table")

  for name,method in pairs(mixin) do
    if name ~= "included" and name ~= "static" then aClass[name] = method end
  end

  for name,method in pairs(mixin.static or {}) do
    aClass.static[name] = method
  end

  if type(mixin.included)=="function" then mixin:included(aClass) end
  return aClass
end

local DefaultMixin = {
  __tostring   = function(self) return "instance of " .. tostring(self.class) end,

  -- VU: конструктор называется __init
  __init       = function(self, ...) end,

  -- алиас для кода, написанного под оригинальный middleclass
  initialize   = function(self, ...) return self:__init(...) end,

  isInstanceOf = function(self, aClass)
    return type(aClass) == 'table'
       and type(self) == 'table'
       and (self.class == aClass
            or type(self.class) == 'table'
            and type(self.class.isSubclassOf) == 'function'
            and self.class:isSubclassOf(aClass))
  end,

  static = {
    allocate = function(self)
      assert(type(self) == 'table',
             "Make sure that you are using 'Class:allocate'")
      return setmetatable({ class = self }, self.__instanceDict)
    end,

    new = function(self, ...)
      assert(type(self) == 'table',
             "Make sure that you are using 'Class:new' instead of 'Class.new'")
      local instance = self:allocate()
      instance:__init(...)
      return instance
    end,

    subclass = function(self, name)
      assert(type(self) == 'table',
             "Make sure that you are using 'Class:subclass'")
      assert(type(name) == "string", "You must provide a name(string)")
      local subclass = _createClass(name, self)

      for methodName, f in pairs(self.__instanceDict) do
        if not (methodName == "__index" and type(f) == "table") then
          _propagateInstanceMethod(subclass, methodName, f)
        end
      end
      subclass.__init = function(instance, ...)
        return self.__init(instance, ...)
      end

      self.subclasses[subclass] = true
      self:subclassed(subclass)
      return subclass
    end,

    subclassed = function(self, other) end,

    isSubclassOf = function(self, other)
      return type(other)      == 'table' and
             type(self.super) == 'table' and
             ( self.super == other or self.super:isSubclassOf(other) )
    end,

    include = function(self, ...)
      assert(type(self) == 'table',
             "Make sure you that you are using 'Class:include'")
      for _,mixin in ipairs({...}) do _includeMixin(self, mixin) end
      return self
    end
  }
}

function middleclass.class(name, super)
  assert(type(name) == 'string', "A name (string) is needed for the new class")
  local created = super and super:subclass(name)
      or _includeMixin(_createClass(name), DefaultMixin)
  _G[name] = created
  return created
end

setmetatable(middleclass, {
  __call = function(_, ...) return middleclass.class(...) end
})

return middleclass
)LUA";

}  // namespace

bool RegisterMiddleClass(lua_State* L) {
    static const int kSourceLen = static_cast<int>(
        sizeof(kMiddleClassSource) - 1);
    if (luaL_loadbufferx(L, kMiddleClassSource, kSourceLen,
                         "=[middleclass]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LogPrintf("ERR ", "EVBots: middleclass не собрался: %s",
                  err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LogPrintf("ERR ", "EVBots: middleclass не выполнился: %s",
                  err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }

    // глобальный class('Name') — как в VU
    // Обе ссылки получают результат чанка: setglobal снимает его со стека.
    lua_pushvalue(L, -1);
    lua_setglobal(L, "class");
    // и сам модуль под именем middleclass
    lua_setglobal(L, "middleclass");
    return true;
}

}  // namespace evbots
