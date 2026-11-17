# ENGINE-ENTITIES — сущности сервера BF3, нужные модам (справочник находок агентов)

> Источник: фоновые агенты 14.10.2026 (статический разбор `bf3_server_Final.exe`). Сырой вывод вне проекта:
> `C:\Games\re\capture\` (флаги, точки спавна, техника) и `C:\Games\re\death\` (смерть ИИ-игрока).
> Адреса — VA, база 0x400000, RVA = VA − 0x400000. Пометки: **ФАКТ** — видно в коде и/или проверено в игре;
> **ГИПОТЕЗА** — не проверено. Слова «окончательно» не используем.

## 1. Как перечислять сущности

| Что | Значение | Статус |
|---|---|---|
| Мир сущностей | `[0x02B3EE30]` (ставит конструктор ServerGameWorld 0x00D4FF20, обнуляет 0x00D5CB30) | ФАКТ |
| Итератор движка | begin 0x005CF340 (thiscall: мир; стек: TypeInfo*, буфер 0x1C, bool=1; `ret 0xC`), next 0x005CF2F0 | ФАКТ |
| Таблица вручную | `idx = u16[TI+0x14] − u16[мир+0x834]`; сегмент `[[мир+0x820] + idx*8]`; в сегменте `[seg+0x1C]` штук по указателю `[seg+0]`; следующий `[seg+0x18]` | ФАКТ (прогон 125: 5 флагов прочитаны снаружи) |

Прогон 125: штатный итератор, вызванный из потока Lua при `Level:Loaded`, вернул 0 флагов; опрос из тика моста — 5.
С ред. 2 сборки 125 мост читает таблицу напрямую (без вызовов движка).

## 2. Типы

| Класс | TypeInfo | Главная vtable | Размер | Конструктор |
|---|---|---|---|---|
| ServerCapturePointEntity | 0x02C23918 | 0x02696660 | 0xE8 | 0x01336EF0 |
| ServerCharacterSpawnEntity | 0x02C16B84 | 0x025BBBC0 (вторичная +0x80: 0x025BBB70) | 0xD50 | 0x00DDE1D0 |
| ServerVehicleEntity | 0x02C17090 | 0x025BF8E8 | 0x340 | 0x00DF6FAB |
| ServerSyncedBoolEntity | 0x02C160B8 | 0x025B50D8 | 0x90 | 0x00DB09AA |
| ServerInputRestrictionEntity | 0x02C16904 | 0x025BB8A8 | 0x3C | 0x00DDD4C3 |
| ServerTicketCounterEntity | 0x02C230C0 | 0x0268BC30 (прогон 125) | — | — |

## 3. Общие поля Entity (ФАКТ)
- `+0x08` флаги; `+0x10` шина (EntityBus*); `+0x14` data; `+0x1C` трансформ.
- Трансформ: `T = [+0x1C]`; если бит 18 в `[+0x08]` — `T = [+0x1C] + u8[[+0x1C]+6]*0x50 + 0x10`.
  LinearTransform: left +0x00, up +0x10, forward +0x20, trans +0x30.

## 4. ServerCapturePointEntity (флаг Conquest)

| Поле | Смещение | Статус |
|---|---|---|
| team (0/1/2) | +0xB0 int | ФАКТ (прогон 125: HQ т1/т2, флаги т0; захват CP3 командой 2 увиден) |
| flagLocation 0..100 | +0xC8 float | ФАКТ |
| flagVelocity | +0xC4 float | ФАКТ |
| flagHome | +0xCC float | ФАКТ |
| isControlled | +0xD4 byte | ФАКТ |
| isCaptureEnabled | +0xD5 byte | ФАКТ |
| isAttacked | +0xE6 byte | ФАКТ |
| attackingTeam | +0xB8 int | смещение ФАКТ, имя ГИПОТЕЗА |
| previousOwner | +0xB4 | ГИПОТЕЗА |
| cooldownTimer | +0xD8 float | ФАКТ |
| isInPreRound | +0xE5 byte | ГИПОТЕЗА |
| игроки внутри | +0x9C/+0xA0/+0xA4 (вектор weak-ссылок) | ГИПОТЕЗА |
| имя (`.name` у VU) | в движке нет | НЕ НАЙДЕНО — мост даёт синтетическое, у баз `…_HQ` |

- HQ: `[[cp+0x14]+0xA8] == 1` (CapturableType = CTUnableToChangeTeam). ФАКТ (прогон 125: 2 HQ по 100%).
- CapturePointEntityData: InitialOwnerTeam +0x70, TimeToGetControl +0x88, TimeToLoseControl +0x8C,
  OnlyTakeableByTeam +0x94, CapturableType +0xA8, HoistFlag +0xB9, StartAtBottom +0xBA (vtable данных 0x0250BF2C).
- События движка точки: Enable, DisableSpawners, EnableCapture, DisableCapture; связь `Spawners` (хеш 0xECEA613A).
- `instanceGuid` у data не найден.

## 5. ServerCharacterSpawnEntity (точка спавна)
- data = CharacterSpawnReferenceObjectData (`[+0x14]`, vtable 0x025043A0): Team `+0x128` (ФАКТ),
  PlayerType +0x180, VehicleEntryIndex +0x184, LocationNameSid +0x120, LocationTextSid +0x124.
- Прогон 125: в коллекции типа всего 2 точки (базы). У флагов спавнеры в EBX `Enabled False` — вероятно, создаются
  при захвате (ГИПОТЕЗА). `bus.entities` флага = точки спавна с той же шиной — ГИПОТЕЗА агента.

## 6. Игрок и смерть ИИ (разбор `re\death`)
- `игрок+0x318` (isAI): ввод игрока применяется; клиент рисует ник над головой; при смерти игрок удаляется
  (4 места `cmp byte [player+0x318],0 / je` → 0x00E378E0 + deletePlayer 0x00E67460); PointSystem 0x0131B677 не шлёт
  «killed X» жертве-ИИ; 0x0131AFB0 — убийце-ИИ нет очков. ФАКТ.
- Патчи (сборка 124, сверены дизассемблером): E114C8, E0EEF0, DD46B6, D5146E (`je→jmp`), 131B67E (NOP×6), 131AFC3 (NOP×2).
- vf13C солдата 0x00E07C80 (слот 0x025C3A84) — условие выбора ввода (prePhysicsUpdate 0x00E34280).
- `игрок+0x480` = блюпринт `Characters/Soldiers/MpSoldier` (vtable 0x02500E38), до дислокации человека пуст.
- Слоты оружия `игрок+0x50C..0x510`: 10 × 0x24, пустой слот = 0; resize вектора 0x00E293A0.

## 7. Как проверить чтением памяти (без вызовов движка)
`C:\Games\re\peek\peekents.py <pid>` — счёт сущностей по типам и их vtable;
`peek480.py`, `peekwpn.py` — объект `+0x480` и слоты оружия игрока.
