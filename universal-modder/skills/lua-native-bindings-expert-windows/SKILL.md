---
name: lua-native-bindings-expert-windows
description: Разработка безопасных нативных Lua-расширений на C/C++ под Windows и кроссплатформенно: lua_CFunction, стек Lua, ошибки, userdata, модули, колбэки, MSVC/MinGW, DLL-экспорты, пути, кодировки, SEH-изоляция и совместимость Lua 5.1–5.4.
compatibility: ["opencode", "claude-code", "cursor"]
---

# Lua Native Bindings Expert — Windows / Portable

Ты — senior/lead инженер по нативной интеграции Lua в C/C++ приложениях на Windows и кроссплатформенных проектах. Твоя задача — писать безопасные, предсказуемые, переносимые Lua-расширения, которые не ломают стек Lua, не падают от невалидных аргументов, не течёт память, не зависят от одного движка/проекта/офсета и корректно собираются в Windows-окружении.

Этот файл является универсальным стандартом. Любые project-specific вещи — игровые офсеты, конкретные структуры, чит-модификации, internal hacks, bot-логику, memory editing — выноси в отдельный слой и не смешивай с универсальными правилами Lua C API.

---

## 0. Windows-дефолты этого скилла

Под Windows считай базовыми следующие правила:

- Компиляторы: MSVC, clang-cl, MinGW-w64.
- Стандарт C++: минимум C++17, лучше C++20, если проект позволяет.
- Исключения C++: `/EHsc` для MSVC; не смешивай C++ exceptions и Lua `longjmp` без RAII и контроля.
- Calling convention для Lua API: default `__cdecl` / `/Gd`. Не включай `/Gr` fastcall для файлов, использующих `lua_CFunction`.
- Исходники: UTF-8. Для MSVC добавляй `/utf-8`, если в коде есть не-ASCII строки/комментарии.
- Строки для Lua: передавай как UTF-8 byte sequences через `lua_pushlstring`.
- Пути Windows: внутри Lua предпочтительно использовать `/` или Lua long strings `[[C:\path\to\file]]`.
- DLL-модули Lua: экспортируй `luaopen_<module>` через `extern "C"` и, если модуль грузится динамически, через `__declspec(dllexport)`.
- Low-level память: не давай Lua прямой доступ к произвольным адресам. Если нужен legacy/SEH-доступ, изолируй его в отдельном Windows-specific adapter.

---

## 1. Главные принципы

- **Portable first.** Код должен работать на Lua 5.1, 5.2, 5.3, 5.4 и LuaJIT там, где это возможно.
- **C API — это C контракт.** Не позволяй C++ UB, dangling pointer, stack imbalance и необработанным исключениям выходить в Lua.
- **Никогда не доверяй Lua-стороне.** Все аргументы проверяются.
- **Владелец ресурса очевиден.** `lua_State`, references, userdata, callbacks, buffers имеют явный lifecycle.
- **Ошибки явные.** Не глотай ошибки, не возвращай `0`, если произошла проблема.
- **Стек Lua сбалансирован.** После выхода C-функции на стеке остаются только результаты.
- **Безопасность важнее удобства.** Не раскрывай указатели, офсеты, native handles без контроля.
- **Windows-specific код изолируется.** MSVC intrinsics, SEH, WinAPI paths, DLL loading — в отдельных адаптерах.

---

## 2. Сигнатура функций для Lua: `lua_CFunction`

Любая функция, регистрируемая в Lua через `lua_register`, `lua_pushcfunction`, `luaL_setfuncs`, `luaL_newlib`, обязана иметь сигнатуру:

```cpp
int function_name(lua_State* L);
```

### Правила

- Всегда возвращай `int`.
- Возвращаемое `int` — количество значений, оставленных на вершине стека Lua.
- Ничего не возвращаешь:

```cpp
return 0;
```

- Одно значение:

```cpp
return 1;
```

- Несколько значений:

```cpp
return n;
```

- Никогда не пиши `void` для Lua-функций.

### Правильно

```cpp
int l_no_result(lua_State* L) {
    // логика
    return 0;
}

int l_one_result(lua_State* L) {
    lua_pushinteger(L, 42);
    return 1;
}
```

### Неправильно

```cpp
void l_bad(lua_State* L) {
    // Не является lua_CFunction.
}
```

### Лямбды

Capturing lambda нельзя передать туда, где ожидается `lua_CFunction`.

Неправильно:

```cpp
auto ctx = getContext();
lua_register(L, "foo", [ctx](lua_State* L) {
    return 0;
});
```

Правильно: используй static/free функцию и upvalue.

```cpp
int l_foo(lua_State* L) {
    Context* ctx = static_cast<Context*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!ctx) {
        lua_pushnil(L);
        lua_pushstring(L, "missing context");
        return 2;
    }

    // логика

    return 0;
}

void register_foo(lua_State* L, Context* ctx) {
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, l_foo, 1);
    lua_setfield(L, -2, "foo");
}
```

### Member functions

Используй статический мост.

```cpp
class Widget {
public:
    int setColorLua(lua_State* L);
};

static int l_widget_setColor(lua_State* L) {
    Widget* self = static_cast<Widget*>(luaL_checkudata(L, 1, "mylib.Widget"));
    return self->setColorLua(L);
}
```

---

## 3. Дисциплина стека Lua

Стек Lua — основное место ошибок.

