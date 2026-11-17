# BOTLAYER-REV — разбор botlayer.dll (фаза 1, шаг 1)

Триаж без дизассемблера: `triage_pe.py` (PE-заголовки/секции/импорты) +
`extract_hooks.py` (строки по категориям). Дата: 2026-10-08.
Команды:
```
python triage_pe.py C:\Games\BF3LegacyLan\activation\botlayer.dll --strings
python extract_hooks.py
```
Выгрузки: `triage-botlayer.txt`, `botlayer-strings.txt`, `botlayer-hooks.txt`.

## 1. Что это за файл

| Параметр | Значение | Вывод |
|---|---|---|
| Размер | 1 281 024 б (1.22 МБ) | — |
| Платформа | **PE32 x86 (32-bit)** | подтверждает правило AGENTS.md: код только x86 |
| ImageBase | `0x10000000` | **нестандартный** (не 0x10000000 у exe-стандарта) |
| EntryPoint | `0x000a7c40` | — |
| Сборка | 2026-09-19 20:27 | свежий |
| DLL Characteristics | `0x0140` = ASLR + NX_COMPAT | позиции ASLR, хуки — по RVA/AOB |
| Секций | 6: `.text` `.rdata` `.data` `.fptable` `.rsrc` `.reloc` | `.fptable` — именованная, см. §4 |
| Импорты | **только KERNEL32.dll** (94 функции) | важно, см. §3 |
| Экспорты | **нет** | вход только через DllMain/экспорт загрузчика |
| PDB | не найден | отладочных символов нет |

Секции (vaddr / vsize / rawsize):
```
.text    0x00001000  0x000c757a  816640   X,R,code
.rdata   0x000c9000  0x00067e84  425984   R,idata
.data    0x00131000  0x000deb70    3584   R,W,idata
.fptable 0x00210000  0x00000080     512   R,W,idata
.rsrc    0x00211000  0x000001e0     512   R,idata
.reloc   0x00212000  0x00007e4c   32768   R,idata
```

## 2. Это **C++20 / MSVC / CMake** — уточнение после Ghidra

> ⚠️ Первоначальный вывод по триажу был «Rust-крейт» — **он неверен**, исправлено
> после headless-разбора в Ghidra. Причина ошибки: `build/release/botlayer.dll` —
> это путь **CMake** (multi-config), а не Cargo (у Cargo это `target/release/`),
> а `__crt_strtox::floating_point_value::*` — символы **статического CRT MSVC**,
> принятые за внутренние имена Rust-рантайма.

Доказательства (Ghidra, `ghidra-botlayer.txt`):

```
0x10103a34  Sep 19 2026 22:08:59 release x86 C++20      <- штамп компилятора MSVC
.rsrc       <?xml ...><assembly ...manifestVersion='1.0'> <- манифест MSVC
секция tdb 0xffdff000-0xffdfffff size=4096 rw-           <- MSVC Thread Debugging Block (x86)
__crt_strtox::floating_point_value::as_double|as_float  <- статический CRT MSVC, C++ mangling
```

Поиск Rust-маркеров (`rustc`, `cargo`, `.cargo`, `registry/src`, `panic`,
`unwrap`, `core::`, `alloc::`, `RUST_BACKTRACE`) дал **0 совпадений**.

Следствия для плана:
1. Обфускации нет: строки не зашифрованы, `.rdata` читаема; Ghidra сама
   нашла **2490 функций**.
2. **LegacyEXT пишем на C++20** (тот же язык и ABI), а не на Rust.
3. Хуки движка BF3 — C++-виртуальные вызовы: в строках `vptr`, `ClassInfo`,
   `EnableMasterSlaveCopy` → в обёртках нужен `__thiscall`/`__fastcall`,
   а не `__cdecl` (см. `native.md` §3).

## 3. Импорты: только KERNEL32 → работа через GetProcAddress

В таблице импортов **одна** DLL — `KERNEL32.dll` (94 функции). Движковых API
(D3D, Input, файлы) в импортах нет, но строки содержат `kernel32.dll`,
`mscoree.dll`, `ws2_32.dll`.

Вывод: реальные вызовы идут через **динамическое связывание** —
`LoadLibraryA` / `GetProcAddress` (оба есть в импортах явно). Список функций
Kernel32, который реально используется:

```
GetProcAddress  LoadLibraryA  VirtualAlloc  VirtualProtect  VirtualFree
CreateThread    Sleep         GetTickCount64  QueryPerformanceCounter
GetEnvironmentVariableA/W       GetModuleHandleA   FlushInstructionCache
GetCurrentThreadId  VirtualQuery  DisableThreadLibraryCalls  GetCommandLineA
```

