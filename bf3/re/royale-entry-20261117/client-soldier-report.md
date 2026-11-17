# ClientSoldier, оружие HUD и активная кость ragdoll

17.11.2026. Добавлены `VextClientSoldier.cpp/.h`. Родителю передана интеграция нового .cpp в build.cmd и RegisterVextClientSoldier после RegisterVextBlueprintEntities в клиентском LuaRuntime. Изменены VextClient.cpp (player.soldier/corpse через tracked native wrapper) и VextClientEntities.cpp (свойства, worldTransform alias, каст SoldierEntity). Свежие бэкапы этих двух файлов: `C:/Games/backup/2026-11-17/royale-spawn/*-soldier-adapter-pre.cpp`, сохраняют предыдущие правки родителя.

## Проверенное соответствие shipped VU

| Поле/метод | VU getter / путь |
|---|---|
| health | 107E7F40 → float[soldier+20] |
| teamId | ClientControllable 10E40C00 / setter10E40FC0 → soldier+78 |
| isInteractiveManDown | 107E8070 → [soldier+354], vtable+20 |
| isDead / isAlive | 107E7FB0 /107E7F70 → тот же component, vtable+14 /+C |
| isManDown | 107E80B0 → soldier+F0, vtable+C |
| player | 107E81D4 → owner+B8 либо weak+238 (pointer-8), проверка PlayerManager slots/id |
| ragdollComponent | 107E8330 → soldier+368 |
| GetActiveWorldTransform | 10C1EBF0: component.data+14 → tagged skeleton asset+60; bone array asset+C, count=array-4; active QuatTransform array component+50, stride20h |
| weaponsComponent | 107E8570 → soldier+35C |
| currentWeapon / currentWeaponSlot | 107F7100 /107F7170 → component+D4 handler+4; vector BC/C0 |
| primaryAmmo / secondaryAmmo | 10DA1200 /10DA1240 → weapon+160 firing+F4/F8 |
| fireLogic | 10DA0F80 → firing+110 index, firing+11C array |
| name | 10DA0FD0 → weapon.data+14 → tagged unlock asset+13C → asset+8 string |

Все смещения выше шестнадцатеричные. `client-soldier-proof.py` / `.txt` сохраняют регистрацию и соответствующий машинный код; `vu_binding_map.py` восстанавливает пары имя/getter из фактических инструкций регистрации. Это ClientSoldierEntity (разрешение типа107E8C90), не серверный тип с другими смещениями.

Native чтения/записи выполняются через generation-checked Blueprint Handle и WithEntityContext. Компоненты и оружие перепроверяются против текущего soldier и vector при каждом доступе. Старый ragdoll или удалённое оружие возвращают nil, не читают сохранённый освобождённый указатель. Отсутствующий state component даёт false как в VU. Пустое weaponFiring даёт0 как в VU. teamId читается без ограничения16: локальная расширенная TeamCapacity использует команды до127; запись ограничена установленной capacity (127 либо16). health и клиентские ammo readonly. Native teamId меняется на самом ControllableEntity, не на игроке и не в теневом Lua-поле.

## Проверки

- Compile-only MSVC x86 /W4 /WX прошёл для нового файла.
- `client-soldier-test.cpp` включает фактический production .cpp и фактические VextGeometry raw chunks. Native память заменена изолированными буферами внутри отдельного host; guard/lifetime — явные тестовые doubles. 31 PASS: обновление health без кэша, readonly, команды126/127, canaries соседних полей, stock/extended bounds, stale generation, state-component absence, настоящий bone stride/границы/удаление asset/component, QuatTransform→LinearTransform, актуальные ammo/fireLogic/name, holster/смена слота/удаление оружия и отсутствие firing.
- `client-soldier-test.log`, `test-client-soldier.cmd`; Lua syntax затронутых чанков 4/4 PASS. Основную DLL собирает родитель.

В тест-host не вызывались настоящие виртуальные state методы. Health/team/оружие/кость в живом клиенте требуют игрового прогона; это не выдаётся за уже проверенный игровой результат. Другие не вызываемые Royale свойства Soldier/Weapon здесь не реализуются.