### Базовый шаблон

```cpp
int l_example(lua_State* L) {
    const int base = lua_gettop(L);

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer mode = check_integer(L, 2);

    lua_pushstring(L, "temporary");
    // работа с временными значениями

    lua_settop(L, base);

    lua_pushboolean(L, 1);
    return 1;
}
```

### Правила

- Запоминай начальную высоту:

```cpp
const int base = lua_gettop(L);
```

- Аргументы — абсолютные индексы: `1`, `2`, `3`, ...
- Временные значения пушатся выше `base`.
- Перед возвратом результатов удаляй временные значения:

```cpp
lua_settop(L, base);
```

- Количество возвращаемых значений должно совпадать с реальным числом результатов.

### Проверка места на стеке

```cpp
luaL_checkstack(L, 8, "not enough stack space");
```

### Полезные функции

```cpp
lua_gettop(L);
lua_settop(L, index);
lua_pop(L, n);
lua_pushvalue(L, index);
lua_replace(L, index);
lua_remove(L, index);
lua_insert(L, index);
lua_concat(L, n);
```

### Важно про RAII stack guard

RAII-объект, восстанавливающий стек в деструкторе, ненадёжен, если внутри используется `luaL_error`, потому что Lua может сделать `longjmp`, минуя C++ деструкторы.

Лучше:

- явно контролировать стек;
- не использовать `luaL_error` в сложном C++ коде без RAII-аудита;
- возвращать ошибки как `nil + message`.

---

## 4. Валидация аргументов

Никогда не делай слепо:

```cpp
const char* s = lua_tostring(L, 1);
lua_Integer n = lua_tointeger(L, 2);
```

если аргумент обязателен.

### Обязательные аргументы

```cpp
luaL_checkany(L, 1);
luaL_checktype(L, 1, LUA_TTABLE);
luaL_checkstring(L, 1);
luaL_checknumber(L, 1);
luaL_checkfunction(L, 1);
luaL_checkudata(L, 1, "MyModule.MyType");
```

Для boolean используй:

```cpp
luaL_checktype(L, 1, LUA_TBOOLEAN);
bool value = lua_toboolean(L, 1) != 0;
```

### Опциональные аргументы

```cpp
lua_Integer mode = opt_integer(L, 2, 0);
const char* name = luaL_optstring(L, 3, "default");
lua_Number value = luaL_optnumber(L, 4, 1.0);
```

### Проверки условий

```cpp
luaL_argcheck(L, count > 0, 2, "count must be positive");
```

### Строки с длиной

Всегда работай с длиной:

```cpp
std::size_t len = 0;
const char* data = luaL_checklstring(L, 1, &len);
```

Не полагайся только на null-termination, особенно для бинарных данных.

### Integer compat helper

Для совместимости Lua 5.1–5.4 используй свои обёртки. `luaL_checkinteger` и `luaL_optinteger` появились в Lua 5.2:

```cpp
static lua_Integer check_integer(lua_State* L, int arg) {
#if LUA_VERSION_NUM >= 502
    return luaL_checkinteger(L, arg);
#else
    return static_cast<lua_Integer>(luaL_checknumber(L, arg));
#endif
}

static lua_Integer opt_integer(lua_State* L, int arg, lua_Integer default_value) {
#if LUA_VERSION_NUM >= 502
    return luaL_optinteger(L, arg, default_value);
#else
    if (lua_isnoneornil(L, arg)) {
        return default_value;
    }
    return static_cast<lua_Integer>(luaL_checknumber(L, arg));
#endif
}
```

### Смещение индексов между chunk'ом и C-функцией

Коварный класс багов: Lua-чанк, встроенный в C++, зовёт переданную C-функцию
**обычным вызовом** `chat_yell(message, duration)`, а не методом `ChatManager:Yell`
— тогда `message` лежит в индексе **1**, а C-код, написанный «как для метода»,
читает индекс 2 и получает не то поле (или nil). Типичные симптомы: биндинг
«молчит» (числовые/nil-аргументы тихо отбрасываются), лог пуст, а со стороны
выглядит как «мод завис» — хотя загрузка прошла. Правила:

- Определи контракт индексов **до написания C-кода**: `:`-вызов кладёт self в 1,
  обычный вызов — первый аргумент в 1. Зафиксируй его комментарием у обеих сторон.
- Проверяй тип аргумента явно (`luaL_checktype`/`lua_type`), а не «если не строка —
  тихо выходим»: тихий ранний возврат прячет сломанный контракт на недели.
- `luaL_check*`/`luaL_opt*` на **каждом** `l_*`-аргументе: `lua_tolstring` отдаёт
  `NULL` для не-строки, и `std::string(NULL)` — это `0xC0000005` до всякой логики.
- В конце сборки модуля логируй список зарегистрированных функций; при вызове
  несоответствие типов ловится `luaL_argerror` с номером аргумента, а не молчанием.

---

## 5. Типы данных и преобразования

| Lua | C/C++ |
|---|---|
| `nil` | отсутствие значения / `lua_isnoneornil` |
| `boolean` | `bool`, `lua_toboolean` |
| `number` | `lua_Number`, обычно `double` |
| `integer` | `lua_Integer` |
| `string` | `const char* + std::size_t` |
| `table` | индекс стека, C++ container при необходимости |
| `function` | Lua reference, `std::function` в controlled storage |
| `userdata` | C/C++ объект, handle, opaque pointer |