Значимое:
- `VirtualProtect` + `FlushInstructionCache` = **патчинг кода в процессе** —
  это и есть механизм хуков (он же detour, см. §5).
- `CreateThread` + `Sleep` = фоновые потоки ботов.
- `GetEnvironmentVariableA/W` = чтение 134 флагов `BF3ACT_*` (§6).
- `QueryPerformanceCounter/…` = таймеры (LOS-период, update-интервал).
- `ws2_32.dll` (в строках, не в импортах) = сетевой код, грузится динамически.

## 4. Секция `.fptable` — кэш отложенных импортов CRT (не реестр хуков)

> Первоначальный вывод «реестр хуков, заполняемый в рантайме» — **неверен**.
> Декомпиляция функций, которые туда пишут, показала CRT.

| секция | rawsize | vsize | ненулевых байт на диске |
|---|---|---|---|
| `.text` | 816 640 | 816 506 | 708 353 |
| `.data` | 3 584 | 912 240 | 1 421 |
| **`.fptable`** | **512** | **128** | **0** |
| `.rsrc` | 512 | 480 | 398 |

`.fptable` нулевой на диске — это 32 слота, которые заполняются лениво.
Функции `FUN_100c05fb`, `FUN_100c0625` и др. имеют вид:

```c
if (DAT_10210048 == -1) return 0;              // резолв не удался — кэш отрицательный
if (DAT_10210048 == 0)                         // ещё не резолвили
    DAT_10210048 = FUN_100c067b(0x12, "LCMapStringEx", &DAT_10129b34, "LCMapStringEx");
return DAT_10210048;
```

То есть это **отложенный импорт статического CRT MSVC** с кэшем указателей
(`AreFileApisANSI`, `LCMapStringEx`, …), а не таблица хуков.

Реальный реестр хуков — в очереди у `FUN_100262e0`: массив записей по `0x58`
байт, 11 штук (`0x2C0`), не более 64 за раз. См. `BOTLAYER-HOOKS.md`.

Ghidra автоматически определила **2490 функций**; символьной таблицы нет
(PDB не найден) → все имена `FUN_*`. Это удобно: обфускации нет, код читаем.

## 5. Механизм хуков: ЖЁСТКИЕ АДРЕСА + сверка байт (уточнено декомпиляцией)

> Первоначальная формулировка «detour по AOB-сигнатурам» была **неточной**.
> Декомпиляция показала: адреса **зашиты абсолютно**, а сверка байт — это
> проверка целостности, а не поиск. Подробно: `BOTLAYER-HOOKS.md`.

