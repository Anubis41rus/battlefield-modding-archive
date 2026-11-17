"""gen_fb_types.py — генератор src\\VextFbTypes.cpp: все классы fb из VU-Docs (content\\vext\\ref\\fb\\*.md с разделом
«### Constructors») как глобальные типы клиентского Lua — как у VU, где каждый fb-класс зарегистрирован заранее.

Запуск (из evbots-dll):  python -I tools\\gen_fb_types.py   -> перезаписывает src\\VextFbTypes.cpp
Сборка 215 (23.10.2026). Перечисления fb («## Values») сюда не входят — они в VextEnums.cpp.
"""
import glob, os, re

DOCS = r"C:\Games\EVBots-dev\VU-Docs\content\vext\ref\fb"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "VextFbTypes.cpp")

names = []
for f in sorted(glob.glob(os.path.join(DOCS, "*.md"))):
    text = open(f, encoding="utf-8").read()
    if "### Constructors" not in text:
        continue
    m = re.search(r"^title:\s*([A-Za-z_][A-Za-z0-9_]*)\s*$", text, re.M)
    if m:
        names.append(m.group(1))
names = sorted(set(names))

rows, line = [], "    "
for n in names:
    item = '"%s", ' % n
    if len(line) + len(item) > 118:
        rows.append(line.rstrip())
        line = "    "
    line += item
rows.append(line.rstrip())

cpp = r'''// VextFbTypes.cpp — СГЕНЕРИРОВАН tools\gen_fb_types.py из VU-Docs (content\vext\ref\fb, классы с «Constructors»).
// Руками не править: перегенерировать. Классов: %(count)d.
//
// Клиентский Lua (сборка 215): у VU каждый fb-класс — глобальный тип с тремя конструкторами (VU-Docs, напр.
// fb\registrycontainer.md): Foo() — новый экземпляр, Foo(guid: Guid) — новый с этим GUID, Foo(other: DataContainer) —
// приведение. Мод зовёт их по имени (`RegistryContainer(...)`, `UIScreenAsset(p_Screen)`, `_G[typeInfo.name](x)`).
// На сервере касты появляются по ходу (VextResourceManager: RegisterCastGlobal) — сервер здесь не трогаем.
// Пока данных движка в клиенте нет (К5), экземпляр — Lua-таблица с typeInfo.name, MakeWritable(), Is(); приведение
// отдаёт тот же объект; Foo(nil) — ошибка, как у VU и как у нашего серверного каста.

#include "VextFbTypes.h"

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace evbots {
namespace {

const char* const kFbClassNames[] = {
%(rows)s
};

// Чанк получает таблицу имён классов первым аргументом.
constexpr char kFbTypesSource[] = R"LUA(
local names = ...

local function isGuid(v)
  return type(v) == "table" and v._b ~= nil and type(v.ToString) == "function"
end

local function makeType(name)
  local methods = {}
  function methods:MakeWritable() end
  function methods:Is(typeName) return typeName == name end
  local instMt = { __index = methods, __name = name }
  local function newInstance(guid)
    return setmetatable({ typeInfo = { name = name }, instanceGuid = guid }, instMt)
  end
  return setmetatable({}, {
    __name = name,
    __call = function(_, ...)
      local n = select("#", ...)
      if n == 0 then return newInstance(nil) end
      local v = (...)
      if v == nil then
        error(name .. ": ожидался DataContainer или Guid, получено nil", 2)
      end
      if isGuid(v) then return newInstance(v) end
      return v  -- приведение: тот же объект
    end,
  })
end

local added = 0
for i = 1, #names do
  local n = names[i]
  if rawget(_G, n) == nil then
    _G[n] = makeType(n)
    added = added + 1
  end
end
return added
)LUA";

}  // namespace

int RegisterVextFbTypes(lua_State* L) {
    if (luaL_loadbufferx(L, kFbTypesSource, sizeof(kFbTypesSource) - 1, "=[vext-fbtypes]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: fb-типы не собрались: %%s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return -1;
    }
    const int count = static_cast<int>(sizeof(kFbClassNames) / sizeof(kFbClassNames[0]));
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushstring(L, kFbClassNames[i]);
        lua_rawseti(L, -2, i + 1);
    }
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: fb-типы не выполнились: %%s", err != nullptr ? err : "?");
        lua_pop(L, 1);
        return -1;
    }
    const int added = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    LOG_INFO("EVBots: fb-типы VU (клиент): %%d из %%d классов зарегистрированы как глобалы", added, count);
    return added;
}

}  // namespace evbots
''' % {"count": len(names), "rows": "\n".join(rows)}

open(OUT, "w", encoding="utf-8", newline="\n").write(cpp)
print("classes:", len(names), "->", os.path.normpath(OUT))
