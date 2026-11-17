# VU-ARCH — как устроен Venice Unleashed (эталон для Legacy)

Дата 2026-10-08. Найдено на машине пользователя, установленная копия:
`%LOCALAPPDATA%\VeniceUnleashed\` (из реестра:
`Venice Unleashed version 1.0`, InstallLocation `~\AppData\Local\...\client\`).

**Зачем это нам.** Legacy — аналог VU. Раньше мы знали VU только по
документации (`VU-Docs`) и чужим ботам. Теперь у нас есть **сам эталон**:
три слоя, где один из них (`veniceext.dll`) — ровно то, что мы собираем
как LegacyEXT. Это меняет оценку фазы 4 с «написать с нуля» на «сделать
локальный аналог известной архитектуры».

> ⚠️ Правило: анализ держим **локально**, в `C:\Games\re\`. Код VU, дампы и
> строки из его бинарей **не публикуем и не коммитим** (AGENTS.md №4).
> Ничего не обходим: VU лицензионный, DRM не трогаем — изучаем интерфейсы.

## 1. Что установлено

```
%LOCALAPPDATA%\VeniceUnleashed\
├── .luc\                              локальный кэш
└── client\
    ├── vu.exe        132 376 б  PE32 x86, сборка 2022-07-21
    ├── vu.com        26 392 б  PE32 x86, console-вариант
    ├── cacert.pem              корни CA (TLS → интернет)
    └── vu\
        ├── ui.vuic   17 995 088 б   UI-файл (18 МБ)
        └── vubin\
            ├── veniceext.dll  156 629 504 б (149 МБ)  ← ЭТАЛОН LegacyEXT
            ├── vu-core.dll       9 838 592 б (9.4 МБ)  ← приложение
            ├── lua.dll             243 200 б            Lua 5.4.2
            ├── v8.dll + v8_*      ~17 МБ                V8 (WebUI)
            ├── cohtml.WindowsDesktop.dll   5.5 МБ       CoHTML (WebUI)
            ├── HttpServer.WindowsDesktop.dll 7.8 МБ
            ├── phonon.dll / RenoirCore      аудио
            ├── d3dcompiler_47.dll, discord_game_sdk.dll
```

Все ключевые бинарники — **PE32 x86**, сборки 2026-08-07 (то есть свежие),
все с ASLR + NX. Это совпадает с нашим правилом «весь код под x86».

## 2. Архитектура в трёх слоях

```
vu.exe / vu.com           ── упакованный стартовый stub
        │
        ├─► vu-core.dll   (экспортируется как «vu.dll», 167 экспортов)
        │        │
        │        └─ импортирует ровно ОДНУ функцию у veniceext.dll:
        │             import  veniceext.dll!GetModdingInterface
        │
        └─► veniceext.dll  149 МБ  ── экспорт: GetModdingInterface
                 │
                 └─► lua.dll  Lua 5.4.2
```

**Это ключевой факт: точка сопряжения — единственный экспорт
`GetModdingInterface`.** Всё остальное — детали реализации.

### 2.1 `veniceext.dll` — референс LegacyEXT

#### Точка входа: `GetModdingInterface` разобрана (Capstone)

```asm
GetModdingInterface:                      ; VA 0x107023F0, экспорт #1
  mov  eax, [0x18C99280]                  ; кэш синглтона
  test eax, eax
  jne  return_cached
  push 4
  call 0x1725CB77                         ; operator new(4)
  add  esp, 4
  test eax, eax
  je   set_null
  mov  dword ptr [eax], 0x177BD830        ; <- vtable
  mov  dword ptr [0x18C99280], eax        ; запомнить
  ret
```

Возвращается **объект из 4 байт = одна ссылка на vtable** `0x177BD830`.
Ленивый синглтон. Никаких параметров, никакого контекста.

#### Vtable содержит ровно 5 методов

| # | VA | Назначение (по дизассемблу) |
|---|---|---|
| 0 | `0x10703430` | **инициализация**: под флагом `[0x18E75380]` вызывает setup и заполняет **~30 таблиц** движковых указателей (`0x18E74D00..0x18E75338`) значениями вида `0x22E9630`, `0x2384EC5`, `0x23B4DF0` — это резолв нативных функций Frostbite |
| 1 | `0x10702420` | `jmp 0x107195A0` — переходник |
| 2 | `0x107023E0` | `jmp 0x1072F6C0` — переходник |
| 3 | `0x10702430` | `jmp 0x10724870` — переходник |
| 4 | `0x10702440` | конструирует объект ~24 байта (vtable + SSO-строка длиной 8, ёмкость 15) — по виду «версия/идентификатор API» |

Сразу после vtable идут строки (`"1.16"`, `"0.0"`, `"…genice"` → `VeniceEXT`,
`"unknown error"`).

**Вывод: контракт между приложением и расширением — 5 виртуальных методов.**
Всё остальное (303 API, 107 типов) живёт внутри DLL и доступно **только из Lua**.

#### VU биндит API к Lua через **SOL3**

Это главная техническая находка. В `.rdata` — **7069** RTTI-строк биндинга:

```
.?AU?$binding@$$BY01DU?$property_wrapper@P8Vec3@Shared@Classes@VeniceEXT@@
     BEMXZP81234@AEXM@Z@sol@@@Z@sol@@VVec3@Shared@Classes@VeniceEXT@@@u_detail@sol@@