Ключевые строки:
```
: detour
`omni callsig'
BF3ACT_BOTLAYER_HITHOOK
BF3ACT_BOTLAYER_UPDHOOK
```
Паттерн `0x%08x -> 0x%08x` — лог перехвата адреса (оригинал → патч).

Установщики найдены по xref на `FlushInstructionCache` / `VirtualProtect` /
`VirtualAlloc`: `FUN_100256f0` (NETDMG), `FUN_10025a00` (CHATPROBE),
`FUN_10026740` (пробный, TEAMSWITCHPROBE), `FUN_10026b80`.

Итог: **inline-hook 8 байт** (E9 rel32 + NOP), трамплин на 5 первых байт
оригинала, атомарная запись под `LOCK()`, цели — в `bf3_server_Final.exe`.
Четыре адреса сверены с байтами в реальном образе — **все совпали**.

## 6. Флаги `BF3ACT_BOTLAYER_*` — 134 штуки (поверхность настройки)

Это и есть «боевой профиль» бота. Полный список в `botlayer-hooks.txt`.
Сгруппированно по смыслу (выборка):

| Группа | Флаги |
|---|---|
| Прицел | `AIM`, `AIMBIAS`, `AIMBIAS_BUDGET`, `AIMPROBE`, `LOS`, `LOSPERIOD`, `FOV` |
| Огонь | `FIREMODE`, `HITHOOK`, `AMMO`, `NADEAMMO`, `RECOIL` |
| Движение | `AUTHMOVE`, `AUTHVEL_X/Y/Z`, `BODY`, `BODYYAW`, `BONEMAX`, `BONEPROBE`, `NAVI`, `PATH*` |
| Симуляция | `BAKERAYS`, `SOLDIER`, `POSE`, `LOD`, `ANIM*` |
| Спавн | `SPAWN`, `RESPAWN`, `TEAM*`, `REGISTER`, `CUSTOMIZE` |
| Данные | `BRAIN_DATA` (путь к brain.ini), `UPDHOOK`, `DEBUG`, `ARBITRATION`, `ANCHOR2` |
| Логи | `BF3ACT_LOG` |

## 7. Подтверждённые хуки движка (аннотации автора из Ghidra)

Это **цель фазы 4** — мост движок↔Lua строим поверх них.

| Класс движка | Метод | Что это даёт |
|---|---|---|
| `ServerPlayer` | `update` | главный тик игрока — сердце бота |
| `ServerProjectileEntityUpdater` | `update` | снаряды: запуск, траектория, урон |
| `ServerSoldierHealthModuleImpl` | `onDamage` | урон → триггеры, death-события |
| `ServerChatManager` | `handleMessage` | чат: команды ботов, подтверждения |
| `ServerPlayerSquadManager` | `onNewRound` | старт раунда, инициализация ботов |
| `ServerPlayerSquadManager` | `switchTeams` | смена команд |
| `ServerAutoTeamEntity` | `onMessage` | автораспределение в команды |
| `ClientDamageListener` | `onMessage` | клиентская сторона: урон, попадания |
| `BF3Legacy` | `botLos` | собственная функция LOS (line of sight) |

Лог-строка про снаряды содержит **прямой адрес функции движка**:
```
ServerProjectileEntityUpdater::update (0x00e78080)
```
и две перехваченные:
```
ClientDamageListener::onMessage 0x%08x -> 0x%08x
ServerChatManager::handleMessage 0x%08x -> 0x%08x
ServerProjectileEntityUpdater::update 0x%08x -> 0x%08x
```

## 8. Вшитые Ghidra-адреса (анализ bf3_server_Final.exe уже сделан)

Адреса движка из строк (сгруппированы, 8 уникальных):
```
0x2b2ad70    0x2b2afd4    0x2b3ee2c    0x2b47088
0x28586ac    0x28586b0    0x27de1c0    0x2435ef4    0x2b18144
```
Плюс структурные оффсеты, разобранные автором (всего 161 строка с оффсетами),
например:
```
[[0x2b3ee2c]+0x18] = 0x%08x, ServerEnable [0x2b2afd4]+0x44 = %u
cvar ant.DetailedCollision.forceServerPoseUpdate [[0x2b47088] = 0x%08x] = %s%u
[GA+0x150] %u, [GA+0x155] %u, lodValue [GA+0x158] = %d, shouldUpdate [GA+0x160] = 1
validTransforms [outer+0x38] %u, poseUpdateEnabled [+0x39] %u, poseNeeded [+0x3a] %u
[WF+0xb0] %d   [WF+0xfc]   [WF+0x101]   [wc+0x110]   [ws+0x04]   [ws+0x10]
ForcePoseUpdate [+0x39]   UpdateEnable [+0x32]   DisableAILodFeature [+0x36]
```

**Это карта полей внутренних структур Frostbite.** Например:
- `ant.DetailedCollision.forceServerPoseUpdate` — реальный cvar движка;
- `ServerEnable`/`UpdateEnable`/`DisableAILodFeature` — поля сущностей анимации.

Значение для нас: **не нужно начинать реверс с нуля.** Карта частично готова.
Но: эти адреса — из конкретной версии bf3_server_Final.exe. Наша задача —
воспроизвести их **AOB-сигнатурами**, иначе сломается при обновлении игры.

## 9. Что делать дальше (шаг 2 фазы 1)

1. **Ghidra headless** на `botlayer.dll` — разобрать `.fptable`, `DllMain`,
   функцию установки хуков, механизм `omni callsig`.
2. Найти в `.rdata` таблицу сигнатур паттернов (AOB) — она должна быть рядом
   с `.fptable`.
3. По адресам из §8 восстановить AOB-сигнатуры в `bf3_server_Final.exe`.
4. Отметить 9 хуков §7 как «мост для Lua» (фаза 4).
5. Всё найденное — в `Legacy\ext\MODLOG.md`, затем field note через `um kb new`.

Инструменты (установлено 2026-10-08):
- `IDA Free 9.1` — `C:\Program Files\IDA Free 9.1` (только GUI, без idat/idalib)
- `x64dbg` 2026.05.27 — winget (динамика)
- `Ghidra` — в `C:\Games\tools\` (headless `analyzeHeadless.bat` — основной)
- Python: `pefile`, `capstone`, `lief`, `r2pipe` (pip, Python 3.13)

## Ограничения этого разбора

Без дизассемблера видны только строки и метаданные PE. После Ghidra-разбора
осталось невыясненным:
- адреса 5 хуков из аннотаций автора (установщики не найдены статически);
- как ведёт себя бот при несовпадении сигнатуры (молча деградирует);
- устойчивость к обновлению BF3 (адреса жёсткие → не устойчивы).

Файлы дампов/декомпилированного кода в git не кладутся (правило AGENTS.md №4) —
весь вывод реверса лежит в `C:\Games\re\`, вне проекта.