### Правила

- Размеры: `std::size_t`, не `int`.
- Указатели: не передавай как числа, если можно избежать.
- Если указатель обязан быть числом, используй `uintptr_t` и немедленную валидацию.
- Lua строки — byte sequences. Для текста используй UTF-8.
- Не интерпретируй `lua_touserdata` как валидный pointer без проверки метатаблицы.
- Не смешивай `lua_Number` и `lua_Integer` без понимания версии Lua.

---

## 6. UserData и жизненный цикл объектов

Предпочитай `userdata` с метатаблицей вместо сырых указателей.

### Пример

```cpp
struct Widget {
    int color = 0;
};

static const char* WIDGET_METATABLE = "mylib.Widget";

int l_new_widget(lua_State* L) {
    void* memory = lua_newuserdata(L, sizeof(Widget));
    Widget* widget = new (memory)();

    lua_compat_setmetatable(L, WIDGET_METATABLE);

    return 1;
}
```

### GC

```cpp
int l_widget_gc(lua_State* L) {
    Widget* widget = static_cast<Widget*>(lua_touserdata(L, 1));
    if (widget) {
        widget->~Widget();
    }
    return 0;
}
```

### Регистрация метатаблицы

```cpp
void create_widget_metatable(lua_State* L) {
    luaL_newmetatable(L, WIDGET_METATABLE);

    lua_pushcfunction(L, l_widget_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, l_widget_setColor);
    lua_setfield(L, -2, "setColor");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pop(L, 1);
}
```

### Метод

```cpp
int l_widget_setColor(lua_State* L) {
    Widget* widget = static_cast<Widget*>(luaL_checkudata(L, 1, WIDGET_METATABLE));
    widget->color = static_cast<int>(check_integer(L, 2));
    return 0;
}
```

### Compat setmetatable

```cpp
static void lua_compat_setmetatable(lua_State* L, const char* tname) {
#if LUA_VERSION_NUM >= 502
    luaL_setmetatable(L, tname);
#else
    luaL_getmetatable(L, tname);
    lua_setmetatable(L, -2);
#endif
}
```

### Правила

- Всегда проверяй тип через `luaL_checkudata`.
- `__gc` освобождает ресурсы и не бросает исключения.
- Не сохраняй указатели на userdata после `lua_close`.
- Не передавай userdata между разными `lua_State` напрямую.
- Для сложных объектов лучше хранить handle, ID или `std::shared_ptr` внутри userdata.

---

## 7. Registry и ссылки

Для долгоживущих Lua-функций и значений используй registry.

```cpp
int ref = luaL_ref(L, LUA_REGISTRYINDEX);
```

Получение:

```cpp
lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
```

Освобождение:

```cpp
luaL_unref(L, LUA_REGISTRYINDEX, ref);
```

### Правила

- Не храни индексы стека между вызовами.
- Все `luaL_ref` должны иметь парный `luaL_unref`.
- После `lua_close` все references невалидны.
- Не храни `lua_State*` в долгих объектах без понимания ownership.
- Для callback'ов храни reference в контролируемом контейнере.

---

## 8. Обработка ошибок

### Fatality / API misuse

Если вызов невозможно продолжить:

```cpp
return luaL_error(L, "expected table at argument #1, got %s", luaL_typename(L, 1));
```

Но осторожно: `luaL_error` делает `longjmp`. Не используй его в сложном C++ коде без RAII-аудита.

### Recoverable error

Лучший стиль для большинства native API:

```cpp
lua_pushnil(L);
lua_pushstring(L, "invalid handle");
return 2;
```

Lua-код:

```lua
local ok, err = native.doSomething()
if not ok then
    print(err)
end
```

### C++ exceptions

Никогда не позволяй C++ исключениям выходить за пределы `lua_CFunction`.

Шаблон безопасной точки входа:

```cpp
int l_safe_operation(lua_State* L) {
    try {
        const int base = lua_gettop(L);

        luaL_checktype(L, 1, LUA_TTABLE);

        performOperation(L);

        lua_settop(L, base);

        lua_pushboolean(L, 1);
        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushfstring(L, "native error: %s", e.what());
        return 2;
    } catch (...) {
        lua_pushnil(L);
        lua_pushstring(L, "unknown native error");
        return 2;
    }
}
```

### Важно

- Не смешивай C++ exceptions и Lua `longjmp` без контроля.
- Не лови ошибки молча.
- Не используй `assert()` для валидации пользовательских аргументов.
- Логируй native errors с именем функции и контекстом.
- Если внутри `try` может быть вызван `luaL_error`, перепроверь, какие C++ объекты будут уничтожаться некорректно.

---

## 9. Встраивание Lua-кода в C++: raw strings

Если Lua-код вшивается в C++, используй raw string literals.

### Правильно

```cpp
const char* script = R"LUA(
local config = {
    name = "hero",
    enabled = true
}

return config
)LUA";
```

### Неправильно для C++ raw string

```cpp
const char* script = R"LUA(
local config = {
    name = \"hero\"
}
)return config
)LUA";
```

Внутри C++ raw string не нужно экранировать кавычки для C++. Обратный слэш станет частью Lua-кода.

### Но помни про Lua escapes

C++ raw string убирает C++ escaping. Lua всё равно имеет свои escape sequences.

Для Windows путей лучше:

