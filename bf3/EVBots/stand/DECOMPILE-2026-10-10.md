# Декомпиляция bf3_server_Final.exe (2026-10-10) — что нашёл Ghidra

Файл: `C:\Games\Legacy\LegacyBF3\DECOMPILE-2026-10-10.md`
Гарантия: находки проверены по `re\server-decompile.txt` (полный тело трёх
функций). Разбор честный, не по памяти — сверен с исходником декомпиляции.

## 1. Контекст: как получили

- Ghidra 12.1.4 headless (`analyzeHeadless.bat`) импортировал
  `bf3_server_Final.exe` (RVA-пространство), проект лежит в
  `C:\Games\re\ghidra-jsrv\server` (база `server.rep`, **427 МБ** — импорт и
  автоанализ прошли полностью, `Import succeeded`).
- Скрипт-декомпилятор: `C:\Games\re\ghidra\DecompileAddr.java`.
- **Важно про окружение скрипта:** Python-версии в этой сборке Ghidra НЕТ
  (`Ghidra was not started with PyGhidra. Python is not available`), поэтому
  Python-скрипты (`DecompileAddr.py`) не выполняются. Только Java-скрипты.
- Команда (запускается быстро — анализ уже сделан, просто дикомпилируем по
  адресам):
  ```
  analyzeHeadless C:\Games\re\ghidra-jsrv server -process bf3_server_Final.exe \
    -scriptPath C:\Games\re\ghidra \
    -postScript DecompileAddr.java 0x00E5EF00 0x00E78080 0x0117EA40
  ```
  Вывод — в `C:\Games\re\server-decompile.txt` (переменная `BOTLAYER_OUT6`).

## 2. Три функций — что реально декодировали

### 2.1 `0x00E5EF00` — `FUN_00e5ef00` (это и есть tick/таргет TICK)
- Сигнатура (настоящая):
  ```c
  void __thiscall FUN_00e5ef00(void* this_, int param_2)
  ```
- Размер 453 байта. Название по аннотации движка — `ServerPlayer::update`.
- Что делает по коду: «тик» одного плеера — прокручивает массивы по смещениям
  этого-объекта (`+0x80..0x84`, `+0x90..0x94`), вызывает `FUN_00e38190(param_2)`
  на элементы, ставит флаги `+0xf0`, сбрасывает `+0xa4/+0xa8` к `-1`.
- **Ключевой вывод для фикса**: соглашение вызова `__thiscall` (this в ECX,
  второй аргумент — в стеке), а наш detour был `__fastcall` (оба в регистрах).
  Трамплин звал оригинал со сдвинутым стеком → краш. Детур убран (см. MODLOG),
  `Engine:Update` шлёт фоновый таймер.

### 2.2 `0x00E78080` — `FUN_00e78080` (UPDHOOK)
- Сигнатура:
  ```c
  float* __thiscall FUN_00e78080(void* this_, undefined8* param_2)
  ```
- Размер 1340 байт. Это `ServerProjectileEntityUpdater::update`:
  - строка-аннотация прям в коде:
    `"C:\monkey\VeniceXpack5.MpDeploy.Server\TnT\Code\Engine/Game/Server/Gameplay/Projectiles/ServerProjectileEntityUpdater.cpp"`,
  - стадии внутри: `CollectRaycasts` → `PerformRaycasts` → `ReportRaycastResults`
    (видны в вызовах `FUN_004deee0`), обрабатывает снаряды (массив по
    `this+0x54/0x58`, приращение 0xC = 3 float'а позиции).
- Соглашение вызова `__thiscall` — то же замечание, что для tick: детурить
  аккуратно, со стеком.

### 2.3 `0x0117EA40` — `FUN_0117ea40` (onNewRound / TEAMSWITCHPROBE первый хук)
- Сигнатура:
  ```c
  int* FUN_0117ea40(int* this_)
  ```
- Размер 396 байт. Это `ServerPlayerSquadManager::onNewRound`:
  - строка-аннотация: `"onNewRound "` (см. `FUN_00490900(local_130,"onNewRound ",...)`);
  - крутит список отрядов (материи по `DAT_02b3ee2c+0x30` → `+0x80/+0x84`),
    вызывает `FUN_00e207a0(...)` (спавн/ресайз отряда), флаг срабатывания
    `DAT_0285babd = 1`.
- Возвращает `int*` (this_ или результат последнего вызова) — на x86 EAX.

## 3. Как пользоваться готовым Ghidra-проектом (экономия времени)

- Проект **уже импортирован и проанализирован** — повторный запуск с
  `-process` (без `-import`) быстрый: просто декомпиляция по адресам.
- Если нужна декомпиляция другого адреса — тот же `-postScript` с новым адресом:
  ```
  analyzeHeadless C:\Games\re\ghidra-jsrv server -process bf3_server_Final.exe \
    -scriptPath C:\Games\re\ghidra -postScript DecompileAddr.java <VA адрес>
  ```
- Адреса задаются в **VA** (абсолютные, с `0x00400000` — ImageBase), не в RVA.
- Для сети/класса: `0x00e38190` (вызывается из тика для каждого элемента) —
  вероятный кандидат «обновления плеерной записи».

## 4. Что это меняет для плана

1. `ServerPlayer::update` детурить НЕЛЬЗЯ в текущем виде (соглашение вызова
   не совпадает с fastcall-детуром) — либо делать thiscall-детур, либо вообще
   не вешать; выбран второй путь (таймер).
2. `0x00E78080` (Projectile updater) и `0x0117EA40` (onNewRound) — тоже
   `__thiscall`, для них те же ограничения при будущих детурах.
3. Строки-аннотации внутри функций (пути `*\.cpp`) — это готовые «маяки» для
   идентификации функций движка: поиск по ним надёжнее любых AOB.

Эти три функции — **первые реально декомпилированные** из нашего проекта кода
движка BF3; текст полностью в `re\server-decompile.txt` (13 КБ, сохранён вне
проекта по правилу «декомпиляцию держать вне репо»).