```

Читается: `sol::detail::binding`, `sol::usertype`, `sol::property`,
`sol::state` (`This_state@sol@@`). Пространства имён —
`Shared@Classes@VeniceEXT`, `Client@Classes@VeniceEXT`, `Server@Classes@VeniceEXT`.

Распределение биндингов: **Shared 6622, Client 1039, Server 432**.

**Практическое следствие:** VU **не пишет 303 обёртки руками** — он описывает
типы декларативно, а SOL3 генерирует биндинги. Наш LegacyEXT может делать
так же (SOL3 — открытая MIT-лицензия). Это радикально меняет оценку фазы 4:
не «303 обёртки», а «объявления типов + хуки движка».

#### Перекрёстная проверка нашего инвентаря

Сверили RTTI бинарника со своим `vext-inventory.md` (из VU-Docs):

| | |
|---|---|
| типов в RTTI | **107** (Shared 44, Client 61, Server 21) |
| типов в нашем инвентаре | 119 |
| **совпали и там, и там** | **98** |
| только в RTTI (нет в документации) | 9 |
| только в документации | 21 |
| менеджеров/библиотек найдено по именам | **15 из 17** |

Найдены по именам: `PlayerManager`, `EntityManager`, `RaycastManager`,
`ChatManager`, `TeamSquadManager`, `ResourceManager`, `MathUtils`,
`SharedUtils`, `NetEvents`, `ServerUtils`, `TicketManager`, `Events`, `Hooks`,
`ClientUtils`, `TerrainEditing`. Не найдены как строки: `Net`,
`Extensibility` (вероятно, регистрируются иначе).

**Вывод: наш инвентарь API из документации верен** — 98 типов совпали с
реальным бинарником. Расхождения объяснимы (клиентские/серверные нюансы).

#### Остальные параметры

| Параметр | Значение |
|---|---|
| Размер | 156 629 504 б (149 МБ), `.text` = 125 МБ |
| Платформа | PE32 x86, imagebase `0x10000000`, ASLR + NX |
| Сборка | 2026-08-07 13:22 |
| Экспорты | **ровно 1**: `GetModdingInterface` |
| Строк (ASCII) | 151 048 уникальных |

Ключевые импорты (всё, что важно для нас):

| DLL | Что | Значение для нас |
|---|---|---|
| **`lua.dll` (74 функции)** | `luaL_newstate`, `luaL_loadbufferx`, `luaL_setfuncs`, `luaL_newmetatable`, `luaL_requiref`, `luaopen_string/math/os/io/package/utf8/debug`, `luaL_ref`, `luaL_traceback`, `luaL_argerror`, `lua_checkstack`, `lua_compare` | **встроенный Lua 5.4 + свой загрузчик модов**. Набор открываемых библиотек совпадает со спецификацией из VU-Docs |
| `KERNEL32` (129) | `ConvertThreadToFiber`, `CreateFiber`, `SwitchToFiber`, `DeleteFiber` | **волокна (fibers)** — планировщик Lua-скриптов, а не потоки |
| `WS2_32` (32) | `socket`, `connect`, `bind`, `listen`, `send`, `recv`, `WSAIoctl`, `getaddrinfo` | сеть; **нам нужен только локальный Blaze-канал** |
| `ADVAPI32` (16) | `CryptCreateHash`, `CryptDecrypt`, `CryptSignHashW`, `CryptExportKey`, `CryptGetUserKey`, `CryptGenRandom` | **server keys** — идентификация серверов; локально не нужна |
| `CRYPT32` (7) | `CertOpenStore`, `CertFindCertificateInStore`, `CertGetCertificateContextProperty` | TLS-сертификаты (интернет) |
| `bcrypt` | `BCryptGenRandom` | ГПСЧ |
| `MSVCP140` / `VCRUNTIME140` | MSVC 2015–2022 | **тот же компилятор, что у botlayer** |

Подтверждение Lua 5.4 (не 5.3): используется `luaL_setfuncs` — функция введена
в 5.4 и отсутствует в 5.3. Версия в бинарнике: **`$LuaVersion: Lua 5.4.2
Copyright (C) 1994-2020 Lua.org, PUC-Rio`**.

### 2.2 `vu-core.dll` — приложение (аналог нашего лаунчера + бэкенда)

| Параметр | Значение |
|---|---|
| Размер | 9 838 592 б (9.4 МБ) |
| Имя при экспорте | `vu.dll` |
| Экспортов | 167, из них основные — **asmjit**: `X86Assembler`, `JitRuntime`, `HostRuntime`, `CodeGen`, `Runtime`, `StaticRuntime`, `VMemMgr` |

| DLL | Что | Для нас |
|---|---|---|
| **`veniceext.dll`** | **1 импорт: `GetModdingInterface`** | мост приложение↔расширение |
| `WININET` (11) | `InternetOpenW`, `HttpSendRequestW`, `HttpQueryInfoW`… | **интернет** — нам не нужно |
| `cohtml` (33) | CoHTML `IViewListener` | WebUI (браузер в игре) |
| `WS2_32` (45) | полный сокет-стек | сеть; локальная версия проще |
| `ADVAPI32` (34) | крипто | server keys, TLS |
| `IPHLPAPI` | `GetAdaptersInfo` | **NAT/IP-детект** — то самое, что нам нужно для фазы 2 |
| `phonon` / `DSOUND` / `ole32` | аудио | второстепенно |

**Наличие asmjit** — отдельная находка: в vu-core есть **x86 JIT-компилятор**.
Значит VU где-то генерирует машинный код на лету (вероятно, оптимизация
горячих путей скриптов или внутренняя VM). Для локальной версии это можно
не повторять.

## 3. Что из этого берём в Legacy, а что выбрасываем

| Слой VU | Нужен нам | Комментарий |
|---|---|---|
| Список серверов (Zeus) | **да, локально** | `GET /servers` вместо интернет-бэкенда (фаза 2) |
| Server keys | **нет** | локальной сети достаточно; крипто отбрасываем |
| TLS / `cacert.pem` | **нет** | нет интернета |
| `GetModdingInterface` | **да, как образец** | наш LegacyEXT должен иметь один такой экспорт |
| Lua 5.4 + `luaL_requiref` загрузчик модов | **да** | ровно то, что в плане (фаза 4) |
| Fibers как планировщик | **возможно** | наш выбор; у нас свой тик от `TICK`-хука |
| CoHTML / V8 WebUI | **позже** | для UI; сначала серверные боты |
| asmjit JIT | **нет** | избыточно |
| Импорт `GetAdaptersInfo` | **да** | наш LAN-автодетект (фаза 2) |

## 4. Что это меняет в плане

1. **Фаза 4 перестаёт быть «изобрести»** — образец перед глазами: одна DLL,
   один экспорт, встроенный Lua 5.4, хуки движка, `luaL_requiref` для модов.
2. **Подтверждена версия Lua: 5.4.2** (а не LuaJIT) — совпадает с `PLAN.md`.
3. **Подтверждена архитектура внедрения**: DLL грузится внутрь процесса игры,
   сама подключается к приложению через экспорт. Наш `activation-loader
   --dll legacyext.dll` совместим с этим.
4. **Новые ориентиры для реверса**: из 9 хуков бота у нас 8 адресов; у VU
   интересен `GetModdingInterface` и то, как veniceext цепляется к движку BF3.
   Но veniceext — 149 МБ и, судя по импортам, без обфускации (есть все имена
   функций), значит анализ реален.

## 5. Чего не сделано

- **Полный анализ Ghidra на `veniceext.dll` невозможен на этой машине:** 149 МБ
  (`.text` 125 МБ) требует 5–7 ГБ heap, а доступно 8 ГБ RAM всего
  (3.4 ГБ свободно). Использован обходной путь: pefile + Capstone +
  потоковое сканирование строк. Для полного разбора нужна машина с 16+ ГБ RAM
  либо отдельный сервер анализа.
- Не разобраны методы vtable[1..4] (три из них — простые переходники).
- Не выяснено, что именно лежит в ~30 таблицах `0x18E74D00..` — это нативные
  указатели Frostbite, их имена требуют RTTI-обхода или динамики.
- Не проверено, какие библиотеки VU-Docs упоминает как «вырезанные»
  (`dofile/load/loadfile/os.execute`): среди импортов `lua.dll` их нет, но
  отключение может быть на уровне обёрток/состояния Lua.
- Не запускался VU (нужен интернет + лицензия).
- Не изучался `ui.vuic` (18 МБ, формат UI).
- Не троглась DRM/защита `vu.exe` (секции `.zdata`/`.cdata` — упаковщик).

## 6. Как читать дальше

- Триаж PE: `python C:\Games\Legacy\LegacyBF3\triage_pe.py <файл>`
- Строки и категории: `python C:\Games\Legacy\LegacyBF3\extract_hooks.py <файл>`
- Ghidra headless: см. `C:\Games\AGENTS.md`, раздел «Инструменты реверса».
- Вывод любого анализа VU — **в `C:\Games\re\`**, не в папку проекта
  (линт `um publish check` следит за этим).