```cpp
const char* script = R"LUA(
local path1 = [[C:\Users\Public\file.txt]]
local path2 = "C:/Users/Public/file.txt"
return path1, path2
)LUA";
```

`[[ ... ]]` — Lua long string, backslashes там литеральные.

Если используешь обычную Lua-строку `"..."`, то для одного backslash в значении нужны два backslash в Lua-исходнике:

```cpp
const char* script = R"LUA(
local path = "C:\\Users\\Public\\file.txt"
return path
)LUA";
```

Это не C++ escaping, это Lua escaping.

### Правила

- Не добавляй лишние `\` только потому, что это C++ строка.
- Не экранируй `"` внутри raw string, если не хочешь реально передать backslash в Lua.
- Для путей предпочитай `/` или Lua `[[...]]`.
- Не собирай Lua-код конкатенацией недоверенных данных.
- Динамические значения передавай через стек Lua:

```cpp
lua_pushlstring(L, path.data(), path.size());
lua_setfield(L, -2, "path");
```

### Уникальный delimiter

Если в тексте может встретиться `)LUA`, используй другой delimiter:

```cpp
R"LUASCRIPT(
...
)LUASCRIPT"
```

### Безопасная загрузка строки

```cpp
int load_and_run(lua_State* L, const char* source, std::size_t len, const char* chunkname) {
#if LUA_VERSION_NUM >= 502
    if (luaL_loadbufferx(L, source, len, chunkname, "bt") != LUA_OK) {
#else
    if (luaL_loadbuffer(L, source, len, chunkname) != LUA_OK) {
#endif
        const char* err = lua_tostring(L, -1);
        lua_pushnil(L);
        lua_pushfstring(L, "load error: %s", err ? err : "unknown");
        return 2;
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        lua_pushnil(L);
        lua_pushfstring(L, "run error: %s", err ? err : "unknown");
        return 2;
    }

    return 1;
}
```

---

## 10. Регистрация модулей

### Стандартная точка входа

```cpp
extern "C" int luaopen_mylib(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"hello", l_hello},
        {"add", l_add},
        {nullptr, nullptr}
    };

#if LUA_VERSION_NUM >= 502
    luaL_newlib(L, funcs);
#else
    luaL_register(L, "mylib", funcs);
#endif

    return 1;
}
```

### Правила

- `luaopen_*` обязан быть `extern "C"`.
- Массив `luaL_Reg` заканчивается `{nullptr, nullptr}`.
- Возвращай таблицу модуля.
- Не засоряй `_G` без необходимости.
- Для динамической загрузки под Windows экспортируй символ.

### Windows export macro

```cpp
#if defined(_WIN32) && defined(MYLIB_BUILD_DLL)
#define MYLIB_API extern "C" __declspec(dllexport)
#else
#define MYLIB_API extern "C"
#endif
```

Использование:

```cpp
MYLIB_API int luaopen_mylib(lua_State* L) {
    // ...
}
```

Если модуль статически встроен в приложение, `__declspec(dllexport)` не нужен.

### Dotted module names

Если Lua-модуль называется `my.sub.module`, loader обычно ищет `luaopen_my_sub_module`. Проверяй конкретную версию Lua и свой loader.

### Compat macro для 5.1

```cpp
#if LUA_VERSION_NUM < 502
#ifndef luaL_newlib
#define luaL_newlib(L, l) (lua_newtable(L), luaL_register(L, nullptr, l))
#endif

#ifndef luaL_setmetatable
#define luaL_setmetatable(L, t) (luaL_getmetatable(L, t), lua_setmetatable(L, -2))
#endif
#endif
```

---

## 11. Колбэки из Lua в C++

### Сохранение callback

```cpp
int l_set_callback(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);

    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // сохранить ref в контексте/handle

    return 0;
}
```

### Вызов callback

```cpp
void call_callback(lua_State* L, int ref) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL) {
        return;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);

    lua_pushstring(L, "event");

    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        // залогировать ошибку
        lua_pop(L, 1);
    }
}
```

### Освобождение

```cpp
luaL_unref(L, LUA_REGISTRYINDEX, ref);
```

### Правила

- Проверяй, что аргумент действительно функция.
- Используй `lua_pcall`, а не `lua_call`, для пользовательского кода.
- Освобождай reference.
- Защищайся от reentrancy.
- Не вызывай Lua callback из произвольного потока, если `lua_State` не защищён.
- Учитывай, что callback может удалить объект, из которого его вызывают.

---

## 12. Многопоточность и жизнь `lua_State`

### Базовые правила

- Один `lua_State` не thread-safe.
- Не вызывай Lua API из разных потоков одновременно.
- Либо thread-local `lua_State`, либо mutex, либо message queue.
- Не дели userdata между разными `lua_State` напрямую.
- `lua_close` инвалидирует все references и userdata этого состояния.

### Рекомендуемые паттерны

#### Thread-local Lua state

Хорошо для плагинов и per-thread VM.

#### Single-state + mutex

Подходит, если Lua — центральный scripting hub.

#### Message queue

Самый безопасный вариант для game engines, редакторов и GUI-приложений:

- Lua вызывает native API;
- native API кладёт команду в очередь;
- главный поток исполняет команду.

Не делай прямой вызов графического/игрового API из произвольного потока, если движок этого не разрешает.

---

## 13. Низкоуровневая память и безопасность

Универсальный Lua binding не должен давать Lua произвольный доступ к памяти.

### Запрещено в универсальном слое

- Передавать arbitrary pointers как обычные числа.
- Делать `read_memory(address, size)` / `write_memory(address, value)` без sandbox.
- Использовать игровые офсеты как часть общего API.
- Полагаться на SEH вместо валидации.
- Писать в память по вычисленным адресам без bounds checking.

### Правильная абстракция

Вместо:

```lua
native.writeUint32(0x12345678, 10)
```

лучше:

```lua
native.setValue(handle, 10)
```

где `handle` — opaque ID с generation counter, правами и lifetime.

### Если low-level доступ действительно нужен

Изолируй в отдельном platform-specific модуле:

- opaque handles вместо сырых указателей;
- generation/ID для защиты от use-after-free;
- проверка null, диапазона, прав;
- locks/atomic для разделяемых ресурсов;
- логирование подозрительных обращений;
- тесты и sanitizers;
- никаких прямых адресов в Lua API.

---

## 14. Windows-specific: SEH только как последний барьер

`__try/__except` — Windows/MSVC-specific механизм. Не делай его частью универсального Lua API.

Используй SEH только в изолированном adapter'е, когда:

- уже выполнены все возможные проверки;
- нет C++ объектов, требующих unwinding внутри функции с `__try`;
- ошибка recoverable;
- состояние после исключения считается потенциально повреждённым;
- есть логирование и диагностика.

### Пример изолированного helper

```cpp
#if defined(_MSC_VER)

#include <windows.h>
#include <cstring>
#include <cstddef>
#include <cstdint>

static bool safe_copy_bytes_seh(void* dst, const void* src, std::size_t size) noexcept {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool safe_read_uint32_seh(const volatile std::uint32_t* addr, std::uint32_t* out) noexcept {
    __try {
        *out = *addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#endif
```

### Правила

- Не вызывай SEH helper напрямую из Lua по пользовательскому адресу.
- Сначала валидируй handle/object.
- Не используй SEH как замену bounds checking.
- Не глотай исключения без лога.
- Не помещай `__try` в функции с C++ деструкторами, если не понимаешь последствия.
- Для clang/MinGW `__try/__except` может отсутствовать или работать иначе. Используй `#if defined(_MSC_VER)`.

---

## 15. Windows-specific: DLL, экспорты и линковка

### Частые ошибки

#### C2664 / cannot convert parameter

Обычно неправильная сигнатура `lua_CFunction`.

Проверяй тип:

```cpp
int (*)(lua_State*)
```

а не:

```cpp
void (*)(lua_State*)
int (*)(lua_State*, int)
std::function<int(lua_State*)>
```

#### Unresolved external `luaopen_module`

Причина: C++ name mangling.

Решение:

```cpp
extern "C" int luaopen_module(lua_State* L);
```

Для динамической DLL под Windows:

```cpp
extern "C" __declspec(dllexport) int luaopen_module(lua_State* L);
```

#### Missing `{nullptr, nullptr}` in `luaL_Reg`

Lua будет читать за пределами массива.

#### Mixing Lua versions

Заголовки и библиотека должны соответствовать одной версии Lua и одному ABI.

#### Debug/Release mismatch

На Windows важно совпадение:

- CRT;
- architecture x86/x64/ARM64;
- runtime library `/MD` или `/MT`;
- defines;
- Lua build config.

#### Calling convention

Lua API ожидает default cdecl. Не включай `/Gr` для translation units, использующих `lua_CFunction`.

### MSVC пример сборки DLL

```bat
cl /EHsc /W4 /permissive- /utf-8 /LD mylib.cpp ^
   /I"C:\lua\include" ^
   "C:\lua\lib\lua54.lib" ^
   /DMYLIB_BUILD_DLL
```

### MinGW-w64 пример

```bat
g++ -std=c++17 -O2 -shared -o mylib.dll mylib.cpp ^
    -I"C:/lua/include" ^
    -L"C:/lua/lib" -llua ^
    -static-libgcc -static-libstdc++ ^
    -DMYLIB_BUILD_DLL
```

Пути и имена библиотек зависят от твоего Lua build.

### CMake пример

```cmake
cmake_minimum_required(VERSION 3.20)
project(lua_native_module LANGUAGES CXX)

find_package(Lua 5.4 REQUIRED)

add_library(mylib MODULE mylib.cpp)

target_include_directories(mylib PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(mylib PRIVATE ${LUA_LIBRARIES})

set_target_properties(mylib PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mylib"
)

if(MSVC)
    target_compile_options(mylib PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_compile_definitions(mylib PRIVATE MYLIB_BUILD_DLL)
endif()
```

Для Lua 5.1/5.2/5.3 замени `find_package(Lua 5.4 REQUIRED)` на свою логику поиска.

---

## 16. Windows-specific: пути и кодировки

### Lua строки — bytes

Передавай пути как UTF-8:

```cpp
lua_pushlstring(L, utf8_path.data(), utf8_path.size());
```

Не используй `lua_pushstring` для данных, которые могут содержать embedded null.

### Wide WinAPI -> UTF-8

```cpp
#if defined(_WIN32)

#include <string>
#include <string_view>
#include <windows.h>

static std::string utf8_from_wide(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }

    int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        w.data(),
        static_cast<int>(w.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (size <= 0) {
        return {};
    }

    std::string out(static_cast<std::size_t>(size), '\0');

    WideCharToMultiByte(
        CP_UTF8,
        0,
        w.data(),
        static_cast<int>(w.size()),
        out.data(),
        size,
        nullptr,
        nullptr
    );

    return out;
}

#endif
```

### Правила

- WinAPI используй wide версии: `CreateFileW`, `FindFirstFileW`, `GetFullPathNameW`.
- В Lua отдавай UTF-8.
- Не конвертируй через `CP_ACP`, если не нужен legacy ANSI.
- Для MSVC исходников используй `/utf-8`.
- Для console output при необходимости:

```cpp
SetConsoleOutputCP(CP_UTF8);
SetConsoleCP(CP_UTF8);
```

### Пути в Lua-коде

Предпочтительно:

```lua
local path = "C:/Users/Public/file.txt"
```

или:

```lua
local path = [[C:\Users\Public\file.txt]]
```

Избегай:

```lua
local path = "C:\Users\Public\file.txt"
```

потому что Lua интерпретирует backslash escapes.

---

## 17. Windows-specific: безопасная загрузка DLL

Если твой нативный модуль сам грузит DLL, используй безопасные флаги.

```cpp
#if defined(_WIN32)

#include <windows.h>

static HMODULE safe_load_library(const wchar_t* path) {
    return LoadLibraryExW(
        path,
        nullptr,
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR
    );
}

#endif
```

### Правила

- Не используй голое `LoadLibraryA` с относительными путями, если не контролируешь DLL search order.
- Предпочитай абсолютные пути.
- Используй `AddDllDirectory` / `RemoveDllDirectory`, если нужно.
- Защищайся от DLL hijacking.
- Проверяй `GetLastError()` при ошибке.
- Не грузи DLL из недоверенных директорий.

---

## 18. Совместимость Lua 5.1 / 5.2 / 5.3 / 5.4 / LuaJIT

### Частые различия

| Feature | Lua 5.1 | Lua 5.2+ | Lua 5.3+ | Lua 5.4 |
|---|---|---|---|---|
| Регистрация библиотеки | `luaL_register` | `luaL_setfuncs`, `luaL_newlib` | аналогично 5.2 | аналогично 5.2 |
| Длина строки/таблицы | `lua_objlen` | `lua_rawlen` | `lua_rawlen` | `lua_rawlen` |
| Integer | нет отдельного типа | нет отдельного типа | `lua_Integer` отдельно | `lua_Integer` отдельно |
| `luaL_checkint` | есть | deprecated/нет | нет | нет |
| `luaL_loadbufferx` | нет | есть | есть | есть |
| GC metamethod | `__gc` | `__gc` | `__gc` | `__gc` |

### Compat helpers

```cpp
#if LUA_VERSION_NUM < 502
#define lua_rawlen lua_objlen
#endif
```

Не раскидывай макросы по всему проекту. Сделай один `lua_compat.h`.

### Рекомендуемая структура compat

```text
native/lua/compat/lua_compat.h
```

Внутри:

- integer helpers;
- table length helpers;
- metatable helpers;
- library registration helpers;
- load buffer helpers.

---

## 19. Производительность

### Рекомендации

- Кэшируй индексы реестра и метатаблиц.
- Не создавай таблицы на каждый вызов без нужды.
- Используй upvalues для контекста.
- Минимизируй переходы Lua <-> C++.
- Для горячих путей делай batch API.
- Не аллоцируй лишнего в частых вызовах.
- Избегай `luaL_error` в нормальных flow.
- Для строк используй length-aware API.
- Для больших буферов не копируй лишнего.

### Batch API

Вместо:

```lua
for i = 1, 10000 do
    native.set(i, values[i])
end
```

лучше:

```lua
native.setMany(values)
```

---

## 20. Тестирование

Каждый native binding должен иметь тесты.

### Что тестировать

- Корректные аргументы.
- Неправильные типы.
- `nil`.
- Missing arguments.
- Extra arguments.
- Ошибки C++ exceptions.
- Stack balance.
- Утечки references.
- Повторную регистрацию модуля.
- Закрытие `lua_State`.
- Вызов из coroutine.
- Многопоточность, если поддерживается.
- Большие строки.
- Бинарные данные.
- UTF-8.
- Embedded null bytes.
- Invalid userdata.
- Use-after-free handles.
- Callback errors.
- Reentrant calls.

### Проверка баланса стека

```cpp
int before = lua_gettop(L);
lua_pcall(L, nargs, nresults, 0);
int after = lua_gettop(L);
```

После корректного вызова высота стека должна быть предсказуемой.

### Sanitizers и инструменты

Кроссплатформенно:

- AddressSanitizer;
- UndefinedBehaviorSanitizer;
- LeakSanitizer;
- Valgrind на Linux/macOS.

Windows:

- MSVC AddressSanitizer, если доступно;
- Debug CRT;
- Application Verifier;
- Page Heap для диагностики heap corruption;
- ETW/WinDbg для crash analysis.

---

## 21. Анти-паттерны

Не делай так:

```cpp
void l_bad(lua_State* L) {}
```

```cpp
int l_bad(lua_State* L) {
    const char* s = lua_tostring(L, 1);
    strcpy(buffer, s);
    return 0;
}
```

```cpp
int l_bad(lua_State* L) {
    auto ptr = static_cast<void*>(lua_tointeger(L, 1));
    *static_cast<int*>(ptr) = 42;
    return 0;
}
```

```cpp
int l_bad(lua_State* L) {
    lua_pushstring(L, "result");
    return 2;
}
```

```cpp
int l_bad(lua_State* L) {
    try {
        dangerousCall();
    } catch (...) {
        // проглотили ошибку
    }
    return 0;
}
```

```cpp
const char* code = R"LUA(
print(\"hello\")
)LUA";
```

```cpp
lua_register(L, "foo", [] (lua_State* L) mutable {
    return 0;
});
```

---

## 22. Чек-лист перед коммитом

- [ ] Функция имеет сигнатуру `int(lua_State*)`.
- [ ] Возвращаемое число совпадает с количеством результатов.
- [ ] Стек сбалансирован.
- [ ] Все обязательные аргументы проверены.
- [ ] Опциональные аргументы имеют безопасные defaults.
- [ ] Нет raw pointer arithmetic без валидации.
- [ ] Нет утечек `luaL_ref`.
- [ ] UserData имеет `__gc`, если владеет ресурсами.
- [ ] Метатаблицы имеют уникальные имена.
- [ ] Ошибки C++ перехвачены на границе Lua.
- [ ] `luaopen_*` объявлен как `extern "C"`.
- [ ] Для Windows DLL экспорт сделан корректно.
- [ ] `luaL_Reg` завершается `{nullptr, nullptr}`.
- [ ] Код совместим с целевыми версиями Lua.
- [ ] Нет project-specific офсетов в универсальном слое.
- [ ] Нет произвольного чтения/записи памяти без audit-слоя.
- [ ] Windows paths передаются как UTF-8 или через safe helpers.
- [ ] SEH используется только в изолированном adapter.
- [ ] Есть тесты на invalid input.
- [ ] Есть логирование native ошибок.
- [ ] Код собирается MSVC/MinGW/clang-cl на целевой архитектуре.

---

## 23. Минимальный шаблон безопасного Windows/Lua модуля

```cpp
#include <lua.hpp>

#include <cstddef>
#include <cstring>
#include <exception>

#if defined(_WIN32) && defined(MYLIB_BUILD_DLL)
#define MYLIB_API extern "C" __declspec(dllexport)
#else
#define MYLIB_API extern "C"
#endif

#if LUA_VERSION_NUM < 502
#ifndef luaL_newlib
#define luaL_newlib(L, l) (lua_newtable(L), luaL_register(L, nullptr, l))
#endif

#ifndef luaL_setmetatable
#define luaL_setmetatable(L, t) (luaL_getmetatable(L, t), lua_setmetatable(L, -2))
#endif

#define lua_rawlen lua_objlen
#endif

namespace {

static lua_Integer check_integer(lua_State* L, int arg) {
#if LUA_VERSION_NUM >= 502
    return luaL_checkinteger(L, arg);
#else
    return static_cast<lua_Integer>(luaL_checknumber(L, arg));
#endif
}

static lua_Integer opt_integer(lua_State* L, int arg, lua_Integer default_value) {
#if LUA_VERSION_NUM >= 502
    return luaL_optinteger(L, arg, default_value);
#else
    if (lua_isnoneornil(L, arg)) {
        return default_value;
    }
    return static_cast<lua_Integer>(luaL_checknumber(L, arg));
#endif
}

int l_hello(lua_State* L) {
    const int base = lua_gettop(L);

    std::size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);

    lua_settop(L, base);

    lua_pushliteral(L, "hello, ");
    lua_pushlstring(L, name, len);
    lua_concat(L, 2);

    return 1;
}

int l_add(lua_State* L) {
    const int base = lua_gettop(L);

    lua_Integer a = check_integer(L, 1);
    lua_Integer b = check_integer(L, 2);

    lua_settop(L, base);

    lua_pushinteger(L, a + b);
    return 1;
}

int l_try_divide(lua_State* L) {
    const int base = lua_gettop(L);

    lua_Number a = luaL_checknumber(L, 1);
    lua_Number b = luaL_checknumber(L, 2);

    lua_settop(L, base);

    if (b == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "division by zero");
        return 2;
    }

    lua_pushnumber(L, a / b);
    return 1;
}

int l_safe_operation(lua_State* L) {
    try {
        const int base = lua_gettop(L);

        luaL_checktype(L, 1, LUA_TTABLE);

        // Здесь может быть сложный C++ код.
        // Не вызывай luaL_error, если не уверен в longjmp/RAII.

        lua_settop(L, base);

        lua_pushboolean(L, 1);
        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushfstring(L, "native error: %s", e.what());
        return 2;
    } catch (...) {
        lua_pushnil(L);
        lua_pushstring(L, "unknown native error");
        return 2;
    }
}

const luaL_Reg mylib_funcs[] = {
    {"hello", l_hello},
    {"add", l_add},
    {"tryDivide", l_try_divide},
    {"safeOperation", l_safe_operation},
    {nullptr, nullptr}
};

} // namespace

MYLIB_API int luaopen_mylib(lua_State* L) {
    luaL_newlib(L, mylib_funcs);
    return 1;
}
```

---

## 24. Шаблон userdata с Windows-safe lifecycle

```cpp
#include <lua.hpp>

#include <cstddef>
#include <new>
#include <utility>

namespace {

struct Widget {
    int color = 0;

    ~Widget() = default;
};

constexpr const char* WIDGET_METATABLE = "mylib.Widget";

int l_widget_gc(lua_State* L) {
    void* memory = lua_touserdata(L, 1);
    if (memory) {
        static_cast<Widget*>(memory)->~Widget();
    }
    return 0;
}

int l_widget_setColor(lua_State* L) {
    Widget* widget = static_cast<Widget*>(luaL_checkudata(L, 1, WIDGET_METATABLE));
    widget->color = static_cast<int>(luaL_checknumber(L, 2));
    return 0;
}

int l_widget_getColor(lua_State* L) {
    Widget* widget = static_cast<Widget*>(luaL_checkudata(L, 1, WIDGET_METATABLE));
    lua_pushinteger(L, widget->color);
    return 1;
}

int l_new_widget(lua_State* L) {
    void* memory = lua_newuserdata(L, sizeof(Widget));
    new (memory) Widget();

#if LUA_VERSION_NUM >= 502
    luaL_setmetatable(L, WIDGET_METATABLE);
#else
    luaL_getmetatable(L, WIDGET_METATABLE);
    lua_setmetatable(L, -2);
#endif

    return 1;
}

void create_widget_metatable(lua_State* L) {
    luaL_newmetatable(L, WIDGET_METATABLE);

    lua_pushcfunction(L, l_widget_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, l_widget_setColor);
    lua_setfield(L, -2, "setColor");

    lua_pushcfunction(L, l_widget_getColor);
    lua_setfield(L, -2, "getColor");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pop(L, 1);
}

} // namespace
```

Регистрация в модуле:

```cpp
create_widget_metatable(L);

lua_newtable(L);
lua_pushcfunction(L, l_new_widget);
lua_setfield(L, -2, "new");
lua_setfield(L, -3, "widget");
```

---

## 25. Шаблон protected callback holder

```cpp
#include <lua.hpp>

#include <utility>

class LuaCallback {
public:
    LuaCallback() = default;

    ~LuaCallback() {
        release();
    }

    LuaCallback(const LuaCallback&) = delete;
    LuaCallback& operator=(const LuaCallback&) = delete;

    LuaCallback(LuaCallback&& other) noexcept
        : L_(other.L_), ref_(other.ref_) {
        other.L_ = nullptr;
        other.ref_ = LUA_NOREF;
    }

    LuaCallback& operator=(LuaCallback&& other) noexcept {
        if (this != &other) {
            release();
            L_ = other.L_;
            ref_ = other.ref_;
            other.L_ = nullptr;
            other.ref_ = LUA_NOREF;
        }
        return *this;
    }

    void reset(lua_State* L, int stack_index) {
        release();

        L_ = L;

        lua_pushvalue(L_, stack_index);
        ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    }

    bool valid() const noexcept {
        return L_ != nullptr && ref_ != LUA_NOREF && ref_ != LUA_REFNIL;
    }

    bool call(lua_State* /*unused*/, int nargs, int nresults) {
        if (!valid()) {
            return false;
        }

        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_);

        // Функция запушилась на вершину стека ПОСЛЕ аргументов.
        // lua_pcall требует, чтобы функция была ПОД аргументами.
        // lua_insert перемещает функцию (верхний элемент) на позицию перед первым аргументом.
        lua_insert(L_, -nargs - 1);

        if (lua_pcall(L_, nargs, nresults, 0) != LUA_OK) {
            const char* err = lua_tostring(L_, -1);
            // Здесь должен быть логгер проекта.
            (void)err;
            lua_pop(L_, 1);
            return false;
        }

        return true;
    }

private:
    void release() {
        if (L_ != nullptr && ref_ != LUA_NOREF && ref_ != LUA_REFNIL) {
            luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
        }

        L_ = nullptr;
        ref_ = LUA_NOREF;
    }

    lua_State* L_ = nullptr;
    int ref_ = LUA_NOREF;
};
```

Важно: этот класс не thread-safe. Владелец должен гарантировать, что `lua_State` жив и не используется из другого потока одновременно.

---

## 26. Рекомендуемая структура проекта

```text
native/
  lua/
    compat/
      lua_compat.h
    core/
      error_utils.h
      registry.h
      callback.h
      stack_utils.h
    modules/
      mylib/
        mylib.cpp
        mylib.h
        mylib_test.cpp
    platform/
      windows/
        path_utils.cpp
        path_utils.h
        seh_adapter.cpp
        seh_adapter.h
        dll_loader.cpp
        dll_loader.h
      generic/
        memory_guard.cpp
        memory_guard.h
```

### Назначение

- `compat` — различия Lua 5.1–5.4.
- `core` — общие утилиты bindings.
- `modules` — бизнес-логика.
- `platform/windows` — только Windows-specific код.
- Никаких игровых офсетов и project-specific hacks в `core`.

---

## 27. Финальное правило

Хороший Lua C/C++ binding под Windows — это не просто «чтобы компилировалось в MSVC». Это интерфейс, который:

- не ломает стек;
- не падает от кривых аргументов;
- не течёт;
- не держит dangling pointers;
- понятно сообщает ошибки;
- работает на Lua 5.1–5.4;
- корректно собирается MSVC/MinGW/clang-cl;
- безопасно обрабатывает пути и кодировки;
- не превращает Lua в инструмент произвольного доступа к памяти;
- изолирует Windows-specific хаки в отдельных адаптерах.

Держи этот уровень качества во всех нативных расширениях.
